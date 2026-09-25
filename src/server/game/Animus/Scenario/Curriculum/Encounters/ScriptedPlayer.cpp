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

#include "ScriptedPlayer.h"
#include "Creature.h"
#include "EncoderSupport.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellChecks.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;
    using namespace Animus::SpellChecks;
    using ScriptedPlayer::State;
    using ScriptedPlayer::Tuning;

    constexpr uint32 MOVE_POINT_ID = 2;
    constexpr uint32 CHASE_REPATH_MS = 1000;
    constexpr uint32 REGEN_INTERVAL_MS = 1000;
    constexpr uint32 CAST_RETRY_MS = 500;           // after a cast that could not start
    constexpr float WANDER_MIN_DISTANCE = 8.0f;
    constexpr float WANDER_MAX_DISTANCE = 20.0f;
    constexpr float WANDER_LEASH = 30.0f;       // never wander further than this from home
    /// A run (ScriptedPlayerTuning::RunChance): how many spots are tried for one with a real route, how much longer
    /// than the straight line that route may be, and the run speed the leg's time is budgeted at.
    constexpr uint32 RUN_ATTEMPTS = 4;
    constexpr float RUN_MAX_DETOUR = 1.8f;
    constexpr float RUN_SPEED = 7.0f;
    constexpr float HEAL_RANGE = 40.0f;
    constexpr uint32 SEARCH_REPATH_MS = 2500;   // a hidden enemy: time between search steps
    constexpr float SEARCH_RADIUS = 10.0f;      // ... around where it was last seen

    bool IsHeal(SpellInfo const* info)
    {
        if (!info || info->IsPassive() || !info->IsPositive() || !info->NeedsExplicitUnitTarget())
            return false;

        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_HEAL
                || (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL))
                return true;

        return false;
    }

    /// Stealth itself: a rogue's Stealth (a stealth form) or a druid's Prowl (stealth inside Cat Form). Not
    /// Shadowmeld, which needs neither and breaks as soon as the player moves.
    bool IsStealth(SpellInfo const* info)
    {
        return info && !info->IsPassive() && info->HasAura(SPELL_AURA_MOD_STEALTH)
            && (info->HasAura(SPELL_AURA_MOD_SHAPESHIFT) || info->Stances);
    }

    /// Whether `stances` (a spell's form requirement) allows the player's current form. No requirement: any form.
    bool InRequiredForm(Player const* player, uint32 stances)
    {
        uint8 const form = uint8(player->GetShapeshiftForm());
        return !stances || (form && (stances & (1u << (form - 1))));
    }

    /// A spell the player knows that puts it in a form `stances` allows (Prowl's Cat Form); 0 if it knows none.
    uint32 FormSpellFor(Player const* player, uint32 stances)
    {
        if (!stances)
            return 0;

        for (auto const& [spellId, spell] : player->GetSpellMap())
        {
            if (spell->State == PLAYERSPELL_REMOVED || !spell->Active)
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive())
                continue;

            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.ApplyAuraName == SPELL_AURA_MOD_SHAPESHIFT && effect.MiscValue > 0
                    && (stances & (1u << (effect.MiscValue - 1))))
                    return spellId;
        }

        return 0;
    }

    /// A harmful single-target spell only usable from stealth that starts the fight (not Sap, which keeps stealth).
    bool IsOpener(SpellInfo const* info)
    {
        return info && !info->IsPassive() && info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) && !info->IsPositive()
            && info->NeedsExplicitUnitTarget() && !info->HasAttribute(SPELL_ATTR1_ALLOW_WHILE_STEALTHED);
    }

    /// Breaks or ends crowd control on itself: Every Man for Himself, Will of the Forsaken, Berserker Rage.
    bool IsBreak(SpellInfo const* info)
    {
        return info && !info->IsPassive() && info->IsPositive() && info->HasAura(SPELL_AURA_MECHANIC_IMMUNITY)
            && !info->NeedsExplicitUnitTarget();
    }

    /// A cooldown that keeps it alive: an immunity, or less damage taken, or avoiding it.
    bool IsDefensive(SpellInfo const* info)
    {
        if (!info || info->IsPassive() || !info->IsPositive())
            return false;

        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect != SPELL_EFFECT_APPLY_AURA)
                continue;

            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_SCHOOL_IMMUNITY:
                case SPELL_AURA_MOD_DODGE_PERCENT:
                case SPELL_AURA_MOD_PARRY_PERCENT:
                case SPELL_AURA_DEFLECT_SPELLS:
                case SPELL_AURA_REFLECT_SPELLS:
                    return true;
                case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                    if (effect.CalcValue() < 0)
                        return true;
                    break;
                default:
                    break;
            }
        }

        return false;
    }

    bool IsTaunt(SpellInfo const* info)
    {
        if (!info || info->IsPassive() || !info->NeedsExplicitUnitTarget())
            return false;

        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_ATTACK_ME
                || (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_MOD_TAUNT))
                return true;

        return false;
    }

    /// How long a scripted player keeps trying to get into stealth before it gives up and just fights.
    constexpr uint32 SNEAK_MS = 5000;

    bool TryCast(Player* caster, uint32 spellId, Unit* target)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || caster->HasSpellCooldown(info->Id) || caster->GetGlobalCooldownMgr().HasGlobalCooldown(info)
            || caster->IsNonMeleeSpellCast(false))
            return false;

        SpellCastTargets targets;
        targets.SetUnitTarget(target);
        Spell* spell = new Spell(caster, info, TRIGGERED_NONE);
        return spell->prepare(&targets) == SPELL_CAST_OK;
    }

    /// The first of `spells` it casts at `target`.
    bool TryAny(Player* caster, std::vector<uint32> const& spells, Unit* target)
    {
        for (uint32 spellId : spells)
            if (TryCast(caster, spellId, target))
                return true;

        return false;
    }

    /// A random spell of `spells` at `target`, if the spell timer allows. A cast restarts the timer; a failed one
    /// (range, cooldown, power) tries again shortly, so the scripted player casts as often as it is tuned to.
    void CastSometimes(Player* caster, std::vector<uint32> const& spells, Unit* target, uint32 nowMs, uint32& nextMs,
        uint32 minMs, uint32 maxMs)
    {
        if (nowMs < nextMs || spells.empty())
            return;

        bool const cast = TryCast(caster, spells[urand(0, uint32(spells.size()) - 1)], target);
        nextMs = nowMs + (cast ? urand(minMs, maxMs) : CAST_RETRY_MS);
    }

    void MoveNear(Player* player, Unit* target, float distance, uint32 nowMs, State& state)
    {
        if (nowMs < state.NextMoveMs)
            return;

        state.NextMoveMs = nowMs + CHASE_REPATH_MS;

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        target->GetNearPoint(player, x, y, z, player->GetCombatReach(), distance, target->GetAngle(player));
        player->GetMotionMaster()->MovePoint(MOVE_POINT_ID, x, y, z);
    }

    /// Between pulls: recover out of combat and wander near home, now and then running a longer leg (RunChance).
    void Idle(Player* player, uint32 nowMs, Position const& home, State& state, Tuning const& tuning)
    {
        if (player->GetVictim())
            player->AttackStop();

        if (!player->IsInCombat() && nowMs >= state.NextRegenMs)
        {
            state.NextRegenMs = nowMs + REGEN_INTERVAL_MS;
            player->SetHealth(std::min(player->GetMaxHealth(),
                player->GetHealth() + uint32(float(player->GetMaxHealth()) * tuning.RegenFraction)));
            if (uint32 const maxMana = player->GetMaxPower(POWER_MANA))
                player->SetPower(POWER_MANA, std::min(maxMana, player->GetPower(POWER_MANA)
                    + uint32(float(maxMana) * tuning.RegenFraction)));
        }

        if (nowMs < state.NextMoveMs)
            return;

        state.NextMoveMs = nowMs + urand(tuning.WanderMinMs, tuning.WanderMaxMs);

        // A run: a leg to somewhere, as a player crossing to the next camp, so that following is a thing the seats
        // beside it have to do rather than a step they never see. Measured from home, not from the player, so the
        // legs stay inside what the critic's origin-relative state can express, and the wander's leash makes the
        // return leg. Only a spot with a real route (PATHFIND_NORMAL and not a shortcut: see TravelEncounter) that
        // is not much longer than the straight line; otherwise this step is an ordinary wander.
        if (roll_chance_i(tuning.RunChance))
        {
            for (uint32 attempt = 0; attempt < RUN_ATTEMPTS; ++attempt)
            {
                float const angle = frand(0.0f, 2.0f * float(M_PI));
                float const distance = frand(tuning.RunMinYards, tuning.RunMaxYards);
                Position destination(home.GetPositionX() + distance * std::cos(angle),
                    home.GetPositionY() + distance * std::sin(angle), home.GetPositionZ());
                float const straight = player->GetExactDist2d(&destination);
                if (straight < tuning.RunMinYards)
                    continue;

                player->UpdateAllowedPositionZ(destination.m_positionX, destination.m_positionY,
                    destination.m_positionZ);
                PathGenerator path(player);
                if (!path.CalculatePath(destination.GetPositionX(), destination.GetPositionY(),
                    destination.GetPositionZ()) || !(path.GetPathType() & PATHFIND_NORMAL)
                    || (path.GetPathType() & PATHFIND_NOT_USING_PATH)
                    || path.getPathLength() > straight * RUN_MAX_DETOUR)
                    continue;

                player->GetMotionMaster()->MovePoint(MOVE_POINT_ID, destination);
                // The next step waits for the leg to be run.
                state.NextMoveMs += uint32(path.getPathLength() / RUN_SPEED * 1000.0f);
                return;
            }
        }

        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float const distance = frand(WANDER_MIN_DISTANCE, WANDER_MAX_DISTANCE);
        Position destination(player->GetPositionX() + distance * std::cos(angle),
            player->GetPositionY() + distance * std::sin(angle), player->GetPositionZ());

        // Stay near home: head back when the step would leave the leash.
        if (destination.GetExactDist2d(&home) > WANDER_LEASH)
            destination.Relocate(home.GetPositionX() + frand(-5.0f, 5.0f), home.GetPositionY() + frand(-5.0f, 5.0f),
                home.GetPositionZ());

        player->UpdateAllowedPositionZ(destination.m_positionX, destination.m_positionY, destination.m_positionZ);
        player->GetMotionMaster()->MovePoint(MOVE_POINT_ID, destination);
    }

    /// Melee the target, casting one of the player's damage spells now and then.
    void Fight(Player* player, Unit* target, uint32 nowMs, State& state, Tuning const& tuning)
    {
        if (!player->IsWithinMeleeRange(target))
        {
            MoveNear(player, target, 0.5f, nowMs, state);
            return;
        }

        if (player->GetVictim() != target)
        {
            player->GetMotionMaster()->Clear();
            player->SetFacingToObject(target);
            player->Attack(target, true);
        }

        CastSometimes(player, state.Spells, target, nowMs, state.NextSpellMs, tuning.SpellMinMs, tuning.SpellMaxMs);
    }

    /// An enemy player it cannot see: stop swinging at nothing, go where it was last seen, then look around there.
    void Search(Player* player, uint32 nowMs, State& state)
    {
        if (player->GetVictim())
            player->AttackStop();

        if (!state.QuarrySeen || nowMs < state.NextMoveMs)
            return;

        state.NextMoveMs = nowMs + SEARCH_REPATH_MS;

        Position destination = state.LastSeen;
        if (player->GetExactDist2d(&state.LastSeen) < SEARCH_RADIUS * 0.5f)
        {
            float const angle = frand(0.0f, 2.0f * float(M_PI));
            float const distance = frand(0.0f, SEARCH_RADIUS);
            destination.Relocate(state.LastSeen.GetPositionX() + distance * std::cos(angle),
                state.LastSeen.GetPositionY() + distance * std::sin(angle), state.LastSeen.GetPositionZ());
        }

        player->UpdateAllowedPositionZ(destination.m_positionX, destination.m_positionY, destination.m_positionZ);
        player->GetMotionMaster()->MovePoint(MOVE_POINT_ID, destination);
    }

    /// The enemy to fight: one already on the player, else the nearest.
    Unit* DefaultTarget(Player* player, std::vector<Unit*> const& enemies)
    {
        Unit* target = nullptr;
        for (Unit* enemy : enemies)
        {
            if (!enemy->IsAlive())
                continue;

            bool const onPlayer = enemy->GetVictim() == player;
            bool const targetOnPlayer = target && target->GetVictim() == player;
            if (!target || (onPlayer && !targetOnPlayer)
                || (onPlayer == targetOnPlayer && player->GetDistance(enemy) < player->GetDistance(target)))
                target = enemy;
        }

        return target;
    }

    void UpdateTank(Player* member, std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home,
        State& state, Tuning const& tuning)
    {
        bool const pullUp = std::any_of(enemies.begin(), enemies.end(), [](Unit* enemy) { return enemy->IsAlive(); });
        if (!pullUp)
        {
            Idle(member, nowMs, home, state, tuning);
            return;
        }

        if (nowMs < state.EngageMs)
            return;

        // Whatever is hitting someone else comes first; taunt it off them.
        Unit* loose = nullptr;
        for (Unit* enemy : enemies)
            if (enemy->IsAlive() && enemy->GetVictim() && enemy->GetVictim() != member
                && (!loose || member->GetDistance(enemy) < member->GetDistance(loose)))
                loose = enemy;

        if (loose && member->IsWithinDist(loose, tuning.TauntRange))
            for (uint32 taunt : state.Taunts)
                if (TryCast(member, taunt, loose))
                    break;

        Fight(member, loose ? loose : DefaultTarget(member, enemies), nowMs, state, tuning);
    }

    void UpdateHealer(Player* member, std::vector<Player*> const& party, Player* tank,
        std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home, State& state, Tuning const& tuning)
    {
        // The most hurt party member in range, below the threshold.
        Player* patient = nullptr;
        for (Player* ally : party)
            if (ally && ally->IsAlive() && ally->GetHealthPct() < tuning.HealBelow * 100.0f
                && member->IsWithinDist(ally, HEAL_RANGE)
                && (!patient || ally->GetHealthPct() < patient->GetHealthPct()))
                patient = ally;

        if (patient && nowMs >= state.NextHealMs && !state.Heals.empty())
        {
            member->GetMotionMaster()->Clear();
            member->StopMoving();
            CastSometimes(member, state.Heals, patient, nowMs, state.NextHealMs, tuning.HealMinMs, tuning.HealMaxMs);
            return;
        }

        bool const pullUp = std::any_of(enemies.begin(), enemies.end(), [](Unit* enemy) { return enemy->IsAlive(); });
        if (!pullUp)
        {
            Idle(member, nowMs, home, state, tuning);
            return;
        }

        if (member->IsNonMeleeSpellCast(false))
            return;

        // Stay in reach of the tank, out of the melee.
        Player* anchor = tank && tank != member && tank->IsAlive() ? tank : nullptr;
        if (anchor && !member->IsWithinDist(anchor, tuning.HealerRange))
        {
            MoveNear(member, anchor, tuning.HealerRange * 0.5f, nowMs, state);
            return;
        }

        // Nobody to heal: help with a damage spell on the tank's target.
        Unit* target = anchor && anchor->GetVictim() ? anchor->GetVictim() : DefaultTarget(member, enemies);
        if (target && nowMs >= state.EngageMs && nowMs >= state.NextSpellMs && !state.Spells.empty())
        {
            member->SetFacingToObject(target);
            CastSometimes(member, state.Spells, target, nowMs, state.NextSpellMs, tuning.SpellMinMs,
                tuning.SpellMaxMs);
        }
    }
}

