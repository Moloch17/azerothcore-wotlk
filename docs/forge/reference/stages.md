# Reference: the twelve live stages

Purpose and scope: one section per live curriculum stage (`move1_controls` to `dungeon3_deadmines`), written from the
stage's definition in `src/server/game/Animus/Scenario/Curriculum/Stages/Stages.cpp`, its learner config
`apps/forge/python/configs/<stage>.yaml` (with its `extends:` chain merged as `animus/config.py` `load_yaml` does) and
the encounters and tuning it uses. It describes what the code does, not what a comment says; where they disagree the
text says so. Facts that could not be checked are marked `UNVERIFIED`. The chapter that explains how to run and tune a
stage is [../04-curriculum.md](../04-curriculum.md). Related references: [00-architecture.md](00-architecture.md),
[cpp-stagescenario.md](cpp-stagescenario.md), [cpp-encounters.md](cpp-encounters.md), [cpp-blocks.md](cpp-blocks.md),
[cpp-rewards-routing.md](cpp-rewards-routing.md), [cpp-tuning-keys.md](cpp-tuning-keys.md),
[config-yaml.md](config-yaml.md), [config-keys.md](config-keys.md), [metrics.md](metrics.md), [tests.md](tests.md),
[known-issues.md](known-issues.md), [glossary.md](glossary.md), [../decisions/README.md](../decisions/README.md).

## Map of the source files this document is written from

| Path | Lines | Role |
|---|---|---|
| `src/server/game/Animus/Scenario/Curriculum/Stages/Stages.cpp` | 1393 | The twelve `StageDefinition`s, the map data tables they use (hallways, rooms, objects, sight pairs, Deadmines sites) and the stage and arena validation |
| `src/server/game/Animus/Scenario/Curriculum/Stages/StageDefinition.h` | 331 | `StageDefinition`, `ArenaDefinition`, `Opposition`, the drills and `MAX_ARENAS` |
| `src/server/game/Animus/Scenario/Curriculum/CurriculumTuning.h` | 1185 | Every reward weight and draw parameter (`AnimusForge.Curriculum.*`) with its default |
| `src/server/game/Animus/Scenario/Curriculum/Rewards/RewardLedger.h` | 387 | `RewardTerm`, `RewardCategory` (Outcome, Cost, Shaping), the score and the scales |
| `src/server/game/Animus/Scenario/Curriculum/Encounters/SightEncounter.cpp` | see cpp-encounters.md | M1 |
| `.../Encounters/SeekEncounter.cpp` | | M2 |
| `.../Encounters/InteractEncounter.cpp` | | M3 |
| `.../Encounters/PartyFollowEncounter.cpp` | | M4 |
| `.../Encounters/CombatEncounter.cpp` | | C1 to C3 |
| `.../Encounters/RolesEncounter.cpp` | | G1 |
| `.../Encounters/InstanceEncounter.cpp` | 2752 | G2, D1, D2, D3 |
| `.../Encounters/WingLadder.h`, `StageScenario.h` (`WING_RUNGS`) | | The whole-dungeon difficulty ladder |
| `.../Encounters/DifficultyLadder.cpp` | 104 | The per-class ladder of C1 to C3 and G1 |
| `apps/forge/python/configs/*.yaml` | 13 files, 1351 | One learner config per stage, plus the `fast.yaml` overlay |
| `apps/forge/python/animus/stage.py` | 829 | `ShapingFade`, `CostLadder`, `ConvergenceController` |
| `apps/forge/python/animus/config.py` | 853 | Config dataclasses and their defaults |
| `src/test/server/game/Animus/LiveLayoutPinTest.cpp` and `LiveLayoutPin.golden.inc` | 219, 149 | Pins every live stage's layout |

(The other encounters' and tuning files' line counts are in the cpp-* documents.)

## Overview of all twelve

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
| 9 | `group2_corridor` | Ragefire and Deadmines, real packs | band | 5 | Instance (corridor) | wing ladder + fade (`cleared` 0.6) | 500M | never |
| 10 | `dungeon1_pulls` | Ragefire, real packs | band + up to 2 | 5 | Instance (pull drill) | drill ladder + wing + fade (`cleared` 0.7) | 300M | never |
| 11 | `dungeon2_ragefire` | Ragefire, door to Bazzalan | band | 5 | Instance (whole wing) | wing ladder + fade (`full_clear` 0.5) | 1500M | never |
| 12 | `dungeon3_deadmines` | Deadmines, door to VanCleef | 17 to 20 | 5 | Instance (whole wing) | wing ladder + fade (`bar_clear` 0.5) | 2000M | never |

