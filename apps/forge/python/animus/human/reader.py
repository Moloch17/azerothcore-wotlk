"""Reading capture files (FORMAT.md §1-2): framing, record types, gzip members, the hourly layout and its index.

A capture directory holds `<yyyy-mm-dd>/<hh>/<stream>-<map>.bin.gz` files, each a sequence of gzip members whose
concatenated payload is a sequence of framed records (`u16 type, u16 length, payload`). The realm may have many
players and the files are kept forever, so nothing here loads more than a bounded block of one file at a time:
`read_file` decompresses member by member and yields `Batch`es of at most `block_bytes` of records, and
`CaptureDir.iter_stream` walks a date/hour range file by file.

Every record type has a fixed-size **prefix** (a numpy structured dtype), parsed for a whole batch at once: a Move
file of millions of packets is a handful of vectorised gathers, not a Python loop per record. Four types carry a
variable tail after their prefix (GroupState's members, KnownSpells' ids, Snapshot's units/auras/cooldowns and
CompanionDecision's actions); the tails of the small streams are parsed eagerly, a Snapshot's only on request
(`snapshot_details`), since the Self part that the track builder needs is all in the prefix.

Forward compatibility, as FORMAT.md promises it: a record type the reader does not know is skipped by its length,
and a record longer than its known prefix keeps the prefix and skips the rest. A record shorter than its prefix is
malformed and counted (`Batch.malformed`), never guessed at.

Truncation: a file cut short by a crash loses its last gzip member. A member that never reaches its end is dropped
whole (its partial output is not trusted) and counted in `FileStats.truncated_members`; a plain `.bin` that ends
inside a record drops that record (`FileStats.trailing_bytes`). Records are allowed to straddle a member boundary:
the bytes left over at the end of one member are carried into the next.
"""

from __future__ import annotations

import datetime as dt
import json
import struct
import warnings
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

import numpy as np

FORMAT_VERSION = 3
MAGIC = b"ANCAP\0\0\0"
FRAME = struct.Struct("<HH")
BLOCK_BYTES = 32 << 20          # decompressed bytes framed and parsed at once
READ_BYTES = 1 << 20            # compressed bytes read from disk at once

STREAMS = {1: "session", 2: "move", 3: "action", 4: "snapshot", 5: "outcome", 6: "companion"}
STREAM_IDS = {name: sid for sid, name in STREAMS.items()}


class CaptureError(ValueError):
    """A capture file that is not one (bad magic, a format older than any reader knows)."""


# Record types (FORMAT.md §2).
FILE_HEADER = 0
SESSION_START, SESSION_CONTEXT, SESSION_END, GROUP_STATE, KNOWN_SPELLS, LATENCY = 1, 2, 3, 4, 5, 6
MOVE, SPEEDS, MOTION_EVENT, MOVER_STATE, MOVE_TALLY, MAP_UPDATE = 10, 11, 12, 13, 14, 15

# Move `source` (FORMAT.md §2.3): a player's client packet; format 1's synthesised companion sample (never written by
# format 2); a companion's player controller packet, through its session's movement handlers (format 2).
SOURCE_CLIENT, SOURCE_COMPANION_SAMPLE, SOURCE_CONTROLLER = 0, 1, 2
CAST_REQUEST, CAST_RESULT, CAST_END, SELECT, ITEM_USE, ATTACK, INTERACT = 20, 21, 22, 23, 24, 25, 26
SNAPSHOT = 30
DAMAGE, HEAL, KILL, DEATH, QUEST, ENCOUNTER, PVP, AREA = 40, 41, 42, 43, 44, 45, 46, 47
COMPANION_DECISION, COMPANION_COMMAND, COMPANION_RATING = 50, 51, 52

# MotionEvent `event` (FORMAT.md §2.3).
EV_MOUNT, EV_DISMOUNT, EV_TAXI_START, EV_TAXI_END, EV_TELEPORT, EV_DEATH, EV_RESURRECT = 1, 2, 3, 4, 5, 6, 7
EV_ROOT, EV_UNROOT, EV_STUN_START, EV_STUN_END, EV_FEAR_START, EV_FEAR_END, EV_KNOCKBACK = 8, 9, 10, 11, 12, 13, 14
EV_LOADING_START, EV_LOADING_END, EV_SHAPESHIFT = 15, 16, 17
EV_VEHICLE_ENTER, EV_VEHICLE_EXIT, EV_TRANSPORT_BOARD, EV_TRANSPORT_LEAVE = 18, 19, 20, 21

