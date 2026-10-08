# C++ runtime: the process, the learner launcher, the build wiring and the GPU layer

Scope: `src/server/apps/worldserver/ForgeMain.cpp`, `src/server/game/Forge/Forge.{h,cpp}` (`ForgeCore`),
`A/Learner/*`, `A/Json/BoostJson.cpp`, `src/server/game/CMakeLists.txt`, `A/Gpu/*` (`A/` = `src/server/game/Animus/`). Entry point:
[cpp-runtime.md](cpp-runtime.md). Camera code that the GPU layer ports is in [cpp-vision.md](cpp-vision.md).

## `ForgeMain.cpp` (the worldserver's `main`)

This file defines `main()`; upstream `Main.cpp` is not compiled (the app directory is globbed, `src/server/apps/CMakeLists.txt:95-101`,
and only `ForgeMain.cpp` has a `main`). It is unconditional: no flag brings back the stock startup.

Startup order (`main`, `ForgeMain.cpp:337-520`):

1. process holder, `SIGABRT` handler, fatal-signal handlers (SIGSEGV/BUS/FPE/ILL print a backtrace to stderr then re-raise);
2. `sConfigMgr->Configure/LoadAppConfigs`; `ForgeCore::LoadSettings()` (reads `Forge.Playtest`); the legacy
   `modules/mod_animus_forge.conf` is merged if it exists (:359-361);
3. log init (synchronous, no DB appender), OpenSSL thread setup, PRNG seed, a one-thread `IoContext` for SIGINT/SIGTERM
   (`World::StopNow`), `SetProcessPriority`;
4. `LoadModulesConfigs`, script manager, `ForgeStartDB` (`DatabaseLoader` for login, character, world; in playtest also
   the realmlist), `sWorld->SetInitialWorldSettings()`;
5. playtest only: world socket listener (`StartWorldNetwork`);
6. `sScriptMgr->OnStartup()`, **`sAnimusForge->OnStartup()`** (:458: load `ForgeConfig`, configure camera/map/memory, warm
   caches, open the learner socket, log status);
7. outside playtest: `ForgeSealDatabases()` (:462): `AccountMgr::LoadSnapshot()`, then `Seal(strict)` on the three pools
   (`strict` = `Forge.SealStrict`, default on): later reads get no rows and are logged once with their origin; writes are
   dropped (`DatabaseWorkerPool.h:49,216`; test `SealedWriteTest.cpp`);
8. optional SOAP thread (`SOAP.Enabled`), optional console thread (only when stdin is a tty);
9. the update loop; at shutdown the console thread and the IoContext are joined, `sAnimusForge->OnShutdown()`,
   `sScriptMgr->OnShutdown()`; the `shared_ptr` handles unwind the map manager, the database and the scripts.

Update loops: `ForgeUpdateLoop` (:266) calls `sWorld->Update(ForgeCore::TickMs() or tickMs)` with a fixed diff and no sleep, so
the sim runs as fast as it can; it turns on `WarnAboutSyncQueries` for the three pools. `ForgePlaytestLoop` (:244) uses the wall
clock and `MinWorldUpdateTime`.

`ForgeCore` (`Forge.cpp`): `Playtest()` (a plain bool set once), `HasClients()` (`sWorldSessionMgr->GetActiveSessionCount() > 0`),
`SetTickMs/TickMs` (atomic). Owner: [01-forge-core-delta.md](01-forge-core-delta.md).

Observed issues here: the doc comment of `ForgeUpdateLoop` ("Fixed-tick world loop ...", :214-221) is placed above the
unrelated `ForgeSealStrict` comment, so two comments are stacked on one function (:214-230); `ForgeMain.cpp:37` says the
`TC9/libsidecar` plumbing is removed yet `libsidecar` is still linked (`src/server/apps/CMakeLists.txt`, `game/CMakeLists.txt`).

## Learner launcher (`A/Learner/`)

