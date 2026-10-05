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
        GoalReached,        // the goal held was reached (Goals.Reached)
        GoalSwitch,         // the goal head changed a seat's goal (Goals.Switch)
        GoalProgress,       // closing on the goal held (Goals.Progress, potential-based)
        OrderMatch,
        PlaceMatch,
        BrokeContact,
        Stalk,
        OpenerDamage,       // what a stealth opener's first seconds took off the opponent (Stealth.OpenerDamage)
        /// Room to move: charged by the second for being closer to the edge of walkable space than a seat
        /// ought to be. Shaped, never a gate -- a doorway is narrower than any margin worth keeping in the open,
        /// so a rule that forbade closeness would forbid doorways.
        Clearance,
        Repeat,
        /// Steering that does not commit (Actions.Jitter): a turn undone moments after it was chosen, feet swung
        /// round from a bearing just pressed.
        Jitter,
        /// Presses against the seat's own goal (Actions.Aimless), every press but the no-op (Actions.Effort), and
        /// moving in a fight while already at range (Actions.Fidget).
        Aimless,
        Effort,
        Fidget,
        Hazard,
        HealingMana,
        /// An instance boss fight lost with the boss part dead: the share of its health the fight took off it.
        BossProgress,
        /// Life outside the fight (the quest, gather and town stages).
        Wasted,             // a press that did nothing in the world
        QuestAccepted,
        QuestCredit,        // objective counts as they land
        QuestTurnIn,
        Poach,              // quest credit taken in a place another group holds (Life.Poach)
        GatherNode,
        GatherSkillUp,
        TownSold,
        TownRepaired,
        TownStocked,
        TownEquipped,
        TownDone,
        /// The corpse run (Death.*): time dead, dying again soon after rising, rising safely, the spirit healer.
        DeathRun,
        /// Every second an engaged enemy lives (Output.Clock): what makes killing faster pay.
        CombatClock,
        /// A ranged spec shooting from range, and its shooting stopped by moving for nothing (Duel.Shot*).
        Ranged,
        /// A pet taking the enemies' blows instead of its owner (Duel.PetTank).
        PetTank,
        /// A pull drill's pack killed alone (Instance.PullClean): paid as Kill until 2026-10-03, which put a drill's
        /// lesson in the same column as the dungeon's kills.
        PullClean,
        /// A director replacing or churning its side's orders (Director.OrderChange, Director.OrderChurn): charged to
        /// the director's own row, outside any seat's. A term since 2026-10-03, so it fades with the rest of shaping.
        OrderChurn,
        /// A party's damage dealer or healer with enemies on it while the tank has not engaged (Raid.EarlyPull): the
        /// pull opened before the tank was there to take it. A cost, not shaping: it never fades (2026-10-03).
        EarlyPull,
        /// The dummy drills' own lesson (DummyEncounter, stage3_rotation): output against the dummy's health, mana
        /// kept at the end, and time spent hurt in the bleeding drill. Paid as DamageDealt, Readiness and HealthKept
        /// until 2026-10-05, all Shaping: the shaping fade took away everything the stage is for, and stage3 stopped
        /// casting (output 5 -> 1.1 a dummy) while its score -- kills in the one drill that has any -- rose.
        DummyOutput,
        DummyMana,
        DummyHurt,
        /// A role drill's own lesson, paid to the drilled seat only (ArenaDefinition::DrillRole, seat 0; stage6): the
        /// tank holding the pack (Raid.TankHold/TankLoose), the damage dealer on the tank's target and off its
        /// enemies (Raid.TankTarget/PulledOff), the healer keeping the party up without waste (Raid.KeepUp, Overheal).
        /// Every other seat, and every other stage, is paid these as the shaping terms they were (Threat, DamageDealt,
        /// TeammateHealing): a fight is won by them, so the score already sees what they buy, but a drill is for them.
        /// Shaping until 2026-10-05, when a stage6 run faded to x0 and its drill lessons drifted (camp packs pulled
        /// clean 0.41 -> 0.14) while the score rose.
        DrillHold,
        DrillFocus,
        DrillKeep,
        /// A pull drill's second pack dragged into the fight (Instance.PullExtra): a cost. Paid as Threat (Shaping)
        /// until 2026-10-05, so a faded stage8 drill charged nothing for a double pull.
        PullExtra,
        Count
    };

    constexpr std::size_t REWARD_TERM_COUNT = std::size_t(RewardTerm::Count);

    [[nodiscard]] std::string_view RewardTermName(RewardTerm term);

    /// What a term is for (peak-play plan, W0). Outcome is what a stage is for and Cost what a best player also keeps
    /// down; together they are the episode's score (RewardLedger::Score), which is what evaluation, best.pt and the
    /// league are judged on. Everything else is Shaping: a nudge toward the outcome, which a policy should be able to
    /// do without once it has learned what the nudge pointed at.
    ///
    /// No term is potential-based shaping (F = gamma Phi' - Phi at the learner's gamma), which would leave the optimal
    /// policy alone. Instance.WingProgress pays a high-water mark and never charges a step back, and Goals.Progress
    /// discounts at a constant ProgressGamma rather than the learner's gamma and re-bases its potential when a goal is
    /// switched, so walking away and choosing again is free: both are Shaping.
    enum class RewardCategory : uint8
    {
        Outcome,
        Cost,
        Shaping,
        None
    };

    [[nodiscard]] constexpr RewardCategory RewardTermCategory(RewardTerm term)
    {
        switch (term)
        {
            // What the stages are for: the kill (and a dungeon's trash, mid-bosses and last boss), the clear, the
            // capture and return, the player killed, the arrival, the quest handed in, the node gathered, the errands
            // done. A resurrection is not here: standing an ally up is a means, and the one that was farmable earned
            // 88% of a stage's return (animus.rewards).
            case RewardTerm::Kill:
            case RewardTerm::Clear:
            case RewardTerm::PlayerKill:
            case RewardTerm::Arrive:
            case RewardTerm::FlagCapture:
            case RewardTerm::FlagReturn:
            case RewardTerm::QuestTurnIn:
            case RewardTerm::GatherNode:
            case RewardTerm::TownDone:
            case RewardTerm::DummyOutput:
            case RewardTerm::DummyMana:
            case RewardTerm::DrillHold:
            case RewardTerm::DrillFocus:
            case RewardTerm::DrillKeep:
            // A pull drill's pack killed on its own: what the drill is for (2026-10-05; Shaping since 2026-10-03).
            case RewardTerm::PullClean:
                return RewardCategory::Outcome;
            // What the outcome costs: deaths (the seat's, a teammate's, the owner's; a wipe is paid as deaths), the
            // flag lost, the clock run out, the step cost a stage charges for time, the corpse run.
            case RewardTerm::Death:
            case RewardTerm::OwnerDeath:
            case RewardTerm::TeammateDeath:
            case RewardTerm::FlagLost:
            case RewardTerm::Timeout:
            case RewardTerm::StepCost:
            case RewardTerm::DeathRun:
            case RewardTerm::EarlyPull:
            case RewardTerm::DummyHurt:
            case RewardTerm::PullExtra:
            // The prices of noise: a press that did nothing again, a turn and its reversal, a press its goal did not
            // call for, any press at all, standing in place shuffling. As shaping they faded with the rest, and by a
            // stage's end spinning, strafing and re-pressing cost nothing: stage5's casters ended at 90-190 turns and
            // 5-7 presses a cast, its melee at 60-110 strafes, and the next stage was seeded from that (2026-10-04).
            case RewardTerm::Repeat:
            case RewardTerm::Jitter:
            case RewardTerm::Aimless:
            case RewardTerm::Effort:
            case RewardTerm::Fidget:
                return RewardCategory::Cost;
            case RewardTerm::DamageDealt:
            case RewardTerm::DamageTaken:
            case RewardTerm::Casting:
            case RewardTerm::Approach:
            case RewardTerm::StealthOpener:
            case RewardTerm::StealthUtility:
            case RewardTerm::Interrupt:
            case RewardTerm::HealthKept:
            case RewardTerm::OwnerDamageTaken:
            case RewardTerm::OwnerHealing:
            case RewardTerm::TankDamageRefund:
            case RewardTerm::Threat:
            case RewardTerm::SoloFight:
            case RewardTerm::Follow:
            case RewardTerm::TeammateDamageTaken:
            case RewardTerm::TeammateHealing:
            case RewardTerm::TeammateThreat:
            case RewardTerm::Revive:
            case RewardTerm::Progress:
            case RewardTerm::FlagPickup:
            case RewardTerm::CarrierKill:
            case RewardTerm::Stall:
            case RewardTerm::Spacing:
            case RewardTerm::Readiness:
            case RewardTerm::Control:
            case RewardTerm::SelfHealing:
            case RewardTerm::GoalReached:
            case RewardTerm::GoalSwitch:
            case RewardTerm::GoalProgress:
            case RewardTerm::OrderMatch:
            case RewardTerm::PlaceMatch:
            case RewardTerm::BrokeContact:
            case RewardTerm::Stalk:
            case RewardTerm::OpenerDamage:
            case RewardTerm::Clearance:
            case RewardTerm::Hazard:
            case RewardTerm::HealingMana:
            case RewardTerm::BossProgress:
            case RewardTerm::Wasted:
            case RewardTerm::QuestAccepted:
            case RewardTerm::QuestCredit:
            case RewardTerm::Poach:
            case RewardTerm::GatherSkillUp:
            case RewardTerm::TownSold:
            case RewardTerm::TownRepaired:
            case RewardTerm::TownStocked:
            case RewardTerm::TownEquipped:
            case RewardTerm::CombatClock:
            case RewardTerm::Ranged:
            case RewardTerm::PetTank:
            case RewardTerm::OrderChurn:
                return RewardCategory::Shaping;
            case RewardTerm::Count:
                break;
        }

        return RewardCategory::None;
    }

    [[nodiscard]] constexpr bool EveryRewardTermCategorised()
    {
        for (std::size_t term = 0; term < REWARD_TERM_COUNT; ++term)
            if (RewardTermCategory(RewardTerm(term)) == RewardCategory::None)
                return false;
        return true;
    }

    static_assert(EveryRewardTermCategorised(), "every RewardTerm needs a RewardCategory");

    [[nodiscard]] constexpr bool ScoresOutcome(RewardTerm term)
    {
        RewardCategory const category = RewardTermCategory(term);
        return category == RewardCategory::Outcome || category == RewardCategory::Cost;
    }

    /// The Cost terms that price noise rather than the outcome: a repeated press, a turn and its reversal, a press its
    /// goal did not call for, any press at all, shuffling in place. The learner's cost ladder pays these times its
    /// rung (RewardLedger::SetCosts); deaths, the clock and the step cost are always paid in full.
    [[nodiscard]] constexpr bool PricesNoise(RewardTerm term)
    {
        switch (term)
        {
            case RewardTerm::Repeat:
            case RewardTerm::Jitter:
            case RewardTerm::Aimless:
            case RewardTerm::Effort:
            case RewardTerm::Fidget:
                return true;
            default:
                return false;
        }
    }

    /// One seat's reward: this decision's total, every term's sum over the episode, and the episode's score.
    class RewardLedger
    {
    public:
        /// `tier` is the factor a ladder stage's rung puts on the term (CombatReward::TierScale: a win times it, a
        /// loss divided by it), passed apart from `value` so the score can leave it out. The reward is the same
        /// either way; the score is not, and a score that moved whenever a rung stepped would read a harder rung as
        /// learning and an easier one as collapse. Returns what was paid, which a Shaping term has had the stage's
        /// shaping scale put on (SetShaping), and a noise price the cost scale (SetCosts): what mirrors a payment
        /// elsewhere has to mirror this, not `value`. The score takes `value` as tuned, at full price.
        float Add(RewardTerm term, float value, float tier = 1.0f)
        {
            if (ScoresOutcome(term))
                _score += value;
            value *= _scale[std::size_t(term)] * tier * Shaped(term);
            _step += value;
            _episode[std::size_t(term)] += value;
            return value;
        }
        /// What this seat is paid of a term, whichever encounter adds it: 0 for a party healer's damage, a share
        /// for a party tank's. 1 unless set.
        void Scale(RewardTerm term, float factor) { _scale[std::size_t(term)] = factor; }

        /// A term paid into a decision whose total has already been taken (StageScenario pays a goal reached, seen
        /// at the observation, into the reward row of the decision that reached it): the episode's sums only.
        /// Returns what was paid, shaping scale and all, for the caller to put in that row.
        float AddTaken(RewardTerm term, float value)
        {
            if (ScoresOutcome(term))
                _score += value;
            value *= Shaped(term);
            _episode[std::size_t(term)] += value;
            return value;
        }

        /// The stage's shaping scale (the learner's fade ladder, peak-play plan W1): every Shaping term is paid
        /// times it, Outcome and Cost terms never. 1 until the learner says otherwise; 0 is the outcome alone.
        void SetShaping(float scale) { _shaping = scale; }

        /// The stage's cost scale (the learner's cost ladder): every noise price (PricesNoise) is paid times it, so a
        /// fresh policy can find the outcome before it is charged in full for the noise of looking. The score is
        /// always at full price. 1 until the learner says otherwise.
        void SetCosts(float scale) { _costs = scale; }

        /// Start a decision; returns the previous decision's total.
        float TakeStep()
        {
            float const step = _step;
            _step = 0.0f;
            return step;
        }

        [[nodiscard]] float Episode(RewardTerm term) const { return _episode[std::size_t(term)]; }

        /// The episode's Outcome and Cost terms as tuned, before any rung's tier and any role's scale: the same
        /// kill is worth the same at every rung and in every seat, so the score compares across them.
        [[nodiscard]] float Score() const { return _score; }

        void ResetEpisode()
        {
            _episode.fill(0.0f);
            _step = 0.0f;
            _score = 0.0f;
        }

    private:
        std::array<float, REWARD_TERM_COUNT> _episode{};
        std::array<float, REWARD_TERM_COUNT> _scale = MakeOnes();
        static std::array<float, REWARD_TERM_COUNT> MakeOnes()
        {
            std::array<float, REWARD_TERM_COUNT> ones;
            ones.fill(1.0f);
            return ones;
        }
        [[nodiscard]] float Shaped(RewardTerm term) const
        {
            if (RewardTermCategory(term) == RewardCategory::Shaping)
                return _shaping;
            return PricesNoise(term) ? _costs : 1.0f;
        }

        float _step = 0.0f;
        float _score = 0.0f;
        float _shaping = 1.0f;
        float _costs = 1.0f;
    };
}

#endif
