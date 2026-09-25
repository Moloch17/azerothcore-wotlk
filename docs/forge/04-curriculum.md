# 4. The curriculum

The curriculum is the set of scenarios the policies train on. It lives in animus-lib under
`src/Scenario/Curriculum/`. Twenty-five stages are defined, numbered in the order they are trained;
**twenty-seven are the default queue**, and the raid stages (28-32) are trained only by name, because forty
seats an env does not run at the usual env count. No stage has a pass gate: each ends when its convergence signals
say so (see "Budgets" below), and the queue moves on.

Every stage trains the same ten class policies over a shared trunk, so what one class learns about moving,
threat or interrupts helps the others. A class policy plays every role its class has specs for, and is measured
per (class, role) -- eighteen such pairs -- so joining the models did not join the bookkeeping.

A stage names the one it `Extends`, and its networks are seeded
from that stage's best checkpoint block by block: blocks it keeps carry over, blocks it drops are left behind,
blocks it adds start from nothing. The chain is one line, and the numbers are the training order, so
`forge start` with no arguments walks the whole thing from stage 1 to stage 27 and never reaches a stage before
the stage it seeds from. A stage none of the run's classes can play (the stealth drill in a run without rogues or
druids) is skipped, and the stage after it seeds from the one before.

```
stage1_move                 open ground, broken ground, water   ── the feet
├─ stage2_indoor            inns: walls within reach, doorways, a jump
├─ stage3_jump              ledges: drop off, or take the long way round; Slow Fall and Levitate open
├─ stage4_dive              lakebeds, and chains of them longer than a breath; the breathing spells open
└─ stage5_dodge             fire underfoot, nothing to fight
   └─ stage6_travel         the mount
      └─ stage7_flight
         └─ stage8_duel                 ── and only now, something that fights back
            └─ stage9_pack
               └─ stage10_gauntlet
                  └─ stage11_endurance
                     ├─ stage20_quest         ── life: a quest, giver to turn-in (+ merges stage6_travel)
                     │  └─ stage21_gather     ── the band's herbs and ore, and what lives among them
                     │     └─ stage22_town    ── sell, repair, restock, dress
                     └─ stage12_pvp           ── against people: self-play, the far side learned
                        └─ stage13_evade      ── a scripted hunter it cannot beat
                           └─ stage14_hide
                              └─ stage15_stealth   (restricted: only classes that can)
                                 └─ stage16_companion   ── beside others (+ merges stage11_endurance)
                                    └─ stage17_party
                                       └─ stage18_tanking
                                          └─ stage19_triage   ── the class curriculum's leaf
                                             ├─ stage23_dungeon         ── real instances: a party against dungeon bosses
                                             │  └─ stage30_raid10       ── by name: Karazhan, Naxxramas (ten seats)
                                             │     └─ stage31_raid25    ── by name: Naxxramas (twenty-five)
                                             │        └─ stage32_raid40 ── by name: Molten Core, Blackwing Lair, AQ40
                                             ├─ stage24_flag  (+ merges stage6_travel, stage12_pvp)
                                             │  └─ stage25_warsong  (+ merges stage19_triage)
                                             │     └─ stage26_duo_led   (a director; + merges stage11_endurance)
                                             ├─ stage27_crossroads      (+ 11 merges incl. the dungeon and the life stages; the stage that ships)
                                             └─ stage28_raid_single     ── by name (the synthetic raid, a control)
                                                └─ stage29_raid_gauntlet
```

**The first seven stages have nothing to kill in them, and that is the point.** A seat steers itself -- eight
egocentric bearings under a held yaw and pitch, with the ground read along each of them -- and where a seat puts
its feet is not something only some stages are about. Putting movement first means everything after it inherits
legs that already work, instead of learning to fight and to walk at the same time and doing both badly.

**It is a line rather than a tree** for one reason: a branch is cheaper to train but ends in several checkpoints,
and everything a leaf teaches is discarded unless the stage exported from is downstream of it. That is how the
drills, the raids and the team stages became a dead end under the old tree. A line ends in one leaf carrying the
lot, which is also what the per-class join needs to take from each class.

`stage27_crossroads` extends `stage19_triage` and merges `stage26_duo_led`, `stage25_warsong`, `stage12_pvp`,
`stage7_flight`, `stage16_companion`, `stage10_gauntlet` and `stage8_duel`: it is where every line becomes one
policy.

> **One stage is restricted, and it used to have to be a leaf.** `stage15_stealth` is played by the classes whose
> own kit carries a stealth aura -- rogue and druid -- because closing on someone unseen is a thing only a real
> stealth aura can do. In a run of all ten classes its checkpoint holds two of the ten layouts, and
> `init_from: auto` takes the **first checkpoint in the chain that exists** -- so a stage seeding from it would
> find that one, stop looking, and start the other eight from random weights without saying so.
>
> `Problem()` used to prevent that by refusing to let anything extend or merge a `NeedsStealth` stage at all,
> which was too blunt: **in a run of one class that stealths, every layout plays the stage and the checkpoint is
> not partial.** That is exactly the druid case, and it is where Prowl has to reach the arena and the flag. The
> check now lives in `animus.bootstrap`, where the run's actual layouts are known: a layout the checkpoint lacks
> is refused, loudly, with the classes it was trained on named. So the chain runs through the stage, and a run
> whose classes cannot all play it is stopped with an error rather than seeded in silence.

## The stages

Seats are the learned agents an episode runs: `Solo` is one, `Party` up to five, `Raid` eight groups of five,
`Mirror` two seats fighting each other, `Teams` two sides of `TeamSeats`. A directed arena adds two more agents,
one commanding each side (see 4.12).

