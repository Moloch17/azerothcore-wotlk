"""Typing a line into a worldserver's console, the way a person does it with `docker attach`.

Ported from the gitignored var/forge_console.py. The rules it keeps:
  - the line is typed into an attached console under a pty, and the session is left with Ctrl-P Ctrl-Q;
  - stdin is never closed: end-of-file on the console shuts the server down, so the detach keys are sent in a `finally`
    and the process is only ever ended after that;
  - `--sig-proxy=false`: a signal sent to the attach client must not be forwarded to the worldserver.
New here: the output is read until the console prompt ("AC> ") comes back after the command (the console streams log
noise all the time, so "until quiet" never happens on a busy host), with a hard timeout; ANSI is stripped; the coloured
log lines that interleave with the reply are dropped; and it works through ssh for the workers.
"""
from __future__ import annotations

import os
import pty
import re
import select
import signal
import time
from dataclasses import dataclass

from . import remote
from .config import Config, Machine
from .ui import strip_ansi

PROMPT = "AC> "
DETACH_KEYS = b"\x10\x11"  # Ctrl-P Ctrl-Q
# A log line starts with a colour escape (the logger colours by level); a command's reply is plain text.
LOG_LINE = re.compile(r"^(?:\x1b\[[0-9;]*m)+")


@dataclass
class ConsoleResult:
    lines: list[str]      # the reply, ANSI stripped, log noise and the prompt removed
    prompt_seen: bool     # the console came back with its prompt: the command finished
    started: bool         # the typed line was seen echoed (we were really attached to a console)
    raw: str = ""

    @property
    def text(self) -> str:
        return "\n".join(self.lines)

    @property
    def ok(self) -> bool:
        return self.prompt_seen


def attach_argv(config: Config, machine: Machine) -> list[str]:
    attach = ["docker", "attach", "--sig-proxy=false", "--detach-keys=ctrl-p,ctrl-q", config.worldserver]
    if machine.local:
        return attach
    return remote.ssh_argv(machine, attach, tty=True)


def spawn(argv: list[str]) -> tuple[int, int]:
    """Start argv under a pty and return (pid, master fd). The tests replace this with a fake console."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(argv[0], argv)
    return pid, fd


def _drain(fd: int, seconds: float, until=None) -> bytes:
    out = b""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
            if until and until(out):
                break
    return out


def parse_reply(raw: str, line: str) -> ConsoleResult:
    """The reply to `line` out of everything the console printed after it was typed."""
    cleaned_lines = []
    for raw_line in raw.replace("\r", "").split("\n"):
        if LOG_LINE.match(raw_line):
            continue
        cleaned_lines.append(strip_ansi(raw_line))
    # the echo of the typed line (the console shows it first); the reply is what follows, up to a line that is the
    # prompt alone. The prompt is also redrawn in front of the reply's first line, so a leading prompt is stripped.
    start = next((i for i, text in enumerate(cleaned_lines) if line in text), None)
    body = cleaned_lines[start + 1:] if start is not None else cleaned_lines
    prompt_seen = False
    reply = []
    for text in body:
        stripped = text.lstrip()
        if stripped.startswith(PROMPT.strip()) and stripped[len(PROMPT.strip()):].strip() == "":
            prompt_seen = True  # the prompt alone on its line: the command is done
            break
        if text.startswith(PROMPT):
            text = text[len(PROMPT):]
        reply.append(text.rstrip())
    while reply and not reply[0].strip():
        reply.pop(0)
    while reply and not reply[-1].strip():
        reply.pop()
    return ConsoleResult(reply, prompt_seen, start is not None, raw)


def send(config: Config, machine: Machine, line: str, timeout: float = 20, settle: float = 1.5) -> ConsoleResult:
    """Type `line` into the machine's worldserver console and return the reply. `timeout` is for the reply."""
    pid, fd = spawn(attach_argv(config, machine))
    raw = b""
    try:
        raw += _drain(fd, settle)  # the screen as it was when we attached, and the connection's own noise
        if remote_failed(raw):
            return parse_reply(raw.decode(errors="replace"), line)
        os.write(fd, (line + "\n").encode())
        typed = b""

        def finished(chunk: bytes) -> bool:
            return parse_reply((typed + chunk).decode(errors="replace"), line).prompt_seen

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            more = _drain(fd, min(1.0, deadline - time.monotonic()), until=None)
            if not more:
                if not alive(pid):
                    break
                continue
            typed += more
            if finished(b""):
                typed += _drain(fd, 0.4)  # the prompt can arrive a line before the last of the reply
                break
        raw += typed
    finally:
        detach(pid, fd)
    return parse_reply(raw.decode(errors="replace"), line)


def remote_failed(raw: bytes) -> bool:
    text = raw.decode(errors="replace")
    return any(word in text for word in ("Error response from daemon", "Permission denied", "Could not resolve",
                                          "Connection refused", "No route to host", "Connection timed out"))


def alive(pid: int) -> bool:
    try:
        done, _ = os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        return False
    return done == 0


def detach(pid: int, fd: int) -> None:
    """Ctrl-P Ctrl-Q, then wait for the client to leave. Only a client that ignores that is terminated, and the fd is
    closed last: closing it earlier would be end-of-file on the console."""
    try:
        os.write(fd, DETACH_KEYS)
    except OSError:
        pass
    end = time.monotonic() + 4
    while time.monotonic() < end:
        try:
            _drain(fd, 0.2)
        except (OSError, ValueError):
            pass
        if not alive(pid):
            break
    else:
        try:
            os.kill(pid, signal.SIGTERM)
            os.waitpid(pid, 0)
        except (ProcessLookupError, ChildProcessError, OSError):
            pass
    try:
        os.close(fd)
    except OSError:
        pass
