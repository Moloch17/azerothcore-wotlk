"""**The school's recorder** (config `school.*`, animus.config.SchoolConfig): samples of a nav stage's rollouts for the
offline planner training (animus.school.train), written off the hot path.

A sample is one agent's decision: the raw map crop bytes of both tiers (`crops`, 2 x 48 x 48 x 8 = 36864 u8, the
camera row's bytes after the image: `[fine][coarse]`, decoded by networks.decode_map), the raw label bytes (`labels`,
protocol.LABEL_BYTES 13848 u8: the true classes and route cost-to-go of both tiers, the six f32 scalars), the sim's
seventeen route scalars off the observation (`route`, networks.ROUTE_COLUMNS order, raw: the sim's Dijkstra waypoint
on the bot's own 1-yd map, for comparison), the memory flag (`memory`: 1 the seat adopted the map's shared memory,
0 blank, -1 unknown -- the episode's `shared_memory` category where the stage has one, else the map block's `kept`
scalar), that `kept` scalar as sent, the game map id and the arena index (`map`, `arena`: episode info columns, so
a decision's sample waits in a pending list until its env's episode ends and is stamped then; the samples of an
episode still running when the learner stops, when an evaluation restarts every env or when a cluster worker
rejoins are dropped and counted), the layout index, the decision's index in
its episode (`step`, 0 the first), an episode id unique over the run (`episode`, for a hold-out by episode) and the
env. Shards are compressed .npz of `record_shard` samples, `rank<r>-<n>.npz`, numbered on from the directory's
own manifest (`manifest-rank<r>.json`: counts per map, layout and memory flag, per shard and in all; a reader merges
every writer's), so a resumed run neither overwrites nor overshoots `record_max_samples`, and learners sharing the
directory never write one file.

Cost: every `record_every`-th decision of an env the sampled rows' bytes are sliced (a device buffer's rows copied
down on the rollout stream, as the finite check reads them) into per-layout batches appended to the env's pending
list; the shard's compression and write run on one background thread. Measured with fake steps in the dev container
(192 envs, every 8, 2026-10-17): 0.41 ms a decision, ~1.7 % of a 24 ms decision, so the default is every 16
(docs/forge/reference/school.md).
"""

from __future__ import annotations

import io
import json
import os
import queue
import threading
import time
from contextlib import nullcontext
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import torch

from .. import protocol
from ..mappo.networks import MAP_BLOCK, MAP_CHANNELS, ROUTE_COLUMNS

#: One tier's crop bytes, and both tiers' (map block revision 5 on: [fine][coarse]).
CROP_SIDE = protocol.LABEL_SIDE
CROP_BYTES = CROP_SIDE * CROP_SIDE * MAP_CHANNELS
CROPS_BYTES = 2 * CROP_BYTES
#: The sample's arrays, their dtypes and their trailing shape: the shard format (data.py reads it, synthetic.py
#: writes it too).
SAMPLE_FIELDS = (("crops", np.uint8, (CROPS_BYTES,)), ("labels", np.uint8, (protocol.LABEL_BYTES,)),
                 ("route", np.float32, (len(ROUTE_COLUMNS),)), ("memory", np.int8, ()), ("kept", np.float32, ()),
                 ("map", np.int32, ()), ("arena", np.int32, ()), ("layout", np.int16, ()), ("step", np.int32, ()),
                 ("episode", np.int64, ()), ("env", np.int32, ()))
SHARD_FORMAT = 1
#: The memory flag's values.
MEMORY_ON, MEMORY_OFF, MEMORY_UNKNOWN = 1, 0, -1
#: Episode info columns the stamp reads (protocol Spec.episode_info_names).
MAP_COLUMN, ARENA_COLUMN, SHARED_MEMORY_COLUMN = "map", "arena", "shared_memory"


def default_record_dir(shared_runs: str, run_name: str) -> Path:
    """var/animus-forge/shared/school/<run_name>: the shared root's `school` beside its `runs` (TrainConfig.
    shared_runs)."""
    return Path(shared_runs).parent / "school" / run_name


def empty_batch(count: int = 0) -> dict[str, np.ndarray]:
    """A sample batch's arrays, `count` rows each, zeroed (SAMPLE_FIELDS)."""
    return {name: np.zeros((count, *shape), dtype=dtype) for name, dtype, shape in SAMPLE_FIELDS}


