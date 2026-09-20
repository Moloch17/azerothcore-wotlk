# Retraining the curriculum from scratch

Written 2026-09-20, after the revive exploit was fixed. Every existing checkpoint trained against a reward
where a druid could score by standing near a corpse (38.4 revives an episode, 88% of `druid_dps`'s stage-4
return), so nothing currently on disk is worth seeding from and the whole tree is rebuilt.

## What decides the order

Two edges, both in `StageDefinition`:

- `Extends` -- the stage whose best checkpoint seeds this one, block by block.
- `Merges` -- further parents whose blocks only they have are also seeded from, and whose arenas can be
  distilled.

Only three stages merge:

| Stage | Extends | Merges |
|---|---|---|
| `stage16_crossroads` | `stage9_party` | `stage15_arena`, `stage14_pvp`, `stage8_companion`,<br>`stage4_gauntlet`, `stage1_duel` |
| `stage17_flag` | `stage15_arena` | `stage6_travel` |
| `mix_duel_pvp` | `stage14_pvp` | `stage1_duel` |

`stage16_crossroads` is the join: it is the only stage that pulls the PvE line and the PvP line back together,
which makes it the stage to export from.

## The order

**Phase 1 -- the root.** Everything descends from it, so its faults are inherited by all eighteen policies in
every later stage. Give it the most budget and the strictest reading.

1. `stage1_duel`

**Phase 2 -- three independent branches.** Nothing here depends on anything else here, so the order within the
phase is free (and it is where a second training host would pay).

2. `stage2_pack` -> 3. `stage4_gauntlet`
4. `stage6_travel` -> 5. `stage7_flight`
6. `stage14_pvp` -> 7. `stage15_arena`

**Phase 3 -- the group line.** Needs `stage4_gauntlet`.

8. `stage8_companion` -> 9. `stage9_party`

**Phase 4 -- the joins.**

10. `stage17_flag` (needs `stage15_arena` and `stage6_travel`)
11. `stage16_crossroads` (needs `stage9_party`, `stage15_arena`, `stage14_pvp`, `stage8_companion`,
    `stage4_gauntlet`, `stage1_duel`) -- **export from here**

**Phase 5 -- by name.** Drills, raids and team stages.

`stage3_hazards`, `stage5_endurance`, `stage10_tanking`, `stage11_triage`, `stage12_raid_single`,
`stage13_raid_gauntlet`, `stage19_duo_led`, `stage18_warsong`

> **Phase 5 is currently a dead end.** Nothing extends or merges any of it, and `forge export <scenario>` ships
> one stage's models. So a tank drilled in `stage10_tanking`, a healer drilled in `stage11_triage`, everything
> the raid stages teach and everything the team stages teach is **discarded** unless the stage you export from
> is downstream of it. Decide this before spending the compute: either add them to `stage16_crossroads`'s
> `Merges`, or add a final join stage after Phase 5 and export from that.

## Objective per stage

"Objective" is the one thing the stage exists to teach. "Reads it" is the metric that shows whether it did.
Where the current gate cannot see the objective, that is called out -- those are the gates to change so the
build trains the right things.

### Phase 1

| Stage | Objective | Reads it | Gate today |
|---|---|---|---|
| `stage1_duel` | Close in and kill one same-level creature fast, taking little damage. The whole kit: ranks, cooldowns, trinkets, consumables, pets | `killed`, `time_to_kill`, `health_left`, `livelocked`, win rate per difficulty tier | **The only properly gated stage.** Win-rate floors per tier, `livelocked` max 0.01, per-layout floors over baseline. Keep it |

### Phase 2

