"""The movement-style reward (animus.style): the seats' motion read exactly as the players' was, the discriminator, the
reward's scale and checkpoint."""

import math
from types import SimpleNamespace

import numpy as np
import pytest
import torch

from animus import protocol as p
from animus.config import StyleConfig, TrainConfig
from animus.human import motion
from animus.mappo.buffer import RolloutBuffer
from animus.style import HumanWindows, StyleReward, reward_of, startup_line
from animus.train import TrainingRun


def walk(n: int, rng: np.random.Generator, jitter: float = 0.0, t0: float = 0.0, mode: int = 0,
         combat: int = 0) -> np.ndarray:
    """A track of `n` samples a decision apart: a steady run forward at run speed, its heading wandering by `jitter`
    radians a decision (a bot's twitch) -- 0 is a clean run."""
    s = np.zeros((n, motion.SAMPLE_DIM), np.float32)
    s[:, motion.T] = t0 + np.arange(n) * motion.DECISION_SECONDS
    yaw = np.cumsum(rng.normal(0.0, jitter, n)) if jitter else np.full(n, 0.5)
    yaw = np.remainder(yaw, 2 * math.pi)
    step = 7.0 * motion.DECISION_SECONDS * (rng.random(n) < 0.5 if jitter else np.ones(n))
    s[:, motion.X] = 100 + np.cumsum(step * np.cos(yaw))
    s[:, motion.Y] = 100 + np.cumsum(step * np.sin(yaw))
    s[:, motion.Z] = 10.0
    s[:, motion.YAW] = yaw
    s[:, motion.MODE] = mode
    s[:, motion.SPEED] = 7.0
    s[:, motion.IN_COMBAT] = combat
    return s


def human_windows(rng: np.random.Generator, tracks: int = 40, length: int = 40) -> HumanWindows:
    windows, contexts = [], []
    for _ in range(tracks):
        samples = walk(length, rng)
        w, c = motion.windows(motion.features(samples), motion.step_contexts(samples))
        windows.append(w)
        contexts.append(c)
    windows, contexts = np.concatenate(windows), np.concatenate(contexts)
    return HumanWindows(windows, contexts, np.ones(len(contexts)))


def config(**overrides) -> StyleConfig:
    values = dict(enabled=True, dataset="x.npz", hidden=(64, 64), minibatches=2, batch=256, lr=1e-3)
    values.update(overrides)
    return StyleConfig(**values)


def step_of(kinematics: np.ndarray, done=None, present=None) -> SimpleNamespace:
    envs, agents = kinematics.shape[:2]
    return SimpleNamespace(kinematics=kinematics, done=np.zeros(envs, bool) if done is None else done,
                           present=np.ones((envs, agents), bool) if present is None else present)


def run_rollouts(style: StyleReward, tracks: np.ndarray, dones: np.ndarray, steps: int, scored: list):
    """Feed `tracks` [N, E, A, S] (sample n is the body after decision n - 1) through rollouts of `steps` decisions;
    `dones` [N - 1, E]. Returns the rewards, rollout by rollout."""
    rewards = []
    step = step_of(tracks[0])
    t_total = len(tracks) - 1
    for start in range(0, t_total, steps):
        style.begin(step)
        for t in range(steps):
            step = step_of(tracks[start + t + 1], dones[start + t])
            style.record(t, slice(0, tracks.shape[1]), step)
        reward, _ = style.finish(dones[start:start + steps], np.ones((steps, *tracks.shape[1:3]), bool), step)
        rewards.append(reward)
    return rewards


def test_the_seats_windows_are_the_players_features_exactly():
    """Every window the discriminator is shown of a seat is motion.windows(motion.features(its episode)) -- across
    rollout boundaries, episodes ending at any decision, and a seat without a body -- so nothing but the motion
    tells the two sides apart."""
    rng = np.random.default_rng(0)
    envs, agents, steps, rollouts = 3, 2, 7, 5
    total = steps * rollouts
    dones = rng.random((total, envs)) < 0.08
    dones[:, 2] = False
    tracks = np.zeros((total + 1, envs, agents, motion.SAMPLE_DIM), np.float32)
    clock = np.zeros(envs)
    for n in range(total + 1):
        for e in range(envs):
            if n > 0 and dones[n - 1, e]:
                clock[e] = 0.0
            for a in range(agents):
                tracks[n, e, a] = walk(1, rng, jitter=0.4, t0=clock[e])[0]
                tracks[n, e, a, motion.X] = rng.normal() * 3
            clock[e] += motion.DECISION_SECONDS
    human = human_windows(rng, 4, 20)
    style = StyleReward(config(), human, envs, agents, steps)
    style.known[:] = True
    seen = {}

    def score(windows, contexts):
        seen["windows"] = windows
        return np.zeros(len(windows), np.float32)

    style.score = score
    expected = []
    for e in range(envs):
        for a in range(agents):
            bounds = [0, *(np.flatnonzero(dones[:, e]) + 1), total + 1]
            for begin, end in zip(bounds[:-1], bounds[1:]):
                track = tracks[begin:end, e, a]
                windows, _ = motion.windows(motion.features(track), motion.step_contexts(track))
                expected += [window.tobytes() for window in windows]

    got = []
    step = step_of(tracks[0])
    for start in range(0, total, steps):
        style.begin(step)
        for t in range(steps):
            step = step_of(tracks[start + t + 1], dones[start + t])
            style.record(t, slice(0, envs), step)
        seen["windows"] = np.zeros((0, style.window, motion.F), np.float32)
        style.finish(dones[start:start + steps], np.ones((steps, envs, agents), bool), step)
        got += [window.tobytes() for window in seen["windows"]]
    assert len(expected) > 20
    assert sorted(got) == sorted(expected)


