"""Seeded evaluation and convergence detection.

An evaluation switches the sim to seeded episodes (protocol MODE): episode seed index i builds the same
characters and opponents every time, so two checkpoints -- or a checkpoint and the random baseline -- are
scored on exactly the same situations. Combat rolls (crits, misses, creature choices during the fight) stay
random, so the scores are averages over the seeds, not replays.

The score is the mean episode return over every agent of every seeded episode: the scenario's own reward summed
over each episode, so it measures what training optimises and is comparable between checkpoints of one scenario
(not between scenarios). Summaries also break it down by level band, by layout (the class), by (class, role) and,
for a stage that
mixes arenas, by arena, each with the standard error of its score: combat rolls make two evaluations of the same
networks differ, and the convergence test only counts an improvement that stands out from that noise.

"""

from __future__ import annotations

import math
import time
from dataclasses import dataclass, field, replace
from typing import Callable

import numpy as np

from . import protocol as p
from .device import host
from .episode_means import PER_EVENT, event_weights

LEVEL_BANDS = ((1, 20), (21, 40), (41, 60), (61, 80))

# How a character's talents were spent, by the episode info column "talent_plan" (SeatCharacter::TalentPlan).
# Scored as its own group so a run shows whether the policy plays a build it was not handed the recipe for.
TALENT_PLANS = ("standard", "noisy", "random")
# There is no role. A seat is summarised by the build it drew (episode info "spec", named per class by
# stage.json's spec_names), which is finer where it matters: a feral cat and a balance druid were one role and are
# not equally hard to win with.

# Ratios of sums over an evaluation's episodes (EvalResult.summary), on the movement stages that report the columns.
RATIO_METRICS = ("arrived_at_rung",)

#: The episode info column an evaluation is scored on by default: the episode's Outcome and Cost terms before any
#: rung's tier and any role's scale (RewardLedger::Score, peak-play plan W0).
SCORE_COLUMN = "score_outcome"


