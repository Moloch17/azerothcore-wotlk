"""`forgectl status --json` and `forgectl cluster --json`: one JSON document of what the cluster is doing now.

The data comes from FILES where there is a file: the newest run directory's `progress.json` (keys in
docs/forge/reference/file-formats.md), `finished.json`, `spec.json`, the last row of `metrics.csv` and of `eval.csv`,
all read over ssh (BatchMode) on the host; the machines' state from the same one-ssh probe `forgectl cluster` uses. The
host's `forge status` text is read only for what no file holds (the last plan's line and which workers the host sees)
and is skipped with `--no-console`. Every field is always present and is null when unknown; the schema is documented
in docs/forge/forgectl.md and changes only by adding fields.
"""
from __future__ import annotations

import csv
import json
import math
import re
import time
from datetime import datetime, timezone

from . import cluster, console as console_module, remote
from .config import Config
from .ui import Failure, say

SCHEMA = 1
ACTIVE_PHASES = ("training", "evaluating")
TRAINING_STALE_S = 300      # a training run rewrites progress.json every 5 to 15 s
EVALUATING_STALE_S = 7200   # an evaluation can run for hours without a write
RUN_NAME = re.compile(r"^[A-Za-z0-9_.-]+$")
TARGET = re.compile(r"^\s*([A-Za-z0-9_]+)\s*(<=|>=|==|<|>|=)\s*([-+0-9.eE]+)\s*$")
OPTIONAL_KEYS = ("wall_steps_per_sec", "update_bound", "rollout_seconds", "update_compute_seconds", "wait_seconds")
EVAL_ROW_KEYS = ("update", "env_steps", "policy", "episodes", "score", "stderr", "margin", "best", "evals_since_best",
                 "seconds")

RUN_READ = r"""
cd {path} 2>/dev/null || {{ echo "##NOPATH"; exit 0; }}
R={runs}
echo "##NOW"; date +%s
d={stage}
if [ -z "$d" ]; then
  d=$(for p in "$R"/*/progress.json; do
        [ -f "$p" ] || continue
        n=$(basename "$(dirname "$p")")
        case "$n" in *.worker-*) continue;; esac
        echo "$(stat -c %Y "$p") $n"
      done | sort -rn | head -n 1 | cut -d' ' -f2-)
fi
echo "##RUN"; echo "$d"
[ -n "$d" ] || exit 0
D="$R/$d"
put() {{ echo "##$1"; cat "$2" 2>/dev/null; echo; }}
put PROGRESS "$D/progress.json"
put FINISHED "$D/finished.json"
put SPEC "$D/spec.json"
echo "##METRICSHEAD"; head -n 1 "$D/metrics.csv" 2>/dev/null
echo "##METRICSLAST"; tail -n 1 "$D/metrics.csv" 2>/dev/null
echo "##EVALHEAD"; head -n 1 "$D/eval.csv" 2>/dev/null
echo "##EVALROWS"; tail -n 40 "$D/eval.csv" 2>/dev/null
echo "##EVALMTIME"; stat -c %Y "$D/eval.csv" 2>/dev/null
echo "##END"
"""


def num(value):
    """A JSON number from a value read from a file (text or number), or None when it is empty or not finite."""
    if value is None or isinstance(value, bool):
        return None if value is None else int(value)
    if isinstance(value, (int, float)):
        return value if math.isfinite(value) else None
    text = str(value).strip()
    if not text:
        return None
    try:
        return int(text)
    except ValueError:
        pass
    try:
        number = float(text)
    except ValueError:
        return None
    return number if math.isfinite(number) else None


def iso(epoch: float | None) -> str | None:
    if epoch is None:
        return None
    return datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sections(out: str) -> dict[str, list[str]]:
    found: dict[str, list[str]] = {}
    current = None
    for line in out.splitlines():
        if line.startswith("##") and line[2:].isalpha() and line[2:].isupper():
            current = line[2:]
            found[current] = []
        elif current is not None:
            found[current].append(line)
    return found


def load_json(lines: list[str]) -> dict | None:
    text = "\n".join(lines).strip()
    if not text:
        return None
    try:
        value = json.loads(text)
    except ValueError:
        return None
    return value if isinstance(value, dict) else None


def csv_row(head: list[str], last: list[str]) -> dict:
    """The last metrics.csv row as a dict (empty when the row is missing or cut short by a write in progress)."""
    if not head or not last or not head[0].strip() or not last[0].strip():
        return {}
    names = next(csv.reader([head[0]]), [])
    values = next(csv.reader([last[0]]), [])
    return dict(zip(names, values)) if len(names) == len(values) else {}


