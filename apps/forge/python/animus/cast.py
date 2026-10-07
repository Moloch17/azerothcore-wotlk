"""Frozen checkpoints in the seats a script used to play (config ``cast``).

A stage's far side -- the second seat of a mirror arena, the other team of a teams arena, and any agent the sim
declares in stage.json's ``cast`` list (an owner played for the seats) -- is answered here by a **frozen actor**
loaded from an earlier checkpoint, and its rows are kept out of the training samples. The opponent is then always
something that learned to fight: the seed chain's parent at first, this run's own past selves as the league fills.

**The league.** Snapshots of this run join ``<run_dir>/league/`` on a clock (``cast.snapshot_every_env_steps`` of
``latest.pt``) and on every improved ``best.pt``, because a league fed from ``best.pt`` alone can go a whole stage
without a new member -- ``best.pt`` only moves behind the convergence margin. Members are drawn per episode by
prioritised fictitious self-play weights, ``(1 - p)^2 + floor`` with ``p`` the live policy's win rate against the
member, so the ones the policy still loses to are met most and none is forgotten; a member beaten above
``retire_above`` for a full window is retired, the newest ``keep_newest`` never are. The live policy plays the far
side of the remaining ``1 - opponent_share`` episodes, so on those the opponent is exactly as good as the policy.

The evaluation never runs a cast actor: the far side of an evaluation is the learner's own, so scores stay
comparable across runs.

Nothing on the wire changes. Which rows are opponents follows from stage.json (each arena's ``plan`` and
``team_seats``, the episode's arena from the critic state one-hot, as animus.distill reads it), and which rows a
stage declares cast from its ``cast`` list.
"""

from __future__ import annotations

import json
import shutil
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import torch

from .distill import Teacher, build_teacher
from .protocol import look_hold
from .stages import arena_state_span
from .device import host

LEAGUE_DIR = "league"
#: A league member that is an exploiter (animus.exploit) is saved as exploiter_<n>.pt: drawn at least
#: cast.exploiter_floor of the time, and never the member the main's league statistics are read from.
EXPLOITER_PREFIX = "exploiter_"
LEAGUE_FILE = "league.json"
AUTO = "auto"
#: Cast.member of an episode whose far side the exploiter plays (animus.exploit), not a league member.
EXPLOITER_MEMBER = -2
LEAGUE = "league"


# ------------------------------------------------------------------ which rows

@dataclass
class CastRule:
    """Which rows of a decision are cast, from stage.json: an arena's opponent seats (mirror: seat 1; teams: the
    second team) and the stage's declared cast agents."""

    arena_first: int
    arena_count: int
    plans: list[str]
    team_seats: list[int]
    seats: int
    static: list[int]

    @classmethod
    def from_stage(cls, stage: dict | None, agents_per_env: int) -> "CastRule | None":
        if not stage:
            return None
        span = arena_state_span(stage)
        arenas = stage.get("arenas") or []
        if span is None or not arenas or "plan" not in arenas[0]:
            return None
        return cls(arena_first=span[0], arena_count=span[1],
                   plans=[str(arena.get("plan", "solo")) for arena in arenas],
                   team_seats=[int(arena.get("team_seats", 0) or 0) for arena in arenas],
                   seats=int(stage.get("seats", agents_per_env)),
                   static=[int(entry["agent"]) for entry in stage.get("cast") or [] if "agent" in entry])

    def arenas(self, state: np.ndarray) -> np.ndarray:
        """Each env's arena index from the critic state one-hot; -1 where none is set."""
        onehot = state[:, self.arena_first:self.arena_first + self.arena_count]
        index = onehot.argmax(axis=-1)
        return np.where(onehot.max(axis=-1) > 0.5, index, -1)

    def opponent_rows(self, state: np.ndarray, agents: int) -> np.ndarray:
        """[E, A] bool: the far side of every env whose arena is self-play."""
        envs = state.shape[0]
        rows = np.zeros((envs, agents), dtype=bool)
        for env, arena in enumerate(self.arenas(state)):
            if arena < 0 or arena >= len(self.plans):
                continue
            plan, width = self.plans[arena], self.team_seats[arena]
            if plan == "mirror" and agents > 1:
                rows[env, 1] = True
            elif plan == "teams" and width > 0:
                rows[env, width:min(self.seats, agents)] = True
        return rows

    def static_rows(self, present: np.ndarray) -> np.ndarray:
        rows = np.zeros_like(present, dtype=bool)
        for agent in self.static:
            if agent < present.shape[1]:
                rows[:, agent] = present[:, agent]
        return rows

    def has_opponents(self) -> bool:
        return any(plan in ("mirror", "teams") for plan in self.plans)


