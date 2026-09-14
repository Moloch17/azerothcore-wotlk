/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * Portions of this file are derived from the AzerothCore Project.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Forge sim entry point.
 *
 * This is the replacement for worldserver's main(). It is the only startup path
 * this fork builds: an instance-only, faster-than-real-time simulation host driven
 * by in-process sessionless bots. It is unconditional -- there is no flag, config
 * switch or fallback to the stock startup path.
 *
 * This file defines main() directly. Upstream's Main.cpp is dropped from the worldserver
 * source list in src/server/apps/CMakeLists.txt, so it stays on disk untouched and
 * uncompiled: upstream can keep adding startup steps to it and a rebase never conflicts,
 * because this fork carries no diff inside it.
 *
 * Removed relative to the stock worldserver main, and why:
 *   - all Windows/service/winmm logic        -- Linux-only sim host
 *   - WorldSocketMgr listener + WorldSocket  -- bots are in-process and sessionless
 *   - SOAP, Remote Access, CLI thread        -- headless batch runs, no operators
 *   - Metric, AppenderDB, PID file, banner   -- telemetry/ops surface the sim does not use
 *   - FreezeDetector                         -- it ABORT()s; a sim tick is allowed to be slow
 *   - TC9/libsidecar cluster plumbing        -- single process, never clustered
 *   - SecretMgr (TOTP)                       -- only the auth login flow consumes it
 *   - LoadRealmInfo + all realmlist UPDATEs  -- no client is ever handed an address
 *   - WORLD_UPD_VERSION write                -- the sim does not stamp the world DB
 *
 * Kept deliberately:
 *   - module config/script/DB loading: DatabaseLoader takes AC_MODULES_LIST and module SQL
 *     updates are load-bearing for schema.
 *   - OpenSSL thread setup and PRNG seeding: BigNumber and packet crypto are linked and used
 *     regardless of whether a listener exists.
 *   - realm.Id.Realm: ~22 sites under game/ and shared/ scope GUIDs and account state by it.
 */

#include "BattlegroundMgr.h"
#include "BigNumber.h"
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DatabaseLoader.h"
#include "GitRevision.h"
#include "IoContext.h"
#include "Log.h"
#include "MapMgr.h"
#include "ModuleMgr.h"
#include "ModulesScriptLoader.h"
#include "MySQLThreading.h"
#include "OpenSSLCrypto.h"
#include "OutdoorPvPMgr.h"
#include "ProcessPriority.h"
#include "Realm.h"
#include "ScriptLoader.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "World.h"
#include <boost/asio/signal_set.hpp>
#include <algorithm>
#include <csignal>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#ifndef _ACORE_CORE_CONFIG
#define _ACORE_CORE_CONFIG "worldserver.conf"
#endif

namespace
{
    void ForgeSignalHandler(boost::system::error_code const& error, int /*signalNumber*/)
    {
        if (!error)
            World::StopNow(SHUTDOWN_EXIT_CODE);
    }

    bool ForgeStartDB()
    {
        MySQL::Library_Init();

        DatabaseLoader loader("server.worldserver", DatabaseLoader::DATABASE_MASK_ALL, AC_MODULES_LIST);
        loader
            .AddDatabase(LoginDatabase, "Login")
            .AddDatabase(CharacterDatabase, "Character")
            .AddDatabase(WorldDatabase, "World");

        if (!loader.Load())
            return false;

        // No realmlist row is read and no realmlist flags are written: the sim never advertises
        // itself to a client. Only the fields the core actually reads are populated.
        realm.Id.Realm = 1;  // scopes accounts, characters and GUIDs

        sWorld->LoadDBVersion();

        LOG_INFO("server.loading", "> RealmID:              {}", realm.Id.Realm);
        LOG_INFO("server.loading", "> Version DB world:     {}", sWorld->GetDBVersion());

        sScriptMgr->OnAfterDatabasesLoaded(loader.GetUpdateFlags());

        return true;
    }

    void ForgeStopDB()
    {
        CharacterDatabase.Close();
        WorldDatabase.Close();
        LoginDatabase.Close();

        MySQL::Library_End();
    }

