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
    10: ("<QQIHIHfffffIffffIB", "ms player client_ms opcode move_flags move_flags2 x y z o pitch fall_ms "
                                "jump_zspeed jump_sin jump_cos jump_xyspeed map source"),
    11: ("<QQfffffffff", "ms player walk run run_back swim swim_back flight flight_back turn_rate pitch_rate"),
    12: ("<QQBIfffI", "ms player event arg x y z map"),
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
