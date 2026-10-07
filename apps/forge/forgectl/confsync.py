"""`forgectl conf-sync`: the host's AnimusForge.Curriculum.* keys copied to every worker's conf.

The cluster fingerprint hashes those keys, and the conf files are per machine and untracked, so they must be kept
identical by hand today (Phase 3 of the plan moves them into tracked stage files and removes the need).
Also here: editing a machine's AnimusForge.Cluster.Role / Host (used by move-host).
"""
from __future__ import annotations

import hashlib
import re
import shlex
import time
from dataclasses import dataclass, field

from . import audit, remote
from .config import Config, Machine
from .ui import Declined, Failure, confirm, say, table

PREFIX = "AnimusForge.Curriculum."
KEY_LINE = re.compile(r"^\s*(AnimusForge\.Curriculum\.[^\s=]+)\s*=\s*(.*?)\s*$")


def curriculum_keys(text: str) -> dict[str, str]:
    """key -> value of every uncommented Curriculum line (the last one wins, as the config manager reads it)."""
    keys = {}
    for line in text.splitlines():
        match = KEY_LINE.match(line)
        if match:
            keys[match.group(1)] = match.group(2)
    return keys


@dataclass
class Diff:
    missing: list[str] = field(default_factory=list)   # on the host, not on the worker
    extra: list[str] = field(default_factory=list)     # on the worker, not on the host
    different: list[str] = field(default_factory=list)  # on both, values differ

    @property
    def same(self) -> bool:
        return not (self.missing or self.extra or self.different)

    def summary(self) -> str:
        return f"{len(self.missing)} missing, {len(self.extra)} extra, {len(self.different)} different"


def compare(host_keys: dict[str, str], worker_keys: dict[str, str]) -> Diff:
    return Diff(missing=sorted(set(host_keys) - set(worker_keys)), extra=sorted(set(worker_keys) - set(host_keys)),
                different=sorted(k for k in host_keys if k in worker_keys and host_keys[k] != worker_keys[k]))


def synced_text(host_text: str, worker_text: str, stamp: str) -> str:
    """The worker's conf with the host's Curriculum keys: values replaced in place, missing keys appended under a
    comment, keys the host does not have removed. Everything else in the worker's file is left as it is."""
    host_keys = curriculum_keys(host_text)
    host_lines = {}
    for line in host_text.splitlines():
        match = KEY_LINE.match(line)
        if match:
            host_lines[match.group(1)] = line
    out, seen = [], set()
    for line in worker_text.splitlines():
        match = KEY_LINE.match(line)
        if not match:
            out.append(line)
            continue
        key = match.group(1)
        if key not in host_keys:
            continue  # the host has no such key: the fingerprint would differ
        if key in seen:
            continue  # a repeated key: one copy is enough
        seen.add(key)
        out.append(host_lines[key])
    missing = [key for key in host_lines if key not in seen]
    if missing:
        if out and out[-1].strip():
            out.append("")
        out.append(f"# forgectl conf-sync {stamp}: Curriculum keys the worker did not have")
        out.extend(host_lines[key] for key in missing)
    return "\n".join(out) + "\n"


def set_cluster_role(text: str, role: str, host: str) -> str:
    """The conf with AnimusForge.Cluster.Role and .Host set (an existing line is replaced, else appended)."""
    wanted = {"AnimusForge.Cluster.Role": f'"{role}"', "AnimusForge.Cluster.Host": f'"{host}"'}
    out, done = [], set()
    for line in text.splitlines():
        match = re.match(r"^\s*(AnimusForge\.Cluster\.(?:Role|Host))\s*=", line)
        if match and match.group(1) not in done:
            out.append(f"{match.group(1)} = {wanted[match.group(1)]}")
            done.add(match.group(1))
        elif not match:
            out.append(line)
    for key in wanted:
        if key not in done:
            out.append(f"{key} = {wanted[key]}")
    return "\n".join(out) + "\n"


def read_conf(config: Config, machine: Machine) -> str:
    result = remote.on(machine, f"cat {remote.sh_path(config.path_of(machine, 'conf'))}\n", timeout=30)
    if not result.ok:
        raise Failure(f"{machine.name}: cannot read its conf ({result.reason()})")
    return result.out


WRITE_MARKER = "FORGECTL_WRITE="
HEREDOC = "FORGECTL_CONF_EOF"
WRITE_TEMPLATE = """set -e
conf=@CONF@
cp -p "$conf" @BACKUP@
tmp="$conf.forgectl-new.$$"
cp -p "$conf" "$tmp"
cat > "$tmp" <<'@HEREDOC@'
@TEXT@@HEREDOC@
if [ "$(sha256sum < "$tmp" | cut -d' ' -f1)" != @DIGEST@ ]; then
  rm -f "$tmp"
  echo 'the temporary file does not hold the new conf; the conf was not touched' >&2
  exit 3
fi
how=mv
if docker inspect -f '{{range .Mounts}}{{println .Source}}{{end}}' @CONTAINER@ 2>/dev/null \\
    | grep -qxF "$(readlink -f "$conf")"; then
  how=in-place-bind
fi
if [ "$how" = mv ] && ! mv -f "$tmp" "$conf"; then
  how=in-place-mv-failed
fi
if [ "$how" != mv ]; then
  cat "$tmp" > "$conf"
  rm -f "$tmp"
fi
echo @MARKER@$how
"""


