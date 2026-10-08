## E2a.1 PartyFollowEncounter (M4 `move4_follow`; `PartyFollowEncounter.h` 182 lines, `.cpp` 738 lines)

`Opposition::PartyFollow`. Four learned followers (`ArenaDefinition::PartySize` = 4) keep with a leader that walks the
dungeon's boss route. Nothing is fought. The leader lives in the **owner's slot** of the scenario
(`StageScenario::OwnerAgent()`, `BuildOwnerSeat`, `ReleaseOwnerSeat`); this encounter is the only user of that slot
(`PartyFollowEncounter.h:43-46`). The party has no core group (no `PartyEncounter`; `PartyGroup` must be false,
`Stages.cpp` `ArenaProblem`), so the frames the followers see are built by this encounter (`View`).

### Types and API

- `static InTheWay(leaderX, leaderY, leaderYaw, leaderMoving, x, y, yards, halfAngle)` (`.cpp:60-76`): false if the
  leader is not moving; false beyond `yards`; true within 0.25 yd; else true when the bearing off the leader's facing
  is at most `halfAngle` degrees (angle difference wrapped with atan2(sin, cos)).
- `static RegroupShare(seconds, window)` (`:78-83`): `clamp(1 - seconds / window, 0, 1)`; for `window <= 0` it is 1 only
  when `seconds <= 0`.
- `static StopSeconds(rung, first, last)` (`:85-89`): linear from `first` (rung 0) to `last` (rung 3) with
  `t = min(rung, 3) / 3`.
- `RUNGS = 4`. `IsCast(env)`, `HasLeader(env)` accessors (used by `StageScenario` for the cast row).
- Private: `enum Phase {Stopped, Walking, BackStep, Done}`; `SeatFollow` (per follower: `RespawnClock Clock`, `InBandMs`,
  `LostMs`, `BlockingMs`, `DistanceSum`, `Samples`, `RegroupPending`, `RegroupStops`, `Regroups`, `RegroupMsTotal`,
  last-reward/stuck/wall stamps, last position); `EnvParty` (`Built`, `Cast`, `Rung`, `Entrance`, `Stops`, `NextStop`,
  `Way`, `Mode`, stop timers, counters, fall tracking, `Seats[GROUP_SEATS]`). `_routes` is a `std::map<mapId, stops>`
  guarded by `_routesLock` (the only cross-thread structure: envs on different map threads call `RouteStops`).

### Lifecycle

