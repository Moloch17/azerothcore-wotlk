# The druid curriculum, end to end — and the removal of roles

## Context

Three things changed under the curriculum and it has not been re-cut for any of them.

**A seat now has legs of its own.** `MoveBlock` (animus-lib `4d9531b`) gives every seat eight egocentric bearings,
a halt and three facings, held from decision to decision, with facing chosen apart from the feet. Before it, every
movement in the curriculum was target-relative — `DuelBlock`'s `MOVE_TO_TARGET`, `MOVE_TO_RANGE`, `BACK_OFF`,
`KEEP_RANGE` all open with `if (!target || !target->IsAlive()) return false` — and `TravelBlock`'s
`ACTION_MOVE_TO_OBJECTIVE` was a pathfind-to-a-point order the policy issued once and watched. The point order goes:
a seat steers. Nothing in the curriculum trains that yet, and the stage list still opens with a fight.

**A class is one model** (`8313dc1`). The layout keys on the class alone; the seat draws a spec. The druid is the
class this matters most for: four specs across what are today three roles (balance, feral_cat, feral_bear,
restoration), a form for each, and Prowl — the only real stealth outside the rogue.

**And roles are to go.** A bot should be handed a character and know how to play it *from the character*: a
beastmaster hunter plays like a beastmaster hunter because of its talents and its spellbook, not because someone
wrote `Role::Dps` next to it. The director should pick who to give an order to by reading what a seat's build can
do, not by reading a label.

This plan covers both: the role removal, which is a prerequisite, and then the druid's curriculum end to end.
The druid goes first because it exercises every part of the machinery — the three things roles used to name, two
specs sharing one of them, forms, and stealth. What survives the druid generalises to the other nine.

Nothing on disk is worth seeding from (`animus-retrain.PLAN.md`: the revive exploit poisoned every checkpoint), so
renumbering and reshaping are free and this is a fresh tree.

---

# Part A — Removing roles

## Is it feasible? Yes, with one distinction that is the whole answer

**Nothing may be *declared*. Things still have to be *measured*.**

Today `Role` is a hand-written label on a spec (`ClassProfile.cpp:126-130`) that does four separate jobs, and they
have very different answers:

| Job | Where | Verdict |
|---|---|---|
| **Telling the seat what it is** | `CoreBlock::OBS_ROLE_FIRST`, a 3-wide one-hot | **Delete.** Strictly replaceable, and by something better |
| **Telling a seat about others** | teammate `PlayRole`, `OwnerRole`, `OpponentRole` | **Delete.** "That one is a healer" is less than "that one knows these heals" |
| **Deciding who gets spawned** | `StageScenario.cpp:176-180,1348`, `DrawLayout`, `DrawSpec` | **Replace, do not delete.** A party with no one who can hold threat is a wasted episode, not a lesson |
| **Measuring whether it worked** | `target.role_metrics`, `DifficultyLadder` keyed on (layout, role) | **Replace with the build**, which is finer-grained anyway |

So the answer to "is it feasible" is yes, and the answer to "can nothing decide composition" is no — but the thing
that decides composition does not have to be a label someone wrote down. It can be computed from the character.

## Aptitude: what a build can do, read off the build

A new `Character/Aptitude.{h,cpp}`, built beside `ClassAssets` from what is already in hand — the seat's
`ActionCatalog`, its `TalentBuilder` ranks and its gear. Roughly twenty-two numbers in `[0,1]`, none of them a label:

| Feature | Read from |
|---|---|
| `taunt` | catalog holds a taunt (Growl, Taunt, Dark Command, Righteous Defense, Hand of Reckoning) |
| `mitigation` | a damage-reduction stance/form/aura/presence, a shield equipped, armour against level |
| `threat` | threat-multiplying abilities and talents present in the build |
| `direct_heal`, `hot_heal`, `area_heal` | share and power of the catalog's `Sustain` group, split by kind |
| `melee_damage`, `spell_damage`, `ranged_damage` | where the build's damage actions come from |
| `control`, `interrupt` | size of the `Tactical` group, and whether it holds an interrupt |
| `buff` | distinct party-wide long buffs it brings — `Action.LongBuff` already exists (`ActionCatalog.cpp:388`) |
| `dispel_friendly`, `cleanse[4]` | it can remove something from an ally, and which of magic / curse / disease / poison |
| `dispel_offensive` | purge, Spellsteal, offensive dispel |
| `protect_other` | it can spend a cooldown on someone else (Pain Suppression, Hand of Protection, Guardian Spirit) |
| `revive`, `battle_revive` | `ActionCatalog::Revives()`, and whether one is castable in combat |
| `pet` | the build summons and keeps one |
| `tree_points[3]` | points per talent tree / 71 |

Every one is a fact about the character. None is a decision about how to play it — a bear-form druid with heals
scores on both, and what to do about that is the policy's problem, which is the point.

**This also answers the off-template case directly.** `Characters.RandomTalentChance` and `NoisyTalentChance`
already put builds into training that no template describes, and today they get whatever `Role` their template
carried, which is a lie. Aptitude reads the build it actually has — so a player who makes a beastmaster hunter, or
something stranger, gets a policy that has seen builds scored the way theirs will be.

### Buffs and dispels need their own features — and dispels need catalog work first

Buffs are half-built already: `Action.LongBuff` is a per-action flag, `buff_coverage` is an episode column and
`Support.BuffCoverage 0.3` is a reward term. The aptitude feature is a count off the flag; nothing else is needed.

