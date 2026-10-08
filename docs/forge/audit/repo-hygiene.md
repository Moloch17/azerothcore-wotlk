# Audit: repository hygiene and technical debt of the forge fork

Scope: the fork as a whole, with the Animus C++ and the Python learner covered only where they touch the repository,
the upstream relationship, the build, the tooling or leftovers. Branch `forge` at `93cbc38ea`, audited 2026-10-07.
Analysis only: nothing was built, run or changed. `UNVERIFIED` marks what could not be checked read-only. Every
`path:line` is of that commit unless it says `upstream/master` (`bce7ed5a6`) or "merge result" (the tree of
`git merge-tree --write-tree forge upstream/master`, object `54dd8dc6ad4d949d069e250fda55bb92c936361c`, written to the
object store only). Related, not repeated: [../reference/01-forge-core-delta.md](../reference/01-forge-core-delta.md)
(its "Observed issues" F-1 to F-15), [../reference/known-issues.md](../reference/known-issues.md) (A1 to G4),
[../principles.md](../principles.md).

Handling rule used throughout: the tracked tree holds private LAN addresses and ssh login names. They are cited by file
and line and never reproduced here.

## Summary

The repository is cleaner than a fork of this age usually is. Nothing large of the fork's own is tracked (the largest
forge blob in 751 commits is 634 KB, `Unit.cpp`; the biggest forge-only files are `.amdl` goldens of about 200 KB), the
14.2 GiB pack is upstream history, `apps/codestyle/codestyle-cpp.py` passes with no finding, and there is not one
TODO, FIXME, HACK, XXX or `#if 0` in the 96 forge `.cpp` and 129 `.h` files under `Animus/`, in `apps/forge`, or in the
lines the forge added to upstream files. The debt is elsewhere:

1. **The upstream merge is more expensive than the documentation says, and one failure hides behind a clean merge.**
   Upstream is 494 commits ahead (the delta document says 206). A dry run conflicts in 6 content files and one
   modify/delete that matters (`Main.cpp`), and the textual merge of `PCQueue.h` is clean but **does not compile**:
   forge and upstream each added `PCQueue::Reset()` and the merge keeps both (merge result `PCQueue.h:100` and `:116`).
   Upstream's "any WorldObject can cast" refactor (`fe6913668`) moves `SendSpellMiss` out of `Unit`, so the forge's
   `HasClients` gate on it is silently lost, and `Spell::m_caster` stops being a `Unit*`, which the `Animus::Hooks`
   signatures assume. Upstream also added headless sessions (`92fed92ea`), which make the forge's `m_simSession`
   largely redundant.
2. **The cluster cannot tell that two machines run different cores.** The build fingerprint hashes `Animus/` only; the
   many in-place edits of `Map.cpp`, `MapUpdater.cpp`, `World.cpp` and the rest, the compiler flags (`-march=native` on
   five different CPUs) and the Python learner are outside it, and a missing hash header degrades silently to the
   string `"unhashed"`.
3. **Operational build facts contradict each other.** Three different default build types (`Release`,
   `Release`, `RelWithDebInfo`), a GPU architecture flag that one comment says breaks and the CMake passes anyway, a
   test container (`claude-syntax`) that no file in the repository creates, and a test script that needs
   `/usr/lib/llvm-17` which the dev Dockerfile does not install.
4. **No CI runs for the fork**: 15 of 16 workflows are gated on the upstream repository's name; the forge's 46 GTest
   files and 95 pytest files run only by hand.
5. **Image and tree weight.** `.dockerignore` does not exclude the in-tree 15 GB `.venv`, 3.1 GB `probes/` or 580 MB
   `models/`, and `Dockerfile.dev-server` does `COPY apps`. The dev image also installs a MySQL server, two compilers
   and a full Boost, from an unpinned base.
6. **Exposure is LAN-only but unauthenticated.** Cluster ports bind every interface with no authentication beyond an
   integrity fingerprint, and the control socket's line buffer is unbounded.
7. **Documentation drift.** Two of the eight numbered manual chapters (1,253 lines) still describe
   `modules/mod-animus-forge/src`; `AGENTS.md` says nothing about the forge; comments cite gitignored plan files.

## Ranked findings

Severity: High = will break or mislead the next deploy or merge; Medium = costs time or hides a risk; Low = tidiness.
Effort: S under a day, M a few days, L a week or more. "Risk of fixing" is the chance the fix itself breaks something.

| ID | Area | Finding | Evidence | Sev | Eff | Risk |
|---|---|---|---|---|---|---|
| R-01 | Upstream | Clean textual merge yields a duplicate `PCQueue::Reset()`: compile error | merge result `PCQueue.h:100,116`; `forge:PCQueue.h:100`; `upstream:PCQueue.h:107` | High | S | Low |
| R-02 | Upstream | `fe6913668` moves `SendSpellMiss`, `SendSpellNonMeleeDamageLog` to `WorldObject`; the forge's `HasClients` gate on the `Unit` copy is lost; `Animus::Hooks` take `Unit*` | merge result `Unit.cpp:6656-6675` (conflict), `Object.cpp:3855`; `AnimusHooks.h:45-49` | High | M | Med |
| R-03 | Upstream | Fork is 494 commits / 519 files behind, not 206; 6 content conflicts, `Main.cpp` modify/delete, 27 e2e modify/deletes | `01-forge-core-delta.md:11-13`; `git rev-list --count forge..upstream/master` = 494 | High | M | Med |
| R-04 | Upstream | `Main.cpp` upstream gained four lifecycle hooks and session-end bookkeeping that `ForgeMain.cpp` never sees | `upstream:Main.cpp` (`SaveSessionEnd`, `OnModuleDatabasesLoading/Closing`, `OnDatabaseWarnAboutSyncQueries`) | Med | S | Low |
| R-05 | Upstream | Upstream headless sessions (`_headless(!sock)`, `IsHeadless`, `OnPlayerCanMarkAccountOffline`) overlap the forge's `m_simSession`; the two conflict hunks in `WorldSession.cpp` are exactly this | `WorldSession.cpp:171,872`; `BotFactory.cpp:76`; merge result `WorldSession.cpp:174-178,885-891` | Med | M | Med |
| R-06 | Build | Cluster fingerprint hashes only `Animus/{*.cpp,*.h,*.hip}`; core edits, flags and Python are invisible; fallback is the string `"unhashed"` | `src/server/game/CMakeLists.txt:61-72`; `AnimusForge.cpp:64-68,99` | High | M | Low |
| R-07 | Build | `-march=native` on a mixed fleet gives per-machine FP behaviour (FMA contraction) that no fingerprint or test covers | `clang/settings.cmake:50-70`; `VisionDevice.h:45-46`; `cluster.md` machine table | Med | M | Med |
| R-08 | Build | Three build-type defaults: CMake and `env.ac` say Release; the worker container and test script say RelWithDebInfo | `CMakeLists.txt:92`; `env.ac:19`; `forge-worldserver.sh:24`; `forgectl-test.sh:34` | Med | S | Low |
| R-09 | Build | GPU arch contradiction: Dockerfile says an explicit `--offload-arch=gfx1100` yields kernels that fail to load; CMake passes exactly that | `Dockerfile.dev-server:62-64`; `game/CMakeLists.txt:113,122` | Med | S | Med |
| R-10 | Build | authserver (and its LTO link) is built on every worker: `CAPPS_BUILD` defaults to `all` | `conf/dist/config.sh:88`; `forge-worldserver.sh:33-35`; `env.ac` sets none | Med | S | Low |
| R-11 | CI | No forge test runs on any push: 15 of 16 workflows are gated to the upstream repo | `.github/workflows/*.yml` (`if: github.repository ==`) | Med | M | Low |
| R-12 | Tooling | The test container `claude-syntax` is created by no tracked file; `forgectl-test.sh` needs `/usr/lib/llvm-17` which the dev image does not install | `cluster.toml:24`; `deploy-gate.md:22`; `forgectl-test.sh:53-58`; `Dockerfile.dev-server:35` | Med | S | Low |
| R-13 | Docker | `.dockerignore` lacks `apps/forge/python/.venv` (15 GB), `probes` (3.1 GB), `models` (580 MB); `COPY apps` | `.dockerignore:1-11`; `Dockerfile.dev-server:109`; `du` of the owner checkout | High | S | Low |
| R-14 | Docker | Dev image: unpinned `ubuntu:24.04` and apt, `mysql-server` inside the image, gcc and clang, `libboost-all-dev`, passwordless sudo | `Dockerfile.dev-server:10,35-44,87` | Low | S | Low |
| R-15 | Python | torch unpinned (`>=2.4`, version var empty); dev venv has 2.9.1+rocm6.4 while `blas.py` measured 2.10+rocm7.0 and 2.13+rocm7.1 on cluster cards | `pyproject.toml:12`; `animus-venv.sh:31`; `docker-compose.yml:140`; `blas.py:6-8` | Med | S | Med |
| R-16 | Python | The venv lives in the source tree with container-absolute paths, so a host python cannot use it, and the cluster is set up by copying the tree | `.venv/pyvenv.cfg`; `animus-venv.sh:6-9`; `game/CMakeLists.txt:58-60` | Med | M | Med |
| R-17 | Safety | Cluster control port 7700 binds all interfaces, no authentication, unbounded line buffer; `REGISTER` lets a peer name the address the host's learner dials | `ClusterLink.cpp:127,164-180,287-291` | Med | M | Low |
| R-18 | Safety | Data port (`tcp://` listener) binds all interfaces by default; gloo 7702 and its ephemeral ports are unauthenticated | `LockstepServer.cpp:73`; `docker-compose.cluster.yml:10` | Med | M | Low |
| R-19 | Modules | `MODULES_FOLDED_INTO_CORE` names the dead `mod-animus-forge`, and the build still installs its `.conf.dist` from the stale checkout | `modules/CMakeLists.txt:20-29,374-393` | Med | S | Med |
| R-20 | Config | The real per-machine config is `modules/mod_animus_forge.conf`, described in code as "left from the module days"; two files can hold a key | `ForgeMain.cpp:354-360`; `cluster.toml:17`; `ForgeCommands.cpp:119-127` | Med | M | Med |
| R-21 | Core | `MapUpdater::ParallelFor` has no caller; `SetMapUpdateInterval` is a no-op still called | `MapUpdater.h:85`; `MapMgr.h:101`; `World.cpp:205` | Low | S | Low |
| R-22 | Docs | `03-animus-lib.md` and `05-animus-forge.md` (1,253 lines) describe `modules/mod-animus-forge/src`; deploy-gate names a worktree branch as "until merged" | `03-animus-lib.md:3-5`; `05-animus-forge.md`; `deploy-gate.md:31` | Med | M | Low |
| R-23 | Docs | `AGENTS.md` names no forge path or doc; it references `.agents/docs/systems/`, which does not exist; no root README | `AGENTS.md`; `ls .agents/docs` | Med | S | Low |
| R-24 | Leftovers | Usage docstrings and help point at deleted `stage4_duel` configs and old stage names | `train.py:3`; `export.py:3,101`; `bench_learner.py:3`; `ForgeConfig.h:60`; `ForgeCommands.cpp:506` | Low | S | Low |
| R-25 | Leftovers | Comments cite gitignored plan files and `var/` scripts that a clone does not have | see section 5 | Low | S | Low |
| R-26 | Repo | `mod-animus-movement.patch` (88 KB) says OBSOLETE on line 1; duplicate 148 KB fixture; the amdl8 patch targets `animus-lib/` paths | `patches/mod-animus-movement.patch:1`; `tests/fixtures/` | Low | S | Low |
| R-27 | Repo | `.gitignore` un-ignores five plan folders, four of which do not exist; a 107 KB plan is tracked although `AGENTS.md` says plans are gitignored | `.gitignore` (Agents block); `.agents/plans/`; `AGENTS.md` | Low | S | Low |
| R-28 | Tests | `test_forgectl.py` and four more test files hard-code the real cluster's addresses and logins, so `cluster.toml` and the tests must change together | `test_forgectl.py:85-87,156,216`; `test_dungeon_stages.py:228` | Low | S | Low |
| R-29 | Safety | Private addresses, logins, the owner's full name and the `lan` remote path are tracked | `cluster.toml:31-72`; `cluster.md:8-17,21,36`; `deploy-gate.md:17,23` | Low | S | Low |
| R-30 | Core | `Forge.Playtest` (48 lines in 9 files) is a live gate; the owner's notes say playtest was abandoned on 2026-10-05 | `Forge.cpp:36`; `ForgeMain.cpp:185-204,244,432,461,497`; `World.cpp:1157,1225` | Low | M | Med |
| R-31 | Headers | 198 forge files carry "Animus Forge project, based on AzerothCore"; upstream tooling and `AUTHORS` expect the AzerothCore line | `BotSlot.cpp:2`; `AUTHORS` | Low | S | Low |
| R-32 | Python | `evaluate.py` offers `--baseline random`, against principle 14; `bench_learner.py`, `evaluate.py`, `blas.py` have no importer | `evaluate.py:3-4`; `principles.md` 14 | Low | S | Low |
| R-33 | Upstream | Many forge edits are upstreamable fixes (scheduler `Update(diff)`, 64-bit timers, BIH hardening, LFG and guid thread safety) | section 1.4 | Low | M | Low |
| R-34 | Build | Worker containers compile inside the run container, each from source, with no artefact promotion; "up to an hour" on the slowest | `forge-worldserver.sh:30-56`; `deploy-gate.md:714` | Med | L | Med |
| R-35 | Repo | Upstream cruft that the fork never uses: `.coderabbit.yml`, `FUNDING.yml` (upstream donation link), `flake.nix`, `doc/changelog`, `apps/{installer,startup-scripts,DatabaseSquash}` | `ls`; `.github/FUNDING.yml` | Low | S | Low |

