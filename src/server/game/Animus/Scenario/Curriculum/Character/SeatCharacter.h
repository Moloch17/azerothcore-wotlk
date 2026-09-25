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

#ifndef ANIMUS_LIB_CURRICULUM_SEAT_CHARACTER_H
#define ANIMUS_LIB_CURRICULUM_SEAT_CHARACTER_H

#include "ClassProfile.h"
#include "Aptitude.h"
#include "TalentBuilder.h"
#include <vector>

class Player;

/*
 * A learned character as a stage's seats are built, for everyone who plays a class model: the forge's seats and
 * mod-animus's companions. What a model saw in training (talents, trainer spells, gear, a stance) is what it must get
 * in play.
 */
namespace Animus::Curriculum
{
    struct Layout;

    namespace SeatCharacter
    {
        /// How a character's talent points are spent (CurriculumTuning::CharacterTuning). Training draws one per
        /// character, so the policy sees builds it has to read rather than one build per spec.
        enum class TalentPlan : uint8
        {
            Standard,       // the spec's standard build (SpecBuilds)
            Noisy,          // ... with its last points spent at random
            Random,         // every point at random, inside the spec's tree first
        };

        struct Built
        {
            TalentBuilder::Build Build;
            TalentPlan Plan = TalentPlan::Standard;
            uint32 UnspentTalentPoints = 0;
            uint32 EquippedItems = 0;
        };

        /// Proficiencies, spec `spec`'s talents (`plan`, moving `noisePoints` for TalentPlan::Noisy) and glyphs, the
        /// class's trainer spells and gear (resilience gear with `pvp`), then full health and mana, full energy and no
        /// rage or runic power. The bot's talent points must be right for its map (InitTalentForLevel after placing
        /// it).
        Built Configure(Player* bot, Layout const& layout, uint8 spec, bool pvp,
            TalentPlan plan = TalentPlan::Standard, uint32 noisePoints = 0);

        /// Get a character ready to fight something that fights back: no XP (levelling up would change the character
        /// under the model) and a warrior's stance (nothing works without one, and only a first login casts it).
        /// Returns a hunter's stable offer (STABLE_SLOTS beasts), empty for other classes.
        std::vector<uint32> PrepareFighter(Player* bot, Layout const& layout, Aptitude const& aptitude);

        /// Put the character's pet out as a returning player has it, without spending a cast: a hunter one of the
        /// `stable` beasts, a warlock a demon it knows, a death knight with Master of Ghouls its ghoul, a frost mage with
        /// Glyph of Eternal Water its elemental. Summons that leave on their own are not given. True if a pet is out.
        bool GivePet(Player* bot, std::vector<uint32> const& stable);
    }
}

#endif
