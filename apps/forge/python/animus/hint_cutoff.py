"""Hint imitation switched off once the probes beat the script (dungeon-curriculum I6).

The dungeon teacher's hints (the sim's hint block, mappo.hint_coef) are a support that has to end: the first taught
Deadmines run imitated a script that stood still, and its bots stood still alone (2026-10-01). The sim decides when --
the first ladder rung whose probes (runs the policy plays alone) clear more of the dungeon than the script's runs do
(StageScenario::WingHintOffRung) -- and from then on writes no hint at that rung or any later one. Every ended
episode of a whole dungeon says whether its rung's imitation is off (episode info `wing_hint_off`).

This is the learner's half, so the cutoff holds whatever the config asks: from the first ended training episode that
says so, hint_coef is 0 for the rest of the run -- the imitation loss is not computed at all, for hints and for the
rows the teacher played alike. It does not come back: an episode of another arena of the stage (a pull drill, a held
out dungeon) reads 0 in the column without meaning "on", and a ladder falling back below the cutoff is the policy
failing, which imitating the script it already beat would not mend. A resumed run learns it again from its first
ended episode (the sim keeps the cutoff with Instance.WingHintOffRung).
"""

from __future__ import annotations

import numpy as np

COLUMN = "wing_hint_off"


class HintCutoff:
    """Follows `wing_hint_off` on the ended training episodes and holds `config.hint_coef` at 0 once it is set."""

    def __init__(self, info_names, config) -> None:
        names = list(info_names)
        self.column = names.index(COLUMN) if COLUMN in names else None
        self.config = config
        self.configured = float(config.hint_coef)
        self.off = False

    @property
    def active(self) -> bool:
        """Whether there is anything to cut: a stage that reports the cutoff and a config that imitates."""
        return self.column is not None and self.configured > 0.0

    def observe(self, ended: np.ndarray | None) -> bool:
        """The ended episodes' info rows ([N, K], the policy's own): whether imitation was switched off now."""
        if self.off or not self.active or ended is None or len(ended) == 0:
            return False
        if not bool((np.asarray(ended)[:, self.column] > 0.5).any()):
            return False
        self.off = True
        self.config.hint_coef = 0.0
        print(f"Hint imitation is off for the rest of the run: the probes beat the script at the dungeon ladder's "
              f"rung (the sim's {COLUMN}); mappo.hint_coef {self.configured:g} -> 0", flush=True)
        return True

    def coef(self) -> float:
        return 0.0 if self.off else self.configured
