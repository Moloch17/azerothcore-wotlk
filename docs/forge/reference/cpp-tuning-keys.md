# Curriculum tuning keys (`CurriculumTuning`)

Reference for every value that shapes what the curriculum trains on and what it is paid for. Written for a manual
review and refactor; everything is checked against the tree at `bd32b9dc8` (branch `forge`, 2026-10-07).

**Scope.** `CurriculumTuning.h` / `CurriculumTuning.cpp`: the 22 tuning groups, their 297 config keys (293 fields of the
struct plus the four `StandIn.*` keys of `StandIn::Tuning`), how they are loaded, recorded and fingerprinted, the
the (removed) conf.dist agreement check, and the three per-arena override families that are read by name outside the
Visit list. It does not describe what the encounters do with the numbers (see [cpp-encounters.md](cpp-encounters.md),
[cpp-stagescenario.md](cpp-stagescenario.md), [cpp-rewards-routing.md](cpp-rewards-routing.md)) nor the forge's own
non-curriculum keys (`AnimusForge.Stage.<name>.Envs` and so on; see [config-keys.md](config-keys.md)).

## Map

Paths relative to the repository root.

| Path | Lines | Role |
|---|---|---|
| `src/server/game/Animus/Scenario/Curriculum/CurriculumTuning.h` | 1185 | The `CurriculumTuning` struct: 21 nested group structs with their defaults (lines 41-846), the `StandIn::Tuning` member (848), the `Visit` template that lists every key once (852-1175), `Load` and `Json` declarations (1178, 1181). |
| `src/server/game/Animus/Scenario/Curriculum/CurriculumTuning.cpp` | 115 | `Load` (reads every key from the config, then clamps), `Json` (every value as a JSON object), and two clamp helpers. |
| `src/server/game/Animus/Scenario/Curriculum/Encounters/StandIn.h` | 161 | Defines `StandIn::Tuning` (lines 105-114), the fourth group's defaults; documented in [cpp-encounters.md](cpp-encounters.md). |
| `src/server/apps/worldserver/worldserver.conf.dist` | 6755 | The only template that documents the keys (the section "CURRICULUM TUNING" starts at line 6004); every key has an uncommented `AnimusForge.Curriculum.<key> = <default>` line. |
| `apps/forge/tools/conf_prune.py` | 311 | Lists/comments out conf keys the build no longer reads; derives the "families" read by name. |
| `apps/forge/forgectl/confsync.py` | 249 | `forgectl conf-sync`: copies the host's `AnimusForge.Curriculum.*` lines to every worker conf. |

## 1. How the tuning gets into the sim

### 1.1 Prefix and `Load`

