"""Unknown keys: dropped (and named) from a checkpoint's saved config, an error in a yaml."""
from dataclasses import asdict

import pytest

from animus.config import TrainConfig
from animus.evaluate import mappo_from_checkpoint
from animus.mappo.trainer import MappoConfig


def test_a_checkpoints_saved_config_drops_options_this_build_lacks_and_says_which():
    saved = {**asdict(TrainConfig().mappo), "removed_option": 1, "another_old_one": True}
    lines = []
    config = mappo_from_checkpoint(saved, emit=lines.append)
    assert isinstance(config, MappoConfig) and isinstance(config.hidden, tuple)
    assert len(lines) == 1 and "another_old_one, removed_option" in lines[0] and "2 option(s)" in lines[0]
    quiet = []
    mappo_from_checkpoint(asdict(TrainConfig().mappo), emit=quiet.append)
    assert quiet == []


def test_a_yaml_with_an_unknown_mappo_key_stays_an_error(tmp_path):
    path = tmp_path / "live.yaml"
    path.write_text("mappo:\n  entropy_coeff: 0.01\n")   # a typo for entropy_coef
    with pytest.raises(ValueError, match="unknown config keys.*mappo.entropy_coeff"):
        TrainConfig.load(path)
    path.write_text("mappo:\n  entropy_coef: 0.01\n")
    assert TrainConfig.load(path).mappo.entropy_coef == 0.01
