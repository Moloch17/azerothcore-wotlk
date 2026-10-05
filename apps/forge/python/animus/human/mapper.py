"""The inverse action mapper (plan §6.5): per decision, the nearest bot action to what a human did, with a confidence.

**Movement.** A clip on the decision grid is reproduced through the fit emulator (fit.beam_fit, MoveBlock's
semantics, segments of SEGMENT_SECONDS re-anchored on the human), and each decision's chosen action is the mapping:
a bearing, HALT, a facing mode, a turn, a pitch, a jump, or no movement press at all. The action is reported as
MoveBlock's local action index (MOVE_LOCAL: bearings 0-7, HALT 8, FACE_TARGET 9, FACE_HEADING 10, FACE_HOLD 11,
turns 12-20 in TURN_ANGLES order, pitches 21-29, JUMP 30, revision 1's fine turns 31-32), or None for no press.
The **confidence** of a decision is how much worse the best alternative action would have done there, the rest of
the sequence kept, over the next LOOKAHEAD decisions: (c_alt - c_fit) / (c_alt + c_fit + CONF_SCALE), in [0, 1].

**Casts.** A CastRequest's spell -> its rank chain's first rank (`spell_ranks`: first_spell_id, spell_id, rank)
-> the catalog action whose `first_rank` it is, in a layout manifest's core block (`blocks[core].catalog`); the
global action index is `core.actions[0] + catalog index`, named by the manifest's `action_names`. A spell absent
from spell_ranks is its own first rank. Confidence 1 when found, 0 (action None) when the catalog has no such spell.

**Selection.** A Select's target -> its slot rank: its position among the snapshot's units of the same reaction,
in the snapshot's order (nearest first, FORMAT.md §2.5); None when the target is not among them.

`spell_ranks` comes from the world DB (`export_spell_ranks`): run inside the forge DB container with the
container's own root password, or with DOCKER_DB_ROOT_PASSWORD from the environment or an env file, or parsed from
an SQL dump (the base `spell_ranks.sql`, offline; pending updates could differ). Never the realm's database.

**Validation** (`validate_movement`, `validate_casts`): known action sequences emulated into kinematics (optionally
with position and facing noise) and mapped back; accuracy is the share of decisions whose mapped action equals the
true one (press accuracy: of the decisions where something was pressed). A press whose removal leaves the motion
exactly as it was (a facing mode with nothing to snap) cannot be recovered from kinematics by anything; it is
counted apart (`unobservable_presses`) and left out of both accuracies. Targets (plan): >= 95% movement, >= 99%
casts.
"""

from __future__ import annotations

import csv
import json
import math
import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from animus.human import fit, motion

SEGMENT_SECONDS = 20.0
LOOKAHEAD = 4
CONF_SCALE = 0.05
DB_CONTAINER = "ac-animus-forge-database"
WORLD_DB = "acore_world"

# MoveBlock::Action, local to the move block.
MOVE_HALT, MOVE_FACE_TARGET, MOVE_FACE_HEADING, MOVE_FACE_HOLD = 8, 9, 10, 11
MOVE_TURN_FIRST, MOVE_PITCH_FIRST, MOVE_JUMP, MOVE_FINE_LEFT, MOVE_FINE_RIGHT = 12, 21, 30, 31, 32


def move_local(space: fit.Space, index: int) -> int | None:
    """MoveBlock's local action for an emulator action (None for no press, and for a turn_rate action, which
    MoveBlock does not have)."""
    kind, value = space.actions[index]
    if kind == fit.NOOP or kind == fit.RATE:
        return None
    if kind == fit.BEARING:
        return int(value)
    if kind == fit.HALT:
        return MOVE_HALT
    if kind == fit.FACE_HEADING:
        return MOVE_FACE_HEADING
    if kind == fit.FACE_HOLD:
        return MOVE_FACE_HOLD
    if kind == fit.JUMP:
        return MOVE_JUMP
    if kind == fit.PITCH:
        return MOVE_PITCH_FIRST + int(np.argmin(np.abs(np.asarray(fit.PITCH_ANGLES) - value)))
    if abs(value - fit.FINE_TURN) < 1e-6:
        return MOVE_FINE_LEFT
    if abs(value + fit.FINE_TURN) < 1e-6:
        return MOVE_FINE_RIGHT
    return MOVE_TURN_FIRST + int(np.argmin(np.abs(np.asarray(fit.TURN_ANGLES) - value)))


@dataclass
class MovementMapping:
    actions: list[int]              # emulator action per decision (space.actions index)
    local: list[int | None]         # MoveBlock local action per decision
    confidence: np.ndarray          # [K]
    pos_err: np.ndarray             # [K + 1]


