"""Movement realism in evaluations (animus.human.realism): the distance from the players' motion, eval_motion.npz, and
the evaluation's motion collection."""

import json
import math
from types import SimpleNamespace

import numpy as np
import pytest

from animus import protocol as p
from animus.evaluation import EvalResult, run_evaluation
from animus.human import motion, realism


def steps(rng: np.random.Generator, n: int, shift: float = 0.0) -> np.ndarray:
    feats = np.zeros((n, motion.F), np.float32)
    feats[:, motion.INDEX["fwd"]] = np.clip(rng.normal(0.8 + shift, 0.2, n), -3, 3)
    feats[:, motion.INDEX["planar"]] = np.abs(feats[:, motion.INDEX["fwd"]])
    feats[:, motion.INDEX["yaw_rate"]] = np.clip(rng.normal(shift, 0.3, n), -3, 3)
    return feats


def reference_of(feats: np.ndarray, contexts: np.ndarray) -> dict:
    hists = motion.histograms(feats, contexts)
    return {"format": 1, "built": "2026-10-04T00:00:00", "source": {}, "step_seconds": 0.25,
            "motion": {str(c): {"name": motion.context_name(c), "steps": int((contexts == c).sum()),
                                "hist": {name: counts.tolist() for name, counts in h.items()}}
                       for c, h in hists.items()},
            "metrics": {}}


def test_realism_is_zero_for_the_players_own_motion_and_grows_with_a_shift(tmp_path):
    rng = np.random.default_rng(0)
    feats = steps(rng, 4000)
    contexts = np.where(np.arange(4000) < 3000, 0, 1).astype(np.int16)
    path = tmp_path / "human_reference.json"
    path.write_text(json.dumps(reference_of(feats, contexts)))
    reference = realism.load_reference(path)
    assert set(reference["motion"]) == {0, 1}

    same = realism.score(feats, contexts, reference)
    assert same["realism_emd"] == 0.0
    assert same["realism_emd_ground"] == 0.0 and same["realism_emd_ground_combat"] == 0.0

    near = realism.score(steps(rng, 4000, 0.2), contexts, reference)
    far = realism.score(steps(rng, 4000, 0.8), contexts, reference)
    assert 0.0 < near["realism_emd"] < far["realism_emd"]
    assert far["features"]["ground"]["fwd"] == pytest.approx(0.8, abs=0.1)   # EMD in the feature's own units

    # A context the players never showed is named, not scored; the overall weighs the scored ones by steps.
    swim = np.full(500, 4, np.int16)
    mixed = realism.score(np.concatenate([feats, steps(rng, 500)]), np.concatenate([contexts, swim]), reference)
    assert mixed["unscored"] == ["swim"] and mixed["realism_emd"] == 0.0
    assert realism.columns(reference) == ["realism_emd", "realism_emd_ground", "realism_emd_ground_combat",
                                          "realism_disc"]
    assert math.isnan(realism.score(np.zeros((0, motion.F)), np.zeros(0), reference)["realism_emd"])


def test_a_reference_of_another_format_or_binning_is_refused(tmp_path):
    path = tmp_path / "ref.json"
    path.write_text(json.dumps({"format": 2, "motion": {}}))
    with pytest.raises(ValueError, match="format"):
        realism.load_reference(path)
    path.write_text(json.dumps({"format": 1, "motion": {"0": {"name": "ground", "steps": 1,
                                                               "hist": {"fwd": [1, 2, 3]}}}}))
    with pytest.raises(ValueError, match="bins"):
        realism.load_reference(path)


def test_eval_motion_npz_has_the_human_windows_layout(tmp_path):
    rng = np.random.default_rng(1)
    tracks = []
    for n in (20, 3, 12):
        s = np.zeros((n, motion.SAMPLE_DIM), np.float32)
        s[:, motion.T] = np.arange(n) * 0.25
        s[:, motion.X] = np.cumsum(rng.random(n))
        s[:, motion.SPEED] = 7.0
        s[:, motion.IN_COMBAT] = n == 12
        tracks.append(s)
    feats, contexts, per_track, per_context = realism.tracks_features(tracks)
    assert feats.shape == (19 + 2 + 11, motion.F) and len(contexts) == len(feats)
    windows, context, weight = realism.motion_windows(per_track, per_context)
    assert windows.shape == (12 + 0 + 4, motion.WINDOW, motion.F)
    assert set(context.tolist()) == {0, 1} and np.all(weight == 1.0)
    path = tmp_path / "eval_motion.npz"
    realism.write_motion(path, windows, context, weight, {"source": "eval"})
    with np.load(path) as data:
        assert data["windows"].dtype == np.float32 and data["windows"].shape == windows.shape
        assert data["context"].dtype == np.int16 and data["weight"].dtype == np.float32
        assert json.loads(str(data["meta"])) == {"source": "eval"}
    # Capped: a uniform draw, each kept window standing for the ones dropped.
    capped = realism.motion_windows(per_track, per_context, cap=4, rng=rng)
    assert len(capped[0]) == 4 and np.all(capped[2] == pytest.approx(16 / 4))


