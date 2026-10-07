"""PartyFrames revision 2 (G1, 2026-10-07): the one source of party-member state, and what seeding by name carries.

The combat block's party members' frames moved into the party frames block (CombatBlock revision 1 keeps the player
frame, the pet frame and the target frame; PartyFramesBlock revision 2 has the members' frames, M4's ten columns and
the combat block's member features and the member's target, and select/focus/assist presses). Both blocks name their
columns, so:

- a C3 checkpoint on the old revisions (combat 0) seeds the new combat block by name: the player frame, the pet frame,
  the target frame and their presses carry; the member frames do not (the combat stages had no party);
- an M4 checkpoint on revision 1 seeds revision 2 by name: its forty columns where their names are now; the combat
  features and the target start at zero;
- G1 seeds from C3 with M4's party frames merged in by name (bootstrap.seed_merges, the overseer's second-source
  ruling): what carries from each source.

The columns' names here are the C++ blocks' own (checked against their sources first)."""

import re
from pathlib import Path
from types import SimpleNamespace

import pytest
import torch

from animus.bootstrap import seed_merges, seed_trainer
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout

REPO = Path(__file__).resolve().parents[4]
BLOCKS = REPO / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum" / "Blocks"
LAYOUT = "priest"
HIDDEN = (16, 16)

# CombatBlock.cpp's FRAME_NAMES and TARGET_NAMES; PartyFramesBlock::FeatureName in its order.
FRAME = ("present", "alive", "health", "power", "mana_user", "in_range", "in_combat", "debuffs", "dispellable",
         "aggro", "selected", "focused")
TARGET = ("present", "hostile", "friendly", "in_view", "dead", "threat", "threat_pct", "tot_self", "tot_pet",
          "tot_party", "tot_other", "debuffs")
MEMBER_R1 = ("present", "alive", "leader", "in_combat", "health", "power", "dot", "dot_right", "dot_forward",
             "dot_distance")
MEMBER_R2 = MEMBER_R1 + ("mana_user", "in_range", "debuffs", "dispellable", "aggro", "selected", "focused", "target",
                         "target_hostile", "target_mine", "target_in_view")
MEMBERS = 4


def test_the_names_are_the_blocks_own():
    combat = (BLOCKS / "CombatBlock.cpp").read_text()
    frames = re.search(r"FRAME_NAMES\[Combat::FRAME_FEATURES\] = \{(.*?)\};", combat, re.S).group(1)
    targets = re.search(r"TARGET_NAMES\[Combat::TARGET_FEATURES\] = \{(.*?)\};", combat, re.S).group(1)
    assert tuple(re.findall(r'"(\w+)"', frames)) == FRAME
    assert tuple(re.findall(r'"(\w+)"', targets)) == TARGET
    party = (BLOCKS / "PartyFramesBlock.cpp").read_text()
    body = party[party.index("::FeatureName(uint32 feature)"):]
    assert tuple(re.findall(r'case FRAME_\w+:\s*return "(\w+)";', body)) == MEMBER_R2
    assert re.search(r'"member\{\}_\{\}"', party), "member{i}_<feature>: revision 1's names"
    assert re.search(r'"\{\}_member\{\}"', party), "select_member{i}, focus_member{i}, assist_member{i}"
    assert "REVISION = 2" in (BLOCKS / "PartyFramesBlock.h").read_text()
    assert "REVISION = 1" in (BLOCKS / "CombatBlock.h").read_text()


# ------------------------------------------------------------------ the layouts, as stage.json writes them

def combat_block(revision: int):
    frames = ("self", "pet") if revision else ("self", "pet", *(f"member_{i}" for i in range(MEMBERS)))
    names = [f"frame_{frame}_{f}" for frame in frames for f in FRAME] + [f"target_{f}" for f in TARGET]
    actions = [f"{press}_frame_{frame}" for press in ("select", "focus") for frame in frames]
    return ("combat", revision, actions, names)


