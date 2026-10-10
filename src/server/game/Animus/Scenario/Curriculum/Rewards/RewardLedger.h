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
#include <algorithm>
#include <array>
#include <string_view>

namespace Animus::Curriculum
{
    /// What a seat is paid for. Every term's episode sum is reported as the episode info column reward_<name>, so
    /// the curves show what the policy is actually rewarded for.
    enum class RewardTerm : uint8
    {
        DamageDealt,
        StepCost,
        Approach,
        Kill,
        Clear,
        Death,
        Threat,
        TeammateDamageTaken,
        TeammateHealing,
        TeammateThreat,
        TeammateDeath,
        Revive,
        Progress,
        Arrive,
        Timeout,
        Stall,
        SelfHealing,
        GoalReached,        // the goal held was reached (Goals.Reached)
        GoalSwitch,         // the goal head changed a seat's goal (Goals.Switch)
        GoalProgress,       // closing on the goal held (Goals.Progress, potential-based)
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
        /// Every second an engaged enemy lives (Output.Clock): what makes killing faster pay.
        CombatClock,
        /// A pull drill's pack killed alone (Roles.PullClean, the roles stage's pull drill): paid as Kill until
        /// 2026-10-03, which put a drill's lesson in the same column as the dungeon's kills.
        PullClean,
        /// A party's damage dealer or healer with enemies on it while the tank has not engaged (Raid.EarlyPull): the
        /// pull opened before the tank was there to take it. A cost, not shaping: it never fades (2026-10-03).
        EarlyPull,
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
        /// A pull drill's second pack dragged into the fight (Roles.PullExtra): a cost. Paid as Threat (Shaping)
        /// until 2026-10-05, so a faded stage8 drill charged nothing for a double pull.
        PullExtra,
        /// Turning toward the marker (SightEncounter, M1): potential shaping on the cosine of the marker's bearing from
        /// the seat's facing, so turning to face it pays and turning away gives it back. A nudge for the first minutes
        /// of M1 (Markers.Facing), Shaping so the fade takes it away.
        Facing,
        /// Moving nowhere (the movement stages): a movement key held and the unit's position still for long enough,
        /// charged by the second after that (Controls.Stuck). A noise price: free until the stage's measure is
        /// learned, on the cost ladder after.
        Stuck,
        /// Pressing into a wall: a movement key held while the controller's step was blocked (BodyState::AgainstWall),
        /// charged by the second (Controls.Wall). A noise price, as Stuck.
        Wall,
        /// The party follow (M4, PartyFollowEncounter): per second within the band of the leader (Outcome) -- what the
        /// stage is for; per second past PartyFollow.LostYards (Cost).
        FollowKept,
        Lost,
        /// The seek stage (M2, SeekEncounter): the first frame the camera shows the hidden object (Seek.Sighting), and
        /// each new cell of floor walked onto before it is found (Seek.NewGround). Training-only aids (the plan's
        /// "fading bonuses for the first sighting and for new ground"): Shaping, so the fade takes both away and the
        /// stage's objective -- Arrive, stopped beside the object -- is never paid as either.
        Sighting,
        NewGround,
        /// The seek stage's third aid (REDESIGN §2, "looked into a room"): the first frame this episode whose cast rays
        /// show a room's floor (Seek.RoomSeen), by the episode's own bookkeeping, never the remembered map's
        /// (amendment 6). Shaping, as the other two.
        RoomSeen,
        /// The interact stage (M3, InteractEncounter): its switch rung's door opened, by the seat's own press on the
        /// lever that opens it (Interact.DoorOpened, Outcome: half of what the rung is for), and a decoy taken for
        /// the object the goal names -- stopped beside or pressed, each decoy once (Interact.WrongObject, Cost).
        DoorOpened,
        WrongObject,
        /// The party follow (M4, PartyFollowEncounter): back in the band of the leader after it stopped, sooner paying
        /// more (Outcome: the stops are the leader's, so it cannot be farmed); standing in a moving leader's way
        /// (Cost).
        Regroup,
        Blocking,
        /// The combat stages (CombatEncounter, dungeon-curriculum C1-C3): what each is for, paid as Outcome so the fade
        /// never takes it -- an episode ended with no death (Combat.Survived), an interrupt that stopped a cast
        /// (Combat.InterruptLanded) -- and what it costs, at a fixed price from the first step: the seat's health taken
        /// (Combat.Hurt, small), what ground fire took (Combat.FireHurt), and every second dead or away from the fight
        /// (Combat.Away: dead, walking back from the entrance, or beyond Combat.AwayYards of the pull while it fights).
        /// Walking back is never paid (a reward for it would pay dying). DamageTaken and Hazard stay Shaping.
        Survived,
        InterruptLanded,
        Away,
        Hurt,
        FireHurt,
        /// The party stages (InstanceEncounter, dungeon-curriculum D2, D3): a pull started with every living member
        /// ready -- at Instance.WingReadyShare of its health and mana -- paid to every seat (Instance.WingEngage,
        /// Outcome: the rest discipline; at most once a run); and every second of a dungeon run with nothing killed and
        /// nothing fighting the party, past the grace (Instance.WingStall, Cost: standing about, which was Stall's
        /// Shaping and faded).
        ReadyPull,
        Idle,
        /// The seek stage's room goals (M2 goals plan, 2026-10-09): a held room goal's room checked after the goal was
        /// chosen (Seek.RoomGoal) and a room goal given up for another (Seek.RoomSwitch, a charge): Aid, paid times the
        /// aid scale (RewardLedger::SetAid). And a return to a room already visited after a real absence (Seek.Return,
        /// Cost at a fixed price).
        RoomGoal,
        RoomSwitch,
        Return,
        /// The seek stage's cell goals (free choice goals, 2026-10-09; Seek.GoalSource 1): a cell goal reached
        /// (CellGoal, once) and a yard of new best closeness to a held one (CellProgress), only for a goal chosen far
        /// enough away on a block the seat had not stood on: Aid, as RoomGoal. And what wasted plans cost, at fixed
        /// prices (Cost): a cell goal given up for another (CellSwitch), one lost for want of headway or an invalid
        /// choice (CellLost), and a block chosen that the seat had stood on (CellStale).
        CellGoal,
        CellProgress,
        CellSwitch,
        CellLost,
        CellStale,
        /// The seek stage's exploration (explore-unstuck, 2026-10-10): each 2-yd cell of floor a ray of the seat's
        /// camera lands on for the first time this episode (Seek.ExploreSeen, rooms not yet entered times
        /// Seek.ExploreRoomBonus, at most Seek.ExploreCap an episode) and each yard closed on the nearest frontier of
        /// its own map (Seek.FrontierPull, a best-distance ratchet per frontier cluster, at most Seek.FrontierCap).
        /// Category Exploring: paid at max(the stage's shaping scale, Seek.ExploreFloor), so the fade never takes
        /// all of it (decision 0023). A Cost besides: standing in circles (Seek.Circling, fixed price) -- and an Aid:
        /// leaving the trap pose a drill starts an episode in (Seek.Escape, once).
        Explore,
        FrontierPull,
        Circling,
        Escape,
        /// Exploration v2 (decision 0025): the first entry of each table room an episode (Seek.RoomEntry, back rooms
        /// times Seek.RoomEntryBackMult, at most Seek.RoomEntryCap), category Exploring as Explore; and the stale
        /// cost, a Cost at a fixed price per second with no new room of the table entered for Seek.StaleRoomMs
        /// (Seek.Stale).
        RoomEntry,
        Stale,
        /// General search (decision 0027, map-derived terms from the seat's own map, no room table): a frontier
        /// cluster of the crop approached and resolved by positive evidence (Seek.FrontierClear, Exploring, in
        /// FrontierPull's cap) and a pocket -- a chamber behind a narrowing, seen as a separate one before -- entered
        /// for the first time (Seek.PocketEntry, Exploring, in RoomEntry's cap). Revisit (Cost, fixed price, in the
        /// score): each second stood on a 2-yd cell of ground the seat stood on earlier this episode, weighed by how
        /// long ago (Seek.Revisit, RevisitAgeMs). Recovered (Aid, fades at Seek.AidUntil, never in the score): a share
        /// of what a pin of Stuck and Wall charges cost, paid back once when the seat gets Seek.RecoverYards from the
        /// pin point within Seek.RecoverMs (movement pacing M5): a rebate, never a bonus, so a pin never pays.
        FrontierClear,
        PocketEntry,
        Revisit,
        Recovered,
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
    /// policy alone. Goals.Progress discounts at a constant ProgressGamma rather than the learner's gamma and re-bases
    /// its potential when a goal is switched, so walking away and choosing again is free: it is Shaping.
    enum class RewardCategory : uint8
    {
        Outcome,
        Cost,
        Shaping,
        /// A teaching aid that fades with the stage's progress rather than with its rung (the aid scale,
        /// RewardLedger::SetAid), so it survives a stage that plays a single rung at shaping 0. Not in the score. The
        /// lesson it points at is still Outcome's: when the aid is gone, only the Outcome is left to hold it.
        Aid,
        /// Exploration of unseen ground (RewardTerm::Explore, FrontierPull): shaping that the fade only takes down to
        /// a floor -- paid times max(the shaping scale, the floor set with RewardLedger::SetShapingFloor). Not in the
        /// score. The owner's exception to "a stage's purpose is paid as Outcome" (decision 0023).
        Exploring,
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
            case RewardTerm::Arrive:
            case RewardTerm::DrillHold:
            case RewardTerm::DrillFocus:
            case RewardTerm::DrillKeep:
            // The roles stage's pull drill: a pack killed on its own, what the drill is for (2026-10-05; Shaping since
            // 2026-10-03).
            case RewardTerm::PullClean:
            // The follow stage's band kept (2026-10-05).
            case RewardTerm::FollowKept:
            // The interact stage's door opened by its lever (M3).
            case RewardTerm::DoorOpened:
            // The party follow's regroup at the leader's stops (2026-10-06).
            case RewardTerm::Regroup:
            // The combat stages' own (2026-10-06): surviving, interrupts landed.
            case RewardTerm::Survived:
            case RewardTerm::InterruptLanded:
            // The party stages' rest discipline: a pull started with the party ready (2026-10-07).
            case RewardTerm::ReadyPull:
                return RewardCategory::Outcome;
            // What the outcome costs: deaths (the seat's, a teammate's, the owner's; a wipe is paid as deaths), the
            // flag lost, the clock run out, the step cost a stage charges for time, the corpse run.
            case RewardTerm::Death:
            case RewardTerm::TeammateDeath:
            case RewardTerm::Timeout:
            case RewardTerm::StepCost:
            case RewardTerm::EarlyPull:
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
            // Moving nowhere and pressing into walls (the movement stages, 2026-10-05).
            case RewardTerm::Stuck:
            case RewardTerm::Wall:
            case RewardTerm::Lost:
            // A decoy taken for the named object (M3).
            case RewardTerm::WrongObject:
            case RewardTerm::Blocking:
            // The combat stages' prices: time dead or away from the fight, health taken, what ground fire took.
            case RewardTerm::Away:
            case RewardTerm::Hurt:
            case RewardTerm::FireHurt:
            // A dungeon run standing about (2026-10-07).
            case RewardTerm::Idle:
            // A return to a room already visited, after a real absence (M2 goals).
            case RewardTerm::Return:
            // A cell goal given up, lost, or chosen on ground already walked (free choice goals).
            case RewardTerm::CellSwitch:
            case RewardTerm::CellLost:
            case RewardTerm::CellStale:
            // Standing in circles: moving about without getting anywhere (M2 explore-unstuck).
            case RewardTerm::Circling:
            // ... and ground gone stale: no newly seen floor for a while (M2 exploration v2).
            case RewardTerm::Stale:
            // ... and ground stood on again (general search).
            case RewardTerm::Revisit:
                return RewardCategory::Cost;
            // The seek stage's room goals: a teaching aid on the progress-linear aid scale.
            case RewardTerm::RoomGoal:
            case RewardTerm::RoomSwitch:
            case RewardTerm::CellGoal:
            case RewardTerm::CellProgress:
            // The trap drill's way out (M2 explore-unstuck): once, on the aid scale.
            case RewardTerm::Escape:
            // The pin rebate (movement pacing M5): on the aid scale, outside the score.
            case RewardTerm::Recovered:
                return RewardCategory::Aid;
            case RewardTerm::Explore:
            case RewardTerm::FrontierPull:
            case RewardTerm::RoomEntry:
            case RewardTerm::FrontierClear:
            case RewardTerm::PocketEntry:
                return RewardCategory::Exploring;
            case RewardTerm::DamageDealt:
            case RewardTerm::Approach:
            case RewardTerm::Threat:
            case RewardTerm::TeammateDamageTaken:
            case RewardTerm::TeammateHealing:
            case RewardTerm::TeammateThreat:
            case RewardTerm::Revive:
            case RewardTerm::Progress:
            case RewardTerm::Stall:
            case RewardTerm::SelfHealing:
            case RewardTerm::GoalReached:
            case RewardTerm::GoalSwitch:
            case RewardTerm::GoalProgress:
            case RewardTerm::Hazard:
            case RewardTerm::HealingMana:
            case RewardTerm::CombatClock:
            case RewardTerm::Facing:
            case RewardTerm::Sighting:
            case RewardTerm::NewGround:
            case RewardTerm::RoomSeen:
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
    /// goal did not call for, any press at all, shuffling in place, moving nowhere and pressing into a wall. The
    /// learner's cost ladder pays these times its rung (RewardLedger::SetCosts); deaths, the clock and the step cost
    /// are always paid in full.
    [[nodiscard]] constexpr bool PricesNoise(RewardTerm term)
    {
        switch (term)
        {
            case RewardTerm::Repeat:
            case RewardTerm::Jitter:
            case RewardTerm::Aimless:
            case RewardTerm::Effort:
            case RewardTerm::Fidget:
            case RewardTerm::Stuck:
            case RewardTerm::Wall:
            // ... going round in circles (Seek.Circling), ground gone stale (Seek.Stale) and ground stood on again
            // (Seek.Revisit).
            case RewardTerm::Circling:
            case RewardTerm::Stale:
            case RewardTerm::Revisit:
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
        /// A noise price paid at its own fixed price, off the cost ladder (M1 controls' Wall and Stuck,
        /// perception-goals REDESIGN §1: on from the first step, never free): as Add, but the cost scale (SetCosts) is
        /// not put on it. Any other term is paid exactly as Add pays it.
        float AddFixed(RewardTerm term, float value)
        {
            if (ScoresOutcome(term))
                _score += value;
            value *= _scale[std::size_t(term)] * (PricesNoise(term) ? 1.0f : Shaped(term));
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

        /// The least the Exploring category (Explore, FrontierPull) is paid at, whatever the shaping scale: it is paid
        /// times max(the shaping scale, this). 0 unless the encounter says otherwise (Seek.ExploreFloor).
        void SetShapingFloor(float floor) { _floor = floor; }

        /// The stage's aid scale: every Aid term is paid times it. It follows the stage's progress, not its rung
        /// (StageScenario::Reward: max(0, 1 - progress / Seek.AidUntil)), and is 1 until the scenario says otherwise.
        void SetAid(float scale) { _aid = scale; }

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
            RewardCategory const category = RewardTermCategory(term);
            if (category == RewardCategory::Shaping)
                return _shaping;
            if (category == RewardCategory::Aid)
                return _aid;
            if (category == RewardCategory::Exploring)
                return std::max(_shaping, _floor);
            return PricesNoise(term) ? _costs : 1.0f;
        }

        float _step = 0.0f;
        float _score = 0.0f;
        float _shaping = 1.0f;
        float _floor = 0.0f;
        float _aid = 1.0f;
        float _costs = 1.0f;
    };
}

#endif
