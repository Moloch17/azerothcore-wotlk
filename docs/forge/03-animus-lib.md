# 3. animus-lib

The curriculum layer (namespace `Animus`) is the code that training and play share. It was animus-lib, a
repository of its own bundled into both modules as a git subtree; it now lives in `modules/mod-animus-forge/src`,
and mod-animus keeps its own copy under `animus-lib/`. The chapter is still named after it because that is what
the layer is called throughout the code and the rest of this manual. Because both
modules run the same scenario code, the stage a game master watches in mod-animus is exactly the stage the forge
trained, and a model's observations and actions mean the same thing in both places.

It is an AzerothCore module in its own right, but a passive one. It has no settings, no commands and no world update.
It only registers the combat hooks that env pools need. Hosts (mod-animus-forge, mod-animus) create scenarios and pools
and drive them.

This chapter covers the machinery: scenarios, env pools, bots, core seams, layouts and models.
[Chapter 4](04-curriculum.md) covers the curriculum built on top of it.

## 3.1 Layout of the repository

| Directory | Contents |
|---|---|
| `src/Scenario/` | `Scenario` (the interface a host drives), `StageSettings` (what a host tells it), `CreateScenario`, `SpawnArea` (clearing the spawn point's own creatures), `SpellChecks`, `SummonLevel` |
| `src/Scenario/Curriculum/` | The curriculum: `StageScenario`, `CurriculumTuning`, `StageState`, and the subdirectories below |
| `.../Stages/` | `StageDefinition`, `ArenaDefinition`, the stage list (`Stages.cpp`) and its validation |
| `.../Layout/` | `Block` (interface), `Layout` (block placement, manifests), `SeatView`, `SeatEncoder`, `EncoderSupport` |
| `.../Blocks/` | One class per block: `CoreBlock`, `MoveBlock`, `DuelBlock`, `PetBlock`, `PackBlock`, `GauntletBlock`, `CompanionBlock`, `PartyBlock`, `PvpBlock`, `ContextBlock`, `HostilesBlock`, `TravelBlock`, `FlagBlock`; `Blocks.cpp` (`GetBlock`) |
| `.../Encounters/` | `Encounter` (interface), creature, pulls, owner, party, opponent, ambush, travel and flag encounters, `ScriptedPlayer`, `EnemyPlayers` (building scripted enemy players), `Opponents` (creature pools and spawn points), `EpisodeInfoTable` |
| `.../Character/` | `ClassProfile` (the 10 classes and their 31 specs), `ClassAssets`, `ClassKit`, `TalentBuilder`, `SpecBuilds` (generated), `ActionCatalog`, `GearBuilder`, `GearStats`, `GearEnhancements`, `PetTalents`, `Supplies`, `WorldCreatures`, `SeatCharacter` |
| `.../Rewards/` | `RewardLedger`, `CombatReward` (the shared one-on-one and pull reward terms) |
| `.../Baselines/` | The scripted `greedy` and `fight` policies |
| `src/Env/` | `Env`, `EnvPool`, `PoolRegistry` |
| `src/Bot/` | `BotFactory`, `BotSlot`, `BotAccounts` |
| `src/Core/` | `CoreHooks` |
| `src/Hooks/` | `AnimusLibScripts.cpp`: the damage, heal, spell and creature-level hooks |
| `src/Json/` | Boost.JSON compiled once for the module |
| `tools/spec_builds/` | `builds.py` (standard talent builds and glyphs), `generate.py` (writes `SpecBuilds.cpp`), `validate.py` |
| `cmake/AnimusLibDependency.cmake` | The linkage rules dependents apply |

All three Animus modules share one include path, so **header basenames must be unique across mod-animus-lib,
mod-animus-forge and mod-animus**.

## 3.2 Build and loading

There is nothing to resolve. mod-animus-forge carries these sources in its own `src/`, so AzerothCore's module
machinery collects them like any other module's, and `AddSC_animus_lib()` is called from
`src/animus_forge_loader.cpp` beside the module's own registrations.

mod-animus keeps its own copy under `animus-lib/`, left from when this was a shared repository and a git subtree in
both. It is a plain copy now: nothing updates it, and it tracks nothing.

**The two modules cannot be built together.** They each carry the curriculum, and two copies in one link is a
duplicate-symbol failure with nothing to say why, so `mod-animus.cmake` stops the configure with a message instead.
Disable one: `-DMODULE_MOD-ANIMUS=disabled` for a forge core, `-DMODULE_MOD-ANIMUS-FORGE=disabled` for a realm.
This is not a new restriction in practice -- a forge core has no clients and no use for companions -- but it used to
be a convention and is now enforced.

## 3.3 The scenario interface

A `Scenario` (`src/Scenario/Scenario.h`) defines one MDP. Every call runs on the world thread, outside
`MapMgr::Update`.

| Method | When | Contract |
|---|---|---|
| `Name()`, `Spec()` | Construction | Fixed shapes: agents per env, padded obs dim, state dim, padded action count, episode info dim, longest episode, and the `LayoutSpec` list (name, obs dim, actions per layout) |
| `Setup(env)` | Once per env | Create bots and targets. Fill `env.Bots` (one per agent), `Targets`, `MapId` and `InstanceId`. False if the env could not be built |
| `Reset(env)` | Each new episode | Start an episode in place. The pool has already cleared the episode clock |
| `Observe(env, obs, state, mask)` | Each decision, and for final observations | Write `AgentsPerEnv x ObsDim` features, `StateDim` critic state and `AgentsPerEnv x NumActions` mask bytes. `mask` is null for a final observation, so costly cast checks can be skipped |
| `AgentLayouts(env, layout)` | After `Observe` | Each agent's layout index, constant within an episode |
| `AgentPresence(env, present)` | After `Observe` | 0 for a seat with no character this episode (a partial party) |
| `ApplyActions(env, actions)` | Each decision | One action per agent. Must ignore masked or out-of-range actions safely |
| `Reward(env, reward)` | Each decision | One reward per agent from `env.StepStats` (the pool clears those afterwards) |
| `IsTerminal(env)` | Each decision | True for a terminal state. The time limit ends an episode as a truncation otherwise |
| `IsOpponentSeat(env, agent)` | Evaluation | Whether the agent is the other side of a self-play episode |
| `EpisodeInfo(env, info)`, `EpisodeInfoNames()` | When an episode ends | Per-agent totals, one named column each |
| `ScriptedAction(policy, obs, mask, layout, action)` | Local policies | A scripted baseline's action for one agent's row. False if the policy isn't known |
| `SetLayoutWeights(weights)` | After each evaluation | How often training episodes draw each layout (the learner's `WEIGHTS`). Evaluation episodes ignore it. Optional |
| `Teardown(env)` | Once | Remove bots (without saving) and targets |

`CreateScenario(name, settings)` returns a scenario by name. Currently every scenario is a curriculum stage
(`StageScenario`). A standalone scenario means implementing `Scenario`, adding a branch in `CreateScenario` and listing
it in `ScenarioNames`.

### StageSettings

`StageSettings` is everything a host tells a scenario and its pool:

| Field | Forge value | mod-animus stage viewer value |
|---|---|---|
| `Envs` | `AnimusForge.Envs` | 1 |
| `FirstEnvId` | 0 | The viewer's slot (0..`MaxViewers`-1) |
| `DecisionMs` | `AnimusForge.DecisionMs` | `Animus.Stage.DecisionMs` |
| `EpisodeSeconds` | `AnimusForge.EpisodeSeconds` | `Animus.Stage.EpisodeSeconds` |
| `ReportEpisodes` | `AnimusForge.ReportEpisodes` | 1 |
| `Classes` | `AnimusForge.Classes` | `Animus.Stage.Classes` |
| `SpawnMapId`, `SpawnPosition` | `AnimusForge.SpawnPoint.*` | `Animus.Stage.SpawnPoint.*` |
| `Level` | 0 (the curriculum's random levels; a fast run does not narrow this) | `Animus.Stage.Level` |
| `TuningPrefix` | `AnimusForge.Curriculum.` | `Animus.Curriculum.` |
| `LayoutsDir` | `<OutputDir>/layouts` | empty (writes nothing) |

`FirstEnvId` matters when several pools run in one server. Bot account ids and names derive from `Env::Id`, so pools
that run side by side need separate ranges.

## 3.4 Envs and the env pool

### Env

An `Env` (`src/Env/Env.h`) is one instance and its contents, always held **by GUID** and resolved on each use, never as
raw pointers across ticks:

- `Bots` (one per agent, in agent order; an empty GUID for an empty seat), `Targets` (enemy creatures or players, in
  slot order), `Allies` (scripted friendly players such as the owner)
- `Index` (its place in the pool) and `Id` (unique among the server's envs: bot accounts and names derive from it)
- `MapId`, `InstanceId`
- `EpisodeSeedIndex`: the evaluation seed index of the episode being built or played, or `NO_EPISODE_SEED`
- the episode clock (`EpisodeElapsedMs`, `EpisodeLengthMs`) and `EpisodesCompleted`
- `StepStats` (since the last decision) and `EpisodeStats` (since the last reset), one `AgentStats` per agent
- `StepInterruptedTargets`: targets whose cast was cut short by something other than themselves since the last decision

`AgentStats` holds damage (total, white, special, hits, and the share its pets and guardians dealt), damage taken,
damage taken by allies (total and per ally), effective healing on allies and on other agents, and cast bookkeeping:
casts completed and cancelled, cast time completed and wasted, and why each cancel happened (stopped by the agent,
moved, target lost, other).

### The decision cycle

`EnvPool` (`src/Env/EnvPool.{h,cpp}`) holds every env of one scenario and the flat structure-of-arrays buffers a host
reads and writes. Rows are ordered by env first, then agent:

| Buffer | Shape | Meaning |
|---|---|---|
| `Obs` | E x A x O | Observation after any auto-reset |
| `State` | E x S | Critic state after any auto-reset |
| `Mask` | E x A x N | 1 = action allowed |
| `Layout` | E x A | Layout index per agent |
| `Present` | E x A | 1 = the agent has a character this episode |
| `Rewards` | E x A | Reward for the transition that just ended |
| `Done`, `Terminated` | E | Episode ended; ended in a terminal state |
| `FinalObs`, `FinalState` | E x A x O, E x S | Last observation and state of an ended episode |
| `EpisodeInfo` | E x A x K | Totals of an ended episode |
| `EpisodeSeed` | E | Evaluation seed index of an ended episode, or `NO_EPISODE_SEED` |
| `Actions` | E x A | Filled by the host (learner, model, baseline) before `ApplyActions` |

A host calls:

```cpp
pool.Setup();          // Scenario::Setup for every env, then index its bots, allies and instance
pool.ResetAll();       // Reset + Observe every env; zero rewards and dones
PoolRegistry::Register(&pool);

// every world tick
pool.AdvanceClock(diff);

// every decision
pool.Collect();        // score, end episodes, auto-reset, observe
/* fill pool.Actions */
pool.ApplyActions();

PoolRegistry::Unregister(&pool);
pool.Teardown();
```

`Collect()` runs these steps for each env:

1. `Scenario::Reward` fills this decision's rewards.
2. `StepStats` are added to `EpisodeStats` and cleared. `StepInterruptedTargets` is cleared.
3. `done = IsTerminal(env) || EpisodeElapsedMs >= EpisodeLengthMs`; `terminated = IsTerminal(env)`.
4. If `done`: observe into `FinalObs`/`FinalState` (no mask), write `EpisodeInfo` and `EpisodeSeed`, count the episode,
   add it to the report means, and `ResetEnv`.
5. Observe into `Obs`/`State`/`Mask`, then write `Layout` and `Present`.

So after `Collect`, `Obs` always describes an episode that is running. The done flags and final arrays describe the
episode that just ended. This is the auto-reset convention the learner's GAE expects.

`ChooseLocalActions(policy, opponentsOnly)` fills `Actions` without a learner. `random` picks uniformly among allowed
actions. Any other name goes to `Scenario::ScriptedAction`. With `opponentsOnly`, only the opponent seats are
overwritten. `SetLayoutWeights(weights)` passes the learner's per-layout draw weights to the scenario.

### Seeded resets

`SetEvaluation(enabled, seedBase, episodes, baseline, opponentsOnly)` switches the pool into evaluation. From the next
reset on, envs take seed indexes `0..episodes-1` in the order they reset. `ResetEnv` then:

1. computes `seed = (seedBase + 1) * 2654435761 ^ (index + 1) * 2246822519` (never 0),
2. calls `CoreHooks::SeedRandom(seed)` so the world thread's generator restarts,
3. runs `Scenario::Reset`, which draws the arena, classes, race, level, spec, gear, opponents and spawn points from
   that generator,
4. calls `CoreHooks::SeedRandom(0)` to return to entropy,
5. clears the step and episode stats. Tearing down the old character still reports to the hooks (a cancelled cast, a
   pet's last hit), and none of that belongs to the new episode,
6. re-indexes the env if the reset rebuilt its bots or allies.

Envs that reset after every index has been handed out run unseeded episodes (`NO_EPISODE_SEED`). On a stock core
`SeedRandom` does nothing, so evaluation episodes aren't reproducible there. Only the forge needs them to be.

### Hooks and threading

The library's scripts (`src/Hooks/AnimusLibScripts.cpp`) feed every registered pool:

| Hook | Runs on | Feeds |
|---|---|---|
| `UnitScript::DealDamage` | Map threads | `EnvPool::RecordDamage` |
| `UnitScript::OnHeal` (health actually gained) | Map threads | `EnvPool::RecordHeal` |
| `AllSpellScript::OnSpellCast` | Map threads | `EnvPool::RecordCastCompleted` |
| `AllSpellScript::OnSpellCastCancel` | Map threads | `EnvPool::RecordCastCancelled` |
| `AllCreatureScript::OnBeforeCreatureSelectLevel` | Whichever thread summons | Applies `PendingSummonLevel` |

Damage is measured in `DealDamage`, before the victim's AI runs. A creature script may rewrite the amount in its
`DamageTaken` handler (a training dummy zeroes it), so `OnDamage` wouldn't always see what was dealt.

`RecordDamage` counts only direct, spell-direct and DoT damage:

- damage an agent takes (the victim is the agent itself; pets absorb their own damage),
- damage an ally takes, charged to every agent of its env,
- damage an agent, **or its pets, guardians and totems** (`GetCharmerOrOwnerOrOwnGUID`), deals to its env's targets or
  to another agent of the same env (self-play).

`RecordHeal` credits healing an agent (or its pets) does on its env's allies and on its env's other agents.
`RecordCastCompleted` counts only non-triggered, non-channelled cast-time spells. `RecordCastCancelled` counts only
casts still in their cast time, clamps the time spent to the cast time (pushback can extend it) and classifies the
cause. When the caster isn't an agent, it checks whether the caster is one of its env's targets and was interrupted by
something other than itself or its own death. That feeds the pack stage's interrupt reward.

**Why there are no locks.** Each map thread updates one instance, and each env lives in its own instance, so a hook
only writes the stats of the env whose instance its thread is updating. The lookup tables (bot GUID to env/agent, ally
GUID to env/ally, instance id to env) and `PoolRegistry`'s list change only on the world thread while no map updates
(`Setup`, resets, register and unregister). Map threads only read them.

`PendingSummonLevel` is a `thread_local` that encounters set around `Map::SummonCreature`. Summoning runs
`Creature::SelectLevel` synchronously, and the hook forces that level. Setting the level at selection time, rather
than calling `SetLevel` afterwards, gives the creature the right base health and armor as well as the right attack
tables.

## 3.5 Bots

### BotFactory

`BotFactory::Create(spec, session)` makes a player with no socket and no character row:

1. A `WorldSession` with no socket, `SEC_PLAYER`, the WotLK expansion and account flags 0 (so no collector's edition
   mail, `Player::Create`'s only database write). `InitRBACDataForTest()` gives it default permissions in memory,
   before `new Player`, whose constructor would otherwise run a synchronous login query.
   `CoreHooks::MarkSimSession` marks it a sim session (on the forge).
2. `Player::Create` with the requested GUID counter or a new one. `PlayerCreateBoundInstancesMaps`. An empty social
   list, assigned to the private `Player::m_social` through an explicit template instantiation, which ignores access
   checks, so no core setter is needed.
3. `SetSaveTimer(0)` (never autosave). If a level was requested: `SetLevel` (not `GiveLevel`, which sends level-reward
   mail), `InitStatsForLevel`, `InitTalentForLevel`, and weapon and defense skills raised to the level's cap and filled
   (otherwise every swing would roll against skill 5 and miss).
4. Full stats and health, and a character cache entry.

**The session is never registered with `WorldSessionMgr`.** A socketless session registered there would be deleted on
the next update, logging its player out with a save. Once placed on a map, the bot is updated by `Map::Update` through
`MapSessionFilter` and `Player::Update`, like any player.

Placement:

- `PlaceInNewInstance(bot, mapId, pos)`: `sMapMgr->CreateMap(mapId, bot)` gives a groupless player with no bind a new
  dungeon instance.
- `PlaceOnContinent(bot, mapId, pos)`: onto a continent's shared base map (`CreateBaseMap`), its grid loaded first.
  `BotSlot::CreateNext` picks this or a new instance by whether the map is instanceable; the scenario puts each env of
  a continent stage in its own phase.
- `PlaceInMap(bot, map, pos)`: reset the map `Player::Create` chose (the race's start zone), relocate, set the map and
  fall information, register with `ObjectAccessor`, `AddPlayerToMap`.
- `TeleportWithinMap(bot, pos)`: anywhere on the bot's own map, an instance included (the flag match's bases). The
  teleport is completed by calling the session's own `HandleMoveTeleportAck`, as the absent client would.

`Destroy(bot, keepSession)` resurrects a dead bot (otherwise logout would repop it at a graveyard, which is a far
teleport), removes its pet without saving (`PET_SAVE_AS_DELETED`) along with totems and guardians, deletes the cache
entry, calls `LogoutPlayer(false)` (no save) and unbinds its instance. The bind's database row is deleted only when
bots aren't sim sessions, that is on a stock core. With `keepSession` the session is returned for reuse.

### BotSlot

A `BotSlot` (`src/Bot/BotSlot.h`) is one bot that is rebuilt every episode: a seat, an owner, an opponent or an
ambusher. It owns **two sessions and two GUID counters** and alternates between them:

```cpp
slot.Begin();                                        // remember the current bot
spec.AccountId = BotAccounts::Seat(env, seat, slot.NextSession());
Player* bot = slot.CreateNext(spec, map, mapId, start);   // new bot on the idle session, placed
...                                                  // configure it
slot.Promote();                                      // destroy the old bot (keep its session); the new one is active
// or slot.Abort();                                  // destroy the new bot; the old one stays
```

This gives three guarantees:

- **An instance never loses its last player.** The new bot enters before the old one leaves, so the instance isn't
  unloaded between episodes and the env keeps its map.
- **No per-episode allocations of sessions or GUIDs.** The core keeps some per-GUID state for the life of the server
  (`InstanceSaveMgr` bind storage), so a new GUID per rebuild would grow memory forever.
- **Rollback.** If any seat of an env fails to build, every slot aborts and the previous characters stay.

`Active()` finds the bot through its session rather than `ObjectAccessor`, because a bot in the middle of a far
teleport is out of the world but still has to be destroyed.

### Account ids

`BotAccounts.h` gives each kind of bot its own id range, far above anything a real realm allocates (`BASE =
0x7F000000`):

| Kind | Id |
|---|---|
| Seat | `BASE + env * 4 * 2 + seat * 2 + session` |
| Owner | `BASE + 100000 + env * 2 + session` |
| Opponent | `BASE + 300000 + env * 2 + session` |
| Ambusher | `BASE + 500000 + env * 2 * 2 + ambusher * 2 + session` |
| Spell probe (per race) | `BASE - 1 - race` |

`MAX_ENVS = 100000 / 8 = 12500`. `AnimusForge.Envs` is capped there. mod-animus companions use a separate base
(`0x7E000000`). Bot names contain digits (`Forge3s0a`), which real character names can't, so they never collide with a
player's.

## 3.6 Core seams (`CoreHooks`)

```cpp
struct Seams
{
    void (*MarkSimSession)(WorldSession*);
    void (*MarkSimGroup)(Group*);
    void (*SeedRandom)(uint32);
};
```

A host installs the seams once, at load, before any bot exists. mod-animus-forge installs all three (see
[chapter 2, section 2.9](02-forge-core.md#29-seams-for-modules)) and every call assumes they are installed: this
module only ever runs on the forge core. (mod-animus's copy of the layer keeps the no-op fallbacks a stock core
needs.)

**Rule:** the curriculum layer must never call a forge-only API directly. Add a seam instead.

## 3.7 Layouts and manifests

A **layout** (`Layout/Layout.h`) is what one class policy sees and does at one stage. `Layout::Build(profile,
stage)`:

1. gets (or builds, which takes seconds per class) the profile's `ClassRoleAssets`: allowed races, `ClassKit`
   (trainer spells by level, resolved learn-spells), `TalentBuilder`, `ActionCatalog` and `GearBuilder`,
2. if the stage has the companion or party block, collects `AllyHeals` (sustain spells that are positive and need an
   explicit unit target) and `AllyRevives` (resurrections, plus the soulstone for warlocks),
3. places each of the stage's blocks in order. A block's slice starts where the previous block's ended:
   `BlockSlice{ObsFirst, ObsCount, ActionFirst, ActionCount}`.

So the observation row is `[core][duel][pack]...` and the action list is `[core actions][duel actions][pack
actions]...`. The learner pads every row to the largest layout of the run and tags it with the layout index.

`Layout::Manifest()` is compact JSON (format 3) of everything the layout's meaning depends on. Abridged, with sizes
that change whenever a block does:

```json
{"format":3,"model":"warrior_tank_duel","stage":"stage8_duel","class_role":"warrior_tank","class":1,"role":"tank",
 "obs_dim":...,"num_actions":...,"specs":[2],
 "blocks":[{"name":"core","obs":[0,...],"actions":[0,...],"action_features":5,
            "catalog":[{"kind":"noop"},{"kind":"cancel_queued"},{"kind":"spell","first_rank":71,"next_swing":false},...],
            ...},
           {"name":"duel","obs":[...],"actions":[...],"stable_slots":0,"stable_features":5},
           {"name":"pet","obs":[...,0],"actions":[...,0]}]}
```

A block a class has no use for (the `pet` block for a warrior) still appears, with no features and no actions.

Each block adds its own entries through `Block::DescribeManifest`: the catalog and talents (core), stable slots (duel),
enemy slots (pack), food and drink (gauntlet), revives (companion), member slots (party), friend slots, rank tiers and
buff groups (support).

**Any change that affects the manifest invalidates models.** That includes a new feature in a block, a new spell in a
catalog (a new spell rule, a different talent build), a block added to a stage, or a different class list.
mod-animus's `ModelLibrary` compares the exported manifest with the one its server builds, character for character
(trailing whitespace ignored), and refuses a model on any difference. Retrain and export again.

## 3.8 Models

### The `.amdl` format

`python/animus/export.py` writes one `.amdl` per layout; the forge only writes them (`forge export`), and mod-animus's
`MlpPolicy` reads them ([chapter 6](06-animus.md)). All values are little-endian:

```
char[4]  "AMDL"
u32      version (2)
u16      model name length, then the name (UTF-8, no terminator)
u32      obs_dim
u32      num_agents     the input is the observation followed by a one-hot agent id
u32      num_actions
u32      layer_count
per layer:
  u32    in_dim
  u32    out_dim
  f32    weight[out_dim * in_dim]    row-major
  f32    bias[out_dim]
u32      recurrent_size              0 = no memory
if recurrent_size:
  f32    weight_ih[3 * recurrent_size * trunk_out]    as torch.nn.GRUCell stores them (r, z, n)
  f32    weight_hh[3 * recurrent_size * recurrent_size]
  f32    bias_ih[3 * recurrent_size]
  f32    bias_hh[3 * recurrent_size]
u32      goal_count                  0 = no goals
u32      goal_every_decisions
if goal_count:
  f32    goal_weight[goal_count * feature_width]      the goal head, over the same features
  f32    goal_bias[goal_count]
  f32    goal_embedding[goal_count * feature_width]   added to what the action head reads
```

**Every layer's input is the previous layer's output, except the action head's.** With a memory the head reads the
GRU's state rather than the trunk's, so the head's input width is the memory's when there is one and the trunk's
when there is not. `animus.export.read_amdl` and `reference_decide` (`python/animus/export.py`) read the file back
and decide from it in plain numpy, which is what the export tests check the writer against.

## 3.9 The stage description (`stage.json`)

When `StageSettings::LayoutsDir` is set, `StageScenario` writes `<LayoutsDir>/<stage>/` at construction: one manifest
per layout and `stage.json` (format 2). Each file is written to a `.partial` file and renamed, and only when its content
changed. The learner reads `stage.json` for:

| Key | Used for |
|---|---|
| `stage`, `suffix`, `summary`, `seats`, `blocks` | Identification |
| `extends`, `seed_chain` | Seeding: the ancestors, closest first |
| `merges` | Merge seeding and distillation teachers |
| `arenas` (name, weight, seats, episode seconds, pvp, ambushers) | Per-arena evaluation and gates. The `arena` episode-info column indexes this list |
| `state.arena_first`, `state.arena_count` | Where the arena one-hot sits in the critic state (distillation) |
| `models` | Class/role to model name (export) |
| `layouts.<class_role>` (obs dim, action count, block spans) | Block-by-block seeding and distillation mapping |
| `episode_info` | Column names |
| `tuning` | Every effective `CurriculumTuning` value. It is copied into each run, so a run records what it trained with |

## 3.10 Using the library from a new host

1. Call `AddSC_animus_lib()` first in your module's loader: the damage, heal, spell and creature-level hooks.
2. Install `CoreHooks` at load, before any bot exists: the seams are the forge core's, and every call assumes them.
3. Build `StageSettings` from your own config. Choose a `TuningPrefix` and a `FirstEnvId` range that doesn't overlap
   another host's.
4. `CreateScenario`, construct an `EnvPool`, then `Setup`, `ResetAll` and `PoolRegistry::Register`.
5. From a `WorldScript::OnUpdate` (world thread, outside map updates): `AdvanceClock` every tick, because game time
   accrues whether or not anyone decided. Every `DecisionMs` of accumulated time, `Collect`, fill `Actions`,
   `ApplyActions`. The two clocks are separate on purpose: a host ticking faster than it decides gets smoother
   splines and auras for free, and the library does not care which it is given as long as `AdvanceClock` is handed
   the real diff.
6. On stop: `Unregister` before `Teardown`, because the hooks must stop feeding a pool before it is destroyed.

