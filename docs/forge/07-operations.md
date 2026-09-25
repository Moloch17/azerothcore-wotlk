# 7. Operations

Step-by-step procedures. Each one links back to the chapter that explains what happens underneath.

## 7.1 Setting up the training host (Docker)

**Prerequisites.** Linux (or WSL2 on an ext filesystem), Docker with Compose, and roughly 50 GB of disk for client
data, builds and runs. A GPU is optional: updates run on the CPU without one, just more slowly.

1. **Get the core and the modules.**

   ```bash
   git clone -b forge git@github.com:Moloch17/azerothcore-wotlk.git animus-forge-core
   cd animus-forge-core
   git clone git@github.com:Moloch17/animus-forge.git modules/mod-animus-forge
   ```

   If you also keep `modules/mod-animus` in this checkout, it must be disabled in the forge build (step 2).

2. **Machine-specific settings** go in `docker-compose.override.yml` (gitignored):

   ```yaml
   services:
     ac-worldserver:
       environment:
         CCUSTOMOPTIONS: "-DMODULE_MOD-ANIMUS=disabled"      # only if modules/mod-animus exists
         # AMD: ANIMUS_TORCH_INDEX_URL: https://download.pytorch.org/whl/rocm6.4
       # NVIDIA (needs the NVIDIA Container Toolkit):
       deploy:
         resources:
           reservations:
             devices: [{ driver: nvidia, count: all, capabilities: [gpu] }]
     ac-dev-server:
       environment:
         CCUSTOMOPTIONS: "-DMODULE_MOD-ANIMUS=disabled"
   ```

   For AMD, pass `/dev/kfd` and the card's render node, add the host's `render` and `video` group ids, and set
   `security_opt: [seccomp=unconfined]`. The commented block in `docker-compose.yml` has the full example.
   **Set `ANIMUS_TORCH_INDEX_URL` before the first start**, because the venv is created only once.

3. **Start.**

   ```bash
   ./forge.sh
   ```

   The first start takes a long time. It builds the images, downloads client data into the volume, builds the
   worldserver from source, creates the MySQL databases (`Updates.AutoSetup`), creates the Python venv with torch and
   TensorBoard, starts TensorBoard, and starts the worldserver. `forge.sh` attaches you to the console. Detach with
   **Ctrl+P Ctrl+Q**. **Ctrl+C stops the server.**

4. **Check.** The console should print "Animus Forge is idle" and the idle settings. From another terminal, check that
   torch sees the GPU:

   ```bash
   docker compose exec ac-worldserver \
     /azerothcore/modules/mod-animus-forge/python/.venv/bin/python -c "import torch; print(torch.cuda.is_available())"
   ```

   (ROCm builds also report through `torch.cuda`.)

5. **Configure.** Edit `env/dist/etc/modules/mod_animus_forge.conf`, which was created from the `.dist` on first start,
   and `env/dist/etc/worldserver.conf`. Restart with `./forge.sh stop` then `./forge.sh`. At minimum, review:
   - `MapUpdate.Threads` in `worldserver.conf`: set it to the number of physical cores.
   - `AnimusForge.Envs`: 64 by default. Raise it until the learner, not the world thread, is the bottleneck (7.11).
   - `AnimusForge.EpisodeSeconds`: 60 by default. Arenas with their own length ignore it. Stages 3-5 need episodes of
     several minutes, and stage 8's arenas set their own.
   - `AnimusForge.Classes`: empty trains all 10. A subset trains faster, but a later change to the list breaks
     seeding and resuming from those runs.

**Native (without Docker).** Build the forge core with the modules as usual (`acore.sh compiler build`). Create the venv
yourself (`python3 -m venv modules/mod-animus-forge/python/.venv` and
`pip install torch && pip install -e 'modules/mod-animus-forge/python[tensorboard,dev]'`), copy the conf `.dist` to
`.conf`, and run `worldserver` in a terminal. Without `AnimusForge.OutputDir`, runs go into the module's `python/`
directory.

## 7.2 Smoke test with a scripted policy (no Python)

```
forge run stage8_duel fight 256
forge status
```

`forge run` builds the scenario and plays the `fight` baseline for 256 episodes. `forge status` shows the episode means:
`killed`, `died`, `dps`, `casts_completed` and so on. This checks that characters build, opponents spawn and the
mechanics behave, without any learner involved. Compare `random`, `greedy` and `fight`. Those numbers are what a
trained policy must beat.

To baseline every queued scenario, set `AnimusForge.Policy = "fight"` and `AnimusForge.Queue.LocalEpisodes = 1024`,
then `forge start`.

## 7.3 Fast test run

Before a long run, or after changing a scenario, the learner or a config:

```
forge fast
```

