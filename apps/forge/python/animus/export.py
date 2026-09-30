"""Export a checkpoint's actor as plain MLP models (.amdl) for in-game inference.

    python -m animus.export --checkpoint runs/stage4_duel/best.pt --out exported/stage4_duel

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
    u32      foresight_outputs              0 = no predictions fed back (Component P, layer 2)
    if foresight_outputs:
        f32  foresight_weight[foresight_outputs * feature_width], foresight_bias[foresight_outputs]
        f32  feedback_weight[feature_width * foresight_outputs], feedback_bias[feature_width]
    u32      slow_size                      0 = the goal head reads the features (no two-clock seat)
    if slow_size:
        f32  slow_weight_ih[3 * slow_size * feature_width], slow_weight_hh[3 * slow_size * slow_size]
        f32  slow_bias_ih[3 * slow_size], slow_bias_hh[3 * slow_size]
    u32      goal_kinds                     0 = the policy has no goals
    u32      goal_targets                   1 = goals name no target (the kind alone)
    u32      goal_every_decisions
    if goal_kinds:
        f32  kind_weight[goal_kinds * feature_width]     the goal head (GoalHead), on the same features
        f32  kind_bias[goal_kinds]
        if goal_targets > 1:
            f32  target_weight[goal_targets * feature_width]
            f32  target_bias[goal_targets]
        f32  pair[goal_kinds * goal_targets]
        u8   accepts[goal_kinds * goal_targets]           which targets each kind can take
        i32  goal_block_at                  the goal block's first observation column (-1: none)
        f32  kind_embedding[goal_kinds * feature_width]  added to the features the action head reads
        if goal_targets > 1:
            f32  target_embedding[goal_targets * feature_width]
        u8   lookahead                      goal-level lookahead (Component P, layer 3)
        if lookahead:
            success and duration, each: kind_weight[K * goal_width], kind_bias[K], (target_weight[T * goal_width],
            target_bias[T] if T > 1), pair[K * T]; then f32 lookahead_weight[2]
        u32  goal_slots                     (version 6) 1 = one goal; else a primary, a secondary and a queue
        if goal_slots > 1:
            f32  slot_bias[(goal_slots - 1) * goal_width]       added to the features of each slot after the primary
            f32  drawn[(K * T + 1) * goal_width]                 what was drawn before, by goal + 1 (0: none)
            f32  none_bias[goal_slots - 1]                       each later slot's "none" logit
            f32  gate                                            the secondary's share of the action head's embedding
    (goal_width: slow_size when there is one, else feature_width)
    u8       director_sets                  the director's members and enemies as sets (DirectorSets), else 0
    if director_sets:
        u32  embed
        per set (members, then enemies): u32 first, slots, width, present;
            f32 w1[embed * width], b1[embed], w2[embed * embed], b2[embed]     encoder: tanh after each
        f32  pool_weight[adapter_out * 4 * embed], pool_bias[adapter_out]  added to the first layer before its tanh
        u32  pointer_count; per pointer: u32 first_action, u32 set (0 members, 1 enemies),
            f32 query_weight[embed * feature_width], query_bias[embed]   its actions' logits are slot . query

Every layer but the last is followed by tanh. With a memory, the last layer (the action head) reads the GRU's state
instead of the trunk's output: the trunk feeds the GRU, whose state is carried from decision to decision and cleared
when an episode ends. With goals, one is chosen from the goal head every goal_every_decisions decisions (the argmax)
or at once when the goal block's "ended" column is set, and kept in between: a goal is kind * goal_targets + target,
its logit the kind's plus the target's plus the pair's, masked by the goal block's columns (kinds there, targets there)
and the accepts table. The kind's and target's embeddings are added to the features the action head reads. The policy is the argmax of the
final logits over allowed actions.

The learner's actor is layout-aware (mappo.networks.LayoutActor): one input adapter and action head per layout
around a shared trunk. For one layout, adapter + trunk + head is exactly such an MLP, so every layout exports as
its own model, <model name>.amdl. A curriculum stage's stage.json names each layout's model (warrior_dps at
stage4_duel -> warrior_dps_duel); a scenario without one keeps its own name (one layout) or appends the layout's.
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
# 6: two goals held and a queue (goal_slots, the slot parameters, the embedding's gate), a twelfth goal kind, and
# the goal block's next-run columns (the secondary ending, the event, the director's primary, what was achieved).
AMDL_VERSION = 6

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
    feedback: dict[str, np.ndarray] | None = None,
    slow: dict[str, np.ndarray] | None = None,
    sets: dict | None = None,
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

        outputs = 0 if feedback is None else int(feedback["foresight_weight"].shape[0])
        out.write(struct.pack("<I", outputs))
        if feedback is not None:
            for name in ("foresight_weight", "foresight_bias", "feedback_weight", "feedback_bias"):
                out.write(np.ascontiguousarray(feedback[name], dtype="<f4").tobytes())
        slow_size = 0 if slow is None else int(slow["weight_hh"].shape[1])
        out.write(struct.pack("<I", slow_size))
        if slow is not None:
            for name in ("weight_ih", "weight_hh", "bias_ih", "bias_hh"):
                out.write(np.ascontiguousarray(slow[name], dtype="<f4").tobytes())

        kinds = 0 if goals is None else int(goals["kind_weight"].shape[0])
        targets = 1 if goals is None else int(goals["pair"].shape[1])
        out.write(struct.pack("<III", kinds, targets, goal_every if kinds else 0))
        if goals is not None:
            names = ["kind_weight", "kind_bias"] + (["target_weight", "target_bias"] if targets > 1 else []) + ["pair"]
            for name in names:
                out.write(np.ascontiguousarray(goals[name], dtype="<f4").tobytes())
            out.write(np.ascontiguousarray(goals["accepts"], dtype="u1").tobytes())
            out.write(struct.pack("<i", int(goals["block_at"])))
            for name in ["kind_embedding"] + (["target_embedding"] if targets > 1 else []):
                out.write(np.ascontiguousarray(goals[name], dtype="<f4").tobytes())
            lookahead = goals.get("lookahead")
            out.write(struct.pack("<B", 1 if lookahead else 0))
            if lookahead:
                for part in ("success", "duration"):
                    names = ["kind_weight", "kind_bias"] + (["target_weight", "target_bias"] if targets > 1 else [])
                    for name in names + ["pair"]:
                        out.write(np.ascontiguousarray(lookahead[part][name], dtype="<f4").tobytes())
                out.write(np.ascontiguousarray(lookahead["weight"], dtype="<f4").tobytes())
            slots = int(goals.get("slots", 1))
            out.write(struct.pack("<I", slots))
            if slots > 1:
                for name in ("slot_bias", "drawn", "none_bias"):
                    out.write(np.ascontiguousarray(goals[name], dtype="<f4").tobytes())
                out.write(struct.pack("<f", float(goals["gate"])))

        out.write(struct.pack("<B", 1 if sets else 0))
        if sets:
            out.write(struct.pack("<I", int(sets["embed"])))
            for part in ("seats", "enemies"):
                spec = sets[part]
                out.write(struct.pack("<IIII", spec["first"], spec["slots"], spec["width"], spec["present"]))
                for name in ("w1", "b1", "w2", "b2"):
                    out.write(np.ascontiguousarray(spec[name], dtype="<f4").tobytes())
            out.write(np.ascontiguousarray(sets["pool_weight"], dtype="<f4").tobytes())
            out.write(np.ascontiguousarray(sets["pool_bias"], dtype="<f4").tobytes())
            out.write(struct.pack("<I", len(sets["pointers"])))
            for pointer in sets["pointers"]:
                out.write(struct.pack("<II", pointer["first"], 0 if pointer["over"] == "seats" else 1))
                out.write(np.ascontiguousarray(pointer["weight"], dtype="<f4").tobytes())
                out.write(np.ascontiguousarray(pointer["bias"], dtype="<f4").tobytes())


def memory_weights(actor_state: dict[str, torch.Tensor]) -> dict[str, np.ndarray] | None:
    """The GRU's weights, or None when the actor has no memory."""
    if "memory.weight_ih" not in actor_state:
        return None
    return {name: actor_state[f"memory.{name}"].detach().cpu().numpy().astype("<f4")
            for name in ("weight_ih", "weight_hh", "bias_ih", "bias_hh")}


