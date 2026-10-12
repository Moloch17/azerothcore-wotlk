# 0032: A glance into a room pays again below the room rung

**Date:** 2026-10-12 (owner: "Change it"). Resumed, not restarted.

**Why.** The fresh third-person run (0031) stalled on the doorway rung in the Stockades: found ~20% from 60M to
100M while Ragefire rose 24% -> 48%. At 100.3M, 110 of the 122 Stockade misses never saw the object; they ran ~750
yd down the corridors past the cells. 0029 made RoomSeen pay only once 60% of a room's floor was seen, so on the
hallway and doorway rungs -- where the Exploring family is off (ExploreFromRung 2) -- nothing paid the look into a
cell's door that the doorway rung is about. The Stockades' cells open side-on from the corridor; Ragefire's caves
are open.

**Change.** `Seek.RoomSeenShareFromRung` 2: the coverage rule from the room rung up, as 0029 meant it. Below it a room
is seen on a glance: `Seek.RoomGlanceRays` 3 rays of one frame on its floor (0028's RoomSeenRays 12, in every conf,
stays the old rule's knob and is unused), paid `Seek.RoomSeenGlance` 0.1 (RoomSeen's price before 0029). RoomSeen
stays category Exploring.

**Judge it by** the Stockades' found on the doorway rung, and the share of its misses that ever saw the object
(12 of 122 at 100.3M).
