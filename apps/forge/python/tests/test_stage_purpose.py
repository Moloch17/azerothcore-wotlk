"""Each stage's purpose is paid as an Outcome or a Cost term (movement-curriculum plan §3, "rewards check before each
run").

The lesson of 2026-10-05: stage3's dummy output and stage6's drill lessons were Shaping, the shaping fade took them
away, and the score rose while what the stage was for fell. So every stage names the term its purpose is paid as,
here, and this checks that the term is an Outcome or a Cost (RewardLedger.h), that an encounter the stage's arenas
use lists it among its reward terms, and that it is actually paid there. A stage without a row fails: adding one is
how a stage's purpose gets written down.
"""

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[4]
CURRICULUM = REPO / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum"
STAGES_CPP = CURRICULUM / "Stages" / "Stages.cpp"
LEDGER = CURRICULUM / "Rewards" / "RewardLedger.h"

# Stage -> the terms its purpose is paid as. M2-M7 add their rows as they land.
PURPOSE = {
    # M1: stopping on the marker -- Arrive is paid only when the seat is stopped inside the radius.
    "move1_controls": ("Arrive",),
}

# Stage -> the terms it pays that must stay Shaping (they fade): a nudge mistaken for the lesson is the failure this
# file exists for, the other way round. M1's Facing is half the change in cos(bearing to the marker), and Progress the
# straight-line distance closed over the leg, both started over at each marker.
SHAPING = {
    "move1_controls": ("Progress", "Facing"),
}

# Opposition -> the encounter source that pays it.
ENCOUNTER = {
    "Markers": "Encounters/MarkerEncounter.cpp",
    "Travel": "Encounters/TravelEncounter.cpp",
    "Creature": "Encounters/CreatureEncounter.cpp",
    "Dummy": "Encounters/DummyEncounter.cpp",
}


def categories() -> dict[str, str]:
    """RewardTerm -> "Outcome", "Cost" or "Shaping", from RewardTermCategory's switch."""
    text = LEDGER.read_text()
    body = text[text.index("RewardTermCategory(RewardTerm term)"):text.index("EveryRewardTermCategorised")]
    out, pending = {}, []
    for token in re.finditer(r"case RewardTerm::(\w+):|return RewardCategory::(\w+);", body):
        if token.group(1):
            pending.append(token.group(1))
        else:
            out.update({term: token.group(2) for term in pending})
            pending = []
    return out


def stage_bodies() -> dict[str, str]:
    out = {}
    for body in re.findall(r"stages\.push_back\(\{(.*?)\n        \}\);", STAGES_CPP.read_text(), re.S):
        name = re.search(r'\.Name = "(\w+)",\s*\.Suffix', body)
        if name:
            out[name.group(1)] = body
    return out


def oppositions(body: str) -> set[str]:
    return set(re.findall(r"\.Against = Opposition::(\w+)", body))


def listed_terms(source: str) -> set[str]:
    text = (CURRICULUM / source).read_text()
    found = re.search(r"::RewardTerms\(\) const\s*\{\s*return \{(.*?)\};", text, re.S)
    return set(re.findall(r"RewardTerm::(\w+)", found.group(1))) if found else set()


def test_the_categories_parse():
    parsed = categories()
    assert parsed.get("Arrive") == "Outcome" and parsed.get("StepCost") == "Cost"
    assert parsed.get("Progress") == "Shaping" and parsed.get("Facing") == "Shaping"


def test_every_stage_names_its_purpose():
    missing = sorted(set(stage_bodies()) - set(PURPOSE))
    assert not missing, f"stages with no purpose row in test_stage_purpose.PURPOSE: {missing}"


@pytest.mark.parametrize("stage", sorted(stage_bodies()))
def test_the_purpose_is_an_outcome_or_cost_the_stage_pays(stage):
    parsed = categories()
    sources = [ENCOUNTER[o] for o in oppositions(stage_bodies()[stage]) if o in ENCOUNTER]
    assert sources, f"{stage}: no known encounter for its arenas' oppositions {oppositions(stage_bodies()[stage])}"
    for term in PURPOSE[stage]:
        assert parsed.get(term) in ("Outcome", "Cost"), (
            f"{stage}'s purpose {term} is {parsed.get(term)}: Shaping fades away, so the stage would stop paying for "
            "what it is for")
        paying = [s for s in sources if term in listed_terms(s)
                  and re.search(rf"ledger\.Add\(RewardTerm::{term}\b", (CURRICULUM / s).read_text())]
        assert paying, f"{stage}: no encounter its arenas use lists and pays {term}"


@pytest.mark.parametrize("stage", sorted(SHAPING))
def test_the_stage_nudges_stay_shaping(stage):
    parsed = categories()
    for term in SHAPING[stage]:
        assert parsed.get(term) == "Shaping", f"{stage}'s nudge {term} is {parsed.get(term)}, not Shaping"
        assert term not in PURPOSE.get(stage, ()), f"{stage} names its nudge {term} as its purpose"
