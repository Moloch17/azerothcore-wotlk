# A director for a team, in every scenario that has one

One policy above a side's seats, deciding what the team is doing -- who to kill, where to gather, what posture to
hold, whose turn it is to interrupt -- while the seats keep deciding how. Built once and used everywhere a team
fights: arena, battleground, party, dungeon, raid.

## The shape, and why it is this shape

**One shared director layout, not one per content.** A director is a layout like `warrior_dps` is, seeded down
the stage chain, so the network that learns to focus fire in a party fight is the network that calls a kill
target in arena and an interrupt rotation in a raid. A director per content would learn "concentrate on one
target" five separate times. Everything below follows from wanting that transfer.

**Two more agents an env where a scenario asks for one**, one a side. `agents_per_env` rises by two only in
directed arenas, so the socket, rollout buffer, per-layout actor and MAPPO update all carry it with no new
transport and no second training loop.

## The action space is team-level, not per seat

An assignment per seat does not scale: forty slots in a raid, and no shared network with a 2 v 2. The director
instead emits a handful of categorical heads, the same shape as the goal head that already exists:

| head | domain | why it scales |
| --- | --- | --- |
| Posture | attack, defend, protect, recover, regroup | one value for the team |
| Focus target | one of N enemy slots | fixed slots, presence-flagged |
| Rally | own base, their base, the carrier, the boss, spread, stack | named places, never coordinates |
| Duty seat | one of MAX_SEATS slots | "you interrupt next", "you hold that one" |

A seat then reads four small fields: the team's posture, the focus target, the rally point, and whether it is
itself the nominated duty seat. Identical whether the team is two or forty.

Focus and duty are the two a seat provably cannot produce alone: nothing in seat 7's own view says it is next in
the rotation, and ten seats each choosing a target is the classic way a team loses a fight it should win.

**Observation:** fixed slots with presence flags, as `FRIEND_SLOTS`, `SPOTLIGHT_SLOTS` and `RaidView` already
do -- per seat position relative to the named places, health, role, alive, in combat; per enemy slot the same;
then the objective state (flags, score, boss, clock). Content-agnostic by construction.

## What is reused, and the two things that are not

Reused: the goal head's "choose on a slow cadence, hold until the next" (`trainer.py:284`,
`state.age % goal_every == 0`); recurrence that already spans an episode -- "what it remembers is bounded by the
episode, not by rollout_length" (`train.py`); per-layout obs and action sizes, already heterogeneous ("18
layouts, obs up to 1087, actions up to 126").

New work, both in the learner:

1. **Director transitions stored at the director's cadence.** Without this the cadence buys nothing: it would
   remember ten minutes and be trained on sixty-four seconds.
2. **Per-layout gamma and lambda.** The trainer applies both per policy today.

## Horizons: the director's, not the seats'

The seats keep what they have. A seat's decisions pay off in seconds, and its credit window is already about
17 s (`gamma 0.99750`, `GAE trace 0.98508`) -- gamma's nominal 100 s is mostly the critic's target. Raising the
seats' gamma to see ten minutes would buy variance across thousands of agents for something two of them need.

| director | value | why |
| --- | --- | --- |
| cadence | every 10 decisions (2.5 s) | 256 of its steps span 640 s |
| rollout | 256 director steps | ~10.7 min of *backpropagated* context, not merely remembered |
| gamma | ~0.996 a director step | 240 steps = 600 s |
| lambda | ~0.98 to start | ~100 s of credit, against a seat's 17 s. It takes ~240 low-noise decisions a match
where a seat takes ~1680, so it affords the longer trace |
| `EpisodeSeconds` | 420 -> 900 where wanted | a ten minute window cannot live in a seven minute episode |

## Opt-in, per arena

A flag on `ArenaDefinition`. Solo stages -- duel, pack, gauntlet, travel, flight -- have one seat and would pay
for an agent with nothing to say. Opting in also lets this arrive gradually instead of changing the agent count
of eighteen stages at once, which would invalidate every checkpoint's layout set in a single step.

Cost where enabled: two agents an env. Stage 18 goes 20 -> 22, about a tenth. A five-man party stage goes 4 -> 5,
about a quarter -- proportionally worst on small teams, which is the second reason for opt-in.

## Where it earns its keep

- **Arena.** The best case and the weakest spot for per-seat policies: "kill the healer" is *the* arena decision,
  and CC chains are cross-seat by definition -- who sheeps, who fears, in what order, without overlapping
  diminishing returns. Stages 7 and 11 are 1 v 1 today, where a director has nothing to coordinate: it is the
  reason to add **2 v 2 and 3 v 3**, which `SeatPlan::Teams` now makes cheap (`TEAM_SEATS` of 2 or 3).
