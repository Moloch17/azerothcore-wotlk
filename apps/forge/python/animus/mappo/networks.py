"""Layout-aware actor and centralized critic for MAPPO.

One set of weights serves every agent of every layout (parameter sharing). A layout is one kind of agent --
a class/build, say -- with its own observation features and actions. Each network has:

- an input adapter per layout: Linear(layout obs dim -> width), reading only that layout's features;
- a shared trunk: every hidden layer after the first, the same for all layouts;
- (actor) an action head per layout: Linear(width -> layout action count).

So what is learned about moving, threat, healing or interrupts is shared through the trunk, while every layout
keeps its exact observation and action spaces. For a single layout this is exactly the previous plain MLP, and
adapter + trunk + head of one layout is a plain MLP too (see animus.export).

The critic adds the global state: a state encoder Linear(state dim -> width) is summed with the agent's layout
adapter over its own observation, so each agent's value sees the whole env and its own situation
("agent-specific global state").

Agents of a layout are identified by their features, not by a seat index: the same network plays any seat.
"""

from __future__ import annotations

import math
from collections.abc import Sequence

import torch
from torch import nn
from torch.distributions import Categorical

MASKED_LOGIT = -1e9


def _linear(in_dim: int, out_dim: int, gain: float) -> nn.Linear:
    linear = nn.Linear(in_dim, out_dim)
    nn.init.orthogonal_(linear.weight, gain=gain)
    nn.init.zeros_(linear.bias)
    return linear


def masked_distribution(logits: torch.Tensor, mask: torch.Tensor) -> Categorical:
    """Categorical over allowed actions only. A row with nothing allowed falls back to action 0."""
    mask = mask.bool()
    empty = ~mask.any(dim=-1, keepdim=True)
    if empty.any():
        fallback = torch.zeros_like(mask)
        fallback[..., 0] = True
        mask = torch.where(empty, fallback, mask)
    return Categorical(logits=logits.masked_fill(~mask, MASKED_LOGIT))


class RunningNorm(nn.Module):
    """Per-feature mean and variance of the observations the policy has been trained on.

    The features arrive on wildly different scales -- a level in 1..80, yards, fractions of health, gear
    ratings -- and meet `tanh` as the first nonlinearity, which saturates on anything far from zero. Centring
    and scaling them is what lets the first layer see all of them at once.

    It is an affine map with no clipping, so `animus.export` can fold it into the adapter that follows and the
    exported model stays an ordinary MLP. The statistics are buffers: they travel in the checkpoint, and a
    stage that seeds from another carries them across for the blocks it keeps (animus.bootstrap).
    """

    def __init__(self, dim: int, epsilon: float = 1e-5):
        super().__init__()
        self.epsilon = epsilon
        self.register_buffer("mean", torch.zeros(dim))
        self.register_buffer("var", torch.ones(dim))
        self.register_buffer("count", torch.zeros(()))

    @torch.no_grad()
    def update(self, rows: torch.Tensor) -> None:
        """Fold a batch of observations into the statistics (Chan's parallel variance)."""
        if rows.shape[0] == 0:
            return

        batch_count = torch.tensor(float(rows.shape[0]), device=self.count.device)
        batch_mean, batch_var = rows.mean(dim=0), rows.var(dim=0, unbiased=False)
        total = self.count + batch_count
        delta = batch_mean - self.mean
        self.mean += delta * (batch_count / total)
        self.var.copy_((self.var * self.count + batch_var * batch_count
                        + delta.pow(2) * (self.count * batch_count / total)) / total)
        self.count.copy_(total)

    def forward(self, rows: torch.Tensor) -> torch.Tensor:
        if float(self.count) == 0.0:
            return rows  # nothing seen yet: the raw features are the best estimate of themselves

        return (rows - self.mean) / torch.sqrt(self.var + self.epsilon)

    @torch.no_grad()
    def scale(self) -> tuple[torch.Tensor, torch.Tensor]:
        """(mean, standard deviation) as the fold in `animus.export` needs them; identity before any update."""
        if float(self.count) == 0.0:
            return torch.zeros_like(self.mean), torch.ones_like(self.var)

        return self.mean.clone(), torch.sqrt(self.var + self.epsilon)


class _Trunk(nn.Module):
    """tanh, then Linear + tanh for every hidden layer after the first."""

    def __init__(self, hidden: Sequence[int]):
        super().__init__()
        self.layers = nn.ModuleList(_linear(a, b, math.sqrt(2)) for a, b in zip(hidden, hidden[1:]))

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = torch.tanh(x)
        for layer in self.layers:
            x = torch.tanh(layer(x))
        return x