@dataclass
class EvalResult:
    policy: str  # "learner" or the baseline's name
    returns: np.ndarray  # [n] episode returns, one per agent of each seeded episode, in (seed, agent) order
    infos: np.ndarray  # [n, K] episode info, same order
    info_names: tuple[str, ...]
    layouts: tuple[str, ...] = ()  # [n] each agent's layout name
    seeds: tuple[int, ...] = ()  # [n] the seed index of each row's episode, for the per-episode log
    arenas: tuple[str, ...] = ()  # the stage's arena names, indexed by the episode info column "arena"
    seconds: float = 0.0
    decisions: int = 0
    # [n, num_actions] how often each row's agent took each action during its episode (the learner's choices; empty
    # for a baseline, whose actions the sim picks), and per layout the actions' names (stage.json
    # "action_names"), for the per-episode log.
    action_counts: np.ndarray | None = None
    # [n, num_actions] how many of the same decisions allowed each action (the mask), so an action the policy never
    # takes can be told from one it was never offered (a missing reagent, a spell the character doesn't have).
    allowed_counts: np.ndarray | None = None
    action_names: dict[str, list[str]] = field(default_factory=dict)
    # Per layout, its class's build names in the order the "spec" episode info column indexes them.
    spec_names: dict[str, list[str]] = field(default_factory=dict)
    # Episode info columns that index a list of names (stage.json episode_categories: the seek stage's seek_room and
    # seek_object), each column's names in index order: the summary splits by each ("categories").
    categories: dict[str, list[str]] = field(default_factory=dict)
    # Decision by decision, for the first `eval.trace_episodes` seeded episodes: what the policy did and what it said
    # it was doing. A summary cannot show a plan -- the order of the decisions is the plan -- so this is what to read
    # when asking whether a bot saved a cooldown, rested before a pull or held an add.
    trace: list[dict] = field(default_factory=list)
    # The episode info column the score is read from (eval.score): SCORE_COLUMN, the episode's Outcome and Cost terms
    # before any rung's tier (RewardLedger::Score), so the yardstick does not move when shaping is turned down or a
    # ladder steps. "" -- or a stage.json from before the column -- scores the return, as before.
    score_column: str = SCORE_COLUMN
    # The scored seats' kinematic samples (protocol 20), one [T, SAMPLE_DIM] track per seat and episode, when the
    # evaluation was asked to keep them (run_evaluation's collect_motion): the realism score's input.
    motion_tracks: list = field(default_factory=list)
    # One (seed index, agent, layout name, episode info row [K]) per motion track, in the tracks' order: the seed is
    # the "seed" of eval_episodes.jsonl, so a route can be matched to its episode (Trainer.save_routes).
    motion_ids: list = field(default_factory=list)

    @property
    def episodes(self) -> int:
        return len(self.returns)

    @property
    def scores(self) -> np.ndarray:
        """[n] what each row is scored on: the score column where the sim writes one, else the return."""
        values = self.column(self.score_column) if self.score_column else None
        return self.returns if values is None else values.astype(np.float64)

    @property
    def score(self) -> float:
        return float(self.scores.mean()) if len(self.returns) else float("nan")

    @property
    def stderr(self) -> float:
        return standard_error(self.scores, self._episode_of_row())

    def _episode_of_row(self) -> np.ndarray | None:
        """The episode each row belongs to, for standard_error; None when the rows were not labelled."""
        return np.asarray(self.seeds) if len(self.seeds) == len(self.returns) else None

    def episodes_log(self, columns: tuple[str, ...] | None = None) -> list[dict]:
        """One row per scored episode: its seed, layout, return, `columns` of its episode info (None: every column).

        The summaries average these away, and an average cannot say whether a class is a little worse
        everywhere or fine except for a handful of episodes it never finishes -- which is what a per-layout gate
        actually turns on. Why those episodes failed is in the columns no summary reports (the character's level and
        spec, the opponent, the form and distance it ended in), so the training run logs them all to
        eval_episodes.jsonl.
        """
        names = self.info_names if columns is None else tuple(c for c in columns if c in self.info_names)
        indices = [self.info_names.index(name) for name in names]
        rows = []
        for index in range(self.episodes):
            row = {
                "policy": self.policy,
                "seed": int(self.seeds[index]) if index < len(self.seeds) else -1,
                "layout": self.layouts[index] if index < len(self.layouts) else "",
                "return": round(float(self.returns[index]), 4),
            }
            for name, column in zip(names, indices):
                row[name] = round(float(self.infos[index, column]), 4)
            if self.action_counts is not None and index < len(self.action_counts):
                row["actions"] = self.actions_taken(index)
            if self.allowed_counts is not None and index < len(self.allowed_counts):
                row["allowed"] = self.actions_allowed(index)
            rows.append(row)
        return rows

    def actions_taken(self, index: int) -> dict[str, int]:
        """Row `index`'s actions other than the no-op, by name, with how often it took each: which spells, items
        and orders a class actually uses, which no episode info column can say."""
        return self._named_counts(index, self.action_counts[index])

    def actions_allowed(self, index: int) -> dict[str, int]:
        """Row `index`'s actions other than the no-op, by name, with how many of its decisions allowed each."""
        return self._named_counts(index, self.allowed_counts[index])

    def _named_counts(self, index: int, counts: np.ndarray) -> dict[str, int]:
        layout = self.layouts[index] if index < len(self.layouts) else ""
        names = self.action_names.get(layout, [])
        return {(names[action] if action < len(names) else str(action)): int(counts[action])
                for action in np.flatnonzero(counts) if action > 0}

    def column(self, name: str) -> np.ndarray | None:
        return self.infos[:, self.info_names.index(name)] if name in self.info_names else None

    @staticmethod
    def merged(parts: list["EvalResult"]) -> "EvalResult":
        """One evaluation from the results of its shares of seeds (data-parallel learners each play their own run
        of them), rows in rank order. Seconds are the slowest share's: they ran at once."""
        parts = [part for part in parts if part is not None]
        first = parts[0]
        if len(parts) == 1:
            return first

        def stacked(name):
            arrays = [getattr(part, name) for part in parts]
            return None if any(array is None for array in arrays) else np.concatenate(arrays)

        return EvalResult(
            policy=first.policy, returns=np.concatenate([part.returns for part in parts]),
            infos=np.concatenate([part.infos for part in parts]), info_names=first.info_names,
            layouts=tuple(layout for part in parts for layout in part.layouts),
            seeds=tuple(seed for part in parts for seed in part.seeds), arenas=first.arenas,
            seconds=max(part.seconds for part in parts), decisions=sum(part.decisions for part in parts),
            action_counts=stacked("action_counts"), allowed_counts=stacked("allowed_counts"),
            action_names=first.action_names, spec_names=first.spec_names, categories=first.categories,
            trace=[row for part in parts for row in part.trace], score_column=first.score_column,
            motion_tracks=[track for part in parts for track in part.motion_tracks],
            motion_ids=[ident for part in parts for ident in part.motion_ids])

    def failed_seeds(self, metric: str) -> list[int]:
        """Seed indexes of the episodes where some scored row fell short on `metric` (a 0/1 episode info column), for
        replaying them in training."""
        values = self.column(metric)
        if values is None or not len(self.seeds):
            return []
        return sorted({int(seed) for seed, value in zip(self.seeds, values) if value < 1.0})

    def summary(self, columns: tuple[str, ...]) -> dict:
        """Score and means of `columns`: overall, per level band, per layout, per arena, per talent build and per
        difficulty tier, and for each tier but the top one everything up to it, per layout too ("up_to")."""
        present = [c for c in columns if c in self.info_names]
        episodes = self._episode_of_row()
        scores = self.scores

        def means(rows: np.ndarray) -> dict:
            out = {
                "episodes": int(rows.sum()),
                "score": float(scores[rows].mean()) if rows.any() else None,
                "stderr": standard_error(scores[rows], episodes[rows] if episodes is not None else None),
                # The whole return, shaping and all: what training was paid, beside what it is judged on.
                "return": float(self.returns[rows].mean()) if rows.any() else None,
            }
            for name in present:
                values = self.column(name)[rows]
                # A per-event column (episode_means.PER_EVENT: arrive_seconds over the markers reached) is weighted
                # by its episode's events, so an episode that never arrived does not read as a zero-second arrival.
                count = PER_EVENT.get(name)
                weights = None
                if count is not None:
                    summed = event_weights(count, lambda c: self.column(c) if c in self.info_names else None)
                    weights = summed[rows] if summed is not None else None
                if weights is not None:
                    total = float(weights.sum())
                    out[name] = float((values * weights).sum() / total) if total > 0 else None
                else:
                    out[name] = float(values.mean()) if len(values) else None
            out.update(ratios(rows))
            return out

        # M1's withholding ladder (SightEncounter, perception-goals REDESIGN amendment 7): an evaluation plays every
        # pair with the compass and without, half and half, whatever the rung, so its plain arrival rate is not the
        # rung's. arrived_at_rung is the arrival at the training rung's own mix -- the chance it withholds the
        # compass (compass_withhold_chance, the rung's even in an evaluation) times the arrival without it, the rest
        # times the arrival with it -- and is what the shaping fade and the cost ladder are gated on.
        withheld, compass_shown, arrived_no, arrived_with, chance = (
            self.column("compass_withheld"), self.column("compass_present"), self.column("arrived_no_compass"),
            self.column("arrived_with_compass"), self.column("compass_withhold_chance"))

        def ratios(rows: np.ndarray) -> dict:
            out = {}
            if all(column is not None for column in (withheld, compass_shown, arrived_no, arrived_with, chance)) \
                    and rows.any():
                p = float(chance[rows].mean())
                without = float(withheld[rows].sum())
                shown = float(compass_shown[rows].sum())
                rate_no = float(arrived_no[rows].sum()) / without if without > 0 else None
                rate_with = float(arrived_with[rows].sum()) / shown if shown > 0 else None
                if rate_with is not None and (rate_no is not None or p <= 0.0):
                    out["arrived_at_rung"] = p * (rate_no or 0.0) + (1.0 - p) * rate_with
                elif rate_no is not None and p >= 1.0:
                    out["arrived_at_rung"] = rate_no
                else:
                    out["arrived_at_rung"] = None
            return out

        everything = np.ones(self.episodes, dtype=bool)
        result = {"policy": self.policy, **means(everything), "bands": {}, "layouts": {}, "specs": {},
                  "castings": {}, "arenas": {}, "builds": {}, "difficulties": {}, "up_to": {},
                  "categories": {}}
        # By each named category (the seek stage's room and object: found by room, found by object type), under
        # "<column>=<name>"; a name no episode drew is left out.
        for column, labels in self.categories.items():
            values = self.column(column)
            if values is None:
                continue
            for index, label in enumerate(labels):
                rows = values == index
                if rows.any():
                    result["categories"][f"{column}={label}"] = means(rows)
        levels = self.column("level")
        if levels is not None:
            for low, high in LEVEL_BANDS:
                rows = (levels >= low) & (levels <= high)
                if rows.any():
                    result["bands"][f"{low}-{high}"] = means(rows)
        if len(set(self.layouts)) > 1:
            names = np.array(self.layouts)
            for layout in sorted(set(self.layouts)):
                result["layouts"][layout] = means(names == layout)
        specs = self.column("spec")
        # Each (class, build) on its own, which is the grain the sim draws at and the grain a class model can be
        # good at one part of and bad at another: one model plays every build its class has, so a paladin's healing
        # has to be scored apart from its tanking or the average hides it. A build is named per class, so the same
        # index means different things across layouts and the pair is the only meaningful key.
        if specs is not None and self.layouts and len(self.layouts) == self.episodes:
            names = np.array(self.layouts)
            for layout in sorted(set(self.layouts)):
                for index, spec in enumerate(self.spec_names.get(layout, [])):
                    rows = (names == layout) & (specs == index)
                    if rows.any():
                        result["castings"][f"{layout}_{spec}"] = means(rows)
                        result["specs"].setdefault(spec, {})
        # And each build name on its own across the classes that have one, for a run of several classes where
        # "restoration" is a thing two of them do.
        if specs is not None and self.layouts and len(self.layouts) == self.episodes:
            names = np.array(self.layouts)
            for spec in list(result["specs"]):
                rows = np.zeros(self.episodes, dtype=bool)
                for layout in sorted(set(self.layouts)):
                    named = self.spec_names.get(layout, [])
                    if spec in named:
                        rows |= (names == layout) & (specs == named.index(spec))
                if rows.any():
                    result["specs"][spec] = means(rows)
                else:
                    result["specs"].pop(spec, None)
        arenas = self.column("arena")
        if len(self.arenas) > 1 and arenas is not None:
            for index, arena in enumerate(self.arenas):
                rows = arenas == index
                if rows.any():
                    result["arenas"][arena] = means(rows)
        # The creature duel's difficulty tiers (episode info "difficulty"): an evaluation spreads its seeds over them.
        tiers = self.column("difficulty")
        if tiers is not None and len(set(tiers.tolist())) > 1:
            seen = sorted(set(int(t) for t in tiers.tolist()))
            for tier in seen:
                result["difficulties"][str(tier)] = means(tiers == tier)
            # The easier tiers together (target.base_difficulty): a stage whose ladder climbs above what it is judged
            # on still gates the classes on the fights below.
            names = np.array(self.layouts) if len(self.layouts) == self.episodes else None
            for tier in seen[:-1]:
                rows = tiers <= tier
                group = means(rows)
                group["layouts"] = {} if names is None else {
                    layout: means(rows & (names == layout)) for layout in sorted(set(self.layouts))}
                group["specs"] = {}
                if specs is not None and names is not None:
                    for layout in sorted(set(self.layouts)):
                        for index, spec in enumerate(self.spec_names.get(layout, [])):
                            picked = rows & (names == layout) & (specs == index)
                            if picked.any():
                                group["specs"][f"{layout}_{spec}"] = means(picked)
                result["up_to"][str(tier)] = group
        # The top of a ladder stage's ladder (episode info `at_top_rung`, else the highest of several difficulty
        # tiers): the episodes a ladder stage converges on (convergence.top_rung). Present, perhaps empty, on every
        # ladder stage, so an evaluation that never reached the top still says it is one.
        top = self.column("at_top_rung")
        top_rows = None
        if top is not None:
            top_rows = top > 0.5
        elif tiers is not None and len(set(tiers.tolist())) > 1:
            top_rows = tiers == max(tiers.tolist())
        if top_rows is not None:
            group = means(top_rows)
            names = np.array(self.layouts) if len(self.layouts) == self.episodes else None
            group["layouts"] = {} if names is None else {
                layout: means(top_rows & (names == layout)) for layout in sorted(set(self.layouts))}
            result["top_rung"] = group
        # The seek stage's gate reading (perception-goals P1: found in >= 90% of the evaluations in the deepest rooms,
        # episode info deep_room: the deepest third by walking distance).
        deep, found = self.column("deep_room"), self.column("found")
        if deep is not None and found is not None:
            rows = deep > 0.5
            result["found_deepest"] = float(found[rows].mean()) if rows.any() else None
        plans = self.column("talent_plan")
        if plans is not None and len(set(plans.tolist())) > 1:
            for index, plan in enumerate(TALENT_PLANS):
                rows = plans == index
                if rows.any():
                    result["builds"][plan] = means(rows)
        return result


