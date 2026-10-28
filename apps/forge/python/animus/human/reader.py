"""Reading capture files (FORMAT.md §1-2): framing, record types, gzip members, the hourly layout and its index.

Format 5 (2026-10-28) is one tier: the hourly files hold every player and every map's surroundings; the manual
recordings of format 4 (`recordings/<id>/`, manifests, notes, raw packets) are gone, and so is this module's API for
them. Format 4 files still read: their record types stay known (60-62, 70 reserved), and the fields format 5
appended (`instance` on the session rows) read as 0 from them.

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

FORMAT_VERSION = 5
MAGIC = b"ANCAP\0\0\0"
FRAME = struct.Struct("<HH")
BLOCK_BYTES = 32 << 20          # decompressed bytes framed and parsed at once
READ_BYTES = 1 << 20            # compressed bytes read from disk at once

STREAMS = {1: "session", 2: "move", 3: "action", 4: "snapshot", 5: "outcome", 6: "companion", 7: "note",
           8: "packet"}       # 8: format 4's raw packets, never written since format 5


class CaptureError(ValueError):
    """A capture file that is not one (bad magic, a format older than any reader knows)."""


# Record types (FORMAT.md §2).
FILE_HEADER = 0
SESSION_START, SESSION_CONTEXT, SESSION_END, GROUP_STATE, KNOWN_SPELLS, LATENCY = 1, 2, 3, 4, 5, 6
INVENTORY, ACTION_BUTTONS, TALENTS = 7, 8, 9                    # format 4; every player's since 5
MOVE, SPEEDS, MOTION_EVENT, MOVER_STATE, MOVE_TALLY, MAP_UPDATE = 10, 11, 12, 13, 14, 15

# Move `source` (FORMAT.md §2.3): a player's client packet; format 1's synthesised companion sample (never written by
# format 2); a companion's player controller packet, through its session's movement handlers (format 2).
SOURCE_CLIENT, SOURCE_COMPANION_SAMPLE, SOURCE_CONTROLLER = 0, 1, 2
CAST_REQUEST, CAST_RESULT, CAST_END, SELECT, ITEM_USE, ATTACK, INTERACT = 20, 21, 22, 23, 24, 25, 26
SNAPSHOT = 30
UNIT_STATE, GAME_OBJECT_STATE, PLAYER_DETAIL, MAP_SPAWNS, INSTANCE_CREATURES = 31, 32, 33, 34, 35    # format 4
DAMAGE, HEAL, KILL, DEATH, QUEST, ENCOUNTER, PVP, AREA = 40, 41, 42, 43, 44, 45, 46, 47
COMBAT_EDGE = 48                                                # format 4
COMPANION_DECISION, COMPANION_COMMAND, COMPANION_RATING = 50, 51, 52
ANNOTATION, RECORDING_START, RECORDING_END, CHAT = 60, 61, 62, 63     # the note stream; 60-62 reserved since 5
RAW_PACKET = 70                                                 # format 4's packet stream, reserved since 5

# Interact `what` (FORMAT.md §2.4).
IA_GOSSIP, IA_LOOT, IA_QUEST_ACCEPT, IA_QUEST_TURN_IN, IA_GAME_OBJECT_USE = 1, 2, 3, 4, 5
# CombatEdge `event`, GameObjectState `event`, RecordingEnd `reason` and `verdict` (FORMAT.md §2.5, §2.6, §2.8).
EDGE_ENTER, EDGE_EXIT, EDGE_EVADE = 0, 1, 2
GO_SEEN, GO_CHANGED, GO_GONE = 0, 1, 2
END_STOP, END_LOGOUT, END_DISCONNECT, END_SHUTDOWN, END_DISK, END_MAX_MINUTES = 0, 1, 2, 3, 4, 5
VERDICT_NONE, VERDICT_KEEP, VERDICT_DISCARD = 0, 1, 2
# UnitState `flags` (format 5: 4 in the observer's group, 8 a player's pet, 32 engaged with players, 64 tapped by a
# player, 128 the unit's last record -- gone from the map) and Annotation `state` bits.
US_COMBAT, US_EVADING, US_PARTY, US_MY_PET, US_COMPANION, US_ENGAGED, US_TAPPED = 1, 2, 4, 8, 16, 32, 64
US_GONE = 128
AN_COMBAT, AN_DEAD, AN_MOUNTED, AN_CASTING, AN_SWIMMING, AN_FALLING, AN_GOD = 1, 2, 4, 8, 16, 32, 64

# MotionEvent `event` (FORMAT.md §2.3).
EV_MOUNT, EV_DISMOUNT, EV_TAXI_START, EV_TAXI_END, EV_TELEPORT, EV_DEATH, EV_RESURRECT = 1, 2, 3, 4, 5, 6, 7
EV_ROOT, EV_UNROOT, EV_STUN_START, EV_STUN_END, EV_FEAR_START, EV_FEAR_END, EV_KNOCKBACK = 8, 9, 10, 11, 12, 13, 14
EV_LOADING_START, EV_LOADING_END, EV_SHAPESHIFT = 15, 16, 17
EV_VEHICLE_ENTER, EV_VEHICLE_EXIT, EV_TRANSPORT_BOARD, EV_TRANSPORT_LEAVE = 18, 19, 20, 21

_SESSION = [("ms", "<u8"), ("player", "<u8"), ("session", "<u8"), ("class_", "u1"), ("race", "u1"),
            ("gender", "u1"), ("level", "u1"), ("tree_points", "u1", (3,)), ("item_level", "<u2"),
            ("map", "<u4"), ("zone", "<u4"), ("area", "<u4"), ("latency_ms", "<u2"), ("client_build", "<u4"),
            ("kind", "u1"), ("instance", "<u4")]
_SELF = [("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("o", "<f4"), ("pitch", "<f4"), ("map", "<u4"),
         ("health_pct", "<f4"), ("power_pct", "<f4"), ("power_type", "u1"), ("flags", "u1"),
         ("casting_spell", "<u4"), ("breath_pct", "<f4"), ("target", "<u8"), ("shapeshift_form", "<u4")]

# The fixed prefix of every record type: (name, dtype). `class_` because `class` is a keyword.
PREFIX: dict[int, tuple[str, np.dtype]] = {
    FILE_HEADER: ("FileHeader", [("magic", "S8"), ("format", "<u2"), ("stream", "<u2"), ("opened_ms", "<u8"),
                                 ("module_revision", "S40"), ("realm_build", "S40")]),
    SESSION_START: ("SessionStart", _SESSION),
    SESSION_CONTEXT: ("SessionContext", _SESSION),
    SESSION_END: ("SessionEnd", [("ms", "<u8"), ("player", "<u8"), ("session", "<u8"), ("reason", "u1"), ("map", "<u4"), ("instance", "<u4")]),
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
                            ("arg", "<u4"), ("spawn_id", "<u4")]),
    SNAPSHOT: ("Snapshot", [("ms", "<u8"), ("player", "<u8")] + _SELF),
    INVENTORY: ("Inventory", [("ms", "<u8"), ("player", "<u8"), ("count", "<u2")]),
    ACTION_BUTTONS: ("ActionButtons", [("ms", "<u8"), ("player", "<u8"), ("count", "u1")]),
    TALENTS: ("Talents", [("ms", "<u8"), ("player", "<u8"), ("active_spec", "u1"), ("count", "u1")]),
    UNIT_STATE: ("UnitState", [("ms", "<u8"), ("player", "<u8"), ("unit", "<u8"), ("spawn_id", "<u4"),
                               ("entry", "<u4"), ("unit_type", "u1"), ("kind", "u1"), ("x", "<f4"), ("y", "<f4"),
                               ("z", "<f4"), ("o", "<f4"), ("vx", "<f4"), ("vy", "<f4"), ("vz", "<f4"),
                               ("move_flags", "<u4"), ("health", "<u4"), ("max_health", "<u4"),
                               ("power_type", "u1"), ("power", "<u4"), ("max_power", "<u4"), ("level", "u1"),
                               ("reaction", "u1"), ("react_state", "u1"), ("rank", "u1"), ("boss", "u1"),
                               ("unit_flags", "<u4"), ("npc_flags", "<u4"), ("dynamic_flags", "<u4"),
                               ("unit_state", "<u4"), ("death_state", "u1"), ("target", "<u8"),
                               ("victim", "<u8"), ("casting_spell", "<u4"), ("cast_left_ms", "<i4"),
                               ("motion_type", "u1"), ("home_x", "<f4"), ("home_y", "<f4"), ("home_z", "<f4"),
                               ("formation_leader_spawn", "<u4"), ("flags", "u1"), ("aura_count", "u1")]),
    GAME_OBJECT_STATE: ("GameObjectState", [("ms", "<u8"), ("player", "<u8"), ("object", "<u8"),
                                            ("spawn_id", "<u4"), ("entry", "<u4"), ("go_type", "u1"),
                                            ("go_state", "u1"), ("loot_state", "u1"), ("flags", "<u4"),
                                            ("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("o", "<f4"),
                                            ("event", "u1")]),
    PLAYER_DETAIL: ("PlayerDetail", [("ms", "<u8"), ("player", "<u8"), ("stats", "<u4", (5,)), ("armor", "<u4"),
                                     ("resistances", "<u4", (6,)), ("attack_power", "<u4"),
                                     ("ranged_attack_power", "<u4"), ("spell_power", "<u4"),
                                     ("heal_power", "<u4"), ("ratings", "<f4", (11,)), ("expertise", "<f4"),
                                     ("max_health", "<u4"), ("max_power", "<u4", (7,)), ("money", "<u4"),
                                     ("xp", "<u4"), ("next_xp", "<u4"), ("combo_points", "u1"),
                                     ("combo_target", "<u8"), ("cheat_flags", "<u4"), ("game_master", "u1")]),
    MAP_SPAWNS: ("MapSpawns", [("ms", "<u8"), ("map", "<u4"), ("part", "<u2"), ("parts", "<u2"),
                               ("count", "<u2")]),
    INSTANCE_CREATURES: ("InstanceCreatures", [("ms", "<u8"), ("map", "<u4"), ("instance", "<u4"),
                                               ("part", "<u2"), ("parts", "<u2"), ("count", "<u2")]),
    DAMAGE: ("Damage", [("ms", "<u8"), ("source", "<u8"), ("target", "<u8"), ("spell", "<u4"), ("amount", "<u4"),
                        ("absorbed", "<u4"), ("overkill", "<u4"), ("school", "u1"), ("flags", "u1")]),
    HEAL: ("Heal", [("ms", "<u8"), ("source", "<u8"), ("target", "<u8"), ("spell", "<u4"), ("amount", "<u4"),
                    ("overheal", "<u4"), ("flags", "u1")]),
    KILL: ("Kill", [("ms", "<u8"), ("killer", "<u8"), ("victim", "<u8"), ("victim_entry", "<u4"),
                    ("victim_kind", "u1"), ("victim_spawn_id", "<u4")]),
    DEATH: ("Death", [("ms", "<u8"), ("player", "<u8"), ("killer", "<u8"), ("cause", "u1"), ("x", "<f4"),
                      ("y", "<f4"), ("z", "<f4"), ("map", "<u4")]),
    QUEST: ("Quest", [("ms", "<u8"), ("player", "<u8"), ("quest", "<u4"), ("event", "u1")]),
    ENCOUNTER: ("Encounter", [("ms", "<u8"), ("player", "<u8"), ("map", "<u4"), ("instance", "<u4"),
                              ("boss_entry", "<u4"), ("event", "u1")]),
    PVP: ("PvP", [("ms", "<u8"), ("player", "<u8"), ("other", "<u8"), ("event", "u1")]),
    AREA: ("Area", [("ms", "<u8"), ("player", "<u8"), ("map", "<u4"), ("zone", "<u4"), ("area", "<u4")]),
    COMBAT_EDGE: ("CombatEdge", [("ms", "<u8"), ("map", "<u4"), ("instance", "<u4"), ("unit", "<u8"),
                                 ("spawn_id", "<u4"), ("entry", "<u4"), ("event", "u1"), ("evade_reason", "u1"),
                                 ("other", "<u8"), ("other_spawn_id", "<u4"), ("x", "<f4"), ("y", "<f4"),
                                 ("z", "<f4"), ("ox", "<f4"), ("oy", "<f4"), ("oz", "<f4")]),
    COMPANION_DECISION: ("CompanionDecision", [("ms", "<u8"), ("companion", "<u8"), ("owner", "<u8"),
                                               ("model", "S32"), ("obs_hash", "<u4"), ("n_actions", "<u2")]),
    COMPANION_COMMAND: ("CompanionCommand", [("ms", "<u8"), ("owner", "<u8"), ("companion", "<u8"),
                                             ("command", "u1"), ("arg", "<u4")]),
    COMPANION_RATING: ("CompanionRating", [("ms", "<u8"), ("owner", "<u8"), ("companion", "<u8"), ("rating", "i1"),
                                           ("reason", "u1")]),
    ANNOTATION: ("Annotation", [("ms", "<u8"), ("player", "<u8"), ("seq", "<u4"), ("kind", "<u2"), ("map", "<u4"),
                                ("instance", "<u4"), ("zone", "<u4"), ("area", "<u4"), ("x", "<f4"), ("y", "<f4"),
                                ("z", "<f4"), ("o", "<f4"), ("target", "<u8"), ("target_spawn_id", "<u4"),
                                ("target_entry", "<u4"), ("target_kind", "u1"), ("target_health_pct", "<f4"),
                                ("target_distance", "<f4"), ("state", "u1"), ("health_pct", "<f4"),
                                ("power_pct", "<f4"), ("power_type", "u1"), ("threat_on_me", "u1"),
                                ("hostiles_near", "u1"), ("party_alive", "u1"), ("party_size", "u1"),
                                ("client_ms", "<u4"), ("text_len", "<u2")]),
    RECORDING_START: ("RecordingStart", [("ms", "<u8"), ("player", "<u8"), ("session", "<u8"), ("id", "S64"),
                                         ("class_", "u1"), ("race", "u1"), ("gender", "u1"), ("level", "u1"),
                                         ("map", "<u4"), ("instance", "<u4"), ("zone", "<u4"), ("area", "<u4"),
                                         ("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("o", "<f4"),
                                         ("snapshot_ms", "<u2"), ("unit_ms", "<u2"), ("streams", "<u4"),
                                         ("flags", "u1"), ("text_len", "<u2")]),
    RECORDING_END: ("RecordingEnd", [("ms", "<u8"), ("player", "<u8"), ("session", "<u8"), ("reason", "u1"),
                                     ("verdict", "u1"), ("notes", "<u4"), ("seconds", "<u4"), ("text_len", "<u2")]),
    CHAT: ("Chat", [("ms", "<u8"), ("player", "<u8"), ("direction", "u1"), ("chat_type", "u1"),
                    ("language", "<u4"), ("sender", "<u8"), ("sender_kind", "u1"), ("sender_entry", "<u4"),
                    ("receiver", "<u8"), ("redacted", "u1"), ("text_len", "<u2")]),
    RAW_PACKET: ("RawPacket", [("ms", "<u8"), ("player", "<u8"), ("direction", "u1"), ("opcode", "<u2"),
                               ("server_ms", "<u4"), ("total", "<u4"), ("offset", "<u4"), ("flags", "u1"),
                               ("length", "<u2")]),
}
PREFIX = {rtype: (name, np.dtype(fields)) for rtype, (name, fields) in PREFIX.items()}
# Fields a later format appended to a record, which an older file's record lacks: read as 0 there. The record's
# shortest valid payload is its prefix without them.
APPENDED: dict[int, tuple[str, ...]] = {MOVE: ("server_ms",),                                   # format 3
                                        KILL: ("victim_spawn_id",), INTERACT: ("spawn_id",),    # format 4
                                        SESSION_START: ("instance",), SESSION_CONTEXT: ("instance",),   # format 5
                                        SESSION_END: ("map", "instance")}
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
# Format 4 tails (FORMAT.md §2.2, §2.5): a recording's dumps and surroundings.
INVENTORY_ITEM = np.dtype([("bag", "u1"), ("slot", "u1"), ("entry", "<u4"), ("count", "<u4"), ("perm_enchant", "<u4"),
                           ("temp_enchant", "<u4"), ("random_property", "<i4"), ("durability", "<u4"),
                           ("max_durability", "<u4")])
ACTION_BUTTON = np.dtype([("button", "u1"), ("kind", "u1"), ("action", "<u4")])
TALENT_RANK = np.dtype([("talent", "<u4"), ("rank", "u1")])
UNIT_STATE_AURA = np.dtype([("spell", "<u4"), ("stacks", "u1"), ("remaining_ms", "<i4"), ("positive", "u1"),
                            ("caster", "<u8")])
UNIT_STATE_THREAT = np.dtype([("unit", "<u8"), ("threat", "<f4")])
MAP_SPAWN = np.dtype([("spawn_id", "<u4"), ("entry", "<u4"), ("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("o", "<f4"),
                      ("rank", "u1"), ("boss", "u1"), ("formation_leader_spawn", "<u4"), ("kind", "u1"),
                      ("go_type", "u1")])
INSTANCE_CREATURE = np.dtype([("spawn_id", "<u4"), ("alive", "u1")])
# Types whose tail is a counted array of one dtype (the count is the prefix's last field).
COUNTED_TAILS = {INVENTORY: INVENTORY_ITEM, ACTION_BUTTONS: ACTION_BUTTON, MAP_SPAWNS: MAP_SPAWN,
                 INSTANCE_CREATURES: INSTANCE_CREATURE}
# Types whose tail is text (or bytes) of the prefix's last field's length.
TEXT_TAILS = {ANNOTATION, RECORDING_START, RECORDING_END, CHAT, RAW_PACKET}
# Types whose tail is read on demand from the batch (the spans kept, nothing parsed up front).
SPANNED = {SNAPSHOT, UNIT_STATE, TALENTS, *COUNTED_TAILS, *TEXT_TAILS}

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
    _spans: dict[int, np.ndarray] = field(default_factory=dict)   # SPANNED type -> [N, 2] payload offset, length

    @property
    def _snapshot_spans(self) -> np.ndarray | None:
        return self._spans.get(SNAPSHOT)

    def get(self, rtype: int) -> np.ndarray:
        """The prefixes of one record type (an empty array of its dtype when the batch has none)."""
        found = self.records.get(rtype)
        return found if found is not None else np.zeros(0, dtype=PREFIX[rtype][1])

    def _span(self, rtype: int, index: int) -> tuple[int, int] | None:
        """(first tail byte, end) of the batch's `index`-th record of a SPANNED type."""
        spans = self._spans.get(rtype)
        if spans is None or index >= len(spans):
            return None
        start, length = (int(v) for v in spans[index])
        return start + PREFIX[rtype][1].itemsize, start + length

    def tail_bytes(self, rtype: int, index: int) -> bytes:
        """The raw tail of a record (everything after its fixed prefix); empty when unknown."""
        span = self._span(rtype, index)
        return b"" if span is None else self._raw[span[0]:span[1]]

    def tail_text(self, rtype: int, index: int) -> str:
        """The text of a TEXT_TAILS record (a Chat line; a format 4 Annotation's note, RecordingStart's name or
        RecordingEnd's title), cut to the length its prefix says."""
        rows = self.records.get(rtype)
        if rows is None or index >= len(rows):
            return ""
        field_name = rows.dtype.names[-1]
        raw = self.tail_bytes(rtype, index)[:int(rows[index][field_name])]
        return raw.decode("utf-8", "replace")

    def tail_rows(self, rtype: int, index: int) -> np.ndarray:
        """The counted array of a COUNTED_TAILS record (Inventory items, ActionButtons, a MapSpawns part, an
        InstanceCreatures part), cut to what the record really holds."""
        dtype = COUNTED_TAILS[rtype]
        rows = self.records.get(rtype)
        if rows is None or index >= len(rows):
            return np.zeros(0, dtype)
        raw = self.tail_bytes(rtype, index)
        count = min(int(rows[index][rows.dtype.names[-1]]), len(raw) // dtype.itemsize)
        return np.frombuffer(raw, dtype=dtype, count=count).copy()

    def talents(self, index: int) -> tuple[np.ndarray, np.ndarray]:
        """(ranks, glyph ids) of the batch's `index`-th Talents record."""
        rows = self.records.get(TALENTS)
        raw = self.tail_bytes(TALENTS, index)
        if rows is None or index >= len(rows):
            return np.zeros(0, TALENT_RANK), np.zeros(0, np.uint32)
        count = min(int(rows[index]["count"]), len(raw) // TALENT_RANK.itemsize)
        ranks = np.frombuffer(raw, dtype=TALENT_RANK, count=count).copy()
        pos = count * TALENT_RANK.itemsize
        if pos + 1 > len(raw):
            return ranks, np.zeros(0, np.uint32)
        glyphs = min(raw[pos], (len(raw) - pos - 1) // 4)
        return ranks, np.frombuffer(raw, dtype="<u4", count=glyphs, offset=pos + 1).astype(np.uint32)

    def unit_state_details(self, index: int) -> dict[str, np.ndarray]:
        """The auras and the threat list of the batch's `index`-th UnitState (a malformed tail reads as empty)."""
        empty = {"auras": np.zeros(0, UNIT_STATE_AURA), "threats": np.zeros(0, UNIT_STATE_THREAT)}
        rows = self.records.get(UNIT_STATE)
        if rows is None or index >= len(rows):
            return empty
        raw = self.tail_bytes(UNIT_STATE, index)
        auras = int(rows[index]["aura_count"])
        size = auras * UNIT_STATE_AURA.itemsize
        if size + 1 > len(raw):
            return empty
        out = {"auras": np.frombuffer(raw, dtype=UNIT_STATE_AURA, count=auras).copy()}
        threats = min(raw[size], (len(raw) - size - 1) // UNIT_STATE_THREAT.itemsize)
        out["threats"] = np.frombuffer(raw, dtype=UNIT_STATE_THREAT, count=threats, offset=size + 1).copy()
        return out

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
        if rtype in SPANNED:
            offs, lens = _offsets(mine)
            batch._spans[rtype] = np.stack([offs, lens], axis=1)
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
                dropped[stream] = dropped.get(stream, 0) + int(count if not isinstance(count, dict)
                                                                else sum(count.values()))
            for stream, flag in (index.get("paused") or {}).items():
                if flag:
                    paused.setdefault(stream, []).append(hour.label)
            players += int(index.get("players", 0))
            sessions += int(index.get("sessions", 0))
        return {"dropped": dropped, "paused": paused, "open_hours": open_hours, "player_hours": players,
                "sessions": sessions}
