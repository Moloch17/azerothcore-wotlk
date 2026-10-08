# 0015: Sarah is the cluster host; the dev machine is not in the cluster

**Date:** 2026-10-07

**Decision.** The host role moved from the dev machine to sarah by hand on 2026-10-07 (cluster.md). Workers: spencer, thomas and the .117 machine (moloch).

**Reason.** UNVERIFIED: the owner's stated reason. Known: the dev machine holds the lan git repo and does the building and testing; sarah has 24 threads and 221 GB free (cluster.md).

**What it constrains.** `apps/forge/cluster.toml` lists the machines; `forgectl cluster move-host` moves the role.

See also [principles.md](../principles.md) and the [index](README.md).
