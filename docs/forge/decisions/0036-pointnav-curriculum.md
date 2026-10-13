# 0036: PointNav on a learned costmap and planner -- the vision movement curriculum (nav1-nav6)

**Date:** 2026-10-13 (`.agents/plans/vision-movement-curriculum/`, the plan and its build contract; the owner's
decisions). A fresh design, not a rework of M1-M7 or the ten live stages: movement only (walk, turn, stop on a point,
detour, step, jump, drop, stairs, swim, follow a mover), no searching -- the goal's position is always known, the route
is not. Habitat's PointNav, not ObjectNav.

**Why.** `move4_follow` at 50M: in 76% of episodes the follower never moved (Lost flat past 40 yd, Regroup collected
by standing, Approach the only gradient at 0.005/yd). M2 at 80-90M: paths 6.8x the shortest, 35-40% of decisions on
visited cells, the memory ablations scoring the same as the learner: end-to-end PPO never learned to *read* the map it
was given. Every stage paid its purpose sparsely and faded the dense signal away, exactly when the policy settled into
a flat-landscape habit; and nothing said *which* part failed -- perception, planning or control.

**Change.**
- **Three learned modules, each with its own truth and metric.** A **costmap** head (egocentric 48x48 traversability
  classes from the camera and the map crop; cross-entropy against the sim's bake, seen cells; per-class IoU), a
  **planner** (value iteration, K 40, 8 channels, over the costmap, the map crop, a coarse 8-yd tier of the mental map
  and the goal; L1 against the true cost-to-go on every cell of both tiers; MAE on known cells, waypoint regret) and
  the **controller** (the 25 move actions and the look head; PPO). One network, one return (MAPPO): the costmap and
  planner are auxiliary heads with auxiliary losses in the actor's forward pass, so the policy acts on its own
  prediction and never on the bake. The hierarchy is architectural (the cell pointer picks a waypoint every 2 s).
- **Labels, not teachers.** The bake (`TravBake`: a 2-yd grid classed by the player controller's own rules, with
  8-neighbour edge costs in yards, never the navmesh) says what the world *is*, not what to *do*; auxiliary labels
  are rewards' kin (0002 forbids behaviour to imitate; 0019 allows stock pathfinding for rewards and setup). Actions
  stay the policy's, pressed through the controller (0006), priced never masked (0008).
- **Shaping that does not fade.** Progress is potential-based on the bake's route cost-to-go, `r = gamma * (-C(s')) -
  (-C(s))`, paid by the **learner** with its own discount (the sim ships `C(body)` in the STEP; 0 on a done row).
  Policy-invariant, paid at x1 for the whole curriculum: no fade, no flat landscape, no farming. Settled, Regroup,
  Drift, Approach and the Exploring/Aid families are not used. **Lost, one rule:** a cost per second while `C(body)`
  exceeds `C(start) + 40`. The ladder keeps one job, difficulty (the draw bins), stepped on argmax SPL (0010).
- **The goal as a player sees it:** a minimap marker -- the compass block (bearing, distance), exact, never withheld
  in a nav stage. The planner also gets it as the target cell of both tiers.
- **The data engine.** Pairs drawn by difficulty (detour and length bins, uniform over the open bins), not by chance;
  failure replay (20% of episodes start where a seat was pinned, fell, drowned or timed out); domain randomisation
  for train-to-realm parity (render sizes, decision-time spikes, map memory kept across resets); a held-out place in
  every stage, Wailing Caverns held out everywhere; argmax parity (entropy anneals to 0.1, evaluation scores argmax
  and reports sampled beside it).
- **Six stages**, a second root seeded from nothing (no M1-M4 weights, decision 7): `nav1_sight` (a point in sight;
  Stockades, Ragefire), `nav2_detour` (the line blocked; + Deadmines, Shadowfang), `nav3_vertical` (steps, jumps,
  drops; + Gnomeregan, Razorfen Kraul), `nav4_beyond` (the route out of view, dead ends, Revisit; Gnomeregan,
  Uldaman, Scarlet Monastery, Maraudon), `nav5_water` (swim, the bed, surfacing; Blackfathom, the cove, the pools),
  `nav6_follow` (the moving point: move4_follow's party, the leader's believed position the compass's point, PointNav
  a labeller only; a frozen-checkpoint leader is a follow-up). Level 1, every race and class. Budgets 50/150/200/300/
  150/250M, ceilings. N7 mounted and flight is out of v1.
- **Dungeons only in this build** (plan decision 10): the camera's scene baker bakes whole maps, so outdoor zone boxes
  (Mulgore, Elwynn, Thousand Needles, the capitals, the lakes) are **Phase B**; every arena is an emptied instance.
- **Infrastructure:** protocol 31 (`LabelBytes` in SPEC, labels last in STEP), map block revision 5 (two tiers,
  `MapTiers` per stage), the cell grid opened to unknown blocks and any height, `.amdl` format 11, `RewardTerm::
  FallHurt` and `Drowned`, the `PointNav.*` tuning group.

**Judge it by** N1's costmap IoU and cost-to-go MAE at the first evaluation (supervised: they should learn within
10M); if they do and SPL still stalls, the controller is the problem -- and that is the part M1 proved learnable. If
IoU stalls, the labels or the crop are wrong, visible in the video's costmap inset. Then `spl` on the held-out
`wailing` arena per stage (N1 >= 0.9 with stop error <= 0.5 yd; N2 >= 0.85 at detour 1.6-2.5; N3 >= 0.8 with
lethal-drop recall >= 0.98 and falls <= 0.05 a trip; N4 >= 0.7 at detour >= 2.5; N5 >= 0.8 with drowned <= 0.01;
N6 follow_kept_share >= 0.8 argmax), the no_planner and no_map arms, and the argmax-vs-sampled gap.

**Alternatives declined (for now).** RL only without aux heads (expected to repeat M2); a privileged teacher fed the
true costmap and distilled by DAgger (the fallback if N4 stalls; close enough to 0002 to ask first); a noisy marker
(not needed where positions are exact); seeding from M1-M4 (the owner: "don't base your plans off what came before").

**Revert:** the nav stages are six definitions in `Stages.cpp` and their yamls; `move4_follow` keeps running until N1
is ready to deploy (decision 8) and N6 supersedes it. The bake is training-only and nothing of it ships.
