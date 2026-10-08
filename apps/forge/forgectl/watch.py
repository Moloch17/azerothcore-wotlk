"""`forgectl watch`: poll the cluster the way `forgectl status --json` reads it and tell the operator what changed.

READ-ONLY toward the cluster (the same ssh reads as `status --json`, without the console); it writes only to its sinks
(see notify.py) and to its small state file, ~/.forgectl/watch-state.json. With no sink configured it prints.

`diff(previous, current, limits, state)` is the whole decision of what to say, from two status documents:
  transitions   a stage finished (converged or reached its ceiling), the plan moved to the next stage, a ladder rung or
                the fade scale stepped, an evaluation finished (with its headline numbers), the plan ended or the sim
                went idle without a decision;
  conditions    (told when they appear, and again as "cleared" when they go) ladder_collapsed, ladder_stalled,
                learner_silent, plan_stale, worker_down, disk_low, fingerprint_refused, host_unreadable.
A worker_down or host_unreadable must hold for `confirm_polls` polls in a row before it is told (a ssh flap is not a
drop). The same event (kind and subject) is not sent twice inside `debounce_minutes`.
"""
from __future__ import annotations

import fcntl
import json
import os
import time
from pathlib import Path

from . import notify, snapshot
from .config import Config
from .home import forgectl_home
from .notify import Event
from .ui import Failure, say

STATE_NAME = "watch-state.json"
LOCK_NAME = "watch.lock"
CONFIRMED = ("worker_down", "host_unreadable")   # conditions that must hold for several polls in a row
MAX_HEADLINE = 8


def mega(steps) -> str:
    return "?" if steps is None else f"{steps / 1e6:.1f}M"


def number(value, digits: int = 3) -> str:
    return "?" if value is None else f"{value:.{digits}g}"


def machine_state(machine: dict, doc: dict) -> str:
    """up | down (reachable, worldserver not running) | unreachable | stalled (the plan trains and its learner does not
    step)."""
    if not machine["reachable"] or machine.get("problem"):
        return "unreachable"
    if machine["worldserver"]["up"] is False:
        return "down"
    plan = doc.get("plan") or {}
    if plan.get("state") == "running" and plan.get("phase") == "training" and machine["learner"]["state"] == "stalled":
        return "stalled"
    return "up"


def conditions(doc: dict, limits: dict) -> dict[tuple, Event]:
    """What is wrong right now, as events keyed by (kind, subject)."""
    found: dict[tuple, Event] = {}

    def add(kind, severity, subject, text):
        found[(kind, subject)] = Event(kind, severity, subject, text)
    plan, ladder = doc.get("plan") or {}, doc.get("ladder") or {}
    host = doc.get("host", "the host")
    if not doc.get("read_ok"):
        add("host_unreadable", "critical", host, f"cannot read the host {host}: "
            f"{doc.get('read_problem') or 'it does not answer'}; the watcher is blind")
    else:
        stage = plan.get("stage") or "?"
        if plan.get("state") == "running":
            if ladder.get("collapsed"):
                add("ladder_collapsed", "warn", stage, f"{stage}: the ladder collapsed at rung "
                    f"{number(ladder.get('alarm_rung'))} (shaping scale {number(ladder.get('shaping_scale'))})")
            if ladder.get("stalled"):
                add("ladder_stalled", "warn", stage, f"{stage}: the ladder stalled at rung "
                    f"{number(ladder.get('alarm_rung'))} (shaping scale {number(ladder.get('shaping_scale'))})")
            age = (doc.get("learner") or {}).get("log_age_s")
            minutes = limits["learner_silent_minutes"]
            if plan.get("phase") == "training" and age is not None and age > minutes * 60:
                add("learner_silent", "critical", host, f"{stage}: the learner on {host} has not stepped for "
                    f"{age // 60:.0f} min (limit {minutes:g}); is it dead or hung?")
        elif plan.get("state") == "stale":
            add("plan_stale", "critical", stage, f"{stage}: progress.json says {plan.get('phase')} but was last "
                f"written {(plan.get('progress_age_s') or 0) // 60:.0f} min ago; the learner is not running")
        for line in (doc.get("cluster") or {}).get("refused") or []:
            add("fingerprint_refused", "warn", line[-60:], f"the host refused a worker: {line[:200]}")
    for machine in doc.get("machines") or []:
        if not machine["in_cluster"]:
            continue
        name = machine["name"]
        state = machine_state(machine, doc)
        if state != "up" and name != doc.get("host"):
            why = machine.get("problem") or {"down": "its worldserver container is not running",
                                              "stalled": "its learner stopped stepping"}.get(state, state)
            add("worker_down", "warn", name, f"worker {name} dropped: {why}")
        disk = machine.get("disk_free_gb")
        if disk is not None and disk < limits["disk_min_gb"]:
            add("disk_low", "warn", name, f"{name}: {disk:.0f} GB free, below {limits['disk_min_gb']:g} GB")
    return found


