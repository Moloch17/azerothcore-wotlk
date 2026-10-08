# 0010: Ladders step on their gate, never on a plateau, never back on a score

**Date:** 2026-10-07

**Decision.** A gate-stepped ladder (`fade.require_plateau: false`) steps forward as soon as its gate metric is met at one evaluation. It never steps back on the score; it warns instead (collapse alarm, stall warning). The sim's wing ladder likewise steps down on its probes only and raises a collapse alarm.

**Reason.** The M2 incident: a regression rule read every harder rung as a failure; M2 fell back from the doorway twice and was held at the hallway from about 80M steps (commit 099107507). M1 had waited about 20M steps on a plateau with its gate long met.

**What it constrains.** A harder rung scores lower by design, so a score-based step-back is invalid on difficulty ladders. NOTE: the sim's per-class `DifficultyLadder` (C1-C3, G1) still moves a class down below a 60% win rate (`Encounters/DifficultyLadder.cpp:93`); see known-issues.

See also [principles.md](../principles.md) and the [index](README.md).
