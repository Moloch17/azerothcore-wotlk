# 0005: Goal places in dungeons are seen-only by default

**Date:** 2026-10-07

**Decision.** A sight stage's goal places in a dungeon are what the seat discovered: the hostiles its entity memory holds (last seen place, alive until seen dead), its mental map's frontier and its leader. Never a live pack's or boss's position, never the route order. The dungeon map's layout nodes are off (`StageDefinition::GoalPlaces = SeenOnly`; conf `Curriculum.Stage.<name>.GoalPlaces`, 0 adds the layout nodes).

**Reason.** The owner, 2026-10-07: only let the bots know what they have discovered (commits 8bae0f96d, 099107507). Today's layout nodes follow the boss route, so they leak its shape.

**What it constrains.** Do not set GoalPlaces = 0 until the layout is sampled from the whole instance's walkable area (worldserver.conf.dist note). Pinned by EntityMemoryTest.cpp and GoalObjectiveLeakTest.cpp.

See also [principles.md](../principles.md) and the [index](README.md).
