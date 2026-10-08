# 6. Animus (the realm module): where this repository ends

`mod-animus` (repository `Moloch17/animus`) is a **separate** AzerothCore module that plays exported models on an
ordinary realm with real clients. **Its source is not in this repository's tree** (`ls modules/` shows only
`CMakeLists.txt`, `create_module.sh`, `how_to_make_a_module.md`, `ModulesLoader.cpp.in.cmake`, `ModulesPCH.h`,
`ModulesScriptLoader.h`). Nothing about its internals can be verified here. The earlier version of this chapter
described its commands, addon, companion parties, stage viewer and install steps from the module's own source; none
of that is reproduced, because it cannot be checked from this tree. If you need it, read that repository.

## The boundary

The contract between this repository and the module is **the files `forge export` writes**:

- `forge export [stage] [best|latest]` (console, `ForgeCommands.cpp:1059-1143`) picks `runs/<stage>/best.pt` (or
  `latest.pt`, or the one named) and starts `python -m animus.export --checkpoint <file> --out <AnimusForge.ModelDir>
  --layouts-dir <OutputDir>/layouts`, logging to the export log beside the learner's. The message says "Models stay in the
  forge's folder: copy them to a game server by hand." Nothing is installed anywhere automatically.
- Per class, a model file `<model name>.amdl` and the layout manifest `<model name>.json`
  (written by the sim when the stage is built, `StageScenario::WriteStageFiles`, `StageScenario.cpp:1240`; copied
  beside the model by `animus.export`). Model names come from the stage's `stage.json` `models` map.
- The `.amdl` format is documented in the header of `apps/forge/python/animus/export.py`; the current
  `AMDL_VERSION` is 9 (`export.py:128`). Field-level description: [reference/file-formats.md](reference/file-formats.md).

A realm that loads a model must build the same layout manifest as the forge did for that stage and class; the manifest
is how a model is refused when the blocks differ. (That the module compares manifests is a claim from the old
chapter: UNVERIFIED here.)

## What the repository still carries for the module

- `apps/forge/patches/mod-animus-amdl8.patch`: a patch for `modules/mod-animus` adding a reader for `.amdl` versions 8
  and 9 (seat sets and attention) and golden-logit checks. It targets files under `animus-lib/src/runtime/...`
  (`SupportBlock.h`, `HostilesBlock.cpp`, ... ) that do not exist in the forge's current block set, so it applies
  only to a module checkout of that older shape. UNVERIFIED whether it applies to the current mod-animus.
- `apps/forge/patches/amdl8-check/`: `prep.py`, `run.py`, `bench.py`, `golden.cpp` to compare that reader's logits with
  the learner's (its README).

## Stale claims removed

The old chapter said the committed models were `.amdl` version 1 and refused by a version-2 reader. The current writer
is version 9; that sentence is obsolete. Class companions, the Animus addon, the stage viewer and the module's cost
figures are UNVERIFIED and not repeated. Nothing is deployed to a realm without the owner saying so (principle 19).
