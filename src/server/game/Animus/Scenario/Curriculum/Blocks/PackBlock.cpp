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

#include "PackBlock.h"
#include "CoreBlock.h"
#include "Creature.h"
#include "EncoderSupport.h"
#include "PetBlock.h"
#include "Layout.h"
#include "StringFormat.h"
#include <string>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include "Player.h"
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    bool IsSlotAllowed(SeatView const& view, uint32 slot)
    {
        if (slot >= view.EnemyCount || slot == view.TargetSlot || !view.Bot->IsAlive())
            return false;

        Unit const* enemy = view.Enemies[slot];
        return enemy && enemy->IsAlive();
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::PackBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_GLOBAL_COUNT + PACK_SLOTS * SLOT_FEATURES, ACTION_COUNT };
}

void Animus::Curriculum::PackBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["slots"] = PACK_SLOTS;
    block["slot_features"] = uint32(SLOT_FEATURES);
}

void Animus::Curriculum::PackBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;

    uint32 alive = 0;
    uint32 inCombat = 0;
    for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
    {
        Unit* enemy = view.Enemies[slot];
        if (!enemy)
            continue;

        float* features = obs + OBS_GLOBAL_COUNT + slot * SLOT_FEATURES;
        float const bearing = bot->GetRelativeAngle(enemy);
        Unit const* victim = enemy->GetVictim();

        features[SLOT_PRESENT] = 1.0f;
        features[SLOT_ALIVE] = enemy->IsAlive() ? 1.0f : 0.0f;
        features[SLOT_HEALTH] = enemy->GetHealthPct() / 100.0f;
        features[SLOT_DISTANCE] = std::min(1.0f, bot->GetDistance(enemy) / 60.0f);
        features[SLOT_BEARING_SIN] = std::sin(bearing);
        features[SLOT_BEARING_COS] = std::cos(bearing);
        features[SLOT_BEHIND] = enemy->isInBack(bot) ? 1.0f : 0.0f;
        features[SLOT_ATTACKS_BOT] = victim == bot ? 1.0f : 0.0f;
        features[SLOT_ATTACKS_PET] = victim && victim != bot && victim->GetOwnerGUID() == bot->GetGUID() ? 1.0f : 0.0f;
        features[SLOT_CASTING] = enemy->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
        features[SLOT_IN_COMBAT] = enemy->IsInCombat() ? 1.0f : 0.0f;
        features[SLOT_CROWD_CONTROLLED] = Encoding::IsCrowdControlled(enemy) ? 1.0f : 0.0f;
        features[SLOT_CURRENT_TARGET] = slot == view.TargetSlot ? 1.0f : 0.0f;
        features[SLOT_ELITE] = enemy->ToCreature() && enemy->ToCreature()->isElite() ? 1.0f : 0.0f;
        features[SLOT_LEVEL_DIFFERENCE] = (float(enemy->GetLevel()) - float(bot->GetLevel())) / 5.0f;
        features[SLOT_IN_LINE_OF_SIGHT] = bot->IsWithinLOSInMap(enemy) ? 1.0f : 0.0f;
        features[SLOT_THREAT_SHARE] = Encoding::ThreatShare(enemy, bot);
        IncomingSpell::Observe(enemy, bot, features + SLOT_CAST_FIRST);

        alive += enemy->IsAlive() ? 1 : 0;
        inCombat += enemy->IsAlive() && enemy->IsInCombat() ? 1 : 0;
    }

    obs[OBS_ALIVE] = float(alive) / float(PACK_SLOTS);
    obs[OBS_IN_COMBAT] = float(inCombat) / float(PACK_SLOTS);

    if (!mask)
        return;

    for (uint32 slot = 0; slot < PACK_SLOTS; ++slot)
        mask[slot] = IsSlotAllowed(view, slot) ? 1 : 0;

    // Holding an interrupt is offered to a seat that has one -- its own spell or its pet's -- while there is
    // something to interrupt and it is not already being held. Offered to everyone, it was pressed hardest by the
    // classes that could not interrupt at all (stage2_pack 2026-09-18: the druid healer pressed it 2.0 times a fight
    // with nothing to cast, and each press was a decision spent doing nothing).
    bool const canInterrupt = CoreBlock::KnowsInterrupt(view) || PetBlock::HasInterruptAbility(view);
    mask[ACTION_HOLD_INTERRUPT] = canInterrupt && view.Target && view.Target->IsAlive() && bot->IsAlive()
        && view.Option && !view.Option->Running(SeatOptionKind::HoldInterrupt, view.NowMs) ? 1 : 0;
}

void Animus::Curriculum::PackBlock::Apply(SeatView& view, uint32 local, SeatActionResult& /*result*/) const
{
    if (local == ACTION_HOLD_INTERRUPT)
    {
        if (view.Option && view.Target && view.Target->IsAlive() && view.Bot->IsAlive())
        {
            view.Option->Start(SeatOptionKind::HoldInterrupt, view.NowMs + view.Options.HoldInterruptMs);
        }
        return;
    }

    if (IsSlotAllowed(view, local))
        Encoding::SelectEnemy(view, local);
}

std::string Animus::Curriculum::PackBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    if (local == ACTION_HOLD_INTERRUPT)
        return "hold_interrupt";

    return local < PACK_SLOTS ? Acore::StringFormat("target_slot_{}", local) : std::string();
}
