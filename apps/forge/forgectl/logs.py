"""`forgectl logs [machine] [--errors]`: the worldserver's output and the learner's log, problems first."""
from __future__ import annotations

import re

from . import remote, stage
from .config import Config
from .ui import Failure, say, strip_ansi

PROBLEM = re.compile(r"\b(error|errors|warn|warning|fatal|exception|traceback|crash\w*|segfault|abort\w*|killed|"
                     r"refused the worker|out of memory|OOM)\b|Traceback|\bcore dumped", re.IGNORECASE)
# Lines that match PROBLEM but are the sim's ordinary chatter (a refused key press is a learning signal, not a fault).
NOISE = re.compile(r"Press refused|Can't set process priority|error=0\b|errors? 0\b|0 errors|move_refused|\.refused")

SCRIPT = r"""
cd {path} 2>/dev/null || {{ echo "##NOPATH"; exit 0; }}
echo "##WORLDSERVER"
docker logs --tail {tail} {container} 2>&1
echo "##ERRORS"
tail -n 200 {errors_log} 2>/dev/null
echo "##LEARNER"
tail -c 4000000 {learner_log} 2>/dev/null | grep -a -v '^update [0-9]* | steps' | tail -n {tail} | cut -c1-600
echo "##LASTUPDATE"
tail -c 400000 {learner_log} 2>/dev/null | grep -a '^update [0-9]* | steps' | tail -1 
"""

TITLES = {"WORLDSERVER": "worldserver (docker logs)", "ERRORS": "Errors.log", "LEARNER": "learner log (the update lines left out)"}


def split_sections(out: str) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = {}
    current = None
    for line in out.splitlines():
        if line.startswith("##") and line[2:].isupper():
            current = line[2:]
            sections[current] = []
        elif current:
            sections[current].append(strip_ansi(line.replace("\r", "")))
    return sections


def problems(lines: list[str]) -> list[str]:
    """Lines that look like trouble, with a Python traceback kept whole (the lines that follow its header)."""
    found: list[str] = []
    in_traceback = False
    for line in lines:
        if "Traceback (most recent call last)" in line:
            in_traceback = True
            found.append(line)
        elif in_traceback and (line.startswith((" ", "\t")) or line.strip() == ""):
            if line.strip():
                found.append(line)
        else:
            in_traceback = False
            if PROBLEM.search(line) and not NOISE.search(line):
                found.append(line)
    return found


def clip(line: str, wide: bool) -> str:
    return line if wide or len(line) <= 220 else line[:217] + "..."


def run(config: Config, name: str | None, errors_only: bool, lines: int, wide: bool) -> int:
    machine = config.machine(name) if name else config.host
    script = SCRIPT.format(path=remote.sh_path(machine.path), container=config.worldserver, tail=max(lines, 40) * 50,
                           errors_log=remote.sh_path(config.path_of(machine, "errors_log")),
                           learner_log=remote.sh_path(config.path_of(machine, "learner_log")))
    say(f"Reading the logs of {machine.name} ({machine.target}) ...")
    result = remote.on(machine, script, timeout=90)
    if result.unreachable or (not result.ok and not result.out):
        raise Failure(f"{machine.name}: {result.reason()}")
    sections = split_sections(result.out)
    if "NOPATH" in sections:
        raise Failure(f"{machine.name}: no checkout at {machine.path}")
    total = 0
    say(f"== PROBLEMS on {machine.name} (errors, warnings, tracebacks; the last {lines} of each source) ==")
    for key, title in TITLES.items():
        found = problems(sections.get(key, []))[-lines:]
        total += len(found)
        if found:
            say(f"-- {title}: {len(found)} line(s)")
            for line in found:
                say("   " + clip(line, wide))
    if not total:
        say("(none found)")
    last = (sections.get("LASTUPDATE") or [""])[0].strip()
    say("== " + (stage.headline_from_line(machine.name, last) if last else "no learner update line yet"))
    if not errors_only:
        for key, title in TITLES.items():
            recent = [line for line in sections.get(key, []) if line.strip()][-lines:]
            say(f"== {title}: the last {len(recent)} line(s)")
            for line in recent:
                say("   " + clip(line, wide))
    return 0
