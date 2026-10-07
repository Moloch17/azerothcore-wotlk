"""`forgectl build [--cluster]` and `forgectl cluster move-host`: getting code onto the machines and moving the run."""
from __future__ import annotations

import dataclasses
import re
import shlex
import subprocess
import time
from dataclasses import dataclass

from . import audit, confsync, remote, stage as stage_commands
from .config import Config, Machine
from .ui import Declined, Failure, confirm, note, say, strip_ansi, table

clock = time.monotonic   # replaced in tests
sleep = time.sleep

POLL_SECONDS = 20
PULL_TIMEOUT = 900          # cluster-pull.sh itself (a pull and a container recreate; the compile happens after it)
READY_RE = r"AzerothCore rev\. {sha}.*ready"


@dataclass
class Outcome:
    machine: Machine
    state: str = "pending"      # ready | pull failed | timed out | unreachable
    detail: str = ""
    seconds: float = 0.0

    @property
    def ok(self) -> bool:
        return self.state == "ready"


def local_git(config: Config, *args: str) -> str:
    result = remote.execute(["git", "-C", str(config.repo_root), *args], timeout=120)
    if not result.ok:
        raise Failure(f"git {' '.join(args)} failed: {(result.err or result.out).strip()[:300]}")
    return result.out.strip()


def deploy_state(config: Config) -> tuple[str, str]:
    """(branch, full sha) of this checkout; a deploy ships the configured branch only."""
    branch = local_git(config, "rev-parse", "--abbrev-ref", "HEAD")
    return branch, local_git(config, "rev-parse", "HEAD")


def pull_script(config: Config, machine: Machine) -> str:
    return (f"set -e\ncd {remote.sh_path(machine.path)}\nT0=$(date -u +%Y-%m-%dT%H:%M:%SZ)\necho \"T0=$T0\"\n"
            "apps/forge/tools/cluster-pull.sh 2>&1\n")


def local_build_script(config: Config, machine: Machine) -> str:
    return (f"set -e\ncd {shlex.quote(str(config.repo_root))}\necho \"T0=$(date -u +%Y-%m-%dT%H:%M:%SZ)\"\n"
            "mkdir -p env/dist\ntouch env/dist/.forge-build\n"
            "docker compose up -d --force-recreate ac-worldserver 2>&1\n")


def ready_script(config: Config, sha: str, t0: str) -> str:
    return (f"docker logs --since {t0} {config.worldserver} 2>&1 | sed 's/\\x1b\\[[0-9;]*m//g' | "
            f"grep -a -m1 -E {shlex.quote(READY_RE.format(sha=sha[:9]))} | cut -c1-200\n")


def wait_ready(config: Config, machine: Machine, sha: str, t0: str, timeout: float) -> tuple[bool, str]:
    """Poll the machine's docker logs for 'AzerothCore rev. <sha>... ready' after t0 until `timeout` seconds pass."""
    deadline = clock() + timeout
    last = "no answer yet"
    while True:
        result = remote.on(machine, ready_script(config, sha, t0), timeout=60)
        if result.ok and result.out.strip():
            return True, result.out.strip()
        last = result.reason() if not result.ok else "built so far, not ready yet"
        if clock() >= deadline:
            return False, last
        sleep(POLL_SECONDS)


def deploy_one(config: Config, machine: Machine, sha: str, timeout: float, local_only: bool = False) -> Outcome:
    started = clock()
    outcome = Outcome(machine)
    script = local_build_script(config, machine) if local_only else pull_script(config, machine)
    result = remote.on(machine, script, timeout=PULL_TIMEOUT)
    t0 = next((line.split("=", 1)[1] for line in result.out.splitlines() if line.startswith("T0=")), "")
    if not result.ok or not t0:
        outcome.state = "unreachable" if result.unreachable else "pull failed"
        tail = [line for line in strip_ansi(result.out + result.err).splitlines() if line.strip() and
                not line.startswith("T0=")][-3:]
        outcome.detail = f"{result.reason()}: " + " / ".join(tail)
    else:
        ok, detail = wait_ready(config, machine, sha, t0, timeout)
        outcome.state = "ready" if ok else "timed out"
        outcome.detail = detail if ok else f"no 'ready' line for {sha[:9]} within {timeout / 60:.0f} min ({detail})"
    outcome.seconds = clock() - started
    return outcome


