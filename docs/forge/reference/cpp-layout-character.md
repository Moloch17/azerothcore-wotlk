# Layout assembly and the character (C++)

Purpose and scope. How a layout is built from blocks per class (`Layout/`), how a seat is encoded and acted on per
decision, what a seat remembers between decisions, the manifest and the `stage.json` layout entries, and how every
class and race is turned into a character (`Character/`): action catalog, class kit, aptitude, talents, gear,
supplies, pets, and the client-packet layer for presses. Blocks themselves are in [cpp-blocks.md](cpp-blocks.md);
rewards and routing in [cpp-rewards-routing.md](cpp-rewards-routing.md). See also
[cpp-stagescenario.md](cpp-stagescenario.md), [cpp-encounters.md](cpp-encounters.md),
[cpp-tuning-keys.md](cpp-tuning-keys.md), [file-formats.md](file-formats.md), [protocol.md](protocol.md),
[tests.md](tests.md), [known-issues.md](known-issues.md), [glossary.md](glossary.md),
[00-architecture.md](00-architecture.md).

Paths are relative to `src/server/game/Animus/Scenario/Curriculum/` unless they start with `src/`. Line numbers are
from commit `bd32b9dc8`.

## Map table

| Path | Lines | Role |
| --- | --- | --- |
| Layout/Block.h | 305 | `BlockId`, `Block` interface, sizing constants, `SeatGoal`, goal-target space. |
| Layout/Layout.h | 80 | `Layout` struct: block list, slices, mode groups. |
| Layout/Layout.cpp | 243 | `Layout::Build`, `ActionNames`, `Manifest` (format 9), `BlockName`, `GoalName`, `GoalAccepts`. |
| Layout/SeatView.h | 499 | `SeatView` (a seat's situation), `SeatOption`s, `MovementTrail`, `SeatActionResult`. |
| Layout/SeatEncoder.h | 109 | `Observe`/`Apply` entry points and per-thread observe timing. |
| Layout/SeatEncoder.cpp | 136 | Block loop, dead/no-target early outs, option clearing, press ordering. |
| Layout/SeatMemory.h | 101 | Per-seat memory: pacing, press times, health trends. |
| Layout/SeatMemory.cpp | 193 | Pacing and trend implementation. |
| Layout/EncoderSupport.h | 236 | `Encoding::` helpers shared by blocks. |
| Layout/EncoderSupport.cpp | 827 | Cast checks, spell press, hazards, debuffs, threat, immunities. |
| Character/ActionCatalog.h | 161 | Per-class action space. |
| Character/ActionCatalog.cpp | 786 | Spell classification and catalog construction. |
| Character/Aptitude.h | 172 | 26-feature measured capability vector, `AptitudeDemand`. |
| Character/Aptitude.cpp | 342 | `Aptitude::Of`. |
| Character/ClassAssets.h | 65 | Per-class cached kit/talents/catalog/gear. |
| Character/ClassAssets.cpp | 87 | `ClassAssets::For`, `SpecsMeeting`. |
| Character/ClassKit.h | 79 | Trainer and class-quest spells, reagents, armour type. |
| Character/ClassKit.cpp | 274 | World-DB queries, `Learn`, `StoreReagents`. |
| Character/ClassProfile.h | 124 | Races, classes, specs, stat profiles, weapon layouts. |
| Character/ClassProfile.cpp | 114 | The class table, `DamageScale`. |
| Character/EntityActions.h | 183 | Client-packet presses on entities and casts. |
| Character/EntityActions.cpp | 453 | Packet builders, refusal judging, `Apply`. |
| Character/GearBuilder.h | 189 | Random level-appropriate gear. |
| Character/GearBuilder.cpp | 898 | Pools, item-level bands, equip; also defines the `GearStats` functions. |
| Character/GearEnhancements.cpp | 426 | Enchants, gems, runes, poisons, imbues, quiver. |
| Character/GearStats.h | 77 | `GearStats` declarations (defined in GearBuilder.cpp). |
| Character/IncomingSpell.h | 94 | Enemy-cast features, 26 columns. |
| Character/IncomingSpell.cpp | 228 | Implementation. |
| Character/PetTalents.h | 42 | Hunter pet talents. |
| Character/PetTalents.cpp | 197 | Builds and `Spend`. |
| Character/SeatCharacter.h | 76 | `Configure`, `PrepareFighter`, `GivePet`. |
| Character/SeatCharacter.cpp | 146 | Implementation. |
| Character/SpecBuilds.h | 49 | Standard talent/glyph builds (data). |
| Character/SpecBuilds.cpp | 460 | 31 spec builds. |
| Character/Supplies.h | 136 | Consumables, stable pool, stocking. |
| Character/Supplies.cpp | 408 | Pools from the world DB; hunter beasts. |
| Character/TalentBuilder.h | 130 | Talent trees and builds. |
| Character/TalentBuilder.cpp | 328 | Standard/noisy/random builds, glyphs. |
| Character/WorldCreatures.h | 36 | Spawned and waypoint-walker creature ids. |
| Character/WorldCreatures.cpp | 52 | Two world-DB queries. |

## Layout

`struct Layout` (`Layout/Layout.h:344`): `Index`, `Stage`, `Profile` (class), `Assets`, `ObsDim`, `NumActions`,
`Blocks`,
`Slices[BLOCK_COUNT]` (`ObsFirst, ObsCount, ActionFirst, ActionCount` per block id), `ModeGroups` (per action) and a
private `_blockMask`. A layout is one class at one stage ("one trained model per class", `ClassProfile` comment): there
is
no role.

`Layout::Build(profile, stage)` (`Layout.cpp:127`): copy `stage.Blocks`; for each block in order ask `Size(layout)`,
place
the slice after the previous one, set the mask bit (so a block sized later sees `Has` of earlier blocks but not later
ones, the reason `SightBlock::Width` reads `layout.Blocks`); then resolve `ModeGroups[action]` once. Obs offsets and
action indices are therefore global across the layout and depend on the stage's list order.

Invariants: block order is the stage list; `Core` first so the action catalog is at global actions `0..A-1`; action 0 is
always the no-op (`SeatEncoder`); `ModelName() = Profile->Name + Stage->Suffix` (e.g. `warrior_roles`).

### Manifest (`Layout::Manifest`, `Layout.cpp:193`)

JSON written to `<LayoutsDir>/<stage>/<ModelName>.json` by `StageScenario::WriteStageFiles` (`StageScenario.cpp:1244`).
`MANIFEST_FORMAT = 9` (`Layout.cpp:51`; format history in its comment: 3 generic block list, 5 3D steering, 6
pathfinder choices left, 7 jump drop, 8 controller move block, 9 engine moves gone). Keys: `format`, `model`, `stage`,
`class_name`, `class` (id), `obs_dim`, `num_actions`, `action_names`, `specs` (per spec `name`, `tree`, `aptitude`
object
of the 26 features from the standard build at level cap), `blocks` (per block `name`, `obs` `[first,count]`, `actions`
`[first,count]`, `revision` if non-zero, plus the block's `DescribeManifest` entries). A runtime that plays an exported
model must rebuild the same manifest.

### stage.json layout entries (`StageScenario.cpp:1254-1495`)

Top level (format 3, `STAGE_FILE_FORMAT`): `format`, `stage`, `suffix`, `extends`, `summary`, `seats`, `blocks` (names),
`arenas` (name, weight, seats, episode_seconds, plan, eval_only, stand_in_share,
drill_seat),
`cast`, `seed_chain`, `merges`, `state` (arena columns of the critic state), `models` (class name -> model name),
`layouts`, `episode_info`, `episode_categories`, `reward_terms` (term name -> outcome/cost/shaping), `goals` (kinds,
accepts matrix, targets, block "goal", column offsets, width, `slots_on_wire`), `tuning` (every `CurriculumTuning`
value).
Per class under `layouts.<class>`: `obs_dim`, `num_actions`, `action_names`, `spec_names`, `spec_roles` ("tank" for
`StatProfile::Tank`, "healer" for Healer, else "damage"), `sets` (`DescribeSeatSets`: only the pack's "enemies"), and
`blocks[]` with `name`, `obs`, `actions`, optional `revision`, and: for core `action_features` (8); vision `image`,
`camera`, `look`; entities `entities`; map `map`; sight `sight`; and for blocks with `DescribeColumns` the list
`obs_names`; for blocks with `DescribeRescaled` the list `rescaled`. The learner seeds from these (bootstrap.py).
Facts: the learner no longer has seat sets (`EntitySets`, `seat_sets` deleted) but the C++ always writes `sets` (always only "enemies" for
dungeon stages); `slots_on_wire` and goal order columns are written although nothing fills the order columns.

## SeatEncoder (`Layout/SeatEncoder.cpp`)

`Observe(view, obs, mask)` (`:35`): caller zeroes `obs`/`mask` with `mask[0] = 1` (`StageScenario::ObserveSeat` is the
one
caller). Writes the character (`CoreBlock::ObserveCharacter`) always. Dead seat: only duel `ObserveDead` and the goal
block, `mask[0] = 1`; none of move, vision, entities, map, sight, party frames, combat observe. No bot, or no target and
no hidden target and the layout cannot act without one: return after the character. `ActsWithoutTarget` = has gauntlet,
or sight, or no duel block (`SeatEncoder.h:39`). Otherwise every block's `Observe` in layout order, timing each per
thread into `ObserveTally` (slot `BLOCK_COUNT` is the pre-block view work; `ObserveTotal` is read by `forge status`).

`Apply(view, action, result)` (`:84`): dead -> only `duel.self_resurrect`; no target and layout cannot act without one
->
nothing. Locate the block of the action; clear expired options; any action > 0 cancels every non-standby option
(`HoldInterrupt` is the only standby, `IsStandby`); if the block `PressesFirst` apply it first; run every block's
`BeforeApply`; if not already applied, apply the action. Order consequence: a spell meets the world as observed, a move
press is applied after options ran.

## SeatView and SeatActionResult (`Layout/SeatView.h`)

`SeatView` is one seat's situation at a decision, filled by `StageScenario::ViewSeat` (see cpp-stagescenario.md):
goal/goal2 and ended/reached/event/achieved flags; options (`SeatOptionSet`, one `Standby` slot: rest or held
interrupt); target, hidden target, last-seen place; level, race, spec, `Aptitude`; `Facing`; borrowed state pointers
(trail, controls, body, image, free-look, seen list, ray hits, mental map and its crop row, entity memory, sight GUIDs,
focus, client port); breath; talent build; last-step damage/power; `Memory`; `NowMs`, `DecisionMs` (default 250);
duel data (supplies, stable); enemies (24) and target slot; rank tier; gauntlet data; `Teammates`/`Tank` (7 slots; read
only by `Encoding::FriendUnit`, Core goal-closing and the goal block); party `Frames` (4) and `MinimapYards`; named
task;
objective data, compass withheld, `ObjectivePlaceKnown`, move/close rates; `WorldView` places.
`SeatActionResult`
is what a press did (cast counts, refusals, heal/downrank counters, pet orders, steering reversal counts,
`ActRefused`, `ActedOn`, stealth flags, `CastAt` ...) for rewards and `JudgePress`.

## SeatMemory (`Layout/SeatMemory.cpp`)

Per-seat state across decisions: `_readyMs[action]`, `_pressedMs[action]`, the current cast, last move, per-`ModeGroup`
change times, health averages (3 s time constant, `HEALTH_TREND_MS`).
- `Paced(layout, action, now, ActionTuning)`: true if the action is within `RepeatMs` (1000) of its last press, or
  `MoveRepeatMs` (300) for a movement action; if `duel.stop_casting` within `StopCastMinMs` (500) of a cast start; if
  the
  action's `ModeGroup` changed within `ModeLockMs` (5000). `StageScenario::ObserveSeat` sets `mask[a] = 0` for every
  paced action (`StageScenario.cpp:3711-3721`). So pacing is a mask, not a price.
- `Press(...)`: sets ready/pressed times; stopping a cast delays recasting the stopped spell by `RecastAfterStopMs`
  (2000).
- Features for Core: `SincePressed` (/10 s), `SinceMove` (/5 s), `SinceModeChange` (/10 s), the two trends.
Config keys: `Actions.RepeatMs`, `MoveRepeatMs`, `StopCastMinMs`, `RecastAfterStopMs`, `ModeLockMs` (defaults above).
Tests: none. Reviewer notes: `_targetDropRate`, `_manaSpendRate`, `_target`, `_mana`, `_lastTarget`, `_lastMana` are
computed but have no reader (dead). The pacing mask applies to the NOOP-free actions only (`action >= 1`). Pacing masks
contradict principle 5 as literally stated (only physically impossible presses are masked); owner to confirm it is
intended (tuning comment calls it "masked until it may be pressed again", `CurriculumTuning.h:348`).

## Encoding helpers (`Layout/EncoderSupport.*`)

Namespace `Animus::Curriculum::Encoding`. Groups:
- Lookup: `UnitThrough`, `CreatureThrough` (map-null-safe `ObjectAccessor`), `FriendUnit(view, slot)` (slot 0 self; 1
  "owner"
  names nobody; 2..8 teammates), `FirstPet`, `RelativePosition`.
- Casting: `TargetsFor`, `BeneficialTarget(bot, focus, selection)` (focus if living friend, else selection if friend,
  else
  self), `SupportTarget` (that, only in a layout with the sight block; otherwise the bot), `CanCast`,
  `KnownRank(view, def)` (view's per-episode table else rank walk; rank tier picks about 2/3 or 1/3 up the known ranks,
  skipping ranks the spellbook deactivated), `IsSpellActionAllowed` (see core mask), `SituationalFailure` (maps cast
  results to Facing/Range/Sight/Moving/Power: offered then priced), `ApplySpellAction` (form drop, prepare/CMSG cast,
  fills `SeatActionResult`), `NotePressRefused` (logs the first 600 refusals per process).
- Items: `TrinketSpell`, `UseSpell`, `CanUseItemOn`, `UseItemOn`, `ItemCooldownFraction`, `CancellableForm`.
- Hazards: `StandingInHazards` (auras of dynamic objects), `FindNearestHazard` (grid visit of dynamic objects and traps
  within 30 yd), `TrackNearestHazard` (search once per second, re-measure each call).
- Perception facts: `IncomingDebuffs`, `ThreatShare`, `IsCrowdControlled`, `CrowdControlledBy`, `WriteOpponentType`
  (7 types), `OBSERVED_SCHOOLS` (6), `OBSERVED_MECHANICS` (6), immunity, armour reduction, damage modifier.
- Actions: `SelectEnemy`, `PetAttack`, `StartCallBeastCooldown`.
Reviewer notes: `ApplySpellAction` dereferences `view.L` without a null check at `EncoderSupport.cpp:457` while other
paths guard it; `new Spell` on a failed `prepare` is not deleted (relies on the core's self-ownership; UNVERIFIED);
`NotePressRefused` caps log lines with a process-global counter; `HAZARD_SEARCH_*` constants; stale doc comments
(`SupportTarget`, "Revive features" orphan, `RelativePosition` used by encounters only). Tests: none directly.

## ClassProfile (`Character/ClassProfile.*`)

`PLAYABLE_RACES` (10: human, orc, dwarf, night elf, undead, tauren, gnome, troll, blood elf, draenei) and
`PLAYABLE_CLASSES` (10) fix the one-hot orders. `ClassProfiles()` is the class table in this stable order: warrior,
paladin, hunter, rogue, priest, deathknight, shaman, mage, warlock, druid. Specs (name, talent tab page, `StatProfile`,
`RangeBand`, weapon layouts, wand flag): 31 specs, druid has four (balance, feral_cat, feral_bear both on tab 1,
restoration). Pinned by `LiveLayoutPinTest`. Classes only append (principle 15). `DamageScale(level) = 15 exp(0.068
level)`.
`SpecProfile` carries no role; role is read off the build (`Aptitude`). Reviewer: `Random.h` included unused.

## ClassAssets (`Character/ClassAssets.*`)

`ClassAssets::For(profile)` (`:37`): process-wide cache under a mutex: per class a `ClassKit`, `TalentBuilder` and
`ActionCatalog`; per profile the races that exist for the class (`sObjectMgr->GetPlayerInfo`), a `GearBuilder` for the
profile's specs, and `SpecAptitudes` (Aptitude of the standard build at 71 points, no bot). Built on first use; the
comment says warmed at startup by `WarmCaches` (see WarmCaches.cpp) and built on a map thread only for a missed profile.
Building queries the world database (kit) and probes characters (catalog); if that happens on a map thread after the
database is sealed it will fail (UNVERIFIED: confirm `WarmCaches` covers every profile). `SpecsMeeting(demand)` returns
spec indices whose standard build meets an `AptitudeDemand`. Tests: none.

## ActionCatalog (`Character/ActionCatalog.*`)

The fixed action space of one class. Construction (`ActionCatalog.cpp:293`):
1. Candidate spells = spells known by throwaway level-80 characters of every race that exists for the class after
   `ClassKit::Learn` (`BotFactory::Create`, account `BotAccounts::Probe(race)`), plus all kit spells, plus every talent
   rank spell, plus spells those teach (LEARN_SPELL closure); self-control spells (Grovel) removed.
2. Rank chains (by first rank id, ascending, `std::set`) are assigned to the first list that fits: combat
   (`IsCombatSpell`), else tactical (`IsTacticalSpell`: interrupt, knockback, taunt, distract, traps, offensive dispel,
   stun/silence/fear/root/transform/disarm, snares), else sustain (`IsSustainSpell`: heals, absorbs, friendly dispel).
3. `Actions()` order: `noop`, `cancel_queued`, combat chains, `trinket_1`, `trinket_2`, `use_main_hand`, `use_off_hand`,
   tactical chains, sustain chains. `Tactical()`/`Sustain()` are copies of the tails; `Revives()` (resurrection spells,
   plus `soulstone` for warlocks) is separate.
4. `Action::Index` is the position in its own list. Name: `<lowercase alnum spell name, _ separated>_<first rank id>` or
   `spell_<id>`.
Per-action flags: `Healing, Rankable, DirectHeal, KeepsAura, Defensive, LongBuff, Dispel, DispelFriendly, DispelMask,
FeatherFall, WaterBreathing, WaterWalk, NextSwing, From`. Static predicates are the classification rules (exclusion
lists
`IsExcludedEffect/Aura`, damage-relevant and survival aura lists).
Seeding consequence: catalog size and order depend on the spell DBC and the class kit, so core width varies by class
and data; action NAMES carry the identity. A spell added to or removed from a chain list shifts positions of later
actions; seeding fixes this by name (`_core_by_name`). Name collisions are impossible (id suffix).
Reviewer notes: `Revives()` is used only by `Aptitude::Of`; `Aptitude::Of` iterates `Actions()`, then `Tactical()` and
`Sustain()` again, although `Actions()` already contains them, so tactical and sustain spells are counted twice in the
count-based features (`Aptitude.cpp:251-256`); changing this changes observed columns 11-36 (needs a core revision
bump). Probe characters are created per race at construction: cost and DB dependence. No tests.

## Aptitude (`Character/Aptitude.*`)

A 26-float vector measured from a build (and, with a bot, its known spells, offhand shield and armour): taunt,
mitigation, threat, direct_heal, hot_heal, area_heal, melee_damage, spell_damage, ranged_damage, control, interrupt,
buff, dispel_friendly, cleanse_magic, cleanse_curse, cleanse_disease, cleanse_poison, dispel_offensive, protect_other,
revive, battle_revive, pet, water, tree_0, tree_1, tree_2 (shares of points spent). Saturation points: threat 2, heals 3
or 2, control 5, buff 5, protect_other 2, water 2, mitigation `defensives/4` (+0.5 shield, +0.25 armour), multiplied by
the tank tree's share of points (so a holy paladin reads low mitigation). `WriteBrief` (6 numbers) and
`AptitudeDemand` (`HoldsThePull` = mitigation >= 0.5, `KeepsThemUp` = direct_heal >= 0.34) decide casting
(`StageScenario::Castings`). It is computed per seat per episode (`StageScenario::Configure`) and written to Core
columns 11-36. Tests: none. Note the manifest `specs[].aptitude` uses the same function with no bot.

## ClassKit (`Character/ClassKit.*`)

Reads the world database at construction: class trainers' spells (`trainer`, `trainer_spell`, type 0), class-quest
reward
spells (`quest_template` joined to `quest_template_addon.AllowableClasses`), plus a hard-coded Raise Dead for death
knights; sorted by level. `Learn(bot)` teaches those at or below the bot's level whose required abilities are known (two
passes). `StoreReagents` (11 hard-coded reagent rows: totems, ankh, fish scales/oil, soul shards, corpse dust, light
feather). `ArmorSubclass(level)`. `MinLevelOf`: 55 for death knights, else 1. Tests: none. Reviewer: `ReqAbility` chain
longer than the two passes is not learned; reagent list is data in code.

## TalentBuilder, SpecBuilds, PetTalents

`TalentBuilder(class)` (`TalentBuilder.cpp:62`): all talents of the class from the DBC, sorted by tab page, row, column;
prerequisites by index; resolves glyph items by name from `SpecBuilds`. `Standard(spec, tab, points)` spends points in
the
spec list's order (first talent that wants ranks and `CanTake`: 5 points per row, prerequisite), then leftover points at
random (own tree, then others); `Noisy` = standard for `points - move` then random; `Random` = random with row-weighted
draw (`SPEC_TREE_POINTS 51` in the spec tree first). `Apply` calls `Player::LearnTalent`. `ApplyGlyphs` fills unlocked
slots
(levels 15, 15, 50, 30, 70, 80). `SpecBuilds()` is 460 lines of data for 31 specs; its header says it is generated by
`tools/spec_builds/generate.py` and checked by `validate.py`; that generator and its `builds.py` data were deleted from
the repository (git history keeps them), so the file is edited by hand. `PetTalents::Spend` spends hunter pet points from three fixed builds (ferocity,
tenacity, cunning). Tests: `LiveLayoutPinTest` reads `TREE_COUNT`. No other.
Reviewer: random draws use the global `urand`, so builds depend on the map thread's RNG state (UNVERIFIED how seeded
episodes seed it; `RandomLevel` comment says "one roll from the world thread's random numbers").

## Gear (`GearBuilder`, `GearEnhancements`, `GearStats`)

`GearBuilder(profile, kit)` builds per-`StatProfile` pools of "obtainable" items (loot tables, vendors, quest rewards,
crafting: world-DB queries, cached statics) with weights dungeon 4, quest 3, other 1. `Equip(bot, spec)`: destroys
everything, then fills armour slots from pools by item-level band (`ITEM_LEVEL_ANCHORS`, 16 anchors from level 1 to 80,
linear in between), widening below the band by 0/10/25/1000, never above; epics only at levels 70 and 80; PvP
(resilience)
items never; weapon layouts tried in order; wand, relic, quiver, ammo, reagents; then `Enhance` (enchants and gems
always
at 70 and 80, 50% each item while levelling; death knight runes, rogue poisons, shaman imbues). Candidate choice
weighted by source and closeness to the band centre; up to 6 attempts per slot. `Window` is memoised under a
shared mutex. `WarmGearCaches()` warms statics. `GearStats` functions (preferences by stat profile, enchant verdicts,
`ObtainableItems`, `ItemSources`, `ItemUseSpell`) are defined in `GearBuilder.cpp`, there is no `GearStats.cpp`.
Tests: none. Reviewer: uses global RNG; world-DB access on first call.

## Supplies and WorldCreatures

`ConsumablePool` (best food/drink/potions/bandage/healthstone/soulstone by level, flasks at 70/80 and elixirs 50% while
levelling), `StablePool` (random tameable beast of 4 different families; spawned, non-waypoint creatures),
`StockBattleSupplies` (5 of each potion/bandage, 1 stone, First Aid skill, drink the buff), `StockConsumables`,
`CallHunterBeast`, `CanCallHunterBeast`, `CONSUMABLE_COUNT 5`, `HUNTER_PET_LEVEL 10`. `WorldCreatures` runs two world-DB
queries once. No tests.

## SeatCharacter and how a seat is built

`SeatCharacter::Configure(bot, layout, specIndex, plan, noisePoints)` (`SeatCharacter.cpp:27`): hold stat recompute,
learn proficiencies, build talents per `TalentPlan` (Standard/Noisy/Random), `ClassKit::Learn`, glyphs, gear, resume
recompute, fill health and powers (rage and runic power 0). `PrepareFighter`: no-XP flag, hunter stable (4 beasts), a
warrior casts Defensive Stance when `HoldsThePull` and it has a taunt, else Battle Stance. `GivePet`: puts a pet out
without casting (hunter stable beast; random known warlock demon; death knight with Master of Ghouls; frost mage with
Glyph of Eternal Water).
Seat build in the scenario (`StageScenario.cpp:2160-2530`): level from stage `Level`, episode level, kept level,
focus band (`FocusLevelFirst/Last/Chance`; evaluation of a wholly-focused stage is in the band too), else
`RandomLevel` (Characters.HighLevelFirst 61 / chance 50, LowLevelLast 20 / chance 15, otherwise any); minimum level is
max(stage, arena, class minimum 55 for death knights, tank-mode spell level). Race: random among the class's races (one
faction when `EpisodeTeam`). Spec is drawn with the class (a "casting"). Talent plan drawn with
`Characters.NoisyTalentChance 30`, `RandomTalentChance 10`, `TalentNoisePoints 5`. Characters are reused for
`Characters.ReuseEpisodes` (4) episodes when the same class and build is drawn again (never in evaluation, never when
the
map changes or for a wing's fresh instance); `Characters.KeepCasting 1` makes training keep class and build for such
runs.
Pets: `Characters.PetOutChance 50`. After `Configure`, `KnownRanks` and `Aptitude` are computed. Death knights are
excluded
from the level-band dungeon stages by minimum level (principle 8).

## EntityActions and IncomingSpell

`EntityActions` (`Character/EntityActions.*`): the client-packet layer. `Press` {Select, Interact, UseItem, Assist,
Focus}. `Apply(press, bot, guid, focus, result, port, resolve)`: resolves the entity as the client has it (`AtClient`:
same map, in world, within sight range, detectable), judges (`Refusal`: Gone, Kind, Reach, Sight, Loot, NoItem,
NoTarget,
Cast, Locked), then sends `CMSG_SET_SELECTION`, `CMSG_ATTACKSWING`, `CMSG_GAMEOBJ_USE`, `CMSG_GOSSIP_HELLO`,
`CMSG_USE_ITEM`, `CMSG_CAST_SPELL`, `CMSG_PET_ACTION` through `SessionPort()` (a `ClientPort`; tests replace it) into
the
session's own handlers. Refusals are priced (`ActRefused`), never masked. Loot is refused in every form (chests, nodes,
fishing, corpses). A cast watch (`Movement::ScopedCastWatch`) reads back the server's refusal. Key items: the first bag
or
keyring item whose use spell takes a game object (for objects) or unit. Tests: `SightBlockTest`, `InteractStageTest`,
`DungeonStagesTest`. Notes: cast count is a per-thread counter; pet bar orders from `PetBlock` and consumables from
`DuelBlock` do not use this layer.
`IncomingSpell` (26 features of an enemy's current cast, read from `SpellInfo`, no ids): casting, progress, remaining/3
s,
aimed at me, area, cone, channeled, interruptible, dispellable, shared, heals, summons, radius/40, travel, school
one-hot
(6), mechanic one-hot (6). `Classify`/`Prevented` is used only by `EnvPool.cpp:900`. No tests.

## Observed issues

- `SeatMemory` forecast state unused; pacing is a mask (principle 5).
- `Aptitude::Of` double-counts tactical and sustain spells.
- `SpecBuilds.cpp` names generator tools that are absent.
- `GearStats` has no .cpp (definitions in `GearBuilder.cpp`).
- `EncoderSupport.h`: `SupportTarget` comment mismatches the code; `Teammates`/`FRIEND_OWNER` stubs remain.
- `ClassAssets::For` may query the database from a map thread if warm-up missed a profile.
- Manifest `MANIFEST_FORMAT` comment block mixes `///` and `//`; format 4 undocumented.
- Core block mask removes heal-on-full-health and refresh spells, against principle 5 wording.
- `ClassKit` hard-codes a reagent table and a Raise Dead quest spell.

## Questions for the owner

- Should pacing, heal-on-full and refresh masks remain, or be priced? If priced, a core revision bump is not needed
  (masks only), but eval comparisons change.
- Fix the Aptitude double counting (needs a core revision bump) or leave?
