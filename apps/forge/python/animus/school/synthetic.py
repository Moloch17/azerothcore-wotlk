"""**Synthetic school samples** (the pipeline's smoke; a tool, not a test): random rooms and corridors on a 2-yd world
grid, a body and a goal, the bot's partial view of it as the recorder's crop bytes and the sim's label format, with
the true route cost-to-go planned over the WHOLE world by the oracle's Dijkstra (so a label's route leaves the crop
and runs through cells the crop never saw -- the lesson the school is for). Written as shards the recorder would
write (record.ShardWriter), so the dataset, the model, the trainer and both baselines run end to end without a sim.

    python -m animus.school.synthetic --out /tmp/school-synthetic --episodes 100 --per-episode 4 [--seed 1]

What is synthetic, and so what the numbers do not say: the world is axis-aligned rooms (free), 2-4 cell corridors,
water pools (TRAV water, 1.5 a yard) and hazard patches (5 a yard) inside a wall; the heading is always north (row 0
ahead: no rotated crops); the view is line of sight on the grid within a radius plus, with the memory flag on, whole
rooms remembered; the height byte is flat (128 known, 0 unknown); the sim's route scalars are zero (no sim planned
them). Maps are drawn as 34 (Stockades), 389 (Ragefire) and 43 (Wailing Caverns, the held-out one) with no
difference between them but the id.
"""

from __future__ import annotations

import argparse
import math
import time

import numpy as np
import torch

from .. import protocol
from ..mappo.networks import (CLASSICAL_COSTS, CLASSICAL_ORACLE_UNKNOWN, CLASSICAL_SENTINEL, MAP_AGE, MAP_CODE,
                              MAP_FLOOR, MAP_FRONTIER, MAP_HAZARD, MAP_HEIGHT, MAP_HEIGHT_ZERO, MAP_VISITED,
                              MAP_WALL, MAP_WALLS, MAP_WALLS_MAX, classical_cost_to_go)
from .record import (CROP_BYTES, CROP_SIDE, MAP_CHANNELS, MEMORY_OFF, MEMORY_ON, ROUTE_COLUMNS, ShardWriter,
                     empty_batch)

#: TRAV_CLASS_NAMES indexes.
CLS_UNKNOWN, CLS_FREE, CLS_WALL, CLS_WATER, CLS_HAZARD = 0, 1, 2, 3, 4
WORLD = 128                 # 2-yd cells a side (256 yd)
FINE_CELL, COARSE_CELL = 2.0, 8.0
RATIO = int(COARSE_CELL / FINE_CELL)
ANCHOR = 23                 # the crop's centre four's ahead-left cell (MapEncoder.WAYPOINT_ANCHOR)
MAPS = ((34, 0, 0.4), (389, 1, 0.4), (43, 2, 0.2))   # (map id, arena index, share)
UNREACHABLE_ABOVE = 5000.0  # a world cost above this ran through a wall: unreachable to the label


