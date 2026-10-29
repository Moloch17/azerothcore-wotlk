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

#include "FightTracker.h"
#include "Creature.h"
#include "Env.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include <algorithm>
#include <numeric>

namespace
{
    constexpr std::size_t KILL = 0;
    constexpr std::size_t HOLD = 1;
    constexpr std::size_t KEEP = 2;
    constexpr std::array<Animus::Curriculum::RewardTerm, 3> POTENTIAL_TERMS = {
        Animus::Curriculum::RewardTerm::KillProgress, Animus::Curriculum::RewardTerm::HoldPotential,
        Animus::Curriculum::RewardTerm::KeepPotential };

    Unit* Resolve(Map* map, ObjectGuid guid)
    {
        if (!map || guid.IsEmpty())
            return nullptr;
        if (guid.IsPlayer())
        {
            Player* player = ObjectAccessor::FindPlayer(guid);
            return player && player->GetMap() == map ? player : nullptr;
        }
        return map->GetCreature(guid);
    }

    float HealthShare(Unit const* unit)
    {
        return unit->IsAlive() && unit->GetMaxHealth() ? float(unit->GetHealth()) / float(unit->GetMaxHealth()) : 0.0f;
    }

    /// An enemy a fight can hold: hostile to a seat, and no critter (a rat stepped on is no kill).
    bool Hostile(Unit const* unit, std::vector<Animus::Curriculum::FightTracker::Seat> const& seats)
    {
        Creature const* creature = unit->ToCreature();
        CreatureTemplate const* info = creature ? creature->GetCreatureTemplate() : nullptr;
        if (info && info->type == CREATURE_TYPE_CRITTER)
            return false;
        return std::any_of(seats.begin(), seats.end(), [unit](Animus::Curriculum::FightTracker::Seat const& seat)
        {
            return seat.Bot && unit->IsHostileTo(seat.Bot);
        });
    }

    /// The seat whose bot (or whose pet, guardian or totem) `unit` is; -1 for none.
    int32 SeatOf(Unit const* unit, std::vector<Animus::Curriculum::FightTracker::Seat> const& seats)
    {
        if (!unit)
            return -1;
        Player const* owner = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!owner)
            return -1;
        for (std::size_t seat = 0; seat < seats.size(); ++seat)
            if (seats[seat].Bot == owner)
                return int32(seat);
        return -1;
    }
}

float Animus::Curriculum::FightTracker::RankValue(Unit const* enemy, RewardUnits const& units)
{
    Creature const* creature = enemy ? enemy->ToCreature() : nullptr;
    if (!creature)
        return units.KillNormal;
    if (creature->IsDungeonBoss() || creature->isWorldBoss())
        return units.KillMidBoss;
    CreatureTemplate const* info = creature->GetCreatureTemplate();
    if (info && (info->rank == CREATURE_ELITE_ELITE || info->rank == CREATURE_ELITE_RAREELITE))
        return units.KillElite;
    return units.KillNormal;
}

void Animus::Curriculum::FightTracker::ResetEpisode()
{
    *this = FightTracker();
}

void Animus::Curriculum::FightTracker::StartFight(std::size_t seats)
{
    _fighting = true;
    _lost = false;
    _fightKills = 0;
    ++_fightIndex;
    ++_fights;
    for (std::size_t seat = 0; seat < seats; ++seat)
        _seats[seat].InFight = false;
}

void Animus::Curriculum::FightTracker::PayKill(Enemy const& enemy, std::vector<Seat> const& seats, bool party,
    RewardUnits const& units)
{
    std::size_t const count = seats.size();
    std::vector<float> share(count, 0.0f);
    float total = 0.0f;
    for (std::size_t seat = 0; seat < count; ++seat)
    {
        share[seat] = enemy.Damage[seat] * std::max(0.0f, seats[seat].DamageWeight);
        total += share[seat];
    }
    if (total > 0.0f)
    {
        // The floor: a contributor under it is raised to it, taken from the others in proportion to theirs.
        uint32 contributors = 0;
        for (std::size_t seat = 0; seat < count; ++seat)
        {
            share[seat] /= total;
            contributors += share[seat] > 0.0f ? 1 : 0;
        }
        float const floor = std::clamp(units.ContributorFloor, 0.0f, 1.0f);
        if (floor * float(contributors) >= 1.0f)
        {
            for (float& part : share)
                part = part > 0.0f ? 1.0f / float(contributors) : 0.0f;
        }
        else
        {
            float raised = 0.0f;
            float above = 0.0f;
            for (float part : share)
            {
                if (part > 0.0f && part < floor)
                    raised += floor - part;
                else if (part >= floor)
                    above += part;
            }
            for (float& part : share)
            {
                if (part > 0.0f && part < floor)
                    part = floor;
                else if (part >= floor && above > 0.0f)
                    part *= (above - raised) / above;
            }
        }
    }
    else
    {
        // Nobody's recorded damage reached it (or only a healer's): an even split among the seats in the fight.
        uint32 fighting = 0;
        for (std::size_t seat = 0; seat < count; ++seat)
            fighting += _seats[seat].InFight ? 1 : 0;
        for (std::size_t seat = 0; seat < count && fighting; ++seat)
            share[seat] = _seats[seat].InFight ? 1.0f / float(fighting) : 0.0f;
    }

    for (std::size_t seat = 0; seat < count; ++seat)
    {
        float value = enemy.Value * share[seat];
        if (party && _seats[seat].InFight && seats[seat].Bot && seats[seat].Bot->IsAlive())
            value += units.PartyShare * enemy.Value;
        if (value > 0.0f)
            _seats[seat].Kills.emplace_back(enemy.Guid.GetRawValue(), value);
    }
    ++_fightKills;
}

