"""Export a checkpoint's actor as plain MLP models (.amdl) for in-game inference.

    python -m animus.export --checkpoint runs/stage8_duel/best.pt --out exported/stage8_duel

Exporting runs on request only -- `forge export` on the sim's console starts this module, or run it by hand -- and
the exported files are copied to a server by hand: training never writes models anywhere but its own run directory.

The .amdl format (little-endian); a reader must follow it exactly, and a change bumps AMDL_VERSION:

    char[4]  magic "AMDL"
    u32      version
    u16      model name length, then that many bytes (UTF-8, no terminator)
    u32      obs_dim
    u32      num_agents       the actor input is obs followed by a one-hot agent id
    u32      num_actions
    u32      layer_count
    per layer:
        u32      in_dim
        u32      out_dim
        f32      weight[out_dim * in_dim]   row-major, as nn.Linear stores it
        f32      bias[out_dim]
    u32      recurrent_size                 0 = the policy has no memory
    if recurrent_size:
        f32  weight_ih[3 * recurrent_size * trunk_out]   as torch.nn.GRUCell stores them (r, z, n)
        f32  weight_hh[3 * recurrent_size * recurrent_size]
        f32  bias_ih[3 * recurrent_size]
        f32  bias_hh[3 * recurrent_size]
    u32      goal_count                     0 = the policy has no goals
    u32      goal_every_decisions
    if goal_count:
        f32  goal_weight[goal_count * feature_width]     the goal head, on the same features
        f32  goal_bias[goal_count]
        f32  goal_embedding[goal_count * feature_width]  added to the features the action head reads

Every layer but the last is followed by tanh. With a memory, the last layer (the action head) reads the GRU's state
instead of the trunk's output: the trunk feeds the GRU, whose state is carried from decision to decision and cleared
when an episode ends. With goals, one is chosen from the goal head every goal_every_decisions decisions (the argmax)
and kept in between, and its embedding is added to the features the action head reads. The policy is the argmax of the
final logits over allowed actions.

The learner's actor is layout-aware (mappo.networks.LayoutActor): one input adapter and action head per layout
around a shared trunk. For one layout, adapter + trunk + head is exactly such an MLP, so every layout exports as
its own model, <model name>.amdl. A curriculum stage's stage.json names each layout's model (warrior_dps at
stage8_duel -> warrior_dps_duel); a scenario without one keeps its own name (one layout) or appends the layout's.
num_agents is 1, with a zero-weight agent column (the format has at least one).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
from pathlib import Path

import numpy as np
import torch

from .stages import STAGE_FILE, model_names

AMDL_MAGIC = b"AMDL"
AMDL_VERSION = 2

_TRUNK_KEY = re.compile(r"^trunk\.layers\.(\d+)\.(weight|bias)$")


def layout_layers(actor_state: dict[str, torch.Tensor], layout: int) -> list[tuple[np.ndarray, np.ndarray]]:
    """(weight [out, in], bias [out]) of one layout's adapter, the trunk and its head, in forward order."""

    def pair(prefix: str) -> tuple[np.ndarray, np.ndarray]:
        weight = actor_state[f"{prefix}.weight"].detach().cpu().numpy().astype("<f4")
        bias = actor_state[f"{prefix}.bias"].detach().cpu().numpy().astype("<f4")
        return weight, bias

    trunk = sorted({int(m.group(1)) for key in actor_state if (m := _TRUNK_KEY.match(key))})
    adapter = fold_normalisation(actor_state, layout, *pair(f"adapters.{layout}"))
    layers = [adapter, *(pair(f"trunk.layers.{i}") for i in trunk), pair(f"heads.{layout}")]

    # With a memory the GRU sits between the trunk and the action head, so the head reads the GRU's state and only
    # the layers before it have to line up.
    chain = layers[:-1] if "memory.weight_ih" in actor_state else layers
    for (prev, _), (weight, _) in zip(chain, chain[1:]):
        if weight.shape[1] != prev.shape[0]:
            raise ValueError(f"layer input {weight.shape[1]} does not match previous output {prev.shape[0]}")
    return layers