It trains every curriculum stage in order (the raid stages included), each from scratch, with 32 envs,
**all ten classes at the stage's own levels**, into `<OutputDir>/fast/`. Nothing is skipped, so typing it
again runs the whole curriculum again. `forge fast stage9_pack` trains one stage, seeded from the fast
`stage8_duel` run. Set `AnimusForge.Fast.Queue` to train a shorter list.

**It is a fixed-budget sweep, not an early-stopping smoke test.** Each stage trains a set number of steps and
moves on: 20,000,000 by default, and `forge fast 30M` (or `forge fast 30M stage9_pack`) overrides it for that
invocation. The mechanism is `convergence.patience: 0` in `configs/fast.yaml`, which makes
`ConvergenceTracker.converged` return false, so the stage cannot stop early and cannot trigger a restart; the
cleared `target:` block means a gate cannot halt the sweep either. That makes the sweep a genuine rehearsal of
the real build -- same classes, same levels, same stages, less budget -- rather than a different problem.

The budget in force is printed at the start of the run and in `forge status`, so what is reported is the budget
actually used and not the configured default.

What to check, in `fast/runs/<stage>/`:

- `eval.csv`: the `at_start` score against later scores. It should rise.
- `eval_baseline.json`: the baseline to beat.
- `metrics.csv`: entropy should fall slowly, and `approx_kl` and `clip_frac` should stay moderate.
- `finished.json`: why the stage ended.
- For merge stages, the `distill_kl` column should fall.

To start fresh: `forge clean fast`.

## 7.4 Training the curriculum

```
forge start
```

With an empty `AnimusForge.Queue`, this trains every default-queue stage in order (stages 1 to 23; the raid stages
only when named), skipping any stage whose run already finished. Each stage:

1. builds its env pool (world stalls for a few seconds per class on the first build),
2. writes `layouts/<stage>/`,
3. starts the learner, which seeds from the closest trained ancestor,
4. trains until every class has converged or its budget is reached (the next stage starts), or it is cancelled.

To train particular stages: `forge start stage12_pvp stage13_evade`. List each stage after the stage it extends, or it
won't seed from it (the command warns you). With no arguments the queue is every default-queue stage in number
order, which is already a valid order, so the usual case needs no arguments at all.

### Training one class at a time

The curriculum trains in two parts (see chapter 4, *Training one class at a time*): the movement stages once for
all ten classes, then each class's fighting stages on its own.

**The shared movement root** -- stages 1-7, all ten classes, in the base server:

```
# env/dist/etc/modules/mod_animus_forge.conf
AnimusForge.Classes = ""
AnimusForge.Queue   = "stage1_move, stage2_indoor, stage3_jump, stage4_dive, stage5_dodge, stage6_travel, stage7_flight"

# .env (compose passes AC_ANIMUS_FORGE_OUTPUT_DIR, which beats AnimusForge.OutputDir in the conf)
ANIMUS_FORGE_OUTPUT_DIR=/azerothcore/var/animus-forge/shared
```

The base server needs recreating for that (`./forge.sh --build`, or `docker compose up -d --force-recreate
ac-worldserver` when nothing was compiled), because the conf and the environment are read at startup.

**Then the classes, two at a time**, each in a training server of its own:

```
modules/mod-animus-forge/tools/forge_classes.py run druid mage warrior paladin hunter rogue priest deathknight shaman warlock
```

Nothing in the sim or the learner is shared between two runs but the cores, so a second class is a second
`ac-worldserver` container: the same image, source tree, built worldserver, database and GPU, with its own output
directory (`var/animus-forge/<class>`), its own `AnimusForge.Classes` and `Queue` (stages 8-19), and its own share
of the map-update and torch threads. Compose cannot make services on the fly, so the tool writes one compose file
per class, `env/instances/<class>.yml` (add the directory to your gitignore; it is machine-specific and holds the
rendered environment, `.env` values included), holding the `ac-worldserver` service exactly as `docker compose
config` renders it on this machine (your `docker-compose.override.yml` is part of that: the GPU devices come from
it) under its own name, container, output directory and host ports. Any `AnimusForge.*` or `worldserver.conf` key
is an `AC_` environment variable there (`AnimusForge.Queue` is `AC_ANIMUS_FORGE_QUEUE`; the environment beats the
conf file). It starts
`ANIMUS_FORGE_PARALLEL` (2) of them, watches for the last stage's `finished.json`, stops a finished class's server
and starts the next; Ctrl+C leaves the running ones training and `run` again resumes the schedule. Each instance's
TensorBoard and dashboard are on the base ports plus ten per instance. One instance is addressed by name:

