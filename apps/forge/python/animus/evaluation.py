"""Seeded evaluation and convergence detection.

An evaluation switches the sim to seeded episodes (protocol MODE): episode seed index i builds the same
characters and opponents every time, so two checkpoints -- or a checkpoint and a scripted baseline -- are
scored on exactly the same situations. Combat rolls (crits, misses, creature choices during the fight) stay
random, so the scores are averages over the seeds, not replays.

The score is the mean episode return over every agent of every seeded episode: the scenario's own reward summed
over each episode, so it measures what training optimises and is comparable between checkpoints of one scenario
(not between scenarios). Summaries also break it down by level band, by layout (the class), by (class, role) and,
for a stage that
mixes arenas, by arena, each with the standard error of its score: combat rolls make two evaluations of the same
networks differ, and the convergence test only counts an improvement that stands out from that noise.

Self-play arenas can be scored against a scripted opponent: with `opponents` the sim plays the other side of each
self-play episode with that policy (protocol MODE_FLAG_SCRIPTED_OPPONENTS), and the opponent seats' rows (episode
info opponent_seat) are left out of the result -- of the learner's evaluation and of the baseline's, which then is the
baseline against itself.
"""

from __future__ import annotations

import math
import time
from dataclasses import dataclass, field

import numpy as np

from . import protocol as p

LEVEL_BANDS = ((1, 20), (21, 40), (41, 60), (61, 80))

# How a character's talents were spent, by the episode info column "talent_plan" (SeatCharacter::TalentPlan).
# Scored as its own group so a run shows whether the policy plays a build it was not handed the recipe for.
TALENT_PLANS = ("standard", "noisy", "random")
# There is no role. A seat is summarised by the build it drew (episode info "spec", named per class by
# stage.json's spec_names), which is finer where it matters: a feral cat and a balance druid were one role and are
# not equally hard to win with.

# An episode that cancelled at least this many of its own casts did not merely waste a few: with a decision every
# 100 ms it spent the episode in a start-cast / stop-cast loop. Deterministic actions cannot break out of one --
# the state that chose to stop recurs unchanged -- so a policy can carry it into evaluation and into the exported
# model while its sampled training rollouts look healthy. stage8_duel: a quarter of warlock episodes, up to 299
# cancels in a 60 s episode, scoring 3.30 where the rest scored 7.52.
LIVELOCK_CANCELS = 20

