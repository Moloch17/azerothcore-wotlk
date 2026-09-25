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

#include "Encounters.h"
#include "Opponents.h"
#include "Player.h"
#include "Random.h"

namespace
{
    // Player faction templates: a human's (Alliance) and an orc's (Horde). Enemies get the other side's.
    constexpr uint32 FACTION_ALLIANCE_PLAYER = 1;
    constexpr uint32 FACTION_HORDE_PLAYER = 2;
}

void Animus::Curriculum::EnemyPlayers::MakeEnemies(Player* player, Player* enemy)
{
    enemy->SetFaction(player->GetTeamId() == TEAM_ALLIANCE ? FACTION_HORDE_PLAYER : FACTION_ALLIANCE_PLAYER);
    Flag(player);
    Flag(enemy);
}

void Animus::Curriculum::EnemyPlayers::Flag(Player* player)
{
    if (player && !player->IsPvP())
        player->UpdatePvP(true, true);
}

Animus::Curriculum::EnemyPlayers::Spawned Animus::Curriculum::EnemyPlayers::Create(BotSlot& slot,
    Naming const& naming, uint8 level, CurriculumTuning::OpponentTuning const& tuning, Player* near, Map* map,
    uint32 mapId, ScriptedPlayer::State& state)
{
    // What this enemy is there for, drawn the same way a party's makeup is.
    int32 const roll = irand(0, 99);
    AptitudeDemand demand = roll < tuning.TankChance ? AptitudeDemand::HoldsThePull()
        : roll < tuning.TankChance + tuning.HealerChance ? AptitudeDemand::KeepsThemUp()
        : AptitudeDemand::Anything();
    std::vector<uint8> classes = ClassAssets::ClassesFor(level, demand);
    if (classes.empty())
    {
        demand = AptitudeDemand::Anything();
        classes = ClassAssets::ClassesFor(level, demand);
    }
    if (classes.empty())
        return {};

    uint8 const playerClass = classes[urand(0, uint32(classes.size()) - 1)];
    ClassAssets const& assets = ClassAssets::For(*ClassAssets::FindProfile(playerClass));

    slot.Begin();
    uint8 const session = slot.NextSession();

    BotFactory::BotSpec spec;
    spec.Name = naming.Name(session);
    spec.Race = assets.Races[urand(0, uint32(assets.Races.size()) - 1)];
    spec.Class = playerClass;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = level;
    spec.AccountId = naming.Account(session);

    // Out of range at a random bearing, facing a random way, like the duel's creature.
    Position start = Opponents::FindSpawnPoint(near, map);
    start.SetOrientation(frand(0.0f, 2.0f * float(M_PI)));
    Player* enemy = slot.CreateNext(spec, map, mapId, start);
    if (!enemy)
        return {};

    enemy->InitTalentForLevel();
    ScriptedPlayer::Configure(enemy, assets, demand, state, true);
    slot.Promote();
    return { enemy, playerClass, state.Apt };
}
