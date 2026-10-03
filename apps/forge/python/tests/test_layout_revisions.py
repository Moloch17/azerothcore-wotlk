"""A block re-laid in place (Block::Revision; SupportBlock's selected-friend one-hot, 2026-10-03): a resume is refused
with what changed, and a seed starts that block's columns fresh -- at the same width too, where the size check alone
would carry stale weights -- while every other block carries over exactly."""

import pytest
import torch

from animus.bootstrap import seed_trainer
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout
from animus.stages import layout_changes, layout_signature
from test_bootstrap import checkpoint_spec, spec, stage_with


def revised(stage: dict, block: str, revision: int) -> dict:
    for entry in stage["layouts"]["warrior_dps"]["blocks"]:
        if entry["name"] == block:
            entry["revision"] = revision
    return stage


@pytest.mark.parametrize("width", [3, 5], ids=["same width", "widened"])
def test_a_revised_block_refuses_a_resume_and_seeds_fresh(width, capsys):
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16))
    old_stage = stage_with({"warrior_dps": [("core", 4, 3), ("support", 3, 2), ("pvp", 2, 0)]})
    new_stage = revised(stage_with({"warrior_dps": [("core", 4, 3), ("support", width, 2), ("pvp", 2, 0)]}),
                        "support", 1)
    new_obs = 4 + width + 2

    # Resume: the same shapes or not, the signature differs and the message says which block and how.
    assert layout_signature(old_stage, "warrior_dps") != layout_signature(new_stage, "warrior_dps")
    [change] = layout_changes(old_stage, new_stage)
    assert change.startswith("warrior_dps: support revision 0 -> 1")
    assert ("width" in change) == (width != 3)
    assert layout_changes(old_stage, old_stage) == []

    # Seed: support's columns start at zero (as a new block's do), core's and pvp's are the checkpoint's.
    old = MappoTrainer([(9, 5)], 4, config)
    new = MappoTrainer([(new_obs, 5)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("warrior_dps", 9, 5)]),
                  "stage": old_stage}
    seed_trainer(new, checkpoint, spec([Layout("warrior_dps", new_obs, 5)], 4), new_stage, source="stage6 from x")
    assert f"seeding stage6 from x: warrior_dps block support revision 0 -> 1 (width 3 -> {width}): its columns " \
           "start fresh" in capsys.readouterr().out
    seeded, before = new.actor.state_dict()["adapters.0.weight"], old.actor.state_dict()["adapters.0.weight"]
    assert torch.equal(seeded[:, 0:4], before[:, 0:4])                          # core
    assert torch.equal(seeded[:, 4 + width:], before[:, 7:9])                   # pvp, moved past a wider support
    assert not seeded[:, 4:4 + width].any()                                    # support: fresh
