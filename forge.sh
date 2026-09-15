#!/usr/bin/env bash
#
# Start Animus Forge training and attach this terminal to the worldserver console.
#
#   ./forge.sh              start everything (building what is missing) and attach to the console
#   ./forge.sh attach       attach to the console of a server that is already running
#   ./forge.sh dev          also start the dev container (for VS Code or `docker compose exec`)
#   ./forge.sh stop         stop training (the learner saves a checkpoint first)
#
# `docker compose up` alone only streams logs: Compose does not forward the keyboard to a container,
# so typing console commands needs `docker compose attach`, which this script runs for you.

set -euo pipefail

cd "$(dirname "$0")"

attach()
{
    echo "Attaching to the worldserver console. Detach with Ctrl+P Ctrl+Q; Ctrl+C stops the server."
    exec docker compose attach ac-worldserver
}

case "${1:-up}" in
    up)
        docker compose up -d
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
        sed -n '3,11p' "$0"
        exit 1
        ;;
esac