The queue order is the table's order (`InDefaultQueue` is true for all, `StageDefinition.h`). Budgets are ceilings, not
targets (see [../04-curriculum.md](../04-curriculum.md)).

### Seed chain

`StageDefinition::Extends` (and `Merges`) in `Stages.cpp` is the seed chain; the sim writes it into `stage.json` as
`seed_chain` and `merges`, and the learner seeds from it (`animus/stages.py` `seed_chain`,
`TrainConfig.resolved_init_from`
with `init_from: auto`, `seed_from: latest` in every live yaml: `test_seed_from.py`).

```
move1_controls
 └─ move2_seek
     ├─ move3_interact
     │   └─ combat1_fight
     │       └─ combat2_packs
     │           └─ combat3_survive ──(merges move4_follow's party frames)─┐
     │                                                                      v
     └─ move4_follow ─────────────────────────────────────────────────> group1_roles
                                                                            └─ group2_corridor
                                                                                └─ dungeon1_pulls
                                                                                    └─ dungeon2_ragefire
                                                                                        └─ dungeon3_deadmines
```

`extends:` at the top of a yaml is the separate **config chain**. They differ: `combat1_fight.yaml` has no `extends`
(it repeats the whole MAPPO block; `Observed issues` below), `move4_follow.yaml` and `move3_interact.yaml` extend
`move2_seek.yaml`, `combat2_packs` extends `combat1_fight`, and so on in the order of the table.

What carries over: networks are seeded block by block, by name (`animus/bootstrap.py`): kept blocks' input columns and
action rows move to where the block sits now, new blocks start at zero, trunk copied, critic state encoder and value
head
fresh, value normaliser not copied. The merge in `group1_roles` seeds the blocks only `move4_follow` has (the party
frames) from its `latest.pt`, input columns and action rows only. Details and the revision rules:
[py-learner.md](py-learner.md),
[../decisions/0012-seeding-by-name-append-only-classes.md](../decisions/0012-seeding-by-name-append-only-classes.md).

### Blocks of each stage (effective)

`CurriculumStages()` inserts the `Entities` block right after `Vision` in every stage that has a camera (`Stages.cpp`,
end of the file), so the lists below include it. Widths are the pinned class-independent totals of
`LiveLayoutPin.golden.inc` (observation, actions). `Duel`, `Pet` and `Core` are class dependent.

| Stage | Blocks in layout order | Total obs, actions |
|---|---|---|
| `move1_controls` | core, move, compass, vision, entities, goal | 842, 25 |
| `move2_seek` | core, move, vision, entities, map, goal | 840, 25 |
| `move3_interact` | core, move, vision, entities, map, sight, goal | 2911, 346 |
| `move4_follow` | core, move, vision, entities, map, party_frames, goal | 924, 37 |
| `combat1_fight`, `combat2_packs` | core, move, duel, pet, vision, entities, map, sight, combat, goal | 3779, 350 |
| `combat3_survive` | the above plus gauntlet (after pet) | 3790, 353 |
| `group1_roles` | the above plus party_frames (after sight) | 3874, 365 |
| `group2_corridor` to `dungeon3_deadmines` | core, move, duel, pet, pack, gauntlet, vision, entities, map, sight, party_frames, combat, goal | 4908, 390 |

Block ids: core 0, move 1, compass 2, duel 3, pack 4, gauntlet 5, pet 11, vision 20, entities 21, map 22, sight 23,
party_frames 24, combat 25, goal 26 (`Layout/Block.h`; never renumbered). What each block is:
[cpp-blocks.md](cpp-blocks.md).
The layout pin test covers all twelve (`LiveLayoutPinTest.LiveStageLayoutsAreUnchanged`).

### Common to every stage

* **Decisions and ticks.** `AnimusForge.DecisionMs` 250 and `AnimusForge.Stage.<name>.TicksPerDecision = 5` for every
  live stage (`worldserver.conf.dist` lines 5202 to 5216), i.e. 50 ms world ticks (`test_stage_ticks.py`).
