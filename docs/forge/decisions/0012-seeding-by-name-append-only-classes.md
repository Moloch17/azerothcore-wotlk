# 0012: Seeding is by name; classes only append

**Date:** 2026-10-06

**Decision.** A stage starts from its parent's `latest.pt`: columns and actions carry over by block and name. Classes only append to the class table; a change in a block's meaning bumps its revision. BlockIds are explicit and never renumbered.

**Reason.** Checkpoints, manifests and seeding all know a block by its id; deleting the first curriculum left gaps that stay (Block.h). By-name seeding landed in 302637cc3 (2026-10-06); `latest.pt` as the seed in dfa89105d (2026-10-07). The class-table contract is in the dungeon-curriculum plan.

**What it constrains.** Every live layout must keep its by-name seeding; check with `stage_json_diff.py` (the pin test was removed 2026-10-07; see [tests.md](../reference/tests.md)).

See also [principles.md](../principles.md) and the [index](README.md).