std::array<float, 3> Animus::Curriculum::FightTracker::PhiOf(uint32 seat, std::vector<Seat> const& seats,
    std::vector<float> const& health, std::vector<int32> const& victims, std::vector<float> const& missing,
    bool fighting, bool party, ObjectGuid focus, RewardUnits const& units) const
{
    std::array<float, POTENTIALS> phi{};
    Seat const& me = seats[seat];
    // Kill progress: the engaged enemies' health left, at their values; the tank's victim at the focus multiple for
    // a party's other seats.
    float left = 0.0f;
    for (std::size_t e = 0; e < _enemies.size(); ++e)
    {
        float const weight = party && !me.Tank && !focus.IsEmpty() && _enemies[e].Guid == focus ? units.Focus : 1.0f;
        left += _enemies[e].Value * health[e] * weight;
    }
    phi[KILL] = -units.Progress * left;
    // The tank's hold: enemies on another seat while a fight is on.
    if (me.Tank && fighting)
    {
        uint32 loose = 0;
        for (int32 victim : victims)
            loose += victim >= 0 && uint32(victim) != seat ? 1 : 0;
        phi[HOLD] = -units.Hold * float(loose);
    }
    // The healer's keep: its living teammates' missing health while a fight is on (missing < 0: not a member).
    if (me.Healer && fighting)
    {
        float hurt = 0.0f;
        for (std::size_t mate = 0; mate < missing.size(); ++mate)
            if (mate != seat && missing[mate] >= 0.0f)
                hurt += missing[mate];
        phi[KEEP] = -units.Keep * hurt;
    }
    return phi;
}

