"""`forgectl watch --resume`: put the cluster back into its stage after a drop, the way the operator does by hand.

What it mends (each a condition of watch.py, confirmed over `confirm_polls` polls):
  worker_idle     a worker that answers and runs its worldserver, but the host does not see it training (a worker that
                  rebooted into an idle container, a worker whose learner hung and left the plan)
  worker_down     only when the worker's learner STALLED; an unreachable machine or a stopped container is told, never
                  resumed (a resume cannot bring it back)
  learner_silent  the host's learner stopped stepping
  host_idle       the host's sim is idle while its progress.json says the stage is running (the worldserver crashed
                  and docker restarted it idle)
  plan_stale      progress.json stopped being written
How: `forge cancel` on the host and every worker (the learner saves latest.pt), wait until the host is idle and no
worker runs a plan, then `forge resume <stage>` on the host; the workers rejoin by themselves. The same as
`forgectl stage cancel` + `forgectl stage resume <stage>`, without the collect loop's last pass. Recorded in the audit
log as `forgectl watch --resume recover <stage>`.
When not:
  - the operator's hold is set (`forgectl stage cancel|pause`, `build --stop-running`, `move-host`; cleared by
    `stage start|resume`), or the host's last plan says cancelled (a `forge cancel` typed into the console);
  - the stage is decided (finished.json), or the plan is evaluating (a worker condition waits for training);
  - within resume_grace_minutes of the plan starting to run (machines are still joining);
  - within resume_gap_minutes of the last attempt, or after resume_max_attempts attempts that did not bring a healthy
    plan back (then one critical alert, and only alerts until the plan runs healthy again).
"""
from __future__ import annotations

import time

from . import audit, collect, remote, stage as stage_commands
from .config import Config
from .notify import Event
from .ui import Failure, say

WORKER_KINDS = ("worker_idle", "worker_down")
HOST_KINDS = ("learner_silent", "host_idle", "plan_stale")
POLL_SECONDS = 15


def mendable(doc: dict, active: dict) -> list[str]:
    """The confirmed conditions a cancel and resume can mend, as `kind (subject)` texts."""
    machines = {m["name"]: m for m in doc.get("machines") or []}
    plan = doc.get("plan") or {}
    found = []
    for flat in active:
        kind, _, subject = flat.partition("|")
        if kind in WORKER_KINDS:
            if plan.get("phase") != "training":
                continue
            machine = machines.get(subject)
            if kind == "worker_down" and not (machine and machine["reachable"] and not machine.get("problem")
                                              and machine["worldserver"]["up"]
                                              and machine["learner"]["state"] == "stalled"):
                continue
            found.append(f"{kind} ({subject})")
        elif kind in HOST_KINDS:
            found.append(f"{kind} ({subject})")
    return sorted(found)


