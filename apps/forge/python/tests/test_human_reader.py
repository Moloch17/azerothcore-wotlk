"""animus.human.reader: framing, every record type, gzip members, truncation, unknown records, the hourly layout."""

import gzip
import json
import os
import struct
import warnings
from pathlib import Path

import numpy as np
import pytest

from animus.human import reader as r
import human_capture_writer as w

SAMPLES = {
    1: {"ms": 1, "player": 11, "session": 7, "class_": 8, "race": 1, "gender": 1, "level": 23,
        "tree_points": (5, 13, 0), "item_level": 140, "map": 1, "zone": 17, "area": 380, "latency_ms": 88,
        "client_build": 12340, "kind": 0},
    2: {"ms": 2, "player": 11, "session": 7, "class_": 8, "level": 24, "tree_points": (5, 14, 0), "kind": 1},
    3: {"ms": 3, "player": 11, "session": 7, "reason": 1},
    6: {"ms": 6, "player": 11, "latency_ms": 250},
    10: {"ms": 10, "player": 11, "client_ms": 99, "opcode": 0x0B5, "move_flags": 0x200001, "move_flags2": 8,
         "x": 1.5, "y": -2.25, "z": 3.0, "o": 0.5, "pitch": -0.25, "fall_ms": 12, "jump_zspeed": 7.9,
         "jump_sin": 0.5, "jump_cos": 0.75, "jump_xyspeed": 7.0, "map": 530, "source": 1, "server_ms": 4000123},
    11: {"ms": 11, "player": 11, "walk": 2.5, "run": 7.0, "run_back": 4.5, "swim": 4.75, "swim_back": 2.5,
         "flight": 7.0, "flight_back": 4.5, "turn_rate": 3.25, "pitch_rate": 3.0},
    12: {"ms": 12, "player": 11, "event": 3, "arg": 42, "x": 1.0, "y": 2.0, "z": 3.0, "map": 1},
    20: {"ms": 20, "player": 11, "spell": 133, "target": 2 ** 63 + 5, "target_kind": 5, "tx": 1.0, "ty": 2.0,
         "tz": 3.0, "gcd_active": 1, "casting": 0, "power": 300, "power_type": 0},
    21: {"ms": 21, "player": 11, "spell": 133, "result": 255},
    22: {"ms": 22, "player": 11, "spell": 133, "how": 3},
    23: {"ms": 23, "player": 11, "target": 99, "target_kind": 5, "distance": 12.5},
    24: {"ms": 24, "player": 11, "item": 5512, "spell": 6262, "target": 11},
    25: {"ms": 25, "player": 11, "target": 99, "start": 1},
    26: {"ms": 26, "player": 11, "what": 7, "entry": 3310, "target": 98, "arg": 4},
    40: {"ms": 40, "source": 11, "target": 99, "spell": 0, "amount": 50, "absorbed": 2, "overkill": 1, "school": 1,
         "flags": 3},
    41: {"ms": 41, "source": 11, "target": 12, "spell": 2050, "amount": 400, "overheal": 120, "flags": 1},
    42: {"ms": 42, "killer": 11, "victim": 99, "victim_entry": 1501, "victim_kind": 5},
    43: {"ms": 43, "player": 11, "killer": 0, "cause": 2, "x": 5.0, "y": 6.0, "z": -7.0, "map": 0},
    44: {"ms": 44, "player": 11, "quest": 783, "event": 1},
    45: {"ms": 45, "player": 11, "map": 36, "instance": 3, "boss_entry": 639, "event": 1},
    46: {"ms": 46, "player": 11, "other": 13, "event": 2},
    47: {"ms": 47, "player": 11, "map": 1, "zone": 14, "area": 362},
    13: {"ms": 13, "player": 77, "kind": 1, "class_": 11, "race": 4, "level": 60, "map": 1, "zone": 141,
         "mount": 0, "form": 3, "in_combat": 1, "move_revision": 2, "model": b"druid_travel"},
    14: {"ms": 14, "player": 77, "kind": 1, "sent": 1200, "kept": 1188},
    15: {"ms": 15, "map": 36, "instance": 7, "diff_ms": 53},
    51: {"ms": 51, "owner": 11, "companion": 77, "command": 2, "arg": 0},
    52: {"ms": 52, "owner": 11, "companion": 77, "rating": -1, "reason": 5},
}