void Animus::Curriculum::FightTracker::Update(Env const& env, std::vector<Seat> const& seats,
    KillValueOf const& killValue, RewardUnits const& units)
{
    std::size_t const count = seats.size();
    if (_seats.size() < count)
        _seats.resize(count);
    for (Enemy& enemy : _enemies)
        enemy.Damage.resize(count, 0.0f);
    Map* map = env.FindMap();
    uint32 seated = 0;
    for (Seat const& seat : seats)
        seated += seat.Bot ? 1 : 0;
    bool const party = seated > 1;

    // This decision's damage on the engaged enemies; the hit enemies not yet engaged, to join (or die) below.
    std::vector<ObjectGuid> hit;
    for (Env::DamagedTarget const& damaged : env.StepDamaged)
    {
        if (damaged.Agent >= count || !damaged.Damage)
            continue;
        auto const enemy = std::find_if(_enemies.begin(), _enemies.end(),
            [&damaged](Enemy const& one) { return one.Guid == damaged.Victim; });
        if (enemy != _enemies.end())
            enemy->Damage[damaged.Agent] += float(damaged.Damage);
        else if (std::find(hit.begin(), hit.end(), damaged.Victim) == hit.end())
            hit.push_back(damaged.Victim);
        if (_fighting)
            _seats[damaged.Agent].InFight = true;
    }

    // The state part: the last decision's members as they are now. A dead enemy counts 0 health and its victim as it
    // was; one gone from the world its last reading; a seat that died or rose, a fight that started, are members.
    std::vector<float> health(_enemies.size());
    std::vector<int32> victims(_enemies.size());
    std::vector<Unit*> enemyUnits(_enemies.size(), nullptr);
    for (std::size_t e = 0; e < _enemies.size(); ++e)
    {
        Unit* unit = Resolve(map, _enemies[e].Guid);
        enemyUnits[e] = unit;
        health[e] = unit ? HealthShare(unit) : _enemies[e].Health;
        victims[e] = unit && unit->IsAlive() && unit->IsInCombat() ? SeatOf(unit->GetVictim(), seats)
            : _enemies[e].Victim;
    }
    bool seatsChanged = false;
    std::vector<float> missing(count, -1.0f);
    for (std::size_t seat = 0; seat < count; ++seat)
    {
        bool const alive = seats[seat].Bot && seats[seat].Bot->IsAlive();
        seatsChanged = seatsChanged || alive != _seats[seat].Alive
            || (seats[seat].Bot != nullptr) != _seats[seat].Seated;
        if (_seats[seat].Alive)
            missing[seat] = alive ? 1.0f - HealthShare(seats[seat].Bot) : _seats[seat].Missing;
    }
    for (std::size_t seat = 0; seat < count; ++seat)
    {
        SeatTrack& track = _seats[seat];
        track.Previous = track.Phi;
        bool const alive = seats[seat].Bot && seats[seat].Bot->IsAlive();
        if (!alive || !track.Alive)
        {
            track.State = track.Phi;
            continue;
        }
        track.State = PhiOf(uint32(seat), seats, health, victims, missing, _fighting, party, _focus, units);
        // A teammate's death or rise is the hold's and the keep's membership, never their state.
        if (seatsChanged)
        {
            track.State[HOLD] = track.Phi[HOLD];
            track.State[KEEP] = track.Phi[KEEP];
        }
    }

    // A seat's death in a fight loses it.
    for (std::size_t seat = 0; seat < count; ++seat)
        if (_fighting && _seats[seat].Alive && !(seats[seat].Bot && seats[seat].Bot->IsAlive()))
            _lost = true;

    // Kills: an engaged enemy dead, and an enemy hit this decision and already dead (killed before it was engaged).
    for (ObjectGuid const& guid : hit)
        if (Unit* unit = Resolve(map, guid); unit && !unit->IsAlive() && !_killed.count(guid.GetRawValue())
            && Hostile(unit, seats))
        {
            if (!_fighting)
                StartFight(count);
            for (Env::DamagedTarget const& damaged : env.StepDamaged)
                if (damaged.Victim == guid && damaged.Agent < count)
                    _seats[damaged.Agent].InFight = true;
            Enemy enemy;
            enemy.Guid = guid;
            float const value = killValue ? killValue(unit) : -1.0f;
            enemy.Value = value >= 0.0f ? value : RankValue(unit, units);
            enemy.Damage.assign(count, 0.0f);
            for (Env::DamagedTarget const& damaged : env.StepDamaged)
                if (damaged.Victim == guid && damaged.Agent < count)
                    enemy.Damage[damaged.Agent] += float(damaged.Damage);
            _killed.insert(guid.GetRawValue());
            PayKill(enemy, seats, party, units);
        }
    for (std::size_t e = 0; e < _enemies.size(); ++e)
    {
        Unit* unit = enemyUnits[e];
        if (unit && !unit->IsAlive() && !_killed.count(_enemies[e].Guid.GetRawValue()))
        {
            _killed.insert(_enemies[e].Guid.GetRawValue());
            PayKill(_enemies[e], seats, party, units);
        }
    }

    // Leaves: dead (paid above), gone from the world, or out of combat alive (evaded, reset): the last two lose the
    // fight.
    std::vector<Enemy> kept;
    kept.reserve(_enemies.size());
    for (std::size_t e = 0; e < _enemies.size(); ++e)
    {
        Unit* unit = enemyUnits[e];
        if (unit && unit->IsAlive() && unit->IsInCombat())
        {
            _enemies[e].Health = health[e];
            _enemies[e].Victim = victims[e];
            kept.push_back(std::move(_enemies[e]));
            continue;
        }
        if (!unit || unit->IsAlive())
            _lost = true;
    }
    _enemies = std::move(kept);

    // Joins: an enemy of the slots, or one a seat hit, alive and in combat on a seat or hit by one.
    auto const join = [&](ObjectGuid guid, bool wasHit)
    {
        if (guid.IsEmpty() || _killed.count(guid.GetRawValue()) || std::any_of(_enemies.begin(), _enemies.end(),
            [guid](Enemy const& one) { return one.Guid == guid; }))
            return;
        Unit* unit = Resolve(map, guid);
        if (!unit || !unit->IsAlive() || !unit->IsInCombat() || !Hostile(unit, seats))
            return;
        int32 const victim = SeatOf(unit->GetVictim(), seats);
        if (victim < 0 && !wasHit)
            return;
        if (!_fighting)
            StartFight(count);
        Enemy enemy;
        enemy.Guid = guid;
        float const value = killValue ? killValue(unit) : -1.0f;
        enemy.Value = value >= 0.0f ? value : RankValue(unit, units);
        enemy.Health = HealthShare(unit);
        enemy.Victim = victim;
        enemy.Damage.assign(count, 0.0f);
        for (Env::DamagedTarget const& damaged : env.StepDamaged)
            if (damaged.Victim == guid && damaged.Agent < count)
            {
                enemy.Damage[damaged.Agent] += float(damaged.Damage);
                _seats[damaged.Agent].InFight = true;
            }
        _enemies.push_back(std::move(enemy));
    };
    for (ObjectGuid const& guid : env.Targets)
        join(guid, std::find(hit.begin(), hit.end(), guid) != hit.end());
    for (ObjectGuid const& guid : hit)
        join(guid, true);

    // The seats now: who is in the fight, alive, hurt; the living tank's victim.
    ObjectGuid focus;
    for (std::size_t seat = 0; seat < count; ++seat)
    {
        SeatTrack& track = _seats[seat];
        Player* bot = seats[seat].Bot;
        track.Seated = bot != nullptr;
        track.Alive = bot && bot->IsAlive();
        track.Missing = track.Alive ? 1.0f - HealthShare(bot) : 0.0f;
        if (_fighting && track.Alive && bot->IsInCombat())
            track.InFight = true;
        if (seats[seat].Tank && track.Alive && bot->GetVictim() && focus.IsEmpty())
            focus = bot->GetVictim()->GetGUID();
    }
    for (Enemy const& enemy : _enemies)
        if (enemy.Victim >= 0 && uint32(enemy.Victim) < count)
            _seats[enemy.Victim].InFight = true;
    _focus = focus;

    // The fight's end: no engaged enemy left. Won with every one of them dead and no seat's death in it.
    if (_fighting && _enemies.empty())
    {
        if (!_lost && _fightKills)
        {
            ++_won;
            for (std::size_t seat = 0; seat < count; ++seat)
                if (_seats[seat].InFight && _seats[seat].Alive)
                {
                    _seats[seat].Won = true;
                    _seats[seat].WonKey = _fightIndex;
                }
        }
        _fighting = false;
        _lost = false;
    }

    // Packs cleared since the last decision, to every living seat.
    for (std::size_t seat = 0; seat < count; ++seat)
        if (_seats[seat].Alive)
            _seats[seat].Packs.insert(_seats[seat].Packs.end(), _packs.begin(), _packs.end());
    _packs.clear();

    // phi over the members after this decision's changes: what the next decision's state part is paid against.
    health.assign(_enemies.size(), 0.0f);
    victims.assign(_enemies.size(), -1);
    for (std::size_t e = 0; e < _enemies.size(); ++e)
    {
        health[e] = _enemies[e].Health;
        victims[e] = _enemies[e].Victim;
    }
    for (std::size_t seat = 0; seat < count; ++seat)
        missing[seat] = _seats[seat].Alive ? _seats[seat].Missing : -1.0f;
    for (std::size_t seat = 0; seat < count; ++seat)
        _seats[seat].Phi = _seats[seat].Alive
            ? PhiOf(uint32(seat), seats, health, victims, missing, _fighting, party, _focus, units)
            : std::array<float, POTENTIALS>{};
}

void Animus::Curriculum::FightTracker::Pay(uint32 seat, RewardLedger& ledger, float tier, RewardUnits const& units)
{
    if (seat >= _seats.size())
        return;
    SeatTrack& track = _seats[seat];
    for (auto const& [key, value] : track.Kills)
        ledger.AddOnce(RewardTerm::Kill, key, value, tier);
    track.Kills.clear();
    for (uint64 key : track.Packs)
        ledger.AddOnce(RewardTerm::PackClear, key, units.PackClear, tier);
    track.Packs.clear();
    if (track.Won)
        ledger.AddOnce(RewardTerm::FightWon, track.WonKey, units.FightWon, tier);
    track.Won = false;
    // Paid against the phi this tracker stored last decision, whatever the ledger holds (a decision this seat was
    // not paid leaves no drift), then re-based to the members now.
    for (std::size_t p = 0; p < POTENTIALS; ++p)
    {
        ledger.Rebase(POTENTIAL_TERMS[p], 0, track.Previous[p]);
        ledger.AddPotential(POTENTIAL_TERMS[p], 0, track.State[p]);
        ledger.Rebase(POTENTIAL_TERMS[p], 0, track.Phi[p]);
    }
    track.Previous = track.Phi;
    track.State = track.Phi;
}
