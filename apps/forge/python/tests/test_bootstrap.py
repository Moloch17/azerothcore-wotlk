from types import SimpleNamespace

import pytest
import torch

from animus.bootstrap import SEED_COUNT_CAP, seed_trainer
from animus.config import TrainConfig
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout


def spec(layouts: list[Layout], state_dim: int) -> SimpleNamespace:
    return SimpleNamespace(layouts=tuple(layouts), state_dim=state_dim)


def checkpoint_spec(layouts: list[Layout]) -> dict:
    return {"layouts": [{"name": l.name, "obs_dim": l.obs_dim, "num_actions": l.num_actions} for l in layouts]}


def test_seeded_actor_matches_earlier_stage_on_its_inputs_and_actions():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16))
    old_layouts = [Layout("warrior_dps", 6, 3), Layout("mage_dps", 5, 4)]
    # The same layouts, in a different order and wider: a later stage keeps its base's classes and adds features
    # and actions to them.
    new_layouts = [Layout("mage_dps", 8, 6), Layout("warrior_dps", 9, 5)]
    old = MappoTrainer([(l.obs_dim, l.num_actions) for l in old_layouts], 4, config)
    new = MappoTrainer([(l.obs_dim, l.num_actions) for l in new_layouts], 11, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec(old_layouts)}

    seeded = seed_trainer(new, checkpoint, spec(new_layouts, 11))
    assert sorted(seeded) == ["mage_dps", "warrior_dps"]

    # warrior_dps is layout 0 before and 1 now. New feature columns start at zero, so whatever they hold does
    # not change the earlier actions' logits (up to the normalising shift of a distribution's logits).
    obs = torch.randn(4, 6)
    new_obs = torch.cat([obs, torch.randn(4, 3)], dim=-1)
    padded_old = torch.cat([obs, torch.zeros(4, 0)], dim=-1)

    old_logits = old.actor(padded_old, torch.zeros(4, dtype=torch.long), torch.ones(4, 4)).logits[:, :3]
    new_logits = new.actor(new_obs, torch.ones(4, dtype=torch.long), torch.ones(4, 6)).logits[:, :3]
    torch.testing.assert_close(new_logits - new_logits[:, :1], old_logits - old_logits[:, :1], rtol=1e-4, atol=1e-4)


def test_a_layout_the_checkpoint_lacks_is_refused_rather_than_started_from_scratch():
    """The rule that replaced the sim's "a restricted stage must be a leaf".

    A stage only some classes play leaves a partial checkpoint, and `init_from: auto` takes the first checkpoint
    in the chain that exists -- so seeding a full run from it used to start the missing classes from random
    weights in the middle of the curriculum, silently, which looks exactly like a class that has not learned
    anything yet. Only here are the run's actual layouts known, so this is where it can be caught.
    """
    config = MappoConfig(hidden=(8,))
    old_layouts = [Layout("rogue", 6, 3)]
    new_layouts = [Layout("rogue", 6, 3), Layout("mage", 6, 3)]
    old = MappoTrainer([(6, 3)], 4, config)
    new = MappoTrainer([(6, 3), (6, 3)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec(old_layouts)}

    with pytest.raises(ValueError, match="the checkpoint has no mage"):
        seed_trainer(new, checkpoint, spec(new_layouts, 4))


def test_the_director_is_the_one_layout_allowed_to_be_missing():
    """It is an agent a stage adds, not a class the run plays, so the first directed stage in a chain
    necessarily seeds from one without it."""
    config = MappoConfig(hidden=(8,))
    old_layouts = [Layout("rogue", 6, 3)]
    new_layouts = [Layout("rogue", 6, 3), Layout("director", 6, 3)]
    old = MappoTrainer([(6, 3)], 4, config)
    new = MappoTrainer([(6, 3), (6, 3)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec(old_layouts)}

    assert seed_trainer(new, checkpoint, spec(new_layouts, 4)) == ["rogue"]


def test_critic_state_encoder_and_head_are_not_copied():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))
    layouts = [Layout("warrior_dps", 6, 3)]
    old = MappoTrainer([(6, 3)], 4, config)
    new = MappoTrainer([(6, 3)], 4, config)
    fresh_head = new.critic.state_dict()["head.weight"].clone()
    fresh_state = new.critic.state_dict()["state_encoder.weight"].clone()

    seed_trainer(new, {"trainer": old.state_dict(), "spec": checkpoint_spec(layouts)}, spec(layouts, 4))

    critic = new.critic.state_dict()
    torch.testing.assert_close(critic["head.weight"], fresh_head)
    torch.testing.assert_close(critic["state_encoder.weight"], fresh_state)
    torch.testing.assert_close(critic["adapters.0.weight"], old.critic.state_dict()["adapters.0.weight"])


