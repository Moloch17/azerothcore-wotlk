"""Build golden.cpp against an MlpPolicy and run it on prep.py's inputs.

    python3 run.py <build dir> [Model source dir]

The source dir defaults to the module as built (modules/mod-animus/animus-lib/src/runtime/Model); the compile flags
are the module's, from var/syntax-build-animus/compile_commands.json. Run inside the dev container."""
import json
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
work = Path(sys.argv[1])
model_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "modules/mod-animus/animus-lib/src/runtime/Model"
# A worktree has no build tree of its own: the main checkout's (mounted at /azerothcore) has the same flags.
database = next(path for path in (ROOT / "var/syntax-build-animus/compile_commands.json",
                                  Path("/azerothcore/var/syntax-build-animus/compile_commands.json")) if path.is_file())
commands = json.loads(database.read_text())
entry = next(command for command in commands if command["file"].endswith("Model/MlpPolicy.cpp"))
flags = [flag for flag in shlex.split(entry["command"])[1:] if flag.startswith(("-D", "-I", "-std"))]
binary = work / "golden"
build = ["c++", "-O2", f"-I{model_dir}", *flags, str(model_dir / "MlpPolicy.cpp"), str(HERE / "golden.cpp"),
         str(ROOT / "deps/fmt/src/format.cc"), "-o", str(binary)]
result = subprocess.run(build, capture_output=True, text=True)
if result.returncode:
    sys.exit(f"build failed:\n{result.stderr[-4000:]}")
golden = ROOT / "apps/forge/python/tests/golden"
tolerance = (work / "tolerance.txt").read_text().strip()
sys.exit(subprocess.run([str(binary), str(golden / "seat_sets.amdl"), str(work / "cases.txt"),
                         str(golden / "seat_sets_recurrent.amdl"), str(work / "recurrent.txt"),
                         str(work / "v7.amdl"), str(work / "v9.amdl"), tolerance]).returncode)
