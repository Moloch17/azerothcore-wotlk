"""Action hints: the hint block's columns are kept out of the networks, and the imitation loss reads them."""

import torch

from animus.mappo.networks import attach_blind_columns


class _Net(torch.nn.Module):
    def __init__(self):
        super().__init__()
        self.adapters = torch.nn.ModuleList([torch.nn.Linear(6, 3), torch.nn.Linear(4, 3)])


def test_blind_columns_stay_zero_through_training():
    net = _Net()
    attach_blind_columns(net, {0: [4, 5]})
    assert torch.all(net.adapters[0].weight[:, 4:] == 0)
    opt = torch.optim.Adam(net.parameters(), lr=0.1)
    for _ in range(5):
        x = torch.randn(8, 6)
        loss = net.adapters[0](x).pow(2).sum() + net.adapters[1](torch.randn(8, 4)).sum()
        opt.zero_grad()
        loss.backward()
        opt.step()
    assert torch.all(net.adapters[0].weight[:, 4:] == 0)
    assert torch.any(net.adapters[0].weight[:, :4] != 0)
    # A second call only zeroes again; it does not stack hooks.
    attach_blind_columns(net, {0: [4, 5]})
    assert torch.all(net.adapters[0].weight[:, 4:] == 0)


def test_hint_loss_weights_and_skips():
    from types import SimpleNamespace
    from animus.mappo.trainer import MappoTrainer

    fake = SimpleNamespace(config=SimpleNamespace(hint_coef=2.0), hint_at=torch.tensor([3, -1]))
    obs = torch.zeros(3, 5)
    obs[0, 3], obs[0, 4] = 2.0, 0.5      # layout 0, hint action 2, weight 0.5
    obs[1, 3], obs[1, 4] = 0.0, 0.5      # layout 0, no action: skipped
    layout = torch.tensor([0, 0, 1])     # row 2: a layout without the block
    logits = torch.zeros(3, 4, requires_grad=True)
    dist = torch.distributions.Categorical(logits=logits)
    stats = {}
    loss = MappoTrainer._hint_loss(fake, dist, obs, layout, torch.ones(3, dtype=torch.bool), stats)
    assert abs(float(loss) - 2.0 * torch.log(torch.tensor(4.0)).item()) < 1e-5
    loss.backward()
    assert logits.grad[1].abs().sum() == 0 and logits.grad[2].abs().sum() == 0
    assert stats["hint_n"] == 1.0 and abs(stats["hint_weight"] - 0.5) < 1e-6
