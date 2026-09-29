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

#include "ForecastBlock.h"
#include "IncomingSpell.h"
#include "Player.h"
#include "SeatMemory.h"
#include "SeatView.h"
#include "Spell.h"
#include "SpellInfo.h"
#include <algorithm>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    constexpr float SOON_SCALE_MS = 3000.0f;
    constexpr float SHARE_SCALE = 2.0f;
    constexpr float THREAT_SCALE = 1.5f;
    constexpr float DISTANCE_SCALE = 40.0f;

    /// A cast in progress: the spell, time left, and the unit it is aimed at (null for one with no unit).
    struct Cast
    {
        SpellInfo const* Info = nullptr;
        uint32 LeftMs = 0;
        Unit* Aimed = nullptr;
        Position Where;         // where an area spell lands: its destination, else the caster
    };

    Cast CastOf(Unit* enemy)
    {
        Cast cast;
        uint32 total = 0;
        cast.Info = IncomingSpell::CastInProgress(enemy, &cast.LeftMs, &total, nullptr);
        if (!cast.Info)
            return cast;

        Spell const* spell = enemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!spell)
            spell = enemy->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        cast.Where = enemy->GetPosition();
        if (spell)
        {
            cast.Aimed = spell->m_targets.GetUnitTarget();
            if (spell->m_targets.HasDst())
                cast.Where = spell->m_targets.GetDstPos()->GetPosition();
        }
        return cast;
    }

    /// The damage a harmful cast will do to `unit` if it lands, estimated from its damage effects' base values:
    /// unit-aimed spells at their unit, area spells at anyone inside their radius. Zero for anything else.
    float Estimate(Cast const& cast, Unit* caster, Unit const* unit)
    {
        if (!cast.Info || cast.Info->IsPositive() || !unit || !unit->IsAlive())
            return 0.0f;

        // A weapon strike is the caster's weapon plus the effect's bonus, or a percentage of the weapon.
        float const weapon = (caster->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE)
            + caster->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE)) * 0.5f;
        float damage = 0.0f;
        float percent = 0.0f;
        bool strike = false;
        for (SpellEffectInfo const& effect : cast.Info->GetEffects())
        {
            bool const school = effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE;
            bool const bonus = effect.Effect == SPELL_EFFECT_WEAPON_DAMAGE
                || effect.Effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG
                || effect.Effect == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL;
            bool const scaled = effect.Effect == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE;
            if (!school && !bonus && !scaled)
                continue;

            // Aimed at the unit, or it stands inside the effect's radius around where the spell lands: an area spell
            // centred on the tank splashes the healer standing beside it.
            float const reach = effect.HasRadius() ? effect.CalcRadius(caster) + unit->GetCombatReach() : 0.0f;
            Position const centre = cast.Aimed ? cast.Aimed->GetPosition() : cast.Where;
            bool const hits = (cast.Aimed && cast.Aimed == unit)
                || (effect.HasRadius() && unit->GetExactDist(&centre) <= reach);
            if (!hits)
                continue;

            float const value = float(std::max(0, effect.CalcValue(caster)));
            if (school)
                damage += value;
            else if (bonus)
            {
                damage += value;
                strike = true;
            }
            else
            {
                percent += value / 100.0f;
                strike = true;
            }
        }
        if (strike)
            damage += percent > 0.0f ? weapon * percent : weapon;
        return damage;
    }

    /// Every enemy the seat can see: the slots, and the enemy player.
    template <typename F>
    void ForEachEnemy(SeatView const& view, F&& visit)
    {
        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            if (Unit* enemy = view.Enemies[slot]; enemy && enemy->IsAlive())
                visit(enemy);
        if (view.Opponent && view.Opponent->IsAlive()
            && std::find(view.Enemies.begin(), view.Enemies.begin() + view.EnemyCount, view.Opponent)
                == view.Enemies.begin() + view.EnemyCount)
            visit(view.Opponent);
        if (view.Target && view.Target->IsAlive() && view.Target != view.Opponent
            && std::find(view.Enemies.begin(), view.Enemies.begin() + view.EnemyCount, view.Target)
                == view.Enemies.begin() + view.EnemyCount)
            visit(view.Target);
    }

    /// Where a player will be in `seconds` at its current movement: along its facing, turned for strafing and
    /// backing up, at the speed it is moving at. Where it is now when it stands.
    Position Ahead(Player const* player, float seconds)
    {
        Position where = player->GetPosition();
        if (!player->isMoving())
            return where;

        float angle = player->GetOrientation();
        bool const back = player->HasUnitMovementFlag(MOVEMENTFLAG_BACKWARD);
        bool const left = player->HasUnitMovementFlag(MOVEMENTFLAG_STRAFE_LEFT);
        bool const right = player->HasUnitMovementFlag(MOVEMENTFLAG_STRAFE_RIGHT);
        bool const forward = player->HasUnitMovementFlag(MOVEMENTFLAG_FORWARD);
        if (back)
            angle += float(M_PI);
        if (left)
            angle += forward ? float(M_PI) / 4.0f : back ? -float(M_PI) / 4.0f : float(M_PI) / 2.0f;
        if (right)
            angle -= forward ? float(M_PI) / 4.0f : back ? -float(M_PI) / 4.0f : float(M_PI) / 2.0f;

        float const speed = player->GetSpeed(back ? MOVE_RUN_BACK : player->IsWalking() ? MOVE_WALK : MOVE_RUN);
        where.Relocate(where.GetPositionX() + std::cos(angle) * speed * seconds,
            where.GetPositionY() + std::sin(angle) * speed * seconds, where.GetPositionZ());
        return where;
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::ForecastBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, 0 };
}

