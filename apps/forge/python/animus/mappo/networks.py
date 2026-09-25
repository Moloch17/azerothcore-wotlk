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

import numpy as np
import torch
from torch import nn
from torch.distributions import Categorical

MASKED_LOGIT = -1e9


def _linear(in_dim: int, out_dim: int, gain: float) -> nn.Linear:
    linear = nn.Linear(in_dim, out_dim)
    nn.init.orthogonal_(linear.weight, gain=gain)
    nn.init.zeros_(linear.bias)
    return linear


def masked_logits(logits: torch.Tensor, mask: torch.Tensor) -> torch.Tensor:
    """The logits of allowed actions only, the others MASKED_LOGIT. A row with nothing allowed falls back to action 0."""
    mask = mask.bool()
    # Chosen on the device, not with `if empty.any()`: on a GPU that read waits for everything queued before it.
    empty = ~mask.any(dim=-1, keepdim=True)
    fallback = torch.zeros_like(mask)
    fallback[..., 0] = True
    mask = torch.where(empty, fallback, mask)
    return logits.masked_fill(~mask, MASKED_LOGIT)


def masked_distribution(logits: torch.Tensor, mask: torch.Tensor) -> Categorical:
    """Categorical over allowed actions only. A row with nothing allowed falls back to action 0."""
    return Categorical(logits=masked_logits(logits, mask))


def log_prob_of(logits: torch.Tensor, choice: torch.Tensor) -> torch.Tensor:
    """log softmax(logits)[choice]: Categorical(logits=...).log_prob(choice), without building the distribution."""
    return logits.gather(-1, choice.long()[..., None]).squeeze(-1) - torch.logsumexp(logits, dim=-1)


def sample_logits(logits: torch.Tensor, deterministic: bool = False) -> tuple[torch.Tensor, torch.Tensor]:
    """(choice, its log probability) from unnormalised logits, as Categorical(logits=...).sample() and log_prob draw
    them, in a handful of kernels instead of the ~30 the distribution's normalising, softmax, multinomial and checks
    take: the rollout's inner loop. Gumbel-max: argmax(logits + G), G = -log(-log U), is an exact draw. U is in
    [0, 1), so U = 0 gives G = -inf, never +inf: a masked action cannot win on a lucky draw."""
    if deterministic:
        choice = logits.argmax(dim=-1)
    else:
        choice = (logits - torch.log(-torch.log(torch.rand_like(logits)))).argmax(dim=-1)
    return choice, log_prob_of(logits, choice)


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
        # Folded into the layer that reads it (fold_into): the rollout copies pass rows through untouched.
        self.bypass = False

    @torch.no_grad()
    def update(self, rows: torch.Tensor, ranks=None) -> None:
        """Fold a batch of observations into the statistics (Chan's parallel variance). With data-parallel `ranks`
        (animus.parallel) the batch is every rank's rows together, so every rank's statistics stay the same; every
        rank must then call it, with or without rows of its own."""
        if ranks is not None and ranks.active:
            wide = rows.to(torch.float64)
            packed = ranks.sum(torch.cat([wide.new_tensor([float(rows.shape[0])]), wide.sum(dim=0),
                                          (wide * wide).sum(dim=0)]))
            if float(packed[0]) == 0.0:
                return
            width = rows.shape[1]
            batch_count = packed[0].to(torch.float32)
            batch_mean = (packed[1:1 + width] / packed[0]).to(torch.float32)
            batch_var = (packed[1 + width:] / packed[0] - (packed[1:1 + width] / packed[0]) ** 2).clamp(min=0.0)
            batch_var = batch_var.to(torch.float32)
        else:
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
        if self.bypass:
            return rows
        # Nothing seen yet: the raw features are the best estimate of themselves. Chosen on the device rather than by
        # reading the count back: on a GPU that read waits for every queued kernel, once per layout per forward pass.
        normalised = (rows - self.mean) / torch.sqrt(self.var + self.epsilon)
        return torch.where(self.count > 0.0, normalised, rows)

    @torch.no_grad()
    def scale(self) -> tuple[torch.Tensor, torch.Tensor]:
        """(mean, standard deviation) as the fold in `animus.export` needs them; identity before any update."""
        if float(self.count) == 0.0:
            return torch.zeros_like(self.mean), torch.ones_like(self.var)

        return self.mean.clone(), torch.sqrt(self.var + self.epsilon)


