"""animus.human.parity: bots against players in one capture. Synthetic clients -- the same generator for both sides
-- read as parity; a bot side with the wrong jump launch or a fast heartbeat is flagged."""

import json
import math

import numpy as np
import pytest

import human_capture_writer as w
from animus.human import __main__ as cli
from animus.human import parity
from animus.human import reader as r

GRAVITY, JUMP = parity.GRAVITY, parity.JUMP_SPEED
FORWARD, BACKWARD, STRAFE_LEFT, WALKING = 0x1, 0x2, 0x4, 0x100
FALLING = 0x1000
OP = parity.OP


class Client:
    """A 3.3.5a client's packets for scripted play: a change at the key press, a heartbeat `heartbeat_ms` after the
    last packet while moving, SET_FACING when the mouse has turned the facing 0.1 rad (frame-limited: a little more)."""

    def __init__(self, player: int, t0: int, rng: np.random.Generator, jump: float = JUMP, heartbeat_ms: int = 500,
                 source: int = 0):
        self.player, self.t, self.rng, self.source = player, t0, rng, source
        self.x = self.y = self.z = self.o = 0.0
        self.flags = 0
        self.last = t0
        self.jump, self.heartbeat_ms = jump, heartbeat_ms
        self.reported = 0.0
        self.records: list[bytes] = []

    def send(self, opcode: int, fall_ms: int = 0, jz: float = 0.0) -> None:
        self.records.append(w.record(10, {
            "ms": self.t + 40, "player": self.player, "client_ms": self.t - w.T0 + 5000, "opcode": opcode,
            "move_flags": self.flags, "x": self.x, "y": self.y, "z": self.z, "o": self.o % (2 * math.pi),
            "fall_ms": fall_ms, "jump_zspeed": jz, "map": 1, "source": self.source}))
        self.last = self.t
        self.reported = self.o

    def move(self, seconds: float, speed: float, heading: float = 0.0, turn: float = 0.0, rise: float = 0.0) -> None:
        """Hold the keys for `seconds` at `speed` along the facing (+ `heading`), the mouse turning at `turn` rad/s."""
        step = 5
        frame = 0.1 + float(self.rng.uniform(0.0, 0.005))
        for _ in range(int(seconds * 1000 / step)):
            self.t += step
            self.x += speed * step / 1000 * math.cos(self.o + heading)
            self.y += speed * step / 1000 * math.sin(self.o + heading)
            self.z += rise * step / 1000 / seconds
            self.o += turn * step / 1000
            if turn and abs(self.o - self.reported) >= frame:
                self.send(OP["SET_FACING"])
                frame = 0.1 + float(self.rng.uniform(0.0, 0.005))
            elif self.t - self.last >= self.heartbeat_ms:
                self.send(OP["HEARTBEAT"])

    def press(self, opcode: int, flags: int) -> None:
        self.flags = flags
        self.t += int(self.rng.integers(20, 120))
        self.send(opcode)

    def jump_now(self, speed: float) -> None:
        v = self.jump
        self.flags |= FALLING
        self.send(OP["JUMP"], jz=-v)
        air = 2 * v / GRAVITY
        steps = int(air * 1000 / 10)
        for _ in range(steps):
            self.t += 10
            self.x += speed * 0.01
            if self.t - self.last >= self.heartbeat_ms:
                self.send(OP["HEARTBEAT"], fall_ms=0)
        self.flags &= ~FALLING
        self.send(OP["FALL_LAND"], fall_ms=int(round(air * 1000)))

    def ledge(self, height: float) -> None:
        self.flags |= FALLING
        t = math.sqrt(2 * height / GRAVITY)
        self.t += int(t * 1000)
        self.z -= height
        self.flags &= ~FALLING
        self.send(OP["FALL_LAND"], fall_ms=int(round(t * 1000)))

    def play(self, minutes: float) -> None:
        end = self.t + minutes * 60_000
        self.send(OP["STOP"])
        while self.t < end:
            self.press(OP["START_FORWARD"], FORWARD)
            self.move(float(self.rng.uniform(2, 4)), 7.0, turn=float(self.rng.choice([-1.5, 0.0, 1.0, 2.0])))
            self.jump_now(7.0)
            self.move(float(self.rng.uniform(1, 2)), 7.0)
            self.move(0.5, 7.0, rise=float(self.rng.uniform(0.6, 1.0)))   # a step up, within one heartbeat
            self.move(0.5, 7.0)
            self.press(OP["START_BACKWARD"], BACKWARD)
            self.move(float(self.rng.uniform(1, 2)), 4.5, heading=math.pi)
            self.press(OP["START_STRAFE_LEFT"], STRAFE_LEFT)
            self.move(float(self.rng.uniform(1, 2)), 7.0, heading=math.pi / 2)
            self.press(OP["SET_WALK_MODE"], FORWARD | WALKING)
            self.move(float(self.rng.uniform(1, 2)), 2.5)
            self.press(OP["SET_RUN_MODE"], FORWARD)
            self.move(1.0, 7.0)
            self.ledge(float(self.rng.uniform(3, 8)))
            self.press(OP["STOP"], 0)
            self.t += int(self.rng.integers(300, 900))