def party_frames_block(revision: int):
    features = MEMBER_R2 if revision == 2 else MEMBER_R1
    names = [f"member{i}_{f}" for i in range(MEMBERS) for f in features]
    actions = [f"{press}_member{i}" for press in ("select", "focus", "assist") for i in range(MEMBERS)] \
        if revision == 2 else []
    return ("party_frames", revision, actions, names)


CORE = ("core", 1, ["core_0", "core_1", "core_2"], 5)
MOVE = ("move", 5, [f"move_key_{i}" for i in range(4)], 6)
SIGHT = ("sight", 2, [f"select_{i}" for i in range(3)], 7)
GOAL = ("goal", 0, [], 2)


def stage(blocks):
    obs = actions = 0
    spans, action_names = [], []
    for name, revision, block_actions, columns in blocks:
        width = len(columns) if isinstance(columns, list) else columns
        entry = {"name": name, "obs": [obs, width], "actions": [actions, len(block_actions)]}
        if revision:
            entry["revision"] = revision
        if isinstance(columns, list):
            entry["obs_names"] = columns
        spans.append(entry)
        action_names += block_actions
        obs += width
        actions += len(block_actions)
    return {"layouts": {LAYOUT: {"obs_dim": obs, "num_actions": actions, "blocks": spans,
                                 "action_names": action_names}}}


C3_OLD = stage([CORE, MOVE, SIGHT, combat_block(0), GOAL])
C3_NEW = stage([CORE, MOVE, SIGHT, combat_block(1), GOAL])
M4_R1 = stage([CORE, MOVE, party_frames_block(1), GOAL])
M4_R2 = stage([CORE, MOVE, party_frames_block(2), GOAL])
G1 = stage([CORE, MOVE, SIGHT, party_frames_block(2), combat_block(1), GOAL])


def dims(stage_json):
    entry = stage_json["layouts"][LAYOUT]
    return entry["obs_dim"], entry["num_actions"]


def spans(stage_json):
    return {block["name"]: block for block in stage_json["layouts"][LAYOUT]["blocks"]}


def column(stage_json, name: str) -> int:
    for block in stage_json["layouts"][LAYOUT]["blocks"]:
        if name in block.get("obs_names", ()):
            return block["obs"][0] + block["obs_names"].index(name)
    raise KeyError(name)


def action(stage_json, name: str) -> int:
    return stage_json["layouts"][LAYOUT]["action_names"].index(name)


def trained(stage_json, seed: int) -> MappoTrainer:
    """A trainer for `stage_json` whose weights and observation statistics are its own (seeded at random, the
    normalisers' means set apart), so what carries from it can be told from anything else."""
    torch.manual_seed(seed)
    trainer = MappoTrainer([dims(stage_json)], 4, MappoConfig(hidden=HIDDEN))
    for network in (trainer.actor, trainer.critic):
        state = network.state_dict()
        state["norms.0.mean"].copy_(torch.rand_like(state["norms.0.mean"]) + float(seed))
        state["norms.0.var"].copy_(torch.rand_like(state["norms.0.var"]) + 1.0 + float(seed))
        state["norms.0.count"].fill_(1.0e6)
        network.load_state_dict(state)
    return trainer


def checkpoint(trainer: MappoTrainer, stage_json) -> dict:
    obs, actions = dims(stage_json)
    return {"trainer": trainer.state_dict(), "stage": stage_json,
            "spec": {"layouts": [{"name": LAYOUT, "obs_dim": obs, "num_actions": actions}]}}


def fresh(stage_json) -> MappoTrainer:
    torch.manual_seed(99)
    return MappoTrainer([dims(stage_json)], 4, MappoConfig(hidden=HIDDEN))


def the_spec(stage_json):
    return SimpleNamespace(layouts=(Layout(LAYOUT, *dims(stage_json)),), state_dim=4)