def action_mask_table(names, layout_names, action_names: dict[str, list[str]], num_actions: int):
    """An evaluation-only action mask (eval.mask_actions): per layout, which action indexes `names` resolve to, as a
    [layouts, num_actions] table of what to forbid; None when there is nothing to forbid.

    Resolved per layout, because the same action sits at a different index in every class's catalog. A name no layout has is a mistake in the config, and raises rather than
    silently masking nothing.
    """
    names = tuple(names or ())
    if not names:
        return None
    table = np.zeros((len(layout_names), num_actions), dtype=bool)
    found = set()
    for index, layout in enumerate(layout_names):
        for action, name in enumerate(action_names.get(layout, [])):
            if name in names and action < num_actions:
                table[index, action] = True
                found.add(name)
    missing = sorted(set(names) - found)
    if missing:
        raise ValueError(f"eval.mask_actions names actions no layout has: {missing}")
    return table


def casting_weights(summary: dict, strength: float, max_ratio: float,
                    metric: str = "", roles: dict[str, str] | None = None,
                    role_metrics: dict | None = None) -> dict[str, float]:
    """How often training episodes should draw each (class, role), from how low its score is against the others'
    and, with `metric`, from how far it falls short on that summary field (higher is better).

    A stage is gated on its weakest class and role, so an episode of a pair that scores lowest is worth more than
    one of a pair that is already clear of the rest. Per pair and not per model: one model is a whole class now, and
    weighting a paladin that heals badly by its average would send it more tanking episodes it did not need. The
    score alone misses a pair that scores well yet fails an absolute gate, so the shortfall on the gated metric counts
    as well, whichever of the two is larger. Each is measured in its own standard deviations, so the weights do not
    depend on the size of the scenario's rewards, and the spread is capped: the heaviest pair draws at most
    `max_ratio` times the lightest, whatever the scores are. Weights average 1 (the even draw).
    """
    rows = summary.get("castings", {})
    names = [name for name, row in rows.items() if row.get("score") is not None]
    if not names or strength <= 0.0 or max_ratio <= 1.0:
        return {name: 1.0 for name in rows}

    def standardised(values: np.ndarray) -> np.ndarray:
        spread = float(values.std())
        return (values - values.mean()) / spread if spread > 1e-9 else np.zeros_like(values)

    need = standardised(-np.array([float(rows[name]["score"]) for name in names]))
    if metric and all(rows[name].get(metric) is not None for name in names):
        # The larger of the two needs, not their sum: a high score must not cancel a gate the layout is failing.
        need = np.maximum(need, standardised(-np.array([float(rows[name][metric]) for name in names])))
    need = np.maximum(need, role_needs(rows, names, roles or {}, role_metrics or {}))
    if not need.any():
        return {name: 1.0 for name in names}

    weights = np.exp(strength * np.clip(need, -3.0, 3.0))
    # Cap the spread, then centre on 1 so the total number of episodes is unchanged.
    limit = math.sqrt(max_ratio)
    weights = np.clip(weights / float(np.exp(np.log(weights).mean())), 1.0 / limit, limit)
    weights /= float(weights.mean())
    return {name: float(weight) for name, weight in zip(names, weights)}


