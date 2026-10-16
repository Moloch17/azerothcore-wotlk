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
        f32  kind_scale[goal_kinds * feature_width]      (version 7) the goal's scale on the features the action
        if goal_targets > 1:                                head reads: features x (1 + scale) + embedding (FiLM),
            f32  target_scale[goal_targets * feature_width] the secondary's through the gate as its embedding is
    (goal_width: slow_size when there is one, else feature_width)
    u8       director_sets                  (a director's members and enemies as sets; no longer written: 0)
    u8       seat_sets                      (version 8) this layout's entities as sets (EntitySets), else 0
    if seat_sets:
        u32  embed, u32 set_count
        u8   attention                      (version 9) a pre-norm attention layer over the tokens (EntitySets), else 0
        per set: u32 slots, u32 width, u32 present (slot 0's present column), u32 present_stride, u32 segment_count;
            per segment: u32 first, u32 stride   (slot k reads stride columns from first + k * stride; in order)
            f32 w1[embed * width], b1[embed], w2[embed * embed], b2[embed]     encoder: tanh after each
        if attention:                       tokens: own, then every set's slots in order, each its code + type_embed;
                                            the absent slots no keys. x += out(MHA(LN_a(x))); x += W2 relu(W1 LN_m(x))
            u32  heads
            f32  type_embed[set_count * embed], own_token[embed]
            f32  norm_attend weight[embed], bias[embed]      LayerNorm, eps 1e-5, biased variance (torch's)
            f32  in_proj weight[3 * embed * embed], bias[3 * embed]   q, k, v stacked in that order (torch's
                                            nn.MultiheadAttention in_proj); head h is columns h * embed/heads.. of each;
                                            scores q.k / sqrt(embed/heads), softmax over the present keys
            f32  out_proj weight[embed * embed], bias[embed]
            f32  norm_mix weight[embed], bias[embed]
            f32  mix_in weight[2 * embed * embed], bias[2 * embed], mix_out weight[embed * 2 * embed], bias[embed]
        f32  pool_weight[adapter_out * (2 * embed * set_count + (embed if attention))], pool_bias[adapter_out]   mean
                                            and max of each set's present slots (attended), in set order, then the
                                            own token (attended); added to the first layer before its tanh
        u32  pointer_count; per pointer: u32 first_action, u32 set (its index above),
            f32 query_weight[embed * feature_width], query_bias[embed]   its actions' logits are slot . query
    (version 10 on; a file of 10 is written only for a policy that needs the tail -- a camera, a place pointer, a
    cell head, or two goals read from a goal block of revision 2 on -- every other policy is still written as 9)
    u8       goal_space                     1: the stage had a goal block, so a layout without one has nothing to choose
    u8       place_pointer                  the goal head's pointer over place slots (GoalHead.pointer)
    if place_pointer:
        u32  at (its first column inside the goal block), first_target, count, features, hidden
        f32  w1[hidden * features], b1[hidden], w2[hidden], b2      score = w2 . tanh(w1 x + b1) + b2, per slot, added
                                            to target first_target + k of every kind (primary and later slots)
    u8       cells                          the goal head's cell pointer (GoalHead.cell, CellPointer)
    if cells:
        u32  crop_offset (the crop's first byte in the crop buffer), grid, pool, joint (the cell goal's joint id),
             rise_units, min_floor_cells, features (22), hidden (32), chain_hidden (16)
        f32  local_weight[hidden * features], local_bias[hidden], query_weight[hidden * goal_width], query_bias[hidden],
             score_weight[hidden], score_bias, chain1_weight[chain_hidden * 3], chain1_bias[chain_hidden],
             chain2_weight[chain_hidden], chain2_bias
    u8       camera                         the camera (VisionEncoder) and its free look (LookHead)
    if camera:
        u32  height, width, patch, scalars, scalars_first (the vision block's first column), crop_bytes (the map's
             bytes that travel with the image, read or not)
        u32  pixel_class_count, then pixel_classes[pixel_class_count]     the one-hot planes' classes, in plane order
        u32  class_limit, class_width; f32 class_embed[class_limit * class_width]   (the entity list's and the map's)
        u32  patch_out, mix_out, embed
        f32  patch_weight[patch_out * patch * patch * (4 + pixel_class_count)], patch_bias[patch_out],
             mix_weight[mix_out * patch_out], mix_bias[mix_out], embed_weight[embed * (2 * mix_out + scalars)],
             embed_bias[embed]
        u8   entities                       the entity list (VisibleEntities)
        if entities:
            u32  first, slots, width, present, class_column, type_column; i32 object_column, memory_column;
            u32  buckets, type_width, token
            f32  type_embed[buckets * type_width], w1[token * (width - 2 + class_width + type_width)], b1[token],
                 w2[token * token], b2[token], pool_weight[embed * 2 * token], pool_bias[embed]
        u8   map                            the mental map's encoder (MapEncoder)
        if map:
            u32  first (its scalars' first column), scalars, height, width, patch, codes, code_width, patch_out,
                 mix_out, map_embed
            u32  tiers                      (version 11 only) the crop section's tiers, [fine][coarse] at 2
            f32  code_embed[codes * code_width], patch_weight[patch_out * patch * patch * (code_width + class_width +
                 6)], patch_bias, mix_weight[mix_out * patch_out], mix_bias, embed_weight[map_embed * (tiers * 3 *
                 mix_out + scalars)], embed_bias, join_weight[embed * map_embed], join_bias[embed]
            if tiers > 1:  f32 tier_embed[tiers * mix_out]    added to each tier's patch features (version 11)
        f32  join_weight[adapter_out * embed], join_bias[adapter_out]        (VisionJoin)
        u32  look_heads, then sizes[look_heads]
        f32  look_weight[sum(sizes) * feature_width], look_bias[sum(sizes)]  (LookHead, on the action head's features)
    (version 11 on: a file of 11 is written only for a policy with the planner; every other policy is still 10 or 9)
    u8       planner                        the costmap head and the planner (MapEncoder.plan, a two-tier map)
    if planner:
        u32  tiers, classes (11), vision (8), hidden (32), k (40), channels (8), planes (16, the map's planes a
             cell), readout (9), readout_first (22: the centre 3 x 3 of the value map), compass_present,
             compass_sin, compass_cos, compass_dist (this layout's compass block columns; all 0xFFFFFFFF = none)
        f32  tier_cells[tiers] (yards), compass_scale (yards the distance column is over)
        f32  costmap: vision_weight[vision * embed], vision_bias[vision], tier_bias[tiers * vision],
             conv1_weight[hidden * (planes + vision) * 9], conv1_bias[hidden], conv2_weight[hidden * hidden * 9],
             conv2_bias[hidden], out_weight[classes * hidden], out_bias[classes]     (3 x 3 convs padded 1, SiLU
             after the first two; the camera's embedding -- its `embed` output before the SiLU -- through vision,
             plus the tier's bias, broadcast over the crop as planes + vision input channels)
        f32  planner: fixed_weight[channels * (classes + planes + 1) * 9], fixed_bias[channels],
             recur_weight[channels * 1 * 9]         (value_0 = 0; k times value = max over channels of
             conv3x3(value; recur) + conv3x3(softmax(costmap), planes, goal plane; fixed))
        f32  join_weight[embed * tiers * readout], join_bias[embed]      the readouts (each tier's value at rows and
             columns readout_first..+2) added to the camera's embedding before its SiLU
        f32  pointer_weight                 the cell pointer's bonus: value averaged over each pool x pool block
                                            of the fine tier times this, added to every cell score
        In version 13 (a policy with the camera splat; mappo.camera_splat, networks.CameraSplat), right after
        compass_scale and before the costmap's arrays:
        u32  splat_channels                 C (8); conv1 then reads planes + vision + C + 1 input channels, the
                                            splat's C averaged features and its covered flag after the vision ones
        u32  stride (2), scalar_yaw_sin (0), scalar_yaw_cos (1), scalar_pitch (2), scalar_boom (4),
             scalar_render_width (10)      (the vision block's scalars read, by position)
        f32  fov_h, fov_v (degrees), near (0.25), distance_reference (1000), zoom_scale (12)
        f32  splat_weight[C * mix_out], splat_bias[C]     the projection of a camera patch's features (the camera's
                                            mix output, after its SiLU)
        The splat (networks.CameraSplat, the reference): the samples are the canonical pixels (r, c), r = stride/2,
        stride/2 + stride, ... < height, c likewise < width, row by row; each takes its patch's projected features
        (patch (r / patch) x (width / patch) + c / patch). Its ray: cast width w = round(scalar_render_width x width)
        (the canonical width when the scalar is 0), clamped 1..width, h = round(w x height / width) clamped 1..height;
        yaw_right = ((floor(c w / width) + 0.5) / w x fov_h - fov_h / 2), pitch_up = (fov_v / 2 - (floor(r h / height) +
        0.5) / h x fov_v), radians; the depth byte b (the pixel's byte 0; 255 sky lands nowhere) gives d = near x
        (distance_reference / near) ^ (b / 254). With the scalars' yaw sin s and cos k, pitch p = scalar x pi / 2 and
        boom B = scalar x zoom_scale: cos a = k cos(yaw_right) + s sin(yaw_right), sin a = s cos(yaw_right) - k
        sin(yaw_right), level = d cos(p + pitch_up), forward = level cos a - B cos(p) k, right = -(level sin a - B
        cos(p) s). Per tier of cell yards (tier_cells; the map's height x width crop, side = height): row =
        floor(side / 2 - forward / cell), col = floor(side / 2 + right / cell); a sample inside 0..side adds its
        features to that cell; a cell's planes are the mean of what landed in it (0 when nothing did) and covered (1
        when anything did). Float32 throughout, as the learner computes it.
        In version 14 (a policy with the planned waypoint; mappo.planner_waypoint, networks.MapEncoder.waypoints),
        after pointer_weight:
        u32  waypoint_features              F = tiers x 4 (0: none, and nothing follows)
        if F:
            u32  anchor (23), steps[tiers] (5, 2)
            f32  waypoint_weight[embed * F], waypoint_bias[embed]    added to the camera's embedding before its SiLU
                                            with the readouts' join
        The waypoint (networks.descend and MapEncoder.waypoints, the reference), per tier of cell yards, from the
        tier's value map after the k iterations, cost = -value x 100: start at cell (anchor, anchor) (row 0 furthest
        ahead, column 0 furthest left; the readouts' centre); up to steps[tier] times, take the eight neighbours in the
        order (-1,-1), (-1,0), (-1,1), (0,-1), (0,1), (1,-1), (1,0), (1,1) as (row, column) offsets, those past the
        crop never chosen, and move to the one of the lowest cost (the first of equals) only when its cost is strictly
        lower than the current cell's, else stop. With c = (height - 1) / 2, the reached cell (r, k) is forward =
        (c - r) x cell, right = (k - c) x cell yards from the body, d = sqrt(forward^2 + right^2); valid = 1 when the
        cell is not the start and the compass's objective is present (presence column > 0.5), else 0. The tier's four
        numbers: -right / d x valid, forward / d x valid, d / (steps[tier] x cell) x valid, valid; the tiers' fours in
        tier order are the F inputs. They are computed with the readouts (each replan) and kept between.
    (version 12 on: a file of 12 is written only for a policy with the heading; every other policy is still 11, 10 or
    9. The camera's flag and, without a camera, the planner's 0 flag come before it as in version 11. A file of 13 --
    a policy with the camera splat -- or of 14 -- one with the planned waypoint -- has the section too)
    u8       heading                        the continuous heading (HeadingHead, heading.CONTRACT.md sec 5)
    if heading:
        u32  components (K, 3), feature_width
        f32  kappa_min (0.5), kappa_max (200), weight_floor (0.02)
        f32  weight[4K * feature_width], bias[4K]      one Linear on the action head's features (with the goal's
                                            conditioning, as the free look's); its 4K outputs in this order:
                                            [0, 2K) component k's mean direction (x, y) at 2k, 2k + 1;
                                            [2K, 3K) kappa_k = clamp(softplus(.) + kappa_min, kappa_min, kappa_max);
                                            [3K, 4K) the mixture logits, w = floor + (1 - K floor) softmax(.)
        The realm plays the mode: theta = atan2(y_k, x_k) of the component k with the largest mixture logit (the
        first of equals), radians in (-pi, pi], the target heading relative to the body's facing (0 holds it).

The camera (version 10, mappo.networks.VisionEncoder): the image's bytes [height][width][4] -- distance, height, normal,
class byte -- are decoded as the sim's Vision::DecodePixel (255 -> 1.0 else b / 254; (b - 128) / 125; b / 255; the
objective bit 5) and the class (b & 31) one-hot over pixel_classes; patch x patch squares, each its pixels row by row
and every pixel's planes (distance, height, normal, objective, one-hot), through patch + SiLU and mix + SiLU; per
channel the spatial softmax's expected (x, y) over the patch grid (linspace -1..1), then the block's scalars, embed. The
map's crop (height x width x 7 bytes, decode_map) and the entity list's pool are added to it, then SiLU, then the join,
which is added to the first layer before its tanh. The free look's heads are each an argmax at evaluation.

A version 10 model also decides its goals as the learner does now (LayoutActor.decide_goals, GoalHead.draw): the goal
block has no order columns (revision 2 on), the first goal is allowed only when nothing else is, a queued goal is never
one already drawn at the same choice, and with a cell pointer every slot whose goal is the cell goal names a block of
the crop. Versions 7 to 9 keep the reader's older rules.

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
# the goal block's next-run columns (the secondary ending, the event, an order's primary, what was achieved).
# 7: the goal also scales the action head's features (kind_scale, target_scale), so it can change what the seat does
# in a situation rather than only shift each action's logit.
# 8: a seat layout's entities as sets (seat_sets, mappo.seat_sets: EntitySets). A reader of 8 reads 7 too: a 7 file
# is an 8 without the section.
# 9: the seat sets' attention layer (mappo.entity_attention): the section's attention flag and, with it, the layer and
# the own token's pool columns. A reader of 9 reads 7 and 8.
# 10: the camera (VisionEncoder with its map and entity list), its free look (LookHead), and the goal head's place
# pointer and cell pointer, as a tail after the seat sets, and the learner's current goal decision. Written only where
# a policy has one of them; every other policy is still written as 9, which a reader of 9 keeps reading.
# 11: the nav stages' two-tier map (the map section's tiers and tier embedding) and the planner section (the costmap
# head, the VIN, its join and the pointer's bonus) after the camera's tail (vision movement curriculum, CONTRACT.md
# sec 5). Written only for a policy with the planner; every other policy is still 10 or 9. A checkpoint with a planner
# is never written as 10 (write_amdl refuses).
# 12: the continuous heading (HeadingHead, heading.CONTRACT.md sec 5): a section after the planner's. Written only for a
# policy with the heading; every other policy is still 11, 10 or 9. A checkpoint with a heading head is never written
# as 11 or below (write_amdl refuses).
# 13: the camera splat (networks.CameraSplat, mappo.camera_splat): the planner section's splat header and projection,
# and conv1 reading the splat's planes. Written only for a policy with the splat; every other policy is still 12, 11, 10
# or 9. A checkpoint with the splat is never written as 12 or below (write_amdl refuses).
# 14: the planned waypoint (mappo.planner_waypoint, networks.MapEncoder.waypoints): the planner section's waypoint
# count, walk and join after the pointer's weight. Written only for a policy with the waypoint; every other policy is
# still 13 or below. A checkpoint with the waypoint is never written as 13 or below (write_amdl refuses).
AMDL_VERSION = 14
AMDL_VERSIONS = (7, 8, 9, 10, 11, 12, 13, 14)
#: The version a policy with the camera splat and no planned waypoint is written as.
AMDL_SPLAT_VERSION = 13
#: The version a policy with the heading and no camera splat is written as.
AMDL_HEADING_VERSION = 12
#: The version a policy with the planner and no heading is written as.
AMDL_PLANNER_VERSION = 11
#: The version a policy without the tail is written as, and the one with the camera tail and no planner.
AMDL_PLAIN_VERSION = 9
AMDL_CAMERA_VERSION = 10

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
    seat_sets: dict | None = None,
    tail: dict | None = None,
) -> None:
    """Write one model. `tail` is the version 10 tail (tail_weights); without one the file is version 9. A tail with
    the planned waypoint is version 14, one with the camera splat (and no waypoint) 13, one with a heading (and
    neither) 12, one with a planner (and none of them) 11."""
    if layers[0][0].shape[1] != obs_dim + num_agents:
        raise ValueError(f"first layer takes {layers[0][0].shape[1]} inputs, expected {obs_dim} + {num_agents}")
    if memory is not None and layers[-1][0].shape[1] != memory["weight_hh"].shape[1]:
        raise ValueError("the action head does not read the memory's state")
    if layers[-1][0].shape[0] != num_actions:
        raise ValueError(f"last layer has {layers[-1][0].shape[0]} outputs, expected {num_actions}")

    encoded = name.encode("utf-8")
    planner = bool(tail and tail.get("planner"))
    heading = bool(tail and tail.get("heading"))
    splat = bool(planner and tail["planner"].get("splat"))
    waypoint = bool(planner and tail["planner"].get("waypoint"))
    version = (AMDL_VERSION if waypoint else AMDL_SPLAT_VERSION if splat else AMDL_HEADING_VERSION if heading
               else AMDL_PLANNER_VERSION if planner else (AMDL_CAMERA_VERSION if tail else AMDL_PLAIN_VERSION))
    if planner and version < AMDL_PLANNER_VERSION:
        raise ValueError("a policy with a planner is written as version 11 or above, never 10")
    if heading and version < AMDL_HEADING_VERSION:
        raise ValueError("a policy with a heading head is written as version 12 or above, never 11 or below")
    if splat and version < AMDL_SPLAT_VERSION:
        raise ValueError("a policy with the camera splat is written as version 13, never 12 or below")
    if waypoint and version < AMDL_VERSION:
        raise ValueError("a policy with the planned waypoint is written as version 14, never 13 or below")
    with open(path, "wb") as out:
        out.write(AMDL_MAGIC)
        out.write(struct.pack("<IH", version, len(encoded)))
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
            out.write(np.ascontiguousarray(goals["kind_scale"], dtype="<f4").tobytes())
            if targets > 1:
                out.write(np.ascontiguousarray(goals["target_scale"], dtype="<f4").tobytes())

        out.write(struct.pack("<B", 0))      # director_sets: the section is never written (see the format above)

        out.write(struct.pack("<B", 1 if seat_sets else 0))
        if seat_sets:
            out.write(struct.pack("<II", int(seat_sets["embed"]), len(seat_sets["sets"])))
            attention = seat_sets.get("attention")
            out.write(struct.pack("<B", 1 if attention else 0))
            for entry in seat_sets["sets"]:
                out.write(struct.pack("<IIIII", entry["slots"], entry["width"], entry["present"],
                                      entry["present_stride"], len(entry["segments"])))
                for first, stride in entry["segments"]:
                    out.write(struct.pack("<II", first, stride))
                for name in ("w1", "b1", "w2", "b2"):
                    out.write(np.ascontiguousarray(entry[name], dtype="<f4").tobytes())
            if attention:
                out.write(struct.pack("<I", int(attention["heads"])))
                for name in ATTENTION_ARRAYS:
                    out.write(np.ascontiguousarray(attention[name], dtype="<f4").tobytes())
            out.write(np.ascontiguousarray(seat_sets["pool_weight"], dtype="<f4").tobytes())
            out.write(np.ascontiguousarray(seat_sets["pool_bias"], dtype="<f4").tobytes())
            out.write(struct.pack("<I", len(seat_sets["pointers"])))
            for pointer in seat_sets["pointers"]:
                out.write(struct.pack("<II", pointer["first"], pointer["set"]))
                out.write(np.ascontiguousarray(pointer["weight"], dtype="<f4").tobytes())
                out.write(np.ascontiguousarray(pointer["bias"], dtype="<f4").tobytes())
        if tail:
            _write_tail(out, tail, version)


def _floats(out, *arrays) -> None:
    for array in arrays:
        out.write(np.ascontiguousarray(array, dtype="<f4").tobytes())


def _write_tail(out, tail: dict, version: int = AMDL_CAMERA_VERSION) -> None:
    """The version 10 tail (see the format above), the version 11 map tiers and planner section with one (the
    version 13 camera splat in it), and the version 12 heading section with one (version 12 on)."""
    _write_camera_tail(out, tail, version)
    if version < AMDL_HEADING_VERSION:
        if tail.get("heading"):
            raise ValueError("a policy with a heading head is written as version 12 or above, never 11 or below")
        return
    heading = tail.get("heading")
    out.write(struct.pack("<B", 1 if heading else 0))
    if heading:
        out.write(struct.pack("<2I", int(heading["components"]), int(heading["width"])))
        out.write(struct.pack("<3f", float(heading["kappa_min"]), float(heading["kappa_max"]),
                              float(heading["weight_floor"])))
        _floats(out, heading["weight"], heading["bias"])


def _write_camera_tail(out, tail: dict, version: int) -> None:
    """The version 10 tail and the version 11 sections in it (_write_tail)."""
    out.write(struct.pack("<B", 1 if tail["goal_space"] else 0))
    pointer = tail.get("place_pointer")
    out.write(struct.pack("<B", 1 if pointer else 0))
    if pointer:
        out.write(struct.pack("<5I", *(int(pointer[key]) for key in ("at", "first", "count", "features", "hidden"))))
        _floats(out, *(pointer[key] for key in ("w1", "b1", "w2", "b2")))
    cells = tail.get("cells")
    out.write(struct.pack("<B", 1 if cells else 0))
    if cells:
        out.write(struct.pack("<9I", *(int(cells[key]) for key in CELL_HEADER)))
        _floats(out, *(cells[key] for key in CELL_ARRAYS))
    camera = tail.get("camera")
    out.write(struct.pack("<B", 1 if camera else 0))
    if not camera:
        if version >= AMDL_PLANNER_VERSION:
            out.write(struct.pack("<B", 0))     # no camera: no planner
        return
    out.write(struct.pack("<6I", *(int(camera[key]) for key in ("height", "width", "patch", "scalars", "first",
                                                                 "crop_bytes"))))
    out.write(struct.pack("<I", len(camera["pixel_classes"])))
    out.write(struct.pack(f"<{len(camera['pixel_classes'])}I", *camera["pixel_classes"]))
    out.write(struct.pack("<II", *camera["class_embed"].shape))
    _floats(out, camera["class_embed"])
    out.write(struct.pack("<3I", camera["patch_weight"].shape[0], camera["mix_weight"].shape[0],
                          camera["embed_weight"].shape[0]))
    _floats(out, *(camera[key] for key in ("patch_weight", "patch_bias", "mix_weight", "mix_bias", "embed_weight",
                                           "embed_bias")))
    entities = camera.get("entities")
    out.write(struct.pack("<B", 1 if entities else 0))
    if entities:
        out.write(struct.pack("<6I", *(int(entities[key]) for key in ("first", "slots", "width", "present",
                                                                       "class_column", "type_column"))))
        out.write(struct.pack("<2i", int(entities["object_column"]), int(entities["memory_column"])))
        out.write(struct.pack("<3I", *entities["type_embed"].shape, entities["w2"].shape[0]))
        _floats(out, *(entities[key] for key in ("type_embed", "w1", "b1", "w2", "b2", "pool_weight", "pool_bias")))
    crop = camera.get("map")
    out.write(struct.pack("<B", 1 if crop else 0))
    if crop:
        out.write(struct.pack("<7I", *(int(crop[key]) for key in ("first", "scalars", "height", "width", "patch")),
                              *crop["code_embed"].shape))
        out.write(struct.pack("<3I", crop["patch_weight"].shape[0], crop["mix_weight"].shape[0],
                              crop["embed_weight"].shape[0]))
        tiers = int(crop.get("tiers", 1))
        if version >= AMDL_PLANNER_VERSION:
            out.write(struct.pack("<I", tiers))
        elif tiers > 1:
            raise ValueError("a two-tier map is written as version 11, never 10")
        _floats(out, *(crop[key] for key in MAP_ARRAYS))
        if tiers > 1:
            _floats(out, crop["tier_embed"])
    _floats(out, camera["join_weight"], camera["join_bias"])
    heads = camera["look_heads"]
    out.write(struct.pack("<I", len(heads)))
    out.write(struct.pack(f"<{len(heads)}I", *heads))
    _floats(out, camera["look_weight"], camera["look_bias"])
    if version < AMDL_PLANNER_VERSION:
        return
    planner = tail.get("planner")
    out.write(struct.pack("<B", 1 if planner else 0))
    if not planner:
        return
    out.write(struct.pack("<13I", *(int(planner[key]) & 0xFFFFFFFF for key in PLANNER_HEADER)))
    _floats(out, np.asarray(planner["tier_cells"], dtype="<f4"), np.asarray([planner["compass_scale"]], dtype="<f4"))
    splat = planner.get("splat")
    if version >= AMDL_SPLAT_VERSION:
        out.write(struct.pack("<I", int(splat["channels"]) if splat else 0))
        if splat:
            out.write(struct.pack("<6I", *(int(splat[key]) for key in SPLAT_HEADER)))
            out.write(struct.pack("<5f", *(float(splat[key]) for key in SPLAT_FLOATS)))
            _floats(out, splat["weight"], splat["bias"])
    elif splat:
        raise ValueError("a policy with the camera splat is written as version 13, never 12 or below")
    _floats(out, *(planner[key] for key in PLANNER_ARRAYS))
    waypoint = planner.get("waypoint")
    if version >= AMDL_VERSION:
        out.write(struct.pack("<I", int(waypoint["features"]) if waypoint else 0))
        if waypoint:
            out.write(struct.pack(f"<{1 + len(waypoint['steps'])}I", int(waypoint["anchor"]),
                                  *(int(step) for step in waypoint["steps"])))
            _floats(out, waypoint["weight"], waypoint["bias"])
    elif waypoint:
        raise ValueError("a policy with the planned waypoint is written as version 14, never 13 or below")


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
    goals["kind_scale"] = (array("goal_embedding.kind_scale.weight") if "goal_embedding.kind_scale.weight" in actor_state
                           else np.zeros_like(goals["kind_embedding"]))
    if "target_embedding" in goals:
        goals["target_scale"] = (array("goal_embedding.target_scale.weight")
                                 if "goal_embedding.target_scale.weight" in actor_state
                                 else np.zeros_like(goals["target_embedding"]))
    return goals


#: The attention layer's arrays, in the order they are written (.amdl 9) and their EntitySets keys.
ATTENTION_ARRAYS = ("type_embed", "own_token", "norm_attend_weight", "norm_attend_bias", "in_proj_weight",
                    "in_proj_bias", "out_proj_weight", "out_proj_bias", "norm_mix_weight", "norm_mix_bias",
                    "mix_in_weight", "mix_in_bias", "mix_out_weight", "mix_out_bias")
_ATTENTION_KEYS = {"norm_attend_weight": "norm_attend.weight", "norm_attend_bias": "norm_attend.bias",
                   "in_proj_weight": "attend.in_proj_weight", "in_proj_bias": "attend.in_proj_bias",
                   "out_proj_weight": "attend.out_proj.weight", "out_proj_bias": "attend.out_proj.bias",
                   "norm_mix_weight": "norm_mix.weight", "norm_mix_bias": "norm_mix.bias",
                   "mix_in_weight": "mix_in.weight", "mix_in_bias": "mix_in.bias",
                   "mix_out_weight": "mix_out.weight", "mix_out_bias": "mix_out.bias"}


def seat_set_weights(actor_state: dict[str, torch.Tensor], stage: dict, names: list[str], layout: int) -> dict | None:
    """One seat layout's sets (EntitySets) as its model carries them: its own sets' encoders, the pool's columns for
    those sets alone (a set the layout lacks pools to zero), and its pointers. None for an actor without seat sets or a
    layout without any. `names` is every layout's name, in the actor's order (the set order is the network's)."""
    if "entity_sets.pool.weight" not in actor_state:
        return None
    from .mappo.networks import SEAT_SET_NAMES
    descriptors = [list(stage.get("layouts", {}).get(name, {}).get("sets", ())) for name in names]
    own = descriptors[layout]
    if not own:
        return None
    present_names = {entry["name"] for sets in descriptors for entry in sets}
    order = [name for name in SEAT_SET_NAMES if name in present_names] + sorted(present_names - set(SEAT_SET_NAMES))

    def array(key: str) -> np.ndarray:
        return actor_state[f"entity_sets.{key}"].detach().cpu().numpy().astype("<f4")

    embed = int(actor_state[f"entity_sets.encoders.{order[0]}.0.weight"].shape[0])
    pool = array("pool.weight")
    sets, columns, pointers = [], [], []
    for entry in own:
        name = entry["name"]
        position = order.index(name)
        columns.extend(range(2 * embed * position, 2 * embed * (position + 1)))
        segments = [(int(part["first"]), int(part["stride"])) for part in entry["segments"]]
        sets.append({"slots": int(entry["slots"]), "width": sum(stride for _, stride in segments),
                     "present": segments[0][0] + int(entry["present"]), "present_stride": segments[0][1],
                     "segments": segments,
                     "w1": array(f"encoders.{name}.0.weight"), "b1": array(f"encoders.{name}.0.bias"),
                     "w2": array(f"encoders.{name}.2.weight"), "b2": array(f"encoders.{name}.2.bias")})
        for ordinal, part in enumerate(entry.get("pointers", ())):
            kind = f"{name}_{ordinal}"
            pointers.append({"first": int(part["first"]), "set": len(sets) - 1,
                             "weight": array(f"queries.{kind}.weight"), "bias": array(f"queries.{kind}.bias")})
    attention = None
    if "entity_sets.type_embed" in actor_state:
        # The layer, this layout's own token, its sets' type embeddings, and the own token's pool columns (the last).
        from .mappo.networks import EntitySets
        columns.extend(range(2 * embed * len(order), 2 * embed * len(order) + embed))
        type_embed = array("type_embed")
        attention = {"heads": EntitySets.HEADS,
                     "type_embed": np.stack([type_embed[order.index(entry["name"])] for entry in own]),
                     "own_token": array("self_token")[layout],
                     **{name: array(key) for name, key in _ATTENTION_KEYS.items()}}
    return {"embed": embed, "sets": sets, "pool_weight": np.ascontiguousarray(pool[:, columns]),
            "pool_bias": array("pool.bias"), "pointers": pointers, "attention": attention}


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


#: The cell pointer's header and arrays in the order the tail writes them (version 10).
CELL_HEADER = ("crop_offset", "grid", "pool", "joint", "rise", "min_floor", "features", "hidden", "chain_hidden")
CELL_ARRAYS = ("local_weight", "local_bias", "query_weight", "query_bias", "score_weight", "score_bias",
               "chain1_weight", "chain1_bias", "chain2_weight", "chain2_bias")
_CELL_KEYS = {"local_weight": "local.weight", "local_bias": "local.bias", "query_weight": "query.weight",
              "query_bias": "query.bias", "score_weight": "score.weight", "score_bias": "score.bias",
              "chain1_weight": "chain.0.weight", "chain1_bias": "chain.0.bias", "chain2_weight": "chain.2.weight",
              "chain2_bias": "chain.2.bias"}
#: The planner section's header (version 11) and arrays in the order the tail writes them, with their actor keys.
PLANNER_HEADER = ("tiers", "classes", "vision", "hidden", "k", "channels", "planes", "readout", "readout_first",
                  "compass_present", "compass_sin", "compass_cos", "compass_dist")
PLANNER_ARRAYS = ("vision_weight", "vision_bias", "tier_bias", "conv1_weight", "conv1_bias", "conv2_weight",
                  "conv2_bias", "out_weight", "out_bias", "fixed_weight", "fixed_bias", "recur_weight", "join_weight",
                  "join_bias", "pointer_weight")
_PLANNER_KEYS = {"vision_weight": "costmap.vision.weight", "vision_bias": "costmap.vision.bias",
                 "tier_bias": "costmap.tier_bias", "conv1_weight": "costmap.conv1.weight",
                 "conv1_bias": "costmap.conv1.bias", "conv2_weight": "costmap.conv2.weight",
                 "conv2_bias": "costmap.conv2.bias", "out_weight": "costmap.out.weight", "out_bias": "costmap.out.bias",
                 "fixed_weight": "planner.fixed.weight", "fixed_bias": "planner.fixed.bias",
                 "recur_weight": "planner.recur.weight", "join_weight": "planner_join.weight",
                 "join_bias": "planner_join.bias"}
#: The camera splat's header in the planner section (version 13; networks.CameraSplat), after its channel count: the
#: sample stride and the vision scalars read, then its floats.
SPLAT_HEADER = ("stride", "scalar_yaw_sin", "scalar_yaw_cos", "scalar_pitch", "scalar_boom", "scalar_render_width")
SPLAT_FLOATS = ("fov_h", "fov_v", "near", "distance_reference", "zoom_scale")
#: The map encoder's arrays in the order the tail writes them, and their MapEncoder keys.
MAP_ARRAYS = ("code_embed", "patch_weight", "patch_bias", "mix_weight", "mix_bias", "embed_weight", "embed_bias",
              "join_weight", "join_bias")
_MAP_KEYS = {"code_embed": "code_embed.weight", "patch_weight": "patch.weight", "patch_bias": "patch.bias",
             "mix_weight": "mix.weight", "mix_bias": "mix.bias", "embed_weight": "embed.weight",
             "embed_bias": "embed.bias", "join_weight": "join.weight", "join_bias": "join.bias"}


def tail_weights(actor_state: dict[str, torch.Tensor], stage: dict, names: list[str], layout: int) -> dict | None:
    """The version 10 tail of one layout's model -- the goal head's place pointer and cell pointer, the camera
    (VisionEncoder with its map and entity list) and its free look -- or None when the policy has none of them and
    reads no goal block of revision 2 on with two goals (it is written as version 9). Refused (ValueError): camera parts the realm format has no section for (the coarse coverage
    grid, the sight list and its pointer presses), and a camera without the stage.json that describes it."""
    def array(key: str, dtype: str = "<f4") -> np.ndarray:
        return actor_state[key].detach().cpu().numpy().astype(dtype)

    from .mappo.networks import (CELL_CHAIN_HIDDEN, CELL_FEATURES, CELL_HIDDEN, CELL_MIN_FLOOR, CELL_RISE_UNITS,
                                 IMAGE_BYTES_PER_PIXEL, PLACE_FEATURES, POINTER_HIDDEN, vision_of)
    tail: dict = {"goal_space": bool("goal_head.block_at" in actor_state
                                      and (actor_state["goal_head.block_at"] >= 0).any())}
    goals = (stage or {}).get("goals") or {}
    # A goal block of revision 2 on has no order columns: a reader of 9 would take what was achieved for the director's
    # order, so a policy with two goals and a queue that reads one is written as 10 (the learner's current rules).
    entry = ((stage or {}).get("layouts") or {}).get(names[layout]) or {}
    revision = next((int(block.get("revision") or 0) for block in entry.get("blocks", ())
                     if block.get("name") == goals.get("block", "goal")), 0)
    needed = revision >= 2 and "goal_head.slot_bias" in actor_state
    if "goal_head.place_spec" in actor_state:
        at, first, count, features = (int(v) for v in actor_state["goal_head.place_spec"].tolist())
        if at >= 0 and count > 0:
            if features != PLACE_FEATURES:
                raise ValueError(f"the place pointer reads {features} features a slot, the learner {PLACE_FEATURES}")
            tail["place_pointer"] = {"at": at, "first": first, "count": count, "features": features,
                                     "hidden": POINTER_HIDDEN,
                                     "w1": array("goal_head.pointer.0.weight"), "b1": array("goal_head.pointer.0.bias"),
                                     "w2": array("goal_head.pointer.2.weight"), "b2": array("goal_head.pointer.2.bias")}
            needed = True

    vision = vision_of(stage, names) if stage else None
    if vision is None and any(key.startswith(("vision.", "look_head.")) for key in actor_state):
        raise ValueError("the actor has a camera (vision.* weights) but no stage.json describes it: not exporting")
    own = vision[layout] if vision is not None else None

    if "goal_head.cell_spec" in actor_state:
        at, grid, pool, joint = (int(v) for v in actor_state["goal_head.cell_spec"].tolist())
        slots = int(actor_state["goal_head.slot_bias"].shape[0]) + 1 if "goal_head.slot_bias" in actor_state else 1
        if at >= 0 and joint >= 0 and grid > 0 and pool > 0 and slots > 1:
            if own is None or own.get("map") is None:
                raise ValueError("the goal head points at the map's cells, but this layout has no mental map")
            offset = at - own["image_bytes"]
            if offset < 0 or offset + grid * pool * grid * pool * 7 > own["map"]["map_bytes"]:
                raise ValueError(f"the cell pointer's crop at image byte {at} is not inside the map's crop")
            described = goals.get("cells") or {}
            tail["cells"] = {"crop_offset": offset, "grid": grid, "pool": pool, "joint": joint,
                             # 0 (the opened grid) is written as the feature scale the learner used (rise_scale).
                             "rise": int(described.get("rise_units", CELL_RISE_UNITS)) or CELL_RISE_UNITS,
                             "min_floor": int(described.get("min_floor_cells", CELL_MIN_FLOOR)),
                             "features": CELL_FEATURES, "hidden": CELL_HIDDEN, "chain_hidden": CELL_CHAIN_HIDDEN,
                             **{name: array(f"goal_head.cell.{key}") for name, key in _CELL_KEYS.items()}}
            needed = True

    if own is not None:
        if any(key.startswith(("vision.coverage.", "vision.sight.", "sight_pointers.")) for key in actor_state) \
                or own.get("coverage") is not None or own.get("sight") is not None:
            raise ValueError("the camera's coverage grid and sight list have no .amdl section: not exporting")
        if own["bytes_per_pixel"] != IMAGE_BYTES_PER_PIXEL:
            raise ValueError(f"a camera of {own['bytes_per_pixel']} bytes a pixel")
        camera = {"height": own["height"], "width": own["width"], "patch": own["patch"], "scalars": own["scalars"],
                  "first": own["first"], "crop_bytes": own["map"]["map_bytes"] if own.get("map") else 0,
                  "pixel_classes": [int(c) for c in own["pixel_classes"]],
                  "class_embed": array("vision.class_embed.weight"),
                  "patch_weight": array("vision.patch.weight"), "patch_bias": array("vision.patch.bias"),
                  "mix_weight": array("vision.mix.weight"), "mix_bias": array("vision.mix.bias"),
                  "embed_weight": array("vision.embed.weight"), "embed_bias": array("vision.embed.bias"),
                  "join_weight": array("vision_join.linear.weight"), "join_bias": array("vision_join.linear.bias")}
        listed = own.get("entities")
        if listed is not None and "vision.entities.pool.weight" in actor_state:
            camera["entities"] = {
                "first": listed["first"], "slots": listed["slots"], "width": listed["width"],
                "present": listed["present"], "class_column": listed["class_column"],
                "type_column": listed["type_column"], "object_column": listed["object_column"],
                "memory_column": listed["memory_column"],
                "type_embed": array("vision.entities.type_embed.weight"),
                "w1": array("vision.entities.encoders.visible.0.weight"),
                "b1": array("vision.entities.encoders.visible.0.bias"),
                "w2": array("vision.entities.encoders.visible.2.weight"),
                "b2": array("vision.entities.encoders.visible.2.bias"),
                "pool_weight": array("vision.entities.pool.weight"), "pool_bias": array("vision.entities.pool.bias")}
            inputs = listed["width"] - 2 + camera["class_embed"].shape[1] + camera["entities"]["type_embed"].shape[1]
            if camera["entities"]["w1"].shape[1] != inputs:
                raise ValueError(f"the entity encoder takes {camera['entities']['w1'].shape[1]} inputs, expected "
                                 f"{inputs}")
        crop = own.get("map")
        if crop is not None and "vision.map.patch.weight" in actor_state:
            from .mappo.networks import (IMAGE_NEAR, VISION_SCALAR_BOOM, VISION_SCALAR_PITCH,
                                         VISION_SCALAR_RENDER_WIDTH, VISION_SCALAR_YAW_COS, VISION_SCALAR_YAW_SIN,
                                         VISION_ZOOM_SCALE, CameraSplat, CostmapHead, MapEncoder, Planner)
            tiers = int(crop.get("tiers", 1))
            camera["map"] = {"first": crop["first"], "scalars": crop["scalars"], "height": crop["height"],
                             "width": crop["width"], "patch": MapEncoder.PATCH, "tiers": tiers,
                             **{name: array(f"vision.map.{key}") for name, key in _MAP_KEYS.items()}}
            if tiers > 1:
                camera["map"]["tier_embed"] = array("vision.map.tier_embed")
            if "vision.map.planner.fixed.weight" in actor_state:
                # The planner (version 11): refused without the two-tier map it plans over.
                if tiers < 2 or "vision.map.tier_embed" not in actor_state:
                    raise ValueError("the actor has a planner (vision.map.planner.*) but this layout's map has one "
                                     "tier: not exporting")
                compass = own.get("compass") or {}
                # K from the checkpoint's buffer (mappo.planner_k); a checkpoint from before it ran the old 40.
                k = int(actor_state["vision.map.planner.k"]) if "vision.map.planner.k" in actor_state else 40
                # The camera splat (version 13): its planes are conv1's last input channels.
                splat = None
                if "vision.map.splat.project.weight" in actor_state:
                    fov_h, fov_v = own.get("fov", (120.0, 60.0))
                    splat = {"channels": CameraSplat.CHANNELS, "stride": CameraSplat.STRIDE,
                             "scalar_yaw_sin": VISION_SCALAR_YAW_SIN, "scalar_yaw_cos": VISION_SCALAR_YAW_COS,
                             "scalar_pitch": VISION_SCALAR_PITCH, "scalar_boom": VISION_SCALAR_BOOM,
                             "scalar_render_width": VISION_SCALAR_RENDER_WIDTH, "fov_h": float(fov_h),
                             "fov_v": float(fov_v), "near": IMAGE_NEAR,
                             "distance_reference": float(own.get("distance_reference", 1000.0)),
                             "zoom_scale": VISION_ZOOM_SCALE,
                             "weight": array("vision.map.splat.project.weight"),
                             "bias": array("vision.map.splat.project.bias")}
                    if splat["weight"].shape[0] != CameraSplat.CHANNELS:
                        raise ValueError(f"the camera splat projects to {splat['weight'].shape[0]} channels, the "
                                         f"learner's {CameraSplat.CHANNELS}")
                splat_planes = splat["channels"] + 1 if splat else 0
                tail["planner"] = {
                    "tiers": tiers, "classes": int(actor_state["vision.map.costmap.out.weight"].shape[0]),
                    "vision": CostmapHead.VISION, "hidden": CostmapHead.HIDDEN, "k": k,
                    "channels": Planner.CHANNELS,
                    "planes": (int(actor_state["vision.map.costmap.conv1.weight"].shape[1]) - CostmapHead.VISION
                               - splat_planes),
                    "splat": splat,
                    "readout": MapEncoder.READOUT, "readout_first": MapEncoder.READOUT_FIRST,
                    "compass_present": int(compass.get("present", -1)), "compass_sin": int(compass.get("sin", -1)),
                    "compass_cos": int(compass.get("cos", -1)), "compass_dist": int(compass.get("dist", -1)),
                    "tier_cells": [float(cell) for cell in crop.get("tier_cells", ())],
                    "compass_scale": float(compass.get("scale", 500.0)),
                    **{name: array(f"vision.map.{key}") for name, key in _PLANNER_KEYS.items()},
                    "pointer_weight": (array("goal_head.cell.planner_weight")
                                       if "goal_head.cell.planner_weight" in actor_state
                                       else np.zeros((1,), "<f4"))}
                # The planned waypoint (version 14): its walk and its join.
                if "vision.map.waypoint_join.weight" in actor_state:
                    weight = array("vision.map.waypoint_join.weight")
                    if weight.shape[1] != tiers * MapEncoder.WAYPOINT_FEATURES:
                        raise ValueError(f"the waypoint join reads {weight.shape[1]} inputs, the learner's "
                                         f"{tiers} x {MapEncoder.WAYPOINT_FEATURES}")
                    tail["planner"]["waypoint"] = {
                        "features": tiers * MapEncoder.WAYPOINT_FEATURES, "anchor": MapEncoder.WAYPOINT_ANCHOR,
                        "steps": [MapEncoder.WAYPOINT_STEPS[min(t, len(MapEncoder.WAYPOINT_STEPS) - 1)]
                                  for t in range(tiers)],
                        "weight": weight, "bias": array("vision.map.waypoint_join.bias")}
        if "look_head.linear.weight" in actor_state:
            camera["look_heads"] = [int(h) for h in own["look"]]
            camera["look_weight"] = array("look_head.linear.weight")
            camera["look_bias"] = array("look_head.linear.bias")
            if sum(camera["look_heads"]) != camera["look_weight"].shape[0]:
                raise ValueError(f"the look head has {camera['look_weight'].shape[0]} outputs, the manifest's heads "
                                 f"{camera['look_heads']}")
        else:
            camera["look_heads"], camera["look_weight"], camera["look_bias"] = [], np.zeros((0,), "<f4"), \
                np.zeros((0,), "<f4")
        tail["camera"] = camera
        needed = True
    heading = heading_weights(actor_state, stage, names, layout)
    if heading is not None:
        tail["heading"] = heading
        needed = True
    return tail if needed else None


def heading_weights(actor_state: dict[str, torch.Tensor], stage: dict, names: list[str], layout: int) -> dict | None:
    """The version 12 heading section of one layout's model (HeadingHead), or None when the actor has no heading head
    or this layout has no move block (networks.heading_of). Refused (ValueError): heading weights without the
    stage.json "continuous" list that turns them on."""
    if "heading_head.linear.weight" not in actor_state:
        return None
    from .mappo.networks import (HEADING_COMPONENTS, HEADING_KAPPA_MAX, HEADING_KAPPA_MIN, HEADING_WEIGHT_FLOOR,
                                 heading_of)
    steering = heading_of(stage, names) if stage else None
    if steering is None:
        raise ValueError("the actor has a heading head (heading_head.* weights) but no stage.json names the heading "
                         "among its continuous outputs: not exporting")
    if not steering[layout]:
        return None
    weight = actor_state["heading_head.linear.weight"].detach().cpu().numpy().astype("<f4")
    bias = actor_state["heading_head.linear.bias"].detach().cpu().numpy().astype("<f4")
    if weight.shape[0] != 4 * HEADING_COMPONENTS:
        raise ValueError(f"the heading head has {weight.shape[0]} outputs, the learner's {HEADING_COMPONENTS} "
                         f"components make {4 * HEADING_COMPONENTS}")
    return {"components": HEADING_COMPONENTS, "width": int(weight.shape[1]), "kappa_min": HEADING_KAPPA_MIN,
            "kappa_max": HEADING_KAPPA_MAX, "weight_floor": HEADING_WEIGHT_FLOOR, "weight": weight, "bias": bias}


def export_layouts(
    actor_state: dict[str, torch.Tensor], spec: dict, out_dir: str | Path, manifest_dir: str | Path | None = None,
    goal_every: int = 0, only: list[str] | None = None, stage: dict | None = None
) -> list[Path]:
    """Write every layout's model to out_dir/<model name>.amdl, each atomically; returns the files written.

    manifest_dir is the stage's layouts directory (the sim writes <OutputDir>/layouts/<scenario>/): its stage.json
    names the models, and each <model name>.json layout manifest there is copied beside its model, so whoever loads the
    model can check it reads the same layout.

    A policy with a camera, a place pointer or a cell head is written as version 10 (tail_weights), every other as 9.
    `only` names the layouts to write (all by default); `stage` is the stage.json to read when the manifest directory
    has none (a checkpoint carries its own).
    """
    out_dir = Path(out_dir)
    # The actor's shared movement (mappo.shared_blocks) written into every layout's own adapter and head: the same
    # map, in the format every reader already has.
    from .mappo.networks import materialised_state
    actor_state = materialised_state(actor_state, drop=True)
    manifests = Path(manifest_dir) if manifest_dir is not None else Path("layouts") / spec["scenario"]
    stage_path = manifests / STAGE_FILE
    if stage_path.is_file():
        stage = json.loads(stage_path.read_text())
    stage = stage or {}
    models = model_names(stage) if stage else {}
    layouts = spec["layouts"]
    written = []
    names = [entry["name"] for entry in layouts]
    for index, layout in enumerate(layouts):
        if only is not None and layout["name"] not in only:
            continue
        name = model_name(spec["scenario"], layout["name"], len(layouts), models)
        layers = with_agent_column(layout_layers(actor_state, index))
        memory = memory_weights(actor_state)
        goals = goal_weights(actor_state, index)
        target = out_dir / f"{name}.amdl"
        partial = out_dir / f".{target.name}.partial"
        try:
            seats = seat_set_weights(actor_state, stage, names, index)
            tail = tail_weights(actor_state, stage, names, index)
            write_amdl(partial, name, layout["obs_dim"], 1, layout["num_actions"], layers, memory, goals,
                       goal_every, feedback_weights(actor_state), slow_weights(actor_state), seats, tail)
            os.replace(partial, target)
        except BaseException:
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
    if version not in AMDL_VERSIONS:
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
        goals["kind_scale"] = floats(kinds * width, kinds, width)
        if targets > 1:
            goals["target_scale"] = floats(targets * width, targets, width)

    (has_sets,) = struct.unpack_from("<B", data, offset)
    offset += 1
    if has_sets:
        raise ValueError(f"{path}: a director's set section (no longer written or read here)")

    seat_sets = None
    if version >= 8:
        (has_seat_sets,) = struct.unpack_from("<B", data, offset)
        offset += 1
        if has_seat_sets:
            embed, count = struct.unpack_from("<II", data, offset)
            offset += 8
            attending = False
            if version >= 9:
                (attending,) = struct.unpack_from("<B", data, offset)
                offset += 1
            seat_sets = {"embed": embed, "sets": [], "pointers": [], "attention": None}
            for _ in range(count):
                slots, per, present, stride, segment_count = struct.unpack_from("<IIIII", data, offset)
                offset += 20
                segments = []
                for _ in range(segment_count):
                    segments.append(struct.unpack_from("<II", data, offset))
                    offset += 8
                seat_sets["sets"].append({"slots": slots, "width": per, "present": present, "present_stride": stride,
                                          "segments": segments, "w1": floats(embed * per, embed, per),
                                          "b1": floats(embed, embed), "w2": floats(embed * embed, embed, embed),
                                          "b2": floats(embed, embed)})
            if attending:
                (heads,) = struct.unpack_from("<I", data, offset)
                offset += 4
                shapes = {"type_embed": (count, embed), "own_token": (embed,), "in_proj_weight": (3 * embed, embed),
                          "in_proj_bias": (3 * embed,), "out_proj_weight": (embed, embed),
                          "mix_in_weight": (2 * embed, embed), "mix_in_bias": (2 * embed,),
                          "mix_out_weight": (embed, 2 * embed)}
                seat_sets["attention"] = {"heads": heads}
                for name in ATTENTION_ARRAYS:
                    shape = shapes.get(name, (embed,))
                    seat_sets["attention"][name] = floats(int(np.prod(shape)), *shape)
            adapter_out = layers[0][0].shape[0]
            pooled_width = 2 * embed * count + (embed if attending else 0)
            seat_sets["pool_weight"] = floats(adapter_out * pooled_width, adapter_out, pooled_width)
            seat_sets["pool_bias"] = floats(adapter_out, adapter_out)
            (pointer_count,) = struct.unpack_from("<I", data, offset)
            offset += 4
            for _ in range(pointer_count):
                first, index = struct.unpack_from("<II", data, offset)
                offset += 8
                seat_sets["pointers"].append({"first": first, "set": index,
                                              "weight": floats(embed * width, embed, width),
                                              "bias": floats(embed, embed)})

    tail = None
    if version >= 10:
        def u32s(count: int) -> tuple[int, ...]:
            nonlocal offset
            values = struct.unpack_from(f"<{count}I", data, offset)
            offset += 4 * count
            return values

        def flag() -> bool:
            nonlocal offset
            offset += 1
            return bool(data[offset - 1])

        tail = {"goal_space": flag()}
        if flag():
            at, first, count, features, hidden = u32s(5)
            tail["place_pointer"] = {"at": at, "first": first, "count": count, "features": features, "hidden": hidden,
                                     "w1": floats(hidden * features, hidden, features), "b1": floats(hidden, hidden),
                                     "w2": floats(hidden, 1, hidden), "b2": floats(1, 1)}
        if flag():
            cells = dict(zip(CELL_HEADER, u32s(len(CELL_HEADER))))
            hidden, chain, goal_width = cells["hidden"], cells["chain_hidden"], (slow_size or width)
            shapes = {"local_weight": (hidden, cells["features"]), "local_bias": (hidden,),
                      "query_weight": (hidden, goal_width), "query_bias": (hidden,), "score_weight": (1, hidden),
                      "score_bias": (1,), "chain1_weight": (chain, 3), "chain1_bias": (chain,),
                      "chain2_weight": (1, chain), "chain2_bias": (1,)}
            for key in CELL_ARRAYS:
                cells[key] = floats(int(np.prod(shapes[key])), *shapes[key])
            tail["cells"] = cells
        if flag():
            camera = dict(zip(("height", "width", "patch", "scalars", "first", "crop_bytes"), u32s(6)))
            (count,) = u32s(1)
            camera["pixel_classes"] = list(u32s(count))
            limit, class_width = u32s(2)
            camera["class_embed"] = floats(limit * class_width, limit, class_width)
            patch_out, mix_out, embed = u32s(3)
            per_patch = camera["patch"] ** 2 * (4 + count)
            camera.update(patch_weight=floats(patch_out * per_patch, patch_out, per_patch),
                          patch_bias=floats(patch_out, patch_out), mix_weight=floats(mix_out * patch_out, mix_out,
                                                                                     patch_out),
                          mix_bias=floats(mix_out, mix_out))
            inputs = 2 * mix_out + camera["scalars"]
            camera.update(embed_weight=floats(embed * inputs, embed, inputs), embed_bias=floats(embed, embed))
            if flag():
                entities = dict(zip(("first", "slots", "width", "present", "class_column", "type_column"), u32s(6)))
                entities["object_column"], entities["memory_column"] = struct.unpack_from("<2i", data, offset)
                offset += 8
                buckets, type_width, token = u32s(3)
                inputs = entities["width"] - 2 + class_width + type_width
                entities.update(type_embed=floats(buckets * type_width, buckets, type_width),
                                w1=floats(token * inputs, token, inputs), b1=floats(token, token),
                                w2=floats(token * token, token, token), b2=floats(token, token),
                                pool_weight=floats(embed * 2 * token, embed, 2 * token), pool_bias=floats(embed, embed))
                camera["entities"] = entities
            if flag():
                crop = dict(zip(("first", "scalars", "height", "width", "patch", "codes", "code_width"), u32s(7)))
                map_patch, map_mix, map_embed = u32s(3)
                crop["tiers"] = u32s(1)[0] if version >= 11 else 1
                per_patch = crop["patch"] ** 2 * (crop["code_width"] + class_width + 6)
                inputs = crop["tiers"] * 3 * map_mix + crop["scalars"]
                crop.update(code_embed=floats(crop["codes"] * crop["code_width"], crop["codes"], crop["code_width"]),
                            patch_weight=floats(map_patch * per_patch, map_patch, per_patch),
                            patch_bias=floats(map_patch, map_patch),
                            mix_weight=floats(map_mix * map_patch, map_mix, map_patch),
                            mix_bias=floats(map_mix, map_mix),
                            embed_weight=floats(map_embed * inputs, map_embed, inputs),
                            embed_bias=floats(map_embed, map_embed),
                            join_weight=floats(embed * map_embed, embed, map_embed), join_bias=floats(embed, embed))
                if crop["tiers"] > 1:
                    crop["tier_embed"] = floats(crop["tiers"] * map_mix, crop["tiers"], map_mix)
                camera["map"] = crop
            adapter_out = layers[0][0].shape[0]
            camera["join_weight"] = floats(adapter_out * embed, adapter_out, embed)
            camera["join_bias"] = floats(adapter_out, adapter_out)
            (heads,) = u32s(1)
            camera["look_heads"] = list(u32s(heads))
            total = sum(camera["look_heads"])
            camera["look_weight"] = floats(total * width, total, width)
            camera["look_bias"] = floats(total, total)
            tail["camera"] = camera
        if version >= 11 and flag():
            planner = dict(zip(PLANNER_HEADER, u32s(len(PLANNER_HEADER))))
            for key in ("compass_present", "compass_sin", "compass_cos", "compass_dist"):
                planner[key] = -1 if planner[key] == 0xFFFFFFFF else planner[key]
            tiers, classes, vision_width = planner["tiers"], planner["classes"], planner["vision"]
            hidden, channels, planes = planner["hidden"], planner["channels"], planner["planes"]
            planner["tier_cells"] = floats(tiers, tiers).tolist()
            planner["compass_scale"] = float(floats(1, 1)[0])
            embed = tail["camera"]["embed_weight"].shape[0]
            planner["splat"] = None
            splat_planes = 0
            if version >= 13:
                (channels_splat,) = u32s(1)
                if channels_splat:
                    splat = {"channels": channels_splat, **dict(zip(SPLAT_HEADER, u32s(len(SPLAT_HEADER))))}
                    splat.update(zip(SPLAT_FLOATS, struct.unpack_from(f"<{len(SPLAT_FLOATS)}f", data, offset)))
                    offset += 4 * len(SPLAT_FLOATS)
                    mix_out = tail["camera"]["mix_weight"].shape[0]
                    splat["weight"] = floats(channels_splat * mix_out, channels_splat, mix_out)
                    splat["bias"] = floats(channels_splat, channels_splat)
                    planner["splat"] = splat
                    splat_planes = channels_splat + 1
            shapes = {"vision_weight": (vision_width, embed), "vision_bias": (vision_width,),
                      "tier_bias": (tiers, vision_width),
                      "conv1_weight": (hidden, planes + vision_width + splat_planes, 3, 3),
                      "conv1_bias": (hidden,), "conv2_weight": (hidden, hidden, 3, 3), "conv2_bias": (hidden,),
                      "out_weight": (classes, hidden, 1, 1), "out_bias": (classes,),
                      "fixed_weight": (channels, classes + planes + 1, 3, 3), "fixed_bias": (channels,),
                      "recur_weight": (channels, 1, 3, 3), "join_weight": (embed, tiers * planner["readout"]),
                      "join_bias": (embed,), "pointer_weight": (1,)}
            for key in PLANNER_ARRAYS:
                planner[key] = floats(int(np.prod(shapes[key])), *shapes[key])
            planner["waypoint"] = None
            if version >= 14:
                (features,) = u32s(1)
                if features:
                    anchor, *steps = u32s(1 + tiers)
                    planner["waypoint"] = {"features": features, "anchor": anchor, "steps": list(steps),
                                           "weight": floats(embed * features, embed, features),
                                           "bias": floats(embed, embed)}
            tail["planner"] = planner
        if version >= 12 and flag():
            components, heading_width = u32s(2)
            kappa_min, kappa_max, weight_floor = struct.unpack_from("<3f", data, offset)
            offset += 12
            outputs = 4 * components
            tail["heading"] = {"components": components, "width": heading_width, "kappa_min": kappa_min,
                               "kappa_max": kappa_max, "weight_floor": weight_floor,
                               "weight": floats(outputs * heading_width, outputs, heading_width),
                               "bias": floats(outputs, outputs)}

    if offset != len(data):
        raise ValueError(f"{len(data) - offset} trailing bytes")
    return {
        "version": version,
        "tail": tail,
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
        "seat_sets": seat_sets,
    }


def reference_heading(heading: dict, features: np.ndarray) -> float:
    """The realm's heading from a version 12 model's heading section and the action head's features [feature_width]
    (the goal's conditioning in them): the mode, theta = atan2(y_k, x_k) of the component k with the largest mixture
    logit (the first of equals), radians in (-pi, pi], relative to the body's facing. The reference for the C++ reader;
    animus.golden checks it against the learner's own."""
    k = int(heading["components"])
    raw = heading["weight"].astype(np.float64) @ np.asarray(features, dtype=np.float64) + heading["bias"]
    best = int(np.argmax(raw[3 * k:4 * k]))
    return float(np.arctan2(raw[2 * best + 1], raw[2 * best]))


def reference_decide(model: dict, obs: np.ndarray, mask: np.ndarray, agent: int = 0,
                     state: dict | None = None) -> tuple[int, np.ndarray]:
    """The forward pass of an exported model: returns (greedy allowed action, logits).

    `state` is what the policy carries between decisions -- {"memory": [R], "goal": int, "age": int} -- updated in
    place. A model without a memory or goals ignores it, and so behaves the same however it is called.

    Versions 7 to 9 only: a version 10 model (a camera, the goal head's pointers) is checked against the learner's own
    network instead (animus.golden).
    """
    if model.get("tail"):
        raise ValueError("reference_decide reads versions 7 to 9; check a version 10 model with animus.golden")
    x = np.concatenate([obs.astype(np.float32), np.eye(model["num_agents"], dtype=np.float32)[agent]])
    layers = model["layers"]
    extra = 0.0
    # A seat layout's entities (version 8): each slot gathered from its segments through its set's encoder, pooled.
    seats = model.get("seat_sets")
    seat_codes = []
    if seats is not None:
        presents = []
        for entry in seats["sets"]:
            raw = np.stack([np.concatenate([obs[first + slot * stride : first + (slot + 1) * stride]
                                            for first, stride in entry["segments"]])
                            for slot in range(entry["slots"])]).astype(np.float32)
            seat_codes.append(np.tanh(np.tanh(raw @ entry["w1"].T + entry["b1"]) @ entry["w2"].T + entry["b2"]))
            presents.append(obs[[entry["present"] + slot * entry["present_stride"]
                                 for slot in range(entry["slots"])]] > 0.5)
        own = None
        if seats.get("attention") is not None:
            seat_codes, own = _reference_attention(seats["attention"], seat_codes, presents)
        pooled = []
        for hidden, present in zip(seat_codes, presents):
            pooled.append((hidden * present[:, None]).sum(axis=0) / max(1.0, float(present.sum())))
            pooled.append(np.where(present[:, None], hidden, -1.0).max(axis=0) if present.any()
                          else np.zeros(hidden.shape[1], np.float32))
        if own is not None:
            pooled.append(own)
        extra = extra + seats["pool_weight"] @ np.concatenate(pooled) + seats["pool_bias"]
    for index, (weight, bias) in enumerate(layers[:-1]):
        x = np.tanh(weight @ x + bias + (extra if index == 0 else 0.0))

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
    # A seat layout's slot-naming actions: each slot's encoding against a query from the features.
    for pointer in (seats["pointers"] if seats is not None else ()):
        scores = seat_codes[pointer["set"]] @ (pointer["weight"] @ features + pointer["bias"])
        x[pointer["first"] : pointer["first"] + len(scores)] = scores

    allowed = mask.astype(bool)
    if not allowed.any():
        return 0, x
    return int(np.where(allowed, x, -np.inf).argmax()), x


def _layer_norm(x: np.ndarray, weight: np.ndarray, bias: np.ndarray) -> np.ndarray:
    mean = x.mean(axis=-1, keepdims=True)
    variance = ((x - mean) ** 2).mean(axis=-1, keepdims=True)
    return (x - mean) / np.sqrt(variance + 1e-5) * weight + bias


def _reference_attention(layer: dict, codes: list[np.ndarray], presents: list[np.ndarray]):
    """The seat sets' attention layer (.amdl 9): (each set's attended slots, the attended own token)."""
    embed = layer["own_token"].shape[0]
    heads = int(layer["heads"])
    width = embed // heads
    x = np.concatenate([layer["own_token"][None, :]] + [code + layer["type_embed"][index]
                                                         for index, code in enumerate(codes)]).astype(np.float32)
    keys = np.concatenate([[True]] + presents)
    normed = _layer_norm(x, layer["norm_attend_weight"], layer["norm_attend_bias"])
    q, k, v = np.split(normed @ layer["in_proj_weight"].T + layer["in_proj_bias"], 3, axis=-1)
    merged = np.zeros_like(x)
    for head in range(heads):
        part = slice(head * width, (head + 1) * width)
        scores = q[:, part] @ k[:, part].T / np.sqrt(width)
        scores = np.where(keys[None, :], scores, -np.inf)
        scores = np.exp(scores - scores.max(axis=-1, keepdims=True))
        merged[:, part] = (scores / scores.sum(axis=-1, keepdims=True)) @ v[:, part]
    x = x + merged @ layer["out_proj_weight"].T + layer["out_proj_bias"]
    mixed = np.maximum(_layer_norm(x, layer["norm_mix_weight"], layer["norm_mix_bias"]) @ layer["mix_in_weight"].T
                       + layer["mix_in_bias"], 0.0)
    x = x + mixed @ layer["mix_out_weight"].T + layer["mix_out_bias"]
    out, at = [], 1
    for code in codes:
        out.append(x[at : at + len(code)])
        at += len(code)
    return out, x[0]


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
    ended = secondary_ended = event = False
    if at < 0 and targets > 1:
        allowed[:] = False              # no goal block: only the first goal, which means none
    if at >= 0 and targets > 1:
        width = base + 2 + (2 + base if slots > 1 else 0)
        block = obs[at : at + width] > 0.5
        present = block[:kinds, None] & block[kinds:base][None, :]
        allowed &= present
        ended = bool(block[base])
        if slots > 1:
            secondary_ended, event = bool(block[base + 2]), bool(block[base + 3])
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
            before = [drawn[0]]
            for slot in range(1, slots):
                shifted = source + goals["slot_bias"][slot - 1]
                for previous in before:
                    shifted = shifted + goals["drawn"][previous + 1]
                later = accepts.copy()
                if at >= 0 and targets > 1:
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
        if secondary == primary:
            secondary = -1
        goal = primary * (count + 1) + secondary + 1
    else:
        goal = primary
    if state is not None:
        state["goal"], state["age"], state["queue"] = goal, 1 if choose else age + 1, list(queue)
    def held(table: str) -> np.ndarray:
        one = lambda goal: goals[f"kind_{table}"][goal // targets] + (
            goals[f"target_{table}"][goal % targets] if targets > 1 else 0.0)
        total = one(primary)
        if slots > 1 and secondary >= 0:
            total = total + goals["gate"] * one(secondary)
        return total
    return x * (1.0 + held("scale")) + held("embedding")


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
    for path in export_layouts(checkpoint["trainer"]["actor"], checkpoint["spec"], out, manifests, goal_every,
                               stage=checkpoint.get("stage")):
        print(f"Wrote {path} (update {checkpoint.get('update', '?')})")


if __name__ == "__main__":
    main()
