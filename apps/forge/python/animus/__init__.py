"""Python side of the forge training host: the sim client and the MAPPO learner."""

import os

# Device buffers shared with the sim (animus.device) open only in ROCm's non-legacy IPC mode, and it is read when the
# HIP runtime starts: here, before anything imports torch. The worldserver sets it for the learners it spawns.
os.environ.setdefault("HSA_ENABLE_IPC_MODE_LEGACY", "0")
