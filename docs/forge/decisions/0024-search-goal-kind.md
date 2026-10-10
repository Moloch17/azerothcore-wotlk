# 0024: Looking for an object is its own goal kind, search

**Date:** 2026-10-10

**Decision.** The goal space has a tenth kind, `search` (kind index 9, appended after `resurrect`): look for an object in or
at a place, where a place is a room slot, the way on, or a cell of the seat's own map (place targets 14-21). It is distinct
from `fight` (kill an enemy; an enemy is the target) and from `travel_to` (go to a place for its own sake). The goal space
is 10 kinds x 23 targets = 230 joint ids (a joint is `kind * 23 + target`); the cell goal is `search / target 21` = joint
228 (it was `travel_to / 21` = 159). In M2 (the seek stage) the room and cell goals are offered as `search`
(`Seek.SearchGoals` 1, the default) and no longer as `travel_to`; the `Fight about none` placeholder is offered only while no
place exists, as before. Every other stage offers `search` never: its goal block carries the kind column unused, and its
behaviour is unchanged. Reached, lost, the hindsight label and the room-goal counters and aids follow the place kinds
(`GoalBlock::IsPlaceKind`): the same rules, branch for branch, as the `travel_to` goals had; no reward term or price changed.

**Reason.** The owner, 2026-10-10: the goal head must distinguish search from fight. Until now the M2 head chose
`travel_to` for "look in that room" and `Fight about none` for "no plan yet": a search and a stroll were one choice, and the
placeholder was a fight in name.

**What changed.**

- Goal block revision 5 (width 124, was 122): the kind one-hots and the achieved-kind one-hots grow by one, so the columns
  after the kinds moved; they keep their names, so seeding carries them (cpp-blocks.md). Protocol 29 (`GoalCount` 230),
  layout manifest format 10.
- Appending keeps the ids and checkpoint rows of the first nine kinds in place. A checkpoint of revision 4 seeds the new
  space (`bootstrap._grow_goal_kinds`): the goal head and goal embeddings are padded to ten kinds and the `search` rows are
  warm-copied from `travel_to` (kind embedding and scale, kind logit, pair row, drawn embeddings, the lookahead's; actor and
  critic), so the seeded head chooses `search` about a place with the logit it gave `travel_to`. The head, the cell head and
  the slow loop are kept (py-learner-seeding.md). A checkpoint with more kinds than the stage is refused.
- The cell goal's joint is published as `goals.cells.joint` and follows `Seek.SearchGoals` (228, or 159 at 0); the learner
  reads it from there. `mappo.goal_count` is 10 in the two root configs.
- The pointer over place slots keeps adding its score to the place targets of every kind; the kinds that cannot take a place
  are masked, which leaves it on `search` and `travel_to`.

**What it constrains.** A new goal kind is appended, never inserted or reordered: ids, checkpoint rows and the partners'
(frozen, 9-kind) wire ids depend on it. A kind that a stage does not offer still has its column. `Seek.SearchGoals` 0 restores
the `travel_to` offers.

See also [0005](0005-goal-places-seen-only.md), [0019](0019-vision-only-movement.md),
[principles.md](../principles.md) and the [index](README.md). (The free-choice-goals plan named a decision 0024 for the cell
goals; it was never written, and the number is this one.)
