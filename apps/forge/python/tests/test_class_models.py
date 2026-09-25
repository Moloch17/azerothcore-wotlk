"""One model per class, but every build it has still measured on its own.

The merge joined a class's roles into one policy. What it must not join is the bookkeeping: a paladin whose
protection build wins and whose holy build does not has to be sampled, weighted and reported as two things, or the
healing disappears into the class average and nothing ever asks for more of it.

The grain is the build, not the role. A role could not separate two builds that share it, and the druid has two
that do -- a feral cat and a balance druid are both "damage" and are not equally hard to win with.
"""

import numpy as np
import pytest

from animus.evaluation import EvalResult
from animus.evaluation import casting_weights
from animus.protocol import MAX_SPECS

SPECS = {"paladin": ["holy", "protection", "retribution"], "mage": ["arcane", "fire", "frost"],
         "druid": ["balance", "feral_cat", "feral_bear", "restoration"]}


def result(layouts, specs, scores):
    """An eval result whose episode info carries the spec column, as the sim reports it."""
    return EvalResult(policy="learner", arenas=("duel",), layouts=list(layouts),
                      returns=np.array(scores, dtype=np.float32),
                      infos=np.array([[float(SPECS[layout].index(spec))]
                                      for layout, spec in zip(layouts, specs)], dtype=np.float32),
                      info_names=("spec",), spec_names=dict(SPECS))


def test_a_class_is_summarised_per_build_as_well_as_whole():
    summary = result(["paladin"] * 4, ["protection", "protection", "holy", "holy"],
                     [9.0, 9.0, 1.0, 1.0]).summary(())

    # The class average says nothing useful: it is exactly between a good protection build and a bad holy one.
    assert summary["castings"]["paladin_protection"]["score"] == pytest.approx(9.0)
    assert summary["castings"]["paladin_holy"]["score"] == pytest.approx(1.0)
    assert set(summary["castings"]) == {"paladin_protection", "paladin_holy"}


def test_two_builds_of_one_role_are_told_apart():
    """The case a role could not reach: both of these would have been "damage"."""
    summary = result(["druid"] * 4, ["feral_cat", "feral_cat", "balance", "balance"],
                     [9.0, 9.0, 1.0, 1.0]).summary(())

    assert summary["castings"]["druid_feral_cat"]["score"] == pytest.approx(9.0)
    assert summary["castings"]["druid_balance"]["score"] == pytest.approx(1.0)


def test_the_weights_ask_for_more_of_the_build_that_is_failing():
    summary = {"castings": {"paladin_protection": {"score": 9.0}, "paladin_holy": {"score": 1.0},
                            "mage_frost": {"score": 5.0}}}
    baseline = {"castings": {"paladin_protection": {"score": 5.0}, "paladin_holy": {"score": 5.0},
                             "mage_frost": {"score": 5.0}}}
    weights = casting_weights(summary, baseline, strength=1.0, max_ratio=4.0)

    # The holy build gets the data, not the paladin: protection is not asked to train harder for its sake.
    assert weights["paladin_holy"] > weights["mage_frost"] > weights["paladin_protection"]
    assert np.mean(list(weights.values())) == pytest.approx(1.0)


def test_the_wire_vector_is_layout_major_with_a_slot_per_build():
    """The sim indexes it as layout * MAX_SPECS + spec, so every class carries the full width."""
    weights = {"paladin_holy": 2.0, "paladin_protection": 0.5, "mage_frost": 1.0}
    layouts = ["paladin", "mage"]
    vector = []
    for name in layouts:
        named = SPECS[name]
        for slot in range(MAX_SPECS):
            spec = named[slot] if slot < len(named) else ""
            vector.append(weights.get(f"{name}_{spec}", 1.0) if spec else 1.0)

    assert len(vector) == len(layouts) * MAX_SPECS
    assert vector[0 * MAX_SPECS + SPECS["paladin"].index("holy")] == 2.0
    assert vector[0 * MAX_SPECS + SPECS["paladin"].index("protection")] == 0.5
    assert vector[1 * MAX_SPECS + SPECS["mage"].index("frost")] == 1.0
    # A paladin has three builds and the vector has four slots: the spare is never drawn and stays even.
    assert vector[0 * MAX_SPECS + 3] == 1.0


def test_a_build_name_shared_by_two_classes_is_also_summarised_on_its_own():
    summary = result(["paladin", "druid"], ["holy", "restoration"], [4.0, 6.0]).summary(())

    assert summary["specs"]["holy"]["score"] == pytest.approx(4.0)
    assert summary["specs"]["restoration"]["score"] == pytest.approx(6.0)


def test_a_class_whose_run_draws_one_build_is_unchanged():
    summary = result(["mage"] * 3, ["frost"] * 3, [4.0, 5.0, 6.0]).summary(())
    assert set(summary["castings"]) == {"mage_frost"}
    assert summary["castings"]["mage_frost"]["score"] == pytest.approx(5.0)
