"""The live learner's shapes and numbers, pinned: a trim of the learner must not change them.

Two stages are built on CPU the way tools/resume_check.py builds them (train.trainer_inputs and train.make_trainer):

  move2_seek    the real M2 stage.json (fixtures/seek_stage.json, copied from the 2026-10-07 backup) with the
                spec the checkpoint saved (fixtures/seek_spec.json) and configs/move2_seek.yaml: ten class layouts,
                the camera and the mental map, the goal block, the recurrent actor and critic;
  tiny_layouts  a small synthetic stage of two layouts with different widths and no camera, to cover a second layout set.

Each records (a) the actor's and critic's state-dict keys and shapes, and (b) the update's reported statistics and the
parameters' checksums after ONE update on a fixed synthetic rollout. The parameters are overwritten from per-key seeds
before the update (so the numbers do not depend on how many random draws the construction makes), the rollout's
actions are sampled from a seeded generator, and the rollout is a few decisions of random observations.

golden/learner_update.json holds the expected values. Regenerate it only for a change that is meant to move the live
learner (`python tests/test_golden_update.py write`, in the dev container); a trim never does.
"""

import json
import sys
import zlib
from pathlib import Path

import numpy as np
import pytest
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from animus.config import TrainConfig  # noqa: E402
from animus.mappo.buffer import RolloutBuffer  # noqa: E402
from animus.protocol import Layout, Spec  # noqa: E402
from animus.train import make_trainer, trainer_inputs  # noqa: E402

HERE = Path(__file__).resolve().parent
GOLDEN = HERE / "golden" / "learner_update.json"
CONFIGS = HERE.parent / "configs"
STEPS, ENVS = 8, 4


def seek_case():
    stage = json.loads((HERE / "fixtures" / "seek_stage.json").read_text())
    raw = json.loads((HERE / "fixtures" / "seek_spec.json").read_text())
    layouts = tuple(Layout(item["name"], item["obs_dim"], item["num_actions"]) for item in raw["layouts"])
    spec = Spec(version=raw["version"], num_envs=ENVS, agents_per_env=raw["agents_per_env"], obs_dim=raw["obs_dim"],
                state_dim=raw["state_dim"], num_actions=raw["num_actions"], episode_info_dim=raw["episode_info_dim"],
                goal_count=raw["goal_count"], tick_ms=raw["tick_ms"], decision_ticks=raw["decision_ticks"],
                episode_seconds=raw["episode_seconds"], scenario=raw["scenario"], layouts=layouts,
                episode_info_names=tuple(raw.get("episode_info_names", ())), image_bytes=raw["image_bytes"],
                look_heads=raw["look_heads"], map_bytes=raw["map_bytes"])
    config = TrainConfig.load(CONFIGS / "move2_seek.yaml")
    return config, spec, stage


def tiny_case():
    entries, spans = {}, {}
    for name, blocks in {"warrior": [("core", 24, 5), ("move", 6, 3)], "mage": [("core", 20, 4), ("move", 9, 2)]}.items():
        obs = actions = 0
        spans = []
        for block, features, count in blocks:
            spans.append({"name": block, "obs": [obs, features], "actions": [actions, count]})
            obs += features
            actions += count
        entries[name] = {"obs_dim": obs, "num_actions": actions, "blocks": spans}
    stage = {"stage": "tiny_layouts", "format": 3, "layouts": entries, "episode_info": ["seat"], "tuning": {}}
    layouts = tuple(Layout(name, entry["obs_dim"], entry["num_actions"]) for name, entry in entries.items())
    spec = Spec(version=24, num_envs=ENVS, agents_per_env=1, obs_dim=max(l.obs_dim for l in layouts), state_dim=7,
                num_actions=max(l.num_actions for l in layouts), episode_info_dim=1, goal_count=0, tick_ms=250,
                decision_ticks=1, episode_seconds=60, scenario="tiny_layouts", layouts=layouts,
                episode_info_names=("seat",))
    config = TrainConfig.load(CONFIGS / "move2_seek.yaml", ["mappo.hidden=[16,16]", "mappo.recurrent_size=8",
                                                            "mappo.slow_goal_size=0", "mappo.goal_count=0",
                                                            "mappo.minibatches=2", "mappo.epochs=2"])
    return config, spec, stage


CASES = {"move2_seek": seek_case, "tiny_layouts": tiny_case}


def reseed(module: torch.nn.Module, tag: str) -> None:
    """Every parameter from a seed of its own name: the numbers below do not depend on construction's random draws."""
    for name, parameter in module.named_parameters():
        generator = torch.Generator().manual_seed(zlib.crc32(f"{tag}.{name}".encode()))
        parameter.data.copy_(torch.randn(parameter.shape, generator=generator) * 0.05)


