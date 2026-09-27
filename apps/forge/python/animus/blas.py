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
    # TunableOp writes what it found when the process ends; a learner stopped by its sim ends through atexit too.
    atexit.register(torch.cuda.tunable.write_file)
    print(f"Learner: {arch} matmuls on rocBLAS, tuned by TunableOp (hipBLASLt has no fast kernels for it); "
          f"tunings in {directory}", flush=True)
    return arch
