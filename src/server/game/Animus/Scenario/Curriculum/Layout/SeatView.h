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

#ifndef ANIMUS_LIB_CURRICULUM_SEAT_VIEW_H
#define ANIMUS_LIB_CURRICULUM_SEAT_VIEW_H

#include "RouteShortcut.h"
#include "MoveControls.h"
#include "Aptitude.h"
#include "Block.h"
#include "ClassProfile.h"
#include "CurriculumTuning.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Supplies.h"
#include "TalentBuilder.h"
#include "WorldActions.h"
#include <array>
#include <optional>
#include <vector>

class Creature;
class GameObject;
class Player;
class SpellInfo;
class Unit;

namespace Animus::Curriculum
{
    struct Layout;
    class SeatMemory;

    /// What is on the party past the pack's slots, as a whole dungeon reports it (CrowdBlock).
    struct CrowdView
    {
        bool Present = false;
        uint32 OnParty = 0;                         // creatures whose victim is one of the party
        uint32 OnTank = 0;                          // ... the party's tank
        uint32 Elites = 0;
        Unit const* Tank = nullptr;                 // the seat the crowd is counted against
        GameObject* Object = nullptr;               // the nearest thing the party can use: a lever, the cannon
        std::vector<ObjectGuid>* Used = nullptr;    // where a use is recorded, so each thing is used once a run
        bool Behind = false;                        // the seat's place on the route is behind the party's (risen)
        std::array<Unit*, CROWD_SLOTS> Units{};     // the next enemies past the pack's slots, fight first
        uint32 Count = 0;
        bool HasAhead = false;                      // the nearest pack not in the fight, and how many stand with it
        Position Ahead;
        uint32 AheadSize = 0;
        bool HasSecond = false;                     // the nearest creature out of the fight past the pack ahead
        Position Second;
        float Still = 0.0f;                         // time without progress or a fight / 120 s
        /// A few yards on along the dungeon's field route towards the seat's objective: where the advance action
        /// walks, straight, where the server's navmesh does not join the way (a drop into a cavern).
        bool HasStep = false;
        Position Step;
        /// The run an advance walks (movement-smooth A8): the route's corners about 18 yards on (Step the first), or
        /// the detour's points back to it.
        std::array<Position, 6> Path{};
        uint32 PathPoints = 0;
        bool AtDoor = false;                        // the run was cut at a closed door: nothing walks through it
    };

    /// The world outside a fight, as the life encounters read it for the WorldBlock (or the live module's life
    /// service for a companion): the nearest thing of each kind within the seat's senses, and the episode's quest.
    struct WorldView
    {
        enum QuestStates : uint8 { QUEST_NONE = 0, QUEST_ACTIVE = 1, QUEST_COMPLETE = 2 };

        bool Active = false;                        // something fills this: the block's features are live
        Unit* Corpse = nullptr;                     // the nearest corpse the seat may loot, else one it may skin
        bool CorpseQuestItem = false;
        bool CorpseSkinnable = false;
        Creature* Giver = nullptr;                  // the nearest quest giver the seat has business with
        bool GiverOffers = false;
        bool GiverTurnIn = false;
        GameObject* Node = nullptr;                 // the nearest gathering node
        WorldActions::NodeKind NodeKind = WorldActions::NodeKind::None;
        bool NodeOpenable = false;
        Creature* Vendor = nullptr;                 // the nearest vendor (a repairer when VendorRepairs)
        bool VendorRepairs = false;
        uint8 QuestState = QUEST_NONE;              // the episode's quest
        float QuestProgress = 0.0f;
        /// What the seat's own quests want nearby (QuestPlanner, WorldActions::Sense): an object to use or open,
        /// a creature to use a quest item on (and the item), a vendor selling a quest item (and the item).
        GameObject* QuestObject = nullptr;
        Unit* ItemTarget = nullptr;
        uint32 UseItem = 0;
        Creature* QuestVendor = nullptr;
        uint32 BuyItem = 0;

