"""MAPPO update: PPO-clip actor, clipped value loss on a normalised centralized critic."""

from __future__ import annotations

import copy
import time
from contextlib import nullcontext
from dataclasses import dataclass

import numpy as np
import torch
from torch import nn

from ..device import host
from ..parallel import Ranks
from .buffer import RolloutBuffer
from .networks import (LayoutActor, LayoutCritic, SharedInputDense, _carry_sequence, load_actor_state, log_prob_of,
                       per_layout, per_layout_host, sample_logits, skip_distribution_checks, goal_pair, split_goal_pair,
                       to_device, update_norms)
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
    # a constant rate kept the update growing all run (stage4_duel: approx KL 0.014 -> 0.028, ~20% of samples
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
    # The PPO update's arithmetic: "fp32", or "bf16" -- the networks' products under torch.autocast (the weights,
    # the optimiser and the GRU's memory stay in full precision; the losses are computed in fp32 as autocast does).
    # On stage4_duel's update (RDNA3) bf16 took the update from 0.58 to 0.47 s and the stage from 39,181 to 44,655
    # env steps/s -- and learned worse: fine-tuning from best.pt for 12 minutes, its seeded evaluations were 10.35 and
    # 13.41 at 10M and 20M steps against fp32's 14.20 and 15.36 (standard errors ~0.25), with entropy and approx_kl
    # falling faster. So fp32 it is, unless a stage measures otherwise.
    update_precision: str = "fp32"
    # How data-parallel learners (animus.parallel) keep one policy. "gradients": every optimizer step's gradients are
    # averaged over the ranks, so they train as one learner on the pool -- the right thing on one machine, and ~8
    # Gbit/s a link for stage4_duel's 5.8M parameters across machines. "weights": each rank trains on its own envs
    # with its own optimiser and the ranks' networks are averaged every weight_sync_every updates (local SGD) --
    # one all-reduce of the parameters, which is what a cluster of learners can carry, but every rank waits for the
    # slowest at every one. "async" (animus.async_sync): no collective at all -- each rank trains at its own pace and
    # trades its networks with the leader's in the background every weight_sync_every updates; what a cluster of
    # unequal machines needs.
    rank_sync: str = "gradients"
    weight_sync_every: int = 1
    # A goal head (0 = off): the actor chooses one of goal_count goals every goal_every_decisions and keeps it in
    # between, and its action head is conditioned on it. The chooser then decides on a clock that many times slower
    # than the actions, so the horizon it has to reason over is that many times shorter. The goal is part of the
    # policy's decision: its log probability joins the action's in the PPO ratio on the decisions that chose one.
    goal_count: int = 0
    # The target space of a goal (GoalHead; the sim's GOAL_TARGETS): 1 for goals that name nothing. With more, a goal
    # is kind * goal_targets + target and goal_count counts the kinds.
    goal_targets: int = 1
    goal_every_decisions: int = 16
    # **Two goals and a queue** (next-run plan, Wave 4): 1 is the one goal of before; 4 is a primary, a secondary held
    # beside it and two goals queued behind them (GoalHead.draw, LayoutActor.decide_goals). The learner keeps the
    # queue and promotes its head when the primary ends; ACT carries the two held.
    goal_slots: int = 1
    # **Hindsight** (next-run plan, 3.3): where a decision achieved a goal other than the primary it held -- an enemy
    # killed under Fight about another, health back under Fight -- its action is also trained, as an auxiliary
    # imitation loss of this weight, as if that had been the goal: free goal-following data. Not a PPO term (the
    # relabelled goal was never the behaviour's, so the ratio would mean nothing). Needs goal_slots > 1 (the goal
    # block's achieved columns).
    hindsight_coef: float = 0.0
    # **Action hints**: where the sim writes a suggested action into a layout's hint block (a whole dungeon's support,
    # Instance.WingHint), the action head is also trained to take it, an imitation loss of this weight times the
    # weight the sim wrote (which falls with the support, and is 0 in evaluation). The hint columns are kept out of the
    # networks (attach_blind_columns): the policy is taught the suggestion, never shown it.
    hint_coef: float = 0.0
    # The goal head's share of the entropy bonus, as a factor on what it would get from entropy_coef, falling
    # linearly to goal_entropy_final_fraction of itself over total_env_steps. The action head's exploration and the
    # goal head's are different things: the first keeps the fight's options open, the second keeps the head from
    # ever settling on a plan. Kept at full, the head of the first full run chose close to uniformly all the way
    # through (goal entropy 1.59 of ln 6 = 1.79, goal kept 22% of choices against chance's 17%, 2026-09-28).
    goal_entropy_scale: float = 1.0
    goal_entropy_final_fraction: float = 1.0
    # The entropy bonus of each goal slot after the primary (the secondary and the queue), as a share of the
    # primary's. Summed at full weight over four slots of 12 kinds x 29 targets, the bonus held the head near uniform
    # (goal entropy 10.4-12.2 nats through the next-run trial, 2026-09-30) and actions stopped depending on goals.
    goal_slot_entropy_weight: float = 0.1
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
    # **The two-clock seat** (long-horizon plan, Component D): with slow_goal_size, the goal is chosen by a slow loop of
    # its own -- a GRU of this size that steps only when a goal is chosen -- with its own value head and its own
    # clock: each goal is one transition carrying every reward until the next choice, discounted per goal by
    # slow_goal_gamma (0.993 over ~4 s goals is a horizon of about ten minutes), trained by its own PPO step and
    # optimizer (slow_goal_lr) apart from the actions'. 0 = the goal head reads the fast features and shares the
    # actions' objective, as before.
    slow_goal_size: int = 0
    slow_goal_gamma: float = 0.993
    slow_goal_lambda: float = 0.95
    slow_goal_lr: float = 0.0003
    # Predictions fed back (Component P, layer 2): the foresight head also predicts the seat's own health 2 s and 5 s
    # ahead and whether its goal is reached within 4 s (foresight_obs_targets), and its outputs, detached, are added
    # to what the action head and the slow loop read (foresight_feedback).
    foresight_obs_targets: bool = False
    foresight_feedback: bool = False
    # Goal-level lookahead (Component P, layer 3): per candidate goal, the chance it is reached and how long it takes,
    # learned from the chosen goals' outcomes (lookahead_coef) and read by the goal choice.
    goal_lookahead: bool = False
    lookahead_coef: float = 0.5


#: The foresight's observation targets (MappoConfig.foresight_obs_targets): the seat's health 8 and 20 decisions on
#: (2 s and 5 s at 250 ms), and whether its goal was reached within the next 16 (GoalBlock::OBS_REACHED).
FORESIGHT_OBS_TARGETS = (("health", 8, False), ("health", 20, False), ("goal_reached", 16, True))
#: Where the seat's own health is in every layout that has a core block first (CoreBlock::OBS_HEALTH).
CORE_HEALTH_COLUMN = 37


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
    # The two-clock seat's slow memory [E, A, S] (MappoConfig.slow_goal_size), stepped only when a goal is chosen.
    slow_memory: np.ndarray | None = None
    # The goals queued behind the two held [E, A, goal_slots - 2] (-1 none): the learner's own (decide_goals).
    queue: np.ndarray | None = None

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
        if self.queue is not None:
            self.queue[done] = -1
        if self.slow_memory is not None:
            self.slow_memory[done] = 0.0
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
        self.goal_chosen_at = None
        self.slow_before_at = self.slow_after_at = self.slow_value_at = None
        self.chosen = None
        self.goal_at = self.goal_log_prob_at = self.foresight_at = self.memory_at = None
        self.goal_slots_at = self.queue_at = None
        self.actions_at = self.log_probs_at = None

    def finish(self, fetched: list[np.ndarray]):
        goals = None
        if self.goal_at is not None:
            goal = fetched[self.goal_at]
            # The clock's choices and the ones an ended goal forced (GoalBlock::OBS_ENDED): both start the count again.
            chosen = fetched[self.goal_chosen_at].astype(bool)
            self.state.age = np.where(chosen, 1, self.state.age + 1)
            slow_before = slow_value = None
            if self.slow_before_at is not None:
                slow_before, slow_value = fetched[self.slow_before_at], fetched[self.slow_value_at]
                self.state.slow_memory = fetched[self.slow_after_at]
            goals = (goal, fetched[self.goal_log_prob_at], chosen, slow_before, slow_value,
                     fetched[self.goal_slots_at])
            self.state.goal = goal
            if self.state.queue is not None:
                self.state.queue = fetched[self.queue_at]
        if self.memory_at is not None:
            self.state.memory = fetched[self.memory_at]
        taken = fetched[self.actions_at]
        if self.chosen is not None:
            self.state.action = taken
        foresight = fetched[self.foresight_at] if self.foresight_at is not None else None
        return taken, fetched[self.log_probs_at], foresight, goals, self.chosen


