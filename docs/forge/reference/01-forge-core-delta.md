# Forge core delta: how the fork differs from upstream AzerothCore

> **Update 2026-10-08:** upstream `bce7ed5a6` was merged into `forge` (decision 0018). The delta below was measured against
> the old merge-base 37de65eb0 and its list of edited upstream files is still right; the "no caller" claims for
> `PCQueue::Reset` are stale (upstream now calls it), and `git diff master..forge` is now the delta (`master` mirrors upstream).
>
> **Update 2026-10-08, dead-code pass:** deleted from the forge: `Battleground::SetSimOwned/IsSimOwned` and `_simOwned`, the
> `logMissing` argument of `GetBGObject`, `Group::IsSimGroup`, `PathGenerator::SetIncludeFlags/GetIncludeFlags`,
> `MapUpdater::ParallelFor`, `MapMgr::SetMapUpdateInterval` (and its call in `World::SetInitialWorldSettings`), the dead body of
> `Pet::SavePetToDB`, the battleground diagnostic in `MapInstanced::DestroyInstance`, and the whole playtest mode
> (`Forge.Playtest`, `ForgeCore::LoadSettings/Playtest`, `ForgePlaytestLoop`, the realmlist read, the listener). Rows and
> findings below that describe them (F-7, F-8, F-10, F-13 and the playtest text) are history. `ForgeCore::HasClients()` is
> kept and returns false: the forge opens no listener and registers no sim session, so there is never a real client.


Purpose and scope: every change the `forge` branch makes to upstream AzerothCore **outside** the forge's own code
(`src/server/game/Animus`, `apps/forge`, `docs`, `src/test`). For each changed upstream file there is a row in the
table below, and for each kind of change there is a section with the rationale, the files, and the risk of merging
upstream into the fork. The forge's own code is documented in the sibling reference documents (see
[00-architecture.md](00-architecture.md)).

## How this was measured

- Base: the merge-base of `master` and `forge`, `37de65eb0` (2026-09-02, "chore(DB): import pending files").
- `master` today is `386f3a13f` (2026-09-15) and holds 206 commits the fork has not merged. **Comparing against the
  `master` tip is wrong**: it shows upstream's own later work (Ulduar scripts, `Spell.cpp`, e2e, SQL) as if the fork had
  removed it. All counts here are `git diff --numstat 37de65eb0 forge -- <paths>`.
- The fork is 751 commits ahead of the merge-base. Outside the excluded paths the diff is 119 files (table below), plus
  31 deleted files under `e2e/` (-4295 lines), `.agents/docs/e2e-policy.md` (deleted) and
  `.agents/plans/forge-parallel-core/forge-parallel-core.PLAN.md` (added, 1359 lines). `data/sql` is untouched.
- `modules/` holds no mod-animus or mod-animus-forge checkout in this tree (`ls modules`: `CMakeLists.txt`,
  `create_module.sh`, `how_to_make_a_module.md`, `ModulesLoader.cpp.in.cmake`, `ModulesPCH.h`,
  `ModulesScriptLoader.h`).
- Sections are lettered; the table's "Section" column points at them. Line numbers are forge-side.

## Documents versus code: what the old chapter 2 got wrong

The previous `02-forge-core.md` described a design that the code no longer has. Falsified claims, so nobody carries
them forward:

1. "Replace, don't rewrite": replacement members in `Forge/ForgeWorld.cpp`, `ForgeMapMgr.cpp`, `ForgeMap.cpp`,
   `ForgeGameTime.cpp`, with the stock body left as dead code under `return ForgeUpdate(diff);`. **None of these files
   exist.** `src/server/game/Forge/` holds only `Forge.h` and `Forge.cpp`. `World::Update`, `MapMgr::Update`,
   `MapInstanced::Update` and `Map::SendObjectUpdates` are edited **in place** (principle 17: dead code is deleted, not
   gated), and `World::_UpdateGameTime` is deleted. This is also the largest merge risk (section A, F).
2. "`Main.cpp` is excluded from the build, not edited": `src/server/apps/worldserver/Main.cpp` is **deleted** and
   `ForgeMain.cpp` defines `main()` (`ForgeMain.cpp:337`).
3. "`World::Update` drops auction, LFG, OutdoorPvP, WorldState, Battlefield": it **keeps** them
   (`World.cpp:1222` auctions, `1229` and `1247` LFG, `1242` OutdoorPvP, WorldState and battlefields after it); it drops
   the 5-second work, who list, uptime, log cleaning, autobroadcast, game events, mail expiry, sessions (except in
   playtest), dynamic visibility, metrics and ToCloud9.
4. "`MapMgr::ForgeUpdate` reproduces the stock four-step round robin": the round robin is **gone**
   (`MapMgr.cpp:335`); every non-idle map gets the full diff every tick.
5. "`MapUpdater::activate` clears the token and calls `PCQueue::Reset`": `MapUpdater` no longer uses `PCQueue` at all
   (it is a lock-free task array, `MapUpdater.h`); `PCQueue::Reset` (`PCQueue.h:100`) has no caller anywhere in `src`.
6. "`CoreHooks` function-pointer seams filled by the module": the module is part of the core now. The core calls
   `Animus::Hooks::*` directly (`Unit.cpp:987`, `8148`, `8435`; `Spell.cpp:3775`, `4091`; `SpellAuraEffects.cpp:6657`).
   The only remaining mention of `CoreHooks` in code is a stale comment, `EnvPool.h:112`.
7. "`maxTicks` is a hook for batch runs": not in `ForgeMain.cpp`.
8. "`GameTime::ForgeAdvanceGameTimers` in `Forge/ForgeGameTime.cpp`": the function is `GameTime::AdvanceGameTimers`,
   defined in `GameTime.cpp:113`.

## Table of changed upstream files

