"""golden.cpp's inputs, into OUT: the golden cases as text, a version 7 model (as test_a_version_7_file_still_reads
makes it) and the golden model relabelled version 9. Run with the learner's venv (it exports through animus)."""
import json
import struct
import sys
from pathlib import Path

import torch

PYTHON = Path(__file__).resolve().parents[2] / "python"
sys.path.insert(0, str(PYTHON))
from animus.export import export_layouts  # noqa: E402
from animus.mappo.networks import LayoutActor  # noqa: E402

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
golden = PYTHON / "tests" / "golden"
saved = json.loads((golden / "seat_sets.json").read_text())


def lines(cases):
    return "".join(f"{sequence} " + " ".join(map(repr, case["obs"] + case["logits"])) + "\n"
                   for sequence, case in cases)


(out / "cases.txt").write_text(lines((-1, case) for case in saved["cases"]))
(out / "recurrent.txt").write_text(lines((index, step) for index, steps in enumerate(saved["recurrent_cases"])
                                         for step in steps))
(out / "tolerance.txt").write_text(f"{saved['tolerance']}\n")
torch.manual_seed(0)
[path] = export_layouts(LayoutActor([(9, 3)], [16, 8]).state_dict(), {"scenario": "solo", "layouts": [
    {"name": "warrior_dps", "obs_dim": 9, "num_actions": 3}]}, out, out)
data = bytearray(path.read_bytes())
struct.pack_into("<I", data, 4, 7)
(out / "v7.amdl").write_bytes(bytes(data[:-1]))            # a version 7 file: no seat sets' flag
data = bytearray((golden / "seat_sets.amdl").read_bytes())
struct.pack_into("<I", data, 4, 9)
(out / "v9.amdl").write_bytes(bytes(data))
print(f"prepared {len(saved['cases'])} cases and {len(saved['recurrent_cases'])} sequences in {out}")