def make_world(rng: np.random.Generator) -> tuple[np.ndarray, list[tuple[int, int, int, int]]]:
    """A world of classes [WORLD, WORLD] (uint8, TRAV indexes) -- wall everywhere, rooms carved and joined by
    corridors, pools and hazard patches in some rooms -- and its rooms (r0, c0, r1, c1)."""
    world = np.full((WORLD, WORLD), CLS_WALL, dtype=np.uint8)
    rooms: list[tuple[int, int, int, int]] = []
    for _ in range(int(rng.integers(6, 12))):
        h, w = int(rng.integers(6, 20)), int(rng.integers(6, 20))
        r0, c0 = int(rng.integers(2, WORLD - h - 2)), int(rng.integers(2, WORLD - w - 2))
        world[r0:r0 + h, c0:c0 + w] = CLS_FREE
        rooms.append((r0, c0, r0 + h, c0 + w))
    centres = [((r0 + r1) // 2, (c0 + c1) // 2) for r0, c0, r1, c1 in rooms]
    order = rng.permutation(len(rooms))
    for a, b in zip(order[:-1], order[1:]):
        (ra, ca), (rb, cb) = centres[a], centres[b]
        width = int(rng.integers(2, 5))
        if rng.random() < 0.5:
            world[min(ra, rb):max(ra, rb) + 1, ca:ca + width] = CLS_FREE
            world[rb:rb + width, min(ca, cb):max(ca, cb) + 1] = CLS_FREE
        else:
            world[ra:ra + width, min(ca, cb):max(ca, cb) + 1] = CLS_FREE
            world[min(ra, rb):max(ra, rb) + 1, cb:cb + width] = CLS_FREE
    for r0, c0, r1, c1 in rooms:
        draw = rng.random()
        if draw < 0.3:
            h, w = int(rng.integers(2, max(3, (r1 - r0) // 2 + 1))), int(rng.integers(2, max(3, (c1 - c0) // 2 + 1)))
            pr, pc = int(rng.integers(r0, max(r0 + 1, r1 - h))), int(rng.integers(c0, max(c0 + 1, c1 - w)))
            world[pr:pr + h, pc:pc + w] = CLS_WATER if draw < 0.2 else CLS_HAZARD
    return world, rooms


def world_cost_to_go(worlds: np.ndarray, goals: np.ndarray) -> np.ndarray:
    """The oracle's route cost-to-go over whole worlds [N, WORLD, WORLD] (TRAV classes) to `goals` [N, 2] (row, col):
    networks.classical_cost_to_go with CLASSICAL_COSTS (unknown CLASSICAL_ORACLE_UNKNOWN) at FINE_CELL yards, run to
    convergence. Yards, float32; CLASSICAL_SENTINEL where nothing reached."""
    table = torch.tensor(CLASSICAL_COSTS, dtype=torch.float32)
    table[CLS_UNKNOWN] = CLASSICAL_ORACLE_UNKNOWN
    factor = table[torch.as_tensor(worlds, dtype=torch.long)]
    goal = torch.zeros(factor.shape, dtype=torch.float32)
    goal[torch.arange(len(goals)), torch.as_tensor(goals[:, 0]), torch.as_tensor(goals[:, 1])] = 1.0
    cost, _ = classical_cost_to_go(factor, goal, FINE_CELL, 4 * WORLD, check=True)
    return cost.numpy()


def line_of_sight(world: np.ndarray, body: tuple[int, int], radius: int, rays: int = 360) -> np.ndarray:
    """The cells seen from `body` (a world cell; the view from its centre): a ray a degree, stepped half a cell at a
    time out to `radius` cells, seeing every cell it enters and stopping at the first wall (seen too)."""
    seen = np.zeros(world.shape, dtype=bool)
    br, bc = body
    seen[br, bc] = True
    for angle in np.linspace(0.0, 2.0 * math.pi, rays, endpoint=False):
        dr, dc = -math.cos(angle), math.sin(angle)
        for step in np.arange(0.5, radius, 0.5):
            r, c = int(math.floor(br + 0.5 + dr * step)), int(math.floor(bc + 0.5 + dc * step))
            if not (0 <= r < WORLD and 0 <= c < WORLD):
                break
            seen[r, c] = True
            if world[r, c] == CLS_WALL:
                break
    return seen


def crop_fine(grid: np.ndarray, body: tuple[int, int], fill) -> np.ndarray:
    """The fine crop [48, 48] of a world grid round the body's corner (the shared corner of world cells (br, bc),
    (br, bc + 1), (br + 1, bc), (br + 1, bc + 1): crop cell (ANCHOR, ANCHOR) is world cell (br, bc), ahead-left of
    it, as the learner's anchor is), `fill` outside the world."""
    br, bc = body
    out = np.full((CROP_SIDE, CROP_SIDE), fill, dtype=grid.dtype)
    r0, c0 = br - ANCHOR, bc - ANCHOR
    rs, cs = slice(max(0, r0), min(WORLD, r0 + CROP_SIDE)), slice(max(0, c0), min(WORLD, c0 + CROP_SIDE))
    out[rs.start - r0:rs.stop - r0, cs.start - c0:cs.stop - c0] = grid[rs, cs]
    return out


def coarse_blocks(grid: np.ndarray, body: tuple[int, int], fill) -> np.ndarray:
    """The coarse crop's cells as blocks [48, 48, RATIO, RATIO] of the world grid: coarse cell (ANCHOR, ANCHOR)
    covers world rows br - RATIO + 1 .. br and columns bc - RATIO + 1 .. bc (the same corner as the fine crop's),
    `fill` outside the world."""
    br, bc = body
    span = CROP_SIDE * RATIO
    big = np.full((span, span), fill, dtype=grid.dtype)
    r0, c0 = br + 1 - (ANCHOR + 1) * RATIO, bc + 1 - (ANCHOR + 1) * RATIO
    rs, cs = slice(max(0, r0), min(WORLD, r0 + span)), slice(max(0, c0), min(WORLD, c0 + span))
    big[rs.start - r0:rs.stop - r0, cs.start - c0:cs.stop - c0] = grid[rs, cs]
    return big.reshape(CROP_SIDE, RATIO, CROP_SIDE, RATIO).transpose(0, 2, 1, 3)


def majority(blocks: np.ndarray, values: int) -> np.ndarray:
    """Per block the most frequent value in 0..values - 1 (the first of equals)."""
    flat = blocks.reshape(blocks.shape[0], blocks.shape[1], -1)
    counts = np.stack([(flat == v).sum(-1) for v in range(values)], axis=-1)
    return counts.argmax(-1).astype(np.uint8)


def crop_bytes(classes: np.ndarray, known: np.ndarray, age: np.ndarray, visited: np.ndarray,
               walls_share: np.ndarray) -> np.ndarray:
    """One tier's crop bytes [48 x 48 x 8] from its true classes (TRAV; what a known cell shows), which cells are
    known, their look age (0..254), visited flags and the share of wall among each cell's sub-cells (0..1): the code
    byte (unknown / floor / wall / hazard; a wall by majority), height 128 where known, visited, age (255 never),
    class 0 (no entity), frontier (a known non-wall cell 4-adjacent to an unknown one), searched 0, walls 0..4."""
    code = np.zeros(classes.shape, dtype=np.uint8)
    code[known & (classes != CLS_WALL)] = MAP_FLOOR
    code[known & (classes == CLS_HAZARD)] = MAP_HAZARD
    code[known & (walls_share >= 0.5)] = MAP_WALL
    code[known & (classes == CLS_UNKNOWN)] = MAP_WALL  # outside the world reads as rock
    unknown = ~known
    frontier = known & (code != MAP_WALL) & (
        np.pad(unknown, 1)[:-2, 1:-1] | np.pad(unknown, 1)[2:, 1:-1] | np.pad(unknown, 1)[1:-1, :-2]
        | np.pad(unknown, 1)[1:-1, 2:])
    cells = np.zeros((CROP_SIDE, CROP_SIDE, MAP_CHANNELS), dtype=np.uint8)
    cells[..., MAP_CODE] = code
    cells[..., MAP_HEIGHT] = np.where(known, MAP_HEIGHT_ZERO, 0).astype(np.uint8)
    cells[..., MAP_VISITED] = (visited & known).astype(np.uint8)
    cells[..., MAP_AGE] = np.where(known, np.clip(age, 0, 254), 255).astype(np.uint8)
    cells[..., MAP_FRONTIER] = frontier.astype(np.uint8)
    cells[..., MAP_WALLS] = np.where(known, np.round(walls_share * MAP_WALLS_MAX), 0).astype(np.uint8)
    return cells.reshape(-1)


def label_bytes(fine_class: np.ndarray, coarse_class: np.ndarray, fine_cost: np.ndarray, coarse_cost: np.ndarray,
                scalars: np.ndarray) -> np.ndarray:
    """The label bytes (protocol LABEL_*): the two class grids u8, the two cost grids f16 (LABEL_UNREACHABLE where
    unreachable), the six f32 scalars."""
    out = np.zeros(protocol.LABEL_BYTES, dtype=np.uint8)
    out[protocol.LABEL_FINE_CLASS:protocol.LABEL_COARSE_CLASS] = fine_class.reshape(-1)
    out[protocol.LABEL_COARSE_CLASS:protocol.LABEL_FINE_COST] = coarse_class.reshape(-1)
    out[protocol.LABEL_FINE_COST:protocol.LABEL_COARSE_COST] = fine_cost.astype(np.float16).reshape(-1).view(np.uint8)
    out[protocol.LABEL_COARSE_COST:protocol.LABEL_SCALARS] = coarse_cost.astype(np.float16).reshape(-1).view(np.uint8)
    out[protocol.LABEL_SCALARS:] = np.asarray(scalars, dtype=np.float32).view(np.uint8)
    return out


def label_cost(cost: np.ndarray, classes: np.ndarray) -> np.ndarray:
    """A tier's label cost-to-go from the world's: LABEL_UNREACHABLE on a wall, outside the world, or where the
    oracle's route ran through a wall (UNREACHABLE_ABOVE)."""
    out = np.where((classes == CLS_WALL) | (classes == CLS_UNKNOWN) | (cost >= UNREACHABLE_ABOVE),
                   protocol.LABEL_UNREACHABLE, cost).astype(np.float32)
    return np.minimum(out, protocol.LABEL_UNREACHABLE)


def make_episode(rng: np.random.Generator, per_episode: int, episode: int) -> dict[str, np.ndarray]:
    """One world, `per_episode` (body, goal) draws on it, each as a sample (record.SAMPLE_FIELDS)."""
    world, rooms = make_world(rng)
    free = np.argwhere(world == CLS_FREE)
    memory = MEMORY_ON if rng.random() < 0.5 else MEMORY_OFF
    map_id, arena, _ = MAPS[int(rng.choice(len(MAPS), p=[share for _, _, share in MAPS]))]
    layout = int(rng.integers(0, 3))
    # The remembered rooms (the memory flag on): whole rooms the seat's shared memory holds.
    remembered = np.zeros(world.shape, dtype=bool)
    if memory == MEMORY_ON:
        for index in rng.permutation(len(rooms))[:max(1, int(len(rooms) * rng.uniform(0.3, 0.9)))]:
            r0, c0, r1, c1 = rooms[index]
            remembered[r0 - 1:r1 + 1, c0 - 1:c1 + 1] = True
    goals = free[rng.integers(0, len(free), size=per_episode)]
    costs = world_cost_to_go(np.repeat(world[None], per_episode, axis=0), goals)
    batch = empty_batch(per_episode)
    for i in range(per_episode):
        cost = costs[i]
        reachable = np.argwhere((world == CLS_FREE) & (cost < UNREACHABLE_ABOVE) & (cost > 8.0))
        body = tuple(int(v) for v in (reachable[rng.integers(0, len(reachable))] if len(reachable) else goals[i]))
        radius = int(rng.integers(8, 40))
        seen = line_of_sight(world, body, radius) | (remembered if rng.random() < 0.9 else False)
        age = np.where(line_of_sight(world, body, radius), 0, rng.integers(5, 200)).astype(np.int32)
        visited = np.zeros(world.shape, dtype=bool)
        visited[max(0, body[0] - 2):body[0] + 3, max(0, body[1] - 2):body[1] + 3] = True
        # The fine tier: world cells one to one.
        fine_class = crop_fine(world, body, CLS_UNKNOWN)
        fine_known = crop_fine(seen, body, False)
        fine_cells = crop_bytes(fine_class, fine_known, crop_fine(age, body, 255), crop_fine(visited, body, False),
                                (fine_class == CLS_WALL).astype(np.float32))
        fine_cost = label_cost(crop_fine(cost, body, CLASSICAL_SENTINEL), fine_class)
        # The coarse tier: 4 x 4 world cells a cell, the class by majority, known where any is, the cost the least.
        blocks_class = coarse_blocks(world, body, CLS_UNKNOWN)
        blocks_known = coarse_blocks(seen, body, False)
        coarse_class = majority(blocks_class, protocol.TRAV_CLASSES)
        coarse_known = blocks_known.any(axis=(2, 3))
        walls_share = ((blocks_class == CLS_WALL) & blocks_known).sum(axis=(2, 3)) / np.maximum(
            1, blocks_known.sum(axis=(2, 3)))
        coarse_age = np.where(coarse_known, coarse_blocks(age, body, 255).min(axis=(2, 3)), 255)
        coarse_cells = crop_bytes(coarse_class, coarse_known, coarse_age,
                                  coarse_blocks(visited, body, False).any(axis=(2, 3)), walls_share.astype(np.float32))
        blocks_cost = coarse_blocks(cost, body, CLASSICAL_SENTINEL)
        coarse_cost = label_cost(blocks_cost.min(axis=(2, 3)), coarse_class)
        # The goal in the body frame: the body's corner is at world (br + 1, bc + 1) in cell-edge units, the goal's
        # cell centre at (gr + 0.5, gc + 0.5); forward is up the rows.
        gr, gc = goals[i]
        forward = ((body[0] + 1) - (gr + 0.5)) * FINE_CELL
        right = ((gc + 0.5) - (body[1] + 1)) * FINE_CELL
        c_body = float(fine_cost[ANCHOR:ANCHOR + 2, ANCHOR:ANCHOR + 2].min())
        scalars = np.array([c_body, c_body + float(rng.uniform(0.0, 40.0)), protocol.LABEL_UNREACHABLE,
                            protocol.LABEL_UNREACHABLE, forward, right], dtype=np.float32)
        batch["crops"][i] = np.concatenate([fine_cells, coarse_cells])
        batch["labels"][i] = label_bytes(fine_class, coarse_class, fine_cost, coarse_cost, scalars)
        batch["route"][i] = np.zeros(len(ROUTE_COLUMNS), dtype=np.float32)
        batch["memory"][i] = memory
        batch["kept"][i] = float(memory == MEMORY_ON)
        batch["map"][i] = map_id
        batch["arena"][i] = arena
        batch["layout"][i] = layout
        batch["step"][i] = i * 8
        batch["episode"][i] = episode
        batch["env"][i] = episode % 64
    return batch


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, help="the shard directory (record.ShardWriter's)")
    parser.add_argument("--episodes", type=int, default=100, help="worlds")
    parser.add_argument("--per-episode", type=int, default=4, help="samples a world")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--shard", type=int, default=256, help="samples a shard")
    args = parser.parse_args()
    rng = np.random.default_rng(args.seed)
    writer = ShardWriter(args.out, args.shard, prefix=f"synthetic{args.seed}",
                         meta={"synthetic": True, "seed": args.seed, "world": WORLD,
                               "label_bytes": protocol.LABEL_BYTES, "crop_bytes": CROP_BYTES,
                               "route_columns": list(ROUTE_COLUMNS)})
    started = time.perf_counter()
    base = (int(args.seed) << 32) + writer.number * 100000
    for episode in range(args.episodes):
        writer.add(make_episode(rng, args.per_episode, base + episode))
        if (episode + 1) % 25 == 0:
            print(f"{(episode + 1) * args.per_episode} samples, {time.perf_counter() - started:.0f} s", flush=True)
    writer.close()
    print(f"Wrote {writer.written} samples to {writer.directory} in {time.perf_counter() - started:.0f} s",
          flush=True)


if __name__ == "__main__":
    main()
