"""The sim's device buffers (protocol 15): obs, state and mask written by the worldserver into GPU memory this learner
reads as tensors, instead of sent over the socket.

The worldserver allocates them with its device library, which runs on this torch's own HIP runtime (device memory
exported by one HIP runtime only opens in the same one), exports their IPC handles in a DEVICE message after SPEC, and
writes each group's rows before its STEP. The learner opens them here through that runtime's hipIpcOpenMemHandle and
wraps them as tensors without copying; each STEP's obs, state and mask are views of them, valid until the sim writes
that group's next STEP -- which it does only after the ACT for it, so a decision reads them before it answers.

ROCm's legacy IPC mode cannot export or open here: both processes need HSA_ENABLE_IPC_MODE_LEGACY=0, which the
worldserver sets for the learners it spawns and animus/__init__.py sets for one started by hand, before torch loads.
"""

from __future__ import annotations

import ctypes
import glob
import os

import numpy as np

HANDLE_BYTES = 64


def host(array):
    """`array` as a numpy array: a device tensor copied down, anything else as it is."""
    if hasattr(array, "detach"):
        return array.detach().cpu().numpy()
    return array


class _Handle(ctypes.Structure):
    _fields_ = [("reserved", ctypes.c_char * HANDLE_BYTES)]


class _View:
    """A device pointer torch can wrap without copying (__cuda_array_interface__)."""

    def __init__(self, pointer: int, shape: tuple[int, ...], typestr: str):
        self.__cuda_array_interface__ = {"shape": shape, "typestr": typestr, "data": (pointer, False), "version": 2}


class DeviceBuffers:
    """obs [E, A, O] float32, state [E, S] float32 and mask [E, A, N] bool, on the sim's side of GPU `device`."""

    def __init__(self, device: int, envs: int, agents: int, obs_dim: int, state_dim: int, num_actions: int,
                 handles: tuple[bytes, bytes, bytes]):
        import torch

        self._runtime = ctypes.CDLL(glob.glob(os.path.join(os.path.dirname(torch.__file__), "lib",
                                                           "libamdhip64.so*"))[0])
        self._runtime.hipIpcOpenMemHandle.argtypes = [ctypes.POINTER(ctypes.c_void_p), _Handle, ctypes.c_uint]
        self._runtime.hipIpcCloseMemHandle.argtypes = [ctypes.c_void_p]
        self.device = torch.device("cuda", device)
        self._pointers: list[int] = []
        shapes = ((envs, agents, obs_dim), (envs, state_dim), (envs, agents, num_actions))
        types = ("<f4", "<f4", "|b1")
        tensors = []
        with torch.cuda.device(self.device):
            for handle, shape, typestr in zip(handles, shapes, types):
                pointer = ctypes.c_void_p()
                # 1: hipIpcMemLazyEnablePeerAccess, the only flag there is.
                status = self._runtime.hipIpcOpenMemHandle(ctypes.byref(pointer), _Handle.from_buffer_copy(handle), 1)
                if status != 0 or not pointer.value:
                    self.close()
                    raise OSError(f"hipIpcOpenMemHandle failed ({status})")
                self._pointers.append(pointer.value)
                tensors.append(torch.as_tensor(_View(pointer.value, shape, typestr), device=self.device))
        self.obs, self.state, self.mask = tensors

    def rows(self, begin: int, count: int):
        """(obs, state, mask) of envs [begin, begin + count), as views."""
        end = begin + count
        return self.obs[begin:end], self.state[begin:end], self.mask[begin:end]

    def close(self) -> None:
        for pointer in self._pointers:
            self._runtime.hipIpcCloseMemHandle(ctypes.c_void_p(pointer))
        self._pointers = []


def open_buffers(spec, device: int, envs: int, handles: tuple[bytes, bytes, bytes],
                 rollout_device: str | None) -> tuple[DeviceBuffers | None, str]:
    """The sim's buffers when this learner can use them: its rollouts run on that GPU, under a HIP torch. Otherwise
    None and why (the learner then declines, and the sim keeps sending them over the socket)."""
    try:
        import torch
    except ImportError:
        return None, "no torch"
    if not rollout_device or not str(rollout_device).startswith("cuda") or not torch.cuda.is_available():
        return None, f"rollouts run on {rollout_device}, not the GPU"
    if not torch.version.hip:
        return None, "torch is not a HIP build"
    wanted = torch.device(rollout_device)
    index = wanted.index if wanted.index is not None else torch.cuda.current_device()
    if index != device:
        return None, f"the sim's buffers are on GPU {device}, rollouts on {index}"
    if envs != spec.num_envs:
        return None, f"the buffers hold {envs} envs, the spec {spec.num_envs}"
    try:
        return DeviceBuffers(device, envs, spec.agents_per_env, spec.obs_dim, spec.state_dim, spec.num_actions,
                             handles), ""
    except OSError as error:
        return None, str(error)

