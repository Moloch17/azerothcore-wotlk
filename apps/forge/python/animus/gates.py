"""What is left of the stage gates: a confidence bound for reading shares.

Stages have no pass gates any more (animus.stage): every stage ends on the same convergence signals. The Wilson
bound stays as a reporting helper -- a share of episodes (a win rate, an arrival rate) read at a confidence level
rather than as a raw mean, so a class with few episodes reads as uncertain rather than as lucky.
"""

from __future__ import annotations

import math


def wilson_bound(share: float, episodes: int, confidence: float, lower: bool) -> float:
    """The one-sided Wilson score bound of a binomial share over `episodes` trials at `confidence`."""
    if episodes <= 0:
        return 0.0 if lower else 1.0
    z = _z_one_sided(confidence)
    n = float(episodes)
    centre = share + z * z / (2.0 * n)
    spread = z * math.sqrt(share * (1.0 - share) / n + z * z / (4.0 * n * n))
    denominator = 1.0 + z * z / n
    return (centre - spread) / denominator if lower else (centre + spread) / denominator


def _z_one_sided(confidence: float) -> float:
    """The standard normal quantile for a one-sided confidence level, by bisection on erf."""
    if not 0.0 < confidence < 1.0:
        raise ValueError("confidence must be between 0 and 1 exclusive")
    low, high = 0.0, 10.0
    for _ in range(80):
        middle = (low + high) / 2.0
        if 0.5 * (1.0 + math.erf(middle / math.sqrt(2.0))) < confidence:
            low = middle
        else:
            high = middle
    return (low + high) / 2.0