# ------------------------------------------------------------------ a frozen actor

class CastActor:
    """A frozen checkpoint acting on the stage's rows: the teacher's observation and action mapping
    (animus.distill.build_teacher), its own memory and goals per row, cleared with the episode.

    A checkpoint with a camera reads each of its rows' camera bytes (Step.image: the image, then the map crop) as the
    live actor does, its entity list and sight list from the mapped columns, and chooses its own free look (protocol
    22's ACT look section) beside its action."""

    def __init__(self, path: Path, spec, stage: dict | None, device, deterministic: bool = False):
        self.path = Path(path)
        checkpoint = torch.load(self.path, map_location="cpu", weights_only=False)
        if checkpoint.get("stage") is None and (self.path.parent / "stage.json").is_file():
            checkpoint["stage"] = json.loads((self.path.parent / "stage.json").read_text())
        self.teacher: Teacher = build_teacher(checkpoint, spec, stage, device)
        self.name = self.teacher.name
        self.device = device
        self.home = device  # where it acts; `device` is where its weights are now (offload, CastPool's cap)
        self.deterministic = deterministic
        self.look_heads = int(getattr(spec, "look_heads", 0) or 0)
        if self.teacher.camera_bytes and not getattr(spec, "image_bytes", 0):
            raise ValueError(f"{self.path}: the checkpoint has a camera and the sim sends no image")
        if self.teacher.look_heads and len(self.teacher.look_heads) != self.look_heads:
            raise ValueError(f"{self.path}: the checkpoint's free look has {len(self.teacher.look_heads)} heads, the "
                             f"sim's ACT {self.look_heads}")
        # The last decision's look for its rows [E, A, heads] (int32), and the logits it chose from (debugging, tests).
        self.look: np.ndarray | None = None
        self.last_logits: dict[int, torch.Tensor] = {}
        mappo = checkpoint["config"].get("mappo", {})
        self.goal_every = max(1, int(mappo.get("goal_every_decisions", 16) or 16))
        self.goal_count = int(mappo.get("goal_count", 0) or 0)
        self.recurrent = self.teacher.recurrent_size
        # teacher action index -> stage action index, per stage layout
        self.back = {index: dict(zip(m.actions_teacher.tolist(), m.actions_student.tolist()))
                     for index, m in self.teacher.layouts.items()}
        self.memory: np.ndarray | None = None
        self.goal: np.ndarray | None = None
        self.age: np.ndarray | None = None
        # A two-clock teacher's slow memory (MappoConfig.slow_goal_size), stepped when it chooses a goal.
        self.slow_size = self.teacher.actor.slow_size
        self.slow: np.ndarray | None = None
        # A teacher with two goals and a queue (mappo.goal_slots > 1): the goals queued behind the pair it holds.
        self.queue_width = max(1, self.teacher.actor.goal_slots - 2)
        self.queue: np.ndarray | None = None
        self.fallback_rows = 0

    def ensure(self, envs: int, agents: int) -> None:
        if self.memory is None or self.memory.shape[:2] != (envs, agents):
            self.memory = np.zeros((envs, agents, self.recurrent), dtype=np.float32)
            self.goal = np.zeros((envs, agents), dtype=np.int64)
            self.age = np.zeros((envs, agents), dtype=np.int64)
            self.slow = np.zeros((envs, agents, self.slow_size), dtype=np.float32)
            self.queue = np.full((envs, agents, self.queue_width), -1, dtype=np.int64)

    def clear(self, done: np.ndarray) -> None:
        if self.memory is not None:
            self.memory[done] = 0.0
            self.goal[done] = 0
            self.age[done] = 0
            self.slow[done] = 0.0
            self.queue[done] = -1

    def reset_all(self) -> None:
        self.memory = None

    def resident_bytes(self) -> int:
        """The bytes its weights and buffers take where they are (each frozen member carries its own camera)."""
        actor = self.teacher.actor
        return sum(t.numel() * t.element_size() for t in list(actor.parameters()) + list(actor.buffers()))

    def place(self, device) -> None:
        """Move the frozen weights (and the mapping's indexes) to `device`: CastPool / PartnerPool offload a member
        past their resident cap to the host and bring it back when it next acts. Its per-seat memory is host-side
        either way, so nothing it remembers is lost."""
        if str(device) == str(self.device):
            return
        self.teacher.actor.to(device)
        for mapping in self.teacher.layouts.values():
            for name in ("obs_student", "obs_teacher", "actions_student", "actions_teacher"):
                setattr(mapping, name, getattr(mapping, name).to(device))
        self.device = device

    def _image_rows(self, image, picked: np.ndarray, rows: int) -> torch.Tensor | None:
        """The picked rows' camera bytes [n, camera_bytes] uint8 on this actor's device, from the step's image [E, A,
        I] (numpy, or a view of the sim's device buffer, indexed where it lives); the leading bytes of the stage's row
        when the checkpoint reads less of it (no map). None for a checkpoint without a camera."""
        width = self.teacher.camera_bytes
        if not width:
            return None
        if image is None:
            raise ValueError(f"{self.path}: the checkpoint has a camera; its rows' image bytes (Step.image) have to "
                             f"come with them")
        if isinstance(image, torch.Tensor):
            flat = image.reshape(rows, -1)
            taken = flat[torch.as_tensor(picked, dtype=torch.long, device=flat.device)]
        else:
            taken = torch.from_numpy(np.ascontiguousarray(np.asarray(image).reshape(rows, -1)[picked]))
        if taken.shape[-1] < width:
            raise ValueError(f"{self.path}: the camera rows are {taken.shape[-1]} bytes, the checkpoint reads {width}")
        return taken[:, :width].to(self.device, torch.uint8)

    def act(self, obs: np.ndarray, mask: np.ndarray, layout: np.ndarray, rows: np.ndarray,
            fallback: np.ndarray, image=None) -> np.ndarray:
        """Actions for the `rows` of obs [E, A, O] / mask [E, A, N] / layout [E, A]; other rows keep `fallback`.
        A row whose layout the checkpoint lacks, or whose legal actions it has none of, keeps its fallback too.
        `image` [E, A, I]: the step's camera rows, for a checkpoint with a camera. The look it chose is self.look."""
        return self.decide(obs, mask, layout, rows, fallback, image)[0]

    @torch.no_grad()
    def decide(self, obs: np.ndarray, mask: np.ndarray, layout: np.ndarray, rows: np.ndarray,
               fallback: np.ndarray, image=None,
               look: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray | None]:
        """act(), and the look: `look` [E, A, heads] (the live policy's, or None) with the rows this actor played
        replaced by its own look -- its LookHead's choice with a camera, else the hold (protocol.LOOK_HOLD) -- as
        the ACT's look section carries them. None when the sim takes no look and none was given."""
        envs, agents = layout.shape
        self.ensure(envs, agents)
        actions = fallback.copy()
        self.fallback_rows = 0
        self.last_logits = {}
        if look is None and self.look_heads:
            look = look_hold(envs, agents, self.look_heads)
        elif look is not None:
            look = np.array(look, dtype=np.int32, copy=True)
        self.look = look
        flat = np.nonzero(rows.reshape(-1))[0]
        if not len(flat):
            return actions, look
        flat_look = look.reshape(envs * agents, -1) if look is not None else None
        flat_layout = layout.reshape(-1)
        flat_obs = obs.reshape(envs * agents, -1)
        flat_mask = mask.reshape(envs * agents, -1)
        flat_memory = self.memory.reshape(envs * agents, -1)
        flat_goal = self.goal.reshape(-1)
        flat_age = self.age.reshape(-1)
        flat_slow = self.slow.reshape(envs * agents, -1)
        flat_queue = self.queue.reshape(envs * agents, -1)
        flat_actions = actions.reshape(-1)

        for index in np.unique(flat_layout[flat]):
            mapping = self.teacher.layouts.get(int(index))
            picked = flat[flat_layout[flat] == index]
            if mapping is None:
                self.fallback_rows += len(picked)
                continue
            obs_s = torch.as_tensor(flat_obs[picked], device=self.device)
            allowed = torch.as_tensor(flat_mask[picked], device=self.device)[:, mapping.actions_student].bool()
            usable = allowed.any(dim=-1).cpu().numpy()
            if not usable.all():
                self.fallback_rows += int((~usable).sum())
            t_obs = obs_s.new_zeros(len(picked), self.teacher.obs_dim)
            t_obs[:, mapping.obs_teacher] = obs_s[:, mapping.obs_student]
            t_mask = torch.zeros(len(picked), self.teacher.num_actions, dtype=torch.bool, device=self.device)
            t_mask[:, mapping.actions_teacher] = allowed
            t_layout = torch.full((len(picked),), mapping.teacher_layout, dtype=torch.long, device=self.device)
            memory = (torch.as_tensor(flat_memory[picked], device=self.device) if self.recurrent else None)
            t_image = self._image_rows(image, picked, envs * agents)
            features = self.teacher.actor.features(t_obs, t_layout, memory, image=t_image)
            goal = None
            if self.goal_count:
                ages = flat_age[picked]
                # The same decision as the learner's own (LayoutActor.decide_goals): the queue, the clock, the
                # goal block's ended and event, and the primary an order set.
                goal_features = features
                if self.slow_size:
                    slow = torch.as_tensor(flat_slow[picked], device=self.device)
                    goal_features = self.teacher.actor.slow_step(features, slow)
                decided = self.teacher.actor.decide_goals(
                    goal_features, t_obs, t_layout, torch.as_tensor(flat_goal[picked], device=self.device),
                    torch.as_tensor(flat_queue[picked], device=self.device),
                    torch.as_tensor(ages % self.goal_every == 0, device=self.device), self.deterministic)
                choose = decided["chosen"].cpu().numpy()
                if self.slow_size and choose.any():
                    flat_slow[picked[choose]] = goal_features.cpu().numpy()[choose]
                goal = decided["goal"]
                flat_goal[picked] = goal.cpu().numpy()
                flat_queue[picked] = decided["queue"].cpu().numpy()
                flat_age[picked] = np.where(choose, 1, ages + 1)
            dist = self.teacher.actor.action_distribution(features, t_layout, t_mask, goal, obs=t_obs)
            self.last_logits[int(index)] = dist.logits
            teacher_actions = (dist.probs.argmax(-1) if self.deterministic else dist.sample()).cpu().numpy()
            if self.recurrent:
                flat_memory[picked] = features.cpu().numpy()
            back = self.back[int(index)]
            for row, action, ok in zip(picked, teacher_actions, usable):
                if ok:
                    flat_actions[row] = back.get(int(action), flat_actions[row])
            if flat_look is not None:
                # The look of the rows it played (a row it could not play keeps the live policy's, with its action).
                played = picked[usable]
                if self.teacher.actor.look_head is not None:
                    chosen, _ = self.teacher.actor.look(features, t_layout, goal, self.deterministic)
                    flat_look[played] = chosen.cpu().numpy().astype(np.int32)[usable]
                else:
                    flat_look[played] = look_hold(1, 1, flat_look.shape[-1])[0, 0]
        return actions, look