def eval_row(head: list[str], rows: list[str]) -> dict | None:
    """The newest `learner` row of eval.csv (the argmax evaluation of the learner)."""
    if not head or not head[0].strip():
        return None
    names = next(csv.reader([head[0]]), [])
    for line in reversed([r for r in rows if r.strip()]):
        values = next(csv.reader([line]), [])
        if len(values) == len(names) and values != names:
            row = dict(zip(names, values))
            if row.get("policy") == "learner":
                return {key: (row.get(key) if key == "policy" else num(row.get(key))) for key in EVAL_ROW_KEYS}
    return None


def parse_targets(text) -> dict[str, tuple[str, float]]:
    """`found>=0.95;wall_seconds<=2` into {metric: (op, value)}."""
    targets = {}
    for part in str(text or "").split(";"):
        match = TARGET.match(part)
        if match:
            targets[match.group(1)] = (match.group(2), float(match.group(3)))
    return targets


def meets(value, op: str, target: float) -> bool:
    return {"<=": value <= target, ">=": value >= target, "<": value < target, ">": value > target,
            "==": value == target, "=": value == target}[op]


def headline(progress: dict) -> list[dict]:
    """Each headline measure of the stage: its newest evaluation value against its target."""
    targets = parse_targets(progress.get("status_targets"))
    out = []
    for metric in [m for m in str(progress.get("status_headline") or "").split(",") if m]:
        value = num(progress.get(f"eval_{metric}"))
        op, goal = targets.get(metric, (None, None))
        out.append({"metric": metric, "value": value,
                    "target": None if op is None else {"op": op, "value": goal},
                    "met": None if op is None or value is None else meets(value, op, goal)})
    return out


def ladder_doc(progress: dict, metrics: dict) -> dict:
    def pick(key):
        return num(progress[key]) if key in progress else num(metrics.get(key))
    collapsed, stalled = pick("ladder_collapsed"), pick("ladder_stalled")
    alarm_rung = next((v for v in (collapsed, stalled) if v is not None and v >= 0), None)
    return {"rung": num(progress.get("eval_seek_rung")), "episode_rung": num(progress.get("episode_seek_rung")),
            "alarm_rung": alarm_rung, "shaping_scale": pick("shaping_scale"), "cost_scale": pick("cost_scale"),
            "lr_scale": pick("lr_scale"),
            "collapsed": None if collapsed is None else collapsed >= 0,
            "stalled": None if stalled is None else stalled >= 0}


def run_doc(read: dict, now: float | None) -> dict:
    """The `plan`, `progress`, `ladder`, `eval`, `finished` and `spec` parts from one RUN_READ answer."""
    progress = load_json(read.get("PROGRESS", [])) or {}
    metrics = csv_row(read.get("METRICSHEAD", []), read.get("METRICSLAST", []))
    finished = load_json(read.get("FINISHED", []))
    spec = load_json(read.get("SPEC", [])) or {}
    updated = num(progress.get("updated_at"))
    age = None if updated is None or now is None else max(0, int(now - updated))
    phase = progress.get("phase")
    if not progress:
        state = "idle"
    elif phase in ACTIVE_PHASES:
        limit = EVALUATING_STALE_S if phase == "evaluating" else TRAINING_STALE_S
        state = "running" if age is not None and age <= limit else "stale"
    else:
        state = "idle"

    def value(key):
        if key in progress:
            return num(progress[key])
        return num(metrics.get(key))
    total, steps = num(progress.get("total_env_steps")), num(progress.get("env_steps"))
    mtime = num((read.get("EVALMTIME") or [""])[0])
    return {
        "plan": {"state": state, "stage": progress.get("scenario") or (read.get("RUN") or [None])[0] or None,
                 "run_dir": (read.get("RUN") or [""])[0] or None, "phase": phase,
                 "updated_at": iso(updated), "progress_age_s": age},
        "progress": {
            "update": num(progress.get("update")), "env_steps": steps, "total_env_steps": total,
            "fraction": None if not total or steps is None else round(steps / total, 4),
            "resumed_env_steps": num(progress.get("resumed_env_steps")),
            "env_steps_per_sec": value("env_steps_per_sec"), "update_seconds": value("update_seconds"),
            "elapsed_seconds": value("elapsed_seconds"),
            **{key: value(key) for key in OPTIONAL_KEYS}},
        "ladder": ladder_doc(progress, metrics),
        "eval": {"count": num(progress.get("evals")), "last_env_steps": num(progress.get("last_eval_env_steps")),
                 "last_score": num(progress.get("last_eval_score")), "best_score": num(progress.get("best_score")),
                 "best_env_steps": num(progress.get("best_env_steps")),
                 "evals_since_best": num(progress.get("evals_since_best")), "patience": num(progress.get("patience")),
                 "eval_every": num(progress.get("eval_every")), "file_updated_at": iso(mtime),
                 "latest": eval_row(read.get("EVALHEAD", []), read.get("EVALROWS", [])),
                 "headline": headline(progress)},
        "finished": None if finished is None else {
            "reason": finished.get("reason"), "advanced": finished.get("advanced"),
            "env_steps": num(finished.get("env_steps")), "best_score": num(finished.get("best_score"))},
        "spec": {"decision_ticks": num(spec.get("decision_ticks")), "tick_ms": num(spec.get("tick_ms")),
                 "env_groups": num(spec.get("env_groups")), "num_envs": num(spec.get("num_envs"))},
    }


