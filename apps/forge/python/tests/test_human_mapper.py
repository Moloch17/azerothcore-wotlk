"""animus.human.mapper: movement mapped back through the emulator, casts through rank chains, selections to slots."""

import json
import re
from pathlib import Path

import numpy as np
import pytest

from animus.human import fit, mapper
from animus.human import reader as r

RANKS_CSV = """first_spell_id,spell_id,rank
116,116,1
116,205,2
116,837,3
133,133,1
133,143,2
2136,2136,1
"""


def manifest(tmp_path):
    path = tmp_path / "mage_duel.json"
    path.write_text(json.dumps({
        "action_names": ["noop", "cancel_queued", "frostbolt", "fireball", "move_forward"],
        "blocks": [{"name": "core", "actions": [0, 4], "catalog": [
            {"kind": "noop"}, {"kind": "cancel_queued"}, {"kind": "spell", "first_rank": 116},
            {"kind": "spell", "first_rank": 133}]},
                   {"name": "move", "actions": [4, 29]}]}))
    return path


def test_movement_mapping_meets_the_target_on_known_sequences():
    clean = mapper.validate_movement(sequences=12, length=40, seed=1)
    assert clean["accuracy"] >= 0.95 and clean["press_accuracy"] >= 0.95
    noisy = mapper.validate_movement(sequences=6, length=40, seed=2, noise_yards=0.05, noise_radians=0.01)
    assert noisy["accuracy"] >= 0.95


def test_move_local_indices_follow_movecontrols():
    """Every emulator action is the move block's action of the same name (MoveControls::NAMES, read from the header)."""
    header = (Path(__file__).resolve().parents[4]
              / "src/server/game/Animus/Scenario/Curriculum/Blocks/MoveControls.h").read_text()
    body = re.search(r"NAMES\s*=\s*\{(.*?)\};", header, re.S).group(1)
    names = re.findall(r'"([a-z_0-9]+)"', body)
    assert len(names) == mapper.MOVE_ACTIONS == 25
    for space in fit.SPACES.values():
        labels = {space.label(i): mapper.move_local(space, i) for i in range(len(space.actions))}
        assert labels.pop("noop") is None
        assert labels == {name: local for local, name in enumerate(names)}


def test_confidence_is_high_where_the_motion_decides(tmp_path):
    space = fit.SPACES["controller"]
    acts = [1] + [0] * 6                    # move_forward, then holding it
    path = fit.rollout(space, acts, {"x": 0.0, "y": 0.0, "facing": 0.0}, np.zeros(8), np.full(8, 7.0))
    h = np.zeros((8, 10))
    h[:, 0] = np.arange(8) * 0.25
    h[:, 1:4] = path[:, :3]
    h[:, 4] = path[:, 3]
    h[:, 8] = 7.0
    got = mapper.map_movement(h, space, start_feet=(0, 0))
    assert got.local[0] == 0 and got.confidence[0] > 0.9


def test_casts_map_through_rank_chains(tmp_path):
    csv_path = tmp_path / "ranks.csv"
    csv_path.write_text(RANKS_CSV)
    ranks = mapper.read_spell_ranks(csv_path)
    catalog = mapper.Catalog.from_manifest(manifest(tmp_path))
    assert mapper.map_cast(837, ranks, catalog) == (2, "frostbolt", 1.0)
    assert mapper.map_cast(143, ranks, catalog)[:2] == (3, "fireball")
    assert mapper.map_cast(2136, ranks, catalog) == (None, "", 0.0)          # Fire Blast: not in this catalog
    assert mapper.map_cast(99999, ranks, catalog)[0] is None
    report = mapper.validate_casts(ranks, catalog)
    assert report["accuracy"] >= 0.99 and report["spells"] == 6


def test_selection_slot_ranks_within_the_same_reaction():
    units = np.zeros(4, r.SNAPSHOT_UNIT)
    units["unit"] = [5, 6, 7, 8]
    units["reaction"] = [0, 2, 0, 0]
    assert mapper.selection_slot(7, units) == 1 and mapper.selection_slot(6, units) == 0
    assert mapper.selection_slot(9, units) is None


def test_spell_ranks_export_from_an_sql_dump(tmp_path):
    dump = tmp_path / "spell_ranks.sql"
    dump.write_text("DROP TABLE IF EXISTS `spell_ranks`;\nINSERT INTO `spell_ranks` VALUES\n(116,116,1),\n"
                    "(116,205,2),\n(133,133,1);\n")
    out = tmp_path / "ranks.csv"
    assert mapper.export_spell_ranks(out, sql_dump=dump) == 3
    assert mapper.read_spell_ranks(out) == {116: 116, 205: 116, 133: 133}


@pytest.mark.parametrize("line,value", [("DOCKER_DB_ROOT_PASSWORD=abc", "abc"),
                                        ("DOCKER_DB_ROOT_PASSWORD = 'x y'", "x y")])
def test_env_file_values(tmp_path, line, value):
    env = tmp_path / ".env"
    env.write_text(f"OTHER=1\n{line}\n")
    assert mapper._env_file_value(env, "DOCKER_DB_ROOT_PASSWORD") == value
