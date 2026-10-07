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
#include "FreeLook.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Supplies.h"
#include "TalentBuilder.h"
#include <array>
#include <optional>
#include <vector>

class Creature;
class GameObject;
class Player;
class SpellInfo;
class Unit;

namespace Animus::Vision
{
    struct SeenList;
    struct FrameHits;
    class MentalMap;
    class EntityMemory;
}

namespace Animus::Curriculum::EntityActions
{
    class ClientPort;
}

namespace Animus::Curriculum
{
    struct Layout;
    class SeatMemory;

    /// A dungeon's way on, as the goal head names it (peak-play W3, InstanceEncounter::View and SeenWorld): the goal
    /// block's TravelTo places and its assignment. (The journal of the first curriculum's quests, with the corpses,
    /// nodes and vendors beside it, was deleted with it; the goal space keeps its targets, which are the layout's.)
    struct WorldView
    {
        static constexpr uint32 JOURNAL_PLACES = 8;
        struct JournalPlace
        {
            bool Present = false;
            Position Where;
        };
        std::array<JournalPlace, JOURNAL_PLACES> Places{};
        /// Places and the assignment are a dungeon's way on (the stage has them): the goal block reads them. False in
        /// a stage without a route, where a trip's objective takes the assignment's slot.
        bool RoutePlaces = false;
        bool HasAssignment = false;                 // the seat's own objective, in the assignment slot
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
        /// A sight stage's ground fire is what its camera shows (dungeon-curriculum I3, CombatBlock::ReadHazards):
        /// the visible hazards it stands in and the deepest of them; NearestHazard is then the nearest visible one too.
        /// The duel block reads these in place of the seat's auras.
        bool HazardsSeen = false;
        uint32 StandingSeen = 0;
        Hazard DeepestSeen;
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
        /// A sight stage's selection is in the camera's last frame (dungeon-curriculum I3). Out of it, the duel block
        /// reads its place as where the seat last saw it (LastSeen, from the entity memory), never where it is; its
        /// target-frame facts stay. Always true outside a sight stage.
        bool TargetInView = true;

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
        /// Where it has been, borrowed rather than copied: Observe is const, but the move block samples it in
        /// place once a second.
        MovementTrail* Trail = nullptr;
        /// **The keys and mouse it holds** (MoveControls::SeatControls, MoveBlock), and the body the player
        /// controller moves with them (Movement::BodyState). Borrowed like the probe; null for a view without them,
        /// which holds nothing and moves nowhere.
        MoveControls::SeatControls* Controls = nullptr;
        Movement::BodyState* Body = nullptr;
        /// **The seat's camera image**, written by the vision block: Vision::ImageBytes bytes (camera-vision.BYTES.md),
        /// the seat's row of the pool's image (EnvPool::Image, or FinalImage for an ended episode's last look). Null
        /// for a view without one (a stage with no vision block): the block then writes its scalars alone.
        uint8* Image = nullptr;
        /// **The seat's camera** (Vision::FreeLook::State, SeatState::Look): turned by its look head, advanced and
        /// rendered by the vision block. Borrowed like the controls; null for a view without one, which the block
        /// renders from a fixed camera (yaw offset 0, the conf's pitch and zoom, at the canonical size).
        Vision::FreeLook::State* Look = nullptr;
        /// **What its camera's last frame showed** (perception-goals 1b, SeatState::Seen): written by the vision
        /// block as it renders, read by the entities block after it. Null for a view without a camera.
        Vision::SeenList* Seen = nullptr;
        /// **The rays of its camera's last frame, as cast** (SeatState::Hits): written by the vision block in a stage
        /// with a map, read by the map block after it. Null without a map.
        Vision::FrameHits* Hits = nullptr;
        /// **Its mental map** (perception-goals REDESIGN §3, SeatState::Map) and the row its crop goes to (the pool's,
        /// EnvPool::MapCrop or FinalMapCrop: Vision::CROP_BYTES), written by the map block. MapKept: this episode's
        /// map was kept from the last (amendment 1). Null for a view without one.
        Vision::MentalMap* Map = nullptr;
        uint8* MapRow = nullptr;
        bool MapKept = false;
        /// **Its entity memory** (dungeon-curriculum I2, SeatState::Recall): written by the entities block from the
        /// frame's list, read by the sight block. RecallKept: this episode's was kept from the last. Null for a view
        /// without a sight block.
        Vision::EntityMemory* Recall = nullptr;
        bool RecallKept = false;
        /// **What each of the sight block's slots named at its last observation** (SeatState::SightGuids): the raw
        /// GUID a press on the slot acts on, 0 an empty slot; and the seat's client focus (SeatState::Focus), the
        /// friend its beneficial spells go to. Null for a view without a sight block.
        std::array<uint64, SIGHT_SLOTS>* SightGuids = nullptr;
        ObjectGuid* Focus = nullptr;
        /// Where the sight block's presses and a sight stage's casts go: null for the seat's session's handlers
        /// (EntityActions::SessionPort); a test's recorder otherwise.
        EntityActions::ClientPort* Port = nullptr;
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

        // Duel: time in combat, what the bot brought (potions, bandages, stones), whether it may resurrect itself, and
        // a hunter's beasts on offer.
        float CombatTime = 0.0f;                    // time in combat / 60 s, clamped; 0 out of combat
        BattleSupplies Supplies;
        bool SelfResurrectAllowed = true;           // not in the PvP stages
        std::array<uint32, STABLE_SLOTS> Stable{};
        uint32 StableCount = 0;

