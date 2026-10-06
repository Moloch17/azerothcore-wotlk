"""animus.human parity: Animus companions against human players in one capture (parity.METRICS.md, with the
overseer's statistics). Synthetic clients -- one generator for both sides, capture format 3 -- read as parity; a
companion side with the wrong jump launch or a fast heartbeat is flagged, and survives the correction; a stratum
with few samples is inconclusive, never an attention."""

import json
import math

import numpy as np
import pytest

import human_capture_writer as w
from animus.human import __main__ as cli
from animus.human import motion, parity, realism
from animus.human import reader as r

GRAVITY, JUMP = parity.GRAVITY, parity.JUMP_SPEED
FORWARD, BACKWARD, STRAFE_LEFT, WALKING = 0x1, 0x2, 0x4, 0x100
FALLING, SWIMMING = 0x1000, 0x200000
OP = parity.OP
RESAMPLES = 300             # the tests' bootstrap; the default (parity.BOOTSTRAP, 1000) in test_the_cli_...


class Client:
    """A 3.3.5a client's packets for scripted play, frame by frame (4-17 ms frames): a change at the key press, a
    heartbeat `heartbeat_ms` after the last packet while moving, SET_FACING at the first frame the mouse has turned the
    facing 0.1 rad from the last packet (frame-limited: a little past it). The server takes each packet 40 ms after
    the client stamped it."""

    def __init__(self, player: int, t0: int, rng: np.random.Generator, jump: float = JUMP, heartbeat_ms: int = 500,
                 source: int = 0):
        self.player, self.t, self.rng, self.source = player, t0, rng, source
        self.x = self.y = self.z = self.o = 0.0
        self.flags = 0
        self.last = t0
        self.reported = 0.0
        self.jump, self.heartbeat_ms = jump, heartbeat_ms
        self.records: list[bytes] = []
        self.sent = 0

    def send(self, opcode: int, fall_ms: int = 0, jz: float = 0.0) -> None:
        self.records.append(w.record(10, {
            "ms": self.t + 40, "player": self.player, "client_ms": self.t - w.T0 + 5000, "opcode": opcode,
            "move_flags": self.flags, "x": self.x, "y": self.y, "z": self.z, "o": self.o % (2 * math.pi),
            "fall_ms": fall_ms, "jump_zspeed": jz, "map": 1, "source": self.source,
            "server_ms": self.t - w.T0 + 90_000 + 40}))
        self.last = self.t
        self.reported = self.o
        self.sent += 1

    def move(self, seconds: float, speed: float, heading: float = 0.0, turn: float = 0.0, rise: float = 0.0) -> None:
        """Hold the keys for `seconds` at `speed` along the facing (+ `heading`), the mouse turning at `turn` rad/s."""
        frame = 0.1
        end = self.t + int(seconds * 1000)
        while self.t < end:
            step = int(self.rng.integers(4, 18))                # a frame: 55-250 fps
            self.t += step
            self.x += speed * step / 1000 * math.cos(self.o + heading)
            self.y += speed * step / 1000 * math.sin(self.o + heading)
            self.z += rise * step / 1000 / seconds
            self.o += turn * step / 1000
            if turn and abs(self.o - self.reported) >= frame:
                self.send(OP["SET_FACING"])
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
        for _ in range(int(air * 1000 / 10)):
            self.t += 10
            self.x += speed * 0.01
            if self.t - self.last >= self.heartbeat_ms:
                self.send(OP["HEARTBEAT"])
        self.flags &= ~FALLING
        self.send(OP["FALL_LAND"], fall_ms=int(round(air * 1000)))

    def ledge(self, height: float) -> None:
        self.flags |= FALLING
        t = math.sqrt(2 * height / GRAVITY)
        self.t += int(t * 1000)
        self.z -= height
        self.flags &= ~FALLING
        self.send(OP["FALL_LAND"], fall_ms=int(round(t * 1000)))

    def swim(self, legs: int, speed: float) -> None:
        """A few swims forward at `speed` yd/s (the swim speed in force is 4.72)."""
        for _ in range(legs):
            self.flags = SWIMMING | FORWARD
            self.t += 50
            self.send(OP["START_FORWARD"])
            self.move(0.4, speed)
            self.flags = SWIMMING
            self.t += 20
            self.send(OP["STOP"])
        self.flags = 0

    def play(self, minutes: float) -> None:
        end = self.t + minutes * 60_000
        self.send(OP["STOP"])
        while self.t < end:
            self.press(OP["START_FORWARD"], FORWARD)
            self.move(float(self.rng.uniform(2, 4)), 7.0, turn=float(self.rng.choice([-1.5, 0.0, 1.0, 2.0])))
            self.jump_now(7.0)
            self.move(float(self.rng.uniform(1, 2)), 7.0)
            self.move(0.5, 7.0, rise=float(self.rng.uniform(0.6, 1.0)))    # a step up, within one heartbeat
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


