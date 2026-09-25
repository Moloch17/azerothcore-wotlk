# mod-animus-forge

Animus Forge trains World of Warcraft 3.3.5a bots that play every class, in every build it has. It is an AzerothCore module for the
**forge core** (the `forge` branch of [azerothcore-wotlk](https://github.com/Moloch17/azerothcore-wotlk)), a headless
simulator that runs faster than real time, together with a Python MAPPO learner in [`python/`](python/).

The sim runs many environments in parallel, each its own dungeon instance, and turns every seat into a new character
each episode. It trades observations for actions with the learner over a Unix socket, one decision at a time. The
learner trains one policy for all ten classes through a curriculum of stages, scores it against a scripted
baseline, decides when a stage is good enough to move on, and exports one small `.amdl` model per class.
[mod-animus](https://github.com/Moloch17/animus) plays those models on an ordinary realm.

**The detail is in [the Animus manual](docs/manual/README.md).** This page is the map.

## The pieces

| Piece | What it does | Manual |
|---|---|---|
| Forge core | Fixed-tick, headless AzerothCore: no clients, no bot persistence, a simulated clock | [2](docs/manual/02-forge-core.md) |
| The curriculum layer, `src/` | Stages, blocks, encounters, rewards, characters, env pools and bots. Was animus-lib, a separate repository; folded in here | [3](docs/manual/03-animus-lib.md), [4](docs/manual/04-curriculum.md) |
| This module, `src/` | Plans of stages, the `forge` console commands, the lock-step bridge, the learner process, progress reports, export | [5A](docs/manual/05-animus-forge.md#part-a-the-module) |
| The learner, `python/` | MAPPO, seeding from earlier stages, distillation, seeded evaluation, the convergence rule that ends a stage, `.amdl` export | [5B](docs/manual/05-animus-forge.md#part-b-the-learner) |
| [mod-animus](https://github.com/Moloch17/animus) | Class companions on a stock realm | [6](docs/manual/06-animus.md) |

## The curriculum

One line of stages, each seeded from the one before it. Every stage trains one policy per class, covering every
build that class has.

```
move ─ indoor ─ jump ─ dive ─ dodge ─ travel ─ flight   the feet: ground, rooms, ledges, lakebeds, fire, the mount, the air
     ─ duel ─ pack ─ gauntlet ─ endurance                alone, against things that fight back
     ─ pvp ─ evade ─ hide ─ stealth                      against people: self-play, then not being caught
     ─ companion ─ party ─ tanking ─ triage              beside others, nobody commanding yet
     ─ flag ─ warsong ─ duo_led ─ crossroads             an objective, a director, and everything at once
```

Thirty-two stages, numbered in the order they are trained (`stage1_move` to `stage32_raid40`); twenty-seven are
the queue and the five raid stages are trained by name. No stage has a pass gate: each ends when its convergence signals say so, and the queue
moves on.

**It starts with the feet.** The first seven stages have nothing to kill in them: a seat steers itself now, and
where it puts its feet is not something only some stages are about — so everything after them inherits legs that
already work, rather than learning to fight and to walk at the same time.

Then a duel against a creature grows into packs, a gauntlet of pulls, an owner to protect (played by an earlier
policy) and a real party;
a PvP run goes from self-play through evading, hiding and stealth against a scripted hunter; and the last stages
add an objective and a director. It is one line rather than a tree because a branch ends in several checkpoints and
everything a leaf teaches is discarded unless the stage exported from is downstream of it. See
[chapter 4](docs/manual/04-curriculum.md).

## Quick start (Docker)

```bash
git clone -b forge git@github.com:Moloch17/azerothcore-wotlk.git animus-forge-core
cd animus-forge-core
git clone git@github.com:Moloch17/animus-forge.git modules/mod-animus-forge
./forge.sh            # build and start everything, then attach to the console (detach: Ctrl+P Ctrl+Q)
```

Everything the module needs is in `src/`; there is no library to fetch. The first start builds the images, the
worldserver and the Python venv, so it takes a while. GPU passthrough, native builds and the settings worth reviewing first are in
[Operations 7.1](docs/manual/07-operations.md#71-setting-up-the-training-host-docker).

Then, on the worldserver console:

| Command | What it does |
|---|---|
| `forge run stage8_duel fight 256` | Play the scripted baseline with no learner, to check that characters and fights build |
| `forge fast` | The whole pipeline on an easy profile, minutes per stage, into `<OutputDir>/fast/` |
| `forge start` | Train the curriculum stage by stage; each ends when every class has converged or at its budget, and the queue moves on |
| `forge status` | Rates, ETAs, evaluation scores against the baseline, warnings |
| `forge pause`, `resume`, `cancel`, `skip` | Control a run. The learner saves on cancel and skip |
| `forge export <stage>` | Write the `.amdl` models and their manifests for mod-animus |
| `forge bench` | Find this machine's fastest map thread and env counts |

Every command is in [5.7](docs/manual/05-animus-forge.md#57-console-commands). Monitoring, restarts, halted stages,
deployment and troubleshooting are in [chapter 7](docs/manual/07-operations.md). TensorBoard runs on
http://localhost:16006.

## Repository

| Path | Contents |
|---|---|
| `src/` | The module (C++, namespace `AnimusForge`) |
| `python/animus/` | The learner package, with one config per stage in `python/configs/` and its tests in `python/tests/` |
| `conf/mod_animus_forge.conf.dist` | Every `AnimusForge.*` key, documented. [8.1](docs/manual/08-reference.md#81-configuration-keys) lists them |
| `docs/manual/` | The Animus manual |

Settings come from config files only (`AnimusForge.*` keys, or `AC_ANIMUS_FORGE_*` in the environment), never from
worldserver flags. Training output goes to `AnimusForge.OutputDir` (`/azerothcore/var/animus-forge` in Docker).
