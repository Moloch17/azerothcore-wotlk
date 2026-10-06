"""Training run configuration, loaded from YAML (see configs/).

A config may start with ``extends: <other>.yaml`` (relative to its own file): it is merged over that config, section by
section, so a curriculum stage lists only what differs from the base. Overlays (``--overlay``, e.g. configs/fast.yaml
for ``forge fast``) are merged over the whole result the same way, so one file changes every stage.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field, fields, is_dataclass
from pathlib import Path
from types import UnionType
from typing import Union, get_args, get_origin, get_type_hints

import yaml

from .human.motion import WINDOW as MOTION_WINDOW
from .mappo.trainer import MappoConfig
from .stages import merges, seed_chain

EXTENDS_KEY = "extends"
AUTO = "auto"

REPORT_COLUMNS = (
    "dps", "killed", "died", "timed_out", "deaths", "time_to_kill", "damage_taken", "kills", "pulls_cleared", "wipes",
    "owner_deaths", "owner_healing", "casts_completed", "casts_cancelled", "cancelled_stopped", "cancelled_moved",
    "cancelled_target", "cancelled_other", "cast_seconds_wasted", "consumables_used", "self_resurrections", "revives",
    # Beside an owner: whether and how often it died, what it took, and the role checks -- the share of the owner's
    # damage taken the seat healed (healers) and the share of the enemies' attention on the seat rather than the owner
    # (high for tanks, low for damage dealers and healers).
    "owner_died", "owner_damage_taken", "owner_heal_share", "threat_share",
    # Pets: logged per episode in eval_episodes.jsonl with the episode's class/build, so a pet class's use of its pet
    # can be read on its own.
    "pet_summoned", "pet_at_start", "pet_damage_share", "pet_died", "pet_abilities", "pet_orders",
    # Fights no play could win (a creature with no path to the seat), and one action pressed over and over.
    "target_unreachable_seconds", "target_teleports", "repeated_presses",
    # Style: where the seat's own damage came from (with pet_damage_share they add up to 1), how much of the fight it
    # spent within melee reach, and how much its pet held the opponent. Read by spec: a hunter's shots cannot be used
    # in melee reach, so in_melee_share is the share of the fight it played melee.
    "melee_damage_share", "shot_damage_share", "spell_damage_share", "in_melee_share", "target_on_pet_share",
    # Whether it roots or slows its opponent, and what it has its pet do.
    "target_rooted_share", "target_snared_share", "roots_applied", "snares_applied",
    "pet_attack_orders", "pet_passive_orders", "pet_follow_orders", "pet_stay_orders", "pet_attacking_share",
    "pet_passive_share", "pet_staying_share", "feign_deaths", "feign_death_resets", "item_uses",
    # Gauntlet recovery: health and mana each pull was engaged with, pulls started low or that came to the seat
    # unengaged, time resting, and eating or drinking that failed or ended with something left to restore.
    "engage_health", "engage_mana", "pulls_started_low", "pulls_arrived", "rest_seconds", "food_used", "drink_used",
    "eat_failed", "drink_failed", "meals_cut_short", "buff_coverage",
    # Support: healing and protection done (fractions of the bot's health), overhealing, casts wasted on a friend at
    # full health (masked: 0), defensives, heals cast below their highest rank, and time any friend spent low.
    "healing_done", "protection_done", "overheal_share", "heals_on_full", "defensive_casts", "healing_casts",
    "downranked_share", "low_health_seconds",
    # Crowd control that kept pack members other than the target out of the fight (enemy-seconds).
    "control_seconds",
)


@dataclass
class EvalConfig:
    """Seeded evaluation (see animus.evaluation): the same characters and opponents every time."""

    every_env_steps: int = 0  # evaluate after this many training env steps; 0 = never
    at_start: bool = True  # also evaluate the starting (seeded or fresh) networks before training
    # Seeded episodes per evaluation. The score's standard error falls with the square root of this, and that error
    # sets the convergence margin (a new best must clear it) and the per-layout gates' noise allowance, so too few
    # episodes hide real progress behind noise. An evaluation costs seconds against tens of minutes of training
    # between them: prefer more. Spread over the stage's layouts, each needs its own share (target.min_layout_episodes).
    episodes: int = 1024
    seed: int = 1000  # seed base: which characters and opponents
    deterministic: bool = True  # argmax actions instead of sampling
    baseline: str = ""  # sim scripted policy scored once per run on the same seeds ("greedy", "fight")
    # Self-play arenas: the baseline also plays the other side of every self-play episode when the learner is scored
    # (and against itself when the baseline is), so the score is the learner against a fixed opponent.
    opponent_baseline: bool = False
    report: tuple[str, ...] = REPORT_COLUMNS  # episode info columns printed per level band, when present
    # Decision by decision, for the episodes with the first this many seed indexes: what the policy did and what goal
    # it said it was pursuing, written to eval_trace.jsonl (one object per decision per agent). A summary averages a
    # plan away -- the order of the decisions is the plan -- so this is what to read to see whether a bot rested
    # before a pull, saved a cooldown or held an add. 0 = off; a handful is plenty (an episode is hundreds of rows).
    trace_episodes: int = 0
    # Every this many evaluations, also score sampled actions on the same seeds (policy learner_sampled in eval.csv,
    # eval.jsonl and eval_episodes.jsonl). Training samples; evaluation and exported models take the argmax. A wide
    # gap between the two says the policy that is gated is not the one that trained (see mappo.entropy_final_fraction).
    # 0 = never.
    sampled_every: int = 0
    # Actions the evaluation may not take, by name, resolved per layout (the same name is a different index in
    # every class): an evaluation-only mask, for measuring what an action carried -- re-scoring a checkpoint with
    # follow_route and face_objective forbidden says how much of its arrival rate was the pathfinder's. A name no
    # layout has is refused at startup. Training and the scripted baseline are untouched.
    mask_actions: tuple[str, ...] = ()
    # Curriculum phases by arena (a phase's name -> the stage's arenas in it): the summary adds a row per phase. The
    # ship stage sets it, since its gate is each phase within noise of its own stage. An arena the stage does not have
    # is refused at startup.
    phases: dict = field(default_factory=dict)
    # Held-out arenas (their name -> seeded episodes), played after every evaluation and reported apart as
    # `heldout_<arena>`: content the stage never trains on (stage.json "eval_only", ArenaDefinition::EvalOnly), so a
    # policy that memorised its own route is told from one that learned. A reading, never a target: nothing about it
    # moves best.pt, the league or convergence (peak-play W2). {} = none.
    heldout: dict = field(default_factory=dict)
    # How often the held-out arenas are played: every this many evaluations, and always on the stage's last one and
    # whenever a new best.pt is saved (so best.pt always has a held-out reading beside it). A second dungeon
    # evaluation of hours of sim time is a reading that never steers training; 1 = every evaluation.
    heldout_every: int = 4
    # What an evaluation is scored on, and so what best.pt, the league's snapshots and convergence follow: "outcome",
    # the episode's Outcome and Cost terms before any rung's tier (the sim's score_outcome column), or "return", the
    # whole return with its shaping. Scored on the return, a stage whose shaping is turned down reads as getting
    # worse, and a ladder that steps reads as learning or collapse (peak-play plan W0).
    score: str = "outcome"

    def __post_init__(self) -> None:
        if self.heldout_every < 1:
            raise ValueError(f"eval.heldout_every: expected at least 1 (1 = every evaluation), got "
                             f"{self.heldout_every!r}")

    def score_column(self) -> str:
        if self.score not in ("outcome", "return"):
            raise ValueError(f"eval.score: expected \"outcome\" or \"return\", got {self.score!r}")
        return "score_outcome" if self.score == "outcome" else ""


@dataclass
class ConvergenceConfig:
    """When the stage is done (animus.stage.ConvergenceController): the one rule every stage ends on. Needs
    eval.every_env_steps; without evaluations a stage trains to total_env_steps.

    A class has converged when, over the last `window` evaluations, its score has plateaued (the margin below), its
    LR-normalised approx_kl per update has stayed under `kl`, its entropy over ln(allowed actions) has a slope within
    `entropy_slope` (and sits above entropy_floor.fraction when one is set), and its ladder rung or league win rate
    has settled. The stage advances when every class the run plays has converged, or at total_env_steps.
    """

    # False: the stage never ends on convergence (it trains until stopped or its total_env_steps); classes that
    # converge still leave the draw by hold_share. stage9_deadmines, whose user watches it and stops it by hand.
    advance: bool = True
    patience: int = 0  # evaluations without a new *overall* best before the score counts as plateaued (LR anneal)
    window: int = 4  # evaluations every class signal is read over
    z: float = 2.0  # a new best beats the best by this many standard errors ...
    min_improvement: float = 0.02  # ... or by this fraction of |best| ...
    min_improvement_abs: float = 0.01  # ... or by this much, whichever is largest
    # approx_kl per update, divided by the learning-rate scale in force, under which a class's policy has stopped
    # moving. Measured: stages that never stall sit at 0.003-0.010 at full rate, stalled ones near 0.001.
    kl: float = 0.003
    entropy_slope: float = 0.01  # per evaluation, of entropy / ln(allowed actions)
    # A converged class keeps this share of its training draw (its adapter and head are frozen), so the rest of
    # the budget goes to the classes still learning; it re-enters at full weight if its score regresses.
    hold_share: float = 0.02
    # Hold both learning rates at full until the overall score first plateaus, then anneal: an anneal that starts
    # at step 0 makes the KL fall with the schedule, which read as convergence when it was not.
    lr_hold_until_plateau: bool = True
    # A ladder stage converges only at the top of its ladder (user, 2026-10-05; movement-curriculum M1): a class's
    # convergence reads its evaluation episodes at the top rung only (episode info `at_top_rung`, else the highest
    # `difficulty` tier), and it cannot converge until its own training episodes have been at the top rung for the
    # whole window (a share of TOP_RUNG_SHARE). A stage with no ladder is unaffected.
    top_rung: bool = True
    # The stage's own measure, if not the evaluation score: an episode info column the per-class plateau reads (M1:
    # "arrived"), at the top rung on a ladder stage. Empty: the score.
    measure: str = ""
    # The most of a ladder stage's narrow legs that may fall back to ordinary markers at the top rung (evaluation
    # column fallback_share) while a class converges: above it the class is missing "fallbacks" -- easier legs would
    # otherwise let it converge without the narrow skill (M3, M4). A convergence signal, not a gate. < 0: the sim's
    # Markers.FallbackCeiling from stage.json's tuning (the default); 0 turns the signal off.
    fallback_ceiling: float = -1.0


@dataclass
class FadeConfig:
    """The shaping ladder (animus.stage.ShapingFade, peak-play plan W1): every Shaping reward term is paid times the
    current rung's scale, Outcome and Cost terms never. The ladder steps down when the outcome score has plateaued at
    its rung (and a ladder stage's difficulty ladder has settled), back up one when the score falls more than
    `regress_z` standard errors below what it was at the step, and is settled -- a convergence signal, not a gate --
    at its last rung or once a rung has been fallen back to `give_up` times.

    Earned rather than scheduled: a stage whose budget is a ceiling (stage9's 1e12) never reaches a fraction of it, and
    shaping pulled before the outcome is learned only stops the learning."""

    enabled: bool = False
    rungs: tuple[float, ...] = (1.0, 0.5, 0.25, 0.0)
    window: int = 3  # evaluations at a rung before it may step, and the plateau test's patience
    regress_z: float = 2.0
    give_up: int = 2  # falls back to the same rung before the ladder stays there
    # Classes whose difficulty ladder may still be moving when the shaping ladder steps: ten classes' ladders are rarely
    # all still at once, and waiting for every one held the fade at x1 for the whole of stage4 (2026-10-03).
    moving_classes: int = 2
    # No step down until the evaluation's `gate_metric` (a column of its summary) is at least `gate_value`, on top of
    # the plateau: shaping pulled before the stage's own measure is learned stops the learning (stage3_rotation,
    # 2026-10-05: faded to 0 at output ~5 a dummy, then output fell to 1.1). "" = the plateau alone.
    gate_metric: str = ""
    gate_value: float = 0.0

    def __post_init__(self) -> None:
        rungs = tuple(float(scale) for scale in self.rungs)
        if not rungs or any(not 0.0 <= scale <= 1.0 for scale in rungs) or rungs[-1] != 0.0 \
                or any(later >= earlier for earlier, later in zip(rungs, rungs[1:])):
            raise ValueError(f"fade.rungs: expected scales strictly falling within [0, 1] to 0, got {self.rungs!r}")
        if self.window < 1:
            raise ValueError(f"fade.window: expected at least 1 evaluation, got {self.window!r}")
        if not self.regress_z > 0.0:
            raise ValueError(f"fade.regress_z: expected more than 0 standard errors, got {self.regress_z!r}")
        if self.give_up < 1:
            raise ValueError(f"fade.give_up: expected at least 1 fall, got {self.give_up!r}")
        if self.moving_classes < 0:
            raise ValueError(f"fade.moving_classes: expected 0 or more, got {self.moving_classes!r}")
        self.rungs = rungs


@dataclass
class CostLadderConfig:
    """The cost ladder (animus.stage.CostLadder): every noise price -- Repeat, Jitter, Aimless, Effort, Fidget
    (RewardLedger PricesNoise) -- is paid times the current rung's scale; deaths, the clock and the step cost always
    in full, and the score always at full price. The ladder moves on a rung when the outcome score has plateaued at
    its rung -- off the first, once the evaluation's `gate_metric` has reached `gate_value` instead -- back one when the
    score falls more than `regress_z` standard errors below what it was at the step, and is settled at its last rung
    or once a rung has been fallen back to `give_up` times.

    The rungs are any path through [0, 1]. stage1_move's: free (x0) until arriving is learned, faded in to full
    price, then faded back out to see whether the habits stick -- the score is at full price throughout, so habits
    that do not stick read as a fall, and the ladder moves back to the price that held them. Its first run at full
    price (2026-10-04) paid about -9 an episode in noise for a 6% chance at arriving and stood still from 5M steps
    on; a second, starting at x0.25 on a plateau rule, drifted the same way. Nothing converges, and the learning rate
    does not anneal, until the ladder has played its last rung for `window` evaluations."""

    enabled: bool = False
    rungs: tuple[float, ...] = (0.0, 0.25, 0.5, 1.0, 0.5, 0.25, 0.0)
    window: int = 3  # evaluations at a rung before it may step, and the plateau test's patience
    regress_z: float = 2.0
    give_up: int = 2  # falls back to the same rung before the ladder stays there
    # Off the first rung only once the evaluation's metric (a column of its summary, e.g. arrived) reaches the value:
    # a plateau is no signal there, since a policy stalled short of the outcome has plateaued too. "" = the plateau.
    gate_metric: str = ""
    gate_value: float = 0.0

    def __post_init__(self) -> None:
        rungs = tuple(float(scale) for scale in self.rungs)
        if not rungs or any(not 0.0 <= scale <= 1.0 for scale in rungs) \
                or any(later == earlier for earlier, later in zip(rungs, rungs[1:])):
            raise ValueError(f"costs.rungs: expected scales within [0, 1], each unlike the one before, got "
                             f"{self.rungs!r}")
        if self.window < 1:
            raise ValueError(f"costs.window: expected at least 1 evaluation, got {self.window!r}")
        if not self.regress_z > 0.0:
            raise ValueError(f"costs.regress_z: expected more than 0 standard errors, got {self.regress_z!r}")
        if self.give_up < 1:
            raise ValueError(f"costs.give_up: expected at least 1 fall, got {self.give_up!r}")
        self.rungs = rungs


@dataclass
class StyleConfig:
    """The movement-style reward (animus.style, human-play-data plan 2.4): an adversarial motion prior. A small
    discriminator learns to tell `window`-step windows of human motion (`dataset`, the human_motion_windows.npz the
    offline animus.human pipeline writes) from the seats' own, per context (on foot, swimming, flying, airborne;
    mounted; in combat), and every decision whose window it cannot tell from a human's is paid up to `coef`:
    r = max(0, 1 - 0.25 (D - 1)^2). A kept, Cost-like term, not faded shaping: with `ladder` it is paid times the cost
    ladder's scale (costs), so it arrives with the noise prices once the outcome is learned; without one, or with the
    cost ladder off, in full. It never enters the outcome score, which the sim computes.

    `reference` (human_reference.json, the same pipeline's) adds the realism columns to every evaluation: per context
    the earth mover's distance between the seats' motion histograms and the players' (animus.human.realism). It
    works with the reward off. Paths take {runs_dir}, {run_name} and {shared_runs}."""

    enabled: bool = False
    dataset: str = ""
    reference: str = ""
    coef: float = 0.02          # per decision at D = 1: next to the noise prices, not the outcome
    lr: float = 1e-4
    hidden: tuple[int, ...] = (256, 256)
    grad_penalty: float = 10.0  # times half the mean squared gradient norm on the human windows (AMP's w_gp)
    window: int = MOTION_WINDOW  # steps a window holds (motion.WINDOW); must be the dataset's
    minibatches: int = 4        # discriminator steps an update
    batch: int = 512            # bot windows a step (and as many human windows, matched per context)
    ladder: bool = True
    eval_windows: int = 200000  # most windows eval_motion.npz keeps (a uniform draw, its weight the inverse share)

    def __post_init__(self) -> None:
        if self.enabled and not self.dataset:
            raise ValueError("style.dataset: the style reward needs the human windows (human_motion_windows.npz)")
        if not self.coef >= 0.0:
            raise ValueError(f"style.coef: expected 0 or more, got {self.coef!r}")
        if not self.lr > 0.0:
            raise ValueError(f"style.lr: expected more than 0, got {self.lr!r}")
        hidden = tuple(int(width) for width in self.hidden)
        if not hidden or any(width < 1 for width in hidden):
            raise ValueError(f"style.hidden: expected one or more layer widths above 0, got {self.hidden!r}")
        self.hidden = hidden
        if not self.grad_penalty >= 0.0:
            raise ValueError(f"style.grad_penalty: expected 0 or more, got {self.grad_penalty!r}")
        if self.window < 2:
            raise ValueError(f"style.window: expected at least 2 steps, got {self.window!r}")
        if self.minibatches < 1 or self.batch < 1:
            raise ValueError(f"style.minibatches and style.batch: expected at least 1, got {self.minibatches!r} and "
                             f"{self.batch!r}")
        if self.eval_windows < 0:
            raise ValueError(f"style.eval_windows: expected 0 or more, got {self.eval_windows!r}")


@dataclass
class ExploreConfig:
    """Go-Explore starts for the dungeon wings (animus.explore, peak-play plan W5): the cells ended training runs
    reached are archived, and `share` of a wing's training resets start from one of the `table_size` most promising
    instead of the door. Evaluation always starts at the door. Off, nothing is sent and every run starts at the door."""

    enabled: bool = False
    share: float = 0.5
    table_size: int = 64
    max_cells: int = 4096
    depth_weight: float = 1.0

    def __post_init__(self) -> None:
        if not 0.0 <= self.share <= 1.0:
            raise ValueError(f"explore.share: expected a share within [0, 1], got {self.share!r}")
        if not 1 <= self.table_size <= 64:
            raise ValueError(f"explore.table_size: expected 1 to 64 cells (the sim's table), got {self.table_size!r}")
        if self.max_cells < self.table_size:
            raise ValueError(f"explore.max_cells: expected at least table_size, got {self.max_cells!r}")


@dataclass
class ExploitConfig:
    """Exploiters for the PvP stages (animus.exploit, peak-play plan W6): a second learner in the same run, seeded
    from a league snapshot, playing the far side of `share` of the cast episodes against the main. Once it wins
    `join_at` of them over its last `window` (at least `min_episodes`) it joins the league, tagged, drawn at
    cast.exploiter_floor at least; a budget of `budget_env_steps` spent without that retires it. The next starts
    `every_env_steps` after. Never in evaluation, best.pt or the main's convergence. Needs cast.opponents: league."""

    enabled: bool = False
    share: float = 0.25
    budget_env_steps: int = 50_000_000
    join_at: float = 0.6
    min_episodes: int = 200
    window: int = 200
    every_env_steps: int = 0
    log_every: int = 10

    def __post_init__(self) -> None:
        if not 0.0 < self.share <= 1.0:
            raise ValueError(f"exploit.share: expected a share within (0, 1], got {self.share!r}")
        if not 0.5 <= self.join_at <= 1.0:
            raise ValueError(f"exploit.join_at: expected a win rate within [0.5, 1], got {self.join_at!r}")
        if self.min_episodes < 1 or self.window < 1 or self.budget_env_steps < 1:
            raise ValueError("exploit.min_episodes, window and budget_env_steps: expected at least 1")


@dataclass
class EntropyFloorConfig:
    """Keep exploration from collapsing, measured against how many actions were actually legal.

    A masked action space makes a flat entropy coefficient hard to reason about: the ceiling is ln(legal
    actions), which swings with level, cooldowns and the global cooldown, and is nothing like ln(the padded
    action count). This raises mappo.entropy_coef when the policy's entropy falls below `fraction` of that
    ceiling and lets it fall back to the configured value once it is above -- a floor, never a ceiling, so it
    cannot hold a converging policy stochastic.
    """

    fraction: float = 0.0  # of ln(allowed actions); 0 = off, the coefficient stays where it is configured
    max_boost: float = 4.0  # never raise the coefficient past this many times the configured one
    rate: float = 0.05  # how fast it moves per update, as a fraction of the distance


@dataclass
class DistillConfig:
    """Kickstarting a merge stage (animus.distill): on the decisions of each arena that has a teacher -- the parent
    stage whose model already plays it -- the policy loss gains coef x KL(teacher || policy), and coef decays with
    half_life_env_steps so PPO takes over."""

    # "" = off; "auto" = every arena of the stage that a parent (the extended stage, then the merges, in order) has,
    # taught by that parent's best.pt (else latest.pt); or {arena: checkpoint path} with {runs_dir} filled in.
    teachers: str | dict = ""
    coef: float = 1.0
    half_life_env_steps: int = 30_000_000
    min_coef: float = 0.0  # the coefficient never decays below this; below 1e-4 the term is not computed

    def coef_at(self, env_steps: int) -> float:
        decay = 0.5 ** (env_steps / self.half_life_env_steps) if self.half_life_env_steps > 0 else 1.0
        return max(self.min_coef, self.coef * decay)


@dataclass
class CastConfig:
    """Frozen checkpoints in the seats a script used to play (animus.cast): the far side of self-play arenas and
    any agent the stage declares cast (stage.json `cast`). The evaluation never runs them; the sim's `fight`
    baseline stays the yardstick there."""

    # Who plays the opponent seats in training: "" = the live policy (plain self-play); "auto" = the seed chain's
    # parent best.pt; "league" = the parent plus this run's own snapshots (<run_dir>/league/); or a checkpoint path
    # with {runs_dir} and {run_name} filled in.
    opponents: str = ""
    # The league's first member when it should not be the seed chain's parent: a stage whose parent is a PvE policy
    # (the flag stage extends triage) names the last PvP stage's best.pt here, with {runs_dir} filled in.
    parent: str = ""
    opponent_share: float = 0.5  # share of self-play episodes whose far side is cast, drawn per env at episode start
    # stage.json `cast` entries by name -> checkpoint path, e.g. {owner: "{runs_dir}/stage5_pack/best.pt"}.
    agents: dict = field(default_factory=dict)
    deterministic: bool = False  # training samples: an argmax opponent is one the policy learns to exploit
    league_size: int = 8
    snapshot_every_env_steps: int = 5_000_000  # latest.pt joins the league on this clock; best.pt on every improvement
    rate_window: int = 200  # fights per member behind its win-rate average
    floor: float = 0.05  # minimum draw weight, so no member is forgotten
    retire_above: float = 0.85  # a member the live policy beats this often over a full window is retired
    keep_newest: int = 2  # never retired or pruned
    exploiter_floor: float = 0.15  # the least share of the draw each exploiter in the league gets (animus.exploit)

    def resolved_agents(self, runs_dir: str, run_name: str) -> dict[str, str]:
        return {name: str(path).format(runs_dir=runs_dir, run_name=run_name) for name, path in self.agents.items()}


@dataclass
class LayoutSamplingConfig:
    """Training episodes draw a class/build uniformly, so each layout gets its share of the data whatever it is
    worth. A stage is gated on its weakest layout, though, so the data is worth most where the score is lowest --
    against eval.baseline's per-layout score when the stage has one, else the layouts' own scores against each
    other -- and where `metric` falls shortest. After every evaluation the learner sends the sim a weight per layout
    (protocol WEIGHTS) and training episodes draw layouts in proportion; evaluation stays uniform, whatever the
    weights are. Needs eval.every_env_steps.
    """

    enabled: bool = False
    strength: float = 1.0  # e^(strength x gap in standard deviations of the gaps): 0 = uniform
    max_ratio: float = 3.0  # the heaviest layout draws at most this many times the lightest
    # A summary field where higher is better (an episode info column or a derived one such as clean_kill): a layout's
    # need is the larger of its baseline gap and its shortfall on this, each in standard deviations over the layouts.
    # A layout can beat a weak baseline and still fail an absolute gate; this sends the data there too.
    # "" = the baseline gap alone.
    metric: str = ""
    # Replaying lost fights: after every training evaluation the sim is sent the seeds of the episodes that fell
    # short on `metric` (a per-episode 0/1 field such as clean_kill), and this share of training resets rebuilds one
    # of them -- the same character and opponent, with fresh combat rolls -- instead of a new draw. Confirmation seeds
    # are never sent, so the gate that moves the stage on stays held out. 0 = off.
    replay_fraction: float = 0.0
    # A build graded against the others of its role (stage.json spec_roles: tank, healer, damage) on what the role is
    # for: role -> summary fields, higher is better, a leading "-" for lower is better ("-teammates_died"). A build's
    # need is the larger of the above and its shortfall on the worst of its role's fields, each in standard deviations
    # over the role's builds. A party's score is every seat's, so a healer that barely healed scored as well as the
    # seats that carried it and was never drawn more (2026-10-04, stage6's holy paladins). Empty = off.
    role_metrics: dict = field(default_factory=dict)


@dataclass
class TrainConfig:
    run_name: str = "run"
    runs_dir: str = "runs"  # the sim passes AnimusForge.OutputDir/runs
    layouts_dir: str = "layouts"  # the sim passes AnimusForge.OutputDir/layouts
    socket: str = "/tmp/animus-forge.sock"
    # A cluster host's workers' sims ("tcp://host:port" each; AnimusForge.Cluster): trained on together with the sim
    # at `socket` as one pool (animus.env.ClusterEnv). Empty = the one sim. The host's sim passes it.
    cluster_sims: list[str] = field(default_factory=list)
    # Data-parallel learners (animus.parallel; the sim's AnimusForge.Learner.Ranks): this learner's rank, how many
    # there are, and where rank 0 meets the others. One rank is the plain learner.
    # Seconds a worker's sim may leave the learner waiting before it counts as dropped out (its envs sit out and
    # training goes on; it rejoins when it is back).
    cluster_timeout: float = 60.0
    rank: int = 0
    ranks: int = 1
    dist_address: str = "127.0.0.1:29500"
    # A cluster's learners span machines (AnimusForge.Cluster.Learner): rank/ranks are the cluster's, local_rank and
    # local_ranks this machine's, which share its sim's envs (-1 / 0: the same as rank and ranks). dist_iface is the
    # network interface gloo talks on here (GLOO_SOCKET_IFNAME), dist_timeout how long a collective waits for a rank
    # that is gone before the run stops rather than hanging.
    local_rank: int = -1
    local_ranks: int = 0
    dist_iface: str = ""
    dist_timeout: float = 300.0

    def local(self) -> tuple[int, int]:
        """(this learner's index, how many learners) among the ones sharing its sim."""
        return (self.rank if self.local_rank < 0 else self.local_rank,
                self.ranks if self.local_ranks <= 0 else self.local_ranks)
    seed: int = 1

    total_env_steps: int = 5_000_000  # decisions x envs x agents
    rollout_length: int = 128
    log_every: int = 1  # updates
    checkpoint_every: int = 25  # updates
    # And at least every this many env steps (0 = off): with asynchronous learners most steps are the workers', so
    # the leader's updates come several times slower than the run's steps -- a stage7_flight crash at 18.8M env steps
    # was still before update 100, its first checkpoint, and only best.pt survived.
    checkpoint_env_steps: int = 5_000_000
    keep_checkpoints: int = 5  # numbered checkpoint_*.pt files kept (latest.pt and best.pt always are); 0 = all

    # Run the PPO update on a worker thread, so the sim collects the next rollout instead of waiting for it. The
    # rollout then acts on the weights of the update before last (the rollout networks are synced when the update is
    # joined, one rollout later), which is data one update staler than the strictly serial loop; its log_probs come
    # from the same weights, so the PPO ratio stays consistent. Update stats are logged one update late as well.
    #
    # Off by default; stage8_duel turns it on for the whole curriculum that extends it (2026-09-25). The arithmetic argues
    # the other way -- the sim blocks in ReceiveAny for the whole update, which is 36-42% of wall clock on the
    # stages measured -- and the measurement that seemed to refute it (5,365 against 5,323 env steps/s) measured a
    # bug: every overlapped update was joined in the rollout that submitted it, so nothing overlapped. Fixed
    # 2026-09-25; on stage8_duel `python -m animus.bench_learner` gives 11,594 env steps/s overlapped against 6,896
    # serial. Whether one update of staleness is worth that is a training decision, not a speed one.
    overlap_updates: bool = False

    train_device: str = AUTO  # "auto": cuda when torch sees a GPU (ROCm included), else cpu
    rollout_device: str = "cpu"  # per-layout products on the CPU; stage4_duel's "auto" takes the GPU's dense path
    # CPU threads torch may use; 0 = torch's own default (a thread per core). The learner shares the machine with the
    # sim's map update threads, so fewer can be faster overall (the sim's `forge bench` sweeps both).
    torch_threads: int = 0

    # Checkpoints to seed the networks from (see animus.bootstrap): the first candidate that exists. "auto" takes the
    # stage's seed chain from the sim's stage.json (the closest earlier stage that has been trained); a list names
    # them, with {runs_dir} and {run_name} filled in. A best.pt that does not exist falls back to the latest.pt beside
    # it. Empty = train from scratch.
    init_from: str | list[str] = AUTO
    # Which of a parent run's checkpoints to seed from when both exist: "best" or "latest".
    #
    # "latest", because a queue advances on its own and has to hand the next stage what the last one actually
    # learned. best.pt is only rewritten by an evaluation that clears the convergence margin -- the larger of 1%
    # absolute, 2% of the best, and two standard errors of the two scores -- and that last term is the one that
    # bites: with 64-episode evaluations the error bars are wide, so the bar is high, and best.pt can go a whole
    # stage without moving. Measured on the sweep this default was changed for: stage5_pack reached 8.2M steps
    # with its evaluations up from 2.6 to 6.8 and best.pt still the checkpoint it was seeded with, because the
    # 4.16 improvement fell short of a 4.46 margin. Seeding from best there would have handed stage 3 a network
    # that had learned nothing of stage 2.
    #
    # What "best" buys, and what this gives up: best.pt cannot carry a late regression. A stage that destabilises
    # near its end -- an entropy collapse, a bad restart -- passes that on under "latest" and would not under
    # "best". On a real run the two are close anyway, since convergence stops a stage when it stops improving, so
    # latest is near-best by construction; it is short runs where they diverge. Set "best" here for a long build
    # where the protection is worth more than the freshness.
    seed_from: str = "latest"
    # A merge stage's further parents (stage.json merges), seeding the blocks only they have after init_from: "auto"
    # takes each merged stage's best.pt (else latest.pt); a list names checkpoints; empty = none.
    merge_from: str | list[str] = AUTO
    # Fine-tuning a stage on changed rewards or masks: a checkpoint here seeds the run before init_from (the same stage,
    # so every block is copied). Put a finished run's best.pt there before `forge start <stage>` archives the run; empty
    # = never.
    finetune_from: str = "{runs_dir}/_finetune/{run_name}/best.pt"

    mappo: MappoConfig = field(default_factory=MappoConfig)
    distill: DistillConfig = field(default_factory=DistillConfig)
    eval: EvalConfig = field(default_factory=EvalConfig)
    convergence: ConvergenceConfig = field(default_factory=ConvergenceConfig)
    layout_sampling: LayoutSamplingConfig = field(default_factory=LayoutSamplingConfig)
    entropy_floor: EntropyFloorConfig = field(default_factory=EntropyFloorConfig)
    cast: CastConfig = field(default_factory=CastConfig)
    fade: FadeConfig = field(default_factory=FadeConfig)
    costs: CostLadderConfig = field(default_factory=CostLadderConfig)
    style: StyleConfig = field(default_factory=StyleConfig)
    explore: ExploreConfig = field(default_factory=ExploreConfig)
    exploit: ExploitConfig = field(default_factory=ExploitConfig)

    @property
    def shared_runs(self) -> str:
        """The shared movement root's run directory, a sibling of this run's output directory ({shared_runs}).

        A class's runs live in <output>/<class>/runs and the root's in <output>/shared/runs (manual 4, "Training one
        class at a time"), so a class's stage3_rotation names the shared travel checkpoint as
        `{shared_runs}/stage2_travel/best.pt` without knowing where the output directory is.
        """
        return str(Path(self.runs_dir).resolve().parent.parent / "shared" / "runs")

    def format_path(self, path: str) -> str:
        return str(path).format(runs_dir=self.runs_dir, run_name=self.run_name, shared_runs=self.shared_runs)

    def resolved_init_from(self, stage: dict | None) -> list[str]:
        if self.init_from == AUTO:
            return [str(Path(self.runs_dir) / name / "best.pt") for name in seed_chain(stage)]
        candidates = [self.init_from] if isinstance(self.init_from, str) else list(self.init_from or [])
        return [self.format_path(c) for c in candidates if c]

    def resolved_finetune_from(self) -> str:
        return self.format_path(self.finetune_from) if self.finetune_from else ""

    def resolved_merge_from(self, stage: dict | None) -> list[str]:
        if self.merge_from == AUTO:
            return [str(Path(self.runs_dir) / name / "best.pt") for name in merges(stage)]
        candidates = [self.merge_from] if isinstance(self.merge_from, str) else list(self.merge_from or [])
        return [self.format_path(c) for c in candidates if c]

    def named_teachers(self) -> dict[str, str]:
        """distill.teachers as {arena: checkpoint path}, {runs_dir} filled in; {} when off or "auto"."""
        teachers = self.distill.teachers
        if isinstance(teachers, dict):
            return {arena: str(path).format(runs_dir=self.runs_dir, run_name=self.run_name)
                    for arena, path in teachers.items()}
        if teachers not in ("", AUTO):
            raise ValueError(f"distill.teachers: expected \"\", \"auto\" or {{arena: checkpoint}}, got {teachers!r}")
        return {}

    def resolved_train_device(self) -> str:
        return resolve_device(self.train_device)

    def resolved_rollout_device(self) -> str:
        return resolve_device(self.rollout_device)

    @classmethod
    def load(
        cls, path: str | Path, overrides: list[str] | None = None, overlays: list[str | Path] | None = None
    ) -> "TrainConfig":
        """Load YAML (following extends), merge each overlay file over it, then apply "key=value" overrides (dotted
        keys for sections, values parsed as YAML)."""
        raw = load_yaml(path)
        for overlay in overlays or ():
            raw = merge(raw, load_yaml(overlay))
        for override in overrides or ():
            apply_override(raw, override)
        return from_dict(cls, raw)

    def to_dict(self) -> dict:
        return asdict(self)


def resolve_device(name: str) -> str:
    """A device name torch can use. "auto" is the first GPU (CUDA or ROCm) or the CPU. A GPU that is not there --
    "cuda:1" on a machine with one card, set ahead of the second -- falls back to the first GPU, or the CPU without
    one, and says so, so a config written for the machine as it will be still runs on the machine as it is."""
    import torch

    if name == AUTO:
        return "cuda:0" if torch.cuda.is_available() else "cpu"
    if not name.startswith("cuda"):
        return name
    if not torch.cuda.is_available():
        print(f"Device {name} asked for, but torch sees no GPU: using the CPU", flush=True)
        return "cpu"
    index = torch.device(name).index or 0
    count = torch.cuda.device_count()
    if index >= count:
        print(f"Device {name} asked for, but torch sees {count} GPU{'s' if count != 1 else ''}: using cuda:0",
              flush=True)
        return "cuda:0"
    return f"cuda:{index}"


def load_yaml(path: str | Path, seen: tuple[Path, ...] = ()) -> dict:
    """A config file with its extends chain merged in, base first."""
    path = Path(path).resolve()
    if path in seen:
        raise ValueError(f"config {path} extends itself")

    raw = yaml.safe_load(path.read_text()) or {}
    base = raw.pop(EXTENDS_KEY, None)
    if not base:
        return raw
    return merge(load_yaml(path.parent / base, (*seen, path)), raw)


def merge(base: dict, override: dict) -> dict:
    """`override` over `base`: sections merge key by key, anything else is replaced. An empty map replaces too, so
    `metrics: {}` in an overlay clears what the stage set, and `null` drops a single inherited key.

    Dropping one key matters because gate maps accumulate down the chain, and a stage that drops a capability
    keeps its parent's gate on it: the raid stages inherited `owner_deaths` from the party line, have no owner,
    and so could never produce the column the gate asks for. Clearing the whole map was the only way to be rid
    of one entry, which would have taken the gates worth keeping (`livelocked` reaches every stage this way)
    with it. `null` removes the key, so the field falls back to its default."""
    merged = dict(base)
    for key, value in override.items():
        if value is None:
            merged.pop(key, None)
        elif isinstance(value, dict) and value and isinstance(merged.get(key), dict):
            merged[key] = merge(merged[key], value)
        else:
            merged[key] = value
    return merged


def apply_override(raw: dict, override: str) -> None:
    key, sep, value = override.partition("=")
    if not sep or not key:
        raise ValueError(f"override '{override}' is not key=value")

    *sections, name = key.split(".")
    target = raw
    for section in sections:
        target = target.setdefault(section, {})
        if not isinstance(target, dict):
            raise ValueError(f"override '{override}': {section} is not a section")
    target[name] = yaml.safe_load(value)


def _matches(value, hint) -> bool:
    """Whether a YAML value fits a field's type hint (ints fit floats; bools are never numbers)."""
    origin = get_origin(hint)
    if origin in (Union, UnionType):
        return any(_matches(value, arg) for arg in get_args(hint))
    if hint is type(None):
        return value is None
    if hint is float:
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if hint is int:
        return isinstance(value, int) and not isinstance(value, bool)
    if hint in (bool, str, dict):
        return isinstance(value, hint)
    if origin in (tuple, list):
        # YAML gives sequences as lists; elements are checked loosely (a tuple[int, ...] of numbers).
        if not isinstance(value, (list, tuple)):
            return False
        element = next((arg for arg in get_args(hint) if arg is not Ellipsis), None)
        return element is None or all(_matches(item, element) for item in value)
    if origin is dict:
        return isinstance(value, dict)
    return True


def from_dict(cls, raw: dict, prefix: str = ""):
    """Build dataclass `cls` from a dict, recursing into dataclass fields; unknown keys and values of the wrong type
    are an error (so `total_env_steps: 3e8`, which YAML reads as a string, fails at load rather than hours later)."""
    raw = dict(raw or {})
    by_name = {f.name: f for f in fields(cls)}
    unknown = sorted(f"{prefix}{k}" for k in raw if k not in by_name)
    if unknown:
        raise ValueError(f"unknown config keys: {unknown}")

    hints = get_type_hints(cls)
    kwargs = {}
    for name, value in raw.items():
        default = by_name[name].default_factory() if callable(by_name[name].default_factory) else by_name[name].default
        if is_dataclass(default):
            kwargs[name] = from_dict(type(default), value, f"{prefix}{name}.")
            continue
        if not _matches(value, hints[name]):
            raise ValueError(f"config key {prefix}{name}: expected {hints[name]}, got {type(value).__name__} {value!r}")
        if isinstance(default, tuple) and isinstance(value, list):
            kwargs[name] = tuple(value)
        elif hints[name] is float and isinstance(value, int):
            kwargs[name] = float(value)
        else:
            kwargs[name] = value
    return cls(**kwargs)
