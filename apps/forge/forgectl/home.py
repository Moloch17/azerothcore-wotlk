"""forgectl's own directory on the machine it is run from: the audit log and the console locks live under it."""
from __future__ import annotations

import os
from pathlib import Path


def forgectl_home() -> Path:
    """~/.forgectl, or $FORGECTL_HOME (the tests point it at a temporary directory)."""
    return Path(os.environ.get("FORGECTL_HOME") or Path.home() / ".forgectl")
