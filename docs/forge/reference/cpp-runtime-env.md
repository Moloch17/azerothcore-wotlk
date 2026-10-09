# C++ runtime: the env pool, the hooks, deferred resets and the session-less bots

Scope: `A/Env/*`, `A/Bot/*` (`A/` = `src/server/game/Animus/`). Entry point and map table:
[cpp-runtime.md](cpp-runtime.md). The scenario that fills the buffers is in [cpp-stagescenario.md](cpp-stagescenario.md);
the observation blocks in [cpp-blocks.md](cpp-blocks.md).

## `Env` and `AgentStats` (`A/Env/Env.h`, `Env.cpp`)

`Env` is a plain struct, one per environment: `Index` (position in pool), `Id` (unique among the server's envs;
`StageSettings::FirstEnvId + index`, used in bot names and account ids), `MapId`/`InstanceId` (the env's map; if both are set
before `Setup` the env is built there, else the first seat's bot opens a new instance), `Bots`/`Targets`/`Allies` (GUID
vectors; objects are resolved per use, never held as pointers across ticks), `EpisodeSeedIndex` (evaluation or replay seed
index, `0xFFFFFFFF` for training), `Evaluating`, `EpisodeElapsedMs/LengthMs/Completed`, `StepStats`/`EpisodeStats`
(`AgentStats` per agent), `StepInterruptedTargets`. `Find{Map,Bot,Target,TargetUnit}` resolve through `sMapMgr->FindMap`,
`ObjectAccessor::FindPlayer`, `Map::GetCreature` (`Env.cpp:26-64`). Limits: `MAX_ALLIES` 4, `MAX_AGENTS` 40, `MAX_TARGETS` 24
(`Env.h:35-43`); `StageScenario.cpp:95-98` `static_assert`s that `MAX_SEATS`, `PACK_SLOTS` fit them.

`AgentStats` (`Env.h:47`): per-decision counters written by the hooks (damage total and by class melee/shot/spell, white and
special, pet, hazard, damage taken, self damage, healing by target kind, raw healing, periodic healing, protection soaked,
movement and speed casts); `Add` rolls a decision into the episode (`:86`). Note `SelfDamage` is not added by `Add`
(not in the list at `Env.h:86-113`): `EpisodeStats[...].SelfDamage` therefore stays 0. This is a likely bug or an
intentional per-step-only counter; UNVERIFIED which.

Written by map threads (the hooks) for the env whose map the thread updates; read by the world thread after the join.

## `EnvPool` (`A/Env/EnvPool.{h,cpp}`)

Owns the envs and the flat env-major buffers the bridge sends: `Obs`, `State`, `Mask`, `Rewards`, `Done`, `Terminated`,
`FinalObs`, `FinalState`, `Layout` (u16 per agent), `Present` (u8), `EpisodeInfo`, `EpisodeSeed`, `KinematicSamples`,
`Image/FinalImage`, `MapCrop/FinalMapCrop` (empty without a camera / map block), `Actions`, `Goals` (2 per agent, -1 =
none), `Look` (`LookHeads` per agent, `FreeLook::NEUTRAL` until an ACT arrives). Sizes are fixed in the constructor
(`EnvPool.cpp:42-100`) from `Scenario::Spec()`.

Construction: `EnvPool(Scenario&, StageSettings const&)`. `_resetOnMapThreads = settings.ResetOnMapThreads &&
scenario.ResetsStayOnMap()` (`:48`). `Setup()` calls `Scenario::Setup(env)` for each env and requires `env.Bots.size() ==
AgentsPerEnv`, then `IndexEnv` (`:102`). `Teardown()` flushes `ResetDefer`, clears the indexes, calls `Scenario::Teardown`
per env (`:121`).

### Groups (half-batch)

`SetGroups(split)`: envs `[0, split)` are group 0, the rest group 1; `GroupCount()` = 2 iff `split < NumEnvs()`.
`GroupOfMap(map)` uses the first env filed under the map; `GroupsKeepToTheirMaps()` checks no map holds envs of both
(`EnvPool.cpp:155-181`). Everything per decision takes the group.

### A decision, in phases

0. (Before the world tick, ForgeMain's loop: `Forge::NextWorldTickMs(nominal)` sizes the tick. With the tick jitter
   (decision 0021) the first tick of a decision plans the whole decision with `Animus::DecisionClock::Plan`: length
   `max(ticks, nominal - carry + overshoot)` split over the decision's ticks. `Env::StepAccruedMs` collects what a
   decision lived; `ObserveEnv` makes it `Env::StepMs` before `Reward`.)
