"""Data-parallel learners: several learner processes, one per GPU, training one policy.

Each rank owns a share of the envs (its share of the host sim's pool -- AnimusForge.Learner.Ranks -- and of the
cluster's worker sims), collects rollouts on its own GPU and computes gradients on them; the gradients are averaged
across ranks before every optimizer step, so every rank's networks stay the same networks. What the ranks learn
from together -- the normalisers' statistics, the KL an epoch is stopped on -- is reduced the same way.

Rank 0 is the leader: it alone writes the run (checkpoints, metrics, progress, evaluations, finished.json), decides
what the controller decides, and sends the sim the layout weights and replay seeds. The others follow what it
broadcasts. With one rank every call here is a no-op and nothing is imported from torch.distributed.

A slow rank stalls the others at every all-reduce: the ranks move together, one update at a time.
"""

from __future__ import annotations

import os

import torch


class Ranks:
    def __init__(self, rank: int = 0, world: int = 1, address: str = "127.0.0.1:29500", device: str = "cpu"):
        self.rank = rank
        self.world = max(1, world)
        self.leader = rank == 0
        self.active = self.world > 1
        self.backend = None
        self.group = None           # the process group this view's collectives use; None = the default one
        self.update = self          # the trainer's view (update_view): its own group
        if not self.active:
            return

        import torch.distributed as dist

        host, _, port = address.rpartition(":")
        os.environ.setdefault("MASTER_ADDR", host or "127.0.0.1")
        os.environ.setdefault("MASTER_PORT", port or "29500")
        # The GPUs' own collective library when every rank has a GPU of its own (it refuses two ranks on one card);
        # otherwise gloo, which reduces host copies -- slower, but it runs anywhere.
        own_gpus = (device.startswith("cuda") and torch.cuda.is_available()
                    and torch.cuda.device_count() >= self.world and dist.is_nccl_available())
        self.backend = "nccl" if own_gpus else "gloo"
        if own_gpus:
            # NCCL's object collectives (broadcast, gather) stage through the current device: each rank's own.
            torch.cuda.set_device(torch.device(device))
        dist.init_process_group(self.backend, rank=rank, world_size=self.world)
        self._dist = dist
        # The update runs on the overlap worker thread while the run's own thread broadcasts and gathers: on one
        # group the two threads' collectives could meet in a different order on each rank and wait on each other
        # for ever. The trainer gets a group of its own, so each thread's collectives are ordered among themselves.
        update = object.__new__(Ranks)
        update.__dict__.update(self.__dict__)
        update.group = dist.new_group(backend=self.backend)
        update.update = update
        self.update = update
        print(f"Data-parallel: rank {rank} of {self.world} ({self.backend})", flush=True)

    # ------------------------------------------------------------------ tensors

    def _reduce(self, tensor: torch.Tensor) -> torch.Tensor:
        """Sum `tensor` over the ranks, in place where the backend can, on the tensor's own device either way."""
        if self.backend == "gloo" and tensor.device.type != "cpu":
            host = tensor.detach().cpu()
            self._dist.all_reduce(host, group=self.group)
            tensor.copy_(host)
        else:
            self._dist.all_reduce(tensor, group=self.group)
        return tensor

    def sum(self, tensor: torch.Tensor) -> torch.Tensor:
        """`tensor` summed over the ranks (a new tensor; the input is left as it was)."""
        if not self.active:
            return tensor
        return self._reduce(tensor.detach().clone())

    def mean(self, tensor: torch.Tensor) -> torch.Tensor:
        return self.sum(tensor) / self.world if self.active else tensor

    def average_gradients(self, parameters) -> None:
        """Every rank's gradients replaced by their mean over the ranks: one flat all-reduce per call.

        Every parameter goes in, whether or not it has a gradient here: a layout head no row of this rank's minibatch
        used has none on this rank but may on another, and the ranks' buffers must line up. A presence count rides
        along, so a parameter no rank touched is left without a gradient, as one learner would leave it."""
        if not self.active:
            return
        parameters = [p for p in parameters if p.requires_grad]
        if not parameters:
            return
        device = parameters[0].device
        flat = torch.cat([(p.grad if p.grad is not None else torch.zeros_like(p)).reshape(-1) for p in parameters]
                         + [torch.tensor([float(p.grad is not None) for p in parameters], device=device,
                                         dtype=parameters[0].dtype)])
        self._reduce(flat)
        flat[:-len(parameters)] /= self.world
        present = flat[-len(parameters):].tolist()
        offset = 0
        for parameter, count in zip(parameters, present):
            size = parameter.numel()
            if count > 0:
                if parameter.grad is None:
                    parameter.grad = torch.empty_like(parameter)
                parameter.grad.copy_(flat[offset:offset + size].view_as(parameter))
            offset += size

    def broadcast_module(self, module: torch.nn.Module) -> None:
        """The leader's parameters and buffers, everywhere: the ranks start as one network."""
        if not self.active:
            return
        for tensor in list(module.parameters()) + list(module.buffers()):
            if self.backend == "gloo" and tensor.device.type != "cpu":
                host = tensor.detach().cpu()
                self._dist.broadcast(host, 0, group=self.group)
                tensor.data.copy_(host)
            else:
                self._dist.broadcast(tensor.data, 0, group=self.group)

    # ------------------------------------------------------------------ objects

    def broadcast(self, value):
        """The leader's `value`, on every rank (pickled; for small decisions, not tensors)."""
        if not self.active:
            return value
        box = [value if self.leader else None]
        self._dist.broadcast_object_list(box, 0, group=self.group)
        return box[0]

    def gather(self, value) -> list | None:
        """Every rank's `value`, in rank order, on the leader; None elsewhere."""
        if not self.active:
            return [value]
        box = [None] * self.world if self.leader else None
        self._dist.gather_object(value, box, 0, group=self.group)
        return box

    def any(self, flag: bool) -> bool:
        """Whether `flag` holds on any rank (one small all-reduce, on the host)."""
        if not self.active:
            return flag
        box = torch.tensor([int(flag)], dtype=torch.int32,
                           device=torch.cuda.current_device() if self.backend == "nccl" else "cpu")
        self._dist.all_reduce(box, group=self.group)
        return bool(box.item())

    def barrier(self) -> None:
        if self.active:
            self._dist.barrier(group=self.group)

    def close(self) -> None:
        if self.active and self._dist.is_initialized():
            self._dist.destroy_process_group()


def share(total: int, world: int, rank: int) -> tuple[int, int]:
    """(first, count) of rank `rank`'s even share of `total` things (seeds, sims)."""
    first = total * rank // world
    return first, total * (rank + 1) // world - first


class Silent:
    """What a follower rank has instead of the run's writers (metrics, progress, evaluation logs): every call is a
    no-op, so the code that logs runs unchanged on every rank and only the leader's run is written."""

    def __getattr__(self, name):
        return lambda *args, **kwargs: None