def stage_with(layouts: dict[str, list[tuple[str, int, int]]]) -> dict:
    """A stage.json with block spans: layout name -> [(block, features, actions)], blocks placed in order."""
    entries = {}
    for name, blocks in layouts.items():
        obs = actions = 0
        spans = []
        for block, features, count in blocks:
            spans.append({"name": block, "obs": [obs, features], "actions": [actions, count]})
            obs += features
            actions += count
        entries[name] = {"obs_dim": obs, "num_actions": actions, "blocks": spans}
    return {"layouts": entries}


def test_a_branch_is_seeded_block_by_block():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16))
    # The base (party-like) has core, duel, pack and pvp; the branch keeps core, duel and pvp.
    base = stage_with({"warrior_dps": [("core", 4, 3), ("duel", 3, 2), ("pack", 5, 4), ("pvp", 2, 0)]})
    branch = stage_with({"warrior_dps": [("core", 4, 3), ("duel", 3, 2), ("pvp", 2, 0), ("arena", 1, 1)]})
    old = MappoTrainer([(14, 9)], 4, config)
    new = MappoTrainer([(10, 6)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("warrior_dps", 14, 9)]),
                  "stage": base}

    fresh_head = new.actor.state_dict()["heads.0.weight"].clone()
    assert seed_trainer(new, checkpoint, spec([Layout("warrior_dps", 10, 6)], 4), branch) == ["warrior_dps"]

    for network, old_network in ((new.actor, old.actor), (new.critic, old.critic)):
        new_w = network.state_dict()["adapters.0.weight"]
        old_w = old_network.state_dict()["adapters.0.weight"]
        torch.testing.assert_close(new_w[:, 0:7], old_w[:, 0:7])       # core and duel stay in place
        torch.testing.assert_close(new_w[:, 7:9], old_w[:, 12:14])     # pvp moves up past the dropped pack
        assert torch.count_nonzero(new_w[:, 9:]) == 0                  # the new block starts at zero

    new_head = new.actor.state_dict()["heads.0.weight"]
    old_head = old.actor.state_dict()["heads.0.weight"]
    torch.testing.assert_close(new_head[0:5], old_head[0:5])            # core and duel actions
    torch.testing.assert_close(new_head[5], fresh_head[5])               # the new block's action keeps its init


def test_a_block_that_changed_shape_is_seeded_from_scratch_and_the_rest_carries_over():
    """A block whose shape moved cannot be copied column by column, but it is the only part of a layout that
    cannot. Refusing the whole checkpoint over one block throws away every other block that would have carried,
    and the trunk with them; the changed one reaches the trunk at zero and is learned."""
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))
    old = MappoTrainer([(7, 5)], 4, config)
    new = MappoTrainer([(8, 5)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("mage_dps", 7, 5)]),
                  "stage": stage_with({"mage_dps": [("core", 4, 3), ("duel", 3, 2)]})}

    seed_trainer(new, checkpoint, spec([Layout("mage_dps", 8, 5)], 4),
                 stage_with({"mage_dps": [("core", 4, 3), ("duel", 4, 2)]}))

    new_w = new.actor.state_dict()["adapters.0.weight"]
    # Core kept its four features and carried over; duel went from three to four and starts at nothing.
    torch.testing.assert_close(new_w[:, :4], old.actor.state_dict()["adapters.0.weight"][:, :4])
    assert torch.count_nonzero(new_w[:, 4:]) == 0


def test_without_spans_the_layout_is_seeded_as_a_prefix():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))
    old = MappoTrainer([(5, 3)], 4, config)
    new = MappoTrainer([(7, 4)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("rogue_dps", 5, 3)])}

    seed_trainer(new, checkpoint, spec([Layout("rogue_dps", 7, 4)], 4),
                 stage_with({"rogue_dps": [("core", 5, 3), ("duel", 2, 1)]}))

    new_w = new.actor.state_dict()["adapters.0.weight"]
    torch.testing.assert_close(new_w[:, :5], old.actor.state_dict()["adapters.0.weight"])
    assert torch.count_nonzero(new_w[:, 5:]) == 0


def test_init_from_follows_the_stage_seed_chain():
    stage = {"stage": "stage10_gauntlet", "seed_chain": ["stage9_pack", "stage8_duel"]}
    config = TrainConfig(run_name="stage10_gauntlet", runs_dir="/out/runs")
    assert config.resolved_init_from(stage) == ["/out/runs/stage9_pack/best.pt", "/out/runs/stage8_duel/best.pt"]

    # The first stage, and a scenario the sim wrote no stage.json for, train from scratch.
    assert config.resolved_init_from({"seed_chain": []}) == []
    assert config.resolved_init_from(None) == []

    # Named candidates, with the run directory filled in.
    named = TrainConfig(run_name="mage", runs_dir="runs", init_from=["{runs_dir}/other/best.pt", ""])
    assert named.resolved_init_from(stage) == ["runs/other/best.pt"]
    assert TrainConfig(init_from="").resolved_init_from(stage) == []
    assert TrainConfig(init_from=None).resolved_init_from(stage) == []


