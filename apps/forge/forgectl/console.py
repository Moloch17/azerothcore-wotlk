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

import fcntl
import os
import pty
import re
import select
import signal
import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass

from . import remote
from .config import Config, Machine
from .home import forgectl_home
from .ui import Failure, note, strip_ansi

PROMPT = "AC> "
DETACH_KEYS = b"\x10\x11"  # Ctrl-P Ctrl-Q
LOCK_TIMEOUT = 90.0   # seconds a send waits for another forgectl that is typing into the same console
# A log line starts with a colour escape (the logger colours by level); a command's reply is plain text.
PRIVATE_MODE = re.compile(r"\x1b\[\?[0-9;]*[hl]")  # bracketed paste on/off, which readline writes around a line
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
        if LOG_LINE.match(PRIVATE_MODE.sub("", raw_line)):
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


class Terminated(SystemExit):
    """SIGTERM or SIGHUP arrived during a console send. A SystemExit, so it ends the program with 128+signal once the
    detach in `send`'s `finally` has run (and is not mistaken for a failure of the command)."""

    def __init__(self, signum: int):
        super().__init__(128 + signum)
        self.signum = signum


class SignalGuard:
    """For the length of a send, SIGTERM and SIGHUP raise `Terminated` instead of killing the process outright.

    Python does not run `finally` blocks when the default SIGTERM handler ends the process, so without this a kill
    in the middle of a send would leave a `docker attach` client dangling on the worldserver's console. The detach
    itself is shielded: a signal during it is held back and raised afterwards, so Ctrl-P Ctrl-Q is never cut short.
    Only the main thread can install signal handlers; elsewhere the guard does nothing."""

    SIGNALS = (signal.SIGTERM, signal.SIGHUP)

    def __init__(self):
        self.shielded = False
        self.pending: int | None = None
        self.fired = False
        self.previous: dict = {}

    def _handle(self, signum, frame):
        if self.shielded:
            self.pending = signum
            return
        if self.fired:
            return
        self.fired = True
        raise Terminated(signum)

    def __enter__(self):
        if threading.current_thread() is threading.main_thread():
            self.previous = {number: signal.signal(number, self._handle) for number in self.SIGNALS}
        return self

    def __exit__(self, exc_type, exc, traceback):
        for number, handler in self.previous.items():
            signal.signal(number, handler)
        if self.pending is not None and exc is None:
            raise Terminated(self.pending)
        return False


def lock_path(machine: Machine):
    return forgectl_home() / "locks" / f"{machine.name}.lock"


@contextmanager
def machine_lock(machine: Machine, timeout: float = LOCK_TIMEOUT):
    """One forgectl at a time types into a machine's console: two at once would interleave their characters. An
    exclusive flock on ~/.forgectl/locks/<machine>.lock, held for the whole send and released by the kernel if the
    process dies. Waits up to `timeout` seconds (saying so), then raises Failure."""
    path = lock_path(machine)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)   # not inherited by the attach client (close-on-exec)
    try:
        deadline = time.monotonic() + timeout
        announced = False
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    holder = os.pread(descriptor, 200, 0).decode(errors="replace").strip() or "unknown"
                    raise Failure(f"another forgectl (holder: {holder}) is typing into {machine.name}'s console and "
                                  f"did not finish within {timeout:.0f} s; nothing was sent. Two runs at once would "
                                  f"interleave their characters. Try again when it is done (lock file "
                                  f"{path}).") from None
                if not announced:
                    note(f"another forgectl is typing into {machine.name}'s console; waiting up to {timeout:.0f} s "
                         f"for it (lock file {path}) ...")
                    announced = True
                time.sleep(0.2)
        os.ftruncate(descriptor, 0)
        os.pwrite(descriptor, f"pid {os.getpid()}".encode(), 0)
        yield
    finally:
        os.close(descriptor)


def send(config: Config, machine: Machine, line: str, timeout: float = 20, settle: float = 1.5,
         lock_timeout: float = LOCK_TIMEOUT) -> ConsoleResult:
    """Type `line` into the machine's worldserver console and return the reply. `timeout` is for the reply;
    `lock_timeout` for waiting on another forgectl that is using the same console."""
    with SignalGuard() as guard, machine_lock(machine, lock_timeout):
        pid = fd = None
        raw = b""
        try:
            pid, fd = spawn(attach_argv(config, machine))
            raw += _drain(fd, settle)  # the screen as it was when we attached, and the connection's own noise
            if not remote_failed(raw):
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
            guard.shielded = True   # from here a signal waits for the detach to finish (see SignalGuard)
        finally:
            if pid is not None:
                guard.shielded = True
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
