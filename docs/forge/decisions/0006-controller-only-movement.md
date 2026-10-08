# 0006: Seats move only through the player controller

**Date:** 2026-10-05

**Decision.** Held keys and turn rates, reported to the server as a client would. No splines, teleports or navmesh moves for a bot (commit 5aef8b83e, player-controller C9). Every live stage runs 50 ms world ticks so the controller's facing rule and heartbeat are checked as often as a client's.

**Reason.** A bot must move the way a player can, because the models are meant to play beside a human on a real realm.

**What it constrains.** Navmesh data may author tables offline or drive a scripted leader's keys (M4) but is never a bot input. `AnimusForge.Stage.<name>.TicksPerDecision = 5` for every live stage (worldserver.conf.dist; test_stage_ticks.py).

See also [principles.md](../principles.md) and the [index](README.md).
