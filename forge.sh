#!/usr/bin/env bash
#
# Start Animus Forge training and attach this terminal to the worldserver console.
#
#   ./forge.sh              start everything (building what is missing) and attach to the console
#   ./forge.sh --build      the same, but configure CMake and compile the worldserver from the current source first
#   ./forge.sh attach       attach to the console of a server that is already running
#   ./forge.sh dev          also start the dev container (for VS Code or `docker compose exec`)
#   ./forge.sh stop         stop training (the learner saves a checkpoint first)
#
# `docker compose up` alone only streams logs: Compose does not forward the keyboard to a container,
# so typing console commands needs `docker compose attach`, which this script runs for you.
#
# Without --build the server runs the worldserver already in env/dist/bin, however old; `docker compose up
# --build` only rebuilds the Docker images, never the worldserver. --build recreates the worldserver container
# (stopping a running one; the learner saves first), runs a CMake configure (so added or removed source files are
# picked up) and compiles incrementally before it starts. A failed configure or compile stops the start.
#
# --build waits for the build: it returns 0 once the new worldserver is installed and starting, or 1 with the
# compiler's errors when the build failed. It attaches only when run from a terminal, so a script can run it and go on.

set -euo pipefail

cd "$(dirname "$0")"

CONTAINER=ac-animus-forge-worldserver
# apps/docker/forge-worldserver.sh builds when it finds this, and removes it once the build is installed.
BUILD_REQUEST=env/dist/.forge-build

usage()
{
    sed -n '3,20p' "$0"
    exit 1
}

attach()
{
    if [[ ! -t 0 || ! -t 1 ]]; then
        echo "Not a terminal: not attaching. Attach later with ./forge.sh attach."
        exit 0
    fi
    echo "Attaching to the worldserver console. Detach with Ctrl+P Ctrl+Q; Ctrl+C stops the server."
    exec docker compose attach ac-worldserver
}

# Until the container has removed the build request (built and installed) or stopped (the build failed: the request
# stays, so the next ./forge.sh --build tries again). Prints the build's progress line now and then.
wait_for_build()
{
    local status percent last=""
    echo "Building the worldserver; waiting for it (this returns when it is installed or has failed)..."
    while [[ -f $BUILD_REQUEST ]]; do
        status=$(docker inspect -f '{{.State.Status}}' "$CONTAINER" 2>/dev/null || echo missing)
        if [[ $status != running ]]; then
            echo "The build failed (the worldserver container is $status). The errors:"
            docker logs "$CONTAINER" 2>&1 | grep -E 'error:|Error [0-9]' | head -30 || true
            exit 1
        fi
        percent=$(docker logs --tail 40 "$CONTAINER" 2>&1 | grep -oE '^\[ *[0-9]+%\]' | tail -1 || true)
        if [[ -n $percent && $percent != "$last" ]]; then
            echo "  $percent"
            last=$percent
        fi
        sleep 10
    done
    echo "Built and installed: $(stat -c %y env/dist/bin/worldserver)"
}

command=""
build=0
for arg in "$@"; do
    case "$arg" in
        --build) build=1 ;;
        -*) usage ;;
        *) [[ -z "$command" ]] && command="$arg" || usage ;;
    esac
done
command="${command:-up}"

if [[ $build == 1 && $command != up ]]; then
    echo "--build goes with starting the server: ./forge.sh --build"
    exit 1
fi

case "$command" in
    up)
        if [[ $build == 1 ]]; then
            mkdir -p env/dist
            touch "$BUILD_REQUEST"
            docker compose up -d --force-recreate ac-worldserver
            wait_for_build
        else
            docker compose up -d
        fi
        attach
        ;;
    attach)
        attach
        ;;
    dev)
        docker compose --profile dev up -d ac-dev-server
        ;;
    stop)
        docker compose stop ac-worldserver
        ;;
    *)
        usage
        ;;
esac
