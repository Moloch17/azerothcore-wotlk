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

#ifndef ANIMUS_LIB_CURRICULUM_STAGE_STATE_H
#define ANIMUS_LIB_CURRICULUM_STAGE_STATE_H

#include "Aptitude.h"
#include "Block.h"
#include "BotSlot.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "RewardLedger.h"
#include "SeatMemory.h"
#include "SeatView.h"
#include "SeatCharacter.h"
#include "Supplies.h"
#include "TalentBuilder.h"
#include <array>
#include <vector>

/*
 * The state every curriculum stage has: the seats' characters and their episode totals. What only some stages have
 * (pulls, the owner, the party group, the enemy player) is kept by the encounter that needs it.
 */
class SpellInfo;

namespace Animus::Curriculum
{
    struct Layout;

    /// A seat's fight against whatever fights back (duel, pulls, PvP): what the rewards and episode info share.
    struct CombatTally
    {
        uint64 DamageTaken = 0;
        float LastDistance = -1.0f;             // approach shaping: excess distance at the last reward; < 0 = none yet
        bool Killed = false;                    // its opponent died (pack: the pull was cleared)
        uint32 KillTimeMs = 0;
        bool Died = false;                      // died at least once
        uint32 Deaths = 0;
        bool DeathCounted = false;              // the current death has been paid for (again after standing up)
        uint32 DeathMs = 0;                     // episode time of the current death
        uint32 StealthOpeners = 0;              // harmful spells from stealth that broke it
        bool StepStealthOpener = false;         // one started since the last reward
        uint32 StealthUtilityCasts = 0;         // harmful spells from stealth that kept it, paid ones
        uint32 StepStealthUtility = 0;          // paid ones since the last reward
        std::vector<ObjectGuid> StealthUtilityTargets;  // targets already paid for during the current stealth
        bool Engaged = false;                   // the fight has started: the bot or its opponent entered combat
        uint32 EngageMs = 0;                    // episode time it started; the fast kill bonus counts from here
        bool PetSummoned = false;
        uint32 PreparationMs = 0;               // out of combat: buffs, forms, stealth and summons started (StallGrace)
        uint32 CastsCompleted = 0;
        uint32 CastsCancelled = 0;
        uint64 CastMsWasted = 0;
        uint32 CastsStopped = 0;
        uint32 CastsMoved = 0;
        uint32 CastsTargetLost = 0;
        uint32 CastsOther = 0;
        bool TimedOut = false;                  // creature duel: the clock ran out with neither side dead
        uint32 TargetEvadeMs = 0;               // creature duel: time the opponent spent evading (leashed, unreachable)
        uint32 OutOfSightMs = 0;                // creature duel: time engaged without line of sight to the opponent
        /// Hiding, for the stages that are about it. Unseen time is measured and never paid: the optimal
        /// policy for "seconds unseen" is to run to the far corner at the start and stand there, which is
        /// exactly the farmable shape animus.rewards exists to catch, and it would catch it only after a run
        /// had been spent on it. What is paid is the transition -- breaking contact -- with a cooldown.
        uint32 UnseenMs = 0;
        uint32 UnseenStreakMs = 0;      // ... without being spotted again
        uint32 LongestUnseenMs = 0;
        uint32 ContactBreaks = 0;       // seen -> unseen, however it was done
        uint32 LineOfSightBreaks = 0;   // ... by break_line_of_sight, and it worked
        uint32 ReStealths = 0;          // got back into stealth after losing it in a fight
        bool WasSeen = false;
        bool WasStealthed = false;
        bool PendingLosBreak = false;   // pressed break_line_of_sight; next decision says whether it worked
        uint32 BreakPaidMs = 0;         // cooldown on the transition nudge
        /// Stalking, for the stealth stage: closing on someone while stealthed and unseen, and staying there.
        /// Paid per decision inside StalkYards, which the evade reward deliberately is not -- the difference
        /// is that this one is bounded (StalkMax) and that holding the position it pays for is the hard part,
        /// not the trivial one. Standing stealthed inside melee range of something that is actively looking
        /// is a skill; standing unseen in the far corner of the map is not.
        uint32 StalkMs = 0;             // decisions spent stealthed, unseen and inside StalkYards
        uint32 StalkStreakMs = 0;       // ... unbroken
        uint32 LongestStalkMs = 0;
        uint32 StalkApproaches = 0;     // times it came from outside StalkYards to inside, stealthed
        float ClosestStealthedYards = 0.0f;     // nearest it got while stealthed and unseen; 0 = never stealthed
        float StalkPaid = 0.0f;         // what the stalk nudge has paid this episode, against StalkMax
        bool WasStalking = false;
        uint32 UnreachableMs = 0;               // creature duel: time the opponent had no path to its victim
        uint32 UnreachableStreakMs = 0;         // ... without a break, up to now
        uint32 OpponentTeleports = 0;           // ... times it was put back beside its victim for it
        // Style, over the time the fight was on with the bot alive (one-on-one arenas): how much of it the bot spent
        // within melee reach of its opponent, and how much the opponent spent attacking the bot's pet or guardian.
        uint32 FightMs = 0;
        uint32 InMeleeMs = 0;
        uint32 OnPetMs = 0;
        // ... and how much the opponent spent rooted or snared by the bot, its pet or its totems, and how often the
        // bot put a root or snare on it (a new one where there was none).
        uint32 RootedMs = 0;
        uint32 SnaredMs = 0;
        uint32 RootsApplied = 0;
        uint32 SnaresApplied = 0;
        bool WasRooted = false;
        bool WasSnared = false;
        // Feign death (one-on-one arenas): how often the bot feigned, and how often its opponent then went home to
        // evade (and heal to full) because nothing else held it, rather than turning on the pet.
        uint32 FeignDeaths = 0;
        uint32 FeignDeathResets = 0;
        bool WasFeigning = false;
        uint32 FeignEndMs = 0;                  // episode time the last feign ended (or now, while feigning)
        bool FeignResetCounted = false;         // the last feign's evade has been counted
    };

