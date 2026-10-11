#!/usr/bin/env bash
#
# The forge route viewer without a worldserver: the bots' evaluation routes on each map's baked scene, for looking at
# runs offline (a stopped cluster, a copied run folder, a laptop).
#
#   ./forge-viz.sh [--open] [--lan] [--port N] [--runs DIR] [--scenes DIR]
#
#   --open      open it in the browser once it is up
#   --lan       listen on every interface, so other machines can open it (default: this machine only)
#   --port      default 8770, the port the worldserver's own viewer uses; when that one is already serving here,
#               this says so and opens it instead of starting a second
#   --runs      the run folders (default var/animus-forge/shared/runs)
#   --scenes    the baked scenes (default env/dist/etc/modules/animus/scenes)
#
# It needs Python 3 with numpy and Pillow (the learner's venv has both, or: pip install numpy pillow). It reads the
# runs and writes nothing but runs/<run>/viz/. Ctrl-C stops it.

set -euo pipefail
cd "$(dirname "$0")"

port=8770
host=127.0.0.1
open=""
extra=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --open) open=1; shift ;;
        --lan) host=0.0.0.0; shift ;;
        --port) port="$2"; shift 2 ;;
        --runs|--scenes) extra+=("$1" "$2"); shift 2 ;;
        -h|--help) sed -n '3,16p' "$0"; exit 0 ;;
        *) echo "Unknown option $1 (--help lists them)" >&2; exit 2 ;;
    esac
done

url="http://localhost:$port/"
open_browser()
{
    [[ -n "$open" ]] || return 0
    command -v xdg-open > /dev/null && xdg-open "$url" > /dev/null 2>&1 &
}

if curl -fs -o /dev/null --max-time 2 "${url}api/runs" 2>/dev/null; then
    echo "A route viewer is already serving on $url (the worldserver's, or another ./forge-viz.sh)."
    open_browser
    exit 0
fi

python=""
for candidate in python3 apps/forge/python/.venv/bin/python; do
    if command -v "$candidate" > /dev/null && "$candidate" -c "import numpy, PIL" 2> /dev/null; then
        python="$candidate"
        break
    fi
done
if [[ -z "$python" ]]; then
    echo "No Python here has numpy and Pillow: pip install numpy pillow (or use the learner's venv)." >&2
    exit 1
fi

echo "Route viewer: $url  (Ctrl-C stops it)"
(sleep 1; open_browser) &
exec "$python" apps/forge/tools/viz/server.py --host "$host" --port "$port" "${extra[@]}"
