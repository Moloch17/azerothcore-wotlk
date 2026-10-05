# The archived curriculum (curriculum v1)

These are the learner configs of the first curriculum, `stage1_move` ... `stage21_ship` (movement, classes, parties
and raids, PvP, life, ship), archived on 2026-10-05 when the curriculum was rebuilt from scratch around the player
controller (`.agents/plans/movement-curriculum/`, `.agents/plans/player-controller/`).

- **The stage definitions** (Stages.cpp: arenas, blocks, spawn and held-out ground) are on the git tag
  `curriculum-v1` (local; `git show curriculum-v1:src/server/game/Animus/Scenario/Curriculum/Stages/Stages.cpp`).
  The tag is the commit before the archive, so the whole curriculum builds and trains from it as it was.
- **The training runs** are archived in `var/animus-forge/shared/archive/curriculum-v1-2026-10-05/`.
- **These configs** are kept as they were, in the same layout (the per-class directories included), so every
  `extends:` chain still resolves inside this directory and `TrainConfig.load("configs/archive/stage4_duel.yaml")`
  still loads. Nothing reads them: the sim looks for `configs/<scenario>.yaml` (or `configs/<class>/<scenario>.yaml`)
  and never in here, and no stage of that name exists any more.

`stage4_duel.yaml` was the hyperparameter base every other stage extended (networks, PPO, evaluation, convergence,
the shaping fade). The movement stages need a base of their own; this file is where to start one from.

Do not train from here or add to it. A new stage gets a new config in `configs/`.
