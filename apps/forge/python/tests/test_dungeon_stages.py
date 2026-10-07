"""The dungeon stages (dungeon-curriculum G2 group2_corridor, D1 dungeon1_pulls, D2 dungeon2_ragefire, D3
dungeon3_deadmines; the sim's InstanceEncounter): the configs load and read their own measures, every ladder steps on
its gate alone with every price at full price from the start, the teacher's hints are imitated where the sim writes them,
the evaluation plays fixed seeds and films them by the stage's rung, the seed chain runs from G1, Wailing Caverns is
held out (evaluated, never trained), the stand-in split (H) reaches forge status, and the video collector's dry run
names every worker and connects to none."""

import re
import subprocess
from pathlib import Path

import numpy as np
import pytest

from animus.config import TrainConfig
from animus.episode_means import PER_EVENT, means
from animus.progress import ARM_SPLITS, ProgressWriter
from animus.train import heldout_arenas

PYTHON = Path(__file__).resolve().parents[1]
CONFIGS = PYTHON / "configs"
REPO = Path(__file__).resolve().parents[4]
CURRICULUM = REPO / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum"
STAGES_CPP = CURRICULUM / "Stages" / "Stages.cpp"
COLLECT = REPO / "apps" / "forge" / "tools" / "collect-videos.sh"
STAGES = ("group2_corridor", "dungeon1_pulls", "dungeon2_ragefire", "dungeon3_deadmines")
# The columns only an evaluation arm reads, or a progress.json field derived from them: not sim columns.
DERIVED = {"standin_gap"}


def config(name: str) -> TrainConfig:
    return TrainConfig.load(CONFIGS / f"{name}.yaml")


def sim_columns() -> set[str]:
    """Every episode info column a dungeon stage's sim can report: the scenario's and the stand-in's, the encounters its
    arenas use (InstanceEncounter, PartyEncounter: PartyGroup) -- InstanceEncounter's by role and per boss built from
    lists -- and the reward terms'."""
    names = set(re.findall(r'_info\.Add\("(\w+)"', (CURRICULUM / "StageScenario.cpp").read_text()))
    names |= set(re.findall(r'_info\.Add\((?:std::string\()?"(\w+)"',
                            (CURRICULUM / "Encounters" / "StandInSeat.cpp").read_text()))
    for encounter in ("InstanceEncounter", "PartyEncounter"):
        names |= set(re.findall(r'table\.Add\("(\w+)"', (CURRICULUM / "Encounters" / f"{encounter}.cpp").read_text()))
    instance = (CURRICULUM / "Encounters" / "InstanceEncounter.cpp").read_text()
    roles = re.findall(r'\{ "(\w+)", DUNGEON_\w+ \}', instance)
    assert roles == ["tank", "healer", "damage"]
    names |= {f"{kind}_{role}" for kind in ("role", "deaths") for role in roles}
    names |= {f"boss_{name}" for name in re.findall(r'\{ \d+, \d+, "(\w+)" \}',
                                                    (CURRICULUM / "Encounters" / "InstanceBosses.cpp").read_text())}
    names |= {"reward_" + name for name in re.findall(r'return "(\w+)";',
                                                       (CURRICULUM / "Rewards" / "CombatReward.cpp").read_text())}
    return names


def stage_bodies() -> dict[str, str]:
    out = {}
    for body in re.findall(r"stages\.push_back\(\{(.*?)\n        \}\);", STAGES_CPP.read_text(), re.S):
        name = re.search(r'\.Name = "(\w+)",\s*\.Suffix', body)
        if name:
            out[name.group(1)] = body
    return out


def stage_order() -> list[str]:
    return re.findall(r'stages\.push_back\(\{\s*\.Name = "(\w+)"', STAGES_CPP.read_text())


@pytest.mark.parametrize("name", STAGES)
def test_the_config_loads_and_reads_its_own_measures(name):
    loaded = config(name)
    assert loaded.run_name == name
    known = sim_columns() | DERIVED
    missing = [column for column in (*loaded.status.headline, *loaded.eval.report) if column not in known]
    assert not missing, missing
    assert set(loaded.status.targets) <= set(loaded.status.headline)
    assert loaded.convergence.measure == loaded.layout_sampling.metric == loaded.fade.gate_metric
    assert loaded.convergence.measure in loaded.status.headline and loaded.convergence.measure in loaded.eval.report
    # H: the stand-in split, both halves and their gap, in the status; the with_human arm reads the stand-in's half.
    assert {"clear_allbot", "clear_standin", "standin_gap"} <= set(loaded.status.headline)
    assert loaded.status.targets["standin_gap"] == "<= 0.1"
    assert loaded.eval.arms.get("with_human", 0) > 0
    # Death knights never play a dungeon's level band (they start at 55): excluded by design, with the reason.
    assert "55" in loaded.status.excluded["death_knight"]


