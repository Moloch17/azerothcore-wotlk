"""Co-op partners in party seats (config ``cast.partners``; dungeon-curriculum I7).

The bots must work with anyone -- above all a human collaborator who may lead, follow or take any role -- and a party
of five copies of one policy only ever learns to work with itself. So in a share of a party's episodes some of its
seats are played by **frozen partners**: earlier stages' best checkpoints (by stage name), any checkpoint by path, and
this run's own snapshots on a clock. The sim's "human" stand-in (StandIn.h) is the other half of the same lesson: one
seat the sim picks -- leading from seat 0 or following, in the role it wants -- whose row (present 2 on the wire,
Step.stand_in) a member of this same pool plays, never a script.

**The pool.** Each member's party outcome -- the live seats' mean of the stage's own outcome measure (``score``, the
evaluation's ``score_outcome`` by default) in the episodes it partnered -- is averaged over ``rate_window`` episodes.
Outcomes are not win rates in [0, 1], so they are normalised across the pool: the member the party does best with is
1, the worst 0, and a member not yet met counts as the worst so it is met. The draw weight is
``(1 - normalised)^2 + floor`` -- prioritised fictitious self-play weights: the
partners the party carries worst are met most, so the policy learns to carry weak or odd ones, and the floor keeps
every member in use. ``newest_share`` of the draws go to the newest snapshot whatever the weights say. Snapshots past
``pool_size`` are pruned, the best-carried first, never the newest ``keep_newest``; stage and path members stay.

**Which rows.** At an episode's first decision a party env (an arena whose stage.json plan is "party" or "raid") draws
whether it has partners (``share``), then up to ``max_partners`` of its present seats, always leaving at least one
live seat and never the seat a drill is about (stage.json's arena ``drill_seat``: G1's drilled role, whose lesson the
episode is for); each drawn seat gets a member whose checkpoint has that seat's layout (a member without it is never
drawn for it, so no partner row silently falls back to the live policy). Partner rows take the frozen actor's action and are
never samples; their episodes are left out of the training statistics.

**The stand-in.** Every row the sim marks as the stand-in's gets a member at its episode's first decision, whatever
``share`` says, and it is not one of the ``max_partners`` seats the share draws. The sim fields a stand-in only while
the learner says it can (MODE_FLAG_STAND_IN: ``Partners.can_field_stand_in``, a member for some layout); a stand-in row
whose layout no member has is played by the live policy and still never trained on (``stand_in_unfielded`` counts
them).

**Evaluation.** The plain evaluation is "all bots". The eval arm "with_partners" (eval.arms) plays a fixed partner set
(``eval_partners``, else the stage and path members) in every party, argmax, with its own random numbers seeded from
the evaluation's seed and the env index, so a given env count draws the same partners for the same episodes every
evaluation; the partners' rows are not scored (evaluation.run_evaluation's ``excluded``).
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .cast import CastActor, Residency, snapshot
from .device import host
from .stages import arena_state_span

PARTNERS_DIR = "partners"
PARTNERS_FILE = "partners.json"
PARTY_PLANS = ("party", "raid")
KIND_STAGE = "stage"
KIND_SNAPSHOT = "snapshot"


def partner_snapshot(run_dir: Path, source: Path, tag: str) -> Path | None:
    """Copy `source` into <run_dir>/partners/<tag>.pt; None when it is already there or missing."""
    return snapshot(run_dir, source, tag, PARTNERS_DIR)


# ------------------------------------------------------------------ which envs are parties

@dataclass
class PartyRule:
    """Which episodes are parties, from stage.json: an arena's plan, read off the critic state's arena one-hot (or, for
    a stage without one, every episode when every arena is a party)."""

    arena_first: int
    arena_count: int
    party: list[bool]
    seats: int
    # Per arena, the seat its drill is about (stage.json drill_seat; -1 none): a partner never plays it.
    drill_seats: list[int] | None = None

    @classmethod
    def from_stage(cls, stage: dict | None, agents_per_env: int) -> "PartyRule | None":
        arenas = (stage or {}).get("arenas") or []
        party = [str(arena.get("plan", "solo")) in PARTY_PLANS for arena in arenas]
        if not any(party):
            return None
        drill_seats = [int(arena.get("drill_seat", -1)) for arena in arenas]
        span = arena_state_span(stage)
        seats = min(int((stage or {}).get("seats", agents_per_env)), agents_per_env)
        if span is None:
            return cls(arena_first=-1, arena_count=0, party=party, seats=seats,
                       drill_seats=drill_seats) if all(party) else None
        return cls(arena_first=span[0], arena_count=span[1], party=party, seats=seats, drill_seats=drill_seats)

    def envs(self, state: np.ndarray) -> np.ndarray:
        """[E] bool: the envs whose episode is a party."""
        if self.arena_first < 0:
            return np.ones(state.shape[0], dtype=bool)
        onehot = state[:, self.arena_first:self.arena_first + self.arena_count]
        index = onehot.argmax(axis=-1)
        known = onehot.max(axis=-1) > 0.5
        party = np.array(self.party + [False], dtype=bool)
        return known & party[np.minimum(index, len(self.party))]

    def drill_seat(self, state: np.ndarray, env: int) -> int:
        """The seat env's arena drills (-1 none): the learner's own, never a partner's. Without an arena one-hot, the
        one seat every arena drills, if they agree."""
        seats = self.drill_seats or []
        if self.arena_first < 0:
            return seats[0] if seats and all(seat == seats[0] for seat in seats) else -1
        onehot = state[env, self.arena_first:self.arena_first + self.arena_count]
        if onehot.max() <= 0.5:
            return -1
        index = int(onehot.argmax())
        return seats[index] if index < len(seats) else -1


# ------------------------------------------------------------------ the pool

@dataclass
class Partner:
    path: Path
    kind: str = KIND_STAGE
    order: int = 0
    actor: CastActor | None = None
    episodes: int = 0
    score: float = math.nan  # the party's outcome with it, an average over rate_window episodes
    retired: bool = False
    layouts: frozenset = frozenset()  # the stage layouts its checkpoint plays (read once, when it joins)

    @property
    def name(self) -> str:
        return f"{self.path.parent.name}/{self.path.name}" if self.kind == KIND_STAGE else self.path.name

    def to_json(self) -> dict:
        return {"path": str(self.path), "kind": self.kind, "episodes": self.episodes,
                "party_score": None if math.isnan(self.score) else round(self.score, 4), "retired": self.retired}


class PartnerPool:
    """The partners, drawn by how badly the party does with them."""

    def __init__(self, config, spec, stage: dict | None, run_dir: Path, device, deterministic: bool | None = None):
        self.config = config
        self.spec, self.stage, self.device = spec, stage, device
        self.run_dir = Path(run_dir)
        self.deterministic = config.deterministic if deterministic is None else deterministic
        self.members: list[Partner] = []
        self.counter = 0
        self.missing: list[str] = []
        self.unusable: list[str] = []
        # At most cast.partners.resident_members frozen actors on the device, the rest offloaded (animus.cast).
        self.residency = Residency(getattr(config, "resident_members", 0))

    def add(self, path: Path, kind: str = KIND_STAGE) -> Partner | None:
        path = Path(path)
        if any(member.path == path for member in self.members):
            return None
        if not path.is_file():
            self.missing.append(str(path))
            return None
        member = Partner(path=path, kind=kind, order=self.counter)
        # Built now, so a checkpoint the frozen actor cannot play is named at once rather than failing mid-rollout: a
        # camera this stage cannot feed it (animus.distill.check_camera), weights that do not load.
        try:
            member.layouts = frozenset(self.actor_of(member).teacher.layouts)
        except (ValueError, KeyError, RuntimeError) as error:
            self.residency.forget(member.actor)
            member.actor = None
            self.unusable.append(f"{path} ({error})")
            return None
        self.counter += 1
        self.members.append(member)
        self.prune()
        return member

    def actor_of(self, member: Partner) -> CastActor:
        if member.actor is None:
            member.actor = CastActor(member.path, self.spec, self.stage, self.device, self.deterministic)
        return self.residency.touch(member.actor)

    def active(self) -> list[Partner]:
        return [member for member in self.members if not member.retired]

    def covers(self, member: Partner, layout: int) -> bool:
        """Whether the member's checkpoint has stage layout `layout` (it would fall back to the live policy else)."""
        return int(layout) in member.layouts

    def newest_snapshot(self) -> Partner | None:
        snapshots = [member for member in self.active() if member.kind == KIND_SNAPSHOT]
        return max(snapshots, key=lambda member: member.order) if snapshots else None

    def normalised(self, members: list[Partner]) -> np.ndarray:
        """Each member's party outcome across the pool: best 1, worst 0, one not yet met 0."""
        scores = np.array([member.score for member in members], dtype=np.float64)
        met = ~np.isnan(scores)
        out = np.zeros(len(members), dtype=np.float64)
        if met.sum() >= 2:
            low, high = scores[met].min(), scores[met].max()
            if high - low > 1e-9:
                out[met] = (scores[met] - low) / (high - low)
            else:
                out[met] = 0.5
        elif met.sum() == 1:
            out[met] = 0.5
        return out

    def probabilities(self, members: list[Partner]) -> np.ndarray:
        """The draw over `members`: (1 - normalised)^2 + floor, the newest snapshot lifted to newest_share."""
        weights = (1.0 - self.normalised(members)) ** 2 + self.config.floor
        p = weights / weights.sum()
        newest = self.newest_snapshot()
        share = float(self.config.newest_share)
        if newest is not None and share > 0.0 and newest in members and len(members) > 1:
            at = members.index(newest)
            rest = np.delete(p, at)
            rest = rest / rest.sum() * (1.0 - max(share, p[at])) if rest.sum() > 0 else rest
            p = np.insert(rest, at, max(share, p[at]))
        return p

    def draw_for(self, layout: int, rng: np.random.Generator) -> int:
        """A member index for a seat of stage layout `layout`, or -1 when no member has it."""
        members = [member for member in self.active() if self.covers(member, layout)]
        if not members:
            return -1
        chosen = members[int(rng.choice(len(members), p=self.probabilities(members)))]
        return self.members.index(chosen)

    def record(self, index: int, score: float) -> None:
        if index < 0 or index >= len(self.members) or not math.isfinite(score):
            return
        member = self.members[index]
        member.episodes += 1
        if math.isnan(member.score):
            member.score = float(score)
        else:
            alpha = 1.0 / min(member.episodes, max(1, self.config.rate_window))
            member.score += alpha * (float(score) - member.score)

    def prune(self) -> None:
        """Keep pool_size snapshots: the best carried go first, never the newest keep_newest."""
        snapshots = [member for member in self.active() if member.kind == KIND_SNAPSHOT]
        if len(snapshots) <= self.config.pool_size:
            return
        keep = {member.order for member in sorted(snapshots, key=lambda m: -m.order)[:self.config.keep_newest]}
        candidates = [member for member in snapshots if member.order not in keep]
        while len(snapshots) > self.config.pool_size and candidates:
            normalised = dict(zip((m.order for m in snapshots), self.normalised(snapshots)))
            victim = max(candidates, key=lambda member: (normalised[member.order], -member.order))
            victim.retired = True
            candidates.remove(victim)
            snapshots = [member for member in snapshots if not member.retired]

    def reload(self) -> int:
        """Snapshots from <run_dir>/partners/ not yet in the pool; returns how many joined."""
        folder = self.run_dir / PARTNERS_DIR
        if not folder.is_dir():
            return 0
        joined = 0
        for path in sorted(folder.glob("*.pt"), key=lambda p: p.stat().st_mtime):
            if self.add(path, KIND_SNAPSHOT) is not None:
                joined += 1
        return joined

    def to_json(self) -> dict:
        active = self.active()
        probabilities = self.probabilities(active).tolist() if active else []
        members = []
        for member in self.members:
            entry = member.to_json()
            entry["draw_share"] = round(probabilities[active.index(member)], 4) if member in active else 0.0
            members.append(entry)
        return {"members": members, "active": len(active), "missing": self.missing, "unusable": self.unusable}

    def write(self) -> None:
        (self.run_dir / PARTNERS_FILE).write_text(json.dumps(self.to_json(), indent=2))