def test_every_record_type_round_trips(tmp_path):
    variable = [
        (4, {"ms": 4, "player": 11, "kind": 1, "members": [
            {"unit": 12, "class_": 1, "level": 20, "role": 1, "is_companion": 1},
            {"unit": 13, "class_": 5, "level": 21, "role": 2, "is_companion": 0}]}),
        (5, {"ms": 5, "player": 11, "spells": [133, 143, 145]}),
        (30, {"ms": 30, "player": 11, "x": 1.0, "y": 2.0, "z": 3.0, "o": 0.25, "pitch": 0.0, "map": 1,
              "health_pct": 80.0, "power_pct": 50.0, "power_type": 0, "flags": 1 | 8, "casting_spell": 133,
              "breath_pct": 100.0, "target": 99, "shapeshift_form": 0,
              "units": [{"unit": 99, "entry": 1501, "kind": 5, "x": 4.0, "vx": 1.0, "health_pct": 30.0, "level": 5,
                         "flags": 1, "target": 11, "threat_on_player": 12.5}],
              "auras": [{"spell": 1459, "stacks": 1, "remaining_ms": -1, "positive": 1}],
              "cooldowns": [{"spell": 2139, "remaining_ms": 8000}]}),
        (50, {"ms": 50, "companion": 77, "owner": 11, "model": "mage_companion", "obs_hash": 1234,
              "actions": [3, 66], "goal": 2, "goal2": 5}),
    ]
    body = [w.header("move")] + [w.record(t, v) for t, v in SAMPLES.items()] + [w.record(t, v) for t, v in variable]
    path = w.write_gz(tmp_path / "x.bin.gz", [b"".join(body)])
    stats = r.FileStats(path)
    batch = r.read_all(path, stats)
    assert stats.header["format"] == 1 and stats.header["stream"] == "move"
    assert stats.header["module_revision"] == "modsha" and stats.header["realm_build"] == "realmsha"
    assert stats.records == len(SAMPLES) + len(variable) and stats.malformed == 0 and stats.unknown == 0
    for rtype, values in SAMPLES.items():
        row = batch.get(rtype)
        assert len(row) == 1, r.PREFIX[rtype][0]
        for name, value in values.items():
            got = row[name][0]
            if name == "tree_points":
                assert tuple(int(v) for v in got) == value
            elif isinstance(value, float):
                assert float(got) == pytest.approx(value), (rtype, name)
            elif isinstance(value, bytes):
                assert r.text(got) == value.decode(), (rtype, name)
            else:
                assert int(got) == value, (rtype, name)
    group = batch.tails[r.GROUP_STATE][0]
    assert list(group["unit"]) == [12, 13] and list(group["role"]) == [1, 2] and group["is_companion"][0] == 1
    assert list(batch.tails[r.KNOWN_SPELLS][0]) == [133, 143, 145]
    decision = batch.get(r.COMPANION_DECISION)[0]
    assert r.text(decision["model"]) == "mage_companion" and int(decision["n_actions"]) == 2
    assert list(batch.tails[r.COMPANION_DECISION][0]) == [3, 66, 2, 5]
    snap = batch.get(r.SNAPSHOT)[0]
    assert int(snap["flags"]) == 9 and int(snap["target"]) == 99 and float(snap["health_pct"]) == 80.0
    details = r.read_file(path).__next__().snapshot_details(0)
    assert details["units"]["entry"][0] == 1501 and details["units"]["threat_on_player"][0] == 12.5
    assert details["auras"]["remaining_ms"][0] == -1 and details["cooldowns"]["remaining_ms"][0] == 8000


def test_gzip_members_concatenate_and_records_may_straddle_them(tmp_path):
    recs = [w.record(10, dict(SAMPLES[10], ms=i)) for i in range(500)]
    blob = w.header("move") + b"".join(recs)
    # Members cut at arbitrary byte positions, so records straddle them.
    cuts = [0, 17, 3000, 3001, 20000, len(blob)]
    path = w.write_gz(tmp_path / "move-0.bin.gz", [blob[a:b] for a, b in zip(cuts, cuts[1:])])
    with gzip.open(path) as handle:     # one valid gzip stream, as FORMAT.md promises
        assert handle.read() == blob
    batch = r.read_all(path)
    assert list(batch.get(r.MOVE)["ms"]) == list(range(500))
    # Small blocks: the same records, carried across block boundaries.
    small = r.merge(list(r.read_file(path, block_bytes=1000)))
    assert list(small.get(r.MOVE)["ms"]) == list(range(500))


