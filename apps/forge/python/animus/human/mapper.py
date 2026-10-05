"""The inverse action mapper (plan §6.5): per decision, the nearest bot action to what a human did, with a confidence.

**Movement.** A clip on the decision grid is reproduced through the fit emulator (fit.beam_fit, the player
controller's held keys and mouse, segments of SEGMENT_SECONDS re-anchored on the human), and each decision's chosen
action is the mapping: a key or mouse-rate press, or no movement press at all. The action is reported as the move
block's local action index (MoveControls.h, revision 2: move_forward 0 ... walk_toggle 24; fit's action i + 1), or
None for no press.
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

# MoveControls.h: the move block's local actions (revision 2).
MOVE_ACTIONS = 25


def move_local(space: fit.Space, index: int) -> int | None:
    """The move block's local action for an emulator action: its index less one (the emulator's action 0 is no
    press, None)."""
    return None if space.actions[index][0] == fit.NOOP else index - 1


@dataclass
class MovementMapping:
    actions: list[int]              # emulator action per decision (space.actions index)
    local: list[int | None]         # the move block's local action per decision
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
                 lookahead: int = LOOKAHEAD, start_feet: tuple[int, int] | None = None,
                 jumps: np.ndarray | None = None, confidence: bool = True) -> MovementMapping:
    """Per decision of a clip (on space.dt), the nearest move action and its confidence. Where the clip begins
    the keys held are unknown and every state of the feet is tried, unless `start_feet` says (validation knows it).
    `jumps`
    ([K] bool per decision, a track's jump packets) pins where the jumps were. Without `confidence` (the costly part:
    every alternative replayed a few decisions on) the confidences are nan."""
    space = space or fit.SPACES["controller"]
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
        known = (start_feet,) if first == 0 and start_feet is not None else None
        got = fit.beam_fit(space, part, beam, known,
                           None if jumps is None else np.asarray(jumps[first:first + len(part) - 1], dtype=bool))
        errs += list(got.pos_err[1:])
        # Replay to each decision's state, and price every alternative there.
        state = fit.State.start(1, part[0, motion.X], part[0, motion.Y], part[0, motion.Z], part[0, motion.YAW],
                                part[0, motion.PITCH], got.start_feet)
        if not confidence:
            actions += got.actions
            conf += [float("nan")] * len(got.actions)
            continue
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
                    ) -> tuple[list[int], tuple[int, int]]:
    """A policy-like movement sequence: mostly holding, sometimes a key (forward, back, a strafe or their stops), a
    mouse rate, a walk toggle or a jump; presses that change nothing (the value already held, a stop with nothing
    held) are not made, nor any press mid-jump. Returns the actions and the feet's start state."""
    index = {space.actions[i]: i for i in range(len(space.actions))}
    noop = index[(fit.NOOP, 0.0)]
    forward, strafe = start = fit.FEET[int(rng.integers(len(fit.FEET)))]
    turn = 0.0
    jumping = 0
    out = []
    for _ in range(n):
        roll = rng.random()
        a = noop
        if jumping > 0:
            pass
        elif roll < 0.10:
            value = float(rng.choice([v for v in (1.0, -1.0, 0.0) if v != forward]))
            forward, a = value, index[(fit.FORWARD, value)]
        elif roll < 0.18:
            value = float(rng.choice([v for v in (-1.0, 1.0, 0.0) if v != strafe]))
            strafe, a = value, index[(fit.STRAFE, value)]
        elif roll < 0.33:
            rates = [v for k, v in space.actions if k == fit.TURN and abs(v - turn) > 1e-9]
            turn = float(rng.choice(rates))
            a = index[(fit.TURN, turn)]
        elif roll < 0.35 and (forward or strafe):
            a = index[(fit.WALK, 0.0)]
        elif roll < 0.37 and mode == motion.MODE_GROUND:
            a = index[(fit.JUMP, 0.0)]
            jumping = int(math.ceil(fit.JUMP_SECONDS / space.dt)) + 1
        out.append(a)
        jumping = max(0, jumping - 1)
    return out, (int(start[0]), int(start[1]))


def validate_movement(space: fit.Space | None = None, sequences: int = 20, length: int = 40, noise_yards: float = 0.0,
                      noise_radians: float = 0.0, seed: int = 0, beam: int = 32) -> dict:
    """Mapper accuracy on emulated sequences with known actions."""
    space = space or fit.SPACES["controller"]
    rng = np.random.default_rng(seed)
    total = correct = pressed = pressed_ok = unobservable = 0
    confusions: dict[str, int] = {}
    for _ in range(sequences):
        acts, start_feet = random_sequence(space, length, rng)
        n = len(acts) + 1
        start = {"x": float(rng.normal(0, 100)), "y": float(rng.normal(0, 100)),
                 "facing": float(rng.uniform(0, 2 * math.pi)), "feet": start_feet}
        path = fit.rollout(space, acts, start, np.zeros(n), np.full(n, 7.0))
        h = np.zeros((n, motion.SAMPLE_DIM))
        h[:, motion.T] = np.arange(n) * space.dt
        h[:, motion.X:motion.Z + 1] = path[:, :3] + rng.normal(0, noise_yards, (n, 3)) * (noise_yards > 0)
        h[:, motion.YAW] = path[:, 3] + rng.normal(0, noise_radians, n) * (noise_radians > 0)
        h[:, motion.SPEED] = 7.0
        jumps = np.array([space.actions[a][0] == fit.JUMP for a in acts])
        got = map_movement(h, space, beam=beam, start_feet=start_feet, jumps=jumps)
        truth = [move_local(space, a) for a in acts]
        noop = next(i for i, (k, _) in enumerate(space.actions) if k == fit.NOOP)
        for i, (t, g) in enumerate(zip(truth, got.local)):
            if t is not None:
                # A press with no effect on the motion (a walk toggle while standing, say) cannot be seen.
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