        /// **The journal** (long-horizon plan, Component B): what a player keeps in the quest log and on the map
        /// -- the current quest's objectives with what is left of each and where it is done, the giver and the
        /// turn-in, the places found where something wanted was seen (and how long ago, and whether another group
        /// has claimed them), where it is in a chain of quests, and the area the world coordinator assigned. The
        /// sim's life encounters and the module's life service both fill it; nothing here is remembered by the
        /// network, which is the point: minutes of plan held as facts, not as a 128-unit memory.
        static constexpr uint32 JOURNAL_OBJECTIVES = 4;
        static constexpr uint32 JOURNAL_PLACES = 8;
        struct JournalObjective
        {
            bool Present = false;
            uint8 Kind = 0;                         // ObjectiveKind
            float Left = 0.0f;                      // share still to do, 0 = done
            bool HasPlace = false;
            Position Place;
        };
        struct JournalPlace
        {
            bool Present = false;
            Position Where;
            float AgeSeconds = 0.0f;                // since it was last seen
            bool Claimed = false;                   // another group holds it (the coordinator's claims)
            uint8 Objective = 0;                    // which objective's sources were seen there
        };
        std::array<JournalObjective, JOURNAL_OBJECTIVES> Objectives{};
        bool HasGiver = false;
        Position GiverAt;
        bool HasEnder = false;
        Position EnderAt;
        std::array<JournalPlace, JOURNAL_PLACES> Places{};
        /// A dungeon's way on in Places and the assignment (InstanceEncounter::View: the next packs, the next route
        /// point, the tank) with no journal behind them: the goal block reads the places and the assignment, and
        /// nothing else of the world view is live (Active stays false, so no world action changes).
        bool RoutePlaces = false;
        uint8 ChainIndex = 0;                       // quests of the chain turned in
        uint8 ChainLength = 0;
        bool HasAssignment = false;                 // the area the coordinator gave the seat's group
        Position Assignment;
    };

    /// One bot's situation at a decision: what the blocks cannot read from the world themselves. The scenario fills
    /// it; each part is only used by the blocks that need it.
    /// What a durative action ("option") the seat started is doing. One press stands for many decisions -- resting
    /// until it is ready to fight, holding an interrupt for the target's next cast -- which is how a plan longer
    /// than a decision is expressed at all: 1800 decisions of a 450 s episode are far more than
    /// credit reaches back over. The block that owns the action starts it, the block that can act runs it every
    /// decision until its own stop condition or UntilMs, and any other action the policy takes cancels it.
    ///
    /// No option moves the seat: the duel's keep-range and stay-on-target, the held bearing and the companion's
    /// follow went with every engine move of a seat (player-controller C3, C9). Its feet are the move block's keys.
    enum class SeatOptionKind : uint8
    {
        None = 0,
        RestUntilReady,     // eat and drink between pulls until health and mana are back
        HoldInterrupt,      // interrupt the target as soon as it casts
        Count
    };

    /// Holding an interrupt is a standby, not something the seat does: it waits for the target to cast while the seat
    /// keeps fighting, so every other action leaves it running. Cancelling it on any press left it lasting 0.6 s
    /// against casts of 1.5-2.5 s (stage2_pack 2026-09-18: the warlock pressed it 7.8 times a fight and interrupted
    /// 0.01 casts, the druid 10.4 times for none).
    [[nodiscard]] constexpr bool IsStandby(SeatOptionKind kind)
    {
        return kind == SeatOptionKind::HoldInterrupt;
    }