def headline_text(doc: dict) -> str:
    parts = []
    for item in ((doc.get("eval") or {}).get("headline") or []):
        if item["value"] is None or item["target"] is None:
            continue
        mark = "met" if item["met"] else "not met"
        goal = f"{item['target']['op']}{item['target']['value']:g}"
        parts.append(f"{item['metric']} {number(item['value'])} ({goal} {mark})")
    shown = parts[:MAX_HEADLINE]
    return ", ".join(shown) + (f", and {len(parts) - len(shown)} more" if len(parts) > len(shown) else "")


def transitions(old: dict | None, new: dict) -> list[Event]:
    """What changed between two polls. Nothing on the first poll (there is no before)."""
    if old is None or not old.get("read_ok") or not new.get("read_ok"):
        return []
    out: list[Event] = []
    op, np_ = old.get("plan") or {}, new.get("plan") or {}
    stage = np_.get("stage") or "?"
    finished = new.get("finished")
    moved = op.get("stage") != np_.get("stage")
    # The decided stage: the current run if its finished.json just appeared, else (the plan moved on between two polls)
    # the previous run's finished.json, which the loop read for us.
    done_stage, done = (stage, finished) if finished and not moved and old.get("finished") != finished else (
        (op.get("stage"), (new.get("previous_stage") or {}).get("finished")) if moved else (None, None))
    if done:
        verdict = ("converged and the plan advances" if done.get("advanced") else "converged without advancing"
                   if done.get("reason") == "converged" else "reached its step ceiling without converging")
        out.append(Event("stage_finished", "info", done_stage, f"{done_stage} {verdict} at "
                         f"{mega(done.get('env_steps'))} steps ({done.get('reason')}), best score "
                         f"{number(done.get('best_score'))}"))
    if moved and np_.get("state") == "running" and op.get("stage"):
        out.append(Event("stage_advanced", "info", f"{op.get('stage')}>{stage}", f"the plan moved from "
                         f"{op.get('stage')} to {stage}"))
    same_stage = not moved
    ol, nl = old.get("ladder") or {}, new.get("ladder") or {}
    if same_stage and nl.get("rung") is not None and ol.get("rung") is not None and (
            ol["rung"] != nl["rung"] or ol.get("shaping_scale") != nl.get("shaping_scale")):
        out.append(Event("rung_stepped", "info", f"{stage}:{nl['rung']}:{nl.get('shaping_scale')}",
                         f"{stage}: ladder rung {number(ol['rung'])} -> {number(nl['rung'])}, shaping scale "
                         f"{number(ol.get('shaping_scale'))} -> {number(nl.get('shaping_scale'))}"))
    oe, ne = old.get("eval") or {}, new.get("eval") or {}
    if same_stage and (ne.get("count") or 0) > (oe.get("count") or 0):
        latest = ne.get("latest") or {}
        out.append(Event("eval_finished", "info", f"{stage}:{ne['count']}",
                         f"{stage}: evaluation {ne['count']} finished at {mega(ne.get('last_env_steps'))} steps, score "
                         f"{number(ne.get('last_score'))} +/- {number(latest.get('stderr'), 2)} (best "
                         f"{number(ne.get('best_score'))}); " + (headline_text(new) or "no headline target values")))
    if op.get("state") == "running" and np_.get("state") == "idle":
        if finished and (old.get("finished") != finished or moved):
            out.append(Event("plan_ended", "info", stage, f"the plan ended after {stage}: nothing is running now"))
        elif not finished:
            out.append(Event("plan_ended", "warn", stage, f"{stage} stopped without a decision (phase "
                             f"{np_.get('phase')}, last plan: {np_.get('last_plan') or 'unknown'}); the sim is idle"))
    return out


def diff(old: dict | None, new: dict, limits: dict, state: dict | None = None) -> list[Event]:
    """The events to tell, from the previous status document (or None) and the current one.

    `state` (the watcher's persisted `active` and `streak` dicts) makes a condition wait for `confirm_polls` polls and
    be told once until it clears. Without it, a condition is told when it is in `new` and was not in `old`."""
    events = transitions(old, new)
    now = conditions(new, limits)
    keys = {"|".join(k): k for k in now}
    if state is None:
        before = conditions(old, limits) if old else {}
        events += [e for k, e in now.items() if k not in before]
        events += [Event(f"{k[0]}_cleared", "info", k[1], f"cleared: {e.text}")
                   for k, e in before.items() if k not in now]
        return events
    active, streak = state.setdefault("active", {}), state.setdefault("streak", {})
    need = max(1, int(limits["confirm_polls"]))
    for flat, key in keys.items():
        streak[flat] = streak.get(flat, 0) + 1
        if flat not in active and (key[0] not in CONFIRMED or streak[flat] >= need):
            active[flat] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
            events.append(now[key])
    for flat in [f for f in streak if f not in keys]:
        del streak[flat]
        if flat in active:
            del active[flat]
            kind, _, subject = flat.partition("|")
            events.append(Event(f"{kind}_cleared", "info", subject, f"cleared: {kind.replace('_', ' ')} ({subject})"))
    return events


