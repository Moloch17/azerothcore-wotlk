"""Rollout storage and GAE for auto-resetting vectorised envs.

Everything is numpy on the CPU -- at [T, E, A] = [128, 64, 1] the whole rollout is a few MB, and GAE
is a single backwards loop over T -- except obs, state and mask when the sim writes them into device
memory (protocol 15, animus.device): then they are kept on that device, where the update reads them,
and never cross to the host at all.
"""

from __future__ import annotations

import numpy as np

# Kept where the sim put them when it put them on the device.
DEVICE_FIELDS = ("obs", "state", "mask")


def compute_gae(
    rewards: np.ndarray,
    values: np.ndarray,
    dones: np.ndarray,
    terminated: np.ndarray,
    final_values: np.ndarray,
    last_values: np.ndarray,
    gamma: float,
    gae_lambda: float,
) -> tuple[np.ndarray, np.ndarray]:
    """Generalised advantage estimation with correct time-limit handling.

    Shapes: rewards, values, final_values [T, E, A]; dones, terminated [T, E] (broadcast over
    agents); last_values [E, A] = V of the observation that follows the final step.

    For step t the successor value is:
      - values[t + 1] (or last_values) when the episode continued,
      - final_values[t] (V of the ended episode's last state) when it was truncated,
      - 0 when it terminated.
    The advantage recursion never crosses an episode boundary.

    Returns (advantages, returns), both [T, E, A].
    """
    steps = rewards.shape[0]
    advantages = np.zeros_like(rewards, dtype=np.float32)
    done = dones[..., None].astype(np.float32)
    terminal = terminated[..., None].astype(np.float32)

    gae = np.zeros_like(last_values, dtype=np.float32)
    for t in reversed(range(steps)):
        continued_value = last_values if t == steps - 1 else values[t + 1]
        next_value = (1.0 - done[t]) * continued_value + done[t] * (1.0 - terminal[t]) * final_values[t]
        delta = rewards[t] + gamma * next_value - values[t]
        gae = delta + gamma * gae_lambda * (1.0 - done[t]) * gae
        advantages[t] = gae

    return advantages, advantages + values


def compute_foresight(
    rewards: np.ndarray,
    predictions: np.ndarray,
    dones: np.ndarray,
    terminated: np.ndarray,
    final_predictions: np.ndarray,
    last_predictions: np.ndarray,
    gammas: tuple[float, ...],
) -> np.ndarray:
    """Discounted returns at each of `gammas` (the foresight head's targets), [T, E, A, H].

    The same recursion as compute_gae with lambda 1: what follows a step is the next step's return while the episode
    goes on, the head's own prediction of the ended episode's last state when it was truncated, and nothing when it
    terminated. Each horizon is far shorter than the rollout, so bootstrapping only shows in the last steps.
    """
    steps = rewards.shape[0]
    targets = np.zeros((*rewards.shape, len(gammas)), dtype=np.float32)
    done = dones[..., None, None].astype(np.float32)
    terminal = terminated[..., None, None].astype(np.float32)
    discounts = np.asarray(gammas, dtype=np.float32)
    carried = last_predictions.astype(np.float32)
    for t in reversed(range(steps)):
        continued = carried if t == steps - 1 else targets[t + 1]
        following = (1.0 - done[t]) * continued + done[t] * (1.0 - terminal[t]) * final_predictions[t]
        targets[t] = rewards[t][..., None] + discounts * following
    return targets


