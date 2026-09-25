"""Resuming a run whose columns have changed since it started."""

import csv

from animus.train import RunLogger


def read(path):
    with path.open(newline="") as f:
        return list(csv.reader(f))


def test_a_resumed_run_with_the_same_columns_appends(tmp_path):
    RunLogger(tmp_path, ["update", "loss"]).log(1, {"update": 1, "loss": 0.5})
    RunLogger(tmp_path, ["update", "loss"], append=True).log(2, {"update": 2, "loss": 0.4})

    rows = read(tmp_path / "metrics.csv")
    assert rows[0] == ["update", "loss"]
    assert len(rows) == 3
    assert not list(tmp_path.glob("metrics-before-*.csv"))


def test_a_new_column_rotates_the_file_instead_of_being_dropped(tmp_path):
    """The failure this guards: a column added mid-run was written row after row into a header that had no
    place for it, and extrasaction="ignore" meant nothing said so."""
    RunLogger(tmp_path, ["update", "loss"]).log(1, {"update": 1, "loss": 0.5})
    RunLogger(tmp_path, ["update", "loss", "entropy"], append=True).log(2, {"update": 2, "loss": 0.4,
                                                                            "entropy": 1.2})

    rows = read(tmp_path / "metrics.csv")
    assert rows[0] == ["update", "loss", "entropy"]
    assert rows[1] == ["2", "0.4", "1.2"]

    moved = list(tmp_path.glob("metrics-before-*.csv"))
    assert len(moved) == 1
    assert read(moved[0])[0] == ["update", "loss"]


def test_layouts_rotate_too(tmp_path):
    """layouts.csv took its field names from the current rows while appending under the old header, so the
    values landed under the wrong names entirely."""
    first = RunLogger(tmp_path, ["update"])
    first.log_layouts([{"update": 1, "layout": "mage_dps", "killed": 0.5}])
    first._layout_file.close()

    second = RunLogger(tmp_path, ["update"], append=True)
    second.log_layouts([{"update": 2, "layout": "mage_dps", "killed": 0.5, "revives": 3.0}])
    second._layout_file.close()

    rows = read(tmp_path / "layouts.csv")
    assert rows[0] == ["update", "layout", "killed", "revives"]
    assert len(list(tmp_path.glob("layouts-before-*.csv"))) == 1
