# Reference: StageScenario, stage definitions and the scenario interface

Purpose and scope. This document describes the scenario object that runs a curriculum stage (`StageScenario`), the
stage and arena definitions it is built from (`Curriculum/Stages/`), the abstract `Scenario` interface and the small
shared files of `Scenario/`. Encounters are in [cpp-encounters.md](cpp-encounters.md); every tuning key in
[cpp-tuning-keys.md](cpp-tuning-keys.md); blocks, layouts, character building and rewards in
[cpp-blocks.md](cpp-blocks.md), [cpp-layout-character.md](cpp-layout-character.md) and
[cpp-rewards-routing.md](cpp-rewards-routing.md). System overview: [00-architecture.md](00-architecture.md). Stage
catalogue: [stages.md](stages.md); known issues: [known-issues.md](known-issues.md); terms: [glossary.md](glossary.md).
All statements are about the tree at commit `bd32b9dc8` (branch `forge`). Paths in the sections are relative to
`src/server/game/Animus/Scenario/Curriculum/` unless they start with `src/` or `apps/`.

## Map

| Path (under `src/server/game/Animus/Scenario/`) | Lines | Role |
|---|---|---|
| `Curriculum/StageScenario.cpp` | 5192 | The one Scenario implementation: construction, resets, builds, per-decision flow, observation, rewards, stage.json (split in Parts C1/C2). |
| `Curriculum/StageScenario.h` | 513 | Class declaration of StageScenario and its public helpers. |
| `Curriculum/StageState.h` | 684 | Per-env and per-seat state structs and constants of the stage scenario. |
| `Curriculum/Stages/StageDefinition.h` | 324 | Data model: StageDefinition, ArenaDefinition, enums, authored-ground structs, registry API. |
| `Curriculum/Stages/Stages.cpp` | 1334 | The ten live stage definitions, five authored ground tables, validation, registry, arena draw weights. |
| `Scenario.h` | 218 | Abstract Scenario interface and ScenarioSpec. |
| `Scenario.cpp` | 45 | CreateScenario / ScenarioNames over the curriculum registry. |
| `StageSettings.h` | 86 | Host-to-scenario settings struct. |
| `SpawnArea.h` | 37 | Declarations for clearing database-spawned creatures round a bot. |
| `SpawnArea.cpp` | 66 | Clear (60 yd) and ClearMap (radius) implementations. |
| `SpellChecks.h` | 64 | Cooldown/aura fractions and the core cast check without casting. |
| `SpellChecks.cpp` | 116 | Implementation of the cast check incl. early refusals and the power-cost addition. |
| `SummonLevel.h` | 35 | Thread-local level override read by core Creature::SelectLevel. |
| `Curriculum/BuildRetry.h` | 47 | RetryBuild template and SETUP_BUILD_ATTEMPTS. |
| `Curriculum/WarmCaches.h` | 34 | Declarations of WarmCaches / WarmGearCaches. |
| `Curriculum/WarmCaches.cpp` | 43 | Pre-seal touch of every world-DB table the curriculum reads. |

Contents: Part A stage definitions; Part B the scenario interface and the small files; Part C1 StageScenario.h,
StageState.h and `StageScenario.cpp` lines 1-2631; Part C2 `StageScenario.cpp` lines 2570-5192.

## Part A. Stage definitions (`Curriculum/Stages/`)

`StageDefinition.h` (324 lines) declares the data model; `Stages.cpp` (1,334 lines) holds the ten live definitions,
the authored ground tables they use, the validation, and the registry. Nothing in this directory runs at episode time
except the helpers `ArenaDrawWeights` and the two `SeatCount` members; everything else is read once
at startup by `CurriculumStages()` and then treated as immutable (a function-local `static const`, `Stages.cpp:1354`).

### A.1 Layout of `Stages.cpp`

