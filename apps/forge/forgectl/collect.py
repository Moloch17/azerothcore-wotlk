"""`forgectl collect <stage> | --follow`: bring every worker's share of a run home to the host's run folder.

`forgectl stage start|resume` starts `collect --follow --every 15` on the host (ensure_loop: one loop, by its pid file),
which collects whatever the host is training; `forgectl stage cancel` stops it and makes a last pass (stop_loop).

What a worker holds of a run (a follower learner rank writes nothing into the run; rank 0 on the host writes the
checkpoints, metrics and evaluations) lands under the host's runs/<stage>/workers/<machine>/:

  run/            the worker's runs/<stage>/ (camera audit images, videos, anything else), never checkpoints (*.pt)
  logs/           the learner log, Server.log and Errors.log, appended from where the last pass stopped; the learner
                  log starts at its first line naming the stage, so months of older stages are not copied
  collect.json    the log offsets and what the last pass did

Each pass copies only what is new: files missing here or of another size, and log bytes past the remembered offset
(a log that was truncated or replaced starts again, after a marker line). Run it on the host, which holds the run;
--every keeps it going. Key-based ssh only (BatchMode); an unreachable worker is reported and skipped.
"""
from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
import time
import zlib
from pathlib import Path

from . import remote
from .config import Config, Machine
from .ui import Declined, Failure, confirm, say

LOGS = ("learner_log", "server_log", "errors_log")
SKIP = ("*.pt", "*.pt.*", "*.partial")
STATE = "collect.json"


def expand(path: str) -> Path:
    return Path(os.path.expanduser(path))


def host_run(config: Config, stage: str) -> Path:
    host = config.host
    if not host.local:
        raise Failure(f"run collect on the host, {host.name} ({host.target}): it writes into the run folder there")
    run = expand(config.path_of(host, "runs")) / stage
    if not run.is_dir():
        raise Failure(f"the host has no run folder {run}")
    return run


def machines(config: Config, include_out: bool) -> list[Machine]:
    if include_out:
        return [m for m in config.machines if m.role == "worker" and m.name != config.host_name]
    return list(config.workers)


# ---- run folder files ----------------------------------------------------------------------------------------------

def remote_listing(config: Config, machine: Machine, stage: str) -> dict[str, int] | None:
    """The worker's run-folder files (relative path -> size), checkpoints left out; None when it has no such folder."""
    skip = " ".join(f"! -name {shlex.quote(p)}" for p in SKIP)
    folder = remote.sh_path(config.path_of(machine, "runs") + "/" + stage)
    script = f"cd {folder} 2>/dev/null || {{ echo '##NORUN'; exit 0; }}\nfind . -type f {skip} -printf '%s\\t%P\\n'\n"
    result = remote.on(machine, script, timeout=120)
    if not result.ok:
        raise Failure(result.reason())
    if result.out.startswith("##NORUN"):
        return None
    files = {}
    for line in result.out.splitlines():
        size, _, name = line.partition("\t")
        if name and size.isdigit():
            files[name] = int(size)
    return files


def wanted(files: dict[str, int], local: Path) -> list[str]:
    out = []
    for name, size in sorted(files.items()):
        here = local / name
        if not here.is_file() or here.stat().st_size != size:
            out.append(name)
    return out


