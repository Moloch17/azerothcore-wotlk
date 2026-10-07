"""`forgectl test`: the GTests and the CPU pytest in the dev container, with one summary.

The work is apps/forge/tools/forgectl-test.sh (ported from the gitignored var/staging_test.sh); this module finds
where the tree is as the container sees it, runs the script with `docker exec`, and turns its marker lines into one
summary."""
from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from . import remote
from .config import Config
from .ui import Failure, note, say

SCRIPT = "apps/forge/tools/forgectl-test.sh"


@dataclass
class Counts:
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    errors: int = 0
    exit: int | None = None
    ran: bool = False
    failing: list[str] = field(default_factory=list)


@dataclass
class Summary:
    unit: Counts = field(default_factory=Counts)
    pytest: Counts = field(default_factory=Counts)
    fatal: str = ""
    build_failed: bool = False
    build_errors: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return (not self.fatal and not self.build_failed and self.unit.ran and self.pytest.ran
                and self.unit.exit == 0 and self.pytest.exit == 0 and not self.unit.failed
                and not self.pytest.failed and not self.pytest.errors)


def parse_pytest_line(line: str, counts: Counts) -> None:
    for number, word in re.findall(r"(\d+) (passed|failed|skipped|errors?|xfailed|xpassed)", line):
        if word == "passed":
            counts.passed = int(number)
        elif word == "failed":
            counts.failed = int(number)
        elif word == "skipped":
            counts.skipped = int(number)
        elif word.startswith("error"):
            counts.errors = int(number)


def parse_unit_line(line: str, counts: Counts, total: list[int]) -> None:
    match = re.match(r"\[==========\] (\d+) tests? from", line)
    if match:
        total.append(int(match.group(1)))
    match = re.match(r"\[  PASSED  \] (\d+) test", line)
    if match:
        counts.passed = int(match.group(1))
    match = re.match(r"\[  SKIPPED \] (\d+) test", line)
    if match:
        counts.skipped = int(match.group(1))
    match = re.match(r"\[  FAILED  \] (\d+) test", line)
    if match:
        counts.failed = int(match.group(1))


def parse_output(text: str) -> Summary:
    summary = Summary()
    total: list[int] = []
    for line in text.splitlines():
        tag, _, rest = line.partition(" ")
        if tag == "FATAL":
            summary.fatal = rest
        elif tag == "BUILD_FAILED":
            summary.build_failed = True
        elif tag == "BUILD_ERROR":
            summary.build_errors.append(rest)
        elif tag == "UNIT_EXIT":
            summary.unit.exit, summary.unit.ran = int(rest), True
        elif tag == "UNIT_LINE":
            parse_unit_line(rest, summary.unit, total)
        elif tag == "UNIT_FAILED":
            summary.unit.failing.append(rest)
        elif tag == "PYTEST_EXIT":
            summary.pytest.exit, summary.pytest.ran = int(rest), True
        elif tag == "PYTEST_LINE":
            parse_pytest_line(rest, summary.pytest)
        elif tag == "PYTEST_FAILED":
            summary.pytest.failing.append(rest)
    if total and summary.unit.ran and not summary.unit.passed and not summary.unit.failed:
        summary.unit.passed = total[-1] - summary.unit.skipped  # a run that printed no PASSED line: crashed or empty
    if summary.unit.ran and summary.unit.exit not in (0, None) and not summary.unit.failing and not summary.unit.failed:
        summary.unit.failing.append("(the test binary exited non-zero without naming a test: crashed or timed out)")
    return summary


def render(summary: Summary) -> str:
    lines = ["== forgectl test summary =="]
    if summary.fatal:
        lines.append(f"STOPPED EARLY: {summary.fatal}")
    if summary.build_failed:
        lines.append("BUILD FAILED" + "".join(f"\n  {e}" for e in summary.build_errors))
    for label, counts in (("GTests", summary.unit), ("pytest", summary.pytest)):
        if not counts.ran:
            lines.append(f"{label}: NOT RUN")
            continue
        lines.append(f"{label}: {counts.passed} passed, {counts.failed + counts.errors} failed, {counts.skipped} "
                     f"skipped (exit {counts.exit})")
        lines.extend(f"  FAILED {name}" for name in counts.failing)
    lines.append("RESULT: " + ("PASS" if summary.ok else "FAIL"))
    return "\n".join(lines)


def container_path(config: Config, tree: Path) -> tuple[str, str]:
    """(where `tree` is inside the dev container, that mount's root there): the longest bind-mount whose source
    contains it."""
    result = remote.execute(["docker", "inspect", "-f", '{{range .Mounts}}{{.Source}}={{.Destination}}{{"\\n"}}{{end}}',
                             config.dev["container"]], timeout=30)
    if not result.ok:
        raise Failure(f"cannot inspect the dev container {config.dev['container']!r}: "
                      f"{(result.err or result.out).strip()[:200]} (is it running? pass --tree to skip the lookup)")
    best = None
    for line in result.out.splitlines():
        source, _, destination = line.partition("=")
        if source and (tree == Path(source) or Path(source) in tree.parents):
            if best is None or len(source) > len(best[0]):
                best = (source, destination)
    if best is None:
        raise Failure(f"{tree} is not inside any mount of {config.dev['container']}; pass --tree with the path the "
                      "container sees")
    return str(Path(best[1]) / tree.relative_to(best[0])), best[1]


def run(config: Config, tree: str | None, build_dir: str | None, gpu: bool, jobs: int) -> int:
    container = config.dev["container"]
    if tree and build_dir:
        src, build = tree, build_dir
    else:
        found, mount = container_path(config, config.repo_root)
        src = tree or found
        build = build_dir or f"{mount}/var/{config.dev['build_dir_name']}-{Path(src).name}"
    command = ["docker", "exec", container, "bash", f"{src}/{SCRIPT}", "--src", src, "--build", build,
               "--python", config.dev["python"], "--jobs", str(jobs)] + (["--gpu"] if gpu else [])
    say(f"Testing {src} in the container {container}; build tree {build}; pytest on the "
        f"{'GPU' if gpu else 'CPU (HIP_VISIBLE_DEVICES empty)'}. The first build takes a long time.")
    note("running: " + " ".join(command))
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    captured = []
    for line in process.stdout:
        captured.append(line.rstrip("\n"))
        if line.startswith(("STEP ", "FATAL ")):
            say(line.rstrip())
    process.wait()
    summary = parse_output("\n".join(captured))
    if process.returncode not in (0, 1) and not summary.fatal and not summary.unit.ran:
        summary.fatal = f"the script exited {process.returncode}: " + (captured[-1] if captured else "no output")
    say(render(summary))
    say(f"Logs: {build}.{{build,unit,pytest}}.log inside the container")
    return 0 if summary.ok else 1