# Summary fields derived from the episode info rather than averaged straight from them. Gateable like any metric
# (target.metrics, target.layout_metrics); they are not episode info names, so validation allows them by name.
# clean_kill: the fight was won outright -- the opponent killed and the seat never dead. killed and died are gated
# apart, and their means cannot say whether the episodes that killed are the ones that did not die.
# lost / wedged / spl, on a travel stage: how a trip failed. A seat that did not arrive and covered more than
# LOST_ABOVE times the path it was given wandered (38 of 42 stage1_move failures and 57 of 61 stage6_travel's); one
# that covered less than WEDGED_BELOW of it never got going. They want opposite fixes and look identical in `arrived`.
# spl is success weighted by path length -- arrived x path / max(path, covered), the navigation literature's SPL --
# 1 for a seat that walked exactly the path and 0 for one that did not arrive.
DERIVED_METRICS = ("livelocked", "clean_kill", "lost", "wedged", "spl")
LOST_ABOVE = 3.0
WEDGED_BELOW = 0.5


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
    # for a scripted baseline, whose actions the sim picks), and per layout the actions' names (stage.json
    # "action_names"), for the per-episode log.
    action_counts: np.ndarray | None = None
    # [n, num_actions] how many of the same decisions allowed each action (the mask), so an action the policy never
    # takes can be told from one it was never offered (a missing reagent, a spell the character doesn't have).
    allowed_counts: np.ndarray | None = None
    action_names: dict[str, list[str]] = field(default_factory=dict)
    # Per layout, its class's build names in the order the "spec" episode info column indexes them.
    spec_names: dict[str, list[str]] = field(default_factory=dict)
    # Decision by decision, for the first `eval.trace_episodes` seeded episodes: what the policy did and what it said
    # it was doing. A summary cannot show a plan -- the order of the decisions is the plan -- so this is what to read
    # when asking whether a bot saved a cooldown, rested before a pull or held an add.
    trace: list[dict] = field(default_factory=list)

    @property
    def episodes(self) -> int:
        return len(self.returns)

    @property
    def score(self) -> float:
        return float(self.returns.mean()) if len(self.returns) else float("nan")

    @property
    def stderr(self) -> float:
        return standard_error(self.returns, self._episode_of_row())

    def _episode_of_row(self) -> np.ndarray | None:
        """The episode each row belongs to, for standard_error; None when the rows were not labelled."""
        return np.asarray(self.seeds) if len(self.seeds) == len(self.returns) else None

    def episodes_log(self, columns: tuple[str, ...] | None = None) -> list[dict]:
        """One row per scored episode: its seed, layout, return, `columns` of its episode info (None: every column)
        and the derived fields.

        The summaries average these away, and an average cannot say whether a class is a little worse
        everywhere or fine except for a handful of episodes it never finishes -- which is what a per-layout gate
        actually turns on. Why those episodes failed is in the columns no summary reports (the character's level and
        spec, the opponent, the form and distance it ended in), so the training run logs them all to
        eval_episodes.jsonl.
        """
        names = self.info_names if columns is None else tuple(c for c in columns if c in self.info_names)
        indices = [self.info_names.index(name) for name in names]
        derived = self.derived()
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
            for name, values in derived.items():
                row[name] = float(values[index])
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

    def failed_seeds(self, metric: str) -> list[int]:
        """Seed indexes of the episodes where some scored row fell short on `metric` (a 0/1 field per episode: a
        derived one such as clean_kill, or an episode info column), for replaying them in training."""
        values = self.derived().get(metric)
        if values is None:
            values = self.column(metric)
        if values is None or not len(self.seeds):
            return []
        return sorted({int(seed) for seed, value in zip(self.seeds, values) if value < 1.0})

    def derived(self) -> dict[str, np.ndarray]:
        """Per episode, the DERIVED_METRICS the episode info can give: 1.0 where it holds, else 0.0 (spl, a
        weighted success, is a fraction)."""
        out = {}
        # The share of episodes stuck in a cast/stop loop. A mean of casts_cancelled hides it: the loop is a tail,
        # not a shift (stage8_duel warlock: median 4 cancels, maximum 299), so it is counted per episode.
        cancels = self.column("casts_cancelled")
        if cancels is not None:
            out["livelocked"] = (cancels >= LIVELOCK_CANCELS).astype(np.float64)
        killed, died = self.column("killed"), self.column("died")
        if killed is not None and died is not None:
            out["clean_kill"] = ((killed > 0.0) & (died <= 0.0)).astype(np.float64)
        arrived, covered, path = (self.column("arrived"), self.column("distance_travelled"),
                                  self.column("walk_distance"))
        if arrived is not None and covered is not None and path is not None:
            length = np.maximum(path.astype(np.float64), 1e-6)
            failed = arrived <= 0.0
            out["lost"] = (failed & (covered > LOST_ABOVE * length)).astype(np.float64)
            out["wedged"] = (failed & (covered < WEDGED_BELOW * length)).astype(np.float64)
            out["spl"] = np.where(arrived > 0.0, length / np.maximum(length, covered), 0.0).astype(np.float64)
        return out

    def summary(self, columns: tuple[str, ...]) -> dict:
        """Score and means of `columns`: overall, per level band, per layout, per arena, per talent build and per
        difficulty tier, and for each tier but the top one everything up to it, per layout too ("up_to")."""
        present = [c for c in columns if c in self.info_names]
        derived = self.derived()
        episodes = self._episode_of_row()

        def means(rows: np.ndarray) -> dict:
            out = {
                "episodes": int(rows.sum()),
                "score": float(self.returns[rows].mean()) if rows.any() else None,
                "stderr": standard_error(self.returns[rows],
                                         episodes[rows] if episodes is not None else None),
            }
            for name in present:
                values = self.column(name)[rows]
                out[name] = float(values.mean()) if len(values) else None
            for name, values in derived.items():
                picked = values[rows]
                out[name] = float(picked.mean()) if len(picked) else None
            return out

        everything = np.ones(self.episodes, dtype=bool)
        result = {"policy": self.policy, **means(everything), "bands": {}, "layouts": {}, "specs": {},
                  "castings": {}, "arenas": {}, "builds": {}, "difficulties": {}, "up_to": {}}
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

    Resolved per layout, because the same action sits at a different index in every class's catalog -- follow_route
    was index 90 for the warrior alone. A name no layout has is a mistake in the config, and raises rather than
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


