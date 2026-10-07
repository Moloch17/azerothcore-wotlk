"""Frozen checkpoints with a camera in cast and partner seats (animus.cast, animus.partners): a camera-era checkpoint
-- camera, mental map, entity list, sight list and free look -- rebuilt from its own stage.json, loaded by name, fed
each of its rows' camera bytes as the live actor is, choosing its own look; refused when the stage cannot feed it as
it was fed; its memory per seat cleared with the episode; offloaded past the pool's resident cap. The old checkpoints
without a camera act as they always did, and hold the look."""

import copy
from types import SimpleNamespace

import numpy as np
import pytest

pytest.importorskip("torch")

import torch  # noqa: E402

import test_mental_map as mm  # noqa: E402
import test_sight as sg  # noqa: E402
import test_vision_encoder as ve  # noqa: E402
from animus import protocol as p  # noqa: E402
from animus.cast import Cast, CastActor, Residency  # noqa: E402
from animus.config import CastConfig, MappoConfig, PartnerConfig, TrainConfig  # noqa: E402
from animus.distill import Distiller, build_teacher, frozen_actor  # noqa: E402
from animus.mappo.networks import vision_of  # noqa: E402
from animus.mappo.trainer import MappoTrainer  # noqa: E402
from animus.partners import PartnerPool, Partners, with_partners_chooser  # noqa: E402
from animus.train import save_checkpoint  # noqa: E402
from test_cast import checkpoint as blind_checkpoint  # noqa: E402

NAMES = ve.NAMES  # warrior, priest (both with the camera), director (none)
HEADS = (7, 5, 5)
LOOK_NAMES = ("yaw", "pitch", "zoom")
CORE = {"warrior": 5, "priest": 9}
BASE = 5  # core 3 + move 2 actions
ACTIONS = BASE + len(sg.PRESSES) * sg.SLOTS
STATE_DIM = 4
ENVS, AGENTS = 2, 3


def camera_stage(map_block: bool = True, sight_block: bool = True, sight_revision: int = 1,
                 image: dict | None = None) -> dict:
    """A party stage.json whose two seat layouts have every block the camera's encoder reads: the vision block (its
    scalars, revision 5, the free look), the entity list, the mental map and the sight list, then a goal block; the
    director without a camera."""
    layouts = {}
    for name, core in CORE.items():
        blocks = [ve.block("core", 0, core, (0, 3)), ve.block("move", core, 3, (3, 2))]
        at = core + 3
        shown = dict(image or ve.IMAGE)
        shown["look"] = {"heads": list(HEADS), "names": list(LOOK_NAMES)}
        blocks.append(ve.block("vision", at, ve.SPAN, revision=5, image=shown))
        at += ve.SPAN
        blocks.append(ve.block("entities", at, ve.SLOTS * ve.FEATURES, revision=1, entities=ve.entities(at)))
        at += ve.SLOTS * ve.FEATURES
        if map_block:
            blocks.append(ve.block("map", at, mm.MAP_SCALARS, revision=1, map=dict(mm.CROP)))
            at += mm.MAP_SCALARS
        if sight_block:
            blocks.append(ve.block("sight", at, sg.SLOTS * sg.WIDTH, (BASE, len(sg.PRESSES) * sg.SLOTS),
                                   revision=sight_revision, sight=sg.sight(at, BASE)))
            at += sg.SLOTS * sg.WIDTH
        blocks.append(ve.block("goal", at, 2))
        layouts[name] = {"obs_dim": at + 2, "blocks": blocks}
    layouts["director"] = {"obs_dim": 8, "blocks": [ve.block("core", 0, 6, (0, 4)), ve.block("goal", 6, 2)]}
    return {"format": 3, "stage": "test_cast_vision", "seats": AGENTS,
            "state": {"arena_first": 0, "arena_count": 2},
            "arenas": [{"name": "party", "plan": "party", "team_seats": 0},
                       {"name": "solo", "plan": "solo", "team_seats": 0}],
            "cast": [], "layouts": layouts}


def shapes(stage: dict) -> list[tuple[int, int]]:
    sight = any(b["name"] == "sight" for b in stage["layouts"]["warrior"]["blocks"])
    return [(stage["layouts"][name]["obs_dim"], (ACTIONS if sight else BASE) if name != "director" else 4)
            for name in NAMES]