def shard_counts(batch: dict[str, np.ndarray]) -> dict:
    """The manifest's counts of a batch: per map, layout and memory flag (string keys, JSON's)."""
    def count(values) -> dict[str, int]:
        keys, counts = np.unique(np.asarray(values), return_counts=True)
        return {str(int(key)): int(value) for key, value in zip(keys, counts)}
    memory = count(batch["memory"])
    return {"samples": int(batch["memory"].shape[0]), "maps": count(batch["map"]), "layouts": count(batch["layout"]),
            "memory": {"on": memory.get(str(MEMORY_ON), 0), "off": memory.get(str(MEMORY_OFF), 0),
                       "unknown": memory.get(str(MEMORY_UNKNOWN), 0)}}


def _merge_counts(total: dict, part: dict) -> None:
    total["samples"] = total.get("samples", 0) + part["samples"]
    for group in ("maps", "layouts", "memory"):
        bucket = total.setdefault(group, {})
        for key, value in part[group].items():
            bucket[key] = bucket.get(key, 0) + value


def _empty_manifest() -> dict:
    return {"format": SHARD_FORMAT, "samples": 0, "shards": [], "counts": {"samples": 0, "maps": {}, "layouts": {},
                                                                         "memory": {}}, "meta": {}}


def manifest_name(prefix: str) -> str:
    """Each writer's own manifest, `manifest-<prefix>.json`: learners sharing a directory (a machine's local ranks)
    never write the same file."""
    return f"manifest-{prefix}.json"


def read_manifest(directory: Path, prefix: str | None = None) -> dict:
    """The directory's manifest: the one writer's with `prefix`, else every writer's merged (the shards in file
    order, the counts summed, the metas by prefix); an empty one where there is none."""
    directory = Path(directory)
    paths = [directory / manifest_name(prefix)] if prefix is not None else sorted(directory.glob("manifest-*.json"))
    out = _empty_manifest()
    for path in paths:
        if not path.exists():
            continue
        part = json.loads(path.read_text())
        if part.get("format", SHARD_FORMAT) != SHARD_FORMAT:
            raise ValueError(f"{path}: shard format {part.get('format')}, this reader reads {SHARD_FORMAT}")
        out["shards"].extend(part.get("shards", ()))
        _merge_counts(out["counts"], part.get("counts", {"samples": 0, "maps": {}, "layouts": {}, "memory": {}}))
        out["meta"][path.stem[len("manifest-"):]] = part.get("meta", {})
    out["samples"] = out["counts"]["samples"]
    return out


def write_shard(path: Path, batch: dict[str, np.ndarray], meta: dict | None = None) -> None:
    """One compressed shard at `path` (written beside and renamed, so a reader never sees a part)."""
    tmp = path.with_name(path.name + ".tmp")
    buffer = io.BytesIO()
    np.savez_compressed(buffer, meta=np.array(json.dumps(meta or {})), **batch)
    tmp.write_bytes(buffer.getvalue())
    os.replace(tmp, path)


def read_shard(path: Path) -> dict[str, np.ndarray]:
    """A shard's arrays (SAMPLE_FIELDS; "meta" left out), loaded whole."""
    with np.load(path) as data:
        return {name: np.ascontiguousarray(data[name]) for name, _, _ in SAMPLE_FIELDS}