def weights(trainer: MappoTrainer, network: str = "actor"):
    state = getattr(trainer, network).state_dict()
    return state["adapters.0.weight"], state["heads.0.weight"] if network == "actor" else None


def same_column(new, new_stage, old, old_stage, name: str, network: str = "actor") -> bool:
    new_w = getattr(new, network).state_dict()["adapters.0.weight"][:, column(new_stage, name)]
    old_w = getattr(old, network).state_dict()["adapters.0.weight"][:, column(old_stage, name)]
    return torch.equal(new_w, old_w)


def same_norm(new, new_stage, old, old_stage, name: str) -> bool:
    new_s, old_s = new.actor.state_dict(), old.actor.state_dict()
    return all(torch.equal(new_s[f"norms.0.{stat}"][column(new_stage, name)],
                           old_s[f"norms.0.{stat}"][column(old_stage, name)]) for stat in ("mean", "var"))


def same_action(new, new_stage, old, old_stage, name: str) -> bool:
    new_s, old_s = new.actor.state_dict(), old.actor.state_dict()
    a, b = action(new_stage, name), action(old_stage, name)
    return torch.equal(new_s["heads.0.weight"][a], old_s["heads.0.weight"][b]) and \
        torch.equal(new_s["heads.0.bias"][a], old_s["heads.0.bias"][b])


def zero_column(trainer, stage_json, name: str) -> bool:
    return torch.count_nonzero(trainer.actor.state_dict()["adapters.0.weight"][:, column(stage_json, name)]) == 0


# ------------------------------------------------------------------ C3 on the old revisions -> the new combat block

def test_a_c3_checkpoint_on_the_old_revisions_seeds_the_new_combat_block():
    old = trained(C3_OLD, 1)
    new = fresh(C3_NEW)
    assert seed_trainer(new, checkpoint(old, C3_OLD), the_spec(C3_NEW), C3_NEW) == [LAYOUT]

    # The player frame, the pet frame and the target frame, with their statistics, by name in the revised block.
    for name in [f"frame_{frame}_{f}" for frame in ("self", "pet") for f in FRAME] + [f"target_{f}" for f in TARGET]:
        for network in ("actor", "critic"):
            assert same_column(new, C3_NEW, old, C3_OLD, name, network), name
        assert same_norm(new, C3_NEW, old, C3_OLD, name), name
    # Their presses, by name.
    for press in ("select", "focus"):
        for frame in ("self", "pet"):
            assert same_action(new, C3_NEW, old, C3_OLD, f"{press}_frame_{frame}")
    # The other blocks whole.
    for block in ("core", "move", "sight", "goal"):
        new_b, old_b = spans(C3_NEW)[block], spans(C3_OLD)[block]
        first, width = new_b["obs"]
        old_first = old_b["obs"][0]
        torch.testing.assert_close(new.actor.state_dict()["adapters.0.weight"][:, first:first + width],
                                   old.actor.state_dict()["adapters.0.weight"][:, old_first:old_first + width])
    # The trunk.
    for key, tensor in new.actor.state_dict().items():
        if key.startswith("trunk."):
            torch.testing.assert_close(tensor, old.actor.state_dict()[key])
    # Every column of the new combat block came from the old one: nothing at zero.
    combat = spans(C3_NEW)["combat"]
    first, width = combat["obs"]
    assert torch.count_nonzero(new.actor.state_dict()["adapters.0.weight"][:, first:first + width].abs().sum(0)) \
        == width


# ------------------------------------------------------------------ M4 on revision 1 -> revision 2

