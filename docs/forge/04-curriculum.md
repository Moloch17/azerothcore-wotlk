# 4. The curriculum

> **The movement curriculum (2026-10-05).** The curriculum is being rebuilt from scratch around the player
> controller, stage by stage. Defined so far: `move1_controls` -- an empty Stockades (map 34, its creatures cleared),
> the seat at the entrance and one fixed objective at the end of the entrance hallway (`ArenaDefinition::Objective`,
> MarkerEncounter, `Opposition::Markers`): reach it as fast as possible and stop within a yard (`ObjectiveRadius`),
> the time paid as `Markers.StepCost` and the stop as Arrive. Every class and race at level 1, death knights at 55
> (`StageDefinition::Level`). Every movement stage runs 50 ms world ticks (`AnimusForge.Stage.<name>.TicksPerDecision`;
> `test_stage_ticks.py`), and names the measures `forge status` shows for it (its config's `status.headline` and
> `status.targets`).
>
> `move2_seek` (perception-goals P1, 2026-10-06; redesigned the same day, REDESIGN §2) extends it: the same
> Stockades, the seat at a random hallway point, one real object (a chest, crate, barrel, sack or strongbox) placed by
> a ladder on the shaping fade's rungs -- in the hallway in sight of the spawn, just inside a front cell's opening,
> anywhere in a front cell, then deep (back rooms, hubs, end rooms; `ArenaDefinition::Rooms` and `Objects`,
> SeekEncounter, `Opposition::Seek`) -- found by sight and stopped beside (3 yd), in 90, 120, 200 or 300 s by rung.
> It has no compass: the camera's objective flag shows the object only in line of sight. It carries the mental map
> (`BlockId::Map`, REDESIGN §3): a 48 x 48 heading-up crop at 2 yd of what its own camera and body have written.
> Evaluations play 78 episodes at the training rung; the held-out `sweep` arena is the full 195-pair sweep at the top
> rung, for the stage's end.
>
> `move3_interact` (dungeon-curriculum M3, 2026-10-06) extends it: an empty Deadmines (map 36) at its band, levels
> 17-20, and the sight block (`BlockId::Sight`: the seen and remembered entity list and its presses, sent as a client
> sends them; no looting). The goal names what -- the sight block's named row, a kind's class and template entry and
> the task -- never where: no compass, no objective flag. A ladder on the fade's rungs (InteractEncounter,
> `Opposition::Interact`, `ArenaDefinition::Sites`): the named object among two to four decoys of other kinds, all in
> sight (distinguish); behind a shut door whose real lever, on the seat's side, opens it (switch: the Factory, Foundry
> and Mast Room doors); the cannon, which only the Defias Gunpowder the seat carries from the start opens (key). Paid as
> Arrive and DoorOpened (Outcome), WrongObject (Cost), Sighting (Shaping). Evaluations play 64 episodes at the training
> rung; the held-out `sweep` plays every rung in turn.
> `move4_follow` (dungeon-curriculum M4; I5 and I4, 2026-10-06) extends `move2_seek`: a party of five in an empty
> Ragefire Chasm or Deadmines at the dungeon's level band -- a leader in the owner's slot walking the dungeon's route
> from the door through each boss's place (the script's keys on the player controller; PartyFollowEncounter,
> `Opposition::PartyFollow`) and four learned followers keeping 3-10 yd from it, out of its way, regrouping at its
> stops. The followers see the leader through the camera, the mental map and the party frames block
> (`BlockId::PartyFrames`: the frames always, the minimap's dots within 60 yd). A follower that dies rises at the
> entrance after `Respawn.DelayMs` and walks back (EntranceRespawn); no episode ends on a death. Its ladder (the fade's
> rungs: walking, running, sudden stops, steps back) steps on the kept share alone; no cost ladder.
>
> | Stage | Budget | Eval every | Episodes |
> |---|---|---|---|
> | `move1_controls` | 150M | 5M | 512 |
> | `move2_seek` | 250M | 10M | 78 |
> | `move3_interact` | 200M | 10M | 64 |
> | `move4_follow` | 250M | 10M | 64 |
> | `combat1_fight` | 300M | 10M | 240 |
> | `combat2_packs` | 300M | 10M | 240 |
> | `combat3_survive` | 300M | 10M | 240 |
> | `group1_roles` | 400M | 10M | 384 |
> | `group2_corridor` | 500M | 10M | 128 |
> | `dungeon1_pulls` | 300M | 10M | 192 |
> | `dungeon2_ragefire` | 1500M | 20M | 64 |
> | `dungeon3_deadmines` | 2000M | 20M | 64 |
>
> **The combat stages** (dungeon-curriculum C1-C3, 2026-10-06; `CombatEncounter`, `Opposition::Combat`) extend
> `move3_interact` on a cleared Ragefire Chasm (map 389) at its level band, 13-18: `combat1_fight` one creature at a time
> (and a passive friend to taunt off and heal), `combat2_packs` packs of 2-4 (casters, linked, fire underfoot, the next
> pack further on), `combat3_survive` packs that can kill, with food and drink. The seat perceives what a player does
> (I3): the sight list (what it sees and remembers) with each visible unit's nameplate combat columns, the player and
> pet frames and the target frame's threat (`BlockId::Combat`, revision 1); it selects by sight and casts as the
> client does. A death never ends an episode: the seat is back alive at the dungeon's entrance after 10 s and walks
> back (I4's EntranceRespawn, `ArenaDefinition::RespawnAtEntrance`); every second dead or away from the fight is a
> Cost. Every ladder steps on its gate alone and every price is full from the start.
>
> **The party frames, revision 2** (G1, 2026-10-07): `BlockId::PartyFrames` is the one source of party-member state --
> each member's frame (alive, health, power and whether it is mana, in combat, the leader, in the frame's 40 yd range,
> its debuffs and the dispellable ones, aggro, selected, focused), its minimap dot within 60 yd, and its target (has
> one, hostile, the seat's own, in the camera's frame), with select, focus and assist presses on each frame. The
> combat block (revision 1) keeps only the player frame, the pet frame and the target frame. Both name their columns,
> so a checkpoint of either old revision seeds them by name; the startup check refuses the party frames beside the
> party or support block, and a combat stage with a party but no party frames.
>
> **`group1_roles`** (dungeon-curriculum G1, 2026-10-07; `RolesEncounter`, `Opposition::Roles`) extends
> `combat3_survive` and merges `move4_follow` (its party frames carried by name, `bootstrap.seed_merges`): a party of
> five on the same cleared Ragefire Chasm at 13-18, drilling one role an episode in seat 0 -- `tank_hold` (hold every
> enemy), `heal_keep` (keep everyone up through packs of twice their health, within mana), `damage_discipline` (kill
> the tank's target without pulling it), `pull` (pull one pack of a camp at a time, the packs closer each rung). The
> drilled role's class and build are drawn among those whose spec plays it; the rest is a proper party in a core group,
> with co-op partners (I7, never the drilled seat) and the "human" stand-in in a share of the episodes. Each drill's
> lesson is the drilled seat's own Outcome (DrillHold, DrillKeep, DrillFocus, PullClean), tier-scaled, its misses a
> Cost; Clear and Survived every seat's. Pack after pack for the episode's clock; a death rises at the entrance and
> walks back to the party.
>
> **The dungeon stages** (dungeon-curriculum G2, D1-D3, 2026-10-07; `InstanceEncounter`, `InstanceLadder::Wing`):
> a party of five on a real dungeon's own ground -- a fresh instance a run, every pack and patrol where the world
> database stands it -- with G1's blocks (the party frames revision 2, the sight list and the combat block's frames)
> and the pack block; no crowd block (its pack ahead and nearest object were radius reads through walls) and
> no party or support block. `group2_corridor` extends `group1_roles`: four of a wing's packs in route order a run
> (`ArenaDefinition::CorridorPacks`), in Ragefire and the Deadmines -- pull, fight, rest, ready, next. `dungeon1_pulls`
> drills one Ragefire pack a run on its own ground (the pull drill, evaluated as a drill: `EvaluatesDrills`).
> `dungeon2_ragefire` is the door to Bazzalan, a full clear; `dungeon3_deadmines` the door to VanCleef at 17-20
> (`LevelFirst`/`LevelLast`), its doors and levers by an interact and the cannon by its gunpowder -- the bar is 70% of
> the evaluation's runs cleared with at most one wipe (`bar_clear`), read per boss (`boss_*`) and by role
> (`deaths_tank`, ...). G2, D2 and D3 learn from their own rewards on the whole dungeon's difficulty
> ladder (`StageScenario::WING_RUNGS`: the levels above the band and the wipes spared, stepping on the probes alone);
> nothing scripted plays beside them. **Wailing
> Caverns is held out** from D2 on (the arena `heldout`, `EvalOnly`: never in a training draw, played by
> `eval.heldout`). The "human" stand-in is a frozen learned partner from the learner's co-op partner pool in one seat
> of a fifth of every stage's training runs (`ArenaDefinition::StandInShare`; it leads or follows, in the role it wants;
> never trained on; not fielded while the pool is empty, which `forge status` says),
> and H is read as `clear_standin` (the `with_human` arm) beside `clear_allbot` (the plain evaluation) and their gap
> `standin_gap` (within ~10 points). Outcome: Clear (a corridor's packs in route order, a full clear), ReadyPull (a pull
> started with the party ready), Kill (bosses, trash), PullClean (the drill); Cost at full price: PullExtra (a chain
> pull), Idle (standing about), Lost (straying from the leader), Death (deaths and wipes; the second wipe ends a run),
> StepCost (the clock). A run's outcome scales with the ladder's rung (`WingRun::TierOfRung`), not the pinned row.
> **What a seat knows of the dungeon** (2026-10-07): its goal places and its objective are a player's knowledge --
> what it discovered: the hostiles it saw (entity memory: where it last saw them, alive as last seen), its own mental
> map's frontier and its leader -- never a live pack's or boss's position nor the route's pack order (SeenPlaces;
> `StageDefinition::GoalPlaces`, seen only by default, the user's choice 2026-10-07). The dungeon map's layout nodes
> can be added by `AnimusForge.Curriculum.Stage.<name>.GoalPlaces = 0`, but today they are sampled along the boss
> route, so they stay off until a whole-instance layout exists. 
> Evaluation videos film one seat a party, spread over the tank, healer and damage places and their classes
> (`Vision::EvalVideoAgent`); `apps/forge/tools/collect-videos.sh <stage>` brings the workers' videos home.
>
> **Removed.** Two earlier curricula are gone from the tree: the first movement curriculum (`move1_controls` ...
> `move7_follow`, git tag `curriculum-movement-v1`) and the first full curriculum (`stage1_move` ... `stage21_ship`,
> git tag `curriculum-v1`; the whole tree before the cleanup is the tag `pre-cleanup-2026-10-07`). Their stage
> definitions, arenas, encounters, blocks and learner configs are in those tags and nowhere else.

The curriculum is the set of scenarios the policies train on. It lives under `src/server/game/Animus/Scenario/Curriculum/`.
**Twelve stages in a single line**, in the table above. No stage has a pass gate: each ends when its convergence
signals say so (see "Budgets" below), and the queue moves on.

Every stage trains the same ten class policies over a shared trunk, so what one class learns about moving, threat or
interrupts helps the others. A class policy plays every build its class has, and is measured per (class, build).

A stage names the one it `Extends`, and its networks are seeded from that stage's best checkpoint block by block:
blocks it keeps carry over, blocks it drops are left behind, blocks it adds start from nothing -- unless a stage it
`Merges` has them. The chain is one line, and the order is the training order, so `forge start` with no arguments
walks the whole thing from `move1_controls` and never reaches a stage before the stage it seeds from.

```
movement   move1_controls         the controls, one objective at the end of a hallway (Stockades)
           └─ move2_seek          a real object, found by sight, in a random room
              ├─ move3_interact   named object among decoys, doors and levers, the cannon (Deadmines)
              └─ move4_follow     a party of five; four learned followers keep up with a scripted leader
combat        └─ combat1_fight    one creature at a time (Ragefire Chasm, cleared)
                 └─ combat2_packs     packs of 2-4
                    └─ combat3_survive  (+ merges move4_follow) packs that can kill, food and drink
party                  └─ group1_roles     a party drilling one role an episode
                          └─ group2_corridor   four of a wing's packs in route order
dungeons                     └─ dungeon1_pulls     one Ragefire pack a run, the pull drill
                                └─ dungeon2_ragefire   the door to Bazzalan, a full clear
                                   └─ dungeon3_deadmines   the door to VanCleef
```

`move3_interact` extends `move2_seek`, and so does `move4_follow`; the line is the order a plain run takes, and
`combat1_fight` seeds from `move3_interact` alone. `combat3_survive` is the one stage that merges another
(`move4_follow`, for its party frames).

**It is a line rather than a tree** for one reason: a branch is cheaper to train but ends in several checkpoints,
and everything a leaf teaches is discarded unless the stage exported from is downstream of it.

**A drill is an arena, not a stage.** A separate drill learned nothing over its seed (a later stage overwrites an
earlier one), so drills are weighted arenas of the stage they serve (`ArenaDefinition::Weight`).

### Seed chain and config chain

`.Extends` and `.Merges` in `Stages.cpp` are the **seed chain**: which stage's checkpoint a run starts from. The
sim writes it into `stage.json` as `seed_chain`/`merges`, and the learner reads only that (`animus.stages.seed_chain`,
`TrainConfig.resolved_init_from` with `init_from: auto`). `extends:` at the top of `python/configs/<stage>.yaml` is
the **config chain**: which file's settings are inherited. It never decides what a run seeds from. If you change one
chain, decide what the other should do rather than assuming it follows.

### Budgets

From `python/configs/*.yaml`; the table at the head of this chapter is checked against them by
`python/tests/test_manual.py`. A budget is a **ceiling, never a target**: a stage ends when every class it plays has
converged (`python/animus/stage.py`), and a stage that reaches its budget first advances anyway, with its report
naming the classes that were not done and the signal each was missing. There are no pass gates. A `forge fast` run
replaces the budgets (20M a stage, evaluations every 1M of 64 episodes, and `patience` 0 so every stage trains its
whole budget).


## 4.1 Defining a stage

A stage is one `StageDefinition` entry in `Stages/Stages.cpp`:

```cpp
stages.push_back({
    .Name = "move1_controls",               // scenario name
    .Suffix = "_controls",                  // model names
    .Extends = "",                          // seeds from it (the trunk); empty for the first
    .Summary = "an empty Stockades: from a random hallway point to a real object in sight ...",
    .Blocks = { Core, Move, Compass, Vision, Goal },   // layout order
    .Arenas = {
        { .Name = "hallway", .Weight = 1, .Against = Opposition::Sight, .EpisodeSeconds = 60,
          .SpawnPoints = StockadeHallways(), .MapId = MAP_STORMWIND_STOCKADE, .Objects = SeekObjects(),
          .SightPairs = StockadeSightPairs() },
    },
    .MapId = MAP_STORMWIND_STOCKADE,
    .SpawnPoints = { StockadeEntrance() },
    .Level = 1,
});
```

An `ArenaDefinition` describes one situation:

| Field | Values |
|---|---|
| `Name` | Unique within the stage. Used in episode info, `stage.json`, tuning keys and per-arena gates |
| `Weight`, `WeightFinal` | Share of episodes, overridable with `<TuningPrefix>Arena.<stage>.<arena>.Weight` / `.WeightFinal` (the share at the end of the budget, reached linearly) |
| `Seats` | `Solo` (1) or `Party` (one group: 1 to `GROUP_SEATS`, a tank, a healer and damage) |
| `Against` | `Instance` (a real dungeon's own ground, `Instance` and `InstanceRow`), `Seek`, `Sight`, `Interact`, `PartyFollow`, `Combat` and `Roles` (the movement and dungeon stages' drills) |
| `PartyGroup`, `ProperParty`, `PartySize`, `DrillRole` | The seats form a core group; a full party of a tank, a healer and three damage dealers; how many learned seats; the role seat 0 drills |
| `Instance`, `InstanceRow`, `PullDrill`, `CorridorPacks`, `LevelFirst`/`LevelLast`, `EvalOnly` | A dungeon wing's ladder, the row it always runs, the one-pack drill, the run of packs in route order, the level band, and content only an evaluation plays (Wailing Caverns) |
| `Rooms`, `Objects`, `SeekRadius`, `SightPairs`, `Sites` | What the seek, sight and interact arenas hide or put down, and how near a stop finds it |
| `Combat`, `Ally`, `Roles` | The combat drill (`Fight`, `Packs`, `Survive`), a friend beside the seat, the roles drill |
| `RespawnAtEntrance` | A death brings the seat back alive at the instance's entrance after `Respawn.DelayMs` |
| `StandInShare` | Share of a party arena's training episodes with the "human" stand-in in one seat |
| `Hazards`, `CommandedGoals` | Every pull holds a ground-effect caster; the sim gives the seat its goal as an order |
| `EpisodeSeconds` | 0 = the host's `EpisodeSeconds` |
| `SpawnPoints`, `MapId`, `MinLevel`, `HeldOutSpawnPoints` | Ground of the arena's own, when it is not the stage's |

A `StageDefinition` may also name its own `MapId`, `SpawnPoints` and `HeldOutSpawnPoints` (0 = the host's
`SpawnMapId` and `SpawnPosition`) and a `Level` or `MinLevel`. Every live stage stands on an instance's map (a fresh
instance an episode, or an env's own), so no env sees another's.

**A spawn point is drawn per episode, not per env.** `EnvState::Spawn` is rolled at the reset, so it is stable within
an episode, reproducible from an evaluation seed, and every seat sees all of the stage's ground. A spawn point that no
objective can be found from no longer takes the run down with it either: the reset draws again and moves the seats, up
to four points, and only names a failure when all of them fail.

**`HeldOutSpawnPoints` is where scored episodes stand, and where training never does.** The split is keyed on
`Env::Evaluating` rather than on the presence of a seed. No live stage sets it today (the control ground the first
curriculum measured terrain reading with went with it); the mechanism remains.

**Validation.** `CurriculumStages()` checks each definition in order and leaves out (with an error log) any stage
that:

- doesn't start with `core`, then `move`, or lists a block twice,
- needs a block another is missing (the mental map and the sight block need the vision block, a combat stage with a
  party needs the party frames),
- extends or merges a stage that isn't an earlier valid stage, merges its base or the same stage twice, or merges
  without extending,
- has no arenas, more than `MAX_ARENAS` (16), or two arenas with the same name,
- has an inconsistent arena (`ArenaProblem`): an instance ladder without the pack block or a party, a party group
  without the party frames, a seek, sight or interact arena that is not one seat on its own with the blocks that walk
  and see it, a roles arena that is not a proper party of five on a dungeon's map, a combat drill with the wrong
  blocks, a corridor or level band outside a dungeon wing, or an arena on a map of its own without spawn points of its
  own.

The base only has to exist. Seeding maps blocks by name, so a stage may drop base blocks it doesn't need, and several
stages may share a base.

## 4.2 How `StageScenario` runs a stage

`StageScenario` implements `Scenario` for any `StageDefinition`.

### Construction

1. **Tuning.** `CurriculumTuning::Load(settings.TuningPrefix)` reads every value (4.9). `DecisionScale =
   DecisionMs / 50`.
2. **Layouts.** One `Layout` per class in `StageSettings::Classes` (or all 10) whose assets have at least one
   race. `Layout::Index` is its position in this list, which is the index the learner sees.
3. **Class assets.** Every class's assets are built now (a stand-in or a party's partner can be any class). Building
   takes seconds per class, and doing it now avoids stalling the world thread during an episode reset.
