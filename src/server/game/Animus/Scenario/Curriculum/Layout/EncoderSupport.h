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

#ifndef ANIMUS_LIB_CURRICULUM_ENCODER_SUPPORT_H
#define ANIMUS_LIB_CURRICULUM_ENCODER_SUPPORT_H

#include "ActionCatalog.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include "SeatView.h"
#include "Spell.h"
#include "Unit.h"
#include <array>

class Item;
class Player;
class SpellInfo;

/*
 * What the blocks share: reading cooldowns and auras, the core's cast checks run without casting, enemy slots, and
 * moving the bot the way a client would.
 */
namespace Animus::Curriculum::Encoding
{
    constexpr uint32 IMMOBILE_STATES = UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING;
    constexpr uint32 STUN_STATES = UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING;
    constexpr uint32 CROWD_CONTROL_STATES = STUN_STATES | UNIT_STATE_ROOT;

    /// A position relative to `origin` (the spawn point) for the critic state: / 40 yd, clamped to [-2, 2].
    [[nodiscard]] float RelativePosition(float coordinate, float origin);

    /// A share of an aura's duration or charges: an aura the bot keeps up on a friend with more left than this is not
    /// cast again (IsSpellActionAllowed).
    constexpr float REFRESH_BELOW_FRACTION = 0.25f;

    /// The targets a client would send for `info` aimed at `target` (null: self-cast spells only). A positive spell
    /// that takes a unit goes to `friendUnit` when given, else the bot itself.
    [[nodiscard]] SpellCastTargets TargetsFor(SpellInfo const* info, Player* bot, Unit* target,
        Unit* friendUnit = nullptr);

    /// The unit in friend slot `slot` (FRIEND_SELF, FRIEND_OWNER, teammates) if it is in the bot's map, else null.
    [[nodiscard]] Unit* FriendUnit(SeatView const& view, uint32 slot);

    /// Where the seat's positive single-target spells go: the support block's selected friend while it is alive
    /// (null otherwise), or the bot itself for a layout without the block.
    [[nodiscard]] Unit* SupportTarget(SeatView const& view);

    /// A positive spell that takes a unit target (a heal, shield, blessing or Hand), cast on a friend.
    [[nodiscard]] bool AimsAtFriend(SpellInfo const* info);

    /// The aura of any rank of `info`'s chain `caster` has on `unit`, or null.
    [[nodiscard]] Aura const* OwnAuraOfChain(Unit const* unit, SpellInfo const* info, ObjectGuid caster);

    /// Whether `caster`'s aura of `info`'s chain on `unit` has more than REFRESH_BELOW_FRACTION of its duration or
    /// charges left (or is permanent).
    [[nodiscard]] bool OwnAuraHasPlentyLeft(Unit const* unit, SpellInfo const* info, ObjectGuid caster);

    /// A cast in its cast time (channels excluded): the client refuses to start another spell or use an item
    /// meanwhile. The core only checks this for client casts, so actions check it here.
    [[nodiscard]] bool CastInProgress(Player const* bot);

    /// Whether the cast in flight is a mount.
    ///
    /// Narrower than CastInProgress on purpose. A mount is the one cast in the curriculum that the seat must
    /// stand still through and that any movement destroys, so it is the one worth protecting -- and protecting
    /// casts in general would stop a seat walking out of fire mid-spell, which is a thing it must always be
    /// able to do.
    [[nodiscard]] bool MountCastInProgress(Player const* bot);

    /// The core's own cast validation (Spell::CheckCast), without casting. `target` may be null (self-cast spells).
    [[nodiscard]] bool CanCast(Player* bot, SpellInfo const* info, Unit* target, Item* castItem = nullptr,
        Unit* friendUnit = nullptr);

    /// Whether a spell (a revive) can be cast on `ally` now.
    [[nodiscard]] bool CanHeal(Player* bot, ActionCatalog::Action const& heal, Unit* ally);

    /// Whether a spell action of the catalog could be cast at `target` now.
    /// The rank of `def` the seat casts: the highest the bot knows (from the view's per-episode table when it has one,
    /// else resolved by walking the chain), or for a heal with ranks under the support block, the highest active rank
    /// at its rank tier.
    [[nodiscard]] SpellInfo const* KnownRank(SeatView const& view, ActionCatalog::Action const& def);

    [[nodiscard]] bool IsSpellActionAllowed(SeatView const& view, Unit* target, ActionCatalog::Action const& def);

    /// Cast a spell action at `target` as CMSG_CAST_SPELL would. Returns true if it started.
    bool ApplySpellAction(SeatView const& view, Unit* target, ActionCatalog::Action const& def,
        SeatActionResult& result);

    /// The on-use spell of an item (a trinket), or null.
    [[nodiscard]] SpellInfo const* TrinketSpell(Item const* item);

    /// The on-use spell of an item entry (food, drink, potions, stones), or null.
    [[nodiscard]] SpellInfo const* UseSpell(uint32 itemEntry);