def consider(config: Config, doc: dict, state: dict, now: float) -> list[Event]:
    """After a poll's events are told: mend the plan if a confirmed condition calls for it. The events to tell."""
    settings = config.notify
    heal = state.setdefault("heal", {})
    if not doc.get("read_ok"):
        return []
    plan = doc.get("plan") or {}
    name = plan.get("stage")
    if plan.get("state") == "running":
        heal.setdefault("running_since", now)
    else:
        heal.pop("running_since", None)
    reasons = mendable(doc, state.get("active") or {})
    attempted = heal.get("last_attempt", 0)
    if not reasons:
        # Healthy again, long enough after the last attempt to count it as mended.
        if plan.get("state") == "running" and now - attempted > settings["resume_grace_minutes"] * 60:
            heal.update(attempts=0, gave_up=False)
        return []
    why = ", ".join(reasons)
    if not name or not stage_commands.STAGE_NAME.match(name):
        return []
    if doc.get("finished"):
        return []
    worker_only = all(r.split(" ")[0] in WORKER_KINDS for r in reasons)
    since = heal.get("running_since")
    if worker_only and (since is None or now - since < settings["resume_grace_minutes"] * 60):
        return []
    if now - attempted < settings["resume_gap_minutes"] * 60:
        return []
    if "cancelled" in (plan.get("last_plan") or "") and plan.get("console_idle"):
        return note_once(heal, "console_cancel", Event(
            "auto_resume_skipped", "info", name, f"{name}: {why}; not resumed: the host's last plan was cancelled "
            "from its console"))
    hold = stage_commands.held(config)
    if hold is not None:
        return note_once(heal, "hold", Event(
            "auto_resume_skipped", "info", name, f"{name}: {why}; not resumed: the operator's hold is set"
            + (f" (since {hold})" if hold else "") + "; `forgectl stage resume` clears it"))
    heal.pop("noted", None)
    if heal.get("attempts", 0) >= settings["resume_max_attempts"]:
        if heal.get("gave_up"):
            return []
        heal["gave_up"] = True
        return [Event("auto_resume_gave_up", "critical", name, f"{name}: {why}; {heal['attempts']} automatic resumes "
                      "did not bring a healthy plan back, so the watcher only reports now. Look at `forgectl cluster` "
                      "and `forgectl logs`; `forgectl stage resume` re-arms it.")]
    heal["attempts"] = heal.get("attempts", 0) + 1
    heal["last_attempt"] = now
    attempt = f"attempt {heal['attempts']}/{settings['resume_max_attempts']:g}"
    say(f"{time.strftime('%Y-%m-%d %H:%M:%S')} auto-resume of {name} ({attempt}): {why}")
    ok, detail = recover(config, name, settings)
    heal["last_attempt"] = time.time()
    heal.pop("running_since", None)
    if ok:
        return [Event("auto_resumed", "warn", name, f"{name}: {why}; cancelled everywhere and resumed ({attempt}). "
                      + detail)]
    return [Event("auto_resume_failed", "critical", name, f"{name}: {why}; the automatic resume failed ({attempt}): "
                  + detail)]


def note_once(heal: dict, why: str, event: Event) -> list[Event]:
    if heal.get("noted") == why:
        return []
    heal["noted"] = why
    return [event]


def recover(config: Config, name: str, settings: dict) -> tuple[bool, str]:
    """Cancel everywhere, wait for the host to go idle, resume on the host: (it worked, what happened)."""
    entry = audit.begin(["watch", "--resume", "recover", name], yes=True)
    outcome = "failed"
    try:
        audit.intent(entry, config.cluster)
        host, workers = config.host, config.workers
        missed = []
        for machine in [host, *workers]:
            try:
                stage_commands.send_checked(config, machine, "forge cancel")
                audit.touch([machine])
            except Failure as failure:
                missed.append(machine.name)
                audit.note(str(failure)[:200])
        if host.name in missed and stage_commands.host_plan_state(config)[0] not in ("idle", "down"):
            return False, f"`forge cancel` was not answered on the host {host.name}"
        deadline = time.time() + settings["resume_idle_wait_minutes"] * 60
        while True:
            states = dict(remote.parallel_map(
                lambda m: (m.name, stage_commands.machine_plan_state(config, m)[0]), [host, *workers]))
            # A worker whose console does not answer is not waited for: the host's view of it is what counts.
            busy = [n for n, s in states.items() if s == "running"]
            if states[host.name] == "down":
                return False, f"the worldserver container on {host.name} is not running"
            if states[host.name] == "idle" and not busy:
                break
            if time.time() > deadline:
                return False, (f"still running {settings['resume_idle_wait_minutes']:g} min after the cancel: "
                               + ", ".join(busy or [host.name]))
            time.sleep(POLL_SECONDS)
        reply = stage_commands.send_checked(config, host, f"forge resume {name}", timeout=60)
        text = " ".join(reply.lines)[:200]
        if "resum" not in text.lower():
            return False, f"the host answered `forge resume {name}` with: {text or '(nothing)'}"
        collect.ensure_loop(config)
        outcome = "done"
        return True, text + (f" (cancel not answered on {', '.join(missed)})" if missed else "")
    except Failure as failure:
        return False, str(failure)[:300]
    finally:
        audit.finish(entry, outcome)
