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

#include "GauntletBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include <algorithm>
#include <array>
#include <string>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include "MoveSpline.h"
#include "Player.h"
#include "Supplies.h"

namespace
{
    using namespace Animus::Curriculum;

    /// A rest is done once health and mana are at least this full.
    constexpr float REST_UNTIL_PERCENT = 90.0f;

    /// Health and mana are back (a rest has nothing left to do).
    bool Recovered(Player const* bot)
    {
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        return bot->GetHealthPct() >= REST_UNTIL_PERCENT
            && (!maxMana || float(bot->GetPower(POWER_MANA)) * 100.0f / float(maxMana) >= REST_UNTIL_PERCENT);
    }

    bool IsAllowed(SeatView const& view, uint32 action)
    {
        Player* bot = view.Bot;
        if (action >= GauntletBlock::ACTION_COUNT)
            return false;

        if (action == GauntletBlock::ACTION_REST_UNTIL_READY)
        {
            // Offered when there is something to restore and something to restore it with, and not while it runs.
            return !view.Option->Running(SeatOptionKind::RestUntilReady, view.NowMs) && bot->IsAlive()
                && !bot->IsInCombat() && !Recovered(bot)
                && (IsAllowed(view, GauntletBlock::ACTION_EAT) || IsAllowed(view, GauntletBlock::ACTION_DRINK));
        }

        bool const eat = action == GauntletBlock::ACTION_EAT;
        uint32 const item = eat ? view.FoodItem : view.DrinkItem;

        // The item's own cast check as for potions and bandages (forms, the global cooldown, stuns): eat and drink
        // offered where the cast then failed were pressed over and over for nothing.
        return item && bot->IsAlive() && !bot->IsInCombat() && bot->movespline->Finalized()
            && !bot->HasAuraType(eat ? SPELL_AURA_MOD_REGEN : SPELL_AURA_MOD_POWER_REGEN)
            && Encoding::CanUseItemOn(bot, item, bot);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::GauntletBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_GLOBAL_COUNT, ACTION_COUNT };
}

void Animus::Curriculum::GauntletBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["consumables"] = CONSUMABLE_COUNT;
}

void Animus::Curriculum::GauntletBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;
    bool const pullActive = view.EnemyCount > 0;

    obs[OBS_PULLS_CLEARED] = std::min(1.0f, float(view.PullsCleared) / 10.0f);
    obs[OBS_PULL_ACTIVE] = pullActive ? 1.0f : 0.0f;
    obs[OBS_QUIET_TIME] = pullActive ? 0.0f : view.QuietTime;
    obs[OBS_PULL_TIME] = pullActive ? view.PullTime : 0.0f;
    obs[OBS_ELITE_PULL] = pullActive && view.ElitePull ? 1.0f : 0.0f;
    obs[OBS_EATING] = bot->HasAuraType(SPELL_AURA_MOD_REGEN) ? 1.0f : 0.0f;
    obs[OBS_DRINKING] = bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN) ? 1.0f : 0.0f;
    float const stocked = float(std::max<uint32>(1, view.GauntletSupplies));
    obs[OBS_FOOD_LEFT] = view.FoodItem ? std::min(1.0f, float(bot->GetItemCount(view.FoodItem)) / stocked) : 0.0f;
    obs[OBS_DRINK_LEFT] = view.DrinkItem ? std::min(1.0f, float(bot->GetItemCount(view.DrinkItem)) / stocked) : 0.0f;
    obs[OBS_PULL_ARRIVAL] = view.PullArrival;
    obs[OBS_NEXT_PULL] = pullActive ? 0.0f : view.NextPull;

    uint32 const actions = view.L->Slice(BlockId::Gauntlet).ActionCount;
    for (uint32 action = 0; mask && action < actions; ++action)
        mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void Animus::Curriculum::GauntletBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (!IsAllowed(view, local))
        return;

    if (local == ACTION_REST_UNTIL_READY)
    {
        view.Option->Start(SeatOptionKind::RestUntilReady, view.NowMs + view.Options.RestMaxMs);
        Rest(view, result);
        return;
    }

    Player* bot = view.Bot;
    bool const eat = local == ACTION_EAT;
    if (Encoding::UseItemOn(bot, eat ? view.FoodItem : view.DrinkItem, bot))
        ++(eat ? result.FoodUsed : result.DrinkUsed);
    else
        ++(eat ? result.FoodFailed : result.DrinkFailed);
}

void Animus::Curriculum::GauntletBlock::BeforeApply(SeatView& view, SeatActionResult& result) const
{
    if (!view.Option || !view.Option->Running(SeatOptionKind::RestUntilReady, view.NowMs))
        return;

    // The rest is over once the fight starts, the seat is dead, it is full again, or nothing is left to eat or drink.
    Player* bot = view.Bot;
    bool const usable = IsAllowed(view, ACTION_EAT) || IsAllowed(view, ACTION_DRINK)
        || bot->HasAuraType(SPELL_AURA_MOD_REGEN) || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
    if (!bot->IsAlive() || bot->IsInCombat() || Recovered(bot) || !usable)
    {
        view.Option->Stop(SeatOptionKind::RestUntilReady);
        return;
    }

    Rest(view, result);
}

void Animus::Curriculum::GauntletBlock::Rest(SeatView& view, SeatActionResult& result) const
{
    // Whichever of the two is missing and can be started now; eating and drinking run side by side.
    Player* bot = view.Bot;
    uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
    bool const fed = bot->GetHealthPct() >= REST_UNTIL_PERCENT || bot->HasAuraType(SPELL_AURA_MOD_REGEN);
    bool const watered = !maxMana || float(bot->GetPower(POWER_MANA)) * 100.0f / float(maxMana) >= REST_UNTIL_PERCENT
        || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
    if (!fed && IsAllowed(view, ACTION_EAT))
        Apply(view, ACTION_EAT, result);
    else if (!watered && IsAllowed(view, ACTION_DRINK))
        Apply(view, ACTION_DRINK, result);
}

std::string Animus::Curriculum::GauntletBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    static constexpr std::array<char const*, ACTION_COUNT> NAMES = { "eat", "drink", "rest_until_ready" };
    // back() rather than size(): the array is declared ACTION_COUNT long, so a short initialiser list value-
    // initialises the rest to null and a size check passes anyway. A null name is a crash when the manifest
    // builds a std::string from it (SupportBlock hit exactly that when the raid grew FRIEND_SLOTS).
    static_assert(NAMES.back() != nullptr, "every gauntlet action needs a name");

    return local < NAMES.size() ? NAMES[local] : std::string();
}