    /// A hostile ground effect: where its centre is and how wide it is, so a seat can see both which way out is
    /// shortest and, for one it is not in yet, which way not to walk.
    /// The ray march along each of the SENSE_RAYS rays, kept between decisions.
    ///
    /// A march is eighty height samples and forty-eight navmesh rays where the old single probe was eight, which
    /// is too much to redo every 250 ms for 128 environments. It does not have to be: the ground forty yards out
    /// does not change, only the seat's place in it, so the march is redone when the seat has walked far enough
    /// or turned far enough for the old one to be describing somewhere else -- the same trick the hazard search
    /// already uses, with the triggers that matter here. A plain clock will not do, because at seven yards a
    /// second a one-second-old march is seven yards stale and the nearest cell it reports is six.
    ///
    /// Sixteen rays: a gully's mouth or a doorway sits between two 45-degree rays as often as on one, and a seat
    /// that cannot see it cannot choose the turn that lines it up.
    struct GroundProbe
    {
        float Reach[SENSE_RAYS] = {};           // distance to the first obstruction along each ray / MARCH_MAX
        float Step[SENSE_RAYS] = {};            // the height change that stopped it, signed, / MAX_STEP
        float Shore[SENSE_RAYS] = {};           // how far dry ground runs that way / MARCH_MAX
        float Burns[SENSE_RAYS] = {};           // how near the magma or slime is, 1 at the feet, 0 for none
        float Clearance = 1.0f;                 // yards to the nearest edge of walkable space / CLEARANCE_RANGE
        float ClearanceSin = 0.0f;              // and which way is out, in the seat's frame when it was measured
        float ClearanceCos = 0.0f;
        Position From;                          // where it was marched from
        float Facing = 0.0f;                    // and which way the seat was looking at the time
        uint32 Ms = 0;
        bool Valid = false;
        /// With the layered fields the probe is worked out along fixed compass headings, SENSE_RAYS * 2 of them
        /// (LayeredField::SenseCompass), and turned to the seat's facing every decision (RaysFor): turning costs
        /// nothing, and only walking MARCH_REFRESH_YARDS from CompassFrom works it out again. The room's way out
        /// is kept in the world's frame for the same reason.
        float CompassReach[2 * SENSE_RAYS] = {};
        float CompassStep[2 * SENSE_RAYS] = {};
        float CompassShore[2 * SENSE_RAYS] = {};
        float CompassBurns[2 * SENSE_RAYS] = {};
        float CompassClearance = 1.0f;
        bool CompassDirected = false;
        float CompassAway = 0.0f;
        Position CompassFrom;
        bool CompassValid = false;
    };

    /// Where the seat has been: its last TRAIL_SAMPLES positions, one every INTERVAL_MS, kept between decisions
    /// and read back in the seat's own frame (MoveBlock::OBS_TRAIL_FIRST). A policy has memory, but a recurrent
    /// state is a poor place to keep a map, and the episodes a trained policy loses are lost rather than wedged:
    /// they cover three times the route and end where they began. This is the concrete thing it can hold against
    /// a loop -- the spot it stood on eight seconds ago is behind it and to the left, or it is under its feet
    /// again. Nothing here says which way to go.
    struct MovementTrail
    {
        static constexpr uint32 INTERVAL_MS = 1000;
        /// Within this many yards of an earlier sample the seat counts as still there: arriving's own six.
        static constexpr float DWELL_YARDS = 6.0f;

        float X[TRAIL_SAMPLES] = {};
        float Y[TRAIL_SAMPLES] = {};
        uint32 Count = 0;                       // samples taken so far, up to TRAIL_SAMPLES
        uint32 Next = 0;                        // the ring slot the next sample goes in
        uint64 LastMs = 0;                      // the clock the last one was taken at
        bool Started = false;

        void Clear() { *this = MovementTrail(); }
    };

    struct Hazard
    {
        float Distance = 0.0f;      // yards from the unit to its centre
        float Radius = 0.0f;        // ... and its radius, so Radius - Distance is the way out (negative: outside it)
        float Bearing = 0.0f;       // the direction of its centre, relative to the unit's facing
        Position Centre;            // where it is, so a cached one can be measured again as the seat moves
        bool Present = false;
    };

    struct SeatOption
    {
        SeatOptionKind Kind = SeatOptionKind::None;
        uint64 UntilMs = 0;                         // the clock (SeatView::NowMs) it runs out at

        [[nodiscard]] bool Running(SeatOptionKind kind, uint64 nowMs) const
        {
            return Kind == kind && nowMs < UntilMs;
        }
    };