* **Ladder machinery.** `fade` is the shaping ladder (`ShapingFade`, `animus/stage.py`); in this curriculum it is also
  the
  sim's difficulty ladder, because the sim reads the fade's scale as the rung for M1 to M4. `costs` is the cost ladder
  and is **off in all twelve** (`costs.enabled: false`, set in M1 and inherited, or set explicitly). Every gate-stepped
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
  (`lr_final_fraction`), recurrent_size 128, target_kl 0.02, minibatches 4, `per_layout_advantages`, `seat_sets: false`
  (entity sets are off in every live yaml, yet the sim still writes them for the layouts with a pack block),
  rollout_length
  128, `overlap_updates: true`, convergence `patience 3, window 4, z 2.0, kl 0.003, entropy_slope 0.01, hold_share 0.02`
  for M1 and C1 explicitly and as defaults elsewhere.

### Learner hyperparameters that differ

| Stage | gamma (per 100 ms) | epochs | actor and critic lr | look_entropy_coef | chunk_length | other |
|---|---|---|---|---|---|---|
| `move1_controls` | 0.997 | 4 | 1.5e-4 | 0.001 | 32 | vision_chunk_rows 0 |
| `move2_seek` | 0.998 | 2 | 1.5e-4 | 0.004 | 128 | vision_chunk_rows 2048, `map_vin: false` |
| `move3_interact` | 0.998 (inherited) | 2 | 1.5e-4 | 0.004 | 128 | |
| `move4_follow` | 0.999 | 2 | 1.5e-4 | 0.004 | 128 | |
| `combat1_fight` | 0.998 | 2 | 3e-4 | 0.001 | 128 | standalone yaml |
| `combat2_packs` | 0.999 | 2 | 3e-4 | 0.001 | 128 | |
| `combat3_survive` | 0.999 | 2 | 3e-4 | 0.001 | 128 | |
| `group1_roles` | 0.999 | 2 | 3e-4 | 0.001 | 128 | |
| `group2_corridor` | 0.999 | 2 | 3e-4 | 0.001 | 128 | `sil_coef: 0.0`, `goal_entropy_final_fraction: 0.5`, `explore.enabled: false` |
| `dungeon1_pulls` | 0.998 | 2 | 3e-4 | 0.001 | 128 | |
| `dungeon2_ragefire` | 0.999 | 2 | 3e-4 | 0.001 | 128 | `explore.enabled: true, share 0.5` |
| `dungeon3_deadmines` | 0.999 (inherited) | 2 | 3e-4 | 0.001 | 128 | |

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
mask) with chance `Withhold0..3` = 0, 0.25, 0.6, 0.9 at fade scales 1, 0.5, 0.25, 0 (`SightDraw::Rung` takes the nearest
of
the four scales).

**Rewards** (`SightEncounter::Reward`, `SightEncounter.cpp:319` to `416`). Outcome: `Arrive` 3.0 once on the stop
(`Markers.Arrive`). Cost: `StepCost` 0.002 per 50 ms (about 0.04 a second; `Markers.StepCost`), `Death` 3.0
(`Markers.Death`), `Stuck` and `Wall` at their own fixed price 0.02 per second each (`Controls.Stuck`, `Controls.Wall`,
`WallSlide` 0.5; `AddFixed`, so off the cost ladder). Shaping: `Progress` 1.0 on the straight distance, `Facing` 0.25 on
the cosine of the object's bearing (`Markers.Progress`, `Markers.Facing`), both scaled by the fade.

**Ladder.** One fade with the compass withholding riding on it. As configured (`move1_controls.yaml`) `fade.rungs` is
`[0.0]`: a single rung, so a resumed M1 sits at scale 0 (compass withheld 90%, no shaping). The comments in the yaml
describe the earlier path x0.5, x0.25, x0 and say the fade "starts at x0.5"; the code does not (Observed issues).
`gate_metric: arrived_at_rung`, `gate_value: 0.8`, `window 3`, `give_up 2`; `require_plateau` is the default **true**
here
(the only live stage where it is), but with one rung nothing steps. `costs.enabled: false`; `entropy_floor.fraction
0.3`.

**Evaluation.** Every 5M steps, 512 episodes, `sampled_every: 3` (every third evaluation also scores sampled actions),
`trace_episodes: 64`. The sim plays 32 fixed (spawn, object) pairs (24 in sight, 8 round a corner,
`StockadeSightPairs()`), each with the compass and without, spread over every class and race. No arms and no heldout.

