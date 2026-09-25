"""The reward audit: saying so when a shaping term has become the objective."""

from animus.rewards import audit, describe, reward_mix


def test_reward_mix_reads_the_ledger_columns_only():
    row = {"update": 3, "episode_killed": 0.5, "episode_reward_kill": 5.2, "episode_reward_death": -5.3,
           "episode_reward_order_match": 0.34, "entropy": 1.4}
    assert reward_mix(row) == {"kill": 5.2, "death": -5.3, "order_match": 0.34}


def test_a_nudge_is_not_reported():
    """order_match at 0.001 a decision: 0.34 an episode against a 5.4 kill, which is what a nudge looks like."""
    assert audit({"kill": 5.4, "death": -5.4, "damage_dealt": 2.3, "order_match": 0.34}) is None


def test_shaping_level_with_the_kill_is_reported():
    """order_match at 0.01 a decision, as first shipped: 5.01 against a 5.23 kill."""
    finding = audit({"kill": 5.23, "death": -5.27, "damage_dealt": 2.47, "order_match": 5.01})
    assert finding is not None
    term, earned, outcome, share = finding
    assert term == "order_match" and earned == 5.01 and outcome == 5.23
    assert 0.95 < share < 0.97
    assert "order_match" in describe(finding, {"kill": 5.23, "order_match": 5.01})


def test_the_farmable_revive_is_reported():
    """druid_dps in stage 4: 57.55 of a 65.08 return, against a kill worth a fraction of it. Caught on the
    defaults, which is the point -- a resurrection is a means and not what the stage is for, so it is shaping
    and has to be measured against the kill like any other nudge."""
    finding = audit({"kill": 4.1, "death": -2.0, "revive": 57.55, "teammate_healing": 3.0})
    assert finding is not None and finding[0] == "revive"
    assert finding[3] > 10.0


def test_a_penalty_is_never_reported():
    """The largest negative term in a fight is the death, which is the point of having one."""
    assert audit({"kill": 5.0, "death": -40.0, "hazard": -12.0}) is None


def test_nothing_to_measure_against_is_not_a_finding():
    """A stage whose outcome term has not been earned yet says nothing rather than crying wolf."""
    assert audit({"damage_dealt": 3.0, "step_cost": -0.2}) is None
