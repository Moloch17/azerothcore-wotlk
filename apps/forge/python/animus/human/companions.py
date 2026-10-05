"""Per-model feedback on the shipped companions (plan §3.6, §7): what the humans who had them did about them.

From the companion stream (decisions, commands, ratings), the session stream (which units are companions) and the
outcome stream (deaths). Each companion is credited to the model named by its latest CompanionDecision; one with
none is under `unknown`. Per model:

- `companions`, `owners`, `decisions`;
- `ratings`: `up` / `down` counts, `by_reason` (none, movement, combat, healing, tanking, stuck, other) as up/down;
- `commands` by name; `dismissals` (command 2); `overrides`: orders that take the companion's choice from it
  (follow, assist, guard, stay, other order: commands 3-7), with `overrides_per_1000_decisions`;
- `deaths` (Death records of the companion) and `deaths_per_1000_decisions`;
- `net_rating`: (up - down) / (up + down), None without ratings.
"""

from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

import numpy as np

from animus.human import reader as r

COMMANDS = {1: "summon", 2: "dismiss", 3: "follow", 4: "assist", 5: "guard", 6: "stay", 7: "other"}
OVERRIDES = (3, 4, 5, 6, 7)
REASONS = {0: "none", 1: "movement", 2: "combat", 3: "healing", 4: "tanking", 5: "stuck", 6: "other"}


def report(capture: str | Path, start: str | None = None, end: str | None = None) -> dict:
    cap = r.CaptureDir(capture, start, end)
    model_of: dict[int, str] = {}
    owners: dict[str, set] = {}
    decisions: dict[str, int] = {}
    companions_seen: set[int] = set()
    commands: list[tuple[int, int, int]] = []       # companion, owner, command
    ratings: list[tuple[int, int, int]] = []        # companion, rating, reason
    deaths: list[int] = []
    for hour in cap.hours():
        for path in hour.files("session"):
            for batch in r.read_file(path):
                for rtype in (r.SESSION_START, r.SESSION_CONTEXT):
                    rows = batch.get(rtype)
                    companions_seen.update(int(p) for p in rows["player"][rows["kind"] == 1])
        for path in hour.files("companion"):
            for batch in r.read_file(path):
                dec = batch.get(r.COMPANION_DECISION)
                for row in dec[np.argsort(dec["ms"], kind="stable")]:
                    model = r.text(row["model"]) or "unknown"
                    unit = int(row["companion"])
                    model_of[unit] = model
                    companions_seen.add(unit)
                    owners.setdefault(model, set()).add(int(row["owner"]))
                    decisions[model] = decisions.get(model, 0) + 1
                cmd = batch.get(r.COMPANION_COMMAND)
                commands += [(int(c), int(o), int(k)) for c, o, k in zip(cmd["companion"], cmd["owner"],
                                                                         cmd["command"])]
                rat = batch.get(r.COMPANION_RATING)
                ratings += [(int(c), int(v), int(k)) for c, v, k in zip(rat["companion"], rat["rating"],
                                                                        rat["reason"])]
        for path in hour.files("outcome"):
            for batch in r.read_file(path):
                deaths += [int(p) for p in batch.get(r.DEATH)["player"]]
    models: dict[str, dict] = {}

    def entry(model: str) -> dict:
        return models.setdefault(model, {"companions": set(), "owners": len(owners.get(model, ())),
                                         "decisions": decisions.get(model, 0),
                                         "ratings": {"up": 0, "down": 0, "by_reason": {}}, "commands": {},
                                         "dismissals": 0, "overrides": 0, "deaths": 0})

    for unit in companions_seen:
        entry(model_of.get(unit, "unknown"))["companions"].add(unit)
    for unit, owner, command in commands:
        e = entry(model_of.get(unit, "unknown"))
        name = COMMANDS.get(command, str(command))
        e["commands"][name] = e["commands"].get(name, 0) + 1
        e["dismissals"] += command == 2
        e["overrides"] += command in OVERRIDES
    for unit, rating, reason in ratings:
        e = entry(model_of.get(unit, "unknown"))
        side = "up" if rating > 0 else "down"
        e["ratings"][side] += 1
        by = e["ratings"]["by_reason"].setdefault(REASONS.get(reason, str(reason)), {"up": 0, "down": 0})
        by[side] += 1
    companion_set = companions_seen | set(model_of)
    for unit in deaths:
        if unit in companion_set:
            entry(model_of.get(unit, "unknown"))["deaths"] += 1
    for e in models.values():
        e["companions"] = len(e["companions"])
        per = 1000.0 / e["decisions"] if e["decisions"] else None
        e["overrides_per_1000_decisions"] = round(e["overrides"] * per, 3) if per else None
        e["deaths_per_1000_decisions"] = round(e["deaths"] * per, 3) if per else None
        votes = e["ratings"]["up"] + e["ratings"]["down"]
        e["net_rating"] = round((e["ratings"]["up"] - e["ratings"]["down"]) / votes, 4) if votes else None
    return {"format": 1, "built": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
            "source": {"capture_dir": str(capture), "from": start, "to": end}, "models": dict(sorted(models.items()))}


def write(result: dict, out_dir: str | Path) -> Path:
    path = Path(out_dir) / "human_companions.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=1))
    return path
