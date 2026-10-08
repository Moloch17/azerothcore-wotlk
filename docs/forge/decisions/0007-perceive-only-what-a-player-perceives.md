# 0007: A bot perceives only what a player perceives

**Date:** 2026-10-06

**Decision.** The policy sees the camera, the entities it has seen, its mental map and entity memory, and the UI a client shows (party frames, target frame, minimap). Nothing behind walls, no server lists. The critic may see privileged state. Actions on objects go through the same client packets a player sends.

**Reason.** Same as 0006: the models play on a real realm. The dungeon-curriculum plan (2026-10-06) lists the "nearest" picks and server-list targeting this replaced (infrastructure I1-I3). UNVERIFIED: the earliest date of the principle (perception-goals predates the dungeon plan).

**What it constrains.** Blocks are built from `SeatView`, not world lookups. Stage validation refuses a sight block without the vision and entities blocks before it (`Stages.cpp`, `Problem`).

See also [principles.md](../principles.md) and the [index](README.md).
