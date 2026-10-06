"""A test-only writer of capture records (FORMAT.md §2), written from the format document with `struct` -- not from
the reader's dtypes -- so a round trip checks the reader against the contract rather than against itself.

`CaptureWriter` builds one file's records; `write_capture` lays files out as mod-animus does
(`<dir>/<yyyy-mm-dd>/<hh>/<stream>-<map>.bin.gz`, one gzip member per flush, an index.json per hour).
"""

from __future__ import annotations

import gzip
import json
import math
import struct
from pathlib import Path

MAGIC = b"ANCAP\0\0\0"
STREAM_IDS = {"session": 1, "move": 2, "action": 3, "snapshot": 4, "outcome": 5, "companion": 6}

# type -> (struct format of the fixed fields, field names), straight from FORMAT.md.
FIXED = {
    1: ("<QQQBBBB3BHIIIHIB", "ms player session class_ race gender level tp0 tp1 tp2 item_level map zone area "
                             "latency_ms client_build kind"),
    3: ("<QQQB", "ms player session reason"),
    6: ("<QQH", "ms player latency_ms"),
    10: ("<QQIHIHfffffIffffIBI", "ms player client_ms opcode move_flags move_flags2 x y z o pitch fall_ms "
                                 "jump_zspeed jump_sin jump_cos jump_xyspeed map source server_ms"),
    11: ("<QQfffffffff", "ms player walk run run_back swim swim_back flight flight_back turn_rate pitch_rate"),
    12: ("<QQBIfffI", "ms player event arg x y z map"),
    14: ("<QQBII", "ms player kind sent kept"),
    15: ("<QIII", "ms map instance diff_ms"),
    13: ("<QQBBBBIIIIBB32s", "ms player kind class_ race level map zone mount form in_combat move_revision model"),
    20: ("<QQIQBfffBBIB", "ms player spell target target_kind tx ty tz gcd_active casting power power_type"),
    21: ("<QQIB", "ms player spell result"),
    22: ("<QQIB", "ms player spell how"),
    23: ("<QQQBf", "ms player target target_kind distance"),
    24: ("<QQIIQ", "ms player item spell target"),
    25: ("<QQQB", "ms player target start"),
    26: ("<QQBIQI", "ms player what entry target arg"),
    40: ("<QQQIIIIBB", "ms source target spell amount absorbed overkill school flags"),
    41: ("<QQQIIIB", "ms source target spell amount overheal flags"),
    42: ("<QQQIB", "ms killer victim victim_entry victim_kind"),
    43: ("<QQQBfffI", "ms player killer cause x y z map"),
    44: ("<QQIB", "ms player quest event"),
    45: ("<QQIIIB", "ms player map instance boss_entry event"),
    46: ("<QQQB", "ms player other event"),
    47: ("<QQIII", "ms player map zone area"),
    51: ("<QQQBI", "ms owner companion command arg"),
    52: ("<QQQbB", "ms owner companion rating reason"),
}
FIXED[2] = FIXED[1]
SELF = ("<fffffIffBBIfQI", "x y z o pitch map health_pct power_pct power_type flags casting_spell breath_pct target "
                           "shapeshift_form")
UNIT = ("<QIBfffffffffBBBIQf", "unit entry kind x y z o vx vy vz health_pct power_pct level reaction flags "
                               "casting_spell target threat_on_player")
AURA = ("<IBiB", "spell stacks remaining_ms positive")
COOLDOWN = ("<II", "spell remaining_ms")
MEMBER = ("<QBBBB", "unit class_ level role is_companion")


def _pack(spec: tuple[str, str], values: dict) -> bytes:
    fmt, names = spec
    out = []
    for name in names.split():
        if name in ("tp0", "tp1", "tp2"):
            out.append(values.get("tree_points", (0, 0, 0))[int(name[2])])
        else:
            out.append(values.get(name, 0))
    return struct.pack(fmt, *out)


def payload(rtype: int, values: dict) -> bytes:
    """One record's payload from field values (missing fields are 0)."""
    if rtype in FIXED:
        return _pack(FIXED[rtype], values)
    if rtype == 4:
        members = values.get("members", [])
        return (struct.pack("<QQBB", values.get("ms", 0), values.get("player", 0), values.get("kind", 0),
                            len(members)) + b"".join(_pack(MEMBER, m) for m in members))
    if rtype == 5:
        spells = values.get("spells", [])
        return struct.pack("<QQH", values.get("ms", 0), values.get("player", 0), len(spells)) + \
            b"".join(struct.pack("<I", s) for s in spells)
    if rtype == 30:
        units, auras, cds = values.get("units", []), values.get("auras", []), values.get("cooldowns", [])
        return (struct.pack("<QQ", values.get("ms", 0), values.get("player", 0)) + _pack(SELF, values)
                + bytes([len(units)]) + b"".join(_pack(UNIT, u) for u in units)
                + bytes([len(auras)]) + b"".join(_pack(AURA, a) for a in auras)
                + bytes([len(cds)]) + b"".join(_pack(COOLDOWN, c) for c in cds))
    if rtype == 50:
        actions = values.get("actions", [])
        model = values.get("model", "").encode()[:32].ljust(32, b"\0")
        return (struct.pack("<QQQ32sIH", values.get("ms", 0), values.get("companion", 0), values.get("owner", 0),
                            model, values.get("obs_hash", 0), len(actions))
                + b"".join(struct.pack("<H", a) for a in actions)
                + struct.pack("<HH", values.get("goal", 0), values.get("goal2", 0)))
    raise ValueError(f"unknown record type {rtype}")


