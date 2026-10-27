"""`forgectl probe <what>`: `forge probe <what>` on the host (a mechanic probe its running stage carries out in its
next spare episode), then the `Probe <what>:` lines of the host's server log as they land."""
from __future__ import annotations
import time
from . import console, remote
from .config import Config
from .ui import Failure, note, say

PROBES = ("doors",)
POLL_S = 10


def run(config: Config, what: str, wait: int) -> int:
    host = config.host
    line = f"forge probe {what}"
    note(f"{host.name}: typing `{line}` into the console of {config.worldserver} ...")
    result = console.send(config, host, line, timeout=25)
    for text in result.lines:
        say(f"{host.name}: {text}")
    if not result.ok or "armed" not in result.text:
        raise Failure(f"{host.name}: the probe was not armed (is a dungeon stage running there?)")
    log = remote.sh_path(config.path_of(host, "server_log"))
    marker = f"Probe {what}:"
    shown = 0
    deadline = time.time() + max(POLL_S, wait)
    say(f"Waiting up to {wait} s for `{marker}` lines in {host.name}'s server log ...")
    while time.time() < deadline:
        time.sleep(POLL_S)
        tail = remote.on(host, f"grep -a -F '{marker}' {log} 2>/dev/null | tail -n 60", timeout=30)
        lines = [text for text in tail.out.splitlines() if marker in text]
        # The log holds earlier probes too: only the lines of this one, from its arming line on.
        start = max((index for index, text in enumerate(lines) if "checks the doors this episode" in text), default=0)
        lines = lines[start:]
        for text in lines[shown:]:
            say(f"  {text[text.index(marker):]}")
        shown = len(lines)
        if any("checks passed" in text for text in lines):
            failed = sum("FAIL" in text for text in lines)
            say("Probe done." + (f" {failed} check(s) FAILED." if failed else " Every check passed."))
            return 1 if failed else 0
    say(f"No summary within {wait} s: the probe waits for the stage's next spare episode; `forgectl logs` shows the "
        f"`{marker}` lines when they land.")
    return 1
