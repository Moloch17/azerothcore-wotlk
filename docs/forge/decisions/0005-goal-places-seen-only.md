# 0005: Goal places in dungeons are seen-only by default

**Date:** 2026-10-07

**Decision.** A sight stage's goal places in a dungeon are what the seat discovered: the hostiles its entity memory holds (last seen place, alive until seen dead), its mental map's frontier and its leader. Never a live pack's or boss's position, never the route order. The dungeon map's layout nodes are off (they were `StageDefinition::GoalPlaces = SeenOnly`; conf `Curriculum.Stage.<name>.GoalPlaces`, 0 added the layout nodes).

**Reason.** The owner, 2026-10-07: only let the bots know what they have discovered (commits 8bae0f96d, 099107507). Today's layout nodes follow the boss route, so they leak its shape.

**What it constrains.** The layout source is gone (see the update below); the goal places are seen-only for good.

**Update 2026-10-08 ([0019](0019-vision-only-movement.md)).** The layout source (`SeenPlaces::Layout`, `SeenAndLayout`,
`StageDefinition::GoalPlaces` and its conf key) is deleted with the dungeon route it was cut from, and the leader's place
is clamped to the minimap's range (`PartyFollow.MinimapYards`).

See also [principles.md](../principles.md) and the [index](README.md).