    /// Fixed-tick world loop.
    ///
    /// Tuning constants are declared inline below, not read from config.
    ///
    /// Caveat: much of game/ reads getMSTime() directly, so the synthetic diff decouples this
    /// loop from wall clock but not every downstream timer. A clock shim behind getMSTime() is
    /// what closes that gap; it does not belong in this file.
    void ForgeUpdateLoop()
    {
        // Game milliseconds advanced per tick, independent of how long the tick really took.
        constexpr uint32 tickMs = 50;

        // 0 runs until stopped; otherwise stop after this many ticks (batch runs).
        constexpr uint32 maxTicks = 0;

        LOG_INFO("server.worldserver", "Sim loop: {} ms tick, unthrottled", tickMs);

        if (maxTicks)
            LOG_INFO("server.worldserver", "Sim loop: stopping after {} ticks", maxTicks);

        LoginDatabase.WarnAboutSyncQueries(true);
        CharacterDatabase.WarnAboutSyncQueries(true);
        WorldDatabase.WarnAboutSyncQueries(true);

        // Throughput reporting: min/max/average ticks per second over each 10 second window.
        // The clock is read once per sampleTicks rather than once per tick -- at sim speed a
        // per-tick getMSTime() would be thousands of reads a second for one log line.
        constexpr uint32 sampleTicks = 1000;
        constexpr uint32 reportIntervalMs = 10000;

        uint32 ticks = 0;
        uint32 sinceSample = 0;                 // ticks since the last clock read
        uint32 secondTicks = 0;                 // ticks in the second currently being measured
        uint32 windowTicks = 0;                 // ticks in the current reporting window
        uint32 minRate = std::numeric_limits<uint32>::max();
        uint32 maxRate = 0;
        uint32 samples = 0;
        uint32 secondStart = getMSTime();
        uint32 windowStart = secondStart;

        while (!World::IsStopped())
        {
            ++World::m_worldLoopCounter;

            // Fixed diff, never wall clock: the sim advances in deterministic steps and runs
            // as fast as the CPU allows.
            sWorld->Update(tickMs);

            ++secondTicks;
            ++windowTicks;

            if (++sinceSample >= sampleTicks)
            {
                sinceSample = 0;

                uint32 const now = getMSTime();
                uint32 const secondMs = getMSTimeDiff(secondStart, now);

                // Close off a per-second sample. If the sim is running slower than sampleTicks
                // per second this bucket covers more than a second, so derive the rate from the
                // elapsed time rather than assuming exactly 1000 ms.
                if (secondMs >= 1000)
                {
                    uint32 const rate = uint32(secondTicks * 1000.0 / secondMs);
                    minRate = std::min(minRate, rate);
                    maxRate = std::max(maxRate, rate);
                    ++samples;

                    secondTicks = 0;
                    secondStart = now;
                }

                uint32 const windowMs = getMSTimeDiff(windowStart, now);
                if (windowMs >= reportIntervalMs && samples)
                {
                    // Game time advanced in this window: every tick is a fixed tickMs step,
                    // so this is an exact count rather than an estimate. Hours are not wrapped
                    // at 24 -- a fast window can simulate days.
                    uint64 const gameSeconds = uint64(windowTicks) * tickMs / 1000;

                    LOG_INFO("server.worldserver",
                        "Sim: {} ticks/s avg, {} min, {} max over {} s -- {}:{:02}:{:02} game time",
                        uint32(windowTicks * 1000.0 / windowMs), minRate, maxRate, windowMs / 1000,
                        gameSeconds / 3600, (gameSeconds % 3600) / 60, gameSeconds % 60);

                    windowTicks = 0;
                    windowStart = now;
                    minRate = std::numeric_limits<uint32>::max();
                    maxRate = 0;
                    samples = 0;
                }
            }

            if (maxTicks && ++ticks >= maxTicks)
            {
                LOG_INFO("server.worldserver", "Sim reached the tick limit ({}), stopping...", maxTicks);
                World::StopNow(SHUTDOWN_EXIT_CODE);
            }
        }

        LoginDatabase.WarnAboutSyncQueries(false);
        CharacterDatabase.WarnAboutSyncQueries(false);
        WorldDatabase.WarnAboutSyncQueries(false);
    }

}

