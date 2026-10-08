# 0017: The "human" stand-in is a frozen learned partner

**Date:** 2026-10-07

**Decision.** The stand-in is a frozen policy from the learner's co-op partner pool in one seat of a share of party training runs (`StandIn.Share`; arenas of the dungeon stages set 20%). It is never trained on and is absent while the pool is empty. The `with_human` evaluation arm and `standin_gap` measure play beside it.

**Reason.** Follows 0002: the scripted stand-in was deleted with the other scripts (a355df8ff).

**What it constrains.** It leads or follows in the role it wants (`Encounters/StandIn.h`). The target is stand-in party clears within about 10 points of the all-bot party's.

See also [principles.md](../principles.md) and the [index](README.md).
