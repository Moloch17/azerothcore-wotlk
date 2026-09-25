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

#include "HostilesBlock.h"
#include "ClassProfile.h"
#include "Player.h"
#include "SeatView.h"
#include "Spell.h"
#include "SpellInfo.h"
#include <boost/json/object.hpp>

Animus::Curriculum::BlockSize Animus::Curriculum::HostilesBlock::Size(Layout const& /*layout*/) const
{
    return { PACK_SLOTS * SLOT_FEATURES, 0 };
}

void Animus::Curriculum::HostilesBlock::DescribeManifest(Layout const& /*layout*/,
    boost::json::object& block) const
{
    block["slots"] = PACK_SLOTS;
    block["slot_features"] = uint32(SLOT_FEATURES);
}

void Animus::Curriculum::HostilesBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    for (uint32 slot = 0; slot < view.EnemyCount && slot < PACK_SLOTS; ++slot)
    {
        Unit* enemy = view.Enemies[slot];
        if (!enemy)
            continue;

        float* features = obs + slot * SLOT_FEATURES;
        features[SLOT_STEALTHED] = enemy->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1.0f : 0.0f;

        if (Spell const* cast = enemy->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (cast->m_spellInfo->HasEffect(SPELL_EFFECT_HEAL) || cast->m_spellInfo->HasAura(SPELL_AURA_PERIODIC_HEAL))
                features[SLOT_HEALING] = 1.0f;

        Player* player = enemy->ToPlayer();
        if (!player)
            continue;

        features[SLOT_PLAYER] = 1.0f;
        WriteOneHot(PLAYABLE_CLASSES, player->getClass(), features + SLOT_CLASS_FIRST);
        features[SLOT_PET_OUT] = player->GetPetGUID() || !player->m_Controlled.empty() ? 1.0f : 0.0f;
    }
}
