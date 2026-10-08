"""`forgectl cluster`: one read-only table of the machines. Each is asked once over ssh (in parallel, with timeouts);
a machine that does not answer shows as unreachable."""
from __future__ import annotations

import re
from dataclasses import dataclass, field

from . import confkeys, remote
from .config import Config, Machine
from .ui import say, strip_ansi, table

UPDATE = re.compile(r"^update (\d+) \| steps (\d+) \| (\d+(?:\.\d+)?) sps")
SAMPLE_SECONDS = 5
STALE_SECONDS = 60  # no log write for this long: the learner is not stepping

PROBE = r"""
cd {path} 2>/dev/null || {{ echo "nopath=1"; exit 0; }}
echo "rev=$(git rev-parse --short HEAD 2>/dev/null)"
echo "ws=$(docker ps --filter 'name=^{container}$' --format '{{{{.Status}}}}' 2>/dev/null | head -1)"
docker ps -q >/dev/null 2>&1; echo "dockerrc=$?"
echo "restart=$(docker inspect -f '{{{{.HostConfig.RestartPolicy.Name}}}}' {container} 2>/dev/null)"
L={learner_log}
pick() {{ tail -c 300000 "$L" 2>/dev/null | grep -a '^update [0-9]* | steps' | tail -1 | cut -c1-120; }}
echo "u1=$(pick)"
sleep {sample}
echo "u2=$(pick)"
echo "age=$(( $(date +%s) - $(stat -c %Y "$L" 2>/dev/null || echo 0) ))"
echo "load=$(cut -d' ' -f1 /proc/loadavg) cpus=$(nproc)"
echo "disk_kb=$(df -Pk . | awk 'NR==2{{print $4}}')"
if command -v rocm-smi >/dev/null 2>&1; then
  echo "gpu=$(rocm-smi --showmeminfo vram 2>/dev/null | awk '/Total Memory \(B\)/{{t=$NF}} \
/Total Used Memory \(B\)/{{u=$NF}} END{{if(t > 0) printf "%d/%d MiB", u/1048576, t/1048576}}')"
elif command -v nvidia-smi >/dev/null 2>&1; then
  echo "gpu=$(nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null \
    | head -1 | awk -F, '$2 > 0 {{printf "%d/%d MiB", $1, $2}}')"
fi
C={conf}
if [ -r "$C" ]; then echo "conflines=$(wc -l < "$C")"; else echo "conflines=-1"; fi
echo "##CONF"
{conf_grep} "$C" 2>/dev/null
"""

REFUSED = r"""
cd {path} 2>/dev/null || exit 0
{{ docker logs --since {since} {container} 2>&1; tail -c 2000000 {server_log} 2>/dev/null; }} \
  | grep -a 'refused the worker' | sed 's/\x1b\[[0-9;]*m//g' | cut -c1-300 | sort -u | tail -5
"""


@dataclass
class MachineStatus:
    machine: Machine
    reachable: bool = True
    problem: str = ""
    rev: str = ""
    worldserver: str = ""          # docker's status text ("Up 2 hours"), empty when the container is not running
    learner: str = "-"             # "stepping", "stalled", "no log"
    step: int | None = None
    sps: float | None = None
    load: str = "-"
    disk: str = "-"
    gpu: str = "-"
    fields: dict = field(default_factory=dict)
    restart: str | None = None     # the worldserver container's docker restart policy ("unless-stopped", "no", ...)
    docker_ok: bool | None = None  # `docker ps` works
    conf_lines: int | None = None  # line count of the conf (0: empty, -1: unreadable)
    conf_keys: dict = field(default_factory=dict)   # confkeys.scan of the conf


def parse_update(line: str):
    match = UPDATE.match(line.strip())
    if not match:
        return None
    return int(match.group(1)), int(match.group(2)), float(match.group(3))


def parse_probe(machine: Machine, out: str) -> MachineStatus:
    fields = {}
    out, _, conf_text = out.partition("##CONF")
    for line in out.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            fields[key.strip()] = value.strip()
    status = MachineStatus(machine, fields=fields, conf_keys=confkeys.scan(conf_text))
    if "nopath" in fields:
        status.problem = f"no checkout at {machine.path}"
        return status
    status.rev = fields.get("rev", "")
    status.restart = fields.get("restart") or None
    status.docker_ok = (fields["dockerrc"] == "0") if "dockerrc" in fields else None
    lines = fields.get("conflines", "")
    status.conf_lines = int(lines) if lines.lstrip("-").isdigit() else None
    status.worldserver = fields.get("ws", "")
    first, second = parse_update(fields.get("u1", "")), parse_update(fields.get("u2", ""))
    if second is None:
        status.learner = "no log"
    else:
        status.step, status.sps = second[1], second[2]
        advanced = first is not None and second[1] > first[1]
        age = fields.get("age", "")
        fresh = age.isdigit() and int(age) < STALE_SECONDS  # updates are 5-15 s apart: the sample can miss one
        status.learner = "stepping" if advanced or fresh else "STALLED"
        if age.isdigit():
            status.learner += f" (log {age}s old)"
    load = re.match(r"(\S+) cpus=(\d+)", fields.get("load", ""))
    if load:
        status.load = f"{load.group(1)}/{load.group(2)}"
    if fields.get("disk_kb", "").isdigit():
        status.disk = f"{int(fields['disk_kb']) / 1048576:.0f} GB"
    status.gpu = fields.get("gpu") or "-"
    return status


