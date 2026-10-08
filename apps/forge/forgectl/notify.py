"""Where `forgectl watch` sends a message: the sinks of the [notify] section of cluster.toml.

desktop  `notify-send` if it is installed.
command  any shell command; the message is in $FORGECTL_MESSAGE (with $FORGECTL_KIND, $FORGECTL_SEVERITY and
         $FORGECTL_SUBJECT) and is also the first argument ($1) of the command.
webhook  an HTTP POST of a JSON body {"text", "content", "kind", "severity", "subject", "time"} (text is what Slack and
         ntfy-style hooks read, content is Discord's); an optional header "Name: value".
file     one line appended per message.
A sink that fails is reported and skipped; it never stops the watcher. With no sink configured nothing is sent and the
watcher only prints.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from .config import SEVERITIES

SEND_TIMEOUT = 15


@dataclass(frozen=True)
class Event:
    kind: str          # the closed vocabulary of watch.py
    severity: str      # info | warn | critical
    subject: str       # what it is about: a machine, a stage, ""
    text: str

    @property
    def key(self) -> tuple[str, str]:
        return self.kind, self.subject

    @property
    def message(self) -> str:
        return f"forgectl: {self.text}"


def enabled(settings: dict) -> list[str]:
    names = []
    if settings.get("desktop"):
        names.append("desktop")
    for name, key in (("command", "command"), ("webhook", "webhook_url"), ("file", "file")):
        if settings.get(key):
            names.append(name)
    return names


def wanted(event: Event, settings: dict) -> bool:
    return SEVERITIES.index(event.severity) >= SEVERITIES.index(settings.get("min_severity", "info"))


def stamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def send_desktop(event: Event, settings: dict) -> str:
    program = shutil.which("notify-send")
    if not program:
        return "notify-send is not installed"
    urgency = {"info": "low", "warn": "normal", "critical": "critical"}[event.severity]
    result = subprocess.run([program, "-u", urgency, "-a", "forgectl", f"forgectl {event.kind}", event.text],
                            capture_output=True, text=True, timeout=SEND_TIMEOUT)
    return "" if result.returncode == 0 else f"notify-send exit {result.returncode}: {result.stderr.strip()[:200]}"


def send_command(event: Event, settings: dict) -> str:
    env = dict(os.environ, FORGECTL_MESSAGE=event.message, FORGECTL_KIND=event.kind, FORGECTL_SEVERITY=event.severity,
               FORGECTL_SUBJECT=event.subject)
    result = subprocess.run(["sh", "-c", settings["command"], "forgectl-notify", event.message], env=env,
                            capture_output=True, text=True, timeout=SEND_TIMEOUT)
    detail = (result.stderr or result.stdout).strip()[:200]
    return "" if result.returncode == 0 else f"exit {result.returncode}: {detail}"


def send_webhook(event: Event, settings: dict) -> str:
    url = settings["webhook_url"]
    if not url.startswith(("http://", "https://")):
        return f"webhook_url must start with http:// or https://, got {url[:40]!r}"
    body = json.dumps({"text": event.message, "content": event.message, "kind": event.kind,
                       "severity": event.severity, "subject": event.subject, "time": stamp()}).encode()
    headers = {"Content-Type": "application/json", "User-Agent": "forgectl-watch"}
    name, sep, value = settings.get("webhook_header", "").partition(":")
    if sep and name.strip():
        headers[name.strip()] = value.strip()
    request = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=SEND_TIMEOUT) as answer:
            return "" if 200 <= answer.status < 300 else f"HTTP {answer.status}"
    except urllib.error.HTTPError as error:
        return f"HTTP {error.code}"
    except (urllib.error.URLError, OSError) as error:
        return f"{type(error).__name__}: {getattr(error, 'reason', error)}"


def send_file(event: Event, settings: dict) -> str:
    path = Path(settings["file"]).expanduser()
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(f"{stamp()} {event.severity} {event.kind} {event.subject or '-'}: {event.text}\n")
    return ""


SENDERS = {"desktop": send_desktop, "command": send_command, "webhook": send_webhook, "file": send_file}


def deliver(event: Event, settings: dict) -> dict[str, str]:
    """Send the event to every configured sink: {sink: "" if it worked, else why not}."""
    outcome = {}
    for name in enabled(settings):
        try:
            outcome[name] = SENDERS[name](event, settings)
        except (OSError, subprocess.SubprocessError) as error:
            outcome[name] = f"{type(error).__name__}: {error}"
    return outcome
