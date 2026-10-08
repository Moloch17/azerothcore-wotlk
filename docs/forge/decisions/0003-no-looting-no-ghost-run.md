# 0003: No looting, and no graveyard, ghost or corpse run

**Date:** 2026-10-06

**Decision.** No press loots. A dead seat is out for `Respawn.DelayMs` (10 s), then alive at full health and power at the dungeon entrance, and walks back to the party on the controller. The episode never ends on a death.

**Reason.** The owner's word (dungeon-curriculum plan, "Left out (the user, 2026-10-06)"). A ghost cannot move under the controller-only rule and an instance death was a scripted teleport; the rejoin has to be the seat's own walk.

**What it constrains.** Every stage with deaths sets `ArenaDefinition::RespawnAtEntrance` and uses `EntranceRespawn`. A death is priced as Away (time dead or walking back) and Death; coming back is never rewarded, which would pay dying.

See also [principles.md](../principles.md) and the [index](README.md).
