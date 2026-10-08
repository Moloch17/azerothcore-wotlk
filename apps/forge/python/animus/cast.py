"""Frozen checkpoints in the seats the sim declares cast (config ``cast.agents``), and the actor co-op partners are.

A stage declares in stage.json's ``cast`` list the agents an owner played for (the follow stage's leader): each is
answered here by a **frozen actor** loaded from an earlier checkpoint named in ``cast.agents``, and its rows are kept
out of the training samples. The evaluation never runs a cast actor: the far side of an evaluation is the learner's
own, so scores stay comparable across runs.

Nothing on the wire changes: which rows are cast follows from stage.json's ``cast`` list. The co-op partners
(animus.partners) reuse the frozen actor (CastActor) and the device residency (Residency) defined here.
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import numpy as np
import torch

from .distill import Teacher, build_teacher
from .protocol import look_hold
from .device import host


# ------------------------------------------------------------------ a frozen actor

class CastActor:
    """A frozen checkpoint acting on the stage's rows: the teacher's observation and action mapping
    (animus.distill.build_teacher), its own memory and goals per row, cleared with the episode.

    A checkpoint with a camera reads each of its rows' camera bytes (Step.image: the image, then the map crop) as the
    live actor does, its entity list and sight list from the mapped columns, and chooses its own free look (protocol
    22's ACT look section) beside its action."""

    def __init__(self, path: Path, spec, stage: dict | None, device, deterministic: bool = False):
        self.path = Path(path)
        checkpoint = torch.load(self.path, map_location="cpu", weights_only=False)
        if checkpoint.get("stage") is None and (self.path.parent / "stage.json").is_file():
            checkpoint["stage"] = json.loads((self.path.parent / "stage.json").read_text())
        self.teacher: Teacher = build_teacher(checkpoint, spec, stage, device)
        self.name = self.teacher.name
        self.device = device
        self.home = device  # where it acts; `device` is where its weights are now (offload, CastPool's cap)
        self.deterministic = deterministic
        self.look_heads = int(getattr(spec, "look_heads", 0) or 0)
        if self.teacher.camera_bytes and not getattr(spec, "image_bytes", 0):
            raise ValueError(f"{self.path}: the checkpoint has a camera and the sim sends no image")
        if self.teacher.look_heads and len(self.teacher.look_heads) != self.look_heads:
            raise ValueError(f"{self.path}: the checkpoint's free look has {len(self.teacher.look_heads)} heads, the "
                             f"sim's ACT {self.look_heads}")
        # The last decision's look for its rows [E, A, heads] (int32), and the logits it chose from (debugging, tests).
        self.look: np.ndarray | None = None
        self.last_logits: dict[int, torch.Tensor] = {}
        mappo = checkpoint["config"].get("mappo", {})
        self.goal_every = max(1, int(mappo.get("goal_every_decisions", 16) or 16))
        self.goal_count = int(mappo.get("goal_count", 0) or 0)
        self.recurrent = self.teacher.recurrent_size
        # teacher action index -> stage action index, per stage layout
        self.back = {index: dict(zip(m.actions_teacher.tolist(), m.actions_student.tolist()))
                     for index, m in self.teacher.layouts.items()}
        self.memory: np.ndarray | None = None
        self.goal: np.ndarray | None = None
        self.age: np.ndarray | None = None
        # A two-clock teacher's slow memory (MappoConfig.slow_goal_size), stepped when it chooses a goal.
        self.slow_size = self.teacher.actor.slow_size
        self.slow: np.ndarray | None = None
        # A teacher with two goals and a queue (mappo.goal_slots > 1): the goals queued behind the pair it holds.
        self.queue_width = max(1, self.teacher.actor.goal_slots - 2)
        self.queue: np.ndarray | None = None
        self.fallback_rows = 0

    def ensure(self, envs: int, agents: int) -> None:
        if self.memory is None or self.memory.shape[:2] != (envs, agents):
            self.memory = np.zeros((envs, agents, self.recurrent), dtype=np.float32)
            self.goal = np.zeros((envs, agents), dtype=np.int64)
            self.age = np.zeros((envs, agents), dtype=np.int64)
            self.slow = np.zeros((envs, agents, self.slow_size), dtype=np.float32)
            self.queue = np.full((envs, agents, self.queue_width), -1, dtype=np.int64)

    def clear(self, done: np.ndarray) -> None:
        if self.memory is not None:
            self.memory[done] = 0.0
            self.goal[done] = 0
            self.age[done] = 0
            self.slow[done] = 0.0
            self.queue[done] = -1

    def reset_all(self) -> None:
        self.memory = None

    def resident_bytes(self) -> int:
        """The bytes its weights and buffers take where they are (each frozen member carries its own camera)."""
        actor = self.teacher.actor
        return sum(t.numel() * t.element_size() for t in list(actor.parameters()) + list(actor.buffers()))

    def place(self, device) -> None:
        """Move the frozen weights (and the mapping's indexes) to `device`: CastPool / PartnerPool offload a member
        past their resident cap to the host and bring it back when it next acts. Its per-seat memory is host-side
        either way, so nothing it remembers is lost."""
        if str(device) == str(self.device):
            return
        self.teacher.actor.to(device)
        for mapping in self.teacher.layouts.values():
            for name in ("obs_student", "obs_teacher", "actions_student", "actions_teacher"):
                setattr(mapping, name, getattr(mapping, name).to(device))
        self.device = device

    def _image_rows(self, image, picked: np.ndarray, rows: int) -> torch.Tensor | None:
        """The picked rows' camera bytes [n, camera_bytes] uint8 on this actor's device, from the step's image [E, A,
        I] (numpy, or a view of the sim's device buffer, indexed where it lives); the leading bytes of the stage's row
        when the checkpoint reads less of it (no map). None for a checkpoint without a camera."""
        width = self.teacher.camera_bytes
        if not width:
            return None
        if image is None:
            raise ValueError(f"{self.path}: the checkpoint has a camera; its rows' image bytes (Step.image) have to "
                             f"come with them")
        if isinstance(image, torch.Tensor):
            flat = image.reshape(rows, -1)
            taken = flat[torch.as_tensor(picked, dtype=torch.long, device=flat.device)]
        else:
            taken = torch.from_numpy(np.ascontiguousarray(np.asarray(image).reshape(rows, -1)[picked]))
        if taken.shape[-1] < width:
            raise ValueError(f"{self.path}: the camera rows are {taken.shape[-1]} bytes, the checkpoint reads {width}")
        return taken[:, :width].to(self.device, torch.uint8)

    @torch.no_grad()
    def decide(self, obs: np.ndarray, mask: np.ndarray, layout: np.ndarray, rows: np.ndarray,
               fallback: np.ndarray, image=None,
               look: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray | None]:
        """The actions of the `rows` over `fallback` (other rows, and rows this actor cannot play, keep it), and the look: `look` [E, A, heads] (the live policy's, or None) with the rows this actor played
        replaced by its own look -- its LookHead's choice with a camera, else the hold (protocol.LOOK_HOLD) -- as
        the ACT's look section carries them. None when the sim takes no look and none was given."""
        envs, agents = layout.shape
        self.ensure(envs, agents)
        actions = fallback.copy()
        self.fallback_rows = 0
        self.last_logits = {}
        if look is None and self.look_heads:
            look = look_hold(envs, agents, self.look_heads)
        elif look is not None:
            look = np.array(look, dtype=np.int32, copy=True)
        self.look = look
        flat = np.nonzero(rows.reshape(-1))[0]
        if not len(flat):
            return actions, look
        flat_look = look.reshape(envs * agents, -1) if look is not None else None
        flat_layout = layout.reshape(-1)
        flat_obs = obs.reshape(envs * agents, -1)
        flat_mask = mask.reshape(envs * agents, -1)
        flat_memory = self.memory.reshape(envs * agents, -1)
        flat_goal = self.goal.reshape(-1)
        flat_age = self.age.reshape(-1)
        flat_slow = self.slow.reshape(envs * agents, -1)
        flat_queue = self.queue.reshape(envs * agents, -1)
        flat_actions = actions.reshape(-1)

        for index in np.unique(flat_layout[flat]):
            mapping = self.teacher.layouts.get(int(index))
            picked = flat[flat_layout[flat] == index]
            if mapping is None:
                self.fallback_rows += len(picked)
                continue
            obs_s = torch.as_tensor(flat_obs[picked], device=self.device)
            allowed = torch.as_tensor(flat_mask[picked], device=self.device)[:, mapping.actions_student].bool()
            usable = allowed.any(dim=-1).cpu().numpy()
            if not usable.all():
                self.fallback_rows += int((~usable).sum())
            t_obs = obs_s.new_zeros(len(picked), self.teacher.obs_dim)
            t_obs[:, mapping.obs_teacher] = obs_s[:, mapping.obs_student]
            t_mask = torch.zeros(len(picked), self.teacher.num_actions, dtype=torch.bool, device=self.device)
            t_mask[:, mapping.actions_teacher] = allowed
            t_layout = torch.full((len(picked),), mapping.teacher_layout, dtype=torch.long, device=self.device)
            memory = (torch.as_tensor(flat_memory[picked], device=self.device) if self.recurrent else None)
            t_image = self._image_rows(image, picked, envs * agents)
            features = self.teacher.actor.features(t_obs, t_layout, memory, image=t_image)
            goal = None
            if self.goal_count:
                ages = flat_age[picked]
                # The same decision as the learner's own (LayoutActor.decide_goals): the queue, the clock, the
                # goal block's ended and event, and the primary an order set.
                goal_features = features
                if self.slow_size:
                    slow = torch.as_tensor(flat_slow[picked], device=self.device)
                    goal_features = self.teacher.actor.slow_step(features, slow)
                decided = self.teacher.actor.decide_goals(
                    goal_features, t_obs, t_layout, torch.as_tensor(flat_goal[picked], device=self.device),
                    torch.as_tensor(flat_queue[picked], device=self.device),
                    torch.as_tensor(ages % self.goal_every == 0, device=self.device), self.deterministic)
                choose = decided["chosen"].cpu().numpy()
                if self.slow_size and choose.any():
                    flat_slow[picked[choose]] = goal_features.cpu().numpy()[choose]
                goal = decided["goal"]
                flat_goal[picked] = goal.cpu().numpy()
                flat_queue[picked] = decided["queue"].cpu().numpy()
                flat_age[picked] = np.where(choose, 1, ages + 1)
            dist = self.teacher.actor.action_distribution(features, t_layout, t_mask, goal, obs=t_obs)
            self.last_logits[int(index)] = dist.logits
            teacher_actions = (dist.probs.argmax(-1) if self.deterministic else dist.sample()).cpu().numpy()
            if self.recurrent:
                flat_memory[picked] = features.cpu().numpy()
            back = self.back[int(index)]
            for row, action, ok in zip(picked, teacher_actions, usable):
                if ok:
                    flat_actions[row] = back.get(int(action), flat_actions[row])
            if flat_look is not None:
                # The look of the rows it played (a row it could not play keeps the live policy's, with its action).
                played = picked[usable]
                if self.teacher.actor.look_head is not None:
                    chosen, _ = self.teacher.actor.look(features, t_layout, goal, self.deterministic)
                    flat_look[played] = chosen.cpu().numpy().astype(np.int32)[usable]
                else:
                    flat_look[played] = look_hold(1, 1, flat_look.shape[-1])[0, 0]
        return actions, look


class Residency:
    """At most `limit` frozen actors on their device at once (0 = no limit): the least recently used past it are
    offloaded to the host (CastActor.place) and brought back when they next act. Each member carries its own camera
    encoder, so a pool of camera-era checkpoints is what fills an 8 GB card first."""

    def __init__(self, limit: int):
        self.limit = max(0, int(limit or 0))
        self.order: list[CastActor] = []

    def touch(self, actor: CastActor) -> CastActor:
        actor.place(actor.home)
        self.forget(actor)
        self.order.append(actor)
        if self.limit:
            for other in self.order[:-self.limit]:
                other.place("cpu")
        return actor

    def forget(self, actor: CastActor | None) -> None:
        self.order = [other for other in self.order if other is not actor]

    def resident(self) -> int:
        return sum(1 for actor in self.order if str(actor.device) == str(actor.home))


# ------------------------------------------------------------------ snapshots


def snapshot(run_dir: Path, source: Path, tag: str, folder: str) -> Path | None:
    """Copy `source` (a checkpoint) into <run_dir>/<folder>/<tag>.pt (the co-op partners', animus.partners); None
    when it is already there or missing."""
    source = Path(source)
    if not source.is_file():
        return None
    directory = Path(run_dir) / folder
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / f"{tag}.pt"
    if target.exists():
        return None
    shutil.copyfile(source, target)
    return target


# ------------------------------------------------------------------ the facade

class Cast:
    """What the training loop talks to: which rows are cast this decision, and the actions for them."""

    def __init__(self, cast_config, spec, stage: dict | None, device):
        self.config = cast_config
        self.spec = spec
        self.statics: dict[int, CastActor] = {}
        names = {int(entry["agent"]): entry.get("name", "") for entry in (stage or {}).get("cast") or []
                 if "agent" in entry}
        for agent, name in names.items():
            path = cast_config.agents.get(name)
            if path:
                self.statics[agent] = CastActor(Path(path), spec, stage, device, cast_config.deterministic)
        self.cast_rows_total = 0
        self.rows_total = 0
        self.fallback_total = 0

    def rows(self, step) -> np.ndarray:
        """[E, A] bool: the rows a frozen actor answers this decision (the declared agents' seats that are present)."""
        rows = np.zeros(step.layout.shape, dtype=bool)
        for agent in self.statics:
            if agent < rows.shape[1]:
                rows[:, agent] = step.present[:, agent]
        self.rows_total += int(step.present.sum())
        self.cast_rows_total += int(rows.sum())
        return rows

    def act_and_look(self, step, live_actions: np.ndarray, rows: np.ndarray,
                     look: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray | None]:
        """The cast rows' actions over `live_actions`, and their look over `look` (the live policy's [E, A, heads],
        None without one): each frozen actor reads its rows' camera bytes (step.image, where they are) and sends its
        own look, the hold for one without a camera."""
        actions = live_actions.copy()
        obs, mask, image = host(step.obs), host(step.mask), getattr(step, "image", None)
        for agent, actor in self.statics.items():
            mine = np.zeros_like(rows)
            mine[:, agent] = rows[:, agent]
            if mine.any():
                actions, look = actor.decide(obs, mask, step.layout, mine, actions, image, look)
                self.fallback_total += actor.fallback_rows
        return actions, look

    def clear(self, done: np.ndarray) -> None:
        """Episodes ended: cast memories go."""
        for actor in self.statics.values():
            actor.clear(done)

    def reset_all(self) -> None:
        """Every env starts a fresh episode (after an evaluation): nothing remembered."""
        for actor in self.statics.values():
            actor.reset_all()

    def stats(self) -> dict[str, float]:
        share = self.cast_rows_total / self.rows_total if self.rows_total else 0.0
        out = {"cast_rows": float(share), "cast_fallback_rows": float(self.fallback_total)}
        self.cast_rows_total = self.rows_total = self.fallback_total = 0
        return out
