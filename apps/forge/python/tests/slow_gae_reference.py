"""The per-agent loop the director's slow-clock GAE was, kept as the reference compute_span_gae is tested against."""

import numpy as np


def compute_slow_gae(
    rewards: np.ndarray,
    values: np.ndarray,
    dones: np.ndarray,
    terminated: np.ndarray,
    final_values: np.ndarray,
    last_values: np.ndarray,
    chosen: np.ndarray,
    slow: np.ndarray,
    gamma: float,
    gae_lambda: float,
) -> tuple[np.ndarray, np.ndarray]:
    """GAE for agents whose decisions are spaced out, over their own decisions rather than every step.

    One transition runs from the decision an agent actually took to the next one it takes -- or to the end of its
    episode, or the end of the rollout -- and carries every reward in between. Discounting is then per decision
    taken, so `gamma` and `gae_lambda` mean what they say on the agent's own clock: at ten decisions a span and
    250 ms a decision, gamma 0.996 is a ten minute horizon where the seats' 0.9975 is a hundred seconds.

    Shapes as compute_gae, plus `chosen` and `slow` [T, E, A]. Rewards inside a span are summed and not
    discounted: a span is seconds and the horizon is minutes, so the difference is far below the noise.

    Only the chosen steps of slow agents get an advantage; everything else is left untouched for the caller's
    own per-decision GAE to fill.
    """
    steps, envs, agents = rewards.shape
    advantages = np.zeros_like(rewards, dtype=np.float32)
    returns = np.zeros_like(rewards, dtype=np.float32)

    for env in range(envs):
        for agent in range(agents):
            if not slow[:, env, agent].any():
                continue

            # Walk the rollout once, cutting it into the transitions this agent actually made.
            spans = []      # (step, summed reward, bootstrap value, whether credit flows past it)
            step = 0
            while step < steps:
                if not (chosen[step, env, agent] and slow[step, env, agent]):
                    step += 1
                    continue

                reward = 0.0
                end = step
                while True:
                    reward += float(rewards[end, env, agent])
                    if dones[end, env] or end + 1 >= steps or chosen[end + 1, env, agent]:
                        break
                    end += 1

                if dones[end, env]:
                    # The episode ended inside the span: nothing follows a termination, and a truncation is
                    # worth the value of the state it was cut off in.
                    bootstrap = 0.0 if terminated[end, env] else float(final_values[end, env, agent])
                    flows = False
                elif end + 1 < steps:
                    bootstrap = float(values[end + 1, env, agent])
                    flows = True
                else:
                    # The rollout ended first; the value of what came next is all there is to go on.
                    bootstrap = float(last_values[env, agent])
                    flows = True

                spans.append((step, reward, bootstrap, flows))
                step = end + 1

            gae = 0.0
            for start, reward, bootstrap, flows in reversed(spans):
                delta = reward + gamma * bootstrap - float(values[start, env, agent])
                gae = delta + (gamma * gae_lambda * gae if flows else 0.0)
                advantages[start, env, agent] = gae
                returns[start, env, agent] = gae + float(values[start, env, agent])

    return advantages, returns
