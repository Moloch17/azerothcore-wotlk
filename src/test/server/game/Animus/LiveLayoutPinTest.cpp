/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Block.h"
#include "CoreBlock.h"
#include "DuelBlock.h"
#include "PackBlock.h"
#include "PetBlock.h"
#include "StageScenario.h"
#include "TalentBuilder.h"
#include "ClassProfile.h"
#include "Layout.h"
#include "StageDefinition.h"
#include "gtest/gtest.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace Cu = Animus::Curriculum;

/*
 * **The live stages' layouts are pinned.** A policy checkpoint, a manifest and a seeding by name are tied to the
 * blocks of each live stage: their order, their revisions, their widths and the names of their columns and actions.
 * The first curriculum's code was deleted from the forge around this pin (2026-10-07); it was generated from the
 * unchanged tree and must pass unchanged after any such deletion. A change to it is a change to every checkpoint of the
 * stage: do not edit the golden to make a test pass.
 *
 * What it records, per live stage and per block in order: the block's name and BlockId value, its revision, and --
 * for the blocks whose layout does not read the class's catalog or build (every block but core, duel and pet) -- its
 * observation and action counts, column names, action names and manifest entries (a hash of them, and their counts).
 * The class-dependent three are pinned by name, id and revision; their widths are a function of the class's catalog
 * and talents, which the deletion does not touch. Every class model (ClassProfiles) has the same block list at a
 * stage, so the list of classes is pinned too. Built with no database: a layout of a class needs the spell store.
 *
 * The golden is LiveLayoutPin.golden.inc. To regenerate it deliberately, run unit_tests with
 * ANIMUS_PIN_PRINT=1 --gtest_filter=LiveLayoutPinTest.* and paste the printed text.
 */

namespace
{
    constexpr char const* LIVE_STAGES[] = { "move1_controls", "move2_seek", "move3_interact", "move4_follow",
        "combat1_fight", "combat2_packs", "combat3_survive", "group1_roles", "group2_corridor", "dungeon1_pulls",
        "dungeon2_ragefire", "dungeon3_deadmines" };

    // The golden is one raw string literal, R"PIN(...)PIN" (LiveLayoutPin.golden.inc).
    constexpr char const* GOLDEN =
#include "LiveLayoutPin.golden.inc"
    ;