_SESSION = [("ms", "<u8"), ("player", "<u8"), ("session", "<u8"), ("class_", "u1"), ("race", "u1"),
            ("gender", "u1"), ("level", "u1"), ("tree_points", "u1", (3,)), ("item_level", "<u2"),
            ("map", "<u4"), ("zone", "<u4"), ("area", "<u4"), ("latency_ms", "<u2"), ("client_build", "<u4"),
            ("kind", "u1")]
_SELF = [("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("o", "<f4"), ("pitch", "<f4"), ("map", "<u4"),
         ("health_pct", "<f4"), ("power_pct", "<f4"), ("power_type", "u1"), ("flags", "u1"),
         ("casting_spell", "<u4"), ("breath_pct", "<f4"), ("target", "<u8"), ("shapeshift_form", "<u4")]

# The fixed prefix of every record type: (name, dtype). `class_` because `class` is a keyword.
PREFIX: dict[int, tuple[str, np.dtype]] = {
    FILE_HEADER: ("FileHeader", [("magic", "S8"), ("format", "<u2"), ("stream", "<u2"), ("opened_ms", "<u8"),
                                 ("module_revision", "S40"), ("realm_build", "S40")]),
    SESSION_START: ("SessionStart", _SESSION),
    SESSION_CONTEXT: ("SessionContext", _SESSION),
    SESSION_END: ("SessionEnd", [("ms", "<u8"), ("player", "<u8"), ("session", "<u8"), ("reason", "u1")]),
    GROUP_STATE: ("GroupState", [("ms", "<u8"), ("player", "<u8"), ("kind", "u1"), ("count", "u1")]),
    KNOWN_SPELLS: ("KnownSpells", [("ms", "<u8"), ("player", "<u8"), ("count", "<u2")]),
    LATENCY: ("Latency", [("ms", "<u8"), ("player", "<u8"), ("latency_ms", "<u2")]),
    MOVE: ("Move", [("ms", "<u8"), ("player", "<u8"), ("client_ms", "<u4"), ("opcode", "<u2"),
                    ("move_flags", "<u4"), ("move_flags2", "<u2"), ("x", "<f4"), ("y", "<f4"), ("z", "<f4"),
                    ("o", "<f4"), ("pitch", "<f4"), ("fall_ms", "<u4"), ("jump_zspeed", "<f4"),
                    ("jump_sin", "<f4"), ("jump_cos", "<f4"), ("jump_xyspeed", "<f4"), ("map", "<u4"),
                    ("source", "u1"), ("server_ms", "<u4")]),
    SPEEDS: ("Speeds", [("ms", "<u8"), ("player", "<u8"), ("walk", "<f4"), ("run", "<f4"), ("run_back", "<f4"),
                        ("swim", "<f4"), ("swim_back", "<f4"), ("flight", "<f4"), ("flight_back", "<f4"),
                        ("turn_rate", "<f4"), ("pitch_rate", "<f4")]),
    MOTION_EVENT: ("MotionEvent", [("ms", "<u8"), ("player", "<u8"), ("event", "u1"), ("arg", "<u4"),
                                   ("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("map", "<u4")]),
    MOVER_STATE: ("MoverState", [("ms", "<u8"), ("player", "<u8"), ("kind", "u1"), ("class_", "u1"), ("race", "u1"),
                                 ("level", "u1"), ("map", "<u4"), ("zone", "<u4"), ("mount", "<u4"), ("form", "<u4"),
                                 ("in_combat", "u1"), ("move_revision", "u1"), ("model", "S32")]),
    MOVE_TALLY: ("MoveTally", [("ms", "<u8"), ("player", "<u8"), ("kind", "u1"), ("sent", "<u4"), ("kept", "<u4")]),
    MAP_UPDATE: ("MapUpdate", [("ms", "<u8"), ("map", "<u4"), ("instance", "<u4"), ("diff_ms", "<u4")]),
    CAST_REQUEST: ("CastRequest", [("ms", "<u8"), ("player", "<u8"), ("spell", "<u4"), ("target", "<u8"),
                                   ("target_kind", "u1"), ("tx", "<f4"), ("ty", "<f4"), ("tz", "<f4"),
                                   ("gcd_active", "u1"), ("casting", "u1"), ("power", "<u4"),
                                   ("power_type", "u1")]),
    CAST_RESULT: ("CastResult", [("ms", "<u8"), ("player", "<u8"), ("spell", "<u4"), ("result", "u1")]),
    CAST_END: ("CastEnd", [("ms", "<u8"), ("player", "<u8"), ("spell", "<u4"), ("how", "u1")]),
    SELECT: ("Select", [("ms", "<u8"), ("player", "<u8"), ("target", "<u8"), ("target_kind", "u1"),
                        ("distance", "<f4")]),
    ITEM_USE: ("ItemUse", [("ms", "<u8"), ("player", "<u8"), ("item", "<u4"), ("spell", "<u4"),
                           ("target", "<u8")]),
    ATTACK: ("Attack", [("ms", "<u8"), ("player", "<u8"), ("target", "<u8"), ("start", "u1")]),
    INTERACT: ("Interact", [("ms", "<u8"), ("player", "<u8"), ("what", "u1"), ("entry", "<u4"), ("target", "<u8"),
                            ("arg", "<u4")]),
    SNAPSHOT: ("Snapshot", [("ms", "<u8"), ("player", "<u8")] + _SELF),
    DAMAGE: ("Damage", [("ms", "<u8"), ("source", "<u8"), ("target", "<u8"), ("spell", "<u4"), ("amount", "<u4"),
                        ("absorbed", "<u4"), ("overkill", "<u4"), ("school", "u1"), ("flags", "u1")]),
    HEAL: ("Heal", [("ms", "<u8"), ("source", "<u8"), ("target", "<u8"), ("spell", "<u4"), ("amount", "<u4"),
                    ("overheal", "<u4"), ("flags", "u1")]),
    KILL: ("Kill", [("ms", "<u8"), ("killer", "<u8"), ("victim", "<u8"), ("victim_entry", "<u4"),
                    ("victim_kind", "u1")]),
    DEATH: ("Death", [("ms", "<u8"), ("player", "<u8"), ("killer", "<u8"), ("cause", "u1"), ("x", "<f4"),
                      ("y", "<f4"), ("z", "<f4"), ("map", "<u4")]),
    QUEST: ("Quest", [("ms", "<u8"), ("player", "<u8"), ("quest", "<u4"), ("event", "u1")]),
    ENCOUNTER: ("Encounter", [("ms", "<u8"), ("player", "<u8"), ("map", "<u4"), ("instance", "<u4"),
                              ("boss_entry", "<u4"), ("event", "u1")]),
    PVP: ("PvP", [("ms", "<u8"), ("player", "<u8"), ("other", "<u8"), ("event", "u1")]),
    AREA: ("Area", [("ms", "<u8"), ("player", "<u8"), ("map", "<u4"), ("zone", "<u4"), ("area", "<u4")]),
    COMPANION_DECISION: ("CompanionDecision", [("ms", "<u8"), ("companion", "<u8"), ("owner", "<u8"),
                                               ("model", "S32"), ("obs_hash", "<u4"), ("n_actions", "<u2")]),
    COMPANION_COMMAND: ("CompanionCommand", [("ms", "<u8"), ("owner", "<u8"), ("companion", "<u8"),
                                             ("command", "u1"), ("arg", "<u4")]),
    COMPANION_RATING: ("CompanionRating", [("ms", "<u8"), ("owner", "<u8"), ("companion", "<u8"), ("rating", "i1"),
                                           ("reason", "u1")]),
}
PREFIX = {rtype: (name, np.dtype(fields)) for rtype, (name, fields) in PREFIX.items()}
TYPE_OF = {name: rtype for rtype, (name, _) in PREFIX.items()}
# Fields a later format appended to a record, which an older file's record lacks: read as 0 there. The record's
# shortest valid payload is its prefix without them.
APPENDED: dict[int, tuple[str, ...]] = {MOVE: ("server_ms",)}       # format 3
MIN_LENGTH = {rtype: PREFIX[rtype][1].itemsize - sum(PREFIX[rtype][1].fields[name][0].itemsize for name in names)
              for rtype, names in APPENDED.items()}

# Variable tails (FORMAT.md §2.2, §2.5, §2.7).
GROUP_MEMBER = np.dtype([("unit", "<u8"), ("class_", "u1"), ("level", "u1"), ("role", "u1"),
                         ("is_companion", "u1")])
SNAPSHOT_UNIT = np.dtype([("unit", "<u8"), ("entry", "<u4"), ("kind", "u1"), ("x", "<f4"), ("y", "<f4"),
                          ("z", "<f4"), ("o", "<f4"), ("vx", "<f4"), ("vy", "<f4"), ("vz", "<f4"),
                          ("health_pct", "<f4"), ("power_pct", "<f4"), ("level", "u1"), ("reaction", "u1"),
                          ("flags", "u1"), ("casting_spell", "<u4"), ("target", "<u8"),
                          ("threat_on_player", "<f4")])
SNAPSHOT_AURA = np.dtype([("spell", "<u4"), ("stacks", "u1"), ("remaining_ms", "<i4"), ("positive", "u1")])
SNAPSHOT_COOLDOWN = np.dtype([("spell", "<u4"), ("remaining_ms", "<u4")])

# Snapshot Self flags.
SELF_COMBAT, SELF_SWIMMING, SELF_FLYING, SELF_MOUNTED, SELF_CASTING, SELF_DEAD, SELF_FALLING = 1, 2, 4, 8, 16, 32, 64


def text(raw: bytes) -> str:
    """A fixed char[] field: up to the first NUL, as UTF-8 (undecodable bytes replaced)."""
    return bytes(raw).split(b"\0", 1)[0].decode("utf-8", "replace")


@dataclass
class Batch:
    """A block of one file's records: per record type, the prefixes as a structured array in file order, plus the
    parsed tails of the small variable streams and enough of the raw block to parse a Snapshot's tail later."""

    records: dict[int, np.ndarray] = field(default_factory=dict)
    tails: dict[int, list] = field(default_factory=dict)      # GroupState / KnownSpells / CompanionDecision
    unknown: dict[int, int] = field(default_factory=dict)     # record type -> count skipped
    malformed: int = 0
    _raw: bytes = b""
    _snapshot_spans: np.ndarray | None = None                 # [N, 2] payload offset and length of each Snapshot

    def get(self, rtype: int) -> np.ndarray:
        """The prefixes of one record type (an empty array of its dtype when the batch has none)."""
        found = self.records.get(rtype)
        return found if found is not None else np.zeros(0, dtype=PREFIX[rtype][1])

    def snapshot_details(self, index: int) -> dict[str, np.ndarray]:
        """The units, auras and cooldowns of the batch's `index`-th Snapshot (a malformed tail reads as empty)."""
        empty = {"units": np.zeros(0, SNAPSHOT_UNIT), "auras": np.zeros(0, SNAPSHOT_AURA),
                 "cooldowns": np.zeros(0, SNAPSHOT_COOLDOWN)}
        if self._snapshot_spans is None:
            return empty
        start, length = (int(v) for v in self._snapshot_spans[index])
        end = start + length
        pos = start + PREFIX[SNAPSHOT][1].itemsize
        out = {}
        for key, dtype in (("units", SNAPSHOT_UNIT), ("auras", SNAPSHOT_AURA), ("cooldowns", SNAPSHOT_COOLDOWN)):
            if pos + 1 > end:
                return empty
            count = self._raw[pos]
            pos += 1
            size = count * dtype.itemsize
            if pos + size > end:
                return empty
            out[key] = np.frombuffer(self._raw, dtype=dtype, count=count, offset=pos).copy()
            pos += size
        return out


@dataclass
class FileStats:
    """What reading one file found besides its records."""

    path: Path
    header: dict | None = None
    records: int = 0
    unknown: int = 0
    malformed: int = 0
    truncated_members: int = 0
    trailing_bytes: int = 0


def frame(buf: bytes, start: int = 0) -> tuple[np.ndarray, int]:
    """Walk the record framing of `buf` from `start`: runs of consecutive records of one type and length, as rows
    of (type, payload offset of the first, length, count), and the end of the last whole record. Most of a Move
    file is a few long runs, found a chunk at a time with numpy rather than one header at a time."""
    n = len(buf)
    runs: list[tuple[int, int, int, int]] = []
    pos = start
    chunk = 64
    while pos + 4 <= n:
        rtype, length = FRAME.unpack_from(buf, pos)
        size = 4 + length
        if pos + size > n:
            break
        avail = (n - pos) // size
        run = 1
        if avail > 1:
            k = min(avail, chunk)
            heads = np.ndarray((k,), dtype=np.dtype({"names": ["t", "l"], "formats": ["<u2", "<u2"],
                                                     "offsets": [0, 2], "itemsize": size}),
                               buffer=buf, offset=pos)
            same = (heads["t"] == rtype) & (heads["l"] == length)
            run = k if same.all() else int(np.argmin(same))
            chunk = min(chunk * 2, 1 << 16) if run == k else 64
        if runs and runs[-1][0] == rtype and runs[-1][2] == length \
                and runs[-1][1] + runs[-1][3] * size == pos + 4:
            last = runs[-1]
            runs[-1] = (rtype, last[1], length, last[3] + run)
        else:
            runs.append((rtype, pos + 4, length, run))
        pos += run * size
    return np.asarray(runs, dtype=np.int64).reshape(-1, 4), pos


GATHER_ROWS = 1 << 16       # records gathered at once from short runs (bounds the index array)


def _prefix_rows(buf: bytes, data: np.ndarray, dtype: np.dtype, runs: np.ndarray) -> np.ndarray:
    """The prefixes of the records in `runs` (all of one type, each long enough), packed and in file order. A long
    run is read through a strided view of the buffer; short ones are gathered byte-wise in bounded chunks."""
    parts: list[np.ndarray] = []
    singles: list[np.ndarray] = []

    def gather() -> None:
        if not singles:
            return
        offs = np.concatenate(singles)
        singles.clear()
        for first in range(0, len(offs), GATHER_ROWS):
            rows = offs[first:first + GATHER_ROWS]
            block = data[rows[:, None] + np.arange(dtype.itemsize)[None, :]]
            parts.append(np.ascontiguousarray(block).view(dtype).reshape(-1))

    for _, offset, length, count in runs:
        if count >= 8:
            gather()
            strided = np.dtype({"names": list(dtype.names),
                                "formats": [dtype.fields[name][0] for name in dtype.names],
                                "offsets": [dtype.fields[name][1] + 4 for name in dtype.names],
                                "itemsize": int(length) + 4})
            # From the run's first frame header, so the last item ends where the last record does.
            view = np.ndarray((int(count),), dtype=strided, buffer=buf, offset=int(offset) - 4)
            parts.append(view.astype(dtype))
        else:
            singles.append(int(offset) + np.arange(int(count), dtype=np.int64) * (int(length) + 4))
    gather()
    return np.concatenate(parts) if parts else np.zeros(0, dtype)


def _rows_with_appended(buf: bytes, data: np.ndarray, rtype: int, dtype: np.dtype, runs: np.ndarray,
                        older: np.ndarray) -> np.ndarray:
    """Rows of a type whose older records lack its APPENDED fields (0 there), in file order."""
    names = [name for name in dtype.names if name not in APPENDED[rtype]]
    short = np.dtype({"names": names, "formats": [dtype.fields[n][0] for n in names],
                      "offsets": [dtype.fields[n][1] for n in names], "itemsize": MIN_LENGTH[rtype]})
    parts = []
    for run, old in zip(runs, older):
        one = run[None, :]
        if old:
            part = np.zeros(int(run[3]), dtype)
            rows = _prefix_rows(buf, data, short, one)
            for name in names:
                part[name] = rows[name]
            parts.append(part)
        else:
            parts.append(_prefix_rows(buf, data, dtype, one))
    return np.concatenate(parts)


def _offsets(runs: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Every record's payload offset and length from runs."""
    if len(runs) == 0:
        return np.zeros(0, np.int64), np.zeros(0, np.int64)
    counts = runs[:, 3]
    first = np.repeat(runs[:, 1], counts)
    within = np.arange(int(counts.sum())) - np.repeat(np.cumsum(counts) - counts, counts)
    lengths = np.repeat(runs[:, 2], counts)
    return first + within * (lengths + 4), lengths


def parse(buf: bytes, runs: np.ndarray) -> Batch:
    """The records framed in `buf`, grouped by type into structured arrays (file order kept within a type)."""
    batch = Batch(_raw=buf)
    data = np.frombuffer(buf, dtype=np.uint8)
    for rtype in np.unique(runs[:, 0]):
        rtype = int(rtype)
        mine = runs[runs[:, 0] == rtype]
        if rtype not in PREFIX:
            batch.unknown[rtype] = int(mine[:, 3].sum())
            continue
        dtype = PREFIX[rtype][1]
        ok = mine[:, 2] >= MIN_LENGTH.get(rtype, dtype.itemsize)
        batch.malformed += int(mine[~ok, 3].sum())
        mine = mine[ok]
        if len(mine) == 0:
            continue
        older = mine[:, 2] < dtype.itemsize
        if older.any():
            batch.records[rtype] = _rows_with_appended(buf, data, rtype, dtype, mine, older)
        else:
            batch.records[rtype] = _prefix_rows(buf, data, dtype, mine)
        if rtype == SNAPSHOT:
            offs, lens = _offsets(mine)
            batch._snapshot_spans = np.stack([offs, lens], axis=1)
        elif rtype in (GROUP_STATE, KNOWN_SPELLS, COMPANION_DECISION):
            offs, lens = _offsets(mine)
            batch.tails[rtype] = [_tail(buf, rtype, int(o), int(n), dtype.itemsize) for o, n in zip(offs, lens)]
    return batch


def _tail(buf: bytes, rtype: int, offset: int, length: int, prefix: int) -> np.ndarray:
    """The variable part of a GroupState (members), KnownSpells (spell ids) or CompanionDecision (actions, then
    goal and goal2 as the last two entries). Cut to what the record really holds when its count overstates it."""
    pos, end = offset + prefix, offset + length
    if rtype == GROUP_STATE:
        count = buf[pos - 1]
        count = min(count, (end - pos) // GROUP_MEMBER.itemsize)
        return np.frombuffer(buf, dtype=GROUP_MEMBER, count=count, offset=pos).copy()
    count = struct.unpack_from("<H", buf, pos - 2)[0]
    if rtype == KNOWN_SPELLS:
        count = min(count, (end - pos) // 4)
        return np.frombuffer(buf, dtype="<u4", count=count, offset=pos).astype(np.uint32)
    # CompanionDecision: n_actions x u16 action, u16 goal, u16 goal2.
    count = min(count + 2, (end - pos) // 2)
    return np.frombuffer(buf, dtype="<u2", count=count, offset=pos).astype(np.uint16)


def _members(path: Path) -> Iterator[tuple[bytes, bool]]:
    """Decompressed gzip members of `path` (a plain `.bin` is read in chunks, each 'complete'): (data, complete).
    A member the file ends inside, or a corrupt one, comes back as (b"", False) and ends the file."""
    with open(path, "rb") as handle:
        if not path.name.endswith(".gz"):
            while chunk := handle.read(READ_BYTES * 8):
                yield chunk, True
            return
        pending = b""
        decoder = None
        out: list[bytes] = []
        while True:
            if not pending:
                pending = handle.read(READ_BYTES)
                if not pending:
                    break
            if decoder is None:
                decoder, out = zlib.decompressobj(wbits=31), []
            try:
                out.append(decoder.decompress(pending))
            except zlib.error:
                yield b"", False
                return
            if decoder.eof:
                yield b"".join(out), True
                pending, decoder = decoder.unused_data, None
            else:
                pending = b""
        if decoder is not None:
            yield b"", False                    # a member cut short: dropped whole


def read_file(path: str | Path, stats: FileStats | None = None, block_bytes: int = BLOCK_BYTES
              ) -> Iterator[Batch]:
    """The records of one capture file in blocks of about `block_bytes` decompressed bytes. The FileHeader is
    checked (magic, format) and left out of the batches; `stats` (when given) collects it and the counts."""
    path = Path(path)
    stats = stats if stats is not None else FileStats(path)
    carry = b""
    header_seen = False
    pieces: list[bytes] = []
    size = 0

    def flush(final: bool) -> Iterator[Batch]:
        nonlocal carry, pieces, size, header_seen
        buf = carry + b"".join(pieces)
        pieces, size = [], 0
        start = 0
        if not header_seen and len(buf) >= 4:
            rtype, length = FRAME.unpack_from(buf, 0)
            if rtype != FILE_HEADER:
                raise CaptureError(f"{path}: no FileHeader (first record type {rtype})")
            if len(buf) < 4 + length:
                carry = buf
                return
            stats.header = _check_header(path, buf[4:4 + length])
            header_seen = True
            start = 4 + length
        runs, end = frame(buf, start)
        carry = buf[end:]
        if final and carry:
            stats.trailing_bytes += len(carry)
            carry = b""
        if len(runs):
            batch = parse(buf, runs)
            stats.records += int(sum(len(v) for v in batch.records.values()))
            stats.unknown += int(sum(batch.unknown.values()))
            stats.malformed += batch.malformed
            yield batch

    for data, complete in _members(path):
        if not complete:
            stats.truncated_members += 1
            continue
        pieces.append(data)
        size += len(data)
        if size >= block_bytes:
            yield from flush(False)
    yield from flush(True)
    if not header_seen and stats.trailing_bytes == 0 and stats.truncated_members == 0:
        raise CaptureError(f"{path}: empty capture file")


def _check_header(path: Path, payload: bytes) -> dict:
    dtype = PREFIX[FILE_HEADER][1]
    if len(payload) < dtype.itemsize:
        raise CaptureError(f"{path}: FileHeader too short ({len(payload)} bytes)")
    head = np.frombuffer(payload, dtype=dtype, count=1)[0]
    if bytes(head["magic"]).ljust(8, b"\0") != MAGIC:
        raise CaptureError(f"{path}: bad magic {bytes(head['magic'])!r}")
    version = int(head["format"])
    if version < 1:
        raise CaptureError(f"{path}: format {version}")
    if version > FORMAT_VERSION:
        warnings.warn(f"{path}: format {version} is newer than this reader ({FORMAT_VERSION}); reading the "
                      f"fields it knows", stacklevel=3)
    return {"format": version, "stream": STREAMS.get(int(head["stream"]), str(int(head["stream"]))),
            "opened_ms": int(head["opened_ms"]), "module_revision": text(head["module_revision"]),
            "realm_build": text(head["realm_build"])}


def merge(batches: list[Batch]) -> Batch:
    """One batch holding every record of `batches` (prefixes concatenated per type; Snapshot tails are lost)."""
    out = Batch()
    for rtype in {r for b in batches for r in b.records}:
        out.records[rtype] = np.concatenate([b.records[rtype] for b in batches if rtype in b.records])
    for rtype in {r for b in batches for r in b.tails}:
        out.tails[rtype] = [t for b in batches for t in b.tails.get(rtype, [])]
    for b in batches:
        for rtype, count in b.unknown.items():
            out.unknown[rtype] = out.unknown.get(rtype, 0) + count
        out.malformed += b.malformed
    return out


def read_all(path: str | Path, stats: FileStats | None = None) -> Batch:
    """All of one file's records as a single batch. For one hour shard only: never a whole capture."""
    return merge(list(read_file(path, stats)))


# The directory layout (FORMAT.md §1).

def parse_hour(value: str | None, end: bool = False) -> dt.datetime | None:
    """`yyyy-mm-dd`, `yyyy-mm-ddThh` or `yyyy-mm-dd hh` -> a UTC hour; a bare date is its first hour (or, for the end
    of a range, its last)."""
    if value is None:
        return None
    value = value.strip().replace(" ", "T")
    if "T" in value:
        day, hour = value.split("T", 1)
        return dt.datetime.strptime(f"{day}T{int(hour[:2]):02d}", "%Y-%m-%dT%H")
    day = dt.datetime.strptime(value, "%Y-%m-%d")
    return day + dt.timedelta(hours=23) if end else day


@dataclass
class HourDir:
    hour: dt.datetime
    path: Path

    @property
    def label(self) -> str:
        return self.hour.strftime("%Y-%m-%dT%H")

    def files(self, stream: str | None = None) -> list[Path]:
        """The hour's capture files of one stream (or all), sorted by name."""
        out = []
        for path in sorted(self.path.iterdir()):
            if not (path.name.endswith(".bin.gz") or path.name.endswith(".bin")):
                continue
            if stream is None or path.name.split("-", 1)[0] == stream:
                out.append(path)
        return out

    def index(self) -> dict | None:
        """The hour's index.json, or None while the hour is still open (or its index is unreadable)."""
        path = self.path / "index.json"
        if not path.is_file():
            return None
        try:
            return json.loads(path.read_text())
        except (OSError, json.JSONDecodeError):
            return None


def stream_map(path: Path) -> tuple[str, str]:
    """`move-0.bin.gz` -> ("move", "0"); `session-all.bin.gz` -> ("session", "all")."""
    name = path.name.removesuffix(".gz").removesuffix(".bin")
    stream, _, shard = name.partition("-")
    return stream, shard


class CaptureDir:
    """A capture directory, walked hour by hour within an optional [start, end] range (inclusive, UTC)."""

    def __init__(self, root: str | Path, start: str | None = None, end: str | None = None):
        self.root = Path(root)
        self.start = parse_hour(start)
        self.end = parse_hour(end, end=True)

    def hours(self) -> list[HourDir]:
        out = []
        if not self.root.is_dir():
            return out
        for day in sorted(self.root.iterdir()):
            if not day.is_dir():
                continue
            try:
                date = dt.datetime.strptime(day.name, "%Y-%m-%d")
            except ValueError:
                continue
            for hour in sorted(day.iterdir()):
                if not hour.is_dir() or not hour.name.isdigit():
                    continue
                when = date + dt.timedelta(hours=int(hour.name))
                if (self.start and when < self.start) or (self.end and when > self.end):
                    continue
                out.append(HourDir(when, hour))
        return out

    def files(self, stream: str) -> list[tuple[HourDir, Path]]:
        return [(hour, path) for hour in self.hours() for path in hour.files(stream)]

    def iter_stream(self, stream: str, stats: list[FileStats] | None = None
                    ) -> Iterator[tuple[HourDir, Path, Batch]]:
        """Every batch of one stream across the range, file by file (hour order, then file name)."""
        for hour, path in self.files(stream):
            file_stats = FileStats(path)
            for batch in read_file(path, file_stats):
                yield hour, path, batch
            if stats is not None:
                stats.append(file_stats)

    def read_hour(self, hour: HourDir, stream: str) -> Batch:
        """One stream's records for one hour, across its map shards. Fine for the small streams (session,
        companion, action); the move and snapshot streams are better read per shard."""
        batches = [batch for path in hour.files(stream) for batch in read_file(path)]
        return merge(batches)

    def index_report(self) -> dict:
        """Dropped and paused streams, summed over the range's closed hours, and the hours with no index yet."""
        dropped: dict[str, int] = {}
        paused: dict[str, list[str]] = {}
        open_hours = []
        players = sessions = 0
        for hour in self.hours():
            index = hour.index()
            if index is None:
                open_hours.append(hour.label)
                continue
            for stream, count in (index.get("dropped") or {}).items():
                dropped[stream] = dropped.get(stream, 0) + int(count)
            for stream, flag in (index.get("paused") or {}).items():
                if flag:
                    paused.setdefault(stream, []).append(hour.label)
            players += int(index.get("players", 0))
            sessions += int(index.get("sessions", 0))
        return {"dropped": dropped, "paused": paused, "open_hours": open_hours, "player_hours": players,
                "sessions": sessions}