def test_a_seat_without_a_body_and_a_reset_start_new_tracks():
    rng = np.random.default_rng(1)
    style = StyleReward(config(), human_windows(rng, 4, 20), 1, 1, 12)
    track = walk(13, rng)[:, None, None, :]
    present = np.ones((13, 1, 1), bool)
    present[5] = False
    style.begin(step_of(track[0]))
    for t in range(12):
        style.record(t, slice(0, 1), step_of(track[t + 1], present=present[t + 1]))
    reward, _ = style.finish(np.zeros((12, 1), bool), np.ones((12, 1, 1), bool), "end")
    # Four steps run up to the gap; the track starts again at sample 6, and eight more do not fit before the end.
    assert not reward.any()
    whole = StyleReward(config(), style.human, 1, 1, 12)
    whole.begin(step_of(track[0]))
    for t in range(12):
        whole.record(t, slice(0, 1), step_of(track[t + 1]))
    reward, _ = whole.finish(np.zeros((12, 1), bool), np.ones((12, 1, 1), bool), "end")
    assert not reward[:7].any() and (reward[7:] > 0).all()   # the first full window ends at decision 7
    # Another STEP than the one the last rollout ended on (an evaluation came between) forgets every seat.
    style.begin(step_of(track[12]))
    assert not style.prev_linked.any() and not style.carry_run.any()


def test_the_discriminator_tells_a_twitch_from_a_run_and_pays_the_run():
    rng = np.random.default_rng(2)
    torch.manual_seed(0)
    envs, agents, steps = 8, 2, 32
    style = StyleReward(config(minibatches=4), human_windows(rng), envs, agents, steps, seed=3)

    def rollout(jitter: float) -> np.ndarray:
        tracks = np.stack([np.stack([walk(steps + 1, rng, jitter=jitter) for _ in range(agents)], axis=1)
                           for _ in range(envs)], axis=1)
        style.tail = None
        return run_rollouts(style, tracks, np.zeros((steps, envs), bool), steps, [])[0]

    for _ in range(40):
        twitch = rollout(0.8)
    calm = rollout(0.0)
    twitch = rollout(0.8)
    full = np.zeros((steps, envs, agents), bool)
    full[style.window - 1:] = True
    assert calm[full].mean() > 0.8 > 0.3 > twitch[full].mean()
    assert not calm[:style.window - 1].any()   # no full window yet, no reward


def test_reward_form():
    assert reward_of(np.array([1.0]))[0] == 1.0
    assert reward_of(np.array([-1.0]))[0] == 0.0
    assert reward_of(np.array([0.0]))[0] == pytest.approx(0.75)
    assert reward_of(np.array([3.0]))[0] == 0.0


def test_human_windows_are_drawn_by_weight_per_context():
    windows = np.zeros((6, 8, motion.F), np.float32)
    windows[:, 0, 0] = np.arange(6)
    human = HumanWindows(windows, np.array([0, 0, 0, 4, 4, 9]), np.array([1.0, 0.0, 3.0, 1.0, 1.0, 0.0]))
    assert human.context_set == [0, 4]   # context 9's one window has no weight
    drawn = human.sample(np.array([0] * 4000 + [4] * 10), np.random.default_rng(0))
    first = drawn[:4000, 0, 0]
    assert set(first) == {0.0, 2.0} and (first == 2.0).mean() == pytest.approx(0.75, abs=0.03)
    assert set(drawn[4000:, 0, 0]) <= {3.0, 4.0}


def test_human_windows_load_checks_the_window(tmp_path):
    path = tmp_path / "human_motion_windows.npz"
    np.savez(path, windows=np.zeros((3, 6, motion.F), np.float32), context=np.zeros(3, np.int16),
             weight=np.ones(3, np.float32), meta=np.asarray('{"format": 1}'))
    assert HumanWindows.load(path, 6).meta == {"format": 1}
    with pytest.raises(ValueError, match="steps"):
        HumanWindows.load(path, 8)