| Stage | Extends | Seats | Blocks added | What it is |
|---|---|---|---|---|
| `stage1_move` | — | Solo | core, move, travel, duel | **The root, and nothing to fight.** A place 40-160 yd away on foot -- mounting is masked, so the trip is made with the speed cooldowns the class has. Three arenas: open ground, genuinely broken ground (ridges, canyon and shore, chosen by measured local relief), and water whose way round is longer than the way through. The generator refuses to place an objective the character cannot reach in the time it has, so `arrived` is the column to read |
| `stage2_indoor` | stage1_move | Solo | same | **Inside.** A place 8-40 yd away in an inn -- shorter than an outdoor episode's first step. Where the sixteen navmesh rays, the 15-degree turn, the clearance term and the jump are all worth something. |
| `stage3_jump` | stage1_move | Solo | same | **Down.** A place 20-120 yd away below a ledge, 5-80 yd under the seat, with a way round on foot at least twice the straight line. The jump drops off the edge and the fall after it is the core's own, with the core's own damage: free to fourteen yards, lethal past about seventy. Slow Fall and Levitate are open: a mage or priest learns when a cast is worth it to make the deadly drop free, and every other class learns the bare price of a drop |
| `stage4_dive` | stage1_move | Solo | same | **Down, into the water.** A place 20-120 yd away on the bed of Stonebull Lake under 6-40 yd of water; arriving means standing on it. The breath is the core's three minutes and the drowning after it a fifth of the seat's health a second. A third of the episodes (`chain`) are a chain of lakebeds thirty to sixty yards apart for four minutes, longer than a breath: come up between legs, or make the breath free -- Unending Breath and Water Breathing are open, so the two classes that have one learn when the cast is worth it |
| `stage5_dodge` | stage1_move | Solo | same | **Drill.** Still nothing to fight: fire lands underfoot every few seconds and stays, so getting off it is the only thing in the episode |
| `stage6_travel` | stage5_dodge | Solo | same | A place 60-320 yd away by path: mount when it pays, get there, arrive on foot. Level 20+ |
| `stage7_flight` | stage6_travel | Solo | same | A place 350-700 yd away in Nagrand: take off, fly over what is in the way, land, dismount. Level 60+. A third of its episodes (`flight_air`) put the place on a plateau or island the ground route does not reach, with the ground mount masked, where the spawn point has one in reach; `air_only` reports which trips did |
| `stage8_duel` | stage7_flight | Solo | + pet | **Where the fighting starts.** A same-level creature out of aggro range: close in and kill it fast, taking little damage. It arrives already knowing how to place its feet |
| `stage9_pack` | stage8_duel | Solo | + pack (−travel) | A pack of 2-4, casters included, usually linked: targets, interrupts, crowd control |
| `stage10_gauntlet` | stage9_pack | Solo | + gauntlet, support | Pull after pull with short breaks: heals, food and drink |
| `stage11_endurance` | stage10_gauntlet | Solo | same | **Drill.** A known run of eight pulls, won by finishing it: 900 s, ending on an elite pack two levels up |
| `stage12_pvp` | stage11_endurance | Mirror | + pvp (−pack, −gauntlet) | **Against people.** Self-play one-on-one: two learned seats of any classes. The far side is the live policy or a frozen earlier checkpoint from the learner's cast league, never a script; the scripted `fight` player is only the evaluation yardstick |
| `stage13_evade` | stage12_pvp | Solo | same | **Drill.** A scripted enemy player ten levels up for 120 s: the fight cannot be won, so the score is being alive at the end. Break away, break line of sight, use the class's escape |
| `stage14_hide` | stage13_evade | Solo | same | **Drill.** The same fight six levels up, for every class and race: get out of sight and stay there, and hide again after being found. Terrain, distance, Blink, Disengage, Feign Death, Invisibility, Vanish, Prowl, Shadowmeld -- whatever the kit and the race give it |
| `stage15_stealth` | stage14_hide | Solo | same | **Drill, restricted.** For the classes whose own kit carries a stealth aura (rogue and druid): close on a stronger enemy unseen, hold inside strike range, and open from it. Shadowmeld does not qualify -- it breaks on movement. In a run whose classes cannot play it the queue skips it; in an all-class run its checkpoint holds two layouts and the next stage seeds from the stage before it |
| `stage16_companion` | stage15_stealth (+ stage11_endurance) | Solo | + pack, gauntlet, companion, support (−pvp) | The gauntlet beside an owner: follow, assist, guard and heal it. The owner is a seat of its own played by the endurance policy (the learner's cast) in 70% of training episodes, and the script's wandering owner in the rest and in every evaluation |
| `stage17_party` | stage16_companion | Party | + party | Four learned seats and the owner (cast as in the companion stage) against elite-heavy pulls |
| `stage18_tanking` | stage17_party | Party | same | **Drill.** Seat 0 is drawn from builds that can hold the pull: hold what it brings, and keep it off the others |
| `stage19_triage` | stage18_tanking | Party | same | **Drill, and the leaf of the class curriculum.** Seat 0 is drawn from builds that can keep the hurt one up, and has to spend mana doing it |
| `stage20_quest` | stage11_endurance (+ stage6_travel) | Solo | + travel, world | **Life begins.** A quest of the level band (15-20, 35-40, 58-60; the rung is the band) in the world's own zone: the giver, the creatures around the objectives and the turn-in copied into the env's phase. Take it, do it, hand it in |
| `stage21_gather` | stage20_quest | Solo | same | A field of the band's herb and ore nodes with the zone's creatures among them, the professions at the band's skill: find, open, take, skin, do not die |
| `stage22_town` | stage21_gather | Solo | same | A town of the seat's side around its inn: sell the junk, repair, restock food and drink, put the better item on |
| `stage24_flag` | stage19_triage (+ stage6_travel, stage12_pvp) | Mirror | + pvp, travel, flag | Capture the flag one-on-one: bases 100-180 yd apart, first to three captures. Level 20+ |
| `stage25_warsong` | stage24_flag (+ stage19_triage) | Teams (10) | + party | Ten against ten for the flag on a real Warsong Gulch instance: escort the carrier, hold the base, stop theirs |
| `stage26_duo_led` | stage25_warsong (+ stage11_endurance) | Teams (2) | + context, hostiles, order | Two against two under a **director**: told who to kill, whose turn it is, and where to go (4.12) |
| `stage27_crossroads` | stage19_triage (+ 10 merges) | Mirror/Party/Solo | + pvp, context, hostiles, travel, world | PvE, PvP and life in one policy: every earlier situation, an ambush mid-gauntlet, a ganked owner, a quest, a field, a town |
| `stage28_raid_single` | stage19_triage | Raid | same as triage | **By name.** A raid of eight groups against one elite and its adds, won or lost as the single pack is |
| `stage29_raid_gauntlet` | stage28_raid_single | Raid | same | A raid clearing pull after pull, recovering between them |

The first seven stages have nothing in them to kill, and that is the point. A seat steers itself now, and where it
puts its feet is not something only some stages are about -- so everything after them inherits legs that already
work, rather than learning to fight and to walk at the same time and doing both badly.

The chain is one line rather than a tree. A branch is cheaper to train and ends in several checkpoints, and
everything a leaf teaches is discarded unless the stage exported from is downstream of it -- which is how the
drills, the raids and the team stages became a dead end. A line ends in one leaf that carries the lot.

### Trained by name

The two raid stages, `stage28_raid_single` and `stage29_raid_gauntlet`: forty seats an env is forty bots an env,
so `AnimusForge.Envs` has to come down roughly in proportion (a few dozen envs, not 128) before either is started
with `forge start stage28_raid_single`. Nothing seeds from them.

### Two chains called "extends"

The word means two different things, they are read by different programs, and for six stages they deliberately
disagree. Getting them confused is easy and the consequences are invisible, so:

- **The seed chain** -- which stage's *checkpoint* a run starts from -- is `.Extends` and `.Merges` in
  `Stages.cpp`. The sim writes it into `stage.json` as `seed_chain`/`merges`, and the learner reads only that
  (`animus.stages.seed_chain`, `TrainConfig.resolved_init_from` with `init_from: auto`). **This is the column in
  the table above.**
- **The config chain** -- which YAML file's *settings* are inherited -- is `extends:` at the top of
  `python/configs/<stage>.yaml`. It never decides what a run seeds from.

They diverge wherever a stage should inherit a drill's weights without inheriting its hyperparameters, which is
the whole point of putting drills on the trunk:

| Stage | Seeds from (`Stages.cpp`) | Inherits config from (YAML `extends:`) |
|---|---|---|
| `stage12_pvp` | stage11_endurance | stage8_duel |
| `stage16_companion` | stage15_stealth (+ stage11_endurance) | stage10_gauntlet |
| `stage19_triage` | stage18_tanking | stage17_party |
| `stage24_flag` | stage19_triage (+ stage6_travel, stage12_pvp) | stage12_pvp |
| `stage26_duo_led` | stage25_warsong (+ stage11_endurance) | stage12_pvp |
| `stage27_crossroads` | stage19_triage (+ 10 merges) | stage17_party |

A drill sets its own `patience` and `min_env_steps` for being a drill; the stage after it wants the drill's
weights and the trunk's schedule, so it seeds from the one and inherits from the other. If you change one chain,
decide what the other should do rather than assuming it follows.

### Budgets

From `python/configs/*.yaml`; `python/tests/test_manual.py` fails if this table and the configs drift apart. A
budget is a **ceiling, never a target**: a stage ends when every class it plays has converged (the rule at the head
of this chapter, `python/animus/stage.py`), and a stage that reaches its budget first advances anyway, with its
report naming the classes that were not done and the signal each was missing. There are no pass gates. A
`forge fast` run replaces the budgets (20M a stage, evaluations every 1M of 64 episodes, and `patience` 0 so every
stage trains its whole budget).

| Stage | Budget | Eval every | Episodes | Stage | Budget | Eval every | Episodes |
|---|---|---|---|---|---|---|---|
| `stage1_move` | 30M | 2M | 2048 | `stage2_indoor` | 16M | 2M | 2048 |
| `stage3_jump` | 16M | 2M | 2048 | `stage4_dive` | 20M | 2M | 2048 |
| `stage5_dodge` | 30M | 5M | 2048 | `stage6_travel` | 20M | 2M | 2048 |
| `stage7_flight` | 20M | 2M | 2048 | `stage8_duel` | 100M | 10M | 2048 |
| `stage9_pack` | 40M | 10M | 2048 | `stage10_gauntlet` | 60M | 10M | 2048 |
| `stage11_endurance` | 60M | 10M | 1024 | `stage12_pvp` | 60M | 10M | 2048 |
| `stage13_evade` | 30M | 10M | 2048 | `stage14_hide` | 30M | 10M | 2048 |
| `stage15_stealth` | 20M | 10M | 2048 | `stage16_companion` | 60M | 10M | 2048 |
| `stage17_party` | 90M | 15M | 2048 | `stage18_tanking` | 60M | 15M | 2048 |
| `stage19_triage` | 60M | 15M | 2048 | `stage20_quest` | 60M | 10M | 1024 |
| `stage21_gather` | 30M | 10M | 1024 | `stage22_town` | 20M | 5M | 1024 |
| `stage23_dungeon` | 60M | 10M | 512 | `stage24_flag` | 40M | 10M | 2048 |
| `stage25_warsong` | 40M | 10M | 128 | `stage26_duo_led` | 30M | 10M | 512 |
| `stage27_crossroads` | 100M | 25M | 256 | `stage28_raid_single` | 40M | 20M | 256 |
| `stage29_raid_gauntlet` | 40M | 20M | 256 | `stage30_raid10` | 40M | 10M | 128 |
| `stage31_raid25` | 40M | 10M | 64 | `stage32_raid40` | 40M | 10M | 32 |

**What the budgets assume.** 128 envs (`AnimusForge.Envs`; this machine's `forge bench` result, where the shipped
default is 64 -- every number in this chapter is at 128). Stages 1-7 (the movement root) are trained once, for
every class; stages 8-19 are trained per class, each class with all 128 envs; stages 20-27 (the life stages, the
dungeon, the objective stages and the crossroads) once, after the join; the five raid stages by name. So the queue's
ceiling is 1,202M (1,402M with the raids), and a ten-class build's is 152M for the root, 670M per class (6,700M for
ten) and 380M for the life stages, the dungeon and the objective stages: about 7,232M, against the 15,780M the
earlier per-class plan came
to. Two assumptions carry that number. The objective stages "once after the join" assume the **take-one-trunk**
join below (seed from one class's trunk and let the adapters adapt), the only one of the three options that costs
no training. And every class has a `configs/<class>/stage8_duel.yaml` naming the shared flight checkpoint
(`{shared_runs}` in a path is the shared root's run directory beside the class's own); the druid's directory also
carries its own report columns for the stages where it has something to say.

**What an evaluation costs.** `episodes x episode seconds / envs` sim-seconds per evaluation, which the sim runs
faster than real time: the duel's 2048 x 90 s / 128 is 1,440 sim-seconds (about 80 s of wall clock); the party
line's 2048 x 450 s / 128 is 7,200; Warsong's 128 matches x 420 s / 128 is 420. A stage inherits the duel's 2048
episodes unless its config says otherwise, which is how the two raid stages came to 19,200 and 38,400 sim-seconds
an evaluation before they set 256, and the endurance stage's 900 s episodes to 14,400 before it set 1024;
`python/tests/test_manual.py` now fails any stage over the party line's 7,200.
The duel's second, *sampled* evaluation every third time (`eval.sampled_every`) is off everywhere but the export
stage, which is the only one that reads it. Convergence usually ends a stage well short of its ceiling: an earlier
run of the duel had its best at 80M of a 300M budget, and the party stage its best at 100M of 120M.

**A drill** fixes what one episode is about, where the curriculum otherwise teaches the same skill inside a stage
won by something else and the credit for it is smeared over the clear. Drills are *on* the trunk rather than
beside it (`stage6_travel` seeds from `stage5_dodge`, `stage16_companion` merges `stage11_endurance`,
`stage19_triage` seeds from `stage18_tanking`), so a drill is never a dead end whose lesson nothing inherits.

Two things to know before reading a drill's scores. The hazard charge lands about four times harder on a tank
than on a ranged seat, because a tank cannot walk out of what it is holding an enemy in. And a forced-healer
stage will find any fault in the resurrection path faster than anything else in the curriculum -- it found the
farmable revive described in 4.6.

## Training one class at a time

A run trains the classes of `AnimusForge.Classes`; empty is all ten. Training them together shares one trunk
between every layout, which is a bet that classes have something learnable in common. Training them apart gives
each class a curriculum of its own -- and a trunk of its own, which is the part that has to be thought about.

### The shared root, and where a class branches off it

**Stages 1-4 are trained once, for all ten classes. Every class branches at `stage8_duel`.**

The movement stages are class-agnostic. There is no druid-specific way to cross a field: everyone walks with the
same eight bearings, the same held turn and pitch, the same reading of the ground ahead, and the same question
about whether the water is worth getting into. What a class brings to it is a speed cooldown or two, and those are
actions in a catalog the layout already has.

The fighting stages are the opposite. A druid has 91 actions and four builds across three jobs; a mage has 84 and
three builds that all do the same thing. That is where a curriculum stops being shared and starts being a class's
own.

So the shape is:

```
shared/runs/     stage1_move  stage5_dodge  stage6_travel  stage7_flight      Classes = ""     (all ten)
                                                                │
druid/runs/                                                     ├─ stage8_duel ... stage19_triage
warrior/runs/                                                   ├─ stage8_duel ... stage19_triage
...                                                             └─ ...                          Classes = "<one>"
```

Each class's directory is its own because from `stage8_duel` on every class trains the same *stage names*; one
directory would have the second class overwrite the first's checkpoints.

**This is cheaper than training the movement stages per class**, not dearer: 152M env steps once rather than ten
times, which is 152M against 1,520M. And every class then starts from the same trunk rather than from ten
independent random initialisations, which is the thing that makes the eventual join tractable.

### What the whole plan costs

The ceilings from the budget table above:

| | env steps |
|---|---|
| Shared movement root, stages 1-7, all ten classes | 152M |
| One class, stages 8-19 | 670M |
| Ten classes | **6,700M** |
| The join, the life stages, the dungeon and the objective stages, 20-27, once | 380M |
| **Total** | **7,232M** |

Against 1,202M for the whole queue trained with every class at once. **A per-class curriculum is roughly seven
times the compute**, and that is the price of the thing it buys: a policy per class that has not had to share its
trunk with nine others through the stages where classes have least in common.

Three things take the edge off it:

- **Budgets are ceilings, not targets.** The convergence rule ends a stage when every class it plays has
  converged, and a converged class leaves the draw before that, so a stage routinely finishes well short of its
  ceiling. An earlier run of the duel had its best at 80M of a 300M budget, which is why the duel's ceiling is 100M
  now: the two stages that used to be 300M each (`stage8_duel`, `stage11_endurance`) were 40% of a class's cost
  and were sized for one run covering eighteen class/builds at ~17M each; a run of one class gives it all 128 envs.
- **Classes run two at a time** (chapter 7, *Training one class at a time*): the sim and the learner share nothing
  between runs but the cores, and the machine has 32.
- **Characters are reused across episodes** (`Characters.ReuseEpisodes`): a seat that draws the class and build it
  already has keeps its character for a few episodes, which takes most of the reset cost (a quarter of a decision)
  out of training; evaluations always build fresh.

### How a class's first combat stage finds the shared checkpoint

`init_from: auto` cannot: the seed chain looks under the run's own `runs` directory, and the shared root is a
sibling of it. So a class's `stage8_duel` config names the checkpoint outright:

```yaml
init_from:
  - /azerothcore/var/animus-forge/shared/runs/stage7_flight/best.pt
```

Seeding across works because **the layout check is one-directional**: every layout the *run* has must be in the
checkpoint, not the other way round (`animus.bootstrap`). A druid-only run takes the druid's adapter and head out
of an all-class checkpoint, takes the trunk, and leaves the other nine layouts behind. The reverse -- a run with a
layout the checkpoint lacks -- is refused loudly, because that would start a class from random weights in the
middle of a curriculum, which looks exactly like a class that has simply not learned anything yet.

### The open question: what the trunk does at the join

Ten per-class runs produce ten trunks, all descended from the shared movement root but drifted apart by their own
fighting stages. A stage that puts the classes back together -- the crossroads, or anything with a director
commanding a mixed team -- can seed a trunk from only one of them.

Nothing *requires* a shared trunk for multi-class play. The env runs N seats each with a layout, and the trainer
already keeps adapters and heads per layout; a shared trunk is a training-efficiency device. The shipped model is
unaffected either way, because `animus.export` already writes adapter + trunk + GRU + head per class (~0.84M
parameters each).

Three ways to land it, and the choice is deliberately deferred until there is a measurement to make it with:

| | what it is | cost |
|---|---|---|
| **Distil the ten** | A join stage seeds each class's adapter and head from its own run and relearns the trunk with ten warm adapters. `seed_merges` and `animus.distill` already do this shape for merge stages | A large joint run. The cost is deferred, not avoided |
| **A trunk per class** | Make `trunk.` per-layout, so a class owns its whole network and the join is arithmetic rather than training | A learner change, and **+5.8M parameters**: the actor goes 2.58M to 8.35M. Smaller than it sounds -- the per-layout adapters and heads are already 75% of it -- but it gives up cross-class transfer entirely |
| **Take one trunk** | Seed the join from whichever class's trunk, and let the adapters adapt | Free, and only sane *because* the ten share an ancestor in the movement root |

### The measurement that decides it

Whether cross-class transfer is worth anything at all is answerable in one stage, not one tree. Train
`stage8_duel` for one class twice:

1. seeded from the shared all-class `stage7_flight`, and
2. seeded from a movement run of that class alone,

and compare `eval.at_start` and the first few evaluations. If (1) starts higher or climbs faster, the shared trunk
is carrying something and distillation is worth its cost. If the two are indistinguishable, a trunk per class is
the simpler answer and the join stops being a problem.

## 4.1 Defining a stage

A stage is one `StageDefinition` entry in `Stages/Stages.cpp`:

```cpp
stages.push_back({
    .Name = "stage16_companion",          // scenario name
    .Suffix = "_companion",              // model names: warrior_tank_companion
    .Extends = "stage10_gauntlet",        // seeds from it (the trunk)
    .Summary = "the gauntlet beside a scripted owner: follow, assist, guard and heal it",
    .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion },   // layout order
    .Arenas = { { .Name = "companion", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                  .Owner = true } },
});
```

An `ArenaDefinition` describes one situation:

| Field | Values |
|---|---|
| `Name` | Unique within the stage. Used in episode info, `stage.json`, tuning keys and per-arena gates |
| `Weight` | Share of episodes, overridable with `<TuningPrefix>Arena.<stage>.<arena>.Weight` |
| `Seats` | `Solo` (1), `Party` (4 slots beside the owner, 1-4 filled each episode), `Mirror` (2 that fight each other), `Raid` (40: eight groups of five, a tank and a healer at the head of each) |
| `Against` | `Creature`, `Pulls`, `ScriptedPlayer`, `MirrorSeat`, `Ambush`, `Travel` (a place to get to), `Flag` (a flag match between mirror seats), `Hazards` (nothing to fight: ground to get off) |
| `Schedule` | `None`, `SinglePack` (ends on clear), `Gauntlet` (pull after pull) |
| `MaxRung` | Pin the pack ladder instead of letting it climb: `-1` leaves it to `Pulls.MaxTier`, `0` and up hold every class and role at that rung, for training and evaluation alike. Overridable with `<TuningPrefix>Arena.<stage>.<arena>.MaxRung`. A drill wants one variable |
| `Owner` | An owner the seats fight for; `OwnerCast` plays it through a row of its own from a frozen checkpoint (the learner's cast), the script keeping `Owner.CastScriptedShare` of training episodes and every evaluation |
| `PartyGroup` | The owner and seats form a core group |
| `Pvp` | Resilience gear, no self-resurrection |
| `EpisodeSeconds` | 0 = the host's `EpisodeSeconds` |
| `Ambushers` | 0-2 scripted enemy players who attack the owner |
| `Flying` | Travel: the objective is far enough that flying beats riding |

A `StageDefinition` may also name its own `MapId`, `SpawnPoints` and `HeldOutSpawnPoints` (0 = the host's
`SpawnMapId` and `SpawnPosition`) and a `MinLevel` that raises every character's level, a fixed host level included.
On a continent (a map that isn't instanceable, like Outland for `stage7_flight`) every env shares the map: each
env's seats live in their own phase (`StageScenario::EnvPhase`), so no env sees another's. An arena may carry
`SpawnPoints` of its own, which it needs when its ground is particular -- the water arena's banks, say -- because
the arena is drawn per episode and the stage's list is not keyed to it.

**A spawn point is drawn per episode, not per env.** It used to be `SpawnPoints[env.Index % size]`, which meant an
env stood on the same patch of ground for its whole life: 128 envs saw eight places between them, every episode,
for a whole run. That is a thing a network can fit instead of learning to read what is in front of it.
`EnvState::Spawn` is now rolled at the reset, so it is stable within an episode, reproducible from an evaluation
seed, and every seat sees all of the stage's ground. A spawn point that no objective can be found from no longer
takes the run down with it either: the reset draws again and moves the seats, up to four points, and only names a
failure when all of them fail.

### The control ground

**`HeldOutSpawnPoints` is where scored episodes stand, and where training never does.** A seeded evaluation
randomises the episode, not the world, so an evaluation on the ground training uses cannot tell a policy that
reads terrain from one that has learned those particular places -- and until this existed, nothing in the
curriculum could answer that question, a passing gate included. The split is keyed on `Env::Evaluating` rather than
on the presence of a seed, because a replayed evaluation is a *training* episode that has one.

| Map | Training ground | Control ground -- scoring only |
|---|---|---|
| Kalimdor (`stage1_move`, `stage6_travel`, `stage8_duel`, `stage24_flag`) | The Barrens, Northern Barrens, Durotar, Mulgore, Dustwallow Marsh (26 points) | Northern highlands, Eastern high ground, Mid-east plains (10 points) |
| Outland (`stage7_flight`) | Hellfire Peninsula, Zangarmarsh, Shadowmoon Valley, Terokkar Forest (8 points) | Eversong Woods, Azuremyst Isle, Bloodmyst Isle (6 points) -- other continents on the same map |
| Map 560 (`stage13_evade`, `stage14_hide`, `stage15_stealth`) | The southern approaches (6 points) | The northern farmland (4 points) |
| `stage1_move`'s water arena | Six banks of the Dustwallow pond | Its two far banks |

Kalimdor's control ground was chosen by measured distance from water -- 1,500 to 5,000 yards from the nearest
water-dwelling creature -- after a first attempt on Teldrassil and the Azshara coast had the seats swimming for 21
seconds an episode in the open arena and 33 in the broken one, against 0.03 on the ground they train on. A control
that is wet where training is dry measures the coastline, not the policy.

Two of the sets are deliberately weaker than the rest and say so: map 560 is one small instance, so its split is by
district rather than by region, and the water arena's control is the far side of the same pond, because of four
bodies of water measured only that one had a detour worth avoiding. `stage25_warsong` has no control ground at all
-- its three points are a battleground's own spawn rooms -- and neither do the combat stages after `stage8_duel`,
which still run at the host's single spawn inside a per-env instance.

**What to read from it.** If `arrived` and `saved` at the gate track the same metrics in training, the seat is
reading terrain. If the gate numbers fall away, it had learned the places.

**Validation.** `CurriculumStages()` checks each definition in order and leaves out (with an error log) any stage
that:

- doesn't start with the `core` block, lists a block twice, or lacks `duel` (every stage fights something that fights
  back),
- extends or merges a stage that isn't an earlier valid stage, merges its base or the same stage twice, or merges
  without extending,
- has no arenas, more than `MAX_ARENAS` (12), or two arenas with the same name,
- has an inconsistent arena:
  - a pull schedule without pulls, or pulls without a schedule
  - pulls without `pack`, or a gauntlet without `gauntlet`
  - an owner without pulls or an ambush, or without `companion`
  - a party group without an owner, party seats and `party`
  - mirror seats without `MirrorSeat` or `Flag`, or either without mirror seats
  - travel without `travel` or other than one seat on its own (no owner, PvP or ambushers), `Flying` without travel,
    or a flag match without `travel` and `flag`
  - more than 2 ambushers, ambushers without an owner and `pack`, or an `Ambush` arena with other than exactly one
    ambusher
  - fighting a player without `pvp`, a one-on-one against a player that isn't `Pvp`, or `Pvp` without a player

The base only has to exist. Seeding maps blocks by name, so a stage may drop base blocks it doesn't need, and several
stages may share a base.

## 4.2 How `StageScenario` runs a stage

`StageScenario` implements `Scenario` for any `StageDefinition`.

### Construction

1. **Tuning.** `CurriculumTuning::Load(settings.TuningPrefix)` reads every value (4.9). `DecisionScale =
   DecisionMs / 50`.
2. **Layouts.** One `Layout` per class in `StageSettings::Classes` (or all 10) whose assets have at least one
   race. `Layout::Index` is its position in this list, which is the index the learner sees.
3. **Scripted-player assets.** If any arena has an owner or a scripted enemy player, every class's assets are
   built now, because those bots can be any class. Building takes seconds per class, and doing it now avoids
   stalling the world thread during an episode reset.
4. **Spec.** `AgentsPerEnv` is the largest arena's seat count. `ObsDim` and `NumActions` are the largest layout's.
   `StateDim` is fixed (4.8).
5. **Encounters**, created only if some arena needs them, in **build order**: opponent, owner, party group, pulls,
   creature, ambush. The owner comes before the party group (which it leads) and the pulls (which spawn around it).
   Ambushers come last because they find the owner and take the slots the pulls leave. **Reward order** (which is also
   the order of episode info columns) is creature, pulls, owner, party, opponent, ambush. For each arena the scenario
   keeps the subset it uses, in both orders, plus its weight and episode length.
6. **Pools.** The creature opponent pool and the consumable pool are loaded now rather than on the first episode.
7. **Episode info columns.** The core columns (4.10), then each encounter's, then one `reward_<term>` column per reward
   term any encounter pays.
8. **Stage files.** If `LayoutsDir` is set, the manifests and `stage.json` are written (see
   [3.9](03-animus-lib.md#39-the-stage-description-stagejson)).

### Building an episode (`Rebuild`)

`Setup` builds each env's first episode and marks the env `Fresh`, so the pool's first `Reset` is skipped. After that,
every `Reset` calls `Rebuild`:

1. **Draw the arena** by weight: no draw for a single arena, otherwise `urand` over the
   weights. It is drawn first, so a seeded evaluation episode draws the same arena. The env's episode length is set
   from the arena.
2. **Clear totals.** Every seat's `ResetEpisode` and every encounter's `ResetEpisode`, including encounters this arena
   doesn't use, so their columns read 0.
3. **Deactivate** encounters the previous arena used and this one doesn't (remove the owner, disband the group, remove
   the enemy player). Then `BeforeRebuild` for this arena's encounters (the party group disbands before its members are
   replaced).
4. **`Begin()`** every seat's `BotSlot`, and remember each seat's current character for rollback.
5. **Pick the seats.** A party arena draws its size from `Party.SizeWeight1-4`; a raid arena takes
   all forty of its seats. Half the time
   (`Party.ClassicChance`) the roles are the classic makeup -- a tank and a healer at the head of every group of
   five, the rest damage -- shuffled over the seats in play. Otherwise each seat's
   role is drawn (`RoleTankChance`, `RoleHealerChance`). Each seat then takes a random layout of its role, or any layout
   if the run has none of that role. Other arenas give every seat any layout. A training episode draws it by the
   learner's per-layout weights (`WEIGHTS`, evenly without them); an evaluation episode doesn't draw at all: seed
   *i* plays candidate *(i + seat) mod count*, so every class is scored on an equal share of the seeds. Seats
   beyond the active count get no layout and no bot.
6. **Pick one level** every seat's class can be (death knights start at 55). It is `StageSettings::Level` if set;
   otherwise `Characters.HighLevelChance` percent of the time a level from `HighLevelFirst` to 80,
   `Characters.LowLevelChance` percent of the time a level from the class minimum to `LowLevelLast` (20; skipped when
   the class can't be that low), else any level from the class minimum to 80. An **evaluation** episode takes its
   band from its seed instead, as it takes its difficulty tier: seed *i* plays band *(i / the pairs) mod 4* of
   1-20, 21-40, 41-60, 61-80 (the next band up when the seat's classes cannot be that low). Training then keeps the
   level mix the shipped companions play while the evaluation measures every band in equal numbers -- drawn, the
   middle bands were ~9% of the episodes each, too thin to read a class's hole from.
7. **Build each seat** (`BuildSeat`): random race and spec, `DamageScale(level)`, a bot named
   `Forge<envId>s<seat><a|b>` on the slot's idle session and account, placed in the env's instance (the first build of
   an env opens a new instance, unless the host placed the env). Party seats start spread around the spawn point, and a
   mirror seat 2 spawns 40-50 yd from seat 1 at a random bearing. Talent points are recomputed on the spawn map (death
   knights are created in Ebon Hold, where only quest-rewarded points count), then `SeatCharacter::Configure` dresses
   the bot (4.3).
8. **On failure**, every slot aborts, each seat's previous character is restored, and `Rebuild` returns false. The
   scenario sets `BuildFailed`, `IsTerminal` ends the episode at the next decision, and the following reset tries again.
9. **Commit.** Old targets despawn, every slot `Promote()`s (the old bots log out), and the first build of an env clears
   the spawn point's own creatures (`SpawnArea::Clear`). `env.Bots`, `MapId` and `InstanceId` are updated and
   `env.Targets` cleared.
10. **Encounters build** in build order (spawn the creature, the owner, the group, the first pull, the enemy player).
11. **Stock the seats**: potions, mana potions, bandages, healthstones (a warlock in the party hands them out), a
    soulstone, and a flask or elixir (4.3).

### Each decision

`ApplyActions`:

1. every active encounter's `UpdateEnemies` (linked packs join fights), then `Update` (the owner acts, the next pull
   spawns, the scripted opponent acts, ambushers arrive),
2. `AcceptResurrections`: dead seats and the owner accept a pending resurrection request as a client would, and
   the seat that cast it is credited once the ally is actually alive (`StepRevivedAlly`, `Revives`) -- see the
   note below,
3. for each seat with a living bot: `CurrentTarget` (the first encounter whose `SelectTarget` answers, else target 0),
   `BeforeSeatAction` on every encounter, build the `SeatView`, `SeatEncoder::Apply`, fold the result into the seat's
   totals, `OnSeatAction` on every encounter, and summon a called hunter beast.

> **Why accepting a resurrection needs bookkeeping.** A client answers an offer once, with one
> `CMSG_RESURRECT_RESPONSE`, so nothing in the core clears the request afterwards -- neither
> `Player::ResurectUsingRequestData` nor `ResurrectPlayer` touches `m_resurrectGUID`. Polling it every decision,
> as this must, therefore has to remember what it has already accepted. Before it did, one landed Rebirth stood
> its target back up free of charge every decision it died for the rest of the episode: 38.4 revives an episode
> against 0.97 owner deaths in stage 4, earning 57.55 of `druid_dps`'s 65.08 return. Every stage seeded from
> those learned that a druid scores by standing near a corpse.
>
> The fix is the module's and not the core's: remember which offer was accepted, wait for it (a delayed teleport
> reschedules the resurrect, so it does not always land on the decision it was taken), pay once the ally is
> alive, and clear the request then. A retry window gives up on one that never lands rather than barring the
> seat for the episode.

`Observe`: each seat's row through `SeatEncoder::Observe` (tracking when the seat entered combat for the combat-time
feature), then `WriteState`.

`Reward`: `BeforeRewards` on the arena's encounters in reward order (a pull's clear is decided once here, for every
seat), then for each seat compute `LastStepDamage` (damage / damage scale) and `LastStepDamageTaken` (damage taken / max
health), which several encounters read, and let each encounter add its terms to the seat's ledger. The ledger's step
total is the seat's reward. Finally `AfterRewards` (a cleared gauntlet pull is removed).

`IsTerminal`: a failed build, or any active encounter's `IsTerminal`.

### The seat view and encoder

`SeatView` is everything the blocks can't read from the world themselves: the layout, bot and target; the character
as built (level, race, spec, talent build); last-step damage, power change and damage taken; combat time; the supplies
it carries; whether self-resurrection is allowed; a hunter's stable; the enemy slots and selected slot; pull state
(pulls cleared, quiet time, pull time, elite pull, food and drink items); the owner; the three teammates and the
party's tank; the enemy player and whether it is a learned seat. Encounters fill the parts they own (`Encounter::View`).

`SeatEncoder::Observe` applies these rules in order:

- The row and mask start at zero, and **action 0 (no-op) is always allowed**.
- The character features (level, race, spec, talents) are always written, alive or dead.
- **A dead bot** sees only the duel block's dead features (whether it can resurrect itself), and its only possible
  action is the self-resurrect action.
- **No target** (between gauntlet pulls) blocks observation and actions, unless the layout acts without a target, which
  is any layout with the gauntlet block (food, drink, and self-cast spells between pulls).
- **Hidden enemies.** An enemy the bot can neither see nor detect (`CanSeeOrDetect`: stealth, invisibility) is left out
  of the view, as a client leaves it off the screen. An enemy slot holding one reads empty, the pvp block writes only
  what the bot remembers about a hidden opponent, and a hidden target is `HiddenTarget` instead of `Target`: every block
  sees no target, but the seat still observes and acts, and the duel block shows where the target was last seen and
  lets the seat search there. The critic's state (4.8) keeps everything.
- Otherwise every block writes its slice and mask.

`SeatEncoder::Apply` ignores actions of a missing or dead bot (except its own resurrection). It runs every block's
`BeforeApply` on every decision, including a no-op, then hands a positive action to the block that owns it, as an
index local to that block.

## 4.3 Characters

### Class/roles

| Class | Class/roles (specs) |
|---|---|
| Warrior | `warrior_dps` (Arms, Fury), `warrior_tank` (Protection) |
| Paladin | `paladin_heal` (Holy), `paladin_tank` (Protection), `paladin_dps` (Retribution) |
| Hunter | `hunter_dps` (Beast Mastery, Marksmanship, Survival) |
| Rogue | `rogue_dps` (Assassination, Combat, Subtlety) |
| Priest | `priest_heal` (Discipline, Holy), `priest_dps` (Shadow) |
| Death Knight | `deathknight_tank` (Blood), `deathknight_dps` (Frost, Unholy) |
| Shaman | `shaman_dps` (Elemental, Enhancement), `shaman_heal` (Restoration) |
| Mage | `mage_dps` (Arcane, Fire, Frost) |
| Warlock | `warlock_dps` (Affliction, Demonology, Destruction) |
| Druid | `druid_dps` (Balance, Feral cat), `druid_tank` (Feral bear), `druid_heal` (Restoration) |

Each `SpecProfile` (`Character/ClassRoleProfile.cpp`) sets a talent tab, a stat profile for gear (strength melee,
agility melee, ranged, caster, healer, tank), a range band (melee or ranged, which drives approach shaping) and weapon
layouts tried in order (two-hand, dual wield, daggers, one-hand, one-hand and shield, one-hand and held item, staff,
two-hand stat stick and ranged, optional wand).

Tank and healer roles play their own spec's damage game in role gear until the companion and party stages give them
their actual jobs.

### What `SeatCharacter::Configure` builds

The same function builds training seats and live companions, so a model gets in play exactly what it trained with.

- **Talents.** Every character draws one of three plans (`SeatCharacter::TalentPlan`, `Characters.*TalentChance`),
  reported as the episode info column `talent_plan` and scored as its own group in every evaluation:
  - **standard** (60% by default): the spec's standard 3.3.5 build from `Character/SpecBuilds.cpp` (31 specs),
    generated by `tools/spec_builds/generate.py` from `builds.py` and checked by `validate.py`. A build lists talents
    in the order players take them. Each point goes to the first talent in that order that still wants ranks and is
    legal (row and prerequisite rules follow `Player::LearnTalent`), so a low-level character has the talents players
    pick first. Every build spends exactly 71 points at level 80. Where a spec's tree has a root, snare or silence
    a player of that spec takes for solo play, the build takes it too (fury Piercing Howl, marksmanship Concussive
    Barrage, survival Entrapment, subtlety Waylay, shadow Silence, frost death knight Hungering Cold, affliction
    Curse of Exhaustion, feral cat Infected Wounds), so the policy has them to learn with.
  - **noisy** (30%): that build stopping 1 to `Characters.TalentNoisePoints` points short, with the rest spent at
    random -- a build somebody made up the tail of.
  - **random** (10%): every point spent at random, deeper rows likelier, the spec's tree first (51 points, what its
    last row needs) and then the others.

  The variety is what makes the talent features worth reading: a standard build is the same every time for a spec and
  a level, so a policy trained on those alone can ignore its talents and memorise the spec. A policy that sees all
  three has to play the character it was handed -- which is what a live server hands it. Glyph slots the level has
  opened get the spec's standard major and minor glyphs, whatever the plan.
