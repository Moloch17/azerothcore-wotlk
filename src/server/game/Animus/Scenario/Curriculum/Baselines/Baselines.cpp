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

#include "Baselines.h"
#include "ClassAssets.h"
#include "CoreBlock.h"
#include "DuelBlock.h"
#include "GauntletBlock.h"
#include "MoveBlock.h"
#include "PetBlock.h"
#include "SupportBlock.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TravelBlock.h"
#include "WorldBlock.h"
#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace
{
    using namespace Animus::Curriculum;

    constexpr float EAT_BELOW = 0.8f;
    constexpr float DRINK_BELOW = 0.8f;
    constexpr float HEAL_BELOW = 0.6f;          // the most hurt living friend below this is healed
    constexpr float DEFENSIVE_BELOW = 0.3f;     // the bot below this uses a defensive
    constexpr float CLOSE_IN_BEYOND_YARDS = 4.0f;
    constexpr float HOLD_RANGE_BEYOND_YARDS = 28.0f;    // ranged specs close in from beyond this ...
    constexpr float HOLD_RANGE_WITHIN_YARDS = 24.0f;    // ... and stop once inside this
    constexpr float MOUNT_BEYOND_YARDS = 80.0f;
    /// Nearer than half a bearing to straight ahead, a bearing does the aiming; further off, the held turn does.
    constexpr float TURN_WITHIN_RADIANS = 0.3926991f;
    constexpr float CRUISE_HEIGHT_YARDS = 20.0f;
    /// How much walkable ground is worth against pointing the right way, when choosing a bearing. At 1.0 a
    /// bearing onto ground the seat can cross beats one aimed straight at the objective and into a cliff, and a
    /// 45-degree detour onto good ground beats a blocked straight line -- which is the whole difference between
    /// steering and holding forward.
    constexpr float GROUND_OVER_AIM = 1.0f;

    /// A seat's row, read by block: features and actions by their block-relative index.
    class Row
    {
    public:
        Row(Layout const& layout, float const* obs, uint8 const* mask) : _layout(layout), _obs(obs), _mask(mask) { }

        [[nodiscard]] bool Has(BlockId block) const { return _layout.Has(block); }

        [[nodiscard]] float Obs(BlockId block, uint32 feature) const
        {
            return _obs[_layout.Slice(block).ObsFirst + feature];
        }

        /// A feature by its absolute index in the row, for the parts of a block that sit past its named features
        /// (the core block's per-talent and per-tree tail, whose offsets the block computes).
        [[nodiscard]] float ObsAt(uint32 feature) const { return _obs[feature]; }

        /// The row's action for `action` of `block`, if the layout has it and it is allowed.
        [[nodiscard]] std::optional<int32> Allowed(BlockId block, uint32 action) const
        {
            BlockSlice const& slice = _layout.Slice(block);
            if (!Has(block) || action >= slice.ActionCount || !_mask[slice.ActionFirst + action])
                return std::nullopt;
            return int32(slice.ActionFirst + action);
        }

    private:
        Layout const& _layout;
        float const* _obs;
        uint8 const* _mask;
    };

    /// The summons a pet class's baseline casts when its pet is not out, best first: a warlock's demons from the one
    /// that soaks a fight best, a death knight's ghoul, a frost mage's elemental.
    constexpr std::array<uint32, 7> PET_SUMMONS =
    {
        30146,  // Summon Felguard
        697,    // Summon Voidwalker
        691,    // Summon Felhunter
        712,    // Summon Succubus
        688,    // Summon Imp
        46584,  // Raise Dead
        31687,  // Summon Water Elemental
    };

    /// A pet class's pet: call a hunter's first stable beast or cast the best summon when no living pet is out (out
    /// of combat only: a demon takes seconds to summon), then send the pet at the target.
    std::optional<int32> PetAction(Row const& row, Layout const& layout)
    {
        if (!row.Has(BlockId::Pet) || !PetBlock::HasPet(layout.Profile->Class))
            return std::nullopt;

        bool const petOut = row.Obs(BlockId::Pet, PetBlock::OBS_PRESENT) > 0.0f
            && row.Obs(BlockId::Pet, PetBlock::OBS_ALIVE) > 0.0f;
        bool const inCombat = row.Obs(BlockId::Duel, DuelBlock::OBS_BOT_IN_COMBAT) > 0.0f;

        if (!petOut && !inCombat)
        {
            for (uint32 slot = 0; slot < STABLE_SLOTS; ++slot)
                if (std::optional<int32> call = row.Allowed(BlockId::Duel, DuelBlock::ACTION_CALL_BEAST_FIRST + slot))
                    return call;

            std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
            for (uint32 summon : PET_SUMMONS)
                for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
                    if (actions[action].Type == ActionCatalog::Kind::Spell && actions[action].FirstRank == summon)
                        if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                            return cast;
        }

        if (petOut)
            if (std::optional<int32> send = row.Allowed(BlockId::Duel, DuelBlock::ACTION_PET_ATTACK))
                return send;

        return std::nullopt;
    }

    /// While the pet attacks the target: its first allowed damaging ability, as autocast would. Pets no longer
    /// autocast, and an Imp or a Water Elemental cannot melee (PetAI::_canMeleeAttack), so without this they do
    /// nothing.
    std::optional<int32> PetDamage(Row const& row, Layout const& layout)
    {
        if (!row.Has(BlockId::Pet) || !PetBlock::HasPet(layout.Profile->Class)
            || row.Obs(BlockId::Pet, PetBlock::OBS_ATTACKING_TARGET) == 0.0f)
            return std::nullopt;

        for (uint32 slot = 0; slot < PetBlock::ABILITY_SLOTS; ++slot)
        {
            uint32 const first = PetBlock::OBS_SLOT_FIRST + slot * PetBlock::SLOT_FEATURES;
            if (row.Obs(BlockId::Pet, first + PetBlock::SLOT_DAMAGE) > 0.0f
                && row.Obs(BlockId::Pet, first + PetBlock::SLOT_POSITIVE) == 0.0f)
                if (std::optional<int32> cast = row.Allowed(BlockId::Pet, PetBlock::ACTION_ABILITY_FIRST + slot))
                    return cast;
        }

        return std::nullopt;
    }

    /// One of the seat's aptitude features, read from the core block. A baseline sees only what the model sees,
    /// so it reads the row rather than asking the layout -- which no longer knows, one model now covering every
    /// build its class can have.
    float AptitudeOf(Row const& row, uint32 feature)
    {
        return row.Obs(BlockId::Core, CoreBlock::OBS_APTITUDE_FIRST + feature);
    }

    /// Whether this seat can keep somebody up, and whether it can hold a pull. The same thresholds composition
    /// uses, asked of the row instead of of a character.
    bool CanHeal(Row const& row)
    {
        return AptitudeOf(row, Aptitude::DIRECT_HEAL) >= AptitudeDemand::KeepsThemUp().AtLeast;
    }

    bool CanHoldThePull(Row const& row)
    {
        return AptitudeOf(row, Aptitude::MITIGATION) >= AptitudeDemand::HoldsThePull().AtLeast;
    }

    /// The seat's spec, worked out from what it actually spent its talents on: the tree it put the most points
    /// into (a spec's TabPage is its tree), and where two specs share a tree -- a druid's feral cat and bear --
    /// how much mitigation it reads with tells them apart. Null if it has spent nothing yet.
    ///
    /// There is no spec in the observation to read instead, deliberately (CoreBlock::OBS_APTITUDE_FIRST says why),
    /// and a baseline sees only what the model sees. So it does here what the model has to do: read the build.
    SpecProfile const* SpecOf(Row const& row, Layout const& layout)
    {
        if (!row.Has(BlockId::Core))
            return nullptr;

        uint32 best = TalentBuilder::TREE_COUNT;
        float most = 0.0f;
        for (uint32 tree = 0; tree < TalentBuilder::TREE_COUNT; ++tree)
        {
            float const points = row.ObsAt(CoreBlock::TreeObsFirst(layout) + tree);
            if (points > most)
            {
                most = points;
                best = tree;
            }
        }

        // Below level 10 nothing has been spent and every tree reads 0, so the tree alone cannot answer. Two specs
        // sharing a tree are told apart by how much mitigation the seat reads with -- which is the real difference
        // between a bear and a cat, and is true of a build that took half of each.
        ClassAssets const& assets = ClassAssets::For(*layout.Profile);
        bool const holds = CanHoldThePull(row);
        SpecProfile const* fallback = nullptr;
        for (uint8 index = 0; index < uint8(layout.Profile->Specs.size()); ++index)
        {
            SpecProfile const& spec = layout.Profile->Specs[index];
            bool const tree = best != TalentBuilder::TREE_COUNT && spec.TabPage == best;
            bool const specHolds = index < assets.SpecAptitudes.size()
                && AptitudeDemand::HoldsThePull().MetBy(assets.SpecAptitudes[index]);
            if (tree && specHolds == holds)
                return &spec;
            if (!fallback && tree)
                fallback = &spec;
        }

        if (fallback)
            return fallback;

        return layout.Profile->Specs.empty() ? nullptr : &layout.Profile->Specs.front();
    }

    /// Whether the seat's spec fights from range (hunters, casters, healers).
    bool FightsFromRange(Row const& row, Layout const& layout)
    {
        SpecProfile const* spec = SpecOf(row, layout);
        return spec && spec->Range != RangeBand::Melee;
    }

    /// What a catalog spell is to the `fight` rotation.
    enum class SpellUse : uint8
    {
        Other,
        Damage,             // hits the target now: school or weapon damage, a leech, a melee or ranged weapon attack
        DamageOverTime,     // only ticks: worth casting while it isn't on the target
        Buff,               // an aura on the bot or its party with no cooldown of its own
        Form,               // a shapeshift (stances, forms, Shadowform): only the spec's own, see SpecForms
    };

    SpellUse UseOf(SpellInfo const* info)
    {
        if (!info)
            return SpellUse::Other;

        if (info->HasAura(SPELL_AURA_MOD_SHAPESHIFT))
            return SpellUse::Form;

        if (!info->IsPositive())
        {
            // Crowd control that damage breaks (Polymorph, Scatter Shot, Fear) would undo the fight's own hits.
            if (info->HasAura(SPELL_AURA_MOD_CONFUSE) || info->HasAura(SPELL_AURA_MOD_FEAR)
                || info->HasAura(SPELL_AURA_TRANSFORM))
                return SpellUse::Other;

            bool direct = info->DmgClass == SPELL_DAMAGE_CLASS_MELEE || info->DmgClass == SPELL_DAMAGE_CLASS_RANGED;
            bool periodic = false;
            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                switch (effect.Effect)
                {
                    case SPELL_EFFECT_SCHOOL_DAMAGE:
                    case SPELL_EFFECT_WEAPON_DAMAGE:
                    case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                    case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                    case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                    case SPELL_EFFECT_HEALTH_LEECH:
                        direct = true;
                        break;
                    default:
                        break;
                }

                switch (effect.ApplyAuraName)
                {
                    case SPELL_AURA_PERIODIC_DAMAGE:
                    case SPELL_AURA_PERIODIC_LEECH:
                    case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                    case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                        periodic = true;
                        break;
                    default:
                        break;
                }
            }

            return direct ? SpellUse::Damage : periodic ? SpellUse::DamageOverTime : SpellUse::Other;
        }

        // A buff to keep up, cast once: not a cooldown (a defensive saved for need), not speed (an aspect that dazes
        // when hit), not stealth or invisibility (which a fight breaks), not feigning death.
        bool const aura = info->HasEffect(SPELL_EFFECT_APPLY_AURA)
            || info->HasEffect(SPELL_EFFECT_APPLY_AREA_AURA_PARTY)
            || info->HasEffect(SPELL_EFFECT_APPLY_AREA_AURA_RAID);
        if (!aura || info->RecoveryTime || info->CategoryRecoveryTime || info->HasAura(SPELL_AURA_MOD_INCREASE_SPEED)
            || info->HasAura(SPELL_AURA_MOD_STEALTH) || info->HasAura(SPELL_AURA_MOD_INVISIBILITY)
            || info->HasAura(SPELL_AURA_FEIGN_DEATH))
            return SpellUse::Other;

        return SpellUse::Buff;
    }

    /// The form a spec fights in, best first: the rotation shifts into the first one it knows while in no form. A spec
    /// not listed fights in no form (warriors are put in their stance by SeatCharacter::PrepareFighter).
    struct SpecForm
    {
        uint8 Class;
        std::string_view Spec;
        std::array<uint32, 2> Forms;
    };

    constexpr std::array<SpecForm, 4> SPEC_FORMS =
    {{
        { CLASS_DRUID, "balance", { 24858, 0 } },           // Moonkin Form
        { CLASS_DRUID, "feral_cat", { 768, 0 } },           // Cat Form
        { CLASS_DRUID, "feral_bear", { 9634, 5487 } },      // Dire Bear Form, Bear Form
        { CLASS_PRIEST, "shadow", { 15473, 0 } },           // Shadowform
    }};

    /// The core block's feature `feature` of catalog action `action` (known, cooldown, aura on target, aura on self).
    float ActionFeature(Row const& row, uint32 action, uint32 feature)
    {
        return row.Obs(BlockId::Core, CoreBlock::OBS_GLOBAL_COUNT + action * CoreBlock::ACTION_FEATURES + feature);
    }

    constexpr uint32 ACTION_AURA_ON_TARGET = 2;
    constexpr uint32 ACTION_AURA_ON_SELF = 3;

    /// `fight`'s spells, first match wins: the spec's form while in no form; the first allowed damaging spell (one that
    /// only ticks while it isn't on the target); out of combat, a buff not already on the bot, one per exclusive kind
    /// (a seal, an aura, an armor); otherwise nothing. Casting for its own sake resets the swing timer, and the first
    /// spell in catalog order is often a buff that can be cast again forever.
    std::optional<int32> Rotation(Row const& row, Layout const& layout)
    {
        std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
        auto const castable = [&actions](uint32 action)
        {
            return actions[action].Type == ActionCatalog::Kind::Spell;
        };

        // In no form (the tracked forms' one-hot starts with FORM_NONE): the spec's own.
        SpecProfile const* spec = SpecOf(row, layout);
        if (spec && row.Obs(BlockId::Core, CoreBlock::OBS_FORM_FIRST) > 0.0f)
        {
            for (SpecForm const& entry : SPEC_FORMS)
            {
                if (entry.Class != layout.Profile->Class || entry.Spec != spec->Name)
                    continue;

                for (uint32 form : entry.Forms)
                    for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
                        if (form && castable(action) && actions[action].FirstRank == form)
                            if (std::optional<int32> shift = row.Allowed(BlockId::Core, action))
                                return shift;
            }
        }

        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
        {
            if (!castable(action))
                continue;

            SpellUse const use = UseOf(sSpellMgr->GetSpellInfo(actions[action].FirstRank));
            if (use == SpellUse::Damage
                || (use == SpellUse::DamageOverTime && ActionFeature(row, action, ACTION_AURA_ON_TARGET) == 0.0f))
                if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                    return cast;
        }

        if (row.Obs(BlockId::Duel, DuelBlock::OBS_BOT_IN_COMBAT) > 0.0f)
            return std::nullopt;

        // Exclusive kinds already on the bot: a second seal would only replace the first, and back again.
        std::vector<SpellSpecificType> active;
        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
            if (castable(action) && ActionFeature(row, action, ACTION_AURA_ON_SELF) > 0.0f)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(actions[action].FirstRank))
                    if (info->GetSpellSpecific() != SPELL_SPECIFIC_NORMAL)
                        active.push_back(info->GetSpellSpecific());

        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
        {
            if (!castable(action) || ActionFeature(row, action, ACTION_AURA_ON_SELF) > 0.0f)
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(actions[action].FirstRank);
            if (UseOf(info) != SpellUse::Buff || (info->GetSpellSpecific() != SPELL_SPECIFIC_NORMAL
                && std::find(active.begin(), active.end(), info->GetSpellSpecific()) != active.end()))
                continue;

            if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                return cast;
        }

        return std::nullopt;
    }

    /// The first allowed core spell action that `wanted` picks.
    std::optional<int32> FirstSpell(Row const& row, Layout const& layout,
        bool (*wanted)(ActionCatalog::Action const& action))
    {
        std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
            if (actions[action].Type == ActionCatalog::Kind::Spell && wanted(actions[action]))
                if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                    return cast;

        return std::nullopt;
    }

    /// `fight`'s support, as a simple healer plays: a defensive when the bot is low; the most hurt living friend below
    /// HEAL_BELOW selected and healed (the bot itself without the support block); a healer keeps its own heal over
    /// time or shield on the owner, or a tank teammate, once they are in the fight. The masks keep it from healing a
    /// friend at full health or re-casting what is still up.
    std::optional<int32> Support(Row const& row, Layout const& layout)
    {
        float const health = row.Obs(BlockId::Core, CoreBlock::OBS_HEALTH);
        if (health > 0.0f && health < DEFENSIVE_BELOW)
            if (std::optional<int32> defend = FirstSpell(row, layout,
                [](ActionCatalog::Action const& action) { return action.Defensive; }))
                return defend;

        auto const heal = [](ActionCatalog::Action const& action) { return action.Healing; };
        if (!row.Has(BlockId::Support))
            return health > 0.0f && health < HEAL_BELOW ? FirstSpell(row, layout, heal) : std::nullopt;

        auto const friendObs = [&row](uint32 slot, uint32 feature)
        {
            return row.Obs(BlockId::Support, SupportBlock::OBS_GLOBAL_COUNT + slot * SupportBlock::FRIEND_FEATURES
                + feature);
        };
        auto const aim = [&row](uint32 slot) -> std::optional<int32>
        {
            if (row.Obs(BlockId::Support, SupportBlock::OBS_SELECTED_FIRST + slot) > 0.0f)
                return std::nullopt;
            return row.Allowed(BlockId::Support, SupportBlock::ACTION_SELECT_FRIEND_FIRST + slot);
        };

        uint32 lowest = FRIEND_SLOTS;
        float lowestHealth = HEAL_BELOW;
        for (uint32 slot = 0; slot < FRIEND_SLOTS; ++slot)
        {
            if (friendObs(slot, SupportBlock::FRIEND_ALIVE) == 0.0f)
                continue;

            float const friendHealth = friendObs(slot, SupportBlock::FRIEND_HEALTH);
            if (friendHealth < lowestHealth)
            {
                lowest = slot;
                lowestHealth = friendHealth;
            }
        }

        if (lowest < FRIEND_SLOTS)
        {
            if (std::optional<int32> select = aim(lowest))
                return select;
            if (std::optional<int32> cast = FirstSpell(row, layout, heal))
                return cast;

            // Heals cannot be cast in most forms.
            if (CanHeal(row))
                if (std::optional<int32> cancel = row.Allowed(BlockId::Duel, DuelBlock::ACTION_CANCEL_FORM))
                    return cancel;
        }

        if (!CanHeal(row))
            return std::nullopt;

        for (uint32 slot = FRIEND_OWNER; slot < FRIEND_SLOTS; ++slot)
        {
            bool const tank = friendObs(slot, SupportBlock::FRIEND_APTITUDE_FIRST + Aptitude::BRIEF_MITIGATION)
                >= AptitudeDemand::HoldsThePull().AtLeast;
            if (friendObs(slot, SupportBlock::FRIEND_ALIVE) == 0.0f
                || friendObs(slot, SupportBlock::FRIEND_ATTACKERS) == 0.0f || (slot != FRIEND_OWNER && !tank)
                || friendObs(slot, SupportBlock::FRIEND_OWN_HEAL_OVER_TIME) > 0.0f
                || friendObs(slot, SupportBlock::FRIEND_OWN_ABSORB) > 0.0f)
                continue;

            if (std::optional<int32> select = aim(slot))
                return select;
            return FirstSpell(row, layout, [](ActionCatalog::Action const& action)
            {
                return action.Healing && action.KeepsAura;
            });
        }

        return std::nullopt;
    }

    /// The bearing that takes the seat towards its objective over ground it can actually cross, or nothing to
    /// carry on with the one it is already walking.
    ///
    /// Holding BEARING_FORWARD was what this did before, and it is why the scripted baseline arrived in 8% of its
    /// episodes against a trained policy's 99%: forward is the right way only until something is in front of it,
    /// and a baseline that cannot steer is a yardstick anything clears. Bearing b points at
    /// -b*45 degrees in the seat's own frame (MoveBlock::HeadingOf), so its alignment with an objective lying at
    /// `heading` is cos(heading + b*45). Weighed against OBS_GROUND_FIRST, that is a seat that walks round a
    /// cliff instead of into it -- and, now that the probe reports water as ground it can cross, one that swims a
    /// crossing rather than stopping at the shore.
    std::optional<int32> Steer(Row const& row, float headingSin, float headingCos)
    {
        if (!row.Has(BlockId::Move))
            return std::nullopt;

        float const heading = std::atan2(headingSin, headingCos);

        uint32 best = MoveBlock::BEARING_COUNT;
        float bestScore = 0.0f;
        for (uint32 bearing = 0; bearing < MoveBlock::BEARING_COUNT; ++bearing)
        {
            float const aim = std::cos(heading + float(bearing) * float(M_PI) / 4.0f);
            // Ray 2 * b lies along bearing b: the block senses twice as many rays as it can walk bearings.
            float const reach = row.Obs(BlockId::Move, MoveBlock::OBS_GROUND_FIRST + 2 * bearing);
            float const score = aim + GROUND_OVER_AIM * reach;
            if (best == MoveBlock::BEARING_COUNT || score > bestScore)
            {
                bestScore = score;
                best = bearing;
            }
        }

        if (best == MoveBlock::BEARING_COUNT)
            return std::nullopt;

        // Already walking the best one: it is masked for that reason, and pressing the second best instead would
        // set the seat zig-zagging between two bearings for as long as the objective sat between them.
        if (row.Obs(BlockId::Move, MoveBlock::OBS_BEARING_HELD + best) > 0.0f)
            return std::nullopt;

        return row.Allowed(BlockId::Move, MoveBlock::ACTION_BEARING_FIRST + best);
    }

    /// Stop the feet, if they are walking: the bearing one-hot is the sign that they are.
    std::optional<int32> Halt(Row const& row)
    {
        if (!row.Has(BlockId::Move) || row.Obs(BlockId::Move, MoveBlock::OBS_BEARING_NONE) > 0.0f)
            return std::nullopt;

        return row.Allowed(BlockId::Move, MoveBlock::ACTION_HALT);
    }

    /// Turn towards `heading` (sin, cos in the seat's own frame), as a held key, when it lies more than half a
    /// bearing off straight ahead: the mouse-look, where FACE_OBJECTIVE used to snap the head in one press. Left
    /// is counter-clockwise, the positive way round in WoW's orientation. The key is masked while it is held,
    /// so this falls through to the feet on the decisions in between.
    std::optional<int32> TurnToward(Row const& row, float headingSin, float headingCos)
    {
        if (!row.Has(BlockId::Move))
            return std::nullopt;

        float const heading = std::atan2(headingSin, headingCos);
        if (std::fabs(heading) <= TURN_WITHIN_RADIANS)
            return std::nullopt;

        return row.Allowed(BlockId::Move,
            heading > 0.0f ? MoveBlock::ACTION_TURN_LEFT : MoveBlock::ACTION_TURN_RIGHT);
    }

    std::optional<int32> Fight(Row const& row, Layout const& layout)
    {
        // A living target's health; not the distance, which is 0 in melee range (it is measured between reaches).
        bool const hasTarget = row.Obs(BlockId::Core, CoreBlock::OBS_TARGET_HEALTH) > 0.0f;

        // Travel: a flying mount for a long trip where it flies, else a ground mount; then steer. The point order
        // this used to issue is gone -- the scripted policy crosses ground the same way a learned one has to, by
        // looking where it is going and holding forward, so that "beat the baseline" still means something on a
        // stage about movement.
        // Not while something is on the seat: a quest's creature met on the way is fought, then the trip goes on.
        if (!hasTarget && row.Has(BlockId::Travel) && row.Obs(BlockId::Travel, TravelBlock::OBS_OBJECTIVE) > 0.0f)
        {
            float const yards = row.Obs(BlockId::Travel, TravelBlock::OBS_OBJECTIVE_DISTANCE) * 500.0f;
            float const height = row.Obs(BlockId::Travel, TravelBlock::OBS_HEIGHT) * 50.0f;
            bool const mounted = row.Obs(BlockId::Travel, TravelBlock::OBS_MOUNTED) > 0.0f;
            bool const flying = row.Obs(BlockId::Travel, TravelBlock::OBS_FLYING_MOUNT) > 0.0f;

            if (row.Obs(BlockId::Travel, TravelBlock::OBS_AT_OBJECTIVE) > 0.0f)
                return row.Allowed(BlockId::Travel, TravelBlock::ACTION_DISMOUNT);

            if (!mounted && yards > MOUNT_BEYOND_YARDS)
            {
                if (std::optional<int32> fly = row.Allowed(BlockId::Travel, TravelBlock::ACTION_MOUNT_FLYING))
                    return fly;
                if (std::optional<int32> ride = row.Allowed(BlockId::Travel, TravelBlock::ACTION_MOUNT_GROUND))
                    return ride;
            }

            // A mount is a cast, and moving cancels it. Wait the cast out.
            //
            // Without this the scripted policy cancelled its own mount every single time, and the numbers said
            // so: it mounted in 2 of 2048 evaluation episodes. The sequence is that the press stops the seat and
            // starts the cast; on the next decision the seat is still not mounted, so this branch asks for the
            // mount again -- and CanSummon now refuses it, because a cast is in progress. The request comes back
            // empty, the branch falls through to facing and steering, Encoding::MoveTo runs, and the cast dies
            // one decision after it began. Every time, for both the scripted policy and any learned one that
            // presses mount and then steers.
            //
            // OBS_CASTING is the seat's own "casting or channeling", so the wait needs no state kept between
            // decisions. Scoped to the case that wants it -- on a trip, meaning to be mounted, mid-cast -- so
            // that nothing else in the policy starts waiting on casts it should be moving through.
            if (!mounted && yards > MOUNT_BEYOND_YARDS
                && row.Obs(BlockId::Core, CoreBlock::OBS_CASTING) > 0.0f)
                return 0;

            // Turn towards it, as a held key, while the feet keep walking: the objective's heading in the seat's
            // own frame comes from the move block's pair. FACE_OBJECTIVE used to do this in one press, and the
            // engine snapping the heading every decision was the compass the trained policy collapsed onto.
            if (std::optional<int32> turn = TurnToward(row,
                row.Obs(BlockId::Move, MoveBlock::OBS_OBJECTIVE_BEARING_SIN),
                row.Obs(BlockId::Move, MoveBlock::OBS_OBJECTIVE_BEARING_COS)))
                return turn;

            // In the air, climb to cruising height for the crossing and nose down for the arrival. Pitch is held,
            // so these mask themselves once the angle is reached, the same way the facing does.
            if (flying)
            {
                if (yards > MOUNT_BEYOND_YARDS * 0.5f && height < CRUISE_HEIGHT_YARDS)
                {
                    if (std::optional<int32> up = row.Allowed(BlockId::Move, MoveBlock::ACTION_PITCH_UP))
                        return up;
                }
                else if (yards < MOUNT_BEYOND_YARDS * 0.5f && height > 1.0f)
                {
                    if (std::optional<int32> down = row.Allowed(BlockId::Move, MoveBlock::ACTION_PITCH_DOWN))
                        return down;
                }
                else if (std::optional<int32> level = row.Allowed(BlockId::Move, MoveBlock::ACTION_PITCH_LEVEL))
                    return level;
            }

            // And steer. The objective's direction in the seat's own frame comes from the move block's own pair,
            // because this is the block that owns getting there.
            if (std::optional<int32> go = Steer(row, row.Obs(BlockId::Move, MoveBlock::OBS_OBJECTIVE_BEARING_SIN),
                row.Obs(BlockId::Move, MoveBlock::OBS_OBJECTIVE_BEARING_COS)))
                return go;

            // On the way: wait (the no-op), rather than cast something that would take the mount away.
            return 0;
        }

        if (row.Has(BlockId::Gauntlet) && !hasTarget)
        {
            if (row.Obs(BlockId::Core, CoreBlock::OBS_HEALTH) < EAT_BELOW)
                if (std::optional<int32> eat = row.Allowed(BlockId::Gauntlet, GauntletBlock::ACTION_EAT))
                    return eat;

            float const mana = row.Obs(BlockId::Core, CoreBlock::OBS_MANA);
            if (mana > 0.0f && mana < DRINK_BELOW)
                if (std::optional<int32> drink = row.Allowed(BlockId::Gauntlet, GauntletBlock::ACTION_DRINK))
                    return drink;
        }

        if (std::optional<int32> support = Support(row, layout))
            return support;

        // A pet class fights with its pet, as a player does: out before the fight, and sent at the target.
        if (std::optional<int32> pet = PetAction(row, layout))
            return pet;

        float const yards = row.Obs(BlockId::Duel, DuelBlock::OBS_DISTANCE) * 60.0f;
        bool const inMelee = yards <= CLOSE_IN_BEYOND_YARDS;
        // The target's heading in the seat's own frame, from the block that owns getting there. The duel block
        // used to run to it, to range and back off for the seat; those are bearings now, and a bearing is held,
        // so the feet also have to be told when to stop.
        float const toTargetSin = row.Obs(BlockId::Move, MoveBlock::OBS_TARGET_BEARING_SIN);
        float const toTargetCos = row.Obs(BlockId::Move, MoveBlock::OBS_TARGET_BEARING_COS);

        if (FightsFromRange(row, layout))
        {
            if (hasTarget)
            {
                // To casting range from afar, and closer only when something is in the way; once there, stop, or
                // a caster that keeps walking walks into melee reach.
                bool const closing = yards > HOLD_RANGE_BEYOND_YARDS
                    || row.Obs(BlockId::Duel, DuelBlock::OBS_TARGET_IN_LINE_OF_SIGHT) == 0.0f;
                if (closing)
                    if (std::optional<int32> go = Steer(row, toTargetSin, toTargetCos))
                        return go;

                // A hunter cannot shoot inside melee reach: with its pet on the target, it steps back out and lets
                // the pet hold it. A caster casts where it stands.
                bool const backing = layout.Profile->Class == CLASS_HUNTER && inMelee
                    && row.Obs(BlockId::Duel, DuelBlock::OBS_TARGET_ATTACKS_BOT) > 0.0f
                    && row.Obs(BlockId::Duel, DuelBlock::OBS_PET_ATTACKING) > 0.0f;
                if (backing)
                    if (std::optional<int32> back = Steer(row, -toTargetSin, -toTargetCos))
                        return back;

                if (!closing && !backing && yards <= HOLD_RANGE_WITHIN_YARDS)
                    if (std::optional<int32> halt = Halt(row))
                        return halt;
            }

            // Melee only as the fallback once the target is on it: a hunter with no pet yet, or one still held.
            if (inMelee)
                if (std::optional<int32> attack = row.Allowed(BlockId::Duel, DuelBlock::ACTION_START_ATTACK))
                    return attack;

            return std::nullopt;
        }

        if (std::optional<int32> attack = row.Allowed(BlockId::Duel, DuelBlock::ACTION_START_ATTACK))
            return attack;

        // Close in on a held bearing, and stop once in reach.
        if (hasTarget && !inMelee)
            if (std::optional<int32> go = Steer(row, toTargetSin, toTargetCos))
                return go;
        if (hasTarget && inMelee)
            if (std::optional<int32> halt = Halt(row))
                return halt;

        return std::nullopt;
    }
}

