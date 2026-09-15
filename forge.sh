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

set -euo pipefail

cd "$(dirname "$0")"

usage()
{
    sed -n '3,17p' "$0"
    exit 1
}

attach()
{
    echo "Attaching to the worldserver console. Detach with Ctrl+P Ctrl+Q; Ctrl+C stops the server."
    exec docker compose attach ac-worldserver
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
            # apps/docker/forge-worldserver.sh builds when it finds this, then removes it.
            mkdir -p env/dist
            touch env/dist/.forge-build
            docker compose up -d --force-recreate ac-worldserver
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