def report(outcomes: list[Outcome], sha: str) -> int:
    say(table(["machine", "result", "time", "detail"],
              [[o.machine.name, o.state.upper() if not o.ok else "ready", f"{o.seconds / 60:.1f} min", o.detail[:150]]
               for o in outcomes]))
    bad = [o.machine.name for o in outcomes if not o.ok]
    if bad:
        say(f"NOT READY on {sha[:9]}: {', '.join(bad)}. Nothing was skipped silently; look at "
            "`forgectl logs <machine>` "
            "and run `forgectl build --cluster` again (a machine already on the revision just rebuilds).")
        return 1
    say(f"All {len(outcomes)} machines are ready on {sha[:9]}.")
    return 0


def build(config: Config, cluster: bool, yes: bool, timeout_minutes: float, push: bool = True,
          stop_running: bool = False) -> int:
    branch, sha = deploy_state(config)
    running = ""
    if cluster:
        if push and branch != config.branch:
            raise Failure(f"this checkout is on {branch!r} but the cluster runs {config.branch!r}: check out "
                          f"{config.branch} (or merge into it) before deploying")
        state, running_line = stage_commands.host_plan_state(config)
        if state == "unknown":
            raise Failure(f"cannot tell whether a stage is running on {config.host.name} ({running_line}); a "
                          "rebuild restarts every worldserver and would kill it without a final checkpoint save. "
                          "Look at `forgectl cluster` and `forgectl logs`, then try again")
        if state == "running":
            running = running_line
            if not stop_running:
                raise Failure(f"a stage is running ({running_line!r} on {config.host.name}): a rebuild restarts every "
                              "worldserver and kills it without the final checkpoint save that `stage cancel` makes. "
                              "Run `forgectl stage cancel` first, or pass --stop-running to cancel it as part of "
                              "this build")
        targets = config.cluster
        plan = ([f"push {config.branch} ({sha[:9]}) to the remote {config.lan_remote!r}"] if push else []) + [
            f"on {', '.join(m.name for m in targets)} (in parallel): run apps/forge/tools/cluster-pull.sh (pull, "
            "then recreate the worldserver container, which recompiles from source)",
            "wait until each prints 'AzerothCore rev. <sha> ... ready', for up to "
            f"{timeout_minutes:.0f} min each",
            "every worldserver restarts" + (": no stage is running now" if not running else
                                            ", which is why the running stage is cancelled first"),
            ]
        if running:
            plan.insert(0, f"--stop-running: cancel the running stage ({running}) on "
                           f"{', '.join(m.name for m in targets)} (every learner saves latest.pt first) and wait "
                           f"for 'Plan ended' on {config.host.name}")
            plan.append("afterwards `forgectl stage resume <stage>` continues the cancelled run from its latest.pt")
    else:
        targets = [m for m in config.machines if m.local] or [config.host]
        plan = [f"on {targets[0].name} (this machine): touch env/dist/.forge-build and recreate the worldserver "
                f"container, which recompiles {sha[:9]} from source",
                f"wait for its 'ready' line, up to {timeout_minutes:.0f} min"]
    try:
        confirm(plan, yes)
    except Declined as declined:
        say(f"Nothing was changed ({declined}).")
        return 1
    audit.touch(targets)
    if running:
        audit.note(f"stopped the running stage first: {running}")
        note(f"cancelling the running stage on {', '.join(m.name for m in targets)} before the build ...")
        if stage_commands.run(config, "cancel", [], yes=True) != 0:
            raise Failure("the cancel was not accepted everywhere, so nothing was built (a worldserver that is "
                          "restarted under a running stage loses its final checkpoint save); check `forgectl cluster`")
        wait_plan_ended(config, config.host)
    if cluster and push:
        note(f"pushing {config.branch} to {config.lan_remote} ...")
        local_git(config, "push", config.lan_remote, config.branch)
    note(f"deploying {sha[:9]} to {', '.join(m.name for m in targets)}; slow machines take a long time ...")
    outcomes = remote.parallel_map(lambda m: deploy_one(config, m, sha, timeout_minutes * 60, not cluster), targets)
    return report(outcomes, sha)


