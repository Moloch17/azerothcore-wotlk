"""Every metric a learner yaml names is one its stage's sim (or the learner) produces.

A gate on a column no episode reports reads as "never met": the ladder never steps and nothing says why (found while
writing this: move4_follow inherited move2_seek's costs.gate_metric `found`). The sim declares its columns in C++, so
apps/forge/tools/sim_metrics.py reads them from there; the deploy gate re-checks the extraction against the stage.json
files a fresh build writes (sim_metrics.py --check). See docs/forge/deploy-gate.md.
"""

import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "apps" / "forge" / "tools"))

import sim_metrics  # noqa: E402

from animus import episode_means  # noqa: E402
from animus.config import TrainConfig  # noqa: E402

CONFIGS = ROOT / "apps" / "forge" / "python" / "configs"
FIXTURES = Path(__file__).resolve().parent / "fixtures"
#: Overlays merged over a stage's config (--overlay), not stages of their own.
OVERLAYS = {"fast"}

EXTRACTOR = sim_metrics.Extractor()
STAGES = EXTRACTOR.stages()
DERIVED = sim_metrics.evaluation_names() | sim_metrics.progress_names()
ARMS = sim_metrics.eval_arms()
LIVE_YAMLS = sorted(path for path in CONFIGS.glob("*.yaml") if path.stem not in OVERLAYS)


def metric_fields(config: TrainConfig) -> dict[str, list[str]]:
    """Where a config names metrics: field -> the names."""
    fields = {
        "status.headline": list(config.status.headline),
        "status.targets": list(config.status.targets),
        "eval.report": list(config.eval.report),
        "convergence.measure": [config.convergence.measure],
        "fade.gate_metric": [config.fade.gate_metric],
        "costs.gate_metric": [config.costs.gate_metric],
        "layout_sampling.metric": [config.layout_sampling.metric],
    }
    for role, names in config.layout_sampling.role_metrics.items():
        fields[f"layout_sampling.role_metrics.{role}"] = [name.lstrip("-") for name in names]
    return {field: [name for name in names if name] for field, names in fields.items()}


def is_produced(name: str, columns: sim_metrics.Names) -> bool:
    if name in columns or name in DERIVED:
        return True
    # <metric>_<arm>: a headline measure read off an evaluation arm (eval.arms, clear_rate_with_human).
    return any(name.endswith(f"_{arm}") and (name[:-len(arm) - 1] in columns or name[:-len(arm) - 1] in DERIVED)
               for arm in ARMS)


def test_every_stage_has_a_config_and_every_config_a_stage():
    assert sorted(path.stem for path in LIVE_YAMLS) == sorted(STAGES), (
        "configs/*.yaml and the stages in Stages/Stages.cpp differ: a stage without a config is never trained, a "
        "config without a stage never starts")


def test_the_extractor_resolves_every_name_it_can_see():
    """A name built at run time that the reader cannot resolve becomes a wildcard that would accept a typo; a new
    construct of that kind is looked at by a person, who extends sim_metrics.py rather than allowing it here."""
    for stage in STAGES:
        EXTRACTOR.columns(stage)
    assert not EXTRACTOR.source.unresolved, "\n".join(str(item) for item in EXTRACTOR.source.unresolved)


@pytest.mark.parametrize("path", LIVE_YAMLS, ids=lambda path: path.stem)
def test_every_metric_a_live_config_names_exists(path):
    columns = EXTRACTOR.columns(path.stem)
    config = TrainConfig.load(path)
    missing = [f"{field}: {name}" for field, names in metric_fields(config).items() for name in names
               if not is_produced(name, columns)]
    assert not missing, (f"{path.name} names metrics that stage {path.stem} does not report (an episode info column, "
                         f"a reward_<term> column or a measure the learner derives):\n  " + "\n  ".join(missing))


@pytest.mark.parametrize("path", LIVE_YAMLS, ids=lambda path: path.stem)
def test_heldout_and_phase_arenas_are_the_stages(path):
    config = TrainConfig.load(path)
    arenas = {arena["name"]: arena for arena in STAGES[path.stem]}
    assert set(config.eval.heldout) <= set(arenas), f"eval.heldout names arenas {path.stem} lacks: {sorted(arenas)}"
    for name in config.eval.heldout:
        assert arenas[name].get("EvalOnly"), f"eval.heldout.{name} is an arena the stage also trains on"
    for names in config.eval.phases.values():
        assert set(names) <= set(arenas)
    assert set(config.eval.arms) <= set(ARMS)


def test_per_event_means_name_columns_some_stage_reports():
    """episode_means.PER_EVENT averages a column over the episodes that had the event; both columns must exist."""
    every = sim_metrics.Names()
    for stage in STAGES:
        every.update(EXTRACTOR.columns(stage))
    missing = sorted({name for pair in episode_means.PER_EVENT.items() for name in pair if name not in every})
    assert not missing, f"episode_means.PER_EVENT names columns no stage reports: {missing}"