**Dispels are not built at all, and the gap is one-sided.** The *demand* side is observed — `DuelBlock`'s
`OBS_DEBUFF_DISPELLABLE` (line 123) tells a seat how many of its target's debuffs could be removed, and
`IncomingSpell::FEATURE_DISPELLABLE` says a cast will leave something removable. But on the *supply* side,
`SPELL_EFFECT_DISPEL` is simply swept into `Tactical` (`ActionCatalog.cpp:651`) or `Sustain` (line 711) with no
flag of its own, so nothing anywhere knows that a seat can dispel, let alone *what* it can dispel. A priest that
can remove a curse and one that cannot look identical.

So: add `Action.Dispel` and the spell's dispel mask (`SpellInfo::Dispel` → magic / curse / disease / poison) to
`ActionCatalog`, then the six aptitude features above. This is the single clearest case for the whole approach —
"who can decurse this" is exactly the question the director has to answer about a seat, and it is unanswerable
from a role label no matter how the roles are drawn.

## The seven changes

1. **`CoreBlock`** — `OBS_ROLE_FIRST` (3) → `OBS_APTITUDE_FIRST` (~22). `OBS_GLOBAL_COUNT` 72 → ~91.
2. **`PartyBlock`, `CompanionBlock`, `PvpBlock`** — teammate/owner/opponent `PlayRole` → a compressed six-feature
   aptitude (`tank`, `heal`, `melee`, `spell`, `control`, `pet`) per slot. `PARTY_MEMBERS` is 7, so this is +42 on
   the party block, not +105.
3. **`DirectorLayout`** — `SEAT_FEATURES` gains the same six. **This is the user's ask, exactly**: the director sees
   what each seat's build can do and learns which orders that build answers. It is also strictly more than it has
   today, where every dps seat looks identical to it.
4. **Composition** — `RollRole(Party.RoleTankChance, RoleHealerChance)` → `DrawSeatFor(aptitude demand)`. A party
   still asks for one seat with high `taunt`+`mitigation` and one with high `direct_heal`; it asks the aptitude
   rather than a table, so **every class is eligible for every slot its build qualifies for** — which is what
   "train every class for every role" means once nobody writes the roles down.
5. **`ArenaDefinition::SeatRoles`** → `SeatAptitudes`: "seat 0 is drawn from builds scoring above X on
   `mitigation`". The tanking and triage drills survive unchanged in spirit.
6. **Gates** — `target.role_metrics` → `target.spec_metrics`, keyed on the `spec` episode-info column, which
   already exists. `spec` is the name of a *talent template*, not a role, and it is finer: it separates feral_cat
   from balance, which `role_metrics` cannot. `DifficultyLadder`'s `_tiers` re-key from (layout, role) to
   (layout, spec).
7. **`Baselines.cpp:43`** — `RoleOf` for the hold-range decision → the `melee_damage` / `ranged_damage` aptitudes.

`SpecProfile::PlayRole` and `enum Role` are then deleted and the compiler finds the rest (~150 sites, 25 files).
`GearBuilder`'s `StatProfile` (`SP::Tank`, `SP::Healer`, …) **stays**: it is a stat priority attached to a build,
not a role, and it is only ever read when generating gear.

## What it costs, honestly

- **~150 call sites across 25 files.** Mechanical once `Aptitude` exists, but it touches every encounter.
- **The role one-hot is a learning aid, and removing it is not free.** Three bits that said "you are a healer" are
  gone. The aptitude vector is the replacement and is a better one — it is the *inference* the policy would have
  had to make from 60+ talent features, precomputed and handed over — but the first duel run is where this shows
  up or does not. Watch `spec_metrics` on `restoration` and `feral_bear` at the first evaluation; those are the
  builds that leaned hardest on the label.
- **Observation grows ~19 on core plus ~42 on the party block.** Nothing next to a 690-wide druid layout.
- **Protocol and format bump.** The manifest is at format 4; this changes every layout's shape, so it is format 5
  and `PROTOCOL_VERSION` moves with it. Every checkpoint is invalidated — which costs nothing, because the retrain
  plan already discards all of them.

**Do this before the druid curriculum, not after.** Otherwise every stage gate is written against `role_metrics`
and rewritten a week later.

---

# Part B — The druid curriculum

## The shape: per-class runs, shared stage definitions

A per-class curriculum needs no new stage definitions. It is three config keys:

```
AnimusForge.Classes   = "druid"
AnimusForge.OutputDir = "var/animus-forge/druid"
AnimusForge.Queue     = "stage1_move, stage2_dodge, ..."
```

`OutputDir` already namespaces `runs/`, `layouts/` and `models/`, so druid and warrior runs of the same stage name
never collide. The stage list in `Stages.cpp` stays class-neutral; what is druid-specific is the **queue**, the
**gates** and the **budgets**, all of which are already per-run.

Do **not** add `druid_stage*` definitions. A class-specific *definition* is only warranted where the situation
itself differs, and none does — a druid's Flight Form and a mage's flying mount meet the same
`ArenaDefinition::Flying` arena, and the class kit supplies the difference.

Consequence to accept: a druid-only run trains a druid-only trunk, and its party and arena stages are druids
against druids. Both are fixed at the join (Part C).

## Part 0 — prerequisites

### 0a. Retire `TravelBlock::ACTION_MOVE_TO_OBJECTIVE`

`Blocks/TravelBlock.{h,cpp}` — delete the action (`TravelBlock.h:62`, `TravelBlock.cpp:120,310,326`). `TravelBlock`
keeps mount, dismount, ascend and descend: it becomes *the mount and the air*, `MoveBlock` becomes *the feet*.
Strip it outright rather than gating it off (forge-fork convention).

