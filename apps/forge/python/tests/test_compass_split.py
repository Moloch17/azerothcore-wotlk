"""The compass split (perception-goals P1): the move block's objective columns left it for the compass block (move
revision 4 -> 5, CompassBlock revision 1). An M1 checkpoint trained at revision 4 has to seed the new layouts with its
movement intact: the move block's kept columns by name where revision 5 has them, the objective's columns in the
compass block (M1) or nowhere (M2 seek, which has no compass), and the move block's actions as they were. Without the
by-name remap a changed revision starts the whole move block fresh, which would throw M1's movement away."""

import re
from pathlib import Path
from types import SimpleNamespace

import torch

from animus.bootstrap import MOVE_REVISION_4_COLUMNS, seed_trainer
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout

CURRICULUM = Path(__file__).resolve().parents[4] / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum"

COMPASS = ["objective", "objective_bearing_sin", "objective_bearing_cos", "objective_distance", "objective_near",
           "detour"]
MOVE_5 = [name for name in MOVE_REVISION_4_COLUMNS if name not in COMPASS]
MOVE_ACTIONS = [f"move_key_{index}" for index in range(5)]
CORE, CORE_ACTIONS = 4, 3
GOAL = 2
LAYOUT = "warrior"


def stage(blocks: list[tuple[str, int, list[str], int, list[str] | None]]) -> dict:
    """A stage.json of one layout: (block, revision, action names, features, obs_names or None), in order."""
    obs = actions = 0
    spans, names = [], []
    for block, revision, block_actions, features, columns in blocks:
        entry = {"name": block, "obs": [obs, features], "actions": [actions, len(block_actions)]}
        if revision:
            entry["revision"] = revision
        if columns is not None:
            entry["obs_names"] = columns
        spans.append(entry)
        names += block_actions
        obs += features
        actions += len(block_actions)
    return {"layouts": {LAYOUT: {"obs_dim": obs, "num_actions": actions, "blocks": spans, "action_names": names}}}


CORE_BLOCK = ("core", 1, [f"core_{index}" for index in range(CORE_ACTIONS)], CORE, None)
GOAL_BLOCK = ("goal", 0, [], GOAL, None)
# M1 before the split: move revision 4 of 63 columns, from before stage.json named them.
M1_OLD = stage([CORE_BLOCK, ("move", 4, MOVE_ACTIONS, len(MOVE_REVISION_4_COLUMNS), None), GOAL_BLOCK])
# M1 after it: the move block's 57 and the compass's 6, both named.
M1_NEW = stage([CORE_BLOCK, ("move", 5, MOVE_ACTIONS, len(MOVE_5), MOVE_5), ("compass", 1, [], 6, COMPASS),
                GOAL_BLOCK])
# M2 seek: no compass.
M2 = stage([CORE_BLOCK, ("move", 5, MOVE_ACTIONS, len(MOVE_5), MOVE_5), GOAL_BLOCK])


def dims(stage_json: dict) -> tuple[int, int]:
    entry = stage_json["layouts"][LAYOUT]
    return entry["obs_dim"], entry["num_actions"]


def trainer(stage_json: dict) -> MappoTrainer:
    return MappoTrainer([dims(stage_json)], 4, MappoConfig(hidden=(16, 16)))


def seed(old_stage: dict, new_stage: dict, old: MappoTrainer | None = None) -> tuple[MappoTrainer, MappoTrainer]:
    torch.manual_seed(0)
    old = old or trainer(old_stage)
    # Statistics a trained parent would have, distinct per column (and per network) so a misplaced one shows.
    width = dims(old_stage)[0]
    for offset, network in ((0.0, old.actor), (1000.0, old.critic)):
        network.norms[0].mean.copy_(torch.arange(width, dtype=torch.float32) + offset)
        network.norms[0].var.copy_(torch.arange(width, dtype=torch.float32) * 0.5 + 2.0 + offset)
        network.norms[0].count.fill_(30_000_000.0)
    new = trainer(new_stage)
    layout = Layout(LAYOUT, *dims(new_stage))
    checkpoint = {"trainer": old.state_dict(), "stage": old_stage,
                  "spec": {"layouts": [{"name": LAYOUT, "obs_dim": dims(old_stage)[0],
                                        "num_actions": dims(old_stage)[1]}]}}
    assert seed_trainer(new, checkpoint, SimpleNamespace(layouts=(layout,), state_dim=4), new_stage) == [LAYOUT]
    return old, new


def column(stage_json: dict, block: str, name: str) -> int:
    """The global column of `name` in `block`, by the stage's names (revision 4's frozen list for an unnamed move)."""
    for entry in stage_json["layouts"][LAYOUT]["blocks"]:
        if entry["name"] == block:
            names = entry.get("obs_names") or list(MOVE_REVISION_4_COLUMNS)
            return entry["obs"][0] + names.index(name)
    raise KeyError(block)


def weights(network, key: str = "adapters.0.weight") -> torch.Tensor:
    return network.state_dict()[key]