    /// The one slot an option occupies: resting and holding an interrupt are alternatives (a seat waiting out a pull
    /// is not holding one), so a press of either replaces the other.
    enum class SeatOptionSlot : uint8
    {
        Standby = 0,
        Count
    };

    [[nodiscard]] constexpr SeatOptionSlot SlotOf(SeatOptionKind /*kind*/)
    {
        return SeatOptionSlot::Standby;
    }

    /// The durative actions a seat is running, one per slot.
    struct SeatOptionSet
    {
        std::array<SeatOption, std::size_t(SeatOptionSlot::Count)> Slots{};

        [[nodiscard]] SeatOption& Of(SeatOptionKind kind) { return Slots[std::size_t(SlotOf(kind))]; }
        [[nodiscard]] SeatOption const& Of(SeatOptionKind kind) const { return Slots[std::size_t(SlotOf(kind))]; }

        [[nodiscard]] bool Running(SeatOptionKind kind, uint64 nowMs) const { return Of(kind).Running(kind, nowMs); }
        [[nodiscard]] bool Any(uint64 nowMs) const
        {
            for (SeatOption const& option : Slots)
                if (option.Kind != SeatOptionKind::None && nowMs < option.UntilMs)
                    return true;
            return false;
        }

        void Start(SeatOptionKind kind, uint64 untilMs) { Of(kind) = SeatOption{ kind, untilMs }; }
        void Stop(SeatOptionKind kind) { if (Of(kind).Kind == kind) Of(kind) = SeatOption(); }
        void Clear() { Slots = {}; }
    };

    struct SeatView
    {
        Layout const* L = nullptr;
        Player* Bot = nullptr;
        /// The goal the seat holds (SeatGoal), as its policy last sent it, or NO_GOAL. It shapes what the core block
        /// offers (CoreBlock::GoalCloses): the forge and the module both set it, so the masks agree.
        int32 Goal = -1;
        /// The goal it held was reached or can no longer be pursued this decision: the learner chooses again now
        /// rather than at its clock (GoalBlock::OBS_ENDED).
        bool GoalEnded = false;
        bool GoalReached = false;                   // ... because it was reached
        /// The secondary goal it holds beside the primary (NO_GOAL for none), and whether it ended this decision
        /// (GoalBlock::OBS_SECONDARY_ENDED): both sides then drop it until the next choice. Masks open what either
        /// goal needs (CoreBlock::GoalCloses closes only what both close).
        int32 Goal2 = -1;
        bool Goal2Ended = false;
        /// Something changed that a plan should answer (GoalBlock::OBS_EVENT): the goal head chooses again now.
        bool GoalEvent = false;
        /// The primary the director's order set (NO_GOAL when none): the seat holds it whatever it chose, and the
        /// learner reads it from GoalBlock's order columns.
        int32 OrderGoal = -1;
        /// What the seat achieved this decision whatever it pursued (GoalBlock's hindsight columns), or NO_GOAL.
        int32 Achieved = -1;
        /// The seat's durative action, to read, start and stop. Null for a view without one.
        /// The nearest hostile ground effect the seat is not standing in (StageScenario::TrackHazards): what makes
        /// avoiding one possible rather than only leaving one.
        Hazard NearestHazard;
        SeatOptionSet* Option = nullptr;
        /// How long each durative action may run (CurriculumTuning::OptionTuning).
        CurriculumTuning::OptionTuning Options;
        /// What the actions aim at: the opponent, the selected enemy. May be null (between pulls).
        Unit* Target = nullptr;
        /// The target when the bot can neither see nor detect it (stealth, invisibility). Target is null then, so no
        /// block reads what a player could not know; the duel block searches where it was last seen.
        Unit* HiddenTarget = nullptr;
        bool TargetSeen = false;                    // LastSeen holds where the target was when the bot last saw it
        Position LastSeen;
        float TargetUnseenTime = 0.0f;              // time since the bot last saw the target / 20 s, clamped

        /// Per catalog action, the highest rank the bot knows (SeatState::KnownRanks); null for a view built
        /// without one, where Encoding::KnownRank resolves the chain itself.
        std::vector<SpellInfo const*> const* KnownRanks = nullptr;