def probe(config: Config, machine: Machine) -> MachineStatus:
    script = PROBE.format(path=remote.sh_path(machine.path), container=config.worldserver, sample=SAMPLE_SECONDS,
                          learner_log=remote.sh_path(config.path_of(machine, "learner_log")),
                          conf=remote.sh_path(config.path_of(machine, "conf")), conf_grep=confkeys.GREP)
    result = remote.on(machine, script, timeout=SAMPLE_SECONDS + 20)
    if result.unreachable or (not result.ok and not result.out):
        return MachineStatus(machine, reachable=False, problem=result.reason())
    return parse_probe(machine, result.out)


def refused_lines(config: Config, since: str = "6h") -> list[str]:
    host = config.host
    script = REFUSED.format(path=remote.sh_path(host.path), container=config.worldserver, since=since,
                            server_log=remote.sh_path(config.path_of(host, "server_log")))
    result = remote.on(host, script, timeout=45)
    return [strip_ansi(line).strip() for line in result.out.splitlines() if line.strip()] if result.ok else []


def cadence_drift(config: Config, statuses: list[MachineStatus]) -> list[str]:
    """Machines whose must-match conf keys (ticks, half-batch, camera, map, memory) differ from the host's."""
    host = next((s for s in statuses if s.machine.name == config.host_name and s.reachable and not s.problem), None)
    if host is None:
        return []
    out = []
    for s in statuses:
        if s is not host and s.reachable and not s.problem:
            out += [f"{s.machine.name}: {line}" for line in confkeys.mismatches(host.conf_keys, s.conf_keys)[:3]]
    return out


def stage_ticks_line(statuses: list[MachineStatus]) -> str:
    parts = []
    for s in statuses:
        ticks = confkeys.cadence(s.conf_keys)["stage_ticks"] if s.reachable and not s.problem else {}
        if ticks:
            parts.append(f"{s.machine.name}: " + ", ".join(f"{k}={v}" for k, v in ticks.items()))
    return "; ".join(parts)


def render(config: Config, statuses: list[MachineStatus]) -> str:
    host_rev = next((s.rev for s in statuses if s.machine.name == config.host_name and s.reachable), "")
    rows = []
    for s in statuses:
        role = s.machine.role if s.machine.in_cluster else f"{s.machine.role} (out)"
        if not s.reachable or s.problem:
            rows.append([s.machine.name, role, "-", "-", f"UNREACHABLE: {s.problem}" if not s.reachable else s.problem,
                         "-", "-", "-", "-", "-", "-", "-"])
            continue
        rev = s.rev + ("*" if host_rev and s.rev != host_rev else "")
        ws = "up" if s.worldserver.startswith("Up") else "DOWN"
        learner = s.learner if s.step is None else f"{s.learner}, step {s.step:,}, {s.sps:,.0f}/s"
        rows.append([s.machine.name, role, rev, ws, learner, *confkeys.short(s.conf_keys), s.load, s.disk, s.gpu])
    headers = ["machine", "role", "rev", "worldserver", "learner", "ticks", "half", "envs", "cpus", "load", "disk free",
               "gpu mem"]
    text = table(headers, rows)
    if any(s.reachable and host_rev and s.rev != host_rev for s in statuses):
        text += "\n* the revision differs from the host's: that machine is not on the host's build"
    text += ("\nticks, half, envs, cpus: AnimusForge.TicksPerDecision, HalfBatch, Envs and Learner.Cpus from each "
             "machine's conf ('-' = not set, the build's default applies)")
    per_stage = stage_ticks_line(statuses)
    if per_stage:
        text += "\nper-stage ticks per decision: " + per_stage
    drift = cadence_drift(config, statuses)
    if drift:
        text += ("\nCADENCE DIFFERS from the host's (ticks, half-batch, camera, map, memory keys):\n  "
                 + "\n  ".join(drift))
    return text


def run(config: Config, include_out: bool = False) -> int:
    machines = [m for m in config.machines if include_out or m.in_cluster]
    machines.sort(key=lambda m: (not m.in_cluster, m.name != config.host_name))
    say(f"Asking {len(machines)} machines ({', '.join(m.name for m in machines)}); a learner is sampled for "
        f"{SAMPLE_SECONDS} s to see whether it advances.")
    statuses = remote.parallel_map(lambda m: probe(config, m), machines)
    say(render(config, statuses))
    refused = refused_lines(config) if any(s.machine.name == config.host_name and s.reachable for s in statuses) else []
    say()
    if refused:
        say(f"The host refused workers (last 6 h; the fingerprint differs: code, probe data, protocol or "
            f"AnimusForge.Curriculum.* keys; see forgectl conf-sync --check):")
        for line in refused:
            say(f"  {line}")
    else:
        say("No 'refused the worker' lines on the host in the last 6 h.")
    bad = [s for s in statuses if s.machine.in_cluster and (not s.reachable or s.problem)]
    return 1 if bad else 0