def goal_weights(actor_state: dict[str, torch.Tensor], layout: int = 0) -> dict[str, np.ndarray] | None:
    """The goal head (GoalHead) and its embedding for one layout, or None when the actor has no goals."""
    if "goal_head.kind.weight" not in actor_state:
        return None

    def array(key: str, dtype: str = "<f4") -> np.ndarray:
        return actor_state[key].detach().cpu().numpy().astype(dtype)

    goals = {
        "kind_weight": array("goal_head.kind.weight"),
        "kind_bias": array("goal_head.kind.bias"),
        "pair": array("goal_head.pair"),
        "accepts": array("goal_head.accepts", "u1"),
        "block_at": int(actor_state["goal_head.block_at"][layout]) if "goal_head.block_at" in actor_state else -1,
        "kind_embedding": array("goal_embedding.kind.weight"),
    }
    if "goal_head.target.weight" in actor_state:
        goals["target_weight"] = array("goal_head.target.weight")
        goals["target_bias"] = array("goal_head.target.bias")
        goals["target_embedding"] = array("goal_embedding.target.weight")
    if "goal_head.lookahead_weight" in actor_state:
        def part(prefix):
            values = {"kind_weight": array(f"{prefix}.kind.weight"), "kind_bias": array(f"{prefix}.kind.bias"),
                      "pair": array(f"{prefix}.pair")}
            if f"{prefix}.target.weight" in actor_state:
                values["target_weight"] = array(f"{prefix}.target.weight")
                values["target_bias"] = array(f"{prefix}.target.bias")
            return values
        goals["lookahead"] = {"success": part("goal_head.success"), "duration": part("goal_head.duration"),
                              "weight": array("goal_head.lookahead_weight")}
    if "goal_head.slot_bias" in actor_state:
        goals["slots"] = int(actor_state["goal_head.slot_bias"].shape[0]) + 1
        goals["slot_bias"] = array("goal_head.slot_bias")
        goals["drawn"] = array("goal_head.drawn.weight")
        goals["none_bias"] = array("goal_head.none_bias")
        goals["gate"] = float(actor_state["goal_embedding.gate"])
    return goals


