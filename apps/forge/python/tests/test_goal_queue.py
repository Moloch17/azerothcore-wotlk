"""Two goals and a queue (next-run plan, Wave 4): the goal decision that promotes the queue, drops an ended
secondary and takes the director's primary; the slots drawn and scored alike; the pair on the wire; and a rollout
and update with four slots."""

import numpy as np
import torch

from animus.mappo.buffer import RolloutBuffer
from animus.mappo.networks import LayoutActor, goal_pair, split_goal_pair
from animus.mappo.trainer import MappoConfig, MappoTrainer

KINDS, TARGETS, SLOTS = 3, 4, 4
OWN = 40
# The goal block with the next-run columns: kinds, targets, ended, reached, secondary ended, event, from order,
# order kind, order target, achieved kind, achieved target.
BLOCK = KINDS + TARGETS + 2 + 3 + 2 * (KINDS + TARGETS)
OBS = OWN + BLOCK
COUNT = KINDS * TARGETS


def actor():
    torch.manual_seed(1)
    net = LayoutActor([(OBS, 2)], [8, 8], goal_count=KINDS, goal_targets=TARGETS, slow_size=5, goal_slots=SLOTS)
    net.goal_head.set_space(np.ones((KINDS, TARGETS), bool), [OWN])
    return net


