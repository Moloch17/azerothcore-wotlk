"""The manual's tables against the files they describe.

Every stage budget in the manual was between 1x and 7x the configured value before this existed -- the party stage
was documented at 600M against a configured 120M, the crossroads at 1B against 150M. Numbers copied by hand
into prose drift silently and nobody notices until someone plans a run from them, so the table is checked instead
of trusted.
"""

import re
from pathlib import Path

CONFIGS = Path(__file__).resolve().parents[1] / "configs"
REPO = Path(__file__).resolve().parents[4]
MANUAL = REPO / "docs" / "forge" / "04-curriculum.md"

# `| `stage4_duel` | 50M | 10M | 2048 |` -- the table pairs two stages per row, so each line yields two.
ROW = re.compile(
    r"\|\s*`(?P<name>\w+)`\s*\|\s*(?P<budget>[\d.]+)M\s*\|\s*(?P<every>[\d.]+)M\s*\|"
    r"\s*(?P<episodes>\d+)\s*(?=\|)")


def documented() -> dict[str, dict]:
    out = {}
    for line in MANUAL.read_text().splitlines():
        for m in ROW.finditer(line):
            out[m.group("name")] = {
                "total_env_steps": int(float(m.group("budget")) * 1e6),
                "every_env_steps": int(float(m.group("every")) * 1e6),
                "episodes": int(m.group("episodes")),
            }
    return out


def test_the_table_covers_every_stage_config():
    """A stage added without a row is the way the table goes stale next. (configs/archive/ is not globbed: the
    archived curriculum's rows are history.)"""
    on_disk = {p.stem for p in CONFIGS.glob("*.yaml")} - {"fast"}
    assert on_disk - set(documented()) == set(), "these configs have no row in the manual's budget table"


# The budget table's other checks -- each row against its config, an evaluation's sim-time cost from Stages.cpp's
# episode lengths, and the queue total -- covered the first curriculum only, and went with it to the archive
# (2026-10-05, git tag curriculum-v1). The table they read now documents that archive; the movement stages bring a
# table of their own, and the checks come back against it.


# --------------------------------------------------------------------------- 8.2 tuning defaults

REFERENCE = REPO / "docs" / "forge" / "08-reference.md"
CONF_DIST = REPO / "src" / "server" / "apps" / "worldserver" / "worldserver.conf.dist"


def _template_defaults() -> dict[str, str]:
    text = CONF_DIST.read_text()
    return {m.group(1): m.group(2).strip().strip('"')
            for m in re.finditer(r"^AnimusForge\.Curriculum\.([\w.]+)\s*=\s*(.+?)\s*$", text, re.M)}


def _quick_reference() -> dict[str, str]:
    """Section 8.2's two-pairs-per-row table of the tuning values worth knowing."""
    text = REFERENCE.read_text()
    section = text.split("## 8.2 Curriculum tuning defaults", 1)[1].split("## 8.3", 1)[0]
    return {m.group(1): m.group(2).strip().strip('`"')
            for m in re.finditer(r"`([A-Z][\w.]*)`\s*\|\s*([^|]+?)\s*(?=\||$)", section, re.M)}


def test_the_quick_reference_only_lists_keys_that_exist():
    """test_conf_covers_tuning checks the template against the sim. This checks the manual against the template,
    which closes the loop: a key renamed in the code fails there, and a stale row here fails now."""
    unknown = sorted(set(_quick_reference()) - set(_template_defaults()))
    assert unknown == [], f"08-reference.md lists tuning keys the template does not define: {unknown}"


def test_the_quick_reference_defaults_are_the_templates():
    """Five of these were wrong when the table was last checked by hand -- Duel.Stall and Pulls.Stall at 0.05
    against a configured 0.08, both PreparationRefundMaxMs at 30000 against 15000, Travel.FastArrive at 3.0
    against 6.0. Numbers transcribed into prose drift; this is why the table is checked."""
    template = _template_defaults()
    wrong = {k: (v, template[k]) for k, v in _quick_reference().items() if k in template and v != template[k]}
    assert wrong == {}, f"08-reference.md disagrees with the template (manual, template): {wrong}"
