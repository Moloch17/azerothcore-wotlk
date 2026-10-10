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

#ifndef ANIMUS_LIB_CURRICULUM_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_BLOCK_H

#include "Define.h"
#include <array>
#include <boost/json/fwd.hpp>
#include <string>
#include <string_view>

/*
 * A layout block: one group of observation features and actions of a class/role policy (the class's spells, the
 * duel's movement, the pack's enemy slots, ...). A stage is an ordered list of blocks (see StageDefinition), and a
 * layout places each block's features and actions after the previous block's (see Layout).
 *
 * Blocks are stateless: everything they read comes from the world and the SeatView, so the same code encodes a seat
 * in training and a companion in play.
 */
namespace Animus::Curriculum
{
    struct Layout;
    struct SeatActionResult;
    struct SeatView;

    enum class BlockId : uint8
    {
        Core = 0,           // the character, its spells, trinkets and talents
        Move = 1,           // where it puts its feet, with no reference to a target: bearings and facing
        /// Where the objective is: whether there is one, its bearing and its distance, straight-line (no actions).
        /// Split from the move block (perception-goals P1): M1 carries it, M2 seek finds its objective by sight.
        Compass = 2,
        Duel = 3,           // movement, auto-attack, pets, stopping casts and forms, the opponent's position
        Pack = 4,           // enemy slots, target selection, tactical spells
        Gauntlet = 5,       // pull timing, food, drink, sustain spells
        Pet = 11,            // the pet bar: abilities, stance, follow and stay (classes with a controllable pet)
        /// What the seat's camera sees: a depth-and-meaning image and the camera's scalars (camera-vision; no
        /// actions). Before the goal block, which stays last.
        Vision = 20,
        /// What the camera's last frame showed, as a list of the entities in it (perception-goals 1b; no actions).
        /// Right after the vision block, which every stage with one is given it with (CurriculumStages).
        Entities = 21,
        /// What the seat remembers of the place: its mental map, as one egocentric heading-up crop (perception-goals
        /// REDESIGN §3; no actions). After the camera, whose frames write it.
        Map = 22,
        /// What the seat sees and remembers, and acting on it (dungeon-curriculum I1 and I2): the frame's visible
        /// entities and the most relevant remembered ones, as one list, and pointer presses on it -- select, interact
        /// or use, use an item on, assist, focus -- sent as the client sends them. After the entities block, which
        /// writes the memory it reads.
        Sight = 23,
        /// What a player's UI shows of its party (dungeon-curriculum I5; revision 2, G1: the one source of
        /// party-member state): the party frames -- each member's health, power, alive, in combat, the leader, its
        /// debuffs, aggro and target -- and the minimap's party dots, a position only for a member within the
        /// minimap's radius. Presses: select, focus or assist a member's frame. Only the stages that declare it
        /// (move4_follow on).
        PartyFrames = 24,
        /// Perception-true combat inputs (dungeon-curriculum I3): the player frame and the pet frame (always known;
        /// the party's members are the party frames block's since its revision 1), the target frame's threat
        /// indicator and target of target, and -- in a layout with it -- the sight list's per-target combat columns
        /// (the cast bar, crowd control, elite, whom it hits, the seat's threat on it: visible units only). Presses:
        /// select or focus the player frame or the pet frame. After the sight block.
        Combat = 25,
        Goal = 26,           // which goal kinds and targets are there, and whether the goal held ended (no actions; last)
        Count = 27
    };

    /// **The ids are explicit and never reused.** A manifest, a checkpoint and the learner's seeding by name know a block
    /// by its place in this list; the first curriculum's blocks were deleted (2026-10-07) and left their numbers unused,
    /// so the live blocks keep the ids they have always had. A new block takes a number past the last one.
    constexpr std::size_t BLOCK_COUNT = std::size_t(BlockId::Count);

    /// Kinds of standing choice a player makes and keeps (SeatMemory: a change of one kind holds for a while).
    enum class ModeGroup : uint8
    {
        None,
        Form,           // stances, forms, presences, Shadowform
        Aspect,         // hunter aspects
        Aura,           // paladin auras
        Seal,           // paladin seals
        Armor,          // mage and warlock armors, shaman shields
        PetStance,      // passive, defensive, aggressive
        RankTier,       // which rank a rankable spell is cast at (CoreBlock::ACTION_RANK_TIERS)
        Count
    };

