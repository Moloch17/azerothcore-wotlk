"""Rollout storage and GAE for auto-resetting vectorised envs.

Everything is numpy on the CPU: at [T, E, A] = [128, 64, 1] the whole rollout is a few MB, and GAE
is a single backwards loop over T.
"""

from __future__ import annotations

import numpy as np


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


def compute_slow_gae(
    rewards: np.ndarray,
    values: np.ndarray,
    dones: np.ndarray,
    terminated: np.ndarray,
    final_values: np.ndarray,
    last_values: np.ndarray,
    chosen: np.ndarray,
    slow: np.ndarray,
    gamma: float,
    gae_lambda: float,
) -> tuple[np.ndarray, np.ndarray]:
    """GAE for agents whose decisions are spaced out, over their own decisions rather than every step.

    One transition runs from the decision an agent actually took to the next one it takes -- or to the end of its
    episode, or the end of the rollout -- and carries every reward in between. Discounting is then per decision
    taken, so `gamma` and `gae_lambda` mean what they say on the agent's own clock: at ten decisions a span and
    250 ms a decision, gamma 0.996 is a ten minute horizon where the seats' 0.9975 is a hundred seconds.

    Shapes as compute_gae, plus `chosen` and `slow` [T, E, A]. Rewards inside a span are summed and not
    discounted: a span is seconds and the horizon is minutes, so the difference is far below the noise.

    Only the chosen steps of slow agents get an advantage; everything else is left untouched for the caller's
    own per-decision GAE to fill.
    """
    steps, envs, agents = rewards.shape
    advantages = np.zeros_like(rewards, dtype=np.float32)
    returns = np.zeros_like(rewards, dtype=np.float32)

    for env in range(envs):
        for agent in range(agents):
            if not slow[:, env, agent].any():
                continue

            # Walk the rollout once, cutting it into the transitions this agent actually made.
            spans = []      # (step, summed reward, bootstrap value, whether credit flows past it)
            step = 0
            while step < steps:
                if not (chosen[step, env, agent] and slow[step, env, agent]):
                    step += 1
                    continue

                reward = 0.0
                end = step
                while True:
                    reward += float(rewards[end, env, agent])
                    if dones[end, env] or end + 1 >= steps or chosen[end + 1, env, agent]:
                        break
                    end += 1

                if dones[end, env]:
                    # The episode ended inside the span: nothing follows a termination, and a truncation is
                    # worth the value of the state it was cut off in.
                    bootstrap = 0.0 if terminated[end, env] else float(final_values[end, env, agent])
                    flows = False
                elif end + 1 < steps:
                    bootstrap = float(values[end + 1, env, agent])
                    flows = True
                else:
                    # The rollout ended first; the value of what came next is all there is to go on.
                    bootstrap = float(last_values[env, agent])
                    flows = True

                spans.append((step, reward, bootstrap, flows))
                step = end + 1

            gae = 0.0
            for start, reward, bootstrap, flows in reversed(spans):
                delta = reward + gamma * bootstrap - float(values[start, env, agent])
                gae = delta + (gamma * gae_lambda * gae if flows else 0.0)
                advantages[start, env, agent] = gae
                returns[start, env, agent] = gae + float(values[start, env, agent])

    return advantages, returns


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
                 foresight: int = 0, recurrent: int = 0, goals: bool = False):
        self.steps = steps
        self.foresight = foresight
        self.recurrent = recurrent
        self.goals = goals
        shape = (steps, envs, agents)
        # The goal each decision pursued, what choosing it was worth, and whether this decision chose it: only those
        # decisions carry the goal chooser's own gradient.
        self.goal = np.zeros(shape, dtype=np.int64)
        self.goal_log_probs = np.zeros(shape, dtype=np.float32)
        self.goal_chosen = np.zeros(shape, dtype=bool)
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
        self.obs[t] = obs
        self.state[t] = state
        self.mask[t] = mask
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
            self.goal[t], self.goal_log_probs[t], self.goal_chosen[t] = goals

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
               slow_layout: int = -1, slow_gamma: float = 0.0, slow_gae_lambda: float = 0.0) -> None:
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
                advantages, returns = compute_slow_gae(
                    self.rewards,
                    self.values,
                    self.dones,
                    self.terminated,
                    self.final_values,
                    last_values,
                    self.chosen,
                    slow,
                    slow_gamma,
                    slow_gae_lambda,
                )
                self.advantages = np.where(slow, advantages, self.advantages)
                self.returns = np.where(slow, returns, self.returns)
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

    def reset(self) -> None:
        self.cursor = 0

    def flat(self) -> dict[str, np.ndarray]:
        """Every valid per-agent sample flattened to [n, ...] (n = valid rows of T*E*A). State is repeated per agent.

        Seats without a character (``valid`` False) are left out: they only have the no-op and earn nothing, so as
        samples they would only dilute the advantages, the entropy and the value targets."""
        steps, envs, agents = self.actions.shape
        keep = self.samples.reshape(-1)
        # Boolean indexing copies, so torch gets writable arrays.
        state = np.broadcast_to(self.state[:, :, None, :], (steps, envs, agents, self.state.shape[-1]))
        return {
            "obs": self.obs.reshape(-1, self.obs.shape[-1])[keep],
            "state": state.reshape(-1, self.state.shape[-1])[keep],
            "layout": self.layout.reshape(-1)[keep],
            "mask": self.mask.reshape(-1, self.mask.shape[-1])[keep],
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

        return float(self.mask[self.valid].sum(axis=-1).mean())

    def mean_reward(self) -> float:
        """Mean reward per decision over the valid samples (0 when there are none)."""
        return float(self.rewards[self.valid].mean()) if self.valid.any() else 0.0