# ------------------------------------------------------------------ the facade

class Partners:
    """What the training loop (and the "with_partners" eval arm) talks to: which rows a partner plays this decision,
    their actions, and the pool's bookkeeping at episode ends."""

    def __init__(self, config, spec, stage: dict | None, run_dir: Path, device, members: list[str],
                 seed: int = 0, share: float | None = None, deterministic: bool | None = None,
                 snapshots: bool = True):
        self.config = config
        self.spec = spec
        self.rule = PartyRule.from_stage(stage, spec.agents_per_env)
        self.share = config.share if share is None else share
        self.rng = np.random.default_rng(seed)
        self.pool = PartnerPool(config, spec, stage, run_dir, device, deterministic)
        for path in members:
            self.pool.add(Path(path), KIND_STAGE)
        if snapshots:
            self.pool.reload()
        envs, agents = spec.num_envs, spec.agents_per_env
        # Per row, the member that plays it this episode (-1: the live policy); drawn at an episode's first decision.
        self.assigned = np.full((envs, agents), -1, dtype=np.int64)
        self.fresh = np.ones(envs, dtype=bool)
        self.rows_total = 0
        self.partner_rows_total = 0
        self.fallback_total = 0
        self.episodes_with = 0
        self.stand_in_rows_total = 0
        self.stand_in_unfielded_total = 0

    @property
    def enabled(self) -> bool:
        return self.rule is not None and bool(self.pool.active()) and self.share > 0.0

    @property
    def can_field_stand_in(self) -> bool:
        """Whether a member could play the "human" stand-in's row: a party stage with someone in the pool (the learner
        says so to the sim with MODE_FLAG_STAND_IN; without it the sim fields no stand-in)."""
        return self.rule is not None and bool(self.pool.active())

    def draw(self, step) -> None:
        """The envs starting an episode at this decision: whether they have partners, which seats, and whom."""
        fresh = np.flatnonzero(self.fresh)
        if not len(fresh):
            return
        self.fresh[fresh] = False
        self.assigned[fresh] = -1
        if not self.can_field_stand_in:
            return
        present = np.asarray(step.present, dtype=bool)
        # The stand-in's seat first (the sim drew it): a member for its layout, whatever the share says.
        stand_in = getattr(step, "stand_in", None)
        if stand_in is not None:
            stand_in = np.asarray(stand_in, dtype=bool) & present
            for env in fresh:
                for seat in np.flatnonzero(stand_in[env]):
                    self.assigned[env, seat] = self.pool.draw_for(int(step.layout[env, seat]), self.rng)
                    self.stand_in_rows_total += 1
                    self.stand_in_unfielded_total += int(self.assigned[env, seat] < 0)
        if not self.enabled:
            return
        state = host(step.state)
        party = self.rule.envs(state)
        for env in fresh:
            if not party[env] or self.rng.random() >= self.share:
                continue
            drilled = self.rule.drill_seat(state, int(env))
            standing_in = stand_in[env] if stand_in is not None else np.zeros(present.shape[1], dtype=bool)
            seats = [seat for seat in range(min(self.rule.seats, present.shape[1]))
                     if present[env, seat] and seat != drilled and not standing_in[seat]]
            # Always at least one live seat: the policy is what is being trained (a drill's seat is one already).
            live_drilled = 0 <= drilled < present.shape[1] and bool(present[env, drilled])
            room = min(self.config.max_partners, len(seats) - (0 if live_drilled else 1))
            if room <= 0:
                continue
            count = int(self.rng.integers(1, room + 1))
            chosen = self.rng.choice(len(seats), size=count, replace=False)
            for pick in sorted(int(index) for index in chosen):
                seat = seats[pick]
                self.assigned[env, seat] = self.pool.draw_for(int(step.layout[env, seat]), self.rng)
            self.episodes_with += int((self.assigned[env] >= 0).any())

    def rows(self, step) -> np.ndarray:
        """[E, A] bool: the rows a partner answers this decision."""
        self.draw(step)
        rows = (self.assigned >= 0) & np.asarray(step.present, dtype=bool)
        self.rows_total += int(np.asarray(step.present).sum())
        self.partner_rows_total += int(rows.sum())
        return rows

    def act_and_look(self, step, live_actions: np.ndarray, rows: np.ndarray,
                     look: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray | None]:
        """The partner rows' actions over `live_actions` and their look over `look` (the live policy's [E, A, heads],
        None without one): each partner reads its rows' camera bytes (step.image, where they are) and sends its own
        look, the hold for one without a camera."""
        actions = live_actions.copy()
        if not rows.any():
            return actions, look
        obs, mask, image = host(step.obs), host(step.mask), getattr(step, "image", None)
        for index in np.unique(self.assigned[rows]):
            mine = rows & (self.assigned == index)
            actor = self.pool.actor_of(self.pool.members[int(index)])
            actions, look = actor.decide(obs, mask, step.layout, mine, actions, image, look)
            self.fallback_total += actor.fallback_rows
        return actions, look

    def ended_rows(self, done: np.ndarray) -> np.ndarray:
        """[n, A] bool: the partner rows of the episodes ending in envs `done` (before clear)."""
        return self.assigned[np.asarray(done, dtype=bool)] >= 0

    def party_score(self, info: np.ndarray, env: int, score_column: int, present_column: int | None) -> float:
        """The live seats' mean of the score column in env's ended episode (info [A, K]); nan without any."""
        live = self.assigned[env] < 0
        if present_column is not None:
            live &= info[:, present_column] > 0.0
        return float(info[live, score_column].mean()) if live.any() else math.nan

    def observe_ended(self, step, score_column: int | None, present_column: int | None) -> None:
        """Episodes ended in envs `step.done`: each member that partnered one is scored by the party's outcome."""
        if score_column is None or not step.done.any():
            return
        for env in np.flatnonzero(step.done):
            members = {int(index) for index in self.assigned[env] if index >= 0}
            if not members:
                continue
            score = self.party_score(step.episode_info[env], int(env), score_column, present_column)
            for index in members:
                self.pool.record(index, score)

    def clear(self, done: np.ndarray) -> None:
        """Episodes ended: the partners' memories go, and the next episodes draw afresh."""
        done = np.asarray(done, dtype=bool)
        for member in self.pool.members:
            if member.actor is not None:
                member.actor.clear(done)
        self.assigned[done] = -1
        self.fresh |= done

    def reset_all(self) -> None:
        for member in self.pool.members:
            if member.actor is not None:
                member.actor.reset_all()
        self.assigned[:] = -1
        self.fresh[:] = True

    def stats(self) -> dict[str, float]:
        share = self.partner_rows_total / self.rows_total if self.rows_total else 0.0
        out = {"partner_rows": float(share), "partner_fallback_rows": float(self.fallback_total),
               "partner_members": float(len(self.pool.active())), "partner_episodes": float(self.episodes_with),
               "stand_in_episodes": float(self.stand_in_rows_total),
               "stand_in_unfielded": float(self.stand_in_unfielded_total)}
        self.rows_total = self.partner_rows_total = self.fallback_total = self.episodes_with = 0
        self.stand_in_rows_total = self.stand_in_unfielded_total = 0
        return out


def with_partners_chooser(choose, partners: Partners):
    """The "with_partners" eval arm's chooser -- and the "with_human" arm's, whose Partners has share 0 and so plays
    only the stand-in's rows: the learner's actions with the fixed partners' over their rows, and the rows to leave
    unscored (run_evaluation's `excluded`)."""

    def chooser(step):
        partners.clear(step.done)
        chosen = choose(step)
        # (actions), (actions, goals) or (actions, goals, look): run_evaluation's shapes.
        actions, goals, look = (tuple(chosen) + (None,) * (3 - len(chosen)) if isinstance(chosen, tuple)
                                else (chosen, None, None))
        rows = partners.rows(step)
        if rows.any():
            actions, look = partners.act_and_look(step, np.asarray(actions), rows,
                                                  None if look is None else np.asarray(look))
        if look is not None:
            return actions, goals, look
        return (actions, goals) if goals is not None else actions

    def excluded(env: int) -> np.ndarray:
        return partners.assigned[env] >= 0

    return chooser, excluded
