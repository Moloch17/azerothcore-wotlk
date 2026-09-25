import math

from animus.mappo.trainer import MappoConfig, horizon_seconds, per_decision


def test_reference_interval_keeps_the_configured_values():
    config = MappoConfig(gamma=0.997, gae_lambda=0.985, reference_decision_ms=100)
    assert per_decision(config, 100) == (0.997, 0.985)


def test_horizon_is_the_same_in_seconds_at_any_decision_interval():
    config = MappoConfig(gamma=0.999, gae_lambda=0.99, reference_decision_ms=100)
    horizons = []
    for decision_ms in (50, 100, 200):
        gamma, gae_lambda = per_decision(config, decision_ms)
        # Discounting over one second of game time does not depend on how many decisions it takes.
        assert math.isclose(gamma ** (1000 / decision_ms), 0.999**10)
        assert math.isclose(gae_lambda ** (1000 / decision_ms), 0.99**10)
        horizons.append(horizon_seconds(gamma, decision_ms))

    assert max(horizons) - min(horizons) < 0.1


def test_horizon_seconds():
    assert math.isclose(horizon_seconds(0.997, 100), 100 / 1000 / 0.003)
    assert horizon_seconds(1.0, 100) == float("inf")