4. **Spec.** `AgentsPerEnv` is the largest arena's seat count. `ObsDim` and `NumActions` are the largest layout's.
   `StateDim` is fixed (4.8).
5. **Encounters**, created only if some arena needs them, in **build order** (the party group, then
   the arena's own encounter), and **reward order**, which is also the order of episode info columns. For each arena the scenario
   keeps the subset it uses, in both orders, plus its weight and episode length.
6. **Pools.** The opponent pool and the consumable pool are loaded now rather than on the first episode.
7. **Episode info columns.** The core columns (4.10), then each encounter's, then one `reward_<term>` column per reward
   term any encounter pays.
8. **Stage files.** If `LayoutsDir` is set, the manifests and `stage.json` are written (see
   [3.9](03-animus-lib.md#39-the-stage-description-stagejson)).

### Building an episode (`Rebuild`)

`Setup` builds each env's first episode and marks the env `Fresh`, so the pool's first `Reset` is skipped. After that,
every `Reset` calls `Rebuild`:

1. **Draw the arena** by weight: no draw for a single arena, otherwise `urand` over the
   weights. It is drawn first, so a seeded evaluation episode draws the same arena. The env's episode length is set
   from the arena.
2. **Clear totals.** Every seat's `ResetEpisode` and every encounter's `ResetEpisode`, including encounters this arena
   doesn't use, so their columns read 0.
3. **Deactivate** encounters the previous arena used and this one doesn't (disband the group). Then `BeforeRebuild` for this arena's encounters (the party group disbands before its members are
   replaced).
4. **`Begin()`** every seat's `BotSlot`, and remember each seat's current character for rollback.
5. **Pick the seats.** A party arena draws its size from `Party.SizeWeight1-4`; a party follow's size is fixed. Half the time
   (`Party.ClassicChance`) the roles are the classic makeup -- a tank and a healer at the head of every group of
   five, the rest damage -- shuffled over the seats in play. Otherwise each seat's
   role is drawn (`RoleTankChance`, `RoleHealerChance`). Each seat then takes a random layout of its role, or any layout
   if the run has none of that role. Other arenas give every seat any layout. A training episode draws it by the
   learner's per-layout weights (`WEIGHTS`, evenly without them); an evaluation episode doesn't draw at all: seed
   *i* plays candidate *(i + seat) mod count*, so every class is scored on an equal share of the seeds. Seats
   beyond the active count get no layout and no bot.
6. **Pick one level** every seat's class can be (death knights start at 55). It is `StageSettings::Level` if set;
   otherwise `Characters.HighLevelChance` percent of the time a level from `HighLevelFirst` to 80,
   `Characters.LowLevelChance` percent of the time a level from the class minimum to `LowLevelLast` (20; skipped when
   the class can't be that low), else any level from the class minimum to 80. An **evaluation** episode takes its
   band from its seed instead, as it takes its difficulty tier: seed *i* plays band *(i / the pairs) mod 4* of
   1-20, 21-40, 41-60, 61-80 (the next band up when the seat's classes cannot be that low). Training then keeps the
   level mix the shipped companions play while the evaluation measures every band in equal numbers -- drawn, the
   middle bands were ~9% of the episodes each, too thin to read a class's hole from.
7. **Build each seat** (`BuildSeat`): random race and spec, `DamageScale(level)`, a bot named
   `Forge<envId>s<seat><a|b>` on the slot's idle session and account, placed in the env's instance (the first build of
   an env opens a new instance, unless the host placed the env). Party seats start spread around the spawn point, and a
   mirror seat 2 spawns 40-50 yd from seat 1 at a random bearing. Talent points are recomputed on the spawn map (death
   knights are created in Ebon Hold, where only quest-rewarded points count), then `SeatCharacter::Configure` dresses
   the bot (4.3).
8. **On failure**, every slot aborts, each seat's previous character is restored, and `Rebuild` returns false. The
   scenario sets `BuildFailed`, `IsTerminal` ends the episode at the next decision, and the following reset tries again.
9. **Commit.** Old targets despawn, every slot `Promote()`s (the old bots log out), and the first build of an env clears
   the spawn point's own creatures (`SpawnArea::Clear`). `env.Bots`, `MapId` and `InstanceId` are updated and
   `env.Targets` cleared.
10. **Encounters build** in build order (the group, then the arena's own).
11. **Stock the seats**: potions, mana potions, bandages, healthstones (a warlock in the party hands them out), a
    soulstone, and a flask or elixir (4.3).

### Each decision

`ApplyActions`:

1. every active encounter's `UpdateEnemies` (linked packs join fights), then `Update` (the leader walks, the next pull
   spawns),
2. `AcceptResurrections`: dead seats and the owner accept a pending resurrection request as a client would, and
   the seat that cast it is credited once the ally is actually alive (`StepRevivedAlly`, `Revives`) -- see the
   note below,
3. for each seat with a living bot: `CurrentTarget` (the first encounter whose `SelectTarget` answers, else target 0),
   build the `SeatView`, `SeatEncoder::Apply`, fold the result into the seat's
   totals, `OnSeatAction` on every encounter, and summon a called hunter beast.

> **Why accepting a resurrection needs bookkeeping.** A client answers an offer once, with one
> `CMSG_RESURRECT_RESPONSE`, so nothing in the core clears the request afterwards -- neither
> `Player::ResurectUsingRequestData` nor `ResurrectPlayer` touches `m_resurrectGUID`. Polling it every decision,
> as this must, therefore has to remember what it has already accepted. Before it did, one landed Rebirth stood
> its target back up free of charge every decision it died for the rest of the episode: 38.4 revives an episode
> against 0.97 owner deaths in stage 4, earning 57.55 of `druid_dps`'s 65.08 return. Every stage seeded from
> those learned that a druid scores by standing near a corpse.
>
> The fix is the module's and not the core's: remember which offer was accepted, wait for it (a delayed teleport
> reschedules the resurrect, so it does not always land on the decision it was taken), pay once the ally is
> alive, and clear the request then. A retry window gives up on one that never lands rather than barring the
> seat for the episode.

`Observe`: each seat's row through `SeatEncoder::Observe` (tracking when the seat entered combat for the combat-time
feature), then `WriteState`.

`Reward`: `BeforeRewards` on the arena's encounters in reward order (a pull's clear is decided once here, for every
seat), then for each seat compute `LastStepDamage` (damage / damage scale) and `LastStepDamageTaken` (damage taken / max
health), which several encounters read, and let each encounter add its terms to the seat's ledger. The ledger's step
total is the seat's reward. Finally `AfterRewards` (a cleared pull is removed).

`IsTerminal`: a failed build, or any active encounter's `IsTerminal`.

### The seat view and encoder

`SeatView` is everything the blocks can't read from the world themselves: the layout, bot and target; the character
as built (level, race, spec, talent build); last-step damage, power change and damage taken; combat time; the supplies
it carries; whether self-resurrection is allowed; a hunter's stable; the enemy slots and selected slot; pull state
(pulls cleared, quiet time, pull time, elite pull, food and drink items); the owner; the three teammates and the
party's tank; the enemy player and whether it is a learned seat. Encounters fill the parts they own (`Encounter::View`).

`SeatEncoder::Observe` applies these rules in order:

- The row and mask start at zero, and **action 0 (no-op) is always allowed**.
- The character features (level, race, spec, talents) are always written, alive or dead.
- **A dead bot** sees only the duel block's dead features (whether it can resurrect itself), and its only possible
  action is the self-resurrect action.
- **No target** (between gauntlet pulls) blocks observation and actions, unless the layout acts without a target, which
  is any layout with the gauntlet block (food, drink, and self-cast spells between pulls).
- **Hidden enemies.** An enemy the bot can neither see nor detect (`CanSeeOrDetect`: stealth, invisibility) is left out
  of the view, as a client leaves it off the screen. An enemy slot holding one reads empty, the pvp block writes only
  what the bot remembers about a hidden opponent, and a hidden target is `HiddenTarget` instead of `Target`: every block
  sees no target, but the seat still observes and acts, and the duel block shows where the target was last seen and
  lets the seat search there. The critic's state (4.8) keeps everything.
- Otherwise every block writes its slice and mask.

`SeatEncoder::Apply` ignores actions of a missing or dead bot (except its own resurrection). It runs every block's
`BeforeApply` on every decision, including a no-op, then hands a positive action to the block that owns it, as an
index local to that block.

## 4.3 Characters

### Class/roles

| Class | Class/roles (specs) |
|---|---|
| Warrior | `warrior_dps` (Arms, Fury), `warrior_tank` (Protection) |
| Paladin | `paladin_heal` (Holy), `paladin_tank` (Protection), `paladin_dps` (Retribution) |
| Hunter | `hunter_dps` (Beast Mastery, Marksmanship, Survival) |
| Rogue | `rogue_dps` (Assassination, Combat, Subtlety) |
| Priest | `priest_heal` (Discipline, Holy), `priest_dps` (Shadow) |
| Death Knight | `deathknight_tank` (Blood), `deathknight_dps` (Frost, Unholy) |
| Shaman | `shaman_dps` (Elemental, Enhancement), `shaman_heal` (Restoration) |
| Mage | `mage_dps` (Arcane, Fire, Frost) |
| Warlock | `warlock_dps` (Affliction, Demonology, Destruction) |
| Druid | `druid_dps` (Balance, Feral cat), `druid_tank` (Feral bear), `druid_heal` (Restoration) |

Each `SpecProfile` (`Character/ClassRoleProfile.cpp`) sets a talent tab, a stat profile for gear (strength melee,
agility melee, ranged, caster, healer, tank), a range band (melee or ranged, which drives approach shaping) and weapon
layouts tried in order (two-hand, dual wield, daggers, one-hand, one-hand and shield, one-hand and held item, staff,
two-hand stat stick and ranged, optional wand).

Tank and healer roles play their own spec's damage game in role gear until the companion and party stages give them
their actual jobs.

### What `SeatCharacter::Configure` builds

The same function builds training seats and live companions, so a model gets in play exactly what it trained with.

- **Talents.** Every character draws one of three plans (`SeatCharacter::TalentPlan`, `Characters.*TalentChance`),
  reported as the episode info column `talent_plan` and scored as its own group in every evaluation:
  - **standard** (60% by default): the spec's standard 3.3.5 build from `Character/SpecBuilds.cpp` (31 specs),
    generated by `tools/spec_builds/generate.py` from `builds.py` and checked by `validate.py`. A build lists talents
    in the order players take them. Each point goes to the first talent in that order that still wants ranks and is
    legal (row and prerequisite rules follow `Player::LearnTalent`), so a low-level character has the talents players
    pick first. Every build spends exactly 71 points at level 80. Where a spec's tree has a root, snare or silence
    a player of that spec takes for solo play, the build takes it too (fury Piercing Howl, marksmanship Concussive
    Barrage, survival Entrapment, subtlety Waylay, shadow Silence, frost death knight Hungering Cold, affliction
    Curse of Exhaustion, feral cat Infected Wounds), so the policy has them to learn with.
  - **noisy** (30%): that build stopping 1 to `Characters.TalentNoisePoints` points short, with the rest spent at
    random -- a build somebody made up the tail of.
  - **random** (10%): every point spent at random, deeper rows likelier, the spec's tree first (51 points, what its
    last row needs) and then the others.

  The variety is what makes the talent features worth reading: a standard build is the same every time for a spec and
  a level, so a policy trained on those alone can ignore its talents and memorise the spec. A policy that sees all
  three has to play the character it was handed -- which is what a live server hands it. Glyph slots the level has
  opened get the spec's standard major and minor glyphs, whatever the plan.
- **Kit** (`ClassKit`). Every spell the class trainers teach up to the level (`trainer`/`trainer_spell`, learn-spells
  resolved), talent-gated ranks when the talent was taken, and everything the class quests teach, read from
  `quest_template` (every quest only the class can take, its reward spell's learn-spells followed through, a race's
  quest for that race only: Defensive Stance with Sunder Armor and Taunt, Bear Form with Growl and Maul, Berserker
  Stance, the demons, totems' spells, Redemption, Runeforging and the rest), plus Raise Dead. Weapon and armor skills the race and class may have, maxed for the level.
  Reagents: totems, Ankhs, soul shards, corpse dust, flash powder, Light Feathers for Slow Fall and Levitate, and
  ammo in a quiver or pouch for hunters.
- **Gear** (`GearBuilder`). A random level-appropriate item for every slot, including both trinkets, drawn from every
  obtainable item (loot, vendors, quest rewards, crafted) the class can use and whose stats suit the spec. Random-stat
  items roll only suitable suffixes. The item level must fall in the band players of that level wear
  (`ITEM_LEVEL_ANCHORS`: a few levels above while levelling, Outland gear from 58, Northrend from 70, heroic dungeon
  180-213 at 80). If nothing fits, the search widens to 10, 25, then any number of item levels below the band, never
  above. After that it falls back to lighter armor, then stat-less items. Dungeon drops are weighted 4x and quest
  rewards 3x, and items near the middle of the band are preferred. Epics appear only at 70 and 80, and resilience gear
  only in PvP arenas. Paladins, shamans, druids and death knights get a relic.
- **Enchants and gems** (`GearEnhancements`). At 70 and 80 every item is enhanced, and half of them while levelling:
  the best suitable enchant a player of that level could buy (enchanting skill 5 per level, reaching 300 at 60 and 450
  at 80; weapon procs by name), a matching gem per socket (the socket bonus when all match, meta gems last, epic gems
  only at 80), death knight runeforges, rogue poisons (Instant in the main hand; Deadly in the off hand, or Crippling
  half the time once the rogue can have it), and shaman weapon imbues (enhancement: Windfury and
  Flametongue; elemental: Flametongue; restoration: Earthliving; the highest rank known). No profession-only enchants
  or gems.
- **Riding** (`TravelBlock::LearnRiding`, layouts with the travel block). The level's riding as players learn it
  (Apprentice at 20, Journeyman at 40, Expert at 60, Cold Weather Flying at 68, Artisan at 70) and the side's mounts
  of each speed (60% and 100% ground, 150% and 280% flying).
- **Supplies** (`Supplies`, applied by `StockSeats`). Five of the best healing potions, five mana potions (for mana
  users), five bandages with the level's First Aid skill, a healthstone and soulstone for warlocks, and at 70 and 80 a
  suitable flask (half the time an elixir while levelling). Gauntlet stages add the best vendor food and, for mana
  users, drinks: `Pulls.GauntletSupplies` (7) of each alone, five with an owner.
- **`PrepareFighter`**. No XP gain (levelling would change the character under the model), a warrior's stance (a first
  login normally casts it, and nothing works without one), and for hunters a stable offer of four tameable beasts of
  different random families.

### What an enemy is doing

A seat used to know one thing about an enemy's spellcasting: that it was happening. One bit, no identity, no clock.
It could not tell a filler from a heal, could not see an area effect on the ground at all, and had never read a
threat table -- so interrupting well, stepping out of fire and holding aggro were all unlearnable, and every dungeon
and raid mechanic has one of those three shapes.

All of it is now described by properties rather than by which spell it is (`IncomingSpell`), because a boss's
abilities live in file-scope `enum Spells` blocks inside its own script and no registry of them exists. A level 12
gnoll shaman's Lightning Bolt and a raid boss's produce the same features, and an unseen encounter needs no new code:

- **The cast** (per enemy slot, and for the duel's target): how much of it is left, whether it is aimed at this seat,
  area, cone, channeled, interruptible (by `EffectInterruptCast`'s own test, so the feature promises what pressing an
  interrupt would do), dispellable, shared damage (a soak), a heal, a summon, its radius, its missile flight time,
  its school and its mechanic.
- **The ground it is on** (`Encoding::StandingInHazards`): how many hostile ground effects the seat is standing in,
  how far it still has to walk to leave the worst, and the bearing of that one's centre, so moving away from it is
  the way out. Free to compute -- a ground effect applies an aura, and the aura knows the object that owns it.
- **The ground it is about to be on** (`Encoding::FindNearestHazard`): the nearest hostile ground effect it is *not*
  in yet, within 30 yd -- distance to its edge, bearing, radius. This is what makes avoidance learnable rather than
  only escape: without it nothing distinguishes clear ground from ground about to be walked into. A grid search over
  dynamic objects and armed traps, run once a second and re-measured arithmetically between searches, since a ground
  effect stays where it was cast.
- **What was done to it** (`Encoding::IncomingDebuffs`): harmful auras on the seat, how many are dispellable, the
  worst stack count, the longest remaining, and which crowd control mechanics are among them.
- **Threat** (`Encoding::ThreatShare`): the seat's own threat over the threat of whoever the enemy is on, so 1 means
  it holds aggro. Until this, every "threat" in the codebase was a proxy counted from who an enemy happened to be
  swinging at.

`hazard_seconds`, `hazard_damage` and `interruptible_casts_seen` in the episode info say whether any of it is being
used -- the last is the denominator the press-to-interrupt ratio always lacked.

For any of it to be learnable, the opponents have to produce it. Nothing did: the duel's pool is default-AI
creatures, which never cast, and no creature anywhere was selected for putting something on the ground. So
`Difficulty.CasterChance` (40%) draws a share of duel opponents from the same cast-only scripts the packs use, with
`Difficulty.HazardChance` (30% of those) from the ones that create a persistent area aura; and the upper pack rungs,
the last planned pulls and every raid rung include a hazard caster (`OpponentPool::RandomHazardCaster`). One enemy,
one cast and one pool of fire in a stage 1 duel is the cheapest place any of this can be learned.

### Durative actions

Most actions are one press of one button, and a 450 s episode is 1800 of them -- far more than credit reaches back
over. A few actions instead stand for a stretch of decisions (`SeatOption`, `Options.*`), so a plan can be expressed
in one choice -- and the move block's keys are held until the seat changes them (below):

| Action | Block | What it does until it stops |
|---|---|---|
| `rest_until_ready` | gauntlet | Eats and drinks, whichever is missing, until health and mana are back to 90% |
| `hold_interrupt` | pack | Interrupts the target the moment it starts casting, with the first interrupt the seat has -- its own spell, or its pet's (a felhunter's Spell Lock) when it has none. Offered only to a seat that has one |

A seat runs **one at a time** (`SeatOptionSet`): resting or holding an interrupt. Each runs in its block's
`BeforeApply`, every decision, and stops on its own condition (the fight starts, nothing is left to eat, the interrupt
fires) or when its `Options.*` clock runs out; the held interrupt survives any other press. No option moves the seat:
the companion's follow, the party's follow-the-tank, the dungeon advance and the corpse run were engine moves and went
with the player controller (C9). Keeping up, advancing and the corpse run are the move block's keys, learned in
stages of their own.

**The move block is a player's keys and mouse** (player-controller, 2026-10-05; move revision 2). Its 25 actions each
change one held control -- `move_forward` / `move_back` / `move_stop`, `strafe_left` / `strafe_right` /
`strafe_stop`, a turn rate (`turn_left_30` ... `turn_left_360`, `turn_right_30` ... `turn_right_360`, `turn_stop`:
degrees a second, the mouse's), a pitch rate (`pitch_up_30`, `pitch_up_90`, the same down, `pitch_stop`; water and
air), `ascend` / `descend` / `vertical_stop`, `jump` and `walk_toggle` -- and the controls stay held until the seat
changes them. The player controller (`Animus/Movement`) moves the body with the 3.3.5a client's own physics every
world tick; nothing is pathfound and no spline is laid for a seat. Masks cover only the impossible: pitch rates,
ascend and descend on the ground, a jump while falling or flying, everything when dead. Pressing the control already
held does nothing and costs nothing. This replaced the bearings, the halt, the facing modes and the turns and pitches
chosen whole (revisions 0 and 1), which the user had stripped outright; a checkpoint of those is refused on resume
and seeded fresh.

**Jitter** (`Actions.Jitter`, every stage). A press that reverses a recent one costs `Actions.Jitter` per quarter turn
it takes back (`MoveControls::Press`): the feet or a climb reversed 2, a turn or pitch rate against the last one the
smaller rate in quarter turns a second (left 90 then right 180 is 1), weighed by how recent the reversed one was:
e^(-dt / `Options.JitterDecayMs`), 2500 ms, so 0.45 two seconds on and 0.14 at five, with no window edge to time a
press against and a slow weave no longer free (movement-smooth C, replacing a 1500 ms window charged in full). Nothing else
in the rewards cared how a seat got where it was going, so a wobble that cost nothing was learned as harmless; in
flight the feet changed bearing every quarter second. `turn_reversals`, `pitch_reversals` and `bearing_flips` (in half
turns) count what was charged, and `reward_jitter` what it cost. Jitter, like the repeat, aimless, effort and fidget charges, is a
cost (`RewardCategory::Cost`): it never fades with the shaping, so spinning and re-pressing stay priced to
the end of every stage (as shaping they cost nothing once the fade ran out, 2026-10-04).
Facing is the seat's own: nothing turns it onto its target any more. A spell is offered whenever the only thing
in its way is something the seat can put right -- facing (or getting behind), range, line of sight, standing
still for a cast, power -- and a press that fails on one of them is charged `Actions.Aimless.CastFailed` (0.02, a
cost) under its cause (`aimless_cast_facing`, `_range`, `_sight`, `_moving`, `_power`). A cooldown, the global
cooldown and a cast under way stay masked. A spell press is applied before the decision's upkeep
(`Block::PressesFirst`), so a held interrupt or a step never spoils it (2026-10-04).
- **standby** (`hold_interrupt`): nothing the seat does takes over from it, because waiting for the target's cast is
  not something it stops fighting to do. Cancelled by any press, a hold lasted 0.6 s against casts of 1.5-2.5 s and
  interrupted next to nothing.
- `rest_until_ready`: any other action ends it, as recovering is what the seat is doing rather than something it waits
  through.

Each option's own action is masked while it runs, so nothing cancels itself. What it does is counted as a press would
be (food and drink used, an interrupt pending on a caster). The core block reports how much of each option's clock is
left (the companion block reports the follow's, so only its layouts carry that feature), so a running option is never
hidden state, and `options_started` and `option_seconds` in the episode info say how much a class uses them.

### The action catalog

`ActionCatalog` (per class, built once) is the fixed action space of the core block:

- no-op, cancel queued on-next-swing ability,
- **one action per rank chain of every combat spell** a level-80 character of any of the class's races knows (trainer,
  starting and racial spells, active talents of all three trees). The action casts the highest rank the bot knows. A
  spell counts as combat if it deals damage, applies a damage-relevant buff, debuff or DoT, generates resources,
  shapeshifts or summons, or if it is what separates a player from a rotation: charges and leaps (Charge, Intercept,
  Intervene, Blink, Disengage), threat redirects (Misdirection, Tricks of the Trade), Spellsteal, and survival auras
  (speed; mechanic and school immunity such as the PvP trinket, Every Man for Himself, Will of the Forsaken, Hand of
  Freedom and Fear Ward; dodge, parry, block, reflection; Feign Death, Fade and invisibility). Mounts, teleports,
  crafting, pure heals and charm are excluded. The candidates include the spells a learned spell teaches (the Feral
  Charge talent is Feral Charge - Bear and Feral Charge - Cat), and a spell that fires a missile is judged by the
  missile's spell (Freezing Arrow drops a Freezing Trap),
- one action per trinket slot, and one each for the use effect of the main-hand and off-hand item (weapons, shields
  and held books such as Rituals of the New Moon; `item_uses` counts them),
- then the rest of the kit, from the first stage on, as a player fights with it: **tactical spells** (interrupts, stuns
  with Sap included, silences, fears, roots, polymorphs, knockbacks, taunts, snares, disarms, traps, Distract and
  offensive dispels) cast at the target, and **sustain spells** (heals, HoTs, absorbs and friendly dispels) cast on the
  bot itself. Until they were core actions a duel healer had no heal and a duel mage no Polymorph or Ice Barrier.
  Each catalog entry in the manifest names its `group` (`combat`, `tactical`, `sustain`). Every layout's manifest,
  and its entry in `stage.json`, lists `action_names`: every action of the layout by name (`frostbolt_116`,
  `use_off_hand`, `start_attack`, `pet_stay`), which evaluations use to count the actions each episode took.

The catalog also keeps the lists apart for the blocks that cast them elsewhere:

- `Tactical()`: the tactical spells above.
- `Sustain()`: the sustain spells above.
- `Revives()` (companion and party blocks): Resurrection, Redemption, Ancestral Spirit, Revive, Rebirth (their target
  is a corpse, `TARGET_FLAG_CORPSE_ALLY`), and a warlock's soulstone.

Each spell action also carries its **kind**, read from its first rank's effects: `Healing` (heals, HoTs, absorbs),
`Rankable` (a healing chain with more than one rank), `DirectHeal` (a heal on one unit and nothing else), `Defensive`
(a damage-taken reduction, immunity, split damage or avoidance buff under five minutes), `LongBuff` (a positive buff
of ten minutes or more) and `KeepsAura` (a HoT, absorb, long buff or defensive kept up on one unit, not stacking).
`Layout::BuffGroups` joins the long buffs a unit can only have one of (chains sharing a `spell_group`, such as the
blessings, or Fortitude and Prayer of Fortitude).

**Friendly targets.** A positive spell that takes a unit target (a heal, shield, HoT, blessing, Hand, Power Infusion,
Innervate) is cast on the **support block's selected friend** (`Encoding::SupportTarget`): the bot itself, the owner or
a teammate. Without the block (stages 1, 2, 6, 7 and the travel stages) it is the bot itself, as before. There is one
action per spell whoever it lands on; the companion and party blocks no longer copy the heals per ally (they listed
every heal twice, and the policy could not see what was already on the owner).

**Rank tiers.** A `Rankable` heal is cast at the seat's rank tier (`Encoding::KnownRank`): the highest known rank, or
the highest active rank about two thirds or a third of the way up the known ranks, for mana on a long fight. Every
mana spell keeps all its ranks active in the spellbook; only rage, energy and runic power abilities, paladin auras and
druid forms supersede their lower ranks (`Player::addSpell`).

Every action is masked each decision by the core's own `Spell::CheckCast`, run without casting (race, level, talent,
cooldown, GCD, power, stance, range, reagents). Beyond it, a cast that can only be wasted is never offered: a
`DirectHeal` on a friend at full health, a `KeepsAura` spell whose own aura on that friend still has more than a quarter
of its duration (or charges) left, and any positive unit-target spell while the selected friend is dead or gone. A
spell whose only effect is a control aura on its caster (Grovel, from the hidden GENERIC skill every character has)
is not in the catalog at all. As on a client, **no spell or item can start while a cast is in its cast
time**. The core only enforces that for client casts, and without the check a bot's new cast would silently replace the
current one.

Casting goes through `ApplySpellAction`, which builds the `SpellCastTargets` a client would send for the spell.

**Pacing** (`Actions.*`, `SeatMemory`, the same for forge seats and live companions). A decision comes every
`DecisionMs` (250 ms), and a policy free to act on every one re-issues orders no
player would: a duel's warlocks sent their pet in 125 times an episode and started and stopped the same cast
over and over while never engaging. So the scenario masks, on top of every block's own checks, an action pressed too
recently: the same action again within `Actions.RepeatMs` (1000 ms; `Actions.MoveRepeatMs`, 300 ms, for movement
orders, so steering stays responsive), stopping a cast before it has run `Actions.StopCastMinMs` (500 ms), and
starting a spell the bot stopped itself within `Actions.RecastAfterStopMs` (2000 ms). Spells keep their GCD and
cooldowns as well. One lock keeps a plan from dissolving into dithering: a stance, form, presence, aspect, aura,
seal, armor or pet stance holds `Actions.ModeLockMs` (5000 ms) before another change of its kind (warrior tanks
changed stance 22 times a fight, hunters their aspect 12). The reverse-move lock went with the target-relative moves
it paced: a held key has no toward or away. A paced action a policy sends anyway does nothing. `actions_per_minute` in the episode info shows how busy a
seat was.

**Repeats** (`Actions.Repeat`, every stage). Pacing caps how soon an action can be pressed again, not how often: a
policy can still press one button every second for a whole episode (a duel's warlocks gave their pet 93 orders an
episode while it dealt 1% of their damage). Each press of an action counts that same action's presses within the last
`Actions.RepeatWindowMs` (10 s); past `Actions.RepeatFree` (3) of them, each press costs `Actions.Repeat` (0.02).
Movement orders are always free. How often the seat acts overall is not charged, only the same action over and
over. `repeated_presses` in the episode info
counts the charged presses.

## 4.4 Blocks

| Block | Observation (summary) | Actions |
|---|---|---|
| `core` | globals plus five durative-action clocks (see below), then 8 features per catalog action (known, cooldown, aura on target, aura on self, stacks, time since the seat pressed it, ready -- it would start if pressed now -- and affordable), then rank / max rank per class talent, then each tree's share of the points spent | The catalog |
| `move` | Whether it is moving and how fast; its facing and pitch as sine and cosine; **the controls it holds** (forward/back, strafe, ascend/descend, the turn and pitch rates, walk); **what its body is doing** -- velocity in its own frame, how much of what the keys asked for it got (the stuck and sliding signal), the controller's mode (ground, falling, swimming, flying), how long and how far it has been falling, whether it met a wall or a slope too steep, how deep it stands in water, and whether a jump would do anything; the bearing and distance to the target, all zero without one; the bearing, distance and width of the nearest ground effect it is not standing in; the objective's bearing and its distance twice, over 500 yd and again over 40 so the last few yards are resolvable; **how far the ground runs along each of sixteen rays**, each marched at 6, 12, 20, 30 and 40 yd, reporting the distance to the first thing that stops it, what stopped it, and whether it was water or something that burns; water -- in it, under it, how long under it, and how fast it swims; the detour the ground costs, whether its legs are getting anywhere, its clearance and the way out; and **a trail of where it has been** -- its last eight positions, one a second, each as an offset in its own frame, and how many of them it is still standing on | 25 held keys and mouse rates (above): forward, back and stop; strafe left, right and stop; nine turn rates from 360 degrees a second right to 360 left, stop among them; five pitch rates; ascend, descend and stop; jump; walk. The player controller moves the body with the client's physics: it is the only way a seat moves |
| `duel` | Distance and bearing to the target, behind it, it faces the bot, its combat, target and casting state; the bot's movement, combat, stealth and auto-attack; damage taken last step; pet out, health, attacking; combat time; current cast progress and time left; a cancellable form; potions, healthstones and bandages carried and their cooldowns; Recently Bandaged; can resurrect itself; a hidden target, time since it was seen, and distance and bearing to where it was last seen; the target in line of sight; what the target is (creature type one-hot, max health against the bot's, damage multiplier, the share of the bot's hits its armor takes off, run speed, level difference, immunity to six magic schools and to fear, stun, root, snare, silence and polymorph); the bot stunned, feared or confused, rooted, silenced, snared; hunters' stable families and pet types | Start attack, pet attack, stop casting, cancel form, healing potion, mana potion, healthstone, bandage self, soulstone self (warlock), resurrect self, 4 call-beast actions (hunter). No movement: where to stand in a fight is the move block's held keys, chosen against the target's bearing and distance reported here |
| `pet` (hunters, warlocks, death knights, mages; empty for others) | The pet's presence, health, power, distance to the target, attacking it, casting, stance, following or staying; what it is (a ferocity, tenacity or cunning beast, an Imp, Voidwalker, Succubus, Felhunter or Felguard, a ghoul, a Water Elemental); whether it leaves on its own and how soon; its four most useful abilities (interrupts, then crowd control, dispels, threat, help, damage): present, on cooldown and what each does | Cast each ability (at the target, or on itself when helpful) as the pet bar does; passive, defensive, aggressive; follow; stay |
| `pack` | Living and in-combat enemy counts; 24 enemy slots (`PACK_SLOTS`, the enemies seat set; present, alive, health, distance, bearing, behind, attacking the bot or its pet, casting, in combat, crowd-controlled, current target, elite, level difference, in line of sight); (the tactical spells are core actions, cast at the selected enemy) | Select target slot 1-24 (a pointer over the set); hold an interrupt |
| `gauntlet` | Pulls cleared, pull active, time since the last fight, time into the pull, elite or higher-level pull, eating, drinking, food and drink left, time until an unengaged pull comes to the bot, time until the next pull spawns (the sustain spells are core actions) | Eat, drink (offered only where the item's cast check passes) |
| `compass` | The objective's bearing and distance, when the stage gives one (withheld more often each rung of the seek ladders) | none |
| `vision` | The camera: what a ray-cast third-person view sees, as semantic pixels, with the objective flag when it is in line of sight | none |
| `entities` | The camera's entity list: what is in view, with each unit's nameplate | none |
| `map` | The mental map: a 48 x 48 heading-up crop at 2 yd of what its own camera and body have written | none |
| `sight` | The seen and remembered entity list and its presses, sent as a client sends them: select, target, interact (no looting) | select, target, interact |
| `party_frames` | Each party member's frame (alive, health, power, in combat, the leader, in the frame's range, debuffs, aggro, selected, focused), its minimap dot within 60 yd and its target | select, focus and assist presses on each frame |
| `combat` (revision 1) | The player frame, the pet frame and the target frame (threat, casting), with each visible unit's nameplate combat columns | the client's own presses |
| `goal` (last) | Which goal kinds and targets exist, and whether the goal held has ended | none |

The core block's globals are five durative-action clocks -- resting, the held interrupt, the held bearing, the
turn (`Options.MoveTurnMs` a 45-degree step, so it runs out as the turn does) and the pitch (`Options.MovePitchMs` a
30-degree step); the last two run alongside the feet rather than instead of them, so they have slots
and clocks of their own -- and these features: level; race one-hot (10); the aptitude vector (Aptitude::COUNT: what the build can taunt,
mitigate, heal, control, buff, cleanse, protect, revive, summon and swim with, and where its points went);
health; mana; rage; energy; runic
power; six runes; combo points; form one-hot (13); GCD; casting; queued next-swing; main-hand, off-hand and ranged
swing timers; main-hand speed; target health; target distance; in melee range in front; attack power; spell power;
melee and spell crit; melee and spell haste; melee and spell hit; expertise; armor penetration; last-step damage;
last-step power change; time into the episode (/ 5 min, `EPISODE_TIME_SCALE_MS`); and what the seat has been doing
(`SeatMemory`): time since its last movement order, time since its last stance, form, aspect, aura, seal, armor or
pet stance change, and its own and its target's health
against their average over the last few seconds. All are normalised (see
`Blocks/CoreBlock.h` for the scale of each). The episode time is elapsed time, not the share of the limit left: a
companion has no limit, and without a clock a bot standing still out of combat sees the same row every decision, so a
deterministic policy can repeat a loop forever. A policy sees one observation at a time: without the memory features it
re-decided from scratch every decision, running in and backing off by turns and dancing between stances. A forge seat
and a live companion keep the same `SeatMemory`, so a model plays with what it trained with.

Movement and casting constrain each other: movement actions are masked while casting, and cast-time or channelled
spells are masked while running. The bot turns to face its target whenever it isn't running. Stop casting and cancel
form need no target, so they stay available between pulls.

**The move block needs no target at all, and it is the only way a seat moves.** The duel block's movement used to be
target-relative -- `MOVE_TO_TARGET`, `MOVE_TO_RANGE`, `BACK_OFF`, `KEEP_RANGE`, `STAY_ON_TARGET`, `STOP` and
`BREAK_LINE_OF_SIGHT`, each a position the pathfinder chose and walked to -- and it is gone: where to stand in a
fight is held keys chosen against the target's bearing and distance, learned rather than ordered. Strafing and
backpedalling are keys of their own, so the seat can move one way and look another, as a player does.

**Water.** The move block reports water twice: `OBS_WATER_FIRST`, eight bearings beside the ground probe, saying
what lies that way before the seat is standing in it; and `OBS_IN_WATER`, `OBS_SUBMERGED`, `OBS_SUBMERGED_TIME` and
`OBS_SWIM_SPEED` for water it is already in. Three things had to be fixed before any of that meant anything, and
each was hiding the one under it (animus-lib `eb1f91c`):

- **`GroundReach` called water a wall.** It looks for ground within `MAX_STEP` either side of the seat's own
  height, and a lake bed is well below that band, so `GetHeight` returned `INVALID_HEIGHT` and the probe reported
  reach 0 -- the same answer it gives for a cliff or the edge of the map. The seat was being taught that the one
  route it might swim was impassable.
- **A seat could not get in.** The three-dimensional steering is reached through `Airborne()`, which is
  `IsInWater() || CanFly()`, so a seat could swim only once it was already swimming; entering was a pathfound
  ground step, and mmaps drops the terrain under real liquid, so the walkable mesh stops at the waterline and the
  step had nowhere to land. Sampled across a whole run, every seat near water sat 0.1 to 0.4 yards above the
  surface. Entering is its own case now (`Encoding::SwimTo`: straight in, no pathfinding, no fly flag -- and what a
  seat already in the water uses, since the old path called `FlyTo` and a swimmer is not a flier).
- **`Player::IsInWater()` could never be true for a bot.** `Unit::IsInWater` reads the map; `Player` overrides it
  to return the cached `m_isInWater`, and the only caller of `SetInWater` in the core is the movement opcode
  handler. A sessionless bot sends no opcodes, so the flag was false for the entire life of every bot the sim has
  ever run, and those four water observations were inputs that never changed. `ObserveSeat` keeps that state from
  the map now, once a decision per seat -- the client's job, on a server that has no client.

`swim_seconds` was 0 in every episode of every run before this, which is not a choice a near-random policy makes a
quarter of a million times. It is reported and never gated: every crossing the water arena places has a dry way
round by construction, so where that way round is quicker, walking it is the right answer and a floor on swimming
would punish it. `crossing` keeps its floor, because what the arena offers is the ground's business.

A bearing is held rather than stepped, so the resolution of the path comes from `AnimusForge.TicksPerDecision`
(2.3, 8.1) rather than from deciding more often: the world walks the spline in however many ticks a decision is cut
into, and the policy still chooses once per `DecisionMs`.

Anyone in the air without flight (a dismount, a cast that took the mount away) falls to the ground with a player's fall
damage (`MoveFall`, `Player::HandleFall`).

With the pack block, every spell, movement and pet action aims at the **selected enemy**. When it dies, the nearest
living enemy becomes the selection.

## 4.5 Encounters

An `Encounter` owns one part of what an env contains besides the seats. It keeps its own per-env state and has hooks
for each phase: `RewardTerms`, `AddEpisodeInfo`, `ResetEpisode`, `BeforeRebuild`, `Build`, `UpdateEnemies`, `Update`,
`SelectTarget`, `OnSeatAction`, `View`, `BeforeRewards`, `Reward`, `AfterRewards`, `WriteState`,
`IsTerminal`, `OnRecovered`, `OnPullStarting`, `Deactivate`, `Teardown`.

**`SightEncounter`** (`Opposition::Sight`, `move1_controls`). An empty Stockades: the seat at a random hallway point
(`SightPairs`, each played with the compass and without), one real object in sight, reached as fast as possible and
stopped beside (`ObjectiveRadius`), the compass withheld more often each rung of the ladder.

**`SeekEncounter`** (`Opposition::Seek`, `move2_seek`). One real object hidden in one of the dungeon's rooms (a ladder of
room depths on the fade's rungs), found by sight and stopped beside (`SeekRadius`, 3 yd). No compass.

**`InteractEncounter`** (`Opposition::Interact`, `move3_interact`). An empty Deadmines: the object the goal names among
decoys of other kinds (distinguish), behind a door whose lever opens it (switch), or behind the cannon the seat's
Defias Gunpowder opens (key). Paid as Arrive and DoorOpened, WrongObject a Cost.

**`PartyFollowEncounter`** (`Opposition::PartyFollow`, `move4_follow`). A leader in the owner's slot walks the dungeon's
route from the door through each boss's place, its keys on the player controller; four learned followers keep 3-10 yd
from it, out of its way, regrouping at its stops. The leader is a scripted walker and not dead code: it is the thing
the followers are rewarded for keeping up with. A follower that dies rises at the entrance (`EntranceRespawn`).

**`CombatEncounter`** (`Opposition::Combat`, `combat1_fight` to `combat3_survive`). Creatures on a cleared Ragefire
Chasm: one at a time (with a passive friend to taunt off and heal), packs of 2-4 (casters, linked, fire underfoot), then
packs that can kill with food and drink between them. A death rises at the entrance after `Respawn.DelayMs`.

**`RolesEncounter`** (`Opposition::Roles`, `group1_roles`). A proper party of five on the cleared dungeon, one role drilled
an episode in seat 0 (hold, keep up, discipline, pull), the rest a real party with co-op partners and the "human"
stand-in in a share of the episodes.

**`InstanceEncounter`** (`Opposition::Instance`, `group2_corridor` and the dungeon stages). A real dungeon's own ground,
a fresh instance a run with every pack and boss where the world database stands them. A wing ladder
(`WingLadder`, `InstanceLadder::Wing`) lifts the levels above the band and spares wipes, stepping on the probes alone.
A collapse alarm (`StageScenario::NoteWingRun`) writes to the run's `events.log` when a rung's score falls away, and
when it clears.

**`PartyEncounter`** (`PartyGroup = true`). Every episode the leader and the active seats form a real core `Group`
marked as a sim group (`CoreHooks::MarkSimGroup`), so party buffs, auras and every "party member" check work as in
play. It is disbanded before its members are replaced (`BeforeRebuild`).

## 4.6 Rewards

### The ledger

Each seat has a `RewardLedger`. An encounter adds `(term, value)` pairs, and the ledger sums the decision's total and
each term's episode total. Every term that any encounter of the stage pays becomes an episode info column
`reward_<term>`, so TensorBoard shows exactly what the stage pays for.

Terms: `damage_dealt`, `damage_taken`, `step_cost`, `casting`, `approach`, `stealth_opener`, `stealth_utility`, `kill`,
`clear`, `health_kept`, `death`, `threat`, `teammate_damage_taken`, `teammate_healing`, `teammate_threat`,
`teammate_death`, `revive`, `progress`, `arrive`, `timeout`, `stall`, `readiness`, `self_healing`, `goal_reached`,
`goal_switch`, `goal_progress`, `opener_damage`, `repeat`, `jitter`, `aimless`, `effort`, `fidget`, `hazard`,
`healing_mana`, `boss_progress`, `combat_clock`, `ranged`, `pet_tank`, `pull_clean`, `early_pull`, `drill_hold`,
`drill_focus`, `drill_keep`, `pull_extra`, `facing`, `stuck`, `wall`, `follow_kept`, `lost`, `sighting`, `new_ground`,
`room_seen`, `door_opened`, `wrong_object`, `regroup`, `blocking`, `survived`, `interrupt_landed`, `away`, `hurt`,
`fire_hurt`, `ready_pull`, `idle` (`RewardTerm`).

**Looking after itself and its friends (every stage).** `self_healing` pays `Support.SelfHealing` (0.5) times the
bot's effective healing on itself plus what its own absorbs soaked and its own damage-taken reductions prevented on
itself, as a fraction of its health. It is below every stage's damage taken weight, so a heal recovers part of what the
hit cost and being hit to heal it back never pays. On the owner and teammates, healers are paid `owner_healing` and
`teammate_healing` for healing and protection alike. Absorbs are tracked by polling the bot's own absorb auras on each
friend every decision (what they lost, or what was left when one vanished early); prevented damage is
`damage * (1 / multiplier - 1)` over the bot's own `MOD_DAMAGE_PERCENT_TAKEN` auras on the victim, at the hit
(`EnvPool::RecordPrevented`). In gauntlets, engaging a pull also pays `Support.BuffCoverage` (0.3) times the share of
the layout's buff groups up on the bot (averaged with the owner's where there is one).

