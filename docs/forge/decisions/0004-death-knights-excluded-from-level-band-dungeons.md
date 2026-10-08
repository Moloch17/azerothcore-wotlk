# 0004: Death knights are not fielded in level-band dungeon stages

**Date:** 2026-10-07

**Decision.** G1, G2, D1, D2 and D3 do not field death knights; each declares `status.excluded.death_knight` so the absence reads as a decision. The movement stages and C1-C3 do field them (at their own level 55).

**Reason.** A party shares one level under the dungeon's cap (Ragefire 13-18, Deadmines 17-20) and a death knight starts at 55. The fix (accept it, a death-knight band, scaled content) was left to the owner (comment in configs/group1_roles.yaml). The human-operable plan lists "no death knights in the level-band dungeons" among its decisions.

**What it constrains.** The final models have no dungeon training for death knights. Every evaluation report prints the exclusion (`StatusConfig.excluded_line`). UNVERIFIED: how the sim redraws the seats among the other classes (check StageScenario's casting draw).

See also [principles.md](../principles.md) and the [index](README.md).
