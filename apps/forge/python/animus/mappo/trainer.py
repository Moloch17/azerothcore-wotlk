"""MAPPO update: PPO-clip actor, clipped value loss on a normalised centralized critic."""

from __future__ import annotations

import copy
import time
from contextlib import nullcontext
from dataclasses import dataclass

import numpy as np
import torch
from torch import nn

from ..parallel import Ranks
from .buffer import RolloutBuffer
from .networks import (LayoutActor, LayoutCritic, per_layout, per_layout_host, skip_distribution_checks, to_device,
                       update_norms)
from .valuenorm import ValueNorm


def chunked(value, length: int):
    """A [T, E, ...] rollout array (numpy or torch) as [length, T / length * E, ...]: its T/length chunks of `length`
    decisions side by side, chunk k of env e at column k * E + e."""
    steps, envs = value.shape[0], value.shape[1]
    rest = tuple(value.shape[2:])
    split = value.reshape(steps // length, length, envs, *rest)
    if isinstance(value, torch.Tensor):
        return split.transpose(0, 1).reshape(length, -1, *rest)
    return np.ascontiguousarray(split.swapaxes(0, 1)).reshape(length, -1, *rest)


@dataclass
class MappoConfig:
    hidden: tuple[int, ...] = (128, 128)
    # gamma and gae_lambda are per reference_decision_ms of game time, so a horizon means the same number of seconds
    # at any AnimusForge.DecisionMs (per_decision converts them).
    gamma: float = 0.99
    gae_lambda: float = 0.95
    reference_decision_ms: int = 100
    clip: float = 0.2
    value_clip: float = 0.2
    entropy_coef: float = 0.01
    value_coef: float = 1.0
    actor_lr: float = 5e-4
    critic_lr: float = 5e-4
    epochs: int = 5
    minibatches: int = 4
    max_grad_norm: float = 0.5
    use_value_norm: bool = True
    # How fast the value normaliser follows the returns, as the weight the running stats keep per update. The
    # returns drift as the policy improves, so stats that never follow leave the critic fitting a target measured
    # on a scale it has outgrown: 0.99 halves the old stats every ~69 updates, 0.99999 every ~69,000.
    value_norm_beta: float = 0.99
    # Normalise advantages within each layout rather than over the whole rollout. One mean and one standard
    # deviation across 18 class/builds with different reward scales lets the largest of them set the gradient of
    # the trunk they share. Groups smaller than this fall back to the rollout's own statistics.
    per_layout_advantages: bool = True
    min_layout_rows: int = 32
    # Stop an update early once its epochs have moved the policy this far in KL (0 = never). PPO's clipping
    # bounds each step, not the sum of a rollout's epochs.
    target_kl: float = 0.0
    # Centre and scale each layout's observations by what training has actually seen. The features are on very
    # different scales and meet tanh first, which saturates on anything far from zero. The statistics come from
    # the rollouts, travel in the checkpoint, and are folded into the adapter when a model is exported.
    normalise_observations: bool = True
    # Where the learning rates end, as a fraction of actor_lr and critic_lr, falling linearly over total_env_steps:
    # a constant rate kept the update growing all run (stage8_duel: approx KL 0.014 -> 0.028, ~20% of samples
    # clipped) when late progress needs small steps. 1 = constant.
    lr_final_fraction: float = 1.0
    # Auxiliary foresight heads on the actor's trunk (0 = off). They predict, from the same features the actions are
    # chosen from, the discounted return at each of foresight_horizons_seconds and how much of the episode is left
    # (as a share of foresight_time_scale_seconds), and their loss is added to the actor's. Predicting what happens
    # later is what makes those features carry it; a policy that cannot tell whether a fight is nearly over cannot
    # plan around it. Nothing reads the head at rollout time and exported models leave it out.
    # A GRU between the actor's trunk and its heads (0 = off), carried from decision to decision and cleared when an
    # episode ends: the policy's own memory. The update replays each rollout's sequences in order from the memory the
    # decisions were taken with, so what the GRU stores is learned, not only what it reads.
    # Both networks are recurrent or neither is. The critic's memory is its own -- it sees the whole env, the actor
    # only its seat -- but a feed-forward critic beside a recurrent actor cannot value what the actor remembers: the
    # global state is a snapshot, so every part of the return that follows from memory lands in the advantage as
    # noise.
    recurrent_size: int = 0
    # The recurrent update's sequence length (0 = the whole rollout). The replay is a chain of per-step kernels the
    # GPU runs one after another, so its time goes with the steps, not the rows: chunks of this many decisions, each
    # replayed from the memory the rollout stored at its first decision, are rollout_length / chunk_length times
    # fewer steps over as many times the rows. What it gives up: no gradient flows across a chunk's start, and a
    # chunk starts from the memory the rollout's policy carried rather than one the current weights would have. The
    # reference MAPPO replays 10-step chunks. Rounded down to a divisor of rollout_length; off while a distiller is
    # teaching (the teachers' memories are not stored).
    chunk_length: int = 0
    # Rollout decisions on the GPU as one captured graph per batch shape (_RolloutGraph): the ~250 small kernels of
    # the actor and critic, their inputs' uploads and their results' downloads replayed as one launch, instead of
    # issued one by one from Python. The same computation; off (or off the GPU, or with a slow layout) it runs eager.
    rollout_graphs: bool = True
    # A goal head (0 = off): the actor chooses one of goal_count goals every goal_every_decisions and keeps it in
    # between, and its action head is conditioned on it. The chooser then decides on a clock that many times slower
    # than the actions, so the horizon it has to reason over is that many times shorter. The goal is part of the
    # policy's decision: its log probability joins the action's in the PPO ratio on the decisions that chose one.
    goal_count: int = 0
    goal_every_decisions: int = 16
    # A layout whose agents decide on a slower clock than the seats and are credited on that clock: the director
    # (Curriculum::DirectorLayout). Named rather than indexed, because a layout's index moves with the stage.
    #
    # Its agents choose an action every slow_every_decisions and keep it in between, as the goal head keeps a
    # goal, and -- unlike the goal head -- their transitions are stored over those choices: one transition runs
    # from the decision that made a call to the next one, carrying every reward in between. Without that the
    # cadence buys nothing, because a call would still be credited over the seats' horizon.
    #
    # The cadence is not only about credit. Measured on stage19_duo_led at 5.9M steps with the director choosing
    # every decision: it changed the standing order on 0.706 of them, where the scripted director changed it on
    # 0.03. A seat cannot follow an order that moves every 1.4 decisions, and order_focus_kept sat at chance for
    # the whole run.
    slow_layout: str = ""
    slow_every_decisions: int = 10
    # Per slow decision, not per reference_decision_ms: with slow_every_decisions 10 and 250 ms decisions, a step
    # is 2.5 s, so 0.996 is a ~10 minute value horizon and 0.98 about 2 minutes of credit -- against the seats'
    # 100 s and 9 s. Rewards inside one span are summed rather than discounted: a span is seconds, the horizon
    # minutes.
    slow_gamma: float = 0.996
    slow_gae_lambda: float = 0.98
    foresight_coef: float = 0.0
    foresight_horizons_seconds: tuple[float, ...] = (5.0, 30.0)
    foresight_time_scale_seconds: float = 60.0
    # Where entropy_coef ends, as a fraction of itself, falling linearly over total_env_steps (the entropy floor and
    # a restart's boost still apply on top). Late on, the argmax policy that evaluation and exported models play
    # should be the one training sampled. 1 = constant.
    entropy_final_fraction: float = 1.0


@dataclass
class ActingState:
    """What a policy carries between decisions: the actor's GRU memory [E, A, R], the critic's own [E, A, R], the
    goal each seat is pursuing [E, A] and how many decisions it has held it [E, A]. None for a network without that
    part. Both memories are cleared together when an episode ends."""

    memory: np.ndarray | None = None
    critic_memory: np.ndarray | None = None
    goal: np.ndarray | None = None
    age: np.ndarray | None = None
    # A slow layout's standing action and how many decisions it has stood for, kept as the goal is.
    action: np.ndarray | None = None
    slow_age: np.ndarray | None = None

    def take(self, rows: slice) -> "ActingState":
        """A copy of these envs' state, for acting on them alone (a half-batch group); put() writes it back."""
        return ActingState(**{name: None if value is None else value[rows].copy()
                              for name, value in vars(self).items()})

    def put(self, rows: slice, part: "ActingState") -> None:
        """Write back the state take() gave out, once it has acted."""
        for name, value in vars(self).items():
            if value is not None:
                value[rows] = getattr(part, name)

    def clear(self, done: np.ndarray) -> None:
        """An episode ended in these envs: nothing is remembered, and a goal and a call are made afresh."""
        if self.memory is not None:
            self.memory[done] = 0.0
        if self.critic_memory is not None:
            self.critic_memory[done] = 0.0
        if self.goal is not None:
            self.goal[done] = 0
            self.age[done] = 0
        if self.slow_age is not None:
            # 0 makes the first decision of the new episode a choosing one, so a span never crosses an episode.
            self.action[done] = 0
            self.slow_age[done] = 0


#: An epoch may exceed the target this far before the update stops: the measure is noisy over one epoch.
EPOCH_KL_TOLERANCE = 1.5


def schedule(final_fraction: float, env_steps: int, total_env_steps: int) -> float:
    """A linear schedule's factor: 1 at the start, `final_fraction` at total_env_steps and after."""
    progress = min(1.0, max(0.0, env_steps / total_env_steps)) if total_env_steps > 0 else 0.0
    return 1.0 - (1.0 - final_fraction) * progress


def per_decision(config: MappoConfig, decision_ms: int) -> tuple[float, float]:
    """(gamma, gae_lambda) for one decision of decision_ms: the configured per-reference values, compounded."""
    exponent = max(1, decision_ms) / max(1, config.reference_decision_ms)
    return config.gamma**exponent, config.gae_lambda**exponent


def horizon_seconds(discount: float, decision_ms: int) -> float:
    """The effective horizon 1 / (1 - discount), in seconds of game time."""
    return float("inf") if discount >= 1.0 else decision_ms / 1000.0 / (1.0 - discount)


class _Downloads:
    """Device results a rollout decision needs on the host, fetched together: each is copied into pinned memory as
    soon as it is queued, and finish() waits for the device once. On the CPU they are just converted."""

    def __init__(self, stream):
        self.stream = stream
        self.items: list[torch.Tensor] = []

    def add(self, tensor: torch.Tensor) -> int:
        if tensor.device.type == "cuda":
            host = torch.empty(tensor.shape, dtype=tensor.dtype, pin_memory=True)
            host.copy_(tensor, non_blocking=True)
            tensor = host
        self.items.append(tensor)
        return len(self.items) - 1

    def finish(self) -> list[np.ndarray]:
        if self.stream is not None:
            self.stream.synchronize()
        return [item.numpy() for item in self.items]


class _Decided:
    """An actor decision whose results are still on their way to the host: finish() hands them out and updates the
    acting state from them."""

    def __init__(self, trainer, state, layout, envs, agents):
        self.trainer, self.state, self.layout = trainer, state, layout
        self.goal_t = None
        self.goal_chosen = None
        self.chosen = None
        self.goal_at = self.goal_log_prob_at = self.foresight_at = self.memory_at = None
        self.actions_at = self.log_probs_at = None

    def finish(self, fetched: list[np.ndarray]):
        goals = None
        if self.goal_at is not None:
            goal = fetched[self.goal_at]
            goals = (goal, fetched[self.goal_log_prob_at], self.goal_chosen)
            self.state.goal = goal
        if self.memory_at is not None:
            self.state.memory = fetched[self.memory_at]
        taken = fetched[self.actions_at]
        if self.chosen is not None:
            self.state.action = taken
        foresight = fetched[self.foresight_at] if self.foresight_at is not None else None
        return taken, fetched[self.log_probs_at], foresight, goals, self.chosen


class _RolloutGraph:
    """One rollout decision (MappoTrainer.act_and_value with an acting state) captured as a CUDA / HIP graph for one
    batch shape: upload from fixed pinned buffers, the actor (goal, actions, foresight, memory) and the critic
    (value, memory), and the download of every result to fixed pinned buffers. A replay is one launch and one wait.

    The graph reads the rollout networks' tensors where they were at capture: _sync_rollout copies the weights in
    place and DenseLayouts.refresh keeps the dense matrices where they are, so a replay always uses the latest
    weights. Sampling draws from the device generator, which the capture registers (fresh draws each replay)."""

    def __init__(self, trainer: "MappoTrainer", envs: int, agents: int, obs: np.ndarray, mask: np.ndarray,
                 state_features: np.ndarray, deterministic: bool):
        self.trainer, self.envs, self.agents, self.deterministic = trainer, envs, agents, deterministic
        device, rows = trainer.rollout_device, envs * agents
        recurrent, goals = trainer.recurrent_size, trainer.goal_count

        def pinned(shape, dtype):
            return torch.zeros(shape, dtype=dtype, pin_memory=True)

        # Inputs: a pinned host copy the caller fills and a device copy the graph uploads it into.
        self.host_in = {
            "obs": pinned((envs, agents, obs.shape[-1]), torch.float32),
            "layout": pinned((envs, agents), torch.long),
            "mask": pinned((envs, agents, mask.shape[-1]), torch.bool),
            "state": pinned((envs, state_features.shape[-1]), torch.float32),
        }
        if recurrent:
            self.host_in["memory"] = pinned((envs, agents, recurrent), torch.float32)
            self.host_in["critic_memory"] = pinned((envs, agents, recurrent), torch.float32)
        if goals:
            self.host_in["goal"] = pinned((envs, agents), torch.long)
            self.host_in["chosen"] = pinned((envs, agents), torch.bool)
        self.device_in = {name: torch.empty_like(host, device=device) for name, host in self.host_in.items()}
        self.host_out: dict[str, torch.Tensor] = {}

        stream = trainer._rollout_stream
        # Warm up on the capture stream (allocator and library workspaces), then capture.
        stream.wait_stream(torch.cuda.current_stream(device))
        with torch.no_grad():
            with torch.cuda.stream(stream):
                for _ in range(2):
                    self._body(rows)
            stream.synchronize()
            self.graph = torch.cuda.CUDAGraph()
            with torch.cuda.graph(self.graph, stream=stream):
                self._body(rows)

    def _body(self, rows: int) -> None:
        trainer = self.trainer
        envs, agents = self.envs, self.agents
        actor, critic = trainer._rollout_actor, trainer._rollout_critic
        inputs = self.device_in
        for name, host in self.host_in.items():
            inputs[name].copy_(host, non_blocking=True)

        obs_t = inputs["obs"].reshape(rows, -1)
        layout_t = inputs["layout"].reshape(rows)
        mask_t = inputs["mask"].reshape(rows, -1)
        memory = inputs["memory"].reshape(rows, -1) if "memory" in inputs else None
        features = actor.features(obs_t, layout_t, memory, None)

        out: dict[str, torch.Tensor] = {}
        goal_t = None
        if trainer.goal_count:
            distribution = actor.goal_distribution(features)
            sampled = distribution.logits.argmax(dim=-1) if self.deterministic else distribution.sample()
            goal_t = torch.where(inputs["chosen"].reshape(rows), sampled, inputs["goal"].reshape(rows))
            out["goal"] = goal_t.reshape(envs, agents)
            out["goal_log_prob"] = distribution.log_prob(goal_t).reshape(envs, agents)

        dist = actor.action_distribution(features, layout_t, mask_t, goal_t, None)
        actions = dist.logits.argmax(dim=-1) if self.deterministic else dist.sample()
        out["actions"] = actions.reshape(envs, agents)
        out["log_probs"] = dist.log_prob(actions).reshape(envs, agents)
        if trainer.foresight_outputs:
            out["foresight"] = actor.foresight(features).reshape(envs, agents, trainer.foresight_outputs)
        if memory is not None:
            out["memory"] = features.reshape(envs, agents, trainer.recurrent_size)

        state_t = inputs["state"][:, None, :].expand(envs, agents, inputs["state"].shape[-1]).reshape(rows, -1)
        critic_memory = inputs["critic_memory"].reshape(rows, -1) if "critic_memory" in inputs else None
        values, carried = critic.step(state_t, obs_t, layout_t, goal_t, None, memory=critic_memory)
        if trainer._rollout_value_norm is not None:
            values = trainer._rollout_value_norm.denormalize(values)
        out["values"] = values.reshape(envs, agents)
        if critic_memory is not None:
            out["critic_memory"] = carried.reshape(envs, agents, trainer.recurrent_size)

        if not self.host_out:
            self.host_out = {name: torch.empty(value.shape, dtype=value.dtype, pin_memory=True)
                             for name, value in out.items()}
        for name, value in out.items():
            self.host_out[name].copy_(value, non_blocking=True)

    def run(self, obs, mask, layout, state_features, state: "ActingState"):
        """One decision: fill the inputs, replay, wait once. Returns the act_and_value tuple and updates `state`."""
        trainer = self.trainer
        host = self.host_in
        np.copyto(host["obs"].numpy(), obs)
        np.copyto(host["layout"].numpy(), layout, casting="unsafe")
        np.copyto(host["mask"].numpy(), mask)
        np.copyto(host["state"].numpy(), state_features)
        if trainer.recurrent_size:
            np.copyto(host["memory"].numpy(), state.memory)
            np.copyto(host["critic_memory"].numpy(), state.critic_memory)
        chosen = None
        if trainer.goal_count:
            # A goal is chosen on its own clock and kept in between; a cleared state (a new episode) chooses at once.
            chosen = (state.age % max(1, trainer.config.goal_every_decisions)) == 0
            np.copyto(host["goal"].numpy(), state.goal, casting="unsafe")
            np.copyto(host["chosen"].numpy(), chosen)
            state.age = np.where(chosen, 1, state.age + 1)

        self.graph.replay()
        trainer._rollout_stream.synchronize()
        # Copies: the pinned outputs are overwritten by the next replay.
        fetched = {name: value.numpy().copy() for name, value in self.host_out.items()}

        goals = None
        if trainer.goal_count:
            state.goal = fetched["goal"]
            goals = (fetched["goal"], fetched["goal_log_prob"], chosen)
        if trainer.recurrent_size:
            state.memory = fetched["memory"]
            state.critic_memory = fetched["critic_memory"]
        return fetched["actions"], fetched["log_probs"], fetched["values"], fetched.get("foresight"), goals, None


class MappoTrainer:
    """Owns the networks. Rollouts run on `rollout_device` (a CPU copy is usually fastest for small
    MLPs at batch sizes of a few hundred); updates run on `train_device`."""

    def __init__(
        self,
        layouts: list[tuple[int, int]],
        state_dim: int,
        config: MappoConfig,
        train_device: str = "cpu",
        rollout_device: str = "cpu",
        slow_layout: int = -1,
        ranks=None,
    ):
        """layouts: (obs dim, action count) per agent layout, in the sim's layout order. `slow_layout` is the
        index of config.slow_layout among them, resolved by the caller (layouts carry no names here); -1 when the
        run has none."""
        skip_distribution_checks()
        self.config = config
        self.layouts = list(layouts)
        self.slow_layout = slow_layout if config.slow_layout else -1
        # Data-parallel learners (animus.parallel.Ranks): gradients and statistics reduced across them; alone, none.
        self.ranks = ranks if ranks is not None else Ranks()
        self.state_dim = state_dim
        self.train_device = torch.device(train_device)
        self.rollout_device = torch.device(rollout_device)
        # Starts at config.entropy_coef; the stage controller raises it after a restart (animus.stage).
        self.entropy_coef = config.entropy_coef

        hidden = list(config.hidden)
        self.foresight_outputs = (len(config.foresight_horizons_seconds) + 1) if config.foresight_coef > 0.0 else 0
        self.recurrent_size = config.recurrent_size
        self.goal_count = config.goal_count
        self.actor = LayoutActor(self.layouts, hidden, self.foresight_outputs, self.recurrent_size,
                                 self.goal_count).to(self.train_device)
        self.critic = LayoutCritic(state_dim, self.layouts, hidden, self.goal_count,
                                   self.recurrent_size).to(self.train_device)
        self.value_norm = (ValueNorm(beta=config.value_norm_beta).to(self.train_device)
                           if config.use_value_norm else None)

        self.reset_optimizers()

        self._rollout_stream = (torch.cuda.Stream(device=self.rollout_device, priority=-1)
                                if self.rollout_device.type == "cuda" else None)
        # The recurrent update's actor and critic halves (_update_recurrent); None off the GPU.
        self._update_streams = (tuple(torch.cuda.Stream(device=self.train_device) for _ in range(2))
                                if self.train_device.type == "cuda" else (None, None))
        self._rollout_graphs: dict[tuple, _RolloutGraph] = {}
        self._rollout_actor = copy.deepcopy(self.actor).to(self.rollout_device)
        self._rollout_critic = copy.deepcopy(self.critic).to(self.rollout_device)
        self._rollout_value_norm = (
            copy.deepcopy(self.value_norm).to(self.rollout_device) if self.value_norm is not None else None
        )

        # (trained tensor, rollout tensor) for every parameter and buffer the rollout networks mirror, paired
        # once here so a sync is a copy rather than a state dict.
        self._rollout_pairs = self._pair_tensors()
        self._sync_rollout()

    def _pair_tensors(self) -> list[tuple[torch.Tensor, torch.Tensor]]:
        """Every tensor a rollout copy mirrors, next to the trained tensor it comes from."""
        trained = [self.actor, self.critic]
        rollout = [self._rollout_actor, self._rollout_critic]
        if self.value_norm is not None:
            trained.append(self.value_norm)
            rollout.append(self._rollout_value_norm)

        pairs = []
        for source, destination in zip(trained, rollout):
            for name, tensor in list(source.named_parameters()) + list(source.named_buffers()):
                pairs.append((tensor, dict(list(destination.named_parameters())
                                           + list(destination.named_buffers()))[name]))
        return pairs

    def reset_optimizers(self) -> None:
        """Fresh Adam state: after a restart the step sizes are no longer shrunk by the old gradient history."""
        self.actor_opt = torch.optim.Adam(self.actor.parameters(), lr=self.config.actor_lr, eps=1e-5)
        self.critic_opt = torch.optim.Adam(self.critic.parameters(), lr=self.config.critic_lr, eps=1e-5)
        # The last update's entropy and approx_kl per layout (animus.stage reads them per class), by layout index.
        self.layout_stats: dict[int, dict[str, float]] = {}
        self.frozen_layouts: set[int] = set()

    def freeze_layouts(self, indices: set[int]) -> None:
        """Stop training the adapters and heads of these layouts (a class that has converged, animus.stage): their
        own parameters take no gradient, so only the shared trunk can still move them. Layouts not in `indices`
        are thawed."""
        self.frozen_layouts = set(indices)
        for network in (self.actor, self.critic):
            for name, parameter in network.named_parameters():
                parts = name.split(".")
                if len(parts) >= 2 and parts[0] in ("adapters", "heads") and parts[1].isdigit():
                    parameter.requires_grad_(int(parts[1]) not in self.frozen_layouts)

    def _layout_totals(self, totals: dict, layout: torch.Tensor, entropy: torch.Tensor, kl: torch.Tensor,
                       weight: torch.Tensor | None = None) -> None:
        """Add one minibatch's per-row entropy and KL into per-layout sums (weighted by `weight` where given). The
        sums stay on the device and are read once, in _finish_layout_stats: read per minibatch, they were three
        pipeline stalls per layout per minibatch."""
        flat_layout = layout.reshape(-1).long()
        flat_entropy = entropy.reshape(-1).detach()
        flat_weight = weight.reshape(-1).to(flat_entropy.dtype) if weight is not None else torch.ones_like(flat_entropy)
        rows = torch.stack([flat_entropy * flat_weight, kl.reshape(-1).detach() * flat_weight, flat_weight], dim=1)
        sums = totals.get("sums")
        if sums is None:
            sums = totals["sums"] = rows.new_zeros((len(self.layouts), 3))
        sums.index_add_(0, flat_layout, rows)

    def _finish_layout_stats(self, totals: dict) -> None:
        sums = totals.get("sums")
        if self.ranks.active:
            # Every rank's rows: a collective, so a rank that had none contributes zeros rather than skipping it.
            if sums is None:
                sums = torch.zeros((len(self.layouts), 3), device=self.train_device)
            sums = self.ranks.sum(sums)
        read = sums.double().cpu().tolist() if sums is not None else []
        self.layout_stats = {index: {"entropy": e / max(n, 1e-9), "approx_kl": k / max(n, 1e-9), "rows": n}
                             for index, (e, k, n) in enumerate(read) if n > 0}

    def set_learning_rate_scale(self, scale: float) -> None:
        """Both optimizers at `scale` times their configured learning rate (see MappoConfig.lr_final_fraction)."""
        for optimizer, rate in ((self.actor_opt, self.config.actor_lr), (self.critic_opt, self.config.critic_lr)):
            for group in optimizer.param_groups:
                group["lr"] = rate * scale

    @torch.no_grad()
    def shrink_perturb(self, shrink: float, perturb: float) -> None:
        """weights = shrink x weights + perturb x freshly initialised weights (Ash & Adams, 2020)."""
        hidden = list(self.config.hidden)
        fresh = (LayoutActor(self.layouts, hidden, self.foresight_outputs, self.recurrent_size, self.goal_count),
                 LayoutCritic(self.state_dim, self.layouts, hidden, self.goal_count, self.recurrent_size))
        for network, init in zip((self.actor, self.critic), fresh):
            for param, init_param in zip(network.parameters(), init.to(self.train_device).parameters()):
                param.mul_(shrink).add_(init_param, alpha=perturb)
        self._sync_rollout()

    # ------------------------------------------------------------------ rollout

    def sync_rollout(self) -> None:
        """Copy the trained weights to the rollout networks. `update(sync=False)` leaves this to the caller, which
        overlapping an update with the next rollout needs: the rollout reads these copies while the update runs."""
        self._sync_rollout()

    @torch.no_grad()
    def _sync_rollout(self) -> None:
        # Copy tensor by tensor into the networks that are already there. Building a state dict and loading it
        # allocates a host copy of every parameter and buffer of both networks after every update, which with a
        # GPU is the whole model over the bus; the rollout copies only ever need the values.
        for source, destination in self._rollout_pairs:
            destination.copy_(source)
        # The rollout copies act on 128-row batches, where each normaliser's five elementwise passes cost more than
        # the adapter after it: folded into the adapters, on the copies only.
        self._rollout_actor.fold_normalisation()
        self._rollout_critic.fold_normalisation()
        if self.rollout_device.type == "cuda":
            self._rollout_actor.densify(max(self._rollout_actor.obs_dims))
            self._rollout_critic.densify(max(self._rollout_critic.obs_dims))
        # The rollout's stream reads these copies next: after them, not beside them.
        if self._rollout_stream is not None:
            self._rollout_stream.wait_stream(torch.cuda.current_stream(self.rollout_device))

    def _tensor(self, array: np.ndarray, dtype=None) -> torch.Tensor:
        if self.rollout_device.type != "cuda":
            return torch.as_tensor(array, device=self.rollout_device, dtype=dtype)
        # Up from pinned memory without waiting: from pageable memory every input would wait for the device.
        return to_device(torch.as_tensor(np.ascontiguousarray(array), dtype=dtype), self.rollout_device)

    def _rollout_graph(self, obs, mask, layout, state_features, deterministic: bool,
                       state: "ActingState | None") -> "_RolloutGraph | None":
        """The captured decision for this batch shape, captured on first use; None where it does not apply: off the
        GPU, turned off (mappo.rollout_graphs), without an acting state, or with a slow layout (its held decisions
        branch on the host)."""
        if (self._rollout_stream is None or not self.config.rollout_graphs or state is None
                or self.slow_layout >= 0):
            return None
        envs, agents = layout.shape
        key = (envs, agents, obs.shape[-1], mask.shape[-1], state_features.shape[-1], bool(deterministic))
        graph = self._rollout_graphs.get(key)
        if graph is None:
            graph = self._rollout_graphs[key] = _RolloutGraph(self, envs, agents, obs, mask, state_features,
                                                              bool(deterministic))
        return graph

    def _groups(self, layout: np.ndarray, layout_t: torch.Tensor):
        """The rows of each layout: grouped on the host when the rollout is on the GPU, so no forward pass waits to
        read the layouts back."""
        if self.rollout_device.type == "cuda":
            return None  # the rollout copies are dense on the GPU (densify): no groups to build
        return per_layout(layout_t, len(self.layouts))

    def _switch_stream(self, stream, wait_for=None) -> None:
        """Make `stream` current, after the work queued on `wait_for` (a stream or several). Nothing on the CPU."""
        if stream is None:
            return
        for other in (wait_for if isinstance(wait_for, (tuple, list)) else (wait_for,)):
            if other is not None and other is not stream:
                stream.wait_stream(other)
        torch.cuda.set_stream(stream)

    def _rollout_context(self):
        """The rollout's stream on the GPU: its own and high priority, so its few small kernels per decision do not
        queue behind an overlapped update's thousands."""
        return torch.cuda.stream(self._rollout_stream) if self._rollout_stream is not None else nullcontext()

    @torch.no_grad()
    def act(self, obs: np.ndarray, mask: np.ndarray, layout: np.ndarray, deterministic: bool = False,
            state: "ActingState | None" = None) -> tuple[np.ndarray, np.ndarray]:
        """obs [E, A, O], mask [E, A, N], layout [E, A] -> actions [E, A], log_probs [E, A].

        `state` is carried in and updated in place (memory, goal): the policy of a decision is the policy of what it
        remembers and is pursuing.
        """
        with self._rollout_context():
            actions, log_probs, _, _, _ = self._decide(obs, mask, layout, deterministic, state)
        return actions, log_probs

    @torch.inference_mode()
    def act_and_value(self, obs: np.ndarray, mask: np.ndarray, layout: np.ndarray, state_features: np.ndarray,
                      deterministic: bool = False, state: "ActingState | None" = None):
        """act() and value() of one decision in one pass over the inputs: the rows are converted and grouped by
        layout once for both networks. Returns (actions, log_probs, values, foresight or None, goals or None), the
        last three [E, A, H + 1], (goal, goal log prob, whether this decision chose it), and which
        decisions are samples (a slow layout's held ones are not; None when the run has no slow layout)."""
        with self._rollout_context():
            envs, agents = layout.shape
            graph = self._rollout_graph(obs, mask, layout, state_features, deterministic, state)
            if graph is not None:
                return graph.run(obs, mask, layout, state_features, state)
            rows = envs * agents
            downloads = _Downloads(self._rollout_stream)
            # Converted and grouped by layout once, for the actor and the critic both.
            obs_t = self._tensor(obs).reshape(rows, -1)
            layout_t = self._tensor(layout, torch.long).reshape(rows)
            groups = self._groups(layout, layout_t)
            decided = self._decide(obs, mask, layout, deterministic, state, (obs_t, layout_t, groups), downloads)

            state_t = self._tensor(state_features)[:, None, :].expand(envs, agents, state_features.shape[-1]).reshape(
                rows, -1)
            goal_t = decided.goal_t if self.goal_count and decided.goal_t is not None else None
            critic_memory = (self._memory_tensor(state.critic_memory if state is not None else None, rows)
                             if self.recurrent_size else None)
            values, carried = self._rollout_critic.step(state_t, obs_t, layout_t, goal_t, groups,
                                                        memory=critic_memory)
            carried_at = (downloads.add(carried.reshape(envs, agents, self.recurrent_size))
                          if self.recurrent_size and state is not None else None)
            if self._rollout_value_norm is not None:
                values = self._rollout_value_norm.denormalize(values)
            values_at = downloads.add(values.reshape(envs, agents))

            fetched = downloads.finish()
            actions, log_probs, foresight, goals, chosen = decided.finish(fetched)
            if carried_at is not None:
                state.critic_memory = fetched[carried_at]
            return actions, log_probs, fetched[values_at], foresight, goals, chosen

    @torch.no_grad()
    def _decide(self, obs: np.ndarray, mask: np.ndarray, layout: np.ndarray, deterministic: bool,
                state: "ActingState | None", prepared=None, downloads: "_Downloads | None" = None):
        """One decision of the actor: actions, their log probabilities, the foresight predictions and the goals. The
        acting state's memory and goal are updated when the result is finished. `prepared` is (obs, layout, groups)
        as tensors when the caller has them already. With `downloads` the results are queued there and the caller
        finishes them after its one wait for the device (_Decided.finish); without, they are finished here."""
        own = downloads is None
        if own:
            downloads = _Downloads(self._rollout_stream)
        envs, agents = layout.shape
        rows = envs * agents
        if prepared is None:
            obs_t = self._tensor(obs).reshape(rows, -1)
            layout_t = self._tensor(layout, torch.long).reshape(rows)
            groups = self._groups(layout, layout_t)
        else:
            obs_t, layout_t, groups = prepared
        mask_t = self._tensor(mask).reshape(rows, -1)

        memory = state.memory if state is not None else None
        features = self._rollout_actor.features(
            obs_t, layout_t, self._memory_tensor(memory, rows) if self.recurrent_size else None, groups)

        decided = _Decided(self, state, layout, envs, agents)
        if self.goal_count and state is not None:
            # A goal is chosen on its own clock and kept in between; a cleared state (a new episode) chooses at once.
            chosen = (state.age % max(1, self.config.goal_every_decisions)) == 0
            distribution = self._rollout_actor.goal_distribution(features)
            sampled = distribution.logits.argmax(dim=-1) if deterministic else distribution.sample()
            kept = self._tensor(state.goal, torch.long).reshape(rows)
            chosen_t = self._tensor(chosen).reshape(rows)
            decided.goal_t = torch.where(chosen_t, sampled, kept)
            decided.goal_log_prob_at = downloads.add(distribution.log_prob(decided.goal_t).reshape(envs, agents))
            decided.goal_at = downloads.add(decided.goal_t.reshape(envs, agents))
            decided.goal_chosen = chosen
            state.age = np.where(chosen, 1, state.age + 1)

        dist = self._rollout_actor.action_distribution(features, layout_t, mask_t, decided.goal_t, groups)
        actions = dist.logits.argmax(dim=-1) if deterministic else dist.sample()
        log_probs = dist.log_prob(actions)

        # A slow layout speaks on its own clock and its call stands in between, so the seats have something
        # steady enough to act on. The log probabilities of the held decisions are the sampled action's and not
        # the held one's, which costs nothing: a held decision is not a sample and never reaches the loss.
        if self.slow_layout >= 0 and state is not None and state.slow_age is not None:
            every = max(1, self.config.slow_every_decisions)
            slow = layout == self.slow_layout
            choosing = slow & (state.slow_age % every == 0)
            holding = slow & ~choosing
            if holding.any():
                actions = torch.where(self._tensor(holding).reshape(rows).bool(),
                                      self._tensor(state.action, torch.long).reshape(rows), actions)
            decided.chosen = ~slow | choosing
            # A choosing decision starts the count again at 1, as the goal head's age does.
            state.slow_age = np.where(slow, np.where(decided.chosen, 1, state.slow_age + 1), 0)

        if self.foresight_outputs:
            decided.foresight_at = downloads.add(
                self._rollout_actor.foresight(features).reshape(envs, agents, self.foresight_outputs))
        if state is not None and self.recurrent_size:
            decided.memory_at = downloads.add(features.reshape(envs, agents, self.recurrent_size))
        decided.actions_at = downloads.add(actions.reshape(envs, agents))
        decided.log_probs_at = downloads.add(log_probs.reshape(envs, agents))
        return decided.finish(downloads.finish()) if own else decided

    @torch.no_grad()
    def foresight_of(self, obs: np.ndarray, layout: np.ndarray, memory: np.ndarray | None = None) -> np.ndarray | None:
        """The foresight head on observations the rollout did not act on (an ended episode's last one, and the one
        after the rollout), for the targets' bootstrap: [..., H + 1], or None when the head is off."""
        if not self.foresight_outputs:
            return None

        lead = layout.shape
        rows = int(np.prod(lead))
        with self._rollout_context():
            downloads = _Downloads(self._rollout_stream)
            obs_t = self._tensor(obs).reshape(rows, -1)
            layout_t = self._tensor(layout, torch.long).reshape(rows)
            features = self._rollout_actor.features(
                obs_t, layout_t, self._memory_tensor(memory, rows) if self.recurrent_size else None,
                self._groups(layout, layout_t))
            downloads.add(self._rollout_actor.foresight(features).reshape(*lead, self.foresight_outputs))
            return downloads.finish()[0]

    def _memory_tensor(self, memory: np.ndarray | None, rows: int) -> torch.Tensor:
        """The memory to carry in, as the actor wants it: cleared when the caller has none."""
        if memory is None:
            return torch.zeros((rows, self.recurrent_size), device=self.rollout_device)
        return self._tensor(memory).reshape(rows, self.recurrent_size)

    def acting_state(self, envs: int, agents: int) -> "ActingState":
        """What the policy carries from decision to decision: its memory and the goal it is pursuing. Whoever acts
        keeps one, clears the rows of episodes that ended, and hands it back in -- the policy is only itself with
        them. An actor with neither carries nothing and behaves exactly as before."""
        return ActingState(
            memory=np.zeros((envs, agents, self.recurrent_size), dtype=np.float32) if self.recurrent_size else None,
            critic_memory=(np.zeros((envs, agents, self.recurrent_size), dtype=np.float32)
                           if self.recurrent_size else None),
            goal=np.zeros((envs, agents), dtype=np.int64) if self.goal_count else None,
            age=np.zeros((envs, agents), dtype=np.int64) if self.goal_count else None,
            action=np.zeros((envs, agents), dtype=np.int64) if self.slow_layout >= 0 else None,
            slow_age=np.zeros((envs, agents), dtype=np.int64) if self.slow_layout >= 0 else None,
        )

    def _memory_tensor(self, memory: np.ndarray | None, rows: int) -> torch.Tensor:
        """The memory to carry in, as the actor wants it: cleared when the caller has none."""
        if memory is None:
            return torch.zeros((rows, self.recurrent_size), device=self.rollout_device)
        return self._tensor(memory).reshape(rows, self.recurrent_size)

    @torch.no_grad()
    def value(self, state: np.ndarray, obs: np.ndarray, layout: np.ndarray,
              goal: np.ndarray | None = None, memory: np.ndarray | None = None) -> np.ndarray:
        """state [E, S], obs [E, A, O], layout [E, A], goal [E, A] (with a goal head) -> denormalised V per agent
        [E, A]. The value depends on the goal the actor is pursuing, so pass the same goal the decision used, and on
        what the critic remembers of the episode, so pass the memory those decisions left (bootstrapping a truncated
        episode from a cleared memory values a fight in progress as if it had just begun)."""
        envs, agents = layout.shape
        with self._rollout_context():
            downloads = _Downloads(self._rollout_stream)
            goal_t = self._tensor(goal, torch.long) if self.goal_count and goal is not None else None
            state_t = self._tensor(state)[:, None, :].expand(envs, agents, state.shape[-1])
            memory_t = (self._memory_tensor(memory, envs * agents).reshape(envs, agents, self.recurrent_size)
                        if self.recurrent_size else None)
            layout_t = self._tensor(layout, torch.long)
            values = self._rollout_critic(state_t, self._tensor(obs), layout_t, goal_t,
                                          self._groups(layout, layout_t.reshape(-1)), memory=memory_t)
            if self._rollout_value_norm is not None:
                values = self._rollout_value_norm.denormalize(values)
            downloads.add(values)
            return downloads.finish()[0]

    def _goal_stats(self, data: dict) -> dict[str, float]:
        """How the goal head is being used, from the rollout itself: the share of decisions spent on each goal, and
        how often a goal choice kept the previous goal. A head that has collapsed shows one share at 1 and a kept
        share of 1; one that is not being used at all shows the shares of a uniform draw."""
        if not self.goal_count or "goal" not in data:
            return {}

        goals = data["goal"]
        counts = torch.bincount(goals.reshape(-1), minlength=self.goal_count).float()
        total = counts.sum().clamp(min=1.0)
        stats = {f"goal_{index}_share": float(counts[index] / total) for index in range(self.goal_count)}

        # A goal choice that kept the goal the decision before was pursuing: the head is holding, not switching.
        # Only the sequences have a decision before -- flat rows are shuffled together from every env and step, so
        # their neighbour means nothing.
        chosen = data["goal_chosen"] if "goal_chosen" in data else None
        if goals.dim() == 3 and chosen is not None and bool(chosen.any()):
            previous = torch.roll(goals, shifts=1, dims=0)
            previous[0] = goals[0]
            kept = (goals == previous) & chosen.bool()
            stats["goal_kept_share"] = float(kept.sum() / chosen.sum().clamp(min=1.0))
        return stats

    # ------------------------------------------------------------------ update

    def update(self, buffer: RolloutBuffer, auxiliary=None, sync: bool = True) -> dict[str, float]:
        """One PPO update over the rollout. `auxiliary(data, idx, dist)` may add a loss to each minibatch's actor
        loss: it returns (loss, {stat: value}) or None (see animus.distill)."""
        if self.recurrent_size:
            if auxiliary is not None and not hasattr(auxiliary, "sequence_loss"):
                raise ValueError("a recurrent actor needs a sequence-aware auxiliary loss (animus.distill.Distiller): "
                                 "its rows are replayed in order, so a per-minibatch hook cannot carry the teachers' "
                                 "memories")
            return self._update_recurrent(buffer, auxiliary, sync)

        cfg = self.config
        started = time.perf_counter()
        stats = {"policy_loss": 0.0, "value_loss": 0.0, "entropy": 0.0, "clip_frac": 0.0, "approx_kl": 0.0,
                 "actor_grad_norm": 0.0, "critic_grad_norm": 0.0}
        if self.goal_count:
            stats["goal_entropy"] = 0.0
        foresight = self.foresight_outputs > 0 and buffer.foresight >= self.foresight_outputs
        if foresight:
            stats["foresight_loss"] = 0.0
        data = {k: torch.as_tensor(v, device=self.train_device) for k, v in buffer.flat().items()}
        if data["actions"].shape[0] == 0 and not self.ranks.active:
            return stats  # no seat had a character this rollout: nothing to learn from

        data["advantages"] = self._normalise_advantages(data["advantages"], data["layout"])

        if self.value_norm is not None:
            self.value_norm.update(data["returns"], self.ranks)
            data["returns_target"] = self.value_norm.normalize(data["returns"])
            data["old_values"] = self.value_norm.normalize(data["values"])
        else:
            data["returns_target"] = data["returns"]
            data["old_values"] = data["values"]

        # How much of the returns' spread the critic already accounts for, on the values it produced during the
        # rollout. value_loss is reported in normalised space and shrinks with the normaliser, so it cannot say
        # whether the critic actually fits; this can. 0 = no better than predicting the mean, 1 = perfect.
        returns, values = data["returns"], data["values"]
        variance = returns.var()
        explained = 1.0 - (returns - values).var() / variance if float(variance) > 0.0 else torch.zeros(())

        samples = data["actions"].shape[0]
        # Even splits: `samples // minibatches` with a fixed stride leaves a remainder minibatch, which is a full
        # optimizer step on a fragment of the rollout.
        splits = max(1, min(cfg.minibatches, samples))
        auxiliary_stats: dict[str, float] = {}
        auxiliary_updates = 0
        updates = 0

        # Summed on the device and read once at the end: a .item() per statistic per minibatch is a pipeline stall
        # per statistic per minibatch.
        totals = {name: torch.zeros((), device=self.train_device) for name in stats}
        layout_totals: dict = {}
        epochs_run = 0

        for _ in range(cfg.epochs):
            epoch_kl = torch.zeros((), device=self.train_device)
            epoch_updates = 0
            order = torch.randperm(samples, device=self.train_device)
            for idx in torch.tensor_split(order, splits):
                obs, layout = data["obs"][idx], data["layout"][idx]

                features = self.actor.features(obs, layout)
                goal = data["goal"][idx] if self.goal_count else None
                dist = self.actor.action_distribution(features, layout, data["mask"][idx], goal)
                predictions = (self.actor.foresight(features) if foresight else None)
                log_probs = dist.log_prob(data["actions"][idx])
                action_entropy = dist.entropy().mean()
                entropy = action_entropy
                if self.goal_count:
                    # Choosing a goal is part of the decision that chose it: its log probability joins the action's,
                    # and its entropy is kept up on those decisions too. The goal term is averaged over every row,
                    # not only the rows that chose a goal, so a head consulted once in goal_every_decisions is worth
                    # that share of the bonus rather than as much as the action head on every decision.
                    goals = self.actor.goal_distribution(features)
                    chosen = data["goal_chosen"][idx].float()
                    log_probs = log_probs + goals.log_prob(goal) * chosen
                    goal_entropy = (goals.entropy() * chosen).sum()
                    entropy = entropy + goal_entropy / max(1, chosen.numel())
                    goal_entropy = goal_entropy.detach() / chosen.sum().clamp(min=1.0)
                    data_log_probs = data["log_probs"][idx] + data["goal_log_probs"][idx] * chosen
                else:
                    data_log_probs = data["log_probs"][idx]
                log_ratio = log_probs - data_log_probs
                ratio = log_ratio.exp()

                adv = data["advantages"][idx]
                policy_loss = -torch.min(ratio * adv, ratio.clamp(1 - cfg.clip, 1 + cfg.clip) * adv).mean()

                actor_loss = policy_loss - self.entropy_coef * entropy
                if foresight:
                    # Huber on the discounted returns, which are on the rewards' scale and have outliers; squared
                    # error on the share of the episode left, which is already 0 to 1. Steps whose episode does not
                    # end inside the rollout have no share to learn, and are left out.
                    targets, valid = data["foresight_targets"][idx], data["foresight_valid"][idx]
                    horizons = self.foresight_outputs - 1
                    errors = torch.cat([
                        nn.functional.smooth_l1_loss(predictions[:, :horizons], targets[:, :horizons],
                                                     reduction="none"),
                        (predictions[:, horizons:] - targets[:, horizons:]) ** 2,
                    ], dim=-1)
                    counted = valid.float()
                    foresight_loss = (errors * counted).sum() / counted.sum().clamp(min=1.0)
                    actor_loss = actor_loss + cfg.foresight_coef * foresight_loss
                    totals["foresight_loss"] += foresight_loss.detach()
                if auxiliary is not None and (extra := auxiliary(data, idx, dist)) is not None:
                    loss, extra_stats = extra
                    actor_loss = actor_loss + loss
                    for key, value in extra_stats.items():
                        auxiliary_stats[key] = auxiliary_stats.get(key, 0.0) + value
                    auxiliary_updates += 1

                self.actor_opt.zero_grad()
                actor_loss.backward()
                self.ranks.average_gradients(self.actor.parameters())
                actor_grad = nn.utils.clip_grad_norm_(self.actor.parameters(), cfg.max_grad_norm)
                self.actor_opt.step()

                values = self.critic(data["state"][idx], obs, layout, goal)
                old_values = data["old_values"][idx]
                target = data["returns_target"][idx]
                clipped = old_values + (values - old_values).clamp(-cfg.value_clip, cfg.value_clip)
                value_loss = torch.max((values - target) ** 2, (clipped - target) ** 2).mean()

                self.critic_opt.zero_grad()
                (cfg.value_coef * value_loss).backward()
                self.ranks.average_gradients(self.critic.parameters())
                critic_grad = nn.utils.clip_grad_norm_(self.critic.parameters(), cfg.max_grad_norm)
                self.critic_opt.step()

                with torch.no_grad():
                    self._layout_totals(layout_totals, layout, dist.entropy(), (ratio - 1) - log_ratio)
                    totals["policy_loss"] += policy_loss.detach()
                    totals["value_loss"] += value_loss.detach()
                    # Reported on its own: the entropy floor compares this with ln(allowed actions), and folding
                    # the goal head's entropy in would hide a collapsing action policy.
                    totals["entropy"] += action_entropy.detach()
                    if self.goal_count:
                        totals["goal_entropy"] += goal_entropy
                    totals["clip_frac"] += ((ratio - 1).abs() > cfg.clip).float().mean()
                    totals["approx_kl"] += ((ratio - 1) - log_ratio).mean()
                    totals["actor_grad_norm"] += actor_grad
                    totals["critic_grad_norm"] += critic_grad
                    epoch_kl += ((ratio - 1) - log_ratio).mean()
                updates += 1
                epoch_updates += 1

            epochs_run += 1
            # One read per epoch, not per minibatch: enough to stop before the next epoch pulls the policy
            # further from the rollout that justified it.
            # Every rank stops on the same epoch: on the ranks' mean KL, not its own.
            if cfg.target_kl > 0.0 and epoch_updates \
                    and float(self.ranks.mean(epoch_kl)) / epoch_updates > EPOCH_KL_TOLERANCE * cfg.target_kl:
                break

        # After the epochs, not before: the rollout acted through these statistics, and its stored log_probs are
        # the denominator of every PPO ratio in the loop above. Advancing them first makes the ratio something
        # other than 1 at epoch 0 -- a normaliser shift read as a policy change. Updating here and syncing the
        # rollout copies below keeps the acting and training views of a feature identical, one rollout apart.
        if cfg.normalise_observations:
            update_norms(self.actor.norms, data["obs"], data["layout"], self.actor.obs_dims, self.ranks)
            update_norms(self.critic.norms, data["obs"], data["layout"], self.critic.obs_dims, self.ranks)
            self.critic.state_norm.update(data["state"], self.ranks)

        if sync:
            self._sync_rollout()
        self._finish_layout_stats(layout_totals)
        stats = {name: float(total) for name, total in totals.items()}
        result = {k: v / max(1, updates) for k, v in stats.items()}
        result["explained_variance"] = float(explained)
        result["epochs_run"] = float(epochs_run)
        result.update(self._goal_stats(data))
        result.update({k: v / auxiliary_updates for k, v in auxiliary_stats.items()})
        # What the update itself cost. With overlap_updates the run's own timer measures the wait for this to
        # finish, not the work, so without this the work is invisible.
        result["update_compute_seconds"] = time.perf_counter() - started
        return result

    def _normalise_advantages(self, advantages: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """Centre and scale the advantages, within each layout when there are enough rows of it.

        The layouts of one rollout have their own return scales (a duel against a creature and a healer keeping a
        party alive are not the same numbers), and they share a trunk: one global scale lets the widest-spread
        layout speak loudest for weights every layout uses."""
        normalised = (advantages - advantages.mean()) / (advantages.std() + 1e-8)
        if not self.config.per_layout_advantages or len(self.layouts) < 2:
            return normalised

        for index in torch.unique(layout).tolist():
            rows = layout == int(index)
            if int(rows.sum()) < self.config.min_layout_rows:
                continue  # too few to measure a spread with; the rollout's own is the better estimate

            group = advantages[rows]
            normalised[rows] = (group - group.mean()) / (group.std() + 1e-8)

        return normalised

    # ------------------------------------------------------------------ checkpoints

    def _chunk_length(self, steps: int, auxiliary) -> int:
        """The replay's chunk length for a rollout of `steps` (0 = unchunked): the largest divisor of `steps` at most
        mappo.chunk_length."""
        wanted = self.config.chunk_length
        # A distiller still teaching replays its recurrent teachers from zero memory at each sequence's start.
        teaching = auxiliary is not None and getattr(auxiliary, "coef", 1.0) >= 1e-4
        if wanted <= 0 or wanted >= steps or teaching:
            return 0
        return max(length for length in range(1, wanted + 1) if steps % length == 0)

    def _update_recurrent(self, buffer: RolloutBuffer, auxiliary=None, sync: bool = True) -> dict[str, float]:
        """One PPO update that replays the rollout in order, so the GRU learns what to remember.

        Minibatches are envs rather than rows: every step of an env is replayed from the memory its first decision was
        taken with, cleared wherever an episode ended, and the losses are averaged over the rows that are samples. The
        critic has the global state and stays feed-forward, so it is fitted on the same rows in one pass.
        """
        cfg = self.config
        started = time.perf_counter()
        stats = {"policy_loss": 0.0, "value_loss": 0.0, "entropy": 0.0, "clip_frac": 0.0, "approx_kl": 0.0,
                 "actor_grad_norm": 0.0, "critic_grad_norm": 0.0}
        if self.goal_count:
            stats["goal_entropy"] = 0.0
        foresight = self.foresight_outputs > 0 and buffer.foresight >= self.foresight_outputs
        if foresight:
            stats["foresight_loss"] = 0.0

        # The host copy stays at hand: each minibatch's layout groups and GRU pieces are cut from it, so neither has to
        # be read back from the device.
        host = buffer.sequences()
        data = {name: torch.as_tensor(value, device=self.train_device) for name, value in host.items()}
        chunk_length = self._chunk_length(host["actions"].shape[0], auxiliary)
        if chunk_length:
            # [T, E, ...] as [L, T/L * E, ...]: chunk k of env e is "env" k * E + e, replayed from memory[k * L, e].
            # Everything after this sees a rollout of L steps over that many envs.
            host = {name: chunked(value, chunk_length) for name, value in host.items() if name in ("layout", "dones")}
            data = {name: chunked(value, chunk_length) for name, value in data.items()}
        if self.goal_count and "goal" not in data:
            raise ValueError("the actor chooses goals but the rollout buffer did not keep them")
        steps, envs, agents = data["actions"].shape
        valid = data["valid"]
        if not bool(valid.any()) and not self.ranks.active:
            return stats  # nothing to learn from -- alone; with other ranks this one still joins every all-reduce

        rows = valid.reshape(-1)
        flat_layout = data["layout"].reshape(-1)
        advantages = data["advantages"].reshape(-1).clone()
        advantages[rows] = self._normalise_advantages(advantages[rows], flat_layout[rows])
        data["advantages"] = advantages.reshape(steps, envs, agents)

        if self.value_norm is not None:
            self.value_norm.update(data["returns"].reshape(-1)[rows], self.ranks)
            returns_target = self.value_norm.normalize(data["returns"])
            old_values = self.value_norm.normalize(data["values"])
        else:
            returns_target, old_values = data["returns"], data["values"]

        returns, values = data["returns"].reshape(-1)[rows], data["values"].reshape(-1)[rows]
        variance = returns.var()
        explained = 1.0 - (returns - values).var() / variance if float(variance) > 0.0 else torch.zeros(())

        totals = {name: torch.zeros((), device=self.train_device) for name in stats}
        layout_totals: dict = {}
        splits = max(1, min(cfg.minibatches, envs))
        auxiliary_stats: dict[str, float] = {}
        auxiliary_updates = 0
        updates = 0
        epochs_run = 0
        main = torch.cuda.current_stream(self.train_device) if self.train_device.type == "cuda" else None

        for _ in range(cfg.epochs):
            epoch_kl = torch.zeros((), device=self.train_device)
            epoch_updates = 0
            # Shuffled on the host, where the minibatch's groups are cut, rather than on the device and read back.
            order = torch.randperm(envs)
            for chunk_host in torch.tensor_split(order, splits):
                chunk = to_device(chunk_host, self.train_device)
                picked = chunk_host.numpy()
                envs_here = len(chunk_host)
                rows_here = envs_here * agents
                lead = (steps, envs_here, agents)

                # Every decision of the sequence through the adapters and the trunk in one pass, then the GRU over
                # them in order: the heavy layers run once for the whole minibatch instead of once per step.
                obs_all = data["obs"][:, chunk].reshape(-1, data["obs"].shape[-1])
                layout_all = data["layout"][:, chunk].reshape(-1)
                mask_all = data["mask"][:, chunk].reshape(-1, data["mask"].shape[-1])
                goal_all = data["goal"][:, chunk].reshape(-1) if self.goal_count else None
                dones_all = (data["dones"][:, chunk][:, :, None].expand(steps, envs_here, agents)
                             .reshape(steps, rows_here))

                # Shared by the actor's adapters and heads and the critic's adapters: the same rows in the same order.
                groups = per_layout_host(host["layout"][:, picked], len(self.layouts), self.train_device)
                dones_host = torch.from_numpy(np.repeat(host["dones"][:, picked], agents, axis=1))
                counted = valid[:, chunk].to(torch.float32)
                weight = counted.sum().clamp(min=1.0)

                # The actor's half of the minibatch on one stream, the critic's on another: they share only these
                # inputs, and each replays its GRU through every step, a chain of small kernels the GPU cannot
                # overlap with itself but can with the other. The minibatch's statistics wait for both.
                self._switch_stream(self._update_streams[0], wait_for=main)

                encoded = self.actor.encode(obs_all, layout_all, groups).reshape(steps, rows_here, -1)
                memory = data["memory"][0][chunk].reshape(rows_here, -1)
                carried = self.actor.carry(encoded, memory, dones_host)
                features = carried.reshape(-1, carried.shape[-1])

                dist = self.actor.action_distribution(features, layout_all, mask_all, goal_all, groups)
                log_probs = dist.log_prob(data["actions"][:, chunk].reshape(-1)).reshape(*lead)
                action_entropies = dist.entropy().reshape(*lead)
                entropies = action_entropies
                predictions = (self.actor.foresight(features).reshape(*lead, self.foresight_outputs)
                               if foresight else None)
                goal_entropies = None
                if self.goal_count:
                    goals = self.actor.goal_distribution(features)
                    chosen = data["goal_chosen"][:, chunk].reshape(-1).to(features.dtype)
                    log_probs = log_probs + (goals.log_prob(goal_all) * chosen).reshape(*lead)
                    # Zero on the rows that held their goal, and averaged below over every row: the goal head's
                    # bonus is worth the share of decisions that actually choose a goal.
                    goal_entropies = (goals.entropy() * chosen).reshape(*lead)
                    entropies = entropies + goal_entropies

                # The teachers' own memories follow the same replayed decisions as the student's (animus.distill),
                # so distillation is the one part that stays a loop over the sequence.
                teach = auxiliary if auxiliary is not None and hasattr(auxiliary, "sequence_loss") else None
                distill_loss = torch.zeros((), device=self.train_device)
                distill_rows = 0
                if teach is not None:
                    # The whole chunk in one call. A recurrent teacher still sees the decisions in order, but only
                    # its GRU cell runs per step: replaying every teacher's adapters and trunk decision by decision
                    # cost stage27_crossroads a 306 s update against a 6 s rollout, with six teachers.
                    state_all = (data["state"][:, chunk][:, :, None, :]
                                 .expand(steps, envs_here, agents, data["state"].shape[-1])
                                 .reshape(steps, rows_here, -1))
                    taught = teach.sequence_loss(
                        obs_all.reshape(steps, rows_here, -1), state_all,
                        layout_all.reshape(steps, rows_here), mask_all.reshape(steps, rows_here, -1),
                        dist.logits.reshape(steps, rows_here, -1), dones_all)
                    if taught is not None:
                        distill_loss, distill_rows = taught

                taken = data["log_probs"][:, chunk]
                if self.goal_count:
                    taken = taken + data["goal_log_probs"][:, chunk] * data["goal_chosen"][:, chunk].to(taken.dtype)
                ratio = (log_probs - taken).exp()
                advantage = data["advantages"][:, chunk]
                clipped_ratio = ratio.clamp(1 - cfg.clip, 1 + cfg.clip)
                policy_loss = -(torch.min(ratio * advantage, clipped_ratio * advantage) * counted).sum() / weight
                entropy = (entropies * counted).sum() / weight
                action_entropy = (action_entropies * counted).sum() / weight

                actor_loss = policy_loss - self.entropy_coef * entropy
                if foresight:
                    targets = data["foresight_targets"][:, chunk]
                    known = data["foresight_valid"][:, chunk].to(torch.float32) * counted[..., None]
                    horizons = self.foresight_outputs - 1
                    stacked = predictions
                    errors = torch.cat([
                        nn.functional.smooth_l1_loss(stacked[..., :horizons], targets[..., :horizons],
                                                     reduction="none"),
                        (stacked[..., horizons:] - targets[..., horizons:]) ** 2,
                    ], dim=-1)
                    foresight_loss = (errors * known).sum() / known.sum().clamp(min=1.0)
                    actor_loss = actor_loss + cfg.foresight_coef * foresight_loss
                    totals["foresight_loss"] += foresight_loss.detach()

                if distill_rows:
                    # Already a mean over the sequence's taught decisions (Distiller.sequence_loss divides by
                    # rows_taught), exactly as the flat path's is over a minibatch's. Both paths add it as it comes:
                    # dividing again by the chunk length would scale the coefficient down by rollout_length.
                    actor_loss = actor_loss + distill_loss
                    auxiliary_stats["distill_kl"] = auxiliary_stats.get("distill_kl", 0.0) + float(
                        distill_loss.detach() / max(1e-6, teach.coef))
                    auxiliary_stats["distill_rows"] = auxiliary_stats.get("distill_rows", 0.0) + float(distill_rows)
                    auxiliary_updates += 1

                self.actor_opt.zero_grad()
                actor_loss.backward()
                self.ranks.average_gradients(self.actor.parameters())
                actor_grad = nn.utils.clip_grad_norm_(self.actor.parameters(), cfg.max_grad_norm)
                self.actor_opt.step()

                # The critic carries a memory of its own, so its rows are replayed in order exactly as the
                # actor's are: encode every step in one pass, then walk the GRU through the sequence from the state
                # those decisions were valued with.
                self._switch_stream(self._update_streams[1], wait_for=main)
                obs = data["obs"][:, chunk].reshape(-1, data["obs"].shape[-1])
                state = (data["state"][:, chunk][:, :, None, :]
                         .expand(steps, len(chunk), agents, data["state"].shape[-1])
                         .reshape(-1, data["state"].shape[-1]))
                layout = data["layout"][:, chunk].reshape(-1)
                goal_chunk = data["goal"][:, chunk].reshape(-1) if self.goal_count else None
                encoded_value = self.critic.encode(state, obs, layout, goal_chunk, groups).reshape(steps, rows_here, -1)
                critic_memory = data["critic_memory"][0][chunk].reshape(rows_here, -1)
                predicted = self.critic.values_of(
                    self.critic.carry(encoded_value, critic_memory, dones_host)).reshape(steps, -1, agents)
                previous = old_values[:, chunk]
                target = returns_target[:, chunk]
                bounded = previous + (predicted - previous).clamp(-cfg.value_clip, cfg.value_clip)
                errors = torch.max((predicted - target) ** 2, (bounded - target) ** 2)
                value_loss = (errors * counted).sum() / weight

                self.critic_opt.zero_grad()
                (cfg.value_coef * value_loss).backward()
                self.ranks.average_gradients(self.critic.parameters())
                critic_grad = nn.utils.clip_grad_norm_(self.critic.parameters(), cfg.max_grad_norm)
                self.critic_opt.step()

                # Back on the update's own stream, once both halves are in.
                self._switch_stream(main, wait_for=self._update_streams)
                with torch.no_grad():
                    log_ratio = log_probs - taken
                    self._layout_totals(layout_totals, layout_all, action_entropies, (ratio - 1) - log_ratio,
                                        counted)
                    kl = (((ratio - 1) - log_ratio) * counted).sum() / weight
                    totals["policy_loss"] += policy_loss.detach()
                    totals["value_loss"] += value_loss.detach()
                    totals["entropy"] += action_entropy.detach()
                    if goal_entropies is not None:
                        chose = (data["goal_chosen"][:, chunk].to(torch.float32) * counted).sum().clamp(min=1.0)
                        totals["goal_entropy"] += (goal_entropies * counted).sum().detach() / chose
                    totals["clip_frac"] += ((((ratio - 1).abs() > cfg.clip).to(torch.float32)
                                             * counted).sum() / weight)
                    totals["approx_kl"] += kl
                    totals["actor_grad_norm"] += actor_grad
                    totals["critic_grad_norm"] += critic_grad
                    epoch_kl += kl
                updates += 1
                epoch_updates += 1

            epochs_run += 1
            # Every rank stops on the same epoch: on the ranks' mean KL, not its own.
            if cfg.target_kl > 0.0 and epoch_updates \
                    and float(self.ranks.mean(epoch_kl)) / epoch_updates > EPOCH_KL_TOLERANCE * cfg.target_kl:
                break

        self._finish_layout_stats(layout_totals)
        for name in stats:
            stats[name] = float(totals[name]) / max(1, updates)
        stats["explained_variance"] = float(explained)
        stats["epochs_run"] = float(epochs_run)
        stats.update(self._goal_stats(data))
        stats.update({name: value / auxiliary_updates for name, value in auxiliary_stats.items()})
        # What the update itself cost, as the flat path reports it.
        # After the epochs, not before: the rollout acted through these statistics, and its stored log_probs are
        # the denominator of every PPO ratio in the loop above. Advancing them first makes the ratio something
        # other than 1 at epoch 0 -- a normaliser shift read as a policy change. Updating here and syncing the
        # rollout copies below keeps the acting and training views of a feature identical, one rollout apart.
        if cfg.normalise_observations:
            flat_obs = data["obs"].reshape(-1, data["obs"].shape[-1])[rows]
            update_norms(self.actor.norms, flat_obs, flat_layout[rows], self.actor.obs_dims, self.ranks)
            update_norms(self.critic.norms, flat_obs, flat_layout[rows], self.critic.obs_dims, self.ranks)
            states = data["state"][:, :, None, :].expand(steps, envs, agents, data["state"].shape[-1])
            self.critic.state_norm.update(states.reshape(-1, data["state"].shape[-1])[rows], self.ranks)

        stats["update_compute_seconds"] = time.perf_counter() - started
        if sync:
            self._sync_rollout()
        return stats

    def state_dict(self) -> dict:
        return {
            "actor": self.actor.state_dict(),
            "critic": self.critic.state_dict(),
            "value_norm": self.value_norm.state_dict() if self.value_norm is not None else None,
            "actor_opt": self.actor_opt.state_dict(),
            "critic_opt": self.critic_opt.state_dict(),
        }

    def load_state_dict(self, state: dict, load_optimizers: bool = True) -> None:
        self.actor.load_state_dict(state["actor"])
        self.critic.load_state_dict(state["critic"])
        if self.value_norm is not None and state.get("value_norm") is not None:
            self.value_norm.load_state_dict(state["value_norm"])
        if load_optimizers:
            self.actor_opt.load_state_dict(state["actor_opt"])
            self.critic_opt.load_state_dict(state["critic_opt"])
        self._sync_rollout()