def state_path() -> Path:
    return forgectl_home() / STATE_NAME


def load_state() -> dict:
    try:
        data = json.loads(state_path().read_text())
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_state(state: dict) -> None:
    path = state_path()
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_suffix(".partial")
    temporary.write_text(json.dumps(state, indent=1))
    os.chmod(temporary, 0o600)
    temporary.replace(path)


def limits_of(config: Config) -> dict:
    notify_settings = config.notify
    return {"learner_silent_minutes": notify_settings["learner_silent_minutes"],
            "confirm_polls": notify_settings["confirm_polls"], "disk_min_gb": config.doctor["disk_min_gb"]}


def tell(event: Event, config: Config, state: dict, now: float) -> None:
    """Print the event; send it to the sinks unless it is below min_severity or a repeat inside the debounce window."""
    settings = config.notify
    alerted = state.setdefault("alerted", {})
    flat = "|".join(event.key)
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} [{event.severity}] {event.kind}: {event.text}"
    if now - alerted.get(flat, 0) < settings["debounce_minutes"] * 60:
        say(line + "  (repeat inside the debounce window: not sent)")
        return
    alerted[flat] = now
    if not notify.wanted(event, settings):
        say(line + f"  (below min_severity {settings['min_severity']}: not sent)")
        return
    say(line)
    for sink, problem in notify.deliver(event, settings).items():
        if problem:
            say(f"  sink {sink} failed: {problem}")


def note_previous_stage(config: Config, old: dict | None, new: dict) -> None:
    """If the plan moved to another stage since the last poll, read the old stage's finished.json (the converge
    decision may be gone from the newest run by now) and put it in the new document as `previous_stage`."""
    if not old or not old.get("read_ok") or not new.get("read_ok"):
        return
    before, after = (old.get("plan") or {}).get("stage"), (new.get("plan") or {}).get("stage")
    if before and after and before != after:
        run, problem = snapshot.read_run(config, before)
        if not problem:
            new["previous_stage"] = {"stage": before, "finished": run["finished"]}


def poll(config: Config) -> dict:
    return snapshot.collect(config, use_console=False)


def acquire_lock():
    path = forgectl_home() / LOCK_NAME
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    handle = open(path, "w")
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        raise Failure("another forgectl watch is running on this machine (see ~/.forgectl/watch.lock)") from None
    handle.write(str(os.getpid()))
    handle.flush()
    return handle


def run(config: Config, once: bool, interval: float | None = None) -> int:
    settings = config.notify
    interval = settings["poll_seconds"] if interval is None else interval
    lock = acquire_lock()
    sinks = notify.enabled(settings)
    say(f"forgectl watch: {'one pass' if once else f'polling every {interval:g} s, Ctrl-C to stop'}; sinks: "
        f"{', '.join(sinks) if sinks else 'none (printing only)'}; state {state_path()}")
    state = load_state()
    limits = limits_of(config)
    if once:
        limits = {**limits, "confirm_polls": 1}   # one pass cannot wait for a second poll to confirm
    code = 0
    try:
        while True:
            try:
                document = poll(config)
            except Exception as error:   # a watcher must outlive a bad poll
                say(f"{time.strftime('%Y-%m-%d %H:%M:%S')} poll failed: {type(error).__name__}: {error}")
                document = None
            if document is not None:
                note_previous_stage(config, state.get("snapshot"), document)
                for event in diff(state.get("snapshot"), document, limits, state):
                    tell(event, config, state, time.time())
                state["snapshot"] = document
                state["saved_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
                save_state(state)
                if once:
                    p = document.get("plan") or {}
                    say(f"{time.strftime('%Y-%m-%d %H:%M:%S')} plan {p.get('state')} ({p.get('stage')}, phase "
                        f"{p.get('phase')}); host {'read' if document['read_ok'] else 'NOT readable'}")
                    code = 0 if document["read_ok"] else 1
            elif once:
                code = 1
            if once:
                return code
            time.sleep(interval)
    except KeyboardInterrupt:
        say("forgectl watch: stopped")
        return 0
    finally:
        lock.close()