def casting_weights(summary: dict, baseline: dict | None, strength: float, max_ratio: float,
                    metric: str = "") -> dict[str, float]:
    """How often training episodes should draw each (class, role), from the gap to the baseline's score for it
    and, with `metric`, from how far it falls short on that summary field (higher is better).

    A stage is gated on its weakest class and role, so an episode of a pair that trails its baseline is worth more
    than one of a pair that is already clear of it. Per pair and not per model: one model is a whole class now, and
    weighting a paladin that heals badly by its average would send it more tanking episodes it did not need. The
    baseline gap alone misses a pair that beats a weak baseline yet fails an absolute gate -- stage8_duel's mage
    beat the scripted mage while killing 68% of the time -- so the shortfall on the gated metric counts as well,
    whichever of the two is larger. Each is measured in its own standard deviations, so the weights do not depend
    on the size of the scenario's rewards, and the spread is capped: the heaviest pair draws at most `max_ratio`
    times the lightest, whatever the scores are. Weights average 1 (the even draw).
    """
    rows = summary.get("castings", {})
    names = [name for name, row in rows.items() if row.get("score") is not None]
    if not names or strength <= 0.0 or max_ratio <= 1.0:
        return {name: 1.0 for name in rows}

    def standardised(values: np.ndarray) -> np.ndarray:
        spread = float(values.std())
        return (values - values.mean()) / spread if spread > 1e-9 else np.zeros_like(values)

    base = (baseline or {}).get("castings", {})
    gaps = np.array([float(base.get(name, {}).get("score") or 0.0) - float(rows[name]["score"]) for name in names])
    need = standardised(gaps)
    if metric and all(rows[name].get(metric) is not None for name in names):
        # The larger of the two needs, not their sum: a wide lead over a weak baseline must not cancel a gate
        # the layout is failing.
        need = np.maximum(need, standardised(-np.array([float(rows[name][metric]) for name in names])))
    if not need.any():
        return {name: 1.0 for name in names}

    weights = np.exp(strength * np.clip(need, -3.0, 3.0))
    # Cap the spread, then centre on 1 so the total number of episodes is unchanged.
    limit = math.sqrt(max_ratio)
    weights = np.clip(weights / float(np.exp(np.log(weights).mean())), 1.0 / limit, limit)
    weights /= float(weights.mean())
    return {name: float(weight) for name, weight in zip(names, weights)}


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


