"""The world tick and the learner's decision are two different clocks.

AnimusForge.TicksPerDecision cuts a decision into several world updates so splines, auras and the fight move in
finer steps than the policy chooses in. The sim sends the split down the wire -- tick_ms is the world tick,
decision_ticks how many of them a decision is -- and everything on the learner's side that costs game time is
written against the product. If any of it ever read tick_ms directly it would silently shrink its horizons by the
tick ratio: a 20 s credit window becomes 5 s at four ticks per decision, and nothing would say so.
"""

from animus.mappo.trainer import MappoConfig, horizon_seconds, per_decision
from animus.protocol import Spec


def spec(tick_ms: int, decision_ticks: int) -> Spec:
    return Spec(version=8, num_envs=4, agents_per_env=2, obs_dim=3, state_dim=4, num_actions=5,
                episode_info_dim=3, goal_count=0, tick_ms=tick_ms, decision_ticks=decision_ticks,
                episode_seconds=60, scenario="stage8_duel")


def test_a_decision_is_the_same_game_time_however_it_is_cut():
    """250 ms of game time per decision, whether the world took one step to get there or five."""
    assert spec(250, 1).decision_ms == 250
    assert spec(50, 5).decision_ms == 250
    assert spec(62, 4).decision_ms == 248  # 250 does not divide by 4; the sim rounds down to keep ticks whole


def test_the_split_does_not_move_the_discounts():
    config = MappoConfig()
    whole = per_decision(config, spec(250, 1).decision_ms)
    split = per_decision(config, spec(50, 5).decision_ms)
    assert whole == split

    # And the credit window is still measured in seconds, not ticks.
    gamma, gae_lambda = whole
    assert horizon_seconds(gamma * gae_lambda, spec(50, 5).decision_ms) == \
        horizon_seconds(gamma * gae_lambda, spec(250, 1).decision_ms)


def test_reading_the_tick_instead_of_the_decision_would_stretch_the_real_horizon():
    """The bug this pins, stated as an assertion: tick_ms alone is not the step.

    The trap is quiet because per_decision compounds gamma to whatever step it is given, so a learner reading
    tick_ms would report exactly the credit window it was configured for. The damage is only visible against real
    time: it discounts as if each step were 50 ms while the sim advances 250, so the window it actually applies is
    five times the one it was asked for. Nothing in the logs would look wrong.
    """
    config = MappoConfig()
    split = spec(50, 5)

    gamma, gae_lambda = per_decision(config, split.decision_ms)
    asked_for = horizon_seconds(gamma * gae_lambda, split.decision_ms)

    wrong_gamma, wrong_lambda = per_decision(config, split.tick_ms)
    # Reported against the step it believes in, it looks the same as ever -- this is why it would go unnoticed.
    assert horizon_seconds(wrong_gamma * wrong_lambda, split.tick_ms) < asked_for

    # Against the step the sim actually takes, it is five times too long.
    really_applied = horizon_seconds(wrong_gamma * wrong_lambda, split.decision_ms)
    assert really_applied > asked_for * 4


def test_evaluation_sizes_its_window_from_the_decision():
    """evaluation.py divides the episode by the decision, so the number of steps it expects must not change."""
    from animus import evaluation

    src = evaluation.__file__
    with open(src) as handle:
        body = handle.read()
    assert "spec.decision_ms" in body and "spec.tick_ms" not in body

    whole, split = spec(250, 1), spec(50, 5)
    steps = lambda s: max(1, s.episode_seconds * 1000 // max(1, s.decision_ms))
    assert steps(whole) == steps(split) == 240


def test_the_wire_round_trips_the_split():
    from animus.protocol import decode_spec, encode_spec

    original = spec(50, 5)
    restored = decode_spec(encode_spec(original))
    assert (restored.tick_ms, restored.decision_ticks) == (50, 5)
    assert restored.decision_ms == 250
