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


def test_a_restricted_stage_is_overlaid_on_the_seed_without_its_trunk():
    """Merging the stealth drill forward: seed every class from the full stage, then overlay the restricted stage's
    own layouts (rogue here) and leave the trunk, and every other class, as the full stage left them."""
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))
    layouts = [Layout("rogue", 6, 3), Layout("mage", 6, 3)]
    full = MappoTrainer([(6, 3), (6, 3)], 4, config)
    drill = MappoTrainer([(6, 3)], 4, config)
    new = MappoTrainer([(6, 3), (6, 3)], 4, config)
    seed_trainer(new, {"trainer": full.state_dict(), "spec": checkpoint_spec(layouts)}, spec(layouts, 4))
    before = {key: tensor.clone() for key, tensor in new.actor.state_dict().items()}

    overlaid = seed_trainer(new, {"trainer": drill.state_dict(), "spec": checkpoint_spec(layouts[:1])},
                            spec(layouts, 4), overlay=True)
    assert overlaid == ["rogue"]
    after = new.actor.state_dict()
    drilled = drill.actor.state_dict()
    for key, tensor in after.items():
        if key.startswith(("adapters.0.", "heads.0.")):
            torch.testing.assert_close(tensor, drilled[key])      # rogue: the drill's own
        else:
            torch.testing.assert_close(tensor, before[key])       # trunk and mage: untouched


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
    stage = {"stage": "test_stage_next", "seed_chain": ["test_stage_next", "test_stage"]}
    config = TrainConfig(run_name="test_stage_next", runs_dir="/out/runs")
    assert config.resolved_init_from(stage) == ["/out/runs/test_stage_next/best.pt", "/out/runs/test_stage/best.pt"]

    # The first stage, and a scenario the sim wrote no stage.json for, train from scratch.
    assert config.resolved_init_from({"seed_chain": []}) == []
    assert config.resolved_init_from(None) == []

    # Named candidates, with the run directory filled in.
    named = TrainConfig(run_name="mage", runs_dir="runs", init_from=["{runs_dir}/other/best.pt", ""])
    assert named.resolved_init_from(stage) == ["runs/other/best.pt"]
    assert TrainConfig(init_from="").resolved_init_from(stage) == []
    assert TrainConfig(init_from=None).resolved_init_from(stage) == []


def test_finetune_from_names_the_stage_checkpoint():
    config = TrainConfig(run_name="test_stage_next", runs_dir="/out/runs")
    assert config.resolved_finetune_from() == "/out/runs/_finetune/test_stage_next/best.pt"
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
    for stage in (old_stage, new_stage):
        stage["layouts"]["priest_heal"]["blocks"][0]["action_features"] = F
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


def test_core_global_features_match_the_core_block_header():
    import pathlib
    import re

    from animus.bootstrap import CORE_ACTION_FEATURES, CORE_GLOBAL_FEATURES

    header = (pathlib.Path(__file__).resolve().parents[4]
              / "src/server/game/Animus/Scenario/Curriculum/Blocks/CoreBlock.h").read_text()
    assert int(re.search(r"OBS_GLOBAL_COUNT\s*=\s*(\d+)", header).group(1)) == CORE_GLOBAL_FEATURES
    assert int(re.search(r"ACTION_FEATURES\s*=\s*(\d+)", header).group(1)) == CORE_ACTION_FEATURES


def test_a_new_block_starts_from_nothing_and_core_carries_over():
    """The forecast block (BlockId::Forecast) is new in every layout: seeding a checkpoint without it keeps core."""
    from animus.bootstrap import _common_blocks

    old = {"core": ((0, 400), (0, 60)), "move": ((400, 80), (60, 30))}
    new = {"core": ((0, 400), (0, 60)), "move": ((400, 80), (60, 30)), "forecast": ((480, 13), (90, 0))}
    common = _common_blocks(old, new, "warrior")
    assert ((0, 400), (0, 60)) in [spans[0] for spans in common]
    assert all(spans[1][0][0] != 480 for spans in common)


