# Layout blocks (C++)

Purpose and scope. This document describes the 14 live observation/action blocks of a layout, the `BlockId` enum, the
`Block` interface and registry, and the helpers that live beside them, all under
`src/server/game/Animus/Scenario/Curriculum/Blocks/`. How a layout is assembled from blocks, the encoder, the manifest
and `stage.json` are in [cpp-layout-character.md](cpp-layout-character.md). Rewards and routing are in
[cpp-rewards-routing.md](cpp-rewards-routing.md). Related: [cpp-vision.md](cpp-vision.md) (camera, entity memory, mental
map internals), [cpp-encounters.md](cpp-encounters.md), [cpp-stagescenario.md](cpp-stagescenario.md),
[protocol.md](protocol.md), [file-formats.md](file-formats.md), [tests.md](tests.md),
[known-issues.md](known-issues.md),
[glossary.md](glossary.md).

All paths below are relative to `src/server/game/Animus/Scenario/Curriculum/` unless they start with `src/`. Line
numbers
are those of commit `bd32b9dc8`.

## Map table

| Path | Lines | Role |
| --- | --- | --- |
| Blocks/Blocks.cpp | 118 | Registry (`GetBlock`) and `DescribeSeatSets` (only the pack's "enemies" set). |
| Blocks/CoreBlock.h | 144 | Core block constants, column enum, revision 1. |
| Blocks/CoreBlock.cpp | 527 | Character features, per-catalog-action features, rank tiers, goal-closing masks. |
| Blocks/MoveBlock.h | 168 | Move block column enum, revision 5. |
| Blocks/MoveBlock.cpp | 346 | Move observation (controls, body, trail), masks, press application, column names. |
| Blocks/MoveControls.h | 267 | Pure 25-action key/mouse model, masks, `Press`. |
| Blocks/MovePrice.h | 90 | Pure steering-reversal pricing helpers. |
| Blocks/CompassBlock.h | 65 | Compass columns, revision 1. |
| Blocks/CompassBlock.cpp | 87 | Objective bearing/distance/detour. |
| Blocks/DuelBlock.h | 173 | Duel column and action enums. |
| Blocks/DuelBlock.cpp | 473 | Target/self-state features, consumables, pet-bar-free actions. |
| Blocks/PackBlock.h | 89 | Pack slots enum. |
| Blocks/PackBlock.cpp | 135 | 24 enemy slots, select-slot actions, hold-interrupt. |
| Blocks/GauntletBlock.h | 71 | Gauntlet enum. |
| Blocks/GauntletBlock.cpp | 181 | Pull timing, eat, drink, rest-until-ready. |
| Blocks/PetBlock.h | 136 | Pet bar enums. |
| Blocks/PetBlock.cpp | 579 | Pet state, ability classifier, orders, abilities. |
| Blocks/VisionBlock.h | 65 | Camera block, revision 5. |
| Blocks/VisionBlock.cpp | 219 | Free-look advance, frame render, seen list; manifest. |
| Blocks/EntitiesBlock.h | 94 | Visible-entity set, revision 1. |
| Blocks/EntitiesBlock.cpp | 124 | Writes entity memory and the 32 x 20 columns. |
| Blocks/MapBlock.h | 65 | Mental-map scalars, revision 1. |
| Blocks/MapBlock.cpp | 140 | Writes the map from the frame, crops, four scalars. |
| Blocks/SightBlock.h | 146 | Seen+remembered list and pointer presses, revision 2. |
| Blocks/SightBlock.cpp | 311 | List writer, named row, masks, press dispatch. |
| Blocks/PartyFramesBlock.h | 138 | Party frames, revision 2. |
| Blocks/PartyFramesBlock.cpp | 276 | Frame fill from the core group, minimap dots, presses. |
| Blocks/CombatBlock.h | 200 | Player/pet frames, target frame, per-slot combat columns, revision 1. |
| Blocks/CombatBlock.cpp | 344 | Threat status, visible enemies, ground fire, presses. |
| Blocks/GoalBlock.h | 95 | Goal availability block (128 columns, no actions). |
| Blocks/GoalBlock.cpp | 258 | Availability, goal status, "earned" rule. |
| Blocks/LayeredField.h | 126 | NOT a block: layered height field types and store (documented in cpp-rewards-routing.md). |
| Blocks/LayeredField.cpp | 478 | NOT a block: field bake, file I/O, cache. |
| Layout/Block.h | 305 | `BlockId`, `Block` interface, sizing constants, goal space (documented here). |

`Layout/Block.h` belongs to the Layout directory and is mapped again in cpp-layout-character.md.

## The Block interface and registry

`Layout/Block.h:236`. A block is stateless; everything it reads is the world and the `SeatView`
(`Layout/SeatView.h`). Virtuals:

| Method | Default | Meaning |
| --- | --- | --- |
| `Size(Layout const&) -> {Obs, Actions}` | pure | Columns and actions the block adds; may read `layout.Profile`, `layout.Assets`, `layout.Blocks`. |
| `Revision()` | 0 | Bumped when columns change meaning at the same place. Written to the manifest and to stage.json only when non-zero (`Layout/Layout.cpp:237`, `StageScenario.cpp:1357`). |
| `DescribeRescaled` | none | Columns whose scale changed in place (`{tag, first, count}`); only Core uses it. |
| `DescribeColumns` | none | Column names for by-name seeding. Blocks that name columns: Move, Compass, Combat, PartyFrames. All others write no names. |
| `DescribeManifest` | none | Block-specific manifest entries. |
| `Observe(view, obs, mask)` | pure | Write features and mask; `obs`/`mask` already point at the block's slice (`MoveBlock.cpp:199`). `mask == nullptr` means no mask wanted. |
| `BeforeApply` | none | Runs for every block of the layout on every applied decision (`SeatEncoder.cpp:128`). |
| `PressesFirst` | false | True only for Core spell/trinket actions: applied before every BeforeApply. |
| `Apply(view, local, result)` | none | Apply action `local` (0-based in the block). |
| `IsMovement(local)` | false | True for every Move action; paces with `Actions.MoveRepeatMs` instead of `Actions.RepeatMs`. |
| `ModeGroupOf` | None | Standing-choice kind (`ModeGroup`) for mode-lock pacing. |
| `ActionName` | empty | Name; empty means `<block>_<local>` (`Layout/Layout.cpp:185`). |

Registry: `GetBlock(BlockId)` (`Blocks/Blocks.cpp:78`) holds one static instance per block in an array of
`BLOCK_COUNT` (27) pointers keyed by id; an unregistered id trips `ASSERT`. Adding a block = new id, class, one entry
here, one case in `BlockName` (`Layout/Layout.cpp:103`).

### BlockId (`Layout/Block.h:42`)

| Id | Name (manifest) | Class |
| --- | --- | --- |
| 0 | core | CoreBlock |
| 1 | move | MoveBlock |
| 2 | compass | CompassBlock |
| 3 | duel | DuelBlock |
| 4 | pack | PackBlock |
| 5 | gauntlet | GauntletBlock |
| 11 | pet | PetBlock |
| 20 | vision | VisionBlock |
| 21 | entities | EntitiesBlock |
| 22 | map | MapBlock |
| 23 | sight | SightBlock |
| 24 | party_frames | PartyFramesBlock |
| 25 | combat | CombatBlock |
| 26 | goal | GoalBlock |
| 27 | Count (`BLOCK_COUNT`) | not a block |

Ids 6-10 and 12-19 are unused gaps (first curriculum's deleted blocks). The comment says they are never reused; nothing
in code enforces it other than the explicit numbers (`Block.h:83`). `Layout::_blockMask` is a `uint32` so ids must stay
below 32.

## Layout order, validity, and which stage declares which block

Layout order is the stage's `Blocks` list (`Layout::Build`, `Layout/Layout.cpp:127`). Stage validation
(`Stages/Stages.cpp:1185-1260`, `Problem`) requires: blocks begin `core, move`; no block twice; map needs vision; sight
needs vision and an `entities` entry before it; combat needs sight before it; combat with more than one seat needs
party_frames; a stage that fights needs duel. A stage's vision gets `entities` inserted right after it automatically
(`Stages.cpp:1360`), which is why the move stages' `.Blocks` lists omit it.

Declared blocks per live stage (exact; from `Stages.cpp:649-1020` and the pin golden
`src/test/server/game/Animus/LiveLayoutPin.golden.inc`; `x` = in the layout, class-independent totals from the golden):

| Stage | core | move | compass | duel | pack | gaunt | pet | vision | entities | map | sight | party | combat | goal | class-indep obs / actions |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| move1_controls | x | x | x | | | | | x | x | | | | | x | 842 / 25 |
| move2_seek | x | x | | | | | | x | x | x | | | | x | 840 / 25 |
| move3_interact | x | x | | | | | | x | x | x | x | | | x | 2911 / 346 |
| move4_follow | x | x | | | | | | x | x | x | | x | | x | 924 / 37 |
| combat1_fight | x | x | | x | | | x | x | x | x | x | | x | x | 3779 / 350 |
| combat2_packs | x | x | | x | | | x | x | x | x | x | | x | x | 3779 / 350 |
| combat3_survive | x | x | | x | | x | x | x | x | x | x | | x | x | 3790 / 353 |
| group1_roles | x | x | | x | | x | x | x | x | x | x | x | x | x | 3874 / 365 |
| group2_corridor | x | x | | x | x | x | x | x | x | x | x | x | x | x | 4908 / 390 |
| dungeon1_pulls | same as group2_corridor | | | | | | | | | | | | | | 4908 / 390 |
| dungeon2_ragefire | same | | | | | | | | | | | | | | 4908 / 390 |
| dungeon3_deadmines | same | | | | | | | | | | | | | | 4908 / 390 |

"Class-independent" excludes core, duel and pet, whose widths depend on the class (below). The dungeon stages use
`DungeonBlocks()` = `{Core, Move, Duel, Pet, Pack, Gauntlet, Vision, Entities, Map, Sight, PartyFrames, Combat, Goal}`
(`Stages.cpp:612`). The sight block is 2071 columns in move3 and 2903 in every stage that also has combat
(`SightBlock::Width`, below). Pet contributes 0 for a class without a pet.

Seed lineage (`Extends`): move1 -> move2 -> {move3, move4}; move3 -> combat1 -> combat2 -> combat3 -> group1 (also
merges move4) -> group2 -> dungeon1 -> dungeon2 -> dungeon3 (`Stages.cpp`).

## Pin test and golden

`src/test/server/game/Animus/LiveLayoutPinTest.cpp` (219 lines) with golden
`src/test/server/game/Animus/LiveLayoutPin.golden.inc` (149 lines). It builds, for the 12 live stages, the block list
and, for every block except core, duel and pet (class-dependent, pinned only by name, id and revision), the obs and
action counts, the `DescribeColumns` count and an FNV-1a hash over columns, action names and manifest entries. It also
pins every sizing constant (`PACK_SLOTS=24`, `SIGHT_SLOTS=64`, `GOAL_JOINT_COUNT=348`, `BLOCK_COUNT=27`, state widths,
`core.OBS_GLOBAL_COUNT=91`, ...) and the class list `warrior:1 paladin:2 hunter:3 rogue:4 priest:5 deathknight:6
shaman:7 mage:8 warlock:9 druid:11` with their spec names. Regenerate only deliberately with `ANIMUS_PIN_PRINT=1
--gtest_filter=LiveLayoutPinTest.*`. Caveat: the test builds a bare `Layout` and sets `Stage` and `Blocks` but never
`_blockMask`, so `Layout::Has` is false for everything while sizing; this is why `SightBlock::Width` reads
`layout.Blocks` instead of `Has` (`SightBlock.cpp:109`). Any size function that used `Has` would be pinned wrong.

Pinned revisions (golden): core 1, move 5, compass 1, duel 0, pack 0, gauntlet 0, pet 0, vision 5, entities 1, map 1,
sight 2, party_frames 2, combat 1, goal 0.

## The by-name seeding contract

What the C++ side promises; the implementation is `apps/forge/python/animus/bootstrap.py` (see py-learner.md).

1. A stage is seeded from its `Extends` (seed chain in stage.json, `StageScenario.cpp:1308`), plus `Merges` for blocks
   only
   they have. Seeding is block by block, matched by block NAME in stage.json `layouts.<class>.blocks[].name`.
2. A block is carried whole, by position, when both stages have it, with equal revision (missing = 0), equal obs width
   and equal action count (`bootstrap._common_blocks`).
3. Revision differs: the block starts fresh, except columns/actions matched by name (needs `DescribeColumns` /
   `ActionName` / `action_names`). A revision bump therefore throws away a block's weights wherever names do not rescue
   them; that is its purpose.
4. Width differs, same revision: the core block is re-matched action by action by name (`_core_by_name`, uses
   `CORE_GLOBAL_FEATURES = 91`, `CORE_ACTION_FEATURES = 8`, rank-tier actions recognised by the `rank_` prefix); a block
   with a seat set whose only change is slot count is re-matched by slot (`_slots_grown`; the only set the C++ writes is
   the pack's "enemies", `Blocks.cpp:69`); `GROWS_AT_END = {"crowd"}` is a dead entry (no crowd block exists). Anything
   else starts fresh (the rest of the layout still seeds).
5. Actions carry by name only where the block is re-matched by name. Appending actions at the END of a block is safe
   only through paths 4; otherwise the width change restarts the block.
6. Class table (`ClassProfiles()`) only appends; `PLAYABLE_RACES`/`PLAYABLE_CLASSES` orders define one-hots.
7. `DescribeRescaled` (Core only: tag `tree_share`, aptitude's tree shares and the block's tree columns) resets the
   normaliser statistics of those columns when the parent lacks the tag (`bootstrap._seed_rescaled_norms`).

Open question for the owner: the sight block is 2071 wide in move3_interact and 2903 in combat1_fight (13 combat
columns per slot x 64 + ...); the stage comment (`Stages.cpp`, combat1 comment) says "its slots widened by the combat
columns, which start at zero", but the C++ declares no seat set for sight, so bootstrap path 4 would not apply and the
block would restart. UNVERIFIED: check `bootstrap.py` / stage.json `vision`/`sight` handling for combat1_fight seeding.

## Shared sizing constants (`Layout/Block.h:102-145`)

`RAID_GROUPS 8`, `GROUP_SEATS 5`, `MAX_SEATS 40`, `TEAM_SEATS 10`, `TEAM_COUNT 2`, `GROUP_MEMBERS 4`,
`SPOTLIGHT_SLOTS 3`, `PARTY_MEMBERS 7`, `PACK_SLOTS 24`, `NAMED_ENEMY_SLOTS 4`, `ENEMY_COUNT_SCALE 4`, `CROWD_SLOTS 4`,
`SIGHT_VISIBLE_SLOTS 32`, `SIGHT_RECALLED_SLOTS 32`, `SIGHT_SLOTS 64`, `TRAIL_SAMPLES 8`, `STABLE_SLOTS 4`,
`FRIEND_SLOTS 2 + PARTY_MEMBERS = 9`, `RANK_TIERS 3`. Goal space: `GOAL_COUNT 12`, `GOAL_TARGETS 29`
(none 0, enemy 1-4, friend 5-13, objective 14-17, giver 18, ender 19, place 20-27, assignment 28), `GOAL_JOINT_COUNT
348`. Several comments in `Block.h` name deleted blocks (PartyBlock, CompanionBlock, CrowdBlock, HostilesBlock,
SupportBlock).

## core (id 0, revision 1)

Does: the character, per catalog action readiness features, the talent build, and the spell actions. Files:
`Blocks/CoreBlock.h:29`, `CoreBlock.cpp`.

Size (`CoreBlock.cpp:139`): obs = `91 + A*8 + T + 3`, actions = `A + 3`, with `A` = catalog action count of the class
(`ActionCatalog::Actions().size()`), `T` = number of talents of the class (`TalentBuilder::Talents().size()`),
`TREE_COUNT = 3`. Widths vary by class and with game data (spell and talent DBC).

Global columns (index, all in the block's slice; normalisation as coded):

| Col | Name (enum) | Value |
| --- | --- | --- |
| 0 | OBS_LEVEL | level / 80 |
| 1-10 | OBS_RACE_FIRST | one-hot over `PLAYABLE_RACES` (10; static_assert ends before 11) |
| 11-36 | OBS_APTITUDE_FIRST | `Aptitude::COUNT` = 26 features (see cpp-layout-character.md, Aptitude) |
| 37 | OBS_HEALTH | health pct / 100 |
| 38 | OBS_MANA | mana / max, 0 without mana |
| 39 | OBS_RAGE | rage / 1000 (raw rage is x10, so display rage / 100; the header says "/ 100") |
| 40 | OBS_ENERGY | energy / max |
| 41 | OBS_RUNIC_POWER | runic / 1000 |
| 42-47 | OBS_RUNE_FIRST | per rune 1 - min(1, cooldown / 10 s); death knights only |
| 48 | OBS_COMBO_POINTS | combo points on target / 5 |
| 49-61 | OBS_FORM_FIRST | one-hot over 13 tracked forms (none, cat, tree, bear, dire bear, moonkin, shadow, stealth, battle/defensive/berserker stance, metamorphosis, ghost wolf) |
| 62 | OBS_GCD | remaining GCD / 1.5 s, set from the first known action with a GCD |
| 63 | OBS_CASTING | non-melee cast or channel |
| 64 | OBS_QUEUED_NEXT_SWING | a melee spell is queued |
| 65-67 | main/off/ranged swing | timer / attack time, clamped 0..1 |
| 68 | OBS_MAIN_HAND_SPEED | attack time / 4000 ms |
| 69 | OBS_TARGET_HEALTH | target hp / 100 |
| 70 | OBS_TARGET_DISTANCE | min(1, dist / 40) |
| 71 | OBS_IN_MELEE_FRONT | in melee range and 120 degree arc |
| 72 | OBS_ATTACK_POWER | AP / (100 + 50 level) |
| 73 | OBS_SPELL_POWER | magic bonus damage / (50 + 30 level) |
| 74,75 | melee crit, spell crit (max over schools) | percent / 100 |
| 76-79 | melee haste, spell haste, melee hit, spell hit | rating bonus / 100 |
| 80 | OBS_EXPERTISE | expertise / 30 |
| 81 | OBS_ARMOR_PENETRATION | rating bonus / 100 |
| 82 | OBS_LAST_STEP_DAMAGE | `view.LastStepDamage` |
| 83 | OBS_LAST_STEP_POWER_DELTA | `view.LastStepPowerDelta` |
| 84 | OBS_EPISODE_TIME | `view.EpisodeTime` (elapsed / 300000 ms, clamped) |
| 85 | OBS_SINCE_MOVE | `SeatMemory::SinceMove` (/ 5 s; 1 = none) |
| 86 | OBS_SINCE_MODE_CHANGE | `SeatMemory::SinceModeChange` (/ 10 s) |
| 87, 88 | health trend, target health trend | `SeatMemory` exponential averages (3 s) |
| 89, 90 | OBS_OPTION_FIRST | time left of the two durative options (rest, held interrupt) / 30 s |

Then per catalog action `a` (8 columns each at `91 + a*8`): `[0]` known (1 when a rank is known), `[1]` cooldown
fraction, `[2]` own aura on target (fraction, stacks in `[4]`), `[3]` own aura on self, `[4]` stacks, `[5]` time since
the
seat pressed it / 10 s (1 = never; written even when the spell is unknown), `[6]` ready, `[7]` affordable. Then `T`
talent ranks (rank / max rank), then 3 tree shares (points in tree / points spent). Written by `ObserveCharacter`
(`CoreBlock.cpp:275`) even for a dead seat: level, race, aptitude, talents, trees.

Actions: `A` catalog actions (index 0 noop, 1 cancel_queued, then spells, 4 trinket-slot actions, tactical, sustain; see
ActionCatalog) then `rank_high`, `rank_mid`, `rank_low` (`ActionName`, `CoreBlock.cpp:206`). Names for catalog actions
are `<lowercase_spell_name>_<first rank spell id>` (`ActionCatalog.cpp:192`).

Mask (`CoreBlock.cpp:302-436`): action 0 always set elsewhere; for each action `allowed && !GoalCloses`:
- noop allowed; soulstone kind never; cancel_queued needs a queued melee spell; trinket needs an on-use spell, no cast
  in
  progress, no cooldown, `CheckCast`; spell goes through `Encoding::IsSpellActionAllowed` (known and active, no
  cooldown,
  no cast in progress, next-swing rule, GCD, heal on full-health friend is masked, aura-keeping spell with more than 25%
  left is masked, shapeshift-from-shapeshift rule; facing/range/LOS/moving/power failures are NOT masked, they are
  priced).
- `GoalCloses`: under goals Recover/Prepare/Rest/TravelTo/Loot/Gather/Interact harmful spells are masked unless health
  < 35%, attackers on the seat or a teammate, or stealthed; under Fight/Control/Position long buffs are masked in combat
  (`CoreBlock.cpp:85-128`). With a secondary goal only what both close is closed.
- Rank tiers: all three offered except the tier already chosen.

Apply (`CoreBlock.cpp:438`): rank tier sets `view.RankTier`; trinket casts the on-use spell (counts `TrinketUses` or
`ItemUses`); spells go through `Encoding::ApplySpellAction` (client packet in a sight stage). `PressesFirst` is true for
spells and trinkets. `BeforeApply` runs the held-interrupt option: when the target casts, the first allowed
interrupting spell is cast and the hold stops (`CoreBlock.cpp:495`).

Reads from the world: the bot's stats, ratings, auras, the target, `SeatMemory`, `SeatOption`s, `view.Goal/Goal2`.
Config: none directly; pacing keys act in `SeatMemory` (see Layout doc). Revision history: 0 had five option clocks; 1
(player-controller C3) dropped three move-option clocks, shifting later columns by 3. Bump means a full restart of the
block except by-name action matching in `_core_by_name`.

Tests: `LiveLayoutPinTest` (name, id, revision, constants only). No unit test covers Observe/Apply/masks.

Reviewer notes: Core has no `DescribeColumns`, so its global columns and talent columns carry only positionally; the
talent segment is positional per class and moves if the talent DBC changes. `IsActionAllowed` for Trinket re-reads the
item each call (3 reads per action per decision). `OBS_GCD` is set inside the loop only if still 0. The header comment
for the rage column says "/ 100" but the code divides by 1000. `KnowsInterrupt` counts knockbacks and stuns/fears as
interrupts via `IsInterruptingSpell`.

## move (id 1, revision 5)

Does: where the seat puts its feet, as the player controller's held keys and mouse. No target needed. Size: 57 obs, 25
actions, class-independent (`MoveBlock.cpp:103`). Pin hash `a9e15899154cf795`.

Columns (named by `MoveBlock::ColumnName`; all in the block):

| Col | Name | Normalisation |
| --- | --- | --- |
| 0 | moving | 1 if a key held, speed > 0.1 or `bot->isMoving()` |
| 1 | speed | min(1, run speed / 14) |
| 2-5 | facing_sin, facing_cos, pitch_sin, pitch_cos | sin/cos of `view.Facing`, `body->Pitch` |
| 6 | held_forward | -1, 0, 1 |
| 7 | held_strafe | -1, 0, 1 (+ right) |
| 8 | held_vertical | -1, 0, 1 |
| 9 | held_turn | turn rate / 360 deg/s, clamped |
| 10 | held_pitch | pitch rate / 90 deg/s, clamped |
| 11 | held_walk | 0/1 |
| 12-14 | velocity_ahead, velocity_left, velocity_up | body velocity in its frame / 14, clamped +-1 |
| 15 | progress | moved / commanded last step (1 when nothing asked) |
| 16-19 | mode_ground, mode_falling, mode_swimming, mode_flying | one-hot of `Movement::Mode` |
| 20 | fall_time | fall ms / 3000 |
| 21 | fall_height | (apex - z) / 50, clamped |
| 22 | against_wall | body flag |
| 23 | steep_slope | body flag |
| 24 | depth | (liquid level - z) / collision height, clamped |
| 25 | can_jump | alive and `Movement::CanJump` |
| 26-28 | target_bearing_sin/cos, target_distance | target frame; dist / 40; zero without a target |
| 29-32 | hazard_bearing_sin/cos, hazard_distance, hazard_radius | nearest unentered hazard; /40 |
| 33-37 | in_water, submerged, submerged_time, swim_speed, airborne | swim speed / 7 |
| 38 | move_rate | `view.MoveRate` clamped 0..1 |
| 39 | close_rate | `view.CloseRate` clamped +-1 |
| 40-55 | trail_<i>_ahead / trail_<i>_left | last 8 positions (1 per second) in the seat's frame / 40, oldest first, newest in the last pair |
| 56 | trail_dwell | share of samples within 6 yd |

Actions (25, `MoveControls.h:57`, names `NAMES`): 0 move_forward, 1 move_back, 2 move_stop, 3 strafe_left, 4
strafe_right, 5 strafe_stop, 6-14 turn_right_360, _180, _90, _30, turn_stop, turn_left_30, _90, _180, _360 (rates in
degrees per second, `TURN_RATES_DEG`, + is left), 15-19 pitch_down_90, pitch_down_30, pitch_stop, pitch_up_30,
pitch_up_90, 20 ascend, 21 descend, 22 vertical_stop, 23 jump, 24 walk_toggle. Every action is a movement
(`IsMovement`), so `Actions.Repeat` is never charged on them.

Masks (`MoveControls::Allowed`): a dead seat presses nothing; pitch rates other than stop, ascend, descend need
`CanSteerVertically`; jump needs `CanJump`; everything else always allowed. Pressing the held value is legal.

Apply: `MoveControls::Press` changes one held control and returns a `PressOutcome`; unchanged = `KeyStillHeld`
(not charged); changed sets `ControlChanged`, `JitterWeight`, `BearingFlip`, `TurnReversals`, `PitchReversals`,
`Weaves`, `EffortWeight` on the result (priced by StageScenario, see rewards doc). JUMP is one-shot (controller clears
it). Pricing math is `MovePrice.h` (`COUNT_MS 1500`, `WEAVE_MS 4000`, `QUARTER_TURN`, `Recency`, `EffortOf`).

Reads: `view.Body` (controller body), `view.Controls`, `view.Look`-independent; map liquid; `view.NearestHazard`; trail
in
`view.Trail` (mutated inside the const Observe: first observation of an episode takes sample 1). Revision history
(`MoveControls.h:33`): 0,1 bearing/turn-lattice design (gone); 2 controls; 3 never shipped; 4 no ground rays, no
clearance; 5 objective columns moved to the compass block. Seeding from revision 4 maps columns by name through a
hard-coded list in bootstrap (`MOVE_REVISION_4_COLUMNS`).

Config: `Actions.MoveRepeatMs` (300), `Options.JitterDecayMs` via `view.Options`.
Tests: `MoveBlockTest` (no ray columns, column names), `MoveControlsTest` (layout, masks, presses, reversal pricing),
`MovePriceTest`, `LiveLayoutPinTest`, plus `PlayerControllerTest`/`KinematicsTest` for the controller.

Reviewer notes: `MovePrice::BearingSwing` and `MovePrice::Undone` are used only by tests (bearing design is gone).
`CLEARANCE_RANGE`, `MAX_STEP`, `MARCH_SLOPE` are kept in `MoveBlock.h` for routing (`LayeredField.cpp` includes the move
header for `MAX_STEP`: a routing file depends on a block). The block writes the trail inside a const method: a second
observation of a seat in one decision advances nothing but would add a trail sample if 1 s passed. `RUN_SPEED` comment
refers to a deleted TravelBlock. Hazard columns are fed from the aura/area search (`Encoding::TrackNearestHazard`) in
non-sight stages (see duel notes).

## compass (id 2, revision 1)

Does: whether there is an objective, its bearing, distance and the walking detour; no actions. Size 6 obs, 0 actions
(`CompassBlock.cpp:27`). Columns (named): 0 objective, 1 objective_bearing_sin, 2 objective_bearing_cos, 3
objective_distance (yards / 500, clamped), 4 objective_near (yards / 40), 5 detour (`view.Detour / 4`, clamped).
Declared
by move1_controls only. `view.CompassWithheld` zeroes all six (M1's withholding ladder: an input removed, never a mask;
the detour is also zeroed in that case). Detour is written even when there is no objective. Manifest: `objective_scale
500`, `near_scale 40`. Reads `view.Body`, `view.Objective`, `view.Facing`. Revision 1: the columns that were the move
block's up to its revision 4 (same names). Tests: `CompassBlockTest` (names equal revision 4 move names, nothing without
an objective, M1 carries it and seek does not), `GoalObjectiveLeakTest` (compass-withheld vs goal place).
Reviewer notes: `view.Detour` is the encounter's to measure (the travel encounter no longer exists; UNVERIFIED who sets
it
now, check `SightEncounter`).

## duel (id 3, revision 0)

Does: fighting the selected target and the seat's own combat state, consumables, hunters' stable. Size: obs `111 + 5*S`,
actions `10 + S`, with `S = 4` for hunters (`STABLE_SLOTS`) and 0 for other classes (`DuelBlock.cpp:138`; golden
constants
`duel.OBS_COUNT_WITHOUT_STABLE=111`, `ACTION_COUNT_WITHOUT_STABLE=10`, `STABLE_FEATURES=5`). No `DescribeColumns`.

Columns 0-110 (header enum `DuelBlock.h:44`): 0 distance (/60), 1-2 bearing sin/cos, 3 behind_target, 4
target_facing_bot,
5 target_in_combat, 6 target_attacks_bot, 7 target_casting, 8 bot_moving, 9 bot_in_combat, 10 bot_stealthed, 11
bot_auto_attacking, 12 damage_taken (`view.LastStepDamageTaken`), 13 pet_out, 14 pet_health, 15 pet_attacking, 16
combat_time, 17 cast_progress, 18 cast_remaining (/3 s), 19 shapeshifted (a cancellable form), 20-23 health potions,
mana potions, healthstones, bandages (carried / 5; healthstone / 1), 24 potion cooldown, 25 healthstone cooldown, 26
recently_bandaged, 27 soulstone_on_bot, 28 dead, 29 self_resurrect, 30 target_hidden, 31 target_unseen_time, 32-34
last-seen distance/bearing sin/cos, 35 in_line_of_sight, 36-42 target creature type one-hot (beast, dragonkin, demon,
elemental, giant, undead, humanoid; players humanoid), 43 target max health ratio / 4, 44 damage modifier / 2, 45 armour
reduction, 46 run speed rate / 2, 47 level difference / 5, 48-53 school immunities (holy, fire, nature, frost, shadow,
arcane), 54-59 mechanic immunities (fear, stun, root, snare, silence, polymorph), 60-64 bot stunned, feared, rooted,
silenced, snared, 65 target threat share, 66 hazards standing in (/3), 67 hazard way out (/20), 68-69 hazard bearing
sin/cos, 70-74 near hazard (present, edge/20, bearing sin/cos, radius/20), 75-78 debuff count/5, dispellable/5,
stacks/10, longest/30 s, 79-84 debuff mechanics, 85-110 the 26 `IncomingSpell` features of the target's cast. Then
`5 x S` stable columns (offered, family/50, ferocity, tenacity, cunning).

Actions: 0 start_attack, 1 pet_attack, 2 stop_casting, 3 cancel_form, 4 health_potion, 5 mana_potion, 6 healthstone, 7
bandage, 8 soulstone_self, 9 self_resurrect, then `call_beast_<k>` (hunters). Masks (`IsAllowed`): dead -> only
self_resurrect (needs `SelfResurrectAllowed`, a self-res spell, no prevention aura); stop_casting needs a cast;
cancel_form needs a cancellable form; consumables need the item and `CanUseItemOn`; call_beast needs the slot, no cast,
`CanCallHunterBeast`, no GCD; start_attack needs a living valid target not already the victim; pet_attack needs a
controlled creature not on the target.

Apply: self-res casts the spell; consumables use items directly (`UseItemOn`, `CastItemUseSpell`), NOT through client
packets even in sight stages; in a layout with the sight block start_attack goes as CMSG_ATTACKSWING, pet_attack as
CMSG_PET_ACTION, call_beast as Call Pet through the handler (`DuelBlock.cpp:423-471`). `BeforeApply` is empty.
`ObserveDead` writes only dead / self_resurrect and the mask for action 9 when the seat is dead.

Sight stages: a target the camera does not show is "hidden": position columns show only the entity memory's last-seen
place; `view.TargetInView` false. Hazard columns use `view.HazardsSeen` (camera-seen ground fire) in sight stages and
the
unit's auras otherwise.
Tests: `LiveLayoutPinTest` (names of the 10 fixed actions are pinned).
Reviewer notes: `OBS_BOT_MOVING` reads `bot->movespline->Finalized()`, which is always finalised for controller-moved
seats, so the column is effectively constant 0 (UNVERIFIED: confirm no code sets splines on seats). `OBS_BOT_STEALTHED`
uses two different aura checks in the hidden-target and target branches (`HasStealthAura` vs `HasAuraType`). The header
comments for `OBS_TARGET_THREAT_SHARE`/`OBS_NEAR_HAZARD` are misplaced. In non-sight stages the hazard-standing columns
read the aura list, which is server knowledge (principle 1 only holds for sight stages). Consumables bypass client
packets (principle 4).

## pack (id 4, revision 0)

Does: 24 enemy slots (`PACK_SLOTS`) and select-slot actions. Size 1034 obs (`2 + 24 x 43`), 25 actions. Declared by the
dungeon stages only. Globals: 0 living enemies / 4, 1 enemies in combat / 4. Per slot (43): 0 present, 1 alive, 2
health,
3 distance/60, 4-5 bearing sin/cos, 6 behind, 7 attacks_bot, 8 attacks_pet, 9 casting, 10 in_combat, 11 crowd
controlled, 12 current_target, 13 elite, 14 level difference/5, 15 in LOS, 16 threat share, 17-42 the 26 incoming-cast
features. Actions: 0-23 `target_slot_<k>`, 24 `hold_interrupt`. Mask: slot allowed when it exists, is alive, is not the
current target slot, and the seat is alive; hold_interrupt when the seat (or its pet) can interrupt, the target is alive
and the option is not running. Apply: select (`Encoding::SelectEnemy`) or start `HoldInterrupt` for
`Options.HoldInterruptMs`.
`view.Enemies` in sight stages is the camera's visible living hostiles (`CombatBlock::VisibleEnemies`), so the pack is
perception-true there. `DescribeSeatSets` exposes the pack as set "enemies" (slots 24, present column 0, segment
`first = block.ObsFirst + 2`, stride 43, pointer action range 24 from the block's first action). Hash
`a102ef2571ed2826`.
Tests: pin only. Reviewer notes: `SLOT_ATTACKS_PET` tests `victim->GetOwnerGUID() == bot` only; select goes through
`SelectEnemy` (server-side `SetSelection`, not the CMSG), unlike the sight block's select; `boss_faction_champions.cpp`
(a script) also calls `SelectEnemy`.

## gauntlet (id 5, revision 0)

Pull timing, food and drink. Size 11 obs, 3 actions. Columns: 0 pulls cleared/10, 1 pull_active, 2 quiet time (/20 s),
3 pull time (/60 s), 4 elite pull, 5 eating, 6 drinking, 7 food left, 8 drink left (item count / stocked), 9 pull
arrival
(/30 s), 10 next pull (/20 s). Actions 0 eat, 1 drink, 2 rest_until_ready. Mask: item present, alive, out of combat,
`movespline->Finalized()`, no regen aura already, `CanUseItemOn`; rest also needs not already running and health/mana
< 90%. Apply eat/drink: in sight stages CMSG_USE_ITEM through the handler, else `UseItemOn`; counts `FoodUsed/Failed`,
`DrinkUsed/Failed`. rest starts `RestUntilReady` for `Options.RestMaxMs` and `BeforeApply` repeats eat/drink until 90%
or combat. Hash `7edc6ead75d0551b`. Declared from combat3 on. Tests: pin only. Reviewer notes: `IsAllowed` dereferences
`view.Option` without a null check on the rest path (`GauntletBlock.cpp:57`); `movespline->Finalized()` is always true
for
controller seats, so it never blocks eating while running.

## pet (id 11, revision 0)

Pet bar for hunter, warlock, death knight, mage (`PetBlock::HasPet`); for any other class size is {0, 0}. Size otherwise
74 obs (`26 + 6 x 8`), 11 actions. Columns: 0 present, 1 alive, 2 health, 3 power, 4 target distance/60, 5 attacking
target, 6 casting, 7-9 react state one-hot (passive, defensive, aggressive), 10 following, 11 staying, 12-22 `PetKind`
one-hot (ferocity, tenacity, cunning, imp, voidwalker, succubus, felhunter, felguard, ghoul, water elemental, other), 23
temporary, 24 time left/60 s, 25 commandable, then 6 ability slots x 8 (present, on cooldown, interrupt, control,
dispel, threat, positive, damage). Abilities are the pet's castable non-passive spells, classified by effects, ordered
interrupt, control, dispel, threat, positive, damage and cut to 6, cached per tick per thread. Actions: 0-5
`pet_ability_<k>`, then `pet_passive`, `pet_defensive`, `pet_aggressive`, `pet_follow`, `pet_stay` (follow and stay
masked
while fighting). Orders are applied server-side by the same state changes `HandlePetActionHelper` makes (NOT as packets,
even in sight stages). Abilities cast through a fresh `Spell` after `CheckPetCast`. `BeforeApply` casts the pet's
interrupt when a held interrupt finds nothing in the core block. Tests: pin (name/id/revision only). Reviewer notes:
the mask builds and deletes a `Spell` per ability per observation; guardians with no `CharmInfo` mask all orders;
`DefaultStance` makes a passive pet defensive once per pet GUID (called from StageScenario).

## vision (id 20, revision 5)

Camera scalars (11) and the image (bytes beside the observation). Size 11 obs (`Vi::ObsCount`), 0 actions. Scalars
(`Vision/Camera.h:282`): yaw sin, yaw cos, pitch, zoom, boom, pivot height, underwater, airborne, yaw rate, pitch rate,
render width. The image is `Vi::BYTES_PER_PIXEL = 5` bytes per pixel on the canonical `Height x Width` (conf
`AnimusForge.Vision.*`); not a float column. `Observe` advances free look by the decision length, clears the seen list
and ray hits, gathers sight entities around the pivot, calls `Vi::Render`, fills `view.Seen`. Look head is not an action
of the block (separate head, `FreeLook::HEADS`). Manifest carries image, look and camera objects. Revision history
(`VisionBlock.h:47`): 1 first layout; 2 log distance channel; 3 image to bytes; 4 free look + mixed render sizes; 5
five bytes per pixel with class and entity slot. Declared by every live stage. Details in cpp-vision.md. Tests:
`VisionBlockTest`, `VisionTest`, `VisionProtocolTest`, others in cpp-vision.md. Reviewer notes: Observe mutates seat
camera state through const; correct only if each seat is observed once per decision (comment at `VisionBlock.cpp:127`).

## entities (id 21, revision 1)

The visible-entity set: 32 slots x 20 columns = 640, no actions. Inserted automatically after vision. Per slot
(`EntitiesBlock.h:49`): 0 present, 1 class (raw `Vision::Class`), 2 type (raw template entry), 3 object, 4 level/80, 5
level delta/10, 6 health, 7 reaction (-1,0,1), 8 quest, 9 lootable, 10 usable, 11 distance (log scaled NEAR..1000),
12-15
yaw sin/cos, pitch sin/cos, 16-17 centroid x,y, 18 pixel share, 19 entity memory id (0 without memory). Raw columns, not
normalised (kept out of the learner's adapters). `Observe` first advances and writes `view.Recall` (entity memory), then
writes. Manifest object "entities". Tests: `VisionEntitiesTest`, `SightBlockTest`, `CombatPerceptionTest`.
Reviewer notes: class and type are raw indices (the learner hashes type modulo `TYPE_BUCKETS 4096`).

## map (id 22, revision 1)

The mental map: 4 scalars + the 48x48x6 byte crop (`Vision::CROP`; separate STEP section). Scalars: known, frontier,
visited, kept (share of cells ever seen, frontier, visited, map kept from the previous episode). `Observe` advances the
map clock, writes the frame's rays (`view.Hits`) and the body, crops heading-up. Declared from move2 on. Tests:
`MentalMapTest`. Reviewer notes: uses `bot->GetPosition*` for the crop centre before the body override only when no
body exists; no actions.

## sight (id 23, revision 2)

Seen and remembered list and pointer presses. Size `64 x W + 23` obs where `W = 32` (move3) or `32 + 13 = 45` with the
combat block in the layout (`SightBlock::Width`), actions 321. Pinned: 2071 / 321 (no combat), 2903 / 321. Per slot: the
20
entity columns, then 12 memory columns: visible, age (log2 scaled over 3600 s), dead, open, used, heading sin/cos, speed
(/7, max 2), course sin/cos, selected, focused; then (with combat) 13 combat columns (visible units only). Slots 0-31
are the frame's visible entities in slot order, 32-63 the most relevant remembered ones. After the slots, the named row
(23 columns): 20 entity columns (present, class, type, object only) + 3 task one-hot (reach, interact, use_item), from
`view.NamedTask`. Actions: five groups of 64 (select, interact, use_item, assist, focus) then `clear_focus`: names
`select_<k>` ... `focus_<k>`, `clear_focus`. Mask: only slots with an entity; game objects cannot be selected, assisted
or
focused; clear_focus always. All presses are client packets through `EntityActions` (see layout doc). Spells in sight
stages are cast through the client too. No looting. Manifest object "sight" includes pointers, named row, memory ids
(`Vi::MEMORY_TRAINING_CAP`). Tests: `SightBlockTest`, `SightEncounterTest`, `InteractStageTest`, `CombatPerceptionTest`,
`DungeonStagesTest`. Reviewer notes: no `DescribeColumns` (names exist only in the manifest "features"); the width
depends on layout membership of combat, which changes the block's shape between move3 and combat1 (see seeding
question).

## party_frames (id 24, revision 2)

4 member slots x 21 features = 84 obs, 12 actions. Columns named `member<i>_<feature>`: present, alive, leader,
in_combat,
health, power, dot, dot_right, dot_forward, dot_distance, mana_user, in_range, debuffs, dispellable, aggro, selected,
focused, target, target_hostile, target_mine, target_in_view. Dots are minimap positions heading-up within
`PartyFollow.MinimapYards` (60); normalised by that radius. Actions `select_member<i>`, `focus_member<i>`,
`assist_member<i>` (12). Mask: frame present. `FillFromGroup` fills `view.Frames` from the core group (leader first,
own subgroup only); encounters without a core group fill their own. Revision history: 1 (M4: first 10 columns), 2 (G1:
combat block's member frames merged, presses added). Tests: `PartyFollowTest` (PartyFramesTest cases),
`CombatPerceptionTest`. Reviewer notes: the minimap dot reads the member's server position; a dot beyond the radius is
absent.

## combat (id 25, revision 1)

36 obs, 4 actions, plus 13 columns per sight slot. Columns (named by `DescribeColumns`): `frame_self_*` and
`frame_pet_*`
(present, alive, health, power, mana_user, in_range, in_combat, debuffs, dispellable, aggro, selected, focused), then
`target_*` (present, hostile, friendly, in_view, dead, threat, threat_pct, tot_self, tot_pet, tot_party, tot_other,
debuffs).
Actions: `select_frame_self`, `select_frame_pet`, `focus_frame_self`, `focus_frame_pet`. Per-slot columns: casting,
cast_left, interruptible, cast_heal, cast_area, cast_at_me, controlled, elite, in_combat, attacks_me, attacks_party,
threat,
debuffs (written by `CombatBlock::WriteSlot` from `SightBlock::Observe`). Threat status is the client's
`UnitThreatSituation` (0..3, /3) read from the server threat manager; debuffs shown only for the selection, the focus or
visible units. Also provides `VisibleEnemies` (the encounter enemy list), `ReadHazards` (ground fire from the frame) and
`InView`. Revision 1: member frames moved to party_frames. Tests: `CombatPerceptionTest` (18 cases). Reviewer notes: the
`FrameResolve` comment calls it a "party frame's click"; target-frame threat uses server threat lists as the client's
threat colouring does.

## goal (id 26, revision 0)

128 obs, 0 actions, always last. Layout of the 128: 0-11 kind available, 12-40 target available, 41 ended, 42 reached,
43
secondary_ended, 44 event, 45 from_order, 46-57 order kind, 58-86 order target, 87-98 achieved kind, 99-127 achieved
target. Columns 45-86 (from_order, order kind, order target) are never written by `Observe` (always zero); the
stage.json
`goals.columns` still reports them. `Available` offers Fight always, Control with >= 2 enemies, Recover/Rest when hurt,
Protect with a friend, Position with an enemy, Prepare out of combat, TravelTo with a place or objective; Loot, Gather,
Interact are never offered. `Status` evaluates reached/possible per kind; `Earned` is the "reached, not true at choice"
rule used by the scenario for `Goals.Reached` payment. `PlaceOf` yields the TravelTo target (the assignment slot is the
trip
objective when the stage has no route places, only while `ObjectivePlaceKnown`). Constants `PROTECT_REACHED_PCT 70`,
`PLACE_REACH 20`. Tests: `GoalObjectiveLeakTest`. Reviewer notes: the goal-target constants include objective, giver and
ender targets that nothing populates.

## Observed issues

- `Blocks/LayeredField.h/.cpp` are routing data, not a block; they sit in the wrong directory and include `MoveBlock.h`
  for one constant (`LayeredField.cpp:27`).
- `Layout/Block.h` comments reference deleted blocks (PartyBlock, CompanionBlock, CrowdBlock, HostilesBlock,
  SupportBlock) and `SeatView.h` references WorldBlock.
- Goal block order columns (45-86) are dead.
- `bootstrap.GROWS_AT_END = {"crowd"}` refers to a deleted block.
- `MovePrice::BearingSwing`/`Undone` only used by tests.
- Duel `OBS_BOT_MOVING` and gauntlet `movespline->Finalized()` read spline state that controller-moved seats never set.
- Core mask hides heal-on-full-health and refresh-with-plenty-left spells, and `SeatMemory` pacing masks repeats
  (`StageScenario.cpp:3711`); both conflict with principle 5 as worded (see layout doc).
- Only 2 of the 14 blocks' Observe paths have unit tests that do not need a world; Core, Duel, Pack, Gauntlet, Pet,
  Goal masks are untested.

## Questions for the owner

- Is the sight block restart between move3 and combat1 intended (width 2071 vs 2903)?
- Should pacing masks and the heal/refresh masks stay under principle 5?
