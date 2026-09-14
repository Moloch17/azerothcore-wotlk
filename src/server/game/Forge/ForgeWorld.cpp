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
 * World::ForgeUpdate -- the sim host's world tick.
 *
 * Called from the top of World::Update, which returns immediately afterwards. Upstream's body
 * is left in place untouched but never runs, so upstream can keep editing World::Update and a
 * rebase never conflicts with us.
 *
 * Defining a member of World in this translation unit gives us private access to _timers[],
 * _queryProcessor and friends without friend declarations or accessors.
 *
 * Dropped relative to the stock tick, and why:
 *   - sAuctionMgr, sLFGMgr (x2), sOutdoorPvPMgr, sWorldState, sBattlefieldMgr
 *                                  -- ungated every-tick calls the sim has no use for; this is
 *                                     the bulk of the win in an unthrottled loop
 *   - ProcessCliCommands()         -- no CLI thread exists in the sim host
 *   - sMetric->Update(), METRIC_*  -- Metric is never initialised
 *   - sToCloud9Sidecar block       -- single process, never clustered
 *   - sWorldUpdateTime Update/Record
 *                                  -- percentile bookkeeping only TC9Sidecar reads, plus
 *                                     per-tick slow-update logging; replaced by the once-a-second
 *                                     tick-rate line in ForgeUpdateLoop
 *   - DynamicVisibilityMgr::Update -- see below
 *   - WUPDATE_5_SECS (expired ban delete), WUPDATE_WHO_LIST, WUPDATE_UPTIME, WUPDATE_CLEANDB,
 *     WUPDATE_AUTOBROADCAST        -- these advance on our fixed diff, so at 50 ms/tick the
 *                                     5-second ones fire every 100 ticks: hundreds of DB
 *                                     statements per wall-second at sim speed
 *   - quest/BG/calendar/guild-cap resets, mail expiry
 *                                  -- wall-clock deadlines, so nearly free either way; dropped
 *                                     for surface area rather than speed
 *   - WUPDATE_EVENTS               -- sGameEventMgr changes which creatures exist; a content
 *                                     decision, restore by re-adding the block below
 *
 * Fixed visibility: DynamicVisibilityMgr::visibilitySettingsIndex is a static initialised to 0
 * and only ever rises once session count reaches 500. With at most 40 bots it can never leave
 * tier 0, so *not* calling Update() pins exactly the settings a small realm would use --
 * 300 ms visibility notify, 150 ms AI notify, 1.0 required move distance squared.
 */

#include "BattlegroundMgr.h"
#include "DatabaseEnv.h"
#include "InstanceSaveMgr.h"
#include "MapMgr.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSessionMgr.h"

void World::ForgeUpdate(uint32 diff)
{
    ///- Update the game time and check for shutdown time
    _UpdateGameTime();

    ///- Advance the interval timers. Only WUPDATE_PINGDB is acted on below, but they are all
    /// stepped so anything that reads one sees a sane value.
    for (int i = 0; i < WUPDATE_COUNT; ++i)
    {
        if (_timers[i].GetCurrent() >= 0)
            _timers[i].Update(diff);
        else
            _timers[i].SetCurrent(0);
    }

    ///- Update sessions: the bots are WorldSessions with no socket, so this is what puts them
    /// in the world.
    sWorldSessionMgr->UpdateSessions(diff);

    ///- The simulation itself.
    sMapMgr->Update(diff);

    ///- Battlegrounds are instances the sim will run.
    sBattlegroundMgr->Update(diff);

    ///- Complete async queries. Without this the callback queue grows without bound and
    /// character loads never finish.
    ProcessQueryCallbacks();

    ///- Instance reset bookkeeping.
    sInstanceSaveMgr->Update();

    ///- Where the bot orchestration module hooks in.
    sScriptMgr->OnWorldUpdate(diff);

    ///- Ping to keep MySQL connections alive. Kept because a long run would otherwise sit idle
    /// past the server's wait_timeout and lose its pools.
    if (_timers[WUPDATE_PINGDB].Passed())
    {
        _timers[WUPDATE_PINGDB].Reset();
        LOG_DEBUG("sql.driver", "Ping MySQL to keep connection alive");
        CharacterDatabase.KeepAlive();
        LoginDatabase.KeepAlive();
        WorldDatabase.KeepAlive();
    }
}
