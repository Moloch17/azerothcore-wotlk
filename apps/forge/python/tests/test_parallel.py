"""Data-parallel learners (animus.parallel): two learner processes training one run against the fake sim."""

import csv
import json
import os
import socket
import threading
from pathlib import Path

import pytest

torch = pytest.importorskip("torch")

from animus.config import TrainConfig  # noqa: E402
from animus.train import TrainingRun  # noqa: E402
from test_train_run import SPEC, fake_sim  # noqa: E402


def _free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def _rank(rank: int, tmp: str, port: int) -> None:
    steps_per_update = 4 * 2 * SPEC.num_envs * SPEC.agents_per_env
    config = TrainConfig.load(Path(__file__).parent.parent / "configs" / "stage8_duel.yaml", [
        f"socket={tmp}/sim.sock", f"runs_dir={tmp}/runs", f"layouts_dir={tmp}/layouts", "run_name=fake",
        "rollout_length=4", f"total_env_steps={2 * steps_per_update}", "checkpoint_every=1", "init_from=''",
        "train_device=cpu", "rollout_device=cpu", "mappo.hidden=[8, 8]", "mappo.epochs=1", "mappo.minibatches=1",
        f"eval.every_env_steps={steps_per_update}", "eval.episodes=6", "eval.baseline=''", "eval.sampled_every=3",
        "convergence.patience=0", f"rank={rank}", "ranks=2", f"dist_address=127.0.0.1:{port}",
    ])
    run = TrainingRun(config, resume=False)
    assert run.run() == 0
    torch.save({"actor": run.trainer.actor.state_dict(), "critic": run.trainer.critic.state_dict()},
               f"{tmp}/rank{rank}.pt")


def test_two_ranks_train_one_policy(tmp_path):
    """Two learners share one run: each trains on its own envs, the gradients are averaged, and they end as the same
    networks. Only rank 0 writes the run; each evaluation's seeds are shared out and every one is played once."""
    path = str(tmp_path / "sim.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(2)
    modes = [[], []]
    # One fake sim per learner on the one socket: the sim's pool, as each rank sees its share of it.
    servers = [threading.Thread(target=fake_sim, args=(listener, modes[index], [])) for index in range(2)]
    for server in servers:
        server.start()

    port = _free_port()
    os.environ.pop("MASTER_ADDR", None)
    os.environ.pop("MASTER_PORT", None)
    torch.multiprocessing.spawn(_rank, args=(str(tmp_path), port), nprocs=2, join=True)
    for server in servers:
        server.join(timeout=10)
    listener.close()

    ranks = [torch.load(tmp_path / f"rank{rank}.pt") for rank in range(2)]
    for network in ("actor", "critic"):
        for name, tensor in ranks[0][network].items():
            torch.testing.assert_close(ranks[1][network][name], tensor, msg=lambda text: f"{network} {name}: {text}")

    run_dir = tmp_path / "runs" / "fake"
    with (run_dir / "metrics.csv").open() as f:
        rows = list(csv.DictReader(f))
    assert [int(row["update"]) for row in rows] == [1, 2]
    assert all(float(row["reward_per_decision"]) == pytest.approx(1.0) for row in rows)
    assert json.loads((run_dir / "finished.json").read_text())["advanced"] is True

    # Every evaluation: each rank played 3 of the 6 seeds, and together every seed once, per seat.
    assert all(mode[1] == 3 for sim in modes for mode in sim if mode[0])
    episodes = [json.loads(line) for line in (run_dir / "eval_episodes.jsonl").read_text().splitlines()]
    played = [(row["env_steps"], row["policy"], row["seed"], row["layout"]) for row in episodes]
    assert len(played) == len(set(played)), "a seed was played twice"
    for evaluation in {(steps, policy) for steps, policy, _, _ in played}:
        assert {seed for steps, policy, seed, _ in played if (steps, policy) == evaluation} == set(range(6))
