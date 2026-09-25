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

#ifndef ANIMUS_COMBAT_REWARD_SCENARIO_H
#define ANIMUS_COMBAT_REWARD_SCENARIO_H

#include "Define.h"

class Player;
class Unit;

namespace Animus
{
    struct Env;
}

namespace Animus::Curriculum
{
    class StageScenario;
    class RewardLedger;

    /// The one reward term that needs a running scenario rather than the units in front of it, which is why it is
    /// here and not in CombatReward: it reads the stage's tuning and the seat's own state out of StageScenario.
    ///
    /// Declared apart so the runtime half of the library -- everything mod-animus builds on a stock AzerothCore --
    /// carries no declaration it does not define. CombatReward.h promises only what CombatReward.cpp delivers.
    namespace CombatReward
    {
        /// A seat's reward against one opponent (a creature or a player): damage dealt as a fraction of its health,
        /// damage taken, casting, approach, stealth openers, the kill (faster and healthier pays more), death. The
        /// kill and the death carry `tierScale` (TierScale): 1 outside a ladder.
        void OneOnOne(StageScenario& scenario, Env const& env, uint32 seat, Player* bot, Unit* opponent,
            RewardLedger& ledger, float tierScale = 1.0f);
    }
}

#endif
