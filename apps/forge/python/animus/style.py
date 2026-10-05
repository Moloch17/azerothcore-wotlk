"""The movement-style reward: an adversarial motion prior (human-play-data plan 2.4; StyleConfig).

A small discriminator D reads a window of motion features (animus.human.motion: WINDOW decisions of how a body moved,
in its own frame and in units of the speed in force) and the context the window ended in (on foot, swimming, flying
or airborne; mounted; in combat), and learns to say +1 for players' windows and -1 for the seats' own, least-squares:

    loss = E_human[(D - 1)^2] + E_bot[(D + 1)^2] + grad_penalty / 2 * E_human[|dD/dwindow|^2]

and a seat is paid, every decision that ends a full window of its own motion, r = max(0, 1 - 0.25 (D - 1)^2): 1 for
motion D takes for a player's, 0 for motion it is sure is a bot's. It judges how the body moves, never which key was
pressed, so the policy still finds what to do and is only shaped toward doing it the way people do.

The bot's motion is the sim's kinematic samples (protocol 20, one per agent per decision), read through the same
motion.features the human windows were made with, track by track exactly (motion.features_of_tracks): a seat's track
is one episode, cut where it ended, where the seat had no body, and wherever the episode clock did not move forward.
The decision an episode ends on has no sample after it (the STEP carries the next episode's first), so it earns no
style; nor does a decision whose context the players never showed (the human data has nothing to compare it with).

The human windows come from `human_motion_windows.npz` (FORMAT.md section 5): each training step draws, for every bot
window in its batch, a player window of the same context, by the file's `weight` -- so D is never told a context
apart by how common it is.

Data-parallel learners (animus.parallel): every rank trains D on its own rollout's windows and the gradients are
averaged before every step, on the run's own collective group (the update's has a group of its own), as the policy's
are; the ranks start from the leader's D, so they stay one D. Asynchronous learners (mappo.rank_sync = async) trade D
with the policy's networks (async_sync), elastic-averaged like them.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import torch
from torch import nn

from .human import motion

#: Windows D scores at once when paying a rollout.
CHUNK = 65536
#: Seats whose tracks are read through motion.features_of_tracks at once (bounds the float64 temporaries).
SEAT_CHUNK = 2048


def reward_of(d: np.ndarray) -> np.ndarray:
    """The style reward for discriminator outputs `d`: max(0, 1 - 0.25 (d - 1)^2)."""
    d = np.asarray(d, dtype=np.float32)
    return np.maximum(0.0, 1.0 - 0.25 * (d - 1.0) ** 2).astype(np.float32)


class HumanWindows:
    """The players' windows (human_motion_windows.npz): drawn by weight, per context."""

    def __init__(self, windows: np.ndarray, contexts: np.ndarray, weights: np.ndarray | None = None,
                 meta: dict | None = None):
        self.windows = np.asarray(windows, dtype=np.float32)
        self.contexts = np.asarray(contexts, dtype=np.int64).reshape(-1)
        weights = np.ones(len(self.contexts)) if weights is None else np.asarray(weights, dtype=np.float64)
        if self.windows.ndim != 3 or self.windows.shape[2] != motion.F or len(self.windows) != len(self.contexts) \
                or len(weights) != len(self.contexts):
            raise ValueError(f"human windows: expected [N, W, {motion.F}] windows with N contexts and weights, got "
                             f"{self.windows.shape}, {self.contexts.shape}, {weights.shape}")
        weights = np.where(np.isfinite(weights) & (weights > 0.0), weights, 0.0)
        self.meta = meta or {}
        self.rows: dict[int, np.ndarray] = {}
        self.probabilities: dict[int, np.ndarray] = {}
        for context in np.unique(self.contexts):
            rows = np.flatnonzero((self.contexts == context) & (weights > 0.0))
            if len(rows):
                self.rows[int(context)] = rows
                self.probabilities[int(context)] = weights[rows] / weights[rows].sum()

    @property
    def window(self) -> int:
        return int(self.windows.shape[1])

    @property
    def context_set(self) -> list[int]:
        """The contexts there are players' windows of, ascending."""
        return sorted(self.rows)

    @classmethod
    def load(cls, path: str | Path, window: int) -> "HumanWindows":
        with np.load(path, allow_pickle=False) as data:
            meta = data["meta"] if "meta" in data.files else None
            human = cls(data["windows"], data["context"], data["weight"] if "weight" in data.files else None,
                        json.loads(str(meta)) if meta is not None and str(meta) else None)
        if human.window != window:
            raise ValueError(f"{path}: its windows are {human.window} steps, style.window is {window}")
        return human

    def sample(self, contexts: np.ndarray, rng: np.random.Generator) -> np.ndarray:
        """A player window for each of `contexts` (each one the players have), drawn by weight: [n, W, F]."""
        contexts = np.asarray(contexts, dtype=np.int64)
        out = np.empty((len(contexts), self.window, motion.F), dtype=np.float32)
        for context in np.unique(contexts):
            where = np.flatnonzero(contexts == context)
            picks = rng.choice(self.rows[int(context)], size=len(where), p=self.probabilities[int(context)])
            out[where] = self.windows[picks]
        return out


