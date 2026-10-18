#!/usr/bin/env bash
#
# Bring a cluster machine up to date: the code (this repository, the branch this checkout is on: forge, or forge-dev
# while the cluster runs the dev branch) from its origin, GitHub, then rebuild and restart the worldserver.
#
#   apps/forge/tools/cluster-pull.sh [remote]     default: origin (https://github.com/Moloch17/azerothcore-wotlk.git)
#
# First time on a new machine:
#   git clone -b forge-dev https://github.com/Moloch17/azerothcore-wotlk.git animus-forge && cd animus-forge
#   apps/forge/tools/cluster-pull.sh
# then copy the world data (maps, vmaps, mmaps, dbc) and import the database, set this machine's role in
# env/dist/etc/modules/mod_animus_forge.conf (AnimusForge.Cluster.Role/Host), and start it with ./forge.sh.
#
# Each machine's own configs (env/dist/etc/*.conf) are not in the repository, so a pull never touches them. The
# host refuses a worker whose code or curriculum settings differ from its own (the cluster
# fingerprint): a machine left un-pulled cannot join with stale code. Until 2026-10-18 the workers pulled from a bare
# repository on the dev machine over SSH (git/animus-forge.git); no large file goes through git any more, so GitHub.

set -euo pipefail
cd "$(dirname "$0")/../../.."

source="${1:-origin}"
branch="$(git rev-parse --abbrev-ref HEAD)"
echo "Code: pulling $branch from $source ($(git remote get-url "$source" 2>/dev/null || echo "$source"))..."
git pull --ff-only "$source" "$branch"

echo "Rebuilding and restarting the worldserver..."
mkdir -p env/dist
touch env/dist/.forge-build
docker compose up -d --force-recreate ac-worldserver
echo "Done. \`docker compose logs -f ac-worldserver\` shows the build; \`./forge.sh attach\` the console."