```
ANIMUS_FORGE_INSTANCE=druid modules/mod-animus-forge/tools/forge_classes.py attach     # the console; Ctrl+P Ctrl+Q detaches
ANIMUS_FORGE_INSTANCE=druid modules/mod-animus-forge/tools/forge_classes.py stop
modules/mod-animus-forge/tools/forge_classes.py status
```

Two at a time and not three: a worldserver starts at ~3.5 GB and one long `forge bench` sweep climbed to 19.6 GB
(7.11), which 30 GB holds twice, not three times. `ANIMUS_FORGE_PARALLEL=3` is allowed if memory proves to stay
flat. The thread split (12 map threads and 4 torch threads each on 32 cores) is written into each instance file;
`forge bench` in one instance at that setting says whether it is right, and the file can be edited by hand.

A run of exactly one class also picks up that class's own learner configs: `configs/<class>/<stage>.yaml` where
one exists, and the shared `configs/<stage>.yaml` otherwise. Every class has a `stage8_duel.yaml` there, and it
says one thing: where the class's combat line seeds from -- `{shared_runs}/stage7_flight/best.pt`, the shared
root's checkpoint, which `init_from: auto` cannot find because the seed chain looks under the run's own `runs`
directory and the shared root is a sibling of it.

### Monitoring

| Where | What |
|---|---|
| `forge status` | The live report: rates, ETAs, evaluation scores against baseline, warnings |
| `forge progress 600` | The same report every 10 minutes |
| The dashboard at http://localhost:18800 | One page: the conf the run is using (and what differs from the dist default), steps, rate, ETA, evaluations against baseline, the training curves, and the last evaluation per class. It shows `forge fast` runs as well as real ones, and picks each stage's curves out of its own metrics.csv rather than plotting a fixed list. Started by the worldserver container, refreshes every 5 s. With `SOAP.Enabled` and an `etc/animus-dashboard.auth` holding `user:password` for an account with SEC_ADMINISTRATOR, it also gets pause/resume/skip/cancel and the seeding choice below; without them it is read-only |
| `<OutputDir>/runs/<stage>/layouts.csv` | Per (class, role), **every update**: what each pair is doing in the training episodes themselves (sampled actions, each at its own ladder difficulty). The `layout` column is the class and `role` its own, so a paladin appears twice. metrics.csv averages them all together and the evaluation tables come only every `eval.every_env_steps`; this is the live view, and the dashboard shows it as "Class and role, right now". Read behaviour from it, not scores -- the gates stay on the evaluations |
| TensorBoard at http://localhost:16006 | `episode_*`, losses, entropy, `eval/*`, `eval_<band>/*`, `eval_arena_<arena>/*` |
| `env/dist/logs/animus-learner.log` | Everything the learner prints, including evaluation tables per level band, class and arena |
| `<OutputDir>/runs/<stage>/eval.csv`, `eval.jsonl` | Every evaluation, with full tables |

#### Turning the dashboard's controls on

The page is read-only until it has somewhere to send a command and an account to send it as. Both are off by
default, so a rig that does not ask for them keeps the property the rest of the fork relies on -- the sim opens no
network listener.

1. `SOAP.Enabled = 1` in `env/dist/etc/worldserver.conf`. `ForgeMain.cpp` starts the thread when it is set;
   upstream's `Main.cpp` is not compiled here, so the key did nothing at all before that.
2. An account with `SEC_ADMINISTRATOR`, from the console: `account create <name> <password>` (16 characters at
   most -- a client limit the console enforces) then `account set gmlevel <name> 3 -1`.
3. `env/dist/etc/animus-dashboard.auth` holding `<name>:<password>`, mode 600. The launcher passes it as
   `--soap-auth` when it exists and says nothing when it does not.

Restart the worldserver and the page gains pause, resume, skip and cancel. They run the same handler and the same
`SEC_ADMINISTRATOR` check a typed console command does, so they grant no authority the console does not already
have; the page sends a command *name* from a fixed list, never a command string.

**Which checkpoint the next stage starts from.** `latest.pt`, by default (`seed_from`).

The alternative is `best.pt`, and it is a worse default than it sounds. `best.pt` is only rewritten by an
evaluation that clears the convergence margin -- `max(min_improvement_abs, min_improvement x |best|, z x the two
scores' standard errors)` -- and on a short run the last term dominates, because 64-episode evaluations have wide
error bars. A stage can then improve a great deal without ever clearing the bar. Measured on a fast sweep:
`stage9_pack` reached 8.2M steps with its evaluations up from 2.6 to 6.8 and `best.pt` still the checkpoint it had
been seeded with, because a 4.16 improvement fell short of a 4.46 margin. A queue that advanced there would have
handed stage 3 a network that had learned nothing of stage 2.

