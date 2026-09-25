"""Seeded evaluation against a fake sim, convergence tracking and config overrides."""

import dataclasses
import socket
import threading
from pathlib import Path

import numpy as np
import pytest

from animus import protocol as p
from animus.config import TrainConfig
from animus.env import ForgeEnv
from animus.evaluation import (LIVELOCK_CANCELS, ConvergenceTracker, EvalResult, action_mask_table,
                               casting_weights, run_evaluation, standard_error)
from animus.train import init_from_checkpoint

SPEC = p.Spec(
    version=p.PROTOCOL_VERSION,
    num_envs=2,
    agents_per_env=2,
    obs_dim=3,
    state_dim=3,
    num_actions=2,
    episode_info_dim=2,
    goal_count=0,
    tick_ms=50,
    decision_ticks=1,
    episode_seconds=1,
    scenario="fake",
    layouts=(p.Layout("warrior_dps", 3, 2), p.Layout("mage_dps", 3, 2)),
    episode_info_names=("level", "dps"),
)


def read_exact(conn: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        assert chunk, "client closed early"
        data += chunk
    return data


def blank_step(decision: int) -> p.Step:
    e, a = SPEC.num_envs, SPEC.agents_per_env
    return p.Step(
        decision=decision,
        obs=np.zeros((e, a, SPEC.obs_dim), np.float32),
        state=np.zeros((e, SPEC.state_dim), np.float32),
        mask=np.ones((e, a, SPEC.num_actions), bool),
        layout=np.tile(np.arange(a, dtype=np.uint16), (e, 1)),  # agent 0 warrior, agent 1 mage
        present=np.ones((e, a), bool),
        reward=np.zeros((e, a), np.float32),
        done=np.zeros(e, bool),
        terminated=np.zeros(e, bool),
        final_obs=np.zeros((e, a, SPEC.obs_dim), np.float32),
        final_state=np.zeros((e, SPEC.state_dim), np.float32),
        episode_info=np.zeros((e, a, SPEC.episode_info_dim), np.float32),
        episode_seed=np.full(e, p.NO_EPISODE_SEED, np.uint32),
    )


def test_travel_failures_are_named_by_how_far_they_wandered():
    """lost, wedged and spl derive from arrived, distance_travelled and walk_distance: a trip that did not arrive and
    covered three times its path wandered, one that covered half of it never got going, and spl is success weighted
    by how much further than the path the seat walked."""
    result = EvalResult(
        policy="learner",
        returns=np.zeros(4),
        infos=np.array([[1.0, 100.0, 100.0], [0.0, 400.0, 100.0], [0.0, 30.0, 100.0], [1.0, 200.0, 100.0]],
                       dtype=np.float32),
        info_names=("arrived", "distance_travelled", "walk_distance"),
        layouts=("warrior_dps",) * 4,
        seeds=(0, 1, 2, 3),
    )
    derived = result.derived()
    assert derived["lost"].tolist() == [0.0, 1.0, 0.0, 0.0]
    assert derived["wedged"].tolist() == [0.0, 0.0, 1.0, 0.0]
    assert derived["spl"].tolist() == pytest.approx([1.0, 0.0, 0.0, 0.5])
    summary = result.summary(("arrived",))
    assert summary["lost"] == 0.25 and summary["wedged"] == 0.25
    assert summary["spl"] == pytest.approx(0.375)
    assert result.failed_seeds("arrived") == [1, 2]


def test_eval_action_mask_resolves_names_per_layout():
    """The same action is a different index in every class's catalog, so the mask is resolved by name per layout;
    a name no layout has is a mistake and raises rather than masking nothing."""
    names = {"warrior_dps": ["noop", "a", "follow_route"], "mage_dps": ["noop", "follow_route"]}
    table = action_mask_table(["follow_route"], ["warrior_dps", "mage_dps"], names, 3)
    assert table.tolist() == [[False, False, True], [False, True, False]]
    assert action_mask_table((), ["warrior_dps"], names, 3) is None
    with pytest.raises(ValueError, match="no layout has"):
        action_mask_table(["face_objective"], ["warrior_dps"], names, 3)


def test_run_evaluation_collects_each_seed_once(tmp_path):
    """Fake sim: 3-decision episodes paying reward 1 per decision; seeds go to envs in reset order."""
    path = str(tmp_path / "forge.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    modes = []

    def fake_sim():
        conn, _ = listener.accept()
        with conn:
            read_exact(conn, p.HEADER.size + p.HELLO.size)
            spec_payload = p.encode_spec(SPEC)
            conn.sendall(p.encode_header(p.MsgType.SPEC, len(spec_payload)) + spec_payload)

            decision = 0
            evaluating, episodes, next_seed = False, 0, 0
            env_seed = [p.NO_EPISODE_SEED] * SPEC.num_envs
            env_time = [0] * SPEC.num_envs

            def reset(e):
                nonlocal next_seed
                env_time[e] = 0
                env_seed[e] = p.NO_EPISODE_SEED
                if evaluating and next_seed < episodes:
                    env_seed[e] = next_seed
                    next_seed += 1

            step = blank_step(decision)
            while True:
                payload = p.encode_step(SPEC, step)
                conn.sendall(p.encode_header(p.MsgType.STEP, len(payload)) + payload)
                msg_type, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
                if msg_type == p.MsgType.CLOSE:
                    return
                body = read_exact(conn, length)
                decision += 1
                step = blank_step(decision)
                if msg_type == p.MsgType.MODE:
                    evaluating, seed, episodes, baseline, _ = p.decode_mode(body)
                    modes.append((evaluating, seed, episodes, baseline))
                    next_seed = 0
                    for e in range(SPEC.num_envs):
                        reset(e)
                    continue
                for e in range(SPEC.num_envs):
                    env_time[e] += 1
                    step.reward[e] = (1.0, 2.0)  # the warrior earns 1 per decision, the mage 2
                    if env_time[e] == 3:
                        step.done[e] = True
                        step.episode_seed[e] = env_seed[e]
                        seed = env_seed[e]
                        if seed != p.NO_EPISODE_SEED:
                            step.episode_info[e] = (20 + (seed % 2) * 40, 7.0)
                        reset(e)

    server = threading.Thread(target=fake_sim)
    server.start()
    env = ForgeEnv(path, connect_timeout=5)
    env.reset()

    result, training_step = run_evaluation(
        env, SPEC, lambda step: np.zeros((2, 2), np.int32), episodes=5, seed=77, baseline=""
    )
    env.close()
    server.join(timeout=5)
    listener.close()

    assert modes == [(True, 77, 5, ""), (False, 0, 0, "")]
    assert result.episodes == 10  # 5 seeded episodes x 2 agents
    np.testing.assert_allclose(result.returns, [3.0, 6.0] * 5)
    assert result.score == pytest.approx(4.5)
    assert result.layouts == ("warrior_dps", "mage_dps") * 5
    summary = result.summary(("dps", "missing"))
    assert summary["dps"] == pytest.approx(7.0)
    assert "missing" not in summary
    assert summary["bands"]["1-20"]["episodes"] == 6 and summary["bands"]["41-60"]["episodes"] == 4
    assert summary["layouts"]["mage_dps"]["score"] == pytest.approx(6.0)
    assert not training_step.done.any()


class ScriptedOpponentEnv:
    """In-process fake sim for opponent seats: one env, two agents, 1-decision episodes. Seat 1 is the opponent seat
    (episode info opponent_seat = 1); agent a earns 1 + a per decision. Records every set_mode call."""

    SPEC = p.Spec(
        version=p.PROTOCOL_VERSION, num_envs=1, agents_per_env=2, obs_dim=1, state_dim=1, num_actions=1,
        episode_info_dim=3, goal_count=0, tick_ms=50, decision_ticks=1, episode_seconds=1, scenario="fake",
        layouts=(p.Layout("warrior_dps", 1, 1),), episode_info_names=("level", "arena", "opponent_seat"),
    )

    def __init__(self):
        self.modes = []
        self.next_seed = 0

    def _step(self, done: bool, seed: int) -> p.Step:
        return p.Step(
            decision=0,
            obs=np.zeros((1, 2, 1), np.float32),
            state=np.zeros((1, 1), np.float32),
            mask=np.ones((1, 2, 1), bool),
            layout=np.zeros((1, 2), np.uint16),
            present=np.ones((1, 2), bool),
            reward=np.array([[1.0, 2.0]], np.float32),
            done=np.array([done]),
            terminated=np.array([done]),
            final_obs=np.zeros((1, 2, 1), np.float32),
            final_state=np.zeros((1, 1), np.float32),
            episode_info=np.array([[[10.0, float(seed % 2), 0.0], [10.0, float(seed % 2), 1.0]]], np.float32),
            episode_seed=np.array([seed if done else p.NO_EPISODE_SEED], np.uint32),
        )

    def set_mode(self, evaluate, seed_base=0, episodes=0, baseline="", opponents_only=False):
        self.modes.append((evaluate, baseline, opponents_only))
        self.next_seed = 0
        return self._step(False, p.NO_EPISODE_SEED)

    def step(self, actions, goals=None):
        seed = self.next_seed
        self.next_seed += 1
        return self._step(True, seed)


def test_opponent_seats_are_scripted_and_left_out():
    env = ScriptedOpponentEnv()
    spec = ScriptedOpponentEnv.SPEC
    result, _ = run_evaluation(env, spec, lambda step: np.zeros((1, 2), np.int32), episodes=4, seed=1,
                               opponents="fight", arenas=("duel", "arena_1v1"))
    assert env.modes == [(True, "fight", True), (False, "", False)]
    assert result.episodes == 4 and np.all(result.returns == 1.0)  # only seat 0's rows

    baseline, _ = run_evaluation(env, spec, None, episodes=4, seed=1, baseline="fight", opponents="fight")
    assert env.modes[2] == (True, "fight", False)  # the baseline plays every seat
    assert baseline.episodes == 4

    everyone, _ = run_evaluation(env, spec, lambda step: np.zeros((1, 2), np.int32), episodes=4, seed=1)
    assert everyone.episodes == 8  # without opponents every seat counts

    summary = result.summary(())
    assert set(summary["arenas"]) == {"duel", "arena_1v1"}
    assert summary["arenas"]["duel"]["episodes"] == 2 and summary["arenas"]["arena_1v1"]["episodes"] == 2
    assert baseline.action_counts is None and "actions" not in baseline.episodes_log()[0]
    assert baseline.allowed_counts is None and "allowed" not in baseline.episodes_log()[0]


def test_episode_log_counts_actions_by_name():
    """Each episode counts the learner's actions from its first decision to its done, the no-op left out."""
    env = ScriptedOpponentEnv()
    spec = dataclasses.replace(ScriptedOpponentEnv.SPEC, num_actions=3,
                               layouts=(p.Layout("warrior_dps", 1, 3),))
    result, _ = run_evaluation(env, spec, lambda step: np.array([[2, 0]], np.int32), episodes=2, seed=1,
                               action_names={"warrior_dps": ["noop", "charge_100", "hamstring_1715"]})
    rows = result.episodes_log()
    assert [row["actions"] for row in rows] == [{"hamstring_1715": 1}, {}, {"hamstring_1715": 1}, {}]
    # Every decision's mask allowed both actions, whichever was taken.
    assert all(row["allowed"] == {"charge_100": 1, "hamstring_1715": 1} for row in rows)


def test_summary_by_difficulty_tier():
    infos = np.array([[0.0], [0.0], [2.0]], np.float32)
    result = EvalResult("learner", np.array([1.0, 3.0, 5.0]), infos, ("difficulty",))
    tiers = result.summary(())["difficulties"]
    assert set(tiers) == {"0", "2"} and tiers["0"]["score"] == pytest.approx(2.0) and tiers["2"]["episodes"] == 1
    assert EvalResult("learner", np.array([1.0]), np.zeros((1, 1), np.float32), ("difficulty",)).summary(())[
        "difficulties"] == {}


def test_summary_by_build():
    """A build is named per class, so the summary needs the layout of each row and the class's spec names."""
    infos = np.array([[0.0], [3.0], [3.0]], np.float32)
    result = EvalResult("learner", np.array([1.0, 3.0, 5.0]), infos, ("spec",),
                        layouts=("druid", "druid", "druid"),
                        spec_names={"druid": ["balance", "feral_cat", "feral_bear", "restoration"]})
    summary = result.summary(())
    assert set(summary["specs"]) == {"balance", "restoration"}
    assert summary["specs"]["restoration"]["episodes"] == 2
    assert summary["specs"]["restoration"]["score"] == pytest.approx(4.0)
    assert summary["castings"]["druid_restoration"]["score"] == pytest.approx(4.0)


def test_summary_up_to_each_tier_below_the_top():
    infos = np.array([[0.0], [1.0], [2.0], [2.0]], np.float32)
    result = EvalResult("learner", np.array([1.0, 3.0, 5.0, 7.0]), infos, ("difficulty",),
                        layouts=("mage_dps", "rogue_dps", "mage_dps", "rogue_dps"))
    up_to = result.summary(())["up_to"]
    assert set(up_to) == {"0", "1"}  # up to the top tier is the whole summary
    assert up_to["1"]["episodes"] == 2 and up_to["1"]["score"] == pytest.approx(2.0)
    assert up_to["1"]["layouts"]["rogue_dps"]["episodes"] == 1 and up_to["0"]["layouts"]["rogue_dps"]["episodes"] == 0


def test_arena_summary_needs_several_arenas():
    infos = np.array([[0.0, 0.0], [0.0, 0.0]], np.float32)
    single = EvalResult("learner", np.array([1.0, 2.0]), infos, ("level", "arena"), arenas=("duel",))
    assert single.summary(())["arenas"] == {}


def test_stderr_in_summary():
    result = EvalResult("learner", np.array([1.0, 3.0, 5.0, 7.0]), np.zeros((4, 0), np.float32), ())
    assert result.stderr == pytest.approx(np.std([1, 3, 5, 7], ddof=1) / 2)
    assert result.summary(())["stderr"] == pytest.approx(result.stderr)
    assert EvalResult("learner", np.array([2.0]), np.zeros((1, 0), np.float32), ()).stderr == 0.0


def test_convergence_tracker_margins():
    tracker = ConvergenceTracker(patience=2, min_improvement=0.1, min_improvement_abs=0.01, z=2.0)
    assert tracker.observe(1.0, 10)
    assert not tracker.observe(1.05, 20)  # within 10% of the best
    assert not tracker.converged(20)
    assert not tracker.observe(0.9, 30)
    assert tracker.converged(30)
    assert not tracker.converged(30, min_env_steps=40)
    assert tracker.observe(1.2, 40)
    assert not tracker.converged(40)

    # Noise: 0.5 better than the best is not an improvement when both scores have a standard error of 0.3.
    noisy = ConvergenceTracker(patience=1, min_improvement=0.0, min_improvement_abs=0.0, z=2.0)
    noisy.observe(10.0, 0, stderr=0.3)
    assert not noisy.observe(10.5, 1, stderr=0.3)
    assert noisy.last_margin == pytest.approx(2.0 * np.sqrt(0.18))
    assert noisy.observe(11.0, 2, stderr=0.3)

    restored = ConvergenceTracker(patience=2)
    restored.load_state_dict(tracker.state_dict())
    assert restored.best == 1.2 and restored.best_env_steps == 40 and len(restored.history) == 4


def test_convergence_waits_for_a_flat_trend():
    """No new best for `patience` evaluations is not enough while the recent scores still climb steeply."""
    flat = ConvergenceTracker(patience=2, window=3, min_improvement=0.0, min_improvement_abs=1.0, z=0.0)
    for i, score in enumerate([10.0, 10.6, 10.9]):
        flat.observe(score, i * 100)
    assert flat.evals_since_best == 2
    assert flat.projected_gain() == pytest.approx(0.9)  # 0.45 per evaluation x 2 evaluations
    assert flat.converged(200)  # below the 1.0 margin

    # Recovering from a dip after the best: three evaluations without a new best, but climbing 0.75 per evaluation.
    recovering = ConvergenceTracker(patience=2, window=3, min_improvement=0.0, min_improvement_abs=1.0, z=0.0)
    for i, score in enumerate([12.0, 10.0, 10.5, 11.5]):
        recovering.observe(score, i * 100)
    assert recovering.evals_since_best == 3
    assert recovering.projected_gain() == pytest.approx(1.5)
    assert not recovering.converged(300)


def test_convergence_segment_reset_keeps_best():
    tracker = ConvergenceTracker(patience=1, min_improvement=0.0, min_improvement_abs=0.5)
    tracker.observe(5.0, 0)
    tracker.observe(4.0, 10)
    assert tracker.converged(10)
    tracker.reset_segment(10)
    assert tracker.best == 5.0 and tracker.evals_since_best == 0
    assert not tracker.converged(10)
    assert not tracker.converged(15, min_env_steps=10)  # counted from the restart
    tracker.observe(4.5, 20)
    assert tracker.converged(20, min_env_steps=10)
    assert tracker.projected_gain() is None  # one point in the new segment


def test_config_overrides(tmp_path):
    path = tmp_path / "c.yaml"
    path.write_text("total_env_steps: 100\neval:\n  every_env_steps: 10\n  baseline: greedy\n")
    config = TrainConfig.load(path, ["total_env_steps=5", "eval.episodes=16", "convergence.patience=3",
                                     "eval.report=[dps,died]", "convergence.kl=0.01",
                                     "convergence.hold_share=0.1"])
    assert config.total_env_steps == 5
    assert config.eval.every_env_steps == 10 and config.eval.episodes == 16 and config.eval.baseline == "greedy"
    assert config.eval.report == ("dps", "died")
    assert config.convergence.patience == 3
    assert config.convergence.kl == 0.01 and config.convergence.hold_share == 0.1

    with pytest.raises(ValueError):
        TrainConfig.load(path, ["eval.nope=1"])


def test_config_extends_merges_sections(tmp_path):
    (tmp_path / "base.yaml").write_text("run_name: base\ntotal_env_steps: 100\nmappo:\n  hidden: [8, 8]\n"
                                        "  gamma: 0.9\neval:\n  baseline: greedy\n")
    (tmp_path / "stage.yaml").write_text("extends: base.yaml\nrun_name: stage\nmappo:\n  gamma: 0.99\n")

    config = TrainConfig.load(tmp_path / "stage.yaml")
    assert config.run_name == "stage" and config.total_env_steps == 100
    assert tuple(config.mappo.hidden) == (8, 8) and config.mappo.gamma == 0.99
    assert config.eval.baseline == "greedy"

    (tmp_path / "loop.yaml").write_text("extends: loop.yaml\n")
    with pytest.raises(ValueError):
        TrainConfig.load(tmp_path / "loop.yaml")


def test_config_overlay_merges_over_extends_before_overrides(tmp_path):
    (tmp_path / "base.yaml").write_text("run_name: base\ntotal_env_steps: 100\neval:\n  episodes: 128\n"
                                        "  baseline: fight\nconvergence:\n  window: 6\n")
    (tmp_path / "stage.yaml").write_text("extends: base.yaml\nrun_name: stage\ntotal_env_steps: 200\n")
    (tmp_path / "fast.yaml").write_text("total_env_steps: 10\neval:\n  episodes: 16\n"
                                        "convergence:\n  window: 2\n")

    config = TrainConfig.load(tmp_path / "stage.yaml", ["eval.episodes=8"], [tmp_path / "fast.yaml"])
    assert config.run_name == "stage" and config.total_env_steps == 10
    assert config.eval.episodes == 8 and config.eval.baseline == "fight"
    assert config.convergence.window == 2


def test_fast_overlay_loads_over_every_stage():
    configs = Path(__file__).resolve().parent.parent / "configs"
    for stage in sorted(configs.glob("stage*.yaml")):
        full = TrainConfig.load(stage)
        fast = TrainConfig.load(stage, overlays=[configs / "fast.yaml"])
        assert fast.run_name == full.run_name
        assert fast.total_env_steps < full.total_env_steps
        assert fast.eval.every_env_steps < fast.total_env_steps
        assert fast.convergence.patience > 0
        assert tuple(fast.mappo.hidden) == tuple(full.mappo.hidden)


def test_init_from_falls_back_between_the_two_names(tmp_path):
    """Neither name is required: a run that wrote only one of them still seeds. Which one wins when both exist is
    the seed_from choice, which test_seed_from covers."""
    run = tmp_path / "warrior_dps"
    run.mkdir()
    assert init_from_checkpoint(str(run / "best.pt")) is None
    (run / "latest.pt").write_text("x")
    assert init_from_checkpoint(str(run / "best.pt")) == run / "latest.pt"
    (run / "best.pt").write_text("x")
    assert init_from_checkpoint(str(run / "best.pt"), "best") == run / "best.pt"


def result_with_layouts(returns, layouts, seeds=None):
    returns = np.array(returns, dtype=np.float64)
    return EvalResult(
        policy="learner",
        returns=returns,
        infos=np.arange(len(returns) * 2, dtype=np.float32).reshape(len(returns), 2),
        info_names=("present", "killed"),
        layouts=tuple(layouts),
        seeds=tuple(seeds if seeds is not None else range(len(returns))),
    )


def test_episodes_log_has_one_row_per_episode():
    result = result_with_layouts([1.0, 2.0], ["rogue_dps", "mage_dps"], seeds=[4, 9])
    rows = result.episodes_log(("killed",))
    assert [row["seed"] for row in rows] == [4, 9]
    assert [row["layout"] for row in rows] == ["rogue_dps", "mage_dps"]
    assert [row["return"] for row in rows] == [1.0, 2.0]
    assert rows[0]["killed"] == 1.0 and "present" not in rows[0]  # only the reported columns


def test_episodes_log_writes_every_column_and_the_derived_fields_by_default():
    """Why a class/build loses is in columns no summary reports (level, opponent, form at the end), so the log keeps
    them all, next to whether each episode was a clean kill."""
    result = EvalResult(
        policy="learner",
        returns=np.array([7.0, -3.0, -3.0]),
        infos=np.array([[1, 0, 12, 0], [1, 1, 40, 0], [0, 0, 70, 31]], dtype=np.float32),
        info_names=("killed", "died", "level", "form_at_end"),
        layouts=("mage_dps",) * 3,
        seeds=(0, 1, 2),
    )
    rows = result.episodes_log()
    assert [row["level"] for row in rows] == [12.0, 40.0, 70.0]
    assert rows[2]["form_at_end"] == 31.0
    assert [row["clean_kill"] for row in rows] == [1.0, 0.0, 0.0]  # killed but died is not a clean kill


def test_clean_kill_needs_the_kill_and_no_death():
    infos = np.array([[1, 0], [1, 1], [0, 0], [0, 1]], dtype=np.float32)
    result = EvalResult("learner", np.zeros(4), infos, ("killed", "died"), layouts=("a", "a", "b", "b"))
    summary = result.summary(("killed", "died"))
    assert summary["clean_kill"] == pytest.approx(0.25)
    assert summary["layouts"]["a"]["clean_kill"] == pytest.approx(0.5)
    assert summary["layouts"]["b"]["clean_kill"] == pytest.approx(0.0)
    # Neither mean says it: half killed and half died either way.
    assert summary["killed"] == pytest.approx(0.5) and summary["died"] == pytest.approx(0.5)
    assert "clean_kill" not in EvalResult("learner", np.zeros(1), np.zeros((1, 1), np.float32), ("killed",)).summary(())


def test_summary_groups_by_talent_build():
    names = ("level", "talent_plan")
    infos = np.array([[10, 0], [20, 1], [30, 2], [40, 0]], dtype=np.float32)
    result = EvalResult(policy="learner", returns=np.array([1.0, 2.0, 3.0, 5.0]), infos=infos, info_names=names)

    builds = result.summary(())["builds"]
    assert set(builds) == {"standard", "noisy", "random"}
    assert builds["standard"]["episodes"] == 2 and builds["standard"]["score"] == 3.0
    assert builds["noisy"]["score"] == 2.0 and builds["random"]["score"] == 3.0

    # One build everywhere is no breakdown.
    same = EvalResult(policy="learner", returns=np.array([1.0, 2.0]), info_names=names,
                      infos=np.array([[10, 0], [20, 0]], dtype=np.float32))
    assert same.summary(())["builds"] == {}


def test_casting_weights_favour_the_layouts_below_baseline():
    summary = {"castings": {"rogue_dps": {"score": 6.0}, "mage_dps": {"score": 9.0}, "priest_dps": {"score": 8.0}}}
    baseline = {"castings": {"rogue_dps": {"score": 8.5}, "mage_dps": {"score": 4.0}, "priest_dps": {"score": 8.0}}}
    weights = casting_weights(summary, baseline, strength=1.0, max_ratio=3.0)

    assert weights["rogue_dps"] > weights["priest_dps"] > weights["mage_dps"]
    assert np.mean(list(weights.values())) == pytest.approx(1.0)  # the episode count is unchanged
    assert max(weights.values()) / min(weights.values()) <= 3.0 + 1e-6


def test_casting_weights_follow_the_metric_short_of_the_gate_too():
    """stage8_duel's mage beat the scripted mage's score while killing 68% of the time: the baseline gap alone gave
    it less data than a class and build already killing every time."""
    summary = {"castings": {
        "mage_dps": {"score": 7.0, "clean_kill": 0.68},
        "rogue_dps": {"score": 7.8, "clean_kill": 0.95},
        "warrior_dps": {"score": 7.4, "clean_kill": 0.93},
    }}
    baseline = {"castings": {"mage_dps": {"score": 2.6}, "rogue_dps": {"score": 7.6}, "warrior_dps": {"score": 7.0}}}

    by_score = casting_weights(summary, baseline, 1.0, 4.0)
    assert by_score["mage_dps"] < by_score["rogue_dps"]
    weights = casting_weights(summary, baseline, 1.0, 4.0, metric="clean_kill")
    assert weights["mage_dps"] == max(weights.values())
    assert np.mean(list(weights.values())) == pytest.approx(1.0)
    # A metric some layout does not report leaves the baseline gap to decide.
    del summary["castings"]["rogue_dps"]["clean_kill"]
    assert casting_weights(summary, baseline, 1.0, 4.0, metric="clean_kill") == pytest.approx(by_score)


def test_casting_weights_are_even_without_a_spread_or_a_baseline():
    summary = {"castings": {"a": {"score": 5.0}, "b": {"score": 5.0}}}
    baseline = {"castings": {"a": {"score": 4.0}, "b": {"score": 4.0}}}
    assert casting_weights(summary, baseline, 1.0, 3.0) == {"a": 1.0, "b": 1.0}
    assert casting_weights(summary, baseline, 0.0, 3.0) == {"a": 1.0, "b": 1.0}  # strength 0 = uniform
    assert casting_weights({"castings": {}}, baseline, 1.0, 3.0) == {}


def test_livelocked_counts_episodes_not_cancels():
    """A start-cast / stop-cast loop is a tail, not a shift: stage8_duel's warlock had a median of 4 cancels an
    episode and a maximum of 299, so a mean of casts_cancelled hides it. Counted per episode, per layout."""
    cancels = np.array([0.0, 2.0, float(LIVELOCK_CANCELS), 299.0], dtype=np.float32)
    result = EvalResult(
        policy="learner",
        returns=np.array([7.5, 7.5, 3.3, 0.1]),
        infos=cancels.reshape(4, 1),
        info_names=("casts_cancelled",),
        layouts=("mage_dps", "mage_dps", "warlock_dps", "warlock_dps"),
        seeds=(0, 1, 2, 3),
    )
    summary = result.summary(("casts_cancelled",))
    assert summary["livelocked"] == pytest.approx(0.5)
    assert summary["layouts"]["mage_dps"]["livelocked"] == pytest.approx(0.0)
    assert summary["layouts"]["warlock_dps"]["livelocked"] == pytest.approx(1.0)
    # The mean is unchanged by how the cancels are spread; the share of stuck episodes is the signal.
    assert summary["casts_cancelled"] == pytest.approx(80.25)


def test_livelocked_is_absent_without_cast_counts():
    result = EvalResult("learner", np.array([1.0, 2.0]), np.zeros((2, 0), np.float32), ())
    assert "livelocked" not in result.summary(())


def test_failed_seeds_are_the_episodes_short_on_the_metric():
    infos = np.array([[1, 0], [1, 1], [0, 0], [1, 0], [1, 0]], dtype=np.float32)
    result = EvalResult("learner", np.zeros(5), infos, ("killed", "died"), layouts=("a",) * 5, seeds=(0, 1, 2, 3, 3))
    assert result.failed_seeds("clean_kill") == [1, 2]
    assert result.failed_seeds("killed") == [2]
    assert result.failed_seeds("unknown") == []


def test_a_trace_records_every_decision_of_the_first_seeds():
    """eval.trace_episodes records what the policy did and what goal it said it was pursuing, decision by decision,
    for the episodes with the first seed indexes, and nothing for the others."""
    env = ScriptedOpponentEnv()
    spec = dataclasses.replace(ScriptedOpponentEnv.SPEC, num_actions=3,
                               layouts=(p.Layout("warrior_dps", 1, 3),))

    def choose(step):
        return np.array([[2, 0]], np.int32), np.array([[1, 0]], np.int32)

    result, _ = run_evaluation(env, spec, choose, episodes=4, seed=1, trace_episodes=2,
                               action_names={"warrior_dps": ["noop", "charge_100", "hamstring_1715"]})

    assert result.trace, "the traced seeds should have decisions"
    assert {row["seed"] for row in result.trace} == {0, 1}          # only the first two seeds
    first = result.trace[0]
    assert first["action"] == "hamstring_1715" and first["goal"] == 1 and first["layout"] == "warrior_dps"
    assert {row["agent"] for row in result.trace} == {0, 1}          # every seat of the episode

    untraced, _ = run_evaluation(env, spec, choose, episodes=2, seed=1)
    assert untraced.trace == []


def test_the_noise_is_measured_over_episodes_not_over_the_agents_sharing_one():
    """A row is one agent, and the agents of one episode share its seed, its spawn, its opponents and its
    outcome. Treating them as independent draws divides the spread by the square root of the agent count instead
    of the episode count, which understates the noise by up to sqrt(agents per episode) -- a factor of two in a
    four-seat party, more in a ten-a-side battleground.

    The gates spend this number (animus.gates.noise_allowance), and an understated one passes stages that did not
    actually improve, which is the direction that propagates silently down a seed chain."""
    infos = np.zeros((8, 1), np.float32)
    returns = np.array([1.0, 1.0, 3.0, 3.0, 5.0, 5.0, 7.0, 7.0])
    seeds = (0, 0, 1, 1, 2, 2, 3, 3)          # four episodes of two agents each

    grouped = EvalResult("learner", returns, infos, ("difficulty",), seeds=seeds)
    # The four episodes are 1, 3, 5, 7: the same spread whether each was played by two agents or by one.
    alone = EvalResult("learner", np.array([1.0, 3.0, 5.0, 7.0]), np.zeros((4, 1), np.float32), ("difficulty",),
                       seeds=(0, 1, 2, 3))
    assert grouped.stderr == pytest.approx(alone.stderr)
    assert grouped.stderr == pytest.approx(standard_error(np.array([1.0, 3.0, 5.0, 7.0])))

    # Duplicating an episode's agents must not buy confidence the evaluation did not earn.
    assert grouped.stderr > standard_error(returns)
    assert grouped.summary(())["stderr"] == pytest.approx(grouped.stderr)


def test_unlabelled_rows_still_report_a_standard_error():
    """Callers that never filled in `seeds` (and the baselines recorded before it was kept) fall back to one row
    per draw rather than reporting nothing."""
    returns = np.array([1.0, 3.0, 5.0, 7.0])
    result = EvalResult("learner", returns, np.zeros((4, 1), np.float32), ("difficulty",))
    assert result.stderr == pytest.approx(standard_error(returns))
