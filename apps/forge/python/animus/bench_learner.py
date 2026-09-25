"""Time the learner's whole loop -- rollouts, updates, the exchange -- against a stand-in for the sim.

    python -m animus.bench_learner --config configs/stage8_duel.yaml --spec <run>/spec.json \\
        --layouts <sim's layouts dir> [--sim-ms 5] [--updates 6] [--set KEY=VALUE ...]

A fake sim in its own process speaks the real protocol with a real run's SPEC: random observations, episodes as
long as the stage's, and `--sim-ms` of sleep per decision standing in for the map update. The real TrainingRun
trains against it in this process, so what the sim's `forge bench` calls "learner ms" can be taken apart and changed
without a worldserver: env steps per second over the whole run, and per update the rollout and update seconds the
learner logs. Nothing about learning is measured; the data is noise.
"""

from __future__ import annotations

import argparse
import csv
import json
import multiprocessing
import socket
import tempfile
import time
from pathlib import Path

import numpy as np

from . import protocol as p


def load_spec(path: Path) -> p.Spec:
    raw = json.loads(path.read_text())
    layouts = tuple(p.Layout(item["name"], item["obs_dim"], item["num_actions"]) for item in raw.get("layouts", ()))
    fields = {name: raw[name] for name in ("version", "num_envs", "agents_per_env", "obs_dim", "state_dim",
                                           "num_actions", "episode_info_dim", "goal_count", "tick_ms",
                                           "decision_ticks", "episode_seconds", "scenario")}
    return p.Spec(**fields, layouts=layouts, episode_info_names=tuple(raw.get("episode_info_names", ())))


def _read_exact(conn: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise ConnectionError("learner closed")
        data += chunk
    return bytes(data)


def fake_sim(path: str, spec: p.Spec, sim_ms: float, decisions: int) -> None:
    """Serve one learner for `decisions` decisions, then hang up, as the sim's own bench ends a trial: a STEP, then
    `sim_ms` of sleep after every ACT or MODE."""
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    conn, _ = listener.accept()

    envs, agents = spec.num_envs, spec.agents_per_env
    rng = np.random.default_rng(0)
    layout = rng.integers(0, len(spec.layouts), size=(envs, agents)).astype(np.uint16)
    mask = np.zeros((envs, agents, spec.num_actions), bool)
    obs = np.zeros((envs, agents, spec.obs_dim), np.float32)
    for index, item in enumerate(spec.layouts):
        mask[layout == index, : item.num_actions] = True
    # A few distinct observation frames reused, so encoding a STEP is not the fake sim's own bottleneck.
    frames = [rng.standard_normal(obs.shape, dtype=np.float32) for _ in range(8)]
    for frame in frames:
        for index, item in enumerate(spec.layouts):
            frame[layout == index, item.obs_dim:] = 0.0
    state = rng.standard_normal((envs, spec.state_dim), dtype=np.float32)
    episode = max(1, int(spec.episode_seconds * 1000 / (spec.tick_ms * spec.decision_ticks)))
    clock = rng.integers(0, episode, size=envs)

    with conn:
        _read_exact(conn, p.HEADER.size + p.HELLO.size)
        payload = p.encode_spec(spec)
        conn.sendall(p.encode_header(p.MsgType.SPEC, len(payload)) + payload)

        decision = 0
        while True:
            clock += 1
            done = clock >= episode
            clock[done] = 0
            step = p.Step(
                decision=decision, obs=frames[decision % len(frames)], state=state, mask=mask, layout=layout,
                present=np.ones((envs, agents), bool), reward=rng.standard_normal((envs, agents)).astype(np.float32),
                done=done, terminated=done, final_obs=obs, final_state=np.zeros_like(state),
                episode_info=np.zeros((envs, agents, spec.episode_info_dim), np.float32),
                episode_seed=np.full(envs, p.NO_EPISODE_SEED, np.uint32))
            payload = p.encode_step(spec, step)
            try:
                conn.sendall(p.encode_header(p.MsgType.STEP, len(payload)) + payload)
                while True:
                    msg_type, length = p.HEADER.unpack(_read_exact(conn, p.HEADER.size))
                    if msg_type == p.MsgType.CLOSE:
                        return
                    _read_exact(conn, length)
                    if msg_type in (p.MsgType.ACT, p.MsgType.MODE):
                        break
            except (ConnectionError, OSError):
                return
            decision += 1
            if decision > decisions:
                return
            time.sleep(sim_ms / 1000.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True, help="the stage's training config (YAML)")
    parser.add_argument("--spec", required=True, help="spec.json of a run of that stage")
    parser.add_argument("--layouts", required=True, help="the sim's layouts directory (stage.json)")
    parser.add_argument("--sim-ms", type=float, default=5.0, help="the sim's own time per decision")
    parser.add_argument("--updates", type=int, default=6, help="updates to run; the first is reported apart")
    parser.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="config override")
    args = parser.parse_args()

    from .config import TrainConfig
    from .train import TrainingRun

    spec = load_spec(Path(args.spec))
    steps_per_update = None
    with tempfile.TemporaryDirectory() as scratch:
        path = str(Path(scratch) / "sim.sock")
        config = TrainConfig.load(args.config, [
            *args.set, f"socket={path}", f"runs_dir={scratch}/runs", f"layouts_dir={args.layouts}",
            "run_name=bench", "init_from=[]", "merge_from=[]", "distill.teachers=\"\"",
            "eval.every_env_steps=1000000000000", "eval.at_start=false", "checkpoint_every=1000000",
            "total_env_steps=1000000000000", "convergence.patience=0"])
        steps_per_update = config.rollout_length * spec.num_envs * spec.agents_per_env

        # One decision past the last rollout, so its update has run (or, overlapped, been joined) before the hang-up.
        decisions = (args.updates + 1) * config.rollout_length + 1
        sim = multiprocessing.get_context("spawn").Process(target=fake_sim,
                                                           args=(path, spec, args.sim_ms, decisions), daemon=True)
        sim.start()

        started = time.perf_counter()
        try:
            TrainingRun(config, resume=False).run()
        except ConnectionError:
            pass  # the hang-up that ends the measurement
        wall = time.perf_counter() - started
        sim.join(timeout=10)

        with (Path(scratch) / "runs" / "bench" / "metrics.csv").open() as f:
            rows = list(csv.DictReader(f))

    rows = rows[: args.updates]
    later = rows[1:] or rows
    rollout = np.median([float(row["env_steps_per_sec"]) for row in later])
    update = np.median([float(row["update_seconds"]) for row in later])
    compute = np.median([float(row.get("update_compute_seconds") or "nan") for row in later])
    per_rollout = steps_per_update / rollout + update
    print(f"{spec.scenario}, sim {args.sim_ms:g} ms per decision, {args.updates} updates in {wall:.1f} s: "
          f"after the first, rollouts at {rollout:,.0f} env steps/s, {update:.3f} s waiting on each update "
          f"({compute:.3f} s of update work) -> {steps_per_update / per_rollout:,.0f} env steps/s overall",
          flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