| Lines | What |
|---|---|
| 19-48 | file header comment: the stage chain, and the note that every movement stage runs 50 ms ticks (`AnimusForge.Stage.<name>.TicksPerDecision` in the conf template) |
| 50-62 | includes; `DRILL_DAMAGE = 3` (`:62`, the highest drillable role) |
| 70-73 | `StockadeEntrance()`: `{54.23, 0.28, -18.34, 6.26}`, where areatrigger 101 puts a player |
| 91-163 | `StockadeHallways()`: 202 points (the entrance first, then a 3-yd grid over the Stockades' hallways), facing always 0 |
| 185-355 | `StockadeRooms()`: 39 `SeekRoom`s (name, FloorZ, Opening, Centre, Walk, convex Floor polygon); `Front` is set after the table by a name list (`:343-353`, 13 front cells) |
| 365-374 | `SeekObjects()`: 5 `SeekObject`s (chest 144111, crate 179972, barrel 179967, sack 180660, strongbox 2039) |
| 385-421 | `StockadeSightPairs()`: 32 `SightPair`s, 8 with `Corner = true` |
| 443-588 | `DeadminesSites()`: 4 `InteractSite`s (factory: door 13965, lever 101831; foundry: 16399 / 101834; mast_room: 16400 / 101832; iron_clad: door 16397, opener 16398 = the cannon, `Key` 5397 = Defias Gunpowder) |
| 592-595 | `DeadminesMiddle()`: `{-150, -576, 19.32, 3.14}`, the interact stage's first spawn |
| 599-602 | `RagefireEntrance()`: `{3.81, -14.82, -17.84, 4.39}` |
| 612-616 | `DungeonBlocks()`: the block list of the dungeon stages |
| 621 | `DUNGEON_STAND_IN_SHARE = 20` (percent) |
| 623-1025 | `Definitions()`: the ten `StageDefinition`s, in training order |
| 1028-1186 | `ArenaProblem()`: per-arena validation |
| 1189-1280 | `Problem()`: per-stage validation |
| 1283-1342 | out-of-line `ArenaDefinition::SeatCount`, `ArenaDrawWeights`, `StageDefinition::Has`, `SeatCount` |
| 1345-1383 | `LeftOut()`, `CurriculumStages()`, `CurriculumProblems()` |
| 1386-1393 | `FindStage()` |

The five authored tables are offline products: the comments (`Stages.cpp:77-90, 165-184, 357-364, 423-441`) say they
were scanned from the navmesh and vmaps by throw-away scripts under `.agents/plans/...` (gitignored, so not in the tree)
and validated by data tests that need the map data (`FORGE_VISION_DATA`): `StockadeHallwaysDataTest.cpp`,
`StockadeRoomsDataTest.cpp`, `DeadminesSitesDataTest.cpp`. UNVERIFIED: whether the authoring scripts still exist on
the owner's machine; without them the tables cannot be regenerated from the repository.

### A.2 Types (`StageDefinition.h`)

**Enums.** `SeatPlan {Solo, Party}` (`:36`). `Opposition {Instance, Seek, Sight, Interact, PartyFollow, Combat,
Roles}` (`:43`): Instance runs InstanceEncounter, Seek SeekEncounter, Sight SightEncounter, Interact
InteractEncounter, PartyFollow PartyFollowEncounter, Combat CombatEncounter, Roles RolesEncounter (the header's
comments name them). `CombatDrill {None, Fight, Packs, Survive}` (`:70`), `RolesDrill {None, Hold, Keep, Focus, Pull}`
(`:80`), `InstanceLadder {None, Wing}` (`:148`). `MAX_ARENAS = 16` (`:156`; "the critic state has one column per
arena").

**Value structs.** `SeekRoom` (`:94`: Name, FloorZ, Opening, Centre, Walk, Floor, Front), `SeekObject` (`:112`: Entry,
Kind, Height, Radius), `SightPair` (`:124`: Spawn, Object, Corner; indexes into the arena's `SpawnPoints`),
`InteractSite` (`:137`: Name, Door, Opener, Key, Near, Far).

**`ArenaDefinition`** (`:161-251`), one situation an episode can be (default in brackets):

| Field | Meaning (what the code does with it) |
|---|---|
| `Name` | unique in the stage; key of the episode info, `stage.json` and the `Arena.<stage>.<name>.*` tuning keys |
| `Weight` [1] | share of episodes; conf `<TuningPrefix>Arena.<stage>.<name>.Weight` (`StageScenario.cpp:489-491`) |
| `WeightFinal` [-1] | share at the end of the budget; -1 keeps `Weight`; conf `...WeightFinal` (`StageScenario.cpp:492-494`; a negative conf value is clamped to 0). Set by no live stage |
| `Seats` [Solo] | `SeatPlan` |
| `Against` [Instance] | the opposition; the default is `Instance`, so an arena that forgets it is read as a dungeon run |
| `PartyGroup` [false] | the seats form a core group (PartyEncounter); validated to dungeon-wing or roles arenas only (`:1049`) |
| `Instance`, `InstanceRow` [None, -1] | which ladder and row an Instance arena runs; the row indexes `InstanceLadderRows(Wing)` (`InstanceBosses.cpp:102`) |
| `EvalOnly` | never drawn in training or in an ordinary evaluation; only an evaluation pinned to it (`PinEvaluationArena`, `StageScenario.cpp:4624`) plays it |
| `PartySize` [0] | learned seats of a party; 0 = a whole group (`GROUP_SEATS` = 5). M4 sets 4 (`GROUP_MEMBERS`) |
| `EpisodeSeconds` [0] | 0 = `StageSettings::EpisodeSeconds` |
| `ProperParty` | tank + healer + three damage drawn by build (`StageScenario::FitsDungeonRole`); G1 only |
| `DrillRole` [0] | 1 tank, 2 healer, 3 damage; the drilled role is seat 0 |
| `Hazards` | every pull holds a ground-effect caster (`OpponentPool::RandomHazardCaster`); `combat2_packs/fire` only |
| `SpawnPoints`, `MapId` | per-arena overrides of the stage's (`ArenaDefinition::MinLevel` was deleted: no arena set it) |
| `Rooms`, `Objects`, `SeekRadius` [3.0] | Seek: the rooms and the object pool; Sight and Interact: `Objects` is the pool |
| `SightPairs` | Sight: the fixed evaluation pairs |
| `Sites` | Interact: doors with their openers |
| `Combat`, `Ally` | CombatDrill, and a passive friend the creatures go for first (`guard`) |
| `Roles` | RolesDrill |
| `RespawnAtEntrance` | a death rises at the instance entrance after `Respawn.DelayMs` (EntranceRespawn; see [cpp-encounters.md](cpp-encounters.md)) |
| `StandInShare` [-1] | percent of training runs with the "human" stand-in; -1 keeps the tuning key `StandIn.Share`; per-arena conf `Arena.<stage>.<arena>.StandInShare`, clamped to -1..100 (`StageScenario.cpp:496-499`) |
| `LevelFirst`, `LevelLast` | Wing: the level band of the runs before the ladder's lift (Deadmines 17-20); 0 = the dungeon finder's |

`SeatCount()` (`Stages.cpp:1283`): Party = `PartySize` or `GROUP_SEATS`; Solo = 1.

**`StageDefinition`** (`:272-317`): `Name`; `Suffix` (written to `stage.json` as `suffix`, `StageScenario.cpp:1256`; no
C++ reader; python shows it only in test fixtures and `apps/forge/tools/stage_json_diff.py:169`: UNVERIFIED whether any
learner code reads `suffix` from a live `stage.json`); `Extends`; `Merges`; `Summary` (written as `summary`,
`StageScenario.cpp:1257`); `Blocks`; `Arenas`; `MapId`; `SpawnPoints`; `MinLevel` (`StageScenario.cpp:2164`); the focus band (`FocusLevelFirst`,
`FocusLevelLast`, `FocusChance`, `StageScenario.cpp:2194-2206`); `Level` (`StageScenario.cpp:2204`: every character of
the stage at this level, raised to the class minimum). Members `Has(block)`, `SeatCount()` (the largest arena's), `AnyArena(pred)`.

Free functions. `ArenaDrawWeights(arenas, weights, finals, evaluating, progress)` (`:1309`)
returns `lround(100 * (from + (to - from) * along))` per arena, so the weights are integer percent-scaled; `along` is
1 when evaluating, else `clamp(progress, 0, 1)`; `from` and `to` come from the passed vectors, falling back to the
definition's `Weight` and then to `from`. An arena gets 0 if it is `EvalOnly`. Callers: `StageScenario.cpp:678`; `DungeonStagesTest.cpp:191,229-232`.

### A.3 Registry and validation

`CurriculumStages()` (`:1352`) walks `Definitions()` in order. For each stage it first **inserts `BlockId::Entities`
directly after `Vision` when the stage has Vision but not Entities** (`:1358-1360`), so the literal block lists in
`Definitions()` for move1 to move4, the combat stages and G1 differ from the live ones (`DungeonBlocks()` lists
`Entities` itself, `:615`). Then `Problem(stage, valid)`; a stage with a problem is logged (`LOG_ERROR module.animus`,
`:1366`), recorded in `LeftOut()` and skipped, so a stage whose base was left out is also left out (`valid` only holds
accepted stages). `CurriculumProblems()` forces the registry and returns the list; `ForgeCommands.cpp:268`
(`CurriculumSound`) refuses to train while it is not empty (`:271`). `FindStage` is a linear search.
`CreateScenario` and `ScenarioNames` (`Scenario/Scenario.cpp`) only wrap these.

**Stage-level checks (`Problem`, `:1189-1280`)**, in order:

1. blocks start with `Core`, then `Move` (`:1192`);
2. no block twice (`:1196`);
3. `Map` needs `Vision` (`:1200`);
4. `Sight` needs `Vision`, and `Entities` before `Sight` (`:1202-1208`);
5. `Combat` with `SeatCount() > 1` needs `PartyFrames` (`:1213`);
6. `Combat` needs `Sight` before it (`:1214-1220`);
7. `Extends`, if set, names an earlier valid stage (`:1234`); a merge needs an `Extends`, appears once, is not the
   `Extends`, and is an earlier valid stage (`:1236-1244`);
8. a stage with any arena other than Seek, Sight, Interact or PartyFollow needs `Duel` (`:1246-1253`);
9. 1 to `MAX_ARENAS` arenas (`:1256`), not all `EvalOnly` (`:1258-1263`);
10. every arena has a name, names are unique, and `ArenaProblem` is empty (`:1265-1277`).

The header comment (`StageDefinition.h:319`) says "an unknown or later base"; the code only tests that the base is an
earlier valid stage (`:1224-1234`), which gives the same effect.

**Arena-level checks (`ArenaProblem`, `:1028-1186`)**:

- instance and ladder go together; the row is in range; an Instance arena needs the `Pack` block and Party seats;
- `PartyGroup` needs `PartyFrames` and must be a dungeon group or a roles drill group; `ProperParty` only for a drill
  group; `DrillRole` <= 3 and only with `ProperParty`; `InstanceRow >= 0` only for Instance arenas;
- the level band only for Wing; the level band is both ends or neither, in order; `StandInShare` <= 100 and only for Party seats;
- Seek: solo, needs Move and Vision, no Compass, rooms, objects and a positive radius;
- Sight: solo, no rooms, Move and Compass and Vision, objects, at least 2 spawn points, every `SightPair` indexes two
  different `SpawnPoints`;
- Interact: solo, no rooms, Move and Vision and Sight, no Compass, sites, at least 3 objects, radius > 0, a `MapId`;
  every site has a name, Door, Opener, Near and (`Key` or `Far`); at least one lever site (`Key == 0`) and one key site;
- PartyFollow: Party seats, `PartySize` 1..4 (`GROUP_MEMBERS`), not a `PartyGroup`, a `MapId`; Move, Vision and
  PartyFrames; no Compass; any `PartySize` <= `GROUP_SEATS` (5, `:1134`);
- roles: `Roles` and `Opposition::Roles` agree; Party seats, no `PartySize`, `ProperParty`, `PartyGroup`,
  `RespawnAtEntrance`, `DrillRole == RolesDraw::DrilledRole(Roles)`; the six blocks Move, Vision, Sight, Combat,
  PartyFrames, Gauntlet; a map;
- combat: `Combat` and `Opposition::Combat` agree; solo; Move, Vision, Sight, Combat blocks; `Survive` needs
  `Gauntlet`; `Ally` only with `Fight`; a map; `RespawnAtEntrance` needs a map;
- rooms only on Seek; objects only on Seek, Sight or Interact; each room named with at least 3 floor corners;
- an arena on a map other than the stage's, with no spawn points of its own, must be Instance or PartyFollow.

Not checked: that some trained arena has `Weight > 0` (all-held-out is caught, all-zero weights is not), `WeightFinal`,
`Suffix` or `Summary` uniqueness, `EpisodeSeconds` ranges, `FocusLevelFirst <= FocusLevelLast`, `Level` against class
minima, and that a Wing row fits a `LevelFirst` band.

### A.4 The ten live definitions, field by field

Order is that of `Definitions()`
(training order). "Eff. blocks" are after the `Entities` insertion.

| Stage (`Stages.cpp` line), suffix | Extends / Merges | Eff. blocks (layout order) | Map, spawn, level fields |
|---|---|---|---|
| `move1_controls` (648) `_controls` | none | Core, Move, Compass, Vision, Entities, Goal | `MapId` 34 (Stockades), `SpawnPoints {StockadeEntrance}`, `Level 1` |
| `move2_seek` (684) `_seek` | move1_controls | Core, Move, Vision, Entities, Map, Goal | 34, entrance, `Level 1` |
| `move3_interact` (722) `_interact` | move2_seek | Core, Move, Vision, Entities, Map, Sight, Goal | 36 (Deadmines), `{DeadminesMiddle}`, `MinLevel 17`, focus 17-20 at 100% |
| `move4_follow` (761) `_follow` | move2_seek | Core, Move, Vision, Entities, Map, PartyFrames, Goal | stage `MapId` 389, arenas carry 389 and 36; no stage spawn (the encounter finds its own) |
| `combat1_fight` (801) `_fight` | move3_interact | Core, Move, Duel, Pet, Vision, Entities, Map, Sight, Combat, Goal | 389, `{RagefireEntrance}`, focus 13-18 at 100% |
| `combat2_packs` (825) `_packs` | combat1_fight | as combat1 | same |
| `combat3_survive` (850) `_survive` | combat2_packs | Core, Move, Duel, Pet, Gauntlet, Vision, Entities, Map, Sight, Combat, Goal | same |
| `group1_roles` (889) `_roles` | combat3_survive; merges `move4_follow` | Core, Move, Duel, Pet, Gauntlet, Vision, Entities, Map, Sight, PartyFrames, Combat, Goal | same |
| `dungeon2_ragefire` (940) `_ragefire` | group1_roles | `DungeonBlocks()`: Core, Move, Duel, Pet, Pack, Gauntlet, Vision, Entities, Map, Sight, PartyFrames, Combat, Goal | no stage map: each arena's row fixes it |
| `dungeon3_deadmines` (963) `_deadmines` | dungeon2_ragefire | DungeonBlocks | same |

`LiveLayoutPinTest.cpp` with `LiveLayoutPin.golden.inc` pins every live stage's resulting layout, so a change to a list
or to the insertion rule shows there (see [cpp-layout-character.md](cpp-layout-character.md) and
[cpp-blocks.md](cpp-blocks.md) for what the blocks are). The seed graph is a tree: move3 and move4 both extend move2;
move4 reaches later stages only as a merge into G1. `docs/forge/04-curriculum.md` calls the chain "one line"; the
code only demands that a base be an earlier valid stage.

Arenas (`name (weight)`; fields exactly as in `Definitions()`):

| Stage | Arena | Against, seats | Seconds | Other fields |
|---|---|---|---|---|
| move1_controls | `hallway` (1) | Sight, Solo | 60 | `SpawnPoints StockadeHallways()` (202), `MapId 34`, `Objects SeekObjects()`, `SightPairs` (32) |
| move2_seek | `rooms` (1) | Seek, Solo | 300 | hallway spawn points, map 34, `Rooms` (39), `Objects`, `SeekRadius 3.0` |
| move2_seek | `sweep` (1) | Seek, Solo, `EvalOnly` | 300 | as `rooms` |
| move3_interact | `sites` (1) | Interact, Solo | 120 | map 36, `Objects`, `SeekRadius 3.0`, `Sites` (4) |
| move3_interact | `sweep` (1) | Interact, Solo, `EvalOnly` | 120 | as `sites` |
| move4_follow | `ragefire` (1) | PartyFollow, Party | 300 | `PartySize 4`, map 389 |
| move4_follow | `deadmines` (1) | PartyFollow, Party | 300 | `PartySize 4`, map 36 |
| combat1_fight | `fight` (3) | Combat, Solo | 150 | `Combat Fight`, `RespawnAtEntrance` |
| combat1_fight | `guard` (1) | Combat, Solo | 150 | `Fight`, `Ally`, `RespawnAtEntrance` |
| combat2_packs | `packs` (2) | Combat, Solo | 240 | `Packs`, `RespawnAtEntrance` |
| combat2_packs | `fire` (1) | Combat, Solo | 240 | `Packs`, `Hazards`, `RespawnAtEntrance` |
| combat3_survive | `survive` (1) | Combat, Solo | 360 | `Survive`, `RespawnAtEntrance` |
| group1_roles | `tank_hold` (2) | Roles, Party | 240 | `PartyGroup`, `ProperParty`, `DrillRole 1`, `Hold`, `RespawnAtEntrance` |
| group1_roles | `heal_keep` (2) | Roles, Party | 300 | same, `DrillRole 2`, `Keep` |
| group1_roles | `damage_discipline` (2) | Roles, Party | 240 | same, `DrillRole 3`, `Focus` |
| group1_roles | `pull` (2) | Roles, Party | 360 | same, `DrillRole 1`, `Pull` |
| dungeon2_ragefire | `dungeon` (1) | Instance, Party | 7200 | `Wing`, row 0, share 20 |
| dungeon2_ragefire | `heldout` (0) | Instance, Party, `EvalOnly` | 10800 | `Wing`, row 2 (Wailing Caverns) |
| dungeon3_deadmines | `dungeon` (1) | Instance, Party | 14400 | `Wing`, row 1, share 20, levels 17-20 |
| dungeon3_deadmines | `heldout` (0) | Instance, Party, `EvalOnly` | 10800 | `Wing`, row 2 |

M4's `PartySize` of 4 means four learned followers beside the scripted leader (the owner slot): its seat count is 4,
not 5. `PartyGroup` is set on G1 and the dungeon arenas only; M4 forbids it.

### A.5 Overrides that act on a definition

Keys are `<TuningPrefix>...` with the prefix `AnimusForge.Curriculum.` on the forge: `Arena.<stage>.<arena>.Weight`, `.WeightFinal` (`:489-494`), `.StandInShare` (`:496-499`).
The complete key table is [cpp-tuning-keys.md](cpp-tuning-keys.md). The host reads `AnimusForge.Stage.<name>.*`
separately (not a definition field; see [config-keys.md](config-keys.md)).

### A.6 Tests

`DungeonStagesTest.cpp` (dungeon stage shape, draw weights, stand-in), `RolesStageTest.cpp`,
`InteractStageTest.cpp`, `SeekEncounterTest.cpp`, `SightEncounterTest.cpp`, `PartyFollowTest.cpp`,
`CombatPerceptionTest.cpp` (each asserts `CurriculumProblems().empty()`; the last also the focus band),
`LiveLayoutPinTest.cpp`, `CompassBlockTest.cpp`, and the three data tests above. The python side: see
[tests.md](tests.md) and [stages.md](stages.md).

### A.7 Reviewer notes

- Stages are code, not data: adding or tuning one is a C++ edit, a rebuild and a changed cluster fingerprint.
- `ArenaDefinition` mixes every arena kind in one struct; each field matters for one or two `Against` values. Per-kind
  structs would remove most of the 160 lines of `ArenaProblem`.
- The default `Against = Instance` is a trap for a new arena.
- `Suffix` and `Summary` have no C++ reader beyond the JSON dump.
- `WeightFinal` is used by no live stage (plumbing and tests only).
- PartyFollow caps `PartySize` at 4 while the generic check allows 5 (`:1134`).
- The block-order rules (Sight after Entities, Combat after Sight, Map after Vision) are enforced here only; the
  encoders assume them.

### A.8 Observed issues (Stages)

1. Lines over 120 columns (`.editorconfig`): `Stages.cpp:31,33,440,705,710,933,975,1005` (`:933` is 175 columns),
   `StageDefinition.h:45,224`.
2. `StageDefinition.h:319` says "an unknown or later base"; only "not an earlier valid stage" is tested.
3. `StageDefinition.h:39`: the `SeatPlan::Party` comment ("1-GROUP_SEATS with a character each episode: a tank, a
   healer and damage") describes an older draw; the live meaning is `SeatCount()` plus `ProperParty`.
4. `StageDefinition.h:269` says the curriculum "is a tree" while `04-curriculum.md` says "line"; the code supports a
   tree and the live graph is one (move3 and move4 share a base).
5. `Stages.cpp:62` `DRILL_DAMAGE` duplicates `RolesDraw::ROLE_DAMAGE` (`RolesDraw.h:36`).
6. Comments in `Definitions()` cite archived stages (`stage6`, `move3_vertical`) and gitignored plan paths; the
   authoring scripts for the five ground tables are not in the tree.
7. `Stages.cpp:1358-1360` mutates the block list during registration, so the literal lists are not the live lists.
8. `StockadeRooms()` sets `Front` by a hard-coded name list (`:343-353`) rather than in the table; a renamed room
   silently becomes a deep room (`SeekEncounterTest.cpp:151-160` would catch the count: 13 front, 26 deep).


## Part B. The scenario interface and the small files (`Scenario/`)

### B.1 `Scenario.h` / `Scenario.cpp` (231 + 45 lines)

`class Scenario` is the abstract contract between the env pool (`Env/EnvPool`) and a training scenario. The only
implementation is `Curriculum::StageScenario` (grep of `public Scenario` over the tree and tests finds only
`StageScenario.h:57`). `Scenario.h` says "All calls happen on the world thread, outside MapMgr::Update"; two hooks
(`ResetsStayOnMap`, `AgentLayouts`/`AgentKinematics`) say they run "on the thread updating the env's map", so the file
comment is only partly true: `ResetOnMapThreads` (`StageSettings.h`) moves resets to the map threads.

**Types.** `LayoutSpec {Name, ObsDim, NumActions}`. `ScenarioSpec {AgentsPerEnv, ObsDim, StateDim, NumActions, EpisodeInfoDim,
GoalCount, LongestEpisodeSeconds, ImageBytes, LookHeads, MapBytes, Layouts}` (`:61-80`): the tensor shapes the learner
gets in SPEC ([protocol.md](protocol.md)). `ObsDim` and `NumActions` are the largest layout's; every agent's row is
padded to it.

**Virtual interface**, in call order of a decision (the one in `StageScenario` is documented in
[cpp-stagescenario.md](cpp-stagescenario.md)):

| Method | Default | Contract |
|---|---|---|
| `Name()`, `Spec()` | pure | scenario name = stage name; shapes |
| `Playable()` | true | false if a restricted run has no layout; a queue skips it |
| `CharactersReused()` | 0 | cumulative count of seats kept across an episode boundary |
| `ResetsStayOnMap()` | false | true when every reset keeps its env on its map, so the pool may reset on the map thread |
| `Setup(env)` | pure | once: create bots and place them; false = cannot build |
| `Reset(env)` | pure | new episode in place (EnvPool already cleared clock and stats) |
| `ApplyGoals(env, goals)` | no-op | before `ApplyActions`; a goal is scored, reported, shown to teammates, never masks |
| `ApplyLook(env, look)` | no-op | only when `LookHeads > 0`; free look "free: nothing may reach what prices, tallies or judges an action" |
| `ApplyActions(env, actions)` | pure | one chosen action per agent; masked actions from a misbehaving client must be ignored safely |
| `SubTick(env, diffMs, decided)` | no-op | every world tick after `ApplyActions` on a decision tick |
| `CameraRenderSize(env, agent)` | {0,0} | the agent's camera render size this episode |
| `Observe(env, obs, state, mask, image, map)` | pure | `mask` null for an ended episode's final observation |
| `AgentLayouts`, `AgentPresence`, `AgentKinematics` | layout 0, present, zeros | per agent; constant within an episode (kinematics: `Kinematics::SAMPLE_DIM` floats) |
| `Reward(env, reward)` | pure | from `env.StepStats` |
| `IsTerminal(env)` | false | checked each decision; the time limit is a truncation |
| `EpisodeInfo(env, info)`, `EpisodeInfoNames()` | pure | `EpisodeInfoDim` floats per agent; names go to SPEC |
| `EvaluationPairs()` | 1 | how many seeds an evaluation's placements cycle through (eval videos) |
| `FilmedRole(env, agent)` | 0 | 1 tank, 2 healer, 3 damage, read off the build; for the eval video choice |
| `SetLayoutWeights`, `SetStageProgress`, `SetShapingScale`, `SetCostScale` | no-ops | learner's WEIGHTS and PROGRESS messages (protocol 18/19) |
| `PinEvaluationArena(pin)` | `pin == 0` | the held-out arena of the next evaluation (index + 1); false if it is not held out |
| `SetStandIn(bool)` | no-op | MODE_FLAG_STAND_IN: play the stand-in row with a frozen partner |
| `TakeClusterTally`, `AddClusterTally`, `ClusterRung`, `FollowClusterRung`, `ClusterLadderCollapsed` | empty / -1 | the cluster's shared dungeon ladder; see [cpp-encounters.md](cpp-encounters.md) (wing ladder) |
| `Teardown(env)` | pure | once at shutdown |

Free functions: `CreateScenario(name, settings)` returns `StageScenario` for any stage `FindStage` knows, else
`nullptr` (`Scenario.cpp:30-36`); `ScenarioNames()` lists the valid stages in order (`:38-45`). Callers:
`AnimusForge.cpp:672`, `ForgeCommands.cpp:279,384,417,906`.

Reviewer notes: the interface was designed for several scenarios ("Adding a standalone scenario = implementing Scenario,
one branch in CreateScenario", `Scenario.cpp:26-27`) but now has exactly one implementation; a good deal of it
(the cluster tally calls, `FilmedRole`) is wing-ladder or stand-in plumbing that exists only for
`StageScenario`.

### B.2 `StageSettings.h` (86 lines)

The host-to-scenario settings struct, filled by the host from its conf (the forge: `ForgeConfig`; mod-animus: its own
conf). Fields and defaults: `Envs` 1; `FirstEnvId` 0 (bot account ids and names derive from it); `DecisionMs` 100
("per-decision reward terms are tuned per 50 ms and scaled by it"); `EpisodeSeconds` 60 (for arenas without their own);
`ReportEpisodes` 256 (episode-info means kept every this many episodes, `EnvPool::LastEpisodeMeans`);
`ResetOnMapThreads` true (conf `AnimusForge.ResetOnMapThreads`); `Classes` (empty = every class); `SpawnMapId` 560 and
`SpawnPosition {2741.9, 1315.2, 14.0, 2.96}` (an instanceable map and its position; a stage's own `MapId` and spawn
points override); `Level` 0 (0 = the curriculum's random levels); `ContinentReplicas` 0 (0 asks for the fewest the
31 phase bits allow; `StageScenario.cpp:353-355` clamps envs per replica to `ENV_PHASE_BITS`); `TuningPrefix`
(`"AnimusForge.Curriculum."` forge, `"Animus.Curriculum."` mod-animus, trailing dot); `LayoutsDir` (empty = nowhere;
where `stage.json` and layout manifests go); `EventsLog` (the run's `events.log`, appended, empty = nowhere). Which
conf keys fill these is in [config-keys.md](config-keys.md); the keys under `TuningPrefix` are in
[cpp-tuning-keys.md](cpp-tuning-keys.md). Note the defaults here (`SpawnMapId` 560, `Level` 0) matter
only for a stage that has no map of its own; none of the ten live stages is such.

### B.3 `SpawnArea.h/.cpp` (37 + 66 lines)

`SpawnArea::Clear(Player*)` removes (`DespawnOrUnsummon(0ms, Seconds(WEEK))`) every creature, dead or alive, within
`SPAWN_AREA_CLEAR_RADIUS = 60` yd of the bot (`GetDeadCreatureListInGrid(..., false)`), logs a warning with the count.
`SpawnArea::ClearMap(bot, radius)` loads grids in range, copies the map's `GetCreatureBySpawnIdStore()` entries
that are in the world and within `radius` (2-D distance), and despawns them for a week. Both only remove
database-spawned creatures; neither touches gameobjects (doors, levers, chests: `ObjectPool::ClearOwn` does, see
cpp-encounters.md). Callers: `StageScenario.cpp:2294-2307` on the env's first build of an instanceable map (and for the
party follow whenever a new instance opens): ClearMap with `INSTANCE_CLEAR_RADIUS 300` (`:108`) for Seek, Sight, Combat,
Roles;
`DUNGEON_CLEAR_RADIUS 1000` for PartyFollow (`:113`); `INTERACT_CLEAR_RADIUS 600` for Interact; `Clear` (60 yd)
otherwise (Instance arenas, which spawn their own instance fresh). No test covers either. A creature despawned with
`WEEK` respawn is gone for the instance's life; instances are per env, so the next run starts again from the database.

### B.4 `SpellChecks.h/.cpp` (64 + 116 lines)

Spell state every scenario reads the same way, without casting. API: `CooldownFraction(bot, info)` (remaining over
`max(RecoveryTime, CategoryRecoveryTime)`, capped 1, 0 for null or no cooldown); `AuraFraction(unit, spellId, caster,
float* stacks)` (remaining duration fraction, 1 for a permanent aura, 0 if absent; with `stacks`, the max of the
current and `min(1, max(stackAmount, charges) / 5)`); `CheckCast(bot, info, targets, castItem, reason)` and
`CastResult(bot, info, targets, castItem, exact)`; constants `GCD_MS = 1500`, `SPELL_BATTLE_STANCE = 2457`,
`SPELL_DEFENSIVE_STANCE = 71`.

`CastResult` is the mask-and-feature workhorse (called by `Layout/EncoderSupport.cpp:209,339,497`). What it does
(`SpellChecks.cpp:59-116`): when not `exact`, it refuses early without building a `Spell`: a mounted caster
(`NOT_ON_TAXI` / `NOT_MOUNTED`), else the shapeshift rule unless `SPELL_AURA_MOD_IGNORE_SHAPESHIFT`, then the
combat rule unless `SPELL_AURA_ABILITY_IGNORE_AURASTATE`. Otherwise it builds a `Spell(bot, info, TRIGGERED_NONE)`,
sets `m_CastItem`, loads scripts, `InitExplicitTargets`, runs `Spell::CheckCast(!stunsPet)`, and **adds the power-cost
check the core only does in `prepare`** (health cost vs current health -> `CASTER_AURASTATE`; power cost vs current
power -> `NO_POWER`; runes left to `CheckCast`), then `delete`s the spell. For a warlock with a pet casting a demon
summon it runs the loose check and does the strict-only checks (`HasGlobalCooldown` -> `NOT_READY`, `CheckShapeshift`)
itself, so the strict check does not stun the pet (the comment's reason: "Summoning Disorientation").
`CheckCast` passes `exact = (reason != nullptr)` and stores the numeric result in `*reason`.

Quirks and risks: every mask evaluation allocates and deletes a `Spell` (the early-out removes about two fifths of
them, per the comment, which I could not measure: UNVERIFIED); a `Spell` is constructed from a `Player*` that must be on
its map thread (the call sites are inside encoders, which run on map threads in training); the power-cost addition
duplicates the core's cost computation and can drift from it. No test covers this file (no match in `src/test`).
`SPELL_*_STANCE` is used by `Character/SeatCharacter.cpp:142-143` and is unrelated to spell checks.

### B.5 `SummonLevel.h` (35 lines)

`inline thread_local uint8 PendingSummonLevel = 0` (`Animus::`), "Level forced onto the next creature whose level is
selected on this thread; 0 = no override". Set only in `Opponents::SummonOpponent` (`Opponents.cpp:332-334`) around one
`Map::SummonCreature`. Read in **core**, `src/server/game/Entities/Creature/Creature.cpp:1509-1511`, immediately after
`sScriptMgr->OnBeforeCreatureSelectLevel` at `:1507`: the header comment says the hook applies it, but the application
is a direct read in `Creature::SelectLevel` (a forge edit of core, see
[01-forge-core-delta.md](01-forge-core-delta.md)).
It is not reset if `SummonCreature` throws (no RAII); in `SummonOpponent` it is reset right after the call.

### B.6 `Curriculum/BuildRetry.h` (47 lines)

`SETUP_BUILD_ATTEMPTS = 8` and `template RetryBuild(attempts, build, onFailure)` (loops `build()` until true, calling
`onFailure(attempt)` after each failure; returns whether one succeeded). Used once, at env setup
(`StageScenario.cpp:1857-1860`); tested by `BuildRetryTest.cpp` (>= 4 attempts asserted). Distinct from the four spawn
attempts inside `Rebuild` (`SPAWN_ATTEMPTS`, `StageScenario.cpp:2327`).

### B.7 `Curriculum/WarmCaches.h/.cpp` (34 + 43 lines)

`WarmCaches()` touches every table the curriculum reads from the world DB on first use so "no episode ever queries"
after
the pools are sealed: `ConsumablePool::Instance()`, `WarmGearCaches()` (defined in `Character/GearBuilder.cpp:893`),
`Opponents::OpponentPool::Instance()`, `WorldCreatures::SpawnedIds()`, `WorldCreatures::WaypointWalkerIds()`, and
`ClassAssets::For(profile)` for every `ClassProfile` (the kit, talents and catalog). Logs "Curriculum caches warmed in N
ms". Called once from `AnimusForge.cpp:239` before the DB pools are sealed (the comment at `:236-238`). Contract: any
new
world-DB read in the curriculum must be added here or it will fail (or query) after sealing. Not covered by a test.
Observed: the log line at `WarmCaches.cpp:42` is 134 columns (over the 120 limit).


# Part C1. StageScenario.h, StageState.h and StageScenario.cpp 1-2631

## S1.0 Scope of this part

Fragment S1 covers `StageScenario.h` (513 lines), `StageState.h` (684 lines) and `StageScenario.cpp` lines 1-2631
(the file has 5,192 lines; the table below lists every definition in it with its start line so the second half can be
located). Paths are relative to `src/server/game/Animus/Scenario/Curriculum/` unless they start with `src/` or `apps/`.

Where the 5,192 lines of `StageScenario.cpp` go (start lines come from a grep of the `StageScenario::` definitions):

| Lines | Content | Part |
|---|---|---|
| 1-293 | includes, anonymous-namespace helpers (levels, demands, party size, `WriteIfChanged`) | S1.3 |
| 295-587 | constructor | S1.4 |
| 589-701 | spawn ground, map, replica, phase, arena and encounter lookups, `DrawArena` | S1.5 |
| 703-1238 | destructor, `Name`, `AddCoreEpisodeInfo` | S1.6 |
| 1240-1508 | `WriteStageFiles` (stage.json and layout manifests) | S1.7 |
| 1510-1590 | `Data`, `SeatBot`, `SeatBotInWorld`, cast owner build and release, `PartyTank` | S1.8 |
| 1592-1681 and 1792-1835 | castings: `Castings`, `FitsDungeonRole`, `SpecName`, `DrawCasting`, `Weight`, `SetLayoutWeights` | S1.9 |
| 1683-1790 | wing ladder glue: `NoteWingRun`, `AppendRunEvent`, cluster tally, `FollowClusterRung` | S1.10 |
| 1837-1884 | `IsTerminal`, `Setup`, `Reset` | S1.11 |
| 1886-2385 | `Rebuild` (the episode build) | S1.12 |
| 2387-2569 | `GivePets`, `BuildSeat`, `ReuseSeat`, `Configure`, `PrepareFighter`, `StockSeats` | S1.13 |
| 2580-2630 | `AcceptResurrections` | S1.14 |
| 2632-2820 | `DeadForGood`, `ApplyGoals`, `ApplyLook`, `CameraRenderSize`, `GoalHeld`, `ApplyActions` | later part |
| 2821-3109 | `StartMover`, `SubTick`, `MayLog`, `WatchFall`, `TrackController`, `CourseKink`, targets | later part |
| 3110-3531 | `ViewSeat`, `TrackTarget`, `ApplySeatAction`, `Observe`, `AgentLayouts/Presence/Kinematics` | later part |
| 3532-4589 | `ObserveSeat`, `Paced`, `Press`, goal gap/potential/value, goal signals, `JudgePress`, `LogDeath`, `SettleIntent` | later part |
| 4590-4721 | `PinEvaluationArena`, `SetShapingScale`, `SetCostScale`, `Reward`, `TrackSeatStep` | later part |
| 4722-5192 | hazards, motion, interruptible casts, support, `SeatReward`, `WriteState`, `EpisodeInfo`, `Teardown` | later part |

Constants used throughout (`Layout/Block.h:103-110`, `Character/ClassProfile.h:33`, `Bot/BotAccounts.h:35`):
`RAID_GROUPS = 8`, `GROUP_SEATS = 5`, `MAX_SEATS = 40` (8 x 5), `MAX_SPECS = 4`, `SEATS_PER_ENV = 40`. A "party" is one
group of five; a stage's `_seatCount` is how many of the 40 `SeatState`s it uses.

## S1.1 StageScenario.h: the class

`class StageScenario final : public Scenario` (`StageScenario.h:57`). It is the only `Scenario` the forge builds
(`Scenario` is declared in `../Scenario.h`). One object exists per stage run per process; it owns every env's
`EnvState` (`_data`, `StageScenario.h:461`, sized `settings.Envs` at `StageScenario.cpp:403`) and the encounters.

### Critic state layout (`StageScenario.h:61-123`)

Three plain enums give the column offsets of the class-agnostic critic state (the `state` array on the wire):

- `StateGlobal`: `STATE_EPISODE_TIME=0`, the owner (the party follow's leader) `STATE_OWNER_PRESENT/ALIVE/HEALTH/X/Y =
  1..5`, `STATE_TIER=6`, `STATE_ARENA_FIRST=7` (a one-hot over `MAX_ARENAS` columns), `STATE_GLOBAL_COUNT = 7 +
  MAX_ARENAS` (23). Until 2026-10-08 it also held pull active/cleared/next/elite/linked, owner mana and owner in
  combat, which no encounter wrote (STATE_TIER was 13, the arena one-hot at 14).
- `StateSeat` (81-96): named columns `0..25`, `STATE_SEAT_FEATURES = 26` is the width per seat. Columns 6-11 are the
  six-number aptitude brief, 12-21 a one-hot over `PLAYABLE_CLASSES`.
- `StateEnemy` (98-123): width `STATE_ENEMY_FEATURES = 28 + RAID_GROUPS` (36; 37 before `STATE_ENEMY_ON_OWNER`, never written, was removed); columns from 11 on depend on `RAID_GROUPS`.
- The state width is `STATE_GLOBAL_COUNT + MAX_SEATS * STATE_SEAT_FEATURES + PACK_SLOTS * STATE_ENEMY_FEATURES`
  (`StageScenario.cpp:395`): `MAX_SEATS` (40) seats are always reserved, however few the stage uses.
- `stage.json` publishes the state width (`state.dim`, 1927) and where the arena one-hot sits (`state.arena_first`, `state.arena_count`,
  `StageScenario.cpp:1309-1311`). `WriteState` is at `StageScenario.cpp:5073` (a later part), and each encounter has
  its own `WriteState`.
- `LiveLayoutPinTest` pins layout constants (`add("MAX_ARENAS", ...)` list at
  `src/test/server/game/Animus/LiveLayoutPinTest.cpp:97-114`; UNVERIFIED: whether the `STATE_*` offsets are in the list
  beyond line 114).

### Public API by group (`StageScenario.h:125-303`)

Scenario overrides (called by the env pool and the forge): `Name`, `IsTerminal`, `Spec`, `Setup`, `Reset`,
`ApplyActions`, `SubTick(env, diffMs, decided)`, `ApplyGoals`, `ApplyLook`, `CameraRenderSize`,
`Observe(env, obs, state, mask, image, map)`, `AgentLayouts`, `AgentPresence`, `AgentKinematics`, `Reward`,
`EpisodeInfo`, `EpisodeInfoNames`, `SetLayoutWeights`, `Teardown`, `Playable`, `CharactersReused`, `ResetsStayOnMap`,
`EvaluationPairs`, `FilmedRole`, `TakeClusterTally`, `AddClusterTally`, `ClusterRung`, `FollowClusterRung`,
`ClusterLadderCollapsed`. The private overrides (`StageScenario.h:342-347`) are `SetStageProgress` (inline, stores the
atomic `_stageProgress`), `SetShapingScale`, `SetCostScale`, `PinEvaluationArena`, `SetStandIn`;
they are reached through the base-class virtuals, so the `private` is cosmetic.

For the encounters (accessors): `Stage()`, `Arena(env)`, `Uses(env, encounter)`, `ActiveEncounters(env)`, `Tuning()`,
`ShapingScale()`, `SpawnPoint()`, `SpawnGroundFor(env)`, `SpawnPointFor(env)`, `EnvPhase(env)`
(static), `ReplicaOf(env)`, `SpawnMapId()`, `EpisodeMapId(env)`, `SeatCount()`, `OwnerAgent()`, `CastOwnerActive(env)`,
`StandInSeat(env)`, `StandInShare(arena)`, `StandInLeads(env)`, `BuildOwnerSeat`, `ReleaseOwnerSeat`,
`DecisionScale()`, `WingRungNow()`, `NoteWingRun`, `AppendRunEvent`, `DecisionMs()`, `Layouts()`, `CastingCount()`,
`Data(env)`, `SeatBot`, `SeatBotInWorld`, `SeatTarget`, `PartyTank`, `PrepareFighter`, `DeadForGood`, `SpecName`.
(`StandInSeat`, `StandInShare`, `StandInLeads`, `DrawStandIn` are defined in
`Encounters/StandInSeat.cpp`, not in `StageScenario.cpp`.)

Static movement helpers: `StartMover`, `TrackController`, `CourseKink` (unit-tested:
`src/test/server/game/Animus/StandingTest.cpp:65-78`), plus `WatchFall`, `MayLog`, `LogDeath`.

### Contracts and invariants

- `WING_RUNGS` (`StageScenario.h:211-229`): nine `{Lift, ExtraWipes}` rungs `{8,4} {7,3} {6,3} {5,2} {4,2} {3,1} {2,1}
  {1,0} {0,0}`; the last is the evaluation's own conditions. The ladder object is built with `WING_RUNGS.size()`
  rungs (`StageScenario.cpp:302`). The ladder is described in [cpp-encounters.md](cpp-encounters.md).
- `ENV_PHASE_BITS = 31` (`StageScenario.h:177`): phase 1 (bit 0) belongs to the world; an env takes bit
  `1 + Index % 31` (`EnvPhase`, `StageScenario.cpp:631-637`).
- `OwnerAgent()` is `_seatCount`: the owner's agent slot follows the seats (`StageScenario.h:189`); it exists only when
  `_castOwner` (`StageScenario.cpp:382-386`, `AgentsPerEnv = _seatCount + 1`).
- `EvaluationPairs()` is `max(1, CastingCount())` (`StageScenario.h:255`); `CastingCount()` is the number of
  (class, build) pairs the run can field.
- `FilmedRole(env, agent)` returns `Seats[agent].DungeonRole` for agents below `_seatCount` and `MAX_SEATS`, else 0
  (`StageScenario.h:258-261`).
- `ClusterRung()` returns `WingRungNow()` cast to int (`StageScenario.h:240`), always, also for a stage with no wing
  arena (the ladder object exists in every scenario). UNVERIFIED: how `Scenario.h` documents the default.

### Members (`StageScenario.h:435-509`)

| Member | Role | Thread notes |
|---|---|---|
| `_stage` (reference) | the `StageDefinition`; must outlive the scenario | read-only |
| `_tuning` | `CurriculumTuning::Load(prefix)` result, a copy | read-only after construction |
| `_data` | per-env `EnvState` vector | each env is touched by its own map thread or the world thread; no lock |
| `_logged[LOG_KINDS=5][LOG_LAYOUTS=64]` | caps of diagnostic lines per kind and layout (`MayLog`) | `std::atomic<uint32>`, mutable |
| `_reused` | count of kept characters | atomic |
| `_layouts`, `_layoutWeights` | the layouts; the learner's WEIGHTS per (layout, spec) | `_layoutWeights` is replaced by `SetLayoutWeights` and read while drawing castings: a plain `std::vector<float>` copy-assigned, no lock. UNVERIFIED: whether the forge only delivers WEIGHTS on the world thread while no map-thread reset runs |
| `_arenaWeights`, `_arenaWeightsFinal`, `_arenaEpisodeMs`, `_arenaStandInShare` | per-arena config, fixed after construction | read-only |
| `_stageProgress`, `_shapingScale`, `_costScale` | learner-driven scalars | `std::atomic<float>`, relaxed |
| `_evaluationArena` | pinned arena index + 1, 0 = unpinned | atomic |
| `_standIn` | whether the learner plays a stand-in row (`MODE_FLAG_STAND_IN`) | atomic |
| `_wingLadder`, `_wingLadderLock` | the dungeon ladder and its mutex | the mutex guards every use in `NoteWingRun`, `TakeClusterTally`, `FollowClusterRung`; `WingRungNow()` and `ClusterLadderCollapsed()` read the ladder without it (`StageScenario.h:231, 242`) |
| `_wingFollower`, `_wingTallyProbes/Others/Rung` | cluster worker state | under `_wingLadderLock` |
| `_partyFollow`, `_party` | raw pointers into `_encounters` | set in the constructor |

### Observed issues (StageScenario.h)

- Misplaced doc comments: the comment about `ShapingScale` (`StageScenario.h:165-166`) sits above the next accessor; the
  comment about `SpawnPointFor` (171-173) sits above `SpawnGroundFor`; a doc comment for `SettleIntent`-style judging
  (376-379) is above `IsPartyTank`; 385-387 above `GoalGap`; 415-417 above `TrackInterruptibleCast`. Several private
  methods are indented wrongly (418-425: 4 spaces instead of 8). Cosmetic.

## S1.2 StageState.h: per-env and per-seat state

All plain data; the only behaviour is `SeatState::ResetEpisode()`. It is included by every encounter.

### Types

- `CombatTally` (`StageState.h:55-66`): died, deaths, `DeathCounted`, `DeathMs`, stealth counters, `PreparationMs`,
  `TimedOut`. Read by core episode info (`died`, `stealth_*`, `preparation_seconds`, `timed_out`).
- `GOAL_SLOTS = 2` and `GoalHold` (`StageState.h:71-92`): one goal held (kind, ended/reached/rewarded flags, the
  `Fresh`/`SatisfiedAtChoice` rule that a goal already true when chosen is unpaid until it stops being true and is
  reached again, the progress potential, an optional place or friend, `ProtectSafeMs`).
- `AimlessCause` (`StageState.h:96-124`): 21 causes plus `Count`: OffFocus, AoeMissed, InRangeCast, UnprovokedHarm,
  HelpOffGoal, StepAway, TargetSwitch, PetOffGoal, ConsumeNotNeeded, TrapNoEnemy, ModeFlip, ModeReverse, NeedlessMove,
  TauntOffRole, TankModeOffRole, CastFacing, CastRange, CastSight, CastMoving, CastPower, ActRefused. Each is counted
  (`AimlessBy[]`) and is an episode info column `aimless_<name>` (`AimlessCauseName`, defined elsewhere).
- `DungeonRole` (`StageState.h:130`): `DUNGEON_ANY=0, TANK, HEALER, DAMAGE`.
- `SeatState` (`StageState.h:132-624`), about 300 fields in these groups: the character (`L`, `Bot`, `Race`, `Level`,
  `Spec`, `Apt`, `Want`, `TalentPlan`, `Build`, `Stable`, `EpisodesPlayed`, `KnownRanks`); movement and controller
  (`Controls`, `Look`, `Seen`, `Hits`, `Mover`, `Link`, counters `WallMs/StuckMs/CourseKinks`, `Facing`, `Trail`, motion
  marks); the mental map and entity memory (`Map`, `Recall`, `MapPending`, `MapKeep`, `MapAgeOffset`, `MapMapId`,
  `MapInstanceId`, `MapKept`, `RecallKept`); sight block (`SightGuids`, `Focus`, `ActRefusedBy`); water and falls;
  goals (`Holds`, `GoalDecisions`, `GoalMatches`, `GoalsChosenBy`, `GoalsReachedBy`, `Event*`, `Achieved`); support and
  absorbs; hazards; interrupts; target tracking (`CurrentTargetGuid`, `DecisionTarget`, `DecisionTargetKnown`);
  supplies and pets; action pacing, repeats, jitter and intent (`Memory`, `PressTimes`, `StepAimlessBy`, ...); `Combat`
  and `Rewards` (the `RewardLedger`).
- `NO_ARENA = ~0u` (`StageState.h:627`) and `EnvState` (`StageState.h:629-681`): `Arena`, `Spawn`, `SpawnDrawn`, the
  `Seats` array (`MAX_SEATS`), `StepReward` pointer, `StepEngaged`, `ActiveSeats`, `Fresh`, `BuildFailed`, the
  encounter-fixed episode parameters (`EpisodeMapId`, `HasEpisodeMap`, `EpisodeLevel`, `EpisodeTeam`,
  `DungeonDifficulty`, `RaidDifficulty`, `HasEpisodeSpawn`, `EpisodeSpawn`), resurrection bookkeeping (`ResurrectBy`,
  `ResurrectMs`, each `MAX_SEATS + 1` long, the last for the owner slot) and `StandInPlay` (`Seat`, `Style`, `Role`).

### Invariants and contracts

- `SeatState::ResetEpisode()` (`StageState.h:458-623`) is the only reset. It does NOT reset the character fields
  (`L`, `Bot`, `Race`, `Level`, `Spec`, `Apt`, `Want`, `TalentPlan`, `Build`, `UnspentTalentPoints`, `EquippedItems`,
  `DamageScale`, `Stable`, `PetAtStart`, `EpisodesPlayed`, `KnownRanks`, `DungeonRole`), nor the client state beyond
  `Mover.Stop()` (`Mover`'s counters are snapshotted into `MoverAtStart`, line 503; `Link` is kept), nor the map and
  memory fields (`Map`, `Recall`, `Seen`, `Hits`; the map roll is set in `Rebuild`, `StageScenario.cpp:1930-1944`), nor
  `LastCourse`, `CourseX/Y`. A new field that must start each episode at zero has to be added by hand; there is no
  mechanical guard. Reviewer note: the easiest place in the scenario to leak state between episodes.
- `Controls.Clear()` and `Vision::FreeLook::Reset(Look, Vision::Current())` run in it (`StageState.h:498-501`), so held
  keys never outlive an episode; body and facing are re-seeded in `Rebuild` after placement
  (`StageScenario.cpp:2362-2377`). `Look.Render` is overwritten after `ResetEpisode` (`StageScenario.cpp:1924-1929`).
- `EnvState::StepReward` is set by `Reward` and cleared by `Observe` (comment at `StageState.h:642-644`): a goal reached
  is paid into the decision that reached it. UNVERIFIED: the set and clear sites (later part).
- `Spawn` vs `SpawnDrawn`: `SpawnDrawn` is the first draw, `Spawn` the point finally built from; the columns
  `spawn_drawn` and `spawn_point` expose them (`StageScenario.cpp:845-846`).
- `ActiveSeats` can be below `_seatCount`; seats at or above it have no layout and no bot.

### Tests

No test names `EnvState`; `StandingTest.cpp` uses a default `SeatState` for `CourseKink`. The columns reach the learner
through `stage.json`, covered by `apps/forge/python/tests/test_metric_names.py` and `test_stage_json_diff.py` (S1.7).

### Observed issues (StageState.h)

- `StageState.h:638-644` still tells the story of a removed stage (`stage2_indoor`, "three control rooms"); the v1
  curriculum is gone. `StageScenario.cpp:841-844` repeats it.
- `StageState.h:68-69`: the doc comment "One learned agent: its character ..." sits above `GOAL_SLOTS` / `GoalHold`
  rather than above `SeatState` (line 132).
- `StageState.h:482-511`: `LastKind` and `FallFrom` are reset but `LastCourse`, `CourseX/Y` are not (flags
  `HasCourse`/`HasCoursePos` gate them, so harmless).
- `Memory.Reset(0)` (line 580): the comment at 403-404 says `Memory` is sized at the first observation; code reading it
  between `Rebuild` and the first `ObserveSeat` sees size 0. UNVERIFIED whether anything does.

## S1.3 StageScenario.cpp 1-293: includes and local helpers

- Includes (19-87): `CombatReward.h` is included (line 64) for `RewardTermName` / `RewardTermCategory`; the file name no
  longer fits its content (see [cpp-rewards-routing.md](cpp-rewards-routing.md)).
- `static_assert`s (95-100): `MAX_SEATS <= BotAccounts::SEATS_PER_ENV`, `MAX_SEATS <= Animus::MAX_AGENTS`,
  `PACK_SLOTS <= Animus::MAX_TARGETS`, `NAMED_ENEMY_SLOTS <= PACK_SLOTS`, `MAX_SEATS == RAID_GROUPS * GROUP_SEATS`,
  `PARTY_MEMBERS == GROUP_MEMBERS + SPOTLIGHT_SLOTS`.
- Constants (102-133):

| Name | Value | Meaning |
|---|---|---|
| `PARTY_SPACING` | 3.0 yd | spread of party seats at spawn (`Rebuild`, 2242-2243) |
| `REWARD_TUNING_MS` | 50 | decision interval the reward terms are tuned for; `_decisionScale = DecisionMs / 50` (300) |
| `MAX_COMBAT_TIME_MS`, `MAX_UNSEEN_TIME_MS` | 60000, 20000 | UNVERIFIED: not used in lines 1-2631 (check later part, else dead) |
| `INSTANCE_CLEAR_RADIUS` | 300 yd | `SpawnArea::ClearMap` for Seek, Sight, Combat, Roles (2302) |
| `INTERACT_CLEAR_RADIUS` | 600 yd | Interact (2305) |
| `DUNGEON_CLEAR_RADIUS` | 1000 yd | party follow (2302) |
| `GOAL_RANGE_SLACK_YARDS` 5, `LOW_HEALTH_PCT` 35, `INTERRUPTIBLE_CAST_RANGE` 30 | | used in later parts (UNVERIFIED) |
| `RESURRECT_RETRY_MS` | 5000 | `AcceptResurrections` retry window (2614) |
| `ABSORB_EXPIRY_SLACK_MS` | 500 | support tracking, later part |
| `STAGE_FILE_FORMAT` | 3 | `format` in stage.json (1254) |
| `LEVEL_BANDS` | 4 | evaluation level bands (`RandomLevel`) |

- `BreathMs()` (121-124): `max(1000, sWorld->getIntConfig(CONFIG_WATER_BREATH_TIMER))`, a core config.
- `RandomTalentPlan(tuning)` (136-145): one `irand(0,99)`; `< NoisyTalentChance` Noisy, then `< + RandomTalentChance`
  Random, else Standard. Keys `Characters.NoisyTalentChance`, `Characters.RandomTalentChance`
  ([cpp-tuning-keys.md](cpp-tuning-keys.md)).
- `TankModeSpell(class)` (159-169): hard-coded ids: warrior 71 (Defensive Stance), paladin 25780 (Righteous Fury),
  druid 5487 (Bear Form), death knight 48263 (Frost Presence), else 0. Used in `Rebuild` (2161) and `PrepareFighter`
  (2539).
- `RandomLevel(minLevel, fixed, tuning, seedIndex, layouts)` (171-198): (a) `fixed` (`StageSettings::Level`) wins,
  clamped to `[minLevel, 80]`; (b) an evaluation (`seedIndex != NO_EPISODE_SEED`) takes a band from its seed: width
  `80 / 4 = 20`, `band = (seed / layouts) % 4`, level `urand(max(minLevel, band*20+1), min(80, (band+1)*20))`, a band
  wholly below `minLevel` falls to `urand(minLevel, 80)`; (c) training: one roll `irand(0,99)`: `< HighLevelChance`
  gives `urand(HighLevelFirst, 80)` when `minLevel <= highFirst`; else `< HighLevelChance + LowLevelChance` gives
  `urand(minLevel, LowLevelLast)` when `minLevel <= lowLast`; else `urand(minLevel, 80)`. Keys
  `Characters.HighLevelChance`, `HighLevelFirst`, `LowLevelChance`, `LowLevelLast`. Note the band divides the seed by
  `layouts` (the class count), but `DrawCasting` spreads seeds over the (class, build) pairs
  (`CastingCount()`), so the pair-by-band cross is not exactly even.
- `ClassicDemands(seats)` (206-220): seat 0 of every group of `GROUP_SEATS` gets `HoldsThePull`, seat 1 `KeepsThemUp`,
  the rest `Anything`.
- `RollDemand(tankChance, healerChance)` (225-234): one roll: tank, else healer, else anything.
- (deleted 2026-10-08) `RandomPartySize` and `Party.SizeWeight1..4`: no live arena reached the draw.
- `OtherPower(unit)` (259-267): non-mana power fraction; UNVERIFIED users after line 2631.
- `WriteIfChanged(path, content)` (271-292): compares size then bytes; otherwise writes `<path>.partial` and renames.
  Returns false on error. Used for every manifest and `stage.json`.

### Observed issues (S1.3)

- `StageScenario.cpp:150-156`: the doc comment of `RandomLevel` sits above `TankModeSpell` (a function was inserted
  between them).
- `StageScenario.cpp:2300`: a line over 120 columns.

## S1.4 The constructor (`StageScenario.cpp:295-587`)

Signature `StageScenario(StageSettings const& settings, StageDefinition const& stage)`. Runs on the world thread at
stage start. In order:

1. Member init (295-304): `_tuning = CurriculumTuning::Load(settings.TuningPrefix)` (the prefix is
   `"AnimusForge.Curriculum."` for the forge, `ForgeConfig.cpp:669`); the spawn map and point are the stage's
   (`stage.MapId`, `stage.SpawnPoints.front()`) when the stage names a map, else the settings'
   (`StageSettings::SpawnMapId`
   default 560, `SpawnPosition`); `_seatCount = stage.SeatCount()`; `_level = settings.Level`;
   `_decisionScale = DecisionMs / 50`; `_wingLadder(WING_RUNGS.size(), Instance.WingRungRuns, Instance.WingRungTarget,
   Instance.WingRungStart)`. The comment says the ladder is not saved with the policy: a resumed run names its rung.
2. `_eventsLog = settings.EventsLog` (305).
3. `_continent` = the spawn map is not instanceable (307-308).
4. `_resetsStayOnMap` (313-317): a continent stage with no arena that has a `PartyGroup`, is `Opposition::Instance` or
   names another map. Such a stage resets on the map thread (gated further by `StageSettings::ResetOnMapThreads`,
   default true; UNVERIFIED in `EnvPool`). Every live stage is an instance stage (UNVERIFIED: confirm in
   `Stages.cpp`), so for them this is false and the preload branch below never runs.
5. If `_resetsStayOnMap`: preload every grid within +-80 yd of each spawn point of the stage and of each arena on the
   base map (`EnsureGridCreated`), so a map-thread reset never waits on the world thread for collision (319-348).
6. `_envsPerReplica = clamp(ceil(Envs / max(1, ContinentReplicas)), 1, 31)` (354-355).
7. Layouts (357-371): for each `ClassProfile` of `ClassProfiles()` not excluded by `settings.Classes` and with
   non-empty `ClassAssets::For(profile).Races`, `Layout::Build(profile, _stage)`; `Index` = position. The order is the
   class table's; "classes only append" (principle 15) keeps indexes stable.
8. With any `PartyFollow` arena every profile's `ClassAssets` is built now (seconds each), since the leader may be any
   class (375-377).
9. `_castOwner` = any arena is `PartyFollow` (382-385); `AgentsPerEnv = _seatCount + (_castOwner ? 1 : 0)` (386).
10. `ScenarioSpec` (387-402): `ObsDim` and `NumActions` are the maxima over layouts; one `LayoutSpec{name, obsDim,
    numActions}` per layout; `StateDim` as in S1.1; `ImageBytes = Vision::ImageBytes(Vision::Current())` and
    `LookHeads = Vision::FreeLook::HEADS` only with `BlockId::Vision`, else 0; `MapBytes = Vision::CROP_BYTES` only
    with `BlockId::Map`.
11. Encounters (405-456), each created only when some arena asks, in this build order: `PartyEncounter` (an arena with
    `PartyGroup`), `InstanceEncounter` (`Against == Instance`), `PartyFollowEncounter`, `SeekEncounter`,
    `SightEncounter`, `InteractEncounter`, `CombatEncounter`, `RolesEncounter`. The comment at 431 says the party
    precedes the instance because the instance moves the group to the boss.
12. `_rewardOrder` (458-463): `instance, _party, _partyFollow, seek, sight, interact, combat, roles` (null ones
    skipped): a different order from `_encounters` (party first). `_arenaEncounters[a]` keeps `_encounters` order,
    `_arenaRewardOrder[a]` keeps `_rewardOrder` order. The comment at 458-460 says an encounter left out of the list
    still runs and only its columns and terms are missed; all eight are in the list. The comment at 427-428 says reward
    order does not matter because shared inputs are computed before any `Reward`; the header comment on
    `_rewardOrder` (`StageScenario.h:465`) says "a reward may read what an earlier one recorded": the two comments
    disagree. UNVERIFIED which is true.
13. Per arena (465-505): the encounters it uses; config `<prefix>Arena.<stage>.<arena>.Weight` (default `arena.Weight`),
    `...WeightFinal` (default `arena.WeightFinal` if >= 0, else the Weight; negative config values clamp to 0),
    `...StandInShare` (default `arena.StandInShare`, clamped -1..100); episode length
    `(arena.EpisodeSeconds ? : settings.EpisodeSeconds) * 1000`; `longestMs` is the maximum and becomes
    `_spec.LongestEpisodeSeconds` (521). Read with `sConfigMgr->GetOption(..., false)`.
14. Weight fix-ups (513-519): all final weights 0 -> final = initial; all weights 0 -> error log and every weight 1.
15. `ConsumablePool::Instance()` is touched so the pool exists before play (523).
16. Episode info (525-573): `AddCoreEpisodeInfo()`, `AddStandInEpisodeInfo()` (in `StandInSeat.cpp`), each
    encounter's `AddEpisodeInfo` in reward order; then a `reward_<term>` column for every term any encounter pays
    (skipping names already present); then the fixed scenario-paid terms Repeat, Jitter, Aimless, Effort, Fidget,
    SelfHealing, GoalReached, GoalSwitch, Hazard, HealingMana, CombatClock; then `vision_render_width` (vision block
    only) and `score_outcome` (`Rewards.Score()` of the seat, 0 for an agent at or above `_seatCount`). The column
    ORDER is the order of these calls.
17. `_spec.EpisodeInfoDim = _info.Size()`, `_spec.GoalCount = GOAL_JOINT_COUNT` (575-576).
18. When `settings.LayoutsDir` is non-empty, `WriteStageFiles(settings)` (578-579).
19. Debug logs (581-586).

Contracts: a new encounter has to be added in three places: the `add` (405-456), the `initializer_list` of 460 and the
`uses` lambda (469-477). `apps/forge/tools/sim_metrics.py` reads these `add(std::make_unique<...>)` lines and the
`AnyArena(...)` conditions as text (header of that script, lines 14-17), so reformatting this block can break
`apps/forge/python/tests/test_metric_names.py`.

Config read directly: `<prefix>Arena.<stage>.<arena>.Weight|WeightFinal|StandInShare`;
everything else through `CurriculumTuning::Load`, `Vision::Current()` and `Vision::MapCurrent()`.

### Observed issues (constructor)

- `StageScenario.cpp:2317`, comment says "OwnerEncounter::Build fills it": no `OwnerEncounter` exists in the tree
  (grep: only this comment); the slot is filled by `PartyFollowEncounter`.
- `StageScenario.cpp:379-381` and `StageScenario.h:505-509` argue in comments that the leader "is no dead code"; they
  read as leftovers of a review thread.
- The `_resetsStayOnMap` branch (319-348) runs for no live stage (if confirmed): dead in practice.
- The constructor does file I/O and heavy asset building; a scenario cannot be constructed in a unit test without the
  whole world stack, which is why only static helpers are tested.

## S1.5 Spawn ground, map, replica, phase, arena draw (`StageScenario.cpp:589-701`)

- `SpawnGroundFor(env)` (589-606): the positions an episode may start on. Empty when neither the stage nor the arena
  has a map. If the arena has its own `SpawnPoints`, those; else, if the arena's map differs from the stage's, empty
  (the startup check `CurriculumProblems()` refuses an arena on its own map without spawn points, comment at 601);
  else the stage's `SpawnPoints`. The empty case returns a function-local static.
- `SpawnPointFor(env)` (614-624): `EnvState::EpisodeSpawn` when an encounter fixed it (`HasEpisodeSpawn`), else
  `ground[min(Spawn, size-1)]`, else `_spawnPoint`.
- `EpisodeMapId(env)` (608-612): `EpisodeMapId` if `HasEpisodeMap`, else `_spawnMapId`.
- `ReplicaOf(env) = env.Index / _envsPerReplica` (626-629). `EnvPhase(env) = 1 << (1 + env.Index % 31)` (631-637),
  at most `1 << 31`.
- `Arena(env)` (639-643): the stage's arena at `Data(env).Arena`, arena 0 when out of range (`NO_ARENA` before the
  first episode).
- `Uses`, `ActiveEncounters`, `ActiveRewardOrder` (645-665): lookups into `_arenaEncounters` / `_arenaRewardOrder`;
  an empty static vector for `NO_ARENA`.
- `DrawArena(evaluating)` (667-701): (1) an evaluation pinned by `_evaluationArena` (set by `PinEvaluationArena`, the
  learner's `eval.heldout`) returns `pinned - 1`; (2) a single-arena stage returns 0 unless that arena is `EvalOnly`;
  (3) otherwise `ArenaDrawWeights(arenas, weights, finalWeights, evaluating, progress)` (in the stage definition part;
  per the comment at 675-677 it interpolates Weight to WeightFinal over `_stageProgress`, an evaluation uses the final
  weights, a held-out arena gets none;
  UNVERIFIED
  by reading that function, see [cpp-stagescenario.md](cpp-stagescenario.md)); (4) all weights 0 returns the first
  non-`EvalOnly` arena; (5) a weighted `urand` pick from the world thread's engine, so an evaluation's draw
  follows its seed.

### Observed issues

- `StageScenario.cpp:672`: the single-arena shortcut returns 0 without a random draw ("so its random numbers are as
  before", `StageScenario.h:340`); an `EvalOnly` single arena goes through the weighted path. A trap for
  reproducibility.

## S1.6 Episode info: `AddCoreEpisodeInfo` (`StageScenario.cpp:710-1238`)

`_info` is an `EpisodeInfoTable` (`Encounters/EpisodeInfoTable.h`): ordered (name, getter
`float(Env const&, uint32 seat)`). `StageScenario::EpisodeInfo` (line 5161, later part) writes one row per seat.
The core columns, in the order added (per seat unless noted):

- Damage (714-737): `damage`, `dps` (over episode seconds), `combat_dps` (over `CombatMs`, at least 1 s), `dps_scaled`
  (`combat_dps / max(1, DamageScale)`), `white_damage`, `special_damage`.
- Character (738-751): `level`, `race`, `spec`, `talent_plan` (0 standard, 1 noisy, 2 random),
  `unspent_talent_points`, `equipped_items`, `spell_casts`.
- Water (758-787): `swim_seconds`, `dive_seconds`, `breaths`, `breath_spent`, `drowning_damage`, `drowned` (also true
  for a dead seat with `DrowningDamage > 0`, 768-773), `water_walk_seconds`, `aquatic_seconds`, `breathing_casts`.
- Leaving the ground (788-796): `jumps`, `drops`, `fell`, `fall_damage`, `fall_deaths`, `void_deaths`, `into_terrain`.
- Misc (797-812): `options_started`, `option_seconds`, `trinket_uses`, `item_uses`, `class` (the profile's class id).
- Aptitude (816-825): `aptitude_mitigation`, `aptitude_healing` (max of direct and HoT heal aptitude).
- Presence and arena (828-849): `present` (the seat has a layout and is not the stand-in's seat), `arena` (index into
  stage.json's `arenas`), `spawn_point`, `spawn_drawn`, `build_failed`. The last four are env-level values repeated on
  every seat's row.
- Combat tally (856-947): `died`, `health_left`, `stealth_openers`, `stealth_utility_casts`, `preparation_seconds`,
  `pet_damage_share`, `melee_damage_share`, `shot_damage_share`, `spell_damage_share`, `pet_died`, `pet_abilities`,
  `pet_orders`, `pet_attack_orders`, `pet_passive_orders`, `pet_defensive_orders`, `pet_aggressive_orders`,
  `pet_follow_orders`, `pet_stay_orders`, `pet_out_seconds`, `pet_attacking_share`, `pet_passive_share`,
  `pet_staying_share`, `pet_at_start`, `consumables_used`, `self_resurrections`.
- End state (950-977): `timed_out`, `target_health_left`, `distance_at_end` (0 unless seat and target share a map),
  `form_at_end` (shapeshift form id), `power_left` (own power type's fraction).
- Intent and rates (984-1049): `actions_per_minute`, `serving_share`, `aimless_presses`, `aimless_<cause>` per
  `AimlessCause` (`ActRefused` only with a sight block), `act_refused_<reason>` per `EntityActions::Refusal` from 1
  (sight stage only), `goal_success_<kind>` per goal kind, `mode_switches`, `effort_presses`, `move_starts_per_minute`,
  `move_stop_starts` (per minute), `fidget_seconds`, `combat_actions_per_minute`.
- Smoothness (1051-1073): `repeated_presses`, `turn_reversals`, `bearing_flips`, `pitch_reversals`, `weaves`.
- Controller (1078-1106): `moves_refused`, `wall_seconds`, `stuck_seconds`, `course_kinks` (per minute),
  `control_changes_per_minute`, `move_reports_per_minute`.
- Support (1119-1189): `healing_per_mana`, `hot_healing_share`, `healing_mana_spent` (over max mana), `healing_done`
  (over max health), `protection_done`, `overheal_share`, `heals_on_full`, `defensive_casts`, `healing_casts`,
  `downranked_share`, `low_health_seconds`.
- Hazard and interrupts (1194-1207): `hazard_seconds`, `hazard_damage`, `interruptible_casts_seen`.
- Goals (1216-1237): `goals_reached`, `goals_lost`, `goal_targeted_share`, `goal_<kind>_share` per goal kind,
  `goal_match_share`, `goal_changes`.

After these the constructor adds the stand-in columns (`StandInSeat.cpp`), the encounters' columns, the `reward_<term>`
family, `vision_render_width` and `score_outcome` (S1.4 item 17).

Contracts:
- Getters read `Data(env)`, `env.EpisodeStats` or `env.FindBot` when the episode ends. `EpisodeInfoTable::Contains` is
  used only for the `reward_` family (536), so a duplicated non-reward name would silently produce two columns.
- `env.FindBot` takes the global object accessor's lock (comment `StageScenario.h:282-284`); `health_left`,
  `power_left`, `form_at_end`, `drowned`, `hazard_damage`, `healing_mana_spent` call it once per episode end.
- `_spec.EpisodeInfoDim = _info.Size()` goes to the learner (575); adding a column changes the wire width and the
  stage.json list. The learner finds columns by name. Which of these the learner configs use is in
  [metrics.md](metrics.md); the extraction of the whole name list from this C++ is `apps/forge/tools/sim_metrics.py`.

### Observed issues (S1.6)

- Dangling comments with no code under them: `StageScenario.cpp:752-757` (jumps, falls, water: the jump columns come at
  788); `978-983` (opponent pathing, melee-reach share, roots and snares, feign deaths: the duel opponent is gone and no
  column follows).
- `StageScenario.cpp:961`: `distance_at_end` is 0 when the seat or target is missing or on another map, which a reader
  cannot tell from "adjacent".
- `present` (828-831) excludes the stand-in seat but the other columns (`damage`, `dps`, ...) still carry the frozen
  partner's row. UNVERIFIED how the learner masks it (`apps/forge/python/animus/evaluation.py`).
- `arena`, `spawn_point`, `spawn_drawn`, `build_failed` are per-env, repeated per seat row, so any per-seat mean weights
  them by seat count.
- Most core columns exist in every stage even where meaningless (a stage without pets or water reports 0): the table is
  wide (roughly 190 core columns; UNVERIFIED exact count, use `sim_metrics.py --stage <name>`).

## S1.7 `WriteStageFiles` (`StageScenario.cpp:1240-1508`): stage.json

Called once from the constructor when `settings.LayoutsDir` is set (`StageSettings.h:78-79`: empty = nowhere). Writes
into `<LayoutsDir>/<stage name>/`: one `<layout.ModelName()>.json` per layout (`layout.Manifest()`, warning on
failure, 1248-1251) and `stage.json`. Every write goes through `WriteIfChanged`.

`stage.json` keys in insertion order (`boost::json::object` serializes in insertion order):

| Key | Content | Line |
|---|---|---|
| `format` | 3 | 1254 |
| `stage`, `suffix`, `extends`, `summary` | from `StageDefinition` | 1255-1258 |
| `seats` | `_seatCount` | 1259 |
| `blocks` | the stage's block names in layout order | 1261-1263 |
| `arenas[]` | per arena: `name`, `weight` (the configured initial weight), `seats`, `episode_seconds`, `plan` ("solo" or "party"), `eval_only`, `stand_in_share` (resolved), `drill_seat` (0 if `DrillRole` else -1) | 1266-1286 |
| `cast[]` | with `_castOwner`: `{agent: OwnerAgent(), name: "leader"}` (or "owner" when `_partyFollow` is null) | 1289-1296 |
| `seed_chain` | the `Extends` ancestors, closest first, via `FindStage` | 1299-1301 |
| `merges` | the stage's `Merges` | 1304-1306 |
| `state` | `{arena_first, arena_count}` | 1309-1311 |
| `models` | class name -> model name | 1315-1317 |
| `layouts{class}` | `obs_dim`, `num_actions`, `action_names`, `spec_names`, `spec_roles` ("tank" / "healer" / "damage" by `StatProfile`), `sets` (`DescribeSeatSets`), `blocks[]` | 1320-1403 |
| `episode_info` | the column names in order | 1405-1407 |
| `episode_categories` | name lists for categorical columns: `seek_room`, `seek_object`, `interact_site`, `interact_object`, `sight_object`, `objective_corner` (`["in_sight","corner"]`) | 1411-1453 |
| `reward_terms` | every `RewardTerm` name -> "outcome", "cost" or "shaping" | 1457-1466 |
| `goals` | `kinds`, `accepts` (kind by target 0/1 matrix), `targets`, `block` = "goal", `columns` (the goal block's column offsets), `slots_on_wire` | 1471-1503 |
| `tuning` | `_tuning.Json()`, the effective tuning | 1504 |

Per block entry in `layouts[].blocks[]` (1349-1402): `name`, `obs` and `actions` spans, `revision` when nonzero,
`action_features` (core block only), the vision block's `image`/`camera`/`look`, the entities block's `entities`, the
map
block's `map`, the sight block's `sight` (each from `GetBlock(id).DescribeManifest`), `obs_names` (`DescribeColumns`)
and `rescaled` (`DescribeRescaled`) when non-empty. Seeding by name depends on `obs_names` (principle 15).

Consumers: the learner (`apps/forge/python/animus/stages.py`: seed chain, merges, block spans, arena names, state
span), bootstrap seeding, evaluation categories, export; the file is copied into every run directory. Tests that read
or compare it: `apps/forge/python/tests/test_stage_json_diff.py`, `test_metric_names.py`, `test_bootstrap.py`,
`test_export.py`, `test_export_seat_sets.py`, `test_party_frames_seeding.py`, `test_heldout.py`. File format for the
learner: [file-formats.md](file-formats.md).

### Observed issues (S1.7)

- `stage.json` carries `arenas[].weight` only; `WeightFinal` is not written, so the learner cannot know the arena
  interpolation. UNVERIFIED whether the learner needs it (check `animus/stages.py`).
- `"name": _partyFollow ? "leader" : "owner"` (1295): inside `if (_castOwner)` `_partyFollow` is always set, so the
  "owner" branch is unreachable.
- `STAGE_FILE_FORMAT` comment (`StageScenario.cpp:132`) says 2 added arenas and 3 the seat plan and cast list; later
  fields (`reward_terms`, `goals`, `state`, `drill_seat`, `stand_in_share`) did not bump it, so `format` does not
  identify the schema. `docs/forge/03-animus-lib.md:405` says "format 2": stale.
- The inner `categories` (1458) shadows the outer one (1411): harmless but confusing.
- `episode_categories` has no entry for combat, roles or wing columns; they are plain numbers.

## S1.8 Env and seat access, owner seat (`StageScenario.cpp:1510-1590`)

- `Data(env)` returns `_data[env.Index]` unchecked (1510-1518). `SeatBot` returns the slot's `Bot.Active()` (the bot "in
  or out of the world", see `BotSlot`) for `seat < MAX_SEATS`, else null. `SeatBotInWorld` adds `IsInWorld()` and exists
  to avoid `Env::FindBot`'s lock in loops that run on every map thread every decision (1525-1529).
- `CastOwnerActive(env)` (1531-1538): `_castOwner && !env.Evaluating && arena is PartyFollow && _partyFollow &&
  _partyFollow->IsCast(env)`. In an evaluation the leader always plays its script (comment at 1535); in training only
  when the encounter drew the cast.
- `BuildOwnerSeat(env, map, level, start, demand)` (1540-1576): draws a casting evenly (`weighted = false`: a converged
  class is as good an owner), sets `seat.L/Spec/Want`, `Bot.Begin()`, `BuildSeat`; on failure `Bot.Abort()` and null;
  else `PrepareFighter`, `Bot.Promote()`, stores the GUID in `env.Bots[agent]`, stocks supplies like a seat
  (`ConsumablePool::Supplies`, `StockBattleSupplies`) and gives a pet with probability `Characters.PetOutChance`.
  Called by `PartyFollowEncounter::Build`.
- `ReleaseOwnerSeat(env)` (1578-1585): `Bot.Destroy()`, clears `L` and the `env.Bots` slot.
- `PartyTank(env)` (1587-1590): the `PartyEncounter`'s living tank when the arena has a party group.

## S1.9 Castings and layout weights (`StageScenario.cpp:1592-1681, 1792-1835`)

- A `Casting` is `{Layout const* L, uint8 Spec}` (private, `StageScenario.h:307`).
- `Castings(demand)` (1592-1613): if the demand is not `Any`, every layout's specs meeting it
  (`ClassAssets::SpecsMeeting`); if that is empty or nothing was asked, every (layout, spec) pair. An unsatisfiable
  demand silently falls back to everything.
- `FitsDungeonRole(casting, role)` (1615-1635): by the spec's `StatProfile`: tank = `Tank`; healer = `Healer` and not
  `Tank`; damage = neither. `DUNGEON_ANY` or an unknown casting fits.
- `SpecName(layout, spec)` (1637-1644): the spec's name or "?".
- `DrawCasting(env, seat, demand, weighted)` (1646-1681): evaluation (`EpisodeSeedIndex != NO_EPISODE_SEED`): pair
  `(seed + seat) % count`; training: by `Weight(layout, spec)` when `weighted`, else (or all weights zero) uniform
  `urand`. Empty `castings` would divide by zero, but `Setup` refuses an empty layout list (1849-1853).
- `Weight(layout, spec)` (1792-1796): `_layoutWeights[layout.Index * MAX_SPECS + min(spec, MAX_SPECS-1)]`, 1.0 when the
  vector is empty or too short.
- `SetLayoutWeights(weights)` (1798-1835): empty clears; wrong length (`!= layouts * MAX_SPECS`), a non-finite or
  negative entry or a zero sum is logged as an error and the old weights kept; else copied. They come from the
  learner's WEIGHTS message (pairs furthest below baseline get more data).
- Tests: UNVERIFIED; no test in `src/test` names `DrawCasting` or `SetLayoutWeights`.

Reviewer notes: weights are layout-major and `MAX_SPECS` (4) wide, so a stale weights message from before an appended
class has a different length and is refused with an error log, not a crash. A class with more than 4 specs would
be clamped to spec 3's weight (`min(spec, MAX_SPECS - 1)`).

## S1.10 Wing ladder glue and the cluster tally (`StageScenario.cpp:1683-1790`)

The ladder itself (`WingLadder`, rungs, probes, collapse alarm) is in [cpp-encounters.md](cpp-encounters.md); this is
how the scenario hosts it.

- `NoteWingRun(rung, probe, progress)` (1683-1724): called by `InstanceEncounter` when a whole-dungeon run ends
  (`progress` is the share of the dungeon cleared, 1 when the last boss died). Under `_wingLadderLock`: clamp progress
  to [0,1]; on a cluster worker (`_wingFollower`) the run is NOT counted locally: dropped if `rung` differs from the
  worker's rung, else appended (3 decimals) to `_wingTallyProbes` or `_wingTallyOthers` (both cleared when the rung
  changed since the last tally) and the function returns. On the host or a single process: `_wingLadder.Note(rung,
  probe, progress)`; a `Moved` result logs at INFO the step down (rung from/to, mean of the last `Instance.WingRungRuns`
  probes, `Instance.WingRungTarget`, the others' mean); an `Alarm` is logged as WARN AND appended to events.log; a
  `Cleared` message is logged INFO and appended to events.log. The comment (1685-1688): the ladder moves only on the
  probes against a fixed target; it never steps back on a score; the collapse alarm "is the host's, here".
- `AppendRunEvent(line)` (1726-1744, `const`): appends `<UTC ISO-8601 Z> <stage>: <line>\n` to `_eventsLog`
  (`StageSettings::EventsLog`, `<RunsDir>/<stage>/events.log`), creating the directory; a no-op when empty; a failed
  open logs a warning. Not locked itself (only its caller `NoteWingRun` holds `_wingLadderLock`; the method is public).
- `TakeClusterTally()` (1746-1756): a worker's pending runs as `"<rung>/<probes>/<others>"` (comma lists of 3-decimal
  progress values, or `-`); clears them; empty when not a follower or nothing pending. Call site
  `src/server/game/Animus/AnimusForge.cpp:1144`.
- `AddClusterTally(tally)` (1758-1780): the host parses a worker's tally and feeds each value into `NoteWingRun`,
  OTHERS first, then probes. A tally with fewer than two `/` is dropped silently. Call site `AnimusForge.cpp:1083`.
- `FollowClusterRung(rung)` (1782-1790): sets `_wingFollower = true` (permanently for this scenario object), clamps
  `rung` to `WING_RUNGS.size() - 1`, logs a change, `_wingLadder.Follow(next)`. Call sites `AnimusForge.cpp:1143, 1218`.
- `ClusterRung()` / `ClusterLadderCollapsed()` read the ladder; the forge reports them (`sim.WingLadderCollapsed`,
  `AnimusForge.cpp:2169`).

Contracts: a worker reports only runs at the rung it is on; a run that started on an earlier rung and finished after
the host moved the rung is dropped (1695-1696). On the host, `AddClusterTally` calls `NoteWingRun` with the worker's
reported `rung`; UNVERIFIED how `WingLadder::Note` treats a rung that is not the ladder's current one
(`Encounters/WingLadder.cpp`).

### Observed issues

- `WingRungNow()` and `ClusterLadderCollapsed()` read `_wingLadder` without `_wingLadderLock` (`StageScenario.h:231,
  242`) while `NoteWingRun` writes under it. UNVERIFIED whether `Rung()` is atomic; if it is a plain integer this is a
  data race (benign in practice, undefined in C++).
- The comment at `StageScenario.cpp:301` says a resumed run "names the rung it had reached"; UNVERIFIED who passes that
  into `Instance.WingRungStart`.
- No stall alarm for the wing ladder (known; `docs/forge/deploy-gate.md`).

## S1.11 Setup, Reset, IsTerminal (`StageScenario.cpp:1837-1884`)

- `IsTerminal(env)` (1837-1845): true if `BuildFailed`, else true if any ACTIVE encounter's `IsTerminal(env)` is true.
  Time-outs are not here; the env pool ends an episode at `EpisodeLengthMs` (set in `Rebuild`).
- `Setup(env)` (1847-1867): false with an error when `_layouts` is empty. Otherwise `RetryBuild(SETUP_BUILD_ATTEMPTS =
  8,
  Rebuild, log)` (`BuildRetry.h:30`); on success `Fresh = true`.
- `Reset(env)` (1869-1884): if `Fresh`, clears it and returns (`Setup` already built the first episode); else
  `BuildFailed = !Rebuild(env)`; a failed build logs an error, the episode ends at the next `IsTerminal` and the next
  reset rebuilds. There is no retry cap in `Reset` (`Setup` has 8).

Threads: `Setup` on the world thread; `Reset` on the world thread or, when `ResetsStayOnMap()` and
`StageSettings::ResetOnMapThreads`, on the thread updating the env's map.

## S1.12 `Rebuild(env)`: the episode build (`StageScenario.cpp:1886-2385`)

This is the "reset and build" step of an episode's lifecycle. It returns false when it cannot build (the seats are
restored to their previous characters). Timers go into the thread-local `CurrentReset` (`Env/ResetTiming.h:52`):
`PrepareNs`, `SeatsNs`, `DespawnNs`, `DestroyNs`, `EncounterNs`, `StockNs`, plus `CreateNs/PlaceNs/ConfigureNs` from
`BuildSeat`.

1. **Arena** (1892-1895): remember `previousEncounters`; `Arena = DrawArena(env.Evaluating)`; clear `StandInPlay`.
2. **Episode parameters** (1896-1904): `EpisodeMapId = arena.MapId` (`HasEpisodeMap` = nonzero); `EpisodeLevel`,
   `EpisodeTeam`, `DungeonDifficulty`, `RaidDifficulty`, `HasEpisodeSpawn` cleared.
3. **Spawn draw** (1910-1914): `Spawn = urand(0, ground.size()-1)` (0 with no ground); `SpawnDrawn = Spawn`. One draw
   per
   episode from the seeded engine.
4. `env.EpisodeLengthMs = _arenaEpisodeMs[Arena]` (1916).
5. **Seat totals** (1919-1920): `ResetEpisode()` on all `MAX_SEATS` seat states.
6. **Camera size** (1924-1929): with a vision block only, `Look.Render = Vision::DrawRenderSize(Vision::Current(),
   draw)`
   per seat (`frand`, clamped below the total with `nextafter`).
7. **Map keep roll** (1934-1944): with a Map or Sight block, per seat `MapPending = true`, `MapKept = false`,
   `MapKeep = !Evaluating && KeepShare > 0 && frand < KeepShare`, `MapAgeOffset = frand(0, AgeOffsetSeconds)` when
   kept. Config `AnimusForge.Map.KeepShare` (0..1) and `AnimusForge.Map.AgeOffsetSeconds` (0..36000,
   `ForgeConfig.cpp:320-321`). Applied at the first observation.
8. **Offers cleared** (1946-1947): `ResurrectBy.fill(NO_SEAT)`, `ResurrectMs.fill(0)`.
9. **Encounter resets** (1948-1949): `ResetEpisode(env)` on EVERY encounter of the stage, used or not.
10. **Deactivate** (1952-1954): encounters the previous arena used and the new one does not get `Deactivate` (default
    `Teardown`); then `BeforeRebuild(env)` on each active encounter (1956-1957).
11. **Old targets** (1962-1965): the creatures still in `env.Targets` after the encounters cleared theirs are collected
    to despawn later; the comment records a crash (a creature despawned twice, `Map.cpp:682` PendingAdd assert).
12. `firstBuild = !SeatBot(env, 0)`; `Bot.Begin()` on every seat of `_seatCount` (1967-1969).
13. **Save the old characters** (1972-1993) in a local `Character` array (layout, race, level, spec, talent plan, damage
    scale, build, unspent points, equipped items, known ranks) to restore on failure.
14. **Seats and classes** (1997-2115), `ActiveSeats = arena.SeatCount()`:
    - Party arenas (`SeatPlan::Party`): the size is the arena's `SeatCount()` (the size draw was deleted). Demands start as `ClassicDemands` (tank seat 0, healer seat
      1 of the group); a drill swaps the drilled role into seat 0 (healer: swap 0 and 1; damage: swap 0 and 2)
      (2016-2019). "Classic" holds for a drill, an instance, a proper party, or with probability `Party.ClassicChance`
      percent; a classic non-drill is shuffled with `RandomEngine::Instance()`; a non-classic party draws each seat's
      demand with `RollDemand(Party.RoleTankChance, Party.RoleHealerChance)` (2023-2028). Each active seat then draws a
      casting meeting its demand (`DrawCasting`); seats past `ActiveSeats` get no layout. `DungeonRole` is `DUNGEON_ANY`
      at this point.
    - "Proper" (`arena.Instance == Wing` in an Instance arena, or `ProperParty`): every active seat gets `DungeonRole`
      from its demand (tank, healer, damage) and redraws among the castings that `FitsDungeonRole`; for a stage with
      `FocusChance >= 100` only classes whose `MinLevel <= FocusLevelLast` are eligible (keeps death knights out of
      band stages, 2052, 2058); training draws by the learner's weights, an evaluation by `(seed + seat) % fits`.
    - Solo arenas: with `Characters.KeepCasting && Characters.ReuseEpisodes > 0`, not the first build, training, no
      seed,
      a seat whose previous bot is active with `EpisodesPlayed < ReuseEpisodes` keeps its casting; the others draw over
      every pair with `Anything` (2095-2114).
15. **Encounter-fixed parameters** (2119-2120): `BeforeLevel(env)` on each active encounter (an instance writes map,
    level, difficulty and spawn).
16. **Level-capped redraw** (2128-2150): for a wing instance arena or a `PartyFollow` arena all seats from 0, else from
    seat 1, when the fixed `EpisodeLevel` is below a seat's class `MinLevel` (a death knight, 55), redraw among castings
    that can be that level and fit its dungeon role; keep the old when none. Seat 0 of other arenas is left alone
    because the rung was drawn for it (comment 2122-2126).
17. **Minimum level** (2155-2164): max over seats of the class `MinLevel`; for a drawn tank also the level its tank-mode
    spell is learned (`Kit->LevelOf(TankModeSpell(class))`); then `_stage.MinLevel`.
18. **Reuse flags** (2171-2192): need `Characters.ReuseEpisodes > 0`, not the first build, not an evaluation, not
    `changesMap` (a different episode map, or an arena with `Instance == Wing`, which always opens a fresh instance); a
    seat is reused when its bot is in world and not teleporting, same layout and spec, `EpisodesPlayed <
    ReuseEpisodes`, old level >= `minLevel`, and consistent with a single kept level, a fixed `EpisodeLevel` and
    `EpisodeTeam`. All reused seats share `keptLevel`.
19. **Level** (2195-2207), first match wins: `EpisodeLevel` (clamped to `[minLevel, 80]`); `_stage.Level` (clamped);
    `keptLevel`; the stage focus band (`FocusChance` percent in training, or any evaluation of a 100%-focus stage:
    `urand(max(minLevel, FocusLevelFirst), FocusLevelLast)`); else `RandomLevel(...)`. A nonzero `StageSettings::Level`
    (`_level`) disables the focus band.
20. **Map** (2213-2226): reuse `env.FindMap()` unless it is the first build, a fresh wing instance, or the map id
    differs
    from the episode's; for a non-instanceable episode map use `CreateContinentReplica(map id, ReplicaOf(env))`.
21. **Place seats** (2233-2278): per active seat a start = `SpawnPointFor(env)`, in a party offset by `PARTY_SPACING`
    (alternating sides, rows by group); `ReuseSeat` when flagged (falling back to `BuildSeat` if it fails) else
    `BuildSeat`. If any seat cannot be built every seat's new bot is aborted, every saved character field and
    `ActiveSeats` are restored and the function returns false.
22. Timers; **despawn old targets** (2281-2283); `Bot.Promote()` for every seat, where the old bots are destroyed (the
    new ones were already in the map so the instance always has a bound player, 2228-2229).
23. **Clearing creatures** (2290-2308): on the first build (or a party follow's new instance) of an INSTANCEABLE map:
    `SpawnArea::ClearMap(lead, 300)` for Seek, Sight, Combat, Roles; `ClearMap(lead, 1000)` for the party follow;
    `ClearMap(lead, 600)` for Interact; any other arena (a wing instance among them) `SpawnArea::Clear(lead)` (around
    the spawn).
24. `env.MapId`, `env.InstanceId` set; `env.Bots` rebuilt (one entry per seat, empty for inactive ones, plus one empty
    owner slot when `_castOwner`); `env.Targets.clear()` (2310-2320).
25. **Build encounters** (2326-2358): up to `SPAWN_ATTEMPTS = 4`: `Build(env, map, level)` on each active encounter in
    build order; on a false return and with two or more spawn points, draw another point (`(Spawn + 1 + urand(0, n-2))
    % n`, never the failed one), teleport every active bot there (`BotFactory::TeleportWithinMap`) and try again.
    All attempts failed: error log with the last position, return false.
26. **Facing and controller** (2366-2377): each active seat's `Facing` = bot orientation and `StartMover`; likewise the
    owner slot when `_castOwner` and it holds a layout.
27. `StockSeats`, `GivePets`, `DrawStandIn(env)` (2379-2383); return true.

Contracts and hazards:
- All seats of an env share one `level` (the loop at 2233 passes it to every `BuildSeat`).
- A failed first attempt of `Build` may have partly built the world (creatures, groups); the retry calls no rollback and
  `Encounter.h` has no `Unbuild`. UNVERIFIED: each encounter's `Build` must be idempotent or clean up (encounter docs).
- Failure restoration does not restore `Want`, `DungeonRole`, `Apt`, `Stable`, `PetAtStart`, `EpisodesPlayed` or the
  `ResetEpisode` totals, nor `data.Arena`/`StandInPlay`; the episode is marked `BuildFailed` and the next `Rebuild`
  overwrites them, so UNVERIFIED consequence only if something reads them in between.
- `env.FindMap()->GetId() != EpisodeMapId(env)` (2177) is false when `FindMap()` is null.
- `map->GetId()` (2297) and `map->Instanceable()` (2298) dereference `map`, non-null only when a bot was created into it
  or an old map was found; with zero active seats `map` could be null. UNVERIFIED reachable.

Config read (via `_tuning`): `Party.ClassicChance`, `Party.RoleTankChance`,
`Party.RoleHealerChance`, `Characters.KeepCasting`, `Characters.ReuseEpisodes`, `Characters.HighLevelChance/First`,
`LowLevelChance/Last`; stage fields `FocusChance`, `FocusLevelFirst`, `FocusLevelLast`, `Level`, `MinLevel`; arena
fields
`PartySize`, `ProperParty`, `DrillRole`, `Instance`, `MapId`, `Seats`. Full key table:
[cpp-tuning-keys.md](cpp-tuning-keys.md).

Tests: no unit test drives `Rebuild`. `DungeonStagesTest.cpp` and `LiveLayoutPinTest.cpp` check the stage definitions
that feed it.

### Observed issues (Rebuild)

- `StageScenario.cpp:2230, 2276-2277`: `firstNew` is assigned and never read (dead variable).
- `StageScenario.cpp:2169-2170`: garbled comment ("Never in an evaluation ..., never on never a seat that was empty").
- `StageScenario.cpp:2006`: `arena.Seats == SeatPlan::Party` is repeated inside the `if (arena.Seats ==
  SeatPlan::Party)`
  that starts at 1998; redundant.
- `StageScenario.cpp:2176`: `Arena(env).Instance == InstanceLadder::Wing` is read for every arena, named
  `freshInstance`; it relies on the default `Instance` of a non-instance arena not being `Wing`.
- `StageScenario.cpp:2298-2307`: the `ClearMap` choice is a hand-kept list of `Opposition` values; a new encounter on an
  instance falls to `SpawnArea::Clear` (around the spawn) unless added. A wing (real dungeon) arena reaches
  `SpawnArea::Clear` on the env's first build: UNVERIFIED whether that clears real dungeon packs (principle 13 wants
  them kept); check `SpawnArea.cpp` and `InstanceEncounter::Build`.
- `StageScenario.cpp:2340`: with fewer than two spawn points there is no retry at another point, only repeats of the
  same build (up to 4).
- Retries multiply: `Setup` tries `Rebuild` up to 8 times, each with 4 encounter-build attempts.

## S1.13 Seats: `BuildSeat`, `ReuseSeat`, `Configure`, `PrepareFighter`, `StockSeats`, `GivePets` (`StageScenario.cpp:2387-2569`)

- `BuildSeat(env, seat, map, level, start)` (2404-2471): the race is drawn from the layout's `Assets->Races`,
  restricted to the episode's team when `EpisodeTeam` is set (an empty pool falls back to all races); gender random;
  spec defaulted to 0 if out of range; `DamageScale(level)`; bot name `Forge<env.Id>s<seat><a|b>` (letter = session
  parity from `seat.Bot.NextSession()`), account `BotAccounts::Seat(env.Id, seat, session)`, the episode's
  `DungeonDifficulty` / `RaidDifficulty`; `seat.Bot.CreateNext(spec, map, EpisodeMapId(env), start)`; on success
  `EpisodesPlayed = 0`; for a continent or any non-instanceable map `SetPhaseMask(EnvPhase(env), true)`;
  `UpdatePositionData()` (outdoor flag; the comment records the stage-10 gryphon failure); `InitTalentForLevel()` (death
  knight talent points); `Configure`. Returns the bot or null.
- `ReuseSeat(env, seat, start)` (2473-2502): the "kept character": `CombatStopWithPets`, `ClearInCombat`, resurrect at
  full health if dead, `RemovePet`, `RemoveArenaAuras`, `RemoveAllSpellCooldown`, `ResetAllPowers`, dismount,
  `BotFactory::TeleportWithinMap(bot, start)`; null if it cannot be teleported. On success `Bot.Abort()` (nothing was
  created), `EpisodesPlayed++`, `_reused++`. Bags and gear stay; supplies are topped up in `StockSeats`.
- `Configure(bot, seat)` (2504-2527): `TalentPlan = RandomTalentPlan`, `noise = max(1, Characters.TalentNoisePoints)`,
  `SeatCharacter::Configure(bot, layout, spec, plan, urand(1, noise))`; stores `Build`, `UnspentTalentPoints`,
  `EquippedItems`; resolves `KnownRanks` for every catalog `Spell` action with `ActionCatalog::KnownRank` (the comment
  says each ask walked the rank chain three times per action per decision); finally `Apt = Aptitude::Of(...)`. A reused
  seat is not reconfigured, so `KnownRanks`, `Apt` and `Build` stay from the original build, right as long as nothing in
  an episode changes spellbook or gear (the assertion at 2515-2517).
- `PrepareFighter(bot, seat)` (2529-2544): `seat.Stable = SeatCharacter::PrepareFighter(bot, layout, apt)`; a
  `DUNGEON_TANK` seat casts its tank mode on itself (`TankModeSpell`, or Dire Bear Form 9634 for a druid that knows it)
  when it knows the spell and lacks the aura. Called by `CombatEncounter.cpp:224`, `RolesEncounter.cpp:236`,
  `InstanceEncounter.cpp:683` and `BuildOwnerSeat`; not by `Rebuild`.
- `StockSeats(env)` (2546-2569): per active seat with a layout, `ConsumablePool::Supplies(level, hasMana, isWarlock,
  warlockInParty)` and `StockBattleSupplies(bot, supplies, spec stat profile)`; a party arena with a warlock gives all
  seats the healthstone flag.
- `GivePets(env)` (2387-2402): per active seat with a pet class and `roll_chance_i(Characters.PetOutChance)`:
  `SeatCharacter::GivePet(bot, seat.Stable)`; `PetAtStart` cleared first and set from the result.

Config keys: `Characters.TalentNoisePoints`, `NoisyTalentChance`, `RandomTalentChance`, `PetOutChance`,
`ReuseEpisodes`, `KeepCasting`.

### Observed issues

- `StageScenario.cpp:2571-2579`: the doc comment of `AcceptResurrections` cites "stage 4", "druid_dps" and
  "druid_heal": removed v1 stages and roles.
- A reused seat keeps `PetAtStart` and `Stable` from before, but `GivePets` recomputes `PetAtStart`; the reused seat's
  `Stable` is not refreshed (PrepareFighter is called only by encounters), UNVERIFIED consequence for hunters.

## S1.14 `AcceptResurrections` (`StageScenario.cpp:2580-2630`)

Called each decision from `ApplyActions` (`StageScenario.cpp:2812`). It walks `players[MAX_SEATS + 1]`, filled only for
`seat < ActiveSeats` (2584-2586): (1) a player alive with an accepted offer pending (`ResurrectMs[slot] != 0`) credits
the offerer (`StepRevivedAlly = true`, `Revives++` when `ResurrectBy[slot] < ActiveSeats`), clears the offer and the
core's request data; (2) a dead player whose accepted offer is within `RESURRECT_RETRY_MS` (5 s) of episode time waits;
(3) a dead player with `isResurrectRequested()` accepts: `ResurrectBy[slot]` = the last seat whose GUID made the request
(`NO_SEAT` if none), `ResurrectMs[slot] = max(1, EpisodeElapsedMs)` (0 means "no offer in flight"),
`ResurectUsingRequestData()` (2620-2629). The doc comment (2571-2579) records the exploit this guards: a single landed
Rebirth standing its target up every decision.

### Observed issues

- The array has an owner slot (`MAX_SEATS`, matching the `MAX_SEATS + 1` sizes in `StageState.h:666-671`) but only seats
  below `ActiveSeats` are filled, and the owner's agent index is `_seatCount`, never below `ActiveSeats`; so the owner
  (the party follow's leader) is never polled for a resurrection offer, and the owner slot of `ResurrectBy/Ms` is dead.
  UNVERIFIED whether the leader's death is handled elsewhere (`PartyFollowEncounter`, `EntranceRespawn`).

## S1.15 Reviewer notes for the whole of S1

1. `StageScenario` is a 5,192-line class with about 80 members and many responsibilities (build, observe, act, reward,
   goals, intent pricing, logging, cluster, stage.json). A split must keep: the order of `AddEpisodeInfo` calls (column
   order), the `RewardTerm` order and the text patterns `sim_metrics.py` parses (its header, lines 14-20).
2. `Data(env)` is unchecked: `_data.size() == settings.Envs` is assumed.
3. Anything added to `SeatState` needs a decision in `ResetEpisode` (S1.2).
4. The arena keys are read in the constructor with `sConfigMgr->GetOption(..., false)`, outside
   `CurriculumTuning`, so they are not in its `Visit` list and probably not in the conf.dist agreement test
   (UNVERIFIED; see [cpp-tuning-keys.md](cpp-tuning-keys.md)).
5. `stage.json` `format` does not identify the schema (S1.7).
6. Lines 1-2631 have no unit test of their own. Covering tests: `LiveLayoutPinTest.cpp` and `DungeonStagesTest.cpp`
   (stage definitions and layout constants), `StandingTest.cpp` (`CourseKink`), and the Python
   `test_metric_names.py` / `test_stage_json_diff.py`, which read what the constructor and `WriteStageFiles` produce.


# Part C2. StageScenario.cpp 2570-5192

## S2.0 Scope, line map and how a decision flows through this half

This fragment documents `src/server/game/Animus/Scenario/Curriculum/StageScenario.cpp` lines 2570-5192 (the file ends
at 5192). Lines 1-2570 (construction, `stage.json`, the draw of arenas and layouts, `Reset`, seat building) are "S1" in
the assembled document. `AcceptResurrections` starts (with its comment) at 2570, a few lines before the nominal
split, and is included here. Paths below are relative to `src/server/game/Animus/Scenario/Curriculum/` unless they
start with `src/`.

Nothing in this half reads `stage.json`, builds an episode or draws an arena. It is the **per-decision and per-tick
runtime**: apply the learner's actions, tick the player controller, build each seat's observation, judge presses, pay
rewards, write the critic state and the episode-info row. The evaluation modes, the cluster rung followers, the
stand-in draw and the wing-ladder host stepping are NOT in this half; the only traces here are `PinEvaluationArena`
(4622), the stand-in column of `AgentPresence` (3475) and the
shaping/cost scale setters (4637-4651). See S1 and `cpp-encounters.md` (InstanceEncounter, WingLadder) for the rest.

| Lines | Unit | Role |
|---|---|---|
| 2570-2630 | `AcceptResurrections` | accepts pending resurrection offers for the seats and credits the reviver |
| 2632-2644 | `DeadForGood` | whether a seat is dead with nothing left to wait for (three movement encounters ask) |
| 2646-2687 | `ApplyGoals` | installs the learner's chosen goals into each seat's two `GoalHold` slots |
| 2689-2714 | `ApplyLook` | applies the camera-look heads to each seat's free-look state |
| 2716-2726 | `CameraRenderSize` | the render size of a seat's camera this episode |
| 2728-2796 | `GoalHeld` | whether this decision's play matched the primary goal (the `goal_match` columns) |
| 2798-2819 | `ApplyActions` | per-decision entry: encounter upkeep, resurrections, each seat's action |
| 2821-2830 | `StartMover` | start a seat's player controller on the server's body |
| 2832-2882 | `SubTick` | every world tick: step each seat's player controller |
| 2884-2986 | `MayLog`, `WatchFall` | capped diagnostics for burial, voids, terrain, falls |
| 2988-3045 | `TrackController`, `CourseKink` | controller columns (wall, stuck, jumps, falls, kinks) |
| 3047-3108 | `SeatTarget`, `CurrentTarget`, `DecisionTarget` | which unit a seat's actions aim at |
| 3110-3246 | `ViewSeat` | assemble a `SeatView` from the seat's state and the encounters |
| 3248-3256 | `TrackTarget` | remember where the seat last saw its target |
| 3258-3416 | `ApplySeatAction` | one seat's action: encode, apply, water/breath, press accounting, tallies |
| 3418-3463 | `Observe` | all seats' observations plus the owner row plus the critic state |
| 3465-3530 | `AgentLayouts`, `AgentPresence`, `AgentKinematics` | per-agent wire metadata |
| 3532-3717 | `ObserveSeat` | one seat's observation row, goal bookkeeping, mask |
| 3719-3761 | `Paced`, `Press` | action pacing and repeat accounting |
| 3763-3941 | `GoalGap`, `GoalPotential`, `GoalValue`, `ObserveGoalSignals` | goal geometry and pay |
| 3943-3971 | `AimlessCauseName` | names of the 21 aimless causes |
| 3973-4007 | anonymous `AimlessPrice` | cause to `Actions.Aimless.*` price |
| 4009-4025 | `IsPartyTank`, `PartyHasLivingTank` | dungeon-role helpers for the taunt rule |
| 4027-4419 | `JudgePress` | per-press intent verdicts (serves, neutral, aimless) |
| 4421-4443 | `LogDeath` | one capped log line per seat death |
| 4445-4588 | `SettleIntent` | step verdicts, jitter, fidget, needless move, and the prices of the noise terms |
| 4622-4651 | `PinEvaluationArena`, `SetShapingScale`, `SetCostScale` | learner-driven knobs |
| 4653-4700 | `Reward` | per-decision entry for rewards |
| 4702-4906 | `TrackSeatStep`, `TrackHazards`, `TrackMotion`, `TrackInterruptibleCast`, `TrackSupport`, `GroupHealer`, `GroupTank` | per-step trackers |
| 4908-5071 | `SeatReward` | everything a seat is paid or charged each decision |
| 5073-5159 | `WriteState` | the critic state (global, seats, enemies) |
| 5161-5173 | `EpisodeInfo` | writes each seat's episode-info row |
| 5175-5192 | `Teardown` | removes what the env holds at shutdown |

### The order of one decision

`EnvPool` drives these methods (`src/server/game/Animus/Env/EnvPool.cpp`):

1. `ApplyActionsForMap` (EnvPool.cpp:324-350), on the map thread that owns the env, per env: `ApplyGoals`, then
   `ApplyLook` (each only when the learner sent the array: `Goals`/`Look` non-empty, 340-345), then `ApplyActions`
   (347).
2. The world ticks. Each tick `EnvPool::SubTickMap` (EnvPool.cpp:354-362) calls `SubTick` for the env, on its map
   thread.
3. When a decision is due `EnvPool::ObserveEnv` (EnvPool.cpp:241-293) runs: `Reward` (248), then folds each agent's
   `StepStats` into `EpisodeStats` and resets it (250-254), then `IsTerminal` (258), then `Observe` into the `Final*`
   buffers (episode done: no mask, 269) or into the next `Obs` buffers with mask (279), then `DescribeAgents`, which
   calls `AgentLayouts`, `AgentPresence`, `AgentKinematics` (EnvPool.cpp:474-476).
4. When the episode ended `FinishEnv` calls `EpisodeInfo` (EnvPool.cpp:292) and then the reset (S1).

So an observation always follows the reward step of the same decision. The goal reached at the observation is paid into
the reward row of the decision just rewarded (S2.8, `StepReward`).

Threading: calls above run on the env's map thread (or the world thread on the non-map path, `onMapThread`). The only
cross-thread state in this half is the atomics `_shapingScale`, `_costScale`, `_evaluationArena`
and the `_logged` atomic counters. The learner's scalars arrive on the wire thread (`AnimusForge.cpp:2630-2775`).

## S2.1 Resurrection offers: `AcceptResurrections`, `DeadForGood` (2570-2644)

**What.** Each decision (called from `ApplyActions`, 2814) the sim accepts, for each dead player, a resurrection another
seat offered, and credits the offering seat once the target is actually alive. The comment at 2570-2578 records why the
credit is paid on landing: a delayed teleport can postpone the resurrect, and the earlier version paid for offers that
never landed (measured 38.4 revives an episode in the deleted stage 4).

**Code.** `players[0..ActiveSeats)` are filled from `SeatBot` (2586); the array has `MAX_SEATS + 1` entries, the last
never filled. For a living player with an outstanding offer (`ResurrectMs[slot] != 0`) the reviver `ResurrectBy[slot]`
gets `StepRevivedAlly = true` and `++Revives` (2603-2604), the offer is zeroed and `clearResurrectRequestData()` is
called (2608). For a dead player: skip while an accepted offer is within `RESURRECT_RETRY_MS` = 5000
(StageScenario.cpp:129) of its acceptance (2614); skip if no request is pending; find the requesting seat with
`isResurrectRequestedBy` (2622; if several seats offered, the highest index wins); store `ResurrectBy`; store
`ResurrectMs = max(1, EpisodeElapsedMs)` so an offer on millisecond 0 still counts; call `ResurectUsingRequestData()`
(2628).

**Contracts.** `StepRevivedAlly` is consumed by `PartyEncounter::Reward` (`Encounters/PartyEncounter.cpp:268-271`),
which pays `RewardTerm::Revive` (Shaping) = `Resurrection.ReviveAlly` (default 1.5) and clears the flag. `Revives` feeds
the `revives` info column (`PartyEncounter.cpp:50`). So revive credit exists only in arenas that use the party
encounter.

**`DeadForGood(env, seat)`** (2632-2644): false when `Arena(env).RespawnAtEntrance` (2635); otherwise true when the
seat's `Combat.Died` is set, its bot is not alive, and either it has no `PLAYER_SELF_RES_SPELL` or
`EpisodeElapsedMs >= Combat.DeathMs + Resurrection.GraceMs` (default 20000 ms, `CurriculumTuning.h:831`). Callers:
`SightEncounter.cpp:445`, `SeekEncounter.cpp:571`, `InteractEncounter.cpp:707`, all for seat 0 only. In every arena with
`RespawnAtEntrance` it is false; in the movement stages that do not respawn (see `cpp-stagescenario.md` stage list) a
dead seat 0 ends the episode through it.

**Config.** `Resurrection.GraceMs`, `Resurrection.ReviveAlly` (cpp-tuning-keys.md).

**Tests.** None found: `grep` of `src/test` for `AcceptResurrections` and `ResurrectMs` returns nothing.

**Reviewer notes.** The extra `players` slot (2584-2586) is never filled, so a revive offered by a non-seat (the old
owner) is never accepted or credited. The comment block above describes stages 4 and druid classes of the deleted
curriculum.

## S2.2 Goals: `ApplyGoals`, `GoalHeld` and the goal bookkeeping in `ObserveSeat`

**Wire shape.** The learner sends `GOAL_SLOTS_ON_WIRE` ints a seat (primary, secondary, 2653-2656). `GoalHold` is
`StageState.h:72-92`; `GOAL_SLOTS = 2`. A goal id packs kind and target (`GoalKindOf`, `GoalTargetOf`, `MakeGoal`; see
cpp-blocks.md).

**`ApplyGoals`** (2646-2687). Out-of-range ids become `NO_GOAL`; a secondary equal to the primary is dropped
(2656-2658). Per slot: if the goal changed while one was in progress (old and new both not `NO_GOAL`) then
`++GoalChanges`, and when the old goal had not ended `++StepGoalSwitches`, charged as `Goals.Switch` in `SeatReward`
(2670-2672); a goal that ended (reached, or no longer possible) is replaced free. Any change resets the hold to a fresh
`GoalHold` with `Fresh = true` and counts `GoalsChosenBy[kind]` (2679-2683). Only seats `< _seatCount` are touched; the
cast owner has no goals.

**`GoalHeld`** (2728-2796): per kind of the primary (`Holds[0]`), did this decision's play match? Fight: damage dealt
this step; Control: some other living enemy is crowd-controlled; Recover and Rest: self healing this step or a regen
aura; Protect: ally healing or protection given (`AllyHealing`, `AgentHealingBy`, `AllyProtectionBy`,
`AgentProtectionBy`); Position: within melee range for a melee-range seat, else between `Duel.MeleeRange` and desired
range plus `GOAL_RANGE_SLACK_YARDS` (5.0, StageScenario.cpp:114); Prepare: not in combat and `StepPreparationMs > 0` or
stealthed; TravelTo, Gather, Interact: within `GoalBlock::PLACE_REACH` of the goal's place, or a non-finalized
movespline, or casting, or a loot window open (2784-2786); Loot: loot window or movespline (2788); Resurrect:
`StepRevivedAlly` or casting (2790). The result is counted into `GoalMatches[kind]` in `SeatReward` (5004-5010),
which gives the `goal_match*` info columns (S1, StageScenario.cpp:1214-1237).

**Reviewer notes and quirks.**
- `GoalHeld` reads `bot->movespline->Finalized()` (2786, 2788); `AgentKinematics` (3510-3511) and `SettleIntent` (4484,
  4509) read `movespline` too. Principle 3 (bots move only through the controller) means a seat's spline is never
  initialised, so these clauses are only true for server-originated motion (fear, knockback). UNVERIFIED: whether any
  live stage relies on the spline clause.
- (Fixed 2026-10-08: `SeatGoal::Loot`, Gather, Interact and `Goals.WorldValue` were removed.)
- In party arenas `PartyEncounter::Reward` (reached at `SeatReward` 4993) clears `StepRevivedAlly` before `GoalHeld`
  reads it at 5009, so a Resurrect goal never matches there. UNVERIFIED by test; derived from the call order.

## S2.3 Camera look and render size (2689-2726)

`ApplyLook` returns at once unless the stage has `BlockId::Vision` (2691). For each seat with a layout it calls
`Vision::FreeLook::Apply(seat.Look, look + agent * HEADS, Vision::Current())` (2705), which returns a body turn; a
nonzero turn is written to `seat.Controls.Held.FaceTurn` (2708), so the facing changes through the player controller
at the next tick's start, not by a direct set. The cast owner's row is applied when `CastOwnerActive` (2712). Looking is
free: nothing prices or tallies it (comment 2700-2703). Caller: `EnvPool.cpp:345`.

`CameraRenderSize` returns `{0,0}` without a vision block or for an empty seat; otherwise the seat's `Look.Render`
resolution. Callers: `AnimusForge.cpp:1516, 1651`. Tests: `VisionFreeLookTest.cpp`, `VisionTest.cpp` cover free look and
render sizes, not these wrappers.

## S2.4 Per-tick controller stepping and its diagnostics (2821-3045)

**`StartMover`** (2821-2830): builds a `Movement::PlayerLink`, a `MapWorldQuery`, `ShapeOf(bot)`;
`seat.Mover.Start(...)`; `seat.Facing = Mover.Body.Yaw`. Static; also called from `EntranceRespawn.cpp:50` after a rise
and from S1 (2370, 2376).

**`SubTick`** (2832-2882), once per world tick per env, after the decision's presses on a decision tick. For each seat
`< _seatCount` with a layout and a live in-world bot: start the mover if not started; drain the session's
`MovementOrders()` inbox into a `thread_local std::vector<Client::Order>` (2855-2856) and give each order to
`Mover.Order` (what the server ordered the client: root, knockback, speed change); `above` = terrain surface below and
body not in terrain, taken before `Mover.Tick(seat.Controls.Held, SpeedsOf(bot), shape, world, diffMs, nowMs, link)`
(2865), and `intoTerrain` = was above and is now inside; `seat.Facing = Mover.Body.Yaw`; `TrackController`;
`WatchFall`. The owner slot is ticked too when `CastOwnerActive` or when the arena is PartyFollow and the encounter
`HasLeader` (2875-2877): the party follow's leader moves through the same controller from the encounter's scripted keys.
The wall time of the whole loop goes to `Movement::ControllerCost` (2879). `nowMs` is `env.EpisodeElapsedMs`.
UNVERIFIED: whether `EpisodeElapsedMs` advances per world tick or per decision (it decides whether `StuckMs` and
`WatchFall` timing have tick resolution); check `EnvPool`/`AnimusForge` tick code. The `decided` argument is unused.

**`MayLog`** (2892-2896): a diagnostic line is allowed while the atomic `_logged[kind][layout]` (`fetch_add`, relaxed;
`LOG_KINDS = 5`, `LOG_LAYOUTS = 64`, `StageScenario.h:441-443`) is below the cap; `layout` is the seat's `Layout::Index`
clamped to 63. Kinds: `LOG_DEATH 0, LOG_VOID_FALL 1, LOG_BURIED 2, LOG_INTO_TERRAIN 3, LOG_OVER_VOID 4` (2884-2890). The
counters are per scenario object and never reset: caps are per process and stage, not per episode.

**`WatchFall`** (2898-2986), const but mutates `seat`:
- records when the body enters `Mode::Falling` (`FallStartMs`, `FallStartZ`, `FallFrom`);
- once per episode per seat each: "put inside the ground" (`Counts.Unburied` grew since `MoverAtStart`, 2913; cap 8),
  "kept from stepping over nothing" (`Counts.OverVoid`, 2926; cap 8); `intoTerrain` (2938) is counted on every
  occurrence in `IntoTerrain` and logged once per seat (cap 8);
- a seat falling at least `VOID_FALL_MS` = 3000 ms (2954-2955) with no floor within 2000 yd writes one long `LOG_WARN`
  (cap 4) naming auras, speeds, and the client's counters, once per episode (`VoidFallLogged`).

**`TrackController`** (2988-3023), after each tick: with a movement key held, `WallMs += diffMs` if the tick hit a wall;
`stuck` = keys held and `TickCommanded > 0.01` and `TickMoved < 0.1 * TickCommanded`; a stuck run counts toward
`StuckMs`
only once it has lasted 1000 ms (3001-3002: the whole run is added the moment it crosses 1 s, then each tick);
`Jumps += TickJumps`; a landing from >= 2.0 yd increments `Drops` and `Falls` together (3006-3009); `FallDamage`,
`FallDeaths`, `VoidDeaths` are drained from `seat.Link` and zeroed; `CourseKink` increments `CourseKinks`.

**`CourseKink`** (3025-3045, static and public for the test): true when the heading from the last tick's position to
this
one differs from the previous tick's heading by more than 0.3490659 rad (20 degrees) while moving at >= 0.5 yd/s over
the tick (3035). State is `CourseX/Y`, `HasCoursePos`, `HasCourse`, `LastCourse`. Test: `StandingTest.cpp:66-78`.

**Info columns fed** (declared in S1, StageScenario.cpp:788-796, 1085-1094): `wall_seconds`, `stuck_seconds`,
`course_kinks` (per minute), `jumps`, `drops`, `fall_damage`, `fall_deaths`, `void_deaths`, `into_terrain`.

**Reviewer notes.** `SubTick` builds a fresh `MapWorldQuery` per seat per tick (2848). UNVERIFIED whether that is cheap.
The `thread_local` vector is one allocation per map thread.

## S2.5 Target selection (3047-3108, 3248-3256)

- `SeatTarget` (3047): the unit `CurrentTargetGuid` names, resolved through `Encoding::UnitThrough(*bot, guid)`; else
  target slot 0. Const readers (episode info, state) use it.
- `CurrentTarget` (3058): in a stage with `BlockId::Sight` the target is the client's selection, `bot->GetTarget()`
  (3067), never an encounter's choice. Otherwise the first active encounter whose `SelectTarget` answers true wins
  (3071); none: `env.FindTargetUnit(0)`. A target that is not in the world or is being removed (3082), or on another
  map than the seat's, is dropped to null. The comment cites a fault in a stage of the deleted first curriculum; the
  guard stays.
- `DecisionTarget` (3090): caches the answer for the decision (`DecisionTargetKnown`, `DecisionTarget` guid,
  3093-3097) so observation and action agree; the cache is invalidated in `ApplyActions` after the encounters' upkeep
  (2808-2810) and in `Reward` after the world ticked (4684-4687). When cached, the guid is re-resolved and re-validated
  (3104) each call.
- `TrackTarget` (3248): if the target is visible (`CanSeeOrDetect`), remember its guid, position and time
  (`LastSeenGuid`, `LastSeen`, `LastSeenMs`), used for the hidden-target fields of the view.

## S2.6 `ViewSeat` (3110-3246)

Builds a `SeatView` (`Layout/SeatView.h`, see cpp-layout-character.md) from `SeatState`, tuning and the encounters.
Non-obvious points:
- vision pointers (`Look`, `Seen`) only with the vision block (3136-3139); sight pointers (`Recall`, `RecallKept`,
  `SightGuids`, `Focus`) only with the sight block (3141-3147);
- `Body` is the controller's body only once the mover started (3149);
- `EpisodeTime = min(1, EpisodeElapsedMs / EPISODE_TIME_SCALE_MS)` with 300000 ms (`Layout/Block.h:212`, used at 3160);
  `CombatTime` uses `MAX_COMBAT_TIME_MS` 60000 (104, used at 3161); `TargetUnseenTime` uses `MAX_UNSEEN_TIME_MS` 20000
  (105);
- `SelfResurrectAllowed = true` always (3164);
- **Enemies.** In a sight stage the enemy list is only what the seat's last frame showed: `CombatBlock::VisibleEnemies`
  into `view.Enemies` (`PACK_SLOTS`), and only for a living, in-world bot (3171-3185); `TargetInView`, `TargetSeen`,
  `LastSeen`, `TargetUnseenTime` come from the frame and the entity memory `Recall` (3187-3197). In other stages the
  enemies are `env.Targets` in slot order up to `PACK_SLOTS`. A dead seat in a sight stage sees no enemies;
- party frames: with `BlockId::PartyFrames`, `MinimapYards = PartyFollow.MinimapYards` (default 60,
  `CurriculumTuning.h:547`) and `PartyFramesBlock::FillFromGroup(view)` (3209-3212). An encounter with no core group
  (PartyFollow) fills frames in its own `View`;
- every active encounter then gets `View(env, seat, view)`;
- `ObjectivePlaceKnown = GoalBlock::ObjectivePlaceKnown(_stage.Has(Compass), view.CompassWithheld)` (3220): a goal place
  for a trip's objective exists only with a compass that is not withheld;
- **what a player could not know** (3225 onward): for a living bot, a target or enemy it cannot `CanSeeOrDetect` is
  removed from the view (`HiddenTarget` keeps the unit); this is the server-truth stealth and line-of-sight filter,
  applied after the encounters filled the view. The critic's state (`WriteState`) keeps everything.

## S2.7 `ApplySeatAction` (3258-3416)

Per seat per decision (from `ApplyActions` 2814-2819, for each seat and the cast owner when active).
1. Return if no bot or no layout. `seat.Pressed = action` (3263).
2. `DecisionTarget`; with no target and a layout that cannot act without one (`SeatEncoder::ActsWithoutTarget`, 3269) it
   returns without applying anything.
3. `TrackTarget`; a paced action becomes 0 (3275; the mask normally removes it, this protects against a policy that
   ignores the mask).
4. `ViewSeat`; in a sight stage `HazardsSeen = true` and the ground fire is what the camera shows
   (`CombatBlock::ReadHazards(seat.Seen, x, y, orientation)`), else `NearestHazard` from `TrackHazards`.
5. `SeatEncoder::Apply(view, action, result)` (3294) fills a `SeatActionResult`; then `seat.Facing = view.Facing`.
6. **Water and breath** (3299-3345): `WaterMs` while swimming, `AquaticMs` in aquatic form, `WaterWalkMs` on a
   water-walk
   liquid, `SubmergedMs` under water. Under water without a water-breathing aura `BreathSpentMs += decisionMs` and
   `DrowningDamage += LastStepSelfDamage`; above water breath returns ten times faster (3334). `BreathMs()` is
   `max(1000, sWorld CONFIG_WATER_BREATH_TIMER)` (StageScenario.cpp:121-124, the core's `WaterBreath.Timer`, not an
   AnimusForge key). A death while submerged with self damage or an empty breath sets `Drowned` (3341).
7. If `action > 0 && !result.KeyStillHeld`: `Press(...)` then `JudgePress(...)` (3347-3348). A held movement control
   pressed again is not a press (MoveBlock).
8. Durative options: `OptionMs += decisionMs` when any option slot is running (3366); `OptionPresses` for each slot
   whose
   kind changed during this action.
9. Copies the per-action result counters into `SeatState` (3375-3400: `HealsOnFull`, `DefensiveCasts`, `HealingCasts`,
   `HealingPowerSpent`, `DownrankedCasts`, `SpellCasts`, `BreathingCasts`, `ControlChanges`, `TurnReversals`,
   `BearingFlips`, `PitchReversals`, `Weaves`, `StepJitter += JitterWeight` (3384), `TrinketUses`, `ItemUses`,
   `ConsumablesUsed`, `SelfResurrections`, `PetAbilities`, `PetOrders`, `PetOrderCounts`) and the combat tally
   (`PreparationMs`, `StealthOpeners`, new `StealthUtilityTargets` once per target per stealth, cleared when stealth
   ends,
   3409).
10. Every active encounter's `OnSeatAction` (3412); then the hunter's Call Pet (3414).

`_tuning.Options` is copied into the view (3154). `StepJitter` accumulated here is charged in `SeatReward` (4999).

## S2.8 Observation: `Observe`, `ObserveSeat`, agent metadata (3418-3717)

**`Observe`** (3418-3463). For each seat `< _seatCount`: `ObserveSeat(env, seat, obs + seat*ObsDim, mask row, image row,
map row)`. Then `Data(env).StepReward = nullptr` (3436: the pointer is only valid during the seat calls). The owner row
(`_castOwner`): a seat observation when `CastOwnerActive`, else zeros, a no-frame image (`Vision::FillNoFrame`, 3451),
zero map cells and a mask that allows only action 0. Finally `WriteState`.

**`AgentLayouts`** (3465): per agent the seat layout's `Index` (0 for none), owner slot included.
**`AgentPresence`** (3475): `StandIn::Presence(hasCharacter, standInSeat == seat)`: 0 absent, 1 learner, 2 stand-in
(`Encounters/StandIn.h:56-62`; protocol 25); the owner slot is 1 only when `CastOwnerActive`. Test: `StandInTest.cpp`.
**`AgentKinematics`** (3489): per agent the body for the learner's kinematic prediction heads: x, y, z, yaw, pitch from
the controller body, `K::ModeOf(jumping, inWater, false)` (3520), mounted, speed, in-combat; `K::Write(seconds, body,
out)` (3528). `jumping` comes from `bot->movespline` (3510-3511), which is never active for a controller-moved seat, so
the mode never reports a jump or fall. The comment at 3507-3509 says no seat flies or mounts and the first curriculum's
mounts and flight were deleted, yet `Mounted` (3521) and `MOVE_FLIGHT` (3524) are still read.

**`ObserveSeat`** (3532-3717). First it zeroes `obs`, sets the image to no-frame, the map cells to 0 and the mask to
`{1, 0, ...}` (3534-3546), so an empty seat still sends a legal row. Then, for a seat with a layout:
1. `DecisionTarget`; note combat start (`CombatStartMs`); `TrackTarget`; `TrackMotion`.
2. Action memory: re-size if `NumActions` differs; `Memory.Observe(bot, target, now)`.
3. **Mental map and entity memory at the episode's first look** (`MapPending`, 3572-3591), in stages with `Map` or
   `Sight`: kept (aged by `MapAgeOffset`) only if `MapKeep` (the reset's roll), the same map and instance, and a
   non-empty map (3576-3577); else cleared. `Recall` follows the same roll and offset.
4. `ViewSeat`; in a sight stage the camera-read hazards; with the map block `Hits`, `Map`, `MapRow`, `MapKept` (3610).
5. **Goals** (about 3615-3690), for each hold that has not ended: `GoalBlock::Status(view, goal, reached, possible)`;
   `reached = GoalBlock::Earned(reached, Fresh, SatisfiedAtChoice)` (a goal already true when chosen is held unpaid
   until it
   stops being true); Protect is also reached by keeping the named friend at or above 50% health with an attacker on it
   for `Goals.ProtectHoldMs` (default 5000; 3637-3638). A reached, not yet rewarded goal is paid
   `GoalReached = GoalValue(hold, bot) * (slot ? Goals.SecondaryShare : 1)` (0 for a group healer's Fight goal) with
   `Rewards.AddTaken` (3651), and the paid amount (shaping scale included) is added to `Data(env).StepReward[seat]`
   (3652-3653). A goal no longer possible counts `GoalsLost`. `hold.Ended = reached || !possible` (3657).
6. Per hold: `HasPlace/Place` via `GoalBlock::PlaceOf`; the named friend; the first-observation potential
   (`GoalPotential`; `ChoiceResource = min(health, mana fraction)`, 0.5 if dead; 3678-3683). An ended secondary is
   cleared.
7. `ObserveGoalSignals` (3691) sets `Event` and `Achieved` (the learner's hindsight labels); `SeatEncoder::Observe(view,
   obs, mask)` writes the row; the time spent building the view is added to `SeatEncoder::AddObserve(OBSERVE_VIEW)`.
8. With an image row and `view.HasObjective`: count the objective-flagged pixels (`Vision::CountObjectivePixels`, 3702)
   and mark the first sighting (`ObjectiveSighted`, `ObjectiveSightMs`): the Seek stage's measures.
9. Finally every `Paced` action is cleared from the mask (3715).

## S2.9 Pacing, repeats and the press: `Paced`, `Press` (3719-3761)

`Paced` asks `seat.Memory.Paced(layout, action, now, _tuning.Actions)`. `Press` (3724): actions outside any block
return; `++ActionsPressed`; `Memory.Press(...)` with the seat's `KnownRanks`; `PendingRepeat = false` (3743); press
times
older than `Actions.RepeatWindowMs` (default 10000) are erased and the new one pushed; if the count in the window is
at most `Actions.RepeatFree` (3, line 3750) nothing more happens. Otherwise, when the press did something or the action
is a movement action it sets `PendingRepeat` (settled by the verdict in `JudgePress` or `SettleIntent`); else the repeat
is charged at once (`StepRepeats`, `RepeatedPresses`).

## S2.10 Goal geometry and pay (3763-3941, 4009-4025)

- `GoalGap(seat, bot, target)` (3763): the minimum over both holds of the yards still to go to where a goal wants the
  seat; -1 means none (no goal, wrong kind, dead). TravelTo, Gather, Interact with a place: `max(0, dist2d(place) -
  PLACE_REACH)`. Fight and Position: a melee-range seat wants `IsWithinMeleeRange`; a ranged seat wants between
  `Duel.MeleeRange` (default 3.5) and `CombatReward::DesiredRange(seat, Duel) + 5`.
- `GoalPotential` (3799) for `Goals.Progress`: Fight is `-health` of the named enemy, or the mean of all enemies when it
  names none; Control is -1 while the named enemy lives uncontrolled; Recover and Rest `min(health, mana) - 1`; Protect
  `friend health - 1`; Position `-min(gap, 60)/60`; TravelTo `-min(dist, 60)/60`; Resurrect -1 while the friend is dead.
  A dead seat: -1 for Resurrect (no target), Recover and Rest; else 0.
- `GoalValue` (3874): Fight `Goals.FightValue` 0.3, Control `ControlValue` 0.2, Protect `ProtectValue` 1.0, TravelTo
  `TravelValue` 0.1, Recover/Rest `RecoverValue` 1.0 x (resource now - resource
  at
  choice), others `Goals.Reached` 0.05.
- `ObserveGoalSignals` (3902): `Achieved` is the first named enemy slot that was alive last time and is dead now
  (`MakeGoal(Fight, ENEMY_FIRST + slot)`), else Recover when the seat was below 0.8 of min(health, mana) and now is not
  and out of combat. `Event` is true when health newly falls below `EVENT_HEALTH_PCT` 35 (3936) or the number of enemies
  in combat rises above the previous count.
- `IsPartyTank` (4009): `DungeonRole == DUNGEON_TANK`, or `DUNGEON_ANY` with aptitude `HoldsThePull().MetBy(Apt)`.
  `PartyHasLivingTank` (4015): another active seat, alive, with a layout, that is a party tank.

All `Goals.*` defaults are in `CurriculumTuning.h:281-323`; keys in cpp-tuning-keys.md.

## S2.11 Intent verdicts: `JudgePress` and `SettleIntent` (4027-4443, 4445-4588)

**Purpose.** Principle 5: presses are judged and priced, never masked. `JudgePress` runs for every non-zero action that
was not a held key (3347-3348); `SettleIntent` runs once per decision from `SeatReward`.

**`JudgePress`** (4027-4419), in order:
1. Effort: `StepEffort += result.EffortWeight`, `++EffortPresses`, `++CombatPresses` in combat (4037-4040).
2. A refused core cast the seat could have prevented (`Encoding::SituationalFailure`, 4049: facing, range, sight,
   moving,
   power): one aimless press of cause `CastFacing`, `CastRange`, `CastSight`, `CastMoving` or `CastPower`, then return
   (4047-4068). Only for the Core block and `!result.SpellCasts && result.RefusedCast`.
3. A sight-block press the world refused (`result.ActRefused` in `1..REFUSALS-1`, 4072): `ActRefusedBy[kind]` and an
   aimless `ActRefused`, then return.
4. With no primary goal (4089): a pending repeat is charged unless the press did something or was a move block press;
   return.
5. A **move block** press (4118-4130): a step (`MoveControls::IsStep`) outside hazards records `MoveGap = GoalGap(...)`;
   a
   repeated step waits for the settle; a repeated non-steer press with no gap is charged at once. Return.
6. Otherwise the `judgeFor(hold)` lambda (4134) returns `Judgement{judged, verdict, cause}` for one goal:
   - **Spell cast** (not a revive): a trap is aimless `TrapNoEnemy` unless an enemy within 30 yd hurts a friend, or the
     named enemy is within 15 under Control or Fight (4162); neutral for interrupts, breath, a defensive cast while hurt
     (< 50%), a stealth opener; **taunt or tank mode by a non-tank beside a living tank** is aimless `TauntOffRole` or
     `TankModeOffRole` (4186); harmful casts (4195) are judged per goal (Fight: serves on the focus or an area spell
     reaching it; Control: serves when tactical on another enemy; Position: neutral only while out of range and on the
     focus, else aimless; Prepare: aimless unless in combat; Protect: serves on an attacker of the friend; Recover,
     Rest,
     TravelTo, Resurrect: aimless when nothing attacks the seat); non-harmful casts by goal
     (Protect serves on the named or any friend; Recover and Rest serve on self or untargeted; Prepare serves with
     preparation time; Fight and Control: aimless "help on another" unless self, untargeted, hurt, or a healing cast;
     Resurrect serves on a revive of the named friend). An aimless verdict with no cause becomes `HelpOffGoal`,
     `AoeMissed`, `OffFocus`, `InRangeCast` or `UnprovokedHarm` (4288-4297).
   - **Food or drink** (4299): aimless `ConsumeNotNeeded` when the resource is at or above `Actions.ConsumeFullPct`
     (85);
     else serves Recover, Prepare, Rest, neutral otherwise.
   - **Pack-block selection** of slot `local < PACK_SLOTS` (4317): serves the named enemy slot; neutral if the goal
     names
     none, or the chosen enemy hurts a friend, or the seat is below `ESCAPE_HEALTH_PCT` 35 (4099); else aimless
     `TargetSwitch`.
   - **Pet attack order** (4335): same shape; aimless `PetOffGoal`.
7. The seat's verdict is the primary's, unless the primary did not serve and the secondary judged and serves (or is
   neutral where the primary was aimless) (4355-4363).
8. Supplies spent are counted for `SupplySpent` (4366). A spell that changed a mode group (`layout.ModeGroups[action]`,
   4374) is a standing choice: the same situation (combat bit, mana band under 30, under 80, or above; mounted) as at
   the
   last change makes it aimless `ModeFlip`, or `ModeReverse` within 10 s; always `++StepModeSwitches`, `++ModeSwitches`.
9. A pending repeat is charged unless the press served (4394). Judged presses count `JudgedPresses` (4399),
   `PurposefulMs`
   (not aimless, 4401), `ServingPresses`, and for aimless `StepAimless`, `AimlessPresses`, `StepAimlessBy[cause]`,
   `AimlessBy[cause]`.

**`SettleIntent`** (4445-4588):
- A step pressed this decision (4450): compares `GoalGap` now with `MoveGap`; closer by more than
  `Actions.IntentSlackYards` (0.5) serves, farther by more than that is aimless `StepAway` (4456-4459); a repeated step
  that did not serve is a repeat.
- Moving = the controller body's speed squared above 0.25, or a non-finalized spline (4481-4484). Each start counts
  `MoveStarts`; a restart within 1 s of the stop counts `MoveStopStarts`; each restart adds
  `MovePrice::Recency(since, Options.JitterDecayMs)` (default 2500) to `StepJitter` (4492-4498).
- **Fidget** (4510-4536): moving in a fight, goal gap exactly 0, target not moving, not getting behind it (only seats
  whose catalog has a spell with `SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET`, cached in `FromBehind`), not in hazards,
  held
  for `Actions.SettleGraceMs` (500): `StepFidgetMs`, `FidgetMs`.
- **Needless move** (4543-4565): a ranged spec moving in a fight, target alive and still, 30 yd or nearer (hunters 8 yd
  or more), in line of sight, nothing in melee on the seat, not in hazards, held `SettleGraceMs`: aimless
  `NeedlessMove`.
- **Prices charged here** (4571-4584): `Aimless` = sum over causes of `AimlessPrice(cause) * StepAimlessBy[cause]` +
  `Actions.Aimless` for uncaused + `Actions.ModeSwitch * StepModeSwitches`; `Effort` = `-Actions.Effort * StepEffort -
  Actions.SupplySpent * StepSuppliesSpent`; `Fidget` = `-Actions.Fidget * StepFidgetMs / 1000`.

**Reward terms** (kinds from `Rewards/RewardLedger.h`): `Aimless`, `Effort`, `Fidget`, `Repeat`, `Jitter` are **Cost**
and noise prices (`PricesNoise`), paid times the learner's cost scale; the episode score takes them at full price. The
five cast causes share one price, `Actions.Aimless.CastFailed` (`AimlessPrice`).

**Defaults** (`CurriculumTuning.h`): `Actions.Repeat` 0.03, `RepeatWindowMs` 10000, `RepeatFree` 3, `Jitter` 0.05,
`Aimless` 0.02, each `Aimless.<cause>` 0.02 except `TargetSwitch` 0.04, `PetOffGoal` 0.04, `ConsumeNotNeeded` 0.03,
`TrapNoEnemy` 0.03, `ModeFlip` 0.03, `ModeReverse` 0.06, `TauntOffRole` 0.15, `TankModeOffRole` 0.04; `ModeSwitch`
0.01, `SupplySpent` 0.02, `ConsumeFullPct` 85, `Effort` 0.004, `Fidget` 0.01, `SettleGraceMs` 500, `IntentSlackYards`
0.5.

**Info columns fed** (S1, StageScenario.cpp:985-1100): `serving_share` (served / judged), `aimless_presses`,
`aimless_<cause>` for 21 causes (`aimless_act_refused` only with a sight block), `act_refused_<reason>` (sight stages),
`mode_switches`, `effort_presses`, `move_starts_per_minute`, `fidget_seconds`, `repeated_presses`, `bearing_flips`,
`pitch_reversals`, `weaves`, `actions_per_minute`.

**`AimlessCauseName`** (3943): `off_focus, aoe_missed, in_range_cast, unprovoked_harm, help_off_goal, step_away,
target_switch, pet_off_goal, consume_not_needed, trap_no_enemy, mode_flip, mode_reverse, needless_move, taunt_off_role,
tank_mode_off_role, cast_facing, cast_range, cast_sight, cast_moving, cast_power, act_refused`. Test:
`SightBlockTest.cpp:418` (`act_refused` only).

**`LogDeath`** (4421-4443): once per seat death (`DeathLogged`), capped by `MayLog(LOG_DEATH, 8)` (4426), one
`LOG_INFO "Seat died: ..."` line. Called from `SeatReward` so a death an episode outlives (a respawn arena) is seen.

**Reviewer notes.**
- The 35% literal exists three times: `ESCAPE_HEALTH_PCT` (4099, local to `JudgePress`), `EVENT_HEALTH_PCT` (3936,
  local)
  and `LOW_HEALTH_PCT` (StageScenario.cpp:115). A comment (4098) says CoreBlock's goal escape uses the same number; that
  is
  a separate literal in Blocks. UNVERIFIED that they agree.
- Position-goal casts are neutral only while out of range (the comment explains: a 99% Position drill had a 0.05
  serving share); a ranged seat that stands in range and fires while holding Position is charged `InRangeCast`.
- Comments carry incident notes from the deleted first curriculum ("stage6", "stage1_duel", "companion stage"). They
  explain the rules but no longer name live stages.
- The `judgeFor` lambda is about 220 lines, captures everything by reference, and has no unit test.

## S2.12 Evaluation pin, shaping and cost scales (4590-4651)

- **`PinEvaluationArena(pin)`** (4622): `pin == 0` means the stage's own arenas; otherwise `pin - 1` must index an arena
  with `EvalOnly` (the held-out arena, 4624), else `LOG_ERROR` and `false`, which makes the learner's MODE refused
  (`AnimusForge.cpp:2764-2765`). Stored in the atomic `_evaluationArena` (4630), read by S1's draw. Only an evaluation
  passes a pin (`mode.Mode == 1 ? mode.Arena : 0`). Logs on change.
- **`SetShapingScale` / `SetCostScale`** (4637, 4645): atomic stores of the learner's fade-ladder and cost-ladder
  rungs. The clamp to [0,1] (NaN to 1) is done by the caller, with one warning each (`AnimusForge.cpp:2637-2662`).
  Applied to every seat's ledger at the start of `Reward` (4678-4682), so a change reaches every env at its next
  decision. Logs on change.

Tests: none for these setters. `WingLadderTest.cpp` and `DungeonStagesTest.cpp` test the ladder and rung tables.

## S2.13 `Reward` and `SeatReward` (4653-4700, 4908-5071)

**`Reward(env, reward)`** (4653-4700):
1. On the episode's last decision (`EpisodeElapsedMs >= EpisodeLengthMs`) each seat's controller `Finish`es (4662),
   crediting the stretch since its last report. Episodes ended by an outcome end on the server's reading.
2. `Data(env).StepReward = reward` (4666): the pointer a goal reached at the next observation pays into.
3. `BeforeRewards` on every encounter in **reward order** (`ActiveRewardOrder`).
4. `StepEngaged` = any target alive and in combat (read by the combat clock).
5. Sets the shaping and cost scales on every seat's ledger and clears `DecisionTargetKnown` (4678-4687).
6. `reward[seat] = SeatReward(env, seat)` for each seat (4690).
7. The owner row's reward is 0; its bookkeeping (`TrackSeatStep`) still runs when active (4693-4699). The owner is never
   paid.

**`SeatReward`** (4908-5071), per seat; an empty seat returns 0. In order (kinds from `RewardLedger.h`):

| Order | Term | Kind | Condition / formula | Key(s), default |
|---|---|---|---|---|
| 1 | `DamageDealt` scale (4920) | scale, not a payment | `Rewards.Scale(DamageDealt, ...)`: 0 for a group healer, `Party.TankDamageShare` for a group tank, else 1; "group" = the arena has a `PartyGroup` and the spec's `Stats` is Healer or Tank (4896-4906) | `Party.TankDamageShare` 0.25 |
| 2 | `Hazard` (4943, 4951) | Shaping | standing in a hazard: `-min(Hazards.Standing * seconds, room)`; hazard damage: `-min(Hazards.Damage * damage / maxHealth, room)`; `room = max(0, Hazards.Max + episode Hazard sum)` | `Hazards.Standing` 0.15, `Damage` 0.5, `Max` 3.0 |
| 3 | `CombatClock` (4989) | Shaping | `-Output.Clock * decisionMs / 1000` while any target is alive and engaged; skipped when `Output.Clock <= 0` | `Output.Clock` 0.01 |
| 4 | the encounters' terms (4993) | per encounter | `Encounter::Reward` in reward order (cpp-encounters.md) | per encounter |
| 5 | `Repeat` (4995) | Cost, noise | `-Actions.Repeat * StepRepeats` | `Actions.Repeat` 0.03 |
| 6 | `Aimless`, `Effort`, `Fidget` (4998 via `SettleIntent`) | Cost, noise | S2.11 | `Actions.*` |
| 7 | `Jitter` (4999) | Cost, noise | `-Actions.Jitter * StepJitter`, after `SettleIntent` so a stop-then-start is in the same step | `Actions.Jitter` 0.05 |
| 8 | `GoalProgress` (5028) | Shaping | per live, unended hold with a read potential: `Goals.Progress * (slot ? SecondaryShare : 1) * (ProgressGamma * potential - hold.Potential)`; 0 for a group healer's Fight goal | `Goals.Progress` 0.5, `ProgressGamma` 0.999, `SecondaryShare` 0.5 |
| 9 | `GoalSwitch` (5033, 5036) | Shaping | `-Goals.Secondary` every decision a secondary is held; `-Goals.Switch * StepGoalSwitches` | `Goals.Secondary` 0.002, `Goals.Switch` 0.15 |
| 10 | `SelfHealing` (5048) | Shaping | `Support.SelfHealing * (step.SelfHealing + step.SelfProtection) / maxHealth` | `Support.SelfHealing` 0.5 |
| 11 | `HealingMana` (5055) | Shaping | `-Support.HealingMana * StepHealingPowerSpent / maxMana` for mana users with mana spent and weight > 0 | `Support.HealingMana` 0.1 |
| at the observation | `GoalReached` (3651) | Shaping | `GoalValue * (secondary ? SecondaryShare : 1)`, once per goal, by `AddTaken` into the previous reward row (S2.8) | `Goals.*Value`, `Goals.Reached` |

Other work in `SeatReward`: `TrackSeatStep` (damage scaled by `DamageScale`, `LastStepDamage`, `LastStepDamageTaken`,
`LastStepSelfDamage`, `CurrentTargetGuid`, `TrackSupport`); `TrackInterruptibleCast` and `TrackHazards` for a live bot;
pet bookkeeping (`PetDied`, `LastPetHealth`, `PetOutMs`, `PetAttackingMs`, `PetPassiveMs`, `PetStayingMs`, default
stance); `Combat.DeathCounted = false` once alive again; `LogDeath`; goal decision counts (`GoalDecisions[kind]`,
`GoalTargetedDecisions`, `GoalMatches[kind]` via `GoalHeld`, 5004-5010); `LastStepPowerDelta` and `LastPower`. It
returns
`seat.Rewards.TakeStep()` (5070).

**Contracts.** Shaping terms are multiplied by the learner's fade scale and noise prices by the cost scale; the
episode's
`Score()` is at full price and leaves out tier (`RewardLedger.h:269-312`). Terms are summed per episode into the
`reward_<name>` columns.

**Reviewer notes.**
- The `Hazard` cap compares an unscaled price with `Episode(Hazard)`, which already has the shaping scale applied
  (`RewardLedger::Add` multiplies by `Shaped(term)`). At fade scale `s` the effective cap is `Hazards.Max / s`. Harmless
  at
  0 and 1.
- `HealingMana` is described as a price but is Shaping, so it fades away; likewise `Hazard`, `SelfHealing` and all three
  goal terms. With the fade at 0 the goal block's pay is gone and the goal head learns from `Achieved` and the
  encounters' outcomes alone. UNVERIFIED whether that is intended (principle 9 says a stage's purpose is Outcome).

## S2.14 Per-step trackers (4702-4906)

- `TrackSeatStep` (4702): see S2.13.
- `TrackHazards` (4722): `Encoding::TrackNearestHazard(bot, self, facing, now, seat.NearestHazard, HazardSearchMs)` from
  the controller's body position; the grid search is throttled by `Encoding::HAZARD_SEARCH_MS` (comment 4716-4718).
- `TrackMotion` (4736): `MoveRate` and `CloseRate` over about 1 s (`MARK_MS` 1000, 4739), in units of `RUN_SPEED` 7 yd/s
  (4740): `MoveRate = distance travelled / (seconds * 7)` (4779), `CloseRate = (old range - new range) / (seconds * 7)`
  to the target in 2D (4781). Reset when dead. Seek and Interact encounters' `View` replaces `CloseRate` with the rate
  toward the objective.
- `TrackInterruptibleCast` (4790): counts an enemy cast the seat could have interrupted (target alive within
  `INTERRUPTIBLE_CAST_RANGE` 30 yd, 4793, a cast in progress), once per distinct (caster, spell):
  `InterruptibleCastsSeen`, the `interruptible_casts_seen` column (S1:1206).
- `TrackSupport` (4818): the seat's own absorb auras (`SPELL_AURA_SCHOOL_ABSORB` cast by this bot) on itself and every
  other seat in the env, compared with the last decision's list: an absorb that lost amount (and was not re-cast)
  credits the lost amount; one that vanished with more than `decisionMs + ABSORB_EXPIRY_SLACK_MS` (500) left (4869,
  4887), its owner alive and amount > 0, credits the whole remainder. Credits go to `StepStats.SelfProtection` or
  `AgentProtectionBy[agent]`. Also accrues `LowHealthMs` while any friend, itself included, is under `LOW_HEALTH_PCT`
  35% (4893). The `friendRef.Ally >= 0` branch (4847) is never taken: every friend is built with `Ally = -1`
  (4838, 4841); the comment above the struct ("itself, the owner (ally 0) and the other seats") is stale.
- `GroupHealer` / `GroupTank` (4896, 4902): arena with a `PartyGroup` and the seat's spec's `StatProfile`.

## S2.15 Critic state, episode info and teardown (5073-5192)

**`WriteState`** (5073-5159): zeroes `StateDim`; `state[STATE_EPISODE_TIME] = elapsed / length`; one-hot arena at
`STATE_ARENA_FIRST + Arena` (< `MAX_ARENAS`); every active encounter's `WriteState` (the encounter-owned global columns
`STATE_PULL_*`, `STATE_OWNER_*`, `STATE_TIER`, `StageScenario.h:62-78`); per seat 26 features (present, alive, health,
mana, other power, level / 80, the six-number aptitude brief, class one-hot, in combat, casting, x and y relative to the
spawn point); per enemy slot up to `PACK_SLOTS`, `STATE_ENEMY_FEATURES = 28 + RAID_GROUPS` features (present, alive,
health, x, y, casting, elite, level difference to seat 0 over 5 (5115), in combat, whose victim it is as seat index,
group one-hot and aptitude brief, max health ratio, armor reduction, damage modifier, run speed 5156, opponent-type
one-hot). This is privileged state (the critic may see it; the policy may not, principle 1).

Quirks: the owner globals are written only by `PartyFollowEncounter::WriteState`. `data.Seats[0].Level` is read without a guard (5115). A null `bots[0]` skips max health and armor.

**`EpisodeInfo`** (5161-5173): `_info.Write(env, seat, info + seat * EpisodeInfoDim)` for each seat; the owner row is
zero,
so its `present` is 0 and no per-seat metric sees it. The columns are `_info`'s, declared in S1.

**`Teardown`** (5175-5192): despawns the env's targets; calls each encounter's `Teardown` **in reverse reward order**
(the party disbands before its owner leaves); destroys the seats' bots; releases the owner seat if `_castOwner`; clears
`env.Bots` and `env.Targets`. Called once at shutdown (`EnvPool.cpp:136`).

## S2.16 Config keys read in this half

Defaults and ranges are in cpp-tuning-keys.md. The groups read here are `Resurrection.*`, `Goals.*`, `Actions.*`
(including `Actions.Aimless.*`), `Options.JitterDecayMs`, `Hazards.*`, `Support.*`, `Output.Clock`,
`Party.TankDamageShare`, `Duel.MeleeRange`, `PartyFollow.MinimapYards`. In code they are read through `_tuning` (the
scenario's `CurriculumTuning`, S1), at the lines cited in the sections above. The core's own `WaterBreath.Timer` is read
at
StageScenario.cpp:123. The `Vision::Current()`, `Vision::MapCurrent()` and `Vision::MemoryCurrent()` settings are read
at
2692, 3580 and 3585 (see cpp-vision.md).

## S2.17 Tests that touch this half

- `src/test/server/game/Animus/StandingTest.cpp:66-78`: `StageScenario::CourseKink`.
- `src/test/server/game/Animus/StandInTest.cpp`: `StandIn::Presence` and `Fields` (used by `AgentPresence`).
- `src/test/server/game/Animus/SightBlockTest.cpp:418`: `AimlessCauseName(ActRefused)`.
- `src/test/server/game/Animus/RewardLedgerTest.cpp`: ledger semantics `Reward` relies on (UNVERIFIED which cases).
- `src/test/server/game/Animus/GoalObjectiveLeakTest.cpp`: the goal block's reached/possible with the compass withheld
  (the `ObjectivePlaceKnown` call at 3220).
- `LiveLayoutPinTest.cpp` and `DungeonStagesTest.cpp` pin live layouts and rung tables.
- No unit test builds a `SeatState` or `Env` to drive `JudgePress`, `SettleIntent`, `ObserveSeat`, `SeatReward`,
  `AcceptResurrections`, `WatchFall`, `TrackSupport` or `WriteState`; they need a live `Player` and `Map`.

## S2.18 Observed issues (this half)

1. `StageScenario.cpp:379-381` (S1 edge): the comment says "The leader is no dead code: ..."; it means "not dead code".
   The code at 382 is right.
2. `StageScenario.cpp:4847-4851`: `TrackSupport`'s `Ally >= 0` branch (`AllyProtectionBy`) is dead; every friend has
   `Ally = -1` (4838, 4841); the comment at 4828-4829 mentions an owner friend that is not built. `AllyProtectionBy` is
   still read by `GoalHeld` (2760) and written only in `EnvPool.cpp:662`.
3. `StageScenario.cpp:2584-2586`: `players` has an extra owner slot that is never filled, so revives by a non-seat are
   never accepted.
4. `StageScenario.cpp:3008-3009`: `Falls` and `Drops` are incremented together. Redundant. UNVERIFIED which column reads
   `Falls`.
5. `StageScenario.cpp:4942-4951`: the Hazard cap mixes unscaled price with a shaping-scaled episode sum (effective cap
   `Hazards.Max / scale`).
6. `StageScenario.cpp:2786, 2788, 3510-3511, 4484, 4509`: `movespline` reads for seats that only move through the
   controller; `AgentKinematics` therefore never reports a jump or fall mode, though the comment at 3507-3509 claims
   mounts and flight were removed while `IsMounted` (3521) and `MOVE_FLIGHT` (3524) remain.
7. `StageScenario.cpp:3164`: `SelfResurrectAllowed` is unconditionally true.
8. `StageScenario.cpp:3936, 4099, 115`: three separate 35% constants.
9. `StageScenario.cpp:4993 vs 5009`: `PartyEncounter::Reward` clears `StepRevivedAlly` before `GoalHeld` reads it, so a
   Resurrect goal never matches in party arenas (derived from call order, no test).
10. `StageScenario.cpp:3749-3750` region (end of `ObserveSeat`, 3716): a stray blank line before the closing brace.
11. `StageScenario.cpp:2892-2896`: the `_logged` caps are never reset; a long process stops logging deaths and voids
    after
    8 lines per layout, across stage runs started in the same process.
12. Size: `JudgePress` is about 390 lines with a 220-line lambda; `SeatReward` (about 165) and `ObserveSeat` (about 185)
    are
    long too; all untested.
14. (Fixed 2026-10-08: the seven unwritten global columns and `STATE_ENEMY_ON_OWNER` were removed, 1958 -> 1927.)
15. (Fixed 2026-10-08: the loot, gather and interact goals were removed.)

## S2.19 Questions for the owner

- Should `Hazard`, `HealingMana`, `SelfHealing` and the three goal terms stay Shaping (they vanish with the fade) or
  become Cost or Outcome (principle 9)?
- Are the `movespline` reads in `AgentKinematics`, `GoalHeld` and `SettleIntent` meant to see only server-driven motion?
  If controller falls should read as jumping, the kinematic mode needs the controller's `Body.Kind`.
- (Done 2026-10-08.) Loot goals and `Goals.WorldValue` were deleted.
- Is `Falls` meant to differ from `Drops`?
