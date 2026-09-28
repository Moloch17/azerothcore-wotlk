"""The matrix library a learner's updates run on, where the default is a bad one.

ROCm routes a linear layer's matmul-plus-bias through hipBLASLt, and on RDNA4 (gfx12) hipBLASLt has no tuned fp32
kernels: it falls back to 8x8 tiles, and a learner step spent 95% of its time there -- 111 ms for a four-layer MLP
update that takes 22 ms on an RTX 3060 Ti and 32 ms on the dev machine's card, with plain matmuls on the same card
running at full speed. Measured on the cluster's RX 9060 XTs: it is the same on torch 2.9.1+rocm6.4, 2.10+rocm7.0 and
2.13+rocm7.1. Sending those matmuls to rocBLAS instead and letting TunableOp pick rocBLAS's best kernel for each shape
brings it to 28 ms. In a cluster every rank waits for the slowest learner each update, so one such card held the
whole run to its pace.

TunableOp times the candidates the first time it meets a shape, a few seconds spread over a learner's first updates,
and keeps what it found in var/animus-forge/tunableop/<arch>_<device>.csv, so a learner started again on that card
does not tune again.
"""
from __future__ import annotations

import atexit
import os
from pathlib import Path


_tuning = False
_saves = 0


# Updates that may tune before tuning stops. The update's shapes depend on how many rows each class has in a
# minibatch, so a learner that tunes for ever meets new ones every update: on the RX 9060 XTs the tuning file reached
# 114,888 shapes, each timed for seconds and never freed -- a learner grew about 400 MB a minute, spencer's (16 GB)
# was killed by the OOM killer before every stage ended, and the tuning itself made their updates 5-7 s against the
# others' 1 (2026-09-28). The shapes that recur are met in the first updates; after these, what was found is used and
# anything new runs on rocBLAS's default kernel.
TUNING_UPDATES = 20


def save() -> None:
    """Write the tunings found so far after each of the first TUNING_UPDATES updates, then stop tuning. Nothing
    where prepare did not enable TunableOp."""
    global _saves
    if not _tuning or _saves > TUNING_UPDATES:
        return
    _saves += 1
    import torch
    torch.cuda.tunable.write_file()
    if _saves > TUNING_UPDATES:
        torch.cuda.tunable.tuning_enable(False)
        print(f"Learner: TunableOp stops tuning after {TUNING_UPDATES} updates; new shapes use rocBLAS's default",
              flush=True)


def prepare(device: str) -> str | None:
    """Set the matrix library up for `device` before anything runs on it; the architecture it did this for, or
    None where the default is the right one (CUDA, the CPU, ROCm cards before gfx12)."""
    import torch

    if not device.startswith("cuda") or not torch.version.hip or not torch.cuda.is_available():
        return None
    index = torch.device(device).index or 0
    arch = torch.cuda.get_device_properties(index).gcnArchName.split(":")[0]
    if not arch.startswith("gfx12"):
        return None

    # Read once, at the first addmm: set before the model does anything.
    os.environ.setdefault("DISABLE_ADDMM_CUDA_LT", "1")
    torch.backends.cuda.preferred_blas_library("cublas")        # rocBLAS, on ROCm
    directory = Path(os.environ.get(
        "ANIMUS_TUNABLEOP_DIR", Path(__file__).resolve().parents[4] / "var" / "animus-forge" / "tunableop"))
    directory.mkdir(parents=True, exist_ok=True)
    torch.cuda.tunable.set_filename(str(directory / f"{arch}_.csv"), insert_device_ordinal=True)
    torch.cuda.tunable.enable(True)
    torch.cuda.tunable.tuning_enable(True)
    # TunableOp writes what it found when the process ends, but a learner its sim stops is killed before that;
    # save() after each update writes it as it grows (atexit catches a clean exit).
    atexit.register(torch.cuda.tunable.write_file)
    global _tuning
    _tuning = True
    print(f"Learner: {arch} matmuls on rocBLAS, tuned by TunableOp (hipBLASLt has no fast kernels for it); "
          f"tunings in {directory}", flush=True)
    return arch
