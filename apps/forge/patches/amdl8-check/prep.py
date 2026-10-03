"""golden.cpp's inputs, into OUT: each golden set's cases as text (seat_sets, version 8, frozen; seat_attention,
version 9), a version 7 model (as test_a_version_7_file_still_reads makes it) and a golden model relabelled with a
version past the reader's. Run with the learner's venv (it exports through animus)."""
import json
import struct
import sys
from pathlib import Path

import torch

PYTHON = Path(__file__).resolve().parents[2] / "python"
sys.path.insert(0, str(PYTHON))
from animus.export import export_layouts  # noqa: E402
from animus.mappo.networks import LayoutActor  # noqa: E402

GOLDEN_SETS = ("seat_sets", "seat_attention")
FUTURE_VERSION = 10

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
golden = PYTHON / "tests" / "golden"


def lines(cases):
    return "".join(f"{sequence} " + " ".join(map(repr, case["obs"] + case["logits"])) + "\n"
                   for sequence, case in cases)


for name in GOLDEN_SETS:
    saved = json.loads((golden / f"{name}.json").read_text())
    (out / f"{name}.cases.txt").write_text(lines((-1, case) for case in saved["cases"]))
    (out / f"{name}.recurrent.txt").write_text(lines((index, step) for index, steps
                                                     in enumerate(saved["recurrent_cases"]) for step in steps))
    (out / f"{name}.tolerance.txt").write_text(f"{saved['tolerance']}\n")
torch.manual_seed(0)
[path] = export_layouts(LayoutActor([(9, 3)], [16, 8]).state_dict(), {"scenario": "solo", "layouts": [
    {"name": "warrior_dps", "obs_dim": 9, "num_actions": 3}]}, out, out)
data = bytearray(path.read_bytes())
struct.pack_into("<I", data, 4, 7)
(out / "v7.amdl").write_bytes(bytes(data[:-1]))            # a version 7 file: no seat sets' flag
data = bytearray((golden / "seat_attention.amdl").read_bytes())
struct.pack_into("<I", data, 4, FUTURE_VERSION)
(out / "future.amdl").write_bytes(bytes(data))
print(f"prepared {', '.join(GOLDEN_SETS)} in {out}")
