"""Hint imitation switched off once the probes beat the script, per rung (dungeon-curriculum I6).

The dungeon teacher's hints (the sim's hint block, mappo.hint_coef) are a support that has to end: the first taught
Deadmines run imitated a script that stood still, and its bots stood still alone (2026-10-01). The sim decides where --
the first ladder rung whose probes (runs the policy plays alone) clear more of the dungeon than that rung's reference
runs (the teacher playing every seat there): StageScenario::WingHintOffRung, the `HINTOFF r` of its RUNG broadcast --
and writes no hint at that rung or any later one.

This is the learner's half, so the cutoff holds whatever the config asks: every ended training run of a whole dungeon
says the cutoff rung as the sim has it (`wing_hint_off_rung`: -1 none, -2 a run that is no whole dungeon's training
run) and its own rung (`wing_rung`). hint_coef is 0 exactly while the latest such run's rung is at or past the cutoff
-- the imitation loss is not computed at all, for hints and for the rows the teacher played alike -- and comes back if
the ladder falls back below it.
"""

from __future__ import annotations

import numpy as np

COLUMN = "wing_hint_off_rung"
RUNG = "wing_rung"


class HintCutoff:
    """Follows the sim's cutoff rung and the runs' rung on the ended training episodes; holds `config.hint_coef` at 0
    while the rung is at or past the cutoff."""

    def __init__(self, info_names, config) -> None:
        names = list(info_names)
        self.column = names.index(COLUMN) if COLUMN in names else None
        self.rung_column = names.index(RUNG) if RUNG in names else None
        self.config = config
        self.configured = float(config.hint_coef)
        self.off = False
        self.cutoff = -1
        self.rung = 0

    @property
    def active(self) -> bool:
        """Whether there is anything to cut: a stage that reports the cutoff and a config that imitates."""
        return self.column is not None and self.rung_column is not None and self.configured > 0.0

    def observe(self, ended: np.ndarray | None) -> bool:
        """The ended episodes' info rows ([N, K], the policy's own): whether imitation was switched off or on now."""
        if not self.active or ended is None or len(ended) == 0:
            return False
        rows = np.asarray(ended)
        wing = rows[:, self.column] > -1.5
        if not bool(wing.any()):
            return False
        latest = rows[np.flatnonzero(wing)[-1]]
        self.cutoff = int(round(float(latest[self.column])))
        self.rung = int(round(float(latest[self.rung_column])))
        off = self.cutoff >= 0 and self.rung >= self.cutoff
        if off == self.off:
            return False
        self.off = off
        self.config.hint_coef = 0.0 if off else self.configured
        if off:
            print(f"Hint imitation is off: the dungeon ladder's rung {self.rung} is at or past the cutoff rung "
                  f"{self.cutoff} (the probes beat the script there); mappo.hint_coef {self.configured:g} -> 0",
                  flush=True)
        else:
            print(f"Hint imitation is back on: the dungeon ladder fell back to rung {self.rung}, below the cutoff rung "
                  f"{self.cutoff}; mappo.hint_coef -> {self.configured:g}", flush=True)
        return True

    def coef(self) -> float:
        return 0.0 if self.off else self.configured