def block_obs(rows: int, ended=False, secondary_ended=False, event=False, order=None) -> torch.Tensor:
    obs = torch.zeros(rows, OBS)
    obs[:, OWN : OWN + KINDS + TARGETS] = 1.0                 # every kind and target there
    base = OWN + KINDS + TARGETS
    obs[:, base] = float(ended)
    obs[:, base + 2] = float(secondary_ended)
    obs[:, base + 3] = float(event)
    if order is not None:
        obs[:, base + 4] = 1.0
        obs[:, base + 5 + order // TARGETS] = 1.0
        obs[:, base + 5 + KINDS + order % TARGETS] = 1.0
    return obs


def decide(net, obs, held, queue, clock):
    rows = obs.shape[0]
    features = torch.randn(rows, 5)
    return net.decide_goals(features, obs, torch.zeros(rows, dtype=torch.long), held, queue,
                            torch.full((rows,), clock), False)


def test_the_pair_round_trips():
    primary = torch.tensor([0, 5, COUNT - 1])
    secondary = torch.tensor([-1, 3, 0])
    assert all(torch.equal(a, b) for a, b in zip(split_goal_pair(goal_pair(primary, secondary, COUNT), COUNT),
                                                   (primary, secondary)))


def test_an_ended_primary_is_replaced_by_the_queue_without_a_choice():
    net = actor()
    held = goal_pair(torch.tensor([2]), torch.tensor([-1]), COUNT)
    out = decide(net, block_obs(1, ended=True), held, torch.tensor([[7, 9]]), False)
    primary, _ = split_goal_pair(out["goal"], COUNT)
    assert int(primary) == 7 and out["queue"].tolist() == [[9, -1]]
    assert not bool(out["chosen"]) and float(out["log_prob"]) == 0.0


def test_an_ended_primary_with_nothing_queued_chooses():
    net = actor()
    held = goal_pair(torch.tensor([2]), torch.tensor([-1]), COUNT)
    out = decide(net, block_obs(1, ended=True), held, torch.tensor([[-1, -1]]), False)
    assert bool(out["chosen"]) and float(out["log_prob"]) < 0.0


def test_an_ended_secondary_is_dropped_and_an_event_chooses():
    net = actor()
    held = goal_pair(torch.tensor([2]), torch.tensor([5]), COUNT)
    out = decide(net, block_obs(1, secondary_ended=True), held, torch.tensor([[-1, -1]]), False)
    assert int(split_goal_pair(out["goal"], COUNT)[1]) == -1 and not bool(out["chosen"])
    assert bool(decide(net, block_obs(1, event=True), held, torch.tensor([[-1, -1]]), False)["chosen"])


def test_the_directors_primary_is_held_and_not_scored():
    """Where the order sets the primary, the seat holds it whatever it drew, and the primary's log probability is
    left out: the slots after it are still the seat's own, drawn conditioned on the order."""
    net = actor()
    order = 6
    held = goal_pair(torch.tensor([0]), torch.tensor([-1]), COUNT)
    obs = block_obs(1, order=order)
    out = decide(net, obs, held, torch.tensor([[-1, -1]]), True)
    assert int(split_goal_pair(out["goal"], COUNT)[0]) == order
    signals = net.goal_head.signals(obs, torch.zeros(1, dtype=torch.long))
    features = torch.randn(1, 5)
    slots = out["slots"]
    _, with_order, _ = net.goal_head.draw(features, obs, torch.zeros(1, dtype=torch.long), signals["order_goal"],
                                          signals["from_order"], False, slots)
    _, without, _ = net.goal_head.draw(features, obs, torch.zeros(1, dtype=torch.long), signals["order_goal"],
                                       torch.zeros(1, dtype=torch.bool), False, slots)
    assert float(with_order) > float(without)       # the primary's (negative) log probability left out


def test_the_slots_are_scored_as_they_were_drawn():
    net = actor()
    rows = 16
    features = torch.randn(rows, 5)
    obs = block_obs(rows)
    layout = torch.zeros(rows, dtype=torch.long)
    given = torch.zeros(rows, dtype=torch.bool)
    order = torch.zeros(rows, dtype=torch.long)
    torch.manual_seed(3)
    slots, drawn_lp, _ = net.goal_head.draw(features, obs, layout, order, given, False)
    _, scored_lp, entropy = net.goal_head.draw(features, obs, layout, order, given, False, slots)
    assert torch.allclose(drawn_lp, scored_lp, atol=1e-5) and (entropy > 0).all()
    assert slots.shape == (rows, SLOTS) and (slots[:, 0] >= 0).all()


def test_the_wire_carries_the_two_held():
    config = MappoConfig(hidden=(8, 8), goal_count=KINDS, goal_targets=TARGETS, slow_goal_size=5, goal_slots=SLOTS)
    trainer = MappoTrainer([(OBS, 2)], 4, config)
    held = goal_pair(torch.tensor([[3], [8]]), torch.tensor([[-1], [2]]), COUNT).numpy()
    assert trainer.wire_goals(held).tolist() == [[[3, -1]], [[8, 2]]]


def test_a_rollout_and_update_with_four_slots():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), recurrent_size=6, goal_count=KINDS, goal_targets=TARGETS,
                         goal_every_decisions=3, slow_goal_size=5, goal_lookahead=True, goal_slots=SLOTS, epochs=2)
    trainer = MappoTrainer([(OBS, 2)], 4, config)
    trainer.actor.goal_head.set_space(np.ones((KINDS, TARGETS), bool), [OWN])
    trainer._sync_rollout()
    steps, envs = 24, 2
    buffer = RolloutBuffer(steps, envs, 1, OBS, 4, 2, trainer.foresight_outputs, trainer.recurrent_size, True,
                           trainer.slow_goal_size, trainer.goal_slots)
    rng = np.random.default_rng(2)
    acting = trainer.acting_state(envs, 1)
    for step in range(steps):
        obs = block_obs(envs, ended=bool(step % 5 == 4), secondary_ended=bool(step % 7 == 6)).numpy()[:, None, :]
        obs[..., :OWN] = rng.random((envs, 1, OWN), dtype=np.float32)
        state = rng.random((envs, 4), dtype=np.float32)
        mask = np.ones((envs, 1, 2), bool)
        layout = np.zeros((envs, 1), np.int64)
        memory = acting.memory.copy()
        actions, log_probs, values, foresight, goals, _ = trainer.act_and_value(obs, mask, layout, state,
                                                                                 state=acting)
        assert goals[5].shape == (envs, 1, SLOTS) and acting.queue.shape == (envs, 1, SLOTS - 2)
        assert trainer.wire_goals(goals[0]).shape == (envs, 1, 2)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, foresight, memory, goals)
        dones = np.array([step == 15, False])
        buffer.add_outcome(rng.random((envs, 1), dtype=np.float32), dones, dones, np.zeros((envs, 1), np.float32),
                           np.zeros((envs, 1, trainer.foresight_outputs), np.float32))
        acting.clear(dones)
    buffer.finish(np.zeros((envs, 1), np.float32), 0.99, 0.95,
                  last_foresight=np.zeros((envs, 1, trainer.foresight_outputs), np.float32),
                  foresight_gammas=(0.9, 0.99), time_scale_decisions=240.0,
                  slow_goal=(config.slow_goal_gamma, config.slow_goal_lambda),
                  obs_targets=trainer.foresight_obs_columns())
    assert (buffer.goal_slots[buffer.goal_chosen][:, 0] >= 0).all()
    stats = trainer.update(buffer)
    for key in ("slow_policy_loss", "slow_value_loss", "goal_entropy", "slow_approx_kl"):
        assert key in stats and np.isfinite(stats[key]), key
    # The slow loop scores exactly what it drew: an unchanged slow policy reads no KL against its own rollout.
    assert abs(stats["slow_approx_kl"]) < 5e-2


