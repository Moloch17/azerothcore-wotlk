"""The conf keys that decide what the sim costs and sees, read from a machine's mod_animus_forge.conf.

Two groups. MUST MATCH on every machine, because the workers' data has to mean what the host's learner thinks it means:
the camera (AnimusForge.Vision.*), the map sense (AnimusForge.Map.*), the memory (AnimusForge.Memory.*), the ticks of a
decision (AnimusForge.TicksPerDecision and every AnimusForge.Stage.<stage>.TicksPerDecision) and AnimusForge.HalfBatch.
MAY DIFFER, shown so a deploy cannot change the cost silently (audit finding O1): AnimusForge.Envs,
AnimusForge.Learner.Cpus, MapUpdate.Threads. A key that is not in the conf takes the build's default; here it is None.
"""
from __future__ import annotations

import re

MUST_MATCH = re.compile(r"^AnimusForge\.(Vision\.[A-Za-z0-9_.]+|Map\.[A-Za-z0-9_.]+|Memory\.[A-Za-z0-9_.]+|"
                        r"TicksPerDecision|Stage\.[A-Za-z0-9_]+\.TicksPerDecision|HalfBatch)$")
MAY_DIFFER = ("AnimusForge.Envs", "AnimusForge.Learner.Cpus", "MapUpdate.Threads")
STAGE_TICKS = re.compile(r"^AnimusForge\.Stage\.([A-Za-z0-9_]+)\.TicksPerDecision$")
LINE = re.compile(r"^\s*([A-Za-z][A-Za-z0-9_.]*)\s*=\s*(.*?)\s*$")

# The shell filter the probe runs on a conf (so a machine sends a few lines, not the whole file).
GREP = (r"grep -aE '^[[:space:]]*(AnimusForge\.(Vision|Map|Memory)\.|AnimusForge\.(Stage\.[A-Za-z0-9_]+\.)?"
        r"TicksPerDecision[[:space:]]*=|AnimusForge\.(HalfBatch|Envs|Learner\.Cpus)[[:space:]]*=|"
        r"MapUpdate\.Threads[[:space:]]*=)'")


def scan(text: str) -> dict[str, str]:
    """key -> value of every uncommented line of a key above (the last one wins, as the config manager reads it).
    A trailing `# comment` is cut off; quotes stay."""
    keys = {}
    for line in text.splitlines():
        match = LINE.match(line)
        if not match:
            continue
        key, value = match.group(1), re.sub(r"\s+#.*$", "", match.group(2)).strip()
        if MUST_MATCH.match(key) or key in MAY_DIFFER:
            keys[key] = value
    return keys


def unquote(value: str | None):
    """A conf value as a JSON value: a number if it is one, else the text without quotes; None if the key is unset."""
    if value is None:
        return None
    text = value.strip().strip('"')
    for kind in (int, float):
        try:
            return kind(text)
        except ValueError:
            continue
    return text


def cadence(keys: dict[str, str]) -> dict:
    """The sim's cost knobs of a machine: ticks per decision, half-batch, envs, learner cpus, map threads, and the
    per-stage tick overrides."""
    stage_ticks = {m.group(1): unquote(v) for k, v in sorted(keys.items()) if (m := STAGE_TICKS.match(k))}
    return {"ticks_per_decision": unquote(keys.get("AnimusForge.TicksPerDecision")),
            "half_batch": unquote(keys.get("AnimusForge.HalfBatch")),
            "envs": unquote(keys.get("AnimusForge.Envs")),
            "learner_cpus": unquote(keys.get("AnimusForge.Learner.Cpus")),
            "map_update_threads": unquote(keys.get("MapUpdate.Threads")),
            "stage_ticks": stage_ticks}


def mismatches(host: dict[str, str], other: dict[str, str]) -> list[str]:
    """Each must-match key whose value on `other` differs from the host's (an unset key is shown as `unset`)."""
    out = []
    for key in sorted({k for k in (*host, *other) if MUST_MATCH.match(k)}):
        a, b = host.get(key), other.get(key)
        if a != b:
            out.append(f"{key.removeprefix('AnimusForge.')}: host {a if a is not None else 'unset'}, "
                       f"here {b if b is not None else 'unset'}")
    return out


def short(keys: dict[str, str]) -> list[str]:
    """The cells of the cluster / conf-sync table: ticks, half-batch, envs, cpus (`-` = unset)."""
    c = cadence(keys)
    cells = [c["ticks_per_decision"], c["half_batch"], c["envs"], c["learner_cpus"]]
    return ["-" if v is None else str(v) for v in cells]
