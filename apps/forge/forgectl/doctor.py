"""`forgectl doctor`: the read-only pre-flight. One PASS / WARN / FAIL line per check, a one-line "to do" under every
WARN and FAIL, exit 1 if anything FAILed. It only reads (ssh probes, `cat` of the confs, `docker inspect`, the local
GPU's sysfs); it never changes a machine. Thresholds are in cluster.toml [doctor].

The checks: machines reachable; one revision (the dev checkout's); docker up; worldserver up; its restart policy; free
disk; the dev card idle; the Curriculum keys equal (what `conf-sync --check` says); the camera, map, memory, tick and
half-batch keys equal, with each machine's cadence (decision ticks, HalfBatch, Envs, Cpus) printed (audit finding O1);
no empty conf; the host's `refused the worker` lines; recent errors in each learner log; stale `*.pt.partial`.
"""
from __future__ import annotations

import glob
import time
from dataclasses import dataclass

from . import cluster, confkeys, confsync, remote, snapshot
from .config import Config, Machine
from .ui import Failure, say

PASS, WARN, FAIL, INFO = "PASS", "WARN", "FAIL", "INFO"

EXTRA = r"""
cd {path} 2>/dev/null || exit 0
L={learner_log}
echo "logage=$(( $(date +%s) - $(stat -c %Y "$L" 2>/dev/null || echo 0) ))"
echo "errline=$(tail -n 400 "$L" 2>/dev/null | grep -a -E 'Traceback|Error|CRITICAL|FATAL|Exception' \
  | tail -n 1 | cut -c1-200)"
echo "partials=$(find {runs} -maxdepth 3 -name '*.pt.partial' -mmin +{minutes} 2>/dev/null | head -n 3 | tr '\n' ' ')"
S={scenes}
echo "scenes=$(for f in "$S"/*.scene; do [ -f "$f" ] && printf '%s:%s ' "$(basename "$f" .scene)" \\
  "$(od -An -tx8 -j104 -N8 "$f" | tr -d ' ')"; done)"
"""


@dataclass
class Check:
    name: str
    level: str
    detail: str
    todo: str = ""


@dataclass
class Facts:
    status: cluster.MachineStatus
    extra: dict


def shown(value) -> str:
    return "unset" if value is None else str(value)


def read_extra(config: Config, machine: Machine) -> dict:
    script = EXTRA.format(path=remote.sh_path(machine.path),
                          learner_log=remote.sh_path(config.path_of(machine, "learner_log")),
                          runs=remote.sh_path(config.path_of(machine, "runs")),
                          scenes=remote.sh_path(config.path_of(machine, "scenes")),
                          minutes=int(config.doctor["partial_stale_minutes"]))
    result = remote.on(machine, script, timeout=45)
    fields = {}
    for line in result.out.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            fields[key.strip()] = value.strip()
    return fields


def gather(config: Config, machine: Machine) -> Facts:
    status = cluster.probe(config, machine)
    return Facts(status, read_extra(config, machine) if status.reachable and not status.problem else {})


def read_confs(config: Config, machines: list[Machine]) -> dict:
    """name -> conf text, or the Failure text when it cannot be read."""
    def one(machine):
        try:
            return confsync.read_conf(config, machine)
        except Failure as failure:
            return failure
    return dict(zip([m.name for m in machines], remote.parallel_map(one, machines)))


def scene_checksums(text: str) -> dict[str, str]:
    """map id -> checksum of the scene files a machine reported (`<map>:<checksum>` words)."""
    out = {}
    for word in text.split():
        name, sep, checksum = word.partition(":")
        if sep and name.isdigit():
            out[str(int(name))] = checksum
    return out


