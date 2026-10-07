"""apps/forge/tools/stage_json_diff.py: what changed between two builds' stage.json for a stage."""

import copy
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "apps" / "forge" / "tools"))

import stage_json_diff as sjd  # noqa: E402

#: The live M2 run's stage.json (in the dev container /azerothcore is the host's checkout) and the host's backup of it.
LIVE = [Path("/azerothcore/var/animus-forge/shared/runs/move2_seek/stage.json"),
        ROOT / "var" / "animus-forge" / "shared" / "runs" / "move2_seek" / "stage.json",
        Path("/azerothcore/var/backups/2026-10-07/move2_seek/stage.json"),
        ROOT / "var" / "backups" / "2026-10-07" / "move2_seek" / "stage.json"]


def sample() -> dict:
    return {
        "format": 3, "stage": "move2_seek", "suffix": "_seek", "extends": "move1_controls", "summary": "seek",
        "seats": 1, "state": {"arena_first": 14, "arena_count": 16}, "models": {"warrior": "warrior_seek"},
        "seed_chain": ["move1_controls"], "cast": [],
        "layouts": {
            "warrior": {
                "obs_dim": 100, "num_actions": 5, "action_names": ["move", "turn", "jump", "look", "stop"],
                "spec_names": ["arms", "fury"], "spec_roles": ["damage", "damage"], "sets": [],
                "blocks": [
                    {"name": "core", "obs": [0, 10], "actions": [0, 2], "obs_names": ["hp", "mana", "level"]},
                    {"name": "move", "obs": [10, 90], "actions": [2, 3], "revision": 1}]},
            "mage": {"obs_dim": 100, "num_actions": 5, "action_names": ["move", "turn", "jump", "look", "stop"],
                     "spec_names": [], "spec_roles": [], "sets": [], "blocks": []}},
        "episode_info": ["seat", "found", "died", "reward_arrive", "reward_death", "opponent_seat"],
        "episode_categories": {"seek_room": ["a", "b"]},
        "reward_terms": {"arrive": "outcome", "death": "cost", "sighting": "shaping", "owner_pull": "shaping"},
        "goals": {"kinds": ["fight"], "targets": 4},
        "tuning": {"Seek.Arrive": 10.0, "Seek.StepCost": 0.01, "Opponent.Level": 3, "Move.Wall": 0.5},
        "arenas": [{"name": "rooms", "weight": 1, "ambushers": 0, "eval_only": False},
                   {"name": "sweep", "weight": 1, "ambushers": 0, "eval_only": True}],
    }


def kinds(differences) -> set[str]:
    return {d.kind for d in differences}


def write(tmp_path: Path, name: str, document: dict) -> Path:
    path = tmp_path / name
    path.write_text(json.dumps(document))
    return path


# ------------------------------------------------------------------------------------------ synthetic files

def test_identical_files_have_no_difference_and_exit_zero(tmp_path, capsys):
    old = write(tmp_path, "old.json", sample())
    assert sjd.compare(sample(), sample()) == []
    assert sjd.main([str(old), str(old)]) == 0
    assert "identical" in capsys.readouterr().out


def test_every_kind_of_change_is_reported_under_its_kind():
    new = sample()
    new["format"] = 4
    new["layouts"]["warrior"]["obs_dim"] = 120
    new["layouts"]["warrior"]["action_names"] = ["move", "jump", "turn", "look", "stop"]
    new["layouts"]["warrior"]["spec_names"] = ["arms"]
    new["layouts"]["warrior"]["blocks"][0]["obs_names"] = ["hp", "level", "rage"]
    new["layouts"]["warrior"]["blocks"][1]["obs"] = [10, 95]
    new["layouts"]["warrior"]["blocks"][1]["revision"] = 2
    del new["layouts"]["mage"]
    new["layouts"]["druid"] = copy.deepcopy(new["layouts"]["warrior"])
    new["episode_info"] = ["seat", "found", "died", "sighted"]
    new["episode_categories"]["seek_object"] = ["chest"]
    new["reward_terms"]["arrive"] = "shaping"
    new["tuning"]["Seek.Arrive"] = 12.0
    new["arenas"][0]["weight"] = 2
    new["goals"]["targets"] = 8
    differences = sjd.compare(sample(), new)
    assert kinds(differences) == {"header", "layouts", "layout-shape", "actions", "specs", "blocks", "obs-names",
                                  "episode-info", "categories", "reward-terms", "tuning", "arenas", "other"}
    text = "\n".join(str(d) for d in differences)
    assert "layouts.warrior.blocks.move.obs: [10, 90] -> [10, 95]" in text
    assert "layouts.warrior.blocks.move.revision: 1 -> 2" in text
    assert "class removed" in text and "class added" in text
    assert "reordered" in text                                  # the action names only changed places
    assert "removed 1: \"rage\"" not in text and 'added 1: "rage"' in text   # the obs_names column
    assert "reward_terms.arrive: category outcome -> shaping" in text
    assert "tuning.Seek.Arrive: 10.0 -> 12.0" in text
    assert "goals.targets: 4 -> 8" in text


def test_a_removed_reward_term_is_refused_without_its_flag_and_listed_with_it(tmp_path, capsys):
    new = sample()
    del new["reward_terms"]["owner_pull"]
    old_path, new_path = write(tmp_path, "old.json", sample()), write(tmp_path, "new.json", new)
    assert sjd.main([str(old_path), str(new_path)]) == 1
    assert "reward_terms.owner_pull: removed" in capsys.readouterr().out
    assert sjd.main([str(old_path), str(new_path), "--allow-removed-terms"]) == 0
    out = capsys.readouterr().out
    assert "ALLOWED (1)" in out and "by --allow-removed-terms (1)" in out and "owner_pull" in out
    # The wrong flag does not cover it.
    assert sjd.main([str(old_path), str(new_path), "--allow-removed-keys"]) == 1