    /// One learned agent: its character, as built for the episode, and its episode totals.
    struct SeatState
    {
        Layout const* L = nullptr;              // null for a party seat left empty this episode
        BotSlot Bot;

        uint8 Race = 0;
        uint8 Level = 1;
        uint8 Spec = 0;
        /// What this character can actually do, read off the talents and gear it was built with (Aptitude). This is
        /// what the seat observes about itself and what everything else observes about it; there is no role.
        Aptitude Apt;
        /// What the arena's composition asked of this seat, recorded when the layout was drawn so the spec draw can
        /// honour it. A layout is a class, and a class has several builds, so the two happen apart: the class is
        /// chosen when the seats are laid out and the spec when the character is built.
        AptitudeDemand Want;
        SeatCharacter::TalentPlan TalentPlan = SeatCharacter::TalentPlan::Standard;
        TalentBuilder::Build Build;
        uint32 UnspentTalentPoints = 0;
        uint32 EquippedItems = 0;
        float DamageScale = 1.0f;
        std::vector<uint32> Stable;             // hunters: beasts offered this episode
        bool PetAtStart = false;                // the episode started with the seat's pet out
        /// Episodes this character has played: a seat keeps its character for Characters.ReuseEpisodes episodes
        /// when the next draw gives it the same class and build (StageScenario::ReuseSeat), then rebuilds.
        uint32 EpisodesPlayed = 0;

        /// The highest rank of every catalog action the bot knows, resolved once when the character is built:
        /// walking the rank chain per action per decision is most of what observing a seat costs, and the
        /// spellbook does not change inside an episode. Empty until the seat has a character.
        std::vector<SpellInfo const*> KnownRanks;

