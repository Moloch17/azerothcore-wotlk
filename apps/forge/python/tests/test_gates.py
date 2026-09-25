"""The shipped configs against the curriculum that emits their columns, and the one gate helper left."""

import re
from pathlib import Path

import pytest

from animus.config import TrainConfig
from animus.evaluation import DERIVED_METRICS
from animus.gates import wilson_bound

CONFIGS = Path(__file__).resolve().parents[1] / "configs"


def _curriculum_roots() -> list[Path]:
    """Every directory the curriculum's sources live in, wherever they currently are.

    There have been three layouts: one Curriculum tree under animus-lib/src, then two after the library split into
    runtime and training roots, and then the module's own src once the library was folded into it. These tests care
    about what the sim can emit, not about which layout is in force, so they look in all of them and use what is
    there."""
    repo = Path(__file__).resolve().parents[4]
    root = repo / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum"
    return [root] if root.is_dir() else []


def _curriculum_file(relative: str) -> Path | None:
    """One curriculum source by its path below Curriculum/, from whichever root holds it."""
    for root in _curriculum_roots():
        candidate = root / relative
        if candidate.is_file():
            return candidate
    return None


def test_wilson_bound_tightens_with_evidence():
    assert wilson_bound(1.0, 0, 0.95, lower=True) == 0.0
    few, many = wilson_bound(0.95, 20, 0.95, lower=True), wilson_bound(0.95, 2000, 0.95, lower=True)
    assert 0.0 < few < many < 0.95
    assert wilson_bound(0.5, 100, 0.95, lower=False) > 0.5 > wilson_bound(0.5, 100, 0.95, lower=True)
    with pytest.raises(ValueError):
        wilson_bound(0.5, 10, 1.5, lower=True)


def sim_stage_columns() -> dict[str, set[str]]:
    """Per stage, the episode info columns it can actually emit.

    An encounter is built only when an arena asks for it (StageScenario's constructor), and it registers its
    columns only when it is built: a stage whose arenas are all Opposition::Pulls has no OwnerEncounter and so
    never emits owner_deaths. A report column a stage cannot emit is a column that reads blank for the whole run."""
    def columns(relative: str) -> set[str]:
        path = _curriculum_file(relative)
        return set(re.findall(r'Add\("([a-z0-9_]+)"', path.read_text())) if path else set()

    per_encounter = {name: columns(f"Encounters/{cls}.cpp") for name, cls in (
        ("opponent", "OpponentEncounter"), ("owner", "OwnerEncounter"), ("party", "PartyEncounter"),
        ("pulls", "PullsEncounter"), ("creature", "CreatureEncounter"), ("hazards", "HazardEncounter"),
        ("ambush", "AmbushEncounter"), ("travel", "TravelEncounter"), ("flag", "FlagEncounter"),
        ("director", "DirectorEncounter"), ("instance", "InstanceEncounter"), ("quest", "QuestEncounter"),
        ("gather", "GatherEncounter"), ("town", "TownEncounter"))}
    # The three life encounters share a base whose columns every one of them emits.
    for life in ("quest", "gather", "town"):
        per_encounter[life] |= columns("Encounters/LifeEncounter.cpp")
    # Everything registered outside the encounters -- the scenario, the blocks, the rewards -- is emitted by every
    # stage; only an encounter's columns depend on the arenas.
    always = set(DERIVED_METRICS)
    for root in _curriculum_roots():
        for source in root.rglob("*.cpp"):
            if "Encounters" not in source.parts:
                always |= set(re.findall(r'Add\("([a-z0-9_]+)"', source.read_text()))

    stages_file = _curriculum_file("Stages/Stages.cpp")
    assert stages_file, "Stages.cpp is in no curriculum root"
    source = stages_file.read_text()
    out: dict[str, set[str]] = {}
    for body in re.findall(r"stages\.push_back\(\{(.*?)\n        \}\);", source, re.S):
        name = re.search(r'\.Name = "([^"]+)"', body)
        if not name:
            continue
        active: set[str] = set()
        for arena in re.findall(r'\{\s*\.Name = "[a-z0-9_]+"(.*?)\}', body[name.end():], re.S):
            against = (re.search(r"\.Against = Opposition::(\w+)", arena) or [None, "Creature"])[1]
            if against in ("ScriptedPlayer", "MirrorSeat", "Flag"):
                active.add("opponent")
            for opposition, encounter in (("Pulls", "pulls"), ("Creature", "creature"), ("Hazards", "hazards"),
                                          ("Travel", "travel"), ("Flag", "flag"), ("Instance", "instance"),
                                          ("Quest", "quest"), ("Gather", "gather"), ("Town", "town")):
                if against == opposition:
                    active.add(encounter)
            if ".Owner = true" in arena:
                active.add("owner")
            if ".PartyGroup = true" in arena:
                active.add("party")
            if ".Directed = true" in arena:
                active.add("director")
            if re.search(r"\.Ambushers = [1-9]", arena):
                active.add("ambush")
        out[name.group(1)] = set(always).union(*(per_encounter[e] for e in active)) if active else set(always)
    assert out, "no stage definitions found"
    return out


