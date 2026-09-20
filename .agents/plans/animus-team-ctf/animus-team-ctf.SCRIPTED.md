# Scripted Warsong Gulch: the core's battleground, not our rules

The 10 v 10 stage runs on map 489 with `FlagEncounter` playing the flags itself. The requirement is the real
thing: `BattlegroundWS`, its doors, its flags, its graveyards, its score and its end condition.

## What the investigation found

**Good:**
- The forge branch barely touches battlegrounds -- `git diff master...forge -- Battlegrounds/` is 3 lines in
  `BattlegroundSA`. The whole system is stock.
- **`ForgeWorld.cpp:120` already calls `sBattlegroundMgr->Update(diff)`**, so the sim's tick already drives
  battleground logic. Nothing needs adding to make a battleground run.
- `Battleground::AddPlayer` needs no client: its only session use is `SendPacketToTeam`, and the forge already
  returns early from packet sends.
- `CreateNewBattleground(BATTLEGROUND_WS, bracket, 0, false)` builds a real instance from the template, and
  `MapInstanced::CreateBattleground(id, bg)` is the existing path for its map.
- Map 489 has no database gameobjects: the doors and flags come from `BattlegroundWS::SetupBattleground`, which
  is exactly what we now want to run.

**The two hard parts:**

### 1. Faction, not team id

`player->GetBgTeamId()` decides which side the battleground counts a player on, and it can be set outright. What
it does **not** decide is whether two bots can fight: hostility comes from race faction. Ten Alliance-race bots
labelled Horde would stand and watch each other.

So seats have to be genuinely Alliance and Horde. `ClassRoleAssets::Races` (`ClassRoleAssets.cpp:53`) collects
every playable race for the class with no faction split, and `BuildSeat` picks from it at random. It needs a
required team: side 0 draws from the class's Alliance races, side 1 from its Horde races, via
`Player::TeamIdForRace`. Every class exists on both factions in WotLK, so no layout is lost.

### 2. The battleground owns the match; the sim owns the episode

`Battleground::EndBattleground` scores, rewards, sets `STATUS_WAIT_LEAVE` and then removes players, teleporting
them to their entry point. The sim resets episodes on its own clock and expects its seats to stay put.

Chosen: **let the battleground own it.** An episode is a match. The encounter drives `STATUS_WAIT_JOIN` ->
`STATUS_IN_PROGRESS` so the gates open, the script runs the whole match, and the episode is terminal when the
battleground reaches `STATUS_WAIT_LEAVE`. The alternative -- suppressing the script's start and end to keep fixed
episodes -- keeps the sim's shape but means the thing being trained on is not really the battleground, which is
the requirement.

The cost is a battleground created and torn down per episode per env (128 of them), each spawning its doors and
flags. That is the main performance unknown and wants measuring early.

## Order of work

1. **Faction-correct seats** -- team on `BuildSeat`, race filtered by `TeamIdForRace`. Feasibility-critical and
   independent of everything else, so first.
2. **A battleground per env** -- create, set the map, add the twenty seats with their team ids, drive to
   `IN_PROGRESS`. Replaces `MapInstanced::CreateSimBattleground` with the real `CreateBattleground`.
3. **Episode from match** -- terminal on `STATUS_WAIT_LEAVE`; teardown and a fresh battleground on reset.
4. **Rewards and views read the battleground** -- `GetTeamScore`, the flag state from `BattlegroundWS`, rather
   than `FlagEncounter`'s own counters. `FlagEncounter` becomes an adapter over the script for the team stage,
   and keeps its own rules for the 1 v 1 `stage17_flag`, which does not want a battleground.

## What this does not change

`stage17_flag` keeps `FlagEncounter`'s own rules: it is a 1 v 1 on the Barrens, not a battleground, and it is
mid-run at 20.4M steps.

# What was built, 2026-09-19

Done and running as `stage18_warsong`: a real `BattlegroundWS` an env, ten a side on map 489, the script owning
the match and `FlagEncounter` reading it back.

**The core is untouched.** An earlier attempt gave the sim a bare `BattlegroundMap` with no `Battleground`
behind it, and that needed four core changes -- `MapInstanced::CreateSimBattleground`, a sim branch in
`CreateInstanceForPlayer`, and two in `BattlegroundMap`. With a *real* battleground registered in
`BattlegroundMgr` and the seat told which instance it was invited to, the stock `CreateInstanceForPlayer` path
serves it unmodified, and all four were reverted. If a future change tempts a core patch here, check first
whether a real Battleground makes it unnecessary.

**How a seat reaches the map.** Through the invitation, not the map id: `BotSpec` carries
`BattlegroundId`/`Type`/`Team`, `BotSlot::CreateNext` sets it on the player before placement, and `MapInstanced`
asks the player which battleground it was invited to. This is why the match must exist before the seats, which
is what `Encounter::BeforeSeats(env, level)` is for -- it runs between the level draw and the seat loop.

**Performance was not the problem it looked like.** 26,089 env steps/s with matches cycling, and a per-episode
teardown and recreate costing 2.4 ms (`reset 2.54 ms, 0.08 episodes rebuilt per decision`). The 1,452 steps/s
seen at first was the opening episode building 128 battlegrounds, not the steady state.

**A validation rule had to be widened.** `ArenaProblem` required `SeatPlan::Mirror` for any flag match, so the
stage was dropped silently with `Stage stage18_warsong is left out: ...` in `Server.log`. Self-play now means
Mirror or Teams. Worth remembering that a stage can be rejected at load and the only sign is that line.

## Two traps that cost a cycle each

- **`animus-learner.log` accumulates across runs.** A watcher grepping it for "Baseline" matched a line from an
  earlier stage and fired at once. Wait on a file that a run creates, not on a string in a shared log.
- **A server can be started against a binary that is still being written.** The process began 76 seconds before
  the binary was finalised and ran the previous image, which looked exactly like a change that had not taken.
  Compare the process start time against the binary's mtime before believing a rebuild landed.

## Still open

- **Team-mate observations.** `SeatView` has `Raid` and `Friends`; `FlagEncounter` fills neither, so ten seats
  cannot see each other. The battleground's own raid grouping gives healers valid targets, but the policy is
  blind to its side. This is the next piece of work.
- **The evaluation budget.** The stage inherits stage 11's 2048-episode baseline, and an episode here is a match
  of up to 420 s: about 45 minutes before the first gradient step. The inherited `fight` baseline -- a scripted
  duellist in a ten a side battleground -- measures little either. Same shape as item 17 of the long-build list.