        uint32 LastPower = 0;
        float LastStepDamage = 0.0f;
        float LastStepPowerDelta = 0.0f;
        float LastStepDamageTaken = 0.0f;
        uint32 SpellCasts = 0;
        uint32 TrinketUses = 0;
        /// Leaving the ground: jumps launched and refused, drops (a landing more than MAX_STEP below), the deepest,
        /// drops made under a feather-fall aura, and the falls that followed with what they cost.
        uint32 Jumps = 0;
        uint32 JumpsRefused = 0;
        uint32 Drops = 0;
        float DropYards = 0.0f;
        uint32 FeatherFalls = 0;
        uint32 Falls = 0;
        float FallDamage = 0.0f;
        uint32 FallDeaths = 0;
        /// The durative actions the seat is running (SeatOptionSet: a positioning one and a standby), how many it
        /// started and how long any of them ran: one press that stands for many decisions of resting, holding an
        /// interrupt or keeping range.
        SeatOptionSet Option;
        uint32 OptionPresses = 0;
        uint32 OptionMs = 0;
        /// How the seat is steering, carried from decision to decision (MoveBlock). The compass point its feet are
        /// walking, how it is holding its head, which way it is turning, and how far up or down it is looking.
        ///
        /// These used to live only on SeatView, which is rebuilt every decision -- so they reset before every
        /// observation and every apply. A held bearing was therefore never re-issued (an eight-yard step, not a held
        /// key), OBS_BEARING_HELD never fired, ACTION_HALT was masked off in every decision of every episode because
        /// nothing was ever recorded as being walked, and FACE_TARGET and FACE_HEADING did nothing at all, because
        /// FaceWhile only ever saw the "leave it where it is" default. Steering has to be remembered to work.
        uint8 HeldBearing = 0xFF;
        uint8 FacingMode = 0xFF;
        /// Where the seat is looking, in its own keeping rather than the spline's (SeatView::Facing). Seeded from
        /// the bot when an episode starts, because a default of 0 would aim every seat due east.
        float Facing = 0.0f;
        /// What the ground looks like each way it could go, marched out to MARCH_MAX and reused until the seat
        /// has moved or turned enough to make it stale.
        /// Mutable because it is a cache and nothing else: observing a seat does not change it, but it does
        /// refresh what the seat has already looked at, and ViewSeat reads a const seat.
        mutable GroundProbe Probe;
        /// Where it has been (MovementTrail), a cache like the probe: the move block samples it in place.
        mutable MovementTrail Trail;
        /// Whether its legs are getting anywhere, measured for every seat in every arena
        /// (StageScenario::TrackMotion): where it was at the last observation and how far it has covered since
        /// the episode began, the marks the two rates are taken between about once a second, and the rates
        /// (SeatView::MoveRate and CloseRate -- the second toward its target; the travel encounter replaces it
        /// with the one toward the objective).
        float MotionLastX = 0.0f;
        float MotionLastY = 0.0f;
        bool MotionHasLast = false;
        float MotionTravelled = 0.0f;
        uint32 MotionMarkMs = 0;
        float MotionMarkTravelled = 0.0f;
        float MotionMarkRange = -1.0f;
        float MoveRate = 0.0f;
        float CloseRate = 0.0f;
        int8 Turning = 0;                       // +1 left, -1 right, 0 not turning (counter-clockwise is positive)
        int8 PitchTurning = 0;                  // the pitch key held: -1 down, +1 up, 0 none
        float Pitch = 0.0f;                     // radians above (+) or below (-) level; only used off the ground
        /// The clock its head went under water, or 0 while it is up. Kept as an instant rather than a total so it
        /// needs no per-decision accumulation, and resets the moment the seat surfaces -- which is what a breath is.
        uint32 SubmergedSinceMs = 0;
        /// Water, kept per seat for every stage (StageScenario::ApplySeatAction): time in the water at all, time
        /// with the head under, the breath spent as the core spends it (BreathSpentMs runs up under water and back
        /// down ten times as fast above it, the shape of Player::HandleDrowning), the most of a breath ever spent,
        /// surfacings after a dive, damage taken under water past the breath (as a fraction of maximum health),
        /// whether that killed the seat, and time spent walking on the water with an aura for it.
        uint32 WaterMs = 0;
        uint32 SubmergedMs = 0;
        uint32 BreathSpentMs = 0;
        float BreathSpentMax = 0.0f;
        uint32 Breaths = 0;
        float DrowningDamage = 0.0f;
        bool Drowned = false;
        uint32 WaterWalkMs = 0;
        uint32 AquaticMs = 0;           // time in a druid's Aquatic Form (FORM_AQUA)
        float LastStepSelfDamage = 0.0f;    // AgentStats::SelfDamage over the last step, as a fraction of max health
        bool DeathLogged = false;           // the death diagnostic line was written for this episode
        uint32 BreathingCasts = 0;      // water-breathing spells started (ActionCatalog::Action::WaterBreathing)
        uint32 ItemUses = 0;
        bool InCombat = false;
        uint32 CombatStartMs = 0;               // episode time the bot entered its current combat
        uint32 TargetSlot = 0;                  // the selected enemy (pulls)
        /// The goal the learner is pursuing for this seat (SeatGoal), NO_GOAL when its policy has no goal head, and
        /// how the seat's decisions have matched it: decisions under a goal, matches, and goal changes.
        int32 Goal = NO_GOAL;
        std::array<uint32, GOAL_COUNT> GoalDecisions{};
        std::array<uint32, GOAL_COUNT> GoalMatches{};
        uint32 GoalChanges = 0;
        bool GoalRewarded = false;              // the goal now held has been paid for (Goals.Match, once per goal)
        uint32 StepPreparationMs = 0;           // buffs, summons and stealth started this decision (SeatGoal::Prepare)
        uint32 FriendSlot = FRIEND_SELF;        // the selected friend (support block)
        uint32 RankTier = 0;                    // the heals' rank tier (support block)

