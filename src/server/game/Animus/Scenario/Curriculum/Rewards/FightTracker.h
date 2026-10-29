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

#ifndef ANIMUS_LIB_CURRICULUM_FIGHT_TRACKER_H
#define ANIMUS_LIB_CURRICULUM_FIGHT_TRACKER_H

#include "Define.h"
#include "ObjectGuid.h"
#include "RewardLedger.h"
#include "RewardUnits.h"
#include <array>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

class Player;
class Unit;

namespace Animus
{
    struct Env;
}

namespace Animus::Curriculum
{
    /// **The fight tracker** (reward-structure plan, 2026-10-29): one per env, what every fight stage pays for its
    /// fights, in RewardUnits. StageScenario updates it once a decision (after the encounters' BeforeRewards) and pays
    /// each seat from it, in every arena one of whose encounters says Encounter::TracksFights.
    ///
    /// - **The fight**: an engagement, from the first enemy engaged with the party (alive, in combat, on a seat or a
    ///   seat's pet, or hit by one) until none is left. The engaged set holds its enemies; each one's kill value
    ///   (Encounter::KillValue, else its rank: RankValue) and the damage each seat put on it this fight.
    /// - **Kill** (Outcome, once per enemy, RewardLedger::AddOnce on its guid): value x the seat's damage share --
    ///   each seat's damage times its damage weight (a party healer's 0, a tank's Party.TankDamageShare), shares under
    ///   Reward.ContributorFloor raised to it and the rest renormalised so they sum to 1; a kill nobody's recorded
    ///   damage reached (a corpse in the slots' overflow) is split evenly among the seats in the fight. Damage shares
    ///   are paid to a contributor dead or alive; in a party every living seat that was in the fight gets
    ///   Reward.PartyShare x value besides.
    /// - **FightWon** (Outcome, once per fight): it ended with every engaged enemy dead (none evaded or gone) and no
    ///   seat's death in it, paid to the living seats that were in it.
    /// - **PackClear** (Outcome, once per pack): an encounter that knows packs says one is cleared (PackCleared).
    /// - **Potentials** (Shaping, phi' - phi): kill progress, the tank's hold, the healer's keep (RewardUnits). The
    ///   **membership rule**: each decision phi is taken twice -- over the members of the last decision with what they
    ///   are now (the state part, paid), and over the members after this decision's changes (stored without paying).
    ///   Members are the engaged enemies, the living seats, the fight being on, and the tank's victim (the focus
    ///   weight): an enemy entering or leaving, a seat dying or rising, the tank retargeting move phi for free. An
    ///   engaged enemy that dies counts 0 health, so the last blow pays; one gone from the world keeps its last
    ///   reading. A corpse removed within the decision it died in is a leave, not a kill (unpaid).
    class FightTracker
    {
    public:
        /// One seat as the tracker reads it this decision.
        struct Seat
        {
            Player* Bot = nullptr;              // null: no character in the seat, or not in the world
            bool Tank = false;
            bool Healer = false;
            float DamageWeight = 1.0f;          // its damage in a kill's shares
        };
        /// What an enemy is worth killed, in units; negative: by its rank (RankValue).
        using KillValueOf = std::function<float(Unit const*)>;

        void ResetEpisode();
        /// Once a decision, before any seat is paid: this decision's damage (env.StepDamaged), kills, fight end and
        /// potentials. A party is more than one seated seat.
        void Update(Env const& env, std::vector<Seat> const& seats, KillValueOf const& killValue,
            RewardUnits const& units);
        /// A pack of the encounter's cleared, `key` naming it for the episode: PackClear for every living seat.
        void PackCleared(uint64 key) { _packs.push_back(key); }
        /// Pays seat `seat` what Update found for it, wins times `tier` (the rung's factor; potentials never).
        void Pay(uint32 seat, RewardLedger& ledger, float tier, RewardUnits const& units);

        /// The party is in a fight (an engaged enemy left).
        [[nodiscard]] bool Fighting() const { return _fighting; }
        [[nodiscard]] uint32 Fights() const { return _fights; }
        [[nodiscard]] uint32 FightsWon() const { return _won; }

        /// An enemy's value by its rank: a dungeon boss met on the way (or a world boss) the mid-boss value, an elite
        /// or rare elite the elite one, anything else (a player too) the normal one.
        [[nodiscard]] static float RankValue(Unit const* enemy, RewardUnits const& units);

    private:
        static constexpr std::size_t POTENTIALS = 3;    // KillProgress, HoldPotential, KeepPotential

        struct Enemy
        {
            ObjectGuid Guid;
            float Value = 1.0f;
            float Health = 1.0f;                // the share of its health left at the last decision
            int32 Victim = -1;                  // the seat (or its pet) it was on at the last decision
            std::vector<float> Damage;          // per seat, this fight
        };

        struct SeatTrack
        {
            bool Seated = false;
            bool Alive = false;
            bool InFight = false;               // in this fight: in combat, hit an engaged enemy, or was hit
            float Missing = 0.0f;               // its missing health share at the last decision
            std::array<float, POTENTIALS> Phi{};        // over the members after the last decision
            std::array<float, POTENTIALS> State{};      // this decision's state part
            std::array<float, POTENTIALS> Previous{};   // ... and the phi it is paid against
            std::vector<std::pair<uint64, float>> Kills;
            std::vector<uint64> Packs;
            uint64 WonKey = 0;
            bool Won = false;
        };

        void StartFight(std::size_t seats);
        void PayKill(Enemy const& enemy, std::vector<Seat> const& seats, bool party, RewardUnits const& units);
        [[nodiscard]] std::array<float, POTENTIALS> PhiOf(uint32 seat, std::vector<Seat> const& seats,
            std::vector<float> const& health, std::vector<int32> const& victims, std::vector<float> const& missing,
            bool fighting, bool party, ObjectGuid focus, RewardUnits const& units) const;

        std::vector<Enemy> _enemies;            // the fight's engaged set
        std::vector<SeatTrack> _seats;
        std::vector<uint64> _packs;             // packs cleared since the last pay
        std::unordered_set<uint64> _killed;     // enemies whose kill was paid this episode
        ObjectGuid _focus;                      // the living tank's victim at the last decision
        bool _fighting = false;
        bool _lost = false;                     // this fight: an enemy left alive, or a seat died
        uint32 _fightKills = 0;
        uint32 _fightIndex = 0;
        uint32 _fights = 0;
        uint32 _won = 0;
    };
}

#endif