**What this breaks, and must be fixed in the same change:** the scripted baseline steers with it
(`Baselines.cpp:501`). Rewrite that branch to pick the bearing nearest the objective —
`TravelBlock::OBS_OBJECTIVE_BEARING_SIN/COS` already give it, egocentric. A baseline that cannot cross ground makes
`min_over_baseline` a lie on four stages.

Leave alone: `DuelBlock`'s target-relative movement (`MoveBlock.h:53` says so explicitly), and the
`Encoding::MoveTo` follow splines in `CompanionBlock.cpp:148` and `PartyBlock.cpp:209` — those are follow *options*,
not a point the policy named.

### 0b. Drop the hazard drill's emitter-as-target hack

`HazardEncounter.cpp:54-61,151-154` hands the seats an immune, unattackable emitter as their **target**, purely so
the target-relative movement actions would unmask. `MoveBlock` is what that was standing in for. Keep the emitter as
the thing that lays the fire; stop handing it over as a target.

### 0c. Per-class learner configs

`ForgeConfig.cpp:354` builds `configs/<scenario>.yaml`. Make it try `configs/<class>/<scenario>.yaml` first when
`AnimusForge.Classes` names exactly one class. (Not `--overlay`: an overlay merges section by section, so one
druid overlay would replace the `target:` of every stage in the queue with the same floors, and stage 1 gates
`arrived` while stage 5 gates `clean_kill`.)

### 0d. Fine orientation: the seat needs a mouse-look, not more bearings

**Eight bearings do snap, and the reason is three lines.** `MoveBlock.cpp:55-64`:

```cpp
if (mode == ACTION_FACE_TARGET && target) init.SetFacing(target);      // continuous, but tied to a target
else if (mode == ACTION_FACE_HEADING)     init.SetFacing(heading);     // orientation := the bearing's heading
else                                      init.SetFacing(bot->GetOrientation());   // unchanged
```

`FACE_HEADING` sets orientation *to the bearing it just walked*, so successive bearings accumulate on a 45° lattice;
`FACE_HOLD` leaves orientation fixed, so the eight bearings stay a fixed 45° grid. **With no target in sight — which
is every one of the Part I stages — the reachable set of world-frame headings is a 45° lattice anchored to wherever
the seat happened to be facing.** To hold a true heading between two lattice points the policy has to alternate
bearings, which is a visible zig-zag and costs `saved` on exactly the stages this plan puts first.

**More bearings is the wrong fix.** Sixteen halves the error to 22.5° and still lattices, at double the exploration
cost on the most-pressed actions in the catalog. The real diagnosis is that a WoW player's feet *are* eight
directions — WASD and the diagonals — and what our seat is missing is not more keys but the **mouse-look**: a way
to turn continuously, independent of where the feet are going.

Three actions, `MoveBlock` 12 → 15:

| Action | Behaviour |
|---|---|
| `turn_left`, `turn_right` | Durative, like a bearing: held until something else is chosen. ~45° per decision = 180°/s, the game's own keyboard turn rate. With `face_hold` this reaches any orientation. |
| `face_objective` | Point at the travel objective, or the place the director called. Then `BEARING_FORWARD` is the exact heading, continuously — travel snapping goes away outright. |

**The machinery is already right for this and needs no further change.** `BeforeApply` recomputes
`HeadingOf(bot->GetOrientation(), view.HeldBearing)` *every decision the bearing is held* (`MoveBlock.cpp:176`), so
a held bearing under a held turn curves smoothly rather than stepping — the path bends with the turn exactly the
way it already bends with the ground. Turning is also durative, so it takes the movement repeat pacing and is not
charged for repeating; it should report `IsMovement` for that reason, and `MoveDirection` 0 like the bearings.

Add `OBS_OBJECTIVE_BEARING_SIN/COS` to `MoveBlock`'s own observations as well. `TravelBlock` has them, but
`MoveBlock`'s whole design principle is that it needs no other block to be useful (`MoveBlock.h:33`), and two
features is not a price.

### 0e. Terrain: the seat steers blind today, and water is excluded on purpose

None of water, damaging liquid or obstacles is trained. Worse, the first is an *explicit exclusion*:

- **Water.** `TravelEncounter.cpp:149` rejects any candidate destination with
  `map->IsInWater(phaseMask, x, y, z, BODY_HEIGHT)`. The travel stages deliberately never send a seat near water.
  And `IsInWater` appears **nowhere else in the curriculum**: no swimming observation, no breath or fatigue, no
  liquid status. A seat that ends up in water does not know it is in water.
- **Lava, slime and other damaging fluids.** Nothing, anywhere. `HazardEncounter` is *spell-cast* ground effects
  laid by an emitter — dynamic objects, an entirely different mechanism from map liquid — so the dodge stage
  teaches nothing about a lava river.
- **Obstacles.** Handled by the **pathfinder**, not by the policy. `Encoding::MoveTo` routes with pathfinding on
  (`MoveBlock.cpp:182`: *"a bearing is where the seat wants to go, not a licence to walk through a wall"*), and
  `FindPlace` returns a path-aware walk distance. But `MoveBlock`'s twenty observations are moving, speed, bearing
  held, facing, target bearing/distance and hazard bearing/distance/radius — **nothing about what is in front of
  it.** The seat holds a bearing into a cliff and learns only that `walk_distance` grew.

