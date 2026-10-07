"""Every tuning key the sim reads is documented in the conf template.

The curriculum's tuning keys are declared once, in CurriculumTuning::Visit, and written out again in the conf
template. Keeping two lists in step by hand does not work -- by the time this test was written the template had
already drifted from the header, and five keys (Duel.Interrupt and its three variants, Pulls.ControlFallbackDps)
were readable but nowhere an operator could find them.

A missing key is not loud: CurriculumTuning::Load asks for every key with a default and no warning, on purpose,
so an absent one silently keeps its compiled-in value. That is the right behaviour for a running server and the
wrong one for finding out that a knob was never documented.

This test reads the module's own curriculum sources, so it checks what the module actually builds.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
CONF = ROOT / "src" / "server" / "apps" / "worldserver" / "worldserver.conf.dist"
TUNING = ROOT / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum" / "CurriculumTuning.h"


def tuning_keys() -> set[str]:
    """The keys CurriculumTuning::Visit hands to the config, e.g. "Goals.Match"."""
    return set(re.findall(r'f\("([A-Za-z0-9.]+)"', TUNING.read_text()))


def conf_keys() -> set[str]:
    """The Curriculum keys the template documents, without their prefix."""
    found = re.findall(r"^\s*[A-Za-z]+\.Curriculum\.([A-Za-z0-9.]+)\s*=", CONF.read_text(), re.M)
    return set(found)


def test_the_tuning_header_and_the_conf_template_are_readable():
    assert TUNING.is_file() and CONF.is_file()
    assert len(tuning_keys()) > 100


def test_every_tuning_key_is_in_the_conf_template():
    missing = sorted(tuning_keys() - conf_keys())
    assert not missing, (f"{len(missing)} tuning keys the sim reads are not in {CONF.name}, so nobody can find "
                         f"or set them: {', '.join(missing)}")


def test_the_conf_template_invents_no_keys():
    """A key in the template that the sim never reads is worse than none: it looks settable and does nothing."""
    extra = sorted(conf_keys() - tuning_keys())
    assert not extra, (f"{len(extra)} keys in {CONF.name} are not read by CurriculumTuning::Visit, so setting "
                       f"them does nothing: {', '.join(extra)}")