def test_an_m4_checkpoint_on_revision_1_seeds_revision_2_by_name():
    old = trained(M4_R1, 2)
    new = fresh(M4_R2)
    assert seed_trainer(new, checkpoint(old, M4_R1), the_spec(M4_R2), M4_R2) == [LAYOUT]
    for i in range(MEMBERS):
        for f in MEMBER_R1:
            name = f"member{i}_{f}"
            assert same_column(new, M4_R2, old, M4_R1, name), name
            assert same_column(new, M4_R2, old, M4_R1, name, "critic"), name
            assert same_norm(new, M4_R2, old, M4_R1, name), name
        # The combat block's member features and the member's target are new: at zero, so the seeded policy plays
        # as M4's did until it learns them.
        for f in MEMBER_R2[len(MEMBER_R1):]:
            assert zero_column(new, M4_R2, f"member{i}_{f}"), f
    # Core, move and goal carry whole, and their actions.
    for name in CORE[2] + MOVE[2]:
        assert same_action(new, M4_R2, old, M4_R1, name), name


# ------------------------------------------------------------------ G1: C3 and M4's party frames, by name

@pytest.mark.parametrize("c3_stage", [C3_OLD, C3_NEW], ids=["c3_old_revisions", "c3_new_revisions"])
@pytest.mark.parametrize("m4_stage", [M4_R1, M4_R2], ids=["m4_revision_1", "m4_revision_2"])
def test_g1_seeds_from_c3_with_m4s_party_frames_merged_in_by_name(c3_stage, m4_stage):
    c3 = trained(c3_stage, 3)
    m4 = trained(m4_stage, 4)
    new = fresh(G1)
    base = checkpoint(c3, c3_stage)
    spec = the_spec(G1)
    assert seed_trainer(new, base, spec, G1) == [LAYOUT]
    assert seed_merges(new, [checkpoint(m4, m4_stage)], spec, G1, base) == {LAYOUT: ["party_frames"]}
    actor = new.actor.state_dict()

    # From C3: the trunk and the memory, the core, move and sight blocks, the combat block (the player, pet and target
    # frames) and its presses -- none of it M4's, though M4 has a core and a move block too.
    for key, tensor in actor.items():
        if key.startswith("trunk."):
            torch.testing.assert_close(tensor, c3.actor.state_dict()[key])
    for block in ("core", "move", "sight"):
        first, width = spans(G1)[block]["obs"]
        old_first = spans(c3_stage)[block]["obs"][0]
        torch.testing.assert_close(actor["adapters.0.weight"][:, first:first + width],
                                   c3.actor.state_dict()["adapters.0.weight"][:, old_first:old_first + width])
    for name in [f"frame_{frame}_{f}" for frame in ("self", "pet") for f in FRAME] + [f"target_{f}" for f in TARGET]:
        assert same_column(new, G1, c3, c3_stage, name), name
        assert same_norm(new, G1, c3, c3_stage, name), name
    for name in CORE[2] + MOVE[2] + SIGHT[2] + ["select_frame_self", "focus_frame_pet"]:
        assert same_action(new, G1, c3, c3_stage, name), name

    # From M4: every party frames column it names, with its statistics, actor's and critic's -- the leader, the
    # members' health and power, the minimap's dots.
    m4_features = MEMBER_R2 if m4_stage is M4_R2 else MEMBER_R1
    for i in range(MEMBERS):
        for f in m4_features:
            name = f"member{i}_{f}"
            assert same_column(new, G1, m4, m4_stage, name), name
            assert same_column(new, G1, m4, m4_stage, name, "critic"), name
            assert same_norm(new, G1, m4, m4_stage, name), name
        # What neither source has (an M4 of revision 1 has no combat features or target): at zero. C3's member frames
        # (revision 0's frame_member_*) never reach the party frames: a combat stage had no party to read.
        for f in MEMBER_R2[len(m4_features):]:
            assert zero_column(new, G1, f"member{i}_{f}"), f
    # The frames' presses: M4's by name where it has them (revision 2), else as fresh as any new action.
    if m4_stage is M4_R2:
        for name in ("select_member0", "focus_member2", "assist_member3"):
            assert same_action(new, G1, m4, m4_stage, name), name
    else:
        untouched = fresh(G1).actor.state_dict()
        index = action(G1, "assist_member0")
        torch.testing.assert_close(actor["heads.0.weight"][index], untouched["heads.0.weight"][index])
