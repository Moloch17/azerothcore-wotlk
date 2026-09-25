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

#include "CurriculumTuning.h"
#include "Config.h"
#include "Log.h"
#include <algorithm>
#include <boost/json/object.hpp>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <type_traits>

namespace
{
    /// A percent chance: clamped to [0, 100].
    void ClampPercent(std::string const& prefix, char const* key, int32& chance)
    {
        int32 const clamped = std::clamp(chance, 0, 100);
        if (clamped != chance)
            LOG_WARN("module.animus", "{}{} = {} is not a percentage; using {}", prefix, key, chance, clamped);
        chance = clamped;
    }

    /// Two role chances drawn from one roll (the rest are damage dealers): scaled down when they add up past 100.
    void ClampRolePair(std::string const& prefix, char const* tankKey, int32& tank, char const* healerKey,
        int32& healer)
    {
        ClampPercent(prefix, tankKey, tank);
        ClampPercent(prefix, healerKey, healer);
        if (tank + healer <= 100)
            return;

        LOG_WARN("module.animus", "{}{} + {}{} = {} is over 100; scaling both down", prefix, tankKey, prefix,
            healerKey, tank + healer);
        int32 const total = tank + healer;
        tank = tank * 100 / total;
        healer = 100 - tank;
    }
}

Animus::Curriculum::CurriculumTuning Animus::Curriculum::CurriculumTuning::Load(std::string const& prefix)
{
    CurriculumTuning tuning;
    Visit(tuning, [&prefix](char const* key, auto& value)
    {
        // Every key is optional: a missing one keeps its default without a "missing property" warning.
        auto const loaded = sConfigMgr->GetOption(prefix + key, value, false);
        if (std::is_floating_point_v<std::decay_t<decltype(value)>> && !std::isfinite(loaded))
        {
            LOG_WARN("module.animus", "{}{} is not a finite number; using {}", prefix, key, value);
            return;
        }

        value = loaded;
    });

    ClampRolePair(prefix, "Characters.HighLevelChance", tuning.Characters.HighLevelChance,
        "Characters.LowLevelChance", tuning.Characters.LowLevelChance);
    ClampPercent(prefix, "Characters.PetOutChance", tuning.Characters.PetOutChance);
    ClampPercent(prefix, "Difficulty.ReviewChance", tuning.Difficulty.ReviewChance);
    tuning.Difficulty.Window = std::max<uint32>(1, tuning.Difficulty.Window);
    ClampRolePair(prefix, "Characters.NoisyTalentChance", tuning.Characters.NoisyTalentChance,
        "Characters.RandomTalentChance", tuning.Characters.RandomTalentChance);
    ClampPercent(prefix, "Party.ClassicChance", tuning.Party.ClassicChance);
    ClampRolePair(prefix, "Party.RoleTankChance", tuning.Party.RoleTankChance, "Party.RoleHealerChance",
        tuning.Party.RoleHealerChance);
    ClampPercent(prefix, "Pulls.LinkedChance", tuning.Pulls.LinkedChance);
    ClampPercent(prefix, "Pulls.EliteChance", tuning.Pulls.EliteChance);
    ClampPercent(prefix, "Pulls.HigherLevelChance", tuning.Pulls.HigherLevelChance);
    ClampPercent(prefix, "Pulls.PartyEliteChance", tuning.Pulls.PartyEliteChance);
    ClampPercent(prefix, "Pulls.OwnerPullsChance", tuning.Pulls.OwnerPullsChance);
    ClampPercent(prefix, "ScriptedPlayers.StealthChance", tuning.ScriptedPlayers.StealthChance);
    ClampPercent(prefix, "ScriptedPlayers.RunChance", tuning.ScriptedPlayers.RunChance);
    ClampPercent(prefix, "ScriptedPlayers.TacticsChance", tuning.ScriptedPlayers.TacticsChance);
    ClampRolePair(prefix, "Owner.TankChance", tuning.Owner.TankChance, "Owner.HealerChance", tuning.Owner.HealerChance);
    ClampRolePair(prefix, "Opponent.TankChance", tuning.Opponent.TankChance, "Opponent.HealerChance",
        tuning.Opponent.HealerChance);
    tuning.Owner.LevelSpread = std::max(0, tuning.Owner.LevelSpread);
    tuning.Opponent.LevelSpread = std::max(0, tuning.Opponent.LevelSpread);

    auto const order = [](uint32& low, uint32& high) { if (low > high) std::swap(low, high); };
    order(tuning.Pulls.NextPullMinMs, tuning.Pulls.NextPullMaxMs);
    order(tuning.Pulls.ArriveMinMs, tuning.Pulls.ArriveMaxMs);
    tuning.Pulls.GauntletSupplies = std::max<uint32>(1, tuning.Pulls.GauntletSupplies);
    order(tuning.Pulls.OwnerEngageMinMs, tuning.Pulls.OwnerEngageMaxMs);
    order(tuning.Pulls.PartyOwnerEngageMinMs, tuning.Pulls.PartyOwnerEngageMaxMs);
    order(tuning.Pulls.OwnerPullsMinMs, tuning.Pulls.OwnerPullsMaxMs);
    order(tuning.Ambush.MinMs, tuning.Ambush.MaxMs);
    order(tuning.ScriptedPlayers.SpellMinMs, tuning.ScriptedPlayers.SpellMaxMs);
    order(tuning.ScriptedPlayers.HealMinMs, tuning.ScriptedPlayers.HealMaxMs);
    order(tuning.ScriptedPlayers.WanderMinMs, tuning.ScriptedPlayers.WanderMaxMs);
    order(tuning.ScriptedPlayers.ControlMinMs, tuning.ScriptedPlayers.ControlMaxMs);

    auto const orderYards = [](float& low, float& high)
    {
        low = std::max(0.0f, low);
        high = std::max(0.0f, high);
        if (low > high)
            std::swap(low, high);
    };
    orderYards(tuning.Travel.ObjectiveMin, tuning.Travel.ObjectiveMax);
    orderYards(tuning.ScriptedPlayers.RunMinYards, tuning.ScriptedPlayers.RunMaxYards);
    orderYards(tuning.Travel.FlyingMin, tuning.Travel.FlyingMax);
    orderYards(tuning.Flag.BaseMin, tuning.Flag.BaseMax);
    tuning.Flag.CapturesToWin = std::max<uint32>(1, tuning.Flag.CapturesToWin);

    tuning.Party.SizeWeight1 = std::max(0, tuning.Party.SizeWeight1);
    tuning.Party.SizeWeight2 = std::max(0, tuning.Party.SizeWeight2);
    tuning.Party.SizeWeight3 = std::max(0, tuning.Party.SizeWeight3);
    tuning.Party.SizeWeight4 = std::max(0, tuning.Party.SizeWeight4);
    return tuning;
}

boost::json::object Animus::Curriculum::CurriculumTuning::Json() const
{
    boost::json::object json;
    Visit(*this, [&json](char const* key, auto const& value)
    {
        using Value = std::decay_t<decltype(value)>;
        if (!std::is_floating_point_v<Value>)
        {
            json[key] = value;
            return;
        }

        // The float's shortest decimal (0.03, not 0.029999999329447746), as a double.
        char buffer[32];
        auto const result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        json[key] = std::strtod(std::string(buffer, result.ptr).c_str(), nullptr);
    });

    return json;
}
