"""peak-play W4: every seat layout's entities as sets (EntitySets, mappo.seat_sets) -- off, the networks are as they
were; on, a set reads the same in any slot order, its pointer actions follow the entity, and the adapters stay blind to
the slot columns through an update."""

import numpy as np
import pytest
import torch

from animus.bootstrap import seed_trainer
from animus.mappo.buffer import RolloutBuffer
from animus.mappo.networks import seat_sets_of
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout
from test_bootstrap import checkpoint_spec, spec

# Two layouts holding the same set at different offsets (an enemy: 3 pack columns and 2 hostiles columns a slot, 2
# slots, present at the first pack column), their select-enemy actions at different action indexes, and one with none.
SETS_A = [{"name": "enemies", "slots": 2, "present": 0,
           "segments": [{"first": 2, "stride": 3}, {"first": 10, "stride": 2}], "pointers": [{"first": 1, "count": 2}]}]
SETS_B = [{"name": "enemies", "slots": 2, "present": 0,
           "segments": [{"first": 4, "stride": 3}, {"first": 12, "stride": 2}], "pointers": [{"first": 3, "count": 2}]}]
LAYOUTS = [(20, 4), (16, 5), (8, 3)]
DESCRIPTORS = [SETS_A, SETS_B, []]
STATE = 6


def trainer(seat_sets: bool, descriptors=DESCRIPTORS, attention: bool = False) -> MappoTrainer:
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16), recurrent_size=4, seat_sets=seat_sets, epochs=1, minibatches=1,
                         entity_attention=attention)
    return MappoTrainer(LAYOUTS, STATE, config, seat_sets=descriptors)


def awake(sets) -> None:
    """The attention layer moved off its identity start, as training moves it."""
    with torch.no_grad():
        for parameter in (sets.attend.out_proj.weight, sets.mix_out.weight, sets.type_embed,
                          sets.pool.weight[:, -sets.embed:]):
            parameter.normal_(0.0, 0.5)


def enemy_obs(layout: int, slots: list[tuple[float, float, float, float, float]]) -> torch.Tensor:
    """A row of layout `layout` with its two enemy slots filled (present first), padded to the widest layout."""
    row = torch.zeros(20)
    pack, hostiles = (2, 10) if layout == 0 else (4, 12)
    for slot, (present, a, b, c, d) in enumerate(slots):
        row[pack + 3 * slot : pack + 3 * slot + 3] = torch.tensor([present, a, b])
        row[hostiles + 2 * slot : hostiles + 2 * slot + 2] = torch.tensor([c, d])
    return row


def test_off_the_networks_are_exactly_as_they_were():
    """mappo.seat_sets off: descriptors handed in change nothing -- the same parameters, the same outputs. What keeps
    a running chain's checkpoints loading."""
    off, plain = trainer(False), trainer(False, None)
    for a, b in ((off.actor, plain.actor), (off.critic, plain.critic)):
        assert a.state_dict().keys() == b.state_dict().keys()
        for key, value in a.state_dict().items():
            assert torch.equal(value, b.state_dict()[key]), key
    obs = torch.randn(6, 20)
    layout = torch.tensor([0, 1, 2, 0, 1, 2])
    mask = torch.ones(6, 5, dtype=torch.bool)
    for network in (off, plain):
        features = network.actor.features(obs, layout)
        network.logits = network.actor.action_logits(features, layout, mask, obs=obs)
    assert torch.equal(off.logits, plain.logits)


@pytest.mark.parametrize("attention", [False, True], ids=["sets", "attention"])
def test_a_set_reads_the_same_in_any_order_and_its_pointers_follow_the_entity(attention):
    """With attention too (no position in it): only which entities, not which slots. Without it two classes reading
    the same entities pool them alike; with it each class's own token is attended to, so they need not."""
    on = trainer(True, attention=attention)
    sets = on.actor.entity_sets
    if attention:
        awake(sets)
    first = (1.0, 0.3, -0.2, 0.9, 0.1)
    second = (1.0, -0.7, 0.5, 0.0, 0.4)
    obs = torch.stack([enemy_obs(0, [first, second]), enemy_obs(0, [second, first]),
                       enemy_obs(1, [first, second]), torch.zeros(20)])
    layout = torch.tensor([0, 0, 1, 2])
    pooled = sets.pooled(obs, layout)
    torch.testing.assert_close(pooled[0], pooled[1])            # slot order does not matter
    if not attention:
        torch.testing.assert_close(pooled[0], pooled[2])        # nor which class's offsets it came from
    assert torch.equal(pooled[3], torch.zeros_like(pooled[3]))  # a layout without sets gets nothing

    features = torch.randn(4, on.actor.head_width)
    features[1] = features[0]
    features[2] = features[0]
    logits = torch.zeros(4, 5)
    out = sets.with_pointers(logits, features, obs, layout)
    # Layout 0 names its enemies at actions 1-2, layout 1 at 3-4: the same two enemies, swapped slots, swapped scores.
    torch.testing.assert_close(out[0, 1:3], out[1, 1:3].flip(0))
    if not attention:
        torch.testing.assert_close(out[0, 1:3], out[2, 3:5])
    assert torch.equal(out[0, [0, 3, 4]], logits[0, [0, 3, 4]])  # other actions untouched
    assert torch.equal(out[3], logits[3])                        # a layout without sets untouched