class Residency:
    """At most `limit` frozen actors on their device at once (0 = no limit): the least recently used past it are
    offloaded to the host (CastActor.place) and brought back when they next act. Each member carries its own camera
    encoder, so a pool of camera-era checkpoints is what fills an 8 GB card first."""

    def __init__(self, limit: int):
        self.limit = max(0, int(limit or 0))
        self.order: list[CastActor] = []

    def touch(self, actor: CastActor) -> CastActor:
        actor.place(actor.home)
        self.forget(actor)
        self.order.append(actor)
        if self.limit:
            for other in self.order[:-self.limit]:
                other.place("cpu")
        return actor

    def forget(self, actor: CastActor | None) -> None:
        self.order = [other for other in self.order if other is not actor]

    def resident(self) -> int:
        return sum(1 for actor in self.order if str(actor.device) == str(actor.home))


# ------------------------------------------------------------------ the league

@dataclass
class Member:
    path: Path
    actor: CastActor | None = None
    fights: int = 0
    rate: float = 0.5  # the live policy's win rate against it, an EMA over cast.rate_window fights
    retired: bool = False
    order: int = 0  # the order it joined in: the newest are never pruned or retired
    exploiter: bool = False  # trained against the main to beat it (animus.exploit), not one of its own snapshots

    def to_json(self) -> dict:
        return {"path": str(self.path), "fights": self.fights, "win_rate": round(self.rate, 4),
                "retired": self.retired, "order": self.order, "kind": "exploiter" if self.exploiter else "snapshot"}


