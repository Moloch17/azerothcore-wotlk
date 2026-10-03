#!/usr/bin/env bash
#
# Command of the ac-worldserver service (docker-compose.yml): prepare the bind-mounted source tree on
# the first start, then run the forge sim in the foreground so its console is the container's terminal.
#
#   1. build and install the worldserver if env/dist/bin has none yet, or once when ./forge.sh --build asked for it
#   2. put back missing config files: any .conf from its .dist
#   3. install whatever the forge learner's Python venv is missing (animus-venv.sh)
#   4. start TensorBoard in the background, on http://localhost:${FORGE_TENSORBOARD_PORT:-16006}
#   5. exec the worldserver, which starts the learner itself (AnimusForge.Learner.AutoStart)

set -euo pipefail

ROOT=/azerothcore
BIN="$ROOT/env/dist/bin"
LOGS="$ROOT/env/dist/logs"
LEARNER="$ROOT/apps/forge/python"
VENV="$LEARNER/.venv"
# Left by ./forge.sh --build: compile before this start.
BUILD_REQUEST="$ROOT/env/dist/.forge-build"
# Optimised with line info (-O2 -g): a crash on a map thread comes with file and line in its backtrace, which a
# Release build merges into whatever was inlined. conf/dist/env.ac sets CTYPE=Release for every AzerothCore
# container, so the forge's own choice is FORGE_CTYPE (FORGE_CTYPE=Release builds the old way).
export CTYPE="${FORGE_CTYPE:-RelWithDebInfo}"

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

# The CPU a worldserver was built for: it is compiled with -march=native, so a tree copied to another machine (a
# cluster's machines are set up by copying the project) carries a binary that may use instructions this CPU does not
# have. The vendor, family, model and instruction set flags name the CPU; a different one here means building again.
BUILT_FOR="$ROOT/env/dist/.forge-build-cpu"
cpu_signature()
{
    grep -m4 -E "^(vendor_id|cpu family|model|flags)[[:space:]]*:" /proc/cpuinfo | md5sum | cut -d' ' -f1
}

if [[ ! -x "$BIN/worldserver" ]]; then
    echo "No worldserver in $BIN yet: building it (first start only, this takes a while)..."
    build_worldserver
elif [[ -f "$BUILD_REQUEST" ]]; then
    echo "./forge.sh --build: configuring and building the worldserver from the current source..."
    build_worldserver
elif [[ "$(cat "$BUILT_FOR" 2>/dev/null)" != "$(cpu_signature)" ]]; then
    echo "The worldserver in $BIN was built for another CPU (or before builds were stamped): building it for this one..."
    build_worldserver
fi
cpu_signature > "$BUILT_FOR"
# Only this start: a later restart runs what was built. (A failed build stops the script above and keeps the request.)
rm -f "$BUILD_REQUEST"

# Config files, every start: the module's template when the install dir has lost it, and any missing .conf from
# its .dist. An existing .conf is never overwritten.
CONF="$ROOT/env/dist/etc"
mkdir -p "$CONF/modules"
for dist in "$CONF"/*.conf.dist "$CONF"/modules/*.conf.dist; do
    [[ -e "$dist" ]] || continue
    [[ -f "${dist%.dist}" ]] || cp -v "$dist" "${dist%.dist}"
done

bash "$ROOT/apps/docker/animus-venv.sh"

if [[ -x "$VENV/bin/tensorboard" ]]; then
    # The runs of AnimusForge.OutputDir (set by docker-compose.yml), else the module's python/ directory. Earlier
    # runs are archived beside runs/ (<OutputDir>/archive), not inside it, so TensorBoard reads the live ones only.
    RUNS="${AC_ANIMUS_FORGE_OUTPUT_DIR:-$LEARNER}/runs"
    mkdir -p "$RUNS"
    # One port inside and outside the container, so the URL is the same in both networking modes: on the bridge
    # network docker-compose.yml publishes it to the host's loopback on the same number, and on host networking
    # (docker-compose.cluster.yml) this bind is the host's port. FORGE_LOCAL_ONLY keeps a host-network bind on
    # loopback rather than the LAN; a bridge bind has to take every interface for the published port to reach it.
    TENSORBOARD_PORT="${FORGE_TENSORBOARD_PORT:-16006}"
    if [[ -n "${FORGE_LOCAL_ONLY:-}" ]]; then
        tensorboard_bind=(--host 127.0.0.1)
    else
        tensorboard_bind=(--bind_all)
    fi
    "$VENV/bin/tensorboard" --logdir "$RUNS" "${tensorboard_bind[@]}" --port "$TENSORBOARD_PORT" \
        > "$LOGS/tensorboard.log" 2>&1 &
    echo "TensorBoard: http://localhost:$TENSORBOARD_PORT (logdir $RUNS)"
else
    echo "TensorBoard is not installed in $VENV; skipping it."
fi

cd "$BIN"
exec ./worldserver
