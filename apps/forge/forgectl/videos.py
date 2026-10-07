"""`forgectl videos <stage>`: apps/forge/tools/collect-videos.sh, with the workers taken from cluster.toml."""
from __future__ import annotations

import re
import shlex
import subprocess

from . import remote
from .config import Config
from .ui import Failure, say

SCRIPT = "apps/forge/tools/collect-videos.sh"


def argv(config: Config, stage: str, mode: str, on_host: bool) -> list[str]:
    """The script's command line. It is run on the machine that holds the run (the host), where it copies the
    workers' videos into runs/<stage>/videos/from-<worker>/."""
    workers = " ".join(w.target for w in config.workers)
    command = [SCRIPT, *({"copy": [], "check": ["--check"], "dry-run": ["--dry-run"]}[mode]), "--workers", workers,
               "--runs-dir", config.paths["runs"], stage]
    return command


def run(config: Config, stage: str, check: bool, dry_run: bool, on_host: bool) -> int:
    if not re.match(r"^[A-Za-z0-9_]+$", stage):
        raise Failure(f"{stage!r} is not a stage name")
    mode = "check" if check else "dry-run" if dry_run else "copy"
    command = argv(config, stage, mode, on_host)
    if on_host:
        host = config.host
        say(f"Running {SCRIPT} on {host.name}, which holds the run ({mode}) ...")
        quoted = " ".join("./" + c if c == SCRIPT else shlex.quote(c) for c in command)
        script = f"cd {remote.sh_path(host.path)} && {quoted}\n"
        result = remote.on(host, script, timeout=3600)
        say((result.out + result.err).rstrip())
        return 0 if result.ok else 1
    say(f"Running {SCRIPT} here ({mode}); it writes into this checkout's {config.paths['runs']}/{stage}/videos/. "
        "On any machine but the host that is not the run: use --on-host.")
    done = subprocess.run([str(config.repo_root / command[0]), *command[1:]], cwd=config.repo_root)
    return done.returncode
