"""Motion features: the one definition of how a body moves, for human players and bots alike (FORMAT.md §3).

A kinematic sample is [t_seconds, x, y, z, yaw, pitch, mode, mounted, speed, in_combat]. Bots are sampled once a
decision by the sim; humans are resampled from their movement packets to the same rate (`resample`). Features are
taken step to step, in the body's own frame and in units of the speed in force, so a gnome and a tauren, a run and a
swim, read the same: "moving forward at full speed while turning left at 90 degrees a second" is one vector whoever
does it. The style discriminator reads windows of them (`windows`), the realism score their histograms (`histograms`).
Nothing else may compute motion features: the human side and the bot side must never drift apart.
"""

from __future__ import annotations

import math

import numpy as np

# Kinematic sample columns.
T, X, Y, Z, YAW, PITCH, MODE, MOUNTED, SPEED, IN_COMBAT = range(10)
SAMPLE_DIM = 10

MODE_GROUND, MODE_SWIM, MODE_FLY, MODE_AIRBORNE = range(4)
MODES = 4

DECISION_SECONDS = 0.25     # AnimusForge.DecisionMs
WINDOW = 8                  # 2 s of decisions: what the discriminator sees at once
DEFAULT_SPEED = 7.0         # run speed, for a sample whose speed is unknown
CLIP = 3.0                  # continuous features are clipped to [-CLIP, CLIP]
MOVING = 0.1                # planar speed (in units of the speed in force) above which the body is moving

FEATURES = (
    "fwd", "lat", "up", "planar", "moving", "yaw_rate", "course_sin", "course_cos", "pitch", "pitch_rate", "accel",
    "mode_ground", "mode_swim", "mode_fly", "mode_airborne", "mounted", "in_combat",
)
F = len(FEATURES)
INDEX = {name: i for i, name in enumerate(FEATURES)}

# Contexts: a bot is compared with humans in the same situation (mode x mounted x combat).
CONTEXTS = MODES * 2 * 2

# Histogram bins of the realism score, fixed so human references and bot evaluations always line up.
HIST_FEATURES = ("fwd", "lat", "up", "planar", "yaw_rate", "pitch", "pitch_rate", "accel")
HIST_BINS = {name: np.linspace(-CLIP, CLIP, 61) for name in HIST_FEATURES}
HIST_BINS["planar"] = np.linspace(0.0, CLIP, 31)


def wrap(angle: np.ndarray | float) -> np.ndarray | float:
    """An angle onto (-pi, pi]."""
    return np.remainder(np.asarray(angle) + math.pi, 2.0 * math.pi) - math.pi


def context_id(mode: np.ndarray | int, mounted: np.ndarray | int, in_combat: np.ndarray | int) -> np.ndarray:
    """The context of a sample: mode * 4 + mounted * 2 + in_combat, 0..CONTEXTS-1."""
    mode = np.clip(np.asarray(mode, dtype=np.int64), 0, MODES - 1)
    return (mode * 4 + (np.asarray(mounted) > 0.5).astype(np.int64) * 2
            + (np.asarray(in_combat) > 0.5).astype(np.int64))


