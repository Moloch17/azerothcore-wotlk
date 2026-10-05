"""The move block re-laid as a player's keys and mouse (player-controller C3: MoveBlock revision 2, MoveControls.h) and
the core block's option clocks shrunk with it (core revision 1). A checkpoint from the bearing design -- move revision
0 (a stage.json with no revision) or 1 -- is refused on resume, and a seed starts the move and core blocks fresh while
every other block carries over. The header's own revisions are read, so the test follows the C++."""

import re
from pathlib import Path

import pytest
import torch

from animus.bootstrap import seed_trainer
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout
from animus.stages import layout_changes, layout_signature
from test_bootstrap import checkpoint_spec, spec, stage_with

ROOT = Path(__file__).resolve().parents[4]
CURRICULUM = ROOT / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum" / "Blocks"


def header_revision(path: Path, pattern: str) -> int:
    return int(re.search(pattern, path.read_text()).group(1))


MOVE_REVISION = header_revision(CURRICULUM / "MoveControls.h", r"constexpr uint32_t REVISION = (\d+);")
CORE_REVISION = header_revision(CURRICULUM / "CoreBlock.h", r"Revision\(\) const override \{ return (\d+); \}")
MOVE_ACTIONS = len(re.findall(r'"[a-z_0-9]+"', re.search(r"NAMES =\s*\{(.*?)\};", (CURRICULUM / "MoveControls.h")
                                                                   .read_text(), re.S).group(1)))


def with_revisions(stage: dict, revisions: dict[str, int]) -> dict:
    for entry in stage["layouts"]["warrior_dps"]["blocks"]:
        if entry["name"] in revisions and revisions[entry["name"]]:
            entry["revision"] = revisions[entry["name"]]
    return stage


def test_the_header_lays_out_the_controller():
    assert MOVE_REVISION >= 2 and CORE_REVISION >= 1
    assert MOVE_ACTIONS == 25


@pytest.mark.parametrize("old_move", [0, 1], ids=["move revision 0", "move revision 1"])
def test_a_bearing_checkpoint_refuses_a_resume_and_seeds_the_move_block_fresh(old_move, capsys):
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16))
    # The bearing design: core (its five option clocks), move (31 or 33 actions, here 4), pvp. Same widths of move as
    # the new one would be no safer: the revision alone has to refuse it.
    old_stage = with_revisions(stage_with({"warrior_dps": [("core", 5, 3), ("move", 6, 4), ("pvp", 2, 0)]}),
                               {"move": old_move})
    new_stage = with_revisions(stage_with({"warrior_dps": [("core", 4, 3), ("move", 6, 5), ("pvp", 2, 0)]}),
                               {"move": MOVE_REVISION, "core": CORE_REVISION})

    # Resume: refused, and the message names both blocks and how they changed.
    assert layout_signature(old_stage, "warrior_dps") != layout_signature(new_stage, "warrior_dps")
    [change] = layout_changes(old_stage, new_stage)
    assert f"move revision {old_move} -> {MOVE_REVISION}" in change
    assert f"core revision 0 -> {CORE_REVISION}" in change

    # Seed: move and core start fresh (zero columns, the head's own init), pvp carries over exactly.
    old = MappoTrainer([(13, 7)], 4, config)
    new = MappoTrainer([(12, 8)], 4, config)
    fresh_head = new.actor.state_dict()["heads.0.weight"].clone()
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("warrior_dps", 13, 7)]),
                  "stage": old_stage}
    seed_trainer(new, checkpoint, spec([Layout("warrior_dps", 12, 8)], 4), new_stage, source="stage1 from x")
    out = capsys.readouterr().out
    assert f"block move revision {old_move} -> {MOVE_REVISION}" in out and "its columns start fresh" in out
    assert f"block core revision 0 -> {CORE_REVISION}" in out

    seeded = new.actor.state_dict()["adapters.0.weight"]
    before = old.actor.state_dict()["adapters.0.weight"]
    assert not seeded[:, 0:10].any()                                       # core and move: fresh
    assert torch.equal(seeded[:, 10:12], before[:, 11:13])                 # pvp: carried over
    head = new.actor.state_dict()["heads.0.weight"]
    assert torch.equal(head, fresh_head)                                   # no action of either block carries over


def test_a_controller_checkpoint_resumes():
    stage = with_revisions(stage_with({"warrior_dps": [("core", 4, 3), ("move", 6, 5)]}),
                           {"move": MOVE_REVISION, "core": CORE_REVISION})
    assert layout_changes(stage, stage) == []
