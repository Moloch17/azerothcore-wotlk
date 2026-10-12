"""The `forgectl watch --resume` that `forgectl stage start|resume` keeps running on the host (one, by its pid file).

It is not stopped by `stage cancel`: the cancel sets the watch hold (stage.hold), so the watcher goes on telling what
happens and leaves the plan alone until the next start or resume. Stop it by hand with `kill $(cat
~/.forgectl/watch-loop.pid)`.
"""
from __future__ import annotations

from . import remote
from .config import Config
from .ui import say

PID = '"$HOME"/.forgectl/watch-loop.pid'
LOG = '"$HOME"/.forgectl/watch-loop.log'
ALIVE = (f'p=$(cat {PID} 2>/dev/null) && tr "\\0" " " < /proc/$p/cmdline 2>/dev/null | grep -q "forgectl watch"')


def ensure(config: Config) -> None:
    host = config.host
    forge = remote.sh_path(host.path.rstrip("/") + "/apps/forge")
    script = (f"mkdir -p \"$HOME\"/.forgectl\nif {ALIVE}; then echo running $p; exit 0; fi\n"
              f"cd {forge} || exit 1\n"
              f"setsid nohup python3 -m forgectl watch --resume >> {LOG} 2>&1 < /dev/null &\n"
              f"echo $! > {PID}\necho started $!\n")
    result = remote.on(host, script, timeout=30)
    if result.ok:
        running = result.out.split()[:1] == ["running"]
        say(f"Watch: `forgectl watch --resume` {'was already running' if running else 'started'} on {host.name} "
            "(alerts per [notify]; log ~/.forgectl/watch-loop.log).")
    else:
        say(f"Watch: could not start `forgectl watch --resume` on {host.name} ({result.reason()}); run it there by "
            "hand.")
