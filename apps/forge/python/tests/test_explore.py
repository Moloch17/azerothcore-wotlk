"""Go-Explore starts (animus.explore, peak-play W5): the cells read from a wing run's episode info, once a run; the
archive's choice of starts (rarely tried and far in first), its bound; and EXPLORE_STARTS on the wire."""

import numpy as np

from animus import protocol as p
from animus.explore import MARKS, PACK_WORDS, Cell, ExploreArchive, cells_of, mark_columns


def names() -> list[str]:
    columns = ["present", "wing_started", "wing_arena", "wing_tier", "wing_marks"]
    for mark in range(MARKS):
        columns += [f"wing_mark{mark}_packs{word}" for word in range(PACK_WORDS)]
        columns += [f"wing_mark{mark}_yard", f"wing_mark{mark}_seconds"]
    return columns


def run_row(columns: dict[str, int], width: int, arena: int, tier: int, marks: list[tuple[int, int, float]]):
    row = np.zeros(width, np.float32)
    row[columns["wing_arena"]], row[columns["wing_tier"]], row[columns["wing_marks"]] = arena, tier, len(marks)
    for index, (packs, yard, seconds) in enumerate(marks):
        row[columns[f"wing_mark{index}_packs0"]] = packs
        row[columns[f"wing_mark{index}_yard"]] = yard
        row[columns[f"wing_mark{index}_seconds"]] = seconds
    return row


def test_cells_are_read_once_a_run_and_the_table_prefers_the_rarely_tried_and_far_in():
    columns = mark_columns(names())
    assert columns is not None and mark_columns(["present"]) is None
    width = len(names())
    deep = run_row(columns, width, 0, 1, [(0b1, 2, 10.0), (0b111, 5, 40.0)])
    shallow = run_row(columns, width, 0, 1, [(0b1, 2, 8.0)])
    # A run's rows come once per seat: five seats of the deep run, one of the shallow, an empty seat's zeros.
    rows = np.stack([deep] * 5 + [shallow, np.zeros(width, np.float32)])
    found = cells_of(rows, columns)
    assert [(cell.packs[0], seconds) for cell, seconds in found] == [(0b1, 10.0), (0b111, 40.0), (0b1, 8.0)]

    archive = ExploreArchive(max_cells=3)
    for cell, seconds in found:
        archive.add(cell, seconds, env_steps=100)
    first, deeper = Cell(0, 1, (0b1, 0, 0, 0), 2), Cell(0, 1, (0b111, 0, 0, 0), 5)
    assert archive.cells[first].visits == 2 and archive.cells[first].seconds == 8.0
    [(top, _), (second, _)] = archive.table()
    assert (top, second) == (deeper, first)             # one visit and three packs beats two visits and one
    for _ in range(20):
        archive.add(deeper, 50.0, 200)
    assert archive.table()[0][0] == first               # tried enough, the other comes first

    # Bounded: past max_cells the shallowest, most visited goes.
    archive.add(Cell(0, 1, (0b11, 0, 0, 0), 3), 30.0, 300)
    archive.add(Cell(1, 0, (0b1111, 0, 0, 0), 9), 60.0, 300)
    assert len(archive.cells) == 3 and first not in archive.cells
    restored = ExploreArchive()
    restored.load_state_dict(archive.state_dict())
    assert restored.cells == archive.cells


def test_explore_starts_on_the_wire():
    payload = p.encode_explore_starts(0.5, [(2, 1, (5, 0, 0, 1), 7, 0.25)])
    share, count = p.EXPLORE_STARTS.unpack_from(payload)
    assert (share, count) == (0.5, 1) and len(payload) == p.EXPLORE_STARTS.size + p.EXPLORE_CELL.size == 40
    assert p.EXPLORE_CELL.unpack_from(payload, p.EXPLORE_STARTS.size) == (2, 1, 5, 0, 0, 1, 7, 0.25)
