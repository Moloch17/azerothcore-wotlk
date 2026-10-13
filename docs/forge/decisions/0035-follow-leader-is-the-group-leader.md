# 0035: The follow leader runs the stock path as the group's leader, seen at a player's report cadence

**Date:** 2026-10-13 (the move4-follow rework, `.agents/plans/move4-follow-rework/`; the owner's decisions). A fresh
start: the party frames' leader flag changes meaning (the group's leader, not slot 0) and the leader's learner row is
gone.

**Why.** The owner named three flaws in move4_follow: the leader walked (rung 0 was "the leader always walks"), it
was not the party's leader (a hand-filled frame in slot 0, not what a follower reads on a realm), and it was a
controller-driven seat (stuck on rock lips the stock path climbs; a frozen-checkpoint leader was planned). The run
also could not advance: kept 0.69 at best against a gate of 0.75 on a bimodal measure. Research found settled_share
reading 1.0 from a bug (speed read from Body.Vx/Vy, which the ground step zeroes), walls undifferentiated and barely
charged, no measure of line of sight, and an anneal warning that fired when no anneal could happen.

**Change.**
- **Always run.** `WalkRungs`, `WalkChance` and the walk toggle are gone. Rung 0 is forward hops with long stops.
- **The group's leader.** The encounter forms a sim Group led by the leader with the followers as members
  (`PartyFollowEncounter::FormGroup`); the frames come from `PartyFramesBlock::FillFromGroup` as on a realm, and the
  hand-filled frames are gone. The leader has no learner row (`_castOwner` false; `CastShare` gone).
- **Moved along the stock path, not by the controller.** The way is the PathGenerator's chain (unchanged), kept to the
  steps a player's controller can walk: a step that climbs 4 yd or more, or more than tan 50 degrees of its run plus a
  1 yd step, is walked downhill only (`MakeWay`); no Seek steering, no unstick, no give-ups. The leader's true pose
  moves every update at its run speed along the hop's corners (`Walk`).
- **Seen at a player's cadence (the owner's decision).** The server's copy of the leader (its Player's position,
  facing and moving flag) is set only when a client would have sent a movement packet (`Report`, the rules of
  `Movement::Cadence`: at once on a start, a stop or a 0.1 rad turn, else every 500 ms while moving, and once at the
  episode's start). Every reader -- frames, dots, recall, the camera's entity list, the rewards -- reads that copy,
  with no per-reader override. **Deviation from the plan's letter:** the plan said `MotionMaster::MovePoint`; a
  spline relocates the Player every tick, so every reader would need its own reported-state override and the entity
  list could not have one. Relocating the Player at report cadence gives exact parity with a real player in one
  place. `leader_report_age_ms` reads 0-500 ms while moving.
- **Rising gates** (B1): `fade.gate_values [0.65, 0.70, 0.75]` on the eval mean follow_kept_share, one per step.
- **settled_share** (B2): the speed is the distance moved over the decision.
- **Walls** (B3): `wall_seconds` / `steep_seconds` / `edge_seconds` from the controller's AgainstWall, SteepSlope and
  OverVoid flags (every stage); the follow stage escalates Wall as the seek stage does (Seek.WallEscalateSeconds/Max).
- **Leader realism** (B4): speed bursts (`BurstFromRung` 1, `BurstChance` 25, x1.3-1.4 for 3-8 s); reverse hops from
  rung 1; jumps dropped.
- **Line of sight** (B5): `leader_out_of_sight_seconds` (not in the camera's frame and no line of sight).
- **Throughput** (B6): eli out of the cluster; spencer and the ProLiant get fewer follow envs (their confs).
- **Convergence** (B7): progress.json carries `anneal_allowed`; forge status warns of an anneal only when it is set.

**Judge it by** the ladder stepping (rung 0 at 0.65), settled_share below 1 and varying, leader_report_age_ms in
0-500 ms, leader_route_share near 1 (no give-ups), the wall split, and leader_out_of_sight_seconds above 0.

**Revert:** the previous leader is in git history (forge-dev before this change); the keys `BurstChance` 0 and
`ReverseChance` 0 give a forward-only runner.