EMPTY_RUN = run_doc({}, None)


def read_run(config: Config, stage: str | None) -> tuple[dict, str]:
    """(the run's parts of the document, a problem text; empty when the host answered)."""
    if stage is not None and not RUN_NAME.match(stage):
        raise Failure(f"{stage!r} is not a run name")
    host = config.host
    script = RUN_READ.format(path=remote.sh_path(host.path), runs=remote.sh_path(config.path_of(host, "runs")),
                             stage=remote.sh_path("") if stage is None else "'" + stage + "'")
    result = remote.on(host, script, timeout=60)
    if result.unreachable or (not result.ok and not result.out):
        return EMPTY_RUN, result.reason()
    read = sections(result.out)
    if "NOPATH" in read:
        return EMPTY_RUN, f"no checkout at {host.path}"
    return run_doc(read, num((read.get("NOW") or [""])[0])), ""


WORKER = re.compile(r"^\s*tcp://([^:\s]+):(\d+):\s*(.*?)\s*(?:\((\d+) s ago\))?\s*$")


def parse_console(config: Config, text: str) -> dict:
    """What the host's `forge status` says that no file holds: the last plan's line and the workers it sees."""
    names = {m.address: m.name for m in config.machines}
    last_plan, workers = None, []
    for line in text.splitlines():
        match = re.match(r"^\s*last plan\s+(.*)$", line)
        if match:
            last_plan = match.group(1).strip()
        found = WORKER.match(line)
        if found:
            pairs = dict(re.findall(r"(\w+)=(\S+)", found.group(3)))
            workers.append({"address": found.group(1), "machine": names.get(found.group(1)),
                            "state": pairs.get("state"), "scenario": None if pairs.get("scenario") in (None, "-")
                            else pairs["scenario"], "envs": num(pairs.get("envs")),
                            "env_steps_per_sec": num(pairs.get("env_steps_per_s")),
                            "last_seen_s": num(found.group(4))})
    return {"available": True, "last_plan": last_plan, "workers": workers}


def read_console(config: Config) -> dict:
    unavailable = {"available": False, "last_plan": None, "workers": None}
    try:
        result = console_module.send(config, config.host, "forge status", timeout=12)
    except (Failure, OSError):
        return unavailable
    # The prompt is sometimes lost among the log lines of a busy console; a reply that was echoed is still a reply.
    return parse_console(config, result.text) if result.started and result.lines else unavailable


