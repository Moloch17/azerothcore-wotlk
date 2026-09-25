"""Time one PPO update on the real networks, without a sim.

    python -m animus.bench_update --config configs/stage8_duel.yaml --spec <run>/spec.json [--repeat 5] [--profile]

Builds the stage's MappoTrainer from a run's spec.json, fills one rollout with random observations acted on by the
rollout networks (so log_probs, values and memories are the policy's own), and runs `trainer.update` on it `repeat`
times. The first update is reported apart: it pays for allocator growth and kernel selection. `--profile` adds a
torch.profiler table of where one update's time went, which is how a launch-bound update tells itself apart from a
compute-bound one.

The sim blocks for the whole update when overlap_updates is off, so update seconds divided by rollout_length is what
each decision pays for it. This measures that number, and nothing about learning: the data is random.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np
import torch

from .config import TrainConfig
from .mappo.buffer import RolloutBuffer
from .mappo.trainer import MappoTrainer, per_decision
from .protocol import Layout, Spec


def load_spec(path: Path) -> Spec:
    raw = json.loads(path.read_text())
    layouts = tuple(Layout(item["name"], item["obs_dim"], item["num_actions"]) for item in raw.get("layouts", ()))
    fields = {name: raw[name] for name in ("version", "num_envs", "agents_per_env", "obs_dim", "state_dim",
                                           "num_actions", "episode_info_dim", "goal_count", "tick_ms",
                                           "decision_ticks", "episode_seconds", "scenario")}
    return Spec(**fields, layouts=layouts)


def fill(trainer: MappoTrainer, spec: Spec, steps: int, rng: np.random.Generator) -> RolloutBuffer:
    envs, agents = spec.num_envs, spec.agents_per_env
    buffer = RolloutBuffer(steps, envs, agents, spec.obs_dim, spec.state_dim, spec.num_actions,
                           trainer.foresight_outputs, trainer.recurrent_size, bool(trainer.goal_count))
    acting = trainer.acting_state(envs, agents)

    layouts = len(spec.layouts)
    layout = rng.integers(0, layouts, size=(envs, agents)).astype(np.uint16)
    # About one episode end per env per episode length, as a real rollout has.
    end_chance = spec.decision_ms / 1000.0 / max(1, spec.episode_seconds)

    for _ in range(steps):
        obs = rng.standard_normal((envs, agents, spec.obs_dim), dtype=np.float32)
        state = rng.standard_normal((envs, spec.state_dim), dtype=np.float32)
        mask = np.zeros((envs, agents, spec.num_actions), dtype=bool)
        for index, item in enumerate(spec.layouts):
            rows = layout == index
            obs[rows, item.obs_dim:] = 0.0
            mask[rows, : item.num_actions] = rng.random((int(rows.sum()), item.num_actions)) < 0.5
        mask[..., 0] = True

        memory = acting.memory.copy() if acting.memory is not None else None
        critic_memory = acting.critic_memory.copy() if acting.critic_memory is not None else None
        actions, log_probs, values, foresight, goals, chosen = trainer.act_and_value(obs, mask, layout, state,
                                                                                     state=acting)
        present = np.ones((envs, agents), dtype=bool)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, present, foresight, memory, goals,
                            critic_memory, chosen)

        done = rng.random(envs) < end_chance
        rewards = rng.standard_normal((envs, agents)).astype(np.float32)
        final_values = np.zeros((envs, agents), dtype=np.float32)
        final_foresight = (np.zeros((envs, agents, trainer.foresight_outputs), dtype=np.float32)
                           if trainer.foresight_outputs else None)
        acting.clear(done)
        buffer.add_outcome(rewards, done, done, final_values, final_foresight)

    gamma, gae_lambda = per_decision(trainer.config, spec.decision_ms)
    last = np.zeros((envs, agents), dtype=np.float32)
    last_foresight = (np.zeros((envs, agents, trainer.foresight_outputs), dtype=np.float32)
                      if trainer.foresight_outputs else None)
    buffer.finish(last, gamma, gae_lambda, last_foresight=last_foresight)
    return buffer


def synchronize(device: torch.device) -> None:
    if device.type == "cuda":
        torch.cuda.synchronize(device)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True, help="the stage's training config (YAML)")
    parser.add_argument("--spec", required=True, help="spec.json of a run of that stage")
    parser.add_argument("--repeat", type=int, default=5, help="updates timed after the first")
    parser.add_argument("--profile", action="store_true", help="print a torch.profiler table of one update")
    parser.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="config override")
    args = parser.parse_args()

    config = TrainConfig.load(args.config, overrides=args.set)
    spec = load_spec(Path(args.spec))
    rng = np.random.default_rng(1)
    torch.manual_seed(1)
    if config.torch_threads > 0:
        torch.set_num_threads(config.torch_threads)

    trainer = MappoTrainer([(layout.obs_dim, layout.num_actions) for layout in spec.layouts], spec.state_dim,
                           config.mappo, train_device=config.resolved_train_device(),
                           rollout_device=config.resolved_rollout_device())
    device = trainer.train_device
    print(f"{spec.scenario}: {spec.num_envs} envs x {spec.agents_per_env} agents, {len(spec.layouts)} layouts, "
          f"rollout {config.rollout_length}, epochs {config.mappo.epochs}, minibatches {config.mappo.minibatches}, "
          f"recurrent {trainer.recurrent_size}; updates on {device}", flush=True)

    started = time.perf_counter()
    buffer = fill(trainer, spec, config.rollout_length, rng)
    print(f"rollout filled in {time.perf_counter() - started:.2f} s (random data, not a timing)", flush=True)

    def timed() -> float:
        synchronize(device)
        begin = time.perf_counter()
        trainer.update(buffer)
        synchronize(device)
        return time.perf_counter() - begin

    first = timed()
    times = [timed() for _ in range(args.repeat)]
    per_decision_ms = 1000.0 * float(np.median(times)) / config.rollout_length
    print(f"first update {first:.3f} s; then median {np.median(times):.3f} s, min {min(times):.3f} s over "
          f"{len(times)} ({per_decision_ms:.1f} ms per decision when the sim waits for it)", flush=True)

    if args.profile:
        activities = [torch.profiler.ProfilerActivity.CPU]
        if device.type == "cuda":
            activities.append(torch.profiler.ProfilerActivity.CUDA)
        with torch.profiler.profile(activities=activities) as profiler:
            trainer.update(buffer)
            synchronize(device)
        sort = "self_cuda_time_total" if device.type == "cuda" else "self_cpu_time_total"
        print(profiler.key_averages().table(sort_by=sort, row_limit=25), flush=True)
        print(profiler.key_averages().table(sort_by="self_cpu_time_total", row_limit=15), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
