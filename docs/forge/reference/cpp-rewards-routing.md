# Rewards, the reward ledger, and routing data (C++)

Purpose and scope. The reward ledger and its term kinds (`Rewards/`), what the simulator does with the learner's fade
and cost scales, tier scaling, and the routing/field data (`Routing/` plus the field files in `Blocks/LayeredField.*`).
Encounters that pay the terms are documented in [cpp-encounters.md](cpp-encounters.md); tuning keys in
[cpp-tuning-keys.md](cpp-tuning-keys.md) and [config-keys.md](config-keys.md); metrics in [metrics.md](metrics.md);
learner-side fade and cost ladders in [py-learner.md](py-learner.md) and [py-mappo.md](py-mappo.md). Blocks and
layout: [cpp-blocks.md](cpp-blocks.md), [cpp-layout-character.md](cpp-layout-character.md). Stages:
[stages.md](stages.md).

Paths are relative to `src/server/game/Animus/Scenario/Curriculum/` unless they start with `src/`. Lines are from
commit `bd32b9dc8`.

## Map table

| Path | Lines | Role |
| --- | --- | --- |
| Rewards/RewardLedger.h | 387 | `RewardTerm` (53 terms), `RewardCategory`, `RewardLedger`. |
| Rewards/CombatReward.h | 45 | `CombatReward::DesiredRange`, `TierScale`. |
| Rewards/CombatReward.cpp | 91 | `RewardTermName` for every term (not combat specific), `DesiredRange`. |
| Routing/FieldGrids.h | 58 | Grid index and the grids a stage covers. |
| Routing/FieldGrids.cpp | 91 | Implementation (reads `mmaps/` file names). |
| Routing/FieldRoute.h | 58 | A* over the layered field: `Plan`, `Covers`, `Report`. |
| Routing/FieldRoute.cpp | 341 | The search. |
| Routing/RoutePlanner.h | 116 | Navmesh corner routes (`Route`, `RoutePlanner`). |
| Routing/RoutePlanner.cpp | 285 | Detour queries per thread. |
| Routing/FloorScan.h | 113 | Pure classification for `forge floorscan`. |
| Routing/RouteShortcut.h | 150 | Pure corner/door helpers for dungeon routes. |
| Blocks/LayeredField.h | 126 | Layered height field type, file I/O, store. |
| Blocks/LayeredField.cpp | 478 | Bake, file format, cache. |

## RewardLedger

`class RewardLedger` (`Rewards/RewardLedger.h:287`), one per seat (`SeatState::Rewards`).

| Member | Behaviour |
| --- | --- |
| `Add(term, value, tier = 1)` | Score gets `value` if the term is Outcome or Cost. Paid amount = `value * scale[term] * tier * Shaped(term)`. `Shaped` is the shaping scale for Shaping terms, the cost scale for noise prices (`PricesNoise`), else 1. Returns the amount paid. |
| `AddFixed(term, value)` | Same but a noise price is not multiplied by the cost scale and `tier` is not applied. Used for `Stuck` and `Wall` ("on from the first step, never free"). |
| `AddTaken(term, value)` | Episode sum only (a goal reached seen at the next observation is paid into the previous decision's row by the caller). |
| `Scale(term, factor)` | Per-seat per-term factor, default 1. Used: `DamageDealt` for group healers (`StageScenario.cpp:4920`). |
| `SetShaping(s)`, `SetCosts(s)` | Set from the learner's PROGRESS message each `Reward()` call on every learned seat (`StageScenario.cpp:4681`). |
| `TakeStep()` | Returns and clears this decision's total. |
| `Episode(term)`, `Score()`, `ResetEpisode()` | Episode sums per term (these are the `reward_<name>` info columns), the score, reset. |

Score rule: the score is Outcome plus Cost terms as tuned, before tier and role scaling, before the shaping and cost
scales; evaluation, `best.pt` and the league judge on it. Everything else is Shaping.
`PricesNoise(term)` = Repeat, Jitter, Aimless, Effort, Fidget, Stuck, Wall: the terms the learner's cost ladder scales.
`static_assert(EveryRewardTermCategorised())` forces a category for each term. `stage.json reward_terms` publishes every
term's category (`StageScenario.cpp:1448`).
Tests: `RewardLedgerTest` (score ignores tier and role, shaping scale touches only shaping, cost scale only noise
prices, every term has a name and category, drill lessons are outcomes), plus encounter tests that check term kinds
(`CombatStagesTest.TheirTermsAreOutcomeAndCost`, `RolesStageTest.TheDrillTermsAreOutcomesAndCosts`,
`PartyFollowTest.TheTermsAreOutcomeAndCost`).

### Fade ladder and cost ladder, simulator side

The learner sends `ProgressMsg` after every update (`Bridge/Protocol.h:196`): progress (0..1, arena weights), the
shaping
scale and the cost scale. `AnimusForge.cpp:2636-2660` clamps both to [0, 1] (NaN becomes 1, said once) and calls
`SetShapingScale` / `SetCostScale`, stored as atomics in `StageScenario` (`:4637`, `:4645`). `Reward()` copies them onto
every seat ledger before anything is paid. A Shaping term is thus paid times the fade rung; a noise price times the cost
rung; Outcome and the non-noise Costs are never scaled. The rungs and their stepping rules are learner code. The tier
scale: `CombatReward::TierScale(step, tier) = 1 + max(0, step) * tier` with `step = Difficulty.TierScale` (0.25). Combat
and roles encounters multiply Outcome terms by `w` and divide Cost losses by it (`CombatEncounter.cpp:576`,
`RolesEncounter.cpp:560`); the wing passes it as the ledger's `tier` argument (`InstanceEncounter.cpp:2534`,
capped by `Instance.MaxTierScale` 6, `:2731`), so the stored score is unchanged by it.
`CombatReward::DesiredRange(seat, duel)` returns `Duel.MeleeRange` or `Duel.RangedRange` by the spec's `RangeBand`
(used by `StageScenario.cpp:2771` and `:3789`).

