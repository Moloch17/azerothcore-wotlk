# 0021: Decisions last a jittered game time in training, as they do at the realm

**Date:** 2026-10-08 (owner order: "inject tick jitter in training", item 7 of the train/ship parity audit)

**Decision.** Every decision of every stage, in every mode (training, evaluation, `forge run`), lasts a variable game
time: the nominal `AnimusForge.DecisionMs`, less the overshoot carried over from the last decision, plus this decision's
own overshoot. The overshoot is a uniform body `U(0, Decision.JitterMs)` (default 50 ms, a stock world update) plus, with
`Decision.SpikeProb` (default 0.02), a spike of `U(50, Decision.SpikeMaxMs)` ms (default 400). The world advances by
exactly that time: maps, auras, casts, cooldowns, regeneration, the player controller and the episode clock all integrate
the real step. `JitterMs 0` and `SpikeProb 0` is the exact tick of before.

**Why this shape (the realm's behavior).** The realm's companions decide in `CompanionParty::UpdateMember`:
`SinceDecisionMs += diff; if (SinceDecisionMs >= DecisionMs) { SinceDecisionMs %= DecisionMs; Decide(); }` (the same
pattern in `Direct`). The tick that crosses the threshold overshoots it by `o`, and the `%=` carries `o` (mod
`DecisionMs`) into the next interval. So the real interval is

    dt = DecisionMs - carry + o,   carry = (the previous o) mod DecisionMs

which is `DecisionMs + o_n - o_(n-1)` for an overshoot under a decision. The mean is `DecisionMs` however long the
ticks are (the owner's first formulation, `DecisionMs + overshoot`, would have drifted the mean by +25 ms = 10%, and
biased every nominal-dt conversion the learner makes); the spread is a tick either side (200-300 ms for a 50 ms body); a
spike (one long tick) makes one long interval and a short one after it (a spike of 250 ms leaves a carry of about 250 and the next decision comes on the next tick:
intervals of 5-50 ms exist, about 2% of decisions at the defaults, where the policy meets a near-duplicate observation and
a near-zero reward -- faithful to the module, and an owner option below). A spike over a whole `DecisionMs` swallows the
periods it spans (the module's single `if` per update fires once), which the formula reproduces. The audit
(`train-ship-parity.AUDIT.md` C3-3, D-7) had the realm tick only from config defaults (`MinWorldUpdateTime 1`,
`MapUpdateInterval 10`) and could not measure it; the defaults here are an assumption until `python -m animus.human
parity` has realm tick data (`timing()` already reports `jitter_recommended` and the measured p5/p95).

**Granularity: one draw per decision for the whole pool, not per env or map.** The forge's game clock is global
(`GameTime::AdvanceGameTimers(diff)` in `World::Update`): spell and item cooldowns, the global cooldown, proc ICDs,
respawns and the task schedulers read it. A per-map diff (the machinery exists: `Map::AccrueTickDiff` /
`TakeAccruedDiff`) would age auras, casts and regeneration by a different amount than the cooldowns measured against the
one clock, which is not honest time. A world tick with one length is also what the realm does (every companion shares the
world thread's tick). The cost is that the 192 envs of a pool share a decision's length, so a rollout step has one dt
across the batch; the draw is independent from step to step, which is what makes the policy meet variable cadence. The
stream is `splitmix64(seed, draw index)` with the seed a hash of `Decision.Seed`, the scenario name and the cluster
advertise address: reproducible per machine and scenario, different across a cluster's workers, and never the world
thread's random numbers (an evaluation episode reseeds those; a draw there would move every roll after it).

**Mechanism.** `ForgeMain`'s update loop asks `Forge::NextWorldTickMs(nominal)` for each tick. When the tick is the first
of a decision (`_ticksSinceDecision == 0`) the module plans the decision (`Animus::DecisionClock::Plan`): the length
`max(ticks, nominal - carry + o)` is split into the decision's `TicksPerDecision` ticks (`length / ticks` each, the
remainder on the last), so a long carry never makes a tick negative and a movement stage's five 50 ms ticks stay five
ticks of one decision (of 40 to 60 ms without a spike). Under half-batch every world tick is its own plan of half a
decision, and a group's interval is the sum of two consecutive ticks (the same formula over every second overshoot); the
mean then drifts about +7 ms with the default spikes (a spike swallows periods of 125 ms, not 250). The prologue hands each
group the time its maps were owed (`_groupAccruedMs`, the same time `MapMgr::ForgeTickDiff` gives them) and `AdvanceClock`
adds it to the env's `EpisodeElapsedMs` and `Env::StepAccruedMs`; `EnvPool::ObserveEnv` turns the latter into `Env::StepMs`
(the time the decision just scored took) before `Reward`.

**What the rewards and the observation do.** Per-second terms are charged for the time that went by:
`StageScenario::StepScale(env)` (was `DecisionScale()`: the step cost, threat, tank, role terms) and `StepMs(env)` (was the
constant `DecisionMs`: the distance a speed asks for in a decision, the hazard and combat clocks, the time tallies, the
protect hold, and the other judging thresholds). `StepMs` is the interval a decision's reward closes; the judging of the
actions that follow it (`ApplySeatAction`) uses the same value, which is the interval just ended. It is `DecisionMs` before
an env's first decision. What stays nominal on purpose: `SeatView::DecisionMs`, which ages the entity and map memory and the
free look in the observation, and the breath spent. The realm hands its models `DecisionMs` for those every time, so a
training that aged them by the real step would put the policy off the distribution it ships into. The episode clock and the
features built from it (`EpisodeTime`, `CombatTime`, `PullTime`, the unseen age, `STATE_EPISODE_TIME`) carry the real
elapsed time in training; the module keeps its own clock from the real tick diff (`_nowMs += diff`) or fabricates it (audit
C2-f), so they are not worse for it.

**What it does not do (options for the owner).**

- A floor on a decision's length (e.g. `max(length, nominal / 2)`) would remove the 5-50 ms intervals the realm's carry
  rule produces after a spike; it would also stop being the module's behavior. Not done.

- The policy is not told the step: no observation feature carries `dt`. The module knows its accumulated tick time
  (`SinceDecisionMs` before the `%=`), so a `last_dt` input is possible live, but a layout change is an owner decision.
- The learner's discounts stay compounded to the nominal `decision_ms` (the mean is within 2.5 ms of it at the defaults:
  `Spec.mean_decision_ms`). A per-step dt on the wire (STEP) would allow `gamma ** (dt / reference)`.
- The module's memory ageing could be switched to the real step (pass the accumulated time) and training with it; both
  sides must move together.
- Evaluation episodes play jittered too (it is the distribution the model ships into); a deterministic evaluation is
  `JitterMs 0` / `SpikeProb 0` for that run.

**Operations.** The three keys are in the cluster fingerprint (`decision=<DecisionMs>/<ticks>/<JitterMs>/<SpikeProb>/
<SpikeMaxMs>`; a worker that differs is refused) and in `forgectl doctor`'s must-match sim keys. `Decision.Seed` is not.
`forge status` shows a `decision time` row (mean, p95, min, max, spike share) since the scenario started. SPEC (protocol
27) tells the learner the three values (`Spec.jitter_ms`, `spike_max_ms`, `spike_prob`; `mean_decision_ms`); the learner
prints them with its decision line. Changing the keys under a run you resume is a domain shift the learner is not told per
step: nothing refuses it (the fingerprint only compares machines running now); resume it deliberately and expect the value
function to re-fit.

**Consequences.** Return noise: a per-second cost varies with the step it is charged for. Movement stages (five 50 ms
ticks): positions integrate the real step through the controller's sub-steps (`MAX_SUBSTEP 0.05 s`). Cooldown-based
combat: the global cooldown (1.5 s) and cooldowns fall in 4-8 decisions, not a fixed 6. Episode length in decisions varies
(a 60 s episode is ~240 decisions on average); the steps/second figures move with the mean, which is within 1%. Throughput
is unchanged to first order (a decision is still `ticks` world updates).