**Goals** (`SeatGoal`, every stage whose policy has a goal head). The learner's goal head picks one of the goal kinds
every `mappo.goal_every_decisions` and keeps it until the next choice, and sends it to the sim with the actions. The sim
pays `Goals.Reached` for a goal reached and charges `Goals.Switch` for each change (`goal_reached`, `goal_switch`,
`goal_progress`). Columns: `goal_<name>_share` per goal and `goal_changes`; the learner's own metrics add
`goal_<i>_share` and `goal_kept_share` per update. The critic is goal-conditioned, so the advantage a decision gets is
measured against what that goal is worth.

Support columns (every stage): `healing_done` and `protection_done` (fractions of the bot's health), `overheal_share`,
`heals_on_full` (masked: 0), `defensive_casts`, `healing_casts`, `downranked_share`, `low_health_seconds` (any friend
below 35%); gauntlets add `buff_coverage` at engage.

### Scales

- **Decision scale.** Per-decision terms (step cost, threat, follow) are tuned for a 50 ms decision and multiplied by
  `DecisionMs / 50`, so they mean the same per second at any decision interval.
- **Damage scale.** `DamageScale(level) = 15 * e^(0.068 * level)` (about 16 at level 1, 230 at 40, 3500 at 80), so
  damage features and rewards have a similar size at every level. Pet, guardian and totem damage counts for the owner.
- **Health fractions.** Damage dealt is a fraction of the opponent's (or the pull's total) health, and damage taken a
  fraction of the seat's own maximum health.