def test_a_truncated_final_member_loses_only_that_member(tmp_path):
    first = w.header("move") + b"".join(w.record(10, dict(SAMPLES[10], ms=i)) for i in range(10))
    second = b"".join(w.record(10, dict(SAMPLES[10], ms=100 + i)) for i in range(10))
    data = gzip.compress(first) + gzip.compress(second)
    path = tmp_path / "move-0.bin.gz"
    path.write_bytes(data[:-15])
    stats = r.FileStats(path)
    batch = r.read_all(path, stats)
    assert list(batch.get(r.MOVE)["ms"]) == list(range(10))
    assert stats.truncated_members == 1


def test_unknown_types_and_longer_records_are_skipped_by_length(tmp_path):
    body = (w.header("outcome") + w.record(99, raw=b"\x01" * 13) + w.record(21, SAMPLES[21], extra=b"new fields")
            + w.record(1234, raw=b"") + w.record(21, dict(SAMPLES[21], ms=2)) + w.record(21, raw=b"\0" * 5))
    path = tmp_path / "outcome-0.bin"
    path.write_bytes(body)
    stats = r.FileStats(path)
    batch = r.read_all(path, stats)
    assert list(batch.get(r.CAST_RESULT)["ms"]) == [21, 2]
    assert batch.unknown == {99: 1, 1234: 1} and batch.malformed == 1
    assert stats.unknown == 2 and stats.malformed == 1


def test_a_plain_file_cut_inside_a_record_drops_that_record(tmp_path):
    body = w.header("move") + b"".join(w.record(10, dict(SAMPLES[10], ms=i)) for i in range(3))
    path = tmp_path / "move-1.bin"
    path.write_bytes(body[:-5])
    stats = r.FileStats(path)
    assert list(r.read_all(path, stats).get(r.MOVE)["ms"]) == [0, 1]
    assert stats.trailing_bytes > 0


def test_the_file_header_is_checked(tmp_path):
    bad = tmp_path / "bad.bin"
    bad.write_bytes(w.header("move", magic=b"NOPE\0\0\0\0"))
    with pytest.raises(r.CaptureError):
        r.read_all(bad)
    headless = tmp_path / "headless.bin"
    headless.write_bytes(w.record(21, SAMPLES[21]))
    with pytest.raises(r.CaptureError):
        r.read_all(headless)
    newer = tmp_path / "newer.bin"
    newer.write_bytes(w.header("move", fmt=r.FORMAT_VERSION + 1) + w.record(21, SAMPLES[21]))
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        assert len(r.read_all(newer).get(r.CAST_RESULT)) == 1
    assert any("newer" in str(c.message) for c in caught)


def test_long_runs_and_interleaved_types_frame_alike(tmp_path):
    rng = np.random.default_rng(3)
    kinds = rng.choice([10, 10, 10, 11, 12], size=3000)
    body = w.header("move") + b"".join(w.record(int(k), dict(SAMPLES[int(k)], ms=i)) for i, k in enumerate(kinds))
    path = tmp_path / "move-0.bin"
    path.write_bytes(body)
    batch = r.read_all(path)
    for k in (10, 11, 12):
        assert list(batch.get(k)["ms"]) == [i for i, kk in enumerate(kinds) if kk == k]


