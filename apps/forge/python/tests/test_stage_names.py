"""Every stage name written anywhere is a stage that exists.

The curriculum has been renumbered more than once, and each time some file kept the old names: a conf template's
example queue, a manual section, a README, a shipped model manifest. Those strays cost nothing until somebody plans a
run from one. So every `stageN_name` token in the files that people read or the tools parse has to be a `.Name` in
Stages.cpp -- the one place the names are defined.

Lines carrying a date or a run citation (`at 20M`) are skipped: a comment quoting what a run of `stage1_duel` measured
names that run by the name it had, and rewriting it would destroy the citation. The same rule, applied uniformly to
conf templates and docs, is why C++ sources are not scanned at all (their citations are dense, their live names are
checked by the compiler through Stages.cpp).
"""

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[4]
STAGES_CPP = REPO / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum" / "Stages" / "Stages.cpp"
SIBLING_ANIMUS = REPO / "modules" / "mod-animus"

TOKEN = re.compile(r"\bstage\d+[a-z]?_[a-z_]+\b")
DATED = re.compile(r"20\d\d-\d\d-\d\d|\bat \d+(\.\d+)?M\b")


def defined_names() -> set[str]:
    names = set(re.findall(r'\.Name = "(stage\d+_\w+)"', STAGES_CPP.read_text()))
    assert len(names) >= 20, "Stages.cpp parsed badly"
    return names


def scanned_files() -> list[Path]:
    forge = REPO / "apps" / "forge"
    roots = [forge / "python", forge / "models", REPO / "docs" / "forge",
             REPO / "src" / "server" / "apps" / "worldserver" / "worldserver.conf.dist"]
    if SIBLING_ANIMUS.is_dir():
        roots += [SIBLING_ANIMUS / "conf", SIBLING_ANIMUS / "README.md", SIBLING_ANIMUS / "models",
                  SIBLING_ANIMUS / "src" / "AnimusConfig.cpp", SIBLING_ANIMUS / "src" / "AnimusConfig.h"]
    files = []
    for root in roots:
        if root.is_file():
            files.append(root)
        elif root.is_dir():
            files += [p for p in root.rglob("*")
                      if p.is_file() and p.suffix in (".py", ".yaml", ".md", ".dist", ".json", ".cpp", ".h")
                      and "__pycache__" not in p.parts and ".venv" not in p.parts
                      and p.name != "test_stage_names.py"]  # its RESERVED names are not stages yet
    return sorted(files)


def strays(path: Path, names: set[str]) -> list[str]:
    out = []
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if DATED.search(line):
            continue
        for token in TOKEN.findall(line):
            if token not in names:
                out.append(f"{path.relative_to(REPO)}:{number}: {token}")
    return out


@pytest.mark.parametrize("path", scanned_files(), ids=lambda p: str(p.relative_to(REPO)))
def test_every_stage_name_written_down_exists(path):
    found = strays(path, defined_names())
    assert found == [], "stage names that no longer exist:\n" + "\n".join(found)


def test_config_files_are_named_after_stages():
    names = defined_names()
    configs = {p.stem for p in (REPO / "apps" / "forge" / "python" / "configs").glob("stage*.yaml")}
    assert configs == names - set(), f"configs without a stage or stages without a config: {configs ^ names}"


# Numbers held for stages that are planned but not yet defined, so the stages around them do not have to move twice.
# A reserved number is filled by a stage of exactly this name; delete the entry when it lands. (Empty since the
# life stages of the 2026-09-24 plan landed.)
RESERVED: dict[int, str] = {}


def test_numbers_are_contiguous_and_in_seed_order():
    """A stage is never numbered below the stage it extends or merges, and the numbers run 1..N with no gap
    (a reserved number counts as filled)."""
    text = STAGES_CPP.read_text()
    entries = re.findall(r'\.Name = "(stage(\d+)_\w+)",(.*?)\n        \}\);', text, re.S)
    defined = {int(n): name for name, n, _ in entries}
    for number, name in RESERVED.items():
        assert defined.get(number, name) == name, f"{number} is reserved for {name}, not {defined[number]}"
    numbers = sorted(set(defined) | set(RESERVED))
    assert numbers == list(range(1, len(numbers) + 1)), f"numbers are not contiguous: {numbers}"
    number = {name: int(n) for name, n, _ in entries}
    for name, _, body in entries:
        parents = re.findall(r'"(stage\d+_\w+)"', body.split(".Summary")[0])
        for parent in parents:
            assert number[parent] < number[name], f"{name} is numbered below {parent}, which it seeds from"