class ShardWriter:
    """Shards under `directory` from one background thread: `add(batch)` queues a batch of samples (any count); a
    shard is written every `shard_size` samples and the manifest rewritten after each; `close()` writes the rest.
    `limit` (0 none) caps the directory's samples, the manifest's existing ones included (`full`)."""

    def __init__(self, directory: str | Path, shard_size: int, prefix: str = "rank0", limit: int = 0,
                 meta: dict | None = None):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.shard_size = max(1, int(shard_size))
        self.prefix = prefix
        self.limit = int(limit)
        self.meta = dict(meta or {})
        # This writer's own manifest (its numbering), and the directory's total (the limit counts every writer's).
        self.manifest = read_manifest(self.directory, prefix)
        self.manifest["meta"] = self.meta
        self.written = int(read_manifest(self.directory).get("samples", 0))
        self.number = 1 + max((int(shard.get("number", -1)) for shard in self.manifest["shards"]), default=-1)
        self.queued = 0
        self.held: list[dict[str, np.ndarray]] = []
        self.held_count = 0
        self.failed: BaseException | None = None
        self.write_seconds = 0.0
        self.queue: queue.Queue = queue.Queue()
        self.thread = threading.Thread(target=self._run, name="school-shards", daemon=True)
        self.thread.start()

    @property
    def full(self) -> bool:
        return self.limit > 0 and self.written + self.queued >= self.limit

    def add(self, batch: dict[str, np.ndarray]) -> int:
        """Queue `batch` (SAMPLE_FIELDS arrays, n rows each), cut to the limit; returns the rows taken."""
        if self.failed is not None:
            raise RuntimeError("the school's shard writer failed") from self.failed
        count = int(batch["memory"].shape[0])
        if self.limit > 0:
            count = max(0, min(count, self.limit - self.written - self.queued))
        if count == 0:
            return 0
        if count < int(batch["memory"].shape[0]):
            batch = {name: values[:count] for name, values in batch.items()}
        self.queued += count
        self.queue.put(batch)
        return count

    def close(self) -> None:
        self.queue.put(None)
        self.thread.join()
        if self.failed is not None:
            raise RuntimeError("the school's shard writer failed") from self.failed

    def _run(self) -> None:
        try:
            while True:
                batch = self.queue.get()
                if batch is None:
                    if self.held_count:
                        self._flush()
                    return
                self.held.append(batch)
                self.held_count += int(batch["memory"].shape[0])
                while self.held_count >= self.shard_size:
                    self._flush(self.shard_size)
        except BaseException as error:  # noqa: BLE001 - surfaced to the hot path by add()/close()
            self.failed = error

    def _flush(self, count: int | None = None) -> None:
        """Write the next `count` held samples (all of them when None) as one shard."""
        started = time.perf_counter()
        joined = {name: np.concatenate([batch[name] for batch in self.held]) for name, _, _ in SAMPLE_FIELDS}
        take = self.held_count if count is None else count
        batch = {name: values[:take] for name, values in joined.items()}
        rest = {name: values[take:] for name, values in joined.items()}
        self.held = [rest] if take < self.held_count else []
        self.held_count -= take
        name = f"{self.prefix}-{self.number:05d}.npz"
        write_shard(self.directory / name, batch, self.meta)
        counts = shard_counts(batch)
        self.manifest["shards"].append({"file": name, "prefix": self.prefix, "number": self.number, **counts})
        _merge_counts(self.manifest["counts"], counts)
        self.manifest["samples"] = self.manifest["counts"]["samples"]
        self.number += 1
        self.written += take
        self.queued -= take
        tmp = self.directory / (manifest_name(self.prefix) + ".tmp")
        tmp.write_text(json.dumps(self.manifest, indent=1))
        os.replace(tmp, self.directory / manifest_name(self.prefix))
        self.write_seconds += time.perf_counter() - started


@dataclass
class _Pending:
    """An env's samples of the episode it is playing (batches of SAMPLE_FIELDS arrays), waiting for its info."""

    samples: list[dict[str, np.ndarray]] = field(default_factory=list)


def layout_columns(stage: dict | None, vision: list | None, layout_names: list[str]) -> list[dict | None]:
    """Per layout what the recorder reads off a row -- {"image_bytes" (the crop's first byte of the camera row),
    "route" (the seventeen route columns, or None), "kept" (the map block's `kept` column, or -1)} -- or None for a
    layout without the map; from `vision` (networks.vision_of) and stage.json's map block scalar names."""
    out: list[dict | None] = []
    for index, name in enumerate(layout_names):
        entry = vision[index] if vision is not None and index < len(vision) else None
        crop = entry.get("map") if entry else None
        if crop is None:
            out.append(None)
            continue
        kept = -1
        layout = ((stage or {}).get("layouts") or {}).get(name) or {}
        block = next((b for b in layout.get("blocks", ()) if b.get("name") == MAP_BLOCK), None)
        names = list(((block or {}).get("map") or {}).get("scalar_names") or ())
        if "kept" in names:
            kept = int(crop["first"]) + names.index("kept")
        route = crop.get("route")
        out.append({"image_bytes": int(entry["image_bytes"]), "map_bytes": int(crop["map_bytes"]),
                    "route": list(route["columns"]) if route else None, "kept": kept})
    return out