def write_capture(root, bots_jump: float = JUMP, bots_heartbeat: int = 500, movers: int = 3, minutes: float = 2.0):
    d = w.hour_dir(root, "2026-10-05", 14)
    t0 = w.T0
    sessions = [w.header("session", t0)]
    moves = [w.header("move", t0)]
    companion = [w.header("companion", t0)]
    rng = np.random.default_rng(7)
    for i in range(2 * movers):
        bot = i >= movers
        player = 1000 + i
        sessions.append(w.record(1, {"ms": t0, "player": player, "session": player, "class_": 1, "race": 1,
                                     "level": 70, "map": 1, "kind": 1 if bot else 0}))
        moves.append(w.record(11, {"ms": t0, "player": player, "walk": 2.5, "run": 7.0, "run_back": 4.5,
                                   "swim": 4.722222, "swim_back": 2.5, "flight": 7.0, "flight_back": 4.5,
                                   "turn_rate": math.pi, "pitch_rate": math.pi}))
        client = Client(player, t0 + 100, np.random.default_rng(100 + i % movers),
                        jump=bots_jump if bot else JUMP, heartbeat_ms=bots_heartbeat if bot else 500,
                        source=r.SOURCE_CONTROLLER if bot else r.SOURCE_CLIENT)
        client.play(minutes)
        moves += client.records
        if bot:
            ms = t0
            for _ in range(int(minutes * 60 * 4)):
                ms += int(rng.integers(245, 262))
                companion.append(w.record(50, {"ms": ms, "companion": player, "owner": 1, "model": "m"}))
    for k in range(4000):
        moves.append(w.record(14, {"ms": t0 + k * 30, "map": 1, "diff_ms": int(rng.integers(12, 45))}))
    files = {}
    for name, records in (("session-all", sessions), ("move-1", moves), ("companion-all", companion)):
        path = w.write_gz(d / f"{name}.bin.gz", [b"".join(records)])
        files[path.name] = {"records": len(records), "bytes": path.stat().st_size}
    w.write_index(d, "2026-10-05T14", files, players=2 * movers, sessions=2 * movers)
    return root


def study(root):
    return parity.report(parity.study_capture(r.CaptureDir(root)), source=str(root))


def rows(rep, section=None, context="all"):
    return {row["metric"]: row for row in rep["rows"]
            if row["context"] == context and (section is None or row["section"] == section)}