def test_capture_dir_walks_hours_in_range_and_reports_the_index(tmp_path):
    for day, hour in (("2026-10-04", 23), ("2026-10-05", 0), ("2026-10-05", 1)):
        d = w.hour_dir(tmp_path, day, hour)
        w.write_gz(d / "move-0.bin.gz", [w.header("move") + w.record(10, dict(SAMPLES[10], ms=hour))])
        w.write_gz(d / "session-all.bin.gz", [w.header("session") + w.record(1, SAMPLES[1])])
        if hour != 1:
            w.write_index(d, f"{day}T{hour:02d}", {}, players=3, dropped={"move": 0, "snapshot": 4},
                          paused={"snapshot": hour == 0})
    cap = r.CaptureDir(tmp_path, "2026-10-04T23", "2026-10-05")
    assert [h.label for h in cap.hours()] == ["2026-10-04T23", "2026-10-05T00", "2026-10-05T01"]
    assert [p.name for _, p in cap.files("move")] == ["move-0.bin.gz"] * 3
    assert [int(b.get(r.MOVE)["ms"][0]) for _, _, b in cap.iter_stream("move")] == [23, 0, 1]
    report = cap.index_report()
    assert report["dropped"] == {"move": 0, "snapshot": 8} and report["paused"] == {"snapshot": ["2026-10-05T00"]}
    assert report["open_hours"] == ["2026-10-05T01"] and report["player_hours"] == 6
    assert [h.label for h in r.CaptureDir(tmp_path, "2026-10-05T01").hours()] == ["2026-10-05T01"]
    assert r.stream_map(Path("session-all.bin.gz")) == ("session", "all")


SAMPLE_DIRS = [Path(os.environ.get("ANIMUS_CAPTURE_SAMPLE", "")),
               Path(__file__).resolve().parents[2] / "tools" / "capture-sample.bin",
               Path(__file__).resolve().parents[1] / "tools" / "capture-sample.bin",
               Path(__file__).resolve().parents[4] / "tools" / "capture-sample.bin",
               Path(__file__).resolve().parents[4] / "modules" / "mod-animus" / "tools" / "capture-sample.bin"]


def test_reads_the_cpp_serializers_sample_when_present():
    """W1's sample (tools/capture-sample.bin + .json, from mod-animus's serializer) read and compared with the
    values it says it wrote. Skipped until the sample exists."""
    sample = next((p for p in SAMPLE_DIRS if str(p) not in ("", ".") and p.is_file()), None)
    if sample is None:
        pytest.skip("no capture-sample.bin (W1 provides it; ANIMUS_CAPTURE_SAMPLE overrides the path)")
    stats = r.FileStats(sample)
    batch = r.read_all(sample, stats)
    assert stats.malformed == 0 and stats.trailing_bytes == 0 and stats.header is not None
    expected_path = sample.with_suffix(".json")
    if not expected_path.is_file():
        return
    expected = json.loads(expected_path.read_text())
    records = expected.get("records", expected) if isinstance(expected, dict) else expected
    seen: dict[int, int] = {}
    for item in records:
        rtype = item.get("type")
        if isinstance(rtype, str):
            rtype = next(code for code, (name, _) in r.PREFIX.items() if name == rtype)
        if rtype not in r.PREFIX or rtype == r.FILE_HEADER:
            continue
        index = seen.get(rtype, 0)
        seen[rtype] = index + 1
        row = batch.get(rtype)[index]
        for name, value in (item.get("fields") or {k: v for k, v in item.items() if k != "type"}).items():
            key = "class_" if name == "class" else name
            if key not in row.dtype.names:
                continue
            got = row[key]
            if isinstance(value, str):
                assert r.text(got) == value
            elif isinstance(value, list):
                assert [float(v) for v in np.atleast_1d(got)] == pytest.approx([float(v) for v in value])
            else:
                assert float(got) == pytest.approx(float(value), rel=1e-6, abs=1e-6), (rtype, name)


def test_struct_sizes_match_the_reader_prefixes():
    for rtype, (fmt, _) in w.FIXED.items():
        assert struct.calcsize(fmt) == r.PREFIX[rtype][1].itemsize, r.PREFIX[rtype][0]