def _costs(space: fit.Space, state: fit.State, kinds: np.ndarray, values: np.ndarray, human: np.ndarray, k: int,
           tail: list[int], lookahead: int) -> np.ndarray:
    """Cost over decisions k..k+lookahead of each first action (kinds/values, one per state row), the fitted
    `tail` after it."""
    akinds = np.array([a[0] for a in space.actions])
    avalues = np.array([a[1] for a in space.actions], dtype=np.float64)
    total = np.zeros(len(kinds))
    s = state
    for j in range(lookahead + 1):
        if k + j >= len(human) - 1:
            break
        if j == 0:
            kk, vv = kinds, values
        else:
            a = tail[j - 1] if j - 1 < len(tail) else 0
            kk, vv = np.full(len(kinds), akinds[a]), np.full(len(kinds), avalues[a])
        h = human[k + j]
        speed = float(h[motion.SPEED]) if h[motion.SPEED] > 0.1 else motion.DEFAULT_SPEED
        s, allowed = fit.step(s, kk, vv, int(h[motion.MODE]), speed, space)
        target = human[k + j + 1]
        if int(target[motion.MODE]) in (motion.MODE_SWIM, motion.MODE_FLY):
            err2 = (s.x - target[motion.X]) ** 2 + (s.y - target[motion.Y]) ** 2 + (s.z - target[motion.Z]) ** 2
        else:
            s.z[:] = target[motion.Z]
            err2 = (s.x - target[motion.X]) ** 2 + (s.y - target[motion.Y]) ** 2
        total += err2 + (fit.YAW_WEIGHT * motion.wrap(s.facing - target[motion.YAW])) ** 2
        if j == 0:
            total[~allowed] = np.inf
    return total


def map_movement(samples: np.ndarray, space: fit.Space | None = None, beam: int = 32,
                 lookahead: int = LOOKAHEAD, start_bearing: int | None = None,
                 jumps: np.ndarray | None = None) -> MovementMapping:
    """Per decision of a clip (on space.dt), the nearest move action and its confidence. Where the clip begins
    the bearing held is unknown and any is tried, unless `start_bearing` says (validation knows it). `jumps`
    ([K] bool per decision, a track's jump packets) pins where the jumps were."""
    space = space or fit.SPACES["lattice"]
    h = np.asarray(samples, dtype=np.float64)
    seg = int(round(SEGMENT_SECONDS / space.dt))
    actions: list[int] = []
    conf: list[float] = []
    errs = [0.0]
    akinds = np.array([a[0] for a in space.actions])
    avalues = np.array([a[1] for a in space.actions], dtype=np.float64)
    for first in range(0, len(h) - 1, seg):
        part = h[first:first + seg + 1]
        if len(part) < 2:
            break
        known = (start_bearing,) if first == 0 and start_bearing is not None else None
        got = fit.beam_fit(space, part, beam, known,
                           None if jumps is None else np.asarray(jumps[first:first + len(part) - 1], dtype=bool))
        errs += list(got.pos_err[1:])
        # Replay to each decision's state, and price every alternative there.
        state = fit.State.start(1, part[0, motion.X], part[0, motion.Y], part[0, motion.Z], part[0, motion.YAW],
                                part[0, motion.PITCH], got.start_bearing)
        for k, a in enumerate(got.actions):
            rows = state.take(np.zeros(len(space.actions), dtype=np.int64))
            costs = _costs(space, rows, akinds, avalues, part, k, got.actions[k + 1:], lookahead)
            mine = costs[a]
            others = np.delete(costs, a)
            alt = float(np.min(others)) if len(others) else math.inf
            conf.append(1.0 if not math.isfinite(alt) else float(np.clip((alt - mine) / (alt + mine + CONF_SCALE),
                                                                           0.0, 1.0)))
            speed = float(part[k, motion.SPEED]) if part[k, motion.SPEED] > 0.1 else motion.DEFAULT_SPEED
            state, _ = fit.step(state, np.array([akinds[a]]), np.array([avalues[a]]), int(part[k, motion.MODE]),
                                speed, space)
            if int(part[k + 1, motion.MODE]) not in (motion.MODE_SWIM, motion.MODE_FLY):
                state.z[:] = part[k + 1, motion.Z]
        actions += got.actions
    return MovementMapping(actions, [move_local(space, a) for a in actions], np.asarray(conf), np.asarray(errs))


# Casts.

def read_spell_ranks(path: str | Path) -> dict[int, int]:
    """spell id -> its chain's first spell id, from a CSV of first_spell_id, spell_id, rank (header optional)."""
    out: dict[int, int] = {}
    with open(path, newline="") as handle:
        for row in csv.reader(handle):
            if not row or not row[0].strip().lstrip("-").isdigit():
                continue
            out[int(row[1])] = int(row[0])
    return out


