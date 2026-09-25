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

#include "EncoderSupport.h"
#include "CharmInfo.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GearStats.h"
#include "SpellChecks.h"
#include "Item.h"
#include "Layout.h"
#include "MotionMaster.h"
#include "MoveSplineInit.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "DynamicObject.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatManager.h"
#include <algorithm>
#include <limits>

namespace
{
    enum EncoderSpells : uint32
    {
        SPELL_CALL_PET          = 883,      // its GCD is applied to calling a stabled beast
    };

    constexpr uint32 CALL_BEAST_GCD_MS = 1500;
    constexpr uint32 PREPARATION_GCD_MS = 1500;
    constexpr float POSITION_SCALE = 40.0f;
}

namespace Animus::Curriculum::Encoding
{
    /// How far below a seat the ground is looked for when it may be falling, and how high above it a seat has to
    /// be before it is: the same two the travel block uses (MAX_GROUND_SEARCH, AIRBORNE_ABOVE).
    constexpr float FALL_GROUND_SEARCH = 200.0f;
    constexpr float FALL_ABOVE = 2.0f;
    /// How far under the surface a seat is put back when it has come up out of the water: enough to be in it
    /// (LIQUID_MAP_IN_WATER wants the feet below the level), not enough to put the head under.
    constexpr float SURFACE_SINK = 0.5f;

    using SpellChecks::CheckCast;
    using SpellChecks::CooldownFraction;

    float RelativePosition(float coordinate, float origin)
    {
        return std::clamp((coordinate - origin) / POSITION_SCALE, -2.0f, 2.0f);
    }

    SpellCastTargets TargetsFor(SpellInfo const* info, Player* bot, Unit* target, Unit* friendUnit)
    {
        SpellCastTargets targets;

        // No target (between gauntlet pulls): only self-cast spells can succeed.
        if (target && (info->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION))
            targets.SetDst(*target);

        if (info->NeedsExplicitUnitTarget() && target && !info->IsPositive())
            targets.SetUnitTarget(target);
        else if (info->NeedsExplicitUnitTarget() && info->IsPositive() && friendUnit)
            targets.SetUnitTarget(friendUnit);
        else
            targets.SetUnitTarget(bot);

        return targets;
    }

    Unit* FriendUnit(SeatView const& view, uint32 slot)
    {
        Player* bot = view.Bot;
        Player* other = nullptr;
        if (slot == FRIEND_SELF)
            return bot;
        if (slot == FRIEND_OWNER)
            other = view.Owner;
        else if (slot >= FRIEND_TEAMMATE_FIRST && slot < FRIEND_SLOTS)
            other = view.Teammates[slot - FRIEND_TEAMMATE_FIRST].Bot;

        return other && other != bot && other->IsInMap(bot) ? other : nullptr;
    }

    Unit* SupportTarget(SeatView const& view)
    {
        if (!view.L || !view.L->Has(BlockId::Support))
            return view.Bot;

        Unit* selected = FriendUnit(view, view.FriendSlot);
        return selected && selected->IsAlive() ? selected : nullptr;
    }

    bool AimsAtFriend(SpellInfo const* info)
    {
        return info->IsPositive() && info->NeedsExplicitUnitTarget();
    }

    Aura const* OwnAuraOfChain(Unit const* unit, SpellInfo const* info, ObjectGuid caster)
    {
        for (SpellInfo const* rank = info->GetFirstRankSpell(); rank; rank = rank->GetNextRankSpell())
            if (Aura const* aura = unit->GetAura(rank->Id, caster))
                return aura;
        return nullptr;
    }

    bool OwnAuraHasPlentyLeft(Unit const* unit, SpellInfo const* info, ObjectGuid caster)
    {
        Aura const* aura = OwnAuraOfChain(unit, info, caster);
        if (!aura)
            return false;

        // A shield with charges (Earth Shield, Water Shield, Lightning Shield) wears down by charges; a timed aura by
        // its duration; a permanent one never.
        if (uint8 const maxCharges = aura->CalcMaxCharges())
            return float(aura->GetCharges()) > float(maxCharges) * REFRESH_BELOW_FRACTION;
        if (aura->GetMaxDuration() <= 0)
            return true;

        return float(aura->GetDuration()) > float(aura->GetMaxDuration()) * REFRESH_BELOW_FRACTION;
    }

    bool CastInProgress(Player const* bot)
    {
        return bot->IsNonMeleeSpellCast(false, true, true);
    }

    bool MountCastInProgress(Player const* bot)
    {
        Spell const* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
        return info && info->HasAura(SPELL_AURA_MOUNTED);
    }

