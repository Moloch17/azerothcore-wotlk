"""Running commands on the machines: ssh with keys only (BatchMode, so a missing key fails instead of prompting) or
locally. Everything goes through `execute`, which is the one function the tests replace."""
from __future__ import annotations

import shlex
import subprocess
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Callable, Iterable, TypeVar

from .config import Machine

T = TypeVar("T")
R = TypeVar("R")


@dataclass
class Result:
    rc: int
    out: str = ""
    err: str = ""
    timed_out: bool = False

    @property
    def ok(self) -> bool:
        return self.rc == 0 and not self.timed_out

    @property
    def unreachable(self) -> bool:
        """ssh itself failed (no route, no key, refused) or the command outlived its timeout."""
        return self.timed_out or self.rc == 255

    def reason(self) -> str:
        if self.timed_out:
            return "timed out"
        if self.rc == 255:
            return "unreachable: " + (self.err.strip().splitlines() or ["ssh failed"])[-1]
        return f"exit {self.rc}" + (": " + self.err.strip().splitlines()[-1] if self.err.strip() else "")


def execute(argv: list[str], input: str | None = None, timeout: float = 30) -> Result:
    """Run argv, text in and out, killed after `timeout` seconds. Never raises for a failing command."""
    try:
        done = subprocess.run(argv, input=input, capture_output=True, text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as expired:
        def text(value):
            return value.decode(errors="replace") if isinstance(value, bytes) else (value or "")
        return Result(124, text(expired.stdout), text(expired.stderr), timed_out=True)
    except FileNotFoundError as error:
        return Result(127, "", str(error))
    return Result(done.returncode, done.stdout, done.stderr)


SSH_OPTIONS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout={connect}", "-o", "ServerAliveInterval=5",
               "-o", "ServerAliveCountMax=3"]


def ssh_argv(machine: Machine, remote: list[str], connect: int = 6, tty: bool = False) -> list[str]:
    options = [option.format(connect=connect) for option in SSH_OPTIONS]
    return ["ssh", *(["-tt", "-e", "none"] if tty else []), *options, machine.target, *remote]


def on(machine: Machine, script: str, timeout: float = 30, connect: int = 6) -> Result:
    """Run a shell script on the machine (bash reading the script from stdin, so no quoting of it is needed)."""
    if machine.local:
        return execute(["bash", "-s"], input=script, timeout=timeout)
    return execute(ssh_argv(machine, ["bash", "-s"], connect), input=script, timeout=timeout)


def sh_path(path: str) -> str:
    """A checkout path for a shell script: a leading ~/ becomes $HOME, the rest is quoted."""
    if path == "~":
        return '"$HOME"'
    if path.startswith("~/"):
        return '"$HOME"/' + shlex.quote(path[2:])
    return shlex.quote(path)


def parallel_map(function: Callable[[T], R], items: Iterable[T]) -> list[R]:
    items = list(items)
    if not items:
        return []
    with ThreadPoolExecutor(max_workers=len(items)) as pool:
        return list(pool.map(function, items))
