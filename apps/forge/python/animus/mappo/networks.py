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

#: The goal block's place slots (m2-goals; stage.json goals.place_slots): the values a slot has (sin and cos of the
#: bearing, distance, coverage, glimpse age) and the pointer's hidden width.
PLACE_FEATURES = 5
POINTER_HIDDEN = 16


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


def uniform_logits(logits: torch.Tensor) -> torch.Tensor:
    """Equal logits for every goal that is allowed, the masked ones still masked."""
    return torch.where(logits > MASKED_LOGIT / 2, torch.zeros_like(logits), logits)


def none_only_logits(logits: torch.Tensor) -> torch.Tensor:
    """Only the last column (none) allowed: a slot that holds nothing."""
    out = torch.full_like(logits, MASKED_LOGIT)
    out[:, -1] = 0.0
    return out


def log_prob_of(logits: torch.Tensor, choice: torch.Tensor) -> torch.Tensor:
    """log softmax(logits)[choice]: Categorical(logits=...).log_prob(choice), without building the distribution."""
    return logits.gather(-1, choice.long()[..., None]).squeeze(-1) - torch.logsumexp(logits, dim=-1)


def _entropy(logits: torch.Tensor) -> torch.Tensor:
    """The entropy of softmax(logits) per row, masked logits included (they carry ~0 probability)."""
    log_p = logits - torch.logsumexp(logits, dim=-1, keepdim=True)
    return -(log_p.exp() * log_p).sum(-1)


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
        # Columns kept at mean 0, variance 1 whatever is seen (hide): the camera's image, which the adapters never read
        # and the vision encoder reads raw. Made from the stage, not saved: None until hide() is called.
        self.register_buffer("hidden_columns", None, persistent=False)

    @torch.no_grad()
    def hide(self, columns: Sequence[int]) -> None:
        """Keep `columns` at the identity (mean 0, variance 1) through every update."""
        hidden = torch.zeros(self.mean.shape[0], dtype=torch.bool, device=self.mean.device)
        hidden[[c for c in columns if c < hidden.shape[0]]] = True
        if self.hidden_columns is not None:
            hidden |= self.hidden_columns
        self.hidden_columns = hidden
        self.mean.masked_fill_(hidden, 0.0)
        self.var.masked_fill_(hidden, 1.0)

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
        if self.hidden_columns is not None:
            self.mean.masked_fill_(self.hidden_columns, 0.0)
            self.var.masked_fill_(self.hidden_columns, 1.0)

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
def update_norms(norms: nn.ModuleList, obs: torch.Tensor, layout: torch.Tensor, obs_dims, ranks=None,
                 groups=None) -> None:
    """Fold a rollout's observations into each layout's statistics, each from its own rows and own features. With
    data-parallel `ranks` every layout is visited on every rank, rows or none: each is a collective. `groups` is
    _per_layout(layout) when the caller has it already (the actor's and the critic's norms read the same rows)."""
    if ranks is not None and ranks.active:
        for index, norm in enumerate(norms):
            norm.update(obs[layout == index, : obs_dims[index]], ranks)
        return
    for index, rows in groups if groups is not None else _per_layout(layout, len(norms)):
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
    was ~210,000 kernel launches per stage4_duel update; this took the update from 1.67 s to 1.29 s. (A hand-written
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


#: **The cell pointer** (free-choice-goals, stage.json goals.cells): a goal over the mental map's crop. The crop (48 x 48
#: cells of 2 yd, heading-up) is pooled CELL_POOL x CELL_POOL into a grid of CELL_GRID x CELL_GRID blocks; the head scores
#: every block and the draw is a block index. These are the contract's defaults; the manifest's values are what a run
#: uses (GoalHead.set_space).
CELL_GRID = 24
CELL_POOL = 2
CELL_FEATURES = 18
CELL_HIDDEN = 32
CELL_CHAIN_HIDDEN = 16
#: A block is choosable when it has at least this many floor-like cells, all within CELL_RISE_UNITS (4 yd) of the feet.
CELL_MIN_FLOOR = 2
CELL_RISE_UNITS = 16
#: The block offsets are scaled by this (the grid of the contract's pool 2).
CELL_OFFSET_SCALE = 24.0
#: A cell word is (ticket << CELL_BITS) | (cell + 1); tickets are 1..2^TICKET_BITS - 1.
CELL_BITS = 12
TICKET_BITS = 11
TICKET_COUNT = (1 << TICKET_BITS) - 1


def _block_sum(x: torch.Tensor, grid: int, pool: int) -> torch.Tensor:
    """[N, side, side] -> [N, grid, grid]: the sum over each pool x pool block."""
    return x.reshape(x.shape[0], grid, pool, grid, pool).sum(dim=(2, 4))


def cell_valid(crops: torch.Tensor, grid: int = CELL_GRID, pool: int = CELL_POOL, rise: int = CELL_RISE_UNITS,
               min_floor: int = CELL_MIN_FLOOR) -> torch.Tensor:
    """Which pooled blocks of the mental map's crop a cell goal may name, bool [N, grid * grid] (row-major, block (r, c)
    at r * grid + c) from the crop bytes [N, side x side x 6] (or [N, side, side, 6]) alone -- the contract's one
    definition (free-choice-goals CONTRACT 1.3), which the sim's CellGrid::Choosable implements too: no cell of the block
    is a Wall or a Hazard; at least `min_floor` are floor-like (Floor or Door); and every floor-like cell has a known
    height within `rise` units (4 yd) of the feet's. An unknown cell is not floor. A pure function of the bytes, with no
    branch on data: the rollout graph captures it."""
    side = grid * pool
    cells = crops.reshape(-1, side, side, MAP_CHANNELS)
    code = cells[..., MAP_CODE]
    height = cells[..., MAP_HEIGHT].to(torch.int16)
    like = (code == MAP_FLOOR) | (code == MAP_DOOR)
    barred = (code == MAP_WALL) | (code == MAP_HAZARD)
    off = like & ((height == 0) | ((height - MAP_HEIGHT_ZERO).abs() > rise))
    count = lambda mask: _block_sum(mask.to(torch.int32), grid, pool)
    return (count(barred) == 0) & (count(off) == 0) & (count(like) >= min_floor)


def cell_features(crops: torch.Tensor, grid: int = CELL_GRID, pool: int = CELL_POOL, rise: int = CELL_RISE_UNITS,
                  min_floor: int = CELL_MIN_FLOOR) -> tuple[torch.Tensor, torch.Tensor]:
    """The cell pointer's inputs from the crop bytes [N, side x side x 6]: (features [N, grid * grid, CELL_FEATURES],
    choosable bool [N, grid * grid]). Per block, over its pool x pool cells: the shares of Floor, Door, Wall, Hazard and
    Unknown; the share visited and the share on the frontier; whether any cell holds an entity; the age of the newest
    look at any seen cell (b / 255) and whether none was seen; the mean floor height over the feet ((b - 128) / 16,
    clamped to [-1, 1]); the 3 x 3 average over neighbouring blocks of the floor, wall, unknown and frontier shares; and
    the block's centre as forward, right and distance in the crop's yards over 48. All from the crop, which is the
    mental map's own (never the navmesh)."""
    side = grid * pool
    cells = crops.reshape(-1, side, side, MAP_CHANNELS)
    n = cells.shape[0]
    code = cells[..., MAP_CODE]
    height = cells[..., MAP_HEIGHT].to(torch.int16)
    area = float(pool * pool)
    share = lambda mask: _block_sum(mask.to(torch.float32), grid, pool) / area
    floor, door = share(code == MAP_FLOOR), share(code == MAP_DOOR)
    wall, hazard = share(code == MAP_WALL), share(code == MAP_HAZARD)
    unknown = share(code == 0)
    visited = share(cells[..., MAP_VISITED] != 0)
    frontier = share(cells[..., MAP_FRONTIER] != 0)
    entity = (_block_sum(((cells[..., MAP_CLASS] & CLASS_MASK) != 0).to(torch.float32), grid, pool) > 0).to(
        torch.float32)
    seen = code != 0
    age = torch.where(seen, cells[..., MAP_AGE], torch.full_like(cells[..., MAP_AGE], 255)).to(torch.float32)
    newest = age.reshape(n, grid, pool, grid, pool).amin(dim=(2, 4)) / 255.0
    never = (_block_sum(seen.to(torch.float32), grid, pool) == 0).to(torch.float32)
    like = (code == MAP_FLOOR) | (code == MAP_DOOR)
    rise_over = ((height - MAP_HEIGHT_ZERO).to(torch.float32) / float(rise)) * like.to(torch.float32)
    like_count = _block_sum(like.to(torch.float32), grid, pool)
    mean_height = (_block_sum(rise_over, grid, pool) / like_count.clamp(min=1.0)).clamp(-1.0, 1.0)
    around = nn.functional.avg_pool2d(torch.stack([floor, wall, unknown, frontier], dim=1), 3, stride=1, padding=1,
                                      count_include_pad=False)
    forward, right, distance = cell_geometry(grid, pool, cells.device)
    geometry = torch.stack([forward, right, distance], dim=0)[None].expand(n, -1, -1, -1)
    features = torch.cat([torch.stack([floor, door, wall, hazard, unknown, visited, frontier, entity, newest, never,
                                       mean_height], dim=1), around, geometry], dim=1)
    return features.reshape(n, CELL_FEATURES, grid * grid).transpose(1, 2), cell_valid(crops, grid, pool, rise,
                                                                                               min_floor)


def cell_geometry(grid: int, pool: int, device) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """Each block's centre from the body in yards (the crop's cells are 2 yd, heading-up, row 0 furthest ahead),
    [grid, grid] each: forward = side - pool * (2r + 1) and right = pool * (2c + 1) - side, over 48, and the distance
    over 48. For pool 2 on a 48-cell crop: forward = 46 - 4r, right = 4c - 46."""
    side = grid * pool
    rows = torch.arange(grid, device=device, dtype=torch.float32)
    forward = (side - pool * (2.0 * rows + 1.0)) / 48.0
    right = (pool * (2.0 * rows + 1.0) - side) / 48.0
    forward = forward[:, None].expand(grid, grid)
    right = right[None, :].expand(grid, grid)
    return forward, right, torch.sqrt(forward * forward + right * right)


class CellPointer(nn.Module):
    """The cell pointer's parameters (GoalHead.cell, `goal_head.cell.*` in a checkpoint): a block's own features
    (`local`, CELL_FEATURES -> CELL_HIDDEN, SiLU in the head), the slot's query (`query`, the head's feature width ->
    CELL_HIDDEN), the score (`score`, CELL_HIDDEN -> 1) and the chain feature (`chain`, the block's offset from the
    previous plan cell -> a score). The score and the chain's last layer start at zero: a fresh head draws uniformly over
    the choosable blocks."""

    def __init__(self, width: int):
        super().__init__()
        self.local = _linear(CELL_FEATURES, CELL_HIDDEN, math.sqrt(2))
        self.query = _linear(width, CELL_HIDDEN, 1.0)
        self.score = nn.Linear(CELL_HIDDEN, 1)
        nn.init.zeros_(self.score.weight)
        nn.init.zeros_(self.score.bias)
        self.chain = nn.Sequential(nn.Linear(3, CELL_CHAIN_HIDDEN), nn.Tanh(), nn.Linear(CELL_CHAIN_HIDDEN, 1))
        nn.init.orthogonal_(self.chain[0].weight, gain=1.0)
        nn.init.zeros_(self.chain[0].bias)
        nn.init.zeros_(self.chain[2].weight)
        nn.init.zeros_(self.chain[2].bias)


class GoalHead(nn.Module):
    """**Goals as a kind and a target** (long-horizon plan, Component C): a goal is kind * targets + target, one
    number on the wire and in the buffers. Its logits are the kind's, plus the target's, plus a learned table of the
    pair, over every (kind, target) -- a joint categorical parametrised in parts, so the target's score is shared
    across the kinds that can take it. Masked by the sim's goal block (GoalBlock: which kinds and targets are there
    now, the last block of each layout) and by which targets each kind accepts (stage.json "goals"). With one target
    it is exactly the old goal head over `kinds` goals."""

    def __init__(self, width: int, kinds: int, targets: int, layout_count: int, lookahead: bool = False,
                 slots: int = 1):
        super().__init__()
        self.kinds, self.targets = kinds, max(1, targets)
        # **Two goals and a queue** (next-run plan, Wave 4): with slots > 1 a choice draws the primary, then a
        # secondary held beside it and up to slots - 2 queued behind it, each conditioned on the ones before (a
        # learned bias per slot plus the embedding of what was drawn) and each able to say "none". The kind, target
        # and pair parameters are shared by every slot. The learner keeps the queue; the sim holds the two in front.
        self.slots = max(1, slots)
        # The entropy of each slot after the primary, as a share of the primary's (MappoConfig.goal_slot_entropy_weight).
        self.slot_entropy_weight = 0.1
        if self.slots > 1:
            self.slot_bias = nn.Parameter(torch.zeros(self.slots - 1, width))
            self.drawn = nn.Embedding(kinds * self.targets + 1, width)     # 0 = none, goal g at g + 1
            nn.init.zeros_(self.drawn.weight)
            self.none_bias = nn.Parameter(torch.zeros(self.slots - 1))
        self.kind = _linear(width, kinds, 0.01)
        self.target = _linear(width, self.targets, 0.01) if self.targets > 1 else None
        self.pair = nn.Parameter(torch.zeros(kinds, self.targets))
        # **Goal-level lookahead** (Component P, layer 3): for every candidate goal, the chance it is reached before
        # it ends and how long it would take (a share of a minute), predicted from the same features and trained on
        # what the chosen goals actually did. The choice reads them: the logits add each prediction times a learned
        # weight, so the policy chooses from what it expects of each goal rather than from a bare score.
        self.lookahead = lookahead
        if lookahead:
            self.success = _Factored(width, kinds, self.targets)
            self.duration = _Factored(width, kinds, self.targets)
            self.lookahead_weight = nn.Parameter(torch.tensor([1.0, -0.5]))
        self.register_buffer("accepts", torch.ones(kinds, self.targets, dtype=torch.bool))
        # Per layout, where its goal block starts in the observation; -1 for a layout without one.
        self.register_buffer("block_at", torch.full((max(1, layout_count),), -1, dtype=torch.long))
        self.has_space = False
        # **The pointer over place slots** (m2-goals W3a): the goal block (revision 3) carries per place slot k its
        # bearing, distance, coverage and glimpse age (PLACE_FEATURES values), and the target logit of place k is
        # raised by pointer(slot k's features) -- an MLP shared by the slots, so the head chooses over rooms by what it
        # knows of each, not by a slot id (slots are bound in discovery order, so an id means nothing across episodes).
        # The last layer starts at zero: a seeded head chooses exactly as it did.
        self.pointer = nn.Sequential(nn.Linear(PLACE_FEATURES, POINTER_HIDDEN), nn.Tanh(),
                                     nn.Linear(POINTER_HIDDEN, 1)) if self.targets > 1 else None
        if self.pointer is not None:
            nn.init.orthogonal_(self.pointer[0].weight, gain=1.0)
            nn.init.zeros_(self.pointer[0].bias)
            nn.init.zeros_(self.pointer[2].weight)
            nn.init.zeros_(self.pointer[2].bias)
        # Where the features are (stage.json "goals": columns.place_features and place_slots), as four numbers: the
        # first feature column inside the goal block, the first place target, how many slots and how many features a
        # slot; -1 throughout for no pointer (a manifest of goal block revision 2). A buffer, so a checkpoint plays the
        # same way where it is loaded without a stage (evaluate, export); the host copy is for the captured graphs.
        self.register_buffer("place_spec", torch.full((4,), -1, dtype=torch.long))
        self.place_at, self.place_first, self.place_count = -1, -1, 0
        # **The pointer over the map's cells** (free-choice-goals): a goal that is a block of the mental map's crop, scored
        # by `cell` (CellPointer) -- parameters of every goal head with targets, unused (and at zero) where the stage has
        # no cells. Where the crop sits is cell_spec, as four numbers: the byte the crop starts at in a row's image
        # bytes (after the camera's pixels), the grid, the pool and the joint goal id of a cell goal; -1 throughout for
        # no cell head (a manifest before goal block revision 4). A buffer, like place_spec; the host copy is for the
        # captured graphs.
        self.cell = CellPointer(width) if self.targets > 1 else None
        self.register_buffer("cell_spec", torch.full((4,), -1, dtype=torch.long))
        # The weight of the cell's entropy in the head's (MappoConfig.goal_cell_entropy_weight).
        self.cell_entropy_weight = 0.3
        self.cell_at, self.cell_grid, self.cell_pool, self.cell_joint = -1, CELL_GRID, CELL_POOL, -1
        self.cell_rise, self.cell_min_floor = CELL_RISE_UNITS, CELL_MIN_FLOOR
        # The goal block's column (from the block's start) of goal_from_present, -1 without: the planner's hindsight.
        self.cell_from = -1

    @property
    def count(self) -> int:
        return self.kinds * self.targets

    def _load_from_state_dict(self, state_dict, prefix, *args, **kwargs):
        super()._load_from_state_dict(state_dict, prefix, *args, **kwargs)
        # A loaded goal space (a checkpoint's block positions) says itself whether there is one.
        self.has_space = bool((self.block_at >= 0).any())
        self._mirror_place_spec()
        self._mirror_cell_spec()

    @property
    def block_width(self) -> int:
        """The goal block's columns (GoalBlock::Obs): kinds there, targets there, ended, reached, and from the
        next-run format on (slots > 1) the secondary ending, the event and what was achieved (kind and target
        one-hots; goal block revision 2: the order columns left it)."""
        base = self.kinds + self.targets + 2
        return base if self.slots <= 1 else base + 2 + (self.kinds + self.targets)

    # Where the next-run columns sit in the goal block (GoalBlock::Obs; stage.json "goals"."columns").
    @property
    def columns(self) -> dict[str, int]:
        base = self.kinds + self.targets + 2
        return {"secondary_ended": base, "event": base + 1, "achieved_kind": base + 2,
                "achieved_target": base + 2 + self.kinds}

    def set_space(self, accepts, block_at, goals: dict | None = None, image_offset: int = -1,
                  map_side: int = 0) -> str:
        """The goal space the sim wrote (stage.json "goals"): accepts [kinds][targets], and per layout the goal
        block's first observation column (-1 without one). `goals` is the manifest's "goals" object, which says where
        the place slots' features sit (columns.place_features, place_slots) and, from goal block revision 4, the cell
        goals (cells); without them the pointer, or the cell head, is off. `image_offset` is the byte the mental map's
        crop starts at in a row's image bytes (the camera's pixels before it; -1 without a map) and `map_side` the
        crop's cells a side. Returns a line about the pointers for the log."""
        self.accepts.copy_(torch.as_tensor(accepts, dtype=torch.bool, device=self.accepts.device))
        self.block_at.copy_(torch.as_tensor(block_at, dtype=torch.long, device=self.block_at.device))
        # Kept on the host: a rollout graph cannot read the device while it is captured.
        self.has_space = bool(np.any(np.asarray(block_at) >= 0))
        spec, note = self._place_spec_of(goals)
        self.place_spec.copy_(torch.as_tensor(spec, dtype=torch.long, device=self.place_spec.device))
        self._mirror_place_spec()
        cell_spec, cell_note = self._cell_spec_of(goals, image_offset, map_side)
        self.cell_spec.copy_(torch.as_tensor(cell_spec, dtype=torch.long, device=self.cell_spec.device))
        self._mirror_cell_spec()
        return f"{note}; {cell_note}"

    def _cell_spec_of(self, goals: dict | None, image_offset: int, map_side: int) -> tuple[list[int], str]:
        """The cell head's cell_spec from a manifest's "goals" (see __init__) and a line saying what it found. Sets the
        host constants the block features and the validity rule read (rise, floor cells, the hindsight column). Refused:
        a manifest whose cell constants this learner does not implement (codes, channels, field widths)."""
        off = [-1, -1, -1, -1]
        self.cell_rise, self.cell_min_floor, self.cell_from = CELL_RISE_UNITS, CELL_MIN_FLOOR, -1
        cells = (goals or {}).get("cells")
        if cells is None:
            return off, "no goals.cells in the manifest (goal block before revision 4): no cell head"
        if self.cell is None or self.slots <= 1:
            return off, "goals.cells in the manifest, but the goal head has one slot or no targets: no cell head"
        if image_offset < 0 or map_side <= 0:
            return off, "goals.cells in the manifest, but the stage's layouts have no mental map: no cell head"
        grid, pool, crop = int(cells["grid"]), int(cells["pool"]), int(cells["crop"])
        joint, target = int(cells["joint"]), int(cells["target"])
        if crop != grid * pool or crop != map_side:
            raise ValueError(f"goals.cells: a {grid} x {grid} grid of pool {pool} is not the {crop}-cell crop, or the "
                             f"map's crop is {map_side} cells a side")
        if int(cells.get("channels", MAP_CHANNELS)) != MAP_CHANNELS or \
                (int(cells["code_channel"]), int(cells["height_channel"])) != (MAP_CODE, MAP_HEIGHT) or \
                (int(cells["floor_code"]), int(cells["wall_code"]), int(cells["door_code"]),
                 int(cells["hazard_code"])) != (MAP_FLOOR, MAP_WALL, MAP_DOOR, MAP_HAZARD) or \
                int(cells["height_zero"]) != MAP_HEIGHT_ZERO:
            raise ValueError(f"goals.cells describes a crop this learner does not decode: {cells}")
        if int(cells["ticket_bits"]) != TICKET_BITS or int(cells["cell_bits"]) != CELL_BITS \
                or int(cells["positions"]) != 4 or grid * grid >= (1 << CELL_BITS):
            raise ValueError(f"goals.cells: cell words of {cells['ticket_bits']} + {cells['cell_bits']} bits over "
                             f"{cells['positions']} positions, this learner writes {TICKET_BITS} + {CELL_BITS} over 4")
        if not 0 <= joint < self.count or joint % self.targets != target or not 0 <= target < self.targets:
            raise ValueError(f"goals.cells: joint {joint} / target {target} is not in the {self.kinds} x "
                             f"{self.targets} goal space")
        self.cell_rise = int(cells.get("rise_units", CELL_RISE_UNITS))
        self.cell_min_floor = int(cells.get("min_floor_cells", CELL_MIN_FLOOR))
        columns = (goals or {}).get("columns") or {}
        width = int(columns.get("width", 0))
        if "from" in columns and int(columns["from"]) + 3 <= width:
            self.cell_from = int(columns["from"])
        return ([int(image_offset), grid, pool, joint],
                f"cell head over {grid} x {grid} blocks (pool {pool}) of the map crop at image byte {image_offset}, "
                f"cell goals are joint {joint}, source {cells.get('source', '?')}"
                + ("" if self.cell_from >= 0 else "; no goals.columns.from: no planner hindsight"))

    def _mirror_cell_spec(self) -> None:
        """The host copy of cell_spec (a rollout graph cannot read the device while it is captured)."""
        at, grid, pool, joint = (int(v) for v in self.cell_spec.tolist())
        on = self.cell is not None and at >= 0 and joint >= 0 and grid > 0 and pool > 0
        self.cell_at, self.cell_grid, self.cell_pool, self.cell_joint = (at, grid, pool, joint) if on \
            else (-1, CELL_GRID, CELL_POOL, -1)

    @property
    def cell_on(self) -> bool:
        return self.cell_joint >= 0 and self.slots > 1

    def _place_spec_of(self, goals: dict | None) -> tuple[list[int], str]:
        """The pointer's place_spec from a manifest's "goals" (see __init__) and a line saying what it found."""
        off = [-1, -1, -1, -1]
        columns = (goals or {}).get("columns") or {}
        slots = (goals or {}).get("place_slots")
        if self.pointer is None or not slots or "place_features" not in columns:
            return off, "no place slots in the manifest (goal block before revision 3): no pointer term"
        first, count, features = int(slots["first"]), int(slots["count"]), int(slots["features"])
        at = int(columns["place_features"])
        if features != PLACE_FEATURES:
            raise ValueError(f"goals.place_slots.features is {features}, the pointer reads {PLACE_FEATURES} "
                             f"({', '.join(slots.get('feature_names') or ())})")
        if first < 0 or count < 1 or first + count > self.targets:
            raise ValueError(f"goals.place_slots {first}..{first + count - 1} is not inside the {self.targets} goal "
                             f"targets")
        width = int(columns.get("width", at + count * features))
        if at + count * features > width:
            raise ValueError(f"goals.columns.place_features {at} + {count} x {features} reaches past the goal "
                             f"block's width {width}")
        return ([at, first, count, features],
                f"pointer over {count} place slots (targets {first}..{first + count - 1}) x {features} features "
                f"at goal block column {at}")

    def _mirror_place_spec(self) -> None:
        """The host copy of place_spec (a rollout graph cannot read the device while it is captured)."""
        at, first, count, features = (int(v) for v in self.place_spec.tolist())
        on = self.pointer is not None and at >= 0 and count > 0 and features == PLACE_FEATURES
        self.place_at, self.place_first, self.place_count = (at, first, count) if on else (-1, -1, 0)

    @property
    def pointer_on(self) -> bool:
        return self.place_count > 0

    def place_scores(self, obs: torch.Tensor, layout: torch.Tensor, dtype: torch.dtype) -> torch.Tensor | None:
        """The pointer's score per place slot, [rows, slots]: the slot's features read off the goal block, zero for a
        row whose layout has no goal block. None without a pointer. No branch on data: the row is a mask, so the
        rollout graph captures it."""
        if not self.pointer_on:
            return None
        obs = obs.reshape(-1, obs.shape[-1])
        at = self.block_at[layout.reshape(-1).long()]
        has = at >= 0
        span = self.place_count * PLACE_FEATURES
        columns = (at.clamp(min=0) + self.place_at)[:, None] + torch.arange(span, device=obs.device)[None, :]
        columns = columns.clamp(max=obs.shape[-1] - 1)
        values = obs.gather(1, columns).reshape(-1, self.place_count, PLACE_FEATURES)
        values = values * has[:, None, None].to(values.dtype)
        weight = self.pointer[0].weight
        return self.pointer(values.to(weight.dtype)).squeeze(-1).to(dtype)

    def _with_places(self, joint: torch.Tensor, obs: torch.Tensor | None, layout: torch.Tensor | None) -> torch.Tensor:
        """`joint` [rows, kinds, targets] with the pointer's score added to each place target's logit (every kind
        alike: the target's score is shared across the kinds that accept it)."""
        if obs is None or layout is None:
            return joint
        scores = self.place_scores(obs, layout, joint.dtype)
        if scores is None:
            return joint
        padded = nn.functional.pad(scores, (self.place_first, self.targets - self.place_first - self.place_count))
        return joint + padded[:, None, :]

    def _block(self, obs: torch.Tensor, layout: torch.Tensor):
        """The goal block's columns of flat rows, and which rows have one."""
        at = self.block_at[layout.long()]
        has = at >= 0
        columns = at.clamp(min=0)[:, None] + torch.arange(self.block_width, device=obs.device)[None, :]
        columns = columns.clamp(max=obs.shape[-1] - 1)
        return obs.gather(1, columns) > 0.5, has

    def signals(self, obs: torch.Tensor, layout: torch.Tensor) -> dict[str, torch.Tensor]:
        """What the goal block says of the goals held, per flat row: the primary ended, the secondary ended, an
        event (choose again now) and what was achieved this decision (achieved, -1 for nothing)."""
        obs = obs.reshape(-1, obs.shape[-1])
        block, has = self._block(obs, layout.reshape(-1))
        rows = obs.shape[0]
        k, t = self.kinds, self.targets
        base = k + t
        out = {"ended": block[:, base] & has}
        if self.slots <= 1:
            none = torch.zeros(rows, dtype=torch.bool, device=obs.device)
            out.update(secondary_ended=none, event=none,
                       achieved=torch.full((rows,), -1, dtype=torch.long, device=obs.device))
            return out
        c = self.columns
        out["secondary_ended"] = block[:, c["secondary_ended"]] & has
        out["event"] = block[:, c["event"]] & has
        achieved_kind = block[:, c["achieved_kind"]:c["achieved_kind"] + k]
        achieved_target = block[:, c["achieved_target"]:c["achieved_target"] + t].float().argmax(-1)
        out["achieved"] = torch.where(achieved_kind.any(-1) & has,
                                      achieved_kind.float().argmax(-1) * t + achieved_target,
                                      torch.full_like(achieved_target, -1))
        return out

    def _shifted(self, features: torch.Tensor, slot: int, drawn: list[torch.Tensor]) -> torch.Tensor:
        """The vector a slot after the primary scores from: the features plus the slot's bias and the embedding of
        each goal drawn before it."""
        shifted = features + self.slot_bias[slot - 1].to(features.dtype)
        for goal in drawn:
            shifted = shifted + self.drawn(goal.long() + 1).to(features.dtype)
        return shifted

    def slot_logits(self, features: torch.Tensor, slot: int, drawn: list[torch.Tensor],
                    obs: torch.Tensor | None = None, layout: torch.Tensor | None = None,
                    cell_ok: torch.Tensor | None = None) -> torch.Tensor:
        """Masked logits [rows, count + 1] of a slot after the primary, the last column being none: the shared
        parameters over the features plus the slot's bias and the embedding of what was drawn before it. The
        secondary and the queue are masked by what the goal block says is there, as the primary is: a queued goal
        about something absent was a draw from hundreds of goals that could never be pursued, and its entropy kept
        the whole head near uniform (next-run trial, 2026-09-30). `cell_ok` [rows] (with a cell head): whether a block
        is left for a cell goal after the cells drawn before this slot (the draw's own de-duplication); the cell goal
        is withdrawn where none is, so a plan never holds a cell the sim would find invalid."""
        shifted = self._shifted(features, slot, drawn)
        joint = self.kind(shifted)[:, :, None] + self.pair[None].to(features.dtype)
        if self.target is not None:
            joint = joint + self.target(shifted)[:, None, :]
        joint = self._with_places(joint, obs, layout)
        allowed = self.accepts[None].expand(features.shape[0], -1, -1)
        if obs is not None and layout is not None:
            block, has = self._block(obs.reshape(-1, obs.shape[-1]), layout.reshape(-1))
            present = block[:, : self.kinds, None] & block[:, self.kinds : self.kinds + self.targets][:, None, :]
            allowed = torch.where(has[:, None, None], allowed & present, allowed)
        allowed = allowed.reshape(features.shape[0], -1)
        goals = torch.arange(allowed.shape[-1], device=allowed.device)[None, :]
        if slot >= 2:
            # A queued goal is not one already drawn at this choice (the primary, the secondary, the queue so far): a
            # repeat of the primary ended lost on promotion, and a repeat in the queue was a wasted place. Not the cell
            # goal, which every cell shares (the cells' own de-duplication is the draw's, by block).
            for goal in drawn:
                repeat = (goal.long()[:, None] == goals) & (goal[:, None] >= 0)
                if self.cell_joint >= 0:
                    repeat = repeat & (goals != self.cell_joint)
                allowed = allowed & ~repeat
        if cell_ok is not None and self.cell_joint >= 0:
            allowed = allowed & ~((goals == self.cell_joint) & ~cell_ok[:, None])
        none = self.none_bias[slot - 1].to(features.dtype).expand(features.shape[0], 1)
        logits = torch.cat([masked_logits(joint.reshape(features.shape[0], -1), allowed), none], dim=-1)
        # A row whose layout has no goal block has only none.
        if obs is not None and layout is not None and self.has_space:
            _, has = self._block(obs.reshape(-1, obs.shape[-1]), layout.reshape(-1))
            only_none = torch.zeros_like(logits, dtype=torch.bool)
            only_none[:, -1] = True
            logits = torch.where(has[:, None], logits, logits.masked_fill(~only_none, MASKED_LOGIT))
        return logits

    def cell_inputs(self, image: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """What every slot's cell scores share, once per row, from the image bytes [rows, I] (the camera's pixels, then
        the mental map's crop): (the blocks' hidden features [rows, B, CELL_HIDDEN], the choosable mask [rows, B]),
        B = grid x grid."""
        side = self.cell_grid * self.cell_pool
        crops = image.reshape(image.shape[0], -1)[:, self.cell_at:self.cell_at + side * side * MAP_CHANNELS]
        x, valid = cell_features(crops, self.cell_grid, self.cell_pool, self.cell_rise, self.cell_min_floor)
        weight = self.cell.local.weight
        return nn.functional.silu(self.cell.local(x.to(weight.dtype))), valid

    def cell_scores(self, hidden: torch.Tensor, shifted: torch.Tensor, prev_row: torch.Tensor,
                    prev_col: torch.Tensor) -> torch.Tensor:
        """A slot's score per block [rows, B]: the score of tanh(the block's hidden features + the slot's query), plus
        the chain feature -- the block's offset (rows, columns, distance, over CELL_OFFSET_SCALE blocks) from the
        previous cell of the plan, `prev_row`, `prev_col` [rows] floats (the seat's own block at the start)."""
        weight = self.cell.query.weight
        query = self.cell.query(shifted.to(weight.dtype))
        score = self.cell.score(torch.tanh(hidden + query[:, None, :])).squeeze(-1)
        grid = self.cell_grid
        index = torch.arange(grid * grid, device=hidden.device)
        rows = torch.div(index, grid, rounding_mode="floor").to(hidden.dtype)
        columns = (index % grid).to(hidden.dtype)
        d_row = (rows[None, :] - prev_row[:, None].to(hidden.dtype)) / CELL_OFFSET_SCALE
        d_col = (columns[None, :] - prev_col[:, None].to(hidden.dtype)) / CELL_OFFSET_SCALE
        offset = torch.stack([d_row, d_col, torch.sqrt(d_row * d_row + d_col * d_col)], dim=-1)
        return score + self.cell.chain(offset).squeeze(-1)

    def cell_primary(self, features: torch.Tensor, image: torch.Tensor, block: torch.Tensor):
        """The primary slot's cell distribution scored again (the planner's hindsight and the update's statistics):
        (log probability of `block` [rows], whether that block is choosable, the distribution's entropy, whether any
        block is choosable). The slot is the first, so it scores from `features` as it was drawn, from the seat's own
        block, with every choosable block allowed."""
        hidden, valid = self.cell_inputs(image)
        seat = torch.full((features.shape[0],), (self.cell_grid - 1) / 2.0, dtype=hidden.dtype, device=hidden.device)
        logits = masked_logits(self.cell_scores(hidden, features, seat, seat).to(features.dtype), valid)
        block = block.long().clamp(0, valid.shape[-1] - 1)
        return (log_prob_of(logits, block), valid.gather(1, block[:, None])[:, 0], _entropy(logits),
                valid.any(dim=-1))

    def draw(self, features: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, deterministic: bool,
             slots: torch.Tensor | None = None, uniform: bool = False, cells: torch.Tensor | None = None,
             image: torch.Tensor | None = None, uniform_cells: bool = False, single: bool = False):
        """The slots drawn at a choice, or scored when `slots` [rows, S] (and `cells`) is given: (slots [rows, S] with -1
        for none, log probability, entropy, cells [rows, S] -- the block each slot named, -1 where its goal is not the
        cell goal). `uniform` (the eval arm random_goal): the primary is drawn uniformly over the goals on offer and the
        cell uniformly over the choosable blocks, and nothing is held beside it or queued. `uniform_cells`
        (random_cell): the cells are drawn uniformly, the goals from the head. `single` (no_plan): nothing is held
        beside the primary or queued, the primary from the head.

        With a cell head (and the image bytes): a slot whose goal is the cell goal also draws a block, from the blocks
        the crop says are choosable (cell_valid) less every block within one of a cell drawn before it in this choice,
        scored by the slot's query and by the offset from the previous cell of the plan (CellPointer); the joint and the
        block both count in the log probability, and the block in the entropy as `cell_entropy_weight` times its
        entropy where the cell goal is drawn."""
        cell_on = self.cell_on and image is not None
        primary_logits = self.logits(features, obs, layout)
        if uniform:
            primary_logits = uniform_logits(primary_logits)
        if slots is None:
            primary, primary_lp = sample_logits(primary_logits, deterministic)
        else:
            primary = slots[:, 0].clamp(min=0)
            primary_lp = log_prob_of(primary_logits, primary)
        log_prob = primary_lp
        entropy = _entropy(primary_logits)
        drawn, out = [primary], [primary]
        none = self.count
        rows = features.shape[0]
        out_cells = [torch.full_like(primary, -1)]

        if cell_on:
            hidden, valid = self.cell_inputs(image)
            grid = self.cell_grid
            index = torch.arange(grid * grid, device=features.device)
            block_row, block_col = torch.div(index, grid, rounding_mode="floor"), index % grid
            seat = (grid - 1) / 2.0
            previous = [torch.full((rows,), seat, dtype=features.dtype, device=features.device),
                        torch.full((rows,), seat, dtype=features.dtype, device=features.device)]
            earlier: list[tuple[torch.Tensor, torch.Tensor, torch.Tensor]] = []

            def allowed_blocks() -> torch.Tensor:
                allowed = valid
                for row, column, is_cell in earlier:
                    near = ((block_row[None, :] - row[:, None]).abs() <= 1) \
                        & ((block_col[None, :] - column[:, None]).abs() <= 1) & is_cell[:, None]
                    allowed = allowed & ~near
                return allowed

            def draw_cell(shifted: torch.Tensor, allowed: torch.Tensor, goal: torch.Tensor, slot: int):
                """(the block a cell goal names, -1 for another goal; its log probability; its entropy)."""
                logits = masked_logits(self.cell_scores(hidden, shifted, previous[0], previous[1]).to(features.dtype),
                                       allowed)
                if uniform or uniform_cells:
                    logits = uniform_logits(logits)
                if cells is None:
                    block, lp = sample_logits(logits, deterministic)
                else:
                    block = cells[:, slot].clamp(min=0)
                    lp = log_prob_of(logits, block)
                is_cell = goal == self.cell_joint
                row = torch.div(block, grid, rounding_mode="floor")
                column = block - row * grid
                earlier.append((row, column, is_cell))
                previous[0] = torch.where(is_cell, row.to(features.dtype), previous[0])
                previous[1] = torch.where(is_cell, column.to(features.dtype), previous[1])
                return (torch.where(is_cell, block, torch.full_like(block, -1)),
                        torch.where(is_cell, lp, torch.zeros_like(lp)), _entropy(logits))

            block, lp, cell_entropy = draw_cell(features, allowed_blocks(), primary, 0)
            out_cells[0] = block
            log_prob = log_prob + lp
            share = torch.softmax(primary_logits, dim=-1)[:, self.cell_joint]
            entropy = entropy + share * self.cell_entropy_weight * cell_entropy

        for slot in range(1, self.slots):
            if cell_on:
                allowed = allowed_blocks()
                logits = self.slot_logits(features, slot, drawn, obs, layout, allowed.any(dim=-1))
            else:
                logits = self.slot_logits(features, slot, drawn, obs, layout)
            if uniform or single:
                logits = none_only_logits(logits)
            if slots is None:
                choice, lp = sample_logits(logits, deterministic)
            else:
                choice = torch.where(slots[:, slot] < 0, torch.full_like(slots[:, slot], none), slots[:, slot])
                lp = log_prob_of(logits, choice)
            log_prob = log_prob + lp
            # The primary is the plan; the rest beside it are weighed at slot_entropy_weight, so the entropy bonus
            # does not grow with the number of slots and keep the primary near uniform.
            slot_entropy = _entropy(logits)
            goal = torch.where(choice == none, torch.full_like(choice, -1), choice)
            if cell_on:
                block, lp_cell, cell_entropy = draw_cell(self._shifted(features, slot, drawn), allowed, goal, slot)
                out_cells.append(block)
                log_prob = log_prob + lp_cell
                slot_entropy = slot_entropy + torch.softmax(logits, dim=-1)[:, self.cell_joint] \
                    * self.cell_entropy_weight * cell_entropy
            else:
                out_cells.append(torch.full_like(goal, -1))
            entropy = entropy + self.slot_entropy_weight * slot_entropy
            drawn.append(goal)
            out.append(goal)
        return torch.stack(out, dim=-1), log_prob, entropy, torch.stack(out_cells, dim=-1)

    def predictions(self, features: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """The lookahead: per candidate goal [rows, kinds * targets], the logit of reaching it and the share of a
        minute it would take."""
        return self.success(features), torch.sigmoid(self.duration(features))

    def logits(self, features: torch.Tensor, obs: torch.Tensor | None = None,
               layout: torch.Tensor | None = None) -> torch.Tensor:
        """Masked joint logits [rows, kinds * targets]."""
        joint = self.kind(features)[:, :, None] + self.pair[None].to(features.dtype)
        if self.target is not None:
            joint = joint + self.target(features)[:, None, :]
        joint = self._with_places(joint, obs, layout)
        if self.lookahead:
            # What each goal is expected to do, as the policy weighs it; the predictions learn from outcomes only.
            success, duration = self.predictions(features.detach())
            weight = self.lookahead_weight.to(features.dtype)
            joint = joint + (weight[0] * success.detach() + weight[1] * duration.detach()).reshape(joint.shape)
        allowed = self.accepts[None].expand(features.shape[0], -1, -1)
        if obs is not None and layout is not None and self.targets > 1:
            block, has = self._block(obs.reshape(-1, obs.shape[-1]), layout.reshape(-1))
            present = block[:, : self.kinds, None] & block[:, self.kinds : self.kinds + self.targets][:, None, :]
            # A layout with no goal block has no goals to choose: only the first, which means none.
            nothing = torch.zeros_like(allowed)
            allowed = torch.where(has[:, None, None], allowed & present, nothing if self.has_space else allowed)
        allowed = allowed.reshape(features.shape[0], -1).clone()
        # Always something to choose: the first goal (Fight about no one in particular) is allowed when nothing else
        # is on offer. A stage whose goal block always offers it (every stage but M2's room goals) is unchanged; M2
        # withdraws it once a place exists, so the head has to choose a place.
        allowed[:, 0] = allowed[:, 0] | ~allowed.any(dim=-1)
        return masked_logits(joint.reshape(features.shape[0], -1), allowed)


class _Factored(nn.Module):
    """A score per (kind, target) pair: the kind's plus the target's plus a table of the pair, as GoalHead's logits
    are built. [rows, kinds * targets]."""

    def __init__(self, width: int, kinds: int, targets: int):
        super().__init__()
        self.kinds, self.targets = kinds, targets
        self.kind = _linear(width, kinds, 0.01)
        self.target = _linear(width, targets, 0.01) if targets > 1 else None
        self.pair = nn.Parameter(torch.zeros(kinds, targets))

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        joint = self.kind(features)[:, :, None] + self.pair[None].to(features.dtype)
        if self.target is not None:
            joint = joint + self.target(features)[:, None, :]
        return joint.reshape(features.shape[0], -1)


def attach_blind_columns(network: nn.Module, columns: dict[int, list[int]], tag: str) -> None:
    """Keep layout adapters blind to some of their observation columns (layout index -> columns): the weights start at
    zero and their gradient is masked, so they stay zero through every update and the rollout copies' folding. For
    columns the policy must not read directly: the camera's. Once per network and tag; a later call only zeroes
    again."""
    for index, cols in columns.items():
        if not cols or index >= len(network.adapters):
            continue
        weight = network.adapters[index].weight
        name = f"{tag}_keep_{index}"
        keep = torch.ones((1, weight.shape[1]), dtype=weight.dtype, device=weight.device)
        keep[0, [c for c in cols if c < weight.shape[1]]] = 0.0
        if hasattr(network, name):
            getattr(network, name).copy_(keep)
        else:
            network.register_buffer(name, keep)
            weight.register_hook(lambda grad, network=network, name=name: grad * getattr(network, name))
        with torch.no_grad():
            weight.mul_(getattr(network, name))


def clear_blind_columns(network: nn.Module) -> None:
    """Zero every seat layout's camera columns (VisionEncoder) again (after a seed or a load brought weights from
    elsewhere)."""
    with torch.no_grad():
        for index in range(len(network.adapters)):
            for tag in ("vision",):
                keep = getattr(network, f"{tag}_keep_{index}", None)
                if keep is not None:
                    network.adapters[index].weight.mul_(keep)


#: The vision block's name in stage.json (BlockId::Vision); its manifest entry carries the camera image's shape.
VISION_BLOCK = "vision"


#: The camera's bytes a pixel (protocol 26, Vision::BYTES_PER_PIXEL): distance, height, normal and the class byte (the
#: semantic class and the objective bit), of the static world alone (vision block revision 6: units and objects are
#: in the entity list, not the image).
IMAGE_BYTES_PER_PIXEL = 4
#: The decoded image channels, as the sim's Vision::DecodePixel lays them out: the class is channel 3 (an index, which
#: the encoder reads as one-hot planes over the manifest's pixel_classes), the objective channel 4.
IMAGE_CHANNELS = 5
IMAGE_CLASS_CHANNEL = 3
#: The class byte's bits (Camera.h): the class in the low five, the objective in bit 5.
CLASS_MASK = 0x1F
#: The classes the wire can carry (5 bits): the size of the class embedding's table, so a class added later needs no
#: new shape.
CLASS_LIMIT = 32
#: The encoder's patch size when the manifest names none (revision 3 and before: 4 x 4 at 64 x 32).
DEFAULT_PATCH = 4
#: The entity list's block in stage.json (BlockId::Entities), which comes with a camera.
ENTITIES_BLOCK = "entities"
#: The sight block in stage.json (BlockId::Sight, dungeon-curriculum I1 and I2): the visible and remembered entities
#: as one list, and the pointer presses on it.
SIGHT_BLOCK = "sight"
#: The mental map's block in stage.json (BlockId::Map, perception-goals REDESIGN §3): its manifest's "map" describes
#: the crop that travels as bytes after the image (protocol 24).
MAP_BLOCK = "map"
#: The crop's byte channels (Vision::CropChannel), and the codes (Vision::MapCode: unknown, floor, wall, door, hazard).
MAP_CHANNELS = 6
MAP_CODES = 5
MAP_CODE, MAP_HEIGHT, MAP_VISITED, MAP_AGE, MAP_CLASS, MAP_FRONTIER = range(MAP_CHANNELS)
MAP_HEIGHT_ZERO = 128
MAP_FLOOR, MAP_WALL, MAP_DOOR, MAP_HAZARD = 1, 2, 3, 4      # the crop codes (Vision::MapCode; 0 unknown)


def _map_of(entry: dict, name: str) -> dict | None:
    """A layout's mental map as its map block describes it -- {"first" (its scalar columns), "scalars", "height",
    "width", "cell", "channels", "map_bytes", "codes", "classes"} -- or None without one. Refused: a crop this learner
    does not decode (its channels or codes differ, or its bytes are not height x width x channels)."""
    block = next((b for b in entry.get("blocks", ()) if b.get("name") == MAP_BLOCK), None)
    if block is None:
        return None
    described = block.get("map")
    if not described:
        raise ValueError(f"{name}: stage.json has a map block without its description")
    out = {key: int(described[key]) for key in ("height", "width", "channels", "map_bytes", "codes", "classes",
                                                 "scalars")}
    out["cell"] = float(described.get("cell", 2.0))
    out["first"] = int(block["obs"][0])
    if (out["channels"], out["codes"]) != (MAP_CHANNELS, MAP_CODES) or not 1 <= out["classes"] <= CLASS_LIMIT \
            or out["map_bytes"] != out["height"] * out["width"] * out["channels"]:
        raise ValueError(f"{name}: a map crop of {out['height']} x {out['width']} x {out['channels']} channels "
                         f"({out['map_bytes']} bytes, {out['codes']} codes, {out['classes']} classes); this learner "
                         f"decodes {MAP_CHANNELS} channels and {MAP_CODES} codes, height x width x channels bytes")
    if int(block["obs"][1]) != out["scalars"]:
        raise ValueError(f"{name}: the map block is {block['obs'][1]} columns, its scalars {out['scalars']}")
    return out


def _entities_of(entry: dict, name: str) -> dict | None:
    """A layout's entity list as its entities block describes it (perception-goals 1b): {"first", "slots", "width",
    "present", "class_column", "type_column", "object_column", "classes", "type_buckets"}, or None without one."""
    block = next((b for b in entry.get("blocks", ()) if b.get("name") == ENTITIES_BLOCK), None)
    if block is None:
        return None
    described = block.get("entities")
    if not described:
        raise ValueError(f"{name}: stage.json has an entities block without its description")
    features = list(described.get("features", ()))
    out = {key: int(described[key]) for key in ("first", "slots", "width", "present", "class_column", "type_column",
                                                 "classes", "type_buckets")}
    out["object_column"] = features.index("object") if "object" in features else -1
    out["memory_column"] = features.index("memory") if "memory" in features else -1
    first, count = int(block["obs"][0]), int(block["obs"][1])
    if out["first"] != first or out["slots"] * out["width"] != count:
        raise ValueError(f"{name}: the entities block spans {count} columns from {first}, its description "
                         f"{out['slots']} slots of {out['width']} from {out['first']}")
    return out


def _sight_of(entry: dict, name: str, entities: dict | None) -> dict | None:
    """A layout's sight list as its sight block describes it (dungeon-curriculum I1, I2): {"first", "slots",
    "visible_slots", "width", "present", "class_column", "type_column", "object_column", "memory_column",
    "visible_column", "memory_ids", "type_buckets", "presses", "pointer_first"} -- `pointer_first` each press's first
    action in the layout -- or None without one. Refused: a list without the camera's entity list, or whose visible
    half or leading columns are not that list's (its encoder is the list's own)."""
    block = next((b for b in entry.get("blocks", ()) if b.get("name") == SIGHT_BLOCK), None)
    if block is None:
        return None
    described = block.get("sight")
    if not described:
        raise ValueError(f"{name}: stage.json has a sight block without its description")
    out = {key: int(described[key]) for key in ("first", "slots", "visible_slots", "width", "present", "class_column",
                                                 "type_column", "object_column", "memory_column", "visible_column",
                                                 "memory_ids", "type_buckets")}
    pointers = list(described.get("pointers", ()))
    out["presses"] = tuple(str(p["press"]) for p in pointers)
    out["pointer_first"] = tuple(int(p["first"]) for p in pointers)
    # The named row (revision 2, M3 interact): what the goal names, after the slots -- the entity list's columns,
    # then the task one-hot. Absent (revision 1): none.
    named = described.get("named")
    out["named_offset"] = int(named["offset"]) if named else -1
    out["named_width"] = int(named["width"]) if named else 0
    out["named_entity_width"] = int(named["entity_width"]) if named else 0
    first, count = int(block["obs"][0]), int(block["obs"][1])
    if out["first"] != first or out["slots"] * out["width"] + out["named_width"] != count:
        raise ValueError(f"{name}: the sight block spans {count} columns from {first}, its description "
                         f"{out['slots']} slots of {out['width']} and a named row of {out['named_width']} from "
                         f"{out['first']}")
    if named and (out["named_offset"] != out["slots"] * out["width"]
                  or out["named_entity_width"] >= out["named_width"]):
        raise ValueError(f"{name}: the sight block's named row ({named}) does not follow its slots")
    if any(int(p["count"]) != out["slots"] for p in pointers):
        raise ValueError(f"{name}: a sight press names {[int(p['count']) for p in pointers]} slots, the list has "
                         f"{out['slots']}")
    if entities is None:
        raise ValueError(f"{name}: a sight block without the camera's entity list, whose encoder it shares")
    if (out["visible_slots"] != entities["slots"] or out["width"] <= entities["width"]
            or any(out[key] != entities[key] for key in ("class_column", "type_column", "object_column",
                                                         "memory_column", "type_buckets"))):
        raise ValueError(f"{name}: the sight list ({out}) does not lead with the entity list's slots and columns "
                         f"({entities}): it reads them with that list's encoder")
    return out


def vision_of(stage: dict | None, layout_names: Sequence[str]) -> list[dict | None] | None:
    """Per layout, in `layout_names` order, its camera as the vision block of stage.json describes it -- {"first": the
    block's first observation column (its scalars), "height", "width", "channels", "classes", "class_channel",
    "scalars", "bytes_per_pixel", "pixel_classes", "patch", "render_sizes", "look", "look_names", "image_bytes",
    "entities"} -- or None
    for a layout without one; None altogether when no layout has a camera (no encoder, nothing new in the networks).
    The block's first column differs per layout (the core block before it is as wide as the class's spells), so every
    layout keeps its own; so does its entity list ("entities", _entities_of: the entities block after the camera).

    The image travels as bytes beside the observation (protocol 21, Spec.image_bytes): the block's columns are its
    scalars alone. Revision 4 (camera-vision.FREELOOK.md) added the encoder's "patch" (4 when absent), the
    "render_sizes" the sim draws from (scaled up to the canonical height x width it sends) and the free look's heads,
    `"look": {"heads": [7, 5, 5], "names": [...]}` (() when absent: no look head). Revision 5 (perception-goals P2):
    the semantic class in place of the kind, and an entity slot as a fifth byte. Revision 6 (entity sensing): four
    bytes a pixel again, of the static world alone, and `"pixel_classes"`, the classes a pixel can carry (the
    encoder's one-hot planes, in that order). Refused: a vision block without its image, or from before revision 6
    (no pixel classes, five bytes a pixel), layouts whose images differ (one encoder reads them all), and a block
    whose width is not its scalars."""
    if stage is None:
        return None
    layouts = stage.get("layouts") or {}
    out: list[dict | None] = []
    shape = None
    for name in layout_names:
        entry = layouts.get(name) or {}
        block = next((b for b in entry.get("blocks", ()) if b.get("name") == VISION_BLOCK), None)
        if block is None:
            out.append(None)
            continue
        image = block.get("image")
        if not image:
            raise ValueError(f"{name}: stage.json has a vision block without its image (the sim's manifest is older "
                             f"than the learner's camera); its columns would reach the adapters raw")
        if image.get("transport") != "bytes":
            raise ValueError(f"{name}: the vision block's image is not sent as bytes (transport "
                             f"{image.get('transport')!r}): a sim from before protocol 21, which this learner does "
                             f"not read")
        if "classes" not in image or "pixel_classes" not in image:
            raise ValueError(f"{name}: the vision block's image has no class table or no pixel classes (revision "
                             f"{block.get('revision')}, {image.get('bytes_per_pixel')} bytes a pixel): a sim from "
                             f"before revision 6 (protocol 26), which this learner does not read")
        described = {key: int(image[key]) for key in ("height", "width", "channels", "classes", "class_channel",
                                                      "scalars", "bytes_per_pixel")}
        described["patch"] = int(image.get("patch", DEFAULT_PATCH))
        described["class_limit"] = int(image.get("class_limit", CLASS_LIMIT))
        described["pixel_classes"] = tuple(int(value) for value in image["pixel_classes"])
        if not described["pixel_classes"] or len(set(described["pixel_classes"])) != len(described["pixel_classes"]) \
                or any(not 0 <= value < described["class_limit"] for value in described["pixel_classes"]):
            raise ValueError(f"{name}: the vision block's pixel classes {list(described['pixel_classes'])} are not "
                             f"distinct classes of the {described['class_limit']} the wire carries")
        described["render_sizes"] = tuple(tuple(int(v) for v in size) for size in image.get("render_sizes", ()))
        look = block.get("look") or image.get("look") or {}
        heads = tuple(int(head) for head in look.get("heads", ()))
        look_names = tuple(str(n) for n in look.get("names", ()))
        if any(head < 1 for head in heads) or (look_names and len(look_names) != len(heads)):
            raise ValueError(f"{name}: the vision block's look heads {list(heads)} (names {list(look_names)}): every "
                             f"head needs a choice, and a name if any has one")
        described["look"], described["look_names"] = heads, look_names
        if described["patch"] < 1:
            raise ValueError(f"{name}: the vision block's patch is {described['patch']}")
        for width, height in described["render_sizes"]:
            if not (1 <= width <= described["width"] and 1 <= height <= described["height"]):
                raise ValueError(f"{name}: render size {width}x{height} is not within the canonical "
                                 f"{described['width']}x{described['height']} image")
        if (described["bytes_per_pixel"], described["channels"], described["class_channel"],
                described["class_limit"]) != (IMAGE_BYTES_PER_PIXEL, IMAGE_CHANNELS, IMAGE_CLASS_CHANNEL,
                                              CLASS_LIMIT) or not 1 <= described["classes"] <= CLASS_LIMIT:
            raise ValueError(f"{name}: a vision image of {described['bytes_per_pixel']} bytes a pixel decoded to "
                             f"{described['channels']} channels (class {described['class_channel']}, "
                             f"{described['classes']} of {described['class_limit']} classes); this learner decodes "
                             f"{IMAGE_BYTES_PER_PIXEL} bytes to {IMAGE_CHANNELS} channels, the class channel "
                             f"{IMAGE_CLASS_CHANNEL}, at most {CLASS_LIMIT} classes")
        if shape is None:
            shape = described
        elif described != shape:
            raise ValueError(f"{name}: vision image {described}, another layout's {shape}; one encoder needs one "
                             f"shape")
        first, count = int(block["obs"][0]), int(block["obs"][1])
        if count != described["scalars"]:
            raise ValueError(f"{name}: the vision block is {count} columns, its scalars {described['scalars']} (the "
                             f"image travels as bytes beside the observation)")
        obs_dim = entry.get("obs_dim")
        if obs_dim is not None and first + count > int(obs_dim):
            raise ValueError(f"{name}: the vision block ends at {first + count}, past the layout's {obs_dim} columns")
        image_bytes = described["height"] * described["width"] * described["bytes_per_pixel"]
        crop = _map_of(entry, name)
        listed = _entities_of(entry, name)
        described_layout = {"first": first, **described, "image_bytes": image_bytes, "entities": listed, "map": crop,
                            "camera_bytes": image_bytes + (crop["map_bytes"] if crop else 0)}
        sight = _sight_of(entry, name, listed)
        if sight is not None:
            described_layout["sight"] = sight
        out.append(described_layout)
    maps = [entry["map"] for entry in out if entry is not None]
    if any(crop is None for crop in maps) and any(crop is not None for crop in maps):
        raise ValueError("some layouts with a camera have a mental map and some do not: one encoder reads them all")
    return out if any(entry is not None for entry in out) else None


def vision_image_bytes(vision: list[dict | None] | None) -> int:
    """The bytes of an agent's camera row stage.json's camera makes, 0 without one: its image (height x width x bytes
    a pixel, the sim's Spec.image_bytes) and, with a map block, its map crop after it (Spec.map_bytes) -- the row the
    buffer keeps and the encoder reads (Spec.camera_bytes)."""
    return next((entry.get("camera_bytes", entry["image_bytes"]) for entry in vision or () if entry is not None), 0)


def vision_look_heads(vision: list[dict | None] | None) -> tuple[int, ...]:
    """The free look's heads stage.json's camera has (revision 4: (7, 5, 5)), () without a camera or a look."""
    return next((tuple(entry.get("look", ())) for entry in vision or () if entry is not None), ())


def check_look_heads(vision: list[dict | None] | None, spec_look_heads: int) -> None:
    """Refuse a sim and a stage.json that disagree about the look (protocol 22): SPEC's LookHeads against the number
    of heads in the vision block's "look"."""
    heads = vision_look_heads(vision)
    if len(heads) != int(spec_look_heads):
        raise ValueError(f"the sim's SPEC announces {spec_look_heads} look heads, stage.json's vision block has "
                         f"{len(heads)} ({list(heads)})")


def check_image_bytes(vision: list[dict | None] | None, spec_image_bytes: int, spec_map_bytes: int = 0) -> None:
    """Refuse a sim and a stage.json that disagree about the camera: SPEC's image bytes per agent against stage.json's
    height x width x bytes a pixel, or one side with a camera and the other without; and about the mental map's crop
    (protocol 24): SPEC's map bytes against the map block's."""
    crop = next((entry.get("map") for entry in vision or () if entry is not None), None)
    expected_map = crop["map_bytes"] if crop else 0
    if expected_map != int(spec_map_bytes):
        raise ValueError(f"the sim sends {spec_map_bytes} map bytes an agent, stage.json's map block makes "
                         f"{expected_map}")
    expected = next((entry["image_bytes"] for entry in vision or () if entry is not None), 0)
    if expected == int(spec_image_bytes):
        return
    if not expected:
        raise ValueError(f"the sim sends {spec_image_bytes} image bytes an agent, but stage.json has no camera (no "
                         f"vision block with an image)")
    if not spec_image_bytes:
        raise ValueError(f"stage.json has a camera ({expected} bytes an agent), but the sim's SPEC sends no image")
    raise ValueError(f"the sim sends {spec_image_bytes} image bytes an agent, stage.json's camera makes {expected} "
                     f"(height x width x bytes a pixel)")


def decode_image(image: torch.Tensor, height: int, width: int) -> torch.Tensor:
    """The camera's bytes [N, H x W x 4] uint8 as its five float image channels [N, H, W, 5] -- distance, height,
    normal, class, objective -- exactly as Vision::DecodePixel: 255 -> 1.0 (sky) else b / 254; (b - 128) / 125;
    b / 255; b & 31; (b >> 5) & 1. On the device: the bytes are what the rollout keeps."""
    pixels = image.reshape(-1, height, width, IMAGE_BYTES_PER_PIXEL)
    distance, rise, normal, flags = pixels[..., 0], pixels[..., 1], pixels[..., 2], pixels[..., 3]
    as_float = lambda value: value.to(torch.float32)
    return torch.stack([
        torch.where(distance == 255, torch.ones_like(as_float(distance)), as_float(distance) / 254.0),
        (as_float(rise) - 128.0) / 125.0,
        as_float(normal) / 255.0,
        as_float(flags & CLASS_MASK),
        as_float((flags >> 5) & 1),
    ], dim=-1)


def decode_map(crops: torch.Tensor, height: int, width: int) -> dict[str, torch.Tensor]:
    """The mental map's crop bytes [N, H x W x 6] uint8 as the encoder reads them (Vision::DecodeCropCell): "code" and
    "class" [N, H, W] long (indexes it embeds), and "values" [N, H, W, 5] float -- the floor's height over the feet
    ((b - 128) / 127, 0 with none), whether a floor is known, visited, the newest look's age (b / 255, 1 never seen)
    and the frontier."""
    cells = crops.reshape(-1, height, width, MAP_CHANNELS)
    as_float = lambda value: value.to(torch.float32)
    rise = cells[..., MAP_HEIGHT]
    known = rise != 0
    return {
        "code": cells[..., MAP_CODE].long().clamp(0, MAP_CODES - 1),
        "class": (cells[..., MAP_CLASS] & CLASS_MASK).long(),
        "values": torch.stack([
            torch.where(known, (as_float(rise) - MAP_HEIGHT_ZERO) / 127.0, torch.zeros_like(as_float(rise))),
            as_float(known),
            as_float(cells[..., MAP_VISITED] != 0),
            as_float(cells[..., MAP_AGE]) / 255.0,
            as_float(cells[..., MAP_FRONTIER] != 0),
        ], dim=-1),
    }


def _spatial_softmax(features: torch.Tensor, grid_x: torch.Tensor, grid_y: torch.Tensor) -> torch.Tensor:
    """Per channel of [N, C, H, W] the expected (x, y) of its softmax over the grid, [N, 2C]."""
    flat = features.flatten(2)
    weights = torch.softmax(flat, dim=-1)
    x = (weights * grid_x.to(weights.dtype)).sum(-1)
    y = (weights * grid_y.to(weights.dtype)).sum(-1)
    return torch.cat([x, y], dim=-1)


class MapEncoder(nn.Module):
    """**The mental map's encoder** (perception-goals REDESIGN §3, the Change): the one heading-up crop (48 x 48 cells
    of 2 yd, protocol 24's bytes after the camera's image), shared by every layout as the camera is. Per cell, the code
    embedded (CODE_EMBED wide), the entity's class through the camera's own class embedding, and five values
    (decode_map): CODE_EMBED + the class embedding + 5 planes (15). **Patches**, as the camera's: PATCH x PATCH cells
    (8 yd square at 2 yd; a 12 x 12 grid at 48 x 48) through Linear -> 64 + SiLU and Linear 64 -> 64 + SiLU, each
    channel's spatial-softmax keypoint over the grid, the channels' mean, the block's scalars (its own columns, read
    raw), and Linear -> EMBED. A convolution stack at the crop's own resolution was measured first: 3.6 times the
    camera's update cost on the host's card, the patches a fraction of it.

    **The join** (`join`, orthogonal): EMBED -> the camera's embedding width, after a SiLU, added to the camera's
    embedding before its own SiLU, so both networks read the map through their own camera join. Seeding from a
    checkpoint without a map zeroes it (bootstrap._seed_map): the seeded policy starts as it was, and the map comes in
    as the join learns."""

    CODE_EMBED = 4
    PATCH = 4
    WIDTHS = (64, 64)
    EMBED = 128

    def __init__(self, crop: dict, class_embedding: nn.Embedding, out_width: int, starts: Sequence[int]):
        super().__init__()
        self.height, self.width = crop["height"], crop["width"]
        if self.height % self.PATCH or self.width % self.PATCH:
            raise ValueError(f"the map's crop is {self.width} x {self.height}; its encoder cuts it into {self.PATCH} x "
                             f"{self.PATCH} patches")
        self.map_bytes, self.scalars = crop["map_bytes"], crop["scalars"]
        self.code_embed = nn.Embedding(MAP_CODES, self.CODE_EMBED)
        # The camera's class embedding, held by reference: one table for the pixels, the list and the map.
        self.__dict__["class_embedding"] = class_embedding
        self.planes_per_cell = self.CODE_EMBED + class_embedding.embedding_dim + 5
        self.grid = (self.height // self.PATCH, self.width // self.PATCH)
        self.patch = nn.Linear(self.PATCH * self.PATCH * self.planes_per_cell, self.WIDTHS[0])
        self.mix = nn.Linear(self.WIDTHS[0], self.WIDTHS[1])
        grid_y, grid_x = torch.meshgrid(torch.linspace(-1.0, 1.0, self.grid[0]),
                                        torch.linspace(-1.0, 1.0, self.grid[1]), indexing="ij")
        self.register_buffer("grid_x", grid_x.reshape(-1), persistent=False)
        self.register_buffer("grid_y", grid_y.reshape(-1), persistent=False)
        self.embed = nn.Linear(3 * self.WIDTHS[1] + self.scalars, self.EMBED)
        self.join = _linear(self.EMBED, out_width, math.sqrt(2))
        # Each layout's first map column (the scalars), -1 for a layout without the map.
        self.register_buffer("start", torch.tensor(list(starts), dtype=torch.long), persistent=False)
        self.register_buffer("offsets", torch.arange(self.scalars, dtype=torch.long), persistent=False)

    def planes(self, crops: torch.Tensor) -> torch.Tensor:
        """[N, M] bytes -> [N, H, W, P] planes: the code's embedding, the class's, the five values."""
        decoded = decode_map(crops, self.height, self.width)
        dtype = self.embed.weight.dtype
        # The embeddings' backward (a scatter of 2,304 cells a row onto a few rows) is about two thirds of the
        # encoder's update on the host's card; one-hot products instead measured no faster (the code's) or slower
        # (the class's: a 32-wide plane a cell), so they stay lookups.
        return torch.cat([self.code_embed(decoded["code"]).to(dtype), self.class_embedding(decoded["class"]).to(dtype),
                          decoded["values"].to(dtype)], dim=-1)

    def patches(self, planes: torch.Tensor) -> torch.Tensor:
        """[N, H, W, P] -> [N, patches, PATCH x PATCH x P], row by row as the camera's."""
        rows, cols = self.grid
        cut = planes.reshape(planes.shape[0], rows, self.PATCH, cols, self.PATCH, self.planes_per_cell)
        return cut.permute(0, 1, 3, 2, 4, 5).reshape(planes.shape[0], rows * cols, -1)

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, crops: torch.Tensor) -> torch.Tensor:
        """What the map adds to the camera's embedding (before its SiLU), [N, out_width]."""
        silu = nn.functional.silu
        layout = layout.reshape(-1).long()
        columns = (self.start[layout].clamp(min=0)[:, None] + self.offsets[None, :]).clamp(max=obs.shape[-1] - 1)
        scalars = obs.gather(1, columns).to(self.embed.weight.dtype)
        features = silu(self.mix(silu(self.patch(self.patches(self.planes(crops))))))     # [N, patches, C]
        grid = features.transpose(1, 2).reshape(features.shape[0], -1, *self.grid)
        pooled = torch.cat([_spatial_softmax(grid, self.grid_x, self.grid_y), features.mean(dim=1), scalars], dim=-1)
        return self.join(silu(self.embed(pooled)))


class VisibleEntities(nn.Module):
    """**The entity list** (perception-goals 1b; entity sensing): the entities block's slots read as a set -- one
    encoder shared by every slot and layout, the present slots pooled (mean and max) -- with a change to how a slot is
    encoded: the class and the type are embedded, never one-hot (amendment 10): the class through the camera's own
    class embedding (one table for the list and the map), the type -- the creature or game object entry, raw in its
    column -- hashed into TYPE_EMBED-wide buckets, (entry x 2 + is object) mod the manifest's type_buckets; the
    slot's other columns go in as they are, among them the line of sight (los) and the angular width and height the
    sensor measured (revision 2). The list is geometric, so a slot reads the same at every render size.
    The pool (Linear onto the camera's embedding width) is added to the camera's embedding; a layout without a list
    adds zero. The sight list (SightEntities) shares the encoder and points at the slots."""

    TYPE_EMBED = 8

    def __init__(self, descriptors: Sequence[dict | None], class_embedding: nn.Embedding, out_width: int,
                 embed: int = 64):
        super().__init__()
        entries = [entry for entry in descriptors if entry is not None]
        spec = entries[0]
        self.embed = embed
        self.slots, self.width_ = spec["slots"], spec["width"]
        self.class_column, self.type_column = spec["class_column"], spec["type_column"]
        self.object_column, self.buckets = spec["object_column"], spec["type_buckets"]
        for entry in entries:
            if any(entry[key] != spec[key] for key in ("slots", "width", "class_column", "type_column",
                                                       "object_column", "type_buckets")):
                raise ValueError(f"the entity lists of the layouts differ ({entry} against {spec}): one encoder "
                                 f"reads them all")
        # The camera's class embedding, held by reference (it is the camera's module, not a second copy).
        self.__dict__["class_embedding"] = class_embedding
        kept = [c for c in range(self.width_) if c not in (self.class_column, self.type_column)]
        inputs = len(kept) + class_embedding.embedding_dim + self.TYPE_EMBED
        self.encoders = nn.ModuleDict({"visible": nn.Sequential(_linear(inputs, embed, math.sqrt(2)), nn.Tanh(),
                                                                _linear(embed, embed, math.sqrt(2)), nn.Tanh())})
        self.pool = _linear(2 * embed, out_width, 1.0)
        self.type_embed = nn.Embedding(self.buckets, self.TYPE_EMBED)
        # Per layout: [layouts, slots * width] observation columns and [layouts, slots] present columns (-1: the layout
        # has no list). Derived from stage.json, not learned: kept out of the state dict.
        layouts = len(descriptors)
        columns = torch.zeros(layouts, self.slots * self.width_, dtype=torch.long)
        present = torch.full((layouts, self.slots), -1, dtype=torch.long)
        for index, entry in enumerate(descriptors):
            if entry is None:
                continue
            start = int(entry["first"])
            columns[index] = torch.arange(start, start + self.slots * self.width_)
            present[index] = start + torch.arange(self.slots) * self.width_ + int(entry["present"])
        self.register_buffer("columns_visible", columns, persistent=False)
        self.register_buffer("present_visible", present, persistent=False)
        self.register_buffer("has_sets", torch.tensor([entry is not None for entry in descriptors]), persistent=False)
        self.register_buffer("kept_columns", torch.tensor(kept, dtype=torch.long), persistent=False)
        # The memory id (entity memory, dungeon-curriculum I2) is a label, not a quantity: it reads 0 here, as it did
        # before memory existed, and the sight list embeds it instead (SightEntities).
        self.memory_column = int(spec.get("memory_column", -1))
        keep = torch.ones(self.width_)
        if self.memory_column >= 0:
            keep[self.memory_column] = 0.0
        self.register_buffer("column_keep", keep, persistent=False)

    def token(self, raw: torch.Tensor) -> torch.Tensor:
        """Slots' tokens [..., embed] from their entity-list columns [..., width] alone: the class and the
        type embedded, the memory id read as 0, the rest as they are. The sight list reads its slots so too."""
        dtype = self.pool.weight.dtype
        raw = raw * self.column_keep.to(raw.dtype)
        classes = raw[..., self.class_column].round().clamp(0, self.class_embedding.num_embeddings - 1).long()
        entry = raw[..., self.type_column].round().clamp(min=0).long()
        is_object = raw[..., self.object_column].round().long() if self.object_column >= 0 else torch.zeros_like(entry)
        bucket = (entry * 2 + is_object) % self.buckets
        x = torch.cat([raw.index_select(-1, self.kept_columns).to(dtype), self.class_embedding(classes).to(dtype),
                       self.type_embed(bucket).to(dtype)], dim=-1)
        return self.encoders["visible"](x)

    def encode(self, obs: torch.Tensor, layout: torch.Tensor) -> dict[str, tuple[torch.Tensor, torch.Tensor]]:
        """The slots' tokens [N, slots, embed] and which are present."""
        rows = obs.shape[0]
        layout = layout.reshape(-1).long()
        raw = obs.gather(1, self.columns_visible[layout]).reshape(rows, self.slots, self.width_)
        codes = self.token(raw)
        present_at = self.present_visible[layout]
        present = (present_at >= 0) & (obs.gather(1, present_at.clamp(min=0)) > 0.5)
        return {"visible": (codes, present)}

    def pooled(self, obs: torch.Tensor, layout: torch.Tensor,
               encoded: dict[str, tuple[torch.Tensor, torch.Tensor]]) -> torch.Tensor:
        """What the list adds to the camera's embedding: [rows, out], zero for a layout without it."""
        codes, present = encoded["visible"]
        weight = present.to(codes.dtype)[..., None]
        mean = (codes * weight).sum(dim=1) / weight.sum(dim=1).clamp(min=1.0)
        peak = torch.where(present[..., None], codes, torch.full_like(codes, -1.0)).amax(dim=1)
        peak = torch.where(present.any(dim=1, keepdim=True), peak, torch.zeros_like(peak))
        pooled = self.pool(torch.cat([mean, peak], dim=-1))
        return pooled * self.has_sets[layout.reshape(-1).long()][:, None].to(pooled.dtype)


class SightEntities(nn.Module):
    """**What the seat sees and remembers, as one list** (dungeon-curriculum I1 and I2; the sight block): its slots --
    the camera's visible entities first, in the entity list's order, then the most relevant remembered ones -- read by
    the entity list's own encoder (VisibleEntities.token: the same weights, shared by reference, not a copy), each
    token plus:
    - a projection of the memory's columns (visible now, age, dead, open, used, heading, course, selected, focused);
    - an embedding of its memory id (a label stable while the entity is remembered, ids past `memory_ids` folded onto
      the table; 0 none).
    The present slots are pooled (mean and max) onto the camera's embedding width, as the list's are: the pool is what
    seeding zeroes when the checkpoint has no sight list, so a seeded policy starts as it was.

    **The named row** (sight block revision 2, M3 interact), where the layouts have one: what the goal names -- a kind,
    never a place -- read by the same encoder (its class, type and object columns, the rest 0) plus a projection of the
    task, as one token. It looks over the slots (a match score of each slot's token against a query of the named
    token, softmaxed over the present slots), and the named token and the slots it matched are pooled onto the
    embedding (named_pool: zeroed by seeding as the pool is), so the trunk knows what it is after and where what it
    sees of it is; and each press's pointer scores gain the match times a learned weight (named_gain, from 0), so "the
    one named" is a slot the presses can point at from their first update.

    **The pointer heads** read the same tokens without the pixels (an action's logits are taken from the observation
    alone): one query per press, from the actor's features (SightPointers), scored against every slot, so a press
    follows the entity, not the slot -- one head over the combined list of visible and remembered."""

    def __init__(self, descriptors: Sequence[dict | None], visible: "VisibleEntities", out_width: int,
                 embed: int = 64):
        super().__init__()
        spec = next(entry for entry in descriptors if entry is not None)
        for entry in descriptors:
            if entry is not None and any(entry[key] != spec[key] for key in (
                    "slots", "visible_slots", "width", "presses", "memory_ids", "visible_column")):
                raise ValueError(f"the sight lists of the layouts differ ({entry} against {spec}): one encoder reads "
                                 f"them all")
        if spec["visible_slots"] != visible.slots or spec["width"] <= visible.width_:
            raise ValueError(f"the sight list's visible half ({spec['visible_slots']} slots) is not the entity list's "
                             f"({visible.slots})")
        self.slots, self.width_, self.visible_slots = spec["slots"], spec["width"], spec["visible_slots"]
        self.base = visible.width_
        self.memory_column, self.memory_ids = spec["memory_column"], spec["memory_ids"]
        self.presses = tuple(spec["presses"])
        self.named_width = int(spec.get("named_width", 0))
        for entry in descriptors:
            if entry is not None and int(entry.get("named_width", 0)) != self.named_width:
                raise ValueError("the sight lists' named rows differ: one encoder reads them all")
        if self.named_width:
            if int(spec["named_entity_width"]) != self.base:
                raise ValueError(f"the sight list's named row leads with {spec['named_entity_width']} columns, the "
                                 f"entity list's tokens read {self.base}")
            self.named_task = _linear(self.named_width - self.base, embed, 1.0)
            self.named_query = _linear(embed, embed, 1.0)
            self.named_pool = _linear(2 * embed, out_width, 1.0)
            self.named_gain = nn.Parameter(torch.zeros(max(1, len(self.presses))))
        # The entity list's encoder, held by reference (it is the list's module).
        self.__dict__["shared"] = visible
        self.extra = _linear(self.width_ - self.base, embed, 1.0)
        self.memory_embed = nn.Embedding(self.memory_ids + 1, embed, padding_idx=0)
        with torch.no_grad():
            self.memory_embed.weight.normal_(0.0, 0.1)
            self.memory_embed.weight[0].zero_()
        self.pool = _linear(2 * embed, out_width, 1.0)
        layouts = len(descriptors)
        columns = torch.zeros(layouts, self.slots * self.width_, dtype=torch.long)
        present = torch.full((layouts, self.slots), -1, dtype=torch.long)
        first = torch.full((layouts, max(1, len(self.presses))), -1, dtype=torch.long)
        self.blind = {}
        for index, entry in enumerate(descriptors):
            if entry is None:
                continue
            start = entry["first"]
            columns[index] = torch.arange(start, start + self.slots * self.width_)
            present[index] = start + torch.arange(self.slots) * self.width_ + entry["present"]
            for at, action in enumerate(entry["pointer_first"]):
                first[index, at] = action
            self.blind[index] = list(range(start, start + self.slots * self.width_))
        # Derived from stage.json, not learned: kept out of the state dict.
        self.register_buffer("columns", columns, persistent=False)
        self.register_buffer("present_at", present, persistent=False)
        self.register_buffer("pointer_first", first, persistent=False)
        self.register_buffer("has_sight", present[:, 0] >= 0, persistent=False)
        named = torch.zeros(layouts, max(1, self.named_width), dtype=torch.long)
        has_named = torch.zeros(layouts, dtype=torch.bool)
        for index, entry in enumerate(descriptors):
            if entry is None or not self.named_width:
                continue
            start = entry["first"] + entry["named_offset"]
            named[index] = torch.arange(start, start + self.named_width)
            has_named[index] = True
            self.blind[index] += list(range(start, start + self.named_width))
        self.register_buffer("named_columns", named, persistent=False)
        self.register_buffer("has_named", has_named, persistent=False)

    def tokens(self, obs: torch.Tensor, layout: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """The slots' tokens [N, slots, embed] and which are present [N, slots], from the observation alone (the
        pointers read the list so too)."""
        rows = obs.shape[0]
        layout = layout.reshape(-1).long()
        raw = obs.gather(1, self.columns[layout]).reshape(rows, self.slots, self.width_)
        dtype = self.pool.weight.dtype
        codes = self.shared.token(raw[..., : self.base])
        codes = codes + self.extra(raw[..., self.base:].to(dtype))
        ids = raw[..., self.memory_column].round().clamp(min=0).long()
        ids = torch.where(ids > 0, (ids - 1) % self.memory_ids + 1, ids)
        codes = codes + self.memory_embed(ids).to(dtype)
        present_at = self.present_at[layout]
        present = (present_at >= 0) & (obs.gather(1, present_at.clamp(min=0)) > 0.5)
        return codes, present

    def named(self, obs: torch.Tensor, layout: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """The named row's token [N, embed] and whether a row names anything [N] (its present column; a layout
        without the row names nothing)."""
        layout = layout.reshape(-1).long()
        raw = obs.gather(1, self.named_columns[layout])
        dtype = self.pool.weight.dtype
        token = self.shared.token(raw[:, : self.base]) + self.named_task(raw[:, self.base:].to(dtype))
        present = self.has_named[layout] & (raw[:, 0] > 0.5)
        return token, present

    def match(self, codes: torch.Tensor, token: torch.Tensor) -> torch.Tensor:
        """Each slot's match with the named token [N, slots]: its token against the named token's query, scaled."""
        query = self.named_query(token).to(codes.dtype)
        return torch.einsum("rse,re->rs", codes, query) / math.sqrt(codes.shape[-1])

    def pooled(self, codes: torch.Tensor, present: torch.Tensor, layout: torch.Tensor,
               obs: torch.Tensor | None = None) -> torch.Tensor:
        """What the list adds to the camera's embedding: [N, out], zero for a layout without it; with the named row
        (and `obs`), the named token and the slots that match it too."""
        weight = present.to(codes.dtype)[..., None]
        mean = (codes * weight).sum(dim=1) / weight.sum(dim=1).clamp(min=1.0)
        peak = torch.where(present[..., None], codes, torch.full_like(codes, -1.0)).amax(dim=1)
        peak = torch.where(present.any(dim=1, keepdim=True), peak, torch.zeros_like(peak))
        out = self.pool(torch.cat([mean, peak], dim=-1))
        if self.named_width and obs is not None:
            token, named = self.named(obs, layout)
            scores = self.match(codes, token.to(codes.dtype)).masked_fill(~present, -1e4)
            weights = torch.softmax(scores.float(), dim=-1).to(codes.dtype) * present.to(codes.dtype)
            attended = (weights[..., None] * codes).sum(dim=1)
            extra = self.named_pool(torch.cat([token.to(codes.dtype), attended], dim=-1))
            out = out + extra * named[:, None].to(extra.dtype)
        return out * self.has_sight[layout.reshape(-1).long()][:, None].to(out.dtype)

    def with_pointers(self, logits: torch.Tensor, features: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
                      queries: "SightPointers") -> torch.Tensor:
        """`logits` with every press's slots scored by their tokens against the press's query, in the rows whose
        layout has the list; the other rows' as they were (nothing is read back)."""
        layout = layout.reshape(-1).long()
        codes, _ = self.tokens(obs, layout)
        steps = torch.arange(self.slots, device=logits.device)[None, :]
        matched = None
        if self.named_width:
            token, named = self.named(obs, layout)
            matched = self.match(codes, token.to(codes.dtype)) * named[:, None].to(codes.dtype)
        for at, press in enumerate(self.presses):
            first = self.pointer_first[layout, at]
            scores = torch.einsum("rse,re->rs", codes, queries.queries[press](features).to(codes.dtype))
            if matched is not None:
                scores = scores + self.named_gain[at].to(codes.dtype) * matched
            valid = first >= 0
            columns = (first.clamp(min=0)[:, None] + steps).clamp(max=logits.shape[-1] - 1)
            kept = logits.gather(1, columns)
            # Out of place: the update differentiates through `logits`, which the gather above has just read.
            logits = logits.scatter(1, columns, torch.where(valid[:, None], scores.to(logits.dtype), kept))
        return logits


class SightPointers(nn.Module):
    """The sight list's pointer queries (SightEntities.with_pointers): one per press, a Linear from the actor's
    features to the token width, initialised small as the other pointer queries are. The actor's own (the critic
    acts on nothing)."""

    def __init__(self, presses: Sequence[str], head_width: int, embed: int = 64):
        super().__init__()
        self.queries = nn.ModuleDict({press: _linear(head_width, embed, 0.01) for press in presses})


class VisionEncoder(nn.Module):
    """**The camera** (camera-vision): a seat's H x W image of the static world its camera's rays hit (terrain,
    models, closed doors, liquids, ground hazards: units and objects are the entity list's), encoded by one small
    network shared by every layout, as VisibleEntities shares its encoder.

    The image arrives as bytes beside the observation (protocol 26: [N, H x W x 4] uint8, [row][col][byte]) and is
    decoded on the device to its five channels, channels last (decode_image); the block's scalars are gathered from
    each row's own layout's columns (a table by layout id: the vision block starts at a different column in every
    class). **The class is a one-hot** over the manifest's pixel_classes (7: sky, terrain, model, door, water, deadly,
    ground hazard), a float data plane compared against the class channel, so 4 + 7 planes a pixel (11) and no
    learned embedding on the pixel path (an embedding's scatter backward was a third of the CPU update). The class
    table `class_embed` stays for the entity tokens and the map. **Patches**, not
    convolutions: the image is cut into PATCH x PATCH squares (128 x 64 with patch 8 -> a 16 x 8 grid), each square's
    pixels and planes (8 x 8 x 11 = 704 features, ordered row in the patch, column in the patch, plane) go through
    one shared Linear -> 64 + SiLU and Linear 64 -> 64 + SiLU, a spatial softmax per channel over the patch grid gives
    its expected (x, y) in [-1, 1] (128 keypoints), the block's scalars join them, and Linear -> 256: the camera's
    embedding, after SiLU. PATCH is the manifest's "patch" (4 when it names none). Each network reads it through a
    `vision_join` of its own, a Linear onto its adapters' output width (VisionJoin).

    **The entity list** (perception-goals 1b; VisibleEntities): where the layouts have one, its slots are read as a
    set and the set's pool is added to the embedding before the SiLU. The image carries no entities, so a token does
    not look at pixels: what it knows of where and how big it is, is its columns (direction, distance, angular size,
    line of sight).

    **One encoder for the actor and the critic** (MappoTrainer): the actor owns it -- it is in the actor's state dict,
    so a policy alone (export, distillation, the league) carries its camera -- and the critic holds a reference that
    is not one of its modules, so neither its parameters, its optimiser nor its state dict has a second copy. Both
    losses' gradients reach it, summed, and its own optimiser steps it once per minibatch (MappoTrainer.vision_opt).

    No normalisation: the channels are 0-1 or -1-1 already, and the list's columns are scaled by the sim. Every
    shape is fixed and nothing is read back, so a rollout graph captures it."""

    PATCH = DEFAULT_PATCH       # the class's default; an encoder's own is its manifest's "patch" (revision 4: 8)
    WIDTHS = (64, 64)
    EMBED = 256
    CLASS_EMBED = 6

    @classmethod
    def update_bytes_per_row(cls, image: dict) -> int:
        """Roughly the device memory one row's camera (and map) keeps for the update's backward pass, in bytes, from the
        stage's vision descriptor: float32 tensors the encoder saves per pixel (the decoded channels, the planes
        without the class, the concatenated planes with the class one-hot and the patch copy of them) and per map
        cell (the code and class embeddings, the values, the planes and their patch copy, and the int64 decode).
        The pixel class is a one-hot (no embedding output kept), so a row of the 128 x 64 camera is 1.0 MB against the
        embedded design's 1.15 MB (0.98 MB measured then, 5.6 GiB over 6,144 rows, move1_controls.yaml); not
        re-measured. The map adds 0.45 MB."""
        channels = int(image["channels"])
        planes = channels - 1 + len(image["pixel_classes"])
        pixels = int(image["height"]) * int(image["width"])
        total = 4 * pixels * (channels + (channels - 1) + 2 * planes)
        crop = image.get("map")
        if crop is not None:
            cells = int(crop["height"]) * int(crop["width"])
            per_cell = MapEncoder.CODE_EMBED + cls.CLASS_EMBED + 5
            total += cells * (4 * 3 * per_cell + 16)
        return total

    def __init__(self, descriptors: Sequence[dict | None]):
        super().__init__()
        image = next(entry for entry in descriptors if entry is not None)
        self.height, self.width = image["height"], image["width"]
        self.channels, self.classes = image["channels"], image["classes"]
        self.class_channel, self.scalars = image["class_channel"], image["scalars"]
        self.class_limit = int(image.get("class_limit", CLASS_LIMIT))
        self.pixel_classes = tuple(image["pixel_classes"])
        self.PATCH = int(image.get("patch", DEFAULT_PATCH))
        if self.height % self.PATCH or self.width % self.PATCH:
            raise ValueError(f"the camera's image is {self.width} x {self.height}; the vision encoder cuts it into "
                             f"{self.PATCH} x {self.PATCH} patches, so both must be multiples of {self.PATCH} "
                             f"(AnimusForge.Vision.Width, Height)")
        self.image_bytes = image["image_bytes"]
        self.span = self.scalars
        self.class_embed = nn.Embedding(self.class_limit, self.CLASS_EMBED)
        self.planes_per_pixel = self.channels - 1 + len(self.pixel_classes)
        self.grid = (self.height // self.PATCH, self.width // self.PATCH)
        features = self.PATCH * self.PATCH * self.planes_per_pixel
        self.patch = nn.Linear(features, self.WIDTHS[0])
        self.mix = nn.Linear(self.WIDTHS[0], self.WIDTHS[1])
        self.embed = nn.Linear(2 * self.WIDTHS[1] + self.scalars, self.EMBED)
        self.feature_shape = (self.WIDTHS[1], *self.grid)
        # Derived from stage.json, not learned: kept out of the state dict.
        self.register_buffer("start", torch.tensor([entry["first"] if entry is not None else -1
                                                    for entry in descriptors], dtype=torch.long), persistent=False)
        self.register_buffer("has_vision", self.start >= 0, persistent=False)
        self.register_buffer("offsets", torch.arange(self.span, dtype=torch.long), persistent=False)
        grid_y, grid_x = torch.meshgrid(torch.linspace(-1.0, 1.0, self.grid[0]),
                                        torch.linspace(-1.0, 1.0, self.grid[1]), indexing="ij")
        self.register_buffer("grid_x", grid_x.reshape(-1), persistent=False)
        self.register_buffer("grid_y", grid_y.reshape(-1), persistent=False)
        # The classes a pixel can carry, in plane order: the one-hot compares the class channel against them. Derived
        # from the manifest, not learned.
        self.register_buffer("pixel_class_ids", torch.tensor(self.pixel_classes, dtype=torch.long), persistent=False)
        # The entity list, where the layouts have one (every revision 5 stage does).
        lists = [entry.get("entities") if entry is not None else None for entry in descriptors]
        self.entities = None
        self.slots = 0
        if any(entry is not None for entry in lists):
            self.entities = VisibleEntities(lists, self.class_embed, self.EMBED)
            self.slots = self.entities.slots
        # The mental map, where the layouts have one (perception-goals REDESIGN §3): its crop follows the image in
        # each row's bytes, and its encoder adds to the embedding.
        crops = [entry.get("map") if entry is not None else None for entry in descriptors]
        self.map = None
        self.map_bytes = 0
        if any(crop is not None for crop in crops):
            crop = next(crop for crop in crops if crop is not None)
            self.map = MapEncoder(crop, self.class_embed, self.EMBED,
                                  [c["first"] if c is not None else -1 for c in crops])
            self.map_bytes = crop["map_bytes"]
        # The seen and remembered list (dungeon-curriculum I1, I2), where the layouts have a sight block: read with
        # the entity list's encoder, pooled onto the embedding, and pointed at by the actor's presses.
        sights = [entry.get("sight") if entry is not None else None for entry in descriptors]
        self.sight = None
        if any(sight is not None for sight in sights):
            if self.entities is None:
                raise ValueError("a sight block needs the camera's entity list, whose encoder it shares")
            self.sight = SightEntities(sights, self.entities, self.EMBED)
        #: Per layout with the camera, the columns its adapter and normaliser do not read: the block's scalars, which
        #: the encoder reads raw, its entity list's, which the list's encoder reads, its map's scalars, and its sight
        #: list's.
        self.blind = {}
        for index, entry in enumerate(descriptors):
            if entry is None:
                continue
            columns = list(range(entry["first"], entry["first"] + self.span))
            if lists[index] is not None:
                columns += list(range(lists[index]["first"],
                                      lists[index]["first"] + lists[index]["slots"] * lists[index]["width"]))
            if crops[index] is not None:
                columns += list(range(crops[index]["first"], crops[index]["first"] + crops[index]["scalars"]))
            if self.sight is not None:
                columns += self.sight.blind.get(index, [])
            self.blind[index] = columns

    def gather(self, obs: torch.Tensor, layout: torch.Tensor,
               image: torch.Tensor | None) -> tuple[torch.Tensor, torch.Tensor]:
        """Each row's camera: (its image decoded, [N, H, W, C], and its own layout's scalar columns, [N, S], raw).
        Rows of a layout without the camera read whatever is at column 0 on (their output is zeroed by the join)."""
        if image is None:
            raise ValueError("this network has a camera: its image bytes have to come with the observation")
        layout = layout.reshape(-1).long()
        columns = (self.start[layout].clamp(min=0)[:, None] + self.offsets[None, :]).clamp(max=obs.shape[-1] - 1)
        scalars = obs.gather(1, columns).to(self.embed.weight.dtype)
        decoded = decode_image(self.pixels(image), self.height, self.width)
        return decoded.to(self.embed.weight.dtype), scalars

    def pixels(self, image: torch.Tensor) -> torch.Tensor:
        """The image's bytes of each row [N, I]: the row's first image_bytes (its map crop follows, protocol 24)."""
        rows = image.reshape(-1, self.image_bytes + self.map_bytes)
        return rows[:, :self.image_bytes] if self.map_bytes else rows

    def planes(self, image: torch.Tensor) -> torch.Tensor:
        """[N, H, W, C] -> [N, H, W, C - 1 + P]: every channel but the class as it is, then the class as a one-hot over
        the P pixel classes (the class rounded; a class past the list is all zeros). Data planes: nothing here needs a
        gradient, so there is no embedding to scatter into."""
        classes = image[..., self.class_channel].round().long()
        rest = torch.cat([image[..., : self.class_channel], image[..., self.class_channel + 1:]], dim=-1)
        onehot = classes[..., None] == self.pixel_class_ids
        return torch.cat([rest, onehot.to(rest.dtype)], dim=-1)

    def patches(self, planes: torch.Tensor) -> torch.Tensor:
        """[N, H, W, P] -> [N, patches, PATCH x PATCH x P]: the grid's patches row by row, each its pixels row by
        row and every pixel's planes."""
        rows, cols = self.grid
        n = planes.shape[0]
        cut = planes.reshape(n, rows, self.PATCH, cols, self.PATCH, self.planes_per_pixel).permute(0, 1, 3, 2, 4, 5)
        return cut.reshape(n, rows * cols, -1)

    def features(self, patches: torch.Tensor) -> torch.Tensor:
        """Each patch's features, [N, patches, 64]."""
        silu = nn.functional.silu
        return silu(self.mix(silu(self.patch(patches))))

    def keypoints(self, features: torch.Tensor) -> torch.Tensor:
        """Spatial softmax: per channel the expected (x, y) of its softmax over the patch grid, [N, 2 x channels]."""
        weights = torch.softmax(features.transpose(1, 2), dim=-1)
        x = (weights * self.grid_x.to(weights.dtype)).sum(-1)
        y = (weights * self.grid_y.to(weights.dtype)).sum(-1)
        return torch.cat([x, y], dim=-1)

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, image: torch.Tensor | None) -> torch.Tensor:
        """The camera's embedding of rows `obs` [N, O] with their images `image` [N, I] uint8, [N, 256] (a layout
        without the camera's rows are zeroed by the join)."""
        decoded, scalars = self.gather(obs, layout, image)
        features = self.features(self.patches(self.planes(decoded)))
        out = self.embed(torch.cat([self.keypoints(features), scalars], dim=-1))
        if self.map is not None:
            crops = image.reshape(-1, self.image_bytes + self.map_bytes)[:, self.image_bytes:]
            out = out + self.map(obs, layout, crops).to(out.dtype)
        if self.entities is not None:
            encoded = self.entities.encode(obs, layout)
            out = out + self.entities.pooled(obs, layout, encoded).to(out.dtype)
            if self.sight is not None:
                codes, present = self.sight.tokens(obs, layout)
                out = out + self.sight.pooled(codes, present, layout, obs).to(out.dtype)
        return nn.functional.silu(out)


class VisionJoin(nn.Module):
    """A network's own reading of the shared camera embedding (VisionEncoder): a Linear onto its adapters' output
    width, initialised as the adapters are, so a network trained from scratch sees with the camera from its first
    update (M1, the user, 2026-10-06). Seeding from a checkpoint without the camera zeroes it instead
    (bootstrap._seed_vision), so the seeded policy starts as it was. A layout without the camera adds zero."""

    def __init__(self, layouts_with: torch.Tensor, width: int):
        super().__init__()
        self.linear = _linear(VisionEncoder.EMBED, width, math.sqrt(2))
        self.register_buffer("has_vision", layouts_with.clone(), persistent=False)

    def forward(self, embedding: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """What the camera adds to the adapter's output: [N, width], zero for a layout without it."""
        joined = self.linear(embedding)
        return joined * self.has_vision[layout.reshape(-1).long()][:, None].to(joined.dtype)


#: The look head's initial lean toward the choice that changes nothing (camera-vision.FREELOOK.md, amendments): that
#: choice's logit starts this much above the others', so early turning of the camera is rarer -- at 2.0 a fresh head
#: holds the yaw rate at 0 about 55% of the time, against 14% for an even 7-way draw. Every choice stays possible.
LOOK_HOLD_BIAS = 2.0


def look_hold_indices(heads: Sequence[int]) -> tuple[int, ...]:
    """Per look head, its choice that changes nothing, by position: every head but the last is a rate, whose middle
    choice is rate 0; the last is the camera's command head (zoom, recentre, face), whose choice 0 is "hold". Not by
    size: the command head's 5 choices (with "face") are odd too. Revision 4's (7, 5, 5) gives (3, 2, 0),
    protocol.LOOK_HOLD."""
    return tuple(head // 2 for head in heads[:-1]) + ((0,) if heads else ())


class LookHead(nn.Module):
    """**The free look** (camera-vision.FREELOOK.md D): a second policy head over the features the movement head reads,
    Linear(features -> sum(heads)) split into one categorical per head -- yaw rate, pitch rate, zoom -- chosen every
    decision, independently of the movement action and of each other. Initialised as the other policy outputs are
    (small gain), its bias leaning toward each head's hold choice (LOOK_HOLD_BIAS).

    Only rows whose layout has the camera look: for the others the choice is 0 (what the wire carries, and the sim
    ignores), and its log probability and entropy are 0 with no gradient. Nothing is masked: every choice is allowed.
    Fixed shapes and no reads back, so a rollout graph captures it."""

    def __init__(self, width: int, heads: Sequence[int], layouts_with: torch.Tensor):
        super().__init__()
        self.heads = tuple(int(head) for head in heads)
        self.linear = _linear(width, sum(self.heads), 0.01)
        with torch.no_grad():
            offset = 0
            for head, hold in zip(self.heads, look_hold_indices(self.heads)):
                self.linear.bias[offset + hold] = LOOK_HOLD_BIAS
                offset += head
        self.register_buffer("has_vision", layouts_with.clone(), persistent=False)

    def logits(self, features: torch.Tensor) -> list[torch.Tensor]:
        """Each head's logits for flat rows, [[N, head], ...]."""
        return list(torch.split(self.linear(features), self.heads, dim=-1))

    def _seeing(self, layout: torch.Tensor) -> torch.Tensor:
        return self.has_vision[layout.reshape(-1).long()]

    def sample(self, features: torch.Tensor, layout: torch.Tensor,
               deterministic: bool) -> tuple[torch.Tensor, torch.Tensor]:
        """(choice [N, heads] long, its log probability [N], the heads' summed) for flat rows; 0 and 0 for a row
        without the camera. Deterministic is each head's argmax."""
        seeing = self._seeing(layout)
        choices, log_prob = [], None
        for logits in self.logits(features):
            choice, chosen = sample_logits(logits.float(), deterministic)
            choices.append(choice)
            log_prob = chosen if log_prob is None else log_prob + chosen
        choice = torch.where(seeing[:, None], torch.stack(choices, dim=-1), torch.zeros_like(choices[0])[:, None])
        return choice, torch.where(seeing, log_prob, torch.zeros_like(log_prob))

    def evaluate(self, features: torch.Tensor, layout: torch.Tensor,
                 choice: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """(log probability [N], entropy [N]) of the choices `choice` [N, heads] taken, each the heads' sum; 0 with no
        gradient for a row without the camera."""
        seeing = self._seeing(layout)
        choice = choice.reshape(-1, len(self.heads)).long()
        log_prob = entropy = None
        for index, logits in enumerate(self.logits(features)):
            logits = logits.float()
            part = log_prob_of(logits, choice[:, index].clamp(0, self.heads[index] - 1))
            spread = _entropy(logits)
            log_prob = part if log_prob is None else log_prob + part
            entropy = spread if entropy is None else entropy + spread
        zero = torch.zeros_like(log_prob)
        return torch.where(seeing, log_prob, zero), torch.where(seeing, entropy, zero)


def _attach_vision(network: nn.Module, vision, encoder: "VisionEncoder | None" = None) -> None:
    """Give an actor or critic the camera: the encoder (VisionEncoder) and its own join (VisionJoin), and keep the
    camera's columns -- image and scalars -- out of every layout's adapter (attach_blind_columns, tag "vision") and
    normaliser (RunningNorm.hide). `vision` is vision_of()'s per-layout image, or None (no camera: nothing changes).
    With `encoder` (another network's: the trainer's critic gets the actor's), the network reads that one through a
    reference that is not one of its modules -- not in its parameters, its state dict or its .to() -- else it builds
    and owns its own."""
    network.vision = None
    network.vision_join = None
    if vision is None:
        return
    if encoder is None:
        network.vision = VisionEncoder(vision)
    else:
        share_vision(network, encoder)
    network.vision_join = VisionJoin(network.vision.has_vision, network.adapters[0].out_features)
    attach_blind_columns(network, network.vision.blind, tag="vision")
    for index, columns in network.vision.blind.items():
        network.norms[index].hide(columns)


def share_vision(network: nn.Module, encoder: "VisionEncoder") -> None:
    """Point `network` at another network's camera encoder without making it one of its modules (no second copy in
    its parameters or state dict)."""
    network._modules.pop("vision", None)
    network.__dict__["vision"] = encoder


def vision_term(network: nn.Module, obs: torch.Tensor, layout: torch.Tensor,
                embedding: torch.Tensor | None = None, image: torch.Tensor | None = None) -> torch.Tensor:
    """What the camera adds to `network`'s adapter output: its join over the encoder's embedding of the rows' images
    `image` [N, I] uint8 (computed here unless the caller has it: the rollout graph and the update encode once for
    both networks)."""
    if embedding is None:
        embedding = network.vision(obs, layout, image)
    return network.vision_join(embedding, layout)


def _vision_extra(network: nn.Module, obs: torch.Tensor, layout: torch.Tensor,
                  embedding: torch.Tensor | None = None, image: torch.Tensor | None = None):
    """What the camera adds to the adapter's output, or None for a network without one."""
    if getattr(network, "vision", None) is None:
        return None
    return vision_term(network, obs, layout, embedding, image)


class GoalEmbedding(nn.Module):
    """A goal as the features it adds: its kind's embedding plus its target's (zero at the start, so a new goal head
    changes nothing until it is trained).

    With `paired` the goal a seat holds is its pair, primary * (count + 1) + secondary + 1 (goal_pair): the same one
    number in every buffer and call, and the secondary adds its own embedding through a learned gate.

    The actor also scales the features by the goal (condition(): features x (1 + scale) + shift, FiLM). Added alone,
    the goal only moved each action's logit by a constant through the linear heads -- it could not change what the
    seat does in a situation, and swapping it changed the argmax action 13-21% of the time (next-run trials,
    2026-09-30). The scale starts at zero, so a new model acts as the shift alone did until it learns one."""

    def __init__(self, kinds: int, targets: int, width: int, paired: bool = False, scaled: bool = False):
        super().__init__()
        self.targets = max(1, targets)
        self.count = kinds * self.targets
        self.paired = paired
        self.kind = nn.Embedding(kinds, width)
        self.target = nn.Embedding(self.targets, width) if self.targets > 1 else None
        # The actor's scale (condition()); the critic reads the shift alone and has none.
        self.kind_scale = nn.Embedding(kinds, width) if scaled else None
        self.target_scale = nn.Embedding(self.targets, width) if scaled and self.targets > 1 else None
        nn.init.zeros_(self.kind.weight)
        if self.target is not None:
            nn.init.zeros_(self.target.weight)
        for table in (self.kind_scale, self.target_scale):
            if table is not None:
                nn.init.zeros_(table.weight)
        if paired:
            self.gate = nn.Parameter(torch.tensor(0.5))

    def _one(self, goal: torch.Tensor, scale: bool = False) -> torch.Tensor:
        kind, target = (self.kind_scale, self.target_scale) if scale else (self.kind, self.target)
        embedded = kind(torch.div(goal, self.targets, rounding_mode="floor"))
        if target is not None:
            embedded = embedded + target(goal % self.targets)
        return embedded

    def _held(self, goal: torch.Tensor, scale: bool) -> torch.Tensor:
        goal = goal.reshape(-1).long()
        if not self.paired:
            return self._one(goal, scale)
        primary, secondary = split_goal_pair(goal, self.count)
        extra = self._one(secondary.clamp(min=0), scale) * (secondary >= 0)[:, None].to(self.kind.weight.dtype)
        return self._one(primary, scale) + self.gate.to(self.kind.weight.dtype) * extra

    def forward(self, goal: torch.Tensor) -> torch.Tensor:
        """The shift alone (the critic's reading of the goal)."""
        return self._held(goal, False)

    def condition(self, features: torch.Tensor, goal: torch.Tensor) -> torch.Tensor:
        """The actor's features under `goal`: features x (1 + scale) + shift."""
        if self.kind_scale is None:
            return features + self._held(goal, False).to(features.dtype)
        scale = self._held(goal, True).to(features.dtype)
        return features * (1.0 + scale) + self._held(goal, False).to(features.dtype)


# The goal's scale on the actor's features (GoalEmbedding.condition, .amdl 7) starts at zero; an actor saved before it
# had none, and loads as exactly the actor it was.
_GOAL_SCALE_KEYS = ("goal_embedding.kind_scale.weight", "goal_embedding.target_scale.weight")
#: The goal head's pointer over place slots (GoalHead.pointer, m2-goals W3a) and where its features sit
#: (GoalHead.place_spec): an actor saved before them loads with the pointer as initialised -- its last layer at zero, so
#: it adds nothing -- and no place slots until the stage's manifest says (MappoTrainer.set_goal_space).
#: The same for the cell head (GoalHead.cell, free-choice-goals): an actor or critic saved before it loads with
#: goal_head.cell.* as initialised (the score at zero: a uniform draw over the choosable blocks) and no cells until the
#: manifest says (cell_spec).
_GOAL_POINTER_PREFIXES = ("goal_head.pointer.", "goal_head.place_spec", "goal_head.cell.", "goal_head.cell_spec")


#: The blind-column masks' buffer names (attach_blind_columns' tags): "blind_keep_" (a checkpoint from before the hint
#: block went still carries it, and it is dropped on load), the camera's.
BLIND_KEEP_PREFIXES = ("blind_keep_", "vision_keep_")


def without_blind_columns(state: dict) -> dict:
    """A saved network's state without its blind-column masks (attach_blind_columns): they are made again from the
    stage's layouts after loading (the camera), and a network not yet given them refused them."""
    return {key: value for key, value in state.items()
            if not key.split(".")[-1].startswith(BLIND_KEEP_PREFIXES)}


def load_actor_state(actor: nn.Module, state: dict) -> None:
    """actor.load_state_dict(state), except that an actor saved before the goal scale existed loads with it at zero, and
    one saved before the goal head's pointer existed loads with the pointer as initialised (a no-op)."""
    missing, unexpected = actor.load_state_dict(without_blind_columns(state), strict=False)
    # The blind-column masks are never taken from a checkpoint: an actor built with seat sets has its own already (the
    # stage's), which a resume keeps.
    wrong = [key for key in missing
             if key not in _GOAL_SCALE_KEYS and not key.startswith(_GOAL_POINTER_PREFIXES)
             and not key.split(".")[-1].startswith(BLIND_KEEP_PREFIXES)]
    if wrong or unexpected:
        raise RuntimeError(f"Error(s) in loading state_dict for {type(actor).__name__}: missing {wrong}, "
                           f"unexpected {list(unexpected)}")
    parameters = dict(actor.named_parameters())
    with torch.no_grad():
        for key in missing:
            if key in _GOAL_SCALE_KEYS:
                parameters[key].zero_()


def goal_pair(primary: torch.Tensor, secondary: torch.Tensor, count: int) -> torch.Tensor:
    """The pair of goals a seat holds as one number: primary * (count + 1) + secondary + 1 (secondary -1: none)."""
    return primary.long() * (count + 1) + secondary.long() + 1


def split_goal_pair(pair: torch.Tensor, count: int) -> tuple[torch.Tensor, torch.Tensor]:
    """(primary, secondary) of goal_pair's number."""
    pair = pair.long()
    return torch.div(pair, count + 1, rounding_mode="floor"), pair % (count + 1) - 1


class LayoutActor(nn.Module):
    def __init__(self, layouts: Sequence[tuple[int, int]], hidden: Sequence[int], foresight_outputs: int = 0,
                 recurrent_size: int = 0, goal_count: int = 0, goal_targets: int = 1, slow_size: int = 0,
                 foresight_feedback: bool = False, lookahead: bool = False, goal_slots: int = 1,
                 vision=None):
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
        #
        # `goal_count` is the goal *kinds* and `goal_targets` the target space (GoalHead); self.goal_count is the
        # joint count, which is what the buffers, the wire and the stats count.
        #
        # **The two-clock seat** (Component D, `slow_size`): the goal is chosen by a slow loop of its own -- a GRU that
        # steps only when a goal is chosen, over the fast loop's features at that moment, so what it remembers spans
        # minutes of goals rather than seconds of decisions -- with its own value head, credited on its own clock
        # (MappoTrainer's slow goal update). Without it the goal head reads the fast features as before.
        self.slow_size = slow_size if goal_count else 0
        self.slow_memory = nn.GRUCell(head_width, self.slow_size) if self.slow_size else None
        self.slow_value = _linear(self.slow_size, 1, 1.0) if self.slow_size else None
        goal_width = self.slow_size or head_width
        self.goal_head = (GoalHead(goal_width, goal_count, goal_targets, len(layouts), lookahead, goal_slots)
                          if goal_count else None)
        self.goal_count = self.goal_head.count if self.goal_head is not None else 0
        self.goal_slots = self.goal_head.slots if self.goal_head is not None else 1
        self.goal_embedding = (GoalEmbedding(goal_count, goal_targets, head_width, self.goal_slots > 1, scaled=True)
                               if goal_count else None)
        # **Predictions fed back** (Component P, layer 2): the foresight head's outputs, detached, projected onto the
        # features the action head and the slow loop read, so the policy acts on what it expects to happen.
        self.foresight_feedback = bool(foresight_feedback and foresight_outputs)
        self.foresight_proj = _linear(foresight_outputs, head_width, 0.01) if self.foresight_feedback else None
        # The camera (VisionEncoder; vision_of(stage.json), None = no layout has one).
        _attach_vision(self, vision)
        # The sight list's pointer heads (SightPointers), where the layouts have a sight block: the actor's own.
        sight = self.vision.sight if self.vision is not None else None
        self.sight_pointers = SightPointers(sight.presses, head_width) if sight is not None else None
        # Its free look (LookHead), where the vision block has look heads (revision 4): the actor's own, not shared.
        look_heads = vision_look_heads(vision)
        self.look_head = LookHead(head_width, look_heads, self.vision.has_vision) if look_heads else None

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, groups=None,
                memory: torch.Tensor | None = None, image: torch.Tensor | None = None) -> Categorical:
        """obs [..., O], layout [...], mask [..., N] (padded) -> distribution over N actions. `groups` is
        per_layout(layout) when the caller already has it; `memory` [..., recurrent_size] the state carried in;
        `image` [..., I] uint8 the camera's bytes (protocol 21), with a camera."""
        return self._forward(obs, layout, mask, groups, memory, image=image)[0]

    def step(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, memory: torch.Tensor | None = None,
             groups=None, goal: torch.Tensor | None = None,
             image: torch.Tensor | None = None) -> tuple[Categorical, torch.Tensor, torch.Tensor]:
        """One decision: (distribution, memory carried out, foresight predictions). Without a GRU the memory out is
        whatever came in (an empty tensor), and without foresight heads the predictions are empty."""
        dist, features, lead = self._forward(obs, layout, mask, groups, memory, goal, image)
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

    def encode(self, obs: torch.Tensor, layout: torch.Tensor, groups=None,
               vision_embedding: torch.Tensor | None = None, image: torch.Tensor | None = None) -> torch.Tensor:
        """Adapters and trunk for flat rows: everything that depends only on this decision's observation, before the
        GRU. A replayed sequence encodes every step in one pass and then carries the memory through them (carry),
        which is the difference between one large matmul per layer and one per step. `vision_embedding` is the
        camera's embedding of these rows when the caller has it (the update encodes once for the actor and the
        critic); else it is computed here, from the rows' camera bytes `image` [N, I]."""
        extra = _vision_extra(self, obs, layout, vision_embedding, image)
        if self.dense_adapters is not None:
            hidden = self.dense_adapters(obs, layout)
            return self.trunk(hidden if extra is None else hidden + extra.to(hidden.dtype))
        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        width = self.adapters[0].out_features
        hidden = obs.new_zeros(obs.shape[0], width)
        for index, rows in groups:
            # Cast on the way in: the rows' dtype is the adapters', the zeros' the observation's.
            hidden[rows] = self.adapters[index](self.norms[index](obs[rows, : self.obs_dims[index]])).to(
                hidden.dtype)
        if extra is not None:
            hidden = hidden + extra.to(hidden.dtype)
        return self.trunk(hidden)

    def carry(self, encoded: torch.Tensor, memory: torch.Tensor, dones: torch.Tensor) -> torch.Tensor:
        """Run the GRU over a sequence: `encoded` [T, N, H] from encode(), `memory` [N, R] the state its first
        decision was taken with, `dones` [T, N] where an episode ended (the next decision starts cleared). Returns
        the features each decision was taken with, [T, N, R]. Only the GRU cell is sequential; it is the small part."""
        if self.memory is None:
            return encoded

        return _carry_sequence(self.memory, self.recurrent_size, encoded, memory, dones)

    def features(self, obs: torch.Tensor, layout: torch.Tensor, memory: torch.Tensor | None = None,
                 groups=None, image: torch.Tensor | None = None,
                 vision_embedding: torch.Tensor | None = None) -> torch.Tensor:
        """The trunk's output for flat rows, through the GRU when there is one: what every head reads.
        `vision_embedding`: the camera's embedding of these rows when the caller has it (as for encode)."""
        return self.features_from(self.encode(obs, layout, groups, vision_embedding, image), memory)

    def features_from(self, hidden: torch.Tensor, memory: torch.Tensor | None = None) -> torch.Tensor:
        """features() from the trunk's output on: the GRU, when there is one."""
        if self.memory is not None:
            carried = (memory.reshape(-1, self.recurrent_size) if memory is not None
                       else hidden.new_zeros(hidden.shape[0], self.recurrent_size))
            hidden = self.memory(hidden, carried)
        return hidden

    def action_distribution(self, features: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                            goal: torch.Tensor | None = None, groups=None,
                            obs: torch.Tensor | None = None) -> Categorical:
        """The actions of flat rows whose features are `features`, under `goal` where the actor has goals. `obs`
        lets the sight list's pointer heads score their per-slot actions."""
        return Categorical(logits=self.action_logits(features, layout, mask, goal, groups, obs))

    def with_foresight(self, features: torch.Tensor) -> torch.Tensor:
        """The features with the foresight head's predictions fed back (Component P, layer 2), or as they are."""
        if self.foresight_proj is None:
            return features
        return features + self.foresight_proj(self.foresight(features).detach().to(features.dtype))

    def slow_step(self, features: torch.Tensor, slow: torch.Tensor) -> torch.Tensor:
        """One step of the slow loop over flat rows: the slow memory after this decision's features (detached: the
        slow loop learns on top of the fast one without pulling on it)."""
        return self.slow_memory(self.with_foresight(features).detach(), slow)

    def policy_features(self, features: torch.Tensor, goal: torch.Tensor | None = None) -> torch.Tensor:
        """The final policy features the action head reads -- the predictions fed back, then the goal's conditioning
        -- and the look head with it."""
        features = self.with_foresight(features)
        if self.goal_embedding is not None and goal is not None:
            features = self.goal_embedding.condition(features, goal.reshape(-1))
        return features

    def look(self, features: torch.Tensor, layout: torch.Tensor, goal: torch.Tensor | None = None,
             deterministic: bool = False) -> tuple[torch.Tensor, torch.Tensor]:
        """The free look's choice for flat rows (LookHead.sample): (choice [N, heads], log probability [N])."""
        return self.look_head.sample(self.policy_features(features, goal), layout, deterministic)

    def look_terms(self, features: torch.Tensor, layout: torch.Tensor, choice: torch.Tensor,
                   goal: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """The free look's (log probability [N], entropy [N]) of choices taken (LookHead.evaluate)."""
        return self.look_head.evaluate(self.policy_features(features, goal), layout, choice)

    def action_logits(self, features: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                      goal: torch.Tensor | None = None, groups=None, obs: torch.Tensor | None = None) -> torch.Tensor:
        """action_distribution's masked logits, unnormalised (for sample_logits)."""
        features = self.policy_features(features, goal)

        if self.dense_heads is not None:
            dense = self.dense_heads
            own = torch.where(dense.valid[layout.long()], dense(features, layout), MASKED_LOGIT)
            logits = own if own.shape[-1] == mask.shape[-1] else nn.functional.pad(
                own, (0, mask.shape[-1] - own.shape[-1]), value=MASKED_LOGIT)
            return masked_logits(self._with_sight_pointers(logits, features, layout, obs), mask)

        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        logits = features.new_full((features.shape[0], mask.shape[-1]), MASKED_LOGIT)
        for index, rows in groups:
            logits[rows, : self.action_counts[index]] = self.heads[index](features[rows]).to(logits.dtype)
        return masked_logits(self._with_sight_pointers(logits, features, layout, obs), mask)

    def _with_sight_pointers(self, logits: torch.Tensor, features: torch.Tensor, layout: torch.Tensor,
                             obs: torch.Tensor | None) -> torch.Tensor:
        """The sight list's presses scored by their slots' tokens (SightEntities.with_pointers)."""
        if self.sight_pointers is None or obs is None:
            return logits
        return self.vision.sight.with_pointers(logits, features, obs.reshape(-1, obs.shape[-1]), layout,
                                               self.sight_pointers)

    def goal_distribution(self, features: torch.Tensor, obs: torch.Tensor | None = None,
                          layout: torch.Tensor | None = None) -> Categorical:
        """Which goal to pursue next, from the same features, masked by what the goal block says is there."""
        return Categorical(logits=self.goal_head.logits(features, obs, layout))

    def decide_goals(self, features: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, held: torch.Tensor,
                     queue: torch.Tensor, clock: torch.Tensor, deterministic: bool,
                     uniform: bool = False, image: torch.Tensor | None = None, plan: torch.Tensor | None = None,
                     serial: torch.Tensor | None = None, uniform_cells: bool = False,
                     single: bool = False) -> dict[str, torch.Tensor]:
        """**The goal decision of one step** (next-run plan, Wave 4), the same in the rollout, its captured graph,
        the teachers and the module's runtime. Flat rows; `held` the goal (pair) held, `queue` [rows, S - 2] the goals
        waiting (-1 none), `clock` the goal clock's choices. In order:

        - an ended secondary is dropped (the sim dropped it too);
        - an ended primary with a queue is replaced by the queue's head, with no choice made (the plan that queued
          it keeps the credit);
        - a choice is made on the clock, where the primary ended with nothing queued, or on the block's event.

        Returns the pair held now (`goal`), the queue, which rows chose (`chosen`), their log probability (0 where
        none), the slots drawn [rows, S] (what the slow update scores) and the observed signals. `uniform`: draw the
        primary uniformly over the goals on offer (the eval arm random_goal), a host flag the training graph never
        sets; `uniform_cells` and `single` the eval arms random_cell and no_plan (GoalHead.draw).

        **The plan of cells** (free-choice-goals, with the cell head and `image`, the rows' image bytes): `plan` [rows, 4]
        are the cell words the seat holds at the four positions (0 none; ticket << CELL_BITS | block + 1) and `serial`
        [rows] the choices it has made this episode. The words follow the goals as the goals move -- an ended secondary
        loses its word, a promotion shifts the queue's up -- and a choice writes the words of the cells it drew, each
        with a ticket of its own (the sim latches a cell once, at the ticket's first sight, in the frame of the
        observation it was drawn from). Returned as `cells` [rows, S] (the blocks drawn, -1 none) and `plan`."""
        head = self.goal_head
        rows = features.shape[0]
        signals = head.signals(obs, layout)
        if head.slots <= 1:
            chosen = clock | signals["ended"]
            logits = head.logits(features, obs, layout)
            if uniform:
                logits = uniform_logits(logits)
            drawn, _ = sample_logits(logits, deterministic)
            goal = torch.where(chosen, drawn, held.long())
            return {"goal": goal, "queue": queue, "chosen": chosen,
                    "log_prob": torch.where(chosen, log_prob_of(logits, goal), torch.zeros_like(logits[:, 0])),
                    "slots": goal[:, None], **signals}

        primary, secondary = split_goal_pair(held, head.count)
        secondary = torch.where(signals["secondary_ended"], torch.full_like(secondary, -1), secondary)
        promote = signals["ended"] & (queue[:, 0] >= 0)
        primary = torch.where(promote, queue[:, 0], primary)
        shifted = torch.cat([queue[:, 1:], torch.full_like(queue[:, :1], -1)], dim=-1)
        queue = torch.where(promote[:, None], shifted, queue)
        chosen = clock | (signals["ended"] & ~promote) | signals["event"]

        slots, log_prob, _, cells = head.draw(features, obs, layout, deterministic, uniform=uniform, image=image,
                                              uniform_cells=uniform_cells, single=single)
        primary = torch.where(chosen, slots[:, 0], primary)
        secondary = torch.where(chosen, slots[:, 1], secondary)
        queue = torch.where(chosen[:, None], slots[:, 2:], queue)
        # Two cell goals share a joint id (159) and differ by their cell: only another goal repeated is dropped.
        secondary = torch.where((secondary == primary) & (primary != head.cell_joint),
                                torch.full_like(secondary, -1), secondary)
        out = {"goal": goal_pair(primary, secondary, head.count), "queue": queue, "chosen": chosen,
               "log_prob": torch.where(chosen, log_prob, torch.zeros_like(log_prob)), "slots": slots, "cells": cells,
               **signals}
        if plan is not None and serial is not None and head.cell_on and image is not None:
            out["plan"] = self._plan_words(plan, serial, cells, signals["secondary_ended"], promote, chosen)
        return out

    def _plan_words(self, plan: torch.Tensor, serial: torch.Tensor, cells: torch.Tensor,
                    secondary_ended: torch.Tensor, promote: torch.Tensor, chosen: torch.Tensor) -> torch.Tensor:
        """The cell words held at the four plan positions [rows, 4] after a decision (see decide_goals)."""
        first, second, third, fourth = plan.unbind(dim=-1)
        none = torch.zeros_like(first)
        second = torch.where(secondary_ended, none, second)
        first, third, fourth = (torch.where(promote, third, first), torch.where(promote, fourth, third),
                                torch.where(promote, none, fourth))
        held = torch.stack([first, second, third, fourth], dim=-1)
        positions = min(cells.shape[-1], plan.shape[-1])
        fresh = []
        for position in range(plan.shape[-1]):
            if position >= positions:
                fresh.append(none)
                continue
            ticket = ((serial + 1) * plan.shape[-1] + position) % TICKET_COUNT + 1
            block = cells[:, position]
            fresh.append(torch.where(block >= 0, (ticket << CELL_BITS) | (block + 1), none))
        return torch.where(chosen[:, None], torch.stack(fresh, dim=-1), held)

    def _forward(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, groups=None,
                 memory: torch.Tensor | None = None, goal: torch.Tensor | None = None,
                 image: torch.Tensor | None = None) -> tuple[Categorical, torch.Tensor, tuple[int, ...]]:
        lead = obs.shape[:-1]
        obs, layout, mask = obs.reshape(-1, obs.shape[-1]), layout.reshape(-1), mask.reshape(-1, mask.shape[-1])
        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))

        hidden = self.features(obs, layout, memory, groups, image)
        dist = self.action_distribution(hidden, layout, mask, goal, groups, obs)
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
                 goal_count: int = 0, recurrent_size: int = 0, goal_targets: int = 1,
                 goal_slots: int = 1, vision=None,
                 vision_encoder: "VisionEncoder | None" = None):
        super().__init__()
        if not hidden:
            raise ValueError("the critic needs at least one hidden layer")
        # The goal the actor is pursuing (mappo.goal_count) is part of what the value depends on: the same situation
        # is worth different things while resting and while fighting. Without it the critic averages over goals and
        # the advantage says nothing about which goal was the right one, which is what lets goals collapse into one.
        self.goal_count = goal_count * max(1, goal_targets)
        self.goal_embedding = (GoalEmbedding(goal_count, goal_targets, hidden[0], goal_slots > 1) if goal_count
                               else None)
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
        # The camera: the actor's encoder when given one (`vision_encoder`, MappoTrainer: one encoder, both losses'
        # gradients), else its own; the join onto its adapters' output is always its own.
        _attach_vision(self, vision, vision_encoder)

    def encode(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
               goal: torch.Tensor | None = None, groups=None,
               vision_embedding: torch.Tensor | None = None, image: torch.Tensor | None = None) -> torch.Tensor:
        """Everything that depends only on this decision -- the state, the seat's own observation and its goal --
        for flat rows, before the GRU. A replayed sequence encodes every step in one pass and then carries the memory
        through them (carry). `vision_embedding` as for LayoutActor.encode."""
        hidden, own = self.encode_goal_free(state, obs, layout, groups, vision_embedding, image)
        return self.encode_goal(hidden, own, goal)

    def encode_goal_free(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
                         groups=None, vision_embedding: torch.Tensor | None = None,
                         image: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """encode's first half, which does not need the goal: (the state's encoding, the seat's own). The larger
        part of the critic's work, so a rollout decision runs it beside the actor choosing the goal."""
        hidden = self.state_encoder(self.state_norm(state))
        if self.dense_adapters is not None:
            own = self.dense_adapters(obs, layout)
        else:
            own = torch.zeros_like(hidden)
            for index, rows in groups if groups is not None else _per_layout(layout, len(self.adapters)):
                own[rows] = self.adapters[index](self.norms[index](obs[rows, : self.obs_dims[index]])).to(
                    own.dtype)
        if self.vision is not None:
            own = own + vision_term(self, obs, layout, vision_embedding, image).to(own.dtype)
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
             groups=None, memory: torch.Tensor | None = None, image: torch.Tensor | None = None,
             vision_embedding: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """One decision: (value [...], the memory carried out). Without a GRU the memory out is whatever came in.
        `vision_embedding`: the camera's embedding of these rows when the caller has it (the actor's, in a rollout
        decision), else it is computed from `image`."""
        lead = obs.shape[:-1]
        state, obs, layout = state.reshape(-1, state.shape[-1]), obs.reshape(-1, obs.shape[-1]), layout.reshape(-1)
        return self.step_encoded(self.encode(state, obs, layout, goal, groups, vision_embedding=vision_embedding,
                                             image=image), lead, memory)

    def step_encoded(self, encoded: torch.Tensor, lead, memory: torch.Tensor | None = None):
        """step() from the encoding on: the GRU and the value head."""
        if self.memory is None:
            return self.head(encoded).reshape(lead), (memory if memory is not None else encoded.new_zeros((*lead, 0)))

        carried = self.memory(encoded, memory.reshape(-1, self.recurrent_size) if memory is not None
                              else encoded.new_zeros(encoded.shape[0], self.recurrent_size))
        return self.head(carried).reshape(lead), carried.reshape(*lead, self.recurrent_size)

    def forward(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, goal: torch.Tensor | None = None,
                groups=None, memory: torch.Tensor | None = None, image: torch.Tensor | None = None) -> torch.Tensor:
        """state [..., S], obs [..., O], layout [...], goal [...] (with a goal head) -> value [...]. `groups` as for
        LayoutActor.forward; `memory` [..., recurrent_size] the state carried in; `image` as for LayoutActor."""
        return self.step(state, obs, layout, goal, groups, memory, image)[0]
