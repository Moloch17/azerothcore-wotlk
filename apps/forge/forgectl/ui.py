"""What every command prints, and the confirmation before anything that changes state."""
from __future__ import annotations

import re
import sys

from . import audit

ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[()][A-Za-z0-9]|\x1b[=>]")


class Declined(Exception):
    """The operator did not confirm; nothing was changed."""


class Failure(Exception):
    """A command failed; the message is printed and the exit code is 1."""


def strip_ansi(text: str) -> str:
    return ANSI.sub("", text)


def say(message: str = "") -> None:
    print(message, flush=True)


def note(message: str) -> None:
    print(f"forgectl: {message}", flush=True)


def ask(prompt: str) -> str:  # replaced in tests
    return input(prompt)


def confirm(plan: list[str], yes: bool, what: str = "Proceed") -> None:
    """Print what is about to happen, then ask (unless --yes). Raises Declined."""
    say("This will:")
    for line in plan:
        say(f"  - {line}")
    if yes:
        say("(--yes: not asking)")
        audit.confirmed("--yes")
        return
    if not sys.stdin.isatty():
        audit.confirmed("declined")
        raise Declined("not a terminal, so no way to ask; pass --yes to go ahead")
    answer = ask(f"{what}? [y/N] ").strip().lower()
    if answer not in ("y", "yes"):
        audit.confirmed("declined")
        raise Declined("not confirmed")
    audit.confirmed("prompt")


def table(headers: list[str], rows: list[list[str]]) -> str:
    widths = [max(len(str(row[i])) for row in [headers, *rows]) for i in range(len(headers))]
    lines = ["  ".join(str(cell).ljust(widths[i]) for i, cell in enumerate(row)).rstrip() for row in [headers, *rows]]
    return "\n".join(lines)