**Status.** Headline: arrived, arrived_no_compass, arrived_with_compass, compass_withheld, arrive_seconds_sight,
time_ratio_sight, time_ratio_corner, stop_distance, overshoot, stops_near, course_kinks, control_changes_per_minute,
wall_seconds, timed_out, died. Targets: arrived >= 0.95, arrive_seconds_sight <= 18, time_ratio_sight <= 1.1,
stop_distance <= 0.5, overshoot <= 0.5, stops_near <= 1.2, course_kinks <= 5, wall_seconds <= 0.5, timed_out <= 0.02,
died <= 0.01. `convergence.measure: arrived`. `layout_sampling.metric: arrived`, `replay_fraction 0.2`.

**Key conf knobs.** `Controls.*` (Nearest, Furthest, CornerShare, CornerFrom, Withhold0..3, Stuck, Wall), `Markers.*`.

**Status of training.** Trained (it is the first stage and its run finished before M2 began). Its checkpoint is the
seed of everything. `UNVERIFIED`: its final numbers (look in `var/animus-forge/shared/archive`).

**Tests.** `test_m1_sight.py`, `SightEncounterTest.cpp`, `CompassBlockTest.cpp`, `StockadeHallwaysDataTest.cpp`,
`MoveControlsTest.cpp`, `test_compass_split.py`, `LiveLayoutPinTest.cpp`.

## 2. `move2_seek`

**Purpose.** Find a hidden object by sight alone: no compass, the camera's objective flag shows it only in line of
sight.
Teaches search, memory of where it has looked, and the mental map. It follows M1 because it needs M1's camera and legs,
and
it precedes everything that perceives and remembers entities.

**Seed.** `move1_controls`: the move block's columns carry by name, the camera carries, the compass's weights stay
behind
and the map block starts fresh (its join at zero). `seed_from: latest`.

**Place.** The same emptied Stockades; seat at a random hallway point. Arenas: `rooms` (weight 1, 300 s) and `sweep`
(`EvalOnly`, never in training, 300 s). 39 rooms (`StockadeRooms()`), five objects.

**Blocks.** core, move, vision, entities, map, goal.

**Episode.** One object placed by the ladder: hallway in sight of the spawn (rung 0), just inside a front cell's opening
(1), anywhere in a front cell (2), deep (3). Each rung keeps `Seek.CarryShare` 0.1 of the one below. Episode seconds by
rung
`RungSeconds0..3` = 90, 120, 200, 300. Found is stopping within `SeekRadius` 3.0 yd, on the floor (`ArriveRise` 2.0).

**Rewards** (`SeekEncounter.cpp:441` to `550`). Outcome: `Arrive` 3.0 (`Seek.Arrive`). Cost: `StepCost` 0.0005 per 50 ms
(a 300 s episode costs 3.0), `Death` 6.0, `Stuck` and `Wall` 0.02 fixed. Shaping: `Sighting` 0.5 once, `NewGround` 0.004
per
4 yd cell first walked, `RoomSeen` 0.1 once per room whose floor the camera first shows (3 rays).

**Ladder.** `fade.rungs [1.0, 0.5, 0.25, 0.0]` (hallway, doorway, front room, deep), `gate_metric found`, `gate_value
0.8`,
`require_plateau: false`. The comment in the yaml says the other noise prices "keep M1's cost ladder", but M1's
`costs.enabled` is false and is inherited, so the cost ladder is off (Observed issues).

