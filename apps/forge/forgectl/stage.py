"""`forgectl stage ...` and `forgectl status`: the worldserver's `forge ...` console commands, sent reliably."""
from __future__ import annotations

import re
from dataclasses import dataclass

from . import audit, console, remote
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
        raise Failure(f"{machine.name}: the console did not finish answering `{line}` within {timeout:.0f} s"
                      + (f"; got: {result.text[:300]!r}" if result.lines else " (no reply at all: is the "
                         "worldserver container up, and is ssh working?)"))
    return result


ARCHIVE_TOKEN_STEPS = 1_000_000   # a run of at most this many steps is archived without --archive-ok under --yes


@dataclass
class ExistingRun:
    """What `forge start <stage>` would archive: the host's runs/<stage>/ directory. exists is None when the host
    could not be read; steps is None when the run's step count could not be."""
    stage: str
    exists: bool | None
    steps: int | None = None
    why: str = ""

    @property
    def big(self) -> bool:
        """Archiving it needs --archive-ok under --yes: more than a token run, or one that cannot be measured."""
        return self.exists is not False and (self.steps is None or self.steps > ARCHIVE_TOKEN_STEPS)

    def describe(self, host: str) -> str:
        if self.exists is False:
            return f"{self.stage} has no run on {host}; nothing is archived"
        if self.exists is None:
            return (f"{self.stage}: the run on {host} could not be read ({self.why}); if there is one, start "
                    "archives it")
        if self.steps is None:
            return (f"{self.stage} has a run on {host} whose step count could not be read ({self.why}); start "
                    "archives it")
        size = f"{self.steps / 1e6:.0f}M steps" if self.steps >= 1_000_000 else f"{self.steps:,} steps"
        return f"{self.stage} has a run at {size}; start archives it"


def run_info_script(runs_dir: str, stage: str) -> str:
    """Shell: does runs/<stage> hold anything (the learner archives a non-empty directory), and the env_steps column of
    the last row of its metrics.csv (the learner's own record of how far the run got)."""
    path = remote.sh_path(f"{runs_dir}/{stage}")
    return (f"d={path}\nif [ -d \"$d\" ] && [ -n \"$(ls -A \"$d\" 2>/dev/null)\" ]; then\n  echo exists=1\n"
            "  echo steps=$(awk -F, 'NR==1{for(i=1;i<=NF;i++)if($i==\"env_steps\")c=i} END{if(c)print $c}' "
            "\"$d/metrics.csv\" 2>/dev/null)\nelse\n  echo exists=0\nfi\n")


def existing_run(config: Config, name: str) -> ExistingRun:
    host = config.host
    result = remote.on(host, run_info_script(config.path_of(host, "runs"), name), timeout=30)
    if not result.ok:
        return ExistingRun(name, None, why=result.reason())
    values = dict(line.split("=", 1) for line in result.out.splitlines() if "=" in line)
    if values.get("exists") != "1":
        return ExistingRun(name, False)
    steps = values.get("steps", "").strip()
    if steps.isdigit():
        return ExistingRun(name, True, int(steps))
    return ExistingRun(name, True, why="no env_steps in its metrics.csv")


def check_archives(config: Config, stages: list[str], yes: bool, archive_ok: bool) -> list[str]:
    """The plan lines saying what `start` archives (also put in the audit record). Under --yes, archiving a run of more
    than a token size needs --archive-ok: nobody is asked, so the operator must have said so."""
    lines, big = [], []
    for name in stages:
        run = existing_run(config, name)
        line = run.describe(config.host.name)
        lines.append(line)
        audit.note(line)
        if run.big:
            big.append(line)
    if big and yes and not archive_ok:
        for line in big:
            say(line)
        raise Failure("--yes does not archive a run of more than 1M steps (or one that cannot be measured) without "
                      "--archive-ok: " + "; ".join(big) + ". Check it is the run you mean to archive, then add "
                      "--archive-ok (or leave out --yes and answer the prompt). Nothing was sent.")
    return lines


def host_plan_state(config: Config) -> tuple[str, str]:
    """The plan state of the host: see machine_plan_state."""
    return machine_plan_state(config, config.host)


def machine_plan_state(config: Config, machine: Machine) -> tuple[str, str]:
    """("idle" | "running" | "down" | "unknown", the first line of the machine's `forge status`). "down" is a
    worldserver container that is not running (nothing can be training); "unknown" is a console that did not answer
    while the container is up, which is not evidence that nothing runs."""
    host = machine
    try:
        status = send_checked(config, host, "forge status")
    except Failure as failure:
        up = remote.on(host, f"docker ps -q --filter name=^/{config.worldserver}$ --filter status=running\n")
        if up.ok and not up.out.strip():
            return "down", f"the worldserver container is not running on {host.name}"
        return "unknown", str(failure)
    first = status.lines[0] if status.lines else ""
    return ("idle" if "idle" in first.lower() else "running"), first


def show(machine: Machine, result: console.ConsoleResult) -> None:
    say(f"--- {machine.name} ---")
    say(result.text if result.lines else "(no reply text: the command was accepted silently)")


def run(config: Config, action: str, stages: list[str], yes: bool, archive_ok: bool = False) -> int:
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
    if action == "start":
        plan += check_archives(config, stages, yes, archive_ok)
    plan += [f"type `{console_line(action, stages)}` into the console of {config.worldserver} on "
             f"{', '.join(m.name for m in targets)}"]
    try:
        confirm(plan, yes)
    except Declined as declined:
        say(f"Nothing was sent ({declined}).")
        return 1
    audit.touch(targets)
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
