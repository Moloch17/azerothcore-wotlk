"""forgectl: operate the forge cluster. Run `./forgectl --help` (or `python3 -m forgectl` with apps/forge on the
path)."""
from __future__ import annotations

import argparse
import sys

from . import audit, cluster, config as config_module, confsync, deploy, doctor, logs, snapshot, stage, videos
from .config import ConfigError
from .ui import Failure, say

DESCRIPTION = """forgectl operates the forge training cluster from one place. The machines, the host and the ports come
from apps/forge/cluster.toml. Every command says what it is about to do and what happened, exits non-zero on failure,
and asks before it changes anything on a machine (--yes answers for you). Nothing here uses a password: ssh keys
only."""

EXAMPLES = """examples:
  forgectl cluster                    which machines are up, on which revision, and whether the learners step
  forgectl status                     the host's `forge status` table and the learner's latest numbers
  forgectl stage resume move2_seek    continue a stage on the host
  forgectl build --cluster            push, rebuild every machine, wait for each to be ready
Each command has its own --help with an example. The manual is docs/forge/forgectl.md."""


def parser() -> argparse.ArgumentParser:
    raw = argparse.RawDescriptionHelpFormatter
    main = argparse.ArgumentParser(prog="forgectl", description=DESCRIPTION, epilog=EXAMPLES, formatter_class=raw)
    main.add_argument("--config", help="the cluster file (default apps/forge/cluster.toml, or $FORGECTL_CONFIG)")
    commands = main.add_subparsers(dest="command", metavar="<command>")

    def add(name, help_text, example, **kwargs):
        return commands.add_parser(name, help=help_text, description=help_text, epilog="example:\n  " + example,
                                   formatter_class=raw, **kwargs)

    cl = add("cluster", "one table of the machines: role, revision, worldserver, learner, load, disk, GPU",
             "forgectl cluster            |   forgectl cluster move-host thomas move2_seek")
    cl.add_argument("--all", action="store_true", help="include machines that are out of the cluster")
    cl.add_argument("--json", action="store_true",
                    help="print one JSON object (schema in docs/forge/forgectl.md) instead of the table; exit 0 when "
                         "the host could be read")
    cl_sub = cl.add_subparsers(dest="cluster_command", metavar="[move-host]")
    mv = cl_sub.add_parser("move-host", formatter_class=raw,
                           help="move the host role (and a run) to another machine",
                           description="Move the host: cancel, copy the run directory, set the roles in every conf, "
                                       "pull and rebuild everything, resume. Prints the plan first and asks.",
                           epilog="example:\n  forgectl cluster move-host thomas move2_seek")
    mv.add_argument("machine", help="the new host (must be in the cluster)")
    mv.add_argument("stage", nargs="?", help="the stage whose run directory is copied and then resumed")
    mv.add_argument("--yes", action="store_true", help="do not ask")
    mv.add_argument("--timeout", type=float, default=60, help="minutes to wait for each machine's build (default 60)")

    st = add("status", "the host's `forge status` table plus the learner's latest metrics (--json: one JSON object)",
             "forgectl status   |   forgectl status --json")
    st.add_argument("--json", action="store_true",
                    help="print one JSON object read from the run's files (schema in docs/forge/forgectl.md); exit 0 "
                         "when the host could be read")
    st.add_argument("--stage", help="with --json: the run directory to read (default: the newest progress.json)")
    st.add_argument("--no-console", action="store_true",
                    help="with --json: do not type `forge status` into the host's console (files and ssh only)")

    add("doctor", "read-only pre-flight: PASS, WARN or FAIL per check, exit 1 on any FAIL, a 'to do' under each",
        "forgectl doctor")

    sg = add("stage", "start, resume, pause or cancel a stage on the host (pause and cancel reach the workers too)",
             "forgectl stage resume move2_seek    |    forgectl stage cancel")
    sg.add_argument("action", choices=stage.ACTIONS)
    sg.add_argument("stages", nargs="*", metavar="stage", help="stage name(s); start needs one; pause and cancel take "
                                                              "none (they act on the whole plan)")
    sg.add_argument("--yes", action="store_true", help="do not ask")
    sg.add_argument("--archive-ok", action="store_true",
                    help="start: with --yes, allow archiving an existing run of more than 1M steps (or one whose "
                         "size cannot be read); without --yes the prompt shows the run and you answer it")

    lg = add("logs", "the worldserver and learner logs of a machine, errors and warnings first",
             "forgectl logs thomas --errors")
    lg.add_argument("machine", nargs="?", help="default: the host")
    lg.add_argument("--errors", action="store_true", help="only the problems")
    lg.add_argument("--lines", type=int, default=40, help="lines per section (default 40)")
    lg.add_argument("--wide", action="store_true", help="do not cut long lines")

    bd = add("build", "rebuild the worldserver here, or with --cluster push and rebuild every machine",
             "forgectl build --cluster")
    bd.add_argument("--cluster", action="store_true", help="push to the lan remote, then cluster-pull on every "
                                                          "machine in the cluster, and wait for each to be ready")
    bd.add_argument("--yes", action="store_true", help="do not ask")
    bd.add_argument("--stop-running", action="store_true",
                    help="with --cluster: if a stage is running, cancel it on every machine (it saves latest.pt), "
                         "wait for 'Plan ended', then build; without this a running stage makes the build refuse")
    bd.add_argument("--timeout", type=float, default=60, help="minutes to wait for each machine (default 60)")

    cs = add("conf-sync", "copy the host's AnimusForge.Curriculum.* keys to every worker's conf (with backups)",
             "forgectl conf-sync --check")
    cs.add_argument("--check", action="store_true", help="only compare; change nothing; exit 1 if they differ")
    cs.add_argument("--yes", action="store_true", help="do not ask")


    vd = add("videos", "collect a stage's evaluation videos from the workers into the run folder",
             "forgectl videos move2_seek --check")
    vd.add_argument("stage")
    vd.add_argument("--check", action="store_true", help="list what each worker would send; copy nothing")
    vd.add_argument("--dry-run", action="store_true", help="print the commands; connect to nothing")
    vd.add_argument("--on-host", action="store_true", help="run on the host (where the run is) instead of here")
    vd.add_argument("--yes", action="store_true", help="do not ask before copying")
    return main


