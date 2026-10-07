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

#ifndef ANIMUS_LIB_CURRICULUM_STAND_IN_H
#define ANIMUS_LIB_CURRICULUM_STAND_IN_H

#include <array>
#include <cstdint>

/// **The "human" stand-in** (dungeon-curriculum plan I7): one party seat played by somebody other than the policy
/// being trained, so the learned seats learn to work with whoever joins them -- the end use is a human collaborator in
/// any role -- and not only with copies of themselves.
///
/// **Who plays it**: a frozen learned partner, drawn by the learner from its co-op partner pool (animus.partners: an
/// earlier stage's best, this run's snapshots), never a script. The sim marks the seat (its row's `present` reads 2,
/// protocol 25); the learner plays that row with the partner and never trains on it. The sim fields a stand-in only
/// while the learner says it has a partner to play one (MODE_FLAG_STAND_IN).
///
/// **Its style**, drawn once an episode from a seed (Draw), so an evaluation meets the same people every time:
/// - **leads or follows.** A leading stand-in sits in seat 0, which PartyEncounter makes the group's leader -- the one
///   the party keeps with (WingRun::LeaderSeat); a follower is any other seat;
/// - **a role** -- tank, healer or damage -- within what its class and build can do (RoleFor, read off the seat's
///   Aptitude): a follower takes a seat whose build plays the role it wants, when the party has one.
/// The partner checkpoint the learner draws for the seat is the rest of its style.
///
/// Pure -- no core types, its own random numbers from its seed -- so the draws are tested on their own (StandInTest).
namespace Animus::Curriculum::StandIn
{
    enum class Role : uint8_t
    {
        Tank = 0,
        Healer,
        Damage,
        Count
    };
    constexpr std::array<char const*, uint32_t(Role::Count)> ROLE_NAMES = { "tank", "healer", "damage" };

    /// A seat's `present` on the wire (protocol 25): 0 no character, 1 the learner's, 2 the stand-in's -- a real row
    /// the learner plays with a frozen partner and never trains on.
    constexpr uint8_t PRESENT_NONE = 0;
    constexpr uint8_t PRESENT_LEARNER = 1;
    constexpr uint8_t PRESENT_STAND_IN = 2;

    [[nodiscard]] constexpr uint8_t Presence(bool hasCharacter, bool standIn)
    {
        return !hasCharacter ? PRESENT_NONE : standIn ? PRESENT_STAND_IN : PRESENT_LEARNER;
    }

    /// Whether an episode fields a stand-in at all. Only a party or a raid with someone beside the seat, and only while
    /// the learner has a frozen partner to play it (`modeAllows`: the MODE's MODE_FLAG_STAND_IN) -- without the flag no
    /// episode has one, an evaluation's included. Then an evaluation of the stand-in arm has one in every episode, and
    /// training in `share` percent of them: `roll(share)` is asked only then, so a stage without the stand-in draws no
    /// random number and builds exactly the episodes it did.
    template <typename Roll>
    [[nodiscard]] bool Fields(bool modeAllows, bool partyOrRaid, uint32_t activeSeats, bool evaluating, int32_t share,
        Roll&& roll)
    {
        if (!partyOrRaid || activeSeats < 2 || !modeAllows)
            return false;

        return evaluating || (share > 0 && roll(share));
    }

    /// How often the stand-in plays, and how its styles are drawn (CurriculumTuning StandIn.*). Off by default
    /// (Share 0): a stage turns it on.
    struct Tuning
    {
        /// Percent of a party or raid arena's training episodes with a stand-in in one of its seats, while the learner
        /// can field one. Evaluations have one only when the learner asks for the "with the human stand-in" arm
        /// (MODE_FLAG_STAND_IN), in every episode.
        int32_t Share = 0;
        int32_t LeadChance = 50;            // percent of styles that lead (only where the party has no owner)
        int32_t TankChance = 34;            // percent of styles that want to tank ...
        int32_t HealerChance = 33;          // ... or heal; the rest deal damage
    };

    /// Its own random numbers (splitmix64): the same seed plays the same person, whatever else the world drew.
    class Rng
    {
    public:
        explicit Rng(uint64_t seed = 0) : _state(seed) { }

        uint64_t Next()
        {
            uint64_t z = (_state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        /// Uniform in [0, 1).
        float Unit() { return float(Next() >> 40) / float(1ull << 24); }
        /// True `percent` percent of the time.
        bool Chance(int32_t percent) { return float(percent) > Unit() * 100.0f; }
        /// Uniform in [low, high].
        uint32_t Between(uint32_t low, uint32_t high)
        {
            return high <= low ? low : low + uint32_t(Next() % (uint64_t(high - low) + 1));
        }

    private:
        uint64_t _state;
    };

    /// An evaluation episode's stand-in seed from its seed index: the same index meets the same person in every
    /// evaluation, whichever env plays it.
    [[nodiscard]] inline uint64_t EvaluationSeed(uint32_t seedIndex)
    {
        Rng mix(0x5374616E64496EULL ^ (uint64_t(seedIndex) << 17));
        return mix.Next();
    }

    struct Style
    {
        uint64_t Seed = 0;
        bool Leads = false;
        Role Wanted = Role::Damage;         // what it would like to play; RoleFor says what it can
    };

    /// The style seed `seed` plays. `canLead`: the party has no owner (nor a drilled seat 0), so the stand-in may be
    /// its leader.
    [[nodiscard]] inline Style Draw(uint64_t seed, Tuning const& tuning, bool canLead)
    {
        Rng rng(seed);
        Style style;
        style.Seed = seed;
        bool const leads = rng.Chance(tuning.LeadChance);
        style.Leads = canLead && leads;
        float const role = rng.Unit() * 100.0f;
        style.Wanted = role < float(tuning.TankChance) ? Role::Tank
            : role < float(tuning.TankChance + tuning.HealerChance) ? Role::Healer : Role::Damage;
        return style;
    }

    /// The role it plays: the one it wants when its build can do it, else damage (which every build can deal).
    [[nodiscard]] constexpr Role RoleFor(Role wanted, bool canTank, bool canHeal)
    {
        if (wanted == Role::Tank && canTank)
            return Role::Tank;
        if (wanted == Role::Healer && canHeal)
            return Role::Healer;
        return Role::Damage;
    }
}

#endif
