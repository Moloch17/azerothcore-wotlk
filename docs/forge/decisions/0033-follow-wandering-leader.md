# 0033: The follow stage's leader wanders, followers wait with it, and remember where the party was

**Date:** 2026-10-12 (owner: "Make all the changes. Also, as part of following, we need to incentivize standing still
when nearby the unmoving player so they don't constantly run around"; "Bots need to remember and predict teammate
locations when they look away or are out of view"). Built on `forge-dev`, before move4_follow's first run.

**Why.** move4_follow as written walked one fixed route per dungeon -- the door, then each boss's place in order --
every episode. A follower could score by walking Ragefire's route itself and never watch the leader: the kept share
would not tell following from memorising. The leader moved like a script (corner to corner, one speed, standing
stock-still at stops, then standing at the route's end for the rest of the clock). Nothing paid a follower for
standing still beside a leader who stands, so running about in the band cost nothing. Between 10 and 40 yd nothing
was paid or charged. A teammate out of the minimap's 60 yd and out of the camera was simply gone from the inputs.
There were no ablation arms and no held-out map.

**Change.**
- **The way and the wandering** (PartyFollowEncounter). The stock PathGenerator's path from the door through each boss's
  place is kept as one chain of corners per map, each step marked walkable both ways, a drop (forward only: nothing
  climbs back up Ragefire's cavern) or a cut (a leg that stopped short of its stop: neither way); waypoints every
  `PartyFollow.WaypointYards` (15) and at each boss's place. An episode starts at a random waypoint
  (`StartAnywhere` 75%; else the door; the map's first episode starts at the door and works the way out). The leader
  hops `HopMinYards`-`HopMaxYards` (20-90) along the way, backwards `ReverseChance` 30% of hops from
  `ReverseFromRung` 1, passing on without a stop `PassChance` 35% of hops, walking `WalkChance` 20% of hops above the
  walking rung, and looking round at `LookChance` 60% of its halts (`LookRate` 1.2 rad/s, once or twice) -- until the
  clock ends. A hop it is stuck on for `GiveUpMs` is given up from the nearest corner. Its draws come from the
  episode's own generator, seeded by the evaluation seed in an evaluation, so an evaluation replays the same
  wanderings. The script is still the thing followed, never an input or a teacher of the policy (0002, 0019); the
  owner chose to keep a scripted leader until a learned one walks dungeons (`CastShare`).
- **Waiting.** `Settled` (new term, Outcome) 0.02/s standing still (under `SettleSpeed` 0.5 yd/s, translation only:
  turning and looking are free) in the band once the leader has stood `SettleAfterMs` 1.5 s; `Fidget` (the existing
  term, Cost) 0.01/s moving about in the band then. Measures `settled_share`, `wait_seconds`, `wait_fidget_seconds`.
- **Drifting.** `Drift` (new term, Cost) 0.01/s between `DriftYards` 20 and `LostYards` 40. Measure `drift_seconds`.
- **Rising.** A dead follower rises at the episode's start point (the door, or the waypoint it began at), not always
  the door: from a mid-way start the door can be a long way off.
- **Recall** (the party frames block, revision 3). Each seat keeps where it last saw each member -- on the minimap
  or in the camera's frame -- and its pace then (`PartyRecall`, SeatState::Party). Seven columns per member: seen this
  episode, seen now, believed right/forward/distance (over 120 yd, in the seat's facing frame: the last sighting
  carried on by the last pace for at most 3 s, then held), seconds since seen (over 30 s), pace when seen. Appended
  and named: revision 2 checkpoints seed every column they had; the recall columns start fresh. Every stage with a
  party frames block reads it (G1 on). A learned predictor (an auxiliary loss on the true positions) needs the
  leader's position on the learner's side, which the scripted leader's absent row does not give: left for later.
- **Held out and ablations.** `wailing` (Wailing Caverns, map 43, its bosses without Mutanus, who has no spawn row):
  EvalOnly, every evaluation (eval.heldout). Arms `no_camera`, `no_map`, `no_memory`, `no_party` (the whole party
  frames block) and `no_recall` (its recall columns alone), 48 episodes every second evaluation.
- **Viewer.** Follow episodes show in the route viewer (`follow_map`, `start_*`, kept share as "found", the follow
  numbers in place of a room); traces are written for every evaluation (`trace_all`).

**Judge it by** follow_kept_share on the held-out Wailing Caverns against the training dungeons (memorising would show
as a gap), the no_party and no_recall gaps (what the frames and the memory carry), settled_share and
wait_fidget_seconds (the waiting the owner asked for), and leader_route_share / leader_skips per map in the preview (a
way that the leader cannot walk shows as skips).

**Revert:** `PartyFollow.StartAnywhere`, `ReverseChance`, `PassChance`, `LookChance` 0 and `HopMaxYards` large give a
door-start forward walk; `Settled`, `Fidget` and `Drift` 0 remove the new terms. The recall columns cannot be turned
off by a key; `no_recall` measures them.
