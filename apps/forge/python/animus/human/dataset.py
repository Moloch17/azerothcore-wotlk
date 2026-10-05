"""human_motion_windows.npz (FORMAT.md §5): windows of human motion features for the style discriminator.

```
windows  f32 [N, motion.WINDOW, motion.F]    motion.windows over each clip's motion.features
context  i16 [N]                             the window's context (its last step's, motion.step_contexts)
weight   f32 [N]                             latency down-weighting (latency_weight)
meta     json string                         source, window/stride/step, per-context counts and balance factors
```

Only usable human clips enter (segment.clips: no involuntary, idle or cut motion, no companions). Windows never
span two clips. **Weight** is the latency weight alone: 1 up to LATENCY_FULL ms, falling linearly to LATENCY_FLOOR
at LATENCY_WORST ms and beyond; a session whose latency is unknown weighs 1. **Balancing** across contexts is left
to the consumer, with what it needs in `meta["contexts"]`: windows kept and seen per context and `balance`, the
factor (kept windows / contexts present) / (kept in this context) that would give every context equal total weight.

The realm keeps everything forever, so the file is bounded: `stride` steps between window starts within a clip,
and at most `max_windows` windows, a uniform sample of all seen (bottom-k on random keys, so the sample does not
depend on the order shards are read in beyond the seed).
"""

from __future__ import annotations

import datetime as dt
import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import motion

FORMAT = 1
LATENCY_FULL = 150.0
LATENCY_WORST = 500.0
LATENCY_FLOOR = 0.25
FILE = "human_motion_windows.npz"


def latency_weight(latency_ms: float) -> float:
    if latency_ms <= LATENCY_FULL:
        return 1.0
    share = min(1.0, (latency_ms - LATENCY_FULL) / (LATENCY_WORST - LATENCY_FULL))
    return float(1.0 - share * (1.0 - LATENCY_FLOOR))


@dataclass
class DatasetBuilder:
    stride: int = 1
    max_windows: int = 2_000_000
    seed: int = 0
    _windows: list[np.ndarray] = field(default_factory=list)
    _context: list[np.ndarray] = field(default_factory=list)
    _weight: list[np.ndarray] = field(default_factory=list)
    _keys: list[np.ndarray] = field(default_factory=list)
    _held: int = 0
    seen: dict[int, int] = field(default_factory=dict)

    def __post_init__(self):
        self._rng = np.random.default_rng(self.seed)

    def add_clip(self, samples: np.ndarray, latency_ms: float = 0.0) -> int:
        """Windows of one clip; returns how many it gave."""
        feats = motion.features(samples)
        ctx = motion.step_contexts(samples)
        win, wctx = motion.windows(feats, ctx, stride=self.stride)
        if len(win) == 0:
            return 0
        for context, count in zip(*np.unique(wctx, return_counts=True)):
            self.seen[int(context)] = self.seen.get(int(context), 0) + int(count)
        self._windows.append(win)
        self._context.append(wctx)
        self._weight.append(np.full(len(win), latency_weight(latency_ms), dtype=np.float32))
        self._keys.append(self._rng.random(len(win)))
        self._held += len(win)
        if self._held > 2 * self.max_windows:
            self._shrink()
        return len(win)

    def _shrink(self) -> None:
        keys = np.concatenate(self._keys)
        keep = np.sort(np.argpartition(keys, self.max_windows - 1)[:self.max_windows]) \
            if len(keys) > self.max_windows else np.arange(len(keys))
        self._windows = [np.concatenate(self._windows)[keep]]
        self._context = [np.concatenate(self._context)[keep]]
        self._weight = [np.concatenate(self._weight)[keep]]
        self._keys = [keys[keep]]
        self._held = len(keep)

    def arrays(self) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        if not self._windows:
            return (np.zeros((0, motion.WINDOW, motion.F), np.float32), np.zeros(0, np.int16),
                    np.zeros(0, np.float32))
        self._shrink()
        return self._windows[0].astype(np.float32), self._context[0].astype(np.int16), self._weight[0]

    def write(self, out_dir: str | Path, source: dict) -> Path:
        windows, context, weight = self.arrays()
        kept = {int(c): int(n) for c, n in zip(*np.unique(context, return_counts=True))}
        present = max(1, len(kept))
        meta = {"format": FORMAT, "built": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
                "source": source, "window": motion.WINDOW, "stride": self.stride,
                "step_seconds": motion.DECISION_SECONDS, "features": list(motion.FEATURES),
                "max_windows": self.max_windows, "sampled": sum(self.seen.values()) > len(windows),
                "latency_weight": {"full_ms": LATENCY_FULL, "worst_ms": LATENCY_WORST, "floor": LATENCY_FLOOR},
                "contexts": {str(c): {"name": motion.context_name(c), "windows": kept.get(c, 0),
                                      "seen": self.seen.get(c, 0),
                                      "balance": round(len(windows) / present / kept[c], 6) if kept.get(c) else 0.0}
                             for c in sorted(set(self.seen) | set(kept))}}
        path = Path(out_dir) / FILE
        path.parent.mkdir(parents=True, exist_ok=True)
        partial = path.with_suffix(".partial.npz")
        np.savez_compressed(partial, windows=windows, context=context, weight=weight, meta=np.asarray(json.dumps(meta)))
        partial.replace(path)
        return path


def load(path: str | Path) -> dict:
    """windows, context, weight and meta (parsed) of a human_motion_windows.npz or a bot eval_motion.npz."""
    with np.load(path, allow_pickle=False) as data:
        out = {key: data[key] for key in data.files}
    if "meta" in out:
        out["meta"] = json.loads(str(out["meta"]))
    return out
