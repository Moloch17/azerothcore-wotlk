#!/usr/bin/env bash
#
# Command of the ac-worldserver service (docker-compose.yml): prepare the bind-mounted source tree on
# the first start, then run the forge sim in the foreground so its console is the container's terminal.
#
#   1. build and install the worldserver if env/dist/bin has none yet, or once when ./forge.sh --build asked for it
#   2. put back missing config files: mod-animus-forge's template, and any .conf from its .dist
#   3. create mod-animus-forge's Python venv (torch, the learner, TensorBoard) if it is missing
#   4. start TensorBoard in the background
#   5. exec the worldserver, which starts the learner itself (AnimusForge.Learner.AutoStart)

set -euo pipefail

ROOT=/azerothcore
BIN="$ROOT/env/dist/bin"
LOGS="$ROOT/env/dist/logs"
LEARNER="$ROOT/modules/mod-animus-forge/python"
VENV="$LEARNER/.venv"
# Left by ./forge.sh --build: compile before this start.
BUILD_REQUEST="$ROOT/env/dist/.forge-build"

mkdir -p "$LOGS"

# Every build configures first: CMake collects sources with file(GLOB) at configure time, so a source file added to
# the core or a module (animus-lib's new blocks and encounters) is only compiled after a configure.
build_worldserver()
{
    echo "Configuring CMake (picks up added and removed source files)..."
    "$ROOT/acore.sh" compiler configure
    echo "Compiling and installing the worldserver (incremental)..."
    "$ROOT/acore.sh" compiler compile
}

if [[ ! -x "$BIN/worldserver" ]]; then
    echo "No worldserver in $BIN yet: building it (first start only, this takes a while)..."
    build_worldserver
elif [[ -f "$BUILD_REQUEST" ]]; then
    echo "./forge.sh --build: configuring and building the worldserver from the current source..."
    build_worldserver
fi
# Only this start: a later restart runs what was built. (A failed build stops the script above and keeps the request.)
rm -f "$BUILD_REQUEST"

# Config files, every start: the module's template when the install dir has lost it, and any missing .conf from
# its .dist. An existing .conf is never overwritten.
CONF="$ROOT/env/dist/etc"
mkdir -p "$CONF/modules"
cp -n "$ROOT/modules/mod-animus-forge/conf/"*.conf.dist "$CONF/modules/" 2>/dev/null || true
for dist in "$CONF"/*.conf.dist "$CONF"/modules/*.conf.dist; do
    [[ -e "$dist" ]] || continue
    [[ -f "${dist%.dist}" ]] || cp -v "$dist" "${dist%.dist}"
done

if [[ ! -d "$LEARNER" ]]; then
    echo "WARNING: $LEARNER not found; the learner cannot start. Clone mod-animus-forge into modules/."
elif [[ ! -x "$VENV/bin/python" ]]; then
    echo "Creating the learner's Python environment in $VENV (first start only)..."
    python3 -m venv "$VENV"
    if [[ -n "${ANIMUS_TORCH_INDEX_URL:-}" ]]; then
        "$VENV/bin/pip" install torch --index-url "$ANIMUS_TORCH_INDEX_URL"
    else
        "$VENV/bin/pip" install torch
    fi
    "$VENV/bin/pip" install -e "$LEARNER[tensorboard]"
fi

if [[ -x "$VENV/bin/tensorboard" ]]; then
    # The runs of AnimusForge.OutputDir (set by docker-compose.yml), else the module's python/ directory.
    RUNS="${AC_ANIMUS_FORGE_OUTPUT_DIR:-$LEARNER}/runs"
    mkdir -p "$RUNS"
    "$VENV/bin/tensorboard" --logdir "$RUNS" --bind_all --port 6006 > "$LOGS/tensorboard.log" 2>&1 &
else
    echo "TensorBoard is not installed in $VENV; skipping it."
fi

cd "$BIN"
exec ./worldserver
