"""What a decision costs the in-game reader at a model's real size: one layout of the shipped widths (hidden
[256, 512, 512], memory 128) whose sets are the party stages' (4 enemies, 7 members, 9 friends, 4 creatures of the
crowd), with the sets alone (.amdl 8's forward) and with the attention layer (9). Writes the two models into OUT and
prints the lines golden's bench mode should be run with. Run with the learner's venv."""
import json
import sys
from pathlib import Path

import torch

PYTHON = Path(__file__).resolve().parents[2] / "python"
sys.path.insert(0, str(PYTHON))
from animus.export import export_layouts  # noqa: E402
from animus.mappo.networks import LayoutActor  # noqa: E402

OBS, ACTIONS = 600, 120
SETS = [{"name": name, "slots": slots, "present": 0, "segments": [{"first": first, "stride": stride}],
         "pointers": [{"first": pointer, "count": slots}] if pointer is not None else []}
        for name, slots, first, stride, pointer in (("enemies", 4, 100, 43, 60), ("members", 7, 280, 20, 70),
                                                   ("friends", 9, 420, 16, None), ("crowd", 4, 570, 7, None))]

out = Path(sys.argv[1])
for attention in (False, True):
    torch.manual_seed(0)
    actor = LayoutActor([(OBS, ACTIONS)], [256, 512, 512], recurrent_size=128, seat_sets=[SETS],
                        entity_attention=attention)
    name = "bench_attention" if attention else "bench_sets"
    stage_dir = out / name / "stage"
    stage_dir.mkdir(parents=True, exist_ok=True)
    (stage_dir / "stage.json").write_text(json.dumps({"stage": name, "models": {"warrior_dps": name},
                                                      "layouts": {"warrior_dps": {"sets": SETS}}}))
    [path] = export_layouts(actor.state_dict(), {"scenario": name, "layouts": [
        {"name": "warrior_dps", "obs_dim": OBS, "num_actions": ACTIONS}]}, out, stage_dir)
    print(f"bench {path} {name} {OBS} {ACTIONS}")