def record(rtype: int, values: dict | None = None, raw: bytes | None = None, extra: bytes = b"") -> bytes:
    """A framed record; `raw` overrides the payload, `extra` is appended (a newer writer's added fields)."""
    body = (raw if raw is not None else payload(rtype, values or {})) + extra
    return struct.pack("<HH", rtype, len(body)) + body


def header(stream: str, opened_ms: int = 0, fmt: int = 1, magic: bytes = MAGIC) -> bytes:
    return record(0, raw=struct.pack("<8sHHQ40s40s", magic, fmt, STREAM_IDS[stream], opened_ms,
                                     b"modsha", b"realmsha"))


def write_gz(path: Path, members: list[bytes]) -> Path:
    """One gzip member per element, concatenated (one writer flush each)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"".join(gzip.compress(m) for m in members))
    return path


def hour_dir(root: Path, day: str, hour: int) -> Path:
    return Path(root) / day / f"{hour:02d}"


def write_index(directory: Path, hour_label: str, files: dict, players: int = 1, sessions: int = 1,
                dropped: dict | None = None, paused: dict | None = None) -> None:
    (directory / "index.json").write_text(json.dumps(
        {"format": 1, "hour": hour_label, "module_revision": "x", "realm_build": "y", "files": files,
         "players": players, "sessions": sessions, "dropped": dropped or {"move": 0, "snapshot": 0},
         "paused": paused or {"snapshot": False}}))


# Synthetic play.

FORWARD, BACKWARD, STRAFE_LEFT, STRAFE_RIGHT = 0x1, 0x2, 0x4, 0x8
FALLING, SWIMMING, FLYING = 0x1000, 0x200000, 0x02000000
MSG_MOVE_START_FORWARD, MSG_MOVE_STOP, MSG_MOVE_JUMP, MSG_MOVE_HEARTBEAT = 0x0B5, 0x0B7, 0x0BB, 0x0EE


def run_packets(player: int, t0_ms: int, points: list[tuple[float, float, float, float]], flags: int = FORWARD,
                every_ms: int = 100, mapid: int = 0, stop: bool = True, source: int = 0,
                opcode: int = MSG_MOVE_HEARTBEAT) -> list[bytes]:
    """Move records along `points` [(x, y, z, o)], one every `every_ms`, ending in a stop packet when `stop`."""
    out = []
    for i, (x, y, z, o) in enumerate(points):
        last = stop and i == len(points) - 1
        out.append(record(10, {"ms": t0_ms + i * every_ms, "player": player, "client_ms": 1000 + i * every_ms,
                               "opcode": MSG_MOVE_STOP if last else (MSG_MOVE_START_FORWARD if i == 0 else opcode),
                               "move_flags": 0 if last else flags, "x": x, "y": y, "z": z, "o": o, "map": mapid,
                               "source": source}))
    return out


def straight(x0: float, y0: float, heading: float, speed: float, seconds: float, every_ms: int = 100,
             z: float = 0.0, facing: float | None = None, turn_rate: float = 0.0) -> list[tuple]:
    """Points of a run at `speed` from (x0, y0), its course turning at `turn_rate` rad/s; facing follows the course
    unless fixed."""
    out = []
    x, y, h = x0, y0, heading
    n = int(round(seconds * 1000 / every_ms)) + 1
    dt = every_ms / 1000.0
    for i in range(n):
        out.append((x, y, z, (facing if facing is not None else h) % (2 * math.pi)))
        x += speed * dt * math.cos(h)
        y += speed * dt * math.sin(h)
        h += turn_rate * dt
    return out


T0 = 1_790_000_000_000      # 2026-09-21, server ms
HUMAN, COMPANION, OWNER_TARGET = 11, 77, 99


def synthetic_hour(root: Path, day: str = "2026-10-05", hour: int = 14, t0: int = T0) -> dict:
    """One hour shard of one map with one human (a level 23 frost mage) and one companion:

    stand 4 s at A -> run east 10 s at 7 yd/s (a jump at 3 s) -> stand 6 s at B (an interaction there) -> run north
    8 s while a snapshot says combat and damage flows -> stunned 1 s -> run on 4 s -> teleport -> run 3 s elsewhere.
    Returns the times and places the tests check against."""
    d = hour_dir(root, day, hour)
    moves: list[bytes] = [header("move", t0)]
    moves.append(record(11, {"ms": t0, "player": HUMAN, "walk": 2.5, "run": 7.0, "run_back": 4.5, "swim": 4.72,
                             "swim_back": 2.5, "flight": 7.0, "flight_back": 4.5, "turn_rate": 3.14,
                             "pitch_rate": 3.14}))
    t = t0
    # Standing at A: one stop packet, then nothing for 4 s.
    moves.append(record(10, {"ms": t, "player": HUMAN, "opcode": MSG_MOVE_STOP, "x": 0.0, "y": 0.0, "o": 0.0}))
    t += 4000
    east = straight(0.0, 0.0, 0.0, 7.0, 10.0)
    packets = run_packets(HUMAN, t, east)
    jump_at = t + 3000
    moves += packets
    moves.append(record(10, {"ms": jump_at, "player": HUMAN, "opcode": MSG_MOVE_JUMP, "move_flags": FORWARD,
                             "x": 21.0, "y": 0.0, "o": 0.0}))
    t += 10000
    b = east[-1]
    interact_ms = t + 2000
    t += 6000
    north_start = t
    north = straight(b[0], b[1], math.pi / 2, 7.0, 8.0)
    moves += run_packets(HUMAN, t, north, stop=False)
    t += 8000
    stun = (t + 100, t + 1100)
    moves.append(record(12, {"ms": stun[0], "player": HUMAN, "event": 10, "x": north[-1][0], "y": north[-1][1]}))
    moves.append(record(12, {"ms": stun[1], "player": HUMAN, "event": 11, "x": north[-1][0], "y": north[-1][1]}))
    on = straight(north[-1][0], north[-1][1], math.pi / 2, 7.0, 5.0)
    moves += run_packets(HUMAN, t + 100, on)
    t += 5100
    teleport_ms = t + 200
    moves.append(record(12, {"ms": teleport_ms, "player": HUMAN, "event": 5, "arg": 0}))
    far = straight(5000.0, 5000.0, 0.0, 7.0, 3.0)
    moves += run_packets(HUMAN, t + 400, far)
    # The companion: synthesised samples (source 1), never human data.
    moves += run_packets(COMPANION, t0, straight(10.0, 10.0, 0.0, 7.0, 20.0), source=1)
    write_gz(d / "move-0.bin.gz", [b"".join(moves[:5]), b"".join(moves[5:])])

    session = [header("session", t0),
               record(1, {"ms": t0, "player": HUMAN, "session": 1, "class_": 8, "race": 1, "level": 23,
                          "tree_points": (0, 2, 16), "latency_ms": 80, "kind": 0}),
               record(1, {"ms": t0, "player": COMPANION, "session": 2, "class_": 1, "level": 23, "kind": 1})]
    write_gz(d / "session-all.bin.gz", [b"".join(session)])

    snaps = [header("snapshot", t0)]
    for ms in range(north_start, north_start + 8000, 250):
        snaps.append(record(30, {"ms": ms, "player": HUMAN, "flags": 1}))
    for ms in range(north_start + 8000, north_start + 14000, 1000):
        snaps.append(record(30, {"ms": ms, "player": HUMAN, "flags": 0}))
    write_gz(d / "snapshot-0.bin.gz", [b"".join(snaps)])

    outcome = [header("outcome", t0)]
    for k in range(9):
        outcome.append(record(40, {"ms": north_start + k * 1000, "source": HUMAN, "target": OWNER_TARGET,
                                   "amount": 100}))
    outcome.append(record(41, {"ms": north_start + 2000, "source": HUMAN, "target": HUMAN, "amount": 300,
                               "overheal": 100}))
    outcome.append(record(43, {"ms": teleport_ms - 50, "player": HUMAN, "cause": 2, "x": 1.0, "y": 2.0, "z": 3.0}))
    write_gz(d / "outcome-0.bin.gz", [b"".join(outcome)])

    actions = [header("action", t0), record(26, {"ms": interact_ms, "player": HUMAN, "what": 6, "entry": 1})]
    for k in range(40):
        actions.append(record(20, {"ms": north_start + k * 2000, "player": HUMAN, "spell": 116}))
        actions.append(record(21, {"ms": north_start + k * 2000 + 10, "player": HUMAN, "spell": 116,
                                   "result": 255 if k % 4 else 173}))
    write_gz(d / "action-0.bin.gz", [b"".join(actions)])
    write_index(d, f"{day}T{hour:02d}", {}, players=2, sessions=2)
    return {"a": (0.0, 0.0), "b": (b[0], b[1]), "jump_ms": jump_at, "north_start": north_start, "stun": stun,
            "teleport_ms": teleport_ms, "interact_ms": interact_ms}