class WalkingEnv:
    """One env, two agents, 5-decision episodes; each agent walks along x at 1 yd a decision."""

    SPEC = p.Spec(version=p.PROTOCOL_VERSION, num_envs=1, agents_per_env=2, obs_dim=1, state_dim=1, num_actions=1,
                  episode_info_dim=1, goal_count=0, tick_ms=50, decision_ticks=5, episode_seconds=2,
                  scenario="fake", layouts=(p.Layout("warrior_dps", 1, 1),), episode_info_names=("level",),
                  kinematics_dim=motion.SAMPLE_DIM)

    def __init__(self):
        self.t = 0
        self.seed = 0

    def _step(self, done: bool, seed: int) -> p.Step:
        kinematics = np.zeros((1, 2, motion.SAMPLE_DIM), np.float32)
        kinematics[0, :, motion.T] = self.t * 0.25
        kinematics[0, :, motion.X] = self.t * np.array([1.0, 1.0])
        kinematics[0, :, motion.SPEED] = 7.0
        return p.Step(decision=0, obs=np.zeros((1, 2, 1), np.float32), state=np.zeros((1, 1), np.float32),
                      mask=np.ones((1, 2, 1), bool), layout=np.zeros((1, 2), np.uint16),
                      present=np.ones((1, 2), bool), reward=np.zeros((1, 2), np.float32), done=np.array([done]),
                      terminated=np.array([done]), final_obs=np.zeros((1, 2, 1), np.float32),
                      final_state=np.zeros((1, 1), np.float32),
                      episode_info=np.array([[[0.0], [1.0]]], np.float32),
                      episode_seed=np.array([seed if done else p.NO_EPISODE_SEED], np.uint32), kinematics=kinematics)

    def set_mode(self, evaluate, seed_base=0, episodes=0, baseline="", **_):
        self.t = 0
        self.seed = 0
        return self._step(False, 0)

    def step(self, actions, goals=None):
        self.t += 1
        if self.t == 5:
            self.t = 0
            self.seed += 1
            return self._step(True, self.seed - 1)
        return self._step(False, 0)


def test_an_evaluation_keeps_the_scored_seats_tracks():
    env = WalkingEnv()
    result, _ = run_evaluation(env, WalkingEnv.SPEC, lambda step: np.zeros((1, 2), np.int32), episodes=3, seed=1,
                               collect_motion=True)
    # Three episodes of two seats; each track is the episode's first five samples (the sample it ended on is the next
    # episode's first).
    assert len(result.motion_tracks) == 6
    for track in result.motion_tracks:
        np.testing.assert_array_equal(track[:, motion.X], np.arange(5) * 1.0)
    merged = EvalResult.merged([result, result])
    assert len(merged.motion_tracks) == 12
    plain, _ = run_evaluation(env, WalkingEnv.SPEC, lambda step: np.zeros((1, 2), np.int32), episodes=3, seed=1)
    assert plain.motion_tracks == []
    feats, contexts, *_ = realism.tracks_features(result.motion_tracks)
    assert np.allclose(feats[:, motion.INDEX["fwd"]], 4.0 / 7.0)   # 1 yd a quarter second against run speed 7


def test_score_motion_writes_the_npz_and_the_columns(tmp_path):
    from animus.config import TrainConfig
    from animus.train import TrainingRun

    rng = np.random.default_rng(2)
    env = WalkingEnv()
    result, _ = run_evaluation(env, WalkingEnv.SPEC, lambda step: np.zeros((1, 2), np.int32), episodes=40, seed=1,
                               collect_motion=True)
    feats, contexts, *_ = realism.tracks_features(result.motion_tracks)
    path = tmp_path / "ref.json"
    path.write_text(json.dumps(reference_of(steps(rng, 2000), np.zeros(2000, np.int16))))
    run = SimpleNamespace(config=TrainConfig(), run_dir=tmp_path, spec=WalkingEnv.SPEC, update=3, env_steps=10,
                          realism_reference=realism.load_reference(path), style=None)
    run.config.style.window = 4
    summary = {}
    TrainingRun.score_motion(run, result, summary)
    assert summary["realism_emd"] > 0.0 and "realism_emd_ground" in summary
    assert summary["realism"]["steps"] == {"ground": len(feats)}
    with np.load(tmp_path / "eval_motion.npz") as data:
        assert data["windows"].shape[1:] == (4, motion.F)
        assert json.loads(str(data["meta"]))["update"] == 3


def test_eval_csv_carries_the_realism_columns_and_moves_an_old_file_aside(tmp_path):
    import csv

    from animus.evaluation import ConvergenceTracker
    from animus.train import EvalLog

    (tmp_path / "eval.csv").write_text(",".join(EvalLog.COLUMNS) + "\n" + ",".join("1" * len(EvalLog.COLUMNS)) + "\n")
    log = EvalLog(tmp_path, None, ["realism_emd", "realism_disc"])
    assert len(list(tmp_path.glob("eval-before-*.csv"))) == 1
    result = EvalResult(policy="learner", returns=np.ones(2), infos=np.zeros((2, 0), np.float32), info_names=())
    log.write(1, 10, result, {"realism_emd": 0.25}, ConvergenceTracker())
    with (tmp_path / "eval.csv").open() as f:
        rows = list(csv.DictReader(f))
    assert rows[0]["realism_emd"] == "0.25" and rows[0]["realism_disc"] == ""
    # Resumed under the same columns, it appends.
    EvalLog(tmp_path, None, ["realism_emd", "realism_disc"]).write(2, 20, result, {}, ConvergenceTracker())
    with (tmp_path / "eval.csv").open() as f:
        assert len(list(csv.DictReader(f))) == 2
