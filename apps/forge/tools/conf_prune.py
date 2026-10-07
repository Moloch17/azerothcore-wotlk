#!/usr/bin/env python3
"""Which AnimusForge.* keys a build dropped, and clean a machine's conf of the ones it no longer reads.

    conf_prune.py --removed OLD_REV NEW_REV           keys in worldserver.conf.dist at OLD_REV and not at NEW_REV
    conf_prune.py --check CONF                        list the unknown keys of a machine's conf file (exit 1 if any)
    conf_prune.py --prune CONF                        comment them out (a timestamped backup is written first)
    conf_prune.py --check|--prune --ssh user@host:PATH    the same, over ssh (BatchMode, no passwords), for a cluster
                                                      machine; ~/ in PATH is the remote user's home

CONF is a machine's mod_animus_forge.conf or worldserver.conf (the sim reads both: ForgeMain.cpp loads
modules/mod_animus_forge.conf after worldserver.conf). A key is *unknown* when the checkout's
src/server/apps/worldserver/worldserver.conf.dist (or --dist FILE / --dist-rev REV) neither lists it nor names a family
it belongs to (AnimusForge.Stage.<name>.Envs and .TicksPerDecision, AnimusForge.Curriculum.Arena.<stage>.<arena>.<key>,
AnimusForge.Curriculum.Stage.<name>.GoalPlaces: read by name at run time, so they are not listed key by key; the
families are read from the template's comments and from the C++ that reads them). A key of one of those families
for a stage that no longer exists is unknown too.

The conf.dist diff IS the curriculum tuning's Visit diff. tests/test_conf_covers_tuning.py enforces that every key
CurriculumTuning::Visit hands to the config is in conf.dist and every AnimusForge.Curriculum.* key in conf.dist is
read by Visit, so --removed OLD NEW names exactly what the sim stopped reading (test_conf_prune.py checks this
between the pre-cleanup tag and HEAD).

An unknown key in a live conf is harmless (docs/forge/deploy-gate.md, "Unknown keys"): it neither stops the sim
starting nor enters the cluster fingerprint. Pruning is housekeeping, so a stale key cannot be mistaken for a setting
that still does something. Only uncommented assignments are touched; --prune never deletes a line.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable

REPO = Path(__file__).resolve().parents[3]
DIST = "src/server/apps/worldserver/worldserver.conf.dist"
GAME = REPO / "src" / "server" / "game" / "Animus"
KEY = re.compile(r"^[ \t]*(AnimusForge\.[A-Za-z0-9_.]+)[ \t]*=", re.M)
Runner = Callable[[list, "str | None"], "tuple[int, str, str]"]


# -------------------------------------------------------------------------------------------- the template

def dist_keys(text: str) -> set[str]:
    """The AnimusForge.* keys a conf.dist assigns (uncommented lines)."""
    return set(KEY.findall(text))


def git_show(repo: Path, rev: str, path: str = DIST) -> str:
    result = subprocess.run(["git", "-C", str(repo), "show", f"{rev}:{path}"], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"conf_prune: git show {rev}:{path}: {result.stderr.strip()}")
    return result.stdout


def removed_keys(repo: Path, old: str, new: str) -> list[str]:
    return sorted(dist_keys(git_show(repo, old)) - dist_keys(git_show(repo, new)))


def group_of(key: str) -> str:
    """The tuning group a key belongs to: AnimusForge.Curriculum.<Group>, or AnimusForge.<Section> outside it."""
    parts = key.split(".")
    return ".".join(parts[:3]) if len(parts) > 3 and parts[1] == "Curriculum" else ".".join(parts[:2])


def group_counts(keys: list[str]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for key in keys:
        counts[group_of(key)] = counts.get(group_of(key), 0) + 1
    return dict(sorted(counts.items()))


# -------------------------------------------------------------------------------------------- families

def family_patterns(dist_text: str, source_root: Path = GAME) -> list[re.Pattern]:
    """Regexes of the keys the sim reads by name at run time rather than through a list of keys.

    From the template's comments (`AnimusForge.Stage.<name>.Envs`: each <placeholder> stands for one dotted name) and
    from the C++ that builds the names: the Arena.{}.{}.<key> and Stage.{}.<key> formats of the curriculum scenario
    (under AnimusForge.Curriculum.) and the suffixes ForgeConfig::Load picks out of AnimusForge.Stage.*."""
    patterns: set[str] = set()
    for placeholder in re.findall(r"AnimusForge\.[A-Za-z0-9_.]*<[A-Za-z0-9_.<>]*", dist_text):
        placeholder = placeholder.rstrip(".")
        patterns.add(re.sub(r"<[a-z]+>", "[^.]+", re.escape(placeholder).replace("\\<", "<").replace("\\>", ">")))
    for path in sorted(source_root.rglob("*.cpp")):
        text = path.read_text()
        for kind, leaf in re.findall(r'"\{\}(Arena|Stage)\.\{\}(?:\.\{\})?\.([A-Za-z0-9]+)"', text):
            holes = "[^.]+\\.[^.]+" if kind == "Arena" else "[^.]+"
            patterns.add(f"AnimusForge\\.Curriculum\\.{kind}\\.{holes}\\.{leaf}")
        if path.name == "ForgeConfig.cpp":
            for suffix in re.findall(r'rfind\("\.([A-Za-z]+)"\)|std::string const suffix = "\.([A-Za-z]+)"', text):
                patterns.add(f"AnimusForge\\.Stage\\.[^.]+\\.{suffix[0] or suffix[1]}")
    return [re.compile(pattern) for pattern in sorted(patterns)]


def stage_of(key: str) -> str | None:
    """The stage a stage-scoped key names, or None for any other key."""
    found = re.match(r"AnimusForge\.Stage\.([^.]+)\.", key) or re.match(r"AnimusForge\.Curriculum\.Arena\.([^.]+)\.",
                                                                        key) \
        or re.match(r"AnimusForge\.Curriculum\.Stage\.([^.]+)\.", key)
    return found.group(1) if found else None


def live_stages(source_root: Path = GAME) -> set[str] | None:
    """The stage names of Stages.cpp (sim_metrics reads them), None when that file cannot be read."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        import sim_metrics
        return set(sim_metrics.Extractor(sim_metrics.Source(source_root / "Scenario" / "Curriculum")).stages())
    except Exception:  # noqa: BLE001 - the stage check is an extra; the key check stands without it
        return None