def dispatch(args, config) -> int:
    if args.command == "cluster":
        if getattr(args, "cluster_command", None) == "move-host":
            return deploy.move_host(config, args.machine, args.stage, args.yes, args.timeout)
        if args.json:
            return snapshot.cluster_json(config, include_out=args.all)
        return cluster.run(config, include_out=args.all)
    if args.command == "status":
        if args.json:
            return snapshot.status_json(config, args.stage, not args.no_console)
        if args.stage or args.no_console:
            raise Failure("--stage and --no-console only go with --json")
        return stage.status(config)
    if args.command == "doctor":
        return doctor.run(config)
    if args.command == "stage":
        return stage.run(config, args.action, args.stages, getattr(args, "yes", False),
                          getattr(args, "archive_ok", False))
    if args.command == "logs":
        return logs.run(config, args.machine, args.errors, args.lines, args.wide)
    if args.command == "build":
        return deploy.build(config, args.cluster, args.yes, args.timeout, stop_running=args.stop_running)
    if args.command == "conf-sync":
        return confsync.run(config, args.check, args.yes)
    if args.command == "videos":
        return videos.run(config, args.stage, args.check, args.dry_run, args.on_host, args.yes)
    raise Failure(f"unknown command {args.command}")


def changes_state(args) -> bool:
    """Whether the command can change something (and so is audited): not the read-only ones."""
    if args.command == "stage":
        return args.action != "status"
    if args.command == "build":
        return True
    if args.command == "conf-sync":
        return not args.check
    if args.command == "cluster":
        return getattr(args, "cluster_command", None) == "move-host"
    if args.command == "videos":
        return not (args.check or args.dry_run)
    return False


def planned_machines(args, config) -> list:
    """The machines a state-changing command is expected to touch (the intent line; the result line has the ones it did)."""
    if args.command == "stage":
        return [config.host] + (config.workers if args.action in ("pause", "cancel") else [])
    if args.command == "build":
        return list(config.cluster) if args.cluster else [m for m in config.machines if m.local] or [config.host]
    if args.command == "conf-sync":
        return list(config.workers)
    if args.command == "cluster":
        return list(config.cluster)
    if args.command == "videos":
        return [*config.workers, *([config.host] if args.on_host else [])]
    return []


def main(argv: list[str] | None = None) -> int:
    main_parser = parser()
    args = main_parser.parse_args(argv)
    if not args.command:
        main_parser.print_help()
        return 2
    entry, outcome = None, "failed"
    try:
        if changes_state(args):
            entry = audit.begin(sys.argv[1:] if argv is None else argv, getattr(args, "yes", False))
        config = config_module.load(args.config)
        if entry:
            audit.intent(entry, planned_machines(args, config))
        code = dispatch(args, config)
        outcome = "declined" if entry and entry.confirmation == "declined" else "done" if code == 0 else "failed"
        return code
    except (ConfigError, Failure, audit.AuditError) as problem:
        print(f"forgectl: error: {problem}", file=sys.stderr)
        if entry:
            entry.notes.append(str(problem)[:300])
        return 1
    except KeyboardInterrupt:
        print("forgectl: interrupted", file=sys.stderr)
        return 130
    finally:
        if entry:
            audit.finish(entry, outcome)


if __name__ == "__main__":
    sys.exit(main())
