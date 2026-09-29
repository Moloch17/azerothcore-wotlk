"""The director reads its members and enemies as sets (DirectorSets): the same judgement whichever slots they are in and
however many empty slots there are, per-slot actions that move with their member, and slot columns its own adapter
never learns."""

import numpy as np
import torch

from animus.mappo.networks import LayoutActor, LayoutCritic, clear_director_columns

GLOBALS, SEATS, SEAT_W, ENEMIES, ENEMY_W = 6, 8, 5, 3, 4
OBS = GLOBALS + SEATS * SEAT_W + ENEMIES * ENEMY_W
ACTIONS = 2 + SEATS + ENEMIES             # hold, posture; address member m; focus enemy e
DESCRIPTOR = {
    "globals": GLOBALS,
    "seats": {"first": GLOBALS, "slots": SEATS, "width": SEAT_W, "present": 0},
    "enemies": {"first": GLOBALS + SEATS * SEAT_W, "slots": ENEMIES, "width": ENEMY_W, "present": 0},
    "pointers": [{"first": 2, "over": "seats"}, {"first": 2 + SEATS, "over": "enemies"}],
    "may_call": 1,
}


def director_obs(globals_, members, enemies):
    obs = np.zeros(OBS, np.float32)
    obs[:GLOBALS] = globals_
    for slot, seat in enumerate(members):
        obs[GLOBALS + slot * SEAT_W : GLOBALS + (slot + 1) * SEAT_W] = seat
    first = GLOBALS + SEATS * SEAT_W
    for slot, enemy in enumerate(enemies):
        obs[first + slot * ENEMY_W : first + (slot + 1) * ENEMY_W] = enemy
    return obs


def networks():
    torch.manual_seed(0)
    layouts = [(7, 3), (OBS, ACTIONS)]
    actor = LayoutActor(layouts, [16, 16], director=(1, DESCRIPTOR))
    critic = LayoutCritic(4, layouts, [16, 16], director=(1, DESCRIPTOR))
    with torch.no_grad():                  # as if trained: every weight, the blind columns included, moved
        for net in (actor, critic):
            for parameter in net.parameters():
                parameter.add_(torch.randn_like(parameter) * 0.5)
            clear_director_columns(net)
    return actor, critic


def evaluate(actor, critic, obs):
    obs_t = torch.tensor(obs)[None]
    layout = torch.ones(1, dtype=torch.long)
    with torch.no_grad():
        features = actor.features(obs_t, layout)
        logits = actor.action_distribution(features, layout, torch.ones(1, ACTIONS, dtype=torch.bool),
                                           obs=obs_t).logits[0].numpy()
        value = critic(torch.zeros(1, 4), obs_t, layout)[0].item()
    return logits, value


def test_the_director_is_the_same_whichever_slot_a_member_is_in_and_however_many_are_empty():
    rng = np.random.default_rng(0)
    actor, critic = networks()
    members = [np.concatenate([[1.0], rng.random(SEAT_W - 1)]) for _ in range(3)]
    enemies = [np.concatenate([[1.0], rng.random(ENEMY_W - 1)]) for _ in range(2)]
    globals_ = rng.random(GLOBALS)
    logits, value = evaluate(actor, critic, director_obs(globals_, members, enemies))

    # The members in reverse order, and the enemies swapped: the per-slot actions move with them, the rest is equal.
    swapped = director_obs(globals_, members[::-1], enemies[::-1])
    logits_s, value_s = evaluate(actor, critic, swapped)
    assert np.isclose(value, value_s, atol=1e-5)
    np.testing.assert_allclose(logits[:2], logits_s[:2], atol=1e-5)
    np.testing.assert_allclose(logits[2:5], logits_s[2:5][::-1], atol=1e-5)
    np.testing.assert_allclose(logits[2 + SEATS : 4 + SEATS], logits_s[2 + SEATS : 4 + SEATS][::-1], atol=1e-5)

    # The same three members spread over other slots with empty ones between: nothing changes but where they are.
    empty = np.zeros(SEAT_W)
    spread = director_obs(globals_, [members[0], empty, members[1], empty, empty, members[2]], enemies)
    logits_p, value_p = evaluate(actor, critic, spread)
    assert np.isclose(value, value_p, atol=1e-5)
    np.testing.assert_allclose(logits[:2], logits_p[:2], atol=1e-5)
    np.testing.assert_allclose(logits[2:5], logits_p[[2, 4, 7]], atol=1e-5)


def test_the_director_adapter_never_learns_the_slot_columns():
    actor, _ = networks()
    weight = actor.adapters[1].weight
    blind = actor.director_sets.column_mask
    assert (weight[:, blind] == 0).all()
    loss = actor.adapters[1](torch.ones(1, OBS)).sum()
    loss.backward()
    assert (weight.grad[:, blind] == 0).all() and (weight.grad[:, ~blind] != 0).any()


def test_the_exported_director_matches_the_learner(tmp_path):
    """The set encoder, the pooling and the pointer heads reach the .amdl, and the reference forward pass scores
    every action as the learner does."""
    import json

    from animus.export import export_layouts, read_amdl, reference_decide

    actor, _ = networks()
    spec = {"scenario": "stage", "layouts": [{"name": "warrior_dps", "obs_dim": 7, "num_actions": 3},
                                             {"name": "director", "obs_dim": OBS, "num_actions": ACTIONS}]}
    (tmp_path / "stage.json").write_text(json.dumps({"director": DESCRIPTOR}))
    export_layouts(actor.state_dict(), spec, tmp_path, tmp_path)
    model = read_amdl(tmp_path / "stage_director.amdl")
    assert model["sets"] is not None and len(model["sets"]["pointers"]) == 2

    rng = np.random.default_rng(5)
    members = [np.concatenate([[1.0], rng.random(SEAT_W - 1)]) for _ in range(4)]
    enemies = [np.concatenate([[1.0], rng.random(ENEMY_W - 1)]) for _ in range(2)]
    obs = director_obs(rng.random(GLOBALS), members, enemies)
    _, logits = reference_decide(model, obs, np.ones(ACTIONS, bool))
    expected, _ = evaluate(actor, LayoutCritic(4, [(7, 3), (OBS, ACTIONS)], [16, 16], director=(1, DESCRIPTOR)), obs)
    np.testing.assert_allclose(logits - logits[0], expected - expected[0], atol=1e-4)


def test_a_director_without_its_sets_is_not_exported(tmp_path):
    import pytest

    from animus.export import export_layouts

    actor, _ = networks()
    spec = {"scenario": "stage", "layouts": [{"name": "warrior_dps", "obs_dim": 7, "num_actions": 3},
                                             {"name": "director", "obs_dim": OBS, "num_actions": ACTIONS}]}
    with pytest.raises(ValueError, match="set encoder"):
        export_layouts(actor.state_dict(), spec, tmp_path, tmp_path)