## 1. The upstream relationship

### 1.1 How far behind, measured

`git fetch upstream` (read-only) then `git rev-list --count forge..upstream/master` gives **494** commits;
`upstream/master` is `bce7ed5a6`, dated 2026-10-07. The delta document counts 206 against `386f3a13f` of 2026-09-15
(`01-forge-core-delta.md:11-13`), so the number has more than doubled in three weeks. Since the merge-base `37de65eb0`
upstream changed 519 files (95,149 lines added, 5,163 removed); 75,089 of those changed lines are `data/sql`
(`git diff --numstat`), and the forge touches no SQL, so a merge also brings a world-database schema and data update
that the sim picks up through `Updates.AutoSetup` (`docker-compose.yml:75-78`). The source part is about 17,000 lines
under `src/server`, 632 under `src/test`.

Of the forge's 119 edited upstream files, **41 were also edited upstream** and **78 were not**. The 78 include the
files the delta document ranks as the highest risk: `MapMgr.cpp/.h`, `MapUpdater.cpp/.h`, `MapInstanced.cpp`,
`MovementHandler.cpp`, `LFGMgr.cpp`, `GridTerrainData.cpp`, `DatabaseWorkerPool.h`, `ObjectMgr.cpp` and
`ObjectAccessor.cpp`. So today's merge is easy where the delta document expects it to be hard, and hard where it
expects it to be easy.

### 1.2 Dry-run result, and the ranking of the 119 files

`git merge-tree --write-tree --name-only --messages forge upstream/master` (no working tree touched; one tree object
`54dd8dc6ad4d949d069e250fda55bb92c936361c` written). Result: not clean. In order of cost:

**Tier 1: conflicts a person must resolve (6 content files plus `AGENTS.md`, 1 modify/delete that matters).**

| File | Hunks | Why it conflicts | Resolution |
|---|---|---|---|
| `src/server/apps/worldserver/Main.cpp` | modify/delete | forge deleted it; upstream added `SaveSessionEnd`, `OnModuleDatabasesLoading`, `OnModuleDatabasesClosing`, `OnDatabaseWarnAboutSyncQueries` (16 lines) | keep the deletion; port the four calls into `ForgeMain.cpp` by hand or consciously drop them (R-04) |
| `src/server/game/Spells/Spell.cpp` | 4 (merge result `:3892,4216,4882,4969`) | upstream `fe6913668` replaced `m_caster` with `unitCaster` in `OnSpellCast/OnSpellCastCancel` and moved `Unit* unitCaster` into `SendSpellStart/Go`; forge adds `Animus::Hooks::CastCancelled/CastCompleted` and `HasClients` early returns on the same lines | take upstream's names, re-add the two hooks (their first parameter must become `unitCaster`, see R-02) and the two `HasClients` returns before `unitCaster` is read |
| `src/server/game/Entities/Unit/Unit.cpp` | 1 (`:6656-6675`) | forge's `HasClients`-gated `Unit::SendSpellMiss` against upstream's deletion of it | delete the forge copy; gate the new `WorldObject::SendSpellMiss` instead (R-02) |
| `src/server/game/Server/WorldSession.cpp` | 2 (`:174-178`, `:885-891`) | `if (!m_simSession)` against upstream `if (!_headless)`, and `!m_simSession` against `OnPlayerCanMarkAccountOffline` | take upstream's forms and delete `m_simSession` where `_headless` suffices (R-05) |
| `src/server/game/Entities/Player/Player.cpp` | 1 (`:12068-12077`) | upstream folded the equip proc-cooldown into a lambda `applyProcCooldown` that reads `std::chrono::steady_clock::now()`; the forge line is `GameTime::Now()` | take the lambda, change its clock to `GameTime::Now()`. The conflict is easy; losing the clock change is the bug (section B of the delta document) |
| `src/server/game/Entities/Player/PlayerUpdates.cpp` | 1 (`:2473-2482`) | forge's early return in `UpdateAdditionalSaves` against upstream's rename `isLogingOut` to `IsLoggingOut` | keep the forge return, upstream's name below it. The merged `Player.cpp:10444` and `PlayerStorage.cpp:7315` already use the new name; no stale caller was found in `Animus/` |
| `AGENTS.md` | 3 (`:10,29,44`) | forge removed four e2e lines; upstream edited them | keep the forge side |

Noise that resolves with `git rm`: 27 `e2e/**` files, `.github/workflows/e2e-live.yml` and
`.agents/docs/e2e-policy.md` (modify/delete: the forge removed the Go e2e suite, upstream kept editing it). Expect the
same noise at every merge until the owner decides whether upstream's e2e suite is wanted again (Q-6).

**Tier 2: merges cleanly, but does not compile or silently loses a forge guarantee.** This is the dangerous class
because git reports success.

