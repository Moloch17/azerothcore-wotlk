# Reference: the sixteen live stages

Purpose and scope: one section per live curriculum stage (`move1_controls` to `dungeon3_deadmines`, and the six nav
stages `nav1_sight` to `nav6_follow` of the vision movement curriculum, decision 0036, added 2026-10-13), written from
the
stage's definition in `src/server/game/Animus/Runtime/Scenario/Curriculum/Stages/Stages.cpp`, its learner config
`apps/forge/python/configs/<stage>.yaml` (with its `extends:` chain merged as `animus/config.py` `load_yaml` does) and
the encounters and tuning it uses. It describes what the code does, not what a comment says; where they disagree the
text says so. Facts that could not be checked are marked `UNVERIFIED`. The chapter that explains how to run and tune a
stage is [../04-curriculum.md](../04-curriculum.md). Related references: [00-architecture.md](00-architecture.md),
[cpp-stagescenario.md](cpp-stagescenario.md), [cpp-encounters.md](cpp-encounters.md), [cpp-blocks.md](cpp-blocks.md),
[cpp-rewards-routing.md](cpp-rewards-routing.md), [cpp-tuning-keys.md](cpp-tuning-keys.md),
[config-yaml.md](config-yaml.md), [config-keys.md](config-keys.md), [metrics.md](metrics.md), [tests.md](tests.md),
[known-issues.md](known-issues.md), [glossary.md](glossary.md), [../decisions/README.md](../decisions/README.md).

**Seeding note (entity sensing, protocol 26).** The per-stage "Seed" notes below that say the camera, entities or sight list "carry" predate vision block revision 6, entities revision 2 and sight revision 3: seeding from a checkpoint of the earlier revisions starts the whole perception stack (`vision.*` but for the map and sight sub-modules, the look head, and the sight list) fresh with the joins zeroed; the trunk, goals and per-layout adapters and heads still carry (bootstrap.py `_seed_vision`, `_seed_sight`, `_seed_look`).

## Map of the source files this document is written from

| Path | Lines | Role |
|---|---|---|
| `src/server/game/Animus/Runtime/Scenario/Curriculum/Stages/Stages.cpp` | 1334 | The ten `StageDefinition`s, the map data tables they use (hallways, rooms, objects, sight pairs, Deadmines sites) and the stage and arena validation |
| `src/server/game/Animus/Runtime/Scenario/Curriculum/Stages/StageDefinition.h` | 324 | `StageDefinition`, `ArenaDefinition`, `Opposition` and `MAX_ARENAS` |
| `src/server/game/Animus/Scenario/Curriculum/CurriculumTuning.h` | 1185 | Every reward weight and draw parameter (`AnimusForge.Curriculum.*`) with its default |
| `src/server/game/Animus/Scenario/Curriculum/Rewards/RewardLedger.h` | 387 | `RewardTerm`, `RewardCategory` (Outcome, Cost, Shaping), the score and the scales |
| `src/server/game/Animus/Scenario/Curriculum/Encounters/SightEncounter.cpp` | see cpp-encounters.md | M1 |
| `.../Encounters/SeekEncounter.cpp` | | M2 |
| `.../Encounters/InteractEncounter.cpp` | | M3 |
| `.../Encounters/PartyFollowEncounter.cpp` | | M4 |
| `.../Encounters/CombatEncounter.cpp` | | C1 to C3 |
| `.../Encounters/RolesEncounter.cpp` | | G1 |
| `.../Encounters/InstanceEncounter.cpp` | 1341 | D2, D3 |
| `.../Encounters/WingLadder.h`, `StageScenario.h` (`WING_RUNGS`) | | The whole-dungeon difficulty ladder |
| `.../Encounters/DifficultyLadder.cpp` | 104 | The per-class ladder of C1 to C3 and G1 |
| `apps/forge/python/configs/*.yaml` | 11 files, 1159 | One learner config per stage, plus the `fast.yaml` overlay |
| `apps/forge/python/animus/stage.py` | 829 | `ShapingFade`, `CostLadder`, `ConvergenceController` |
| `apps/forge/python/animus/config.py` | 831 | Config dataclasses and their defaults |

(The other encounters' and tuning files' line counts are in the cpp-* documents.)

## Overview of all sixteen

| # | Stage | Place | Level | Seats | Opposition | Ladder (gate) | Budget | Trained? |
|---|---|---|---|---|---|---|---|---|
| 1 | `move1_controls` | Stockades (map 34), emptied | 1 (death knight 55) | 1 | Sight (`SightEncounter`) | fade, compass withheld (`arrived_at_rung` 0.8) | 150M | yes (finished, see below) |
| 2 | `move2_seek` | Stockades, emptied | 1 | 1 | Seek (`SeekEncounter`) | fade = placement (`found` 0.8) | 250M | yes (the run in progress on 2026-10-07) |
| 3 | `move3_interact` | Deadmines (map 36), emptied | 17 to 20 | 1 | Interact | fade = site kind (`right_object` 0.8) | 200M | never |
| 4 | `move4_follow` | Ragefire (389) and Deadmines, emptied | dungeon band | 5 (4 learned) | PartyFollow | fade = leader behaviour (`follow_kept_share` 0.75) | 250M | never |
| 5 | `combat1_fight` | Ragefire, cleared | 13 to 18 | 1 | Combat (Fight) | per-class tier + fade (`won` 0.7) | 300M | never |
| 6 | `combat2_packs` | Ragefire, cleared | 13 to 18 | 1 | Combat (Packs) | same (`won` 0.7) | 300M | never |
| 7 | `combat3_survive` | Ragefire, cleared | 13 to 18 | 1 | Combat (Survive) | same (`survived` 0.7) | 300M | never |
| 8 | `group1_roles` | Ragefire, cleared | 13 to 18 | 5 | Roles | per-class tier + fade (`won` 0.6) | 400M | never |
| 9 | `dungeon2_ragefire` | Ragefire, door to Bazzalan | band | 5 | Instance (whole wing) | wing ladder + fade (`full_clear` 0.5) | 1500M | never |
| 10 | `dungeon3_deadmines` | Deadmines, door to VanCleef | 17 to 20 | 5 | Instance (whole wing) | wing ladder + fade (`bar_clear` 0.5) | 2000M | never |
| 11 | `nav1_sight` | Stockades (34) and Ragefire (389), emptied | 1 (death knight 55) | 1 | PointNav (`PointNavEncounter`) | fade = draw bins (`spl` 0.6, 0.7, 0.8) | 50M | never |
| 12 | `nav2_detour` | Stockades, Deadmines (36), Shadowfang Keep (33) | 1 | 1 | PointNav | same | 150M | never |
| 13 | `nav3_vertical` | Deadmines, Shadowfang, Gnomeregan (90), Razorfen Kraul (47) | 1 | 1 | PointNav | same | 200M | never |
| 14 | `nav4_beyond` | Gnomeregan, Uldaman (70), Scarlet Monastery (189), Maraudon (349) | 1 | 1 | PointNav (Revisit) | same | 300M | never |
| 15 | `nav5_water` | Blackfathom Deeps (48), Deadmines, Maraudon | 1 | 1 | PointNav | same | 150M | never |
| 16 | `nav6_follow` | Ragefire and Deadmines, emptied | 1 (intent; see 16) | 5 (4 learned) | PartyFollow + PointNav labels | same (`follow_kept_share`) | 250M | never |

The queue order is the table's order (every stage is in the default queue; `Stages.cpp` lists the nav stages right
after `move4_follow`, so the default queue runs them between it and `combat1_fight`). Budgets are ceilings, not
targets (see [../04-curriculum.md](../04-curriculum.md)). The nav stages are **dungeons only** in this build: every
arena is an emptied instance; outdoor arenas (continent boxes) are Phase B of the vision movement plan. Wailing
Caverns (43) is held out of every nav stage as the arena `wailing`.