def test_the_adapters_stay_blind_to_the_slot_columns_through_an_update():
    on = trainer(True)
    blind = on.actor.entity_sets.blind
    assert set(blind) == {0, 1} and blind[0] == [2, 3, 4, 5, 6, 7, 10, 11, 12, 13]
    for network in (on.actor, on.critic):
        for index, columns in blind.items():
            assert torch.equal(network.adapters[index].weight[:, columns], torch.zeros(16, len(columns)))
    rng = np.random.default_rng(0)
    envs, agents = 3, 1
    buffer = RolloutBuffer(4, envs, agents, 20, STATE, 5, 0, on.recurrent_size)
    acting = on.acting_state(envs, agents)
    for _ in range(4):
        layout = np.array([[0], [1], [2]])
        obs = rng.random((envs, agents, 20), dtype=np.float32)
        mask = np.ones((envs, agents, 5), bool)
        state = rng.random((envs, STATE), dtype=np.float32)
        memory = acting.memory.copy()
        actions, log_probs, values, *_ = on.act_and_value(obs, mask, layout, state, state=acting)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, None, memory)
        buffer.add_outcome(rng.random((envs, agents), dtype=np.float32), np.zeros(envs, bool), np.zeros(envs, bool),
                           np.zeros((envs, agents), np.float32))
    buffer.finish(np.zeros((envs, agents), np.float32), 0.99, 0.95)
    before = on.actor.entity_sets.pool.weight.clone()
    on.update(buffer)
    assert not torch.equal(before, on.actor.entity_sets.pool.weight)    # the sets learn
    assert on.director_columns_clear()                                  # the adapters still read none of them


def test_a_stage_json_without_seat_sets_is_refused():
    assert seat_sets_of({"layouts": {"a": {"sets": SETS_A}, "b": {}}}, ["a", "b"]) == [SETS_A, []]
    assert seat_sets_of({"layouts": {"a": {"sets": []}, "b": {"sets": []}}}, ["a", "b"]) is None   # stage4_duel
    with pytest.raises(ValueError, match="names no seat sets"):
        seat_sets_of({"layouts": {"a": {}, "b": {}}}, ["a", "b"])


def test_seeding_carries_the_sets_and_starts_them_at_zero_from_a_stage_without():
    """From a stage with seat sets: its encoders, pool and pointer queries. From one without (stage2 -> stage3): the
    pool starts at zero, so the seeded policy is the parent's until the sets learn."""
    names = [Layout(name, obs, actions) for name, (obs, actions) in zip("abc", LAYOUTS)]
    parent = trainer(True)
    with torch.no_grad():
        for parameter in parent.actor.entity_sets.parameters():
            parameter.add_(0.5)
    child = trainer(True)
    seed_trainer(child, {"trainer": parent.state_dict(), "spec": checkpoint_spec(names)}, spec(names, STATE))
    for key, value in parent.actor.entity_sets.state_dict().items():
        assert torch.equal(child.actor.entity_sets.state_dict()[key], value), key

    child = trainer(True)
    seed_trainer(child, {"trainer": trainer(False).state_dict(), "spec": checkpoint_spec(names)}, spec(names, STATE))
    for network in (child.actor, child.critic):
        assert not network.entity_sets.pool.weight.any() and not network.entity_sets.pool.bias.any()


def test_attention_with_every_slot_absent_is_finite_and_seeds_from_sets_unchanged():
    """Every slot absent: the own token is the only key, so no softmax row is empty -- the logits are finite, the sets'
    mean and max read 0 and only the own token's pool columns can add anything. And an attention actor seeded from a
    sets checkpoint starts as that checkpoint: the layer is the identity and the own token's columns are 0."""
    attended = trainer(True, attention=True)
    awake(attended.actor.entity_sets)
    obs = torch.zeros(3, 20)
    obs[1] = enemy_obs(1, [(0.0, 0.3, -0.2, 0.9, 0.1), (0.0, 1.0, 1.0, 1.0, 1.0)])    # slots filled, both absent
    layout = torch.tensor([0, 1, 2])
    features = attended.actor.features(obs, layout)
    logits = attended.actor.action_logits(features, layout, torch.ones(3, 5, dtype=torch.bool), obs=obs)
    assert torch.isfinite(logits).all()

    parent = trainer(True)
    with torch.no_grad():
        for parameter in parent.actor.entity_sets.parameters():
            parameter.add_(0.3)
    names = [Layout(name, o, a) for name, (o, a) in zip("abc", LAYOUTS)]
    child = trainer(True, attention=True)
    seed_trainer(child, {"trainer": parent.state_dict(), "spec": checkpoint_spec(names)}, spec(names, STATE))
    obs = torch.stack([enemy_obs(0, [(1.0, 0.3, -0.2, 0.9, 0.1), (1.0, -0.7, 0.5, 0.0, 0.4)]),
                       enemy_obs(1, [(1.0, 0.1, 0.2, 0.3, 0.4), (0.0, 0.0, 0.0, 0.0, 0.0)]), torch.rand(20)])
    mask = torch.ones(3, 5, dtype=torch.bool)
    for network in (parent, child):
        network.out = network.actor.action_logits(network.actor.features(obs, layout), layout, mask, obs=obs)
    torch.testing.assert_close(child.out, parent.out, rtol=1e-5, atol=1e-5)
