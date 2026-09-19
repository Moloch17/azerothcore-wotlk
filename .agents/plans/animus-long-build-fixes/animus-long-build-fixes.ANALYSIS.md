# Animus: issues to fix before the long build

Written 2026-09-19, from the fast coarse sweep (30M-per-stage, behaviour-probing run). That sweep is not meant to
produce a shipping policy, so nothing here was fixed live -- every fix below needs a rebuild, and several invalidate
stages that have already "passed". Ordered by what corrupts training, then what corrupts measurement, then what
merely wastes time.

Current sweep state when this was written: stage1 11.2M (best 9.18, incomplete), stage2 10.6M (best -1.03, skipped),
stage3 30M / 26.44, stage4 30M / 36.76, stage5 30M / 43.03, stage6 ~25.6M / 13.02 in progress, stages 7-11 pending.

---

## 1. Revive is farmable, and it dominates druid scoring  (BLOCKER)

**What happens.** In stage 4's final evaluation `druid_dps` records 38.4 revives per episode against 0.97 owner
deaths, earning 57.55 of its 65.08 return -- 88% of its score. `druid_heal` is 41.96 of 57.33 (73%). In stage 5
`druid_heal` is 52.38 of 98.48 (53%). No other class exceeds 3.3 revives. The stage-4 aggregate "revives 0.13 ->
4.57" is almost entirely this, not coaching.

**Root cause, in the core.** `Player::ResurectUsingRequestData()` (`src/server/game/Entities/Player/Player.cpp`)
never clears `m_resurrectGUID`, and neither does `Player::ResurrectPlayer`. In the real game this is harmless: a
client sends exactly one `CMSG_RESURRECT_RESPONSE` (`MiscHandler.cpp:688`), and the next request overwrites the guid.
`StageScenario::AcceptResurrections` (`StageScenario.cpp:1445`) instead *polls* `isResurrectRequested()` every
decision, so after one landed Rebirth the request is permanently stuck: every later death of that ally is instantly
stood back up, free, with no cast and no cooldown, and each cycle increments `Revives` and pays
`RewardTerm::Revive`.

**Two compounding faults in `AcceptResurrections` itself.**
- It increments `Revives` and sets `StepRevivedAlly` *before* `ResurectUsingRequestData()` is known to have
  succeeded.
- That function returns early when the teleport is delayed (`IsBeingTeleported()` -> `ScheduleDelayedOperation`),
  leaving the player dead with the request still pending, so the next decision counts it again.

**Why `owner_deaths` hid it.** The death counter is guarded by a counted-once flag (`EnvOwner::DeathCounted`), so
the repeated deaths never appear -- the episode looks like one death and 38 resurrections.

**Fix.** Clear the request once accepted (`clearResurrectRequestData()`, or have the core clear it inside
`ResurrectPlayer`), and only count/pay after the ally is actually alive. Consider also capping revives per episode as
a belt-and-braces guard, since any future stuck-request bug becomes a scoring exploit again.

**Blast radius.** Stages 4, 5 and 8 (crossroads mixes companion and party arenas). Their scores are not comparable
to anything and every later stage seeds from a policy that learned druids score by standing near a corpse. Stages 4
and 5 must be re-run after the fix. Stage 6 (PvP) has no owner or teammates and is unaffected.

---

## 2. A resumed run silently drops new metrics columns, and misaligns layouts.csv  (data corruption)

`RunLogger.__init__` (`python/animus/train.py:59-72`) takes its fieldnames from the *existing* metrics.csv when
appending, with `extrasaction="ignore"` -- so any column added since the run started is dropped without a warning.
Worse, `log_layouts` (`:80-99`) builds its fieldnames from the *current* rows while appending under the *old*
header, so after an episode-info change the layout rows carry more columns than the header names and every
header-based reader mis-attributes them.

Hit live this session: after adding `interrupts` / `control_seconds` to the PvP encounter, the resumed stage 6 wrote
neither, and would have corrupted layouts.csv. Worked around by hand -- renaming both files to
`*-before-interrupt-fix.csv` so a fresh header was written -- which splits that run's history across two files.

**Fix.** On resume, compare the stored header against the current column set; if they differ, either rotate the file
automatically (what was done by hand) or write a new header block. Never append rows under a header of a different
width.

---

## 3. `forge start <stage>` seeds from `Extends`, not from queue order  (footgun, nearly cost stages 1-5)

