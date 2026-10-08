## E2b. RolesEncounter, PartyEncounter, the stand-in and Standing

Files (all under `src/server/game/Animus/Scenario/Curriculum/Encounters/`): `RolesEncounter.h` (154), `RolesEncounter.cpp`
(727), `RolesDraw.h` (239), `PartyEncounter.cpp` (541) with the class declaration in `Encounters.h:78-141`, `StandIn.h`
(161), `StandInSeat.cpp` (169, members of `StageScenario`), `Standing.h` (69). `DifficultyLadder.h/.cpp` (100 + 104)
is the rung ladder RolesEncounter owns; it is summarised in E2b.1.5 because the rung decides the episode.

### E2b.1 RolesEncounter (G1, `group1_roles`; `Opposition::Roles`)

**What it does.** One drill an episode on the cleared Ragefire Chasm (map 389). Seat 0 is the drilled role (the stage
draws its class and build among those whose spec plays it, `StageScenario::FitsDungeonRole`, see
[cpp-stagescenario.md](cpp-stagescenario.md)); the other four seats are a proper party in a core group
(`PartyEncounter`). The encounter spawns packs of creatures for the party to fight, pack after pack until the
episode clock ends it. It pays the drilled seat's lesson as its own Outcome term, plus party-wide Clear, Survived, Death,
Away and clock terms.

**State** (`RolesEncounter.h:68-123`): per env `EnvRoles` (drill, rung `Tier`, `Counts`, layout/spec/level of the drilled
seat, `Corridor` index, `Packs`, per-seat `SeatRoles` {`RespawnClock Clock`, `DeathPaid`, `Deaths`, `AwaySeconds`},
`AllDown`, this decision's `NewClears/NewClean/NewExtra`, episode `RolesDraw::Tally`, `Pulls`, `CleanClears`, `KeptUp`,
`KeptMembers`, `LowManaSeconds`); one shared `DifficultyLadder _ladder` (name "roles rung") and a mutex-guarded
per-map cache of corridor points (`_corridors`, `RolesEncounter.cpp:179-187`).

**Lifecycle, step by step.**

