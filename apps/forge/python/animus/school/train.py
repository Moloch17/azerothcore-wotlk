"""**The school's trainer** (docs/forge/reference/school.md):

    python -m animus.school.train --config configs/school.yaml [--set key=value ...] [--eval-only] \\
        [--checkpoint <run>/best.pt]

Supervised, offline, on the recorder's shards (animus.school.record; the synthetic generator's for a smoke): per tier
a Huber loss on the cost-to-go over the label's reachable cells (in log1p space for the U-Net, yards / 100 for the
VIN: each model's own cost space), a binary cross-entropy of the reachable logit against the label's reachability
over every cell, the direction cross-entropy over the graded cells (the aux pass's multi-hot form; a cell the crop
does not know weighted `unseen_weight`: the unseen cells are the school's point), and a class-balanced cross-entropy
on the classes over the labelled cells (inverse frequency, clipped at `balance_clip`, the unseen weight again).

Every epoch the model is measured on the validation and the held-out splits (metrics.Scores) beside Dijkstra over the
crop and the oracle over the truth on the same batches (metrics.Baselines), the row logged to `metrics.csv` and
printed, `latest.pt` and `best.pt` (the validation `dir_acc_body`) saved under `<out_dir>/<name>/`, and the pass mark
(metrics.pass_mark, on the held-out split) printed last.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
import torch
from torch import nn

from .. import protocol
from ..config import apply_override, from_dict, load_yaml, resolve_device
from . import model as models
from .data import SampleSource, ShardList, check_batch, prepare
from .metrics import BASELINES, HEADLINE, Baselines, Scores, pass_mark

EVAL_SPLITS = ("val", "heldout")


@dataclass
class SchoolTrainConfig:
    """configs/school.yaml."""

    # The shard directories (record.ShardWriter's; a list reads several). Relative to the working directory, which
    # the documented command makes apps/forge/python: the repository's var/ is three levels up.
    data_dir: str | list[str] = "../../../var/animus-forge/shared/school/nav2_detour"
    # Where this run's checkpoints and metrics.csv go: <out_dir>/<name>/.
    out_dir: str = "../../../var/animus-forge/shared/school/_runs"
    name: str = "unet"
    # "unet" (model.SchoolPlanner) or "vin" (model.VinPlanner).
    model: str = "unet"
    widths: tuple[int, ...] = (32, 64, 96, 128)
    vin_k: int = 40
    epochs: int = 10
    batch: int = 64
    lr: float = 0.001
    weight_decay: float = 0.0001
    grad_clip: float = 1.0
    device: str = "auto"
    threads: int = 0
    seed: int = 1
    # Shards loaded at a time (data.SampleSource), and whether training draws (map, memory) groups equally.
    pool_shards: int = 4
    balance: bool = True
    # The losses' weights, and the weight of a cell the crop does not know in the direction and class losses.
    cost_coef: float = 1.0
    reach_coef: float = 0.5
    dir_coef: float = 1.0
    cls_coef: float = 0.25
    unseen_weight: float = 3.0
    balance_clip: float = 10.0
    # Samples an evaluation split reads each epoch (0 all of them), and a training epoch's cap (0 one pass).
    eval_samples: int = 4096
    train_samples: int = 0
    log_every: int = 50
    resume: str = ""

    def data_dirs(self) -> list[str]:
        return [self.data_dir] if isinstance(self.data_dir, str) else list(self.data_dir)

    @classmethod
    def load(cls, path: str | Path, overrides: list[str] | None = None) -> "SchoolTrainConfig":
        raw = load_yaml(path)
        for override in overrides or ():
            apply_override(raw, override)
        return from_dict(cls, raw)


def class_weights(classes: torch.Tensor, clip: float) -> torch.Tensor:
    """[TRAV_CLASSES] inverse-frequency weights over a batch's labelled cells (class 0 is unlabelled: weight 0),
    clipped at `clip` x the least, mean 1 over the classes present."""
    counts = torch.bincount(classes.reshape(-1), minlength=protocol.TRAV_CLASSES)[:protocol.TRAV_CLASSES].float()
    counts[0] = 0.0
    present = counts > 0
    weights = torch.where(present, 1.0 / counts.clamp(min=1.0), torch.zeros_like(counts))
    if bool(present.any()):
        weights = torch.minimum(weights, weights[present].min() * clip)
        weights = weights / weights[present].mean()
    return weights


def losses(out: dict, batch: dict, cfg: SchoolTrainConfig, cost_space: str) -> tuple[torch.Tensor, dict[str, float]]:
    """The module docstring's losses, averaged over the tiers; (the total, each part as a float)."""
    parts = {"cost": 0.0, "reach": 0.0, "dir": 0.0, "cls": 0.0}
    total = torch.zeros((), device=batch["memory"].device)
    for tier in range(2):
        label, reachable = batch["cost"][tier], batch["reachable"][tier]
        known = batch["known"][tier].to(torch.float32)
        seen_weight = known + (1.0 - known) * cfg.unseen_weight
        if cost_space == "log1p":
            error = nn.functional.smooth_l1_loss(out["cost_log"][tier], torch.log1p(label.clamp(min=0.0)),
                                                 reduction="none")
        else:
            error = nn.functional.smooth_l1_loss(out["cost"][tier] / 100.0, label / 100.0, reduction="none")
        cost = (error * reachable).sum() / reachable.sum().clamp(min=1)
        reach = nn.functional.binary_cross_entropy_with_logits(out["reach"][tier], reachable.to(torch.float32))
        logits, target, graded = out["dir"][tier].to(torch.float32), batch["target"][tier], batch["graded"][tier]
        crossing = torch.logsumexp(logits, dim=1) - torch.logsumexp(
            torch.where(target, logits, torch.full_like(logits, -1e9)), dim=1)
        weight = seen_weight * graded
        direction = (crossing * weight).sum() / weight.sum().clamp(min=1e-6)
        classes = batch["classes"][tier]
        entropy = nn.functional.cross_entropy(out["cls"][tier].to(torch.float32), classes, reduction="none")
        weight = class_weights(classes, cfg.balance_clip)[classes] * seen_weight
        cls = (entropy * weight).sum() / weight.sum().clamp(min=1e-6)
        total = total + (cfg.cost_coef * cost + cfg.reach_coef * reach + cfg.dir_coef * direction
                         + cfg.cls_coef * cls) / 2.0
        for name, value in (("cost", cost), ("reach", reach), ("dir", direction), ("cls", cls)):
            parts[name] += float(value.detach()) / 2.0
    return total, parts