That last one is not a missing feature, it is a contradiction of this plan's premise. If the seat steers blind and
the pathfinder quietly bends every route, `move_to_location` is back through the side door — retired from the
action space and doing the same job one layer down. **The terrain probe is therefore core, not optional.**

| | What | Cost |
|---|---|---|
| **Core** | **Terrain probe in `MoveBlock`**: `map->GetHeight` along each of the eight bearings at a fixed reach, as normalised walkable distance, plus the step height of the ground ahead. +9 obs. | `SnapToGround` (`EncoderSupport.cpp:675-687`) is *one* `GetHeight` call and `MoveBlock::BeforeApply` already makes it every decision per seat, so this is eight times a cost already paid. `FindCover`'s 36 `GetHeight` plus 36 line-of-sight checks is the precedent for the heavier version. Measure it against the 24 ms decision budget before committing the reach. |
| **Core, and free** | **An obstacle arena.** No new machinery: `MapId` + `SpawnPoints` already let a stage choose its ground, and stages 16-18 use map 560 for exactly this reason — *"an approach needs something to come round"*. Broken terrain plus the probe is an obstacle course. | Spawn points only. |
| **Core — see 0f** | **Water**, as a swimming lesson and a judgement about whether to get in at all. | Its own section below. |
| **Defer** | **Damaging liquid.** `LiquidStatus` gives the type (water / ocean / magma / slime), so "cross without stepping in the magma" is expressible as an arena beside the dodge drill. Genuinely new content rather than a gap being closed. | A stage's worth. |

The obstacle work belongs in Part I as an arena of `stage1_move` rather than a stage of its own — it is the same
lesson (read the ground, steer) on harder ground.

### 0f. Water: pitch is the missing axis, and "worth it" is a scenario problem

Two separate things are needed, and they fail for different reasons.

#### Swimming in three dimensions needs a pitch axis

`MoveBlock` is flat. `HeadingOf(orientation, bearing)` is a heading in the XY plane, the destination is
`x + STEP·cos h, y + STEP·sin h`, and then `SnapToGround` forces z onto the ground (`MoveBlock.cpp:179-184`). There
is no way for the policy to express "down". The only vertical primitives in the whole curriculum are
`TravelBlock::ACTION_ASCEND` / `ACTION_DESCEND`, which are coarse one-shot 15-yard hops (`CLIMB_STEP`) issued
through `FlyTo` and usable only while flying.

**Add pitch, exactly as 0d added yaw.** `pitch_up`, `pitch_down`, `pitch_level` — durative, ~15° per decision,
clamped to ±60°. Together with 0d the steering becomes genuinely three-dimensional: **yaw from the turn keys,
pitch from these, and the eight bearings as the egocentric offset**, which is precisely how a player swims and
flies — look where you are going, hold forward.

