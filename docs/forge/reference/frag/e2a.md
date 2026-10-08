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
