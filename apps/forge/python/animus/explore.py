"""Go-Explore starts for the dungeon wings (peak-play W5).

A wing run is long and its successes are rare, so a run that got far is remembered by where it got to -- its **cell**:
the arena and its row (tier), which packs on the route are cleared (InstanceEncounter's marks: one each time the set
changes) and how far along the route the party stood (in 16-yard buckets) -- and later training runs start again from
there rather than always from the door (the sim's StartAt). PPO trained from those starts is Go-Explore's
robustification phase, in the one loop. A start is "rested at that point": every seat whole, the cell's packs gone.

The archive keeps, per cell, how many episodes reached it (a start from it included) and the soonest an episode
reached it. A cell is chosen as a start with weight 1/sqrt(visits + 1), raised for cells further along the route:
rarely tried and far in is what is worth starting from. The sim takes a start from the table it was last sent
(EXPLORE_STARTS) `share` of its training resets; evaluation always starts at the door, so best.pt and every evaluated
score are the door's.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

#: Words of the cleared-packs bitset, 24 bits each (a float of the episode info holds 24 bits exactly).
PACK_WORDS = 4
#: Marks a run reports (InstanceEncounter::EXPLORE_MARKS) and cells sent to the sim at most (MAX_EXPLORE_STARTS).
MARKS = 8
TABLE_SIZE = 64


@dataclass(frozen=True)
class Cell:
    arena: int                          # the stage's arena index
    tier: int                           # its row (map and boss)
    packs: tuple[int, ...]              # cleared packs, route order, PACK_WORDS words of 24 bits
    yard: int                           # the party's place on the route / 16 yards

    @property
    def depth(self) -> int:
        """How far along: the packs cleared."""
        return sum(bin(word).count("1") for word in self.packs)


@dataclass
class Entry:
    visits: int = 0
    seconds: float = math.inf           # the soonest an episode reached the cell
    found: int = 0                      # env steps when it was first reached


def mark_columns(names: list[str]) -> dict[str, int] | None:
    """The episode info columns a wing's cells are read from (InstanceEncounter::AddEpisodeInfo), or None when the
    stage reports none (no wing arena)."""
    wanted = ["wing_started", "wing_arena", "wing_tier", "wing_marks"]
    for mark in range(MARKS):
        wanted += [f"wing_mark{mark}_packs{word}" for word in range(PACK_WORDS)]
        wanted += [f"wing_mark{mark}_yard", f"wing_mark{mark}_seconds"]
    index = {name: i for i, name in enumerate(names)}
    return {name: index[name] for name in wanted} if all(name in index for name in wanted) else None


def cells_of(rows: np.ndarray, columns: dict[str, int]) -> list[tuple[Cell, float]]:
    """Every (cell, seconds into its run) the ended runs in `rows` reached. A run's rows come once per seat, alike:
    each run is taken once."""
    found = []
    seen = set()
    for row in rows:
        marks = int(row[columns["wing_marks"]])
        if marks <= 0:
            continue
        key = tuple(float(value) for value in row[list(columns.values())])
        if key in seen:
            continue
        seen.add(key)
        arena, tier = int(row[columns["wing_arena"]]), int(row[columns["wing_tier"]])
        for mark in range(min(marks, MARKS)):
            packs = tuple(int(row[columns[f"wing_mark{mark}_packs{word}"]]) for word in range(PACK_WORDS))
            cell = Cell(arena, tier, packs, int(row[columns[f"wing_mark{mark}_yard"]]))
            found.append((cell, float(row[columns[f"wing_mark{mark}_seconds"]])))
    return found


@dataclass
class ExploreArchive:
    """Cells of the stage's wings. `max_cells` bounds the memory: past it the shallowest, most visited cell goes."""

    max_cells: int = 4096
    depth_weight: float = 1.0
    cells: dict[Cell, Entry] = field(default_factory=dict)

    def add(self, cell: Cell, seconds: float, env_steps: int) -> bool:
        """An episode reached `cell` `seconds` into its run; True when the cell is new."""
        entry = self.cells.get(cell)
        new = entry is None
        if new:
            entry = self.cells[cell] = Entry(found=env_steps)
        entry.visits += 1
        entry.seconds = min(entry.seconds, seconds)
        if new and len(self.cells) > self.max_cells:
            del self.cells[min(self.cells, key=lambda c: (c.depth, -self.cells[c].visits))]
        return new

    def weight(self, cell: Cell, deepest: int) -> float:
        bonus = 1.0 + self.depth_weight * (cell.depth / deepest if deepest else 0.0)
        return bonus / math.sqrt(self.cells[cell].visits + 1)

    def table(self, size: int = TABLE_SIZE) -> list[tuple[Cell, float]]:
        """The start table: the `size` cells of highest weight, each with its weight (the sim draws by weight)."""
        candidates = [cell for cell in self.cells if cell.depth > 0]
        if not candidates:
            return []
        deepest = max(cell.depth for cell in candidates)
        weighted = sorted(((cell, self.weight(cell, deepest)) for cell in candidates),
                          key=lambda item: (-item[1], -item[0].depth))
        return weighted[:size]

    def summary(self) -> dict[str, float]:
        return {"explore_cells": float(len(self.cells)),
                "explore_deepest": float(max((cell.depth for cell in self.cells), default=0))}

    def state_dict(self) -> dict:
        return {"cells": [[cell.arena, cell.tier, list(cell.packs), cell.yard, entry.visits, entry.seconds, entry.found]
                          for cell, entry in self.cells.items()]}

    def load_state_dict(self, state: dict) -> None:
        self.cells = {Cell(arena, tier, tuple(packs), yard): Entry(visits, seconds, found)
                      for arena, tier, packs, yard, visits, seconds, found in state.get("cells", ())}