void Animus::Curriculum::ForecastBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    Player* bot = view.Bot;
    obs[OBS_INCOMING_SELF_SOON] = 1.0f;
    obs[OBS_INCOMING_ALLY_SOON] = 1.0f;
    obs[OBS_INTERRUPT_WINDOW] = 1.0f;
    obs[OBS_MANA_SECONDS] = view.Memory ? view.Memory->ManaSecondsLeft() : 1.0f;
    obs[OBS_TARGET_SECONDS] = view.Memory ? view.Memory->TargetSecondsLeft() : 1.0f;
    if (!bot || !bot->IsAlive())
        return;

    // The friends whose incoming damage counts: the owner and the teammates, in the map.
    std::array<Player*, 1 + PARTY_MEMBERS> friends{};
    uint32 friendCount = 0;
    if (view.Owner && view.Owner != bot && view.Owner->IsInMap(bot))
        friends[friendCount++] = view.Owner;
    for (SeatView::Teammate const& teammate : view.Teammates)
        if (teammate.Bot && teammate.Bot != bot && teammate.Bot->IsInMap(bot) && friendCount < friends.size())
            friends[friendCount++] = teammate.Bot;

    float selfDamage = 0.0f;
    float selfSoon = SOON_SCALE_MS;
    float allyShare = 0.0f;
    float allySoon = SOON_SCALE_MS;
    float interruptSoon = SOON_SCALE_MS;
    bool interrupt = false;
    float pull = 0.0f;
    float hold = 0.0f;
    std::array<float, 1 + PARTY_MEMBERS> allyDamage{};

    ForEachEnemy(view, [&](Unit* enemy)
    {
        Cast const cast = CastOf(enemy);
        if (cast.Info)
        {
            if (float const damage = Estimate(cast, enemy, bot); damage > 0.0f)
            {
                selfDamage += damage;
                selfSoon = std::min(selfSoon, float(cast.LeftMs));
            }
            for (uint32 i = 0; i < friendCount; ++i)
                if (float const damage = Estimate(cast, enemy, friends[i]); damage > 0.0f)
                {
                    allyDamage[i] += damage;
                    allySoon = std::min(allySoon, float(cast.LeftMs));
                }
            if (IncomingSpell::Interruptible(enemy))
            {
                interrupt = true;
                interruptSoon = std::min(interruptSoon, float(cast.LeftMs));
            }
        }

        ThreatManager& threat = enemy->GetThreatMgr();
        if (!threat.CanHaveThreatList())
            return;

        if (enemy->GetVictim() == bot)
        {
            // Holding it: how close a friend is to taking it.
            float const mine = threat.GetThreat(bot);
            if (mine > 0.0f)
                for (uint32 i = 0; i < friendCount; ++i)
                    hold = std::max(hold, threat.GetThreat(friends[i]) / mine);
        }
        else if (Unit const* victim = enemy->GetVictim())
        {
            // Not holding it: how close the seat is to taking it.
            float const top = threat.GetThreat(victim);
            if (top > 0.0f)
                pull = std::max(pull, threat.GetThreat(bot) / top);
        }
    });

    obs[OBS_INCOMING_SELF] = std::min(1.0f, selfDamage / float(std::max<uint32>(1, bot->GetHealth())) / SHARE_SCALE);
    obs[OBS_INCOMING_SELF_SOON] = selfSoon / SOON_SCALE_MS;
    for (uint32 i = 0; i < friendCount; ++i)
        allyShare = std::max(allyShare, allyDamage[i] / float(std::max<uint32>(1, friends[i]->GetHealth())));
    obs[OBS_INCOMING_ALLY] = std::min(1.0f, allyShare / SHARE_SCALE);
    obs[OBS_INCOMING_ALLY_SOON] = allyShare > 0.0f ? allySoon / SOON_SCALE_MS : 1.0f;
    obs[OBS_INTERRUPT_WINDOW] = interruptSoon / SOON_SCALE_MS;
    obs[OBS_INTERRUPT_PRESENT] = interrupt ? 1.0f : 0.0f;
    obs[OBS_THREAT_PULL] = std::min(1.0f, pull / THREAT_SCALE);
    obs[OBS_THREAT_HOLD] = std::min(1.0f, hold / THREAT_SCALE);

    if (view.Owner && view.Owner != bot && view.Owner->IsInMap(bot) && view.Owner->IsAlive())
    {
        Position const ahead = Ahead(view.Owner, OWNER_AHEAD_SECONDS);
        float const bearing = bot->GetRelativeAngle(&ahead);
        obs[OBS_OWNER_AHEAD_DISTANCE] = std::min(1.0f, bot->GetExactDist2d(&ahead) / DISTANCE_SCALE);
        obs[OBS_OWNER_AHEAD_SIN] = std::sin(bearing);
        obs[OBS_OWNER_AHEAD_COS] = std::cos(bearing);
    }

    // The teammate who will be furthest from the seat in the same time: the one the group is about to lose.
    float furthest = -1.0f;
    Position furthestAt;
    for (SeatView::Teammate const& teammate : view.Teammates)
        if (teammate.Bot && teammate.Bot != bot && teammate.Bot->IsInMap(bot) && teammate.Bot->IsAlive())
        {
            Position const ahead = Ahead(teammate.Bot, OWNER_AHEAD_SECONDS);
            if (float const distance = bot->GetExactDist2d(&ahead); distance > furthest)
            {
                furthest = distance;
                furthestAt = ahead;
            }
        }
    if (furthest >= 0.0f)
    {
        float const bearing = bot->GetRelativeAngle(&furthestAt);
        obs[OBS_TEAMMATE_AHEAD_DISTANCE] = std::min(1.0f, furthest / DISTANCE_SCALE);
        obs[OBS_TEAMMATE_AHEAD_SIN] = std::sin(bearing);
        obs[OBS_TEAMMATE_AHEAD_COS] = std::cos(bearing);
    }
}