Reviewer notes: `Rewards/CombatReward.cpp` holds `RewardTermName` for all terms and `CombatReward.h` holds only
two functions; the file name no longer fits (rename suggestion: `RewardTerms.cpp`). The brief mentioned 63 remaining
terms; the enum has 53 (a script count and a hand count agree; see the table). `RewardLedger::Add`'s doc says no
term is potential-based; `GoalProgress` is potential-style but explicitly Shaping.

## The 53 reward terms

Columns: kind (O outcome, C cost, S shaping, N = also a noise price); who pays; tuning key (config suffix under the
curriculum prefix `AnimusForge.Curriculum.`) and default; live stages. Payer = the code that calls `Add`. "Scenario"
= `StageScenario::SeatReward`, which runs for every seat of every stage (`StageScenario.cpp:4989-5060`, `4578`). Stages
are abbreviated m1..m4 (move1_controls..move4_follow), c1..c3 (combat1_fight..combat3_survive), g1 (group1_roles), g2
(group2_corridor), d1..d3 (dungeon1_pulls..dungeon3_deadmines). The encounter-to-stage map is the arena `Against` of
each
stage (`Stages.cpp`): Sight m1, Seek m2, Interact m3, PartyFollow m4, Combat c1-c3, Roles g1, Instance g2 d1 d2 d3;
`PartyEncounter` is added for any stage with a `PartyGroup` arena (g1, g2, d1-d3).

