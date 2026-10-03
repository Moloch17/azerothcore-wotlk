"""peak-play W4/W7: a seat layout's sets -- and their attention layer (version 9) -- travel in its .amdl, the reference
forward pass matches the learner's, version 7 and 8 files still read, and golden vectors are kept for the in-game
reader (mod-animus) to be checked against: seat_sets* (version 8, frozen) and seat_attention* (version 9)."""

import json
import struct
from pathlib import Path

import numpy as np
import pytest
import torch

from animus.export import AMDL_VERSION, export_layouts, read_amdl, reference_decide
from animus.mappo.networks import LayoutActor

GOLDEN = Path(__file__).with_name("golden")
SETS_A = [{"name": "enemies", "slots": 2, "present": 0,
           "segments": [{"first": 2, "stride": 3}, {"first": 10, "stride": 2}], "pointers": [{"first": 1, "count": 2}]},
          {"name": "friends", "slots": 2, "present": 1, "segments": [{"first": 14, "stride": 2}], "pointers": []}]
SETS_B = [{"name": "enemies", "slots": 2, "present": 0,
           "segments": [{"first": 4, "stride": 3}, {"first": 12, "stride": 2}], "pointers": [{"first": 3, "count": 2}]}]
LAYOUTS = [(20, 4), (16, 5)]
NAMES = ("warrior_dps", "priest_heal")


def actor_and_stage(tmp_path: Path, recurrent_size: int = 0, attention: bool = False):
    torch.manual_seed(3 + recurrent_size)
    actor = LayoutActor(LAYOUTS, [16, 8], recurrent_size=recurrent_size, seat_sets=[SETS_A, SETS_B],
                        entity_attention=attention)
    with torch.no_grad():
        for head in actor.heads:
            head.weight.mul_(50.0)
        for query in actor.entity_sets.queries.values():
            query.weight.mul_(50.0)
        if attention:
            # Off the identity start, as training takes it: every part of the layer counts in the logits.
            sets = actor.entity_sets
            for parameter in (sets.attend.out_proj.weight, sets.attend.out_proj.bias, sets.mix_out.weight,
                              sets.mix_out.bias, sets.type_embed, sets.norm_attend.weight, sets.norm_mix.bias):
                parameter.add_(torch.randn_like(parameter) * 0.5)
            sets.pool.weight[:, -sets.embed:] = torch.randn_like(sets.pool.weight[:, -sets.embed:]) * 0.5
    stage_dir = tmp_path / f"layouts{recurrent_size}{'a' if attention else ''}" / "stage5_pack"
    stage_dir.mkdir(parents=True)
    stage = {"stage": "stage5_pack", "models": {name: f"{name}_pack" for name in NAMES},
             "layouts": {"warrior_dps": {"sets": SETS_A}, "priest_heal": {"sets": SETS_B}}}
    (stage_dir / "stage.json").write_text(json.dumps(stage))
    spec = {"scenario": "stage5_pack",
            "layouts": [{"name": n, "obs_dim": o, "num_actions": a} for n, (o, a) in zip(NAMES, LAYOUTS)]}
    return actor, spec, stage_dir


def torch_logits(actor: LayoutActor, index: int, obs: np.ndarray, mask: np.ndarray, memory=None):
    """The learner's logits for one decision; with `memory` ([1, R]), (logits, memory carried out)."""
    padded = np.zeros(max(o for o, _ in LAYOUTS), np.float32)
    padded[: obs.shape[0]] = obs
    padded_mask = np.zeros(max(a for _, a in LAYOUTS), bool)
    padded_mask[: mask.shape[0]] = mask
    with torch.no_grad():
        dist, carried, _ = actor.step(torch.from_numpy(padded)[None], torch.tensor([index]),
                                      torch.from_numpy(padded_mask)[None], memory)
    logits = dist.logits[0, : mask.shape[0]].numpy()
    return logits if memory is None else (logits, carried)


def observation(rng, obs_dim: int) -> np.ndarray:
    obs = rng.standard_normal(obs_dim).astype(np.float32)
    obs[[2, 5, 4, 7]] = rng.integers(0, 2, 4)              # present flags, whichever layout reads them
    if obs_dim >= 18:                                       # the friends' (layout 0 only)
        obs[[15, 17]] = rng.integers(0, 2, 2)
    return obs


@pytest.mark.parametrize("attention", [False, True], ids=["sets", "attention"])
def test_a_seat_layouts_sets_round_trip_and_match_the_learner(tmp_path, attention):
    actor, spec, stage_dir = actor_and_stage(tmp_path, attention=attention)
    out = tmp_path / "models"
    out.mkdir()
    written = export_layouts(actor.state_dict(), spec, out, stage_dir)
    rng = np.random.default_rng(5)
    for index, ((obs_dim, actions), path) in enumerate(zip(LAYOUTS, written)):
        model = read_amdl(path)
        assert model["seat_sets"] is not None and len(model["seat_sets"]["sets"]) == (2 if index == 0 else 1)
        assert (model["seat_sets"]["attention"] is not None) == attention
        for _ in range(16):
            obs = observation(rng, obs_dim)
            mask = np.ones(actions, bool)
            _, logits = reference_decide(model, obs, mask)
            np.testing.assert_allclose(logits - logits[0], torch_logits(actor, index, obs, mask)
                                       - torch_logits(actor, index, obs, mask)[0], rtol=1e-4, atol=1e-4)