`forge start stage6_pvp stage7_arena ...` was issued to restart the sweep at stage 6. Stage 6 extends `stage1_duel`,
which has no *finished* run, so the plan fell back to training stage 6 **from scratch** -- discarding everything
learned in stages 1-5 -- announcing it only as a warning line, then archiving the existing run. Caught within a
minute; the archived run held the weights, so nothing was lost, and `forge resume` was used instead.

**Fix.** When a scenario is started mid-queue and its `Extends` parent has no finished run, either seed from the
previous scenario in the queue (which is what the operator means) or refuse and say so. A warning that scrolls past
while the run is already being archived is not enough.

---

## 4. The seeding chain is broken at the bottom

Stage 1 stopped at 11.2M (best 9.18) and stage 2 at 10.6M (best -1.03, hand-promoted `latest.pt` to `best.pt` to
get past it; the at-start model is preserved as `best.pt.atstart-backup`). Because neither finished, every stage that
extends `stage1_duel` -- 6, 8, 9 at least -- reports "no finished run ... it will not seed from it".

Stage 2 never actually passed its gate: the 10M gain (0.62) fell short of the 2-SE bar (~0.87). Its -1.03 best score
is the weakest link in the curriculum and it was skipped rather than solved.

**For the long build:** run the full queue from stage 1 rather than resuming mid-chain, and treat stage 2's gate as a
real target rather than something to promote past.

---

## 5. Stage 6's PvP score is a mix of two reward functions

The interrupt fix (lib `3397175`) landed while stage 6 was at 20.1M of 30M, and the stage was resumed rather than
restarted -- `forge start` would have trained it from scratch (issue 3). So roughly 20M of that stage trained with no
payment for interrupts and 10M with it; the final score is not a clean stage result.

Re-run stage 6 from scratch on the long build.

---

## 6. Interrupt uptake in PvP looks weak

Over the 2.1M steps after the fix went live: interrupts per episode 0.106 -> 0.125 (+0.005/M), against a steady ~1.7
interruptible casts seen -- a kick rate of 6.6% -> 7.2%. The term is being found (its reward rises at the same rate)
and it cost nothing elsewhere (kills 0.92, wins 0.92, time-to-kill 24.1s all flat), but 7% of visible casts is low.

**Check on the long build** whether `Duel.Interrupt` is simply too small relative to a PvP fight's damage terms. Note
this reading is from a short window against a policy that spent 20M steps with no reason to press it, so it may just
be slow uptake.

Related: PvP `control_seconds` drifts *down* (1.79 mean, -0.019/M) while interrupts rise. Control is measured but not
paid in PvP, so this may be the interrupt term pulling presses away from CC rather than adding new ones. Worth
deciding whether PvP should pay for control at all.

---

## 7. Stages 4 and 5 win harder and die more

Both stages improved their headline score while getting *less* safe:

| | stage 4 start -> end | stage 5 start -> end |
|---|---|---|
| kills | 13.3 -> 14.7 | 23.7 -> 24.7 |
| pulls cleared | 5.9 -> 6.5 | 7.7 -> 8.0 |
| seat deaths | 0.43 -> 0.51 | 0.39 -> 0.47 |
| wipes | 28% -> 33% | 19.7% -> 24.9% |
| teammates died | - | 0.56 -> 0.73 |
| owner died | 48.5% -> 37.7% | 42.4% -> 39.0% |

