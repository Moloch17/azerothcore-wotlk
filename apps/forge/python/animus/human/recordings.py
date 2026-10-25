"""Manual recordings (FORMAT.md §1, §2.8): what a capture root holds, and a recording's notes joined to where the
player stood and what they were doing.

A recording is a player's own play from Record to Stop, in `<Animus.Capture.Dir>/recordings/<id>/`, at the
recording rates (100 ms snapshots, every nearby unit's state, raw packets both ways). The notes are the only typed
annotation; everything else (pulls, rests, doors) `dungeon_run` works out from the records themselves.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np

from animus.human import reader as r


def describe(rec: r.RecordingDir, count_records: bool = False) -> dict:
    """One recording as a row: id, status, verdict, title, character, start, duration, notes, files (and the
    records per type when `count_records`)."""
    manifest = rec.manifest or {}
    out = {
        "id": rec.id, "path": str(rec.path), "status": rec.status, "verdict": rec.verdict, "title": rec.title,
        "character": manifest.get("character"), "start_utc": manifest.get("start_utc"),
        "duration_s": manifest.get("duration_s"), "end_reason": manifest.get("end_reason"),
        "notes": manifest.get("notes"), "maps": manifest.get("maps"), "flags": manifest.get("flags"),
        "dropped": manifest.get("dropped"), "format": manifest.get("format"),
        "files": {path.name: path.stat().st_size for path in rec.files()},
    }
    if count_records:
        counts: dict[str, int] = {}
        malformed = unknown = 0
        for path in rec.files():
            stats = r.FileStats(path)
            for batch in r.read_file(path, stats):
                for rtype, rows in batch.records.items():
                    name = r.PREFIX[rtype][0]
                    counts[name] = counts.get(name, 0) + len(rows)
            malformed += stats.malformed
            unknown += stats.unknown
        out["records"] = counts
        out["malformed"] = malformed
        out["unknown"] = unknown
    return out


def list_recordings(root: str | Path, count_records: bool = False) -> list[dict]:
    return [describe(rec, count_records) for rec in r.list_recordings(root)]


def start_record(rec: r.RecordingDir) -> tuple[dict | None, str]:
    """The recording's RecordingStart row (as a dict) and the character's name, from the note stream."""
    for batch in rec.read("note"):
        rows = batch.get(r.RECORDING_START)
        if len(rows):
            row = {name: rows[0][name] for name in rows.dtype.names}
            row["id"] = r.text(rows[0]["id"])
            return row, batch.tail_text(r.RECORDING_START, 0)
    return None, ""


def end_record(rec: r.RecordingDir) -> tuple[dict | None, str]:
    """The recording's RecordingEnd row and its title; None while it is still open."""
    last, title = None, ""
    for batch in rec.read("note"):
        rows = batch.get(r.RECORDING_END)
        if len(rows):
            last = {name: rows[-1][name] for name in rows.dtype.names}
            title = batch.tail_text(r.RECORDING_END, len(rows) - 1)
    return last, title


def notes(rec: r.RecordingDir) -> list[dict]:
    """The recording's notes in order: each Annotation with its text, where the player stood (the record's own
    position: the note is written on the world thread from the player's live position) and the state bits named.
    The nearest Move before the note is joined too (`move_ms`, `move_x/y/z`), for tools that key on the move
    stream."""
    out: list[dict] = []
    for batch in rec.read("note"):
        rows = batch.get(r.ANNOTATION)
        for i, row in enumerate(rows):
            state = int(row["state"])
            out.append({
                "seq": int(row["seq"]), "ms": int(row["ms"]), "client_ms": int(row["client_ms"]),
                "map": int(row["map"]), "instance": int(row["instance"]), "zone": int(row["zone"]),
                "area": int(row["area"]),
                "x": float(row["x"]), "y": float(row["y"]), "z": float(row["z"]), "o": float(row["o"]),
                "target": int(row["target"]), "target_spawn_id": int(row["target_spawn_id"]),
                "target_entry": int(row["target_entry"]), "target_kind": int(row["target_kind"]),
                "target_health_pct": float(row["target_health_pct"]),
                "target_distance": float(row["target_distance"]),
                "state": [name for bit, name in ((r.AN_COMBAT, "combat"), (r.AN_DEAD, "dead"),
                                                 (r.AN_MOUNTED, "mounted"), (r.AN_CASTING, "casting"),
                                                 (r.AN_SWIMMING, "swimming"), (r.AN_FALLING, "falling"),
                                                 (r.AN_GOD, "god")) if state & bit],
                "health_pct": float(row["health_pct"]), "power_pct": float(row["power_pct"]),
                "threat_on_me": int(row["threat_on_me"]), "hostiles_near": int(row["hostiles_near"]),
                "party_alive": int(row["party_alive"]), "party_size": int(row["party_size"]),
                "text": batch.tail_text(r.ANNOTATION, i),
            })
    out.sort(key=lambda n: (n["ms"], n["seq"]))
    if not out:
        return out
    moves = rec.load("move").get(r.MOVE)
    if len(moves):
        order = np.argsort(moves["ms"], kind="stable")
        ms = moves["ms"][order]
        for note in out:
            k = int(np.searchsorted(ms, note["ms"], side="right")) - 1
            if k >= 0:
                row = moves[order[k]]
                note["move_ms"] = int(row["ms"])
                note["move_x"], note["move_y"], note["move_z"] = (float(row["x"]), float(row["y"]), float(row["z"]))
    return out


def chat(rec: r.RecordingDir) -> list[dict]:
    """The recording's chat lines in order (redacted ones keep their length only)."""
    out = []
    for batch in rec.read("note"):
        rows = batch.get(r.CHAT)
        for i, row in enumerate(rows):
            out.append({"ms": int(row["ms"]), "direction": "sent" if int(row["direction"]) == 0 else "received",
                        "chat_type": int(row["chat_type"]), "language": int(row["language"]),
                        "sender": int(row["sender"]), "sender_kind": int(row["sender_kind"]),
                        "sender_entry": int(row["sender_entry"]), "receiver": int(row["receiver"]),
                        "redacted": bool(row["redacted"]), "text": batch.tail_text(r.CHAT, i)})
    out.sort(key=lambda c: c["ms"])
    return out
