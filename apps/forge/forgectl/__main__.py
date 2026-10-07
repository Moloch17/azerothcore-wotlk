"""forgectl: operate the forge cluster. Run `./forgectl --help` (or `python3 -m forgectl`, with apps/forge on the path)."""
from __future__ import annotations

import argparse
import sys

from . import cluster, config as config_module, confsync, deploy, logs, stage, testcmd, videos
from .config import ConfigError
from .ui import Failure, say

DESCRIPTION = """forgectl operates the forge training cluster from one place. The machines, the host and the ports come
from apps/forge/cluster.toml. Every command says what it is about to do and what happened, exits non-zero on failure,
and asks before it changes anything on a machine (--yes answers for you). Nothing here uses a password: ssh keys only."""

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

    st = add("status", "the host's `forge status` table plus the learner's latest metrics",
             "forgectl status")

    sg = add("stage", "start, resume, pause or cancel a stage on the host (pause and cancel reach the workers too)",
             "forgectl stage resume move2_seek    |    forgectl stage cancel")
    sg.add_argument("action", choices=stage.ACTIONS)
    sg.add_argument("stages", nargs="*", metavar="stage", help="stage name(s); start needs one; pause and cancel take "
                                                              "none (they act on the whole plan)")
    sg.add_argument("--yes", action="store_true", help="do not ask")

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
    bd.add_argument("--timeout", type=float, default=60, help="minutes to wait for each machine (default 60)")

    cs = add("conf-sync", "copy the host's AnimusForge.Curriculum.* keys to every worker's conf (with backups)",
             "forgectl conf-sync --check")
    cs.add_argument("--check", action="store_true", help="only compare; change nothing; exit 1 if they differ")
    cs.add_argument("--yes", action="store_true", help="do not ask")

    ts = add("test", "build and run the GTests and the CPU pytest in the dev container; one summary",
             "forgectl test            |   forgectl test --gpu")
    ts.add_argument("--gpu", action="store_true", help="run pytest on the card instead of the CPU")
    ts.add_argument("--tree", help="the source tree as the container sees it (default: this checkout, found from the "
                                   "container's mounts)")
    ts.add_argument("--build-dir", help="the build directory inside the container")
    ts.add_argument("--jobs", type=int, default=16, help="compile jobs (default 16)")

    vd = add("videos", "collect a stage's evaluation videos from the workers into the run folder",
             "forgectl videos move2_seek --check")
    vd.add_argument("stage")
    vd.add_argument("--check", action="store_true", help="list what each worker would send; copy nothing")
    vd.add_argument("--dry-run", action="store_true", help="print the commands; connect to nothing")
    vd.add_argument("--on-host", action="store_true", help="run on the host (where the run is) instead of here")
    return main


def dispatch(args, config) -> int:
    if args.command == "cluster":
        if getattr(args, "cluster_command", None) == "move-host":
            return deploy.move_host(config, args.machine, args.stage, args.yes, args.timeout)
        return cluster.run(config, include_out=args.all)
    if args.command == "status":
        return stage.status(config)
    if args.command == "stage":
        return stage.run(config, args.action, args.stages, getattr(args, "yes", False))
    if args.command == "logs":
        return logs.run(config, args.machine, args.errors, args.lines, args.wide)
    if args.command == "build":
        return deploy.build(config, args.cluster, args.yes, args.timeout)
    if args.command == "conf-sync":
        return confsync.run(config, args.check, args.yes)
    if args.command == "test":
        return testcmd.run(config, args.tree, args.build_dir, args.gpu, args.jobs)
    if args.command == "videos":
        return videos.run(config, args.stage, args.check, args.dry_run, args.on_host)
    raise Failure(f"unknown command {args.command}")


def main(argv: list[str] | None = None) -> int:
    main_parser = parser()
    args = main_parser.parse_args(argv)
    if not args.command:
        main_parser.print_help()
        return 2
    try:
        config = config_module.load(args.config)
        return dispatch(args, config)
    except (ConfigError, Failure) as problem:
        print(f"forgectl: error: {problem}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("forgectl: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