- **Tier scale.** On a ladder -- the duel's tiers, the pack's and raid's rungs, the endurance run's pulls -- the
  outcome terms scale with the rung: a win (kill, clear, health kept) is multiplied by `1 + Difficulty.TierScale x
  tier` and a loss (death, timeout, overtime) divided by it. A tier-0 fight is unchanged; at tier 6 and the default
  0.25 a kill pays 2.5x and a death costs 0.4x. Evaluations spread their seeds over every tier while training climbs
  per class, so with flat terms the score fell as the ladder rose -- every rung-6 loss cost as much as a rung-0 one
  -- and convergence read the fall as done. Scaled, the break-even win rate falls with the tier, a hard fight is
  worth attempting, and the score is comparable across rungs. The tier is in the critic's state (4.8). Fixed-bonus
  opponents (evade, hide, stealth) are not a ladder and stay flat; the `difficulties` group of `eval.jsonl` is where
  the per-tier win rates are read.

### Fighting terms (defaults)

**One-on-one** (duel, PvP, escort duel; `Duel.*`, `Casting.*`):

- per decision: damage dealt x2, damage taken x1, potential-based approach shaping toward the spec's range (melee
  3.5 yd, ranged 25 yd; 0.5 per 40 yd closed), step cost 0.0002
- stealth: +0.5 for a harmful spell cast from stealth that breaks it (Ambush, Garrote, Cheap Shot, Pounce, an attack
  out of Shadowmeld; it can't be repeated without earning stealth back), +0.05 for one that keeps it (Sap, Distract,
  Premeditation), once per target per stealth so it can't be farmed
