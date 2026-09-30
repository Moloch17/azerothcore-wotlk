#!/usr/bin/env bash
#
# Command of the ac-worldserver service (docker-compose.yml): prepare the bind-mounted source tree on
# the first start, then run the forge sim in the foreground so its console is the container's terminal.
#
#   1. build and install the worldserver if env/dist/bin has none yet, or once when ./forge.sh --build asked for it
#   2. put back missing config files: any .conf from its .dist
#   3. install whatever the forge learner's Python venv is missing (animus-venv.sh)
#   4. start TensorBoard in the background
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
# Release build (conf/dist/config.sh's default) merges into whatever was inlined. CTYPE=Release in the service's
# environment builds the old way.
export CTYPE="${CTYPE:-RelWithDebInfo}"

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
    # The runs of AnimusForge.OutputDir (set by docker-compose.yml), else the module's python/ directory.
    RUNS="${AC_ANIMUS_FORGE_OUTPUT_DIR:-$LEARNER}/runs"
    mkdir -p "$RUNS"
    # Every interface inside the container's own network (published to the host's loopback); on host networking
    # (docker-compose.cluster.yml) FORGE_LOCAL_ONLY keeps it on loopback rather than the LAN.
    if [[ -n "${FORGE_LOCAL_ONLY:-}" ]]; then
        tensorboard_bind=(--host 127.0.0.1)
    else
        tensorboard_bind=(--bind_all)
    fi
    "$VENV/bin/tensorboard" --logdir "$RUNS" "${tensorboard_bind[@]}" --port 6006 > "$LOGS/tensorboard.log" 2>&1 &
else
    echo "TensorBoard is not installed in $VENV; skipping it."
fi

# The forge dashboard (http://localhost:18800): the run's config and its live progress in one page. Standard library
# only, so it runs on the image's python rather than the learner's venv, and it reads the run files without touching
# the sim -- a crash or a restart of it costs nothing.
DASHBOARD="$ROOT/apps/forge/python/animus/dashboard.py"
# The page's stage controls (pause, resume, skip, cancel) go to the sim over SOAP and need an account to do it
# as. They exist only when this file does: `user:password` for an account with SEC_ADMINISTRATOR, alongside
# SOAP.Enabled in worldserver.conf. Absent -- which is the default -- the dashboard is read-only, exactly as it
# was before the controls existed. It is a file rather than an argument because argv is in every ps listing.
# The `+` expansion is what keeps `set -u` quiet when the array is empty, which is the normal case.
DASHBOARD_AUTH="$CONF/animus-dashboard.auth"
if [[ -f "$DASHBOARD" ]]; then
    dashboard_soap=()
    if [[ -r "$DASHBOARD_AUTH" ]]; then
        dashboard_soap=(--soap-auth "$DASHBOARD_AUTH")
    fi
    dashboard_host=0.0.0.0
    [[ -n "${FORGE_LOCAL_ONLY:-}" ]] && dashboard_host=127.0.0.1
    python3 "$DASHBOARD" --host "$dashboard_host" --port 8800 \
        --runs "${AC_ANIMUS_FORGE_OUTPUT_DIR:-$LEARNER}/runs" \
        --conf "$CONF/worldserver.conf" \
        ${dashboard_soap[@]+"${dashboard_soap[@]}"} > "$LOGS/dashboard.log" 2>&1 &
fi

cd "$BIN"
exec ./worldserver
