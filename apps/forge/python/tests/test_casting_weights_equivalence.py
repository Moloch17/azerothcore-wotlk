"""casting_weights without a baseline equals the function the deployed build ran.

The cleanup (91811bba6, "baseline remnants: random is the only baseline the sim has", and the scripted baseline's
removal before it) took the baseline-gap term out of animus.evaluation.casting_weights: the old need was
standardised(baseline score - own score), and with no baseline configured every baseline score was 0, so it was
standardised(-score). That must be exactly what is computed now. The reference below is the deployed function, copied
verbatim from the tag pre-cleanup-2026-10-07 (git show pre-cleanup-2026-10-07:apps/forge/python/animus/evaluation.py);
no live config sets eval.baseline, so the live learner always called it with baseline=None.
"""

import math
import zlib
from pathlib import Path

import numpy as np
import pytest

from animus.config import TrainConfig
from animus.evaluation import casting_weights

CONFIGS = Path(__file__).resolve().parents[1] / "configs"


# ---------------------------------------------------------------- the reference: the deployed function, verbatim

def reference_casting_weights(summary: dict, baseline: dict | None, strength: float, max_ratio: float,
                              metric: str = "", roles: dict[str, str] | None = None,
                              role_metrics: dict | None = None) -> dict[str, float]:
    rows = summary.get("castings", {})
    names = [name for name, row in rows.items() if row.get("score") is not None]
    if not names or strength <= 0.0 or max_ratio <= 1.0:
        return {name: 1.0 for name in rows}

    def standardised(values: np.ndarray) -> np.ndarray:
        spread = float(values.std())
        return (values - values.mean()) / spread if spread > 1e-9 else np.zeros_like(values)

    base = (baseline or {}).get("castings", {})
    gaps = np.array([float(base.get(name, {}).get("score") or 0.0) - float(rows[name]["score"]) for name in names])
    need = standardised(gaps)
    if metric and all(rows[name].get(metric) is not None for name in names):
        # The larger of the two needs, not their sum: a wide lead over a weak baseline must not cancel a gate
        # the layout is failing.
        need = np.maximum(need, standardised(-np.array([float(rows[name][metric]) for name in names])))
    need = np.maximum(need, reference_role_needs(rows, names, roles or {}, role_metrics or {}))
    if not need.any():
        return {name: 1.0 for name in names}

    weights = np.exp(strength * np.clip(need, -3.0, 3.0))
    # Cap the spread, then centre on 1 so the total number of episodes is unchanged.
    limit = math.sqrt(max_ratio)
    weights = np.clip(weights / float(np.exp(np.log(weights).mean())), 1.0 / limit, limit)
    weights /= float(weights.mean())
    return {name: float(weight) for name, weight in zip(names, weights)}


def reference_role_needs(rows: dict, names: list[str], roles: dict[str, str], role_metrics: dict) -> np.ndarray:
    need = np.full(len(names), -np.inf)
    for role, fields in role_metrics.items():
        members = [index for index, name in enumerate(names) if roles.get(name) == role]
        if len(members) < 2:
            continue
        for field_name in fields:
            sign = -1.0 if field_name.startswith("-") else 1.0
            key = field_name.lstrip("-")
            if not all(rows[names[index]].get(key) is not None for index in members):
                continue
            values = sign * np.array([float(rows[names[index]][key]) for index in members])
            spread = float(values.std())
            if spread <= 1e-9:
                continue
            shortfall = -(values - values.mean()) / spread
            for slot, index in enumerate(members):
                need[index] = max(need[index], shortfall[slot])
    return need


# ------------------------------------------------------------------------------------------------ the inputs

CLASSES = ["warrior", "paladin", "hunter", "rogue", "priest", "deathknight", "shaman", "mage", "warlock", "druid"]
BUILDS = ["arms", "fury", "protection", "holy", "retribution", "frost", "shadow", "restoration", "balance", "feral"]


def random_summary(rng: np.random.Generator, rows: int, metrics: tuple[str, ...], none_share: float = 0.0) -> dict:
    """Castings shaped like EvalResult.summary's: "<class>_<build>" -> {"score", <metric>...}."""
    castings = {}
    for index in range(rows):
        name = f"{CLASSES[index % len(CLASSES)]}_{BUILDS[(index * 3) % len(BUILDS)]}{index // len(CLASSES) or ''}"
        row = {"score": float(rng.normal(5.0, 3.0)), "stderr": float(rng.uniform(0.05, 0.5)),
               "episodes": int(rng.integers(20, 200))}
        for metric in metrics:
            row[metric] = float(rng.uniform(0.0, 1.0))
        if none_share and rng.random() < none_share:
            row["score"] = None
        castings[name] = row
    return {"castings": castings}


def same(summary: dict, strength: float, max_ratio: float, **kwargs) -> None:
    expected = reference_casting_weights(summary, kwargs.pop("baseline", None), strength, max_ratio, **kwargs)
    actual = casting_weights(summary, strength, max_ratio, **kwargs)
    assert actual.keys() == expected.keys()
    for name in expected:
        assert actual[name] == pytest.approx(expected[name], rel=1e-12, abs=1e-15), name