namespace
{
    /// Life: the world presses that apply where the seat stands -- loot what is open, use the thing in reach, sell,
    /// repair, buy, dress -- before anything else.
    std::optional<int32> LifeHere(Row const& row)
    {
        if (!row.Has(BlockId::World))
            return std::nullopt;

        auto const obs = [&row](uint32 feature) { return row.Obs(BlockId::World, feature); };
        if (obs(WorldBlock::OBS_LOOT_OPEN) > 0.0f)
            if (std::optional<int32> loot = row.Allowed(BlockId::World, WorldBlock::ACTION_LOOT_ALL))
                return loot;
        if (obs(WorldBlock::OBS_CASTING) > 0.0f)
            return 0;   // a gathering cast finishes on its own
        if (obs(WorldBlock::OBS_CORPSE_IN_REACH) > 0.0f || obs(WorldBlock::OBS_GIVER_IN_REACH) > 0.0f
            || (obs(WorldBlock::OBS_NODE_IN_REACH) > 0.0f && obs(WorldBlock::OBS_NODE_OPENABLE) > 0.0f))
            if (std::optional<int32> use = row.Allowed(BlockId::World, WorldBlock::ACTION_INTERACT))
                return use;
        if (obs(WorldBlock::OBS_VENDOR_IN_REACH) > 0.0f)
            for (uint32 action : { WorldBlock::ACTION_SELL_JUNK, WorldBlock::ACTION_REPAIR,
                WorldBlock::ACTION_BUY_SUPPLIES })
                if (std::optional<int32> trade = row.Allowed(BlockId::World, action))
                    return trade;
        if (std::optional<int32> dress = row.Allowed(BlockId::World, WorldBlock::ACTION_EQUIP_UPGRADE))
            return dress;
        return std::nullopt;
    }