def fetch_files(config: Config, machine: Machine, stage: str, names: list[str], local: Path) -> None:
    """tar over ssh (a worker need not have rsync), only the names asked for."""
    local.mkdir(parents=True, exist_ok=True)
    folder = remote.sh_path(config.path_of(machine, "runs") + "/" + stage)
    sender = subprocess.Popen(remote.ssh_argv(machine, [f"cd {folder} && tar -cf - --null -T -"], connect=10),
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    receiver = subprocess.Popen(["tar", "-xf", "-", "-C", str(local)], stdin=sender.stdout)
    sender.stdout.close()
    sender.stdin.write(b"".join(n.encode() + b"\0" for n in names))
    sender.stdin.close()
    if receiver.wait(timeout=3600) != 0 or sender.wait(timeout=60) != 0:
        raise Failure(f"tar from {machine.name} failed (ssh {sender.returncode}, tar {receiver.returncode})")


# ---- logs ----------------------------------------------------------------------------------------------------------

def log_stat(machine: Machine, path: str, stage: str, marker: bool) -> dict | None:
    """inode and size, and with `marker` the byte offset of the first line naming the stage (-1 when none)."""
    p = remote.sh_path(path)
    find = f" $(grep -a -b -m1 -F {shlex.quote(stage)} {p} | cut -d: -f1)" if marker else ""
    result = remote.on(machine, f"[ -f {p} ] || {{ echo none; exit 0; }}\necho $(stat -c '%i %s' {p}){find}\n",
                       timeout=300)
    if not result.ok:
        raise Failure(result.reason())
    words = result.out.split()
    if not words or words[0] == "none":
        return None
    return {"ino": int(words[0]), "size": int(words[1]), "marker": int(words[2]) if len(words) > 2 else -1}


def fetch_bytes(machine: Machine, path: str, start: int, count: int, out: Path) -> None:
    """Append bytes [start, start+count) of the worker's file to `out`, gzip on the wire."""
    command = f"tail -c +{start + 1} {remote.sh_path(path)} | head -c {count} | gzip -1"
    sender = subprocess.Popen(remote.ssh_argv(machine, [command], connect=10), stdout=subprocess.PIPE)
    inflate = zlib.decompressobj(wbits=31)
    with open(out, "ab") as sink:
        for chunk in iter(lambda: sender.stdout.read(1 << 20), b""):
            sink.write(inflate.decompress(chunk))
        sink.write(inflate.flush())
    if sender.wait(timeout=60) != 0:
        raise Failure(f"reading {path} on {machine.name} failed (ssh {sender.returncode})")


def continues(seen: dict | None, now: dict) -> bool:
    """The log is the one the last pass read, grown or not (not truncated, not replaced)."""
    return bool(seen) and seen.get("ino") == now["ino"] and now["size"] >= seen.get("offset", 0)


def log_plan(key: str, seen: dict | None, now: dict, stage: str) -> tuple[int, str | None]:
    """Where this pass starts reading a log, and a marker line to write first when it is a new file."""
    if continues(seen, now):
        return seen["offset"], None
    if key == "learner_log":
        start = now["marker"] if now["marker"] >= 0 else now["size"]   # nothing of this stage yet: only what follows
    else:
        start = 0
    why = "first collection" if not seen else "the log was truncated or replaced"
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    return start, f"==== forgectl collect {stamp}: {why} (inode {now['ino']}), from byte {start} ({stage}) ====\n"


# ---- one pass ------------------------------------------------------------------------------------------------------

def human(size: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return ""


def collect_one(config: Config, machine: Machine, stage: str, run: Path, check: bool) -> bool:
    home = run / "workers" / machine.name
    state_file = home / STATE
    try:
        state = json.loads(state_file.read_text())
    except (OSError, ValueError):
        state = {}
    try:
        files = remote_listing(config, machine, stage)
        names = wanted(files, home / "run") if files is not None else []
        moved = sum(files[n] for n in names) if names else 0
        logs = {}
        for key in LOGS:
            path = config.path_of(machine, key)
            seen = state.get("logs", {}).get(key)
            now = log_stat(machine, path, stage, marker=False)
            if now is None:
                continue
            if key == "learner_log" and not continues(seen, now):
                now = log_stat(machine, path, stage, marker=True)
            start, marker = log_plan(key, seen, now, stage)
            logs[key] = (path, now, start, marker)
    except Failure as problem:
        say(f"  {machine.name}: skipped ({problem})")
        return False

    log_bytes = sum(now["size"] - start for _, now, start, _ in logs.values())
    folder = "no run folder" if files is None else f"{len(names)} of {len(files)} file(s), {human(moved)}"
    say(f"  {machine.name}: {folder}; logs {human(log_bytes)} new")
    if check:
        return True
    try:
        if names:
            fetch_files(config, machine, stage, names, home / "run")
        (home / "logs").mkdir(parents=True, exist_ok=True)
        done = state.setdefault("logs", {})
        for key, (path, now, start, marker) in logs.items():
            out = home / "logs" / Path(path).name
            if marker:
                with open(out, "a") as sink:
                    sink.write(marker)
            if now["size"] > start:
                fetch_bytes(machine, path, start, now["size"] - start, out)
            done[key] = {"ino": now["ino"], "offset": now["size"], "path": path}
    except (Failure, OSError, subprocess.TimeoutExpired) as problem:
        say(f"  {machine.name}: stopped part way ({problem}); the next pass picks up from the saved offsets")
        state_file.parent.mkdir(parents=True, exist_ok=True)
        state_file.write_text(json.dumps(state, indent=1) + "\n")
        return False
    state.update({"machine": machine.name, "target": machine.target, "last": time.strftime("%Y-%m-%d %H:%M:%S"),
                  "last_files": len(names), "last_bytes": moved + log_bytes})
    state_file.write_text(json.dumps(state, indent=1) + "\n")
    return True


STAGE_NAME = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]*$")
# --follow takes plain stage names only: the folders the workers write by the same name. A/B overlays and archives
# (move2_seek.ab-control-..., _archive) are the host's alone.
PLAIN_STAGE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_]*$")
FOLLOW_MINUTES = 60      # --follow: a run whose progress.json was written this recently is being trained


def active_stages(config: Config) -> list[str]:
    """The host's runs that are training, or were within FOLLOW_MINUTES (a plan that moved on to its next stage still
    gets the tail of the last one): their progress.json is that fresh. Archive folders (_archive, _finetune) never."""
    runs = expand(config.path_of(config.host, "runs"))
    fresh = []
    for progress in runs.glob("*/progress.json"):
        name = progress.parent.name
        if PLAIN_STAGE.match(name) and time.time() - progress.stat().st_mtime < FOLLOW_MINUTES * 60:
            fresh.append(name)
    return sorted(fresh)


