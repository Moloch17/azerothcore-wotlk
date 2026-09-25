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

#ifndef ANIMUS_LIB_CURRICULUM_COMBAT_REWARD_H
#define ANIMUS_LIB_CURRICULUM_COMBAT_REWARD_H

#include "CurriculumTuning.h"
#include "Define.h"
#include <algorithm>

class Player;
class Unit;

namespace Animus
{
    struct AgentStats;
    struct Env;
}

namespace Animus::Curriculum
{
    class StageScenario;
    class RewardLedger;
    struct CombatTally;
    struct SeatState;

    /// Reward pieces every fight against something that fights back shares (duel, pulls, PvP).
    namespace CombatReward
    {
        /// The range the approach shaping aims for: the spec's melee or ranged range.
        [[nodiscard]] float DesiredRange(SeatState const& seat, CurriculumTuning::DuelTuning const& duel);

        /// The step's cast counts into the tally, and the casting term: cast time wasted on casts cut short, and cast
        /// time of casts that finished in combat.
        void Casting(Player* bot, AgentStats const& step, CombatTally& tally,
            CurriculumTuning::CastingTuning const& tuning, RewardLedger& ledger);

        /// Potential-based shaping on the distance still to close to `desiredRange` from `target`: it pays for getting
        /// there and takes it back for leaving, so it cannot be farmed. A null target restarts it.
        void Approach(Player* bot, Unit* target, float desiredRange, float weight, CombatTally& tally,
            RewardLedger& ledger);

        /// Harmful spells cast from stealth since the last reward: `opener` for each that broke stealth, `utility` for
        /// each new target of one that kept it.
        void Stealth(CombatTally& tally, float opener, float utility, RewardLedger& ledger);

        /// The share of `unit` still standing, 1 for a unit that is gone or cannot be read: what a fight left undone
        /// is charged in (the timeout).
        [[nodiscard]] float HealthLeft(Unit const* unit);

        /// The share of a timeout charge to take when the fight ended with `healthLeft` of its enemy standing:
        /// `floor` of it always, the rest with the work left undone.
        [[nodiscard]] float TimeoutScale(float floor, float healthLeft);

        /// The factor a fight's outcome terms carry for its difficulty tier: 1 + step x tier (Difficulty.TierScale).
        /// A win is multiplied by it and a loss divided by it, so the score stays comparable across the ladder.
        [[nodiscard]] inline float TierScale(float step, uint32 tier)
        {
            return 1.0f + std::max(0.0f, step) * float(tier);
        }

        /// How the seat is fighting `target`, measured only: time in the fight (FightMs, which every style share is
        /// divided by), time inside melee reach, time the target spent on the bot's pet, and the roots and snares the
        /// bot or its pet and totems hold it with. The caller decides when a fight is on; a null target still counts
        /// the time. Counted for whatever the seat is fighting, so a pack's target counts as a duel's opponent does.
        void Style(Player const* bot, Unit const* target, uint32 decisionMs, CombatTally& tally);

        /// The fraction of the episode length not yet spent since `sinceMs`. The fast kill and clear bonuses count
        /// from the engagement, so the approach, stealth and preparation before it cost nothing but the discount.
        [[nodiscard]] float TimeLeftSince(Env const& env, uint32 sinceMs);
    }
}

#endif