| File | What happens | Evidence |
|---|---|---|
| `src/common/Threading/PCQueue.h` | forge added `Reset()` (`forge:PCQueue.h:100`); upstream added an identical `Reset()` (`c1893c0e5`, used by `DatabaseWorkerPool::Open`). The merge keeps both: **redefinition, compile error**. Drop the forge copy; this also retires F-9 of the delta document | merge result `PCQueue.h:100` and `:116` |
| `src/server/game/Entities/Object/Object.cpp` | upstream added 793 lines, including `WorldObject::SendSpellMiss` and `SendSpellNonMeleeDamageLog` (`fe6913668`). Forge's gate is on the `Unit` versions; the new ones build and send a packet with no `HasClients` test | merge result `Object.cpp:3855-3859` |
| `Animus/Env/AnimusHooks.h` callers in `Spell.cpp` | `CastCompleted(Unit*, Spell*)`, `CastCancelled(Unit*, Spell*, bool)`, `HealCast`, `Damage`, `Heal` take `Unit*`; upstream's `Spell::m_caster` is a `WorldObject*` after `fe6913668`, so the call needs `ToUnit()` and a null check | `AnimusHooks.h:45-49`; forge `Spell.cpp:3775,4091` |
| `src/server/game/World/World.cpp` | upstream added a `LoginDatabase` write at start-up and `SaveSessionEnd`; `OnModuleDatabasesKeepAlive` landed inside the ping block the forge kept. Harmless today; `SaveSessionEnd` is called only from the deleted `Main.cpp`, so the session outcome is never recorded for the forge | merge result `World.cpp:909-916,1278-1285` |
| `src/server/game/Maps/Map.cpp` | `CanSendObjectUpdatesToPlayer` landed correctly after the forge's `HasClients` drain. New `ForceCreatureRespawn` calls `SaveCreatureRespawnTime`, a character-database write that a sealed pool drops with a log line | merge result `Map.cpp:1875` |
| `src/server/game/Time/GameTime.cpp` | upstream added `StartSteadyPoint = steady_clock::now()`; the forge's `AdvanceGameTimers` seeds its own point. Harmless unless something reads the new one in gameplay | upstream diff of `GameTime.cpp` |
| `Creature.cpp`, `Pet.cpp`, `SpellAuras.cpp`, `Group.cpp`, `InstanceSaveMgr.cpp` | auto-merged; upstream changed behaviour in the same files (lazy creature terrain status `b573d7e61`; a pet-save refactor of 145/122 lines; group invite and loot-roll fixes). `Pet.cpp` keeps the forge's early `return` above upstream's new body, so upstream's pet-save changes are dead code in the fork | `Pet.cpp:547-560` |

**Tier 3: both sides edited, auto-merged, low risk (about 25 files):** `Battleground.h`, `BattlegroundSA.h`,
`LFGMgr.h`, `Creature.h`, `DynamicObject.h`, `GameObject.cpp/.h`, `Object.h`, `Player.h`, `PlayerStorage.cpp`,
`Unit.h`, `Group.h`, `InstanceScript.cpp`, `Map.h`, `WorldSession.h`, `SpellAuraEffects.cpp`, `GameTime.h`,
`World.h`, `boss_xt002.cpp`, `spell_druid.cpp`, `spell_paladin.cpp`, `DatabaseWorkerPool.cpp`,
`worldserver.conf.dist` (upstream +20 lines in a file the forge extended by 1,823).

**Tier 4: forge-only edits upstream has not touched (78 files)**, including every file in the delta document's
sections F (except `Map`, `LFG` and `Battleground` headers) and H and all of the camera accessors (G). Zero risk this
round, and the likely source of surprises later.

Two points of method. First, git cannot see semantic conflicts, so section 1.6 gives a checklist. Second, upstream's
`data/sql` volume means the useful first step of a merge is to read the commit subjects: the list for `src/server`,
`src/common`, `Maps`, `Movement`, `DungeonFinding` and `database` is 21 commits, readable in ten minutes.

### 1.3 What happens to the build if upstream changes a file the forge edited in place

Three things, in order of how loud they are.

