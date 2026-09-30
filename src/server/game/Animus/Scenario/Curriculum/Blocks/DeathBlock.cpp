/*
 * This file is part of the Animus Forge project, based on AzerothCore.
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

#include "DeathBlock.h"
#include "BotFactory.h"
#include "Corpse.h"
#include "GameGraveyard.h"
#include "GameTime.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MotionMaster.h"
#include "Player.h"
#include "SeatView.h"
#include <cmath>
#include <list>

namespace
{
    using namespace Animus::Curriculum;

    bool IsGhost(Player const* bot)
    {
        return bot && bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
    }

    bool ReclaimReady(Player* bot)
    {
        Corpse const* corpse = bot ? bot->GetCorpse() : nullptr;
        if (!corpse || !IsGhost(bot) || !corpse->IsInMap(bot) || !corpse->IsWithinDist(bot, CORPSE_RECLAIM_RADIUS, true))
            return false;
        return time_t(corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP))
            <= time_t(GameTime::GetGameTime().count());
    }
}

uint32 Animus::Curriculum::DeathBlock::HostilesNear(WorldObject const* at, Player* bot, float range, bool aggroOnly)
{
    if (!at || !bot)
        return 0;
    std::list<Creature*> creatures;
    Acore::AllWorldObjectsInRange check(at, range);
    Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> searcher(at, creatures, check);
    Cell::VisitObjects(at, searcher, range);
    uint32 count = 0;
    for (Creature const* creature : creatures)
    {
        if (!creature->IsAlive() || creature->IsCritter() || !creature->IsHostileTo(bot))
            continue;
        if (aggroOnly && at->GetExactDist(creature) > creature->GetAggroRange(bot) + SAFE_RISE_MARGIN)
            continue;
        ++count;
    }
    return count;
}

Animus::Curriculum::BlockSize Animus::Curriculum::DeathBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, ACTION_COUNT };
}

bool Animus::Curriculum::DeathBlock::IsAllowed(SeatView const& view, uint32 action)
{
    Player* bot = view.Bot;
    if (!bot || bot->IsAlive())
        return false;
    bool const ghost = IsGhost(bot);
    switch (action)
    {
        case ACTION_RELEASE:        return !ghost && view.DeathRuns;
        case ACTION_ACCEPT:         return view.DeathRuns && bot->isResurrectRequested();
        case ACTION_RUN_TO_CORPSE:
        {
            Corpse const* corpse = bot->GetCorpse();
            return ghost && corpse && corpse->IsInMap(bot) && bot->GetExactDist2d(corpse) > 5.0f;
        }
        case ACTION_RISE_AT_CORPSE: return ReclaimReady(bot);
        case ACTION_SPIRIT_HEALER:
        {
            // At the graveyard, where the spirit healer stands.
            GraveyardStruct const* graveyard = ghost && view.DeathRuns
                ? sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId()) : nullptr;
            return graveyard && graveyard->Map == bot->GetMapId()
                && bot->GetExactDist2d(graveyard->x, graveyard->y) <= SPIRIT_HEALER_RANGE;
        }
        default:                    return false;
    }
}

void Animus::Curriculum::DeathBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;
    if (!bot)
        return;
    bool const ghost = IsGhost(bot);
    obs[OBS_DEAD] = !bot->IsAlive() && !ghost ? 1.0f : 0.0f;
    obs[OBS_GHOST] = ghost ? 1.0f : 0.0f;
    obs[OBS_RESURRECT_OFFERED] = bot->isResurrectRequested() ? 1.0f : 0.0f;
    obs[OBS_DEAD_TIME] = view.DeadSeconds > 0.0f ? std::log1p(view.DeadSeconds) / std::log1p(600.0f) : 0.0f;
    obs[OBS_SICKNESS] = bot->HasAura(SICKNESS_SPELL) ? 1.0f : 0.0f;
    obs[OBS_DEATH_RUNS] = view.DeathRuns ? 1.0f : 0.0f;
    if (Corpse* corpse = bot->GetCorpse(); corpse && corpse->IsInMap(bot))
    {
        obs[OBS_CORPSE_DISTANCE] = std::min(1.0f, bot->GetExactDist2d(corpse) / 100.0f);
        float const bearing = bot->GetRelativeAngle(corpse);
        obs[OBS_CORPSE_SIN] = std::sin(bearing);
        obs[OBS_CORPSE_COS] = std::cos(bearing);
        uint32 const alive = HostilesNear(corpse, bot, CORPSE_HOSTILE_RANGE, false);
        obs[OBS_HOSTILES_AT_CORPSE] = std::min(1.0f, float(alive) / 4.0f);
    }
    obs[OBS_RECLAIM_READY] = ReclaimReady(bot) ? 1.0f : 0.0f;
    if (ghost)
        if (GraveyardStruct const* graveyard = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
            graveyard && graveyard->Map == bot->GetMapId())
            obs[OBS_GRAVEYARD_DISTANCE] = std::min(1.0f, bot->GetExactDist2d(graveyard->x, graveyard->y) / 100.0f);

    if (mask)
        for (uint32 action = 0; action < ACTION_COUNT; ++action)
            mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void Animus::Curriculum::DeathBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    Player* bot = view.Bot;
    if (!IsAllowed(view, local))
        return;
    switch (local)
    {
        case ACTION_RELEASE:
        {
            // To the nearest graveyard on the same map; one on another map (an instance's) would take the ghost
            // out of the episode's world, so it stays where it fell instead.
            bot->BuildPlayerRepop();
            GraveyardStruct const* graveyard = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
            if (graveyard && graveyard->Map == bot->GetMapId())
                BotFactory::TeleportWithinMap(bot, Position(graveyard->x, graveyard->y, graveyard->z,
                    bot->GetOrientation()));
            result.Released = true;
            break;
        }
        case ACTION_ACCEPT:
            // Taken by whoever runs the seat (StageScenario::AcceptResurrections, the module's party), which knows
            // who offered it and pays them when it lands.
            result.AcceptResurrection = true;
            break;
        case ACTION_RUN_TO_CORPSE:
            if (Corpse* corpse = bot->GetCorpse())
                bot->GetMotionMaster()->MovePoint(0, corpse->GetPosition());
            break;
        case ACTION_RISE_AT_CORPSE:
            bot->ResurrectPlayer(0.5f);
            bot->SpawnCorpseBones();
            result.RoseAtCorpse = true;
            break;
        case ACTION_SPIRIT_HEALER:
            bot->ResurrectPlayer(0.5f, true);
            bot->SpawnCorpseBones();
            result.SpiritHealer = true;
            break;
        default:
            break;
    }
}

std::string Animus::Curriculum::DeathBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    switch (local)
    {
        case ACTION_RELEASE:        return "release_spirit";
        case ACTION_ACCEPT:         return "accept_resurrection";
        case ACTION_RUN_TO_CORPSE:  return "run_to_corpse";
        case ACTION_RISE_AT_CORPSE: return "rise_at_corpse";
        case ACTION_SPIRIT_HEALER:  return "spirit_healer";
        default:                    return {};
    }
}
