# 0016: eli is out of the cluster

**Date:** 2026-10-06

**Decision.** eli (4 cores, the slowest) is not in the cluster; its container is stopped. Builds go to the host plus the workers.

**Reason.** The dungeon-curriculum plan says builds go to the host and 4 workers, "eli excluded until the user says" (2026-10-06). UNVERIFIED: any reason beyond speed.

**What it constrains.** Re-adding eli means `in_cluster = true` in cluster.toml (human-operable plan, acceptance step 1).

See also [principles.md](../principles.md) and the [index](README.md).