def machine_doc(status: cluster.MachineStatus, host_rev: str) -> dict:
    fields = status.fields
    machine = status.machine
    doc = {"name": machine.name, "role": machine.role, "in_cluster": machine.in_cluster,
           "reachable": status.reachable, "problem": status.problem or None, "revision": status.rev or None,
           "revision_matches_host": None, "worldserver": {"up": None, "status": None},
           "learner": {"state": None, "env_steps": None, "steps_per_sec": None, "log_age_s": None},
           "load": {"one_minute": None, "cpus": None}, "disk_free_gb": None, "gpu": None}
    if not status.reachable or status.problem:
        return doc
    doc["revision_matches_host"] = None if not (host_rev and status.rev) else (
        status.rev.startswith(host_rev) or host_rev.startswith(status.rev))
    doc["worldserver"] = {"up": status.worldserver.startswith("Up"), "status": status.worldserver or None}
    state = {"stepping": "stepping", "STALLED": "stalled", "no log": "no_log"}.get(status.learner.split(" ")[0], None)
    if status.learner.startswith("no log"):
        state = "no_log"
    doc["learner"] = {"state": state, "env_steps": status.step,
                      "steps_per_sec": None if status.sps is None else round(status.sps, 1),
                      "log_age_s": num(fields.get("age"))}
    load = re.match(r"(\S+) cpus=(\d+)", fields.get("load", ""))
    if load:
        doc["load"] = {"one_minute": num(load.group(1)), "cpus": num(load.group(2))}
    if fields.get("disk_kb", "").isdigit():
        doc["disk_free_gb"] = round(int(fields["disk_kb"]) / 1048576, 1)
    gpu = re.match(r"(\d+)/(\d+) MiB", fields.get("gpu", ""))
    if gpu:
        doc["gpu"] = {"used_mib": int(gpu.group(1)), "total_mib": int(gpu.group(2))}
    return doc


def local_revision(config: Config) -> str | None:
    result = remote.execute(["git", "-C", str(config.repo_root), "rev-parse", "--short", "HEAD"], timeout=20)
    return result.out.strip() or None if result.ok else None


def header(config: Config, kind: str) -> dict:
    return {"schema": SCHEMA, "kind": kind, "time": iso(time.time()), "source": "files", "host": config.host_name}


def cluster_part(config: Config, statuses: list, refused: list[str]) -> tuple[list[dict], dict]:
    host_rev = next((s.rev for s in statuses if s.machine.name == config.host_name and s.reachable), "")
    machines = [machine_doc(s, host_rev) for s in statuses]
    in_cluster = [m for m in machines if m["in_cluster"]]
    revisions = sorted({m["revision"] for m in in_cluster if m["revision"]})
    return machines, {"host": config.host_name, "dev_revision": local_revision(config),
                      "host_revision": host_rev or None,
                      "revisions_equal": None if not revisions else all(
                          m["revision_matches_host"] for m in in_cluster if m["revision"]),
                      "refused": refused, "refused_window": "6h"}


def probe_all(config: Config, include_out: bool) -> list:
    machines = [m for m in config.machines if include_out or m.in_cluster]
    machines.sort(key=lambda m: (not m.in_cluster, m.name != config.host_name))
    return remote.parallel_map(lambda m: cluster.probe(config, m), machines)


def host_reachable(statuses: list, config: Config) -> bool:
    return any(s.machine.name == config.host_name and s.reachable and not s.problem for s in statuses)


def cluster_json(config: Config, include_out: bool = False) -> int:
    statuses = probe_all(config, include_out)
    ok = host_reachable(statuses, config)
    refused = cluster.refused_lines(config) if ok else []
    machines, parts = cluster_part(config, statuses, refused)
    say(json.dumps({**header(config, "cluster"), "read_ok": ok, "cluster": parts, "machines": machines}, indent=2))
    return 0 if ok else 1


def collect(config: Config, stage: str | None = None, use_console: bool = True, include_out: bool = False) -> dict:
    """The whole `status --json` document (also what `forgectl watch` polls)."""
    jobs = [lambda: probe_all(config, include_out), lambda: read_run(config, stage),
            lambda: cluster.refused_lines(config)]
    if use_console:
        jobs.append(lambda: read_console(config))
    results = remote.parallel_map(lambda job: job(), jobs)
    statuses, (run, run_problem), refused = results[0], results[1], results[2]
    console = results[3] if use_console else {"available": False, "last_plan": None, "workers": None}
    ok = host_reachable(statuses, config) and not run_problem
    machines, parts = cluster_part(config, statuses, refused)
    host_doc = next((m for m in machines if m["name"] == config.host_name), None) or {}
    learner = dict(host_doc.get("learner") or {})
    plan = {**run["plan"], "last_plan": console["last_plan"], "console_available": console["available"]}
    return {**header(config, "status"), "read_ok": ok, "read_problem": run_problem or None, "cluster": parts,
            "plan": plan, "progress": run["progress"], "ladder": run["ladder"], "eval": run["eval"],
            "finished": run["finished"], "spec": run["spec"], "learner": learner,
            "workers_seen_by_host": console["workers"], "machines": machines}


def status_json(config: Config, stage: str | None, use_console: bool) -> int:
    document = collect(config, stage, use_console)
    say(json.dumps(document, indent=2, allow_nan=False))
    return 0 if document["read_ok"] else 1
