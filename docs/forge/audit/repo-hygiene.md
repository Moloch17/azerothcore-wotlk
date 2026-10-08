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