@torch.no_grad()
def fold_into(norm: RunningNorm, linear: nn.Linear) -> None:
    """Fold `norm` into the `linear` that reads its output, as animus.export does for the exported models, and
    bypass it: W' = W / sigma, b' = b - W (mu / sigma). The same map in one matrix product instead of five
    elementwise passes before it, which on the rollout's 128-row batches is most of what normalising cost."""
    mean, std = norm.scale()
    linear.bias.sub_(linear.weight @ (mean / std))
    linear.weight.div_(std[None, :])
    norm.bypass = True


class DenseLayouts:
    """One linear layer per layout (the adapters, the action heads), as one matrix over every layout: each row goes
    through all of them in a single product and keeps its own layout's slice. A layout's input past its width is the
    padding the sim writes as zeros, so its slice is exactly its own layer's output. For a rollout on the GPU, where
    ten small products a network cost more in launches than one ten times the size costs in arithmetic. Built from
    the layers as they are; rebuilt after the weights change (MappoTrainer._sync_rollout).
    """

    @torch.no_grad()
    def __init__(self, linears, in_width: int):
        self.layouts = len(linears)
        self.out = max(linear.out_features for linear in linears)
        first = linears[0].weight
        self.weight = first.new_zeros((in_width, self.layouts * self.out))
        self.bias = first.new_zeros(self.layouts * self.out)
        self.valid = torch.zeros((self.layouts, self.out), dtype=torch.bool, device=first.device)
        for index, linear in enumerate(linears):
            self.valid[index, : linear.out_features] = True
        self.refresh(linears)

    @torch.no_grad()
    def refresh(self, linears) -> None:
        """The layers' current weights, written into this matrix in place: a rollout graph (MappoTrainer) captured
        these tensors' addresses, so the weights after a sync have to land in them rather than in new ones."""
        for index, linear in enumerate(linears):
            columns = slice(index * self.out, index * self.out + linear.out_features)
            self.weight[: linear.in_features, columns] = linear.weight.t()
            self.bias[columns] = linear.bias

    def __call__(self, rows: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """rows [N, in_width], layout [N] -> each row's own layer's output, [N, out] (padded past its width)."""
        every = torch.addmm(self.bias, rows, self.weight).view(rows.shape[0], self.layouts, self.out)
        return every.gather(1, layout.long().view(-1, 1, 1).expand(-1, 1, self.out)).squeeze(1)


class SharedInputDense:
    """Two DenseLayouts that read the same rows (the actor's and the critic's adapters both read the observation) as
    one product: [first | second] stacked, and computed transposed -- W^T x^T -- which is the shape the GPU's
    library runs well at a rollout's ~100 rows (96 x 909 x 2560: 59 us, against 113 us for the two products as
    they were). For the rollout graph (MappoTrainer); refreshed in place after each sync, as DenseLayouts are."""

    @torch.no_grad()
    def __init__(self, first: DenseLayouts, second: DenseLayouts):
        assert (first.layouts, first.out, first.weight.shape[0]) == (second.layouts, second.out, second.weight.shape[0])
        self.layouts, self.out = first.layouts, first.out
        self.weight_t = torch.empty((2 * first.weight.shape[1], first.weight.shape[0]), device=first.weight.device)
        self.bias = torch.empty((2 * first.weight.shape[1], 1), device=first.weight.device)
        self.refresh(first, second)

    @torch.no_grad()
    def refresh(self, first: DenseLayouts, second: DenseLayouts) -> None:
        width = first.weight.shape[1]
        self.weight_t[:width].copy_(first.weight.t())
        self.weight_t[width:].copy_(second.weight.t())
        self.bias[:width, 0].copy_(first.bias)
        self.bias[width:, 0].copy_(second.bias)

    def __call__(self, rows: torch.Tensor, layout: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """rows [N, in], layout [N] -> (first's output, second's) for each row's own layout, each [N, out]."""
        every = torch.addmm(self.bias, self.weight_t, rows.t()).view(2, self.layouts, self.out, rows.shape[0])
        index = layout.long().view(1, 1, 1, -1).expand(2, 1, self.out, rows.shape[0])
        own = every.gather(1, index).squeeze(1).transpose(1, 2).contiguous()
        return own[0], own[1]


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
def update_norms(norms: nn.ModuleList, obs: torch.Tensor, layout: torch.Tensor, obs_dims, ranks=None) -> None:
    """Fold a rollout's observations into each layout's statistics, each from its own rows and own features. With
    data-parallel `ranks` every layout is visited on every rank, rows or none: each is a collective."""
    if ranks is not None and ranks.active:
        for index, norm in enumerate(norms):
            norm.update(obs[layout == index, : obs_dims[index]], ranks)
        return
    for index, rows in _per_layout(layout, len(norms)):
        norms[index].update(obs[rows, : obs_dims[index]])


def per_layout(layout: torch.Tensor, count: int) -> list[tuple[int, torch.Tensor]]:
    """(layout, row indices) for every layout present in the flat batch `layout`, for the forward passes to share: the
    grouping is a unique and a nonzero per layout, which with 18 layouts is most of a small batch's forward cost."""
    return _per_layout(layout.reshape(-1), count)


def to_device(tensor: torch.Tensor, device: torch.device) -> torch.Tensor:
    """A small host tensor onto `device` without waiting: from pageable memory a copy to the GPU blocks the host until
    every kernel queued before it has run, which turns each index upload into a pipeline drain."""
    if device.type != "cuda":
        return tensor.to(device)
    return tensor.pin_memory().to(device, non_blocking=True)


def per_layout_host(layout: np.ndarray, count: int, device: torch.device) -> list[tuple[int, torch.Tensor]]:
    """per_layout for a batch whose layouts are still on the host (the rollout buffer's): the grouping is done in
    numpy and only the row indices go to the device, so building it never waits on the GPU."""
    flat = np.asarray(layout).reshape(-1)
    if count == 1:
        return [(0, to_device(torch.arange(flat.shape[0]), device))]
    return [(int(index), to_device(torch.from_numpy(np.flatnonzero(flat == index)), device))
            for index in np.unique(flat)]


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
    is sequential, which is the small part; everything before it runs in one pass. `dones` may be on the host even
    when the rest is on the GPU: the cut into pieces is made there, so passing it from the host saves a read back.

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
    inputs = encoded[to_device(times, device), to_device(piece_row[:, None].expand_as(times).contiguous(), device)]

    first = torch.nonzero(piece_start == 0, as_tuple=True)[0]
    initial = carried.new_zeros((len(piece_row), size)).index_copy(0, to_device(first, device),
                                                                   carried[to_device(piece_row[first], device)])

    packed = nn.utils.rnn.pack_padded_sequence(inputs, piece_length, batch_first=True, enforce_sorted=False)
    hx = initial.index_select(0, packed.sorted_indices)[None]
    # What nn.GRU.forward calls for a packed sequence, given the cell's parameters rather than a module's own.
    output, _ = torch._VF.gru(packed.data, packed.batch_sizes, hx,
                              [cell.weight_ih, cell.weight_hh, cell.bias_ih, cell.bias_hh], True, 1, 0.0,
                              cell.training, False)
    padded, _ = nn.utils.rnn.pad_packed_sequence(
        nn.utils.rnn.PackedSequence(output, packed.batch_sizes, packed.sorted_indices, packed.unsorted_indices),
        batch_first=True, total_length=longest)
    return padded[to_device(piece_of, device), to_device(position_of, device)]


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
        # The rollout copy's adapters and heads as DenseLayouts (densify); None everywhere else.
        self.dense_adapters: DenseLayouts | None = None
        self.dense_heads: DenseLayouts | None = None
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

    def fold_normalisation(self) -> None:
        """Fold every layout's RunningNorm into its adapter (fold_into): for the rollout copy only, which is
        overwritten from the trained network at every sync and folded again."""
        for norm, adapter in zip(self.norms, self.adapters):
            fold_into(norm, adapter)

    def densify(self, obs_width: int) -> None:
        """Adapters and heads as DenseLayouts, after fold_normalisation: for a rollout copy on the GPU only. Once
        built they are refreshed in place (DenseLayouts.refresh)."""
        if self.dense_adapters is None:
            self.dense_adapters = DenseLayouts(self.adapters, obs_width)
            self.dense_heads = DenseLayouts(self.heads, self.head_width)
        else:
            self.dense_adapters.refresh(self.adapters)
            self.dense_heads.refresh(self.heads)

    def initial_memory(self, *lead: int, device=None) -> torch.Tensor:
        """A cleared memory for `lead` rows (what an episode starts with)."""
        return torch.zeros((*lead, self.recurrent_size), dtype=torch.float32, device=device)

    def encode(self, obs: torch.Tensor, layout: torch.Tensor, groups=None) -> torch.Tensor:
        """Adapters and trunk for flat rows: everything that depends only on this decision's observation, before the
        GRU. A replayed sequence encodes every step in one pass and then carries the memory through them (carry),
        which is the difference between one large matmul per layer and one per step."""
        if self.dense_adapters is not None:
            return self.trunk(self.dense_adapters(obs, layout))
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
        return self.features_from(self.encode(obs, layout, groups), memory)

    def features_from(self, hidden: torch.Tensor, memory: torch.Tensor | None = None) -> torch.Tensor:
        """features() from the trunk's output on: the GRU, when there is one."""
        if self.memory is not None:
            carried = (memory.reshape(-1, self.recurrent_size) if memory is not None
                       else hidden.new_zeros(hidden.shape[0], self.recurrent_size))
            hidden = self.memory(hidden, carried)
        return hidden

    def action_distribution(self, features: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                            goal: torch.Tensor | None = None, groups=None) -> Categorical:
        """The actions of flat rows whose features are `features`, under `goal` where the actor has goals."""
        return Categorical(logits=self.action_logits(features, layout, mask, goal, groups))

    def action_logits(self, features: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                      goal: torch.Tensor | None = None, groups=None) -> torch.Tensor:
        """action_distribution's masked logits, unnormalised (for sample_logits)."""
        if self.goal_embedding is not None and goal is not None:
            features = features + self.goal_embedding(goal.reshape(-1))

        if self.dense_heads is not None:
            dense = self.dense_heads
            own = torch.where(dense.valid[layout.long()], dense(features, layout), MASKED_LOGIT)
            logits = own if own.shape[-1] == mask.shape[-1] else nn.functional.pad(
                own, (0, mask.shape[-1] - own.shape[-1]), value=MASKED_LOGIT)
            return masked_logits(logits, mask)

        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        logits = features.new_full((features.shape[0], mask.shape[-1]), MASKED_LOGIT)
        for index, rows in groups:
            logits[rows, : self.action_counts[index]] = self.heads[index](features[rows])
        return masked_logits(logits, mask)

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
        self.dense_adapters: DenseLayouts | None = None     # as LayoutActor's

    def encode(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
               goal: torch.Tensor | None = None, groups=None) -> torch.Tensor:
        """Everything that depends only on this decision -- the state, the seat's own observation and its goal --
        for flat rows, before the GRU. A replayed sequence encodes every step in one pass and then carries the memory
        through them (carry)."""
        hidden, own = self.encode_goal_free(state, obs, layout, groups)
        return self.encode_goal(hidden, own, goal)

    def encode_goal_free(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
                         groups=None) -> tuple[torch.Tensor, torch.Tensor]:
        """encode's first half, which does not need the goal: (the state's encoding, the seat's own). The larger
        part of the critic's work, so a rollout decision runs it beside the actor choosing the goal."""
        hidden = self.state_encoder(self.state_norm(state))
        if self.dense_adapters is not None:
            own = self.dense_adapters(obs, layout)
        else:
            own = torch.zeros_like(hidden)
            for index, rows in groups if groups is not None else _per_layout(layout, len(self.adapters)):
                own[rows] = self.adapters[index](self.norms[index](obs[rows, : self.obs_dims[index]]))
        return hidden, own

    def encode_goal(self, hidden: torch.Tensor, own: torch.Tensor, goal: torch.Tensor | None = None) -> torch.Tensor:
        """encode's second half: the goal, then the trunk."""
        if self.goal_embedding is not None and goal is not None:
            own = own + self.goal_embedding(goal.reshape(-1))
        return self.trunk(hidden + own)

    def fold_normalisation(self) -> None:
        """As LayoutActor.fold_normalisation, and the state's normaliser into the state encoder."""
        for norm, adapter in zip(self.norms, self.adapters):
            fold_into(norm, adapter)
        fold_into(self.state_norm, self.state_encoder)

    def densify(self, obs_width: int) -> None:
        """As LayoutActor.densify."""
        if self.dense_adapters is None:
            self.dense_adapters = DenseLayouts(self.adapters, obs_width)
        else:
            self.dense_adapters.refresh(self.adapters)

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
        return self.step_encoded(self.encode(state, obs, layout, goal, groups), lead, memory)

    def step_encoded(self, encoded: torch.Tensor, lead, memory: torch.Tensor | None = None):
        """step() from the encoding on: the GRU and the value head."""
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