What `best` buys is protection from a late regression: an entropy collapse or a bad restart near the end of a
stage is carried by `latest.pt` and not by `best.pt`. On a real run the two are close, since convergence ends a
stage when it stops improving and `latest` is then near-best by construction; it is short runs where they
diverge. For a long build where that protection is worth more than the freshness, set `seed_from: best`.

The dashboard's "Seeding the next stage" panel says which file the next stage would take and how far behind
`best.pt` is, and lets you pick either or fall back to the default. Picking one writes a one-word `seed_from` file
in the run directory, which `animus.train.seed_preference` reads; it applies to merge parents as well as the base,
and a run's own file wins over the config.

| `<OutputDir>/runs/<stage>/stage.jsonl` | Restart, advance and halt decisions with their gates |
| `forge scenarios` | Every stage's run: finished and why, checkpoints, steps, best score |

Warnings to act on:

- **"learner has not answered"**: it is stalled or doing a very long update. Check the learner log.
- **"step rate dropped"**: another process may be competing for CPU, or an evaluation is running.
- **"entropy under 25%"**: the policy may have collapsed early. Consider raising `mappo.entropy_coef`.
- **"approx KL / clip fraction high"**: updates are too large. Lower the learning rates or the epochs.
- **"best below baseline after 2 evaluations"**: check the reward for that stage, and compare against the fast run.

### Warnings the learner prints without being asked

Two things are checked every update and reported when they happen. Neither changes what the run does; both are
there because the failure they describe is invisible in the ordinary metrics until a run has been wasted on it.

**`reward: <term> earns N an episode, X% of the largest outcome term`**

A shaping term has grown into the objective. Outcome terms -- the kill, the clear, the capture, the arrival --
are what a stage is *for* and may be any size; everything else is a nudge, and a nudge worth more than half a
kill is not a nudge. Only earnings trip it, never charges: a penalty is not farmable, and the largest negative
term in a fight is the death, which is the point of having one.

What to do: read the mix on the same line and decide whether the term is mispriced or exploitable. Three times
in this project it was both. A resurrection offer the core never clears was being accepted every decision and
came to 88% of `druid_dps`'s return; a goal paid for every decision it was held made standing at range the
second largest earner; an order nudge priced per decision reached 23.7% of gross, level with the kill. The
first two were found by hand after runs had already trained on them.

The rule lives in `python/animus/rewards.py`, including which terms count as outcomes. A resurrection is
deliberately not one of them -- standing an ally up is a means, and listing it as an outcome is exactly what
would let a farmable revive read itself as the yardstick.

**`learning has stalled: approx_kl has stayed under ... for N updates`**

The updates have stopped moving the policy. Roughly half the stages measured end their run this way --
`approx_kl` falls eight to elevenfold between the first eighth of a run and the last, with `clip_frac` down to
about 0.01 -- so the final third costs wall clock and buys very little.

What to do: **nothing automatically.** The other half of the stages do not stall at all (`stage8_duel`'s KL
*rises* over 683 updates; travel and flight stay flat), and `stage10_gauntlet` trips this check and then went on
to 916 productive updates. Read it together with the evaluation: if the score is not improving either, the rest
of the run is wall clock and the budget is better spent on the next stage.

### Controlling a run

| Goal | Command |
|---|---|
| Freeze everything, sim and learner | `forge pause`, later `forge resume` |
| Stop and keep the progress | `forge cancel` (saves `latest.pt`), later `forge resume` |
| Give up on the current stage and go to the next | `forge skip` |
| Continue a particular stage from its checkpoint | `forge resume stage10_gauntlet [stage16_companion ...]` |
| Retrain a finished stage | `forge start stage10_gauntlet`, which archives the old run |
| Fine-tune a stage from its own best (after reward or mask changes) | copy its `best.pt` to `runs/_finetune/<stage>/best.pt`, then `forge start <stage>`: the learner seeds from it before the seed chain (`finetune_from`) |

### After changing C++

```bash
docker compose exec ac-dev-server ./acore.sh compiler build     # incremental build into the shared volumes
docker compose restart ac-worldserver                          # the learner saves on stop
```

Or run `./forge.sh --build`, which recreates the container and builds before starting. The restarted sim is idle:

- `forge start` continues with the stages that haven't advanced yet, **from scratch**.
- `forge resume <stage>` continues a run from its `latest.pt`, **if the stage's shapes didn't change**. If a block,
  catalog or the class list changed, the learner refuses to resume. Start the stage fresh; it still seeds from its
  ancestors.

If layouts changed for a stage that earlier stages were trained on, those ancestors still seed block by block where
block sizes match. A block whose size changed raises an error during seeding, and the ancestor must be retrained.

## 7.5 When a stage ends at its budget

A stage never halts the plan: it advances when every class has converged, or at `total_env_steps` with reason
`budget`. The second case is the one to read.

