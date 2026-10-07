"""The audit log: one line in ~/.forgectl/audit.log for every command that changes something.

A line holds the time, the local user, the machines the command touched, whether `--yes` answered the question or a
person did at the prompt (or declined it), the outcome (done, failed, declined) and the command line. Commands that
only read (cluster, status, logs, test, `conf-sync --check`, `stage status`, `videos --check`) are not logged.

The line is written when the command ends. The log is checked for being writable *before* a state-changing command
starts, and the command is refused if it is not: a change nobody can trace is not made. A command killed by SIGKILL (or
a power cut) leaves no line; SIGTERM and SIGHUP during a console send do (they end the command through `finally`).
"""
from __future__ import annotations

import getpass
import json
import os
import shlex
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from .home import forgectl_home

LOG_NAME = "audit.log"
NOT_REACHED = "not-reached"   # the command ended before it asked (a refusal, a bad argument)


@dataclass
class Entry:
    command: str
    machines: list[str] = field(default_factory=list)
    confirmation: str = NOT_REACHED     # --yes | prompt | declined | not-reached
    notes: list[str] = field(default_factory=list)


current: Entry | None = None


class AuditError(Exception):
    pass


def log_path() -> Path:
    return forgectl_home() / LOG_NAME


def begin(argv: list[str]) -> Entry:
    """Start the entry for a state-changing command, after checking the log can be appended to."""
    global current
    path = log_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        os.close(os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600))
    except OSError as error:
        raise AuditError(f"cannot write the audit log {path} ({error}); a command that changes a machine is not "
                         "run without a record") from None
    current = Entry("forgectl " + shlex.join(argv))
    return current


def touch(machines) -> None:
    """The machines the command is changing (names or Machine objects), added to the entry."""
    if current is None:
        return
    for machine in machines:
        name = getattr(machine, "name", machine)
        if name not in current.machines:
            current.machines.append(name)


def note(text: str) -> None:
    if current is not None and text not in current.notes:
        current.notes.append(text)


def confirmed(how: str) -> None:
    """How the first question of the command was answered; later questions (the sub-steps of move-host) do not
    overwrite it."""
    if current is not None and current.confirmation == NOT_REACHED:
        current.confirmation = how


def username() -> str:
    try:
        return getpass.getuser()
    except (KeyError, OSError):
        return f"uid{os.getuid()}"


def format_line(entry: Entry, outcome: str, when: float | None = None) -> str:
    stamp = time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(when))
    line = (f"{stamp} user={username()} machines={','.join(entry.machines) or '-'} confirm={entry.confirmation} "
            f"outcome={outcome} cmd={json.dumps(entry.command)}")
    if entry.notes:
        line += " notes=" + json.dumps("; ".join(entry.notes))
    return line


def finish(entry: Entry, outcome: str) -> None:
    global current
    current = None
    raised = sys.exc_info()[0]
    if raised is not None and raised is not KeyboardInterrupt:
        entry.notes.append(f"ended by {raised.__name__}")
    line = format_line(entry, outcome) + "\n"
    try:
        # one O_APPEND write: lines from two forgectl runs at once do not interleave
        descriptor = os.open(log_path(), os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
        try:
            os.write(descriptor, line.encode())
        finally:
            os.close(descriptor)
    except OSError as error:
        print(f"forgectl: WARNING: could not write the audit line ({error}): {line.strip()}", file=sys.stderr)
