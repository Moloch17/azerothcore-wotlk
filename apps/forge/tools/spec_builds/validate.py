"""Check every build in builds.py against the client's talent data.

    python3 tools/spec_builds/validate.py <dbc dir>

<dbc dir> holds Spell.dbc, Talent.dbc and TalentTab.dbc (the server's data/dbc). Each build is spent point by
point as TalentBuilder::Standard spends it -- every point to the first talent in list order that still wants ranks
and may take one (5 points per row in its tree, its prerequisite at the required rank) -- and must spend exactly 71
points and reach every rank it asks for.
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from builds import BUILDS  # noqa: E402

TOTAL = 71
POINTS_PER_ROW = 5
CLASSES = {1: "warrior", 2: "paladin", 4: "hunter", 8: "rogue", 16: "priest", 32: "deathknight", 64: "shaman",
           128: "mage", 256: "warlock", 1024: "druid"}


def load(path):
    data = path.read_bytes()
    _, rows, fields, size, _ = struct.unpack("<4s4I", data[:20])
    strings = data[20 + rows * size:]
    return [struct.unpack_from(f"<{fields}I", data, 20 + i * size) for i in range(rows)], strings


def text(strings, offset):
    return strings[offset:strings.index(b"\0", offset)].decode("utf-8", "replace")


def main(dbc):
    spells, spell_strings = load(dbc / "Spell.dbc")
    spell_name = {row[0]: text(spell_strings, row[136]) for row in spells}
    tabs, _ = load(dbc / "TalentTab.dbc")
    tab_of = {row[0]: (CLASSES[row[20]], row[22]) for row in tabs if row[20] in CLASSES}

    talents, _ = load(dbc / "Talent.dbc")
    by_name, by_id = {}, {}
    for row in talents:
        if row[1] not in tab_of:
            continue
        cls, page = tab_of[row[1]]
        ranks = [spell for spell in row[4:13] if spell]
        talent = {"id": row[0], "row": row[2], "max": len(ranks), "dep": row[13], "dep_rank": row[16], "page": page}
        by_name[(cls, page, spell_name[ranks[0]])] = talent
        by_id[row[0]] = talent

    ok = True
    for (cls, spec, _), build in BUILDS.items():
        problems, entries, wanted = [], [], {}
        for page, name, ranks in build:
            talent = by_name.get((cls, page, name))
            if not talent:
                problems.append(f"unknown talent {page}:{name}")
                continue
            if ranks:
                wanted[talent["id"]] = wanted.get(talent["id"], 0) + ranks
                entries.append((talent, name))

        rank, tree, spent = {}, [0, 0, 0], 0
        while spent < TOTAL:
            for talent, _ in entries:
                have = rank.get(talent["id"], 0)
                if (have < min(wanted[talent["id"]], talent["max"])
                        and tree[talent["page"]] >= talent["row"] * POINTS_PER_ROW
                        and (talent["dep"] not in by_id or rank.get(talent["dep"], 0) > talent["dep_rank"])):
                    rank[talent["id"]] = have + 1
                    tree[talent["page"]] += 1
                    spent += 1
                    break
            else:
                break

        missing = [name for talent, name in entries if rank.get(talent["id"], 0) < wanted[talent["id"]]]
        if missing:
            problems.append("not reached: " + ", ".join(dict.fromkeys(missing)))
        if spent != TOTAL:
            problems.append(f"spends {spent} of {TOTAL}")

        ok &= not problems
        print(f"{'OK ' if not problems else 'BAD'} {cls}/{spec}: {tree[0]}/{tree[1]}/{tree[2]}"
              + ("" if not problems else "  " + "; ".join(problems)))

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(Path(sys.argv[1])))