| File | +/- | Section | What changed |
|---|---|---|---|
| `.devcontainer/devcontainer.json` | +4 -3 | K | container renamed ac-animus-forge-dev-server; `shutdownAction: none` so closing the editor does not stop training |
| `.gitattributes` | +4 -0 | K | `*.amdl` marked binary |
| `.github/workflows/core-build-nopch.yml` | +4 -49 | K | e2e job removed; the clang-18 cell becomes an ordinary matrix cell |
| `.github/workflows/e2e-live.yml` | +0 -421 | K | deleted with the Go e2e suite |
| `.gitignore` | +32 -6 | K | ignores training output, venv; un-ignores `.agents/plans/forge-parallel-core/` and four animus plan folders (see I-4) |
| `AGENTS.md` | +0 -4 | K | four e2e lines removed |
| `CMakeLists.txt` | +4 -2 | I | default build type Release (was RelWithDebInfo); includes `ConfigureLTO` |
| `apps/docker/Dockerfile` | +3 -2 | I | CTYPE Release; llvm, lld, libzstd-dev added (libzstd-dev is no longer needed by the forge) to the build image |
| `apps/docker/Dockerfile.dev-server` | +14 -2 | I | llvm, libzstd-dev (no longer needed), hipcc, libamdhip64-dev, python3-venv added; ccache dir |
| `apps/docker/animus-venv.sh` | +42 -0 | I | new: creates/updates the learner's Python venv on every start |
| `apps/docker/forge-worldserver.sh` | +95 -0 | I | new: the `ac-worldserver` container command (build if asked, conf restore, venv, TensorBoard, exec worldserver) |
| `conf/dist/config.cmake` | +6 -0 | I | options `WITH_LTO`, `FORGE_PGO`, `FORGE_PGO_DIR` |
| `conf/dist/env.ac` | +5 -1 | I | `CTYPE=Release` |
| `docker-compose.cluster.yml` | +25 -0 | I | new: host networking for a cluster machine |
| `docker-compose.yml` | +163 -95 | I | rewritten: project `ac-animus-forge`, `ac-worldserver` runs the forge from the bind-mounted tree, GPU devices, TensorBoard port |
| `forge.sh` | +109 -0 | I | new: `./forge.sh [--build|attach|dev|stop]` |
| `forgectl` | +8 -0 | I | new: shim that runs the `apps/forge/forgectl` package |
| `modules/CMakeLists.txt` | +40 -0 | I | `MODULES_FOLDED_INTO_CORE` (mod-animus-forge): a stale checkout is not built, its config files are still installed |
| `src/cmake/compiler/clang/settings.cmake` | +46 -0 | I | -O3, -march=native, -fno-semantic-interposition, PGO passes |
| `src/cmake/compiler/gcc/settings.cmake` | +47 -0 | I | the same for gcc |
| `src/cmake/macros/ConfigureLTO.cmake` | +77 -0 | I | new: LTO for non-Debug builds (clang needs llvm-ar, llvm-ranlib, lld) |
| `src/common/Collision/BoundingIntervalHierarchy.h` | +30 -7 | G | read-only tree accessors for the GPU copy; a primitive with non-finite or inside-out bounds is left out of the tree (was `std::terminate` in `subdivide`) |
| `src/common/Collision/DynamicTree.cpp` | +35 -5 | G | surface-normal and model out-parameters on `GetIntersectionTime`; read-only listing of models and cells for the camera |
| `src/common/Collision/DynamicTree.h` | +13 -1 | G | declarations for the above |
| `src/common/Collision/Maps/MapTree.cpp` | +39 -4 | G | `GetLiquidIntersection`, `GetSurfaceIntersection`, normal out-parameter, read-only spawn listing |
| `src/common/Collision/Maps/MapTree.h` | +15 -1 | G | declarations for the above |
| `src/common/Collision/Models/GameObjectModel.cpp` | +6 -2 | G | normal out-parameter; accessors |
| `src/common/Collision/Models/GameObjectModel.h` | +21 -1 | G | accessors for the GPU copy |
| `src/common/Collision/Models/ModelInstance.cpp` | +22 -2 | G | normal out-parameter; liquid intersection |
| `src/common/Collision/Models/ModelInstance.h` | +10 -1 | G | declarations |
| `src/common/Collision/Models/WorldModel.cpp` | +190 -10 | G | `IntersectLiquid`, Moeller-Trumbore triangle test, normals, read-only views of group data |
| `src/common/Collision/Models/WorldModel.h` | +26 -2 | G | declarations |
| `src/common/Configuration/Config.h` | +3 -1 | A | `LoadAdditionalFile` made public so `main()` can read the legacy `modules/mod_animus_forge.conf` |
| `src/common/Threading/CpuPlacement.cpp` | +278 -0 | F | new: CPU topology order (largest-L3 die first, physical cores before SMT siblings), pinning |
| `src/common/Threading/CpuPlacement.h` | +62 -0 | F | new: `Order`, `AwayFrom`, `Split`, `Parse`, `PinThisThread`, `PinProcess`, `Describe` |
| `src/common/Threading/PCQueue.h` | +9 -0 | F | `Reset()` added; upstream now calls it (`DatabaseWorkerPool::Open`) |
| `src/common/Utilities/Random.cpp` | +6 -0 | A | `rand_seed(seed)`: restart the calling thread's generator (seeded evaluation episodes) |
| `src/common/Utilities/RandomSeed.h` | +28 -0 | A | new header declaring `rand_seed` |
| `src/common/Utilities/SFMTRand.cpp` | +11 -0 | A | `SFMTRand::Seed(seed)`; the constructor calls `Seed(0)` |
| `src/common/Utilities/SFMTRand.h` | +1 -0 | A | declaration |
| `src/common/Utilities/TaskScheduler.cpp` | +1 -1 | B | `GetNextGroupOccurrence` measures against the scheduler's own `_now`, not `clock_t::now()` |
| `src/server/apps/worldserver/ForgeMain.cpp` | +520 -0 | A | new: the only `main()` of the fork |
| `src/server/apps/worldserver/Main.cpp` | +0 -761 | A | deleted (761 lines) |
| `src/server/apps/worldserver/worldserver.conf.dist` | +1823 -3 | A | adds `Forge.*`, `AnimusForge.*` (414 key lines) and related sections, +1823 lines |
| `src/server/database/Database/DatabaseWorkerPool.cpp` | +140 -0 | C | `Seal`, `NoteSealedWrite`, sealed-pool branches in every query and write entry point |
| `src/server/database/Database/DatabaseWorkerPool.h` | +18 -0 | C | `Seal(bool)`, `IsSealed()`, `_sealed`, `_sealStrict`, `NoteSealedWrite` |
| `src/server/game/Accounts/AccountMgr.cpp` | +100 -0 | C | `LoadSnapshot()`: accounts and access rows in memory so the console still logs in after the seal |
| `src/server/game/Accounts/AccountMgr.h` | +7 -0 | C | declaration |
| `src/server/game/Achievements/AchievementMgr.cpp` | +30 -0 | E | 13 entry points return at once ("sim bots do not use achievements") |
| `src/server/game/Battlegrounds/Battleground.cpp` | +15 -5 | F | `BroadcastWorker` skips players with no session |
| `src/server/game/Battlegrounds/Battleground.h` | +9 -1 | F | (nothing left: `SetSimOwned`, `_simOwned` and `logMissing` deleted) |
| `src/server/game/Battlegrounds/Zones/BattlegroundSA.cpp` | +2 -2 | B | demolisher respawn timers 64-bit |
| `src/server/game/Battlegrounds/Zones/BattlegroundSA.h` | +1 -1 | B | `DemoliserRespawnList` value type `uint64` |
| `src/server/game/CMakeLists.txt` | +79 -0 | I | `FORGE_PYTHON_DIR`, `ForgeSourceHash.h`, the `forge-gpu` shared library built with hipcc |
| `src/server/game/DungeonFinding/LFGMgr.cpp` | +71 -46 | F | `_storeLock`; `PlayerData(guid)` and `GroupData(guid)` replace `PlayersStore[guid]`/`GroupsStore[guid]` |
| `src/server/game/DungeonFinding/LFGMgr.h` | +8 -0 | F | the mutex and the two accessors |
| `src/server/game/Entities/Creature/Creature.cpp` | +10 -5 | H | `PendingSummonLevel` overrides the selected level; 64-bit school lockouts and cooldown ends |
| `src/server/game/Entities/Creature/Creature.h` | +3 -1 | F | `ToUpdatableMapObject()`; `m_ProhibitSchoolTime` is `uint64` |
| `src/server/game/Entities/Creature/CreatureData.h` | +2 -2 | B | `CreatureSpellCooldown::end` is `uint64` |
| `src/server/game/Entities/DynamicObject/DynamicObject.h` | +2 -0 | F | `ToUpdatableMapObject()` |
| `src/server/game/Entities/GameObject/GameObject.cpp` | +13 -13 | B | cooldown arithmetic in 64 bits |
| `src/server/game/Entities/GameObject/GameObject.h` | +3 -1 | F | `ToUpdatableMapObject()`; `m_cooldownTime` is `uint64` |
| `src/server/game/Entities/Item/Container/Bag.cpp` | +4 -0 | D | `BuildCreateUpdateBlockForPlayer` returns without a client |
| `src/server/game/Entities/Object/Object.cpp` | +26 -1 | D | create/destroy/out-of-range/send-to-player/send-to-set return without a client; `AddToObjectUpdateIfNeeded` queues nothing without one |
| `src/server/game/Entities/Object/Object.h` | +7 -0 | F | virtual `WorldObject::ToUpdatableMapObject()` replaces `dynamic_cast` |
| `src/server/game/Entities/Object/ObjectGuid.h` | +9 -5 | F | GUID generators atomic (`_nextGuid`) |
| `src/server/game/Entities/Object/Updates/UpdateMask.h` | +37 -26 | D | one bit per field in 32-bit words instead of one byte per field; the packet form is the storage form |
| `src/server/game/Entities/Pet/Pet.cpp` | +32 -5 | E | `SavePetToDB` returns early; `LoadPetFromDB` runs its completion against an empty holder when the pool is sealed |
| `src/server/game/Entities/Player/Player.cpp` | +45 -19 | D | packet builders gated on `HasClients`; `CreateUnlinked`; spell cooldowns on game time and 64-bit; `ResetMap` unindexes from the map |
| `src/server/game/Entities/Player/Player.h` | +5 -1 | B | `SpellCooldown::end` is `uint64`; `Player::CreateUnlinked` |
| `src/server/game/Entities/Player/PlayerStorage.cpp` | +22 -0 | E | both `SaveToDB` overloads return unless `create`; zero the periodic-save timers |
| `src/server/game/Entities/Player/PlayerUpdates.cpp` | +9 -0 | E | `UpdateAdditionalSaves` drops queued partial saves; `GetInitialVisiblePackets` gated on `HasClients` |
| `src/server/game/Entities/Unit/CharmInfo.cpp` | +5 -4 | B | `GetGlobalCooldown` on game time through wrap-safe `getMSTimeDiff` |
| `src/server/game/Entities/Unit/Unit.cpp` | +37 -2 | D | ten combat-log senders gated on `HasClients`; `Animus::Hooks::Damage/Heal/HealCast`; proc cooldown on `GameTime::Now()` |
| `src/server/game/Entities/Unit/Unit.h` | +2 -2 | B | `DealHeal(..., bool periodic)`; `m_lastSanctuaryTime` is `uint64` |
| `src/server/game/Forge/Forge.cpp` | +60 -0 | A | new: `ForgeCore` (HasClients, tick override) |
| `src/server/game/Forge/Forge.h` | +51 -0 | A | new: `ForgeCore` declarations |
| `src/server/game/Globals/ObjectAccessor.cpp` | +11 -2 | F | `PlayerNameMapLock`; `GetPlayer(Map const*, guid)` answers from the map's own index |
| `src/server/game/Globals/ObjectMgr.cpp` | +8 -0 | F | `SetHighestGuids` creates every global GUID generator up front |
| `src/server/game/Grids/GridTerrainData.cpp` | +151 -73 | G | `resolveLiquid` factored out of `GetLiquidData`; `GetMaxHeight`, `GetCellHeights`, `HasLiquid`, `GetLiquidSurface` |
| `src/server/game/Grids/GridTerrainData.h` | +19 -0 | G | declarations; `gridMaxHeight` |
| `src/server/game/Grids/GridTerrainLoader.cpp` | +9 -2 | F | instance-0 vmap/mmap tile loads are deferred while map tasks run |
| `src/server/game/Groups/Group.cpp` | +37 -20 | E | `m_simGroup`, `IsPersisted()` replace the `!isBGGroup() && !isBFGroup()` tests |
| `src/server/game/Groups/Group.h` | +8 -0 | E | `SetSimGroup`, `IsPersisted` |
| `src/server/game/Handlers/MovementHandler.cpp` | +23 -236 | H | the three movement handlers' bodies moved to `ClientMovement`; this file keeps thin wrappers |
| `src/server/game/Instances/InstanceSaveMgr.cpp` | +33 -10 | E | no bind rows for sim sessions; the weekly/daily global reset skips instances that hold a sim seat |
| `src/server/game/Instances/InstanceScript.cpp` | +4 -0 | C | `LoadInstanceSavedGameobjectStateData` returns on a sealed pool |
| `src/server/game/Maps/Map.cpp` | +153 -13 | F | phase timing, unseen-spawn skipping, `_playersByGuid`, `OnCreateMap` for replicas, update-list removal fix, `SendObjectUpdates` drains without a client, sealed-pool guards |
| `src/server/game/Maps/Map.h` | +123 -0 | F | `UpdateTiming`, `TaskSample`, accrued diff, `GetPlayerByGuid`, `GetCreatedGridTerrainData` |
| `src/server/game/Maps/MapCollisionData.cpp` | +72 -0 | G | `GetLiquidHit`, `GetSurfaceHit` (static and dynamic): the camera's additions only |
| `src/server/game/Maps/MapCollisionData.h` | +13 -0 | G | declarations |
| `src/server/game/Maps/MapInstanced.cpp` | +49 -12 | F | empty children are not ticked; half-batch freeze; heap-trim notice |
| `src/server/game/Maps/MapMgr.cpp` | +275 -31 | F | uniform per-tick map update, continent replicas, deferred tile loads, heap trim, task timing |
| `src/server/game/Maps/MapMgr.h` | +100 -10 | F | declarations for the above |
| `src/server/game/Maps/MapUpdater.cpp` | +262 -103 | F | rewritten: fixed task array, pinned spinning workers, `RunMapTick`, `schedule_work` |
| `src/server/game/Maps/MapUpdater.h` | +90 -18 | F | rewritten interface |
| `src/server/game/Movement/ClientMovement.cpp` | +295 -0 | H | new: `Verify`, `Apply`, `Relocate` (moved from `WorldSession`) |
| `src/server/game/Movement/ClientMovement.h` | +76 -0 | H | new: `Refusal`, `Client`, the three functions |
| `src/server/game/Movement/MovementGenerators/PathGenerator.h` | +8 -0 | H | (nothing left: `SetIncludeFlags/GetIncludeFlags` deleted) |
| `src/server/game/Movement/Spline/MoveSpline.h` | +1 -0 | H | `isParabolic()` |
| `src/server/game/Movement/Spline/MoveSplineInit.cpp` | +9 -0 | D | `Launch` and `Stop` skip the monster-move packet without a client |
| `src/server/game/OutdoorPvP/OutdoorPvPMgr.cpp` | +12 -0 | F | `InitOutdoorPvP` loads nothing while `AnimusForge.Enable` is on |
| `src/server/game/Server/WorldSession.cpp` | +42 -78 | E | sim-session guards; movement-order inbox; `SanitizeMovementFlags` extracted to shared rules |
| `src/server/game/Server/WorldSession.h` | +23 -0 | E | `SetSimSession`, `EnableMovementOrders`, `MovementOrders()`, `SanitizeMovementFlags` |
| `src/server/game/Spells/Auras/SpellAuraEffects.cpp` | +3 -1 | D | `Animus::Hooks::HealCast` and periodic flag on `DealHeal` |
| `src/server/game/Spells/Auras/SpellAuras.cpp` | +6 -1 | D | `AuraApplication::ClientUpdate` gated; proc cooldown on `GameTime::Now()` |
| `src/server/game/Spells/Spell.cpp` | +15 -2 | D | `Animus::Hooks::CastCompleted/CastCancelled`, `NoteCastFailed`; spell-start/pet-cast-result packets gated; 64-bit sanctuary compare |
| `src/server/game/Time/GameTime.cpp` | +63 -0 | B | `AdvanceGameTimers` |
| `src/server/game/Time/GameTime.h` | +4 -0 | B | declaration |
| `src/server/game/World/World.cpp` | +108 -239 | A | `World::Update` rewritten in place; `_UpdateGameTime` deleted |
| `src/server/game/World/World.h` | +0 -1 | A | `_UpdateGameTime` declaration removed |
| `src/server/scripts/Commands/cs_forge.cpp` | +1256 -0 | A | new: the `.forge` console command table (registered as `forge` on the console) |
| `src/server/scripts/Commands/cs_script_loader.cpp` | +2 -0 | A | registers `AddSC_forge_commandscript` |
| `src/server/scripts/EasternKingdoms/ZulGurub/boss_jeklik.cpp` | +2 -2 | B | `_scheduler.Update(diff)` |
| `src/server/scripts/Northrend/Ulduar/Ulduar/boss_xt002.cpp` | +3 -2 | B | two `getMSTime()` reads become game time |
| `src/server/scripts/Northrend/zone_howling_fjord.cpp` | +2 -2 | B | `_scheduler.Update(diff)` |
| `src/server/scripts/Spells/spell_druid.cpp` | +4 -4 | B | Eclipse proc cooldown ends 64-bit |
| `src/server/scripts/Spells/spell_generic.cpp` | +3 -3 | B | apply-time list 64-bit |
| `src/server/scripts/Spells/spell_paladin.cpp` | +1 -1 | B | Sacred Shield cooldown on `GameTime::Now()` |

