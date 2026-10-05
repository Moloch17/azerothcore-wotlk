"""Every stage name written anywhere is a stage that exists.

The curriculum has been renumbered more than once, and each time some file kept the old names: a conf template's
example queue, a manual section, a README, a shipped model manifest. Those strays cost nothing until somebody plans a
run from one. So every `stageN_name` token in the files that people read or the tools parse has to be a `.Name` in
Stages.cpp -- the one place the names are defined.

Lines carrying a date or a run citation (`at 20M`) are skipped: a comment quoting what a run of `stage1_duel` measured
names that run by the name it had, and rewriting it would destroy the citation. The same rule, applied uniformly to
conf templates and docs, is why C++ sources are not scanned at all (their citations are dense, their live names are
checked by the compiler through Stages.cpp).

**The archived curriculum** (stage1_move ... stage21_ship, archived 2026-10-05: git tag `curriculum-v1`,
configs/archive/) is cited everywhere -- the manual, comments, the conf template's tuning notes. Its names are
citations of that curriculum, as a dated line is of a run, and are allowed as such: an archived name is a stray only
where a value is set from it (a conf template line `AnimusForge.<key> = ...`), since that is a run planned from a
stage that no longer exists. A name that is neither defined nor archived is a stray everywhere.
"""

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[4]
STAGES_CPP = REPO / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum" / "Stages" / "Stages.cpp"
SIBLING_ANIMUS = REPO / "modules" / "mod-animus"

TOKEN = re.compile(r"\bstage\d+[a-z]?_[a-z_]+\b")
DATED = re.compile(r"20\d\d-\d\d-\d\d|\bat \d+(\.\d+)?M\b")
# A conf template line that sets a value: the one place an archived name is still a stray.
SETTING = re.compile(r"^\s*AnimusForge\.\S+\s*=")
CONFIGS = REPO / "apps" / "forge" / "python" / "configs"
ARCHIVE = CONFIGS / "archive"


def defined_names() -> set[str]:
    """The stages Stages.cpp defines (none between the archive and the movement stages): a `.Name` followed by
    a `.Suffix`, which an arena's never is."""
    assert STAGES_CPP.is_file()
    return set(re.findall(r'\.Name = "(\w+)",\s*\.Suffix', STAGES_CPP.read_text()))


def archived_names() -> set[str]:
    """The archived curriculum's stages: one config each in configs/archive/ (per-class ones included)."""
    names = {p.stem for p in ARCHIVE.rglob("stage*.yaml")}
    assert len(names) >= 15, "configs/archive/ parsed badly"
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
                      and ARCHIVE not in p.parents  # the archived curriculum's own files
                      and p.name != "test_stage_names.py"]
    return sorted(files)


def trained_model(path: Path) -> bool:
    """An exported model's manifest: its `stage` records the stage that trained it -- a citation of that run, like a
    dated line -- and stays until the models are replaced by a run of the current curriculum."""
    return path.suffix == ".json" and "models" in path.parts and path.read_text(errors="replace").startswith(
        '{"format"')


def strays(path: Path, names: set[str], archived: set[str], root: Path = REPO) -> list[str]:
    out = []
    if trained_model(path):
        return out
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if DATED.search(line):
            continue
        setting = SETTING.match(line) is not None
        for token in TOKEN.findall(line):
            if token not in names and (setting or token not in archived):
                out.append(f"{path.relative_to(root)}:{number}: {token}")
    return out


@pytest.mark.parametrize("path", scanned_files(), ids=lambda p: str(p.relative_to(REPO)))
def test_every_stage_name_written_down_exists(path):
    found = strays(path, defined_names(), archived_names())
    assert found == [], "stage names that no longer exist:\n" + "\n".join(found)


def test_an_archived_name_is_a_stray_where_a_value_is_set_from_it(tmp_path):
    conf = tmp_path / "worldserver.conf.dist"
    conf.write_text("# stage4_duel was the bench default\nAnimusForge.Bench.Scenario = \"stage4_duel\"\n"
                    "# stage99_nothing never existed\n")
    found = strays(conf, set(), {"stage4_duel"}, root=tmp_path)
    assert found == ["worldserver.conf.dist:2: stage4_duel", "worldserver.conf.dist:3: stage99_nothing"]


def test_config_files_are_named_after_stages():
    """Every stage has its learner config in configs/, and every config there is a stage's (fast.yaml is an
    overlay, archive/ the archived curriculum's)."""
    names = defined_names()
    configs = {p.stem for p in CONFIGS.glob("*.yaml")} - {"fast"}
    assert configs == names, f"configs without a stage or stages without a config: {configs ^ names}"