class _Packed:
    """Named arrays laid out in one pinned host buffer and one device buffer, so moving them all between the two
    is one copy: in a captured rollout decision each copy is a DMA with its own setup latency, and there were
    seventeen. `host[name]` and `device[name]` are typed views into them."""

    ALIGN = 16

    def __init__(self, specs: list[tuple[str, tuple[int, ...], torch.dtype]], device):
        offsets, size = {}, 0
        for name, shape, dtype in specs:
            offsets[name] = size
            nbytes = int(np.prod(shape)) * torch.empty((), dtype=dtype).element_size()
            size += -(-nbytes // self.ALIGN) * self.ALIGN
        self.host_bytes = torch.zeros(max(size, self.ALIGN), dtype=torch.uint8, pin_memory=True)
        self.device_bytes = torch.zeros_like(self.host_bytes, device=device)

        def views(buffer):
            out = {}
            for name, shape, dtype in specs:
                nbytes = int(np.prod(shape)) * torch.empty((), dtype=dtype).element_size()
                out[name] = buffer[offsets[name]:offsets[name] + nbytes].view(dtype).view(shape)
            return out

        self.host, self.device = views(self.host_bytes), views(self.device_bytes)

    def upload(self) -> None:
        self.device_bytes.copy_(self.host_bytes, non_blocking=True)

    def download(self) -> None:
        self.host_bytes.copy_(self.device_bytes, non_blocking=True)


class _RolloutGraph:
    """One rollout decision (MappoTrainer.act_and_value with an acting state) captured as a CUDA / HIP graph for one
    batch shape: upload from fixed pinned buffers, the actor (goal, actions, foresight, memory) and the critic
    (value, memory), and the download of every result to fixed pinned buffers. A replay is one launch and one wait.

    The graph reads the rollout networks' tensors where they were at capture: _sync_rollout copies the weights in
    place and DenseLayouts.refresh keeps the dense matrices where they are, so a replay always uses the latest
    weights. Sampling draws from the device generator, which the capture registers (fresh draws each replay)."""

    def __init__(self, trainer: "MappoTrainer", envs: int, agents: int, obs, mask, state_features,
                 deterministic: bool, device_fed: bool = False):
        self.trainer, self.envs, self.agents, self.deterministic = trainer, envs, agents, deterministic
        # Fed from the device (the sim's buffers, protocol 15): the observation, mask and state are copied device to
        # device into the graph's inputs before a replay, and only the small host inputs are uploaded by it.
        self.device_fed = device_fed
        device, rows = trainer.rollout_device, envs * agents
        recurrent, goals = trainer.recurrent_size, trainer.goal_count

        # The large inputs, and the small ones: each a pinned host buffer the caller fills and a device buffer the graph
        # uploads it into -- the large ones only when they came from the host.
        self.large = _Packed([("obs", (envs, agents, obs.shape[-1]), torch.float32),
                              ("mask", (envs, agents, mask.shape[-1]), torch.bool),
                              ("state", (envs, state_features.shape[-1]), torch.float32)], device)
        specs = [("layout", (envs, agents), torch.long)]
        if recurrent:
            specs += [("memory", (envs, agents, recurrent), torch.float32),
                      ("critic_memory", (envs, agents, recurrent), torch.float32)]
        if goals:
            specs += [("goal", (envs, agents), torch.long), ("chosen", (envs, agents), torch.bool),
                      ("queue", (envs, agents, max(1, trainer.goal_slots - 2)), torch.long)]
        if trainer.slow_goal_size:
            specs += [("slow_memory", (envs, agents, trainer.slow_goal_size), torch.float32)]
        self.inputs = _Packed(specs, device)
        self.host_in = {**self.inputs.host, **self.large.host}
        self.outputs: _Packed | None = None     # laid out by the first warm-up, once the results' shapes are known

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
        self.inputs.upload()
        if not self.device_fed:
            self.large.upload()
        inputs = {**self.inputs.device, **self.large.device}

        obs_t = inputs["obs"].reshape(rows, -1)
        layout_t = inputs["layout"].reshape(rows)
        mask_t = inputs["mask"].reshape(rows, -1)
        memory = inputs["memory"].reshape(rows, -1) if "memory" in inputs else None
        state_t = inputs["state"][:, None, :].expand(envs, agents, inputs["state"].shape[-1]).reshape(rows, -1)

        # One stream, in order. A side stream for the critic's branch ran them side by side and measured faster alone,
        # but slower in training (47,144 -> 35,352 env steps/s): ROCm maps streams onto a few hardware queues, and
        # beside the update's two the side stream's kernels queued behind the update's.
        critic_hidden = critic.state_encoder(critic.state_norm(state_t))
        # Both networks' adapters read the observation: one product (SharedInputDense), the critic's half handed over.
        actor_own, critic_own = trainer._shared_adapters(obs_t, layout_t)
        features = actor.features_from(actor.trunk(actor_own), memory)

        out: dict[str, torch.Tensor] = {}
        goal_t = None
        # The draws as Categorical makes them, in a few kernels each (sample_logits): they were two thirds of the
        # decision's kernels.
        if trainer.goal_count:
            # The goal decision (LayoutActor.decide_goals): the queue promoted, the clock, the goal block's ended and
            # event, and the director's primary; masked by what the goal block says is there.
            goal_features = features
            slow_before = None
            if trainer.slow_goal_size:
                # The slow loop steps where a goal is chosen, and the goals are drawn from where it stepped to.
                slow_before = inputs["slow_memory"].reshape(rows, trainer.slow_goal_size)
                goal_features = actor.slow_step(features, slow_before)
            decided = actor.decide_goals(goal_features, obs_t, layout_t, inputs["goal"].reshape(rows),
                                         inputs["queue"].reshape(rows, -1), inputs["chosen"].reshape(rows),
                                         self.deterministic)
            chosen_t = decided["chosen"]
            if slow_before is not None:
                out["slow_memory"] = torch.where(chosen_t[:, None], goal_features, slow_before).reshape(
                    envs, agents, trainer.slow_goal_size)
                out["slow_before"] = slow_before.reshape(envs, agents, trainer.slow_goal_size)
                out["slow_value"] = actor.slow_value(goal_features).reshape(envs, agents)
            goal_t = decided["goal"]
            out["goal"] = goal_t.reshape(envs, agents)
            out["goal_log_prob"] = decided["log_prob"].reshape(envs, agents)
            out["goal_chosen"] = chosen_t.reshape(envs, agents)
            out["goal_slots"] = decided["slots"].reshape(envs, agents, -1)
            out["queue"] = decided["queue"].reshape(envs, agents, -1)

        critic_memory = inputs["critic_memory"].reshape(rows, -1) if "critic_memory" in inputs else None
        values, carried = critic.step_encoded(critic.encode_goal(critic_hidden, critic_own, goal_t), (rows,),
                                              critic_memory)
        if trainer._rollout_value_norm is not None:
            values = trainer._rollout_value_norm.denormalize(values)
        out["values"] = values.reshape(envs, agents)
        if critic_memory is not None:
            out["critic_memory"] = carried.reshape(envs, agents, trainer.recurrent_size)

        logits = actor.action_logits(features, layout_t, mask_t, goal_t, None, obs_t)
        actions, log_probs = sample_logits(logits, self.deterministic)
        out["actions"] = actions.reshape(envs, agents)
        out["log_probs"] = log_probs.reshape(envs, agents)
        if trainer.foresight_outputs:
            out["foresight"] = actor.foresight(features).reshape(envs, agents, trainer.foresight_outputs)
        if memory is not None:
            out["memory"] = features.reshape(envs, agents, trainer.recurrent_size)

        if self.outputs is None:
            self.outputs = _Packed([(name, tuple(value.shape), value.dtype) for name, value in out.items()],
                                   trainer.rollout_device)
        # Gathered into the output buffer on the device (small copy kernels), then down in one transfer.
        for name, value in out.items():
            self.outputs.device[name].copy_(value)
        self.outputs.download()

    def run(self, obs, mask, layout, state_features, state: "ActingState"):
        """One decision: fill the inputs, replay, wait once. Returns the act_and_value tuple and updates `state`."""
        trainer = self.trainer
        host = self.host_in
        if self.device_fed:
            # On the rollout stream, ahead of the replay that reads them.
            large = self.large.device
            large["obs"].copy_(obs)
            large["mask"].copy_(mask)
            large["state"].copy_(state_features)
        else:
            np.copyto(host["obs"].numpy(), obs)
            np.copyto(host["mask"].numpy(), mask)
            np.copyto(host["state"].numpy(), state_features)
        np.copyto(host["layout"].numpy(), layout, casting="unsafe")
        if trainer.recurrent_size:
            np.copyto(host["memory"].numpy(), state.memory)
            np.copyto(host["critic_memory"].numpy(), state.critic_memory)
        chosen = None
        if trainer.goal_count:
            # A goal is chosen on its own clock and kept in between; a cleared state (a new episode) chooses at once.
            chosen = (state.age % max(1, trainer.config.goal_every_decisions)) == 0
            np.copyto(host["goal"].numpy(), state.goal, casting="unsafe")
            np.copyto(host["chosen"].numpy(), chosen)
            if state.queue is not None:
                np.copyto(host["queue"].numpy(), state.queue, casting="unsafe")
            else:
                host["queue"].numpy().fill(-1)
        if trainer.slow_goal_size:
            np.copyto(host["slow_memory"].numpy(), state.slow_memory)

        self.graph.replay()
        trainer._rollout_stream.synchronize()
        # Copies: the pinned outputs are overwritten by the next replay.
        fetched = {name: value.numpy().copy() for name, value in self.outputs.host.items()}

        goals = None
        if trainer.goal_count:
            # The clock's choices and the ones an ended goal forced: both start the count again.
            chosen = fetched["goal_chosen"].astype(bool)
            state.age = np.where(chosen, 1, state.age + 1)
            state.goal = fetched["goal"]
            if state.queue is not None:
                state.queue = fetched["queue"]
            slow_before = slow_value = None
            if trainer.slow_goal_size:
                slow_before, slow_value = fetched["slow_before"], fetched["slow_value"]
                state.slow_memory = fetched["slow_memory"]
            goals = (fetched["goal"], fetched["goal_log_prob"], chosen, slow_before, slow_value,
                     fetched["goal_slots"])
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
        director=None,
    ):
        """layouts: (obs dim, action count) per agent layout, in the sim's layout order. `slow_layout` is the
        index of config.slow_layout among them, resolved by the caller (layouts carry no names here); -1 when the
        run has none. `director` is (the director layout's index, stage.json's "director"): its members and
        enemies are read as sets, and its turns are the sim's (the "may_call" column)."""
        skip_distribution_checks()
        self.config = config
        self.layouts = list(layouts)
        self.slow_layout = slow_layout if config.slow_layout else -1
        self.director = director
        # The column whose flag says a slow layout's agent may choose now (the sim decides its turns); -1 = its clock.
        self.slow_choose_column = (int(director[1].get("may_call", -1))
                                   if director is not None and director[0] == self.slow_layout else -1)
        # Data-parallel learners (animus.parallel.Ranks): gradients and statistics reduced across them; alone, none.
        self.ranks = ranks if ranks is not None else Ranks()
        self.state_dim = state_dim
        self.train_device = torch.device(train_device)
        self.rollout_device = torch.device(rollout_device)
        # Starts at config.entropy_coef; the stage controller raises it after a restart (animus.stage).
        self.entropy_coef = config.entropy_coef
        # The goal head's factor on it (goal_entropy_scale, annealed by the learner loop every update).
        self.goal_entropy_factor = config.goal_entropy_scale

        hidden = list(config.hidden)
        self.foresight_outputs = ((len(config.foresight_horizons_seconds) + 1
                                   + (len(FORESIGHT_OBS_TARGETS) if config.foresight_obs_targets else 0))
                                  if config.foresight_coef > 0.0 else 0)
        self.recurrent_size = config.recurrent_size
        # config.goal_count is the goal kinds and config.goal_targets the target space (GoalHead); goal_count here is
        # the joint count, which the buffers, the wire and the stats count.
        self.goal_kinds = config.goal_count
        self.goal_targets = max(1, config.goal_targets)
        self.goal_count = self.goal_kinds * self.goal_targets if self.goal_kinds else 0
        self.slow_goal_size = config.slow_goal_size if self.goal_count else 0
        self.goal_slots = max(1, config.goal_slots) if self.goal_count else 1
        self.hint_at: torch.Tensor | None = None
        # The slots are drawn and scored by the slow loop's own update (_update_goals); a queue needs two slots behind
        # the pair held.
        if self.goal_slots > 1 and (not self.slow_goal_size or self.goal_slots < 3):
            raise ValueError("mappo.goal_slots > 1 needs mappo.slow_goal_size and at least 3 slots (a queue)")
        self.actor = LayoutActor(self.layouts, hidden, self.foresight_outputs, self.recurrent_size,
                                 self.goal_kinds, self.goal_targets, self.slow_goal_size, config.foresight_feedback,
                                 config.goal_lookahead, director, self.goal_slots).to(self.train_device)
        if self.actor.goal_head is not None:
            self.actor.goal_head.slot_entropy_weight = config.goal_slot_entropy_weight
        self.critic = LayoutCritic(state_dim, self.layouts, hidden, self.goal_kinds,
                                   self.recurrent_size, self.goal_targets, director,
                                   self.goal_slots).to(self.train_device)
        self.value_norm = (ValueNorm(beta=config.value_norm_beta).to(self.train_device)
                           if config.use_value_norm else None)

        self.reset_optimizers()

        self._rollout_stream = (torch.cuda.Stream(device=self.rollout_device, priority=-1)
                                if self.rollout_device.type == "cuda" else None)
        # The recurrent update's actor and critic halves (_update_recurrent); None off the GPU.
        self._update_streams = (tuple(torch.cuda.Stream(device=self.train_device) for _ in range(2))
                                if self.train_device.type == "cuda" else (None, None))
        self._rollout_graphs: dict[tuple, _RolloutGraph] = {}
        #: The last act_and_value's obs, mask and state on the device, when a device-fed graph took them (else None).
        self.device_inputs: dict[str, torch.Tensor] | None = None
        self._shared_adapters: SharedInputDense | None = None   # the rollout graph's actor + critic adapters
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

    def slow_parameters(self) -> list[torch.nn.Parameter]:
        """The two-clock seat's slow loop (its GRU, the goal head, its value head): trained by the slow goal update
        with an optimizer of their own, and by nothing else."""
        if not self.slow_goal_size:
            return []
        actor = self.actor
        return [*actor.slow_memory.parameters(), *actor.goal_head.parameters(), *actor.slow_value.parameters()]

    def reset_optimizers(self) -> None:
        """Fresh Adam state: after a restart the step sizes are no longer shrunk by the old gradient history."""
        slow = {id(parameter) for parameter in self.slow_parameters()}
        self.actor_opt = torch.optim.Adam([p for p in self.actor.parameters() if id(p) not in slow],
                                          lr=self.config.actor_lr, eps=1e-5)
        self.slow_opt = (torch.optim.Adam(self.slow_parameters(), lr=self.config.slow_goal_lr, eps=1e-5)
                         if self.slow_goal_size else None)
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
        fresh = (LayoutActor(self.layouts, hidden, self.foresight_outputs, self.recurrent_size, self.goal_kinds,
                             self.goal_targets, self.slow_goal_size, self.config.foresight_feedback,
                             self.config.goal_lookahead, self.director, self.goal_slots),
                 LayoutCritic(self.state_dim, self.layouts, hidden, self.goal_kinds, self.recurrent_size,
                              self.goal_targets, self.director, self.goal_slots))
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
    def set_goal_space(self, stage: dict | None, layout_names: list[str]) -> None:
        """The goal space the sim wrote (stage.json "goals" and each layout's blocks): which targets each kind
        accepts, and where each layout's goal block starts, so the goal head is masked by what is there. A run
        whose stage has no goal space keeps an unmasked head."""
        if not self.goal_count or self.goal_targets <= 1 or not stage or "goals" not in stage:
            return
        goals = stage["goals"]
        if len(goals["kinds"]) != self.goal_kinds or int(goals["targets"]) != self.goal_targets:
            raise ValueError(f"the sim's goal space is {len(goals['kinds'])} kinds x {goals['targets']} targets, "
                             f"the learner's {self.goal_kinds} x {self.goal_targets} (mappo.goal_count, goal_targets)")
        block_at = []
        for layout in layout_names:
            blocks = ((stage.get("layouts") or {}).get(layout) or {}).get("blocks") or []
            at = next((int(b["obs"][0]) for b in blocks if b.get("name") == goals.get("block", "goal")), -1)
            block_at.append(at)
        for actor in (self.actor, self._rollout_actor):
            if actor is not None and actor.goal_head is not None:
                actor.goal_head.set_space(goals["accepts"], block_at)

    def set_hint_space(self, stage: dict | None, layout_names: list[str]) -> None:
        """Where each layout's hint block is (stage.json layouts' blocks named "hint"): its two columns, the suggested
        action and its weight, are read by the imitation loss and kept out of both networks."""
        from .networks import attach_blind_columns
        at = []
        for layout in layout_names:
            blocks = (((stage or {}).get("layouts") or {}).get(layout) or {}).get("blocks") or []
            at.append(next((int(b["obs"][0]) for b in blocks if b.get("name") == "hint"), -1))
        self.hint_at = torch.tensor(at, dtype=torch.long, device=self.train_device) if any(a >= 0 for a in at) \
            else None
        if self.hint_at is None:
            return
        columns = {index: [a, a + 1] for index, a in enumerate(at) if a >= 0}
        for network in (self.actor, self.critic):
            if network is not None and hasattr(network, "adapters"):
                attach_blind_columns(network, columns)
        self._sync_rollout()

    def _hint_loss(self, dist, obs: torch.Tensor, layout: torch.Tensor, valid: torch.Tensor, stats: dict):
        """The imitation term for the rows with a hint: -log p(hinted action) weighted by the sim's weight, over the
        weight; None where no row has one."""
        if self.config.hint_coef <= 0.0 or getattr(self, "hint_at", None) is None:
            return None
        at = self.hint_at[layout.long()]
        rows = (at >= 0) & valid
        if not bool(rows.any()):
            return None
        index = torch.arange(obs.shape[0], device=obs.device)
        safe = at.clamp(min=0)
        action = obs[index, safe].round().long()
        weight = obs[index, safe + 1].float()
        hinted = rows & (action > 0) & (weight > 0)
        if not bool(hinted.any()):
            return None
        log_prob = dist.log_prob(torch.where(hinted, action, torch.zeros_like(action)))
        w = weight * hinted.float()
        loss = -(log_prob * w).sum() / w.sum().clamp(min=1e-6)
        with torch.no_grad():
            match = (dist.logits.argmax(-1) == action) & hinted
            stats["hint_loss"] = stats.get("hint_loss", 0.0) + float(loss.detach())
            stats["hint_match"] = stats.get("hint_match", 0.0) + float(match.sum() / hinted.sum())
            stats["hint_weight"] = stats.get("hint_weight", 0.0) + float(weight[hinted].mean())
            stats["hint_n"] = stats.get("hint_n", 0.0) + 1.0
        return self.config.hint_coef * loss

    def director_columns_clear(self) -> bool:
        """Whether both networks' director adapters still read nothing from the slot columns."""
        for network in (self.actor, self.critic):
            if getattr(network, "director_sets", None) is not None:
                weight = network.adapters[network.director_index].weight
                if bool((weight[:, network.director_sets.column_mask] != 0).any()):
                    return False
        return True

    def clear_director_columns(self) -> None:
        """Zero the director adapters' slot columns in both networks, and in the rollout copies."""
        from .networks import clear_director_columns
        for network in (self.actor, self.critic):
            clear_director_columns(network)
        self._sync_rollout()

    def _sync_rollout(self) -> None:
        # Copy tensor by tensor into the networks that are already there. Building a state dict and loading it
        # allocates a host copy of every parameter and buffer of both networks after every update, which with a
        # GPU is the whole model over the bus; the rollout copies only ever need the values.
        with torch.no_grad():
            for source, destination in self._rollout_pairs:
                destination.copy_(source)
        # The rollout copies act on 128-row batches, where each normaliser's five elementwise passes cost more than
        # the adapter after it: folded into the adapters, on the copies only.
        self._rollout_actor.fold_normalisation()
        self._rollout_critic.fold_normalisation()
        if self.rollout_device.type == "cuda":
            self._rollout_actor.densify(max(self._rollout_actor.obs_dims))
            self._rollout_critic.densify(max(self._rollout_critic.obs_dims))
            actor_dense, critic_dense = self._rollout_actor.dense_adapters, self._rollout_critic.dense_adapters
            if self._shared_adapters is None:
                self._shared_adapters = SharedInputDense(actor_dense, critic_dense)
            else:
                self._shared_adapters.refresh(actor_dense, critic_dense)
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
                or self.slow_layout >= 0 or self.director is not None):
            return None
        envs, agents = layout.shape
        device_fed = isinstance(obs, torch.Tensor)
        key = (envs, agents, obs.shape[-1], mask.shape[-1], state_features.shape[-1], bool(deterministic), device_fed)
        graph = self._rollout_graphs.get(key)
        if graph is None:
            graph = self._rollout_graphs[key] = _RolloutGraph(self, envs, agents, obs, mask, state_features,
                                                              bool(deterministic), device_fed)
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

    @property
    def rollout_stream(self):
        """The rollout's own GPU stream (None off the GPU)."""
        return self._rollout_stream

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
        obs, mask = host(obs), host(mask)
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
            self.device_inputs = None
            if graph is not None:
                decided = graph.run(obs, mask, layout, state_features, state)
                # The decision's device inputs, where the graph copied them: they stay there, on the rollout stream,
                # until its next replay.
                if graph.device_fed:
                    self.device_inputs = graph.large.device
                return decided
            # Device inputs (protocol 15) are for the captured decision; every other path reads the host's.
            obs, mask, state_features = host(obs), host(mask), host(state_features)
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
            clock = self._tensor((state.age % max(1, self.config.goal_every_decisions)) == 0).reshape(rows).bool()
            goal_features = features
            slow_before = None
            if self.slow_goal_size:
                # The slow loop steps where a goal is chosen, and the goals are drawn from where it stepped to.
                slow_before = self._tensor(state.slow_memory).reshape(rows, self.slow_goal_size)
                goal_features = self._rollout_actor.slow_step(features, slow_before)
            queue = (self._tensor(state.queue, torch.long).reshape(rows, -1) if state.queue is not None
                     else torch.full((rows, 1), -1, dtype=torch.long, device=self.rollout_device))
            goals = self._rollout_actor.decide_goals(goal_features, obs_t, layout_t,
                                                     self._tensor(state.goal, torch.long).reshape(rows), queue,
                                                     clock, deterministic)
            chosen_t = goals["chosen"]
            if slow_before is not None:
                slow_after = torch.where(chosen_t[:, None], goal_features, slow_before)
                decided.slow_before_at = downloads.add(slow_before.reshape(envs, agents, self.slow_goal_size))
                decided.slow_after_at = downloads.add(slow_after.reshape(envs, agents, self.slow_goal_size))
                decided.slow_value_at = downloads.add(
                    self._rollout_actor.slow_value(goal_features).reshape(envs, agents))
            decided.goal_t = goals["goal"]
            decided.goal_log_prob_at = downloads.add(goals["log_prob"].reshape(envs, agents))
            decided.goal_at = downloads.add(decided.goal_t.reshape(envs, agents))
            decided.goal_chosen_at = downloads.add(chosen_t.reshape(envs, agents))
            decided.goal_slots_at = downloads.add(goals["slots"].reshape(envs, agents, -1))
            decided.queue_at = downloads.add(goals["queue"].reshape(envs, agents, -1))

        dist = self._rollout_actor.action_distribution(features, layout_t, mask_t, decided.goal_t, groups, obs_t)
        actions = dist.logits.argmax(dim=-1) if deterministic else dist.sample()
        log_probs = dist.log_prob(actions)

        # A slow layout speaks on its own clock and its call stands in between, so the seats have something
        # steady enough to act on. The log probabilities of the held decisions are the sampled action's and not
        # the held one's, which costs nothing: a held decision is not a sample and never reaches the loss.
        if self.slow_layout >= 0 and state is not None and state.slow_age is not None:
            every = max(1, self.config.slow_every_decisions)
            slow = layout == self.slow_layout
            # The sim's turns where it decides them (the director's "may call"), else the learner's own clock.
            if self.slow_choose_column >= 0:
                choosing = slow & (host(obs)[..., self.slow_choose_column] > 0.5)
            else:
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

        obs = host(obs)
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

    def hindsight_targets(self, buffer: RolloutBuffer) -> None:
        """Fill buffer.achieved: per decision, what the next observation's goal block says it achieved (-1 for
        nothing, and at the rollout's last step and where the episode ended, which have no next observation here)."""
        buffer.achieved.fill(-1)
        head = self.actor.goal_head
        if head is None or head.slots <= 1 or buffer.steps < 2:
            return
        steps, envs, agents = buffer.layout.shape
        with torch.no_grad():
            obs = torch.as_tensor(buffer.obs[1:], device=self.train_device).reshape(-1, buffer.obs.shape[-1])
            layout = torch.as_tensor(buffer.layout[1:], device=self.train_device).reshape(-1)
            achieved = head.signals(obs.float(), layout)["achieved"].reshape(steps - 1, envs, agents).cpu().numpy()
        ended = np.broadcast_to(buffer.dones[:-1, :, None], achieved.shape)
        buffer.achieved[:-1] = np.where(ended, -1, achieved)

    def wire_goals(self, goal: np.ndarray) -> np.ndarray:
        """The goals ACT carries for held goals `goal` [E, A]: [E, A, 2], primary then secondary (-1 none)."""
        goal = np.asarray(goal, dtype=np.int64)
        if self.goal_slots > 1:
            primary, secondary = goal // (self.goal_count + 1), goal % (self.goal_count + 1) - 1
        else:
            primary, secondary = goal, np.full_like(goal, -1)
        return np.stack([primary, secondary], axis=-1).astype(np.int32)

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
            slow_memory=(np.zeros((envs, agents, self.slow_goal_size), dtype=np.float32) if self.slow_goal_size
                         else None),
            queue=(np.full((envs, agents, self.goal_slots - 2), -1, dtype=np.int64) if self.goal_slots > 2
                   else None),
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
        state, obs = host(state), host(obs)
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
        if self.goal_slots > 1:
            goals = torch.div(goals, self.goal_count + 1, rounding_mode="floor")     # the primary of the pair held
        # Per kind (a goal is kind * targets + target), and how many name a target.
        kinds = torch.div(goals.reshape(-1), self.goal_targets, rounding_mode="floor")
        counts = torch.bincount(kinds, minlength=self.goal_kinds).float()
        total = counts.sum().clamp(min=1.0)
        stats = {f"goal_{index}_share": float(counts[index] / total) for index in range(self.goal_kinds)}
        if self.goal_targets > 1:
            stats["goal_targeted_share"] = float(((goals.reshape(-1) % self.goal_targets) != 0).float().mean())

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

    def foresight_obs_columns(self) -> list | None:
        """The foresight's observation targets for RolloutBuffer.finish (MappoConfig.foresight_obs_targets): per
        target, each layout's column (-1 without one), how far ahead, and whether it is "at any point by then". A
        layout counts as a seat's when it has a goal block (set_goal_space); without a goal space, none has."""
        if not self.config.foresight_obs_targets or not self.foresight_outputs:
            return None
        head = self.actor.goal_head
        block_at = (head.block_at.cpu().numpy() if head is not None
                    else np.full(len(self.layouts), -1, dtype=np.int64))
        seats = block_at >= 0
        targets = []
        for name, ahead, window in FORESIGHT_OBS_TARGETS:
            if name == "health":
                columns = np.where(seats, CORE_HEALTH_COLUMN, -1)
            else:
                columns = np.where(seats, block_at + head.kinds + head.targets + 1, -1)
            targets.append((columns.astype(np.int64), ahead, window))
        return targets

    def _update_goals(self, buffer: RolloutBuffer) -> dict[str, float]:
        """The slow goal update (Component D): each seat's goal choices of the rollout replayed in order through
        the slow GRU from the memory the first one started with (reset where an episode ended between two), then a
        PPO step on the slow clock's advantages, the slow value on its returns, and the lookahead on what the chosen
        goals did -- reached before the next choice or not, and how long they ran."""
        cfg = self.config
        chosen = buffer.goal_chosen & buffer.valid
        if not chosen.any():
            if self.ranks.active and cfg.rank_sync == "gradients":
                # Every rank steps together: with nothing of its own to learn from, this one still takes its part
                # of the others' averaged gradients, epoch for epoch.
                for _ in range(cfg.epochs):
                    self.slow_opt.zero_grad(set_to_none=True)
                    self.ranks.average_gradients(self.slow_parameters())
                    self.slow_opt.step()
            return {}
        device = self.train_device
        steps, envs, agents = chosen.shape
        columns = envs * agents
        flat = chosen.reshape(steps, columns)
        position = np.cumsum(flat, axis=0) - 1
        length = int(flat.sum(axis=0).max())
        step_of = np.full((length, columns), -1, dtype=np.int64)
        t_idx, c_idx = np.nonzero(flat)
        step_of[position[t_idx, c_idx], c_idx] = t_idx
        present = step_of >= 0
        env_of = np.broadcast_to((np.arange(columns) // agents)[None, :], step_of.shape)
        agent_of = np.broadcast_to((np.arange(columns) % agents)[None, :], step_of.shape)
        safe = np.maximum(step_of, 0)

        # An episode that ended between two choices (a done in [t_k, t_k+1 - 1]) clears the slow memory before the
        # second. done_count[t] counts the dones of steps before t.
        done_count = np.concatenate([np.zeros((1, envs), np.int64), np.cumsum(buffer.dones, axis=0)])
        ended_between = np.zeros((length, columns), dtype=bool)
        if length > 1:
            before = done_count[safe[:-1], env_of[:-1]]
            after = done_count[safe[1:], env_of[1:]]
            ended_between[:-1] = present[1:] & (after > before)
        # The outcome of each choice, where the next one is inside the rollout and the same episode.
        head = self.actor.goal_head
        block_at = head.block_at.cpu().numpy()
        # The goal block's "reached" column of each layout; a layout without a goal block knows no outcomes.
        reached_column = np.where(block_at >= 0, block_at + head.kinds + head.targets + 1, -1)
        known = np.zeros((length, columns), dtype=bool)
        duration = np.zeros((length, columns), dtype=np.float32)
        if length > 1:
            known[:-1] = present[1:] & ~ended_between[:-1]
            # How long the goal ran, as a share of a minute (240 decisions of 250 ms).
            duration[:-1] = np.where(known[:-1], (step_of[1:] - step_of[:-1]) / 240.0, 0.0)

        def rows(array, index=(safe, env_of, agent_of)):
            if isinstance(array, np.ndarray):
                return torch.as_tensor(array[index], device=device)
            return array[tuple(torch.as_tensor(i, device=array.device) for i in index)].to(device)

        obs = rows(buffer.obs).float().reshape(length * columns, -1)
        layout = rows(buffer.layout).long().reshape(-1)
        goal = rows(buffer.goal).long().reshape(-1)
        # With two goals and a queue: the slots each choice drew (scored again below), the primary as held for the
        # lookahead's outcome, and whether the primary was the director's (not drawn, so not scored).
        slots = rows(buffer.goal_slots).long().reshape(length * columns, -1) if head.slots > 1 else None
        if slots is not None:
            goal = split_goal_pair(goal, head.count)[0]
            signals = head.signals(obs, layout)
            given, order_goal = signals["from_order"], signals["order_goal"]
        old_log_prob = rows(buffer.goal_log_probs).float().reshape(-1)
        advantages = rows(buffer.slow_advantages).float().reshape(-1)
        returns = rows(buffer.slow_returns).float().reshape(-1)
        memory = rows(buffer.memory).float().reshape(length * columns, -1) if self.recurrent_size else None
        first = torch.as_tensor(buffer.slow_memory[safe[0], env_of[0], agent_of[0]], device=device)
        valid = torch.as_tensor(present.reshape(-1), device=device)
        # Only the choices whose span has a target (compute_span_gae): one cut by the rollout's end or a truncation
        # is still replayed, for the memory the next choices carry, but not learned from.
        targeted = rows(buffer.slow_valid).bool().reshape(-1) & valid
        reached = np.zeros((length, columns), dtype=np.float32)
        if length > 1:
            next_rows = (safe[1:], env_of[1:], agent_of[1:])
            next_obs = rows(buffer.obs, next_rows).float()
            next_layout = buffer.layout[next_rows]
            column_np = reached_column[next_layout]
            known[:-1] &= column_np >= 0
            column = torch.as_tensor(np.clip(column_np, 0, next_obs.shape[-1] - 1), device=device)
            reached[:-1] = (next_obs.gather(-1, column[..., None])[..., 0] > 0.5).cpu().numpy()
        known_t = torch.as_tensor(known.reshape(-1), device=device) & valid
        reached_t = torch.as_tensor(reached.reshape(-1), device=device)
        duration_t = torch.as_tensor(duration.reshape(-1), device=device)
        dones_seq = torch.as_tensor(ended_between, device=device)

        with torch.no_grad():
            features = self.actor.features(obs, layout, memory)
            inputs = self.actor.with_foresight(features).reshape(length, columns, -1)

        counted = targeted.float()
        normalised = advantages[targeted]
        advantages = (advantages - normalised.mean()) / (normalised.std() + 1e-8) if normalised.numel() > 1 else advantages
        totals = {"slow_policy_loss": 0.0, "slow_value_loss": 0.0, "goal_entropy": 0.0, "slow_approx_kl": 0.0,
                  "lookahead_loss": 0.0}
        for _ in range(cfg.epochs):
            states = _carry_sequence(self.actor.slow_memory, self.slow_goal_size, inputs, first, dones_seq)
            states = states.reshape(length * columns, -1)
            if slots is not None:
                _, log_prob, entropy_rows = head.draw(states, obs, layout, order_goal, given, False, slots)
            else:
                logits = head.logits(states, obs, layout)
                dist = torch.distributions.Categorical(logits=logits)
                log_prob = dist.log_prob(goal)
                entropy_rows = dist.entropy()
            log_ratio = (log_prob - old_log_prob) * counted
            ratio = log_ratio.exp()
            clipped = ratio.clamp(1 - cfg.clip, 1 + cfg.clip)
            weight = counted.sum().clamp(min=1.0)
            policy_loss = -(torch.min(ratio * advantages, clipped * advantages) * counted).sum() / weight
            entropy = (entropy_rows * counted).sum() / weight
            value = self.actor.slow_value(states).squeeze(-1)
            value_loss = (((value - returns) ** 2) * counted).sum() / weight
            loss = policy_loss - self.entropy_coef * self.goal_entropy_factor * entropy + cfg.value_coef * value_loss
            lookahead_loss = torch.zeros((), device=device)
            if head.lookahead:
                success, expected = head.predictions(states)
                picked = goal[:, None]
                success = success.gather(1, picked)[:, 0]
                expected = expected.gather(1, picked)[:, 0]
                mask = known_t.float()
                lookahead_loss = ((nn.functional.binary_cross_entropy_with_logits(success, reached_t, reduction="none")
                                   + (expected - duration_t) ** 2) * mask).sum() / mask.sum().clamp(min=1.0)
                loss = loss + cfg.lookahead_coef * lookahead_loss
                with torch.no_grad():
                    # How good the goal-level predictions are, on the goals actually chosen: the Brier score of
                    # "reached" against always predicting the rollout's own rate, and the duration's mean error.
                    known_n = mask.sum().clamp(min=1.0)
                    probability = torch.sigmoid(success)
                    base_rate = (reached_t * mask).sum() / known_n
                    # Averaged over the epochs, as the losses are (the last epoch alone was the one reported).
                    totals["lookahead_brier"] = totals.get("lookahead_brier", 0.0) + float(
                        (((probability - reached_t) ** 2) * mask).sum() / known_n) / cfg.epochs
                    totals["lookahead_brier_base"] = totals.get("lookahead_brier_base", 0.0) + float(
                        (((base_rate - reached_t) ** 2) * mask).sum() / known_n) / cfg.epochs
                    totals["lookahead_duration_error"] = totals.get("lookahead_duration_error", 0.0) + float(
                        ((expected - duration_t).abs() * mask).sum() / known_n) / cfg.epochs
                    # Whether the goal chosen is the one the lookahead itself scores best (success and duration
                    # weighed as the logits weigh them): a head that plans from its predictions picks it.
                    all_success, all_duration = head.predictions(states)
                    weight_la = head.lookahead_weight.to(all_success.dtype)
                    planned = (weight_la[0] * all_success + weight_la[1] * all_duration).argmax(dim=-1)
                    chosen_rows = counted > 0
                    totals["goal_best_by_lookahead"] = totals.get("goal_best_by_lookahead", 0.0) + float(
                        (planned == goal)[chosen_rows].float().mean() if bool(chosen_rows.any()) else 0.0) / cfg.epochs
            self.slow_opt.zero_grad(set_to_none=True)
            loss.backward()
            if cfg.rank_sync == "gradients":
                self.ranks.average_gradients(self.slow_parameters())
            nn.utils.clip_grad_norm_(self.slow_parameters(), cfg.max_grad_norm)
            self.slow_opt.step()
            with torch.no_grad():
                totals["slow_policy_loss"] += float(policy_loss) / cfg.epochs
                totals["slow_value_loss"] += float(value_loss) / cfg.epochs
                totals["goal_entropy"] += float(entropy) / cfg.epochs
                totals["slow_approx_kl"] += float((((ratio - 1) - log_ratio) * counted).sum() / weight) / cfg.epochs
                totals["lookahead_loss"] += float(lookahead_loss) / cfg.epochs
        totals["goal_reached_share"] = float(reached_t[known_t].mean()) if bool(known_t.any()) else 0.0
        return totals

    def _foresight_quality(self, predicted, targets, known, horizons: int, out: dict) -> None:
        """Accumulate how good the observation forecasts are (FORESIGHT_OBS_TARGETS), in `out` as sums with a count
        (the update divides): the health forecasts' mean absolute error, as a share of full health, and the Brier
        score of "the goal is reached within 16 decisions"."""
        if not self.config.foresight_obs_targets or predicted.shape[-1] < horizons + len(FORESIGHT_OBS_TARGETS):
            return
        with torch.no_grad():
            for offset, (name, decisions, binary) in enumerate(FORESIGHT_OBS_TARGETS):
                column = horizons + 1 + offset
                if column >= predicted.shape[-1]:
                    continue
                weight = known[..., column]
                count = float(weight.sum())
                if count <= 0.0:
                    continue
                # Trained by squared error on the value itself (not a logit): a probability is the value, clamped.
                guess = predicted[..., column].clamp(0.0, 1.0) if binary else predicted[..., column]
                error = (guess - targets[..., column]) ** 2 if binary else (guess - targets[..., column]).abs()
                key = f"forecast_{name}_{decisions}_{'brier' if binary else 'error'}"
                out[key] = out.get(key, 0.0) + float((error * weight).sum())
                out[key + "_n"] = out.get(key + "_n", 0.0) + count

    # ------------------------------------------------------------------ update

    def update(self, buffer: RolloutBuffer, auxiliary=None, sync: bool = True) -> dict[str, float]:
        """One PPO update over the rollout. `auxiliary(data, idx, dist)` may add a loss to each minibatch's actor
        loss: it returns (loss, {stat: value}) or None (see animus.distill)."""
        if self.config.hindsight_coef > 0.0:
            self.hindsight_targets(buffer)
        precision = self.config.update_precision
        if precision not in ("fp32", "bf16"):
            raise ValueError(f"mappo.update_precision is {precision!r}: fp32 or bf16")
        if self.config.rank_sync not in ("gradients", "weights", "async"):
            raise ValueError(f"mappo.rank_sync is {self.config.rank_sync!r}: gradients, weights or async")
        if precision == "bf16" and self.train_device.type == "cuda":
            with torch.autocast("cuda", dtype=torch.bfloat16):
                stats = self._update(buffer, auxiliary, sync)
        else:
            stats = self._update(buffer, auxiliary, sync)
        # The two-clock seat's goals, on their own clock and optimizer (after the fast update, whose features the
        # slow loop reads detached). In full precision: it is small, and its value head is on the rewards' scale.
        if self.slow_goal_size:
            stats.update(self._update_goals(buffer))
            if sync:
                self._sync_rollout()

        if self.config.rank_sync == "weights" and self.ranks.active:
            self._updates_since_sync = getattr(self, "_updates_since_sync", 0) + 1
            if self._updates_since_sync >= max(1, self.config.weight_sync_every):
                self._updates_since_sync = 0
                sync_started = time.perf_counter()
                self.ranks.average_parameters([self.actor, self.critic])
                # Mostly waiting for the slowest rank to arrive: the all-reduce itself is a few tenths of a second.
                stats["weight_sync_seconds"] = time.perf_counter() - sync_started
                # A serial update copied its weights to the rollout networks already: again, averaged. (An
                # overlapped one is copied when it is joined, after this.)
                if sync:
                    self._sync_rollout()
        return stats

    def _update(self, buffer: RolloutBuffer, auxiliary=None, sync: bool = True) -> dict[str, float]:
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
                dist = self.actor.action_distribution(features, layout, data["mask"][idx], goal, obs=obs)
                predictions = (self.actor.foresight(features) if foresight else None)
                log_probs = dist.log_prob(data["actions"][idx])
                action_entropy = dist.entropy().mean()
                entropy = action_entropy
                if self.goal_count and not self.slow_goal_size:
                    # Choosing a goal is part of the decision that chose it: its log probability joins the action's,
                    # and its entropy is kept up on those decisions too. The goal term is averaged over every row,
                    # not only the rows that chose a goal, so a head consulted once in goal_every_decisions is worth
                    # that share of the bonus rather than as much as the action head on every decision.
                    goals = self.actor.goal_distribution(features, obs, layout)
                    chosen = data["goal_chosen"][idx].float()
                    log_probs = log_probs + goals.log_prob(goal) * chosen
                    goal_entropy = (goals.entropy() * chosen).sum()
                    entropy = entropy + self.goal_entropy_factor * goal_entropy / max(1, chosen.numel())
                    goal_entropy = goal_entropy.detach() / chosen.sum().clamp(min=1.0)
                    data_log_probs = data["log_probs"][idx] + data["goal_log_probs"][idx] * chosen
                else:
                    data_log_probs = data["log_probs"][idx]
                log_ratio = log_probs - data_log_probs
                ratio = log_ratio.exp()

                adv = data["advantages"][idx]
                policy_loss = -torch.min(ratio * adv, ratio.clamp(1 - cfg.clip, 1 + cfg.clip) * adv).mean()

                actor_loss = policy_loss - self.entropy_coef * entropy
                hint_loss = self._hint_loss(dist, obs, layout, torch.ones_like(layout, dtype=torch.bool),
                                            auxiliary_stats)
                if hint_loss is not None:
                    actor_loss = actor_loss + hint_loss
                if foresight:
                    # Huber on the discounted returns, which are on the rewards' scale and have outliers; squared
                    # error on the share of the episode left, which is already 0 to 1. Steps whose episode does not
                    # end inside the rollout have no share to learn, and are left out.
                    targets, valid = data["foresight_targets"][idx], data["foresight_valid"][idx]
                    horizons = len(cfg.foresight_horizons_seconds)
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
                if cfg.rank_sync == "gradients":
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
                if cfg.rank_sync == "gradients":
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
                    if self.goal_count and not self.slow_goal_size:
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
        if "hint_n" in auxiliary_stats:
            count = max(auxiliary_stats.pop("hint_n"), 1.0)
            for name in ("hint_loss", "hint_match", "hint_weight"):
                result[name] = auxiliary_stats.pop(name, 0.0) / count
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

                dist = self.actor.action_distribution(features, layout_all, mask_all, goal_all, groups, obs_all)
                log_probs = dist.log_prob(data["actions"][:, chunk].reshape(-1)).reshape(*lead)
                action_entropies = dist.entropy().reshape(*lead)
                entropies = action_entropies
                predictions = (self.actor.foresight(features).reshape(*lead, self.foresight_outputs)
                               if foresight else None)
                goal_entropies = None
                if self.goal_count and not self.slow_goal_size:
                    goals = self.actor.goal_distribution(features, obs_all, layout_all)
                    chosen = data["goal_chosen"][:, chunk].reshape(-1).to(features.dtype)
                    log_probs = log_probs + (goals.log_prob(goal_all) * chosen).reshape(*lead)
                    # Zero on the rows that held their goal, and averaged below over every row: the goal head's
                    # bonus is worth the share of decisions that actually choose a goal.
                    goal_entropies = (goals.entropy() * chosen).reshape(*lead)
                    entropies = entropies + self.goal_entropy_factor * goal_entropies

                # The teachers' own memories follow the same replayed decisions as the student's (animus.distill),
                # so distillation is the one part that stays a loop over the sequence.
                teach = auxiliary if auxiliary is not None and hasattr(auxiliary, "sequence_loss") else None
                distill_loss = torch.zeros((), device=self.train_device)
                distill_rows = 0
                if teach is not None:
                    # The whole chunk in one call. A recurrent teacher still sees the decisions in order, but only
                    # its GRU cell runs per step: replaying every teacher's adapters and trunk decision by decision
                    # cost stage18_ship a 306 s update against a 6 s rollout, with six teachers.
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
                # The goal's log probability joins the old one only where it joins the new one: with a slow loop the
                # goal is trained on its own clock (_update_goals), and adding it here alone made every goal-choosing
                # row's ratio 1 / p(goal) -- approx_kl 0.2-2 on the first epoch, every update cut to one epoch, and
                # those rows' action gradients clipped or blown up by the goal's probability.
                if self.goal_count and not self.slow_goal_size:
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
                    horizons = len(cfg.foresight_horizons_seconds)
                    stacked = predictions
                    errors = torch.cat([
                        nn.functional.smooth_l1_loss(stacked[..., :horizons], targets[..., :horizons],
                                                     reduction="none"),
                        (stacked[..., horizons:] - targets[..., horizons:]) ** 2,
                    ], dim=-1)
                    foresight_loss = (errors * known).sum() / known.sum().clamp(min=1.0)
                    actor_loss = actor_loss + cfg.foresight_coef * foresight_loss
                    totals["foresight_loss"] += foresight_loss.detach()
                    self._foresight_quality(stacked.detach(), targets, known, horizons, auxiliary_stats)

                if self.goal_count and epoch_updates == 0:
                    # Does the goal change what is done? The greedy action under the goal held against under another
                    # drawn at random, on the first minibatch of each epoch: a goal the actions ignore reads 0.
                    with torch.no_grad():
                        other = torch.randint(0, self.goal_count, goal_all.shape, device=goal_all.device)
                        if self.goal_slots > 1:
                            other = goal_pair(other, torch.full_like(other, -1), self.goal_count)
                        swapped = self.actor.action_distribution(features, layout_all, mask_all, other, groups,
                                                                 obs_all).logits.argmax(-1)
                        rows_valid = valid[:, chunk].reshape(-1)
                        changed = (swapped != dist.logits.argmax(-1))[rows_valid]
                        auxiliary_stats["goal_swap_action_change"] = auxiliary_stats.get(
                            "goal_swap_action_change", 0.0) + float(changed.float().mean() if changed.numel() else 0.0)
                        auxiliary_stats["goal_swap_n"] = auxiliary_stats.get("goal_swap_n", 0.0) + 1.0

                if cfg.hindsight_coef > 0.0 and self.goal_slots > 1 and "achieved" in data:
                    # Hindsight: the rows whose decision achieved something other than the primary held, trained to
                    # take the same action with that as the goal. An imitation term on the rows it covers.
                    achieved = data["achieved"][:, chunk].reshape(-1)
                    primary = split_goal_pair(goal_all, self.goal_count)[0]
                    relabel = (achieved >= 0) & (achieved != primary) & valid[:, chunk].reshape(-1)
                    if bool(relabel.any()):
                        swapped = goal_pair(achieved[relabel], torch.full_like(achieved[relabel], -1),
                                            self.goal_count)
                        relabelled = self.actor.action_distribution(features[relabel], layout_all[relabel],
                                                                    mask_all[relabel], swapped, None,
                                                                    obs_all[relabel])
                        hindsight_loss = -relabelled.log_prob(data["actions"][:, chunk].reshape(-1)[relabel]).mean()
                        actor_loss = actor_loss + cfg.hindsight_coef * hindsight_loss
                        auxiliary_stats["hindsight_loss"] = auxiliary_stats.get("hindsight_loss", 0.0) + float(
                            hindsight_loss.detach())
                        auxiliary_stats["hindsight_rows"] = auxiliary_stats.get("hindsight_rows", 0.0) + float(
                            relabel.sum())

                hint_loss = self._hint_loss(dist, obs_all, layout_all, valid[:, chunk].reshape(-1), auxiliary_stats)
                if hint_loss is not None:
                    actor_loss = actor_loss + hint_loss

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
                if cfg.rank_sync == "gradients":
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
                if cfg.rank_sync == "gradients":
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
        # The forecasts' quality: sums over every minibatch with their counts (_foresight_quality).
        for name in [n for n in auxiliary_stats if n.startswith("forecast_") and not n.endswith("_n")]:
            count = auxiliary_stats.pop(name + "_n", 0.0)
            stats[name] = auxiliary_stats.pop(name) / max(count, 1.0)
        if "goal_swap_n" in auxiliary_stats:
            count = auxiliary_stats.pop("goal_swap_n")
            stats["goal_swap_action_change"] = auxiliary_stats.pop("goal_swap_action_change") / max(count, 1.0)
        # Hints: the imitation loss, how often the greedy action is the hint and the sim's weight, per minibatch.
        if "hint_n" in auxiliary_stats:
            count = max(auxiliary_stats.pop("hint_n"), 1.0)
            for name in ("hint_loss", "hint_match", "hint_weight"):
                stats[name] = auxiliary_stats.pop(name, 0.0) / count
        # Hindsight: the relabelled loss per minibatch that had one, and the rows relabelled over the update.
        if "hindsight_loss" in auxiliary_stats:
            stats["hindsight_rows"] = auxiliary_stats.pop("hindsight_rows")
            stats["hindsight_loss"] = auxiliary_stats.pop("hindsight_loss") / max(1, updates)
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
        load_actor_state(self.actor, state["actor"])
        self.critic.load_state_dict(state["critic"])
        if self.value_norm is not None and state.get("value_norm") is not None:
            self.value_norm.load_state_dict(state["value_norm"])
        if load_optimizers:
            self.actor_opt.load_state_dict(state["actor_opt"])
            self.critic_opt.load_state_dict(state["critic_opt"])
        self._sync_rollout()