def director_sets(actor_state: dict[str, torch.Tensor], descriptor: dict | None) -> dict | None:
    """The director's set encoder, pooling and pointer heads (DirectorSets), or None without them."""
    if descriptor is None or "director_sets.pool.weight" not in actor_state:
        return None

    def array(key: str) -> np.ndarray:
        return actor_state[f"director_sets.{key}"].detach().cpu().numpy().astype("<f4")

    sets = {"embed": int(actor_state["director_sets.seats_encoder.0.weight"].shape[0]),
            "pool_weight": array("pool.weight"), "pool_bias": array("pool.bias"), "pointers": []}
    for part in ("seats", "enemies"):
        spec = descriptor[part]
        sets[part] = {"first": int(spec["first"]), "slots": int(spec["slots"]), "width": int(spec["width"]),
                      "present": int(spec.get("present", 0)),
                      "w1": array(f"{part}_encoder.0.weight"), "b1": array(f"{part}_encoder.0.bias"),
                      "w2": array(f"{part}_encoder.2.weight"), "b2": array(f"{part}_encoder.2.bias")}
    for index, pointer in enumerate(descriptor.get("pointers", ())):
        sets["pointers"].append({"first": int(pointer["first"]), "over": pointer["over"],
                                 "weight": array(f"queries.{index}.weight"), "bias": array(f"queries.{index}.bias")})
    return sets


def feedback_weights(actor_state: dict[str, torch.Tensor]) -> dict[str, np.ndarray] | None:
    """The foresight head and the projection that feeds its predictions back, or None when they are not fed back."""
    if "foresight_proj.weight" not in actor_state:
        return None
    return {name: actor_state[key].detach().cpu().numpy().astype("<f4") for name, key in (
        ("foresight_weight", "foresight.weight"), ("foresight_bias", "foresight.bias"),
        ("feedback_weight", "foresight_proj.weight"), ("feedback_bias", "foresight_proj.bias"))}


