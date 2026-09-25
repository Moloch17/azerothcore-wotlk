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

#include "DifficultyLadder.h"
#include "Env.h"
#include "EnvPool.h"
#include "Log.h"
#include "Random.h"
#include "StageScenario.h"
#include <algorithm>

Animus::Curriculum::DifficultyLadder::DifficultyLadder(StageScenario const& scenario, std::string what)
    : _scenario(scenario), _what(std::move(what)), _tiers(scenario.Layouts().size() * MAX_SPECS)
{
}

Animus::Curriculum::DifficultyLadder::Pick Animus::Curriculum::DifficultyLadder::Draw(Env const& env, uint16 layout,
    uint8 spec, uint32 maxTier) const
{
    Pick pick;
    if (env.EpisodeSeedIndex != NO_EPISODE_SEED)
    {
        // Seed i plays (class, build) pair i mod pairs (StageScenario::DrawCasting), and rung (i / pairs) mod
        // rungs, so every pair meets every rung. It has to be the pair count and not the layout count: dividing by
        // ten classes while the seeds cycle through eighteen pairs leaves the two out of step, and a pair would
        // wait far longer than it should to see a rung -- i mod rungs would otherwise tie each pair to one.
        uint32 const pairs = std::max<uint32>(1, _scenario.CastingCount());
        pick.Tier = (env.EpisodeSeedIndex / pairs) % (maxTier + 1);
    }
    else
    {
        CurriculumTuning::DifficultyTuning const& difficulty = _scenario.Tuning().Difficulty;
        uint32 const current = std::min(Tier(layout, spec), maxTier);
        pick.Tier = current;
        pick.Counts = true;
        if (current && roll_chance_i(difficulty.ReviewChance))
        {
            pick.Tier = urand(0, current - 1);
            pick.Counts = false;
        }
        else if (current < maxTier && roll_chance_i(difficulty.StretchChance))
        {
            // One rung above, and it does not count: a class/build is scored on every rung, so it should have met
            // the next one before it is asked to clear it -- without its losses there dragging it back down.
            pick.Tier = current + 1;
            pick.Counts = false;
        }
    }

    return pick;
}

void Animus::Curriculum::DifficultyLadder::Record(uint16 layout, uint8 spec, uint32 fightTier, bool won,
    uint32 maxTier)
{
    CurriculumTuning::DifficultyTuning const& difficulty = _scenario.Tuning().Difficulty;
    std::lock_guard<std::mutex> guard(_lock);
    if (Row(layout, spec) >= _tiers.size())
        return;

    LayoutTier& tier = _tiers[Row(layout, spec)];
    if (tier.Tier != fightTier)
        return;         // the rung moved while this fight was on

    ++tier.Fights;
    tier.Wins += won ? 1 : 0;
    if (tier.Fights < difficulty.Window)
        return;

    float const rate = float(tier.Wins) / float(tier.Fights);
    uint32 const was = tier.Tier;
    if (rate >= difficulty.RaiseAbove && tier.Tier < maxTier)
        ++tier.Tier;
    else if (rate < difficulty.LowerBelow && tier.Tier > 0)
        --tier.Tier;

    tier.Fights = 0;
    tier.Wins = 0;
    if (tier.Tier != was)
        LOG_INFO("module.animus", "{}: {} {} moves from {} {} to {} ({:.0f}% won)", _scenario.Name(),
            _scenario.Layouts()[layout].Profile->Name, _scenario.SpecName(layout, spec), _what, was, tier.Tier,
            rate * 100.0f);
}

uint32 Animus::Curriculum::DifficultyLadder::Tier(uint16 layout, uint8 spec) const
{
    std::lock_guard<std::mutex> guard(_lock);
    return Row(layout, spec) < _tiers.size() ? _tiers[Row(layout, spec)].Tier : 0;
}