void Animus::Curriculum::ScriptedPlayer::Configure(Player* player, ClassAssets const& assets,
    AptitudeDemand demand, State& state, bool pvp)
{
    // What this player is there for is the caller's business, not the profile's: one class profile holds every
    // build the class can have, and which of them it draws is what decides what it can do.
    uint8 const specIndex = DrawSpec(assets, demand);
    SpecProfile const& spec = assets.Profile->Specs[specIndex];

    GearBuilder::LearnProficiencies(player);
    assets.Talents->Apply(player, assets.Talents->Standard(spec.Name, spec.TabPage, player->GetFreeTalentPoints()));
    assets.Kit->Learn(player);
    assets.Talents->ApplyGlyphs(player, spec.Name);
    assets.Gear->Equip(player, spec, pvp);

    player->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    player->UpdateAllStats();
    player->SetFullHealth();
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    player->SetPower(POWER_ENERGY, player->GetMaxPower(POWER_ENERGY));

    state = State();
    state.Spec = specIndex;
    state.Ranged = spec.Range == RangeBand::Ranged;
    // Read after the build and the gear are on, so it describes the player that is actually standing there.
    state.Apt = Aptitude::Of(assets, assets.Talents->Standard(spec.Name, spec.TabPage,
        TalentBuilder::MAX_POINTS), player);

    if (player->getClass() == CLASS_WARRIOR)
        player->CastSpell(player, AptitudeDemand::HoldsThePull().MetBy(state.Apt)
            && state.Apt[Aptitude::TAUNT] > 0.0f && player->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE, true);

    // Its repertoire, highest ranks only: harmful single-target combat spells, heals and taunts.
    for (auto const& [spellId, spell] : player->GetSpellMap())
    {
        if (spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (IsTaunt(info))
            state.Taunts.push_back(spellId);
        else if (IsHeal(info))
            state.Heals.push_back(spellId);
        else if (IsStealth(info))
        {
            state.Stealths.push_back(spellId);
            // Prowl works inside Cat Form only: remember the shift, so a feral druid can sneak up like a rogue.
            if (!state.StealthForm)
                state.StealthForm = FormSpellFor(player, info->Stances);
        }
        else if (IsOpener(info))
            state.Openers.push_back(spellId);
        else if (IsBreak(info))
            state.Breaks.push_back(spellId);
        else if (IsDefensive(info))
            state.Defensives.push_back(spellId);
        else if (info && !info->IsPassive() && !info->IsPositive() && info->NeedsExplicitUnitTarget()
            && ActionCatalog::IsInterruptingSpell(info))
            state.Interrupts.push_back(spellId);
        else if (info && !info->IsPassive() && !info->IsPositive() && info->NeedsExplicitUnitTarget()
            && ActionCatalog::IsTacticalSpell(info))
            state.Controls.push_back(spellId);
        else if (ActionCatalog::IsCombatSpell(info) && !info->IsPositive() && info->NeedsExplicitUnitTarget()
            && !info->IsAutoRepeatRangedSpell())
            state.Spells.push_back(spellId);
    }
}

void Animus::Curriculum::ScriptedPlayer::UpdateMember(Player* member, std::vector<Player*> const& party,
    Player* tank, std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home, State& state,
    Tuning const& tuning)
{
    if (!member || !member->IsAlive())
        return;

    // What it does follows from what it can do. Holding the pull comes first: a build that can do both is more
    // use in front of the enemies than behind them.
    if (AptitudeDemand::HoldsThePull().MetBy(state.Apt) && state.Apt[Aptitude::TAUNT] > 0.0f)
    {
        UpdateTank(member, enemies, nowMs, home, state, tuning);
        return;
    }

    if (AptitudeDemand::KeepsThemUp().MetBy(state.Apt))
    {
        UpdateHealer(member, party, tank, enemies, nowMs, home, state, tuning);
        return;
    }

    // Anything else fights the tank's target once the tank has one.
    Unit* tankTarget = tank && tank != member && tank->IsAlive() ? tank->GetVictim() : nullptr;
    Unit* target = tankTarget && tankTarget->IsAlive() ? tankTarget : DefaultTarget(member, enemies);
    if (!target)
    {
        Idle(member, nowMs, home, state, tuning);
        return;
    }

    if (nowMs < state.EngageMs)
        return;

    Fight(member, target, nowMs, state, tuning);
}

void Animus::Curriculum::ScriptedPlayer::UpdateOpponent(Player* player, Player* enemy, uint32 nowMs, State& state,
    Tuning const& tuning)
{
    if (!player || !player->IsAlive() || !enemy || !enemy->IsAlive())
        return;

    // It follows what it can see, from the start: it knows where the enemy was before the fight, not where it went
    // once hidden.
    if (state.Quarry != enemy->GetGUID())
    {
        state.Quarry = enemy->GetGUID();
        state.QuarrySeen = false;
        state.StealthDecided = false;
    }

    bool const visible = Encoding::CanSee(player, enemy);
    if (visible)
    {
        state.QuarrySeen = true;
        state.LastSeen.Relocate(enemy);
    }

    if (player->IsNonMeleeSpellCast(false))
        return;

    if (AptitudeDemand::KeepsThemUp().MetBy(state.Apt) && player->GetHealthPct() < tuning.SelfHealBelow * 100.0f
        && !state.Heals.empty() && nowMs >= state.NextHealMs)
    {
        player->GetMotionMaster()->Clear();
        player->StopMoving();
        CastSometimes(player, state.Heals, player, nowMs, state.NextHealMs, tuning.HealMinMs, tuning.HealMaxMs);
        return;
    }

    if (nowMs < state.EngageMs)
        return;

    // A rogue or a feral druid may sneak up: once per engagement, before any fighting.
    if (!state.StealthDecided)
    {
        state.StealthDecided = true;
        state.Tactics = roll_chance_i(tuning.TacticsChance);
        state.NextControlMs = nowMs + urand(0, tuning.ControlMinMs);
        state.Sneaking = !state.Stealths.empty() && !player->IsInCombat()
            && roll_chance_i(tuning.StealthChance);
        state.SneakUntilMs = nowMs + SNEAK_MS;
    }

    // Getting into stealth can take two casts (Cat Form, then Prowl), a global cooldown apart, so it runs over
    // several updates: shift if the stealth needs a form, then stealth. Combat, or the time limit, ends the attempt.
    if (state.Sneaking)
    {
        SpellInfo const* stealth = sSpellMgr->GetSpellInfo(state.Stealths.front());
        if (player->HasStealthAura() || player->IsInCombat() || nowMs >= state.SneakUntilMs || !stealth)
            state.Sneaking = false;
        else if (state.StealthForm && !InRequiredForm(player, stealth->Stances))
        {
            TryCast(player, state.StealthForm, player);
            return;
        }
        else if (TryAny(player, state.Stealths, player))
            state.Sneaking = false;
        else
            return;
    }

    // Survive first: break crowd control, then a defensive when low.
    float const health = player->GetHealthPct() / 100.0f;
    if (state.Tactics && health < tuning.BreakBelow && Encoding::IsCrowdControlled(player)
        && TryAny(player, state.Breaks, player))
        return;
    if (state.Tactics && health < tuning.DefensiveBelow && TryAny(player, state.Defensives, player))
        return;

    if (!visible)
    {
        Search(player, nowMs, state);
        return;
    }

    if (state.Tactics)
    {
        // Stop a cast, then crowd control now and then (not an enemy already under it).
        if (enemy->IsNonMeleeSpellCast(false) && TryAny(player, state.Interrupts, enemy))
            return;

        if (nowMs >= state.NextControlMs && !Encoding::IsCrowdControlled(enemy) && !state.Controls.empty())
        {
            uint32 const control = state.Controls[urand(0, uint32(state.Controls.size()) - 1)];
            bool const controlled = TryCast(player, control, enemy);
            state.NextControlMs = nowMs
                + (controlled ? urand(tuning.ControlMinMs, tuning.ControlMaxMs) : CAST_RETRY_MS);
            if (controlled)
                return;
        }

        // A caster with an enemy on top of it slows or roots it before backing off.
        if (state.Ranged && player->GetDistance(enemy) < tuning.RangedMin * 0.5f
            && TryAny(player, state.Controls, enemy))
            return;
    }

    if (!state.Ranged)
    {
        // From stealth, open before the first swing breaks it.
        if (player->HasStealthAura() && player->IsWithinMeleeRange(enemy))
            for (uint32 opener : state.Openers)
                if (TryCast(player, opener, enemy))
                    return;

        Fight(player, enemy, nowMs, state, tuning);
        return;
    }

    float const distance = player->GetDistance(enemy);
    if (distance > tuning.RangedMax || distance < tuning.RangedMin * 0.5f)
    {
        MoveNear(player, enemy, (tuning.RangedMin + tuning.RangedMax) * 0.5f, nowMs, state);
        return;
    }

    if (!player->movespline->Finalized())
        return;

    player->SetFacingToObject(enemy);
    CastSometimes(player, state.Spells, enemy, nowMs, state.NextSpellMs, tuning.SpellMinMs, tuning.SpellMaxMs);
}
