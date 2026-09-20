# A director for each side

One policy a team, above the ten seats, choosing what each of them is *for* over a whole match: escort the
carrier, hold our base, intercept theirs. The seats keep choosing how -- which spell, which target, when to
run -- as they do now.

## What already exists, and is reused

- **A goal head.** `SeatGoal` (Fight, Control, Recover, Protect, Position, Prepare) chosen every
  `mappo.goal_every_decisions` (16) and held in between, with a goal-conditioned actor *and* critic
  (`LayoutActor`, `mappo.goal_count`). The pattern for "choose on a slow cadence, hold until the next" is
  `trainer.py:284` -- `state.age % goal_every == 0`.
- **Recurrence that already spans the episode.** The GRU state is carried across rollout boundaries: "what it
  remembers is bounded by the episode, not by rollout_length" (`train.py`). A director needs no new memory
  machinery, only a coarse enough step that its own rollout covers the match.
- **Heterogeneous layouts.** The spec already carries per-layout obs and action sizes ("18 layouts, obs up to
  1087, actions up to 126"), so a director is just another layout with its own shapes.

## Shape

**A director is a 19th layout and two more agents an env** -- `agents_per_env` 20 -> 22, one a side. It then
rides the existing socket, rollout buffer, per-layout actor and MAPPO update with no new transport and no second
training loop. This is by far the cheapest route to something real.

- **Observation** (~100 floats, trivial beside a seat's 1087): its ten seats -- position relative to both bases,
  health, role, alive, in combat, current assignment -- plus both flag states, who carries them, the score and
  the clock.
- **Action:** one `TeamAssignment` per seat, emitted every `director_every_decisions`.
- **Reward:** the team reward it already shares. A capture pays the side, which is exactly the director's
  objective, so nothing new has to be invented to score it.

## Decisions, settled

**1. A parallel `TeamAssignment` channel, not `SeatGoal`.** Overloading six combat-shaped goals with strategy
would force both through one channel built for the latter. A seat told *escort the carrier* still chooses Fight
or Recover underneath. First vocabulary: `TakeFlag`, `EscortCarrier`, `DefendBase`, `InterceptCarrier`,
`ReturnFlag`, `Free`.

**2. The director gets its own discount; the seats keep theirs.** A seat's choices pay off in seconds and its
credit window is already ~17 s (`GAE trace 0.98508`), of which gamma's nominal 100 s is mostly the critic's
target. Raising the seats' gamma to see ten minutes would buy variance across 2,560 agents for something only
two of them need. The director instead runs gamma ~0.996 on its own 2.5 s steps -- about a ten-minute horizon.
This is the one piece that is **not** reuse: the trainer applies gamma per policy, not per layout.

## Ten minutes of context, concretely

| what | value | why |
| --- | --- | --- |
| director cadence | every 10 decisions (2.5 s) | 256 of its steps then span 640 s |
| its rollout | 256 director steps | ~10.7 minutes of *backpropagated* context, not merely remembered |
| gamma | ~0.996 per director step | 240 steps = 600 s |
| `EpisodeSeconds` | 420 -> 900 | a ten-minute window cannot live in a seven-minute episode; real Warsong has no timer |

The distinction that matters: recurrence already *carries* a match's worth of state, but gradients only flow
within a rollout. Without the coarse cadence the director would remember ten minutes and be trained on 64
seconds.

## Order of work

1. `TeamAssignment` on `SeatView`, and the seats observing theirs (cheap, independent, testable alone).
2. The director layout: observation, action space, and its two agents an env.
3. Director transitions stored at the director's cadence -- the real trainer work. Without it the cadence buys
   nothing.
4. Per-layout gamma.
5. `EpisodeSeconds` 900.

## What it waits on

Stage 18 scoring at all. A director commanding "take their flag" inherits every failure the flag stage has
already had, with one more layer between the command and the cause. Five defects were found there by measuring;
the sixth is still open.