    /// Life: walk to the nearest thing worth walking to, by priority -- a corpse, a giver with business, the
    /// objective the travel block shows, a node the seat can open, a vendor -- and stop on reaching it.
    std::optional<int32> LifeGo(Row const& row)
    {
        if (!row.Has(BlockId::World))
            return std::nullopt;

        auto const obs = [&row](uint32 feature) { return row.Obs(BlockId::World, feature); };
        struct Aim { uint32 Present; uint32 InReach; uint32 Sin; uint32 Cos; bool Wanted; };
        Aim const aims[] =
        {
            { WorldBlock::OBS_CORPSE, WorldBlock::OBS_CORPSE_IN_REACH, WorldBlock::OBS_CORPSE_SIN,
                WorldBlock::OBS_CORPSE_COS, obs(WorldBlock::OBS_CORPSE_DISTANCE) < 0.4f },
            { WorldBlock::OBS_GIVER, WorldBlock::OBS_GIVER_IN_REACH, WorldBlock::OBS_GIVER_SIN,
                WorldBlock::OBS_GIVER_COS, obs(WorldBlock::OBS_GIVER_OFFERS) > 0.0f
                    || obs(WorldBlock::OBS_GIVER_TURN_IN) > 0.0f },
            { WorldBlock::OBS_NODE, WorldBlock::OBS_NODE_IN_REACH, WorldBlock::OBS_NODE_SIN,
                WorldBlock::OBS_NODE_COS, obs(WorldBlock::OBS_NODE_OPENABLE) > 0.0f },
            { WorldBlock::OBS_VENDOR, WorldBlock::OBS_VENDOR_IN_REACH, WorldBlock::OBS_VENDOR_SIN,
                WorldBlock::OBS_VENDOR_COS, obs(WorldBlock::OBS_HAS_JUNK) > 0.0f
                    || obs(WorldBlock::OBS_DURABILITY) < 1.0f || obs(WorldBlock::OBS_FOOD) < 1.0f },
        };
        for (Aim const& aim : aims)
        {
            if (obs(aim.Present) <= 0.0f || !aim.Wanted)
                continue;
            if (obs(aim.InReach) > 0.0f)
                return Halt(row);
            if (std::optional<int32> turn = TurnToward(row, obs(aim.Sin), obs(aim.Cos)))
                return turn;
            return Steer(row, obs(aim.Sin), obs(aim.Cos));
        }

        // Nothing sensed: the travel block's objective is where the episode wants the seat (the quest's next place).
        if (row.Has(BlockId::Travel) && row.Obs(BlockId::Travel, TravelBlock::OBS_OBJECTIVE) > 0.0f)
        {
            if (row.Obs(BlockId::Travel, TravelBlock::OBS_AT_OBJECTIVE) > 0.0f)
                return Halt(row);
            float const sin = row.Obs(BlockId::Travel, TravelBlock::OBS_OBJECTIVE_BEARING_SIN);
            float const cos = row.Obs(BlockId::Travel, TravelBlock::OBS_OBJECTIVE_BEARING_COS);
            if (std::optional<int32> turn = TurnToward(row, sin, cos))
                return turn;
            return Steer(row, sin, cos);
        }

        return std::nullopt;
    }
}