def spec_of(stage: dict) -> p.Spec:
    layouts = tuple(p.Layout(name, obs, actions) for name, (obs, actions) in zip(NAMES, shapes(stage)))
    crop = any(b["name"] == "map" for b in stage["layouts"]["warrior"]["blocks"])
    return p.Spec(version=p.PROTOCOL_VERSION, num_envs=ENVS, agents_per_env=AGENTS,
                  obs_dim=max(l.obs_dim for l in layouts), state_dim=STATE_DIM,
                  num_actions=max(l.num_actions for l in layouts), episode_info_dim=2, goal_count=0, tick_ms=50,
                  decision_ticks=1, episode_seconds=1, scenario="camera", layouts=layouts,
                  episode_info_names=("present", "score_outcome"), image_bytes=ve.BYTES, look_heads=len(HEADS),
                  map_bytes=mm.M if crop else 0)


def trainer_of(stage: dict, seed: int = 0) -> tuple[MappoTrainer, TrainConfig]:
    """A tiny trainer with the stage's camera, its encoder and joins stirred so the camera moves the policy."""
    config = TrainConfig()
    config.mappo = MappoConfig(hidden=(16, 16), recurrent_size=8, goal_count=0)
    torch.manual_seed(seed)
    trainer = MappoTrainer(shapes(stage), STATE_DIM, config.mappo, "cpu", "cpu", vision=vision_of(stage, NAMES))
    with torch.no_grad():
        for name, parameter in trainer.actor.named_parameters():
            if name.startswith(("vision", "look_head", "sight_pointers")):
                parameter.normal_(0.0, 0.5)
    return trainer, config


def save(tmp_path, stage: dict, name: str = "camera.pt", seed: int = 0):
    trainer, config = trainer_of(stage, seed)
    path = tmp_path / name
    save_checkpoint(path, trainer, config, spec_of(stage), 0, 0, {"stage": stage})
    return path, trainer


def step_rows(stage: dict, seed: int = 0, arenas=(0, 0)) -> SimpleNamespace:
    """A fake STEP of ENVS x AGENTS: warriors and priests with plausible entity and sight lists, camera rows of a
    frame and a map crop (as decode_step joins them), the director in no seat."""
    generator = torch.Generator().manual_seed(seed)
    spec = spec_of(stage)
    rows = ENVS * AGENTS
    layout = torch.tensor([0, 1, 0, 1, 0, 1][:rows])
    obs = torch.zeros(rows, spec.obs_dim)
    image = ve.blank(rows, (255, 128, 0, 0, 0))
    for row in range(rows):
        name = NAMES[int(layout[row])]
        entry = stage["layouts"][name]
        spans = {b["name"]: b["obs"] for b in entry["blocks"]}
        for block in ("core", "move", "vision", "map", "goal"):
            if block in spans:
                first, count = spans[block]
                obs[row, first:first + count] = torch.rand(count, generator=generator)
        first, _ = spans["entities"]
        listed = torch.rand(ve.SLOTS, ve.FEATURES, generator=generator)
        listed[:, 0] = 1.0
        listed[:, 1] = torch.randint(0, 23, (ve.SLOTS,), generator=generator).float()
        listed[:, 2] = torch.randint(0, 5000, (ve.SLOTS,), generator=generator).float()
        listed[:, 3] = torch.randint(0, 2, (ve.SLOTS,), generator=generator).float()
        listed[:, 19] = 0.0
        obs[row, first:first + ve.SLOTS * ve.FEATURES] = listed.reshape(-1)
        if "sight" in spans:
            first, _ = spans["sight"]
            seen = torch.rand(sg.SLOTS, sg.WIDTH, generator=generator)
            seen[:, 0] = (torch.rand(sg.SLOTS, generator=generator) < 0.7).float()
            seen[:, 1] = torch.randint(0, 23, (sg.SLOTS,), generator=generator).float()
            seen[:, 2] = torch.randint(0, 5000, (sg.SLOTS,), generator=generator).float()
            seen[:, 3] = torch.randint(0, 2, (sg.SLOTS,), generator=generator).float()
            seen[:, sg.MEMORY] = torch.randint(0, 80, (sg.SLOTS,), generator=generator).float()
            seen[:sg.VISIBLE, sg.VISIBLE_COLUMN] = 1.0
            seen[sg.VISIBLE:, sg.VISIBLE_COLUMN] = 0.0
            obs[row, first:first + sg.SLOTS * sg.WIDTH] = seen.reshape(-1)
        image[row] = ve.frame(generator)
    if spec.map_bytes:
        image = torch.cat([image, mm.crops(rows, seed)], dim=1)
    state = np.zeros((ENVS, STATE_DIM), np.float32)
    for env, arena in enumerate(arenas):
        state[env, arena] = 1.0
    return SimpleNamespace(
        obs=obs.reshape(ENVS, AGENTS, -1).numpy(), mask=np.ones((ENVS, AGENTS, spec.num_actions), bool),
        layout=layout.reshape(ENVS, AGENTS).numpy().astype(np.int64), state=state,
        present=np.ones((ENVS, AGENTS), bool), done=np.zeros(ENVS, bool),
        episode_info=np.zeros((ENVS, AGENTS, 2), np.float32),
        image=image.reshape(ENVS, AGENTS, -1).numpy())


