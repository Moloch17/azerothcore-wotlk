"""A whole learner run (animus.train.TrainingRun) against a fake sim: updates, evaluations, checkpoints, finish."""

import csv
import dataclasses
import json
import socket
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("torch")

from sim_threads import accept, joined, sim_thread  # noqa: E402
from animus import protocol as p  # noqa: E402
from animus.config import TrainConfig  # noqa: E402
from animus.env import ForgeEnv  # noqa: E402
from animus.train import TrainingRun  # noqa: E402

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
    layouts=(p.Layout("warrior_dps", 3, 2), p.Layout("mage_dps", 2, 2)),
    episode_info_names=("present", "dps"),
)

EPISODE_DECISIONS = 3


def read_exact(conn: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise ConnectionError("client closed")
        data += chunk
    return data


def fake_sim(listener: socket.socket, modes: list, replays: list, spec: p.Spec = SPEC,
             hang_up_after: int | None = None, marks: list | None = None) -> None:
    """3-decision episodes paying 1 per decision; env 1's second seat is empty (present 0, only the no-op).
    `marks` gets the decision each MODE came on.

    With spec.env_groups 2 it runs as the half-batch sim does: every group's STEP after a reset, then for each reply
    only that group's envs move on and only its STEP goes out."""
    conn = accept(listener)
    with conn:
        read_exact(conn, p.HEADER.size + p.HELLO.size)
        payload = p.encode_spec(spec)
        conn.sendall(p.encode_header(p.MsgType.SPEC, len(payload)) + payload)

        e_count, a_count = spec.num_envs, spec.agents_per_env
        groups = spec.env_groups_ranges()
        evaluating, episodes, next_seed, last_seed = False, 0, 0, 0
        env_seed = [p.NO_EPISODE_SEED] * e_count
        env_time = [0] * e_count
        decision = 0

        def reset(e):
            nonlocal next_seed
            env_time[e] = 0
            env_seed[e] = p.NO_EPISODE_SEED
            if evaluating and next_seed < last_seed:
                env_seed[e], next_seed = next_seed, next_seed + 1

        def blank():
            present = np.ones((e_count, a_count), bool)
            present[1, 1] = False
            mask = np.ones((e_count, a_count, spec.num_actions), bool)
            mask[1, 1, 1:] = False
            return p.Step(
                decision=decision,
                obs=np.random.default_rng(decision).random((e_count, a_count, spec.obs_dim), dtype=np.float32),
                state=np.zeros((e_count, spec.state_dim), np.float32),
                mask=mask,
                layout=np.tile(np.arange(a_count, dtype=np.uint16), (e_count, 1)),
                present=present,
                reward=np.zeros((e_count, a_count), np.float32),
                done=np.zeros(e_count, bool),
                terminated=np.zeros(e_count, bool),
                final_obs=np.zeros((e_count, a_count, spec.obs_dim), np.float32),
                final_state=np.zeros((e_count, spec.state_dim), np.float32),
                episode_info=np.zeros((e_count, a_count, spec.episode_info_dim), np.float32),
                episode_seed=np.full(e_count, p.NO_EPISODE_SEED, np.uint32),
                kinematics=bodies(range(e_count)),
            )

        def bodies(envs, kinematics=None):
            """Protocol 20 kinematics, when the spec has them: every seat runs along x at run speed, its clock the
            episode's."""
            if kinematics is None:
                kinematics = np.zeros((e_count, a_count, spec.kinematics_dim), np.float32)
            if spec.kinematics_dim:
                for e in envs:
                    kinematics[e, :, 0] = env_time[e] * 0.25
                    kinematics[e, :, 1] = env_time[e] * 1.75
                    kinematics[e, :, 8] = 7.0
            return kinematics

        def send(step, begin, count):
            payload = p.encode_step(spec, p.rows_of(step, begin, count))
            conn.sendall(p.encode_header(p.MsgType.STEP, len(payload)) + payload)

        step = blank()
        for begin, count in groups:
            send(step, begin, count)
        turn = 0
        while True:
            try:
                # WEIGHTS and REPLAY are applied without an answer, as the sim does: read on to the ACT or MODE.
                while True:
                    msg_type, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
                    if msg_type == p.MsgType.CLOSE:
                        return
                    body = read_exact(conn, length)
                    if msg_type == p.MsgType.REPLAY:
                        replays.append(p.decode_replay(body))
                    elif msg_type not in (p.MsgType.WEIGHTS, p.MsgType.PROGRESS):
                        break
                decision += 1
                if hang_up_after is not None and decision > hang_up_after:
                    return  # the machine went down: the connection closes mid-run
                step = blank()
                if msg_type == p.MsgType.MODE:
                    evaluating, _, episodes, baseline, _ = p.decode_mode(body)
                    modes.append((evaluating, episodes, baseline))
                    if marks is not None:
                        marks.append(decision)
                    next_seed = p.decode_mode_first_seed(body)
                    last_seed = next_seed + episodes
                    for e in range(e_count):
                        reset(e)
                    for begin, count in groups:
                        send(step, begin, count)
                    turn = 0
                    continue
                begin, count = groups[turn]
                assert p.ACT_HEADER.unpack_from(body) == (begin, count), "an ACT for the group whose turn it is not"
                for e in range(begin, begin + count):
                    env_time[e] += 1
                    step.reward[e] = np.where(step.present[e], 1.0, 0.0)
                    if env_time[e] == EPISODE_DECISIONS:
                        step.done[e] = True
                        step.episode_seed[e] = env_seed[e]
                        step.episode_info[e, :, 0] = step.present[e]
                        reset(e)
                bodies(range(begin, begin + count), step.kinematics)
                send(step, begin, count)
                turn = (turn + 1) % len(groups)
            except (ConnectionError, OSError):
                return

def test_training_run_trains_evaluates_and_finishes(tmp_path):
    path = str(tmp_path / "forge.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    modes, replays = [], []
    server = sim_thread(fake_sim, listener, modes, replays)
    server.start()

    steps_per_update = 4 * SPEC.num_envs * SPEC.agents_per_env
    config = TrainConfig.load(Path(__file__).parent / "fixtures" / "test_stage.yaml", [
        f"socket={path}", f"runs_dir={tmp_path / 'runs'}", f"layouts_dir={tmp_path / 'layouts'}", "run_name=fake",
        "rollout_length=4", f"total_env_steps={2 * steps_per_update}", "checkpoint_every=1", "init_from=''",
        "train_device=cpu", "mappo.hidden=[8, 8]", "mappo.epochs=1", "mappo.minibatches=1",
        f"eval.every_env_steps={steps_per_update}", "eval.episodes=2", "eval.baseline=''", "eval.sampled_every=3",
        "convergence.patience=0",
    ])

    exit_code = TrainingRun(config, resume=False).run()
    joined(server, 10)
    listener.close()

    run_dir = tmp_path / "runs" / "fake"
    assert exit_code == 0
    finished = json.loads((run_dir / "finished.json").read_text())
    assert (finished["advanced"], finished["update"], finished["env_steps"]) == (True, 2, 2 * steps_per_update)
    assert finished["reason"] == "budget" and set(finished["layouts"]) == {"warrior_dps", "mage_dps"}

    with (run_dir / "metrics.csv").open() as f:
        rows = list(csv.DictReader(f))
    assert [int(row["update"]) for row in rows] == [1, 2]
    # Empty seats earn nothing and are not samples: every present seat earns 1 per decision.
    assert all(float(row["reward_per_decision"]) == pytest.approx(1.0) for row in rows)

    # Every scored episode is logged, so a class/build's failures can be read back seed by seed.
    episodes = [json.loads(line) for line in (run_dir / "eval_episodes.jsonl").read_text().splitlines()]
    assert {row["layout"] for row in episodes} <= {"warrior_dps", "mage_dps"}
    assert all("return" in row and row["seed"] >= 0 for row in episodes)

    with (run_dir / "eval.csv").open() as f:
        evals = list(csv.DictReader(f))
    # The third evaluation also plays sampled actions on the same seeds (eval.sampled_every 3, as the export stage).
    assert [int(row["env_steps"]) for row in evals] == [0, steps_per_update, 2 * steps_per_update, 2 * steps_per_update]
    assert [row["policy"] for row in evals] == ["learner", "learner", "learner", "learner_sampled"]
    assert modes.count((True, 2, "")) == 4
    # After each training evaluation the lost seeds go to the sim (the test stage replays clean_kill losses). The fake
    # episodes have no killed or died columns, so none can be told lost: an empty replay each time.
    assert len(replays) == 3 and all(len(seeds) == 0 for _, _, seeds in replays)

    assert (run_dir / "latest.pt").exists() and (run_dir / "best.pt").exists()
    assert json.loads((run_dir / "progress.json").read_text())["phase"] == "finished"


def test_a_run_with_the_style_reward_and_the_realism_score(tmp_path):
    """style.enabled with a human dataset and a reference: the reward is logged, the realism columns are in eval.csv,
    the evaluation's motion is in eval_motion.npz, and the discriminator is in the checkpoint."""
    import torch

    from animus.human import motion

    spec = dataclasses.replace(SPEC, kinematics_dim=motion.SAMPLE_DIM)
    track = np.zeros((20, motion.SAMPLE_DIM), np.float32)
    track[:, 0] = np.arange(20) * 0.25
    track[:, 1] = np.arange(20) * 1.75
    track[:, 8] = 7.0
    feats, contexts = motion.features(track), motion.step_contexts(track)
    windows, context = motion.windows(feats, contexts, 2)
    human = tmp_path / "human_motion_windows.npz"
    np.savez(human, windows=windows, context=context, weight=np.ones(len(context), np.float32),
             meta=np.asarray("{}"))
    hists = motion.histograms(feats, contexts)
    reference = tmp_path / "human_reference.json"
    reference.write_text(json.dumps({"format": 1, "motion": {
        str(c): {"name": motion.context_name(c), "steps": 19, "hist": {k: v.tolist() for k, v in h.items()}}
        for c, h in hists.items()}}))

    path = str(tmp_path / "forge.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    server = sim_thread(fake_sim, listener, [], [], spec)
    server.start()
    steps_per_update = 4 * spec.num_envs * spec.agents_per_env
    config = TrainConfig.load(Path(__file__).parent / "fixtures" / "test_stage.yaml", [
        f"socket={path}", f"runs_dir={tmp_path / 'runs'}", f"layouts_dir={tmp_path / 'layouts'}", "run_name=fake",
        "rollout_length=4", f"total_env_steps={2 * steps_per_update}", "checkpoint_every=1", "init_from=''",
        "train_device=cpu", "mappo.hidden=[8, 8]", "mappo.epochs=1", "mappo.minibatches=1",
        f"eval.every_env_steps={steps_per_update}", "eval.episodes=2", "eval.baseline=''", "eval.sampled_every=0",
        "convergence.patience=0", "style.enabled=true", f"style.dataset={human}", f"style.reference={reference}",
        "style.window=2", "style.hidden=[8]", "style.batch=8", "style.minibatches=2",
    ])
    assert TrainingRun(config, resume=False).run() == 0
    joined(server, 10)
    listener.close()

    run_dir = tmp_path / "runs" / "fake"
    with (run_dir / "metrics.csv").open() as f:
        rows = list(csv.DictReader(f))
    assert all(0.0 < float(row["style_reward"]) <= 1.0 for row in rows)
    assert all(row["style_disc_human"] != "" and row["style_reward_ground"] != "" for row in rows)
    # The style term is in the rollout's reward on top of the sim's 1 a decision; the cost ladder is off, so in full.
    assert all(float(row["reward_per_decision"]) > 1.0 and float(row["style_scale"]) == 1.0 for row in rows)
    with (run_dir / "eval.csv").open() as f:
        evals = list(csv.DictReader(f))
    assert all(row["realism_emd"] != "" and row["realism_disc"] != "" for row in evals)
    assert float(evals[0]["realism_emd"]) == pytest.approx(0.0, abs=1e-9)   # the players' own motion
    with np.load(run_dir / "eval_motion.npz") as data:
        assert data["windows"].shape[1:] == (2, motion.F) and len(data["context"]) == len(data["weight"])
    checkpoint = torch.load(run_dir / "latest.pt", weights_only=False)
    assert set(checkpoint["style"]) >= {"disc", "optimizer"}


@pytest.mark.slow
def test_overlapped_updates_run_behind_the_next_rollout(tmp_path, monkeypatch):
    """With overlap_updates, each rollout hands its update to the worker and returns without joining it; the join
    comes after the next rollout. It used to join every update where it was submitted, so nothing overlapped."""
    path = str(tmp_path / "forge.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    server = sim_thread(fake_sim, listener, [], [])
    server.start()

    steps_per_update = 4 * SPEC.num_envs * SPEC.agents_per_env
    config = TrainConfig.load(Path(__file__).parent / "fixtures" / "test_stage.yaml", [
        f"socket={path}", f"runs_dir={tmp_path / 'runs'}", f"layouts_dir={tmp_path / 'layouts'}", "run_name=fake",
        "rollout_length=4", f"total_env_steps={3 * steps_per_update}", "checkpoint_every=1000", "init_from=''",
        "train_device=cpu", "mappo.hidden=[8, 8]", "mappo.epochs=1", "mappo.minibatches=1",
        "eval.every_env_steps=1000000", "eval.at_start=false", "eval.episodes=2", "eval.baseline=''",
        "convergence.patience=0", "overlap_updates=true",
    ])

    pending_after_rollout = []
    rollout = TrainingRun.rollout

    def watched(self):
        result = rollout(self)
        pending_after_rollout.append(self.pending_update is not None)
        return result

    monkeypatch.setattr(TrainingRun, "rollout", watched)
    assert TrainingRun(config, resume=False).run() == 0
    joined(server, 10)
    listener.close()

    assert pending_after_rollout == [True, True, True]
    with (tmp_path / "runs" / "fake" / "metrics.csv").open() as f:
        rows = list(csv.DictReader(f))
    # One row per update; the first has no update of its own to report yet, the later ones the update before.
    assert [int(row["update"]) for row in rows] == [1, 2, 3]
    assert rows[0]["policy_loss"] == "" and rows[1]["policy_loss"] != ""


def test_half_batch_training_answers_each_half_as_it_comes(tmp_path, monkeypatch):
    """Against a half-batch sim the rollout answers each half's STEP on its own (pipelined), evaluations go through
    the whole-decision facade, and the run learns what the lock-step run does: every present seat earns 1."""
    spec = dataclasses.replace(SPEC, num_envs=4, env_groups=2)
    path = str(tmp_path / "forge.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    modes, replays = [], []
    server = sim_thread(fake_sim, listener, modes, replays, spec)
    server.start()

    steps_per_update = 4 * spec.num_envs * spec.agents_per_env
    config = TrainConfig.load(Path(__file__).parent / "fixtures" / "test_stage.yaml", [
        f"socket={path}", f"runs_dir={tmp_path / 'runs'}", f"layouts_dir={tmp_path / 'layouts'}", "run_name=fake",
        "rollout_length=4", f"total_env_steps={3 * steps_per_update}", "checkpoint_every=1", "init_from=''",
        "train_device=cpu", "mappo.hidden=[8, 8]", "mappo.epochs=1", "mappo.minibatches=1",
        f"eval.every_env_steps={steps_per_update}", "eval.episodes=2", "eval.baseline=''", "eval.sampled_every=3",
        "convergence.patience=0",
    ])
    sent_one_half, events = [], []
    send_act, receive_step = ForgeEnv.send_act, ForgeEnv.receive_step

    def watched_send(self, env_begin, actions, goals=None):
        sent_one_half.append(actions.shape[0] == spec.num_envs // 2)
        events.append(("act", env_begin))
        return send_act(self, env_begin, actions, goals)

    def watched_receive(self):
        step = receive_step(self)
        events.append(("step", step.env_begin))
        return step

    monkeypatch.setattr(ForgeEnv, "send_act", watched_send)
    monkeypatch.setattr(ForgeEnv, "receive_step", watched_receive)
    assert TrainingRun(config, resume=False).run() == 0
    joined(server, 10)
    listener.close()

    assert all(sent_one_half) and sent_one_half
    # Pipelined: the first half is answered as soon as its STEP is in, before the second half's STEP is read.
    half = spec.num_envs // 2
    assert ("step", 0) in events
    pipelined = [events[i:i + 3] for i in range(len(events) - 2)]
    assert [("step", 0), ("act", 0), ("step", half)] in pipelined
    with (tmp_path / "runs" / "fake" / "metrics.csv").open() as f:
        rows = list(csv.DictReader(f))
    assert [int(row["update"]) for row in rows] == [1, 2, 3]
    assert all(float(row["reward_per_decision"]) == pytest.approx(1.0) for row in rows)
    assert modes.count((True, 2, "")) >= 3


@pytest.mark.slow
def test_a_cluster_trains_on_every_sim_and_shares_the_evaluation_seeds(tmp_path):
    """The host's learner trains on its own sim and a worker's as one pool (cluster_sims): a half-batch sim and a
    lock-step one here. Every present seat earns 1 as with one sim, and each evaluation's seeds are shared out so
    that every seed is played exactly once."""
    host_spec = dataclasses.replace(SPEC, num_envs=4, env_groups=2)
    listeners, servers, modes = [], [], []
    for name, spec in (("host", host_spec), ("worker", SPEC)):
        path = str(tmp_path / f"{name}.sock")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(path)
        listener.listen(1)
        sim_modes = []
        servers.append(sim_thread(fake_sim, listener, sim_modes, [], spec))
        servers[-1].start()
        listeners.append(listener)
        modes.append(sim_modes)

    envs = host_spec.num_envs + SPEC.num_envs
    steps_per_update = 4 * envs * SPEC.agents_per_env
    config = TrainConfig.load(Path(__file__).parent / "fixtures" / "test_stage.yaml", [
        f"socket={tmp_path / 'host.sock'}", f"cluster_sims=['{tmp_path / 'worker.sock'}']",
        f"runs_dir={tmp_path / 'runs'}", f"layouts_dir={tmp_path / 'layouts'}", "run_name=fake",
        "rollout_length=4", f"total_env_steps={2 * steps_per_update}", "checkpoint_every=1", "init_from=''",
        "train_device=cpu", "rollout_device=cpu", "mappo.hidden=[8, 8]", "mappo.epochs=1", "mappo.minibatches=1",
        f"eval.every_env_steps={steps_per_update}", "eval.episodes=6", "eval.baseline=''", "eval.sampled_every=3",
        "convergence.patience=0",
    ])
    assert TrainingRun(config, resume=False).run() == 0
    for server, listener in zip(servers, listeners):
        joined(server, 10)
        listener.close()

    run_dir = tmp_path / "runs" / "fake"
    with (run_dir / "metrics.csv").open() as f:
        rows = list(csv.DictReader(f))
    assert [int(row["update"]) for row in rows] == [1, 2]
    assert all(float(row["reward_per_decision"]) == pytest.approx(1.0) for row in rows)
    # Both sims took part in every evaluation, the host with the larger share (4 envs against 2).
    assert modes[0].count((True, 4, "")) >= 1 and modes[1].count((True, 2, "")) >= 1
    # One row per seat: within an evaluation no seat of a seed is scored twice, and every seed is played.
    episodes = [json.loads(line) for line in (run_dir / "eval_episodes.jsonl").read_text().splitlines()]
    played = [(row["env_steps"], row["policy"], row["seed"], row["layout"]) for row in episodes]
    assert len(played) == len(set(played)), "a seed was played twice"
    for evaluation in {(steps, policy) for steps, policy, _, _ in played}:
        assert {seed for steps, policy, seed, _ in played if (steps, policy) == evaluation} == set(range(6))


@pytest.mark.slow
def test_a_worker_that_drops_out_does_not_stop_training_and_rejoins(tmp_path, monkeypatch, capsys):
    """A cluster worker's sim hangs up mid-run: its envs sit out and training goes on on the host's; a worker back on
    the same address rejoins between rollouts. Every present seat still earns 1 -- the rows that sat out are no
    samples."""
    from animus.env import ClusterEnv

    monkeypatch.setattr(ClusterEnv, "REJOIN_INTERVAL", 0.0)
    host_path, worker_path = str(tmp_path / "host.sock"), str(tmp_path / "worker.sock")
    listeners, servers = [], []
    for path, hang_up in ((host_path, None), (worker_path, 6)):
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(path)
        listener.listen(1)
        listeners.append(listener)
        servers.append(sim_thread(fake_sim, listener, [], [], SPEC, hang_up))
        servers[-1].start()

    def worker_comes_back():
        joined(servers[1], 60)
        comeback = sim_thread(fake_sim, listeners[1], [], [], SPEC)
        comeback.start()
        joined(comeback, 60)

    back = sim_thread(worker_comes_back)
    back.start()

    envs = 2 * SPEC.num_envs
    steps_per_update = 4 * envs * SPEC.agents_per_env
    config = TrainConfig.load(Path(__file__).parent / "fixtures" / "test_stage.yaml", [
        f"socket={host_path}", f"cluster_sims=['{worker_path}']", f"runs_dir={tmp_path / 'runs'}",
        f"layouts_dir={tmp_path / 'layouts'}", "run_name=fake", "rollout_length=4",
        f"total_env_steps={5 * steps_per_update}", "checkpoint_every=100", "init_from=''", "train_device=cpu",
        "rollout_device=cpu", "mappo.hidden=[8, 8]", "mappo.epochs=1", "mappo.minibatches=1",
        "eval.every_env_steps=1000000", "eval.at_start=false", "eval.episodes=2", "eval.baseline=''",
        "convergence.patience=0",
    ])
    assert TrainingRun(config, resume=False).run() == 0
    joined(back, 10)
    joined(servers[0], 10)
    for listener in listeners:
        listener.close()

    printed = capsys.readouterr().out
    assert "lost the worker sim" in printed and "is back" in printed
    with (tmp_path / "runs" / "fake" / "metrics.csv").open() as f:
        rows = list(csv.DictReader(f))
    assert [int(row["update"]) for row in rows] == [1, 2, 3, 4, 5]
    assert all(float(row["reward_per_decision"]) == pytest.approx(1.0) for row in rows)