    // Sizes several blocks and the scenario agree on.
    constexpr uint32 RAID_GROUPS = 8;       // a raid's groups (a bound: no stage fields a raid)
    constexpr uint32 GROUP_SEATS = 5;       // seats in a group: a party is one of them
    /// Learned agents per env: 1, or a party's 1-5 (one group); a raid's groups of five are the bound.
    constexpr uint32 MAX_SEATS = RAID_GROUPS * GROUP_SEATS;
    constexpr uint32 TEAM_SEATS = 10;       // a battleground side: Warsong Gulch as it is played
    constexpr uint32 TEAM_COUNT = 2;
    constexpr uint32 NO_SEAT = 0xFFFFFFFF;
    constexpr uint32 GROUP_MEMBERS = GROUP_SEATS - 1;    // the seat's own group, itself aside
    /// Raiders outside the seat's group that it still has to act on: the raid's main tank, its most hurt member,
    /// and the nearest one. Empty in every stage, where the group is the whole of it.
    constexpr uint32 SPOTLIGHT_SLOTS = 3;
    /// Teammate slots a seat observes and acts on (PartyBlock). Bounded on purpose: a raider heals, assists and
    /// guards its own group and a few named others, never 39 people, and a slot is 36 features and three actions.
    constexpr uint32 PARTY_MEMBERS = GROUP_MEMBERS + SPOTLIGHT_SLOTS;
    /// Enemies observed one by one (PackBlock, HostilesBlock: the seat sets' enemies, picked by pointer). A dungeon's
    /// fights had a median of eight creatures on the party and raids and battlegrounds have more; four left half of
    /// a pull out of sight (2026-10-03). The slots are in order of what matters to the party (the encounters keep
    /// them so: PullsEncounter::OrderCamp), and the crowd block counts what is past them.
    constexpr uint32 PACK_SLOTS = 24;
    /// The first enemy slots the other blocks name by index -- a teammate's target and the slots on it (PartyBlock),
    /// the owner's (CompanionBlock), the tank's (CrowdBlock) and a goal's enemy (GoalTarget): the four that matter
    /// most, the tank's target first. Fixed apart from PACK_SLOTS so those blocks keep their sizes and seeded weights
    /// when the observed slots grow; a target past them reads as none of these.
    constexpr uint32 NAMED_ENEMY_SLOTS = 4;
    /// The scale an enemy count is observed at (living enemies, attackers on a seat): what PACK_SLOTS was when those
    /// features were trained, so a seeded policy reads them as it did. Not a cap: eight attackers read 2.
    constexpr float ENEMY_COUNT_SCALE = 4.0f;
    constexpr uint32 CROWD_SLOTS = 4;       // enemies past the pack's slots, observed one by one (CrowdBlock)
    /// The sight block's list (SightBlock): the camera's visible entities (Vision::ENTITY_SLOTS) and as many of the
    /// most relevant remembered ones not visible now; a press names a slot of it.
    constexpr uint32 SIGHT_VISIBLE_SLOTS = 32;
    constexpr uint32 SIGHT_RECALLED_SLOTS = 32;
    constexpr uint32 SIGHT_SLOTS = SIGHT_VISIBLE_SLOTS + SIGHT_RECALLED_SLOTS;
    /// Positions the movement block remembers of where the seat has been (MovementTrail), one a second.
    constexpr uint32 TRAIL_SAMPLES = 8;
    constexpr uint32 STABLE_SLOTS = 4;      // a hunter's stabled beasts
    /// Friends a seat heals, shields and buffs (SupportBlock): itself, the owner, the teammate slots.
    constexpr uint32 FRIEND_SLOTS = 2 + PARTY_MEMBERS;
    constexpr uint32 FRIEND_SELF = 0;
    constexpr uint32 FRIEND_OWNER = 1;
    constexpr uint32 FRIEND_TEAMMATE_FIRST = 2;
    /// Rank tiers of a heal with ranks: the highest known, about two thirds up the known ranks, about a third up.
    constexpr uint32 RANK_TIERS = 3;

    /// What a seat is trying to do over the next few seconds. The learner's goal head picks one every
    /// mappo.goal_every_decisions and keeps it until the next choice (MappoConfig), and sends it with the actions;
    /// the sim scores whether the seat's decisions match it (StageScenario::GoalHeld), pays Goals.Match for the ones
    /// that do, reports how each goal was used, and shows a party its teammates' goals. A goal is a statement of
    /// intent, not an order: nothing is masked by it.
    enum class SeatGoal : uint8
    {
        Fight,          // damage the enemy it is fighting
        Control,        // hold the other enemies out of the fight
        Recover,        // heal, eat or drink itself back up
        Protect,        // keep the owner or a teammate alive
        Position,       // get to where its spec fights from
        Prepare,        // buffs, summons and stealth before the fight
        TravelTo,       // get to a place: a route place, the assigned area or a trip's objective
        Rest,           // eat and drink out of a fight until ready
        /// Stand up again after dying: at the own corpse (target none) or a dead member raised (a friend slot).
        /// In the goal space from the next-run format on; offered only once death runs exist (Wave 6).
        Resurrect,
        /// Look for an object in or at a place (goal block revision 5, search-kind, 2026-10-10): a room slot, the way
        /// on or a block of the seat's own map. Not a fight (no enemy is named) and not a trip for its own sake
        /// (TravelTo); the place is where the seat means to look, so it is reached when the place is checked (a room)
        /// or stood at (a cell). Appended: the kinds before it keep their ids.
        Search,
        Count
    };