def fold_normalisation(actor_state: dict[str, torch.Tensor], layout: int, weight: np.ndarray,
                       bias: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Fold the layout's observation statistics into its adapter, so the exported model takes raw features.

    The learner feeds the adapter (x - mean) / sd (mappo.networks.RunningNorm), which is affine, so
    W((x - mean) / sd) + b is the ordinary layer (W / sd)x + (b - W(mean / sd)) -- and the file format, and the
    sim that runs it, stay exactly as they were. A checkpoint without statistics (or one that never trained)
    folds nothing.
    """
    mean_key, var_key, count_key = (f"norms.{layout}.{name}" for name in ("mean", "var", "count"))
    if mean_key not in actor_state or float(actor_state[count_key]) == 0.0:
        return weight, bias

    mean = actor_state[mean_key].detach().cpu().numpy().astype("<f8")
    # The epsilon RunningNorm.forward adds under the root; keep the two in step.
    deviation = np.sqrt(actor_state[var_key].detach().cpu().numpy().astype("<f8") + 1e-5)
    folded = weight.astype("<f8") / deviation
    return (folded.astype("<f4"), (bias.astype("<f8") - folded @ mean).astype("<f4"))


def with_agent_column(layers: list[tuple[np.ndarray, np.ndarray]]) -> list[tuple[np.ndarray, np.ndarray]]:
    """The same network with one zero-weight input appended: the single one-hot agent id of the file format."""
    weight, bias = layers[0]
    padded = np.concatenate([weight, np.zeros((weight.shape[0], 1), dtype="<f4")], axis=1)
    return [(padded, bias), *layers[1:]]


def model_name(scenario: str, layout: str, layout_count: int, models: dict[str, str] | None = None) -> str:
    """The model name stage.json gives the layout; without one, a single-layout scenario keeps its own name."""
    if models and layout in models:
        return models[layout]
    return scenario if layout_count == 1 else f"{scenario}_{layout}"


def write_amdl(
    path: str | Path,
    name: str,
    obs_dim: int,
    num_agents: int,
    num_actions: int,
    layers: list[tuple[np.ndarray, np.ndarray]],
    memory: dict[str, np.ndarray] | None = None,
    goals: dict[str, np.ndarray] | None = None,
    goal_every: int = 0,
) -> None:
    if layers[0][0].shape[1] != obs_dim + num_agents:
        raise ValueError(f"first layer takes {layers[0][0].shape[1]} inputs, expected {obs_dim} + {num_agents}")
    if memory is not None and layers[-1][0].shape[1] != memory["weight_hh"].shape[1]:
        raise ValueError("the action head does not read the memory's state")
    if layers[-1][0].shape[0] != num_actions:
        raise ValueError(f"last layer has {layers[-1][0].shape[0]} outputs, expected {num_actions}")

    encoded = name.encode("utf-8")
    with open(path, "wb") as out:
        out.write(AMDL_MAGIC)
        out.write(struct.pack("<IH", AMDL_VERSION, len(encoded)))
        out.write(encoded)
        out.write(struct.pack("<IIII", obs_dim, num_agents, num_actions, len(layers)))
        for weight, bias in layers:
            out.write(struct.pack("<II", weight.shape[1], weight.shape[0]))
            out.write(np.ascontiguousarray(weight, dtype="<f4").tobytes())
            out.write(np.ascontiguousarray(bias, dtype="<f4").tobytes())

        recurrent = 0 if memory is None else int(memory["weight_hh"].shape[1])
        out.write(struct.pack("<I", recurrent))
        if memory is not None:
            for name in ("weight_ih", "weight_hh", "bias_ih", "bias_hh"):
                out.write(np.ascontiguousarray(memory[name], dtype="<f4").tobytes())

        count = 0 if goals is None else int(goals["weight"].shape[0])
        out.write(struct.pack("<II", count, goal_every if count else 0))
        if goals is not None:
            for name in ("weight", "bias", "embedding"):
                out.write(np.ascontiguousarray(goals[name], dtype="<f4").tobytes())


def memory_weights(actor_state: dict[str, torch.Tensor]) -> dict[str, np.ndarray] | None:
    """The GRU's weights, or None when the actor has no memory."""
    if "memory.weight_ih" not in actor_state:
        return None
    return {name: actor_state[f"memory.{name}"].detach().cpu().numpy().astype("<f4")
            for name in ("weight_ih", "weight_hh", "bias_ih", "bias_hh")}


def goal_weights(actor_state: dict[str, torch.Tensor]) -> dict[str, np.ndarray] | None:
    """The goal head and its embedding, or None when the actor has no goals."""
    if "goal_head.weight" not in actor_state:
        return None
    return {
        "weight": actor_state["goal_head.weight"].detach().cpu().numpy().astype("<f4"),
        "bias": actor_state["goal_head.bias"].detach().cpu().numpy().astype("<f4"),
        "embedding": actor_state["goal_embedding.weight"].detach().cpu().numpy().astype("<f4"),
    }


def export_layouts(
    actor_state: dict[str, torch.Tensor], spec: dict, out_dir: str | Path, manifest_dir: str | Path | None = None,
    goal_every: int = 0
) -> list[Path]:
    """Write every layout's model to out_dir/<model name>.amdl, each atomically; returns the files written.

    manifest_dir is the stage's layouts directory (the sim writes <OutputDir>/layouts/<scenario>/): its stage.json
    names the models, and each <model name>.json layout manifest there is copied beside its model, so whoever loads the
    model can check it reads the same layout.
    """
    out_dir = Path(out_dir)
    manifests = Path(manifest_dir) if manifest_dir is not None else Path("layouts") / spec["scenario"]
    stage_path = manifests / STAGE_FILE
    models = model_names(json.loads(stage_path.read_text())) if stage_path.is_file() else {}
    layouts = spec["layouts"]
    written = []
    for index, layout in enumerate(layouts):
        name = model_name(spec["scenario"], layout["name"], len(layouts), models)
        layers = with_agent_column(layout_layers(actor_state, index))
        memory = memory_weights(actor_state)
        goals = goal_weights(actor_state)
        target = out_dir / f"{name}.amdl"
        partial = out_dir / f".{target.name}.partial"
        try:
            write_amdl(partial, name, layout["obs_dim"], 1, layout["num_actions"], layers, memory, goals,
                       goal_every)
            os.replace(partial, target)
        except OSError:
            partial.unlink(missing_ok=True)
            raise
        written.append(target)

        manifest = manifests / f"{name}.json"
        if manifest.is_file():
            partial_manifest = out_dir / f".{name}.json.partial"
            partial_manifest.write_bytes(manifest.read_bytes())
            os.replace(partial_manifest, out_dir / f"{name}.json")
    return written


def read_amdl(path: str | Path) -> dict:
    """Parse an .amdl file. Used by the tests as the reference for the C++ loader."""
    data = Path(path).read_bytes()
    if data[:4] != AMDL_MAGIC:
        raise ValueError("not an .amdl file")
    offset = 4
    version, name_len = struct.unpack_from("<IH", data, offset)
    offset += 6
    if version != AMDL_VERSION:
        raise ValueError(f"unsupported .amdl version {version}")
    name = data[offset : offset + name_len].decode("utf-8")
    offset += name_len
    obs_dim, num_agents, num_actions, layer_count = struct.unpack_from("<IIII", data, offset)
    offset += 16

    layers = []
    for _ in range(layer_count):
        in_dim, out_dim = struct.unpack_from("<II", data, offset)
        offset += 8
        weight = np.frombuffer(data, dtype="<f4", count=out_dim * in_dim, offset=offset).reshape(out_dim, in_dim)
        offset += out_dim * in_dim * 4
        bias = np.frombuffer(data, dtype="<f4", count=out_dim, offset=offset)
        offset += out_dim * 4
        layers.append((weight, bias))

    def floats(count: int, *shape: int) -> np.ndarray:
        nonlocal offset
        values = np.frombuffer(data, dtype="<f4", count=count, offset=offset).reshape(*shape)
        offset += count * 4
        return values

    (recurrent,) = struct.unpack_from("<I", data, offset)
    offset += 4
    memory = None
    if recurrent:
        features = layers[-2][0].shape[0]
        memory = {
            "weight_ih": floats(3 * recurrent * features, 3 * recurrent, features),
            "weight_hh": floats(3 * recurrent * recurrent, 3 * recurrent, recurrent),
            "bias_ih": floats(3 * recurrent, 3 * recurrent),
            "bias_hh": floats(3 * recurrent, 3 * recurrent),
        }

    goal_count, goal_every = struct.unpack_from("<II", data, offset)
    offset += 8
    goals = None
    if goal_count:
        width = layers[-1][0].shape[1]
        goals = {
            "weight": floats(goal_count * width, goal_count, width),
            "bias": floats(goal_count, goal_count),
            "embedding": floats(goal_count * width, goal_count, width),
        }

    if offset != len(data):
        raise ValueError(f"{len(data) - offset} trailing bytes")
    return {
        "name": name,
        "obs_dim": obs_dim,
        "num_agents": num_agents,
        "num_actions": num_actions,
        "layers": layers,
        "memory": memory,
        "goals": goals,
        "goal_every": goal_every,
    }


def reference_decide(model: dict, obs: np.ndarray, mask: np.ndarray, agent: int = 0,
                     state: dict | None = None) -> tuple[int, np.ndarray]:
    """The forward pass of an exported model: returns (greedy allowed action, logits).

    `state` is what the policy carries between decisions -- {"memory": [R], "goal": int, "age": int} -- updated in
    place. A model without a memory or goals ignores it, and so behaves the same however it is called.
    """
    x = np.concatenate([obs.astype(np.float32), np.eye(model["num_agents"], dtype=np.float32)[agent]])
    layers = model["layers"]
    for index, (weight, bias) in enumerate(layers[:-1]):
        x = np.tanh(weight @ x + bias)

    memory = model.get("memory")
    if memory is not None:
        size = memory["weight_hh"].shape[1]
        carried = np.zeros(size, dtype=np.float32) if state is None else state.setdefault(
            "memory", np.zeros(size, dtype=np.float32))
        gates = memory["weight_ih"] @ x + memory["bias_ih"]
        recurrent = memory["weight_hh"] @ carried + memory["bias_hh"]
        reset = _sigmoid(gates[:size] + recurrent[:size])
        update = _sigmoid(gates[size:2 * size] + recurrent[size:2 * size])
        candidate = np.tanh(gates[2 * size:] + reset * recurrent[2 * size:])
        x = (1.0 - update) * candidate + update * carried
        if state is not None:
            state["memory"] = x

    goals = model.get("goals")
    if goals is not None:
        every = max(1, model.get("goal_every", 1))
        age = 0 if state is None else state.get("age", 0)
        goal = 0 if state is None else state.get("goal", 0)
        if age % every == 0:
            goal = int((goals["weight"] @ x + goals["bias"]).argmax())
        if state is not None:
            state["goal"], state["age"] = goal, 1 if age % every == 0 else age + 1
        x = x + goals["embedding"][goal]

    weight, bias = layers[-1]
    x = weight @ x + bias

    allowed = mask.astype(bool)
    if not allowed.any():
        return 0, x
    return int(np.where(allowed, x, -np.inf).argmax()), x


def _sigmoid(x: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-x))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--out", required=True, help="directory for the .amdl files, one per layout")
    parser.add_argument(
        "--layouts-dir", default="layouts", help="the sim's layouts directory (AnimusForge.OutputDir/layouts)"
    )
    args = parser.parse_args()

    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifests = Path(args.layouts_dir) / checkpoint["spec"]["scenario"]
    goal_every = int(checkpoint.get("config", {}).get("mappo", {}).get("goal_every_decisions", 0))
    for path in export_layouts(checkpoint["trainer"]["actor"], checkpoint["spec"], out, manifests, goal_every):
        print(f"Wrote {path} (update {checkpoint.get('update', '?')})")


if __name__ == "__main__":
    main()
