"""Kickstarting a merge stage from the stages it joins (distill: in the config).

A merge stage mixes arenas that different parents already play: the extended stage's arenas and each merged stage's.
Seeding (animus.bootstrap) copies the trunk from one parent only, so on the other parents' arenas the seeded policy
starts off worse than those parents. Distillation pulls it back: on the decisions of an arena that has a teacher, the
policy loss gains coef x KL(teacher || policy), with coef decaying over training so PPO takes over (Schmitt et al.,
"Kickstarting Deep Reinforcement Learning", 2018).

A teacher is a parent's frozen actor. Its layouts, blocks and actions are matched to the stage's by name through both
stage.json files (block spans): the teacher sees the stage's observation columns of the blocks it has (the others
are zero), and the KL is over the actions both have and the stage's mask allows, each distribution renormalised
over that set. Each decision's arena comes from the critic state's arena one-hot (stage.json ``state``).
"""

from __future__ import annotations

from dataclasses import dataclass

import torch

from .mappo.networks import MASKED_LOGIT, LayoutActor
from .stages import Span, arena_names, arena_state_span, block_spans


@dataclass
class LayoutMap:
    """How one of the stage's layouts reads and writes a teacher layout."""

    teacher_layout: int
    obs_student: torch.Tensor  # stage observation columns ...
    obs_teacher: torch.Tensor  # ... and where they go in the teacher's
    actions_student: torch.Tensor  # stage actions ...
    actions_teacher: torch.Tensor  # ... and the teacher's same actions


@dataclass
class Teacher:
    name: str  # the checkpoint's scenario
    actor: LayoutActor
    obs_dim: int  # the teacher's padded observation width
    num_actions: int  # the teacher's padded action count
    layouts: dict[int, LayoutMap]  # the stage's layout index -> its map; layouts the teacher lacks are absent
    recurrent_size: int = 0  # the teacher's own memory, carried between its decisions like the student's


def _index_pairs(student: dict[str, tuple[Span, Span]] | None, teacher: dict[str, tuple[Span, Span]] | None,
                 student_dims: tuple[int, int], teacher_dims: tuple[int, int]):
    """(obs student, obs teacher, actions student, actions teacher) index lists for the blocks both have with equal
    sizes; without block spans, the common prefix."""
    if student is None or teacher is None:
        obs = list(range(min(student_dims[0], teacher_dims[0])))
        actions = list(range(min(student_dims[1], teacher_dims[1])))
        return obs, obs, actions, actions

    obs_s, obs_t, act_s, act_t = [], [], [], []
    for block, ((s_obs, s_count), (s_act, s_act_count)) in student.items():
        if block not in teacher:
            continue
        (t_obs, t_count), (t_act, t_act_count) = teacher[block]
        if (s_count, s_act_count) != (t_count, t_act_count):
            continue
        obs_s += range(s_obs, s_obs + s_count)
        obs_t += range(t_obs, t_obs + t_count)
        act_s += range(s_act, s_act + s_act_count)
        act_t += range(t_act, t_act + t_act_count)
    return obs_s, obs_t, act_s, act_t