- casting: -0.03 per second already spent on a cast-time spell that didn't finish, +0.03 per second of cast time for
  each one that finished in combat (channels pay through their ticks), and -0.05 for each cast the seat cut short
  itself (the stop-casting action, or moving out of its own cast), however little of it had run, so a start/stop loop
  costs more than an episode can earn. An enemy's interrupt costs only the seconds lost
- kill: +10, plus up to +1 for the share of the episode length left since the fight was engaged (the bot or its
  opponent entered combat), plus up to +0.5 for the share of health kept (damage taken is already charged as it happens, so a
  larger share would pay for surviving over winning). The approach, stealth and preparation before engaging
  cost only the discount
- death: -10 each time, including after a self-resurrection. With a self-resurrection available the seat has
  `Resurrection.GraceMs` to use it before the episode ends
- timeout (creature duel only): -10 when the episode's time runs out with neither side dead. The fight is lost, so the
  episode ends as a terminal outcome rather than a cut-off the critic bootstraps past; before it, never engaging was
  the cheapest way to lose
- stall (creature duel only): -0.08 per second the fight hasn't started once `Duel.StallGraceMs` (15 s) of the episode
  are gone. The timeout comes 900 decisions later, too far for the policy to tell standing still from closing in: at
  20M steps stage4_duel's deterministic policy stood where it spawned for the whole episode in 67 of 2048 evaluation
  fights. Preparing isn't stalling: the grace grows by the time the seat spent starting helpful spells out of combat
  (buffs, forms, stances, stealth, pet summons, conjuring; each its cast time, at least a 1.5 s global cooldown), up to
  `Duel.PreparationRefundMaxMs` (15 s), so a warlock summoning its demon or a druid shifting before the pull isn't
  charged for it and nothing has to start prepared. `preparation_seconds` in the episode info is that time, uncapped