def test_the_extraction_covers_the_live_m2_stage_json():
    """The extractor against the one real stage.json it has been checked against: the 235 episode_info names the
    deployed build (64b7c7dc5) wrote for move2_seek. Everything the cleanup did not delete must be extracted."""
    recorded = json.loads((FIXTURES / "move2_seek_episode_info.json").read_text())
    columns = EXTRACTOR.columns(recorded["stage"])
    terms = EXTRACTOR.reward_terms()
    # Deleted with the scripted opponents (91811bba6): the seat of the scripted opponent.
    deleted = {"opponent_seat"}
    gone = [name for name in recorded["episode_info"] if name not in columns and name not in deleted
            and not (name.startswith("reward_") and name[len("reward_"):] not in terms)]
    assert not gone, f"the extractor misses columns the real build reports: {gone}"
    assert len(recorded["episode_info"]) == 235


def test_a_removed_name_is_not_extracted():
    columns = EXTRACTOR.columns("move2_seek")
    assert "opponent_seat" not in columns
    assert "no_such_column" not in columns


def test_a_move4_costs_ladder_is_off_and_names_no_gate():
    """The finding this test file started from: M4 has no cost ladder, and no `found` for one to gate on."""
    config = TrainConfig.load(CONFIGS / "move4_follow.yaml")
    assert not config.costs.enabled and config.costs.gate_metric == ""


# ------------------------------------------------------------------------------ the reader, on synthetic C++

def reader(tmp_path: Path, source: str) -> tuple[sim_metrics.Extractor, Path]:
    path = tmp_path / "Encounters" / "Thing.cpp"
    path.parent.mkdir(parents=True)
    path.write_text(source)
    extractor = sim_metrics.Extractor.__new__(sim_metrics.Extractor)
    extractor.source = sim_metrics.Source(tmp_path)
    return extractor, path


def names_of(tmp_path: Path, body: str) -> sim_metrics.Names:
    extractor, path = reader(tmp_path, "void Thing::AddEpisodeInfo(EpisodeInfoTable& table)\n{\n" + body + "\n}\n")
    text = extractor.source.files[path]
    inner, offset = sim_metrics.function_body(text, r"Thing::AddEpisodeInfo\(")
    return extractor.adds_in(text, path, offset, offset + len(inner))


def test_the_reader_takes_literals_and_ignores_comments(tmp_path):
    names = names_of(tmp_path, '''
    table.Add("found", f);
    // table.Add("commented_out", f);
    /* table.Add("also_commented", f); */
    table.Add("seat", [](Env const&, uint32 seat) { return float(seat); });''')
    assert names.exact == {"found", "seat"}


def test_the_reader_expands_a_loop_over_literals_into_exact_names(tmp_path):
    names = names_of(tmp_path, '''
    for (auto const& [name, drill] : { std::pair{ "won_hold", A }, std::pair{ "won_keep", B } })
        table.Add(name, [this, drill](Env const& env, uint32) { return 1.0f; });''')
    assert names.exact == {"won_hold", "won_keep"} and not names.families


def test_the_reader_follows_a_name_array_and_a_format(tmp_path):
    extractor, path = reader(tmp_path, '''
constexpr char const* RUNG_NAMES[3] = { "hallway", "room", "deep" };
void Thing::AddEpisodeInfo(EpisodeInfoTable& table)
{
    for (uint32 rung = 0; rung < 3; ++rung)
    {
        std::string const name = RUNG_NAMES[rung];
        table.Add("found_" + name, f);
        table.Add(Acore::StringFormat("mark{}_yard", rung), f);
    }
}
''')
    text = extractor.source.files[path]
    body, offset = sim_metrics.function_body(text, r"Thing::AddEpisodeInfo\(")
    names = extractor.adds_in(text, path, offset, offset + len(body))
    assert names.exact == {"found_hallway", "found_room", "found_deep"}
    assert names.families == ["mark[0-9]+_yard"]
    assert names.match("mark12_yard") == "family" and names.match("found_deepest") == ""


def test_an_expression_it_cannot_resolve_is_listed_not_swallowed(tmp_path):
    extractor, path = reader(tmp_path, '''
void Thing::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("x_" + Mystery(), f);
}
''')
    text = extractor.source.files[path]
    body, offset = sim_metrics.function_body(text, r"Thing::AddEpisodeInfo\(")
    names = extractor.adds_in(text, path, offset, offset + len(body))
    assert names.families == ["x_[a-z0-9_]+"]
    assert len(extractor.source.unresolved) == 1 and "Mystery" in extractor.source.unresolved[0].expression


def test_strip_comments_keeps_offsets_and_strings():
    text = 'a "//not a comment" // gone\nb /* gone\ntoo */ c'
    stripped = sim_metrics.strip_comments(text)
    assert len(stripped) == len(text) and stripped.count("\n") == text.count("\n")
    assert '"//not a comment"' in stripped and "gone" not in stripped