class SchoolRecorder:
    """The recorder in a TrainingRun (train.py: `observe` in _act_on_rows before the actions go, `episode_ended` in
    _take_outcome_of, `close` in finish). Built only with `school.record`; the module docstring says what a sample
    is."""

    def __init__(self, directory: str | Path, every: int, shard: int, limit: int, spec, stage: dict | None,
                 vision: list | None, rank: int = 0, run_name: str = ""):
        if int(getattr(spec, "label_bytes", 0)) != protocol.LABEL_BYTES:
            raise ValueError(f"school.record: the stage sends {getattr(spec, 'label_bytes', 0)} label bytes an agent, "
                             f"the school reads {protocol.LABEL_BYTES} (a nav stage at protocol "
                             f"{protocol.PROTOCOL_VERSION})")
        if int(getattr(spec, "map_bytes", 0)) != CROPS_BYTES:
            raise ValueError(f"school.record: the stage's map crop is {getattr(spec, 'map_bytes', 0)} bytes an agent, "
                             f"the school reads two tiers of {CROP_BYTES}")
        names = [layout.name for layout in spec.layouts]
        self.columns = layout_columns(stage, vision, names)
        if not any(self.columns):
            raise ValueError("school.record: no layout of the stage has the map block")
        info = list(spec.episode_info_names)
        self.info_map = info.index(MAP_COLUMN) if MAP_COLUMN in info else -1
        self.info_arena = info.index(ARENA_COLUMN) if ARENA_COLUMN in info else -1
        self.info_memory = info.index(SHARED_MEMORY_COLUMN) if SHARED_MEMORY_COLUMN in info else -1
        self.every = max(1, int(every))
        self.envs, self.agents = spec.num_envs, spec.agents_per_env
        # Staggered, so the envs are not all due on the same decision (192 rows copied at once, every sample of
        # the first shards from the same decisions) until their episodes desynchronise them.
        self.counters = np.arange(self.envs, dtype=np.int64) % self.every
        self.pending = [_Pending() for _ in range(self.envs)]
        self.episode_base = int(rank) << 40
        self.episodes = 0
        self.rank = int(rank)
        meta = {"run_name": run_name, "protocol": protocol.PROTOCOL_VERSION, "label_bytes": protocol.LABEL_BYTES,
                "crop_bytes": CROP_BYTES, "route_columns": list(ROUTE_COLUMNS), "layouts": names,
                "arenas": [arena.get("name") for arena in (stage or {}).get("arenas", ())],
                "episode_info": {"map": self.info_map, "arena": self.info_arena,
                                 "shared_memory": self.info_memory}}
        self.writer = ShardWriter(directory, shard, prefix=f"rank{self.rank}", limit=limit, meta=meta)
        self.seconds = 0.0
        self.sampled = 0
        self.dropped = 0
        self.stamped = 0

    @property
    def full(self) -> bool:
        return self.writer.full

    def describe(self) -> str:
        return (f"School recorder: every {self.every} decisions an env into {self.writer.directory} (shards of "
                f"{self.writer.shard_size}; {self.writer.written} there already"
                + (f", {self.writer.limit} at most" if self.writer.limit else "")
                + f"; memory flag from the episode's {SHARED_MEMORY_COLUMN} column"
                + ("" if self.info_memory >= 0 else " -- absent, so the map block's kept scalar") + ")")

    def observe(self, part: protocol.Step, rows: slice, stream=None) -> None:
        """A decision's STEP `part` of envs `rows` (before its actions go): the envs on their clock are sampled."""
        if self.full:
            return
        started = time.perf_counter()
        envs = np.arange(rows.start, rows.stop)
        due = self.counters[rows] % self.every == 0
        self.counters[rows] += 1
        picked = np.flatnonzero(due)
        if picked.size and part.labels is not None and part.image is not None:
            present = np.asarray(part.present, dtype=bool)[picked]
            if present.any():
                self._sample(part, picked, envs[picked], present, stream)
        self.seconds += time.perf_counter() - started

    def _sample(self, part: protocol.Step, picked: np.ndarray, envs: np.ndarray, present: np.ndarray,
                stream) -> None:
        """The sampled envs' rows (`picked` into `part`, `envs` their absolute indexes, `present` [k, A]) as one
        batch a layout, appended to each env's pending list (no per-row Python: the hot path's cost is the slices)."""
        image, obs = part.image, part.obs
        # The crop's bytes alone (one slice where every layout's camera row lays them at the same byte, the usual
        # case), the rows' observations, labels and layouts; a device buffer's rows are cut on the device and copied
        # down on the rollout stream (the finite check's way).
        firsts = sorted({columns["image_bytes"] for columns in self.columns if columns is not None})
        span = CROPS_BYTES if len(firsts) == 1 else max(firsts) + CROPS_BYTES - firsts[0]
        if hasattr(image, "detach"):
            index = torch.as_tensor(picked, device=image.device)
            with torch.cuda.stream(stream) if (stream is not None and image.is_cuda) else nullcontext():
                crops = image.index_select(0, index)[:, :, firsts[0]:firsts[0] + span].cpu().numpy()
                obs = obs.index_select(0, index).cpu().numpy() if hasattr(obs, "detach") else \
                    np.asarray(obs)[picked]
        else:
            crops = np.asarray(image)[picked, :, firsts[0]:firsts[0] + span]
            obs = np.asarray(obs)[picked]
        labels = np.asarray(part.labels)[picked]
        layout = np.asarray(part.layout)[picked].astype(np.int64)
        steps = (self.counters[envs] - 1).astype(np.int32)
        for which in np.unique(layout[present]):
            columns = self.columns[int(which)]
            if columns is None:
                continue
            rows_i, rows_a = np.nonzero(present & (layout == which))
            first = columns["image_bytes"] - firsts[0]
            batch = empty_batch(len(rows_i))
            batch["crops"][:] = crops[rows_i, rows_a, first:first + CROPS_BYTES]
            batch["labels"][:] = labels[rows_i, rows_a]
            if columns["route"] is not None:
                batch["route"][:] = obs[rows_i, rows_a][:, columns["route"]]
            batch["kept"][:] = obs[rows_i, rows_a, columns["kept"]] if columns["kept"] >= 0 else -1.0
            batch["memory"][:] = MEMORY_UNKNOWN
            batch["map"][:] = -1
            batch["arena"][:] = -1
            batch["layout"][:] = which
            batch["step"][:] = steps[rows_i]
            batch["env"][:] = envs[rows_i]
            self.sampled += len(rows_i)
            # One batch an env (an env's rows are consecutive in rows_i: nonzero is row-major).
            for env in np.unique(rows_i):
                own = rows_i == env
                self.pending[int(envs[env])].samples.append({name: values[own] for name, values in batch.items()})

    def episode_ended(self, rows: slice, done: np.ndarray, info: np.ndarray) -> None:
        """The envs `rows` whose episodes ended (`done` [E]; `info` [D, A, K] their episode info, protocol
        Step.episode_info[done]): their pending samples are stamped and queued, their clocks reset."""
        started = time.perf_counter()
        ended = np.flatnonzero(done)
        for which, offset in enumerate(ended):
            env = rows.start + int(offset)
            self.counters[env] = 0
            pending = self.pending[env].samples
            if not pending:
                continue
            self.pending[env].samples = []
            self.episodes += 1
            episode = self.episode_base + self.episodes
            row = info[which]
            agent = int(np.argmax(np.isfinite(row).all(axis=-1))) if row.ndim == 2 else 0
            values = row[agent] if row.ndim == 2 else row
            map_id = int(round(float(values[self.info_map]))) if self.info_map >= 0 else -1
            arena = int(round(float(values[self.info_arena]))) if self.info_arena >= 0 else -1
            batch = {name: np.concatenate([piece[name] for piece in pending]) for name, _, _ in SAMPLE_FIELDS}
            if self.info_memory >= 0:
                batch["memory"][:] = MEMORY_ON if float(values[self.info_memory]) > 0.5 else MEMORY_OFF
            else:
                kept = batch["kept"]
                batch["memory"][:] = np.where(kept < 0.0, MEMORY_UNKNOWN,
                                              np.where(kept > 0.5, MEMORY_ON, MEMORY_OFF)).astype(np.int8)
            batch["map"][:] = map_id
            batch["arena"][:] = arena
            batch["episode"][:] = episode
            self.stamped += self.writer.add(batch)
        self.seconds += time.perf_counter() - started

    def clear(self, mask: np.ndarray | None = None) -> None:
        """The envs `mask` [E] (every env when None) start over without an episode's end -- a cluster worker's
        rejoin, the training MODE after an evaluation -- so their pending samples are dropped (no episode info will
        ever name their map) and their clocks reset."""
        envs = np.flatnonzero(mask) if mask is not None else np.arange(self.envs)
        for env in envs:
            self.dropped += sum(int(piece["memory"].shape[0]) for piece in self.pending[int(env)].samples)
            self.pending[int(env)].samples = []
            self.counters[int(env)] = int(env) % self.every

    def close(self) -> str:
        """Finish the shards; the samples of episodes still running are dropped. Returns the summary line."""
        self.dropped += sum(int(piece["memory"].shape[0]) for pending in self.pending for piece in pending.samples)
        self.writer.close()
        return (f"School recorder: {self.writer.written} samples in {self.writer.directory} ({self.stamped} this "
                f"run, {self.dropped} of unfinished episodes dropped; {self.seconds * 1000.0:.0f} ms on the rollout "
                f"path, {self.writer.write_seconds:.1f} s writing shards on the side)")

