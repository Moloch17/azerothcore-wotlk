# Metrics: what a stage can report, and who uses it

Purpose and scope: the catalogue of every number the forge measures about an episode or a run: the **episode-info columns** the
sim writes at the end of each episode, the **`reward_<term>` columns**, the **derived measures** the learner computes in
evaluation, the **training-row columns** and the **progress.json measures**, and which live yaml uses which as a gate, a
convergence measure, a headline or a target. Written from the source at `forge` bd32b9dc8 (not from the stale backup, which has
more columns). Related: [file-formats.md](file-formats.md) (the files these land in), [protocol.md](protocol.md) (episode info on the
wire), [stages.md](stages.md), [config-yaml.md](config-yaml.md), [cpp-rewards-routing.md](cpp-rewards-routing.md),
[cpp-encounters.md](cpp-encounters.md), [py-learner.md](py-learner.md), [known-issues.md](known-issues.md).

Stage abbreviations used in the tables: **M1** move1_controls, **M2** move2_seek, **M3** move3_interact, **M4** move4_follow,
**C1** combat1_fight, **C2** combat2_packs, **C3** combat3_survive, **G1** group1_roles, **G2** group2_corridor,
**D1** dungeon1_pulls, **D2** dungeon2_ragefire, **D3** dungeon3_deadmines.

## Map of files

| Path | Lines | Role |
|---|---|---|
| src/server/game/Animus/Scenario/Curriculum/StageScenario.cpp | (about 4000) | `AddCoreEpisodeInfo` (lines 710-1240): the columns every stage has; the constructor adds `reward_*`, `vision_render_width`, `score_outcome` (530-570). |
| src/server/game/Animus/Scenario/Curriculum/Encounters/EpisodeInfoTable.h | | The `Add(name, lambda(env, seat))` table; the order of `Add` calls is the wire order. |
| src/server/game/Animus/Scenario/Curriculum/Encounters/SightEncounter.cpp | | M1 columns. |
| .../Encounters/SeekEncounter.cpp | | M2 columns. |
| .../Encounters/InteractEncounter.cpp | | M3 columns. |
| .../Encounters/PartyFollowEncounter.cpp | | M4 columns. |
| .../Encounters/CombatEncounter.cpp | | C1-C3 columns. |
| .../Encounters/RolesEncounter.cpp | | G1 columns. |
| .../Encounters/PartyEncounter.cpp | | Party-seat columns (G1, G2, D1-D3). |
| .../Encounters/InstanceEncounter.cpp | | G2, D1-D3 columns (wing, corridor, drill, boss, role, stand-in split, Go-Explore marks). |
| .../Encounters/StandInSeat.cpp | | `with_stand_in`, `stand_in_leads`, `stand_in_role`. |
| .../Rewards/RewardLedger.h | 400 | `RewardTerm`, `RewardCategory`, `RewardTermCategory`, `PricesNoise`, the ledger. |
| .../Rewards/CombatReward.cpp | | `RewardTermName`: term -> the name in `reward_<name>`. |
| apps/forge/python/animus/evaluation.py | 750 | `EvalResult.summary` (derived measures and the tables), `run_evaluation`, `ConvergenceTracker`, `casting_weights`. |
| apps/forge/python/animus/episode_means.py | 108 | `PER_EVENT` weighted means; `undefined`. |
| apps/forge/python/animus/progress.py | 165 | progress.json writer, `ARM_SPLITS`. |
| apps/forge/python/animus/rewards.py | 88 | Reward-mix audit (`audit`, `MAX_SHAPING_SHARE` 0.5). |
| apps/forge/python/animus/stage.py | 829 | Convergence controller: what reads which measure. |
| apps/forge/python/animus/config.py | 853 | `StatusConfig`, `EvalConfig`, `FadeConfig`, `CostLadderConfig`, `ConvergenceConfig`, `LayoutSamplingConfig`. |
| apps/forge/tools/sim_metrics.py | 586 | Extracts the column names from the C++ (cross-check). |
| apps/forge/python/tests/test_metric_names.py, test_layout_metrics.py, test_status_headline.py, test_outcome_score.py | | Tests that pin names (see Tests). |

## 1. How a column is made

An *episode-info column* is one `float` per agent per episode, registered with `_info.Add("name", lambda(Env, seat))` /
`table.Add(...)` (`EpisodeInfoTable`). The sim evaluates every lambda once when an episode ends and ships the row in the STEP's
`episode_info` array (protocol.md section 4). The names go in SPEC and in stage.json `episode_info` (so the order is the registration
order: core columns, stand-in columns, each encounter in build order, the `reward_*` columns, `vision_render_width`, `score_outcome`,
StageScenario.cpp:525-570). Values are plain floats: a flag is 0 or 1, a count is a float count, seconds are seconds, "share" is 0..1.
A column that an episode did not exercise is 0 (not NaN); the **per-event** columns are therefore averaged with a weight (below).

The learner turns each column into `episode_<name>` in metrics.csv / progress.json (mean over the training episodes that ended in an
update, `episode_means.means`) and into evaluation summary keys `<name>` (mean over the scored rows, `EvalResult.summary`).

