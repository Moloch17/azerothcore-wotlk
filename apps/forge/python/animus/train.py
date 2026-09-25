"""Train a MAPPO policy against a running Animus Forge sim.

    python -m animus.train --config configs/stage8_duel.yaml --run-name stage8_duel

The worldserver starts this when told to (`forge start`, `forge resume`, `forge run`) and
AnimusForge.Learner.AutoStart = 1, and passes where runs and layouts go (AnimusForge.OutputDir). Run by hand, the client
retries until the sim's socket appears. A start trains from scratch: an earlier run in <runs_dir>/<run_name>/ is
archived first (animus.runs). With --resume the run continues from its latest.pt instead, as long as the scenario's
layouts and dimensions are unchanged.

Progress (steps, losses, evaluation scores, convergence per class) is kept in <runs_dir>/<run_name>/progress.json for the sim's
console (animus.progress).

With eval.every_env_steps set, the networks are scored on seeded episodes as they train (see
animus.evaluation): the best-scoring networks are kept in best.pt. The stage ends when every class the run
plays has converged on the generic signals (animus.stage), or at total_env_steps; there are no pass gates,
and the learner exits 0 either way so the queue moves on.
"""

from __future__ import annotations

import argparse
import copy
import csv
import json
import random
import time
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import asdict
from pathlib import Path

import numpy as np
import torch
import yaml

from .bootstrap import DIRECTOR_LAYOUT, seed_merges, seed_trainer
from .cast import LEAGUE, Cast, league_snapshot
from .config import TrainConfig
from .distill import Distiller, auto_teachers, build_teacher
from .env import ForgeEnv
from .evaluation import (DERIVED_METRICS, ConvergenceTracker, EvalResult, action_mask_table, casting_weights,
                         format_summary,
                         run_evaluation)
from .mappo.buffer import RolloutBuffer
from .mappo.trainer import MappoTrainer, horizon_seconds, per_decision
from .progress import ProgressWriter
from . import protocol
from .protocol import MAX_SPECS
from .rewards import WARN_EVERY, audit, describe, reward_mix

#: A run whose approx_kl stays under this for STALL_WINDOW updates is told it has stopped moving. Measured
#: against the stages that do stall (they end around 0.001) and those that do not (0.003 to 0.010).
STALL_KL = 0.0015
STALL_WINDOW = 10
STALL_MIN_UPDATES = 20
from .runs import FINISHED_FILE, archive_run, prune_checkpoints, resume_checkpoint_path, resume_mismatch
from .stage import ADVANCE, ConvergenceController, Outcome
from .stages import STAGE_FILE, load_stage


def _rotate(path: Path, columns: list[str]) -> bool:
    """Whether `path` can be appended to under `columns`: it exists and its header is exactly them.

    A resumed run used to take its field names from the file already on disk and write with
    extrasaction="ignore", so a column added since the run started -- a new episode info column, a new reward
    term -- was dropped row after row without a word. layouts.csv had it worse: its field names came from the
    *current* rows while it appended under the *old* header, so every header-based reader mis-attributed the
    extra columns to the wrong names. Both were hit for real once and repaired by renaming the files by hand.

    Rather than guess, a file whose header no longer matches is moved aside and a new one started. The run's
    history is then in two files, which is honest and greppable, instead of one file that quietly lies.
    """
    if not path.exists() or path.stat().st_size == 0:
        return False

    with path.open(newline="") as f:
        header = next(csv.reader(f), [])
    if header == columns:
        return False

    stamp = time.strftime("%Y%m%d-%H%M%S")
    moved = path.with_name(f"{path.stem}-before-{stamp}{path.suffix}")
    path.rename(moved)
    added = [name for name in columns if name not in header]
    gone = [name for name in header if name not in columns]
    change = ", ".join(filter(None, [f"added {', '.join(added)}" if added else "",
                                     f"dropped {', '.join(gone)}" if gone else ""]))
    print(f"  {path.name}: the columns changed since this run started ({change}); the rows so far are in "
          f"{moved.name} and a new file starts here", flush=True)
    return True


class RunLogger:
    """CSV always; TensorBoard when it is installed.

    Columns are fixed up front so metrics that only exist some updates (episode stats) are never dropped. A
    resumed run appends to its metrics.csv, unless its columns have changed since -- see _rotate.
    """

    def __init__(self, run_dir: Path, columns: list[str], append: bool = False):
        self.csv_path = run_dir / "metrics.csv"
        existing = append and not _rotate(self.csv_path, columns)
        existing = existing and self.csv_path.exists() and self.csv_path.stat().st_size > 0
        self._columns = list(columns)
        layouts_path = run_dir / "layouts.csv"
        self._layouts_exist = append and layouts_path.exists() and layouts_path.stat().st_size > 0
        self._layout_writer = None
        self._layout_file = None
        self._csv_file = self.csv_path.open("a" if existing else "w", newline="")
        self._csv_writer = csv.DictWriter(self._csv_file, fieldnames=columns, restval="", extrasaction="ignore")
        if not existing:
            self._csv_writer.writeheader()
        try:
            from torch.utils.tensorboard import SummaryWriter

            self.tb = SummaryWriter(run_dir / "tb")
        except ImportError:
            self.tb = None

    def log_layouts(self, rows: list[dict]) -> None:
        """One row per (class, role) per update (layouts.csv): what each of them is doing right now, from the
        training episodes themselves. metrics.csv averages them all together, which answers how the run is going
        and never which class is in trouble; the evaluation tables answer that but only every
        eval.every_env_steps. These are sampled-policy episodes at each class and role's own ladder difficulty, so
        they are for reading behaviour, not for gating -- the gates stay on the evaluations."""
        if not rows:
            return

        if self._layout_writer is None:
            path = self.csv_path.parent / "layouts.csv"
            columns = list(rows[0])
            # The same check metrics.csv gets, and for the same reason -- more sharply here, because these
            # field names come from the rows themselves and so always match the data, never the old header.
            if self._layouts_exist and _rotate(path, columns):
                self._layouts_exist = False
            self._layout_file = path.open("a" if self._layouts_exist else "w", newline="")
            self._layout_writer = csv.DictWriter(self._layout_file, fieldnames=columns, restval="",
                                                 extrasaction="ignore")
            if not self._layouts_exist:
                self._layout_writer.writeheader()

        for row in rows:
            self._layout_writer.writerow(row)
        self._layout_file.flush()

    def log(self, step: int, row: dict[str, float]) -> None:
        self._csv_writer.writerow(row)
        self._csv_file.flush()

        if self.tb is not None:
            for key, value in row.items():
                self.tb.add_scalar(key, value, step)

    def close(self) -> None:
        self._csv_file.close()
        if self.tb is not None:
            self.tb.close()


def save_checkpoint(
    path: Path, trainer: MappoTrainer, config: TrainConfig, spec, update: int, env_steps: int, extra: dict | None = None
) -> None:
    # Write then rename, so a server stop mid-save never leaves a truncated latest.pt or best.pt behind.
    partial = path.with_suffix(path.suffix + ".partial")
    torch.save(
        {
            "trainer": trainer.state_dict(),
            "config": config.to_dict(),
            "spec": asdict(spec),
            "update": update,
            "env_steps": env_steps,
            **(extra or {}),
        },
        partial,
    )
    partial.replace(path)