@pytest.mark.parametrize("name", STAGES)
def test_every_ladder_steps_on_its_gate_alone_with_full_prices_from_the_start(name):
    """The dungeon plan's rules: no cost ladder (every price at full price from the first step) and the shaping fade
    stepping on its gate at the first evaluation that meets it, never on a plateau or the classes' own rungs. The sim's
    own ladders -- the support ladder (WING_RUNGS) on the probes, the drill's on its clean share -- step on their gates
    alone too."""
    loaded = config(name)
    assert loaded.costs.enabled is False
    assert loaded.fade.enabled is True and loaded.fade.require_plateau is False
    assert loaded.fade.rungs == (1.0, 0.5, 0.25, 0.0)
    assert 0.0 < loaded.fade.gate_value <= 1.0 and loaded.fade.moving_classes >= 100
    # The teacher's hints imitated where the sim writes them (a taught arena), never self-imitation (no camera images).
    taught = ".Taught = true" in stage_bodies()[name]
    assert (loaded.mappo.hint_coef > 0.0) == taught, name
    assert loaded.mappo.sil_coef == 0.0


def test_the_stages_purposes():
    """G2: the corridor cleared (in order), the bar's measures for D3, a drill's clean pull for D1, the full clear for
    D2."""
    assert config("group2_corridor").convergence.measure == "cleared"
    assert config("dungeon1_pulls").convergence.measure == "cleared"
    assert config("dungeon2_ragefire").convergence.measure == "full_clear"
    d3 = config("dungeon3_deadmines")
    assert d3.convergence.measure == "bar_clear" and d3.status.headline[0] == "bar_clear"
    assert d3.status.targets["bar_clear"] == ">= 0.7" and d3.status.targets["wing_wipes"] == "<= 1"
    # Per boss and by role.
    for boss in ("rhahkzor", "sneed", "gilnid", "smite", "greenskin", "cookie", "vancleef"):
        assert f"boss_{boss}" in d3.eval.report
    for role in ("tank", "healer", "damage"):
        assert f"deaths_{role}" in d3.eval.report and PER_EVENT[f"deaths_{role}"] == f"role_{role}"


@pytest.mark.parametrize("name", STAGES)
def test_the_evaluation_plays_fixed_seeds_and_films_them_by_its_rung(name):
    """Fixed seeds, argmax; the eval videos read the rung off the stage's own *_rung column (the sim registers it first
    of its rung columns: Vision::EvalVideoRungColumn) and the outcome off `cleared` (EVAL_VIDEO_OUTCOMES)."""
    loaded = config(name)
    assert loaded.eval.seed == 1000 and loaded.eval.deterministic is True
    assert loaded.eval.episodes >= 64
    rungs = [c for c in loaded.eval.report if (c.endswith("_rung") or c in ("rung", "tier")) and
             not c.startswith("at_top")]
    assert rungs == (["pull_rung"] if name == "dungeon1_pulls" else ["wing_rung"])
    assert "cleared" in loaded.eval.report
    instance = (CURRICULUM / "Encounters" / "InstanceEncounter.cpp").read_text()
    order = [m.group(1) for m in re.finditer(r'table\.Add\("(\w+_rung)"', instance)]
    assert order[:3] == ["pull_rung", "wing_rung", "boss_rung"]


def test_the_seed_chain_runs_from_g1():
    """C3 -> G1 -> G2 -> D1 -> D2 -> D3, in Stages.cpp and in the configs' extends."""
    bodies = stage_bodies()
    chain = {"group2_corridor": "group1_roles", "dungeon1_pulls": "group2_corridor",
             "dungeon2_ragefire": "dungeon1_pulls", "dungeon3_deadmines": "dungeon2_ragefire"}
    order = stage_order()
    for name, parent in chain.items():
        assert re.search(rf'\.Extends = "{parent}"', bodies[name]), name
        assert order.index(parent) < order.index(name)
        text = (CONFIGS / f"{name}.yaml").read_text()
        assert re.search(rf"^extends: {parent}\.yaml$", text, re.M), name
        loaded = config(name)
        assert loaded.init_from == "auto" and loaded.seed_from == "latest"
        stage = {"seed_chain": [parent], "merges": []}
        assert [Path(p).parent.name for p in loaded.resolved_init_from(stage)] == [parent]
    # Every one on the player controller's world tick, and in the conf's queue documentation.
    conf = (REPO / "src" / "server" / "apps" / "worldserver" / "worldserver.conf.dist").read_text()
    for name in STAGES:
        assert re.search(rf"^AnimusForge\.Stage\.{name}\.TicksPerDecision = 5$", conf, re.M), name
        assert f'"{name}"' in conf