# -------------------------------------------------------------------------------------------- a conf file

def unknown_assignments(conf_text: str, known: set[str], families: list[re.Pattern],
                        stages: set[str] | None = None) -> list[tuple[int, str, str]]:
    """[(line number, key, why)] for each uncommented AnimusForge.* assignment the new build does not read."""
    out = []
    for number, line in enumerate(conf_text.splitlines(), 1):
        found = KEY.match(line)
        if not found:
            continue
        key = found.group(1)
        if key in known:
            continue
        if any(pattern.fullmatch(key) for pattern in families):
            stage = stage_of(key)
            if stages is not None and stage is not None and stage not in stages:
                out.append((number, key, f"stage {stage} no longer exists"))
            continue
        out.append((number, key, "not in worldserver.conf.dist"))
    return out


def prune_text(conf_text: str, unknown: list[tuple[int, str, str]], stamp: str) -> str:
    """The conf with each unknown assignment commented out, the reason on the line, never deleted."""
    lines = conf_text.split("\n")
    for number, _, why in unknown:
        lines[number - 1] = f"#pruned {stamp} ({why}): {lines[number - 1]}"
    return "\n".join(lines)


def local_runner(command: list, input_text: str | None) -> tuple[int, str, str]:
    result = subprocess.run(command, input=input_text, capture_output=True, text=True)
    return result.returncode, result.stdout, result.stderr


def ssh_command(target: str, remote: str) -> list[str]:
    """ssh to `target` (user@host) running `remote`: keys only, never a password prompt, never hanging."""
    return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", target, remote]


def quote(text: str) -> str:
    """A shell word for `text`; a leading ~/ is kept as the remote user's home."""
    if text.startswith("~/"):
        return '"$HOME"/' + quote(text[2:])
    return "'" + text.replace("'", "'\\''") + "'"


def read_conf(spec: str, ssh: bool, runner: Runner) -> str:
    if not ssh:
        return Path(spec).read_text()
    target, path = spec.split(":", 1)
    code, out, error = runner(ssh_command(target, f"cat {quote(path)}"), None)
    if code:
        raise SystemExit(f"conf_prune: ssh {target}: could not read {path}: {error.strip() or code}")
    return out