def run_evaluation(env, spec, choose_actions, episodes: int, seed: int, baseline: str = "",
                   max_decisions: int | None = None, opponents: str = "",
                   arenas: tuple[str, ...] = (),
                   action_names: dict[str, list[str]] | None = None,
                   trace_episodes: int = 0) -> tuple[EvalResult, p.Step]:
    """Run seeded episodes 0..episodes-1 and return their results and the fresh training STEP after them.

    choose_actions(step) -> [E, A] actions, or (actions, goals) from a policy with a goal head (the goals go to the
    sim, which scores and reports them); ignored by the sim when `baseline` names a scripted policy. `opponents`
    names a scripted policy for the opponent seats of self-play episodes (the learner plays the rest, or `baseline`
    everything); their rows are left out. `arenas` are the stage's arena names, for the per-arena summary.
    `action_names` names each layout's actions in the per-episode log's action counts. `trace_episodes` records every
    decision of the episodes with the first seed indexes, in EvalResult.trace.
    """
    started = time.perf_counter()
    envs, agents = spec.num_envs, spec.agents_per_env
    names = [layout.name for layout in spec.layouts]

    if max_decisions is None:
        per_episode = max(1, spec.episode_seconds * 1000 // max(1, spec.decision_ms))
        # Seeds go out as envs reset, so the last one can start up to one episode after the others.
        max_decisions = per_episode * (-(-episodes // envs) + 2)

    if baseline:
        step = env.set_mode(True, seed, episodes, baseline)
    else:
        step = env.set_mode(True, seed, episodes, opponents, opponents_only=bool(opponents))
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
    # Against a scripted opponent its seats are not the learner's (nor, for the baseline, the seat being scored).
    opponent_seat = info_names.index("opponent_seat") if opponents and "opponent_seat" in info_names else None
    decisions = 0

    while len(finished) < episodes and decisions < max_decisions:
        chosen = np.zeros((envs, agents), dtype=np.int32) if baseline else choose_actions(step)
        actions, goals = chosen if isinstance(chosen, tuple) else (chosen, None)
        # The episode's layouts: after a done, the next STEP already carries the new episode's.
        layout = step.layout
        if not baseline:
            taken[env_rows, agent_rows, np.clip(actions, 0, spec.num_actions - 1)] += 1
            allowed += step.mask
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
                        "goal": int(goals[e, a]) if goals is not None else -1,
                    })

        step = env.step(actions, goals)
        decisions += 1

        running += step.reward
        for e in np.flatnonzero(step.done):
            index = int(step.episode_seed[e])
            if index != p.NO_EPISODE_SEED and index < episodes and index not in finished:
                finished[index] = [
                    (float(running[e, a]), step.episode_info[e, a].copy(), names[int(layout[e, a])], taken[e, a].copy(),
                     allowed[e, a].copy())
                    for a in range(agents)
                    if (present is None or step.episode_info[e, a, present] > 0.0)
                    and (opponent_seat is None or step.episode_info[e, a, opponent_seat] <= 0.0)
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
    )

    training_step = env.set_mode(False)
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
    segment_index: int = 0  # history index where the current segment starts
    segment_env_steps: int = 0
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
        """Score gain over the next `patience` evaluations on the current segment's trend; None below 3 points."""
        points = self.history[self.segment_index:][-max(3, self.window):]
        if len(points) < 3:
            return None
        steps = np.array([point[0] for point in points], dtype=np.float64)
        scores = np.array([point[1] for point in points], dtype=np.float64)
        if np.ptp(steps) == 0:
            return None
        slope = np.polyfit(steps, scores, 1)[0]
        spacing = np.ptp(steps) / (len(points) - 1)
        return float(slope * spacing * self.patience)

    def converged(self, env_steps: int, min_env_steps: int = 0) -> bool:
        if self.patience <= 0 or self.evals_since_best < self.patience:
            return False
        if env_steps - self.segment_env_steps < min_env_steps:
            return False
        gain = self.projected_gain()
        if gain is None:
            return True
        recent = self.history[self.segment_index:][-max(3, self.window):]
        stderr = float(np.mean([point[2] for point in recent]))
        return gain <= self.margin(stderr)

    def promote(self, score: float, env_steps: int, stderr: float = 0.0) -> None:
        """Make the latest evaluation (already observed) the best whatever its score: one that passes the stage
        target outranks a higher score that does not."""
        self.best = score
        self.best_stderr = stderr
        self.best_env_steps = env_steps
        self.evals_since_best = 0

    def reset_segment(self, env_steps: int) -> None:
        """Start a new segment (after a restart): counters and trend start over, the best score is kept."""
        self.segment_index = len(self.history)
        self.segment_env_steps = env_steps
        self.evals_since_best = 0

    def state_dict(self) -> dict:
        return {
            "best": self.best,
            "best_stderr": self.best_stderr,
            "best_env_steps": self.best_env_steps,
            "evals_since_best": self.evals_since_best,
            "segment_index": self.segment_index,
            "segment_env_steps": self.segment_env_steps,
            "history": list(self.history),
        }

    def load_state_dict(self, state: dict | None) -> None:
        if not state:
            return
        self.best = state.get("best")
        self.best_stderr = state.get("best_stderr", 0.0)
        self.best_env_steps = state.get("best_env_steps", 0)
        self.evals_since_best = state.get("evals_since_best", 0)
        self.segment_index = state.get("segment_index", 0)
        self.segment_env_steps = state.get("segment_env_steps", 0)
        self.history = [tuple(h) for h in state.get("history", [])]


def format_summary(summary: dict, baseline: dict | None, columns: tuple[str, ...]) -> str:
    """Multi-line table: overall, per level band, layout, arena, talent build and difficulty tier, learner next to
    baseline."""

    def cell(row: dict | None, name: str) -> str:
        value = None if row is None else row.get(name)
        return f"{value:10.2f}" if isinstance(value, (int, float)) else f"{'-':>10}"

    names = ["score", *[c for c in columns if c in summary], *[d for d in DERIVED_METRICS if d in summary]]
    rows = [("all", summary, baseline)]
    for group in ("bands", "layouts", "arenas", "builds", "difficulties"):
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
