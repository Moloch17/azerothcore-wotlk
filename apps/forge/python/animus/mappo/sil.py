"""Self-imitation (Oh et al. 2018; peak-play W5): the policy learns from its own best runs.

The K best episodes by outcome score are kept -- each as its tail, the decisions of it the rollout it ended in holds
(a wing run is longer than a rollout; the tail is where it was won), with the memories its first decision was taken
with and its discounted returns, Monte Carlo to the episode's end. Every minibatch replays a few of them through the
actor and critic and adds sil_coef * max(R - V, 0) * -log pi(a|s) to the actor's loss and sil_value_coef *
max(R - V, 0)^2 / 2 to the critic's: an action is imitated only where it did better than the critic now expects, so
an episode stops teaching once the policy reliably does what it did. Its data is the policy's own, discovered, so
nothing scripted enters.

Collected inside the update (from the buffer it is handed, whose ended_episodes the rollout noted), so the overlapped
update's thread is the only one that touches it. Bounded: at most `episodes` tails of at most a rollout's decisions.
Not checkpointed (a run that resumes starts the replay afresh).
"""

from __future__ import annotations

import numpy as np
import torch

#: What a tail keeps of the rollout, per decision [L, A, ...] (state [L, S]).
TAIL_KEYS = ("obs", "mask", "layout", "actions", "samples", "state")


class SelfImitation:
    def __init__(self, episodes: int, seed: int = 0):
        self.episodes = episodes
        self.tails: list[dict] = []
        self.rng = np.random.default_rng(seed)
        self.collected = 0

    def floor(self) -> float:
        """The score an episode must beat to be kept (-inf while there is room)."""
        return -np.inf if len(self.tails) < self.episodes else min(tail["score"] for tail in self.tails)

    def collect(self, buffer, gamma: float) -> int:
        """Keep the buffer's ended episodes (buffer.ended_episodes: (decision, envs, scores)) that are among the best;
        returns how many were taken."""
        taken = 0
        for decision, envs, scores in getattr(buffer, "ended_episodes", ()):
            for env, score in zip(envs, scores):
                if not np.isfinite(score) or score <= self.floor():
                    continue
                self._keep(buffer, int(decision), int(env), float(score), gamma)
                taken += 1
        self.collected += taken
        return taken

    def _keep(self, buffer, end: int, env: int, score: float, gamma: float) -> None:
        earlier = np.flatnonzero(buffer.dones[:end, env])
        start = int(earlier[-1]) + 1 if len(earlier) else 0
        rewards = buffer.rewards[start:end + 1, env].astype(np.float32)
        returns = np.zeros_like(rewards)
        # To the episode's end: nothing after it where it terminated, the critic's value of its last state where it
        # was cut short (a run out of time).
        following = (np.zeros(rewards.shape[1:], np.float32) if buffer.terminated[end, env]
                     else buffer.final_values[end, env].astype(np.float32))
        for t in range(len(rewards) - 1, -1, -1):
            following = rewards[t] + gamma * following
            returns[t] = following
        tail = {"score": score, "returns": returns,
                "memory": buffer.memory[start, env].copy(), "critic_memory": buffer.critic_memory[start, env].copy()}
        for key in TAIL_KEYS:
            tail[key] = getattr(buffer, key)[start:end + 1, env].copy()
        if getattr(buffer, "goals", False):
            tail["goal"] = buffer.goal[start:end + 1, env].copy()
        self.tails.append(tail)
        if len(self.tails) > self.episodes:
            self.tails.remove(min(self.tails, key=lambda kept: kept["score"]))

    def batch(self, count: int) -> dict[str, np.ndarray] | None:
        """`count` tails drawn at random, as a rollout [L, n, A, ...] padded to the longest (padding is not a sample);
        None with nothing kept."""
        if not self.tails:
            return None
        picked = [self.tails[i] for i in self.rng.choice(len(self.tails), min(count, len(self.tails)), replace=False)]
        length = max(len(tail["actions"]) for tail in picked)
        out = {}
        for key in (*TAIL_KEYS, "returns", *(("goal",) if "goal" in picked[0] else ())):
            first = picked[0][key]
            array = np.zeros((length, len(picked), *first.shape[1:]), dtype=first.dtype)
            for index, tail in enumerate(picked):
                array[: len(tail[key]), index] = tail[key]
            out[key] = array
        out["memory"] = np.stack([tail["memory"] for tail in picked])
        out["critic_memory"] = np.stack([tail["critic_memory"] for tail in picked])
        return out

    def nbytes(self, steps: int, envs_row: dict[str, np.ndarray]) -> int:
        """At most what the kept tails take: `episodes` tails of `steps` decisions of a buffer env's row sizes."""
        return int(self.episodes * steps * sum(array.nbytes for array in envs_row.values()))

    def stats(self) -> dict[str, float]:
        scores = [tail["score"] for tail in self.tails]
        return {"sil_episodes": float(len(scores)), "sil_best_score": float(max(scores)) if scores else 0.0,
                "sil_floor_score": float(min(scores)) if scores else 0.0}


def sil_policy_loss(target: torch.Tensor, expected: torch.Tensor, log_probs: torch.Tensor, counted: torch.Tensor):
    """The actor's term, mean over the counted rows of max(R - V, 0) * -log pi(a|s) (the gap a constant), and the
    share of them whose return beat the critic: a row that did no better adds nothing and pulls on nothing."""
    weight = counted.sum().clamp(min=1.0)
    gap = (target - expected).clamp(min=0.0).detach()
    return -(gap * log_probs * counted).sum() / weight, ((gap > 0) * counted).sum() / weight


def sil_value_loss(target: torch.Tensor, predicted: torch.Tensor, counted: torch.Tensor) -> torch.Tensor:
    """The critic's term, mean over the counted rows of max(R - V, 0)^2 / 2: it is only ever pulled up."""
    weight = counted.sum().clamp(min=1.0)
    return 0.5 * (((target - predicted).clamp(min=0.0) ** 2) * counted).sum() / weight