        // Pack: the current pull's enemies, in slot order (null for a slot whose enemy is gone).
        std::array<Unit*, PACK_SLOTS> Enemies{};
        uint32 EnemyCount = 0;                      // slots in use; 0 between pulls
        uint32 TargetSlot = 0;                      // the selected enemy; target selection updates it

        /// The rank tier rankable spells are cast at (0 = the highest known; CoreBlock's rank actions set it).
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

        /// **What a player's UI shows of the rest of its party** (the party frames block, revision 2; dungeon-
        /// curriculum I5 and G1): the party frames -- always: health, power, alive, in combat, which one leads, the
        /// debuffs on it, whether something attacks it, whom it has targeted -- and the minimap's party dots, only for
        /// members within its radius, as a position relative to the seat. The leader first, then the other members in
        /// group (or seat) order. Filled from the seat's group (PartyFramesBlock::FillFromGroup, StageScenario::
        /// ViewSeat) or by the encounter whose party has no core group (PartyFollowEncounter::View); each frame by
        /// PartyFramesBlock::FillFrame, the one place a member's frame is read.
        struct PartyFrame
        {
            bool Present = false;
            bool Alive = false;
            bool Leader = false;
            bool InCombat = false;
            float Health = 0.0f;                    // fractions of the maximum
            float Power = 0.0f;
            bool DotShown = false;                  // on the minimap: within its radius
            float DotRight = 0.0f;                  // yards, in the seat's facing frame (heading-up minimap)
            float DotForward = 0.0f;
            bool ManaUser = false;                  // its power is mana
            bool InRange = false;                   // within PartyFramesBlock::FRAME_RANGE (the client fades it past)
            uint32 Debuffs = 0;                     // harmful auras on it, as its frame shows them
            uint32 Dispellable = 0;                 // ... of a kind a dispel removes
            bool Aggro = false;                     // something is attacking it (the frame's red border)
            bool Selected = false;                  // the seat's selection
            bool Focused = false;                   // the seat's focus
            bool HasTarget = false;                 // it has a selection the seat's client also has
            bool TargetHostile = false;             // ... hostile to the seat
            bool TargetMine = false;                // ... the seat's own selection
            bool TargetInView = false;              // ... in the seat's camera's frame now
            ObjectGuid Guid;                        // the member, for a press on its frame
        };
        std::array<PartyFrame, GROUP_MEMBERS> Frames{};
        float MinimapYards = 60.0f;

        /// **What the goal names** (M3 interact; the sight block's named row): the kind of thing the seat is to find
        /// or act on -- its semantic class (Vision::Class), its template entry and whether it is a game object, as a
        /// quest's log names its objective -- and how (NamedTask: 1 reach it, 2 use it, 3 use the key item on it; 0
        /// nothing named). Never where it is.
        uint8 NamedTask = 0;
        uint8 NamedClass = 0;
        uint32 NamedEntry = 0;
        bool NamedObject = false;

        // Travel: where the seat is going, and whether it may ride there.
        bool HasObjective = false;
        /// How near a camera ray has to pass the Objective to flag it (Vision::ObjectiveFlag): Vision::OBJECTIVE_RADIUS,
        /// or a seek object's own (Vision::ObjectiveRadiusFor its SeekObject::Radius), so the flag sits on the object.
        float ObjectiveRadius = 1.0f;
        Position Objective;
        /// The compass is withheld this episode (M1's withholding ladder, SightEncounter): the CompassBlock reads as
        /// absent -- its presence column and every value 0 -- as a player with no quest arrow. Only that input goes:
        /// the objective is still the camera's to flag, and the critic's state still has it.
        bool CompassWithheld = false;
        /// Whether the seat is told where the Objective is (a compass shown, or the travel block's bearing): without
        /// it the goal block gives a trip's objective no place, so no observed goal bit says how near it is
        /// (GoalBlock::PlaceOf). Arrival is the encounter's to decide and pay.
        bool ObjectivePlaceKnown = true;
        /// How much longer the walking way round to the objective is than the straight line to it, as a ratio;
        /// 0 without an objective and 1 when the straight line is the route. Measured on foot at the episode's
        /// build, water and magma excluded, so it is what the ground costs rather than what the pathfinder would
        /// permit -- a player's filter admits both and would call a lake a straight shot.
        float Detour = 0.0f;
        /// Whether the legs are getting anywhere, over about the last second: how far the seat moved against how
        /// far running would have carried it, and the share of the distance to the objective that closed.
        float MoveRate = 0.0f;
        float CloseRate = 0.0f;
        /// How near counts as arrived, which is not the same number indoors as it is in open country. Carried
        /// on the view so OBS_AT_OBJECTIVE, the masks that ask whether the seat is there yet, and the reward
        /// that pays for arriving all read one answer.
        float ArriveWithin = 6.0f;

        /// A dungeon's way on (WorldView), as the goal head names it.
        WorldView World;

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
        /// The sight block's presses (EntityActions): a selection, an assist or a focus taken, and a press refused
        /// before it was sent or by the server's cast checks (EntityActions::Refusal, 0 none), priced as
        /// Actions.Aimless.ActRefused.
        uint32 Selections = 0;
        uint8 ActRefused = 0;
        /// The entity a sight press named once it was found (sent or refused), and the press (EntityActions::Press):
        /// what an encounter judges the press by (M3 interact: the right object, the lever, the key's lock).
        ObjectGuid ActedOn;
        uint8 ActPress = 0;
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