Not in the table: `src/test/server/game/Animus/*` (48 added test files, no upstream test touched), `apps/forge`, `docs`
and `src/server/game/Animus` (added, 227 tracked files).

## A. Entry point, world loop, console

**What.** `main()` is replaced (`ForgeMain.cpp:337`) and `World::Update` is rewritten in place (`World.cpp:1150`).

`main()` keeps: config loading and the legacy `modules/mod_animus_forge.conf` (read in full with
`LoadAdditionalFile`, `ForgeMain.cpp:361`; this is why `ConfigMgr::LoadAdditionalFile` was made public,
`Config.h:75`), a one-thread IoContext that serves only SIGINT/SIGTERM (`World::StopNow`), OpenSSL setup, script and
module loading, the three databases through `DatabaseLoader` with `AC_MODULES_LIST`, `realm.Id.Realm` from `RealmID`
(`ForgeMain.cpp:166-200`), `SetInitialWorldSettings`, the CLI thread (only if stdin is a terminal,
`ForgeMain.cpp:491`) and SOAP (only if `SOAP.Enabled`, `ForgeMain.cpp:475`). It drops the world listener (except in
playtest, `ForgeMain.cpp:438`), remote access, metrics, AppenderDB, PID file, FreezeDetector, ToCloud9, SecretMgr,
realmlist reads and writes (except playtest, `ForgeLoadRealmInfo`, `ForgeMain.cpp:121`) and Windows code. It installs a
fatal-signal handler that prints a backtrace (`ForgeMain.cpp:313`).

