# 0006: Seats move only through the player controller

**Date:** 2026-10-05

**Decision.** Held keys and turn rates, reported to the server as a client would. No splines, teleports or navmesh moves for a bot (commit 5aef8b83e, player-controller C9). The controller's facing rule and heartbeat run once a world tick, so a finer tick (50 ms, 5 ticks a 250 ms decision) keeps the server's view of a seat's facing closer to a client's. See the update of 2026-10-09 for what the live curriculum actually trains at.

**Reason.** A bot must move the way a player can, because the models are meant to play beside a human on a real realm.

**What it constrains.** Navmesh data may author tables offline or, through the stock `PathGenerator`, drive the scripted leader's keys (M4) but is never a bot input. `AnimusForge.Stage.<name>.TicksPerDecision` exists to give one stage a finer tick (worldserver.conf.dist).

**Update 2026-10-08 ([0019](0019-vision-only-movement.md)).** Movement is vision-only: no route, waypoint or path hint
reaches the policy, and the forge's own navigation layers (private navmesh queries, route planners, route-ordered
rewards) are deleted. Stock `PathGenerator` (upstream's, no forge diff) is allowed for reward terms and reset-time
setup, and for the M4 leader's legs, nothing beyond.

**Update 2026-10-09.** The live curriculum trains at `TicksPerDecision` 1 (a 250 ms world tick under 250 ms decisions)
with `HalfBatch` 1, on every machine, and no stage sets `AnimusForge.Stage.<name>.TicksPerDecision`. M1 and M2 were
trained that way; changing the tick mid-curriculum would change the timing the seeded policy has already learned (press
timing, facing, the follow's gaps), so M3, M4, C1 and everything after keep 1 rather than the 5 first written here. The
per-stage key stays for a stage that needs a finer world and a restart between stages; the template no longer sets it.

See also [principles.md](../principles.md) and the [index](README.md).