class EvalLog:
    """eval.csv (one row per evaluation), eval.jsonl (the full summary, level bands included), eval_episodes.jsonl
    (one row per scored episode), eval_trace.jsonl (every decision of the traced seeds) and stage.jsonl (the decision
    to move on, with every class's convergence signals behind it)."""

    COLUMNS = ["update", "env_steps", "policy", "episodes", "score", "stderr", "margin", "best", "evals_since_best",
               "seconds"]

    def __init__(self, run_dir: Path, tb):
        self.csv_path = run_dir / "eval.csv"
        self.jsonl_path = run_dir / "eval.jsonl"
        self.episodes_path = run_dir / "eval_episodes.jsonl"
        self.trace_path = run_dir / "eval_trace.jsonl"
        self.stage_path = run_dir / "stage.jsonl"
        self.tb = tb

    def write(self, update: int, env_steps: int, result: EvalResult, summary: dict, tracker: ConvergenceTracker) -> None:
        row = {
            "update": update,
            "env_steps": env_steps,
            "policy": result.policy,
            "episodes": result.episodes,
            "score": result.score,
            "stderr": result.stderr,
            "margin": tracker.last_margin,
            "best": tracker.best,
            "evals_since_best": tracker.evals_since_best,
            "seconds": round(result.seconds, 1),
        }
        new_file = not self.csv_path.exists()
        with self.csv_path.open("a", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=self.COLUMNS)
            if new_file:
                writer.writeheader()
            writer.writerow(row)
        with self.jsonl_path.open("a") as f:
            f.write(json.dumps({**row, "summary": summary}) + "\n")

        # The episodes behind the summary, every episode info column of each: which seeds a class failed, not
        # just that its mean is low, and what those episodes have in common.
        with self.episodes_path.open("a") as f:
            for episode in result.episodes_log():
                f.write(json.dumps({"update": update, "env_steps": env_steps, **episode}) + "\n")

        # Decision by decision for the traced seeds (eval.trace_episodes), which is the only place the order of a
        # policy's decisions -- its plan -- can be read.
        if result.trace:
            with self.trace_path.open("a") as f:
                for row in result.trace:
                    f.write(json.dumps({"update": update, "env_steps": env_steps, "policy": result.policy,
                                        **row}) + "\n")

        if self.tb is None or result.policy != "learner":
            return
        for name, value in summary.items():
            if isinstance(value, float):
                self.tb.add_scalar(f"eval/{name}", value, env_steps)
        self.tb.add_scalar("eval/margin", tracker.last_margin, env_steps)
        groups = [*summary.get("bands", {}).items(),
                  *((f"arena_{arena}", values) for arena, values in summary.get("arenas", {}).items()),
                  *((f"build_{build}", values) for build, values in summary.get("builds", {}).items()),
                  *((f"tier_{tier}", values) for tier, values in summary.get("difficulties", {}).items())]
        for group, values in groups:
            for name, value in values.items():
                if isinstance(value, float):
                    self.tb.add_scalar(f"eval_{group}/{name}", value, env_steps)

    def write_outcome(self, update: int, env_steps: int, outcome: Outcome) -> None:
        with self.stage_path.open("a") as f:
            f.write(json.dumps({
                "update": update,
                "env_steps": env_steps,
                "action": outcome.action,
                "reason": outcome.reason,
                "layouts": outcome.report,
            }) + "\n")


SEED_MARKER = "seed_from"


def seed_preference(run_dir: Path, default: str = "latest") -> str:
    """Which of a finished run's checkpoints should seed the stage after it: "best" or "latest".

    A run can carry the answer itself, in a one-word `seed_from` file beside its checkpoints -- which is what the
    dashboard writes when you pick one. It is per run rather than per queue because the reason to want `latest` is
    usually about one stage: best.pt is only rewritten by an evaluation that clears the convergence margin, so a
    stage whose later evaluations scored better without clearing it has a best.pt that is genuinely behind its
    latest.pt, and seeding the next stage from `best` there throws that training away."""
    try:
        choice = (run_dir / SEED_MARKER).read_text().strip().lower()
    except OSError:
        return default
    return choice if choice in ("best", "latest") else default


def init_from_checkpoint(path: str, default: str = "latest") -> Path | None:
    """The seed checkpoint a candidate path resolves to, honouring the run's own `seed_from` choice.

    Either name falls back to the other, so a run that has only ever written one of them still seeds."""
    candidate = Path(path)
    if candidate.name in ("best.pt", "latest.pt"):
        prefer = seed_preference(candidate.parent, default)
        for name in (["latest.pt", "best.pt"] if prefer == "latest" else ["best.pt", "latest.pt"]):
            if (found := candidate.parent / name).exists():
                return found
        return None
    return candidate if candidate.exists() else None


def load_parent(path: Path) -> dict:
    """A parent stage's checkpoint, with the stage.json of its run when it carries none (for block positions)."""
    checkpoint = torch.load(path, map_location="cpu", weights_only=False)
    if checkpoint.get("stage") is None and (path.parent / STAGE_FILE).is_file():
        checkpoint["stage"] = json.loads((path.parent / STAGE_FILE).read_text())
    return checkpoint


def make_distiller(config: TrainConfig, spec, stage: dict | None, parents: list[dict], device) -> Distiller | None:
    """The distillation loss for distill.teachers: auto (from `parents`, extended stage first) or named checkpoints."""
    if not config.distill.teachers:
        return None

    named = config.named_teachers()
    if named:
        chosen = {}
        for arena, candidate in named.items():
            path = init_from_checkpoint(candidate)
            if path is None:
                print(f"Teacher {candidate} for arena {arena} does not exist; that arena is not distilled", flush=True)
                continue
            chosen[arena] = load_parent(path)
    else:
        chosen = auto_teachers(stage, parents)

    if not chosen:
        print("Distillation is on, but no arena has a teacher", flush=True)
        return None

    teachers = {arena: build_teacher(checkpoint, spec, stage, device) for arena, checkpoint in chosen.items()}
    for arena, teacher in teachers.items():
        print(f"Arena {arena} is taught by {teacher.name} ({len(teacher.layouts)} of {len(spec.layouts)} layouts)",
              flush=True)
    return Distiller(stage, teachers)


def use_threads(threads: int) -> None:
    """CPU threads for torch (0 = its own default): the learner runs beside the sim's map update threads."""
    if threads > 0:
        torch.set_num_threads(threads)


def seed_everything(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)


class DecisionRows:
    """One decision's inputs and choices, filled group by group (a half-batch half at a time) and handed to the
    rollout buffer whole."""

    FIELDS = ("obs", "state", "mask", "layout", "actions", "log_probs", "values", "present", "foresight", "memory",
              "goal", "goal_log_prob", "goal_chosen", "critic_memory", "chosen")

    def __init__(self, envs: int, agents: int):
        self.envs = envs
        self.arrays: dict[str, np.ndarray | None] = {}

    def set(self, rows: slice, **values) -> None:
        for name, value in values.items():
            if value is None:
                self.arrays[name] = None
                continue
            array = self.arrays.get(name)
            if array is None:
                array = self.arrays[name] = np.empty((self.envs, *value.shape[1:]), dtype=value.dtype)
            array[rows] = value

    def __getattr__(self, name: str):
        if name in DecisionRows.FIELDS:
            return self.__dict__["arrays"].get(name)
        raise AttributeError(name)

    def recorded(self) -> tuple:
        """RolloutBuffer.add_decision's arguments."""
        a = self.arrays
        goals = (a["goal"], a["goal_log_prob"], a["goal_chosen"]) if a.get("goal") is not None else None
        return (a["obs"], a["state"], a["mask"], a["layout"], a["actions"], a["log_probs"], a["values"], a["present"],
                a.get("foresight"), a.get("memory"), goals, a.get("critic_memory"), a.get("chosen"))


class RolloutOutcome:
    """What one decision earned, filled group by group like DecisionRows: RolloutBuffer.add_outcome's arguments."""

    def __init__(self, envs: int, agents: int, foresight_outputs: int):
        self.reward = np.zeros((envs, agents), dtype=np.float32)
        self.done = np.zeros(envs, dtype=bool)
        self.terminated = np.zeros(envs, dtype=bool)
        self.final_values = np.zeros((envs, agents), dtype=np.float32)
        self.final_foresight = (np.zeros((envs, agents, foresight_outputs), dtype=np.float32)
                                if foresight_outputs else None)