1. Read `runs/<stage>/finished.json`: per class, whether it converged and which of `score`, `kl`, `entropy` and
   `ladder` it was still missing. `progress.json` carried the same while it ran (`weakest_layout`,
   `weakest_missing`), as does `forge status`.
2. Read `eval.jsonl` for per class, per-band and per-arena scores next to the `fight` baseline, and `layouts.csv`
   for each class's `entropy` and `approx_kl` over the run.
3. Decide:
   - **It was still learning.** A class missing `score` or `kl` at the budget wanted more steps: raise
     `total_env_steps` for that stage.
   - **Its ladder was still climbing.** A class missing `ladder` was still moving up the difficulty rungs, which is
     progress, not a fault; more budget, or a lower `Difficulty.MaxTier` if the top rungs are not wanted.
   - **The reward or scenario is wrong.** Change `AnimusForge.Curriculum.*` tuning or the code, check it with
     `forge fast <stage>`, then retrain.
4. `forge start <stage>` (and the stages after it) to train again; the earlier run is archived, not deleted.

## 7.6 Exporting and deploying models

1. **Export**, even during training:

   ```
   forge export stage17_party            # best.pt, else latest.pt
   forge export stage17_party latest
   ```

   Output goes to `AnimusForge.ModelDir` (default `modules/mod-animus-forge/models/`) as one `.amdl` and one `.json`
   per class, for example `warrior_tank_party.amdl` and `warrior_tank_party.json`. The export log is
   `animus-export.log`. "Export of stage17_party finished" appears in the console.

2. **Copy both files for every class** to the realm's `Animus.ModelDir` (default `<DataDir>/animus`).

3. **Configure the realm** (`mod_animus.conf`): set `Animus.Curriculum.Stage` to the stage whose models companions
   should play (`stage17_party`, or `stage27_crossroads` for PvE and PvP), and `Animus.Curriculum.DecisionMs` to the
   training decision interval.

4. **Load.** Models load on first use. On a running realm, `.reload config` resets the model cache.

5. **Verify** in game: `.animus summon human warrior tank`, then `.animus list`. A model that is refused shows the
   reason, and the log says `Animus model <name> not loaded: <reason>`.

The realm's animus-lib must build the same manifests. Use the animus-lib revision the forge trained with, and the same
world database and DBC data, because trainer spells and the spell catalog come from them.

## 7.7 Watching a stage in game

On a stock realm with mod-animus and the models (`.animus stage open` turns GM mode on for you):

```
.animus stage open stage8_duel model         # teleports you; the first episode spawns frozen
.animus stage spawn 6 warlock_dps 70         # a new episode, frozen: tier 6 (elite, +2 levels), a level 70 warlock
.animus stage start                          # play, episode after episode
.animus stage stop                           # freeze where it is
.animus stage status
.animus stage close
```

To see exactly the training conditions, copy the run's `stage.json` `"tuning"` values into `Animus.Curriculum.*`, and
match `Animus.Stage.DecisionMs`, `EpisodeSeconds`, `Level` and `SpawnPoint.*` to the forge settings. To look at one
situation of stage 8: `.animus stage open stage27_crossroads model ambush`. To compare with the baseline:
`.animus stage open stage17_party fight`.

## 7.8 Running the learner by hand

Useful for debugging the learner in an IDE:

1. Set `AnimusForge.Learner.AutoStart = 0` and restart the server.
2. `forge start stage8_duel`. The console prints the exact learner command to run.
3. From `python/`:

   ```bash
   .venv/bin/python -m animus.train --config configs/stage8_duel.yaml --run-name stage8_duel \
       --socket /tmp/animus-forge.sock --runs-dir <OutputDir>/runs --layouts-dir <OutputDir>/layouts
   ```

   A hand-started learner retries until the socket exists. Ctrl+C is safe: the learner saves, and the sim waits for the
   next learner and resets every env when one connects. Point `--runs-dir` and `--layouts-dir` at the sim's output, or
   the learner won't find `stage.json`.

**Tests:**

```bash
docker compose exec -w /azerothcore/modules/mod-animus-forge/python ac-dev-server .venv/bin/python -m pytest
```

**Standalone evaluation of a checkpoint** (the sim must be running the same scenario with no other learner attached):

```bash
python -m animus.evaluate --checkpoint runs/stage8_duel/best.pt --episodes 128 --seed 1000 --baseline fight
python -m animus.evaluate --checkpoint runs/stage12_pvp/best.pt --baseline fight --opponent-baseline
```

## 7.9 Extending the curriculum

Most changes alter layout manifests. **Any change to a block, a catalog rule, a stage's blocks or a character-building
rule that affects actions invalidates exported models and blocks `forge resume` of affected runs.** Plan to retrain from
the first affected stage.

