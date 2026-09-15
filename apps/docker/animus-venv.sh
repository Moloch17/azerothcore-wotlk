#!/usr/bin/env bash
#
# Make sure mod-animus-forge's Python venv has torch, the learner (with TensorBoard) and pytest. Idempotent: run
# on every start of ac-worldserver and ac-dev-server, it only installs what is missing. The venv lives on the bind
# mount (modules/mod-animus-forge/python/.venv), so both containers share it; its interpreter paths are container
# paths, so run it from a container:
#
#   docker compose exec -w /azerothcore/modules/mod-animus-forge/python ac-dev-server .venv/bin/python -m pytest
#
# ANIMUS_TORCH_INDEX_URL picks the torch wheel index (empty = PyPI).

set -euo pipefail

LEARNER=/azerothcore/modules/mod-animus-forge/python
VENV="$LEARNER/.venv"

if [[ ! -d "$LEARNER" ]]; then
    echo "WARNING: $LEARNER not found; the learner cannot start. Clone mod-animus-forge into modules/."
    exit 0
fi

if [[ ! -x "$VENV/bin/python" ]]; then
    echo "Creating the learner's Python environment in $VENV (first start only)..."
    python3 -m venv "$VENV"
fi

if ! "$VENV/bin/python" -c "import torch" 2>/dev/null; then
    echo "Installing torch into $VENV (this takes a while)..."
    if [[ -n "${ANIMUS_TORCH_INDEX_URL:-}" ]]; then
        "$VENV/bin/pip" install torch --index-url "$ANIMUS_TORCH_INDEX_URL"
    else
        "$VENV/bin/pip" install torch
    fi
fi

if ! "$VENV/bin/python" -c "import animus, pytest, tensorboard" 2>/dev/null; then
    echo "Installing the learner, TensorBoard and pytest into $VENV..."
    "$VENV/bin/pip" install -e "$LEARNER[tensorboard,dev]"
fi