`ChildProcess` (`ChildProcess.{h,cpp}`): `Start(args, workDir, logFile)` uses `posix_spawnp` with `addchdir_np(workDir)`,
stdin `/dev/null`, stdout+stderr appended to the log, `addclosefrom_np(3)` (no descriptor leak), its own process group
(`POSIX_SPAWN_SETPGROUP`, so Ctrl+C reaches only the server), and the worldserver's `environ` (so
`HSA_ENABLE_IPC_MODE_LEGACY=0`, set by `Gpu::PrepareEnvironment`, is inherited). `Poll` reaps with `waitpid(WNOHANG)` and logs
the end once; `FinishedCleanly` = exited with status 0; `FailedUnexpectedly` = exited otherwise and `ExpectExit` was not
called. `Stop(grace)`: wait `grace`, `SIGINT` (Python saves on KeyboardInterrupt), wait, `SIGKILL`, wait; blocks the world
thread up to 3 x grace. The destructor calls `Stop(10 s)`. Nothing ties the child to the parent's death except the socket:
a crashed worldserver leaves the learner to notice the closed socket.

`LearnerProcess` (`LearnerProcess.{h,cpp}`): `Start(config, scenario, resume)` checks `<WorkDir>/animus/train.py` and the stage
YAML exist, then spawns one process per rank (`LearnerRanks`, or all-or-none). Command line (`LearnerArgs`,
`LearnerProcess.cpp:80-155`):

```
<Learner.Python> -u -m animus.train --config <LearnerConfigFor(stage)> --socket <socket> --run-name <stage>
    --runs-dir <OutputDir>/runs --layouts-dir <OutputDir>/layouts [--resume]
    [--set torch_threads=N] [--set cluster_sims=['tcp://..', ..]]
    [--set rank=.. ranks=.. local_rank=.. local_ranks=.. dist_address=.. dist_iface=.. mappo.rank_sync=..]   (cluster ranks)
    [--set rank=k ranks=N dist_address=127.0.0.1:<free port>]                                             (local multi-GPU)
    [--set train_device=.. --set rollout_device=..]  then  AnimusForge.Learner.Args (and the GPU mode's, Fast's) 
```

Working directory: `AnimusForge.Learner.WorkDir` (default the build tree's `apps/forge/python`, baked at configure time as
`FORGE_PYTHON_DIR`, `game/CMakeLists.txt:54`). Python: `Learner.Python`, else `<WorkDir>/.venv/bin/python` if present, else
`python3` from `PATH`. Log: `Learner.LogFile` (rank k > 0: `animus-learner.rank<k>.log` beside it, `RankLogFile`). The socket given
to a worker's own learner is `tcp://127.0.0.1:<port>` when the sim listens on `tcp://0.0.0.0:<port>`. The device of rank k is
`Learner.TrainDevice`/`RolloutDevice` (several ranks: consecutive `cuda:N`) or the counted GPUs. CPU placement: the learner
is pinned with `sched_setaffinity` **after** spawn (`CpuPlacement::PinProcess`, `LearnerProcess.cpp:244`) to the cores the
map update does not use (`MapUpdater::PoolCpus` -> `AwayFrom`), or to `Learner.Cpus`, shared out by whole physical cores
per rank; threads already created by the interpreter before the pin keep the old mask (the comment at :236 claims the pin
is "set before the interpreter has started any thread"; `PinProcess` documents only that threads started later inherit it).
`Poll` stops the other ranks when one fails (`:285-298`). `ManualCommand` prints the equivalent shell line; its `--set
cluster_sims=[...]` has quotes and spaces unescaped.

Lifetime: the learner ends when the socket closes (cancel, shutdown, crash); `TeardownScenario` disconnects first and then
waits up to 15 s (`LEARNER_STOP_GRACE`) for the checkpoint save.

`Json/BoostJson.cpp`: the one translation unit with `#include <boost/json/src.hpp>`; the build needs Boost >= 1.75.

## CMake wiring (`src/server/game/CMakeLists.txt`)

- All `Animus/**` sources are collected by `CollectSourceFiles` like the rest of `game`. `FORGE_PYTHON_DIR` is a compile
  definition for `game` (:54).
