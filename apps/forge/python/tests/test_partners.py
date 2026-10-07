"""Co-op partners in party seats and the evaluation arms (animus.partners; dungeon-curriculum I7)."""

from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

pytest.importorskip("torch")

from animus import protocol as p  # noqa: E402
from animus.config import EvalConfig, PartnerConfig, TrainConfig, from_dict  # noqa: E402
from animus.evaluation import run_evaluation  # noqa: E402
from animus.partners import (  # noqa: E402
    KIND_SNAPSHOT, PartnerPool, Partners, PartyRule, partner_snapshot, with_partners_chooser)
from animus.progress import ProgressWriter  # noqa: E402
from animus.train import TrainingRun  # noqa: E402
from test_cast import checkpoint  # noqa: E402

CONFIGS = Path(__file__).resolve().parent.parent / "configs"
LAYOUTS = (p.Layout("warrior_dps", 3, 4), p.Layout("mage_dps", 3, 4))


def spec(envs: int = 4, agents: int = 3) -> p.Spec:
    return p.Spec(version=p.PROTOCOL_VERSION, num_envs=envs, agents_per_env=agents, obs_dim=3, state_dim=4,
                  num_actions=4, episode_info_dim=2, goal_count=0, tick_ms=50, decision_ticks=1,
                  episode_seconds=1, scenario="fake", layouts=LAYOUTS,
                  episode_info_names=("present", "score_outcome"))


def party_stage(seats: int = 3) -> dict:
    return {"format": 3, "seats": seats, "state": {"arena_first": 0, "arena_count": 2},
            "arenas": [{"name": "party", "plan": "party", "team_seats": 0},
                       {"name": "solo", "plan": "solo", "team_seats": 0}],
            "cast": [], "layouts": {}}


def state_for(arenas: list[int]) -> np.ndarray:
    state = np.zeros((len(arenas), 4), dtype=np.float32)
    for env, arena in enumerate(arenas):
        state[env, arena] = 1.0
    return state


def step_for(arenas: list[int], agents: int = 3, present=None, done=None) -> SimpleNamespace:
    envs = len(arenas)
    return SimpleNamespace(
        obs=np.zeros((envs, agents, 3), np.float32), mask=np.ones((envs, agents, 4), bool),
        layout=np.zeros((envs, agents), np.int64), state=state_for(arenas),
        present=np.ones((envs, agents), bool) if present is None else present,
        done=np.zeros(envs, bool) if done is None else done,
        episode_info=np.zeros((envs, agents, 2), np.float32), image=None)


def config(**overrides) -> PartnerConfig:
    values = dict(share=1.0, max_partners=2, rate_window=4, floor=0.1)
    values.update(overrides)
    return PartnerConfig(**values)


# ------------------------------------------------------------------ config

def test_partners_and_arms_are_off_by_default():
    defaults = TrainConfig()
    assert not defaults.cast.partners.enabled and defaults.cast.partners.share == 0.0
    assert defaults.eval.arms == {}


@pytest.mark.parametrize("name", ["move1_controls.yaml", "move2_seek.yaml"])
def test_the_movement_stages_load_with_no_partners_and_no_stand_in(name):
    """M1 and M2 resume where they left off after the rebuild: nothing of I7 is on in them."""
    loaded = TrainConfig.load(CONFIGS / name)
    assert not loaded.cast.partners.enabled
    assert loaded.cast.partners == PartnerConfig()
    assert loaded.eval.arms == {}       # no "with_human" arm: no evaluation asks the sim for the stand-in


def test_the_partner_config_loads_and_resolves():
    loaded = from_dict(TrainConfig, {"cast": {"partners": {
        "stages": ["stage_g1_roles"], "paths": ["{runs_dir}/x/best.pt"], "share": 0.3, "max_partners": 2,
        "snapshot_every_env_steps": 1000, "eval_partners": ["stage_c3_survive"]}},
        "eval": {"arms": {"with_human": 64, "with_partners": 32}, "arms_every": 2}})
    partners = loaded.cast.partners
    assert partners.enabled and partners.stages == ("stage_g1_roles",) and partners.max_partners == 2
    assert partners.members("/runs", "r") == ["/runs/stage_g1_roles/best.pt", "/runs/x/best.pt"]
    assert partners.eval_members("/runs", "r") == ["/runs/stage_c3_survive/best.pt"]
    assert loaded.eval.arms == {"with_human": 64, "with_partners": 32} and loaded.eval.arms_every == 2

    with pytest.raises(ValueError):
        EvalConfig(arms={"with_ghosts": 8})
    with pytest.raises(ValueError):
        PartnerConfig(share=1.5)
    with pytest.raises(ValueError):
        PartnerConfig(max_partners=0)
    with pytest.raises(ValueError):
        from_dict(TrainConfig, {"cast": {"partners": {"nonsense": 1}}})


