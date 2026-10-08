# 4. The curriculum: the stages manual

This chapter is the manual for the twelve live stages: what order they run in, what a stage is made of, how a stage
ends,
how to read one while it trains, and what to touch to change one. The per-stage detail (places, rewards, ladders,
evaluations, targets) is in [reference/stages.md](reference/stages.md); the vocabulary is in
[reference/glossary.md](reference/glossary.md); the reasons behind the rules are in [principles.md](principles.md) and
[decisions/README.md](decisions/README.md). The first curriculum (`stage1_move` to `stage21_ship`) and the first
movement
curriculum (`move1_controls` to `move7_follow` of 2026-10-05) were deleted on 2026-10-07; their code and configs are in
the git tags `curriculum-v1`, `curriculum-movement-v1` and `pre-cleanup-2026-10-07` and nowhere else. Nothing in this
chapter describes them.

## 4.1 The line of stages

Twelve stages, trained in this order, each seeded from the stage named in [reference/stages.md](reference/stages.md)'s
seed
chain. All are in the default queue, so `forge start` with no stage name walks them in order.

| Stage | Budget | Eval every | Episodes |
|---|---|---|---|
| `move1_controls` | 150M | 5M | 512 |
| `move2_seek` | 250M | 10M | 78 |
| `move3_interact` | 200M | 10M | 64 |
| `move4_follow` | 250M | 10M | 64 |
| `combat1_fight` | 300M | 10M | 240 |
| `combat2_packs` | 300M | 10M | 240 |
| `combat3_survive` | 300M | 10M | 240 |
| `group1_roles` | 400M | 10M | 384 |
| `group2_corridor` | 500M | 10M | 128 |
| `dungeon1_pulls` | 300M | 10M | 192 |
| `dungeon2_ragefire` | 1500M | 20M | 64 |
| `dungeon3_deadmines` | 2000M | 20M | 64 |

The table is read by `apps/forge/python/tests/test_manual.py` and checked against each `configs/<stage>.yaml`
(`total_env_steps`, `eval.every_env_steps`, `eval.episodes`). Budgets are env steps (decisions x envs x seats). A budget
is a
ceiling, never a target (4.4).

Only `move1_controls` and `move2_seek` have ever trained. The other ten are built, configured and tested but have never
produced a checkpoint; treat their numbers (targets, gates, hyperparameters) as untested designs.

```
movement   move1_controls   the controls, walk and stop beside an object (emptied Stockades)
           └─ move2_seek    find a hidden object by sight, no compass
              ├─ move3_interact   named object among decoys, doors and levers, the cannon (emptied Deadmines)
              │   └─ combat1_fight   one creature at a time (cleared Ragefire Chasm)
              │       └─ combat2_packs   packs of 2-4
              │           └─ combat3_survive   packs that can kill; food, drink, rest
              └─ move4_follow   a party of five keeps with a scripted leader
party      group1_roles (from combat3_survive, merging move4_follow's party frames)   one role drilled an episode
           └─ group2_corridor   four of a wing's real packs in route order
dungeons      └─ dungeon1_pulls   one Ragefire pack a run
                 └─ dungeon2_ragefire   Ragefire, door to Bazzalan (Wailing Caverns held out)
                    └─ dungeon3_deadmines   the Deadmines, door to VanCleef (the bar)
```

It is a line, not a tree, for one reason: a branch is cheaper to train but ends in several checkpoints, and what a leaf
teaches is lost unless the stage exported from is downstream of it. `move4_follow` is the one side branch, rejoined by a
merge in `group1_roles`. A drill is an arena of a stage, not a stage of its own (a separate drill learned nothing over
its
seed).

**Two chains.** `.Extends` and `.Merges` in `Stages.cpp` are the **seed chain**: which stage's `latest.pt` a run starts
from (the sim writes it to `stage.json`, the learner reads only that). `extends:` at the top of a learner yaml is the
**config chain**: which file's settings it inherits. They differ (for example `combat1_fight.yaml` extends nothing) and
neither follows the other. If you change one, decide what the other does.

## 4.2 What a stage is made of

A stage is one `StageDefinition` (`src/server/game/Animus/Scenario/Curriculum/Stages/Stages.cpp`, type in
`StageDefinition.h`) and one learner config.