def test_finetune_from_names_the_stage_checkpoint():
    config = TrainConfig(run_name="stage10_gauntlet", runs_dir="/out/runs")
    assert config.resolved_finetune_from() == "/out/runs/_finetune/stage10_gauntlet/best.pt"
    assert TrainConfig(finetune_from="").resolved_finetune_from() == ""


def test_a_core_catalog_that_lost_a_spell_is_seeded_by_action_name():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))
    from animus.bootstrap import CORE_ACTION_FEATURES as F, CORE_GLOBAL_FEATURES as G

    # The old catalog has noop, grovel_7267 and smite_585; the new one drops grovel. Talents (2) follow the actions.
    old_core, new_core = G + 3 * F + 2, G + 2 * F + 2
    old_stage = stage_with({"priest_heal": [("core", old_core, 3), ("duel", 3, 1)]})
    new_stage = stage_with({"priest_heal": [("core", new_core, 2), ("duel", 3, 1)]})
    old_stage["layouts"]["priest_heal"]["action_names"] = ["noop", "grovel_7267", "smite_585", "stop"]
    new_stage["layouts"]["priest_heal"]["action_names"] = ["noop", "smite_585", "stop"]
    old = MappoTrainer([(old_core + 3, 4)], 4, config)
    new = MappoTrainer([(new_core + 3, 3)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("priest_heal", old_core + 3, 4)]),
                  "stage": old_stage}

    seed_trainer(new, checkpoint, spec([Layout("priest_heal", new_core + 3, 3)], 4), new_stage)

    new_w = new.actor.state_dict()["adapters.0.weight"]
    old_w = old.actor.state_dict()["adapters.0.weight"]
    torch.testing.assert_close(new_w[:, :G], old_w[:, :G])                                  # globals
    torch.testing.assert_close(new_w[:, G : G + F], old_w[:, G : G + F])                    # noop's features
    torch.testing.assert_close(new_w[:, G + F : G + 2 * F], old_w[:, G + 2 * F : G + 3 * F])  # smite moved up
    torch.testing.assert_close(new_w[:, new_core:], old_w[:, old_core:])                    # talents and duel
    new_head = new.actor.state_dict()["heads.0.weight"]
    old_head = old.actor.state_dict()["heads.0.weight"]
    torch.testing.assert_close(new_head[1], old_head[2])                                    # smite's row
    torch.testing.assert_close(new_head[2], old_head[3])                                    # the duel action


def test_a_block_the_parent_lacked_is_not_normalised_on_the_parents_confidence():
    """`count` is one scalar for a whole normaliser while mean and var are per feature, so it cannot say
    "certain about these columns, ignorant of those". Inheriting a parent's tens of millions of rows whole applies
    that confidence to features that have never been observed: RunningNorm.update moves a mean by
    batch_count / (count + batch_count), so a rollout barely touches them and the new block spends most of the
    stage feeding tanh a value whose scale it never learned.

    The carried-over columns are already close, so capping the count costs them nothing and lets the new ones be
    described within the first few rollouts. It stays above zero because RunningNorm.forward passes rows through
    raw at count 0, which would throw the seeded statistics away for a whole rollout."""
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16))
    base = stage_with({"warrior_dps": [("core", 4, 3), ("duel", 3, 2)]})
    branch = stage_with({"warrior_dps": [("core", 4, 3), ("duel", 3, 2), ("arena", 2, 1)]})
    old = MappoTrainer([(7, 5)], 4, config)
    new = MappoTrainer([(9, 6)], 4, config)

    # A parent that trained for 30M steps, with statistics on the blocks it had.
    old.actor.norms[0].mean.copy_(torch.full((7,), 5.0))
    old.actor.norms[0].var.copy_(torch.full((7,), 4.0))
    old.actor.norms[0].count.fill_(30_000_000.0)

    seeded = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("warrior_dps", 7, 5)]), "stage": base}
    seed_trainer(new, seeded, spec([Layout("warrior_dps", 9, 6)], 4), branch)

    norm = new.actor.norms[0]
    torch.testing.assert_close(norm.mean[:7], old.actor.norms[0].mean)   # the shared blocks carried over
    assert float(norm.mean[7:].abs().max()) == 0.0                       # the new block starts at 0/1
    assert float(norm.var[7:].min()) == 1.0
    assert 0.0 < float(norm.count) <= SEED_COUNT_CAP

    # One rollout describes the new block: its mean lands near the batch's, while the inherited columns hold.
    rows = torch.cat([torch.full((4096, 7), 5.0), torch.full((4096, 2), 40.0)], dim=-1)
    norm.update(rows)
    assert float(norm.mean[7:].min()) > 4.0      # moved most of the way from 0 towards 40
    torch.testing.assert_close(norm.mean[:7], old.actor.norms[0].mean)