def test_wailing_caverns_is_held_out_from_d2_on():
    """D2 and D3 hold Wailing Caverns out (arena `heldout`, the WING ladder's row 2, EvalOnly) and evaluate it through
    eval.heldout; no stage trains on it -- every arena on row 2 is held out -- and the learner refuses to evaluate a
    trained arena as held out."""
    bodies = stage_bodies()
    for name, body in bodies.items():
        for arena in re.findall(r"\{ \.Name = \"\w+\"[^{}]*?\}", body, re.S):
            if ".Instance = InstanceLadder::Wing" in arena and ".InstanceRow = 2" in arena:
                assert ".EvalOnly = true" in arena, f"{name} trains on Wailing Caverns"
    for name in ("dungeon2_ragefire", "dungeon3_deadmines"):
        assert re.search(r'\.Name = "heldout"[^{}]*\.InstanceRow = 2, \.EvalOnly = true', bodies[name], re.S), name
        loaded = config(name)
        assert loaded.eval.heldout == {"heldout": 16}
        stage = {"arenas": [{"name": "dungeon"}, {"name": "heldout", "eval_only": True}]}
        assert heldout_arenas(loaded.eval.heldout, stage) == {"heldout": (2, 16)}
        with pytest.raises(ValueError, match="trains on"):
            heldout_arenas({"dungeon": 16}, stage)
    for name in ("group2_corridor", "dungeon1_pulls"):
        assert config(name).eval.heldout == {}
        assert ".InstanceRow = 2" not in bodies[name]


def test_the_stand_in_split_is_weighted_by_the_episodes_that_had_it():
    """clear_standin over the episodes with the stand-in in a seat, clear_allbot over those without (PER_EVENT)."""
    assert PER_EVENT["clear_standin"] == "with_stand_in" and PER_EVENT["clear_allbot"] == "without_stand_in"
    names = ["clear_standin", "with_stand_in", "clear_allbot", "without_stand_in"]
    rows = np.array([
        [1.0, 1.0, 0.0, 0.0],      # with the stand-in, cleared
        [0.0, 1.0, 0.0, 0.0],      # with it, not cleared
        [0.0, 0.0, 1.0, 1.0],      # all bots, cleared
        [0.0, 0.0, 1.0, 1.0],
        [0.0, 0.0, 0.0, 1.0],
    ])
    out = dict(zip(names, means(rows, names)))
    assert out["clear_standin"] == pytest.approx(0.5)
    assert out["clear_allbot"] == pytest.approx(2.0 / 3.0)


def test_the_stand_in_split_reaches_forge_status(tmp_path):
    """The plain evaluation is all bots (clear_allbot); the with_human arm's clear_standin is the stand-in party's, and
    standin_gap their difference -- H's measure, within ~10 points."""
    assert ARM_SPLITS["with_human"]["clear_standin"] == ("clear_allbot", "standin_gap")
    loaded = config("group2_corridor")

    class Spec:
        scenario = "group2_corridor"

    class Tracker:
        history = [(10, 1.0)]
        best = 1.0
        best_env_steps = 10
        evals_since_best = 0

    progress = ProgressWriter(tmp_path, loaded, Spec())
    progress.evaluated(10, 1.0, None, Tracker(), summary={"clear_allbot": 0.8, "clear_standin": None, "cleared": 0.8})
    progress.arm_evaluated("with_human", {"score": 0.9, "episodes": 64, "clear_standin": 0.75})
    progress.write("training", 1, 10)
    import json
    written = json.loads((tmp_path / "progress.json").read_text())
    assert written["eval_clear_allbot"] == pytest.approx(0.8)
    assert written["eval_clear_standin"] == pytest.approx(0.75)
    assert written["eval_standin_gap"] == pytest.approx(0.05)


def test_the_video_collector_dry_run_names_every_worker_and_connects_to_none():
    """apps/forge/tools/collect-videos.sh --dry-run: one rsync over key-based, BatchMode SSH per worker (eli left out),
    each into videos/from-<worker>/ of this machine's run, videos only; nothing is connected to. A stage name is
    required, and nothing else is taken for one."""
    run = subprocess.run(["bash", str(COLLECT), "--dry-run", "dungeon3_deadmines"], capture_output=True, text=True,
                         timeout=30)
    assert run.returncode == 0, run.stderr
    lines = [line for line in run.stdout.splitlines() if line.startswith("rsync")]
    workers = ("spencer@192.168.0.66", "thomas@192.168.0.67", "sarah@192.168.0.68", "moloch@192.168.0.117")
    assert len(lines) == len(workers)
    for worker, line in zip(workers, lines):
        name = worker.split("@")[0]
        assert f"{worker}:azerothcore/var/animus-forge/shared/runs/dungeon3_deadmines/videos/" in line
        assert f"var/animus-forge/shared/runs/dungeon3_deadmines/videos/from-{name}/" in line
        assert "BatchMode=yes" in line and "--exclude=\\*" in line
    assert "192.168.0.65" not in run.stdout and "eli" not in run.stdout
    assert "password" not in COLLECT.read_text().lower().replace("never a password prompt", "")
    custom = subprocess.run(["bash", str(COLLECT), "--dry-run", "--workers", "a@h1", "--remote-dir", "/srv/forge",
                             "group2_corridor"], capture_output=True, text=True, timeout=30)
    assert custom.returncode == 0 and "a@h1:/srv/forge/var/animus-forge/shared/runs/group2_corridor/videos/" in \
        custom.stdout
    for bad in ([], ["../etc"], ["--bogus", "x"]):
        refused = subprocess.run(["bash", str(COLLECT), "--dry-run", *bad], capture_output=True, text=True, timeout=30)
        assert refused.returncode != 0, bad