Startup order that matters: `SetInitialWorldSettings` (`:417`) -> optional playtest listener -> `OnStartup` hooks
(`:457-458`, `sAnimusForge->OnStartup()` warms every world table the curriculum will read) -> `ForgeSealDatabases()`
(`:462`, skipped in playtest, see C) -> SOAP -> CLI -> loop. Shutdown: console thread joined, IoContext stopped,
`sAnimusForge->OnShutdown()` (`:511`) stops the learner, then scoped handles unload maps, close databases, unload
scripts.

Two loops. `ForgeUpdateLoop` (`:266`) calls `sWorld->Update(tick)` with a **fixed** diff: `ForgeCore::TickMs()` if a
stage set one, else `DecisionMs / TicksPerDecision / (HalfBatch ? 2 : 1)` (both read straight from the config,
`:272-281`, minimum 1 ms).

`World::Update`, in the order the code runs it (`World.cpp:1150-1275`): advance the clock (`AdvanceGameTimers`) and run the shutdown timer (the body of the deleted `_UpdateGameTime`); step all
interval timers; the daily/weekly/monthly quest, random-BG, calendar and guild-cap resets; `sAuctionMgr->Update`;
`sLFGMgr->Update(diff, 0)`; **`sAnimusForge->OnWorldPrologue(diff)`** (`:1233`);
`sMapMgr->Update`; battlegrounds; outdoor PvP, world state, battlefields; `sLFGMgr->Update(diff, 2)`;
`ProcessQueryCallbacks`; `sInstanceSaveMgr->Update`; `ProcessCliCommands`; **`sAnimusForge->OnUpdate(diff)`** (`:1261`);
`sScriptMgr->OnWorldUpdate`; the MySQL keep-alive ping (`WUPDATE_PINGDB`).