**Evaluation.** Every 10M, 78 episodes at the training rung (each room in turn, objects cycled). Heldout `sweep: 195`
(every
(room, object) pair once, 39 x 5, at the top rung), `heldout_every: 1000` (so only the stage's last evaluation) and
`heldout_on_best: false`.

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

**Tests.** `SeekEncounterTest.cpp`, `SeekFlagTest.cpp`, `SeekTest.cpp`, `StockadeRoomsDataTest.cpp`,
`MentalMapTest.cpp`,
`test_seek_metrics.py`, `test_mental_map.py`, `test_rung_rebaseline.py`, `test_shaping_fade.py`.

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

**Unproven.** Never trained. Its sites are authored tables validated by `DeadminesSitesDataTest`; the authoring script
is not
in the repository (Observed issues).

**Tests.** `InteractStageTest.cpp`, `DeadminesSitesDataTest.cpp`, `SightBlockTest.cpp`, `EntityMemoryTest.cpp`,
`test_interact.py`, `test_sight.py`.

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

**Ladder.** `fade.rungs [1.0, 0.5, 0.25, 0.0]` = a slow steady leader (walking), running, sudden stops, sudden stops and
steps back (`PartyFollow.WalkRungs 1, SuddenFromRung 2, BackStepFromRung 3`); gate `follow_kept_share` 0.75,
`require_plateau: false`; `costs.enabled: false` with its gate removed.

**Evaluation.** Every 10M, 64 episodes (fixed routes, seeded), `heldout: null` (M2's sweep arena does not exist here).

**Status.** Headline: follow_kept_share, regroup_share, regroup_seconds, lost_seconds, blocking_seconds, deaths,
rejoin_seconds,
rejoined, leader_route_share, difficulty, wall_seconds, died. Targets: follow_kept_share >= 0.9, regroup_share >= 0.9,
regroup_seconds <= 5, lost_seconds <= 5, blocking_seconds <= 3, rejoined >= 0.9, wall_seconds <= 2, died <= 0.02.
`convergence.measure: follow_kept_share`.

**Unproven.** Never trained. The scripted leader is the one remaining script ([decision
0002](../decisions/0002-no-scripted-teachers.md)).

**Tests.** `PartyFollowTest.cpp`, `test_move4_follow.py`, `test_party_frames_seeding.py`.

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

**Status.** Headline: won, survived, kills, kill_seconds, hurt_share, deaths, rejoin_seconds, combat_rung,
target_in_view,
selected_share, ally_deaths. Targets: won >= 0.9, survived >= 0.9, kills >= 3, kill_seconds <= 25, hurt_share <= 0.8,
target_in_view >= 0.8, ally_deaths <= 0.1. `convergence.measure: won`.

**Unproven.** Never trained. **Tests.** `CombatPerceptionTest.cpp`, `test_combat_stages.py`, `test_sight.py`.

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
hurt_share, deaths, rejoin_seconds, combat_rung, target_in_view; targets: won/survived >= 0.85, packs_cleared >= 3,
extra_pulls <= 0.1, fire_share <= 0.1, hurt_share <= 1.5, target_in_view >= 0.8, interrupt_earnings <= 0.3 (a watch that
`InterruptLanded` is not farmed). gamma 0.999. 300M, every 10M, 240 episodes.

**Unproven.** Never trained. **Tests.** `test_combat_stages.py`, `CombatPerceptionTest.cpp`.

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
combat_rung;
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

**Unproven.** Never trained. **Tests.** `RolesStageTest.cpp`, `test_group1_roles.py`, `test_partners.py`,
`StandInTest.cpp`.

## 9. `group2_corridor`

**Purpose.** Real dungeon ground: four of a wing's packs in route order a run (pull, fight, rest, ready, next), in
Ragefire and the
Deadmines, a fresh instance a run with the world database's packs and patrols.

**Seed.** `group1_roles` (every block carries but the pack block, which starts fresh).

**Place.** Arenas `ragefire` (`InstanceRow 0`, 900 s) and `deadmines` (`InstanceRow 1`, 1200 s, `LevelFirst/Last` 17 to
20), both
`CorridorPacks = 4`, `StandInShare` 20 (`DUNGEON_STAND_IN_SHARE`). The packs before the first are cleared as a party
that came from
the door would have left them; the first pack is drawn each run and, in an evaluation, taken from the seed.

**Rewards** (`InstanceEncounter::Reward`, `InstanceEncounter.cpp:2530` to `2700`; tier scale = the wing ladder rung
capped at
`Instance.MaxTierScale` 6, Outcome multiplied, Cost divided). Outcome: `Clear` `CorridorPack` 4.0 per pack in route
order,
`ReadyPull` `WingEngage` 1.0 per pull started with everyone at 80% health and mana (once per route pack), `Kill`
`WingTrashKill`
1.0. Cost: `PullExtra` `WingChainPull` 3.0, `Idle` `WingStall` 0.1 per second after 60 s with no progress (others 0.2 of
it),
`Lost` `WingStray` 0.02 per second beyond 25 yd of the leader, `Away` `WingAway` 0.02, `Death` `WingDeath` 3.0 and
`WingWipe` 5.0,
`StepCost` `WingClock` 0.002 per second, `Timeout` `WingTimeout` 30 x share left. Shaping: `Approach` (waypoints 0.5,
route progress
`WingProgress` 60), `Threat` (`WingCrowd` 0.15 per second per hostile past 4 on the party; note this is a cost paid as
Shaping and so fades),
`DamageDealt`.

**Ladder.** Wing ladder in the sim (`WING_RUNGS`, 9 rungs, level lift 8 to 0 and spare wipes 4 to 0,
`StageScenario.h:218`): a fifth of
training runs are probes (`WingProbe 0.2`); once 40 probes at a rung average 0.6 of the dungeon the ladder steps down
one rung. It never
steps back and raises a collapse alarm (3 reads under 0.1 or a quarter of the rung below) but has no stall alarm.
Learner fade
`[1, 0.5, 0.25, 0]` gate `cleared` 0.6, `require_plateau: false`. An evaluation plays the last rung (band levels, no
spare wipe).

**Evaluation.** Every 10M, 128 episodes; arms `with_human 64`, `with_partners 64`, `arms_every 2`; partners
`[group1_roles]`.
`status.excluded.death_knight`. Headline: cleared, corridor_share, corridor_in_order, chain_pulls, ready_pulls,
wing_wipes,
wing_rejoin_seconds, clear_allbot, clear_standin, standin_gap, deaths_tank, deaths_healer, wing_rung; targets cleared >=
0.7,
corridor_in_order >= 3, chain_pulls <= 0.25, wing_wipes <= 0.3, standin_gap <= 0.1.

**Unproven.** Never trained. **Tests.** `DungeonStagesTest.cpp`, `WingLadderTest.cpp`, `test_dungeon_stages.py`.

## 10. `dungeon1_pulls`

**Purpose.** Clean pulls on real geometry: Ragefire, one pack a run, the party 35 yd back along the route with the packs
before it cleared.

**Seed.** `group2_corridor` (same layout). **Place.** Arena `ragefire`, `PullDrill`, 180 s, `InstanceRow 0`. Seats at
most `PullLift` 2 levels above
the dungeon's range.

**Rewards.** Outcome: `PullClean` 5.0 (the tank in full, others `PullOthers` 0.5), `ReadyPull`. Cost: `PullExtra` 5.0
(ends the run),
`Timeout` `PullTimeout` 2.0, `Idle` after `PullGraceMs` 20 s, `Lost`, `Death`, clock.

**Ladder.** The drill's own ladder opens packs by how far the nearest other pack stands, 30, 22, 14 yd, then any
(`PULL_GAPS`), once 100 drills
at the newest rung pull clean 70% (`PullRungRuns 100`, `PullRungTarget 0.7`); each machine climbs its own. Fade gate
`cleared` 0.7.
Evaluations drill too (`EvaluatesDrills`); seed i drills pack i mod packs at the dungeon's own levels.

**Evaluation.** Every 10M, 192 episodes. Headline: cleared, drill_clean, drill_extra, drill_pulled, ready_pulls,
wing_wipes, clear_allbot,
clear_standin, standin_gap, deaths_tank, pull_rung; targets cleared >= 0.8, drill_extra <= 0.1, wing_wipes <= 0.1,
standin_gap <= 0.1.
gamma 0.998. **Unproven.** Never trained. **Tests.** `DungeonStagesTest.cpp`, `test_dungeon_stages.py`.

## 11. `dungeon2_ragefire`

**Purpose.** A full clear of Ragefire from the door to Bazzalan: every pull and side boss (Oggleflint, Taragaman,
Jergosh), deaths and rejoins in play.

**Seed.** `dungeon1_pulls`. **Place.** Arena `dungeon` (`InstanceRow 0`, 7200 s) and `heldout` (Wailing Caverns,
`InstanceRow 2`, `EvalOnly`, weight 0, 10800 s, door to Lord Serpentis).
`Instance.WingWipes` 2: the first wipe is scored and the party rises at the entrance, the second ends the run.

**Rewards.** Outcome: `Kill` (`WingMidBoss` 8.0 per boss on the way, `WingBoss` 25.0 the last, `WingTrashKill` 1.0),
`Clear` `WingClear` 25.0 for the full clear, `ReadyPull`.
Cost as group2 (`PullExtra` chain pulls 3.0, `Idle`, `Lost`, `Away`, `Death`, `StepCost`, `Timeout`). Shaping
`Approach`, `Threat`, `DamageDealt`.

**Ladder.** Wing ladder (see 9) and fade gate `full_clear` 0.5. Go-Explore: `explore.enabled`, `share 0.5` of training
runs start from a cell an earlier run reached; an evaluation starts at the door.

**Evaluation.** Every 20M, 64 episodes, `sampled_every 4`; heldout `heldout: 16` (Wailing Caverns) every 4th evaluation,
the last and each new best; arms 32 and 32. Budget 1500M.
Headline: full_clear, cleared, wing_cleared_share, wing_wipes, boss_oggleflint, boss_taragaman, boss_jergosh,
boss_bazzalan, chain_pulls, wing_rejoin_seconds, clear_allbot, clear_standin, standin_gap,
wing_rung; target full_clear >= 0.7. gamma 0.999.

**Unproven.** Never trained. **Tests.** `DungeonStagesTest.cpp`, `WingLadderTest.cpp`, `test_explore.py`,
`test_heldout.py`.

## 12. `dungeon3_deadmines`

**Purpose.** The Deadmines from the door to VanCleef at 17 to 20, every pull and side boss, doors and levers by real
interacts and the Iron Clad Door by the cannon and the gunpowder
carried from the door. The bar (the owner's, unchanged since 2026-10-02): 70% or more of the evaluation's runs cleared
with at most one wipe (`bar_clear`).

**Seed.** `dungeon2_ragefire`. **Place.** Arena `dungeon` (`InstanceRow 1`, 14400 s, `LevelFirst/Last` 17 to 20) and
`heldout` (Wailing Caverns). Bosses tracked: rhahkzor, sneed_shredder, sneed, gilnid, smite, greenskin, cookie,
vancleef.

**Rewards, ladders.** As D2; fade gate `bar_clear` 0.5; `convergence.measure: bar_clear`. Budget 2000M, every 20M, 64
episodes. Headline starts bar_clear, cleared, full_clear, wing_wipes, the boss reads, deaths_tank/healer/damage,
chain_pulls,
clear_allbot, clear_standin, standin_gap, wing_rung; targets bar_clear, cleared, full_clear, boss_vancleef >= 0.7,
wing_wipes <= 1.

**Unproven.** Never trained; the highest-risk stage (the first curriculum never reached the bar, at best about 10% in
evaluation).

---

## Observed issues

* `configs/move1_controls.yaml`: `fade.rungs: [0.0]` while the comments in the same block describe a path x0.5, x0.25,
  x0; the file is a snapshot of a finished ladder, not the design.
* `configs/move2_seek.yaml` comment says the noise prices "keep M1's cost ladder" gated on `found`; M1's `costs.enabled`
  is false and is inherited, so the cost ladder is off.
* `configs/combat1_fight.yaml` has no `extends`, repeats the whole MAPPO block and sets `actor_lr` 3e-4 and
  `look_entropy_coef` 0.001, so M2's tuned values (1.5e-4, 0.004) stop at M4; its comment "As M1 and M2" is stale for
  look entropy.
* `tests/test_combat_stages.py::test_the_seed_chain_runs_from_m2` asserts C1 extends `move3_interact`; the name is
  stale.
* `Stages.cpp` comments for the `Opposition::Sight` enum (`StageDefinition.h`) say the ladder "withholds more and more
  often" and "PvE and PvP" (`ArenaDefinition` comment); the PvP half is gone.
* `RewardLedger.h`: `Threat` (the `WingCrowd` charge) and `HealingMana` are costs in meaning but categorised Shaping, so
  the fade removes them.
* The map data tables (hallways, rooms, sight pairs, sites) were authored by scripts under the gitignored
  `.agents/plans/*/tools` (`dungeon-curriculum/tools` is empty in this checkout); they cannot be regenerated from the
  repository.
* `DifficultyLadder` has no GTest and steps classes down on a score
  ([../decisions/0010-ladders-step-on-their-gate.md](../decisions/0010-ladders-step-on-their-gate.md)).
* The wing ladder's rung, the drill rung and the per-class tiers live in worldserver memory and are not in `latest.pt`;
  a restart returns them to `Instance.WingRungStart`/`PullRungStart`/0 unless the conf names them
  (`StageScenario.cpp:302`, `AnimusForge.cpp:1084`). `UNVERIFIED`: whether forgectl resume sets them.