def test_the_reward_follows_the_cost_ladder_and_leaves_the_score_alone():
    """reward += coef * scale * r before the advantages: scale is the cost ladder's in force (style.ladder), else 1.
    The outcome score is the sim's episode info, which nothing here touches."""
    steps, envs, agents = 4, 2, 1
    buffer = RolloutBuffer(steps, envs, agents, 1, 1, 1)
    buffer.rewards[:] = 0.5
    r = np.full((steps, envs, agents), 0.8, np.float32)
    style = SimpleNamespace(finish=lambda dones, valid, step: (r, {"style_reward": 0.8}))
    info = np.array([[[3.0]], [[4.0]]], np.float32)
    last = SimpleNamespace(episode_info=info.copy())
    run = SimpleNamespace(style=style, step=last, cost_scale_now=0.25, config=TrainConfig())
    run.config.style.coef = 0.1
    run.style_scale = lambda: TrainingRun.style_scale(run)
    TrainingRun.add_style(run, buffer)
    np.testing.assert_allclose(buffer.rewards, 0.5 + 0.1 * 0.25 * 0.8)
    assert run.style_stats["style_scale"] == 0.25
    np.testing.assert_array_equal(last.episode_info, info)

    run.config.style.ladder = False
    buffer.rewards[:] = 0.5
    TrainingRun.add_style(run, buffer)
    np.testing.assert_allclose(buffer.rewards, 0.5 + 0.1 * 1.0 * 0.8)
    run.style = None
    TrainingRun.add_style(run, buffer)   # off: nothing added
    np.testing.assert_allclose(buffer.rewards, 0.5 + 0.1 * 1.0 * 0.8)


def test_checkpoint_round_trip(tmp_path):
    rng = np.random.default_rng(4)
    human = human_windows(rng, 4, 20)
    style = StyleReward(config(), human, 1, 1, 4)
    windows = human.windows[:16]
    contexts = human.contexts[:16]
    with torch.no_grad():
        for parameter in style.disc.parameters():
            parameter.add_(0.1)
    style.optimizer.step()
    path = tmp_path / "latest.pt"
    torch.save({"style": style.state_dict()}, path)

    fresh = StyleReward(config(), human, 1, 1, 4)
    assert not np.allclose(fresh.score(windows, contexts), style.score(windows, contexts))
    assert fresh.load_state_dict(torch.load(path, weights_only=False)["style"])
    np.testing.assert_array_equal(fresh.score(windows, contexts), style.score(windows, contexts))
    # A checkpoint without one, or of another shape, starts fresh.
    assert not StyleReward(config(), human, 1, 1, 4).load_state_dict(None)
    assert not StyleReward(config(hidden=(32,)), human, 1, 1, 4).load_state_dict(style.state_dict())


def test_config_validation_and_default_off():
    assert not TrainConfig().style.enabled and TrainConfig().style.window == motion.WINDOW
    with pytest.raises(ValueError, match="dataset"):
        StyleConfig(enabled=True)
    with pytest.raises(ValueError, match="hidden"):
        StyleConfig(hidden=())
    with pytest.raises(ValueError, match="coef"):
        StyleConfig(coef=-1.0)
    loaded = TrainConfig.load("configs/stage1_move.yaml")
    assert not loaded.style.enabled
    assert not TrainConfig.load("configs/stage2_travel.yaml").style.enabled


def test_startup_line():
    human = human_windows(np.random.default_rng(5), 2, 12)
    assert startup_line(StyleConfig(), None, True) == "style reward off"
    line = startup_line(config(), human, True)
    assert "on the cost ladder" in line and "ground" in line
    assert "in full" in startup_line(config(), human, False)


def test_a_protocol_step_carries_what_style_reads():
    spec = p.Spec(version=p.PROTOCOL_VERSION, num_envs=2, agents_per_env=1, obs_dim=1, state_dim=1, num_actions=1,
                  episode_info_dim=1, goal_count=0, tick_ms=50, decision_ticks=5, episode_seconds=10,
                  scenario="fake", kinematics_dim=motion.SAMPLE_DIM)
    kinematics = np.arange(20, dtype=np.float32).reshape(2, 1, 10)
    step = p.Step(decision=0, obs=np.zeros((2, 1, 1), np.float32), state=np.zeros((2, 1), np.float32),
                  mask=np.ones((2, 1, 1), bool), layout=np.zeros((2, 1), np.uint16), present=np.ones((2, 1), bool),
                  reward=np.zeros((2, 1), np.float32), done=np.zeros(2, bool), terminated=np.zeros(2, bool),
                  final_obs=np.zeros((2, 1, 1), np.float32), final_state=np.zeros((2, 1), np.float32),
                  episode_info=np.zeros((2, 1, 1), np.float32), episode_seed=np.zeros(2, np.uint32),
                  kinematics=kinematics)
    decoded = p.decode_step(spec, p.encode_step(spec, step))
    style = StyleReward(config(), human_windows(np.random.default_rng(6), 2, 12), 2, 1, 1)
    style.begin(decoded)
    np.testing.assert_array_equal(style.samples[0], kinematics)
