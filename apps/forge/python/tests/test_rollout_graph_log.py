"""The rollout graph log lines (CPU): the capture itself needs a GPU, the line saying graphs are on, or why they are not,
does not. The graph class is replaced by a stand-in."""
import pytest
import torch

from animus.mappo import trainer as trainer_module
from animus.mappo.trainer import MappoConfig, MappoTrainer, announce_graph


def make_trainer(**overrides):
    return MappoTrainer([(7, 5)], 11, MappoConfig(hidden=(16, 16), **overrides))


class Layout:
    shape = (6, 2)


def arrays():
    return (torch.zeros(6, 2, 7), torch.zeros(6, 2, 5, dtype=torch.bool), Layout(), torch.zeros(6, 11))


def test_announce_prints_each_key_once():
    seen, lines = set(), []
    assert announce_graph(seen, "a", "first", lines.append)
    assert not announce_graph(seen, "a", "again", lines.append)
    assert announce_graph(seen, "b", "other", lines.append)
    assert lines == ["first", "other"]


def test_the_first_capture_of_a_batch_shape_logs_its_shape_once(monkeypatch, capsys):
    trainer = make_trainer()
    trainer._rollout_stream = object()
    monkeypatch.setattr(trainer_module, "_RolloutGraph", lambda *args: object())
    state = trainer.acting_state(2, 1)
    obs, mask, layout, features = arrays()
    for _ in range(3):
        assert trainer._rollout_graph(obs, mask, layout, features, False, state) is not None
    lines = capsys.readouterr().out.splitlines()
    assert len(lines) == 1
    assert "rollout graph captured: 6 envs x 2 agents" in lines[0] and "obs 7" in lines[0] and "sampled" in lines[0]
    trainer._rollout_graph(obs, mask, layout, features, True, state)
    assert "deterministic" in capsys.readouterr().out


@pytest.mark.parametrize("setup, reason", [
    (lambda t: setattr(t, "_rollout_stream", None), "not on a GPU"),
    (lambda t: setattr(t.config, "rollout_graphs", False), "mappo.rollout_graphs is off"),
    (lambda t: setattr(t, "seat_sets", [[]]), "seat sets are on"),
])
def test_graphs_not_used_logs_the_reason_once(setup, reason, capsys):
    trainer = make_trainer()
    trainer._rollout_stream = object()
    setup(trainer)
    state = trainer.acting_state(2, 1)
    for _ in range(3):
        assert trainer._rollout_graph(*arrays(), False, state) is None
    lines = capsys.readouterr().out.splitlines()
    assert len(lines) == 1 and "rollout graphs NOT used" in lines[0] and reason in lines[0]


def test_no_acting_state_is_a_logged_reason_too(capsys):
    trainer = make_trainer()
    trainer._rollout_stream = object()
    assert trainer._rollout_graph(*arrays(), False, None) is None
    assert "no acting state" in capsys.readouterr().out