def skip_distribution_checks() -> None:
    """Stop torch validating every Categorical it builds and every sample it scores.

    The checks (finite logits, a sample inside the support) run on every rollout decision and every minibatch of
    every epoch, and the logits here are built by this module, not by a user. Call it once at start-up.
    """
    torch.distributions.Distribution.set_default_validate_args(False)


@torch.no_grad()
def update_norms(norms: nn.ModuleList, obs: torch.Tensor, layout: torch.Tensor, obs_dims) -> None:
    """Fold a rollout's observations into each layout's statistics, each from its own rows and own features."""
    for index, rows in _per_layout(layout, len(norms)):
        norms[index].update(obs[rows, : obs_dims[index]])


def per_layout(layout: torch.Tensor, count: int) -> list[tuple[int, torch.Tensor]]:
    """(layout, row indices) for every layout present in the flat batch `layout`, for the forward passes to share: the
    grouping is a unique and a nonzero per layout, which with 18 layouts is most of a small batch's forward cost."""
    return _per_layout(layout.reshape(-1), count)


def _per_layout(layout: torch.Tensor, count: int) -> list[tuple[int, torch.Tensor]]:
    """(layout, row indices) for every layout present in the batch."""
    if count == 1:
        return [(0, torch.arange(layout.shape[0], device=layout.device))]
    layout = layout.long()
    present = torch.unique(layout).tolist()
    return [(int(index), torch.nonzero(layout == index, as_tuple=True)[0]) for index in present]


def _carry_sequence_loop(cell: nn.GRUCell, size: int, encoded: torch.Tensor, memory: torch.Tensor,
                         dones: torch.Tensor) -> torch.Tensor:
    """_carry_sequence one cell step at a time: what runs on the CPU, and the reference the fused call is tested
    against."""
    carried = memory.reshape(-1, size)
    features = []
    for step in range(encoded.shape[0]):
        carried = cell(encoded[step], carried)
        features.append(carried)
        carried = carried * (~dones[step]).to(carried.dtype)[:, None]
    return torch.stack(features)


def _pieces(dones: torch.Tensor) -> tuple[torch.Tensor, ...]:
    """Cut every row of a [T, N] sequence after each episode end, into pieces with no reset inside them. Returns, on
    the CPU: each piece's row, first step and length, then for every (t, n) the piece it falls in and its position
    there."""
    steps, rows = dones.shape
    ended = dones.detach().to("cpu", torch.bool)
    # A piece starts at step 0 of every row, and after every episode end that has a step after it.
    starts = torch.zeros((steps, rows), dtype=torch.bool)
    starts[0] = True
    starts[1:] = ended[:-1]
    # Numbered row-major (row, then time), so each row's pieces are consecutive.
    flat = starts.t().reshape(-1)
    piece_of = (torch.cumsum(flat.to(torch.int64), 0) - 1).reshape(rows, steps).t().contiguous()     # [T, N]
    first = torch.nonzero(flat, as_tuple=True)[0]
    piece_row = first // steps
    piece_start = first % steps
    # A piece runs to the next piece's start on the same row, else to the end of the sequence.
    same_row = torch.cat([piece_row[1:] == piece_row[:-1], torch.tensor([False])])
    next_start = torch.cat([piece_start[1:], torch.tensor([steps])])
    piece_length = torch.where(same_row, next_start - piece_start, steps - piece_start)
    position_of = torch.arange(steps)[:, None] - piece_start[piece_of]                                  # [T, N]
    return piece_row, piece_start, piece_length, piece_of, position_of