def test_the_exported_goal_queue_matches_the_learner(tmp_path):
    """The slots, the queue, the director's primary and the secondary's gate reach the file, and the reference
    forward pass holds the goals and queue and picks the action the learner's greedy decision does, step by step,
    through promotions, a dropped secondary, an event and an order."""
    from animus.export import export_layouts, read_amdl, reference_decide

    torch.manual_seed(4)
    net = LayoutActor([(OBS, 2)], [8, 8], recurrent_size=6, goal_count=KINDS, goal_targets=TARGETS, slow_size=5,
                      lookahead=True, goal_slots=SLOTS)
    net.goal_head.set_space(np.ones((KINDS, TARGETS), bool), [OWN])
    with torch.no_grad():
        for parameter in net.parameters():
            parameter.add_(torch.randn_like(parameter) * 0.3)
    spec = {"scenario": "stage", "layouts": [{"name": "warrior_dps", "obs_dim": OBS, "num_actions": 2}]}
    export_layouts(net.state_dict(), spec, tmp_path, tmp_path, goal_every=3)
    model = read_amdl(tmp_path / "stage.amdl")
    assert model["goals"]["slots"] == SLOTS

    state = {}
    memory = torch.zeros(1, 6)
    slow = torch.zeros(1, 5)
    held = torch.zeros(1, dtype=torch.long)
    queue = torch.full((1, SLOTS - 2), -1, dtype=torch.long)
    age = 0
    layout = torch.zeros(1, dtype=torch.long)
    script = [dict(), dict(ended=True), dict(), dict(secondary_ended=True), dict(event=True), dict(order=5),
              dict(ended=True, order=5), dict(), dict(ended=True), dict()]
    for step, signals in enumerate(script):
        obs = block_obs(1, **signals)[0].numpy()
        obs[:OWN] = np.random.default_rng(step).standard_normal(OWN)
        action, logits = reference_decide(model, obs, np.ones(2, bool), state=state)
        with torch.no_grad():
            obs_t = torch.tensor(obs)[None]
            features = net.features(obs_t, layout, memory)
            memory = features
            stepped = net.slow_step(features, slow)
            decided = net.decide_goals(stepped, obs_t, layout, held, queue,
                                       torch.tensor([age % 3 == 0]), True)
            if bool(decided["chosen"]):
                slow = stepped
            held, queue = decided["goal"], decided["queue"]
            age = 1 if bool(decided["chosen"]) else age + 1
            expected = net.action_distribution(features, layout, torch.ones(1, 2, dtype=torch.bool), held)
        assert state["goal"] == int(held), step
        assert state["queue"] == queue[0].tolist(), step
        torch_logits = expected.logits[0].numpy()
        assert np.allclose(logits - logits[0], torch_logits - torch_logits[0], atol=1e-4), step