- repeats (every stage): -0.02 per press of the same action past the free ones in its window, and only when the
  press did nothing -- a spell that started casting, an item or a pet ability is never a repeat, because a caster's
  rotation is one nuke over and over. Orders to a pet already obeying, a target selected again and a stance pressed
  twice all still count (see Repeats, 4.3)
- winning outweighs winning fast: with the kill at 10, speed at most 1 and a loss at -10, a risky fast opener only pays
  more than a sure slow win above about 97% odds (at the earlier 3, 3 and -3 it was 79%)

The combat stages (`CombatEncounter`) and the dungeon stages (`InstanceEncounter`) price the same kills, clears, deaths
and hazards through `CombatReward`, and add their own terms (the ledger's list above, each named for what it pays);
every price is in the stage's tuning keys (4.9, `Stage.<name>.*`) and, with its category (Outcome, Cost, Shaping), in
`RewardLedger.h`. The first curriculum's gauntlet, companion, party, travel and flag-match terms went with it (git tag
`curriculum-v1`).

## 4.7 No scripted baselines

There are none (principle 14): the `greedy` and `fight` policies, the `Baselines/` code and `forge run <stage> fight N`
are gone. Smoke tests use the learner (`forge fast <stage>`); `forge run` and `forge bench` use the random policy,
uniform over the unmasked actions.