        // Core: the character, as built, and what happened since the last decision.
        uint8 Level = 1;
        uint8 Race = 0;
        uint8 Spec = 0;
        Aptitude Apt;                               // what this character can do (SeatState::Apt)
        /// **Where the seat is looking**: the frame every bearing it observes is measured off. The controlled body's
        /// yaw (Movement::BodyState::Yaw) once the controller steps it; seeded from the bot at an episode's start.
        float Facing = 0.0f;
        /// The seat's own ray march, borrowed rather than copied: Observe is const, but the march it reads is
        /// refreshed in place, exactly as the hazard search is.
        GroundProbe* Probe = nullptr;
        /// Where it has been, the same way: sampled in place by the move block once a second.
        MovementTrail* Trail = nullptr;
        /// **The keys and mouse it holds** (MoveControls::SeatControls, MoveBlock), and the body the player
        /// controller moves with them (Movement::BodyState). Borrowed like the probe; null for a view without them,
        /// which holds nothing and moves nowhere.
        MoveControls::SeatControls* Controls = nullptr;
        Movement::BodyState* Body = nullptr;
        float SubmergedTime = 0.0f;                 // seconds its head has been under, 0 while it is up
        /// How much of its breath the seat has spent, 0 to 1 and past it while drowning: the core's own timer
        /// (WaterBreath.Timer, 180 s by default), run up under water and back down ten times as fast above it. 0
        /// under a water-breathing aura, when the core runs no timer at all.
        float BreathSpent = 0.0f;
        TalentBuilder::Build const* Build = nullptr;
        float LastStepDamage = 0.0f;                // damage done / the level's damage scale
        float LastStepPowerDelta = 0.0f;            // primary power change, as a fraction of max
        float LastStepDamageTaken = 0.0f;           // / the bot's max health
        float EpisodeTime = 0.0f;                   // time into the episode / EPISODE_TIME_SCALE_MS, clamped
        SeatMemory const* Memory = nullptr;         // what the seat has been doing; null: none (features at rest)
        uint64 NowMs = 0;                           // the clock Memory was kept with
        uint32 DecisionMs = 250;                    // how long a decision lasts (the scenario's decision clock)
        /// The closed doors near a dungeon wing's party (InstanceEncounter's EnvInstance::ClosedDoors); null
        /// elsewhere. Steer's runs stop at one, as the advance's do: a spline walks through anything.
        std::vector<RouteShortcut::Door> const* ClosedDoors = nullptr;

        // Duel: time in combat, what the bot brought (potions, bandages, stones), whether it may resurrect itself, and
        // a hunter's beasts on offer.
        float CombatTime = 0.0f;                    // time in combat / 60 s, clamped; 0 out of combat
        BattleSupplies Supplies;
        bool SelfResurrectAllowed = true;           // not in the PvP stages
        /// Death runs on here (ArenaDefinition::DeathRuns; a companion in the world): a dead seat releases, runs
        /// back and rises (DeathBlock) rather than being stood up. And how long it has been dead, in seconds.
        bool DeathRuns = false;
        float DeadSeconds = 0.0f;
        std::array<uint32, STABLE_SLOTS> Stable{};
        uint32 StableCount = 0;

        // The crowd past the pack's slots (CrowdBlock), where the encounter reports one.
        CrowdView Crowd;

        // Pack: the current pull's enemies, in slot order (null for a slot whose enemy is gone).
        std::array<Unit*, PACK_SLOTS> Enemies{};
        uint32 EnemyCount = 0;                      // slots in use; 0 between pulls
        uint32 TargetSlot = 0;                      // the selected enemy; target selection updates it

        // Support: the selected friend (FRIEND_SELF, FRIEND_OWNER, FRIEND_TEAMMATE_FIRST + teammate) that positive
        // single-target spells are cast on, and the rank tier heals with ranks are cast at (0 = highest known). Only
        // read with the support block; the actions update them.
        uint32 FriendSlot = FRIEND_SELF;
        uint32 RankTier = 0;

