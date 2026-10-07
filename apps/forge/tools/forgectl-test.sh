#!/bin/bash
# The forge's tests in a throwaway build tree, run INSIDE the dev container (`forgectl test` starts it with
# `docker exec`). Ported from the gitignored var/staging_test.sh.
#
#   forgectl-test.sh --src SRC --build BUILD --python PYTHON [--gpu] [--jobs N]
#
#   SRC     the source tree as the container sees it (a worktree is a subdirectory of the mounted checkout)
#   BUILD   the cmake build directory (created on the first run; kept so the next run is incremental)
#   PYTHON  the venv python that has torch and pytest
#
# It prints progress lines "STEP ..." and, at the end, machine-readable lines for forgectl to summarise:
#   UNIT_EXIT <n>, UNIT_LINE <gtest summary line>, UNIT_FAILED <test>, PYTEST_EXIT <n>, PYTEST_LINE <summary>,
#   PYTEST_FAILED <test>, and FATAL <why> when the build could not get as far as running tests.
# Logs stay next to the build directory: BUILD.configure.log, .build.log, .link.log, .unit.log, .pytest.log.
set -u

SRC="" B="" PY="" GPU=0 JOBS=16
while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC="$2"; shift 2 ;;
    --build) B="$2"; shift 2 ;;
    --python) PY="$2"; shift 2 ;;
    --gpu) GPU=1; shift ;;
    --jobs) JOBS="$2"; shift 2 ;;
    *) echo "FATAL unknown argument $1"; exit 2 ;;
  esac
done
if [ -z "$SRC" ] || [ -z "$B" ] || [ -z "$PY" ]; then echo "FATAL --src, --build and --python are required"; exit 2; fi
mkdir -p "$(dirname "$B")"

if [ ! -f "$B/CMakeCache.txt" ]; then
  echo "STEP configure (first time in $B)"
  cmake -S "$SRC" -B "$B" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DMODULES=static -DSCRIPTS=static -DAPPS_BUILD=all -DTOOLS_BUILD=none \
    -DUSE_COREPCH=OFF -DUSE_SCRIPTPCH=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "$B.configure.log" 2>&1 \
    || { echo "FATAL configure failed, see $B.configure.log"; tail -5 "$B.configure.log"; exit 1; }
else
  echo "STEP re-configure"
  cmake -S "$SRC" -B "$B" > "$B.configure.log" 2>&1
fi

echo "STEP build unit_tests (-j$JOBS)"
nice -n 15 cmake --build "$B" --target unit_tests -j"$JOBS" > "$B.build.log" 2>&1
build_rc=$?
grep -E " error:" "$B.build.log" | head -10 | sed 's/^/BUILD_ERROR /'
[ $build_rc -eq 0 ] || { echo "BUILD_FAILED $build_rc (see $B.build.log)"; exit 1; }

# The container's clang is version 18 and its resource directory has no compiler-rt libraries (libclang_rt.profile,
# which the instrumented link needs); llvm-17's does. So the unit_tests link line is run again with
# -resource-dir pointing at llvm-17.
echo "STEP relink unit_tests against the llvm-17 profile runtime"
cd "$B/src/test" || { echo "FATAL no $B/src/test"; exit 1; }
link=$(cat CMakeFiles/unit_tests.dir/link.txt)
link=${link/\/usr\/bin\/clang++/\/usr\/bin\/clang++ -resource-dir=\/usr\/lib\/llvm-17\/lib\/clang\/17}
eval "$link" > "$B.link.log" 2>&1 || { echo "FATAL link failed"; tail -5 "$B.link.log"; exit 1; }

echo "STEP run the GTests"
./unit_tests > "$B.unit.log" 2>&1
echo "UNIT_EXIT $?"
grep -aE "^\[==========\] [0-9]+ tests? from .* ran|^\[  PASSED  \]|^\[  SKIPPED \] [0-9]+ test|^\[  FAILED  \] [0-9]+ test" \
  "$B.unit.log" | sed 's/^/UNIT_LINE /'
grep -aE "^\[  FAILED  \] [A-Za-z0-9_./]+" "$B.unit.log" | grep -avE "^\[  FAILED  \] [0-9]+ test" | sort -u | head -50 \
  | sed 's/^\[  FAILED  \] //; s/ (.*//; s/^/UNIT_FAILED /'

echo "STEP run the CPU pytest"
if [ "$GPU" = 1 ]; then unset HIP_VISIBLE_DEVICES; else export HIP_VISIBLE_DEVICES=""; fi
cd "$SRC/apps/forge/python" && "$PY" -m pytest -q tests/ -p no:cacheprovider > "$B.pytest.log" 2>&1
echo "PYTEST_EXIT $?"
tail -1 "$B.pytest.log" | sed 's/^/PYTEST_LINE /'
grep -aE "^(FAILED|ERROR) " "$B.pytest.log" | head -50 | sed 's/ - .*//; s/^[A-Z]* //; s/^/PYTEST_FAILED /'
echo "STEP done; logs: $B.build.log $B.unit.log $B.pytest.log"
