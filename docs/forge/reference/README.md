# Reference documents: index and how to use them

Written 2026-10-07/08 against `forge` at about ce42b61b3 (after the cleanup of the first curriculum). They are meant for a
person reviewing and refactoring the forge without an assistant.

## How far to trust them

- Every non-obvious claim cites `path:line`. The line numbers will drift as the code changes; the file and the function
  name will not.
- Claims that could not be checked are marked `UNVERIFIED: <what to check>`. There are many (several hundred across the
  documents); they are the places to read the code yourself first.
- Each document ends with "Observed issues" and "Reviewer notes". `known-issues.md` collects and ranks the main ones.
- The documents were written by assistants reading the code, not by running it. Where a document and the code
  disagree, the code is right: fix the document.
- There are no tests (see `tests.md`): nothing will tell you a document went stale. `config-keys.md` has a generated
  table (`python3 apps/forge/tools/gen_config_reference.py --check` tells you if it is out of date).

## Suggested reading order for a review from start to finish

1. `../principles.md`, then `glossary.md`: the rules and the vocabulary.
2. `00-architecture.md`: the whole system on one page, then in depth. `01-forge-core-delta.md`: how the fork differs
   from upstream AzerothCore.
3. The C++ sim, from the outside in: `cpp-runtime.md` (and its `-bridge`, `-env`, `-console`, `-config`,
   `-process-gpu` parts), `cpp-stagescenario.md`, `cpp-encounters.md`, `cpp-blocks.md`, `cpp-layout-character.md`,
   `cpp-rewards-routing.md`, `cpp-movement.md`, `cpp-vision.md` (`-memory`, `-video`), `cpp-tuning-keys.md`.
4. The wire: `protocol.md`, `file-formats.md`, `metrics.md`.
5. The learner: `py-learner.md` (and `-train`, `-stage`, `-config`, `-seeding`, `-export-tools`), `py-mappo.md` (and
   `-networks`, `-trainer`, `-buffer`), `py-human-and-misc.md`.
6. The stages: `stages.md` and `../04-curriculum.md`; the decisions in `../decisions/`.
7. Operations: `config-keys.md`, `config-yaml.md`, `tools-and-ops.md`, then `../cluster.md`, `../forgectl.md`,
   `../deploy-gate.md`.
8. Last: `known-issues.md` (the ranked list) and `../audit/SUMMARY.md` (the consolidated, ranked findings and the owner's
   decisions from the efficiency, operator-experience and repository-hygiene audits, which sit beside it).

## The documents

| File | What it covers |
|---|---|
| `00-architecture.md` | processes, tick and decision loop, observation and action path, stage lifecycle, cluster, threading, state |
| `01-forge-core-delta.md` | every upstream AzerothCore file the fork changes, why, and the merge risk (measured against the merge-base 37de65eb0) |
| `cpp-runtime.md` | the forge state machine, plan runner, threading and ownership; the entry point for the runtime |
| `cpp-runtime-bridge.md` | the lock-step server, wire messages in the sim, the cluster link and fingerprint |
| `cpp-runtime-env.md` | the env pool, decision phases, resets, how bots and seats are made |
| `cpp-runtime-console.md` | every `forge` console command with arguments and handler, the status report |
| `cpp-runtime-config.md` | every key `ForgeConfig::Load` and the main program read |
| `cpp-runtime-process-gpu.md` | `ForgeMain`, `ForgeCore`, the learner launcher, the CMake source hash, the `Gpu/` layer |
| `cpp-movement.md` | the player controller: held keys, stepping, floors, water, the client link, capture |
| `cpp-vision.md` | the camera and its pixel format, ray casting, render sizes, free look, classes, GPU parity |
| `cpp-vision-memory.md` | the mental map and the entity memory |
| `cpp-vision-video.md` | frame images and the evaluation videos |
| `cpp-blocks.md` | the 14 live blocks: columns, actions, masks, revisions; the stage-by-block table |
| `cpp-layout-character.md` | layout building, the manifest, the seat encoder; character building (class, talents, gear) |
| `cpp-rewards-routing.md` | the reward ledger and all reward terms; the fade and cost ladders (sim side); route and field data |
| `cpp-encounters.md` | every encounter: Sight, Seek, Interact, PartyFollow, Combat, Roles, Party, StandIn, Instance, the wing ladder, respawn |
| `cpp-stagescenario.md` | `Stages.cpp` field by field, `StageScenario.cpp` by line range, the `Scenario` interface and helpers |
| `cpp-tuning-keys.md` | all `AnimusForge.Curriculum.*` keys: defaults, ranges, readers, live stages |
| `protocol.md` | the version 25 wire protocol byte by byte, the cluster messages, the version history |
| `file-formats.md` | stage.json, progress.json, metrics.csv, evaluation files, checkpoints, `.amdl`, probe data, run directory layout |
| `metrics.md` | every episode-info and reward column, derived measures, how evaluation tables are built |
| `py-learner.md` | the map of the learner's top-level modules (index) |
| `py-learner-train.md` | `TrainingRun`: the loop, rollout, update, evaluation, checkpoints; the wire client |
| `py-learner-stage.md` | convergence, the ladders, re-baselining, the alarms, schedules |
| `py-learner-config.md` | every config dataclass, yaml `extends` and overlays |
| `py-learner-seeding.md` | seeding by name, merges, the partner pool, cast, distillation |
| `py-learner-export-tools.md` | `.amdl` export, `evaluate.py`, `bench_learner.py`, run logging |
| `py-mappo.md` | how the networks are built from stage.json; the parameter inventory; shapes |
| `py-mappo-networks.md` | every network class |
| `py-mappo-trainer.md` | the MAPPO trainer: losses, rollout graphs, schedules, save and load |
| `py-mappo-buffer.md` | the rollout buffer, value normalisation |
| `py-human-and-misc.md` | the human-capture and parity tools; the smaller learner modules |
| `config-keys.md` | every non-Curriculum `AnimusForge.*` key (a generated table) |
| `config-yaml.md` | every learner yaml field and the 12 live yamls' inheritance |
| `tools-and-ops.md` | `apps/forge/tools`, forgectl internals, compose and docker, the repository layout |
| `stages.md` | the 12 live stages, one section each, with the seed chain |
| `known-issues.md` | the consolidated, ranked list of bugs, debts and refactor candidates |
| `glossary.md` | the project's terms |
| `tests.md` | there are no tests; what the removed suites guarded; how to check by hand |
