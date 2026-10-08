# Reference: encounters

Purpose and scope. Everything under `src/server/game/Animus/Scenario/Curriculum/Encounters/`: the `Encounter` hook
interface, and every live encounter (Sight, Seek, Interact, PartyFollow, Combat, Roles, Instance with the wing ladder,
the core Party group, the stand-in, EntranceRespawn, Standing, SeenPlaces, Opponents, DifficultyLadder). For each:
episode lifecycle, draws and placement, rewards paid (name, kind, formula, key), episode-info columns, config keys,
tests, quirks and reviewer notes. Stages and the scenario are in [cpp-stagescenario.md](cpp-stagescenario.md); the
tuning keys in [cpp-tuning-keys.md](cpp-tuning-keys.md); blocks, layouts and rewards in
[cpp-blocks.md](cpp-blocks.md), [cpp-layout-character.md](cpp-layout-character.md) and
[cpp-rewards-routing.md](cpp-rewards-routing.md); metrics in [metrics.md](metrics.md); the whole picture in
[00-architecture.md](00-architecture.md). State of the tree: commit `bd32b9dc8`. The sections come from several passes
and are prefixed E1 to E4 in their headings (E1 Sight, Seek, Interact and ObjectPool; E2a PartyFollow, Combat,
DifficultyLadder and Opponents; E2b Roles, Party, StandIn and Standing; E3 and E4 Instance, bosses, wing ladder,
respawn and seen places); section 1 below is the interface. Each part ends with its own Observed issues and
UNVERIFIED items; [known-issues.md](known-issues.md) aggregates them.

## Map

| Path (under `src/server/game/Animus/Scenario/`) | Lines | Role |
|---|---|---|
| `Curriculum/Encounters/CombatDraw.h` | 148 | Pure draws for the combat stages: pull plans, creature levels, rung tables. |
| `Curriculum/Encounters/CombatEncounter.cpp` | 707 | CombatEncounter implementation (C1-C3: creatures on a cleared Ragefire Chasm). |
| `Curriculum/Encounters/CombatEncounter.h` | 183 | CombatEncounter class and per-env state. |
| `Curriculum/Encounters/DifficultyLadder.cpp` | 104 | Per-class and role difficulty rung ladder (implementation). |
| `Curriculum/Encounters/DifficultyLadder.h` | 100 | Per-class and role difficulty rung ladder (declaration). |
| `Curriculum/Encounters/Encounter.h` | 109 | The Encounter base class (hook interface). |
| `Curriculum/Encounters/Encounters.h` | 470 | Declarations of PartyEncounter and InstanceEncounter, EnemyRank and RankEnemy. |
| `Curriculum/Encounters/EntranceRespawn.cpp` | 58 | Respawn clock, wipe latch and rise-at-entrance (implementation). |
| `Curriculum/Encounters/EntranceRespawn.h` | 147 | RespawnClock, WipeLatch, RiseAtEntrance declarations (I4). |
| `Curriculum/Encounters/EpisodeInfoTable.h` | 71 | Name-plus-getter table of per-seat episode statistics. |
| `Curriculum/Encounters/InstanceBosses.cpp` | 116 | Boss table rows for the wing ladder and the follow bosses; row validation at startup. |
| `Curriculum/Encounters/InstanceBosses.h` | 62 | BossRow and the ladder-row accessors. |
| `Curriculum/Encounters/InstanceEncounter.cpp` | 2752 | InstanceEncounter: real dungeon runs on the wing ladder (corridor, drill, full clear). |
| `Curriculum/Encounters/InteractDraw.h` | 251 | Pure draws for M3: sites, spots, decoys, rungs. |
| `Curriculum/Encounters/InteractEncounter.cpp` | 720 | InteractEncounter implementation (M3). |
| `Curriculum/Encounters/InteractEncounter.h` | 162 | InteractEncounter class and state. |
| `Curriculum/Encounters/ObjectPool.cpp` | 66 | Spawning/removing the seek pool's objects and clearing a dungeon's own gameobjects. |
| `Curriculum/Encounters/ObjectPool.h` | 51 | ObjectPool declarations. |
| `Curriculum/Encounters/Opponents.cpp` | 387 | OpponentPool: creature choice, spawn points, summoning at a level. |
| `Curriculum/Encounters/Opponents.h` | 98 | OpponentPool declarations. |
| `Curriculum/Encounters/PartyEncounter.cpp` | 541 | PartyEncounter: the core group and role pay. |
| `Curriculum/Encounters/PartyFollowEncounter.cpp` | 738 | PartyFollowEncounter implementation (M4, scripted leader). |
| `Curriculum/Encounters/PartyFollowEncounter.h` | 182 | PartyFollowEncounter class and state. |
| `Curriculum/Encounters/RolesDraw.h` | 239 | Pure draws and role constants for G1 drills. |
| `Curriculum/Encounters/RolesEncounter.cpp` | 727 | RolesEncounter implementation (G1). |
| `Curriculum/Encounters/RolesEncounter.h` | 154 | RolesEncounter class and state. |
| `Curriculum/Encounters/SeekDraw.h` | 323 | Pure draws for M2: rungs, rooms, spots, evaluation picks. |
| `Curriculum/Encounters/SeekEncounter.cpp` | 579 | SeekEncounter implementation (M2). |
| `Curriculum/Encounters/SeekEncounter.h` | 130 | SeekEncounter class and state. |
| `Curriculum/Encounters/SeenPlaces.h` | 248 | What a seat discovered: goal places, seen-only or with layout nodes. |
| `Curriculum/Encounters/SightDraw.h` | 218 | Pure draws for M1: spots, pairs, rungs. |
| `Curriculum/Encounters/SightEncounter.cpp` | 453 | SightEncounter implementation (M1). |
| `Curriculum/Encounters/SightEncounter.h` | 130 | SightEncounter class and state. |
| `Curriculum/Encounters/StandIn.h` | 161 | The 'human' stand-in partner: draw and shares (I7). |
| `Curriculum/Encounters/StandInSeat.cpp` | 169 | Stand-in seat setup. |
| `Curriculum/Encounters/Standing.h` | 69 | Pure movement readings: WallCharge, Stopped, Band. |
| `Curriculum/Encounters/WingLadder.cpp` | 111 | Whole-dungeon difficulty ladder: steps, collapse alarm (implementation). |
| `Curriculum/Encounters/WingLadder.h` | 98 | Whole-dungeon difficulty ladder declarations. |
| `Curriculum/Encounters/WingRun.h` | 175 | Pure bookkeeping of a dungeon run (corridor start, cleared packs, tier of a rung). |

## 1. The encounter interface (`Encounters/Encounter.h`, `EpisodeInfoTable.h`)

### 1.1 What an encounter is

`class Encounter` (`Encounter.h:44-107`, 109 lines) is the unit `StageScenario` composes an episode from: "one part of
what a stage's envs contain besides the seats". It owns per-env state sized at construction (`Encounters.h` header
comment, and each encounter's `std::vector<Env...> _envs`-style member), a reference to the owning `StageScenario`
(`_scenario`, `Encounter.h:104`), is non-copyable, and has a virtual destructor. Every hook has an empty default, so
an encounter overrides only what it needs. The scenario creates one object of each kind any arena of the stage asks for
and shares it between all of the stage's envs; the env is passed in every call (`Env&`, `Env const&`).

| Hook | Signature | Called from (`StageScenario.cpp`) | Contract |
|---|---|---|---|
| `RewardTerms` | `std::vector<RewardTerm> RewardTerms() const` | constructor `:531-552` | the terms the encounter pays; each becomes a `reward_<name>` episode-info column named `"reward_" + RewardTermName(term)` |
| `AddEpisodeInfo` | `void (EpisodeInfoTable&)` | constructor `:527-528` | once; adds the encounter's columns, in `_rewardOrder` |
| `ResetEpisode` | `void (Env&)` | `:1948-1949`, on **every** encounter of the stage (not only the arena's) | clear episode totals |
| `Deactivate` | `void (Env&)` (default calls `Teardown`) | `:1951-1954`, for encounters of the previous episode's arena that this arena does not use | remove what it keeps in the world (a bot, a group) before seats are rebuilt; may be built again later |
| `BeforeRebuild` | `void (Env&)` | `:1956-1957`, active encounters | before the seats' old bots are replaced |
| `BeforeLevel` | `void (Env&)` | `:2119-2120` | after the arena and the seats' classes are drawn, before the level: write level, map or spawn into the `EnvState` (an instance's boss rung) |
| `Build` | `bool (Env&, Map*, uint8 level)` | `:2330-2334` | after the seats' new bots are placed, in encounter (construction) order; false = the env cannot be built |
| `UpdateEnemies` | `void (Env&)` | `ApplyActions`, `:2803` | each decision before the actions: enemies joining fights |
| `Update` | `void (Env&)` | `ApplyActions`, `:2805` | each decision, everything else |
| `SelectTarget` | `bool (Env const&, uint32 seat, Unit*&)` | `:3071` | what a seat's actions aim at when the encounter decides it; **never reached in a stage with the sight block**, where the seat's own client selection is the target (`:3062-3068`) |
| `OnSeatAction` | `void (Env&, uint32 seat, SeatActionResult const&)` | `:3412` | after a seat's action is applied |
| `View` | `void (Env const&, uint32 seat, SeatView&) const` | `:3216` | fill the parts of the seat's view the encounter knows |
| `BeforeRewards` | `void (Env&)` | `:4668`, `ActiveRewardOrder` | once per decision before any seat is rewarded |
| `Reward` | `void (Env&, uint32 seat, Player*, RewardLedger&)` | `:4993`, `ActiveRewardOrder` | each seat's terms |
| `WriteState` | `void (Env const&, float*) const` | `:5087` | the encounter's part of the critic state (buffer pre-zeroed) |
| `IsTerminal` | `bool (Env const&) const` | `:1844`, any of the active encounters | the episode ended; the scenario also reports terminal when `BuildFailed` (`:1838`) |
| `Teardown` | `void (Env&)` | `:5182`, reverse reward order | once at shutdown ("the party disbands before its owner leaves") |

The header comment says "each episode it calls the hooks of the encounters the episode's arena uses, in a fixed order";
exactly: `ResetEpisode` goes to all encounters, `Deactivate` to the previous arena's unused ones, and everything else to
the arena's (`ActiveEncounters(env)`, in construction order; `ActiveRewardOrder(env)` for `BeforeRewards` and
`Reward`). A failed `Build` is retried by drawing the spawn again, at most `SPAWN_ATTEMPTS = 4` times per
`Rebuild` (`StageScenario.cpp:2327-2340`); the setup path adds `RetryBuild` with `SETUP_BUILD_ATTEMPTS = 8`
(`BuildRetry.h:30`, used at `StageScenario.cpp:1857`).

### 1.2 Construction order and reward order

`StageScenario`'s constructor creates, if any arena asks (`StageScenario.cpp:403-462`):

1. `PartyEncounter` (any arena with `PartyGroup`), 2. `InstanceEncounter` (any `Opposition::Instance`, "after the
group: it moves it to the boss"), 3. `PartyFollowEncounter`, 4. `SeekEncounter`, 5. `SightEncounter`,
6. `InteractEncounter`, 7. `CombatEncounter`, 8. `RolesEncounter` ("after the party group, whose members it places").

That is the order of `Build`, `Update`, `View`, `OnSeatAction` calls. The order of **episode-info columns and reward
terms** is a different list, `_rewardOrder` (`:458-463`): instance, party, partyFollow, seek, sight, interact, combat,
roles, "an encounter left out of this list still runs; only its columns and terms are missed" (`:458-459`). Per arena
the scenario keeps `_arenaEncounters` and `_arenaRewardOrder` filtered by what the arena uses (`:468-487`). The comment
at `:427-428` says rewards do not depend on each other's order: what several read (a seat's damage taken) is computed
before any `Reward`.

Seven of the eight classes map one-to-one onto an `Opposition` (Instance, PartyFollow, Seek, Sight, Interact, Combat,
Roles); `PartyEncounter` is selected by `ArenaDefinition::PartyGroup`, not by an opposition, and runs beside
InstanceEncounter and RolesEncounter.

Note that `Encounters.h` (470 lines) declares only `EnemyRank`, `RankEnemy`, `PartyEncounter` and `InstanceEncounter`;
the other six encounters have their own headers. `Encounters.h` includes `<mutex>` twice (`:39`, `:45`) and a long
list of headers (Battleground, `RouteShortcut.h`, `BotSlot.h`, ...) that suggests accretion: see Observed issues.

### 1.3 `EpisodeInfoTable` (`EpisodeInfoTable.h`, 71 lines)

A vector of `(name, getter)` pairs; `Getter = std::function<float(Env const&, uint32 seat)>`. `Add(name, getter)`,
`Contains(name)` (linear), `Size()`, `Names()`, `Write(env, seat, float* info)` (calls every getter; no bounds check of
`info`). The columns are the union over all arenas of the stage, in the order added: first the scenario's own
(`AddCoreEpisodeInfo`, `AddStandInEpisodeInfo`, `StageScenario.cpp:524-525`), then each encounter's in `_rewardOrder`,
then one `reward_<term>` column per term an encounter pays (`:530-541`; a term two encounters pay gets one column,
`:534-535`), then `reward_` columns for the eleven terms the scenario pays itself in every stage (Repeat, Jitter,
Aimless, Effort, Fidget, SelfHealing, GoalReached, GoalSwitch, Hazard, HealingMana, CombatClock; `:543-552`). An
encounter an episode does not use reports 0 (the getter
must return 0 for an env whose arena does not use it). Names go to the learner in SPEC (`Scenario::EpisodeInfoNames`);
the metric names the learner yaml configs read are these names or their `_clear`-style derivations (see
[metrics.md](metrics.md) and [protocol.md](protocol.md)). The column order is therefore a wire contract per run: it is
rebuilt at each stage start, and the learner maps by name.

### 1.4 Shared infrastructure the encounters use

- `Opposition` and `ArenaDefinition` are in [cpp-stagescenario.md](cpp-stagescenario.md) Part A.
- `RewardLedger`, `RewardTerm`, `RewardTermName` and the tier scaling are documented in
  [cpp-rewards-routing.md](cpp-rewards-routing.md); layouts and blocks in
  [cpp-layout-character.md](cpp-layout-character.md) and [cpp-blocks.md](cpp-blocks.md).
- `Standing.h` (69 lines, pure functions: `WallCharge`, `Stopped`, `Band`; tested by `StandingTest.cpp`) is the
  movement encounters' shared reading of a seat's feet; it is documented with the movement encounters below.


## E1.0 Scope of this fragment

Paths are relative to `src/server/game/Animus/Scenario/Curriculum/Encounters/` unless they start with `src/` or
`apps/` or are named otherwise (`Stages/Stages.cpp` and `StageScenario.cpp` are under `Scenario/Curriculum/`). All line
numbers are those of commit `bd32b9dc8`.

| Path | Lines | Role |
|---|---|---|
| SightEncounter.h | 130 | Declaration and per-env state of the M1 (`move1_controls`) encounter. |
| SightEncounter.cpp | 453 | M1: places one real object in sight of a hallway spawn, withholds the compass by rung, pays Arrive/Progress/Facing. |
| SightDraw.h | 218 | Pure functions for M1: withholding ladder, evaluation (pair, compass) pick, in-sight and round-a-corner placement. |
| SeekEncounter.h | 130 | Declaration and per-env state of the M2 (`move2_seek`) encounter. |
| SeekEncounter.cpp | 579 | M2: hides one object by the placement ladder (hallway, doorway, room, deep), pays Arrive plus three shaping aids. |
| SeekDraw.h | 323 | Pure functions for M2: depth tiers, the placement ladder, evaluation picks, polygon sampling, "looked into a room". |
| InteractEncounter.h | 162 | Declaration and per-env state of the M3 (`move3_interact`) encounter. |
| InteractEncounter.cpp | 720 | M3: spawns the named object and decoys at a Deadmines site, judges presses, watches the door, pays Arrive/DoorOpened/WrongObject. |
| InteractDraw.h | 251 | Pure functions for M3: ladder, sites per rung, decoys, evaluation pick, `DoorWatch`, `Judge` of a press. |
| ObjectPool.h | 51 | Spawn/remove/clear helpers for the seek pool's game objects, shared by M1, M2 and M3. |
| ObjectPool.cpp | 66 | Their implementation. |

The `Encounter` interface these classes implement is in `Encounter.h` (documented elsewhere in cpp-encounters.md).
The three stages and their data tables (`StockadeHallways`, `StockadeRooms`, `SeekObjects`, `StockadeSightPairs`,
`DeadminesSites`, `Stages/Stages.cpp:91-590`; stage definitions at `Stages/Stages.cpp:648`, `:684`, `:722`) are
documented in [cpp-stagescenario.md](cpp-stagescenario.md). The tuning keys are listed in
[cpp-tuning-keys.md](cpp-tuning-keys.md); the learner side in [py-learner.md](py-learner.md); the wire in
[protocol.md](protocol.md); metrics in [metrics.md](metrics.md).

### Common machinery of the three encounters

What all three do the same way (each section below lists only what differs):