def test_the_stand_in_flag_rides_on_mode():
    payload = p.encode_mode(True, 1000, 64, stand_in=True)
    assert p.decode_mode_stand_in(payload) and p.MODE.unpack(payload)[3] == p.MODE_FLAG_STAND_IN
    assert not p.decode_mode_stand_in(p.encode_mode(True, 1000, 64))
    both = p.encode_mode(True, 1, 2, "random", stand_in=True)
    assert p.MODE.unpack(both)[3] == p.MODE_FLAG_STAND_IN
    assert p.decode_mode(both)[3] == "random"


# ------------------------------------------------------------------ the pool

def test_party_rule_reads_the_arena():
    rule = PartyRule.from_stage(party_stage(), 3)
    assert rule.envs(state_for([0, 1, 0])).tolist() == [True, False, True]
    assert PartyRule.from_stage({"arenas": [{"plan": "solo"}]}, 3) is None
    # A stage without an arena one-hot: every episode a party when every arena is one.
    assert PartyRule.from_stage({"arenas": [{"plan": "raid"}], "seats": 5}, 3).envs(np.zeros((2, 1))).all()


def test_the_pool_prefers_the_partners_the_party_does_worst_with(tmp_path):
    pool = PartnerPool(config(), spec(), party_stage(), tmp_path, "cpu")
    for name in ("a.pt", "b.pt", "c.pt"):
        pool.add(checkpoint(tmp_path, name))
    assert len(pool.active()) == 3
    # Nobody met yet: an even draw.
    assert np.allclose(pool.probabilities(pool.active()), 1 / 3)
    for _ in range(4):
        pool.record(0, 10.0)    # the party does well with a
        pool.record(1, 0.0)     # ... badly with b
    # c is not yet met: it counts as the worst, so it is met; b outweighs a; a keeps the floor.
    probabilities = pool.probabilities(pool.active())
    assert probabilities[1] > probabilities[0] and probabilities[2] >= probabilities[1]
    assert probabilities[0] == pytest.approx(0.1 / (0.1 + 1.1 + 1.1))
    pool.record(2, 5.0)
    probabilities = pool.probabilities(pool.active())
    assert probabilities[1] > probabilities[2] > probabilities[0] > 0.0
    drawn = [pool.draw_for(0, np.random.default_rng(seed)) for seed in range(300)]
    assert drawn.count(1) > drawn.count(2) > drawn.count(0) > 0

    # A score average over rate_window episodes, and a missing member skipped with its name.
    assert pool.members[0].episodes == 4 and pool.members[0].score == pytest.approx(10.0)
    assert pool.add(tmp_path / "nowhere.pt") is None and str(tmp_path / "nowhere.pt") in pool.missing
    assert pool.to_json()["active"] == 3


def test_snapshots_join_the_newest_share_and_the_pruning(tmp_path):
    parent = checkpoint(tmp_path, "parent.pt")
    pool = PartnerPool(config(pool_size=2, keep_newest=1, newest_share=0.6), spec(), party_stage(), tmp_path, "cpu")
    pool.add(parent)
    for tag in ("step_1", "step_2", "step_3"):
        assert partner_snapshot(tmp_path, parent, tag) is not None
    assert partner_snapshot(tmp_path, parent, "step_1") is None
    assert pool.reload() == 3
    snapshots = [m for m in pool.active() if m.kind == KIND_SNAPSHOT]
    assert len(snapshots) == 2 and pool.newest_snapshot().path.name == "step_3.pt"
    active = pool.active()
    assert pool.probabilities(active)[active.index(pool.newest_snapshot())] == pytest.approx(0.6)
    assert pool.probabilities(active).sum() == pytest.approx(1.0)


def test_a_member_without_the_seats_layout_is_never_drawn_for_it(tmp_path):
    pool = PartnerPool(config(), spec(), party_stage(), tmp_path, "cpu")
    pool.add(checkpoint(tmp_path))
    assert pool.draw_for(0, np.random.default_rng(0)) == 0
    assert pool.draw_for(7, np.random.default_rng(0)) == -1


# ------------------------------------------------------------------ the rows

