# Reference: glossary

Purpose and scope: every term the forge's code, configs and docs use as a word of its own, in one or two sentences, with
a pointer to where it lives. Alphabetical. `UNVERIFIED` marks a definition not checked in the code. Longer treatments:
[00-architecture.md](00-architecture.md), [stages.md](stages.md), [cpp-runtime.md](cpp-runtime.md),
[cpp-blocks.md](cpp-blocks.md), [cpp-encounters.md](cpp-encounters.md), [py-learner.md](py-learner.md),
[py-mappo.md](py-mappo.md), [protocol.md](protocol.md), [tools-and-ops.md](tools-and-ops.md).

This file maps itself: it has no source files of its own; it summarises those named in each entry.

| Term | Definition and where it lives |
|---|---|
| **Action catalog** | A class's list of pressable actions (spells and others) built from its spell data; `Character/ActionCatalog.cpp`. The core block lays them out; a changed catalog is seeded by action name. |
| **Aimless** | A Cost paid for a press that works against the goal the seat itself holds; judged by `StageScenario::JudgePress`, priced in `Actions.Aimless*`. |
| **Aptitude** | What a class build is geared to play (tank, healer, damage), read off its spec; `Character/Aptitude.cpp`. A role is never chosen by the caller, it is read off the build. |
| **Arena** | One situation a stage's episodes can be (a hallway, a pack drill, a dungeon wing), drawn by weight each episode; `ArenaDefinition` in `Stages/StageDefinition.h`. A drill is an arena, not a stage. |
| **Arm** | An extra evaluation beside the plain "all bots" one: `with_human` (the stand-in in one seat) or `with_partners`; `eval.arms`, `animus/config.py`. Never moves best.pt or convergence. |
| **Best / latest** | `best.pt` is the checkpoint with the best evaluation score (kept per gate-stepped rung as `best_rung<k>.pt`); `latest.pt` is the newest and is what a later stage is seeded from (`seed_from: latest`). |
| **Block** | A named slice of a layout with its own observation columns and action rows (core, move, vision, sight, ...); `Layout/Block.h`, ids never renumbered. Seeding carries a block by name; a change in meaning bumps its revision. |
| **BlockId** | The explicit numeric id of a block (core 0 to goal 26, with gaps); `Layout/Block.h`. |
| **Bot / session-less bot** | A server-side player character with no client or database row, made per seat per episode; `Bot/BotFactory.cpp`. |
| **Camera / canonical image** | The seat's third-person ray-cast view; every frame is scaled by nearest pixel to the canonical image of `Vision.Height` x `Width` (128 x 64 by default; the cast size varies per episode), four bytes a pixel, the static world only (the entity list, found by line of sight, carries who is in view); `Blocks/VisionBlock.h`. |
| **Cast** | Frozen checkpoints placed in seats a stage declares (`stage.json` `cast`, `cast.agents`), and the co-op partner pool config `cast.partners`; `animus/cast.py`, `animus/partners.py`. |
| **Class table** | The table of classes, either the ten model classes (`ClassProfile`) or the camera's semantic classes (`Vision::Class`, `CLASS_LIMIT`); principles say classes only append. `UNVERIFIED`: which one the principle means; both are append-only in practice (`LiveLayoutPin.golden.inc` lists the model classes). |
| **Clock / tick / decision** | A *tick* is one world update (50 ms in every live stage); a *decision* is one policy step, `AnimusForge.DecisionMs` = 250 ms, here five ticks (`Stage.<name>.TicksPerDecision`). |
| **Cluster fingerprint** | A hash a worker sends the host at registration (source, protocol version, decision timing, curriculum tuning); a mismatch is refused; `Bridge/ClusterLink.cpp`. |
| **Collapse alarm / stall warning** | Warnings of a gate-stepped ladder: collapse = gate metric under a floor for three evaluations; stall = no improvement beyond the standard error for `stall_evals` and `stall_env_steps`; `animus/stage.py`. They never act. |
| **Combat block** | Perception-true combat inputs: the player and pet frames, the target frame's threat and the sight list's per-target combat columns; `Blocks/CombatBlock.h`. |
| **Config chain** | `extends:` at the top of a stage yaml: which file's settings it inherits (not the seed chain); `animus/config.py` `load_yaml`. |
| **Controller (player controller)** | The only way a seat moves: held keys and turn rates run through a client's movement physics and reported as a client would; `Movement/PlayerController.cpp`. |
| **Convergence** | The one rule that ends a stage: every class has a plateaued score, a quiet policy (KL), settled entropy, a settled ladder and (ladder stages) the top rung; `animus/stage.py` `ConvergenceController`. |
| **Cost (reward category)** | An Outcome-side price: deaths, the clock, noise prices; counted in the score at full price; `RewardCategory::Cost`, `Rewards/RewardLedger.h`. |
| **Cost ladder** | The ladder that scales the noise prices (Repeat, Jitter, Aimless, Effort, Fidget, Stuck, Wall) by rung; `costs:` in the yaml, `CostLadder` in `animus/stage.py`. Off in all twelve stages. |
| **Critic / critic state** | The value network's input, which may see privileged state (`STATE_DIM` 1958); the policy may not; `StageScenario::WriteState`. |
| **Decision** | See Clock. Also the unit all per-decision reward terms are scaled to (`DecisionScale` = DecisionMs / 50). |
| **Drill** | An arena that trains one lesson (a role, a pull) on a stage's ground; weighted in a stage, never a stage of its own. |
| **Encounter** | The object that runs an arena's episode: builds the situation, pays rewards, says when it ends; `Encounters/*.cpp` (Sight, Seek, Interact, PartyFollow, Combat, Roles, Instance, Party). |
| **Entities block / entity list** | The list of entities the camera's last frame showed, with class, level, health and where; `Blocks/EntitiesBlock.h`. Inserted automatically after `Vision`. |
| **Entity memory** | A seat's session-long record of entities it saw (last seen place and state); written by the entities block, read by the sight block and goal places; `Layout/SeatMemory.cpp`, `Encounters/SeenPlaces.h`. |
| **Env** | One independent copy of a training situation (its own instance), updated on a map thread; an *env pool* holds all of a scenario's envs; `Env/EnvPool.h`. |
| **Episode** | One run of an arena from reset to its end (clock, boss dead, wipe limit, corridor done). |
| **Evaluation** | A seeded, argmax-action play of the stage at set intervals; the same characters and layouts every time; `animus/evaluation.py`. Not training data. |
| **Extends / Merges** | `StageDefinition.Extends` is the stage whose checkpoint seeds this one (the seed chain); `Merges` seeds the blocks only those stages have; `Stages.cpp`. |
| **Fade (shaping ladder)** | The ladder that multiplies every Shaping reward by the rung's scale (default rungs 1, 0.5, 0.25, 0); in M1 to M4 and the dungeon stages the sim also reads it as the difficulty rung; `FadeConfig`, `ShapingFade`. |
| **Free look** | The seat's camera is turned by the seat itself (a three-headed look policy output), separate from the body's facing; `Vision/FreeLook.h`. |
| **Gate / gate metric** | The evaluation column a ladder waits for before stepping (`fade.gate_metric`, `gate_value`); a *gate-stepped* ladder has `require_plateau: false`. Not a pass gate. |
| **Go-Explore** | In D2 and D3 a share (`explore.share` 0.5) of training runs start from a cell an earlier run reached; evaluations start at the door; `animus/explore.py`. |
| **Goal / goal places** | The goal head's current objective kind and target; in a dungeon the places it can name are only what the seat discovered ([decision 0005](../decisions/0005-goal-places-seen-only.md)); `Blocks/GoalBlock.h`, `SeenPlaces.h`. |
| **Heldout** | An arena marked `EvalOnly` (Wailing Caverns, M2's and M3's `sweep`): never drawn in training, played only by `eval.heldout`; a reading, not a target. |
| **Host / worker** | In a cluster the host runs the run and holds the checkpoints; workers add envs and (with `Cluster.Learner = auto`) their own learner, exchanging weights; `docs/forge/cluster.md`. |
| **Layout** | The exact observation vector and action list of one class at one stage, built by placing the stage's blocks in order; `Layout/Layout.cpp`. Pinned by `LiveLayoutPinTest`. |
| **Ledger (reward ledger)** | A seat's per-decision reward total and per-term episode sums, with scales for shaping and noise; `Rewards/RewardLedger.h`. |
| **Lock-step** | The world thread sends every env's observations to the learner and blocks until actions return; `Bridge/LockstepServer.cpp`. |
| **MAPPO** | Multi-agent PPO with a centralised critic, the learner's algorithm; `animus/mappo/`. |
| **Manifest** | The JSON describing a layout's meaning; a model works only where the same manifest is built. |
| **Mental map** | A seat's egocentric, heading-up 48 x 48 crop at 2 yd of what its own camera and body have written (six bytes a cell, including a frontier); `Blocks/MapBlock.h`, `Vision/MentalMap.h`. |
| **Opposition** | What an arena's seats face and which encounter runs it (`Opposition::Sight`, `Seek`, `Interact`, `PartyFollow`, `Combat`, `Roles`, `Instance`); `StageDefinition.h`. |
| **Outcome (reward category)** | What a stage is for (kill, clear, arrive, drill lesson, ready pull); the score includes it at full price; never faded. |
| **Overlay** | A yaml merged over a stage's whole config (`configs/fast.yaml` for `forge fast`). |
| **Partner pool** | The learner's set of frozen policies (earlier stages' checkpoints and this run's snapshots) that fill party seats; `animus/partners.py`, `cast.partners`. |
| **Plan** | The list of stages the forge will train in order (the queue); `forge start` with names makes one; "Plan ended: cancelled" is its log line; `AnimusForge.cpp`. |
| **Policy / model** | One network per class, covering every build the class has; trained over a shared trunk and exported as one `.amdl` per class. |
| **Probe** | A training run on a dungeon wing (`Instance.WingProbe` = 20% of runs, never a Go-Explore start) whose progress alone steps the wing ladder; `Encounters/WingLadder.h`. |
| **Pull drill** | D1's arena: one pack a run, the party started 35 yd back along the route with the packs before it cleared; `ArenaDefinition::PullDrill`. |
| **Revision** | A block's version number, bumped when its columns change meaning in place; seeding treats a different revision by name. |
| **Rung** | One step of a ladder: a fade rung is a scale; a sim rung is a difficulty (placement, site kind, leader behaviour, tier, wing lift). |
| **Sealed pool** | After startup the database connection pools drop writes (logged once a kind) so a forge run never persists; MySQL is read only at startup; commit 8438fea02. |
| **Score (`score_outcome`)** | The episode's Outcome plus Cost terms at full price, before any tier or role scale; what best.pt and convergence follow when `eval.score: outcome`. |
| **Seat** | One learned agent in an env; a stage has 1 to 5 per env. Each episode a seat becomes a new random character of its layout's class. |
| **Seed (two meanings)** | An *evaluation seed* fixes the characters, arena and spawns of episode i (`eval.seed` 1000); a *seed* of a stage's networks is the checkpoint it starts from. |
| **Seed chain** | The `Extends` line from a stage back to the first, written to `stage.json` `seed_chain`; the learner seeds from the first that exists. |
| **Seeding by name** | Copying a checkpoint into a different layout block by block and column by column using names, not positions; `animus/bootstrap.py`. |
| **Shaping (reward category)** | An aid toward the outcome (progress, facing, sighting, damage dealt); multiplied by the fade's scale; not in the score. |
| **Sight block** | What the seat sees and remembers as one list plus pointer presses (select, interact, use an item on, assist, focus) sent as a client sends them; `Blocks/SightBlock.h`. |
| **Stage** | A scenario the learner trains (`combat2_packs`): blocks, arenas, ground and a seed; `StageDefinition`. |
| **Stage.json** | The sim's description of a stage written to `layouts/<stage>/` and copied into the run: blocks, layouts, seed chain, tuning, columns; `animus/stages.py`. |
| **Stand-in ("human")** | A frozen learned partner placed in one seat of a share of party training runs to stand for a human (leads or follows, any role); never trained on; absent when the pool is empty; [decision 0017](../decisions/0017-human-stand-in-is-a-frozen-learned-partner.md). |
| **Status headline / targets** | The measures `forge status` shows for a stage and the bounds it is judged against; a readout, never a gate; `StatusConfig`. |
| **Step (env step)** | One decision of one seat in one env; budgets and evaluation intervals are in env steps. |
| **Tier** | The per-class-and-build difficulty rung of the combat and roles drills (`DifficultyLadder`, 0 to `MaxTier` 5); outcome terms scale by w = 1 + `TierScale` x tier. |
| **Update** | One PPO update of the learner over a rollout. |
| **Wing** | A real dungeon's route from its door to its last boss (`InstanceLadder::Wing`, rows: Ragefire, Deadmines, Wailing Caverns); `Encounters/InstanceBosses.cpp`. |
| **Wing ladder** | The sim's nine-rung whole-dungeon ladder (level lift 8 down to 0, spare wipes 4 down to 0), stepped by probes alone; `StageScenario.h` `WING_RUNGS`, `WingLadder.h`. |
| **Wipe** | The whole party down at once; scored; `Instance.WingWipes` (2) of them end a run. |
