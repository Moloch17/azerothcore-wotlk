"""Running value normalisation (ValueNorm from the MAPPO paper: debiased exponential moving stats)."""

from __future__ import annotations

import torch
from torch import nn


class ValueNorm(nn.Module):
    def __init__(self, beta: float = 0.99, epsilon: float = 1e-5):
        super().__init__()
        self.beta = beta
        self.epsilon = epsilon
        self.register_buffer("running_mean", torch.zeros((), dtype=torch.float32))
        self.register_buffer("running_mean_sq", torch.zeros((), dtype=torch.float32))
        self.register_buffer("debiasing_term", torch.zeros((), dtype=torch.float32))

    def _stats(self) -> tuple[torch.Tensor, torch.Tensor]:
        debias = self.debiasing_term.clamp(min=self.epsilon)
        mean = self.running_mean / debias
        mean_sq = self.running_mean_sq / debias
        var = (mean_sq - mean**2).clamp(min=1e-2)
        return mean, var

    @torch.no_grad()
    def update(self, values: torch.Tensor, ranks=None) -> None:
        """Fold a batch of returns in; with data-parallel `ranks` (animus.parallel), every rank's together."""
        values = values.detach().to(torch.float32)
        if ranks is not None and ranks.active:
            wide = values.to(torch.float64).reshape(-1)
            packed = ranks.sum(torch.stack([wide.new_tensor(float(wide.numel())), wide.sum(), (wide * wide).sum()]))
            count = packed[0].clamp(min=1.0)
            mean, mean_sq = (packed[1] / count).to(torch.float32), (packed[2] / count).to(torch.float32)
        else:
            mean, mean_sq = values.mean(), (values**2).mean()
        self.running_mean.mul_(self.beta).add_(mean * (1.0 - self.beta))
        self.running_mean_sq.mul_(self.beta).add_(mean_sq * (1.0 - self.beta))
        self.debiasing_term.mul_(self.beta).add_(1.0 - self.beta)

    def normalize(self, values: torch.Tensor) -> torch.Tensor:
        mean, var = self._stats()
        return ((values.to(torch.float32) - mean) / var.sqrt()).to(values.dtype)

    def denormalize(self, values: torch.Tensor) -> torch.Tensor:
        mean, var = self._stats()
        return (values.to(torch.float32) * var.sqrt() + mean).to(values.dtype)