def scene_check(config: Config, live: list) -> Check:
    """The baked camera scenes (decision 0020): every machine bakes its own, so each worker's must be the host's byte
    for byte (the cluster fingerprint refuses a worker whose scenes differ)."""
    host = next((f for f in live if f.status.machine.name == config.host_name), None)
    if host is None:
        return Check("camera scenes", WARN, "the host is not reachable, so there is nothing to compare against")
    reference = scene_checksums(host.extra.get("scenes", ""))
    if not reference:
        return Check("camera scenes", WARN, f"{host.status.machine.name} has no scene file yet",
                     "the worldserver bakes them when it starts (look for 'Scene map' in its log); is "
                     f"{config.paths['scenes']} where its AnimusForge.DataDir puts them?")
    bad = []
    for f in live:
        if f is host:
            continue
        mine = scene_checksums(f.extra.get("scenes", ""))
        problems = [f"{m} missing" if m not in mine else f"{m} differs" for m in sorted(reference, key=int)
                    if mine.get(m) != reference[m]]
        if problems:
            bad.append(f"{f.status.machine.name}: {', '.join(problems)}")
    if not bad:
        return Check("camera scenes", PASS, f"maps {', '.join(sorted(reference, key=int))} equal on "
                     f"{len(live)} machines")
    where = config.paths["scenes"]
    return Check("camera scenes", FAIL, "; ".join(bad), "the host's file is the reference: copy "
                 f"{where}/<map>.scene from the host to the same place on that machine, or delete the worker's file "
                 "and restart its worldserver to bake it again; a worker whose scenes differ is refused by the "
                 "cluster fingerprint")


def dev_card(max_busy: float) -> tuple[str, str]:
    """(level, text) for the local GPU: the card with the most VRAM, busy percentage sampled 4 times over 2 s."""
    cards = []
    for path in glob.glob("/sys/class/drm/card[0-9]*/device"):
        try:
            total = int(open(f"{path}/mem_info_vram_total").read())
            cards.append((total, path))
        except (OSError, ValueError):
            continue
    if not cards:
        return WARN, "no GPU with readable sysfs counters on this machine (/sys/class/drm/card*/device)"
    total, path = max(cards)
    busy = []
    for _ in range(4):
        try:
            busy.append(int(open(f"{path}/gpu_busy_percent").read()))
        except (OSError, ValueError):
            return WARN, f"cannot read {path}/gpu_busy_percent"
        time.sleep(0.5)
    used = int(open(f"{path}/mem_info_vram_used").read()) / 1048576
    text = f"busy {max(busy)}% (limit {max_busy:g}%), VRAM {used:.0f}/{total / 1048576:.0f} MiB used"
    return (PASS if max(busy) <= max_busy else WARN), text