    /// Whether the bot can use its `entry` item (a potion, healthstone, bandage or soulstone) on `target` now, and
    /// using it as the client does (true if one was used up).
    [[nodiscard]] bool CanUseItemOn(Player* bot, uint32 entry, Unit* target);
    bool UseItemOn(Player* bot, uint32 entry, Unit* target);

    /// The remaining cooldown of an item's on-use spell as a fraction; 0 without the item.
    [[nodiscard]] float ItemCooldownFraction(Player const* bot, uint32 entry);

    /// A revive (a resurrection spell on a dead ally, or a warlock's soulstone on a living one) usable on `ally` now,
    /// and using it.
    [[nodiscard]] bool CanRevive(SeatView const& view, ActionCatalog::Action const& revive, Player* ally);
    void Revive(SeatView const& view, ActionCatalog::Action const& revive, Player* ally, SeatActionResult& result);

    /// Revive features, two per revive: known (a spell the bot knows, or a soulstone in its bags) and cooldown.
    void WriteRevives(SeatView const& view, float* out);

    /// The bot's pet, or its first living controlled unit other than a totem.
    [[nodiscard]] Unit* FirstPet(Player* bot);

    /// A shapeshift the player could cancel from the client (druid forms, Shadowform, Ghost Wolf, Stealth).
    [[nodiscard]] SpellInfo const* CancellableForm(Player const* bot);

    [[nodiscard]] bool IsCrowdControlled(Unit const* unit);

    /// The hostile ground effects the unit is standing in (persistent area auras: a fire pool, a poison cloud, a
    /// consecration), counted, with the one it is deepest inside. Read from the unit's own aura list -- a ground
    /// effect applies an aura to whoever stands in it, and the aura knows the object that owns it -- so this costs
    /// no searching at all. Nothing in the module has ever observed area effects, which is why no seat has ever had
    /// a reason to step out of one.
    [[nodiscard]] uint32 StandingInHazards(Unit const* unit, Hazard* deepest);

    /// The nearest hostile ground effect the unit is NOT yet standing in, within `range` yards: a grid search over
    /// dynamic objects (persistent area auras) and armed traps.
    ///
    /// This is what makes avoidance learnable at all. StandingInHazards reads the unit's own auras, so it can only
    /// say "you are in fire" -- which teaches a seat to leave, never to stay out, because nothing in the observation
    /// distinguishes clear ground from ground it is about to walk into. Hazards do not move, so the caller caches
    /// what this finds and recomputes the distance itself (StageScenario::TrackHazards); the search runs about once
    /// a second, not every decision.
    [[nodiscard]] bool FindNearestHazard(Unit const* unit, float range, Hazard& out);

    /// The harmful auras on a unit, summarised: what is on it, what could be taken off, and how long the worst of
    /// it lasts. A seat could always see its own buffs (per catalog action) and never what had been done to it, so
    /// a debuff to dispel, a stack to run from and a snare to break all looked the same as nothing.
    struct Debuffs
    {
        uint32 Count = 0;               // harmful auras
        uint32 Dispellable = 0;         // ... of a kind a dispel can remove (magic, curse, disease, poison)
        uint32 Stacks = 0;              // the most stacks any one of them has
        uint32 LongestMs = 0;           // and the longest left to run
        std::array<bool, 6> Mechanics{};// which of OBSERVED_MECHANICS are among them
    };

    [[nodiscard]] Debuffs IncomingDebuffs(Unit const* unit);

    /// Where `unit` stands on `enemy`'s threat list: its own threat over the threat of whoever the enemy is on, so 1
    /// means it holds aggro (or is tied for it) and 0 means the enemy has never noticed it. Holding a pack off a
    /// healer is threat management, and until now nothing in any block could see a threat table at all -- a tank
    /// kept aggro only by accident, and every "threat" the encounters count is a proxy for it (who an enemy happens
    /// to be swinging at this decision).
    [[nodiscard]] float ThreatShare(Unit const* enemy, Unit const* unit);

    /// What kind of opponent a unit is, for the duel block and the critic state.
    ///
    /// The fair opponent creature types, one-hot in this order; a player counts as humanoid.
    constexpr std::array<uint32, 7> OPPONENT_TYPES = { CREATURE_TYPE_BEAST, CREATURE_TYPE_DRAGONKIN,
        CREATURE_TYPE_DEMON, CREATURE_TYPE_ELEMENTAL, CREATURE_TYPE_GIANT, CREATURE_TYPE_UNDEAD,
        CREATURE_TYPE_HUMANOID };
    /// The magic schools whose immunity is observed, in this order.
    constexpr std::array<SpellSchools, 6> OBSERVED_SCHOOLS = { SPELL_SCHOOL_HOLY, SPELL_SCHOOL_FIRE,
        SPELL_SCHOOL_NATURE, SPELL_SCHOOL_FROST, SPELL_SCHOOL_SHADOW, SPELL_SCHOOL_ARCANE };
    /// The crowd control mechanics whose immunity is observed, in this order (fear covers horror too).
    constexpr std::array<Mechanics, 6> OBSERVED_MECHANICS = { MECHANIC_FEAR, MECHANIC_STUN, MECHANIC_ROOT,
        MECHANIC_SNARE, MECHANIC_SILENCE, MECHANIC_POLYMORPH };