    constexpr uint32 GOAL_COUNT = uint32(SeatGoal::Count);
    constexpr int32 NO_GOAL = -1;

    [[nodiscard]] std::string_view GoalName(SeatGoal goal);

    /// **What a goal is about** (Component C): a goal is a kind and a target, sent as one number, kind *
    /// GOAL_TARGETS + target. The targets are one space for every kind -- nothing, an enemy slot, a friend slot, a
    /// route place, the assigned area -- and GoalAccepts says which a kind can take. (Goal block revision 1: the
    /// Loot, Gather and Interact kinds and the journal objective, giver and turn-in targets were never offered, and
    /// are gone.) The sim tells the learner which kinds and targets are there each decision (GoalBlock), so a
    /// goal can only be chosen about something that exists.
    enum GoalTarget : uint32
    {
        GOAL_TARGET_NONE        = 0,
        GOAL_TARGET_ENEMY_FIRST = 1,
        GOAL_TARGET_FRIEND_FIRST = GOAL_TARGET_ENEMY_FIRST + NAMED_ENEMY_SLOTS,
        GOAL_TARGET_PLACE_FIRST = GOAL_TARGET_FRIEND_FIRST + FRIEND_SLOTS,
        GOAL_TARGET_ASSIGNMENT  = GOAL_TARGET_PLACE_FIRST + 8,
        GOAL_TARGETS
    };

    constexpr uint32 GOAL_JOINT_COUNT = GOAL_COUNT * GOAL_TARGETS;

    /// The seek stage's room goals (M2): place targets 0..GOAL_ROOM_SLOTS-1 are rooms, the next one the way on; the
    /// last place target is never offered. The goal block publishes GOAL_PLACE_SLOTS slots of features.
    constexpr uint32 GOAL_ROOM_SLOTS = 6;
    constexpr uint32 GOAL_PLACE_SLOTS = GOAL_ROOM_SLOTS + 1;

    [[nodiscard]] constexpr int32 MakeGoal(SeatGoal kind, uint32 target)
    {
        return int32(uint32(kind) * GOAL_TARGETS + target);
    }

    /// **A cell goal** (free choice goals, 2026-10-09; goal block revision 4): the joint goal search / place_7 (travel_to
    /// / place_7 before goal block revision 5) -- the last place target, which the room goals never offer -- plus a cell
    /// of the seat's own mental-map crop, chosen by the learner's pointer head and sent beside it (CellGrid, ACT's cell
    /// words). Derived: Search * GOAL_TARGETS + GOAL_CELL_TARGET = 9 * 23 + 21 = 228; stage.json goals.cells.joint
    /// carries it to the learner. At Seek.SearchGoals 0 the cell goal is travel_to / place_7 instead (159) and the
    /// manifest says so; GoalBlock::IsCellGoal accepts either.
    constexpr uint32 GOAL_CELL_TARGET = GOAL_TARGET_PLACE_FIRST + GOAL_PLACE_SLOTS;
    constexpr int32 GOAL_CELL_JOINT = MakeGoal(SeatGoal::Search, GOAL_CELL_TARGET);
    /// The kind of a goal (NO_GOAL stays NO_GOAL as -1 compares), and its target.
    [[nodiscard]] constexpr int32 GoalKindOf(int32 goal) { return goal < 0 ? NO_GOAL : goal / int32(GOAL_TARGETS); }
    [[nodiscard]] constexpr uint32 GoalTargetOf(int32 goal) { return goal < 0 ? 0 : uint32(goal) % GOAL_TARGETS; }
    /// Whether a kind can be about a target: Fight, Control and Position about an enemy (Fight and Position about
    /// no one in particular too), Protect about a friend, TravelTo about any route place or the assignment,
    /// Search about a place (a room slot, the way on or the cell target; never the assignment),
    /// Resurrect about no one or a friend; Recover, Prepare and Rest about nothing.
    [[nodiscard]] bool GoalAccepts(SeatGoal kind, uint32 target);
    /// The episode clock's scale: the longest arena's episode, so it rises through every episode instead of
    /// saturating. Elapsed time, not the fraction of an episode's own limit: a companion has no limit, and the
    /// critic already sees the fraction (StageScenario::STATE_EPISODE_TIME).
    constexpr float EPISODE_TIME_SCALE_MS = 300000.0f;

    [[nodiscard]] std::string_view BlockName(BlockId id);