        /// An absorb the bot keeps on a friend, as it was at the last reward: what it soaked since is read from how
        /// much it lost, or from its disappearing early.
        struct AbsorbTrack
        {
            ObjectGuid Unit;
            uint32 SpellId = 0;
            int32 Amount = 0;
            int32 DurationLeftMs = 0;
        };
        std::vector<AbsorbTrack> Absorbs;

        // Support (every stage): wasted and deliberate casts, and time any friend spent low.
        uint32 HealsOnFull = 0;
        uint32 DefensiveCasts = 0;
        uint32 HealingCasts = 0;
        /// Mana spent on healing over the episode. Healing is worth what it restores for what it costs, and only
        /// the restoring half was ever measured: an effective heal of 3% of a health bar and one of 20% paid the
        /// same, so nothing made down-ranking worth the press.
        uint64 HealingPowerSpent = 0;
        uint32 StepHealingPowerSpent = 0;       // ... of it spent since the last reward
        uint32 DownrankedCasts = 0;
        /// Standing in a hostile ground effect: how long, and what it cost. A seat that never learns to step out
        /// pays for it here, and the two numbers say whether it is learning to (hazard_seconds falling while the
        /// fights stay the same length).
        uint32 HazardMs = 0;
        uint64 HazardDamage = 0;
        /// The nearest hazard the seat is not already in, cached: the grid search runs every
        /// StageScenario::HAZARD_SEARCH_MS, and the distance and bearing are recomputed from the seat's own
        /// position every decision, since a ground effect stays where it was cast.
        Hazard NearestHazard;
        uint32 HazardSearchMs = 0;      // episode time of the last search
        /// Enemy casts the seat could have interrupted: counted when one starts, so the press-to-interrupt ratio
        /// can be read against what was actually there to interrupt rather than against presses alone.
        uint32 InterruptibleCastsSeen = 0;
        /// The unit this seat is actually fighting, resolved once a decision (StageScenario::CurrentTarget). The
        /// env's target slots hold creatures and the scripted enemy player; in self-play the opponent is the other
        /// seat and is in no slot at all, so anything that looked a seat's target up by slot was blind there.
        ObjectGuid CurrentTargetGuid;
        ObjectGuid LastInterruptibleCaster;     // ... the caster of the one last counted, so a cast counts once
        uint32 LastInterruptibleSpell = 0;
        uint32 LowHealthMs = 0;

        // Where the bot last saw its target, for when the target hides (SeatView::HiddenTarget).
        ObjectGuid LastSeenGuid;
        Position LastSeen;
        uint32 LastSeenMs = 0;