        // Gauntlet.
        uint32 PullsCleared = 0;
        float QuietTime = 0.0f;                     // time since the last fight ended / 20 s, clamped
        float PullTime = 0.0f;                      // time into the current pull / 60 s, clamped
        bool ElitePull = false;
        uint32 FoodItem = 0;
        uint32 DrinkItem = 0;
        uint32 GauntletSupplies = CONSUMABLE_COUNT; // food and drink stocked, each
        float PullArrival = 0.0f;                   // an unengaged pull comes to the seat in this / 30 s; else 0
        float NextPull = 0.0f;                      // between pulls: the next one spawns in this / 20 s

        // Companion: the player the bot fights for.
        Player* Owner = nullptr;
        /// What the owner can do, the same six numbers a teammate is described by. Unset when the scenario has no
        /// owner: "there is nobody" and "there is somebody who heals nothing" are different things.
        std::optional<Aptitude> OwnerApt;

        // Party: the other learned players, and the party's living tank (may be the bot).
        struct Teammate
        {
            Player* Bot = nullptr;
            int32 Goal = NO_GOAL;                   // what it is pursuing (SeatGoal), as its policy last sent
            Aptitude Apt;                           // what it can do; the blocks show the six-number brief of it
            uint8 Class = 0;
        };

        std::array<Teammate, PARTY_MEMBERS> Teammates{};
        Player* Tank = nullptr;

        /// The raid the seat's group belongs to, in aggregate: a seat acts on its own group and the spotlight slots,
        /// but it has to know how the rest of the raid is doing. All zero below a party.
        struct RaidView
        {
            uint32 Group = 0;                       // the seat's group index (0 in a party)
            float Alive = 0.0f;                     // living seats, as a share of the seats in play
            float GroupAlive = 0.0f;                // ... of the seat's own group
            float InCombat = 0.0f;                  // seats in combat, as a share of the living
            float LowestHealth = 1.0f;              // the most hurt living seat
            float TanksAlive = 0.0f;                // living tanks / RAID_GROUPS, clamped
            float HealersAlive = 0.0f;              // living healers / RAID_GROUPS, clamped
        };

        RaidView Raid;

        // Travel: where the seat is going, and whether it may ride there.
        bool HasObjective = false;
        Position Objective;
        /// How much longer the walking way round to the objective is than the straight line to it, as a ratio;
        /// 0 without an objective and 1 when the straight line is the route. Measured on foot at the episode's
        /// build, water and magma excluded, so it is what the ground costs rather than what the pathfinder would
        /// permit -- a player's filter admits both and would call a lake a straight shot.
        float Detour = 0.0f;
        /// Whether the legs are getting anywhere, over about the last second: how far the seat moved against how
        /// far running would have carried it, and the share of the distance to the objective that closed.
        float MoveRate = 0.0f;
        float CloseRate = 0.0f;
        /// False in an on-foot arena (ArenaDefinition::OnFoot): the mount actions are masked out.
        bool MountsAllowed = true;
        /// How near counts as arrived, which is not the same number indoors as it is in open country. Carried
        /// on the view so OBS_AT_OBJECTIVE, the masks that ask whether the seat is there yet, and the reward
        /// that pays for arriving all read one answer.
        float ArriveWithin = 6.0f;
        /// False in an air-only arena (ArenaDefinition::AirOnly): the ground mount is masked, the wings are not.
        bool GroundMountAllowed = true;

        // Flag match: the seat's flag and the other side's, from the seat's side.
        enum class FlagState : uint8 { AtBase, Carried, Dropped };
        struct FlagMatch
        {
            bool Active = false;
            FlagState Own = FlagState::AtBase;      // carried: by the enemy
            FlagState Enemy = FlagState::AtBase;    // carried: by the seat
            Position OwnBase;
            Position EnemyBase;
            Position OwnDropped;                    // where each lies when dropped
            Position EnemyDropped;
            uint32 OwnScore = 0;
            uint32 EnemyScore = 0;
            /// The flag the seat could take or return right now, if one is in reach. A real battleground scores a
            /// pickup only when the player uses the object (BattlegroundWS::EventPlayerClickedOnFlag), so
            /// standing on it does nothing: this is what ACTION_TAKE_FLAG acts on. Empty when none is in reach,
            /// and always empty for an arena that plays the flag rules by proximity itself.
            ObjectGuid Usable;
        } Flags;