def test_hindsight_relabels_what_was_achieved():
    """A decision whose next observation shows an enemy killed that the held primary did not name is trained under
    that goal too: the targets are read off the goal block, and the update reports the relabelled rows."""
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), recurrent_size=6, goal_count=KINDS, goal_targets=TARGETS,
                         goal_every_decisions=3, slow_goal_size=5, goal_slots=SLOTS, hindsight_coef=0.5, epochs=1)
    trainer = MappoTrainer([(OBS, 2)], 4, config)
    trainer.actor.goal_head.set_space(np.ones((KINDS, TARGETS), bool), [OWN])
    trainer._sync_rollout()
    steps, envs = 12, 2
    buffer = RolloutBuffer(steps, envs, 1, OBS, 4, 2, trainer.foresight_outputs, trainer.recurrent_size, True,
                           trainer.slow_goal_size, trainer.goal_slots)
    acting = trainer.acting_state(envs, 1)
    base = OWN + KINDS + TARGETS
    achieved_kind = base + 5 + KINDS + TARGETS
    for step in range(steps):
        obs = block_obs(envs).numpy()[:, None, :]
        if step % 3 == 2:
            obs[:, 0, achieved_kind + 0] = 1.0                           # Fight ...
            obs[:, 0, achieved_kind + KINDS + 1 + step % 3] = 1.0        # ... about an enemy slot
        state = np.zeros((envs, 4), np.float32)
        mask = np.ones((envs, 1, 2), bool)
        layout = np.zeros((envs, 1), np.int64)
        memory = acting.memory.copy()
        actions, log_probs, values, foresight, goals, _ = trainer.act_and_value(obs, mask, layout, state,
                                                                                 state=acting)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, foresight, memory, goals)
        buffer.add_outcome(np.ones((envs, 1), np.float32), np.zeros(envs, bool), np.zeros(envs, bool),
                           np.zeros((envs, 1), np.float32), np.zeros((envs, 1, trainer.foresight_outputs), np.float32))
    buffer.finish(np.zeros((envs, 1), np.float32), 0.99, 0.95,
                  slow_goal=(config.slow_goal_gamma, config.slow_goal_lambda))
    trainer.hindsight_targets(buffer)
    assert (buffer.achieved[1::3] >= 0).all() and (buffer.achieved[0::3] == -1).all()
    stats = trainer.update(buffer)
    assert stats.get("hindsight_rows", 0) > 0 and np.isfinite(stats["hindsight_loss"])


def test_every_slot_draws_only_goals_that_are_there():
    # Only kind 1 and target 2 are present: whatever a slot draws is that goal or none -- the queue too, which
    # once drew from every goal its kind accepts and held the head's entropy near uniform (2026-09-30).
    net = actor()
    obs = block_obs(64)
    obs[:, OWN : OWN + KINDS + TARGETS] = 0.0
    obs[:, OWN + 1] = 1.0
    obs[:, OWN + KINDS + 2] = 1.0
    features = torch.randn(64, 5)
    head = net.goal_head
    drawn = [torch.full((64,), 1 * TARGETS + 2)]
    for slot in range(1, SLOTS):
        logits = head.slot_logits(features, slot, drawn, obs, torch.zeros(64, dtype=torch.long))
        allowed = logits > -1e8
        assert allowed[:, -1].all()                                  # none is always there
        assert allowed[:, :-1].sum(dim=-1).le(1).all()
        assert not allowed[:, :-1].any() or allowed[:, 1 * TARGETS + 2].all()


def test_the_slots_after_the_primary_are_a_share_of_the_entropy():
    net = actor()
    head = net.goal_head
    rows = 16
    obs = block_obs(rows)
    features = torch.randn(rows, 5)
    layout = torch.zeros(rows, dtype=torch.long)
    given = torch.zeros(rows, dtype=torch.bool)
    primary_given = torch.zeros(rows, dtype=torch.long)
    torch.manual_seed(3)
    head.slot_entropy_weight = 0.0
    slots, _, alone = head.draw(features, obs, layout, primary_given, given, False)
    head.slot_entropy_weight = 1.0
    _, _, full = head.draw(features, obs, layout, primary_given, given, False, slots)
    head.slot_entropy_weight = 0.1
    _, _, tenth = head.draw(features, obs, layout, primary_given, given, False, slots)
    assert torch.allclose(tenth, alone + 0.1 * (full - alone), atol=1e-5)
    assert (full > alone).all()