def role_needs(rows: dict, names: list[str], roles: dict[str, str], role_metrics: dict) -> np.ndarray:
    """Each build's shortfall against the other builds of its role (roles: casting name -> role) on that role's fields
    (role_metrics: role -> names, a leading "-" for lower is better): the worst of them, in standard deviations over
    the role's builds. A role with one build, or a field a build lacks, adds nothing; a build with no role, nothing."""
    need = np.full(len(names), -np.inf)
    for role, fields in role_metrics.items():
        members = [index for index, name in enumerate(names) if roles.get(name) == role]
        if len(members) < 2:
            continue
        for field_name in fields:
            sign = -1.0 if field_name.startswith("-") else 1.0
            key = field_name.lstrip("-")
            if not all(rows[names[index]].get(key) is not None for index in members):
                continue
            values = sign * np.array([float(rows[names[index]][key]) for index in members])
            spread = float(values.std())
            if spread <= 1e-9:
                continue
            shortfall = -(values - values.mean()) / spread
            for slot, index in enumerate(members):
                need[index] = max(need[index], shortfall[slot])
    return need


def standard_error(values: np.ndarray, groups: np.ndarray | None = None) -> float:
    """Standard error of the mean; 0 with fewer than two values.

    `groups` labels the independent draw each value came from -- for an evaluation, the episode. A row is one
    agent, and the agents of one episode share its seed, its spawn, its opponents and its outcome, so they are not
    independent of each other: dividing by the square root of the agent count instead of the episode count
    understates the noise by up to sqrt(agents per episode), which in a party or raid stage is a factor of two or
    more. The gates spend this number (animus.gates.noise_allowance), and understating it passes stages that did
    not improve. Averaging each episode to one value first measures the spread the gate actually needs.
    """
    values = np.asarray(values, dtype=float)
    if groups is not None and len(groups) == len(values) and len(values):
        groups = np.asarray(groups)
        values = np.array([values[groups == group].mean() for group in np.unique(groups)])
    return float(np.std(values, ddof=1) / math.sqrt(len(values))) if len(values) > 1 else 0.0