def checks(config: Config, facts: list[Facts], dev_rev: str | None, confs: dict, refused: list[str],
           dev: tuple[str, str], dev_disk_gb: float | None) -> list[Check]:
    settings = config.doctor
    out: list[Check] = []
    names = [f.status.machine.name for f in facts]
    live = [f for f in facts if f.status.reachable and not f.status.problem]
    down = [f for f in facts if not (f.status.reachable and not f.status.problem)]

    if down:
        out.append(Check("machines reachable", FAIL, f"{len(live)}/{len(facts)}; no answer from "
                         + ", ".join(f"{f.status.machine.name} ({f.status.problem or 'no checkout'})" for f in down),
                         "power it on or fix the network and try `ssh -o BatchMode=yes user@address true`; if it is "
                         "meant to be out, set in_cluster = false in apps/forge/cluster.toml"))
    else:
        out.append(Check("machines reachable", PASS, f"{len(live)}/{len(facts)} ({', '.join(names)})"))

    host_rev = next((f.status.rev for f in live if f.status.machine.name == config.host_name), "")
    reference = dev_rev or host_rev
    differ = [f"{f.status.machine.name} {f.status.rev or '?'}" for f in live
              if reference and not (f.status.rev.startswith(reference) or reference.startswith(f.status.rev))]
    if not reference:
        out.append(Check("revision", FAIL, "this checkout's revision could not be read", "run it from a git checkout"))
    elif differ:
        out.append(Check("revision", FAIL, f"dev checkout is {reference}; different: {', '.join(differ)}",
                         "a stage must not be running; run `forgectl build --cluster` (it refuses under a running "
                         "stage unless --stop-running)"))
    else:
        out.append(Check("revision", PASS, f"{len(live)} machines on {reference} (the dev checkout's)"))

    docker_down = [f.status.machine.name for f in live if f.status.docker_ok is False]
    if docker_down:
        out.append(Check("docker", FAIL, f"`docker ps` fails on {', '.join(docker_down)}",
                         "start docker on that machine (systemctl start docker); the user must be in the docker group"))
    else:
        out.append(Check("docker", PASS, f"answers on {len(live)} machines"))

    ws_down = [f.status.machine.name for f in live if not f.status.worldserver.startswith("Up")]
    out.append(Check("worldserver up", WARN, f"{config.worldserver} is not running on {', '.join(ws_down)}",
                     "fine if no stage is meant to run; otherwise `docker start` it there or run "
                     "`forgectl build --cluster`")
               if ws_down else Check("worldserver up", PASS, f"running on {len(live)} machines"))

    no_restart = [f"{f.status.machine.name} ({f.status.restart or 'none'})" for f in live
                  if f.status.restart in (None, "", "no")]
    out.append(Check("restart policy", WARN, "will stay down after a reboot: " + ", ".join(no_restart),
                     "on that machine: docker update --restart unless-stopped " + config.worldserver)
               if no_restart else Check("restart policy", PASS, "restarts at boot on " + str(len(live)) + " machines"))

    floor = float(settings["disk_min_gb"])
    disks = {f.status.machine.name: int(f.status.fields["disk_kb"]) / 1048576 for f in live
             if f.status.fields.get("disk_kb", "").isdigit()}
    low = [f"{n} {gb:.0f} GB" for n, gb in disks.items() if gb < floor]
    summary = ", ".join(f"{n} {gb:.0f} GB" for n, gb in disks.items())
    out.append(Check("disk free", FAIL, f"below {floor:g} GB: {', '.join(low)} (all: {summary})",
                     "free space: archive old runs (var/animus-forge/shared/runs/_archive), delete old checkpoint_*.pt")
               if low else Check("disk free", PASS, f"{summary} (need >= {floor:g})"))
    if dev_disk_gb is not None and dev_disk_gb < floor:
        out.append(Check("dev disk free", WARN, f"{dev_disk_gb:.0f} GB on this machine (need >= {floor:g})",
                         "free space before a local build"))

    out.append(Check("dev card idle", dev[0], dev[1],
                     "something is using the dev GPU; check `ps`, the desktop and the dev containers before a "
                     "run that wants it" if dev[0] != PASS else ""))

    host_text = confs.get(config.host_name)
    if not isinstance(host_text, str):
        out.append(Check("conf-sync", FAIL, f"the host's conf cannot be read ({host_text})", "fix ssh to the host"))
    else:
        host_keys = confsync.curriculum_keys(host_text)
        bad, unreadable = [], []
        for worker in config.workers:
            text = confs.get(worker.name)
            if not isinstance(text, str):
                unreadable.append(worker.name)
                continue
            diff = confsync.compare(host_keys, confsync.curriculum_keys(text))
            if not diff.same:
                bad.append(f"{worker.name} ({diff.summary()})")
        if bad or unreadable:
            out.append(Check("curriculum keys", FAIL, "; ".join(
                ([f"differ from the host's {len(host_keys)}: " + ", ".join(bad)] if bad else [])
                + ([f"conf not readable: {', '.join(unreadable)}"] if unreadable else [])),
                "run `forgectl conf-sync --check` for the keys, then `forgectl conf-sync` (a worker reads its conf "
                "at start)"
                if bad else "reach the machine first"))
        else:
            out.append(Check("curriculum keys", PASS,
                             f"{len(host_keys)} keys equal on {len(config.workers) + 1} machines"))

    host_facts = next((f for f in live if f.status.machine.name == config.host_name), None)
    drift = []
    if host_facts:
        for f in live:
            if f is not host_facts:
                drift += [f"{f.status.machine.name}: {line}"
                          for line in confkeys.mismatches(host_facts.status.conf_keys, f.status.conf_keys)]
    if drift:
        more = f"; ... {len(drift) - 6} more" if len(drift) > 6 else ""
        out.append(Check("sim keys", FAIL, "; ".join(drift[:6]) + more,
                         "make the conf equal to the host's (edit by hand; conf-sync does not copy these) and "
                         "restart that worldserver"))
    else:
        out.append(Check("sim keys", PASS, "Vision.*, Map.*, Memory.*, TicksPerDecision (global and per stage) and "
                                           "HalfBatch equal on " + str(len(live)) + " machines"))
    for f in live:
        c = confkeys.cadence(f.status.conf_keys)
        stage = ",".join(f"{k}={v}" for k, v in c["stage_ticks"].items()) or "none"
        out.append(Check(f"cadence {f.status.machine.name}", INFO,
                         f"decision_ticks {shown(c['ticks_per_decision'])}, HalfBatch {shown(c['half_batch'])}, "
                         f"Envs {shown(c['envs'])}, Cpus {shown(c['learner_cpus'])}, stage ticks: {stage}"))
    out.append(scene_check(config, live))
    empty = [f.status.machine.name for f in live if f.status.conf_lines is not None and f.status.conf_lines <= 0]
    out.append(Check("conf not empty", FAIL, f"the conf is empty or unreadable on {', '.join(empty)}",
                     "restore it from its mod_animus_forge.conf.bak-* before ANY restart")
               if empty else Check("conf not empty", PASS, "every conf has lines"))

    if refused:
        out.append(Check("fingerprint refusals", WARN, f"{len(refused)} 'refused the worker' line(s) in the host's "
                         f"last {settings['refused_hours']:g} h, e.g. {refused[-1][:160]}",
                         "if the revision and curriculum checks above pass these are from before the fix; else "
                         "`forgectl build --cluster` / `forgectl conf-sync`"))
    else:
        out.append(Check("fingerprint refusals", PASS, f"none in the host's last {settings['refused_hours']:g} h"))

    hours = float(settings["learner_error_hours"])
    errors = []
    for f in live:
        age, line = f.extra.get("logage", ""), f.extra.get("errline", "")
        if line and age.isdigit() and int(age) < hours * 3600:
            errors.append(f"{f.status.machine.name} ({int(age) // 60} min ago): {line[:120]}")
    out.append(Check("learner log errors", WARN, "; ".join(errors),
                     "`forgectl logs <machine> --errors` shows the traceback")
               if errors else Check("learner log errors", PASS,
                                    f"none in the tail of a log written in the last {hours:g} h"))

    partial = [f"{f.status.machine.name}: {f.extra['partials']}" for f in live if f.extra.get("partials")]
    out.append(Check("stale checkpoint writes", WARN, "; ".join(partial)[:300],
                     "a *.pt.partial older than " + f"{settings['partial_stale_minutes']:g} min is a write that never "
                     "finished; make sure no learner is running there, then delete it (the .pt beside it is the "
                     "good one)")
               if partial else Check("stale checkpoint writes", PASS, "no *.pt.partial left behind"))
    return out