def test_a_version_7_file_still_reads(tmp_path):
    """The deployed models are version 7: the same file without the seat sets' flag."""
    torch.manual_seed(0)
    actor = LayoutActor([(9, 3)], [16, 8])
    out = tmp_path / "models"
    out.mkdir()
    [path] = export_layouts(actor.state_dict(), {"scenario": "solo", "layouts": [
        {"name": "warrior_dps", "obs_dim": 9, "num_actions": 3}]}, out, tmp_path)
    data = bytearray(path.read_bytes())
    assert struct.unpack_from("<I", data, 4)[0] == AMDL_VERSION == 9 and data[-1] == 0
    struct.pack_into("<I", data, 4, 7)
    old = tmp_path / "old.amdl"
    old.write_bytes(bytes(data[:-1]))
    model = read_amdl(old)
    assert model["seat_sets"] is None
    obs = np.random.default_rng(0).standard_normal(9).astype(np.float32)
    np.testing.assert_allclose(reference_decide(model, obs, np.ones(3, bool))[1],
                               reference_decide(read_amdl(path), obs, np.ones(3, bool))[1])


def golden_vectors(tmp_path: Path, attention: bool = True) -> dict:
    """warrior_dps_pack.amdl and, for 8 seeded observations, the logits the learner gives: what mod-animus's reader
    (MlpPolicy) has to reproduce to 1e-4. Then the same with a memory (recurrent_size 8, as the shipped models carry
    one): 2 sequences of 3 decisions from a cleared memory, the logits at every step. With the attention layer
    (version 9); the sets alone (version 8) are frozen in seat_sets*."""
    actor, spec, stage_dir = actor_and_stage(tmp_path, attention=attention)
    out = tmp_path / "golden"
    out.mkdir()
    [path, _] = export_layouts(actor.state_dict(), spec, out, stage_dir)
    rng = np.random.default_rng(11)
    mask = np.ones(LAYOUTS[0][1], bool)
    cases = []
    for _ in range(8):
        obs = observation(rng, LAYOUTS[0][0])
        logits = torch_logits(actor, 0, obs, mask)
        cases.append({"obs": obs.tolist(), "logits": (logits - logits[0]).tolist()})

    recurrent, spec, stage_dir = actor_and_stage(tmp_path, recurrent_size=8, attention=attention)
    out = tmp_path / "golden_recurrent"
    out.mkdir()
    [recurrent_path, _] = export_layouts(recurrent.state_dict(), spec, out, stage_dir)
    sequences = []
    for _ in range(2):
        memory = recurrent.initial_memory(1)
        steps = []
        for _ in range(3):
            obs = observation(rng, LAYOUTS[0][0])
            logits, memory = torch_logits(recurrent, 0, obs, mask, memory)
            steps.append({"obs": obs.tolist(), "logits": (logits - logits[0]).tolist()})
        sequences.append(steps)
    return {"model": path.read_bytes(), "cases": cases, "recurrent_model": recurrent_path.read_bytes(),
            "recurrent_cases": sequences}


def test_the_frozen_version_8_goldens_read_the_same_through_the_version_9_reader():
    """seat_sets* were written as version 8 (W4): the reader of 9 gives their logits as they were recorded."""
    saved = json.loads((GOLDEN / "seat_sets.json").read_text())
    for name, cases in (("seat_sets.amdl", [[case] for case in saved["cases"]]),
                        ("seat_sets_recurrent.amdl", saved["recurrent_cases"])):
        model = read_amdl(GOLDEN / name)
        assert struct.unpack_from("<I", (GOLDEN / name).read_bytes(), 4)[0] == 8
        assert model["seat_sets"]["attention"] is None
        for steps in cases:
            state = {}
            for step in steps:
                _, logits = reference_decide(model, np.asarray(step["obs"], np.float32), np.ones(4, bool),
                                             state=state)
                np.testing.assert_allclose(logits - logits[0], step["logits"], rtol=1e-4, atol=1e-4)


def test_the_golden_vectors_are_current(tmp_path):
    """tests/golden/ holds the vectors the in-game reader is checked against; regenerated here, they must match (a
    format or forward change regenerates them: delete the three seat_attention files and run this test)."""
    vectors = golden_vectors(tmp_path)
    model_file, cases_file = GOLDEN / "seat_attention.amdl", GOLDEN / "seat_attention.json"
    recurrent_file = GOLDEN / "seat_attention_recurrent.amdl"
    if not cases_file.exists():
        GOLDEN.mkdir(exist_ok=True)
        model_file.write_bytes(vectors["model"])
        recurrent_file.write_bytes(vectors["recurrent_model"])
        cases_file.write_text(json.dumps({
            "tolerance": 1e-4, "note": "logits relative to action 0, all allowed; recurrent_cases: each a sequence "
            "from a cleared memory, through seat_attention_recurrent.amdl", "cases": vectors["cases"],
            "recurrent_cases": vectors["recurrent_cases"]}, indent=1) + "\n")
    assert model_file.read_bytes() == vectors["model"]
    assert recurrent_file.read_bytes() == vectors["recurrent_model"]
    saved = json.loads(cases_file.read_text())
    assert len(saved["cases"]) == 8 and [len(steps) for steps in saved["recurrent_cases"]] == [3, 3]
    fresh = vectors["cases"] + [step for steps in vectors["recurrent_cases"] for step in steps]
    for case, new in zip(saved["cases"] + [step for steps in saved["recurrent_cases"] for step in steps], fresh):
        np.testing.assert_allclose(case["logits"], new["logits"], rtol=1e-5, atol=1e-5)

    # The reference reader (what MlpPolicy mirrors) agrees with the learner through the memory as well.
    model = read_amdl(recurrent_file)
    for steps in saved["recurrent_cases"]:
        state = {"memory": np.zeros(8, np.float32)}
        for step in steps:
            _, logits = reference_decide(model, np.asarray(step["obs"], np.float32), np.ones(4, bool), state=state)
            np.testing.assert_allclose(logits - logits[0], step["logits"], rtol=1e-4, atol=1e-4)