def _carry_sequence(cell: nn.GRUCell, size: int, encoded: torch.Tensor, memory: torch.Tensor,
                    dones: torch.Tensor) -> torch.Tensor:
    """Run a GRU over a replayed sequence: `encoded` [T, N, H], `memory` [N, R] the state its first decision was
    taken with, `dones` [T, N] where an episode ended (the next decision starts cleared) -> [T, N, R]. Only the cell
    is sequential, which is the small part; everything before it runs in one pass.

    On the GPU, one fused RNN call per sequence instead of stepping the cell: the rows are cut at episode ends into
    pieces that each start from their own memory (the carried one for a row's first piece, zero after an end), run
    packed through the library's GRU with the cell's own weights, and put back in [T, N] order. Stepping the cell
    was ~210,000 kernel launches per stage8_duel update; this took the update from 1.67 s to 1.29 s. (A hand-written
    persistent kernel was tried and lost to it: at 16 rows a minibatch is one workgroup walking 128 dependent steps.)
    """
    carried = memory.reshape(-1, size)
    steps, rows = encoded.shape[0], encoded.shape[1]
    if not encoded.is_cuda or steps == 0:
        return _carry_sequence_loop(cell, size, encoded, carried, dones)

    piece_row, piece_start, piece_length, piece_of, position_of = _pieces(dones)
    device = encoded.device
    longest = int(piece_length.max())

    # [pieces, longest] gather of each piece's steps; positions past a piece's end are dropped by the packing.
    times = (piece_start[:, None] + torch.arange(longest)[None, :]).clamp(max=steps - 1)
    inputs = encoded[times.to(device), piece_row[:, None].expand_as(times).to(device)]

    first = torch.nonzero(piece_start == 0, as_tuple=True)[0]
    initial = carried.new_zeros((len(piece_row), size)).index_copy(0, first.to(device),
                                                                   carried[piece_row[first].to(device)])

    packed = nn.utils.rnn.pack_padded_sequence(inputs, piece_length, batch_first=True, enforce_sorted=False)
    hx = initial.index_select(0, packed.sorted_indices)[None]
    # What nn.GRU.forward calls for a packed sequence, given the cell's parameters rather than a module's own.
    output, _ = torch._VF.gru(packed.data, packed.batch_sizes, hx,
                              [cell.weight_ih, cell.weight_hh, cell.bias_ih, cell.bias_hh], True, 1, 0.0,
                              cell.training, False)
    padded, _ = nn.utils.rnn.pad_packed_sequence(
        nn.utils.rnn.PackedSequence(output, packed.batch_sizes, packed.sorted_indices, packed.unsorted_indices),
        batch_first=True, total_length=longest)
    return padded[piece_of.to(device), position_of.to(device)]


