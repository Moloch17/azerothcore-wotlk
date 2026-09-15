#!/usr/bin/env bash
#
# Command of the ac-worldserver service (docker-compose.yml): prepare the bind-mounted source tree on
# the first start, then run the forge sim in the foreground so its console is the container's terminal.
#
#   1. build and install the worldserver if env/dist/bin has none yet
#   2. create mod-animus-forge's Python venv (torch, the learner, TensorBoard) if it is missing
#   3. start TensorBoard in the background
#   4. exec the worldserver, which starts the learner itself (AnimusForge.Learner.AutoStart)

set -euo pipefail

ROOT=/azerothcore
BIN="$ROOT/env/dist/bin"
LOGS="$ROOT/env/dist/logs"
LEARNER="$ROOT/modules/mod-animus-forge/python"
VENV="$LEARNER/.venv"

mkdir -p "$LOGS"

if [[ ! -x "$BIN/worldserver" ]]; then
    echo "No worldserver in $BIN yet: building it (first start only, this takes a while)..."
    "$ROOT/acore.sh" compiler build
fi

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
    "$VENV/bin/tensorboard" --logdir "$LEARNER/runs" --bind_all --port 6006 > "$LOGS/tensorboard.log" 2>&1 &
else
    echo "TensorBoard is not installed in $VENV; skipping it."
fi

cd "$BIN"
exec ./worldserver