class CastPool:
    """The league: members drawn by how hard they still are, snapshots added on a clock, beaten ones retired."""

    def __init__(self, cast_config, spec, stage, run_dir: Path, device, parent: Path | None):
        self.config = cast_config
        self.spec, self.stage, self.device = spec, stage, device
        self.run_dir = Path(run_dir)
        self.members: list[Member] = []
        self.counter = 0
        self.residency = Residency(getattr(cast_config, "resident_members", 0))
        if parent is not None:
            self.add(Path(parent))

    def add(self, path: Path) -> Member | None:
        path = Path(path)
        if any(member.path == path for member in self.members) or not path.is_file():
            return None
        member = Member(path=path, order=self.counter, exploiter=path.name.startswith(EXPLOITER_PREFIX))
        self.counter += 1
        self.members.append(member)
        self.prune()
        return member

    def actor_of(self, member: Member) -> CastActor:
        if member.actor is None:
            member.actor = CastActor(member.path, self.spec, self.stage, self.device, self.config.deterministic)
        return self.residency.touch(member.actor)

    def active(self) -> list[Member]:
        return [member for member in self.members if not member.retired]

    def newest(self, count: int) -> set[int]:
        return {member.order for member in sorted(self.members, key=lambda m: -m.order)[:count]}

    def hardest(self) -> Member | None:
        """The member the live policy beats least -- of its own snapshots: an exploiter is there to be answered, and
        never stands for the league in the main's statistics."""
        fought = [member for member in self.active() if member.fights > 0 and not member.exploiter]
        return min(fought, key=lambda member: member.rate) if fought else None

    def weights(self) -> np.ndarray:
        active = self.active()
        return np.array([(1.0 - member.rate) ** 2 + self.config.floor for member in active], dtype=np.float64)

    def probabilities(self) -> np.ndarray:
        """Each active member's share of the draw: prioritised fictitious self-play weights, an exploiter lifted to
        cast.exploiter_floor at least (the main must keep answering it) and the snapshots sharing what is left."""
        active = self.active()
        weights = self.weights()
        p = weights / weights.sum()
        exploiters = np.array([member.exploiter for member in active], dtype=bool)
        floor = float(getattr(self.config, "exploiter_floor", 0.0))
        if not exploiters.any() or floor <= 0.0:
            return p
        lifted = np.maximum(p[exploiters], floor)
        if lifted.sum() >= 1.0 or not (~exploiters).any():
            p[exploiters] = lifted / lifted.sum()
            p[~exploiters] = 0.0
            return p
        rest = p[~exploiters]
        p[~exploiters] = rest / rest.sum() * (1.0 - lifted.sum())
        p[exploiters] = lifted
        return p

    def draw(self, count: int, rng: np.random.Generator) -> list[int]:
        """Indexes into `members` for `count` episodes (probabilities)."""
        active = self.active()
        if not active:
            return [-1] * count
        indexes = [self.members.index(member) for member in active]
        return [indexes[i] for i in rng.choice(len(active), size=count, p=self.probabilities())]

    def record(self, index: int, won: float) -> None:
        if index < 0 or index >= len(self.members):
            return
        member = self.members[index]
        member.fights += 1
        alpha = 1.0 / min(member.fights, max(1, self.config.rate_window))
        member.rate += alpha * (float(won) - member.rate)
        if (member.fights >= self.config.rate_window and member.rate >= self.config.retire_above
                and member.order not in self.newest(self.config.keep_newest) and len(self.active()) > 1):
            member.retired = True

    def prune(self) -> None:
        """Keep league_size members: the most-beaten go first, never the newest nor the hardest."""
        active = self.active()
        keep = self.newest(self.config.keep_newest)
        if (hardest := self.hardest()) is not None:
            keep.add(hardest.order)
        while len(active) > self.config.league_size:
            candidates = [member for member in active if member.order not in keep]
            if not candidates:
                break
            victim = max(candidates, key=lambda member: member.rate)
            victim.retired = True
            active = self.active()

    def reload(self) -> int:
        """Members from <run_dir>/league/ not yet in the pool; returns how many joined."""
        league = self.run_dir / LEAGUE_DIR
        if not league.is_dir():
            return 0
        joined = 0
        for path in sorted(league.glob("*.pt"), key=lambda p: p.stat().st_mtime):
            if self.add(path) is not None:
                joined += 1
        return joined

    def hardest_win_rate(self) -> float | None:
        hardest = self.hardest()
        return hardest.rate if hardest is not None else None

    def to_json(self) -> dict:
        return {"members": [member.to_json() for member in self.members],
                "hardest_win_rate": self.hardest_win_rate(), "active": len(self.active())}

    def write(self) -> None:
        (self.run_dir / LEAGUE_FILE).write_text(json.dumps(self.to_json(), indent=2))


