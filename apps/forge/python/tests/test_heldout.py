"""peak-play W2: a held-out arena is named against stage.json, refused unless the stage holds it out, and pinned on the
wire for the evaluation alone."""

import pytest

from animus import protocol as p
from animus.config import EvalConfig
from animus.train import heldout_arenas, heldout_due

STAGE = {"arenas": [{"name": "dungeon"}, {"name": "pull"}, {"name": "heldout", "eval_only": True}]}


def test_a_held_out_arena_resolves_to_its_pin_and_anything_else_is_refused():
    assert heldout_arenas({"heldout": 16}, STAGE) == {"heldout": (3, 16)}
    assert heldout_arenas({}, STAGE) == {}
    with pytest.raises(ValueError, match="does not have"):
        heldout_arenas({"wailing": 16}, STAGE)
    # The stage trains on it: evaluating there would read memorised content as generalisation.
    with pytest.raises(ValueError, match="trains on"):
        heldout_arenas({"dungeon": 16}, STAGE)
    with pytest.raises(ValueError, match="positive episode count"):
        heldout_arenas({"heldout": 0}, STAGE)
    # A stage.json from before held-out arenas has no eval_only: refused rather than trusted.
    old = {"arenas": [{"name": "dungeon"}, {"name": "heldout"}]}
    with pytest.raises(ValueError, match="trains on"):
        heldout_arenas({"heldout": 16}, old)


def test_mode_carries_the_pin_and_matches_the_sims_layout():
    """ModeMsg in Protocol.h: six uint32 fields (mode, seed base, episodes, flags, first seed, arena) and a 32-byte
    policy name, packed (protocol 18)."""
    assert p.MODE.size == 6 * 4 + 32
    payload = p.encode_mode(True, 1000, 16, first_seed=8, arena=3)
    assert p.decode_mode_arena(payload) == 3 and p.decode_mode_first_seed(payload) == 8
    assert p.decode_mode(payload) == (True, 1000, 16, "", False)
    assert p.decode_mode_arena(p.encode_mode(False)) == 0


def test_the_held_out_cadence_is_every_nth_the_last_and_every_new_best():
    every = [n for n in range(1, 13) if heldout_due(n, 4, final=False, improved=False)]
    assert every == [4, 8, 12]
    assert heldout_due(5, 4, final=True, improved=False) and heldout_due(6, 4, final=False, improved=True)
    assert all(heldout_due(n, 1, final=False, improved=False) for n in range(1, 6))
    with pytest.raises(ValueError, match="heldout_every"):
        EvalConfig(heldout_every=0)
