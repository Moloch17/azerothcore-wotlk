# 1. Overview

## The goal

Train bots that play World of Warcraft 3.3.5a class characters the way a player does: seeing only what a player sees,
moving only with held keys and turn rates, acting only through the packets a client sends. One policy per class
(ten classes), each covering every talent build of its class, trained with MAPPO (multi-agent PPO with a centralised
critic that may see privileged state; the policy may not). The rules this follows, and why, are in
[principles.md](principles.md).

Trained policies are exported as `.amdl` models for a separate realm module (mod-animus) that is **not in this
repository**; see [06-animus.md](06-animus.md).

## The pieces

```
 worldserver (C++, the forge core + src/server/game/Animus)  <-- Unix socket / TCP, lock-step -->  learner (Python)
        ^ console, SOAP, forgectl                                                                   apps/forge/python
        | cluster control (host <-> workers)                                              runs/<stage>/ , best.pt, latest.pt
```

- **The forge core**: AzerothCore edited to run headless at a fixed tick with no clients, no bot persistence and a
  sealed database. [reference/01-forge-core-delta.md](reference/01-forge-core-delta.md).
- **The sim** (`src/server/game/Animus`): envs (each an instance map with its bots), scenarios built from stage
  definitions, observation/action blocks, encounters, the player controller, the camera, rewards, the learner bridge,
  plans and the console. [reference/00-architecture.md](reference/00-architecture.md) and the `cpp-*` documents.
- **The learner** (`apps/forge/python/animus`): rollouts, MAPPO updates, seeded evaluation, convergence, seeding from
  earlier stages, export. `py-*` documents.
- **Operations**: `forge.sh`, `forgectl`, the cluster. [07-operations.md](07-operations.md).

## Core ideas

- **Env**: one independent copy of a training situation, normally its own instance map holding the seats and what they
  face. An **env pool** holds all envs of a stage and the flat arrays a STEP carries.
- **Seat**: one learned agent in an env (1 to 40 per env). Each episode every seat becomes a new character (race, level,
  build, talents, gear, supplies).
- **Layout**: the exact observation vector and action list a class gets in a stage, built from the stage's **blocks**.
  A model only works with the layout it was trained on (its manifest records it).
- **Stage**: a training scenario defined in `Stages.cpp`; it extends an earlier stage and seeds from its checkpoint by
  name. **Arena**: one situation a stage's episodes can be, drawn by weight.
- **Decision**: one step of the env, `AnimusForge.DecisionMs` of game time (default 250). **Tick**: one world update
  (`DecisionMs / TicksPerDecision`).
- **Lock-step**: the world thread sends every env's observation and blocks until the actions come back, answering the
  console meanwhile.
- **Convergence, not pass gates**: a stage ends when its classes' convergence signals say so (or at a step ceiling);
  the learner exits 0 either way and the plan moves on.

## The ten stages (default queue, in order)

`move1_controls`, `move2_seek`, `move3_interact`, `move4_follow`, `combat1_fight`, `combat2_packs`, `combat3_survive`,
`group1_roles`, `dungeon2_ragefire`, `dungeon3_deadmines` (order from `Stages.cpp`). Seeding parents (`Extends`):
move2 from move1; move3 and move4 from move2; combat1 from move3; then each combat/group/dungeon stage from the
previous one; `group1_roles` also merges `move4_follow`. One-line summaries of each are the `Summary` fields in `Stages.cpp`; the full descriptions are in
[reference/stages.md](reference/stages.md). The first curriculum (v1) was deleted on 2026-10-07 (git tags
`curriculum-v1` and `pre-cleanup-2026-10-07`).

## Life of a stage

1. `forge start <stage>` (console) or `forgectl stage start <stage>`.
2. The sim builds the stage, writes `layouts/<stage>/` (manifests, `stage.json`), builds the envs and starts the learner.
3. The learner seeds its networks from the parent stage (by name), trains, evaluates on seeded episodes every
   `eval.every_env_steps`, and decides when to stop.
4. It saves `latest.pt`, writes `finished.json`, exits 0; the plan moves to the next stage.
5. `forge export <stage>` writes models for the realm module.

The same, traced through the code: [reference/00-architecture.md](reference/00-architecture.md) section 5.

## Not in the current design

No scripted teachers, scripted baselines or scripted players exist; the only non-learned policy is `random`. The
stand-in "human" partner in party stages is a frozen learned policy from the partner pool, absent when the pool is empty.
(M4's follow leader, the owner slot, is still scripted.) Roles (tank/healer/damage) are not asked for: a stage asks for
an aptitude and the build decides. The first curriculum's director, companions and PvP stages are gone.