@dataclass
class Catalog:
    """A layout manifest's spell actions: first rank -> global action index, and the action names."""

    first_rank_action: dict[int, int]
    names: list[str]

    @staticmethod
    def from_manifest(path: str | Path) -> "Catalog":
        manifest = json.loads(Path(path).read_text())
        core = next(b for b in manifest["blocks"] if b["name"] == "core")
        offset = int(core["actions"][0])
        mapping = {int(entry["first_rank"]): offset + i for i, entry in enumerate(core.get("catalog", []))
                   if entry.get("kind") == "spell" and entry.get("first_rank")}
        return Catalog(mapping, list(manifest.get("action_names", [])))


def map_cast(spell: int, ranks: dict[int, int], catalog: Catalog) -> tuple[int | None, str, float]:
    """(global action, its name, confidence) of a cast spell."""
    first = ranks.get(int(spell), int(spell))
    action = catalog.first_rank_action.get(first)
    if action is None:
        return None, "", 0.0
    name = catalog.names[action] if action < len(catalog.names) else f"action_{action}"
    return action, name, 1.0


def selection_slot(target: int, units: np.ndarray) -> int | None:
    """The slot rank of a selected unit among the snapshot's units of the same reaction, nearest first."""
    hit = np.flatnonzero(units["unit"] == target)
    if len(hit) == 0:
        return None
    same = np.flatnonzero(units["reaction"] == units["reaction"][hit[0]])
    return int(np.flatnonzero(same == hit[0])[0])


# spell_ranks export.

def _env_file_value(path: Path, key: str) -> str | None:
    if not path.is_file():
        return None
    for line in path.read_text().splitlines():
        m = re.match(rf"\s*{re.escape(key)}\s*=\s*(.*)\s*$", line)
        if m:
            return m.group(1).strip().strip("'\"")
    return None


def parse_sql_dump(path: str | Path) -> list[tuple[int, int, int]]:
    """(first_spell_id, spell_id, rank) rows of an `INSERT INTO spell_ranks` dump."""
    text = Path(path).read_text()
    out = []
    for block in re.findall(r"INSERT INTO `?spell_ranks`?[^;]*?VALUES(.*?);", text, flags=re.S):
        out += [(int(a), int(b), int(c)) for a, b, c in re.findall(r"\((\d+),\s*(\d+),\s*(\d+)\)", block)]
    return out


def export_spell_ranks(out_csv: str | Path, container: str = DB_CONTAINER, env_file: str | Path | None = None,
                       sql_dump: str | Path | None = None, database: str = WORLD_DB) -> int:
    """Write spell_ranks to `out_csv` (first_spell_id, spell_id, rank); returns the row count. From an SQL dump
    when given, else from the forge DB container: the password is DOCKER_DB_ROOT_PASSWORD from the environment or
    `env_file`, passed through docker's environment (never the command line), or else the container's own
    MYSQL_ROOT_PASSWORD, read inside it."""
    if sql_dump is not None:
        rows = parse_sql_dump(sql_dump)
    else:
        env = dict(os.environ)
        password = env.get("DOCKER_DB_ROOT_PASSWORD") or (
            _env_file_value(Path(env_file), "DOCKER_DB_ROOT_PASSWORD") if env_file else None)
        query = f"SELECT first_spell_id, spell_id, `rank` FROM {database}.spell_ranks"
        cmd = ["docker", "exec"]
        if password:
            env["MYSQL_PWD"] = password
            cmd += ["-e", "MYSQL_PWD"]
        cmd += [container, "sh", "-c", 'MYSQL_PWD="${MYSQL_PWD:-$MYSQL_ROOT_PASSWORD}" exec mysql -uroot -N -B -e "$0"',
                query]
        done = subprocess.run(cmd, env=env, capture_output=True, text=True, check=True)
        rows = [tuple(int(v) for v in line.split("\t")) for line in done.stdout.splitlines() if line.strip()]
    out_csv = Path(out_csv)
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with open(out_csv, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["first_spell_id", "spell_id", "rank"])
        writer.writerows(rows)
    return len(rows)


# Validation.

