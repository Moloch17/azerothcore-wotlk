# 0023: Exploration of unseen territory is paid at a floor the fade does not take

**Date:** 2026-10-10 (owner order: "We need to reward the bot for exploration of unseen territory. It is simply not
searching well enough and we also need to teach it how to get itself unstuck and not turn in circles.")

**Decision.** In the seek stage (M2), exploring is paid as shaping that the fade only takes down to a floor.
`Explore` pays `Seek.ExploreSeen` (0.002) for each 2-yd cell of floor a ray of the seat's camera lands on for the first
time in the episode (times `Seek.ExploreRoomBonus` 2.0 in a room of the room table the seat has not entered), at most
`Seek.ExploreCap` (1.0, before the scale: a third of `Arrive`) an episode. `FrontierPull` pays `Seek.FrontierPull`
(0.004) a yard closed on the nearest frontier of the seat's own mental map, by a best-distance ratchet kept per
frontier cluster, at most `Seek.FrontierCap` (1.0). Both are in a new reward category, `Exploring`, paid times
`max(shaping scale, Seek.ExploreFloor)` (0.5): at the deep rung, where the shaping scale is 0, they still pay half. The
floor is applied sim-side (`RewardLedger::SetShapingFloor`); the learner's fade is unchanged, and stage.json still lists
the two terms as `shaping`. They are not in the score. Beside them: `Circling` (a Cost at a fixed price, in the score),
and a trap drill (a start distribution; its `Escape` is an Aid term, so it fades with the aid scale).

**Exception to principle 9 and decision 0009.** "A stage's purpose is paid as Outcome; shaping fades." Exploring unseen
territory is not the stage's measure (that is `Arrive`: stopped beside the object), but the owner's explicit direction
is that it must not fade away: at 0 shaping nothing paid for reaching an unvisited door, and the policy stopped
searching. This is the one exception; any other term still fades. What keeps it from becoming the objective: the
episode cap (1.0 against `Arrive` 3.0 and a 300 s clock that costs 3.0), the floor at 0.5, a first look that is never
paid, a per-episode record (a map kept from before takes nothing away), and a ratchet per frontier cluster, so walking
back and forth between two farms nothing.

**Why.** The failure analysis of 1,404 evaluation episodes (110-140M steps, `var/m2-failures`): 211 failures, all
timeouts; 74% never got the object into view (the front doors are visited, the 16 `_back` rooms and most end rooms
in 1-6% of searches); 60% loop (at least half of the 3-yd cells already visited, under 2% new room floor in the last 60
s); failed searches revisit 0.49 of their cells against 0.19; 22% are argmax dead-locks in a doorway for 150-290 s;
wall time is a tail at a few chokepoints (the hub doors) plus corridor rubbing. `shaping_scale` was 0.0, so `Sighting`,
`RoomSeen` and `NewGround` were gone.

**What it constrains.** The reward audit sees `explore` and `frontier_pull` as shaping: each is bounded by its cap, so
they stay well under half of `Arrive`. `Circling` and `Stuck` are priced on distinct things (moving in circles; a key
held and no movement) and `Circling` is not charged on a decision that charged `Stuck`; it never double-charges the
Return price (going back into a visited room after leaving it). The trap drill is training-only (never in an
evaluation: it would move `found`, the fade's gate), and nothing scripts an action in it (principle 14).

See also [0009](0009-outcome-versus-shaping-and-the-fade.md), [0008](0008-price-bad-presses-never-mask.md),
[0019](0019-vision-only-movement.md) and the [index](README.md).