def write_script(config: Config, machine: Machine, text: str, stamp: str) -> str:
    """The shell script that replaces the machine's conf with `text`: back it up, write the new text to a temporary
    file in the same directory (copied from the conf first, so owner and mode are kept), check the file holds the
    bytes meant, then `mv` it over the conf. A rename is atomic: a dropped ssh leaves the old conf or the new one,
    never a truncated one. If the conf is a file bind-mounted into the worldserver container (a rename would leave the
    container looking at the old file) or the `mv` fails, the verified temporary file is written over the conf in
    place instead. The last line says which: FORGECTL_WRITE=mv, in-place-bind or in-place-mv-failed."""
    if HEREDOC in text:
        raise Failure("the conf text contains the heredoc marker; refusing to write it")
    values = {"CONF": remote.sh_path(config.path_of(machine, "conf")),
              "BACKUP": remote.sh_path(f"{config.path_of(machine, 'conf')}.bak-{stamp}"),
              "DIGEST": hashlib.sha256(text.encode()).hexdigest(), "CONTAINER": shlex.quote(config.worldserver),
              "HEREDOC": HEREDOC, "MARKER": WRITE_MARKER}
    script = WRITE_TEMPLATE
    for key, value in values.items():
        script = script.replace(f"@{key}@", value)
    return script.replace("@TEXT@", text)


def upload(config: Config, machine: Machine, text: str, stamp: str) -> str:
    """Replace the machine's conf with `text` (see write_script) and say how it was written. Returns the backup."""
    result = remote.on(machine, write_script(config, machine, text, stamp), timeout=60)
    if not result.ok:
        raise Failure(f"{machine.name}: writing the conf failed ({result.reason()}); the conf is as it was")
    how = next((line.split("=", 1)[1] for line in result.out.splitlines() if line.startswith(WRITE_MARKER)), "")
    if not how:
        raise Failure(f"{machine.name}: the conf write did not report how it went; check the file and its backup")
    if how == "in-place-bind":
        say(f"  {machine.name}: the conf is a bind-mounted file, so a rename would leave the container on the old "
            "one: wrote it in place, after checking the temporary copy")
    elif how != "mv":
        say(f"  {machine.name}: mv over the conf failed (bind-mounted file?): wrote it in place instead, after "
            "checking the temporary copy")
    return config.path_of(machine, "conf") + f".bak-{stamp}"


def rewrite(config: Config, machine: Machine, transform, stamp: str) -> str:
    """Read the machine's conf, apply transform(text), back up and write it. Returns the backup's path."""
    return upload(config, machine, transform(read_conf(config, machine)), stamp)


def run(config: Config, check_only: bool, yes: bool) -> int:
    host = config.host
    workers = config.workers
    say(f"Reading the host's conf ({host.name}) and {len(workers)} workers' confs ...")
    host_text = read_conf(config, host)
    host_keys = curriculum_keys(host_text)
    if not host_keys:
        raise Failure(f"{host.name}'s conf has no {PREFIX}* keys: refusing to sync from it")
    texts, diffs, rows = {}, {}, []
    for worker in workers:
        try:
            texts[worker.name] = read_conf(config, worker)
        except Failure as failure:
            rows.append([worker.name, "UNREADABLE", str(failure)])
            continue
        keys = curriculum_keys(texts[worker.name])
        diffs[worker.name] = compare(host_keys, keys)
        rows.append([worker.name, f"{len(keys)} keys", "same" if diffs[worker.name].same else
                     diffs[worker.name].summary()])
    say(table(["machine", f"keys (host: {len(host_keys)})", "against the host"], rows))
    for name, diff in diffs.items():
        for label, keys in (("different", diff.different), ("missing", diff.missing), ("extra", diff.extra)):
            for key in keys[:5]:
                say(f"  {name}: {label}: {key}" + (f" (host {host_keys[key]}, here "
                    f"{curriculum_keys(texts[name])[key]})" if label == "different" else ""))
            if len(keys) > 5:
                say(f"  {name}: ... and {len(keys) - 5} more {label}")
    unreadable = [row[0] for row in rows if row[1] == "UNREADABLE"]
    out_of_sync = [name for name, diff in diffs.items() if not diff.same]
    if check_only or not out_of_sync:
        if unreadable:
            say(f"Could not read: {', '.join(unreadable)}")
            return 1
        say("All workers match the host." if not out_of_sync else
            f"Out of sync: {', '.join(out_of_sync)}. Run forgectl conf-sync (no --check) to copy the host's keys.")
        return 1 if (check_only and out_of_sync) else 0
    stamp = time.strftime("%Y%m%d-%H%M%S")
    plan = [f"back up each worker's conf as mod_animus_forge.conf.bak-{stamp}, then rewrite its {PREFIX}* keys from "
            f"{host.name}'s on: {', '.join(out_of_sync)}",
            "verify afterwards that the counts and values match",
            "(a worker reads its conf at start: the change takes effect when its worldserver restarts)"]
    try:
        confirm(plan, yes)
    except Declined as declined:
        say(f"Nothing was written ({declined}).")
        return 1
    audit.touch(out_of_sync)
    failed = list(unreadable)
    for name in out_of_sync:
        worker = config.machine(name)
        try:
            backup = upload(config, worker, synced_text(host_text, texts[name], stamp), stamp)
            after = curriculum_keys(read_conf(config, worker))
            diff = compare(host_keys, after)
            if not diff.same:
                raise Failure(f"{name}: after the write the conf still differs ({diff.summary()})")
            say(f"{name}: synced, {len(after)} keys match; backup {backup}")
        except Failure as failure:
            say(f"FAILED {failure}")
            failed.append(name)
    return 1 if failed else 0
