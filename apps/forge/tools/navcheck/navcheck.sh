#!/usr/bin/env bash
# navcheck.sh <map id> [pairs]: build navcheck in a throwaway dev container and run it on the host's bake of that map
# against the worldserver's own mmaps (both copied out of ac-animus-forge-worldserver). Read-only.
set -euo pipefail
map=$(printf "%03d" "$1")
pairs=${2:-300}
root=$(cd "$(dirname "$0")/../../../.." && pwd)
work="$root/var/navcheck-$$"
mkdir -p "$work/mmaps"
trap 'rm -rf "$work"' EXIT
container=ac-animus-forge-worldserver
trav=$(docker exec "$container" sh -c "ls /azerothcore/env/dist/etc/modules/animus/trav/ | grep '^$((10#$map))-' | head -1")
docker cp "$container:/azerothcore/env/dist/etc/modules/animus/trav/$trav" "$work/map.trav"
for f in $(docker exec "$container" sh -c "ls /azerothcore/env/dist/data/mmaps | grep '^$map'"); do
    docker cp "$container:/azerothcore/env/dist/data/mmaps/$f" "$work/mmaps/" >/dev/null
done
cp "$root/apps/forge/tools/navcheck/navcheck.cpp" "$work/"
docker run --rm -v "$root:/azerothcore" --entrypoint bash ac-animus-forge-dev-server:latest -c "
    cd /azerothcore/var/navcheck-$$ &&
    clang++ -O2 -std=c++17 -I/azerothcore/deps/recastnavigation/Detour/Include navcheck.cpp \
        /azerothcore/deps/recastnavigation/Detour/Source/*.cpp -o navcheck &&
    ./navcheck map.trav mmaps $pairs"