Remember that animus-lib must still build on a stock core. Use only public APIs, or add a `CoreHooks` seam. Header names
must stay unique across the three modules.

### Change a reward weight or a chance

Set the key in `mod_animus_forge.conf` (`AnimusForge.Curriculum.Pulls.Interrupt = 0.5`). No rebuild is needed, only a
restart before the stage starts. The value is recorded in the run's `stage.json`. Rewards don't affect the manifest.

### Add a tuning value

1. Add a field with its default and comment to the right group in `CurriculumTuning.h`.
2. Add one `f("Group.Name", tuning.Group.Name);` line to `Visit`.
3. Document it in `animus-forge/conf/mod_animus_forge.conf.dist`, and optionally in `animus/conf/mod_animus.conf.dist`
   under `Animus.Curriculum.`.

### Add a reward term

1. Add it to `RewardTerm` (`Rewards/RewardLedger.h`) and to `RewardTermName` (`Rewards/CombatReward.cpp`).
2. Return it from the paying encounter's `RewardTerms()` and add it in `Reward()` with
   `ledger.Add(RewardTerm::X, value)`. Scale per-decision terms by `_scenario.DecisionScale()`.
3. It appears automatically as `reward_<name>` in episode info, TensorBoard and the learner's summaries.

### Add an observation feature or action to a block

1. Change the block's `Size`, `Observe` (and `Apply` for actions), and `DescribeManifest` if the feature depends on
   lists.