def live_layout_sampling() -> list[tuple[str, object]]:
    return [(path.stem, TrainConfig.load(path).layout_sampling) for path in sorted(CONFIGS.glob("*.yaml"))
            if path.stem != "fast"]


# ------------------------------------------------------------------------------------------------- the tests

@pytest.mark.parametrize("baseline", [None, {}, {"castings": {}}], ids=["none", "empty", "empty castings"])
def test_randomised_summaries_match_the_deployed_function(baseline):
    rng = np.random.default_rng(20261007)
    for trial in range(300):
        rows = int(rng.integers(1, 41))
        metric = ["", "won", "cleared"][trial % 3]
        summary = random_summary(rng, rows, ("won", "cleared"), none_share=0.15 if trial % 5 == 0 else 0.0)
        strength = float(rng.choice([0.0, 0.5, 1.0, 2.0]))
        max_ratio = float(rng.choice([1.0, 2.0, 3.0, 4.0, 9.0]))
        same(summary, strength, max_ratio, baseline=baseline, metric=metric)


@pytest.mark.parametrize("stage, sampling", live_layout_sampling(), ids=lambda value: getattr(value, "metric", value))
def test_each_live_stages_layout_sampling_settings_match(stage, sampling):
    """The strength, ratio and metric each live config sets (enabled or not: the function takes the same numbers)."""
    rng = np.random.default_rng(zlib.crc32(stage.encode()))
    for rows in (1, 2, 5, 12, 30):
        summary = random_summary(rng, rows, (sampling.metric,) if sampling.metric else ())
        same(summary, sampling.strength, sampling.max_ratio, metric=sampling.metric,
             role_metrics=sampling.role_metrics)


def test_roles_and_role_metrics_match():
    rng = np.random.default_rng(7)
    role_metrics = {"healer": ["-teammates_died", "group_kept_share", "healing_coverage"], "tank": ["tank_hold_share"],
                    "damage": ["focus_share"]}
    for _ in range(100):
        summary = random_summary(rng, 12, ("won", "teammates_died", "group_kept_share", "healing_coverage",
                                           "tank_hold_share", "focus_share"))
        names = list(summary["castings"])
        roles = {name: ["healer", "tank", "damage", ""][index % 4] for index, name in enumerate(names)}
        same(summary, 1.0, 4.0, metric="won", roles=roles, role_metrics=role_metrics)
        same(summary, 1.0, 4.0, roles=roles, role_metrics=role_metrics)


def test_edge_cases_match():
    even = {"castings": {"a_x": {"score": 3.0}, "b_y": {"score": 3.0}, "c_z": {"score": 3.0}}}
    same(even, 1.0, 3.0)                                          # every score equal: no spread, the even draw
    same({"castings": {"a_x": {"score": 3.0}}}, 1.0, 3.0)          # one row
    same({"castings": {}}, 1.0, 3.0)                              # no rows
    same({}, 1.0, 3.0)                                            # no castings at all
    same({"castings": {"a_x": {"score": None}, "b_y": {"score": None}}}, 1.0, 3.0)   # no scores
    same({"castings": {"a_x": {"score": None}, "b_y": {"score": 2.0}, "c_z": {"score": 4.0}}}, 1.0, 3.0)
    same({"castings": {"a_x": {"score": 0.0}, "b_y": {"score": 0.0}, "c_z": {"score": 1e-12}}}, 1.0, 3.0)  # a zero
    same({"castings": {"a_x": {"score": 2.0, "won": 0.5}, "b_y": {"score": 4.0}}}, 1.0, 3.0, metric="won")  # a row
    #                                                                           lacking the metric: the score alone
    same({"castings": {"a_x": {"score": 2.0, "won": 0.0}, "b_y": {"score": 4.0, "won": 1.0}}}, 1.0, 3.0,
         metric="won")
    same(random_summary(np.random.default_rng(1), 8, ()), 0.0, 3.0)        # strength 0: the even draw
    same(random_summary(np.random.default_rng(2), 8, ()), 1.0, 1.0)        # ratio 1: the even draw
    same(random_summary(np.random.default_rng(3), 8, ()), 50.0, 1.01)      # a huge strength under a tight cap


def test_a_configured_baseline_is_the_one_thing_that_changed():
    """Documents the difference rather than hiding it: with a baseline the old need was the gap to it. No live config
    sets eval.baseline (the sim's only baseline is "random"), so the live learner never reached this case."""
    summary = {"castings": {"a_x": {"score": 2.0}, "b_y": {"score": 4.0}, "c_z": {"score": 6.0}}}
    baseline = {"castings": {"a_x": {"score": 1.0}, "b_y": {"score": 9.0}, "c_z": {"score": 1.0}}}
    old = reference_casting_weights(summary, baseline, 1.0, 4.0)
    new = casting_weights(summary, 1.0, 4.0)
    assert old != pytest.approx(new)
    for path in sorted(CONFIGS.glob("*.yaml")):
        assert TrainConfig.load(path).eval.baseline == "", f"{path.name} sets eval.baseline"
