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
#include "DirectorLayout.h"
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
    constexpr uint32 MANIFEST_FORMAT = 7;

    /// The catalog's long buffs, grouped by what a unit can have at once: chains joined when any of their ranks share
    /// a spell group (spell_group, whose stack rules keep one of them per target) or an exclusive kind (a seal, an
    /// armor, an elemental shield: SpellInfo::GetSpellSpecific), each group listing every rank.
    std::vector<std::vector<uint32>> BuffGroupsOf(Animus::Curriculum::ActionCatalog const& catalog)
    {
        struct Chain
        {
            std::vector<uint32> Ranks;
            std::set<uint32> Groups;
            SpellSpecificType Specific = SPELL_SPECIFIC_NORMAL;
        };

        std::vector<Chain> chains;
        for (Animus::Curriculum::ActionCatalog::Action const& action : catalog.Actions())
        {
            if (action.Type != Animus::Curriculum::ActionCatalog::Kind::Spell || !action.LongBuff)
                continue;

            Chain chain;
            if (SpellInfo const* first = sSpellMgr->GetSpellInfo(action.FirstRank))
                chain.Specific = first->GetSpellSpecific();
            for (SpellInfo const* rank = sSpellMgr->GetSpellInfo(action.FirstRank); rank;
                rank = rank->GetNextRankSpell())
            {
                chain.Ranks.push_back(rank->Id);
                auto const bounds = sSpellMgr->GetSpellSpellGroupMapBounds(rank->Id);
                for (auto itr = bounds.first; itr != bounds.second; ++itr)
                    chain.Groups.insert(uint32(itr->second));
            }
            chains.push_back(std::move(chain));
        }

        // Union chains that share a group, until nothing joins.
        std::vector<uint32> parent(chains.size());
        for (uint32 i = 0; i < parent.size(); ++i)
            parent[i] = i;
        auto const root = [&parent](uint32 i)
        {
            while (parent[i] != i)
                i = parent[i] = parent[parent[i]];
            return i;
        };

        for (uint32 i = 0; i < chains.size(); ++i)
            for (uint32 j = i + 1; j < chains.size(); ++j)
                if ((chains[i].Specific != SPELL_SPECIFIC_NORMAL && chains[i].Specific == chains[j].Specific)
                    || std::any_of(chains[i].Groups.begin(), chains[i].Groups.end(),
                        [&](uint32 group) { return chains[j].Groups.contains(group); }))
                    parent[root(j)] = root(i);

        std::map<uint32, std::vector<uint32>> groups;
        for (uint32 i = 0; i < chains.size(); ++i)
        {
            std::vector<uint32>& ranks = groups[root(i)];
            ranks.insert(ranks.end(), chains[i].Ranks.begin(), chains[i].Ranks.end());
        }

        std::vector<std::vector<uint32>> result;
        for (auto& [id, ranks] : groups)
            result.push_back(std::move(ranks));
        return result;
    }
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
        case SeatGoal::Count:    break;
    }

    return "unknown";
}

std::string_view Animus::Curriculum::PostureName(TeamPosture posture)
{
    switch (posture)
    {
        case TeamPosture::Attack:  return "attack";
        case TeamPosture::Defend:  return "defend";
        case TeamPosture::Protect: return "protect";
        case TeamPosture::Recover: return "recover";
        case TeamPosture::Regroup: return "regroup";
        case TeamPosture::Hold:    return "hold";
        case TeamPosture::Count:   break;
    }

    return "unknown";
}

std::string_view Animus::Curriculum::RallyName(TeamRally rally)
{
    switch (rally)
    {
        case TeamRally::None:      return "none";
        case TeamRally::OwnBase:   return "own_base";
        case TeamRally::EnemyBase: return "enemy_base";
        case TeamRally::Carrier:   return "carrier";
        case TeamRally::Focus:     return "focus";
        case TeamRally::Spread:    return "spread";
        case TeamRally::Stack:     return "stack";
        case TeamRally::Point:     return "point";
        case TeamRally::Count:     break;
    }

    return "unknown";
}

std::string_view Animus::Curriculum::AnchorName(PlaceAnchor anchor)
{
    switch (anchor)
    {
        case PlaceAnchor::TeamCentre:    return "team";
        case PlaceAnchor::Focus:         return "focus";
        case PlaceAnchor::LastSeenEnemy: return "last_seen";
        case PlaceAnchor::Objective:     return "objective";
        case PlaceAnchor::OwnBase:       return "own_base";
        case PlaceAnchor::EnemyBase:     return "enemy_base";
        case PlaceAnchor::Count:         break;
    }

    return "unknown";
}

