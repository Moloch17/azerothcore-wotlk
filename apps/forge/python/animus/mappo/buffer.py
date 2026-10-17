"""Rollout storage and GAE for auto-resetting vectorised envs.

Everything is numpy on the CPU -- at [T, E, A] = [128, 64, 1] the whole rollout is a few MB, and GAE
is a single backwards loop over T -- except obs, state and mask when the sim writes them into device
memory (protocol 15, animus.device): then they are kept on that device, where the update reads them,
and never cross to the host at all.
"""

from __future__ import annotations

import numpy as np

from ..protocol import LABEL_UNREACHABLE, c_body_of, c_map_of

# Kept where the sim put them when it put them on the device.


def compute_potential(c_body: np.ndarray, c_valid: np.ndarray, dones: np.ndarray, last_c_body: np.ndarray,
                      last_c_valid: np.ndarray, gamma: float, yards_scale: float, clip: float) -> np.ndarray:
    """The potential-based progress term of every step, [T, E, A] (vision movement curriculum, CONTRACT.md sec 5):
    gamma x phi(next) - phi(cur) with phi = -min(C, clip x 100) x yards_scale, C the body's route cost-to-go in yards
    the sim labels each observation with (c_body [T, E, A]; last_c_body [E, A] the observation after the rollout's
    last step). 0 on a done row (a timeout must not pay the remaining distance; arrival already drives C to ~0) and
    wherever either C is unreachable (c_valid False). Policy-invariant with the learner's own gamma (Ng, Harada,
    Russell 1999), which is why the learner and not the sim pays it."""
    steps = c_body.shape[0]
    cap = clip * 100.0
    phi = -np.minimum(c_body, cap).astype(np.float32) * np.float32(yards_scale)
    phi_last = -np.minimum(last_c_body, cap).astype(np.float32) * np.float32(yards_scale)
    out = np.zeros_like(phi)
    for t in range(steps):
        next_phi = phi_last if t == steps - 1 else phi[t + 1]
        next_valid = last_c_valid if t == steps - 1 else c_valid[t + 1]
        term = np.float32(gamma) * next_phi - phi[t]
        paid = c_valid[t] & next_valid & ~dones[t][:, None]
        out[t] = np.where(paid, term, 0.0).astype(np.float32)
    return out


#: What the potential term is paid on (potential.source; config.PotentialConfig): "true", the bake's route cost-to-go
#: (compute_potential); "map", the route cost-to-go on the bot's own mental map (compute_potential_map); "blend",
#: potential.blend of the map term and the rest of the true one.
POTENTIAL_SOURCES = ("true", "map", "blend")


def check_map_costs(*costs: np.ndarray) -> None:
    """Refuse map costs (c_map_of) that cannot be route costs: a route cost-to-go is never negative, while the goal's
    forward and right yards in the body frame -- the label scalars beside them -- are negative about half the time, so
    a learner whose LABEL_SCALAR_MAP_* offsets do not match the sim's layout would otherwise pay the goal's yards as
    progress on a connect the protocol version let through. Raises ValueError; the first rollout trips it."""
    for cost in costs:
        if cost is not None and np.asarray(cost).size and (np.asarray(cost) < 0.0).any():
            raise ValueError("potential.source map: a label's map route cost is negative, which no route cost is; "
                             "protocol.LABEL_SCALAR_MAP_BODY / LABEL_SCALAR_MAP_PREV do not match the sim's label "
                             "layout (TravBake.h TRAV_LABEL_SCALARS): the goal yards are being read as costs")