**Per-event means** (`episode_means.PER_EVENT`, evaluation.py:summary): for a column `x` with count column `n`, the mean over episodes is
`sum(x*n)/sum(n)`, and NaN (`null`) when `sum(n)=0`. The pairs, verbatim from the dict: arrive_seconds, time_ratio, overshoot -> markers;
stop_distance -> stops_near; find_seconds, rooms_before_found -> found; sight_seconds -> sighted; sight_to_arrival -> found_sighted;
revisit_rate -> room_entries; found_hallway/doorway/room/deep -> rung_hallway/doorway/room/deep; arrived_no_compass -> compass_withheld;
arrived_with_compass -> compass_present; arrive_seconds_sight, time_ratio_sight -> markers_sight; arrive_seconds_corner, time_ratio_corner -> markers_corner;
right_seconds -> right_object; right_distinguish/switch/key -> rung_distinguish/switch/key; regroup_share -> regroup_stops; regroup_seconds -> regroups;
rejoined -> rises; rejoin_seconds -> rejoins; wing_rejoin_seconds -> wing_rejoins; kill_seconds -> kills; interrupt_earnings -> outcome_paid;
won_hold/keep/focus/pull, hold_share, kept_share, low_mana_seconds, focus_share, pulled_seconds -> drill_hold/keep/focus/pull (hold_share, won_hold -> drill_hold ...);
clean_share -> packs_cleared; clear_standin -> with_stand_in; clear_allbot -> without_stand_in; deaths_tank/healer/damage -> role_tank/healer/damage.
In the training log a per-event column with no events in the update is NaN and listed as *undefined* (`episode_means.undefined`) so progress.json writes null without naming it in `nonfinite`.
Note `found_sighted` (a count column) is itself summed as a plain mean (it is a 0/1 per episode), as are `sighted`, `markers`, `rung_*`.

**Row selection.** Every column is read for each *present* agent row (`episode_info[present] > 0`). `present` is 0 for an empty party seat, for the stand-in's row, and
for rows excluded by an evaluation arm (`excluded`). Party episodes therefore contribute one row per live seat; columns that are per-episode in meaning (e.g. `cleared`)
repeat on each seat's row, and the evaluation divides by episodes only for the standard error (`standard_error` averages the rows of an episode first).

## 2. Core columns (every stage)

Written by `AddCoreEpisodeInfo` (StageScenario.cpp:710-1240) unless noted. `seat(...)` = the per-seat `SeatState`. "Unit" is the unit of the number.

### Body, build, and bookkeeping

| Column | Meaning (what the lambda returns) | Unit |
|---|---|---|
| damage, dps | Total damage the seat did (`EpisodeStats.Damage`); that over elapsed episode seconds. | hp; hp/s |
| combat_dps | Damage over seconds spent in combat (min 1 s). | hp/s |
| dps_scaled | combat_dps over the level's damage scale (`SeatState.DamageScale`, min 1). | ratio |
| white_damage, special_damage | Damage by melee swings / by abilities. | hp |
| level, race, class | Character level; race id; class id (`Layout.Profile->Class`). | id |
| spec | Index of the build among the class's specs (`spec_names` in stage.json). | index |
| talent_plan | 0 standard, 1 noisy, 2 random (`SeatCharacter::TalentPlan`). | enum |
| unspent_talent_points, equipped_items, spell_casts | Counts at the end of the episode / casts started. | count |
| aptitude_mitigation, aptitude_healing | The build's aptitude features (MITIGATION; the larger of DIRECT_HEAL and HOT_HEAL). | 0..1 |
| present | 1 if the seat has a character and is not the stand-in's seat, else 0 (StageScenario.cpp:828). | flag |
| arena | Index of the episode's arena in stage.json `arenas` (0 when none). | index |
| spawn_point, spawn_drawn | Index of the spawn point used / first drawn; unequal means the first draw could not build. | index |
| build_failed | 1 if the episode could not be built and was rebuilt. | flag |
| vision_render_width | Width the camera cast at this episode (`RenderSizes`); only with a vision block (all live stages). | px |
| score_outcome | `RewardLedger::Score()`: the sum of the episode's Outcome and Cost terms as tuned, before the rung's tier and the role's scale. **The evaluation yardstick** (`eval.score: outcome`) and best.pt's criterion. 0 for agents beyond the seats. | reward |

### Water and falls (every stage gets them)

| Column | Meaning | Unit |
|---|---|---|
| swim_seconds, dive_seconds | Time in water / with the head under. | s |
| breaths, breath_spent, drowning_damage, drowned, water_walk_seconds | Surfacings; most of a breath spent (past 1 = drowning); damage taken under water past the breath; died by drowning (also true if dead underwater with drowning damage); seconds walking on water under an aura. | count, 0..>1, hp, flag, s |
| aquatic_seconds, breathing_casts | Seconds in a druid's Aquatic Form; water-breathing spells started. | s, count |
| jumps, drops, fell, fall_damage, fall_deaths, void_deaths, into_terrain | Jumps; landings 2+ yd below where the fall began; whether any fall; health lost to falls; deaths from falls; of those, kills under the map floor; ticks that ended inside terrain from above (a controller bug if > 0). | count/flag/hp |

### Fighting style, pets, resources