- Source hash (:60-75): at configure time `file(GLOB_RECURSE ... Animus/*.cpp *.h *.hip)` (no `CONFIGURE_DEPENDS`), sorted,
  each file's SHA-256 concatenated and hashed again, first 16 hex digits written to `${CMAKE_CURRENT_BINARY_DIR}/ForgeSourceHash.h` as
  `#define FORGE_SOURCE_HASH`. Only `AnimusForge.cpp` includes it (a change rebuilds one file). The hash is stale until cmake
  re-runs: the container script reconfigures on every build (`apps/docker/forge-worldserver.sh:30-36`). Coverage and limits:
  see the fingerprint section of [cpp-runtime-bridge.md](cpp-runtime-bridge.md).
- `libforge-gpu.so` (:104-148): only if `hipcc` is found. Each `Animus/Gpu/Device/*.hip` is compiled by a custom command with `hipcc
  --offload-arch=<FORGE_GPU_ARCHS, default gfx1100> -O3 -fPIC -std=c++20 -fvisibility=hidden`, linked as a shared library
  `forge-gpu` (no HIP runtime linked), `add_dependencies(game forge-gpu)`, installed to `bin`. Without hipcc the build
  prints "built without it" and everything stays on the CPU. Header dependencies are the `Gpu/*.h`, `Gpu/Device/*.h`.

## GPU layer (`A/Gpu/`)

What is built: a device library with one exported symbol `ForgeGpuGetApi()` returning a `ForgeGpuApi` table (`DeviceApi.h:46`,
`FORGE_GPU_API_VERSION` = 6): `Init, Alloc, Free, Export` (IPC handle), `CopyToDevice, CopyToHost, Synchronize, AllocHost,
FreeHost, LastError`. `GpuRuntime.cpp` loads it (`std::call_once`): `PrepareEnvironment()` sets
`HSA_ENABLE_IPC_MODE_LEGACY=0` if unset; `TorchLibDir` runs `<python> -c 'import torch ...'` through `popen` in WorkDir to find the learner's torch
`lib` directory (empty if torch has no HIP); `dlopen(libamdhip64.so, RTLD_NOW|RTLD_GLOBAL)` from there, then
`dlopen(<dir of /proc/self/exe>/libforge-gpu.so, RTLD_LOCAL)`; a table with another version is refused (`AcceptApi`). The worldserver therefore only has HIP in memory if this was asked for.

What uses it:

| Consumer | Needs | Switch |
|---|---|---|
| Device observation buffers (`OfferDevice`/`UploadRows`, `AnimusForge.cpp:2824-2926`) | `Gpu::Api()` non-null | `AnimusForge.Gpu.Observe` (default **0**) loads at startup |

So in production training nothing runs on the GPU from the worldserver: the camera is cast on the CPU (`Vision/VisionCaster.cpp`; the GPU camera was removed 2026-10-08, tag `archive/gpu-camera`),
observations travel over the socket. `Gpu.Observe = 0` is the shipped default (conf.dist: the socket transport measured faster).

Files:

- `Device/Runtime.hip`: the table implementation (one process-wide stream `g_stream`, created non-blocking in `Init`).
  `Device/DeviceRuntime.h` maps HIP names to CUDA under `FORGE_GPU_CUDA` (no CUDA
  build exists in the tree).

## Observed issues

- `Runtime.hip:38-47`: `Init` creates a new stream whenever the requested device differs from the last, never destroying the
  old one (stream leak with several ranks on different GPUs; latent).
- `GpuRuntime.cpp:35`, `ForgeConfig.cpp:143`: shell command lines built by concatenating paths in single quotes.
- `DeviceRuntime.h:22-25` describes a CUDA build "that comes with G0"; no CUDA target exists in `CMakeLists.txt`.
- `LearnerProcess.cpp:236` comment versus `CpuPlacement.h:55` (see above).

## Reviewer notes

- Question: keep the device-buffer path at all (principle 17: dead code is deleted)? Its only user is a default-off switch
  (`AnimusForge.Gpu.Observe`), plus the loader and a hipcc build step. The GPU camera that used to share the library was
  removed 2026-10-08 (tag `archive/gpu-camera`).
- The fingerprint hashes the `.hip` and `Gpu/` sources even on machines without hipcc, so a worker without the device library
  still matches.