def test_identical_clients_are_at_parity(tmp_path):
    rep = study(write_capture(tmp_path))
    physics = rows(rep, "physics")
    for name in ("JUMP_SPEED", "jump apex (yd)", "GRAVITY", "HEARTBEAT_MS", "MOUSE_FACING_THRESHOLD", "STEP_UP"):
        assert physics[name]["flag"] == "pass", physics[name]
    # Both sides measure the controller's constants from the packets.
    assert physics["JUMP_SPEED"]["players"] == pytest.approx(JUMP, rel=1e-4)
    assert physics["GRAVITY"]["players"] == pytest.approx(GRAVITY, rel=0.02)
    assert physics["HEARTBEAT_MS"]["bots"] == pytest.approx(500.0, abs=10.0)
    assert 0.1 <= physics["MOUSE_FACING_THRESHOLD"]["players"] <= 0.112
    kin = rows(rep, "kinematics")
    for mode in ("run", "walk", "back", "strafe"):
        assert kin[f"{mode} speed / speed in force"]["players"] == pytest.approx(1.0, abs=0.02)
        assert kin[f"{mode} speed / speed in force"]["flag"] == "pass"
    assert kin["jumps a minute"]["flag"] == "pass" and kin["fall height (yd) distribution"]["flag"] == "pass"
    cadence = rows(rep, "cadence")
    assert cadence["HEARTBEAT_MS (median)"]["flag"] == "pass" and cadence["changes a minute"]["flag"] == "pass"
    assert rows(rep, "realism")["realism EMD (all motion)"]["flag"] == "pass"
    assert not [row for row in rep["attention"] if row["kind"] == "controller"], rep["attention"]
    # The realm's timing: ticks of 12-45 ms and decisions 245-262 ms -- no jitter needed by the stated rule.
    timing = rep["timing"]
    assert 12 <= timing["tick_ms"]["p50"] <= 45 and timing["tick_ms"]["n"] == 4000
    assert timing["decision_ms"]["p50"] == pytest.approx(253, abs=6) and timing["jitter_recommended"] is False
    # The class/race/map/mode context is matched as well as `all`.
    assert any(row["context"] == "warrior/race1/map1/ground" for row in rep["rows"])


def test_a_wrong_jump_speed_is_flagged(tmp_path):
    rep = study(write_capture(tmp_path, bots_jump=9.0))
    physics = rows(rep, "physics")
    assert physics["JUMP_SPEED"]["flag"] == "attention"
    assert physics["JUMP_SPEED"]["bots"] == pytest.approx(9.0, rel=1e-4)
    assert physics["jump apex (yd)"]["flag"] == "attention"
    assert physics["HEARTBEAT_MS"]["flag"] == "pass"


def test_a_fast_heartbeat_is_flagged(tmp_path):
    rep = study(write_capture(tmp_path, bots_heartbeat=250))
    cadence = rows(rep, "cadence")
    assert cadence["HEARTBEAT_MS (median)"]["flag"] == "attention"
    assert cadence["ms before HEARTBEAT"]["flag"] == "attention"
    assert rows(rep, "physics")["HEARTBEAT_MS"]["bots"] == pytest.approx(250.0, abs=10.0)
    assert rows(rep, "physics")["JUMP_SPEED"]["flag"] == "pass"


def test_the_cli_writes_both_reports(tmp_path, capsys):
    root = write_capture(tmp_path / "cap", bots_jump=9.0, minutes=1.0)
    out = tmp_path / "out" / "parity.md"
    assert cli.main(["parity", str(root), "--out", str(out)]) == 0
    printed = json.loads(capsys.readouterr().out)
    assert printed["summary"]["attention"] >= 1 and "all: JUMP_SPEED" in printed["attention"]
    text = (tmp_path / "out" / "parity.md").read_text()
    assert "| attention | all | JUMP_SPEED |" in text and "## realm timing" in text
    saved = json.loads((tmp_path / "out" / "parity.json").read_text())
    assert saved["thresholds"]["physics"] == parity.PHYSICS_TOLERANCE
