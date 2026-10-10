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

#include "Layout.h"
#include "ClassAssets.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StageDefinition.h"
#include "StringFormat.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace
{
    /// Manifest format: 3 lists blocks generically (format 2 had one fixed field per stage block).
    // 5: the move block steers in three dimensions (a held yaw and pitch, the ground read along each bearing, and
    // water), and the travel block gave up the point order and the two climb hops that went with it. Every layout
    // changed shape, so a manifest of an earlier format describes a model that no longer fits.
    /// 6: the pathfinder's choices left the actor -- the travel block's follow-route and the route features, the
    /// move block's face-objective, the duel block's target-relative moves and their two option clocks -- the
    /// ground is sensed along sixteen rays instead of eight, and the move block carries a trail of where the
    /// seat has been. Every block of every layout changed shape; no format 5 checkpoint fits a format 6 layout.
    /// 7: the jump can drop off a ledge, and the move block says how far the landing is below the seat
    /// (OBS_JUMP_DROP) and whether it is in the air (OBS_FALLING); Slow Fall and Levitate joined the mage's and
    /// the priest's catalogs. The move block changed shape for every layout, the core block for those two.
    /// 8: the move block is a player's keys and mouse driving the player controller (move revision 2, 25 actions;
    /// player-controller C3), and the core block lost the move options' three clocks (core revision 1). Every layout
    /// changed shape: a runtime holding a format 7 model must refuse it, not read it through this layout.
    /// 9: the seats' engine moves are gone (player-controller C9): the crowd block's advance and approach, the party
    /// block's follow-the-tank, the companion block's follow and its clock, the death block's corpse run (each block at
    /// revision 1). Layouts with any of those blocks changed shape.
    /// 10: the goal space has a tenth kind (search; goal block revision 5): the goal block is two columns wider in
    /// every layout, and an exported goal head has ten kinds.
    constexpr uint32 MANIFEST_FORMAT = 10;
}

std::string_view Animus::Curriculum::GoalName(SeatGoal goal)
{
    switch (goal)
    {
        case SeatGoal::Fight:    return "fight";
        case SeatGoal::Control:  return "control";
        case SeatGoal::Recover:  return "recover";
        case SeatGoal::Protect:  return "protect";
        case SeatGoal::Position: return "position";
        case SeatGoal::Prepare:  return "prepare";
        case SeatGoal::TravelTo: return "travel_to";
        case SeatGoal::Rest:     return "rest";
        case SeatGoal::Resurrect: return "resurrect";
        case SeatGoal::Search:   return "search";
        case SeatGoal::Count:    break;
    }

    return "unknown";
}

bool Animus::Curriculum::GoalAccepts(SeatGoal kind, uint32 target)
{
    bool const none = target == GOAL_TARGET_NONE;
    bool const enemy = target >= GOAL_TARGET_ENEMY_FIRST && target < GOAL_TARGET_FRIEND_FIRST;
    bool const friendly = target >= GOAL_TARGET_FRIEND_FIRST && target < GOAL_TARGET_PLACE_FIRST;
    bool const place = target >= GOAL_TARGET_PLACE_FIRST && target < GOAL_TARGET_ASSIGNMENT;
    switch (kind)
    {
        case SeatGoal::Fight:
        case SeatGoal::Position: return none || enemy;
        case SeatGoal::Control:  return enemy;
        case SeatGoal::Protect:  return friendly;
        case SeatGoal::TravelTo: return place || target == GOAL_TARGET_ASSIGNMENT;
        case SeatGoal::Search:   return place;
        case SeatGoal::Recover:
        case SeatGoal::Prepare:
        case SeatGoal::Rest:     return none;
        case SeatGoal::Resurrect: return none || friendly;
        case SeatGoal::Count:    break;
    }
    return false;
}

std::string_view Animus::Curriculum::BlockName(BlockId id)
{
    switch (id)
    {
        case BlockId::Core:      return "core";
        case BlockId::Move:      return "move";
        case BlockId::Compass:   return "compass";
        case BlockId::Duel:      return "duel";
        case BlockId::Pack:      return "pack";
        case BlockId::Gauntlet:  return "gauntlet";
        case BlockId::Pet:       return "pet";
        case BlockId::Vision:    return "vision";
        case BlockId::Entities:  return "entities";
        case BlockId::Map:       return "map";
        case BlockId::Sight:     return "sight";
        case BlockId::PartyFrames: return "party_frames";
        case BlockId::Combat:    return "combat";
        case BlockId::Goal:      return "goal";
        case BlockId::Count:     break;
    }

    return "unknown";
}