def test_a_block_grown_at_its_end_keeps_its_old_columns():
    """The crowd block gained its second-pack columns after its slots: the old columns seed where they were, and a
    block that changed in the middle (any other) still starts from nothing."""
    from animus.bootstrap import _common_blocks

    old = {"core": ((0, 400), (0, 60)), "crowd": ((400, 102), (60, 3)), "party": ((502, 20), (63, 2))}
    new = {"core": ((0, 400), (0, 60)), "crowd": ((400, 109), (60, 3)), "party": ((509, 24), (63, 2))}
    common = _common_blocks(old, new, "warrior")
    assert (((400, 102), (60, 3)), ((400, 102), (60, 3))) in common
    assert all(spans[0][0][0] != 502 for spans in common)


def test_a_seat_set_that_gained_slots_carries_its_slots_and_actions_by_name():
    """The enemies went from four slots to twenty-four (PACK_SLOTS, 2026-10-03): the pack block's globals and its
    first four slots seed where they were, hold_interrupt's row moves past the new slot actions, the new slots'
    actions keep their init, and their normaliser statistics start as the last old slot's."""
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))
    G, F = 2, 3                                         # the pack block's globals, the slot's features

    def stage(slots: int) -> dict:
        built = stage_with({"warrior_tank": [("core", 4, 2), ("pack", G + slots * F, slots + 1), ("duel", 3, 1)]})
        entry = built["layouts"]["warrior_tank"]
        entry["action_names"] = (["noop", "stop"] + [f"target_slot_{slot}" for slot in range(slots)]
                                 + ["hold_interrupt", "kick"])
        entry["sets"] = [{"name": "enemies", "slots": slots, "present": 0,
                          "segments": [{"first": 4 + G, "stride": F}], "pointers": [{"first": 2, "count": slots}]}]
        return built

    old_obs, new_obs = 4 + G + 4 * F + 3, 4 + G + 24 * F + 3
    old = MappoTrainer([(old_obs, 2 + 5 + 1)], 4, config)
    new = MappoTrainer([(new_obs, 2 + 25 + 1)], 4, config)
    old.actor.norms[0].mean.copy_(torch.arange(old_obs, dtype=torch.float32))
    old.actor.norms[0].count.fill_(1_000_000.0)
    fresh_head = new.actor.state_dict()["heads.0.weight"].clone()
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("warrior_tank", old_obs, 8)]),
                  "stage": stage(4)}

    seed_trainer(new, checkpoint, spec([Layout("warrior_tank", new_obs, 28)], 4), stage(24))

    new_w = new.actor.state_dict()["adapters.0.weight"]
    old_w = old.actor.state_dict()["adapters.0.weight"]
    kept = 4 + G + 4 * F
    torch.testing.assert_close(new_w[:, :kept], old_w[:, :kept])                    # core, globals, slots 0-3
    torch.testing.assert_close(new_w[:, new_obs - 3 :], old_w[:, old_obs - 3 :])    # duel after the slots
    assert torch.count_nonzero(new_w[:, kept : new_obs - 3]) == 0                   # slots 4-23 start at zero

    new_head = new.actor.state_dict()["heads.0.weight"]
    old_head = old.actor.state_dict()["heads.0.weight"]
    torch.testing.assert_close(new_head[:6], old_head[:6])                          # noop, stop, slots 0-3
    torch.testing.assert_close(new_head[26], old_head[6])                           # hold_interrupt moved
    torch.testing.assert_close(new_head[27], old_head[7])                           # the duel's kick
    torch.testing.assert_close(new_head[6:26], fresh_head[6:26])                    # the new slots' actions

    mean = new.actor.norms[0].mean
    last = old.actor.norms[0].mean[4 + G + 3 * F : 4 + G + 4 * F]
    for slot in range(4, 24):
        torch.testing.assert_close(mean[4 + G + slot * F : 4 + G + (slot + 1) * F], last)


