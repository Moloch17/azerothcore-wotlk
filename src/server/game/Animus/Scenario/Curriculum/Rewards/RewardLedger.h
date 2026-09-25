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

#ifndef ANIMUS_LIB_CURRICULUM_REWARD_LEDGER_H
#define ANIMUS_LIB_CURRICULUM_REWARD_LEDGER_H

#include "Define.h"
#include <array>
#include <string_view>

namespace Animus::Curriculum
{
    /// What a seat is paid for. Every term's episode sum is reported as the episode info column reward_<name>, so
    /// the curves show what the policy is actually rewarded for.
    enum class RewardTerm : uint8
    {
        DamageDealt,
        DamageTaken,
        StepCost,
        Casting,
        Approach,
        StealthOpener,
        StealthUtility,
        Interrupt,
        Kill,
        Clear,
        HealthKept,
        Death,
        OwnerDamageTaken,
        OwnerHealing,
        TankDamageRefund,
        Threat,
        SoloFight,
        Follow,
        OwnerDeath,
        TeammateDamageTaken,
        TeammateHealing,
        TeammateThreat,
        TeammateDeath,
        Revive,
        PlayerKill,
        Progress,
        Arrive,
        FlagCapture,
        FlagPickup,
        FlagReturn,
        CarrierKill,
        FlagLost,
        Timeout,
        Stall,
        Spacing,
        Readiness,
        Control,
        SelfHealing,
        GoalMatch,
        OrderMatch,
        PlaceMatch,
        BrokeContact,
        Stalk,
        /// Room to move: charged by the second for being closer to the edge of walkable space than a seat
        /// ought to be. Shaped, never a gate -- a doorway is narrower than any margin worth keeping in the open,
        /// so a rule that forbade closeness would forbid doorways.
        Clearance,
        Repeat,
        Hazard,
        HealingMana,
        /// An instance boss fight lost with the boss part dead: the share of its health the fight took off it.
        BossProgress,
        /// Life outside the fight (the quest, gather and town stages).
        Wasted,             // a press that did nothing in the world
        QuestAccepted,
        QuestCredit,        // objective counts as they land
        QuestTurnIn,
        GatherNode,
        GatherSkillUp,
        TownSold,
        TownRepaired,
        TownStocked,
        TownEquipped,
        TownDone,
        Count
    };

    constexpr std::size_t REWARD_TERM_COUNT = std::size_t(RewardTerm::Count);

    [[nodiscard]] std::string_view RewardTermName(RewardTerm term);

    /// One seat's reward: this decision's total, and every term's sum over the episode.
    class RewardLedger
    {
    public:
        void Add(RewardTerm term, float value)
        {
            _step += value;
            _episode[std::size_t(term)] += value;
        }

        /// Start a decision; returns the previous decision's total.
        float TakeStep()
        {
            float const step = _step;
            _step = 0.0f;
            return step;
        }

        [[nodiscard]] float Episode(RewardTerm term) const { return _episode[std::size_t(term)]; }

        void ResetEpisode()
        {
            _episode.fill(0.0f);
            _step = 0.0f;
        }

    private:
        std::array<float, REWARD_TERM_COUNT> _episode{};
        float _step = 0.0f;
    };
}

#endif