class Discriminator(nn.Module):
    """A window of motion features and its context's one-hot -> one number (+1 a player's, -1 a bot's)."""

    def __init__(self, window: int, hidden: tuple[int, ...]):
        super().__init__()
        self.window = window
        layers: list[nn.Module] = []
        width = window * motion.F + motion.CONTEXTS
        for size in hidden:
            layers += [nn.Linear(width, size), nn.ReLU()]
            width = size
        layers.append(nn.Linear(width, 1))
        self.net = nn.Sequential(*layers)

    def forward(self, windows: torch.Tensor, contexts: torch.Tensor) -> torch.Tensor:
        context = nn.functional.one_hot(contexts.long().clamp(0, motion.CONTEXTS - 1), motion.CONTEXTS)
        return self.net(torch.cat([windows.flatten(1), context.to(windows.dtype)], dim=1)).squeeze(-1)


class StyleReward:
    """The discriminator, its optimizer, and every seat's motion across rollouts.

    A rollout calls begin(step) with the STEP its first decision is taken on, record(t, rows, part) with each STEP that
    answers decision t, and finish(dones, samples, last_step) once the rollout is in: the style reward of each of its
    transitions [T, E, A] (unscaled, 0 to 1) and the statistics to log. D is trained there too, after the rollout is
    paid, on that rollout's windows."""

    def __init__(self, config, human: HumanWindows, envs: int, agents: int, steps: int, device="cpu", ranks=None,
                 seed: int = 0):
        self.config = config
        self.human = human
        self.window = int(config.window)
        self.device = torch.device(device)
        self.ranks = ranks
        self.rng = np.random.default_rng(seed)
        self.disc = Discriminator(self.window, tuple(config.hidden)).to(self.device)
        self.optimizer = torch.optim.Adam(self.disc.parameters(), lr=config.lr)
        self.known = np.zeros(motion.CONTEXTS, dtype=bool)
        self.known[human.context_set] = True
        self.envs, self.agents, self.steps = envs, agents, steps
        self.samples = np.zeros((steps + 1, envs, agents, motion.SAMPLE_DIM), dtype=np.float32)
        self.present = np.zeros((steps + 1, envs, agents), dtype=bool)
        self._clear_carry()
        self.tail = None
        self.unknown_seen: set[int] = set()

    # ------------------------------------------------------------------ the seats' motion

    def _clear_carry(self) -> None:
        """Forget every seat's motion: a whole-pool reset (an evaluation, a worker rejoining) started new tracks."""
        e, a, w = self.envs, self.agents, self.window
        self.prev_sample = np.zeros((e, a, motion.SAMPLE_DIM), dtype=np.float32)  # the sample before the next K[0]
        self.prev_linked = np.zeros((e, a), dtype=bool)        # ... on the same track as K[0]
        self.carry_feats = np.zeros((w - 1, e, a, motion.F), dtype=np.float32)  # the last W - 1 steps
        self.carry_run = np.zeros((e, a), dtype=np.int64)      # unbroken steps of the track ending there

    def begin(self, step) -> None:
        """The STEP the rollout's first decision is taken on. Unless it is where the last rollout ended, every seat
        starts a new track."""
        if step is not self.tail:
            self._clear_carry()
        self.samples[0] = _kinematics(step, self.agents)
        self.present[0] = np.asarray(step.present, dtype=bool)

    def record(self, t: int, rows: slice, part) -> None:
        """The STEP answering decision `t` for envs `rows`: each seat's body after it."""
        self.samples[t + 1, rows] = _kinematics(part, self.agents)
        self.present[t + 1, rows] = np.asarray(part.present, dtype=bool)

    def _features(self, dones: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
        """Every seat's steps of the rollout: features [T, E, A, F], which are steps [T, E, A], which begin a track
        (no step before them) [T, E, A], and every series' track starts [T + 2, E, A]."""
        k, present = self.samples, self.present
        steps, e, a = self.steps, self.envs, self.agents
        series = np.concatenate([self.prev_sample[None], k], axis=0)          # [T + 2, E, A, S]
        starts = np.zeros((steps + 2, e, a), dtype=bool)
        starts[0] = True
        starts[1] = ~self.prev_linked | ~present[0]
        clock = k[..., motion.T]
        starts[2:] = (np.asarray(dones, dtype=bool)[:, :, None] | ~present[1:] | ~present[:-1]
                      | (clock[1:] <= clock[:-1]))
        # Seat-major, each seat's series one run of rows: motion.features_of_tracks reads them all at once.
        flat_series = series.transpose(1, 2, 0, 3).reshape(e * a, steps + 2, motion.SAMPLE_DIM)
        flat_starts = starts.transpose(1, 2, 0).reshape(e * a, steps + 2)
        feats = np.zeros((e * a, steps + 2, motion.F), dtype=np.float32)
        valid = np.zeros((e * a, steps + 2), dtype=bool)
        for first in range(0, e * a, SEAT_CHUNK):
            seats = slice(first, min(e * a, first + SEAT_CHUNK))
            count = seats.stop - seats.start
            f, v = motion.features_of_tracks(flat_series[seats].reshape(-1, motion.SAMPLE_DIM),
                                             flat_starts[seats].reshape(-1))
            feats[seats] = f.reshape(count, steps + 2, motion.F)
            valid[seats] = v.reshape(count, steps + 2)
        feats = feats.reshape(e, a, steps + 2, motion.F).transpose(2, 0, 1, 3)[2:]
        valid = valid.reshape(e, a, steps + 2).transpose(2, 0, 1)[2:]
        return feats, valid, starts[1:-1], starts

    def _windows(self, all_feats: np.ndarray, where: tuple[np.ndarray, ...]) -> np.ndarray:
        """The windows ending at transitions `where` (t, e, a): [n, W, F] from the carried steps and the rollout's."""
        t, e, a = where
        offsets = np.arange(self.window)
        return all_feats[t[:, None] + offsets[None, :], e[:, None], a[:, None]]

    def finish(self, dones: np.ndarray, samples: np.ndarray, last_step) -> tuple[np.ndarray, dict[str, float]]:
        """The rollout is in (`dones` [T, E], `samples` [T, E, A] the rows that are training samples, `last_step`
        the STEP it ended on): the style reward of each transition [T, E, A], unscaled, and D trained on its windows."""
        feats, valid, first, starts = self._features(dones)
        steps, w = self.steps, self.window
        run = np.zeros((steps, self.envs, self.agents), dtype=np.int64)
        previous = self.carry_run
        for t in range(steps):
            previous = np.where(valid[t], np.where(first[t], 1, previous + 1), 0)
            run[t] = previous
        all_feats = np.concatenate([self.carry_feats, feats], axis=0)          # [W - 1 + T, E, A, F]
        after = self.samples[1:]
        contexts = motion.context_id(after[..., motion.MODE], after[..., motion.MOUNTED], after[..., motion.IN_COMBAT])
        full = (run >= w) & self.present[1:]
        known = full & self.known[contexts]
        for context in np.unique(contexts[full & ~known]):
            if int(context) not in self.unknown_seen:
                self.unknown_seen.add(int(context))
                print(f"Style: the human windows have no {motion.context_name(int(context))} motion; the seats' is "
                      f"neither paid nor trained on", flush=True)

        reward = np.zeros((steps, self.envs, self.agents), dtype=np.float32)
        where = np.nonzero(known)
        if len(where[0]):
            scores = self.score(self._windows(all_feats, where), contexts[where])
            reward[where] = reward_of(scores)

        stats = self.train(all_feats, np.nonzero(known & np.asarray(samples, dtype=bool)), contexts)
        rows = np.asarray(samples, dtype=bool)
        stats["style_reward"] = float(reward[rows].mean()) if rows.any() else 0.0
        for context in self.human.context_set:
            mine = known & rows & (contexts == context)
            if mine.any():
                stats[f"style_reward_{motion.context_name(context)}"] = float(reward[mine].mean())

        # What the next rollout carries on from: its K[0] is this one's K[T].
        self.carry_feats = all_feats[-(w - 1):].copy() if w > 1 else self.carry_feats
        self.carry_run = run[-1].copy()
        self.prev_sample = self.samples[-2].copy()
        self.prev_linked = ~starts[-1] & self.present[-1]
        self.tail = last_step
        return reward, stats

    # ------------------------------------------------------------------ the discriminator

    @torch.no_grad()
    def score(self, windows: np.ndarray, contexts: np.ndarray) -> np.ndarray:
        """D of each window, [n]."""
        out = np.empty(len(windows), dtype=np.float32)
        for first in range(0, len(windows), CHUNK):
            chunk = slice(first, first + CHUNK)
            d = self.disc(torch.as_tensor(windows[chunk], device=self.device),
                          torch.as_tensor(np.asarray(contexts[chunk], dtype=np.int64), device=self.device))
            out[chunk] = d.float().cpu().numpy()
        return out

    def train(self, all_feats: np.ndarray, where: tuple[np.ndarray, ...], contexts: np.ndarray) -> dict[str, float]:
        """`minibatches` steps of D on bot windows ending at `where` and as many players' windows of the same
        contexts. Every rank takes every step, with or without windows of its own, so the averaged gradients line
        up."""
        config = self.config
        count = len(where[0])
        take = min(count, config.batch * config.minibatches)
        picks = self.rng.choice(count, size=take, replace=False) if take else np.zeros(0, dtype=np.int64)
        batches = np.array_split(picks, config.minibatches)
        sums = {"style_disc_human": 0.0, "style_disc_bot": 0.0, "style_gp": 0.0, "style_disc_loss": 0.0}
        trained = 0
        for batch in batches:
            self.optimizer.zero_grad(set_to_none=True)
            if len(batch):
                index = tuple(axis[batch] for axis in where)
                bot_contexts = contexts[index]
                bot = torch.as_tensor(self._windows(all_feats, index), device=self.device)
                human = torch.as_tensor(self.human.sample(bot_contexts, self.rng), device=self.device)
                human.requires_grad_(True)
                context = torch.as_tensor(bot_contexts.astype(np.int64), device=self.device)
                d_human = self.disc(human, context)
                d_bot = self.disc(bot, context)
                gradient, = torch.autograd.grad(d_human.sum(), human, create_graph=True)
                penalty = gradient.pow(2).sum(dim=(1, 2)).mean()
                loss = ((d_human - 1.0) ** 2).mean() + ((d_bot + 1.0) ** 2).mean() \
                    + 0.5 * config.grad_penalty * penalty
                loss.backward()
                sums["style_disc_human"] += float(d_human.detach().mean())
                sums["style_disc_bot"] += float(d_bot.detach().mean())
                sums["style_gp"] += float(penalty.detach())
                sums["style_disc_loss"] += float(loss.detach())
                trained += 1
            if self.ranks is not None:
                self.ranks.average_gradients(self.disc.parameters())
            self.optimizer.step()
        return {name: value / trained for name, value in sums.items()} if trained else {}

    def disc_mean(self, windows: np.ndarray, contexts: np.ndarray) -> float | None:
        """Mean D over windows of the contexts the players have (an evaluation's realism_disc), None if none."""
        keep = self.known[np.asarray(contexts, dtype=np.int64)] if len(contexts) else np.zeros(0, dtype=bool)
        if not keep.any():
            return None
        return float(self.score(np.asarray(windows)[keep], np.asarray(contexts)[keep]).mean())

    # ------------------------------------------------------------------ checkpoints

    def state_dict(self) -> dict:
        return {"disc": self.disc.state_dict(), "optimizer": self.optimizer.state_dict(), "window": self.window,
                "hidden": list(self.config.hidden)}

    def load_state_dict(self, state: dict | None) -> bool:
        """Restore D and its optimizer; False (and D left fresh) when there is none or it is another shape."""
        if not state or int(state.get("window", -1)) != self.window \
                or tuple(state.get("hidden", ())) != tuple(self.config.hidden):
            return False
        self.disc.load_state_dict(state["disc"])
        self.optimizer.load_state_dict(state["optimizer"])
        return True


def _kinematics(step, agents: int) -> np.ndarray:
    """A STEP's kinematic samples [E, A, SAMPLE_DIM]; zeros from a step without them."""
    kinematics = getattr(step, "kinematics", None)
    envs = np.asarray(step.done).shape[0]
    if kinematics is None or np.shape(kinematics)[-1] != motion.SAMPLE_DIM:
        return np.zeros((envs, agents, motion.SAMPLE_DIM), dtype=np.float32)
    return np.asarray(kinematics, dtype=np.float32)


def startup_line(config, human: HumanWindows | None, costs_enabled: bool) -> str:
    """What the style reward is doing, said once at startup beside the cost ladder's line."""
    if not config.enabled or human is None:
        return "style reward off"
    names = ", ".join(motion.context_name(context) for context in human.context_set)
    scale = ("on the cost ladder" if costs_enabled else "in full (the cost ladder is off)") if config.ladder \
        else "in full (style.ladder off)"
    return (f"style reward on: up to {config.coef:g} a decision {scale}, D {list(config.hidden)} over "
            f"{config.window}-step windows, {config.minibatches}x{config.batch} windows an update; "
            f"{len(human.windows)} human windows in {names}")