def test_format3_player_and_companion_moves_read_back(tmp_path):
    """Format 3: a player's client packet (source 0) and a companion's controller packet (source 2) in one move file,
    each with both clocks, a MoverState and a MoveTally, read back field for field; the companion's track is told apart
    by its source."""
    from animus.human import tracks as t
    player, companion = 11, 77
    human = dict(SAMPLES[10], player=player, source=r.SOURCE_CLIENT, ms=1000, move_flags=1)
    bot = dict(SAMPLES[10], player=companion, source=r.SOURCE_CONTROLLER, ms=1000, client_ms=1001000, opcode=0x0EE,
               move_flags=1)
    movers = [dict(SAMPLES[13], ms=999, player=player, kind=0, move_revision=0, model=b"", mount=23229),
              dict(SAMPLES[13], ms=999, player=companion)]
    moves = []
    for i in range(12):
        moves.append(w.record(10, dict(human, ms=1000 + 100 * i, x=1.5 + 0.7 * i, client_ms=99 + 100 * i,
                                       server_ms=5000 + 100 * i)))
        moves.append(w.record(10, dict(bot, ms=1000 + 100 * i, x=1.5 + 0.7 * i, client_ms=1001000 + 100 * i,
                                       server_ms=5000 + 100 * i)))
    tallies = [w.record(14, {"ms": 2300, "player": player, "kind": 0, "sent": 13, "kept": 12}),
               w.record(14, {"ms": 2300, "player": companion, "kind": 1, "sent": 14, "kept": 12})]
    ticks = [w.record(15, {"ms": 1000 + 50 * i, "map": 1, "instance": 0, "diff_ms": 50 + i % 3}) for i in range(24)]
    path = tmp_path / "move-1.bin"
    path.write_bytes(w.header("move", fmt=r.FORMAT_VERSION) + b"".join(w.record(13, m) for m in movers)
                     + b"".join(moves) + b"".join(tallies) + b"".join(ticks))
    stats = r.FileStats(path)
    batch = r.read_all(path, stats)
    assert stats.malformed == 0 and stats.header["format"] == r.FORMAT_VERSION == 3
    rows = batch.get(r.MOVE)
    assert sorted(set(rows["source"].tolist())) == [r.SOURCE_CLIENT, r.SOURCE_CONTROLLER]
    mine = rows[rows["player"] == companion]
    assert int(mine["opcode"][0]) == 0x0EE and int(mine["client_ms"][0]) == 1001000
    assert list(mine["server_ms"][:3]) == [5000, 5100, 5200]
    assert list(rows[rows["player"] == player]["server_ms"][:2]) == [5000, 5100]
    tally = {int(row["player"]): row for row in batch.get(r.MOVE_TALLY)}
    assert (int(tally[companion]["sent"]) - int(tally[companion]["kept"]), int(tally[companion]["kind"])) == (2, 1)
    assert (int(tally[player]["sent"]) - int(tally[player]["kept"]), int(tally[player]["kind"])) == (1, 0)
    updates = batch.get(r.MAP_UPDATE)
    assert len(updates) == 24 and list(updates["diff_ms"][:4]) == [50, 51, 52, 50] and int(updates["map"][0]) == 1
    state = batch.get(r.MOVER_STATE)
    assert len(state) == 2
    them = {int(row["player"]): row for row in state}
    assert int(them[player]["kind"]) == 0 and int(them[player]["mount"]) == 23229 and r.text(them[player]["model"]) == ""
    assert int(them[companion]["kind"]) == 1 and int(them[companion]["move_revision"]) == 2
    assert r.text(them[companion]["model"]) == "druid_travel" and int(them[companion]["in_combat"]) == 1
    assert int(them[companion]["form"]) == 3 and int(them[companion]["class_"]) == 11
    built = t.build_tracks(batch, include_companions=True)
    kinds = {track.player: track.companion for track in built}
    assert kinds.get(companion) is True and kinds.get(player) is False
    assert companion not in {track.player for track in t.build_tracks(batch)}


def test_older_moves_without_server_ms_still_read(tmp_path):
    """A format 1/2 Move (no server_ms) is read with server_ms 0, in file order among format 3 ones; a record shorter
    than even the old Move is malformed."""
    old = w.record(10, dict(SAMPLES[10], ms=1))[:-4]
    old = struct.pack("<HH", 10, len(old) - 4) + old[4:]
    new = w.record(10, dict(SAMPLES[10], ms=2))
    runt = struct.pack("<HH", 10, 20) + bytes(20)
    path = tmp_path / "mixed.bin"
    path.write_bytes(w.header("move", fmt=2) + old + new + old.replace(struct.pack("<Q", 1), struct.pack("<Q", 3), 1)
                     + runt)
    stats = r.FileStats(path)
    rows = r.read_all(path, stats).get(r.MOVE)
    assert list(rows["ms"]) == [1, 2, 3] and list(rows["server_ms"]) == [0, 4000123, 0]
    assert float(rows["x"][0]) == 1.5 and int(rows["source"][2]) == 1
    assert stats.malformed == 1