def league_snapshot(run_dir: Path, source: Path, tag: str, folder: str = LEAGUE_DIR) -> Path | None:
    """Copy `source` (a checkpoint) into <run_dir>/<folder>/<tag>.pt (the league's, or the co-op partners' with
    animus.partners); None when it is already there or missing."""
    source = Path(source)
    if not source.is_file():
        return None
    league = Path(run_dir) / folder
    league.mkdir(parents=True, exist_ok=True)
    target = league / f"{tag}.pt"
    if target.exists():
        return None
    shutil.copyfile(source, target)
    return target


# ------------------------------------------------------------------ the facade

class Cast:
    """What the training loop talks to: which rows are cast this decision, the actions for them, and the league's
    bookkeeping at episode ends."""

    def __init__(self, cast_config, spec, stage: dict | None, run_dir: Path, device, parent: Path | None,
                 seed: int = 0):
        self.config = cast_config
        self.spec = spec
        self.rule = CastRule.from_stage(stage, spec.agents_per_env)
        self.rng = np.random.default_rng(seed)
        self.pool: CastPool | None = None
        self.statics: dict[int, CastActor] = {}
        names = {int(entry["agent"]): entry.get("name", "") for entry in (stage or {}).get("cast") or []
                 if "agent" in entry}
        for agent, name in names.items():
            path = cast_config.agents.get(name)
            if path:
                self.statics[agent] = CastActor(Path(path), spec, stage, device, cast_config.deterministic)
        if cast_config.opponents and self.rule is not None and self.rule.has_opponents():
            first = Path(cast_config.parent) if getattr(cast_config, "parent", "") else parent
            self.pool = CastPool(cast_config, spec, stage, run_dir, device,
                                 first if cast_config.opponents in (AUTO, LEAGUE) else Path(cast_config.opponents))
            if cast_config.opponents == LEAGUE:
                self.pool.reload()
        envs = spec.num_envs
        # The share of the cast episodes the exploiter plays while there is one (animus.exploit sets it).
        self.exploit_share = 0.0
        self.cast_env = np.zeros(envs, dtype=bool)
        self.member = np.full(envs, -1, dtype=np.int64)
        self.begin_episodes(np.ones(envs, dtype=bool))
        self.cast_rows_total = 0
        self.rows_total = 0
        self.fallback_total = 0
        self.last_rows: np.ndarray | None = None
        self.last_present: np.ndarray | None = None

    @property
    def enabled(self) -> bool:
        return bool(self.statics) or (self.pool is not None and bool(self.pool.active()))

    def begin_episodes(self, envs: np.ndarray) -> None:
        """New episodes start in these envs: draw whether their far side is cast this time, and by whom."""
        if self.pool is None or not self.pool.active():
            self.cast_env[envs] = False
            self.member[envs] = -1
            return
        count = int(envs.sum())
        self.cast_env[envs] = self.rng.random(count) < self.config.opponent_share
        drawn = np.asarray(self.pool.draw(count, self.rng), dtype=np.int64)
        # Of the cast episodes, exploit_share are the exploiter's: the league's draw keeps its weights over the rest.
        if self.exploit_share > 0.0:
            drawn[self.rng.random(count) < self.exploit_share] = EXPLOITER_MEMBER
        self.member[envs] = drawn

    def exploiter_rows(self, rows: np.ndarray) -> np.ndarray:
        """Of this decision's cast rows [E, A], the exploiter's."""
        return rows & (self.cast_env & (self.member == EXPLOITER_MEMBER))[:, None]

    def exploiter_results(self, step, won_column: int | None) -> list[float]:
        """The exploiter's `won` in the episodes it played that ended this decision (its seats' mean)."""
        if won_column is None or self.last_rows is None or not step.done.any():
            return []
        results = []
        for env in np.nonzero(step.done)[0]:
            if not self.cast_env[env] or self.member[env] != EXPLOITER_MEMBER:
                continue
            seats = self.last_rows[env]
            if seats.any():
                results.append(float(step.episode_info[env][seats, won_column].mean()))
        return results

    def rows(self, step) -> np.ndarray:
        """[E, A] bool: the rows a frozen actor answers this decision."""
        agents = step.layout.shape[1]
        rows = np.zeros(step.layout.shape, dtype=bool)
        if self.rule is None:
            self.last_rows, self.last_present = rows, step.present
            return rows
        if self.pool is not None and self.cast_env.any():
            opponents = self.rule.opponent_rows(host(step.state), agents)
            rows |= opponents & self.cast_env[:, None] & step.present
        if self.statics:
            static = self.rule.static_rows(step.present)
            static[:, [agent for agent in range(agents) if agent not in self.statics]] = False
            rows |= static
        self.last_rows, self.last_present = rows, step.present
        self.rows_total += int(step.present.sum())
        self.cast_rows_total += int(rows.sum())
        return rows

    def act(self, step, live_actions: np.ndarray, rows: np.ndarray) -> np.ndarray:
        return self.act_and_look(step, live_actions, rows)[0]

    def act_and_look(self, step, live_actions: np.ndarray, rows: np.ndarray,
                     look: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray | None]:
        """The cast rows' actions over `live_actions`, and their look over `look` (the live policy's [E, A, heads],
        None without one): each frozen actor reads its rows' camera bytes (step.image, where they are) and sends its
        own look, the hold for one without a camera."""
        actions = live_actions.copy()
        obs, mask, image = host(step.obs), host(step.mask), getattr(step, "image", None)
        if self.pool is not None:
            for index in np.unique(self.member[self.cast_env]):
                if index < 0:
                    continue
                mine = rows & (self.member == index)[:, None] & self.cast_env[:, None]
                for agent in self.statics:
                    mine[:, agent] = False
                if mine.any():
                    actor = self.pool.actor_of(self.pool.members[int(index)])
                    actions, look = actor.decide(obs, mask, step.layout, mine, actions, image, look)
                    self.fallback_total += actor.fallback_rows
        for agent, actor in self.statics.items():
            mine = np.zeros_like(rows)
            mine[:, agent] = rows[:, agent]
            if mine.any():
                actions, look = actor.decide(obs, mask, step.layout, mine, actions, image, look)
                self.fallback_total += actor.fallback_rows
        return actions, look

    def observe_ended(self, step, won_column: int | None) -> None:
        """Episodes ended in the envs `step.done`: the live seats' `won` against the member that played them."""
        if self.pool is None or won_column is None or self.last_rows is None or not step.done.any():
            return
        for env in np.nonzero(step.done)[0]:
            if not self.cast_env[env] or self.member[env] < 0:
                continue
            live = self.last_present[env] & ~self.last_rows[env]
            if not live.any():
                continue
            won = float(step.episode_info[env][live, won_column].mean())
            self.pool.record(int(self.member[env]), won)

    def clear(self, done: np.ndarray) -> None:
        """Episodes ended: cast memories go, and the next episodes draw their members afresh."""
        if self.pool is not None:
            for member in self.pool.members:
                if member.actor is not None:
                    member.actor.clear(done)
        for actor in self.statics.values():
            actor.clear(done)
        self.begin_episodes(done)

    def reset_all(self) -> None:
        """Every env starts a fresh episode (after an evaluation): nothing remembered, members redrawn."""
        if self.pool is not None:
            for member in self.pool.members:
                if member.actor is not None:
                    member.actor.reset_all()
        for actor in self.statics.values():
            actor.reset_all()
        self.begin_episodes(np.ones(self.spec.num_envs, dtype=bool))

    def league_stats(self, layout_names: list[str]) -> dict[str, float]:
        """The live policy's win rate against the hardest member, for every class (the pool is not per class)."""
        rate = self.pool.hardest_win_rate() if self.pool is not None else None
        return {name: rate for name in layout_names} if rate is not None else {}

    def stats(self) -> dict[str, float]:
        share = self.cast_rows_total / self.rows_total if self.rows_total else 0.0
        out = {"cast_rows": float(share), "cast_fallback_rows": float(self.fallback_total)}
        if self.pool is not None:
            out["cast_members"] = float(len(self.pool.active()))
            rate = self.pool.hardest_win_rate()
            out["cast_hardest_win_rate"] = float(rate) if rate is not None else float("nan")
        self.cast_rows_total = self.rows_total = self.fallback_total = 0
        return out