1. World thread, before the maps tick: `AdvanceClock(group, owed)` (`owed` = the game time the group's maps were owed, the
   same `MapMgr::ForgeTickDiff` hands them; it is `diff` without half-batch) then, if the tick ends a decision,
   `BeginDecision(group)` (clears the group's per-env timing/`_observed`/`_finishedOnMap`, marks `_decisionOpen[group]`).
2. Map thread, before `Map::Update`: `ApplyActionsForMap(map)` - for each env on the map, `ApplyGoals`, `ApplyLook` (if the
   buffers exist) and `ApplyActions`; adds its time to `_applyNs` (`:329`). Then `SubTickMap(map, diff, decided)` ->
   `Scenario::SubTick` for every tick of a running group.
3. Map thread, after `Map::Update`, only on a decision tick: `ObserveMap(map)` -> per env `ObserveEnv`: `Scenario::Reward`,
   roll `StepStats` into `EpisodeStats`, `IsTerminal`, `done = terminal || elapsed >= length`, `Done/Terminated`, then
   `Observe` (a done env writes `FinalObs/FinalState/FinalImage/FinalMapCrop` without a mask and, when
   `_resetOnMapThreads`, is marked `_finishedOnMap`; a live env writes `Obs/State/Mask/Image/MapCrop` and
   `DescribeAgents` = layouts, presence, kinematics). Ended envs of the map are reset as a separate work task
   (`MapUpdater::schedule_work(ResetMapEnvs)`, else inline) under a `ResetDefer::Scope` (`:364-398`).
4. World thread, `FinishCollect(group)` once the maps joined (`:406`): `ResetDefer::Flush()`; any env whose map never ticked
   is scored here and an error is logged once ("Env N ... no map task ticked"); ended envs not reset on their map get
   `FinishEnv` here; per-env timings are summed into `_collect`.

`FinishEnv(env, timing)` (`:286`): `EpisodeInfo`, store `EpisodeSeed`, `++EpisodesCompleted`, `ReportEpisode`, `ResetEnv`,
timers into `RecentResets`, then observe the new episode (`Obs` etc.). `ResetAll()` (`:183`) flushes the defer queue, resets
and observes every env and zeroes Rewards/Done/Terminated.

`ResetEnv` (`:670`) seeds evaluation or replay builds: with `_evaluating` and a seed left in the env's run (`_evalNextSeed`
or its data-parallel run), `rand_seed(seedFor(base, index))` after saving a resume seed drawn from the generator
(`rand32() | 1`), the scenario builds, then `rand_seed(resume)`; a replay draws a seed from `_replaySeeds` with probability
`_replayFraction`. The seed draw is under `_seedLock`; the reseed is of the calling thread's RNG. `seedFor(base, index) =
((base+1)*2654435761 ^ (index+1)*2246822519)`, 0 mapped to 1. After `Scenario::Reset` it clears `StepStats`, and queues
the index update through `ResetDefer::Run` (erase old bot/ally GUIDs if they changed, then `IndexEnv`).

`ChooseLocalActions("random", begin, count)` (`:479`): uniform over unmasked actions per agent, action 0 if all masked, and
resets the look row to neutral. Any other policy name returns false.

`SetEvaluation(enabled, seedBase, episodes, baseline, firstSeed)` stores the end of the seed run (`_evalEpisodes = first +
episodes`), `SetEvaluationRuns` gives each learner rank its own run of seeds; `SetReplay` clamps the fraction to [0,1]
(non-finite -> 0).

### Hooks into combat (`RecordDamage`, `RecordHeal`, `RecordHealCast`, `RecordCastCompleted`, `RecordCastCancelled`)

Called from the core through `Animus::Hooks::*` (`A/Env/AnimusHooks.cpp`), which look up the active pool (atomic
pointer, null while nothing runs) and forward. They run on map threads and touch only the env whose instance the calling
thread updates, by GUID lookup in `_agents`/`_allies`/`_envByInstance` (read-only during a tick).

- `RecordDamage` (`EnvPool.cpp:554`): `SELF_DAMAGE` to the same unit is tallied as `SelfDamage` and ends; only
  `DIRECT_DAMAGE`, `SPELL_DIRECT_DAMAGE`, `DOT` continue. Damage on an agent adds `DamageTaken` and, when the spell has a
  persistent area aura effect or an area aura, `HazardDamage`. Damage by an agent (or its charm/owner chain
  `GetCharmerOrOwnerOrOwnGUID`) counts only when the victim is one of the env's targets or another agent of the same env,
  split into melee (white or melee-class spells), shot (ranged class) and spell, plus white/special and pet. Mitigation by
  other agents' `MOD_DAMAGE_PERCENT_TAKEN` auras is credited as protection (`RecordPrevented`, `:625`), reconstructing the
  unreduced hit as `damage / product(multipliers)` with each multiplier floored at 0.01.
- `RecordHeal` (`:791`): effective healing on agents of the same env (self vs other agent, periodic share) or on the env's
  allies. `RecordHealCast`: raw healing to friendly targets of the same env.
- `RecordCastCompleted`: non-triggered casts that teleport/leap/charge/jump count as `MovementCasts`, speed auras as
  `SpeedCasts`. `RecordCastCancelled`: if not cancelled by the caster itself and the caster is alive, and the caster is a
  target or a bot of the env, an interrupt is recorded with what it prevented (`IncomingSpell::Classify`) in
  `StepInterruptedTargets` (`:878-903`).

Call sites in the core: `Unit.cpp:987` (Damage), `:8148` (Heal), `:8435` and `SpellAuraEffects.cpp:6657` (HealCast),
`Spell.cpp:3775` (CastCancelled), `:4091` (CastCompleted).

### Reports

`ReportEpisode` (`:905`) sums the per-agent `EpisodeInfo` of present agents; every `StageSettings::ReportEpisodes` episodes
it stores `_lastEpisodeMeans` (name -> mean) and the count for the status table and logs at debug level. Under `_reportLock`.

## `ResetDefer` (`A/Env/ResetDefer.{h,cpp}`)

A thread-local flag (`Scope`, nestable) says "this thread is inside a map-thread reset". `Run(work)` runs at once on any other
thread; inside a scope it queues behind a global mutex; `Flush()` (world thread, after the join) runs the queue in queue
order, which across map threads is lock-acquisition order, not map order. Used by: `BotFactory::Create` (instance-bind
maps, social list, character cache entry), `BotFactory::Destroy`/`DestroyUnplaced` (logout), `EnvPool::ResetEnv` (index
updates). `Flush` is called from `FinishCollect`, `ResetAll`, `Teardown`. Work queued during `Flush` runs inline because the
world thread has no scope.

## Reset timing (`A/Env/ResetTiming.h`, header only)

`ResetTiming` per-thread accumulators (`CurrentReset`, `thread_local`): Create/Place/Configure/Destroy/Encounter/Stock/
Prepare/Despawn/Seats/Scenario ns. `ResetSamples` keeps the last 1024 resets (`WINDOW`) with nearest-rank p50/p95/max/
mean for placement and whole reset; `Add` and `Summarise` lock a mutex. `Stall(resets, resetMsPerDecision,
decisionMs)`: needs `STALL_MIN_RESETS` = 20 samples; stall if reset time per decision >= 25 % of the decision's wall time or
the p95 reset > max(20 ms, a decision); the cause is placement (placement mean x2 >= total), else reset.
`RecentResets` is the global instance.

## `Kinematics` (`A/Env/Kinematics.h`)

Defines the 10-float sample per agent (`t, x, y, z, yaw, pitch, mode, mounted, speed, in_combat`), `Mode` (ground, swimming,
flying, airborne), `ModeOf`, `Write`, `Clear`. It mirrors the human-capture sample (`apps/forge/python/animus/human/`).

## Bots (`A/Bot/*`)

A bot is a `Player` with a socket-less `WorldSession`, never saved, never registered with `WorldSessionMgr`.

- `BotAccounts` (`BotAccounts.h`): `BASE = 0x7F000000`; seat account = `BASE + env*40*2 + seat*2 + session`
  (`SEATS_PER_ENV` 40, `SESSIONS_PER_BOT` 2); `SEAT_RANGE` 100000 -> `MAX_ENVS` = 1250 (`static_assert >= 1024`); probe
  characters use `BASE - 1 - race`. `ForgeConfig` caps `Envs` to `MAX_ENVS`.
- `BotFactory::Create(spec, session)` (`BotFactory.cpp:70`): builds a `WorldSession(accountId, name, ..., SEC_PLAYER, WotLK)`
  when no session is given, `InitRBACDataForTest()` (so `Player`'s constructor does not query), `SetSimSession(true)` (forge
  core: logout, play time and instance binds write nothing) and `EnableMovementOrders()` (server movement orders are kept
  for the controller to answer); `new Player`, `Player::Create` (with `Player::CreateUnlinked = ResetDefer::Active()` so a
  map-thread create does not link into the race's start continent), queues the instance-bind maps, the social list
  (installed through a private-member access trick on `Player::m_social`, `:41-53`) and the character-cache entry via
  `ResetDefer::Run`; `SetSaveTimer(0)`; sets the level with `SetLevel`+`InitStatsForLevel`+`InitTalentForLevel`+skill
  refresh; full health. `Place*` functions put the bot into a new instance (`MapMgr::CreateMap`), a continent
  (`CreateBaseMap` + `LoadGrid`) or an existing map (`ResetMap`, `Relocate`, `SetMap`, `ObjectAccessor::AddObject`,
  `AddPlayerToMap`). `TeleportWithinMap` calls `TeleportTo` then feeds a synthetic `MSG_MOVE_TELEPORT_ACK`. `Destroy`
  removes transport, resurrects, removes pet/totems, deletes the cache entry, `LogoutPlayer(false)`, unbinds the instance;
  deferred under `ResetDefer`.
- `BotSlot` (`BotSlot.{h,cpp}`): two sessions and two GUIDs alternate so the new bot is created and placed on the idle
  session before the old one leaves (an instance never loses its last player; GUIDs are not allocated per rebuild because
  the core keeps per-GUID state for the server's life). Protocol: `Begin()` (remember the current bot), `NextSession()`,
  `CreateNext(spec, map, mapId, start)`, then `Promote()` (destroy the old one, keep its session, make the new one active)
  or `Abort()` (destroy the new one, keep the old).
- Who builds seats: `StageScenario::BuildSeat` (`A/Scenario/Curriculum/StageScenario.cpp:2403`) names bots
  `Forge<envId>s<seat><a|b>`, picks race from the layout's race list (one team in a party follow), draws gender, calls
  `BotSlot::CreateNext`, sets the phase mask on continents, `UpdatePositionData`, `InitTalentForLevel`, `Configure`.
  `ReuseSeat` (`:2473`) keeps a character across episodes, teleporting within the map and calling `Abort`. Class asset
  discovery builds throwaway probe characters (`ActionCatalog.cpp:306-323`) and deletes them with `DestroyUnplaced`.

## Observed issues

- `BotFactory.h:32` says the bots are built "with public core APIs only, so the same code runs on the forge core and on a
  stock AzerothCore"; the code calls the fork's `SetSimSession`, `EnableMovementOrders` and `Player::CreateUnlinked`.
- `BotSlot::Promote` (`BotSlot.cpp:84-97`) destroys the previous bot even when `CreateNext` produced none (`_created` false);
  callers must use `Abort` on failure (they do at `StageScenario.cpp:1558`, `:2255`, `:2498`).
- `BotSlot.cpp:20,23` include `Player.h` twice.
- `EnvPool.h:194-195`: `ResetSeatsNs`/`ResetScenarioNs` comments are swapped on one line ("Scenario::Reset as a whole // ...
  the seats' loop as a whole").
- `EnvPool.cpp:867`: stray blank line at the end of `RecordCastCompleted`.
- `Env.h:86` `AgentStats::Add` omits `SelfDamage` (and `AgentStats` has no other omission): see above.
- `EnvPool.cpp:890`: comment names deleted stages `stage15_arena`, `stage17_flag`.
- `ResetTiming.h:48-49`: a comment is cut in the middle ("(inside EncounterNs" with no closing).
- `Progress.h`, `ResetTiming.h` and `Kinematics.h` have no `.cpp`; `ResetTiming.h` defines inline globals (`RecentResets`),
  included from `EnvPool.cpp`, `Progress.h` and tests.

## Reviewer notes

- The ownership rule "indexes change only while no map updates" is the key invariant; every new write to `_agents`,
  `_allies`, `_envByInstance`, `_mapEnvs` must go through `ResetDefer::Run` when it can happen on a map thread.
- `ObserveEnv` sets `Done[e]` per env, but the other group's `Done` stays from its last decision; `SendStep` therefore must
  send only the deciding group's range (it does, via `RankGroup`).
- Hooks run on whichever thread applies damage; an agent hit by a creature on another map thread would race on its env's
  `StepStats`, which the design avoids by one env per instance (envs sharing a map share its thread).