def write_conf(spec: str, ssh: bool, runner: Runner, original: str, pruned: str, stamp: str) -> str:
    """Back the file up, then write the pruned text; returns the backup's path."""
    if not ssh:
        path = Path(spec)
        backup = path.with_name(f"{path.name}.bak-{stamp}")
        backup.write_text(original)
        path.write_text(pruned)
        return str(backup)
    target, path = spec.split(":", 1)
    backup = f"{path}.bak-{stamp}"
    script = (f"set -e; cp -p {quote(path)} {quote(backup)}; cat > {quote(path + '.new')}; "
              f"mv {quote(path + '.new')} {quote(path)}")
    code, _, error = runner(ssh_command(target, script), pruned)
    if code:
        raise SystemExit(f"conf_prune: ssh {target}: could not write {path}: {error.strip() or code}")
    return f"{target}:{backup}"


def run(args: argparse.Namespace, runner: Runner = local_runner, out=sys.stdout) -> int:
    repo = Path(args.repo)
    if args.removed:
        keys = removed_keys(repo, *args.removed)
        for key in keys:
            print(key, file=out)
        print(f"\n{len(keys)} key(s) in {args.removed[0]} and not in {args.removed[1]}", file=out)
        for group, count in group_counts(keys).items():
            print(f"  {group}: {count}", file=out)
        return 0
    spec = args.ssh or args.conf
    dist_text = Path(args.dist).read_text() if args.dist else (
        git_show(repo, args.dist_rev) if args.dist_rev else (repo / DIST).read_text())
    known = dist_keys(dist_text)
    source = repo / "src" / "server" / "game" / "Animus"
    conf_text = read_conf(spec, bool(args.ssh), runner)
    unknown = unknown_assignments(conf_text, known, family_patterns(dist_text, source),
                                  None if args.no_stage_check else live_stages(source))
    old_known = dist_keys(git_show(repo, args.old)) if args.old else None
    for number, key, why in unknown:
        note = ""
        if old_known is not None and why == "not in worldserver.conf.dist":
            note = "  (removed by this build)" if key in old_known else "  (the old build did not read it either)"
        print(f"{spec}:{number}: {key}  [{why}]{note}", file=out)
    print(f"{len(unknown)} unknown key(s) of {len(KEY.findall(conf_text))} AnimusForge.* assignments in {spec}",
          file=out)
    if args.prune and unknown:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        backup = write_conf(spec, bool(args.ssh), runner, conf_text, prune_text(conf_text, unknown, stamp), stamp)
        print(f"commented out {len(unknown)} line(s); the original is {backup}", file=out)
    return 1 if (args.check and unknown) else 0


def parse(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--removed", nargs=2, metavar=("OLD_REV", "NEW_REV"))
    parser.add_argument("--check", action="store_true", help="list the unknown keys of CONF; exit 1 if there are any")
    parser.add_argument("--prune", action="store_true", help="comment them out, after a timestamped backup")
    parser.add_argument("conf", nargs="?", help="a machine's mod_animus_forge.conf or worldserver.conf")
    parser.add_argument("--ssh", metavar="USER@HOST:PATH", help="the conf of a cluster machine, over ssh (BatchMode)")
    parser.add_argument("--dist", help="the template to compare against (default: this checkout's)")
    parser.add_argument("--dist-rev", help="compare against the template at this git revision")
    parser.add_argument("--old", metavar="REV", help="say which unknown keys the old build did not read either")
    parser.add_argument("--no-stage-check", action="store_true", help="do not flag keys of stages that are gone")
    parser.add_argument("--repo", default=str(REPO))
    args = parser.parse_args(argv)
    if args.removed:
        if args.check or args.prune or args.conf or args.ssh:
            parser.error("--removed stands alone")
    else:
        if args.check == args.prune:
            parser.error("one of --removed, --check or --prune")
        if bool(args.conf) == bool(args.ssh):
            parser.error("give CONF or --ssh USER@HOST:PATH, not both or neither")
        if args.ssh and ":" not in args.ssh:
            parser.error("--ssh takes user@host:/path")
    return args


if __name__ == "__main__":
    sys.exit(run(parse()))
