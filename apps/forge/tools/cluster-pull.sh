#!/usr/bin/env bash
#
# Bring a cluster machine up to date with the dev machine, over SSH: the code (this repository, branch forge) and the
# baked probe data (apps/forge/probes, a repository of its own), then rebuild and restart the worldserver.
#
#   apps/forge/tools/cluster-pull.sh [user@host]     default: the host the code was cloned from
#
# First time on a new machine:
#   git clone -b forge moloch@192.168.0.69:git/animus-forge.git azerothcore && cd azerothcore
#   apps/forge/tools/cluster-pull.sh
# then copy the world data (maps, vmaps, mmaps, dbc) and import the database, set this machine's role in
# env/dist/etc/modules/mod_animus_forge.conf (AnimusForge.Cluster.Role/Host), and start it with ./forge.sh.
#
# Each machine's own configs (env/dist/etc/*.conf) are not in either repository, so a pull never touches them. The
# host refuses a worker whose code, probe data or curriculum settings differ from its own (the cluster
# fingerprint): a machine left un-pulled cannot join with stale code.

set -euo pipefail
cd "$(dirname "$0")/../../.."

source="${1:-}"
if [[ -z "$source" ]]; then
    # user@host of the clone's origin, e.g. moloch@192.168.0.69 from moloch@192.168.0.69:git/animus-forge.git
    source="$(git remote get-url origin | sed -n 's|^\([^:/]*@[^:]*\):.*|\1|p')"
fi
if [[ -z "$source" ]]; then
    echo "Which machine? apps/forge/tools/cluster-pull.sh user@host" >&2
    exit 1
fi

echo "Code: pulling forge from $source..."
git pull --ff-only "$source:git/animus-forge.git" forge

probes=apps/forge/probes
if [[ -d "$probes/.git" ]]; then
    echo "Probe data: pulling..."
    git -C "$probes" pull --ff-only "$source:git/animus-probes.git" main
else
    echo "Probe data: first copy..."
    if [[ -d "$probes" ]] && [[ -n "$(ls -A "$probes")" ]]; then
        mv "$probes" "$probes.before-clone"
        echo "  (the probe files that were here are in $probes.before-clone)"
    fi
    git clone "$source:git/animus-probes.git" "$probes"
fi

echo "Rebuilding and restarting the worldserver..."
mkdir -p env/dist
touch env/dist/.forge-build
docker compose up -d --force-recreate ac-worldserver
echo "Done. \`docker compose logs -f ac-worldserver\` shows the build; \`./forge.sh attach\` the console."