        /// Life outside the fight (WorldBlock). Inactive in every arena that has no life encounter.
        WorldView World;

        /// What the side's director asked of this seat. Advice, not a lever: the seat reads it and still chooses
        /// its own actions. Inactive in an arena with no director, where every field below is ignored.
        struct TeamOrder
        {
            bool Active = false;
            TeamPosture Posture = TeamPosture::Attack;
            TeamRally Rally = TeamRally::None;
            Position RallyPlace;                    // where Rally resolved to, when it names a place
            bool HasRallyPlace = false;
            Unit* Focus = nullptr;                  // the enemy the side concentrates on, when one is called
            /// A focus was called and this seat cannot see it. Without this, "no call" and "a call I cannot
            /// see" are the same all-zero observation, and a seat told to kill someone it has lost would read
            /// it as having been told nothing.
            bool FocusUnseen = false;
            bool IsDuty = false;                    // this seat owes an interrupt or a control (its own order)
            /// The order to this seat alone (Component E): its kind, what it is about -- an enemy or a friend
            /// (Target), a journal objective (Objective) or the called place (GoTo, RallyPlace) -- who it came
            /// from, and how old it is. None when the seat holds only the side's order.
            OrderKind Kind = OrderKind::None;
            Unit* Target = nullptr;
            uint32 Objective = 0;
            OrderSource Source = OrderSource::Side;
            float Age = 0.0f;                       // decisions since it was given / 40, clamped
        } Order;

        // PvP: the enemy player.
        Player* Opponent = nullptr;
        bool OpponentHidden = false;                // the bot can neither see nor detect it
        uint8 OpponentClass = 0;
        Aptitude OpponentApt;
        bool Mirror = false;                        // the opponent is a learned agent too
    };

    /// What an applied action did, for the scenario's bookkeeping and rewards.
    /// A pet order a seat gave (SeatActionResult::PetOrderGiven), for the per-order episode counts.
    enum class PetOrder : uint8
    {
        None,
        Attack,                                     // sent pets and guardians at the target
        Passive,
        Defensive,
        Aggressive,
        Follow,
        Stay,
        Count
    };

    struct SeatActionResult
    {
        uint32 SpellCasts = 0;
        // What a world press did (WorldBlock): the life encounters read these for their rewards and columns.
        uint32 Interactions = 0;
        uint32 Wasted = 0;                          // a press that did nothing in the world
        uint32 CorpsesLooted = 0;
        uint32 NodesLooted = 0;
        uint32 ItemsLooted = 0;
        uint32 CopperLooted = 0;
        uint32 GatherCasts = 0;                     // gathering and skinning casts started
        uint32 Equipped = 0;
        uint32 CopperSold = 0;
        uint32 Repairs = 0;
        uint32 CopperRepaired = 0;
        uint32 SuppliesBought = 0;
        bool QuestAccepted = false;
        bool QuestTurnedIn = false;
        uint32 TrinketUses = 0;
        uint32 ItemUses = 0;                        // use effects of an equipped weapon or off-hand item
        uint32 SustainCasts = 0;
        uint32 FoodUsed = 0;
        uint32 DrinkUsed = 0;
        uint32 HealsOnFull = 0;                     // direct heals started on a friend at full health (masked: 0)
        uint32 DefensiveCasts = 0;                  // short damage reductions and immunities started
        uint32 BreathingCasts = 0;                  // water-breathing spells started (Unending Breath, Aquatic Form)
        uint32 HealingCasts = 0;                    // heals, HoTs and absorbs started ...
        uint32 DownrankedCasts = 0;                 // ... below the highest known rank
        uint32 HealingPowerSpent = 0;               // ... and the mana they cost (SpellInfo::CalcPowerCost)
        uint32 FoodFailed = 0;                      // eat or drink pressed and allowed, but nothing was consumed
        uint32 DrinkFailed = 0;
        bool StealthOpener = false;                 // a harmful spell that breaks stealth started from stealth
        ObjectGuid StealthUtilityTarget;            // a harmful spell that keeps stealth (Sap, Distract) aimed here
        ObjectGuid PendingInterrupt;                // an interrupt was cast at this casting enemy
        uint32 CallBeast = 0;                       // hunters: call this stable beast (the scenario creates the pet)
        uint32 ConsumablesUsed = 0;                 // potions, healthstones, bandages, soulstones
        /// Whether the press did something in the world that costs a resource or a global cooldown: a spell that
        /// started casting, an item or trinket used, food or drink, a pet ability. Pressing the same button again is
        /// only waste when the button did nothing -- a caster's rotation is the same nuke over and over, and
        /// charging it as a repeat charges the correct play (stage1_duel at 10M: warlock_dps 39.7 repeated presses
        /// an episode, mage_dps 16.6, the two lowest-scoring layouts in the run).
        [[nodiscard]] bool DidSomething() const
        {
            return SpellCasts || ItemUses || TrinketUses || PetAbilities || FoodUsed || DrinkUsed;
        }

