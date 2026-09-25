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

The evaluation never runs a cast actor: the sim's ``fight`` baseline stays the fixed yardstick on the far side
(``eval.opponent_baseline``), so scores stay comparable across runs.

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
from .stages import arena_state_span

LEAGUE_DIR = "league"
LEAGUE_FILE = "league.json"
AUTO = "auto"
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
    (animus.distill.build_teacher), its own memory and goals per row, cleared with the episode."""

    def __init__(self, path: Path, spec, stage: dict | None, device, deterministic: bool = False):
        self.path = Path(path)
        checkpoint = torch.load(self.path, map_location="cpu", weights_only=False)
        if checkpoint.get("stage") is None and (self.path.parent / "stage.json").is_file():
            checkpoint["stage"] = json.loads((self.path.parent / "stage.json").read_text())
        self.teacher: Teacher = build_teacher(checkpoint, spec, stage, device)
        self.name = self.teacher.name
        self.device = device
        self.deterministic = deterministic
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
        self.fallback_rows = 0

    def ensure(self, envs: int, agents: int) -> None:
        if self.memory is None or self.memory.shape[:2] != (envs, agents):
            self.memory = np.zeros((envs, agents, self.recurrent), dtype=np.float32)
            self.goal = np.zeros((envs, agents), dtype=np.int64)
            self.age = np.zeros((envs, agents), dtype=np.int64)

    def clear(self, done: np.ndarray) -> None:
        if self.memory is not None:
            self.memory[done] = 0.0
            self.goal[done] = 0
            self.age[done] = 0

    def reset_all(self) -> None:
        self.memory = None

    @torch.no_grad()
    def act(self, obs: np.ndarray, mask: np.ndarray, layout: np.ndarray, rows: np.ndarray,
            fallback: np.ndarray) -> np.ndarray:
        """Actions for the `rows` of obs [E, A, O] / mask [E, A, N] / layout [E, A]; other rows keep `fallback`.
        A row whose layout the checkpoint lacks, or whose legal actions it has none of, keeps its fallback too."""
        envs, agents = layout.shape
        self.ensure(envs, agents)
        actions = fallback.copy()
        self.fallback_rows = 0
        flat = np.nonzero(rows.reshape(-1))[0]
        if not len(flat):
            return actions
        flat_layout = layout.reshape(-1)
        flat_obs = obs.reshape(envs * agents, -1)
        flat_mask = mask.reshape(envs * agents, -1)
        flat_memory = self.memory.reshape(envs * agents, -1)
        flat_goal = self.goal.reshape(-1)
        flat_age = self.age.reshape(-1)
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
            features = self.teacher.actor.features(t_obs, t_layout, memory)
            goal = None
            if self.goal_count:
                ages = flat_age[picked]
                goals = flat_goal[picked].copy()
                choose = ages % self.goal_every == 0
                if choose.any():
                    drawn = self.teacher.actor.goal_distribution(features)
                    chosen = (drawn.probs.argmax(-1) if self.deterministic else drawn.sample()).cpu().numpy()
                    goals[choose] = chosen[choose]
                flat_goal[picked] = goals
                flat_age[picked] = ages + 1
                goal = torch.as_tensor(goals, dtype=torch.long, device=self.device)
            dist = self.teacher.actor.action_distribution(features, t_layout, t_mask, goal)
            teacher_actions = (dist.probs.argmax(-1) if self.deterministic else dist.sample()).cpu().numpy()
            if self.recurrent:
                flat_memory[picked] = features.cpu().numpy()
            back = self.back[int(index)]
            for row, action, ok in zip(picked, teacher_actions, usable):
                if ok:
                    flat_actions[row] = back.get(int(action), flat_actions[row])
        return actions


# ------------------------------------------------------------------ the league

@dataclass
class Member:
    path: Path
    actor: CastActor | None = None
    fights: int = 0
    rate: float = 0.5  # the live policy's win rate against it, an EMA over cast.rate_window fights
    retired: bool = False
    order: int = 0  # the order it joined in: the newest are never pruned or retired

    def to_json(self) -> dict:
        return {"path": str(self.path), "fights": self.fights, "win_rate": round(self.rate, 4),
                "retired": self.retired, "order": self.order}


class CastPool:
    """The league: members drawn by how hard they still are, snapshots added on a clock, beaten ones retired."""

    def __init__(self, cast_config, spec, stage, run_dir: Path, device, parent: Path | None):
        self.config = cast_config
        self.spec, self.stage, self.device = spec, stage, device
        self.run_dir = Path(run_dir)
        self.members: list[Member] = []
        self.counter = 0
        if parent is not None:
            self.add(Path(parent))

    def add(self, path: Path) -> Member | None:
        path = Path(path)
        if any(member.path == path for member in self.members) or not path.is_file():
            return None
        member = Member(path=path, order=self.counter)
        self.counter += 1
        self.members.append(member)
        self.prune()
        return member

    def actor_of(self, member: Member) -> CastActor:
        if member.actor is None:
            member.actor = CastActor(member.path, self.spec, self.stage, self.device, self.config.deterministic)
        return member.actor

    def active(self) -> list[Member]:
        return [member for member in self.members if not member.retired]

    def newest(self, count: int) -> set[int]:
        return {member.order for member in sorted(self.members, key=lambda m: -m.order)[:count]}

    def hardest(self) -> Member | None:
        fought = [member for member in self.active() if member.fights > 0]
        return min(fought, key=lambda member: member.rate) if fought else None

    def weights(self) -> np.ndarray:
        active = self.active()
        return np.array([(1.0 - member.rate) ** 2 + self.config.floor for member in active], dtype=np.float64)

    def draw(self, count: int, rng: np.random.Generator) -> list[int]:
        """Indexes into `members` for `count` episodes, by prioritised fictitious self-play weights."""
        active = self.active()
        if not active:
            return [-1] * count
        weights = self.weights()
        weights = weights / weights.sum()
        indexes = [self.members.index(member) for member in active]
        return [indexes[i] for i in rng.choice(len(active), size=count, p=weights)]

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


def league_snapshot(run_dir: Path, source: Path, tag: str) -> Path | None:
    """Copy `source` (a checkpoint) into <run_dir>/league/<tag>.pt; None when it is already there or missing."""
    source = Path(source)
    if not source.is_file():
        return None
    league = Path(run_dir) / LEAGUE_DIR
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
        drawn = self.pool.draw(count, self.rng)
        self.member[envs] = np.asarray(drawn, dtype=np.int64)

    def rows(self, step) -> np.ndarray:
        """[E, A] bool: the rows a frozen actor answers this decision."""
        agents = step.layout.shape[1]
        rows = np.zeros(step.layout.shape, dtype=bool)
        if self.rule is None:
            self.last_rows, self.last_present = rows, step.present
            return rows
        if self.pool is not None and self.cast_env.any():
            opponents = self.rule.opponent_rows(step.state, agents)
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
        actions = live_actions.copy()
        if self.pool is not None:
            for index in np.unique(self.member[self.cast_env]):
                if index < 0:
                    continue
                mine = rows & (self.member == index)[:, None] & self.cast_env[:, None]
                for agent in self.statics:
                    mine[:, agent] = False
                if mine.any():
                    actor = self.pool.actor_of(self.pool.members[int(index)])
                    actions = actor.act(step.obs, step.mask, step.layout, mine, actions)
                    self.fallback_total += actor.fallback_rows
        for agent, actor in self.statics.items():
            mine = np.zeros_like(rows)
            mine[:, agent] = rows[:, agent]
            if mine.any():
                actions = actor.act(step.obs, step.mask, step.layout, mine, actions)
                self.fallback_total += actor.fallback_rows
        return actions

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