- **Kit** (`ClassKit`). Every spell the class trainers teach up to the level (`trainer`/`trainer_spell`, learn-spells
  resolved), talent-gated ranks when the talent was taken, and class quest spells trainers don't teach (stances, Bear
  Form, warlock demons, Raise Dead). Weapon and armor skills the race and class may have, maxed for the level.
  Reagents: totems, Ankhs, soul shards, corpse dust, flash powder, Light Feathers for Slow Fall and Levitate, and
  ammo in a quiver or pouch for hunters.
- **Gear** (`GearBuilder`). A random level-appropriate item for every slot, including both trinkets, drawn from every
  obtainable item (loot, vendors, quest rewards, crafted) the class can use and whose stats suit the spec. Random-stat
  items roll only suitable suffixes. The item level must fall in the band players of that level wear
  (`ITEM_LEVEL_ANCHORS`: a few levels above while levelling, Outland gear from 58, Northrend from 70, heroic dungeon
  180-213 at 80). If nothing fits, the search widens to 10, 25, then any number of item levels below the band, never
  above. After that it falls back to lighter armor, then stat-less items. Dungeon drops are weighted 4x and quest
  rewards 3x, and items near the middle of the band are preferred. Epics appear only at 70 and 80, and resilience gear
  only in PvP arenas. Paladins, shamans, druids and death knights get a relic.
- **Enchants and gems** (`GearEnhancements`). At 70 and 80 every item is enhanced, and half of them while levelling:
  the best suitable enchant a player of that level could buy (enchanting skill 5 per level, reaching 300 at 60 and 450
  at 80; weapon procs by name), a matching gem per socket (the socket bonus when all match, meta gems last, epic gems
  only at 80), death knight runeforges, rogue poisons (Instant in the main hand; Deadly in the off hand, or Crippling
  half the time once the rogue can have it), and shaman weapon imbues (enhancement: Windfury and
  Flametongue; elemental: Flametongue; restoration: Earthliving; the highest rank known). No profession-only enchants
  or gems.
- **Riding** (`TravelBlock::LearnRiding`, layouts with the travel block). The level's riding as players learn it
  (Apprentice at 20, Journeyman at 40, Expert at 60, Cold Weather Flying at 68, Artisan at 70) and the side's mounts
  of each speed (60% and 100% ground, 150% and 280% flying).
- **Supplies** (`Supplies`, applied by `StockSeats`). Five of the best healing potions, five mana potions (for mana
  users), five bandages with the level's First Aid skill, a healthstone and soulstone for warlocks, and at 70 and 80 a
  suitable flask (half the time an elixir while levelling). Gauntlet stages add the best vendor food and, for mana
  users, drinks: `Pulls.GauntletSupplies` (7) of each alone, five with an owner.
- **`PrepareFighter`**. No XP gain (levelling would change the character under the model), a warrior's stance (a first
  login normally casts it, and nothing works without one), and for hunters a stable offer of four tameable beasts of
  different random families.

### Raid stages

`stage28_raid_single` and `stage29_raid_gauntlet` train `MAX_SEATS` learned seats as `RAID_GROUPS` groups of
`GROUP_SEATS` (`SeatPlan::Raid`), each group with its own tank and healer. The first is one elite and its adds, won
or lost as the single pack is; the second is pull after pull with recovery between, which is what a wing of a raid
instance is before its boss.

A raid is not a bigger party, so it does not fight a bigger pack. Its rungs (`RAID_RUNGS`) put the difficulty in what
the enemy is -- elite, levels above, something on the ground -- rather than in how many there are, which `PACK_SLOTS`
caps at what a seat can observe anyway. The seats outnumber the enemies on purpose: what is being trained is
coordination against a fight that punishes standing in the wrong place.

Neither stage is in the default queue, and **neither is runnable at the usual env count**: forty seats an env is
forty bots an env, so `AnimusForge.Envs` has to come down roughly in proportion (a few dozen envs, not 128) before
starting one. Train by name: `forge start stage28_raid_single`.

### What an enemy is doing

A seat used to know one thing about an enemy's spellcasting: that it was happening. One bit, no identity, no clock.
It could not tell a filler from a heal, could not see an area effect on the ground at all, and had never read a
threat table -- so interrupting well, stepping out of fire and holding aggro were all unlearnable, and every dungeon
and raid mechanic has one of those three shapes.

All of it is now described by properties rather than by which spell it is (`IncomingSpell`), because a boss's
abilities live in file-scope `enum Spells` blocks inside its own script and no registry of them exists. A level 12
gnoll shaman's Lightning Bolt and a raid boss's produce the same features, and an unseen encounter needs no new code:

- **The cast** (per enemy slot, and for the duel's target): how much of it is left, whether it is aimed at this seat,
  area, cone, channeled, interruptible (by `EffectInterruptCast`'s own test, so the feature promises what pressing an
  interrupt would do), dispellable, shared damage (a soak), a heal, a summon, its radius, its missile flight time,
  its school and its mechanic.
- **The ground it is on** (`Encoding::StandingInHazards`): how many hostile ground effects the seat is standing in,
  how far it still has to walk to leave the worst, and the bearing of that one's centre, so moving away from it is
  the way out. Free to compute -- a ground effect applies an aura, and the aura knows the object that owns it.
- **The ground it is about to be on** (`Encoding::FindNearestHazard`): the nearest hostile ground effect it is *not*
  in yet, within 30 yd -- distance to its edge, bearing, radius. This is what makes avoidance learnable rather than
  only escape: without it nothing distinguishes clear ground from ground about to be walked into. A grid search over
  dynamic objects and armed traps, run once a second and re-measured arithmetically between searches, since a ground
  effect stays where it was cast.
- **What was done to it** (`Encoding::IncomingDebuffs`): harmful auras on the seat, how many are dispellable, the
  worst stack count, the longest remaining, and which crowd control mechanics are among them.
- **Threat** (`Encoding::ThreatShare`): the seat's own threat over the threat of whoever the enemy is on, so 1 means
  it holds aggro. Until this, every "threat" in the codebase was a proxy counted from who an enemy happened to be
  swinging at.

`hazard_seconds`, `hazard_damage` and `interruptible_casts_seen` in the episode info say whether any of it is being
used -- the last is the denominator the press-to-interrupt ratio always lacked.

For any of it to be learnable, the opponents have to produce it. Nothing did: the duel's pool is default-AI
creatures, which never cast, and no creature anywhere was selected for putting something on the ground. So
`Difficulty.CasterChance` (40%) draws a share of duel opponents from the same cast-only scripts the packs use, with
`Difficulty.HazardChance` (30% of those) from the ones that create a persistent area aura; and the upper pack rungs,
the last planned pulls and every raid rung include a hazard caster (`OpponentPool::RandomHazardCaster`). One enemy,
one cast and one pool of fire in a stage 1 duel is the cheapest place any of this can be learned.

### Durative actions

Most actions are one press of one button, and a 450 s episode is 1800 of them -- far more than credit reaches back
over. Two actions and the move block's held keys instead stand for a stretch of decisions (`SeatOption`,
`Options.*`), so a plan can be expressed in one choice:

| Action | Block | What it does until it stops |
|---|---|---|
| `rest_until_ready` | gauntlet | Eats and drinks, whichever is missing, until health and mana are back to 90% |
| `hold_interrupt` | pack | Interrupts the target the moment it starts casting, with the first interrupt the seat has -- its own spell, or its pet's (a felhunter's Spell Lock) when it has none. Offered only to a seat that has one |
| the eight bearings | move | Walks that compass point, re-aimed from where the seat stands every decision, for `Options.MoveBearingMs` (3 s) or until the feet are told something else |
| `follow` | companion | Runs to just behind the owner, re-aimed at where the owner is now every decision, for `Options.FollowMs` (6 s), until the seat is there and the owner has stopped, or until the feet are told something else. A press, not the client's right-click follow: the policy re-presses to keep following |
| `turn_left`, `turn_right`, `pitch_up`, `pitch_down` | move | Turns or pitches 15 degrees a decision for `Options.MoveTurnMs` / `MovePitchMs` (750 ms), or until the opposite key |

A seat runs **four at a time**: one positioning option (the bearing, or the follow), one standby, a turn and a pitch
(`SeatOptionSet`), since walking, waiting for the target's cast and looking round are not alternatives. Each runs in
its block's `BeforeApply`, every decision, and stops on its own condition (the fight starts, nothing is left to eat,
the interrupt fires, the seat halts or jumps) or when its `Options.*` clock runs out. What any other action does to
it depends on what it is:

- **positioning** (the held bearing, the follow): only the feet take over -- another bearing, the halt, a jump, the
  follow itself. A turn or a
  pitch does not, so the walk curves rather than stopping; casting and swinging do not either, since a fight is
  spells and swings between steps. Nothing but the feet may end it: the duel block's per-decision hook used to
  clear the slot whenever there was no living target, which in a travel arena is always, and every bearing
  ended one decision after it was pressed (2026-09-21 to 09-23; the three-second hold was a one-decision hold).
- **aiming** (the held turn and pitch): only its own opposite, or levelling off, takes over. Ending it on any
  press meant a seat could not turn while it did anything else.
- **standby** (`hold_interrupt`): nothing the seat does takes over from it, because waiting for the target's cast is
  not something it stops fighting to do. Cancelled by any press, a hold lasted 0.6 s against casts of 1.5-2.5 s and
  interrupted next to nothing.
- `rest_until_ready`: any other action ends it, as recovering is what the seat is doing rather than something it waits
  through.

Each option's own action is masked while it runs, so nothing cancels itself. What it does is counted as a press would
be (food and drink used, an interrupt pending on a caster). The core block reports how much of each option's clock is
left (the companion block reports the follow's, so only its layouts carry that feature), so a running option is never
hidden state, and `options_started` and `option_seconds` in the episode info say how much a class uses them.

### The action catalog

`ActionCatalog` (per class, built once) is the fixed action space of the core block:

- no-op, cancel queued on-next-swing ability,
- **one action per rank chain of every combat spell** a level-80 character of any of the class's races knows (trainer,
  starting and racial spells, active talents of all three trees). The action casts the highest rank the bot knows. A
  spell counts as combat if it deals damage, applies a damage-relevant buff, debuff or DoT, generates resources,
  shapeshifts or summons, or if it is what separates a player from a rotation: charges and leaps (Charge, Intercept,
  Intervene, Blink, Disengage), threat redirects (Misdirection, Tricks of the Trade), Spellsteal, and survival auras
  (speed; mechanic and school immunity such as the PvP trinket, Every Man for Himself, Will of the Forsaken, Hand of
  Freedom and Fear Ward; dodge, parry, block, reflection; Feign Death, Fade and invisibility). Mounts, teleports,
  crafting, pure heals and charm are excluded. The candidates include the spells a learned spell teaches (the Feral
  Charge talent is Feral Charge - Bear and Feral Charge - Cat), and a spell that fires a missile is judged by the
  missile's spell (Freezing Arrow drops a Freezing Trap),
- one action per trinket slot, and one each for the use effect of the main-hand and off-hand item (weapons, shields
  and held books such as Rituals of the New Moon; `item_uses` counts them),
- then the rest of the kit, from the first stage on, as a player fights with it: **tactical spells** (interrupts, stuns
  with Sap included, silences, fears, roots, polymorphs, knockbacks, taunts, snares, disarms, traps, Distract and
  offensive dispels) cast at the target, and **sustain spells** (heals, HoTs, absorbs and friendly dispels) cast on the
  bot itself. Until they were core actions a duel healer had no heal and a duel mage no Polymorph or Ice Barrier.
  Each catalog entry in the manifest names its `group` (`combat`, `tactical`, `sustain`). Every layout's manifest,
  and its entry in `stage.json`, lists `action_names`: every action of the layout by name (`frostbolt_116`,
  `use_off_hand`, `start_attack`, `pet_stay`), which evaluations use to count the actions each episode took.

The catalog also keeps the lists apart for the blocks that cast them elsewhere:

- `Tactical()`: the tactical spells above.
- `Sustain()`: the sustain spells above.
- `Revives()` (companion and party blocks): Resurrection, Redemption, Ancestral Spirit, Revive, Rebirth (their target
  is a corpse, `TARGET_FLAG_CORPSE_ALLY`), and a warlock's soulstone.

Each spell action also carries its **kind**, read from its first rank's effects: `Healing` (heals, HoTs, absorbs),
`Rankable` (a healing chain with more than one rank), `DirectHeal` (a heal on one unit and nothing else), `Defensive`
(a damage-taken reduction, immunity, split damage or avoidance buff under five minutes), `LongBuff` (a positive buff
of ten minutes or more) and `KeepsAura` (a HoT, absorb, long buff or defensive kept up on one unit, not stacking).
`Layout::BuffGroups` joins the long buffs a unit can only have one of (chains sharing a `spell_group`, such as the
blessings, or Fortitude and Prayer of Fortitude).

**Friendly targets.** A positive spell that takes a unit target (a heal, shield, HoT, blessing, Hand, Power Infusion,
Innervate) is cast on the **support block's selected friend** (`Encoding::SupportTarget`): the bot itself, the owner or
a teammate. Without the block (stages 1, 2, 6, 7 and the travel stages) it is the bot itself, as before. There is one
action per spell whoever it lands on; the companion and party blocks no longer copy the heals per ally (they listed
every heal twice, and the policy could not see what was already on the owner).

**Rank tiers.** A `Rankable` heal is cast at the seat's rank tier (`Encoding::KnownRank`): the highest known rank, or
the highest active rank about two thirds or a third of the way up the known ranks, for mana on a long fight. Every
mana spell keeps all its ranks active in the spellbook; only rage, energy and runic power abilities, paladin auras and
druid forms supersede their lower ranks (`Player::addSpell`).

Every action is masked each decision by the core's own `Spell::CheckCast`, run without casting (race, level, talent,
cooldown, GCD, power, stance, range, reagents). Beyond it, a cast that can only be wasted is never offered: a
`DirectHeal` on a friend at full health, a `KeepsAura` spell whose own aura on that friend still has more than a quarter
of its duration (or charges) left, and any positive unit-target spell while the selected friend is dead or gone. A
spell whose only effect is a control aura on its caster (Grovel, from the hidden GENERIC skill every character has)
is not in the catalog at all. As on a client, **no spell or item can start while a cast is in its cast
time**. The core only enforces that for client casts, and without the check a bot's new cast would silently replace the
current one.

Casting goes through `ApplySpellAction`, which builds the `SpellCastTargets` a client would send for the spell.

**Pacing** (`Actions.*`, `SeatMemory`, the same for forge seats and live companions). A decision comes every
`DecisionMs` (250 ms), and a policy free to act on every one re-issues orders no
player would: stage8_duel's warlocks sent their pet in 125 times an episode and started and stopped the same cast
over and over while never engaging. So the scenario masks, on top of every block's own checks, an action pressed too
recently: the same action again within `Actions.RepeatMs` (1000 ms; `Actions.MoveRepeatMs`, 300 ms, for movement
orders, so steering stays responsive), stopping a cast before it has run `Actions.StopCastMinMs` (500 ms), and
starting a spell the bot stopped itself within `Actions.RecastAfterStopMs` (2000 ms). Spells keep their GCD and
cooldowns as well. One lock keeps a plan from dissolving into dithering: a stance, form, presence, aspect, aura,
seal, armor or pet stance holds `Actions.ModeLockMs` (5000 ms) before another change of its kind (warrior tanks
changed stance 22 times a fight, hunters their aspect 12). The reverse-move lock went with the target-relative moves
it paced: a bearing has no toward or away. A paced action a policy sends anyway does nothing. `actions_per_minute` in the episode info shows how busy a
seat was.

**Repeats** (`Actions.Repeat`, every stage). Pacing caps how soon an action can be pressed again, not how often: a
policy can still press one button every second for a whole episode (stage8_duel's warlocks gave their pet 93 orders an
episode while it dealt 1% of their damage). Each press of an action counts that same action's presses within the last
`Actions.RepeatWindowMs` (10 s); past `Actions.RepeatFree` (3) of them, each press costs `Actions.Repeat` (0.02).
Movement orders are always free. How often the seat acts overall is not charged, only the same action over and
over. `repeated_presses` in the episode info
counts the charged presses.

## 4.4 Blocks