    /// Write the one-hot of OPPONENT_TYPES for `unit` to out[0..6] (nothing for another type).
    void WriteOpponentType(Unit const* unit, float* out);

    /// The creature's damage multiplier from its template (1 for a player).
    [[nodiscard]] float DamageModifier(Unit const* unit);

    /// The share of a physical hit from an attacker of `attackerLevel` that `unit`'s armor takes off (0-0.75), as the
    /// core computes it.
    [[nodiscard]] float ArmorReduction(Unit const* unit, uint8 attackerLevel);

    /// Whether `unit` is immune to `school` damage, and to `mechanic`.
    [[nodiscard]] bool IsImmuneToSchool(Unit const* unit, SpellSchools school);
    [[nodiscard]] bool IsImmuneToMechanic(Unit const* unit, Mechanics mechanic);

    /// The enemy slot of `unit`, or -1.
    [[nodiscard]] int32 SlotOf(SeatView const& view, Unit const* unit);

    /// An enemy slot, other than `except`, whose living enemy attacks `victim`; -1 if none.
    [[nodiscard]] int32 SlotAttacking(SeatView const& view, Unit const* victim, uint32 except);

    /// Select enemy `slot` and keep swinging, at the new target.
    void SelectEnemy(SeatView& view, uint32 slot);

    /// Run to a point, replacing whatever movement the bot had.
    /// Put `at` on the ground beneath it, or say there is no ground there to stand on.
    ///
    /// `fromZ` is the height the probe drops from and `maxStep` how far the result may differ from it, which
    /// is what keeps a point from landing on a roof above or in a pit below. Shared by anything that picks a
    /// spot to walk to out of thin air -- a step along a held bearing (MoveBlock), a place a director names --
    /// because a point that is not snapped sends a bot into a wall or off a ledge.
    [[nodiscard]] bool SnapToGround(Map const* map, uint32 phaseMask, Position& at, float fromZ,
        float maxStep = 6.0f);

    /// Whether `watcher` actually has eyes on `target`: it can detect it *and* nothing is in the way.
    ///
    /// `CanSeeOrDetect` alone is grid visibility plus stealth and invisibility -- it does not raycast, so it
    /// stays true through a wall. Anything whose meaning is "is being watched right now" has to add the line
    /// of sight, or breaking it changes nothing: a hunter keeps tracking its quarry around a pillar, and an
    /// evade drill reads zero for every class that has no stealth. The cheap test runs first, so the raycast
    /// only happens for something already detectable.
    [[nodiscard]] bool CanSee(WorldObject const* watcher, WorldObject const* target);

    /// Walk to a point, by path. `facing` turns the head as it goes: it has to be part of the same spline,
    /// because a spline sets the unit's orientation as it runs and a facing applied by any other means is
    /// overwritten the moment the unit moves. Null leaves the head to the spline, which points it along the path.
    void MoveTo(Player* bot, uint32 pointId, float x, float y, float z, float const* facing = nullptr);

    /// Move along a straight spline through the air or the water, with no path and no ground under it. Steering in
    /// three dimensions needs this: a ground move is snapped to the ground by definition, which is exactly what a
    /// dive or a climb is trying not to be. `facing` as above.
    /// Straight to a point with no pathfinding and no fly flag: how a seat enters and crosses water,
    /// where the walkable mesh stops at the surface.
    void SwimTo(Player* bot, float x, float y, float z, float const* facing = nullptr);

    /// Jump along a heading: a parabola at `speedXY` forward and `speedZ` up, with no pathfinding.

    /// The caller must already know there is somewhere to land.

    void JumpTo(Player* bot, float x, float y, float z, float speedXY, float speedZ,

        float const* facing = nullptr);

    void FlyTo(Player* bot, float x, float y, float z, float const* facing = nullptr);

    /// A seat in the air without flight (the end of a drop jump, a dismount, a cast that took the mount away)
    /// falls to the ground the way a player does: MotionMaster::MoveFall, then Player::HandleFall for the
    /// damage, which is nothing under a feather-fall or hover aura and lethal past about seventy yards. True
    /// when a fall was started; `yards` and `healthFraction` (of maximum health) say what it cost. Nothing
    /// happens while a spline is still running, so a jump's own arc is never cut short, and the falling flag
    /// the core leaves on a player is taken off again -- EffectMovementGenerator::Finalize clears it for
    /// creatures only, and with it stuck the jump stayed masked for the rest of the episode.
    bool FallToGround(Player* bot, float* yards = nullptr, float* healthFraction = nullptr);

    /// Send the bot's pets and guardians at `target`, as the pet bar's Attack does. True if any was ordered.
    bool PetAttack(Player* bot, Unit* target);

    /// Put the Call Pet global cooldown on the bot, as calling a beast does.
    void StartCallBeastCooldown(Player* bot);
}

#endif
