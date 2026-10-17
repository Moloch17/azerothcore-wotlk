"""Golden vectors for the realm's forward pass: a checkpoint's layouts exported as .amdl, and the learner's own network
run over a sequence of decisions, so a C++ reader (mod-animus: animus-lib/tools/golden_check.cpp) can be checked
against it.

    python -m animus.golden --checkpoint runs/move4_follow/latest.pt --out golden/ --layouts warrior druid priest

For each layout it writes <model>.amdl (animus.export) and <model>.golden: one seat's decisions from a cleared state,
each its inputs and what the learner decided -- exactly as the trainer's acting path decides at evaluation
(MappoTrainer._decide with deterministic=True: the goals, the cells, the action and the free look all argmax, the
heading its mode). The
oracle is the network itself (distill.frozen_actor), not a numpy copy of it.

The inputs are synthetic but in range (no recorded observations or images are kept by a run): the observation drawn
from the layout's own normaliser statistics, the camera's scalars, entity list and map scalars as the sim writes them
(integer classes and types, present flags), a blocky image of the pixel classes, a crop of floor, walls and unknown with
heights near the feet, and a goal block whose ended, secondary-ended and event columns fire on chosen steps so every
branch of the goal decision is taken.

The .golden format (little-endian):

    char[4] "AGLD", u32 version (1; 2 for a layout that steers by a heading, which alone has the fields marked (2))
    u32 obs_dim, num_actions, image_bytes, crop_bytes, look_total, look_heads, recurrent, slow, features, camera_term,
        steps, (2) heading_components (K)
    per step:
        inputs:   f32 obs[obs_dim], u8 mask[num_actions], u8 image[image_bytes], u8 crop[crop_bytes]
        expected: f32 logits[num_actions] (unmasked), i32 action, f32 look_logits[look_total], i32 look[look_heads],
                  u8 chosen, i32 goal (the pair held), i32 queue[2], i32 cells[4] (-1 where the step chose nothing),
                  u32 plan[4], u32 serial, u32 age, f32 memory[recurrent], f32 slow[slow], f32 features[features] (the
                  action head's input), f32 camera_term[camera_term] (what the camera adds to the first layer),
                  (2) f32 heading_raw[4K] (the heading head's Linear on those features, unclamped, in the .amdl's
                  order), f32 heading (the mode, radians: export.reference_heading of heading_raw's inputs)
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

import numpy as np
import torch

from .distill import frozen_actor
from .export import export_layouts, read_amdl, reference_heading
from .mappo.networks import vision_term

GOLDEN_MAGIC = b"AGLD"
GOLDEN_VERSION = 1
#: The version of a layout that steers by a heading (HeadingHead): the header's heading_components, each step's heading.
GOLDEN_HEADING_VERSION = 2
#: How far the exported heading section's mode (float64 numpy) may be from the learner's (float32 torch), radians.
HEADING_TOLERANCE = 1e-3


def _image(rng: np.random.Generator, height: int, width: int, patch: int, classes: list[int]) -> np.ndarray:
    """A blocky camera image [H, W, 4]: each patch a class (sky above, ground below, some models and doors), its pixels
    the class with a little noise, the objective bit on a few."""
    out = np.zeros((height, width, 4), dtype=np.uint8)
    for r in range(0, height, patch):
        for c in range(0, width, patch):
            sky = r < height * 0.35 and rng.random() < 0.7
            klass = classes[0] if sky else int(rng.choice(classes[1:]))
            distance = 255 if sky else int(rng.integers(5, 250))
            block = out[r:r + patch, c:c + patch]
            block[..., 0] = np.clip(distance + rng.integers(-4, 5, size=block.shape[:2]), 0, 255) if not sky else 255
            block[..., 1] = np.clip(128 + rng.integers(-60, 61) + rng.integers(-3, 4, size=block.shape[:2]), 0, 255)
            block[..., 2] = rng.integers(0, 256, size=block.shape[:2])
            objective = (rng.random(block.shape[:2]) < 0.03).astype(np.uint8)
            noisy = rng.random(block.shape[:2]) < 0.1
            pixel_class = np.where(noisy, rng.choice(classes, size=block.shape[:2]), klass).astype(np.uint8)
            block[..., 3] = pixel_class | (objective << 5)
    return out.reshape(-1)


def _crop(rng: np.random.Generator, side: int, pool: int, empty: bool = False) -> np.ndarray:
    """A mental map crop [side, side, 8]: per pool x pool block mostly floor near the feet's height, some walls, doors,
    hazards and unknown; visited, age, class, frontier, searched and walls (map block revision 6: the wall count of
    the cell's four 1-yd cells, 3 or 4 on a wall cell) as the sim writes them. `empty`: all unknown."""
    cells = np.zeros((side, side, 8), dtype=np.uint8)
    if empty:
        cells[..., 3] = 255
        return cells.reshape(-1)
    for r in range(0, side, pool):
        for c in range(0, side, pool):
            code = int(rng.choice([0, 1, 1, 1, 1, 2, 3, 4], p=[0.2, 0.15, 0.15, 0.15, 0.15, 0.1, 0.05, 0.05]))
            block = cells[r:r + pool, c:c + pool]
            codes = np.where(rng.random(block.shape[:2]) < 0.85, code, rng.integers(0, 5, size=block.shape[:2]))
            block[..., 0] = codes
            rise = int(rng.integers(-24, 25))
            heights = np.clip(128 + rise + rng.integers(-3, 4, size=block.shape[:2]), 1, 255)
            block[..., 1] = np.where(codes == 0, 0, heights)
            block[..., 2] = (rng.random(block.shape[:2]) < 0.3) & (codes != 0)
            block[..., 3] = np.where(codes == 0, 255, rng.integers(0, 255, size=block.shape[:2]))
            block[..., 4] = np.where(rng.random(block.shape[:2]) < 0.04, rng.integers(1, 32, size=block.shape[:2]), 0)
            block[..., 5] = (rng.random(block.shape[:2]) < 0.15) & (codes != 0)
            block[..., 6] = rng.integers(0, 5, size=block.shape[:2]) * (codes != 0)
            # The wall count (a wall cell holds 3 or 4 of its four 1-yd cells, the majority rule; a corridor's edge
            # cell a stray 1 or 2 beside a floor code).
            edge = rng.random(block.shape[:2]) < 0.1
            block[..., 7] = np.where(codes == 2, rng.integers(3, 5, size=block.shape[:2]),
                                     np.where(edge, rng.integers(1, 3, size=block.shape[:2]), 0)) * (codes != 0)
    return cells.reshape(-1)


def _observation(rng: np.random.Generator, actor, index: int, vision: dict, step: int, steps: int) -> np.ndarray:
    """One in-range observation of layout `index` (see the module's docstring)."""
    norm = actor.norms[index]
    mean, sd = norm.mean.numpy().astype(np.float64), np.sqrt(norm.var.numpy().astype(np.float64) + 1e-5)
    obs = (mean + sd * rng.standard_normal(mean.shape[0])).astype(np.float32)

    first = vision["first"]
    obs[first:first + vision["scalars"]] = rng.uniform(-1.0, 1.0, vision["scalars"])
    listed = vision.get("entities")
    if listed is not None:
        present_share = 0.0 if step == 3 else 0.4
        for slot in range(listed["slots"]):
            row = obs[listed["first"] + slot * listed["width"]: listed["first"] + (slot + 1) * listed["width"]]
            row[:] = rng.uniform(0.0, 1.0, listed["width"])
            row[listed["present"]] = float(rng.random() < present_share)
            row[listed["class_column"]] = float(rng.integers(0, 32))
            row[listed["type_column"]] = float(rng.integers(0, 40000))
            if listed["object_column"] >= 0:
                row[listed["object_column"]] = float(rng.integers(0, 2))
            if listed["memory_column"] >= 0:
                row[listed["memory_column"]] = float(rng.integers(0, 50))
    crop = vision.get("map")
    if crop is not None:
        obs[crop["first"]:crop["first"] + crop["scalars"]] = rng.uniform(0.0, 1.0, crop["scalars"])

    head = actor.goal_head
    at = int(head.block_at[index])
    if at >= 0:
        kinds, targets = head.kinds, head.targets
        block = obs[at:at + head.block_width]
        block[:] = (rng.random(head.block_width) < 0.5).astype(np.float32)
        # Kinds and targets on offer: goal 0 (fight about none) absent on some steps; the cell goal's kind and target
        # (search, the cell target) usually there so cells are drawn.
        block[:kinds] = rng.random(kinds) < 0.5
        block[kinds:kinds + targets] = rng.random(targets) < 0.5
        if step % 4 == 1:
            block[0] = 0.0
            block[kinds] = 0.0
        if head.cell_joint >= 0 and step % 5 != 2:
            block[head.cell_joint // targets] = 1.0
            block[kinds + head.cell_joint % targets] = 1.0
        if head.cell_joint >= 0 and step in (0, 11, 20):
            # Only the cell goal's kind on offer, about the cell target and a few places (step 20: the cell target
            # alone, so every slot is a cell or none and the cells exclude each other's neighbours).
            block[:kinds] = 0.0
            block[head.cell_joint // targets] = 1.0
            block[kinds:kinds + targets] = (rng.random(targets) < 0.2) if step != 20 else 0.0
            block[kinds + head.cell_joint % targets] = 1.0
        if step == 18:
            block[:kinds + targets] = 0.0          # nothing on offer at a choice: only the first goal
        base = kinds + targets
        block[base] = float(step in (5, 9, 14, 18))            # the primary ended
        block[base + 1] = float(rng.random() < 0.2)              # reached
        columns = head.columns
        block[columns["secondary_ended"]] = float(step in (6, 9, 16))
        block[columns["event"]] = float(step in (11, 20))
        block[columns["achieved_kind"]:columns["achieved_kind"] + kinds] = 0.0
        block[columns["achieved_target"]:columns["achieved_target"] + targets] = 0.0
        if step == 12:
            block[columns["achieved_kind"] + 2] = 1.0
            block[columns["achieved_target"] + 3] = 1.0
        if head.pointer_on:
            places = head.place_at
            obs[at + places:at + places + head.place_count * 5] = rng.uniform(-1.0, 1.0, head.place_count * 5)
    return obs


def golden_layout(actor, layout_name: str, index: int, vision: dict, num_actions: int, steps: int, goal_every: int,
                  seed: int, path: Path, model: dict | None = None) -> None:
    """Run `steps` decisions of one seat of layout `index` from a cleared state and write them to `path`. With the
    exported `model` (read_amdl) its heading section is checked against the learner's mode at every step
    (export.reference_heading): a mismatch over HEADING_TOLERANCE radians is refused."""
    rng = np.random.default_rng(seed)
    image_bytes = vision["image_bytes"]
    crop_bytes = vision["map"]["map_bytes"] if vision.get("map") else 0
    side = vision["map"]["height"] if vision.get("map") else 0
    look_heads = list(actor.look_head.heads) if actor.look_head is not None else []
    recurrent, slow_size, width = actor.recurrent_size, actor.slow_size, actor.head_width
    camera_width = actor.adapters[0].out_features
    obs_dim = actor.obs_dims[index]
    head = actor.goal_head
    steering = actor.heading_head is not None and bool(actor.heading_head.has_heading[index])
    components = actor.heading_head.components if steering else 0

    memory = np.zeros(recurrent, np.float32)
    slow = np.zeros(slow_size, np.float32)
    goal, age, serial = 0, 0, 0
    queue = np.full(max(1, actor.goal_slots - 2), -1, np.int64)
    plan = np.zeros(4, np.int64)
    layout_t = torch.tensor([index])

    with open(path, "wb") as out:
        out.write(GOLDEN_MAGIC)
        header = [GOLDEN_HEADING_VERSION if steering else GOLDEN_VERSION, obs_dim, num_actions, image_bytes,
                  crop_bytes, sum(look_heads), len(look_heads), recurrent, slow_size, width, camera_width, steps]
        if steering:
            header.append(components)
        out.write(struct.pack(f"<{len(header)}I", *header))
        for step in range(steps):
            obs = _observation(rng, actor, index, vision, step, steps)
            mask = (rng.random(num_actions) < 0.7).astype(np.uint8)
            if step == 13:
                mask[:] = 0                                      # nothing allowed: action 0
            image = _image(rng, vision["height"], vision["width"], vision["patch"], list(vision["pixel_classes"]))
            # Step 11 chooses (the block's event) with no block choosable: the cell goal falls back and is withdrawn.
            # One synthetic crop a tier (a two-tier map's section is [fine][coarse], the same bytes each).
            crop = np.concatenate([_crop(rng, side, head.cell_pool if head.cell_on else 2, empty=step == 11)
                                   for _ in range(int(vision["map"].get("tiers", 1)))]) if crop_bytes else \
                np.zeros(0, np.uint8)

            with torch.no_grad():
                obs_t = torch.from_numpy(obs)[None]
                image_t = torch.from_numpy(np.concatenate([image, crop]))[None]
                features = actor.features(obs_t, layout_t, torch.from_numpy(memory)[None], image=image_t)
                term = vision_term(actor, obs_t, layout_t, image=image_t)[0]
                clock = torch.tensor([age % goal_every == 0])
                goal_features = actor.slow_step(features, torch.from_numpy(slow)[None])
                decided = actor.decide_goals(goal_features, obs_t, layout_t, torch.tensor([goal]),
                                             torch.from_numpy(queue)[None], clock, True, image=image_t,
                                             plan=torch.from_numpy(plan)[None], serial=torch.tensor([serial]))
                goal_t = decided["goal"]
                chosen = bool(decided["chosen"][0])
                policy = actor.policy_features(features, goal_t)
                every = torch.ones(1, num_actions, dtype=torch.bool)
                logits = actor.action_logits(features, layout_t, every, goal_t, obs=obs_t)[0]
                action = int(actor.action_logits(features, layout_t, torch.from_numpy(mask)[None].bool(), goal_t,
                                                 obs=obs_t)[0].argmax())
                look_logits = actor.look_head.linear(policy)[0] if actor.look_head is not None else torch.zeros(0)
                look = actor.look(features, layout_t, goal_t, True)[0][0] if actor.look_head is not None else \
                    torch.zeros(0, dtype=torch.long)
                heading_raw = actor.heading_head.linear(policy)[0] if steering else torch.zeros(0)
                heading = float(actor.heading(features, layout_t, goal_t, True)[0][0]) if steering else 0.0

            cells = decided["cells"][0].numpy() if "cells" in decided and chosen else np.full(4, -1)
            cells = np.concatenate([cells, np.full(4 - len(cells), -1)]) if len(cells) < 4 else cells
            new_plan = decided["plan"][0].numpy() if "plan" in decided else plan
            # The acting state, as MappoTrainer's acting path keeps it.
            age = 1 if chosen else age + 1
            serial = serial + 1 if chosen else serial
            goal = int(goal_t[0])
            queue = decided["queue"][0].numpy().astype(np.int64)
            plan = new_plan.astype(np.int64)
            if chosen:
                slow = goal_features[0].numpy().copy()
            memory = features[0].numpy().copy()

            out.write(obs.astype("<f4").tobytes())
            out.write(mask.tobytes())
            out.write(image.tobytes())
            out.write(crop.tobytes())
            out.write(logits.numpy().astype("<f4").tobytes())
            out.write(struct.pack("<i", action))
            out.write(look_logits.numpy().astype("<f4").tobytes())
            out.write(np.asarray(look.numpy(), dtype="<i4").tobytes())
            out.write(struct.pack("<B", chosen))
            out.write(struct.pack("<i", goal))
            padded = np.concatenate([queue, np.full(max(0, 2 - len(queue)), -1)])[:2]
            out.write(np.asarray(padded, dtype="<i4").tobytes())
            out.write(np.asarray(cells[:4], dtype="<i4").tobytes())
            out.write(np.asarray(plan[:4], dtype="<u4").tobytes())
            out.write(struct.pack("<II", serial, age))
            out.write(memory.astype("<f4").tobytes())
            out.write(slow.astype("<f4").tobytes())
            out.write(policy[0].numpy().astype("<f4").tobytes())
            out.write(term.numpy().astype("<f4").tobytes())
            if steering:
                out.write(heading_raw.numpy().astype("<f4").tobytes())
                out.write(struct.pack("<f", heading))
                exported = (model or {}).get("tail") or {}
                if exported.get("heading") is not None:
                    theirs = reference_heading(exported["heading"], policy[0].numpy())
                    off = abs(math.remainder(theirs - heading, 2.0 * math.pi))
                    if off > HEADING_TOLERANCE:
                        raise ValueError(f"{layout_name} step {step}: the exported heading's mode is {theirs:.6f}, "
                                         f"the learner's {heading:.6f}")
            print(f"  {layout_name} step {step:2d}: action {action:3d} goal {goal:5d} chosen {int(chosen)} "
                  f"queue {padded.tolist()} cells {cells[:4].tolist()} look {look.tolist()}"
                  + (f" heading {np.degrees(heading):7.2f}" if steering else ""))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--stage", help="the run's stage.json (default: the checkpoint's own, else beside it)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--layouts", nargs="+", default=["warrior", "druid", "priest"])
    parser.add_argument("--steps", type=int, default=24)
    parser.add_argument("--seed", type=int, default=20261012)
    parser.add_argument("--no-none", action="store_true",
                        help="a check of the reader only: the later goal slots' 'none' made the worst choice (both the "
                             "exported model and the oracle), so every slot draws a goal -- and with the cell goal "
                             "alone on offer, a cell beside the cells before it")
    args = parser.parse_args()

    torch.set_num_threads(1)
    checkpoint_path = Path(args.checkpoint)
    checkpoint = torch.load(checkpoint_path, map_location="cpu", weights_only=False)
    if args.stage:
        checkpoint["stage"] = json.loads(Path(args.stage).read_text())
    elif checkpoint.get("stage") is None and (checkpoint_path.parent / "stage.json").is_file():
        checkpoint["stage"] = json.loads((checkpoint_path.parent / "stage.json").read_text())
    spec, state = checkpoint["spec"], checkpoint["trainer"]["actor"]
    if args.no_none and "goal_head.none_bias" in state:
        state["goal_head.none_bias"] = torch.full_like(state["goal_head.none_bias"], -50.0)
    names = [entry["name"] for entry in spec["layouts"]]
    goal_every = max(1, int(checkpoint["config"]["mappo"].get("goal_every_decisions", 1) or 1))

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    written = export_layouts(state, spec, out, checkpoint_path.parent, goal_every, only=args.layouts,
                             stage=checkpoint["stage"])
    actor = frozen_actor(checkpoint, "cpu")
    for path in written:
        model = read_amdl(path)
        layout_name = next(name for name in args.layouts if model["name"] in (name, checkpoint["stage"].get(
            "models", {}).get(name)))
        index = names.index(layout_name)
        keep = state.get(f"vision_keep_{index}")
        if keep is not None:
            blind = (keep[0] == 0).nonzero().flatten()
            print(f"{path.name}: version {model['version']}, {len(blind)} camera columns blind, adapter weight there "
                  f"at most {float(state[f'adapters.{index}.weight'][:, blind].abs().max()):.3g}")
        golden = path.with_suffix(".golden")
        golden_layout(actor, layout_name, index, actor.frozen_vision[index], spec["layouts"][index]["num_actions"],
                      args.steps, goal_every, args.seed + index, golden, model)
        print(f"Wrote {path} and {golden}")


if __name__ == "__main__":
    main()
