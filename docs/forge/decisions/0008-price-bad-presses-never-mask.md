# 0008: Bad presses are priced, never masked

**Date:** 2026-09-28

**Decision.** Only the physically impossible is masked. A heal on a full-health friend, a refused cast or a press against the seat's own goal is judged and priced (Aimless and its per-cause prices, Effort, Fidget, Repeat, Jitter), so the policy learns.

**Reason.** Masks hide what the policy has not learned and break when the situation changes. The code landed as fddf80d1b (2026-09-28, presses judged against the goal). UNVERIFIED: the date the owner stated the rule.

**What it constrains.** An action nobody ever wants is removed from the catalog (e.g. a self-stun spell), not masked. Prices are in `CurriculumTuning::Actions`.

See also [principles.md](../principles.md) and the [index](README.md).
