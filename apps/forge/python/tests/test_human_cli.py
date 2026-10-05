"""python -m animus.human: every command on a synthetic hour; prices and companions on known inputs."""

import json
import math

import numpy as np
import pytest

import human_capture_writer as w
from animus.human import __main__ as cli
from animus.human import dataset, fit, motion, prices


@pytest.fixture()
def capture(tmp_path):
    root = tmp_path / "capture"
    w.synthetic_hour(root)
    d = w.hour_dir(root, "2026-10-05", 14)
    comp = [w.header("companion", w.T0)]
    for k in range(10):
        comp.append(w.record(50, {"ms": w.T0 + k * 250, "companion": w.COMPANION, "owner": w.HUMAN,
                                  "model": "warrior_companion", "actions": [3], "goal": 1}))
    comp += [w.record(51, {"ms": w.T0 + 100, "owner": w.HUMAN, "companion": w.COMPANION, "command": 3}),
             w.record(51, {"ms": w.T0 + 200, "owner": w.HUMAN, "companion": w.COMPANION, "command": 2}),
             w.record(52, {"ms": w.T0 + 300, "owner": w.HUMAN, "companion": w.COMPANION, "rating": -1, "reason": 1}),
             w.record(52, {"ms": w.T0 + 400, "owner": w.HUMAN, "companion": w.COMPANION, "rating": 1, "reason": 2})]
    w.write_gz(d / "companion-all.bin.gz", [b"".join(comp)])
    w.write_gz(d / "outcome-1.bin.gz", [w.header("outcome") + w.record(43, {"ms": w.T0, "player": w.COMPANION,
                                                                          "cause": 0})])
    return root


def run(capsys, *argv):
    assert cli.main([str(a) for a in argv]) == 0
    return json.loads(capsys.readouterr().out)


def test_summary_build_realism_companions(capture, tmp_path, capsys):
    out = tmp_path / "out"
    summary = run(capsys, "summary", "--capture", capture, "--out", out)
    assert summary["hours"] == 1 and summary["streams"]["move"]["records"]["Move"] > 100
    assert summary["streams"]["move"]["players"] == 2 and summary["revisions"][0]["module_revision"] == "modsha"
    built = run(capsys, "build", "--capture", capture, "--out", out, "--from", "2026-10-05", "--to", "2026-10-05T14")
    assert built["windows_seen"] > 0 and set(built["files"]) == {"dataset", "reference", "trips", "hard_spots"}
    data = dataset.load(out / "human_motion_windows.npz")
    bot = tmp_path / "run"
    bot.mkdir()
    np.savez(bot / "eval_motion.npz", windows=data["windows"], context=data["context"])
    real = run(capsys, "realism", "--out", out, "--run", bot)
    assert real["mean_emd"] is not None and real["mean_emd"] < 0.2
    # The headline is the evaluations' own number (realism.score): the players against themselves are near 0.
    assert real["realism_emd"] < 0.2 and real["contexts"]
    comp = run(capsys, "companions", "--capture", capture, "--out", out)
    report = json.loads((out / "human_companions.json").read_text())["models"]["warrior_companion"]
    assert comp["models"] == ["warrior_companion"] and report["decisions"] == 10 and report["dismissals"] == 1
    assert report["overrides"] == 1 and report["deaths"] == 1 and report["net_rating"] == 0.0
    assert report["ratings"]["by_reason"]["movement"] == {"up": 0, "down": 1}


def test_fit_prices_and_mapper_validate(capture, tmp_path, capsys):
    out = tmp_path / "out"
    got = run(capsys, "fit", "--capture", capture, "--out", out, "--beam", "8", "--spaces", "controller",
              "controller_125ms")
    assert got["clips"] >= 1 and set(got["expressible"]) == {"controller", "controller_125ms"}
    assert "| controller |" in (out / "human_fit.md").read_text()
    priced = run(capsys, "prices", "--capture", capture, "--out", out, "--beam", "8")
    assert "Actions.Jitter" in priced["proposed"] or json.loads((out / "human_prices.json").read_text())["units"] == 0
    val = run(capsys, "mapper-validate", "--out", out, "--sequences", "3", "--length", "20")
    assert all(m["accuracy"] >= 0.9 for m in val["movement"])


def test_spell_ranks_command_from_a_dump(tmp_path, capsys):
    dump = tmp_path / "spell_ranks.sql"
    dump.write_text("INSERT INTO `spell_ranks` VALUES\n(116,116,1),\n(116,205,2);\n")
    got = run(capsys, "spell-ranks", "--out", tmp_path, "--sql-dump", dump)
    assert got["rows"] == 2 and (tmp_path / "spell_ranks.csv").is_file()


def test_prices_follow_press_and_meet_the_budget():
    space = fit.SPACES["controller"]
    index = {space.label(i): i for i in range(len(space.actions))}
    # MoveControls::Press: a turn rate reversed while held takes back the smaller rate over a quarter turn a second,
    # at once (since 0); the feet reversed are two quarters, at their recency; a rate's effort is its size over the
    # full press, a key's a whole one; the value already held again is no press.
    tally = prices.UnitTally()
    acts = [index[n] for n in ("turn_left_90", "noop", "turn_right_30", "move_forward", "move_forward", "move_stop",
                               "noop", "noop", "noop", "move_back")]
    prices.tally_movement(tally, np.zeros((len(acts) + 1, motion.SAMPLE_DIM)), acts, space)
    assert tally.jitter_amount == pytest.approx([(math.pi / 6) / (math.pi / 2), 2.0])
    assert tally.jitter_since == pytest.approx([0.0, 1.0])
    assert tally.effort == pytest.approx(0.5 + 1 / 6 + 1 + 1 + 1)
    left, right = index["turn_left_90"], index["turn_right_90"]
    units = []
    for jitter_every in (8, 12, 16):
        acts = [index["move_forward"]] + [0] * 239
        for k in range(4, 240, jitter_every):
            acts[k] = left
            acts[k + 2] = right
        path = fit.rollout(space, acts, {"x": 0.0, "y": 0.0, "facing": 0.0}, np.zeros(241), np.full(241, 7.0))
        samples = np.zeros((241, motion.SAMPLE_DIM))
        samples[:, motion.T] = np.arange(241) * 0.25
        samples[:, motion.X:motion.Z + 1] = path[:, :3]
        samples[:, motion.YAW] = path[:, 3]
        samples[:, motion.SPEED] = 7.0
        tally = prices.UnitTally()
        prices.tally_movement(tally, samples, acts, space)
        prices.tally_casts(tally, np.array([0, 1000, 2000, 3000, 4000, 30000]), np.array([5, 5, 5, 5, 5, 6]))
        units.append(tally)
    assert units[0].repeated == 2 and units[0].max_in_window == 5
    report = prices.propose(units, budget=0.005)
    p = report["proposed"]
    med = report["median_human"]
    assert p["Actions.Jitter"] * med["jitter_weight_per_min"] <= 0.005 + 1e-9
    assert p["Actions.Effort"] * med["effort_per_min"] <= 0.005 + 1e-9
    assert p["Actions.RepeatFree"] == 5 and p["Options.JitterDecayMs"] in prices.DECAY_GRID
    assert "| Actions.Jitter |" in prices.markdown(report)