2. If it needs something the world can't provide directly, add a field to `SeatView` and fill it in the encounter's
   `View` (and in mod-animus's `CompanionParty::View` for companions).
3. Update `Baselines.cpp` if it reads a moved index.
4. Retrain every stage that has the block. Seeding treats a block whose size changed as incompatible, so retrain from
   the first stage that has it.

### Add a block

1. Add a `BlockId` (before `Count`) and its name in `BlockName` (`Layout/Layout.cpp`).
2. Implement `Block` in `Blocks/<Name>Block.{h,cpp}`: `Id`, `Size`, `Observe`, and optionally `BeforeApply`, `Apply`,
   `DescribeManifest`.
3. Register it in `Blocks/Blocks.cpp` (`GetBlock`).
4. Add it to the stages that need it. Seeding gives the new block zero input weights and small action weights in those
   stages. Other blocks keep their trained weights.

### Add an encounter

1. Declare it in `Encounters/Encounters.h` and implement it in `Encounters/<Name>Encounter.cpp`, overriding only the
   hooks it needs.
2. Decide what arena field selects it (possibly a new `ArenaDefinition` field and `ArenaProblem` rule), create it in the
   `StageScenario` constructor in the right build order, and add it to the reward order list and to `uses`.
3. Keep per-env state sized at construction. Give it `Deactivate` if it leaves anything in the world, so an arena switch
   removes it.
4. Put its tuning in `CurriculumTuning`. If it spawns scripted players, give their accounts a range in
   `BotAccounts.h`.

### Add a stage

1. Add a `StageDefinition` to `Stages/Stages.cpp`, after the stage it extends. Validation rules are in
   [4.1](04-curriculum.md#41-defining-a-stage).
2. Write `python/configs/<name>.yaml` with `extends: <parent>.yaml` and only what differs: `run_name`, budget, gamma,
   evaluation report, convergence, target, and `distill:` for a merge stage.
3. Leave `InDefaultQueue` true to add it to `forge start`, or false to train it only by name.
4. Check it: `forge run <name> fight 64`, then `forge fast <name>`.
5. For mod-animus, nothing else is needed. `.animus stage list` shows it and companions can use its models.

### Add a standalone scenario

Implement `Animus::Scenario`, create it in `CreateScenario` and list it in `ScenarioNames`
(`src/Scenario/Scenario.cpp`), and write a learner config. The protocol and learner are shape-generic. For more than one
agent per env, provide a real global `State`.

### Change a spec build

Edit `tools/spec_builds/builds.py`, run `validate.py`, then `generate.py` to rewrite `SpecBuilds.cpp`. After a rebuild,
`forge talents <class_role> [spec] [points] [plan]` prints the build a character gets at any point count, under any of
the three talent plans. Talent features change, so models of that class must be retrained.

## 7.10 Changing the forge core

Follow [chapter 2's rules](02-forge-core.md#21-rules-the-fork-follows): a `Forge*` replacement called from the top of
the original, no config gates, the upstream body left verbatim, and a header comment explaining what was dropped and
why. Build with `acore.sh compiler build` in the dev container. When a module needs a new core capability, add the API
to the core and a seam in `CoreHooks`, and install it from mod-animus-forge.

## 7.11 Performance

- **Measure it: `forge bench`.** It times the sim at every `AnimusForge.Bench.Threads` x `Envs` pair, then runs the
  fastest few again with the real learner (x `Bench.LearnerTorchThreads`), and reports the env steps per second of
  each. Trials run in `<OutputDir>/bench/` with evaluation, seeding and distillation off, so no real run is touched;
  the results are in `bench/bench.json`, and `forge bench apply` writes the winner in place: `MapUpdate.Threads` into
  `worldserver.conf`, `AnimusForge.Envs` and (when a torch thread count won) `AnimusForge.Learner.TorchThreads` into
  `mod_animus_forge.conf`, backing each file up as `<file>.before-bench`. The thread count takes effect when the
  worldserver restarts, the env count at the next `forge start`. `forge cancel` stops a sweep and restores the
  configured thread count.
- **Know which side is the bottleneck.** `forge status` shows env steps per second and where a decision's wall time
  goes: **world** (the map update, spread over `MapUpdate.Threads`; every env is its own instance), **sim** (this
  module's rewards, observations and actions, on the world thread, linear in envs) and **learner** (blocked on its
  actions and updates). In remote mode every decision is one Python round trip for all envs.
  - If the learner's forward pass dominates (high CPU in the learner process, the world thread idle waiting), fewer,
    larger batches help: raise `AnimusForge.Envs`.
  - If the world thread dominates, more map threads (`MapUpdate.Threads`) and fewer classes or simpler arenas help.
  - The learner's torch and the map update threads share the cores: `AnimusForge.Learner.TorchThreads` caps torch,
    and the benchmark sweeps both together.
- **Know which part of the sim.** The `sim parts` row splits that **sim** share into reward, observe (which carries
  the action mask), final observe (the same work without the mask, for an episode that just ended), reset (building
  the next episode's characters) and apply. Observe against final observe, per call, is the cheapest read on what
  mask building costs; reset against the episodes rebuilt per decision says whether episode turnover is worth
  attacking. Measure here before optimising the sim: the answer decides what is worth doing.
- **Characters are reused.** Building a character (race, level, spec, talents, gear) was 6.4 ms of a 24 ms decision
  at 0.96 episodes rebuilt per decision. `AnimusForge.Curriculum.Characters.ReuseEpisodes` (4) lets a seat keep its
  character when the next episode draws the same class and build: it is healed, cleared of buffs and cooldowns,
  restocked and moved to the new spawn, and the env keeps its level. The `sim parts` row shows the characters reused
  per decision beside the episodes rebuilt. Evaluations always build, so the yardstick is unchanged; a battleground
  episode always builds too. Set it to 0 to build every episode.
- **Three measurements, each one `forge fast` pair, decide three defaults.** Read them from `metrics.csv` and record
  the numbers here before flipping anything:
  1. *`overlap_updates` on the GPU*: a fast stage on vs off, comparing `rollout_seconds` (contention shows as a
     longer rollout), not `update_seconds`. If the rollout does not lengthen, `overlap_updates: true` in
     `stage8_duel.yaml` and `stage1_move.yaml` buys back the 36-42% below.
  2. *Take one trunk* (chapter 4, "The measurement that decides it"): one class's `stage8_duel` seeded from the
     shared root against a class-only movement chain; `eval.at_start` and the first three evaluations.
  3. *Character reuse* on vs off: `env_steps_per_sec`, the `reset` ms, and the first three evaluations of a fast
     duel, to see the policy does not overfit the fewer characters.
- **Envs are also a training setting.** One update is `rollout_length x envs x seats` env steps, so a different env
  count changes the batch PPO trains on, not only the speed. `forge bench` says so when its winner differs from the
  env count you train with.
- **Updates versus rollouts.** `update_seconds` in `metrics.csv` is time spent in PPO updates, which a GPU speeds up.
  Rollouts stay on the CPU on purpose. Serially that time is sim idle time: `env_steps_per_sec` in `metrics.csv` is the
  rollout's own rate, and the rate over the wall clock is lower by the update's share. `overlap_updates` runs the
  update on a worker thread while the sim collects the next rollout and closes most of that gap; the rollout then acts
  on the update before last, and update stats are logged one update late. It is **off**, and `stage8_duel` sets it
  off for the whole curriculum that extends it. The serial cost is real and large -- `stage1_move` is a 1.82 s update
  against a 3.2 s rollout, `stage15_stealth` 3.08 s against 4.26 s, so 36-42% of wall clock with the sim blocked in
  `ReceiveAny`, and the rollout being the longer of the two is the case overlap should hide completely. It was
  measured on this machine anyway and gained nothing: 5,365 against 5,323 env steps/s, inside the noise. Whatever
  the rollout's forward pass and the update contend for does not show up in the per-decision buckets. Do not
  re-enable it on the arithmetic alone.
  What would settle it: that measurement was taken at a 1.1 s update against a 2 s rollout -- the same ratio as
  today, but updates now run on the GPU, and `forge bench` moves learner time by 0.1% between `torch_threads` 0 and
  8, which is what a GPU-bound update looks like. One A/B on a fast stage would say. Compare `rollout_seconds`, not
  `update_seconds`: contention shows up as a longer rollout.
- **Evaluation cost.** Every evaluation resets all envs and runs `eval.episodes` seeded episodes plus confirmation
  episodes. Large evaluations every few million steps can take a significant share of wall time. `eval.every_env_steps`
  and `eval.episodes` trade that time against the reliability of convergence decisions -- but the trade is cheap in the
  curriculum stages: an evaluation is seconds of sim time against tens of minutes of training, while its noise sets the
  convergence margin and the per (class, role) gates. Too few episodes is the more common mistake.
- **Asset builds** take a few seconds per class at the start of each stage (trainer data and item pools). This is
  expected.
- **Memory.** Instances are created once and reused. Bots reuse two GUIDs per slot. Steadily growing memory during a run
  points to a leak worth investigating (a new per-GUID core cache, or instances not unloading).

## 7.12 Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Server exits at once in Docker | The console read end of file. Run through `forge.sh`/Compose, which gives it a TTY. A server without a TTY skips the console and keeps running |
| Every `AnimusForge.*` (or `Animus.*`) key logs "Missing property" | The module's `.conf` doesn't exist: AzerothCore no longer reads a module's `.dist`. Installing creates it when missing (Docker copies it to the config volume on start); for an install that predates that, copy it from the `.dist` |
| "The world ticks N ms, but AnimusForge.DecisionMs M over TicksPerDecision T wants X ms" | The worldserver and the module disagree about the split, because it was built before this, or because the conf changed without restarting both. Run `./forge.sh --build` |
| Configure fails: "mod-animus-forge needs mod-animus-lib, which is disabled" or linkage mismatch | Build both the same way. Set the named variable to `static` or `dynamic` |
| Configure fails: "mod-animus and mod-animus-forge each carry their own copy of the curriculum" | Enable one, not both: `-DMODULE_MOD-ANIMUS=disabled` on a forge core |
| "Learner directory ... does not contain animus/train.py" | The worldserver runs from a baked image, or the module moved. Set `AnimusForge.Learner.WorkDir` |
| "waiting for learner" forever | Auto-start failed (see the server log and `animus-learner.log`), or `AutoStart = 0`. Run the printed command by hand |
| The learner exits right after connecting | Config error (unknown key, wrong type), target validation (a gate on a missing metric), or a resume mismatch. See `animus-learner.log` |
| "cannot resume ...: the scenario's layouts changed" | Shapes changed since the checkpoint. Use `forge start <stage>` instead |
| "trunk.…: the trunk in the checkpoint does not match (hidden sizes must be equal)" | The stage's `mappo.hidden` differs from the ancestor's. Keep `[256, 512, 512]` across stages |
| A stage always starts "from scratch" | Its ancestors have no `best.pt`/`latest.pt` in this `runs/`. Train them first, or move old runs from `modules/mod-animus-forge/python/runs/` to `var/animus-forge/runs/` |
| "Stage X is left out: ..." at startup | A definition broke a validation rule (4.1). Fix `Stages.cpp` |
| Evaluation "stopped after N decisions with k of M episodes" | Episodes are longer than `SPEC.EpisodeSeconds` suggests, or envs are stuck rebuilding. Check for "could not build its episode" errors |
| "env N could not build its episode" repeated | Character or encounter build failures, usually spawn point or map problems, or missing world data. Check `AnimusForge.SpawnPoint.*` and the log lines before it |
| Async query queue or memory keeps growing | A database write on a bot path. Check that the core has sim sessions and groups (2.8) and look at sync-query warnings |
| Realm refuses a model: "its manifest differs" | Different animus-lib revision, world database or DBC data than training. Retrain with the realm's, or align versions |
| Realm: "no layout manifest ... beside the model" | Copy the `.json` next to the `.amdl`. The CMake install step copies only `.amdl` |
| Companion only follows you | Its model is missing or refused (`.animus list`), or it has no target and can't act without one |
| Stage viewer: "you are not in an instance of its map" | `Animus.Stage.SpawnPoint.MapId` isn't instanceable, or the teleport failed within 60 s |
| Not enough detail in the log | Set `Logger.module.animus=1,Console Server` in `worldserver.conf` for debug-level Animus logging (both modules and animus-lib log under `module.animus`) |