## 4.8 The critic state

The centralised critic sees a class-agnostic global state of the env. `StateDim = 30 + 40 x 26 + 24 x 37 = 1958`
(`STATE_GLOBAL_COUNT` is 30 global columns, then `MAX_SEATS` = 40 seat blocks of `STATE_SEAT_FEATURES` = 26 and
`PACK_SLOTS` = 24 enemy blocks of `STATE_ENEMY_FEATURES` = 37 -- the enum in `StageScenario.h` is the source, and the
live-layout pin test fails if any of them moves).

| Part | Features |
|---|---|
| Global (21) | Episode time fraction; pull active; pulls cleared / 10; time to next pull / 20 s; elite pull; linked pull; owner present, alive, health, mana, x, y (relative to the spawn point, / 40), in combat; arena one-hot (8) |
| Per seat (4 x 26) | Present, alive, health, mana, other power, level / 80, the six-number aptitude brief, class one-hot (10), in combat, casting, x, y |
| Per enemy slot (4 x 25) | Present, alive, health, x, y, casting, elite, level difference / 5, in combat, victim is the owner, victim is seat s (4), max health against seat 0's, damage multiplier, armor reduction against seat 0, run speed, creature type one-hot (7) |

The episode time *fraction* (the share of the episode's own limit spent) appears only in the critic state, because live
play has no time limit. Observations carry elapsed episode time instead (the core block's last global feature). In
self-play, each seat's opponent is the other seat and already appears in the seat part.

## 4.9 Tuning

Every value that shapes the curriculum is a config key `<TuningPrefix><Group>.<Name>`: `AnimusForge.Curriculum.*` in
the forge and `Animus.Curriculum.*` in mod-animus. `CurriculumTuning::Visit` lists them once, for loading and for
writing. Min/max pairs are put in order on load.

| Group | Controls |
|---|---|
| `Characters.*` | High-level threshold and chance, how talent points are spent, how often a pet class starts with its pet out |
| `Party.*` | Size weights, classic makeup chance, role chances, teammate reward weights |
| `Duel.*` | One-on-one reward weights and preferred ranges |
| `Casting.*` | Cast time wasted and completed, the charge per self-inflicted cancel |
| `Actions.*` | Pacing: how soon the same action, the same movement order, a stop of a new cast and a recast of a stopped spell are allowed again |
| `Difficulty.*`, `Goals.*`, `Support.*`, `Hazards.*`, `Options.*`, `Resurrection.*`, `Respawn.*` | Ladders, goal prices, self-healing and hazard prices, durative-action clocks, resurrection grace, the respawn at the entrance |
| `Controls.*`, `Markers.*`, `Seek.*`, `Interact.*`, `Evade.*` | The movement stages' prices and ladders |
| `Combat.*`, `Roles.*`, `PartyFollow.*`, `Instance.*`, `Raid.*`, `StandIn.*`, `Stealth.*` | The combat, roles, follow and dungeon stages' prices, ladders and the stand-in |
| `Arena.<stage>.<arena>.Weight` | Arena weights (read by `StageScenario`, not `Visit`) |
| `Arena.<stage>.<arena>.MaxRung` | The arena's pinned pack rung, `-1` for none (read by `StageScenario`, not `Visit`) |

The effective values are written into `stage.json` under `tuning` and copied into each run directory. To watch a stage
in mod-animus exactly as a model trained on it, copy that run's `tuning` into `Animus.Curriculum.*`.
`animus-forge/conf/mod_animus_forge.conf.dist` documents every key.

## 4.10 Episode info

Every stage reports these **core columns** per seat:

- `damage`, `dps`, `white_damage`, `special_damage`
- `level`, `race`, `spec`, `class`, `role`, `talent_plan` (0 standard, 1 noisy, 2 random), `unspent_talent_points`,
  `equipped_items`
- `spell_casts`, `trinket_uses`
- `present` (0 for an empty party seat; ignore that row), `arena` (index into `stage.json` arenas), `opponent_seat`
- `spawn_point` (which spawn point the episode was built from) and `spawn_drawn` (which one it drew first),
  indices into the stage's or the arena's `SpawnPoints`, or into `HeldOutSpawnPoints` while evaluating.
  Equal, the first choice worked; different, that point could not build an episode and the reset moved on.
  A point drawn often and built from never is ground no episode can start on -- held-out ground like that
  is counted as control and scores nothing, which was how a stage came to be gated on two of its
  three rooms without anything saying so.
- `killed`, `died`, `time_to_kill`, `damage_taken`, `health_left`, `stealth_openers`, `stealth_utility_casts`,
  `pet_summoned`, `pet_at_start`, `pet_damage_share` (of the seat's damage, what its pets and guardians dealt),
  `pet_died`, `pet_abilities` (pet bar abilities started), `pet_orders` (stances, follow, stay, sending the pet in),
  `item_uses` (use effects of the main-hand or off-hand item),
  `opponent` (creature entry)
- what the seat did with its pet: `pet_attack_orders`, `pet_passive_orders`, `pet_defensive_orders`,
  `pet_aggressive_orders`, `pet_follow_orders`, `pet_stay_orders` (each order given), `pet_out_seconds`, and the share
  of that time the pet was attacking something (`pet_attacking_share`), set passive (`pet_passive_share`) or told to
  stay (`pet_staying_share`). A pet's abilities are the policy's to cast: its spells are learned with autocast off
- `casts_completed`, `casts_cancelled`, `cast_seconds_wasted`, `cancelled_stopped`, `cancelled_moved`,
  `cancelled_target`, `cancelled_other`
- `consumables_used`, `self_resurrections`
- how a fight ended, to tell the ways of losing apart: `timed_out` (creature duel: time ran out with neither side
  dead), `engaged`, `engage_time`, `target_health_left`, `distance_at_end`, `form_at_end` (the `ShapeshiftForm`),
  `power_left` (of the primary power), `target_evade_seconds` and `out_of_sight_seconds` (creature duel: the opponent
  evading, and engaged without line of sight to it), `target_unreachable_seconds` and `target_teleports` (creature duel:
  the opponent without a path to its victim, and put beside it for that), `actions_per_minute` (actions other than the
  no-op), `repeated_presses` (presses charged by `Actions.Repeat`), `turn_reversals` and `bearing_flips` (steering
  charged by `Actions.Jitter`)
- how the seat fights, to grade a spec's playstyle (they reward nothing):
  - `melee_damage_share`, `shot_damage_share`, `spell_damage_share`: the seat's own damage by the game's damage class
    (`SpellInfo::DmgClass`), as shares of all its damage, so with `pet_damage_share` they add up to 1. Melee is melee
    swings and melee abilities (Raptor Strike, Sinister Strike), shots are ranged weapon attacks (Auto Shot, Steady
    Shot, a wand) and spells are the rest, DoTs included. The damage hook doesn't say which spell dealt a hit, so the
    library notes the spell in `ModifySpellDamageTaken` and `ModifyPeriodicDamageAurasTick`, which run just before it
    for the same attacker and victim. Spell damage it can't match counts as a spell
  - `in_melee_share`, `target_on_pet_share` (one-on-one arenas): the share of the fight, engaged with the seat alive,
    it spent within melee reach of the opponent, and the share the opponent spent attacking its pet or guardian. A
    hunter's shots can't be used inside melee reach (`SPELL_FAILED_TOO_CLOSE`), so for a hunter `in_melee_share` is
    the share of the fight it played melee. For a caster it is mostly where the opponent chose to fight
  - `target_rooted_share`, `target_snared_share`, `roots_applied`, `snares_applied` (one-on-one arenas): the share of
    the same time the opponent spent rooted (Frost Nova, Entangling Roots) or slowed (Concussive Shot, Wing Clip,
    Frost Shock, Earthbind) by the seat, its pet or its totems, and how often one went on where there was none
  - `feign_deaths`, `feign_death_resets` (one-on-one arenas): how often the seat feigned death, and how often its
    opponent then evaded home at full health (within 3 s of the feign ending) because nothing else held it. With a pet
    on the opponent, feign death hands the fight to the pet; without one it throws the fight away

Encounters then add their own columns:

- the pack and combat columns: `kills`, `pack_size`, `linked`, `pulls_cleared`, `food_used`, `drink_used`,
  `sustain_casts`, `deaths`, `wipes`, `control_seconds` and the gauntlet block's pacing columns
- party: `seat`, `teammates_died`, `teammate_damage_taken`, `teammate_healing`, `threat_on_teammates`
- the movement, seek, interact, follow, roles and dungeon encounters add theirs (arrival, objects found, doors opened,
  distance to the leader, drill outcomes, bosses killed, `bar_clear`)

The authoritative list is the `table.Add("...")` registrations themselves, under
`src/Scenario/Curriculum/`; `tests/test_gates.py`'s `sim_episode_info()` extracts exactly those, which is why a
config that gates on a column the sim never emits fails the test suite rather than five hours into a queue.

Then come the `reward_<term>` columns. Columns of encounters an episode's arena doesn't use read 0. The exact list for a
stage is `episode_info` in its `stage.json`.

## 4.11 Stage by stage

The live stages are described in the notes at the head of this chapter; each stage's arenas, ladders and rewards are
in its `Stages.cpp` definition and its encounter (`Encounters/`). The first curriculum's stage-by-stage notes went with
it (git tag `curriculum-v1`).