class TrainingRun:
    """One learner run against the sim, from connecting to finished.json.

    The state the phases share -- the networks, the current STEP, the update and env step counters, the stage
    controller -- lives here, so rollouts, evaluation and the stage's end are methods rather than closures over
    one long function.
    """

    def __init__(self, config: TrainConfig, resume: bool):
        self.config = config
        use_threads(config.torch_threads)
        seed_everything(config.seed)

        self.run_dir = Path(config.runs_dir) / config.run_name
        self.resume_path: Path | None = None
        if resume:
            try:
                self.resume_path = resume_checkpoint_path(self.run_dir)
            except FileNotFoundError as error:
                raise SystemExit(str(error)) from None
        elif archived := archive_run(self.run_dir):
            print(f"Archived the earlier {config.run_name} run to {archived}; training from scratch", flush=True)
        self.finished_path = self.run_dir / FINISHED_FILE
        if self.resume_path:
            self.finished_path.unlink(missing_ok=True)
        (self.run_dir / "config.yaml").write_text(yaml.safe_dump(config.to_dict(), sort_keys=False))

        print(f"Connecting to {config.socket} ...", flush=True)
        self.env = ForgeEnv(config.socket)
        self.spec = spec = self.env.spec
        (self.run_dir / "spec.json").write_text(json.dumps(asdict(spec), indent=2))

        # The sim writes stage.json once it has built the scenario, which is before it accepts a learner.
        self.stage = load_stage(config.layouts_dir, spec.scenario)
        if self.stage is not None:
            (self.run_dir / STAGE_FILE).write_text(json.dumps(self.stage, indent=2))
        print(
            f"Scenario {spec.scenario}: {spec.num_envs} envs x {spec.agents_per_env} agents, {len(spec.layouts)} "
            f"layouts (obs up to {spec.obs_dim}, actions up to {spec.num_actions}), state {spec.state_dim}, decision "
            f"every {spec.decision_ms} ms",
            flush=True,
        )
        self.arena_names = tuple(arena["name"] for arena in (self.stage or {}).get("arenas", ()))
        # Each layout's action names, so the evaluations' per-episode logs say which actions were taken.
        self.action_names = {name: layout.get("action_names", [])
                             for name, layout in (self.stage or {}).get("layouts", {}).items()}
        # And its class's build names, in the order the "spec" episode info column indexes them.
        self.spec_names = {name: layout.get("spec_names", [])
                           for name, layout in (self.stage or {}).get("layouts", {}).items()}
        # An evaluation-only action mask (eval.mask_actions), resolved by name per layout here so that a name no
        # layout has is refused before anything trains.
        self.eval_action_mask = action_mask_table(config.eval.mask_actions, [layout.name for layout in spec.layouts],
                                                  self.action_names, spec.num_actions)
        if self.eval_action_mask is not None:
            print(f"Evaluation masks {list(config.eval.mask_actions)} in every layout that has them", flush=True)

        # The layout that decides on a slow clock, by name: its index moves with the stage, and a stage without
        # one simply has no agents of it.
        names = [layout.name for layout in spec.layouts]
        self.slow_layout = names.index(config.mappo.slow_layout) if config.mappo.slow_layout in names else -1
        if config.mappo.slow_layout and self.slow_layout < 0:
            print(f"No layout named {config.mappo.slow_layout!r} in this stage: nothing decides on a slow clock",
                  flush=True)

        self.trainer = MappoTrainer(
            [(layout.obs_dim, layout.num_actions) for layout in spec.layouts],
            spec.state_dim,
            config.mappo,
            train_device=config.resolved_train_device(),
            rollout_device=config.resolved_rollout_device(),
            slow_layout=self.slow_layout,
        )
        # ROCm wears the CUDA API's name: torch.cuda is HIP on an AMD card and the device prints as "cuda",
        # which reads as though the wrong backend were in use. Say what it actually is, and which card.
        def named(device: torch.device) -> str:
            if device.type != "cuda":
                return str(device)
            backend = "rocm" if torch.version.hip else "cuda"
            return f"{backend}:{device.index or 0} ({torch.cuda.get_device_name(device.index or 0)})"

        print(f"Updates on {named(self.trainer.train_device)}, rollouts on {named(self.trainer.rollout_device)}",
              flush=True)

        # Horizons are configured in game time; each decision compounds them.
        self.discounts = per_decision(config.mappo, spec.decision_ms)
        # The foresight heads' horizons, in seconds of game time: a discount whose 1 / (1 - gamma) is that many
        # decisions, and the scale the share of the episode left is measured on.
        seconds = spec.decision_ms / 1000.0
        self.foresight_discounts = tuple(max(0.0, 1.0 - seconds / max(seconds, horizon))
                                         for horizon in config.mappo.foresight_horizons_seconds)
        self.foresight_time_decisions = config.mappo.foresight_time_scale_seconds / max(1e-6, seconds)
        gamma, gae_lambda = self.discounts
        print(
            f"Per {spec.decision_ms} ms decision: gamma {gamma:.5f} (horizon "
            f"{horizon_seconds(gamma, spec.decision_ms):.0f} s), GAE trace {gamma * gae_lambda:.5f} (credit "
            f"{horizon_seconds(gamma * gae_lambda, spec.decision_ms):.1f} s), rollout "
            f"{config.rollout_length * spec.decision_ms / 1000.0:.1f} s",
            flush=True,
        )
        if self.trainer.slow_layout >= 0:
            every = max(1, config.mappo.slow_every_decisions)
            step_ms = every * spec.decision_ms
            slow_gamma = config.mappo.slow_gamma
            slow_trace = slow_gamma * config.mappo.slow_gae_lambda
            print(
                f"Per {step_ms / 1000.0:.1f} s {config.mappo.slow_layout} decision ({every} of them): gamma "
                f"{slow_gamma:.5f} (horizon {horizon_seconds(slow_gamma, step_ms):.0f} s), GAE trace "
                f"{slow_trace:.5f} (credit {horizon_seconds(slow_trace, step_ms):.0f} s), rollout "
                f"{config.rollout_length / every:.1f} of its decisions",
                flush=True,
            )

        self.evaluating = config.eval.every_env_steps > 0
        self.controller = ConvergenceController(config, [layout.name for layout in spec.layouts])
        self.tracker = self.controller.tracker
        self.update = 0
        self.env_steps = 0
        self._load_or_seed()

        def new_buffer() -> RolloutBuffer:
            return RolloutBuffer(config.rollout_length, spec.num_envs, spec.agents_per_env, spec.obs_dim,
                                 spec.state_dim, spec.num_actions, self.trainer.foresight_outputs,
                                 self.trainer.recurrent_size, bool(self.trainer.goal_count))

        # What the policy carries between decisions (its memory and the goal it pursues), cleared with an episode.
        self.acting = self.trainer.acting_state(spec.num_envs, spec.agents_per_env)

        self.buffer = new_buffer()
        # Overlapped updates fill this one while the update reads the other; they swap after every rollout.
        self.spare_buffer = new_buffer() if config.overlap_updates else None
        self.pending_update: Future | None = None
        self.carried_stats: dict[str, float] | None = None  # an update drained outside a rollout, still to log
        self.rollout_reward = 0.0
        self.rollout_allowed_actions = 0.0
        self.started_at = time.perf_counter()
        self.updater = ThreadPoolExecutor(max_workers=1, thread_name_prefix="update") \
            if config.overlap_updates else None
        columns = [
            "update", "env_steps", "env_steps_per_sec", "update_seconds", "reward_per_decision", "episodes",
            *(f"episode_{name}" for name in spec.episode_info_names),
            "policy_loss", "value_loss", "entropy", "entropy_coef", "clip_frac", "approx_kl",
            "explained_variance", "actor_grad_norm", "critic_grad_norm", "epochs_run", "allowed_actions",
            "lr_scale", "frozen_layouts", "cast_rows", "cast_fallback_rows", "cast_members", "cast_hardest_win_rate",
            "elapsed_seconds", "update_compute_seconds", "distill_coef", "distill_kl", "distill_rows",
        ]
        if self.trainer.goal_count:
            # What the goal head is doing: the entropy it is kept at, how often a chosen goal is the one held, and
            # the share of decisions spent under each goal.
            columns += ["goal_entropy", "goal_kept_share",
                        *(f"goal_{index}_share" for index in range(self.trainer.goal_count))]
        self.logger = RunLogger(self.run_dir, columns, append=self.resume_path is not None)
        self.report = tuple(config.eval.report)
        self.eval_log = EvalLog(self.run_dir, self.logger.tb)
        # Self-play arenas scored against the baseline as their opponent (eval.opponent_baseline).
        self.opponents = config.eval.baseline if config.eval.opponent_baseline else ""
        self.baselines: dict[tuple[int, int], dict] = {}
        self.best_path = self.run_dir / "best.pt"

        self.progress = ProgressWriter(self.run_dir, config, spec, resumed_update=self.update,
                                       resumed_env_steps=self.env_steps)
        cached_baseline_score = None
        if self.resume_path and (self.run_dir / "eval_baseline.json").exists():
            cached = json.loads((self.run_dir / "eval_baseline.json").read_text())
            cached_baseline_score = cached.get("summary", {}).get("score")
        self.progress.restore_evaluation(self.tracker, cached_baseline_score, self.controller)

        self.step = None
        self.last_eval_env_steps = 0
        self.finished_episodes: list[np.ndarray] = []
        # The class each of those episodes was played by, so an update can say what each one is doing rather
        # than only what all eighteen did on average.
        self.finished_layouts: list[int] = []
        # A party seat left empty for an episode reports present = 0; its row is not an episode.
        names = spec.episode_info_names
        self.present_column = names.index("present") if "present" in names else None
        # The live seats' `won` at an episode's end is what the league scores its members by (animus.cast).
        self.won_column = names.index("won") if "won" in names else None
        # The update a reward-mix warning was last printed on, so a run that trips it says so without saying it
        # every update for the rest of the run.
        self.reward_warned_at: int | None = None
        # The recent KL, and when a stall was last mentioned (audit_progress).
        self.recent_kl: list[float] = []
        self.stall_warned_at: int | None = None
        # The learning-rate scale in force (animus.stage reads the KL against it), the layouts whose classes have
        # converged (frozen and out of the training draw), and the last update's per-class statistics.
        self.lr_scale_now = 1.0
        self.frozen = np.zeros(0, dtype=np.int64)
        self.layout_allowed: dict[int, float] = {}
        self.last_layout_stats: dict[str, dict[str, float]] = {}
        self.difficulty_column = names.index("difficulty") if "difficulty" in names else None
        self.apply_holds()

    # ------------------------------------------------------------------ setup

    def _load_or_seed(self) -> None:
        """Resume the run's latest.pt, or seed the fresh networks from the stage this one extends and a merge stage's
        further parents; either way the parents teach a distilled run (self.distiller)."""
        config, spec = self.config, self.spec
        self.cast: Cast | None = None
        self.last_snapshot_env_steps = 0
        if self.resume_path:
            checkpoint = torch.load(self.resume_path, map_location="cpu", weights_only=False)
            if mismatch := resume_mismatch(checkpoint.get("spec", {}), asdict(spec)):
                raise SystemExit(f"cannot resume {config.run_name}: the scenario's {', '.join(mismatch)} changed since "
                                 f"{self.resume_path} was saved; start it fresh instead")
            self.trainer.load_state_dict(checkpoint["trainer"])
            self.update = int(checkpoint.get("update", 0))
            self.env_steps = int(checkpoint.get("env_steps", 0))
            # The convergence test and the best evaluation carry on where the run stopped.
            self.tracker.load_state_dict(checkpoint.get("convergence"))
            self.controller.load_state_dict(checkpoint.get("controller"))
            print(f"Resumed {config.run_name} from {self.resume_path} at update {self.update}, {self.env_steps} env "
                  f"steps", flush=True)

        # The parents: the extended stage's checkpoint (the first init_from candidate that exists) and a merge stage's
        # further parents.
        finetune = config.resolved_finetune_from()
        if finetune and Path(finetune).is_file() and not self.resume_path:
            print(f"Fine-tuning from {finetune}", flush=True)
        candidates = config.resolved_init_from(self.stage)
        if finetune and Path(finetune).is_file():
            candidates = [finetune, *candidates]
        prefer = config.seed_from
        base_path, base = self.seed_candidate(candidates, prefer, spec, config.init_from == "auto")
        merge_paths = []
        for candidate in config.resolved_merge_from(self.stage):
            if path := init_from_checkpoint(candidate, prefer):
                merge_paths.append(path)
            else:
                print(f"Merged stage checkpoint {candidate} does not exist: nothing is seeded or taught from it",
                      flush=True)
        merged = [load_parent(path) for path in merge_paths]

        if not self.resume_path and base is not None:
            seeded = seed_trainer(self.trainer, base, spec, self.stage)
            print(f"Seeded the networks from {base_path}: trunk and {len(seeded)} of {len(spec.layouts)} layouts",
                  flush=True)
            for layout, blocks in (seed_merges(self.trainer, merged, spec, self.stage, base) if merged else {}).items():
                print(f"  {layout}: {', '.join(blocks)} from the merged stages", flush=True)
        elif not self.resume_path and candidates:
            print(f"None of {', '.join(candidates)} to seed from; starting from scratch", flush=True)

        self.distiller = make_distiller(config, spec, self.stage, [p for p in (base, *merged) if p is not None],
                                        self.trainer.train_device)

        # Frozen checkpoints in the seats a script used to play (animus.cast): the far side of self-play arenas,
        # from the parent the networks seeded from and this run's own league, and any agent the stage declares.
        cast_config = copy.copy(config.cast)
        cast_config.agents = config.cast.resolved_agents(config.runs_dir, config.run_name)
        if cast_config.opponents not in ("", "auto", LEAGUE):
            cast_config.opponents = cast_config.opponents.format(runs_dir=config.runs_dir, run_name=config.run_name)
        if cast_config.parent:
            cast_config.parent = cast_config.parent.format(runs_dir=config.runs_dir, run_name=config.run_name)
        if cast_config.opponents or cast_config.agents:
            self.cast = Cast(cast_config, spec, self.stage, self.run_dir, self.trainer.rollout_device, base_path,
                             seed=config.seed)
            self.last_snapshot_env_steps = self.env_steps
            if self.cast.pool is not None:
                members = [member.path.name for member in self.cast.pool.active()]
                print(f"Cast opponents ({cast_config.opponents}, share {cast_config.opponent_share:.0%}): "
                      f"{', '.join(members) or 'nobody yet'}", flush=True)
                self.cast.pool.write()
            elif cast_config.opponents:
                print(f"cast.opponents is {cast_config.opponents!r} but the stage has no self-play arena "
                      f"(or no parent checkpoint): the live policy plays every seat", flush=True)
            for agent, actor in self.cast.statics.items():
                print(f"Cast agent {agent} is played by {actor.path}", flush=True)

    @staticmethod
    def seed_candidate(candidates: list[str], prefer: str, spec, auto: bool) -> tuple[Path | None, dict | None]:
        """The first init_from candidate that exists and, on the automatic seed chain, covers every layout this run
        plays: (path, loaded checkpoint), or (None, None).

        A restricted stage (the stealth drill) played by an all-class run leaves a checkpoint holding only the layouts
        it played. On the automatic chain that checkpoint is stepped over, with a line saying which one and why, and
        the stage seeds from the next one down -- the stage before the restricted one, which every class did play.
        An explicit `init_from` path is never stepped over: seeding the missing layouts from random weights in the
        middle of a curriculum is what animus.bootstrap refuses, loudly, when it is asked to."""
        for candidate in candidates:
            path = init_from_checkpoint(candidate, prefer)
            if path is None:
                continue
            checkpoint = load_parent(path)
            if auto:
                names = {layout["name"] for layout in checkpoint["spec"].get("layouts", ())}
                missing = [layout.name for layout in spec.layouts
                           if layout.name not in names and layout.name != DIRECTOR_LAYOUT]
                if missing:
                    print(f"Not seeding from {path}: it has no {', '.join(missing)} (a restricted stage's checkpoint); "
                          f"trying the next stage down the chain", flush=True)
                    continue
            return path, checkpoint
        return None, None

    # ------------------------------------------------------------------ checkpoints

    def _checkpoint_extra(self) -> dict:
        # The stage (its block positions) travels with the checkpoint, for seeding the stages that extend it.
        return {"convergence": self.tracker.state_dict(), "controller": self.controller.state_dict(),
                "stage": self.stage}

    def _save(self, path: Path) -> None:
        self.drain_update()
        save_checkpoint(path, self.trainer, self.config, self.spec, self.update, self.env_steps,
                        self._checkpoint_extra())

    def maybe_checkpoint(self) -> None:
        if self.update % self.config.checkpoint_every == 0:
            self._save(self.run_dir / f"checkpoint_{self.update:06d}.pt")
            self._save(self.run_dir / "latest.pt")
            prune_checkpoints(self.run_dir, self.config.keep_checkpoints)

    # ------------------------------------------------------------------ evaluation

    def learner_actions(self):
        """A chooser for one evaluation, with the acting state that evaluation carries through its episodes."""
        return self._acting(self.config.eval.deterministic)

    def _acting(self, deterministic: bool):
        """A chooser for run_evaluation that carries a recurrent actor's memory between decisions and clears it where
        an episode has just ended (the step it is given is the new one's first)."""
        acting = self.trainer.acting_state(self.spec.num_envs, self.spec.agents_per_env)
        forbidden = getattr(self, "eval_action_mask", None)

        def choose(step):
            acting.clear(step.done)
            # The sim's mask less the actions the evaluation may not take (eval.mask_actions), per layout.
            mask = step.mask if forbidden is None else np.logical_and(step.mask, ~forbidden[step.layout])
            actions = self.trainer.act(step.obs, mask, step.layout, deterministic, acting)[0]
            return (actions, acting.goal) if acting.goal is not None else actions

        return choose

    def baseline_for(self, seed: int, episodes: int) -> dict | None:
        """The eval.baseline policy's summary on these seeds, scored once per run."""
        config = self.config
        if not config.eval.baseline:
            return None
        if (seed, episodes) in self.baselines:
            return self.baselines[seed, episodes]

        is_eval_seeds = (seed, episodes) == (config.eval.seed, config.eval.episodes)
        baseline_path = self.run_dir / ("eval_baseline.json" if is_eval_seeds
                                        else f"eval_baseline_{seed}_{episodes}.json")
        key = {"policy": config.eval.baseline, "seed": seed, "episodes": episodes, "opponents": self.opponents,
               "arenas": list(self.arena_names),
               # The tuning prices the reward terms the baseline is scored in: a change must score it again.
               "tuning": (self.stage or {}).get("tuning")}
        cached = json.loads(baseline_path.read_text()) if baseline_path.exists() else None
        if cached and cached.get("key") == key:
            summary = cached["summary"]
        else:
            result, _ = run_evaluation(self.env, self.spec, self.learner_actions(), episodes, seed,
                                       baseline=config.eval.baseline, opponents=self.opponents,
                                       arenas=self.arena_names, action_names=self.action_names)
            summary = result.summary(self.report)
            baseline_path.write_text(json.dumps({"key": key, "summary": summary}, indent=2))
            self.eval_log.write(self.update, self.env_steps, result, summary, self.tracker)
            print(f"Baseline {config.eval.baseline}: score {result.score:.4g} over {result.episodes} seeded "
                  f"episodes (seed {seed}, {result.seconds:.0f} s)", flush=True)
        self.baselines[seed, episodes] = summary
        return summary

    def maybe_league_snapshot(self) -> None:
        """Every cast.snapshot_every_env_steps the current networks join the league (animus.cast): best.pt alone
        moves only behind the convergence margin, and a league fed from it alone goes stale."""
        cast = self.config.cast
        if (self.cast is None or self.cast.pool is None or cast.opponents != LEAGUE
                or cast.snapshot_every_env_steps <= 0
                or self.env_steps - self.last_snapshot_env_steps < cast.snapshot_every_env_steps):
            return
        self.last_snapshot_env_steps = self.env_steps
        latest = self.run_dir / "latest.pt"
        self._save(latest)
        if league_snapshot(self.run_dir, latest, f"step_{self.env_steps}") is not None:
            self.cast.pool.reload()
            self.cast.pool.write()
            print(f"League: latest.pt at {self.env_steps} env steps joined ({len(self.cast.pool.active())} members)",
                  flush=True)

    def evaluate(self) -> None:
        """Score the networks on the seeds (and the baseline once per run); the next training STEP becomes current."""
        self.drain_update()
        config, tracker, controller = self.config, self.tracker, self.controller
        controller.baseline_summary = self.baseline_for(config.eval.seed, config.eval.episodes)
        baseline_summary = controller.baseline_summary

        self.progress.write("evaluating", self.update, self.env_steps)
        result, self.step = run_evaluation(self.env, self.spec, self.learner_actions(), config.eval.episodes,
                                           config.eval.seed, opponents=self.opponents, arenas=self.arena_names,
                                           action_names=self.action_names,
                                           trace_episodes=config.eval.trace_episodes)
        summary = result.summary(self.report)
        if self.cast is not None:
            self.cast.reset_all()
            controller.observe_league(self.cast.league_stats([layout.name for layout in self.spec.layouts]))
        improved = controller.observe(summary, self.env_steps)
        self.eval_log.write(self.update, self.env_steps, result, summary, tracker)
        self.progress.evaluated(self.env_steps, result.score, baseline_summary["score"] if baseline_summary else None,
                                tracker, controller)
        self.progress.write("training", self.update, self.env_steps)
        self.last_eval_env_steps = self.env_steps

        against = f", baseline {baseline_summary['score']:.4g}" if baseline_summary else ""
        print(f"Eval at {self.env_steps} env steps: score {result.score:.4g} +/- {result.stderr:.2g} "
              f"(best {tracker.best:.4g}, {tracker.evals_since_best} evals since, margin {tracker.last_margin:.2g})"
              f"{against}; "
              f"{result.episodes} episodes in {result.seconds:.0f} s"
              f" [learner/baseline]\n{format_summary(summary, baseline_summary, self.report)}", flush=True)

        if improved:
            self._save(self.best_path)
            if self.cast is not None and self.cast.pool is not None and config.cast.opponents == LEAGUE:
                if league_snapshot(self.run_dir, self.best_path, f"best_{self.env_steps}") is not None:
                    self.cast.pool.reload()
        if self.cast is not None and self.cast.pool is not None:
            self.cast.pool.write()

        sampled_every = config.eval.sampled_every
        if sampled_every > 0 and len(tracker.history) % sampled_every == 0:
            self.evaluate_sampled(summary)

        self.apply_holds()
        self.send_layout_weights(summary, baseline_summary)
        self.send_replay(result)

    def apply_holds(self) -> None:
        """Freeze the classes that have converged (animus.stage) and keep them out of the rollout's samples."""
        converged = set(self.controller.converged_layouts())
        frozen = [index for index, layout in enumerate(self.spec.layouts) if layout.name in converged]
        if set(frozen) != set(int(index) for index in self.frozen):
            entering = sorted(converged - {self.spec.layouts[i].name for i in self.frozen})
            leaving = sorted({self.spec.layouts[i].name for i in self.frozen} - converged)
            if entering:
                print(f"Converged and out of the training draw: {', '.join(entering)}", flush=True)
            if leaving:
                print(f"Regressed and back in the training draw: {', '.join(leaving)}", flush=True)
        self.frozen = np.asarray(frozen, dtype=np.int64)
        self.trainer.freeze_layouts(set(frozen))

    def evaluate_sampled(self, argmax: dict) -> None:
        """Score sampled actions on the evaluation seeds, next to the argmax evaluation that just ran."""
        config = self.config
        result, self.step = run_evaluation(
            self.env, self.spec,
            self._acting(False),
            config.eval.episodes, config.eval.seed, opponents=self.opponents, arenas=self.arena_names,
            action_names=self.action_names)
        result.policy = "learner_sampled"
        if self.cast is not None:
            self.cast.reset_all()
        summary = result.summary(self.report)
        fields = [name for name in ("score", "clean_kill", "killed", "died", "timed_out", "arrived")
                  if name in summary]
        # The gap, sampled minus argmax, kept with the sampled row of eval.jsonl: a wide one says the gated policy is
        # not the one that trained, which is what mappo.entropy_final_fraction is there to close.
        summary["argmax_gap"] = {name: float(summary[name]) - float(argmax[name])
                                 for name in fields if isinstance(argmax.get(name), (int, float))}
        self.eval_log.write(self.update, self.env_steps, result, summary, self.tracker)
        print("Sampled vs argmax actions on the evaluation seeds: " + ", ".join(
            f"{name} {summary[name]:.4g} / {argmax.get(name, float('nan')):.4g}" for name in fields), flush=True)

    def send_replay(self, result: EvalResult) -> None:
        """Send the sim the seeds this evaluation lost, for training resets to rebuild (protocol REPLAY)."""
        sampling = self.config.layout_sampling
        if not sampling.enabled or sampling.replay_fraction <= 0.0 or not sampling.metric:
            return

        seeds = result.failed_seeds(sampling.metric)
        self.env.set_replay(self.config.eval.seed, sampling.replay_fraction, seeds)
        print(f"Replaying {len(seeds)} lost evaluation episodes in {sampling.replay_fraction:.0%} of training resets",
              flush=True)

    def send_layout_weights(self, summary: dict, baseline: dict | None) -> None:
        """Weight the training episodes toward the (class, build) pairs furthest below their baseline (WEIGHTS).

        The wire vector is one weight per pair, layout-major in the spec's layout order and spec-minor, MAX_SPECS
        wide, so a class with fewer builds than that still has the slots -- never drawn, and left at the even 1.0.
        Per pair and not per layout because a layout is a whole class: weighting a paladin by its average would
        send more tanking episodes to fix its healing. Per build and not per role because two builds of one role
        are not equally hard to win with, which is the case a feral druid is.
        """
        sampling = self.config.layout_sampling
        weights = {}
        if sampling.enabled and baseline is not None and summary.get("castings"):
            weights = casting_weights(summary, baseline, sampling.strength, sampling.max_ratio, sampling.metric)
        hold = self.controller.hold_weights()
        if not weights and all(factor == 1.0 for factor in hold.values()):
            return

        vector = []
        for layout in self.spec.layouts:
            named = self.spec_names.get(layout.name, [])
            for slot in range(MAX_SPECS):
                spec = named[slot] if slot < len(named) else ""
                weight = weights.get(f"{layout.name}_{spec}", 1.0) if spec else 1.0
                vector.append(weight * hold.get(layout.name, 1.0))

        self.env.set_layout_weights(vector)
        heaviest = sorted(weights.items(), key=lambda item: -item[1])[:3]
        held = [name for name, factor in hold.items() if factor != 1.0]
        print("Layout weights: " + ", ".join(f"{name} {weight:.2f}" for name, weight in heaviest)
              + f" (of {len(weights)} class/builds, {len(vector)} slots)"
              + (f"; held at {self.config.convergence.hold_share:.0%}: {', '.join(held)}" if held else ""), flush=True)

    # ------------------------------------------------------------------ stage decisions

    def handle(self, outcome: Outcome) -> bool:
        """Carry out the controller's decision; True when training stops."""
        if outcome.action != ADVANCE:
            return False
        tracker = self.tracker
        self.eval_log.write_outcome(self.update, self.env_steps, outcome)
        pending = {name: row["missing"] for name, row in (outcome.report or {}).items() if not row["converged"]}
        print(f"Stage complete ({outcome.reason}): best score {tracker.best:.4g} at {tracker.best_env_steps} env "
              f"steps; {len((outcome.report or {})) - len(pending)} of {len(outcome.report or {})} classes converged.",
              flush=True)
        for name, missing in pending.items():
            print(f"  {name}: not converged, missing {', '.join(missing) or 'nothing'}", flush=True)
        return True

    # ------------------------------------------------------------------ training

    def finish_update(self) -> dict[str, float] | None:
        """Wait for an overlapped update to finish and hand its weights to the rollout networks. None if none ran."""
        if self.pending_update is None:
            return None

        pending, self.pending_update = self.pending_update, None
        stats = pending.result()  # an update that raised re-raises here, on the training thread
        self.trainer.sync_rollout()
        return stats

    def drain_update(self) -> None:
        """Finish any overlapped update, so the networks are whole: before an evaluation, a checkpoint or a restart.
        Its stats are kept for the next logged row."""
        if (stats := self.finish_update()) is not None:
            self.carried_stats = stats

    def rollout(self) -> tuple[dict[str, float], float, float]:
        """Fill the buffer from the sim and update the networks; returns (update stats, start time, rollout s)."""
        spec, trainer, buffer = self.spec, self.trainer, self.buffer
        envs, agents = spec.num_envs, spec.agents_per_env
        buffer.reset()
        started = time.perf_counter()

        # self.acting is never reset between rollouts, only where an episode ended: a policy's memory carries on
        # across rollout boundaries, so what it remembers is bounded by the episode, not by rollout_length. The
        # update replays each rollout from the memory its first decision was taken with, so only the gradient is
        # truncated there.
        #
        # A decision is taken group by group. In half-batch (the sim ticks one half's maps while the other half
        # decides) each half is answered as soon as its STEP arrives, so this side's inference runs while the sim
        # ticks the other half: the sim and the learner stop taking turns. Otherwise there is one group, the whole
        # pool, and env.step -- also what half-batch falls back to with a cast, which acts on whole decisions.
        pipelined = len(self.env.groups) > 1 and self.cast is None
        groups = self.env.groups if pipelined else [(0, envs)]
        sent: dict[str, np.ndarray | None] = {}

        def send(begin: int, count: int, actions: np.ndarray, goals: np.ndarray | None) -> None:
            if pipelined:
                self.env.send_act(begin, actions, goals)
            else:
                sent["actions"], sent["goals"] = actions, goals

        def receive(begin: int, count: int) -> protocol.Step:
            if pipelined:
                part = self.env.receive_step()
                if (part.env_begin, part.done.shape[0]) != (begin, count):
                    raise ConnectionError(f"expected the STEP of envs {begin}+{count}, got {part.env_begin}+"
                                          f"{part.done.shape[0]}")
                return part
            return self.env.step(sent["actions"], sent["goals"])

        decision = self._act_on_rows_of(self.step, groups, send)
        while True:
            buffer.add_decision(*decision.recorded())
            last = buffer.cursor + 1 >= buffer.steps
            outcome = RolloutOutcome(envs, agents, trainer.foresight_outputs)
            following = None if last else DecisionRows(envs, agents)
            parts = []
            for begin, count in groups:
                part = receive(begin, count)
                parts.append(part)
                rows = slice(begin, begin + count)
                self._take_outcome_of(part, rows, decision, outcome)
                if following is not None:
                    self._act_on_rows(part, rows, following, send)
            self.step = protocol.join_steps(parts)
            buffer.add_outcome(outcome.reward, outcome.done, outcome.terminated, outcome.final_values,
                               outcome.final_foresight)
            if following is None:
                break
            decision = following

        rollout_seconds = time.perf_counter() - started
        buffer.finish(trainer.value(self.step.state, self.step.obs, self.step.layout, self.acting.goal,
                                    self.acting.critic_memory),
                      *self.discounts,
                      last_foresight=trainer.foresight_of(self.step.obs, self.step.layout, self.acting.memory),
                      foresight_gammas=self.foresight_discounts,
                      time_scale_decisions=self.foresight_time_decisions,
                      slow_layout=self.slow_layout,
                      slow_gamma=self.config.mappo.slow_gamma,
                      slow_gae_lambda=self.config.mappo.slow_gae_lambda)
        # Read before the buffers swap below: log_update runs on the rollout that has just been collected.
        self.rollout_reward = buffer.mean_reward()
        self.rollout_allowed_actions = buffer.mean_allowed_actions()
        self.layout_allowed = self.allowed_actions_by_layout(buffer)
        trainer.entropy_coef = self.controller.entropy_coef(self.env_steps)
        # With an overlapped update this applies to the update submitted below: a rollout's worth late, which a
        # schedule over hundreds of millions of steps does not notice. The scale is the controller's: held at full
        # until the score first plateaus, so the KL it reads is the policy's and not the schedule's.
        self.lr_scale_now = self.controller.lr_scale(self.env_steps)
        trainer.set_learning_rate_scale(self.lr_scale_now)
        if self.distiller is not None:
            self.distiller.coef = self.config.distill.coef_at(self.env_steps)

        self.update += 1
        self.env_steps += self.config.rollout_length * envs * agents
        self.maybe_league_snapshot()

        if self.updater is None:
            stats = trainer.update(buffer, self.distiller)
            return stats, started, rollout_seconds

        # Overlapped: the update of the rollout before last has been running while this one was collected. Take its
        # stats and its weights, then hand this rollout to the worker and collect the next one meanwhile. The
        # networks are synced on the join, so the rollout that follows acts on the weights of the update before it.
        stats = self.finish_update()
        self.pending_update = self.updater.submit(trainer.update, buffer, self.distiller, sync=False)
        self.buffer, self.spare_buffer = self.spare_buffer, buffer
        if stats is None:
            stats, self.carried_stats = self.carried_stats, None
        # The first rollout has no finished update to report, and its row goes without update stats (log_update
        # takes that). It used to wait for its own update instead, which left the next rollout nothing to join, so
        # it waited too, and so on: every update was joined where it was submitted and overlap_updates never
        # overlapped anything.
        return stats if stats is not None else {}, started, rollout_seconds

    def _act_on_rows_of(self, step: protocol.Step, groups: list[tuple[int, int]], send) -> DecisionRows:
        """Act on every group of a decision already received whole (the one a rollout starts from)."""
        decision = DecisionRows(self.spec.num_envs, self.spec.agents_per_env)
        for begin, count in groups:
            rows = slice(begin, begin + count)
            self._act_on_rows(protocol.rows_of(step, begin, count), rows, decision, send)
        return decision

    def _act_on_rows(self, part: protocol.Step, rows: slice, decision: DecisionRows, send) -> None:
        """The policy's decision for envs `rows` (`part` is their STEP), recorded into `decision` and sent."""
        trainer = self.trainer
        memory = self.acting.memory[rows].copy() if self.acting.memory is not None else None
        critic_memory = self.acting.critic_memory[rows].copy() if self.acting.critic_memory is not None else None
        # A non-finite observation reaches the networks as a non-finite logit and comes back out of
        # torch.multinomial as "probability tensor contains either `inf`, `nan` or element < 0" -- an error
        # that names neither the observation nor the seat it came from, several layers away from whichever
        # block wrote it. Caught here it names both, which is the difference between a fix and a hunt.
        if not np.isfinite(part.obs).all():
            bad = np.argwhere(~np.isfinite(part.obs))
            where = ", ".join(f"env {int(e) + rows.start} agent {int(a)} obs[{int(i)}]={part.obs[e, a, i]}"
                              for e, a, i in bad[:8])
            raise RuntimeError(
                f"{len(bad)} non-finite observation(s) from the sim at step {self.env_steps}: {where}"
                + ("" if len(bad) <= 8 else f" (and {len(bad) - 8} more)"))

        acting = self.acting.take(rows)
        actions, log_probs, values, foresight, goals, chosen = trainer.act_and_value(
            part.obs, part.mask, part.layout, part.state, state=acting)
        self.acting.put(rows, acting)
        # A converged class still plays (its rows are needed to act and to carry the recurrence) but is not a
        # sample: its adapter and head are frozen, and the trunk is trained on the classes still learning.
        present = part.present
        if len(self.frozen):
            present = present & ~np.isin(part.layout, self.frozen)
        # A cast row (a frozen checkpoint's seat) takes the frozen actor's action and is not a sample either. A cast
        # acts on whole decisions, so a run with one is never pipelined and `part` is the whole pool.
        if self.cast is not None:
            cast_rows = self.cast.rows(part)
            if cast_rows.any():
                actions = self.cast.act(part, actions, cast_rows)
                present = present & ~cast_rows
        goal, goal_log_prob, goal_chosen = goals if goals is not None else (None, None, None)
        decision.set(rows, obs=part.obs, state=part.state, mask=part.mask, layout=part.layout, actions=actions,
                     log_probs=log_probs, values=values, present=present, foresight=foresight, memory=memory,
                     goal=goal, goal_log_prob=goal_log_prob, goal_chosen=goal_chosen, critic_memory=critic_memory,
                     chosen=chosen)
        send(rows.start, rows.stop - rows.start, actions, goals[0] if goals is not None else None)

    def _take_outcome_of(self, part: protocol.Step, rows: slice, decision: DecisionRows,
                         outcome: RolloutOutcome) -> None:
        """What `decision` earned in envs `rows`, from their next STEP `part`: rewards, and for the episodes that
        ended, their final values, their info, and a cleared memory for the episodes that follow."""
        trainer, spec = self.trainer, self.spec
        done = part.done
        outcome.reward[rows], outcome.done[rows], outcome.terminated[rows] = part.reward, done, part.terminated
        if not done.any():
            return

        # The ended episodes' layouts are the decision's: the STEP already carries the new episodes'. Only the envs
        # that finished are valued: an env ends an episode once in hundreds of decisions, so valuing all of them and
        # then throwing most away is a forward pass over ~20x the rows needed.
        layout = decision.layout[rows]
        memory = decision.memory[rows] if decision.memory is not None else None
        # The goal in force is the one the ended episode's last decision pursued, which the value depends on; the
        # memory the critic ends the episode with is the last decision's own, which acting has carried forward and
        # clear() has not yet reset.
        goal = self.acting.goal[rows] if self.acting.goal is not None else None
        critic_end = self.acting.critic_memory[rows] if self.acting.critic_memory is not None else None
        final_values = outcome.final_values[rows]
        final_values[done] = trainer.value(part.final_state[done], part.final_obs[done], layout[done],
                                           goal[done] if goal is not None else None,
                                           critic_end[done] if critic_end is not None else None)
        outcome.final_values[rows] = final_values
        if outcome.final_foresight is not None:
            final_foresight = outcome.final_foresight[rows]
            final_foresight[done] = trainer.foresight_of(part.final_obs[done], layout[done],
                                                         memory[done] if memory is not None else None)
            outcome.final_foresight[rows] = final_foresight
        ended = part.episode_info[done].reshape(-1, spec.episode_info_dim)
        ended_layouts = layout[done].reshape(-1)
        present = self.present_column
        keep = slice(None) if present is None else ended[:, present] > 0.0
        self.finished_episodes.extend(ended[keep])
        self.finished_layouts.extend(int(index) for index in ended_layouts[keep])

        # A new episode starts with nothing remembered and no goal; the league scores the ended ones.
        if self.cast is not None:
            self.cast.observe_ended(part, self.won_column)
            self.cast.clear(part.done)
        cleared = np.zeros(spec.num_envs, dtype=bool)
        cleared[rows] = done
        self.acting.clear(cleared)

    @staticmethod
    def allowed_actions_by_layout(buffer: RolloutBuffer) -> dict[int, float]:
        """Mean legal actions per decision, per layout index, over the rollout's samples."""
        layout = buffer.layout.reshape(-1)
        valid = buffer.valid.reshape(-1)
        allowed = buffer.mask.reshape(-1, buffer.mask.shape[-1]).sum(-1)
        out = {}
        for index in np.unique(layout[valid]):
            rows = valid & (layout == index)
            out[int(index)] = float(allowed[rows].mean()) if rows.any() else 0.0
        return out

    def named_layout_stats(self) -> dict[str, dict[str, float]]:
        """The last update's per-class entropy, approx_kl and allowed actions, by layout name."""
        names = [layout.name for layout in self.spec.layouts]
        out = {}
        for index, stats in self.trainer.layout_stats.items():
            if index < len(names):
                out[names[index]] = {**stats, "allowed_actions": self.layout_allowed.get(index, 0.0)}
        return out

    def log_update(self, stats: dict[str, float], started: float, rollout_seconds: float) -> None:
        config, spec = self.config, self.spec
        self.last_layout_stats = self.named_layout_stats()
        self.controller.observe_update(self.last_layout_stats, self.lr_scale_now)
        if self.difficulty_column is not None and self.finished_episodes:
            names = [layout.name for layout in spec.layouts]
            episodes = np.asarray(self.finished_episodes)
            layouts = np.asarray(self.finished_layouts)
            self.controller.observe_training_episodes({
                names[index]: float(episodes[layouts == index, self.difficulty_column].mean())
                for index in np.unique(layouts) if index < len(names)})
        if self.update % config.log_every != 0:
            return

        row: dict[str, float] = {
            "update": self.update,
            "env_steps": self.env_steps,
            "env_steps_per_sec": config.rollout_length * spec.num_envs * spec.agents_per_env / rollout_seconds,
            "update_seconds": time.perf_counter() - started - rollout_seconds,
            "reward_per_decision": self.rollout_reward,
            # Entropy is only readable against how many actions were legal to begin with.
            "allowed_actions": self.rollout_allowed_actions,
            "elapsed_seconds": time.perf_counter() - self.started_at,
            "episodes": len(self.finished_episodes),
            "entropy_coef": self.trainer.entropy_coef,
            "lr_scale": self.lr_scale_now,
            "frozen_layouts": len(self.frozen),
            **(self.cast.stats() if self.cast is not None else {"cast_rows": 0.0, "cast_fallback_rows": 0.0}),
            **({"distill_coef": self.distiller.coef} if self.distiller is not None else {}),
        }
        if self.finished_episodes:
            means = np.mean(self.finished_episodes, axis=0)
            for name, value in zip(spec.episode_info_names, means):
                row[f"episode_{name}"] = float(value)
            self.log_layout_rows(spec)
        row.update(stats)

        # The floor reads the entropy this update reached against how many actions were legal for it.
        if "entropy" in row:
            self.controller.observe_entropy(row["entropy"], self.rollout_allowed_actions)

        self.audit_reward(row)
        self.audit_progress(row)
        self.logger.log(self.update, row)
        self.progress.training(row)
        self.progress.write("training", self.update, self.env_steps)
        summary = ", ".join(
            f"{k} {v:.4g}" for k, v in row.items() if k.startswith("episode_") or k in ("entropy", "value_loss")
        )
        print(f"update {self.update} | steps {self.env_steps} | {row['env_steps_per_sec']:.0f} sps | {summary}",
              flush=True)
        self.finished_episodes.clear()
        self.finished_layouts.clear()

    def audit_progress(self, row: dict[str, float]) -> None:
        """Say so when the updates have stopped moving the policy.

        Roughly half the stages measured end their run barely changing: approx_kl falls eight to eleven fold
        between the first eighth of a run and the last (the party stage 11.2x, duo_led 10.5x, companion 9.2x,
        stage4 8.3x) with clip_frac down to ~0.01, so the final third costs wall clock and buys very little.
        The other half do not -- stage8_duel's KL *rises* over 683 updates, travel and flight stay flat -- so
        this is reported and never acted on. Stopping a stalled run automatically would have cut stage4
        short, and it went on to 916 updates.
        """
        kl = row.get("approx_kl")
        if kl is None or self.update < STALL_MIN_UPDATES:
            return

        # Against the learning rate in force: a KL that fell with the anneal is the schedule, not a stall.
        kl = float(kl) / max(self.lr_scale_now, 1e-6)
        self.recent_kl.append(float(kl))
        if len(self.recent_kl) > STALL_WINDOW:
            self.recent_kl.pop(0)
        if len(self.recent_kl) < STALL_WINDOW or max(self.recent_kl) >= STALL_KL:
            return

        if self.stall_warned_at is not None and self.update - self.stall_warned_at < WARN_EVERY:
            return

        self.stall_warned_at = self.update
        print(f"  learning has stalled: approx_kl has stayed under {STALL_KL:g} for {STALL_WINDOW} updates "
              f"(now {kl:.2g}, clip_frac {row.get('clip_frac', 0.0):.2g}). The policy is barely moving; if the "
              f"evaluation is not improving either, the rest of this run is wall clock.", flush=True)

    def audit_reward(self, row: dict[str, float]) -> None:
        """Say so when a shaping term has become the thing being optimised (animus.rewards)."""
        finding = audit(reward_mix(row))
        if finding is None:
            self.reward_warned_at = None
            return

        if self.reward_warned_at is not None and self.update - self.reward_warned_at < WARN_EVERY:
            return

        self.reward_warned_at = self.update
        print(f"  {describe(finding, reward_mix(row))}", flush=True)

    def log_layout_rows(self, spec) -> None:
        """Per (class, build) means of this update's training episodes, one row each (RunLogger.log_layouts).

        Split by build and not only by layout, because a layout is a whole class: averaging a paladin's healing
        episodes into its tanking ones would report a number describing neither, and episode_spec itself would
        come out as a fraction between two builds. The class is the `layout` column and the build its own.
        """
        names = [layout.name for layout in spec.layouts]
        episodes = np.asarray(self.finished_episodes)
        layouts = np.asarray(self.finished_layouts)
        info = list(spec.episode_info_names)
        role_at = info.index("spec") if "spec" in info else None
        rows = []
        for index in np.unique(layouts):
            name = names[index] if index < len(names) else str(index)
            mine = episodes[layouts == index]
            if not len(mine):
                continue

            if role_at is None:
                groups = [("", mine)]
            else:
                named = self.spec_names.get(name, [])
                drawn = mine[:, role_at].astype(int)
                groups = [(named[spec] if 0 <= spec < len(named) else str(spec), mine[drawn == spec])
                          for spec in np.unique(drawn)]

            for spec, group in groups:
                if not len(group):
                    continue

                means = np.mean(group, axis=0)
                row = {"update": self.update, "env_steps": self.env_steps, "layout": name, "spec": spec,
                       "episodes": len(group)}
                # The class's own convergence signals (animus.stage): what its policy is doing, not only what its
                # episodes came to.
                signals = self.last_layout_stats.get(name, {})
                row.update({"entropy": signals.get("entropy", float("nan")),
                            "approx_kl": signals.get("approx_kl", float("nan")),
                            "allowed_actions": signals.get("allowed_actions", float("nan")),
                            "lr_scale": self.lr_scale_now, "frozen": int(name in
                                                                        {self.spec.layouts[i].name for i in self.frozen})})
                row.update({f"episode_{field}": float(value) for field, value in zip(info, means)})
                rows.append(row)

        self.logger.log_layouts(rows)

    def train(self) -> Outcome:
        """Train until every class has converged, or the step budget runs out."""
        config, controller = self.config, self.controller
        while self.env_steps < config.total_env_steps:
            stats, started, rollout_seconds = self.rollout()
            self.log_update(stats, started, rollout_seconds)
            self.maybe_checkpoint()

            if self.evaluating and self.env_steps - self.last_eval_env_steps >= config.eval.every_env_steps:
                # The evaluation resets every env: the training episodes in progress are cut short, and the next
                # rollout starts from fresh ones (this rollout's advantages were already computed above).
                self.evaluate()
                if self.handle(decision := controller.after_eval(self.env_steps)):
                    return decision

        # One last score, so the best model also considers the final networks.
        if self.evaluating and self.last_eval_env_steps < self.env_steps:
            self.evaluate()
        outcome = controller.at_budget()
        self.handle(outcome)
        return outcome

    def run(self) -> int:
        """The whole run; returns the process exit code."""
        self.step = self.env.reset()
        if self.cast is not None:
            self.cast.reset_all()
        self.progress.write("training", self.update, self.env_steps)
        if self.evaluating and self.config.eval.at_start and not self.tracker.history:
            self.evaluate()
        self.last_eval_env_steps = self.tracker.history[-1][0] if self.tracker.history else self.env_steps

        outcome: Outcome | None = None
        try:
            outcome = self.train()
        finally:
            self.finish(outcome)

        return 0

    def finish(self, outcome: Outcome | None) -> None:
        """Save latest.pt and, when the stage was decided, finished.json; close the logs and the connection."""
        tracker = self.tracker
        self._save(self.run_dir / "latest.pt")
        if outcome:
            self.finished_path.write_text(json.dumps({
                "reason": outcome.reason,
                "advanced": outcome.action == ADVANCE,
                "env_steps": self.env_steps,
                "update": self.update,
                "best_score": tracker.best,
                "best_env_steps": tracker.best_env_steps,
                "layouts": outcome.report,
            }, indent=2))
            print(f"{self.config.run_name} finished: {outcome.reason}", flush=True)
        self.progress.write("finished" if outcome else "stopped", self.update, self.env_steps,
                            outcome.reason if outcome else "", advanced=bool(outcome and outcome.action == ADVANCE))
        self.drain_update()
        if self.updater is not None:
            self.updater.shutdown()
        self.logger.close()
        self.env.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True)
    parser.add_argument("--socket", help="sim socket path, overriding the config")
    parser.add_argument(
        "--run-name", help="run name (runs/<name>/), overriding the config; the sim passes its scenario"
    )
    parser.add_argument("--runs-dir", help="where runs go, overriding the config; the sim passes its own")
    parser.add_argument("--layouts-dir", help="where the sim writes layouts and stage.json, overriding the config")
    parser.add_argument(
        "--overlay", action="append", default=[], metavar="YAML",
        help="merge this config over --config, section by section (before --set), e.g. configs/fast.yaml",
    )
    parser.add_argument(
        "--set", action="append", default=[], metavar="KEY=VALUE",
        help="override a config value, e.g. --set total_env_steps=5000000 --set eval.every_env_steps=1000000",
    )
    parser.add_argument(
        "--resume", action="store_true",
        help="continue runs/<run name>/latest.pt instead of archiving the run and training from scratch",
    )
    args = parser.parse_args()

    config = TrainConfig.load(args.config, args.set, args.overlay)
    if args.socket:
        config.socket = args.socket
    if args.run_name:
        config.run_name = args.run_name
    if args.runs_dir:
        config.runs_dir = args.runs_dir
    if args.layouts_dir:
        config.layouts_dir = args.layouts_dir

    return TrainingRun(config, resume=args.resume).run()


if __name__ == "__main__":
    raise SystemExit(main())