1. **A textual conflict** (Tier 1): the merge stops; loud.
2. **A semantic break**: a compile error (R-01) is loud; a lost guard (R-02) is silent and shows up as wasted CPU,
   not wrong results (the delta document's section D says the same). A new post-startup `AsyncQuery` or holder would
   `ABORT` the process on the sealed pool; the scan of the upstream diff found none, only synchronous writes the seal
   drops (`World.cpp`, `Guild.cpp`, `ArenaTeam.cpp`, all `Execute`).
3. **The cluster does not notice.** `FORGE_SOURCE_HASH` hashes only `Animus/` (`game/CMakeLists.txt:61-72`), so a
   worker that missed a core change, or built it differently, passes the fingerprint (R-06). The build also reads
   sources through `file(GLOB)` at configure time (`forge-worldserver.sh:28-29` documents it): an upstream file added
   by a merge is not compiled until the next configure, which `./forge.sh --build` does and a plain restart does not.

### 1.4 What could be upstreamed or replaced by an upstream hook

Upstream has moved toward the forge's needs since the fork point. Mapping each to a forge edit:

| Upstream (commit) | What it gives | Forge edit it can replace | Verdict |
|---|---|---|---|
| headless sessions (`92fed92ea`): `_headless(!sock)`, `IsHeadless()`, `m_Address = "headless"`, no `account` writes in the destructor | a session built with a null socket is headless automatically | `m_simSession`, `SetSimSession`, `IsSimSession` (`WorldSession.h:501-502,1314`), the guards at `WorldSession.cpp:171` and `InstanceSaveMgr.cpp:793,809`. `BotFactory.cpp:76` already builds the session with `nullptr` | replace; shrinks the delta, removes two conflict hunks |
| `OnPlayerCanMarkAccountOffline` (`2d6742b26`) | veto the account-offline write on logout | `!redirecting && !m_simSession` at `WorldSession.cpp:872` | replace with a `PlayerScript` in `Animus/` that returns false for a headless session |
| `CanSendObjectUpdatesToPlayer` (`6792c7de3`) | per-player veto of an update packet | partly `Map::SendObjectUpdates` | cannot replace it: the forge's early drain (`Map.cpp:1824-1838`) skips `BuildUpdate` for every object, which the hook cannot do |
| `SessionScript::OnSessionUpdate`, `OnPacketSent` (`6792c7de3`, `92fed92ea`) | observe a session tick and every packet sent | possibly the movement-order inbox (`WorldSession::EnableMovementOrders`, `WorldSession.cpp:306`) | UNVERIFIED: depends on whether `Animus::Client::Inbox` needs the packet before or after the socket test |
| `NextQueuedPacket` (`92fed92ea`) | drain a headless session's receive queue | the same inbox | UNVERIFIED |
| `OnModuleDatabasesLoading/Closing/KeepAlive` (`e1823bb2d`) | modules that own a database | nothing; the forge seals instead | not applicable, but `Main.cpp` calls them (R-04) |
| `129a2d0e2` read-only module accessors | accessors for modules | possibly part of section G of the delta document (collision accessors) | UNVERIFIED; read the commit before the next merge |

Forge changes that **cannot** be hooks, because they change a hot path rather than react to an event: `HasClients`
packet gating (a global predicate used inside `Object.cpp`, `Unit.cpp`, `Spell.cpp`), the sealed database pool, the
`MapUpdater`/`MapMgr` rewrite, `GameTime::AdvanceGameTimers`, the `UpdateMask` bit packing and the world-loop rewrite.

Forge changes that are **genuine bug fixes upstream would take** and could be sent as small pull requests (the owner
said no pull requests unless asked; this is a list for a decision, Q-7):

- `_scheduler.Update(diff)` where upstream passed nothing, so the scheduler ran on the wall clock: `boss_jeklik.cpp`,
  `zone_howling_fjord.cpp` (delta table); `TaskScheduler::GetNextGroupOccurrence` measuring against `_now`.
- 64-bit cooldown and respawn timestamps (`SpellCooldown::end`, `m_ProhibitSchoolTime`, `m_cooldownTime`,
  `m_lastSanctuaryTime`, SotA demolisher respawn): an overflow after 49.7 days of uptime that also affects a real
  realm.
- `BoundingIntervalHierarchy` leaving out a primitive with non-finite or inside-out bounds instead of
  `std::terminate` in `subdivide` (`BoundingIntervalHierarchy.h:89-92`).
- `LFGMgr::_storeLock`, `PlayerNameMapLock`, atomic `ObjectGuidGenerator::_nextGuid`: thread-safety for anything that
  places players from map threads.
- `RemoveFromMap` always calling `RemoveObjectFromMapUpdateList` (fixes an assert, `Map.cpp:855`).
- `UpdateMask` one bit per field (a pure speedup, applies with clients).
- `InstanceSaveMgr` leaking an unexecuted prepared statement (`InstanceSaveMgr.cpp:809-860`, noted in the delta
  document).

### 1.5 Dead core additions and gates in core code

Dead (no caller in `src`, re-verified with `grep -rw` over `src`):

| Symbol | Location | Note |
|---|---|---|
| `Battleground::SetSimOwned/IsSimOwned`, `_simOwned` | `Battleground.h:570-571` | F-10 |
| `Group::IsSimGroup` | `Group.h:230` | F-10 |
| `PathGenerator::SetIncludeFlags/GetIncludeFlags` | `PathGenerator.h:90-91` | F-10 |
| `GetBGObject(type, logMissing = false)` form | `Battleground.h:437` | F-10; no call passes `false` |
| `PCQueue::Reset` (forge copy) | `PCQueue.h:100` | F-9; now also a merge break (R-01) |
| **`MapUpdater::ParallelFor`** | `MapUpdater.h:85`, `MapUpdater.cpp:182` | **new**: only its declaration, definition and a comment exist; the delta document describes it as used "for a map task's own parallel pieces" |
| `MapMgr::SetMapUpdateInterval` | `MapMgr.h:101` (no-op), called at `World.cpp:205` | F-13; the caller is dead weight too |

Gates (forge settings read from upstream code): `OutdoorPvPMgr.cpp:49` reads `AnimusForge.Enable` straight from
`sConfigMgr` (F-6, already filed); `Forge.cpp:36` reads `Forge.Playtest`, which `World.cpp:1157,1225` and
`ForgeMain.cpp` branch on (17 lines in `ForgeMain.cpp`); `ForgeMain.cpp:229` reads `Forge.SealStrict`; `:276-281` read
`AnimusForge.DecisionMs`, `TicksPerDecision` and `HalfBatch` in the world loop; `:354-360` dual-reads the legacy
`modules/mod_animus_forge.conf`. Everything else reaches the forge through `ForgeCore::HasClients()` (a predicate on
session count, `Forge.cpp`), not through configuration. The one genuine behavioural gate is `Forge.Playtest`, which
principle 17 names and the owner's own notes mark as abandoned (R-30, Q-4).

### 1.6 Recommended upstream-merge routine

Cadence: monthly, or when an upstream commit touches a forge-edited file (`git log forge..upstream/master -- <the
119 files>` shows it). A merge is a review task, not a mechanical one; the owner decides when (principle 20: work and
merges go on `forge`).

1. `git fetch upstream`. Read the commit subjects that touch `src/server`, `src/common` and `src/test`.
2. Dry run: `git merge-tree --write-tree --name-only --messages forge upstream/master`. Compare the conflict list with
   section 1.2; anything new is a new Tier 1 file.
3. Merge on a throwaway branch. Resolve Tier 1 with the table above. `git rm` the e2e noise.
4. Run the **semantic checklist** on the merged tree, each a single grep over the added lines of the merge diff:
   `getMSTime\(|steady_clock::now|system_clock::now` (wall clock in gameplay code: convert to `GameTime::Now()`);
   `Send[A-Z][A-Za-z]*\(` builders on a hot path without `ForgeCore::HasClients()`; `AsyncQuery|DelayQueryHolder`
   after startup (aborts a sealed pool); `!isBGGroup\(\) && !isBFGroup\(\)` (should be `IsPersisted()`); anything that
   assumes `Spell::m_caster` is a `Unit`; two definitions of one member (`PCQueue::Reset`).
5. `./forgectl test` (GTests and CPU pytest in the dev container), then `./forgectl test --gpu` on a free card.
6. A short `forge run <stage>` on the sealed pool with the learner (the only test of the seal), then the deploy gate
   ([../deploy-gate.md](../deploy-gate.md)). The cluster needs one rebuild per merge (principle 18).
7. Record the merged upstream commit in `01-forge-core-delta.md` ("master today is ...") so the next dry run starts
   from a known base; today's figure is stale by a month.

Who: the owner or an agent the owner starts for it; nothing here is safe to automate without a person reading the
Tier 2 table, because git reports it as clean.

## 2. Repository contents

### 2.1 Size, history and what is tracked

- `git count-objects -vH`: one pack set of **14.24 GiB** (236,484 objects in 6 packs) plus 9.8 MiB loose. `HEAD`
  holds 20,122 commits, of which 751 are the forge's since the merge-base. The size is upstream history: its tracked
  tree is 10,842 files, 847 MB of them `data/` (7,606 files, nearly all SQL; the single largest tracked file is
  `data/sql/base/db_world/broadcast_text_locale.sql`, 76 MB) and 21 MB `deps/` (including the upstream binary
  `deps/acore/mysql-tools/bin/mysql.exe`, 4 MB). None of this is the forge's doing and none of it can be trimmed
  without rewriting upstream's history.
- **Forge-only history is small.** `git rev-list --objects 37de65eb0..HEAD` piped to `git cat-file --batch-check`
  gives a largest blob of 634,598 bytes (a revision of `Unit.cpp`); the next are `Player.cpp`, `ObjectMgr.cpp`,
  `Spell.cpp` and `worldserver.conf.dist` revisions of 330 to 620 KB, which are edits of upstream files, not additions.
  No model, probe, checkpoint or video was ever committed on the forge side. The forge added 549 files, deleted 34
  (31 under `e2e/`, `e2e-live.yml`, `e2e-policy.md`, `Main.cpp`) and modified 102 beyond the Animus tree.
- **Binary or large forge files that are tracked, all deliberate:** four `.amdl` goldens
  (`apps/forge/python/tests/golden/*.amdl`, 56 to 197 KB each, marked `binary` by `.gitattributes:33-36`), three stage
  JSON fixtures of 132 to 148 KB, `LiveLayoutPin.golden.inc`, and 128 KB of patches. `StageScenario.cpp` is 254 KB
  and 5,192 lines (the brief's "about 6,000" is the pre-trim figure; `known-issues.md` has the measured number).
- **Untracked bulk, correctly ignored** (sizes of the owner's checkout, `du`): `apps/forge/python/.venv` 15 GB (torch
  alone 14 GB), `apps/forge/probes` 3.1 GB, `apps/forge/models` 580 MB, `var/` 38 GB, `env/` 1.2 GB. The ignore rules
  are at `.gitignore` ("Forge" block), `/var/*`, `/env/dist/*`. Two things follow: the probes live in a second
  repository (`animus-probes.git` on the dev machine, `cluster-pull.sh:35-46`) whose creation is not documented in
  this tree (UNVERIFIED where), and the cluster fingerprint checks them only by count and total bytes of `*.field`
  files (`AnimusForge.cpp:79-87`), so a copy with the same sizes and different content would pass.
- **Not a venv, but close:** `apps/forge/python/animus_forge.egg-info` and `.pytest_cache` exist in the owner's tree
  and are ignored; they are not tracked.
- **Branch and worktree litter** (shared repository): 30 branches in the owner's repository including a run of
  `worktree-agent-*` branches, 7 registered worktrees, no stash. Housekeeping, not a defect; the agent worktrees are
  disposable once merged.

### 2.2 Top level, file by file

| Path | What it is | Forge-specific or upstream | Comment |
|---|---|---|---|
| `AGENTS.md`, `CLAUDE.md` (`@AGENTS.md`), `.agents/` | agent instructions and docs | upstream, minus four e2e lines | no forge guidance (R-23) |
| `CMakeLists.txt`, `PreLoad.cmake`, `conf/`, `src/cmake/` | build | upstream, forge edits (section 3) | |
| `acore.sh`, `acore.json`, `bin/`, `install.sh`, `apps/{compiler,installer,startup-scripts,...}` | upstream's dashboard and tooling | upstream | `acore.json` still says `azerothcore-wotlk` 17.0.0-dev; the forge uses `acore.sh compiler` inside its containers (`forge-worldserver.sh:33-35`) |
| `docker-compose.yml`, `docker-compose.cluster.yml`, `forge.sh`, `forgectl`, `apps/docker/{forge-worldserver,animus-venv}.sh` | forge operations | forge | |
| `apps/forge/` | forge tooling: `forgectl/` (14 modules), `python/` (learner, 13 stage configs, 95 test files), `tools/` (12 scripts), `patches/`, `cluster.toml` | forge | |
| `src/server/game/Animus/`, `src/server/game/Forge/`, `src/test/server/game/Animus/` | the sim, `ForgeCore`, its GTests | forge | |
| `docs/forge/` | the forge manual (this audit lives in `audit/`) | forge | |
| `data/` | SQL base, archive, updates | upstream, untouched (`git diff --stat 37de65eb0 forge -- data` is empty) | |
| `deps/`, `doc/`, `tools/socket_stress_heavy.py` | vendored libraries, upstream docs, a stress script | upstream | |
| `modules/` | module loader plumbing; `.gitignore` ignores every module checkout | upstream, +40 lines of CMake | the owner's tree holds `mod-animus` and a stale `mod-animus-forge` (names from `ls`; contents not read) |
| `env/`, `var/` | install prefix and scratch | upstream layout; only `.gitkeep` files are tracked | |
| `.coderabbit.yml`, `.git_commit_template.txt`, `.suppress.cppcheck`, `flake.nix`, `flake.lock`, `pull_request_template.md`, `.github/FUNDING.yml`, `.github/CODEOWNERS`, `.github/agents/` | upstream project machinery | upstream | unused by the fork; `FUNDING.yml` shows upstream's donation link on the fork's page (R-35) |
| `.devcontainer/`, `.vscode/` | editor containers | upstream, `devcontainer.json` renamed and `shutdownAction: none` | works against the compose file |
| `AUTHORS`, `LICENSE` | credits, GPL v2 | upstream, unchanged | the fork's files cite "See AUTHORS" but `AUTHORS` has no forge entry (R-31) |

There is **no root `README`** in either upstream or the fork; the manual's entry point is `docs/forge/README.md` and
nothing in the root points to it.

### 2.3 `e2e/`, `data/`, `conf/dist`, `env/dist`

- `e2e/` is gone (31 Go files, 4,295 lines) along with its workflow; the delta document records it, and upstream keeps
  editing it, so it returns as 27 modify/delete conflicts at every merge (section 1.2). `AGENTS.md` still carries the
  other half of the removal.
- `data/`: no change by the forge. `data/sql/custom/` is gitignored (`.gitignore`), so nothing forge-specific enters
  the schema.
- `conf/dist`: `config.cmake` +6 lines (`WITH_LTO`, `FORGE_PGO`, `FORGE_PGO_DIR`) and `env.ac` (`CTYPE=Release` and a
  comment). The forge's settings are not in `conf/dist` at all; they are in `worldserver.conf.dist` (+1,823 lines, 414
  key lines) and in a per-machine `modules/mod_animus_forge.conf` that is untracked (R-20).
- `env/dist`: only `.gitkeep` files are tracked; the installed binaries, configs and logs are untracked.

### 2.4 `apps/forge`: patches, models, probes, tools

- `apps/forge/patches/mod-animus-movement.patch` (88,804 bytes): line 1 reads "OBSOLETE (player-controller,
  2026-10-05) ... do not apply". Principle 17 (dead code is deleted, git history keeps it) says remove it.
- `apps/forge/patches/mod-animus-amdl8.patch` (39,779 bytes) and `amdl8-check/` (5 files): a reader for `.amdl` 8 and 9
  for the realm module `mod-animus`, targeting `animus-lib/src/runtime/...`. `06-animus.md:30-36` already says it
  applies only to an older module shape. The owner's notes say the realm is parked. Whether the realm module reads
  today's `.amdl` version 9 is UNVERIFIED (`modules/` was not read). Candidates: move to the module's repository.