def all_rows() -> np.ndarray:
    return np.ones((ENVS, AGENTS), bool)


# ------------------------------------------------------------------ the builder


def test_a_camera_checkpoint_rebuilds_its_whole_camera_from_its_own_stage(tmp_path):
    stage = camera_stage()
    path, trainer = save(tmp_path, stage)
    checkpoint = torch.load(path, map_location="cpu", weights_only=False)
    actor = frozen_actor(checkpoint, "cpu")
    assert actor.vision is not None and actor.vision.map is not None and actor.vision.sight is not None
    assert actor.look_head is not None and actor.sight_pointers is not None
    assert not any(parameter.requires_grad for parameter in actor.parameters())
    for name, value in trainer.actor.state_dict().items():
        if name.startswith(("vision", "look_head", "sight_pointers")):
            torch.testing.assert_close(actor.state_dict()[name], value)

    teacher = build_teacher(checkpoint, spec_of(stage), stage, "cpu")
    assert teacher.camera_bytes == ve.BYTES + mm.M and teacher.look_heads == HEADS
    assert sorted(teacher.layouts) == [0, 1, 2]


def test_camera_weights_without_their_stage_json_are_refused(tmp_path):
    path, _ = save(tmp_path, camera_stage())
    checkpoint = torch.load(path, map_location="cpu", weights_only=False)
    checkpoint["stage"] = None
    with pytest.raises(ValueError, match="no stage.json describes it"):
        frozen_actor(checkpoint, "cpu")


def test_a_camera_the_stage_cannot_feed_as_it_was_fed_is_refused(tmp_path):
    path, _ = save(tmp_path, camera_stage())
    checkpoint = torch.load(path, map_location="cpu", weights_only=False)

    def refused(stage, match):
        with pytest.raises(ValueError, match=match):
            build_teacher(copy.deepcopy(checkpoint), spec_of(stage), stage, "cpu")

    refused(camera_stage(sight_revision=2), "sight block is revision 1 for the teacher, 2 in the stage")
    refused(camera_stage(map_block=False), "reads a mental map")
    refused(camera_stage(sight_block=False), "reads its sight block")
    refused(camera_stage(image={**ve.IMAGE, "classes": 20}), "image classes is 23, the stage's 20")
    looking = camera_stage()
    for name in CORE:
        looking["layouts"][name]["blocks"][2]["image"]["look"] = {"heads": [5, 5, 5], "names": list(LOOK_NAMES)}
    refused(looking, "free look's heads")
    blind = ve.stage(None)
    blind.update(arenas=camera_stage()["arenas"], state=camera_stage()["state"])
    blind_spec = SimpleNamespace(layouts=tuple(p.Layout(n, blind["layouts"][n]["obs_dim"], 5) for n in NAMES))
    with pytest.raises(ValueError, match="has a camera and the stage's layout has none"):
        build_teacher(copy.deepcopy(checkpoint), blind_spec, blind, "cpu")


