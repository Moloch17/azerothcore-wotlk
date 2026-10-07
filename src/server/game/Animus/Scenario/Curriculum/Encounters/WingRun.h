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

#ifndef ANIMUS_LIB_CURRICULUM_WING_RUN_H
#define ANIMUS_LIB_CURRICULUM_WING_RUN_H

#include <algorithm>
#include <cstdint>
#include <vector>

/// **The party stages' bookkeeping of a dungeon run** (dungeon-curriculum G2, D1-D3; InstanceEncounter): the pure
/// parts -- which pack a corridor or an evaluation's drill starts at, which of a corridor's packs were cleared in route
/// order, which packs a fight drew in, the tier a run's outcome is scaled by -- with no core types, so they are tested
/// on their own (DungeonStagesTest).
namespace Animus::Curriculum::WingRun
{
    /// The support ladder's rungs (StageScenario::WING_RUNGS, 13) a tier spans: a run's outcome terms scale with
    /// 1 + Difficulty.TierScale x its tier, rung 0 (the most support, eight levels up) tier 0 and the evaluation's rung
    /// 12 (the dungeon's own levels, no help) tier 4, so a ladder that steps down raises what a clear pays rather
    /// than the score falling as it gets harder (animus-tier-scaled-outcomes).
    constexpr uint32_t RUNGS_PER_TIER = 3;

    [[nodiscard]] constexpr uint32_t TierOfRung(uint32_t rung)
    {
        return rung / RUNGS_PER_TIER;
    }

    /// One of `count` choices taken from an evaluation's seed index: the same seed takes the same one in every
    /// evaluation, and successive seeds spread over all of them (a multiplicative hash, so a run of seeds that plays
    /// one
    /// class after another does not walk the choices in step with the classes). 0 for no choice to make.
    [[nodiscard]] constexpr uint32_t SeededPick(uint32_t count, uint32_t seed)
    {
        if (count <= 1)
            return 0;
        uint64_t const mixed = (uint64_t(seed) + 1) * 2654435761ull;
        return uint32_t((mixed ^ (mixed >> 16)) % count);
    }

    /// **A corridor's first pack** (ArenaDefinition::CorridorPacks): a corridor of `length` packs over a route of
    /// `packs` starts at one of the packs leaving `length` after it (the whole route when it is shorter): a training
    /// run's from `roll` (any uniform number), an evaluation's from its seed (SeededPick).
    [[nodiscard]] constexpr uint32_t CorridorFirst(uint32_t packs, uint32_t length, bool evaluating, uint32_t seed,
        uint32_t roll)
    {
        uint32_t const starts = packs > length ? packs - length + 1 : 1;
        return evaluating ? SeededPick(starts, seed) : roll % starts;
    }

    /// **A corridor run's packs** (G2: "packs cleared in order"): packs [First, End) of the route, each noted once as
    /// it is found cleared -- in route order when every pack of the corridor before it already was, out of order
    /// otherwise (pulled past, or dragged into another pack's fight). Done when every one is cleared.
    struct Corridor
    {
        uint32_t First = 0;
        uint32_t End = 0;
        uint32_t Next = 0;                  // the first corridor pack not yet noted cleared
        uint32_t InOrder = 0;
        uint32_t OutOfOrder = 0;
        std::vector<bool> Noted;

        void Begin(uint32_t first, uint32_t end)
        {
            First = first;
            End = std::max(first, end);
            Next = First;
            InOrder = 0;
            OutOfOrder = 0;
            Noted.assign(End - First, false);
        }

        [[nodiscard]] bool Active() const { return End > First; }
        [[nodiscard]] uint32_t Length() const { return End - First; }
        [[nodiscard]] bool Done() const { return Active() && Next >= End; }
        [[nodiscard]] uint32_t Cleared() const { return InOrder + OutOfOrder; }
        /// The share of the corridor cleared: the run's progress (the support ladder's measure for a corridor).
        [[nodiscard]] float Share() const { return Active() ? float(Cleared()) / float(Length()) : 0.0f; }

        /// The route's packs' cleared flags as they stand now (index = route pack): the corridor's packs newly found
        /// cleared are noted, in route order. Returns how many of them were cleared in route order.
        uint32_t Note(std::vector<bool> const& cleared)
        {
            uint32_t inOrder = 0;
            for (uint32_t pack = First; pack < End; ++pack)
            {
                if (Noted[pack - First] || pack >= cleared.size() || !cleared[pack])
                    continue;
                Noted[pack - First] = true;
                if (pack == Next)
                {
                    ++InOrder;
                    ++inOrder;
                }
                else
                    ++OutOfOrder;
                while (Next < End && Noted[Next - First])
                    ++Next;
            }
            return inOrder;
        }
    };

    /// **The packs one fight has drawn in** (the chain pull's measure: "Costs: chain pulls"): the route packs whose
    /// creatures are fighting the party, as each fight goes. A pack joining a fight another pack started is a chain
    /// pull; the fight's end forgets them.
    struct FightPacks
    {
        std::vector<uint32_t> Packs;

        /// The route packs fighting the party this decision (any order, repeats allowed): returns how many joined a
        /// fight that already had a pack in it.
        uint32_t Note(std::vector<uint32_t> const& fighting)
        {
            uint32_t joined = 0;
            for (uint32_t pack : fighting)
            {
                if (std::find(Packs.begin(), Packs.end(), pack) != Packs.end())
                    continue;
                if (!Packs.empty())
                    ++joined;
                Packs.push_back(pack);
            }
            return joined;
        }

        void End() { Packs.clear(); }
    };
}

#endif
