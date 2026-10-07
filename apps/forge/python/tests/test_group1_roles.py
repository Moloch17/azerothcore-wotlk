"""G1 group1_roles (dungeon-curriculum G1; the sim's RolesEncounter): the config loads and reads its own measures, its
ladder steps on its gate alone with every price at full price from the start, its evaluation plays fixed seeds and
films them by roles_rung and won, it seeds from combat3_survive with move4_follow merged in, the co-op partners never
take the drilled seat, and each drill's readings are weighted by the episodes that drilled it.

The seeding itself (C3 and M4's party frames, by name) is test_party_frames_seeding's."""

import re
from pathlib import Path

import numpy as np
import pytest

from animus.config import TrainConfig
from animus.episode_means import PER_EVENT, means

CONFIGS = Path(__file__).resolve().parents[1] / "configs"
CURRICULUM = Path(__file__).resolve().parents[4] / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum"
STAGES_CPP = CURRICULUM / "Stages" / "Stages.cpp"
NAME = "group1_roles"
DRILLS = ("hold", "keep", "focus", "pull")


def config() -> TrainConfig:
    return TrainConfig.load(CONFIGS / f"{NAME}.yaml")


def sim_columns() -> set[str]:
    """Every episode info column G1's sim can report: the scenario's own and the stand-in's, the encounters its arenas
    use (RolesEncounter, PartyEncounter: PartyGroup), the reward terms'."""
    names = set(re.findall(r'_info\.Add\("(\w+)"', (CURRICULUM / "StageScenario.cpp").read_text()))
    names |= set(re.findall(r'_info\.Add\((?:std::string\()?"(\w+)"',
                            (CURRICULUM / "Encounters" / "StandInSeat.cpp").read_text()))
    for encounter in ("RolesEncounter", "PartyEncounter"):
        names |= set(re.findall(r'table\.Add\("(\w+)"', (CURRICULUM / "Encounters" / f"{encounter}.cpp").read_text()))
    # RolesEncounter's per-drill wins, added from a list.
    names |= set(re.findall(r'std::pair\{ "(\w+)"', (CURRICULUM / "Encounters" / "RolesEncounter.cpp").read_text()))
    names |= {"reward_" + name for name in re.findall(r'return "(\w+)";',
                                                       (CURRICULUM / "Rewards" / "CombatReward.cpp").read_text())}
    return names


def stage_body() -> str:
    for body in re.findall(r"stages\.push_back\(\{(.*?)\n        \}\);", STAGES_CPP.read_text(), re.S):
        if re.search(rf'\.Name = "{NAME}"', body):
            return body
    raise AssertionError(f"{NAME} is not in Stages.cpp")


def stage_order() -> list[str]:
    return re.findall(r'stages\.push_back\(\{\s*\.Name = "(\w+)"', STAGES_CPP.read_text())


def test_the_config_loads_and_reads_its_own_measures():
    loaded = config()
    assert loaded.run_name == NAME
    known = sim_columns()
    missing = [column for column in (*loaded.status.headline, *loaded.eval.report) if column not in known]
    assert not missing, missing
    assert set(loaded.status.targets) <= set(loaded.status.headline)
    # C3's own targets dropped.
    assert "survived" not in loaded.status.targets and "packs_cleared" not in loaded.status.targets
    assert loaded.convergence.measure == "won" and loaded.layout_sampling.metric == "won"
    assert loaded.status.headline[0] == "won"
    for drill in DRILLS:
        assert f"won_{drill}" in loaded.status.headline and f"drill_{drill}" in loaded.eval.report
    assert {"hold_share", "kept_share", "focus_share", "clean_share", "rejoined", "roles_rung"} \
        <= set(loaded.status.headline)


def test_the_ladder_steps_on_its_gate_alone_with_full_prices_from_the_start():
    """The dungeon plan's rules: the cost ladder off (every price at full price from the first step) and the shaping
    fade stepping on its gate at the first evaluation that meets it, never on a plateau or the classes' own rungs."""
    loaded = config()
    assert loaded.costs.enabled is False
    assert loaded.fade.enabled is True and loaded.fade.require_plateau is False
    assert loaded.fade.gate_metric == "won" and 0.0 < loaded.fade.gate_value <= 1.0
    assert loaded.fade.gate_metric in loaded.eval.report and loaded.fade.gate_metric in loaded.status.headline
    assert loaded.fade.rungs == (1.0, 0.5, 0.25, 0.0)
    assert loaded.fade.moving_classes >= 100