def test_a_teacher_on_fewer_classes_is_fed_and_one_on_more_is_refused(tmp_path):
    """The class table only appends (Vision::Class; the embedding has class_limit rows): a checkpoint trained on 23
    classes plays a stage that sends 24 (ground_hazard added) -- it never met the new one, nothing it knew moved -- and
    one on the same count plays as before; a checkpoint on more classes than the stage sends is refused. A change of a
    class's meaning is a vision revision, refused on its own."""
    path, _ = save(tmp_path, camera_stage())
    checkpoint = torch.load(path, map_location="cpu", weights_only=False)
    teacher_classes = ve.IMAGE["classes"]
    for stage_classes in (teacher_classes, teacher_classes + 1):
        stage = camera_stage(image={**ve.IMAGE, "classes": stage_classes})
        assert build_teacher(copy.deepcopy(checkpoint), spec_of(stage), stage, "cpu") is not None
    fewer = camera_stage(image={**ve.IMAGE, "classes": teacher_classes - 1})
    with pytest.raises(ValueError, match=f"image classes is {teacher_classes}, the stage's {teacher_classes - 1}"):
        build_teacher(copy.deepcopy(checkpoint), spec_of(fewer), fewer, "cpu")


def test_distillation_still_refuses_a_teacher_with_a_camera(tmp_path):
    stage = camera_stage()
    path, _ = save(tmp_path, stage)
    teacher = build_teacher(torch.load(path, map_location="cpu", weights_only=False), spec_of(stage), stage, "cpu")
    with pytest.raises(ValueError, match="teacher with a camera"):
        Distiller(stage, {"party": teacher})


# ------------------------------------------------------------------ acting


def test_a_camera_cast_actor_acts_and_looks_on_a_step_with_image_bytes(tmp_path):
    stage = camera_stage()
    path, _ = save(tmp_path, stage)
    actor = CastActor(path, spec_of(stage), stage, "cpu")
    step = step_rows(stage)
    rows = np.zeros((ENVS, AGENTS), bool)
    rows[:, 1:] = True
    fallback = np.full((ENVS, AGENTS), -7, dtype=np.int64)
    live_look = np.full((ENVS, AGENTS, 3), 9, dtype=np.int32)
    actions, look = actor.decide(step.obs, step.mask, step.layout, rows, fallback, step.image, live_look)
    assert actor.fallback_rows == 0
    assert (actions[:, 0] == -7).all() and ((actions[rows] >= 0) & (actions[rows] < ACTIONS)).all()
    # Its rows carry its own look, in range of every head; the others keep the live policy's.
    assert look.shape == (ENVS, AGENTS, 3) and look.dtype == np.int32
    assert (look[:, 0] == 9).all()
    for head, size in enumerate(HEADS):
        assert ((look[rows][:, head] >= 0) & (look[rows][:, head] < size)).all()
    assert (live_look == 9).all()  # the caller's array is not written into

    # The image is read: another frame and map, other logits.
    before = {index: logits.clone() for index, logits in actor.last_logits.items()}
    actor.reset_all()
    other = step_rows(stage, seed=5)
    other.obs = step.obs
    actor.decide(step.obs, step.mask, step.layout, rows, fallback, other.image)
    assert any(not torch.allclose(before[index], actor.last_logits[index]) for index in before)

    # A camera row has to come with its bytes.
    with pytest.raises(ValueError, match="image bytes"):
        actor.act(step.obs, step.mask, step.layout, rows, fallback)


def test_the_cast_actor_gives_the_same_logits_as_the_checkpoints_own_actor(tmp_path):
    stage = camera_stage()
    path, trainer = save(tmp_path, stage)
    actor = CastActor(path, spec_of(stage), stage, "cpu", deterministic=True)
    step = step_rows(stage, seed=3)
    actor.decide(step.obs, step.mask, step.layout, all_rows(), np.zeros((ENVS, AGENTS), np.int64), step.image)

    obs = torch.as_tensor(step.obs).reshape(ENVS * AGENTS, -1)
    layout = torch.as_tensor(step.layout).reshape(-1)
    mask = torch.as_tensor(step.mask).reshape(ENVS * AGENTS, -1)
    image = torch.as_tensor(step.image).reshape(ENVS * AGENTS, -1)
    trainer.actor.eval()
    with torch.no_grad():
        own = trainer.actor(obs, layout, mask, memory=torch.zeros(ENVS * AGENTS, 8), image=image).logits
    assert sorted(actor.last_logits) == [0, 1]
    for index, logits in actor.last_logits.items():
        picked = np.nonzero(step.layout.reshape(-1) == index)[0]
        torch.testing.assert_close(logits, own[picked][:, :logits.shape[-1]])