class LayoutActor(nn.Module):
    def __init__(self, layouts: Sequence[tuple[int, int]], hidden: Sequence[int], foresight_outputs: int = 0,
                 recurrent_size: int = 0, goal_count: int = 0):
        """layouts: (obs dim, action count) per layout; hidden: widths, the first being the adapters' output.

        `foresight_outputs` adds a head on the trunk that predicts what happens after this decision (mappo.trainer's
        foresight_*): one output per extra discount horizon and one for how much of the episode is left. Nothing reads
        it at rollout time; it is there to make the trunk the actions are chosen from carry the future, and it is left
        out of exported models.

        `recurrent_size` puts a GRU between the trunk and the heads, carried from decision to decision and cleared when
        an episode ends: the policy's own memory of what it has already done and seen, which no observation of the
        moment can hold (who was crowd-controlled, that the enemy has spent its trinket, where the adds came from).
        """
        super().__init__()
        if not hidden:
            raise ValueError("the actor needs at least one hidden layer")
        self.obs_dims = [obs for obs, _ in layouts]
        self.action_counts = [actions for _, actions in layouts]
        self.max_actions = max(self.action_counts)
        self.norms = nn.ModuleList(RunningNorm(obs) for obs, _ in layouts)
        self.adapters = nn.ModuleList(_linear(obs, hidden[0], math.sqrt(2)) for obs, _ in layouts)
        self.trunk = _Trunk(hidden)
        self.recurrent_size = recurrent_size
        self.memory = nn.GRUCell(hidden[-1], recurrent_size) if recurrent_size else None
        head_width = recurrent_size if recurrent_size else hidden[-1]
        self.head_width = head_width
        self.heads = nn.ModuleList(_linear(head_width, actions, 0.01) for _, actions in layouts)
        self.foresight_outputs = foresight_outputs
        self.foresight = _linear(head_width, foresight_outputs, 1.0) if foresight_outputs else None
        # The goal head (mappo.goal_count): a goal chosen every mappo.goal_every_decisions and kept in between, which
        # the action head is conditioned on. The goal chooser decides on a clock many times slower than the actions,
        # so its own horizon is that many times shorter -- which is where a plan can be learned at all.
        self.goal_count = goal_count
        self.goal_head = _linear(head_width, goal_count, 0.01) if goal_count else None
        self.goal_embedding = nn.Embedding(goal_count, head_width) if goal_count else None
        if self.goal_embedding is not None:
            nn.init.zeros_(self.goal_embedding.weight)

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, groups=None,
                memory: torch.Tensor | None = None) -> Categorical:
        """obs [..., O], layout [...], mask [..., N] (padded) -> distribution over N actions. `groups` is
        per_layout(layout) when the caller already has it; `memory` [..., recurrent_size] the state carried in."""
        return self._forward(obs, layout, mask, groups, memory)[0]

    def step(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, memory: torch.Tensor | None = None,
             groups=None, goal: torch.Tensor | None = None) -> tuple[Categorical, torch.Tensor, torch.Tensor]:
        """One decision: (distribution, memory carried out, foresight predictions). Without a GRU the memory out is
        whatever came in (an empty tensor), and without foresight heads the predictions are empty."""
        dist, features, lead = self._forward(obs, layout, mask, groups, memory, goal)
        predictions = (self.foresight(features).reshape(*lead, self.foresight_outputs) if self.foresight is not None
                       else features.new_zeros((*lead, 0)))
        carried = features.reshape(*lead, features.shape[-1]) if self.memory is not None else (
            memory if memory is not None else features.new_zeros((*lead, 0)))
        return dist, carried, predictions

    def initial_memory(self, *lead: int, device=None) -> torch.Tensor:
        """A cleared memory for `lead` rows (what an episode starts with)."""
        return torch.zeros((*lead, self.recurrent_size), dtype=torch.float32, device=device)

    def encode(self, obs: torch.Tensor, layout: torch.Tensor, groups=None) -> torch.Tensor:
        """Adapters and trunk for flat rows: everything that depends only on this decision's observation, before the
        GRU. A replayed sequence encodes every step in one pass and then carries the memory through them (carry),
        which is the difference between one large matmul per layer and one per step."""
        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        width = self.adapters[0].out_features
        hidden = obs.new_zeros(obs.shape[0], width)
        for index, rows in groups:
            hidden[rows] = self.adapters[index](self.norms[index](obs[rows, : self.obs_dims[index]]))
        return self.trunk(hidden)

    def carry(self, encoded: torch.Tensor, memory: torch.Tensor, dones: torch.Tensor) -> torch.Tensor:
        """Run the GRU over a sequence: `encoded` [T, N, H] from encode(), `memory` [N, R] the state its first
        decision was taken with, `dones` [T, N] where an episode ended (the next decision starts cleared). Returns
        the features each decision was taken with, [T, N, R]. Only the GRU cell is sequential; it is the small part."""
        if self.memory is None:
            return encoded

        return _carry_sequence(self.memory, self.recurrent_size, encoded, memory, dones)

    def features(self, obs: torch.Tensor, layout: torch.Tensor, memory: torch.Tensor | None = None,
                 groups=None) -> torch.Tensor:
        """The trunk's output for flat rows, through the GRU when there is one: what every head reads."""
        hidden = self.encode(obs, layout, groups)
        if self.memory is not None:
            carried = (memory.reshape(-1, self.recurrent_size) if memory is not None
                       else hidden.new_zeros(hidden.shape[0], self.recurrent_size))
            hidden = self.memory(hidden, carried)
        return hidden

    def action_distribution(self, features: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                            goal: torch.Tensor | None = None, groups=None) -> Categorical:
        """The actions of flat rows whose features are `features`, under `goal` where the actor has goals."""
        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        if self.goal_embedding is not None and goal is not None:
            features = features + self.goal_embedding(goal.reshape(-1))

        logits = features.new_full((features.shape[0], mask.shape[-1]), MASKED_LOGIT)
        for index, rows in groups:
            logits[rows, : self.action_counts[index]] = self.heads[index](features[rows])
        return masked_distribution(logits, mask)

    def goal_distribution(self, features: torch.Tensor) -> Categorical:
        """Which goal to pursue next, from the same features."""
        return Categorical(logits=self.goal_head(features))

    def _forward(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, groups=None,
                 memory: torch.Tensor | None = None,
                 goal: torch.Tensor | None = None) -> tuple[Categorical, torch.Tensor, tuple[int, ...]]:
        lead = obs.shape[:-1]
        obs, layout, mask = obs.reshape(-1, obs.shape[-1]), layout.reshape(-1), mask.reshape(-1, mask.shape[-1])
        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))

        hidden = self.features(obs, layout, memory, groups)
        dist = self.action_distribution(hidden, layout, mask, goal, groups)
        if len(lead) != 1:
            dist = Categorical(logits=dist.logits.reshape(*lead, -1))
        return dist, hidden, lead