def slow_weights(actor_state: dict[str, torch.Tensor]) -> dict[str, np.ndarray] | None:
    """The two-clock seat's slow GRU, or None without one."""
    if "slow_memory.weight_ih" not in actor_state:
        return None
    return {name: actor_state[f"slow_memory.{name}"].detach().cpu().numpy().astype("<f4")
            for name in ("weight_ih", "weight_hh", "bias_ih", "bias_hh")}


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
    stage = json.loads(stage_path.read_text()) if stage_path.is_file() else {}
    models = model_names(stage) if stage else {}
    layouts = spec["layouts"]
    written = []
    for index, layout in enumerate(layouts):
        name = model_name(spec["scenario"], layout["name"], len(layouts), models)
        layers = with_agent_column(layout_layers(actor_state, index))
        memory = memory_weights(actor_state)
        goals = goal_weights(actor_state, index)
        target = out_dir / f"{name}.amdl"
        partial = out_dir / f".{target.name}.partial"
        try:
            # The director's sets travel with its model only; a director without them would read nothing of its
            # members (its adapter is blind to their columns), so refuse it rather than write it.
            sets = director_sets(actor_state, stage.get("director")) if layout["name"] == "director" else None
            if layout["name"] == "director" and sets is None:
                raise ValueError(f"{name}: a director needs its set encoder, and this checkpoint or its stage.json "
                                 f"has none")
            write_amdl(partial, name, layout["obs_dim"], 1, layout["num_actions"], layers, memory, goals,
                       goal_every, feedback_weights(actor_state), slow_weights(actor_state), sets)
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

    width = layers[-1][0].shape[1]
    (outputs,) = struct.unpack_from("<I", data, offset)
    offset += 4
    feedback = None
    if outputs:
        feedback = {"foresight_weight": floats(outputs * width, outputs, width),
                    "foresight_bias": floats(outputs, outputs),
                    "feedback_weight": floats(width * outputs, width, outputs),
                    "feedback_bias": floats(width, width)}
    (slow_size,) = struct.unpack_from("<I", data, offset)
    offset += 4
    slow = None
    if slow_size:
        slow = {"weight_ih": floats(3 * slow_size * width, 3 * slow_size, width),
                "weight_hh": floats(3 * slow_size * slow_size, 3 * slow_size, slow_size),
                "bias_ih": floats(3 * slow_size, 3 * slow_size),
                "bias_hh": floats(3 * slow_size, 3 * slow_size)}

    kinds, targets, goal_every = struct.unpack_from("<III", data, offset)
    offset += 12
    goals = None
    if kinds:
        goal_width = slow_size or width
        goals = {"kind_weight": floats(kinds * goal_width, kinds, goal_width), "kind_bias": floats(kinds, kinds)}
        if targets > 1:
            goals["target_weight"] = floats(targets * goal_width, targets, goal_width)
            goals["target_bias"] = floats(targets, targets)
        goals["pair"] = floats(kinds * targets, kinds, targets)
        goals["accepts"] = np.frombuffer(data, dtype="u1", count=kinds * targets, offset=offset).reshape(kinds, targets)
        offset += kinds * targets
        (goals["block_at"],) = struct.unpack_from("<i", data, offset)
        offset += 4
        goals["kind_embedding"] = floats(kinds * width, kinds, width)
        if targets > 1:
            goals["target_embedding"] = floats(targets * width, targets, width)
        (lookahead,) = struct.unpack_from("<B", data, offset)
        offset += 1
        if lookahead:
            def part():
                values = {"kind_weight": floats(kinds * goal_width, kinds, goal_width), "kind_bias": floats(kinds, kinds)}
                if targets > 1:
                    values["target_weight"] = floats(targets * goal_width, targets, goal_width)
                    values["target_bias"] = floats(targets, targets)
                values["pair"] = floats(kinds * targets, kinds, targets)
                return values
            goals["lookahead"] = {"success": part(), "duration": part(), "weight": floats(2, 2)}
        (slots,) = struct.unpack_from("<I", data, offset)
        offset += 4
        goals["slots"] = slots
        if slots > 1:
            goals["slot_bias"] = floats((slots - 1) * goal_width, slots - 1, goal_width)
            goals["drawn"] = floats((kinds * targets + 1) * goal_width, kinds * targets + 1, goal_width)
            goals["none_bias"] = floats(slots - 1, slots - 1)
            (goals["gate"],) = struct.unpack_from("<f", data, offset)
            offset += 4

    (has_sets,) = struct.unpack_from("<B", data, offset)
    offset += 1
    sets = None
    if has_sets:
        (embed,) = struct.unpack_from("<I", data, offset)
        offset += 4
        sets = {"embed": embed, "pointers": []}
        for part in ("seats", "enemies"):
            first, slots, per, present = struct.unpack_from("<IIII", data, offset)
            offset += 16
            sets[part] = {"first": first, "slots": slots, "width": per, "present": present,
                          "w1": floats(embed * per, embed, per), "b1": floats(embed, embed),
                          "w2": floats(embed * embed, embed, embed), "b2": floats(embed, embed)}
        adapter_out = layers[0][0].shape[0]
        sets["pool_weight"] = floats(adapter_out * 4 * embed, adapter_out, 4 * embed)
        sets["pool_bias"] = floats(adapter_out, adapter_out)
        (count,) = struct.unpack_from("<I", data, offset)
        offset += 4
        for _ in range(count):
            first, over = struct.unpack_from("<II", data, offset)
            offset += 8
            sets["pointers"].append({"first": first, "over": "seats" if over == 0 else "enemies",
                                     "weight": floats(embed * width, embed, width), "bias": floats(embed, embed)})

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
        "feedback": feedback,
        "slow": slow,
        "sets": sets,
    }


