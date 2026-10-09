# 0009: A stage's purpose is paid as Outcome; shaping fades

**Date:** 2026-10-05

**Decision.** Terms are Outcome, Cost or Shaping (`RewardCategory`). The score (`score_outcome`, which best.pt and convergence follow) is Outcome plus Cost at full price. Shaping is multiplied by the fade's scale and the noise prices by the cost ladder's scale.

**Reason.** Stage 3 of the first curriculum collapsed because its lesson was paid as Shaping and the fade took it away (commit ad57c84e7, 2026-10-05); G1's drills held only once paid as Outcome (dungeon-curriculum plan).

**What it constrains.** Every stage should name its purpose as an Outcome or Cost it pays (no test checks this since 2026-10-07). A new term needs a category (`static_assert` in RewardLedger.h).

See also [principles.md](../principles.md) and the [index](README.md).
