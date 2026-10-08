# 0014: Build the whole approved plan, then rebuild the cluster once

**Date:** 2026-10-06

**Decision.** Implement the full plan first; then one rebuild and deploy. A resume follows a rebuild, never a restart from scratch.

**Reason.** Every machine compiles from source with -march=native and the fingerprint forces all to match, so a rebuild is slow (20 to 60 minutes a machine). The owner's standing note "build the whole plan, then rebuild"; the human-operable plan batches all C++ changes into one rebuild. UNVERIFIED: exact date.

**What it constrains.** deploy-gate.md is the ordered procedure. Do not deploy piecemeal.

See also [principles.md](../principles.md) and the [index](README.md).
