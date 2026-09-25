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

#ifndef ANIMUS_LIB_CURRICULUM_PET_TALENTS_H
#define ANIMUS_LIB_CURRICULUM_PET_TALENTS_H

#include "Define.h"

class Pet;
class Player;

/*
 * A hunter pet's talents, spent as a player spends them: a standard build per talent tree (ferocity, tenacity,
 * cunning), point by point in the order players take the talents, through Player::LearnPetTalent, which checks every
 * rule (3 points per row, prerequisites, the tree matching the pet's family). Points the list cannot place go to a
 * random talent that can take one, deeper rows first.
 *
 * Family-specific talents (Dash or Dive, Charge or Swoop, and the Mobility that goes with them) are never spent: the
 * core does not know which families may take them, so a wolf could learn a bat's Dive.
 */
namespace Animus::Curriculum::PetTalents
{
    /// Spend `pet`'s free talent points; `owner` is the hunter whose pet it is. Returns the points left unspent.
    uint32 Spend(Player* owner, Pet* pet);
}

#endif