- `apps/forge/models/` (580 MB, ignored): exported `.amdl` plus `.json` pairs, e.g. `deathknight_duel`,
  `deathknight_gauntlet`, `deathknight_companion`, `deathknight_life`: these are model names of the archived first
  curriculum (`ls` of the owner's directory); the current stages are `move*`, `combat*`, `group*`, `dungeon*`. Stale
  exports are not tracked but are 580 MB of the first curriculum.
- `apps/forge/probes/` (3.1 GB, ignored): baked ground-probe `*.field` tables, in their own repository.
- `apps/forge/tools/`: every script is referenced by `forgectl`, a doc or a test:
  `cluster-pull.sh` (`deploy.py:50`), `collect-videos.sh` (`videos.py:12`), `forgectl-test.sh` (`testcmd.py:17`),
  `conf_prune.py`, `resume_check.py`, `run_snapshot.py`, `stage_json_diff.py`, `spec_builds/` (tests exist for each),
  `rename_runs.py`, `forge_classes.py`, `sim_metrics.py` (referenced in `08-reference.md`, `deploy-gate.md` and
  `test_metric_names.py`). `forgectl-test.sh:3` and `testcmd.py:3` say they were "ported from the gitignored
  `var/staging_test.sh`", and `console.py:3` cites `var/forge_console.py`: the originals are in the ignored scratch
  directory and the ports may have drifted (not compared; `var/` was not read).
- Duplicates: `tests/fixtures/seek_stage.json` and `tests/fixtures/stage_move2_seek.json` are byte-identical
  (blob `4595e6cef`, 148,174 bytes each), used by `test_golden_update.py:5,42` and `test_resume_check.py:219`.
  `bench.py`/`prep.py` in `patches/amdl8-check` are used only by that README.

### 2.5 License headers

`apps/codestyle/codestyle-cpp.py` was run read-only from the root and prints "Everything looks good": multiple blank
lines, trailing whitespace, `GetCounter()`, misc, `GetTypeId()`, the three flag-helper checks and qualifier alignment
all pass over the whole of `src`. The header check the brief asks for:

- Upstream's header line is "This file is part of the AzerothCore Project. See AUTHORS file for Copyright
  information" with the GPL v2-or-later text (`MapMgr.cpp:2-15`; `LICENSE` is GPL v2).
- **198 forge files** carry "This file is part of the Animus Forge project, based on AzerothCore. See AUTHORS file for
  Copyright information" with the identical GPL text (`BotSlot.cpp:2-15`). The licence is the same; the project line
  differs, and `AUTHORS` (unchanged, line 18 lists mangos) has no entry for the forge. A few `Animus/Movement/` files
  carry the AzerothCore line instead (`CastWatch.cpp`, `Client.cpp`, `ControllerCost.h`, `Capture.cpp`,
  `MapWorldQuery.*`, `Replay.cpp`, `Seek.h`, `FlagRules.h`, `ReportCadence.h`).
- **One file has no header:** `src/test/server/game/Animus/LiveLayoutPin.golden.inc` (a generated golden; harmless).
  Python, shell, CMake and YAML files carry none, as upstream's helpers do not either.
- Not found: any forge C++ file with a foreign licence or no licence line.

The wording is a decision, not a defect (Q-8): either add the forge as a copyright holder in `AUTHORS` and keep the
project line, or switch the 198 files to upstream's line so the codestyle tooling and future upstream merges treat them
alike.

### 2.6 `.agents/`, `AGENTS.md`, `CLAUDE.md`

- `CLAUDE.md` is `@AGENTS.md`; `AGENTS.md` is upstream's with four e2e lines removed. It does not mention `docs/forge`,
  `forgectl`, `src/server/game/Animus` or `apps/forge`, so an agent started in this repository learns the forge only
  from the owner's private memory notes. The "Repository layout" list is upstream's.
- `AGENTS.md` sends the reader to `.agents/docs/systems/`; the directory does not exist in the fork or upstream
  (`git ls-tree upstream/master .agents` lists seven docs and no `systems`). Upstream defect, harmless.
- `.agents/docs/*.md` (7 files) are upstream's, accurate for stock AzerothCore, silent on the forge. The one stale
  reference removed was `e2e-policy.md`.
- `.agents/plans/forge-parallel-core/forge-parallel-core.PLAN.md` (107 KB, 1,359 lines) is tracked, against `AGENTS.md`
  ("Planning docs go in `.agents/plans/<task-slug>/` (gitignored)"). `.gitignore` carries explicit un-ignores for it and
  for four more folders (`animus-long-build-fixes`, `animus-team-ctf`, `animus-director`, `animus-curriculum`,
  `animus-retrain`) that do not exist in this tree. Its first section opens "Until now it kept every change rebasable
  ... Main.cpp is still in the tree ... That constraint is being dropped": a record of a decision, now history.
  Candidate: move to `docs/forge/decisions/` or delete (R-27).
- `docs/forge/decisions/0002` to `0017` are 11-line records; fine, but `0014` carries "UNVERIFIED: exact date".

## 3. Build system, containers and CI

### 3.1 What the forge changes in CMake

| Change | Where | Observation |
|---|---|---|
| Default build type Release (stock: RelWithDebInfo) | `CMakeLists.txt:92` | contradicted by the container, see R-08 below |
| `include(ConfigureLTO)`; `WITH_LTO` default 1 | `CMakeLists.txt:103`; `conf/dist/config.cmake:113`; `ConfigureLTO.cmake:18-77` | applies to Release, RelWithDebInfo and MinSizeRel, so the unit-test tree gets LTO too (`forgectl-test.sh:34` builds RelWithDebInfo) |
| `-O3`, `-march=native`, optional `-mtune=znver5/4`, `-fno-semantic-interposition` for x86-64 | `clang/settings.cmake:50-70`, `gcc/settings.cmake:49-68` | `-O3` is appended after the config's own `-O2` so it wins; the Zen test reads `/proc/cpuinfo` at configure time, so a tree configured on one machine and built on another gets the first one's tuning |
| `FORGE_PGO=off|generate|use` | `clang/settings.cmake:77-88`, `gcc/settings.cmake:75-84` | no caller anywhere in the tracked tree sets it (`grep -rn FORGE_PGO` finds only the two compiler files and `conf/dist/config.cmake`); PGO is unused build surface |
| Source hash into `ForgeSourceHash.h` | `game/CMakeLists.txt:61-72` | see 3.2 |
| zstd, `REQUIRED` | `game/CMakeLists.txt:77-78` | no tracked CI or installer installs `libzstd-dev` (`grep zstd .github apps/ci apps/installer` is empty); only `Dockerfile.dev-server:45` and `Dockerfile` do. A fresh machine configures to a hard failure |
| `libforge-gpu.so` with hipcc | `game/CMakeLists.txt:112-147` | see 3.3 |
| `MODULES_FOLDED_INTO_CORE` | `modules/CMakeLists.txt:20-29,374-393` | see 3.4 |
| `FORGE_PYTHON_DIR` compile definition | `game/CMakeLists.txt:55-56` | bakes the build machine's source path into the binary; the worldserver in a container finds the learner at `/azerothcore/apps/forge/python` only because the bind mount keeps that path |

### 3.2 The source-hash step

`file(GLOB_RECURSE ... Animus/*.cpp *.h *.hip)`, SHA-256 of each, then a 16-character digest written with
`file(CONFIGURE)` to `ForgeSourceHash.h` in the build tree (`game/CMakeLists.txt:61-72`), consumed at
`AnimusForge.cpp:64-68,99`. Weaknesses (the delta document notes the first, not the rest):

- It covers `Animus/` only. `Forge/`, `ForgeMain.cpp`, `ClientMovement.cpp`, `CpuPlacement.*`, `cs_forge.cpp` and every
  in-place edit of an upstream file (the whole of the delta document's sections B to F) are outside it, as are the
  compiler, flags, `libforge-gpu.so`, and the Python learner. A worker with an older `MapUpdater.cpp` or a different
  build type is accepted (R-06).
- The header is optional: `#if __has_include("ForgeSourceHash.h")` falls back to the string `"unhashed"`
  (`AnimusForge.cpp:64-68`), so two builds that both lost the generated file agree with each other. Nothing logs it.
- It is computed at **configure** time. A source edited after configure and built without a configure carries the old
  hash; `forge-worldserver.sh:30-36` always configures first, a hand-run `make` does not.
- It hashes contents in sorted path order and not the paths themselves, so a rename that keeps the sort position does
  not change it (the delta document notes the same).

### 3.3 The HIP device-library step

`find_program(FORGE_HIPCC hipcc)`; if present, every `Animus/Gpu/Device/*.hip` is compiled with
`--offload-arch=${FORGE_GPU_ARCHS}` (default `gfx1100`, `game/CMakeLists.txt:113`) into `libforge-gpu.so`, installed
beside the worldserver, which loads it at run time; with no `hipcc`, the library is skipped with one status line
(`:147`). Problems:

- **Architecture contradiction (R-09).** `Dockerfile.dev-server:62-64` says "Compile with --offload-arch=native ... an
  explicit --offload-arch=gfx1100 produces a binary whose kernels fail to load ('invalid device function')". The CMake
  step passes exactly `--offload-arch=gfx1100`. Either the comment is outdated or the build relies on a default that
  works on the dev card only. Nothing in the tracked tree sets `FORGE_GPU_ARCHS` for another card, while
  `animus/blas.py:3` says the cluster has RDNA4 cards (`gfx12`) and `blas.py:5` mentions an RTX 3060 Ti (NVIDIA, which
  `hipcc` does not target). Whether those machines build a usable device library is UNVERIFIED (per-machine
  `docker-compose.override.yml` and build arguments are untracked).
- **The fingerprint does not record whether a worker has the device library**, so a mixed cluster (some with
  `libforge-gpu.so`, some without) is accepted, and `game/CMakeLists.txt:108-110` says such a machine has "no device
  observations and no GPU camera".
- **HIP 5.7 headers against a 6.4 runtime** (`DeviceApi.h:28-35`): the device library is built with Ubuntu's `hipcc` and
  works only because it resolves its symbols from the learner's torch runtime, "measured: 6.4 to 6.4 works". Torch
  versions seen in the code base are 2.9.1+rocm6.4 (the dev venv), 2.10+rocm7.0 and 2.13+rocm7.1 (`blas.py:6-8`).
  A torch upgrade on any machine is a possible silent break of device sharing; the API version check
  (`FORGE_GPU_API_VERSION = 5`) does not cover the runtime (R-15).
- It adds `-ffp-contract=off` to match the CPU caster; the CPU caster is built with `-march=native` and clang's default
  contraction, so the two differ by design and `forge camera diff` measures it (`VisionDevice.h:45-46`).
- `DeviceRuntime.h:23` says "CUDA under the same names, so the kernels and Runtime.hip are one source", but the CMake
  has no CUDA path.

### 3.4 `modules/CMakeLists.txt` and `MODULES_FOLDED_INTO_CORE`

`MODULES_FOLDED_INTO_CORE` is `mod-animus-forge` (`:20`), the checkout the owner's notes call dead. The build still
walks it: it drops the module from the list, but then **installs its `.conf.dist` from the stale checkout**
(`:374-393`, "a folded module keeps its config file"). So (a) the live template `mod_animus_forge.conf.dist` comes from
a directory nobody maintains, (b) deleting that checkout changes what is installed (the message becomes "no config
directory left"), and (c) the per-machine `mod_animus_forge.conf` the whole cluster tool-chain reads is a compatibility
artefact of this block (R-19, R-20). The same duplicate-symbol hazard applies to `mod-animus`, which the owner's
checkout also holds and which carries its own `animus-lib` in namespace `Animus`; `03-animus-lib.md:46-50` says its
`mod-animus.cmake` stops the configure with a message. That guard lives in the module (not read) and the forge
list does not mention it: UNVERIFIED that a forge build with `MODULES=static` and `mod-animus` present stops cleanly.

### 3.5 Compiler flags, `-march=native`, reproducibility

- Each worker builds for itself (`forge-worldserver.sh:38-57` rebuilds when the CPU signature differs). That fixes the
  illegal-instruction problem of the old `-march=znver5` setting (comment at `clang/settings.cmake:44-49`) and creates
  a different one: the fleet is a Ryzen 9 9950X3D (dev), 7900X, 3800X, an i7-6700K and a Xeon E5-2640
  (`cluster.md:10-17`). They differ in FMA and AVX-512 availability, so clang's default floating-point contraction
  gives different rounding on different machines. For a simulator whose physics, ray casts and pathing feed an RL
  loop, runs are not bit-reproducible across machines, an evaluation seed ("seeded evaluation episodes",
  `Random.cpp`) gives the same episode only on the same machine, and a host cannot tell (R-07). The code base knows
  the effect for the camera (`VisionDevice.h:45-46`) and measures it with `forge camera diff`; nothing measures it for
  the world.
- `-fno-semantic-interposition` and LTO make the worldserver a single optimised unit; they make a crash backtrace
  harder to read, which is why `forge-worldserver.sh:21-24` builds RelWithDebInfo while `env.ac:13-19` says Release
  because of "a third of a gigabyte of debug info" and "a much slower link once LTO is on". Both are written down by
  the same author and they disagree: **three defaults** are in force, `CMakeLists.txt:92` Release, `env.ac:19` Release,
  `forge-worldserver.sh:24` and `forgectl-test.sh:34` RelWithDebInfo. What the cluster actually builds is the last
  (the `export CTYPE` overrides `env.ac`), so the `env.ac` comment describes a build nobody makes (R-08).
- `-Wno-profile-instr-unprofiled` and other PGO options are dead (section 3.1).

### 3.6 Build times and what drives them

No build was run. The only recorded numbers: "a rebuild is slow (20 to 60 minutes a machine)" (`decisions/0014`) and
"the slowest build takes up to an hour" (`deploy-gate.md:714`); the first build "takes a long time" (`forgectl.md:238`).
Drivers visible in the tree (all UNVERIFIED as to magnitude):

- **No unity build** (`grep UNITY` over the CMake files is empty), PCH on by default (`conf/dist/config.cmake:98-99`,
  `CMakeLists.txt:97-98` turns it off only for a hidden dev option), `forgectl-test.sh:35` turns it off for tests.
- **LTO plus `-O3` on a 254 KB translation unit.** `StageScenario.cpp` is 5,192 lines; `AnimusForge.cpp` 3,087;
  `InstanceEncounter.cpp` 2,752; `cs_forge.cpp` 1,256 (compiled into the scripts, static). Under LTO the link, not the
  compile, is the serial tail; on the 8-thread i7-6700K it is the whole critical path.
- **Header fan-out**, counted as direct `#include "..."` sites in `src/server` and `src/test`: `SeatView.h` 30,
  `Layout.h` 29, `CurriculumTuning.h` 18, `StageScenario.h` 17, `StageState.h` 15, `AnimusForge.h` 7. And
  `AnimusForge.h:24-26` includes `Map.h`, `MapMgr.h` and `MapUpdater.h`, which `World.cpp`, `MapMgr.cpp` and
  `MapUpdater.cpp` include for that reason (`AnimusForge.h` has 7 direct includers). A change to a core map header
  rebuilds both trees. `CurriculumTuning.h` (1,185 lines, one struct of about 725 keys, F2) is the widest.
- **`file(GLOB)`** means every added file needs a configure (`forge-worldserver.sh:28-29`), which re-hashes all
  `Animus/` sources (`game/CMakeLists.txt:61-72`; fast, a few hundred files).
- **The authserver is built too** (R-10): `conf/dist/config.sh:88` sets `CAPPS_BUILD=${CAPPS_BUILD:-all}`; `env.ac`
  does not override it; the sim never starts `authserver` (compose profile `stock`). `APPS_BUILD=world-only` is an
  existing option (`conf/dist/config.cmake:16`). `forgectl-test.sh:34` also builds `all`.
- **Workers compile inside the run container**, from source, one by one (`forge-worldserver.sh:30-56`), and the dev
  machine does not ship a binary even though the cluster fingerprint is designed to prove builds are interchangeable
  (R-34). Shipping would defeat `-march=native`, which is a reason, not a defence.

### 3.7 Containers, compose and `forge.sh`

- **Images.** `Dockerfile.dev-server` is the only forge-specific image: `FROM ubuntu:24.04` with no digest (`:10`), apt
  packages unpinned (`:31-73`), so a rebuild a month later is a different toolchain. It installs `mysql-server` (`:43`)
  although the database runs in the separate `ac-database` container, both gcc and clang, `libboost-all-dev` (`:44`),
  and gives the user passwordless sudo (`:86-87`, stock). The base `Dockerfile` (stock, for `ac-db-import` and
  `ac-authserver`, profile `stock`) is a second full compile of the core inside `docker build`
  (`docker-compose.yml:75-79`
  explains it is skipped by default). Image sizes: UNVERIFIED (no docker use); but see the next item.
- **`.dockerignore` (R-13).** It ignores `/var/*`, `/env/dist/*`, `/build*/` and `.idea`
  (`.dockerignore:1-11`); it does not ignore `apps/forge/python/.venv`, `apps/forge/probes` or `apps/forge/models`. The
  compose build context is `.` (`docker-compose.yml:39`) and `Dockerfile.dev-server:109` does `COPY ... apps`. On the
  owner's checkout that is 15 GB of venv, 3.1 GB of probes and 580 MB of models sent to the daemon and, by the `COPY`,
  written into an image layer that the bind mount then hides at run time. `:112` also copies all of `data/`
  (847 MB of upstream SQL) for the sake of the upstream dashboard.
- **The dev image is not the test image.** `cluster.toml:24` and `deploy-gate.md:22` run tests in a container named
  `claude-syntax`; no tracked file creates it (`grep claude-syntax` finds only those and tests). It is also not
  `ac-animus-forge-dev-server`, which `deploy-gate.md:22` says "--build recreates". `forgectl-test.sh:53-58` relinks
  `unit_tests` with `-resource-dir=/usr/lib/llvm-17/lib/clang/17` because "the container's clang is version 18 and its
  resource directory has no compiler-rt libraries"; `Dockerfile.dev-server` installs `clang llvm lldb lld` (`:35`) and
  nothing named 17, so the test script depends on state the image does not provide (R-12). Why an instrumented link is
  needed at all is unclear (UNVERIFIED: perhaps a stale `FORGE_PGO=generate` in a cached tree).
- **Compose.** `docker-compose.yml` (284 lines): GPU passed with `/dev/kfd` and `/dev/dri` plus
  `seccomp=unconfined` on both build and run services (`:48-52`); `cluster.md:13` still says the two device lines live
  in `docker-compose.override.yml`, which is stale because they are in the shared anchor now (and a duplicate in the
  override file is untested). `:151` uses the `:cached` mount option, a no-op on Linux. Named volumes hold the build
  tree and ccache (`:152-153,277-280`), so the build tree is invisible to the host, backups and `du`. The torch index
  default is ROCm 6.4 (`:138`), the version empty (`:140`, R-15). `restart: "no"` (`:126`) is deliberate.
- **The override file** (`/*.override.yml` is gitignored, `.gitignore`) carries each machine's GPU and volume choices,
  so the cluster's per-machine runtime configuration is not in the repository: not an error, but `cluster.md` and
  `deploy-gate.md` should say what it must contain (UNVERIFIED what it contains).
- **`docker-compose.cluster.yml`** sets `network_mode: host` and resets `networks` and `ports` (`:15-17`), puts the DB
  address on `127.0.0.1` and sets `FORGE_LOCAL_ONLY=1` so TensorBoard stays on loopback (`:25`; honoured by
  `forge-worldserver.sh:82-86`). Its header says "Open the cluster ports in the firewall between the machines": no
  firewall rule is in the repository (section 6).
- **`forge.sh`** (109 lines) is a clean wrapper: `--build` writes `env/dist/.forge-build`, recreates the worldserver
  container, then polls `docker logs` every 10 s for a `[ nn%]` line (`:48-67`). It greps the logs for `error:` or
  `Error N` to report a failed build (`:56`), so a build that fails without either reports only "container is
  exited". It has no
  check that the dev machine's tree equals the cluster's, which is `forgectl`'s job.
- **`forge-worldserver.sh`** (95 lines) is the container command: build if asked or if the CPU signature changed
  (`:38-57`), copy missing `.conf` from `.dist` (`:61-68`; it never overwrites, so a new key added to a `.conf.dist`
  never reaches an existing `.conf`, which is why `forgectl conf-sync` exists), then `animus-venv.sh`, TensorBoard,
  `exec ./worldserver`. The CPU signature (`:42-45`) hashes the first match of `vendor_id`, `cpu family`, `model` and
  `flags`, and misses a microcode or kernel change that disables a feature (e.g. a mitigation switching off `avx512`
  after a BIOS update); the next start would run a binary compiled for a flag the CPU no longer reports. Edge case.
- **`animus-venv.sh`** installs `torch` then `-e .[tensorboard,dev]` only when the import fails (`:29-42`): an existing
  venv is never upgraded or checked against a version, which is how two machines drift (R-15).

### 3.8 CI

15 of the 16 workflows carry `if: github.repository == 'azerothcore/azerothcore-wotlk'` (`grep` over
`.github/workflows`; the 16th, `add-to-project.yml`, files labelled issues into upstream's project board URLs and is
inert). `core-build-pch.yml` and
`core-build-nopch.yml` build and run `unit_tests` through `.github/actions/linux-build/action.yml:215-219` upstream; on
`Moloch17/azerothcore-wotlk` they never start, and they trigger only on `master` or pull requests. The forge's
46 GTest files and 95 pytest files are run only by `forgectl test` on the dev machine (`known-issues.md` D4).

What a fork CI would take, in order:

1. One workflow `forge-ci.yml` with `if: github.repository == 'Moloch17/azerothcore-wotlk'`, on `forge`, in a container
   built from `Dockerfile.dev-server` (needs `libzstd-dev`, present; `hipcc` optional, so the device library is
   skipped, which is also the cheapest path).
2. Job A: configure with `-DBUILD_TESTING=ON -DUSE_COREPCH=OFF -DAPPS_BUILD=world-only -DWITH_LTO=OFF` (LTO off to save
   the link), build `unit_tests`, run `apps/ci/ci-run-unit-tests.sh`. The data-dependent tests skip without
   `FORGE_VISION_DATA` (`known-issues.md` C4), so CI proves compilation and the logic tests, not the map tables.
3. Job B: `python -m venv`, `pip install torch --index-url .../cpu`, `pip install -e apps/forge/python[dev]`,
   `pytest -q` (the default excludes `slow`). `test_forgectl.py` hard-codes the real cluster (R-28) but uses a fake ssh,
   so it runs anywhere. The GPU tests (`test_rollout_graph*.py`) skip without a card (`known-issues.md` A5) and need a
   self-hosted runner.
4. A third, weekly job: the semantic merge checklist of section 1.6 against `upstream/master`, so the merge cost is
   visible before it is paid. Budget: a hosted runner needs ccache and `-j` of 4; an uncached full build is likely well
   over an hour (UNVERIFIED), so the cache is a requirement.
5. The untouched upstream workflows should be left as they are: they are inert and a merge will keep updating them.

## 4. Python packaging and tooling

### 4.1 Package, pins, extras

`apps/forge/python/pyproject.toml` (29 lines):

- Distribution `animus-forge` 0.1.0, `requires-python >=3.11` (`:5`), runtime dependencies `numpy>=1.26`, `pyyaml>=6`,
  `torch>=2.4` (`:6-12`), extras `tensorboard>=2.16` and `dev` = `pytest>=8` (`:15-16`), setuptools build, packages
  `animus*` only (`:21-22`). pytest runs `tests/` with `-m 'not slow'` by default (`:24-27`).
- **No lock file and no upper pin anywhere.** The owner's dev venv resolved to numpy 2.5.3, pytest 9.1.1, tensorboard
  2.21.0 and torch 2.9.1+rocm6.4 (`.venv/lib/python3.12/site-packages/*.dist-info`, the owner's checkout). Another
  machine's first start resolves whatever is newest that day. `animus-venv.sh:29-37` installs `torch` first with
  `ANIMUS_TORCH_VERSION` empty by default (`docker-compose.yml:140`: "empty = the index's newest"), then
  `pip install -e` (`:41`) that skips torch because it is already satisfied. `animus-venv.sh:10-12` and the compose
  comment say "every machine of a cluster runs the same one" torch, because the learners average their networks over
  gloo; nothing enforces it, and `animus/blas.py:6-8` records measurements on three different torch builds
  (2.9.1+rocm6.4, 2.10+rocm7.0, 2.13+rocm7.1) "on the cluster's RX 9060 XTs". Either the cluster does not run one
  version, or that sentence is a history of upgrades; the venvs cannot be inspected from here (R-15). Because the C++
  device library is built against torch's HIP runtime (section 3.3), this is a correctness risk, not only a tidiness
  one.
- **The Python version** is the container's: Ubuntu 24.04's `python3` = 3.12.3 (`.venv/pyvenv.cfg`), because the venv
  is created inside the dev image (`Dockerfile.dev-server:72`). `forgectl` itself runs on the host with `python3` and
  uses `tomllib` (3.11+), `fcntl` and `pty` (Linux only); the host's version is not pinned or checked
  (UNVERIFIED which). The cluster machines run the worldserver in the same image, so they share 3.12.
- **`forgectl` is not part of the package.** `packages.find include = ["animus*"]` excludes `apps/forge/forgectl`; it
  runs through the `forgectl` shim (`forgectl:1-8`), which prepends `apps/forge` to `sys.path`. It is standard-library
  only (no third-party import found in 2,182 lines), which is a virtue, so it needs neither the venv nor torch.
- **No console scripts.** Entry points are `python -m animus.train` (started by the worldserver,
  `LearnerProcess.cpp:92`), `animus.export` (`ForgeCommands.cpp:1127`), `animus.evaluate`, `animus.bench_learner`,
  `animus.human`. `evaluate.py` and `bench_learner.py` have no importer (R-32): they exist for hand use, and
  `evaluate.py:3-4` advertises `--baseline random` (a scripted baseline, principle 14).
- The in-tree venv is described in R-16; its interpreter paths are the container's (`pyvenv.cfg`
  `command = ... /azerothcore/apps/forge/python/.venv`), so the host's Python cannot use it and the editable install's
  `animus_forge.egg-info` (ignored) points into the container too.

### 4.2 Structure, imports, dead code

- 48 modules, 20,835 lines (`animus` 25 modules, `mappo` 6, `human` 17) against 17,972 lines in 88 test modules and
  7 helpers. `train.py` is the hub: it imports `blas`, `explore`, `style`, `distill`, `partners`, `async_sync`,
  `parallel`, `progress`, `runs`, `cast` and `episode_means` (`grep` of import lines); apart from `cast`, `distill`
  and `episode_means` (each also used by `partners.py`, `cast.py` or `evaluation.py`) these have `train.py` as their
  only production importer. A refactor of `train.py` (known-issues G4) is therefore the single choke point.
- **No import cycle**: an AST scan of all 48 modules (top-level and function-level imports alike) found none.
- **Lazy imports.** `known-issues.md` A4 lists the function-level imports and warns that a file changed under a
  running learner can mix versions. Checking each target: every lazily imported module is already imported at the top
  of the same process (`env.py:203` imports `.device`, which `train.py:68` already loaded; `.networks` is loaded by
  `trainer.py:18` and `train.py:49`; `bootstrap.py:464` and `export.py:365,396` import names from the same
  `mappo.networks`). So a running learner cannot pick up new code through them; only `bench_learner.py:155-156`, a
  hand tool, imports something that may be new. **The real window is process starts:** each of the data-parallel ranks
  is a separate interpreter spawned by the worldserver (`LearnerProcess.cpp:92`), the host "restarts them all from the
  latest checkpoint, up to three times" after a failure (`worldserver.conf.dist:5740-5742`), and `forge export` starts a
  fresh one. A `git pull` that lands between two of those starts runs ranks of two versions in
  one gloo group. A4's remedy (stop the learner before changing the tree, which the deploy gate does) is right; the
  hoisting it suggests would change nothing.
- **Dead or hand-only modules:** `bench_learner.py` (its docstring uses the deleted `configs/stage4_duel.yaml`, `:3`),
  `evaluate.py`, and the offline half of `human/` (reader, tracks, dataset, build, fit, mapper, parity;
  `human/__main__.py` is its own entry). The `human` package is 17 of the 48 modules; the learner imports only
  `human.motion` and `human.realism` (`config.py:18`, `style.py:38`, `train.py:56`), for a style reward that is off in
  every live stage. The owner's notes say the realm and its data capture are parked. Not dead, but parked: a decision
  (Q-5).
- `config.py` carries options no live stage uses (`eval.mask_actions`, `style.*`, `distill.*`, `cast.agents`; B8 of
  `known-issues.md`).

### 4.3 Tests

- 88 `test_*.py`, 7 helper modules (`conftest.py`, `sim_threads.py`, `human_capture_writer.py`,
  `slow_gae_reference.py`, ...), `fixtures/` (148 KB x 2 identical, 132 KB), `golden/` (4 `.amdl`, 3 `.json`).
  Markers: only `slow` (`test_parallel.py`, `test_train_run.py`), excluded by default. 19 files skip conditionally
  (CUDA/ROCm card, missing data). Two read the environment: `test_human_reader.py:199` (`ANIMUS_CAPTURE_SAMPLE`) and
  `test_stage_validation.py:23` (`ANIMUS_COMPILE_DB`, a configured build's `compile_commands.json`). Without those
  variables, both skip and nothing in the default run says so beyond pytest's skip count; `forgectl test`'s summary
  would need to show the skip counts to catch it (not checked).
- Tests that bake in operational facts: `test_forgectl.py` (the real cluster's addresses, logins and the container name
  `claude-syntax`, `:85-92,156,216,880`), `test_dungeon_stages.py:228-236`, `test_conf_prune.py`. Editing
  `cluster.toml` fails them (R-28).
- Coverage gaps are in `known-issues.md` C1 to C5 and not repeated.

## 5. Leftovers inventory

Method: `grep -rIE` over the forge-owned trees (`src/server/game/Animus`, `src/test/server/game/Animus`,
`apps/forge`, `docs/forge`, the forge's own core files) and over the lines the forge added to upstream files
(`git diff 37de65eb0 forge`). Counts are lines, not files.

### 5.1 The good news, stated as findings

| Pattern | Forge code | Upstream-edited files (forge's added lines) |
|---|---|---|
| `TODO`, `FIXME`, `HACK`, `XXX` | **0** in `Animus/`, `apps/forge`, `docs/forge`, `forge.sh`, compose files | 0 (the one in the tree is upstream's `apps/docker/docker-cmd.sh:3`) |
| `#if 0` | **0** in `src/server/game`, `src/common`, `src/server/apps`, `src/server/scripts/Commands`, `src/test` | 0 |
| `deprecated` | 0 | 0 |
| commented-out code (heuristic: a `//` line that is a complete statement) | none found in `Animus/`, `ForgeMain.cpp`, `Forge/`, `cs_forge.cpp` | none |
| `obsolete` | 1 in Python (`rename_runs.py` header), 5 in docs | 0 |

The comment style is prose, long and dated. That is a different cost (section 5.4).

### 5.2 By directory

| Directory | Pattern and count | Harmless or hiding a problem |
|---|---|---|
| `src/server/game/Animus` | `stage\d+_` 32 lines; `animus-lib` 2; `curriculum-v1` 2; `.agents/plans` 3; `Playtest` 3 | harmless history in 29 of the 32 stage lines (they cite what a run measured, e.g. `CurriculumTuning.h:46,61,68`, `PetBlock.cpp:243,338,381`). Misleading in two: `ForgeConfig.h:60` (`forge start stage11_raids`) and `ForgeCommands.cpp:506` (`forge fast 30M stage8_duel`), the help text an operator reads |
| `apps/forge/python/animus` | `stage\d+_` 27 (`config.py` 11, `train.py` and `mappo/*` 10); `.agents/plans` 3 | historic measurement comments, except the **usage docstrings** `train.py:3`, `export.py:3,101`, `bench_learner.py:3` (a deleted config and run name as the example command) |
| `apps/forge/python/tests` | `stage\d+_` 15 (mostly `test_stage_names.py`'s archived-name set) | deliberate: that test is the guard against strays (`test_stage_names.py:1-30`) and reads `modules/mod-animus` (`:36`), a gitignored sibling |
| `apps/forge/tools` | `stage\d+_` 35, all in `rename_runs.py` (`:13-28`) | a **one-shot migration script** for runs renamed on an earlier renumbering; its mapping is finished history. Candidate for deletion (R-26) |
| `src/server/apps/worldserver/worldserver.conf.dist` | 7 stage-name lines (`:5068`, `:5616-5629`, `:5764`, `:5798`, `:6286`) | measurement history inside option descriptions; `:5798` ("`stage4_duel` was the default until it was archived") describes a default that no longer exists |
| `src/server/game/Maps`, `Map.cpp:854` | `stage20_quest` | harmless; F-15 |
| `docs/forge/0*.md` | `stage\d+_` 13; `animus-lib` 12; `mod-animus-forge` 9 | **hiding a problem**: chapters 03 and 05 describe the removed module layout (R-22) |
| `docs/forge/reference` | `stage\d+_` 12 | quotes of the code's own comments |
| everywhere | `mod_animus_forge` 64 lines in the scanned trees (25 files repo-wide) | the real per-machine config name (R-20); not a leftover but a legacy name in current use |
| everywhere | `Playtest` 48 lines in 9 files | live gate (R-30) |

### 5.3 Dead and unreachable code in the core

The forge's principle 17 is "dead code is deleted, not gated", and the delta document says the stock bodies were
deleted. Five places still keep the upstream body under an early `return`, which is the opposite of that rule but
keeps the diff small for a merge. After the merge in section 1, upstream's changes to those bodies are silently dead:

| Place | What stays dead |
|---|---|
| `Pet.cpp:547-560` (F-8) | the whole of `SavePetToDB` after `return;`; upstream's 145/122-line pet refactor lands inside it |
| `PlayerUpdates.cpp` `UpdateAdditionalSaves` (merge conflict hunk, `:2473-2482`) | everything after `return;`, including the line upstream just renamed |
| `AchievementMgr.cpp:521` onward | 13 entry points that return first thing; the bodies stay |
| `PlayerStorage.cpp:7199-7221` | not dead (it returns only `if (!create)`); the `create` path is live |
| `MapUpdater::ParallelFor`, `PCQueue::Reset` (forge copy), `SetSimOwned`, `IsSimGroup`, `Set/GetIncludeFlags`, `SetMapUpdateInterval` | section 1.5 |

### 5.4 Stale references to deleted things, and comments that cite files a clone does not have

- **Deleted stages and tools.** `stage\d+_` (142 lines overall), `curriculum-v1` (19), `animus-lib` (19),
  `mod-animus-forge` (13). The git tags exist: `curriculum-v1`, `curriculum-movement-v1`, `pre-cleanup-2026-10-07`,
  `archive/*` (`git tag`). Harmless where dated; problematic only in operator-facing text (above).
- **`camera-vision.GPU.md` is cited on 65 lines in 44 files and exists nowhere in the tree** (`git ls-files | grep -c
  camera-vision` = 0), for example `DeviceApi.h:64`, `VisionDevice.h:27`, `VisionGpu.h:33`, `VisionScene.h:43`,
  `VisionDiff.h:30`, `Vision.hip:23,33`, `DeviceRuntime.h:23`, `game/CMakeLists.txt:111,132`,
  `VisionGpuDataTest.cpp:39`, `VisionGpuTest.cpp:31`, `worldserver.conf.dist`. The "amendment 5" that justifies
  `-ffp-contract=off` (`game/CMakeLists.txt:132`) is in that missing file.
- **Gitignored plan folders cited from code** (`.gitignore` excludes `.agents/plans/**`): `PlayerController.h:29` and
  `ReportCadence.h:27` (`.agents/plans/player-controller/client-constants.md`, the constants' source), `Stages.cpp:23,
  83,436` (`.agents/plans/movement-curriculum/`, `perception-goals/tools/rooms.py`,
  `dungeon-curriculum/tools/sites.py`),
  `StockadeRoomsDataTest.cpp:43` (`perception-goals/tools/table.py`), `DeadminesSitesDataTest.cpp:45` (`sites.py`),
  `parity.py:2`, `human/README.md:4`, `human/FORMAT.md:4`. **This one hides a problem:** the scripts that generate the
  Stockade room table and the Deadmines site table live in those folders, so the tables in `Stages.cpp` cannot be
  regenerated from the repository (`known-issues.md` C4, there for `dungeon-curriculum/tools` being empty "here").
- **Gitignored scripts cited from tracked code:** `var/staging_test.sh` (`testcmd.py:3`, `forgectl-test.sh:3`),
  `var/forge_console.py` (`console.py:3`), `var/camera/composite.py` (`FrameImage.h:66`,
  `VisionFrameImageTest.cpp:138`). The first two are "ported" but not compared.
- **Comments that retell history.** `CurriculumTuning.h` (F2), `config.py` and the conf template carry dated
  measurement narratives (2026-09-18 to 2026-10-07). Valuable as a lab notebook, expensive as a header: the same file is
  included from 18 sites (section 3.6), and every edit to a comment rebuilds them.
- **Lazy-import and "left from the module days" comments** are covered in R-20 and 4.2.

### 5.5 Cleanup list, ordered by value

1. Delete the forge's `PCQueue::Reset` at the merge (R-01): avoids a compile break; one block.
2. Add `libzstd-dev` to the installer and CI package lists, or make zstd `REQUIRED` only when `Animus` is on
   (`game/CMakeLists.txt:77-78`).
3. Fix the three operator-facing examples: `train.py:3`, `export.py:3`, `bench_learner.py:3`, and the console help
   `ForgeConfig.h:60`, `ForgeCommands.cpp:506`.
4. Delete `mod-animus-movement.patch`; decide on the amdl8 patch (R-26).
5. Drop `rename_runs.py` and the stale `.gitignore` un-ignores (R-27).
6. Commit the missing source of truth: `camera-vision.GPU.md` and the table-generator scripts under `docs/forge/` or
   `apps/forge/tools/`, or delete the 65 citation lines and the claim that the tables are generated.
7. Rewrite or delete chapters 03 and 05; move the forge pointer into `AGENTS.md` (R-22, R-23).
8. Remove the dead core additions in section 1.5 (a build is needed; batch with the next C++ change, principle 18).
9. Shorten `CurriculumTuning.h`'s history comments when it is split (F2).