### Seed chain

`StageDefinition::Extends` (and `Merges`) in `Stages.cpp` is the seed chain; the sim writes it into `stage.json` as
`seed_chain` and `merges`, and the learner seeds from it (`animus/stages.py` `seed_chain`,
`TrainConfig.resolved_init_from`
with `init_from: auto`, `seed_from: latest` in every live yaml).

```
move1_controls
 └─ move2_seek
     ├─ move3_interact
     │   └─ combat1_fight
     │       └─ combat2_packs
     │           └─ combat3_survive ──(merges move4_follow's party frames)─┐
     │                                                                      v
     └─ move4_follow ─────────────────────────────────────────────────> group1_roles
                                                                            └─ dungeon2_ragefire
                                                                                └─ dungeon3_deadmines

nav1_sight (a second root: seeds from nothing before it, plan decision 7)
 └─ nav2_detour
     └─ nav3_vertical
         └─ nav4_beyond
             └─ nav5_water
                 └─ nav6_follow
```

`extends:` at the top of a yaml is the separate **config chain**. They differ: `combat1_fight.yaml` has no `extends`
(it repeats the whole MAPPO block; `Observed issues` below), `move4_follow.yaml` and `move3_interact.yaml` extend
`move2_seek.yaml`, `combat2_packs` extends `combat1_fight`, `combat3_survive` extends `combat2_packs`, `group1_roles` extends
`combat3_survive`, `dungeon2_ragefire` extends `group1_roles` and `dungeon3_deadmines` extends `dungeon2_ragefire`.

What carries over: networks are seeded block by block, by name (`animus/bootstrap.py`): kept blocks' input columns and
action rows move to where the block sits now, new blocks start at zero, trunk copied, critic state encoder and value
head
fresh, value normaliser not copied. The merge in `group1_roles` seeds the blocks only `move4_follow` has (the party
frames) from its `latest.pt`, input columns and action rows only. Details and the revision rules:
[py-learner.md](py-learner.md),
[../decisions/0012-seeding-by-name-append-only-classes.md](../decisions/0012-seeding-by-name-append-only-classes.md).

### Blocks of each stage (effective)

`CurriculumStages()` inserts the `Entities` block right after `Vision` in every stage that has a camera (`Stages.cpp`,
end of the file), so the lists below include it. Widths are the class-independent totals
(observation, actions), as the removed layout pin recorded them. `Duel`, `Pet` and `Core` are class dependent.

| Stage | Blocks in layout order | Total obs, actions |
|---|---|---|
| `move1_controls` | core, move, compass, vision, entities, goal | 841, 25 |
| `move2_seek` | core, move, vision, entities, map, goal | 840, 25 |
| `move3_interact` | core, move, vision, entities, map, sight, goal | 2911, 346 |
| `move4_follow` | core, move, vision, entities, map, party_frames, goal | 924, 37 |
| `combat1_fight`, `combat2_packs` | core, move, duel, pet, vision, entities, map, sight, combat, goal | 3779, 350 |
| `combat3_survive` | the above plus gauntlet (after pet) | 3790, 353 |
| `group1_roles` | the above plus party_frames (after sight) | 3874, 365 |
| `dungeon2_ragefire`, `dungeon3_deadmines` | core, move, duel, pet, pack, gauntlet, vision, entities, map, sight, party_frames, combat, goal | 4908, 390 |
| `nav1_sight` to `nav5_water` | core, move, compass, vision, entities, map, goal (the map block with two tiers, `MapTiers = 2`: revision 5) | `UNVERIFIED` (not yet built) |
| `nav6_follow` | core, move, compass, vision, entities, map, party_frames, goal (two map tiers) | `UNVERIFIED` |

Block ids: core 0, move 1, compass 2, duel 3, pack 4, gauntlet 5, pet 11, vision 20, entities 21, map 22, sight 23,
party_frames 24, combat 25, goal 26 (`Layout/Block.h`; never renumbered). What each block is:
[cpp-blocks.md](cpp-blocks.md).

### Common to every stage

* **Decisions and ticks.** `AnimusForge.DecisionMs` 250 and `AnimusForge.Stage.<name>.TicksPerDecision = 5` for every
  live stage (`worldserver.conf.dist` lines 5202 to 5216), i.e. 50 ms world ticks.
* **Ladder machinery.** `fade` is the shaping ladder (`ShapingFade`, `animus/stage.py`); in this curriculum it is also
  the
  sim's difficulty ladder, because the sim reads the fade's scale as the rung for M1 to M4. `costs` is the cost ladder
  and is **off in all ten** (`costs.enabled: false`, set in M1 and inherited, or set explicitly). Every gate-stepped
  fade has `require_plateau: false` except M1's (see M1). A gate-stepped ladder never steps back on the score and raises
  a collapse alarm (3 evaluations under max(0.1, a quarter of the rung below)) and a stall warning (no better than the
  best by more than the standard error for `fade.stall_evals` = 4 evaluations and `stall_env_steps` = 20M steps,
  `config.py` FadeConfig); neither acts. Convergence is re-baselined at every forward step
  ([../decisions/0011-convergence-rebaselined-per-rung.md](../decisions/0011-convergence-rebaselined-per-rung.md)).
* **Universal reward terms** (paid in every stage by `StageScenario::SeatReward`, `StageScenario.cpp:4909` to `5070`):
  noise prices, all Cost, scaled by the cost ladder (which is off, so full): Repeat 0.03 per repeated press beyond 3 in
  10 s, Jitter 0.05, Aimless 0.02 (with per-cause prices, `Actions.Aimless*`), Effort 0.004 per press, Fidget 0.01 per
  second; Shaping, scaled by the fade: GoalSwitch (`Goals.Switch` 0.15 per goal change), GoalProgress (0.5 x potential),
  GoalReached (0.05, or `*Value` by kind), SelfHealing 0.5, HealingMana 0.1 (a cost in value but categorised Shaping),
  CombatClock 0.03 per second an engaged enemy lives (`Output.Clock`), Hazard (standing 0.15 per second, damage 0.5 per
  max-health share, capped 3.0 an episode). The goal block's order columns are always zero (Observed issues).