- The config key of a value is `<prefix><key>`. The prefix is `StageSettings::TuningPrefix`
  (`Scenario/StageSettings.h:73-75`):
  `"AnimusForge.Curriculum."` for the forge (`ForgeConfig.cpp:669`) and `"Animus.Curriculum."` for mod-animus (the
  header's claim; the module is not in this tree: UNVERIFIED, see the end).
- `CurriculumTuning::Load(prefix)` (`CurriculumTuning.cpp:58-92`) default-constructs the struct, then calls `Visit` and
  for every `(key, value)` runs `sConfigMgr->GetOption(prefix + key, value, false)` with the current value as the
  default. `showLogs = false`: a missing key is silent (intended: the comment says so), and so is a **malformed** one
  (`Config.cpp:583-597` returns the default without logging when `showLogs` is false). A float that loads as NaN or
  infinity is logged (`module.animus` warning) and the default is kept (`CurriculumTuning.cpp:63-68`).
- `ConfigMgr::GetOption` also honours an environment variable `AC_<KEY>` (`common/Configuration/Config.cpp:435-441,
  540-552`), so a tuning key can be set from the environment; it then also enters the fingerprint (1.4).
- After loading, `Load` clamps (`CurriculumTuning.cpp:74-91`), logging a `module.animus` warning when it changes a
  value:
  - percent chances to 0..100 (`ClampPercent`): `Characters.PetOutChance`, `Difficulty.ReviewChance`,
    `Party.ClassicChance`, `StandIn.Share`, `StandIn.LeadChance`, and each member of the pairs below;
  - role pairs whose two chances come from one roll (`ClampRolePair`, scaled down proportionally to
    `tank = tank*100/(tank+healer)`, `healer = 100 - tank` when the sum passes 100): (`Characters.HighLevelChance`,
    `Characters.LowLevelChance`), (`Characters.NoisyTalentChance`, `Characters.RandomTalentChance`),
    (`Party.RoleTankChance`, `Party.RoleHealerChance`), (`StandIn.TankChance`, `StandIn.HealerChance`);
  - `Difficulty.Window >= 1`.
- Nothing else is clamped in `Load`. Unsigned keys are not range-checked at all; what a negative or oversized text
  does for a `uint32` key depends on `Acore::StringTo<uint32>` (UNVERIFIED: read `common/Utilities/StringConvert.h`),
  and with `showLogs = false` any failure falls back to the default silently.
- The header says `Load` returns values where "min/max pairs are ordered" (`CurriculumTuning.h:1176-1177`). **The code
  orders no pair.** `BandMin/BandMax`, `Nearest/Furthest`, `SightNearest/SightFurthest`,
  `HallwayNearest/HallwayFurthest`,
  `FightNearest/FightFurthest`, `NextNearest/NextFurthest`, `PartyNearest/PartyFurthest`, `DecoysMin/DecoysMax`,
  `SuddenStopMinMs/MaxMs`, `SuddenGapMin/Max` are used as set. Only the two `Sudden*` pairs are guarded where they are
  read (`PartyFollowEncounter.cpp:319, 359`: `max(min, max)`).

Who calls `Load`: the scenario constructor (`StageScenario.cpp:296`, held as the member `_tuning`,
`StageScenario.h:436`,
exposed as `Tuning()`, `StageScenario.h:164`) and `ClusterFingerprint` (`AnimusForge.cpp:92-93`). A scenario therefore
holds one copy read at construction; a config reload does not change a running scenario (UNVERIFIED: whether anything
rebuilds scenarios on reload; check `AnimusForge.cpp` / `ForgeMain.cpp`).

### 1.2 `Visit`

`Visit(tuning, f)` (`CurriculumTuning.h:851-1175`) is a static template over a `const` or non-const tuning: one
`f("Group.Key", tuning.Group.Field)` per key, 297 lines. It is the **only** list of keys: `Load`, `Json`, the conf.dist
test and (through `Json`) the fingerprint and `stage.json` all derive from it. A field added to a group struct and not
added to `Visit` is never read from the config, never recorded and never fingerprinted; nothing in the C++ checks this.
(One-off check done for this document: all 309 struct fields appear in `Visit`, no key appears twice.)

The key order of `Visit` is not the struct order. `Actions.Aimless.*` map to the fields `Actions.AimlessX`;
`Options.JitterDecayMs`
is listed late, after `Roles.*` (`CurriculumTuning.h:1160`), while `Options.RestMaxMs` and `Options.HoldInterruptMs` are
listed
earlier (lines ~992-993). The order matters: it is the order of the serialised JSON that is hashed (1.4).

### 1.3 `Json` and `stage.json`

`Json()` (`CurriculumTuning.cpp:94-114`) builds a `boost::json::object` in `Visit` order, key as in the config.
Integers are stored as is; a float is stored as the *shortest decimal that round-trips the float* converted to a
double (`std::to_chars` then `strtod`), so `0.03f` is recorded as `0.03`, not `0.029999999329447746`.
`StageScenario` writes it as `stage.json` -> `"tuning"` (`StageScenario.cpp:1504`), written only when changed
(`WriteIfChanged`, `StageScenario.cpp:1506`) to `<LayoutsDir>/<stage>/stage.json` and copied into each run directory.
The learner reads it back: `animus/train.py:326` puts it in the baseline-cache key
(`baseline_cache_key`), so changing any tuning value invalidates a cached `eval_baseline*.json`
(`train.py:1198`); `stage_json_diff.py` covers the "tuning" section of the stage diff.

### 1.4 The cluster fingerprint

`ClusterFingerprint` (`AnimusForge.cpp:71-103`) serialises `CurriculumTuning::Load("AnimusForge.Curriculum.").Json()`,
hashes it with FNV-1a 64 and puts it in the string `curriculum=<016x>`. The host refuses a worker whose fingerprint
differs ([cluster.md](../cluster.md)). Consequences:

- The hash covers **effective values of the 297 Visit keys**, so two confs that list different keys but agree on every
  value agree (the comment in `AnimusForge.cpp:88-91`). A value left at its default and a key written with the same
  value hash the same.
- It does **not** cover the override families of section 2 (`Arena.*`): two machines can differ
  there and be accepted. `forgectl conf-sync` does copy them (`confsync.py:16-17`: every uncommented
  `AnimusForge.Curriculum.*` line), so only a machine that was not synced is exposed.
- A reordering or renaming in `Visit` changes the hash of otherwise identical values (harmless across a build because
  the
  source hash differs anyway).
- Environment variables `AC_ANIMUS_FORGE_...` (1.1) enter the hash; they are per machine and invisible in the conf.

### 1.5 The conf.dist agreement check and the tools around it

The agreement test (`test_conf_covers_tuning.py`) was removed 2026-10-07 (see [tests.md](tests.md)). What it checked:
the keys of `CurriculumTuning::Visit` (every string literal passed to `f(` in `CurriculumTuning.h`) and the uncommented
`X.Curriculum.<key> =` lines of `worldserver.conf.dist` should be the same set, both ways. It compared **key sets only**,
not default values or types. A script written for this document compared all 297 uncommented conf lines against the
in-class defaults: they agree today. Arena/stage override keys are commented in conf.dist, so the check did not see them.
By hand: `conf_prune.py --removed <old-rev> <new-rev>` and a read of `CurriculumTuning.h`.

`conf_prune.py` treats `AnimusForge.Curriculum.Arena.<stage>.<arena>.<key>`
as a family "read by name" (`conf_prune.py:16-19`), and relies on the principle that "the conf.dist diff IS the Visit
diff" (`conf_prune.py:21-25`).

## 2. Override keys read outside `Visit`

All read in the `StageScenario` constructor with `GetOption(..., false)` (silent on missing or malformed values):

| Key (prefix `AnimusForge.Curriculum.`) | Default | Reader | Effect |
|---|---|---|---|
| `Arena.<stage>.<arena>.Weight` | the arena's `ArenaDefinition::Weight` | `StageScenario.cpp:489-491` | share of training episodes (also the start weight of the ramp). |
| `Arena.<stage>.<arena>.WeightFinal` | `ArenaDefinition::WeightFinal` if >= 0, else the arena's Weight | `StageScenario.cpp:492-494` | weight at the end of the stage's budget; evaluations draw by the final weights (`StageScenario.cpp:675-677`). Clamped >= 0. |
| `Arena.<stage>.<arena>.StandInShare` | `ArenaDefinition::StandInShare` | `StageScenario.cpp:497-499` | percent of the arena's training episodes with a stand-in; clamped -1..100 (-1 = defer to `Roles.StandInShare` for a Roles arena, else `StandIn.Share`; `StandInSeat.cpp:82-91`). |

If every `Weight` is 0 the arenas are drawn evenly with an error log (`StageScenario.cpp:520-524`); if every
`WeightFinal`
is 0 they take the start weights (`StageScenario.cpp:516-517`).

conf.dist documents `Weight` and `WeightFinal`, and `StandInShare` (in prose). (The undocumented-`WeightFinal`
and unread-`MaxRung` findings of Observed issues 3 and 4 were fixed 2026-10-08: `MaxRung` was removed from conf.dist
and `WeightFinal` documented.)

## 3. The groups

Struct line ranges are in `CurriculumTuning.h`. "Per decision" terms are tuned for 50 ms decisions and multiplied by
`DecisionScale = DecisionMs / 50` (`StageScenario.cpp:103, 300`); per-second terms are multiplied by the elapsed seconds
(for example `Combat.Clock * decision`, `CombatEncounter.cpp:624`, `Output.Clock * _decisionMs / 1000`,
`StageScenario.cpp:4989`). The keys of every group are in the table of section 4.

| Group | Struct lines | Keys | Read by | What it tunes |
|---|---|---|---|---|
| `Characters` | 41-73 | 10 | `StageScenario.cpp` | The seat characters: level draw (`RandomLevel`, 171-196), talent plan (`RandomTalentPlan`, 136-145), pet out at start (1573, 2397), and character reuse (`ReuseEpisodes`, `KeepCasting`: 2095-2185; evaluations always build). |
| `Party` | 75-105 | 15 | `StageScenario.cpp` (makeup draw 1995-2035, `TankDamageShare` 4921), `PartyEncounter.cpp` | Which party seats have a character and the party's per-teammate reward weights. |
| `Raid` | 107-143 | 12 | `PartyEncounter.cpp:410-535` | Per-role reward weights of a party (tank hold, damage on the tank's target, healer keep-up and overheal, stance, early pull, idle), despite the name no raid stage exists. |
| `Duel` | 145-151 | 2 | `StageScenario.cpp:2773, 3790`, `Rewards/CombatReward.cpp:90` | The melee and ranged range the position goal aims for. The group name is a leftover of the deleted duel stages. |
| `Difficulty` | 153-178 | 7 | `Encounters/DifficultyLadder.cpp`, `CombatEncounter.cpp:252, 576`, `RolesEncounter.cpp:560`, `InstanceEncounter.cpp:2731` | The per-class/build tier ladder (raise/lower rates, window, review and stretch draws) and the tier scale of outcome terms. |
| `Instance` | 180-279 | 26 | `InstanceEncounter.cpp`, `StageScenario.cpp:302-303, 1712` | Whole-dungeon wing: prices (`Wing*`) and the wing ladder (`WingProbe`, `WingRungRuns`, `WingRungTarget`, `WingRungStart`). |
| `Goals` | 281-324 | 12 | `StageScenario.cpp:3638-3898, 5026-5036` | The learner-chosen goals (SeatGoal): reached payments, switch cost, potential progress, per-kind values, the secondary goal. |
| `Support` | 326-348 | 2 | `StageScenario.cpp:5048-5053` | Self healing pay and the mana price of healing. |
| `Actions` | 350-442 | 34 | `Layout/SeatMemory.cpp` (pacing), `StageScenario.cpp:3734-4584` | Press pacing (`RepeatMs`...), repeat/jitter/effort/fidget prices, and the "aimless" press price and its 19 per-cause prices. |
| `Options` | 444-462 | 3 | `Blocks/GauntletBlock.cpp:113`, `Blocks/PackBlock.cpp:120`, `StageScenario.cpp:4498`, `Blocks/MoveBlock.cpp:333` | Durative action limits and the steering-jitter decay. |
| `Hazards` | 464-478 | 3 | `StageScenario.cpp:4942-4951` | Ground-effect damage prices and their per-episode cap. |
| `Markers` | 480-509 | 8 | `SightEncounter.cpp`, `SeekEncounter.cpp:543`, `InteractEncounter.cpp:649` | M1 marker prices and stop/arrive geometry. |
| `Respawn` | 511-531 | 2 | `CombatEncounter.cpp:407-412`, `InstanceEncounter.cpp:1004`, `PartyFollowEncounter.cpp:446`, `RolesEncounter.cpp:401` | The entrance-respawn clock (I4): delay and rejoin distance. See the respawn section of [cpp-encounters.md](cpp-encounters.md). |
| `PartyFollow` | 533-586 | 26 | `PartyFollowEncounter.cpp`, `StageScenario.cpp:3211` | M4 follower prices, band, leader script rungs. Its Stuck/Wall prices are `Seek.*` (`PartyFollowEncounter.cpp:643`). |
| `Seek` | 470-540 | 63 | `SeekEncounter.cpp`, `StageScenario.cpp`, `PartyFollowEncounter.cpp:643-652` | M2 prices, placement and rung seconds, and the room goals (`Goals`, `GoalsFromRung`, `RoomGoal`, `RoomSwitch`, `Return`, `AidUntil`, `GlimpseRays`, `CheckedShare`, `EnterDwellMs`, `ReturnAwayYards`, `ReturnAwayMs`; 2026-10-09) and the cell goals (`GoalSource`, `CellReach`, `CellRise`, `CellSame`, `CellMinYards`, `CellPatienceMs`, `CellPatienceYards`, `CellGoal`, `CellProgress`, `CellSwitch`, `CellLost`, `CellStale`; 2026-10-09, free choice goals) and explore-unstuck (`ExploreSeen`, `ExploreCap`, `ExploreFloor`, `ExploreRoomBonus`, `FrontierPull`, `FrontierCap`, `CircleWindowMs`, `CircleYards`, `CircleNetYards`, `CircleTurnDeg`, `Circling`, `TrapShare`, `Escape`, `TrapEscapeYards`, `TrapEscapeMs`; 2026-10-10, decision 0023). |
| `Interact` | 634-675 | 21 | `InteractEncounter.cpp` | M3 prices, decoys and sweep. |
| `Controls` | 677-718 | 14 | `SightEncounter.cpp` | M1 object placement, corner share, compass withhold chances, wall/stuck prices. |
| `Combat` | 720-785 | 30 | `CombatEncounter.cpp`, `CombatDraw.h`, `RolesEncounter.cpp:186, 287, 296` | C1-C3 prices, ladder, placement. |
| `Roles` | 787-827 | 37 | `RolesEncounter.cpp`, `RolesDraw.h`, `StandInSeat.cpp:89` | G1 prices, ladder, camp, win measure, stand-in share. |
| `Resurrection` | 829-836 | 2 | `StageScenario.cpp:2643`, `PartyEncounter.cpp:270` | Grace to wait for a resurrection; payment for reviving an ally. |
| `Output` | 838-846 | 1 | `StageScenario.cpp:4986-4989` | A clock charge per second an engaged enemy lives. |
| `StandIn` | 848 (`StandIn.h:105-114`) | 4 | `StandInSeat.cpp:82-91`, `StandIn.h:142-146` | The "human" stand-in: share of episodes and style draw (lead / role). |

Notes per group worth knowing before changing it:

- **Characters.** `HighLevelFirst`/`LowLevelLast` only matter when no fixed level applies: `StageDefinition::Level`
  (move1, move2), an `EpisodeLevel` set by an encounter, a kept character level, or a focus band
  (`FocusChance = 100` on move3 and combat1-3 and group1: `Stages.cpp:742, 818, 842, 865, 916`) take precedence
  (`StageScenario.cpp:2203-2207`). Evaluations take the band from the seed index, 4 bands of 20 levels
  (`StageScenario.cpp:171-181`). So in training only `move4_follow` (no level, no focus) reaches the tuned draw
  (note 3 in the table); the dungeon stages' levels come from the instance rung (UNVERIFIED: read
  `InstanceEncounter::BeforeLevel`). The doc comment of `RandomLevel` sits above `TankModeSpell`
  (`StageScenario.cpp:142-153` vs the function at 171): misplaced.
- **Party / Raid.** `Party.SizeWeight*` and `Raid.DrillWeight` were deleted (2026-10-08: no live party arena reached
  the size draw, and no drilled seat is paid by `PartyEncounter`); the makeup draw
  (`ClassicChance`, `RoleTankChance`, `RoleHealerChance`) is reached only by `move4_follow`
  (`classic = DrillRole || instance || proper || roll`, `StageScenario.cpp:2023`). PartyEncounter is used by arenas with
  `PartyGroup` (`StageScenario.cpp:470-475`): group1, dungeon2-3. The `Roles.*` per-role prices mirror the
  `Raid.*` ones
  at three times the value (`Roles.Hold = 3 x Raid.TankHold`, `Roles.Focus = 3 x Raid.TankTarget`, `Roles.Keep = 3 x
  Raid.KeepUp`: header comments, values 0.045 / 0.9 / 0.0006 check out).
- **Instance.** The stall (`WingStall`) is paid as `Idle` (a Cost, full price), the stray (`WingStray`) as `Lost`, the
  away (`WingAway`) as `Away`, the engage as `ReadyPull` (Outcome); all of that is in `InstanceEncounter.cpp:2554-2691`
  and documented in [cpp-encounters.md](cpp-encounters.md). The tier scale is capped by `MaxTierScale`
  (`InstanceEncounter.cpp:2731`). `WingRungStart` is only the starting rung; a resumed run names its own
  (`WingLadder.cpp:32`).
- **Actions.** The 19 `Aimless*` prices plus `Aimless` itself: the base price applies to aimless presses not
  already charged by a cause (`StageScenario.cpp:4576`: `Aimless * (StepAimless - min(StepAimless, caused))`).
  The long comment above the struct says "each decision is 100 ms apart"; the sim's decisions are 50 ms
  (`REWARD_TUNING_MS`, `StageScenario.cpp:103`, and the header's own preamble, line 34).
- **Markers / Seek / Interact / Controls.** Four groups hold near-identical `Stuck`, `Wall`, `WallSlide`, `ArriveRise`,
  `Arrive`, `StepCost`, `Death` prices, one per stage family, and M4 borrows `Seek.Stuck/Wall/WallSlide`. Changing M2's
  wall price therefore changes M4's.

## 4. Keys table

297 rows, in `Visit` order (which is also the order of `stage.json` -> `tuning`). Columns:

- **Default**: the in-class initialiser (`CurriculumTuning.h`, `StandIn.h` for `StandIn.*`). Equal to the uncommented
  value in `worldserver.conf.dist` for every row (checked once by script).
- **Range**: the clamp that exists, `Load` (section 1.1) or at the reader; `-` = none anywhere I found. There is no
  documented valid range for any key beyond these.
- **Readers**: up to three `file:line` of code that reads the field, relative to
  `src/server/game/Animus/Scenario/Curriculum/`; `(+n)` = n more. Found mechanically: a match of `Group.Field` or of
  a local reference typed `<Group>Tuning const&` / bound to `.Group`, with comment-only matches removed. Where two
  groups share a reference name in one file a match can be a false positive, and a read through another kind of alias
  would be missed; I read the lines of the shared-name families (Markers/Seek/Interact/Controls, Combat, PartyFollow,
  Instance, Party/Raid) by eye. Every key has at least one real reader; none is read only in a comment.
- **Live stages**: where the reader runs in the ten live stages, derived from the arenas' `Against`/`Seats`/blocks
  in `Stages/Stages.cpp` (`all` = every stage; short names `move1` ... `dungeon3`; `combat1-3`, `dungeon2-3` are
  ranges).

Notes: (3) the tuned level draw is reached only where no fixed level, episode level, kept level or focus band applies:
`move4_follow` in training (see section 3); (4) the party-size draw (`RandomPartySize`, `Party.SizeWeight*`) was deleted 2026-10-08: every live party arena has an
instance, a `ProperParty` or a `PartySize`; (5) arenas of dungeon2-dungeon3 carry their own `StandInShare =
20`
(`Stages.cpp:621`), group1's roles arenas use `Roles.StandInShare`, `heldout` arenas are `EvalOnly`; no live arena falls
through to `StandIn.Share`, so its default 0 is never consulted today (the "defer" path still exists).

| Group | Key | Default | Type | Range | Readers | Live stages |
|---|---|---|---|---|---|---|
| Characters | `Characters.HighLevelFirst` | 61 | uint32 | reader: clamp 1..80 | `StageScenario.cpp:188` | move4 (see note 3) |
| Characters | `Characters.HighLevelChance` | 50 | int32 | Load: 0..100; with LowLevelChance scaled so the sum <= 100 | `StageScenario.cpp:191`, `StageScenario.cpp:192` | move4 (see note 3) |
| Characters | `Characters.LowLevelLast` | 20 | uint32 | reader: min(.., 80) | `StageScenario.cpp:189` | move4 (see note 3) |
| Characters | `Characters.LowLevelChance` | 15 | int32 | Load: 0..100; pair scaled to sum <= 100 | `StageScenario.cpp:192` | move4 (see note 3) |
| Characters | `Characters.NoisyTalentChance` | 30 | int32 | Load: 0..100; with RandomTalentChance sum <= 100 | `StageScenario.cpp:139`, `StageScenario.cpp:141` | all |
| Characters | `Characters.RandomTalentChance` | 10 | int32 | Load: 0..100; pair scaled to sum <= 100 | `StageScenario.cpp:141` | all |
| Characters | `Characters.TalentNoisePoints` | 5 | uint32 | reader: max(1, ..) | `StageScenario.cpp:2508` | all |
| Characters | `Characters.PetOutChance` | 50 | int32 | Load: 0..100 | `StageScenario.cpp:1573`, `StageScenario.cpp:2397` | all |
| Characters | `Characters.ReuseEpisodes` | 4 | uint32 | - | `StageScenario.cpp:2095`, `StageScenario.cpp:2101`, `StageScenario.cpp:2178` (+1) | all |
| Characters | `Characters.KeepCasting` | 1 | uint32 | - | `StageScenario.cpp:2095` | all |
| Party | `Party.ClassicChance` | 50 | int32 | Load: 0..100 | `StageScenario.cpp:2023` | move4 only |
| Party | `Party.RoleTankChance` | 25 | int32 | Load: 0..100; pair scaled to sum <= 100 | `StageScenario.cpp:2028` | move4 only |
| Party | `Party.RoleHealerChance` | 25 | int32 | Load: 0..100; pair scaled to sum <= 100 | `StageScenario.cpp:2028` | move4 only |
| Party | `Party.TeammateDamageTakenDps` | 0.5f | float | - | `Encounters/PartyEncounter.cpp:345` | group1, dungeon2-3 |
| Party | `Party.TeammateDamageTakenProtector` | 1.0f | float | - | `Encounters/PartyEncounter.cpp:345` | group1, dungeon2-3 |
| Party | `Party.TeammateHealing` | 2.0f | float | - | `Encounters/PartyEncounter.cpp:349`, `Encounters/PartyEncounter.cpp:519`, `Encounters/RolesEncounter.cpp:645` | group1, dungeon2-3 |
| Party | `Party.HealOffGoal` | 1.0f | float | - | `Encounters/PartyEncounter.cpp:387` | group1, dungeon2-3 |
| Party | `Party.TankDamageShare` | 0.25f | float | - | `StageScenario.cpp:4921` | group1, dungeon2-3 |
| Party | `Party.TankLoseTeammate` | 0.02f | float | - | `Encounters/PartyEncounter.cpp:363` | group1, dungeon2-3 |
| Party | `Party.PulledThreat` | 0.004f | float | - | `Encounters/PartyEncounter.cpp:308` | group1, dungeon2-3 |
| Party | `Party.TeammateDeath` | 3.0f | float | - | `Encounters/PartyEncounter.cpp:372` | group1, dungeon2-3 |
| Raid | `Raid.TankHold` | 0.015f | float | - | `Encounters/PartyEncounter.cpp:467` | group1, dungeon2-3 |
| Raid | `Raid.TankLoose` | 0.006f | float | - | `Encounters/PartyEncounter.cpp:468` | group1, dungeon2-3 |
| Raid | `Raid.TankTarget` | 0.3f | float | - | `Encounters/PartyEncounter.cpp:483` | group1, dungeon2-3 |
| Raid | `Raid.PulledOff` | 0.004f | float | - | `Encounters/PartyEncounter.cpp:487` | group1, dungeon2-3 |
| Raid | `Raid.EarlyPull` | 0.01f | float | - | `Encounters/PartyEncounter.cpp:496` | group1, dungeon2-3 |
| Raid | `Raid.KeepUp` | 0.0002f | float | - | `Encounters/PartyEncounter.cpp:510` | group1, dungeon2-3 |
| Raid | `Raid.Overheal` | 0.5f | float | - | `Encounters/PartyEncounter.cpp:519` | group1, dungeon2-3 |
| Raid | `Raid.TankStance` | 0.001f | float | - | `Encounters/PartyEncounter.cpp:456` | group1, dungeon2-3 |
| Raid | `Raid.Idle` | 0.001f | float | - | `Encounters/Encounters.h:104`, `Encounters/PartyEncounter.cpp:534` | group1, dungeon2-3 |
| Raid | `Raid.IdleMs` | 4000 | uint32 | - | `Encounters/PartyEncounter.cpp:531` | group1, dungeon2-3 |
| Raid | `Raid.IdleReach` | 40.0f | float | - | `Encounters/PartyEncounter.cpp:429` | group1, dungeon2-3 |
| Duel | `Duel.MeleeRange` | 3.5f | float | - | `StageScenario.cpp:2773`, `StageScenario.cpp:2774`, `StageScenario.cpp:3790` (+4) | all |
| Duel | `Duel.RangedRange` | 25.0f | float | - | `Rewards/CombatReward.cpp:90` | all |
| Difficulty | `Difficulty.RaiseAbove` | 0.9f | float | - | `Encounters/DifficultyLadder.cpp:87` | combat1-3, group1_roles |
| Difficulty | `Difficulty.LowerBelow` | 0.6f | float | - | `Encounters/DifficultyLadder.cpp:89` | combat1-3, group1_roles |
| Difficulty | `Difficulty.Window` | 200 | uint32 | Load: >= 1 | `Encounters/DifficultyLadder.cpp:82` | combat1-3, group1_roles |
| Difficulty | `Difficulty.ReviewChance` | 25 | int32 | Load: 0..100 | `Encounters/DifficultyLadder.cpp:51` | combat1-3, group1_roles |
| Difficulty | `Difficulty.StretchChance` | 10 | int32 | - | `Encounters/DifficultyLadder.cpp:56` | combat1-3, group1_roles |
| Difficulty | `Difficulty.CasterChance` | 40 | uint32 | - | `Encounters/CombatEncounter.cpp:252` | combat1-3 |
| Difficulty | `Difficulty.TierScale` | 0.25f | float | - | `Encounters/CombatEncounter.cpp:576`, `Encounters/InstanceEncounter.cpp:2731`, `Encounters/RolesEncounter.cpp:560` | combat1-3, group1, dungeon2-3 |
| Instance | `Instance.MaxTierScale` | 6 | uint32 | - | `Encounters/InstanceEncounter.cpp:2731` | dungeon2-3 |
| Instance | `Instance.WingTrashKill` | 1.0f | float | - | `Encounters/InstanceEncounter.cpp:2607` | dungeon2-3 |
| Instance | `Instance.WingBoss` | 25.0f | float | - | `Encounters/InstanceEncounter.cpp:2682` | dungeon2, dungeon3 |
| Instance | `Instance.WingMidBoss` | 8.0f | float | - | `Encounters/InstanceEncounter.cpp:2609` | dungeon2-3 |
| Instance | `Instance.WingDeath` | 3.0f | float | - | `Encounters/InstanceEncounter.cpp:2636` | dungeon2-3 |
| Instance | `Instance.WingWipe` | 5.0f | float | - | `Encounters/InstanceEncounter.cpp:2640` | dungeon2-3 |
| Instance | `Instance.WingWipes` | 2 | uint32 | - | `Encounters/InstanceEncounter.cpp:497` | dungeon2-3 |
| Instance | `Instance.WingStall` | 0.1f | float | - | `Encounters/InstanceEncounter.cpp:2555` | dungeon2-3 |
| Instance | `Instance.WingStallOthers` | 0.2f | float | - | `Encounters/InstanceEncounter.cpp:2555` | dungeon2-3 |
| Instance | `Instance.WingEngage` | 1.0f | float | - | `Encounters/InstanceEncounter.cpp:2563` | dungeon2-3 |
| Instance | `Instance.WingReadyShare` | 0.8f | float | - | `Encounters/InstanceEncounter.cpp:739` | dungeon2-3 |
| Instance | `Instance.WingStallGraceMs` | 60000 | uint32 | - | `Encounters/InstanceEncounter.cpp:2554` | dungeon2-3 |
| Instance | `Instance.WingTimeout` | 30.0f | float | - | `Encounters/InstanceEncounter.cpp:2676`, `Encounters/InstanceEncounter.cpp:2691` | dungeon2-3 |
| Instance | `Instance.WingClear` | 25.0f | float | - | `Encounters/InstanceEncounter.cpp:2687` | dungeon2, dungeon3 (full clear) |
| Instance | `Instance.WingClock` | 0.002f | float | - | `Encounters/InstanceEncounter.cpp:2557` | dungeon2-3 |
| Instance | `Instance.WingProbe` | 0.2f | float | - | `Encounters/InstanceEncounter.cpp:496` | dungeon2-3 |
| Instance | `Instance.WingRungRuns` | 40 | uint32 | reader: max(1, ..) (StageScenario.cpp:1712, WingLadder.cpp:31) | `StageScenario.cpp:302`, `StageScenario.cpp:1712` | dungeon2-3 |
| Instance | `Instance.WingRungTarget` | 0.6f | float | - | `StageScenario.cpp:302`, `StageScenario.cpp:1713` | dungeon2-3 |
| Instance | `Instance.WingRungStart` | 0 | uint32 | reader: min(.., rungs-1) (WingLadder.cpp:32) | `StageScenario.cpp:303` | dungeon2-3 |
| Instance | `Instance.WingSupplies` | 60 | uint32 | - | `Encounters/InstanceEncounter.cpp:687` | dungeon2-3 |
| Instance | `Instance.WingTrace` | 1 | uint32 | - | `Encounters/InstanceEncounter.cpp:407`, `Encounters/InstanceEncounter.cpp:423`, `Encounters/InstanceEncounter.cpp:768` (+5) | dungeon2-3 |
| Instance | `Instance.WingCrowd` | 0.15f | float | - | `Encounters/InstanceEncounter.cpp:2582` | dungeon2-3 |
| Instance | `Instance.WingCrowdFree` | 4 | uint32 | - | `Encounters/InstanceEncounter.cpp:1107`, `Encounters/InstanceEncounter.cpp:2581`, `Encounters/InstanceEncounter.cpp:2582` | dungeon2-3 |
| Instance | `Instance.WingStray` | 0.02f | float | - | `Encounters/InstanceEncounter.cpp:2600` | dungeon2-3 |
| Instance | `Instance.WingAway` | 0.02f | float | - | `Encounters/InstanceEncounter.cpp:2603` | dungeon2-3 |
| Instance | `Instance.WingStrayYards` | 25.0f | float | - | `Encounters/InstanceEncounter.cpp:2599` | dungeon2-3 |
| Actions | `Actions.RepeatMs` | 1000 | uint32 | - | `Layout/SeatMemory.cpp:147` | all |
| Actions | `Actions.MoveRepeatMs` | 300 | uint32 | - | `Layout/SeatMemory.cpp:147` | all |
| Actions | `Actions.StopCastMinMs` | 500 | uint32 | - | `Layout/SeatMemory.cpp:130` | all |
| Actions | `Actions.RecastAfterStopMs` | 2000 | uint32 | - | `Layout/SeatMemory.cpp:176` | all |
| Actions | `Actions.ModeLockMs` | 5000 | uint32 | - | `Layout/SeatMemory.cpp:135` | all |
| Actions | `Actions.Repeat` | 0.03f | float | - | `StageScenario.cpp:4995` | all |
| Actions | `Actions.RepeatWindowMs` | 10000 | uint32 | - | `StageScenario.cpp:3748` | all |
| Actions | `Actions.RepeatFree` | 3 | uint32 | - | `StageScenario.cpp:3750` | all |
| Actions | `Actions.Jitter` | 0.05f | float | - | `StageScenario.cpp:4999`, `Blocks/MoveControls.h:151` | all |
| Actions | `Actions.Aimless` | 0.02f | float | - | `StageScenario.cpp:4005`, `StageScenario.cpp:4576` | all |
| Actions | `Actions.Aimless.OffFocus` | 0.02f | float | - | `StageScenario.cpp:3982` | all |
| Actions | `Actions.Aimless.AoeMissed` | 0.02f | float | - | `StageScenario.cpp:3983` | all |
| Actions | `Actions.Aimless.InRangeCast` | 0.02f | float | - | `StageScenario.cpp:3984` | all |
| Actions | `Actions.Aimless.UnprovokedHarm` | 0.02f | float | - | `StageScenario.cpp:3985` | all |
| Actions | `Actions.Aimless.HelpOffGoal` | 0.02f | float | - | `StageScenario.cpp:3986` | all |
| Actions | `Actions.Aimless.StepAway` | 0.02f | float | - | `StageScenario.cpp:3987` | all |
| Actions | `Actions.Aimless.TargetSwitch` | 0.04f | float | - | `StageScenario.cpp:3988` | all |
| Actions | `Actions.Aimless.PetOffGoal` | 0.04f | float | - | `StageScenario.cpp:3989` | all |
| Actions | `Actions.Aimless.ConsumeNotNeeded` | 0.03f | float | - | `StageScenario.cpp:3990` | all |
| Actions | `Actions.Aimless.TrapNoEnemy` | 0.03f | float | - | `StageScenario.cpp:3991` | all |
| Actions | `Actions.Aimless.ModeFlip` | 0.03f | float | - | `StageScenario.cpp:3992` | all |
| Actions | `Actions.Aimless.ModeReverse` | 0.06f | float | - | `StageScenario.cpp:3993` | all |
| Actions | `Actions.Aimless.NeedlessMove` | 0.02f | float | - | `StageScenario.cpp:3994` | all |
| Actions | `Actions.Aimless.TauntOffRole` | 0.15f | float | - | `StageScenario.cpp:3995` | all |
| Actions | `Actions.Aimless.TankModeOffRole` | 0.04f | float | - | `StageScenario.cpp:3996` | all |
| Actions | `Actions.Aimless.CastFailed` | 0.02f | float | - | `StageScenario.cpp:4001` | all |
| Actions | `Actions.Aimless.ActRefused` | 0.02f | float | - | `StageScenario.cpp:4002` | all |
| Actions | `Actions.ModeSwitch` | 0.01f | float | - | `StageScenario.cpp:4577`, `StageState.h:431` | all |
| Actions | `Actions.SupplySpent` | 0.02f | float | - | `StageScenario.cpp:4580` | all |
| Actions | `Actions.ConsumeFullPct` | 85.0f | float | - | `StageScenario.cpp:4305` | all |
| Actions | `Actions.Effort` | 0.004f | float | - | `StageScenario.cpp:4579`, `Blocks/MoveControls.h:156` | all |
| Actions | `Actions.Fidget` | 0.01f | float | - | `StageScenario.cpp:4584` | all |
| Actions | `Actions.SettleGraceMs` | 500 | uint32 | - | `StageScenario.cpp:4529`, `StageScenario.cpp:4556` | all |
| Actions | `Actions.IntentSlackYards` | 0.5f | float | - | `StageScenario.cpp:4456`, `StageScenario.cpp:4459` | all |
| Goals | `Goals.Reached` | 0.05f | float | - | `StageScenario.cpp:3898`, `Rewards/RewardLedger.h:49` | all |
| Goals | `Goals.Switch` | 0.15f | float | - | `StageScenario.cpp:2672`, `StageScenario.cpp:5036`, `StageState.h:307` (+1) | all |
| Goals | `Goals.Progress` | 0.5f | float | - | `StageScenario.cpp:5026`, `Rewards/RewardLedger.h:51` | all |
| Goals | `Goals.ProgressGamma` | 0.999f | float | - | `StageScenario.cpp:5027` | all |
| Goals | `Goals.FightValue` | 0.3f | float | - | `StageScenario.cpp:3879` | all |
| Goals | `Goals.ControlValue` | 0.2f | float | - | `StageScenario.cpp:3880` | all |
| Goals | `Goals.RecoverValue` | 1.0f | float | - | `StageScenario.cpp:3895` | all |
| Goals | `Goals.ProtectValue` | 1.0f | float | - | `StageScenario.cpp:3881` | all |
| Goals | `Goals.ProtectHoldMs` | 5000 | uint32 | - | `StageScenario.cpp:3638` | all |
| Goals | `Goals.TravelValue` | 0.1f | float | - | `StageScenario.cpp:3882` | all |
| Goals | `Goals.SecondaryShare` | 0.5f | float | - | `StageScenario.cpp:3650`, `StageScenario.cpp:5026` | all |
| Goals | `Goals.Secondary` | 0.002f | float | - | `StageScenario.cpp:5033` | all |
| Support | `Support.SelfHealing` | 0.5f | float | - | `StageScenario.cpp:5048` | all |
| Support | `Support.HealingMana` | 0.1f | float | - | `StageScenario.cpp:5053` | all |
| Options | `Options.RestMaxMs` | 30000 | uint32 | - | `Blocks/GauntletBlock.cpp:113` | combat3, group1, dungeon2-3 (Gauntlet block) |
| Options | `Options.HoldInterruptMs` | 10000 | uint32 | - | `Blocks/PackBlock.cpp:120` | dungeon2-3 (Pack block) |
| Hazards | `Hazards.Damage` | 0.5f | float | - | `StageScenario.cpp:4951` | all |
| Hazards | `Hazards.Standing` | 0.15f | float | - | `StageScenario.cpp:4943` | all |
| Hazards | `Hazards.Max` | 3.0f | float | - | `StageScenario.cpp:4942`, `StageScenario.cpp:4950` | all |
| Markers | `Markers.Arrive` | 3.0f | float | - | `Encounters/SightEncounter.cpp:416` | move1_controls |
| Markers | `Markers.StepCost` | 0.002f | float | - | `Encounters/SightEncounter.cpp:319` | move1_controls |
| Markers | `Markers.Death` | 3.0f | float | - | `Encounters/SightEncounter.cpp:350` | move1_controls |
| Markers | `Markers.Progress` | 1.0f | float | - | `Encounters/SightEncounter.cpp:386` | move1_controls |
| Markers | `Markers.Facing` | 0.25f | float | - | `Encounters/SightEncounter.cpp:391` | move1_controls |
| Markers | `Markers.StopMoved` | 0.05f | float | - | `Encounters/InteractEncounter.cpp:649`, `Encounters/SeekEncounter.cpp:543`, `Encounters/SightEncounter.cpp:404` | move1, move2, move3 |
| Markers | `Markers.StopNear` | 10.0f | float | - | `Encounters/SightEncounter.cpp:405` | move1_controls |
| Markers | `Markers.ArriveRise` | 2.0f | float | - | `Encounters/SightEncounter.cpp:396` | move1_controls |
| Respawn | `Respawn.DelayMs` | 10000 | uint32 | - | `Encounters/CombatEncounter.cpp:412`, `Encounters/InstanceEncounter.cpp:1004`, `Encounters/PartyFollowEncounter.cpp:446` (+1) | move4, combat1-3, group1, dungeon2-3 |
| Respawn | `Respawn.RejoinYards` | 15.0f | float | - | `Encounters/CombatEncounter.cpp:412`, `Encounters/InstanceEncounter.cpp:1004`, `Encounters/PartyFollowEncounter.cpp:446` (+1) | move4, combat1-3, group1, dungeon2-3 |
| PartyFollow | `PartyFollow.BandMin` | 3.0f | float | - | `Encounters/PartyFollowEncounter.cpp:664` | move4_follow |
| PartyFollow | `PartyFollow.BandMax` | 10.0f | float | - | `Encounters/PartyFollowEncounter.cpp:664` | move4_follow |
| PartyFollow | `PartyFollow.LostYards` | 40.0f | float | - | `Encounters/PartyFollowEncounter.cpp:664` | move4_follow |
| PartyFollow | `PartyFollow.Kept` | 0.02f | float | - | `Encounters/PartyFollowEncounter.cpp:668` | move4_follow |
| PartyFollow | `PartyFollow.Lost` | 0.02f | float | - | `Encounters/PartyFollowEncounter.cpp:673` | move4_follow |
| PartyFollow | `PartyFollow.Regroup` | 0.5f | float | - | `Encounters/PartyFollowEncounter.cpp:685` | move4_follow |
| PartyFollow | `PartyFollow.RegroupWindow` | 20.0f | float | - | `Encounters/PartyFollowEncounter.cpp:685`, `Encounters/PartyFollowEncounter.cpp:687` | move4_follow |
| PartyFollow | `PartyFollow.RegroupMinStopMs` | 2000 | uint32 | - | `Encounters/PartyFollowEncounter.cpp:329` | move4_follow |
| PartyFollow | `PartyFollow.Blocking` | 0.05f | float | - | `Encounters/PartyFollowEncounter.cpp:698` | move4_follow |
| PartyFollow | `PartyFollow.BlockYards` | 2.5f | float | - | `Encounters/PartyFollowEncounter.cpp:695` | move4_follow |
| PartyFollow | `PartyFollow.BlockHalfAngle` | 45.0f | float | - | `Encounters/PartyFollowEncounter.cpp:695` | move4_follow |
| PartyFollow | `PartyFollow.Death` | 3.0f | float | - | `Encounters/PartyFollowEncounter.cpp:623` | move4_follow |
| PartyFollow | `PartyFollow.MinimapYards` | 60.0f | float | - | `StageScenario.cpp:3211`, `Encounters/PartyFollowEncounter.cpp:573` | move4, group1, dungeon2-3 (every PartyFrames stage) |
| PartyFollow | `PartyFollow.WalkRungs` | 1 | uint32 | - | `Encounters/PartyFollowEncounter.cpp:429` | move4_follow |
| PartyFollow | `PartyFollow.SuddenFromRung` | 2 | uint32 | - | `Encounters/PartyFollowEncounter.cpp:312` | move4_follow |
| PartyFollow | `PartyFollow.BackStepFromRung` | 3 | uint32 | - | `Encounters/PartyFollowEncounter.cpp:318`, `Encounters/PartyFollowEncounter.cpp:360` | move4_follow |
| PartyFollow | `PartyFollow.StopSecondsFirst` | 8.0f | float | - | `Encounters/PartyFollowEncounter.cpp:390` | move4_follow |
| PartyFollow | `PartyFollow.StopSecondsLast` | 3.0f | float | - | `Encounters/PartyFollowEncounter.cpp:390` | move4_follow |
| PartyFollow | `PartyFollow.SuddenStopMinMs` | 1000 | uint32 | - | `Encounters/PartyFollowEncounter.cpp:359` | move4_follow |
| PartyFollow | `PartyFollow.SuddenStopMaxMs` | 4000 | uint32 | reader: max(Min, Max) | `Encounters/PartyFollowEncounter.cpp:359` | move4_follow |
| PartyFollow | `PartyFollow.SuddenGapMin` | 12.0f | float | - | `Encounters/PartyFollowEncounter.cpp:319` | move4_follow |
| PartyFollow | `PartyFollow.SuddenGapMax` | 30.0f | float | reader: max(Min, Max) | `Encounters/PartyFollowEncounter.cpp:319` | move4_follow |
| PartyFollow | `PartyFollow.BackStepYards` | 4.0f | float | - | `Encounters/PartyFollowEncounter.cpp:364`, `Encounters/PartyFollowEncounter.cpp:365` | move4_follow |
| PartyFollow | `PartyFollow.BackStepChance` | 30 | int32 | - | `Encounters/PartyFollowEncounter.cpp:360` | move4_follow |
| PartyFollow | `PartyFollow.GiveUpMs` | 6000 | uint32 | - | `Encounters/PartyFollowEncounter.cpp:375`, `Encounters/PartyFollowEncounter.cpp:381` | move4_follow |
| PartyFollow | `PartyFollow.CastShare` | 0 | int32 | - | `Encounters/PartyFollowEncounter.cpp:260` | move4_follow |
| Seek | `Seek.Arrive` | 3.0f | float | - | `Encounters/SeekEncounter.cpp:550` | move2_seek |
| Seek | `Seek.StepCost` | 0.0005f | float | - | `Encounters/SeekEncounter.cpp:441` | move2_seek |
| Seek | `Seek.Death` | 6.0f | float | - | `Encounters/SeekEncounter.cpp:468` | move2_seek |
| Seek | `Seek.ArriveRise` | 2.0f | float | - | `Encounters/SeekEncounter.cpp:541` | move2_seek |
| Seek | `Seek.Sighting` | 0.5f | float | - | `Encounters/SeekEncounter.cpp:503` | move2_seek |
| Seek | `Seek.NewGround` | 0.004f | float | - | `Encounters/SeekEncounter.cpp:523` | move2_seek |
| Seek | `Seek.NewGroundCell` | 4.0f | float | reader: max(1.0, ..) | `Encounters/SeekEncounter.cpp:522` | move2_seek |
| Seek | `Seek.Stuck` | 0.02f | float | - | `Encounters/PartyFollowEncounter.cpp:645`, `Encounters/SeekEncounter.cpp:483` | move2, move4 |
| Seek | `Seek.Wall` | 0.02f | float | - | `Encounters/PartyFollowEncounter.cpp:651`, `Encounters/SeekEncounter.cpp:489` | move2, move4 |
| Seek | `Seek.WallSlide` | 0.5f | float | - | `Encounters/PartyFollowEncounter.cpp:652`, `Encounters/SeekEncounter.cpp:490` | move2, move4 |
| Seek | `Seek.Attempts` | 24 | uint32 | reader: max(1, ..) | `Encounters/SeekEncounter.cpp:269`, `Encounters/SeekEncounter.cpp:281`, `Encounters/SeekEncounter.cpp:318` | move2_seek |
| Seek | `Seek.FloorTolerance` | 2.0f | float | - | `Encounters/SeekEncounter.cpp:274`, `Encounters/SeekEncounter.cpp:286`, `Encounters/SeekEncounter.cpp:296` | move2_seek |
| Seek | `Seek.Clearance` | 0.8f | float | - | `Encounters/SeekEncounter.cpp:252` | move2_seek |
| Seek | `Seek.RoomSeen` | 0.1f | float | - | `Encounters/SeekEncounter.cpp:515` | move2_seek |
| Seek | `Seek.RoomSeenRays` | 3 | uint32 | - | `Encounters/SeekEncounter.cpp:511` | move2_seek |
| Seek | `Seek.CarryShare` | 0.1f | float | - | `Encounters/SeekEncounter.cpp:374` | move2_seek |
| Seek | `Seek.RungSeconds0` | 90 | uint32 | - | `Encounters/SeekEncounter.cpp:71` | move2_seek |
| Seek | `Seek.RungSeconds1` | 120 | uint32 | - | `Encounters/SeekEncounter.cpp:71` | move2_seek |
| Seek | `Seek.RungSeconds2` | 200 | uint32 | - | `Encounters/SeekEncounter.cpp:71` | move2_seek |
| Seek | `Seek.RungSeconds3` | 300 | uint32 | - | `Encounters/SeekEncounter.cpp:71` | move2_seek |
| Seek | `Seek.HallwayNearest` | 8.0f | float | - | `Encounters/SeekEncounter.cpp:315` | move2_seek |
| Seek | `Seek.HallwayFurthest` | 120.0f | float | - | `Encounters/SeekEncounter.cpp:316` | move2_seek |
| Seek | `Seek.DoorwayInside` | 2.0f | float | - | `Encounters/SeekEncounter.cpp:271` | move2_seek |
| Seek | `Seek.DoorwayDeeper` | 1.5f | float | - | `Encounters/SeekEncounter.cpp:271` | move2_seek |
| Seek | `Seek.DoorwaySpread` | 1.0f | float | - | `Encounters/SeekEncounter.cpp:272` | move2_seek |
| Seek | `Seek.Goals` | 1 | uint32 | temporary experiment switch: 0 offers no room goals, pays no aid, restores the dud assignment goal | `Encounters/SeekEncounter.cpp:493`, `Encounters/SeekEncounter.cpp:521` | move2_seek |
| Seek | `Seek.GoalsFromRung` | 2 | uint32 | room goals only at a placement rung >= this (after the carry-over) | `Encounters/SeekEncounter.cpp:493` | move2_seek |
| Seek | `Seek.RoomGoal` | 0.05f | float | Aid; x `Goals.SecondaryShare` for a secondary | `StageScenario.cpp:3637` | move2_seek |
| Seek | `Seek.RoomSwitch` | 0.03f | float | Aid; charged per room goal (or the way on) given up | `StageScenario.cpp:5012` | move2_seek |
| Seek | `Seek.Return` | 0.05f | float | Cost, `AddFixed` | `Encounters/SeekEncounter.cpp:649` | move2_seek |
| Seek | `Seek.AidUntil` | 0.4f | float | the aid scale is `max(0, 1 - progress / AidUntil)`; 0 or less is no aid | `StageScenario.cpp:4646` | move2_seek |
| Seek | `Seek.GlimpseRays` | 1 | uint32 | reader: max(1, ..) | `Encounters/SeekEncounter.cpp:611` | move2_seek |
| Seek | `Seek.CheckedShare` | 0.6f | float | above 1 reduces the check to "stood in it" | `Encounters/SeekEncounter.cpp:625` | move2_seek |
| Seek | `Seek.EnterDwellMs` | 1000 | uint32 | - | `Encounters/SeekEncounter.cpp:621` | move2_seek |
| Seek | `Seek.ReturnAwayYards` | 8.0f | float | - | `Encounters/SeekEncounter.cpp:656` | move2_seek |
| Seek | `Seek.ReturnAwayMs` | 2000 | uint32 | - | `Encounters/SeekEncounter.cpp:660` | move2_seek |
| Seek | `Seek.GoalSource` | 1 | uint32 | temporary A/B switch (free choice goals): 1 = cell goals (a block of the seat's crop), 0 = the room slots and the way on; only with `Goals` 1; deleted with the loser. Echoed in stage.json `goals.cells.source` | `Encounters/SeekEncounter.cpp:593`, `Encounters/SeekEncounter.cpp:658`, `StageScenario.cpp:1499` | move2_seek |
| Seek | `Seek.CellReach` | 4.0f | float | yards, 2D: a cell goal is reached within this of its latched point; also `GoalHeld`'s reach for a cell hold | `Encounters/SeekEncounter.cpp:594`, `StageScenario.cpp:2941` | move2_seek |
| Seek | `Seek.CellRise` | 3.0f | float | yards of height for reaching (the choosable window is fixed at 4 yd: `rise_units` 16) | `Encounters/SeekEncounter.cpp:595` | move2_seek |
| Seek | `Seek.CellSame` | 6.0f | float | a new draw within this of the held, unended cell goal's point is that goal chosen again (free) | `StageScenario.cpp:2736` | move2_seek |
| Seek | `Seek.CellMinYards` | 8.0f | float | `CellGoal` is paid only for a goal chosen at least this far away | `StageScenario.cpp:3836` | move2_seek |
| Seek | `Seek.CellPatienceMs` | 20000 | uint32 | no best-distance gain of `CellPatienceYards` for this long ends the cell goal as lost | `StageScenario.cpp:3819` | move2_seek |
| Seek | `Seek.CellPatienceYards` | 2.0f | float | - | `StageScenario.cpp:3814` | move2_seek |
| Seek | `Seek.CellGoal` | 0.05f | float | Aid, once per cell goal reached (not stale, chosen >= `CellMinYards` away); x `Goals.SecondaryShare` for a secondary; `AddTaken` | `StageScenario.cpp:3839` | move2_seek |
| Seek | `Seek.CellProgress` | 0.004f | float | Aid, per yard of new best closeness to the held primary's point (a ratchet), not stale | `StageScenario.cpp:5271` | move2_seek |
| Seek | `Seek.CellSwitch` | 0.02f | float | Cost, `AddFixed`; a cell goal given up for another before it ended | `StageScenario.cpp:5261` | move2_seek |
| Seek | `Seek.CellLost` | 0.03f | float | Cost, `AddTaken`; a cell goal ended by the patience rule or an invalid latch (seat alive) | `StageScenario.cpp:3868` | move2_seek |
| Seek | `Seek.CellStale` | 0.01f | float | Cost, `AddFixed`; a block chosen that the seat had stood on (not a free re-choice) | `StageScenario.cpp:2827` | move2_seek |
| Seek | `Seek.ExploreSeen` | 0.002f | float | Exploring; per newly seen 2-yd floor cell, x `ExploreRoomBonus` in a room not yet entered; paid x `max(shaping scale, ExploreFloor)`; not in the score | `Encounters/SeekEncounter.cpp` (`Explore`) | move2_seek |
| Seek | `Seek.ExploreCap` | 1.0f | float | episode cap on the nominal Explore sum (before the scale and the floor) | `Encounters/SeekEncounter.cpp` (`Explore`) | move2_seek |
| Seek | `Seek.ExploreFloor` | 0.5f | float | the least Explore and FrontierPull are paid at whatever the shaping scale (`RewardLedger::SetShapingFloor`); 0 is the old fade | `Encounters/SeekEncounter.cpp` (`Reward`) | move2_seek |
| Seek | `Seek.ExploreRoomBonus` | 2.0f | float | weight of a cell in a table room the seat has not entered | `Encounters/SeekEncounter.cpp` (`Explore`) | move2_seek |
| Seek | `Seek.FrontierPull` | 0.004f | float | Exploring; per yard closed on the nearest frontier, best-distance ratchet per cluster; 0 switches it off | `Encounters/SeekEncounter.cpp` (`Explore`) | move2_seek |
| Seek | `Seek.FrontierCap` | 1.0f | float | episode cap on the nominal FrontierPull sum | `Encounters/SeekEncounter.cpp` (`Explore`) | move2_seek |
| Seek | `Seek.CircleWindowMs` | 6000 | uint32 | sliding window of decisions the circling rule reads; 0 switches Circling off | `Encounters/SeekEncounter.cpp` (`Circle`) | move2_seek |
| Seek | `Seek.CircleYards` | 12.0f | float | path in the window that counts as circling when the net displacement is under `CircleNetYards` | `Encounters/SeekEncounter.cpp` (`Circle`) | move2_seek |
| Seek | `Seek.CircleNetYards` | 4.0f | float | net displacement over the window under which path or turning is circling | `Encounters/SeekEncounter.cpp` (`Circle`) | move2_seek |
| Seek | `Seek.CircleTurnDeg` | 540.0f | float | summed absolute yaw change in the window that counts as circling (net displacement under `CircleNetYards`) | `Encounters/SeekEncounter.cpp` (`Circle`) | move2_seek |
| Seek | `Seek.Circling` | 0.02f | float | Cost, `AddFixed` per second while the rule holds (not on a decision that paid Stuck); 0 switches it off | `Encounters/SeekEncounter.cpp` (`Circle`) | move2_seek |
| Seek | `Seek.TrapShare` | 0.12f | float | probability a training (unseeded, non-sweep) episode starts in a trap pose against a door jamb | `Encounters/SeekEncounter.cpp` (`Build`) | move2_seek |
| Seek | `Seek.Escape` | 0.3f | float | Aid, once: the trap seat is `TrapEscapeYards` from its start within `TrapEscapeMs` | `Encounters/SeekEncounter.cpp` (`Reward`) | move2_seek |
| Seek | `Seek.TrapEscapeYards` | 6.0f | float | 2D yards from the trap pose that count as out | `Encounters/SeekEncounter.cpp` (`Reward`) | move2_seek |
| Seek | `Seek.TrapEscapeMs` | 20000 | uint32 | episode clock by which the seat must be out | `Encounters/SeekEncounter.cpp` (`Reward`) | move2_seek |
| Interact | `Interact.Arrive` | 3.0f | float | - | `Encounters/InteractEncounter.cpp:643`, `Encounters/InteractEncounter.cpp:678` | move3_interact |
| Interact | `Interact.DoorOpened` | 1.0f | float | - | `Encounters/InteractEncounter.cpp:629` | move3_interact |
| Interact | `Interact.WrongObject` | 0.5f | float | - | `Encounters/InteractEncounter.cpp:634`, `Encounters/InteractEncounter.cpp:685` | move3_interact |
| Interact | `Interact.StepCost` | 0.0005f | float | - | `Encounters/InteractEncounter.cpp:557` | move3_interact |
| Interact | `Interact.Death` | 6.0f | float | - | `Encounters/InteractEncounter.cpp:584` | move3_interact |
| Interact | `Interact.ArriveRise` | 2.0f | float | - | `Encounters/InteractEncounter.cpp:657` | move3_interact |
| Interact | `Interact.Sighting` | 0.5f | float | - | `Encounters/InteractEncounter.cpp:621` | move3_interact |
| Interact | `Interact.Stuck` | 0.02f | float | - | `Encounters/InteractEncounter.cpp:598` | move3_interact |
| Interact | `Interact.Wall` | 0.02f | float | - | `Encounters/InteractEncounter.cpp:604` | move3_interact |
| Interact | `Interact.WallSlide` | 0.5f | float | - | `Encounters/InteractEncounter.cpp:605` | move3_interact |
| Interact | `Interact.CarryShare` | 0.1f | float | - | `Encounters/InteractEncounter.cpp:321` | move3_interact |
| Interact | `Interact.RungSeconds0` | 60 | uint32 | - | `Encounters/InteractEncounter.cpp:61` | move3_interact |
| Interact | `Interact.RungSeconds1` | 120 | uint32 | - | `Encounters/InteractEncounter.cpp:61` | move3_interact |
| Interact | `Interact.RungSeconds2` | 90 | uint32 | - | `Encounters/InteractEncounter.cpp:61` | move3_interact |
| Interact | `Interact.DecoysMin` | 2 | uint32 | - | `Encounters/InteractEncounter.cpp:374` | move3_interact |
| Interact | `Interact.DecoysMax` | 4 | uint32 | - | `Encounters/InteractEncounter.cpp:374` | move3_interact |
| Interact | `Interact.Spacing` | 2.5f | float | - | `Encounters/InteractEncounter.cpp:412` | move3_interact |
| Interact | `Interact.SightNearest` | 4.0f | float | - | `Encounters/InteractEncounter.cpp:420` | move3_interact |
| Interact | `Interact.SightFurthest` | 30.0f | float | - | `Encounters/InteractEncounter.cpp:420` | move3_interact |
| Interact | `Interact.Attempts` | 64 | uint32 | reader: max(1, ..) | `Encounters/InteractEncounter.cpp:407` | move3_interact |
| Interact | `Interact.SummonSweep` | 80.0f | float | - | `Encounters/InteractEncounter.cpp:281` | move3_interact |
| Controls | `Controls.Nearest` | 10.0f | float | - | `Encounters/SightEncounter.cpp:257` | move1_controls |
| Controls | `Controls.Furthest` | 120.0f | float | - | `Encounters/SightEncounter.cpp:258` | move1_controls |
| Controls | `Controls.ArriveTolerance` | 1.0f | float | reader: max(0.1, ..) | `Encounters/SightEncounter.cpp:290` | move1_controls |
| Controls | `Controls.CornerShare` | 0.25f | float | - | `Encounters/SightEncounter.cpp:262` | move1_controls |
| Controls | `Controls.CornerFrom` | 0.75f | float | - | `Encounters/SightEncounter.cpp:262` | move1_controls |
| Controls | `Controls.CornerStep` | 8.0f | float | - | `Encounters/SightEncounter.cpp:260` | move1_controls |
| Controls | `Controls.Attempts` | 64 | uint32 | reader: max(1, ..) | `Encounters/SightEncounter.cpp:261` | move1_controls |
| Controls | `Controls.Withhold0` | 0.25f | float | - | `Encounters/SightEncounter.cpp:219` | move1_controls |
| Controls | `Controls.Withhold1` | 0.6f | float | - | `Encounters/SightEncounter.cpp:219` | move1_controls |
| Controls | `Controls.Withhold2` | 0.9f | float | - | `Encounters/SightEncounter.cpp:219` | move1_controls |
| Controls | `Controls.Withhold3` | 0.9f | float | - | `Encounters/SightEncounter.cpp:220` | move1_controls |
| Controls | `Controls.Stuck` | 0.02f | float | - | `Encounters/SightEncounter.cpp:365` | move1_controls |
| Controls | `Controls.Wall` | 0.02f | float | - | `Encounters/SightEncounter.cpp:371` | move1_controls |
| Controls | `Controls.WallSlide` | 0.5f | float | - | `Encounters/SightEncounter.cpp:372` | move1_controls |
| Combat | `Combat.Kill` | 1.0f | float | - | `Encounters/CombatEncounter.cpp:584`, `Encounters/CombatEncounter.cpp:585` | combat1-3 |
| Combat | `Combat.Clear` | 2.0f | float | - | `Encounters/CombatEncounter.cpp:589`, `Encounters/CombatEncounter.cpp:590` | combat1-3 |
| Combat | `Combat.Survived` | 1.0f | float | - | `Encounters/CombatEncounter.cpp:684` | combat1-3 |
| Combat | `Combat.SurviveSurvived` | 2.0f | float | - | `Encounters/CombatEncounter.cpp:684` | combat1-3 |
| Combat | `Combat.InterruptLanded` | 0.25f | float | - | `Encounters/CombatEncounter.cpp:603`, `Encounters/CombatEncounter.cpp:604` | combat1-3 |
| Combat | `Combat.Away` | 0.02f | float | - | `Encounters/CombatEncounter.cpp:660` | combat1-3 |
| Combat | `Combat.AwayYards` | 30.0f | float | - | `Encounters/CombatEncounter.cpp:657` | combat1-3 |
| Combat | `Combat.Death` | 2.0f | float | - | `Encounters/CombatEncounter.cpp:652` | combat1-3 |
| Combat | `Combat.AllyDeath` | 1.0f | float | - | `Encounters/CombatEncounter.cpp:614` | combat1-3 |
| Combat | `Combat.Hurt` | 0.2f | float | - | `Encounters/CombatEncounter.cpp:620` | combat1-3 |
| Combat | `Combat.FireHurt` | 1.0f | float | - | `Encounters/CombatEncounter.cpp:622` | combat1-3 |
| Combat | `Combat.ExtraPull` | 1.0f | float | - | `Encounters/CombatEncounter.cpp:612` | combat1-3 |
| Combat | `Combat.Clock` | 0.01f | float | - | `Encounters/CombatEncounter.cpp:624` | combat1-3 |
| Combat | `Combat.Damage` | 0.3f | float | - | `Encounters/CombatEncounter.cpp:638` | combat1-3 |
| Combat | `Combat.MaxTier` | 5 | uint32 | reader: max(1, ..) at one of four uses | `Encounters/CombatEncounter.cpp:130`, `Encounters/CombatEncounter.cpp:204`, `Encounters/CombatEncounter.cpp:687` (+1) | combat1-3 |
| Combat | `Combat.LevelBase` | -2 | int32 | - | `Encounters/CombatDraw.h:55`, `Encounters/CombatDraw.h:62`, `Encounters/CombatDraw.h:70` | combat1-3 |
| Combat | `Combat.LevelsPerTier` | 1 | uint32 | - | `Encounters/CombatDraw.h:51` | combat1-3 |
| Combat | `Combat.EliteTier` | 4 | uint32 | - | `Encounters/CombatDraw.h:57`, `Encounters/CombatDraw.h:64`, `Encounters/CombatDraw.h:72` | combat1-3 |
| Combat | `Combat.CasterTier` | 1 | uint32 | - | `Encounters/CombatDraw.h:58`, `Encounters/CombatDraw.h:65`, `Encounters/CombatDraw.h:73` | combat1-3 |
| Combat | `Combat.LinkedTier` | 2 | uint32 | - | `Encounters/CombatDraw.h:67` | combat1-3 |
| Combat | `Combat.HazardChance` | 33 | int32 | - | `Encounters/CombatEncounter.cpp:251` | combat1-3 |
| Combat | `Combat.SurviveSize` | 3 | uint32 | reader: max(1, ..) | `Encounters/CombatDraw.h:71` | combat1-3 |
| Combat | `Combat.SurviveLevels` | 2 | uint32 | - | `Encounters/CombatDraw.h:70` | combat1-3 |
| Combat | `Combat.FightNearest` | 28.0f | float | - | `Encounters/CombatEncounter.cpp:288`, `Encounters/RolesEncounter.cpp:296` | combat1-3, group1 |
| Combat | `Combat.FightFurthest` | 40.0f | float | - | `Encounters/CombatEncounter.cpp:288`, `Encounters/RolesEncounter.cpp:296` | combat1-3, group1 |
| Combat | `Combat.NextNearest` | 30.0f | float | - | `Encounters/CombatEncounter.cpp:285`, `Encounters/RolesEncounter.cpp:287` | combat1-3, group1 |
| Combat | `Combat.NextFurthest` | 45.0f | float | - | `Encounters/CombatEncounter.cpp:286` | combat1-3 |
| Combat | `Combat.NextFightMs` | 2000 | uint32 | - | `Encounters/CombatEncounter.cpp:563` | combat1-3 |
| Combat | `Combat.CorridorWalk` | 220.0f | float | - | `Encounters/CombatEncounter.cpp:182`, `Encounters/CombatEncounter.cpp:184` | combat1-3, group1 |
| Combat | `Combat.CorridorSpacing` | 8.0f | float | - | `Encounters/CombatEncounter.cpp:182` | combat1-3, group1 |
| Roles | `Roles.Clear` | 1.0f | float | - | `Encounters/RolesEncounter.cpp:570` | group1_roles |
| Roles | `Roles.Hold` | 0.045f | float | - | `Encounters/RolesDraw.h:120` | group1_roles |
| Roles | `Roles.Loose` | 0.018f | float | - | `Encounters/RolesDraw.h:120` | group1_roles |
| Roles | `Roles.Focus` | 0.9f | float | - | `Encounters/RolesDraw.h:128` | group1_roles |
| Roles | `Roles.PulledOff` | 0.012f | float | - | `Encounters/RolesDraw.h:128` | group1_roles |
| Roles | `Roles.Keep` | 0.0006f | float | - | `Encounters/RolesDraw.h:152` | group1_roles |
| Roles | `Roles.KeepLow` | 0.0006f | float | - | `Encounters/RolesDraw.h:153` | group1_roles |
| Roles | `Roles.Overheal` | 0.5f | float | - | `Encounters/RolesDraw.h:153` | group1_roles |
| Roles | `Roles.PullClean` | 2.0f | float | - | `Encounters/RolesEncounter.cpp:576` | group1_roles |
| Roles | `Roles.PullExtra` | 1.5f | float | - | `Encounters/RolesEncounter.cpp:578` | group1_roles |
| Roles | `Roles.PullOthers` | 0.5f | float | - | `Encounters/RolesEncounter.cpp:574` | group1_roles |
| Roles | `Roles.Survived` | 1.0f | float | - | `Encounters/RolesEncounter.cpp:702` | group1_roles |
| Roles | `Roles.Death` | 2.0f | float | - | `Encounters/RolesEncounter.cpp:688` | group1_roles |
| Roles | `Roles.Away` | 0.02f | float | - | `Encounters/RolesEncounter.cpp:696` | group1_roles |
| Roles | `Roles.AwayYards` | 30.0f | float | - | `Encounters/RolesEncounter.cpp:693` | group1_roles |
| Roles | `Roles.Clock` | 0.01f | float | - | `Encounters/RolesEncounter.cpp:659` | group1_roles |
| Roles | `Roles.Damage` | 0.3f | float | - | `Encounters/RolesEncounter.cpp:674` | group1_roles |
| Roles | `Roles.MaxTier` | 5 | uint32 | reader: max(1, ..) at one of several uses | `Encounters/RolesEncounter.cpp:166`, `Encounters/RolesEncounter.cpp:208`, `Encounters/RolesEncounter.cpp:286` (+2) | group1_roles |
| Roles | `Roles.LevelBase` | -1 | int32 | - | `Encounters/RolesDraw.h:70` | group1_roles |
| Roles | `Roles.LevelsPerTier` | 1 | uint32 | - | `Encounters/RolesDraw.h:70` | group1_roles |
| Roles | `Roles.PackSizeFirst` | 2 | uint32 | reader: see RolesDraw.h:72 | `Encounters/RolesDraw.h:72` | group1_roles |
| Roles | `Roles.PackGrowEvery` | 2 | uint32 | reader: max(1, ..) | `Encounters/RolesDraw.h:72` | group1_roles |
| Roles | `Roles.PackSizeMax` | 4 | uint32 | reader: max(1, min(..)) | `Encounters/RolesDraw.h:71` | group1_roles |
| Roles | `Roles.CasterTier` | 1 | uint32 | - | `Encounters/RolesDraw.h:73` | group1_roles |
| Roles | `Roles.LinkedTier` | 2 | uint32 | - | `Encounters/RolesDraw.h:74` | group1_roles |
| Roles | `Roles.EliteTier` | 4 | uint32 | - | `Encounters/RolesDraw.h:75` | group1_roles |
| Roles | `Roles.KeepHealthPct` | 200 | uint32 | reader: max(1, ..) | `Encounters/RolesDraw.h:76` | group1_roles |
| Roles | `Roles.CampPacksFirst` | 2 | uint32 | reader: result max(2, ..) | `Encounters/RolesDraw.h:87` | group1_roles |
| Roles | `Roles.CampPacksMax` | 4 | uint32 | reader: result max(2, ..) | `Encounters/RolesDraw.h:87` | group1_roles |
| Roles | `Roles.CampSpacingFirst` | 45.0f | float | - | `Encounters/RolesDraw.h:98` | group1_roles |
| Roles | `Roles.CampSpacingLast` | 25.0f | float | - | `Encounters/RolesDraw.h:98` | group1_roles |
| Roles | `Roles.PartyNearest` | 2.0f | float | - | `Encounters/RolesEncounter.cpp:232` | group1_roles |
| Roles | `Roles.PartyFurthest` | 6.0f | float | - | `Encounters/RolesEncounter.cpp:232` | group1_roles |
| Roles | `Roles.WinHold` | 0.75f | float | - | `Encounters/RolesDraw.h:229` | group1_roles |
| Roles | `Roles.WinFocus` | 0.6f | float | - | `Encounters/RolesDraw.h:231` | group1_roles |
| Roles | `Roles.WinPulledSeconds` | 5.0f | float | - | `Encounters/RolesDraw.h:232` | group1_roles |
| Roles | `Roles.StandInShare` | 20 | int32 | - | `Encounters/StandInSeat.cpp:89` | group1_roles |
| Options | `Options.JitterDecayMs` | 2500 | uint32 | - | `StageScenario.cpp:4498`, `Blocks/MoveBlock.cpp:333` | all |
| Resurrection | `Resurrection.GraceMs` | 20000 | uint32 | - | `StageScenario.cpp:2643` | party stages |
| Resurrection | `Resurrection.ReviveAlly` | 1.5f | float | - | `Encounters/PartyEncounter.cpp:270` | party stages |
| Output | `Output.Clock` | 0.03f | float | - | `StageScenario.cpp:4986`, `StageScenario.cpp:4989` | all |
| StandIn | `StandIn.Share` | 0 | int32 | Load: 0..100 | `Encounters/StandInSeat.cpp:90` | arenas with no own share: none live (note 5) |
| StandIn | `StandIn.LeadChance` | 50 | int32 | Load: 0..100 | `Encounters/StandIn.h:142` | group1, dungeon2-3 |
| StandIn | `StandIn.TankChance` | 34 | int32 | Load: 0..100; pair scaled to sum <= 100 | `Encounters/StandIn.h:145`, `Encounters/StandIn.h:146` | group1, dungeon2-3 |
| StandIn | `StandIn.HealerChance` | 33 | int32 | Load: 0..100; pair scaled to sum <= 100 | `Encounters/StandIn.h:146` | group1, dungeon2-3 |

## 5. Reviewer notes

- The whole tuning is one flat struct with 297 keys and a hand-kept `Visit` list. Adding a key is four edits: the field,
  the `Visit` line, a conf.dist block, and the reader. The conf.dist agreement check (removed) caught a missing `Visit` line only in the
  direction "conf.dist has it, `Visit` has not" and the reverse; it does not catch a field that is not in `Visit`
  (never loaded, never fingerprinted). Consider generating `Visit` from the struct.
- `Load` uses `showLogs = false` for everything: a typo in a value (`0,5`) silently keeps the default, and no key is
  ever reported as unknown at startup (`conf_prune.py --check` is the only unknown-key detector, and it works from
  conf.dist, not from the sim).
- Because the fingerprint hashes values, any retune of a key on the host alone makes every worker refuse; use
  `forgectl conf-sync --check` first ([forgectl.md](../forgectl.md)).
- Four near-identical price blocks (`Markers`, `Seek`, `Interact`, `Controls`) and the `Raid`/`Roles`/`Party`
  triplet invite consolidation; M4 reading `Seek.Stuck/Wall/WallSlide` means a retune of M2 silently retunes M4.
- Group names are history: `Duel` (no duel stage exists), `Raid` (no raid stage exists). Renaming changes the keys and so every deployed conf and the
  fingerprint.
- `Characters.*` level keys are reached in training only by `move4_follow` (table note 3); the other stages fix the
  level by `Level`, a focus band or the instance rung. Check that this is intended before tuning them.
- The fall-through to `StandIn.Share` is dead in the live curriculum (note 5). (`Party.SizeWeight1..4` were deleted.)

## 6. Observed issues

1. `CurriculumTuning.h:1176-1177`: the doc comment of `Load` says "min/max pairs are ordered"; `Load` orders none.
   Only the two `Sudden*` pairs are guarded at their reader (`PartyFollowEncounter.cpp:319, 359`).
2. `Load` reads with `showLogs = false` (`CurriculumTuning.cpp:60-62`): a malformed value is silently replaced by the
   default, and unsigned keys have no range check at all.
3. (fixed 2026-10-08) conf.dist documented `Arena.<stage>.<arena>.MaxRung`, which nothing reads; the block is gone.
   `04-curriculum.md:858` still lists it (Python/doc side, not touched here).
4. (fixed 2026-10-08) conf.dist now documents `Arena.<stage>.<arena>.WeightFinal`.
5. The `Arena.*` overrides are not part of the fingerprint (section 1.4).
6. `StageScenario.cpp:142-153` vs `:171`: the doc comment of `RandomLevel` sits above `TankModeSpell` (misplaced).
7. The `Actions` preamble comment says decisions are 100 ms apart; the sim's tuning unit is 50 ms
   (`StageScenario.cpp:103`, header line 34).
8. `Duel.*` and `Raid.*` group names describe stages that no longer exist, but their keys are read (the duel's approach
   ranges; the party and roles prices); only the names are history. (`Party.SizeWeight1..4` were deleted.)
9. `CurriculumTuning.h` is 1185 lines with many lines over 120 columns in doc comments (for example the `Raid.KeepUp`
   comment at about line 126: UNVERIFIED exact lines; run `awk 'length>120'`).
10. `AnimusForge.Curriculum.*` environment overrides (`AC_...`) enter the fingerprint invisibly (section 1.4).

## 7. UNVERIFIED

- That `Animus.Curriculum.` is the mod-animus prefix (the module is not in this tree; only the header says so).
- What `Acore::StringTo<uint32>` does with a negative or oversized value (read `common/Utilities/StringConvert.h`).
- Whether anything re-runs the scenario constructor on a config reload (`AnimusForge.cpp`, `ForgeMain.cpp`).
- That the dungeon stages' character levels come only from the instance rung (`InstanceEncounter::BeforeLevel`).
- The "Live stages" column is derived from the arenas in `Stages.cpp`, not from running the stages.
- Exact lines of the over-120-column comments (Observed issue 9).
