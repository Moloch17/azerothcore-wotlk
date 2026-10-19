#!/usr/bin/env python3
"""The nav stages' map audit, read back (PointNavEncounter::RunAudit).

  nav_audit.py summary <eval_episodes.jsonl> [--steps N]   the audit_* columns of the trips that did not arrive
  nav_audit.py render <dir-of-.audit-files> [--out DIR]     one PNG per timed-out evaluation trip's band

The PNG: the bake underneath (dark: no ground, light: floor, tan: a step or drop, blue: water), the bot's map over it
(red: a wall where the bake has floor, maroon: a wall on the bake's rock, magenta: a hole the configuration space
closed, orange: a hazard; unknown cells are left dim), the bake's true route in green, the cheapest way through the
band on the bot's map in yellow, the body cyan and the goal white.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

NAMES = ["audit_unreachable", "audit_true_yards", "audit_corridor_cost", "audit_truncated", "audit_cells",
         "audit_wall", "audit_closed", "audit_unknown", "audit_hazard", "audit_floor", "audit_first_block_yards",
         "audit_wall_ray", "audit_wall_mesh", "audit_wall_bump", "audit_wall_inherited", "audit_wall_true",
         "audit_wall_face", "audit_wall_step", "audit_wall_phantom", "audit_wall_floorless", "audit_wall_layer_off",
         "audit_fell", "audit_stranded", "audit_route_drops", "audit_route_drops_seen"]
SOLID = {0, 2, 10}
STEP = {5, 6, 7, 8, 9}


def summary(path, steps):
    rows = [json.loads(line) for line in open(path)]
    if steps is None:
        steps = max(r["env_steps"] for r in rows)
    rows = [r for r in rows if r["env_steps"] == steps and r.get("policy") == "learner" and not r["arrived"]
            and "audit_cells" in r]
    print(f"steps {steps}: {len(rows)} trips short of the goal")
    if not rows:
        return

    def bin_of(r):
        if r.get("audit_stranded"):
            return "stranded after a drop"
        if r["audit_unreachable"]:
            return "fell, no route" if r.get("audit_fell") else "no route (bake reading)"
        if r["audit_wall"] or r["audit_closed"]:
            return "map blocks the true way"
        if r["audit_corridor_cost"] > 1.5 * max(1.0, r["audit_true_yards"]) and r["audit_unknown"]:
            return "true way unseen (unknown)"
        return "true way open on the map"

    bins = {}
    for r in rows:
        bins.setdefault(bin_of(r), []).append(r)
    for name, members in sorted(bins.items(), key=lambda kv: -len(kv[1])):
        print(f"  {name:28s} {len(members):4d}")
    total = {n: sum(r[n] for r in rows) for n in NAMES}
    walls = max(1.0, total["audit_wall"])
    print("walls on the ways: %d, closed holes %d, unknown cells %d, of %d cells" % (
        total["audit_wall"], total["audit_closed"], total["audit_unknown"], total["audit_cells"]))
    for group in (("ray", "mesh", "bump", "inherited"), ("true", "face", "step", "phantom"),
                  ("floorless", "layer_off")):
        print("  " + "  ".join(f"{g} {total['audit_wall_' + g]:.0f} ({100 * total['audit_wall_' + g] / walls:.0f}%)"
                               for g in group))
    print("\nseed   map  true_yd  corridor  map_route  wall closed unknown  ray mesh bump inh  phantom face step")
    for r in sorted(rows, key=lambda r: -r["audit_wall"] - r["audit_closed"]):
        print("%5d %4d %8.0f %9.0f %10.0f %5d %6d %7d %4d %4d %4d %3d %8d %4d %4d" % (
            r["seed"], r["map"], r["audit_true_yards"], r["audit_corridor_cost"], r["map_route_end_yards"],
            r["audit_wall"], r["audit_closed"], r["audit_unknown"], r["audit_wall_ray"], r["audit_wall_mesh"],
            r["audit_wall_bump"], r["audit_wall_inherited"], r["audit_wall_phantom"], r["audit_wall_face"],
            r["audit_wall_step"]))


def render(directory, out, scale):
    from PIL import Image, ImageDraw
    out.mkdir(parents=True, exist_ok=True)
    for file in sorted(Path(directory).glob("*.audit")):
        data = file.read_bytes()
        if data[:7] != b"NAVAUD1":
            print(f"{file}: not an audit file", file=sys.stderr)
            continue
        side, x0, y0, seed = struct.unpack_from("<4i", data, 8)
        body_x, body_y, goal_x, goal_y, map_route = struct.unpack_from("<5f", data, 24)
        audit = struct.unpack_from(f"<{len(NAMES)}f", data, 44)
        offset = 44 + 4 * len(NAMES)
        cells = side * side
        code, kind, source, bake, flags = (data[offset + i * cells: offset + (i + 1) * cells] for i in range(5))
        image = Image.new("RGB", (side, side))
        pixels = image.load()
        for at in range(cells):
            col, row = at % side, at // side
            what = bake[at]
            colour = (40, 40, 44) if what in SOLID else (70, 110, 170) if what == 3 else \
                (160, 140, 100) if what in STEP else (150, 150, 150)
            if code[at] == 0:
                colour = tuple(c // 2 for c in colour)
            if kind[at] == 1:
                colour = (120, 20, 30) if what in SOLID else (230, 30, 30)
            elif kind[at] == 2:
                colour = (220, 40, 220)
            elif kind[at] == 4:
                colour = (240, 140, 20)
            if flags[at] & 2 and kind[at] == 0:
                colour = (colour[0] // 2, min(255, colour[1] + 70), colour[2] // 2)
            if flags[at] & 4:
                colour = (250, 230, 40) if kind[at] in (0, 3) else (255, 255, 120)
            pixels[col, side - 1 - row] = colour
        image = image.resize((side * scale, side * scale), Image.NEAREST)
        draw = ImageDraw.Draw(image)

        def mark(x, y, colour):
            px = (x - x0) * scale
            py = (side - (y - y0)) * scale
            draw.ellipse((px - 2 * scale, py - 2 * scale, px + 2 * scale, py + 2 * scale), outline=colour, width=2)

        mark(body_x, body_y, (0, 255, 255))
        mark(goal_x, goal_y, (255, 255, 255))
        values = dict(zip(NAMES, audit))
        draw.text((4, 4), "seed %d  true %.0f yd  corridor %.0f  map route %.0f  walls %d (phantom %d face %d step %d)"
                  "  closed %d  unknown %d" % (seed, values["audit_true_yards"], values["audit_corridor_cost"],
                                               map_route, values["audit_wall"], values["audit_wall_phantom"],
                                               values["audit_wall_face"], values["audit_wall_step"],
                                               values["audit_closed"], values["audit_unknown"]), fill=(255, 255, 255))
        target = out / (file.stem + ".png")
        image.save(target)
        print(target)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    s = sub.add_parser("summary")
    s.add_argument("episodes")
    s.add_argument("--steps", type=int)
    r = sub.add_parser("render")
    r.add_argument("directory")
    r.add_argument("--out", type=Path)
    r.add_argument("--scale", type=int, default=4)
    args = parser.parse_args()
    if args.command == "summary":
        summary(args.episodes, args.steps)
    else:
        render(args.directory, args.out or Path(args.directory), args.scale)


if __name__ == "__main__":
    main()
