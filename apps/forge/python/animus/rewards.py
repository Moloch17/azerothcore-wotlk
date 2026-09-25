"""What the reward is actually paying for.

Three separate faults in this project shared one shape: a term quietly became the largest earner and the policy
optimised it instead of the game.

- A farmable resurrection was 57.55 of druid_dps's 65.08 return in stage 4 -- 88% of its whole score -- because
  a resurrection offer the core never clears was accepted again every decision.
- A goal paid for every decision it was held made standing at range the stage's second largest earner, so a
  ranged seat was paid to keep out of the fight.
- An order-compliance nudge priced per decision came to 5.01 an episode, 23.7% of gross and level with the kill
  (5.23) and the death (-5.27), which would have bought tunnel vision on the called target.

Each was found by hand, and the first two only after runs had been trained on them. They are not the same bug
and no single check would have predicted them, but they broke the same rule, which is the one checked here: no
shaping term should be worth an appreciable fraction of what winning is worth.

Outcome terms -- the kill, the clear, the capture, the arrival -- are what a stage is *for*, and may be as
large as they like. Everything else is shaping. Shaping that out-earns the outcome is not shaping any more, it
is the objective, and the policy will tell you so.
"""

from __future__ import annotations

#: What a stage is for. Everything else in the ledger is shaping around it.
#:
#: A resurrection is deliberately NOT here. Standing an ally up is a means -- what the stage is for is the
#: party living through the pull -- and listing it as an outcome is exactly what would have let the farmable
#: revive through this check, since it was the largest earner by a factor of ten and would have been reading
#: itself as the yardstick.
OUTCOME_TERMS = ("kill", "clear", "flag_capture", "flag_return", "player_kill", "arrive",
                 "quest_turn_in", "gather_node", "town_done")

#: A shaping term worth more than this much of the largest outcome term is reported. Half a kill is already a
#: lot for a nudge; the three faults above scored 0.96, well over 1, and far over 1 respectively.
MAX_SHAPING_SHARE = 0.5

#: Updates between repeats of the same warning, so a run that is going to trip it does not print 153 times.
WARN_EVERY = 25


def reward_mix(row: dict) -> dict[str, float]:
    """{term: mean earned an episode} from a metrics row's episode_reward_* columns."""
    prefix = "episode_reward_"
    mix = {}
    for key, value in row.items():
        if not key.startswith(prefix):
            continue
        try:
            mix[key[len(prefix):]] = float(value)
        except (TypeError, ValueError):
            continue
    return mix


def audit(mix: dict[str, float], outcome_terms=OUTCOME_TERMS,
          max_shaping_share: float = MAX_SHAPING_SHARE) -> tuple[str, float, float, float] | None:
    """The worst-behaved shaping term as (term, earned, outcome earned, share), or None when none is.

    Only earnings count, never charges: a penalty is not something a policy can farm, and the largest negative
    term in a fight is usually the death, which is the point.
    """
    outcome = max((value for term, value in mix.items() if term in outcome_terms and value > 0.0), default=0.0)
    shaping = [(term, value) for term, value in mix.items() if term not in outcome_terms and value > 0.0]
    if not shaping or outcome <= 0.0:
        return None

    term, earned = max(shaping, key=lambda pair: pair[1])
    share = earned / outcome
    return (term, earned, outcome, share) if share > max_shaping_share else None


def describe(finding: tuple[str, float, float, float], mix: dict[str, float], top: int = 4) -> str:
    """The warning line: what is out of hand, against what, and the rest of the mix for context."""
    term, earned, outcome, share = finding
    rest = sorted(mix.items(), key=lambda pair: -abs(pair[1]))[:top]
    context = ", ".join(f"{name} {value:+.3g}" for name, value in rest)
    return (f"reward: {term} earns {earned:.3g} an episode, {share:.0%} of the largest outcome term "
            f"({outcome:.3g}) -- shaping this large is the objective, not a nudge. Mix: {context}")
