# The druid's own configs

A run of a single class (`AnimusForge.Classes = "druid"`) takes its learner config from
`configs/druid/<stage>.yaml` where one exists, and from the shared `configs/<stage>.yaml` otherwise
(`ForgeConfig::LearnerConfigFor`). Only the stages where the druid has something of its own to say have a file
here; the rest inherit.

**What a class's own config holds.** Two things, neither of them a gate -- stages have none, every stage ends on the
same convergence rule (animus.stage):

- **Where its combat line seeds from.** The movement stages (1-7) are trained once for every class in the shared
  run, and a class's `stage8_duel` names that checkpoint outright, because `init_from: auto` only looks under the
  run's own `runs` directory: `init_from: [{shared_runs}/stage7_flight/best.pt]`. Seeding across works because the
  layout check is one-directional: every layout the run has must be in the checkpoint, and a druid-only run takes
  the druid's adapter and head out of an all-class checkpoint and leaves the other nine behind.
- **What to report.** `eval.report` columns that are the druid's to read: `form_at_end` on the duel and the stealth
  drill (whether it fought in the form its build is for), the tank's threat columns, the healer's mana and
  overheal columns. They are reports per class and per build; nothing is judged on them.

**The druid is four builds across what used to be three roles:**

| build | tree | what to read |
|---|---|---|
| `balance` | 0 | ranged caster; the easiest of the four to win a duel with |
| `feral_cat` | 1 | melee damage, and the only one that can open from Prowl |
| `feral_bear` | 1 | holds the pull; kills slowly, so the clock is its risk |
| `restoration` | 2 | heals; the weakest solo |

`feral_cat` and `feral_bear` share a talent tree, which is exactly the case a role could not separate and the
reason the ladder and the training weights are keyed on the build.