Animus::Curriculum::Layout Animus::Curriculum::Layout::Build(ClassProfile const& profile,
    StageDefinition const& stage)
{
    Layout layout;
    layout.Stage = &stage;
    layout.Profile = &profile;
    layout.Assets = &ClassAssets::For(profile);
    layout.Blocks = stage.Blocks;

    // Each block starts where the previous one ended.
    for (BlockId id : layout.Blocks)
    {
        BlockSize const size = GetBlock(id).Size(layout);
        layout.Slices[std::size_t(id)] = { layout.ObsDim, size.Obs, layout.NumActions, size.Actions };
        layout.ObsDim += size.Obs;
        layout.NumActions += size.Actions;
        layout._blockMask |= 1u << uint32(id);
    }

    layout.ModeGroups.assign(layout.NumActions, 0);
    for (BlockId id : layout.Blocks)
    {
        BlockSlice const& slice = layout.Slice(id);
        for (uint32 local = 0; local < slice.ActionCount; ++local)
            layout.ModeGroups[slice.ActionFirst + local] = uint8(GetBlock(id).ModeGroupOf(layout, local));
    }

    return layout;
}

std::optional<Animus::Curriculum::BlockId> Animus::Curriculum::Layout::BlockOfAction(uint32 action) const
{
    for (BlockId id : Blocks)
        if (Slice(id).ContainsAction(action))
            return id;

    return std::nullopt;
}

boost::json::array Animus::Curriculum::Span(uint32 first, uint32 count)
{
    return { first, count };
}

std::string Animus::Curriculum::Layout::ModelName() const
{
    return Profile->Name + Stage->Suffix;
}

std::vector<std::string> Animus::Curriculum::Layout::ActionNames() const
{
    std::vector<std::string> names(NumActions);
    for (BlockId id : Blocks)
    {
        BlockSlice const& slice = Slice(id);
        for (uint32 local = 0; local < slice.ActionCount && slice.ActionFirst + local < NumActions; ++local)
        {
            std::string name = GetBlock(id).ActionName(*this, local);
            names[slice.ActionFirst + local] = name.empty()
                ? Acore::StringFormat("{}_{}", BlockName(id), local) : std::move(name);
        }
    }

    return names;
}

std::string Animus::Curriculum::Layout::Manifest() const
{
    boost::json::object manifest;
    manifest["format"] = MANIFEST_FORMAT;
    manifest["model"] = ModelName();
    manifest["stage"] = Stage->Name;
    manifest["class_name"] = Profile->Name.c_str();
    manifest["class"] = Profile->Class;
    manifest["obs_dim"] = ObsDim;
    manifest["num_actions"] = NumActions;

    boost::json::array& actionNames = manifest["action_names"].emplace_array();
    for (std::string const& name : ActionNames())
        actionNames.push_back(boost::json::string(name));

    // Every build the class can have, because one model plays all of them, and what each one can do -- there is
    // no role to name, and the aptitude is the thing a reader of the manifest actually wants.
    boost::json::array& specs = manifest["specs"].emplace_array();
    {
        ClassAssets const& assets = ClassAssets::For(*Profile);
        for (uint8 index = 0; index < uint8(Profile->Specs.size()); ++index)
        {
            SpecProfile const& spec = Profile->Specs[index];
            boost::json::object entry;
            entry["name"] = spec.Name;
            entry["tree"] = spec.TabPage;

            boost::json::object aptitude;
            if (index < assets.SpecAptitudes.size())
                for (uint32 feature = 0; feature < Aptitude::COUNT; ++feature)
                    aptitude[Aptitude::FeatureName(feature)] = double(assets.SpecAptitudes[index][feature]);
            entry["aptitude"] = std::move(aptitude);
            specs.push_back(std::move(entry));
        }
    }

    boost::json::array& blocks = manifest["blocks"].emplace_array();
    for (BlockId id : Blocks)
    {
        BlockSlice const& slice = Slice(id);
        boost::json::object& block = blocks.emplace_back(boost::json::object()).get_object();
        block["name"] = BlockName(id);
        block["obs"] = Span(slice.ObsFirst, slice.ObsCount);
        block["actions"] = Span(slice.ActionFirst, slice.ActionCount);
        if (uint32 const revision = GetBlock(id).Revision())
            block["revision"] = revision;
        GetBlock(id).DescribeManifest(*this, block);
    }

    return boost::json::serialize(manifest);
}
