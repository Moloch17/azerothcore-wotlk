#!/usr/bin/env python3
"""Regenerate the AnimusForge.* key table in docs/forge/reference/config-keys.md.

What it reads (read-only), all inside the repository:
  * src/server/apps/worldserver/worldserver.conf.dist, the FORGE section (from "# FORGE" up to "# CURRICULUM TUNING"):
    the group of each key, its documented description and range, and the value the template assigns;
  * every .cpp / .h file under src/ and every .py file under apps/forge/forgectl and apps/forge/tools: the string
    literals "AnimusForge.<key>" and "Forge.<key>", which are the readers (path:line), and the default each reader
    passes (GetOption<T>("key", default) or the ranged(...) camera helper, whose low and high are the range).

What it writes: only the text between the two marker lines in docs/forge/reference/config-keys.md
    <!-- BEGIN GENERATED KEY TABLE (apps/forge/tools/gen_config_reference.py) -->
    <!-- END GENERATED KEY TABLE -->
Everything outside the markers is hand-written and is never touched. The AnimusForge.Curriculum.* keys are left out on
purpose (reference/cpp-tuning-keys.md documents them).

Run from anywhere:
    python3 apps/forge/tools/gen_config_reference.py            rewrite the table in place
    python3 apps/forge/tools/gen_config_reference.py --check    exit 1 if the file is out of date, write nothing
    python3 apps/forge/tools/gen_config_reference.py --stdout   print the table, write nothing

The two columns "same on every machine" and "in the fingerprint" are not derivable from the conf template; they come
from the SAME_RULES table below, which was written from AnimusForge.cpp (ClusterFingerprint, WorkerPlan,
DealClusterLearners) and docs/forge/deploy-gate.md. Edit the rules when those change; a key matching no rule shows
"per machine".
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CONF_DIST = ROOT / "src/server/apps/worldserver/worldserver.conf.dist"
OUT = ROOT / "docs/forge/reference/config-keys.md"
BEGIN = "<!-- BEGIN GENERATED KEY TABLE (apps/forge/tools/gen_config_reference.py) -->"
END = "<!-- END GENERATED KEY TABLE -->"

KEY = r"(?:Animus)?Forge\.[A-Za-z0-9_.<>]+"
INTRO_MARK = "# FORGE (the in-process training host"
STOP_MARK = "# CURRICULUM TUNING"

# (key regex, same on every machine, in the cluster fingerprint). First match wins.
SAME_RULES = [
    (r"AnimusForge\.DecisionMs", "yes, enforced (the fingerprint refuses a worker that differs)",
     "yes: decision=<DecisionMs>/<TicksPerDecision> (ClusterFingerprint in AnimusForge.cpp)"),
    (r"AnimusForge\.TicksPerDecision", "yes, enforced (the fingerprint refuses a worker that differs)",
     "yes: decision=<DecisionMs>/<TicksPerDecision> (ClusterFingerprint in AnimusForge.cpp)"),
    (r"AnimusForge\.Stage\.<name>\.TicksPerDecision|AnimusForge\.Stage\..*\.TicksPerDecision",
     "keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in "
     "AnimusForge.cpp)", "no"),
    (r"AnimusForge\.Stage\..*\.Envs", "no: per machine; a host's line caps a worker's count for that stage "
     "(DealClusterLearners and WorkerPlan in AnimusForge.cpp)", "no"),
    (r"AnimusForge\.Vision\.(Width|Height)", "yes (a mismatch is caught: the learner checks the SPEC image byte "
     "count; deploy-gate.md step 6)", "no"),
    (r"AnimusForge\.Vision\.(RenderSizes|FovH|FovV|Range|Zoom|Pitch|EvalVideos|EvalVideoScale)",
     "yes, by hand (nothing compares it; deploy-gate.md step 6)", "no"),
    (r"AnimusForge\.Vision\.Audit.*", "no: a local audit of the camera", "no"),
    (r"AnimusForge\.Map\..*|AnimusForge\.Memory\..*", "yes, by hand (nothing compares it; deploy-gate.md step 6)",
     "no"),
    (r"AnimusForge\.(Classes|EpisodeSeconds|ContinentReplicas|HalfBatch|SpawnPoint\..*)",
     "yes, by hand (what every episode is built from; deploy-gate.md step 6)", "no"),
    (r"AnimusForge\.Probe\.Dir", "the path is per machine; the data in it must match",
     "indirectly: the count and bytes of *.field files in it (fields=<n>/<bytes>)"),
    (r"AnimusForge\.Cluster\.(Host|DataPort|Advertise|Learner)", "per machine (worker keys)", "no"),
    (r"AnimusForge\.Cluster\.(ControlPort|DistPort|Sync)",
     "host key: every machine reads it, only a host uses it", "no"),
    (r"AnimusForge\.Cluster\.Role", "per machine (host or worker)", "no"),
    (r"(Animus)?Forge\.(Playtest|SealStrict)", "per machine", "no"),
]

# Validation the C++ applies that is not visible as a documented range: key -> text.
CODE_RULES = {
    "AnimusForge.Envs": "at least 1; capped at BotAccounts::MAX_ENVS, 1250 (also after the GPU mode scales it)",
    "AnimusForge.DecisionMs": "at least 1; rounded down to a multiple of TicksPerDecision, with an error",
    "AnimusForge.TicksPerDecision": "at least 1; capped at DecisionMs",
    "AnimusForge.HalfBatch": "needs TicksPerDecision 1 (error otherwise); an odd DecisionMs is lowered by 1",
    "AnimusForge.EpisodeSeconds": "at least 1",
    "AnimusForge.ReportEpisodes": "at least 1",
    "AnimusForge.Probe.CacheGrids": "at least 1 (the template says at least 9; the code does not enforce 9)",
    "AnimusForge.Cluster.Role": "standalone, host or worker (anything else: error, standalone); a worker with an "
                                "empty Host falls back to standalone",
    "AnimusForge.Cluster.Sync": "async or weights (anything else: error, async)",
    "AnimusForge.Cluster.Learner": "auto, 0/false, 1/true",
    "AnimusForge.Gpu.Mode": "auto, single, multi (anything else: error, auto)",
    "AnimusForge.Gpu.Multi.Learners": "capped at 16",
    "AnimusForge.Bench.MaxMemoryPercent": "clamped to 1..100",
    "AnimusForge.Bench.Threads": "a list of positive numbers, or auto",
    "AnimusForge.Bench.Envs": "a list of positive numbers, or auto",
    "AnimusForge.Fast.Envs": "at least 1",
    "AnimusForge.Fast.Budget": "at least 1000",
    "AnimusForge.Fast.OutputDir": "must not contain AnimusForge.OutputDir (error; falls back to <OutputDir>/fast)",
    "AnimusForge.Vision.RenderSizes": "entries that do not parse or exceed Width x Height are dropped with an "
                                      "error",
}

# Defaults a reader takes from a struct or a lambda rather than a literal: key -> value (read from the headers named).
STRUCT_DEFAULTS = {
    "AnimusForge.Vision.Width": "128",          # Animus::Vision::Settings, Vision/Camera.h
    "AnimusForge.Vision.Height": "64",
    "AnimusForge.Vision.FovH": "120",
    "AnimusForge.Vision.FovV": "60",
    "AnimusForge.Vision.Range": "100",
    "AnimusForge.Vision.Zoom": "6",
    "AnimusForge.Vision.Pitch": "-15",
    "AnimusForge.Vision.RenderSizes": '"32x16, 48x24, 64x32, 128x64:0.4"',   # DEFAULT_RENDER_SIZES, Vision/Camera.h
    "AnimusForge.Map.MaxTiles": "4096",         # MapSettings, Vision/MentalMap.h
    "AnimusForge.Map.CoarseTiles": "0",
    "AnimusForge.Map.KeepShare": "0.5",         # MapRunSettings, Vision/MentalMap.h
    "AnimusForge.Map.AgeOffsetSeconds": "600",
    "AnimusForge.Memory.MaxEntities": "64",     # MEMORY_TRAINING_CAP, Vision/EntityMemory.h
    "AnimusForge.Learner.TrainDevice": '"auto"',    # the placement lambda in ForgeConfig::Load
    "AnimusForge.Learner.RolloutDevice": '"auto"',
    "AnimusForge.Learner.Cpus": '"auto"',
    "AnimusForge.Bench.Threads": '"auto"',          # isAuto / GetNumberList in ForgeConfig::Load
    "AnimusForge.Bench.Envs": '"auto"',
}

# Keys whose reader builds the name at run time: key regex -> (file, line regex).
DYNAMIC = [
    (r"AnimusForge\.Gpu\.(Single|Multi)\.(\w+)", "src/server/game/Animus/ForgeConfig.cpp",
     r'prefix \+ "{2}"'),
    (r"AnimusForge\.Stage\..*\.Envs", "src/server/game/Animus/ForgeConfig.cpp", r'rfind\("\.Envs"\)'),
    (r"AnimusForge\.Stage\..*\.TicksPerDecision", "src/server/game/Animus/ForgeConfig.cpp",
     r'suffix = "\.TicksPerDecision"'),
]


def read_lines(path):
    return path.read_text(encoding="utf-8").split("\n")


def parse_conf():
    """Return (ordered keys, info) from the FORGE section of the template."""
    lines = read_lines(CONF_DIST)
    start = next(i for i, line in enumerate(lines) if line.startswith(INTRO_MARK))
    stop = next(i for i, line in enumerate(lines) if i > start and line.startswith(STOP_MARK))
    group = ""
    blocks = []            # {"keys": [...], "desc": [...], "default": [...], "active": {...}, "group": g}
    current = None
    previous_header = False
    seen_rule = False
    info = {}
    order = []
    for i in range(start, stop):
        line = lines[i]
        if line.startswith("#####"):
            seen_rule = True
            continue
        if seen_rule and re.match(r"^# [A-Z][A-Z ]+$", line):
            group = line[2:].strip().title()
            current = None
            seen_rule = False
            continue
        seen_rule = False
        header = re.match(r"^#    (" + KEY + r")((?:\s*/\s*\w+)*)\s*$", line)
        if header:
            names = [header.group(1)]
            for suffix in re.findall(r"/\s*(\w+)", header.group(2)):
                names.append(header.group(1).rsplit(".", 1)[0] + "." + suffix)
            if previous_header and current is not None and not current["active"]:
                current["keys"].extend(names)
            else:
                current = {"keys": names, "desc": [], "default": [], "active": {}, "group": group,
                           "line": i + 1, "mode": None}
                blocks.append(current)
            previous_header = True
            continue
        previous_header = False
        active = re.match(r"^(" + KEY + r")\s*=\s*(.*?)\s*$", line)
        if active:
            if current is not None:
                current["active"][active.group(1)] = active.group(2)
            key = active.group(1)
            info[key] = {"value": active.group(2), "block": current, "line": i + 1, "group": group}
            order.append(key)
            continue
        if line.startswith("#") and current is not None and not current["active"]:
            text = line[1:].rstrip()
            label = re.match(r"^\s{8}(Description|Default|Example|Values|Important|Note):\s*(.*)$", text)
            if label:
                current["mode"] = label.group(1)
                text = label.group(2)
            elif current["mode"] is None:
                continue
            elif current["mode"] == "Default" and re.match(r"^\s{21}\S", text):
                text = text.strip()
            else:
                text = text.strip()
            if current["mode"] == "Description":
                current["desc"].append(text)
            elif current["mode"] == "Default":
                current["default"].append(text)
    # An assignment belongs to the block that lists its key in its header, else to the block it follows.
    listed = {key: block for block in blocks for key in block["keys"]}
    for key, entry in info.items():
        if key in listed:
            entry["block"] = listed[key]
    # documented keys with no assignment (the Stage.<name>.Envs placeholder)
    for block in blocks:
        for key in block["keys"]:
            if key not in info:
                info[key] = {"value": None, "block": block, "line": block["line"], "group": block["group"]}
                order.append(key)
    order.sort(key=lambda key: info[key]["line"])
    return order, info


def block_for(key, info):
    entry = info.get(key)
    if entry and entry["block"] is not None:
        return entry["block"]
    # a Stage.<name>.X assignment shares the placeholder's documentation
    generic = re.sub(r"^(AnimusForge\.Stage\.)[^.]+(\..*)$", r"\1<name>\2", key)
    entry = info.get(generic)
    return entry["block"] if entry else None


def scan_readers():
    """Map key -> [(path, line, text)] from string literals; and the raw source lines per file."""
    readers = {}
    files = []
    for pattern in ("src/**/*.cpp", "src/**/*.h"):
        files.extend(ROOT.glob(pattern))
    for pattern in ("apps/forge/forgectl/*.py", "apps/forge/tools/*.py"):
        files.extend(ROOT.glob(pattern))
    cache = {}
    skip = {OUT.resolve(), Path(__file__).resolve()}
    for path in sorted(set(files)):
        if path.resolve() in skip or "src/test" in path.as_posix():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace").split("\n")
        except OSError:
            continue
        cache[path] = text
        for number, line in enumerate(text, 1):
            for match in re.finditer(r'"((?:Animus)?Forge\.[A-Za-z0-9_.]+)"', line):
                key = match.group(1)
                if "Curriculum" in key:
                    continue
                readers.setdefault(key, []).append((path.relative_to(ROOT).as_posix(), number, "\n".join(
                    text[number - 1:number + 1])))
    return readers, cache


def dynamic_readers(key, cache):
    found = []
    for pattern, rel, line_pattern in DYNAMIC:
        match = re.fullmatch(pattern, key)
        if not match:
            continue
        regex = line_pattern
        if "{" in regex and match.groups():
            regex = regex.replace("{2}", match.group(match.lastindex))
        path = ROOT / rel
        for number, line in enumerate(cache.get(path) or read_lines(path), 1):
            if re.search(regex, line):
                found.append((rel, number, line))
    return found


def code_default(key, sites):
    """The default the first reader passes, and the (low, high) of a ranged() reader."""
    for _, _, text in sites:
        first = text.split("\n")[0]
        joined = text.replace("\n", " ")
        ranged = re.search(r'ranged\("' + re.escape(key) + r'",\s*([^,]+(?:\([^)]*\))?[^,]*),\s*([-\d.]+f?),\s*'
                           r'([-\d.]+f?)', joined)
        if ranged:
            return ranged.group(1).strip(), ranged.group(2).rstrip("f") + " to " + ranged.group(3).rstrip("f")
        option = re.search(r'GetOption<[^>]+>\(\s*"' + re.escape(key) + r'"\s*,\s*([^,)]+(?:\([^)]*\))?[^,)]*)',
                           joined)
        if option and "ranged" not in first:
            return option.group(1).strip(), ""
        listing = re.search(r'GetList\("' + re.escape(key) + r'"\s*(?:,\s*("[^"]*"))?', joined)
        if listing:
            return listing.group(1) or '""', ""
    return "", ""


def clean_value(value):
    if value is None:
        return "(no line)"
    return value if value else "(empty)"


def first_sentence(lines, limit=230):
    text = " ".join(part for part in lines if part).strip()
    text = re.sub(r"^(Worker|Host|Worker/host):\s*", "", text)
    match = re.match(r"(.+?[a-z0-9)\"`>]\.)(\s+[A-Z`(]|$)", text)
    text = match.group(1) if match else text
    text = text.replace("|", "/")
    return text if len(text) <= limit else text[:limit - 1].rstrip() + "..."


def doc_range(block):
    if block is None:
        return ""
    text = " ".join(block["default"])
    found = re.findall(r"\((-?[\d.]+ to -?[\d.]+)\)", text)
    return found[0] if found else ""


def rule_for(key):
    for pattern, same, fingerprint in SAME_RULES:
        if re.fullmatch(pattern, key):
            return same, fingerprint
    return "per machine", "no"


def builds_table():
    order, info = parse_conf()
    readers, cache = scan_readers()
    rows = []
    mismatches = []
    unread = []
    for key in order:
        entry = info[key]
        block = block_for(key, info)
        sites = list(readers.get(key, []))
        if not sites:
            sites = dynamic_readers(key, cache)
        if not sites:
            generic = re.sub(r"^(AnimusForge\.Stage\.)[^.]+(\..*)$", r"\1x\2", key)
            sites = dynamic_readers(generic.replace(".x.", ".zz."), cache) or dynamic_readers(key, cache)
        default, ranged = code_default(key, sites)
        default = STRUCT_DEFAULTS.get(key, default)
        if not default and re.match(r"AnimusForge\.Gpu\.", key):
            default = "0" if re.search(r"\.(Envs|Minibatches|Learners)$", key) else '""'
        if key == "AnimusForge.Stage.<name>.Envs":
            default = "AnimusForge.Envs"
        range_text = ranged or doc_range(block) or CODE_RULES.get(key, "")
        extra = CODE_RULES.get(key, "")
        if extra and extra != range_text:
            range_text = (range_text + "; " if range_text else "") + extra
        if not sites:
            unread.append(key)
        def label(site):
            path, number, text = site
            tag = " (writes it)" if "WriteConfigValue" in text.split("\n")[0] else \
                " (tool, reads a conf file)" if path.endswith(".py") else ""
            return f"{path}:{number}{tag}"
        where = ", ".join(label(site) for site in sites[:3]) + (f" (+{len(sites) - 3})" if len(sites) > 3 else "")
        same, fingerprint = rule_for(key)
        value = clean_value(entry["value"])
        template_default = re.sub(r'^"(.*)"$', r"\1", value)
        code_value = re.sub(r'^"(.*)"$', r"\1", default).rstrip("f").rstrip("u")
        code_value = re.sub(r"^(?:float|uint32|uint64)\((.*)\)$", r"\1", code_value)
        if default and entry["value"] not in (None, "") and re.fullmatch(r"-?[\d.]+", template_default) \
                and re.fullmatch(r"-?[\d.]+", code_value):
            if float(template_default) != float(code_value):
                mismatches.append((key, value, default, sites[0][0] + ":" + str(sites[0][1])))
        meaning = first_sentence(block["desc"]) if block else ""
        rows.append((entry["group"], key, f"`{value}`" if entry["value"] is not None else value,
                     f"`{default}`" if default else "-", range_text.replace("|", "/") or "-", where or "none found",
                     same, fingerprint, meaning))
    return rows, mismatches, unread


def render():
    rows, mismatches, unread = builds_table()
    out = [BEGIN, "",
           "Generated from the FORGE section of `src/server/apps/worldserver/worldserver.conf.dist` and the",
           "string literals in the sources; do not edit by hand",
           f"(run `python3 apps/forge/tools/gen_config_reference.py`). {len(rows)} keys.", "",
           "| Group | Key | Template value | Code default | Range / validation | Reader | Same on every machine | "
           "In the cluster fingerprint | Meaning |",
           "|---|---|---|---|---|---|---|---|---|"]
    for row in rows:
        out.append("| " + " | ".join(row) + " |")
    out.append("")
    out.append("Template value differs from the code default (numeric keys, the template line is what a copied "
               "conf.dist starts with):")
    out.append("")
    if mismatches:
        out.append("| Key | Template | Code default | Reader |")
        out.append("|---|---|---|---|")
        for key, value, default, where in mismatches:
            out.append(f"| {key} | `{value}` | `{default}` | {where} |")
    else:
        out.append("None.")
    out.append("")
    out.append("Keys documented in the template with no reader found by the literal scan: "
               + (", ".join(f"`{k}`" for k in unread) if unread else "none") + ".")
    out.append("")
    out.append(END)
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--check", action="store_true", help="exit 1 if the table in the file is out of date")
    parser.add_argument("--stdout", action="store_true", help="print the table instead of writing the file")
    args = parser.parse_args()

    table = render()
    if args.stdout:
        print(table)
        return 0
    text = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
    if BEGIN not in text or END not in text:
        print(f"{OUT} has no marker lines; add:\n{BEGIN}\n{END}", file=sys.stderr)
        return 2
    before, rest = text.split(BEGIN, 1)
    _, after = rest.split(END, 1)
    new = before + table + after
    if args.check:
        if new != text:
            print("config-keys.md is out of date: run apps/forge/tools/gen_config_reference.py", file=sys.stderr)
            return 1
        return 0
    if new != text:
        OUT.write_text(new, encoding="utf-8")
        print(f"wrote {OUT.relative_to(ROOT)}")
    else:
        print("already up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