| Stage | Objective | Reads it | Gate today |
|---|---|---|---|
| `stage2_pack` | Pick targets, interrupt, control what it cannot tank. A pack of 2-4, usually linked | `pulls_cleared`, `interrupts`, `control_seconds`, `died` | None. Needs at least a clear rate and an interrupt rate -- interrupt uptake was already flagged weak and nothing would catch it |
| `stage4_gauntlet` | Sustain across pulls: heal, eat, drink, arrive at the next pull ready | `pulls_cleared`, `readiness`, `died` | None. Needs `readiness` -- this is the stage that teaches it and nothing measures whether it did |
| `stage6_travel` | Mount when the trip is long enough to pay, get there, dismount to fight | `arrived`, `saved`, `mounted_fraction` | None. `saved` (fraction of walking time saved) is the objective and is unread |
| `stage7_flight` | Take off, fly over what is in the way, land | `arrived`, `flew`, `saved_if_flew` vs `saved_if_ground` | None. `flew` adoption decayed in an earlier run and nothing caught it |
| `stage14_pvp` | Beat a scripted enemy player: interrupts, crowd control, defensives | `won`, `killed`, `died`, `interrupts` | None |
| `stage15_arena` | The same against a learned opponent, which fights back adaptively | `won` (self-play, so ~0.5 by construction), `time_to_kill`, `health_left` | None. **Self-play win rate is a bad gate** -- it is 0.5 whatever happens. Gate on score against the scripted-opponent baseline instead |

### Phase 3

| Stage | Objective | Reads it | Gate today |
|---|---|---|---|
| `stage8_companion` | Keep a scripted owner alive while clearing: follow, assist, guard, heal it | `owner_deaths`, `owner_healing`, `pulls_cleared` | None. `owner_deaths` is the objective |
| `stage9_party` | Four learned seats acting as a group against elite-heavy pulls | `pulls_cleared`, `low_health_seconds`, `died`, `threat_share` | None |

### Phase 4

| Stage | Objective | Reads it | Gate today |
|---|---|---|---|
| `stage17_flag` | Warsong's rules one on one: take it, bring it home, stop theirs | `flag_captures`, `flag_pickups`, `flag_returns`, `won` | None. `flag_pickups` above zero was never confirmed even once -- gate on it |
| `stage16_crossroads` | Hold everything at once: every earlier situation, an ambush mid-gauntlet, a ganked owner | Per-arena scores (`arenas:` targets), and each merged parent's own measure | None. **This is the shipped model** -- it should be the most gated stage, per arena, so one regressed situation cannot hide behind the others |

### Phase 5

| Stage | Objective | Reads it |
|---|---|---|
| `stage3_hazards` | See a ground effect and walk out of it, every pull | `hazard_seconds`, `reward_hazard` |
| `stage5_endurance` | Finish a known eight-pull run | `pulls_cleared` against the run length; it is gated |
| `stage10_tanking` | Hold what the pull brings, keep it off the others | `threat_share`, `threat_on_teammates`, `teammate_damage_taken` |
| `stage11_triage` | Keep the hurt one up and spend mana to do it | `low_health_seconds`, `overheal_share`, `healing_per_mana` |
| `stage12_raid_single` | Forty seats on one elite and its adds | `pulls_cleared`, `died` |
| `stage13_raid_gauntlet` | A raid clearing and recovering | `pulls_cleared`, `readiness` |
| `stage19_duo_led` | Two a side under a director: follow the call | `order_focus_lowest` vs `order_focus_chance`, `order_focus_kept` |
| `stage18_warsong` | Ten a side for the flag | `flag_captures`, `won` |

## Budgets

The current numbers are coarse-sweep leftovers, not a designed build: 30M for most stages, 300M for
`stage1_duel` and `stage5_endurance`, 100M for `stage3_hazards`, 150M for the two drills. Suggested shape:

- **`stage1_duel` largest.** It is the root, it starts from nothing, and it carries the whole action catalog.
- **Stages that only add a block or two can be much smaller** than stages that change the situation: they
  inherit a working policy and only have to learn the new part.
- **Watch for the stall.** Roughly half the stages stop moving in their last third (`approx_kl` falls eight to
  elevenfold); the learner now says so. A stage that has stalled with a flat evaluation is done, and the
  budget is better spent on the next one.

## Two things to settle before starting

1. **What Phase 5 feeds.** Decide now or the drills, raids and team stages are compute spent on models nobody
   ships.
2. **Whether `stage19_duo_led` is worth running at all.** The learned director has not beaten chance on call
   quality in four 30M runs. Run `stage19_duo_led` once with `DirectorLearned` off first -- the scripted
   director is a perfect caller by construction, so if it does not beat the undirected arena there is nothing
   at two a side for a learned director to find, and the stage can be dropped from the build.