| Block | Observation (summary) | Actions |
|---|---|---|
| `core` | globals plus five durative-action clocks (see below), then 6 features per catalog action (known, cooldown, aura on target, aura on self, stacks, time since the seat pressed it), then rank / max rank per class talent, then points per tree / 71 | The catalog |
| `move` | Whether it is moving and how fast; the bearing it is walking (one-hot over the eight, or none); its own facing as sine and cosine, which way it is turning, how far up or down it is looking, and which facing mode it is holding; the bearing and distance to the target, all zero without one; the bearing, distance and width of the nearest ground effect it is not standing in; the objective's bearing and its distance twice, over 500 yd and again over 40 so the last few yards are resolvable; **how far the ground runs along each of sixteen rays** -- the eight bearings and the rays half way between them, each marched at 6, 12, 20, 30 and 40 yd, reporting the distance to the first thing that stops it, what stopped it, and whether it was water or something that burns; water -- in it, under it, how long under it, and how fast it swims; the detour the ground costs, whether its legs are getting anywhere, its clearance and the way out; and **a trail of where it has been** -- its last eight positions, one a second, each as an offset in its own frame, and how many of them it is still standing on | 8 egocentric bearings (forward, forward-right, ... clockwise) and halt; three facings chosen apart from the feet (the target, the way it is going, hold); a held turn either way, which is the mouse-look and the only way to reach a heading between two bearings; a held pitch up, down or level, which is how it swims and flies; and a jump. It is the only way a seat moves: the duel block's target-relative orders were the pathfinder choosing a position on the policy's behalf, and they are gone |
| `duel` | Distance and bearing to the target, behind it, it faces the bot, its combat, target and casting state; the bot's movement, combat, stealth and auto-attack; damage taken last step; pet out, health, attacking; combat time; current cast progress and time left; a cancellable form; potions, healthstones and bandages carried and their cooldowns; Recently Bandaged; can resurrect itself; a hidden target, time since it was seen, and distance and bearing to where it was last seen; the target in line of sight; what the target is (creature type one-hot, max health against the bot's, damage multiplier, the share of the bot's hits its armor takes off, run speed, level difference, immunity to six magic schools and to fear, stun, root, snare, silence and polymorph); the bot stunned, feared or confused, rooted, silenced, snared; hunters' stable families and pet types | Start attack, pet attack, stop casting, cancel form, healing potion, mana potion, healthstone, bandage self, soulstone self (warlock), resurrect self, 4 call-beast actions (hunter). No movement: where to stand in a fight is a bearing, chosen against the target's bearing and distance reported here |
| `pet` (hunters, warlocks, death knights, mages; empty for others) | The pet's presence, health, power, distance to the target, attacking it, casting, stance, following or staying; what it is (a ferocity, tenacity or cunning beast, an Imp, Voidwalker, Succubus, Felhunter or Felguard, a ghoul, a Water Elemental); whether it leaves on its own and how soon; its four most useful abilities (interrupts, then crowd control, dispels, threat, help, damage): present, on cooldown and what each does | Cast each ability (at the target, or on itself when helpful) as the pet bar does; passive, defensive, aggressive; follow; stay |
| `pack` | Living and in-combat enemy counts; 4 enemy slots (present, alive, health, distance, bearing, behind, attacking the bot or its pet, casting, in combat, crowd-controlled, current target, elite, level difference, in line of sight); (the tactical spells are core actions, cast at the selected enemy) | Select target slot 1-4 |
| `gauntlet` | Pulls cleared, pull active, time since the last fight, time into the pull, elite or higher-level pull, eating, drinking, food and drink left, time until an unengaged pull comes to the bot, time until the next pull spawns (the sustain spells are core actions) | Eat, drink (offered only where the item's cast check passes) |
| `companion` | The owner's presence, health, mana, distance, bearing, combat, movement, level difference and class; enemies on it; which slot it attacks; which enemies attack it; each revive's known and cooldown | Follow, assist (owner's target), guard (an enemy attacking the owner), one revive-on-owner per revive |
| `party` | Living party size, the most hurt ally's health, living tank and healer present; per teammate: presence, health, mana, distance, bearing, combat, role, class, attackers, target slot, which enemies attack it | Follow the tank; per teammate: assist, guard, revives |
| `party` teammate goals | Each teammate's goal one-hot (`SeatGoal`), so a party can divide the work | |
| `party` raid summary | The seat's group index, the living share of the raid and of its own group, the share of the living in combat, the most hurt living seat anywhere, and living tanks and healers over `RAID_GROUPS` | |
| `support` (stages 3-5, 8) | The selected friend and rank tier (one-hot); per friend slot (self, owner, then the party block's teammate slots): presence, alive, health, mana, distance, line of sight, attackers, role, the bot's own HoT (duration left) and absorb on it, buff coverage | Select a friend (the target of positive unit-target spells); set the rank tier (high, mid, low) |
| `pvp` | The opponent's class, role, level difference, mana, rage/energy/runic power, crowd-controlled, stealthed, pet out, casting a heal; the bot stunned/feared, rooted or silenced; whether the opponent is a learned agent; what a player tracks from what it saw used: the opponent's trinket cooldown, racial control break cooldown and number of spells of a minute or more cooling down; diminishing returns (controlled and opening stuns, fear, disorient, root, silence, horror, cyclone) on the opponent and on the bot, and the crowd control each has left; the opponent hidden (then only class, role, level, the cooldowns and diminishing returns are written) | none |
| `context` (12) | Owner present and alive, living teammates, living enemy players and creatures in the slots, nearest enemy player's distance, a player attacks the bot or the owner, PvP flag, battleground/arena map, dungeon/raid map, self-resurrection allowed, group size | none |
| `hostiles` (14 per slot) | Per enemy slot: player or creature, class, casting a heal, stealthed, pet out | none |
| `travel` (16) | Mounted, on a flying mount, can summon a ground or flying mount now, riding skill, indoors, height above the ground; the objective's presence, distance, bearing (in the seat's own heading, the frame `move` uses) and height; at the objective; in combat; speed; moving | Mount the fastest ground mount (masked in an air-only arena), mount the fastest flying mount, dismount. Follow-route is gone with the route features: a pathfound leg walked by the engine was the engine navigating, and the policy pressed it in most of its episodes. The route still exists for the reward's progress shaping and the episode's measurements; the actor never sees it. `move` steers on the ground and in the air alike |
| `flag` (17) | Carrying the other side's flag; the seat's flag at base, carried or dropped; the other's at base or dropped; distance and bearing to both bases and to the nearest dropped flag; both scores | none |
| `order` (13) | What the side's director asked of this seat: the posture and rally one-hots, distance and bearing to the rally place, distance, bearing, health and whether the seat is already on the called target, and whether this seat holds the duty. All zero in an arena with no director | none: an order is advice, not a lever |

The core block's globals are five durative-action clocks -- resting, the held interrupt, the held bearing, the
held turn and the held pitch; the last two run alongside the feet rather than instead of them, so they have slots
and clocks of their own -- and these features: level; race one-hot (10); the aptitude vector (Aptitude::COUNT: what the build can taunt,
mitigate, heal, control, buff, cleanse, protect, revive, summon and swim with, and where its points went);
health; mana; rage; energy; runic
power; six runes; combo points; form one-hot (13); GCD; casting; queued next-swing; main-hand, off-hand and ranged
swing timers; main-hand speed; target health; target distance; in melee range in front; attack power; spell power;
melee and spell crit; melee and spell haste; melee and spell hit; expertise; armor penetration; last-step damage;
last-step power change; time into the episode (/ 5 min, `EPISODE_TIME_SCALE_MS`); and what the seat has been doing
(`SeatMemory`): time since its last movement order, time since its last stance, form, aspect, aura, seal, armor or
pet stance change, and its own and its target's health
against their average over the last few seconds. All are normalised (see
`Blocks/CoreBlock.h` for the scale of each). The episode time is elapsed time, not the share of the limit left: a
companion has no limit, and without a clock a bot standing still out of combat sees the same row every decision, so a
deterministic policy can repeat a loop forever. A policy sees one observation at a time: without the memory features it
re-decided from scratch every decision, running in and backing off by turns and dancing between stances. A forge seat
and a live companion keep the same `SeatMemory`, so a model plays with what it trained with.

Movement and casting constrain each other: movement actions are masked while casting, and cast-time or channelled
spells are masked while running. The bot turns to face its target whenever it isn't running. Stop casting and cancel
form need no target, so they stay available between pulls. Layouts with the travel block act without a target too.

**The move block needs no target at all, and it is the only way a seat moves.** The duel block's movement used to be
target-relative -- `MOVE_TO_TARGET`, `MOVE_TO_RANGE`, `BACK_OFF`, `KEEP_RANGE`, `STAY_ON_TARGET`, `STOP` and
`BREAK_LINE_OF_SIGHT`, each a position the pathfinder chose and walked to -- and it is gone: where to stand in a
fight is a bearing chosen against the target's bearing and distance, learned rather than ordered. A bearing is
chosen against the seat's own facing and cares about nothing else. Its facing actions
are what make a strafe expressible: `SetFacing` on the spline, so the seat can run one way and look another, where
a spline left to set its own orientation always turns the seat the way it is going.

**Water.** The move block reports water twice: `OBS_WATER_FIRST`, eight bearings beside the ground probe, saying
what lies that way before the seat is standing in it; and `OBS_IN_WATER`, `OBS_SUBMERGED`, `OBS_SUBMERGED_TIME` and
`OBS_SWIM_SPEED` for water it is already in. Three things had to be fixed before any of that meant anything, and
each was hiding the one under it (animus-lib `eb1f91c`):

- **`GroundReach` called water a wall.** It looks for ground within `MAX_STEP` either side of the seat's own
  height, and a lake bed is well below that band, so `GetHeight` returned `INVALID_HEIGHT` and the probe reported
  reach 0 -- the same answer it gives for a cliff or the edge of the map. The seat was being taught that the one
  route it might swim was impassable.
- **A seat could not get in.** The three-dimensional steering is reached through `Airborne()`, which is
  `IsInWater() || CanFly()`, so a seat could swim only once it was already swimming; entering was a pathfound
  ground step, and mmaps drops the terrain under real liquid, so the walkable mesh stops at the waterline and the
  step had nowhere to land. Sampled across a whole run, every seat near water sat 0.1 to 0.4 yards above the
  surface. Entering is its own case now (`Encoding::SwimTo`: straight in, no pathfinding, no fly flag -- and what a
  seat already in the water uses, since the old path called `FlyTo` and a swimmer is not a flier).
- **`Player::IsInWater()` could never be true for a bot.** `Unit::IsInWater` reads the map; `Player` overrides it
  to return the cached `m_isInWater`, and the only caller of `SetInWater` in the core is the movement opcode
  handler. A sessionless bot sends no opcodes, so the flag was false for the entire life of every bot the sim has
  ever run, and those four water observations were inputs that never changed. `ObserveSeat` keeps that state from
  the map now, once a decision per seat -- the client's job, on a server that has no client.

`swim_seconds` was 0 in every episode of every run before this, which is not a choice a near-random policy makes a
quarter of a million times. It is reported and never gated: every crossing the water arena places has a dry way
round by construction, so where that way round is quicker, walking it is the right answer and a floor on swimming
would punish it. `crossing` keeps its floor, because what the arena offers is the ground's business.

A bearing is held rather than stepped, so the resolution of the path comes from `AnimusForge.TicksPerDecision`
(2.3, 8.1) rather than from deciding more often: the world walks the spline in however many ticks a decision is cut
into, and the policy still chooses once per `DecisionMs`.

Anyone in the air without flight (a dismount, a cast that took the mount away) falls to the ground with a player's fall
damage (`MoveFall`, `Player::HandleFall`).

With the pack block, every spell, movement and pet action aims at the **selected enemy**. When it dies, the nearest
living enemy becomes the selection.

## 4.5 Encounters

An `Encounter` owns one part of what an env contains besides the seats. It keeps its own per-env state and has hooks
for each phase: `RewardTerms`, `AddEpisodeInfo`, `ResetEpisode`, `BeforeRebuild`, `Build`, `UpdateEnemies`, `Update`,
`SelectTarget`, `BeforeSeatAction`, `OnSeatAction`, `View`, `BeforeRewards`, `Reward`, `AfterRewards`, `WriteState`,
`IsTerminal`, `OnRecovered`, `OnPullStarting`, `Deactivate`, `Teardown`.

**`CreatureEncounter`** (`Opposition::Creature`). It spawns a random creature whose natural level range covers the
seat's level: normal rank, attackable, default AI with no script, no NPC services, not a civilian, guard or trigger,
walking on the ground in plain sight (no flying, hovering, swim-only or rooted movement, and no stealth or invisibility
aura on its addon), and spawned somewhere in the world. **Difficulty adapts per class and build** (`Difficulty.*`): tier t
below `EliteTier` (4) is a normal creature t x `LevelsPerTier` (1) levels above the seat, and from `EliteTier` on an
elite, (t - `EliteTier`) levels above, up to `MaxTier` (6). A class and role moves up a tier once it wins (kills without
dying) `RaiseAbove` (90%) of `Window` (200) fights at its tier, and down below `LowerBelow` (60%);
`ReviewChance` (25%) of its training fights come from a lower tier, so none is forgotten, and `StretchChance` (10%)
from the tier above, which does not count towards moving it: an evaluation scores every tier, so a class and role that has
stalled should not be meeting the tiers above its own for the first time there (the rogue sat at tier 4 and lost 44% of
the elite fights it was scored on). A fight that simple play wins
every time teaches nothing a plan would add. An evaluation spreads its seeds over every tier, every class and role over
every one (seed i plays pair i mod the pairs, at tier (i / the pairs) mod the tiers), so two
checkpoints meet the same fights, and the summary scores each tier on its own
(`difficulties`). Tiers restart at 0 with the worldserver.
`difficulty` and `opponent_elite` in the episode info say what each fight was. The bookkeeping is
`DifficultyLadder`, which the single pack's ladder shares. The creature is summoned at the seat's
level plus its tier's levels (`PendingSummonLevel`) 40-50 yd away at a random line-of-sight bearing on level ground
the seat can walk to (a path at most 1.5 times the straight line), facing a random way, hostile and aggressive, without
health regeneration. It starts out of aggro range. A creature with no path to its victim stops and regenerates, then
evades home at full health after 10 s, which no play can win: after 3 s without a path it is put beside its victim
instead (as instance trash is with `Creature.Instance.TeleportToUnreachableTarget`). `target_unreachable_seconds` and
`target_teleports` count it. Rewards are the one-on-one terms (`CombatReward::OneOnOne`). The episode is terminal on the
kill or on death with no resurrection left.

**`PullsEncounter`** (`Opposition::Pulls`). The pack pool adds creatures whose SmartAI only casts or talks (about 3500
casters and ability users) to the duel pool.

- **Single pack** (stage 2): creatures at the seat's level, clustered 40-50 yd away. `Pulls.LinkedChance` (70%)
  are linked, meaning once one member is in combat the rest attack. The episode is terminal on clear, death or the
  clock. **The pack climbs a ladder per class and build**, with the duel's `Difficulty.*` rates (up at 90% of 200 packs
  cleared without dying, down below 60%, 25% reviews), up to `Pulls.MaxTier` (5). Every rung has a spellcaster: a
  creature whose SmartAI casts a spell with a cast time, one an interrupt can stop (`OpponentPool::RandomCaster`).
  The other members are any pack creature, and the slots are shuffled.

  | Rung | Pack |
  |---|---|
  | 0 | 2: a caster and one more |
  | 1 | 3: a caster and two more |
  | 2 | 4: a caster and three more |
  | 3 | 4: two casters and two more |
  | 4 | 3: a caster, an elite and one more |
  | 5 | 4: two casters, an elite and one more, a level above the seat |

  Evaluations spread their seeds over the rungs as the duel's over its tiers, and `difficulty` in the episode info is
  the rung. A stage viewer's `spawn` tier picks the rung.
- **Gauntlet** (stages 3-5, 8): 1-4 creatures, or `EliteChance` (15%) a single elite, or `HigherLevelChance` (25%) a
  pack 1-3 levels higher (at most one level below level 20 and two below 30). In a party arena each member is elite
  with `PartyEliteChance` (50%) and up to 2 levels above. After a clear the field empties and the next pull spawns
  `NextPullMinMs`-`NextPullMaxMs` (8-20 s) later, out of aggro range. With an owner, pulls spawn around the owner.
  **Alone** the gauntlet is paced: a pull nobody has engaged comes to the seat `ArriveMinMs`-`ArriveMaxMs` (20-40 s)
  after it spawned (creatures that can't see the seat walk to it), and each pull cleared takes `ArriveShrinkMs` (1.5 s)
  off that, down to `ArriveFloorMs` (10 s), and `NextPullShrinkMs` (1 s) off the break, down to `NextPullFloorMs`
  (4 s). Resting has a clock, and staying away from the pulls is no way to last.
- **Elites.** The pool takes elites with health and damage multipliers up to 3 and 2.5 (normal creatures: 2), which
  keeps open-world and quest elites; at 2 the whole world had six.
- **Clear and interrupts.** A pull's clear is decided once per decision in `BeforeRewards`. The interrupt reward pays
  when a seat cast an interrupt, stun, silence, fear or polymorph at a casting enemy and that enemy's cast was then cut
  short by something other than itself or its death. A cast that finishes on its own doesn't count.
- **Deaths in owner arenas.** Deaths don't end the episode. After a pull, the dead wait up to
  `Resurrection.GraceMs` (20 s), and the next pull waits with them, for a resurrection they can get: their own
  Soulstone or Reincarnation, or a living seat's resurrection spell. Then whoever is still dead, companion or owner,
  stands up with `RecoverFraction` (half) health and mana (`NotifyRecovered`). A pull that kills everyone is cleared
  away (a wipe). Every death is paid once, and again after standing up.

**`OwnerEncounter`** (`Owner = true`). A scripted player (`ScriptedPlayer`) within `Owner.LevelSpread` (2) levels of
the seats, with a random role (tank 25%, healer 25%, DPS 50%) and a class that can fill it, dressed like a seat, given
the seats' faction. It is the env's ally 0.

- Between pulls it wanders near the spawn point and regenerates (`RegenFraction` per second). `RunChance` percent of
  its steps are a run instead: a leg at a run to a spot `RunMinYards`-`RunMaxYards` from the spawn point with a real
  route there, and the wander's leash brings it back. The next pull spawns around wherever it is, so a seat that has
  not followed fights from behind.
- A tank owner starts every pull and taunts enemies off others. A healer owner heals the most hurt party member
  under `HealBelow` and stays within `HealerRange` of the tank. A DPS owner walks in after `OwnerEngageMin/MaxMs` (in a
  party, after `PartyOwnerEngageMin/MaxMs` so the tank can pull), and starts the pull itself `OwnerPullsChance` percent
  of the time. It casts a spell every `SpellMin/MaxMs`.
- Linked packs join in on whoever their engaged member fights.

**`PartyEncounter`** (`PartyGroup = true`). Every episode the owner, as leader, and the active seats form a real core
`Group` marked as a sim group (`CoreHooks::MarkSimGroup`), so party buffs, auras, group heals and every "party member"
check work as in play. It is disbanded before its members are replaced (`BeforeRebuild`). Each seat sees its three
teammates (the other seats, in order) and is rewarded for them.

**`OpponentEncounter`** (`ScriptedPlayer` or `MirrorSeat`). A scripted enemy player
(`EnemyPlayers::Create`) at the seat's level within `Opponent.LevelSpread` (1), with a random role (DPS 60%, tank 20%,
healer 20%), its spec, talents, kit and resilience gear, spawned 40-50 yd away. It engages within `EngageMaxMs` (3 s).
Melee specs fight in melee, ranged specs keep 10-30 yd (`RangedMin/Max`), and healers heal themselves below
`SelfHealBelow`. A rogue sneaks up in Stealth `ScriptedPlayers.StealthChance` (50) percent of the time and opens with
a stealth opener. `ScriptedPlayers.TacticsChance` (75) percent of engagements it plays its kit: it interrupts the
enemy's casts, crowd controls it every `ControlMin/MaxMs` (8-15 s) when it isn't already controlled, snares or roots a
melee enemy before backing off (ranged specs), breaks crowd control under `BreakBelow` (60%) health and uses a
defensive under `DefensiveBelow` (35%). Scripted enemy players see no more than a player: one that can neither see nor
detect its enemy stops attacking, goes to where it last saw it and searches around there. In a mirror arena the
"opponent" is the other seat. Both players get opposing factions and the PvP flag. Against a scripted player the
episode is terminal when the seat dies or kills it. In a mirror arena it is terminal when either seat dies, except in
a flag match. PvP arenas allow no self-resurrection.

**`AmbushEncounter`** (`Ambushers > 0`). One or two scripted enemy players with the opponent's class, role and gear
rules.

- Beside pulls (`ambush` arena), they arrive `Ambush.MinMs`-`MaxMs` (20-120 s) into the episode, engage within
  `EngageMaxMs`, attack the owner while it lives and then the nearest seat they can see (a hidden one only when they see
  none). They take enemy slots the pulls leave free (a pull has at most 4 minus the arena's ambushers creatures). Every
  seat earns `Ambush.Kill` (3) per ambusher killed, and the pulls and owner rewards pay the rest.
- Alone (`escort_duel`, `Opposition::Ambush`), exactly one ambusher is the whole fight from the start, paid as a
  one-on-one against it.

The pvp block observes the first living ambusher.

**`TravelEncounter`** (`Opposition::Travel`). An objective the seat has to reach: on the ground a place
`Travel.ObjectiveMin`-`Max` (60-320) yd away that it can walk to by a path at most 1.8 times the straight line, not in
water; in a `Flying` arena a place `FlyingMin`-`Max` (350-700) yd away. It arrives within 6 yd, on the ground. The
episode is terminal on arriving or death with no resurrection left.

**`FlagEncounter`** (`Opposition::Flag`, mirror seats). Warsong Gulch's rules between the two seats. The first seat's
base is where it starts; the other's is a place `Flag.BaseMin`-`Max` (100-180) yd away by path, where it is teleported.

- Touching (within `TouchDistance`, 4 yd) the other side's flag at its base or dropped takes it, and a carrier can't
  ride (it is dismounted, and every decision after). Touching one's own dropped flag returns it. Bringing the other's
  flag home while one's own is there captures it.
- A carrier who dies drops the flag where it fell. A dropped flag goes home on its own after `DroppedReturnMs` (10 s).
- The dead stand up at their base with full health after `RespawnMs` (15 s), like a graveyard wave.
- The seat's travel objective follows the flags: take the other's flag home, return one's own, chase the carrier of
  one's own flag, take the other's flag, pick it up where it lies.
- The episode is terminal at `CapturesToWin` (3).

Real battleground instances (Warsong Gulch's map, arenas with pillars) aren't used: their lifecycle (queues, a
premature end when a side is short, players teleported out at the end, one instance per match) doesn't fit an env
that keeps one instance for its lifetime.

## 4.6 Rewards

### The ledger

Each seat has a `RewardLedger`. An encounter adds `(term, value)` pairs, and the ledger sums the decision's total and
each term's episode total. Every term that any encounter of the stage pays becomes an episode info column
`reward_<term>`, so TensorBoard shows exactly what the stage pays for.

Terms: `damage_dealt`, `damage_taken`, `step_cost`, `casting`, `approach`, `stealth_opener`, `stealth_utility`,
`interrupt`, `kill`, `clear`, `health_kept`, `death`, `owner_damage_taken`, `owner_healing`, `tank_damage_refund`,
`threat`, `solo_fight`, `follow`, `owner_death`, `teammate_damage_taken`, `teammate_healing`, `teammate_threat`,
`teammate_death`, `revive`, `player_kill`, `progress`, `arrive`, `flag_capture`, `flag_pickup`, `flag_return`,
`carrier_kill`, `flag_lost`, `timeout`, `stall`, `spacing`, `readiness`, `control`, `self_healing`, `repeat`.

**Looking after itself and its friends (every stage).** `self_healing` pays `Support.SelfHealing` (0.5) times the
bot's effective healing on itself plus what its own absorbs soaked and its own damage-taken reductions prevented on
itself, as a fraction of its health. It is below every stage's damage taken weight, so a heal recovers part of what the
hit cost and being hit to heal it back never pays. On the owner and teammates, healers are paid `owner_healing` and
`teammate_healing` for healing and protection alike. Absorbs are tracked by polling the bot's own absorb auras on each
friend every decision (what they lost, or what was left when one vanished early); prevented damage is
`damage * (1 / multiplier - 1)` over the bot's own `MOD_DAMAGE_PERCENT_TAKEN` auras on the victim, at the hit
(`EnvPool::RecordPrevented`). In gauntlets, engaging a pull also pays `Support.BuffCoverage` (0.3) times the share of
the layout's buff groups up on the bot (averaged with the owner's where there is one).

**Goals** (`SeatGoal`, every stage whose policy has a goal head). The learner's goal head picks one of fight, control,
recover, protect, position or prepare every `mappo.goal_every_decisions` (16, so 4 s) and keeps it until the next
choice, and sends it to the sim with the actions (protocol 8: ACT carries the goals after the actions). The sim scores
whether each decision matched the goal -- damage for fight, an enemy other than the target held for control, healing or
resting itself for recover, healing or shielding the owner or a teammate for protect, its spec's range for position, a
buff, summon or stealth out of combat for prepare -- and pays `Goals.Match` (0.02) **once for each goal held**, on the
first decision that matches it. A goal is there to be reached, not to sit in: paid per decision, holding
`SeatGoal::Position` by standing at a spec's range earned +0.93 an episode in stage8_duel (2026-09-17), more than the
approach, casting and health terms together, and ranged seats learned to keep their distance for it. The charge is
small on purpose: it keeps the goals apart (nothing else stops a goal head collapsing into one goal), and the stage's
own terms still price the play. `goal_match_share` still counts every matching decision, paid or
not. A party's teammates see each other's goals in the party block. Columns:
`goal_<name>_share` per goal, `goal_match_share` and `goal_changes`; the learner's own metrics add `goal_<i>_share` and
`goal_kept_share` per update. The critic is goal-conditioned, so the advantage a decision gets is measured against what
that goal is worth.

Support columns (every stage): `healing_done` and `protection_done` (fractions of the bot's health), `overheal_share`,
`heals_on_full` (masked: 0), `defensive_casts`, `healing_casts`, `downranked_share`, `low_health_seconds` (any friend
below 35%); gauntlets add `buff_coverage` at engage.

### Scales

- **Decision scale.** Per-decision terms (step cost, threat, follow) are tuned for a 50 ms decision and multiplied by
  `DecisionMs / 50`, so they mean the same per second at any decision interval.
- **Damage scale.** `DamageScale(level) = 15 * e^(0.068 * level)` (about 16 at level 1, 230 at 40, 3500 at 80), so
  damage features and rewards have a similar size at every level. Pet, guardian and totem damage counts for the owner.
- **Health fractions.** Damage dealt is a fraction of the opponent's (or the pull's total) health, and damage taken a
  fraction of the seat's own maximum health.
- **Tier scale.** On a ladder -- the duel's tiers, the pack's and raid's rungs, the endurance run's pulls -- the
  outcome terms scale with the rung: a win (kill, clear, health kept) is multiplied by `1 + Difficulty.TierScale x
  tier` and a loss (death, timeout, overtime) divided by it. A tier-0 fight is unchanged; at tier 6 and the default
  0.25 a kill pays 2.5x and a death costs 0.4x. Evaluations spread their seeds over every tier while training climbs
  per class, so with flat terms the score fell as the ladder rose -- every rung-6 loss cost as much as a rung-0 one
  -- and convergence read the fall as done. Scaled, the break-even win rate falls with the tier, a hard fight is
  worth attempting, and the score is comparable across rungs. The tier is in the critic's state (4.8). Fixed-bonus
  opponents (evade, hide, stealth) are not a ladder and stay flat; the `difficulties` group of `eval.jsonl` is where
  the per-tier win rates are read.

### By stage (defaults)

**One-on-one** (duel, PvP, escort duel; `Duel.*`, `Casting.*`):

- per decision: damage dealt x2, damage taken x1, potential-based approach shaping toward the spec's range (melee
  3.5 yd, ranged 25 yd; 0.5 per 40 yd closed), step cost 0.0002
- stealth: +0.5 for a harmful spell cast from stealth that breaks it (Ambush, Garrote, Cheap Shot, Pounce, an attack
  out of Shadowmeld; it can't be repeated without earning stealth back), +0.05 for one that keeps it (Sap, Distract,
  Premeditation), once per target per stealth so it can't be farmed
- casting: -0.03 per second already spent on a cast-time spell that didn't finish, +0.03 per second of cast time for
  each one that finished in combat (channels pay through their ticks), and -0.05 for each cast the seat cut short
  itself (the stop-casting action, or moving out of its own cast), however little of it had run, so a start/stop loop
  costs more than an episode can earn. An enemy's interrupt costs only the seconds lost
- kill: +10, plus up to +1 for the share of the episode length left since the fight was engaged (the bot or its
  opponent entered combat), plus up to +0.5 for the share of health kept (damage taken is already charged as it happens, so a
  larger share would pay for surviving over winning). The approach, stealth and preparation before engaging
  cost only the discount
- death: -10 each time, including after a self-resurrection. With a self-resurrection available the seat has
  `Resurrection.GraceMs` to use it before the episode ends
- timeout (creature duel only): -10 when the episode's time runs out with neither side dead. The fight is lost, so the
  episode ends as a terminal outcome rather than a cut-off the critic bootstraps past; before it, never engaging was
  the cheapest way to lose
- stall (creature duel only): -0.08 per second the fight hasn't started once `Duel.StallGraceMs` (15 s) of the episode
  are gone. The timeout comes 900 decisions later, too far for the policy to tell standing still from closing in: at
  20M steps stage8_duel's deterministic policy stood where it spawned for the whole episode in 67 of 2048 evaluation
  fights. Preparing isn't stalling: the grace grows by the time the seat spent starting helpful spells out of combat
  (buffs, forms, stances, stealth, pet summons, conjuring; each its cast time, at least a 1.5 s global cooldown), up to
  `Duel.PreparationRefundMaxMs` (15 s), so a warlock summoning its demon or a druid shifting before the pull isn't
  charged for it and nothing has to start prepared. `preparation_seconds` in the episode info is that time, uncapped
- spacing (creature duel, ranged specs): -0.03 per second the opponent stands in melee range attacking the seat. The
  approach term only pays for closing in, so nothing kept a hunter, mage or warlock at its range
- repeats (every stage): -0.02 per press of the same action past the free ones in its window, and only when the
  press did nothing -- a spell that started casting, an item or a pet ability is never a repeat, because a caster's
  rotation is one nuke over and over. Orders to a pet already obeying, a target selected again and a stance pressed
  twice all still count (see Repeats, 4.3)
- winning outweighs winning fast: with the kill at 10, speed at most 1 and a loss at -10, a risky fast opener only pays
  more than a sure slow win above about 97% odds (at the earlier 3, 3 and -3 it was 79%)

**Pack** (`Pulls.*`): damage x2 of the pack's total health, damage taken x1, approach to the nearest enemy, +0.5 per
kill, +0.3 per interrupt, the stealth terms. Like the duel, a single pack is won or lost:

- interrupt +0.3 (`Interrupt`) times what the interrupt stopped: a heal 3x (`InterruptHeal`, it undoes damage already
  dealt), an area spell 2x (`InterruptArea`), a long cast 1.5x (`InterruptLong`), an ordinary cast 1x. The kind comes
  from the spell's own properties at the moment the cast dies. Never below 1: the flat term is how a class finds
  interrupting at all
- clear +10 (`PackClear`), up to +1 more for the share of the episode length left since a pack member entered combat
  (`FastClear`), up to +0.5 for the share of health kept (`PackHealthKept`)
- death -10 (`PackDeath`); timeout -10 (`Timeout`) when the 150 s run out with the pack and the seat both alive,
  ending the episode as a lost fight rather than a cut-off the critic bootstraps past
- overtime -0.1 per second (`Overtime`, in the timeout column) once a fight has gone on `OvertimeGraceMs` (60 s)
  since a pack member entered combat, and a death in overtime is charged the overtime left to the end of the
  episode. With the timeout alone, a -10 about 100 s away was worth about 2 to the discounted return against a whole
  -10 death now, and stage 2's warlocks learned to kite out the clock (28% timeouts at 10M steps). Dying never ends
  an overtime fight more cheaply than timing out
- stall -0.08 per second while no pack member has entered combat, once `StallGraceMs` (15 s) of the episode are gone,
  plus the preparation time as the duel's, up to `PreparationRefundMaxMs` (15 s). Counted per pull, not per episode:
  over a gauntlet, preparing once bought the full refund on every pull after it, and a seat that dropped combat to
  re-buff kept earning grace (stage 2's warlock went from 5.6 s of preparation a fight to 14.2 s, 19.4 s in the
  fights it lost)
- hazard -0.15 per second standing in a ground effect (`Hazards.Standing`) and -0.5 per fraction of maximum health
  taken from one (`Hazards.Damage`), together capped at `Hazards.Max` (3.0) an episode. The seconds are the term that
  teaches the behaviour: the damage arrives in ticks after the decision that caused it, and over two hours of stage 1
  it came to -0.003 an episode against a kill worth 10. The cap exists because melee have to stand in melee -- a
  hazard under the enemy is a real trade, and an uncapped charge teaches a seat to leave the fight
 Damage that could have been walked out of is worse
  than damage that could not, and this is the only term that pays a seat for moving its feet. It reads zero wherever
  nothing puts anything on the ground, which is most of the curriculum and none of a dungeon
- control +0.5 (`SinglePackControl`) times the damage prevented, in the seat's current health (floored at
  `ControlHealthFloor`, 20% of maximum), for every pack member other than the target that is held out of the fight
  -- each credited its own measured damage rate, or the pull's mean, or `ControlFallbackDps` for one that never got
  to act; up to `SinglePackControlMax` (1.0) a pull. Priced in the same currency as damage taken, at half its weight
  because the damage prevented is estimated rather than observed. The overtime grace also grows by the time an add
  was held, up to `ControlGraceMaxMs` (15 s), so holding one is not charged as dragging the fight out
