"""Every movement stage runs 50 ms world ticks.

The player controller's mouse-look facing rule -- a facing report whenever the yaw has moved 0.1 rad from the last
report -- and its heartbeat are checked once per world tick (until the report check moves to the controller's
sub-step, player-controller C4). At a coarser tick the server's view of a seat's facing is staler than a real
client's would be, which is what a model trained here would then be taught against. The decision cadence is a
separate question (movement-curriculum plan 8.5: decisions at 250, 125 or 100 ms against a world tick held at
50 ms), so the tick is checked as DecisionMs / TicksPerDecision, whatever the decision interval is.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
STAGES_CPP = REPO / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum" / "Stages" / "Stages.cpp"
CONF = REPO / "src" / "server" / "apps" / "worldserver" / "worldserver.conf.dist"
CONFIGS = REPO / "apps" / "forge" / "python" / "configs"

WORLD_TICK_MAX_MS = 50
MOVEMENT_STAGE = re.compile(r"move\d+_\w+")


def movement_stages() -> list[str]:
    names = re.findall(r'\.Name = "(\w+)",\s*\.Suffix', STAGES_CPP.read_text())
    return sorted(name for name in names if MOVEMENT_STAGE.fullmatch(name))


def conf_value(key: str) -> str | None:
    found = re.search(rf"^{re.escape(key)}\s*=\s*(\S+)\s*$", CONF.read_text(), re.M)
    return found.group(1).strip('"') if found else None


def world_tick_ms(stage: str) -> tuple[int, int, int]:
    """(decision ms, ticks a decision, world tick ms) the conf template gives `stage`."""
    decision = int(conf_value("AnimusForge.DecisionMs") or 250)
    ticks = int(conf_value(f"AnimusForge.Stage.{stage}.TicksPerDecision")
                or conf_value("AnimusForge.TicksPerDecision") or 1)
    return decision, ticks, decision // max(1, ticks)


def test_the_movement_stages_are_found():
    """Never vacuous: a movement stage config means a movement stage this parse has to find."""
    if any(MOVEMENT_STAGE.fullmatch(p.stem) for p in CONFIGS.glob("*.yaml")):
        assert movement_stages(), "movement stage configs exist but Stages.cpp parsed to no movement stage"


def test_every_movement_stage_ticks_the_world_every_50_ms():
    stale = []
    for stage in movement_stages():
        decision, ticks, tick = world_tick_ms(stage)
        if decision % ticks or tick > WORLD_TICK_MAX_MS:
            stale.append(f"{stage}: {decision} ms decisions at {ticks} ticks = {decision / ticks:g} ms world ticks")
    assert not stale, (
        "movement stages whose world tick is over 50 ms -- the controller's facing (a report per 0.1 rad of yaw) and "
        "heartbeat are checked once a world tick, so the server's facing would be staler than a client's; set "
        "AnimusForge.Stage.<name>.TicksPerDecision = DecisionMs / 50 in worldserver.conf.dist:\n" + "\n".join(stale))


def test_the_check_reads_a_coarse_tick_as_stale(monkeypatch, tmp_path):
    conf = tmp_path / "worldserver.conf.dist"
    conf.write_text("AnimusForge.DecisionMs = 250\nAnimusForge.TicksPerDecision = 1\n"
                    "AnimusForge.Stage.move2_ground.TicksPerDecision = 5\n")
    monkeypatch.setattr(__import__(__name__), "CONF", conf)
    assert world_tick_ms("move2_ground") == (250, 5, 50)
    assert world_tick_ms("move3_vertical") == (250, 1, 250)