/// Launch the Forge sim host. Returns the process exit code
/// (0 normal, 1 error, 2 restart requested).
int main(int argc, char** argv)
{
    Acore::Impl::CurrentServerProcessHolder::_type = SERVER_PROCESS_WORLDSERVER;
    signal(SIGABRT, &Acore::AbortHandler);

    // The config system itself stays: DatabaseLoader gets its connection info from it and
    // SetInitialWorldSettings reads hundreds of values out of it. Only Forge's own knobs
    // above are hardcoded.
    sConfigMgr->Configure(sConfigMgr->GetConfigPath() + std::string(_ACORE_CORE_CONFIG),
        { argv, argv + argc }, CONFIG_FILE_LIST);

    if (!sConfigMgr->LoadAppConfigs())
        return 1;

    std::shared_ptr<Acore::Asio::IoContext> ioContext = std::make_shared<Acore::Asio::IoContext>();

    // No AppenderDB: the sim does not log to the database. Logging is synchronous, so the
    // IoContext is never handed to it.
    sLog->Initialize(nullptr);

    LOG_INFO("server.worldserver", "{} (forge)", GitRevision::GetFullVersion());
    LOG_INFO("server.worldserver", "> Using configuration file       {}", sConfigMgr->GetFilename());

    OpenSSLCrypto::threadsSetup();
    std::shared_ptr<void> opensslHandle(nullptr, [](void*) { OpenSSLCrypto::threadsCleanup(); });

    // Seed OpenSSL's PRNG up front so the first BigNumber::SetRand does not stall.
    BigNumber seed;
    seed.SetRand(16 * 8);

    // Signal handlers must be installed before the IoContext threads start, or they would
    // unblock and exit immediately.
    boost::asio::signal_set signals(*ioContext, SIGINT, SIGTERM);
    signals.async_wait(ForgeSignalHandler);

    std::shared_ptr<std::vector<std::thread>> threadPool(new std::vector<std::thread>(),
        [ioContext](std::vector<std::thread>* del)
    {
        ioContext->stop();
        for (std::thread& thr : *del)
            thr.join();

        delete del;
    });

    // One thread: the IoContext now serves only signal handling.
    threadPool->push_back(std::thread([ioContext]() { ioContext->run(); }));

    // No affinity mask (leave placement to the OS), high priority requested.
    SetProcessPriority("server.worldserver", 0, true);

    sConfigMgr->LoadModulesConfigs();

    sScriptMgr->SetScriptLoader(AddScripts);
    sScriptMgr->SetModulesLoader(AddModulesScripts);

    std::shared_ptr<void> sScriptMgrHandle(nullptr, [](void*) { sScriptMgr->Unload(); });

    LOG_INFO("server.loading", "Initializing Scripts...");
    sScriptMgr->Initialize();

    if (!ForgeStartDB())
        return 1;

    std::shared_ptr<void> dbHandle(nullptr, [](void*) { ForgeStopDB(); });

    Acore::Module::SetEnableModulesList(AC_MODULES_LIST);

    sWorld->SetInitialWorldSettings();

    std::shared_ptr<void> mapManagementHandle(nullptr, [](void*)
    {
        // Unload battleground templates before the singletons they reference are destroyed.
        sBattlegroundMgr->DeleteAllBattlegrounds();

        sOutdoorPvPMgr->Die();                     // unload it before MapMgr
        sMapMgr->UnloadAll();                      // unload all grids (including locked in memory)

        sScriptMgr->OnAfterUnloadAllMaps();
    });

    LOG_INFO("server.worldserver", "{} (Animus Forge) ready...", GitRevision::GetFullVersion());

    sScriptMgr->OnStartup();

    ForgeUpdateLoop();

    // Shutdown starts here. Stop the IoContext first so nothing posts work into a world that
    // is being torn down; the remaining teardown runs in reverse declaration order:
    // mapManagementHandle -> dbHandle -> sScriptMgrHandle -> opensslHandle.
    threadPool.reset();

    sLog->SetSynchronous();

    sScriptMgr->OnShutdown();

    LOG_INFO("server.worldserver", "Halting process...");

    // 0 - normal shutdown
    // 1 - shutdown at error
    // 2 - restart command used, this code can be used by restarter for restart AzerothCore
    return World::GetExitCode();
}
