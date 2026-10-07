#!/usr/bin/env python3
"""Compare two stage.json files: what a new build changed about a stage, before a run resumes on it.

    stage_json_diff.py old.json new.json [--allow-removed-terms] [--allow-removed-keys] [--allow-removed-columns]

The sim writes <layouts>/<stage>/stage.json each time it builds a stage (Scenario/Curriculum/StageScenario.cpp,
WriteStageFiles). The learner resumes a checkpoint against it (animus.stages.layout_changes, animus.runs.resume_mismatch):
a layout whose blocks, spans, revisions or action list moved cannot resume, and a changed tuning value changes what the
run is paid. This tool lists every difference, grouped by kind, and exits 1 on any difference that was not allowed.

Kinds: header, layouts (classes added or removed), layout-shape (obs_dim, num_actions), actions (the action names),
specs, blocks (order, spans, revisions and the block's other fields), obs-names (a block's named columns),
sets, episode-info (the columns the stage reports), categories, reward-terms, tuning, arenas, other.

What the flags allow (each lists what it let through, under ALLOWED):
  --allow-removed-terms    a reward term in old.json's reward_terms that new.json no longer has.
  --allow-removed-keys     a tuning key, or a field of an arena or another record, that new.json no longer has.
  --allow-removed-columns  an episode info column that new.json no longer reports.
Added terms, keys and columns are always reported and never allowed: a resumed run is then priced or read by
something it was not trained on. Exit status: 0 identical or only allowed differences, 1 anything else, 2 bad input.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path

KINDS = ("header", "layouts", "layout-shape", "actions", "specs", "blocks", "obs-names", "sets", "episode-info",
         "categories", "reward-terms", "tuning", "arenas", "other")
FLAGS = {"terms": "--allow-removed-terms", "keys": "--allow-removed-keys", "columns": "--allow-removed-columns"}
#: A layout's own scalar and list fields, and the kind each is reported under.
LAYOUT_FIELDS = {"obs_dim": "layout-shape", "num_actions": "layout-shape", "action_names": "actions",
                 "spec_names": "specs", "spec_roles": "specs", "sets": "sets", "blocks": "blocks"}


@dataclass
class Difference:
    kind: str
    where: str
    what: str
    removal: str = ""   # "terms", "keys" or "columns" when a flag may allow it

    def __str__(self) -> str:
        return f"{self.where}: {self.what}" if self.where else self.what


def short(value, limit: int = 60) -> str:
    text = json.dumps(value, sort_keys=True)
    return text if len(text) <= limit else text[:limit - 3] + "..."


def list_changes(old: list, new: list) -> list[str]:
    """What happened to a list of hashable-or-not items: removed, added, and (when only the order differs) moved."""
    def key(item):
        return json.dumps(item, sort_keys=True)

    old_keys, new_keys = [key(item) for item in old], [key(item) for item in new]
    old_set, new_set = set(old_keys), set(new_keys)
    removed = [short(old[i]) for i, k in enumerate(old_keys) if k not in new_set]
    added = [short(new[i]) for i, k in enumerate(new_keys) if k not in old_set]
    out = []
    if removed:
        out.append(f"removed {len(removed)}: {', '.join(removed[:12])}{' ...' if len(removed) > 12 else ''}")
    if added:
        out.append(f"added {len(added)}: {', '.join(added[:12])}{' ...' if len(added) > 12 else ''}")
    if not removed and not added and old_keys != new_keys:
        moved = [short(old[i]) for i in range(len(old)) if old_keys[i] != new_keys[i]]
        out.append(f"reordered ({len(moved)} places differ, first: {', '.join(moved[:6])})")
    return out


def dict_changes(kind: str, where: str, old: dict, new: dict, removal: str = "keys") -> list[Difference]:
    """Keys removed or added, and values changed, one level deep (a changed nested value is reported whole)."""
    out = []
    for name in old.keys() - new.keys():
        out.append(Difference(kind, f"{where}.{name}", f"removed (was {short(old[name])})", removal))
    for name in new.keys() - old.keys():
        out.append(Difference(kind, f"{where}.{name}", f"added ({short(new[name])})"))
    for name in old.keys() & new.keys():
        if old[name] != new[name]:
            out.append(Difference(kind, f"{where}.{name}", f"{short(old[name])} -> {short(new[name])}"))
    return out


def deep(kind: str, where: str, old, new, removal: str = "keys") -> list[Difference]:
    """A generic difference of two JSON values, recursing through dicts."""
    if old == new:
        return []
    if isinstance(old, dict) and isinstance(new, dict):
        out = []
        for name in sorted(old.keys() | new.keys()):
            here = f"{where}.{name}" if where else name
            if name not in new:
                out.append(Difference(kind, here, f"removed (was {short(old[name])})", removal))
            elif name not in old:
                out.append(Difference(kind, here, f"added ({short(new[name])})"))
            else:
                out += deep(kind, here, old[name], new[name], removal)
        return out
    if isinstance(old, list) and isinstance(new, list):
        return [Difference(kind, where, text) for text in list_changes(old, new)]
    return [Difference(kind, where, f"{short(old)} -> {short(new)}")]


def block_changes(layout: str, old: list, new: list) -> list[Difference]:
    out = []
    old_by, new_by = {b["name"]: b for b in old}, {b["name"]: b for b in new}
    where = f"layouts.{layout}.blocks"
    if list(old_by) != list(new_by):
        for text in list_changes(list(old_by), list(new_by)):
            out.append(Difference("blocks", where, text))
    for name in old_by.keys() & new_by.keys():
        before, after = old_by[name], new_by[name]
        for field in sorted(before.keys() | after.keys()):
            here = f"{where}.{name}.{field}"
            if field == "obs_names":
                for text in list_changes(before.get(field, []), after.get(field, [])):
                    out.append(Difference("obs-names", f"layouts.{layout}.{name}", text))
            elif before.get(field) != after.get(field):
                out.append(Difference("blocks", here, f"{short(before.get(field))} -> {short(after.get(field))}"))
    return out


def layout_changes(old: dict, new: dict) -> list[Difference]:
    out = []
    for name in sorted(old.keys() - new.keys()):
        out.append(Difference("layouts", name, "class removed"))
    for name in sorted(new.keys() - old.keys()):
        out.append(Difference("layouts", name, "class added"))
    if sorted(old) != sorted(new) or len(old) != len(new):
        out.append(Difference("layouts", "layouts", f"{len(old)} classes -> {len(new)}"))
    for name in sorted(old.keys() & new.keys()):
        before, after = old[name], new[name]
        for field in sorted(before.keys() | after.keys()):
            if before.get(field) == after.get(field):
                continue
            kind = LAYOUT_FIELDS.get(field, "blocks")
            where = f"layouts.{name}.{field}"
            if field == "blocks":
                out += block_changes(name, before.get(field, []), after.get(field, []))
            elif isinstance(before.get(field), list) and isinstance(after.get(field), list):
                out += [Difference(kind, where, text) for text in list_changes(before[field], after[field])]
            else:
                out.append(Difference(kind, where, f"{short(before.get(field))} -> {short(after.get(field))}"))
    return out


def arena_changes(old: list, new: list) -> list[Difference]:
    old_by, new_by = {a["name"]: a for a in old}, {a["name"]: a for a in new}
    out = []
    for name in sorted(old_by.keys() - new_by.keys()):
        out.append(Difference("arenas", name, "arena removed"))
    for name in sorted(new_by.keys() - old_by.keys()):
        out.append(Difference("arenas", name, "arena added"))
    for name in sorted(old_by.keys() & new_by.keys()):
        out += deep("arenas", f"arenas.{name}", old_by[name], new_by[name])
    if list(old_by) != list(new_by) and old_by.keys() == new_by.keys():
        out.append(Difference("arenas", "arenas", f"order {list(old_by)} -> {list(new_by)}"))
    return out


def compare(old: dict, new: dict) -> list[Difference]:
    """Every difference between two stage.json documents."""
    out: list[Difference] = []
    handled = {"layouts", "episode_info", "episode_categories", "reward_terms", "tuning", "arenas"}
    scalar = {"format", "stage", "suffix", "extends", "summary", "seats"}
    for key in sorted(old.keys() | new.keys()):
        if key in handled:
            continue
        kind = "header" if key in scalar else "other"
        if key not in new:
            out.append(Difference(kind, key, f"removed (was {short(old[key])})", "keys"))
        elif key not in old:
            out.append(Difference(kind, key, f"added ({short(new[key])})"))
        else:
            out += deep(kind, key, old[key], new[key])
    out += layout_changes(old.get("layouts", {}), new.get("layouts", {}))
    for text in list_changes(old.get("episode_info", []), new.get("episode_info", [])):
        removal = "columns" if text.startswith("removed") else ""
        out.append(Difference("episode-info", "episode_info", text, removal))
    out += deep("categories", "episode_categories", old.get("episode_categories", {}),
                new.get("episode_categories", {}))
    # Reward terms: a removed term is its own flag; a category change (outcome <-> shaping) is never allowed.
    old_terms, new_terms = old.get("reward_terms", {}), new.get("reward_terms", {})
    for term in sorted(old_terms.keys() - new_terms.keys()):
        out.append(Difference("reward-terms", f"reward_terms.{term}", f"removed (was {old_terms[term]})", "terms"))
    for term in sorted(new_terms.keys() - old_terms.keys()):
        out.append(Difference("reward-terms", f"reward_terms.{term}", f"added ({new_terms[term]})"))
    for term in sorted(old_terms.keys() & new_terms.keys()):
        if old_terms[term] != new_terms[term]:
            out.append(Difference("reward-terms", f"reward_terms.{term}",
                                  f"category {old_terms[term]} -> {new_terms[term]}"))
    out += dict_changes("tuning", "tuning", old.get("tuning", {}), new.get("tuning", {}))
    out += arena_changes(old.get("arenas", []), new.get("arenas", []))
    return out


def split(differences: list[Difference], allow: set[str]) -> tuple[list[Difference], list[Difference]]:
    """(the unallowed, the allowed) of `differences` given the allowed kinds of removal."""
    refused = [d for d in differences if not d.removal or d.removal not in allow]
    allowed = [d for d in differences if d.removal and d.removal in allow]
    return refused, allowed


def report(differences: list[Difference], allowed: list[Difference], limit: int = 40) -> str:
    lines = []
    for kind in KINDS:
        group = [d for d in differences if d.kind == kind]
        if not group:
            continue
        lines.append(f"{kind} ({len(group)})")
        for d in group[:limit]:
            lines.append(f"  {d}")
        if len(group) > limit:
            lines.append(f"  ... and {len(group) - limit} more")
    if allowed:
        lines.append(f"ALLOWED ({len(allowed)})")
        for removal, flag in FLAGS.items():
            group = [d for d in allowed if d.removal == removal]
            if group:
                lines.append(f"  by {flag} ({len(group)})")
                lines += [f"    {d}" for d in group[:limit]]
                if len(group) > limit:
                    lines.append(f"    ... and {len(group) - limit} more")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    parser.add_argument("--allow-removed-terms", action="store_true")
    parser.add_argument("--allow-removed-keys", action="store_true")
    parser.add_argument("--allow-removed-columns", action="store_true")
    args = parser.parse_args(argv)
    try:
        old, new = json.loads(args.old.read_text()), json.loads(args.new.read_text())
    except (OSError, ValueError) as error:
        print(f"stage_json_diff: {error}", file=sys.stderr)
        return 2
    allow = {name for name, on in (("terms", args.allow_removed_terms), ("keys", args.allow_removed_keys),
                                    ("columns", args.allow_removed_columns)) if on}
    refused, allowed = split(compare(old, new), allow)
    if not refused and not allowed:
        print(f"identical: {args.old} == {args.new}")
        return 0
    print(report(refused, allowed))
    print(f"\n{len(refused)} difference(s) not allowed, {len(allowed)} allowed")
    return 1 if refused else 0


if __name__ == "__main__":
    sys.exit(main())