        // What the character brought (potions, bandages, stones), and what it did with it.
        BattleSupplies Supplies;
        uint32 ConsumablesUsed = 0;
        uint32 SelfResurrections = 0;
        uint32 PetAbilities = 0;
        uint32 PetOrders = 0;
        // Which pet orders the seat gave (by PetOrder), and what its pet was doing while out: attacking something,
        // set passive, told to stay.
        std::array<uint32, std::size_t(PetOrder::Count)> PetOrderCounts{};
        uint32 PetOutMs = 0;
        uint32 PetAttackingMs = 0;
        uint32 PetPassiveMs = 0;
        uint32 PetStayingMs = 0;
        bool PetDied = false;                   // a pet the seat had died this episode
        float LastPetHealth = 0.0f;             // the pet's health at the last decision (0 = no pet)
        ObjectGuid LastPetGuid;                 // the pet given its default stance (PetBlock::DefaultStance)
        uint32 Revives = 0;                   // dead allies (owner, teammates) the seat resurrected
        bool StepRevivedAlly = false;           // an ally the seat resurrected stood up this decision

        // Pacing (CurriculumTuning::ActionTuning) and what the seat has been doing, on the episode clock (sized to the
        // layout at the episode's first observation).
        SeatMemory Memory;
        uint32 ActionsPressed = 0;              // actions other than the no-op the seat took

        // Repeats (ActionTuning::Repeat): per layout action, the episode times of its presses within the window (empty
        // until the first press); the charged presses since the last reward, and over the episode.
        std::vector<std::vector<uint32>> PressTimes;
        uint32 StepRepeats = 0;
        uint32 RepeatedPresses = 0;

        CombatTally Combat;
        RewardLedger Rewards;

        /// Clear the episode totals (not the character).
        void ResetEpisode()
        {
            LastStepDamage = 0.0f;
            LastStepPowerDelta = 0.0f;
            LastStepDamageTaken = 0.0f;
            SpellCasts = 0;
            TrinketUses = 0;
            SubmergedSinceMs = 0;
            WaterMs = 0;
            SubmergedMs = 0;
            BreathSpentMs = 0;
            BreathSpentMax = 0.0f;
            Breaths = 0;
            DrowningDamage = 0.0f;
            Drowned = false;
            WaterWalkMs = 0;
            AquaticMs = 0;
            BreathingCasts = 0;
            LastStepSelfDamage = 0.0f;
            DeathLogged = false;
            Jumps = 0;
            JumpsRefused = 0;
            Drops = 0;
            DropYards = 0.0f;
            FeatherFalls = 0;
            Falls = 0;
            FallDamage = 0.0f;
            FallDeaths = 0;
            Option = SeatOptionSet();
            // Steering is state, and it used to be the only state that outlived its episode. A FACE_* is masked
            // once chosen, so a mode picked in one episode latched for the rest of the run and could never be
            // pressed again; a bearing and a turn carried over the same way. Facing is seeded from the bot once
            // the seat has been placed (StageScenario::ResetSeats), not here, where there is no bot to ask.
            HeldBearing = 0xFF;
            FacingMode = 0xFF;
            Turning = 0;
            PitchTurning = 0;
            Pitch = 0.0f;
            Facing = 0.0f;
            Probe = GroundProbe();
            Trail.Clear();
            MotionHasLast = false;
            MotionTravelled = 0.0f;
            MotionMarkMs = 0;
            MotionMarkTravelled = 0.0f;
            MotionMarkRange = -1.0f;
            MoveRate = 0.0f;
            CloseRate = 0.0f;
            OptionPresses = 0;
            OptionMs = 0;
            ItemUses = 0;
            InCombat = false;
            CombatStartMs = 0;
            TargetSlot = 0;
            Goal = NO_GOAL;
            GoalDecisions.fill(0);
            GoalMatches.fill(0);
            GoalChanges = 0;
            GoalRewarded = false;
            StepPreparationMs = 0;
            FriendSlot = FRIEND_SELF;
            RankTier = 0;
            Absorbs.clear();
            HealsOnFull = 0;
            DefensiveCasts = 0;
            HealingCasts = 0;
            HealingPowerSpent = 0;
            StepHealingPowerSpent = 0;
            DownrankedCasts = 0;
            HazardMs = 0;
            HazardDamage = 0;
            NearestHazard = Hazard();
            HazardSearchMs = 0;
            InterruptibleCastsSeen = 0;
            CurrentTargetGuid.Clear();
            LastInterruptibleCaster.Clear();
            LastInterruptibleSpell = 0;
            LowHealthMs = 0;
            LastSeenGuid.Clear();
            LastSeenMs = 0;
            Supplies = BattleSupplies();
            ConsumablesUsed = 0;
            SelfResurrections = 0;
            PetAbilities = 0;
            PetOrders = 0;
            PetOrderCounts.fill(0);
            PetOutMs = 0;
            PetAttackingMs = 0;
            PetPassiveMs = 0;
            PetStayingMs = 0;
            PetDied = false;
            LastPetHealth = 0.0f;
            LastPetGuid.Clear();
            Revives = 0;
            StepRevivedAlly = false;
            Memory.Reset(0);
            ActionsPressed = 0;
            PressTimes.clear();
            StepRepeats = 0;
            RepeatedPresses = 0;
            Combat = CombatTally();
            Rewards.ResetEpisode();
        }
    };