| Part | Where | What it decides |
|---|---|---|
| Name, suffix | `StageDefinition.Name`, `.Suffix` | Scenario name; model file suffix (`warrior_seek`) |
| Seed | `.Extends`, `.Merges` | Which checkpoints seed the networks, by block and name |
| Blocks | `.Blocks` | The layout: the observation and action vector, in order. `Entities` is inserted after `Vision` automatically |
| Arenas | `.Arenas` (1 to `MAX_ARENAS` = 16) | The situations an episode can be, drawn by weight (`Weight`, or `Arena.<stage>.<arena>.Weight` in the conf) |
| Ground | `.MapId`, `.SpawnPoints`, `.MinLevel`, `.Level`, `.FocusLevelFirst/Last/Chance` | Map, start points and levels |
| Goal places | `.GoalPlaces` | Seen-only by default ([decision 0005](decisions/0005-goal-places-seen-only.md)) |
| Rewards, draws | `CurriculumTuning` (`AnimusForge.Curriculum.*`) | Every weight and distance |
| Learner | `apps/forge/python/configs/<stage>.yaml` | MAPPO values, evaluation, ladders, convergence, status |

An arena is an `ArenaDefinition`: its `Opposition` (Sight, Seek, Interact, PartyFollow, Combat, Roles, Instance) picks
the
encounter that runs the episode; the other fields (seats, party, level band, corridor, drill, respawn, stand-in share,
`EvalOnly`) are listed with comments in `StageDefinition.h`.

**Validation.** `CurriculumStages()` checks each definition in order and leaves out, with a logged error, any stage that
has
a problem; `CurriculumProblems()` lists them and the forge refuses to start training while it is not empty. A stage must
start
with `core` then `move`, list no block twice, give the mental map a camera, the sight block the entities block before
it, the
combat block the sight block before it, and a party combat stage the party frames; extend and merge only earlier valid
stages;
carry the duel block if it fights; and have 1 to 16 uniquely named arenas, not all held out. `ArenaProblem` checks each
arena
against its opposition (a seek arena has rooms and objects and no compass; an instance arena needs the pack block and a
party;
and so on). `test_stage_validation.py` runs the sim's own validation from Python.

**A spawn point is drawn per episode, not per env,** so it is reproducible from an evaluation seed; a reset that cannot
find
an objective redraws up to four points.

**What a stage pays.** Every term is Outcome, Cost or Shaping (`RewardLedger.h`). The stage's purpose is an Outcome (or
Cost)
term; shaping is an aid that the fade removes; the noise prices (Repeat, Jitter, Aimless, Effort, Fidget, Stuck, Wall)
are
Costs that the cost ladder may scale. The score the learner follows (`score_outcome`) is Outcome plus Cost at full
price.
`test_stage_purpose.py` fails if a stage does not name its purpose as one it pays. Term lists per stage:
[reference/stages.md](reference/stages.md).

## 4.3 How an episode runs

`StageScenario` (`StageScenario.cpp`, [reference/cpp-stagescenario.md](reference/cpp-stagescenario.md)) runs any stage.
At
each reset it draws an arena by weight (an evaluation seed draws the same one), clears the episode's totals, picks the
seats'
classes and builds, picks the level, builds each seat's character on its slot, builds the encounters (a party group
first,
then the arena's own), and stocks the seats. At each decision (250 ms, five 50 ms world ticks) it updates the
encounters,
builds each seat's `SeatView`, runs the `SeatEncoder` (every block writes its slice; action 0 is always allowed; a dead
bot sees only what a dead player does), applies the actions through the player controller, then pays rewards through the
ledger. The episode ends when an encounter says so or the arena's clock runs out; no live stage ends on a death.
Details: [reference/cpp-stagescenario.md](reference/cpp-stagescenario.md),
[reference/cpp-encounters.md](reference/cpp-encounters.md), [reference/cpp-blocks.md](reference/cpp-blocks.md),
[reference/cpp-layout-character.md](reference/cpp-layout-character.md).

Characters: every class and race, a random spec and level inside the stage's band (`FocusLevelFirst/Last` and `Level`;
the movement stages are level 1 and a death knight is raised to its own 55); talents, gear and consumables built per
episode;
`Characters.ReuseEpisodes` 4 keeps a character across episodes of the same class and build in training (an evaluation
always
builds). Death knights are not fielded in the level-band dungeon stages
([decision 0004](decisions/0004-death-knights-excluded-from-level-band-dungeons.md)).

## 4.4 How a stage ends and moves on