def test_rescaled_columns_start_their_normaliser_afresh_and_keep_their_weights():
    """The talent trees read as a share of the points spent (2026-10-03): a parent without the tag measured those
    columns on the old scale, so their statistics start at 0/1 with the count capped; the adapter columns carry. A
    parent that has the tag already keeps everything."""
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))

    def stage(tagged: bool) -> dict:
        built = stage_with({"paladin": [("core", 6, 2), ("duel", 3, 1)]})
        if tagged:
            built["layouts"]["paladin"]["blocks"][0]["rescaled"] = [{"tag": "tree_share", "first": 4, "count": 2}]
        return built

    old = MappoTrainer([(9, 3)], 4, config)
    old.actor.norms[0].mean.copy_(torch.full((9,), 5.0))
    old.actor.norms[0].var.copy_(torch.full((9,), 4.0))
    old.actor.norms[0].count.fill_(30_000_000.0)
    for parent, fresh in ((stage(False), True), (stage(True), False)):
        new = MappoTrainer([(9, 3)], 4, config)
        checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("paladin", 9, 3)]), "stage": parent}
        seed_trainer(new, checkpoint, spec([Layout("paladin", 9, 3)], 4), stage(True))
        norm = new.actor.norms[0]
        torch.testing.assert_close(norm.mean[:4], torch.full((4,), 5.0))
        torch.testing.assert_close(norm.mean[6:], torch.full((3,), 5.0))
        torch.testing.assert_close(new.actor.state_dict()["adapters.0.weight"][:, 4:6],
                                   old.actor.state_dict()["adapters.0.weight"][:, 4:6])
        if fresh:
            assert float(norm.mean[4:6].abs().max()) == 0.0 and float(norm.var[4:6].min()) == 1.0
            assert float(norm.count) <= SEED_COUNT_CAP
        else:
            torch.testing.assert_close(norm.mean[4:6], torch.full((2,), 5.0))
            assert float(norm.count) == 30_000_000.0


def test_a_core_block_from_before_ready_and_affordable_keeps_each_actions_features():
    """A stage.json without "action_features" is from before ready and affordable (6 per action): each action's six
    seed into the first six of its eight, the new two start at zero, the rank tiers keep their rows, and the talents
    after the catalog land where they now are (2026-10-04)."""
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))
    from animus.bootstrap import CORE_ACTION_FEATURES as F, CORE_GLOBAL_FEATURES as G

    names = ["noop", "smite_585", "heal_2054", "rank_high", "rank_mid", "rank_low", "stop"]
    talents = 2
    old_core, new_core = G + 3 * 6 + talents, G + 3 * F + talents
    old_stage = stage_with({"priest_heal": [("core", old_core, 6), ("duel", 3, 1)]})
    new_stage = stage_with({"priest_heal": [("core", new_core, 6), ("duel", 3, 1)]})
    old_stage["layouts"]["priest_heal"]["action_names"] = names
    new_stage["layouts"]["priest_heal"]["action_names"] = names
    new_stage["layouts"]["priest_heal"]["blocks"][0]["action_features"] = F
    old = MappoTrainer([(old_core + 3, 7)], 4, config)
    new = MappoTrainer([(new_core + 3, 7)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("priest_heal", old_core + 3, 7)]),
                  "stage": old_stage}

    seed_trainer(new, checkpoint, spec([Layout("priest_heal", new_core + 3, 7)], 4), new_stage)

    new_w = new.actor.state_dict()["adapters.0.weight"]
    old_w = old.actor.state_dict()["adapters.0.weight"]
    torch.testing.assert_close(new_w[:, :G], old_w[:, :G])
    for action in range(3):
        torch.testing.assert_close(new_w[:, G + action * F : G + action * F + 6],
                                   old_w[:, G + action * 6 : G + action * 6 + 6])
        assert torch.count_nonzero(new_w[:, G + action * F + 6 : G + (action + 1) * F]) == 0
    torch.testing.assert_close(new_w[:, new_core - talents :], old_w[:, old_core - talents :])   # talents and duel
    torch.testing.assert_close(new.actor.state_dict()["heads.0.weight"], old.actor.state_dict()["heads.0.weight"])