def test_the_evaluation_plays_fixed_seeds_and_films_them_by_its_rung():
    """Fixed seeds (the same parties at the same rungs every evaluation), argmax; the eval videos read the rung off a
    column named *_rung (Vision::EvalVideoRungColumn, never at_top_rung) and the outcome off `won`
    (EVAL_VIDEO_OUTCOMES)."""
    loaded = config()
    assert loaded.eval.seed == 1000 and loaded.eval.deterministic is True
    assert loaded.eval.episodes >= 4 * 64
    rungs = [c for c in loaded.eval.report if (c.endswith("_rung") or c in ("rung", "tier")) and
             not c.startswith("at_top")]
    assert rungs == ["roles_rung"]
    assert "won" in loaded.eval.report
    # The I7 arms: the stand-in and the fixed partners.
    assert loaded.eval.arms == {"with_human": 64, "with_partners": 64}


def test_the_seed_chain_and_the_merge():
    """G1 extends combat3_survive and merges move4_follow (the overseer's ruling); both come before it, and the config
    seeds from the chain and merges from the merged stage's checkpoint (init_from and merge_from auto)."""
    body = stage_body()
    assert re.search(r'\.Extends = "combat3_survive"', body)
    assert re.search(r'\.Merges = \{ "move4_follow" \}', body)
    order = stage_order()
    assert order.index("combat3_survive") < order.index(NAME) and order.index("move4_follow") < order.index(NAME)
    loaded = config()
    assert loaded.init_from == "auto" and loaded.merge_from == "auto" and loaded.seed_from == "latest"
    stage = {"seed_chain": ["combat3_survive", "combat2_packs"], "merges": ["move4_follow"]}
    assert [Path(p).parent.name for p in loaded.resolved_init_from(stage)] == ["combat3_survive", "combat2_packs"]
    assert [Path(p).parent.name for p in loaded.resolved_merge_from(stage)] == ["move4_follow"]


def test_the_stage_perceives_through_the_party_frames_and_drills_every_role():
    body = stage_body()
    blocks = re.search(r"\.Blocks = \{([^}]*)\}", body).group(1)
    assert "PartyFrames" in blocks and "Combat" in blocks and "Sight" in blocks
    assert "Party," not in blocks and "Support" not in blocks and "Pack," not in blocks
    arenas = dict(re.findall(r'\{ \.Name = "(\w+)"[^{}]*?\.Roles = RolesDrill::(\w+)', body, re.S))
    assert arenas == {"tank_hold": "Hold", "heal_keep": "Keep", "damage_discipline": "Focus", "pull": "Pull"}
    assert body.count(".RespawnAtEntrance = true") == 4 and ".DeathRuns = true" not in body


def test_partners_with_the_party_and_the_stand_in():
    loaded = config()
    partners = loaded.cast.partners
    assert partners.enabled and partners.share > 0.0 and partners.max_partners >= 1
    assert partners.stages == ("combat3_survive",) and partners.snapshot_every_env_steps > 0
    assert partners.score == "won"
    conf = (Path(__file__).resolve().parents[4] / "src" / "server" / "apps" / "worldserver" /
            "worldserver.conf.dist").read_text()
    share = re.search(r"^AnimusForge\.Curriculum\.Roles\.StandInShare = (\d+)", conf, re.M)
    assert share and 0 < int(share.group(1)) < 100
    assert re.search(r"^AnimusForge\.Stage\.group1_roles\.TicksPerDecision = 5", conf, re.M)


def test_the_drill_readings_are_weighted_by_the_episodes_that_drilled_them():
    for drill in DRILLS:
        assert PER_EVENT[f"won_{drill}"] == f"drill_{drill}"
    assert PER_EVENT["hold_share"] == "drill_hold" and PER_EVENT["kept_share"] == "drill_keep"
    assert PER_EVENT["focus_share"] == "drill_focus" and PER_EVENT["clean_share"] == "packs_cleared"
    names = ["won_hold", "drill_hold", "hold_share", "clean_share", "packs_cleared"]
    rows = np.array([
        [1.0, 1.0, 0.9, 1.0, 2.0],     # a hold drill, won, 90% held, both packs clean
        [0.0, 0.0, 0.0, 0.5, 4.0],     # another drill: half its four packs clean
        [0.0, 1.0, 0.5, 0.0, 0.0],     # a hold drill, lost, nothing cleared
    ])
    out = dict(zip(names, means(rows, names)))
    assert out["won_hold"] == pytest.approx(0.5)
    assert out["hold_share"] == pytest.approx(0.7)
    assert out["clean_share"] == pytest.approx((1.0 * 2 + 0.5 * 4) / 6)