#: The ablation eval arms (config.EVAL_ARMS): the learner's own input, edited, on the evaluation's seeds. The sim is not
#: told: it still withholds the compass and shows the flag as the episode asks, so the episode columns stay the plain
#: ones and the arm's gap to the plain evaluation is what the edited input carried.
#:   no_flag     the objective bit of every camera pixel cleared;
#:   no_camera   the camera's image replaced by the no-frame pixel (the mental map's crop stays: it is no_map's);
#:   no_compass  the compass block's observation columns zeroed;
#:   no_map      the map block: its observation columns zeroed and the mental map's crop (the bytes after the image in
#:               the camera's row) zeroed, which is what the sim sends a seat without a map (every cell unknown);
#:   no_memory   the recurrent state (the actor GRU memory, slow memory, held goal and its age, goal queue)
#:               reset to its episode-start value before every decision: the policy acts from the current observation.
#:   no_goal     the goal block's observation columns zeroed (m2-goals): no goal is on offer but the placeholder (Fight
#:               about no one), the held-goal compass reads 0 and the place slots are empty, which is the policy as it
#:               was before room goals;
#:   random_goal the goal head replaced by a uniform draw over the goals on offer (the primary only; nothing held beside
#:               it or queued): the follower without the planner. A learner-side flag, MappoTrainer.uniform_goals;
#:   random_cell the goal head as it is, but every cell goal's cell drawn uniformly over the choosable blocks of the
#:               map crop (free-choice-goals): what the cell pointer is worth. MappoTrainer.uniform_cells;
#:   no_plan     the goal head as it is, but nothing held beside the primary and nothing queued: what the chain of
#:               cells (the plan) is worth. MappoTrainer.single_goal.
ABLATIONS = ("no_flag", "no_camera", "no_compass", "no_map", "no_memory", "no_goal", "random_goal", "random_cell",
             "no_plan")
#: The ablations that are a flag on the trainer's goal draw (Run.evaluate_arms sets them), not an edit of the input.
GOAL_DRAW_ARMS = ("random_goal", "random_cell", "no_plan")
#: What an ablation arm reports and shows in forge status (<column>_<arm>).
ABLATION_COLUMNS = ("arrived", "arrived_no_compass", "arrived_with_compass", "objective_visible")
#: Bit 5 of the class byte (byte 3) of every pixel: the objective flag (Vision EncodePixel, cpp-vision.md).
OBJECTIVE_FLAG_BIT = 0x20
CLASS_BYTE = 3