- **Dungeon and raid.** Stages 13 and 14 are forty seats in eight groups, and a raid leader is exactly a
  director: kill order, interrupt rotation, spread and stack for mechanics, who peels. Stages 15, 16 and 17
  (hazards, tanking, triage) are drills for precisely these.
- **Party.** Stages 4 and 5, the same machinery over five seats: focus, who controls what, who guards the owner.

## Order of work

1. `TeamOrder` on `SeatView` (posture, focus, rally, duty seat) and the seats observing theirs. Cheap,
   independent, testable with a scripted director before any learning.
2. The director layout: observation slots, the four heads, two agents an env, opt-in flag.
3. Director transitions at the director's cadence -- the real trainer work.
4. Per-layout gamma and lambda.
5. `EpisodeSeconds` 900 where wanted; 2 v 2 and 3 v 3 arenas as the first small directed content.

## What it waits on

Stage 18 scoring at all. A director commanding "take their flag" inherits every failure the flag stage has had,
with one more layer between the command and the cause. Six defects have been found there by measuring; the last
is still open.

# The curriculum the director needs

The ladder as it stands teaches a director nothing. Team size goes 1, then 4 at stage 5, then 40 at stage 13,
then 20 at stage 18, and the two stages called self-play -- the arena and the flag -- are one seat a side, where
there is nobody to command. A shared director layout would meet its first team at stage 5 and its second at
forty seats.

Four principles shape the rework.

**1. A seat learns to obey before a director learns to command.** Two policies that both start random co-adapt
badly: the seats see noise and learn to ignore the channel, which is then hard to unlearn. The curriculum
already solves this once -- stage 6 fights a *scripted* player before stage 7 fights a learned one -- and the
same trick applies. A scripted director gives consistent, legible orders (focus the lowest enemy, interrupt in
seat order, rally on the carrier), the seats learn that following them pays, and only then does a learned
director take the seat.

**2. One channel at a time.** Posture, focus, rally and duty are four heads; introducing them together gives the
credit assignment four ways to be wrong at once. Focus first -- it is the simplest and the biggest win -- then
duty, then rally, then posture.

**3. Team size climbs 2, 3, 5, 10, 40.** Each step roughly doubles, and each is a real format rather than a
contrivance: 2 v 2 and 3 v 3 arena, a five-man group, a battleground side, a raid.

**4. Orders live in their own block.** A new `Order` block, carried only by directed stages, holds the four
fields a seat reads. Undirected stages keep the layouts they have, so adding the director does not change the
action or observation shape of the whole curriculum at once -- and the seeding cost of a changed block, which
stage 18 has already paid once, falls only where the director is actually used.

## The stages

The existing trunk to stage 12 is untouched: it teaches a seat to play, and a director has nothing to say to one
seat. The directed ladder branches from the stages that already teach fighting.

| stage | from | team | director | channels | what is new |
| --- | --- | --- | --- | --- | --- |
| `stage19_duo_led` | `stage7_arena` | 2 v 2 | scripted | focus | a seat learns the called target is the right one |
| `stage20_duo` | `stage19_duo_led` | 2 v 2 | **learned** | focus | the director learns to call it |
| `stage21_trio` | `stage20_duo` | 3 v 3 | learned | focus, duty | a healer to kill and a chain to hold it down |
| `stage22_group` | `stage5_party` | 5 | learned | + rally | a group against pulls: spread, stack, peel |
| `stage23_warsong` | `stage18_warsong` | 10 v 10 | learned | + posture | the objective, with a side to split |
| `stage24_raid` | `stage14_raid_gauntlet` | 40 | learned | all four | eight groups, kill order and rotations |

`stage18_warsong` stays as it is -- undirected ten a side -- so the director's contribution is measurable against
it rather than assumed. Same for `stage13`/`stage14` against `stage24`.

## What the arena stages need first

2 v 2 and 3 v 3 do not exist yet. `SeatPlan::Teams` already supports them: `TEAM_SEATS` becomes a property of
the arena rather than a constant, and `Opposition::MirrorSeat` with two or three a side is the whole of it. This
is the smallest piece of new content in the plan and the one the director depends on most, because arena is
where a called target and a held chain decide the fight.

## What to measure, per stage

A director is worth having only where it beats its own absence, so each directed stage is scored against the
undirected one it came from:

- **focus**: the share of a side's damage landing on the called target, and time-to-kill against the undirected
  stage.
- **duty**: interrupts landed as a share of interruptible casts seen, and overlapping crowd control -- the thing
  a rotation is supposed to stop.
- **rally**: seats inside the called shape when a hazard lands; `hazard_seconds` against the undirected stage.
- **posture**: win rate, which is the only honest test of a macro call.

## Cost

Two agents an env in directed stages only. Six new or reworked stages, of which two (`stage19`, `stage20`) are
small arena content and two (`stage23`, `stage24`) are directed variants of stages that already exist.
