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

#ifndef ANIMUS_LIB_CURRICULUM_REWARD_UNITS_H
#define ANIMUS_LIB_CURRICULUM_REWARD_UNITS_H

namespace Animus::Curriculum
{
    /// **The one table** (reward-structure plan, 2026-10-29): every fight term of the seat curriculum in one unit,
    /// 1.0 = killing one normal enemy of the seat's level, read from AnimusForge.Curriculum.Reward.* (CurriculumTuning
    /// ::Visit). The fight tracker (FightTracker) pays the outcomes and potentials from it; CombatEncounter,
    /// RolesEncounter, PartyEncounter and InstanceEncounter charge their costs from it; StageScenario its two clocks.
    /// The dm_clear way's own outcomes (a leg, a gate, the full clear) stay InstanceTuning's and DirectorTuning's, at
    /// the same scale (8, 2, 25).
    struct RewardUnits
    {
        // Outcomes, once per thing (RewardLedger::AddOnce). A kill by rank: the creature template's normal or elite
        // (rare elite too), a dungeon boss met on the way (mid-boss), a run's last boss.
        float KillNormal = 1.0f;
        float KillElite = 2.0f;
        float KillMidBoss = 8.0f;
        float KillBoss = 25.0f;
        /// A fight (an engagement until no enemy is in combat with the party) ended with every engaged enemy dead and
        /// no party death in it.
        float FightWon = 1.0f;
        /// A pack's last member dead, on top of its kills (the encounters that know packs).
        float PackClear = 0.5f;
        /// In a party, every living seat that was in the fight gets this share of each kill's value besides its
        /// damage share: the tank and the healer are paid for the party's kills.
        float PartyShare = 0.2f;
        /// The least damage share of a kill any seat that damaged it is paid, taken from the others (renormalised).
        float ContributorFloor = 0.10f;
        float Interrupt = 0.25f;                // an enemy cast stopped, once per cast

        // Costs, never faded. A death costs more than the fight it lost could pay the seat.
        float DeathSolo = 3.0f;
        float DeathParty = 4.0f;
        /// The whole party down. No solo encounter has a wipe (a solo death is the Death alone): WipeSolo is the
        /// table's for one that would.
        float WipeSolo = 6.0f;
        float WipeParty = 10.0f;
        float TeammateDeath = 1.0f;             // each living seat, a party member's death
        /// The one clock (EngagedClock): per second an engaged enemy lives, every seat, every stage but dm1's.
        float Clock = 0.01f;
        /// Per decision, every seat, every stage but dm1's (StandingClock): so standing idle is never worth zero.
        float StandingClock = 0.002f;
        float ExtraPull = 1.0f;                 // a second pack pulled before the first is dead
        float Hurt = 0.2f;                      // per maximum health taken
        float Fire = 0.5f;                      // per maximum health ground fire took
        float Away = 0.02f;                     // per second dead or away from the fight

        // Shaping, potential-based (phi' - phi, with the membership rule; FightTracker), faded with the rest.
        /// Kill progress: phi = -Progress x sum over the engaged enemies of kill value x health fraction left.
        float Progress = 1.0f;
        /// The tank's hold: phi = -Hold x the engaged enemies whose victim is another seat.
        float Hold = 0.5f;
        /// The healer's keep: phi = -Keep x the sum of its teammates' missing health fractions, in a fight.
        float Keep = 0.5f;
        /// Focus: a non-tank seat's kill progress counts the living tank's victim at this multiple.
        float Focus = 1.5f;
    };
}

#endif
