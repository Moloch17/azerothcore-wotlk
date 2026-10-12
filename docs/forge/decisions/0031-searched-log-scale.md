# 0031: The whole-map search totals on a log scale

**Date:** 2026-10-12 (owner: "we need to make the searched cells metric robust because the entire search behavior
depends on it"). Map block revision 3 -> 4; move2_seek restarted fresh with it (the third-person run started an hour
earlier, decision 0030's camera follow-up).

**Why.** The map block's `searched_cells` (the 1-yd cells looked at this episode over the whole map) was
`min(1, cells / 6000)` and `total` (cells stood on) `min(1, cells / 3000)`. In the 210.6M evaluation the searched
total reached 1 in 244 of 312 episodes, a third of the way in: past that point the seat's only whole-map record of
its search said nothing, and the route viewer's coverage curve (the same value) went flat. The per-cell searched
channel and the coverage block's per-tile shares (over a tile's 1024 cells) were not affected.

**Change.** `MapBlock::LogShare(cells, max) = min(1, log1p(cells) / log1p(max))`:

- `searched_cells` over `SEARCHED_LOG_MAX` 131072 (100 cells 0.39, 1000 0.59, 6000 0.74, 20000 0.84, 100000 0.98);
- `total` over `TOTAL_LOG_MAX` 32768.

Same columns, same names; the manifest's `floor_scale` / `total_scale` become `searched_cells_log_max` /
`total_log_max`. The route viewer converts the traced value back to cells (log for revision 4, linear for older
runs). No learner change: the learner reads these scalars by name only (the `no_coverage` arm zeroes them).