def one_pass(config: Config, stages: list[str], targets: list[Machine], check: bool) -> bool:
    ok = True
    for stage in stages:
        try:
            folder = host_run(config, stage)
        except Failure as problem:
            say(f"  {stage}: {problem}")
            ok = False
            continue
        say(f"{'Checking' if check else 'Collecting'} {stage} from {len(targets)} worker(s) "
            f"({time.strftime('%H:%M:%S')}) ...")
        ok = all([collect_one(config, m, stage, folder, check) for m in targets]) and ok
    return ok


def run(config: Config, stage: str | None, check: bool, every: float | None, include_out: bool, yes: bool,
        follow: bool = False) -> int:
    if follow == bool(stage):
        raise Failure("name one stage, or --follow for the ones the host is training")
    if stage and not STAGE_NAME.match(stage):
        raise Failure(f"{stage!r} is not a stage name")
    if stage:
        host_run(config, stage)
    elif not config.host.local:
        raise Failure(f"run collect on the host, {config.host.name} ({config.host.target})")
    targets = machines(config, include_out)
    if not targets:
        raise Failure("no workers to collect from (cluster.toml: in_cluster = true, or --all)")
    what = stage or f"every run the host trains (progress.json newer than {FOLLOW_MINUTES} min)"
    if not check:
        try:
            confirm([f"copy {', '.join(m.name for m in targets)}'s share of {what} (run folder without checkpoints, "
                     f"logs) into runs/<stage>/workers/<machine>/ on {config.host.name}"
                     + (f", every {every:g} min" if every else "")], yes)
        except Declined as declined:
            say(f"Nothing was copied ({declined}).")
            return 1
    while True:
        stages = [stage] if stage else active_stages(config)
        if not stages:
            say(f"Nothing is training on {config.host.name} ({time.strftime('%H:%M:%S')}): nothing to collect.")
        try:
            ok = one_pass(config, stages, targets, check)
        except Exception as problem:   # a loop outlives one bad pass
            if not every or check:
                raise
            say(f"The pass failed ({type(problem).__name__}: {problem}); trying again next time.")
            ok = False
        if not every or check:
            return 0 if ok else 1
        time.sleep(every * 60)


# ---- the loop that `forgectl stage` starts and stops on the host ----------------------------------------------------

LOOP_MINUTES = 15
LOOP_PID = '"$HOME"/.forgectl/collect-loop.pid'
LOOP_LOG = '"$HOME"/.forgectl/collect-loop.log'
LOOP_ALIVE = (f'p=$(cat {LOOP_PID} 2>/dev/null) && tr "\\0" " " < /proc/$p/cmdline 2>/dev/null '
              '| grep -q "forgectl collect"')


def ensure_loop(config: Config) -> None:
    """Make sure `forgectl collect --follow --every 15` runs on the host (one at a time, by its pid file). It follows
    whatever the host trains, so a plan moving to its next stage needs nothing."""
    host = config.host
    forge = remote.sh_path(host.path.rstrip("/") + "/apps/forge")
    script = (f"mkdir -p \"$HOME\"/.forgectl\nif {LOOP_ALIVE}; then echo running $p; exit 0; fi\n"
              f"cd {forge} || exit 1\n"
              f"setsid nohup python3 -m forgectl collect --follow --every {LOOP_MINUTES} --yes >> {LOOP_LOG} 2>&1 "
              f"< /dev/null &\necho $! > {LOOP_PID}\necho started $!\n")
    result = remote.on(host, script, timeout=30)
    if result.ok:
        state = result.out.split()
        say(f"Worker data: the collect loop {'was already running' if state[:1] == ['running'] else 'started'} on "
            f"{host.name} (every {LOOP_MINUTES} min into runs/<stage>/workers/; log ~/.forgectl/collect-loop.log).")
    else:
        say(f"Worker data: could not start the collect loop on {host.name} ({result.reason()}); run "
            f"`forgectl collect --follow --every {LOOP_MINUTES}` there.")


def stop_loop(config: Config) -> None:
    """Stop the host's collect loop, then one last pass so the run's end is home too."""
    host = config.host
    forge = remote.sh_path(host.path.rstrip("/") + "/apps/forge")
    script = (f"if {LOOP_ALIVE}; then kill $p; echo stopped; fi\nrm -f {LOOP_PID}\n"
              f"cd {forge} && python3 -m forgectl collect --follow --yes >> {LOOP_LOG} 2>&1\n")
    say(f"Worker data: stopping the collect loop on {host.name} and collecting a last time ...")
    result = remote.on(host, script, timeout=1800)
    say("Worker data: " + ("the last pass is done." if result.ok else
                           f"the last pass did not finish ({result.reason()}); see ~/.forgectl/collect-loop.log."))