@torch.no_grad()
def evaluate(model: nn.Module, shards: ShardList, cfg: SchoolTrainConfig, device,
             baselines: Baselines) -> dict[str, dict[str, Scores]]:
    """Per evaluation split, the model's and both baselines' Scores."""
    model.eval()
    out: dict[str, dict[str, Scores]] = {}
    for split in EVAL_SPLITS:
        scores = {"model": Scores(), **{name: Scores() for name in BASELINES}}
        source = SampleSource(shards, split, cfg.batch, cfg.pool_shards, False, cfg.seed, limit=cfg.eval_samples)
        for raw in source.batches():
            batch = prepare(raw, device)
            predicted = model(batch["decoded"], batch["goal_plane"], batch["memory"])
            scores["model"].update(batch, predicted["cost"], predicted["dir"], predicted["reach"], predicted["cls"])
            for name in BASELINES:
                costs, dirs = baselines.plan(batch, name)
                scores[name].update(batch, costs, dirs)
        out[split] = scores
    model.train()
    return out


def report_lines(results: dict[str, dict[str, Scores]]) -> list[str]:
    lines = []
    for split, scores in results.items():
        rows = int(scores["model"].count("dir_acc_body"))
        lines.append(f"  {split} ({rows} body rows, {int(scores['model'].count('dir_acc_body_unseen'))} unseen, "
                     f"{int(scores['model'].count('dir_acc_body_mem_on'))} memory on): "
                     f"{'metric':<28}{'model':>9}{'dijkstra':>10}{'oracle':>9}")
        for metric in HEADLINE:
            lines.append(f"    {metric:<30}" + "".join(f"{scores[name].value(metric):>9.3f}"
                                                         + (" " if name == "model" else "")
                                                         for name in ("model", *BASELINES)))
        extra = ", ".join(f"{name} {scores['model'].value(name):.3f}"
                          for name in ("reach_acc", "cls_acc_known", "cls_acc_unseen"))
        lines.append(f"    model heads: {extra}")
    return lines


def flat_metrics(results: dict[str, dict[str, Scores]]) -> dict[str, float]:
    out: dict[str, float] = {}
    for split, scores in results.items():
        for name, score in scores.items():
            for metric, value in score.values().items():
                out[f"{split}_{name}_{metric}"] = value
            out[f"{split}_{name}_n_body"] = score.count("dir_acc_body")
            out[f"{split}_{name}_n_body_unseen"] = score.count("dir_acc_body_unseen")
    return out


