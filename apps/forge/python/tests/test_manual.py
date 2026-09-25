"""The manual's tables against the files they describe.

Every stage budget in the manual was between 1x and 7x the configured value before this existed -- stage17_party
was documented at 600M against a configured 120M, stage27_crossroads at 1B against 150M. Numbers copied by hand
into prose drift silently and nobody notices until someone plans a run from them, so the table is checked instead
of trusted.
"""

import re
from pathlib import Path

import pytest

from animus.config import TrainConfig

CONFIGS = Path(__file__).resolve().parents[1] / "configs"
REPO = Path(__file__).resolve().parents[4]
MANUAL = REPO / "docs" / "forge" / "04-curriculum.md"

# `| `stage8_duel` | 100M | 10M | 2048 |` -- the table pairs two stages per row, so each line yields two.
ROW = re.compile(
    r"\|\s*`(?P<name>\w+)`\s*\|\s*(?P<budget>[\d.]+)M\s*\|\s*(?P<every>[\d.]+)M\s*\|"
    r"\s*(?P<episodes>\d+)\s*(?=\|)")


def documented() -> dict[str, dict]:
    out = {}
    for line in MANUAL.read_text().splitlines():
        for m in ROW.finditer(line):
            out[m.group("name")] = {
                "total_env_steps": int(float(m.group("budget")) * 1e6),
                "every_env_steps": int(float(m.group("every")) * 1e6),
                "episodes": int(m.group("episodes")),
            }
    return out


def configured(name: str) -> dict:
    config = TrainConfig.load(CONFIGS / f"{name}.yaml")
    return {
        "total_env_steps": config.total_env_steps,
        "every_env_steps": config.eval.every_env_steps,
        "episodes": config.eval.episodes,
    }


def test_the_table_covers_every_stage_config():
    """A stage added without a row is the way the table goes stale next."""
    on_disk = {p.stem for p in CONFIGS.glob("*.yaml")} - {"fast"}
    assert on_disk - set(documented()) == set(), "these configs have no row in the manual's budget table"


@pytest.mark.parametrize("name", sorted(documented()))
def test_the_manual_matches_the_config(name):
    assert documented()[name] == configured(name), f"04-curriculum.md disagrees with configs/{name}.yaml"


STAGES_CPP = REPO / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum" / "Stages" / "Stages.cpp"
# AnimusForge.Envs the manual's arithmetic assumes (this machine's forge bench result), and the host default episode
# length (AnimusForge.EpisodeSeconds) for an arena that sets none.
ENVS = 128
DEFAULT_EPISODE_SECONDS = 60
# The party line's 2048 x 450 s / 128 = 7,200 sim-seconds is the most any stage spends on one evaluation today.
MAX_EVAL_SIM_SECONDS = 7_200
# The raid stages run at their own env count (AnimusForge.Stage.<name>.Envs in the conf template).
STAGE_ENVS = {"stage30_raid10": 32, "stage31_raid25": 16, "stage32_raid40": 8}


def longest_episode_seconds() -> dict[str, int]:
    """Per stage, the longest arena episode in Stages.cpp: what one evaluation episode can cost."""
    out, stage = {}, None
    for line in STAGES_CPP.read_text().splitlines():
        if m := re.search(r'\.Name = "(stage\d+_\w+)"', line):
            stage = m.group(1)
            out[stage] = DEFAULT_EPISODE_SECONDS
        elif stage and (m := re.search(r"\.EpisodeSeconds = (\d+)", line)):
            out[stage] = max(out[stage], int(m.group(1)))
    return out


@pytest.mark.parametrize("name", sorted(documented()))
def test_an_evaluation_stays_affordable(name):
    """episodes x episode seconds / envs (manual 4, "What an evaluation costs"): a stage that inherits the duel's
    2048 episodes with a 300 s episode and a few dozen envs would spend more sim time evaluating than training. The
    raid stages did exactly that until they set their own count."""
    seconds = longest_episode_seconds()[name]
    envs = STAGE_ENVS.get(name, ENVS)
    cost = configured(name)["episodes"] * seconds / envs
    assert cost <= MAX_EVAL_SIM_SECONDS, (f"{name}: {cost:,.0f} sim-seconds an evaluation at {envs} envs "
                                          f"({seconds} s episodes); set eval.episodes in its config")


def test_the_queue_total_is_what_the_manual_says():
    """The manual states the whole queue in one number, which is the one a person plans a run from."""
    rows = documented()
    # The raid stages are not in the default queue (StageDefinition::InDefaultQueue is false for them: forty seats
    # an env cannot run at the usual env count): they are trained by name.
    outside = {"stage28_raid_single", "stage29_raid_gauntlet", "stage30_raid10", "stage31_raid25", "stage32_raid40"}
    queue = sum(v["total_env_steps"] for k, v in rows.items() if k not in outside)
    assert queue == 1_202_000_000, f"the queue is {queue/1e6:.0f}M; the manual says 1,202M"
    assert sum(v["total_env_steps"] for v in rows.values()) == 1_402_000_000


# --------------------------------------------------------------------------- 8.2 tuning defaults

REFERENCE = REPO / "docs" / "forge" / "08-reference.md"
CONF_DIST = REPO / "src" / "server" / "apps" / "worldserver" / "worldserver.conf.dist"


def _template_defaults() -> dict[str, str]:
    text = CONF_DIST.read_text()
    return {m.group(1): m.group(2).strip().strip('"')
            for m in re.finditer(r"^AnimusForge\.Curriculum\.([\w.]+)\s*=\s*(.+?)\s*$", text, re.M)}


def _quick_reference() -> dict[str, str]:
    """Section 8.2's two-pairs-per-row table of the tuning values worth knowing."""
    text = REFERENCE.read_text()
    section = text.split("## 8.2 Curriculum tuning defaults", 1)[1].split("## 8.3", 1)[0]
    return {m.group(1): m.group(2).strip().strip('`"')
            for m in re.finditer(r"`([A-Z][\w.]*)`\s*\|\s*([^|]+?)\s*(?=\||$)", section, re.M)}


def test_the_quick_reference_only_lists_keys_that_exist():
    """test_conf_covers_tuning checks the template against the sim. This checks the manual against the template,
    which closes the loop: a key renamed in the code fails there, and a stale row here fails now."""
    unknown = sorted(set(_quick_reference()) - set(_template_defaults()))
    assert unknown == [], f"08-reference.md lists tuning keys the template does not define: {unknown}"


def test_the_quick_reference_defaults_are_the_templates():
    """Five of these were wrong when the table was last checked by hand -- Duel.Stall and Pulls.Stall at 0.05
    against a configured 0.08, both PreparationRefundMaxMs at 30000 against 15000, Travel.FastArrive at 3.0
    against 6.0. Numbers transcribed into prose drift; this is why the table is checked."""
    template = _template_defaults()
    wrong = {k: (v, template[k]) for k, v in _quick_reference().items() if k in template and v != template[k]}
    assert wrong == {}, f"08-reference.md disagrees with the template (manual, template): {wrong}"