- **Lifecycle, as the scenario calls it.** `ResetEpisode(env)` clears the episode's totals but keeps the guids of
  the spawned object(s) so the next `Build` can remove them (Sight `SightEncounter.cpp:169-178`, Seek
  `SeekEncounter.cpp:205-214`, Interact `InteractEncounter.cpp:222-232`). `BeforeLevel` (Sight only) swaps the spawn
  for an evaluation pair. `Build(env, map, level)` removes last episode's object, draws, teleports the seat
  (`BotFactory::TeleportWithinMap`, which is the episode's placement, not a bot move), spawns the object(s) with
  `ObjectPool::Summon` and sets `env.EpisodeLengthMs` (Seek and Interact override the arena value; Sight keeps the
  arena's 60 s, `Stages/Stages.cpp:656`). Per decision the scenario calls `View` (observe), then `Reward`, then
  `IsTerminal`; the env pool ends the episode when `terminal || EpisodeElapsedMs >= EpisodeLengthMs`
  (`Env/EnvPool.cpp:259`). `StageScenario::IsTerminal` also ends it when a Build failed (`StageScenario.cpp:1837-1845`).
- **One seat.** All three use `_scenario.SeatBot(env, 0)` only; there is no party and no stand-in.
- **No target.** `SelectTarget` returns true with a null target (Sight `:298-303`, Seek `:418-423`; reached only in stages without the sight block: Interact's override was deleted 2026-10-08), so the seat acts without a target from the encoder's side.
- **Death.** In the first `Reward` call after the bot is dead, a `Death` Cost of `-Death` is added through
  `ledger.Add` (so it sits on the cost ladder if one is on) and the tally is marked (Sight `:343-351`, Seek
  `:461-469`, Interact `:577-585`). None of the three stages sets `RespawnAtEntrance`, so `StageScenario::DeadForGood`
  (`StageScenario.cpp:2632-2641`) is true once `tally.Died` and the bot is still dead and cannot self-resurrect (or
  `Resurrection.GraceMs` has passed), and `IsTerminal` is true then. A death ends the episode in M1-M3 (the respawn
  clock of I4 does not apply here).
- **Time-out.** `tally.TimedOut` is set in `Reward` when the clock is up and the object is not reached (Sight
  `:352-354`, Seek `:470-472`, Interact `:586-588`); the clock itself ends the episode in the env pool.
- **Noise prices.** `Stuck` and `Wall` are read from the seat's controller counts (`seat.StuckMs`, `seat.WallMs`) as
  the increment since the last decision and charged with `ledger.AddFixed`, i.e. at their own fixed price, off the
  cost ladder (Sight `:358-375`, Seek `:476-493`, Interact `:592-608`). Stuck: `-Stuck * ms/1000`. Wall: `-charge`
  with `charge = Standing::WallCharge(wallSeconds, moved, asked, Wall, WallSlide)`, where `asked` is the speed for the
  held keys (walk, run-back or run) times `DecisionMs`/1000. `Standing::WallCharge` is in `Standing.h` (documented
  elsewhere in cpp-encounters.md).
- **Step cost.** `-StepCost * _scenario.DecisionScale()` is added every `Reward` call, before the `bot` null check
  (Sight `:319`, Seek `:441`, Interact `:557`).
- **Critic state.** Each writes the object's place into the first enemy slot of the critic state (`STATE_ENEMY_*`,
  relative to the spawn) and one value into `STATE_TIER` (Sight `:426-441`, Seek `:553-567`, Interact `:689-703`).
  These are privileged inputs for the critic only; the actor never sees them (principle 1).
- **Stage info the sim writes about them.** `stage.json` `episode_categories` (`StageScenario.cpp:1419-1453`): for
  Seek `seek_room` (room names) and `seek_object` (pool kinds); for Interact `interact_site` and `interact_object`
  (pool kinds plus a last name `"lock"`); for Sight `sight_object` and `objective_corner` (`["in_sight","corner"]`).
- **Map clearing.** On an env's first build in an instance, `StageScenario` clears the map's creatures with
  `SpawnArea::ClearMap` for Seek and Sight (radius `INSTANCE_CLEAR_RADIUS`) and `INTERACT_CLEAR_RADIUS` for Interact
  (`StageScenario.cpp:2300-2308`).

## E1.1 ObjectPool (ObjectPool.h/.cpp)

**What it does.** Three free functions in `Animus::Curriculum::ObjectPool`, shared by M1, M2 and M3 so M1 does not
reach into M2's encounter (`ObjectPool.h:32-35`).

| Function | Behaviour |
|---|---|
| `GameObject* Summon(Map*, SeekObject const&, Position const& spot, uint32 phase)` | `map->LoadGrid` at the spot, then `Map::SummonGameObject(entry, spot, 0, 0, sin(f/2), cos(f/2), 0)` (respawn 0), then `SetPhaseMask(phase, true)`; null on failure (`ObjectPool.cpp:26-39`). The yaw is the spot's orientation. |
| `void Remove(Map*, ObjectGuid&)` | If the guid is non-empty and still in the map: phase 0, `EnableCollision(false)`, `Delete()` (so it leaves the dynamic tree at once); always clears the guid (`:41-54`). |
| `void ClearOwn(Map*)` | Every object in `GetGameObjectBySpawnIdStore()` that is in world and spawned is `DespawnOrUnsummon(0ms, Seconds(WEEK))` (`:56-66`). |

**Contracts.** `Remove` is safe on an empty guid and a null map. `ClearOwn` walks the spawn-id store, i.e. the map's
own database spawns. UNVERIFIED: that a `SummonGameObject` object is absent from that store (check `Map.cpp`); if it
were present, `ClearOwn` would despawn the episode's own object. It is called on every `Build` of M1 and M2, not M3
(M3 has its own `ClearWorld`, E1.4).

**Data flow.** Called on the world thread from `Build`/`Teardown` of the three encounters. Nothing crosses threads.

**Config keys.** None. **Tests.** No unit test names `ObjectPool` (a `grep` of `src/test` finds the word only in the
data tests' comments); it is exercised only through a live map.

**Reviewer notes.** `Remove` relies on `Delete()` plus the phase/collision calls to take the model out of the dynamic
tree immediately; a change in core `GameObject::Delete` would break the "placement sees nothing of last episode's
object" assumption stated at `ObjectPool.cpp:46-48`. The comment at `ObjectPool.h:33-34` says SeekEncounter does the
same in its own Place and Build; the code of SeekEncounter now calls `ObjectPool` (Seek `:220`, `:343`, `:347`), so
that comment is stale.

## E1.2 SightEncounter (M1 `move1_controls`)

Files: `SightEncounter.h/.cpp`, `SightDraw.h`. Selected by `Opposition::Sight` (`StageScenario.cpp:422`).

### What it does

The seat stands at a random point of the Stockades' hallway table facing a random way; one real object of the seek
pool stands at another table point `Controls.Nearest..Furthest` yards off, in sight of the seat's eye (or, in a share
of episodes at the later rungs, just round a corner). The seat must reach it and stop beside it as fast as it can. The
camera's objective flag marks the object when it is in line of sight; the compass block also points at it unless the
compass is withheld that episode (the withholding chance rises by rung).

### Stage definition it runs under

`Stages/Stages.cpp:648-663`: one arena `hallway` (`Against = Sight`, `EpisodeSeconds = 60`, `SpawnPoints =
StockadeHallways()` (202 points, counted from the table at `Stages.cpp:91-183`), `Objects = SeekObjects()` (5 kinds:
chest, crate, barrel, sack, strongbox, `Stages.cpp:365-373`), `SightPairs = StockadeSightPairs()` (32 pairs, 8 with
`Corner = true`, `Stages.cpp:385-`)), `Blocks = {Core, Move, Compass, Vision, Goal}`, `Level = 1`. Python config:
`apps/forge/python/configs/move1_controls.yaml`.

### Public API

```cpp
SightEncounter(StageScenario&, uint32 envs);
std::vector<RewardTerm> RewardTerms() const;      // Arrive, StepCost, Death, Progress, Facing, Stuck, Wall
static std::vector<std::string> ObjectNames(ArenaDefinition const&);   // the pool's kinds, for sight_object
static float StopGap(float distance, float bound, float body);         // max(0, distance - bound - body)
```

`SightDraw` (namespace `Animus::Curriculum::SightDraw`): `RUNGS = 4`, `FADE_SCALES = {1, 0.5, 0.25, 0}`;
`Rung(shaping)` the nearest scale (`SightDraw.h:44-51`); `WithholdChance(shaping, chances)` linear interpolation of the
four chances between the scales, clamped to [0,1] (`:55-69`); `EvaluationPick(seed, castings, pairs)` (`:84-90`);
`InSight(eye, centre, radius, world)` (`:96-104`); `Seen`, `Steps`, `SeenFromStep`, `Place` (`:127-215`).

### Episode lifecycle

1. **ResetEpisode** (`SightEncounter.cpp:169-178`): the `EnvSight` struct is reset except `Object` and `Placed`.
2. **BeforeLevel** (`:180-194`): only when `env.EpisodeSeedIndex != NO_EPISODE_SEED` (an evaluation, or the replay of
   one) and the arena has `SightPairs`: `SightDraw::EvaluationPick(seed, CastingCount, pairs)`; the pair's `Spawn`
   index replaces `Data(env).Spawn`, so the scenario places the seat at the pair's spawn.
3. **Build** (`:196-296`). Returns false (the scenario marks the env `BuildFailed`) if the seat or map is missing, the
   table has under 2 points, the pool is empty, `Place` finds no point, or the object cannot be spawned:
   1. `ObjectPool::Remove` of the last object, state reset, `ObjectPool::ClearOwn(map)` (`:208-210`).
   2. Rung and chance from `ShapingScale()` (`:217-220`): `sight.Rung = SightDraw::Rung(shaping)`,
      `WithholdChance = SightDraw::WithholdChance(shaping, {Withhold0..3})`.
   3. Seeded: `pick` from the seed, `sight.Pair = pick.Pair`. Compass: `Withheld = seeded && env.Evaluating ?
      pick.Withheld : frand() < WithholdChance` (`:228`). An evaluation takes the compass from its seed (alternating by
      round, below); a replay of an evaluation episode in training (seeded, not `Evaluating`) draws it from the rung.
   4. Facing: `SeekDraw::SeedUniform(pair, SALT_FACING = 1) * 2pi` if seeded, else `frand(0, 2pi)`; the seat is
      teleported to the spawn at that facing (`:232-236`).
   5. Object and place: seeded: `ObjectIndex = pair % objects`, point = the pair's `Object` index, `Corner =
      pair.Corner` (`:241-248`). Unseeded: `ObjectIndex = urand(0, n-1)`; `CornerAsked = (1 - shaping >= CornerFrom -
      1e-4) && frand() < CornerShare` (`:262`); then `SightDraw::Place` with `Viewing{EyeRise = PIVOT_SHARE * body
      height, CentreRise = kind.Height/2, Radius = kind.Radius, Nearest/Furthest = Controls.Nearest/Furthest,
      StoreyRise = 4.0 (file constant), CornerStep = Controls.CornerStep, Attempts = max(1, Controls.Attempts)}`
      (`:253-265`).
   6. Spawn: yaw `SeedUniform(pair, SALT_OBJECT_FACING = 2) * 2pi` or random; `ObjectPool::Summon` in the bot's phase
      (`:271-276`). Records `Spot` (base), `Centre` (base + height/2), `Bound = kind.Radius`, `Radius = Bound +
      max(0.1, ArriveTolerance)`, `LegStartMs = EpisodeElapsedMs`, `Straight` (2D spawn-to-spot), `Bearing` (absolute
      angle between the facing and the object, `:284-295`).
4. **View** (`:305-313`): `HasObjective = Placed && !Reached`, `Objective = Centre`, `ObjectiveRadius =
   Vision::ObjectiveRadiusFor(Bound)`, `CompassWithheld = Withheld`. The compass block reads absent when withheld
   (presence and values 0); the camera's objective flag is unaffected and no action is masked.
5. **Reward** (`:315-424`): see "Reward terms". The frame's flag-pixel count comes from `StageScenario::ObserveSeat`
   (`StageScenario.cpp:3700-3710`).
6. **IsTerminal** (`:443-446`): `Reached || DeadForGood(env, 0)`. The clock ends it otherwise.
7. **Teardown** (`:448-453`): removes the object, `Placed = false`.

### Placement algorithm (`SightDraw::Place`, `SightDraw.h:173-215`)

1. Candidates: table points whose 2D distance from `from` is in [Nearest, Furthest] (`:177-182`).
2. Shuffle uniformly (Fisher-Yates driven by `uniform()`), test from the front, at most `Attempts` candidates
   (`:184-192`).
3. "Seen": one ray from the seat's eye (feet z + `EyeRise`) to the object's centre (point z + `CentreRise`), cast with
   `Vision::CastRay` against the static and dynamic trees, WMO liquids and terrain, no units; seen when the hit
   distance is at least `length - Radius` (`InSight`, `:96-104`).
4. Not corner asked: the first seen candidate wins. Corner asked: a candidate that is not seen and is seen from a
   "step" point wins with `Corner = true`; steps are table points 0.5..`CornerStep` yards (2D) from the spawn, within
   `StoreyRise` (4) of its height, and in sight of the eye (eye-level ray, radius 0) (`:137-155`). If no corner
   candidate is found, the first seen candidate is used (`Corner = false`, reported as `corner_fallback`); failing
   that, a second pass over all band candidates (beyond `Attempts`) looks for any seen one (`:207-213`). None at all
   gives `Point = -1` and the Build fails.
5. `Placement::Step` is returned but only `SightEncounterTest.cpp:367` reads it; the encounter does not.

### Evaluation pick (`EvaluationPick`, `SightDraw.h:84-90`)

With `castings` = the scenario's casting count, `round = seed / castings`, `slot = (round/2) * castings + seed %
castings`; pair = `slot % pairs`; compass withheld when `round` is odd. Each casting therefore meets each of its pairs
once with and once without the compass, and rounds 2k and 2k+1 play the same pairs.

### Reward terms (kinds from `RewardTermCategory`, `Rewards/RewardLedger.h:162-241`)

| Term (column `reward_<name>`) | Kind | Formula / condition | Key (default) |
|---|---|---|---|
| `step_cost` | Cost | `-StepCost * DecisionScale()` every decision (`:319`) | `Markers.StepCost` (0.002) |
| `death` | Cost | `-Death` once, when first seen dead (`:350`) | `Markers.Death` (3.0) |
| `stuck` | Cost (noise price) | `AddFixed(-Controls.Stuck * stuckMs/1000)` (`:365`) | `Controls.Stuck` (0.02) |
| `wall` | Cost (noise price) | `AddFixed(-WallCharge(...))` (`:374`) | `Controls.Wall` (0.02), `Controls.WallSlide` (0.5) |
| `progress` | Shaping | `Markers.Progress * (lastDistance - distance) / max(Straight, Radius)`, from the second look (`:383-387`) | `Markers.Progress` (1.0) |
| `facing` | Shaping | `Markers.Facing * 0.5 * (cos(bearing) - lastCos)`, bearing = the object's angle off `seat.Facing` (`:388-392`) | `Markers.Facing` (0.25) |
| `arrive` | Outcome | `+Markers.Arrive` once, on the decision when stopped and inside (`:416`) | `Markers.Arrive` (3.0) |

Conditions. The Progress/Facing/Wall/Stuck/arrival code runs only while `Placed && !Reached && alive` (`:355-356`).
**Inside**: 2D distance from the object's base `<= Radius` and `|z - spot z| <= Markers.ArriveRise` (`:395-396`).
**Stopped**: `Standing::Stopped(movementFlags, moved, Markers.StopMoved)`; the first look counts as stopped but never
pays (`:404`, `:411`). Arrival also records `ArriveSeconds` and `TimeRatio = seconds / max(0.25, max(0, Straight -
Radius)/runSpeed + Bearing/2pi)` (`:417-423`). Progress and Facing are Shaping, so they are multiplied by the stage's
shaping scale; Arrive is Outcome; Death and StepCost are Cost.

### Episode-info columns written (`AddEpisodeInfo`, `:77-167`) and who reads them

| Column | Value | Read by |
|---|---|---|
| `arrived`, `markers` | 1 if reached (both identical) | `move1_controls.yaml`: `eval.report`, `status.headline`, `convergence.measure`, `layout_sampling.metric`; `markers` is the PER_EVENT count (`animus/episode_means.py:15-17`) |
| `compass_withheld`, `compass_present` | 1/0 and the complement | report; PER_EVENT counts for `arrived_no_compass` / `arrived_with_compass` (`episode_means.py:35-36`); `evaluation.py:217-235` computes `arrived_at_rung` |
| `compass_withhold_chance` | the rung's chance (also in evaluations) | report; `evaluation.py` |
| `arrived_no_compass`, `arrived_with_compass` | 1 if reached under that condition | report, status headline |
| `compass_rung` | `SightDraw::Rung(shaping)` | report. This encounter writes no `at_top_rung` and no `difficulty` (`:103-106`). |
| `marker_radius` | `Radius` | no config reads it; present in `apps/forge/python/tests/fixtures/stage_move1_controls.json:4543` |
| `arrive_seconds`, `time_ratio`, `overshoot` | per arrival; PER_EVENT over `markers` | report |
| `markers_sight`, `markers_corner`, `arrive_seconds_sight/_corner`, `time_ratio_sight/_corner` | the same split by `Corner` | report; headline (`_sight`) |
| `stop_distance`, `stops_near` | mean `StopGap` of the stops within `Markers.StopNear`; their count | report; headline; PER_EVENT `stop_distance` over `stops_near` |
| `distance_travelled`, `movement_casts`, `speed_casts` | sums over the episode | report |
| `objective_distance`, `objective_corner`, `corner_fallback`, `sight_object`, `sight_pair` | placement description | report; `objective_corner` and `sight_object` are `episode_categories` |
| `objective_visible` | visible decisions / decisions | report |

`Overshoot` is the largest `distance - Radius` seen after the seat had first been inside (`:397-400`); `Stops` counts
decisions where the seat came to rest after moving with the object within `StopNear` (`:404-409`).

### Config keys read

`Controls.{Nearest 10, Furthest 120, ArriveTolerance 1.0, CornerShare 0.25, CornerFrom 0.75, CornerStep 8, Attempts 64,
Withhold0 0, Withhold1 0.25, Withhold2 0.6, Withhold3 0.9, Stuck 0.02, Wall 0.02, WallSlide 0.5}` and
`Markers.{Arrive 3, StepCost 0.002, Death 3, Progress 1, Facing 0.25, StopMoved 0.05, StopNear 10, ArriveRise 2}`
(`Scenario/Curriculum/CurriculumTuning.h:480-504`, `:677-693`; visited at `:1001-1008`, `:1083-1096`). No clamping is
applied to these keys in `CurriculumTuning.cpp:62-100`. The full table is in [cpp-tuning-keys.md](cpp-tuning-keys.md).

### Interaction with the learner

The ladder is the shaping fade: `SightDraw::Rung(ShapingScale())` and `WithholdChance(ShapingScale(), ...)` read the
scale the learner sets (`StageScenario::SetShapingScale`, `StageScenario.cpp:4637`; default 1.0, `StageScenario.h:477`).
`move1_controls.yaml` sets `fade.rungs: [0.0]` (one rung), `gate_metric: arrived_at_rung`, `costs.enabled: false`. If
the learner applies that single rung's scale 0 from the first update (UNVERIFIED: check the fade code in
`apps/forge/python/animus`), then in live training the shaping scale is 0 (Progress and Facing pay nothing), the rung
is 3, the withhold chance is `Withhold3 = 0.9`, and corners are on (`1 - 0 >= 0.75`); `SightDraw` rungs 0-2 are then
unreachable in training. `arrived_at_rung` is computed in Python (`evaluation.py:40`, `:213-235`) as
`p * rate_no + (1 - p) * rate_with` with `p` the mean `compass_withhold_chance`.

### Tests

`src/test/server/game/Animus/SightEncounterTest.cpp`: TheStageIsDefined, TheHallwayTableIsWellFormed,
TheEvaluationPairsAreFixedAndInTheBand, TheCompassIsWithheldMoreOftenEachRung, AWithheldCompassReadsAsAbsent,
EveryEvaluationPairIsPlayedWithAndWithoutTheCompass, InSightIsOneCameraRayFromTheEye,
ThePlacementIsInSightOrJustRoundACorner, WallAndStuckArePaidOffTheCostLadder, TheStopIsMeasuredFromTheObjectsSide.
Data tests `StockadeHallwaysDataTest.cpp` (EveryHallwayPointIsOnTheFloorAndClear, EveryEvaluationPairIsWhatItSays,
AuthoringPairs; they need `FORGE_VISION_DATA`). Python: `apps/forge/python/tests/test_m1_sight.py`, `test_gates.py`,
`test_stage_purpose.py` (UNVERIFIED which assert on this encounter's columns). `Build` and `Reward` need a live map and
have no unit test; only the draws and the data are tested.

### Quirks, debts, dead ends

- `SightDraw.h:37-38` says the scales are `configs/move1_controls.yaml fade.rungs`, but that file has `rungs: [0.0]`;
  the four scales are hard-coded in C++ (`SightDraw.h:41`) and agree with the learner only by convention.
- `Death` is added with `ledger.Add` and so is subject to the cost scale if a cost ladder is on; `Stuck` and `Wall`
  use `AddFixed`. M1's yaml has `costs.enabled: false`.
- `marker_radius` is written and no config reads it. `Placement::Step` is read by a test only.
- `SightEncounter.cpp:244` takes the object as `pair % objects`; `Stages.cpp:377-378` says "object i mod 5": they
  agree while the pool has 5 kinds.

### Reviewer notes

- `Build` returns false when no point qualifies; the scenario then marks `BuildFailed` and the env is terminal at
  once (`StageScenario.cpp:1837-1840`). What the pool does with a stage whose draws keep failing (a reset loop) is
  UNVERIFIED.
- The frame's flag-pixel count is produced in `ObserveSeat` after `Reward`, so `Reward` reads the previous frame's
  `seat.ObjectivePixels` (the frame the seat decided on). Reordering the two in the env pool would shift
  `objective_visible` by one decision.
- Arrival is judged on the server's applied position and `Stopped`, not on the controller's intent; changing the
  controller's stop semantics changes the stage's measure.
- `Corner` placement needs the world's collision data (`MapVisionWorld`); without vision data the distribution differs
  and the data tests are skipped.

## E1.3 SeekEncounter (M2 `move2_seek`)

Files: `SeekEncounter.h/.cpp`, `SeekDraw.h`. `Opposition::Seek`.

### What it does

One real object is hidden in a Stockades room (or the hallway, by rung); the seat has no compass and finds it by sight
(the camera's objective flag, shown only on pixels whose ray reaches the object first). Found = stopped within
`arena.SeekRadius` (3 yd) of the object's base on its floor.

### Stage definition

`Stages/Stages.cpp:684-702`: `Extends = move1_controls`, `Blocks = {Core, Move, Vision, Map, Goal}`, arenas `rooms`
(training) and `sweep` (`EvalOnly = true`), both `Against = Seek`, `EpisodeSeconds = 300`, `SpawnPoints =
StockadeHallways()`, `Rooms = StockadeRooms()` (39 rooms, `Stages.cpp:185-353`; 13 are `Front`: hall_east_1,
hall_west_1,
hall_east_2, hall_west_2, hall_end, east_south_1, east_north_1, east_south_2, east_north_2, west_south_1, west_north_1,
west_south_2, west_north_2, `Stages.cpp:343-348`), `Objects = SeekObjects()`, `SeekRadius = 3.0`. `Level = 1`. Python
`move2_seek.yaml`: `fade.rungs [1.0, 0.5, 0.25, 0.0]`, `gate_metric: found`, `require_plateau: false`, `eval.episodes
78`, `heldout.sweep 195`, `convergence.measure: found`.

### Public API

```cpp
SeekEncounter(StageScenario&, uint32 envs);
std::vector<RewardTerm> RewardTerms() const;  // Arrive, StepCost, Death, Stuck, Wall, Sighting, NewGround, RoomSeen
static std::vector<std::string> RoomNames(ArenaDefinition const&);    // seek_room categories
static std::vector<std::string> ObjectNames(ArenaDefinition const&);  // seek_object categories
```

`SeekDraw` (`SeekDraw.h`): `Rung {Hallway, Doorway, Room, Deep}`, `RUNG_NAMES`; `Depths(rooms)` (rank by `Walk`, 0..1,
`:42-54`), `Tier(depth)` (thirds, `TIERS = 3`, `:57-60`), `PlacedRung(ladder, u, carry)` (`:80-83`), `RungRooms`
(`:87-95`), `EvaluationRooms` (`:100-107`), `RungEvaluationPick` (`:111-115`), `DoorwaySpot` (`:119-133`),
`FloorRays`/`NewlyLooked` (`:140-190`), `RungSeconds` (`:193`), `NearestOpening` (`:199-213`), `Pick`, `SweepLength`,
`EvaluationPick` (the sweep, `:239-244`), `SeedUniform` (splitmix64, `:248-255`), `Inside`, `Area`, `PointIn`, `RoomAt`.

### Episode lifecycle

1. **ResetEpisode** (`SeekEncounter.cpp:205-214`): as Sight (keeps `Object` and `Placed`).
2. **Build** (`:332-416`); false if the seat or map is missing, or there are no rooms or objects:
   1. Remove last object, reset, `ObjectPool::ClearOwn` (`:343-347`).
   2. **Ladder rung**: `seek.Sweep = arena.EvalOnly`; `ladder = Sweep ? Deep : Rung(SightDraw::Rung(ShapingScale()))`
      (`:354-356`): the fade's scale chooses the placement rung (scales 1/0.5/0.25/0 map to hallway/doorway/room/deep).
   3. **Room and object**: `Sweep && seeded`: `SeekDraw::EvaluationPick(seed, rooms, objects)`: seed i is pair `i mod
      (rooms*objects)`, room `pair / objects`, object `pair % objects`. Other seeded (an evaluation at the training
      rung): `RungEvaluationPick(seed, EvaluationRooms(rooms, ladder), objects)`: the rung's rooms in turn (front cells
      for doorway and room, every room for hallway and deep), object `seed % objects` (`:358-371`). Unseeded:
      `placed = PlacedRung(ladder, frand(), CarryShare)` (the rung below with probability `CarryShare`, never below
      hallway), a room drawn uniformly from `RungRooms(rooms, placed)` (hallway has none, so the room is drawn from all
      rooms, for the episode info only), an object uniformly (`:372-379`).
   4. Spawn: the scenario placed the seat at a random hallway point (`SpawnPointFor`); the encounter gives it a random
      facing and teleports (`:385-387`).
   5. **Hallway rung** (`PlaceInHallway`, `:305-330`): `SightDraw::Place` over the hallway table with `Nearest =
      Seek.HallwayNearest`, `Furthest = Seek.HallwayFurthest`, no corner; the object stands on the chosen point with a
      random yaw (`frand`); `seek.Room` becomes the room whose opening is nearest (`NearestOpening`). If no point is
      found, the episode switches to the doorway rung: a random front room, `seek.Rung = Doorway` (`:392-403`), and goes
      through `Place`.
   6. **Room placement** (`Place`, `:235-303`): spots are drawn from `uniform(attempt, salt)` (`SeedUniform(seed,
      attempt * 8 + salt)` when seeded, else `frand`). Doorway rung first: `DoorwaySpot(room, DoorwayInside,
      DoorwayDeeper, DoorwaySpread, u, v)`, a point `inside + u*deeper` yards from the opening toward the centre and up
      to `spread` yards to either side. Then (and directly for the room and deep rungs) `PointIn(room.Floor, u, a, b)`,
      uniform over the convex polygon. A candidate is accepted when `Map::GetHeight` (from 1 yd above the room floor
      down 4 yd) is within `FloorTolerance` of `room.FloorZ` and `isInLineOfSight` along the four axes at knee height
      (0.5) for `Clearance` yards is clear. Up to `max(1, Attempts)` tries each; with no spot: the room's centre (floor
      looked up there if within tolerance) and `seek.Fallback = true`. Yaw `uniform(0, SALT_FACING = 4) * 2pi`.
   7. **Clock**: `env.EpisodeLengthMs = (Sweep ? max(1, arena.EpisodeSeconds) : Seek.RungSeconds<rung>) * 1000`, where
      the rung is the PLACEMENT's (a carried episode gets the shorter clock of the rung below) (`:406-407`).
   8. Depth and tier of the room; `Entered`/`Looked` bit-vectors sized to the room count (`:408-414`).
3. **View** (`:425-436`): `HasObjective = Placed && !Found`, `Objective = Centre`, `ObjectiveRadius` from the pool
   entry. The stage has no compass block, and the goal block is not told a place.
4. **Reward** (`:438-551`), see below.
5. **IsTerminal**: `Found || DeadForGood(env, 0)`. **Teardown**: removes the object.

### Reward terms

| Term | Kind | Formula / condition | Key (default) |
|---|---|---|---|
| `step_cost` | Cost | `-StepCost * DecisionScale()` per decision (`:441`) | `Seek.StepCost` (0.0005) |
| `death` | Cost | `-Death` once (`:468`) | `Seek.Death` (6.0) |
| `stuck`, `wall` | Cost (noise, `AddFixed`) | as in the common machinery | `Seek.Stuck` 0.02, `Seek.Wall` 0.02, `Seek.WallSlide` 0.5 |
| `sighting` | Shaping | `+Seek.Sighting` once, on the first decision with `seat.ObjectiveSighted` (the first frame with any flag pixel); `SightMs = seat.ObjectiveSightMs` (`:499-504`) | `Seek.Sighting` (0.5) |
| `room_seen` | Shaping | `+Seek.RoomSeen` per room newly looked into this episode: `NewlyLooked` counts the frame's cast rays that hit a floor (terrain or model, normal z at least `Vision::FLOOR_NORMAL`, distance at most `Vision::WRITE_REACH`) inside a room's polygon within 4 yd of its `FloorZ`; at least `max(1, RoomSeenRays)` rays (`:510-516`, `SeekDraw.h:140-190`). Bookkeeping is the episode's own (`seek.Looked`), never the mental map's. | `Seek.RoomSeen` (0.1), `Seek.RoomSeenRays` (3) |
| `new_ground` | Shaping | `+Seek.NewGround` when the seat's cell (`CellKey`: x, y in `NewGroundCell`-yard squares, z in 3-yd storeys) is new this episode and it is not the first look (`:522-523`) | `Seek.NewGround` (0.004), `Seek.NewGroundCell` (4) |
| `arrive` | Outcome | `+Seek.Arrive` once: 2D distance to the base `<= arena.SeekRadius`, `|dz| <= Seek.ArriveRise`, and `Standing::Stopped` using `Markers.StopMoved` (not a Seek key) (`:540-550`) | `Seek.Arrive` (3.0), `Seek.ArriveRise` (2.0), `Markers.StopMoved` (0.05) |

`Found` is set at arrival; `FoundMs = EpisodeElapsedMs`; `RoomsBeforeFound = RoomsEntered`.

### Episode-info columns (`AddEpisodeInfo`, `SeekEncounter.cpp:102-203`)

| Column | Value | Notes and readers |
|---|---|---|
| `found` | 1 if found | the stage measure: `move2_seek.yaml` `convergence.measure`, `fade.gate_metric`, `costs.gate_metric`, `layout_sampling.metric`, report, headline |
| `find_seconds` | `FoundMs/1000` if found | PER_EVENT over `found` (`episode_means.py:22`) |
| `sighted`, `sight_seconds` | first-flag-pixel frame seen; its clock | `sight_seconds` PER_EVENT over `sighted` |
| `found_sighted`, `sight_to_arrival` | found and sighted; `FoundMs - SightMs` | PER_EVENT |
| `objective_visible` | visible decisions / decisions | |
| `rooms_entered`, `rooms_reentered`, `room_entries`, `revisit_rate`, `rooms_before_found`, `rooms_looked` | room walking and looking counts | `revisit_rate` PER_EVENT over `room_entries`, `rooms_before_found` over `found` |
| `seek_room`, `seek_object` | indexes (`max(0, Room)`; a hallway object's room = nearest opening) | `episode_categories` |
| `room_depth`, `difficulty`, `deep_room` | depth 0..1, tier 0..2, tier is the deepest | `difficulty` also splits the evaluation tables |
| `seek_rung`, `room_ladder`, `rung_carried`, `at_top_rung` | placement rung 0..3; ladder rung / 3; carried; ladder is at deep | `at_top_rung` for `convergence.top_rung` |
| `rung_hallway`, `rung_doorway`, `rung_room`, `rung_deep` | placed at that rung | PER_EVENT counts |
| `found_hallway`, `found_doorway`, `found_room`, `found_deep` | found and placed at that rung | PER_EVENT over `rung_<name>` (`episode_means.py:29-32`) |
| `object_fallback` | `Fallback` | |
| `distance_travelled` | sum of per-decision 2D displacement | |

`found_deepest`, used in the yaml headline and targets, is not written here; it is computed in Python
(`apps/forge/python/animus/evaluation.py:337`).

### Config keys read

`Seek.{Arrive 3, StepCost 0.0005, Death 6, ArriveRise 2, Sighting 0.5, NewGround 0.004, NewGroundCell 4, Stuck 0.02,
Wall 0.02, WallSlide 0.5, Attempts 24, FloorTolerance 2, Clearance 0.8, RoomSeen 0.1, RoomSeenRays 3, CarryShare 0.1,
RungSeconds0..3 90/120/200/300, HallwayNearest 8, HallwayFurthest 120, DoorwayInside 2, DoorwayDeeper 1.5,
DoorwaySpread 1}` (`CurriculumTuning.h:588-615`, visited `:1037-1061`) and `Markers.StopMoved`. Arena fields read:
`Rooms`, `Objects`, `SeekRadius`, `EvalOnly`, `EpisodeSeconds` (sweep only), `SpawnPoints` (via the scenario).

### Tests

`SeekEncounterTest.cpp`: TheStageIsDefined, TheRoomTableIsWellFormed, TheObjectPoolIsTheHardCodedList,
TheLadderPlacesByRung, TheEvaluationPlaysTheTrainingRung, EpisodeLengthByRung, LookingIntoARoomIsPaidPerEpisode,
ObjectsAreUniformAndEvaluationsCoverEveryRoom, PlacementPointsAreOnTheFloor, TheAidsAreShapingAndFindingIsTheOutcome.
`StockadeRoomsDataTest.cpp` (EveryRoomSampleIsOnTheFloor, AuthoringScan). Python: `test_seek_metrics.py` (UNVERIFIED
what it asserts). `LiveLayoutPinTest` pins the layout (see tests.md).

### Quirks, debts, dead ends

- `SeekEncounter.cpp:513`: the loop variable `room` of the `NewlyLooked` loop is discarded (`(void)room`).
- The hallway-rung object's yaw and the hallway placement use `frand`, not the episode seed (`:323`, `:329`); the
  comment at `:319-320` says the world thread's random numbers are reseeded before each seeded episode, which would
  make them deterministic (UNVERIFIED: where that reseed happens).
- The unseeded branch (`:372-379`) also applies to the `sweep` arena when it is not seeded and may carry it to a lower
  rung; `Sweep` itself sets only `ladder = Deep` and the clock.
- When a hallway placement fails, `seek.Rung` becomes `Doorway` but `rung_carried` still reflects the original
  `placed != ladder` (`:381`, `:392-400`).
- `HallwayNearest` is 8 here against `Controls.Nearest` 10 in M1.

### Reviewer notes

- The placement rung is read from `ShapingScale()` through `SightDraw::Rung`, so M2's four rungs exist only because
  the yaml's `fade.rungs` has four entries and the scales are hard-coded in `SightDraw.h:41`. A change to either needs
  the other.
- The seat's spawn is the scenario's draw (`SpawnPointFor`); the hallway rung's `Nearest` band is relative to it.
- `Place` calls `map->isInLineOfSight` with `LINEOFSIGHT_ALL_CHECKS` and `ModelIgnoreFlags::Nothing` in the bot's phase.
- `Build` changes `env.EpisodeLengthMs` after the scenario set it from the arena (`StageScenario.cpp:1916`); the
  `STATE_EPISODE_TIME` feature (`StageScenario.cpp:5081`) therefore reads the per-rung value.

## E1.4 InteractEncounter (M3 `move3_interact`)

Files: `InteractEncounter.h/.cpp`, `InteractDraw.h`. `Opposition::Interact`.

### What it does

In an empty Deadmines (map 36) at levels 17-20 the goal names an object by kind (class and template entry), never by
place. Rungs: **distinguish** (the named object among 2-4 decoys of other pool kinds, all in sight of the seat; reach
the right one), **switch** (the named object behind a shut door; the lever on the seat's side opens it), **key** (the
named object is the cannon, which only the Defias Gunpowder opens when used on it).

### Stage definition

`Stages/Stages.cpp:722-743`: `Extends = move2_seek`, `Blocks = {Core, Move, Vision, Map, Sight, Goal}`, arenas `sites`
and `sweep` (`EvalOnly`), `Against = Interact`, `EpisodeSeconds = 120`, `MapId = MAP_DEADMINES`, `Objects =
SeekObjects()`, `SeekRadius = 3.0`, `Sites = DeadminesSites()`; stage `SpawnPoints = {DeadminesMiddle()}`, `MinLevel =
17`, `FocusLevelFirst/Last = 17/20`, `FocusChance = 100`. The four sites (`Stages/Stages.cpp:443-`), with the number of
`Near` plus `Far` points counted from the table: `factory` (door 13965, lever 101831, no key, 65), `foundry` (16399,
101834, 0, 68), `mast_room` (16400, 101832, 0, 56), `iron_clad` (door 16397, opener = the cannon 16398, key item 5397,
43). Python `move3_interact.yaml`: `fade.rungs [1.0, 0.5, 0.0]`, `gate_metric: right_object`, `require_plateau: false`,
`costs.enabled: false`, `eval.episodes 64`, `heldout.sweep 60`.

### Public API

```cpp
InteractEncounter(StageScenario&, uint32 envs);
RewardTerms(): Arrive, DoorOpened, WrongObject, StepCost, Death, Stuck, Wall, Sighting
static std::vector<std::string> SiteNames(ArenaDefinition const&);
static std::vector<std::string> ObjectNames(ArenaDefinition const&);   // pool kinds + "lock"
static bool FollowLever(GameObject* lever, GameObject* door, Unit* user);
static void ResetObject(GameObject*);
```

`InteractDraw`: `Rung {Distinguish, Switch, Key}` (`RUNG_NAMES`), `FADE_SCALES {1, 0.5, 0}` (`:49-51`), `Task {None,
Reach, Use, UseItem}`, `RungOf` (`:63`), `PlacedRung` (`:73`), `TaskOf` (`:79-82`), `RungSites` (`:86-93`), `Index`,
`DecoyCount`, `DecoyKinds`, `EvaluationEpisode` and `EvaluationPick` (`:136-146`), `SweepRung` (`:149`), `DoorWatch`
(`:158-184`), `Reached` (`:189-203`), `Verdict`, `PressFacts`, `Judge` (`:226-248`).

### Episode lifecycle

1. **ResetEpisode** (`InteractEncounter.cpp:222-232`): keeps `Target`, `TargetSpawned`, `Decoys`, `Placed`.
2. **Build** (`:300-471`). Returns false if the seat or map is missing, there are no sites or objects, no site fits the
   rung, the site has no `Near` points, the map lacks the site's door or opener (logged), or the named object cannot be
   placed:
   1. `ClearWorld` (`:249-298`): remove last target (if spawned) and decoys; for every map-spawned game object: doors
      and buttons and every site opener get `ResetObject` (in-use flag cleared, go state ready, loot state ready),
      everything else spawned is `DespawnOrUnsummon(0ms, 1 week)`; then every summoned creature within
      `Interact.SummonSweep` yards of each site's opener (a `WorldObjectWorker` creature grid sweep) is despawned (the
      cannon's pirates).
   2. **Rung**: `ladder = Sweep && seeded ? SweepRung(seed) (= seed % 3) : RungOf(ShapingScale())`; `placed = seeded ?
      ladder : PlacedRung(ladder, frand(), CarryShare)` (a seeded episode never carries, `:318-323`); sites =
      `RungSites(arena.Sites, placed)`: every site for distinguish, sites with `Key == 0` (lever doors) for switch,
      sites with a key for key (`InteractDraw.h:86-93`).
   3. **Draw**: seeded: `EvaluationPick(seed, sites)` (site `sites[seed % n]`, uniforms `SeedUniform(seed, 101, 102,
      104, 105, 106)` for spawn, target, kinds, count, facing); unseeded: the site by `urand`, the five uniforms by
      `frand`. A further stream (seed salt from 200, or `frand`) orders the spots and gives the yaws (`:341-345`).
   4. The door and the opener are the map's own spawns found by entry (`OwnOf`, the first spawn of that entry in the
      spawn-id store).
   5. **Key item**: every site's key item is added to the bot, one of each, on every rung (`:361-364`; guarded by
      `HasItemCount`), so the bags never say the rung. UNVERIFIED: whether items persist between episodes.
   6. The seat on a `Near` point `Index(SpawnU, Near.size())`, facing `FacingU * 2pi`, teleported.
   7. **Kinds**: key rung: `ObjectIndex = objects` (the "lock" index), decoys `DecoyKinds(objects, count, objects, u)`
      (any pool kinds); otherwise the named kind `Index(TargetU, objects)` plus `DecoyKinds(named, count, ...)`, `count
      =
      DecoyCount(CountU, DecoysMin, DecoysMax, objects)` clamped to the number of other kinds, kinds taken consecutively
      from the pool's other kinds from a rotation offset `Index(KindsU, others)` (`InteractDraw.h:102-123`).
   8. **Spots**: the pool is the site's `Far` for switch, else `Near`. Candidates in a shuffled order; for each kind
      (the named one first) the first candidate at least `Spacing` yd (2D) from every taken spot (the seat's start
      included) is taken; for non-switch rungs it must also be `SightNearest..SightFurthest` yd from the seat and
      `SightDraw::Seen` from its eye. `tested` is one counter across all kinds, capped at `max(1, Attempts)` in total
      (`:395-429`). Kinds that find no spot are left out (`Fallback = spots.size() < kinds.size()`).
   9. **Spawn**: on the key rung the target is the opener (the cannon): `Target = Opener`, `Spot` = its position,
      `NamedEntry`, `NamedClass = Vision::Classify(FactsOf(bot, opener)).What`. Pool kinds are summoned with
      `ObjectPool::Summon`; the named kind sets `Target`, `TargetSpawned`, `Spot`, `Radius`, `Height`, `NamedEntry`,
      `NamedClass`; the others become `Decoys` and `DecoySpots`. `Placed = !Target.IsEmpty()` (`:434-470`).
   10. **Clock**: `env.EpisodeLengthMs = RungSeconds[placed rung] * 1000` = 60, 120, 90 s (`:469`).
3. **OnSeatAction(env, seat, result)** (`:480-530`): after a seat's action is applied, `Draw::Judge` classifies a
   sight press (`SeatActionResult::ActPress`, `ActedOn`, `ActRefused`); see "Press judgement".
4. **View** (`:532-546`): `HasObjective = false`, `ObjectivePlaceKnown = false`; if placed: `NamedTask`, `NamedClass`,
   `NamedEntry`, `NamedObject = true`. No flag and no compass.
5. **Reward** (`:554-687`), see below.
6. **IsTerminal**: `Found || DeadForGood(env, 0)`. **Teardown**: removes the target and decoys.

### Press judgement (`InteractDraw::Judge`, `:226-248`)

A press counts only if it is Interact or UseItem on a non-null entity. Order: (1) a decoy pressed while `Sent ||
Reached` gives `Wrong` (even if the world refused for what the object is: loot, kind, locked, no item, see
`InteractDraw::Reached`, `:189-203`); (2) if not sent, `None`; (3) on the target: for `Task::UseItem` only a UseItem
press is `Right` (an Interact gives `None`), otherwise `Right`; (4) on the site's opener with an Interact press:
`Opener`. `TaskOf` returns `UseItem` for the key rung and `Reach` for both distinguish and switch; `Task::Use` is never
returned (`InteractDraw.h:57`, `:79-82`).

On `Opener` the encounter sets `LeverPressed`, calls `Watch.Press()`, and calls `FollowLever(opener, door, bot)`: if
the lever's loot state is `GO_ACTIVATED` and the door is still ready, it calls `door->UseDoorOrButton(0, false, user)`,
i.e. the encounter completes the door's opening if the map's script did not (`:116-124`, `:516-526`).

### Reward terms

| Term | Kind | Formula / condition | Key (default) |
|---|---|---|---|
| `step_cost` | Cost | `-StepCost * DecisionScale()` | `Interact.StepCost` (0.0005) |
| `death` | Cost | `-Death` once | `Interact.Death` (6.0) |
| `stuck`, `wall` | Cost (noise, `AddFixed`) | as in the common machinery | `Interact.Stuck` 0.02, `Interact.Wall` 0.02, `Interact.WallSlide` 0.5 |
| `sighting` | Shaping | `+Interact.Sighting` once, on the first decision whose sight list (`seat.Seen.Info[slot].Guid`) contains the target guid; `SightMs = EpisodeElapsedMs` (`:610-622`) | `Interact.Sighting` (0.5) |
| `door_opened` | Outcome | `+Interact.DoorOpened` once, on the first shut-to-open of the site's door after the seat's own lever press, and only on the switch rung (`DoorWatch::Look`, `:624-629`); an opening before a press is measured (`door_opened`) but not paid | `Interact.DoorOpened` (1.0) |
| `wrong_object` | Cost | `-Interact.WrongObject * pending` for decoys newly pressed this decision (each decoy once), and `-WrongObject` when stopped beside a not-yet-taken decoy (`:631-636`, `:681-686`) | `Interact.WrongObject` (0.5) |
| `arrive` | Outcome | `+Interact.Arrive` once, when a pending Right press is seen (`:638-645`) or, on a Reach task, when stopped within `arena.SeekRadius` of the named object on its floor (`|dz| <= ArriveRise`) with it nearer than any decoy (`:647-680`) | `Interact.Arrive` (3.0), `Interact.ArriveRise` (2.0), `Markers.StopMoved` |

Details. A press the world refuses is priced by the sight block (`Actions.Aimless.ActRefused`), not here. The stopped-
beside check covers decoys on every rung, including key (where the named object is not checked because `reachTask` is
false, so a decoy stop is charged but the cannon is never "reached" by standing near). A stop beside a decoy charges
`WrongObject` once per decoy (`DecoyTaken`) and counts `WrongStops`; a press counts `WrongPresses` always and charges
once per decoy.

### Episode-info columns (`AddEpisodeInfo`, `:137-220`)

`right_object` (the stage measure: the yaml's convergence, fade gate and `layout_sampling.metric`), `right_seconds`
(PER_EVENT over `right_object`), `sighted`, `sight_seconds`, `found_sighted`, `sight_to_arrival` (as Seek's),
`objective_visible` (visible = the target guid is in the seat's list), `wrong_objects` (count of `DecoyTaken`),
`wrong_presses`, `wrong_stops`, `decoys` (count), `lever_pressed`, `door_opened` (`Watch.Opened`), `door_by_lever`
(`Watch.ByLever`), `key_used`, `interact_site`, `interact_object` (the pool size means "lock"), `interact_rung`,
`interact_ladder` (rung share of the top), `rung_carried`, `at_top_rung`, `rung_distinguish/_switch/_key`,
`right_distinguish/_switch/_key` (PER_EVENT over `rung_<name>`, `episode_means.py:44-47`), `object_fallback`,
`distance_travelled`. The yaml's `act_refused_*` columns are not written by this encounter (they come from the
scenario's sight-block accounting; UNVERIFIED which file).

### Config keys read

`Interact.{Arrive 3, DoorOpened 1, WrongObject 0.5, StepCost 0.0005, Death 6, ArriveRise 2, Sighting 0.5, Stuck 0.02,
Wall 0.02, WallSlide 0.5, CarryShare 0.1, RungSeconds0..2 60/120/90, DecoysMin 2, DecoysMax 4, Spacing 2.5,
SightNearest 4, SightFurthest 30, Attempts 64, SummonSweep 80}` (`CurriculumTuning.h:634-657`, visited `:1062-1082`) and
`Markers.StopMoved`.

### Tests

`InteractStageTest.cpp`: TheStagesLayout, TheSites, TheLadder, DecoysAndEvaluationDraws, AWrongObjectPressIsPriced,
TheDoorIsPaidOnceAnEpisode, AWrongObjectIsTakenByPressingOrStoppingNeverPassing, TheNamedRowIsAlwaysInTheSightBlock,
and the fixture tests TheDistinguishGoalNamesATypeNotAPlace, ALeverPressThroughTheHandlerOpensItsDoor,
TheKeyItemUseOpensTheLock, AnOpenDoorIsTheBandAtTheTopOfItsFrame. `DeadminesSitesDataTest.cpp`
(EverySitePointIsOnTheFloorAndClear, OnlyTheShutDoorShowsTheFarSide, AuthoringScan). Python `test_interact.py`
(UNVERIFIED contents).

### Quirks, debts, dead ends

- `Task::Use` is never produced. The header (`InteractEncounter.h:49-50`) and the yaml describe the switch rung as a
  lever "use", but the named object on switch is asked as `Reach`; the lever is a means, not the named object.
- `EnvInteract::Radius` and `Height` are assigned (`:452-453`) and read nowhere in the repo (`grep` of
  `InteractEncounter.cpp` finds only the writes at `:452-453` and unrelated uses of `object.Radius`).
- The key rung's decoys use `DecoyKinds(objects, ...)`, so every pool kind can be a decoy.
- On the switch rung decoys also stand behind the door and are stop-checked at `SeekRadius` (`:664-672`); whether a
  wrong stop through a door can be charged depends on the world's geometry (UNVERIFIED).
- `FollowLever` is a server-side completion of the door's opening after the seat's own press; principle 4 forbids
  server shortcuts to actions, and this one only fires after the seat pressed the lever.
- `ClearWorld` despawns every non-door, non-opener spawned game object for a week at each reset (a few dozen).
- `sight_seconds` here is the episode clock at the first listing; in Seek it is `ObjectiveSightMs`.

### Reviewer notes

- `tested` is shared across kinds (`:397`): a long run of rejections for the first kind leaves later kinds fewer
  attempts; `object_fallback` counts the shortfall but not its cause.
- The `Wrong` verdict is judged before `Sent` (`InteractDraw.h:233`): pressing a decoy out of reach gives `None`, but a
  locked or no-item refusal counts as taking it.
- The door state test (`door->GetGoState() != GO_STATE_READY`, `:628`) takes any non-ready state as open.
- `OwnOf` returns the first spawn of an entry; two doors of one entry would be ambiguous (UNVERIFIED that the
  Deadmines entries are unique).
- Adding a site needs an `InteractSite` plus a data test and the door and opener spawns in the map.

## E1.5 Observed issues (this fragment)

1. `SightDraw.h:37-38` points at `configs/move1_controls.yaml fade.rungs` but the yaml has `rungs: [0.0]`; the
   withholding ladder's other rungs are unreachable in live training if the learner applies the scale (E1.2).
2. `Task::Use` is dead (`InteractDraw.h:57`); switch asks `Reach` (`:79-82`) while headers say "use".
3. `ObjectPool.h:33-34` comment says SeekEncounter has its own copy of the steps; it does not.
4. `SeekEncounter.cpp:513` discards the loop variable (`(void)room`).
5. `SightEncounter` writes `marker_radius` (`:108`), which no config reads; `Placement::Step` is read only by a test.
6. `InteractEncounter::EnvInteract::Radius` and `Height` are write-only (`:452-453`).
7. Seek hallway placement and its object yaw use `frand`, not the seed (`SeekEncounter.cpp:323`, `:329`), although the
   comment at `:319-320` claims seeded determinism through reseeding.
8. The unseeded branch of Seek's `Build` also runs for the `sweep` arena when not seeded and can carry the rung
   (`:372-379`).
9. `Death` uses `ledger.Add` (cost ladder applies) while `Stuck` and `Wall` use `AddFixed`; no live consequence only
   because the cost ladder is off in `move1_controls.yaml` and `move3_interact.yaml` (`costs.enabled: false`);
   whether `move2_seek.yaml` inherits that is UNVERIFIED (its `costs:` block sets only `gate_metric` and `gate_value`).
10. Interact's attempt budget `tested` is shared across all kinds of an episode (`InteractEncounter.cpp:397-407`).
11. Seek's rung for the clock and `rung_*` columns is relabelled to doorway after a hallway failure while
    `rung_carried` keeps the old value (`SeekEncounter.cpp:381`, `:392-400`).


## E2a.1 PartyFollowEncounter (M4 `move4_follow`; `PartyFollowEncounter.h` 182 lines, `.cpp` 738 lines)

`Opposition::PartyFollow`. Four learned followers (`ArenaDefinition::PartySize` = 4) keep with a leader that walks the
dungeon's boss route. Nothing is fought. The leader lives in the **owner's slot** of the scenario
(`StageScenario::OwnerAgent()`, `BuildOwnerSeat`, `ReleaseOwnerSeat`); this encounter is the only user of that slot
(`PartyFollowEncounter.h:43-46`). The party has no core group (no `PartyEncounter`; `PartyGroup` must be false,
`Stages.cpp` `ArenaProblem`), so the frames the followers see are built by this encounter (`View`).

### Types and API

- `static InTheWay(leaderX, leaderY, leaderYaw, leaderMoving, x, y, yards, halfAngle)` (`.cpp:60-76`): false if the
  leader is not moving; false beyond `yards`; true within 0.25 yd; else true when the bearing off the leader's facing
  is at most `halfAngle` degrees (angle difference wrapped with atan2(sin, cos)).
- `static RegroupShare(seconds, window)` (`:78-83`): `clamp(1 - seconds / window, 0, 1)`; for `window <= 0` it is 1 only
  when `seconds <= 0`.
- `static StopSeconds(rung, first, last)` (`:85-89`): linear from `first` (rung 0) to `last` (rung 3) with
  `t = min(rung, 3) / 3`.
- `RUNGS = 4`. `IsCast(env)`, `HasLeader(env)` accessors (used by `StageScenario` for the cast row).
- Private: `enum Phase {Stopped, Walking, BackStep, Done}`; `SeatFollow` (per follower: `RespawnClock Clock`,
  `InBandMs`,
  `LostMs`, `BlockingMs`, `DistanceSum`, `Samples`, `RegroupPending`, `RegroupStops`, `Regroups`, `RegroupMsTotal`,
  last-reward/stuck/wall stamps, last position); `EnvParty` (`Built`, `Cast`, `Rung`, `Entrance`, `Stops`, `NextStop`,
  `Way`, `Mode`, stop timers, counters, fall tracking, `Seats[GROUP_SEATS]`). `_routes` is a `std::map<mapId, stops>`
  guarded by `_routesLock` (the only cross-thread structure: envs on different map threads call `RouteStops`).

### Lifecycle

1. **ResetEpisode** (`:206-213`): `party = EnvParty()` but `Built` is kept (the leader's character stays until `Build`).
2. **BeforeLevel** (`:215-239`): for the arena's map, finds the `FollowBosses()` row (`InstanceBosses.cpp:113`) and sets
   `EpisodeLevel = urand(low, high)` from `InstanceEncounter::DungeonLevels(row)` capped at `DEFAULT_MAX_LEVEL`;
   `DungeonDifficulty = RaidDifficulty = 0`; `EpisodeTeam = urand(TEAM_ALLIANCE, TEAM_HORDE) + 1` (one faction for the
   whole party and the leader, so nobody reads another as a hostile player); if the map has an entrance area trigger
   (`sObjectMgr->GetMapEntranceTrigger`), `EpisodeSpawn` is its target and `HasEpisodeSpawn = true`.
3. **Build** (`:241-287`): needs seat 0's bot and the map. Releases last episode's leader (`ReleaseOwnerSeat`), resets
   the state, `ObjectPool::ClearOwn(map)` (removes the dungeon's own gameobjects: doors, levers, chests, so the script
   is
   never stopped by a shut door), `Rung = min(SightDraw::Rung(ShapingScale()), 3)` (the fade's rung; evaluations play
   the training rung), `Cast = !env.Evaluating && CastShare > 0 && roll_chance_i(CastShare)`, `Entrance =
   SpawnPointFor(env)`. `Stops = RouteStops(mapId)`: the spawn position of each boss row of `FollowBosses()` with
   `row.MapId == mapId`, via `InstanceEncounter::FindSpawn`, in table order, cached per map. No stops -> `LOG_ERROR`,
   build fails. Builds the leader with `BuildOwnerSeat(env, map, level, Entrance, AptitudeDemand::Anything())` (any
   class), gives it seat 0's faction, `env.Allies = {leader}`, `Mode = Stopped`, `StopUntilMs = now + 3000`
   (`START_PAUSE_MS`), records the leader's controller-stuck baseline, and schedules the first sudden stop.
4. **Update** (per decision, `:458-517`): `RespawnFollowers` first; then if there is a leader in the world:
   - a dead leader (a fall in Ragefire's cavern) is stood up where it fell after `LEADER_RISE_MS = 2000` through
     `RiseAtEntrance(leader, seat, here, now)`, `LeaderRises++`, and nothing else happens that decision;
   - a hurt leader is set to full health every decision;
   - drops: a fall (z below the previous z by more than 0.5) is tracked; on landing, `FallFromZ - z >= 4` (`DROP_YARDS`)
     adds one to `Drops`;
   - a **cast** leader (a frozen checkpoint plays the row) only advances `NextStop`/`StopsReached` when within 3 yd
     (`LEADER_ARRIVE`) of the next stop; it keeps no stop pauses and never counts regroup stops;
   - a **scripted** leader runs `Steer`.
5. **Steer** (`:344-433`): the leader's keys are the script (the only scripted actor left in the M4 stage; see
   principle 14 and the note below).
   - Stopped and `now >= StopUntilMs`: `Mode = Walking` (or `Done` if no stops left).
   - Walking with a sudden stop due (`NextSuddenMs`): `SuddenStops++`; duration `urand(SuddenStopMinMs,
     SuddenStopMaxMs)`;
     if `Rung >= BackStepFromRung` and `roll_chance_i(BackStepChance)` it first backs `BackStepYards` along its reversed
     facing (`Mode = BackStep`, then `Stop` on arrival within 1 yd or after `GiveUpMs` stuck), else `Stop(ms, counts)`
     at once; the next sudden stop is rescheduled.
   - Walking: reaching the stop (2-D within 3 yd) or being stuck `GiveUpMs` on the leg advances `NextStop`; arrival
     counts `StopsReached++` and stops for `StopSeconds(Rung, StopSecondsFirst, StopSecondsLast)` seconds; giving up
     counts `Skips++`. After the last stop `Mode = Done` (the leader stands there for good).
   - Keys: none unless Walking/BackStep and the controller `Started()`. Walking plans with
     `RoutePlanner::Instance().Plan`
     (navmesh corners; a replan when `Way` is invalid or its end is over 1 yd from the target, or the leader strays over
     15 yd, `LEADER_STRAY`, from the next corner), then `Movement::Seek(body, x, y)` gives the `ControlState`;
     `held.Walk = Rung < WalkRungs || Mode == BackStep`. `seat.Controls.Held = held` keeping `FaceTurnApplied`.
   - `Stop(counts=true)` with `ms >= RegroupMinStopMs` increments `RegroupStops` and marks every living, not-out
     follower `RegroupPending` (counting `RegroupStops` per follower). Sudden stops count as regroup stops too.
6. **RespawnFollowers** (`:435-456`): per active seat < 5, `Clock.Note(now, alive, PartyYards, Respawn.DelayMs,
   Respawn.RejoinYards)`; on `Died` clears `RegroupPending`; on `Rise` `RiseAtEntrance(bot, seat, party.Entrance, now)`
   and
   `Clock.Risen`. See `EntranceRespawn.h` ([EntranceRespawn section of this document]).
7. **SelectTarget**: always sets `target = nullptr` and returns true (never reached in M4 since the stage has no sight
   block? M4's blocks have no `Sight`, so this runs: no target, nothing to fight).
8. **View** (`:556-599`): the cast leader's row gets `HasObjective` = there is a next stop and `Objective =
   Stops[NextStop]`.
   A follower gets no objective; `MinimapYards = PartyFollow.MinimapYards`; its `Frames` are filled by
   `PartyFramesBlock::FillFrame`: slot 0 the leader (`leads = true`), then the other followers in seat order (up to
   `GROUP_MEMBERS` slots).
9. **Reward**: below.
10. **WriteState** (critic only): `STATE_TIER = Rung / 3`; leader present/alive/health and its x, y relative to the door
    in
    the owner's state columns.
11. **IsTerminal**: always false (the episode runs to its clock; no death ends it).
12. **Deactivate/Teardown**: release the leader seat, `Built = false`, `env.Allies.clear()`.

### Rewards (per follower, `Reward`, `.cpp:601-700`)

`RewardTerms()`: FollowKept, Regroup, Lost, Blocking, Death, Stuck, Wall. Reads `PartyFollow.*` and
`Seek.Stuck/Wall/WallSlide`.
`stepMs` = time since the seat's last reward call; only seats < 5 and active.

| Term | Kind | Condition and formula | Keys (defaults) |
|---|---|---|---|
| Death | Cost | once per death while the seat is dead and `!DeathCounted`: `-Death` (via `Add`); sets tally Died/DeathMs, `Deaths++` | `PartyFollow.Death` 3.0 |
| Stuck | Cost | `ledger.AddFixed(-Stuck * stuckMs/1000)`, only while alive and not out | `Seek.Stuck` 0.02 |
| Wall | Cost | `AddFixed(-Standing::WallCharge(wallS, moved, asked, Wall, WallSlide))`, `asked = speed(kind) * DecisionMs/1000` | `Seek.Wall` 0.02, `Seek.WallSlide` 0.5 |
| FollowKept | Outcome | band 1 (distance within `BandMin..BandMax`): `+Kept * seconds` | `Kept` 0.02, `BandMin` 3, `BandMax` 10 |
| Lost | Cost | band 3 (past `LostYards`): `-Lost * seconds` | `Lost` 0.02, `LostYards` 40 |
| Regroup | Outcome | once per counted stop when a pending follower is in band: `+Regroup * RegroupShare(since, RegroupWindow)`; expires unpaid after the window | `Regroup` 0.5, `RegroupWindow` 20 s, `RegroupMinStopMs` 2000 |
| Blocking | Cost | `InTheWay(...)` with the leader's held keys moving: `-Blocking * seconds` | `Blocking` 0.05, `BlockYards` 2.5, `BlockHalfAngle` 45 |

`Standing::Band(distance, BandMin, BandMax, LostYards)` returns 0 too close, 1 in band, 2 behind, 3 lost (`Standing.h`).
After a death or while out (`Clock.Out`) the function returns before the cost terms; nothing is paid without a living
leader on the same map. Death is the only term here paid while dead.

### Episode info columns (exact names)

`follow_kept_share` (InBandMs / elapsed), `follow_distance_mean`, `lost_seconds`, `blocking_seconds`, `regroup_stops`,
`regroups`, `regroup_share` (regroups / regroup_stops), `regroup_seconds` (mean), `deaths`, `rises`, `rejoins`,
`rejoin_seconds`, `rejoined` (share of rises that rejoined; 0 when no rises), `dead_seconds`, `leader_stops_reached`,
`leader_route_share`, `leader_skips`, `leader_sudden_stops`, `leader_drops`, `leader_rises`, `leader_cast`, `difficulty`
(the rung), `at_top_rung`. Plus `reward_*` for the seven terms. `apps/forge/python/configs/move4_follow.yaml:37-93`
reads
`follow_kept_share` (headline, target `>= 0.9`, `gate_metric`, `measure`), `regroup_share >= 0.9`, `lost_seconds <= 5`,
`rejoined >= 0.9` and the others in its report list. `deaths` and `rejoined` columns are also defined by other
encounters
(same names): the scenario builds one column per name (first added wins; see `EpisodeInfoTable::Contains` only used for
reward columns, so duplicates in AddEpisodeInfo are UNVERIFIED: check whether a second `Add("deaths")` from another
encounter in the same stage creates a duplicate column; in M4 only this encounter is present).

### Quirks and notes

- The leader is scripted: it is the M4 exception to "no scripted players" (principle 14); its keys are `Movement::Seek`
  toward navmesh corners, and `CastShare` (default 0 percent, `PartyFollow.CastShare`) hands it to a frozen checkpoint.
- `Cast` leaders skip `Stop` entirely, so `regroup_stops` is 0 and no `Regroup` is ever paid in cast episodes.
- Sudden stops are scheduled with at least 1 s (`std::max<uint32>(1000, ...)`) and the gap is halved from
  `BackStepFromRung` (`ScheduleSudden`, `:309-321`).
- `PartyFollow` on the second episode in a new instance relies on `StageScenario.cpp:2294-2307` clearing creatures with
  `DUNGEON_CLEAR_RADIUS` 1000.
- `SelectTarget` comment says nothing to fight but is only reached when the stage has no sight block (M4 has none).
- Tests: `PartyFollowTest.cpp` (stage validity at `:428-445`; the pure helpers).

### Reviewer notes
- `Steer` mixes plan state, key generation and counters in one 110-line function; `Phase::Done` is reached from two
  places.
- `RouteStops` copies the vector under the lock on every `Build`.
- The follower frames are hand-built because there is no core group; any change to `PartyFramesBlock::FillFrame`'s
  contract affects both this and `FillFromGroup`.

### Observed issues
1. `PartyFollowEncounter.cpp:470-481` comment/docs say "scripted leader that died stands up where it fell" - correct,
   but a
   dead leader returns from `Update` before `Steer`, so a stop that was running resumes with stale timers after rising
   (`StopUntilMs` is absolute episode time).
2. `Reward` returns early for a dead/out follower after the Death charge, so `follow.LastRewardMs` advances while dead:
   no time-in-band is credited for the interval after rising (intended, but undocumented).
3. `RegroupPending` can remain true when a follower is out; cleared at `Died` (`:448`) only.
4. `.h:43-46` states the leader is "no dead code" - the leader is the sole user of `OwnerAgent`/`BuildOwnerSeat`,
   a leftover slot of the deleted owner mechanism.

## E2a.2 CombatEncounter (C1-C3 `combat1_fight`, `combat2_packs`, `combat3_survive`; `CombatEncounter.h` 183, `.cpp` 707, `CombatDraw.h` 148)

`Opposition::Combat`, one solo seat (seat 0 only: `Reward` returns for `seatIndex != 0`, `.cpp:569-572`) on a cleared
Ragefire Chasm. The three stages are three `CombatDrill`s of the same class: `Fight` (C1: `fight` weight 3 and `guard`
weight 1 with `Ally`), `Packs` (C2: `packs` weight 2, `fire` weight 1 with `Hazards`), `Survive` (C3: `survive`). All
arenas set `RespawnAtEntrance`. The seat perceives through the sight/combat blocks and selects its own target
(`SelectTarget` is not overridden; the scenario takes the seat's client selection in a sight stage). Per-env state is
`EnvCombat` (`.h:96-150`); the encounter also owns a `DifficultyLadder _ladder` (section E2a.3) and a per-map cache of
corridor start points (`_corridors`, guarded by `_corridorLock`).

### Draw: where the seat starts, what spawns (`CombatDraw.h`, `CombatEncounter.cpp:145-320`)

- **Corridor points** (`FindCorridors`, `.cpp:156-186`, once per map, `Corridors()` `:145`): every creature spawn of the
  map from `sObjectMgr->GetAllCreatureData()` sorted by spawn id (fixed order, so an evaluation's seed picks the same
  point), kept when on dry ground (`map->GetLiquidData(...) == LIQUID_MAP_NO_WATER`, body height 2) and reached from the
  entrance (where `bot` stands then) by a whole `PathGenerator` path (type `NORMAL`, not `NOPATH`) of length at most
  `Combat.CorridorWalk` (220 yd), each at least `Combat.CorridorSpacing` (8 yd) from every point already kept
  (`CombatDraw::CorridorPoints`, `.h:96-117`). The first call must be made by a bot standing at the entrance. The log
  line
  reports the count. `RolesEncounter` reuses `FindCorridors` statically.
- **Seat placement** (`Build`, `:188-242`): `Despawn(env)` first; `combat = EnvCombat()`; rung from
  `_ladder.Draw(env, layout, spec, Combat.MaxTier)`; the start point is `corridors[EpisodeSeedIndex % n]` in an
  evaluation, else a uniform draw; `BotFactory::TeleportWithinMap(bot, point, random facing)`; `StartWalk` is the
  straight-line distance to the entrance (episode info `start_walk`). Then `_scenario.PrepareFighter(bot, seat)`; for
  `Survive` it stocks food and drink (`ConsumablePool::Food/Drink(level)`, drink only if the seat has mana,
  `StockConsumables`); for an `Ally` arena `SpawnAlly`; `SpawnPull(env, map, bot, nullptr)` (false fails the build); for
  non-`Fight` drills a second pull is spawned beyond the first (`from = &Packs.front().Spot`); `ListTargets`.
  If there are no corridor points the seat is left where it is (no error).
- **What a pull is** (`CombatDraw::PlanPull`, `.h:45-80`), with `steps = tier * LevelsPerTier` (1):

| Drill | LevelOffset | Size | Elite | Caster | Hazard | Linked |
|---|---|---|---|---|---|---|
| Fight | `LevelBase + steps` (-2 + tier) | 1 | `tier >= EliteTier` (4) | `!Elite && tier >= CasterTier (1) && casterRoll` | no | no |
| Packs | `LevelBase + steps/2` | `min(4, 2 + tier/2)` | `tier >= 4` | `tier >= 1` | `fireRoll` | `tier >= LinkedTier (2)` |
| Survive | `LevelBase + steps + SurviveLevels (2)` | `max(1, SurviveSize (3)) + (tier >= 3 ? 1 : 0)` | `tier >= 4` | `tier >= 1` | `fireRoll` | always |

  `fireRoll = drill != Fight && (arena.Hazards || roll_chance_i(Combat.HazardChance 33))`; `casterRoll =
  roll_chance_i(Difficulty.CasterChance 40)`. Creature level = `clamp(seatLevel + LevelOffset, 1, 83)`
  (`CreatureLevel`). Note that 83 is not `DEFAULT_MAX_LEVEL` (80) used elsewhere (`Opponents`).
- **Entries** (`SpawnPull`, `:244-320`): an elite (`pool.RandomElite`) first if the plan has one, then a hazard caster
  (`RandomHazardCaster`), then a caster (`RandomCaster`), each only while there is room, the rest `pool.Random(level)`
  (C1: default-AI open-world creatures) or `pool.RandomPackMember(level)` (C2/C3); an empty draw ends the fill and
  zero entries fails the pull. **Where**: with `from` (the next pack) `Opponents::FindSpawnPointFrom(bot, map, *from,
  bearing from the seat through `from`, spread 1.0 rad (`NEXT_SPREAD`), NextNearest 30, NextFurthest 45)`; otherwise, or
  if that finds nothing, `Opponents::FindSpawnPoint(bot, map, FightNearest 28, FightFurthest 40)`.
  `Opponents::SpawnPack`
  summons the pack around the spot. The `guard` ally's creature gets 10 threat on the ally and `AttackStart(ally)` for
  the first pack only (`Packs.size() == 1`).
- **Ally** (`SpawnAlly`, `:322-340`): a creature from `pool.Random(level)` placed 2-5 yd from the seat, faction of the
  seat, `REACT_PASSIVE`.

### Per-tick and per-decision flow

1. `UpdateEnemies` (`:371-396`): per pack, marks `Engaged` (and `EngageMs`) once a member has a victim; for a `Linked`
   pack, every idle member attacks the victim.
2. `Update` (`:398-440`): I4 respawn first. `FellAt` is stored while dead and not yet out; `fightYards = dist(bot,
   FightPoint)` (or -1 when dead); `Clock.Note(...)`; on `Step::Rise` with `RespawnAtEntrance`: `RiseAtEntrance(bot,
   seat, SpawnPointFor(env), now)`, `Clock.Risen`, `DeathPaid = false`. If dead, stop. C1: when `NextFightMs` is due,
   respawn the ally if dead and spawn the next creature. C2/C3: top the queue up to two packs whenever fewer remain.
3. `OnSeatAction` (`:442`): remembers `PendingInterrupt` (the target the seat's interrupt was aimed at).
4. `View` (`:448-468`): `PullsCleared = Clears + Kills`; for `Survive` the gauntlet block's `FoodItem`, `DrinkItem`,
   `GauntletSupplies = CONSUMABLE_COUNT`; if the front pack fights, `ElitePull` and `PullTime = min(1, (now -
   EngageMs) / 60000)`.
5. `BeforeRewards` (`:497-567`): zeroes the per-decision counters; detects the ally's death (`AllyDeaths`); counts each
   dead member of the front pack once (C1: `Kills++`, and `KillSeconds += (now - EngageMs)` if engaged); an **extra
   pull** is counted once when the second pack is engaged while the front one still lives; when no member of the front
   pack is alive: non-`Fight` drills `Clears++`/`NewClears++`, the pack is erased, C1 schedules `NextFightMs = now +
   Combat.NextFightMs (2000)`, C2/C3 spawn a replacement beyond the back pack if the seat is alive; `ListTargets`.
6. `Reward` (`:569-689`): see the table. Also counts `Decisions`, `SelectedDecisions` (`bot->GetTarget()` non-empty),
   `InViewDecisions` (`CombatBlock::InView`), `RestMs` (a regen aura) when alive.
7. `IsTerminal` (`:698`): `EpisodeElapsedMs >= EpisodeLengthMs` only; a death never ends it. `WriteState`: `STATE_TIER =
   Tier / max(1, MaxTier)`.
8. `Teardown`/default `Deactivate`: `Despawn` all packs and the ally.

### Rewards (`Reward`, with `w = TierWeight(Difficulty.TierScale 0.25, tier) = 1 + 0.25 * tier`)

`RewardTerms()` (`.cpp:58-63`): Kill, Clear, Survived, InterruptLanded, Away, Death, TeammateDeath, Hurt, FireHurt,
PullExtra, StepCost, DamageDealt. (`CombatReward.cpp:65-73` is where the term names live; see
[cpp-rewards-routing.md](cpp-rewards-routing.md).) Kinds from `RewardLedger.h:178-250`.

| Term | Kind | Condition and amount | Key (default) |
|---|---|---|---|
| Kill | Outcome | C1 only: `Kill * w` per creature newly dead | `Combat.Kill` 1.0 |
| Clear | Outcome | C2/C3: `Clear * w` per pack newly cleared | `Combat.Clear` 2.0 |
| InterruptLanded | Outcome | C2/C3 when the seat's pending interrupt target's cast is in `env.StepInterruptedTargets`: flat `+InterruptLanded` (not tier-scaled; C1 counts `Interrupts` but pays nothing) | `Combat.InterruptLanded` 0.25 |
| Survived | Outcome | once, on the decision the clock runs out, if `Deaths == 0`: `(Survive ? SurviveSurvived : Survived) * w` | `Combat.Survived` 1.0, `Combat.SurviveSurvived` 2.0 |
| PullExtra | Cost | `-ExtraPull` per extra pull counted | `Combat.ExtraPull` 1.0 |
| TeammateDeath | Cost | the guard ally fell: `-AllyDeath / w` | `Combat.AllyDeath` 1.0 |
| Hurt | Cost | `-Hurt * (non-hazard damage taken / max health)` | `Combat.Hurt` 0.2 |
| FireHurt | Cost | `-FireHurt * (hazard damage taken / max health)` | `Combat.FireHurt` 1.0 |
| StepCost | Cost | while a front-pack member is alive and in combat: `-Clock * decisionSeconds` | `Combat.Clock` 0.01 |
| Death | Cost | once per death: `-Death / w` | `Combat.Death` 2.0 |
| Away | Cost | `-Away * decisionSeconds` when `CombatDraw::AwayCharged`: dead, or alive and further than `AwayYards` from `FightPoint` while rejoining (`Clock.Rejoining`) or while the front pack fights | `Combat.Away` 0.02, `Combat.AwayYards` 30 |
| DamageDealt | Shaping | `Damage * step.Damage / mean max-health of the front pack` | `Combat.Damage` 0.3 |

Win/loss tier scaling thus follows principle "tier-scaled outcomes": outcomes times `w`, deaths divided by it. `Hurt`,
`FireHurt`, `ExtraPull`, `Away` and `Clock` are at full price (no cost ladder: the yaml says "full price from the
start"). At the clock's end (`timeIsUp && !Recorded`) the encounter also records the window: `if (combat.Counts)
_ladder.Record(layout, spec, tier, Won(Kills + Clears, Deaths), MaxTier)`, where `Won` is something taken down and no
death (`CombatDraw.h:143-146`). Whether `Reward` is reached on the final decision (so `Recorded` is set) is
UNVERIFIED: check the order of `Reward` and `IsTerminal` in `StageScenario::Reward`/`Observe`.

### Episode info columns

`won`, `survived`, `kills`, `packs_cleared`, `pulls`, `extra_pulls`, `interrupts`, `deaths`, `respawns`, `rises`
(identical values: both are `Clock.Rises`), `rejoins`, `rejoin_seconds`, `rejoined` (`RejoinedShare` when rises),
`dead_seconds`, `away_seconds`, `outcome_paid`, `interrupt_earnings` (`InterruptPaid / OutcomePaid`), `kill_seconds`,
`ally_deaths`, `hurt_share`, `fire_share`, `hazard_pulls`, `linked_pulls`, `caster_pulls`, `rest_seconds`,
`selected_share`, `target_in_view`, `start_walk`, `combat_rung`, `difficulty` (same value as `combat_rung`),
`at_top_rung` (`Tier >= MaxTier`), and the twelve `reward_*` columns. Read by the learner configs
`apps/forge/python/configs/combat1_fight.yaml` (headline `won, survived, kills, kill_seconds, hurt_share, deaths,
rejoin_seconds, combat_rung, target_in_view, selected_share, ally_deaths`; convergence measure `won`; fade gate `won >=
0.7`
with rungs [1.0, 0.5, 0.25, 0.0]), `combat2_packs.yaml` (`packs_cleared`, `extra_pulls`, `interrupts`,
`interrupt_earnings
<= 0.3`, `fire_share`; gate `won`), `combat3_survive.yaml` (convergence measure and fade gate `survived`; `rejoined >=
0.9`,
`rejoin_seconds <= 60`, `away_seconds`, `rest_seconds`). The `deaths`, `rises`, `rejoins`, `rejoin_seconds`, `rejoined`,
`dead_seconds` names are shared with PartyFollow, Roles and Instance.

### Config keys

All under `AnimusForge.Curriculum.` + `Combat.*` (defaults above and: `MaxTier` 5, `LevelBase` -2, `LevelsPerTier` 1,
`EliteTier` 4, `CasterTier` 1, `LinkedTier` 2, `HazardChance` 33, `SurviveSize` 3, `SurviveLevels` 2, `FightNearest` 28,
`FightFurthest` 40, `NextNearest` 30, `NextFurthest` 45, `NextFightMs` 2000, `CorridorWalk` 220, `CorridorSpacing` 8),
`Difficulty.*` (E2a.3) and `Respawn.DelayMs` 10000, `Respawn.RejoinYards` 15. Clamps and the conf-file agreement:
[cpp-tuning-keys.md](cpp-tuning-keys.md).

### Tests

`CombatPerceptionTest.cpp` (`CombatDrawTest.TheRungsPulls`, `OutcomesScaleWithTheRung`,
`TheCorridorPointsAreReachableAndApart`,
`CombatRespawnTest.AwayIsDeadOrOffFromTheFight`, `TheClockRisesAfterTheDelayAndRejoinsAtTheFight`,
`CombatStagesTest.*`);
no test drives the encounter against a world (`Build`, `SpawnPull`, `Reward` are not unit-tested); `RolesStageTest.cpp`
covers the shared stage setup.

### Reviewer notes

- C1's win is "something taken down and no death" (`Won`), the gate for the shaping fade and the DifficultyLadder alike.
- `Survived` pays only at the exact decision the clock runs out and only with zero deaths in the whole episode: a seat
  that dies once never gets it, even if it comes back and clears everything.
- Interrupt credit depends on `PendingInterrupt` being the same GUID as a caster in `StepInterruptedTargets`.
- Spawning uses `PathGenerator` per attempt (up to 24 attempts, each a path computation) on the map thread.

### Observed issues

1. `CombatEncounter.cpp:93-94`: `respawns` and `rises` report the same number.
2. `difficulty` and `combat_rung` columns duplicate each other (`CombatEncounter.cpp:127-128`).
3. `CombatDraw.h:CreatureLevel` clamps to 83 while the pools stop at `DEFAULT_MAX_LEVEL` (80).
4. `Build` silently leaves the seat where it was when the map has no corridor point (`:208-221`); `FindCorridors`
   logs only the count.
5. `StepCost` is paid under the name "Clock" in tuning (`Combat.Clock`) but is `RewardTerm::StepCost`, whereas the
   scenario's own `RewardTerm::CombatClock` (`Output.Clock`) is a different, shaping term.
6. `ResetEpisode` keeps `Packs` and `Ally` across the episode boundary (`:134-143`) so `Build` can despawn them with a
   map; a reset that never reaches `Build` (failed build) leaves creatures alive.

## E2a.3 DifficultyLadder (`DifficultyLadder.h` 100, `.cpp` 104)

An adaptive per-(class, build) rung used by CombatEncounter and RolesEncounter (`CombatEncounter.h:177`,
`RolesEncounter.h:148`; SightEncounter's comment at `SightEncounter.cpp:104` refers to it but does not use it).
`DifficultyLadder(scenario, what)` sizes `_tiers` as `Layouts().size() * MAX_SPECS` rows (`Row(layout, spec) = layout *
MAX_SPECS + min(spec, MAX_SPECS - 1)`); each row `{Tier, Fights, Wins}`; a mutex guards it ("envs finish on map update
threads"). Rungs start at 0 with the worldserver and are not persisted.

- `Draw(env, layout, spec, maxTier) -> {Tier, Counts}` (`.cpp:29-61`): an **evaluation** (`EpisodeSeedIndex !=
  NO_EPISODE_SEED`) plays `(seed / CastingCount()) % (maxTier + 1)` and never counts, so every (class, build) pair meets
  every rung. **Training** plays the current rung (capped to `maxTier`), counting; with `Difficulty.ReviewChance`
  (25) percent and a rung above 0 it plays a uniformly drawn lower rung and does not count; else with
  `Difficulty.StretchChance` (10) percent below the top it plays one rung up and does not count. (`roll_chance_i` is
  called on the world thread, per the header.)
- `Record(layout, spec, fightTier, won, maxTier)` (`:63-92`): ignored if the row's rung moved meanwhile; counts a fight;
  once `Fights >= Difficulty.Window` (200) it moves the rung up if `wins/fights >= RaiseAbove` (0.9) and below the top,
  or down if `< LowerBelow` (0.6) and above 0; resets the window and logs "<scenario>: <class> <spec> moves from <what>
  <a> to <b>".
- `Tier(layout, spec)` reads a row.

This ladder **does step back on the score**, unlike the gate-stepped fade and wing ladders in principle 10: it is the
older per-class pacing, kept for the combat and roles stages, and the learner also tracks the mean of the `difficulty`
column per class (`apps/forge/python/animus/stage.py:21,51` "the ladder settled"; see [py-mappo.md](py-mappo.md) and
[stages.md](stages.md)). Config: `Difficulty.RaiseAbove`, `LowerBelow`, `Window`, `ReviewChance`, `StretchChance`,
`CasterChance`, `TierScale`, `MaxTierScale` 6 (the last is not read by this class). Tests: no direct test file
(UNVERIFIED:
grep found none for `DifficultyLadder`).

Observed: the rung counters are lost on every worldserver restart, so a resumed stage restarts every class at rung 0
(the learner's `difficulty` column then drops); `Draw` uses `urand` and the ladder state is shared by all envs of the
pool, so training order matters for reproducibility.

## E2a.4 Opponents (`Opponents.h` 98, `.cpp` 387)

The curriculum's hostile creature pools, spawn-point finder and summoner, used by Combat, Roles and others.

- **`OpponentPool::Instance()`** (a function static; built on first call, which `WarmCaches()` forces before the DB
  pools are sealed, `WarmCaches.cpp:34`). The constructor (`.cpp:111-220`) runs two `WorldDatabase` queries over
  `smart_scripts` (SmartAI creatures with `ScriptName = ''` whose scripts are only spell casts (action 11) and talks
  (action
  1) on a list of combat events: "cast-only"; and their spells, to find which have a cast time (an interrupt can stop
  it) and which carry a persistent area aura or area aura effect: "hazard"), then walks
  `sObjectMgr->GetCreatureTemplates()`.
  A template qualifies when it is spawned in the world and not a waypoint walker (`WorldCreatures::SpawnedIds`,
  `WaypointWalkerIds`), of a fair type (beast, dragonkin, demon, elemental, giant, undead, humanoid), no npcflag, no
  vehicle, not non-attackable/immune/non-selectable/pacified, not civilian/trigger/guard, `ModHealth` and
  `DamageModifier` within
  0.5-2.0 (elite: health up to 3.0, damage up to 2.5), nonzero `minlevel`, and not `SpawnsUnreachable` (no ground
  movement, flying, rooted, hovering/flying/submerged anim tier, stealth or invisibility addon auras), and either
  default AI
  (no script, no AI name) or cast-only SmartAI. Normal-rank creatures go into `_packByLevel` for every level in their
  range, into `_byLevel` if default AI, `_castersByLevel` if cast-time, `_hazardCastersByLevel` if hazard; elite-rank
  into `_elitesByLevel`. Counts are logged.
- **Draws** `Random`, `RandomPackMember`, `RandomElite`, `RandomCaster`, `RandomHazardCaster` -> `PickNear(byLevel,
  level)`:
  the level itself then the nearest levels either side (lower first), uniform in the bucket; 0 if everything is empty.
- **`FindSpawnPoint(bot, map, min, max)`** (`:262-300`): up to 24 tries at a random bearing and distance; keeps a spot
  with ground (`GetHeight`) within 6 yd of the bot's height, `Walkable` (a `PathGenerator` path of normal type no longer
  than 1.5 times the straight line) and in line of sight (`IsWithinLOS` at +2 z); a walkable spot out of sight is the
  fallback; otherwise the last random attempt's position (possibly unvalidated, with `m_positionZ` possibly the bot's
  z).
  Random facing.
- **`FindSpawnPointFrom`** (`:302-319`): as above but from a point along a bearing within `spread`, no line-of-sight
  test; `nullopt` if none.
- **`SummonOpponent`** (`:321-350`): sets thread-local `PendingSummonLevel = level` around `map->SummonCreature(entry,
  pos)` (see `SummonLevel.h`), then puts the creature in the bot's phase mask, `FACTION_MONSTER`, `REACT_AGGRESSIVE`,
  home position, full health, `SetRegeneratingHealth(false)`. Returns the `Creature*` or null with `LOG_ERROR`.
- **`SpawnPack`** (`:352-387`): the first entry at the centre, each further one 2-5 yd (`PACK_SPREAD` = 5) away at a
  random
  angle, ground-snapped, random facing; failed summons are skipped.

Quirks: the pool is built from the world DB at startup and never refreshed; the query's `HAVING` clause is a
hand-written
description of "cast-only" scripts that nothing tests (UNVERIFIED: the world data it reads is outside the repo tests);
`PickNear` falls to other levels without telling the caller (a level-60 seat may get a level-1 pool entry if buckets are
empty); comments mention `stage1_duel`, `stage8_duel` and "the scripted baseline" (`.cpp:78-84`, `:333-341`), stale
since
the first curriculum was deleted. `Opponents.h:42` (`Random`) says "The duel stage's opponents".


## E2b. RolesEncounter, PartyEncounter, the stand-in and Standing

Files (all under `src/server/game/Animus/Scenario/Curriculum/Encounters/`): `RolesEncounter.h` (154),
`RolesEncounter.cpp`
(727), `RolesDraw.h` (239), `PartyEncounter.cpp` (541) with the class declaration in `Encounters.h:78-141`, `StandIn.h`
(161), `StandInSeat.cpp` (169, members of `StageScenario`), `Standing.h` (69). `DifficultyLadder.h/.cpp` (100 + 104)
is the rung ladder RolesEncounter owns; it is summarised in E2b.1.5 because the rung decides the episode.

### E2b.1 RolesEncounter (G1, `group1_roles`; `Opposition::Roles`)

**What it does.** One drill an episode on the cleared Ragefire Chasm (map 389). Seat 0 is the drilled role (the stage
draws its class and build among those whose spec plays it, `StageScenario::FitsDungeonRole`, see
[cpp-stagescenario.md](cpp-stagescenario.md)); the other four seats are a proper party in a core group
(`PartyEncounter`). The encounter spawns packs of creatures for the party to fight, pack after pack until the
episode clock ends it. It pays the drilled seat's lesson as its own Outcome term, plus party-wide Clear, Survived,
Death,
Away and clock terms.

**State** (`RolesEncounter.h:68-123`): per env `EnvRoles` (drill, rung `Tier`, `Counts`, layout/spec/level of the
drilled
seat, `Corridor` index, `Packs`, per-seat `SeatRoles` {`RespawnClock Clock`, `DeathPaid`, `Deaths`, `AwaySeconds`},
`AllDown`, this decision's `NewClears/NewClean/NewExtra`, episode `RolesDraw::Tally`, `Pulls`, `CleanClears`, `KeptUp`,
`KeptMembers`, `LowManaSeconds`); one shared `DifficultyLadder _ladder` (name "roles rung") and a mutex-guarded
per-map cache of corridor points (`_corridors`, `RolesEncounter.cpp:179-187`).

**Lifecycle, step by step.**

1. `ResetEpisode` (`:170-177`): keeps `Packs` (the creatures stay until `Build` clears them), zeroes everything else.
2. `Build(env, map, level)` (`:190-250`; the `level` argument is ignored). Fails (false) with no seat-0 bot or map.
   a. `Despawn(env)` removes the previous episode's packs. `roles = EnvRoles()`; `Drill = Arena(env).Roles`.
   b. Rung: `_ladder.Draw(env, layout, spec, Roles.MaxTier)` for the drilled seat's class and build (E2b.1.5);
      `Tier`, `Counts`, `Level = drilled.Level`.
   c. Seat 0 is teleported (`BotFactory::TeleportWithinMap`) to a corridor point (`CombatEncounter::FindCorridors`,
      cached per map id), index `EpisodeSeedIndex % corridors` in an evaluation, else `urand` (`:206-217`), random
      facing. If no corridor points exist seat 0 stays where it is.
   d. Each other seat up to `GROUP_SEATS`: teleported to `Opponents::FindSpawnPoint(lead, map, Roles.PartyNearest,
      Roles.PartyFurthest)` (2 to 6 yd, in seat 0's sight), random facing; every seat `PrepareFighter` and is stocked
      with
      food, and drink if it has mana (`StockConsumables`, `ConsumablePool`).
   e. First pack `SpawnPack(env, map, lead, nullptr)` (fail = build fails), then packs one after another from
      `roles.Packs.back().Spot` until `StandingPacks(drill, tier)` stand; `ListTargets` fills `env.Targets`
      (first `MAX_TARGETS` pack members).
3. `UpdateEnemies` (per decision, `:347-380`): per pack it reads whether any alive member is in combat
   (`Fight.Fighting`),
   first engagement stamps `EngageMs` and counts a `Pulls`; a `Linked` pack sends its idle members to attack the victim
   of a fighting member.
4. `Update` (per decision, `:382-424`): for each seat up to 5, `RespawnClock::Note(...)` with `Respawn.DelayMs` and
   `Respawn.RejoinYards`; on `Step::Rise` and `ArenaDefinition::RespawnAtEntrance`: `RiseAtEntrance(bot, seatState,
   SpawnPointFor(env), elapsed)` then `Clock.Risen`, `DeathPaid = false` (the I4 respawn clock, documented in
   [cpp-encounters.md](cpp-encounters.md) with EntranceRespawn). Then, only if seat 0 is alive, standing packs are
   topped up
   (`StandingPacks`), whatever happened while the party was down; a failed spawn is retried next decision. While seat 0
   is
   dead no new pack is placed.
5. `SelectTarget`, `OnSeatAction`: not overridden (the stage has the sight block, the seat selects by itself).
6. `View` (`:476-495`): fills only the gauntlet block's fields: `PullsCleared` (= `Tally.Clears`), `FoodItem`,
   `DrinkItem`
   (0 for a manaless seat), `GauntletSupplies = CONSUMABLE_COUNT`, and `PullTime` = min(1, ms since the first fighting
   pack engaged / 60000).
7. `BeforeRewards` (`:497-548`), once a decision: `NoteFights(packs)` (each fighting pack beyond the first is an
   extra pull, counted once a pack; every fighting pack stops being clean); a wipe (every seat dead) is counted on the
   edge (`AllDown`); a pack with no living member is a clear (clean if never fought beside another), erased from
   `Packs`;
   `ListTargets` again if any was removed. Cleared packs are replaced in `Update`.
8. `Reward` (per seat, see E2b.1.3).
9. `IsTerminal` (`:718-722`): only the clock: `EpisodeLengthMs && elapsed >= length`. Never a death or a wipe.
10. `WriteState`: `state[StageScenario::STATE_TIER] = Tier / max(1, Roles.MaxTier)`.
11. `Teardown`: `Despawn`. `Deactivate` is the default (calls `Teardown`).

**Pack plan (`RolesDraw::PlanPack`, `RolesDraw.h:62-73`).** At rung `t`: level offset over the party's level
`Roles.LevelBase + t * Roles.LevelsPerTier / 2` (integer division; defaults -1 + t/2); size `min(PackSizeMax,
PackSizeFirst + t / max(1, PackGrowEvery))` (2, +1 every 2 rungs, max 4); a caster from `CasterTier` (1), linked from
`LinkedTier` (2), an elite from `EliteTier` (4); creature health `KeepHealthPct` (200) of normal for `Keep` only, else
100.
`SpawnPack` (`:253-319`) builds the entry list: the elite first (`OpponentPool::RandomElite(level)`), then a caster
(`RandomCaster`), the rest `RandomPackMember`; creature level `CombatDraw::CreatureLevel(roles.Level, pull)`; place:
the first pack `Opponents::FindSpawnPoint(lead, map, Combat.FightNearest, Combat.FightFurthest)` (28 to 40 yd, in seat
0's
sight); later ones `FindSpawnPointFrom(lead, map, previousSpot, bearing, NEXT_SPREAD = 1.0 rad, spacing, spacing +
10)` along the bearing from seat 0 through the previous pack (spacing `PackSpacing`: for `Pull`, linear from
`CampSpacingFirst` 45 yd at rung 0 to `CampSpacingLast` 25 yd at `MaxTier`; for other drills `Combat.NextNearest`, 30);
the stats modifier `ApplyStatPctModifier(UNIT_MOD_HEALTH, TOTAL_PCT, pct - 100)`, `UpdateMaxHealth`, `SetFullHealth` for
`Keep`.
How many stand: `StandingPacks` = 2 for every drill but `Pull`, where it is `max(2, min(CampPacksMax, CampPacksFirst +
t/2))`
(2 to 4). The "next" pack standing in reach is what makes an extra pull possible.

**Drill = what each drill is** (all by `ArenaDefinition::Roles`, seat 0 must hold `DungeonRole == DrilledRole(drill)`,
else no drill term is paid, `Reward` `drilledSeat`):

| Arena | Drill | Drilled role | Episode s | What the lesson pays |
|---|---|---|---|---|
| `tank_hold` | Hold | tank (1) | 240 | per enemy on the seat, less per enemy on another party member |
| `heal_keep` | Keep | healer (2) | 300 | per member above half health, less per member under 35% or dead, less waste |
| `damage_discipline` | Focus | damage (3) | 240 | damage on the tank's target, less per enemy it holds |
| `pull` | Pull | tank (1) | 360 | Hold terms too, plus PullClean per clean pack; PullExtra when a second pack is drawn in |

**E2b.1.3 Rewards paid (`Reward`, `RolesEncounter.cpp:550-709`).** Term categories from `RewardTermCategory`
(`RewardLedger.h:162-243`). `w = 1 + Difficulty.TierScale * Tier` (`RolesDraw::TierWeight`; key `Difficulty.TierScale`),
`scale = StageScenario::DecisionScale()`, `decision = DecisionMs / 1000`. Losses are divided by `w`, earnings
multiplied.

| Term (episode column `reward_<name>`) | Kind | Who | Amount | Key (default) |
|---|---|---|---|---|
| `Clear` | Outcome | every seat | `Roles.Clear * w * newClears` | `Roles.Clear` (1.0) |
| `PullClean` | Outcome | Pull drill only: `Roles.PullClean * pullShare * w * newClean` | puller (drilled seat) pullShare 1, others `Roles.PullOthers` | `Roles.PullClean` (2.0), `Roles.PullOthers` (0.5) |
| `PullExtra` | Cost | every drill, every seat | `-Roles.PullExtra * pullShare * newExtra / w` (pullShare 1 for the Pull drill's drilled seat, else `PullOthers`; so in non-Pull drills it is `PullOthers`) | `Roles.PullExtra` (1.5) |
| `DrillHold` (Hold, Pull) | Outcome (both signs) | drilled seat, alive | `+Roles.Hold * onSeat * scale * w` and `-Roles.Loose * onOthers * scale / w`; `onSeat` = living enemies in combat whose victim is the seat, `onOthers` = victim is another player or pet | `Roles.Hold` (0.045), `Roles.Loose` (0.018) |
| `DrillFocus` (Focus) | Outcome (both signs) | drilled seat | `+Roles.Focus * (LastStepDamage if the seat's victim == the tank's victim, else 0) * w`; `-Roles.PulledOff * onSeat * scale / w` | `Roles.Focus` (0.9), `Roles.PulledOff` (0.012) |
| `DrillKeep` (Keep) | Outcome (both signs) | drilled seat, only while a fight is on | `+Roles.Keep * up * scale * w`; `-(Roles.KeepLow * low * scale + Party.TeammateHealing * Roles.Overheal * wasted) / w` | `Roles.Keep` (0.0006), `Roles.KeepLow` (0.0006), `Roles.Overheal` (0.5), `Party.TeammateHealing` (2.0) |
| `StepCost` | Cost | every seat while any pack is fighting | `-Roles.Clock * decision` | `Roles.Clock` (0.01 per s) |
| `DamageDealt` | Shaping | every seat that did damage | `Roles.Damage * stepDamage / (mean max health of standing creatures)` | `Roles.Damage` (0.3) |
| `Death` | Cost | once per death of a seat | `-Roles.Death / w` | `Roles.Death` (2.0) |
| `Away` | Cost | per decision, when `CombatDraw::AwayCharged(alive, rejoining, fighting, yardsFromFight, Roles.AwayYards)` | `-Roles.Away * decision` | `Roles.Away` (0.02/s), `Roles.AwayYards` (30) |
| `Survived` | Outcome | at the episode's end if the seat never died and is not currently out | `Roles.Survived * w` | `Roles.Survived` (1.0) |

Detail the code shows: `wasted` = `(HealingRaw - effective) / MaxHealth` where `effective = SelfHealing + AllyHealing +
sum
AgentHealingBy`; `up`/`low` are counted over all seats up to 5 that have a layout (alive and >50% health is up; dead or
<35% is low, `RolesDraw::Count`); Hold/Focus/Keep readings (`Tally.Held`, `OnParty`, `Damage`, `FocusDamage`,
`PulledOffSeconds`, `KeptUp`, `KeptMembers`, `LowManaSeconds`) are taken in the same branch, so a seat that is not the
drilled one (wrong role) contributes no reading. The death branch also sets `state.Combat.Died/DeathCounted/DeathMs` and
`++Deaths`. `Death`'s `1/w` and `PullExtra`'s `1/w`: harder rungs charge less per miss.
`Death` is paid once per `DeathPaid`; the flag is reset only by a rise in `Update` (`Update`, `RolesEncounter.cpp:406`),
so a seat that dies,
rises, dies again is charged twice.

**Won (`RolesDraw::Won`, `RolesDraw.h:~212-232`)**: at least one pack cleared and no wipe, and by drill: Hold
`HoldShare >= Roles.WinHold` (0.75; Held / OnParty); Keep `PartyDeaths == 0`; Focus `FocusShare >= Roles.WinFocus`
(0.6) and `PulledOffSeconds <= Roles.WinPulledSeconds` (5); Pull `ExtraPulls == 0`.

**E2b.1.5 Rung ladder (`DifficultyLadder`).** One ladder per encounter (roles: the drilled seat's class and build).
Per (class layout, spec) row, top rung `Roles.MaxTier` (5). Training `Draw` (`DifficultyLadder.cpp:30-60`): current
tier, but
with `Difficulty.ReviewChance` percent a random lower tier (not counted) and with `Difficulty.StretchChance` the next
tier up (not counted); `Counts` true only for a fight at the pair's own tier. Seeded evaluation: tier =
`(EpisodeSeedIndex /
CastingCount) % (MaxTier + 1)`, `Counts` false. `Record` (at the episode's end, once, from `Reward` on the drilled seat
when
time is up and `Counts`, `:700-708`): after `Difficulty.Window` counted fights the pair moves up at win rate `>=
RaiseAbove`
or down below `LowerBelow`; a log line when it moves. Mutex-guarded; starts at 0 with the worldserver (not saved across
a
restart; UNVERIFIED: whether `Tier` state is restored on `resume`). `Draw`'s review roll comment says world thread;
`Record`
runs on map threads. Keys: `Difficulty.*` (see [cpp-tuning-keys.md](cpp-tuning-keys.md)).

**Episode-info columns written** (`AddEpisodeInfo`, `RolesEncounter.cpp:69-168`; names exact; the yaml
`apps/forge/python/configs/group1_roles.yaml` reports and heads most of them):
`won`, `drill_hold`, `drill_keep`, `drill_focus`, `drill_pull` (1 if the arena's drill), `won_hold`, `won_keep`,
`won_focus`,
`won_pull`, `hold_share`, `kept_share` (KeptUp / KeptMembers), `low_mana_seconds`, `focus_share`, `pulled_seconds`,
`clean_share` (CleanClears / Clears), `packs_cleared`, `clean_pulls`, `extra_pulls`, `pulls`, `party_deaths`, `wipes`,
`rises`, `rejoins`, `rejoin_seconds` (mean over rejoins), `rejoined` (rejoins / rises), `dead_seconds`, `away_seconds`
(party sums), `roles_rung` (= Tier; the eval videos read it), `difficulty` (= Tier; the learner's per-class ladder
tracking), `at_top_rung`. Plus 11 `reward_*` columns for `RewardTerms()` (`RolesEncounter.cpp:62-67`): Clear, DrillHold,
DrillKeep, DrillFocus, PullClean, PullExtra, Survived, Death, Away, StepCost, DamageDealt. All are per-env totals read
for every seat (the same number on every seat's row). The yaml also reports `with_stand_in`, `tank_hold_share`,
`group_kept_share`, `tank_target_share`, `teammates_died`, `died`, which come from the stand-in, PartyEncounter and the
core columns.

**Config keys** (all under the tuning prefix; defaults in `CurriculumTuning.h:787-826`, Visit at `:1127-1163`):
`Roles.Clear 1`, `Hold 0.045`, `Loose 0.018`, `Focus 0.9`, `PulledOff 0.012`, `Keep 0.0006`, `KeepLow 0.0006`, `Overheal
0.5`, `PullClean 2`, `PullExtra 1.5`, `PullOthers 0.5`, `Survived 1`, `Death 2`, `Away 0.02`, `AwayYards 30`, `Clock
0.01`,
`Damage 0.3`, `MaxTier 5`, `LevelBase -1`, `LevelsPerTier 1`, `PackSizeFirst 2`, `PackGrowEvery 2`, `PackSizeMax 4`,
`CasterTier 1`, `LinkedTier 2`, `EliteTier 4`, `KeepHealthPct 200`, `CampPacksFirst 2`, `CampPacksMax 4`,
`CampSpacingFirst 45`, `CampSpacingLast 25`, `PartyNearest 2`, `PartyFurthest 6`, `WinHold 0.75`, `WinFocus 0.6`,
`WinPulledSeconds 5`, `StandInShare 20`. Also read: `Combat.FightNearest/FightFurthest/NextNearest`, `Respawn.DelayMs`,
`Respawn.RejoinYards`, `Difficulty.*`, `Party.TeammateHealing` (PartyEncounter).

**Tests**: `RolesStageTest.cpp` (stage defined; packs by rung; the pull camp; the drill terms are outcomes and costs;
Hold,
Keep, Focus, Pull outcomes; a death rises at the entrance and rejoins; a wipe rises together), `StandInTest.cpp`.

**Quirks / debts.**
- A comment in `RolesEncounter.h:47` names a `Roles.StandInShare` percent of training episodes; it is real
  (`StandInSeat.cpp:84-90`) but is consulted only when the arena's own share is < 0, and the live roles arenas set none.
- `Build` ignores `level`; the pack level derives from the drilled seat's `Level`.
- `roles.Counts` / `Recorded` mean a ladder window sees one result per episode, at the clock's end only. An episode that
  never reaches its clock (a build failure, a reset) records nothing.
- `Reward` computes `FindTargetUnit` loops per seat per decision (O(seats * targets)).
- DrillHold/Focus/Keep are Outcome-category terms that carry negative amounts (the losses). `ScoresOutcome` includes
  Cost,
  so the effect is only on accounting by category (learner's outcome/cost split): the "Cost" part of a drill is counted
  under Outcome.
- The drilled seat's class/build constraint: "a makeup that could not fit it pays no drill" (`:615`), silently.

### E2b.2 PartyEncounter (the core group and the party's role shaping)

`PartyEncounter(scenario, envs)` is created for any stage with an arena `PartyGroup` (`StageScenario.cpp:429-430`).
Used live by group1_roles and the four dungeon stages. Lifecycle:

- `BeforeRebuild` -> `Disband`: `Group::Disband(true)` of the env's group (`PartyEncounter.cpp:179-183`, `:221-230`).
- `Build` (`:185-219`; returns true always): every seat from 1 gets seat 0's faction; creates a `Group` with
  `SetSimGroup(true)` (a sim group: only in memory), `Create(leader = seat 0)`, `sGroupMgr->AddGroup`, `AddMember` for
  each other seat. A failed create/add is logged (`LOG_ERROR`) and the build still succeeds (without a group).
  Skipped if the env still has a `PartyGroup`.
- `View` (`:232-262`): fills the seat's `view.Teammates` slots with its group's other seats (the group = seats `seat/5*5
  ..
  +5`), and `view.Tank = Tank(env)`.
- `Reward` (`:264-388`) and `RewardRole` (`:406-536`), `Teardown` (Disband).
- `Tank(env)` (`:~135-165`): the living seat whose `IsTank` (stage-drawn `DungeonRole == DUNGEON_TANK`, or role-less and
  a
  build that holds the pull) has the most `Apt[MITIGATION]`; with no real tank, the living seat with the most
  mitigation.

**Terms** (`RewardTerms()`: TeammateDamageTaken, TeammateHealing, TeammateThreat, TeammateDeath, Threat, Revive,
DamageDealt, Stall, EarlyPull; additionally paid but not listed: DrillHold, DrillFocus, DrillKeep, so in a stage with a
non-roles `DrillRole` arena their columns exist only via another encounter; no live stage has one). Categories:
TeammateDamageTaken, TeammateHealing, TeammateThreat, Threat, Revive, DamageDealt, Stall are Shaping; TeammateDeath is
Cost,
EarlyPull is Cost, Revive Shaping (`RewardLedger.h:162-243`; note: TeammateDeath, EarlyPull listed Cost). Formulas
(`scale = DecisionScale`, `Party.*`, `Raid.*`):

| Term | When | Amount | Keys (default) |
|---|---|---|---|
| `Revive` | seat resurrected an ally this decision | `+Resurrection.ReviveAlly` | `Resurrection.ReviveAlly` (1.5) |
| `Threat` | a non-tank, alive, with a living tank in the party, per enemy on it | `-Party.PulledThreat * onBot * scale` | `Party.PulledThreat` (0.004) |
| `TeammateDamageTaken` | per other seat that is not a tank: damage it took this step | `-(Protects(apt) ? Party.TeammateDamageTakenProtector : Party.TeammateDamageTakenDps) * taken / teammateMaxHealth` | 1.0 / 0.5 |
| `TeammateHealing` | seat's build `Heals` | `healDrill * HealShare * Party.TeammateHealing * (AgentHealingBy + AgentProtectionBy of that teammate) / health` | `Party.TeammateHealing` (2.0), `Party.HealOffGoal` (1.0) |
| `TeammateThreat` | the seat is the tank: per enemy on a non-tank living teammate | `-Party.TankLoseTeammate * onTeammate * scale` | 0.02 |
| `TeammateDeath` | first time the seat sees a teammate dead (resets when it lives) | `-Party.TeammateDeath` | 3.0 |
| `Threat` (stance) | tank in combat in Defensive Stance, Bear or Dire Bear Form, or with Righteous Fury (25780) or Frost Presence (48263) | `+Raid.TankStance * scale` | `Raid.TankStance` (0.001) |
| `Threat`/`DrillHold` (hold) | tank | `+Raid.TankHold * onBot * scale` and `-Raid.TankLoose * onOthers * scale` | 0.015, 0.006 |
| `DamageDealt`/`DrillFocus` (focus) | non-tank non-healer with a living other tank | `+Raid.TankTarget * LastStepDamage` if its victim is the tank's victim; `-Raid.PulledOff * onBot * scale` (term `Threat`/`DrillFocus`) | 0.3, 0.004 |
| `EarlyPull` | non-tank with enemies on it while the party tank is alive and not in combat | `-Raid.EarlyPull * onBot * scale` | 0.01 |
| `TeammateHealing`/`DrillKeep` (keep) | healer: members of its group alive >50% (+1) or <35% (-1) | `+Raid.KeepUp * keptPay * scale` (above-half share at `HealShare`) and `-Party.TeammateHealing * Raid.Overheal * wasted` | `Raid.KeepUp` (0.0002), `Raid.Overheal` (0.5) |
| `Stall` | in combat, an enemy within `Raid.IdleReach` (40 yd), nothing done for `Raid.IdleMs` (4000) | `-Raid.Idle * scale` per decision | 0.001 |

(The drill weight `Raid.DrillWeight` and the DrillHold/Focus/Keep term switch were deleted 2026-10-08: only roles arenas set
`DrillRole`, and they pay the drilled seat in `RolesEncounter`.) In a roles arena the drilled seat's role terms are not paid here (`rolePay = 0`, `:~432`); the readings are still taken.
`HealShare` is 1 under a Protect goal or no goal, else `Party.HealOffGoal` (`:~369-379`).

**Episode info (PartyEncounter's columns, `AddEpisodeInfo`, `:41-114`)**: `seat`, `teammates_died`, `revives`,
`teammate_damage_taken`, `teammate_healing`, `group_kept_share`, `healing_coverage`, `threat_on_teammates`,
`idle_seconds_in_combat`, `tank_hold_share` (the same on every seat: the tank's EnemiesHeld / EnemiesOnParty, tank = the
seat
with most EnemiesOnParty), `tank_form_share`, `tank_target_share`, `pulled_off_seconds`. Python readers:
`apps/forge/python/animus/config.py` (role metrics) and `episode_means.py`; group1_roles and the four dungeon yamls list
these. 

**Enemy ranking.** `EnemyRank {TankTarget, OnPlayer, Fighting, Standing, Gone}` and `RankEnemy(enemy, tank)` are
declared at
`Encounters.h:65-74` and defined in `InstanceEncounter.cpp:57`, used for the enemy slot order at
`InstanceEncounter.cpp:1907`. They order enemy
slots for the pack/crowd blocks.

**Quirks.** `Build` dereferences `SeatBot(env, seat)` for every active seat without a null check (`:206-207`:
`SeatBot(env, seat)->SetFaction`), and `lead` likewise; the group is skipped (and the build still succeeds) if creation
fails, leaving later code to find no group. `Tank()` iterates `SeatCount()` (not `ActiveSeats`).
`IsTank`/`Heals`/`Protects`
read the build's `Aptitude`, which differs from what `FitsDungeonRole` drew in role-less arenas. The `TeammateHealing`
pay is for every seat whose build heals, not only the drawn healer. Comments in the file mention the "Deadmines'
parties", "stage6" and 2026-10 dates that no longer match any live stage.

### E2b.3 The "human" stand-in (`StandIn.h`, `StandInSeat.cpp`)

**What it is.** A party seat whose row the learner plays with a frozen partner policy from its co-op partner pool, never
trained on; the sim only chooses and marks the seat. It is not scripted (principle 14). The sim marks it with `present =
2`
(protocol 25): `StandIn::Presence(hasCharacter, isStandIn)` -> 0 none, 1 learner, 2 stand-in (`StandIn.h:53-62`,
used in `StageScenario::AgentPresence`, `StageScenario.cpp:3475-3481`, and the `present` episode-info column,
`StageScenario.cpp:828-831`, which excludes the stand-in's seat). It is absent when the learner has no partner
(`MODE_FLAG_STAND_IN` unset): `StageScenario::SetStandIn(bool)`
(`StandInSeat.cpp:~58-65`) stores an atomic flag (relaxed) and logs the change.

**API (pure, `Animus::Curriculum::StandIn`)**: `enum Role {Tank, Healer, Damage}` with `ROLE_NAMES`; `Presence`;
`Fields(modeAllows, partyOrRaid, activeSeats, evaluating, share, roll)` (false unless party, >= 2 active seats and the
mode flag;
then true in every evaluation, and in training with `roll(share)` only if `share > 0`, so a stage without a share draws
no
random number); `Tuning {Share 0, LeadChance 50, TankChance 34, HealerChance 33}`; `Rng` (splitmix64: `Next`, `Unit`,
`Chance(percent)`, `Between(lo, hi)`); `EvaluationSeed(seedIndex)`; `Style {Seed, Leads, Wanted}`; `Draw(seed, tuning,
canLead)` (leads = `canLead && Chance(LeadChance)`; wanted role from one uniform: < TankChance tank, < Tank+Healer
healer, else damage); `RoleFor(wanted, canTank, canHeal)` (the wanted role if the build can, else damage).

**Scenario members (`StandInSeat.cpp`)**: `StandInSeat(env)` (the chosen seat or -1), `StandInLeads(env)`,
`StandInShare(arena)` (the arena's own `Arena.<stage>.<arena>.StandInShare`, clamped -1..100, if >= 0; else for a Roles
arena
`Roles.StandInShare` (20); else `StandIn.Share` (0)), `DrawStandIn(env)` (called at the end of `Rebuild`,
`StageScenario.cpp:2383`),
`AddStandInEpisodeInfo()` (`:2` only if any arena is Party).

`DrawStandIn` step by step: reset `StandInPlay`; ask `Fields(...)` with `_standIn`, party seats, `ActiveSeats`,
`env.Evaluating`, `StandInShare(arena)`, `roll_chance_i`; seed = `EvaluationSeed(EpisodeSeedIndex)` in an evaluation
else two
`rand32()`; style = `Draw(seed, _tuning.StandIn, canLead = !arena.DrillRole)`; if it leads, seat 0 is chosen (must have
a
layout and a bot); else among seats 1.. with a layout and a bot, prefer those whose build fits the wanted role
(`FitsRole`:
tank = `DungeonRole == TANK` or role-less + holds the pull + taunt > 0; healer similarly with `KeepsThemUp`; damage =
neither),
else any, picked with `Rng(seed ^ 0x5EA7)`; record `StandInPlay {Seat, Style, Role = RoleFor(wanted, CanTank,
CanHeal)}`; a
`LOG_DEBUG` line. Who reads it: `InstanceEncounter.cpp:2509, :2595` (`WingRun::LeaderSeat(StandInSeat, StandInLeads)`: a
leading stand-in is the party's leader), InstanceEncounter's `clear_standin`/`clear_allbot`/`standin_gap` columns
(`:314-322`).

**Episode-info columns** (`AddStandInEpisodeInfo`): `with_stand_in` (1 when one played), `stand_in_leads`,
`stand_in_role`
(1 tank, 2 healer, 3 damage, 0 none). Every row reports them (the stand-in's own row is not reported).

**Config keys**: `StandIn.Share` (0), `StandIn.LeadChance` (50), `StandIn.TankChance` (34), `StandIn.HealerChance` (33)
(Visit
`CurriculumTuning.h:1170-1173`), `Roles.StandInShare` (20), per-arena `Arena.<stage>.<arena>.StandInShare`; the live
dungeon
arenas set `StandInShare = 20` in their definition (`Stages.cpp:621`). **Tests**: `StandInTest.cpp` (determinism per
seed,
style coverage, role within the build, present = 2, no stand-in without the mode flag, evaluation vs training share).

**Quirks.** `LeadChance` comment says "only where the party has no owner" (`StandIn.h:92`); the code allows leading
unless the
arena has a `DrillRole` (an owner is not consulted). `canLead` false skips no random number (the `Chance` is still
drawn),
good for determinism. `Fields` treats `partyOrRaid` as `arena.Seats == Party`; a Party-seat arena of 1 active seat never
has one.
The seat choice loop starts at index 1, so a follower never sits in seat 0. `StandIn.h` header comment says the leading
stand-in sits in seat 0 "which PartyEncounter makes the group's leader" - true (`PartyEncounter::Build` leader = seat
0).
`StageScenario.cpp:826-831` makes the `present` info column 0 for the stand-in's seat, so no class's episode counts it.

### E2b.4 Standing (`Standing.h`, 69 lines)

Three pure inline functions in `Animus::Curriculum::Standing`, shared by the movement encounters (Sight, Seek, Interact,
PartyFollow; call sites `SightEncounter.cpp:371,404`, `SeekEncounter.cpp:489,542`, `InteractEncounter.cpp:604,648`,
`PartyFollowEncounter.cpp:651,664`):

- `WallCharge(wallSeconds, moved, asked, price, slideShare)`: 0 if `wallSeconds <= 0` or `price <= 0`; ratio =
  clamp(moved /
  asked, 0..1) (0 if asked < 1e-4); blocked = clamp((share - ratio)/share, 0..1) with share = clamp(slideShare, 0..1) (0
  if
  share is 0); result `price * wallSeconds * blocked`. Sliding along a wall with good progress is free.
- `Stopped(movementFlags, movedYards, stopMoved)`: true when none of `MOVEMENTFLAG_MASK_MOVING | SWIMMING | FLYING` is
  set
  and the unit moved under `stopMoved` yards since the last decision. Turning in place does not count.
- `Band(distance, bandMin, bandMax, lostYards)`: 0 too close, 1 in band (`<= bandMax`), 2 behind (`<= lostYards`), 3
  lost.

Tests: `StandingTest.cpp` (`WallChargesOnlyTheGroundNotCovered`, `StoppedReadsTheServersFlags`, `FollowBands`); its
fourth
test, `CourseKinksReadTheWayBetweenTicks`, tests `StageScenario::CourseKink`, not this header. The header comment lists
"seek,
sight, interact, the party follow" as readers: accurate per the call sites above. Keys that supply the parameters are in
each
movement encounter's section (E1, E2).

### E2b.5 Observed issues (this fragment)

1. `RolesEncounter.h:47` mentions a `Roles.StandInShare` percent; its only effect is as a fallback
   (`StandInSeat.cpp:88-90`),
   and every live Roles arena (`Stages.cpp:899-910`) leaves `StandInShare` at -1, so the key is the live G1 share (20).
2. `RolesEncounter.cpp:235-236`: `Build`'s `level` parameter unused; the level is from the drilled seat.
3. `RolesEncounter.cpp:432`: no pack top-up while seat 0 is dead; a party whose seat 0 is dead and the rest alive faces
   only
   what is left, and the clock continues.
4. `RolesEncounter.cpp:411` (`DeathPaid` reset only on the entrance rise): a seat that dies twice is charged `Death`
   twice, a
   wipe counts via `AllDown` separately; the per-seat `Deaths` and `PartyDeaths` double count nothing, but `Survived`
   checks
   only `seat.Deaths == 0`.
5. `RolesEncounter.cpp:615`, `:624-625`: drill terms silently not paid when seat 0's `DungeonRole` does not equal the
   drill's
   role; the stage relies on the draw (`FitsDungeonRole`) to guarantee it. UNVERIFIED how often it fails to fit.
6. `Drill*` terms are Outcome-category terms carrying negative parts (losses), see `RewardLedger.h:171-175`, so learner
   outcome/cost splits include costs in "outcome".
7. `PartyEncounter.cpp:206-207`: `SeatBot(env, seat)->SetFaction` and `lead->GetFaction()` are dereferenced without a
   null
   check, unlike the rest of the file.
8. `PartyEncounter.cpp:212-215`: a failed `Group::Create` still returns true; downstream group-based features (party
   frames
   `FillFromGroup`) then see no group.
9. `Encounters.h:48,55`: `<mutex>` included twice; `Encounters.h` includes headers for encounters it does not declare
   (`Battleground`, `WingRun.h`, `RouteShortcut.h`).
10. `PartyEncounter.cpp:381-385` and `:392-398`: the `drilled`/`rolesDrilled` logic for `DrillHold/Focus/Keep` is the
    pre-G1 drill path; no live non-roles arena sets `DrillRole`, so the non-roles branch of `drill`, `drilled`,
    `holdTerm` and
    `healDrill` (`:290-293`) is dead in the live curriculum (UNVERIFIED: check no dungeon arena sets `DrillRole`;
    `Stages.cpp`
    shows none).
11. `StandIn.h:92` comment about "owner" is stale (no live stage has an owner except the follow leader, which has no
    stand-in).
12. `Standing.h` header comment and `StandingTest.cpp:63` mix `StageScenario::CourseKink` into a file named for
    Standing.h.
13. `PartyEncounter.cpp` reward loops read `env.Targets` with `FindTargetUnit` per seat per teammate per decision
    (quadratic
    in seats).


## E3. InstanceEncounter (first half) and InstanceBosses

Scope of this fragment: `src/server/game/Animus/Scenario/Curriculum/Encounters/InstanceEncounter.cpp` lines 1-1400
(the other half, 1400-2752, is documented in the neighbouring fragment), the class declaration in `Encounters.h:147-467`
and `InstanceBosses.h` / `InstanceBosses.cpp`. Paths below are relative to
`src/server/game/Animus/Scenario/Curriculum/Encounters/` unless they start with another directory. The reward, terminal
and critic-state code lives after line 1400 (`Reward` 2529-2693, `SelectTarget` 2695, `TierScale` 2723, `IsTerminal`
2745); it was read for this fragment so that the lifecycle and the reward table are complete, but the neighbouring
fragment is the authority for everything after line 1400.

### E3.0 Map of the files

| Path | Lines | Role |
|---|---|---|
| `InstanceEncounter.cpp` | 2752 | The whole-dungeon encounter (`Opposition::Instance`, `InstanceLadder::Wing`): full runs, corridors and pull drills. |
| `InstanceBosses.h` | 62 | `BossRow`, `WingBoss` and the three accessors (`InstanceLadderRows`, `FollowBosses`, `WingBosses`). |
| `InstanceBosses.cpp` | 116 | The data: the follow stage's eight bosses, the three wing rows, the per-boss measure table. |
| `Encounters.h` (`InstanceEncounter` at 147-467) | 470 | Declaration of `InstanceEncounter` with its per-env structs (`EnvInstance`, `SeatInstance`, `WingPlan`, `WingPack`). |

Logical parts of `InstanceEncounter.cpp` 1-1400:

| Lines | Part |
|---|---|
| 19-56 | includes |
| 57-67 | `RankEnemy` (a free function that belongs to `Encounters.h:60-72`'s `EnemyRank`; unrelated to the instance) |
| 69-138 | anonymous namespace: constants, `KeyOf`, `CanUse`, `GameObjectsNearPoint`, `EngageKey`, `Distance2d` |
| 140-179 | constructor: which boss rows the world database can field |
| 181-188 | `FindSpawn` |
| 190-195 | `RewardTerms()` |
| 197-399 | `AddEpisodeInfo()` (every column) |
| 401-449 | `ResetEpisode()` (end-of-run logging and ladder bookkeeping, then the state wipe) |
| 451-514 | `BeforeLevel()` (pinned row, level, difficulty, spawn at the door); 516-522 `Rows()` |
| 524-576 | `FindBoss()` |
| 578-695 | `Build()` |
| 697-700 | `UpdateEnemies()` (forwards to `UpdateWingEnemies`, second half) |
| 702-960 | `Update()` (per-decision bookkeeping) |
| 962-1024 | `RiseDead()` (I4, the respawn clock) |
| 1026-1165 | `TraceWing()` and `LogWipe()` (tank choice, crowd measure, wipe log) |
| 1167-1226 | `Hostile()`, `Usable()`, `KeyItems()` |
| 1228-1240 | `DungeonLevels()` |
| 1242-1465 | `WingRoute()` (boss order, navmesh route, full-clear packs; continues past 1400) |

### E3.1 InstanceBosses: the boss tables

**What it is.** Pure data in two anonymous-namespace vectors (`FOLLOW`, `WING`) and a function-local static in
`WingBosses()`, behind three accessors (`InstanceBosses.h:49-61`). Nothing in it is a position: where a boss stands
comes from its spawn in the world database (`FindSpawn`, `InstanceEncounter.cpp:181-188`).

**Types.**

- `struct BossRow { uint32 MapId = 0; uint32 Entry = 0; uint8 Level = 60; uint8 Difficulty = 0; char const* Name = "";
  }`
  (`InstanceBosses.h:32-39`). `Difficulty` is a `DUNGEON_DIFFICULTY_*` / `RAID_DIFFICULTY_*MAN_NORMAL` value.
- `struct WingBoss { uint32 MapId = 0; uint32 Entry = 0; char const* Name = ""; }`.

**Accessors.**

- `InstanceLadderRows(InstanceLadder)` (`InstanceBosses.cpp:105-113`): `Wing` returns the `WING` table, `None` an empty
  vector.
- `FollowBosses()` (`InstanceBosses.cpp:115-116`): the `FOLLOW` table; its users are `PartyFollowEncounter.cpp:221,297`,
  not this encounter.
- `WingBosses()` (`InstanceBosses.cpp:68-101`): function-local static vector.

**The WING table (`InstanceBosses.cpp:41-55`).** Row index = `ArenaDefinition::InstanceRow`.

| Row | Map | Boss entry | Level | Difficulty | Name |
|---|---|---|---|---|---|
| 0 | 389 (Ragefire Chasm) | 11519 | 16 | 0 | "Ragefire Chasm to Bazzalan" |
| 1 | 36 (Deadmines) | 639 | 20 | 0 | "the Deadmines to Edwin VanCleef" |
| 2 | 43 (Wailing Caverns) | 3673 | 20 | 0 | "Wailing Caverns to Lord Serpentis" (held out) |

The row's `Level` is only the fallback level: `DungeonLevels` (E3.9) normally supplies the range.

**The FOLLOW table (`InstanceBosses.cpp:17-26`).** Eight rows. Ragefire (map 389, level 15): 11517 Oggleflint, 11520
Taragaman the Hungerer, 11518 Jergosh the Invoker, 11519 Bazzalan. Deadmines (map 36, level 20): 644 Rhahk'Zor, 643
Sneed, 1763 Gilnid, 639 Edwin VanCleef. Used by the PartyFollow stage; listed here because it shares the header.

**The WingBosses table (`InstanceBosses.cpp:68-98`).** Names are the suffixes of the `boss_<name>` episode-info columns.

| Map | Entry : column suffix |
|---|---|
| 389 | 11517 oggleflint, 11520 taragaman, 11518 jergosh, 11519 bazzalan |
| 36 | 644 rhahkzor, 642 sneed_shredder, 643 sneed, 1763 gilnid, 646 smite, 647 greenskin, 645 cookie, 639 vancleef |
| 43 | 3671 anacondra, 3669 cobrahn, 3653 kresh, 3670 pythas, 3674 skum, 5775 verdan, 3673 serpentis |

Tests: `DungeonStagesTest.EveryBossOfTheDungeonsIsMeasured` (`src/test/server/game/Animus/DungeonStagesTest.cpp:374`)
and
`WailingCavernsIsNeverDrawnInTraining` (`:180`) read these tables.
UNVERIFIED: what exactly the first test asserts against the table (read `DungeonStagesTest.cpp:374-401`).

**Invariants / contracts.**

- A WING row whose creature template, boss spawn or map entrance trigger is missing in the world database is dropped
  from the encounter's usable rows at construction with a `LOG_ERROR` (`InstanceEncounter.cpp:154-173`). That filter
  shifts
  the indices, while `BeforeLevel` indexes the filtered vector with the arena's pinned index (`:474-475`): see O3.
- The long comment on `WING` (`InstanceBosses.cpp:30-40`) is history: it says the Scarlet Monastery wings and Utgarde
  Keep "come back once this one is learned"; no such rows exist.

**Reviewer notes.**

- `WingBosses()` is hand-kept duplicate knowledge of the dungeons (entries "looked up 2026-10-07" per `InstanceBosses.h:
  47-48`). A boss missing from it gets no `boss_<name>` column; a wrong entry gives a column that is always 0.
- `FindSpawn` returns the first creature spawn with the row's `mapid` and `id` in `GetAllCreatureData()` order (an
  unordered container). With more than one spawn of one entry on a map the chosen spawn is unspecified.
  UNVERIFIED: that Bazzalan, VanCleef and Serpentis each have exactly one spawn in the world database.

### E3.2 InstanceEncounter: what it is

`class InstanceEncounter final : public Encounter` (`Encounters.h:147`). One instance per scenario, built when an arena
of
the stage has `Against = Opposition::Instance`; per-env state is `std::vector<EnvInstance> _envs`
(`InstanceEncounter.cpp:141`).

An episode is a **run of a whole dungeon**: a fresh instance (every creature alive, every boss script at its start:
`StageScenario.cpp:2175-2177`, `freshInstance = Arena(env).Instance == InstanceLadder::Wing`, which forces a map change
and disables character reuse), the party of five at the instance's door, the route from the door to the last boss as the
objective. Three modes share the class, chosen per episode in `BeforeLevel`:

1. **Full run** (`dungeon2_ragefire`, `dungeon3_deadmines`): door to last boss, full clear. Ends on the last boss's
   death, the last allowed wipe, or the clock.
2. **Corridor** (`group2_corridor`, `ArenaDefinition::CorridorPacks = 4`): four packs of the route in order; the packs
   before them are cleared and the party stood short of them (`StartCorridor`, second half).
3. **Pull drill** (`dungeon1_pulls`, `ArenaDefinition::PullDrill`): one pack of the route, the packs before it cleared
   (`StartDrill`, second half). One wipe allowed.

Plus **Go-Explore starts**: a training full run may start from a "cell" sent by the learner (`StartAt`, second half).

**Stage definitions that use it** (`Stages/Stages.cpp`): `group2_corridor` arenas `ragefire` (row 0, 900 s, 4 packs) and
`deadmines` (row 1, 1200 s, 4 packs, levels 17-20) at `:942-951`; `dungeon1_pulls` arena `ragefire` (row 0, drill, 180
s)
at `:966-971`; `dungeon2_ragefire` arenas `dungeon` (row 0, 7200 s) and `heldout` (row 2, 10800 s, `EvalOnly`, weight 0)
at `:990-998`; `dungeon3_deadmines` arenas `dungeon` (row 1, 14400 s, levels 17-20) and `heldout` (row 2) at
`:1013-1021`. All have `PartyGroup = true` and `Seats = SeatPlan::Party`; every arena except the two `heldout` ones
carries `StandInShare = DUNGEON_STAND_IN_SHARE`. Validation of these fields: `Stages.cpp:1033-1067`
(`CurriculumProblems`).

### E3.3 Public API and per-env state

Public (`Encounters.h:150-182`):

| Member | Meaning |
|---|---|
| `EXPLORE_PACK_WORDS = 4`, `EXPLORE_PACK_BITS = 24`, `EXPLORE_PACKS = 96`, `EXPLORE_YARD_BUCKET = 16`, `EXPLORE_MARKS = 8` | Go-Explore cell encoding: a cell's cleared packs are 4 words of 24 bits (a `float` episode-info value holds 24 bits exactly), so only the route's first 96 packs; the party's yard in buckets of 16. |
| `static std::pair<uint32,uint32> DungeonLevels(BossRow const&)` | The level range the dungeon is run at. |
| `static CreatureData const* FindSpawn(BossRow const&)` | The boss's world-database spawn, or null. |
| the `Encounter` overrides | `RewardTerms`, `AddEpisodeInfo`, `ResetEpisode`, `BeforeLevel`, `Build`, `UpdateEnemies`, `Update`, `View`, `Reward`, `WriteState`, `IsTerminal` (its `SelectTarget` was deleted 2026-10-08: every instance stage has the sight block, which `CurriculumStages` now requires of an instance). No `BeforeRebuild`, `Deactivate` or `Teardown` override: the encounter spawns nothing of its own (the instance's creatures belong to the map). |

Per-env state (`Encounters.h`, `EnvInstance` and `SeatInstance`, read by the first half):

`EnvInstance`: `Row` (the chosen `BossRow const*`), `MapId`, `Entry`, `Tier` (the **pinned row index**, not the ladder
rung), `Boss` guid, `BossHealth`, `HealthLeft`, `Engaged`/`EngageMs`, `BossDead`, `Wiped`, `Evaded`, `Recorded`,
`Route`/`RouteRemain`/`Dense`/`RouteDense`/`CornerAhead`/`CornerBack`/`RouteNext`, `TrashKills`, `BossKills`,
`ProgressMs`/`ProgressSeen`, `Wipes`, `Rung` (the ladder rung the run was drawn on), `Probe`, `WipesAllowed`,
`Evaluating`, `Trace` (a `FightTrace`), `Fighting`, `ReadyEngages`, `OnParty`, `CrowdSeconds`, `HostileTotal`, `LastMs`,
`Level`, `StuckLoggedMs`, `Rises`/`Rejoins`/`RejoinMsTotal`, `Entrance`, `Wipe` (a `WipeLatch`), `Tank`, `Overflow`,
`Objects`, `Used`, `Approached`/`ApproachedMs`, `EndLogged`, `LastKillMs`,
`HasAhead`/`Ahead`/`AheadSize`,
`HasSecond`/`Second`, the drill block (`Drill`, `DrillRung`, `DrillPackIndex`, `DrillGap`, `DrillPoint`, `DrillPack`,
`DrillGroups`, `DrillLocked`, `DrillOther`, `DrillEngaged`, `DrillCleared`, `DrillExtra`, `DrillExtraEntry`,
`DrillPeak`),
`Watched`/`Counted`, `RoutePacks`, `PackOf`, `MapLayout`, `CorridorRun`/`Corridor`, `Drawn`/`ChainPulls`,
`ReadyPaidCap`, `BossesKilled`, `Seats[MAX_SEATS]`, and the Go-Explore block (`Started`, `StartPacks`, `StartYard`,
`Marks`).

`SeatInstance`: the pay latches (`OutcomePaid`, `KillsPaid`, `BossKillsPaid`, `WaypointsPaid`, `WipesPaid`, `DeathPaid`,
`EngagesPaid`, `ClearsPaid`, `ChainPaid`, `FullClearPaid`), `Potential`/`PotentialReady`, `Clock` (`RespawnClock`),
`Walk`, `Deaths`, the view caches (`DenseAt`, `Detour`, `Frontier`, `FrontierMs`, `FrontierReady`, all `mutable`) and
the
supplies (`FoodItem`, `DrinkItem`).

Encounter-level state: `_rows` (per ladder, the usable `BossRow const*` list), `_drillLock`, `_drillRung`, `_drillRuns`
(the pull drill's ladder, shared by every env of the scenario).

Function-local statics that outlive episodes: `WingRoute`'s `routes` cache keyed by `EngageKey{map, entry}`
(`InstanceEncounter.cpp:1245-1253`, process-wide, never cleared) and `KeyItems`' `keys` map (`:1207-1208`), each with
its
own mutex. They are shared by every scenario in the process.

### E3.4 Episode lifecycle, step by step

The scenario (`StageScenario.cpp`) calls the hooks in this order (call sites: `ResetEpisode` `:1949`, `BeforeRebuild`
`:1957`, `BeforeLevel` `:2120`, `Build` `:2334` inside a retry loop, `UpdateEnemies` `:2803`, `Update` `:2805`,
`IsTerminal` `:1837-1844`).

**1. Reset (`ResetEpisode`, `:401-449`).** Runs when a new episode starts; it reports the run that just ended and then
clears the env's state.

- If the ended run was a drill (`fight.Drill`): writes the `Pull drill:` log line when `Instance.WingTrace` is on
  (`:407-414`); if `fight.Row` is set and the run was not an evaluation, calls `NoteDrill(DrillRung, DrillCleared &&
  !DrillExtra)` (`:416-417`, feeds the drill ladder, second half); then `fight = EnvInstance()` and returns
  (`:418-419`). A drill never calls `NoteWingRun`.
- Otherwise, if the run has a route and `WingTrace` is on, writes one `Wing run:` line (`:423-436`): evaluation or
  training or "train from a cell", probe flag, rung, level, route point, kills, bosses, wipes, rises, rejoins, seconds
  with no progress, and the corridor or chain-pull summary.
- If the run has a route, was not an evaluation and not started from a cell, computes `progress` (`CorridorRun` ->
  `Corridor.Share()`; else `BossDead` -> 1; else `min(1, (TrashKills + BossDead) / (HostileTotal + 1))`, or 0 with no
  hostiles) and calls `StageScenario::NoteWingRun(Rung, Probe, progress)` (`:440-447`). That feeds the wing difficulty
  ladder; only probes step it (see the WingLadder section of `cpp-encounters.md`).
- `fight = EnvInstance()` clears everything (`:448`). The encounter-level drill ladder (`_drillRung`, `_drillRuns`) is
  not touched.

**2. BeforeLevel (`:451-514`).** After the arena and the seats' classes are drawn, before the level. Skipped
(`:457-459`) when `Rows(env)` is empty: nothing is set and `Build` then fails on the null `Row`.

1. `pinned = uint32(arena.InstanceRow)`. `InstanceRow` is an `int8` defaulting to -1 (`StageDefinition.h:178`);
   `CurriculumProblems` rejects an instance arena with `InstanceRow < 0` (`Stages.cpp:1033`).
2. A Go-Explore start is drawn only when `!env.Evaluating && !arena.PullDrill && !arena.CorridorPacks`:
   `_scenario.DrawExploreStart(data.Arena, pinned + 1)`; `fight.Started` is true when it returned a start whose `Tier ==
   pinned` (`:466-473`); `StartPacks`/`StartYard` are copied.
3. `fight.Tier = min(pinned, rows.size()-1)`; `Row`, `MapId`, `Entry` from the row (`:474-477`).
4. `EpisodeMapId` and `HasEpisodeMap` are set so the seats are built on the dungeon's map (`:479-480`).
5. Ladder rung: `fight.Evaluating = env.Evaluating`; `fight.Rung = Evaluating ? WING_RUNGS.size()-1 : WingRungNow()`
   (`:487-488`). `StageScenario::WING_RUNGS` (`StageScenario.h:218-228`) has 9 entries `{Lift, ExtraWipes}`: {8,4} {7,3}
   {6,3} {5,2} {4,2} {3,1} {2,1} {1,0} {0,0}. An evaluation always runs at the last rung (no lift, no spare wipes).
6. `fight.Drill = arena.PullDrill && (!env.Evaluating || EvaluatesDrills(stage.Arenas))`; `fight.CorridorRun =
   arena.CorridorPacks > 0 && !fight.Drill`; `fight.Probe = !Evaluating && !Drill && !Started && frand(0,1) <
   Instance.WingProbe`; `fight.WipesAllowed = Drill ? 1 : Instance.WingWipes + rung.ExtraWipes` (`:494-497`).
7. Level: `[low, high]` = the arena's `[LevelFirst, LevelLast]` when `LevelFirst != 0`, else `DungeonLevels(*Row)`;
   `lift = Drill ? min(rung.Lift, Instance.PullLift) : rung.Lift`; `EpisodeLevel = min(urand(low, high) + lift,
   DEFAULT_MAX_LEVEL)` (`:499-502`). One level for the whole party, drawn once per episode.
8. Difficulty: for a raid map (`MapEntry::IsRaid`) `RaidDifficulty = row.Difficulty` and `DungeonDifficulty = 0`;
   otherwise the reverse (`:504-507`).
9. Spawn: `GetMapEntranceTrigger(MapId)`'s target position becomes `EpisodeSpawn` (`HasEpisodeSpawn = true`) and is
   copied to `fight.Entrance`, where the dead rise (`:509-513`). The trigger pointer is dereferenced unchecked; the
   constructor guarantees one exists for every kept row (`:166-171`).

**3. Build (`:578-695`).** After the seats' bots are placed on the fresh instance. Returns false (the scenario retries
up
to 4 spawn attempts, `StageScenario.cpp:2328-2345`) when `Row` or `map` is missing or the boss is not found.

1. `seat = SeatBot(env, 0)`; `boss = FindBoss(map, *Row, seat)` (`:584-591`; logs `... is not in instance ...` on
   failure).
2. The boss as the party should find it: `Respawn(true)` if dead, `AI()->EnterEvadeMode()` if in combat,
   `SetFullHealth`;
   `fight.Boss`, `fight.BossHealth` = max health, at least 1 (`:593-600`).
3. `plan = WingRoute(env, map, seat, boss)` (E3.10); copy `Route`, `Dense`, `RouteDense`, `CornerAhead`, `CornerBack`;
   reset each seat's `DenseAt` and `Detour` (`:604-614`).
4. `HostileTotal` and the sorted `counted` spawn-id list: every creature of `map->GetCreatureBySpawnIdStore()` for which
   `Hostile(seat, creature)` holds and (the plan is not a field route, or the creature is a dungeon boss or world boss,
   or
   its spawn id is in `plan.Reachable`) (`:617-626`). This is the full clear's denominator.
5. `RouteRemain[i]` = yards along the route from point i to the end (`:627-629`).
6. `RoutePacks` and `PackOf` rebuilt from `plan.Packs`: each pack stands at `Dense[pack.Yard]` (when `Yard` is nonzero
   and
   inside `Dense`) else `pack.At`; `Cleared = false`, `Resolved` left false (`:630-638`).
7. `MapLayout = SeenPlaces::Layout(ground, 25 yd)` over the dense route (the route when there is no dense one)
   (`:639-646`): ground nodes every 25 yards, unordered, no creature on them.
8. `ReadyPaidCap = max(1, RoutePacks.size())` (`:648`); `env.Targets.clear()` (`:651`).
9. Mode set-up: `if (Drill && !StartDrill(...))` falls back to the whole dungeon with a `LOG_WARN` (`:652-657`); `if
   (Started && !StartAt(...))` falls back to the door (`:658-663`); `if (CorridorRun && !StartCorridor(...))` falls back
   to
   the whole dungeon (`:664-669`). Then `ReadyPaidCap` is 1 for a drill and `max(1, Corridor.Length())` for a corridor
   (`:671-674`).
10. Supplies, for every active seat (`:678-693`): `_scenario.PrepareFighter(bot, seatState)`; `FoodItem =
    ConsumablePool::Food(level)`; `DrinkItem = Drink(level)` only for a seat with mana; `StockConsumables(bot, food,
    drink, Instance.WingSupplies)`; and every key item the map's locks take (`KeyItems(MapId)`) is added once
    (`AddItem(key, 1)`). The Defias Gunpowder for the Deadmines cannon is the real case (comment `:1186-1188`).

**4. Per-decision update (`UpdateEnemies` then `Update`).** Both run once per decision, before the actions are applied.
`UpdateEnemies` is `UpdateWingEnemies(env, _envs[env.Index])` (`:697-700`; body in the second half: fills `env.Targets`
and `Objects`, counts kills, resolves pack states). `Update` (`:702-960`) does, in order:

1. Resolve the boss: `seat = SeatBot(env, 0)`; `boss = Encoding::CreatureThrough(*seat, fight.Boss)`. **If either is
   null the whole function returns** (`:704-708`): no wipe detection, no rises, no route advance on that decision (O1).
2. Boss readings: `HealthLeft = health / BossHealth` (0 when dead); `BossDead` latches when the boss is seen dead;
   `Engaged`/`EngageMs` latch on the first decision the boss is in combat; `Evaded` latches when engaged, boss not dead,
   not in combat, health >= 99% (`EVADED_HEALTH_PCT`) and the clock is past `EngageMs + DecisionMs` (`:710-721`).
   `Evaded` is only reported; nothing ends the run on it (the comment at `:2748` says a wing goes on past an evade).
3. `anyoneAlive` over the active seats (`:724-728`).
4. Fight state: `Fighting` = some `env.Targets` slot holds a living in-combat unit (`:730-734`). A fight that starts
   this
   decision with every living seat at `>= WingReadyShare` of health and mana (mana only for seats with mana) increments
   `ReadyEngages` (`:737-750`).
5. `TraceWing(env, fight, Fighting || !anyoneAlive)` (E3.6): chooses the tank, counts the crowd, records the trace.
6. `UpdateDrill` for a drill, else `UpdateDrawnPacks` (chain-pull detection) (`:752-755`); a corridor run feeds
   `Corridor.Note(cleared flags of RoutePacks)` (`:757-763`).
7. `LastMs = EpisodeElapsedMs`; `Level = EpisodeLevel` (`:764-765`).
8. With `WingTrace` on, not a drill, not yet logged, and `TimeIsUp(env)`: one `Wing time:` line listing what is in
   combat
   within 80 yd of a living seat (`:768-794`).
9. **Wipe**: `fight.Wipe.Note(anyoneAlive, fight.BossDead)` (`WipeLatch`, `EntranceRespawn.h`) returns true once when
   nobody
   is alive and the dungeon is not cleared, and not again until somebody is alive (`:798`). On a wipe: `++Wipes`;
   `LogWipe` when tracing; `Trace` cleared; **if `Wipes >= WipesAllowed`, `Wiped = true` and `Update` returns at once**
   (`:800-808`); otherwise it falls through. With the defaults (`WingWipes = 2`; rung 8's `ExtraWipes = 0`) the second
   wipe
   ends the run; a drill ends at the first (`WipesAllowed = 1`); a training run on rung 0 allows `2 + 4 = 6`.
10. `RiseDead(env, fight)` (E3.7) (`:810`).
11. Object give-up (`:812-841`): the tank (the seat whose guid equals `fight.Tank`, alive), when not fighting, picks the
    nearest `Usable` and `CanUse` object of `fight.Objects` within 25 yd (`NEAR_OBJECT_YARDS`) that is not in
    `fight.Used`;
    if the same object stays nearest for more than 45 s (`GIVE_UP_MS`) it is added to `Used` ("passed by for the rest of
    the run, as used") and `Approached` is cleared.
12. Each living seat's `Walk` (its place on the route): looks up to `WALK_LOOK = 6` points ahead; a point counts as
    reached within `WALK_REACH = 12` yd; the cap is `RouteNext` for the tank and the tank's `Walk` for everyone else,
    never past the last point (`:845-864`). `RiseDead` resets `Walk` to 0.
13. Stuck log: after `STUCK_FIRST_MS = 120000` ms with no progress (`ProgressMs`) and again every `STUCK_EVERY_MS =
    600000`, a long `Wing stuck:` line (seats, objects, PathGenerator probes) when `WingTrace` is on and the run is not
    a
    drill (`:866-947`). It builds `PathGenerator` objects on the map thread only for this log.
14. **Route advance** (`:949-959`): when `RouteNext < Route.size()`, the party is not `Fighting`, and (not a drill or
    `RouteNext < DrillPoint`), the first living seat within 15 yd of `Route[RouteNext]` advances `RouteNext` by one (one
    point per decision, then `break`).

**5. Observe.** `View` (`:2432`, second half) fills the seat's goal places through `SeenWorld` (what the seat saw, its
map's frontier, the layout, the leader). (`SelectTarget` was deleted 2026-10-08: it was never reached, since the seat's
own client selection is the target in a sight stage.)

**6. Reward (`Reward`, `:2529-2693`; terms in E3.5).** Per seat per decision.

**7. Terminal (`IsTerminal`, `:2745-2752`).** True when `BossDead || Wiped || TimeIsUp(env) || (Drill && (DrillCleared
||
DrillExtra)) || (CorridorRun && Corridor.Done())`. `TimeIsUp` is `env.EpisodeLengthMs != 0 && EpisodeElapsedMs >=
EpisodeLengthMs` (`:2734-2737`), the length coming from the arena's `EpisodeSeconds`. With default tuning the second
wipe
is the only wipe-based end (E3.4 step 9); a first wipe is scored and the party rises at the entrance. `Wiped` is set
only
at `:806`.

**8. Episode info.** Every column is a lambda over `_envs[env.Index]` read when the scenario writes the row
(`:197-399`); see E3.8.

**9. Critic state.** `WriteState` (`:2739-2743`) writes only `state[STATE_TIER] = Tier / (max(2, Rows.size()) - 1)`
(`STATE_TIER = 13`, `StageScenario.h:76`): the pinned row index over the top index; with a single usable row the divisor
is 1.

### E3.5 Reward terms paid

`RewardTerms()` (`:190-195`) declares `StepCost, Approach, Kill, Death, Timeout, Threat, PullClean, PullExtra, Clear,
ReadyPull, Idle, Lost, Away`; each gets a `reward_<name>` episode-info column (per the base-class contract,
`Encounter.h:57`). Values are in `Instance` tuning (`CurriculumTuning.h:180-278`; keys `Instance.<Field>`, read through
the `Visit` list `:901-942`; see `cpp-tuning-keys.md`). `s` = `DecisionMs / 1000` seconds. `T` = `TierScale(env)`
(`:2723-2732`) = `1 + Difficulty.TierScale (0.25) x min(tier, Instance.MaxTierScale (6))`, with `tier = DrillRung` for a
drill and `WingRun::TierOfRung(Rung) = Rung / 2` otherwise (0..4 over the 9 rungs; an evaluation's rung 8 is tier 4, T =
2.0). The third argument of `ledger.Add` is a factor that multiplies the paid reward but is left out of the score
(`Rewards/RewardLedger.h:288-298`): `T` for wins and `1/T` for losses.

Categories (`Rewards/RewardLedger.h:152-228`): Kill, Clear, ReadyPull, PullClean are Outcome; Death, Timeout, StepCost,
PullExtra, Lost, Away, Idle are Cost; Approach and Threat are Shaping (scaled by the shaping fade). The code is in the
second half; this table was checked against `:2529-2693`.

| Term | Kind | Condition and formula | Key (default) |
|---|---|---|---|
| Idle | Cost | Every seat: when `EpisodeElapsedMs > ProgressMs + grace`, `-WingStall x (tank ? 1 : WingStallOthers) x s`; `grace` = `PullGraceMs` for a drill, else `WingStallGraceMs`. `ProgressMs` is moved by seat 0's call when kills + waypoints changed or `StepEngaged` (`:2540-2555`). | `WingStall` (0.1), `WingStallOthers` (0.2), `WingStallGraceMs` (60000), `PullGraceMs` (20000) |
| StepCost | Cost | Every seat every decision: `-WingClock x s` (`:2557`). | `WingClock` (0.002) |
| ReadyPull | Outcome | `+WingEngage x (ready - EngagesPaid)`, tier `T`, `ready = min(ReadyEngages, ReadyPaidCap)` (`:2560-2565`). | `WingEngage` (1.0); readiness `WingReadyShare` (0.8) |
| PullExtra | Cost | Chain pull: `-WingChainPull x (ChainPulls - ChainPaid)`, tier `1/T` (`:2567-2572`). Drill, a second pack joined: `-PullExtra x share`, tier `1/T` (`:2654-2655`). | `WingChainPull` (3.0), `PullExtra` (5.0) |
| Clear | Outcome | Corridor: `+CorridorPack x (Corridor.InOrder - ClearsPaid)`, tier `T` (`:2574-2579`). Full clear: `+WingClear` once per seat when the last boss is dead, `FullClear` holds and it is not a corridor, tier `T` (`:2684-2688`). | `CorridorPack` (4.0), `WingClear` (25.0) |
| Threat | Shaping | `OnParty > WingCrowdFree`: `-WingCrowd x (OnParty - WingCrowdFree) x s` (`:2581-2583`). | `WingCrowd` (0.15), `WingCrowdFree` (4) |
| Lost | Cost | `WingRun::Strays(alive, isLeader, walkingBack, leaderHere, yards, WingStrayYards)`: `-WingStray x s`. Leader = the stand-in seat when it leads, else the tank (`WingRun::LeaderSeat`) (`:2587-2600`). | `WingStray` (0.02), `WingStrayYards` (25) |
| Away | Cost | `WingRun::Away(alive, walkingBack)` (dead, or risen and not yet rejoined): `-WingAway x s` (`:2602-2603`). | `WingAway` (0.02) |
| Kill | Outcome | Living seat: `+WingTrashKill x (TrashKills - KillsPaid)` and `+WingMidBoss x (BossKills - BossKillsPaid)`, tier `T` (`:2607,2609`); last boss dead, once per seat: `+WingBoss`, tier `T` (`:2682`). | `WingTrashKill` (1.0), `WingMidBoss` (8.0), `WingBoss` (25.0) |
| Approach | Shaping | Living seat: `+WingWaypoint x T x (waypoints - WaypointsPaid)` (`:2608`); route potential: `+WingProgress x T x (potential - Potential)` when the party is not fighting and the potential rose (`:2615-2627`), `potential = -(dist to next point + RouteRemain[next]) / RouteRemain[0]`. | `WingWaypoint` (0.5), `WingProgress` (60.0) |
| Death | Cost | Own death, once per death (`DeathPaid` resets on a rise): `-WingDeath`, tier `1/T` (`:2632-2637`). Each new wipe: `-WingWipe x newWipes`, tier `1/T` (`:2638-2642`). | `WingDeath` (3.0), `WingWipe` (5.0) |
| Timeout | Cost | At the run's end, once per seat: a drill with the clock out and the pack alive `-PullTimeout x share`; a corridor `-WingTimeout x (1 - Corridor.Share())` when time is up and it is not done; a full run `-WingTimeout x (1 - waypoints / Route.size())` when time is up and the boss is not dead; all tier `1/T` (`:2658-2659,2675-2676,2690-2692`). | `PullTimeout` (2.0), `WingTimeout` (30.0) |
| PullClean | Outcome | Drill, the pack dead alone: `+PullClean x share`, tier `T`; `share = tank ? 1 : PullOthers` (`:2653,2656-2657`). | `PullClean` (5.0), `PullOthers` (0.5) |

The end-of-run latch is `OutcomePaid` per seat (`:2644-2648`). The first seat also sets `Recorded` and writes a
debug-level count of route packs never found (`:2662-2671`).

### E3.6 TraceWing: tank choice, crowd measure, wipe log (`:1026-1165`)

`TraceWing(env, fight, fighting)` runs every decision (`Update` step 5) and does three unrelated jobs:

1. **Chooses `fight.Tank`** every decision (`:1034-1053`): among living seats with a built `L`, a seat "can" tank when
   its
   `DungeonRole == DUNGEON_TANK`, or it is `DUNGEON_ANY` and `AptitudeDemand::HoldsThePull().MetBy(Apt)`; the winner is
   the
   "can" seat with the highest `Apt[Aptitude::MITIGATION]`, else the highest mitigation among all. The comment says this
   is the party block's rule (`PartyEncounter::Tank`, declared `Encounters.h:84`): UNVERIFIED (compare the two bodies).
2. **Crowd measure** (`:1071-1127`), only while `fighting` (`:1054-1058` returns first otherwise, `OnParty` having been
   zeroed at `:1030`): counts non-player units within 60 yd of the first living seat whose victim is a player
   (`OnParty`); `CrowdSeconds += DecisionMs / 1000` when `OnParty > WingCrowdFree`; the peaks (`PeakEngaged`,
   `PeakElites`, `PeakOnTank`, `PeakEntries`) go to the wipe log. The `tank` used for `PeakOnTank` is a different rule:
   the first living seat in seat order that meets `HoldsThePull()` (`:1078`), see O4.
3. **Death record** for the wipe log (`:1129-1148`): each seat's mana share when last seen alive; each newly dead seat
   is
   appended as "role class seconds [mana %]". Roles there are `tank`/`healer`/`dps` from the aptitude demands
   (`HoldsThePull`, `KeepsThemUp`), not from `DungeonRole`.

`LogWipe` (`:1151-1165`) writes the `Wing wipe:` line. Because `Update` calls `TraceWing` before `RiseDead` and before
`Reward`, the rest of the decision uses this decision's tank and `OnParty`.

### E3.7 The respawn clock: `RiseDead` (`:962-1024`) (I4)

Rule (principle 7): a dead seat comes back alive at the entrance after a short delay and walks back to the party; no
graveyard, ghost or corpse run.

Per decision, for every active seat that is in the world:

1. Reference tank: the living seat whose guid equals `fight.Tank` and which is in the world (`:971-974`).
2. `partyYards` for a living seat: distance to that tank when it exists, is another seat and is in the seat's map; else
   the 2D distance to the centroid of the other living seats in its map; `-1` when there is nobody. A dead seat gets
   `-1`
   (`:981-1002`).
3. `step = seat.Clock.Note(EpisodeElapsedMs, alive, partyYards, Respawn.DelayMs, Respawn.RejoinYards)`
   (`EntranceRespawn.h`): `Died` on the first dead decision (records `DeadSinceMs`, `++Deaths`); `Rise` once `nowMs >=
   DeadSinceMs + delayMs`; `Rejoined` when a seat in the `Rejoining` state is within `rejoinYards`. A seat stood up by
   anything else (a friend's resurrection) is treated as risen where it lies (`Risen` inside `Note`).
4. On `Rise` (`:1005-1015`): `RiseAtEntrance(bot, data.Seats[index], fight.Entrance, EpisodeElapsedMs)` (documented with
   `EntranceRespawn.cpp`: stands the bot up at the entrance, full health and power, forgets its frame, restarts the
   controller), then `Clock.Risen(...)`, `DeathPaid = false`, `Walk = 0`, `DenseAt = 0`, `Detour.clear()`, `++Rises`.
5. On `Rejoined` (`:1016-1022`): `++Rejoins`; `RejoinMsTotal` is reassigned as the sum over all seats' clocks.

Keys: `Respawn.DelayMs = 10000`, `Respawn.RejoinYards = 15.0` (`CurriculumTuning.h:511-515`). The wipe latch (E3.4 step
9)
runs before `RiseDead`; when all five die together, the first wipe is counted at once and every seat rises at the same
decision `DelayMs` later. `Away` is paid while dead or walking back; `Lost` never on those seconds (`WingRun::Strays`).
The risen seat's `Walk` restarts at 0 (the entrance's place on the route).

### E3.8 Episode-info columns written (`AddEpisodeInfo`, `:197-399`)

Per-seat rows; unless noted a column is the same for every seat of the env. "Yaml" = used in the learner configs
`apps/forge/python/configs/{dungeon1_pulls,dungeon2_ragefire,dungeon3_deadmines,group2_corridor}.yaml` (`report` or
`status.headline`), checked by grep.

| Column | Value | Notes / readers |
|---|---|---|
| `difficulty` | `Tier` (pinned row index: 0, 1 or 2) | Read by the learner's evaluation and training loop (`animus/evaluation.py:296`, `animus/train.py:900`), see O10. |
| `pull_rung` | `DrillRung` | Added only when `EvaluatesDrills(stage.Arenas)` (`:205-206`): `dungeon1_pulls`; in its yaml. |
| `wing_rung` | `Rung` (0-8) | The comment (`:202-204`) says the evaluation videos read the first `_rung` column (`Vision::EvalVideoRungColumn`); UNVERIFIED. Headline in group2/dungeon2/dungeon3. |
| `at_top_rung` | 1 when `Drill ? DrillRung+1 >= PULL_GAPS.size() (4) : Rung+1 >= WING_RUNGS.size() (9)` | Read by `animus/evaluation.py:320`, `animus/train.py:902`, `animus/stage.py:24,111,513` (the convergence top-rung rule). |
| `boss_rung` | `Tier` | Same value as `difficulty`. Not in the dungeon yamls. |
| `instance_map`, `boss_entry` | `MapId`, `Entry` | Not in the dungeon yamls. |
| `boss_killed` | `BossDead` | Not in the dungeon yamls. |
| `boss_health_left` | `HealthLeft` | Boss health fraction; 0 once dead. |
| `engaged`, `evaded` | `Engaged`, `Evaded` | Not in the dungeon yamls. |
| `wiped` | `Wiped` (the run ended on its last allowed wipe) | Not the wipe count. |
| `wing_trash_kills`, `wing_boss_kills` | `TrashKills`, `BossKills` (mid-bosses on the way) | Report in dungeon2/dungeon3/group2. |
| `wing_route_share` | `min(RouteNext, Route.size()) / Route.size()`, 0 with no route | |
| `wing_wipes` | `Wipes` | Headline everywhere; target `<= 1` in dungeon2/dungeon3 yaml. |
| `wing_cleared_share` | `min(1, (TrashKills + BossDead) / (HostileTotal + 1))`, 0 with no hostiles | Same formula as the ladder progress at `:442-443`. |
| `wing_crowd_seconds` | `CrowdSeconds` | Not in the yamls. |
| `wing_probe` | `Probe` | |
| `wing_rises`, `wing_rejoins` | `Rises`, `Rejoins` | |
| `wing_rejoin_seconds` | `RejoinMsTotal / Rejoins / 1000`, 0 with none | `animus/episode_means.py:56` weights it by `wing_rejoins`; target `<= 90` in dungeon2. |
| `wing_level` | `Data(env).EpisodeLevel` | |
| `cleared` | `Succeeded(fight)` (`:2199-2206`): drill `DrillCleared && !DrillExtra`; corridor `Corridor.Done()`; else `BossDead` | Gate and measure of `group2_corridor` and `dungeon1_pulls` (`gate_metric: cleared`); partner `score: cleared`. |
| `full_clear` | `FullClear(fight)` (`:2188-2197`): boss dead and every `RoutePacks` pack cleared, else (no packs) `HostileTotal && TrashKills + 1 >= HostileTotal` | Gate and measure of `dungeon2_ragefire`. |
| `bar_clear` | `Succeeded && Wipes <= 1` | Gate and measure of `dungeon3_deadmines`. |
| `chain_pulls` | `ChainPulls` | |
| `ready_pulls` | `min(ReadyEngages, ReadyPaidCap)` | |
| `corridor_packs`, `corridor_first`, `corridor_cleared`, `corridor_in_order`, `corridor_share` | `Corridor.Length()`, `.First`, `.Cleared()`, `.InOrder`, `.Share()` | Added only when some arena has `CorridorPacks > 0` (`:269`): `group2_corridor`. |
| `drill_clean`, `drill_extra`, `drill_pulled`, `drill_gap`, `drill_pack` | `Drill && DrillCleared && !DrillExtra`; `Drill && DrillExtra`; `Drill && DrillEngaged`; `DrillGap` yards; `DrillPackIndex` | Added only when some arena has `PullDrill` (`:289`): `dungeon1_pulls`. `drill_pack` feeds the evaluation's per-pack tables via `stage.json` `episode_categories` (comment `:305-306`; UNVERIFIED). |
| `without_stand_in`, `clear_standin`, `clear_allbot` | 1 when `StandInPlay.Seat < 0`; `Seat >= 0 && Succeeded`; `Seat < 0 && Succeeded` | Per-event columns over the episodes with or without the stand-in. `with_stand_in` is added in `StandInSeat.cpp:159`. `standin_gap` (a yaml target) is not a sim column; UNVERIFIED where it is computed (expected in Python). |
| `role_tank`, `role_healer`, `role_damage` | 1 when `Seats[seat].DungeonRole` equals `DUNGEON_TANK`, `DUNGEON_HEALER`, `DUNGEON_DAMAGE` | Seat-specific. |
| `deaths_tank`, `deaths_healer`, `deaths_damage` | the seat's `Deaths` when its role matches, else 0 | Seat-specific. |
| `seat_deaths` | the seat's `Deaths` | Seat-specific. |
| `boss_<name>` | 1 when `(BossDead && Entry == entry)` or `entry` is in `BossesKilled` | One per `WingBosses()` row whose map is the map of the pinned WING row of some arena (`:347-369`); the `heldout` arena pins row 2, so the Wailing Caverns columns also exist in dungeon2/dungeon3. |
| `wing_started`, `wing_arena`, `wing_tier`, `wing_marks` | `Started`; `Data(env).Arena`; `Tier`; `Marks.size()` | Go-Explore, read by `animus/explore.py:53`. |
| `wing_mark<m>_packs<w>`, `wing_mark<m>_yard`, `wing_mark<m>_seconds` for m = 0..7, w = 0..3 | the m-th cell mark: cleared-pack words, yard, `Ms / 1000`; 0 when absent | 8 x 6 = 48 columns (`:375-398`). `animus/explore.py:24-26` mirrors `PACK_WORDS = 4` and `MARKS = 8` as separate constants that must be kept equal by hand. |

Column order is the order of the `table.Add` calls above; the learner finds columns by name.

Tests: `DungeonStagesTest.ThePurposesAreOutcomesAndThePricesCosts` (`:338`), `AWingsTierIsItsLaddersRung` (`:301`),
`TheStandInPlaysAShareOfEveryPartyStage` (`:353`), `EveryBossOfTheDungeonsIsMeasured` (`:374`).

### E3.9 Level range: `DungeonLevels` (`:1228-1240`)

Scans `sLFGDungeonStore` for an entry with the row's `MapID` and `Difficulty`; returns `[TargetLevelMin or MinLevel,
min(TargetLevelMax or MaxLevel, DEFAULT_MAX_LEVEL)]` for the first entry with `low != 0` and `high >= low`; else
`{row.Level, row.Level}`. Used only when the arena has no `LevelFirst` (`:499-500`): Ragefire's range comes from the LFG
DBC, not from `Stages.cpp`. UNVERIFIED: the concrete range for map 389 (the DBC was not read). Death knights are
excluded
from level-band dungeon stages (see `status.excluded.death_knight` in `dungeon2_ragefire.yaml`); that exclusion is in
the
draw code of `StageScenario`, not here.

### E3.10 `WingRoute` (`:1242-1465`, first part)

Computes the door-to-boss plan once per boss and caches it in the process-wide `routes` map (`:1245-1253`).

1. Cache lookup by `{MapId, Entry}`; the cached `WingPlan` is returned by value (the vectors are copied each `Build`).
2. **Stops** (`:1258-1279`): every living dungeon boss or world boss in the map's spawn store other than `boss`, ordered
   greedily nearest-next from the seat's start position, then the last boss. The order depends on server-side spawn
   positions (the route is the server's, used for rewards and the layout; what the bot is shown is `SeenWorld`).
3. (Removed 2026-10-08: the layered-field branch, `FieldRoute::Covers` / `FieldWingRoute`. Every dungeon is now planned
   by step 4; the plan has no packs, no dense route and no corner tables, so the pack drill, Go-Explore starts and
   corridor runs, which need `Packs`, find none and the run is the whole dungeon. Line numbers in this file are from
   before the removal.)
4. **Navmesh route** (was "fallback") (`pathThrough`, `:1297-1385`): per stop, up to `PATH_LEGS = 16` PathGenerator legs from the
   cursor
   (one leg is limited to about 296 yd); on an incomplete path a second path back from the stop; the gap between the
   halves
   is crossed creature to creature ("breadcrumbs": the nearest creature home position within `BREADCRUMB_REACH = 45` yd
   that
   is at least `BREADCRUMB_MIN_GAIN = 3` yd closer to the far side, at most 64 steps), then the straight remainder.
5. With `Instance.WingFullClear` (default 1), packs are built from every `Hostile` creature: grouped by home position
   within `PACK_REACH = 15` yd, stood at their middle and placed along the spine (`:1387-1465`; continues in the second
   half).

A failed field plan is not cached (the `LOG_WARN` repeats each episode); the navmesh plan is cached only when the code
at
the end of the function (second half) stores it: UNVERIFIED.

### E3.11 Helpers: `Hostile`, `Usable`, `KeyItems`, `FindBoss`

- `Hostile(seat, creature)` (`:1167-1173`): alive, not a critter, civilian, totem, pet or summon, no `NOT_SELECTABLE` or
  `NON_ATTACKABLE` flag, hostile to the seat. Full clear's denominator and the pack builder's membership test.
- `Usable(object)` (`:1175-1201`): a spawned object in `GO_STATE_READY` with loot state `GO_READY` and none of
  `NOT_SELECTABLE | LOCKED | INTERACT_COND | IN_USE`; a button or goober is usable unless its lock needs a skill
  (`LOCK_KEY_SKILL`); a door is usable only when it has no lock; every other type (so chests) is not usable: no looting.
- `KeyItems(mapId)` (`:1203-1226`): per map, once (static map under a mutex), the `LOCK_KEY_ITEM` item ids of the locks
  of
  every spawned game object of the map whose item template exists.
- `KeyOf(object)` / `CanUse(bot, object)` (`:74-90`): the key item a lock names, and whether the bot carries one.
- `FindBoss(map, row, anchor)` (`:524-576`): loads the boss's grid; looks it up in the spawn-id store; else searches
  within
  100 yd (`BOSS_SEARCH_YARDS`) of its spawn by entry; else, when the core's dynamic respawn removed the dead creature,
  clears the respawn time and `LoadCreatureFromDB`s a fresh one (logs `reloaded into instance`; `:553-573`).

### E3.12 Config keys read by this half

| Key | Default | Where read (first half) |
|---|---|---|
| `Instance.PullRungStart` | 0 | `:143` |
| `Instance.WingTrace` | 1 | `:407,423,768,801,871` |
| `Instance.WingProbe` | 0.2 | `:496` |
| `Instance.WingWipes` | 2 | `:497` |
| `Instance.PullLift` | 2 | `:501` |
| `Instance.WingSupplies` | 60 | `:687` |
| `Instance.WingReadyShare` | 0.8 | `:739` |
| `Instance.WingCrowdFree` | 4 | `:1107` |
| `Instance.WingFullClear` | 1 | `:1391` |
| `Respawn.DelayMs` | 10000 | `:1003-1004` |
| `Respawn.RejoinYards` | 15.0 | `:1003-1004` |

Keys read in the second half are in E3.5's table and in `cpp-tuning-keys.md`. Key spelling in the conf file is
`AnimusForge.Curriculum.<name>` (see `cpp-tuning-keys.md` for the exact prefix and the conf.dist agreement test);
UNVERIFIED here: the prefix and the conf.dist rows for these keys.

### E3.13 Tests covering this half

- `src/test/server/game/Animus/DungeonStagesTest.cpp`: stages, layouts and encounters validate (`:93`), Wailing Caverns
  never drawn in training (`:180`), corridor order (`:239`), chain pull (`:287`), tier is the ladder's rung (`:301`),
  purposes are Outcomes and prices Costs (`:338`), stand-in share (`:353`), every boss measured (`:374`), Deadmines
  doors,
  levers and cannon used through the handlers (`:403`), movement stages unchanged (`:449`), `Lost` priced from the
  actual
  leader (`:469`), a risen seat walking back is Away not Lost (`:489`), an unseen pack or boss never reaches the goal
  places
  (`:525`), the layout holds no creature and no order (`:586`), the frontier (`:609`).
- `src/test/server/game/Animus/WingLadderTest.cpp`: the ladder class.
- `RolesStageTest.cpp`, `CombatPerceptionTest.cpp` and `PartyFollowTest.cpp` mention `EntranceRespawn` / `WingRun`
  (UNVERIFIED which assertions).
- Python: `apps/forge/python/tests/test_dungeon_stages.py`, `test_explore.py`, `test_gates.py`, `test_stage_purpose.py`
  mention the columns (UNVERIFIED which assertions).
- No unit test constructs `InstanceEncounter` itself (it needs a live `Map`). `FindBoss`, `Build`, `Update`, `RiseDead`,
  `TraceWing` and `WingRoute` are covered only by live runs.

### E3.14 Observed issues

- **O1.** `InstanceEncounter.cpp:704-708`: `Update` returns at once when seat 0 is missing or the boss guid cannot be
  resolved through seat 0. Wipe detection (`:798`), the rises (`:810`), the object give-up, the route advance, the
  clock-out log, `Corridor.Note` and `TraceWing` (which chooses the tank) are all skipped on those decisions. If seat 0
  is
  absent or the boss has despawned, a wipe is never counted by this path.
- **O2.** `InstanceEncounter.cpp:723`: the comment "Nobody stands up in an instance; the dead wait for the episode to
  end" is stale; `RiseDead` (`:810`, `:962`) stands the dead up after `Respawn.DelayMs`.
- **O3.** `InstanceEncounter.cpp:154-173` and `:474-475`: `_rows` drops rows the world database cannot field, then
  `BeforeLevel` indexes the filtered vector with `ArenaDefinition::InstanceRow` (`min(pinned, rows.size()-1)`). If row 0
  were
  dropped, an arena pinning row 0 would silently run row 1 (VanCleef), and `fight.Tier` would then mean a filtered
  index.
  `CurriculumProblems` checks the unfiltered table size (`Stages.cpp:1033-1034`), so it would not notice.
- **O4.** Two tank rules in one function: `fight.Tank` (`:1034-1053`, role-aware, highest mitigation) and the crowd
  measure's local `tank` (`:1078`, first living seat in seat order that meets `HoldsThePull`). `PeakOnTank` in the wipe
  log
  can name a different seat from the one the party follows.
- **O5.** `Evaded` (`:719-721`), `Engaged`/`EngageMs` and `HealthLeft` are written and reported but nothing ends or
  scores a
  run on them; `boss_rung` duplicates `difficulty` (`:201,218`); `engaged`, `evaded`, `boss_killed`, `boss_health_left`,
  `boss_entry`, `instance_map` and `wing_crowd_seconds` appear in none of the dungeon/group2 yamls.
- **O6.** `Update` comment `:812-813` says "within reach of the script (25 yd)" and the code uses `NEAR_OBJECT_YARDS =
  25`: consistent. The constants `NEAR_OBJECT_YARDS`, `GIVE_UP_MS`, `WALK_REACH`, `WALK_LOOK`, `STUCK_*`,
  `LAYOUT_SPACING`
  and `PATH_LEGS` are local literals, not tuning keys (cluster fingerprint covers them only through the source hash).
- **O7.** `Rows()` (`:516-522`) and `BeforeLevel` (`:457-459`): an empty row list makes `BeforeLevel` return without
  setting a map or spawn; `Build` then returns false (`:581`) and the scenario retries four times and gives up. A
  mis-configured arena therefore fails at the first episode, not at startup (the constructor logs the empty list at
  `:175-177` as an error only).
- **O8.** `WingRoute` (`:1245-1253`): process-wide cache keyed by `{MapId, Entry}` that is never invalidated and is
  returned
  by value, copying `Route`, `Dense`, `RouteDense`, `Reachable`, `Packs` and the corner tables on each `Build`.
- **O9.** `FindSpawn` is called by the constructor, by `FindBoss` (`:526`) and by `Build`'s callers; each call, and
  `FindBoss`'s loops (`:532`, `:556`), iterates the whole creature-data map: O(spawns) on the map thread per episode.
- **O10.** `AddEpisodeInfo` comment `:199-200` calls the ladder's rung `difficulty`, but `difficulty` is the pinned row
  (`:201`: `Tier`), constant per arena; the rung is `wing_rung`. The learner (`animus/evaluation.py:295-312`) still
  treats
  `difficulty` as a tier spread: for `dungeon2_ragefire`/`dungeon3_deadmines` an evaluation containing the `heldout`
  arena
  (row 2) beside the main arena (row 0 or 1) therefore reports "difficulties" and "up_to" groups split by row, not by
  rung.
  `at_top_rung` is present, so the top-rung selection uses it (`evaluation.py:320-323`).
- **O11.** The `Wing run:` log's "seconds with no progress at the end" (`:430`) is `LastMs - ProgressMs`, but
  `ProgressMs`
  is advanced only inside `Reward` for seat 0 (`:2540-2548`), and `LastMs` only inside `Update` (O1).

### E3.15 Reviewer notes (questions and risks for a refactor)

- `ResetEpisode` wipes the state with `fight = EnvInstance()` (`:418,448`): about sixty fields including several
  `std::unordered_set` and `std::vector` members rebuilt per episode per env. Nothing else holds references into
  `EnvInstance` (UNVERIFIED), `env.Targets` is cleared in `Build` (`:651`).
- Three notions of "difficulty" feed different consumers: the pinned row `Tier` (`difficulty`, `boss_rung`, `wing_tier`,
  `WriteState`), the ladder rung `Rung` (`wing_rung`, `at_top_rung`, `TierScale`, `WipesAllowed`, level lift) and
  `DrillRung` (`pull_rung`, drill `TierScale`). A refactor that merges them must keep each consumer on the right one
  (O10).
- The `Update` order matters: `TraceWing` (tank, `OnParty`) -> drill/drawn packs -> corridor note -> wipe -> `RiseDead`
  ->
  object give-up -> walk -> route advance. `Reward` reads `Fighting`, `OnParty`, `Tank`, `ReadyEngages`, `Wipes` and
  `RouteNext` as that order leaves them; `UpdateEnemies` runs before all of it.
- `Build` is retried up to 4 times by the scenario with a different spawn point and mutates the instance (respawns the
  boss, adds key items, stocks consumables, `StartDrill` despawns packs). Whether a second attempt after a partial first
  attempt double-stocks or double-despawns is UNVERIFIED: read `StartDrill`, `StartAt` and `StartCorridor`.
- Principle 2 concerns what the bot is shown. The encounter's rewards (`Approach` waypoints and the `WingProgress`
  potential, `RouteRemain`) are paid along a route built from server-side boss positions that the bot is never shown:
  confirm with the owner that this is the intended reading.
- Anonymous-namespace constants are not tuning keys (O6); `PULL_GAPS` (`:119`) fixes the drill ladder at four rungs
  while
  `Instance.PullRung*` keys only tune its start, window and target.


## E4. InstanceEncounter (second half), the wing ladder, EntranceRespawn, SeenPlaces

Scope: `src/server/game/Animus/Scenario/Curriculum/Encounters/InstanceEncounter.cpp` lines 1395-2752 (the first half,
1-1400, and `InstanceBosses` are in the E3 fragment: `Build`, `Update`, `RiseDead`, `TraceWing`, `WingRoute` up to the
navmesh fallback, the episode-info columns and the first reward table), `WingLadder.h/.cpp`, `WingRun.h`,
`EntranceRespawn.h/.cpp`, `SeenPlaces.h`, and the call sites in `StageScenario.cpp`, `AnimusForge.cpp` and
`Bridge/ClusterLink.*` that make the wing ladder the cluster's. Paths are relative to
`src/server/game/Animus/Scenario/Curriculum/Encounters/` unless they start with another directory. Section numbers
E3.x refer to the E3 fragment. Defaults quoted for tuning keys are those in `CurriculumTuning.h`; a live conf file can
override them (see [cpp-tuning-keys.md](cpp-tuning-keys.md) and [config-keys.md](config-keys.md)); I did not read the
live conf, so every "default" below is UNVERIFIED as the live value.

### E4.0 Map of the files

| Path | Lines | Role |
|---|---|---|
| `InstanceEncounter.cpp` (1395-2752 here) | 2752 | per-decision enemy upkeep, Go-Explore/corridor/drill starts, drill ladder, seen-places view, rewards, terminal |
| `WingLadder.h` | 98 | the whole-dungeon difficulty ladder state machine (one way, probes only, collapse alarm) |
| `WingLadder.cpp` | 111 | its implementation (`Note`, `Follow`) |
| `WingRun.h` | 175 | pure helpers: tier of rung, leader seat, stray/away predicates, seeded picks, corridor bookkeeping, chain-pull tracker |
| `EntranceRespawn.h` | 147 | `RespawnClock`, `WipeLatch`, `RiseAtEntrance`, `ForgetFrame` declarations |
| `EntranceRespawn.cpp` | 58 | `RiseAtEntrance`, `ForgetFrame` |
| `SeenPlaces.h` | 248 | pure goal-place choice from what a seat has seen (no live creature data) |

Logical parts of `InstanceEncounter.cpp` 1395-2752:

| Lines | Part |
|---|---|
| 1395-1465 | tail of `WingRoute`'s navmesh fallback (packs by `PACK_REACH`, route points every `WingWaypointYards`, cache store) |
| 1754-1980 | `UpdateWingEnemies` (usable objects, closed doors, pack clearing, kills, enemy slots, crowd past the slots) |
| 1982-2004 | `PlaceParty` |
| 2006-2092 | `StartAt` (Go-Explore cell start) |
| 2094-2118 | `ClearedWords`, `MarkCell` (the cell archive) |
| 2120-2154 | `StartCorridor` |
| 2156-2186 | `UpdateDrawnPacks` (chain-pull tracking) |
| 2188-2206 | `FullClear`, `Succeeded` |
| 2208-2335 | `StartDrill` |
| 2337-2409 | `UpdateDrill` |
| 2411-2430 | `NoteDrill` (the drill's own ladder) |
| 2432-2527 | `View`, `SeenWorld` |
| 2529-2693 | `Reward` |
| 2723-2732 | `TierScale` |
| 2734-2737 | `TimeIsUp` |
| 2739-2743 | `WriteState` |
| 2745-2752 | `IsTerminal` |

### E4.1 `FieldWingRoute` (removed 2026-10-08)

The layered-field route (spine, packs and their gaps, dense route, drill bands, corner tables) was removed with the
ground probe and layered fields (owner order). `WingPlan::Packs`, `Dense`, `RouteDense`, `Reachable`, `Field`,
`CornerAhead` and `CornerBack` stay declared but are always empty / false, as they already were for a navmesh route.

### E4.2 `UpdateWingEnemies` (`:1754-1980`), run from `UpdateEnemies`

Per decision, for the whole party. Uses seat 0's player as the visitor anchor; returns early when seat 0 is not in
the world. Steps:

1. **Fighting flag** (`:1766-1769`): any enemy slot unit alive and in combat.
2. **Usable objects and doors** (`:1770-1823`): clears `fight.Objects`. One
   `GameObjectListSearcher` from the middle of the living seats, radius `OBJECT_SIGHT (40) + spread`. An object that is `Usable` (E3.11), not yet in `fight.Used`, and within 40 yd of
   some living seat goes to `fight.Objects`. Nothing opens by itself (principle 4).
3. **Route packs** (`:1825-1862`): for the first `WorldView::JOURNAL_PLACES` (8) uncleared `RoutePacks`, look their
   members up by spawn id in `GetCreatureBySpawnIdStore()`; `Resolved = Resolved || found`, `Cleared = found &&
   !standing`. So a pack none of whose members is in the store (grid not loaded) stays unknown, not cleared. Outside a
   drill it then calls `MarkCell` (the Go-Explore archive).
4. **Kills** (`:1864-1888`): each watched guid now dead moves to `Counted`, bumps `TrashKills`, `LastKillMs`, and for a
   dungeon boss or world boss `BossKills` and `BossesKilled`. (`TrashKills` therefore also counts side bosses; the last
   boss is `BossDead`, handled in `Update`.)
5. **Enemy slots** (`:1890-1950`): viewed from the tank while it lives (else seat 0), unfriendly non-player, non-totem,
   non-evading units within `WING_SIGHT = 45` yd, ranked by `RankEnemy(unit, tank)` (`Encounters.h:60`): tank's target,
   on a player, fighting, standing, gone. A fighting rank keeps the slot index it had in `env.Targets`; a standing unit
   sorts by distance. The first `PACK_SLOTS` fill `env.Targets`; the boss replaces the last slot if it is in combat
   and not in the list. Every slotted guid not yet `Counted` is added to `Watched`.
6. **Crowd past the slots** (`:1952-1980`): `fight.Overflow` (up to `CROWD_SLOTS` more, fight first), `HasAhead`/`Ahead`
   (the nearest idle unit), `AheadSize` (idle units within `PACK_REACH = 12` yd of it), `HasSecond`/`Second` (the first
   idle one further). These feed `SeatView::Crowd`, which is the encounter's own bookkeeping and is documented as
   never reaching the goal places (see E4.9). UNVERIFIED: whether `Crowd` is blanked in the stage layouts, since the
   dungeon stages have no crowd block (`Stages.cpp:604-616` comment); check `CrowdBlock` use in
   `cpp-layout-character.md`.

Perception note (principles 1 and 2): the enemy slots are filled from the server's unit list within 45 yd, **through
walls**, and viewed from the tank. They feed the pack block and the critic state; whether the policy's pack block still
carries them in the dungeon stages is a layout question (`DungeonBlocks()` includes `Pack`; `Stages.cpp:604-616` says
the pack block's slots "in a sight stage, are what the seat saw: StageScenario::ViewSeat"). UNVERIFIED: that
`ViewSeat` replaces these server-side slots with seen ones for every dungeon stage. This is the single most important
principle-1 question for a reviewer of this function.

### E4.3 Starts: `PlaceParty`, `StartAt`, `StartCorridor`, `StartDrill`

All four are called from `Build` after the route is known (E3.4; `StartDrill` at `:652`, `StartAt` at `:658`,
`StartCorridor` at `:664`). They return false when their preconditions fail (no packs, `Dense` empty, `RouteDense`
size different from `Route`), which fails the build and so triggers the scenario's retry (`SPAWN_ATTEMPTS`, cpp-
stagescenario.md).

**`PlaceParty(env, start, yard)`** (`:1982-2004`): records `EpisodeSpawn`, then for every active seat sets `Walk` and
`WaypointsPaid` to the next route point (so the route already walked is not paid), `DenseAt = yard`, and teleports it
(`BotFactory::TeleportWithinMap`; the one place seats are moved by teleport, which principle 3 allows for placement,
not movement) to `start` offset by `(index % 3 - 1, index / 3 - 1)` yards. Sets `ProgressMs = now` (the idle clock).

**`StartAt`** (`:2006-2092`), the Go-Explore start: the learner sends cells (`ExploreStart`: arena, tier, 4 words of
cleared-pack bits, yard/16, weight); `Build` copies the drawn cell into `StartPacks`, `StartYard` (`:471`, E3.4).
`StartAt` decodes the bits into pack indexes (fail if a bit is past the plan's or the route's packs), loads each such
pack's grid, marks them `Cleared`/`Resolved`, and over the spawn store: living creatures of those packs are
**despawned** (`DespawnOrUnsummon(0ms, TRASH_RESPAWN 3600 s)`) except dungeon/world bosses, which `KillSelf(false)` so
the boss script opens what its death opens (`:2058-2064`); `HostileTotal` is decremented for despawned creatures that
were counted. The party goes to `StartYard * 16` on the dense route, walked **back** along it until no living hostile
is within `PULL_START_CLEARANCE = 25` yd, at most `PULL_START_MAX_YARDS = 120` yd. `RouteNext` becomes the first
route point past that yard; a mark for the start is pushed (the archive counts the visit).

**`ClearedWords` / `MarkCell`** (`:2094-2118`): the cleared flags of the first `EXPLORE_PACKS` (96) route packs as 4
words of 24 bits (24 so each word survives a float32 round trip: `Encounters.h:150`). `MarkCell` is called every
decision
in a non-drill run; it records `(words, yard / 16, elapsed)` whenever the set of cleared packs changes and is not
empty, keeping the last `EXPLORE_MARKS` = 8 per run. The yard is the **furthest** yard any seat stands at. The marks
leave the env through the episode info / protocol (E3.8 and [protocol.md](protocol.md)); the learner's archive is
`apps/forge/python/animus/explore.py` (py-learner.md). UNVERIFIED: the exact wire path of `Marks`.

**`StartCorridor`** (`:2120-2154`): `length = CorridorPacks`; `packs = min(plan.Packs, RoutePacks, 96)`. First pack =
`WingRun::CorridorFirst(packs, length, evaluating, EpisodeSeedIndex, rand32())`: starts = `packs - length + 1`
(1 if the route is shorter), an evaluation picks `SeededPick(starts, seed)` (a multiplicative hash,
`(seed + 1) * 2654435761`, so the same seed gives the same corridor every evaluation), training `roll % starts`. If the
first pack is not 0, the packs before it are cleared by `StartAt` using `StartPacks` made of bits `0..first-1` and the
party stands at pack `first`'s yard; then `Marks`, `StartPacks`, `StartYard` are reset (a corridor start is no
Go-Explore cell). `fight.Corridor.Begin(first, min(packs, first + length))`. Logged with `WingTrace`.

**`StartDrill`** (`:2208-2335`): the pull drill (D1). Picks the ladder rung: an evaluation uses the last rung
(`PULL_GAPS.size()-1`, any pack); training uses `_drillRung` under `_drillLock`. The candidate packs are those with a
non-zero `Yard` inside `Dense` and `Gap >= PULL_GAPS[rung]` (`{30, 22, 14, 0}` yd, `:119`); if none, any drillable
pack. An evaluation takes `SweepPick(open.size(), seed)` (seed mod count, so every pack in turn), training `urand`.
The packs before the chosen one are despawned (their grids loaded first so absent creatures do not stand up behind the
party) and marked cleared; the chosen pack's members are `DrillPack`; every other standing creature gets a
`DrillGroups` entry (which later pack of the plan it belongs to, else a unique group), so a second pack can be named
whole. The party starts `Instance.PullStartYards` (35) back along the route from the pack and further, up to 120 yd,
until nothing standing is within 25 yd. `DrillPoint` is the first route point at or past the pack's yard. Fails if
`DrillPack` is empty.

### E4.4 `UpdateDrill` and the drill's ladder (`:2337-2430`)

`UpdateDrill` (called from `Update` when `fight.Drill`, `:753`): the anchor is the tank if alive, else the last
living seat. `DrillEngaged` latches when a drill-pack creature is in combat. For every living seat, units within
`DRILL_SIGHT = 50` yd that are alive creatures, not summons, totems or pets, whose victim belongs to a player, are "on
the party". The **first** creature on the party locks the pack (`DrillLocked`): if it is not in `DrillPack` then the
pull is "another pack" (`DrillOther`), `DrillPack` is replaced by that creature plus its `DrillGroups` mates, and the
drill is judged on that pack instead. After the lock, any on-party creature outside `DrillPack` sets `DrillExtra`
(and `DrillExtraEntry`). `DrillPeak` = most creatures on the party at once. `DrillCleared` latches when no drill-pack
creature is alive, the party is not fighting and nothing is on it.

`NoteDrill(rung, clean)` (`:2411`), called from `ResetEpisode` for a training drill (E3.4 at `:417`): a deque of the
last
`Instance.PullRungRuns` (100) outcomes (clean = cleared and not extra), ignored unless `rung == _drillRung`; when the
window is full and the clean share >= `Instance.PullRungTarget` (0.7) the rung steps up (`++_drillRung`, window cleared,
logged "the pull drill steps to rung N"). It never steps back and stops at the last rung (`PULL_GAPS.size() - 1` = 3).
**This ladder is per process** ("each machine climbs its own", `CurriculumTuning.h:267`): it is **not** shared with the
cluster the way the wing ladder is, and there is no alarm of any kind for it. `_drillRung` starts at
`Instance.PullRungStart`. A resumed run restarts it at that key.

### E4.5 `UpdateDrawnPacks`, `FullClear`, `Succeeded` (`:2156-2206`)

`UpdateDrawnPacks` (called each decision for a non-drill run, `:755`): when not fighting, `fight.Drawn.End()`. Otherwise
the route packs of every creature in the slots and in `Overflow` that is alive, in combat and whose **victim is a
player or a player's pet/charm** (`GetCharmerOrOwnerPlayerOrPlayerItself`), via `PackOf[spawnId]`, are passed to
`WingRun::FightPacks::Note`, which counts a pack joining a fight that already had a pack as one **chain pull**
(`fight.ChainPulls += ...`). `FullClear`: the last boss dead and either every route pack `Cleared` (field route) or, on
a navmesh route with no packs, `TrashKills + 1 >= HostileTotal`. `Succeeded`: drill = cleared and not extra; corridor =
`Corridor.Done()`; else `BossDead`. (`Succeeded` is the source of the `cleared`, `clear_allbot` and
`clear_standin`-style columns (`:256-318`, E3.8).)

### E4.6 `View` and `SeenWorld` (`:2432-2527`) and `SeenPlaces.h`

`View` (only when the route is known) copies the seat's food and drink item ids into the view and calls `SeenWorld`.
`SeenWorld` fills `view.World.Places` (8 slots) and the assignment from `SeenPlaces::Choose`, and sets
`world.RoutePlaces = true`. Inputs:

- **Memory**: the seat's `Recall.Entries()` (entity memory: last seen position, reaction < 0 = hostile, dead flag,
  gameobject flag).
- **Frontier**: `SeenPlaces::Frontier(seatPos, 40 yd, 2 yd step, ROAM_PLACES = 6 points, probe)` over the seat's own
  `MentalMap`, cached per seat and refreshed every 2000 ms of episode time (`FRONTIER_MS`). A cell is `Unknown` if
  absent or not `Vision::Known`, `Shut` with a wall-low/high or hazard flag, `Open` if it has a floor or is
  free/visited.
- **Layout** (only `GoalPlaces == SeenAndLayout` and `fight.MapLayout` non-empty): the dungeon map's nodes, with a per-
  node "explored" flag from the seat's map. Today `MapLayout` is built in `Build` (`:645`) by
  `SeenPlaces::Layout(ground,
  LAYOUT_SPACING)` from the route's ground, so, as `Stages.cpp` and the docs say, it follows the boss route and is off
  by default (`SeenOnly`).
- **Leader**: `WingRun::LeaderSeat(standInSeat, standInLeads, tankSeat)`; present if it is another seat, alive, in the
  same map. Position is the leader's exact server position (its frame and map dot show it to a player).

`SeenPlaces::Choose` (pure, `SeenPlaces.h:115-175`): place slots 0..2 (`MEMORY_PLACES`) are remembered **living
hostile non-gameobject** entries sorted nearest first and de-duplicated within `SAME_PLACE = 10` yd; slots up to 5
(`ROAM_PLACES`) are then the frontier points and unexplored layout nodes nearest first, de-duplicated; slot 6
(`WAY_ON`) is the single nearest frontier/layout point (even if already used as a roam place); slot 7 (`LEADER`) is the
leader. The assignment is the first present of slot 0, `WAY_ON`, `LEADER`. The `static_assert(sizeof(Point) ==
3 * sizeof(float))` is the code's guard that a place carries no creature data (`:42`). A remembered hostile that has
died but whose death the seat has not seen is still a place; a dead-as-last-seen entry is skipped. `Layout()` sorts the
ground by position and keeps nodes at least `spacing` apart, so nothing of the walk order survives. Tested by
`DungeonStagesTest.AnUnseenPackOrBossNeverReachesTheGoalPlaces`, `TheLayoutHoldsNoCreatureDataAndNoOrder`,
`TheFrontierIsOpenGroundBesideTheUnseen`.

Quirk: `SeenWorld` writes `own.Frontier`, `FrontierMs`, `FrontierReady` through a `const` env reference (the members
are evidently `mutable`); observation of seat `s` therefore mutates state, from the map thread that observes it. The
leader's position is also read from other seats' player objects from within one seat's observation, on the same map.
UNVERIFIED: that every seat of one env is observed on one thread (it should be: one env, one map).

### E4.7 `Reward` (`:2529-2693`): the exact terms

Called per seat per decision (after `BeforeRewards`). `tierScale = TierScale(env)` (E4.8). `seconds = DecisionMs /
1000`.
Kinds are from `RewardTermCategory` (`Rewards/RewardLedger.h:162-250`). `ledger.Add(term, value, tier)` multiplies by
the
tier argument and, for Shaping, by the fade scale; the **score** takes `value` without the tier. A cost "over the tier
scale" is passed `1 / tierScale` so it shrinks as the rung climbs.

Paid every decision to every seat (`:2537-2566`):

| Term (kind) | Condition and amount | Key (default) |
|---|---|---|
| `Idle` (Cost) | `EpisodeElapsedMs > ProgressMs + grace`, where the grace is `PullGraceMs` for a drill, else `WingStallGraceMs`: `-WingStall * (tank ? 1 : WingStallOthers) * seconds`. `ProgressMs` is bumped by seat 0 when `TrashKills + waypoints` changed or the env's `StepEngaged` is true | `Instance.WingStall` 0.1, `WingStallGraceMs` 60000, `WingStallOthers` 0.2, `PullGraceMs` 20000 |
| `StepCost` (Cost) | `-WingClock * seconds` | `Instance.WingClock` 0.002 |
| `ReadyPull` (Outcome) | for each new fight started with every living member ready (`fight.ReadyEngages`, capped by `ReadyPaidCap`, counted in the first half): `+WingEngage * newEngages`, tier `tierScale` | `Instance.WingEngage` 1.0 (the ready share `WingReadyShare` 0.8 is read in `Update`, `:739-749`; the cap `ReadyPaidCap` is set in `Build`, `:648,672-674`) |
| `PullExtra` (Cost) | each new chain pull: `-WingChainPull * new`, tier `1/tierScale` | `Instance.WingChainPull` 3.0 |
| `Clear` (Outcome) | corridor run, each pack cleared **in route order** since last paid: `+CorridorPack * new`, tier `tierScale` | `Instance.CorridorPack` 4.0 |
| `Threat` (Shaping) | `OnParty > WingCrowdFree`: `-WingCrowd * (OnParty - WingCrowdFree) * seconds` | `Instance.WingCrowd` 0.15, `WingCrowdFree` 4 |
| `Lost` (Cost) | `WingRun::Strays(alive, isLeader, walkingBack, leaderAlive, yards, WingStrayYards)`: `-WingStray * seconds` | `Instance.WingStray` 0.02, `WingStrayYards` 25 |
| `Away` (Cost) | `WingRun::Away(alive, walkingBack)` = dead or `Clock.Rejoining`: `-WingAway * seconds` | `Instance.WingAway` 0.02 |

Paid only to a living bot (`:2568-2625`): `Kill` (Outcome) `+WingTrashKill * newTrashKills`, tier `tierScale`;
`Approach`
(Shaping) `+WingWaypoint * tierScale * newWaypoints`; `Kill` `+WingMidBoss * newBossKills`, tier `tierScale`; and the
forward potential: when not fighting and the route ahead is known, `potential = -(distance to next route point +
RouteRemain[next]) / RouteRemain[0]`, paid `+WingProgress * tierScale * (potential - previous)` when it rises (the max
is kept; the first reading only seeds it). Keys: `WingTrashKill` 1.0, `WingWaypoint` 0.5, `WingMidBoss` 8.0,
`WingProgress` 60.0. Note the tier is multiplied into a Shaping term here as an argument, not as an outcome scale.

Deaths (`:2632-2641`): the first decision a bot is found dead, `Death` (Cost) `-WingDeath` (3.0) tier `1/tierScale`; for
each new wipe `Death` `-WingWipe` (5.0) times the number, tier `1/tierScale`. Both `paid.DeathPaid` and `Deaths` reset
how E3.7 describes.

**Terminal outcome**, paid once per seat when `over` (`BossDead || Wiped || TimeIsUp || drill done || corridor done`,
`:2644-2646`), guarded by `OutcomePaid`:

- Drill: share = 1 for the tank (`DungeonRole == DUNGEON_TANK`), else `PullOthers` (0.5). Extra pack: `PullExtra`
  `-PullExtra * share`, tier `1/tierScale`; else cleared: `PullClean` (Outcome) `+PullClean * share`, tier `tierScale`;
  else time up: `Timeout` (Cost) `-PullTimeout * share`, tier `1/tierScale`. Then returns. Keys `PullExtra` 5.0,
  `PullClean` 5.0, `PullTimeout` 2.0, `PullOthers` 0.5.
- Seat 0 only, once: a `LOG_DEBUG` of route packs never found (`:2655-2670`).
- Corridor: time up and not done: `Timeout` `-WingTimeout * (1 - Corridor.Share())`, tier `1/tierScale`; returns unless
  `BossDead` (a corridor can end on the last boss too).
- `BossDead`: `Kill` `+WingBoss`, tier `tierScale`; and for a whole run (not corridor) with `FullClear` once:
  `Clear` `+WingClear`, tier `tierScale`. Keys `WingBoss` 25.0, `WingClear` 25.0.
- Else time up with a route: `Timeout` `-WingTimeout * (1 - waypoints / Route.size())`, tier `1/tierScale`;
  `WingTimeout` 30.0.

A **wipe ends nothing here** (the wipe latch and its counting are in the first half, `Update`); the run ends when
`fight.Wiped` is set by exceeding the allowed wipes (`Instance.WingWipes` 2 plus the rung's `ExtraWipes`, E3.4).

### E4.8 `SelectTarget`, `TierScale`, `TimeIsUp`, `WriteState`, `IsTerminal` (`:2695-2752`)

- `SelectTarget`: returns the slot the seat has selected if alive; else the nearest living enemy of the slots, with
  slot 0 (the boss while it fights) winning. **Never reached in the dungeon stages**, whose layout carries the sight
  block (the scenario uses the seat's own client selection; cpp-stagescenario.md, `StageScenario.cpp:3062`). It is
  vestigial for the live stages. Always returns true.
- `TierScale`: `tier = Drill ? DrillRung : WingRun::TierOfRung(Rung)` (rung / 2, so rungs 0-1 tier 0 ... rung 8 tier
  4); then `CombatReward::TierScale(Difficulty.TierScale, min(tier, Instance.MaxTierScale))` (`MaxTierScale` default 6,
  never binding for tiers 0-4). The formula is in cpp-rewards-routing.md; the header says `1 + Difficulty.TierScale *
  tier` (`WingRun.h:32`). A training run's tier is the **rung it was drawn on** (`fight.Rung`), not the ladder's current
  one.
- `TimeIsUp`: `EpisodeLengthMs && Elapsed >= Length`.
- `WriteState`: `state[STATE_TIER] = fight.Tier / (max(2, Rows.size()) - 1)`. `Tier` here is the boss-row index, not the
  ladder rung (E3.4).
- `IsTerminal`: `BossDead || Wiped || TimeIsUp || drill cleared/extra || corridor done`. The same expression as `over`
  in `Reward`; the two copies must agree.

### E4.9 `WingRun.h` (pure helpers; tests: `DungeonStagesTest.cpp`)

- `RUNGS_PER_TIER = 2`; `TierOfRung(rung) = rung / 2`.
- `LeaderSeat(standInSeat, standInLeads, tankSeat)`: the stand-in's seat when it leads and exists, else the tank's.
- `Strays(alive, isLeader, walkingBack, leaderAlive, yards, strayYards)`: alive, not leader, not walking back, leader
  alive, `yards > strayYards`. `Away(alive, walkingBack)`: `!alive || walkingBack`. Their disjointness (the same seconds
  never charged twice) is tested by `ARisenSeatWalkingBackIsAwayNotLost` and `LostIsPricedFromTheActualLeader`.
- `SeededPick(count, seed)`, `SweepPick(count, seed)`, `CorridorFirst(...)` as in E4.3.
- `Corridor` (`First`, `End`, `Next`, `InOrder`, `OutOfOrder`, `Noted`): `Note(cleared)` notes each newly cleared pack
  of `[First, End)`, in order if it equals `Next`; `Done()` when `Next >= End`; `Share()` = cleared / length. A pack
  cleared out of order is counted in `OutOfOrder` and never pays `Clear` (only `InOrder` is paid). Note `Done()` needs
  every pack noted, but `Next` skips ahead over out-of-order ones, so a corridor can be `Done` with `InOrder <
  Length`.
- `FightPacks::Note(fighting)`: counts packs joining a fight already holding one. Tested by
  `AChainPullIsAPackJoiningAnotherPacksFight`.

### E4.10 The wing ladder (`WingLadder.h/.cpp`, `StageScenario::WING_RUNGS`)

**What it is.** A nine-rung difficulty ladder for whole-dungeon runs (`StageScenario.h:218-228`). A rung is `{Lift,
ExtraWipes}`: the levels the party is lifted above the dungeon's range (`BeforeLevel`, E3.4) and the wipes spared on top
of `Instance.WingWipes`:

| Rung | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| Lift | 8 | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| ExtraWipes | 4 | 3 | 3 | 2 | 2 | 1 | 1 | 0 | 0 |

Rung 8 is "the evaluation's own conditions"; **every evaluation run is on rung 8** (`fight.Rung = Evaluating ? size-1 :
WingRungNow()`, `InstanceEncounter.cpp:488`). The tier a rung pays at is `rung / 2` (E4.9). The ladder is built in the
`StageScenario` constructor (`StageScenario.cpp:302-303`) from `Instance.WingRungRuns` (window, 40), `WingRungTarget`
(0.6) and `WingRungStart` (0), and exists in every stage, whether or not it has a wing arena (see E4.12).

**State** (`WingLadder.h:48-97`): `_rung` and `_collapsed` (atomics, readable anywhere), `_probes` and `_others` (the
last
`window` progress values), `_sinceRead`, `_reads` (probe means at each read on this rung), `_lower` (the probe mean that
earned this rung). Not thread-safe by itself; `StageScenario::_wingLadderLock` guards every mutating call.

**Probes.** A training run of a whole dungeon from the door is a *probe* with probability `Instance.WingProbe` (0.2)
(`fight.Probe`, `InstanceEncounter.cpp:496`): never an evaluation, a drill, or a run started from a Go-Explore cell.
Probes are what the policy does without help; the rung's other training runs get `Probe = false`. Why only probes:
"the rung's other runs, as the reference, crept up from 0.71 to 0.82 on rung 0 and took the target with them"
(`StageScenario.cpp:1688-1690`). UNVERIFIED: what makes a probe differ in play from other runs (the 0.2 are named
"probes" and measured; `Probe` is the only difference found in this half; check `Build`/`Update` in E3 for any
assistance given to non-probe runs).

**Progress.** Reported at `ResetEpisode` for a run with a route, not an evaluation, not started from a cell
(`InstanceEncounter.cpp:441-447`): corridor = `Corridor.Share()`; else 1 if the last boss died; else `min(1, (TrashKills
+ BossDead) / (HostileTotal + 1))` (0 if `HostileTotal` is 0). A drill does not report to this ladder.

**`Note(rung, probe, progress)`** (`WingLadder.cpp:45-107`), per finished run, returns `{Moved, Alarm, Cleared}`:

1. A run on a rung other than the ladder's current one is ignored (`:48-50`).
2. The progress is appended to `_probes` or `_others`, each trimmed to the last `window` entries; probes also
   increment `_sinceRead`. Only a probe triggers the rest, and only once `_probes` holds a full window (`:63-64`).
3. **Step** (`:66-77`): if not on the top rung and `mean(_probes) >= target`, the rung goes up by one (towards the
   harder,
   "down a rung"), `_probes`, `_others`, `_reads` are cleared, `_lower = probes`, `_collapsed = -1`, and `Result.Moved`
   carries `{From, To, Probes, Others}`. It never steps back (no code path decrements `_rung` except `Follow`).
4. **Read** (`:79-84`): otherwise, one *read* per `window` fresh probes (the sliding mean overlaps, so reads are spaced
   by
   `_sinceRead >= window`): `_reads.push_back(mean(_probes))`.
5. **Collapse** (`:86-107`): `needed = 5` at rung 0 (`COLLAPSE_READS_FIRST`), else 3 (`COLLAPSE_READS`). `floor =
   max(0.1 (COLLAPSE_FLOOR), 0.25 (COLLAPSE_SHARE) * _lower)`. Collapsed means at least `needed` reads exist and the
   last
   `needed` are all below the floor. On the transition to collapsed, `Result.Alarm` is a line (below); on a flagged rung
   whose probes come back to `>= floor` and no longer collapsed, `Result.Cleared`. `_collapsed` is then set to the
   rung while collapsed, else -1. At the top rung the step branch is skipped, so reads and alarms continue there.

**The alarm texts** (the exact strings, `WingLadder.cpp:92-101`):

- rung 0: `WARNING the dungeon ladder's first rung is not learning yet: the probes made {reads} of the dungeon over
  {needed}
  reads of {window} runs, under {floor:.3f}; the ladder does not move on it`
- other rungs: `WARNING the dungeon ladder's rung {now} collapsed: the probes made {reads} of the dungeon over {needed}
  reads of
  {window} runs, under {floor:.3f} (the rung below earned {lower:.3f}); the ladder does not step back by itself -- roll
  back
  to a checkpoint or change the rung`
- cleared: `the dungeon ladder's rung {now} is over the floor again: the probes made {probes:.3f} of the dungeon (floor
  {floor:.3f})`

`reads` is the last `needed` reads joined with ", " at 3 decimals. After a resume `_lower` is 0 (the rung comes from
`WingRungStart`, not from history), so the floor is the absolute 0.1 even above rung 0.

**What the host does with a result** (`StageScenario::NoteWingRun`, `StageScenario.cpp:1685-1727`):
`progress` is clamped to [0,1]. A *step* is logged `LOG_INFO "{stage}: the dungeon ladder steps down from rung {a} to
{b}: the
last {window} probes made {x:.2f} of the dungeon (target {t:.2f}; the rung's other runs {o:.2f})"`. An alarm is logged
`LOG_WARN "{stage}: {text}"` **and** appended to events.log; a clear is logged `LOG_INFO` and appended.

**events.log.** `StageScenario::AppendRunEvent` (`StageScenario.cpp:1729-1746`): if `StageSettings::EventsLog` is
non-empty,
create the directory, and append `<UTC time %Y-%m-%dT%H:%M:%SZ> <stage name>: <line>\n`. The path is
`<RunsDir>/<scenario>/events.log` (`ForgeConfig.cpp:671-672`); mod-animus sets none. It is appended per call with a
fresh
`ofstream` (no lock besides `_wingLadderLock` held by the caller path of `NoteWingRun`; `AppendRunEvent` itself is
`const` and unlocked). The only lines written by C++ are the wing ladder's alarm and clear. A **step is not written to
events.log**, only to the server log. The host shows the alarm in `forge status` (`Console/Progress.cpp:643-651`,
`sim.WingLadderCollapsed` from `AnimusForge.cpp:2169`): `WARNING dungeon ladder | rung 0 | not learning yet: the probes
under
the floor for 5 reads (see events.log in the run directory)` or `... | rung N | collapsed: the probes under the floor
for 3
reads; it does not step back by itself (see events.log in the run directory)`. The literal 5 and 3 are hard-coded in
`Progress.cpp`, duplicating `COLLAPSE_READS(_FIRST)`.

**No stall alarm.** The ladder raises the collapse alarm only. The "stalled" alarm that `forge status` shows
(`Progress.cpp:640-642`, "its gate metric flat for the stage's stall window") belongs to the learner's fade ladders
(`fade.stall_evals`, py-mappo.md); the wing ladder is not a learner fade and nothing counts reads with no new best.
A wing ladder that sits flat below target is silent; `deploy-gate.md` lists this as a wanted alarm.

**Tests**: `WingLadderTest.cpp` (8 tests: steps on probes alone, never steps back, alarm fires once after three low
reads
and clears, reads are fresh probes not a sliding window, first rung warns after five reads, first rung over the floor
never warns, a resumed ladder uses the absolute floor, a follower takes the host's rung).

### E4.11 The cluster's ladder: workers report, the host decides

One ladder for the whole cluster, the host's. Flow:

1. **Worker marks itself a follower** when `FollowClusterRung(rung)` is first called (`StageScenario.cpp:1782-1791`), by
   `RUNG <n>` order (`AnimusForge.cpp:1214-1218`, which stores `_clusterRung` and calls it if a scenario exists) or by
   its
   own PROGRESS build (`AnimusForge.cpp:1141-1143` calls `FollowClusterRung(_clusterRung)` every report). The call sets
   `_wingFollower = true`, clamps the rung to the ladder and `Follow`s it (atomic store). Before the first call a worker
   behaves as a host and would step its own ladder.
2. A follower's `NoteWingRun` does **not** call `Note`: if the run's rung equals the current one, it appends the
   progress
   (3 decimals) to `_wingTallyProbes` or `_wingTallyOthers` (comma separated); a change of rung clears both
   (`StageScenario.cpp:1697-1706`).
3. Each PROGRESS report, the worker calls `TakeClusterTally()` (`:1746-1756`): `"{rung}/{probes or -}/{others or -}"`,
   clears the lists, and sends it as ` wing=<tally>` among the `PROGRESS` fields (`AnimusForge.cpp:1139-1144`,
   `Bridge/ClusterLink.h:50-51`).
4. The host's `ClusterLink` pulls ` wing=` out of each worker's PROGRESS line into `_tallies` and strips it from the
   stored
   progress (`ClusterLink.cpp:319-325`). `TakeTallies()` returns and clears them.
5. Each host update (`AnimusForge.cpp:1080-1092`): for each tally `AddClusterTally(tally)`
   (`StageScenario.cpp:1758-1780`)
   parses `rung/probes/others` and calls `NoteWingRun(rung, probe, progress)` for each value, **others first, then
   probes**;
   so the host's ladder gets the workers' runs in batches. Then, if `ClusterRung() >= 0`, the state is Training, and the
   rung differs from `_clusterRungSent` or the 30-second timer is up, it broadcasts `RUNG <n>`
   (`Bridge/ClusterLink.h:52-53`) and records it.
6. A worker's `RUNG` order calls `FollowClusterRung`, and its next runs are drawn on that rung
   (`InstanceEncounter.cpp:488`, `WingRungNow()`).

A worker's runs on a stale rung (it has not yet received a step) are dropped on the host by the `rung != now` test, so
batches straddling a step are partly lost; nothing counts how many.

**Quirks of the plumbing** (each with its line):

- `StageScenario::ClusterRung()` is `int32(WingRungNow())` unconditionally (`StageScenario.h:240`), so `RUNG` is
  broadcast every 30 s in **every** stage, not only the dungeon ones, and every worker becomes a follower.
- `_clusterRungSent` and the worker's `_clusterRung` are never reset between scenarios (`AnimusForge.h:407-409`,
  `AnimusForge.cpp:1086-1089,1216`). On a stage change where the new ladder's rung equals the last one sent, no
  immediate
  broadcast happens (the 30-second timer does it), and a worker's report-time `FollowClusterRung(_clusterRung)`
  re-imposes
  the *previous* stage's rung on it meanwhile. With `WingRungStart > 0` (a resume) on the host, workers sit on rung 0 or
  on the old value until the first broadcast.
- The host ladder is not persisted; after a restart it begins at `Instance.WingRungStart`, which the operator must set
  to
  the rung reached (`CurriculumTuning.h:243`). That key is in the cluster fingerprint (the curriculum tuning values), so
  all machines must agree.
- The drill ladder (E4.4) is *not* shared; `forge cluster` shows nothing for it.
- Workers' `AddClusterTally` is never called on a worker (guarded by the scenario's role only implicitly: only the host
  reads `TakeTallies`).

### E4.12 `EntranceRespawn.h/.cpp` and the respawn clock (I4)

The behaviour: a dead seat is out for `Respawn.DelayMs` (default 10000), then stands up alive at full health and power
at
the instance entrance and walks back on the controller; the episode goes on (no graveyard, ghost, corpse run or
teleport to the party). It has rejoined once within `Respawn.RejoinYards` (15) of the party's leader (or of the living
party's centroid when the leader is down). The first half (`RiseDead`, E3.7) drives it; this section is the unit.

**`RespawnClock`** (`EntranceRespawn.h:48-122`, pure, per seat): fields `Out`, `DeadSinceMs`, `Rejoining`, `RoseAtMs`,
`Deaths`, `Rises`, `Rejoins`, `RejoinMsTotal`, `OutMsTotal`. `Note(nowMs, alive, partyYards, delayMs, rejoinYards)`
returns
a `Step`:

- not alive, not yet `Out`: sets `Out`, clears `Rejoining`, `DeadSinceMs = max(1, now)`, `++Deaths`, returns `Died`;
- not alive and `Out`: `Rise` once `now >= DeadSinceMs + delayMs`, else `None` (it returns `Rise` on every decision
  until the
  caller calls `Risen`);
- alive and `Out` (stood up by something else, e.g. a friend's resurrection): `Risen(now)` is called inside `Note`, so
  it
  walks back from where it lies;
- alive and `Rejoining` and `0 <= partyYards <= rejoinYards`: `Rejoining = false`, `++Rejoins`, `RejoinMsTotal += now -
  RoseAtMs`,
  returns `Rejoined`. A negative `partyYards` means no party to be with and never rejoins.

`Risen(now)` adds the dead time to `OutMsTotal`, clears `Out`, sets `Rejoining`, `RoseAtMs = max(1, now)`, `++Rises`.
`RejoinSeconds()` = mean ms / 1000 (0 with none); `RejoinedShare()` = rejoins / rises (1 with no rise). These feed the
`wing_rises`, `wing_rejoins`, `wing_rejoin_seconds` columns the dungeon yamls read (`dungeon2_ragefire.yaml:46-47`,
threshold `wing_rejoin_seconds: "<= 90"`, `:69`). A seat that rises but never rejoins stays `Rejoining` for the rest of
the run, paying `Away` (and not `Lost`) every second.

**`WipeLatch`** (`:127-148`): `Note(anyoneAlive, cleared)`: resets when anyone is alive or the dungeon is cleared;
otherwise
the first call counts one wipe (returns true) and the latch holds until someone stands again. The run ends past its
allowance. Because a death never teleports the party to the door, a wipe is "nobody standing", and the risen seats
immediately end it.

**`RiseAtEntrance(bot, seat, entrance, nowMs)`** (`EntranceRespawn.cpp:24-47`): `ForgetFrame(seat)` first (its entity
list
`Seen` and `SightGuids` are cleared, so nothing is in view at the entrance until the camera casts there: principle 1);
returns false if the bot is gone or not in world; if dead, `ResurrectPlayer(1.0f, false)`; `SetFullHealth`; mana and
energy to max (rage/runic untouched); `CombatStopWithPets(true)`; the held keys are zeroed but the face-turn rate is
kept;
`BotFactory::TeleportWithinMap(bot, entrance)`; `StageScenario::StartMover(seat, bot, nowMs)` restarts the controller
from
the server's body. Returns whether the move succeeded (the bot is alive either way). Note this *is* a teleport of a bot
by the sim (movement principle 3 concerns movement, and the rise is a respawn), and `RiseAtEntrance` does not reset
auras, cooldowns, pets or durability (`SetFullHealth` only); UNVERIFIED: whether `RiseDead` (E3.7) does that.

Tests: `DungeonStagesTest.cpp:489-523` (`ARisenSeatWalkingBackIsAwayNotLost`, drives `RespawnClock::Note`: Died at 1000,
Rise at 11000, Rejoined at 60000), `PartyFollowTest.cpp:54-69` (the follow stage's use). There is **no
`EntranceRespawnTest`** though the header comment (`EntranceRespawn.h:30-31`) names one; `RiseAtEntrance`, `ForgetFrame`
and `WipeLatch` have no test of their own (`WipeLatch` is referenced in no test found: grep over `src/test`).

### E4.13 Config keys read by this half

All `AnimusForge.Curriculum.` + key (`StageSettings::TuningPrefix`); read through `CurriculumTuning::Instance`
(`Encounters` reads `_scenario.Tuning().Instance.X`). Defaults from `CurriculumTuning.h:185-277`.

| Key | Default | Used at |
|---|---|---|
| `Instance.WingFullClear` | 1 | route packs: `InstanceEncounter.cpp:1391,1513` |
| `Instance.WingWaypointYards` | 30 (min 5 applied) | route spacing `:1666` |
| `Instance.WingTrace` | 1 | gates the `LOG_INFO` lines of starts and run ends |
| `Instance.WingProbe` | 0.2 | probe share `:496` |
| `Instance.WingRungRuns`, `WingRungTarget`, `WingRungStart` | 40, 0.6, 0 | ladder construction `StageScenario.cpp:302-303` |
| `Instance.WingStall`, `WingStallGraceMs`, `WingStallOthers`, `WingClock` | 0.1, 60000, 0.2, 0.002 | `Reward` |
| `Instance.WingEngage`, `WingReadyShare` | 1.0, 0.8 | `Reward`, `Update` `:739` |
| `Instance.WingChainPull`, `CorridorPack`, `WingCrowd`, `WingCrowdFree` | 3.0, 4.0, 0.15, 4 | `Reward` |
| `Instance.WingStray`, `WingStrayYards`, `WingAway` | 0.02, 25, 0.02 | `Reward` |
| `Instance.WingTrashKill`, `WingWaypoint`, `WingMidBoss`, `WingBoss`, `WingProgress`, `WingClear`, `WingTimeout` | 1.0, 0.5, 8.0, 25.0, 60.0, 25.0, 30.0 | `Reward` |
| `Instance.WingDeath`, `WingWipe`, `WingWipes` | 3.0, 5.0, 2 | `Reward` / `Update` |
| `Instance.PullClean`, `PullExtra`, `PullTimeout`, `PullOthers`, `PullGraceMs` | 5.0, 5.0, 2.0, 0.5, 20000 | drill `Reward` |
| `Instance.PullStartYards`, `PullLift` | 35, 2 | `StartDrill` `:2304`; `PullLift` caps a drill's lift at `BeforeLevel` `:501` |
| `Instance.PullRungStart`, `PullRungRuns`, `PullRungTarget` | 0, 100, 0.7 | drill ladder `:143`, `NoteDrill` |
| `Instance.MaxTierScale` | 6 | `TierScale` |
| `Difficulty.TierScale` | see tuning | `TierScale` |
| `Respawn.DelayMs`, `Respawn.RejoinYards` | 10000, 15 | `RiseDead` (E3.7) |
| `Stage.<name>.GoalPlaces` | `SeenOnly` (1) | `SeenWorld` `:2493` |

The complete table with clamps is in [cpp-tuning-keys.md](cpp-tuning-keys.md).

### E4.14 Tests covering this half

`WingLadderTest.cpp`; `DungeonStagesTest.cpp` (stage shape, `ACorridorsPacksAreClearedInRouteOrder`,
`AChainPullIsAPackJoiningAnotherPacksFight`, `AWingsTierIsItsLaddersRung`, `ThePurposesAreOutcomesAndThePricesCosts`,
`LostIsPricedFromTheActualLeader`, `ARisenSeatWalkingBackIsAwayNotLost`, `AnUnseenPackOrBossNeverReachesTheGoalPlaces`,
`TheLayoutHoldsNoCreatureDataAndNoOrder`, `TheFrontierIsOpenGroundBesideTheUnseen`,
`WailingCavernsIsNeverDrawnInTraining`,
`TheDeadminesDoorsLeversAndCannonAreUsedThroughTheHandlers`); `PartyFollowTest.cpp` (clock use). Not covered by any C++
test found: `UpdateWingEnemies`, `StartAt`, `StartCorridor`, `StartDrill`, `UpdateDrill`, `NoteDrill`,
`Reward` as a whole, `RiseAtEntrance`, `AppendRunEvent`, the cluster tally parse. Those need a live map; the pure parts
were pulled into `WingRun.h`, `SeenPlaces.h`, `WingLadder.h` so they could be tested, and are.

### E4.15 Observed issues (this half)

1. `InstanceEncounter.cpp:2569-2625` (`Reward`): the outer `bool const tankSeat` (`:2551`) is shadowed by `int32
   tankSeat`
   inside `if (bot)` (`:2589`); the leader lookup (`tankSeat`, `LeaderSeat`) is written out twice, here and in
   `SeenWorld`
   (`:2501-2516`), and `TraceWing` (E3.6) chooses the tank again. Three copies of "who is the leader".
2. `Reward` and `IsTerminal` both spell the "run is over" condition (`:2644-2646`, `:2745-2752`); they must stay in
   step.
3. `SelectTarget` (`:2695`) is unreachable in every live stage (the sight block makes the client's selection the target)
   and always returns true: dead code under principle 17 unless a non-sight wing stage is planned.
4. `WriteState` writes `Tier / (rows - 1)` (the boss row), but the reward tier is the ladder rung / 2: two different
   meanings of "tier" in one encounter (`:2739` vs `:2723`).
5. `ClusterRung()` is unconditional (`StageScenario.h:240`): `RUNG` is broadcast in every stage; stale-rung
   re-imposition
   across stages (E4.11).
6. The host's ladder rung is not persisted; resuming needs `Instance.WingRungStart` set by hand
   (`CurriculumTuning.h:243`).
7. `WingLadder` alarm clears silently: `_collapsed` is set to -1 on any non-collapsed read (`WingLadder.cpp:106`), but
   the
   `Cleared` line is only produced when `probes >= floor`; a read still under the floor but not yet `needed` in a row
   clears the flag (and the status warning) with no events.log line.
8. `Progress.cpp:645-651` hard-codes "5 reads" and "3 reads"; `WingLadder::COLLAPSE_READS(_FIRST)` can drift from it.
9. A step is logged only to the server log, not to events.log; the file therefore has alarms without the steps that give
   them context (`StageScenario.cpp:1710-1716` vs `:1717-1726`).
10. `AppendRunEvent` opens, appends and closes per call with no lock and no flush guard; concurrent map threads can
    interleave only if two alarms fire at once (the ladder lock serialises `NoteWingRun`, which is the only caller).
11. `EntranceRespawn.h:30-31` names `EntranceRespawnTest`, which does not exist; `WipeLatch` is untested.
12. `UpdateWingEnemies` fills `env.Targets` from the server's unit list through walls (E4.2); principle 1 depends on a
    later stage of the pipeline hiding them. Needs an owner's confirmation (listed as UNVERIFIED).
13. `StartDrill` may select a pack whose `Gap` is `float max` (no other creature), reported as `999` in `DrillGap`
    (`std::min(pack.Gap, 999.0f)`); the log says "gap 999 yd".
14. Comment/code drift: `Scenario`-level comment in `StageScenario.h:490-491` ("The running route share of training runs
    ... runs on several map threads may lose a step") describes a former running-average design; `WingLadder` is now
    mutex-guarded. `InstanceEncounter.cpp:441` comment "count toward the support's running share" refers to the removed
    support ladder.
15. Long lines over 120 columns in this range: `InstanceEncounter.cpp:2273-2276,1767` (several `LOG_INFO` format
    strings); not
    counted exactly.
16. `Instance.PullRungStart`'s ladder has four fixed rungs (`PULL_GAPS`, `:119`); `PullRungRuns`/`Target` tune only the
    step, so adding a rung needs a code edit.

### E4.16 Reviewer notes

- Is the probe/others split worth its cost? Others are tallied, shipped to the host and averaged only for a log line
  (`Result.Moved->Others`); only the probe mean decides. Dropping `_others` removes half the tally traffic.
- The ladder never steps back and the alarm is advisory. Decide whether a stall alarm (reads with no new best, or flat
  below target) belongs in `WingLadder` (host-side, where the reads are) or in the learner; the deploy gate wants one.
- Persisting the rung: write it to `progress.json`/`stage.json` and read it at resume instead of a conf key that is part
  of
  the cluster fingerprint.
- Replace `ClusterRung()`'s unconditional answer with `-1` for stages without a wing arena, and reset
  `_clusterRungSent` on every `START`.
- `UpdateWingEnemies` is 230 lines doing five jobs (objects, doors, pack bookkeeping, kills, slot ranking, crowd). Split
  before touching it. Its per-decision cost: a grid visit for objects and another for units; fine now, check before
  adding seats.
- `Reward` pays a Shaping term (`Approach`, `Threat`) with `tierScale` passed as the `tier` argument; check
  `RewardLedger::Add`'s score rule (outcome/cost only) before changing categories.
- `fight` state is `EnvInstance` reset by value-assignment `fight = EnvInstance()` at every `ResetEpisode`: large
  vectors reallocate every episode; `routes`/`plan` copies per `Build` (`WingPlan` returned by value with its dense
  route and corner tables) are the likely reset-time cost behind `place_p95_ms`.
- `WingRoute` is cached by `{MapId, Entry}` per process, not per seed: a change to the spawn
  data needs a restart, and Deadmines' `Hostile` filter is evaluated on whichever instance builds first.

### E4.17 UNVERIFIED (check these)

- Live conf values of every `Instance.*` key (defaults above are from `CurriculumTuning.h`).
- Whether the dungeon stages' pack block receives the server-side enemy slots or only seen ones (E4.2).
- Whether `CrowdBlock`/`SeatView::Crowd` are blanked in the dungeon layouts.
- The wire path of `Marks` (Go-Explore archive) from `EnvInstance` to `explore.py`.
- What distinguishes a probe run in play, beyond the flag.
- Whether `RiseDead` resets auras, cooldowns, pets (E3.7).
- That all seats of one env are observed on one thread (`SeenWorld` mutates cached frontier state).
- Line ranges in E4.1 and E4.7 are within a few lines; re-grep before quoting.