1. **ResetEpisode** (`:206-213`): `party = EnvParty()` but `Built` is kept (the leader's character stays until `Build`).
2. **BeforeLevel** (`:215-239`): for the arena's map, finds the `FollowBosses()` row (`InstanceBosses.cpp:113`) and sets
   `EpisodeLevel = urand(low, high)` from `InstanceEncounter::DungeonLevels(row)` capped at `DEFAULT_MAX_LEVEL`;
   `DungeonDifficulty = RaidDifficulty = 0`; `EpisodeTeam = urand(TEAM_ALLIANCE, TEAM_HORDE) + 1` (one faction for the
   whole party and the leader, so nobody reads another as a hostile player); if the map has an entrance area trigger
   (`sObjectMgr->GetMapEntranceTrigger`), `EpisodeSpawn` is its target and `HasEpisodeSpawn = true`.
3. **Build** (`:241-287`): needs seat 0's bot and the map. Releases last episode's leader (`ReleaseOwnerSeat`), resets
   the state, `ObjectPool::ClearOwn(map)` (removes the dungeon's own gameobjects: doors, levers, chests, so the script is
   never stopped by a shut door), `Rung = min(SightDraw::Rung(ShapingScale()), 3)` (the fade's rung; evaluations play
   the training rung), `Cast = !env.Evaluating && CastShare > 0 && roll_chance_i(CastShare)`, `Entrance =
   SpawnPointFor(env)`. `Stops = RouteStops(mapId)`: the spawn position of each boss row of `FollowBosses()` with
   `row.MapId == mapId`, via `InstanceEncounter::FindSpawn`, in table order, cached per map. No stops -> `LOG_ERROR`,
   build fails. Builds the leader with `BuildOwnerSeat(env, map, level, Entrance, AptitudeDemand::Anything())` (any
   class), gives it seat 0's faction, `env.Allies = {leader}`, `Mode = Stopped`, `StopUntilMs = now + 3000`
   (`START_PAUSE_MS`), records the leader's controller-stuck baseline, and schedules the first sudden stop.
4. **Update** (per decision, `:458-517`): `RespawnFollowers` first; then if there is a leader in the world:
   - a dead leader (a fall in Ragefire's cavern) is stood up where it fell after `LEADER_RISE_MS = 2000` through
     `RiseAtEntrance(leader, seat, here, now)`, `LeaderRises++`, and nothing else happens that decision;
   - a hurt leader is set to full health every decision;
   - drops: a fall (z below the previous z by more than 0.5) is tracked; on landing, `FallFromZ - z >= 4` (`DROP_YARDS`)
     adds one to `Drops`;
   - a **cast** leader (a frozen checkpoint plays the row) only advances `NextStop`/`StopsReached` when within 3 yd
     (`LEADER_ARRIVE`) of the next stop; it keeps no stop pauses and never counts regroup stops;
   - a **scripted** leader runs `Steer`.
5. **Steer** (`:344-433`): the leader's keys are the script (the only scripted actor left in the M4 stage; see
   principle 14 and the note below).
   - Stopped and `now >= StopUntilMs`: `Mode = Walking` (or `Done` if no stops left).
   - Walking with a sudden stop due (`NextSuddenMs`): `SuddenStops++`; duration `urand(SuddenStopMinMs, SuddenStopMaxMs)`;
     if `Rung >= BackStepFromRung` and `roll_chance_i(BackStepChance)` it first backs `BackStepYards` along its reversed
     facing (`Mode = BackStep`, then `Stop` on arrival within 1 yd or after `GiveUpMs` stuck), else `Stop(ms, counts)`
     at once; the next sudden stop is rescheduled.
   - Walking: reaching the stop (2-D within 3 yd) or being stuck `GiveUpMs` on the leg advances `NextStop`; arrival
     counts `StopsReached++` and stops for `StopSeconds(Rung, StopSecondsFirst, StopSecondsLast)` seconds; giving up
     counts `Skips++`. After the last stop `Mode = Done` (the leader stands there for good).
   - Keys: none unless Walking/BackStep and the controller `Started()`. Walking plans with `RoutePlanner::Instance().Plan`
     (navmesh corners; a replan when `Way` is invalid or its end is over 1 yd from the target, or the leader strays over
     15 yd, `LEADER_STRAY`, from the next corner), then `Movement::Seek(body, x, y)` gives the `ControlState`;
     `held.Walk = Rung < WalkRungs || Mode == BackStep`. `seat.Controls.Held = held` keeping `FaceTurnApplied`.
   - `Stop(counts=true)` with `ms >= RegroupMinStopMs` increments `RegroupStops` and marks every living, not-out
     follower `RegroupPending` (counting `RegroupStops` per follower). Sudden stops count as regroup stops too.
6. **RespawnFollowers** (`:435-456`): per active seat < 5, `Clock.Note(now, alive, PartyYards, Respawn.DelayMs,
   Respawn.RejoinYards)`; on `Died` clears `RegroupPending`; on `Rise` `RiseAtEntrance(bot, seat, party.Entrance, now)` and
   `Clock.Risen`. See `EntranceRespawn.h` ([EntranceRespawn section of this document]).
7. **SelectTarget**: always sets `target = nullptr` and returns true (never reached in M4 since the stage has no sight
   block? M4's blocks have no `Sight`, so this runs: no target, nothing to fight).
8. **View** (`:556-599`): the cast leader's row gets `HasObjective` = there is a next stop and `Objective = Stops[NextStop]`.
   A follower gets no objective; `MinimapYards = PartyFollow.MinimapYards`; its `Frames` are filled by
   `PartyFramesBlock::FillFrame`: slot 0 the leader (`leads = true`), then the other followers in seat order (up to
   `GROUP_MEMBERS` slots).
9. **Reward**: below.
10. **WriteState** (critic only): `STATE_TIER = Rung / 3`; leader present/alive/health and its x, y relative to the door in
    the owner's state columns.
11. **IsTerminal**: always false (the episode runs to its clock; no death ends it).
12. **Deactivate/Teardown**: release the leader seat, `Built = false`, `env.Allies.clear()`.

### Rewards (per follower, `Reward`, `.cpp:601-700`)

`RewardTerms()`: FollowKept, Regroup, Lost, Blocking, Death, Stuck, Wall. Reads `PartyFollow.*` and `Seek.Stuck/Wall/WallSlide`.
`stepMs` = time since the seat's last reward call; only seats < 5 and active.

| Term | Kind | Condition and formula | Keys (defaults) |
|---|---|---|---|
| Death | Cost | once per death while the seat is dead and `!DeathCounted`: `-Death` (via `Add`); sets tally Died/DeathMs, `Deaths++` | `PartyFollow.Death` 3.0 |
| Stuck | Cost | `ledger.AddFixed(-Stuck * stuckMs/1000)`, only while alive and not out | `Seek.Stuck` 0.02 |
| Wall | Cost | `AddFixed(-Standing::WallCharge(wallS, moved, asked, Wall, WallSlide))`, `asked = speed(kind) * DecisionMs/1000` | `Seek.Wall` 0.02, `Seek.WallSlide` 0.5 |
| FollowKept | Outcome | band 1 (distance within `BandMin..BandMax`): `+Kept * seconds` | `Kept` 0.02, `BandMin` 3, `BandMax` 10 |
| Lost | Cost | band 3 (past `LostYards`): `-Lost * seconds` | `Lost` 0.02, `LostYards` 40 |
| Regroup | Outcome | once per counted stop when a pending follower is in band: `+Regroup * RegroupShare(since, RegroupWindow)`; expires unpaid after the window | `Regroup` 0.5, `RegroupWindow` 20 s, `RegroupMinStopMs` 2000 |
| Blocking | Cost | `InTheWay(...)` with the leader's held keys moving: `-Blocking * seconds` | `Blocking` 0.05, `BlockYards` 2.5, `BlockHalfAngle` 45 |

`Standing::Band(distance, BandMin, BandMax, LostYards)` returns 0 too close, 1 in band, 2 behind, 3 lost (`Standing.h`).
After a death or while out (`Clock.Out`) the function returns before the cost terms; nothing is paid without a living
leader on the same map. Death is the only term here paid while dead.

### Episode info columns (exact names)

`follow_kept_share` (InBandMs / elapsed), `follow_distance_mean`, `lost_seconds`, `blocking_seconds`, `regroup_stops`,
`regroups`, `regroup_share` (regroups / regroup_stops), `regroup_seconds` (mean), `deaths`, `rises`, `rejoins`,
`rejoin_seconds`, `rejoined` (share of rises that rejoined; 0 when no rises), `dead_seconds`, `leader_stops_reached`,
`leader_route_share`, `leader_skips`, `leader_sudden_stops`, `leader_drops`, `leader_rises`, `leader_cast`, `difficulty`
(the rung), `at_top_rung`. Plus `reward_*` for the seven terms. `apps/forge/python/configs/move4_follow.yaml:37-93` reads
`follow_kept_share` (headline, target `>= 0.9`, `gate_metric`, `measure`), `regroup_share >= 0.9`, `lost_seconds <= 5`,
`rejoined >= 0.9` and the others in its report list. `deaths` and `rejoined` columns are also defined by other encounters
(same names): the scenario builds one column per name (first added wins; see `EpisodeInfoTable::Contains` only used for
reward columns, so duplicates in AddEpisodeInfo are UNVERIFIED: check whether a second `Add("deaths")` from another
encounter in the same stage creates a duplicate column; in M4 only this encounter is present).

### Quirks and notes

- The leader is scripted: it is the M4 exception to "no scripted players" (principle 14); its keys are `Movement::Seek`
  toward navmesh corners, and `CastShare` (default 0 percent, `PartyFollow.CastShare`) hands it to a frozen checkpoint.
- `Cast` leaders skip `Stop` entirely, so `regroup_stops` is 0 and no `Regroup` is ever paid in cast episodes.
- Sudden stops are scheduled with at least 1 s (`std::max<uint32>(1000, ...)`) and the gap is halved from
  `BackStepFromRung` (`ScheduleSudden`, `:309-321`).
- `PartyFollow` on the second episode in a new instance relies on `StageScenario.cpp:2294-2307` clearing creatures with
  `DUNGEON_CLEAR_RADIUS` 1000.
- `SelectTarget` comment says nothing to fight but is only reached when the stage has no sight block (M4 has none).
- Tests: `PartyFollowTest.cpp` (stage validity at `:428-445`; the pure helpers).

### Reviewer notes
- `Steer` mixes plan state, key generation and counters in one 110-line function; `Phase::Done` is reached from two places.
- `RouteStops` copies the vector under the lock on every `Build`.
- The follower frames are hand-built because there is no core group; any change to `PartyFramesBlock::FillFrame`'s
  contract affects both this and `FillFromGroup`.

### Observed issues
1. `PartyFollowEncounter.cpp:470-481` comment/docs say "scripted leader that died stands up where it fell" - correct, but a
   dead leader returns from `Update` before `Steer`, so a stop that was running resumes with stale timers after rising
   (`StopUntilMs` is absolute episode time).
2. `Reward` returns early for a dead/out follower after the Death charge, so `follow.LastRewardMs` advances while dead:
   no time-in-band is credited for the interval after rising (intended, but undocumented).
3. `RegroupPending` can remain true when a follower is out; cleared at `Died` (`:448`) only.
4. `.h:43-46` states the leader is "no dead code" - the leader is the sole user of `OwnerAgent`/`BuildOwnerSeat`,
   a leftover slot of the deleted owner mechanism.

## E2a.2 CombatEncounter (C1-C3 `combat1_fight`, `combat2_packs`, `combat3_survive`; `CombatEncounter.h` 183, `.cpp` 707, `CombatDraw.h` 148)

`Opposition::Combat`, one solo seat (seat 0 only: `Reward` returns for `seatIndex != 0`, `.cpp:569-572`) on a cleared
Ragefire Chasm. The three stages are three `CombatDrill`s of the same class: `Fight` (C1: `fight` weight 3 and `guard`
weight 1 with `Ally`), `Packs` (C2: `packs` weight 2, `fire` weight 1 with `Hazards`), `Survive` (C3: `survive`). All
arenas set `RespawnAtEntrance`. The seat perceives through the sight/combat blocks and selects its own target
(`SelectTarget` is not overridden; the scenario takes the seat's client selection in a sight stage). Per-env state is
`EnvCombat` (`.h:96-150`); the encounter also owns a `DifficultyLadder _ladder` (section E2a.3) and a per-map cache of
corridor start points (`_corridors`, guarded by `_corridorLock`).

### Draw: where the seat starts, what spawns (`CombatDraw.h`, `CombatEncounter.cpp:145-320`)

- **Corridor points** (`FindCorridors`, `.cpp:156-186`, once per map, `Corridors()` `:145`): every creature spawn of the
  map from `sObjectMgr->GetAllCreatureData()` sorted by spawn id (fixed order, so an evaluation's seed picks the same
  point), kept when on dry ground (`map->GetLiquidData(...) == LIQUID_MAP_NO_WATER`, body height 2) and reached from the
  entrance (where `bot` stands then) by a whole `PathGenerator` path (type `NORMAL`, not `NOPATH`) of length at most
  `Combat.CorridorWalk` (220 yd), each at least `Combat.CorridorSpacing` (8 yd) from every point already kept
  (`CombatDraw::CorridorPoints`, `.h:96-117`). The first call must be made by a bot standing at the entrance. The log line
  reports the count. `RolesEncounter` reuses `FindCorridors` statically.
- **Seat placement** (`Build`, `:188-242`): `Despawn(env)` first; `combat = EnvCombat()`; rung from
  `_ladder.Draw(env, layout, spec, Combat.MaxTier)`; the start point is `corridors[EpisodeSeedIndex % n]` in an
  evaluation, else a uniform draw; `BotFactory::TeleportWithinMap(bot, point, random facing)`; `StartWalk` is the
  straight-line distance to the entrance (episode info `start_walk`). Then `_scenario.PrepareFighter(bot, seat)`; for
  `Survive` it stocks food and drink (`ConsumablePool::Food/Drink(level)`, drink only if the seat has mana,
  `StockConsumables`); for an `Ally` arena `SpawnAlly`; `SpawnPull(env, map, bot, nullptr)` (false fails the build); for
  non-`Fight` drills a second pull is spawned beyond the first (`from = &Packs.front().Spot`); `ListTargets`.
  If there are no corridor points the seat is left where it is (no error).
- **What a pull is** (`CombatDraw::PlanPull`, `.h:45-80`), with `steps = tier * LevelsPerTier` (1):

| Drill | LevelOffset | Size | Elite | Caster | Hazard | Linked |
|---|---|---|---|---|---|---|
| Fight | `LevelBase + steps` (-2 + tier) | 1 | `tier >= EliteTier` (4) | `!Elite && tier >= CasterTier (1) && casterRoll` | no | no |
| Packs | `LevelBase + steps/2` | `min(4, 2 + tier/2)` | `tier >= 4` | `tier >= 1` | `fireRoll` | `tier >= LinkedTier (2)` |
| Survive | `LevelBase + steps + SurviveLevels (2)` | `max(1, SurviveSize (3)) + (tier >= 3 ? 1 : 0)` | `tier >= 4` | `tier >= 1` | `fireRoll` | always |

  `fireRoll = drill != Fight && (arena.Hazards || roll_chance_i(Combat.HazardChance 33))`; `casterRoll =
  roll_chance_i(Difficulty.CasterChance 40)`. Creature level = `clamp(seatLevel + LevelOffset, 1, 83)`
  (`CreatureLevel`). Note that 83 is not `DEFAULT_MAX_LEVEL` (80) used elsewhere (`Opponents`).
- **Entries** (`SpawnPull`, `:244-320`): an elite (`pool.RandomElite`) first if the plan has one, then a hazard caster
  (`RandomHazardCaster`), then a caster (`RandomCaster`), each only while there is room, the rest `pool.Random(level)`
  (C1: default-AI open-world creatures) or `pool.RandomPackMember(level)` (C2/C3); an empty draw ends the fill and
  zero entries fails the pull. **Where**: with `from` (the next pack) `Opponents::FindSpawnPointFrom(bot, map, *from,
  bearing from the seat through `from`, spread 1.0 rad (`NEXT_SPREAD`), NextNearest 30, NextFurthest 45)`; otherwise, or
  if that finds nothing, `Opponents::FindSpawnPoint(bot, map, FightNearest 28, FightFurthest 40)`. `Opponents::SpawnPack`
  summons the pack around the spot. The `guard` ally's creature gets 10 threat on the ally and `AttackStart(ally)` for
  the first pack only (`Packs.size() == 1`).
- **Ally** (`SpawnAlly`, `:322-340`): a creature from `pool.Random(level)` placed 2-5 yd from the seat, faction of the
  seat, `REACT_PASSIVE`.

### Per-tick and per-decision flow

1. `UpdateEnemies` (`:371-396`): per pack, marks `Engaged` (and `EngageMs`) once a member has a victim; for a `Linked`
   pack, every idle member attacks the victim.
2. `Update` (`:398-440`): I4 respawn first. `FellAt` is stored while dead and not yet out; `fightYards = dist(bot,
   FightPoint)` (or -1 when dead); `Clock.Note(...)`; on `Step::Rise` with `RespawnAtEntrance`: `RiseAtEntrance(bot,
   seat, SpawnPointFor(env), now)`, `Clock.Risen`, `DeathPaid = false`. If dead, stop. C1: when `NextFightMs` is due,
   respawn the ally if dead and spawn the next creature. C2/C3: top the queue up to two packs whenever fewer remain.
3. `OnSeatAction` (`:442`): remembers `PendingInterrupt` (the target the seat's interrupt was aimed at).
4. `View` (`:448-468`): `PullsCleared = Clears + Kills`; for `Survive` the gauntlet block's `FoodItem`, `DrinkItem`,
   `GauntletSupplies = CONSUMABLE_COUNT`; if the front pack fights, `ElitePull` and `PullTime = min(1, (now -
   EngageMs) / 60000)`.
5. `BeforeRewards` (`:497-567`): zeroes the per-decision counters; detects the ally's death (`AllyDeaths`); counts each
   dead member of the front pack once (C1: `Kills++`, and `KillSeconds += (now - EngageMs)` if engaged); an **extra
   pull** is counted once when the second pack is engaged while the front one still lives; when no member of the front
   pack is alive: non-`Fight` drills `Clears++`/`NewClears++`, the pack is erased, C1 schedules `NextFightMs = now +
   Combat.NextFightMs (2000)`, C2/C3 spawn a replacement beyond the back pack if the seat is alive; `ListTargets`.
6. `Reward` (`:569-689`): see the table. Also counts `Decisions`, `SelectedDecisions` (`bot->GetTarget()` non-empty),
   `InViewDecisions` (`CombatBlock::InView`), `RestMs` (a regen aura) when alive.
7. `IsTerminal` (`:698`): `EpisodeElapsedMs >= EpisodeLengthMs` only; a death never ends it. `WriteState`: `STATE_TIER =
   Tier / max(1, MaxTier)`.
8. `Teardown`/default `Deactivate`: `Despawn` all packs and the ally.

### Rewards (`Reward`, with `w = TierWeight(Difficulty.TierScale 0.25, tier) = 1 + 0.25 * tier`)

`RewardTerms()` (`.cpp:58-63`): Kill, Clear, Survived, InterruptLanded, Away, Death, TeammateDeath, Hurt, FireHurt,
PullExtra, StepCost, DamageDealt. (`CombatReward.cpp:65-73` is where the term names live; see
[cpp-rewards-routing.md](cpp-rewards-routing.md).) Kinds from `RewardLedger.h:178-250`.

| Term | Kind | Condition and amount | Key (default) |
|---|---|---|---|
| Kill | Outcome | C1 only: `Kill * w` per creature newly dead | `Combat.Kill` 1.0 |
| Clear | Outcome | C2/C3: `Clear * w` per pack newly cleared | `Combat.Clear` 2.0 |
| InterruptLanded | Outcome | C2/C3 when the seat's pending interrupt target's cast is in `env.StepInterruptedTargets`: flat `+InterruptLanded` (not tier-scaled; C1 counts `Interrupts` but pays nothing) | `Combat.InterruptLanded` 0.25 |
| Survived | Outcome | once, on the decision the clock runs out, if `Deaths == 0`: `(Survive ? SurviveSurvived : Survived) * w` | `Combat.Survived` 1.0, `Combat.SurviveSurvived` 2.0 |
| PullExtra | Cost | `-ExtraPull` per extra pull counted | `Combat.ExtraPull` 1.0 |
| TeammateDeath | Cost | the guard ally fell: `-AllyDeath / w` | `Combat.AllyDeath` 1.0 |
| Hurt | Cost | `-Hurt * (non-hazard damage taken / max health)` | `Combat.Hurt` 0.2 |
| FireHurt | Cost | `-FireHurt * (hazard damage taken / max health)` | `Combat.FireHurt` 1.0 |
| StepCost | Cost | while a front-pack member is alive and in combat: `-Clock * decisionSeconds` | `Combat.Clock` 0.01 |
| Death | Cost | once per death: `-Death / w` | `Combat.Death` 2.0 |
| Away | Cost | `-Away * decisionSeconds` when `CombatDraw::AwayCharged`: dead, or alive and further than `AwayYards` from `FightPoint` while rejoining (`Clock.Rejoining`) or while the front pack fights | `Combat.Away` 0.02, `Combat.AwayYards` 30 |
| DamageDealt | Shaping | `Damage * step.Damage / mean max-health of the front pack` | `Combat.Damage` 0.3 |

Win/loss tier scaling thus follows principle "tier-scaled outcomes": outcomes times `w`, deaths divided by it. `Hurt`,
`FireHurt`, `ExtraPull`, `Away` and `Clock` are at full price (no cost ladder: the yaml says "full price from the
start"). At the clock's end (`timeIsUp && !Recorded`) the encounter also records the window: `if (combat.Counts)
_ladder.Record(layout, spec, tier, Won(Kills + Clears, Deaths), MaxTier)`, where `Won` is something taken down and no
death (`CombatDraw.h:143-146`). Whether `Reward` is reached on the final decision (so `Recorded` is set) is
UNVERIFIED: check the order of `Reward` and `IsTerminal` in `StageScenario::Reward`/`Observe`.

### Episode info columns

`won`, `survived`, `kills`, `packs_cleared`, `pulls`, `extra_pulls`, `interrupts`, `deaths`, `respawns`, `rises`
(identical values: both are `Clock.Rises`), `rejoins`, `rejoin_seconds`, `rejoined` (`RejoinedShare` when rises),
`dead_seconds`, `away_seconds`, `outcome_paid`, `interrupt_earnings` (`InterruptPaid / OutcomePaid`), `kill_seconds`,
`ally_deaths`, `hurt_share`, `fire_share`, `hazard_pulls`, `linked_pulls`, `caster_pulls`, `rest_seconds`,
`selected_share`, `target_in_view`, `start_walk`, `combat_rung`, `difficulty` (same value as `combat_rung`),
`at_top_rung` (`Tier >= MaxTier`), and the twelve `reward_*` columns. Read by the learner configs
`apps/forge/python/configs/combat1_fight.yaml` (headline `won, survived, kills, kill_seconds, hurt_share, deaths,
rejoin_seconds, combat_rung, target_in_view, selected_share, ally_deaths`; convergence measure `won`; fade gate `won >= 0.7`
with rungs [1.0, 0.5, 0.25, 0.0]), `combat2_packs.yaml` (`packs_cleared`, `extra_pulls`, `interrupts`, `interrupt_earnings
<= 0.3`, `fire_share`; gate `won`), `combat3_survive.yaml` (convergence measure and fade gate `survived`; `rejoined >= 0.9`,
`rejoin_seconds <= 60`, `away_seconds`, `rest_seconds`). The `deaths`, `rises`, `rejoins`, `rejoin_seconds`, `rejoined`,
`dead_seconds` names are shared with PartyFollow, Roles and Instance.

### Config keys

All under `AnimusForge.Curriculum.` + `Combat.*` (defaults above and: `MaxTier` 5, `LevelBase` -2, `LevelsPerTier` 1,
`EliteTier` 4, `CasterTier` 1, `LinkedTier` 2, `HazardChance` 33, `SurviveSize` 3, `SurviveLevels` 2, `FightNearest` 28,
`FightFurthest` 40, `NextNearest` 30, `NextFurthest` 45, `NextFightMs` 2000, `CorridorWalk` 220, `CorridorSpacing` 8),
`Difficulty.*` (E2a.3) and `Respawn.DelayMs` 10000, `Respawn.RejoinYards` 15. Clamps and the conf-file agreement:
[cpp-tuning-keys.md](cpp-tuning-keys.md).

### Tests

`CombatPerceptionTest.cpp` (`CombatDrawTest.TheRungsPulls`, `OutcomesScaleWithTheRung`, `TheCorridorPointsAreReachableAndApart`,
`CombatRespawnTest.AwayIsDeadOrOffFromTheFight`, `TheClockRisesAfterTheDelayAndRejoinsAtTheFight`, `CombatStagesTest.*`);
no test drives the encounter against a world (`Build`, `SpawnPull`, `Reward` are not unit-tested); `RolesStageTest.cpp`
covers the shared stage setup.

### Reviewer notes

- C1's win is "something taken down and no death" (`Won`), the gate for the shaping fade and the DifficultyLadder alike.
- `Survived` pays only at the exact decision the clock runs out and only with zero deaths in the whole episode: a seat
  that dies once never gets it, even if it comes back and clears everything.
- Interrupt credit depends on `PendingInterrupt` being the same GUID as a caster in `StepInterruptedTargets`.
- Spawning uses `PathGenerator` per attempt (up to 24 attempts, each a path computation) on the map thread.

### Observed issues

1. `CombatEncounter.cpp:93-94`: `respawns` and `rises` report the same number.
2. `difficulty` and `combat_rung` columns duplicate each other (`CombatEncounter.cpp:127-128`).
3. `CombatDraw.h:CreatureLevel` clamps to 83 while the pools stop at `DEFAULT_MAX_LEVEL` (80).
4. `Build` silently leaves the seat where it was when the map has no corridor point (`:208-221`); `FindCorridors`
   logs only the count.
5. `StepCost` is paid under the name "Clock" in tuning (`Combat.Clock`) but is `RewardTerm::StepCost`, whereas the
   scenario's own `RewardTerm::CombatClock` (`Output.Clock`) is a different, shaping term.
6. `ResetEpisode` keeps `Packs` and `Ally` across the episode boundary (`:134-143`) so `Build` can despawn them with a
   map; a reset that never reaches `Build` (failed build) leaves creatures alive.

## E2a.3 DifficultyLadder (`DifficultyLadder.h` 100, `.cpp` 104)

An adaptive per-(class, build) rung used by CombatEncounter and RolesEncounter (`CombatEncounter.h:177`,
`RolesEncounter.h:148`; SightEncounter's comment at `SightEncounter.cpp:104` refers to it but does not use it).
`DifficultyLadder(scenario, what)` sizes `_tiers` as `Layouts().size() * MAX_SPECS` rows (`Row(layout, spec) = layout *
MAX_SPECS + min(spec, MAX_SPECS - 1)`); each row `{Tier, Fights, Wins}`; a mutex guards it ("envs finish on map update
threads"). Rungs start at 0 with the worldserver and are not persisted.

- `Draw(env, layout, spec, maxTier) -> {Tier, Counts}` (`.cpp:29-61`): an **evaluation** (`EpisodeSeedIndex !=
  NO_EPISODE_SEED`) plays `(seed / CastingCount()) % (maxTier + 1)` and never counts, so every (class, build) pair meets
  every rung. **Training** plays the current rung (capped to `maxTier`), counting; with `Difficulty.ReviewChance`
  (25) percent and a rung above 0 it plays a uniformly drawn lower rung and does not count; else with
  `Difficulty.StretchChance` (10) percent below the top it plays one rung up and does not count. (`roll_chance_i` is
  called on the world thread, per the header.)
- `Record(layout, spec, fightTier, won, maxTier)` (`:63-92`): ignored if the row's rung moved meanwhile; counts a fight;
  once `Fights >= Difficulty.Window` (200) it moves the rung up if `wins/fights >= RaiseAbove` (0.9) and below the top,
  or down if `< LowerBelow` (0.6) and above 0; resets the window and logs "<scenario>: <class> <spec> moves from <what>
  <a> to <b>".
- `Tier(layout, spec)` reads a row.

This ladder **does step back on the score**, unlike the gate-stepped fade and wing ladders in principle 10: it is the
older per-class pacing, kept for the combat and roles stages, and the learner also tracks the mean of the `difficulty`
column per class (`apps/forge/python/animus/stage.py:21,51` "the ladder settled"; see [py-mappo.md](py-mappo.md) and
[stages.md](stages.md)). Config: `Difficulty.RaiseAbove`, `LowerBelow`, `Window`, `ReviewChance`, `StretchChance`,
`CasterChance`, `TierScale`, `MaxTierScale` 6 (the last is not read by this class). Tests: no direct test file (UNVERIFIED:
grep found none for `DifficultyLadder`).

Observed: the rung counters are lost on every worldserver restart, so a resumed stage restarts every class at rung 0
(the learner's `difficulty` column then drops); `Draw` uses `urand` and the ladder state is shared by all envs of the
pool, so training order matters for reproducibility.

## E2a.4 Opponents (`Opponents.h` 98, `.cpp` 387)

The curriculum's hostile creature pools, spawn-point finder and summoner, used by Combat, Roles and others.

- **`OpponentPool::Instance()`** (a function static; built on first call, which `WarmCaches()` forces before the DB
  pools are sealed, `WarmCaches.cpp:34`). The constructor (`.cpp:111-220`) runs two `WorldDatabase` queries over
  `smart_scripts` (SmartAI creatures with `ScriptName = ''` whose scripts are only spell casts (action 11) and talks (action
  1) on a list of combat events: "cast-only"; and their spells, to find which have a cast time (an interrupt can stop
  it) and which carry a persistent area aura or area aura effect: "hazard"), then walks `sObjectMgr->GetCreatureTemplates()`.
  A template qualifies when it is spawned in the world and not a waypoint walker (`WorldCreatures::SpawnedIds`,
  `WaypointWalkerIds`), of a fair type (beast, dragonkin, demon, elemental, giant, undead, humanoid), no npcflag, no
  vehicle, not non-attackable/immune/non-selectable/pacified, not civilian/trigger/guard, `ModHealth` and `DamageModifier` within
  0.5-2.0 (elite: health up to 3.0, damage up to 2.5), nonzero `minlevel`, and not `SpawnsUnreachable` (no ground
  movement, flying, rooted, hovering/flying/submerged anim tier, stealth or invisibility addon auras), and either default AI
  (no script, no AI name) or cast-only SmartAI. Normal-rank creatures go into `_packByLevel` for every level in their
  range, into `_byLevel` if default AI, `_castersByLevel` if cast-time, `_hazardCastersByLevel` if hazard; elite-rank
  into `_elitesByLevel`. Counts are logged.
- **Draws** `Random`, `RandomPackMember`, `RandomElite`, `RandomCaster`, `RandomHazardCaster` -> `PickNear(byLevel, level)`:
  the level itself then the nearest levels either side (lower first), uniform in the bucket; 0 if everything is empty.
- **`FindSpawnPoint(bot, map, min, max)`** (`:262-300`): up to 24 tries at a random bearing and distance; keeps a spot
  with ground (`GetHeight`) within 6 yd of the bot's height, `Walkable` (a `PathGenerator` path of normal type no longer
  than 1.5 times the straight line) and in line of sight (`IsWithinLOS` at +2 z); a walkable spot out of sight is the
  fallback; otherwise the last random attempt's position (possibly unvalidated, with `m_positionZ` possibly the bot's z).
  Random facing.
- **`FindSpawnPointFrom`** (`:302-319`): as above but from a point along a bearing within `spread`, no line-of-sight
  test; `nullopt` if none.
- **`SummonOpponent`** (`:321-350`): sets thread-local `PendingSummonLevel = level` around `map->SummonCreature(entry,
  pos)` (see `SummonLevel.h`), then puts the creature in the bot's phase mask, `FACTION_MONSTER`, `REACT_AGGRESSIVE`,
  home position, full health, `SetRegeneratingHealth(false)`. Returns the `Creature*` or null with `LOG_ERROR`.
- **`SpawnPack`** (`:352-387`): the first entry at the centre, each further one 2-5 yd (`PACK_SPREAD` = 5) away at a random
  angle, ground-snapped, random facing; failed summons are skipped.

Quirks: the pool is built from the world DB at startup and never refreshed; the query's `HAVING` clause is a hand-written
description of "cast-only" scripts that nothing tests (UNVERIFIED: the world data it reads is outside the repo tests);
`PickNear` falls to other levels without telling the caller (a level-60 seat may get a level-1 pool entry if buckets are
empty); comments mention `stage1_duel`, `stage8_duel` and "the scripted baseline" (`.cpp:78-84`, `:333-341`), stale since
the first curriculum was deleted. `Opponents.h:42` (`Random`) says "The duel stage's opponents".
