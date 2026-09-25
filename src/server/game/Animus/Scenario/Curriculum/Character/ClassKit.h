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

#ifndef ANIMUS_LIB_CURRICULUM_CLASS_KIT_H
#define ANIMUS_LIB_CURRICULUM_CLASS_KIT_H

#include "Define.h"
#include <array>
#include <vector>

class Player;

namespace Animus::Curriculum
{
    /// Everything a class learns and carries by level, outside talents and gear: the class trainers'
    /// spells, the class-quest spells trainers do not teach, and the reagents its spells consume.
    class ClassKit
    {
    public:
        struct KitSpell
        {
            uint32 SpellId = 0;                     // the spell the player ends up knowing
            uint8 ReqLevel = 0;
            std::array<uint32, 3> ReqAbility{};     // must all be known (talent-gated ranks)
        };

        struct Reagent
        {
            uint32 ItemId = 0;
            uint32 Count = 0;
            uint8 ReqLevel = 0;
        };

        /// Loads the class trainers' spells from the world database.
        explicit ClassKit(uint8 playerClass);

        [[nodiscard]] uint8 Class() const { return _class; }
        [[nodiscard]] uint8 MinLevel() const { return MinLevelOf(_class); }

        /// The lowest level a character of `playerClass` can be (death knights start at 55).
        [[nodiscard]] static uint8 MinLevelOf(uint8 playerClass);
        [[nodiscard]] std::vector<KitSpell> const& Spells() const { return _spells; }

        /// Learns every kit spell available at the bot's level whose required abilities it knows.
        /// Call after talents, so talent-gated ranks (e.g. Mortal Strike rank 2+) are picked up.
        void Learn(Player* bot) const;

        /// Puts the class reagents for the bot's level in its backpack.
        void StoreReagents(Player* bot) const;

        /// Armor subclass (ITEM_SUBCLASS_ARMOR_*) the class wears at a level.
        [[nodiscard]] uint32 ArmorSubclass(uint8 level) const;

    private:
        uint8 _class;
        std::vector<KitSpell> _spells;          // sorted by level
        std::vector<Reagent> _reagents;
    };
}

#endif
