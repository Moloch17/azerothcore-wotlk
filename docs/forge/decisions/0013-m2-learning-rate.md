# 0013: M2's learning rate is 1.5e-4

**Date:** 2026-10-07

**Decision.** `move2_seek` sets actor_lr and critic_lr to 1.5e-4 (commit 21dad7b74). Its descendants M3 and M4 inherit it through the config chain; the combat line does not (combat1_fight.yaml is standalone and sets 3e-4).

**Reason.** A convergence bug annealed M2's rate from about 30M steps, so the configured 3e-4 really ran at about 3e-4 x 0.41. At the full 3e-4 kl_move was expected near 0.03, over the 0.02 target. The re-baseline fix resets the anneal to 1.0, so 1.5e-4 keeps the rate M2 effectively ran at.

**What it constrains.** Revisit if kl_move reads well under 0.01 and found rises slowly (comment in configs/move2_seek.yaml). Whether C1's 3e-4 is intended is a question for the owner.

See also [principles.md](../principles.md) and the [index](README.md).