def save(path: Path, model: nn.Module, cfg: SchoolTrainConfig, epoch: int, metrics: dict[str, float]) -> None:
    torch.save({"model": model.state_dict(), "config": asdict(cfg), "epoch": epoch, "metrics": metrics,
                "format": {"protocol": protocol.PROTOCOL_VERSION, "cost_space": model.cost_space}}, path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True)
    parser.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    parser.add_argument("--eval-only", action="store_true", help="measure the checkpoint (or a fresh model) and stop")
    parser.add_argument("--checkpoint", default="", help="weights to load (also `resume` in the yaml)")
    args = parser.parse_args()
    cfg = SchoolTrainConfig.load(args.config, args.set)
    if cfg.threads > 0:
        torch.set_num_threads(cfg.threads)
    torch.manual_seed(cfg.seed)
    np.random.seed(cfg.seed)
    device = torch.device(resolve_device(cfg.device))
    shards = ShardList.scan(cfg.data_dirs())
    print(f"Data: {shards.describe()}", flush=True)
    first = next(iter(SampleSource(shards, "train", min(cfg.batch, 256), 1, False, cfg.seed).batches()), None)
    if first is not None:
        print(f"Check: {check_batch(first)}", flush=True)
    model = models.build(cfg.model, widths=cfg.widths, k=cfg.vin_k).to(device)
    print(f"Model: {cfg.model} ({models.parameter_count(model):,} parameters, cost in {model.cost_space}) on "
          f"{device}", flush=True)
    checkpoint = args.checkpoint or cfg.resume
    epoch_start = 0
    if checkpoint:
        state = torch.load(checkpoint, map_location=device, weights_only=False)
        model.load_state_dict(state["model"])
        epoch_start = int(state.get("epoch", 0))
        print(f"Loaded {checkpoint} (epoch {epoch_start})", flush=True)
    baselines = Baselines(device)
    run_dir = Path(cfg.out_dir) / cfg.name
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "config.json").write_text(json.dumps(asdict(cfg), indent=1))
    if args.eval_only:
        results = evaluate(model, shards, cfg, device, baselines)
        print("\n".join(report_lines(results)), flush=True)
        print(pass_mark(results["heldout"]["model"], results["heldout"]["dijkstra"])[1], flush=True)
        return 0
    optimiser = torch.optim.AdamW(model.parameters(), lr=cfg.lr, weight_decay=cfg.weight_decay)
    csv_path = run_dir / "metrics.csv"
    writer = None
    best = -math.inf
    for epoch in range(epoch_start, cfg.epochs):
        started = time.perf_counter()
        source = SampleSource(shards, "train", cfg.batch, cfg.pool_shards, cfg.balance, cfg.seed + epoch,
                              limit=cfg.train_samples)
        sums = {"loss": 0.0, "cost": 0.0, "reach": 0.0, "dir": 0.0, "cls": 0.0}
        batches = 0
        for raw in source.batches():
            batch = prepare(raw, device)
            predicted = model(batch["decoded"], batch["goal_plane"], batch["memory"])
            loss, parts = losses(predicted, batch, cfg, model.cost_space)
            optimiser.zero_grad(set_to_none=True)
            loss.backward()
            if cfg.grad_clip > 0:
                nn.utils.clip_grad_norm_(model.parameters(), cfg.grad_clip)
            optimiser.step()
            batches += 1
            sums["loss"] += float(loss)
            for name, value in parts.items():
                sums[name] += value
            if cfg.log_every and batches % cfg.log_every == 0:
                print(f"  epoch {epoch + 1} batch {batches}: loss {sums['loss'] / batches:.4f} (cost "
                      f"{sums['cost'] / batches:.4f} reach {sums['reach'] / batches:.4f} dir "
                      f"{sums['dir'] / batches:.4f} cls {sums['cls'] / batches:.4f})", flush=True)
        train_seconds = time.perf_counter() - started
        results = evaluate(model, shards, cfg, device, baselines)
        metrics = {"epoch": epoch + 1, "batches": batches, "train_seconds": train_seconds,
                   "eval_seconds": time.perf_counter() - started - train_seconds,
                   **{f"train_{name}": value / max(1, batches) for name, value in sums.items()},
                   **flat_metrics(results)}
        if writer is None:
            handle = csv_path.open("a", newline="")
            writer = csv.DictWriter(handle, fieldnames=list(metrics))
            if csv_path.stat().st_size == 0:
                writer.writeheader()
        writer.writerow(metrics)
        handle.flush()
        print(f"Epoch {epoch + 1}/{cfg.epochs}: {batches} batches of {cfg.batch} in {train_seconds:.0f} s, loss "
              f"{metrics['train_loss']:.4f}; evaluated in {metrics['eval_seconds']:.0f} s", flush=True)
        print("\n".join(report_lines(results)), flush=True)
        save(run_dir / "latest.pt", model, cfg, epoch + 1, metrics)
        score = results["val"]["model"].value("dir_acc_body")
        if score > best:
            best = score
            save(run_dir / "best.pt", model, cfg, epoch + 1, metrics)
        print(pass_mark(results["heldout"]["model"], results["heldout"]["dijkstra"])[1], flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