def build_teacher(checkpoint: dict, spec, stage: dict | None, device: torch.device) -> Teacher:
    """A frozen actor from `checkpoint`, mapped onto the stage's layouts (spec.layouts, stage.json `stage`)."""
    t_spec = checkpoint["spec"]
    t_layouts = [(entry["obs_dim"], entry["num_actions"]) for entry in t_spec["layouts"]]
    mappo = checkpoint["config"].get("mappo", {})
    hidden = list(mappo["hidden"])
    # A teacher trained with its own memory or goal head keeps them: its weights only load into the same shape, and a
    # recurrent teacher has to be replayed in order for its advice to mean anything (Distiller.kl).
    recurrent_size = int(mappo.get("recurrent_size", 0) or 0)
    goal_count = int(mappo.get("goal_count", 0) or 0)
    horizons = mappo.get("foresight_horizons_seconds", ())
    foresight_outputs = (len(horizons) + 1) if float(mappo.get("foresight_coef", 0.0) or 0.0) > 0.0 else 0
    actor = LayoutActor(t_layouts, hidden, foresight_outputs, recurrent_size, goal_count)
    actor.load_state_dict(checkpoint["trainer"]["actor"])
    actor.to(device).eval()
    for param in actor.parameters():
        param.requires_grad_(False)

    t_names = [entry["name"] for entry in t_spec["layouts"]]
    t_stage = checkpoint.get("stage")
    layouts = {}
    for index, layout in enumerate(spec.layouts):
        if layout.name not in t_names:
            continue
        t_index = t_names.index(layout.name)
        obs_s, obs_t, act_s, act_t = _index_pairs(
            block_spans(stage, layout.name), block_spans(t_stage, layout.name),
            (layout.obs_dim, layout.num_actions), t_layouts[t_index])
        if not act_s:
            continue

        def tensor(values):
            return torch.as_tensor(values, dtype=torch.long, device=device)

        layouts[index] = LayoutMap(t_index, tensor(obs_s), tensor(obs_t), tensor(act_s), tensor(act_t))

    return Teacher(
        name=t_spec.get("scenario", "?"),
        actor=actor,
        obs_dim=max(obs for obs, _ in t_layouts),
        num_actions=max(actions for _, actions in t_layouts),
        layouts=layouts,
        recurrent_size=recurrent_size,
    )


def auto_teachers(stage: dict | None, parents: list[dict]) -> dict[str, dict]:
    """distill.teachers: auto -- every arena of the stage goes to the first parent checkpoint (in order) whose own
    stage.json has an arena of that name."""
    chosen = {}
    for arena in arena_names(stage):
        for parent in parents:
            if arena in arena_names(parent.get("stage")):
                chosen[arena] = parent
                break
    return chosen