    /// A block's place in a layout's observation row and action mask.
    struct BlockSlice
    {
        uint32 ObsFirst = 0;
        uint32 ObsCount = 0;
        uint32 ActionFirst = 0;
        uint32 ActionCount = 0;

        [[nodiscard]] bool ContainsAction(uint32 action) const
        {
            return action >= ActionFirst && action < ActionFirst + ActionCount;
        }
    };

    struct BlockSize
    {
        uint32 Obs = 0;
        uint32 Actions = 0;
    };

    class Block
    {
    public:
        virtual ~Block() = default;

        /// The features and actions the block adds to `layout` (profile, assets and ally heals are set).
        [[nodiscard]] virtual BlockSize Size(Layout const& layout) const = 0;

        /// Bumped when the block's columns change meaning at the same place (a re-layout, not new features at the
        /// end): written to the manifest and stage.json when not 0, so a model or checkpoint of the old layout is
        /// told apart from one of the new even where the widths agree.
        [[nodiscard]] virtual uint32 Revision() const { return 0; }
        /// Columns of the block whose scale changed in place, each {tag, first (relative to the block), count}: the
        /// learner's seeding starts their normaliser statistics afresh when the parent's block lacks the tag, and
        /// keeps the rest of the block (bootstrap._seed_rescaled_norms). A Revision is for columns that changed
        /// meaning; this is for a reading that changed its scale.
        virtual void DescribeRescaled(Layout const& /*layout*/, boost::json::array& /*out*/) const { }

        /// Each observation column's name, in order (stage.json obs_names), for a block whose columns may move to
        /// another block or revision: the learner's seeding maps named columns across by name (bootstrap), where a
        /// changed revision would otherwise start the block fresh. Empty (the default) for a block that does not say.
        virtual void DescribeColumns(Layout const& /*layout*/, boost::json::array& /*names*/) const { }

        /// Block-specific manifest entries (spell lists, slot counts), written inside the block's manifest object.
        virtual void DescribeManifest(Layout const& /*layout*/, boost::json::object& /*block*/) const { }

        /// Write the block's features and action mask for a living bot: `obs` and `mask` point at the block's slice.
        /// `mask` is null when no mask is wanted (an ended episode's final observation): skip the cast checks.
        virtual void Observe(SeatView const& view, float* obs, uint8* mask) const = 0;

        /// Before an action of a layout with this block is applied (whichever block the action belongs to).
        /// Every decision before the chosen action, whatever it is: where a durative action (SeatOption) acts. What
        /// it does is recorded in `result` as a press would be.
        virtual void BeforeApply(SeatView& /*view*/, SeatActionResult& /*result*/) const { }
        /// The action is applied before every block's BeforeApply rather than after: a press that must meet the
        /// world as the seat saw it (a spell: facing, range, the global cooldown).
        [[nodiscard]] virtual bool PressesFirst(Layout const& /*layout*/, uint32 /*local*/) const { return false; }

        /// Apply the block's action `local` (0-based within the block) as the client would. Masked actions do nothing.
        virtual void Apply(SeatView& /*view*/, uint32 /*local*/, SeatActionResult& /*result*/) const { }

        /// Whether action `local` is a movement order, which is paced by CurriculumTuning::ActionTuning::MoveRepeatMs
        /// rather than RepeatMs: steering has to be re-issued more often than a spell or an order.
        [[nodiscard]] virtual bool IsMovement(uint32 /*local*/) const { return false; }

        /// The kind of standing choice action `local` makes, if any (ModeGroup).
        [[nodiscard]] virtual ModeGroup ModeGroupOf(Layout const& /*layout*/, uint32 /*local*/) const
        {
            return ModeGroup::None;
        }

        /// A readable name for action `local`, for the manifest and the evaluation's per-action counts; empty for
        /// the default, "<block>_<local>".
        [[nodiscard]] virtual std::string ActionName(Layout const& /*layout*/, uint32 /*local*/) const { return {}; }
    };

    /// The block implementation of `id`.
    [[nodiscard]] Block const& GetBlock(BlockId id);

    /// A seat layout's entities as sets, for the learner's shared set encoders and pointer heads (peak-play W4,
    /// stage.json layouts.<name>.sets): per set its name, slot count, the column of a slot's "present" feature, the
    /// observation segments a slot is gathered from (each `first` column of slot 0 and `stride` columns a slot, read
    /// in order and concatenated) and the action ranges that name its slots (`first` global action, `count` = slots).
    /// The enemies are the pack block's slots joined with the hostiles block's for the same slot; the members the
    /// party block's teammates; the friends the support block's; the crowd the crowd block's. Empty for a layout with
    /// none of them.
    void DescribeSeatSets(Layout const& layout, boost::json::array& sets);
}

#endif