`BeforeApply` gains one branch: when swimming or flying, skip `SnapToGround`, take the destination as
`pos + STEP · (cos p · cos h, cos p · sin h, sin p)`, and issue it on the three-dimensional spline path (`FlyTo`'s)
rather than `MovePoint` plus a ground snap. Mask pitch on dry land, where the ground decides z.

**This also fixes flight, and retires two more actions.** `ASCEND` and `DESCEND` are what caused the altitude
ratchet recorded in `TravelBlock.cpp:263` — *"a seat that drifted up stayed up through every move after it and
could only come down by choosing DESCEND often enough"*. A held pitch is how a player actually flies. Once pitch
exists they should retire alongside `MOVE_TO_OBJECTIVE`, for the same reason and in the same commit family.

#### "Worth getting into" has to be made a real choice before it can be learned

This is a scenario problem, not an action-space one. The tradeoff is quantitative and already measurable:

- **Swimming is about 4.7 yd/s against 7 running**, so crossing water pays only when the straight line saves more
  than roughly a third of the distance. That is a genuine threshold a policy can find.
- **Both numbers are already in hand.** `FindPlace` returns a *path-aware* `walk` distance next to the
  straight-line `StartDistance` (`TravelEncounter.cpp:189-194`), so the dry detour and the direct crossing are
  both known at reset. A water arena picks objectives where `walk / straight` is large — water between start and
  goal — and `saved` scores which route the seat chose.
- **So invert the exclusion rather than deleting it.** `TravelEncounter.cpp:149`'s `IsInWater` rejection stops
  being "never near water" and becomes a per-arena statement of what ground an objective may sit on.

#### The risk half, or water is a free shortcut

Without a cost, "worth it" is always yes. Breath underwater and fatigue in deep water are the cost, and they need
observing: **in water, submerged, breath remaining, fatigue remaining, current swim speed** (~5 obs, in `MoveBlock`
beside the terrain probe).

And the answer is **class-specific**, which is the shape of this whole plan: aptitude gains a `water` feature for
whether the build can breathe in it, move faster in it, or walk on it — druid Aquatic Form at **level 16**,
shaman Water Walking, priest Levitate, warlock Unending Breath, death knight Path of Frost. A druid's answer to
"is this water worth crossing" is simply not the same as a warrior's, and after 0f it has the observations to know
that.

#### Where it is trained

`stage1_move` gets the water arena (cross it, with a dry route available and sometimes better). `stage4_flight`
uses pitch instead of `ASCEND`/`DESCEND`. And `stage10_evade` gets water as an **escape**: breaking melee contact
by swimming is a real tool, and a druid leaving a fight in Aquatic Form is close to the strongest version of it in
the game.

### 0g. Part A — roles out, aptitude in.

## The ladder

Every gate below is a **first-pass floor**: conservative, because a failed gate halts the queue, so it asks "did
this stage learn its lesson at all", not "is it good". Raise them once a full pass gives real numbers. Every share
gated with `confidence: 0.95` is judged on its one-sided Wilson bound, so thin evidence fails rather than passing
on luck. All metric names are real columns (verified against the `info.Add(...)` set) or derived summary fields
(`clean_kill`, `livelocked`).

**The tree is linear, 1 → 17.** Every stage extends the one before it. The retrain plan's warning is the reason:
*"Phase 5 is a dead end — nothing extends or merges it, so it is discarded unless the export is downstream."* A
linear chain means the class curriculum ends in **one leaf** (`stage17_triage`) whose checkpoint carries every block
the druid learned, and the join has one thing to take per class instead of a per-class merge stage.

`Travel` stays in all four Part I stages rather than being dropped at stage 2 and re-added at stage 3: block-wise
seeding starts a re-added block *fresh*, which would throw away the `OBS_OBJECTIVE_BEARING` adapter columns that are
the entire lesson of stage 1. Sixteen masked observations is nothing.

### Part I — Feet (1-4)

Nothing here fights. Every stage still carries the `Duel` block — `Stages.cpp:643` requires it of every stage —
with its actions masked for want of a target, exactly as the hazard drill does today.

| # | Stage | Extends | Blocks | Situation |
|---|---|---|---|---|
| 1 | `stage1_move` | — (root) | Core, Move, Travel, Duel | `Opposition::Travel`, `OnFoot`, 40-160 yd, 120 s — three arenas: **open** ground, **broken** ground (map 560 spawns), and **water** (0f) |
| 2 | `stage2_dodge` | 1 | Core, Move, Travel, Duel | `Opposition::Hazards`, no target (0b), `MaxRung` pinned |
| 3 | `stage3_travel` | 2 | Core, Move, Travel, Duel | 60-320 yd by path, mount allowed, `MinLevel` 20 |
| 4 | `stage4_flight` | 3 | Core, Move, Travel, Duel | Nagrand, 350-700 yd, `Flying`, `MinLevel` 60 |

**1 — `stage1_move`. The root, from scratch.** Hold a bearing, halt, face apart from the feet, and use what the
class has before it can ride: for the druid, Travel Form (30), Dash in Cat Form (20+), Feral Swiftness. Mounting is
masked, not merely unpaid, so the lesson stays clean. This is the root every later stage seeds from, which is the
argument for putting it first: where a seat puts its feet is not something only some stages are about.

```yaml
target:
  min_over_baseline: 0.0          # needs 0a; drop this line if the baseline rewrite slips
  metrics:
    arrived:   {min: 0.95, confidence: 0.95}
    saved:     {min: 0.05}
    timed_out: {max: 0.05}
  spec_metrics:                   # a resto druid must cross ground as well as a cat
    balance:     {arrived: {min: 0.90}}
    feral_cat:   {arrived: {min: 0.90}}
    feral_bear:  {arrived: {min: 0.90}}
    restoration: {arrived: {min: 0.90}}
  until_passed: true
```

**Watch `allowed_actions` and `entropy` at the first evaluation.** The hazard drill's failure mode — 1.00 allowed,
entropy 0, seats standing still — is the one this stage can reproduce if the objective bearing does not reach the
policy. It does: `TravelBlock::OBS_OBJECTIVE_BEARING_SIN/COS/DISTANCE` are egocentric and in the layout.

**2 — `stage2_dodge`.** Fire lands underfoot and stays; nothing to fight and nowhere to be. With the emitter no
longer a target this is finally the drill it was written as. `MaxRung` stays pinned so the fire and everything
around it do not move together.

```yaml
target:
  metrics:
    hazard_seconds: {max: 4.0}
    hazard_damage:  {max: 0.10}   # a share of max health (StageScenario.cpp:938-942), so this reads right
  until_passed: true
```

`hazard_patches` is a property of the drill, not the policy (`HazardEncounter.cpp:95` counts what was placed) —
report it as a sanity check that the seats met enough fire for the rest to mean anything; do not gate it.

**3 — `stage3_travel`.** Mount when the trip is long enough to pay, ride it, dismount to fight. The druid's own
question: Travel Form is instant and free where a mount is a cast, so `saved` should clear the mount's, not merely
beat walking.

```yaml
target:
  metrics:
    arrived: {min: 0.95, confidence: 0.95}
    saved:   {min: 0.15}
  # mounted_fraction is reported, never gated: a druid that beats the number in Travel Form is right.
  until_passed: true
```

**4 — `stage4_flight`.** `MinLevel` 60; the druid's Flight Form is 68 and Swift Flight Form 77, so a good share of
this stage's characters have to ride instead, which is the honest distribution.

```yaml
target:
  metrics:
    arrived: {min: 0.90, confidence: 0.95}
    flew:    {min: 0.80}          # adoption decayed unwatched in an earlier run; this is the gate that catches it
  until_passed: true
```

### Part II — Fighting, solo (5-8)

**5 — `stage5_duel`, extending `stage4_flight`.** The main line runs *through* the movement block rather than beside
it, so the combat root inherits legs. Blocks: Core, Move, Travel, Duel, Pet. This is where the druid's 91-action
catalog lives and where all four builds are drawn. The existing `stage1_duel` gates are the only properly-designed
ones in the tree — keep them, and let `spec_metrics` do what `role_metrics` could not: separate feral_cat from
balance, which share a role today and would let one carry the other.

```yaml
target:
  min_layout_over_baseline: 0.0
  noise_z: 1.0
  metrics:
    livelocked: {max: 0.01}
  difficulties:
    0: {metrics: {clean_kill: {min: 0.95, confidence: 0.95}}}
  spec_metrics:
    balance:     {clean_kill: {min: 0.92, confidence: 0.95}}
    feral_cat:   {clean_kill: {min: 0.92, confidence: 0.95}}
    feral_bear:  {clean_kill: {min: 0.90, confidence: 0.95}}  # a bear kills slowly; the clock is the risk
    restoration: {clean_kill: {min: 0.85, confidence: 0.95}}  # the weakest case, and it must still pass
  confirm_episodes: 4096
  until_passed: true
```

**6 — `stage6_pack`.** Two to four, casters included, usually linked: targets, interrupts, what it cannot tank. The
druid's answer is Entangling Roots, Hibernate, Cyclone, Faerie Fire, and Bash / Maim in bear and cat.

```yaml
target:
  metrics:
    pulls_cleared:   {min: 0.90}
    died:            {max: 0.10}
    interrupts:      {min: 0.5}    # a per-episode count, not a rate (OpponentEncounter.cpp:102)
    control_seconds: {min: 3.0}
```

> Worth adding next to `clean_kill` in `config.py`'s `DERIVED_METRICS`: `interrupt_rate` =
> `interrupts / interruptible_casts_seen`. Both columns exist; the count alone rises with pack size and says
> less than it looks like it does.

**7 — `stage7_gauntlet`.** Pull after pull with short breaks: heal, eat, drink, arrive at the next pull ready. The
druid stage where shifting *out* of form to heal is the lesson, and Innervate is the mana answer.

```yaml
target:
  metrics:
    pulls_cleared: {min: 0.85}
    died:          {max: 0.15}
    engage_health: {min: 0.85}    # readiness: it has to arrive at the next pull whole
    engage_mana:   {min: 0.60}
```

**8 — `stage8_endurance`.** A known run of eight in a fixed order, won by finishing it: what to spend and what to
keep. Gate `pulls_cleared` against the run length. Per the standing rule, resting is never rewarded directly —
`rest_seconds` is reported, never gated.

### Part III — Fighting people, solo (9-13)

| # | Stage | Extends | Gates |
|---|---|---|---|
| 9 | `stage9_pvp` | 8 | `won {min: 0.55, confidence: 0.95}`, `interrupts`, `died` |
| 10 | `stage10_evade` | 9 | `escaped {min: 0.60, confidence: 0.95}`, `line_of_sight_breaks {min: 1}` |
| 11 | `stage11_hide` | 10 | `unseen_seconds`, `re_hides {min: 1}` |
| 12 | `stage12_stealth` | 11 | `stealth_openers {min: 0.8}`, `stalked_into_range {min: 0.7}`, `won {min: 0.55, confidence: 0.95}` |
| 13 | `stage13_arena` | 12 | `min_over_baseline`, `time_to_kill`, `health_left` — **never `won`** |

**10 — `stage10_evade`** is the druid's best case in the game: Travel Form, Dash, Roots-and-run, Barkskin, against
an `OpponentLevelBonus` fight it cannot win.

**12 — `stage12_stealth` — and here the druid stops being a leaf.** `NeedsStealth` forces a leaf today
(`Stages.cpp:618-631`) because a restricted checkpoint holds only four of eighteen layouts, and `init_from: auto`
would find it and start the rest from random weights in silence. **In a druid-only run every configured class can
stealth, so the restriction is vacuous and the leaf rule is wrong.**

> Two ways to fix it, and the second is better. (i) Pass the configured classes into stage validation so
> `restricted()` asks whether every configured class fails `CanStealth` (`StageScenario.cpp:96`) — but that makes
> the static `CurriculumStages()` config-dependent, and it is called from `FindStage`, the queue and
> `forge scenarios`. (ii) **Drop the static leaf rule entirely and make `bootstrap.py` refuse**, loudly, a layout
> the checkpoint lacks, instead of silently fresh-initialising it. That enforces the real hazard at the one place
> the actual layouts are known. Recommend (ii).

Without this the druid's Prowl dies at stage 12 and never reaches the arena or the flag — which is most of the
reason to run the druid first. The lesson is genuinely druid-shaped and two casts long: Cat Form, then Prowl, a
global cooldown apart (`ScriptedPlayer.cpp:428,532` already models exactly this for the scripted side), then close
unseen and open with Pounce or Ravage, six levels down, so the opener has to decide the fight.

**13 — `stage13_arena`. Do not gate on `won`**: self-play makes it 0.5 by construction.

### Part IV — The group, before the director (14-17)

The judgment call worth stating plainly: **these belong in the class curriculum, not in group training.** A bear
cannot learn to hold threat alone, and a resto druid cannot learn triage with nobody to heal. They are undirected —
no director, no orders channel — so they are combat, not command. A druid-only run fills them with druids, and with
aptitude-driven composition that is a real party: the high-`mitigation` seat and the high-`direct_heal` seat are
drawn because their builds qualify, not because anyone wrote "tank" anywhere.

| # | Stage | Extends | Seat plan | Gates |
|---|---|---|---|---|
| 14 | `stage14_companion` | 13 | Solo + scripted owner | `owner_deaths {max: 0.10}`, `pulls_cleared {min: 0.85}` |
| 15 | `stage15_party` | 14 | `Party` | `pulls_cleared {min: 0.85}`, `wipes {max: 0.10}`, `teammates_died` |
| 16 | `stage16_tanking` | 15 | `Party`, `SeatAptitudes` pin `mitigation` | `spec_metrics.feral_bear`: `threat_share {min: 0.60}`, `threat_on_teammates {max: 0.25}` |
| 17 | `stage17_triage` | 16 | `Party`, `SeatAptitudes` pin `direct_heal` | `spec_metrics.restoration`: `low_health_seconds {max: 5}`, `overheal_share {max: 0.35}`, `healing_per_mana` |

Raid stages stay in the list but out of the druid build's critical path: they teach nothing a druid needs that the
party did not, and they cost forty seats an env.

---

# Part C — The join, and the director

`stage18_flag` (mirror CTF, merging `stage3_travel`), `stage19_warsong`, the crossroads join and the directed team
stages are **not** part of the druid's individual curriculum. They begin after the join, where the druid meets the
other nine classes and the director. This is where the aptitude work pays: the director reads each seat's build
and learns which orders it answers — a seat is chosen for a duty because it can do it, never because it was
labelled.

## The director drafts its own team

Feasible, and it is the natural end of the aptitude work: once a seat is described by what its build can do, picking
a team is the same question as picking who to give an order to, asked before the episode instead of during it.

**A draft phase at reset.** The env generates a pool of `P` candidate characters (16 is a reasonable pool for a
duo or a trio) but **describes them without building them** — class, level, talent plan and the aptitude vector
are all computable from the plan, and `GearBuilder` only runs for the ones actually picked. Each director then
makes `TeamSeats` picks, one per decision, from a shrinking pool, and the episode starts. That is `P` more director
actions and `P × (aptitude + class + level)` more observations on a layout that is already 257 wide and entirely
class-free, so it fits the existing shape.

**Self-play is what makes the signal clean.** Both directors draft from the *same* pool, alternating picks, so a
bad drafter loses to a good one on the same characters — unlike the order-quality problem, where a learned
director scores against its own side's rewards and has never separated from chance.

**The caveat, stated plainly.** The learned director has not beaten the scripted caller on call quality in four
30M runs (`animus-retrain.PLAN.md`), and drafting adds a second, sparser credit-assignment problem — ten picks
paying off once, at the end of an episode — to an agent that has not yet solved the first. So **isolate it**: a
draft stage where drafting is the *only* thing the director learns and the orders are scripted, exactly as the
existing advice is to run `stage20_duo_led` with `DirectorLearned` off first. If drafting beats a random draft from
the same pool, it is real; if it does not, nothing downstream has been built on it.

This comes after the join, not before: a director drafting from a pool of druids is choosing between four talent
templates, which is not the lesson.

### Choosing talents: yes, as a tree budget — the primitive already exists

Not talent by talent. 71 points across a class's own tree layout is a decision sequence no class-free action head
can express, and giving the director a per-class head throws away the property that makes one director serve every
composition (`DirectorLayout` is 257 obs / 42 actions and entirely class-free).

What works instead is **the split across the three trees**, which is class-free by construction — every class has
exactly three — and is also where the interesting choice actually lives. "How deep into feral before going back for
Nature's Swiftness" is a real decision; "rank 3 or rank 4 of Ferocity" is not.

`TalentBuilder` already has the primitive: `Random(uint8 specTab, uint32 points)` spends a budget inside one tree,
row-weighted so the build walks down far enough to reach its last row rather than filling cheap rows first, and
`Build::TreePoints[3]` already records the result. So the director picks one of ~8 coarse splits (51/20/0,
51/0/20, 41/30/0, 31/20/20, …; `SPEC_TREE_POINTS = 51` is what a last row needs) and the existing filler does the
rest. Eight actions, no per-class head, and it reaches genuine hybrids that no template in `SpecBuilds.cpp`
describes.

### Choosing gear: no — it is a function of the build, not a second decision

`GearBuilder::Equip(Player*, SpecProfile const& spec, bool pvp)` keys entirely on the spec's `StatProfile`, so a
director "choosing gear" means choosing one of five stat profiles. It is cheap to wire and there is nothing in it
to learn: a bear in caster gear is simply worse, so the director would converge in a few thousand episodes on
"match the gear to the talents" and the decision would be dead weight in the action space forever after.

And once roles are gone the point is stronger. `StatProfile` has to be derived from the build's tree points anyway
— that is the same change as everything else in Part A — at which point **gear follows from the talents
automatically** and there is no second choice left to make. Let it.

### The guard this needs, and why

There is a real argument that build-choosing is the *wrong objective*, and it should be bounded rather than
refused:

1. **At serve time the build is given.** A player makes a beastmaster hunter and the bot plays it. A director that
   chooses builds is optimising a problem that does not exist where the models ship.
2. **It narrows the training distribution.** A director rewarded on its side's performance converges on the
   strongest compositions in the game, and every seat then trains almost only on those builds. The diversity that
   makes a shipped model able to play whatever a player made comes precisely from the random draw.
3. **Where it genuinely is the lesson:** team exercises, where composition and counter-composition are the point.

So: **build-choosing lives in the team stages only, never in the class curriculum**, and even there a fixed share
of episodes (a third, say) keeps the random draw, so the policy never stops seeing builds nobody would choose.

**The one thing that blocks the join.** `bootstrap.py:184-201`: the trunk, the GRU, the goal head and the goal
embedding are **shared across every layout**, and a stage must agree on them (`"the trunk in the checkpoint does
not match"`). A druid-only run produces a druid trunk; a warrior-only run produces a different one. Ten
class-trained adapters against ten different trunks cannot be joined as the learner stands.

The real choice is **change the learner now, or run a large distillation later**:

1. **Per-class trunks now.** Make `trunk.` per-layout, so a class owns its whole network. Matches how models
   already ship (`export.py:78` writes adapter + trunk + head per layout). Cost: a learner change before the druid
   run, ~10x parameters, and no cross-class transfer ever — but also no distillation ever, and the join becomes
   arithmetic rather than training.
2. **Distil ten teachers later.** A class-join stage where `bootstrap` takes a *map* of layout → checkpoint and
   copies each class's adapter and head with a fresh trunk. The bootstrap change is small, but an adapter trained
   against trunk A is noise to a fresh trunk B, so the trunk must be relearned with ten warm adapters — that is a
   large joint run, and `distill.py` exists precisely because this is the expensive path. The cost is deferred,
   not avoided.
3. **Abandon individual training.** Shared trunk, all classes together, per-class arena weights. Cheapest, and not
   what was asked for.

Recommendation: **(1)**. It is a smaller total spend, it matches what the exported model already is, and it makes
"each class learns its own curriculum" true rather than approximately true.

---

## Order of work

1. **Part A** — `Action.Dispel` and the dispel mask in `ActionCatalog`, then `Character/Aptitude.{h,cpp}`, then
   the seven changes, then delete `enum Role` and let the compiler find the rest. Manifest format 5,
   `PROTOCOL_VERSION` bump. One commit for the catalog, one for `Aptitude`, one for the removal.
2. `0a` retire `MOVE_TO_OBJECTIVE` and rewrite the baseline's travel branch; `0b` drop the emitter target;
   `0c` per-class config lookup; `0d` the turn actions and `face_objective`; `0e` the terrain probe and the
   obstacle arena; `0f` pitch, the water arena, and retiring `ASCEND`/`DESCEND` with it. One commit each — and
   measure `0e`'s probe against the decision budget before settling its reach.
3. The stage list in `Stages.cpp`, renumbered to the ladder above, movement root first.
4. The leaf rule: drop it from `Stages.cpp`, make `bootstrap.py` refuse a missing layout loudly.
5. `python/configs/stage*.yaml` for the new names and gates; `python/configs/druid/` for the druid's floors.
6. Train: `AnimusForge.Classes = "druid"`, `OutputDir = var/animus-forge/druid`, the queue in order.
7. The join, once (1) in Part C is settled.

Durable copy on implementation: `.agents/plans/animus-druid-curriculum/animus-druid-curriculum.PLAN.md`.

### Budgets

Per-class changes the arithmetic: 300M spread over 18 layouts gave each ~17M; a druid-only run gives the druid all
64 envs. Convergence ends stages early anyway (`patience: 3`), so these are ceilings.

| Stages | Budget | Why |
|---|---|---|
| 1-4 (feet) | 20-30M each | Short episodes, few actions live, one clear objective |
| 5 (duel) | 100M | The root of the catalog: 91 actions, four builds, 80 levels, and no role label to lean on |
| 6-8 | 50M each | Inherit a working policy, add one block |
| 9-13 | 30-50M each | Ditto; 13 is self-play and stalls early |
| 14-17 | 50M each | Four seats an episode, so an env step buys more rows |

## Verification

1. **Roles are gone.** `grep -rn "Role" modules/mod-animus-lib/src/` returns only `PlayRole`-free matches; the
   build is the check. `forge scenarios` still lists every stage.
2. **Aptitude reads the build, not the template.** Dump `layouts/<stage>/layouts/druid.json` and confirm the
   aptitude features differ between a feral_bear and a restoration seat, and that a `RandomTalentChance` seat gets
   values matching its actual talents rather than its template's.
3. **Stages load.** `Server.log` for `Stage ... is left out` — what a broken `Extends`, a repeated block or a
   missing dependency prints.
4. **Orientation is continuous.** With a random policy on `stage1_move`, log the seat's orientation each decision
   and confirm it takes values off the 45° lattice — before `0d` it cannot, which is the failing case that proves
   the test reads what it claims. Then on a trained policy, `walk_distance / start_distance` should approach 1 on
   open ground; a zig-zag shows up there as a ratio stuck near 1.08 (the cost of averaging two lattice bearings).
5. **The root can move.** Run `stage1_move` with `AnimusForge.Policy = "random"` and
   `AnimusForge.Queue.LocalEpisodes = 64` first. `walk_distance` well above zero and `arrived` above zero on random
   actions proves the legs and the objective bearing are wired; the hazard drill's failure was invisible until 4M
   steps in.
6. **The baseline still travels.** `eval.baseline: fight` on `stage1_move` must report a non-trivial `arrived`
   after `0a`. If it is ~0, the rewrite is wrong and `min_over_baseline` must come out of the gate.
7. **The gates read what they claim.** After each stage's first evaluation, check `eval.csv` and `layouts.csv`
   carry every gated column and that `spec_metrics` splits four ways for the druid — `episode_spec` is already
   emitted per episode.
8. **Composition still works without roles.** In `stage15_party`, confirm over a few hundred episodes that parties
   get a high-`mitigation` and a high-`direct_heal` seat at about the rate `RollRole` used to produce, and that
   `wipes` has not jumped — that is the number that says aptitude-driven composition is as good as the table was.
9. **Per-class runs do not collide.** Train `stage1_move` for druid, then for warrior with a different `OutputDir`,
   and confirm two independent `runs/stage1_move/` trees with two `best.pt`.