def compute_span_gae(rewards: np.ndarray, values: np.ndarray, dones: np.ndarray, terminated: np.ndarray,
                     chosen: np.ndarray, gamma: float, gae_lambda: float, final_values: np.ndarray | None = None,
                     last_values: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """GAE over the decisions where `chosen`, for every agent at once (vectorised over [E, A]): one transition runs
    from a decision the agent took to the next it takes, or to its episode's end, and carries every reward in
    between; discounting is per decision taken, so gamma is a horizon on the agent's own clock. The one slow-clock
    GAE: the director's turns and the two-clock seat's goals both use it.

    A span cut short -- by the rollout's end, or by a truncated episode -- is bootstrapped on the value of the state
    it was cut in: `last_values` [E, A] and `final_values` [T, E, A], when the caller has them (the director's
    critic values every state). When it does not (the slow goal value exists only where a goal was chosen), such a
    span has no honest bootstrap, and it is left out: the third result says which chosen decisions have a target
    (bootstrapping it on the value it began at instead would pull its advantage to nothing)."""
    steps = rewards.shape[0]
    shape = values.shape[1:]
    advantages = np.zeros_like(rewards, dtype=np.float32)
    returns = np.zeros_like(rewards, dtype=np.float32)
    valid = np.zeros(rewards.shape, dtype=bool)
    known_final = final_values is not None
    known_last = last_values is not None

    tail = np.zeros(shape, dtype=np.float32)
    boot = last_values.astype(np.float32).copy() if known_last else np.zeros(shape, dtype=np.float32)
    flows = np.ones(shape, dtype=bool)
    target = np.full(shape, known_last)          # the open span has an honest bootstrap
    following = np.zeros(shape, dtype=np.float32)
    following_valid = np.zeros(shape, dtype=bool)
    for t in reversed(range(steps)):
        done = np.broadcast_to(dones[t][:, None], shape)
        if t + 1 < steps:
            # A span ending here: the next chosen decision bootstraps it and the credit flows on from it.
            cut = chosen[t + 1] & ~done
            tail = np.where(cut, 0.0, tail)
            boot = np.where(cut, values[t + 1], boot)
            following = np.where(cut, advantages[t + 1], following)
            following_valid = np.where(cut, valid[t + 1], following_valid)
            flows = np.where(cut, True, flows)
            target = np.where(cut, True, target)
        ends = np.broadcast_to(terminated[t][:, None], shape)
        final = np.where(ends, 0.0, final_values[t] if known_final else 0.0)
        tail = np.where(done, rewards[t], rewards[t] + tail)
        boot = np.where(done, final, boot)
        flows = np.where(done, False, flows)
        following = np.where(done, 0.0, following)
        following_valid = np.where(done, True, following_valid)
        target = np.where(done, ends | known_final, target)
        delta = tail + gamma * boot - values[t]
        # The trace only through a following span that has a target of its own.
        gae = delta + np.where(flows & following_valid, gamma * gae_lambda * following, 0.0)
        advantages[t] = np.where(chosen[t], gae, 0.0)
        returns[t] = np.where(chosen[t], gae + values[t], 0.0)
        valid[t] = chosen[t] & target
    return advantages, returns, valid


def decisions_left(dones: np.ndarray) -> np.ndarray:
    """Decisions until each step's episode ends, [T, E]; -1 where it does not end inside the rollout."""
    steps, envs = dones.shape
    left = np.full((steps, envs), -1.0, dtype=np.float32)
    following = np.full(envs, -1.0, dtype=np.float32)
    for t in reversed(range(steps)):
        following = np.where(dones[t], 0.0, np.where(following >= 0.0, following + 1.0, -1.0))
        left[t] = following
    return left


class RolloutBuffer:
    def __init__(self, steps: int, envs: int, agents: int, obs_dim: int, state_dim: int, num_actions: int,
                 foresight: int = 0, recurrent: int = 0, goals: bool = False, slow_goal: int = 0,
                 goal_slots: int = 1):
        self.steps = steps
        self.foresight = foresight
        self.recurrent = recurrent
        self.goals = goals
        self.slow_goal = slow_goal
        shape = (steps, envs, agents)
        # The two-clock seat (MappoConfig.slow_goal_size): the slow memory each decision started from, the slow value
        # where a goal was chosen, and the slow clock's advantages and returns over the chosen decisions.
        self.slow_memory = np.zeros((*shape, slow_goal), dtype=np.float32)
        self.slow_values = np.zeros(shape, dtype=np.float32)
        self.slow_advantages = np.zeros(shape, dtype=np.float32)
        self.slow_returns = np.zeros(shape, dtype=np.float32)
        self.slow_valid = np.zeros(shape, dtype=bool)          # the chosen decisions whose span has a target
        # The goal each decision pursued, what choosing it was worth, and whether this decision chose it: only those
        # decisions carry the goal chooser's own gradient.
        self.goal = np.zeros(shape, dtype=np.int64)
        self.goal_log_probs = np.zeros(shape, dtype=np.float32)
        self.goal_chosen = np.zeros(shape, dtype=bool)
        # With two goals and a queue (goal_slots > 1): the slots a choice drew, -1 for none (what the slow update
        # scores again; `goal` holds the pair the seat then held).
        self.goal_slots = np.full((*shape, goal_slots), -1, dtype=np.int64)
        # The memory each decision was taken with (LayoutActor's GRU), so the update can replay the rollout's
        # sequences from where they actually started.
        self.memory = np.zeros((*shape, recurrent), dtype=np.float32)
        # The critic's own memory at the same decisions: it carries a GRU too, over the whole env rather than one
        # seat, and the update has to replay its sequences from the state they were produced with.
        self.critic_memory = np.zeros((*shape, recurrent), dtype=np.float32)
        self.obs = np.zeros((*shape, obs_dim), dtype=np.float32)
        self.state = np.zeros((steps, envs, state_dim), dtype=np.float32)
        self.mask = np.zeros((*shape, num_actions), dtype=bool)
        self.layout = np.zeros(shape, dtype=np.int64)
        self.valid = np.ones(shape, dtype=bool)  # False for a seat without a character: not a sample
        # False for a decision a slow layout did not actually take (its previous call still standing). The
        # agent was there and the env moved on, so the recurrence has to replay it -- but it chose nothing, so
        # it is not a sample: `samples` is what the loss and the advantages are drawn from.
        self.chosen = np.ones(shape, dtype=bool)
        self.actions = np.zeros(shape, dtype=np.int64)
        self.log_probs = np.zeros(shape, dtype=np.float32)
        self.values = np.zeros(shape, dtype=np.float32)  # denormalised
        self.rewards = np.zeros(shape, dtype=np.float32)
        self.dones = np.zeros((steps, envs), dtype=bool)
        self.terminated = np.zeros((steps, envs), dtype=bool)
        self.final_values = np.zeros(shape, dtype=np.float32)  # denormalised, valid where done
        self.advantages = np.zeros(shape, dtype=np.float32)
        self.returns = np.zeros(shape, dtype=np.float32)
        # The foresight head: what it predicted, what it predicted of an ended episode's last state, and the targets
        # finish() works out. The last column is the share of the episode left, which only some steps know.
        self.foresight_preds = np.zeros((*shape, foresight), dtype=np.float32)
        self.final_foresight = np.zeros((*shape, foresight), dtype=np.float32)
        self.foresight_targets = np.zeros((*shape, foresight), dtype=np.float32)
        self.foresight_valid = np.zeros((*shape, foresight), dtype=bool)
        self.cursor = 0

    @property
    def samples(self) -> np.ndarray:
        """The rows the loss is drawn from: an agent that had a character and made a decision of its own."""
        return self.valid & self.chosen

    def add_decision(self, obs, state, mask, layout, actions, log_probs, values, present=None,
                     foresight=None, memory=None, goals=None, critic_memory=None, chosen=None) -> None:
        """Record what the policy saw and did at step `cursor`; `present` [E, A] marks the agents with a character
        (default: all)."""
        t = self.cursor
        for name, value in (("obs", obs), ("state", state), ("mask", mask)):
            if value is not None:       # None: store_rows wrote it already
                self._store(name, t, value)
        self.layout[t] = layout
        self.valid[t] = True if present is None else present
        self.chosen[t] = True if chosen is None else chosen
        self.actions[t] = actions
        self.log_probs[t] = log_probs
        self.values[t] = values
        if self.foresight and foresight is not None:
            self.foresight_preds[t] = foresight
        if self.recurrent and memory is not None:
            self.memory[t] = memory
        if self.recurrent and critic_memory is not None:
            self.critic_memory[t] = critic_memory
        if self.goals and goals is not None:
            self.goal[t], self.goal_log_probs[t], self.goal_chosen[t] = goals[:3]
            if self.slow_goal and len(goals) > 3 and goals[3] is not None:
                self.slow_memory[t], self.slow_values[t] = goals[3], goals[4]
            if len(goals) > 5 and goals[5] is not None:
                self.goal_slots[t] = goals[5]

    def store_rows(self, name: str, t: int, rows: slice, value, stream) -> None:
        """Envs `rows` of step `t` of obs, state or mask, from a device tensor, copied on `stream` (queued: the caller
        finishes it). For inputs the sim wrote into device memory it overwrites at its next step, which have to be
        copied out before the actions go back rather than when the whole decision is recorded."""
        import torch

        with torch.cuda.stream(stream):
            target = getattr(self, name)
            if isinstance(target, np.ndarray):
                target = torch.from_numpy(target).to(value.device)
                setattr(self, name, target)
            target[t, rows] = value

    def _store(self, name: str, t: int, value) -> None:
        """Step `t` of obs, state or mask. The first device tensor moves that array to its device for good."""
        import torch

        target = getattr(self, name)
        if isinstance(value, torch.Tensor) and isinstance(target, np.ndarray):
            target = torch.from_numpy(target).to(value.device)
            setattr(self, name, target)
        if isinstance(target, np.ndarray):
            target[t] = value
        else:
            target[t] = torch.as_tensor(value, device=target.device)

    def add_outcome(self, rewards, dones, terminated, final_values, final_foresight=None) -> None:
        """Record the result of the step-`cursor` actions and advance."""
        t = self.cursor
        if self.foresight and final_foresight is not None:
            self.final_foresight[t] = final_foresight
        self.rewards[t] = rewards
        self.dones[t] = dones
        self.terminated[t] = terminated
        self.final_values[t] = final_values
        self.cursor += 1

    @property
    def full(self) -> bool:
        return self.cursor >= self.steps

    def finish(self, last_values: np.ndarray, gamma: float, gae_lambda: float, last_foresight=None,
               foresight_gammas: tuple[float, ...] = (), time_scale_decisions: float = 0.0,
               slow_layout: int = -1, slow_gamma: float = 0.0, slow_gae_lambda: float = 0.0,
               slow_goal: tuple[float, float] | None = None, obs_targets=None) -> None:
        self.advantages, self.returns = compute_gae(
            self.rewards,
            self.values,
            self.dones,
            self.terminated,
            self.final_values,
            last_values,
            gamma,
            gae_lambda,
        )

        # A slow layout's own clock, over its own decisions: the per-decision advantages above are meaningless
        # for it, because nine decisions in ten it did nothing and the tenth is credited with 250 ms of
        # consequences rather than the 2.5 s its call actually governed.
        if slow_layout >= 0:
            slow = self.layout == slow_layout
            if slow.any():
                # The one slow-clock GAE (compute_span_gae), as the two-clock seat's goals use: over the decisions
                # the agent actually took -- its turns, where the sim decides them.
                advantages, returns, _ = compute_span_gae(
                    self.rewards, self.values, self.dones, self.terminated, self.chosen & slow, slow_gamma,
                    slow_gae_lambda, self.final_values, last_values)
                self.advantages = np.where(slow, advantages, self.advantages)
                self.returns = np.where(slow, returns, self.returns)

        # The two-clock seat's goals, on their own clock: every seat that chose a goal (not a slow layout's agent,
        # whose goal means nothing), over the decisions that chose one.
        if slow_goal is not None and self.slow_goal:
            chosen = self.goal_chosen & self.valid
            if slow_layout >= 0:
                chosen = chosen & (self.layout != slow_layout)
            self.slow_advantages, self.slow_returns, self.slow_valid = compute_span_gae(
                self.rewards, self.slow_values, self.dones, self.terminated, chosen, slow_goal[0], slow_goal[1])
            self.goal_chosen = chosen
        if not self.foresight or last_foresight is None:
            return

        horizons = len(foresight_gammas)
        self.foresight_targets[..., :horizons] = compute_foresight(
            self.rewards,
            self.foresight_preds[..., :horizons],
            self.dones,
            self.terminated,
            self.final_foresight[..., :horizons],
            last_foresight[..., :horizons],
            foresight_gammas,
        )
        self.foresight_valid[..., :horizons] = True

        # How much of the episode is left, as a share of time_scale_decisions: only the steps whose episode ends
        # inside the rollout know it, and the rest are left out of the loss.
        left = decisions_left(self.dones)
        known = left >= 0.0
        scale = max(1.0, time_scale_decisions)
        self.foresight_targets[..., horizons] = np.clip(left / scale, 0.0, 1.0)[..., None]
        self.foresight_valid[..., horizons] = known[..., None]

        # What the seat will observe (MappoConfig.foresight_obs_targets): per target, the column of each layout
        # (-1 where it has none), how many decisions ahead, and whether it is "at any point up to then" rather than
        # "then". Only steps whose episode runs that far inside the rollout know it.
        if obs_targets:
            steps = self.steps
            done_count = np.cumsum(self.dones.astype(np.int64), axis=0)
            layout = self.layout
            for index, (columns, ahead, window) in enumerate(obs_targets):
                slot = horizons + 1 + index
                column = np.asarray(columns, dtype=np.int64)[layout]
                values = self._obs_column(np.maximum(column, 0))
                target = np.zeros_like(values)
                valid = np.zeros(values.shape, dtype=bool)
                if ahead < steps:
                    later = values[ahead:]
                    if window:
                        stacked = np.stack([values[k : steps - ahead + k] for k in range(1, ahead + 1)])
                        later = stacked.max(axis=0)
                    before = np.concatenate([np.zeros((1, done_count.shape[1]), np.int64), done_count[:-1]])
                    clear = (done_count[ahead - 1 : steps - 1] - before[: steps - ahead]) == 0
                    target[: steps - ahead] = later
                    valid[: steps - ahead] = clear[..., None] & (column[: steps - ahead] >= 0)
                self.foresight_targets[..., slot] = target
                self.foresight_valid[..., slot] = valid

    def _obs_column(self, column: np.ndarray) -> np.ndarray:
        """obs[t, e, a, column[t, e, a]] as a [T, E, A] numpy array, wherever the observations are kept."""
        if isinstance(self.obs, np.ndarray):
            return np.take_along_axis(self.obs, column[..., None], axis=-1)[..., 0]
        import torch

        index = torch.as_tensor(column, device=self.obs.device)[..., None]
        return self.obs.gather(-1, index)[..., 0].float().cpu().numpy()

    def reset(self) -> None:
        self.cursor = 0

    def flat(self) -> dict[str, np.ndarray]:
        """Every valid per-agent sample flattened to [n, ...] (n = valid rows of T*E*A). State is repeated per agent.

        Seats without a character (``valid`` False) are left out: they only have the no-op and earn nothing, so as
        samples they would only dilute the advantages, the entropy and the value targets."""
        steps, envs, agents = self.actions.shape
        keep = self.samples.reshape(-1)
        # Boolean indexing copies, so torch gets writable arrays.
        if isinstance(self.state, np.ndarray):
            state = np.broadcast_to(self.state[:, :, None, :], (steps, envs, agents, self.state.shape[-1]))
        else:
            state = self.state[:, :, None, :].expand(steps, envs, agents, self.state.shape[-1])

        def kept(array, width: int):
            rows = array.reshape(-1, width)
            if isinstance(rows, np.ndarray):
                return rows[keep]
            import torch

            return rows[torch.as_tensor(keep, device=rows.device)]

        return {
            "obs": kept(self.obs, self.obs.shape[-1]),
            "state": kept(state, self.state.shape[-1]),
            "layout": self.layout.reshape(-1)[keep],
            "mask": kept(self.mask, self.mask.shape[-1]),
            "actions": self.actions.reshape(-1)[keep],
            "log_probs": self.log_probs.reshape(-1)[keep],
            "values": self.values.reshape(-1)[keep],
            "advantages": self.advantages.reshape(-1)[keep],
            "returns": self.returns.reshape(-1)[keep],
            **({"foresight_targets": self.foresight_targets.reshape(-1, self.foresight)[keep],
                "foresight_valid": self.foresight_valid.reshape(-1, self.foresight)[keep]} if self.foresight else {}),
            **({"goal": self.goal.reshape(-1)[keep],
                "goal_log_probs": self.goal_log_probs.reshape(-1)[keep],
                "goal_chosen": self.goal_chosen.reshape(-1)[keep]} if self.goals else {}),
        }

    def sequences(self) -> dict[str, np.ndarray]:
        """The rollout as it happened, [T, E, A, ...]: what a recurrent update replays in order. `valid` marks the
        rows that are samples, as flat() does, and `dones` [T, E] say where a memory is cleared."""
        return {
            "obs": self.obs,
            "state": self.state,
            "mask": self.mask,
            "layout": self.layout,
            "valid": self.samples,
            "actions": self.actions,
            "log_probs": self.log_probs,
            "values": self.values,
            "advantages": self.advantages,
            "returns": self.returns,
            "dones": self.dones,
            "memory": self.memory,
            "critic_memory": self.critic_memory,
            **({"foresight_targets": self.foresight_targets, "foresight_valid": self.foresight_valid}
               if self.foresight else {}),
            **({"goal": self.goal, "goal_log_probs": self.goal_log_probs, "goal_chosen": self.goal_chosen}
               if self.goals else {}),
        }

    def mean_allowed_actions(self) -> float:
        """Mean legal actions per decision over the valid samples (0 when there are none).

        Entropy only means something against this: a policy over 6 legal actions and one over 60 have very
        different ceilings, and the masked action space here swings with level, cooldowns and the global cooldown.
        """
        if not self.valid.any():
            return 0.0

        if isinstance(self.mask, np.ndarray):
            return float(self.mask[self.valid].sum(axis=-1).mean())
        import torch

        valid = torch.as_tensor(self.valid, device=self.mask.device)
        return float(self.mask[valid].sum(dim=-1).float().mean())

    def mean_reward(self) -> float:
        """Mean reward per decision over the valid samples (0 when there are none)."""
        return float(self.rewards[self.valid].mean()) if self.valid.any() else 0.0