`Forge/Forge.{h,cpp}` is the whole `ForgeCore` namespace: `HasClients()` (false: the forge has no listener),
`SetTickMs/TickMs` (an atomic the
module sets when a stage starts and clears when the plan ends).

`scripts/Commands/cs_forge.cpp` (1256 lines) registers the `forge` console command table (`cs_forge.cpp:196-225`), all
`SEC_ADMINISTRATOR` and console-capable. It calls the `AnimusForge::Forge::Command*` members.

`rand_seed` / `SFMTRand::Seed` (`Random.cpp:81`, `SFMTRand.cpp:64`): restart the calling thread's generator from a seed
(seed 0 = from `std::random_device`, exactly the old constructor). Used by `EnvPool::ResetEnv` to build seeded
evaluation episodes on the world thread.

**Why.** A headless instance-only simulator that runs faster than real time and is steered from a console.
**Merge risk.** `World::Update` is a hot upstream function edited in place: any upstream addition to it is a textual
conflict *and* a silent behavioural one (a new every-tick manager call will simply be missing unless someone re-reads the
function). After a merge, diff the upstream body against this one by eye. `Main.cpp` upstream edits no longer conflict
but never reach the fork either. Check for new upstream work in `Main.cpp` (new subsystems started at boot).

## B. The game clock and 64-bit timestamps

**What.** `GameTime::AdvanceGameTimers(Milliseconds)` (`GameTime.cpp:113`) seeds `GameMSTime`, `GameTimeSystemPoint` and
`GameTimeSteadyPoint` once from the wall clock and then only adds the tick diff; whole seconds derive from the system
point. `UpdateGameTimers()` keeps wall-clock semantics (unit tests, playtest). `getMSTime()` stays on the wall clock.

Sites converted from wall clock to game time (the list in the `GameTime.cpp` comment, confirmed in the diff):
`Player::HasSpellCooldown`, `HasSpellItemCooldown`, `GetSpellCooldownDelay`; the item proc cooldown in
`ApplyEquipCooldown`; `GlobalCooldownMgr::GetGlobalCooldown` (now through the wrap-safe `getMSTimeDiff`,
`CharmInfo.cpp:403`); `Unit::GetProcAurasTriggeredOnEvent`; `Aura::ResetProcCooldown`; `spell_paladin.cpp` Sacred
Shield; `boss_xt002.cpp` (two reads); `boss_jeklik.cpp` and `zone_howling_fjord.cpp` (`_scheduler.Update(diff)` where
upstream passed nothing, so the scheduler ran on the wall clock); `TaskScheduler::GetNextGroupOccurrence`.

Timestamps widened to `uint64` because absolute game milliseconds overflow 32 bits in about 49.7 game days, which the
sim reaches within hours: `SpellCooldown::end` (`Player.h:201`), `CreatureSpellCooldown::end`,
`Creature::m_ProhibitSchoolTime` (`Creature.h:259`), `GameObject::m_cooldownTime` (`GameObject.h:384`),
`Unit::m_lastSanctuaryTime` (`Unit.h:2090`), SotA demolisher respawns, the Eclipse proc ends and the Turkey-marker
list, and the cooldown saving code in `Player` and `Pet`.