def assert_norms_follow(old, new, old_stage, moved, new_stage):
    """Each (block, name, old name) column's normaliser mean and variance, in both networks, are the old column's."""
    for network, old_network in ((new.actor, old.actor), (new.critic, old.critic)):
        for block, name, old_name in moved:
            to, at = column(new_stage, block, name), column(old_stage, "move", old_name)
            for stat in ("mean", "var"):
                assert float(getattr(network.norms[0], stat)[to]) == float(getattr(old_network.norms[0], stat)[at]), (
                    block, name, stat)
        # Every column carried, so the parent's confidence is kept rather than capped.
        assert float(network.norms[0].count) == float(old_network.norms[0].count)


def test_m1_at_revision_4_seeds_the_new_m1_column_by_column():
    old, new = seed(M1_OLD, M1_NEW)
    for network, old_network in ((new.actor, old.actor), (new.critic, old.critic)):
        new_w, old_w = weights(network), weights(old_network)
        # Every move column revision 5 kept: its weights unchanged, wherever it now sits.
        for name in MOVE_5:
            torch.testing.assert_close(new_w[:, column(M1_NEW, "move", name)], old_w[:, column(M1_OLD, "move", name)])
        # The objective's columns landed in the compass block.
        for name in COMPASS:
            torch.testing.assert_close(new_w[:, column(M1_NEW, "compass", name)],
                                       old_w[:, column(M1_OLD, "move", name)])
        # Core and goal carried as blocks.
        torch.testing.assert_close(new_w[:, :CORE], old_w[:, :CORE])
        torch.testing.assert_close(new_w[:, -GOAL:], old_w[:, -GOAL:])
    # The normalisers' statistics -- mean and variance, actor's and critic's -- moved with them, so the copied
    # weights read the columns at the scale they were trained on.
    assert_norms_follow(old, new, M1_OLD, [("move", name, name) for name in MOVE_5]
                        + [("compass", name, name) for name in COMPASS], M1_NEW)
    # The move block's actions are its own as they were.
    torch.testing.assert_close(weights(new.actor, "heads.0.weight"), weights(old.actor, "heads.0.weight"))
    torch.testing.assert_close(weights(new.actor, "heads.0.bias"), weights(old.actor, "heads.0.bias"))


def test_m2_seeds_its_movement_from_m1_at_revision_4_and_leaves_the_compass_behind():
    old, new = seed(M1_OLD, M2)
    new_w, old_w = weights(new.actor), weights(old.actor)
    for name in MOVE_5:
        torch.testing.assert_close(new_w[:, column(M2, "move", name)], old_w[:, column(M1_OLD, "move", name)])
    assert_norms_follow(old, new, M1_OLD, [("move", name, name) for name in MOVE_5], M2)
    # Every column of M2 is a carried one: nothing of the objective's has anywhere to go, and nothing was zeroed.
    assert new_w.shape[1] == CORE + len(MOVE_5) + GOAL
    assert all(torch.count_nonzero(new_w[:, index]) > 0 for index in range(new_w.shape[1]))
    torch.testing.assert_close(weights(new.actor, "heads.0.weight"), weights(old.actor, "heads.0.weight"))


def test_m2_seeds_from_the_new_m1_with_the_compass_simply_unused():
    old, new = seed(M1_NEW, M2)
    new_w, old_w = weights(new.actor), weights(old.actor)
    # The move block is the same revision and width: it carries as a block, the compass is dropped.
    torch.testing.assert_close(new_w[:, :CORE + len(MOVE_5)], old_w[:, :CORE + len(MOVE_5)])
    torch.testing.assert_close(new_w[:, -GOAL:], old_w[:, -GOAL:])
    torch.testing.assert_close(weights(new.actor, "heads.0.weight"), weights(old.actor, "heads.0.weight"))


def test_without_names_a_changed_revision_still_starts_fresh():
    """The remap follows names only: a revision 3 move block (never shipped, no frozen list) starts fresh as before."""
    old_stage = stage([CORE_BLOCK, ("move", 3, MOVE_ACTIONS, 63, None), GOAL_BLOCK])
    _, new = seed(old_stage, M1_NEW)
    new_w = weights(new.actor)
    first = column(M1_NEW, "move", MOVE_5[0])
    assert torch.count_nonzero(new_w[:, first : first + len(MOVE_5) + 6]) == 0


def test_the_frozen_revision_4_names_are_the_blocks_own():
    """MOVE_REVISION_4_COLUMNS is revision 4's order with the C++'s names: every name MoveBlock and CompassBlock give
    their columns is in it, and nothing else is."""
    named = set()
    for source in ("Blocks/MoveBlock.cpp", "Blocks/CompassBlock.cpp"):
        named |= set(re.findall(r'case OBS_[A-Z_]+:\s*return "([a-z_]+)";', (CURRICULUM / source).read_text()))
    modes = {"mode_ground", "mode_falling", "mode_swimming", "mode_flying"}
    trail = {name for name in MOVE_REVISION_4_COLUMNS if re.fullmatch(r"trail_\d_(ahead|left)", name)}
    assert len(MOVE_REVISION_4_COLUMNS) == 63 == len(set(MOVE_REVISION_4_COLUMNS))
    assert named | modes | trail == set(MOVE_REVISION_4_COLUMNS)
    assert set(COMPASS) <= named