    uint64_t Fnv(std::string_view text, uint64_t hash = 1469598103934665603ull)
    {
        for (unsigned char c : text)
        {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    bool ClassDependent(Cu::BlockId id)
    {
        return id == Cu::BlockId::Core || id == Cu::BlockId::Duel || id == Cu::BlockId::Pet;
    }

    std::string Hex(uint64_t value)
    {
        char buffer[17];
        std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
        return buffer;
    }

    /// The sizes every layout, the critic's state and the wire are built from: the slots, the goal space, the state
    /// widths (the critic's input is STATE_GLOBAL_COUNT + MAX_SEATS * STATE_SEAT_FEATURES + PACK_SLOTS *
    /// STATE_ENEMY_FEATURES wide), and the constants of the class-dependent blocks.
    std::string Constants()
    {
        using Ss = Cu::StageScenario;
        std::string out = "constants";
        auto const add = [&out](char const* name, uint64_t value)
        {
            out += std::string(" ") + name + "=" + std::to_string(value);
        };
        add("MAX_ARENAS", Cu::MAX_ARENAS);
        add("RAID_GROUPS", Cu::RAID_GROUPS);
        add("GROUP_SEATS", Cu::GROUP_SEATS);
        add("MAX_SEATS", Cu::MAX_SEATS);
        add("TEAM_SEATS", Cu::TEAM_SEATS);
        add("TEAM_COUNT", Cu::TEAM_COUNT);
        add("GROUP_MEMBERS", Cu::GROUP_MEMBERS);
        add("SPOTLIGHT_SLOTS", Cu::SPOTLIGHT_SLOTS);
        add("PARTY_MEMBERS", Cu::PARTY_MEMBERS);
        add("PACK_SLOTS", Cu::PACK_SLOTS);
        add("NAMED_ENEMY_SLOTS", Cu::NAMED_ENEMY_SLOTS);
        add("CROWD_SLOTS", Cu::CROWD_SLOTS);
        add("SIGHT_SLOTS", Cu::SIGHT_SLOTS);
        add("TRAIL_SAMPLES", Cu::TRAIL_SAMPLES);
        add("STABLE_SLOTS", Cu::STABLE_SLOTS);
        add("FRIEND_SLOTS", Cu::FRIEND_SLOTS);
        add("RANK_TIERS", Cu::RANK_TIERS);
        add("GOAL_COUNT", Cu::GOAL_COUNT);
        add("GOAL_TARGETS", Cu::GOAL_TARGETS);
        add("GOAL_JOINT_COUNT", Cu::GOAL_JOINT_COUNT);
        add("BLOCK_COUNT", Cu::BLOCK_COUNT);
        add("STATE_GLOBAL_COUNT", Ss::STATE_GLOBAL_COUNT);
        add("STATE_SEAT_FEATURES", Ss::STATE_SEAT_FEATURES);
        add("STATE_ENEMY_FEATURES", Ss::STATE_ENEMY_FEATURES);
        add("STATE_DIM", Ss::STATE_GLOBAL_COUNT + Cu::MAX_SEATS * Ss::STATE_SEAT_FEATURES
            + Cu::PACK_SLOTS * Ss::STATE_ENEMY_FEATURES);
        add("core.OBS_GLOBAL_COUNT", Cu::CoreBlock::OBS_GLOBAL_COUNT);
        add("core.ACTION_FEATURES", Cu::CoreBlock::ACTION_FEATURES);
        add("core.ACTION_RANK_TIERS", Cu::CoreBlock::ACTION_RANK_TIERS);
        add("TREE_COUNT", Cu::TalentBuilder::TREE_COUNT);
        add("duel.OBS_COUNT_WITHOUT_STABLE", Cu::DuelBlock::OBS_COUNT_WITHOUT_STABLE);
        add("duel.ACTION_COUNT_WITHOUT_STABLE", Cu::DuelBlock::ACTION_COUNT_WITHOUT_STABLE);
        add("duel.STABLE_FEATURES", Cu::DuelBlock::STABLE_FEATURES);
        add("pet.OBS_SLOT_FIRST", Cu::PetBlock::OBS_SLOT_FIRST);
        add("pet.ABILITY_SLOTS", Cu::PetBlock::ABILITY_SLOTS);
        add("pet.SLOT_FEATURES", Cu::PetBlock::SLOT_FEATURES);
        add("pet.ACTION_COUNT", Cu::PetBlock::ACTION_COUNT);
        // The enemies set DescribeSeatSets writes for a layout with the pack block (the only set a live stage has).
        add("pack.OBS_GLOBAL_COUNT", Cu::PackBlock::OBS_GLOBAL_COUNT);
        add("pack.SLOT_FEATURES", Cu::PackBlock::SLOT_FEATURES);
        add("pack.SLOT_PRESENT", Cu::PackBlock::SLOT_PRESENT);
        add("pack.ACTION_SLOT_FIRST", Cu::PackBlock::ACTION_SLOT_FIRST);
        out += " duel.actions=";
        Cu::Layout none;
        for (uint32_t local = 0; local < Cu::DuelBlock::ACTION_COUNT_WITHOUT_STABLE; ++local)
            out += Cu::GetBlock(Cu::BlockId::Duel).ActionName(none, local) + ',';
        out += '\n';
        return out;
    }

    std::string Describe()
    {
        std::string out = Constants();
        out += "classes";
        for (Cu::ClassProfile const& profile : Cu::ClassProfiles())
        {
            out += ' ' + profile.Name + ':' + std::to_string(unsigned(profile.Class));
            for (Cu::SpecProfile const& spec : profile.Specs)
                out += '/' + spec.Name;
        }
        out += '\n';
        for (char const* name : LIVE_STAGES)
        {
            Cu::StageDefinition const* stage = Cu::FindStage(name);
            if (!stage)
            {
                out += std::string(name) + " MISSING\n";
                continue;
            }

            Cu::Layout layout;
            layout.Stage = stage;
            layout.Blocks = stage->Blocks;
            uint32_t obs = 0;
            uint32_t actions = 0;
            out += "stage " + std::string(name) + " suffix=" + stage->Suffix + "\n";
            for (Cu::BlockId id : stage->Blocks)
            {
                Cu::Block const& block = Cu::GetBlock(id);
                out += "  " + std::string(Cu::BlockName(id)) + " id=" + std::to_string(unsigned(id))
                    + " rev=" + std::to_string(block.Revision());
                if (ClassDependent(id))
                {
                    out += " class-dependent\n";
                    continue;
                }

                Cu::BlockSize const size = block.Size(layout);
                obs += size.Obs;
                actions += size.Actions;
                boost::json::array columns;
                block.DescribeColumns(layout, columns);
                std::string actionNames;
                for (uint32_t local = 0; local < size.Actions; ++local)
                    actionNames += block.ActionName(layout, local) + ',';
                boost::json::object manifest;
                block.DescribeManifest(layout, manifest);
                uint64_t hash = Fnv(boost::json::serialize(columns));
                hash = Fnv(actionNames, hash);
                hash = Fnv(boost::json::serialize(manifest), hash);
                out += " obs=" + std::to_string(size.Obs) + " actions=" + std::to_string(size.Actions)
                    + " columns=" + std::to_string(columns.size()) + " hash=" + Hex(hash) + "\n";
            }
            out += "  class-independent totals obs=" + std::to_string(obs) + " actions=" + std::to_string(actions)
                + "\n";
        }
        return out;
    }
}

TEST(LiveLayoutPinTest, LiveStageLayoutsAreUnchanged)
{
    std::string const now = Describe();
    if (std::getenv("ANIMUS_PIN_PRINT"))
        std::cout << "----PIN-BEGIN----\n" << now << "----PIN-END----\n";
    EXPECT_EQ(std::string("\n") + now, std::string(GOLDEN));
}
