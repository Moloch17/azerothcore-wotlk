#!/usr/bin/env python3
"""Old camera (live collision trees) against the baked camera, pose by pose.

Reads the old camera's snapshots (`forge camera snapshot`: oldcam-<pose>-{depth.pgm,kind.ppm,height.pgm} and
old_console.json) and the baked camera's (scene_baker bench: newcam-<pose>-... and new_timing.json), and writes into
<root>/compare/:

  <pose>-compare.png     old kind | new kind | old depth | new depth | old slope | new slope | difference, 3x, captioned with the timings
  contact-sheet.png      every pose, one row each
  metrics.json           per pose and overall agreement
  summary.md             the agreement and timing tables

and <root>/new/newcam-<pose>.png (the new kind, depth and height panels).

    python3 apps/forge/tools/baked_camera_compare.py var/baked-camera-compare
    python3 apps/forge/tools/baked_camera_compare.py var/baked-camera-compare --map 36 --old old36 --new new36 \\
        --out compare36 --scene var/baked-camera-compare/data/scenes/036.scene

Arguments: <root> and, all optional, --map (the map id shown in the captions, default 34), --old / --new / --out
(directories under <root>, default old, new, compare), --scene (the scene file, for attributing parting pixels to
cracks of the mesh; default <root>/data/scenes/<map>.scene).

The depth channel is read back from the .pgm byte (round(255 x the log-scaled distance), sky 255): one step is about
3.3 percent of the distance, so "within 1 percent" means "the same byte". Distances below are that byte turned back
into yards (NEAR 0.25 to DISTANCE_REFERENCE 1000 on a log scale), which is only as exact as the byte.
"""
import argparse
import json
import math
import re
import statistics
import sys
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

NEAR = 0.25
REFERENCE = 1000.0
SCALE = 3

CLASS_NAMES = ["sky", "terrain", "model", "door", "water", "deadly", "hostile_creature", "neutral_creature",
               "friendly_creature", "hostile_player", "friendly_player", "quest_giver", "vendor", "trainer",
               "lootable_corpse", "corpse", "chest", "herb", "ore", "mailbox", "quest_object", "usable_object",
               "other_object", "ground_hazard"]
CLASS_COLOURS = [(30, 30, 80), (60, 160, 60), (160, 160, 160), (170, 100, 40), (40, 90, 220), (240, 80, 0),
                 (220, 0, 0), (230, 230, 0), (0, 230, 160), (255, 0, 170), (0, 160, 255), (255, 170, 0),
                 (170, 90, 230), (110, 60, 180), (255, 120, 120), (110, 40, 40), (200, 150, 60), (120, 255, 80),
                 (120, 180, 200), (40, 40, 200), (255, 255, 140), (0, 255, 255), (120, 90, 60), (255, 40, 220)]