| Column | Meaning | Unit |
|---|---|---|
| died | `Combat.Died` (the seat died this episode). | flag |
| health_left | Bot health fraction at the end (0 if no bot). | 0..1 |
| stealth_openers, stealth_utility_casts, preparation_seconds | Stealth openers; stealth utility casts; out-of-combat buffs/forms/summons time. | count, s |
| pet_damage_share, melee_damage_share, shot_damage_share, spell_damage_share | Shares of the seat's own damage by source (pet + the three add to 1 when damage > 0). | 0..1 |
| pet_died, pet_abilities, pet_orders, pet_attack_orders, pet_passive_orders, pet_defensive_orders, pet_aggressive_orders, pet_follow_orders, pet_stay_orders | Pet bookkeeping: died, ability uses, orders overall and by kind. | flag/count |
| pet_out_seconds, pet_attacking_share, pet_passive_share, pet_staying_share, pet_at_start | Seconds the pet was out; shares of that time attacking / passive / staying; had a pet at the start. | s, 0..1, flag |
| consumables_used, item_uses, trinket_uses, self_resurrections, options_started, option_seconds | Consumables used; item uses; trinkets; self-resurrections; durative actions started / their seconds. | count, s |
| timed_out | `Combat.TimedOut` (the clock ran out). | flag |
| target_health_left, distance_at_end, form_at_end, power_left | The seat's target health fraction (0 if none/dead); distance to the target at the end; shapeshift form id at the end; primary power fraction left. | 0..1, yd, id, 0..1 |
| healing_per_mana, hot_healing_share, healing_mana_spent, healing_done, protection_done, overheal_share, heals_on_full, defensive_casts, healing_casts, downranked_share, low_health_seconds | Support readings: healing restored over mana spent on healing; share of healing by periodic ticks; mana spent / pool; healing and absorb done as fractions of the bot's max health; share overhealed; heals cast on a full-health friend; defensives; heal casts; share cast below top rank; seconds any friend was below `LOW_HEALTH_PCT`. | ratio/count/s |
| hazard_seconds, hazard_damage, interruptible_casts_seen | Time standing in ground effects; health taken from them (fraction of max); interruptible casts seen. | s, fraction, count |

### Intent, actions, movement (the "noise" readings behind the cost ladder)

| Column | Meaning | Unit |
|---|---|---|
| actions_per_minute | Actions pressed per minute of episode (players ~30-70 inside a fight per the comment). | /min |
| combat_actions_per_minute | Presses in combat per minute of combat (0 if no combat). | /min |
| serving_share | Presses that served the seat's goal over judged presses (`JudgePress`). | 0..1 |
| aimless_presses and `aimless_<cause>` | Presses against the seat's own goal in total and by cause. Causes (`AimlessCauseName`): off_focus, aoe_missed, in_range_cast, unprovoked_harm, help_off_goal, step_away, target_switch, pet_off_goal, consume_not_needed, trap_no_enemy, mode_flip, mode_reverse, needless_move, taunt_off_role, tank_mode_off_role, cast_facing, cast_range, cast_sight, cast_moving, cast_power, act_refused. `act_refused` only with a Sight block. (The sim_metrics extraction also lists `aimless_unknown`; that is an artefact of looping to the enum count, not a real column.) | count |
| act_refused_<why> | Presses the client packet path refused, by reason: gone, kind, reach, sight, loot, no_item, no_target, cast, locked (loop starts at 1 so no `none`; extractor artefacts `act_refused_none/unknown` are not columns). Only with a Sight block: M3, C1-C3, G1, G2, D1-D3. | count |
| mode_switches, effort_presses, repeated_presses | Mode changes; every non-noop press charged effort; presses of an action past the free ones in its window (`Actions.Repeat`). | count |
| move_starts_per_minute, move_stop_starts, fidget_seconds | Feet starting per minute; stop-then-start within 1 s per minute; seconds shuffling at range. | /min, s |
| turn_reversals, bearing_flips, pitch_reversals, weaves | Turn rates reversed in the jitter window; feet reversed (fwd to back, left to right); pitch reversals; 1.5-4 s wobble. | count |
| moves_refused | Movement reports the server refused (controller C4). | count |
| wall_seconds, stuck_seconds | Time pressing into a wall; time with a key held and no motion >= 1 s. | s |
| course_kinks | Course turns of more than 20 degrees in one tick, per minute. | /min |
| control_changes_per_minute, move_reports_per_minute | Control changes per minute; movement packets sent per minute (client cadence). | /min |

### Goals