def test_partners_take_party_seats_only_and_leave_a_live_one(tmp_path):
    partners = Partners(config(max_partners=5), spec(envs=4), party_stage(), tmp_path, "cpu",
                        [str(checkpoint(tmp_path))], seed=3)
    present = np.ones((4, 3), bool)
    present[3, 1:] = False     # a party of one: no room for a partner
    step = step_for([0, 1, 0, 0], present=present)
    rows = partners.rows(step)
    assert not rows[1].any()                    # a solo arena never has partners
    assert rows[0].any() and not rows[0].all()  # at least one live seat
    assert rows[2].any() and not rows[2].all()
    assert not rows[3].any()
    # Drawn once an episode: the same rows until the episode ends.
    assert (partners.rows(step) == rows).all()

    live = np.full((4, 3), 3, np.int64)
    step.mask[:] = False
    step.mask[..., 1] = True
    actions = partners.act(step, live, rows)
    assert (actions[rows] == 1).all() and (actions[~rows] == 3).all()

    # The ended episodes' partner rows, then a fresh draw.
    done = np.array([True, False, False, False])
    assert (partners.ended_rows(done) == rows[:1]).all()
    partners.clear(done)
    assert (partners.assigned[0] == -1).all() and partners.fresh[0] and not partners.fresh[2]
    stats = partners.stats()
    assert stats["partner_rows"] > 0.0 and stats["partner_members"] == 1.0


def test_a_partner_never_takes_the_drilled_seat(tmp_path):
    """G1's drills (stage.json drill_seat 0): the drilled role's seat is the learner's whatever is drawn, and with it
    live every other seat may be a partner; an arena that drills no one (-1) leaves any seat to the draw."""
    stage = party_stage()
    stage["arenas"][0]["drill_seat"] = 0
    rule = PartyRule.from_stage(stage, 3)
    assert rule.drill_seat(state_for([0, 1]), 0) == 0 and rule.drill_seat(state_for([0, 1]), 1) == -1
    partners = Partners(config(max_partners=5), spec(envs=64), stage, tmp_path, "cpu", [str(checkpoint(tmp_path))],
                        seed=11)
    rows = partners.rows(step_for([0] * 64))
    assert not rows[:, 0].any()
    assert rows[:, 1:].all(axis=1).any(), "with the drilled seat live, both other seats can be partners"
    # Without a drill, the first seat is drawn like the others.
    undrilled = Partners(config(max_partners=5), spec(envs=64), party_stage(), tmp_path, "cpu",
                         [str(checkpoint(tmp_path))], seed=11)
    assert undrilled.rows(step_for([0] * 64))[:, 0].any()


def test_the_party_outcome_scores_the_partner(tmp_path):
    partners = Partners(config(max_partners=1), spec(envs=1), party_stage(), tmp_path, "cpu",
                        [str(checkpoint(tmp_path))], seed=0)
    step = step_for([0])
    rows = partners.rows(step)
    seat = int(np.flatnonzero(rows[0])[0])
    ended = step_for([0], done=np.array([True]))
    ended.episode_info[0, :, 0] = 1.0
    ended.episode_info[0, :, 1] = 4.0
    ended.episode_info[0, seat, 1] = 100.0      # the partner's own row is not the party's measure
    partners.observe_ended(ended, score_column=1, present_column=0)
    member = partners.pool.members[0]
    assert member.episodes == 1 and member.score == pytest.approx(4.0)


def test_partner_rows_are_never_samples(tmp_path):
    """TrainingRun._act_on_rows: a partner's row takes its frozen actor's action and is recorded not present."""
    partners = Partners(config(max_partners=1), spec(envs=2), party_stage(), tmp_path, "cpu",
                        [str(checkpoint(tmp_path))], seed=1)
    part = step_for([0, 1])
    part.mask[:] = False
    part.mask[..., 2] = True
    recorded = {}
    acting = SimpleNamespace(memory=None, critic_memory=None, look=None, look_log_prob=None)
    trainer = SimpleNamespace(
        rollout_stream=None,
        act_and_value=lambda *a, **k: (np.zeros((2, 3), np.int64), None, None, None, None, None),
        wire_look=lambda look: None, wire_goals=lambda goals: None)
    run = SimpleNamespace(trainer=trainer, acting=SimpleNamespace(take=lambda rows: acting, put=lambda rows, a: None),
                          frozen=np.zeros(0, np.int64), cast=None, exploit=None, partners=partners,
                          env_steps=0)
    decision = SimpleNamespace(buffer=None, set=lambda rows, **fields: recorded.update(fields))
    TrainingRun._act_on_rows(run, part, slice(0, 2), decision, lambda *a: None)
    rows = partners.assigned >= 0
    assert rows[0].sum() == 1 and not rows[1].any()
    assert (recorded["present"] == ~rows).all()
    assert (recorded["actions"][rows] == 2).all() and (recorded["actions"][~rows] == 0).all()


# ------------------------------------------------------------------ the evaluation arms

