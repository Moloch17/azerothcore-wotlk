# Group capture the flag: two teams of ten

Warsong Gulch as it is actually played -- 10 v 10, both sides learned -- beside the 1 v 1 `stage11_flag`
already has. A new stage; stage 11 is untouched and keeps running.

## What exists, and what assumes one seat a side

`FlagEncounter` (281 lines) is written for `SeatPlan::Mirror`: seat 0 is one side, seat 1 is the other, and the
identity `side == seat` is used throughout -- `Sides[seat]`, `Sides[1 - seat]`, `SeatBot(env, 1 - seat)` for the
carrier, and `for (seat = 0; seat < 2; ++seat)` in `Update`. `EnvFlags` holds `std::array<Side, 2>`, which stays
right: there are still two sides. What is wrong is that `Side` also carries **per-seat** state -- `Dead`,
`RespawnMs`, `LastDistance`, `LastGoal` -- because with one seat a side the two were the same thing.

Seat capacity is not a problem: `MAX_SEATS` is 40 and stages 13/14 already plan 40, so 20 fits with room.

## Design decisions, made rather than asked

1. **Both teams learned** (self-play), as stage 11 is. Ten seats a side, `SeatPlan::Teams`, seats 0-9 are side 0
   and 10-19 side 1: `side = seat / TEAM_SEATS`.
2. **Team-level reward for team events, personal for personal ones.** A capture, pickup, return or carrier kill
   pays every seat on that side; a death charges only the seat that died. This is the standard shape for a team
   objective and suits a shared critic. It needs the `Step*` counters zeroed once a decision rather than by the
   first seat to read them -- see below.
3. **Each team is a group.** Reuse `PartyEncounter`'s pattern (`new Group`, `CoreHooks::MarkSimGroup`,
   `sGroupMgr->AddGroup`) so party and raid spells, and the healer layouts, work at all. Without this a healer
   seat cannot target a team-mate and ten of the eighteen layouts are dead weight.
4. **A new stage, `stage18_warsong`**, extending `stage11_flag` and seeding from it, so the 1 v 1 flag skill
   carries in rather than being relearned.

## Changes

1. **`Layout/Block.h`** -- `TEAM_SEATS = 10`, `TEAM_COUNT = 2`, `TEAM_MATCH_SEATS = 20`.
2. **`Stages/StageDefinition.h`** -- `SeatPlan::Teams`.
3. **`Stages/Stages.cpp`** -- `ArenaDefinition::SeatCount()` returns `TEAM_MATCH_SEATS` for it; the
   `stage18_warsong` definition.
4. **`Encounters/Encounters.h`** -- move `Dead`, `RespawnMs`, `LastDistance`, `LastGoal` off `Side` into a new
   `SeatFlagState`, one per seat in `EnvFlags`. Add `Side::CarriedBy` (the seat holding this side's flag, or
   `NO_SEAT`), which replaces `SeatBot(env, 1 - seat)`.
5. **`Encounters/FlagEncounter.cpp`** -- `SideOf(seat)`; `Update` loops every seat and zeroes the team `Step*`
   counters at its top so all ten seats read the same decision's events; `Build` places two bases and spreads ten
   seats around each; `CurrentGoal` and `View` read `SideOf(seat)`; `Reward` splits team from personal.
6. **`StageScenario.cpp`** -- seat placement for `Teams` (team 0 around the spawn, team 1 moved by the encounter
   once base 1 is known, as Mirror's seat 1 already is); group forming per team; generalise the
   `agent == 1 && Seats == Mirror` opponent test at line 1117.
7. **Config + YAML** -- `stage18_warsong.yaml` seeding from stage 11, and the arena's tuning keys.

## Not in this pass, and why

- **Team-mate observations.** `SeatView` has `Raid` and `Friends`, and `PartyEncounter` fills them; the flag
  encounter does not. Ten seats that cannot see each other will co-ordinate poorly. This is the first thing to
  add next, but it is a separate piece of work and the stage runs without it.
- **Target selection among ten enemies.** `FlagEncounter` has no `SelectTarget`; with one enemy that was fine.
  Worth measuring before designing -- the policy may pick reasonably through the existing hostile views.

## Verification

- `forge start stage18_warsong` sets up env 0 (stages 9 and 11 both failed here first, on placement).
- 20 agents an env in `forge status`, and `flag_pickups` and `flag_captures` coming off zero.
- Both sides score: captures on side 0 and side 1 both non-zero, else the teams are not symmetric.
- A healer layout shows `healing_done` above its 1 v 1 value, which is the check that grouping took.