Owner survival genuinely improved (and owner healing 307 -> 812, the seat's share of it 4.9% -> 13.5%). But deaths
and wipes rising alongside the score suggests kills and clears outweigh survival in the reward. Re-check after the
revive fix, since revive income currently pays for dying.

Also in stage 4: `overheal_share` is still 0.567 at the end and `heals_on_full` rose 0.002 -> 0.018 -- small, but the
wrong direction for the healing-efficiency work.

---

## 8. Measurement note, to avoid re-learning it

Per-episode counts are confounded with fight length: shorter fights mean fewer of everything, and fight length
correlates -0.56 to -0.95 with score in every class. Rates, shares and ratios survive that; raw counts do not. An
earlier "movement oscillation is pathological" reading was wrong for exactly this reason -- `moves/s ~ score` is
positive for 11 of 18 classes once length is controlled.

Also: comparing a run against an archive that never reached the same step count makes numbers look wrong. Always
compare at matched steps against an archive that actually got there.

---

## Suggested order for the long build

1. Fix revives (1) -- core + `AcceptResurrections`, plus the per-episode cap.
2. Fix the resume/CSV header handling (2) and the `forge start` seeding fallback (3). Both are cheap and both have
   already cost time this week.
3. Full queue from stage 1, no mid-chain resumes (4, 5).
4. Read (6) and (7) off the clean run rather than tuning against the coarse one.

---

# Look-ahead: what stages 8-11 will hit

Added 2026-09-19 after auditing the remaining queue while stage 7 trained. Items 1-4 are new; the checks that came
back clean are listed at the end so nobody re-runs them.

## 9. The broken stage-1 seed cascades into three stages, not one

`stage9_travel` extends `stage1_duel`, which stopped at 11.2M and has no `finished.json`, so it will train **from
scratch**. That is not contained:

- `stage10_flight` extends `stage9_travel` -- it inherits whatever stage 9 becomes.
- `stage11_flag` merges `stage9_travel` for the mounting half of a flag match ("mounting between bases comes from
  travel"), on top of `stage7_arena` for the fight.

So one unfinished stage at the bottom degrades the last three stages of the sweep. `stage8_crossroads` is fine: it
extends `stage5_party` (finished) and merges stage 7, 6, 4 and 3; only its `stage1_duel` merge is missing, which
costs it the duel arena's distillation teacher and nothing else.

**Options**: give stage 1 a `finished.json` as was done for stage 6 (seeds stages 9-11 from the 9.18 model at 11.2M,
which is weak but far better than random), or accept from-scratch travel for this coarse sweep and fix it properly by
running the full queue from stage 1 on the long build. Needs an operator decision; it is not a code bug.

## 10. Stage 8 is where the revive exploit does its damage

Its arena weights are companion 20 and party 20 out of 100 -- 40% of episodes carry an owner or teammates, which is
exactly where `RewardTerm::Revive` is farmable (issue 1). Whatever stage 8 scores will be inflated the same way
stages 4 and 5 were, and stage 8 is the stage that joins the PvE and PvP branches, so the contamination lands in the
policy everything after it builds on.

## 11. warrior_dps has an interrupt and does not use it

The action catalogs say warrior_dps carries both `shield_bash_72` and `pummel_6552`, the same as warrior_tank -- yet
in stage 6 it kicked 1.3% of visible casts against the tank's 11.8%. This is not availability. Pummel needs berserker
stance and shield bash needs a shield, so a dps warrior has to stance-dance to use either; the tank is already in a
stance that works. Worth checking whether the stance change is reachable in one decision, and whether its cost is
being charged twice (the stance swap plus the interrupt).

## 12. The warlock's interrupt is its pet's, and the pet may be the wrong one

warlock_dps has no interrupt in its own action list: Spell Lock belongs to the felhunter and comes through the pet
block. Its 1.4% kick rate may therefore be a pet-choice problem rather than an interrupt problem -- an imp or
succubus out means no interrupt exists at all that episode. Check what the policy actually summons in PvP before
concluding anything about the interrupt weight.

(For contrast, druid and priest sit at the floor legitimately: no druid interrupt exists in 3.3.5a. Paladin scores
10-12% with no named interrupt in its list, which is consistent with Hammer of Justice and Avenger's Shield being
counted through their effects rather than by name -- the property-based classifier working as intended.)

## Checked and clean -- do not re-investigate

- **Double payment in stage 8**: encounters are selected per arena (`_arenaEncounters[arena]`), so the pulls
  encounter does not run in a `arena_1v1` or `pvp_scripted` env. No interrupt is paid twice.
- **Outland map data for stage 10**: present -- 800 map tiles, 408 vmaps, 499 mmaps for map 530.
- **Riding and flying provisioning**: `TravelBlock::LearnRiding`, called from `SeatCharacter`, grants the riding
  skills and the side's mounts by level, flying included from 60 (Expert Riding + gryphon/wind rider).
- **Gates for stages 8-11**: all four configs carry the cleared `target` block, so each advances on its step budget
  rather than halting the queue.
- **Self-play interrupt chain**: fixed end to end on 2026-09-19 (lib `8e0c287`, `1463168`, `005c598`); stage 7 now
  records 0.086 interrupts an episode where it recorded exactly zero. Stage 11 uses the same mirror seats and
  inherits the fix.

---

# Calibration questions from stage 7's self-play run

Added 2026-09-19 from stage7_arena's first ~4M steps. None of these is a bug: the mechanisms were read and are
working as written. They are weights and signals that were tuned for PvE and may not carry into a mirror match.

## 13. The hazard penalty is the third-largest negative term in a 1v1, and melee pay it 4x

Average `reward_hazard` is -0.43 an episode against a kill reward of 3.5, while the hazard *damage* it stands in for
is about 4% of maximum health. Per layout, over the last 1.5M steps:

| layout | reward_hazard | hazard seconds | melee share |
|---|---|---|---|
| paladin_tank | -0.834 | 6.60 | 0.69 |
| warrior_tank | -0.725 | 6.22 | 0.75 |
| paladin_dps | -0.639 | 4.27 | 0.73 |
| druid_tank | -0.413 | 3.10 | 0.72 |
| warrior_dps | -0.204 | 1.41 | 0.66 |
| priest_dps | -0.198 | 1.34 | 0.38 |

Verified not a bug: `Encoding::StandingInHazards` skips `IsPositive()` applications, so these are real enemy ground
effects (an opposing warlock's Rain of Fire, a mage's Blizzard, a paladin's Consecration, a death knight's Death and
Decay), and nothing is near the `Hazards.Max` cap of 3.0 -- the worst layout is at 28% of it.

The issue is that `Hazards.Standing` (0.15/second) was tuned for PvE, where walking out of the fire is a choice. In a
one-on-one a melee seat that leaves the hazard leaves the fight; the tuning comment already anticipates this ("melee
have to stand in melee: ... an uncapped charge would teach a seat to leave the fight instead"), and the cap is the
answer it chose. The per-second rate may still want to be lower in a PvP arena, or the cap lower, so a tank is not
paying a standing tax it cannot avoid.

## 14. The goal head changes its mind ~7 times a fight

`goal_changes` is 6.8-6.9 per 40-second episode and `goal_match_share` is 0.22, for a `reward_goal_match` of 0.078 --
about 2% of the kill term. A goal re-chosen every 6 seconds is not structuring behaviour over any useful horizon.

Read this again a few million steps into a clean run before touching it: the Position goal was **unreachable** in
self-play until lib `8e0c287` earlier the same day (`GoalHeld` resolved the target by env slot, which a mirror
opponent never occupies), so every number above was measured on a goal head that had one of its six options broken.
If the change rate stays near 7 once that has had time to take, the head is adding noise rather than structure in
PvP, and the question becomes whether it deserves a hold bonus or a longer commitment window.

## 15. Entropy rises through the run

Policy entropy went 1.042 -> 1.182 over stage 7's first 3M steps with `entropy_coef` pinned at 0.01, so it is the
policy becoming less decisive, not a schedule. `repeated_presses` rose with it (1.63 -> 2.10).

In self-play this is defensible -- the opponent keeps changing, so no action stays reliably best, and an arms race
should raise the entropy of the best response. Paired with more repeats it could equally be dithering. Worth a look
on the long build; if entropy keeps climbing while reward per decision falls (0.0157 -> 0.0124 here), the two
together say the policy is losing its grip rather than exploring.

## 16. Overheal share is still ~0.44 in the arena

The healing-efficiency work (healing per mana, the HoT metric) was meant to bring this down; stage 7 reports
`overheal_share` 0.42-0.47 with `heals_on_full` at 0. Not a regression, but not yet a win either.

## Learner health, for the record

Clean through stage 7's first 4M steps: `approx_kl` 0.019 -> 0.012 (target 0.02), `clip_frac` 0.137 -> 0.106,
`explained_variance` 0.63 -> 0.84, all 4 epochs running every update, 10.5k env steps/s steady, no nonfinite updates.
The `ConnectionError: sim closed the connection` entries in animus-learner.log are from `forge cancel` cycles on
2026-09-19, not live failures.

## A measurement correction worth keeping

In a mirror match only one of the two seats can take the kill, so per-seat `killed` has a ceiling of 0.5, not 1.0.
`killed` 0.36 means ~72% of matches are decided, not 36%. Self-play per-seat means need doubling before they are read
as per-match rates -- `won`, `killed` and `died` all have this shape.

# Stage 9 (travel), 2026-09-19

## 17. Stage 9's step budget is ~20x larger than the skill needs

`stage9_travel.yaml` inherits `total_env_steps: 30M`, `convergence.min_env_steps: 20M` and `eval.eval_every: 10M`
from `stage1_duel.yaml`. Travel converges at **~1.1M**: `mounted_fraction` 0.26 -> 0.79, `saved` 0.018 -> 0.396,
trip 21.4 s -> 14.7 s over an unchanged ~175 yd walk, arrive reward 3.11 -> 5.38, entropy 0.90 -> 0.40, all flat from
update 66 (1.08M) through update 167 (2.74M). The remaining 27M steps confirm a number that stopped moving after
four minutes of wall clock, at ~3,000 steps/s -- about 2.5 h.

Duelling deserves that budget; travelling does not. Suggested for this stage (and stage 10, which inherits the same
shape): `total_env_steps: 8M`, `min_env_steps: 4M`, `eval_every: 2M`. Learner-side config only -- no rebuild, the
YAML is read at learner start.

**Deferred deliberately:** raised with the user on 2026-09-19 mid-run, answer was "not yet".

## 18. A child stage started before the parent's first evaluation seeds from an untrained snapshot

`best.pt` is only written at an evaluation, and `eval_every` is 10M (item 17). Until then the file on disk is the
**update-0** snapshot -- the parent's own seed, seconds old, before any gradient step. `init_from_checkpoint()`
(`python/animus/train.py:216`) falls back to `latest.pt` only when `best.pt` is *absent*, so a stale `best.pt` wins
silently and nothing warns.

Concretely: starting `stage10_flight` while `stage9_travel` sat at 2.8M steps would have seeded it from stage 1's
weights, discarding every bit of the travel learning, with no message saying so. The run looks normal afterwards.

This is the same family as items 3 and 4 and should be fixed with them. Options: seed from `latest.pt` when it is
newer than `best.pt`, or refuse to seed from a `best.pt` whose recorded evaluation is update 0, or simply evaluate
once early in every run so `best.pt` is never the seed snapshot. The third also fixes it for every future stage.

## 19. Movement state that waits for a client acknowledgement never arrives for a bot

`Unit::SetCanFly` hands a client-controlled unit its movement flag by packet and returns, leaving the flag unset
until the client acknowledges (`Player::SetPendingFlightChange`, an order counter). A seat sits on an idle session
and never answers, so the flag stays clear for the bot's whole life. Six more core entry points branch the same way
on `IsClientControlled()`:

| method | `Unit.cpp` | what silently fails for a bot |
| --- | --- | --- |
| `SetDisableGravity` | 16540 | rarely player-facing |
| `SetCanFly` | 16600 | flying mounts -- **fixed**, see below |
| `SetFeatherFall` | 16636 | Slow Fall, Levitate |
| `SetHover` | 16677 | hover auras |
| `SetWaterWalking` | 16730 | Water Walking, Path of Frost |
| `KnockbackFrom` | 15431 | knockbacks: the displacement itself is client-side |

`Unit::SetSpeed` (11349) branches the same way and is **not** affected, checked on 2026-09-19: it assigns
`m_speed_rate[mtype]` and calls `propagateSpeedChange()` before the branch, so the speed takes server-side and the
packet is only notification. Do not chase it again.

None of this reports an error. The state simply never changes, and the symptom surfaces far away as a policy that
"refuses" to use something. Stage 10 cost four rebuilds and two wrong diagnoses (stale area cache, then weak
exploration) before the flag was found: the seats had Expert Riding and a gryphon, mounted it the moment the ground
mount was withheld, and still could not fly, because a flying mount without `MOVEMENTFLAG_CAN_FLY` is a 60% ground
mount against Journeyman Riding's 100%. The policy had been choosing correctly the whole time.

**None of these needs a core patch.** `AddUnitMovementFlag` / `RemoveUnitMovementFlag` are public
(`Unit.h:773-774`), and the aura-side truth is public too -- `Player::IsFreeFlying()` (`Player.h:2620`) against
`Player::CanFly()` (`Player.h:2618`), which is just the flag. `TravelBlock::AllowFlight` reconciles the two every
decision from `BeforeApply`, which is exactly what `SetCanFly` does for a unit no client controls. The other five
want the same treatment when something reaches for them. `KnockbackFrom` is the one with no module-side answer:
a bot cannot be knocked back by Typhoon or Thunderstorm unless the module moves it itself.

**How to apply:** before blaming a policy for not using an ability, check whether the ability's state is set through
one of these. The cheap instrument is `SpellChecks::CheckCast`'s optional `reason` out-parameter, which turns a
silently masked action into a `SpellCastResult` number.

## 20. Stage 10 could not fly: four bugs, none of them the policy

Settled 2026-09-19. The flying arena reported `flying_fraction` 0.0000 for 182 updates while the seats mounted and
arrived, and the obvious readings -- a stale area cache, then weak exploration -- were both wrong. What was
actually stacked up:

| # | bug | flights ran at | `saved` on a flight |
| --- | --- | --- | --- |
| 1 | `MOVEMENTFLAG_CAN_FLY` never set (item 19) | 11.2 yd/s, a 60% ground mount | n/a, never flew |
| 2 | `MOVEMENTFLAG_FLYING` never set | 11.2 yd/s | 0.009 |
| 3 | the fix for 2 gated on altitude > `AIRBORNE_ABOVE` | 11.2 unless the seat climbed first | 0.045 |
| 4 | `MOVE_TO_OBJECTIVE` held the seat's altitude | 17.5-26.6, but 70-109 yd up | 0.079 |
| - | all four fixed | 17.5-26.6 flat | **0.501**, against a ground mount's 0.392 |

**2** is the one worth remembering. `MoveSplineInit::Launch` takes the spline's velocity from
`MovementInfo::GetSpeedType` (`Object.cpp:1014`), which returns `MOVE_FLIGHT` only when `MOVEMENTFLAG_FLYING` is
set and otherwise falls through to `MOVE_RUN`; `Launch` itself adds only `SPLINE_ENABLED` and `FORWARD`. The flag
is *reported by* the client on take-off, never commanded to it -- note it is absent from
`MOVEMENTFLAG_MASK_HAS_PLAYER_STATUS_OPCODE`, which does carry `CAN_FLY`. A seat has no client, so nothing set it,
and a gryphon was slower than a ground mount (11.2 against 14). The core hits the same wall for charmed flyers and
says so: *"Xinef: If creature can fly, add normal player flying flag (fixes speed)"*, `Unit.cpp:14751`.

**3 and 4 were self-inflicted**, introduced while fixing 1 and 2. Gating the flag on altitude made flight speed
conditional on climbing, and `MOVE_TO_OBJECTIVE`'s `std::max(landing, z)` was a one-way ratchet that kept a seat
wherever it had drifted. Altitude is pure loss -- 1 yd up saved 0.390, 69 yd up saved nothing -- and it buys
nothing, because a server spline does not collide with terrain.

**The policy was right at every step.** It rode when the gryphon was a 60% mount, rode when it was 11.2 yd/s,
pruned flight when climbing ate the trip, and adopted flying within 34 updates (1.1% -> 16.6% of episodes, unforced)
once flying was actually faster. Three separate investigations went looking for a learning failure that was never
there.

**How to apply:** measure the mechanism before theorising about the policy. `flight_speed` read a healthy 17.5
throughout because `GetSpeed(MOVE_FLIGHT)` is the honest speed *value* -- the spline simply never asked for it. The
metric that settled it was the peak horizontal distance covered in one decision while aloft, which no policy
behaviour can confound and no advertised value can fake. It should have been the first instrument built, not the
sixth.

# Upstream candidates: what mod-animus would gain from stock AzerothCore

The goal for the shipping module is to need no core patch at all. These are the changes worth proposing upstream,
ranked by what they buy the end-user module. Group A are plain bugs that bite any server and are the easy sell;
group B is what currently forces every bot module to reach around the core.

## A. Bugs that affect any server, bot module or not

**A1. `MapUpdater` cannot be restarted after `deactivate()`.** Both stops are sticky: `_cancelationToken` stays
true and the queue stays cancelled, so workers from the next `activate()` leave their loop at once and every
request pushed is dropped. The pool reports itself activated while running nothing, and the world thread's
`wait()` for map updates never returns -- a hang. Also `_workerThreads` is not cleared, so `activated()` lies
about joined threads. Fixed on `forge` in `488dca10a`; the fix is four lines and has no sim-specific content.

**A2. 32-bit game-clock timestamps wrap every 49.7 days of uptime.** `GameTime::GetGameTimeMS()` is 64-bit and
these truncate it to `uint32`, then compare absolute values, so each one misbehaves for a window after every wrap:

- `CharmInfo.cpp` pet action cooldowns -- `start + delay > now` reads "ready" for up to a GCD (use wrap-safe
  `getMSTimeDiff`, as the forge version does)
- `Pet.cpp` infinity-cooldown check
- `Spell.cpp` sanctuary timing
- `spell_druid.cpp` Eclipse proc cooldowns (`_lunarProcCooldownEnd`, `_solarProcCooldownEnd`)
- `spell_generic.cpp` `_applyTimes`
- `boss_xt002.cpp` (`getMSTime()` against game time)

Plenty of real servers run past 49.7 days, so these are ordinary long-uptime bugs; the sim only found them sooner
because its clock runs thousands of times faster. Each is a one-line widening.

**A3. Accepting a resurrection does not clear the request.** `Player::ResurectUsingRequestData()`
(`Player.cpp:13129`) resurrects and never calls `clearResurrectRequestData()`, while the *reject* path does
(`MiscHandler.cpp:681`); otherwise it is only cleared on construction and in `setDeathState`
(`Player.cpp:1099`). A real client sends one response so it rarely shows, but anything that polls
`isResurrectRequested()` -- which is the only way a module can accept on a bot's behalf -- re-accepts forever.
This is the revive exploit of item 1, and the core side of it is two words.

## B. What would let a bot module work on stock core without reaching around it

**B1. Movement state for a player-controlled unit with no client.** The whole of item 19: `SetCanFly`,
`SetWaterWalking`, `SetFeatherFall`, `SetHover` and `SetDisableGravity` send a packet and wait for an
acknowledgement, so they never take for a bot. Upstream shape: take the immediate path when the unit has no
active session (a `Player::HasActiveSession()`-style check beside `IsClientControlled()`), rather than every bot
module re-deriving it through `AddUnitMovementFlag`. This is the single most valuable one -- it is the difference
between flying mounts, Levitate, Slow Fall, Water Walking and Path of Frost working or silently doing nothing.

**B2. `KnockbackFrom` for a sessionless player.** The only one of the family with no module-side answer: the
displacement is entirely client-driven, so a bot cannot be knocked back by Typhoon, Thunderstorm or a boss at all.
Needs a server-side path upstream.

**B3. Groups that do not touch the database.** `Group::IsPersisted()` / `m_simGroup` on `forge`. A companion or
party module makes and breaks groups constantly; every one of them currently writes and deletes `groups` and
`group_member` rows for a party that exists for one fight. An opt-out flag on `Group` is small and self-contained.

**B4. Characters that are never saved.** The `PlayerStorage.cpp` no-persist path. Same family as B3: a module
with transient bots should be able to say so once, rather than fighting `PlayerSaveInterval`, logout saves, quest
saves and delayed saves separately.

B3 and B4 are not correctness issues on a real server -- persisting is the right default -- so they want to be
opt-in flags, which is also what makes them plausible upstream.

**B5. A server-side way to say a player is flying.** `MOVEMENTFLAG_FLYING` decides the speed of every flying
spline (item 20) and no core method sets it -- the client reports it on take-off, which is why it is absent from
`MOVEMENTFLAG_MASK_HAS_PLAYER_STATUS_OPCODE` while `CAN_FLY` is present. Any bot module that flies has to set the
flag itself or move at run speed, and there is nothing in the core that makes that discoverable. Upstream shape:
set it alongside the mount aura when the unit has no active session, or at minimum have `MoveSplineInit::Launch`
select `MOVE_FLIGHT` when the spline is a flying one (`args.flags.flying`) rather than asking the unit's flags.
The second is arguably a plain bug fix: a spline explicitly launched as flying should not move at run speed.

**Not upstreamable, and deliberately so:** everything marked `// Forge: no client sockets exist in the sim host`
(the packet-build skips in `Object.cpp`, `Unit.cpp`, `Bag.cpp`, `MoveSplineInit.cpp`, `Spell.cpp`,
`SpellAuras.cpp`), the accelerated clock (`src/server/game/Forge/*`, `GameTime`, `World`), and the instance
lifecycle changes. A real server must send those packets and must keep real time; these belong to the sim host
alone and mod-animus needs none of them.
