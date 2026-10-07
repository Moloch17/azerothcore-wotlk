"""The audit log: one line in ~/.forgectl/audit.log for every command that changes something.

A line holds the time, the local user, the machines the command touched, whether `--yes` answered the question or a
person did at the prompt (or declined it), the outcome (done, failed, declined) and the command line. Commands that
only read (cluster, status, logs, test, `conf-sync --check`, `stage status`, `videos --check`) are not logged.

Two lines per command, one format, a `kind=` field says which. `kind=intent` is written when the command starts, before
it touches anything: the machines it is to touch, and `confirm=--yes` or `confirm=prompt-pending`. `kind=result` is
written when it ends, with the confirmation as it turned out and the outcome. A command killed by SIGKILL (or a power
cut) therefore still leaves its intent line and no result line. The log is checked for being writable before the
command starts (the intent line is the write), and the command is refused if it is not: a change nobody can trace is
not made.
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
    planned: list[str] = field(default_factory=list)   # the machines named in the intent line
    yes_given: bool = False


current: Entry | None = None


class AuditError(Exception):
    pass


def log_path() -> Path:
    return forgectl_home() / LOG_NAME


def begin(argv: list[str], yes: bool = False) -> Entry:
    """Start the entry for a state-changing command, after checking the log can be appended to."""
    global current
    path = log_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        os.close(os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600))
    except OSError as error:
        raise AuditError(f"cannot write the audit log {path} ({error}); a command that changes a machine is not "
                         "run without a record") from None
    current = Entry("forgectl " + shlex.join(argv), yes_given=yes)
    return current


def intent(entry: Entry, machines) -> None:
    """Write the intent line: the command is about to start. Raises AuditError if it cannot be written."""
    for machine in machines:
        name = getattr(machine, "name", machine)
        if name not in entry.planned:
            entry.planned.append(name)
    how = "--yes" if entry.yes_given else "prompt-pending"
    try:
        append(format_line(entry, kind="intent", machines=entry.planned, confirm=how))
    except OSError as error:
        raise AuditError(f"cannot write the audit log {log_path()} ({error}); a command that changes a machine is "
                         "not run without a record") from None


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


def format_line(entry: Entry, outcome: str | None = None, when: float | None = None, kind: str = "result",
                machines: list[str] | None = None, confirm: str | None = None) -> str:
    stamp = time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(when))
    names = entry.machines if machines is None else machines
    line = (f"{stamp} kind={kind} user={username()} machines={','.join(names) or '-'} "
            f"confirm={entry.confirmation if confirm is None else confirm} ")
    if kind == "result":
        line += f"outcome={outcome} "
    line += f"cmd={json.dumps(entry.command)}"
    if kind == "result" and entry.notes:
        line += " notes=" + json.dumps("; ".join(entry.notes))
    return line


def append(line: str) -> None:
    # one O_APPEND write: lines from two forgectl runs at once do not interleave
    descriptor = os.open(log_path(), os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
    try:
        os.write(descriptor, (line + "\n").encode())
    finally:
        os.close(descriptor)


def finish(entry: Entry, outcome: str) -> None:
    global current
    current = None
    raised = sys.exc_info()[0]
    if raised is not None and raised is not KeyboardInterrupt:
        entry.notes.append(f"ended by {raised.__name__}")
    line = format_line(entry, outcome)
    try:
        append(line)
    except OSError as error:
        print(f"forgectl: WARNING: could not write the audit line ({error}): {line}", file=sys.stderr)