def font(size):
    for name in ("DejaVuSans.ttf", "LiberationSans-Regular.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def read(path):
    return np.array(Image.open(path))


def classes_of(ppm):
    """The class id of each pixel of a kind image (-1 for a colour no class has)."""
    out = np.full(ppm.shape[:2], -1, dtype=np.int32)
    for index, colour in enumerate(CLASS_COLOURS):
        out[(ppm == np.array(colour, dtype=np.uint8)).all(axis=2)] = index
    return out


def yards(byte):
    """The distance a depth byte stands for (inf for sky)."""
    byte = byte.astype(np.float64)
    out = NEAR * np.exp(byte / 255.0 * math.log(REFERENCE / NEAR))
    out[byte >= 255] = np.inf
    return out


def old_timing(text):
    """The numbers of a `forge camera snapshot` console print."""
    def grab(pattern):
        found = re.search(pattern, text)
        return float(found.group(1)) if found else None
    return {
        "rays": grab(r"(\d+) rays in"),
        "wall_us": grab(r"rays in (\d+) us"),
        "static_and_dynamic_us": grab(r"trees (\d+) us"),
        "tree_casts": grab(r"trees \d+ us \((\d+) casts"),
        "liquid_us": grab(r"WMO liquids (\d+) us"),
        "terrain_us": grab(r"terrain (\d+) us"),
        "units_us": grab(r"units (\d+) us"),
    }


def load_scene_triangles(path):
    """The scene's solid triangles as (vertex 0, edge 1, edge 2) in double precision (Scene header and slot 0)."""
    import struct
    data = Path(path).read_bytes()
    count = struct.unpack_from("<I", data, 72)[0]
    offset = struct.unpack_from("<QQ", data, 256)[0]
    return np.frombuffer(data, dtype="<f4", count=count * 9, offset=offset).reshape(-1, 3, 3).astype(np.float64)


def pixel_ray(pose, row, col, width=128, height=64, fov_h=120.0, fov_v=60.0):
    """The camera and the unit direction of a pixel, as Vision::PlaceCamera (zoom 0) and PixelDirection."""
    px, py, pz, yaw, pitch, _zoom = pose
    rad = math.pi / 180.0
    origin = np.array([px, py, pz + 0.9 * 2.0])
    yaw_right = ((col + 0.5) / width * fov_h - fov_h / 2.0) * rad
    pitch_up = (fov_v / 2.0 - (row + 0.5) / height * fov_v) * rad
    azimuth, elevation = yaw * rad - yaw_right, pitch * rad + pitch_up
    return origin, np.array([math.cos(elevation) * math.cos(azimuth), math.cos(elevation) * math.sin(azimuth),
                             math.sin(elevation)])


def first_hit(tris, origin, direction, slack):
    """Nearest distance along the ray to a triangle fattened by `slack` (barycentric), or None."""
    v0, e1, e2 = tris[:, 0], tris[:, 1], tris[:, 2]
    p = np.cross(direction, e2)
    det = (e1 * p).sum(1)
    ok = np.abs(det) > 1e-18
    inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
    s = origin - v0
    u = (s * p).sum(1) * inv
    q = np.cross(s, e1)
    v = (q * direction).sum(1) * inv
    t = (e2 * q).sum(1) * inv
    hit = ok & (u >= -slack) & (v >= -slack) & (u + v <= 1 + slack) & (t >= 0)
    return float(t[hit].min()) if hit.any() else None


def attribute(tris, pose, pixels):
    """For pixels where the two cameras part by 2 or more depth steps: who passes through a crack of the mesh.

    exact = the nearest hit of the triangles as extracted; closed = the nearest once triangles are fattened by 1
    percent of their size (a crack of the mesh shut). old / new are the cameras' distances (from their bytes)."""
    out = []
    for row, col, old_byte, new_byte in pixels:
        origin, direction = pixel_ray(pose, row, col)
        exact = first_hit(tris, origin, direction, 0.0)
        closed = first_hit(tris, origin, direction, 0.01)
        old_d, new_d = float(yards(np.array([old_byte]))[0]), float(yards(np.array([new_byte]))[0])
        near = lambda a, b: a is not None and abs(a - b) <= 0.04 * b + 0.05
        if closed is not None and exact is not None and closed < exact - 0.05:
            if near(closed, old_d) and near(exact, new_d):
                kind = "crack: old stops at it, new passes through"
            elif near(closed, new_d) and near(exact, old_d):
                kind = "crack: new stops at it, old passes through"
            else:
                kind = "crack: neither matches (a different edge)"
        elif near(exact, new_d) and not near(exact, old_d):
            kind = "no crack here: new = extracted geometry, old differs"
        elif near(exact, old_d) and not near(exact, new_d):
            kind = "no crack here: old = extracted geometry, new differs"
        else:
            kind = "other"
        out.append({"row": row, "col": col, "old_byte": old_byte, "new_byte": new_byte,
                    "old_yd": round(old_d, 2), "new_yd": round(new_d, 2),
                    "exact_yd": None if exact is None else round(exact, 2),
                    "closed_yd": None if closed is None else round(closed, 2), "kind": kind})
    return out


def old_slope(path):
    """The old snapshot's slope panel (the 4th of oldcam-<pose>.png, 2x nearest, 4 pixels between panels)."""
    image = np.array(Image.open(path).convert("L"))
    return image[::2, 3 * 260:3 * 260 + 256:2]


def difference_image(old_depth, new_depth, old_class, new_class):
    """Black where the depth bytes agree, blue to red as they part, magenta where the class differs."""
    step = np.abs(old_depth.astype(int) - new_depth.astype(int))
    out = np.zeros(old_depth.shape + (3,), dtype=np.uint8)
    out[step == 1] = (40, 40, 140)
    out[step == 2] = (60, 120, 255)
    out[(step >= 3) & (step <= 5)] = (255, 220, 0)
    out[step > 5] = (255, 60, 0)
    out[old_class != new_class] = (255, 0, 255)
    return out


def pose_metrics(old_depth, new_depth, old_class, new_class):
    pixels = old_depth.size
    step = np.abs(old_depth.astype(int) - new_depth.astype(int))
    same_class = old_class == new_class
    both = same_class & (old_depth < 255) & (new_depth < 255)
    old_yd = yards(old_depth)
    new_yd = yards(new_depth)
    rel = np.abs(new_yd - old_yd)[both] / old_yd[both] if both.any() else np.array([])
    absd = np.abs(new_yd - old_yd)[both] if both.any() else np.array([])
    pairs = Counter()
    for o, n in zip(old_class[~same_class], new_class[~same_class]):
        pairs[f"{CLASS_NAMES[o] if o >= 0 else '?'} -> {CLASS_NAMES[n] if n >= 0 else '?'}"] += 1
    big = np.argwhere(step >= 2)
    return {
        "pixels": pixels,
        "same_class": float(same_class.mean()),
        "same_depth_byte": float((step == 0).mean()),
        "depth_within_1_step": float((step <= 1).mean()),
        "depth_within_3_steps": float((step <= 3).mean()),
        "rel_within_1pct": float((rel <= 0.01).mean()) if rel.size else None,
        "rel_within_5pct": float((rel <= 0.05).mean()) if rel.size else None,
        "rel_within_10pct": float((rel <= 0.10).mean()) if rel.size else None,
        "abs_diff_yd_mean": float(absd.mean()) if absd.size else None,
        "abs_diff_yd_median": float(np.median(absd)) if absd.size else None,
        "abs_diff_yd_max": float(absd.max()) if absd.size else None,
        "max_depth_step": int(step.max()),
        "class_mismatches": dict(pairs),
        "pixels_2_or_more_steps": [[int(r), int(c), int(old_depth[r, c]), int(new_depth[r, c])] for r, c in big],
    }


def panel(image, label, f):
    """A 3x nearest-neighbour panel with its label above."""
    big = Image.fromarray(image).resize((image.shape[1] * SCALE, image.shape[0] * SCALE), Image.NEAREST)
    out = Image.new("RGB", (big.width, big.height + 22), (20, 20, 20))
    out.paste(big, (0, 22))
    ImageDraw.Draw(out).text((4, 3), label, fill=(235, 235, 235), font=f)
    return out


def depth_rgb(depth):
    return np.stack([depth] * 3, axis=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", nargs="?", default="var/baked-camera-compare")
    parser.add_argument("--map", type=int, default=34)
    parser.add_argument("--old", default="old")
    parser.add_argument("--new", default="new")
    parser.add_argument("--out", default="compare")
    parser.add_argument("--scene", default=None)
    args = parser.parse_args()
    root = Path(args.root)
    scene = Path(args.scene) if args.scene else root / "data" / "scenes" / f"{args.map:03d}.scene"
    tris = load_scene_triangles(scene) if scene.exists() else None
    old_dir, new_dir = root / args.old, root / args.new
    old = json.loads((old_dir / "old_console.json").read_text())
    new = json.loads((new_dir / "new_timing.json").read_text())
    out = root / args.out
    out.mkdir(exist_ok=True)
    small, normal = font(13), font(15)
    metrics, rows, sheet_rows = {}, [], []
    for name, record in old.items():
        o_depth = read(old_dir / f"oldcam-{name}-depth.pgm")
        n_depth = read(new_dir / f"newcam-{name}-depth.pgm")
        o_kind = read(old_dir / f"oldcam-{name}-kind.ppm")
        n_kind = read(new_dir / f"newcam-{name}-kind.ppm")
        n_height = read(new_dir / f"newcam-{name}-height.pgm")
        o_class, n_class = classes_of(o_kind), classes_of(n_kind)
        o_slope = old_slope(old_dir / f"oldcam-{name}.png")
        n_slope = read(new_dir / f"newcam-{name}-normal.pgm")
        stat = pose_metrics(o_depth, n_depth, o_class, n_class)
        timing_old = old_timing(record["console"])
        timing_new = new["poses"][name]
        slope_step = np.abs(o_slope.astype(int) - n_slope.astype(int))
        stat["slope_same_byte"] = float((slope_step == 0).mean())
        stat["slope_within_2_bytes"] = float((slope_step <= 2).mean())
        stat["slope_within_10_bytes"] = float((slope_step <= 10).mean())
        stat["slope_mean_abs_byte"] = float(slope_step.mean())
        stat["slope_max_abs_byte"] = int(slope_step.max())
        stat["timing_old"], stat["timing_new"] = timing_old, timing_new
        if tris is not None:
            stat["attribution"] = attribute(tris, [float(v) for v in record["pose"]], stat["pixels_2_or_more_steps"])
        stat["speedup"] = timing_old["wall_us"] / timing_new["median_us"]
        metrics[name] = stat

        diff = difference_image(o_depth, n_depth, o_class, n_class)
        panels = [panel(o_kind, "old kind", small), panel(n_kind, "new kind", small),
                  panel(depth_rgb(o_depth), "old depth (near dark)", small),
                  panel(depth_rgb(n_depth), "new depth", small),
                  panel(depth_rgb(o_slope), "old slope (white floor, black wall)", small),
                  panel(depth_rgb(n_slope), "new slope", small),
                  panel(diff, "difference: black same byte, blue 1, light blue 2, yellow 3-5, red >5, magenta class", small)]
        gap = 8
        width = sum(p.width for p in panels) + gap * (len(panels) - 1)
        caption = 56
        image = Image.new("RGB", (width, panels[0].height + caption), (20, 20, 20))
        x = 0
        for p in panels:
            image.paste(p, (x, caption))
            x += p.width + gap
        pose = [float(v) for v in record["pose"]]
        draw = ImageDraw.Draw(image)
        draw.text((6, 4), f"{name}   map {args.map}, feet ({pose[0]}, {pose[1]}, {pose[2]}), yaw {pose[3]}, pitch {pose[4]}, "
                  f"zoom {pose[5]}   128x64, 120x60 deg", fill=(255, 255, 255), font=normal)
        draw.text((6, 26), f"old {timing_old['wall_us']:.0f} us ({timing_old['wall_us'] / timing_old['rays']:.2f} us/ray)   "
                  f"new median {timing_new['median_us']:.0f} us / min {timing_new['min_us']:.0f} us "
                  f"({timing_new['us_per_ray']:.3f} us/ray)   x{stat['speedup']:.1f}   |   same class "
                  f"{100 * stat['same_class']:.2f}%, same depth byte {100 * stat['same_depth_byte']:.2f}%, within one step "
                  f"{100 * stat['depth_within_1_step']:.2f}%", fill=(200, 230, 255), font=small)
        image.save(out / f"{name}-compare.png", optimize=True)
        sheet_rows.append(image)

        # The new frame on its own: kind, depth, height.
        trio = [panel(n_kind, "new kind", small), panel(depth_rgb(n_depth), "new depth", small),
                panel(depth_rgb(n_height), "new height (grey at the feet)", small)]
        solo = Image.new("RGB", (sum(p.width for p in trio) + 16, trio[0].height), (20, 20, 20))
        x = 0
        for p in trio:
            solo.paste(p, (x, 0))
            x += p.width + 8
        solo.save(new_dir / f"newcam-{name}.png", optimize=True)

    # Contact sheet: every pose, half size.
    half = [r.resize((r.width * 2 // 3, r.height * 2 // 3), Image.LANCZOS) for r in sheet_rows]
    sheet = Image.new("RGB", (half[0].width, sum(r.height for r in half)), (20, 20, 20))
    y = 0
    for r in half:
        sheet.paste(r, (0, y))
        y += r.height
    sheet.save(out / "contact-sheet.png", optimize=True)

    # Overall.
    names = list(metrics)
    pooled = Counter()
    for name in names:
        pooled.update(metrics[name]["class_mismatches"])
    overall = {key: statistics.mean(metrics[n][key] for n in names)
               for key in ("same_class", "same_depth_byte", "depth_within_1_step", "depth_within_3_steps")}
    overall["class_mismatches"] = dict(pooled)
    kinds = Counter(a["kind"] for n in names for a in metrics[n].get("attribution", []))
    overall["attribution_of_pixels_2_or_more_steps"] = dict(kinds)
    overall["worst_depth_step"] = max(metrics[n]["max_depth_step"] for n in names)
    overall["speedup_median"] = statistics.median(metrics[n]["speedup"] for n in names)
    metrics["overall"] = overall
    (out / "metrics.json").write_text(json.dumps(metrics, indent=1))

    lines = ["| pose | same class | same depth byte | within 1 step | within 3 steps | <=1% | <=5% | <=10% | mean / median / max |dt| yd |",
             "|---|---|---|---|---|---|---|---|---|"]
    for n in names:
        m = metrics[n]
        pct = lambda v: "-" if v is None else f"{100 * v:.2f}%"
        lines.append(f"| {n} | {pct(m['same_class'])} | {pct(m['same_depth_byte'])} | {pct(m['depth_within_1_step'])} | "
                     f"{pct(m['depth_within_3_steps'])} | {pct(m['rel_within_1pct'])} | {pct(m['rel_within_5pct'])} | "
                     f"{pct(m['rel_within_10pct'])} | {m['abs_diff_yd_mean']:.3f} / {m['abs_diff_yd_median']:.3f} / "
                     f"{m['abs_diff_yd_max']:.3f} |")
    lines += ["", "| pose | slope same byte | within 2 bytes | within 10 bytes | mean / max abs byte |", "|---|---|---|---|---|"]
    for n in names:
        m = metrics[n]
        lines.append(f"| {n} | {100 * m['slope_same_byte']:.2f}% | {100 * m['slope_within_2_bytes']:.2f}% | "
                     f"{100 * m['slope_within_10_bytes']:.2f}% | {m['slope_mean_abs_byte']:.3f} / {m['slope_max_abs_byte']} |")
    lines += ["", "| pose | old wall us | old trees / liquid / terrain us | new median us | new min us | new us/ray | "
              "new trace alone us | new reach us | new pixel dir+encode us | speed-up (old wall / new median) |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    for n in names:
        m = metrics[n]
        o, t = m["timing_old"], m["timing_new"]
        lines.append(f"| {n} | {o['wall_us']:.0f} | {o['static_and_dynamic_us']:.0f} / {o['liquid_us']:.0f} / "
                     f"{o['terrain_us']:.0f} | {t['median_us']:.0f} | {t['min_us']:.0f} | {t['us_per_ray']:.3f} | "
                     f"{t['trace_alone_us']:.0f} | {t['reach_alone_us']:.0f} | {t['pixel_alone_us']:.0f} | "
                     f"x{m['speedup']:.2f} |")
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    print("overall:", json.dumps(overall, indent=1))


if __name__ == "__main__":
    main()