def random_sequence(space: fit.Space, n: int, rng: np.random.Generator, mode: int = motion.MODE_GROUND
                    ) -> tuple[list[int], int]:
    """A policy-like movement sequence: mostly holding, sometimes a new bearing, a turn (none while one is under
    way), a halt, a facing mode or a jump; presses that would be no-ops (the held bearing again) are not made.
    Returns the actions and the start bearing."""
    index = {space.actions[i]: i for i in range(len(space.actions))}
    noop = index[(fit.NOOP, 0.0)]
    bearing = start = int(rng.integers(-1, fit.BEARINGS))
    face = fit.FACE_MODE_HOLD
    turning = 0
    jumping = 0
    out = []
    for _ in range(n):
        roll = rng.random()
        a = noop
        if jumping > 0:
            pass
        elif roll < 0.15:
            choices = [b for b in range(fit.BEARINGS) if b != bearing and not (face == fit.FACE_MODE_HEADING)]
            if choices:
                bearing = int(rng.choice(choices))
                a = index[(fit.BEARING, float(bearing))]
        elif roll < 0.30 and turning == 0 and face == fit.FACE_MODE_HOLD:
            turns = [v for k, v in space.actions if k == fit.TURN]
            if turns:
                angle = float(rng.choice(turns))
                a = index[(fit.TURN, angle)]
                turning = int(math.ceil(abs(angle) / space.turn_step - 1e-3))
        elif roll < 0.33 and bearing >= 0:
            a = index[(fit.HALT, 0.0)]
            bearing = -1
        elif roll < 0.35 and mode == motion.MODE_GROUND:
            a = index[(fit.JUMP, 0.0)]
            bearing = -1
            jumping = int(math.ceil(fit.JUMP_SECONDS / space.dt)) + 1
        elif roll < 0.37 and turning == 0:
            face = fit.FACE_MODE_HEADING if face == fit.FACE_MODE_HOLD else fit.FACE_MODE_HOLD
            a = index[(fit.FACE_HEADING if face == fit.FACE_MODE_HEADING else fit.FACE_HOLD, 0.0)]
            if face == fit.FACE_MODE_HEADING and bearing > 0:
                bearing = 0
        out.append(a)
        turning = max(0, turning - 1)
        jumping = max(0, jumping - 1)
        if face == fit.FACE_MODE_HEADING and bearing > 0:
            bearing = 0
    return out, start


def validate_movement(space: fit.Space | None = None, sequences: int = 20, length: int = 40, noise_yards: float = 0.0,
                      noise_radians: float = 0.0, seed: int = 0, beam: int = 32) -> dict:
    """Mapper accuracy on emulated sequences with known actions."""
    space = space or fit.SPACES["lattice"]
    rng = np.random.default_rng(seed)
    total = correct = pressed = pressed_ok = unobservable = 0
    confusions: dict[str, int] = {}
    for _ in range(sequences):
        acts, start_bearing = random_sequence(space, length, rng)
        n = len(acts) + 1
        start = {"x": float(rng.normal(0, 100)), "y": float(rng.normal(0, 100)),
                 "facing": float(rng.uniform(0, 2 * math.pi)), "bearing": start_bearing}
        path = fit.rollout(space, acts, start, np.zeros(n), np.full(n, 7.0))
        h = np.zeros((n, motion.SAMPLE_DIM))
        h[:, motion.T] = np.arange(n) * space.dt
        h[:, motion.X:motion.Z + 1] = path[:, :3] + rng.normal(0, noise_yards, (n, 3)) * (noise_yards > 0)
        h[:, motion.YAW] = path[:, 3] + rng.normal(0, noise_radians, n) * (noise_radians > 0)
        h[:, motion.SPEED] = 7.0
        jumps = np.array([space.actions[a][0] == fit.JUMP for a in acts])
        got = map_movement(h, space, beam=beam, start_bearing=start_bearing, jumps=jumps)
        truth = [move_local(space, a) for a in acts]
        noop = next(i for i, (k, _) in enumerate(space.actions) if k == fit.NOOP)
        for i, (t, g) in enumerate(zip(truth, got.local)):
            if t is not None:
                # A press with no effect on the motion (a facing mode with nothing to snap, say) cannot be seen.
                alt = fit.rollout(space, acts[:i] + [noop] + acts[i + 1:], start, np.zeros(n), np.full(n, 7.0))
                if np.abs(alt - path).max() < 1e-6:
                    unobservable += 1
                    continue
            total += 1
            correct += t == g
            if t is not None:
                pressed += 1
                pressed_ok += t == g
            if t != g:
                key = f"{t}->{g}"
                confusions[key] = confusions.get(key, 0) + 1
    return {"space": space.name, "decisions": total, "accuracy": correct / max(1, total),
            "press_accuracy": pressed_ok / max(1, pressed), "presses": pressed,
            "unobservable_presses": unobservable, "noise_yards": noise_yards,
            "noise_radians": noise_radians,
            "top_confusions": dict(sorted(confusions.items(), key=lambda kv: -kv[1])[:10]), "target": 0.95}


def validate_casts(ranks: dict[int, int], catalog: Catalog) -> dict:
    """Every spell in `ranks` whose chain the catalog holds maps to that chain's action, and a spell the catalog
    does not hold maps to none."""
    total = correct = 0
    inverse = {action: first for first, action in catalog.first_rank_action.items()}
    for spell, first in ranks.items():
        expected = catalog.first_rank_action.get(first)
        action, _, _ = map_cast(spell, ranks, catalog)
        total += 1
        correct += action == expected and (action is None or inverse[action] == first)
    return {"spells": total, "accuracy": correct / max(1, total), "target": 0.99}