    /// EnvState::Arena before the env's first episode.
    constexpr uint32 NO_ARENA = ~uint32(0);

    /// A layout index that names none: StageScenario::_directorLayout when no arena has a learned director.
    constexpr uint32 NO_LAYOUT = ~uint32(0);

    struct EnvState
    {
        uint32 Arena = NO_ARENA;                // index into the stage's arenas: what this episode is
        /// Which spawn point this episode drew, into whichever list it drew from. Drawn once at the reset and
        /// held, because the seat is placed from it and the state's origin is read off it every decision.
        uint32 Spawn = 0;
        /// The point this episode drew before anything was tried with it; Spawn is where it was finally built.
        /// The two differ exactly when a drawn point could not build an episode and the reset moved to another,
        /// which used to leave no trace at all -- and silently dropped the tightest of stage2_indoor's three
        /// control rooms out of all 14336 evaluation episodes it ever ran, so its gate was measured on two rooms
        /// while reading as three. The episode columns `spawn_drawn` and `spawn_point` are these two fields.
        uint32 SpawnDrawn = 0;
        std::array<SeatState, MAX_SEATS> Seats;
        uint32 ActiveSeats = 1;                 // seats with a character this episode (the first ones)
        bool Fresh = false;                     // built by Setup, not yet reset
        bool BuildFailed = false;               // the last reset could not build the episode: end it and retry
        uint32 OpponentEntry = 0;               // creature entry: the duel's opponent, the first pull's first member

        /// What an encounter fixed for this episode before its seats were built (Encounter::BeforeLevel): the map
        /// the seats are placed on (0 = the stage's), their level (0 = drawn), the instance difficulty they open it
        /// at, and where they spawn (an instance's front door). Cleared when the arena is drawn.
        uint32 EpisodeMapId = 0;            // read only when HasEpisodeMap: Eastern Kingdoms is map 0
        bool HasEpisodeMap = false;
        uint8 EpisodeLevel = 0;
        /// The side the episode wants its seats on (TeamId + 1; 0: any): a quest or a town belongs to one. The race
        /// draw honours it, and a kept character of the other side is rebuilt.
        uint8 EpisodeTeam = 0;
        uint8 DungeonDifficulty = 0;
        uint8 RaidDifficulty = 0;
        bool HasEpisodeSpawn = false;
        Position EpisodeSpawn;

        /// A resurrection offer already accepted for each seat, and for the owner in the last slot: which seat
        /// made it, and when it was taken. A client sends one CMSG_RESURRECT_RESPONSE and is done; the core has
        /// no reason to clear the request afterwards, so a poll that does not remember it has accepted one will
        /// accept the same offer again every decision (StageScenario::AcceptResurrections).
        std::array<uint32, MAX_SEATS + 1> ResurrectBy{};
        std::array<uint64, MAX_SEATS + 1> ResurrectMs{};
    };
}

#endif