def write_capture(root, bots_jump: float = JUMP, bots_heartbeat: int = 500, movers: int = 4, minutes: float = 2.0,
                  swim_bot_speed: float | None = None):
    """One hour of `movers` players and as many companions on map 1, each its own session, recorded alike (format
    3). The companions play the players' own scripts (the same seeds), with `bots_jump` and `bots_heartbeat`.
    With `swim_bot_speed`, one player and one companion swim a few legs (the companion at that speed): a small
    stratum."""
    d = w.hour_dir(root, "2026-10-05", 14)
    t0 = w.T0
    sessions = [w.header("session", t0)]
    moves = [w.header("move", t0)]
    companion = [w.header("companion", t0)]
    rng = np.random.default_rng(7)
    for i in range(2 * movers):
        bot = i >= movers
        player = 1000 + i
        sessions.append(w.record(1, {"ms": t0, "player": player, "session": 5000 + i, "class_": 1, "race": 1,
                                     "level": 70, "map": 1, "zone": 17, "kind": 1 if bot else 0}))
        moves.append(w.record(13, {"ms": t0, "player": player, "kind": 1 if bot else 0, "class_": 1, "race": 1,
                                   "level": 70, "map": 1, "zone": 17, "move_revision": 2 if bot else 0,
                                   "model": b"warrior_move1" if bot else b""}))
        moves.append(w.record(11, {"ms": t0, "player": player, "walk": 2.5, "run": 7.0, "run_back": 4.5,
                                   "swim": 4.722222, "swim_back": 2.5, "flight": 7.0, "flight_back": 4.5,
                                   "turn_rate": math.pi, "pitch_rate": math.pi}))
        client = Client(player, t0 + 100, np.random.default_rng(100 + i % movers),
                        jump=bots_jump if bot else JUMP, heartbeat_ms=bots_heartbeat if bot else 500,
                        source=r.SOURCE_CONTROLLER if bot else r.SOURCE_CLIENT)
        client.play(minutes)
        if swim_bot_speed is not None and i % movers == 0:
            client.swim(3, swim_bot_speed if bot else 4.722222)
        moves += client.records
        moves.append(w.record(14, {"ms": client.t + 100, "player": player, "kind": 1 if bot else 0,
                                   "sent": client.sent, "kept": client.sent}))
        if bot:
            ms = t0
            for _ in range(int(minutes * 60 * 4)):
                ms += int(rng.integers(245, 262))
                companion.append(w.record(50, {"ms": ms, "companion": player, "owner": 1, "model": "m"}))
    for k in range(4000):
        moves.append(w.record(15, {"ms": t0 + k * 30, "map": 1, "diff_ms": int(rng.integers(12, 45))}))
    files = {}
    for name, records in (("session-all", sessions), ("move-1", moves), ("companion-all", companion)):
        path = w.write_gz(d / f"{name}.bin.gz", [b"".join(records)])
        files[path.name] = {"records": len(records), "bytes": path.stat().st_size}
    w.write_index(d, "2026-10-05T14", files, players=2 * movers, sessions=2 * movers)
    return root


def study(root, resamples: int = RESAMPLES):
    return parity.report(parity.study_capture(r.CaptureDir(root)), source=str(root), resamples=resamples)


def rows(rep, section=None, context="all"):
    return {row["metric"]: row for row in rep["rows"]
            if row["context"] == context and (section is None or row["section"] == section)}


def test_identical_clients_are_at_parity(tmp_path):
    rep = study(write_capture(tmp_path))
    assert rep["clock"] == {"server": sum(rep["clock"].values())}         # every interval on the server's clock
    physics = rows(rep, "physics")
    for name in ("JUMP_SPEED", "jump apex", "GRAVITY", "HEARTBEAT_MS", "MOUSE_FACING_THRESHOLD"):
        assert physics[name]["flag"] == "pass", physics[name]
    # Each side against the constant: the generators keep the controller's constants.
    assert physics["JUMP_SPEED (players vs constant)"]["flag"] == "pass"
    assert physics["GRAVITY (bots vs constant)"]["flag"] == "pass"
    assert physics["STEP_UP (highest rise walked) (players vs constant)"]["flag"] == "pass"     # not above it
    assert physics["JUMP_SPEED"]["players"] == pytest.approx(JUMP, rel=1e-4)
    assert physics["HEARTBEAT_MS"]["bots"] == pytest.approx(500.0, abs=10.0)
    kin = rows(rep, "kinematics")
    for mode in ("run", "walk", "back", "strafe"):
        assert kin[f"{mode} speed / speed in force (median)"]["flag"] == "pass"
        assert kin[f"{mode} speed / speed in force (median)"]["players"] == pytest.approx(1.0, abs=0.02)
    assert kin["jumps a minute"]["flag"] == "pass" and kin["fall height (yd)"]["flag"] == "pass"
    cadence = rows(rep, "cadence")
    assert cadence["HEARTBEAT spacing while moving"]["flag"] == "pass"
    assert rows(rep, "refusals")["refused per 1000 packets"]["flag"] == "pass"
    assert rows(rep, "realism")["realism EMD"]["flag"] == "pass"
    assert not rep["attention"] and not rep["proposals"]
    assert all(s["survive_correction"] == 0 for s in rep["sections"].values())
    # n beside every row, the kinds stated, behaviour never gating.
    assert all("n" in row for row in rep["rows"] if row["section"] != "reported")
    assert "never gates" in rep["kinds"]["behaviour"]
    # MoverState strata: rows read in the warrior / zone 17 stratum, not only in `all`.
    assert any(row["context"].startswith("class=warrior") and "zone=17" in row["context"] for row in rep["rows"])
    # The realm's timing: ticks of 12-45 ms and decisions 245-262 ms -- no jitter by the stated rule.
    timing = rep["timing"]
    assert 12 <= timing["tick_ms"]["p50"] <= 45 and timing["jitter_recommended"] is False