class PartyEnv:
    """In-process fake sim: two envs of a three-seat party, 1-decision episodes; seat a earns 1 + a. Records MODE."""

    SPEC = spec(envs=2)

    def __init__(self):
        self.modes = []
        self.next_seed = 0

    def _step(self, done: bool) -> p.Step:
        seeds = np.full(2, p.NO_EPISODE_SEED, np.uint32)
        if done:
            seeds[:] = [self.next_seed, self.next_seed + 1]
            self.next_seed += 2
        info = np.zeros((2, 3, 2), np.float32)
        info[..., 0] = 1.0
        return p.Step(decision=0, obs=np.zeros((2, 3, 3), np.float32), state=state_for([0, 0]),
                      mask=np.ones((2, 3, 4), bool), layout=np.zeros((2, 3), np.uint16),
                      present=np.ones((2, 3), bool), reward=np.tile(np.array([1.0, 2.0, 3.0], np.float32), (2, 1)),
                      done=np.array([done, done]), terminated=np.array([done, done]),
                      final_obs=np.zeros((2, 3, 3), np.float32), final_state=np.zeros((2, 4), np.float32),
                      episode_info=info, episode_seed=seeds)

    def set_mode(self, evaluate, seed_base=0, episodes=0, baseline="", **options):
        self.modes.append((evaluate, options.get("stand_in", False)))
        self.next_seed = 0
        return self._step(False)

    def step(self, actions, goals=None):
        return self._step(True)


def test_the_human_arm_asks_the_sim_for_the_stand_in():
    env = PartyEnv()
    result, _ = run_evaluation(env, env.SPEC, lambda step: np.zeros((2, 3), np.int32), episodes=4, seed=1,
                               stand_in=True)
    assert env.modes == [(True, True), (False, False)]
    assert result.episodes == 12
    run_evaluation(env, env.SPEC, lambda step: np.zeros((2, 3), np.int32), episodes=4, seed=1)
    assert env.modes[2] == (True, False)        # the plain evaluation is all bots


def test_the_partner_arm_leaves_the_partners_rows_unscored(tmp_path):
    env = PartyEnv()
    arm = Partners(config(max_partners=1, deterministic=True), env.SPEC, party_stage(), tmp_path, "cpu",
                   [str(checkpoint(tmp_path))], seed=5, share=1.0, deterministic=True, snapshots=False)
    chooser, excluded = with_partners_chooser(lambda step: np.zeros((2, 3), np.int32), arm)
    result, _ = run_evaluation(env, env.SPEC, chooser, episodes=4, seed=1, excluded=excluded)
    # Every party had one partner: two of its three seats scored.
    assert result.episodes == 8
    assert result.returns.sum() < 4 * (1.0 + 2.0 + 3.0)


def test_an_arm_reading_reaches_the_status(tmp_path):
    loaded = TrainConfig()
    loaded.status.headline = ("clear_rate", "clear_rate_with_human")
    writer = ProgressWriter(tmp_path, loaded, spec())
    assert writer.arm_columns("with_human") == ("clear_rate",)
    writer.arm_evaluated("with_human", {"score": 1.5, "episodes": 64, "clear_rate": 0.62})
    writer.evaluated(100, 2.0, None, SimpleNamespace(history=[(100, 2.0)], best=2.0, best_env_steps=100,
                                                   evals_since_best=0), summary={"clear_rate": 0.7})
    path = writer.write("training", 1, 100)
    import json
    progress = json.loads(path.read_text())
    assert progress["eval_clear_rate_with_human"] == pytest.approx(0.62)
    assert progress["eval_clear_rate"] == pytest.approx(0.7)
    assert progress["eval_with_human_score"] == pytest.approx(1.5)


def test_a_stage_named_as_a_partner_is_the_policy_it_ended_with_not_its_best_at_a_rung(tmp_path):
    """best.pt is the best at the stage's current rung (a gate-stepped ladder's first evaluation at a new rung overwrites
    it, and one saved under the old bug can be the policy from step 0), so a bare stage name is its latest.pt; a path
    is taken as given (best_rung<k>.pt, or a best.pt someone chose)."""
    done, only_best, neither = tmp_path / "done", tmp_path / "only_best", tmp_path / "neither"
    for folder, names in ((done, ("best.pt", "latest.pt")), (only_best, ("best.pt",))):
        folder.mkdir()
        for name in names:
            (folder / name).write_text(name)
    config = PartnerConfig(stages=("done", "only_best", "neither"),
                           paths=(f"{done}/best.pt", "{runs_dir}/done/best_rung1.pt"), eval_partners=("done",))
    runs = str(tmp_path)
    assert config.members(runs, "r") == [
        f"{done}/latest.pt", f"{only_best}/best.pt", f"{neither}/best.pt", f"{done}/best.pt", f"{done}/best_rung1.pt"]
    assert config.eval_members(runs, "r") == [f"{done}/latest.pt"]
