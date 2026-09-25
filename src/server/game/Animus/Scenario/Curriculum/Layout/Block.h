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
        Core,           // the character, its spells, trinkets and talents
        Move,           // where it puts its feet, with no reference to a target: bearings and facing
        Duel,           // movement, auto-attack, pets, stopping casts and forms, the opponent's position
        Pack,           // enemy slots, target selection, tactical spells
        Gauntlet,       // pull timing, food, drink, sustain spells
        Companion,      // the owner: follow, assist, guard, heal it
        Party,          // three teammates: follow the tank, assist, guard and heal them
        Pvp,            // the enemy player's class, role and state
        Context,        // the situation: allies, hostile players and creatures, PvP flag, map kind (no actions)
        Hostiles,       // per enemy slot: player or creature, class, healing, stealth, pet (no actions)
        Pet,            // the pet bar: abilities, stance, follow and stay (classes with a controllable pet)
        Travel,         // mounts, flying and an objective to get to
        Flag,           // a flag match: both flags, both bases, the score (no actions)
        Support,        // friends (self, owner, teammates) to heal, shield and buff, and the heals' rank tier
        Order,          // what the side's director asked of this seat (no actions: an order is advice, not a lever)
        World,          // life outside the fight: corpses, quest givers, nodes, vendors, bags, gold, gear
        Count
    };

    constexpr std::size_t BLOCK_COUNT = std::size_t(BlockId::Count);

    /// What a director asks of its side. Four channels: the posture the team holds, the enemy it concentrates
    /// on, the shape it takes, and which seat owes the next duty. A seat reads them and still chooses its own
    /// actions -- an order is advice, and a seat that has learned better is free to ignore it.
    enum class TeamPosture : uint8
    {
        Attack,         // press the enemy
        Defend,         // hold what the side has
        Protect,        // keep one of its own alive
        Recover,        // disengage, heal, drink
        Regroup,        // gather before anything else
        Hold,           // stay where the side was put, at the called place: do not chase
        Count
    };

    enum class TeamRally : uint8
    {
        None,           // no shape asked for
        OwnBase,        // home: the flag room, the graveyard, the safe side
        EnemyBase,      // theirs
        Carrier,        // whoever of the side carries the objective
        Focus,          // on the called target
        Spread,         // away from each other
        Stack,          // together
        Point,          // the place the director named (SideOrder::Place, from an anchor and an offset)
        Count
    };

    constexpr uint32 TEAM_POSTURE_COUNT = uint32(TeamPosture::Count);
    constexpr uint32 TEAM_RALLY_COUNT = uint32(TeamRally::Count);

    /// How a director names a spot without an action space the size of the world.
    ///
    /// A place is an anchor, an offset from it and how far: "behind the flag room", "pull back from the
    /// target", "twenty yards left of where we are". Three small categorical fields the director edits one at
    /// a time, exactly as it edits the rest of a standing order, so the whole vocabulary is 13 actions and
    /// stays 13 whether the map is an arena or a continent -- only the ring radii change.
    ///
    /// The offset is relative to an axis the side can actually perceive, not a compass bearing. A director has
    /// no idea which way north is: its observation carries distances, and (since the bearing features beside
    /// this) angles relative to its own side, but nothing that orients it to the map. "Sixty yards north" would
    /// be a direction it could not learn to use; "sixty yards back from them" is one it can.
    enum class PlaceAnchor : uint8
    {
        TeamCentre,     // where the side is now
        Focus,          // the enemy it called
        LastSeenEnemy,  // where it last saw one, which is the only place a scout has to go on
        Objective,      // what the arena is about, when it has one
        OwnBase,
        EnemyBase,
        Count
    };

    /// Along the axis from the anchor towards the enemy (falling back to the objective, then the last
    /// sighting, then the side's own facing when it knows of no enemy at all).
    enum class PlaceOffset : uint8
    {
        At,             // the anchor itself
        Toward,
        Away,
        Left,
        Right,
        Count
    };

    enum class PlaceRing : uint8 { Near, Far, Count };

    constexpr uint32 PLACE_ANCHOR_COUNT = uint32(PlaceAnchor::Count);
    constexpr uint32 PLACE_OFFSET_COUNT = uint32(PlaceOffset::Count);
    constexpr uint32 PLACE_RING_COUNT = uint32(PlaceRing::Count);

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
    constexpr uint32 RAID_GROUPS = 8;       // a raid's groups
    constexpr uint32 GROUP_SEATS = 5;       // seats in a group: a party is one of them
    /// Learned agents per env: 1, an arena's 2, a party's 1-5, or a raid's groups of five.
    constexpr uint32 MAX_SEATS = RAID_GROUPS * GROUP_SEATS;
    constexpr uint32 TEAM_SEATS = 10;       // a battleground side: Warsong Gulch as it is played
    constexpr uint32 TEAM_COUNT = 2;
    constexpr uint32 TEAM_MATCH_SEATS = TEAM_SEATS * TEAM_COUNT;
    constexpr uint32 NO_SEAT = 0xFFFFFFFF;
    constexpr uint32 GROUP_MEMBERS = GROUP_SEATS - 1;    // the seat's own group, itself aside
    /// Raiders outside the seat's group that it still has to act on: the raid's main tank, its most hurt member,
    /// and the nearest one. Empty in every stage below a raid, where the group is the whole of it.
    constexpr uint32 SPOTLIGHT_SLOTS = 3;
    /// Teammate slots a seat observes and acts on (PartyBlock). Bounded on purpose: a raider heals, assists and
    /// guards its own group and a few named others, never 39 people, and a slot is 36 features and three actions.
    constexpr uint32 PARTY_MEMBERS = GROUP_MEMBERS + SPOTLIGHT_SLOTS;
    constexpr uint32 PACK_SLOTS = 4;        // enemies observed
    /// Rays the movement block senses the ground along: twice the bearings it can walk, because a gap between
    /// two 45-degree bearings is visible at 22.5 degrees and not at 45 (GroundProbe, MoveBlock::RAY_COUNT).
    constexpr uint32 SENSE_RAYS = 16;
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
        Count
    };

    constexpr uint32 GOAL_COUNT = uint32(SeatGoal::Count);
    constexpr int32 NO_GOAL = -1;

    [[nodiscard]] std::string_view GoalName(SeatGoal goal);
    [[nodiscard]] std::string_view PostureName(TeamPosture posture);
    [[nodiscard]] std::string_view RallyName(TeamRally rally);
    [[nodiscard]] std::string_view AnchorName(PlaceAnchor anchor);
    [[nodiscard]] std::string_view OffsetName(PlaceOffset offset);
    [[nodiscard]] std::string_view RingName(PlaceRing ring);
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

        [[nodiscard]] virtual BlockId Id() const = 0;

        /// The features and actions the block adds to `layout` (profile, assets and ally heals are set).
        [[nodiscard]] virtual BlockSize Size(Layout const& layout) const = 0;

        /// Block-specific manifest entries (spell lists, slot counts), written inside the block's manifest object.
        virtual void DescribeManifest(Layout const& /*layout*/, boost::json::object& /*block*/) const { }

        /// Write the block's features and action mask for a living bot: `obs` and `mask` point at the block's slice.
        /// `mask` is null when no mask is wanted (an ended episode's final observation): skip the cast checks.
        virtual void Observe(SeatView const& view, float* obs, uint8* mask) const = 0;

        /// Before an action of a layout with this block is applied (whichever block the action belongs to).
        /// Every decision before the chosen action, whatever it is: where a durative action (SeatOption) acts. What
        /// it does is recorded in `result` as a press would be.
        virtual void BeforeApply(SeatView& /*view*/, SeatActionResult& /*result*/) const { }

        /// Apply the block's action `local` (0-based within the block) as the client would. Masked actions do nothing.
        virtual void Apply(SeatView& /*view*/, uint32 /*local*/, SeatActionResult& /*result*/) const { }

        /// Whether action `local` is a movement order, which is paced by CurriculumTuning::ActionTuning::MoveRepeatMs
        /// rather than RepeatMs: steering has to be re-issued more often than a spell or an order.
        [[nodiscard]] virtual bool IsMovement(uint32 /*local*/) const { return false; }

        /// Whether action `local` aims the seat rather than moving its feet: a held turn or pitch (MoveBlock). Aiming
        /// is movement for pacing, and it is the one kind of movement that does not take the feet over -- a player
        /// looks round while walking -- so SeatEncoder::Apply asks this before it ends a held bearing.
        [[nodiscard]] virtual bool IsAiming(uint32 /*local*/) const { return false; }

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
}

#endif
