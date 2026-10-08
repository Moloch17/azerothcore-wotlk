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