class Distiller:
    """The auxiliary loss for MappoTrainer.update: KL(teacher || policy) on the decisions of taught arenas."""

    def __init__(self, stage: dict | None, teachers: dict[str, Teacher]):
        span = arena_state_span(stage)
        if span is None:
            raise ValueError("distillation needs the stage.json arena state span (a sim that writes stage.json 2)")
        names = arena_names(stage)
        unknown = sorted(set(teachers) - set(names))
        if unknown:
            raise ValueError(f"distill.teachers: the stage has no arena {', '.join(unknown)} ({', '.join(names)})")
        self.arena_first, self.arena_count = span
        self.teachers = {names.index(arena): teacher for arena, teacher in teachers.items()}
        self.coef = 0.0

    def arenas(self, state: torch.Tensor) -> torch.Tensor:
        """Each row's arena index from the critic state one-hot; -1 when none is set."""
        onehot = state[..., self.arena_first : self.arena_first + self.arena_count]
        value, index = onehot.max(dim=-1)
        return torch.where(value > 0.5, index, torch.full_like(index, -1))

    def begin_sequence(self, rows: int, device) -> dict[int, torch.Tensor]:
        """Cleared memories for a recurrent teacher, one row per student row, to carry through a replayed sequence
        (MappoTrainer's recurrent update). Teachers without memory need none."""
        return {arena: teacher.actor.initial_memory(rows, device=device)
                for arena, teacher in self.teachers.items() if teacher.recurrent_size}

    def kl(self, obs: torch.Tensor, state: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
           log_probs: torch.Tensor, memories: dict[int, torch.Tensor] | None = None,
           dones: torch.Tensor | None = None) -> tuple[torch.Tensor, int]:
        """Summed KL(teacher || policy) over the taught rows, and how many rows that is. `log_probs` [n, N] are the
        policy's normalised log-probabilities (Categorical.logits) for the rows.

        With `memories` (from begin_sequence) the rows are one decision of a replayed sequence: a recurrent teacher is
        run on every row of a layout it has, whether or not that row is taught, because its memory has to follow the
        same decisions the student's did, and `dones` clears it where an episode ended.
        """
        arena = self.arenas(state)
        total = log_probs.new_zeros(())
        rows_taught = 0

        for arena_index, teacher in self.teachers.items():
            in_arena = arena == arena_index
            memory = memories.get(arena_index) if memories is not None else None
            if not in_arena.any() and memory is None:
                continue
            for layout_index in torch.unique(layout).tolist():
                mapping = teacher.layouts.get(int(layout_index))
                if mapping is None:
                    continue

                # With a recurrent teacher every row of a layout it has is replayed, so its memory follows the same
                # decisions; only the rows of its arena are taught from.
                of_layout = layout == layout_index
                replayed = torch.nonzero(of_layout if memory is not None else of_layout & in_arena,
                                         as_tuple=True)[0]
                if not len(replayed):
                    continue

                rows = replayed
                allowed = mask[rows][:, mapping.actions_student].bool()
                taught = allowed.any(dim=-1) & in_arena[rows]
                if memory is None and not taught.any():
                    continue

                with torch.no_grad():
                    t_obs = obs.new_zeros(len(rows), teacher.obs_dim)
                    t_obs[:, mapping.obs_teacher] = obs[rows][:, mapping.obs_student]
                    t_mask = torch.zeros(len(rows), teacher.num_actions, dtype=torch.bool, device=obs.device)
                    t_mask[:, mapping.actions_teacher] = allowed
                    t_layout = torch.full((len(rows),), mapping.teacher_layout, dtype=torch.long, device=obs.device)
                    if memory is not None:
                        distribution, carried, _ = teacher.actor.step(t_obs, t_layout, t_mask, memory[rows])
                        memory[rows] = carried
                        t_logits = distribution.logits[:, mapping.actions_teacher]
                    else:
                        t_logits = teacher.actor(t_obs, t_layout, t_mask).logits[:, mapping.actions_teacher]
                    t_logp = torch.log_softmax(t_logits.masked_fill(~allowed, MASKED_LOGIT), dim=-1)

                if not taught.any():
                    continue

                s_logits = log_probs[rows][:, mapping.actions_student]
                s_logp = torch.log_softmax(s_logits.masked_fill(~allowed, MASKED_LOGIT), dim=-1)
                kl = (t_logp.exp() * (t_logp - s_logp)).masked_fill(~allowed, 0.0).sum(dim=-1)
                total = total + kl[taught].sum()
                rows_taught += int(taught.sum())

        if memories is not None and dones is not None:
            # An episode ended here: the next decision of that row starts the teacher with nothing remembered, as it
            # starts the student.
            keep = (~dones).to(obs.dtype).reshape(-1, 1)
            for memory in memories.values():
                memory.mul_(keep)

        return total, rows_taught

    def __call__(self, data: dict, idx: torch.Tensor, dist) -> tuple[torch.Tensor, dict[str, float]] | None:
        """MappoTrainer.update's auxiliary hook: coef x mean KL on the minibatch's taught rows."""
        if self.coef < 1e-4:
            return None
        total, rows = self.kl(data["obs"][idx], data["state"][idx], data["layout"][idx], data["mask"][idx],
                              dist.logits)
        if rows == 0:
            return None
        mean = total / rows
        return self.coef * mean, {"distill_kl": float(mean.detach()), "distill_rows": float(rows)}

    def sequence_loss(self, obs: torch.Tensor, state: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                      logits: torch.Tensor, dones: torch.Tensor):
        """The whole replayed chunk at once, in place of a call per decision.

        Every argument carries a leading [steps, rows] (logits [steps, rows, actions]). A recurrent teacher still has
        to see the decisions in order, but only its GRU cell does: its adapters and trunk run once over every step,
        which is the difference between a matmul per step per teacher and one per teacher. stage27_crossroads has six
        teachers and measured a 306 s update against a 6 s rollout before this.

        Returns (loss, rows) with rows 0 when nothing was taught.
        """
        if self.coef < 1e-4:
            return None

        steps, rows = layout.shape[0], layout.shape[1]
        flat_obs = obs.reshape(steps * rows, -1)
        flat_layout = layout.reshape(-1)
        flat_mask = mask.reshape(steps * rows, -1)
        flat_logits = logits.reshape(steps * rows, -1)
        arena = self.arenas(state.reshape(steps * rows, -1))

        total = logits.new_zeros(())
        rows_taught = 0
        for arena_index, teacher in self.teachers.items():
            in_arena = arena == arena_index
            recurrent = bool(teacher.recurrent_size)
            if not in_arena.any() and not recurrent:
                continue

            # The teacher's own padded view of every step, built once: a row whose layout the teacher does not have
            # is not replayed at all, and its memory stands still, exactly as the per-decision path left it.
            t_obs = flat_obs.new_zeros(steps * rows, teacher.obs_dim)
            t_mask = torch.zeros(steps * rows, teacher.num_actions, dtype=torch.bool, device=obs.device)
            t_layout = torch.zeros(steps * rows, dtype=torch.long, device=obs.device)
            allowed = torch.zeros_like(flat_mask, dtype=torch.bool)
            mapped = torch.zeros(steps * rows, dtype=torch.bool, device=obs.device)
            columns: dict[int, torch.Tensor] = {}
            for layout_index in torch.unique(flat_layout).tolist():
                mapping = teacher.layouts.get(int(layout_index))
                if mapping is None:
                    continue

                of_layout = flat_layout == layout_index
                picked = torch.nonzero(of_layout if recurrent else of_layout & in_arena, as_tuple=True)[0]
                if not len(picked):
                    continue

                student_allowed = flat_mask[picked][:, mapping.actions_student].bool()
                t_obs[picked.unsqueeze(1), mapping.obs_teacher.unsqueeze(0)] = flat_obs[picked][:,
                    mapping.obs_student]
                t_mask[picked.unsqueeze(1), mapping.actions_teacher.unsqueeze(0)] = student_allowed
                t_layout[picked] = mapping.teacher_layout
                allowed[picked.unsqueeze(1), mapping.actions_student.unsqueeze(0)] = student_allowed
                mapped[picked] = True
                columns[int(layout_index)] = mapping.actions_student

            if not mapped.any():
                continue

            with torch.no_grad():
                if recurrent:
                    encoded = teacher.actor.encode(t_obs, t_layout).reshape(steps, rows, -1)
                    memory = teacher.actor.initial_memory(rows, device=obs.device)
                    carried = []
                    valid = mapped.reshape(steps, rows)
                    for step in range(steps):
                        moved = teacher.actor.memory(encoded[step], memory)
                        # A row the teacher cannot read keeps the memory it had, as it did decision by decision.
                        memory = torch.where(valid[step].unsqueeze(1), moved, memory)
                        carried.append(memory)
                        memory = memory * (~dones[step]).to(memory.dtype).unsqueeze(1)
                    features = torch.stack(carried).reshape(steps * rows, -1)
                    t_logits = teacher.actor.action_distribution(features, t_layout, t_mask).logits
                else:
                    t_logits = teacher.actor(t_obs, t_layout, t_mask).logits

            # Taught rows are the teacher's own arena, and only where the student had something to choose between.
            for layout_index, student_columns in columns.items():
                of_layout = flat_layout == layout_index
                picked = torch.nonzero(of_layout & in_arena, as_tuple=True)[0]
                if not len(picked):
                    continue

                mapping = teacher.layouts[layout_index]
                row_allowed = allowed[picked][:, student_columns]
                taught = row_allowed.any(dim=-1)
                if not taught.any():
                    continue

                t_logp = torch.log_softmax(
                    t_logits[picked][:, mapping.actions_teacher].masked_fill(~row_allowed, MASKED_LOGIT), dim=-1)
                s_logp = torch.log_softmax(
                    flat_logits[picked][:, student_columns].masked_fill(~row_allowed, MASKED_LOGIT), dim=-1)
                kl = (t_logp.exp() * (t_logp - s_logp)).masked_fill(~row_allowed, 0.0).sum(dim=-1)
                total = total + kl[taught].sum()
                rows_taught += int(taught.sum())

        if rows_taught == 0:
            return None

        return self.coef * (total / rows_taught), rows_taught

    def step_loss(self, obs: torch.Tensor, state: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                  logits: torch.Tensor, memories: dict[int, torch.Tensor], dones: torch.Tensor):
        """One decision of a replayed sequence (MappoTrainer's recurrent update): the same loss as __call__, with the
        teachers' memories carried in `memories` and cleared where `dones`. Returns (loss, rows) with rows 0 when
        nothing was taught."""
        if self.coef < 1e-4:
            return None
        total, rows = self.kl(obs, state, layout, mask, logits, memories, dones)
        if rows == 0:
            return None
        return self.coef * (total / rows), rows