| Term (info column `reward_<name>`) | Kind | Paid by | Tuning key = default | Stages |
| --- | --- | --- | --- | --- |
| DamageDealt `damage_dealt` | S | Combat (`Combat.Damage` * share of target health), Roles (`Roles.Damage`); Party (as the focus term of a non-drilled seat, `PartyEncounter.cpp:450`); scaled 0 for a group healer | Combat.Damage = 0.3; Roles.Damage = 0.3 | c1-c3, g1, g2, d1-d3 |
| StepCost `step_cost` | C | Seek (`Seek.StepCost` per decision scale), Sight (`Markers.StepCost`), Interact (`Interact.StepCost`), Combat (`Combat.Clock`), Roles (`Roles.Clock`), Instance (`Instance.WingClock` per second) | Seek.StepCost 0.0005; Markers.StepCost 0.002; Interact.StepCost 0.0005; Combat.Clock 0.01; Roles.Clock 0.01; Instance.WingClock 0.002 | m1, m2, m3, c1-c3, g1, g2, d1-d3 |
| Approach `approach` | S | Instance: waypoints (`Instance.WingWaypoint`) and route progress high-water (`Instance.WingProgress`) times tier | WingWaypoint 0.5; WingProgress 60 | g2, d1-d3 |
| Kill `kill` | O | Combat (`Combat.Kill` * w per kill); Instance (`WingTrashKill`, `WingMidBoss`, `WingBoss` times tier) | Combat.Kill 1; WingTrashKill 1; WingMidBoss 8; WingBoss 25 | c1-c3, g2, d1-d3 |
| Clear `clear` | O | Combat (`Combat.Clear` * w); Roles (`Roles.Clear` * w); Instance (`CorridorPack` per pack in order; `WingClear` full clear) | Combat.Clear 2; Roles.Clear 1; CorridorPack 4; WingClear 25 | c1-c3, g1, g2, d1-d3 |
| Death `death` | C | Seek (`Seek.Death`), Sight (`Markers.Death`), Interact (`Interact.Death`), PartyFollow (`PartyFollow.Death`), Combat (`Combat.Death` / w), Roles (`Roles.Death` / w), Instance (`WingDeath`, `WingWipe` per wipe, divided by tier) | Seek 6; Markers 3; Interact 6; PartyFollow 3; Combat 2; Roles 2; WingDeath 3; WingWipe 5 | m1-m4, c1-c3, g1, g2, d1-d3 |
| Threat `threat` | S | Party (`Party.PulledThreat` for enemies on the seat; `Raid.TankStance`); Instance `WingCrowd` (negative, enemies on the party beyond `WingCrowdFree`) | Party.PulledThreat 0.004; Raid.TankStance 0.001; WingCrowd 0.15; WingCrowdFree 4 | g1, g2, d1-d3 |
| TeammateDamageTaken | S | Party (`Party.TeammateDamageTaken*`, tank's share `Party.TankDamageShare`) | Party.TeammateDamageTakenDps 0.5; ...Protector 1.0; TankDamageShare 0.25 | g1, g2, d1-d3 |
| TeammateHealing | S | Party (`Party.TeammateHealing`, `Party.HealOffGoal`); as the keep term of a non-drilled seat | TeammateHealing 2.0; HealOffGoal 1.0 | g1, g2, d1-d3 |
| TeammateThreat | S | Party (`Party.TankLoseTeammate` for a tank losing a teammate) | 0.02 | g1, g2, d1-d3 |
| TeammateDeath `teammate_death` | C | Party (`Party.TeammateDeath`), Combat (`Combat.AllyDeath` / w) | Party.TeammateDeath 3; Combat.AllyDeath 1 | c1 (guard arena), g1, g2, d1-d3 |
| Revive `revive` | S | Party (`Resurrection.ReviveAlly`) | 1.5 | g1, g2, d1-d3 (only if a revive lands) |
| Progress `progress` | S | Sight (`Markers.Progress`, distance closed per leg) | 1.0 | m1 |
| Arrive `arrive` | O | Sight (`Markers.Arrive`), Seek (`Seek.Arrive`), Interact (`Interact.Arrive`) | 3; 3; 3 | m1, m2, m3 |
| Timeout `timeout` | C | Instance: `PullTimeout` (drill), `WingTimeout` (share of route/corridor not done), divided by tier | PullTimeout 2; WingTimeout 30 | g2, d1-d3 |
| Stall `stall` | S | Party (`Raid.Idle` after `Raid.IdleMs` of nothing done in reach) | Raid.Idle 0.001; IdleMs 4000; IdleReach 40 | g1, g2, d1-d3 |
| SelfHealing `self_healing` | S | Scenario (`Support.SelfHealing` * effective self heal+protection / max health) | 0.5 | every stage |
| GoalReached `goal_reached` | S | Scenario, `AddTaken` at the observation that sees it (`Goals.Reached`), only via `GoalBlock::Earned` | 0.05 | every stage |
| GoalSwitch `goal_switch` | S | Scenario (`Goals.Switch` per switch, `Goals.Secondary` while a secondary is held) | 0.15; 0.002 | every stage (a charge, but categorised Shaping) |
| GoalProgress `goal_progress` | S | Scenario potential (`Goals.Progress`, `ProgressGamma`, secondary share) | 0.5; 0.999; 0.5 | every stage |
| Repeat `repeat` | C, N | Scenario (`Actions.Repeat` per repeat beyond `RepeatFree` in `RepeatWindowMs`; movement never) | 0.03; free 3; window 10000 ms | every stage |
| Jitter `jitter` | C, N | Scenario (`Actions.Jitter` * `StepJitter` quarter-turns, from `SeatActionResult.JitterWeight`) | 0.05 | every stage |
| Aimless `aimless` | C, N | Scenario (`Actions.Aimless` and 17 per-cause keys `Actions.Aimless.*`, `Actions.ModeSwitch`) | 0.02 base; per cause 0.02-0.15 | every stage |
| Effort `effort` | C, N | Scenario (`Actions.Effort` * press effort, `Actions.SupplySpent`) | 0.004 | every stage |
| Fidget `fidget` | C, N | Scenario (`Actions.Fidget` per second moving in a fight while at range) | 0.01 | every stage |
| Hazard `hazard` | S | Scenario (`Hazards.Standing`, `Hazards.Damage`, cap `Hazards.Max`) | 0.15; 0.5; 3 | stages with ground fire (c2 `fire`, dungeons) |
| HealingMana `healing_mana` | S | Scenario (`Support.HealingMana`, negative) | 0.1 | every stage with mana healers |
| CombatClock `combat_clock` | S | Scenario (`Output.Clock` per second an engaged enemy lives, all seats) | 0.03 | stages with fights |
| PullClean `pull_clean` | O | Roles (`Roles.PullClean`); Instance drill (`Instance.PullClean`) times tier | Roles 2; Instance 5 | g1 (pull arena), d1 (drills) |
| EarlyPull `early_pull` | C | Party (`Raid.EarlyPull`) | 0.01 | g1, g2, d1-d3 |
| DrillHold `drill_hold` | O | Roles / Party (`Roles.Hold`, `Roles.Loose`, `Raid.TankHold/TankLoose`), drilled seat 0 | Roles.Hold 0.045; Loose 0.018 | g1 (tank_hold) |
| DrillFocus `drill_focus` | O | Roles (`Roles.Focus`, `Roles.PulledOff`) | 0.9; 0.012 | g1 (damage_discipline) |
| DrillKeep `drill_keep` | O | Roles (`Roles.Keep`, `Roles.KeepLow`, `Roles.Overheal`) | 0.0006; 0.0006; 0.5 | g1 (heal_keep) |
| PullExtra `pull_extra` | C | Combat (`Combat.ExtraPull`), Roles (`Roles.PullExtra`), Instance (`WingChainPull`, drill `PullExtra`) | 1; 1.5; 3; 5 | c2, c3, g1, g2, d1-d3 |
| Facing `facing` | S | Sight (`Markers.Facing`) | 0.25 | m1 |
| Stuck `stuck` | C, N | Seek, Sight, Interact, PartyFollow via `AddFixed` (`Seek.Stuck`, `Controls.Stuck`, `Interact.Stuck`) per second | 0.02 | m1-m4 |
| Wall `wall` | C, N | same encounters via `AddFixed` (`Seek.Wall`, `Controls.Wall`, `Interact.Wall`; slide thresholds `*.WallSlide` 0.5) | 0.02 | m1-m4 |
| FollowKept `follow_kept` | O | PartyFollow (`PartyFollow.Kept` per second in band) | 0.02 | m4 |
| Lost `lost` | C | PartyFollow (`PartyFollow.Lost`), Instance (`Instance.WingStray`) | 0.02; 0.02 | m4, g2, d1-d3 |
| Sighting `sighting` | S | Seek (`Seek.Sighting`), Interact (`Interact.Sighting`) | 0.5; 0.5 | m2, m3 |
| NewGround `new_ground` | S | Seek (`Seek.NewGround`) | 0.004 | m2 |
| RoomSeen `room_seen` | S | Seek (`Seek.RoomSeen`) | 0.1 | m2 |
| DoorOpened `door_opened` | O | Interact (`Interact.DoorOpened`) | 1.0 | m3 |
| WrongObject `wrong_object` | C | Interact (`Interact.WrongObject`) | 0.5 | m3 |
| Regroup `regroup` | O | PartyFollow (`PartyFollow.Regroup`, window `RegroupWindow`) | 0.5 | m4 |
| Blocking `blocking` | C | PartyFollow (`PartyFollow.Blocking`) | 0.05 | m4 |
| Survived `survived` | O | Combat (`Combat.Survived`; `Combat.SurviveSurvived` in the survive drill), Roles (`Roles.Survived`) | 1; 2; 1 | c1-c3, g1 |
| InterruptLanded `interrupt_landed` | O | Combat (`Combat.InterruptLanded`) | 0.25 | c1-c3 |
| Away `away` | C | Combat (`Combat.Away`, yards `AwayYards`), Roles (`Roles.Away`), Instance (`WingAway`) | 0.02 each | c1-c3, g1, g2, d1-d3 |
| Hurt `hurt` | C | Combat (`Combat.Hurt`) | 0.2 | c1-c3 |
| FireHurt `fire_hurt` | C | Combat (`Combat.FireHurt`) | 1.0 | c1-c3 |
| ReadyPull `ready_pull` | O | Instance (`Instance.WingEngage` per pull started ready, share `WingReadyShare`) | 1.0; 0.8 | g2, d1-d3 |
| Idle `idle` | C | Instance (`Instance.WingStall`, others `WingStallOthers`, grace `WingStallGraceMs`) | 0.1; 0.2; 60000 | g2, d1-d3 |

Counts by kind: Outcome 13, Cost 20, Shaping 20 (total 53); noise prices: Repeat, Jitter, Aimless, Effort, Fidget,
Stuck,
Wall. Each encounter lists the terms it owns in `RewardTerms()` so a `reward_<term>` info column exists
(`StageScenario.cpp:538-552`); scenario-level terms are listed separately.

UNVERIFIED items in the table: the exact payment conditions and keys of Party terms (`PartyEncounter.cpp:270-535`),
Roles drill terms' formulas (`Roles.*` earn/lose helpers at `RolesEncounter.cpp:590-660`), and whether `Revive` ever
fires in a live stage; the per-stage columns come from which encounters a stage instantiates, not from reading each
condition. Defaults were extracted from `CurriculumTuning.h` by script; the effective values are in every run's
`stage.json` `tuning`.

Observed issues for rewards: `GoalSwitch` is a pure charge but is categorised Shaping and so fades; `Hazard`,
`HealingMana`, `CombatClock` likewise charges categorised Shaping (they fade away by design of the ladder, which is the
opposite of principle 9 for anything that is a price). `Threat` is used both as a shaping bonus (Party) and a charge
(Instance crowd); `Approach` pays a high-water mark and never charges a step back (header comment). `RewardTerm::Stall`
is
Shaping, so a faded stage pays nothing for standing about except the wing's `Idle`.

## Layered fields (`Blocks/LayeredField.*`)

Not a block. A `Grid` is one 533.33 yd map grid at 1 yd cells (`STANDARD_CELL`): per cell the intervals of open air
above each
floor: `Interval` (8 bytes): `Floor8` (eighths of a yard), `Headroom8` (0xFFFF open sky), `Liquid8` (surface above floor
or none), `LiquidFlags`, `Flags` (low nibble the navmesh polygon flags ground/magma/slime/water, 0x80 open above).
`Bake(map, x, y, cell, threads)` (`LayeredField.cpp:204`): for every cell column, floors = navmesh floors (snapped to
the
core's height within a step) + terrain + up to 48 downward collision-ray surfaces from 600 yd above; floors within 0.5
yd
merged; headroom by an upward collision ray (100 yd search), floors with under 0.5 yd dropped unless on the navmesh;
liquid within 10 yd below a floor attached. Multi-threaded by row. `Write`/`Read`: header (magic `AHLF`, version 1, map,
grid x/y, cell, side, min x/y, interval count, raw bytes), then zstd level 15 of (one count byte per cell, capped at
255,
then the intervals); written to `<path>.partial` and renamed. `Store`: `Configure(dir, cacheGrids)`
(`AnimusForge.cpp:191`, from `AnimusForge.Probe.Dir` and `AnimusForge.Probe.CacheGrids`; minimum 9), `Find(map, gx, gy)`
thread-safe with a read
lock, files `NNN_gx_gy.field`, negative results cached, least-recently-read eviction over the cap, one warning when file
reads reach 4 x cap. Produced by `forge fieldstage <scenario> [rebake]` and `forge fieldworld <all|map> [rebake]`
(`src/server/scripts/Commands/cs_forge.cpp:939`, `:1015`). Tests: none. Reviewer notes: `Enabled()` and `Dir()` read
unprotected globals set under a lock; negative cache entries are never evicted; the per-cell count byte silently caps at
255; the file includes `MoveBlock.h` only for `MAX_STEP`.

## FieldGrids

`GridIndex(c) = floor(c / SIZE_OF_GRIDS)`. `StageGrids(stage, wholeMaps)`: for each map the stage or its arenas or
instance
ladder rows use; a continent contributes the 3x3 grids at 80 yd around each spawn (or the whole map with `wholeMaps`);
an instanceable map contributes every `mmaps/MMMXXYY.mmtile` it finds (core grid coordinates counted down from
`CENTER_GRID_ID`, converted to field numbering). Caller: `cs_forge.cpp:957`. No tests.

## FieldRoute

`Plan(mapId, from, to, out, maxNodes = 4000000)`: A* over a 1-yard lattice of (x, y, floor index), eight neighbours,
cost `across (1 or 1.414) + 0.3 off-mesh + 0.1 |rise| + drop surcharge`; standable = headroom >= 2 yd and not burning
(magma/slime nav flag or liquid); step up/down limit `MAX_SLOPE 1.2` per yard across; larger drops only onto navmesh
floors and at most `MAX_DROP 8`; floors within `SNAP 3` of the start z; goal within 2.5 yd horizontally and 3 in z.
Returns the yard cells, false without a field (store disabled or no file) or no way. `Covers` asks if a column exists.
`Report` is the console diagnostic. Callers: `InstanceEncounter.cpp:1485, 1553, 1554, 1642` (the wing route),
`:1282` (`Covers`), `cs_forge.cpp:1126`. `thread_local` visit table; the header says "World thread" but the code is
per-thread. Tests: none.

## RoutePlanner

Navmesh corner routes with a private 65535-node query per thread per mesh (`QueryFor`); filter ground and water, never
magma or slime. `Plan` returns up to `Route::MAX_CORNERS = 256` corners with `Remaining[i]` (length to the end), partial
routes return true with `Complete = false`; extents 3 x 5 x 3 yd; plan time goes to `CurrentReset.RouteNs`. `SurfaceAt`
(`forge floorscan`). `Route::Advance` steps `Next` past reached corners (its comment mentions a `RemainingFrom` that
does
not exist). Callers: `PartyFollowEncounter.cpp:413` (the follow leader's route), `cs_forge.cpp:381, 1166`. Tests: none.
Reviewer notes: thread safety is per-thread queries; the long comment about "thread safety is inherited, not enforced"
is stale. Bot movement does not use this; it is the scripted follow leader and diagnostics (principle 3 concerns bots).

## FloorScan and RouteShortcut

`FloorScan` (pure): `Classify(nav, navZ, floor, floorZ, normalZ)` -> Ok, Hole, Mismatch (> 0.75 yd), Steep (normal z <
cos 50),
NoNav, Unwalkable; glyphs, severities, `GridSpan`. Tests: `FloorScanTest`. `RouteShortcut` (pure): `Corners(count, step,
clear)` gives each yard the farthest straight-walkable yard within `REACH 20`, used by `InstanceEncounter.cpp:1742` to
build `CornerAhead/CornerBack`. (`Door`, `EntersDoor`, `CutAtDoors`, `Chain`, `ADVANCE_YARDS` and `MAX_POINTS` were deleted
2026-10-08, with `ClosedDoors` of the instance encounter, which only they read.)

## Observed issues (routing)

- (fixed 2026-10-08) `RouteShortcut::Chain/CutAtDoors/EntersDoor` and constants were deleted.
- `LayeredField` lives in `Blocks/` and depends on `MoveBlock.h`.
- `Route::Advance` and `RoutePlanner` comments refer to removed designs; `FieldRoute.h` says world thread, code is per
  thread.
- No unit tests for FieldRoute, LayeredField, FieldGrids, RoutePlanner (only data-dependent integration checks exist
  elsewhere, UNVERIFIED).

## Questions for the owner

- Move the three score-irrelevant charges (GoalSwitch, Hazard, HealingMana, CombatClock) to Cost so the fade does not
  remove them, or leave them as shaping on purpose?
- Rename `CombatReward.cpp` and delete the dead `RouteShortcut` functions?