def digest(module: torch.nn.Module) -> dict:
    return {key: [float(tensor.double().sum()), float(tensor.double().abs().sum())]
            for key, tensor in module.state_dict().items() if tensor.is_floating_point()}


def shapes(module: torch.nn.Module) -> dict:
    return {key: list(tensor.shape) for key, tensor in module.state_dict().items()}


def run_case(name: str) -> dict:
    config, spec, stage = CASES[name]()
    inputs = trainer_inputs(config, spec, stage)
    trainer = make_trainer(config, spec, inputs, device="cpu")
    reseed(trainer.actor, "actor")
    reseed(trainer.critic, "critic")
    trainer.set_goal_space(stage, [layout.name for layout in spec.layouts])
    trainer.sync_rollout()
    out = {"actor_shapes": shapes(trainer.actor), "critic_shapes": shapes(trainer.critic)}

    rng = np.random.default_rng(11)
    torch.manual_seed(5)
    agents = spec.agents_per_env
    buffer = RolloutBuffer(STEPS, ENVS, agents, spec.obs_dim, spec.state_dim, spec.num_actions,
                           trainer.foresight_outputs, trainer.recurrent_size, bool(trainer.goal_count),
                           trainer.slow_goal_size, trainer.goal_slots, spec.camera_bytes, len(trainer.look_heads))
    acting = trainer.acting_state(ENVS, agents)
    layout = (np.arange(ENVS)[:, None] + np.zeros((1, agents), np.int64)) % len(spec.layouts)
    widths = np.array([spec.layouts[i].num_actions for i in layout.reshape(-1)]).reshape(layout.shape)
    for step in range(STEPS):
        obs = rng.random((ENVS, agents, spec.obs_dim), dtype=np.float32)
        state = rng.random((ENVS, spec.state_dim), dtype=np.float32)
        mask = np.arange(spec.num_actions)[None, None, :] < widths[..., None]
        image = (rng.integers(0, 256, (ENVS, agents, spec.camera_bytes), dtype=np.uint8)
                 if spec.camera_bytes else None)
        memory = acting.memory.copy() if acting.memory is not None else None
        critic_memory = acting.critic_memory.copy() if acting.critic_memory is not None else None
        actions, log_probs, values, foresight, goals, chosen = trainer.act_and_value(
            obs, mask, layout, state, state=acting, image=image)
        look = trainer.wire_look(acting.look)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, foresight, memory, goals,
                            critic_memory, chosen, image, look, acting.look_log_prob)
        rewards = rng.standard_normal((ENVS, agents)).astype(np.float32)
        done = np.zeros(ENVS, bool)
        done[step % ENVS] = step % 3 == 2
        buffer.add_outcome(rewards, done, done & (step % 2 == 0), np.zeros((ENVS, agents), np.float32),
                           np.zeros((ENVS, agents, trainer.foresight_outputs), np.float32))
        acting.clear(done)
    obs = rng.random((ENVS, agents, spec.obs_dim), dtype=np.float32)
    state = rng.random((ENVS, spec.state_dim), dtype=np.float32)
    image = (rng.integers(0, 256, (ENVS, agents, spec.camera_bytes), dtype=np.uint8) if spec.camera_bytes else None)
    last = trainer.value(state, obs, layout, acting.goal, acting.critic_memory, image)
    buffer.finish(last, config.mappo.gamma, config.mappo.gae_lambda,
                  last_foresight=trainer.foresight_of(obs, layout, acting.memory, image),
                  slow_goal=(config.mappo.slow_goal_gamma, config.mappo.slow_goal_lambda)
                  if trainer.slow_goal_size else None)
    stats = trainer.update(buffer)
    out["stats"] = {key: float(value) for key, value in sorted(stats.items()) if not key.endswith("_seconds")}
    out["actor"], out["critic"] = digest(trainer.actor), digest(trainer.critic)
    return out


def compare(got, expected, path="") -> None:
    if isinstance(expected, dict):
        assert sorted(got) == sorted(expected), f"{path}: keys differ: {sorted(set(got) ^ set(expected))}"
        for key in expected:
            compare(got[key], expected[key], f"{path}.{key}")
    elif isinstance(expected, list):
        assert len(got) == len(expected), path
        for index, (a, b) in enumerate(zip(got, expected)):
            compare(a, b, f"{path}[{index}]")
    else:
        assert got == pytest.approx(expected, rel=2e-3, abs=2e-4), path


@pytest.mark.parametrize("name", sorted(CASES))
def test_the_live_learner_is_unchanged(name):
    compare(run_case(name), json.loads(GOLDEN.read_text())[name])


if __name__ == "__main__":
    if sys.argv[1:] == ["write"]:
        GOLDEN.write_text(json.dumps({name: run_case(name) for name in sorted(CASES)}, indent=1, sort_keys=True) + "\n")
