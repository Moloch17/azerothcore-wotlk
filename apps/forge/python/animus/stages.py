"""The sim's stage descriptions.

When the worldserver builds a curriculum stage it writes ``<layouts_dir>/<scenario>/stage.json`` beside the stage's
layout manifests: the stage's blocks, the stages it seeds from (``seed_chain``, closest first), the model name of
every layout, its episode info columns and the effective tuning. The learner copies it into the run directory, seeds
from it, and export names models from it.
"""

from __future__ import annotations

import hashlib
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


def block_revisions(stage: dict | None, layout: str) -> dict[str, int]:
    """Block name -> its revision in `layout` (Block::Revision: bumped when a block's columns change meaning in
    place); 0 for a block the stage.json gives none, which is every block of a stage.json from before revisions."""
    entry = (stage or {}).get("layouts", {}).get(layout) or {}
    return {block["name"]: int(block.get("revision", 0)) for block in entry.get("blocks", ())}


def revised_blocks(old_stage: dict | None, new_stage: dict | None, layout: str) -> dict[str, tuple[int, int]]:
    """The blocks both stages give `layout` whose revision differs: name -> (old revision, new revision)."""
    old, new = block_revisions(old_stage, layout), block_revisions(new_stage, layout)
    return {name: (old[name], revision) for name, revision in new.items() if name in old and old[name] != revision}


def layout_signature(stage: dict | None, layout: str) -> str | None:
    """What `layout`'s observation and actions are, as a short hash of its blocks in order (name, spans, revision); None
    without spans. Two stage.jsons with the same signature lay the layout out identically."""
    entry = (stage or {}).get("layouts", {}).get(layout)
    if not entry or "blocks" not in entry:
        return None
    blocks = [[block["name"], list(block["obs"]), list(block["actions"]), int(block.get("revision", 0))]
              for block in entry["blocks"]]
    return hashlib.sha1(json.dumps(blocks).encode()).hexdigest()[:12]


def layout_changes(old_stage: dict | None, new_stage: dict | None) -> list[str]:
    """Every layout both stage.jsons describe whose signature differs, with what changed ("warrior_dps: support
    revision 0 -> 1, width 152+9 -> 156+9 (obs+actions)"), one entry a layout. Empty when nothing changed, or when
    either side has no spans (resume_mismatch's shapes are then the only check)."""
    changes = []
    for layout in (new_stage or {}).get("layouts", {}):
        old_signature, new_signature = layout_signature(old_stage, layout), layout_signature(new_stage, layout)
        if old_signature is None or new_signature is None or old_signature == new_signature:
            continue
        old, new = block_spans(old_stage, layout), block_spans(new_stage, layout)
        old_revisions, new_revisions = block_revisions(old_stage, layout), block_revisions(new_stage, layout)
        parts = [f"{name} added" for name in new if name not in old]
        parts += [f"{name} dropped" for name in old if name not in new]
        for name in (name for name in new if name in old):
            (old_obs, old_actions), (new_obs, new_actions) = old[name], new[name]
            what = []
            if old_revisions[name] != new_revisions[name]:
                what.append(f"revision {old_revisions[name]} -> {new_revisions[name]}")
            if old_obs[1] != new_obs[1] or old_actions[1] != new_actions[1]:
                what.append(f"width {old_obs[1]}+{old_actions[1]} -> {new_obs[1]}+{new_actions[1]} (obs+actions)")
            elif old_obs[0] != new_obs[0] or old_actions[0] != new_actions[0]:
                what.append(f"moved from obs {old_obs[0]} to {new_obs[0]}")
            if what:
                parts.append(f"{name} {', '.join(what)}")
        changes.append(f"{layout}: {'; '.join(parts) or 'block order'}")
    return changes


def model_names(stage: dict | None) -> dict[str, str]:
    """Layout name -> model name (warrior -> warrior_duel): one model per class, covering its every role."""
    return dict(stage.get("models", {})) if stage else {}


def arena_plans(stage: dict | None) -> list[tuple[str, int]]:
    """Each arena's (seat plan, team width) -- "solo", "party", "mirror", "raid", "teams" or "shared" (groups
    sharing a zone, not opponents) -- from a stage.json of format 3; [] for an older one."""
    return [(str(arena.get("plan", "solo")), int(arena.get("team_seats", 0) or 0))
            for arena in (stage or {}).get("arenas", ()) if "plan" in arena]


def cast_agents(stage: dict | None) -> list[dict]:
    """The agents the stage declares for a frozen checkpoint to play: [{agent, name}]."""
    return [dict(entry) for entry in (stage or {}).get("cast", ()) if "agent" in entry]