* **Convergence ends a stage.** A class has converged when, over the last `convergence.window` evaluations, its score
  (the stage's `convergence.measure` if set) has plateaued, its learning-rate-normalised KL stayed under `kl`, its
  entropy
  settled, its ladder rung settled and (for a ladder stage) it has been at the top rung. The stage advances when every
  class
  has converged. A converged class leaves the training draw (it keeps `hold_share` 0.02) and re-enters if its score
  falls.
  (`animus/stage.py`, `ConvergenceController`; [reference/py-learner.md](reference/py-learner.md).)
* **The budget is a ceiling.** A stage that reaches `total_env_steps` first advances anyway, with a report naming the
  classes not done and the signal each lacked. There are no pass gates, no metric floors and no baseline comparisons.
* **Gate-stepped ladders** step forward on their gate metric at one evaluation (`require_plateau: false`), never back on
  the score, warn on collapse and on a stall, re-baseline convergence at each step and keep `best_rung<k>.pt`
  ([decisions 0010 and 0011](decisions/README.md)). Such a stage can converge only at its top rung.
* **Seeding** uses the parent's `latest.pt` (`seed_from: latest`); a cancelled run resumes where it left off.

A `forge fast` run overlays `configs/fast.yaml` on every stage: 10M-step fallback budget (the sim's `--set` wins),
evaluations
every 1M of 64 episodes, `convergence.patience 0` so each stage trains its whole budget.

## 4.5 Reading a stage while it trains

`forge status` / `forgectl status` show the stage's own **headline** measures (`status.headline` in its yaml) with the
last
evaluation's mean and the training mean, judged against `status.targets` (a readout, never a gate), the ladder rung, the
stall and collapse warnings, the stand-in split (`clear_allbot`, `clear_standin`, `standin_gap`) for party stages, and
the
classes a stage never fields by design (`status.excluded`). A run directory `runs/<stage>/` holds `progress.json`,
`metrics.csv`, `eval.jsonl`, `eval.csv`, `latest.pt`, `best.pt`, `best_rung<k>.pt`, `stage.json`, camera images and
`videos/<env steps>/` (eight animated PNGs an evaluation, always from the same seeds). Files and columns:
[reference/file-formats.md](reference/file-formats.md), [reference/metrics.md](reference/metrics.md).

## 4.6 Changing a stage

* **A reward weight or draw parameter:** an `AnimusForge.Curriculum.*` key in the conf. The curriculum keys are part of
  the
  cluster fingerprint and must be identical on every machine (`forgectl conf-sync`). Defaults and meaning:
  [reference/cpp-tuning-keys.md](reference/cpp-tuning-keys.md), [reference/config-keys.md](reference/config-keys.md).
* **A learner value** (learning rate, gamma, evaluation size, gates, targets): the stage's yaml.
  [reference/config-yaml.md](reference/config-yaml.md). A key that does not exist is an error at load.
* **A stage's layout, rewards or arenas:** `Stages.cpp` and the encounter. A layout change changes every checkpoint of
  the
  stage: `LiveLayoutPinTest` fails on purpose and its golden must not be edited to make it pass.
* **Adding a stage** (what the tests insist on): a `StageDefinition` in `Stages.cpp` after its base; a
  `configs/<stage>.yaml` named after it; a row in the budget table above; `AnimusForge.Stage.<name>.TicksPerDecision`
  in the conf template for a movement stage; every `AnimusForge.Curriculum.*` key it reads in `worldserver.conf.dist`;
  every metric its yaml names must be one the sim or learner produces (`test_metric_names.py`); the stage name wherever
  written must exist (`test_stage_names.py`); the layout pin updated deliberately for a new stage. After any C++ change
  a
  cluster rebuild is needed: build the whole plan first ([decision
  0014](decisions/0014-one-cluster-rebuild-per-plan.md)),
  then follow [deploy-gate.md](deploy-gate.md).

## 4.7 Rules that every stage keeps

No scripted teachers or baselines; the only sim policy is `random`. A bot perceives what a player perceives and moves
only
through the player controller. Nothing is masked but the physically impossible. No looting, no corpse run. Ladders are
curriculum, not gates. See [principles.md](principles.md).

## 4.8 Where the old chapter's content went

The previous version of this chapter (about the construction of `StageScenario`, characters, durative actions, the
action
catalog, blocks, encounters, rewards, tuning, episode info columns and the critic state) mixed live and deleted
material.
What remains true now lives in: [reference/cpp-stagescenario.md](reference/cpp-stagescenario.md),
[reference/cpp-layout-character.md](reference/cpp-layout-character.md),
[reference/cpp-blocks.md](reference/cpp-blocks.md),
[reference/cpp-encounters.md](reference/cpp-encounters.md),
[reference/cpp-rewards-routing.md](reference/cpp-rewards-routing.md),
[reference/cpp-tuning-keys.md](reference/cpp-tuning-keys.md), [reference/metrics.md](reference/metrics.md) and
[reference/00-architecture.md](reference/00-architecture.md). Notably stale in the old text: the gauntlet-pull arena,
the pvp and
duel blocks' opponents, "a seat view" fields for owners and companions, and the narrative of the first curriculum's
stages. They
are in git history (`git show bd32b9dc8:docs/forge/04-curriculum.md`).
