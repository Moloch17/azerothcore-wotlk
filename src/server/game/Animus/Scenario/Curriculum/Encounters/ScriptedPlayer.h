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

#ifndef ANIMUS_LIB_CURRICULUM_SCRIPTED_PLAYER_H
#define ANIMUS_LIB_CURRICULUM_SCRIPTED_PLAYER_H

#include "ClassAssets.h"
#include "CurriculumTuning.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <vector>

class Player;
class Unit;

/*
 * Scripted players: the companion and party stages' owner, and the PvP stage's opponent. Deliberately simple and
 * readable -- they are the situations the learned policies train in, not policies themselves. How fast and how well
 * they play is tuning (<TuningPrefix>ScriptedPlayers.*).
 */
namespace Animus::Curriculum::ScriptedPlayer
{
    using Tuning = CurriculumTuning::ScriptedPlayerTuning;

    /// A scripted player's build, timers and repertoire.
    struct State
    {
        Aptitude Apt;                   // what its build can do, which is what decides what it does
        uint8 Spec = 0;                 // which of its class's builds it drew
        bool Ranged = false;            // its build fights at range
        uint32 EngageMs = 0;            // don't engage a pull before this episode time
        uint32 NextMoveMs = 0;
        uint32 NextSpellMs = 0;
        uint32 NextHealMs = 0;
        uint32 NextRegenMs = 0;
        std::vector<uint32> Spells;     // harmful single-target combat spells it knows (highest ranks)
        std::vector<uint32> Heals;      // single-target heals it knows
        std::vector<uint32> Taunts;     // single-target taunts it knows
        std::vector<uint32> Stealths;   // Stealth, Prowl: it may sneak up on an enemy player
        uint32 StealthForm = 0;         // the form its stealth needs first (Prowl: Cat Form); 0 when it needs none
        std::vector<uint32> Openers;    // harmful spells only usable from stealth (Cheap Shot, Garrote, Ambush)
        std::vector<uint32> Interrupts; // Kick, Counterspell, Wind Shear, Spell Lock-style silences
        std::vector<uint32> Controls;   // stuns, fears, polymorphs, roots and snares at an enemy
        std::vector<uint32> Defensives; // immunities and damage reductions on itself (Divine Shield, Ice Block, ...)
        std::vector<uint32> Breaks;     // breaking crowd control (Every Man for Himself, Will of the Forsaken, ...)

        // PvP tactics: whether this engagement plays them, and when it may crowd control next.
        bool TacticsDecided = false;
        bool Tactics = false;
        uint32 NextControlMs = 0;

        // PvP: the enemy it hunts and where it last saw it; it cannot see through stealth any more than a player can.
        ObjectGuid Quarry;
        bool QuarrySeen = false;
        Position LastSeen;
        bool StealthDecided = false;    // it rolled whether to sneak up this engagement
        bool Sneaking = false;          // it rolled to sneak and is getting into (its form and) stealth
        uint32 SneakUntilMs = 0;        // ... giving up at this episode time, so a failed shift cannot stall it
    };

    /// Dress a placed bot of the assets' class: proficiencies, a build of one of the specs that meets `demand`,
    /// trainer spells for its level, gear (PvP gear too when `pvp`). Fills state's repertoire and aptitude.
    void Configure(Player* player, ClassAssets const& assets, AptitudeDemand demand, State& state, bool pvp);

    /// One decision of a scripted party member, which follows from what its build can do (State::Apt):
    /// - it can hold the pull: engages first, goes for enemies attacking someone else and taunts them off;
    /// - it can heal: heals the most hurt party member, keeps near the tank, and only casts damage spells when
    ///   nobody needs healing;
    /// - damage dealer: between pulls it wanders near `home`, recovering health and mana; once a pull is up (and
    ///   state.EngageMs has passed) it fights the tank's target, else the enemy attacking it, else the nearest.
    /// `party` is every party player, the member itself included; `tank` the party's tank (may be null).
    void UpdateMember(Player* member, std::vector<Player*> const& party, Player* tank,
        std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home, State& state, Tuning const& tuning);

    /// One decision of a scripted PvP opponent fighting `enemy`: a healer heals itself when hurt, a ranged spec keeps
    /// its distance and casts, a melee spec closes in and fights; all use damage spells. A rogue sneaks up in stealth
    /// ScriptedPlayers.StealthChance percent of the time and opens from it. An enemy it can neither see nor detect it
    /// searches for where it last saw it. ScriptedPlayers.TacticsChance percent of engagements it also plays its
    /// kit: interrupts the enemy's casts, crowd controls it now and then, snares it before backing off (ranged), uses
    /// a defensive when low and breaks crowd control, so the learned side has to bait, trinket and time its burst.
    void UpdateOpponent(Player* player, Player* enemy, uint32 nowMs, State& state, Tuning const& tuning);
}

#endif