**Why.** At simulation speed a wall-clock cooldown lasts thousands of game seconds.
**Merge risk.** Medium and creeping: every new upstream `getMSTime()` or `steady_clock::now()` in gameplay code is a
latent bug in the fork. After each merge grep for them. Left on the wall clock on purpose (comment in `GameTime.cpp`):
session time sync, client movement sync, LFG, arena spectator, world state, game events, battlefield, transports,
scourge invasion, midsummer, `cs_mmaps`, `UpdateTime`. **UNVERIFIED:** whether any of these is reached by a live stage
(e.g. LFG, which the curriculum's dungeon stages may touch).

## C. The sealed database

**What.** `DatabaseWorkerPool<T>::Seal(bool strict)` (`DatabaseWorkerPool.cpp:141`): shuts the async queue down,
clears the async connections, and when `strict` clears the synchronous connections too; sets `_sealed`, `_sealStrict`.
After the seal:

- `Execute`, `DirectExecute`, transaction commits (three variants) drop the write and call
  `NoteSealedWrite(database, key, what)` (`:614`), which logs each distinct kind once per database
  (`:345,374,392,635,648,665,679`).
- Synchronous `Query` (text and prepared) is logged with a stack trace (prepared: once per statement index) and,
  strict, answers **no rows** (`:206-250`).
- `AsyncQuery`, `DelayQueryHolder` **ABORT** (`:282,295,308`); `GetFreeConnection` aborts if strictly sealed (`:590`).
- `KeepAlive` returns at once.

`ForgeSealDatabases` (`ForgeMain.cpp:233`) first calls `AccountMgr::LoadSnapshot()` (`AccountMgr.cpp`), which reads
account names, SRP6 salt and verifier, and access rows into memory; `GetId`, `GetName`, `CheckPassword` and both
`GetSecurity` then answer from it, so the console and SOAP still log in. Strictness is `Forge.SealStrict`, default 1
(`ForgeMain.cpp:227-230`). Callers that would otherwise read the sealed database test `IsSealed()`: `Map::LoadRespawnTimes`
(`Map.cpp:2674`), `Map::LoadCorpseData` (`:3643`), `InstanceScript::LoadInstanceSavedGameobjectStateData`
(`InstanceScript.cpp:811`), `Pet::LoadPetFromDB` (`Pet.cpp:503`, runs the completion lambda against an empty holder).

**Why.** Memory is the truth after startup; a bot is rebuilt many times a second and any write per rebuild backs up the
async queue without bound.
**Merge risk.** Low in lines, high in consequence. Any new upstream code that issues an async query or holder after
startup **aborts the process** (`ABORT`), and any new sync read silently returns nothing. This is found by running a
stage, not by compiling. The `cluster` and `playtest` modes keep the databases open (playtest) or share nothing.

## D. No clients: packet building is skipped

**What.** `ForgeCore::HasClients()` gates every builder that exists only to make a packet for a client:

- `Object::BuildCreateUpdateBlockForPlayer`, `SendUpdateToPlayer`, `BuildOutOfRangeUpdateBlock`, `DestroyForPlayer`
  (`Object.cpp:185,245,271,279`); `AddToObjectUpdateIfNeeded` does not queue the object (`:535`); `SendMessageToSet*`
  (`:2184,2193,2202`); the `Player` and `Bag` overrides (`Player.cpp:3985,4019,5843-5875,11960`; `Bag.cpp:172`);
  `Player::GetInitialVisiblePackets` (`PlayerUpdates.cpp:1716`).
- Ten `Unit::Send*` combat-log senders (`Unit.cpp:6745-8450`, list in the table row).
- `Spell::SendPetCastResult`, `SendSpellStart` (`Spell.cpp:4728,4811`); `AuraApplication::ClientUpdate`
  (`SpellAuras.cpp:231`, the flag reset stays); `MoveSplineInit::Launch/Stop` (`MoveSplineInit.cpp:116,166`, the spline
  itself is still initialised).
- `Map::SendObjectUpdates` with no client drains `_updateObjects` and clears each object's update mask
  (`Map.cpp:1824-1838`).
- `UpdateMask` stores one bit per field in 32-bit words (`UpdateMask.h`), where stock kept a byte per field and
  transposed it into words for every packet. This one is a pure speedup that applies with clients too.

When a real client exists (playtest) all of these behave as stock.
**Merge risk.** Low per site, but **every new upstream packet builder on a hot path is unguarded** until someone adds the
test. Symptom: wasted CPU, not wrong results. Sites are marked either `Forge:` comments or the `HasClients` call; `grep
-rn HasClients src/server/game` lists them.

## E. No persistence for sim characters

**What.** Bots have no database rows after creation.

- `Player::SaveToDB` (both overloads) returns unless `create`, zeroing `m_nextSave`, `m_additionalSaveTimer` and
  `m_additionalSaveMask` (`PlayerStorage.cpp:7199,7221`); `UpdateAdditionalSaves` drops queued partial saves
  (`PlayerUpdates.cpp:2416`). Character creation still saves (`create == true`).
- `AchievementMgr`: thirteen entry points return first thing (`AchievementMgr.cpp:521` onward).
- `Pet::SavePetToDB` removes auras for non-current save modes and returns before any database work (`Pet.cpp:547`).
- `WorldSession::SetSimSession(true)` (`WorldSession.h`, set by `BotFactory::Create`): the destructor skips the
  `account.totaltime` write (`WorldSession.cpp:171`), logout skips `CHAR_UPD_ACCOUNT_ONLINE` (`:872`),
  `InstanceSaveMgr::PlayerBindToInstance` builds and executes its statements only for non-sim sessions
  (`InstanceSaveMgr.cpp:809-860`; also fixes a leak of an unexecuted prepared statement), and the global instance reset
  skips instances with a sim seat (`:783-800`).
- `Group::SetSimGroup(true)` (`PartyEncounter.cpp:202`): membership, party spells and shared kills work, but no group
  or member rows, no character-cache entries, no bind resets or homebind timers; `IsPersisted()` is the single test
  (`Group.cpp:2499`). `Group::Disband` always disbands locally for a sim group.

**Merge risk.** Medium. `Group.cpp` replaced about a dozen `!isBGGroup() && !isBFGroup()` tests with `IsPersisted()`;
a new upstream test of the old form silently persists sim groups (which a sealed pool then drops, so the symptom is a
log line, not a bug). `SaveToDB` early returns hide any new upstream save-time side effect that is not a database write.

## F. Maps, threading and the map tick

**The tick.** `MapMgr::Update` (`MapMgr.cpp:335`) has no round robin and no `IntervalTimer`s: it runs the LFG
compatibility pass inline, sets `MapTasksRunning`, then for each non-idle map (`ForgeMapIsIdle`: not instanceable and
no players, `:55`) schedules a task with `ForgeTickDiff` (`:470`, which accrues the diff and returns 0 for a map frozen
by half-batch). Continent replicas (`i_replicaById`) are scheduled the same way. It then calls `m_updater.wait()`,
`sAnimusForge->OnMapsJoined` (`:386` area), loads deferred tiles, trims the heap (`ForgeTrimHeap`, `malloc_trim`, at most
every 10 s after an instance was destroyed, `:490`) and rolls the per-map timings into `TaskTiming`.
`MapInstanced::Update` (`MapInstanced.cpp:49`) skips children with no players, applies `ForgeTickDiff` and ticks children
as their own tasks; `MapInstanced::DelayedUpdate` no longer walks children (each child's `DelayedUpdate` is in its task).

**The pool.** `MapUpdater` is rewritten (`MapUpdater.cpp`): a fixed array of 16384 `Task`s per tick, claimed with one
atomic increment; worker threads pinned in `CpuPlacement` order (world thread first, `MapUpdater.cpp:63-95`) that spin
4000 rounds then park; `wait()` makes the world thread a worker (`:284`); `RunMapTick` (`:207`) is the whole of a map's
tick: `OnMapPrologue`, `Map::Update`, `Map::DelayedUpdate`, `OnMapEpilogue`; `schedule_work(fn, arg)` for work a map task
hands on (the sim's resets); `MapUpdate.Cpus` ("auto") overrides the CPU order. `PCQueue.h` gained `Reset()`, which upstream now calls from `DatabaseWorkerPool::Open`.

**Continent replicas.** `MapMgr::CreateContinentReplica(mapId, index)` (`MapMgr.cpp:127`) makes further `Map` objects
for one continent, each a child of the base map with its own instance id, grids, spawns and navigation query, sharing
terrain, the vmap tree and the navmesh with the base. A continent stage spreads its envs over replicas so the continent
is not one task. `DoForAllMaps` and `DoForAllMapsWithMapId` visit replicas too (`MapMgr.h:275-307`); `FindMap` resolves
a replica by instance id; `UnloadAll` destroys them first. `Map::OnCreateMap` loads all grids only for instanceable
maps (`Map.cpp:96`).

**Deferred tile loads.** While map tasks run, an instance-0 grid's vmap and mmap tile loads are queued
(`GridTerrainLoader.cpp`, `MapMgr::DeferTileLoad` `:648`) and performed by the world thread after the join
(`LoadDeferredTiles` `:654`); until then that tile answers as if it had no collision data.

**Map-level changes.** Per-phase timing in `Map::Update` (`UpdateTiming`); world spawns that share no phase with any
player are not updated (`Map::UpdateNonPlayerObject`, `Map.cpp:597`); the marked-cell bitset is cleared only on a
recheck tick (`:514`); `_playersByGuid` is a lock-free per-map index (`Map.cpp:288`, removed in `UnindexPlayer`,
`:809`), and `ObjectAccessor::GetPlayer(Map const*, guid)` answers from it instead of the global hash map (this is a
behaviour change for stock too, `ObjectAccessor.cpp:252`). `RemoveFromMap` now always calls
`RemoveObjectFromMapUpdateList` (`Map.cpp:855`; upstream only did on delete; fix for an assert when objects move
between maps, comment cites 2026-09-28). `dynamic_cast<UpdatableMapObject*>` became a virtual `ToUpdatableMapObject()`
(`Object.h:482`).

**Thread-safety fixes for placement on map threads.** `LFGMgr::_storeLock` and `PlayerData/GroupData` (`LFGMgr.h:654`;
two threads inserting corrupted the stores, crash 2026-10-04); `PlayerNameMapLock` in `ObjectAccessor.cpp:114`;
`ObjectGuidGenerator::_nextGuid` atomic (`ObjectGuid.h`); all global GUID generators created at startup
(`ObjectMgr.cpp:7576`); `Player::CreateUnlinked` (thread-local, `Player.h`): a character created on a map thread is
not linked into its race's start map's player list (`Player.cpp:518`).

**Outdoor PvP is off.** `OutdoorPvPMgr::InitOutdoorPvP` returns without loading anything whenever
`AnimusForge.Enable` is true (`OutdoorPvPMgr.cpp:49`): a zone object is shared by all continent copies and crashed
when two registered creatures at once. This reads the module's key from core code, which is a gate in the sense of
principle 17 (Observed issues).

**Battlegrounds.** `BroadcastWorker` skips a player with no session (`Battleground.cpp:121`);
the `SetSimOwned` and `logMissing` additions and the `MapInstanced` diagnostic were deleted in the 2026-10-08 dead-code
pass.

**Merge risk.** **Highest of all sections.** `MapMgr.cpp`, `MapUpdater.{h,cpp}`, `MapInstanced.cpp`, `Map.cpp` and
`Map.h` are rewritten or heavily edited; take upstream changes to them by hand, never by textual merge. Upstream
changes to map update order, grid loading, or `LFGMgr` locking will need reasoning against the lock-free design and the
`MapTasksRunning` contract.

## G. Read-only accessors for the bots' camera

The camera casts rays over the same terrain and collision data the server uses, on the CPU and (with the device library)
on the GPU. The core gained read-only access, with no change to any existing query's result:

- `StaticMapTree::GetLiquidIntersection` and `GetSurfaceIntersection`, a normal out-parameter on the intersection
  routines, read-only listing of spawns and models (`MapTree.cpp:152` and others);
  `DynamicMapTree` model and cell listing, `GetIntersectionTime(..., normal, model)`; `GameObjectModel`, `ModelInstance`
  and `WorldModel` accessors including `IntersectLiquid` and a Moeller-Trumbore triangle test
  (`WorldModel.cpp`).
- `GridTerrainData::resolveLiquid` (the body of `GetLiquidData`, factored out), `GetMaxHeight`, `GetCellHeights`,
  `HasLiquid`, `GetLiquidSurface`; `Map::GetCreatedGridTerrainData` (never creates a grid).
- `StaticVMapCollisionData::GetLiquidHit/GetSurfaceHit`, `DynamicVMapCollisionData::GetSurfaceHit`
  (`MapCollisionData.cpp:117,148,230`).
- A hardening fix: `BIH` leaves out a primitive whose bounds are not finite or inside out
  (`BoundingIntervalHierarchy.h:89-92`; seen as `std::terminate` from `BIH::subdivide`).

Callers: `Animus/Vision/MapVisionWorld.cpp`. Note the dynamic-tree listing may only run
while the owning map is not updating (comment in `DynamicTree.cpp`).
**Merge risk.** Low (additive), except the `GetLiquidData` refactor, which touches upstream logic: re-check it on every
merge of `GridTerrainData.cpp`.

## H. Movement and the player controller

The forge's bots move only through a player controller that reports as a client would (principle 3). To reuse the
server's own movement rules:

- `ClientMovement::{Verify, Apply, Relocate}` (`Movement/ClientMovement.cpp`) are the bodies of
  `WorldSession::VerifyMovementInfo`, `ProcessMovementInfo` and `HandleMoverRelocation`, moved unchanged except that the
  two session-only things (clock sync and kick) are a `ClientMovement::Client` interface. `MovementHandler.cpp` keeps
  thin wrappers (`SessionMovementClient`, `:49`, `448-460`). The forge's `PlayerLink` calls `Apply` directly.
- `WorldSession::SanitizeMovementFlags` (`WorldSession.cpp:1135`) replaces the inline macro list in `ReadMovementInfo`
  with `Animus::Movement::SanitizeFlags` (`Animus/Movement/FlagRules.h`), shared with the controller.
- `WorldSession::EnableMovementOrders()` (`WorldSession.cpp:306`): a sim session keeps the movement-order packets sent to
  it (root, speed, knockback, flying, ...) in an `Animus::Client::Inbox` instead of dropping all its packets, for the
  controller to answer.
- `Spell::SendCastResult` calls `Animus::Movement::NoteCastFailed` (`Spell.cpp:4685`).
- `Creature::SelectLevel` honours `Animus::PendingSummonLevel` (`Creature.cpp:1509`, thread-local, set around a summon).
- `MoveSpline::isParabolic` (`PathGenerator::SetIncludeFlags` was deleted, 2026-10-08).

**Merge risk.** High for `MovementHandler.cpp`/`WorldSession.cpp`: upstream fixes to the movement handlers now land in a
file that no longer holds the logic; they must be ported into `ClientMovement.cpp` by hand. Same for upstream changes to
`ReadMovementInfo`'s flag rules.

## I. Build and container

CMake: default build type Release; `-O3`, `-march=native` (plus `-mtune=znver5/4` on a Zen 5 host), and
`-fno-semantic-interposition` on x86-64 (`src/cmake/compiler/{clang,gcc}/settings.cmake`); `ConfigureLTO.cmake`
(LTO for non-Debug builds; clang needs `llvm-ar`, `llvm-ranlib`, `ld.lld`); `FORGE_PGO=off|generate|use`;
`modules/CMakeLists.txt` drops `mod-animus-forge` from the module list when a stale checkout exists but still installs its
`.conf.dist`. `src/server/game/CMakeLists.txt` (`:53-150`): defines `FORGE_PYTHON_DIR`; hashes every `Animus/*.cpp`,
`.h`, `.hip` file at configure time into the generated `ForgeSourceHash.h` (used by the cluster fingerprint;
renames of files do not change the hash, edits outside `Animus/` do not either); builds
`libforge-gpu.so` with hipcc for `FORGE_GPU_ARCHS` (default `gfx1100`) when `hipcc` exists, else builds without it.

Containers: `docker-compose.yml` is rewritten (project `ac-animus-forge`; `ac-worldserver` runs the forge from the
bind-mounted tree through `apps/docker/forge-worldserver.sh`; `ac-database`, `ac-client-data-init`, `ac-dev-server`
(profile `dev`), stock `ac-db-import`/`ac-authserver` (profile `stock`), `ac-tools`), `docker-compose.cluster.yml`
(host networking), `forge.sh`, `forgectl`, `animus-venv.sh`.

**Merge risk.** CMake and Docker files are replaced, not patched: take upstream's changes by reading them. Changing the
compiler flags, the `Animus/` file set or `FORGE_GPU_ARCHS` changes the build every cluster machine makes.

## J. Scripts

Only clock fixes (section B) and `cs_forge.cpp` registration (A). **Merge risk.** Low; the upstream script files
involved are churned by upstream (Ulduar, Druid), expect small conflicts.

## K. Repository hygiene

Go e2e suite and its workflow deleted (`e2e/` 31 files, `.github/workflows/e2e-live.yml`, `.agents/docs/e2e-policy.md`,
four lines of `AGENTS.md`); `.gitignore` additions; `.gitattributes` binary markers; devcontainer rename. The
`.gitignore` un-ignores `.agents/plans/animus-long-build-fixes`, `animus-team-ctf`, `animus-director`,
`animus-curriculum`, `animus-retrain` plan files, but only `.agents/plans/forge-parallel-core/` exists in this tree
(UNVERIFIED whether the others lived on another branch).

## Merge-risk ranking (most to least)

1. Maps/threading (F), `World::Update` (A): in-place rewrites of hot functions.
2. Movement handler extraction (H).
3. Database seal (C): consequences are runtime aborts.
4. Clock (B): new upstream wall-clock reads.
5. Persistence guards (E), packet gating (D): additive, find by grep.
6. Camera accessors (G), build (I), scripts (J), hygiene (K).

The fork has not merged upstream since 2026-09-02. A merge needs: read the upstream `World::Update`, `MapMgr::Update`,
`Map::Update`, `MapUpdater`, `WorldSession::HandleMovementOpcodes` helpers, and `Group.cpp` hunks against sections A, F, H
and E first; then grep for `getMSTime`/`steady_clock::now` (B), then `Send*Packet` builders (D), then run a stage on the
sealed pool (C).

## Observed issues

Plain-terms list, each with a location.

- **F-1** `World.cpp` comment: "the director drives events itself" (header comment of `World::Update`); the director was
  removed with curriculum v1.
- **F-2** `World.cpp:~1381`: the doc comment of `RescheduleShutdownForWintergrasp` lost its first line when
  `_UpdateGameTime` was deleted (the `/// Defer a pending shutdown...` line is gone; a dangling "Returns true when..." remains).
- **F-3** (fixed 2026-10-08) `ForgeMain.cpp` header said the sim is "single process, never clustered" (TC9 note) beside the cluster feature
  (`AnimusForge.Cluster.*`); it means ToCloud9 but reads wrongly.
- **F-4** (fixed 2026-10-08: the comment now sits above `ForgeUpdateLoop` and says a tick is DecisionMs / TicksPerDecision) `ForgeMain.cpp:213-226`: the doc comment of `ForgeUpdateLoop` ("Fixed-tick world loop. One tick is one agent
  decision ... getMSTime caveat") sits above `ForgeSealStrict`, and `ForgeUpdateLoop` has a different comment of its own.
  "One tick is one agent decision" is only true when `TicksPerDecision` is 1.
- **F-5** `ForgeMain.cpp:361`: still reads a legacy `modules/mod_animus_forge.conf` ("left from the module days") in
  addition to `worldserver.conf`; two places can hold a key and the later one wins.
- **F-6** `OutdoorPvPMgr.cpp:49` reads `AnimusForge.Enable` directly from core code: a config gate in core, contrary to
  principle 17, and a core file that knows a module key.
- **F-7** (fixed 2026-10-08) the `MapInstanced::DestroyInstance` battleground diagnostic was deleted.
- **F-8** (fixed 2026-10-08) the unreachable body of `Pet::SavePetToDB` was deleted.
- **F-9** (stale) `PCQueue::Reset` is called by upstream's `DatabaseWorkerPool::Open`; it is not dead.
- **F-10** (fixed 2026-10-08) deleted: `Battleground::SetSimOwned/IsSimOwned` and the `_simOwned` branches,
  `Group::IsSimGroup`, `PathGenerator::SetIncludeFlags/GetIncludeFlags`, the `logMissing` argument of `GetBGObject`, and
  `MapUpdater::ParallelFor`.
- **F-11** (fixed 2026-10-08) the `EnvPool.h` comment now names `rand_seed`.
- **F-12** `GameTime.cpp:113`: the one-time seed uses a function-local non-atomic `static bool`; fine on the world thread
  only (it is called only there).
- **F-13** (fixed 2026-10-08) `MapMgr::SetMapUpdateInterval` and its call were deleted.
- **F-14** `MapUpdater.h:MaxTasks = 16384`: `Push` asserts if a tick schedules more; the stated margin ("more than the
  largest env count") is not enforced by any config check. UNVERIFIED: the largest env count a stage can configure.
- **F-15** (fixed 2026-10-08) the comments naming deleted curriculum-v1 stages in `OutdoorPvPMgr.cpp`, `Map.cpp` and
  `ForgeConfig.h` were reworded.