std::string_view Animus::Curriculum::OffsetName(PlaceOffset offset)
{
    switch (offset)
    {
        case PlaceOffset::At:     return "at";
        case PlaceOffset::Toward: return "toward";
        case PlaceOffset::Away:   return "away";
        case PlaceOffset::Left:   return "left";
        case PlaceOffset::Right:  return "right";
        case PlaceOffset::Count:  break;
    }

    return "unknown";
}

std::string_view Animus::Curriculum::RingName(PlaceRing ring)
{
    switch (ring)
    {
        case PlaceRing::Near:  return "near";
        case PlaceRing::Far:   return "far";
        case PlaceRing::Count: break;
    }

    return "unknown";
}

std::string_view Animus::Curriculum::BlockName(BlockId id)
{
    switch (id)
    {
        case BlockId::Core:      return "core";
        case BlockId::Move:      return "move";
        case BlockId::Duel:      return "duel";
        case BlockId::Pack:      return "pack";
        case BlockId::Gauntlet:  return "gauntlet";
        case BlockId::Companion: return "companion";
        case BlockId::Party:     return "party";
        case BlockId::Pvp:       return "pvp";
        case BlockId::Context:   return "context";
        case BlockId::Hostiles:  return "hostiles";
        case BlockId::Pet:       return "pet";
        case BlockId::Travel:    return "travel";
        case BlockId::Flag:      return "flag";
        case BlockId::Order:     return "order";
        case BlockId::Support:   return "support";
        case BlockId::World:     return "world";
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

    // Resurrections and the soulstone are cast on a dead or living ally (companion and party blocks). Heals, shields
    // and buffs are core actions cast on the support block's selected friend.
    if (stage.Has(BlockId::Companion) || stage.Has(BlockId::Party))
        layout.AllyRevives = layout.Catalog().Revives();

    layout.BuffGroups = BuffGroupsOf(layout.Catalog());

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

Animus::Curriculum::Layout Animus::Curriculum::Layout::BuildDirector(StageDefinition const& stage)
{
    Layout layout;
    layout.Stage = &stage;
    layout.Director = true;
    layout.ObsDim = DirectorLayout::OBS_COUNT;
    layout.NumActions = DirectorLayout::ACTION_COUNT;
    layout.ModeGroups.assign(layout.NumActions, 0);
    return layout;
}

std::optional<Animus::Curriculum::BlockId> Animus::Curriculum::Layout::BlockOfAction(uint32 action) const
{
    for (BlockId id : Blocks)
        if (Slice(id).ContainsAction(action))
            return id;

    return std::nullopt;
}

boost::json::array Animus::Curriculum::SpellList(std::vector<ActionCatalog::Action> const& actions)
{
    boost::json::array list;
    list.reserve(actions.size());
    for (ActionCatalog::Action const& action : actions)
        list.push_back(action.FirstRank);

    return list;
}

boost::json::array Animus::Curriculum::Span(uint32 first, uint32 count)
{
    return { first, count };
}

std::string Animus::Curriculum::Layout::ModelName() const
{
    return (Director ? DirectorLayout::Name() : Profile->Name) + Stage->Suffix;
}

std::vector<std::string> Animus::Curriculum::Layout::ActionNames() const
{
    if (Director)
        return DirectorLayout::ActionNames();

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
    manifest["class_name"] = Director ? DirectorLayout::Name() : Profile->Name.c_str();
    manifest["class"] = Director ? 0 : Profile->Class;
    manifest["obs_dim"] = ObsDim;
    manifest["num_actions"] = NumActions;

    boost::json::array& actionNames = manifest["action_names"].emplace_array();
    for (std::string const& name : ActionNames())
        actionNames.push_back(boost::json::string(name));

    // Every build the class can have, because one model plays all of them, and what each one can do -- there is
    // no role to name, and the aptitude is the thing a reader of the manifest actually wants.
    boost::json::array& specs = manifest["specs"].emplace_array();
    if (!Director)
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
        GetBlock(id).DescribeManifest(*this, block);
    }

    return boost::json::serialize(manifest);
}