def test_a_wrong_jump_speed_is_flagged_and_survives(tmp_path):
    rep = study(write_capture(tmp_path, bots_jump=9.0))
    physics = rows(rep, "physics")
    for name in ("JUMP_SPEED", "jump apex", "JUMP_SPEED (bots vs constant)"):
        assert physics[name]["flag"] == "attention" and physics[name]["survives"], physics[name]
        assert physics[name]["interval"][0] > physics[name]["threshold"]
    assert physics["JUMP_SPEED"]["bots"] == pytest.approx(9.0, rel=1e-4)
    assert physics["JUMP_SPEED (players vs constant)"]["flag"] == "pass"   # the players keep the constant
    assert not rep["proposals"]                                             # ... so no constant change is proposed
    assert physics["HEARTBEAT_MS"]["flag"] == "pass"
    assert any(row["metric"] == "JUMP_SPEED" for row in rep["attention"])


def test_a_fast_heartbeat_is_flagged(tmp_path):
    rep = study(write_capture(tmp_path, bots_heartbeat=250))
    cadence = rows(rep, "cadence")
    assert cadence["HEARTBEAT spacing while moving"]["flag"] == "attention"
    assert cadence["HEARTBEAT spacing while moving"]["survives"]
    assert rows(rep, "physics")["HEARTBEAT_MS"]["flag"] == "attention"
    assert rows(rep, "physics")["HEARTBEAT_MS"]["bots"] == pytest.approx(250.0, abs=10.0)
    assert rows(rep, "physics")["JUMP_SPEED"]["flag"] == "pass"


def test_a_small_stratum_is_inconclusive_not_attention(tmp_path):
    # One companion swims three legs at half the swim speed, one player at the right one: far apart, but too few.
    rep = study(write_capture(tmp_path, swim_bot_speed=2.4))
    swim = [row for row in rep["rows"] if row["metric"].startswith("swim speed")]
    assert swim and all(row["flag"].startswith("inconclusive") for row in swim), swim
    assert all(not row.get("survives") for row in swim)
    median = next(row for row in swim if row["context"] == "all" and "median" in row["metric"])
    assert median["bots"] == pytest.approx(2.4 / 4.722222, abs=0.03) and median["n"]["bots"][1] == 1


def test_the_emd_is_realism_score():
    rng = np.random.default_rng(3)
    t = np.arange(0, 40, motion.DECISION_SECONDS)

    def track(speed):
        s = np.zeros((len(t), motion.SAMPLE_DIM))
        s[:, motion.T] = t
        s[:, motion.X] = np.cumsum(rng.normal(speed, 0.5, len(t))) * motion.DECISION_SECONDS
        s[:, motion.YAW] = np.cumsum(rng.normal(0, 0.1, len(t)))
        s[:, motion.SPEED] = 7.0
        return s

    players, bots = track(7.0), track(5.0)
    pf, pc = motion.features(players), motion.step_contexts(players)
    bf, bc = motion.features(bots), motion.step_contexts(bots)
    reference = {"motion": {c: {"hist": h} for c, h in motion.histograms(pf, pc).items()}}
    expected = realism.score(bf, bc, reference)["realism_emd"]
    got = parity.emd(motion.histograms(pf, pc), motion.histograms(bf, bc), motion.HIST_FEATURES)
    assert got == pytest.approx(expected)


def test_benjamini_hochberg():
    assert parity.benjamini_hochberg([0.001, 0.2, 0.03, 0.04], fdr=0.1) == [True, False, True, True]
    assert parity.benjamini_hochberg([0.5, 0.6]) == [False, False]


def test_the_cli_writes_both_reports(tmp_path, capsys):
    root = write_capture(tmp_path / "cap", bots_jump=9.0, minutes=1.0, movers=3)
    out = tmp_path / "out" / "parity.md"
    assert cli.main(["parity", str(root), "--out", str(out)]) == 0        # the default 1000 resamples
    printed = json.loads(capsys.readouterr().out)
    assert any(line.endswith("all: JUMP_SPEED") for line in printed["attention"])
    text = (tmp_path / "out" / "parity.md").read_text()
    assert "| attention | yes | controller | warrior_move1 r2 | all | JUMP_SPEED |" in text
    assert "## Realm timing" in text and "expected by chance" in text
    saved = json.loads((tmp_path / "out" / "parity.json").read_text())
    assert saved["bootstrap"] == {"resamples": 1000, "confidence": 0.95, "by": "session", "fdr": parity.FDR}
