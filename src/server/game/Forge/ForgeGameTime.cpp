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
 * GameTime::ForgeAdvanceGameTimers -- the sim clock.
 *
 * Stock GameTime::UpdateGameTimers() refills the per-tick time cache from steady_clock and
 * system_clock, so cooldowns, GCD, proc cooldowns, respawns and instance resets all run on the
 * wall clock while cast bars, auras and swing timers run on the tick diff. At sim speed that
 * mismatch is enormous. The sim host calls this instead (from World::ForgeUpdate), advancing the
 * same four cached values by exactly the fixed tick diff, so every GameTime reader moves on game
 * time. UpdateGameTimers() itself is untouched -- the sim simply never calls it, and its unit
 * tests keep their wall-clock semantics.
 *
 * getMSTime() deliberately stays on the wall clock: database timing, logging, load measurements
 * and the throughput report in ForgeUpdateLoop all need real time.
 *
 * Gameplay sites that read the raw clock instead of GameTime were converted in place so they
 * follow the sim clock too. Recorded here so the fork's divergence lives in one place:
 *
 *   Entities/Player/Player.cpp       HasSpellCooldown, HasSpellItemCooldown,
 *                                    GetSpellCooldownDelay: getMSTime() -> GetGameTimeMS()
 *   Entities/Player/Player.cpp       item proc cooldown: steady_clock::now() -> GameTime::Now()
 *   Entities/Unit/CharmInfo.cpp      GlobalCooldownMgr::GetGlobalCooldown: getMSTime() -> GetGameTimeMS(),
 *                                    and the time left through wrap-safe getMSTimeDiff
 *   Entities/Unit/Unit.cpp           GetProcAurasTriggeredOnEvent: steady_clock::now() -> GameTime::Now()
 *   Spells/Auras/SpellAuras.cpp      Aura::ResetProcCooldown: steady_clock::now() -> GameTime::Now()
 *   scripts/Spells/spell_paladin.cpp Sacred Shield internal cooldown: steady_clock::now() -> GameTime::Now()
 *   scripts/.../boss_xt002.cpp       getMSTime() -> GetGameTimeMS() (x2)
 *   scripts/.../boss_jeklik.cpp      _scheduler.Update() -> _scheduler.Update(diff)
 *   scripts/.../zone_howling_fjord   _scheduler.Update() -> _scheduler.Update(diff)
 *   common/Utilities/TaskScheduler   GetNextGroupOccurrence: clock_t::now() -> _now
 *
 * Deliberately left on the wall clock -- dropped from the sim tick, or client-socket only:
 * WorldSession time sync, MovementHandler client sync, LFGMgr, ArenaSpectator, WorldState,
 * GameEventMgr, Battlefield, Transport first-departure sync (continents are skipped),
 * scourge_invasion, midsummer, cs_mmaps, UpdateTime.
 *
 * No clock budget: the game clock is 64-bit milliseconds, and the absolute timestamps compared
 * against it (player and creature spell cooldowns including infinityCooldownDelay "infinite"
 * ones, creature school lockouts, gameobject cooldowns, Sanctuary, SotA demolishers, the Eclipse
 * and turkey marker script timers) are uint64. Relative timers compared through
 * getMSTimeDiff stay uint32 and are wrap-safe. Battleground queue join/invite times and the
 * uint32 fields sent to clients are left as they are: the sim runs neither queues nor clients.
 */

#include "GameTime.h"
#include "Timer.h"
#include <chrono>

namespace GameTime
{
    // Defined in GameTime.cpp. They are namespace-scope and not static, so they have external
    // linkage; declaring them here avoids touching the upstream file. If upstream ever makes them
    // static, this fails at link time rather than silently.
    extern Seconds GameTime;
    extern Milliseconds GameMSTime;
    extern SystemTimePoint GameTimeSystemPoint;
    extern TimePoint GameTimeSteadyPoint;

    void ForgeAdvanceGameTimers(Milliseconds diff)
    {
        using namespace std::chrono;

        // Seed once from the wall clock so absolute times loaded from the database -- respawn
        // times, instance saves -- are measured against the real date the sim started on.
        static bool seeded = false;
        if (!seeded)
        {
            GameMSTime = GetTimeMS();
            GameTimeSystemPoint = system_clock::now();
            GameTimeSteadyPoint = steady_clock::now();
            seeded = true;
        }

        GameMSTime += diff;
        GameTimeSystemPoint += diff;
        GameTimeSteadyPoint += diff;

        // Derive whole seconds from the system point so sub-second remainders carry across ticks.
        GameTime = duration_cast<Seconds>(GameTimeSystemPoint.time_since_epoch());
    }
}