class LayoutCritic(nn.Module):
    """V(global state, agent's own observation). Outputs a normalised value when ValueNorm is in use.

    With `recurrent_size` it carries a GRU of its own, exactly as the actor does. It has to: the global state is a
    snapshot -- healths, positions, roles, the clock -- and holds nothing of what has already happened, while a
    recurrent actor's decisions depend on what it remembers. Every part of the return that follows from that memory
    is then invisible to the critic, and lands in the advantage as noise. The two memories are separate (the critic
    sees the whole env, the actor only its own seat) but they are carried and cleared together.
    """

    def __init__(self, state_dim: int, layouts: Sequence[tuple[int, int]], hidden: Sequence[int],
                 goal_count: int = 0, recurrent_size: int = 0):
        super().__init__()
        if not hidden:
            raise ValueError("the critic needs at least one hidden layer")
        # The goal the actor is pursuing (mappo.goal_count) is part of what the value depends on: the same situation
        # is worth different things while resting and while fighting. Without it the critic averages over goals and
        # the advantage says nothing about which goal was the right one, which is what lets goals collapse into one.
        self.goal_count = goal_count
        self.goal_embedding = nn.Embedding(goal_count, hidden[0]) if goal_count else None
        self.obs_dims = [obs for obs, _ in layouts]
        self.state_norm = RunningNorm(state_dim)
        self.state_encoder = _linear(state_dim, hidden[0], math.sqrt(2))
        self.norms = nn.ModuleList(RunningNorm(obs) for obs, _ in layouts)
        self.adapters = nn.ModuleList(_linear(obs, hidden[0], math.sqrt(2)) for obs, _ in layouts)
        self.trunk = _Trunk(hidden)
        self.recurrent_size = recurrent_size
        self.memory = nn.GRUCell(hidden[-1], recurrent_size) if recurrent_size else None
        self.head = _linear(recurrent_size if recurrent_size else hidden[-1], 1, 1.0)

    def encode(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
               goal: torch.Tensor | None = None, groups=None) -> torch.Tensor:
        """Everything that depends only on this decision -- the state, the seat's own observation and its goal --
        for flat rows, before the GRU. A replayed sequence encodes every step in one pass and then carries the memory
        through them (carry)."""
        hidden = self.state_encoder(self.state_norm(state))
        own = torch.zeros_like(hidden)
        for index, rows in groups if groups is not None else _per_layout(layout, len(self.adapters)):
            own[rows] = self.adapters[index](self.norms[index](obs[rows, : self.obs_dims[index]]))
        if self.goal_embedding is not None and goal is not None:
            own = own + self.goal_embedding(goal.reshape(-1))
        return self.trunk(hidden + own)

    def carry(self, encoded: torch.Tensor, memory: torch.Tensor, dones: torch.Tensor) -> torch.Tensor:
        """The critic's GRU over a replayed sequence, as LayoutActor.carry is for the actor's."""
        if self.memory is None:
            return encoded
        return _carry_sequence(self.memory, self.recurrent_size, encoded, memory, dones)

    def values_of(self, features: torch.Tensor) -> torch.Tensor:
        """The value of features already carried through the GRU (a replayed sequence)."""
        return self.head(features).squeeze(-1)

    def initial_memory(self, *lead: int, device=None) -> torch.Tensor:
        """A cleared memory for `lead` rows (what an episode starts with)."""
        return torch.zeros((*lead, self.recurrent_size), dtype=torch.float32, device=device)

    def step(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, goal: torch.Tensor | None = None,
             groups=None, memory: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """One decision: (value [...], the memory carried out). Without a GRU the memory out is whatever came in."""
        lead = obs.shape[:-1]
        state, obs, layout = state.reshape(-1, state.shape[-1]), obs.reshape(-1, obs.shape[-1]), layout.reshape(-1)
        encoded = self.encode(state, obs, layout, goal, groups)
        if self.memory is None:
            return self.head(encoded).reshape(lead), (memory if memory is not None else encoded.new_zeros((*lead, 0)))

        carried = self.memory(encoded, memory.reshape(-1, self.recurrent_size) if memory is not None
                              else encoded.new_zeros(encoded.shape[0], self.recurrent_size))
        return self.head(carried).reshape(lead), carried.reshape(*lead, self.recurrent_size)

    def forward(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, goal: torch.Tensor | None = None,
                groups=None, memory: torch.Tensor | None = None) -> torch.Tensor:
        """state [..., S], obs [..., O], layout [...], goal [...] (with a goal head) -> value [...]. `groups` as for
        LayoutActor.forward; `memory` [..., recurrent_size] the state carried in."""
        return self.step(state, obs, layout, goal, groups, memory)[0]
