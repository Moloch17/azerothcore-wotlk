"""M2 seek's measures (SeekEncounter's episode info; perception-goals P1): found, the time to the first sight and from
sight to arrival, the rooms entered before finding and the rooms re-entered, found by room and by object, and the found
rate in the deepest rooms -- as the training means, the evaluation tables and `forge status` read them."""

from pathlib import Path

import numpy as np
import pytest

from animus.config import TrainConfig
from animus.episode_means import PER_EVENT, means
from animus.evaluation import EvalResult

CONFIGS = Path(__file__).resolve().parents[1] / "configs"

COLUMNS = ("found", "find_seconds", "sighted", "sight_seconds", "found_sighted", "sight_to_arrival", "rooms_entered",
           "rooms_reentered", "room_entries", "revisit_rate", "rooms_before_found", "seek_room", "seek_object",
           "difficulty", "deep_room")


def episode(found=0.0, find=0.0, sighted=0.0, sight=0.0, entered=0, reentered=0, room=0, obj=0, tier=0):
    both = 1.0 if found and sighted else 0.0
    entries = entered + reentered
    return [found, find if found else 0.0, sighted, sight if sighted else 0.0, both,
            (find - sight) if both else 0.0, entered, reentered, entries,
            reentered / entries if entries else 0.0, entered if found else 0.0, room, obj, tier,
            1.0 if tier == 2 else 0.0]


EPISODES = np.array([
    episode(found=1, find=40, sighted=1, sight=30, entered=3, reentered=1, room=0, obj=0, tier=0),
    episode(found=1, find=100, sighted=1, sight=70, entered=8, reentered=3, room=2, obj=1, tier=2),
    episode(found=0, sighted=1, sight=200, entered=12, reentered=8, room=2, obj=0, tier=2),
    episode(found=0, entered=15, reentered=5, room=1, obj=1, tier=1),
], dtype=np.float32)


def test_the_per_event_measures_are_averaged_over_their_events_only():
    """An episode that never found the object reports find_seconds 0, not "never": averaged over every episode the
    mean would read faster as the policy failed more. Each per-event column is weighted by its own event."""
    for column, count in (("find_seconds", "found"), ("rooms_before_found", "found"), ("sight_seconds", "sighted"),
                          ("sight_to_arrival", "found_sighted"), ("revisit_rate", "room_entries")):
        assert PER_EVENT[column] == count and count in COLUMNS
    out = dict(zip(COLUMNS, means(EPISODES, COLUMNS)))
    assert out["found"] == pytest.approx(0.5)
    assert out["find_seconds"] == pytest.approx(70.0)                   # (40 + 100) / 2 found
    assert out["sight_seconds"] == pytest.approx(100.0)                 # (30 + 70 + 200) / 3 sighted
    assert out["sight_to_arrival"] == pytest.approx(20.0)               # (10 + 30) / 2
    assert out["rooms_before_found"] == pytest.approx(5.5)              # (3 + 8) / 2
    assert out["revisit_rate"] == pytest.approx(17.0 / 55.0)            # re-entries over every entry
    # No episode with the event: NaN, not 0.
    none = np.array([episode(entered=2)], dtype=np.float32)
    assert np.isnan(dict(zip(COLUMNS, means(none, COLUMNS)))["find_seconds"])


def test_the_evaluation_splits_found_by_room_and_by_object_and_reads_the_deepest_rooms():
    result = EvalResult("learner", np.zeros(len(EPISODES)), EPISODES, COLUMNS,
                        categories={"seek_room": ["hall_west_1", "hall_end", "west_end_2_back"],
                                    "seek_object": ["chest", "crate"]})
    summary = result.summary(COLUMNS)
    by = summary["categories"]
    assert by["seek_room=west_end_2_back"]["episodes"] == 2 and by["seek_room=west_end_2_back"]["found"] == 0.5
    assert by["seek_room=hall_west_1"]["found"] == 1.0 and by["seek_room=hall_end"]["found"] == 0.0
    assert by["seek_object=chest"]["found"] == 0.5 and by["seek_object=crate"]["found"] == 0.5
    # The deepest third (deep_room): the P1 gate's reading, beside the found rate by tier.
    assert summary["found_deepest"] == pytest.approx(0.5)
    assert summary["difficulties"]["2"]["found"] == pytest.approx(0.5)
    # The per-event columns in a category are weighted too: the deepest rooms' one arrival took 100 s.
    assert by["seek_room=west_end_2_back"]["find_seconds"] == pytest.approx(100.0)
    # Merged shares keep the names.
    merged = EvalResult.merged([result, result])
    assert merged.categories == result.categories
    assert merged.summary(COLUMNS)["categories"]["seek_object=crate"]["episodes"] == 4


def test_the_stage_config_reads_its_own_measures():
    config = TrainConfig.load(CONFIGS / "move2_seek.yaml")
    assert config.run_name == "move2_seek"
    assert config.convergence.measure == "found" and config.fade.gate_metric == "found"
    assert config.costs.gate_metric == "found" and config.layout_sampling.metric == "found"
    # M1's look entropy, a longer horizon for a 300 s search, and the whole rollout as the GRU's BPTT.
    assert config.mappo.look_entropy_coef == pytest.approx(0.004)
    assert config.mappo.gamma == pytest.approx(0.998)
    assert config.mappo.chunk_length >= config.rollout_length
    assert config.mappo.recurrent_size > 0
    # Amendment 9: 78 episodes at the training rung every 10M; the full sweep (every (room, object) pair once) is the
    # held-out arena, at the stage's end only.
    assert config.eval.episodes == 78 and config.eval.every_env_steps == 10_000_000
    assert config.eval.heldout == {"sweep": 39 * 5} and not config.eval.heldout_on_best
    assert config.eval.heldout_every > 100
    from animus.train import heldout_due
    assert not heldout_due(5, config.eval.heldout_every, False, True, config.eval.heldout_on_best)
    assert heldout_due(5, config.eval.heldout_every, True, False, config.eval.heldout_on_best)
    assert heldout_due(5, 4, False, True)       # elsewhere a new best still plays them
    assert not config.mappo.map_vin
    headline = set(config.status.headline)
    assert {"found", "found_deepest", "sight_seconds", "sight_to_arrival", "rooms_before_found",
            "revisit_rate", "found_hallway", "found_doorway", "found_room", "found_deep", "seek_rung"} <= headline
    # None of M1's marker targets is left over.
    assert "arrived" not in config.status.targets and set(config.status.targets) <= headline
    assert set(config.eval.report) >= {"found", "find_seconds", "sight_seconds", "rooms_entered", "rooms_reentered"}