def test_device_buffer_images_are_indexed_where_they_live(tmp_path):
    """A step whose images are a view of the sim's buffers (a tensor, protocol 15) acts as one with host bytes."""
    stage = camera_stage()
    path, _ = save(tmp_path, stage)
    actor = CastActor(path, spec_of(stage), stage, "cpu", deterministic=True)
    step = step_rows(stage, seed=2)
    fallback = np.zeros((ENVS, AGENTS), np.int64)
    host_actions, host_look = actor.decide(step.obs, step.mask, step.layout, all_rows(), fallback, step.image)
    host_logits = {index: logits.clone() for index, logits in actor.last_logits.items()}
    actor.reset_all()
    device_actions, device_look = actor.decide(step.obs, step.mask, step.layout, all_rows(), fallback,
                                               torch.as_tensor(step.image))
    assert (host_actions == device_actions).all() and (host_look == device_look).all()
    for index, logits in host_logits.items():
        torch.testing.assert_close(logits, actor.last_logits[index])


def test_its_memory_is_per_seat_and_cleared_at_the_episode_end(tmp_path):
    stage = camera_stage()
    path, _ = save(tmp_path, stage)
    actor = CastActor(path, spec_of(stage), stage, "cpu")
    step = step_rows(stage)
    rows = np.zeros((ENVS, AGENTS), bool)
    rows[:, 2] = True
    actor.decide(step.obs, step.mask, step.layout, rows, np.zeros((ENVS, AGENTS), np.int64), step.image)
    assert np.abs(actor.memory[:, 2]).sum(axis=-1).min() > 0.0
    assert np.abs(actor.memory[:, :2]).sum() == 0.0
    actor.clear(np.array([True, False]))
    assert np.abs(actor.memory[0]).sum() == 0.0 and np.abs(actor.memory[1, 2]).sum() > 0.0


def test_an_m1_checkpoint_plays_in_a_stage_with_a_map_and_a_sight_list(tmp_path):
    """An M1 partner (camera, no map, no sight list) in an M2 party: its camera reads the image's leading bytes of
    the stage's row, and the blocks it never had stay out of its view."""
    m1 = camera_stage(map_block=False, sight_block=False)
    path, _ = save(tmp_path, m1, "m1.pt")
    stage = camera_stage()
    actor = CastActor(path, spec_of(stage), stage, "cpu")
    assert actor.teacher.camera_bytes == ve.BYTES
    step = step_rows(stage)
    actions, look = actor.decide(step.obs, step.mask, step.layout, all_rows(), np.full((ENVS, AGENTS), -1),
                                 step.image)
    assert ((actions >= 0) & (actions < BASE)).all()  # the sight presses are not its actions
    assert look.shape == (ENVS, AGENTS, 3)


def test_an_old_checkpoint_without_a_camera_still_acts_and_holds_the_look(tmp_path):
    stage = camera_stage()
    old = blind_checkpoint(tmp_path)  # test_cast's: two layouts of 3 observations and 4 actions, no stage blocks
    spec = SimpleNamespace(layouts=(p.Layout("warrior_dps", 3, 4), p.Layout("mage_dps", 3, 4)), image_bytes=ve.BYTES,
                           look_heads=3, agents_per_env=AGENTS)
    actor = CastActor(old, spec, stage, "cpu")
    assert actor.teacher.camera_bytes == 0 and actor.teacher.actor.vision is None
    obs = np.random.rand(ENVS, AGENTS, 3).astype(np.float32)
    mask = np.ones((ENVS, AGENTS, 4), bool)
    layout = np.zeros((ENVS, AGENTS), np.int64)
    rows = np.zeros((ENVS, AGENTS), bool)
    rows[:, 0] = True
    live = np.full((ENVS, AGENTS, 3), 1, np.int32)
    actions, look = actor.decide(obs, mask, layout, rows, np.full((ENVS, AGENTS), 9), None, live)
    assert ((actions[:, 0] >= 0) & (actions[:, 0] < 4)).all() and (actions[:, 1:] == 9).all()
    assert (look[:, 0] == np.array(p.LOOK_HOLD)).all() and (look[:, 1:] == 1).all()
    # And without a look anywhere (a stage with no look heads), none is made.
    spec.look_heads = 0
    plain = CastActor(old, spec, stage, "cpu")
    assert plain.decide(obs, mask, layout, rows, np.zeros((ENVS, AGENTS), np.int64))[1] is None


# ------------------------------------------------------------------ the pools