def reference_decide(model: dict, obs: np.ndarray, mask: np.ndarray, agent: int = 0,
                     state: dict | None = None) -> tuple[int, np.ndarray]:
    """The forward pass of an exported model: returns (greedy allowed action, logits).

    `state` is what the policy carries between decisions -- {"memory": [R], "goal": int, "age": int} -- updated in
    place. A model without a memory or goals ignores it, and so behaves the same however it is called.
    """
    x = np.concatenate([obs.astype(np.float32), np.eye(model["num_agents"], dtype=np.float32)[agent]])
    layers = model["layers"]
    sets = model.get("sets")
    encoded = {}
    if sets is not None:
        # The director's members and enemies, each slot through the shared encoder, pooled onto the first layer.
        pooled = []
        for part in ("seats", "enemies"):
            spec = sets[part]
            raw = obs[spec["first"] : spec["first"] + spec["slots"] * spec["width"]].reshape(spec["slots"], spec["width"])
            hidden = np.tanh(np.tanh(raw @ spec["w1"].T + spec["b1"]) @ spec["w2"].T + spec["b2"])
            present = raw[:, spec["present"]] > 0.5
            encoded[part] = hidden
            count = max(1.0, float(present.sum()))
            pooled.append((hidden * present[:, None]).sum(axis=0) / count)
            pooled.append(np.where(present[:, None], hidden, -1.0).max(axis=0))
        extra = sets["pool_weight"] @ np.concatenate(pooled) + sets["pool_bias"]
    for index, (weight, bias) in enumerate(layers[:-1]):
        x = np.tanh(weight @ x + bias + (extra if sets is not None and index == 0 else 0.0))

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

    # The predictions fed back (Component P, layer 2), onto what the action head and the slow loop read.
    raw = x
    feedback = model.get("feedback")
    if feedback is not None:
        predictions = feedback["foresight_weight"] @ x + feedback["foresight_bias"]
        x = x + feedback["feedback_weight"] @ predictions + feedback["feedback_bias"]

    goals = model.get("goals")
    if goals is not None:
        x = _reference_goals(model, goals, obs, raw, x, state)

    weight, bias = layers[-1]
    features = x
    x = weight @ x + bias
    # The director's per-slot actions: each slot's encoding against a query from the features.
    for pointer in (sets["pointers"] if sets is not None else ()):
        scores = encoded[pointer["over"]] @ (pointer["weight"] @ features + pointer["bias"])
        x[pointer["first"] : pointer["first"] + len(scores)] = scores

    allowed = mask.astype(bool)
    if not allowed.any():
        return 0, x
    return int(np.where(allowed, x, -np.inf).argmax()), x