def dev_disk(config: Config) -> float | None:
    result = remote.execute(["df", "-Pk", str(config.repo_root)], timeout=10)
    try:
        return int(result.out.splitlines()[1].split()[3]) / 1048576
    except (IndexError, ValueError):
        return None


def run(config: Config) -> int:
    machines = config.cluster
    say(f"Checking {len(machines)} machines ({', '.join(m.name for m in machines)}); read-only, about 10 seconds ...")
    jobs = [lambda m=m: gather(config, m) for m in machines]
    jobs.append(lambda: read_confs(config, machines))
    jobs.append(lambda: cluster.refused_lines(config, f"{config.doctor['refused_hours']:g}h"))
    results = remote.parallel_map(lambda job: job(), jobs)
    facts, confs, refused = results[:len(machines)], results[len(machines)], results[len(machines) + 1]
    found = checks(config, facts, snapshot.local_revision(config), confs, refused,
                   dev_card(float(config.doctor["dev_gpu_busy_max_percent"])), dev_disk(config))
    width = max(len(c.name) for c in found)
    for c in found:
        say(f"{c.level}  {c.name.ljust(width)}  {c.detail}")
        if c.todo and c.level != PASS:
            say(f"      to do: {c.todo}")
    counts = {level: sum(1 for c in found if c.level == level) for level in (PASS, WARN, FAIL)}
    say(f"{counts[PASS]} pass, {counts[WARN]} warn, {counts[FAIL]} fail." + (
        " Fix the FAIL lines before starting or resuming a stage." if counts[FAIL] else ""))
    return 1 if counts[FAIL] else 0
