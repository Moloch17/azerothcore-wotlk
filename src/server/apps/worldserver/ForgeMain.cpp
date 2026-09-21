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
 *   - Remote Access                          -- no network operators; the console is enough
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
 *   - the console (CLI thread, Console.Enable): operators run commands while a training run goes.
 *   - SOAP, when SOAP.Enabled is set: the same console commands, reachable by something that is
 *     not a terminal. Off by default, so the sim still opens no listener unless asked.
 */

#include "ACSoap.h"
#include "BattlegroundMgr.h"
#include "BigNumber.h"
#include "CliRunnable.h"
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DatabaseLoader.h"
#include "GameTime.h"
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
#include "Unit.h"
#include "World.h"
#include <boost/asio/signal_set.hpp>
#include <algorithm>
#include <csignal>
#include <limits>
#include <memory>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef _ACORE_CORE_CONFIG
#define _ACORE_CORE_CONFIG "worldserver.conf"
#endif

namespace
{
    /// Joins the console thread. It returns on its own once the world stops: readline's event hook
    /// (CliRunnable.cpp) ends the pending read when World::IsStopped().
    struct ForgeCliThreadDeleter
    {
        void operator()(std::thread* cliThread) const
        {
            cliThread->join();
            delete cliThread;
        }
    };

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
    /// One tick is one agent decision: its length is mod-animus-forge's AnimusForge.DecisionMs, read once here
    /// (module configs are loaded by then), so game time and decisions advance together.
    ///
    /// Caveat: much of game/ reads getMSTime() directly, so the synthetic diff decouples this
    /// loop from wall clock but not every downstream timer. A clock shim behind getMSTime() is
    /// what closes that gap; it does not belong in this file.
    void ForgeUpdateLoop()
    {
        // Game milliseconds advanced per tick, independent of how long the tick really took.
        //
        // A decision is AnimusForge.DecisionMs of game time and always has been; AnimusForge.TicksPerDecision says
        // how many world updates that decision is cut into. At 1 -- the default, and what the sim did before the two
        // were separable -- a tick is a decision. Above 1 the world moves in finer steps while the module still
        // decides every DecisionMs (AnimusForge::Forge::OnUpdate holds the decision back), so splines, auras and the
        // fight run smoothly without the policy paying for more decisions. The module checks this arithmetic against
        // the diff it is handed and complains if a stale worldserver disagrees.
        uint32 const decisionMs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.DecisionMs", 250));
        uint32 const ticksPerDecision =
            std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.TicksPerDecision", 1));
        uint32 const tickMs = std::max<uint32>(1, decisionMs / ticksPerDecision);

        // 0 runs until stopped; otherwise stop after this many ticks (batch runs).
        constexpr uint64 maxTicks = 0;

        if (maxTicks)
            LOG_INFO("server.worldserver", "Sim loop: stopping after {} ticks", maxTicks);

        LoginDatabase.WarnAboutSyncQueries(true);
        CharacterDatabase.WarnAboutSyncQueries(true);
        WorldDatabase.WarnAboutSyncQueries(true);

        uint64 ticks = 0;

        while (!World::IsStopped())
        {
            ++World::m_worldLoopCounter;

            // Fixed diff, never wall clock: the sim advances in deterministic steps and runs
            // as fast as the CPU allows.
            sWorld->Update(tickMs);

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

    // The console: commands typed into the worldserver's terminal are queued and run on the world
    // thread between ticks (World::ProcessCliCommands), so they work while the sim trains; a module
    // that blocks the world thread (waiting on a learner) runs the queue itself meanwhile. End of input
    // stops the server, so the console only starts when stdin is a terminal: a server started without
    // one (a detached container without stdin, a batch job) keeps running.
    // SOAP: the console's commands over HTTP, for a caller that has no terminal -- the dashboard's stage
    // controls are the reason it exists. Same handler and the same SEC_ADMINISTRATOR check as a typed
    // command, so it adds no authority the console does not already have. Off unless SOAP.Enabled is set,
    // which keeps "the sim opens no listener" true for every run that does not ask for one. The thread
    // polls World::IsStopped and returns on shutdown; joined below with the console's.
    std::shared_ptr<std::thread> soapThread;
    if (sConfigMgr->GetOption<bool>("SOAP.Enabled", false))
    {
        std::string const soapIp = sConfigMgr->GetOption<std::string>("SOAP.IP", "127.0.0.1");
        uint16 const soapPort = uint16(sConfigMgr->GetOption<int32>("SOAP.Port", 7878));
        soapThread.reset(new std::thread(ACSoapThread, soapIp, soapPort), [](std::thread* thread)
        {
            thread->join();
            delete thread;
        });

        LOG_INFO("server.worldserver", "SOAP is listening on {}:{}", soapIp, soapPort);
    }

    std::unique_ptr<std::thread, ForgeCliThreadDeleter> cliThread;
    if (sConfigMgr->GetOption<bool>("Console.Enable", true))
    {
        if (::isatty(STDIN_FILENO))
            cliThread.reset(new std::thread(CliThread));
        else
            LOG_INFO("server.worldserver", "Console disabled: stdin is not a terminal");
    }

    ForgeUpdateLoop();

    // Shutdown starts here. The console thread notices the stopped world and exits. Stop the
    // IoContext next so nothing posts work into a world that is being torn down; the remaining
    // teardown runs in reverse declaration order:
    // mapManagementHandle -> dbHandle -> sScriptMgrHandle -> opensslHandle.
    cliThread.reset();
    threadPool.reset();

    sLog->SetSynchronous();

    sScriptMgr->OnShutdown();

    LOG_INFO("server.worldserver", "Halting process...");

    // 0 - normal shutdown
    // 1 - shutdown at error
    // 2 - restart command used, this code can be used by restarter for restart AzerothCore
    return World::GetExitCode();
}