    bool CanCast(Player* bot, SpellInfo const* info, Unit* target, Item* castItem, Unit* friendUnit)
    {
        return CheckCast(bot, info, TargetsFor(info, bot, target, friendUnit), castItem);
    }

    bool CanHeal(Player* bot, ActionCatalog::Action const& heal, Unit* ally)
    {
        SpellInfo const* info = ActionCatalog::KnownRank(bot, heal.FirstRank);
        if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id)
            || bot->GetGlobalCooldownMgr().HasGlobalCooldown(info) || CastInProgress(bot))
            return false;

        if (!bot->movespline->Finalized() && (info->CalcCastTime(bot) || info->IsChanneled()))
            return false;

        SpellCastTargets targets;
        targets.SetUnitTarget(ally);
        return CheckCast(bot, info, targets, nullptr);
    }

    SpellInfo const* KnownRank(SeatView const& view, ActionCatalog::Action const& def)
    {
        SpellInfo const* top = view.KnownRanks && def.Index < view.KnownRanks->size() ? (*view.KnownRanks)[def.Index]
            : ActionCatalog::KnownRank(view.Bot, def.FirstRank);
        // Every stage can down-rank: the choice is a core action now (CoreBlock::ACTION_RANK_TIERS), because a seat
        // that can only cast the biggest heal it knows overheals by construction, and the duel is where that habit
        // is cheapest to break.
        if (!top || !def.Rankable || !view.RankTier || !view.L)
            return top;

        // The tier's share of the known ranks: tier 1 about two thirds of them up, tier 2 about a third. A lower rank
        // the spellbook keeps inactive (Player::addSpell supersedes ranks of spells that are not stackable with
        // ranks: rage, energy and runic power abilities, paladin auras, druid forms) is skipped; mana spells, heals,
        // HoTs and shields included, keep every rank.
        uint32 known = 0;
        for (SpellInfo const* rank = top; rank; rank = rank->GetPrevRankSpell())
            ++known;

        uint32 const tier = std::min(view.RankTier, RANK_TIERS - 1);
        uint32 const wanted = std::max<uint32>(1, (known * (RANK_TIERS - tier) + RANK_TIERS - 1) / RANK_TIERS);
        SpellInfo const* chosen = top;
        uint32 position = known;
        for (SpellInfo const* rank = top->GetPrevRankSpell(); rank && position > wanted;
            rank = rank->GetPrevRankSpell())
        {
            --position;
            if (view.Bot->HasActiveSpell(rank->Id))
                chosen = rank;
        }

        return chosen;
    }

    /// The form the seat would have to leave to take `info`, or null when it can cast as it stands: a form spell
    /// pressed from another form. Every druid form carries SPELL_ATTR0_NOT_SHAPESHIFTED, so the core refuses Cat
    /// Form from Bear Form and Aquatic Form from Travel Form outright; a player shifts straight across because the
    /// client drops the form it is in first. The breathe drill's druids took Travel Form on the bank in their first
    /// decisions and were never offered Aquatic Form again in the episode.
    SpellInfo const* FormToDropFor(Player const* bot, SpellInfo const* info)
    {
        if (!info->HasAura(SPELL_AURA_MOD_SHAPESHIFT) || bot->GetShapeshiftForm() == FORM_NONE)
            return nullptr;
        if (info->CheckShapeshift(bot->GetShapeshiftForm()) == SPELL_CAST_OK)
            return nullptr;
        return CancellableForm(bot);
    }

    bool IsSpellActionAllowed(SeatView const& view, Unit* target, ActionCatalog::Action const& def)
    {
        Player* bot = view.Bot;

        // Cheap rejections before the full cast check.
        SpellInfo const* info = KnownRank(view, def);
        if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id) || CastInProgress(bot))
            return false;

        if (def.NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
            return false;

        if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
            return false;

        // With movement actions (duel block on): server-driven movement does not set the movement flags CheckCast
        // looks at, so no cast-time or channeled spell while running.
        if (view.L->Has(BlockId::Duel) && !bot->movespline->Finalized()
            && (info->CalcCastTime(bot) || info->IsChanneled()))
            return false;

        // Heals, shields and buffs go to the selected friend (the bot itself without the support block); a friend who
        // is gone or dead takes none. Casts that can only be wasted are not offered: a heal with nothing else to it on
        // a friend at full health, and an aura the bot already keeps up there with plenty left.
        Unit* friendUnit = nullptr;
        if (info->IsPositive())
        {
            friendUnit = AimsAtFriend(info) ? SupportTarget(view) : bot;
            if (!friendUnit)
                return false;

            if (def.DirectHeal && friendUnit->IsFullHealth())
                return false;

            if (def.KeepsAura && OwnAuraHasPlentyLeft(friendUnit, info, bot->GetGUID()))
                return false;
        }

        // A form from another form: judged as if standing in none, since the press drops the current one first
        // (ApplySpellAction). The full cast check cannot be asked that question, so this is the shapeshift rule
        // alone; a press the core still refuses simply does nothing, as any masked-through press does.
        if (FormToDropFor(bot, info))
            return info->CheckShapeshift(FORM_NONE) == SPELL_CAST_OK;

        return CanCast(bot, info, target, nullptr, friendUnit);
    }

    bool ApplySpellAction(SeatView const& view, Unit* target, ActionCatalog::Action const& def,
        SeatActionResult& result)
    {
        Player* bot = view.Bot;
        if (def.NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
            return false;

        SpellInfo const* info = KnownRank(view, def);
        if (!info || !bot->HasActiveSpell(info->Id) || CastInProgress(bot))
            return false;

        // Same path as CMSG_CAST_SPELL. prepare() runs the full cast validation again, so a masked action
        // from a misbehaving client simply fails. The spell owns and frees itself.
        Unit* friendUnit = info->IsPositive() && AimsAtFriend(info) ? SupportTarget(view) : bot;
        if (info->IsPositive() && !friendUnit)
            return false;

        bool const onFullHealth = def.DirectHeal && friendUnit && friendUnit->IsFullHealth();
        SpellCastTargets targets = TargetsFor(info, bot, target, friendUnit);
        bool const stealthed = bot->HasStealthAura();
        bool const targetCasting = target && target->IsNonMeleeSpellCast(false);
        uint32 const castMs = info->CalcCastTime(bot);
        // Shifting straight from one form to another: leave the one the seat is in, as the client does for a
        // player, then cast. A cast the core then refuses has cost the form, which is what it costs a player too.
        if (SpellInfo const* current = FormToDropFor(bot, info))
            bot->RemoveAurasDueToSpell(current->Id);
        Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
        if (spell->prepare(&targets) != SPELL_CAST_OK)
            return false;

        ++result.SpellCasts;
        result.HealsOnFull += onFullHealth ? 1 : 0;
        result.DefensiveCasts += def.Defensive ? 1 : 0;
        result.BreathingCasts += def.WaterBreathing ? 1 : 0;
        if (def.Healing)
        {
            ++result.HealingCasts;
            SpellInfo const* top = view.KnownRanks && def.Index < view.KnownRanks->size()
                ? (*view.KnownRanks)[def.Index] : nullptr;
            result.DownrankedCasts += top && top != info ? 1 : 0;

            // What the heal cost, read from the spell rather than from the power bar: a bar reading also moves with
            // regeneration, another spell's cost and the fight's own drains. Healing is judged by what it restores
            // for what it spends, and the cost is the half nothing measured until now.
            if (bot->getPowerType() == POWER_MANA)
                result.HealingPowerSpent += uint32(std::max(0, info->CalcPowerCost(bot, info->GetSchoolMask())));
        }

        // Getting ready before a fight: a buff, a form or stance, stealth, a pet summoned, something conjured. What it
        // takes (its cast, or the global cooldown of an instant one) is refunded from the stall grace.
        if (info->IsPositive() && !bot->IsInCombat())
            result.PreparationMs = std::max(castMs, PREPARATION_GCD_MS);

        // A harmful spell from stealth. One that breaks it commits to the fight (Ambush, Garrote, Cheap Shot, Pounce,
        // an Aimed Shot out of Shadowmeld) and cannot be repeated without earning stealth back; one that keeps it (Sap,
        // Distract, Premeditation) sets the fight up and is paid once per target per stealth, so it cannot be farmed.
        bool const atEnemy = !info->IsPositive() || info->HasEffect(SPELL_EFFECT_DISTRACT);
        if (view.L->Has(BlockId::Duel) && stealthed && target && target != bot && atEnemy)
        {
            if (info->HasAttribute(SPELL_ATTR1_ALLOW_WHILE_STEALTHED))
                result.StealthUtilityTarget = target->GetGUID();
            else
                result.StealthOpener = true;
        }

        // An interrupt attempt on a casting enemy; the scenario checks next decision whether the cast stopped.
        // Any stage, not only the ones with a pack block: the duel meets casters now (Difficulty.CasterChance), and
        // an interrupt there is the cheapest place to learn what interrupting is for.
        if (targetCasting && target != bot && ActionCatalog::IsInterruptingSpell(info))
            result.PendingInterrupt = target->GetGUID();

        return true;
    }

    SpellInfo const* TrinketSpell(Item const* item)
    {
        return item ? GearStats::ItemUseSpell(item->GetTemplate()) : nullptr;
    }

    SpellInfo const* UseSpell(uint32 itemEntry)
    {
        return GearStats::ItemUseSpell(sObjectMgr->GetItemTemplate(itemEntry));
    }

    bool CanUseItemOn(Player* bot, uint32 entry, Unit* target)
    {
        SpellInfo const* info = entry ? UseSpell(entry) : nullptr;
        Item* item = info ? bot->GetItemByEntry(entry) : nullptr;
        if (!item || !target || !bot->IsAlive() || bot->HasSpellCooldown(info->Id) || CastInProgress(bot))
            return false;

        // Server-driven movement does not set the movement flags CheckCast looks at.
        if (!bot->movespline->Finalized() && (info->CalcCastTime(bot) || info->IsChanneled()))
            return false;

        SpellCastTargets targets;
        targets.SetUnitTarget(target);
        return CheckCast(bot, info, targets, item);
    }

    bool UseItemOn(Player* bot, uint32 entry, Unit* target)
    {
        Item* item = entry ? bot->GetItemByEntry(entry) : nullptr;
        if (!item || !target)
            return false;

        uint32 const before = bot->GetItemCount(entry);
        SpellCastTargets targets;
        targets.SetUnitTarget(target);
        bot->CastItemUseSpell(item, targets, 1, 0);
        return bot->GetItemCount(entry) < before;
    }

    float ItemCooldownFraction(Player const* bot, uint32 entry)
    {
        SpellInfo const* info = entry ? UseSpell(entry) : nullptr;
        return info ? CooldownFraction(bot, info) : 0.0f;
    }

    bool CanRevive(SeatView const& view, ActionCatalog::Action const& revive, Player* ally)
    {
        Player* bot = view.Bot;
        if (!ally || !bot->IsAlive() || !ally->IsInMap(bot))
            return false;

        if (revive.Type == ActionCatalog::Kind::Soulstone)
        {
            SpellInfo const* info = view.Supplies.Soulstone ? UseSpell(view.Supplies.Soulstone) : nullptr;
            return info && ally->IsAlive() && !ally->HasAura(info->Id)
                && CanUseItemOn(bot, view.Supplies.Soulstone, ally);
        }

        return !ally->IsAlive() && !ally->isResurrectRequested() && CanHeal(bot, revive, ally);
    }

    void Revive(SeatView const& view, ActionCatalog::Action const& revive, Player* ally, SeatActionResult& result)
    {
        if (!CanRevive(view, revive, ally))
            return;

        if (revive.Type == ActionCatalog::Kind::Soulstone)
        {
            if (UseItemOn(view.Bot, view.Supplies.Soulstone, ally))
                ++result.ConsumablesUsed;
            return;
        }

        SpellInfo const* info = ActionCatalog::KnownRank(view.Bot, revive.FirstRank);
        SpellCastTargets targets;
        targets.SetUnitTarget(ally);
        Spell* spell = new Spell(view.Bot, info, TRIGGERED_NONE);
        if (spell->prepare(&targets) == SPELL_CAST_OK)
        {
            ++result.SpellCasts;
            ++result.Revives;
        }
    }

    void WriteRevives(SeatView const& view, float* out)
    {
        Player* bot = view.Bot;
        std::vector<ActionCatalog::Action> const& revives = view.L->AllyRevives;
        for (uint32 i = 0; i < revives.size(); ++i)
        {
            if (revives[i].Type == ActionCatalog::Kind::Soulstone)
            {
                if (view.Supplies.Soulstone && bot->GetItemCount(view.Supplies.Soulstone))
                {
                    out[i * 2] = 1.0f;
                    out[i * 2 + 1] = ItemCooldownFraction(bot, view.Supplies.Soulstone);
                }
            }
            else if (SpellInfo const* info = ActionCatalog::KnownRank(bot, revives[i].FirstRank))
            {
                out[i * 2] = 1.0f;
                out[i * 2 + 1] = CooldownFraction(bot, info);
            }
        }
    }

    Unit* FirstPet(Player* bot)
    {
        if (Pet* pet = bot->GetPet())
            return pet;

        for (Unit* controlled : bot->m_Controlled)
            if (controlled->IsAlive() && !controlled->IsTotem())
                return controlled;

        return nullptr;
    }

    SpellInfo const* CancellableForm(Player const* bot)
    {
        // Stances and presences cannot be cancelled from the client.
        for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_MOD_SHAPESHIFT))
        {
            SpellInfo const* info = effect->GetSpellInfo();
            if (!info->HasAttribute(SPELL_ATTR0_NO_AURA_CANCEL) && info->IsPositive() && !info->IsPassive())
                return info;
        }

        return nullptr;
    }

    bool FindNearestHazard(Unit const* unit, float range, Hazard& out)
    {
        if (!unit || !unit->IsInWorld() || range <= 0.0f)
            return false;

        float nearestEdge = std::numeric_limits<float>::max();
        auto const consider = [&](WorldObject* object, float radius)
        {
            if (radius <= 0.0f)
                return;

            float const distance = unit->GetDistance2d(object);
            float const edge = distance - radius;     // negative: the unit is already inside it
            if (edge >= nearestEdge)
                return;

            nearestEdge = edge;
            out.Distance = distance;
            out.Radius = radius;
            out.Bearing = unit->GetRelativeAngle(object);
            out.Centre.Relocate(object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
            out.Present = true;
        };

        auto const worker = [&](WorldObject* object)
        {
            if (DynamicObject* area = object->ToDynObject())
            {
                // Only what would hurt this unit: a friendly caster's ground effect is somewhere to stand, not to
                // leave, and the seat's own consecration is not a hazard to it.
                Unit* caster = area->GetCaster();
                SpellInfo const* info = sSpellMgr->GetSpellInfo(area->GetSpellId());
                if (caster && info && !info->IsPositive() && !unit->IsFriendlyTo(caster))
                    consider(area, area->GetRadius());
                return;
            }

            if (GameObject* object3d = object->ToGameObject();
                object3d && object3d->GetGoType() == GAMEOBJECT_TYPE_TRAP)
            {
                GameObjectTemplate const* info = object3d->GetGOInfo();
                SpellInfo const* spell = info ? sSpellMgr->GetSpellInfo(info->trap.spellId) : nullptr;
                if (spell && !spell->IsPositive())
                    // A trap arms an area of `diameter` around itself.
                    consider(object3d, std::max(1.0f, float(info->trap.diameter) / 2.0f));
            }
        };

        Acore::WorldObjectWorker<decltype(worker)> searcher(unit, worker,
            GRID_MAP_TYPE_MASK_DYNAMICOBJECT | GRID_MAP_TYPE_MASK_GAMEOBJECT);
        Cell::VisitObjects(unit, searcher, range);
        return out.Present;
    }

    Debuffs IncomingDebuffs(Unit const* unit)
    {
        Debuffs debuffs;
        if (!unit)
            return debuffs;

        for (auto const& [spellId, application] : unit->GetAppliedAuras())
        {
            if (!application || application->IsPositive())
                continue;

            Aura const* aura = application->GetBase();
            SpellInfo const* info = aura ? aura->GetSpellInfo() : nullptr;
            if (!info)
                continue;

            ++debuffs.Count;
            switch (info->Dispel)
            {
                case DISPEL_MAGIC:
                case DISPEL_CURSE:
                case DISPEL_DISEASE:
                case DISPEL_POISON:
                    ++debuffs.Dispellable;
                    break;
                default:
                    break;
            }

            debuffs.Stacks = std::max(debuffs.Stacks, uint32(aura->GetStackAmount()));
            if (int32 const left = aura->GetDuration(); left > 0)
                debuffs.LongestMs = std::max(debuffs.LongestMs, uint32(left));

            uint64 const mechanics = info->GetAllEffectsMechanicMask();
            for (std::size_t mechanic = 0; mechanic < OBSERVED_MECHANICS.size(); ++mechanic)
                if (mechanics & (uint64(1) << OBSERVED_MECHANICS[mechanic]))
                    debuffs.Mechanics[mechanic] = true;
        }

        return debuffs;
    }

    uint32 StandingInHazards(Unit const* unit, Hazard* deepest)
    {
        if (!unit)
            return 0;

        uint32 count = 0;
        float deepestLeft = -1.0f;
        for (auto const& [spellId, application] : unit->GetAppliedAuras())
        {
            if (!application || application->IsPositive())
                continue;

            Aura const* aura = application->GetBase();
            if (!aura || aura->GetType() != DYNOBJ_AURA_TYPE)
                continue;

            DynamicObject const* object = aura->GetDynobjOwner();
            if (!object)
                continue;

            ++count;
            if (!deepest)
                continue;

            // Deepest by how far there still is to walk: the radius it has to leave, less how far out it already is.
            float const distance = unit->GetDistance2d(object);
            float const left = object->GetRadius() - distance;
            if (left <= deepestLeft)
                continue;

            deepestLeft = left;
            deepest->Distance = distance;
            deepest->Radius = object->GetRadius();
            deepest->Bearing = unit->GetRelativeAngle(object);
            deepest->Present = true;
        }

        return count;
    }

    float ThreatShare(Unit const* enemy, Unit const* unit)
    {
        if (!enemy || !unit)
            return 0.0f;

        ThreatManager const& threat = enemy->GetThreatMgr();
        if (!threat.CanHaveThreatList())
            return 0.0f;

        float const mine = threat.GetThreat(unit);
        if (mine <= 0.0f)
            return 0.0f;

        // The enemy's own victim tops its list in every case that matters here, and asking for one unit's threat is
        // far cheaper than walking the sorted list every decision for every seat.
        Unit const* victim = threat.GetLastVictim();
        float const top = victim ? std::max(mine, threat.GetThreat(victim)) : mine;
        return top > 0.0f ? std::min(1.0f, mine / top) : 0.0f;
    }

    bool IsCrowdControlled(Unit const* unit)
    {
        return unit->HasUnitState(CROWD_CONTROL_STATES) || unit->HasAuraType(SPELL_AURA_MOD_SILENCE)
            || unit->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE) || unit->HasAuraType(SPELL_AURA_TRANSFORM);
    }

    void WriteOpponentType(Unit const* unit, float* out)
    {
        uint32 const type = unit->IsPlayer() ? CREATURE_TYPE_HUMANOID : unit->GetCreatureType();
        for (uint32 i = 0; i < OPPONENT_TYPES.size(); ++i)
            out[i] = OPPONENT_TYPES[i] == type ? 1.0f : 0.0f;
    }

    float DamageModifier(Unit const* unit)
    {
        Creature const* creature = unit->ToCreature();
        return creature && creature->GetCreatureTemplate() ? creature->GetCreatureTemplate()->DamageModifier : 1.0f;
    }

    float ArmorReduction(Unit const* unit, uint8 attackerLevel)
    {
        // Unit::CalcArmorReducedDamage, before armor penetration.
        float const value = 0.1f * float(unit->GetArmor()) / (8.5f * float(attackerLevel) + 40.0f);
        return std::clamp(value / (1.0f + value), 0.0f, 0.75f);
    }

    bool IsImmuneToSchool(Unit const* unit, SpellSchools school)
    {
        return unit->IsImmunedToDamageOrSchool(SpellSchoolMask(1 << school));
    }

    bool IsImmuneToMechanic(Unit const* unit, Mechanics mechanic)
    {
        auto const& immune = unit->m_spellImmune[IMMUNITY_MECHANIC];
        return immune.count(mechanic) > 0 || (mechanic == MECHANIC_FEAR && immune.count(MECHANIC_HORROR) > 0);
    }

    int32 SlotOf(SeatView const& view, Unit const* unit)
    {
        if (!unit)
            return -1;

        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            if (view.Enemies[slot] == unit)
                return int32(slot);

        return -1;
    }

    int32 SlotAttacking(SeatView const& view, Unit const* victim, uint32 except)
    {
        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            if (Unit* enemy = view.Enemies[slot]; enemy && enemy->IsAlive() && enemy->GetVictim() == victim
                && slot != except)
                return int32(slot);

        return -1;
    }

    void SelectEnemy(SeatView& view, uint32 slot)
    {
        Unit* enemy = view.Enemies[slot];
        view.TargetSlot = slot;
        view.Bot->SetSelection(enemy->GetGUID());

        if (view.Bot->GetVictim())
            view.Bot->Attack(enemy, view.Bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING));
    }

    bool SnapToGround(Map const* map, uint32 phaseMask, Position& at, float fromZ, float maxStep)
    {
        if (!map)
            return false;

        float const z = map->GetHeight(phaseMask, at.GetPositionX(), at.GetPositionY(), fromZ + maxStep, true,
            maxStep * 2.0f);
        if (z <= INVALID_HEIGHT || std::fabs(z - fromZ) > maxStep)
            return false;

        at.Relocate(at.GetPositionX(), at.GetPositionY(), z);
        return true;
    }

    bool CanSee(WorldObject const* watcher, WorldObject const* target)
    {
        return watcher && target && watcher->CanSeeOrDetect(target) && watcher->IsWithinLOSInMap(target);
    }

    void MoveTo(Player* bot, uint32 pointId, float x, float y, float z, float const* facing)
    {
        bot->GetMotionMaster()->Clear();
        if (!facing)
        {
            bot->GetMotionMaster()->MovePoint(pointId, x, y, z);
            return;
        }

        // MovePoint cannot carry a facing, and a facing set on a spline of its own is not a way round that: the
        // second Launch replaces the first, so one of the two is always thrown away -- either the seat turns and
        // stops walking, or it walks and never turns. They have to be the same spline. generatePath matches what
        // MovePoint does, so a bearing is still a direction the seat wants to go rather than a licence to walk
        // through a wall.
        //
        // SetFacing alone was not enough, and this is the whole of why no seat could hold a heading. It raises
        // Final_Angle, and MoveSpline::ComputePosition applies that angle only under `splineflags.done`; any
        // unfinished spline has its orientation overwritten every tick with the direction of travel,
        // atan2(hermite.y, hermite.x), which Unit::UpdateSplinePosition writes onto the player. A held bearing
        // re-launches this spline every 250 ms decision towards a point over a second away, so it never finishes
        // and the facing was thrown away every time. Orientation then *was* the travel direction, and since a
        // bearing is measured off it, holding anything but straight ahead rotated the frame 45 degrees a
        // decision: the seat spiralled.
        //
        // Two things are needed. OrientationFixed stops the overwrite for the spline's whole life rather than
        // only at its end. And the spline must be interrupted before Launch, because Launch seeds
        // args.initialOrientation from ComputePosition while a spline is still live -- MotionMaster::Clear does
        // not finalise it, since these are raw MoveSplineInits and never set UNIT_STATE_MOVING, so
        // IdleMovementGenerator::Reset never calls StopMoving. Without the interrupt the new spline inherits the
        // old one's travel direction and the facing asked for here is discarded a second way.
        bot->DisableSpline();
        // UpdatePosition rather than SetOrientation: same x/y/z makes `relocated` false and `turn` true, so this
        // fires AURA_INTERRUPT_FLAG_TURNING exactly as a real turn does. Nothing in stage 1 casts, but the
        // combat stages do.
        bot->UpdatePosition(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), *facing);

        Movement::MoveSplineInit init(bot);
        init.MoveTo(x, y, z, true);
        init.SetOrientationFixed(true);
        init.SetFacing(*facing);        // and for the final tick, on the rare spline that does finish
        init.Launch();
    }

    void SwimTo(Player* bot, float x, float y, float z, float const* facing)
    {
        // Straight there, no pathfinding: the walkable mesh stops at the waterline -- mmaps drops the terrain
        // under real liquid -- so a pathfound step into a lake has nowhere to land and the seat stands on the
        // shore instead. That was the whole of why no seat ever swam: swimming needed the seat to be in water
        // already, and getting in was a ground move the mesh would not take.
        //
        // No SetFly: a swimming unit is not a flying one, and telling the client otherwise is a different bug.
        bot->GetMotionMaster()->Clear();
        if (facing)
        {
            // Same as MoveTo: interrupt first so Launch seeds the orientation from the unit rather than from the
            // spline still in flight, then hold it for the spline's whole life.
            bot->DisableSpline();
            bot->UpdatePosition(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), *facing);
        }

        Movement::MoveSplineInit init(bot);
        init.MoveTo(x, y, z, false, true);
        if (facing)
        {
            init.SetOrientationFixed(true);
            init.SetFacing(*facing);
        }
        init.Launch();
    }

    void JumpTo(Player* bot, float x, float y, float z, float speedXY, float speedZ, float const* facing)
    {
        // MotionMaster::MoveJump would do most of this, and cannot be used: MoveJumpTo refuses players outright
        // ("this function may make players fall below map") and MoveJump sets no orientation-fixed flag, so the
        // spline would go back to writing the direction of travel onto the seat's orientation -- the exact bug
        // that made a held bearing spiral. A jump is built here for the same reason MoveTo is.
        //
        // No pathfinding, deliberately: a jump is the one move that leaves the navmesh, and asking the
        // pathfinder to route it would either refuse it or walk the seat round. Whether there is anywhere to
        // land is decided before this is called, because nothing here can undo a bad landing.
        bot->GetMotionMaster()->Clear();
        bot->DisableSpline();
        if (facing)
            bot->UpdatePosition(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), *facing);

        float const moveTimeHalf = speedZ / float(Movement::gravity);
        float const maxHeight = -Movement::computeFallElevation(moveTimeHalf, false, -speedZ);

        Movement::MoveSplineInit init(bot);
        init.MoveTo(x, y, z, false);
        init.SetParabolic(maxHeight, 0.0f);
        init.SetVelocity(speedXY);
        if (facing)
        {
            init.SetOrientationFixed(true);
            init.SetFacing(*facing);
        }
        init.Launch();
    }

    bool FallToGround(Player* bot, float* yards, float* healthFraction)
    {
        if (yards)
            *yards = 0.0f;
        if (healthFraction)
            *healthFraction = 0.0f;
        // Not in water: a seat holding still under the surface is swimming, not hanging in the air, and the lakebed
        // twenty yards down is not a fall waiting to happen. Unit::IsInWater reads the terrain; Player::IsInWater
        // is the cached flag the client would have sent.
        if (!bot->IsAlive() || bot->CanFly() || !bot->movespline->Finalized() || bot->Unit::IsInWater())
            return false;

        float const ground = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), true,
            FALL_GROUND_SEARCH);
        if (ground <= INVALID_HEIGHT)
            return false;

        // Over water the surface is where a fall ends, not the bed under it: Map::GetHeight is blind to liquid, so
        // a seat that has just broken the surface of a lake sixty yards deep read as sixty yards up in the air,
        // and was handed a sixty-yard fall's damage -- every time it came up for air. That was the chain drill's
        // "died without damage": a quarter of its seats, each one killed on the third or fourth breath. Within a
        // step of the surface the seat is swimming, not hanging, and the game charges nothing for landing in deep
        // water, so a real drop into it ends at the surface with the seat back in the water and nothing lost.
        LiquidData const liquid = bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), bot->GetPositionX(),
            bot->GetPositionY(), ground, bot->GetCollisionHeight(), {});
        bool const overWater = liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > ground
            && (liquid.Flags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) != 0;
        if (overWater)
        {
            float const above = bot->GetPositionZ() - liquid.Level;
            if (above <= 0.0f)
                return false;
            bot->GetMotionMaster()->Clear();
            bot->DisableSpline();
            bot->UpdatePosition(bot->GetPositionX(), bot->GetPositionY(), liquid.Level - SURFACE_SINK,
                bot->GetOrientation());
            if (above <= FALL_ABOVE)
                return false;
            if (yards)
                *yards = above;
            return true;
        }

        if (bot->GetPositionZ() - ground <= FALL_ABOVE)
            return false;

        // MoveFall remembers where the fall started (SetFallInformation); landing is Player::HandleFall, as for a
        // client. The health before and after is the cost, as a fraction of the most the character can have.
        uint32 const before = bot->GetHealth();
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MoveFall();

        MovementInfo landing = bot->m_movementInfo;
        landing.pos.Relocate(bot->GetPositionX(), bot->GetPositionY(), ground);
        bot->HandleFall(landing);
        bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING);

        if (yards)
            *yards = bot->GetPositionZ() - ground;
        if (healthFraction && bot->GetMaxHealth())
            *healthFraction = float(before - std::min(before, bot->GetHealth())) / float(bot->GetMaxHealth());
        return true;
    }

    void FlyTo(Player* bot, float x, float y, float z, float const* facing)
    {
        // Deliberately NOT orientation-fixed, unlike MoveTo and SwimTo. SetFly puts the spline in Catmullrom
        // mode, and Spline::init_spline places the virtual first control point at
        // controls[0] - (cos(initialOrientation), sin(initialOrientation)): a facing off the direction of travel
        // would bend the start of the flight path rather than only turn the seat's head. Flying seats therefore
        // keep the old face-along-the-path behaviour until a stage needs to strafe in the air.
        bot->GetMotionMaster()->Clear();
        Movement::MoveSplineInit init(bot);
        init.MoveTo(x, y, z, false, true);
        if (facing)
            init.SetFacing(*facing);
        init.SetFly();
        init.Launch();
    }

    bool PetAttack(Player* bot, Unit* target)
    {
        bool ordered = false;
        for (Unit* controlled : bot->m_Controlled)
        {
            Creature* pet = controlled->ToCreature();
            if (!pet || !pet->IsAlive() || !pet->IsAIEnabled || pet->GetVictim() == target
                || !pet->CanCreatureAttack(target))
                continue;

            // HandlePetActionHelper, COMMAND_ATTACK.
            pet->ClearUnitState(UNIT_STATE_FOLLOW);
            pet->AttackStop();
            if (CharmInfo* charmInfo = pet->GetCharmInfo())
            {
                charmInfo->SetIsCommandAttack(true);
                charmInfo->SetIsAtStay(false);
                charmInfo->SetIsFollowing(false);
                charmInfo->SetIsCommandFollow(false);
                charmInfo->SetIsReturning(false);
            }

            pet->AI()->AttackStart(target);
            ordered = true;
        }

        return ordered;
    }

    void StartCallBeastCooldown(Player* bot)
    {
        if (SpellInfo const* callPet = sSpellMgr->GetSpellInfo(SPELL_CALL_PET))
            bot->GetGlobalCooldownMgr().AddGlobalCooldown(callPet, CALL_BEAST_GCD_MS);
    }
}
