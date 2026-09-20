# The curriculum, in the order it should be learned

The numbers are an accident of the order things were written, not of the order they should be trained. Stage 12
(endurance) refines stage 3 and sits nine stages away from it; the three drills added later -- hazards, tanking,
triage -- are numbered 15, 16 and 17 but belong beside stages 2 and 5; the raid is 13 and 14 with the
battleground at 18. Anyone reading the list learns the history of the project rather than the shape of the
skill.

This renumbers every stage into the order it should be trained, and adds the six directed stages in their place
rather than bolted to the end.

## The order

**A seat learns to fight** -- solo, from nothing.

| new | was | what |
| --- | --- | --- |
| `stage1_duel` | `stage1_duel` | one creature its own level: close, kill, keep its health |
| `stage2_pack` | `stage2_pack` | two to four, usually linked: targets, interrupts, control |
| `stage3_hazards` | `stage15_hazards` | something on the ground in every pull: see it, walk out |
| `stage4_gauntlet` | `stage3_gauntlet` | pull after pull: heal, eat, drink, survive the run |
| `stage5_endurance` | `stage12_endurance` | a known run of eight: what to spend, what to keep |

Hazards moves from 15 to 3 because it refines the pack and nothing after stage 2 should be learned without it --
standing in fire is the cheapest bad habit to acquire and the dearest to remove. Endurance moves from 12 to 5,
next to the gauntlet it plans.

**A seat learns to move.**

| new | was | what |
| --- | --- | --- |
| `stage6_travel` | `stage9_travel` | a place 60-320 yd away: mount when it pays, arrive on foot |
| `stage7_flight` | `stage10_flight` | 350-700 yd in Nagrand: take off, fly, land |

Travel moves ahead of the group and PvP stages. Everything after it that crosses ground -- the flag, the
battleground -- wants a seat that already rides.

**A seat learns to fight beside others.**

| new | was | what |
| --- | --- | --- |
| `stage8_companion` | `stage4_companion` | the gauntlet beside a scripted owner: follow, assist, guard, heal |
| `stage9_party` | `stage5_party` | four learned seats and the owner against elite pulls |
| `stage10_tanking` | `stage16_tanking` | a fixed tank seat: hold what the pull brings |
| `stage11_triage` | `stage17_triage` | a fixed healer seat: keep the hurt one up |

The two drills follow the party they drill, instead of sitting eleven stages later.

**A seat learns the raid.**

| new | was | what |
| --- | --- | --- |
| `stage12_raid_single` | `stage13_raid_single` | eight groups against one elite and its adds |
| `stage13_raid_gauntlet` | `stage14_raid_gauntlet` | a raid clearing pull after pull |

**A seat learns to fight people.**

| new | was | what |
| --- | --- | --- |
| `stage14_pvp` | `stage6_pvp` | one on one against a scripted enemy player |
| `stage15_arena` | `stage7_arena` | self-play one on one |
| `stage16_crossroads` | `stage8_crossroads` | PvE and PvP in one policy, both branches merged |

**A seat learns an objective.**

| new | was | what |
| --- | --- | --- |
| `stage17_flag` | `stage11_flag` | capture the flag one on one |
| `stage18_warsong` | `stage18_warsong` | ten a side, the real battleground, no director |

The only stage that keeps its number, and the only one already written to the new shape.

**A team learns to be commanded.**

| new | was | team | director | channels |
| --- | --- | --- | --- | --- |
| `stage19_duo_led` | new | 2 v 2 | scripted | focus |
| `stage20_duo` | new | 2 v 2 | learned | focus |
| `stage21_trio` | new | 3 v 3 | learned | + duty |
| `stage22_group` | new | 5 | learned | + rally |
| `stage23_warsong_led` | new | 10 v 10 | learned | + posture |
| `stage24_raid_led` | new | 40 | learned | all four |

`mix_duel_pvp` keeps its name: it is a pilot, not a rung.

## What renaming costs, and how it is paid

A stage's name is a key in five places, and three of them hold work already done:

1. **`Stages.cpp`** -- the definition, and every `Extends` and `Merges` string that names it.
2. **`python/configs/<name>.yaml`** -- the learner config, and its own `extends:` line.
3. **`var/animus-forge/runs/<name>/`** -- **the checkpoints**. A renamed stage looks like a stage that has never
   trained, and its children seed from nothing.
4. `AnimusForge.Queue` and the default queue order in the host config.
5. `models/<name>` exports.

(3) is the one that matters. Stages 1-11 and 18 hold real runs: 30M steps of duel, 20.4M of flag, and the arena
checkpoint the flag stage seeds from. Renaming without moving them throws all of it away.

**So the rename moves the run directories with it**, in the same change: a table of old to new applied to
`Stages.cpp`, the config filenames, the queue, and `runs/`. It is mechanical, and it must be one commit -- a
half-applied rename leaves stages seeding from directories that no longer exist, and the failure is silent
(`bootstrap` finds no checkpoint and starts fresh, having said so in one line).

**Done once, before the directed stages are built.** Adding six stages to a list that is already in the wrong
order means renumbering twice.

## Order of work

1. The rename table, applied to `Stages.cpp`, the YAML configs and their `extends:` lines, the queue, and
   `runs/` together, in one commit. No behaviour changes in the same commit.
2. Verify: every stage still loads (`Stage ... is left out` in `Server.log` catches a broken `Extends`), and
   each renamed run still seeds -- `forge scenarios` should show the same checkpoints and steps against the new
   names.
3. Only then the `Order` block and the directed stages, in the ladder above.