# ---- move-host ---------------------------------------------------------------------------------------------------

def run_dir(config: Config, machine: Machine, stage: str) -> str:
    return config.path_of(machine, "runs") + "/" + stage


def copy_run(config: Config, old: Machine, new: Machine, stage: str) -> None:
    """tar the stage's run directory (without camera/ and tb/) from the old host into the same place on the new one."""
    runs_old, runs_new = remote.sh_path(config.path_of(old, "runs")), remote.sh_path(config.path_of(new, "runs"))
    pack = f"cd {runs_old} && tar -cf - --exclude=camera --exclude=tb {shlex.quote(stage)}"
    unpack = f"mkdir -p {runs_new} && tar -xf - -C {runs_new}"

    def argv(machine: Machine, command: str) -> list[str]:
        return ["bash", "-c", command] if machine.local else remote.ssh_argv(machine, [command])

    sender = subprocess.Popen(argv(old, pack), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    receiver = subprocess.Popen(argv(new, unpack), stdin=sender.stdout, stderr=subprocess.PIPE)
    sender.stdout.close()
    try:
        _, recv_err = receiver.communicate(timeout=3600)
        send_err = sender.stderr.read()
        sender.wait(timeout=30)
    except subprocess.TimeoutExpired:
        sender.kill()
        receiver.kill()
        raise Failure("copying the run directory took over an hour; stopped") from None
    if sender.returncode or receiver.returncode:
        raise Failure(f"copying runs/{stage} failed: {(send_err + recv_err).decode(errors='replace').strip()[:300]}")


def set_host_in_toml(text: str, host: str) -> str:
    """cluster.toml with [cluster] host = "<host>" (only the first `host =` line, which is in [cluster])."""
    return re.sub(r'(?m)^(host\s*=\s*)"[^"]*"', lambda m: f'{m.group(1)}"{host}"', text, count=1)


def move_plan(config: Config, new: Machine, stage: str | None) -> list[str]:
    old = config.host
    others = ", ".join(m.name for m in config.cluster if m.name != new.name)
    lines = [
        f"cancel the plan on {old.name} (and the workers) and wait for 'Plan ended: cancelled'",
        (f"copy runs/{stage} (without camera/ and tb/) from {old.name} to {new.name}; an existing runs/{stage} on "
         f"{new.name} is renamed, not overwritten" if stage else
         "copy no run directory (no stage given: only the roles move)"),
        f"set {new.name} to Role = \"host\", Host = \"\" and every other machine ({others}) "
        f"to Role = \"worker\", Host = \"{new.address}:{config.control_port}\" in its mod_animus_forge.conf "
        "(timestamped backups)",
        "pull and rebuild every machine in the cluster (the roles need a worldserver restart), waiting for each",
        f"edit host = \"{new.name}\" in {config.file}; commit and push that file afterwards",
    ]
    if stage:
        lines.append(f"resume {stage} on {new.name} and look for 'worker learners join this run'")
    return lines


def move_host(config: Config, target: str, stage: str | None, yes: bool, timeout_minutes: float) -> int:
    new, old = config.machine(target), config.host
    if new.name == old.name:
        raise Failure(f"{target} is already the host")
    if not new.in_cluster:
        raise Failure(f"{target} is not in the cluster: set in_cluster = true for it in {config.file} first")
    if stage:
        stage_commands.check_names([stage])
    branch, sha = deploy_state(config)
    try:
        confirm(move_plan(config, new, stage), yes, what=f"Move the host from {old.name} to {new.name}")
    except Declined as declined:
        say(f"Nothing was changed ({declined}).")
        return 1
    audit.touch(config.cluster)
    done: list[str] = []
    try:
        say(f"[1/6] cancel on {old.name}")
        status = stage_commands.send_checked(config, old, "forge status")
        if "idle" in (status.lines[0].lower() if status.lines else ""):
            say("  the old host is idle: nothing to cancel")
        else:
            if stage_commands.run(config, "cancel", [], yes=True) != 0:
                raise Failure("cancel was not accepted everywhere")
            wait_plan_ended(config, old)
        done.append("cancelled")

        if stage:
            say(f"[2/6] copy runs/{stage} {old.name} -> {new.name}")
            there = remote.sh_path(run_dir(config, new, stage))
            existing = remote.on(new, f"test -e {there} && echo yes\n", timeout=30)
            if existing.out.strip() == "yes":
                aside = time.strftime("%Y%m%d-%H%M%S")
                remote.on(new, f"mv {remote.sh_path(run_dir(config, new, stage))} "
                               f"{remote.sh_path(run_dir(config, new, stage))}.before-move-{aside}\n", timeout=30)
                say(f"  the existing run on {new.name} was renamed to {stage}.before-move-{aside}")
            copy_run(config, old, new, stage)
            done.append("copied")
        else:
            say("[2/6] no stage given: no run directory copied")

        say("[3/6] roles and host in every machine's conf")
        stamp = time.strftime("%Y%m%d-%H%M%S")
        for machine in config.cluster:
            role, host = ("host", "") if machine.name == new.name else (
                "worker", f"{new.address}:{config.control_port}")
            backup = confsync.rewrite(config, machine, lambda text: confsync.set_cluster_role(text, role, host), stamp)
            say(f"  {machine.name}: Role = {role}; backup {backup}")
        done.append("confs")

        say("[4/6] pull and rebuild every machine")
        if build(config, cluster=True, yes=True, timeout_minutes=timeout_minutes, push=False) != 0:
            raise Failure("not every machine came up ready")
        done.append("rebuilt")
    except Failure as failure:
        say(f"STOPPED: {failure}")
        say(f"Done before it stopped: {', '.join(done) or 'nothing'}. Fix the cause and redo the missing steps by "
            "hand (docs/forge/cluster.md, 'what it does underneath'); nothing was rolled back.")
        return 1

    say("[5/6] cluster.toml")
    text = config.file.read_text()
    config.file.write_text(set_host_in_toml(text, new.name))
    say(f"  {config.file}: host = \"{new.name}\" (commit and push it so every checkout agrees)")
    moved = dataclasses.replace(config, host_name=new.name)
    if not stage:
        say(f"[6/6] no stage given: resume with `forgectl stage resume <stage>` (the host is now {new.name}).")
        return 0
    say(f"[6/6] resume {stage} on {new.name}")
    if stage_commands.run(moved, "resume", [stage], yes=True) != 0:
        say("The resume was not accepted. The move itself is done.")
        return 1
    joined = wait_for_log(moved, new, r"worker learners join", 180)
    say(f"  {joined}" if joined else "  no 'worker learners join this run' line within 3 min: check forgectl cluster")
    return 0 if joined else 1


def wait_for_log(config: Config, machine: Machine, pattern: str, seconds: float, since: str | None = None) -> str:
    """The first docker-log line on the machine matching pattern, polled for `seconds`, '' if none."""
    deadline = clock() + seconds
    started = clock()
    while True:
        age = int(clock() - started) + 15
        script = (f"docker logs --since {since or str(age) + 's'} {config.worldserver} 2>&1 | "
                  f"sed 's/\\x1b\\[[0-9;]*m//g' | grep -a -m1 -E {shlex.quote(pattern)} | cut -c1-200\n")
        result = remote.on(machine, script, timeout=60)
        if result.ok and result.out.strip():
            return result.out.strip()
        if clock() >= deadline:
            return ""
        sleep(5)


def wait_plan_ended(config: Config, machine: Machine) -> None:
    line = wait_for_log(config, machine, r"Plan ended", 180)
    if not line:
        raise Failure(f"{machine.name} did not log 'Plan ended' within 3 minutes of the cancel")
    say(f"  {line}")