def partner_config(**overrides) -> PartnerConfig:
    values = dict(share=1.0, max_partners=2, rate_window=4, floor=0.1)
    values.update(overrides)
    return PartnerConfig(**values)


def test_the_partner_pool_lists_a_camera_checkpoint_as_usable(tmp_path):
    stage = camera_stage()
    path, _ = save(tmp_path, stage)
    pool = PartnerPool(partner_config(), spec_of(stage), stage, tmp_path, "cpu")
    member = pool.add(path)
    assert member is not None and pool.unusable == []
    assert member.layouts == frozenset({0, 1, 2}) and pool.covers(member, 0)
    # One its stage cannot feed is named unusable, with the reason.
    other, _ = save(tmp_path, camera_stage(sight_revision=2), "revised.pt")
    assert pool.add(other) is None and "revision" in pool.unusable[0]


def test_partners_act_and_look_and_the_eval_chooser_carries_the_look(tmp_path):
    stage = camera_stage()
    path, _ = save(tmp_path, stage)
    partners = Partners(partner_config(), spec_of(stage), stage, tmp_path, "cpu", [str(path)], seed=1)
    step = step_rows(stage)
    rows = partners.rows(step)
    assert rows.any() and not rows.all(axis=1).any()  # one live seat at least in every party
    live = np.zeros((ENVS, AGENTS), np.int64)
    actions, look = partners.act_and_look(step, live, rows, np.full((ENVS, AGENTS, 3), 9, np.int32))
    assert (look[~rows] == 9).all() and (look[rows] != 9).any()
    assert ((actions[rows] >= 0) & (actions[rows] < ACTIONS)).all()

    partners.reset_all()
    chooser, excluded = with_partners_chooser(
        lambda s: (np.zeros((ENVS, AGENTS), np.int64), None, np.full((ENVS, AGENTS, 3), 9, np.int32)), partners)
    actions, goals, look = chooser(step)
    assert goals is None and look.shape == (ENVS, AGENTS, 3)
    assert (look[~partners.rows(step)] == 9).all()


def test_the_cast_facade_passes_the_image_and_the_look(tmp_path):
    stage = camera_stage()
    stage["arenas"][0] = {"name": "party", "plan": "mirror", "team_seats": 0}
    path, _ = save(tmp_path, stage)
    config = CastConfig(opponents=str(path), opponent_share=1.0)
    cast = Cast(config, spec_of(stage), stage, tmp_path, "cpu", None)
    step = step_rows(stage)
    rows = cast.rows(step)
    assert rows[:, 1].all() and not rows[:, 0].any()
    actions, look = cast.act_and_look(step, np.zeros((ENVS, AGENTS), np.int64), rows,
                                      np.full((ENVS, AGENTS, 3), 9, np.int32))
    assert (look[:, 0] == 9).all() and ((look[:, 1] >= 0) & (look[:, 1] < np.array(HEADS))).all()


def test_the_resident_cap_offloads_the_least_recently_played(tmp_path):
    stage = camera_stage()
    pool = PartnerPool(partner_config(resident_members=1, max_partners=1), spec_of(stage), stage, tmp_path, "cpu")
    first = pool.add(save(tmp_path, stage, "a.pt", seed=1)[0])
    second = pool.add(save(tmp_path, stage, "b.pt", seed=2)[0])
    assert first is not None and second is not None
    assert pool.residency.limit == 1 and pool.residency.order[-1] is second.actor
    assert first.actor.resident_bytes() > 0
    # "cpu" is the home here, so offloading is a move to where it already is: the bookkeeping is what is checked.
    residency = Residency(1)
    calls = []
    fake = [SimpleNamespace(home="cuda", device="cuda", place=lambda d, i=i: calls.append((i, d))) for i in range(3)]
    for actor in fake:
        residency.touch(actor)
    assert calls[-1] == (1, "cpu") and (0, "cpu") in calls
    assert residency.order[-1] is fake[2] and len(residency.order) == 3
    with pytest.raises(ValueError, match="resident_members"):
        PartnerConfig(resident_members=-1)


def test_a_resident_cap_below_max_partners_is_refused_at_load():
    """Below the partners one decision plays it would move members to the host and back within every decision;
    0 (no cap) or max_partners and up load."""
    with pytest.raises(ValueError, match="max_partners"):
        PartnerConfig(max_partners=3, resident_members=2)
    for good in (0, 3, 4):
        PartnerConfig(max_partners=3, resident_members=good)