        uint32 PreparationMs = 0;                   // a helpful spell started out of combat: its cast time or a GCD
        bool SelfResurrected = false;
        /// After dying, where death runs on (DeathBlock): the spirit released, a friend's resurrection accepted
        /// (the runner takes it), risen at the corpse, or raised by the spirit healer.
        bool Released = false;
        bool AcceptResurrection = false;
        bool RoseAtCorpse = false;
        bool SpiritHealer = false;
        uint32 Revives = 0;                         // resurrection spells started on a dead ally
        uint32 PetAbilities = 0;                    // pet bar abilities the pet started
        uint32 PetOrders = 0;                       // pet stances, follow and stay, and sending the pet in
        PetOrder PetOrderGiven = PetOrder::None;    // which of them, when one was given
        /// Steering that failed to commit (MoveBlock, Actions.Jitter, MoveControls::Press): a turn or pitch rate, or
        /// a climb, against the last one within 1500 ms (MovePrice::COUNT_MS), the feet reversed within it (forward to
        /// back, left to right: BearingFlip, in half turns), and any of them 1.5 to 4 s on (Weaves).
        uint32 TurnReversals = 0;
        float BearingFlip = 0.0f;
        uint32 PitchReversals = 0;
        uint32 Weaves = 0;
        /// ... and what they cost, in quarter turns undone weighed by how recent the choice undone was
        /// (MovePrice::Undone, Recency): the Actions.Jitter charge.
        float JitterWeight = 0.0f;
        /// The share of a full press this press costs in Actions.Effort: a steering press by its angle
        /// (MovePrice::EffortOf), everything else 1.
        float EffortWeight = 1.0f;
        /// What a spell press was aimed at, for judging it against the seat's goal (StageScenario::JudgePress):
        /// the unit it went to (the enemy for a harmful spell, the friend or the seat for a helpful one), whether
        /// it was harmful, and whether it came from the tactical list (crowd control, interrupts, taunts).
        ObjectGuid CastAt;
        bool CastHarmful = false;
        bool CastTactical = false;
        bool CastTaunt = false;                     // a taunt (Taunt, Growl, Hand of Reckoning, Dark Command, ...)
        bool CastTankMode = false;                  // a tank's stance, form, aura or presence
        uint32 RefusedCast = 0;                     // a press that did not start: the core's SpellCastResult
        bool KeyStillHeld = false;                  // the control already held, pressed again: not a press
        bool ControlChanged = false;                // a move press that changed a held control (MoveBlock)
        bool CastTrap = false;                      // a trap laid (a trap object summoned, or a missile that drops one)
        bool CastDispel = false;
        bool CastReachesFocus = false;              // an area spell with no unit: the focus was inside its radius
    };
}

#endif