def context_name(context: int) -> str:
    mode = ("ground", "swim", "fly", "airborne")[context // 4]
    return f"{mode}{'_mounted' if context & 2 else ''}{'_combat' if context & 1 else ''}"


def features(samples: np.ndarray) -> np.ndarray:
    """[T, SAMPLE_DIM] samples of one unbroken track -> [T - 1, F] features, one per step (step i is sample i-1 ->
    sample i, labelled with sample i's mode, mount and combat)."""
    s = np.asarray(samples, dtype=np.float64)
    if s.ndim != 2 or s.shape[1] < SAMPLE_DIM:
        raise ValueError(f"samples: expected [T, {SAMPLE_DIM}], got {s.shape}")
    if len(s) < 2:
        return np.zeros((0, F), dtype=np.float32)
    prev, cur = s[:-1], s[1:]
    dt = np.maximum(cur[:, T] - prev[:, T], 1e-3)
    speed = np.where(cur[:, SPEED] > 0.1, cur[:, SPEED], DEFAULT_SPEED)
    dx, dy, dz = cur[:, X] - prev[:, X], cur[:, Y] - prev[:, Y], cur[:, Z] - prev[:, Z]
    yaw = cur[:, YAW]
    cos, sin = np.cos(yaw), np.sin(yaw)
    fwd = (dx * cos + dy * sin) / dt / speed
    lat = (-dx * sin + dy * cos) / dt / speed          # left positive
    up = dz / dt / speed
    planar = np.hypot(fwd, lat)
    moving = planar > MOVING
    course = np.arctan2(lat, fwd)
    yaw_rate = wrap(cur[:, YAW] - prev[:, YAW]) / dt / math.pi
    pitch = cur[:, PITCH] / (math.pi / 3.0)
    pitch_rate = (cur[:, PITCH] - prev[:, PITCH]) / dt / (math.pi / 3.0)
    accel = np.concatenate([[0.0], np.diff(planar)]) / dt

    out = np.zeros((len(cur), F), dtype=np.float64)
    out[:, INDEX["fwd"]] = fwd
    out[:, INDEX["lat"]] = lat
    out[:, INDEX["up"]] = up
    out[:, INDEX["planar"]] = planar
    out[:, INDEX["moving"]] = moving
    out[:, INDEX["yaw_rate"]] = yaw_rate
    out[:, INDEX["course_sin"]] = np.where(moving, np.sin(course), 0.0)
    out[:, INDEX["course_cos"]] = np.where(moving, np.cos(course), 0.0)
    out[:, INDEX["pitch"]] = pitch
    out[:, INDEX["pitch_rate"]] = pitch_rate
    out[:, INDEX["accel"]] = accel
    mode = np.clip(cur[:, MODE].astype(np.int64), 0, MODES - 1)
    out[np.arange(len(cur)), INDEX["mode_ground"] + mode] = 1.0
    out[:, INDEX["mounted"]] = cur[:, MOUNTED] > 0.5
    out[:, INDEX["in_combat"]] = cur[:, IN_COMBAT] > 0.5
    continuous = [INDEX[name] for name in ("fwd", "lat", "up", "planar", "yaw_rate", "pitch", "pitch_rate", "accel")]
    out[:, continuous] = np.clip(out[:, continuous], -CLIP, CLIP)
    return out.astype(np.float32)


def step_contexts(samples: np.ndarray) -> np.ndarray:
    """The context of each step of `features(samples)`."""
    s = np.asarray(samples)
    return context_id(s[1:, MODE], s[1:, MOUNTED], s[1:, IN_COMBAT]).astype(np.int16)


def windows(feats: np.ndarray, contexts: np.ndarray, window: int = WINDOW, stride: int = 1
            ) -> tuple[np.ndarray, np.ndarray]:
    """Sliding windows of `window` consecutive steps of one track: [N, window, F] and each window's context (its
    last step's)."""
    feats = np.asarray(feats, dtype=np.float32)
    if len(feats) < window:
        return np.zeros((0, window, F), dtype=np.float32), np.zeros(0, dtype=np.int16)
    starts = np.arange(0, len(feats) - window + 1, stride)
    index = starts[:, None] + np.arange(window)[None, :]
    return feats[index], np.asarray(contexts, dtype=np.int16)[starts + window - 1]


def resample(samples: np.ndarray, step: float = DECISION_SECONDS, max_gap: float = 1.5) -> list[np.ndarray]:
    """Irregular samples (movement packets) -> tracks on a regular `step` grid, cut wherever two samples are more
    than `max_gap` seconds apart (a client sends a heartbeat every 0.5 s while moving, so a longer gap is a stop,
    a loading screen or lost data). Position and pitch are interpolated linearly, yaw along the shorter arc; mode,
    mount, speed and combat are held from the latest sample at or before each grid time."""
    s = np.asarray(samples, dtype=np.float64)
    if len(s) == 0:
        return []
    s = s[np.argsort(s[:, T], kind="stable")]
    cuts = np.flatnonzero(np.diff(s[:, T]) > max_gap) + 1
    tracks = []
    for part in np.split(s, cuts):
        if len(part) < 2 or part[-1, T] - part[0, T] < step:
            continue
        grid = np.arange(part[0, T], part[-1, T] + 1e-9, step)
        out = np.zeros((len(grid), SAMPLE_DIM))
        out[:, T] = grid
        for column in (X, Y, Z, PITCH):
            out[:, column] = np.interp(grid, part[:, T], part[:, column])
        unwrapped = np.unwrap(part[:, YAW])
        out[:, YAW] = np.remainder(np.interp(grid, part[:, T], unwrapped), 2.0 * math.pi)
        held = np.clip(np.searchsorted(part[:, T], grid, side="right") - 1, 0, len(part) - 1)
        for column in (MODE, MOUNTED, SPEED, IN_COMBAT):
            out[:, column] = part[held, column]
        tracks.append(out)
    return tracks


def histograms(feats: np.ndarray, contexts: np.ndarray) -> dict[int, dict[str, np.ndarray]]:
    """Per context, per HIST_FEATURES feature, the counts in HIST_BINS (moving steps only for the course-dependent
    features is not needed: all are defined at rest)."""
    feats = np.asarray(feats)
    contexts = np.asarray(contexts)
    out: dict[int, dict[str, np.ndarray]] = {}
    for context in np.unique(contexts):
        rows = feats[contexts == context]
        out[int(context)] = {name: np.histogram(rows[:, INDEX[name]], bins=HIST_BINS[name])[0]
                             for name in HIST_FEATURES}
    return out


def histogram_distance(a: np.ndarray, b: np.ndarray, bins: np.ndarray) -> float:
    """Earth mover's distance between two histograms on the same bins, in the feature's own units; nan if either
    is empty."""
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    if a.sum() <= 0 or b.sum() <= 0:
        return float("nan")
    width = float(bins[1] - bins[0])
    return float(np.abs(np.cumsum(a / a.sum()) - np.cumsum(b / b.sum())).sum() * width)