bool Animus::Curriculum::Baselines::Supports(std::string const& policy, Layout const& layout)
{
    return policy == "greedy" || (policy == "fight" && layout.Has(BlockId::Duel))
        || (policy == "life" && layout.Has(BlockId::World) && layout.Has(BlockId::Duel));
}

int32 Animus::Curriculum::Baselines::Choose(std::string const& policy, Layout const& layout, float const* obs,
    uint8 const* mask)
{
    Row const row(layout, obs, mask);

    if (policy == "life" && layout.Has(BlockId::World) && layout.Has(BlockId::Duel))
    {
        // What is here first, then the fight the combat baseline plays, then the walk to the next thing.
        if (std::optional<int32> here = LifeHere(row))
            return *here;
        if (std::optional<int32> action = Fight(row, layout))
            return *action;
        // The rotation only with something to hit: without a target its self-cast spells are all that is
        // allowed, and a warlock's Life Tap, cast on repeat, kills it.
        if (row.Obs(BlockId::Core, CoreBlock::OBS_TARGET_HEALTH) > 0.0f)
        {
            if (std::optional<int32> ability = PetDamage(row, layout))
                return *ability;
            if (std::optional<int32> spell = Rotation(row, layout))
                return *spell;
        }
        if (std::optional<int32> go = LifeGo(row))
            return *go;
        return 0;
    }

    if (policy == "fight" && layout.Has(BlockId::Duel))
    {
        if (std::optional<int32> action = Fight(row, layout))
            return *action;
        if (std::optional<int32> ability = PetDamage(row, layout))
            return *ability;
        if (std::optional<int32> spell = Rotation(row, layout))
            return *spell;
        return 0;
    }

    // greedy: the first usable spell or trinket in catalog order.
    uint32 const catalog = uint32(layout.Catalog().Actions().size());
    for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < catalog; ++action)
        if (std::optional<int32> allowed = row.Allowed(BlockId::Core, action))
            return *allowed;

    return 0;
}