def compute_potential_map(c_map_body: np.ndarray, c_map_prev: np.ndarray, dones: np.ndarray,
                          last_c_map_body: np.ndarray, last_c_map_prev: np.ndarray, yards_scale: float,
                          clip: float) -> np.ndarray:
    """The progress term on the bot's OWN mental map of every step, [T, E, A] (potential.source "map"): yards_scale x
    (min(C_prev, clip x 100) - min(C_body, clip x 100)) read off the labels of the observation the step led to --
    c_map_body [T, E, A] and c_map_prev [T, E, A] are each observation's route cost-to-go on the bot's map from the
    body and from the previous decision's body position, both on that same current map (protocol 33: the sim prices
    both positions on one map, so the difference is the move alone, never the map changing between decisions; the
    last step's are last_c_map_* [E, A], the observation after the rollout). A blank map prices the unknown as the
    classical planner does, so its route is near the straight line and the term pays walking toward the goal; a
    known map's route is the detour, and the term pays following it. 0 on a done row (the row after a reset describes
    a position before it) and wherever either cost is unreachable (LABEL_UNREACHABLE). Not a potential difference
    (the map changes), so no discount inside it."""
    steps = c_map_body.shape[0]
    cap = clip * 100.0
    out = np.zeros_like(c_map_body, dtype=np.float32)
    for t in range(steps):
        body = last_c_map_body if t == steps - 1 else c_map_body[t + 1]
        prev = last_c_map_prev if t == steps - 1 else c_map_prev[t + 1]
        paid = (body < LABEL_UNREACHABLE) & (prev < LABEL_UNREACHABLE) & ~dones[t][:, None]
        term = (np.minimum(prev, cap) - np.minimum(body, cap)).astype(np.float32) * np.float32(yards_scale)
        out[t] = np.where(paid, term, 0.0).astype(np.float32)
    return out


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
                     chosen: np.ndarray, gamma: float, gae_lambda: float
                     ) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """GAE over the decisions where `chosen`, for every agent at once (vectorised over [E, A]): one transition runs
    from a decision the agent took to the next it takes, or to its episode's end, and carries every reward in
    between; discounting is per decision taken, so gamma is a horizon on the agent's own clock. The slow-clock GAE of
    the two-clock seat's goals.

    A span cut short -- by the rollout's end, or by a truncated episode -- has no honest bootstrap (the slow goal value
    exists only where a goal was chosen), and it is left out: the third result says which chosen decisions have a
    target (bootstrapping it on the value it began at instead would pull its advantage to nothing)."""
    steps = rewards.shape[0]
    shape = values.shape[1:]
    advantages = np.zeros_like(rewards, dtype=np.float32)
    returns = np.zeros_like(rewards, dtype=np.float32)
    valid = np.zeros(rewards.shape, dtype=bool)

    tail = np.zeros(shape, dtype=np.float32)
    boot = np.zeros(shape, dtype=np.float32)
    flows = np.ones(shape, dtype=bool)
    target = np.zeros(shape, dtype=bool)         # the open span has no honest bootstrap
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
        tail = np.where(done, rewards[t], rewards[t] + tail)
        boot = np.where(done, 0.0, boot)
        flows = np.where(done, False, flows)
        following = np.where(done, 0.0, following)
        following_valid = np.where(done, True, following_valid)
        target = np.where(done, ends, target)
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
                 goal_slots: int = 1, image_bytes: int = 0, look_heads: int = 0, label_bytes: int = 0,
                 planner_widths: tuple[int, int] = (0, 0), heading: bool = False):
        self.steps = steps
        self.heading = bool(heading)
        self.image_bytes = image_bytes
        self.label_bytes = label_bytes
        self.planner_widths = tuple(planner_widths)
        self.look_heads = look_heads
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
        # And the block of the mental map's crop each cell goal named (free-choice-goals), -1 where the slot's goal is
        # not a cell goal (and everywhere without a cell head): the slow update scores them again beside the slots.
        self.goal_cells = np.full((*shape, goal_slots), -1, dtype=np.int16)
        # Hindsight (MappoTrainer._achieved_of): the goal each decision turned out to achieve, read off the next
        # observation's goal block, -1 for none.
        self.achieved = np.full(shape, -1, dtype=np.int64)
        # The memory each decision was taken with (LayoutActor's GRU), so the update can replay the rollout's
        # sequences from where they actually started.
        self.memory = np.zeros((*shape, recurrent), dtype=np.float32)
        # The critic's own memory at the same decisions: it carries a GRU too, over the whole env rather than one
        # seat, and the update has to replay its sequences from the state they were produced with.
        self.critic_memory = np.zeros((*shape, recurrent), dtype=np.float32)
        self.obs = np.zeros((*shape, obs_dim), dtype=np.float32)
        # The camera's images as the sim sent them (protocol 21): bytes, decoded on the device for each pass only.
        self.image = np.zeros((*shape, image_bytes), dtype=np.uint8)
        # The nav stages' labels (protocol 31): bytes on the host, kept only where the stage sends them, and read off
        # them the body's route cost-to-go (c_body, yards; c_valid where the sim found a route) that the potential
        # term (compute_potential) is paid on. `potential` is what finish() added to each step's reward.
        self.labels = np.zeros((*shape, label_bytes), dtype=np.uint8)
        self.c_body = np.full(shape, LABEL_UNREACHABLE, dtype=np.float32)
        self.c_valid = np.zeros(shape, dtype=bool)
        # ... and the route cost-to-go on the bot's own map, from the body and from the previous decision's position
        # (c_map_of; potential.source "map", compute_potential_map). `potential_map` is the map term's part of
        # `potential` (all of it under "map", potential.blend of it under "blend", 0 under "true").
        self.c_map_body = np.full(shape, LABEL_UNREACHABLE, dtype=np.float32)
        self.c_map_prev = np.full(shape, LABEL_UNREACHABLE, dtype=np.float32)
        self.potential = np.zeros(shape, dtype=np.float32)
        self.potential_map = np.zeros(shape, dtype=np.float32)
        # The plan each decision acted on (MappoTrainer._replan's cache: the planner's readouts and the pointer's
        # bonus), so the update replays the decision on the same plan rather than planning again.
        self.planner_readout = np.zeros((*shape, self.planner_widths[0]), dtype=np.float32)
        self.planner_pool = np.zeros((*shape, self.planner_widths[1]), dtype=np.float32)
        self.state = np.zeros((steps, envs, state_dim), dtype=np.float32)
        self.mask = np.zeros((*shape, num_actions), dtype=bool)
        self.layout = np.zeros(shape, dtype=np.int64)
        self.valid = np.ones(shape, dtype=bool)  # False for a seat without a character: not a sample
        self.actions = np.zeros(shape, dtype=np.int64)
        # The free look's choices (protocol 22, LookHead), one a head -- yaw rate, pitch rate, zoom -- as sent; 0 for a
        # row without the camera. `log_probs` is the joint one: the action's plus the look's.
        self.look = np.zeros((*shape, look_heads), dtype=np.int8)
        # The look's part of `log_probs` (the policy's own, as log_probs is), for the movement-only KL.
        self.look_log_probs = np.zeros(shape, dtype=np.float32)
        # The heading each decision took (protocol 32, HeadingHead), radians as sent, 0 for a row without a move block,
        # and its part of `log_probs` (its log density), for the movement-only KL.
        self.heading_theta = np.zeros(shape, dtype=np.float32)
        self.heading_log_probs = np.zeros(shape, dtype=np.float32)
        self.log_probs = np.zeros(shape, dtype=np.float32)
        # Which seats acted with the argmax (mappo.greedy_env_fraction, exploration v3): their log probabilities are not
        # draws from the policy, so the update's policy gradient skips them.
        self.greedy = np.zeros(shape, dtype=bool)
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
        return self.valid

    def add_decision(self, obs, state, mask, layout, actions, log_probs, values, present=None,
                     foresight=None, memory=None, goals=None, critic_memory=None, image=None,
                     look=None, look_log_prob=None, greedy=None, labels=None, planner_readout=None,
                     planner_pool=None, heading=None, heading_log_prob=None) -> None:
        """Record what the policy saw and did at step `cursor`; `present` [E, A] marks the agents with a character
        (default: all); `image` [E, A, I] uint8 the camera's bytes, with a camera; `look` [E, A, H] the free look's
        choices, with look heads; `labels` [E, A, L] uint8 the nav labels (protocol 31), where the stage sends them;
        `planner_readout` [E, A, R] and `planner_pool` [E, A, B] the cached plan the decision used, with a planner;
        `heading` [E, A] the headings taken and `heading_log_prob` [E, A] their log densities, with a heading head."""
        t = self.cursor
        if self.planner_widths[0] and planner_readout is not None:
            self.planner_readout[t] = planner_readout
            self.planner_pool[t] = planner_pool
        if self.label_bytes and labels is not None:
            self.labels[t] = labels
            self.c_body[t] = c_body_of(labels)
            self.c_valid[t] = self.c_body[t] < LABEL_UNREACHABLE
            self.c_map_body[t], self.c_map_prev[t] = c_map_of(labels)
        for name, value in (("obs", obs), ("state", state), ("mask", mask), ("image", image)):
            if value is not None:       # None: store_rows wrote it already
                self._store(name, t, value)
        self.layout[t] = layout
        self.valid[t] = True if present is None else present
        self.actions[t] = actions
        if self.look_heads and look is not None:
            self.look[t] = look
        if self.look_heads and look_log_prob is not None:
            self.look_log_probs[t] = look_log_prob
        if self.heading and heading is not None:
            self.heading_theta[t] = heading
        if self.heading and heading_log_prob is not None:
            self.heading_log_probs[t] = heading_log_prob
        self.log_probs[t] = log_probs
        if greedy is not None:
            self.greedy[t] = greedy
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
            if len(goals) > 6 and goals[6] is not None:
                self.goal_cells[t] = goals[6]

    def store_rows(self, name: str, t: int, rows: slice, value, stream) -> None:
        """Envs `rows` of step `t` of obs, state, mask or image, from a device tensor, copied on `stream` (queued: the
        caller finishes it). For inputs the sim wrote into device memory it overwrites at its next step, which have to
        be copied out before the actions go back rather than when the whole decision is recorded."""
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
               slow_goal: tuple[float, float] | None = None, obs_targets=None,
               potential: tuple[float, float] | None = None, last_c_body: np.ndarray | None = None,
               potential_source: str = "true", potential_blend: float = 0.0,
               last_c_map: tuple[np.ndarray, np.ndarray] | None = None) -> None:
        """`potential` (yards_scale, clip[, discount]) with `last_c_body` [E, A] (the cost-to-go of the observation
        after the last step; c_body_of(step.labels)) pays the potential term into the rewards before the advantages
        are taken, where the stage sends labels. `potential_source` (POTENTIAL_SOURCES) says what it is paid on:
        "true" the bake's route (compute_potential, as before), "map" the bot's own map's (compute_potential_map, off
        `last_c_map` = c_map_of(step.labels) for the last step), "blend" `potential_blend` of the map term and the
        rest of the true one."""
        self.potential[:] = 0.0
        self.potential_map[:] = 0.0
        if potential is None or not self.label_bytes:
            pass
        elif potential_source == "true":
            if last_c_body is not None:
                last = np.asarray(last_c_body, dtype=np.float32)
                self.potential = compute_potential(self.c_body, self.c_valid, self.dones, last,
                                                   last < LABEL_UNREACHABLE,
                                                   potential[2] if len(potential) > 2 else gamma, potential[0],
                                                   potential[1])
        elif potential_source in ("map", "blend") and last_c_map is not None:
            # (Without the observation after the rollout nothing is paid, as the true term's rule above.)
            last_body = np.asarray(last_c_map[0], dtype=np.float32)
            last_prev = np.asarray(last_c_map[1], dtype=np.float32)
            check_map_costs(self.c_map_body, self.c_map_prev, last_body, last_prev)
            map_term = compute_potential_map(self.c_map_body, self.c_map_prev, self.dones, last_body, last_prev,
                                             potential[0], potential[1])
            if potential_source == "map":
                self.potential_map = map_term
                self.potential = map_term.copy()
            else:
                if last_c_body is None:
                    raise ValueError("potential.source blend: finish needs last_c_body too")
                last = np.asarray(last_c_body, dtype=np.float32)
                true_term = compute_potential(self.c_body, self.c_valid, self.dones, last, last < LABEL_UNREACHABLE,
                                              potential[2] if len(potential) > 2 else gamma, potential[0],
                                              potential[1])
                weight = np.float32(potential_blend)
                self.potential_map = map_term * weight
                self.potential = (self.potential_map + true_term * (np.float32(1.0) - weight)).astype(np.float32)
        elif potential_source not in POTENTIAL_SOURCES:
            raise ValueError(f"potential.source: expected one of {list(POTENTIAL_SOURCES)}, got {potential_source!r}")
        if potential is not None and self.label_bytes:
            self.rewards += self.potential
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

        # The two-clock seat's goals, on their own clock: every seat that chose a goal, over the decisions that chose
        # one.
        if slow_goal is not None and self.slow_goal:
            chosen = self.goal_chosen & self.valid
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

    def sequences(self) -> dict[str, np.ndarray]:
        """The rollout as it happened, [T, E, A, ...]: what a recurrent update replays in order. `valid` marks the
        rows that are samples (a seat without a character is none), and `dones` [T, E] say where a memory is cleared."""
        return {
            "obs": self.obs,
            **({"image": self.image} if self.image_bytes else {}),
            **({"labels": self.labels} if self.label_bytes else {}),
            **({"planner_readout": self.planner_readout, "planner_pool": self.planner_pool}
               if self.planner_widths[0] else {}),
            "state": self.state,
            "mask": self.mask,
            "layout": self.layout,
            "valid": self.samples,
            "actions": self.actions,
            **({"look": self.look, "look_log_probs": self.look_log_probs} if self.look_heads else {}),
            **({"heading": self.heading_theta, "heading_log_probs": self.heading_log_probs} if self.heading else {}),
            "log_probs": self.log_probs,
            "greedy": self.greedy,
            "values": self.values,
            "advantages": self.advantages,
            "returns": self.returns,
            "dones": self.dones,
            "memory": self.memory,
            "critic_memory": self.critic_memory,
            **({"foresight_targets": self.foresight_targets, "foresight_valid": self.foresight_valid}
               if self.foresight else {}),
            **({"goal": self.goal, "goal_log_probs": self.goal_log_probs, "goal_chosen": self.goal_chosen,
                "achieved": self.achieved} if self.goals else {}),
        }

    def mean_reward(self) -> float:
        """Mean reward per decision over the valid samples (0 when there are none)."""
        return float(self.rewards[self.valid].mean()) if self.valid.any() else 0.0

    def mean_potential(self) -> float:
        """Mean potential term per decision over the valid samples (reward_potential; 0 without labels)."""
        return float(self.potential[self.valid].mean()) if self.label_bytes and self.valid.any() else 0.0

    def mean_potential_map(self) -> float:
        """Mean of the map term's part of the potential per decision over the valid samples (reward_potential_map; 0
        without labels or under potential.source "true")."""
        return float(self.potential_map[self.valid].mean()) if self.label_bytes and self.valid.any() else 0.0