def _reference_goals(model: dict, goals: dict, obs: np.ndarray, raw: np.ndarray, x: np.ndarray,
                     state: dict | None) -> np.ndarray:
    """The goal decision of one step (LayoutActor.decide_goals, greedy) and its embedding added to `x`. `state`
    carries "goal" (the pair held, with two slots or more), "queue", "age" and "slow"."""
    every = max(1, model.get("goal_every", 1))
    age = 0 if state is None else state.get("age", 0)
    goal = 0 if state is None else state.get("goal", 0)
    kinds, targets = goals["pair"].shape
    count = kinds * targets
    slots = int(goals.get("slots", 1))
    accepts = goals["accepts"].astype(bool)
    allowed = accepts.copy()
    at = int(goals["block_at"])
    base = kinds + targets
    ended = secondary_ended = event = from_order = False
    order_goal = 0
    if at < 0 and targets > 1:
        allowed[:] = False              # no goal block (the director): only the first goal, which means none
    if at >= 0 and targets > 1:
        width = base + 2 + (3 + 2 * base if slots > 1 else 0)
        block = obs[at : at + width] > 0.5
        present = block[:kinds, None] & block[kinds:base][None, :]
        allowed &= present
        ended = bool(block[base])
        if slots > 1:
            secondary_ended, event, from_order = bool(block[base + 2]), bool(block[base + 3]), bool(block[base + 4])
            order_kind = int(np.argmax(block[base + 5 : base + 5 + kinds]))
            order_target = int(np.argmax(block[base + 5 + kinds : base + 5 + base]))
            order_goal = order_kind * targets + order_target
    allowed = allowed.reshape(-1)
    allowed[0] = True

    queue = [-1] * max(0, slots - 2) if state is None else state.setdefault("queue", [-1] * max(0, slots - 2))
    if slots > 1:
        primary, secondary = goal // (count + 1), goal % (count + 1) - 1
        if secondary_ended:
            secondary = -1
        promote = ended and queue[0] >= 0
        if promote:
            primary, queue = queue[0], queue[1:] + [-1]
        choose = age % every == 0 or (ended and not promote) or event
    else:
        primary, secondary = goal, -1
        choose = age % every == 0 or ended

    if choose:
        # From the slow loop, stepped on the fed-back features, or from the plain features without one.
        source = raw
        slow = model.get("slow")
        if slow is not None:
            size = slow["weight_hh"].shape[1]
            carried = np.zeros(size, np.float32) if state is None else state.setdefault(
                "slow", np.zeros(size, np.float32))
            source = _gru(slow, x, carried)
            if state is not None:
                state["slow"] = source

        def factored(part, features):
            joint = (part["kind_weight"] @ features + part["kind_bias"])[:, None] + part["pair"]
            if targets > 1:
                joint = joint + (part["target_weight"] @ features + part["target_bias"])[None, :]
            return joint

        joint = factored(goals, source)
        lookahead = goals.get("lookahead")
        if lookahead is not None:
            joint = (joint + lookahead["weight"][0] * factored(lookahead["success"], source)
                     + lookahead["weight"][1] * _sigmoid(factored(lookahead["duration"], source)))
        drawn = [int(np.where(allowed, joint.reshape(-1), -np.inf).argmax())]
        if slots > 1:
            before = [order_goal if from_order else drawn[0]]
            for slot in range(1, slots):
                shifted = source + goals["slot_bias"][slot - 1]
                for previous in before:
                    shifted = shifted + goals["drawn"][previous + 1]
                later = accepts.copy()
                if slot == 1 and at >= 0 and targets > 1:
                    later &= present
                if at < 0 and targets > 1:
                    later[:] = False
                scores = np.concatenate([np.where(later.reshape(-1), factored(goals, shifted).reshape(-1), -np.inf),
                                         [goals["none_bias"][slot - 1]]])
                pick = int(scores.argmax())
                pick = -1 if pick == count else pick
                before.append(pick)
                drawn.append(pick)
            primary, secondary, queue = drawn[0], drawn[1], drawn[2:]
        else:
            primary = drawn[0]
    if slots > 1:
        if from_order:
            primary = order_goal
        if secondary == primary:
            secondary = -1
        goal = primary * (count + 1) + secondary + 1
    else:
        goal = primary
    if state is not None:
        state["goal"], state["age"], state["queue"] = goal, 1 if choose else age + 1, list(queue)
    x = x + goals["kind_embedding"][primary // targets]
    if targets > 1:
        x = x + goals["target_embedding"][primary % targets]
    if slots > 1 and secondary >= 0:
        extra = goals["kind_embedding"][secondary // targets]
        if targets > 1:
            extra = extra + goals["target_embedding"][secondary % targets]
        x = x + goals["gate"] * extra
    return x


def _sigmoid(x: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-x))


def _gru(weights: dict, x: np.ndarray, carried: np.ndarray) -> np.ndarray:
    """One torch.nn.GRUCell step."""
    size = weights["weight_hh"].shape[1]
    gates = weights["weight_ih"] @ x + weights["bias_ih"]
    recurrent = weights["weight_hh"] @ carried + weights["bias_hh"]
    reset = _sigmoid(gates[:size] + recurrent[:size])
    update = _sigmoid(gates[size:2 * size] + recurrent[size:2 * size])
    candidate = np.tanh(gates[2 * size:] + reset * recurrent[2 * size:])
    return (1.0 - update) * candidate + update * carried


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