| Column | Meaning |
|---|---|
| goals_reached, goals_lost, goal_changes | Goals reached / lost / changed. All zero without a goal head. |
| goal_targeted_share | Share of goal decisions with a target. |
| goal_<kind>_share | Share of decisions spent on each of the 12 goal kinds: fight, control, recover, protect, position, prepare, travel_to, loot, gather, interact, rest, resurrect. |
| goal_success_<kind> | Of goals chosen of that kind, the share reached. |
| goal_match_share | Share of decisions whose action matched the goal. |
(Extractor artefacts `goal_success_unknown` and `goal_unknown_share` are not columns.) Looting is excluded by design (principles #6) yet `loot` stays a goal kind.

## 3. Encounter columns

### M1 (SightEncounter; stage move1_controls)

arrived (0/1: stopped beside the object before the clock; the stage measure), markers (same 0/1; the weight for per-arrival means), compass_withheld / compass_present (this episode
withheld / showed the compass), compass_withhold_chance (the rung's chance), arrived_no_compass / arrived_with_compass (arrival in episodes of each kind), compass_rung (the fade rung),
marker_radius, arrive_seconds (spawn to stop), time_ratio (that over the optimum), markers_sight / markers_corner, arrive_seconds_sight / _corner, time_ratio_sight / _corner (by where the
object stood), overshoot (yards run past the radius after first reaching it), stop_distance (air between body and object's bounding circle, mean over stops near), stops_near (count),
distance_travelled (yd), movement_casts, speed_casts, objective_distance (straight yards from spawn), objective_corner (1 = round a corner, 0 = in sight), corner_fallback (a corner was asked for but a
point in sight was used), sight_object (index into `episode_categories.sight_object`), sight_pair (evaluation pair index), objective_visible (share of decisions whose frame showed the object's flag).

### M2 (SeekEncounter; move2_seek)

found (0/1, the stage measure), find_seconds (clock at arrival, per found), sighted / sight_seconds (first frame with an objective-flag pixel), found_sighted / sight_to_arrival,
objective_visible, rooms_entered / rooms_reentered / room_entries / revisit_rate (reentered over entries), rooms_before_found, rooms_looked (rooms whose floor a frame showed), seek_room / seek_object (category indexes),
room_depth (rank by walking distance, 0 nearest), difficulty (the room's tier, thirds), deep_room (deepest third), seek_rung (placement rung 0 hallway .. 3 deep), room_ladder (ladder rung as a share of the top),
rung_carried (the episode was a carry-over from the rung below), at_top_rung, rung_<hallway|doorway|room|deep> (placed there) and found_<rung> (found, per event over rung_<rung>), object_fallback, distance_travelled.

### M3 (InteractEncounter; move3_interact)

right_object (stage measure: named object reached or lock given the key), right_seconds, sighted / sight_seconds / found_sighted / sight_to_arrival / objective_visible (as M2, for the named object),
wrong_objects (decoys taken, each once), wrong_presses, wrong_stops, decoys (number of decoys), lever_pressed, door_opened, door_by_lever, key_used, interact_site / interact_object (category indexes),
interact_rung (0 distinguish, 1 switch, 2 key), interact_ladder, rung_carried, at_top_rung, rung_<distinguish|switch|key>, right_<rung>, object_fallback, distance_travelled.

### M4 (PartyFollowEncounter; move4_follow)

follow_kept_share (share of the episode within the leader's band: the measure), follow_distance_mean, lost_seconds, blocking_seconds, regroup_stops / regroups / regroup_share / regroup_seconds, deaths, rises, rejoins,
rejoin_seconds, rejoined, dead_seconds, leader_stops_reached, leader_route_share, leader_skips, leader_sudden_stops, leader_drops, leader_rises, leader_cast (1 = leader played by a cast checkpoint, 0 = the script), difficulty (rung), at_top_rung.
(The M4 leader is still scripted, so `leader_*` read the script.)

### C1-C3 (CombatEncounter)

won (something taken down and no death), survived (no death), kills, packs_cleared, pulls, extra_pulls, interrupts, deaths, respawns (**the same quantity as rises**: both return `Clock.Rises`), rises, rejoins, rejoin_seconds, rejoined,
dead_seconds, away_seconds (charged Away: dead, walking back, or off the fight), outcome_paid and interrupt_earnings (interrupt earnings over kill and clear earnings; to be scaled down above ~0.3), kill_seconds (per kill),
ally_deaths, hurt_share / fire_share (health taken, all / ground fire, in max healths), hazard_pulls, linked_pulls, caster_pulls, rest_seconds (eating/drinking), selected_share, target_in_view, start_walk,
combat_rung (= difficulty = the tier), at_top_rung (tier >= `Combat.MaxTier`).

### G1 (RolesEncounter + PartyEncounter)

won (drill won, `RolesDraw::Won`), drill_hold/keep/focus/pull (episode played that drill), won_hold/keep/focus/pull, hold_share, kept_share, low_mana_seconds, focus_share, pulled_seconds, clean_share, packs_cleared, clean_pulls, extra_pulls, pulls,
party_deaths, wipes, rises/rejoins/rejoin_seconds/rejoined/dead_seconds/away_seconds, roles_rung (= difficulty), at_top_rung.

### Party seat columns (PartyEncounter; G1, G2, D1-D3)

seat, teammates_died, revives, teammate_damage_taken, teammate_healing, group_kept_share (a healer's effectiveness: member-time alive above half health), healing_coverage (healing over teammate damage taken, capped at 1),
threat_on_teammates, idle_seconds_in_combat, tank_hold_share (party tank's enemies held), tank_form_share (tank's time in tanking form), tank_target_share (damage on the tank's target), pulled_off_seconds.

### Stand-in (StandInSeat.cpp; every stage registers them)

with_stand_in (the episode had the stand-in in a seat), stand_in_leads, stand_in_role (1 tank, 2 healer, 3 damage, 0 none). All zero where no stand-in plays.

### G2, D1-D3 (InstanceEncounter)

difficulty / boss_rung (the tier), pull_rung (drill ladder rung; only where evaluations drill, i.e. D1), wing_rung, at_top_rung, instance_map, boss_entry, boss_killed, boss_health_left, engaged, wiped, evaded, wing_trash_kills,
wing_boss_kills, wing_route_share, wing_wipes, wing_cleared_share, wing_crowd_seconds, wing_probe (a probe run, which steps the ladder), wing_rises, wing_rejoins, wing_rejoin_seconds, wing_level, **cleared** (`Succeeded`: a drill's pack
alone and dead; a corridor's packs all done; else the last boss dead), **full_clear** (last boss dead and every route pack cleared), **bar_clear** (cleared with at most one wipe), chain_pulls, ready_pulls (pulls started with the party ready,
capped), corridor_packs / corridor_first / corridor_cleared / corridor_in_order / corridor_share (only stages with a corridor arena: G2), drill_clean / drill_extra / drill_pulled / drill_gap / drill_pack (only with a pull-drill arena: D1;
`drill_pack` indexes `episode_categories.drill_pack` = pack_1.. ), without_stand_in, clear_standin, clear_allbot, role_tank/role_healer/role_damage (the row's place in the party), deaths_tank/_healer/_damage (the row's deaths in its place),
seat_deaths, boss_<name> (that boss killed this run; one per boss of the maps the stage's Wing arenas use: Ragefire Chasm: oggleflint, taragaman, jergosh, bazzalan; Deadmines: rhahkzor, sneed_shredder, sneed, gilnid, smite, greenskin,
cookie, vancleef; Wailing Caverns (held out, in D2/D3's `heldout` arena): anacondra, cobrahn, kresh, pythas, skum, verdan, serpentis), wing_started, wing_arena, wing_tier, wing_marks, and the families
`wing_mark<i>_packs<w>`, `wing_mark<i>_yard`, `wing_mark<i>_seconds` for i < `EXPLORE_MARKS` (8) and w < `EXPLORE_PACK_WORDS` (4) (Go-Explore cells reached).
The sim_metrics extraction lists all boss names for all four stages (a superset; the exact list is stage.json `episode_info`).

### Which stages report what (union checked with `sim_metrics.py`)

M1 221 exact names, M2 225, M3 222, M4 215, C1-C3 228, G1 247, G2/D1/D2/D3 290 + 3 families (a superset where the C++ guards a column by a condition). A name no stage reports but a yaml asks for reads as "never met" and
silently stalls a ladder (sim_metrics.py docstring); `test_metric_names.py` checks the live yamls against the extraction.

## 4. Reward columns `reward_<term>`

One column per term the stage pays: the episode sum of that term for the seat (`RewardLedger::Episode(term)`), after the shaping scale / cost scale / tier. Term names from `RewardTermName` (CombatReward.cpp). The ledger's
**category** (`RewardTermCategory`, RewardLedger.h:162): **Outcome** (what the stage is for), **Cost** (what the outcome costs), **Shaping** (a nudge that fades). Outcome + Cost = `score_outcome`.
`PricesNoise` terms (repeat, jitter, aimless, effort, fidget, stuck, wall) are paid times the cost ladder's scale; Shaping times the shaping fade's scale; the rest in full.

| Term | Category | Stages that pay it |
|---|---|---|
| arrive | Outcome | M1 M2 M3 |
| follow_kept | Outcome | M4 |
| regroup | Outcome | M4 |
| door_opened | Outcome | M3 |
| survived | Outcome | C1 C2 C3 G1 |
| interrupt_landed | Outcome | C1 C2 C3 |
| kill | Outcome | C1 C2 C3 G2 D1 D2 D3 |
| clear | Outcome | C1 C2 C3 G1 G2 D1 D2 D3 |
| drill_hold, drill_focus, drill_keep | Outcome | G1 |
| pull_clean | Outcome | G1 G2 D1 D2 D3 |
| ready_pull | Outcome | G2 D1 D2 D3 |
| death | Cost | all |
| step_cost | Cost | all but M4 |
| timeout | Cost | G2 D1 D2 D3 |
| teammate_death | Cost | C1 C2 C3 G1 G2 D1 D2 D3 |
| early_pull | Cost | G1 G2 D1 D2 D3 |
| pull_extra | Cost | C1 C2 C3 G1 G2 D1 D2 D3 |
| repeat, jitter, aimless, effort, fidget (noise prices) | Cost | all |
| stuck, wall (noise prices) | Cost | M1 M2 M3 M4 |
| lost | Cost | M4 G2 D1 D2 D3 |
| wrong_object | Cost | M3 |
| blocking | Cost | M4 |
| away | Cost | C1 C2 C3 G1 G2 D1 D2 D3 |
| hurt, fire_hurt | Cost | C1 C2 C3 |
| idle | Cost | G2 D1 D2 D3 |
| damage_dealt | Shaping | C1 C2 C3 G1 G2 D1 D2 D3 |
| approach | Shaping | G2 D1 D2 D3 |
| threat, teammate_threat, teammate_damage_taken, teammate_healing, revive, stall | Shaping | G1 G2 D1 D2 D3 |
| facing, progress | Shaping | M1 |
| sighting | Shaping | M2 M3 |
| new_ground, room_seen | Shaping | M2 |
| goal_reached, goal_switch, hazard, healing_mana, self_healing, combat_clock | Shaping | all |

`goal_progress` has a name and a category (Shaping) but no encounter or scenario claims it, so there is no `reward_goal_progress` column in any live stage. `reward_unknown` is the fall-through of `RewardTermName`, never a column.
The stage.json `reward_terms` map is the authoritative list for a build; the learner's reward audit (`rewards.audit`) warns when a Shaping term earns more than 0.5 (`MAX_SHAPING_SHARE`) of the largest positive Outcome/Cost term, at most every
25 updates (`WARN_EVERY`), reading `episode_reward_*`.

## 5. Derived measures (computed by the learner)

From `EvalResult.summary` (evaluation.py):

| Name | Definition | Used by |
|---|---|---|
| score | mean of `score_outcome` over scored rows (or the return when `eval.score: return`). | tracker, best.pt, eval.csv. |
| stderr | std error over **episodes** (rows of one episode averaged first). | margin of a "new best" (z x combined stderr, min_improvement 0.02 x |best|, min_improvement_abs 0.01). |
| return | mean episode return incl. shaping. | reported beside score. |
| arrived_at_rung | `p*rate_no + (1-p)*rate_with` where p = mean `compass_withhold_chance`, rate_no = arrived_no_compass / compass_withheld, rate_with = arrived_with_compass / compass_present (evaluation.py ratios). The arrival at the training rung's own mix. | M1 fade and cost gate. |
| found_deepest | found rate over episodes with `deep_room > 0.5`. | M2 headline/target. |
| clear_standin, clear_allbot | per-event means (see PER_EVENT) = clear rate with / without the stand-in. | G2, D1-D3 headlines. |
| standin_gap (progress only) | `eval_clear_allbot - eval_clear_standin`, in `ProgressWriter.arm_evaluated` for arm `with_human` (ARM_SPLITS). | G2, D1-D3 headline (target <= 0.1). |
| `<metric>_<arm>` | a headline metric named `<metric>_with_human` / `_with_partners` resolves to that arm's reading. | progress.json `eval_<metric>_<arm>`. |
| bands / layouts / specs / castings / arenas / builds / difficulties / up_to / top_rung / categories | grouped copies of the means, see below. | eval.jsonl, `casting_weights`, convergence. |
| realism_emd, realism_emd_<context>, realism_disc | earth mover's distance between the seats' motion histograms and the players' (human_reference.json), per context; discriminator mean output. | eval.csv extra columns (only with `style.reference`). |

Per-class convergence signals (stage.py, `ClassState.missing`): `score` (plateau of the class's measure: `convergence.measure` or the score), `kl` (LR-normalised approx_kl under `convergence.kl` 0.003 over the window), `entropy`
(entropy / ln(allowed actions) slope within `entropy_slope` 0.01 and above the floor), `ladder` (rung settled), `top_rung` (training share at the top rung >= `TOP_RUNG_SHARE` (0.9)); these are the words in `weakest_missing`.

## 6. Training-row columns (metrics.csv; also progress.json)

Listed in file-formats.md ("metrics.csv"). The measures the sim and operators read: `env_steps_per_sec`, `reward_per_decision`, `entropy` (against `allowed_actions`: the mean legal actions per decision), `value_loss`, `policy_loss`, `approx_kl`
(joint) and `approx_kl_move` (movement part), `clip_frac`, `explained_variance`, `actor/critic_grad_norm`, `lr_scale`, `shaping_scale` and `cost_scale` (current ladder scales), `ladder_collapsed` (the fade's rung while its gate has collapsed, else -1),
`ladder_stalled` (the rung whose gate metric has sat flat, else -1), `frozen_layouts`, the partner/stand-in counters (`partner_rows`, `stand_in_episodes`, `stand_in_unfielded`), `look_*` (look entropy and shares of turning/pitching/zooming and the zoom
commands), goal-head and foresight stats, optional style stats.
`approx_kl` handling: a stall warning prints when `approx_kl / lr_scale` stays under 0.0015 for 10 updates (train.py:STALL_KL, never acts).

## 7. progress.json status measures

See file-formats.md for the key list. The sim's `forge status` shows: stage position, `env_steps/total_env_steps`, ETA from `patience`, `eval_every`, `evals_since_best`, `last_eval_score`, `best_score`, `baseline_score`, `converged_layouts`, `weakest_layout`
+ `weakest_missing`, the **headline table** (`status_headline` list; each as `eval_<metric>` beside `episode_<metric>`, compared with `status_targets` `metric>=x;metric<=y`), the excluded classes (`status_excluded`), the `stand_in` note, `ladder_collapsed` /
`ladder_stalled` alarms, reward/entropy/value loss/KL, the first `episode_*` of the training row and `nonfinite`. A headline name not produced by the stage simply shows no value (silent).
Targets are readouts, never gates; convergence alone ends a stage.

## 8. Evaluation tables: how they are built

* `run_evaluation` (evaluation.py:451) sends MODE evaluation with the seed base `eval.seed` (1000) and the episode count, plays decisions until every seed index of its share has ended (or `max_decisions`), collecting for each
  ended seed one row per present, non-excluded agent: its running return, its episode-info row, its layout name, its action counts and "allowed" counts, the kinematic track (if motion is collected) and an optional decision trace.
  It then sends MODE training (with the stand-in flag the training run uses) and returns the fresh STEP.
* **Seeds**: seeded episode i plays (class, build) pair `i % pairs` (the sim draws it), so every pair gets an equal share whatever `layout_sampling` weights train on; `FirstSeed`/share let data-parallel ranks and cluster sims split one evaluation (`weighted_share`, `ClusterEnv.set_mode`/`_shares`).
* **Plain evaluation** (`policy: learner`, argmax unless `eval.deterministic: false`): "all bots" (no stand-in unless the stage always has one). `learner_sampled` repeats it with sampled actions every `eval.sampled_every` evaluations and adds `argmax_gap` for score, died, timed_out, arrived.
* **Arms** (`eval.arms`, `EVAL_ARMS = with_human, with_partners`; every `eval.arms_every` evaluations and on the last): same seeds again. `with_human` sets `MODE_FLAG_STAND_IN` so every party has the stand-in in one seat, its row played by a frozen partner (needs `cast.partners`
  stages/paths; skipped otherwise); `with_partners` puts the fixed partner set (`cast.partners.eval_partners`, default the pool's stage/path members) in some seats of every party. Partner rows are `excluded` (not scored). Reported as separate eval.csv rows
  `with_human` / `with_partners`, and into progress.json via `arm_evaluated`. Live yamls: G1, G2, D1 use 64 episodes each; D2, D3 32 (arms_every 2; G1-D1 all arms every second evaluation).
* **Held-out sweeps** (`eval.heldout`, arena -> episodes, validated against stage.json: must exist and be `eval_only`): played on seeds `eval.seed + 7919` with MODE `Arena = index+1`, reported as policy `heldout_<arena>`; due every `heldout_every` evaluations, on the stage's last
  evaluation, and on a new best if `heldout_on_best`. Never read by the tracker or controller. Live: M2 `sweep: 195` (every_eval 1000, not on best: effectively the last), M3 `sweep: 60`, D2 and D3 `heldout: 16` (Wailing Caverns).
* **Tables inside one summary** (all with the same shape of means): `bands` (by `level`: 1-20, 21-40, 41-60, 61-80), `layouts` (by class, when more than one), `specs` (by build name across classes), `castings` (`<class>_<build>`, the grain `layout_sampling` weights),
  `arenas` (when the stage has more than one arena), `builds` (talent plan standard/noisy/random, when more than one occurs), `difficulties` and `up_to` (by `difficulty` tier, when more than one; `up_to[t]` = tiers <= t, per layout and per casting),
  `top_rung` (rows with `at_top_rung > 0.5`, else the highest tier; per layout), `categories` (`"<column>=<name>"` for each `episode_categories` column: seek_room/object, interact_site/object, sight_object, objective_corner, drill_pack).
  `format_summary` prints a subset of these as the console table (learner / baseline).
* **What feeds back**: after the plain evaluation `casting_weights` turns `castings` into the WEIGHTS vector (score shortfall and `layout_sampling.metric` shortfall in standard deviations, role metrics, capped at `max_ratio` 4, mean 1); `failed_seeds(metric)` (rows with value < 1) go to REPLAY at `replay_fraction` 0.2.
* The two derived summaries named `livelocked` and `clean_kill` were removed in 57206f164 (a live eval.jsonl still holds them).

## 9. Who uses which measure (live yamls, resolved through `extends`)

Resolved with a small script over the twelve files (`extends` merged, `null` drops a key). `fade`/`costs` gate metrics are at the value shown; `require_plateau: false` on every live fade; M4's cost ladder has no gate. All live stages: `fade.enabled`, `costs.enabled: false`, `layout_sampling.enabled` with `replay_fraction 0.2`.

| Stage | convergence.measure | fade gate (value; rungs) | costs gate | layout_sampling.metric | eval episodes / every | arms | held-out |
|---|---|---|---|---|---|---|---|
| M1 | arrived | arrived_at_rung 0.8; rungs [0.0] | arrived_at_rung 0.8 (off) | arrived | 512 / 5M | | |
| M2 | found | found 0.8; [1, .5, .25, 0] | found 0.8 (off) | found | 78 / 10M | | sweep 195 |
| M3 | right_object | right_object 0.8; [1, .5, 0] | right_object 0.8 (off) | right_object | 64 / 10M | | sweep 60 |
| M4 | follow_kept_share | follow_kept_share 0.75; [1, .5, .25, 0] | none | follow_kept_share | 64 / 10M | | |
| C1 | won | won 0.7 | won 0.7 (off) | won | 240 / 10M | | |
| C2 | won | won 0.7 | won 0.7 (off) | won | 240 / 10M | | |
| C3 | survived | survived 0.7 | survived 0.7 (off) | survived | 240 / 10M | | |
| G1 | won | won 0.6 | won 0.6 (off) | won | 384 / 10M | human 64, partners 64 | |
| G2 | cleared | cleared 0.6 | cleared 0.6 (off) | cleared | 128 / 10M | human 64, partners 64 | |
| D1 | cleared | cleared 0.7 | cleared 0.7 (off) | cleared | 192 / 10M | human 64, partners 64 | |
| D2 | full_clear | full_clear 0.5 | full_clear 0.5 (off) | full_clear | 64 / 20M | human 32, partners 32 | heldout 16 |
| D3 | bar_clear | bar_clear 0.5 | bar_clear 0.5 (off) | bar_clear | 64 / 20M | human 32, partners 32 | heldout 16 |

(C1-D3 fade rungs [1, .5, .25, 0] with `moving_classes: 1000`.) Gate semantics: a fade rung steps when the evaluation's `gate_metric` mean is >= `gate_value` (at least one evaluation at the rung); it raises a collapse alarm (`ladder_collapsed`) if the metric falls, and a stall warning
(`ladder_stalled`, `stall_evals` 4 and `stall_env_steps` 20M) if it sits flat; ladders re-baseline stage convergence at every gate-stepped rung (`ConvergenceController.rebaseline`, writes `best_rung<k>.pt`).

Headlines (`status.headline`) and targets, per stage (target in brackets, a readout only):

* **M1**: arrived [>=0.95], arrived_no_compass, arrived_with_compass, compass_withheld, arrive_seconds_sight [<=18], time_ratio_sight [<=1.1], time_ratio_corner, stop_distance [<=0.5], overshoot [<=0.5], stops_near [<=1.2], course_kinks [<=5], control_changes_per_minute, wall_seconds [<=0.5], timed_out [<=0.02], died [<=0.01].
* **M2**: found [>=0.95], found_hallway, found_doorway, found_room, found_deep, seek_rung, found_deepest [>=0.9], find_seconds [<=90], sight_seconds [<=60], sight_to_arrival [<=15], rooms_looked, rooms_before_found [<=12], revisit_rate [<=0.2], objective_visible, wall_seconds [<=2], timed_out [<=0.05], died [<=0.01].
* **M3**: right_object [>=0.9], right_distinguish/switch/key [>=0.9], interact_rung, door_by_lever, key_used, wrong_objects [<=0.1], lever_pressed, act_refused_locked, right_seconds, sight_seconds [<=30], sight_to_arrival [<=15], wall_seconds, timed_out, died.
* **M4**: follow_kept_share [>=0.9], regroup_share [>=0.9], regroup_seconds [<=5], lost_seconds [<=5], blocking_seconds [<=3], deaths, rejoin_seconds, rejoined [>=0.9], leader_route_share, difficulty, wall_seconds [<=2], died [<=0.02].
* **C1**: won [>=0.9], survived [>=0.9], kills [>=3], kill_seconds [<=25], hurt_share [<=0.8], deaths, rejoin_seconds, combat_rung, target_in_view [>=0.8], selected_share, ally_deaths [<=0.1].
* **C2**: won [>=0.85], survived [>=0.85], packs_cleared [>=3], extra_pulls [<=0.1], interrupts, interrupt_earnings [<=0.3], fire_share [<=0.1], hurt_share [<=1.5], deaths, rejoin_seconds, combat_rung, target_in_view [>=0.8].
* **C3**: survived [>=0.8], won [>=0.8], packs_cleared [>=3], deaths, rejoined [>=0.9], rejoin_seconds [<=60], dead_seconds, away_seconds, rest_seconds, extra_pulls [<=0.1], hurt_share [<=1.5], combat_rung.
* **G1**: won [>=0.7], won_hold/keep/focus/pull [>=0.7], hold_share [>=0.8], kept_share [>=0.8], focus_share [>=0.6], clean_share [>=0.9], extra_pulls [<=0.1], party_deaths, wipes [<=0.05], rejoined [>=0.9], rejoin_seconds [<=60], roles_rung. Excluded: death_knight.
* **G2**: cleared [>=0.7], corridor_share, corridor_in_order [>=3], chain_pulls [<=0.25], ready_pulls, wing_wipes [<=0.3], wing_rejoin_seconds [<=60], clear_allbot, clear_standin, standin_gap [<=0.1], deaths_tank, deaths_healer, wing_rung. Excluded: death_knight.
* **D1**: cleared [>=0.8], drill_clean, drill_extra [<=0.1], drill_pulled, ready_pulls, wing_wipes [<=0.1], clear_allbot, clear_standin, standin_gap [<=0.1], deaths_tank, pull_rung. Excluded: death_knight.
* **D2**: full_clear [>=0.7], cleared [>=0.7], wing_cleared_share, wing_wipes [<=1], boss_oggleflint/taragaman/jergosh/bazzalan, chain_pulls [<=1], wing_rejoin_seconds [<=90], clear_allbot, clear_standin, standin_gap [<=0.1], wing_rung.
* **D3**: bar_clear [>=0.7], cleared [>=0.7], full_clear [>=0.7], wing_wipes [<=1], boss_rhahkzor/sneed/gilnid/smite/greenskin/cookie/vancleef (vancleef [>=0.7]), deaths_tank/healer/damage, chain_pulls [<=1], clear_allbot, clear_standin, standin_gap [<=0.1], wing_rung.

The `eval.report` lists (what the console and eval.jsonl summaries carry beyond the headline) are each stage's `reward_*` columns plus the stage's own measures; see the yamls ([config-yaml.md](config-yaml.md)). Names in `eval.report` that the stage does not report are ignored
(`present = [c for c in columns if c in info_names]`).

## Tests that pin names and measures

`test_metric_names.py` (every yaml metric must be a column, a reward column or a derived name; compares `sim_metrics.py` with real stage.json), `test_status_headline.py`, `test_layout_metrics.py` (layouts.csv and per-class means), `test_outcome_score.py` (score column),
`test_shaping_fade.py`, `test_evaluation.py`, `test_heldout.py` (held-out arena validation), `test_partners.py` (arms), `test_run_logger.py` (CSV rotation), C++ `RewardLedgerTest`, `ReportCadenceTest`, `StandingTest`. See [tests.md](tests.md).

## Observed issues

* `respawns` and `rises` in CombatEncounter.cpp:89-90 are the same expression (`Clock.Rises`): a duplicate column.
* `CombatReward.cpp` hosts `RewardTermName` for terms that are no longer combat-only (known; the file name no longer fits).
* `reward_goal_progress` never exists: `GoalProgress` has a name and category but nothing claims it.
* The columns `loot` (goal kind) stays in the goal space while looting is forbidden (principle 6).
* D3's yaml `eval.report` contains `boss_sneed_shredder`; D3's headline uses `boss_sneed`: both exist, but the headline omits the shredder phase.
* `episode_info` ordering depends on `Add` order across encounters; a stage built with a different encounter order changes column positions (`stage_json_diff.py` guards).
* `sim_metrics.py` yields a superset (guarded columns, loop artefacts `*_unknown`, `act_refused_none`); only stage.json lists the exact set.
* `evaluation.RATIO_METRICS` (`arrived_at_rung`) is defined but the ratio is computed inline in `summary`; the constant is only read by `sim_metrics.evaluation_names` (UNVERIFIED that nothing else uses it).
* `standard_error`'s docstring refers to a nonexistent `animus.gates.noise_allowance`.
* Headline names missing from a stage are silently blank in `forge status` (no startup check besides the tests).
* `core` columns for pets, stealth, heals etc. are mostly dead weight in the movement stages (about 100 columns, all zero), inflating every STEP's `episode_info` for ended envs (K x A x D floats).

## Reviewer notes

* Before renaming a column: grep the yamls (`status.headline`, `targets`, `eval.report`, `fade/costs.gate_metric`, `layout_sampling.metric`, `convergence.measure`, `role_metrics`), `PER_EVENT`, `ARM_SPLITS`, `casting_weights`, the sim's `Progress.cpp`, the eval video outcome list (`found, arrived, won, cleared, success, survived`), and the deploy gate.
* A gate on a metric an encounter stops reporting never fires and the ladder never steps (no error): consider failing at startup.
* Unit conventions are not enforced anywhere; this table is the only record. Yards for distances, seconds for times named `*_seconds`, shares are 0..1 fractions of decisions unless stated.