def ablate_image(image: np.ndarray, arm: str, image_bytes: int | None = None) -> np.ndarray:
    """A copy of `image` [..., I] uint8 (4 bytes a pixel) as the arm leaves it: "no_flag" clears the objective bit of
    every pixel's class byte, "no_camera" is p.no_frame (every pixel NO_FRAME_PIXEL), "no_map" zeroes the mental map's
    crop, anything else as it was. A stage with a map block sends the camera's whole row, the image's `image_bytes`
    then the map's crop (protocol 24); the flag and camera arms edit the image part only, never the map's bytes."""
    image = np.array(host(image), dtype=np.uint8, copy=True)
    width = len(p.NO_FRAME_PIXEL)
    whole = image.shape[-1]
    seen = whole if image_bytes is None else min(int(image_bytes), whole)
    if arm == "no_map":
        image[..., seen:] = 0
        return image
    if arm == "no_camera":
        image[..., :seen] = p.no_frame((*image.shape[:-1], seen))
    elif arm == "no_flag":
        pixels = image[..., :seen].reshape(*image.shape[:-1], seen // width, width)
        pixels[..., CLASS_BYTE] &= np.uint8(~OBJECTIVE_FLAG_BIT & 0xFF)
        image[..., :seen] = pixels.reshape(*image.shape[:-1], seen)
    return image


def compass_spans(spec, stage: dict | None, block: str = "compass") -> list[tuple[int, int]]:
    """Per layout of `spec` (its order), the (first column, count) of the named block in its observation; (0, 0) for a
    layout without one. From the stage.json block spans (animus.stages.block_spans)."""
    from .stages import block_spans
    spans = []
    for layout in spec.layouts:
        found = (block_spans(stage, layout.name) or {}).get(block)
        spans.append((int(found[0][0]), int(found[0][1])) if found else (0, 0))
    return spans


def ablate_obs(obs: np.ndarray, layout: np.ndarray, spans: list[tuple[int, int]]) -> np.ndarray:
    """A copy of `obs` [E, A, O] with each row's block columns (its layout's span: the compass or the map) zeroed: what
    the sim writes when it withholds the compass (presence and values 0)."""
    obs = np.array(host(obs), copy=True)
    layout = np.asarray(host(layout))
    for index, (first, count) in enumerate(spans):
        if count:
            obs[layout == index, first:first + count] = 0.0
    return obs


def forget(acting, envs: int) -> None:
    """Reset what `acting` (trainer.ActingState) carries to its episode-start value in every env: the actor's and the
    critic's memory, the held goal and its age, the goal queue and the slow memory (ActingState.clear on all)."""
    acting.clear(np.ones(envs, dtype=bool))


def ablation_chooser(choose: Callable, arm: str, spec, stage: dict | None) -> Callable:
    """`choose` (run_evaluation's chooser) for an ablation arm: the step it is given has its image, compass or map
    edited first (`dataclasses.replace`: the run_evaluation's own step keeps the real bytes for scoring), or, for
    "no_memory", the acting state reset before it decides. `choose` from Trainer._acting carries its state as
    `choose.acting`."""
    if arm not in ABLATIONS:
        raise ValueError(f"unknown ablation arm {arm!r}; expected one of {list(ABLATIONS)}")
    block = {"no_compass": "compass", "no_map": "map", "no_goal": "goal"}.get(arm)
    spans = compass_spans(spec, stage, block) if block else []
    if block and not any(count for _, count in spans):
        raise ValueError(f"eval.arms.{arm}: the stage has no {block} block to zero")
    if arm == "no_memory":
        acting = getattr(choose, "acting", None)
        if acting is None or all(getattr(acting, name) is None for name in ("memory", "goal", "slow_memory", "queue")):
            raise ValueError("eval.arms.no_memory: the policy carries no memory, goal or queue to reset")

    if arm in GOAL_DRAW_ARMS:
        acting = getattr(choose, "acting", None)
        if acting is None or acting.goal is None:
            raise ValueError(f"eval.arms.{arm}: the policy chooses no goals")

    def chooser(step):
        if arm == "no_memory":
            forget(choose.acting, len(step.done))
            return choose(step)
        if arm in GOAL_DRAW_ARMS:
            # The draw itself is the trainer's (uniform_goals, uniform_cells, single_goal: set around the arm by
            # Run.evaluate_arms).
            return choose(step)
        if arm in ("no_compass", "no_goal"):
            return choose(replace(step, obs=ablate_obs(step.obs, step.layout, spans)))
        image = getattr(step, "image", None)
        if image is None:
            raise ValueError(f"eval.arms.{arm}: the stage has no camera image to edit")
        edited = replace(step, image=ablate_image(image, arm, getattr(spec, "image_bytes", None)))
        if arm == "no_map":
            edited = replace(edited, obs=ablate_obs(step.obs, step.layout, spans))
        return choose(edited)

    return chooser


def run_evaluation(env, spec, choose_actions, episodes: int, seed: int, baseline: str = "",
                   max_decisions: int | None = None,
                   arenas: tuple[str, ...] = (),
                   action_names: dict[str, list[str]] | None = None,
                   spec_names: dict[str, list[str]] | None = None,
                   categories: dict[str, list[str]] | None = None,
                   trace_episodes: int = 0, first_seed: int = 0,
                   any_playing: Callable[[bool], bool] | None = None,
                   score_column: str = SCORE_COLUMN, arena: int = 0,
                   collect_motion: bool = False, stand_in: bool = False, training_stand_in: bool = False,
                   excluded: Callable[[int], np.ndarray] | None = None) -> tuple[EvalResult, p.Step]:
    """Run seeded episodes first_seed..first_seed+episodes-1 (a data-parallel learner's share of an evaluation; 0..
    episodes-1 alone) and return their results and the fresh training STEP after them.

    choose_actions(step) -> [E, A] actions, or (actions, goals) from a policy with a goal head (the goals go to the
    sim, which scores and reports them), or (actions, goals or None, look) from one with the free look (protocol 22;
    a baseline's evaluation sends the hold look); ignored by the sim when `baseline` names a policy ("random"). `arenas` are the stage's arena names, for the per-arena summary.
    `action_names` names each layout's actions in the per-episode log's action counts. `trace_episodes` records every
    decision of the episodes with the first seed indexes, in EvalResult.trace.

    `score_column` is the episode info column the result is scored on (EvalResult.score_column; "" = the return).
    `arena` pins the evaluation to a held-out arena (stage.json's index + 1; eval.heldout).

    `collect_motion` keeps the scored seats' kinematic samples, a track per seat and episode (EvalResult.motion_tracks;
    the sample the episode ended on is the next episode's, so a track stops one decision short of the end).

    `stand_in` puts the "human" stand-in in one seat of every party (the eval arm "with_human": its row is present 2,
    which the chooser plays with a frozen partner, and its episode info's present column is 0, so it is never scored);
    `training_stand_in` is the training MODE's after the evaluation (the learner can field one). `excluded(env)` -> [A]
    bool names rows of env's episode that just ended that are not the learner's to be scored (the eval arms
    "with_partners" and "with_human": the partners' seats); it is asked before the chooser sees the next episode's first
    STEP.

    `any_playing(playing)` is whether any data-parallel learner still plays its share (Ranks.any): the sim answers
    every rank's envs on the same decision and switches mode only once all of them ask, so a rank done with its
    seeds keeps stepping until the last one is, and they switch back together.
    """
    started = time.perf_counter()
    envs, agents = spec.num_envs, spec.agents_per_env
    names = [layout.name for layout in spec.layouts]

    if max_decisions is None:
        per_episode = max(1, spec.episode_seconds * 1000 // max(1, spec.decision_ms))
        # Seeds go out as envs reset, so the last one can start up to one episode after the others.
        max_decisions = per_episode * (-(-episodes // envs) + 2)

    share = {"first_seed": first_seed} if first_seed else {}
    if arena:
        share["arena"] = arena
    if stand_in:
        share["stand_in"] = True
    step = env.set_mode(True, seed, episodes, baseline, **share)
    # Decisions of the envs that might be tracing, kept until their episode ends and its seed is known.
    tracing: dict[int, list[dict]] = {env: [] for env in range(envs)} if trace_episodes else {}
    trace: list[dict] = []
    running = np.zeros((envs, agents), dtype=np.float64)
    taken = np.zeros((envs, agents, spec.num_actions), dtype=np.int32)
    allowed = np.zeros((envs, agents, spec.num_actions), dtype=np.int32)
    env_rows, agent_rows = np.indices((envs, agents))
    finished: dict[int, list[tuple[float, np.ndarray, str, np.ndarray, np.ndarray]]] = {}
    info_names = list(spec.episode_info_names)
    # A party seat left empty for an episode reports present = 0: it is not an episode of any class.
    present = info_names.index("present") if "present" in info_names else None
    decisions = 0
    # Per env, its episode's samples so far ([A, SAMPLE_DIM] a decision), for the realism score.
    collect_motion = collect_motion and getattr(step, "kinematics", None) is not None \
        and np.shape(step.kinematics)[-1] > 0
    moving: list[list[np.ndarray]] = [[np.array(step.kinematics[e])] for e in range(envs)] if collect_motion else []
    motion_tracks: list[np.ndarray] = []
    motion_ids: list[tuple] = []

    def playing() -> bool:
        mine = len(finished) < episodes and decisions < max_decisions
        return any_playing(mine) if any_playing else mine

    while playing():
        chosen = np.zeros((envs, agents), dtype=np.int32) if baseline else choose_actions(step)
        # (actions, goals) from a goal head, (actions, goals or None, look) with the free look (protocol 22).
        actions, goals, look = (tuple(chosen) + (None,) * (3 - len(chosen)) if isinstance(chosen, tuple)
                                else (chosen, None, None))
        # The episode's layouts: after a done, the next STEP already carries the new episode's.
        layout = step.layout
        if not baseline:
            taken[env_rows, agent_rows, np.clip(actions, 0, spec.num_actions - 1)] += 1
            allowed += host(step.mask)
        if tracing:
            for e in tracing:
                for a in range(agents):
                    layout_name = names[int(layout[e, a])] if int(layout[e, a]) < len(names) else ""
                    action = int(actions[e, a])
                    tracing[e].append({
                        "decision": decisions,
                        "agent": a,
                        "layout": layout_name,
                        "action": (action_names or {}).get(layout_name, [])[action]
                        if action < len((action_names or {}).get(layout_name, [])) else str(action),
                        "goal": int(goals[e, a, 0] if goals.ndim == 3 else goals[e, a]) if goals is not None else -1,
                    })

        step = env.step(actions, goals, look) if look is not None else env.step(actions, goals)
        decisions += 1

        running += step.reward
        if collect_motion:
            for e in np.flatnonzero(~np.asarray(step.done, dtype=bool)):
                moving[e].append(np.array(step.kinematics[e]))
        for e in np.flatnonzero(step.done):
            index = int(step.episode_seed[e])
            counted = (index != p.NO_EPISODE_SEED and first_seed <= index < first_seed + episodes
                       and index not in finished)
            if collect_motion:
                if counted:
                    track = np.stack(moving[e], axis=1)  # [A, T, SAMPLE_DIM]
                    for a in range(agents):
                        if (present is None or step.episode_info[e, a, present] > 0.0) and len(track[a]) > 1:
                            motion_tracks.append(track[a])
                            motion_ids.append((index, a, names[int(layout[e, a])], step.episode_info[e, a].copy()))
                moving[e] = [np.array(step.kinematics[e])]
            if counted:
                dropped = excluded(int(e)) if excluded is not None else np.zeros(agents, dtype=bool)
                finished[index] = [
                    (float(running[e, a]), step.episode_info[e, a].copy(), names[int(layout[e, a])], taken[e, a].copy(),
                     allowed[e, a].copy())
                    for a in range(agents)
                    if (present is None or step.episode_info[e, a, present] > 0.0)
                    and not dropped[a]
                ]
            if tracing:
                # The episode's seed is only known now, so its decisions are kept until it ends and then either
                # written out (the first trace_episodes seeds) or dropped.
                if index != p.NO_EPISODE_SEED and index < trace_episodes:
                    for row in tracing[e]:
                        trace.append({"seed": index, **row})
                tracing[e] = []

            running[e] = 0.0
            taken[e] = 0
            allowed[e] = 0

    if len(finished) < episodes:
        print(f"Evaluation stopped after {decisions} decisions with {len(finished)} of {episodes} episodes", flush=True)

    rows = [row for index in sorted(finished) for row in finished[index]]
    seeds = [index for index in sorted(finished) for _ in finished[index]]
    result = EvalResult(
        policy=baseline or "learner",
        returns=np.array([row[0] for row in rows], dtype=np.float64),
        infos=np.array([row[1] for row in rows], dtype=np.float32).reshape(len(rows), spec.episode_info_dim),
        info_names=tuple(spec.episode_info_names),
        layouts=tuple(row[2] for row in rows),
        seeds=tuple(seeds),
        arenas=tuple(arenas),
        seconds=time.perf_counter() - started,
        decisions=decisions,
        trace=trace,
        action_counts=None if baseline else np.array([row[3] for row in rows], dtype=np.int32).reshape(
            len(rows), spec.num_actions),
        allowed_counts=None if baseline else np.array([row[4] for row in rows], dtype=np.int32).reshape(
            len(rows), spec.num_actions),
        action_names=dict(action_names or {}),
        # The builds' names: without them the summary had no class x build breakdown at all ("castings" and "specs"
        # empty), and the layout weights that read it had nothing to weight.
        spec_names=dict(spec_names or {}),
        categories=dict(categories or {}),
        score_column=score_column,
        motion_tracks=motion_tracks,
        motion_ids=motion_ids,
    )

    training_step = env.set_mode(False, stand_in=training_stand_in)
    return result, training_step


@dataclass
class ConvergenceTracker:
    """Best evaluation score so far, and whether the score has stopped improving.

    An evaluation is a new best only if it beats the best by the margin: the larger of min_improvement_abs,
    min_improvement x |best| and z standard errors of the difference between the two scores, so a lucky evaluation
    within the noise does not count. The run has converged once `patience` evaluations in a row set no new best
    and the trend of the last `window` scores, projected `patience` evaluations ahead, would not reach the margin
    either (a slow climb hidden by the noise is still a climb).

    A restart (animus.stage) starts a new segment: the counters and the trend start over, the best is kept.
    """

    patience: int = 0
    window: int = 4
    z: float = 2.0
    min_improvement: float = 0.02
    min_improvement_abs: float = 0.01
    best: float | None = None
    best_stderr: float = 0.0
    best_env_steps: int = 0
    evals_since_best: int = 0
    last_margin: float = 0.0  # the margin the latest evaluation had to beat
    history: list[tuple[int, float, float]] = field(default_factory=list)  # (env steps, score, stderr)

    def margin(self, stderr: float = 0.0) -> float:
        if self.best is None:
            return 0.0
        noise = self.z * math.sqrt(stderr ** 2 + self.best_stderr ** 2)
        return max(self.min_improvement_abs, self.min_improvement * abs(self.best), noise)

    def observe(self, score: float, env_steps: int, stderr: float = 0.0) -> bool:
        """Record an evaluation; True if it is a new best."""
        self.history.append((env_steps, score, stderr))
        self.last_margin = self.margin(stderr)
        if self.best is None or score > self.best + self.last_margin:
            self.best = score
            self.best_stderr = stderr
            self.best_env_steps = env_steps
            self.evals_since_best = 0
            return True
        self.evals_since_best += 1
        return False

    def projected_gain(self) -> float | None:
        """Score gain over the next `patience` evaluations on the recent trend; None below 3 points."""
        points = self.history[-max(3, self.window):]
        if len(points) < 3:
            return None
        steps = np.array([point[0] for point in points], dtype=np.float64)
        scores = np.array([point[1] for point in points], dtype=np.float64)
        if np.ptp(steps) == 0:
            return None
        slope = np.polyfit(steps, scores, 1)[0]
        spacing = np.ptp(steps) / (len(points) - 1)
        return float(slope * spacing * self.patience)

    def converged(self) -> bool:
        if self.patience <= 0 or self.evals_since_best < self.patience:
            return False
        gain = self.projected_gain()
        if gain is None:
            return True
        recent = self.history[-max(3, self.window):]
        stderr = float(np.mean([point[2] for point in recent]))
        return gain <= self.margin(stderr)

    def state_dict(self) -> dict:
        return {
            "best": self.best,
            "best_stderr": self.best_stderr,
            "best_env_steps": self.best_env_steps,
            "evals_since_best": self.evals_since_best,
            "history": list(self.history),
        }

    def forget_scores(self) -> None:
        """Drop every score seen -- the best, its steps and the history -- when the scores to come are of another
        kind (stage.restore_evaluation_state): a best measured on the return cannot be beaten by an outcome score,
        nor a trend read across the two."""
        self.best = None
        self.best_stderr = 0.0
        self.best_env_steps = 0
        self.evals_since_best = 0
        self.last_margin = 0.0
        self.history = []

    def load_state_dict(self, state: dict | None) -> None:
        if not state:
            return
        self.best = state.get("best")
        self.best_stderr = state.get("best_stderr", 0.0)
        self.best_env_steps = state.get("best_env_steps", 0)
        self.evals_since_best = state.get("evals_since_best", 0)
        self.history = [tuple(h) for h in state.get("history", [])]


def format_summary(summary: dict, baseline: dict | None, columns: tuple[str, ...]) -> str:
    """Multi-line table: overall, per level band, layout, arena, talent build and difficulty tier, learner next to
    baseline."""

    def cell(row: dict | None, name: str) -> str:
        value = None if row is None else row.get(name)
        return f"{value:10.2f}" if isinstance(value, (int, float)) else f"{'-':>10}"

    names = ["score", *[c for c in columns if c in summary]]
    rows = [("all", summary, baseline)]
    for group in ("bands", "layouts", "arenas", "builds", "difficulties", "categories"):
        for key, row in summary.get(group, {}).items():
            label = f"tier {key}" if group == "difficulties" else key
            rows.append((label, row, (baseline or {}).get(group, {}).get(key)))

    width = max(7, *(len(key) for key, _, _ in rows))
    # "rows": one per agent of each seeded episode (a party episode is up to four), not episodes.
    lines = [f"  {'group':>{width}} {'rows':>4}  " + "  ".join(f"{n[:21]:>21}" for n in names)]
    for key, row, base in rows:
        cells = "  ".join(f"{cell(row, n)}/{cell(base, n).strip():>10}" for n in names)
        lines.append(f"  {key:>{width}} {row['episodes']:>4}  {cells}")
    return "\n".join(lines)
