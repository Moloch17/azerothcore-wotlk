# 0002: No scripted teachers, baselines or scripted players

**Date:** 2026-10-07

**Decision.** Every behaviour is discovered by the learner. The scripted dungeon teacher, the scripted greedy and fight baselines, `ScriptedPlayer` and the scripted "human" stand-in were deleted. The only policy the sim can play itself is `random`. Smoke tests and comparisons use the learner.

**Reason.** Writing and fixing the scripts cost more than they gave; the dungeon teacher's first live run did not move a single seat (principles.md, rule 14). The owner stopped the removal work mid-way (WIP commit 641cf015c) and it was finished in a355df8ff, aa303bc33 (baselines; the protocol stays 25) and 91811bba6 (ScriptedPlayer). The human-operable plan records it ("The user dropped scripted teachers").

**What it constrains.** No stage may rely on a script to teach. The one remaining script is M4's follow leader in the owner slot (`PartyFollowEncounter.h`, not dead code); it is the thing followed, not a teacher of the policy under training. `MODE_FLAG` bit 1 (`MODE_FLAG_SCRIPTED_OPPONENTS`) stays unused until the next protocol change (`Bridge/Protocol.h`). `eval.baseline` accepts only "" or "random" (`animus/config.py`, `EvalConfig.__post_init__`).

See also [principles.md](../principles.md) and the [index](README.md).