@pytest.mark.parametrize("path", sorted(CONFIGS.glob("*.yaml")), ids=lambda p: p.stem)
def test_shipped_configs_load(path):
    config = TrainConfig.load(path)
    assert config.run_name or path.stem == "fast"


def test_layout_sampling_metric_must_exist():
    config = TrainConfig.load(CONFIGS / "stage8_duel.yaml")
    assert config.layout_sampling.metric in (*DERIVED_METRICS, *sim_stage_columns()["stage8_duel"])


def stage_definitions() -> dict[str, dict]:
    """Each stage's Extends, Merges and Blocks, in queue order."""
    stages_file = _curriculum_file("Stages/Stages.cpp")
    assert stages_file, "Stages.cpp is in no curriculum root"
    source = stages_file.read_text()
    out: dict[str, dict] = {}
    for body in re.findall(r"stages\.push_back\(\{(.*?)\n        \}\);", source, re.S):
        name = re.search(r'\.Name = "([^"]+)"', body)
        if not name:
            continue
        def field(pattern: str, default: str = "") -> str:
            found = re.search(pattern, body)
            return found.group(1) if found else default
        out[name.group(1)] = dict(
            extends=field(r'\.Extends = "([^"]*)"'),
            merges=re.findall(r'"(stage[0-9a-z_]+)"', field(r"\.Merges = \{([^}]*)\}")),
            blocks={b.strip() for b in field(r"\.Blocks = \{([^}]*)\}").split(",") if b.strip()},
        )
    assert out, "no stage definitions found"
    return out


def test_no_stage_relearns_a_block_an_earlier_stage_already_trained():
    """A block the parent does not have starts from zero (animus.bootstrap), and a merge is the only way back.

    The curriculum branches: the PvE line trains pack, gauntlet and support, the PvP line drops all three, and a
    stage that rejoins the PvE line by extending the PvP one would start them again from nothing -- three stages
    of training spent twice. Every block should be introduced by exactly one stage and carried by extension or
    merge from then on."""
    stages = stage_definitions()
    order = list(stages)
    relearned = {}
    for index, name in enumerate(order):
        stage = stages[name]
        inherited: set[str] = set()
        for ancestor in [stage["extends"], *stage["merges"]]:
            if ancestor in stages:
                inherited |= stages[ancestor]["blocks"]
        trained_before: set[str] = set()
        for earlier in order[:index]:
            trained_before |= stages[earlier]["blocks"]
        lost = (stage["blocks"] - inherited) & trained_before
        if lost:
            relearned[name] = sorted(lost)
    assert not relearned, ("these stages start a block from zero that an earlier stage already trained; merge "
                           f"the stage that trained it: {relearned}")