- spacing -0.03 per second, for a ranged spec, while a living pack member attacks it in melee reach

With the gauntlet's clear and health kept (+2 and up to +2) and a -3 death, keeping health paid as much as clearing
the pack, and never engaging was the cheapest way to lose.

**Gauntlet**: the pack's per-step terms with damage taken x1.5. With an owner (stages 4, 5, 8), win-first as alone:
each cleared pull (`Pulls.Clear`) +2.5 and up to +0.5 for clearing within a minute of engaging it (not of its spawn, so
resting, sapping or stealthing in first is free), both x2 (`OwnerClearScale`), up to +0.5 for the seat's own health kept
during the pull; the seat's death -10 (`GauntletDeath`) and every owner death -15 (`Owner.Death`), so guarding the
owner comes before the seat's own health. At the earlier +2 +2 (x2) and +2 against deaths of -5 and -6, a pull cleared
was worth more than the owner's life. What alone teaches carries on beside the owner: readiness when a pull is engaged
(`OwnerReadiness`, 0.5), control (`OwnerControl`, 0.02 per enemy-second, up to `OwnerControlMax`, 1.5, a pull), seven
food and drink (`GauntletSupplies`), and a win: reaching the end with the owner never dead, no wipe and `OwnerWinPulls`
(5) pulls cleared counts as the kill, so `clean_kill` is the gauntlet won with the seat alive. **Alone** (stage 4) the gauntlet is won by lasting, and pays
win-first as the single pack does (`Pulls.SoloGauntlet*`): each cleared pull +5, up to +1 for clearing within a minute
of engaging it and up to +0.5 for health kept; a death -10, besides every pull it forfeits. Its pulls charge Stall
(-0.08 per second from `StallGraceMs` plus the preparation refund earned since the pull spawned, not while eating or
drinking) and Spacing as the single pack does. Engaging a pull pays readiness, `SoloGauntletReadiness` (0.5) times the
seat's health fraction the decision before (the lower of health and mana for mana users), so resting between pulls
pays when the next one starts rather than only through the death it avoids. Control pays `SoloGauntletControl` (0.02)
per second for each pack member of an engaged pull, other than the seat's target, that is stunned, incapacitated,
asleep, polymorphed, feared, or rooted out of melee reach and not casting, while another member is alive; up to
`SoloGauntletControlMax` (1.5) per pull. It stops when the control breaks, so controlling an add and then hitting it
pays nothing (stage 2's run used Sap, Blind, Polymorph, Hibernate and roots almost never). Reaching the end of the episode alive with
`SoloGauntletWinPulls` (5) pulls cleared counts as the kill, so `clean_kill` is a gauntlet endured; alive on fewer is a
timeout.

**Companion** (`Owner.*`, added to the gauntlet's, with kills and clears x2):

- everyone: owner damage taken (x1 for DPS, x2 for tanks and healers; a quarter of that when the owner is the tank);
  -0.01 per decision in combat while the owner isn't; +0.0005 per decision within 12 yd out of combat, -0.002 beyond
  25 yd; -15 per owner death; +1.5 when an ally the seat resurrected stands up
- tanks: +0.002 per enemy on the tank and -0.02 per enemy on the owner, per decision; half of the gauntlet's damage
  taken refunded
- everyone: effective healing on the owner x2, and what the seat's absorbs soaked and its damage reductions
  prevented there (overhealing earns nothing, because the heal hook reports health gained). Paying only healers left
  every other class at 0.000-0.005 of its healing going to the owner
- DPS and healers **beside a tank owner**: -0.004 per enemy attacking them, per decision. Beside an owner that does
  not tank, holding the enemies is the seat's job and is not charged: charged whatever the owner was, at 450 s an
  episode it came to -22.9 against +0.6 for healing the owner, the largest term in the stage

**Party** (`Party.*`, added per teammate): teammate damage taken (not for a tank teammate; x0.5 for DPS, x1 for tanks
and healers), healers' effective healing on teammates x2, tanks -0.02 per enemy on a non-tank teammate per decision,
-3 per teammate death. Kills and clears are shared. A tank isn't charged for fighting before the owner joins.

**Ambush**: +3 per ambusher killed, for every seat.

**Travel** (`Travel.*`): potential-based shaping on the distance left to the objective (+1 per 100 yd closed, taken
back for leaving), arriving +3 plus up to +3 for the share of the episode left, damage taken x1 (falls, what it rode
past), death -3, step cost 0.0002. Nothing pays for mounting: a mount is worth its cast time only on a long enough
trip, and the policy learns which.

**Flag match** (`Flag.*`, instead of the one-on-one terms): capture +5, the other side capturing the seat's flag -3,
taking the other's flag +1, returning its own +1, killing the carrier of its own flag +1.5, death -1, step cost 0.0002,
and potential-based shaping toward the seat's current objective (+0.5 per 100 yd), started over whenever the
objective changes, so a flag changing hands pays nothing by itself.

## 4.7 Scripted baselines

`Baselines::Choose` reads a seat's row through its layout, so the baselines follow layout changes automatically.

- **`greedy`**: the first allowed spell or trinket in catalog order. Every layout supports it.
- **`fight`** (layouts with the duel block), first match wins:
  1. with the travel block and an objective: dismount at it; far from it and not mounted, a flying mount where one
     flies, else a ground mount; on a flying mount climb to 20 yd, land at the objective; otherwise **steer** for it
     (and wait while moving, rather than cast something that would dismount). Steering weighs each bearing's aim at
     the objective against what the ground probe says lies that way (`Baselines::Steer`, `GROUND_OVER_AIM`), so a
     bearing onto ground the seat can cross beats one pointed straight into a cliff. It used to hold
     `BEARING_FORWARD`, which is why it arrived in 8% of its episodes against a trained policy's 99%, a yardstick
     anything cleared on the four movement stages. The bearing already being walked
     is left alone rather than swapped for the second best, which would set the seat zig-zagging whenever the
     objective sat between two bearings,
  2. with the gauntlet block and no target: eat when health is low, drink when mana is low,
  3. support: below 30% health, the first allowed defensive; the most hurt living friend below 60% (the bot itself
     without the support block) selected, then its first allowed heal (a healer cancels a form first if needed); a
     healer selects the owner or a tank teammate under attack without its HoT or shield and casts a kept-up heal,
  4. (the masks keep it from healing a friend at full health or re-casting what is still up),
  5. with the pet block and a pet class: out of combat with no living pet, call a stable beast or cast the best summon
     (Felguard, Voidwalker, Felhunter, Succubus, Imp; Raise Dead; Water Elemental); with a pet out, send it at the
     target,
  6. a spec of the ranged band (hunters, casters, healers) holds range: more than 28 yd from a living target, move
     to casting range (24 yd); with the target out of line of sight, move toward it; a hunter the target is hitting
     in melee reach while its pet attacks the target backs off 10 yd (its shots can't be used there) and lets the
     pet hold it; auto-attack only once the target is in melee reach (a hunter with no pet yet, or one still held),
  7. a melee spec starts auto-attack,
  8. and moves to a living target beyond melee reach while not already moving,
  9. while its pet attacks the target, the pet's first allowed damaging ability (pets don't autocast, and an Imp or a
     Water Elemental can't melee, so this is all they do),
  10. otherwise its rotation (not `greedy`), first match wins:
     - in no form, the spec's own: Moonkin Form (balance), Cat Form (feral cat), Dire Bear or Bear Form (feral bear),
       Shadowform (shadow). Other forms and stances are never cast; `SeatCharacter::PrepareFighter` puts a warrior in
       its stance,
     - the first allowed damaging spell in catalog order: school or weapon damage, a leech, or a melee or ranged
       weapon attack. A spell that only ticks is cast while its aura isn't on the target, and crowd control that
       damage breaks (confuse, fear, transform) never,
     - out of combat, a buff that isn't on the bot: an aura with no cooldown of its own, not speed, stealth,
       invisibility or feigning death, and at most one of each exclusive kind (a seal, a paladin aura, an armor, an
       aspect),
     - otherwise nothing.

     A cast resets the caster's swing timer (`Spell::cast`), and the first spell in catalog order is often a buff that
     can be cast again forever. `greedy` presses one every decision the pacing allows, so a paladin with a slow
     two-hander never lands a swing, and a caster holding range casts Lightning Shield or Inner Fire instead of ever
     starting the fight.

They are the reference numbers a trained policy has to beat (evaluation baseline) and a mechanics smoke test
(`forge run <stage> fight`).

## 4.8 The critic state

The centralised critic sees a class-agnostic global state of the env. `StateDim = 22 + 4 x 26 + 4 x 33 = 258`
(`STATE_GLOBAL_COUNT` is 14 plus one arena column each, the tier among the 14, then `MAX_SEATS` seat blocks and
`PACK_SLOTS` enemy blocks -- the enum in `StageScenario.h` is the source).

| Part | Features |
|---|---|
| Global (21) | Episode time fraction; pull active; pulls cleared / 10; time to next pull / 20 s; elite pull; linked pull; owner present, alive, health, mana, x, y (relative to the spawn point, / 40), in combat; arena one-hot (8) |
| Per seat (4 x 26) | Present, alive, health, mana, other power, level / 80, the six-number aptitude brief, class one-hot (10), in combat, casting, x, y |
| Per enemy slot (4 x 25) | Present, alive, health, x, y, casting, elite, level difference / 5, in combat, victim is the owner, victim is seat s (4), max health against seat 0's, damage multiplier, armor reduction against seat 0, run speed, creature type one-hot (7) |

The episode time *fraction* (the share of the episode's own limit spent) appears only in the critic state, because live
play has no time limit. Observations carry elapsed episode time instead (the core block's last global feature). In
self-play, each seat's opponent is the other seat and already appears in the seat part.

## 4.9 Tuning

Every value that shapes the curriculum is a config key `<TuningPrefix><Group>.<Name>`: `AnimusForge.Curriculum.*` in
the forge and `Animus.Curriculum.*` in mod-animus. `CurriculumTuning::Visit` lists them once, for loading and for
writing. Min/max pairs are put in order on load.

| Group | Controls |
|---|---|
| `Characters.*` | High-level threshold and chance, how talent points are spent, how often a pet class starts with its pet out |
| `Party.*` | Size weights, classic makeup chance, role chances, teammate reward weights |
| `Duel.*` | One-on-one reward weights and preferred ranges |
| `Casting.*` | Cast time wasted and completed, the charge per self-inflicted cancel |
| `Actions.*` | Pacing: how soon the same action, the same movement order, a stop of a new cast and a recast of a stopped spell are allowed again |
| `Pulls.*` | Linked, elite and higher-level chances, pull timing, owner engage timing, recovery fraction, pull reward weights |
| `Owner.*` | Level spread, role chances, owner reward weights, follow distances |
| `Resurrection.*` | Grace period, revive reward |
| `Opponent.*` | Level spread, engage time, role chances |
| `Ambush.*` | Arrival window, engage time, kill reward |
| `ScriptedPlayers.*` | Spell and heal intervals, wandering, regeneration, heal thresholds, ranges; PvP stealth and tactics chances, crowd control interval, defensive and break thresholds |
| `Travel.*` | Objective distances on the ground and in the air, travel reward weights |
| `Flag.*` | Base distance, captures to win, respawn and dropped-flag timers, touch distance, flag match reward weights |
| `Arena.<stage>.<arena>.Weight` | Arena weights (read by `StageScenario`, not `Visit`) |
| `Arena.<stage>.<arena>.MaxRung` | The arena's pinned pack rung, `-1` for none (read by `StageScenario`, not `Visit`) |

The effective values are written into `stage.json` under `tuning` and copied into each run directory. To watch a stage
in mod-animus exactly as a model trained on it, copy that run's `tuning` into `Animus.Curriculum.*`.
`animus-forge/conf/mod_animus_forge.conf.dist` documents every key.

## 4.10 Episode info

Every stage reports these **core columns** per seat:

- `damage`, `dps`, `white_damage`, `special_damage`
- `level`, `race`, `spec`, `class`, `role`, `talent_plan` (0 standard, 1 noisy, 2 random), `unspent_talent_points`,
  `equipped_items`
- `spell_casts`, `trinket_uses`
- `present` (0 for an empty party seat; ignore that row), `arena` (index into `stage.json` arenas), `opponent_seat`
- `spawn_point` (which spawn point the episode was built from) and `spawn_drawn` (which one it drew first),
  indices into the stage's or the arena's `SpawnPoints`, or into `HeldOutSpawnPoints` while evaluating.
  Equal, the first choice worked; different, that point could not build an episode and the reset moved on.
  A point drawn often and built from never is ground no episode can start on -- held-out ground like that
  is counted as control and scores nothing, which is how stage2_indoor came to be gated on two of its
  three rooms without anything saying so.
- `killed`, `died`, `time_to_kill`, `damage_taken`, `health_left`, `stealth_openers`, `stealth_utility_casts`,
  `pet_summoned`, `pet_at_start`, `pet_damage_share` (of the seat's damage, what its pets and guardians dealt),
  `pet_died`, `pet_abilities` (pet bar abilities started), `pet_orders` (stances, follow, stay, sending the pet in),
  `item_uses` (use effects of the main-hand or off-hand item),
  `opponent` (creature entry)
- what the seat did with its pet: `pet_attack_orders`, `pet_passive_orders`, `pet_defensive_orders`,
  `pet_aggressive_orders`, `pet_follow_orders`, `pet_stay_orders` (each order given), `pet_out_seconds`, and the share
  of that time the pet was attacking something (`pet_attacking_share`), set passive (`pet_passive_share`) or told to
  stay (`pet_staying_share`). A pet's abilities are the policy's to cast: its spells are learned with autocast off
- `casts_completed`, `casts_cancelled`, `cast_seconds_wasted`, `cancelled_stopped`, `cancelled_moved`,
  `cancelled_target`, `cancelled_other`
- `consumables_used`, `self_resurrections`
- how a fight ended, to tell the ways of losing apart: `timed_out` (creature duel: time ran out with neither side
  dead), `engaged`, `engage_time`, `target_health_left`, `distance_at_end`, `form_at_end` (the `ShapeshiftForm`),
  `power_left` (of the primary power), `target_evade_seconds` and `out_of_sight_seconds` (creature duel: the opponent
  evading, and engaged without line of sight to it), `target_unreachable_seconds` and `target_teleports` (creature duel:
  the opponent without a path to its victim, and put beside it for that), `actions_per_minute` (actions other than the
  no-op), `repeated_presses` (presses charged by `Actions.Repeat`)
- how the seat fights, to grade a spec's playstyle (they reward nothing):
  - `melee_damage_share`, `shot_damage_share`, `spell_damage_share`: the seat's own damage by the game's damage class
    (`SpellInfo::DmgClass`), as shares of all its damage, so with `pet_damage_share` they add up to 1. Melee is melee
    swings and melee abilities (Raptor Strike, Sinister Strike), shots are ranged weapon attacks (Auto Shot, Steady
    Shot, a wand) and spells are the rest, DoTs included. The damage hook doesn't say which spell dealt a hit, so the
    library notes the spell in `ModifySpellDamageTaken` and `ModifyPeriodicDamageAurasTick`, which run just before it
    for the same attacker and victim. Spell damage it can't match counts as a spell
  - `in_melee_share`, `target_on_pet_share` (one-on-one arenas): the share of the fight, engaged with the seat alive,
    it spent within melee reach of the opponent, and the share the opponent spent attacking its pet or guardian. A
    hunter's shots can't be used inside melee reach (`SPELL_FAILED_TOO_CLOSE`), so for a hunter `in_melee_share` is
    the share of the fight it played melee. For a caster it is mostly where the opponent chose to fight
  - `target_rooted_share`, `target_snared_share`, `roots_applied`, `snares_applied` (one-on-one arenas): the share of
    the same time the opponent spent rooted (Frost Nova, Entangling Roots) or slowed (Concussive Shot, Wing Clip,
    Frost Shock, Earthbind) by the seat, its pet or its totems, and how often one went on where there was none
  - `feign_deaths`, `feign_death_resets` (one-on-one arenas): how often the seat feigned death, and how often its
    opponent then evaded home at full health (within 3 s of the feign ending) because nothing else held it. With a pet
    on the opponent, feign death hands the fight to the pet; without one it throws the fight away

Encounters then add their own columns:

- pulls: `kills`, `interrupts`, `pack_size`, `linked`, `pulls_cleared`, `food_used`, `drink_used`, `sustain_casts`,
  `deaths`, `wipes`; gauntlets also `engage_health`, `engage_mana`, `pulls_started_low`, `pulls_arrived`,
  `rest_seconds`, `eat_failed`, `drink_failed`, `meals_cut_short`, `control_seconds`
- owner: `owner_class`, `owner_died`, `owner_deaths`, `owner_damage_taken`, `owner_healing`,
  `owner_healing_aptitude`, `owner_mitigation`, `owner_heal_share`, `threat_on_bot`, `threat_on_owner`, `revives`
- party: `seat`, `teammates_died`, `teammate_damage_taken`, `teammate_healing`, `threat_on_teammates`
- opponent: `won`, `opponent`, `opponent_class`, `opponent_seat`, `opponent_elite`, `opponent_healing`,
  `opponent_mitigation`
- ambush: `ambushers`, `ambushers_killed`
- travel: `arrived`, `travel_seconds`, `start_distance`, `walk_distance`, `distance_travelled`, `dry_distance`,
  `dry_detour`, `route_length`, `route_complete`, `route_failed`, `route_shortcut`, `dry_shortcut`,
  `objective_distance_at_end`, `objective_distance_nearest`, `nearest_at_seconds`, `trip_share`, `crossing`,
  `swim_seconds`, `stall_seconds`, `stalls`, `detour_band`, `air_only`, `mounted_fraction`, `flying_fraction`;
  flight adds `flew`, `flight_speed`, `flight_yps`,
  `flight_yps_peak`, `flight_height`, `flying_flag_share`, `flying_mount_fraction`, `knows_flying_mount`,
  `could_mount_flying`, `saved`, `saved_if_flew`, `saved_if_ground`
- flag: `flag_captures`, `flag_pickups`, `flag_returns`, `carrier_kills`, `flag_deaths`, `match_won`,
  `team_seat`, `flag_in_reach`

The authoritative list is the `table.Add("...")` registrations themselves, under
`src/Scenario/Curriculum/`; `tests/test_gates.py`'s `sim_episode_info()` extracts exactly those, which is why a
config that gates on a column the sim never emits fails the test suite rather than five hours into a queue.

Then come the `reward_<term>` columns. Columns of encounters an episode's arena doesn't use read 0. The exact list for a
stage is `episode_info` in its `stage.json`.

## 4.11 Stage by stage

In the order `forge start` trains them, which is their number. Each seeds from the stage above it in the tree (4. head).

### Stage 1: `stage1_move`

The first travel stage, and the one that comes before mounts exist. A place 40-160 yd away in Kalimdor, 120 s,
and **mounting is masked** (`ArenaDefinition::OnFoot`) -- masked rather than merely unpaid, because a masked
action cannot be explored into and the lesson stays clean. What is left is what a player does before it can
ride: the speed cooldowns the class has (Sprint, Dash, Travel Form, Aspect of the Cheetah), not stopping, and
not wandering off the path.

`mounted_fraction` is the check that the mask holds: it must read 0.0000. Blocks: core, duel, pet, travel --
the pack block is dropped, so the travel line trains straight off the duel.

### Stage 2: `stage2_indoor`

Inside. A place 8-40 yd away in an inn, shorter than an outdoor episode's first step, in 90-second episodes:
where the sixteen navmesh rays, the 15-degree turn, the clearance term and the jump are all worth something.
Every inn on Kalimdor that `areatrigger_tavern` names is a spawn point, each stood on with `forge rays` before it
was written down. `arrived`, `travel_seconds` and `reward_clearance` are the columns to read; the scripted
baseline already arrives every time here, so the trip is the measure, not the arrival.

### Stage 3: `stage3_jump`

Down. A place 20-120 yd away below a ledge, 5-80 yd under the seat, with a way round on foot at least twice the
straight line, for 120 s. The jump drops off the edge and the fall after it is the core's own, with the core's
own damage: free to fourteen yards, lethal past about seventy. The seat is told how far down the landing is
(`OBS_JUMP_DROP`) and nothing about what that costs. Slow Fall and Levitate are open, so a mage or priest learns
when a cast is worth spending to make the deadly drop free; `drops`, `fell`, `fall_deaths` and `feather_falls` say
what each class chose.

### Stage 4: `stage4_dive`

Down, into the water. Two arenas on Stonebull Lake (Lake Elune'ara held out). `depths` (two thirds): a place
20-120 yd away on the lakebed under 6-40 yd of water, 150 s, arriving means standing on it. `chain` (a third): a
chain of lakebeds thirty to sixty yards apart for 240 s, longer than the core's three-minute breath, so a seat
that stays down for the whole chain drowns before the clock and one that surfaces between legs does not.
Unending Breath and Water Breathing are open. `dive`, `breaths`, `breathing_casts`, `checkpoints`, `chain_broken`
and `drowned` are the columns to read.

### Stage 5: `stage5_dodge`

**Drill, and there is nothing to fight in it.** Fire lands under each seat every 2.5 s and burns for as long as
its spell lasts. No creature is spawned, nothing is targetable, and the episode runs its full 120 s. The lesson is
one thing -- get off it -- and it is the only thing in the episode. It adds the support block for the hazard
charge.

Why nothing to fight. The first version was the pack stage's pack with a hazard caster forced into every pull, and its
numbers could not be read: hazard seconds rose over 7M steps while the pack rung rose underneath them, and nothing
in the run could say whether the policy was failing to step out or simply meeting more fire. Pinning the ladder
(`MaxRung`, 4.1) fixes half of that; removing the pack fixes the rest.

Why the fire comes from an invisible caster rather than from nothing. The hazard machinery reads two different
things. `Encoding::FindNearestHazard`, which the *observation* uses, accepts a `DynamicObject` from a hostile
caster **or** a `GAMEOBJECT_TYPE_TRAP` from nobody at all. `Encoding::StandingInHazards`, which the *charge* uses,
counts `DYNOBJ_AURA_TYPE` auras and nothing else. A trap gameobject therefore burns a seat while
`hazard_seconds`, `hazard_damage` and `reward_hazard` all read zero -- damage with no signal, which is worse than
no drill. So `HazardEncounter` summons a World Invisible Trigger, hostile and immune and unselectable, and has it
cast one of the world's own persistent area auras (`OpponentPool::RandomHazardSpell`, the same spells that make a
creature a hazard caster). There is nothing to fight, and every sensing and reward path works untouched.

Why the fire lands underfoot. Every reward in an episode with no enemy is a penalty: `RewardTerm::Hazard` is
charged and never paid. Fire in fixed places would teach a policy to stand in a clear corner and do nothing --
the behaviour the hazard cap exists to prevent in the stages that *do* have a fight. Fire that lands where the
seat is standing removes that option, so the penalty alone is enough and the drill needs no objective of its own.

What to read: `hazard_seconds` and `hazard_damage`, both of which should fall, and `hazard_patches` for what was
laid. The scripted `fight` baseline, which has nothing to fight and mostly stands still, spends about 36 s an
episode in fire and ends at 58% health; that is the number to beat.

It is on the trunk: `stage10_gauntlet` seeds from it, so the lesson carries into every PvE stage after it. The
hazard charge lands about four times harder on a tank than on a ranged seat, because a tank cannot walk out of
what it is holding an enemy in -- read the per-role columns before the overall one.

### Stage 6: `stage6_travel`

Getting somewhere, off the duel. A character of level 20 or more, with its level's riding and its side's mounts, starts
in Old Hillsbrad (which allows mounts) with a place 60-320 yd away by path. A mount's cast time only pays on a long
trip, and arriving on foot is what lets it fight at the end. Blocks: core, duel, pet, travel. 150 s episodes. Config:
budget 20M, a travel report.

### Stage 7: `stage7_flight`

Flying, off travel. Characters of level 60 or more (Expert Riding, and Artisan with a fast flying mount from 70) start
in Outland's Nagrand, where flying mounts fly, at one of eight spawn points, each env in its own phase. The place is
350-700 yd away: flying is several times faster and passes over everything, but dismounting in the air falls with a
player's fall damage, so the policy learns to take off, keep a height, land and dismount. Battlegrounds never allow
flying mounts (the zone must be Outland or Northrend, `SpellInfo::CheckLocation`), so this is for the open world.
180 s episodes. Config: gamma 0.999 and lambda 0.99, budget 20M.

Two arenas. `flight` (weight 2) places its objective anywhere the height probe finds dry ground, which in Nagrand
is nearly always walkable -- 700 yd at run speed is 100 s of the clock, so a ground ride arrived often enough that
nine of ten class heads never found the flying mount. `flight_air` (weight 1, `ArenaDefinition::AirOnly`) is where
the wings are the way: the objective can only be reached by air (no complete ground route within
`Travel.AirDetour` of the straight line), the ground mount is masked, and arriving is measured at the objective's
own height (`Travel.AirArriveRise`) so the cliff foot under a plateau's edge does not count. A spawn point with no
such place within reach builds an ordinary flight instead and reports `air_only` 0, as the water arena reports
`crossing` 0 when it finds no crossing: the column says what the ground offered, not what the arena asked, and a
spawn point that never offers one shows up there rather than as an env that cannot build an episode. `flew` per
class is the column to read beside `arrived`.

### Stage 8: `stage8_duel`

A new character against a real creature (4.5), with the class's whole kit, in 90-second episodes (a timeout is a
lost fight, and a healer against a creature with twice the usual health needs the time). Nothing seeds it, so its
networks start from scratch. Hunters are
offered four beasts each episode through `call_beast` actions, because Call Pet needs a pet saved in the database. The
observation shows each beast's family and pet type, so the policy can learn its preference. Warlock demons, Raise
Dead, Water Elemental and Feral Spirit are ordinary spell actions with their reagents in the bags. A warlock carries 5
Soul Shards: they don't stack, and the 20 it once had filled the 16-slot backpack, so no potion, bandage, healthstone
or soulstone fit and the duel's warlocks never used one. The bot gains no XP.

Pets are played as a player has them:

- **A summon the core does not call a pet is still seen.** `Unit::GetGuardianPet` only returns what is registered as
  the owner's pet, so a ghoul raised without Master of Ghouls, Army of the Dead, an Infernal or Feral Spirits used to
  leave the whole pet block reading zeros while the thing fought: stage8_duel's death knight tanks raised a ghoul in
  90% of their fights, took a tenth of their damage from it and never saw one. `PetBlock::FindPet` falls back to the
  first creature the seat controls, and `PetBlock::OBS_COMMANDABLE` says whether it takes orders -- a guardian has no
  action bar of the owner's, so every pet action of it stays masked, exactly as a player's would be.
- **Six ability slots.** A hunter beast with its talents spent carries more than four castable abilities (a focus
  dump, its family's special, a taunt, a sprint, and what the talents added), and the slots keep the best kinds
  first, so at four a ferocity pet's Rabid or Call of the Wild was never offered.
- **A hunter's beast arrives as a player's does:** at the hunter's level, fed to full happiness (an unhappy beast
  deals 75% damage), with its level-up spells learned and its talent points spent along a standard build for its
  tree (`PetTalents::Spend`).
- **A new pet starts defensive.** Creating a pet's `CharmInfo` sets it passive, and a player's summon then loads the
  stance saved with the pet, which a bot never has, so every pet stayed passive and only fought what it was sent at.
  `PetBlock::DefaultStance` sets a newly seen pet that came out passive to defensive, once per pet, so a stance the
  policy picks afterwards stands. Companions do the same.
- **A warlock's demon isn't stunned by the masks.** A strict `Spell::CheckCast` of a demon summon casts Summoning
  Disorientation (32752) on the warlock's current pet, meant for a summon the player starts. The action masks check
  every summon spell every decision, so stage8_duel's demons were stunned for nearly the whole of every fight,
  ignored every attack order and dealt no damage. `SpellChecks::CheckCast` checks those summons loosely, and does
  the global cooldown and shapeshift checks the strict pass would have made itself.
- **A called beast is fed and talented.** It arrives happy (a freshly tamed beast is unhappy and deals 75% damage) and
  its talent points are spent (`PetTalents`): the build players took for its tree -- ferocity, tenacity or cunning --
  point by point through `Player::LearnPetTalent`, the rest at random. Family-specific talents (Dash, Dive, Charge,
  Swoop, Mobility) are left out, since the core cannot tell which families may take them.
- **A dead pet can be brought back.** Revive Pet is a hunter action, and `call_beast` is allowed over a dead pet (the
  corpse is dismissed first).
- **Half the pet classes arrive with their pet out** (`Characters.PetOutChance`): a hunter one of its offered beasts,
  a warlock a random demon it knows, a death knight with Master of Ghouls its ghoul, a frost mage with Glyph of Eternal
  Water its elemental, summoned without a cast and with the summon ready again. The rest summon it themselves, so the
  policy learns both to get a pet out and to use (or replace) the one it has.
- **Follow and stay are hidden while fighting** (the seat or its pet in combat): they call the pet off its target, and
  stage8_duel's warlocks cycled attack, follow and stay eight times a fight while their pets never landed a hit. They
  are there out of combat, to position a pet before a pull.
- **The `fight` baseline uses pets:** out of combat it calls a stable beast or casts its best summon (Felguard,
  Voidwalker, Felhunter, Succubus, Imp; Raise Dead; Water Elemental) when no living pet is out, and sends the pet at
  the target, so the per (class, role) gates of pet classes compare with a character that plays its pet.

Learner (`configs/stage8_duel.yaml`, the root every other config extends):

- **Networks:** hidden `[256, 512, 512]`: a 256-wide adapter per class and a two-layer 512-wide shared trunk,
  which puts the capacity where every class trains it. Every stage keeps these sizes, or the trunk can't be
  copied.
- **PPO:** gamma 0.997 and lambda 0.985 per 100 ms of game time (`reference_decision_ms`, compounded to
  `AnimusForge.DecisionMs` so horizons stay the same in seconds: a ~33 s horizon and a ~5.5 s GAE credit trace, printed
  at start), clip 0.2, entropy 0.01 with an entropy floor at 30% of `ln(legal actions)` (boosted up to 4x), learning
  rates 3e-4, 4 epochs stopped early past approx KL 0.03 (`target_kl` 0.02 x the 1.5 tolerance), 8 minibatches,
  rollout 128 with updates run serially, value
  normaliser beta 0.99, advantages normalised per class. Budget 100M env steps, a ceiling.
- **Evaluation:** every 10M steps and at the start, 2048 seeded episodes against `fight`. Training episodes lean
  toward the class and role pairs furthest behind the baseline and short of clean kills (`layout_sampling`, by score
  gap and `clean_kill`, at most 4x), and a class that has converged is held at 2% of its draw.
- **Convergence:** the rule at the head of the chapter, per class over a window of 4 evaluations: score plateau
  (2% and 0.01 improvement, 2 standard errors), LR-normalised KL under 0.003, entropy settled above the 30% floor,
  ladder rung settled. The learning rates hold at full until the overall score has gone 3 evaluations without a
  new best, then anneal.
- **What to read:** `clean_kill` per class and per tier (`difficulties`), `livelocked`, and the per-tier win rates
  against `fight`. None of it is a gate.

### Stage 9: `stage9_pack`

Adds the pack block: target slots and the enemy-slot observation (the tactical spells come with the core from stage 1). Linked packs mean pulling one
enemy pulls all of them. The interrupt reward teaches casting interrupts at the right moment. 150 s episodes: a pack
is up to four of the duel's creatures, which took the duel's policy about 17 s each. Rewards are the duel's win-first
ones (4.6).

Config: rollout 256, gamma 0.999 and lambda 0.99 (~100 s horizon), budget 40M. What to read: clean wins overall and
per class and build on rungs 0-2 (the summary's `up_to` group) -- the 2-4 creature packs of the first run, which had no
ladder and reached 90% overall at 20M steps -- and how the caster and elite rungs above fare in `difficulties`.

### Stage 10: `stage10_gauntlet`

Adds the gauntlet block: sustained combat, recovery between pulls with food, drink and the core's sustain spells.
Between pulls there is no target, so target features are 0 and only self-cast actions are allowed. The gauntlet arena
runs 450 s, paced so that eight or more pulls fit (pulls come to the seat, sooner as it clears them), and a solo
gauntlet is won by lasting to the end with five pulls cleared (rewards above). Its episode info adds the recovery
columns: `engage_health` and `engage_mana` (means over the pulls engaged, taken the decision before), `pulls_started_low`
(below half health or 30% mana), `pulls_arrived` (came to the seat unengaged), `rest_seconds`, `eat_failed`,
`drink_failed`, `meals_cut_short` (food or drink that ended early with health or mana still to restore) and
`control_seconds` (enemy-seconds kept out of the fight, as the control reward counts them).
Config (extends stage 9's): gamma 0.999 and lambda 0.99 (~100 s horizon, ~9 s credit trace, so resting before a pull or
stealthing in is tied to the clear it pays for), rollout 256, budget 60M, evaluations every 10M. What to read:
gauntlets won, `pulls_cleared`, `engage_health` and `livelocked`. The first run (300 s, pulls that waited, a win by
merely lasting) reached 63-66% survived with 12% of its wins on at most one pull cleared, rogues and healers avoiding
the pulls.

### Stage 11: `stage11_endurance`

A planned run: eight pulls in a fixed order, the same every episode, seeded from stage 4 and using its blocks. The
order is an opener of two, three, four with two casters, a small one, four, three with an elite, four with an elite a
level up, and last an elite pack two levels up. Nothing about the fights is new -- the gauntlet taught them -- so what is
left is the plan: what to spend on the opener, what to keep for the last pull, and whether the small fourth pull is
used as a rest. It is won by clearing the last pull alive; the 900 s clock running out is a loss however far it got,
and `pulls_cleared` (out of 8) is how far. `PullSchedule::Sequence` builds it; `eval.trace_episodes: 4` records four
whole runs decision by decision, which is how a plan is read.

It is on the trunk: `stage16_companion` seeds from it, so the plan it learns is carried into the companion and
party stages rather than being a dead end beside them.

### Stage 12: `stage12_pvp`

The PvP branch. It extends the endurance run and keeps only core, move, duel and pet, adding pvp; the pack,
gauntlet, companion and party blocks are not in its layouts, so the PvP line trains straight off the PvE one.
Self-play: two learned seats of random classes and builds at one level, both played by the policy, so every fight
is training data for both sides. The far side is the live policy or a frozen earlier checkpoint from the learner's
cast league, never a script. A policy's score against itself does not track progress, so evaluation uses
`eval.opponent_baseline`: the `fight` baseline plays seat 2, the score is seat 1 against it, and the baseline score
is `fight` against `fight` on the same seeds. Config: gamma 0.999 and lambda 0.995, as a fight turns on what
happened tens of seconds before (a stealthy approach, a trinket baited out). Budget 60M.

### Stage 13: `stage13_evade`

**Drill.** A scripted enemy player **ten levels above** the seat
(`ArenaDefinition::OpponentLevelBonus`), for 120 s. The fight is not winnable straight, and that is the point:
everything up to here rewards winning the fight in front of it, so a losing fight is a class of situation the
policy has never been paid to handle and it dies with its cooldowns up. The score is being alive when the clock
runs out (`survived`).

**Time spent unseen is counted and never paid.** The optimal policy for paid seconds out of sight is to walk to
the far corner at t=0 and stand there, which is exactly the farmable shape `animus.rewards` exists to catch.
What is paid is `RewardTerm::BrokeContact`: **once per seen→unseen transition, with a cooldown**
(`Evade.BreakCooldownMs`, 5 s), so strobing around a pillar earns nothing. A break counts as a *line-of-sight*
break when it was achieved without stealth -- the effect, not the button press.

Columns: `survived`, `escaped` (a single unbroken stretch out of sight of at least `Evade.EscapeMs`, 8 s),
`contact_breaks`, `line_of_sight_breaks`, `unseen_seconds`, `unseen_longest_seconds`, `re_stealths`.

Measured `fight` baseline on this arena, 2048 episodes: `survived` 0.405, `contact_breaks` 0.514,
`unseen_seconds` 3.72, `escaped` 0.094, `won` 0.290. `fight` never tries to hide, so those breaks are incidental
terrain occlusion during a chase -- they are the floor a policy that learned nothing already clears, and the
gates sit well above them (`survived` 0.55, `contact_breaks` 0.90, `unseen_seconds` 6.0).

The level bonus is ten because six stopped being a losing fight once the spawn had cover: terrain blocks the
scripted opponent's casting as readily as it hides the seat, and the baseline's `won` went 0.188 → 0.447 on the
same change. At ten it is back to 0.290 -- still beatable about three times in ten, which is deliberate. The
lesson is recognising a losing fight and leaving it, not obeying a rule that says every fight here is lost.

The `fight` baseline is a poor yardstick here on purpose: it is a policy trained to win fights that are not winnable,
so surviving is a new axis rather than a better version of the old one, and the columns above are read against the
baseline's measured values rather than its score. `stage14_hide` seeds from this stage, so every class carries the
lesson on.

**Two things had to be true before any of this could work, and neither was.**

`Unit::CanSeeOrDetect` does not raycast -- it is grid visibility plus stealth and invisibility detection, and it
stays true through a wall. Every "can this see that" in the curriculum used it, so the drill's first run read
0.0000 for `unseen_seconds` and `contact_breaks` across 2048 episodes, and the scripted hunter could never lose
a quarry that was not stealthed, which meant its `Search` behaviour had essentially never run. `Encoding::CanSee`
now answers that question -- detect, then `IsWithinLOSInMap` -- and the hiding tracker, the scripted hunter and
the director's `SideCanSee` all go through it. (The per-seat observation filters deliberately still use the bare
check: those run for every seat in every stage, and changing what a seat observes is a different change.)

And the arena has to have something to hide behind. With line of sight working but the default open-field spawn,
twelve of the eighteen classes still read exactly 0.000 breaks -- only the three that can stealth registered
anything, because on flat ground a warrior cannot break line of sight at all. Both drills now spawn inside
Durnholde Keep and among the Southshore farms, on the same instance map, at exact ground coordinates taken from
the world database.

`ScriptedPlayer::Search` needs no give-up timer: it mills within `SEARCH_RADIUS` (10 yd) of the last sighting,
which is smaller than the outer cover rings `FindCover` uses (8, 16 and 26 yd), so breaking line of sight at 16
or 26 yd genuinely escapes.

### Stage 14: `stage14_hide`

**Drill.** The same losing fight six levels up rather than ten, for **every class and every race**. The lesson
is becoming unseen and staying unseen, and hiding again once the hunter has found you.

Stealth is one way to do that and the rarest: four of the eighteen classes have a stealth aura in their own
kit -- `rogue_dps` (Stealth) and the three druids (Prowl). It is not the lesson. Every class can get out of
sight with terrain, with distance, and with whatever its kit and its race give it -- Blink, Disengage, Feign
Death, Invisibility, Ice Block, Sprint, and Shadowmeld for any night elf of any class. So the stage grades the
**outcome**, not which button produced it.

Six levels rather than stage 13's ten, so the fight is winnable often enough that hiding is a choice rather
than the only move left. That is the whole difference between the two: stage 13 is about leaving a fight that
is lost, this one is about not being found once you have.

Columns that carry the gate:

| Column | What it says |
|---|---|
| `escaped` | One unbroken stretch out of sight of at least `Evade.EscapeMs` (8 s) -- the hunter lost the seat rather than blinked |
| `re_hides` | Contact broken again after the first time: getting back out of sight once something is already looking for you, which is the harder half and the one every class can do |
| `survived` | A sanity floor, not the thing being asked for |

Measured `fight` baseline, 2048 episodes over all eighteen layouts: `escaped` 0.079, `re_hides` 0.240,
`contact_breaks` 0.541, `unseen_seconds` 3.17, `survived` 0.551, `won` 0.450, `re_stealths` exactly 0.0000.
**Every one of the eighteen produced both gate metrics**, which is the check that mattered: no layout is asked
for something it has no way to do. The spread runs from `warlock_dps` (survived 0.257) to `deathknight_tank`
(0.781), and the layout floor is set against the bottom of it.

`re_stealths` and `stealth_openers` are **reported and never gated**. Eight of the ten classes have no stealth
button, and a gate on one would ask them for something they cannot do; the columns are still worth reading,
because they are what a rogue, a druid or any night elf actually presses.

> **An earlier version of this stage was restricted to the classes that could stealth, and that was wrong
> twice over.** It excluded fourteen classes from a lesson all of them need. And the test it used -- does
> the action catalog contain a stealth aura -- returned eleven of the eighteen, because the catalog is the
> union over every race a class may be and that union holds Shadowmeld (58984), the night elf racial. A warrior
> that rolled a human would have played a stealth stage with no stealth at all. `StageDefinition::NeedsStealth`
> and its validation are gone; nothing in the curriculum restricts a stage to a subset of classes.

Racials stay fully available everywhere, here and in every other stage: the action catalog carries Shadowmeld,
Will of the Forsaken, Blood Fury, Escape Artist and the rest, and `Encoding::IsSpellActionAllowed` masks each
by `HasActiveSpell`, so the race that actually rolled is the one whose racials are offered.

### Stage 15: `stage15_stealth`

**Drill, and a leaf.** The one stage in the curriculum restricted to a subset of classes, and the reason
the restriction is worth its cost.

Hiding and stealth are different lessons. Stage 17 is *not being found*: every class can do it, with terrain,
with distance, and with whatever its kit and race give it -- Shadowmeld included. This stage is being **close**
and not found: crossing the ground to someone who is looking for you, arriving inside strike range with the
opener still in hand, and holding there. Shadowmeld cannot do that at all, because it breaks the moment you
move. Only a real stealth aura can, so only the four classes whose own kit carries one play it:
`rogue_dps` (Stealth) and the three druids (Prowl). `StageDefinition::NeedsStealth` asks `ClassKit`, the class
trainers' list, so the answer is true of every member of the class rather than of one race of it.

The opponent is six levels up, as on the hide stage: the fight has to be one the opener decides, or getting
into position is a flourish before a fight that was winnable anyway.

**`RewardTerm::Stalk` is the only reward in the curriculum paid per decision rather than on a transition**, and
that is deliberate rather than an oversight. What made the order nudge farmable -- 5.01 an episode, 23.7% of
gross, cut to a fifth of a percent -- was that it paid for a state that was *free to hold*: a focus that never
changed still paid every decision. This pays only while the seat is stealthed, unseen, and within
`Stealth.StalkYards` (10 yd) of a **living** opponent that is actively looking, which is the opposite of free:
detection is a distance check the seat is losing the whole time it stands there. `Stealth.StalkMax` caps the
episode's total at 1.0 regardless, so the opener it sets up stays the larger prize and loitering cannot change
the sum.

Time unseen is still never paid. The distance condition is the entire difference between this and the farmable
shape: staying stealthed inside melee range of something hunting you is a skill, and staying unseen in the far
corner of the map is the absence of one.

| Column | What it says |
|---|---|
| `stalked_into_range` | Got inside the band at all, stealthed and unseen -- the gate's headline |
| `stalk_seconds` | Held there, rather than touching the band and being spotted |
| `stalk_longest_seconds` | ... in one unbroken approach |
| `stalk_approaches` | Times it came from outside the band to inside it |
| `closest_stealthed` | The nearest it got while stealthed. Reported, never gated: it is a distance, and a gate floor cannot say "lower is better" |
| `stealth_openers` | The position used for what it is for |

**Read these against the at_start learner, not against `fight`.** The scripted baseline reads 0.0000 on every
stalk column because it never presses Stealth at all, so a zero there says nothing about whether the channel
works -- it is the same ambiguous zero that hid the line-of-sight bug in stage 16 for two runs. The untrained
learner explores into Stealth by accident and is the honest first reading: `stalked_into_range` 0.066 over
2048 episodes (136 of them non-zero), `stalk_seconds` 0.14 with a 21.75 s maximum, `stealth_openers` 0.044,
and `reward_stalk` 0.011 reaching its 1.0 cap in at least one episode -- which is also the check that the cap
fires.

**`druid_tank` reads exactly 0.000 untrained, and that is not a bug.** Prowl needs cat form, so a bear tank
would have to shift out of its role, stalk, and shift back. It may learn that and it may not, which is why
there is no per-layout floor on `stalked_into_range`: one layout that cannot reach it would halt the whole
queue. `survived` is the per-layout check instead, and it only says no layout collapsed.

### Stage 16: `stage16_companion`

Adds the companion block and the owner: a seat in the scenario's owner slot, played by the endurance policy through
the learner's cast (`cast.agents.owner`) in 70% of training episodes, and by the script -- which wanders and engages
on a timer, the shape the follow lesson was built on -- in the rest and in every evaluation, so `owner_deaths` keeps
its meaning. The seat learns to follow, assist, guard, heal and resurrect it, and
role-specific behaviour appears (tank threat, healer throughput, DPS threat discipline). Deaths recover after pulls and
the episode always runs its full length (450 s; without its own the arena took the host's 60 s), so letting the owner
die is never a way to escape penalties.

What to read: `clean_kill` -- the win above, with the seat never dead -- overall and per class, `owner_deaths`,
`wipes`, `pulls_cleared` and `livelocked`. The care checks are reported per class and per build (the summary's
`builds`): `owner_heal_share`, the share of the owner's damage taken the seat healed, for healers, and
`threat_share`, the share of the enemies' attention on the seat rather than the owner, high for tanks and low for the
rest. The party run's measured values (owner dead in 66% of episodes at 100M steps against the scripted baseline's
92%) are the numbers to read a new run against.

### Stage 17: `stage17_party`

Adds the party block. One to four learned seats (like a player bringing one to four companions) plus the owner form a
sim group. Every seat plays the same policy and sees the other three. An empty seat has no character and only the
no-op, and the learner drops its rows. Pulls are elite-heavy. The arena runs 450 s, as stage 10's. Config: budget 90M,
evaluation every 15M steps, a party-focused report. What to read: `owner_deaths`, `wipes`, `teammates_died`,
`pulls_cleared` and `low_health_seconds`; the 120M run's best (the owner dead in 66% of episodes against the scripted
baseline's 92%, 5.96 pulls cleared) is the number to read a new run against.

### Stage 18: `stage18_tanking`

**Drill.** Stage 17's party, with seat 0 always the tank (`ArenaDefinition::SeatAptitudes`). The ordinary party
draws every role, so the tanking lesson is smeared over whoever happened to play it; here the episode is about
holding what the pull brings and keeping it off the others. `threat_share` is the column that says whether it
happened. On the trunk: `stage19_triage` seeds from it.

### Stage 19: `stage19_triage`

**Drill.** The same party with seat 0 always the healer: keep the hurt one up, and spend mana to do it. A
forced-healer stage will find any fault in the resurrection path faster than anything else in the curriculum --
it found the farmable revive described in 4.6, where reviving a teammate paid more than keeping it alive. On
the trunk: `stage28_raid_single` seeds from it.

### Stage 20: `stage20_quest`

Life outside the fight begins, and it is learned in the sim rather than scripted for the live module. One seat, a
quest of its level band -- 15-20, 35-40 or 58-60; the rung is the band, drawn on the difficulty ladder like a pull
rung, and the level a draw within it -- in the world's own zone. Nothing about the place is hand-made:
`LifeWorld` indexes the continents' creature and gameobject spawns at startup, filters the quest templates down to
kill and collect quests whose giver and turn-in are spawned within reach of each other and whose objectives (the
quest POI table) lie within reach of the giver, and an episode copies the giver, the turn-in and the world's
creatures around each objective's place into the env's phase (`QuestEncounter`). The side is drawn with the band
and the race follows it (`EnvState::EpisodeTeam`), since a quest belongs to one; the built character is the final
filter (`Player::CanTakeQuest`), and a quest it cannot take fails the build and is not drawn again that episode.

The seat reads the world through the **world block** (`WorldBlock`): the nearest corpse it may loot, quest giver
it has business with, gathering node and vendor (presence, distance, bearing in its own frame, what each is and
whether it is in reach), the episode's quest state and progress, its bags, gold, durability, food and drink, and
whether something in the bags rates higher than what it wears. Six actions: INTERACT is the right-click -- on a
giver it takes or hands in the quest, on a node it gathers, on a corpse it loots (or skins a looted one) -- and the
world decides what it means, so what the policy learns is to stand at the right thing at the right time; LOOT_ALL,
EQUIP_UPGRADE, SELL_JUNK, REPAIR and BUY_SUPPLIES are the buttons a client has. The bodies are
`Character/WorldActions` (the opcode handlers with the packets left out), shared with the live module, and what is
a lookup stays scripted there: `GearScore` decides which quest reward and which bag item is the better one. The
fighting is the endurance run's, unchanged: the pack block's slots are filled with whatever is in combat with the
seat, then the nearest hostile of the objective. The travel block's objective is the quest's next place (the
giver, the objective, the turn-in), which is also what the potential shaping is on.

Rewards (`Life.*`): taking the quest, each objective count as it lands (times the band's tier scale), the turn-in
(times the tier scale; it ends the episode), the clock without it (less what was done), death, a wasted press, the
step cost, and progress toward the waypoint. The `life` baseline uses what is in reach, fights what fights back
(the `fight` baseline's play), and walks to the next thing. What to read: `quest_turned_in`, `quest_progress`,
`quest_kills`, `corpses_looted`, `wasted_presses`, `died`, per `quest_band`.

### Stage 21: `stage21_gather`

A field of the band's herb and ore nodes, with the zone's own creatures among them (`GatherEncounter`). The
grounds are the densest 400-yard cells of node spawns per zone, measured from the world database (the Barrens,
Westfall and Loch Modan at 15-20; Thousand Needles, Stranglethorn, Arathi and Feralas at 35-40; Un'Goro,
Winterspring, the Plaguelands, Silithus and the Burning Steppes at 58-60); an episode summons the nodes within
`Life.NodeRadius` of one into the phase, gives the seat herbalism, mining and skinning at the band's skill and the
tools, and stands it on the ground under the field. A node is opened by the profession's own cast (the rank spell
is the gathering spell) and emptied with LOOT_ALL; a killed creature that can be skinned is skinned with the same
INTERACT once looted. Rewards: each node gathered (times the tier scale), each skill point, death, waste, progress
toward the nearest node the seat can open. There is no winning a field: the clock ends it, and gathering every
node early counts as a win. Read `nodes_gathered` against `nodes_spawned`, `skill_ups`, `skinned`, `died`.

### Stage 22: `stage22_town`

A town of the seat's side, its traders copied into the phase around the inn (`TownEncounter`; the Crossroads and
Goldshire, Camp Taurajo, Menethil and neutral Ratchet, the capitals). The seat arrives with a purse for its level,
junk in its bags, half its durability gone, one food and one drink, and two better items it has not put on: sell,
repair, restock, dress, and it is won when all four are done before the two-minute clock. Rewards: the junk's
vendor value realised pro rata, the repair, the restock, each upgrade worn, all four done. Nothing fights back
here; what is learned is the vendor as a place to stand and the four presses. Read `town_won`, `sold_out`,
`repaired`, `stocked`, `upgrades_equipped` against `upgrades_given`.

**What the core needed for these: nothing.** The plan reserved a per-stage feature registry on the forge core
(`Sim.Features`) for the life stages; none of it was needed. Every API they use is public -- `Map::SummonCreature`
and `SummonGameObject` with the env's phase, `Player::SendLoot` and `StoreLootItem`, `AddQuest` and `RewardQuest`,
the profession casts -- and the state they make lives on the bot in memory, as everything a sim bot does already
does (no save runs after creation). The registry stays deferred until a stage needs a system the forge dropped.

### Stage 23: `stage23_dungeon`

The first real instance. A party of four learned seats and their cast owner against a dungeon's own scripted
bosses, in the dungeon (`InstanceEncounter`): the rungs are the bosses of five dungeons across the level bands --
Ragefire Chasm at 15, the Deadmines at 20, the Scarlet Monastery at 35-40, Stratholme at 60, heroic Utgarde Keep
at 80 -- so the rung fixes the level as well as the fight, and a class climbs a band at a time. The seats spawn at
the instance's front door and are taken to the boss along the server's own path from it (`PathGenerator`, in as
many legs as its point cap needs), and stand `Instance.EngageYards` back up that path: where a group that came in
the front stands, on its side of the trash it never pulled, with the room's triggers in front of it. The trash
around the boss is cleared for the episode; the creatures that are the encounter (`BossRow::Keep`) stay. The boss
fights with the core's script: won when it dies, lost when every seat is dead, when the script evades (the boss
back at full health out of combat), or on the clock. Nobody stands up mid-fight.

The table of bosses is data (`InstanceBosses.cpp`), and nothing in it is a position: the boss's spawn comes from
the world database and the engage point from the path, and a row whose template or spawn the database lacks is
dropped at startup with a log line. Bosses that need an event, a door sequence, a key or a vehicle are left out.
A tenth of the episodes are the party gauntlet on the host map, a control arena, so the same policy is graded on
real bosses and on the pool encounter it has always been graded on (`eval.jsonl`'s `arenas` group).

Rewards: `CombatReward::OneOnOne` against the boss for every seat (damage as a share of its health, so adds count
at the boss's scale; Kill and HealthKept to every seat when it dies; Death once per seat), all scaled by the rung
(`Difficulty.TierScale`, capped at `Instance.MaxTierScale`), plus `Instance.BossProgress` for the share of the
boss's health a lost fight took off it, so a fight has a gradient before its first kill. Read `boss_killed`,
`boss_health_left`, `wiped`, `evaded` and `boss_rung` per rung.

### Stage 24: `stage24_flag`

Warsong Gulch's rules between two learned seats (4.5), extending the arena and merging travel: the fight, and mounting
between bases 100-180 yd apart, with a carrier kept on foot. Blocks: core, duel, pet, pvp, travel, flag. 300 s
episodes, first to three captures. As in the arena, evaluation plays the second seat with `fight` (which heads for the
flags on a mount). Config: gamma 0.999 and lambda 0.99, budget 40M. What to read: `flag_pickups`, `flag_captures`
and `won` against `fight`.

### Stage 25: `stage25_warsong`

Warsong Gulch at its proper size: ten a side, both sides learned, on a real battleground instance. The flag
rules are stage 24's; what is new is that a side is ten seats and a group, so the objective has to be shared --
a carrier to escort home, a base somebody has to hold, and an enemy carrier ten of them can chase. It adds the
party block on top of the flag line.

The instance logs `GetBGObject: gameobject (type: 10) not found` repeatedly. That is pre-existing core noise,
not a stage fault.

### Stage 26: `stage26_duo_led`

Two against two under a **director**: one more agent a side, choosing the team's posture, the enemy it
concentrates on, the shape it takes, whose turn the next duty is, and -- since the place channel landed -- where
to go (4.12). It is the stage the director machinery is exercised on, and the one where the fog of war is
visible: `DirectorEncounter` sees only what its own side's living seats can see (`StageScenario::SideCanSee`),
so `director_enemies_seen` and `order_focus_unseen` distinguish a director that is blind from one that is
merely bad.

Blocks: core, duel, pack, pet, pvp, context, hostiles, support, order. Its arena sets `Directed`, and
`DirectorLearned` decides whether that director is the scripted yardstick or an agent that learns. **The
comparison between the two has not been run**, and 4.12 says why it should be before more budget goes into the
learned one.

### Stage 27: `stage27_crossroads`

Every line joins. It extends `stage19_triage` (the trunk and the PvE blocks) and merges `stage26_duo_led`,
`stage25_warsong`, `stage12_pvp` (the pvp block), `stage7_flight` (travel), `stage16_companion`, `stage10_gauntlet`
and `stage8_duel`, adding `context` and `hostiles`. Its layouts contain the PvE, PvP and travel blocks (the pet block
included). It is the last stage in the queue, and the one whose checkpoint is what ships.

| Arena | Weight | Episode | Situation |
|---|---|---|---|
| `companion` | 20 | 300 s | Stage 16's |
| `party` | 20 | 300 s | Stage 17's |
| `arena_1v1` | 25 | 60 s | Stage 12's |
| `gauntlet` | 10 | 450 s | Stage 10's |
| `duel` | 5 | 60 s | Stage 8's |
| `ambush` | 15 | 300 s | Companion gauntlet plus 1-2 ambushers arriving 20-120 s in |
| `escort_duel` | 5 | 90 s | Owner plus one enemy player, no pulls |

Learner: distilled with `teachers: auto` (each earlier arena is taught by the first parent that has it; the two new
arenas learn from reward alone), coef 1.0 halving every 50M steps. 256 evaluation episodes every 25M steps, the
arena_1v1 seat scored against `fight`. Budget 100M. What to read: each arena's score against the baseline (the
summary's `arenas`), `owner_deaths` in the companion, party and ambush arenas, and `won` in the mirror one.

### Stages 30-32 (by name): `stage30_raid10`, `stage31_raid25`, `stage32_raid40`

The real raids, each seeded from the one before and all from the dungeon: ten seats in Karazhan and Naxxramas,
twenty-five in Naxxramas, forty in Molten Core, Blackwing Lair and the Temple of Ahn'Qiraj (Naxxramas has no
forty-man in 3.3.5). No owner -- forty seats leave no slot for one -- so seat 0 leads the raid group
(`Group::ConvertToRaid`, a subgroup per five seats). Everything else is the dungeon stage's: the front door, the
path, the engage point, the control arena (the synthetic single pack at the same seat count), the terms. Forty
seats an env is forty bots an env, so `AnimusForge.Stage.<name>.Envs` runs them at 32, 16 and 8 envs, and their
evaluations are 128, 64 and 32 episodes.

What to watch in the first runs: a boss whose room the path enters from a side door (the engage point then sits
on the wrong side of a trigger; `EngageOverride` is the per-boss answer), enrage timers on the wall clock at a high
time scale, and the cost of forty players in one map update, which `forge bench` at those env counts will say.

### Stage 28 (by name): `stage28_raid_single`

A raid of eight groups of five against one elite and its adds, won or lost as the single pack is. What is new
is the size: forty learned seats in one episode, every one playing the same policy and seeing the others in its
group. Nothing about the fight is new -- the party stages taught it -- so what is being trained is a policy that
does not fall apart when the group it is in is one of eight.

### Stage 29 (by name): `stage29_raid_gauntlet`

The raid clearing pull after pull, recovering between them, over 600 s. Everything the PvE line taught -- the
duel, the pack, the hazard, the gauntlet's recovery, the companion, the party's roles, the raid's size -- is in one
episode. Nothing seeds from it: `stage27_crossroads` extends `stage19_triage`, and the raids are the yardstick for
a policy that has to hold together at forty.

## 4.12 Team play and the director

`SeatPlan::Teams` splits an arena's seats down the middle: `TeamSeats` a side, `TEAM_COUNT` (2) sides, seat
`s` on side `s / TeamSeats`. Two a side is an arena, ten a side is a battleground. `StageScenario::SideOf`
answers which side a seat plays for and `SideSeats` lists a side's seats in order.

A Teams arena fights the other side, not a scripted opponent: `OpponentEncounter` makes every cross-side pair
hostile, offers each seat the enemy side as selectable slots (so target selection has something to choose
between), and ends the episode when a whole side is down. One seat a side reduces to exactly the old `Mirror`
behaviour.

### The order

A **director** commands one side. It decides what the team is doing -- who to kill, what posture to hold,
where to gather, whose turn the next interrupt is -- while the seats keep deciding how. Two of the four are
things a seat provably cannot work out alone: nothing in seat 7's own view says it is next in the rotation,
and ten seats each picking their own target is the classic way to lose a fight you should win.

The order reaches a seat through the `Order` block, which has **thirteen observations and no actions**. An
order is advice, not a lever: the seat reads the posture, the rally point, the called target and whether it
holds the duty, and still chooses its own action. Everything reads zero in an arena without a director.

A call is dropped the moment it cannot be followed -- the focus when its enemy dies, the duty when its seat
does. Without that the order stands at a corpse until the director's next decision and, if it never spends
another focus action, for good: measured at 37% of all decisions carrying an order aimed at someone dead.

### Naming a place

A director can send its side somewhere: `TeamRally::Point` with a place the director named, and
`TeamPosture::Hold` to stay there rather than chase. The place channel existed end to end and was inert --
`SideOrder::Place` → `SeatView::TeamOrder::RallyPlace` → `OrderBlock`'s distance and bearing observations --
with nothing ever setting it. Only the naming was missing.

**The place is addressed as anchor + offset + ring, never as a coordinate.** One action per reachable spot
would make the action space the size of the world, which is the thing that has to keep working when this leaves
the arena for the open world. Instead the director names one field at a time, as it does with everything else:

| Field | Values |
|---|---|
| `PlaceAnchor` (6) | `TeamCentre`, `Focus`, `LastSeenEnemy`, `Objective`, `OwnBase`, `EnemyBase` |
| `PlaceOffset` (5) | `At`, `Toward`, `Away`, `Left`, `Right` |
| `PlaceRing` (2) | `Near` (`Director.PlaceNearYards`), `Far` (`Director.PlaceFarYards`) |

Thirteen actions, and they scale from a 2v2 arena to a continent unchanged, because a ring is a distance and an
anchor is whatever the side is currently about.

The offset is measured **about the anchor→enemy-centre axis** (falling back to the objective, then the last
sighting, then the side's own facing), not against absolute compass bearings. An absolute bearing would be
scale-free too, but it is not learnable from what the director observes: a director told to go "60 yards north"
cannot tell whether north is toward the enemy or off the map. Relative offsets need no extra observation and
are what a human caller actually says. (Bearings were added to the observation anyway -- `SEAT_BEARING_SIN/COS`
and `ENEMY_BEARING_SIN/COS` -- because the director previously could not tell *where* anything was, only how
far.)

`Place` and `HasPlace` are **derived**, recomputed by `ResolvePlace` every decision, so a place anchored to the
focus or the team centre tracks as they move, and `HasPlace` is true exactly when the rally is `Point` and the
anchor resolved. Two independent ways to say "there is a place" is how a channel like this drifts out of step.
The resolved point is snapped to walkable ground (`Encoding::SnapToGround`, the `Map::GetHeight` probe
`DuelBlock::FindCover` already used) -- an unsnapped place sends ten seats into a wall.

Going there is paid by `RewardTerm::PlaceMatch`, **once on crossing into the place radius, with a per-seat
cooldown**, and routed through `ShapingPaid` so the director's own reward takes it back out exactly as
`OrderMatch` is. Never per decision: the per-decision order nudge came to 5.01 an episode, 23.7% of gross, and
had to be cut to a fifth of a percent. A transition nudge with a cooldown cannot be farmed by oscillating
across the boundary.

The whole group is gated behind `ArenaDefinition::Places`, so an arena with nowhere worth sending anyone keeps
the thirteen actions masked and pays no exploration for a vocabulary it cannot use. `TeamPosture::Scout` was
deliberately **not** added: a posture is read by the whole side, so "one scouts, nine hold" is already
`Hold` + `Rally::Point` + the duty, and a second way to say the same thing is worth adding only once the
metrics show the place channel is used at all.

### The director is not omniscient

`ViewSide` used to read every enemy's `IsAlive`, health, combat and casting state, live position and even its
class straight out of the world with no visibility check. Now **the director sees only what its own side's
living seats can see**: `StageScenario::SideCanSee(env, side, unit)` is true when any living seat of that side
`CanSeeOrDetect`s it, so a side that wiped stops spotting. The seen-mask is computed once per decision in
`Update` and read by `ViewSide`, because a `SideCanSee` per slot per side would be O(own × enemy)
stealth-and-invisibility checks on the world thread -- forty a side a decision in a ten-a-side fight.

An enemy slot is one of three things:

- **Seen now** -- live values.
- **Seen before** -- the remembered position and health, with `ENEMY_UNSEEN_TIME` saying how stale it is
  (scaled by `MAX_UNSEEN_TIME_MS`, 20 s). `ENEMY_CASTING` and `ENEMY_IN_COMBAT` go to **zero**, not to their
  remembered values: they are instantaneous facts, and a stale one is a lie the policy would learn to trust.
- **Never seen** -- presence only, and `ACTION_FOCUS_FIRST + slot` is masked. `ENEMY_PRESENT` now means "ever
  seen".

Memory is per side, keyed by slot rather than GUID (the GUID is held only to notice a slot being reassigned),
which keeps an allocation off the per-decision path.

Three leaks survived the obvious fix and had to be closed separately. `Forget` cleared a focus whose target
died -- to a learned director, a call quietly vanishing *is* the ground-truth signal "he is dead" -- so it is
gated on remembered state when the director is learned. `Call` refused a focus slot whose bot was not alive,
and the refusal was observable through `SinceCall` not advancing, so it now accepts any slot the side
*believes* alive. And a seat's own `OrderBlock` focus observation read a live `Unit*` with no visibility
filter, so a seat could read a called focus's distance and health through a wall; it is now filtered by the
seat's own sight, falling back to the order's last-known place.

**Two deliberate non-changes.** The **critic stays omniscient** -- `WriteState` is unfiltered on purpose;
centralised critic with decentralised execution is what MAPPO is, and filtering it would make the value
function worse for nothing. And the **scripted director keeps reading ground truth**: it is a yardstick and
instrumentation, not a policy, and a yardstick that had to scout would stop being fixed.

This makes the learned director strictly *less* informed than the omniscient yardstick it is scored against, so
the directed stages' baseline comparison was relaxed in the same change (and has since gone with every other gate),
and `order_focus_unseen` and
`director_enemies_seen` were added so a director that is blind is distinguishable from one that is merely bad.

### Scripted or learned

`ArenaDefinition::Directed` gives an arena a director; `DirectorLearned` makes it an agent rather than a
script.

The **scripted** director (`DirectorEncounter::Command`) thinks every 10 decisions and calls the lowest-health
enemy, rotating the duty around the side and switching to Recover below 40% average health. It is the yardstick:
its call quality is near-perfect by construction, so it is what a learned director is measured against.

The **learned** director is an ordinary layout (`DirectorLayout`) with its own 257 observations and 42 actions,
seeded down the stage chain like any other. Two more agents an env, one a side, opted into per arena; an
undirected episode marks them absent rather than resizing anything.

Its action space is one call per decision, not several heads at once: `hold`, then posture (6), rally (8),
place anchor (6), place offset (5), place ring (2), focus slot (`PACK_SLOTS`), duty slot (`TEAM_SEATS`). The
standing order is state it edits, and an action names the single field it changes -- everything else keeps what
it was. That is closer to what a leader does than four
simultaneous heads would be (a call stands until it is changed) and it needs no new transport: the wire carries
one categorical action per agent.

It is paid the mean of its side's seat rewards, less any order-compliance shaping those seats earned, because
that is the one part of their reward it can move without the fight going any better. A director that kept it
would learn to call whoever its seats were already fighting.

It decides on a **slower clock** than the seats -- `mappo.slow_layout` in the learner -- choosing every
`slow_every_decisions` and holding in between, with its transitions stored and discounted over its own
decisions. Both clocks are printed when a run starts:

```
Per 250 ms decision:         gamma 0.99750 (horizon 100 s), GAE trace 0.97275 (credit   9.2 s)
Per 2.5 s director decision: gamma 0.99600 (horizon 625 s), GAE trace 0.97608 (credit 105 s)
```

The cadence is not only about credit. With the director choosing every decision it moved the standing order on
0.706 of them, against the scripted director's 0.03; nothing can follow a call that changes every 1.4
decisions.

### What to measure

A director is worth having only where it beats its own absence, so a directed stage is read against the
undirected one it came from. The columns, all reported per seat:

| Column | What it says |
|---|---|
| `order_focus_alive` | The call named a living enemy at all |
| `order_focus_lowest` | ... and it was the most hurt of them, the call the scripted director makes |
| `order_focus_chance` | What naming one of the living at random would have scored |
| `order_focus_kept` | The share of a side's seats on the called target |
| `order_changes` | How often the call moved |
| `order_focus_unseen` | The called enemy was one the side could not see -- a blind call, not a bad one |
| `director_enemies_seen` | How many enemy slots the side could see, out of those present |
| `order_place_called` | A place was named at all: the first non-zero reading is what says the channel stopped being inert |
| `order_place_reached` | ... and a seat got there |
| `order_place_distance` | How far the side was from it |

`lowest` against `chance` is the one that matters, and it is the same scale whether a side is two or ten: it
tells a director that calls well apart from one the arena makes look good. `order_focus_kept` alone cannot --
a seat that ignores its director and a director that names nothing worth fighting look identical in it, and at
two a side its chance floor is 0.5, because a seat parked on the first enemy slot is on the called target half
the time by construction.

**Honest status.** As of this writing the learned director has not beaten chance on `lowest` in four 30M runs,
through a compliance reward, a slower clock and a clean channel. The seats improve (evaluation 6.6-6.9 to
8.3-8.8 in every run) but that is them learning two on two, and it happens just as much without a director.
The open question is whether a director is worth anything at two a side at all, which the scripted director
answers directly: run `stage26_duo_led` with `DirectorLearned` off and compare. If a perfect caller does not
beat the undirected arena, there is nothing at this stage for a learned one to find.
