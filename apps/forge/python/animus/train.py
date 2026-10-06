"""Train a MAPPO policy against a running Animus Forge sim.

    python -m animus.train --config configs/stage4_duel.yaml --run-name stage4_duel

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
import dataclasses
import json
import random
import time
from concurrent.futures import Future, ThreadPoolExecutor
from contextlib import nullcontext
from dataclasses import asdict
from pathlib import Path

import numpy as np
import torch
import yaml

from .bootstrap import DIRECTOR_LAYOUT, seed_merges, seed_trainer
from .cast import EXPLOITER_MEMBER, LEAGUE, LEAGUE_DIR, Cast, league_snapshot
from .config import TrainConfig
from .distill import Distiller, auto_teachers, build_teacher
from .env import ClusterEnv, ForgeEnv
from .explore import ExploreArchive, cells_of, mark_columns
from .exploit import JOIN, Exploit, exploiter_name
from .evaluation import (DERIVED_METRICS, ConvergenceTracker, EvalResult, action_mask_table, casting_weights,
                         format_summary,
                         run_evaluation)
from .mappo.buffer import RolloutBuffer
from .mappo.trainer import MappoTrainer, horizon_seconds, per_decision, schedule
from .mappo.networks import check_image_bytes, seat_sets_of, vision_of
from .progress import ProgressWriter
from . import blas, episode_means, protocol
from .async_sync import Hub, Link, fetch_shared, shared_listing
from .parallel import Ranks, Silent, weighted_share
from .protocol import MAX_SPECS
from .rewards import WARN_EVERY, audit, describe, outcome_terms, reward_mix
from .human import motion, realism
from .style import HumanWindows, StyleReward, startup_line as style_line

#: A run whose approx_kl stays under this for STALL_WINDOW updates is told it has stopped moving. Measured
#: against the stages that do stall (they end around 0.001) and those that do not (0.003 to 0.010).
STALL_KL = 0.0015
STALL_WINDOW = 10
STALL_MIN_UPDATES = 20
from .runs import FINISHED_FILE, archive_run, prune_checkpoints, resume_checkpoint_path, resume_mismatch
from .stage import ADVANCE, ConvergenceController, Outcome, restore_evaluation_state
from .stages import STAGE_FILE, arena_names, layout_changes, load_stage
from .device import host


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

    def __init__(self, run_dir: Path, tb, extra_columns: list[str] | tuple[str, ...] = ()):
        """`extra_columns`: summary values eval.csv carries after the fixed ones (the realism score's, with
        style.reference). A file under other columns is moved aside first (_rotate), so its rows never misalign."""
        self.csv_path = run_dir / "eval.csv"
        self.jsonl_path = run_dir / "eval.jsonl"
        self.episodes_path = run_dir / "eval_episodes.jsonl"
        self.trace_path = run_dir / "eval_trace.jsonl"
        self.stage_path = run_dir / "stage.jsonl"
        self.tb = tb
        self.columns = [*self.COLUMNS, *extra_columns]
        _rotate(self.csv_path, self.columns)

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
        for name in self.columns[len(self.COLUMNS):]:
            value = summary.get(name)
            row[name] = value if isinstance(value, (int, float)) else ""
        new_file = not self.csv_path.exists()
        with self.csv_path.open("a", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=self.columns)
            if new_file:
                writer.writeheader()
            writer.writerow(row)
        with self.jsonl_path.open("a") as f:
            f.write(json.dumps({**{name: value for name, value in row.items() if value != ""}, "summary": summary})
                    + "\n")

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


def init_from_checkpoint(path: str, prefer: str = "latest") -> Path | None:
    """The seed checkpoint a candidate path resolves to: `prefer` ("best" or "latest", TrainConfig.seed_from) of the
    run's two, whichever name the path gives.

    Either name falls back to the other, so a run that has only ever written one of them still seeds."""
    candidate = Path(path)
    if candidate.name in ("best.pt", "latest.pt"):
        for name in (["latest.pt", "best.pt"] if prefer == "latest" else ["best.pt", "latest.pt"]):
            if (found := candidate.parent / name).exists():
                return found
        return None
    return candidate if candidate.exists() else None


#: The held-out evaluation's seeds are the evaluation's moved on by this much, so its characters are its own.
HELDOUT_SEED_OFFSET = 7919


def heldout_due(evaluations: int, every: int, final: bool, improved: bool) -> bool:
    """Whether the held-out arenas are played after the `evaluations`-th evaluation: every `every`-th, the stage's
    last, and one that saved a new best.pt."""
    return final or improved or evaluations % max(1, every) == 0


def heldout_arenas(heldout: dict, stage: dict | None) -> dict[str, tuple[int, int]]:
    """eval.heldout resolved against stage.json: {arena: (MODE's pin, episodes)}. An arena the stage does not have, or
    has but trains on (not "eval_only"), is refused: evaluating on trained content would read as generalisation."""
    arenas = [arena.get("name") for arena in (stage or {}).get("arenas", ())]
    out = {}
    for name, episodes in (heldout or {}).items():
        if name not in arenas:
            raise ValueError(f"eval.heldout names arena {name!r}, which the stage does not have (it has "
                             f"{', '.join(map(str, arenas)) or 'none'})")
        if not (stage or {})["arenas"][arenas.index(name)].get("eval_only"):
            raise ValueError(f"eval.heldout names arena {name!r}, which the stage trains on: only an arena it holds "
                             f"out (ArenaDefinition::EvalOnly) measures generalisation")
        if not isinstance(episodes, int) or episodes <= 0:
            raise ValueError(f"eval.heldout.{name}: expected a positive episode count, got {episodes!r}")
        out[name] = (arenas.index(name) + 1, episodes)
    return out


def baseline_cache_key(policy: str, seed: int, episodes: int, opponents: str, arenas, tuning,
                       score_kind: str, shaping_scale: float = 1.0) -> dict:
    """What a cached eval_baseline*.json summary is only good for: the scripted policy, the seeds, the opponents, the
    arenas, the tuning that prices the reward terms, and the kind of score it was summarised on ("" = the return).
    A summary on the return read against outcome scores would skew every per-class gap the training draw weights
    by (casting_weights). And the shaping scale it was played at: its outcome score should not move with it, but its
    return does, and the summary carries both."""
    return {"policy": policy, "seed": seed, "episodes": episodes, "opponents": opponents, "arenas": list(arenas),
            "tuning": tuning, "score": score_kind, "shaping": round(float(shaping_scale), 6)}


def reward_terms_line(stage: dict | None) -> str:
    """What the sim says its reward terms are for (stage.json "reward_terms"), counted: the startup line."""
    categories = (stage or {}).get("reward_terms")
    if not categories:
        return "reward terms: no categories in stage.json (a sim from before peak-play W0), audited by name"
    counts = {kind: sum(1 for value in categories.values() if value == kind) for kind in ("outcome", "cost", "shaping")}
    return f"reward terms: {counts['outcome']} outcome, {counts['cost']} cost, {counts['shaping']} shaping"


def fade_line(config: TrainConfig) -> str:
    fade = config.fade
    if not fade.enabled:
        return "shaping fade off (shaping paid in full)"
    rungs = " ".join(f"{scale:g}" for scale in fade.rungs)
    return (f"shaping fade on: rungs {rungs}, window {fade.window}, regress_z {fade.regress_z:g}, "
            f"give_up {fade.give_up}")


def costs_line(config: TrainConfig) -> str:
    costs = config.costs
    if not costs.enabled:
        return "cost ladder off (noise priced in full)"
    rungs = " ".join(f"{scale:g}" for scale in costs.rungs)
    return (f"cost ladder on: rungs {rungs}, window {costs.window}, regress_z {costs.regress_z:g}, "
            f"give_up {costs.give_up}")


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
              "goal", "goal_log_prob", "goal_chosen", "slow_before", "slow_value", "critic_memory", "chosen",
              "goal_slots", "image")

    #: A field store_inputs wrote into the rollout buffer itself.
    IN_BUFFER = object()

    #: The large host fields set() writes straight into the rollout buffer at this decision's step (the field's name
    #: there): copied once, where a decision of their own copied them twice -- into here, then into the buffer.
    DIRECT = {"obs": "obs", "state": "state", "mask": "mask", "memory": "memory", "critic_memory": "critic_memory",
              "image": "image"}

    def __init__(self, envs: int, agents: int, buffer=None, t: int = 0):
        """`buffer` and `t`: the rollout buffer and the step this decision is recorded at (store_inputs)."""
        self.envs = envs
        self.buffer, self.t = buffer, t
        self.arrays: dict[str, np.ndarray | None] = {}

    def _direct(self, name: str, value):
        """The rollout buffer's array this field goes straight into, or None (no buffer, a device value, or a buffer
        array on the device)."""
        if self.buffer is None or name not in DecisionRows.DIRECT or not isinstance(value, np.ndarray):
            return None
        target = getattr(self.buffer, DecisionRows.DIRECT[name], None)
        if not isinstance(target, np.ndarray) or target.shape[2:] != value.shape[1:]:
            return None
        return target

    def set(self, rows: slice, **values) -> None:
        for name, value in values.items():
            if value is None:
                self.arrays[name] = None
                continue
            if (target := self._direct(name, value)) is not None:
                target[self.t, rows] = value
                self.arrays[name] = DecisionRows.IN_BUFFER
                continue
            array = self.arrays.get(name)
            if array is None:
                # A device view (protocol 15) is kept on its device, copied there: it is the sim's buffer, which the
                # next STEP of this group overwrites.
                array = self.arrays[name] = (torch.empty((self.envs, *value.shape[1:]), dtype=value.dtype,
                                                         device=value.device) if isinstance(value, torch.Tensor)
                                             else np.empty((self.envs, *value.shape[1:]), dtype=value.dtype))
            array[rows] = value

    def store_inputs(self, rows: slice, obs, state, mask, stream, image=None) -> None:
        """The sim's device inputs of envs `rows`, straight into the rollout buffer at this decision's step, queued on
        `stream`: recorded() then leaves them out. `image`: the camera's bytes, with a camera (protocol 21)."""
        for name, value in (("obs", obs), ("state", state), ("mask", mask), ("image", image)):
            if value is None:
                continue
            self.buffer.store_rows(name, self.t, rows, value, stream)
            self.arrays[name] = DecisionRows.IN_BUFFER

    def __getattr__(self, name: str):
        if name in DecisionRows.FIELDS:
            value = self.__dict__["arrays"].get(name)
            if value is DecisionRows.IN_BUFFER:
                return getattr(self.__dict__["buffer"], DecisionRows.DIRECT.get(name, name))[self.__dict__["t"]]
            return value
        raise AttributeError(name)

    def recorded(self) -> tuple:
        """RolloutBuffer.add_decision's arguments."""
        a = {name: (None if value is DecisionRows.IN_BUFFER else value) for name, value in self.arrays.items()}
        goals = ((a["goal"], a["goal_log_prob"], a["goal_chosen"], a.get("slow_before"), a.get("slow_value"),
                  a.get("goal_slots")) if a.get("goal") is not None else None)
        return (a["obs"], a["state"], a["mask"], a["layout"], a["actions"], a["log_probs"], a["values"], a["present"],
                a.get("foresight"), a.get("memory"), goals, a.get("critic_memory"), a.get("chosen"), a.get("image"))


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
        # Data-parallel learners (animus.parallel): each rank samples its own actions and shuffles its own
        # minibatches, and rank 0 alone writes the run.
        # Or asynchronous ones (mappo.rank_sync = async, animus.async_sync): no collective at all, so every rank is a
        # learner of its own to this code -- the leader writes the run, a follower (leader False) writes nothing -- and
        # the networks are traded with the leader's Hub in the background.
        self.async_ranks = config.ranks > 1 and config.mappo.rank_sync == "async"
        if self.async_ranks:
            self.ranks = Ranks(0, 1, device=config.resolved_train_device())
            self.ranks.rank, self.ranks.leader = config.rank, config.rank == 0
        else:
            self.ranks = Ranks(config.rank, config.ranks, config.dist_address, config.resolved_train_device(),
                               iface=config.dist_iface, timeout=config.dist_timeout)
        self.hub: Hub | None = None
        self.link: Link | None = None
        leader = self.ranks.leader
        seed_everything(config.seed + config.rank)

        self.run_dir = Path(config.runs_dir) / config.run_name
        self.resume_path: Path | None = None
        # A learner on another machine than the leader's (a worker's rank: the first, and only, of its machine) keeps
        # a runs directory of its own that nothing but it writes. It never resumes from it -- the leader's networks
        # are the run's, and every rank takes them once they are loaded -- and a fresh start archives it as the
        # leader archives its own: a stale stage1_move/latest.pt from an earlier run made every worker's learner
        # refuse to resume.
        remote = not leader and config.local()[0] == 0
        if resume and not remote:
            try:
                self.resume_path = resume_checkpoint_path(self.run_dir)
            except FileNotFoundError as error:
                if leader:
                    raise SystemExit(str(error)) from None
        elif (leader or remote) and not resume and (archived := archive_run(self.run_dir)):
            print(f"Archived the earlier {config.run_name} run to {archived}; training from scratch", flush=True)
        self.ranks.barrier()  # the others read the run directory only once the leader has archived it
        self.finished_path = self.run_dir / FINISHED_FILE
        if self.resume_path and leader:
            self.finished_path.unlink(missing_ok=True)
        if leader:
            (self.run_dir / "config.yaml").write_text(yaml.safe_dump(config.to_dict(), sort_keys=False))

        # Every learner of this machine shares its sim (its own share of the pool); the workers without a learner of
        # their own are dealt round this machine's. A cluster's learners on other machines have their own sims.
        local_rank, local_ranks = config.local()
        workers = config.cluster_sims[local_rank::local_ranks]
        print(f"Connecting to {config.socket}{f' (rank {config.rank} of {config.ranks})' if config.ranks > 1 else ''}"
              " ...", flush=True)
        self.env = (ClusterEnv([config.socket, *workers], rank=local_rank, ranks=local_ranks,
                               timeout=config.cluster_timeout)
                    if workers else ForgeEnv(config.socket, rank=local_rank, ranks=local_ranks,
                                             device=config.resolved_rollout_device()))
        self.spec = spec = self.env.spec
        # Env steps count every rank's envs: budgets, schedules and evaluations are the run's, not a rank's.
        self.run_envs = int(self.ranks.sum(torch.tensor(spec.num_envs)))
        # Every rank's envs, on every rank: evaluations share their seeds out in proportion (weighted_share).
        self.rank_envs = self.ranks.broadcast(self.ranks.gather(spec.num_envs)) if self.ranks.active \
            else [spec.num_envs]
        if leader:
            (self.run_dir / "spec.json").write_text(json.dumps(asdict(spec), indent=2))

        # The sim writes stage.json once it has built the scenario, which is before it accepts a learner.
        self.stage = load_stage(config.layouts_dir, spec.scenario)
        if self.stage is not None and leader:
            (self.run_dir / STAGE_FILE).write_text(json.dumps(self.stage, indent=2))
        if leader:
            print(f"{config.run_name}: {reward_terms_line(self.stage)}; {fade_line(config)}; {costs_line(config)}",
                  flush=True)
        print(
            f"Scenario {spec.scenario}: {spec.num_envs} envs x {spec.agents_per_env} agents, {len(spec.layouts)} "
            f"layouts (obs up to {spec.obs_dim}, actions up to {spec.num_actions}), state {spec.state_dim}, decision "
            f"every {spec.decision_ms} ms",
            flush=True,
        )
        self.arena_names = tuple(arena["name"] for arena in (self.stage or {}).get("arenas", ()))
        self.heldout = heldout_arenas(config.eval.heldout, self.stage) if self.stage is not None else {}
        self.heldout_current = False   # whether the latest evaluation played them
        # eval.phases: a curriculum phase's arenas, for a row per phase in the evaluation summary. A name the stage
        # has no arena of is a config written for another stage: refused rather than reported as an empty phase.
        self.phases = {str(phase): tuple(str(name) for name in names)
                       for phase, names in config.eval.phases.items()}
        unknown = sorted({name for names in self.phases.values() for name in names} - set(self.arena_names))
        if self.phases and unknown:
            raise ValueError(f"eval.phases names arenas {self.stage and self.stage.get('scenario')} does not have: "
                             f"{unknown} (its arenas: {list(self.arena_names)})")
        # Each layout's action names, so the evaluations' per-episode logs say which actions were taken.
        self.action_names = {name: layout.get("action_names", [])
                             for name, layout in (self.stage or {}).get("layouts", {}).items()}
        # And its class's build names, in the order the "spec" episode info column indexes them.
        self.spec_names = {name: layout.get("spec_names", [])
                           for name, layout in (self.stage or {}).get("layouts", {}).items()}
        # Each build's role by its casting name ("paladin_holy": "healer"), for layout_sampling.role_metrics.
        self.casting_roles = {f"{name}_{spec}": role
                              for name, layout in (self.stage or {}).get("layouts", {}).items()
                              for spec, role in zip(layout.get("spec_names", []), layout.get("spec_roles", []))}
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

        # The director's members and enemies are sets (stage.json "director"): its layout index and descriptor.
        director = ((names.index(DIRECTOR_LAYOUT), self.stage["director"])
                    if self.stage and "director" in self.stage and DIRECTOR_LAYOUT in names else None)
        # Every seat layout's entities as sets (mappo.seat_sets, stage.json layouts.<name>.sets): off, None.
        seat_sets = seat_sets_of(self.stage, names) if config.mappo.seat_sets else None
        if config.mappo.seat_sets and seat_sets is None:
            print("mappo.seat_sets is on, but no layout of this stage has a seat set: the networks have none here",
                  flush=True)
        # The camera (stage.json's vision block): on in every network wherever the stage has it, no switch.
        vision = vision_of(self.stage, names)
        # The sim's SPEC and stage.json must agree about the camera's bytes (protocol 21).
        try:
            check_image_bytes(vision, spec.image_bytes)
        except ValueError as error:
            raise SystemExit(f"vision: {error}") from None
        if vision is not None:
            image = next(entry for entry in vision if entry is not None)
            print(f"Vision: a {image['width']} x {image['height']} camera in {sum(e is not None for e in vision)} of "
                  f"{len(names)} layouts, one encoder per network", flush=True)
        def make_trainer() -> MappoTrainer:
            # The exploiter's (animus.exploit) is built by this too: the same layouts, director and seat sets, the
            # same ranks -- its update is data-parallel like the main's.
            return MappoTrainer(
                [(layout.obs_dim, layout.num_actions) for layout in spec.layouts],
                spec.state_dim,
                config.mappo,
                train_device=config.resolved_train_device(),
                rollout_device=config.resolved_rollout_device(),
                slow_layout=self.slow_layout,
                ranks=self.ranks.update,
                director=director,
                seat_sets=seat_sets,
                vision=vision,
            )

        self.make_trainer = make_trainer
        self.trainer = make_trainer()
        if self.trainer.actor.entity_sets is not None and self.trainer.actor.entity_sets.attention:
            sets = self.trainer.actor.entity_sets
            layer = sum(parameter.numel() for name, parameter in sets.named_parameters()
                        if not name.startswith(("encoders.", "queries.", "pool.")))
            print(f"Entity attention: {sets.HEADS} heads over {sum(slots for slots, _ in sets.shapes.values()) + 1} "
                  f"tokens of {sets.embed}, {layer} parameters a network beside the sets'", flush=True)
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
        if config.mappo.recurrent_size <= 0:
            raise ValueError(f"mappo.recurrent_size: expected the recurrent actor's memory width (> 0; stage4_duel "
                             f"sets 128), got {config.mappo.recurrent_size!r} -- the flat update was removed")
        self.controller = ConvergenceController(
            config, [layout.name for layout in spec.layouts],
            sim_fallback_ceiling=float(((self.stage or {}).get("tuning") or {}).get("Markers.FallbackCeiling", 0.0)))
        self.tracker = self.controller.tracker
        # What the evaluations are scored on (EvalResult.score_column): eval.score's column where the sim writes it,
        # else the return ("") -- the kind a checkpoint's scores are of, and a baseline's.
        wanted = config.eval.score_column()
        self.score_kind = wanted if wanted in spec.episode_info_names else ""
        self.update = 0
        self.env_steps = 0
        # Go-Explore starts for the wings (explore.enabled): the archive of cells the ended runs reached, kept by the
        # leader from every rank's runs, and the version of it the sim was last sent.
        self.explore: ExploreArchive | None = None
        # The exploiter (exploit.enabled, animus.exploit): built after the seed, from a resumed state if any.
        self.exploit: Exploit | None = None
        self.exploit_resume: dict | None = None
        self.explore_columns = mark_columns(spec.episode_info_names) if config.explore.enabled else None
        self.explore_sent = -1
        self.explore_version = 0
        if config.explore.enabled and self.explore_columns is None:
            print("explore.enabled, but this stage reports no wing cells (no wing arena): every run starts at the "
                  "door", flush=True)
        elif config.explore.enabled:
            self.explore = ExploreArchive(config.explore.max_cells, config.explore.depth_weight)
            explore = config.explore
            print(f"Go-Explore: {explore.share:.0%} of wing training resets start from the {explore.table_size} most "
                  f"promising of at most {explore.max_cells} cells; evaluation from the door", flush=True)
        # A follower first takes whatever the leader read that this machine lacks -- parents, teachers, the cast's
        # checkpoints, the league -- so it sets up from the same files (async_sync.fetch_shared).
        if self.async_ranks and not leader:
            fetch_shared(config.dist_address, config.rank, Path(config.runs_dir), config.dist_timeout)
        # The style reward before the networks load: a resumed run restores its discriminator with them.
        self.style, self.style_stats, self.realism_reference = self._make_style()
        self._load_or_seed()
        # After the seed and any resume, which bring a parent's goal block positions with its weights: the goal
        # head is masked by this stage's own (stage.json "goals" and the layouts' blocks).
        self.trainer.set_goal_space(self.stage, [layout.name for layout in self.spec.layouts])
        # The hint block's columns (a dungeon's suggested action) are kept out of both networks, seeded or resumed.
        self.trainer.set_hint_space(self.stage, [layout.name for layout in self.spec.layouts])
        # A seed brings the parent's director adapter whole: its slot columns are made blind (DirectorSets). A resumed
        # run's must already be -- their gradient is masked -- and anything else is a checkpoint to stop on, not fix.
        if self.resume_path:
            if not self.trainer.director_columns_clear():
                raise SystemExit(f"{self.resume_path}: the director adapter learned its slot columns; not resuming")
        else:
            self.trainer.clear_director_columns()
        # Every rank carries on from the leader's counters (a learner on another machine resumed nothing), so they
        # stop, evaluate and schedule together.
        self.update, self.env_steps = self.ranks.broadcast((self.update, self.env_steps))
        # Every rank starts as the leader's networks: whatever each seeded or resumed from, one network.
        # The style discriminator too (animus.style): the ranks average its gradients from here on, so they stay one D.
        for module in (self.trainer.actor, self.trainer.critic, self.trainer.value_norm,
                       self.style.disc if self.style is not None else None):
            if module is not None:
                self.ranks.broadcast_module(module)
        if self.async_ranks:
            # Asynchronous ranks trade D with the policy's networks, elastic-averaged like them (async_sync).
            networks = [self.trainer.actor, self.trainer.critic, self.trainer.value_norm,
                        *((self.style.disc,) if self.style is not None else ())]
            if leader:
                self.hub = Hub(config.dist_address, networks, shared_listing(Path(config.runs_dir), self.shared_files))
                self.hub.center_steps = self.env_steps
                self.hub.set(env_steps=self.env_steps, update=self.update)
            else:
                # A follower evaluates nothing and decides nothing: the leader's run is the run.
                self.link = Link(config.dist_address, config.rank, networks, config.mappo.weight_sync_every,
                                 config.dist_timeout)
                control = self.link.hello()
                self.update, self.env_steps = int(control.get("update", 0)), int(control.get("env_steps", 0))
                self.evaluating = False
        self.layout_weights_version = 0
        self.replay_version = 0
        self.shared_version = 0
        self.trainer.sync_rollout()
        self.exploit = self._make_exploit()

        def new_buffer() -> RolloutBuffer:
            return RolloutBuffer(config.rollout_length, spec.num_envs, spec.agents_per_env, spec.obs_dim,
                                 spec.state_dim, spec.num_actions, self.trainer.foresight_outputs,
                                 self.trainer.recurrent_size, bool(self.trainer.goal_count),
                                 self.trainer.slow_goal_size, self.trainer.goal_slots, spec.image_bytes)

        # What the policy carries between decisions (its memory and the goal it pursues), cleared with an episode.
        self.acting = self.trainer.acting_state(spec.num_envs, spec.agents_per_env)

        self.buffer = new_buffer()
        # Self-imitation (mappo.sil_coef): its returns discount by the run's own per-decision gamma, and it keeps the
        # episodes by their outcome score (score_outcome). Its memory bound is said once.
        self.sil_score_column = None
        if self.trainer.sil is not None:
            self.trainer.sil_gamma = float(self.discounts[0])
            names = spec.episode_info_names
            self.sil_score_column = names.index("score_outcome") if "score_outcome" in names else None
            if self.sil_score_column is None:
                print("mappo.sil_coef is on, but this stage reports no score_outcome: nothing is kept to imitate",
                      flush=True)
            else:
                buffer = self.buffer
                row = {name: getattr(buffer, name)[0, 0] for name in ("obs", "mask", "layout", "actions", "state",
                                                                      "rewards")}
                bound = self.trainer.sil.nbytes(config.rollout_length, row)
                print(f"Self-imitation: the {config.mappo.sil_episodes} best episodes by outcome, their last "
                      f"{config.rollout_length} decisions at most, {config.mappo.sil_batch} replayed a minibatch: "
                      f"at most {bound / 2**20:.0f} MiB", flush=True)
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
            "lr_scale", "shaping_scale", "cost_scale", "frozen_layouts", "cast_rows", "cast_fallback_rows", "cast_members", "cast_hardest_win_rate",
            "elapsed_seconds", "update_compute_seconds", "distill_coef", "distill_kl", "distill_rows",
            # Action hints (mappo.hint_coef): the imitation loss, the greedy action's agreement, the sim's weight.
            "hint_loss", "hint_match", "hint_weight", "scripted_share",
            *(f"hint_match_{block}" for block in ("core", "move", "duel", "pack", "party", "support", "crowd", "pet",
                                                  "gauntlet", "companion")),
        ]
        if self.style is not None:
            # The style reward (animus.style): what it paid a decision, the scale it was paid at, and the
            # discriminator's view of both sides; per context the players have.
            columns += ["style_reward", "style_scale", "style_disc_human", "style_disc_bot", "style_gp",
                        "style_disc_loss",
                        *(f"style_reward_{motion.context_name(c)}" for c in self.style.human.context_set)]
        if self.trainer.sil is not None:
            # Self-imitation (mappo.sil_coef): the episodes kept and their scores, what this update took, its terms.
            columns += ["sil_episodes", "sil_best_score", "sil_floor_score", "sil_collected", "sil_policy_loss",
                        "sil_better_share", "sil_value_loss"]
        if self.explore is not None:
            # Go-Explore (explore.enabled): the cells archived and the furthest of them.
            columns += ["explore_cells", "explore_deepest"]
        if config.exploit.enabled:
            # The exploiter (exploit.enabled): whether one plays, its standing against the main, what it costs.
            columns += ["exploit_active", "exploit_joined", "exploit_win_rate", "exploit_episodes",
                        "exploit_update_seconds", "exploit_rows", "exploit_policy_loss"]
        if self.trainer.goal_count:
            # What the goal head is doing: the entropy it is kept at, how often a chosen goal is the one held, and
            # the share of decisions spent under each goal.
            # Whether the goal changes the actions (goal_swap_action_change) and, with hindsight, how much was
            # relabelled.
            columns += ["goal_swap_action_change", "hindsight_loss", "hindsight_rows"]
            columns += ["goal_entropy", "goal_kept_share",
                        *(f"goal_{index}_share" for index in range(self.trainer.goal_kinds)),
                        *(("goal_targeted_share",) if self.trainer.goal_targets > 1 else ())]
        if self.trainer.slow_goal_size:
            # The slow goal loop (Component D) and its goal-level predictions (Component P layer 3): its own
            # losses, and how well it foresees a goal being reached -- the Brier score against always predicting the
            # rollout's rate (lookahead_brier_base) -- and how long it takes.
            columns += ["slow_policy_loss", "slow_value_loss", "slow_approx_kl", "goal_reached_share",
                        "lookahead_loss", "lookahead_brier", "lookahead_brier_base", "lookahead_duration_error",
                        "goal_best_by_lookahead"]
        if self.trainer.foresight_outputs:
            # The foresight (Component P layer 2): its loss, and its observation forecasts' quality -- health 2 s
            # and 5 s on (mean absolute error, as a share of full health), and the goal reached within 4 s (Brier).
            columns += ["foresight_loss", "forecast_health_8_error", "forecast_health_20_error",
                        "forecast_goal_reached_16_brier"]
        self.logger = RunLogger(self.run_dir, columns, append=self.resume_path is not None) if leader else Silent()
        # eval.report's columns, then the stage's headline measures (status.headline) that it does not list, so forge
        # status always has what the stage is read by.
        self.report = tuple(dict.fromkeys((*config.eval.report, *config.status.headline)))
        self.eval_log = EvalLog(self.run_dir, self.logger.tb,
                                realism.columns(self.realism_reference) if self.realism_reference else ()) \
            if leader else Silent()
        # Self-play arenas scored against the baseline as their opponent (eval.opponent_baseline).
        self.opponents = config.eval.baseline if config.eval.opponent_baseline else ""
        self.baselines: dict[tuple[int, int], dict] = {}
        self.best_path = self.run_dir / "best.pt"

        self.progress = (ProgressWriter(self.run_dir, config, spec, resumed_update=self.update,
                                        resumed_env_steps=self.env_steps) if leader else Silent())
        cached_baseline_score = None
        if self.resume_path and (self.run_dir / "eval_baseline.json").exists():
            cached = json.loads((self.run_dir / "eval_baseline.json").read_text())
            # Only a baseline scored on the run's kind of score is shown beside it.
            if cached.get("key", {}).get("score", "") == self.score_kind:
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
        self.shaping_scale_now = self.controller.fade.scale
        self.cost_scale_now = self.controller.costs.scale
        self.frozen = np.zeros(0, dtype=np.int64)
        self.layout_allowed: dict[int, float] = {}
        self.last_layout_stats: dict[str, dict[str, float]] = {}
        self.difficulty_column = names.index("difficulty") if "difficulty" in names else None
        # Whether a training episode was at the top of its class's ladder (convergence.top_rung).
        self.top_rung_column = names.index("at_top_rung") if "at_top_rung" in names else None
        self.apply_holds()

    # ------------------------------------------------------------------ setup

    def _make_style(self) -> tuple[StyleReward | None, dict, dict | None]:
        """The style reward (style.enabled) and the realism reference (style.reference), from their files; the line
        that says what they do. A sim that sends no kinematics (protocol 20) cannot have either."""
        config, spec = self.config, self.spec
        style_config = config.style
        dataset = config.format_path(style_config.dataset) if style_config.dataset else ""
        reference = config.format_path(style_config.reference) if style_config.reference else ""
        if (style_config.enabled or reference) and spec.kinematics_dim != motion.SAMPLE_DIM:
            raise SystemExit(f"style: the sim sends {spec.kinematics_dim} kinematic floats an agent, not "
                             f"{motion.SAMPLE_DIM}: no style reward or realism score without them")
        style = None
        if style_config.enabled:
            if not Path(dataset).is_file():
                raise SystemExit(f"style.dataset: no human windows at {dataset}")
            human = HumanWindows.load(dataset, style_config.window)
            if not human.context_set:
                raise SystemExit(f"style.dataset: {dataset} holds no weighted windows")
            style = StyleReward(style_config, human, spec.num_envs, spec.agents_per_env, config.rollout_length,
                                device=config.resolved_train_device(),
                                ranks=self.ranks if self.ranks.active else None, seed=config.seed + config.rank)
            missing = [motion.context_name(c) for c in range(motion.CONTEXTS) if c not in human.context_set]
            if missing:
                print(f"Style: no human motion in {', '.join(missing)}; the seats' is neither paid nor trained on "
                      f"there", flush=True)
        reference_data = None
        if reference:
            if not Path(reference).is_file():
                raise SystemExit(f"style.reference: no human reference at {reference}")
            reference_data = realism.load_reference(reference)
        if self.ranks.leader:
            print(f"{config.run_name}: {style_line(style_config, style.human if style else None, config.costs.enabled)}"
                  + (f"; realism scored against {reference}" if reference_data else ""), flush=True)
        return style, {}, reference_data

    def _load_or_seed(self) -> None:
        """Resume the run's latest.pt, or seed the fresh networks from the stage this one extends and a merge stage's
        further parents; either way the parents teach a distilled run (self.distiller)."""
        config, spec = self.config, self.spec
        self.cast: Cast | None = None
        self.last_snapshot_env_steps = 0
        # Every checkpoint this setup reads from the runs directory, for the followers (async_sync.Hub).
        self.shared_files: list[Path] = []
        if self.resume_path:
            checkpoint = torch.load(self.resume_path, map_location="cpu", weights_only=False)
            if mismatch := resume_mismatch(checkpoint.get("spec", {}), asdict(spec)):
                raise SystemExit(f"cannot resume {config.run_name}: the scenario's {', '.join(mismatch)} changed since "
                                 f"{self.resume_path} was saved; start it fresh instead")
            # The same shapes can still be a different layout: a block re-laid in place (its revision), or blocks
            # that traded places. Its weights would read the new columns as the old ones.
            if changes := layout_changes(checkpoint.get("stage"), self.stage):
                raise SystemExit(f"cannot resume {config.run_name}: the layouts changed since {self.resume_path} was "
                                 f"saved -- {' | '.join(changes)}; start it fresh (it seeds from the stage before)")
            self.trainer.load_state_dict(checkpoint["trainer"])
            if self.style is not None and not self.style.load_state_dict(checkpoint.get("style")):
                print(f"Resuming {config.run_name}: the checkpoint has no style discriminator of this shape; it "
                      f"starts fresh", flush=True)
            if self.explore is not None and checkpoint.get("explore"):
                self.explore.load_state_dict(checkpoint["explore"])
            self.exploit_resume = checkpoint.get("exploit")
            self.update = int(checkpoint.get("update", 0))
            self.env_steps = int(checkpoint.get("env_steps", 0))
            # The convergence test and the best evaluation carry on where the run stopped -- unless they were scored
            # on another kind of score, which no score from here on could be compared with.
            if dropped := restore_evaluation_state(self.tracker, self.controller, checkpoint, self.score_kind):
                print(f"Resuming {config.run_name}: {dropped}", flush=True)
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
        # Restricted stages the chain stepped over on its way down (the stealth drill), nearest first: their own
        # layouts are merged in over the seed below, so what those classes learned there is not a dead end.
        restricted: list[tuple[Path, dict]] = []
        base_path, base = self.seed_candidate(candidates, prefer, spec, config.init_from == "auto", restricted)
        merge_paths = []
        for candidate in config.resolved_merge_from(self.stage):
            if path := init_from_checkpoint(candidate, prefer):
                merge_paths.append(path)
            else:
                print(f"Merged stage checkpoint {candidate} does not exist: nothing is seeded or taught from it",
                      flush=True)
        merged = [load_parent(path) for path in merge_paths]

        if not self.resume_path and base is not None:
            seeded = seed_trainer(self.trainer, base, spec, self.stage,
                                  source=f"{spec.scenario} from {base_path}")
            print(f"Seeded the networks from {base_path}: trunk and {len(seeded)} of {len(spec.layouts)} layouts",
                  flush=True)
            for layout, blocks in (seed_merges(self.trainer, merged, spec, self.stage, base) if merged else {}).items():
                print(f"  {layout}: {', '.join(blocks)} from the merged stages", flush=True)
            # Farthest first, so the nearest restricted stage's layouts are the ones that stand.
            for path, checkpoint in reversed(restricted):
                overlaid = seed_trainer(self.trainer, checkpoint, spec, self.stage, overlay=True,
                                        source=f"{spec.scenario} from {path}")
                print(f"  {', '.join(overlaid)}: layouts from {path} (a restricted stage), over the seed; the trunk "
                      f"stays the seed's", flush=True)
        elif not self.resume_path and candidates:
            print(f"None of {', '.join(candidates)} to seed from; starting from scratch", flush=True)

        self.distiller = make_distiller(config, spec, self.stage, [p for p in (base, *merged) if p is not None],
                                        self.trainer.train_device)
        # A restricted stage merged in above teaches its own classes through the stage's first stretch, on every
        # arena (a teacher only teaches the layouts it has, so the other classes are untouched): its layouts were
        # trained against another trunk, and this pulls them back to what they did while they settle on this one.
        # distill.coef and half_life_env_steps set how hard and how long. Where the stage distils already, its own
        # teachers keep their arenas.
        if restricted and not self.resume_path and self.distiller is None:
            nearest_path, nearest = restricted[0]
            teacher = build_teacher(nearest, spec, self.stage, self.trainer.train_device)
            self.distiller = Distiller(self.stage, {arena: teacher for arena in arena_names(self.stage)})
            print(f"{', '.join(spec.layouts[i].name for i in teacher.layouts)} taught by {nearest_path} on every "
                  f"arena (a restricted stage merged forward)", flush=True)
        # The parents and named teachers, with what decides how each is read: its stage.json (load_parent's block
        # positions).
        named = [init_from_checkpoint(candidate) for candidate in config.named_teachers().values()]
        for path in (base_path, *merge_paths, *named, *(p for p, _ in restricted)):
            if path is not None:
                self.shared_files += [Path(path), Path(path).parent / STAGE_FILE]

        # Frozen checkpoints in the seats a script used to play (animus.cast): the far side of self-play arenas,
        # from the parent the networks seeded from and this run's own league, and any agent the stage declares.
        cast_config = copy.copy(config.cast)
        cast_config.agents = config.cast.resolved_agents(config.runs_dir, config.run_name)
        # Every agent the stage declares cast has to be played by a checkpoint: a declared row with none would be
        # played -- and trained -- by the live policy, unpaid (the follow stage's leader standing still or wandering).
        unplayed = sorted({entry.get("name", "") for entry in (self.stage or {}).get("cast") or []
                           if "agent" in entry} - set(cast_config.agents))
        if unplayed:
            raise ValueError(f"the stage declares cast agents {unplayed} but cast.agents names no checkpoint for "
                             f"them; set cast.agents.<name> in the stage's config")
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
        # The cast's checkpoints: its agents, the parent and a named opponent, and the league as it stands. The league's
        # later snapshots are the leader's; a follower plays the members it fetched at startup.
        self.shared_files += [Path(path) for path in cast_config.agents.values()]
        for named_path in (cast_config.parent, cast_config.opponents):
            if named_path and named_path not in ("auto", LEAGUE):
                self.shared_files.append(Path(named_path))
        league = self.run_dir / LEAGUE
        if league.is_dir():
            self.shared_files += sorted(league.iterdir())
        # The human data, for a follower on another machine (served only from under the runs directory).
        for path in (config.style.dataset, config.style.reference):
            if path:
                self.shared_files.append(Path(config.format_path(path)))

    @staticmethod
    def seed_candidate(candidates: list[str], prefer: str, spec, auto: bool,
                       restricted: list | None = None) -> tuple[Path | None, dict | None]:
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
                          f"trying the next stage down the chain, and merging its layouts in after", flush=True)
                    if restricted is not None:
                        restricted.append((path, checkpoint))
                    continue
            return path, checkpoint
        return None, None

    # ------------------------------------------------------------------ checkpoints

    def _make_exploit(self) -> Exploit | None:
        """The run's exploiter slot (exploit.enabled): only with a league on the far side (cast.opponents: league)
        and learners in step (an async follower decides nothing). Its resumed state, if the checkpoint has one."""
        config, spec = self.config, self.spec
        if not config.exploit.enabled:
            return None
        if self.cast is None or self.cast.pool is None or config.cast.opponents != LEAGUE or self.async_ranks:
            print("exploit.enabled, but this run has no league on the far side (cast.opponents: league) or its "
                  "learners are not in step: no exploiter", flush=True)
            return None
        names = [layout.name for layout in spec.layouts]

        def prepare(trainer) -> None:
            # As the main's: the stage's goal and hint spaces, blind director columns, every rank the leader's.
            trainer.set_goal_space(self.stage, names)
            trainer.set_hint_space(self.stage, names)
            trainer.clear_director_columns()
            for module in (trainer.actor, trainer.critic, trainer.value_norm):
                if module is not None:
                    self.ranks.broadcast_module(module)
            trainer.sync_rollout()

        exploit = Exploit(config.exploit, self.make_trainer, prepare, spec.num_envs, spec.agents_per_env)
        if getattr(self, "exploit_resume", None):
            exploit.load_state_dict(self.exploit_resume)
            self.cast.exploit_share = config.exploit.share if exploit.active else 0.0
        share = config.cast.opponent_share
        print(f"Far side of the self-play arenas while an exploiter plays: {share * (1 - config.exploit.share):.0%} "
              f"league, {share * config.exploit.share:.0%} exploiter, {1 - share:.0%} the live policy", flush=True)
        return exploit

    def exploit_step(self) -> None:
        """Every update: the exploiter's results from every rank to the leader, who decides whether it joins,
        retires, or (with none playing) whether the next starts and from which snapshot; every rank does the same."""
        exploit = self.exploit
        if exploit is None:
            return
        gathered = self.ranks.gather(list(exploit.results))
        exploit.results.clear()
        decision = None
        if self.ranks.leader:
            results = [won for rows in gathered for won in rows]
            if exploit.active:
                for won in results:
                    exploit.record.observe(won)
                decision = exploit.record.verdict(self.env_steps)
                if decision is None and self.update % max(1, self.config.exploit.log_every) == 0:
                    print(f"Exploiter from {Path(exploit.record.seed).name}: win rate {exploit.record.rate:.0%} over "
                          f"{len(exploit.record.recent)} of {exploit.record.episodes} episodes against the main, "
                          f"{self.env_steps - exploit.record.started_env_steps} of "
                          f"{exploit.record.budget_env_steps} env steps", flush=True)
            elif self.env_steps - exploit.ended_env_steps >= self.config.exploit.every_env_steps:
                # Seeded from the league's newest of the main's own snapshots: a learner, never a script.
                snapshots = [member for member in self.cast.pool.active() if not member.exploiter]
                if snapshots:
                    decision = str(max(snapshots, key=lambda member: member.order).path)
            if decision == JOIN:
                path = self.run_dir / LEAGUE_DIR / exploiter_name(exploit.joined + 1)
                path.parent.mkdir(parents=True, exist_ok=True)
                save_checkpoint(path, exploit.trainer, self.config, self.spec, self.update, self.env_steps,
                                {"stage": self.stage, "exploiter": exploit.record.state_dict()})
        decision = self.ranks.broadcast(decision)
        if decision is None:
            return
        if decision in (JOIN, "retire"):
            exploit.end(decision, self.env_steps)
            self.cast.exploit_share = 0.0
            if decision == JOIN:
                self.cast.pool.reload()
                self.cast.pool.write()
            return
        exploit.launch(Path(decision), load_parent(Path(decision)), self.spec, self.stage, self.env_steps)
        self.cast.exploit_share = self.config.exploit.share

    def _checkpoint_extra(self) -> dict:
        # The stage (its block positions) travels with the checkpoint, for seeding the stages that extend it.
        return {"convergence": self.tracker.state_dict(), "controller": self.controller.state_dict(),
                "stage": self.stage, "score_kind": self.score_kind,
                **({"explore": self.explore.state_dict()} if self.explore is not None else {}),
                **({"style": self.style.state_dict()} if getattr(self, "style", None) is not None else {}),
                **({"exploit": self.exploit.state_dict()} if getattr(self, "exploit", None) is not None else {})}

    def _save(self, path: Path) -> None:
        self.drain_update()
        if not self.ranks.leader:
            return  # the leader's networks are every rank's: it alone writes them
        save_checkpoint(path, self.trainer, self.config, self.spec, self.update, self.env_steps,
                        self._checkpoint_extra())

    def maybe_checkpoint(self) -> None:
        every_steps = self.config.checkpoint_env_steps
        due = self.update % self.config.checkpoint_every == 0 or (
            every_steps > 0 and self.env_steps - getattr(self, "checkpointed_env_steps", 0) >= every_steps)
        if due and self.ranks.leader:
            self.checkpointed_env_steps = self.env_steps
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
            mask = host(step.mask) if forbidden is None else np.logical_and(host(step.mask), ~forbidden[step.layout])
            image = getattr(step, "image", None)
            actions = self.trainer.act(step.obs, mask, step.layout, deterministic, acting, image)[0]
            return (actions, self.trainer.wire_goals(acting.goal)) if acting.goal is not None else actions

        return choose

    def _evaluate_share(self, choose_actions, episodes: int, seed: int, **options) -> EvalResult | None:
        """run_evaluation on this rank's run of the seeds; on the leader, every rank's results merged (None on the
        others). Alone, the whole evaluation."""
        first, count = weighted_share(episodes, self.rank_envs, self.ranks.rank)
        result, self.step = run_evaluation(self.env, self.spec, choose_actions, count, seed, first_seed=first,
                                           any_playing=self.ranks.any if self.ranks.active else None,
                                           spec_names=self.spec_names,
                                           score_column=self.config.eval.score_column(), **options)
        parts = self.ranks.gather(result)
        return EvalResult.merged(parts) if self.ranks.leader else None

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
        key = baseline_cache_key(config.eval.baseline, seed, episodes, self.opponents, self.arena_names,
                                 (self.stage or {}).get("tuning"), self.score_kind, self.controller.fade.scale)
        cached = json.loads(baseline_path.read_text()) if baseline_path.exists() else None
        # The leader's cache decides for every rank: they all play the baseline, or none does.
        fresh = self.ranks.broadcast(not (cached and cached.get("key") == key))
        summary = None
        if not fresh:
            summary = cached["summary"] if self.ranks.leader else None
        else:
            result = self._evaluate_share(self.learner_actions(), episodes, seed, baseline=config.eval.baseline,
                                          opponents=self.opponents, arenas=self.arena_names,
                                          action_names=self.action_names)
            if self.ranks.leader:
                summary = result.summary(self.report, self.phases)
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
        joined = self.ranks.leader and league_snapshot(self.run_dir, latest, f"step_{self.env_steps}") is not None
        # The leader writes the league; every rank plays from it.
        if self.ranks.broadcast(joined):
            self.cast.pool.reload()
            if self.ranks.leader:
                self.cast.pool.write()
                self.share_league()
                print(f"League: latest.pt at {self.env_steps} env steps joined ({len(self.cast.pool.active())} "
                      f"members)", flush=True)

    def share_league(self) -> None:
        """Offer the league's members to the followers (async_sync.Hub.share); nothing without a hub."""
        league = self.run_dir / LEAGUE
        if self.hub is not None and league.is_dir():
            self.hub.share(shared_listing(Path(self.config.runs_dir), sorted(league.iterdir())))

    def evaluate(self, final: bool = False) -> None:
        """Score the networks on the seeds (and the baseline once per run); the next training STEP becomes current.
        With data-parallel learners every rank plays its share of the seeds and the leader scores them all. `final`:
        the stage's last evaluation, which always plays the held-out arenas (eval.heldout_every)."""
        self.drain_update()
        config, tracker, controller = self.config, self.tracker, self.controller
        leader = self.ranks.leader
        controller.baseline_summary = self.baseline_for(config.eval.seed, config.eval.episodes)
        baseline_summary = controller.baseline_summary

        self.progress.write("evaluating", self.update, self.env_steps)
        result = self._evaluate_share(self.learner_actions(), config.eval.episodes, config.eval.seed,
                                      opponents=self.opponents, arenas=self.arena_names,
                                      action_names=self.action_names, trace_episodes=config.eval.trace_episodes,
                                      collect_motion=self.spec.kinematics_dim == motion.SAMPLE_DIM)
        if self.cast is not None:
            self._reset_far_side()
        self.last_eval_env_steps = self.env_steps

        summary, sampled, joined, heldout = None, False, False, False
        if leader:
            summary = result.summary(self.report, self.phases)
            if self.cast is not None:
                controller.observe_league(self.cast.league_stats([layout.name for layout in self.spec.layouts]))
            improved = controller.observe(summary, self.env_steps)
            self.score_motion(result, summary)
            self.eval_log.write(self.update, self.env_steps, result, summary, tracker)
            self.progress.evaluated(self.env_steps, result.score,
                                    baseline_summary["score"] if baseline_summary else None, tracker, controller,
                                    summary=summary)
            self.progress.write("training", self.update, self.env_steps)

            against = f", baseline {baseline_summary['score']:.4g}" if baseline_summary else ""
            played = summary.get("return")
            print(f"Eval at {self.env_steps} env steps: score {result.score:.4g} +/- {result.stderr:.2g} "
                  f"(best {tracker.best:.4g}, {tracker.evals_since_best} evals since, margin {tracker.last_margin:.2g})"
                  f"{against}; return {played if played is None else format(played, '.4g')} at shaping "
                  f"x{self.shaping_scale_now:g}, noise priced x{self.cost_scale_now:g}; "
                  f"{result.episodes} episodes in {result.seconds:.0f} s"
                  f" [learner/baseline]\n{format_summary(summary, baseline_summary, self.report)}", flush=True)

            if controller.costs_message:
                print(f"{config.run_name}: {controller.costs_message}", flush=True)
            if controller.fade_message:
                print(f"{config.run_name}: {controller.fade_message}", flush=True)

            if improved:
                self._save(self.best_path)
                if self.cast is not None and self.cast.pool is not None and config.cast.opponents == LEAGUE:
                    if league_snapshot(self.run_dir, self.best_path, f"best_{self.env_steps}") is not None:
                        self.cast.pool.reload()
                        self.share_league()
                        joined = True
            if self.cast is not None and self.cast.pool is not None:
                self.cast.pool.write()

            sampled_every = config.eval.sampled_every
            sampled = sampled_every > 0 and len(tracker.history) % sampled_every == 0
            heldout = bool(self.heldout) and heldout_due(len(tracker.history), config.eval.heldout_every, final,
                                                          improved)

        # What the leader decided, carried out on every rank.
        sampled, joined, heldout = self.ranks.broadcast((sampled, joined, heldout))
        if joined and not leader:
            self.cast.pool.reload()
        if sampled:
            self.evaluate_sampled(summary)
        self.heldout_current = heldout
        if heldout:
            self.evaluate_heldout()

        self.apply_holds()
        if leader:
            self.send_layout_weights(summary, baseline_summary)
            self.send_replay(result)

    def score_motion(self, result: EvalResult, summary: dict) -> None:
        """The evaluation's motion (protocol 20 kinematics of the scored seats): its windows to eval_motion.npz for the
        offline realism report, and with style.reference the realism columns into `summary` -- per context the
        distance from the players' motion (animus.human.realism), and the discriminator's mean output when there is
        one. Read after the controller has seen the summary: realism informs, it decides nothing yet."""
        if not result.motion_tracks:
            return
        style = self.config.style
        feats, contexts, per_track, per_context = realism.tracks_features(result.motion_tracks)
        windows, context, weight = realism.motion_windows(per_track, per_context, style.window, style.eval_windows,
                                                          np.random.default_rng(self.update))
        realism.write_motion(self.run_dir / "eval_motion.npz", windows, context, weight, {
            "source": "eval", "run": self.config.run_name, "scenario": self.spec.scenario, "update": self.update,
            "env_steps": self.env_steps, "tracks": len(result.motion_tracks), "steps": int(len(feats)),
            "step_seconds": self.spec.decision_ms / 1000.0, "window": style.window})
        line = []
        if self.realism_reference is not None:
            scored = realism.score(feats, contexts, self.realism_reference)
            summary.update({name: value for name, value in scored.items() if name.startswith("realism_emd")})
            summary["realism"] = {name: scored[name] for name in ("features", "steps", "unscored")}
            line.append(f"realism EMD {scored['realism_emd']:.3g} over {len(feats)} steps"
                        + (f" ({', '.join(scored['unscored'])} unscored: no human motion)" if scored["unscored"]
                           else ""))
        if self.style is not None:
            disc = self.style.disc_mean(windows, context)
            if disc is not None:
                summary["realism_disc"] = disc
                line.append(f"discriminator {disc:.3g} (+1 a player, -1 a bot)")
        if line:
            print(f"Motion: {'; '.join(line)}", flush=True)

    def apply_holds(self, converged: list[str] | None = None) -> None:
        """Freeze the classes that have converged (animus.stage) and keep them out of the rollout's samples. An
        asynchronous follower is handed the leader's `converged`."""
        if converged is None:
            converged = self.ranks.broadcast(sorted(self.controller.converged_layouts()))
            if self.hub is not None:
                self.hub.set(converged=sorted(converged))
        converged = set(converged)
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
        result = self._evaluate_share(self._acting(False), config.eval.episodes, config.eval.seed,
                                      opponents=self.opponents, arenas=self.arena_names,
                                      action_names=self.action_names)
        if self.cast is not None:
            self._reset_far_side()
        if not self.ranks.leader:
            return
        result.policy = "learner_sampled"
        summary = result.summary(self.report, self.phases)
        fields = [name for name in ("score", "clean_kill", "killed", "died", "timed_out", "arrived")
                  if name in summary]
        # The gap, sampled minus argmax, kept with the sampled row of eval.jsonl: a wide one says the gated policy is
        # not the one that trained, which is what mappo.entropy_final_fraction is there to close.
        summary["argmax_gap"] = {name: float(summary[name]) - float(argmax[name])
                                 for name in fields if isinstance(argmax.get(name), (int, float))}
        self.eval_log.write(self.update, self.env_steps, result, summary, self.tracker)
        print("Sampled vs argmax actions on the evaluation seeds: " + ", ".join(
            f"{name} {summary[name]:.4g} / {argmax.get(name, float('nan')):.4g}" for name in fields), flush=True)

    def evaluate_heldout(self) -> None:
        """eval.heldout: each held-out arena on its own seeds (eval.seed + HELDOUT_SEED_OFFSET), every rank playing
        its share, reported as policy heldout_<arena> in eval.csv and eval.jsonl. A reading only: neither the tracker,
        the controller nor the league sees it."""
        for name, (pin, episodes) in self.heldout.items():
            result = self._evaluate_share(self.learner_actions(), episodes, self.config.eval.seed + HELDOUT_SEED_OFFSET,
                                          arenas=self.arena_names, action_names=self.action_names, arena=pin)
            if self.cast is not None:
                self._reset_far_side()
            if not self.ranks.leader:
                continue
            result.policy = f"heldout_{name}"
            summary = result.summary(self.report, self.phases)
            self.eval_log.write(self.update, self.env_steps, result, summary, self.tracker)
            shown = ", ".join(f"{column} {summary[column]:.3g}" for column in self.report
                              if isinstance(summary.get(column), (int, float)))
            print(f"Held out {name}: score {result.score:.4g} +/- {result.stderr:.2g} over {result.episodes} "
                  f"episodes in {result.seconds:.0f} s" + (f"; {shown}" if shown else ""), flush=True)

    def send_replay(self, result: EvalResult) -> None:
        """Send the sim the seeds this evaluation lost, for training resets to rebuild (protocol REPLAY)."""
        sampling = self.config.layout_sampling
        if not sampling.enabled or sampling.replay_fraction <= 0.0 or not sampling.metric:
            return

        seeds = result.failed_seeds(sampling.metric)
        self.env.set_replay(self.config.eval.seed, sampling.replay_fraction, seeds)
        if self.hub is not None:
            self.replay_version += 1
            self.hub.set(replay=(self.replay_version, (self.config.eval.seed, sampling.replay_fraction, seeds)))
        print(f"Replaying {len(seeds)} lost evaluation episodes in {sampling.replay_fraction:.0%} of training resets",
              flush=True)

    def send_layout_weights(self, summary: dict, baseline: dict | None) -> None:
        """Weight the training episodes toward the (class, build) pairs furthest below their baseline, or with no
        baseline the ones scoring lowest, and short of `layout_sampling.metric` (WEIGHTS).

        The wire vector is one weight per pair, layout-major in the spec's layout order and spec-minor, MAX_SPECS
        wide, so a class with fewer builds than that still has the slots -- never drawn, and left at the even 1.0.
        Per pair and not per layout because a layout is a whole class: weighting a paladin by its average would
        send more tanking episodes to fix its healing. Per build and not per role because two builds of one role
        are not equally hard to win with, which is the case a feral druid is.
        """
        sampling = self.config.layout_sampling
        weights = {}
        if sampling.enabled and summary.get("castings"):
            weights = casting_weights(summary, baseline, sampling.strength, sampling.max_ratio, sampling.metric,
                                      self.casting_roles, sampling.role_metrics)
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
        if self.hub is not None:
            self.layout_weights_version += 1
            self.hub.set(layout_weights=(self.layout_weights_version, vector))
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
        if not self.ranks.leader:
            return True
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

    def _reset_far_side(self) -> None:
        """Every env starts afresh (after an evaluation): the cast's members redrawn and memories gone, and the
        exploiter's."""
        self.cast.reset_all()
        if self.exploit is not None and self.exploit.active:
            self.exploit.clear(np.ones(self.spec.num_envs, dtype=bool))

    def finish_update(self) -> dict[str, float] | None:
        """Wait for an overlapped update to finish and hand its weights to the rollout networks. None if none ran."""
        if self.pending_update is None:
            return None

        pending, self.pending_update = self.pending_update, None
        stats = pending.result()  # an update that raised re-raises here, on the training thread
        blas.save()
        self.at_safe_point()
        self.trainer.sync_rollout()
        return stats

    def at_safe_point(self) -> None:
        """Where no update is running on the networks: an asynchronous rank trades them here (animus.async_sync)."""
        if self.hub is not None:
            self.hub.at_safe_point(self)
        elif self.link is not None:
            self.link.at_safe_point(self)

    def apply_control(self, control: dict) -> None:
        """A follower carries out what the leader decided (animus.async_sync's reply)."""
        self.env_steps = max(self.env_steps, int(control.get("env_steps", 0)))
        converged = control.get("converged")
        if converged is not None:
            self.apply_holds(converged)
        if (weights := control.get("layout_weights")) and weights[0] != self.layout_weights_version:
            self.layout_weights_version = weights[0]
            self.env.set_layout_weights(weights[1])
        if (replay := control.get("replay")) and replay[0] != self.replay_version:
            self.replay_version = replay[0]
            self.env.set_replay(*replay[1])
        # A new league member on the leader: fetch it, and play it. Without this a follower played the league it
        # fetched at startup for the whole stage, while the leader's grew every few million steps.
        if (shared := int(control.get("shared", 0))) != self.shared_version:
            self.shared_version = shared
            fetch_shared(self.config.dist_address, self.config.rank, Path(self.config.runs_dir),
                         self.config.dist_timeout)
            if self.cast is not None and self.cast.pool is not None and (joined := self.cast.pool.reload()):
                print(f"League: {joined} member(s) from the leader joined ({len(self.cast.pool.active())} active)",
                      flush=True)

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
        exploit = self.exploit if self.exploit is not None and self.exploit.active else None
        if exploit is not None:
            exploit.view(buffer).reset()
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
        # Workers that dropped out and are back rejoin here, between rollouts, where no decision is half answered:
        # their envs start new episodes, remembering nothing.
        if isinstance(self.env, ClusterEnv):
            for offset, fresh in self.env.rejoin():
                count = fresh.done.shape[0]
                parts = [protocol.rows_of(self.step, 0, offset), dataclasses.replace(fresh, env_begin=offset),
                         protocol.rows_of(self.step, offset + count, envs - offset - count)]
                self.step = protocol.join_steps([part for part in parts if part.done.shape[0]])
                cleared = np.zeros(envs, dtype=bool)
                cleared[offset:offset + count] = True
                self.acting.clear(cleared)
                if self.cast is not None:
                    self.cast.clear(cleared)
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

        if self.style is not None:
            self.style.begin(self.step)
        decision = self._act_on_rows_of(self.step, groups, send)
        while True:
            buffer.add_decision(*decision.recorded())
            if exploit is not None:
                exploit.record_decision(buffer, decision.layout, decision.actions)
            last = buffer.cursor + 1 >= buffer.steps
            outcome = RolloutOutcome(envs, agents, trainer.foresight_outputs)
            following = None if last else DecisionRows(envs, agents, buffer, buffer.cursor + 1)
            parts = []
            for begin, count in groups:
                part = receive(begin, count)
                parts.append(part)
                rows = slice(begin, begin + count)
                if self.style is not None:
                    self.style.record(buffer.cursor, rows, part)
                value_ended = self._take_outcome_of(part, rows, decision, outcome)
                if following is not None:
                    self._act_on_rows(part, rows, following, send)
                if value_ended is not None:
                    value_ended()
            # The whole decision's STEP is only read after the rollout (the bootstrap, the next rollout's first
            # decision): joined once there, not copied every decision.
            if following is None:
                self.step = protocol.join_steps(parts)
            buffer.add_outcome(outcome.reward, outcome.done, outcome.terminated, outcome.final_values,
                               outcome.final_foresight)
            if exploit is not None:
                exploit.record_outcome(buffer, outcome)
            if following is None:
                break
            decision = following

        # The last decision's inputs were queued into the buffer on the rollout stream with nothing after them there
        # to wait for them (_act_on_rows): finished before the buffer is valued and handed to the update.
        if trainer.rollout_stream is not None:
            trainer.rollout_stream.synchronize()
        rollout_seconds = time.perf_counter() - started
        self.add_style(buffer)
        buffer.finish(trainer.value(self.step.state, self.step.obs, self.step.layout, self.acting.goal,
                                    self.acting.critic_memory, getattr(self.step, "image", None)),
                      *self.discounts,
                      last_foresight=trainer.foresight_of(self.step.obs, self.step.layout, self.acting.memory,
                                                          getattr(self.step, "image", None)),
                      foresight_gammas=self.foresight_discounts,
                      time_scale_decisions=self.foresight_time_decisions,
                      slow_layout=self.slow_layout,
                      slow_gamma=self.config.mappo.slow_gamma,
                      slow_gae_lambda=self.config.mappo.slow_gae_lambda,
                      slow_goal=(self.config.mappo.slow_goal_gamma, self.config.mappo.slow_goal_lambda)
                      if trainer.slow_goal_size else None,
                      obs_targets=trainer.foresight_obs_columns())
        # The exploiter's rollout over the same decisions, valued by its own critic and updated now, before the
        # next rollout writes into the buffer it shares (a rank with none of its rows still joins every all-reduce).
        self.exploit_stats = {}
        if exploit is not None:
            def finish(view, exploiter) -> None:
                view.finish(exploiter.value(self.step.state, self.step.obs, self.step.layout, exploit.acting.goal,
                                            exploit.acting.critic_memory, getattr(self.step, "image", None)),
                            *self.discounts,
                            last_foresight=exploiter.foresight_of(self.step.obs, self.step.layout,
                                                                  exploit.acting.memory,
                                                                  getattr(self.step, "image", None)),
                            foresight_gammas=self.foresight_discounts,
                            time_scale_decisions=self.foresight_time_decisions,
                            slow_layout=self.slow_layout,
                            slow_gamma=self.config.mappo.slow_gamma,
                            slow_gae_lambda=self.config.mappo.slow_gae_lambda,
                            slow_goal=(self.config.mappo.slow_goal_gamma, self.config.mappo.slow_goal_lambda)
                            if exploiter.slow_goal_size else None,
                            obs_targets=exploiter.foresight_obs_columns())

            # Data-parallel ranks all-reduce on one group: an overlapped main update doing so on its worker thread
            # while this one does on this thread could pair one rank's main collective with another's exploiter
            # one. With other ranks the main's pending update is joined first (its stats carried, as finish_update's
            # are below); alone, nothing is shared and the two overlap.
            if self.ranks.active and self.pending_update is not None:
                if (joined := self.finish_update()) is not None:
                    self.carried_stats = joined
            self.exploit_stats = exploit.update(buffer, finish)
        # Read before the buffers swap below: log_update runs on the rollout that has just been collected.
        self.rollout_reward = buffer.mean_reward()
        self.rollout_allowed_actions, self.layout_allowed = self.allowed_actions(buffer)
        # The leader's controller decides, for every rank.
        entropy_coef = self.controller.entropy_coef(self.env_steps)
        lr_scale = self.controller.lr_scale(self.env_steps)
        shaping_scale = self.controller.fade.scale
        cost_scale = self.controller.costs.scale
        if self.link is not None:
            entropy_coef = self.link.control.get("entropy_coef", entropy_coef)
            lr_scale = self.link.control.get("lr_scale", lr_scale)
            shaping_scale = self.link.control.get("shaping_scale", shaping_scale)
            cost_scale = self.link.control.get("cost_scale", cost_scale)
        elif self.hub is not None:
            self.hub.set(entropy_coef=entropy_coef, lr_scale=lr_scale, shaping_scale=shaping_scale,
                         cost_scale=cost_scale)
        self.shaping_scale_now = self.ranks.broadcast(shaping_scale)
        self.cost_scale_now = self.ranks.broadcast(cost_scale)
        trainer.entropy_coef = self.ranks.broadcast(entropy_coef)
        # The goal head's share of it falls on its own schedule, the same on every rank (it is a function of the
        # steps alone).
        trainer.goal_entropy_factor = self.config.mappo.goal_entropy_scale * schedule(
            self.config.mappo.goal_entropy_final_fraction, self.env_steps, self.config.total_env_steps)
        # With an overlapped update this applies to the update submitted below: a rollout's worth late, which a
        # schedule over hundreds of millions of steps does not notice. The scale is the controller's: held at full
        # until the score first plateaus, so the KL it reads is the policy's and not the schedule's.
        self.lr_scale_now = self.ranks.broadcast(lr_scale)
        trainer.set_learning_rate_scale(self.lr_scale_now)
        if self.distiller is not None:
            self.distiller.coef = self.config.distill.coef_at(self.env_steps)

        self.update += 1
        self.env_steps += self.config.rollout_length * self.run_envs * agents
        if self.link is not None:
            self.link.steps_since += self.config.rollout_length * self.run_envs * agents
        self.maybe_league_snapshot()
        # How far through its budget the stage is, for the arenas whose weights change over it (WeightFinal), and
        # the shaping and cost ladders' scales. Sent while the sim waits for this rollout's last ACT, as WEIGHTS is.
        if hasattr(self.env, "set_stage_progress"):
            total = self.config.total_env_steps
            self.env.set_stage_progress(self.env_steps / total if total > 0 else 0.0, self.shaping_scale_now,
                                        self.cost_scale_now)

        if self.updater is None:
            stats = trainer.update(buffer, self.distiller)
            blas.save()
            self.at_safe_point()
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

    def style_scale(self) -> float:
        """What the style reward is paid times: the cost ladder's scale in force (style.ladder), else 1."""
        return float(self.cost_scale_now) if self.config.style.ladder else 1.0

    def add_style(self, buffer: RolloutBuffer) -> None:
        """The style reward of the rollout just collected, into its rewards before its advantages are taken
        (animus.style), and its discriminator trained on it. The sim's outcome score never sees it."""
        if self.style is None:
            return
        reward, stats = self.style.finish(buffer.dones, buffer.valid, self.step)
        scale = self.style_scale()
        buffer.rewards += np.float32(self.config.style.coef * scale) * reward
        self.style_stats = {**stats, "style_scale": scale}

    def _act_on_rows_of(self, step: protocol.Step, groups: list[tuple[int, int]], send) -> DecisionRows:
        """Act on every group of a decision already received whole (the one a rollout starts from)."""
        decision = DecisionRows(self.spec.num_envs, self.spec.agents_per_env, self.buffer, self.buffer.cursor)
        for begin, count in groups:
            rows = slice(begin, begin + count)
            self._act_on_rows(protocol.rows_of(step, begin, count), rows, decision, send)
        return decision

    def _act_on_rows(self, part: protocol.Step, rows: slice, decision: DecisionRows, send) -> None:
        """The policy's decision for envs `rows` (`part` is their STEP), recorded into `decision` and sent."""
        trainer = self.trainer
        # take() copies these envs' state; acting replaces its memories rather than writing into them, so the copies
        # it was handed are the memories this decision started from, which the rollout buffer records.
        acting = self.acting.take(rows)
        memory, critic_memory = acting.memory, acting.critic_memory
        # A non-finite observation reaches the networks as a non-finite logit and comes back out of
        # torch.multinomial as "probability tensor contains either `inf`, `nan` or element < 0" -- an error
        # that names neither the observation nor the seat it came from, several layers away from whichever
        # block wrote it. Caught here it names both, which is the difference between a fix and a hunt.
        # A device view is checked on the rollout's own stream: on the default one it queued behind an overlapped
        # update's kernels and the decision waited for the whole update.
        if isinstance(part.obs, torch.Tensor):
            with torch.cuda.stream(trainer.rollout_stream) if trainer.rollout_stream is not None else nullcontext():
                finite = bool(part.obs.isfinite().all())
        else:
            finite = np.isfinite(part.obs).all()
        if not finite:
            obs = host(part.obs)
            bad = np.argwhere(~np.isfinite(obs))
            where = ", ".join(f"env {int(e) + rows.start} agent {int(a)} obs[{int(i)}]={obs[e, a, i]}"
                              for e, a, i in bad[:8])
            raise RuntimeError(
                f"{len(bad)} non-finite observation(s) from the sim at step {self.env_steps}: {where}"
                + ("" if len(bad) <= 8 else f" (and {len(bad) - 8} more)"))

        actions, log_probs, values, foresight, goals, chosen = trainer.act_and_value(
            part.obs, part.mask, part.layout, part.state, state=acting, image=getattr(part, "image", None))
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
                # The exploiter's episodes: the far side is its to play (animus.exploit); the league skipped them.
                if self.exploit is not None and self.exploit.active:
                    mine = self.cast.exploiter_rows(cast_rows)
                    if mine.any():
                        actions = self.exploit.act(part, rows, actions, mine)
        goal, goal_log_prob, goal_chosen, slow_before, slow_value, goal_slots = (
            goals if goals is not None else (None, None, None, None, None, None))
        # The obs, state and mask may be views of the sim's device buffers (protocol 15), which the sim overwrites
        # with this group's next STEP as soon as it has these actions. Copied from there into the decision on the
        # current stream, the copies queued behind an overlapped update's kernels (whose first half runs on the
        # default stream) and ran after the sim had written the next step: the rollout buffer paired every action
        # with the observation and mask that followed it, approx_kl in the millions from the fourth update of every
        # fast run. So they go into the rollout buffer on the rollout's own stream: from the captured decision's own
        # copies of them, which stay put until its next replay on that stream (queued, nothing waits); or, without
        # one, from the sim's buffers, finished before the actions go. The buffer is not being read meanwhile:
        # sync_rollout ordered the rollout stream after the update that last read it.
        stream = trainer.rollout_stream
        if stream is not None and decision.buffer is not None and isinstance(part.obs, torch.Tensor) \
                and part.obs.is_cuda:
            inputs = trainer.device_inputs
            if inputs is not None:
                decision.store_inputs(rows, inputs["obs"], inputs["state"], inputs["mask"], stream,
                                      inputs.get("image"))
            else:
                decision.store_inputs(rows, part.obs, part.state, part.mask, stream,
                                      getattr(part, "image", None))
                stream.synchronize()
        else:
            decision.set(rows, obs=part.obs, state=part.state, mask=part.mask)
            if part.image is not None:
                decision.set(rows, image=getattr(part, "image", None))
        decision.set(rows, layout=part.layout, actions=actions, log_probs=log_probs, values=values, present=present,
                     foresight=foresight, memory=memory, goal=goal, goal_log_prob=goal_log_prob,
                     goal_chosen=goal_chosen, slow_before=slow_before, slow_value=slow_value,
                     critic_memory=critic_memory, chosen=chosen, goal_slots=goal_slots)
        send(rows.start, rows.stop - rows.start, actions, trainer.wire_goals(goals[0]) if goals is not None else None)

    def _take_outcome_of(self, part: protocol.Step, rows: slice, decision: DecisionRows,
                         outcome: RolloutOutcome):
        """What `decision` earned in envs `rows`, from their next STEP `part`: rewards, and for the episodes that
        ended, their final values, their info, and a cleared memory for the episodes that follow.

        The ended episodes are valued later: returns None, or a function that values them. What the value depends
        on (the goal and critic memory the episode ended with) is taken here, before their memories are cleared for
        the next decision, so the caller can act on the next decision and send it first, and value the ended
        episodes while the sim ticks -- their forward passes are not what the sim waits for."""
        trainer, spec = self.trainer, self.spec
        done = part.done
        outcome.reward[rows], outcome.done[rows], outcome.terminated[rows] = part.reward, done, part.terminated
        # A cluster worker that dropped out while this decision was out has no outcome to give it: its rows of the
        # decision are no samples (from the next decision on its rows come with no character present anyway).
        if isinstance(self.env, ClusterEnv) and self.env.sat_out[rows].any():
            lost = np.flatnonzero(self.env.sat_out[rows]) + rows.start
            self.buffer.valid[self.buffer.cursor, lost] = False
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
        goal = self.acting.goal[rows][done] if self.acting.goal is not None else None
        critic_end = self.acting.critic_memory[rows][done] if self.acting.critic_memory is not None else None
        ended_memory = memory[done] if memory is not None else None
        ended_layout = layout[done]
        final_state, final_obs = part.final_state[done], part.final_obs[done]
        final_image = part.final_image[done] if getattr(part, "final_image", None) is not None else None

        # Self-imitation keeps the best of them by outcome: each ended env's mean over its seats present.
        if self.sil_score_column is not None:
            info = part.episode_info[done]
            present = (info[..., self.present_column] > 0.0 if self.present_column is not None
                       else np.ones(info.shape[:-1], dtype=bool))
            scores = (info[..., self.sil_score_column] * present).sum(axis=-1) / np.maximum(present.sum(axis=-1), 1)
            self.buffer.ended_episodes.append((self.buffer.cursor, np.flatnonzero(done) + rows.start, scores))

        ended = part.episode_info[done].reshape(-1, spec.episode_info_dim)
        ended_layouts = ended_layout.reshape(-1)
        present = self.present_column
        keep = slice(None) if present is None else ended[:, present] > 0.0
        self.finished_episodes.extend(ended[keep])
        self.finished_layouts.extend(int(index) for index in ended_layouts[keep])
        if self.link is not None:
            self.link.observe(ended[keep], ended_layouts[keep])

        # The exploiter values the episodes ending under it and keeps how its own went, before the cast redraws.
        if self.exploit is not None and self.exploit.active:
            self.exploit.value_ended(part, rows, done, ended_layout)
            self.exploit.results.extend(self.cast.exploiter_results(part, self.won_column))
            cleared_exploit = np.zeros(spec.num_envs, dtype=bool)
            cleared_exploit[rows] = done
            self.exploit.clear(cleared_exploit)

        # A new episode starts with nothing remembered and no goal; the league scores the ended ones.
        if self.cast is not None:
            self.cast.observe_ended(part, self.won_column)
            self.cast.clear(part.done)
        cleared = np.zeros(spec.num_envs, dtype=bool)
        cleared[rows] = done
        self.acting.clear(cleared)

        def value_ended() -> None:
            ended_rows = np.flatnonzero(done) + rows.start
            outcome.final_values[ended_rows] = trainer.value(final_state, final_obs, ended_layout, goal, critic_end,
                                                             final_image)
            if outcome.final_foresight is not None:
                outcome.final_foresight[ended_rows] = trainer.foresight_of(final_obs, ended_layout, ended_memory,
                                                                           final_image)

        return value_ended

    @staticmethod
    def allowed_actions(buffer: RolloutBuffer) -> tuple[float, dict[int, float]]:
        """Mean legal actions per decision over the rollout's samples (0 when there are none), overall and per layout
        index: the mask summed once for both. Entropy only means something against this: a policy over 6 legal
        actions and one over 60 have very different ceilings."""
        layout = buffer.layout.reshape(-1)
        valid = buffer.valid.reshape(-1)
        allowed = host(buffer.mask.reshape(-1, buffer.mask.shape[-1]).sum(-1))
        if not valid.any():
            return 0.0, {}
        out = {}
        for index in np.unique(layout[valid]):
            rows = valid & (layout == index)
            out[int(index)] = float(allowed[rows].mean())
        return float(allowed[valid].mean()), out

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
        # Every rank's ended training episodes, for the leader's controller and log.
        gathered = self.ranks.gather((self.finished_episodes, self.finished_layouts))
        if self.ranks.active:
            if self.ranks.leader:
                self.finished_episodes = [row for rows, _ in gathered for row in rows]
                self.finished_layouts = [layout for _, layouts in gathered for layout in layouts]
            else:
                self.finished_episodes.clear()
                self.finished_layouts.clear()
        self.last_layout_stats = self.named_layout_stats()
        self.controller.observe_update(self.last_layout_stats, self.lr_scale_now)
        self.explore_update()
        self.exploit_step()
        if self.difficulty_column is not None and self.finished_episodes:
            names = [layout.name for layout in spec.layouts]
            episodes = np.asarray(self.finished_episodes)
            layouts = np.asarray(self.finished_layouts)
            played = [index for index in np.unique(layouts) if index < len(names)]
            self.controller.observe_training_episodes(
                {names[index]: float(episodes[layouts == index, self.difficulty_column].mean()) for index in played},
                None if self.top_rung_column is None else {
                    names[index]: float(episodes[layouts == index, self.top_rung_column].mean()) for index in played})
        if self.update % config.log_every != 0:
            return

        row: dict[str, float] = {
            "update": self.update,
            "env_steps": self.env_steps,
            "env_steps_per_sec": config.rollout_length * self.run_envs * spec.agents_per_env / rollout_seconds,
            "update_seconds": time.perf_counter() - started - rollout_seconds,
            "reward_per_decision": self.rollout_reward,
            # Entropy is only readable against how many actions were legal to begin with.
            "allowed_actions": self.rollout_allowed_actions,
            "elapsed_seconds": time.perf_counter() - self.started_at,
            "episodes": len(self.finished_episodes),
            "entropy_coef": self.trainer.entropy_coef,
            "lr_scale": self.lr_scale_now,
            "shaping_scale": self.shaping_scale_now,
            "cost_scale": self.cost_scale_now,
            "frozen_layouts": len(self.frozen),
            **(self.cast.stats() if self.cast is not None else {"cast_rows": 0.0, "cast_fallback_rows": 0.0}),
            **({"distill_coef": self.distiller.coef} if self.distiller is not None else {}),
            **(self.explore.summary() if self.explore is not None else {}),
            **(self.exploit.stats() if self.exploit is not None else {}),
            **getattr(self, "exploit_stats", {}),
            **getattr(self, "style_stats", {}),
        }
        if self.finished_episodes:
            means = episode_means.means(self.finished_episodes, spec.episode_info_names)
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
        # Each rank's own clock, so a cluster's slow rank and phase show in its learner log.
        timing = (f"rollout {rollout_seconds:.2f}s compute {float(stats.get('update_compute_seconds', 0.0)):.2f}s"
                  + (f" sync {float(stats['weight_sync_seconds']):.2f}s" if "weight_sync_seconds" in stats else ""))
        print(f"update {self.update} | steps {self.env_steps} | {row['env_steps_per_sec']:.0f} sps | {timing} | "
              f"{summary}",
              flush=True)
        self.finished_episodes.clear()
        self.finished_layouts.clear()

    def explore_update(self) -> None:
        """The leader archives the cells every rank's ended wing runs reached, and sends the sim the start table when
        the archive changed (protocol EXPLORE_STARTS). A cluster's sims all get it; an async follower's own sim keeps
        starting at the door."""
        if self.explore is None or not self.ranks.leader:
            return
        if self.finished_episodes:
            rows = np.asarray(self.finished_episodes)
            for cell, seconds in cells_of(rows, self.explore_columns):
                self.explore.add(cell, seconds, self.env_steps)
                self.explore_version += 1
        if self.explore_version == self.explore_sent:
            return
        explore = self.config.explore
        table = self.explore.table(explore.table_size)
        if table:
            self.env.set_explore_starts(explore.share, [(cell.arena, cell.tier, cell.packs, cell.yard, weight)
                                                        for cell, weight in table])
            self.explore_sent = self.explore_version

    def audit_progress(self, row: dict[str, float]) -> None:
        """Say so when the updates have stopped moving the policy.

        Roughly half the stages measured end their run barely changing: approx_kl falls eight to eleven fold
        between the first eighth of a run and the last (the party stage 11.2x, duo_led 10.5x, companion 9.2x,
        stage4 8.3x) with clip_frac down to ~0.01, so the final third costs wall clock and buys very little.
        The other half do not -- stage4_duel's KL *rises* over 683 updates, travel and flight stay flat -- so
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
        finding = audit(reward_mix(row), outcome_terms(self.stage))
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

                means = episode_means.means(group, info)
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
        if self.link is not None:
            # A follower trains until the leader stops (or goes): its decisions are the leader's.
            while not self.link.stopped:
                stats, started, rollout_seconds = self.rollout()
                self.log_update(stats, started, rollout_seconds)
            return None
        while self.env_steps < config.total_env_steps:
            stats, started, rollout_seconds = self.rollout()
            self.log_update(stats, started, rollout_seconds)
            self.maybe_checkpoint()

            if self.evaluating and self.env_steps - self.last_eval_env_steps >= config.eval.every_env_steps:
                # The evaluation resets every env: the training episodes in progress are cut short, and the next
                # rollout starts from fresh ones (this rollout's advantages were already computed above).
                self.evaluate()
                decision = self.ranks.broadcast(controller.after_eval(self.env_steps) if self.ranks.leader else None)
                # Converged: that evaluation was the stage's last, so it gets its held-out reading if it had none.
                if decision is not None and decision.action == ADVANCE and self.heldout and not self.heldout_current:
                    self.evaluate_heldout()
                if self.handle(decision):
                    return decision

        # One last score, so the best model also considers the final networks.
        if self.evaluating and self.last_eval_env_steps < self.env_steps:
            self.evaluate(final=True)
        outcome = self.ranks.broadcast(controller.at_budget() if self.ranks.leader else None)
        self.handle(outcome)
        return outcome

    def run(self) -> int:
        """The whole run; returns the process exit code."""
        self.step = self.env.reset()
        if self.cast is not None:
            self._reset_far_side()
        self.progress.write("training", self.update, self.env_steps)
        if self.ranks.broadcast(self.evaluating and self.config.eval.at_start and not self.tracker.history):
            self.evaluate()
        self.last_eval_env_steps = self.ranks.broadcast(self.tracker.history[-1][0] if self.tracker.history
                                                        else self.env_steps)

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
        if outcome and self.ranks.leader:
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
        if self.hub is not None:
            self.hub.close()
        if self.link is not None:
            self.link.close()
        if self.updater is not None:
            self.updater.shutdown()
        self.logger.close()
        self.env.close()
        self.ranks.close()


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

    blas.prepare(config.resolved_train_device())
    return TrainingRun(config, resume=args.resume).run()


if __name__ == "__main__":
    raise SystemExit(main())
