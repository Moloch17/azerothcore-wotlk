"""`forgectl stage ...` and `forgectl status`: the worldserver's `forge ...` console commands, sent reliably."""
from __future__ import annotations

import re

from . import console, remote
from .config import Config, Machine
from .ui import Declined, Failure, confirm, note, say

STAGE_NAME = re.compile(r"^[A-Za-z0-9_]+$")
ACTIONS = ("status", "start", "resume", "pause", "cancel")
# What each action does, for the confirmation. pause and cancel also go to every worker (the host's pause does not
# reach them today).
EFFECT = {
    "start": "start {stages} fresh on the host (the previous run of that stage is archived)",
    "resume": "continue {stages} from its latest.pt on the host (or unpause the plan if none is named)",
    "pause": "freeze the sim and the learner after the current decision, on the host and on every worker",
    "cancel": "stop the plan on the host and every worker; the learner saves latest.pt first",
}
LEARNER_KEYS = ("value_loss", "entropy", "episode_score_outcome")


def check_names(names: list[str]) -> None:
    for name in names:
        if not STAGE_NAME.match(name):
            raise Failure(f"{name!r} is not a stage name (letters, digits and underscores only)")


def console_line(action: str, stages: list[str]) -> str:
    return " ".join(["forge", action, *stages])


def send_checked(config: Config, machine: Machine, line: str, timeout: float = 25) -> console.ConsoleResult:
    note(f"{machine.name}: typing `{line}` into the console of {config.worldserver} ...")
    result = console.send(config, machine, line, timeout=timeout)
    if not result.ok:
        raise Failure(f"{machine.name}: the console did not answer `{line}` with its prompt within {timeout:.0f} s"
                      + (f"; got: {result.text[:300]!r}" if result.lines else " (no reply at all: is the "
                         "worldserver container up, and is ssh working?)"))
    return result


def show(machine: Machine, result: console.ConsoleResult) -> None:
    say(f"--- {machine.name} ---")
    say(result.text if result.lines else "(no reply text: the command was accepted silently)")


def run(config: Config, action: str, stages: list[str], yes: bool) -> int:
    check_names(stages)
    host = config.host
    if action == "status":
        show(host, send_checked(config, host, "forge status"))
        return 0
    if action == "start" and not stages:
        raise Failure("forgectl stage start needs a stage name (it would otherwise start the whole configured queue)")
    targets = [host] + (config.workers if action in ("pause", "cancel") else [])
    if action in ("pause", "cancel") and stages:
        status = send_checked(config, host, "forge status")
        if not any(stage in status.text for stage in stages):
            raise Failure(f"the host's `forge status` does not mention {', '.join(stages)}; the running plan is: "
                          f"{status.lines[0] if status.lines else 'unknown'}. pause and cancel act on the whole plan, "
                          "so name the running stage or none.")
    plan = [EFFECT[action].format(stages=", ".join(stages) or "the plan")]
    plan += [f"type `{console_line(action, stages)}` into the console of {config.worldserver} on "
             f"{', '.join(m.name for m in targets)}"]
    try:
        confirm(plan, yes)
    except Declined as declined:
        say(f"Nothing was sent ({declined}).")
        return 1
    failed = []
    for machine in targets:
        try:
            show(machine, send_checked(config, machine, console_line(action, stages)))
        except Failure as failure:
            say(f"FAILED {failure}")
            failed.append(machine.name)
            if machine is host:
                break  # the plan did not take: do not touch the workers on its behalf
    if failed:
        say(f"Not sent or not answered on: {', '.join(failed)}. Check `forgectl cluster`.")
        return 1
    say(f"Done: `{console_line(action, stages)}` was accepted on {', '.join(m.name for m in targets)}.")
    return 0


def parse_learner_line(line: str) -> dict:
    """`update N | steps S | R sps | rollout .. compute .. | key v, key v, ...` into a dict of the headline values."""
    parts = [part.strip() for part in line.split(" | ")]
    out: dict = {}
    for part in parts:
        match = re.match(r"^(update|steps) (\d+)$", part)
        if match:
            out[match.group(1)] = int(match.group(2))
            continue
        match = re.match(r"^([\d.]+) sps$", part)
        if match:
            out["sps"] = float(match.group(1))
            continue
        if part.startswith("rollout"):
            out["timing"] = part
            continue
        for pair in part.split(", "):
            key, _, value = pair.rpartition(" ")
            if key:
                out[key] = value
    return out


def learner_headline(config: Config, machine: Machine) -> str:
    path = remote.sh_path(config.path_of(machine, "learner_log"))
    script = f"tail -c 400000 {path} 2>/dev/null | grep -a '^update [0-9]* | steps' | tail -1\n"
    result = remote.on(machine, script, timeout=30)
    if not result.ok:
        return f"learner log of {machine.name}: could not be read ({result.reason()})"
    line = result.out.strip()
    if not line:
        return f"learner log of {machine.name}: no update line yet"
    return headline_from_line(machine.name, line)


def headline_from_line(name: str, line: str) -> str:
    data = parse_learner_line(line)
    parts = [f"update {data.get('update', '?'):,}", f"steps {data.get('steps', 0):,}",
             f"{data.get('sps', 0):,.0f} steps/s", data.get("timing", "")]
    parts += [f"{key} {data[key]}" for key in LEARNER_KEYS if key in data]
    return "learner (" + name + "): " + " | ".join(p for p in parts if p)


def status(config: Config) -> int:
    host = config.host
    try:
        show(host, send_checked(config, host, "forge status"))
    except Failure as failure:
        say(f"FAILED {failure}")
        say(learner_headline(config, host))
        return 1
    say()
    say(learner_headline(config, host))
    return 0
