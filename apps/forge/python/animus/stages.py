"""The sim's stage descriptions.

When the worldserver builds a curriculum stage it writes ``<layouts_dir>/<scenario>/stage.json`` beside the stage's
layout manifests: the stage's blocks, the stages it seeds from (``seed_chain``, closest first), the model name of
every layout, its episode info columns and the effective tuning. The learner copies it into the run directory, seeds
from it, and export names models from it.
"""

from __future__ import annotations

import json
from pathlib import Path

STAGE_FILE = "stage.json"


def stage_dir(layouts_dir: str | Path, scenario: str) -> Path:
    return Path(layouts_dir) / scenario


def load_stage(layouts_dir: str | Path, scenario: str) -> dict | None:
    """The scenario's stage.json, or None for a scenario without one (not a curriculum stage, or not built yet)."""
    path = stage_dir(layouts_dir, scenario) / STAGE_FILE
    return json.loads(path.read_text()) if path.is_file() else None


def seed_chain(stage: dict | None) -> list[str]:
    """The stages a run of `stage` seeds from, closest first."""
    return list(stage.get("seed_chain", ())) if stage else []


def merges(stage: dict | None) -> list[str]:
    """A merge stage's further parents: each seeds the blocks only it has and can teach its arenas."""
    return list(stage.get("merges", ())) if stage else []


def arena_names(stage: dict | None) -> tuple[str, ...]:
    """The stage's arena names, in the order the episode info column "arena" and the critic state index them."""
    return tuple(arena["name"] for arena in stage.get("arenas", ())) if stage else ()


def arena_state_span(stage: dict | None) -> Span | None:
    """(first, count) of the arena one-hot in the critic state, or None when the stage.json does not say."""
    state = (stage or {}).get("state", {})
    return (int(state["arena_first"]), int(state["arena_count"])) if "arena_first" in state else None


Span = tuple[int, int]  # (first, count)


def block_spans(stage: dict | None, layout: str) -> dict[str, tuple[Span, Span]] | None:
    """Block name -> (observation span, action span) of `layout` in `stage`, or None when the stage has no spans for
    it (a stage.json from before block spans, or no stage.json)."""
    entry = (stage or {}).get("layouts", {}).get(layout)
    if not entry:
        return None
    return {block["name"]: (tuple(block["obs"]), tuple(block["actions"])) for block in entry["blocks"]}


def model_names(stage: dict | None) -> dict[str, str]:
    """Layout name -> model name (warrior -> warrior_duel): one model per class, covering its every role."""
    return dict(stage.get("models", {})) if stage else {}


def arena_plans(stage: dict | None) -> list[tuple[str, int]]:
    """Each arena's (seat plan, team width) -- "solo", "party", "mirror", "raid" or "teams" -- from a stage.json of
    format 3; [] for an older one."""
    return [(str(arena.get("plan", "solo")), int(arena.get("team_seats", 0) or 0))
            for arena in (stage or {}).get("arenas", ()) if "plan" in arena]


def cast_agents(stage: dict | None) -> list[dict]:
    """The agents the stage declares for a frozen checkpoint to play: [{agent, name}]."""
    return [dict(entry) for entry in (stage or {}).get("cast", ()) if "agent" in entry]