* **Evaluation.** Every config: `eval.seed` 1000, `deterministic: true`, `at_start: true` (inherited), `score: outcome`
  (the sim's `score_outcome`, `EvalConfig.score_column`), eight videos an evaluation at scale 4 (`AnimusForge.Vision.
  EvalVideos = 8`, `worldserver.conf.dist:5443`) written as animated PNGs under `runs/<stage>/videos/<env steps>/`,
  filmed from seeds that are the same every evaluation (`Vision/EvalVideo.h`).
* **Shared MAPPO values** (M1's block, inherited by the others unless noted): hidden [256, 512, 512], gae_lambda 0.985,
  clip 0.2, entropy_coef 0.01 falling to 0.3 of it (`entropy_final_fraction`), lr falling to 0.1 of itself
  (`lr_final_fraction`), recurrent_size 128, target_kl 0.02, minibatches 4, `per_layout_advantages`
  (the sim still writes seat `sets` into stage.json for the layouts with a pack block; the learner ignores them),
  rollout_length
  128, `overlap_updates: true`, convergence `patience 3, window 4, z 2.0, kl 0.003, entropy_slope 0.01, hold_share 0.02`
  for M1 and C1 explicitly and as defaults elsewhere.

### Learner hyperparameters that differ

| Stage | gamma (per 100 ms) | epochs | actor and critic lr | look_entropy_coef | chunk_length | other |
|---|---|---|---|---|---|---|
| `move1_controls` | 0.997 | 2 | 1.5e-4 | 0.001 | 32 | vision_chunk_rows 0 |
| `move2_seek` | 0.998 | 2 | 1.5e-4 | 0.004 | 128 | vision_chunk_rows auto (was 2048) |
| `move3_interact` | 0.998 (inherited) | 2 | 1.5e-4 | 0.004 | 128 | |
| `move4_follow` | 0.999 | 2 | 1.5e-4 | 0.004 | 128 | |
| `combat1_fight` | 0.998 | 2 | 1.5e-4 | 0.004 | 128 | standalone yaml (M2's lr and look entropy repeated, A6) |
| `combat2_packs` | 0.999 | 2 | 1.5e-4 | 0.004 | 128 | |
| `combat3_survive` | 0.999 | 2 | 1.5e-4 | 0.004 | 128 | |
| `group1_roles` | 0.999 | 2 | 1.5e-4 | 0.004 | 128 | |
| `dungeon2_ragefire` | 0.999 | 2 | 1.5e-4 | 0.004 | 128 | extends `group1_roles`; restates `goal_entropy_final_fraction: 0.5` |
| `dungeon3_deadmines` | 0.999 (inherited) | 2 | 1.5e-4 | 0.004 | 128 | |
| `nav1_sight` to `nav6_follow` | per `configs/nav*.yaml` (the learner agent's; contract sec 5) | | | | | `potential:` (enabled, yards_scale 0.01, clip 50) and `aux:` (costmap_coef 0.5, planner_coef 0.5) sections; `entropy_final_fraction 0.1`; `sampled_every 1` |

M1's rate 1.5e-4 was halved at 93M steps (comment in the yaml); M2's is the same value for a different reason
([../decisions/0013-m2-learning-rate.md](../decisions/0013-m2-learning-rate.md)). The combat line does not inherit
M2's rate or look entropy (Observed issues).

---

## 1. `move1_controls`

**Purpose.** Teach the controls: walk a straight line and stop beside a thing, seen through the camera, so that the
next stage begins with a camera that works. First stage, so everything after it inherits legs that move on the player
controller. Nothing to fight; every class and race at level 1 (a death knight at its 55), so a class's kit is one or two
spells and the lesson is movement alone (`Stages.cpp` comment, `.Level = 1`).

**Seed.** None (`Extends = ""`). `finetune_from` defaults to `runs/_finetune/move1_controls/best.pt`; if that file
exists
the run starts from it, copied block by block (the redesign of 2026-10-06 was started from the old M1 this way).
`init_from: auto`, `seed_from: latest`.

**Place.** Stockades, map 34, an instance of its own for every env, creatures cleared on the env's first build
(`SpawnArea::Clear`). Arena `hallway` (weight 1, `Opposition::Sight`, 60 s). Spawn points: the 3-yard hallway grid
(`StockadeHallways()`, the entrance first); the entrance is (54.23, 0.28, -18.34).

**Blocks.** core, move, compass, vision, entities, goal.

**Episode.** Each episode the seat spawns at a random hallway point, facing a random way; one real object of M2's pool
(chest, crate, barrel, sack, strongbox: `SeekObjects()`, five entries) stands at another hallway point
`Controls.Nearest` 10 to `Furthest` 120 yd off, in sight of the seat's eye (`SightDraw::Place`); from `CornerFrom`
0.75 (1 minus the shaping scale) a `CornerShare` 0.25 of the episodes put it just round a corner. Arriving is stopping
(`Standing::Stopped`) with the feet within the object's bounding radius plus `ArriveTolerance` 1.0 of its centre, on its
floor (within `ArriveRise` 2.0). The compass is withheld for an episode (presence and values 0, an absent input, never a
mask) with chance `Withhold0..3` = 0.25, 0.6, 0.9, 0.9 at fade scales 1, 0.5, 0.25, 0 (the code defaults since
2026-10-08; `SightDraw::Rung` takes the nearest of the four scales): a quarter of the episodes already lack the compass at
the first rung, each paid the rung's dense shaping, and the last step (x0.25 to x0) takes only the shaping away.

**Rewards** (`SightEncounter::Reward`, `SightEncounter.cpp:319` to `416`). Outcome: `Arrive` 3.0 once on the stop
(`Markers.Arrive`). Cost: `StepCost` 0.002 per 50 ms (about 0.04 a second; `Markers.StepCost`), `Death` 3.0
(`Markers.Death`), `Stuck` and `Wall` at their own fixed price 0.02 per second each (`Controls.Stuck`, `Controls.Wall`,
`WallSlide` 0.5; `AddFixed`, so off the cost ladder). Shaping: `Progress` 1.0 on the straight distance, `Facing` 0.25 on
the cosine of the object's bearing (`Markers.Progress`, `Markers.Facing`), both scaled by the fade.

**Ladder.** One fade with the compass withholding riding on it. As configured (`move1_controls.yaml`, Design A,
2026-10-08) `fade.rungs` is `[1.0, 0.5, 0.25, 0.0]`: it starts at x1 (compass withheld 25%, full shaping) and steps
x0.5 (60%), x0.25 (90%, corners on), x0 (90%, no shaping) on `gate_metric: arrived_at_rung` >= 0.85, `require_plateau:
false` (a rung steps as soon as the gate is met at one evaluation; the ladder never steps back on the score, decision
0010), `window 3`, `give_up 2`. `costs.enabled: false`; `entropy_floor.fraction 0.3`. The earlier file carried `[0.0]`
(the end state of a hand-stepped run) and started every fresh run at the hardest rung, 0.000 no-compass arrival for 28M
steps; the archived run `move1_controls-20261009-002430` is the one success (0.968 no-compass arrival), reached by
stepping the same rungs from x1.

**Evaluation.** Every 5M steps, 512 episodes, `sampled_every: 3` (every third evaluation also scores sampled actions),
`trace_episodes: 64`. The sim plays 32 fixed (spawn, object) pairs (24 in sight, 8 round a corner,
`StockadeSightPairs()`), each with the compass and without, spread over every class and race. Arms (the learner's input
edited on the first 128 seeds, 31 castings a round, every evaluation): `no_flag` (objective bit of every pixel cleared),
`no_camera` (the image replaced by the no-frame pixel), `no_compass` (compass columns zeroed); reported as
`arrived_no_compass_no_flag` and the like. No heldout. `layout_sampling.replay_fraction 0`: the pairs are not replayed.

**Status.** Headline: arrived, arrived_no_compass, arrived_with_compass, compass_withheld, arrive_seconds_sight,
time_ratio_sight, time_ratio_corner, stop_distance, overshoot, stops_near, course_kinks, control_changes_per_minute,
wall_seconds, timed_out, died. Targets: arrived >= 0.95, arrive_seconds_sight <= 18, time_ratio_sight <= 1.1,
stop_distance <= 0.5, overshoot <= 0.5, stops_near <= 1.2, course_kinks <= 5, wall_seconds <= 0.5, timed_out <= 0.02,
died <= 0.01. `convergence.measure: arrived`. `layout_sampling.metric: arrived`, `replay_fraction 0`.

**Key conf knobs.** `Controls.*` (Nearest, Furthest, CornerShare, CornerFrom, Withhold0..3, Stuck, Wall), `Markers.*`.

**Status of training.** Trained (it is the first stage and its run finished before M2 began). Its checkpoint is the
seed of everything. `UNVERIFIED`: its final numbers (look in `var/animus-forge/shared/archive`).

**Tests.** None (removed 2026-10-07).

## 2. `move2_seek`

**Purpose.** Find a hidden object by sight alone: no compass, the camera's objective flag shows it only in line of
sight.
Teaches search, memory of where it has looked, and the mental map. It follows M1 because it needs M1's camera and legs,
and
it precedes everything that perceives and remembers entities.

**Seed.** `move1_controls`: the move block's columns carry by name, the camera carries, the compass's weights stay
behind
and the map block starts fresh (its join at zero). `seed_from: latest`.

**Place.** The emptied Stockades and, since general search (decision 0027, 2026-10-10), the emptied Ragefire Chasm, 1:1:
an env's episode draws its arena and so its map, and an env that moved to another map clears the new instance when it
opens (whole dungeon radius off the Stockades). Seat at a random hallway point of the arena's table. Arenas: `rooms`
(map 34, weight 1, 420 s ceiling; an episode lasts its rung's `Seek.RungSeconds*`; `TableTerms`), `ragefire` (map 389,
weight 1, 28 rooms of `SeekTables::RagefireRooms()`, 4 front, 192 hallway points; map-derived terms only), `sweep` and,
since decision 0025, `sweep_rotating` (both `EvalOnly`, never in training; 420 s: a sweep plays the deep rung's
`Seek.RungSeconds3`; the rotating one on seed indexes shifted each evaluation), and `deadmines` (map 36, `EvalOnly`: the
held-out transfer measure, 33 rooms of `SeekTables::DeadminesRooms()`, every (room, object) pair once = 165 episodes,
`found_heldout_map` the headline). 39 Stockades rooms (`StockadeRooms()`), five objects. A seeded, unpinned evaluation
episode goes round the trainable arenas by its index (156 a map in 312) and cycles the rooms on the index divided by two.

**Blocks.** core, move, vision, entities, map, coverage, goal (the coverage block is M2's alone; M3 and M4 keep their lists).

**Episode.** One object placed by the ladder: hallway in sight of the spawn (rung 0), just inside a front cell's opening
(1), anywhere in a front cell (2), deep (3). Each rung keeps `Seek.CarryShare` 0.1 of the one below. Episode seconds by
rung
`RungSeconds0..3` = 90, 120, 200, 300. Found is stopping within `SeekRadius` 3.0 yd, on the floor (`ArriveRise` 2.0).

**Rewards** (`SeekEncounter.cpp:441` to `550`). Outcome: `Arrive` 3.0 (`Seek.Arrive`). Cost: `StepCost` 0.0005 per 50 ms
(a 300 s episode costs 3.0), `Death` 6.0, `Stuck` and `Wall` 0.02 fixed. Shaping: `Sighting` 0.5 once, `NewGround` 0.004
per
4 yd cell first walked, `RoomSeen` 0.1 once per room whose floor the camera first shows (3 rays). Exploration v2 (decision 0025, 2026-10-10): `ExploreCap` 5.0, a cell seen from inside its own room x3 (`ExploreInsideBonus`), `RoomEntry` 0.1 per first room entry (x2 back rooms, cap 2.0), `FrontierPull` 0.015 a yard toward frontiers at a door or beyond (cap 2.0), `Stale` 0.01 a second after 20 s with no newly seen floor (Cost). Since 2026-10-10 (explore-unstuck, decision 0023): `Explore` 0.002 per newly seen 2-yd floor cell (x2 in a room not yet entered, cap 1.0) and `FrontierPull` 0.004 a yard (cap 1.0), both paid at least at `Seek.ExploreFloor` 0.5 of their price whatever the fade; `Circling` 0.02 a second (Cost); 12% of training episodes start in a trap pose against a door jamb, `Escape` 0.3 (Aid) for getting 6 yd away within 20 s.

**General search and movement pacing** (decision 0027, 2026-10-10): on `ragefire` and `deadmines` the room table is
placement geometry only; the search terms come from the seat's own crop (`FrontierClear` 0.1 in `FrontierCap`,
`PocketEntry` 0.3 in `RoomEntryCap`, the table-free `Stale` clock, the pocket weights of `Explore`); `Revisit` 0.002 a
second (Cost) on ground stood on before; `StepCost` 0.00025; `ExploreFromRung` 1; the trap drill from `TrapFromRung` 1
with corner and pillar poses and `TrapReplayShare` 0.5 of its episodes started where a training seat pinned;
`TrapEscapeTurnDeg` 0; `Circling`'s turn clause only with a movement key held; `Recovered` (Aid) pays back half a pin's
Stuck and Wall charges (cap 0.3) once the seat is 4 yd away within 10 s. Inputs: map block revision 3 (eight coverage
scalars), the coverage block, move block revision 6 (contact side, blocked share, hold and pinned age).

**Ladder.** `fade.rungs [1.0, 0.5, 0.25, 0.0]` (hallway, doorway, front room, deep), `gate_metric found`, `gate_value
0.8`,
`require_plateau: false`. The comment in the yaml says the other noise prices "keep M1's cost ladder", but M1's
`costs.enabled` is false and is inherited, so the cost ladder is off (Observed issues).

**Evaluation.** Every 10M, 78 episodes at the training rung (each room in turn, objects cycled). Heldout `sweep: 195`
(every
(room, object) pair once, 39 x 5, at the top rung), `heldout_every: 1000` (so only the stage's last evaluation) and
`heldout_on_best: false`. (Since 2026-10-09: 156 episodes, the sampled policy and the sweep every evaluation; see the yaml.)
**Arms** (2026-10-09): `no_flag`, `no_camera`, `no_map`, `no_memory`, 64 episodes each, every evaluation ([metrics.md](metrics.md)); headline gains `found_no_flag`, `found_no_camera`, `found_no_map`, `found_no_memory`.

**Status.** Headline: found, found_hallway, found_doorway, found_room, found_deep, seek_rung, found_deepest,
find_seconds,
sight_seconds, sight_to_arrival, rooms_looked, rooms_before_found, revisit_rate, objective_visible, wall_seconds,
timed_out,
died. Targets: found >= 0.95, found_deepest >= 0.9, find_seconds <= 90, sight_seconds <= 60, sight_to_arrival <= 15,
rooms_before_found <= 12, revisit_rate <= 0.2, wall_seconds <= 2, timed_out <= 0.05, died <= 0.01.
`convergence.measure: found`.

**Hyperparameters.** See the table; `look_entropy_coef` 0.004 (0.4 of the movement entropy coefficient 0.01; a prior
toward looking
around, never a mask) came from a doorway rung stuck at 39% found for 30M steps. `epochs: 2` for the cost of the camera
and map
encoders.

**Status of training.** Trained and the live run on 2026-10-07 (see [cluster.md](../cluster.md)): the doorway rung was
held
at about 35% for 30M steps and the convergence bug that annealed the rate was fixed on 2026-10-07 (decisions 0010, 0011,
0013).
`UNVERIFIED`: its current rung.

**Tests.** None (removed 2026-10-07).

## 3. `move3_interact`

**Purpose.** Tell objects apart and use the dungeon's own doors, levers and locks through the sight block's presses
(select,
interact, use an item on), as a client sends them. No looting. The goal names what, never where.

**Seed.** `move2_seek`: move, camera, entities and map carry by name; the sight block (list, named row, pointer heads)
starts
fresh with its pools at zero.

**Place.** An emptied Deadmines (map 36). Arenas `sites` (120 s) and `sweep` (`EvalOnly`). Four sites
(`DeadminesSites()`:
factory, foundry, mast_room doors with levers, and iron_clad with the cannon). `MinLevel 17`, focus level 17 to 20 at
100%.

**Blocks.** core, move, vision, entities, map, sight, goal.

**Episode.** Rungs on the fade's scales 1, 0.5, 0: *distinguish* (the named object among 2 to 4 decoys, all in sight),
*switch*
(the named object behind a shut door; the lever on the seat's side opens it), *key* (the cannon, opened only by the
Defias
Gunpowder carried from the start). Episodes `RungSeconds0..2` = 60, 120, 90 s (the arena's `EpisodeSeconds` is 120).

**Rewards** (`InteractEncounter.cpp:557` to `685`). Outcome: `Arrive` 3.0, `DoorOpened` 1.0 once. Cost: `WrongObject`
0.5 per decoy
(once each), `StepCost` 0.0005, `Death` 6.0, `Stuck`/`Wall` 0.02 fixed; refused presses are priced by the sight block
(`Actions.AimlessActRefused` 0.02). Shaping: `Sighting` 0.5 once.

**Ladder.** `fade.rungs [1.0, 0.5, 0.0]`, gate `right_object` 0.8, `require_plateau: false`; no cost ladder.

**Evaluation.** Every 10M, 64 episodes at the training rung; heldout `sweep: 60` (every rung, seed mod 3), once at the
end.

**Status.** Headline: right_object, right_distinguish, right_switch, right_key, interact_rung, door_by_lever, key_used,
wrong_objects, lever_pressed, act_refused_locked, right_seconds, sight_seconds, sight_to_arrival, wall_seconds,
timed_out,
died. Targets: the right object rates >= 0.9, wrong_objects <= 0.1, sight_seconds <= 30, sight_to_arrival <= 15,
wall_seconds <= 2, timed_out <= 0.05, died <= 0.01. `convergence.measure: right_object`.

**Unproven.** Never trained. Its sites are authored tables formerly validated by a data test (removed 2026-10-07); the authoring script
is not
in the repository (Observed issues).

**Tests.** None (removed 2026-10-07).

## 4. `move4_follow`

**Purpose.** Keep with a leader through an empty dungeon: spacing, not blocking, keeping up through doors and drops,
waiting when
it stops, regrouping. It extends `move2_seek` (not M3) because it needs the camera and map but not the sight block, and
it runs
beside the combat line; its party frames reach `group1_roles` by the merge.

**Seed.** `move2_seek`; the party frames block starts fresh.

**Place.** Arenas `ragefire` (map 389) and `deadmines` (map 36), weight 1 each, a party of five (`PartySize =
GROUP_MEMBERS` = 4
learned followers plus a leader), 300 s, emptied (creatures, doors, levers and chests removed). Level: the dungeon's
band;
`UNVERIFIED`: the exact draw (the stage sets no `Level`; see `StageScenario` level pick).

**Blocks.** core, move, vision, entities, map, party_frames, goal.

**Episode.** The leader is in the owner's slot and is **scripted**: the seek helper's keys toward the next corner of the
route
planner's way to each boss's place in turn, stopping at each (`PartyFollowEncounter.h`); `PartyFollow.CastShare` is 0,
so no
checkpoint plays it. Four learned followers keep 3 to 10 yd from it. A follower that dies rises at the entrance after 10
s and
walks back; no episode ends on a death.

**Rewards** (`PartyFollowEncounter.cpp:623` to `698`). Outcome: `FollowKept` 0.02 per second inside the band 3 to 10 yd,
`Regroup` 0.5 once per leader stop of at least 2 s on coming back into the band times (1 minus seconds/20). Cost: `Lost`
0.02
per second beyond 40 yd, `Blocking` 0.05 per second within 2.5 yd ahead of a moving leader inside 45 degrees, `Death`
3.0,
`Stuck` and `Wall` 0.02 fixed. No Shaping.

**Ladder.** `fade.rungs [1.0, 0.5, 0.25, 0.0]` = a running leader hopping forward with long stops, turning back with
bursts of speed, sudden stops, sudden stops and steps back (`PartyFollow.ReverseFromRung 1, BurstFromRung 1,
SuddenFromRung 2, BackStepFromRung 3`); rising gates on `follow_kept_share`, `gate_values [0.65, 0.70, 0.75]`
(decision 0035), `require_plateau: false`; `costs.enabled: false` with its gate removed.

**Evaluation.** Every 10M, 96 episodes (seeded wanderings: decision 0033), Wailing Caverns held out (`wailing`, 48
episodes every evaluation), arms `no_camera`, `no_map`, `no_memory`, `no_party`, `no_recall` (48 each, every second
evaluation), `trace_all` on.

**Status.** Headline: follow_kept_share, settled_share (>= 0.8), regroup_share, regroup_seconds, drift_seconds (<= 10),
lost_seconds, blocking_seconds, deaths,
rejoin_seconds,
rejoined, leader_route_share, difficulty, wall_seconds, died. Targets: follow_kept_share >= 0.9, regroup_share >= 0.9,
regroup_seconds <= 5, lost_seconds <= 5, blocking_seconds <= 3, rejoined >= 0.9, wall_seconds <= 2, died <= 0.02.
`convergence.measure: follow_kept_share`.

**The leader** (decision 0035): a player in the owner's slot, the real leader of the party's group, run along the
stock path by the encounter and relocated on the server only at a client's report cadence -- the one remaining script
([decision 0002](../decisions/0002-no-scripted-teachers.md): the thing followed, not a teacher), never a learner row.

**Tests.** None (removed 2026-10-07).

## 5. `combat1_fight`

**Purpose.** Fight by sight: choose a target from the sight list, cast at the selection as the client does, one creature
at a
time; in `guard` also taunt a passive friend off and heal it. It extends `move3_interact` (the plan's base) because it
fights
through the sight block's pointers and only M3 trains that block.

**Seed.** `move3_interact`: camera, entities, map, move and sight carry; the sight slots are widened by the combat
columns (zero),
duel, pet and combat blocks start fresh.

**Place.** Ragefire Chasm (map 389) cleared of creatures, start at one of the dungeon's corridor spawn points within 220
yd
(`Combat.CorridorWalk`) of the entrance. Level: focus 13 to 18 at 100% (`FocusLevelFirst/Last`); a death knight at its
55,
against creatures of its level. Arenas `fight` (weight 3) and `guard` (weight 1, `Ally`), 150 s, `RespawnAtEntrance`.

**Blocks.** core, move, duel, pet, vision, entities, map, sight, combat, goal.

**Episode.** One creature 28 to 40 yd off in line of sight; the next 2 s after each kill. Casters from tier 1, an elite
from
tier 4. The per-class-and-build `DifficultyLadder` picks the tier 0 to 5 (`Combat.MaxTier` 5; creature level = seat
level + -2 +
tier). 10% of training fights are drawn one tier up and 25% from a lower one (`Difficulty.StretchChance`,
`ReviewChance`) and do
not count.

**Rewards** (`CombatEncounter.cpp:584` to `683`; w = 1 + 0.25 x tier). Outcome: `Kill` 1.0 x w per creature, `Survived`
1.0 x w
at the end with no death. Cost: `Death` 2.0 / w, `TeammateDeath` (the guard's friend) 1.0 / w, `Hurt` 0.2 per max-health
share
taken, `StepCost` 0.01 per second a creature lives engaged (`Combat.Clock`), `Away` 0.02 per second dead or beyond 30 yd
of the fight.
Shaping: `DamageDealt` 0.3 per creature-health share.

**Ladder.** Two: the sim's per-class tier (steps up at 90% over a window of 200 fights, **down below 60%**) and the
learner's
fade `[1, 0.5, 0.25, 0]` with gate `won` 0.7, `require_plateau: false`, `moving_classes: 1000` (the fade never waits for
the classes' tiers).
No cost ladder.

**Evaluation.** Every 10M, 240 episodes (seed i plays pair i mod pairs at tier (i / pairs) mod 6), `sampled_every 3`,
`trace_episodes 32`. No arms.

**Status.** Headline: won, survived, kills, kill_seconds, hurt_share, deaths, rejoin_seconds, difficulty,
target_in_view,
selected_share, ally_deaths. Targets: won >= 0.9, survived >= 0.9, kills >= 3, kill_seconds <= 25, hurt_share <= 0.8,
target_in_view >= 0.8, ally_deaths <= 0.1. `convergence.measure: won`.

**Unproven.** Never trained. **Tests.** None (removed 2026-10-07).

## 6. `combat2_packs`

**Purpose.** Packs of 2 to 4: focus, interrupts, crowd control, line of sight, leaving ground fire, one pack at a time.

**Seed.** `combat1_fight`; every block carries (same layout).

**Place and episode.** The same ground. Arenas `packs` (weight 2) and `fire` (weight 1, `Hazards`: every pull has a
ground-effect
caster), 240 s. The next pack stands 30 to 45 yd further on; pulling it before this one is down is an extra pull. A
caster from tier
1, linked from tier 2, fire underfoot in `HazardChance` 33% of `packs` packs.

**Rewards.** Outcome: `Clear` 2.0 x w per pack, `Survived` 1.0 x w, `InterruptLanded` 0.25 per interrupt that stopped a
cast
(unscaled). Cost: `PullExtra` 1.0 per pack drawn in early, `FireHurt` 1.0 per max-health share from ground effects,
`Hurt` 0.2,
`Death` 2.0 / w, clock 0.01/s, `Away`. Shaping: `DamageDealt` 0.3.

**Ladder, evaluation, status.** As C1 but gate `won` 0.7; evaluation `report` adds packs_cleared, extra_pulls,
interrupts,
fire_share, interrupt_earnings. Headline: won, survived, packs_cleared, extra_pulls, interrupts, interrupt_earnings,
fire_share,
hurt_share, deaths, rejoin_seconds, difficulty, target_in_view; targets: won/survived >= 0.85, packs_cleared >= 3,
extra_pulls <= 0.1, fire_share <= 0.1, hurt_share <= 1.5, target_in_view >= 0.8, interrupt_earnings <= 0.3 (a watch that
`InterruptLanded` is not farmed). gamma 0.999. 300M, every 10M, 240 episodes.

**Unproven.** Never trained. **Tests.** None (removed 2026-10-07).

## 7. `combat3_survive`

**Purpose.** Survive: packs that can kill (3 or 4, two levels up on the rung's), pull after pull, with food and drink
stocked and
the gauntlet block (eat, drink, rest until ready) so resting and backing off are the seat's choices; after a death, come
back from
the entrance and finish the pack.

**Seed.** `combat2_packs`; the gauntlet block starts fresh. **Place.** The same ground; arena `survive`, 360 s.

**Rewards.** Outcome: `Survived` 2.0 x w (`Combat.SurviveSurvived`, the stage's purpose), `Clear` 2.0 x w,
`InterruptLanded` 0.25.
Cost: `Away` 0.02 per second dead, walking back or beyond 30 yd of a fighting pull (coming back is never paid), `Death`,
`Hurt`,
`FireHurt`, `PullExtra`, clock. Shaping `DamageDealt`.

**Ladder.** Gate `survived` 0.7, `convergence.measure: survived`, `layout_sampling.metric: survived`. Headline:
survived, won,
packs_cleared, deaths, rejoined, rejoin_seconds, dead_seconds, away_seconds, rest_seconds, extra_pulls, hurt_share,
difficulty;
targets survived/won >= 0.8, packs_cleared >= 3, rejoined >= 0.9, rejoin_seconds <= 60, extra_pulls <= 0.1.

**Unproven.** Never trained. Its checkpoint is also the first partner of `group1_roles` (`cast.partners.stages`).

## 8. `group1_roles`

**Purpose.** Drill one role an episode in a party of five on the combat stages' ground: `tank_hold`, `heal_keep`,
`damage_discipline`, `pull`. The drilled role is in seat 0, its class and build drawn among those whose spec plays it.

**Seed.** `combat3_survive`, with `move4_follow` merged in (`bootstrap.seed_merges`) for the party frames.

**Place.** The cleared Ragefire, level 13 to 18 (focus 100%); death knights not fielded. Arenas `tank_hold` (weight 2,
240 s),
`heal_keep` (2, 300 s), `damage_discipline` (2, 240 s), `pull` (2, 360 s), each `ProperParty`, `PartyGroup`,
`RespawnAtEntrance`.

**Rewards** (`RolesEncounter.cpp:570` to `702`). Outcome: the drilled seat's own `DrillHold` (0.045 per enemy on the
tank per
decision), `DrillKeep` (0.0006 per member above half health), `DrillFocus` (0.9 per damage-scale share on the tank's
target),
`PullClean` 2.0 (others 0.5 of it); `Clear` 1.0 x w per pack; `Survived` 1.0 x w. Cost: misses of the drilled seat
(`Loose`
0.018, `PulledOff` 0.012, `KeepLow` 0.0006, `Overheal` 0.5), `PullExtra` 1.5, `Death` 2.0 / w, `Away` 0.02, clock 0.01.
Shaping
`DamageDealt` 0.3 plus `PartyEncounter`'s role nudges for the seats not drilled.

**Ladder.** Per-class tier (`Roles.MaxTier` 5, `KeepHealthPct 200`, camp spacing 45 to 25 yd) and the fade, gate `won`
0.6,
`require_plateau: false`.

**Evaluation.** Every 10M, 384 episodes over the four drills. Arms: `with_human: 64`, `with_partners: 64`, `arms_every:
2`.
Co-op partners: `cast.partners` stages `[combat3_survive]`, snapshots every 20M, `share 0.3`, up to 2 partners, never
the drilled
seat. Stand-in share `Roles.StandInShare` 20%.

**Status.** Headline: won, won_hold, won_keep, won_focus, won_pull, hold_share, kept_share, focus_share, clean_share,
extra_pulls, party_deaths, wipes, rejoined, rejoin_seconds, roles_rung; targets won rates >= 0.7, hold_share >= 0.8,
kept_share >= 0.8, focus_share >= 0.6, clean_share >= 0.9. `convergence.measure: won`.

**Unproven.** Never trained. **Tests.** None (removed 2026-10-07).

## 9. `dungeon2_ragefire`

**Purpose.** A full clear of Ragefire from the door to Bazzalan: every pull and side boss (Oggleflint, Taragaman,
Jergosh), deaths and rejoins in play. Nothing tells the party the way (vision-only movement,
[../decisions/0019-vision-only-movement.md](../decisions/0019-vision-only-movement.md)).

**Seed.** `group1_roles` (`Extends`; every block carries but the pack block, which starts fresh). The yaml
`dungeon2_ragefire.yaml` extends `group1_roles.yaml`, restates `goal_entropy_final_fraction` 0.5, `eval.seed` 1000 and
`eval.deterministic`, and nulls G1's status targets. **Place.** Arena `dungeon` (`InstanceRow 0`, 7200 s) and `heldout`
(Wailing Caverns, `InstanceRow 2`, `EvalOnly`, weight 0, 10800 s, door to Lord Serpentis).
`Instance.WingWipes` 2: the first wipe is scored and the party rises at the entrance, the second ends the run.

**Rewards** (`InstanceEncounter::RewardTerms()`: StepCost, Kill, Death, Timeout, Threat, Clear, ReadyPull, Idle, Lost,
Away). Outcome: `Kill` (`WingMidBoss` 8.0 per boss on the way, `WingBoss` 25.0 the last, `WingTrashKill` 1.0), `Clear`
`WingClear` 25.0 for the full clear, `ReadyPull` `WingEngage` per pull started with everyone ready (paid at most once a
run: `READY_PULLS_PAID` = 1). Cost: `Idle` `WingStall` per second after `WingStallGraceMs` with no kill and no fight
(the stall clock resets on kills and fights only; others pay `WingStallOthers` of it), `Lost` `WingStray`, `Away`
`WingAway`, `Death` `WingDeath` and `WingWipe`, `StepCost` `WingClock`, `Timeout` at the clock-out
`-WingTimeout x (1 - ClearedShare)`, with `ClearedShare` = min(1, (trash kills + boss dead) / (hostile total + 1)), over
the tier scale. Shaping: `Threat` (`WingCrowd`), `DamageDealt`. There is no waypoint, route-progress or pull term.

**Ladder.** The wing ladder in the sim (`WING_RUNGS`, level lift and spare wipes; probe runs step it) and the fade gate
`full_clear` 0.5.

**Evaluation.** Every 20M, 64 episodes, `sampled_every 4`; heldout `heldout: 16` (Wailing Caverns) every 4th evaluation,
the last and each new best; arms 32 and 32; partners `[group1_roles]`. Budget 1500M.
Headline: full_clear, cleared, wing_cleared_share, wing_wipes, boss_oggleflint, boss_taragaman, boss_jergosh,
boss_bazzalan, wing_rejoin_seconds, clear_allbot, clear_standin, standin_gap,
wing_rung; target full_clear >= 0.7. gamma 0.999.

**Unproven.** Never trained. **Tests.** None (removed 2026-10-07).

## 10. `dungeon3_deadmines`

**Purpose.** The Deadmines from the door to VanCleef at 17 to 20, every pull and side boss, doors and levers by real
interacts and the Iron Clad Door by the cannon and the gunpowder
carried from the door. The bar (the owner's, unchanged since 2026-10-02): 70% or more of the evaluation's runs cleared
with at most one wipe (`bar_clear`).

**Seed.** `dungeon2_ragefire`. **Place.** Arena `dungeon` (`InstanceRow 1`, 14400 s, `LevelFirst/Last` 17 to 20) and
`heldout` (Wailing Caverns). Bosses tracked: rhahkzor, sneed_shredder, sneed, gilnid, smite, greenskin, cookie,
vancleef.

**Rewards, ladders.** As D2; fade gate `bar_clear` 0.5; `convergence.measure: bar_clear`. Budget 2000M, every 20M, 64
episodes. Headline starts bar_clear, cleared, full_clear, wing_wipes, the boss reads, deaths_tank/healer/damage,
clear_allbot, clear_standin, standin_gap, wing_rung; targets bar_clear, cleared, full_clear, boss_vancleef >= 0.7,
wing_wipes <= 1.

**Unproven.** Never trained; the highest-risk stage (the first curriculum never reached the bar, at best about 10% in
evaluation).

---

## The nav stages (11 to 16): what they share

The vision movement curriculum ([../decisions/0036-pointnav-curriculum.md](../decisions/0036-pointnav-curriculum.md);
`.agents/plans/vision-movement-curriculum/`): **PointNav** -- a point goal whose position is always known and whose
route is not. The goal is the **compass block** (bearing sin/cos, distance/500, distance/40: the minimap marker, exact,
never withheld in a nav stage). The route is learned by two auxiliary heads in the learner's network -- a **costmap**
(48x48 traversability classes, cross-entropy against the sim's bake) and a **planner** (value iteration over the
costmap, the map crop and the goal; L1 against the bake's cost-to-go) -- whose readouts feed the policy's trunk and
its cell pointer; what the policy acts on is its own prediction, never the bake (plan decision 1).

* **The bake** (`Runtime/Vision/TravBake`, contract sec 1): a 2-yd grid over the arena's box (an instance's loaded
  grids without one), each cell classed by the player controller's rules (`Unknown, Free, Wall, Water, Hazard, StepUp,
  Jump, DropSafe, DropHurt, DropLethal, Slope`) with 8-neighbour edge costs in yards (a jump x1.5, swimming x1.5, a
  damaging drop + `PointNav.DropHurtYards` x the health share, no edge up past 1.64 yd or down past 69 yd). A Dijkstra
  from the goal once an episode gives `C(cell)`. Baked at the arena's first episode, cached under `trav/` beside the
  camera's scenes.
* **The draw** (`PointNavEncounter`, contract sec 2): a detour bin and a length bin uniform over the bins the rung
  opens (`PointNav.DetourBins`, `LengthBins`, `RungBins0-3`); the goal first, one Dijkstra, the start among the cells
  whose `C` and `C / straight` fall in the bins. `PointNav.ReplayShare` (20%) of training episodes replay a failure.
  Evaluations draw seeded pairs, no replay.
* **The sim pays** (all Outcome/Cost, no Shaping): Arrive 3.0 once (stopped within 1 yd, 2D, on the floor); StepCost
  0.002 per decision; Stuck and Wall 0.02/s (Seek's); FallHurt 2.0 x the share of max health a fall took; Death 3.0;
  Drowned 3.0; Lost 0.02/s while `C(body) > C(start) + 40`; Revisit 0.002/s on ground stood on before (nav4 on);
  Timeout 0. **The potential** `gamma * phi(next) - phi(cur)`, `phi = -min(C, 5000) * 0.01`, is paid by the learner
  with its own discount (0 on a done row, 0 when either C is unreachable), never faded; the sim's `reward_potential`
  column stays 0.
* **Labels in STEP** (protocol 31, contract sec 3.1): per agent 13,840 bytes after the map section -- fine and coarse
  costmap classes (48x48 u8 each, the crop's frame), fine and coarse cost-to-go (48x48 f16), then `C_body`, `C_start`
  and the goal in the body frame. Never in the observation.
* **Death**: the seat rises at its start point after `Respawn.DelayMs` and the episode goes on (`RespawnAtEntrance`).
* **The ladder**: difficulty alone -- the fade's rungs open the bins (`fade.rungs [1.0, 0.5, 0.25, 0.0]`), stepped on
  argmax `spl`, `gate_values [0.6, 0.7, 0.8]` (nav6: `follow_kept_share`); `convergence.measure spl`.
* **Evaluation**: every 10M, 256 episodes, the held-out `wailing` arena every evaluation, arms `no_camera`, `no_map`,
  `no_planner` (value readouts zeroed), `no_memory`; argmax scored, sampled reported beside it.
* **Level 1** everywhere (a death knight at 55), every race and class: the lesson is the movement alone.
* **Dungeons only** in this build: the camera's scene baker bakes whole maps, so a continent box is Phase B. Every
  arena is an emptied instance with its own replica per env; Wailing Caverns (43) is the held-out `wailing` arena of
  every stage and is never trained. Map ids: Stockades 34, Shadowfang Keep 33, Deadmines 36, Wailing Caverns 43,
  Razorfen Kraul 47, Blackfathom Deeps 48, Uldaman 70, Gnomeregan 90, Scarlet Monastery 189, Maraudon 349, Ragefire
  Chasm 389.
* **Spawn points**: the stage definition names one (the map's entrance area trigger, `Stages.cpp` `DungeonEntrance`)
  for the validation and `StageMaps`; the encounter draws every start from the bake's reachable `Free` cells. No
  bake box is authored yet for the Scarlet Monastery or Maraudon: the bake is what the entrance reaches (the
  Monastery's Graveyard wing, its four wings being disconnected inside; Maraudon from the orange wing's door).
* **Validation** (`Stages.cpp` `ArenaProblem`, amended 2026-10-13): a nav arena is one seat on a dungeon's map with
  the move, compass, vision and map blocks, `RespawnAtEntrance` and `MapTiers = 2`; `SightOnly`, `Revisit` and a box
  are nav arenas' only; `LabelsOnly` rides on a party follow with the compass and both map tiers, and is the one
  party follow allowed a compass. PointNav arenas draw their own spawns and do not count as fighting.

## 11. `nav1_sight`

**Purpose.** The controls and the stop: a point in line of sight (`SightOnly`); the costmap's free/wall basics.
The plan's intent is 5 to 60 yd and a start bearing opening to +-180 degrees.

**Seed.** None (`Extends = ""`): a fresh network, the second root of the curriculum.

**Place.** Arenas `stockades` (34, the hallways) and `ragefire` (389), weight 1 each, 60 s; `wailing` (43) held out.

**Blocks.** core, move, compass, vision, entities, map (two tiers), goal.

**Episode.** A seat at a bake cell facing a random way, the goal in sight; Arrive on the stop.

**Rewards.** Arrive 3.0; StepCost, Stuck, Wall, FallHurt, Death, Drowned, Lost (all as the shared list).

**Ladder.** `PointNav.RungBins0 "1,2"` to `RungBins3 "4,5"` (indexes into `DetourBins` "1.0,1.2,1.6,2.5,4.0" and
`LengthBins` "5,20,60,120,250,400"): rung 0 opens detour 1.0-1.2 and lengths 5-20 yd, rung 3 detour up to 2.5 and
lengths up to 250 yd; gates `spl` 0.6, 0.7, 0.8. **One global set:** the bins and rung tables are one `PointNav.*`
tuning loaded per worldserver, with no per-stage override family (only `Arena.<stage>.<arena>.Weight/WeightFinal/
StandInShare` are read by name), so every nav stage opens the same bins at the same rung; the plan's per-stage
ranges below are intent until a per-stage key or a stage-scaled reader exists (hand-back, 2026-10-13).

**Evaluation.** 256 seeded pairs every 10M; `wailing` every evaluation; the four arms.

**Status.** Headline: arrived, spl, time_ratio, stop_error_yards, wall_seconds, stuck_seconds, timed_out, died,
costmap_iou, planner_mae_known. Never trained.

## 12. `nav2_detour`

**Purpose.** The straight line is blocked in view: doors, pillars, room walls; local replanning. Lost joins the costs
(it is paid from nav1, but first matters here).

**Seed.** `nav1_sight`.

**Place.** `stockades` (34, the rooms), `deadmines` (36), `shadowfang` (33), 120 s; `wailing` held out.

**Blocks.** As nav1.

**Episode / Rewards / Ladder / Evaluation.** As nav1; the plan's intent is detour 1.0-1.2 opening to 2.5 and
20 to 120 yd (see the global-bins note under nav1).

**Status.** Headline adds detour_factor, lost_seconds. Never trained.

## 13. `nav3_vertical`

**Purpose.** Step up, jump or go round; safe against hurting and lethal drops; stairs, ramps, decks, storeys.

**Seed.** `nav2_detour`.

**Place.** `deadmines` (36: ramps, decks), `shadowfang` (33: stairs, towers), `gnomeregan` (90: levels, drops),
`razorfen` (47: ramps), 150 s; `wailing` held out.

**Blocks.** As nav1. The cell grid admits any height and unknown blocks (contract sec 3.3), so a storey above is
pressable; a bad pick is priced by `CellLost`.

**Rewards.** FallHurt and Death are the lesson's prices; jumps and falls are counted.

**Status.** Headline adds falls, fall_damage_share, jumps; targets falls <= 0.05 a trip, lethal-drop recall (the
costmap's `drop_lethal` IoU) >= 0.98. Never trained.

## 14. `nav4_beyond`

**Purpose.** The route is not in view: plan over the mental map's two tiers, commit to a direction, recognise a dead
end and leave it, on maps kept half-known across resets (`KeepShare`).

**Seed.** `nav3_vertical`.

**Place.** `gnomeregan` (90), `uldaman` (70), `scarlet` (189, the Graveyard wing: no box authored), `maraudon` (349,
no box authored), 300 s; `wailing` held out. All arenas `Revisit`.

**Rewards.** Revisit 0.002/s on ground stood on before joins the costs (`PointNav.Revisit`, by the arena flag).

**Ladder.** The plan's intent: lengths 100 to 400 yd, detour 1.3 to 4.0, a dead-end share 0 to 0.4 (the global-bins
note under nav1 applies: the committed `RungBins3` stops at detour 2.5 and 250 yd).

**Status.** Headline adds revisit_seconds, waypoint_regret_yards, waypoints_chosen; target spl >= 0.7 at detour >=
2.5. Never trained.

## 15. `nav5_water`

**Purpose.** Swim or go round; banks taller than a step are walls; the bed as a goal (breath); surfacing.

**Seed.** `nav4_beyond`.

**Place.** `blackfathom` (48), `deadmines` (36: the cove), `maraudon` (349: the pools), 180 s; `wailing` (its pools)
held out. `PointNav.BedShare` (30%) of the draws put the goal on a water cell's bed.

**Rewards.** Drowned 3.0 (underwater self-damage at death) joins the costs; `swim_seconds` is counted.

**Status.** Headline adds drowned, swim_seconds; target drowned <= 0.01. Never trained.

## 16. `nav6_follow`

**Purpose.** The moving point: `move4_follow`'s party (four learned followers and their group's leader, the stock-path
leader at a player's report cadence, decision 0035) with the compass on the leader's **believed position** (the party
frames' recall) and the planner routing to it; keep 3 to 10 yd, do not block. It supersedes `move4_follow` (plan
decision 8).

**Seed.** `nav5_water`; the party frames block starts fresh.

**Place.** `ragefire` (389) and `deadmines` (36), `Opposition::PartyFollow` with `LabelsOnly = true`, a party of five
(`PartySize = GROUP_MEMBERS`), 300 s; `wailing` held out.

**Blocks.** core, move, compass, vision, entities, map (two tiers), party_frames, goal.

**Episode.** `PartyFollowEncounter` places the leader, forms the group and pays the followers; PointNav runs beside it
as a **labeller only**: the leader's believed position is the point the compass shows and the C-map is computed to
(recomputed only when the leader's cell changes); it places nothing and pays nothing. A frozen-checkpoint leader
walking its own PointNav trips (plan I9) is a follow-up.

**Rewards.** PartyFollow's: FollowKept, Settled, Regroup (Outcome), Blocking, Lost, Death, Fidget, Drift (Cost),
Approach (Shaping); the learner's potential on the route distance to the believed position.

**Ladder.** PartyFollow's leader rungs; gates on `follow_kept_share`.

**Status.** Headline: follow_kept_share, settled_share, lost_seconds, blocking_seconds, died, and the aux stats.
Target follow_kept_share >= 0.8 argmax on `wailing`. Never trained. **Level:** the stage says 1, but
`PartyFollowEncounter::BeforeLevel` sets the episode level from the dungeon's band and that overrides a stage's
`Level` (`StageScenario.cpp` level pick), so nav6 runs at the band until the encounter is told otherwise.

---

## Observed issues

* `configs/move1_controls.yaml`: `fade.rungs: [0.0]` while the comments in the same block describe a path x0.5, x0.25,
  x0; the file is a snapshot of a finished ladder, not the design.
* `configs/move2_seek.yaml` comment says the noise prices "keep M1's cost ladder" gated on `found`; M1's `costs.enabled`
  is false and is inherited, so the cost ladder is off.
* (Fixed) `configs/combat1_fight.yaml` has no `extends` (it cannot extend `move3_interact` cleanly: its eval, gates and fade
  are the dungeon plan's) and used to reset `actor_lr`/`critic_lr` to 3e-4 and `look_entropy_coef` to 0.001, so M2's tuned
  values stopped at M4. It now sets 1.5e-4 and 0.004 itself, and C2 to D3 inherit them through their own `extends` chain.
* `Stages.cpp` comments for the `Opposition::Sight` enum (`StageDefinition.h`) say the ladder "withholds more and more
  often" and "PvE and PvP" (`ArenaDefinition` comment); the PvP half is gone.
* `RewardLedger.h`: `Threat` (the `WingCrowd` charge) and `HealingMana` are costs in meaning but categorised Shaping, so
  the fade removes them.
* The map data tables (hallways, rooms, sight pairs, sites) were authored by scripts under the gitignored
  `.agents/plans/*/tools` (`dungeon-curriculum/tools` is empty in this checkout); they cannot be regenerated from the
  repository.
* `DifficultyLadder` steps classes down on a score
  ([../decisions/0010-ladders-step-on-their-gate.md](../decisions/0010-ladders-step-on-their-gate.md)).
* The wing ladder's rung and the per-class tiers live in worldserver memory and are not in `latest.pt`;
  a restart returns them to `Instance.WingRungStart`/0 unless the conf names them
  (`StageScenario.cpp:302`, `AnimusForge.cpp:1084`). `UNVERIFIED`: whether forgectl resume sets them.
