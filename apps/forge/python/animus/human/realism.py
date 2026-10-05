"""Movement realism as a measured number (human-play-data plan 2.5): how far the seats' motion in an evaluation is from
the players', per context.

For every context both sides have, each motion.HIST_FEATURES histogram of the seats' steps is compared with the
players' (human_reference.json, written by the offline pipeline) by earth mover's distance, in the feature's own units
(motion.histogram_distance); a context's distance is the mean over its features, and the overall one the mean over
the contexts weighted by how many of the seats' steps each holds. 0 is motion distributed exactly as the players'.

The evaluation's own motion is also written, as windows, to <run>/eval_motion.npz in human_motion_windows.npz's layout
(FORMAT.md section 5), for the offline realism report.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np

from . import motion

REFERENCE_FORMAT = 1


def load_reference(path: str | Path) -> dict:
    """human_reference.json: {"format": 1, "motion": {context id: {"name", "steps", "hist": {feature: counts}}}, ...},
    with the context ids as ints and the counts as arrays. Refuses another format, and histograms whose length is not
    motion.HIST_BINS'."""
    raw = json.loads(Path(path).read_text())
    if int(raw.get("format", 0)) != REFERENCE_FORMAT:
        raise ValueError(f"{path}: human reference format {raw.get('format')!r}, this learner reads "
                         f"{REFERENCE_FORMAT}")
    contexts = {}
    for key, entry in (raw.get("motion") or {}).items():
        context = int(key)
        hist = {}
        for name, counts in (entry.get("hist") or {}).items():
            if name not in motion.HIST_BINS:
                continue
            counts = np.asarray(counts, dtype=np.float64)
            if len(counts) != len(motion.HIST_BINS[name]) - 1:
                raise ValueError(f"{path}: context {context} {name} has {len(counts)} bins, motion.HIST_BINS has "
                                 f"{len(motion.HIST_BINS[name]) - 1}")
            hist[name] = counts
        contexts[context] = {"name": entry.get("name", motion.context_name(context)),
                             "steps": int(entry.get("steps", 0)), "hist": hist}
    return {**raw, "motion": contexts}


def score(feats: np.ndarray, contexts: np.ndarray, reference: dict) -> dict:
    """The realism of the steps `feats` [N, F] in `contexts` [N] against `reference` (load_reference):

        realism_emd                 the mean over the contexts both have, weighted by the seats' steps in each
        realism_emd_<context name>  each context's mean over the features
        features                    {context name: {feature: distance}}
        steps                       {context name: the seats' steps}
        unscored                    the seats' contexts the players have no motion of

    nan where nothing could be compared."""
    out: dict = {"features": {}, "steps": {}, "unscored": []}
    bot = motion.histograms(feats, contexts) if len(contexts) else {}
    human = reference.get("motion", {})
    total = weighted = 0.0
    for context, hists in sorted(bot.items()):
        name = motion.context_name(context)
        steps = int(np.sum(np.asarray(contexts) == context))
        out["steps"][name] = steps
        theirs = human.get(context)
        if theirs is None or not theirs["hist"]:
            out["unscored"].append(name)
            continue
        distances = {feature: motion.histogram_distance(hists[feature], theirs["hist"][feature],
                                                        motion.HIST_BINS[feature])
                     for feature in motion.HIST_FEATURES if feature in theirs["hist"]}
        out["features"][name] = distances
        finite = [value for value in distances.values() if math.isfinite(value)]
        mean = float(np.mean(finite)) if finite else float("nan")
        out[f"realism_emd_{name}"] = mean
        if math.isfinite(mean):
            total += steps
            weighted += steps * mean
    out["realism_emd"] = weighted / total if total > 0 else float("nan")
    return out


def columns(reference: dict) -> list[str]:
    """The eval.csv columns a reference can fill: the overall distance, each of its contexts', and the discriminator's
    mean output."""
    names = [motion.context_name(context) for context in sorted(reference.get("motion", {}))]
    return ["realism_emd", *(f"realism_emd_{name}" for name in names), "realism_disc"]


def tracks_features(tracks: list[np.ndarray]) -> tuple[np.ndarray, np.ndarray, list[np.ndarray], list[np.ndarray]]:
    """Each track's steps (motion.features, motion.step_contexts), joined: features [N, F], contexts [N], and per
    track its own (for its windows)."""
    feats, contexts = [], []
    for samples in tracks:
        samples = np.asarray(samples, dtype=np.float64)
        if len(samples) < 2:
            continue
        feats.append(motion.features(samples))
        contexts.append(motion.step_contexts(samples))
    if not feats:
        return np.zeros((0, motion.F), np.float32), np.zeros(0, np.int16), [], []
    return np.concatenate(feats), np.concatenate(contexts), feats, contexts


def motion_windows(feats: list[np.ndarray], contexts: list[np.ndarray], window: int = motion.WINDOW,
                   cap: int = 0, rng: np.random.Generator | None = None
                   ) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Every track's sliding windows (motion.windows): [N, W, F] windows, [N] contexts and [N] weights. With `cap`
    above 0 and more windows than that, a uniform draw of `cap` of them, each weighted by the inverse share kept."""
    parts = [motion.windows(f, c, window) for f, c in zip(feats, contexts)]
    if parts:
        windows = np.concatenate([part[0] for part in parts])
        context = np.concatenate([part[1] for part in parts]).astype(np.int16)
    else:
        windows, context = np.zeros((0, window, motion.F), np.float32), np.zeros(0, np.int16)
    weight = np.ones(len(windows), dtype=np.float32)
    if cap > 0 and len(windows) > cap:
        rng = rng or np.random.default_rng(0)
        keep = np.sort(rng.choice(len(windows), size=cap, replace=False))
        weight = np.full(cap, len(windows) / cap, dtype=np.float32)
        windows, context = windows[keep], context[keep]
    return windows.astype(np.float32), context, weight


def write_motion(path: str | Path, windows: np.ndarray, context: np.ndarray, weight: np.ndarray, meta: dict) -> None:
    """eval_motion.npz in human_motion_windows.npz's layout (FORMAT.md section 5): windows f32 [N, W, F], context i16
    [N], weight f32 [N], meta (JSON text). Written beside and renamed over, so a reader never sees half of one."""
    path = Path(path)
    partial = path.with_name(path.name + ".partial.npz")
    np.savez_compressed(partial, windows=np.asarray(windows, np.float32), context=np.asarray(context, np.int16),
                        weight=np.asarray(weight, np.float32), meta=np.asarray(json.dumps(meta)))
    partial.replace(path)