1. `ResetEpisode` (`:170-177`): keeps `Packs` (the creatures stay until `Build` clears them), zeroes everything else.
2. `Build(env, map, level)` (`:190-250`; the `level` argument is ignored). Fails (false) with no seat-0 bot or map.
   a. `Despawn(env)` removes the previous episode's packs. `roles = EnvRoles()`; `Drill = Arena(env).Roles`.
   b. Rung: `_ladder.Draw(env, layout, spec, Roles.MaxTier)` for the drilled seat's class and build (E2b.1.5);
      `Tier`, `Counts`, `Level = drilled.Level`.
   c. Seat 0 is teleported (`BotFactory::TeleportWithinMap`) to a corridor point (`CombatEncounter::FindCorridors`,
      cached per map id), index `EpisodeSeedIndex % corridors` in an evaluation, else `urand` (`:206-217`), random
      facing. If no corridor points exist seat 0 stays where it is.
   d. Each other seat up to `GROUP_SEATS`: teleported to `Opponents::FindSpawnPoint(lead, map, Roles.PartyNearest,
      Roles.PartyFurthest)` (2 to 6 yd, in seat 0's sight), random facing; every seat `PrepareFighter` and is stocked with
      food, and drink if it has mana (`StockConsumables`, `ConsumablePool`).
   e. First pack `SpawnPack(env, map, lead, nullptr)` (fail = build fails), then packs one after another from
      `roles.Packs.back().Spot` until `StandingPacks(drill, tier)` stand; `ListTargets` fills `env.Targets`
      (first `MAX_TARGETS` pack members).
3. `UpdateEnemies` (per decision, `:347-380`): per pack it reads whether any alive member is in combat (`Fight.Fighting`),
   first engagement stamps `EngageMs` and counts a `Pulls`; a `Linked` pack sends its idle members to attack the victim
   of a fighting member.
4. `Update` (per decision, `:382-424`): for each seat up to 5, `RespawnClock::Note(...)` with `Respawn.DelayMs` and
   `Respawn.RejoinYards`; on `Step::Rise` and `ArenaDefinition::RespawnAtEntrance`: `RiseAtEntrance(bot, seatState,
   SpawnPointFor(env), elapsed)` then `Clock.Risen`, `DeathPaid = false` (the I4 respawn clock, documented in
   [cpp-encounters.md](cpp-encounters.md) with EntranceRespawn). Then, only if seat 0 is alive, standing packs are topped up
   (`StandingPacks`), whatever happened while the party was down; a failed spawn is retried next decision. While seat 0 is
   dead no new pack is placed.
5. `SelectTarget`, `OnSeatAction`: not overridden (the stage has the sight block, the seat selects by itself).
6. `View` (`:476-495`): fills only the gauntlet block's fields: `PullsCleared` (= `Tally.Clears`), `FoodItem`, `DrinkItem`
   (0 for a manaless seat), `GauntletSupplies = CONSUMABLE_COUNT`, and `PullTime` = min(1, ms since the first fighting
   pack engaged / 60000).
7. `BeforeRewards` (`:497-548`), once a decision: `NoteFights(packs)` (each fighting pack beyond the first is an
   extra pull, counted once a pack; every fighting pack stops being clean); a wipe (every seat dead) is counted on the
   edge (`AllDown`); a pack with no living member is a clear (clean if never fought beside another), erased from `Packs`;
   `ListTargets` again if any was removed. Cleared packs are replaced in `Update`.
8. `Reward` (per seat, see E2b.1.3).
9. `IsTerminal` (`:718-722`): only the clock: `EpisodeLengthMs && elapsed >= length`. Never a death or a wipe.
10. `WriteState`: `state[StageScenario::STATE_TIER] = Tier / max(1, Roles.MaxTier)`.
11. `Teardown`: `Despawn`. `Deactivate` is the default (calls `Teardown`).

**Pack plan (`RolesDraw::PlanPack`, `RolesDraw.h:62-73`).** At rung `t`: level offset over the party's level
`Roles.LevelBase + t * Roles.LevelsPerTier / 2` (integer division; defaults -1 + t/2); size `min(PackSizeMax,
PackSizeFirst + t / max(1, PackGrowEvery))` (2, +1 every 2 rungs, max 4); a caster from `CasterTier` (1), linked from
`LinkedTier` (2), an elite from `EliteTier` (4); creature health `KeepHealthPct` (200) of normal for `Keep` only, else 100.
`SpawnPack` (`:253-319`) builds the entry list: the elite first (`OpponentPool::RandomElite(level)`), then a caster
(`RandomCaster`), the rest `RandomPackMember`; creature level `CombatDraw::CreatureLevel(roles.Level, pull)`; place:
the first pack `Opponents::FindSpawnPoint(lead, map, Combat.FightNearest, Combat.FightFurthest)` (28 to 40 yd, in seat 0's
sight); later ones `FindSpawnPointFrom(lead, map, previousSpot, bearing, NEXT_SPREAD = 1.0 rad, spacing, spacing +
10)` along the bearing from seat 0 through the previous pack (spacing `PackSpacing`: for `Pull`, linear from
`CampSpacingFirst` 45 yd at rung 0 to `CampSpacingLast` 25 yd at `MaxTier`; for other drills `Combat.NextNearest`, 30);
the stats modifier `ApplyStatPctModifier(UNIT_MOD_HEALTH, TOTAL_PCT, pct - 100)`, `UpdateMaxHealth`, `SetFullHealth` for
`Keep`.
How many stand: `StandingPacks` = 2 for every drill but `Pull`, where it is `max(2, min(CampPacksMax, CampPacksFirst + t/2))`
(2 to 4). The "next" pack standing in reach is what makes an extra pull possible.

**Drill = what each drill is** (all by `ArenaDefinition::Roles`, seat 0 must hold `DungeonRole == DrilledRole(drill)`,
else no drill term is paid, `Reward` `drilledSeat`):

| Arena | Drill | Drilled role | Episode s | What the lesson pays |
|---|---|---|---|---|
| `tank_hold` | Hold | tank (1) | 240 | per enemy on the seat, less per enemy on another party member |
| `heal_keep` | Keep | healer (2) | 300 | per member above half health, less per member under 35% or dead, less waste |
| `damage_discipline` | Focus | damage (3) | 240 | damage on the tank's target, less per enemy it holds |
| `pull` | Pull | tank (1) | 360 | Hold terms too, plus PullClean per clean pack; PullExtra when a second pack is drawn in |

**E2b.1.3 Rewards paid (`Reward`, `RolesEncounter.cpp:550-709`).** Term categories from `RewardTermCategory`
(`RewardLedger.h:162-243`). `w = 1 + Difficulty.TierScale * Tier` (`RolesDraw::TierWeight`; key `Difficulty.TierScale`),
`scale = StageScenario::DecisionScale()`, `decision = DecisionMs / 1000`. Losses are divided by `w`, earnings multiplied.

| Term (episode column `reward_<name>`) | Kind | Who | Amount | Key (default) |
|---|---|---|---|---|
| `Clear` | Outcome | every seat | `Roles.Clear * w * newClears` | `Roles.Clear` (1.0) |
| `PullClean` | Outcome | Pull drill only: `Roles.PullClean * pullShare * w * newClean` | puller (drilled seat) pullShare 1, others `Roles.PullOthers` | `Roles.PullClean` (2.0), `Roles.PullOthers` (0.5) |
| `PullExtra` | Cost | every drill, every seat | `-Roles.PullExtra * pullShare * newExtra / w` (pullShare 1 for the Pull drill's drilled seat, else `PullOthers`; so in non-Pull drills it is `PullOthers`) | `Roles.PullExtra` (1.5) |
| `DrillHold` (Hold, Pull) | Outcome (both signs) | drilled seat, alive | `+Roles.Hold * onSeat * scale * w` and `-Roles.Loose * onOthers * scale / w`; `onSeat` = living enemies in combat whose victim is the seat, `onOthers` = victim is another player or pet | `Roles.Hold` (0.045), `Roles.Loose` (0.018) |
| `DrillFocus` (Focus) | Outcome (both signs) | drilled seat | `+Roles.Focus * (LastStepDamage if the seat's victim == the tank's victim, else 0) * w`; `-Roles.PulledOff * onSeat * scale / w` | `Roles.Focus` (0.9), `Roles.PulledOff` (0.012) |
| `DrillKeep` (Keep) | Outcome (both signs) | drilled seat, only while a fight is on | `+Roles.Keep * up * scale * w`; `-(Roles.KeepLow * low * scale + Party.TeammateHealing * Roles.Overheal * wasted) / w` | `Roles.Keep` (0.0006), `Roles.KeepLow` (0.0006), `Roles.Overheal` (0.5), `Party.TeammateHealing` (2.0) |
| `StepCost` | Cost | every seat while any pack is fighting | `-Roles.Clock * decision` | `Roles.Clock` (0.01 per s) |
| `DamageDealt` | Shaping | every seat that did damage | `Roles.Damage * stepDamage / (mean max health of standing creatures)` | `Roles.Damage` (0.3) |
| `Death` | Cost | once per death of a seat | `-Roles.Death / w` | `Roles.Death` (2.0) |
| `Away` | Cost | per decision, when `CombatDraw::AwayCharged(alive, rejoining, fighting, yardsFromFight, Roles.AwayYards)` | `-Roles.Away * decision` | `Roles.Away` (0.02/s), `Roles.AwayYards` (30) |
| `Survived` | Outcome | at the episode's end if the seat never died and is not currently out | `Roles.Survived * w` | `Roles.Survived` (1.0) |

Detail the code shows: `wasted` = `(HealingRaw - effective) / MaxHealth` where `effective = SelfHealing + AllyHealing + sum
AgentHealingBy`; `up`/`low` are counted over all seats up to 5 that have a layout (alive and >50% health is up; dead or
<35% is low, `RolesDraw::Count`); Hold/Focus/Keep readings (`Tally.Held`, `OnParty`, `Damage`, `FocusDamage`,
`PulledOffSeconds`, `KeptUp`, `KeptMembers`, `LowManaSeconds`) are taken in the same branch, so a seat that is not the
drilled one (wrong role) contributes no reading. The death branch also sets `state.Combat.Died/DeathCounted/DeathMs` and
`++Deaths`. `Death`'s `1/w` and `PullExtra`'s `1/w`: harder rungs charge less per miss.
`Death` is paid once per `DeathPaid`; the flag is reset only by a rise in `Update` (`Update`, `RolesEncounter.cpp:406`), so a seat that dies,
rises, dies again is charged twice.

**Won (`RolesDraw::Won`, `RolesDraw.h:~212-232`)**: at least one pack cleared and no wipe, and by drill: Hold
`HoldShare >= Roles.WinHold` (0.75; Held / OnParty); Keep `PartyDeaths == 0`; Focus `FocusShare >= Roles.WinFocus`
(0.6) and `PulledOffSeconds <= Roles.WinPulledSeconds` (5); Pull `ExtraPulls == 0`.

**E2b.1.5 Rung ladder (`DifficultyLadder`).** One ladder per encounter (roles: the drilled seat's class and build).
Per (class layout, spec) row, top rung `Roles.MaxTier` (5). Training `Draw` (`DifficultyLadder.cpp:30-60`): current tier, but
with `Difficulty.ReviewChance` percent a random lower tier (not counted) and with `Difficulty.StretchChance` the next
tier up (not counted); `Counts` true only for a fight at the pair's own tier. Seeded evaluation: tier = `(EpisodeSeedIndex /
CastingCount) % (MaxTier + 1)`, `Counts` false. `Record` (at the episode's end, once, from `Reward` on the drilled seat when
time is up and `Counts`, `:700-708`): after `Difficulty.Window` counted fights the pair moves up at win rate `>= RaiseAbove`
or down below `LowerBelow`; a log line when it moves. Mutex-guarded; starts at 0 with the worldserver (not saved across a
restart; UNVERIFIED: whether `Tier` state is restored on `resume`). `Draw`'s review roll comment says world thread; `Record`
runs on map threads. Keys: `Difficulty.*` (see [cpp-tuning-keys.md](cpp-tuning-keys.md)).

**Episode-info columns written** (`AddEpisodeInfo`, `RolesEncounter.cpp:69-168`; names exact; the yaml
`apps/forge/python/configs/group1_roles.yaml` reports and heads most of them):
`won`, `drill_hold`, `drill_keep`, `drill_focus`, `drill_pull` (1 if the arena's drill), `won_hold`, `won_keep`, `won_focus`,
`won_pull`, `hold_share`, `kept_share` (KeptUp / KeptMembers), `low_mana_seconds`, `focus_share`, `pulled_seconds`,
`clean_share` (CleanClears / Clears), `packs_cleared`, `clean_pulls`, `extra_pulls`, `pulls`, `party_deaths`, `wipes`,
`rises`, `rejoins`, `rejoin_seconds` (mean over rejoins), `rejoined` (rejoins / rises), `dead_seconds`, `away_seconds`
(party sums), `roles_rung` (= Tier; the eval videos read it), `difficulty` (= Tier; the learner's per-class ladder
tracking), `at_top_rung`. Plus 11 `reward_*` columns for `RewardTerms()` (`RolesEncounter.cpp:62-67`): Clear, DrillHold,
DrillKeep, DrillFocus, PullClean, PullExtra, Survived, Death, Away, StepCost, DamageDealt. All are per-env totals read
for every seat (the same number on every seat's row). The yaml also reports `with_stand_in`, `tank_hold_share`,
`group_kept_share`, `tank_target_share`, `teammates_died`, `died`, which come from the stand-in, PartyEncounter and the
core columns.

**Config keys** (all under the tuning prefix; defaults in `CurriculumTuning.h:787-826`, Visit at `:1127-1163`):
`Roles.Clear 1`, `Hold 0.045`, `Loose 0.018`, `Focus 0.9`, `PulledOff 0.012`, `Keep 0.0006`, `KeepLow 0.0006`, `Overheal
0.5`, `PullClean 2`, `PullExtra 1.5`, `PullOthers 0.5`, `Survived 1`, `Death 2`, `Away 0.02`, `AwayYards 30`, `Clock 0.01`,
`Damage 0.3`, `MaxTier 5`, `LevelBase -1`, `LevelsPerTier 1`, `PackSizeFirst 2`, `PackGrowEvery 2`, `PackSizeMax 4`,
`CasterTier 1`, `LinkedTier 2`, `EliteTier 4`, `KeepHealthPct 200`, `CampPacksFirst 2`, `CampPacksMax 4`,
`CampSpacingFirst 45`, `CampSpacingLast 25`, `PartyNearest 2`, `PartyFurthest 6`, `WinHold 0.75`, `WinFocus 0.6`,
`WinPulledSeconds 5`, `StandInShare 20`. Also read: `Combat.FightNearest/FightFurthest/NextNearest`, `Respawn.DelayMs`,
`Respawn.RejoinYards`, `Difficulty.*`, `Party.TeammateHealing`, `Raid.DrillWeight` (PartyEncounter).

**Tests**: `RolesStageTest.cpp` (stage defined; packs by rung; the pull camp; the drill terms are outcomes and costs; Hold,
Keep, Focus, Pull outcomes; a death rises at the entrance and rejoins; a wipe rises together), `StandInTest.cpp`.

**Quirks / debts.**
- A comment in `RolesEncounter.h:47` names a `Roles.StandInShare` percent of training episodes; it is real
  (`StandInSeat.cpp:84-90`) but is consulted only when the arena's own share is < 0, and the live roles arenas set none.
- `Build` ignores `level`; the pack level derives from the drilled seat's `Level`.
- `roles.Counts` / `Recorded` mean a ladder window sees one result per episode, at the clock's end only. An episode that
  never reaches its clock (a build failure, a reset) records nothing.
- `Reward` computes `FindTargetUnit` loops per seat per decision (O(seats * targets)).
- DrillHold/Focus/Keep are Outcome-category terms that carry negative amounts (the losses). `ScoresOutcome` includes Cost,
  so the effect is only on accounting by category (learner's outcome/cost split): the "Cost" part of a drill is counted
  under Outcome.
- The drilled seat's class/build constraint: "a makeup that could not fit it pays no drill" (`:615`), silently.

### E2b.2 PartyEncounter (the core group and the party's role shaping)

`PartyEncounter(scenario, envs)` is created for any stage with an arena `PartyGroup` (`StageScenario.cpp:429-430`).
Used live by group1_roles and the four dungeon stages. Lifecycle:

- `BeforeRebuild` -> `Disband`: `Group::Disband(true)` of the env's group (`PartyEncounter.cpp:179-183`, `:221-230`).
- `Build` (`:185-219`; returns true always): every seat from 1 gets seat 0's faction; creates a `Group` with
  `SetSimGroup(true)` (a sim group: only in memory), `Create(leader = seat 0)`, `sGroupMgr->AddGroup`, `AddMember` for
  each other seat. A failed create/add is logged (`LOG_ERROR`) and the build still succeeds (without a group).
  Skipped if the env still has a `PartyGroup`.
- `View` (`:232-262`): fills the seat's `view.Teammates` slots with its group's other seats (the group = seats `seat/5*5 ..
  +5`), and `view.Tank = Tank(env)`.
- `Reward` (`:264-388`) and `RewardRole` (`:406-536`), `Teardown` (Disband).
- `Tank(env)` (`:~135-165`): the living seat whose `IsTank` (stage-drawn `DungeonRole == DUNGEON_TANK`, or role-less and a
  build that holds the pull) has the most `Apt[MITIGATION]`; with no real tank, the living seat with the most mitigation.

**Terms** (`RewardTerms()`: TeammateDamageTaken, TeammateHealing, TeammateThreat, TeammateDeath, Threat, Revive,
DamageDealt, Stall, EarlyPull; additionally paid but not listed: DrillHold, DrillFocus, DrillKeep, so in a stage with a
non-roles `DrillRole` arena their columns exist only via another encounter; no live stage has one). Categories:
TeammateDamageTaken, TeammateHealing, TeammateThreat, Threat, Revive, DamageDealt, Stall are Shaping; TeammateDeath is Cost,
EarlyPull is Cost, Revive Shaping (`RewardLedger.h:162-243`; note: TeammateDeath, EarlyPull listed Cost). Formulas
(`scale = DecisionScale`, `Party.*`, `Raid.*`):

| Term | When | Amount | Keys (default) |
|---|---|---|---|
| `Revive` | seat resurrected an ally this decision | `+Resurrection.ReviveAlly` | `Resurrection.ReviveAlly` (1.5) |
| `Threat` | a non-tank, alive, with a living tank in the party, per enemy on it | `-Party.PulledThreat * onBot * scale` | `Party.PulledThreat` (0.004) |
| `TeammateDamageTaken` | per other seat that is not a tank: damage it took this step | `-(Protects(apt) ? Party.TeammateDamageTakenProtector : Party.TeammateDamageTakenDps) * taken / teammateMaxHealth` | 1.0 / 0.5 |
| `TeammateHealing` | seat's build `Heals` | `healDrill * HealShare * Party.TeammateHealing * (AgentHealingBy + AgentProtectionBy of that teammate) / health` | `Party.TeammateHealing` (2.0), `Party.HealOffGoal` (1.0) |
| `TeammateThreat` | the seat is the tank: per enemy on a non-tank living teammate | `-Party.TankLoseTeammate * onTeammate * scale` | 0.02 |
| `TeammateDeath` | first time the seat sees a teammate dead (resets when it lives) | `-Party.TeammateDeath` | 3.0 |
| `Threat` (stance) | tank in combat in Defensive Stance, Bear or Dire Bear Form, or with Righteous Fury (25780) or Frost Presence (48263) | `+drill * Raid.TankStance * scale` | `Raid.TankStance` (0.001) |
| `Threat`/`DrillHold` (hold) | tank | `+Raid.TankHold * onBot * scale` and `-Raid.TankLoose * onOthers * scale` | 0.015, 0.006 |
| `DamageDealt`/`DrillFocus` (focus) | non-tank non-healer with a living other tank | `+Raid.TankTarget * LastStepDamage` if its victim is the tank's victim; `-Raid.PulledOff * onBot * scale` (term `Threat`/`DrillFocus`) | 0.3, 0.004 |
| `EarlyPull` | non-tank with enemies on it while the party tank is alive and not in combat | `-drill * Raid.EarlyPull * onBot * scale` | 0.01 |
| `TeammateHealing`/`DrillKeep` (keep) | healer: members of its group alive >50% (+1) or <35% (-1) | `+Raid.KeepUp * keptPay * scale` (above-half share at `HealShare`) and `-Party.TeammateHealing * Raid.Overheal * wasted` | `Raid.KeepUp` (0.0002), `Raid.Overheal` (0.5) |
| `Stall` | in combat, an enemy within `Raid.IdleReach` (40 yd), nothing done for `Raid.IdleMs` (4000) | `-Raid.Idle * scale` per decision | 0.001 |

`drill` = `Raid.DrillWeight` (3.0) for seat 0 when `DrillRole` matches and the arena is not Roles, else 1 (`:~420-430`).
In a roles arena the drilled seat's role terms are not paid here (`rolePay = 0`, `:~432`); the readings are still taken.
`HealShare` is 1 under a Protect goal or no goal, else `Party.HealOffGoal` (`:~369-379`).

**Episode info (PartyEncounter's columns, `AddEpisodeInfo`, `:41-114`)**: `seat`, `teammates_died`, `revives`,
`teammate_damage_taken`, `teammate_healing`, `group_kept_share`, `healing_coverage`, `threat_on_teammates`,
`idle_seconds_in_combat`, `tank_hold_share` (the same on every seat: the tank's EnemiesHeld / EnemiesOnParty, tank = the seat
with most EnemiesOnParty), `tank_form_share`, `tank_target_share`, `pulled_off_seconds`. Python readers:
`apps/forge/python/animus/config.py` (role metrics) and `episode_means.py`; group1_roles and the four dungeon yamls list
these. 

**Enemy ranking.** `EnemyRank {TankTarget, OnPlayer, Fighting, Standing, Gone}` and `RankEnemy(enemy, tank)` are declared at
`Encounters.h:65-74` and defined in `InstanceEncounter.cpp:57`, used for the enemy slot order at `InstanceEncounter.cpp:1907`. They order enemy
slots for the pack/crowd blocks.

**Quirks.** `Build` dereferences `SeatBot(env, seat)` for every active seat without a null check (`:206-207`:
`SeatBot(env, seat)->SetFaction`), and `lead` likewise; the group is skipped (and the build still succeeds) if creation
fails, leaving later code to find no group. `Tank()` iterates `SeatCount()` (not `ActiveSeats`). `IsTank`/`Heals`/`Protects`
read the build's `Aptitude`, which differs from what `FitsDungeonRole` drew in role-less arenas. The `TeammateHealing`
pay is for every seat whose build heals, not only the drawn healer. Comments in the file mention the "Deadmines'
parties", "stage6" and 2026-10 dates that no longer match any live stage.

### E2b.3 The "human" stand-in (`StandIn.h`, `StandInSeat.cpp`)

**What it is.** A party seat whose row the learner plays with a frozen partner policy from its co-op partner pool, never
trained on; the sim only chooses and marks the seat. It is not scripted (principle 14). The sim marks it with `present = 2`
(protocol 25): `StandIn::Presence(hasCharacter, isStandIn)` -> 0 none, 1 learner, 2 stand-in (`StandIn.h:53-62`,
used in `StageScenario::AgentPresence`, `StageScenario.cpp:3475-3481`, and the `present` episode-info column, `StageScenario.cpp:828-831`, which excludes the stand-in's seat). It is absent when the learner has no partner (`MODE_FLAG_STAND_IN` unset): `StageScenario::SetStandIn(bool)`
(`StandInSeat.cpp:~58-65`) stores an atomic flag (relaxed) and logs the change.

**API (pure, `Animus::Curriculum::StandIn`)**: `enum Role {Tank, Healer, Damage}` with `ROLE_NAMES`; `Presence`;
`Fields(modeAllows, partyOrRaid, activeSeats, evaluating, share, roll)` (false unless party, >= 2 active seats and the mode flag;
then true in every evaluation, and in training with `roll(share)` only if `share > 0`, so a stage without a share draws no
random number); `Tuning {Share 0, LeadChance 50, TankChance 34, HealerChance 33}`; `Rng` (splitmix64: `Next`, `Unit`,
`Chance(percent)`, `Between(lo, hi)`); `EvaluationSeed(seedIndex)`; `Style {Seed, Leads, Wanted}`; `Draw(seed, tuning,
canLead)` (leads = `canLead && Chance(LeadChance)`; wanted role from one uniform: < TankChance tank, < Tank+Healer
healer, else damage); `RoleFor(wanted, canTank, canHeal)` (the wanted role if the build can, else damage).

**Scenario members (`StandInSeat.cpp`)**: `StandInSeat(env)` (the chosen seat or -1), `StandInLeads(env)`,
`StandInShare(arena)` (the arena's own `Arena.<stage>.<arena>.StandInShare`, clamped -1..100, if >= 0; else for a Roles arena
`Roles.StandInShare` (20); else `StandIn.Share` (0)), `DrawStandIn(env)` (called at the end of `Rebuild`, `StageScenario.cpp:2383`),
`AddStandInEpisodeInfo()` (`:2` only if any arena is Party).

`DrawStandIn` step by step: reset `StandInPlay`; ask `Fields(...)` with `_standIn`, party seats, `ActiveSeats`,
`env.Evaluating`, `StandInShare(arena)`, `roll_chance_i`; seed = `EvaluationSeed(EpisodeSeedIndex)` in an evaluation else two
`rand32()`; style = `Draw(seed, _tuning.StandIn, canLead = !arena.DrillRole)`; if it leads, seat 0 is chosen (must have a
layout and a bot); else among seats 1.. with a layout and a bot, prefer those whose build fits the wanted role (`FitsRole`:
tank = `DungeonRole == TANK` or role-less + holds the pull + taunt > 0; healer similarly with `KeepsThemUp`; damage = neither),
else any, picked with `Rng(seed ^ 0x5EA7)`; record `StandInPlay {Seat, Style, Role = RoleFor(wanted, CanTank, CanHeal)}`; a
`LOG_DEBUG` line. Who reads it: `InstanceEncounter.cpp:2509, :2595` (`WingRun::LeaderSeat(StandInSeat, StandInLeads)`: a
leading stand-in is the party's leader), InstanceEncounter's `clear_standin`/`clear_allbot`/`standin_gap` columns
(`:314-322`).

**Episode-info columns** (`AddStandInEpisodeInfo`): `with_stand_in` (1 when one played), `stand_in_leads`, `stand_in_role`
(1 tank, 2 healer, 3 damage, 0 none). Every row reports them (the stand-in's own row is not reported).

**Config keys**: `StandIn.Share` (0), `StandIn.LeadChance` (50), `StandIn.TankChance` (34), `StandIn.HealerChance` (33) (Visit
`CurriculumTuning.h:1170-1173`), `Roles.StandInShare` (20), per-arena `Arena.<stage>.<arena>.StandInShare`; the live dungeon
arenas set `StandInShare = 20` in their definition (`Stages.cpp:621`). **Tests**: `StandInTest.cpp` (determinism per seed,
style coverage, role within the build, present = 2, no stand-in without the mode flag, evaluation vs training share).

**Quirks.** `LeadChance` comment says "only where the party has no owner" (`StandIn.h:92`); the code allows leading unless the
arena has a `DrillRole` (an owner is not consulted). `canLead` false skips no random number (the `Chance` is still drawn),
good for determinism. `Fields` treats `partyOrRaid` as `arena.Seats == Party`; a Party-seat arena of 1 active seat never has one.
The seat choice loop starts at index 1, so a follower never sits in seat 0. `StandIn.h` header comment says the leading
stand-in sits in seat 0 "which PartyEncounter makes the group's leader" - true (`PartyEncounter::Build` leader = seat 0).
`StageScenario.cpp:826-831` makes the `present` info column 0 for the stand-in's seat, so no class's episode counts it.

### E2b.4 Standing (`Standing.h`, 69 lines)

Three pure inline functions in `Animus::Curriculum::Standing`, shared by the movement encounters (Sight, Seek, Interact,
PartyFollow; call sites `SightEncounter.cpp:371,404`, `SeekEncounter.cpp:489,542`, `InteractEncounter.cpp:604,648`,
`PartyFollowEncounter.cpp:651,664`):

- `WallCharge(wallSeconds, moved, asked, price, slideShare)`: 0 if `wallSeconds <= 0` or `price <= 0`; ratio = clamp(moved /
  asked, 0..1) (0 if asked < 1e-4); blocked = clamp((share - ratio)/share, 0..1) with share = clamp(slideShare, 0..1) (0 if
  share is 0); result `price * wallSeconds * blocked`. Sliding along a wall with good progress is free.
- `Stopped(movementFlags, movedYards, stopMoved)`: true when none of `MOVEMENTFLAG_MASK_MOVING | SWIMMING | FLYING` is set
  and the unit moved under `stopMoved` yards since the last decision. Turning in place does not count.
- `Band(distance, bandMin, bandMax, lostYards)`: 0 too close, 1 in band (`<= bandMax`), 2 behind (`<= lostYards`), 3 lost.

Tests: `StandingTest.cpp` (`WallChargesOnlyTheGroundNotCovered`, `StoppedReadsTheServersFlags`, `FollowBands`); its fourth
test, `CourseKinksReadTheWayBetweenTicks`, tests `StageScenario::CourseKink`, not this header. The header comment lists "seek,
sight, interact, the party follow" as readers: accurate per the call sites above. Keys that supply the parameters are in each
movement encounter's section (E1, E2).

### E2b.5 Observed issues (this fragment)

1. `RolesEncounter.h:47` mentions a `Roles.StandInShare` percent; its only effect is as a fallback (`StandInSeat.cpp:88-90`),
   and every live Roles arena (`Stages.cpp:899-910`) leaves `StandInShare` at -1, so the key is the live G1 share (20).
2. `RolesEncounter.cpp:235-236`: `Build`'s `level` parameter unused; the level is from the drilled seat.
3. `RolesEncounter.cpp:432`: no pack top-up while seat 0 is dead; a party whose seat 0 is dead and the rest alive faces only
   what is left, and the clock continues.
4. `RolesEncounter.cpp:411` (`DeathPaid` reset only on the entrance rise): a seat that dies twice is charged `Death` twice, a
   wipe counts via `AllDown` separately; the per-seat `Deaths` and `PartyDeaths` double count nothing, but `Survived` checks
   only `seat.Deaths == 0`.
5. `RolesEncounter.cpp:615`, `:624-625`: drill terms silently not paid when seat 0's `DungeonRole` does not equal the drill's
   role; the stage relies on the draw (`FitsDungeonRole`) to guarantee it. UNVERIFIED how often it fails to fit.
6. `Drill*` terms are Outcome-category terms carrying negative parts (losses), see `RewardLedger.h:171-175`, so learner
   outcome/cost splits include costs in "outcome".
7. `PartyEncounter.cpp:206-207`: `SeatBot(env, seat)->SetFaction` and `lead->GetFaction()` are dereferenced without a null
   check, unlike the rest of the file.
8. `PartyEncounter.cpp:212-215`: a failed `Group::Create` still returns true; downstream group-based features (party frames
   `FillFromGroup`) then see no group.
9. `Encounters.h:48,55`: `<mutex>` included twice; `Encounters.h` includes headers for encounters it does not declare
   (`Battleground`, `WingRun.h`, `RouteShortcut.h`).
10. `PartyEncounter.cpp:381-385` and `:392-398`: the `drilled`/`rolesDrilled` logic for `DrillHold/Focus/Keep` is the
    pre-G1 drill path; no live non-roles arena sets `DrillRole`, so the non-roles branch of `drill`, `drilled`, `holdTerm` and
    `healDrill` (`:290-293`) is dead in the live curriculum (UNVERIFIED: check no dungeon arena sets `DrillRole`; `Stages.cpp`
    shows none).
11. `StandIn.h:92` comment about "owner" is stale (no live stage has an owner except the follow leader, which has no stand-in).
12. `Standing.h` header comment and `StandingTest.cpp:63` mix `StageScenario::CourseKink` into a file named for Standing.h.
13. `PartyEncounter.cpp` reward loops read `env.Targets` with `FindTargetUnit` per seat per teammate per decision (quadratic
    in seats).
