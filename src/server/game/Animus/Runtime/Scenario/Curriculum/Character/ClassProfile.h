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

#ifndef ANIMUS_LIB_CURRICULUM_CLASS_PROFILE_H
#define ANIMUS_LIB_CURRICULUM_CLASS_PROFILE_H

#include "Define.h"
#include "SharedDefines.h"
#include <array>
#include <string>
#include <vector>

namespace Animus::Curriculum
{
    /// Most specs any class has (the druid's four: balance, feral_cat, feral_bear, restoration). Grids keyed on
    /// (class, spec) -- the difficulty ladder, the training weights -- are this wide per class and leave the unused
    /// rows of a class with fewer specs empty, which keeps the shape fixed whichever classes a run plays.
    constexpr uint32 MAX_SPECS = 4;

    /// The playable races and classes, in the order of every one-hot over them (observations, critic state).
    constexpr std::array<uint8, 10> PLAYABLE_RACES =
    {
        RACE_HUMAN, RACE_ORC, RACE_DWARF, RACE_NIGHTELF, RACE_UNDEAD_PLAYER, RACE_TAUREN, RACE_GNOME, RACE_TROLL,
        RACE_BLOODELF, RACE_DRAENEI
    };

    constexpr std::array<uint8, 10> PLAYABLE_CLASSES =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_DEATH_KNIGHT, CLASS_SHAMAN,
        CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    /// Write a one-hot of `value` over `order` into `out` (nothing when `value` is not in it).
    template <std::size_t N>
    void WriteOneHot(std::array<uint8, N> const& order, uint8 value, float* out)
    {
        for (std::size_t i = 0; i < N; ++i)
            out[i] = order[i] == value ? 1.0f : 0.0f;
    }

    /// Which item stats a spec's gear is chosen for.
    enum class StatProfile : uint8
    {
        StrengthMelee,      // strength, attack power, melee ratings
        AgilityMelee,       // agility, attack power, melee ratings (rogue, feral cat, enhancement)
        Ranged,             // agility, attack power, ranged/melee ratings (hunter)
        Caster,             // intellect, spell power, spell ratings
        Healer,             // intellect, spirit, spell power, mp5
        Tank,               // stamina, avoidance, defense, block, threat stats
    };

    enum class RangeBand : uint8
    {
        Melee,              // stands next to the dummy
        Ranged,             // stands at casting/shooting range
    };

    /// How a spec wields weapons. Layouts are tried in order; the first whose slots can be filled at
    /// the bot's level and with its talents (dual wield) is used.
    enum class WeaponLayout : uint8
    {
        TwoHand,            // one two-handed melee weapon
        DualWield,          // two one-handed weapons (needs the dual wield skill)
        DualWieldDaggers,   // two daggers (Mutilate, Backstab and Ambush need them)
        OneHand,            // a main-hand weapon alone (before dual wield is learned)
        OneHandShield,
        OneHandHeld,        // one-hander plus a held-in-off-hand item
        Staff,
        TwoHandRanged,      // two-handed stat stick plus a bow, gun or crossbow (hunters)
    };

    struct SpecProfile
    {
        std::string Name;               // "arms"
        uint8 TabPage = 0;              // talent tab: 0, 1 or 2 in TalentTab.dbc order
        // No role. What a spec is for is not a fact about it that anybody should be writing down -- it is a
        // consequence of the build, and Aptitude reads it off the build. StatProfile stays, because which item
        // stats to gear for really is a property of the template and is only ever read while generating gear.
        StatProfile Stats = StatProfile::StrengthMelee;
        RangeBand Range = RangeBand::Melee;
        std::vector<WeaponLayout> Weapons;
        bool Wand = false;              // also fill the ranged slot with a wand
    };

    /// One trained model: a class, over every spec it can be, in every role those specs play.
    ///
    /// One model per class rather than per class and role, because the two were never different networks in
    /// anything but name: a class's kit, talents and action catalog are shared by all of its roles (ClassAssets),
    /// every Block::Size keys on the catalog or the class, and the built layouts of a class came out with
    /// byte-identical observation and action dimensions whichever role they were for. What splitting them bought
    /// was a smaller policy that had to relearn the class from scratch for each role; what joining them buys is a
    /// paladin that knows it is a paladin whether it is holding the line or healing it.
    struct ClassProfile
    {
        std::string Name;               // "<class>": the layout's name; its models add a stage suffix
        uint8 Class = 0;
        std::vector<SpecProfile> Specs;

    };

    /// Every class model, in a stable order.
    std::vector<ClassProfile> const& ClassProfiles();

    /// Per-decision damage scale of a level: roughly how a well-geared character's damage grows with level, so damage
    /// features and rewards have a similar size at every level (about 16 at level 1, 230 at 40, 3500 at 80).
    [[nodiscard]] float DamageScale(uint8 level);
}

#endif
