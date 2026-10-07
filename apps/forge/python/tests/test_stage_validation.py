"""Every curriculum stage passes the sim's own validation (Stages.cpp: Problem and ArenaProblem).

A stage the validation refuses is left out, every stage extending it goes with it, and `forge start` refuses the queue
until all are valid (CurriculumSound) -- which a stage-table error used to show only on a built, running server
(move3_vertical's ledges, 2026-10-05). This compiles Stages.cpp on its own, with the build's own flags
(compile_commands.json) and a stand-in for the logger, links it to a main that asks CurriculumStages() and
CurriculumProblems(), and runs it: the real definitions through the real validation, without a server. Skipped where
there is no configured build to borrow flags from (the dev container has one).
"""

import json
import os
import shlex
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[4]
STAGES_CPP = REPO / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum" / "Stages" / "Stages.cpp"
COMPILE_DB = Path(os.environ.get("ANIMUS_COMPILE_DB", "/azerothcore/var/build/obj/compile_commands.json"))
# The movement curriculum as rebuilt (2026-10-05): M1, M2 seek (perception-goals P1, 2026-10-06), M3 interact and M4
# follow; then the combat stages (dungeon-curriculum C1-C3, 2026-10-06) and the party stages (G1, 2026-10-07). Later
# stages add themselves here as they land.
MOVEMENT = ["move1_controls", "move2_seek", "move3_interact", "move4_follow", "combat1_fight", "combat2_packs",
            "combat3_survive", "group1_roles"]

LOG_STUB = """#pragma once
#define LOG_ERROR(category, ...) ((void)0)
#define LOG_WARN(category, ...) ((void)0)
#define LOG_INFO(category, ...) ((void)0)
"""

MAIN = """#include "StageDefinition.h"
#include <cstdio>
int main()
{
    for (auto const& stage : Animus::Curriculum::CurriculumStages())
        std::printf("valid %s\\n", stage.Name.c_str());
    for (auto const& problem : Animus::Curriculum::CurriculumProblems())
        std::printf("left out %s\\n", problem.c_str());
    return 0;
}
"""


def _command() -> tuple[list[str], str] | None:
    """The compiler and flags the build compiles Stages.cpp with, or None."""
    if not COMPILE_DB.is_file():
        return None
    entries = json.loads(COMPILE_DB.read_text())
    entry = next((e for e in entries if e["file"].endswith("Curriculum/Stages/Stages.cpp")), None)
    if entry is None:
        return None
    args = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
    out, skip = [], 0
    for i, arg in enumerate(args):
        if skip:
            skip -= 1
            continue
        if arg in ("-o", "-MF", "-MT"):
            skip = 1
            continue
        if arg in ("-MD", "-MMD", "-c") or arg == entry["file"] or arg.startswith("-Werror"):
            continue
        if arg == "-Xclang" and i + 1 < len(args) and args[i + 1] in ("-include-pch", "-include"):
            skip = 3
            continue
        if arg.startswith("-Winvalid-pch"):
            continue
        out.append(arg)
    if not out or shutil.which(out[0]) is None and not Path(out[0]).is_file():
        return None
    return out, entry["directory"]


def test_every_stage_passes_the_sims_validation(tmp_path):
    found = _command()
    if found is None:
        pytest.skip("no configured build to borrow compile flags from")
    flags, directory = found
    (tmp_path / "shim").mkdir()
    (tmp_path / "shim" / "Log.h").write_text(LOG_STUB)
    (tmp_path / "main.cpp").write_text(MAIN)
    includes = [f"-I{tmp_path / 'shim'}"] + [f"-I{root}" for root, _, _ in os.walk(REPO / "src")]
    binary = tmp_path / "stages"
    build = subprocess.run(flags[:1] + includes + flags[1:] + [str(STAGES_CPP), str(tmp_path / "main.cpp"), "-o",
                                                                 str(binary)],
                           cwd=directory, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr[-4000:]
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    left_out = [line for line in lines if line.startswith("left out")]
    assert not left_out, "stages the sim's validation leaves out:\n" + "\n".join(left_out)
    valid = [line.split(" ", 1)[1] for line in lines if line.startswith("valid ")]
    assert valid == MOVEMENT, f"the curriculum the sim builds: {valid}"