def test_removed_tuning_keys_and_arena_fields_need_the_keys_flag(tmp_path, capsys):
    new = sample()
    del new["tuning"]["Opponent.Level"]
    del new["tuning"]["Move.Wall"]
    for arena in new["arenas"]:
        del arena["ambushers"]
    old_path, new_path = write(tmp_path, "old.json", sample()), write(tmp_path, "new.json", new)
    assert sjd.main([str(old_path), str(new_path)]) == 1
    capsys.readouterr()
    assert sjd.main([str(old_path), str(new_path), "--allow-removed-keys"]) == 0
    out = capsys.readouterr().out
    assert "by --allow-removed-keys (4)" in out
    for name in ("tuning.Opponent.Level", "tuning.Move.Wall", "arenas.rooms.ambushers", "arenas.sweep.ambushers"):
        assert name in out


def test_added_terms_keys_and_columns_are_never_allowed(tmp_path):
    new = sample()
    new["reward_terms"]["fresh"] = "shaping"
    new["tuning"]["Seek.New"] = 1
    new["episode_info"].append("fresh_column")
    old_path, new_path = write(tmp_path, "old.json", sample()), write(tmp_path, "new.json", new)
    flags = ["--allow-removed-terms", "--allow-removed-keys", "--allow-removed-columns"]
    assert sjd.main([str(old_path), str(new_path), *flags]) == 1
    refused, allowed = sjd.split(sjd.compare(sample(), new), {"terms", "keys", "columns"})
    assert len(refused) == 3 and not allowed


def test_a_removed_column_needs_its_own_flag_and_a_changed_value_is_never_allowed(tmp_path):
    new = sample()
    new["episode_info"].remove("opponent_seat")
    new["tuning"]["Seek.Arrive"] = 11.0
    old_path, new_path = write(tmp_path, "old.json", sample()), write(tmp_path, "new.json", new)
    assert sjd.main([str(old_path), str(new_path), "--allow-removed-columns", "--allow-removed-keys",
                     "--allow-removed-terms"]) == 1   # the changed tuning value stays
    refused, allowed = sjd.split(sjd.compare(sample(), new), {"terms", "keys", "columns"})
    assert [d.where for d in refused] == ["tuning.Seek.Arrive"]
    assert [d.removal for d in allowed] == ["columns"]


def test_a_top_level_field_the_new_file_drops_is_a_removed_key():
    new = sample()
    del new["cast"]
    differences = sjd.compare(sample(), new)
    assert [(d.kind, d.where, d.removal) for d in differences] == [("other", "cast", "keys")]


def test_bad_input_exits_two(tmp_path, capsys):
    bad = tmp_path / "bad.json"
    bad.write_text("{")
    good = write(tmp_path, "good.json", sample())
    assert sjd.main([str(bad), str(good)]) == 2
    assert sjd.main([str(good), str(tmp_path / "missing.json")]) == 2
    assert "stage_json_diff:" in capsys.readouterr().err


# --------------------------------------------------------------------------------------------- the real file

@pytest.fixture(scope="module")
def live_stage_json() -> Path:
    for path in LIVE:
        if path.is_file():
            return path
    pytest.skip("no move2_seek stage.json on this machine (the live run's or the host's backup)")


def test_the_live_m2_file_against_itself_is_identical(live_stage_json, capsys):
    assert sjd.main([str(live_stage_json), str(live_stage_json)]) == 0
    assert "identical" in capsys.readouterr().out


def test_the_live_m2_file_against_a_mutated_copy(live_stage_json, tmp_path, capsys):
    document = json.loads(live_stage_json.read_text())
    mutated = copy.deepcopy(document)
    # A reward term gone, a tuning key gone and another changed, a column dropped, a block moved: the shapes the
    # cleanup's stage.json differ by, and the one that must stop a resume (the span).
    term = sorted(mutated["reward_terms"])[0]
    del mutated["reward_terms"][term]
    gone_key, changed_key = sorted(mutated["tuning"])[:2]
    del mutated["tuning"][gone_key]
    mutated["tuning"][changed_key] = "changed"
    dropped = mutated["episode_info"].pop(5)
    layout = sorted(mutated["layouts"])[0]
    block = mutated["layouts"][layout]["blocks"][1]
    block["obs"] = [block["obs"][0], block["obs"][1] + 1]
    old_path, new_path = tmp_path / "old.json", write(tmp_path, "new.json", mutated)
    old_path.write_text(live_stage_json.read_text())

    assert sjd.main([str(old_path), str(new_path)]) == 1
    out = capsys.readouterr().out
    for expected in (f"reward_terms.{term}", f"tuning.{gone_key}", f"tuning.{changed_key}", "episode_info",
                     f"layouts.{layout}.blocks.{block['name']}.obs"):
        assert expected in out, expected
    flags = ["--allow-removed-terms", "--allow-removed-keys", "--allow-removed-columns"]
    assert sjd.main([str(old_path), str(new_path), *flags]) == 1       # the changed value and the span remain
    out = capsys.readouterr().out
    assert "2 difference(s) not allowed, 3 allowed" in out and dropped in out
    # The live file itself was only read.
    assert live_stage_json.read_text() == old_path.read_text()
