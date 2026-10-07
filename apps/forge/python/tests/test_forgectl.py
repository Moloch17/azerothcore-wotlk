"""forgectl's tests: ssh, docker and subprocess are replaced, so nothing touches a machine.

forgectl is stdlib-only and lives in apps/forge/forgectl; these tests run with the rest of the suite
(`forgectl test` runs them in the dev container)."""
from __future__ import annotations

import dataclasses
import os
import re
import stat
import sys
import textwrap
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from forgectl import (  # noqa: E402
    __main__ as cli, audit, cluster, config as config_module, confsync, console, deploy, logs, remote, stage, testcmd, ui,
    videos)
from forgectl.config import ConfigError  # noqa: E402
from forgectl.remote import Result  # noqa: E402

CLUSTER_TOML = Path(__file__).resolve().parents[2] / "cluster.toml"


@pytest.fixture
def cfg():
    return config_module.load(CLUSTER_TOML)


class FakeExec:
    """Stands in for remote.execute: answers by the first matching rule and records every call."""

    def __init__(self):
        self.rules = []
        self.calls = []

    def when(self, predicate, result):
        self.rules.append((predicate, result))
        return self

    def __call__(self, argv, input=None, timeout=30):
        self.calls.append((argv, input, timeout))
        for predicate, result in self.rules:
            if predicate(argv, input):
                return result(argv, input) if callable(result) else result
        return Result(0, "", "")


def on_target(name_or_address, needle=""):
    """A predicate: the ssh call went to this address and its script (stdin) contains `needle`."""
    return lambda argv, input: any(name_or_address in part for part in argv) and needle in (input or "")


@pytest.fixture
def fake(monkeypatch):
    executor = FakeExec()
    monkeypatch.setattr(remote, "execute", executor)
    return executor


@pytest.fixture(autouse=True)
def forgectl_home(tmp_path, monkeypatch):
    """The audit log and the console locks go to a temporary home, never the real ~/.forgectl; and a console that
    is not a test's own fake is never really attached."""
    home = tmp_path / "forgectl-home"
    monkeypatch.setenv("FORGECTL_HOME", str(home))
    real_spawn = console.spawn

    def guarded(argv):
        assert argv[0] not in ("ssh", "docker"), f"a test tried to attach a real console: {argv}"
        return real_spawn(argv)
    monkeypatch.setattr(console, "spawn", guarded)
    return home


# ---- config ----------------------------------------------------------------------------------------------------------

def test_cluster_toml_describes_the_documented_cluster(cfg):
    assert cfg.host.name == "sarah" and cfg.host.address == "192.168.0.68"
    assert [m.name for m in cfg.workers] == ["spencer", "thomas", "moloch"]
    assert cfg.machine("moloch").address == "192.168.0.117" and cfg.machine("moloch").user == "moloch"
    assert not cfg.machine("eli").in_cluster and not cfg.machine("dev").in_cluster
    assert [m.name for m in cfg.cluster][0] == "sarah"
    assert (cfg.control_port, cfg.data_port, cfg.weights_port) == (7700, 7701, 7702)
    assert (cfg.lan_remote, cfg.branch, cfg.worldserver) == ("lan", "forge", "ac-animus-forge-worldserver")
    assert cfg.dev["container"] == "claude-syntax"
    assert cfg.path_of(cfg.host, "conf") == "~/animus-forge/env/dist/etc/modules/mod_animus_forge.conf"


def test_unknown_machine_names_the_known_ones(cfg):
    with pytest.raises(ConfigError, match="no machine named 'zed'.*sarah"):
        cfg.machine("zed")


def write_toml(tmp_path, text):
    path = tmp_path / "cluster.toml"
    path.write_text(textwrap.dedent(text))
    return path


def test_missing_file_is_a_clear_error(tmp_path):
    with pytest.raises(ConfigError, match="not found"):
        config_module.load(tmp_path / "nope.toml")


def test_invalid_toml_is_a_clear_error(tmp_path):
    with pytest.raises(ConfigError, match="not valid TOML"):
        config_module.load(write_toml(tmp_path, "[cluster\nhost = 1"))


def mutated(tmp_path, old, new):
    path = write_toml(tmp_path, CLUSTER_TOML.read_text().replace(old, new, 1))
    return config_module.load(path)


def test_bad_role_duplicate_names_and_host_outside_the_cluster_are_refused(tmp_path):
    with pytest.raises(ConfigError, match="role must be one of"):
        mutated(tmp_path, 'role = "worker"', 'role = "boss"')
    with pytest.raises(ConfigError, match="unique"):
        mutated(tmp_path, 'name = "spencer"', 'name = "thomas"')
    with pytest.raises(ConfigError, match="host 'sarah' must have in_cluster"):
        mutated(tmp_path, 'path = "~/animus-forge"\nin_cluster = true', 'path = "~/animus-forge"\nin_cluster = false')
    with pytest.raises(ConfigError, match="missing key 'user'"):
        mutated(tmp_path, 'user = "sarah"\n', "")
    with pytest.raises(ConfigError, match="control_port"):
        mutated(tmp_path, "control_port = 7700", 'control_port = "x"')


def test_env_var_selects_the_file(tmp_path, monkeypatch):
    monkeypatch.setenv("FORGECTL_CONFIG", str(CLUSTER_TOML))
    assert config_module.load().host_name == "sarah"


# ---- remote ----------------------------------------------------------------------------------------------------------

def test_execute_kills_a_command_that_outlives_its_timeout():
    started = time.monotonic()
    result = remote.execute(["sleep", "30"], timeout=0.3)
    assert result.timed_out and result.unreachable and not result.ok
    assert time.monotonic() - started < 5


def test_execute_reports_a_missing_program_without_raising():
    assert remote.execute(["definitely-not-a-program-forgectl"]).rc == 127


def test_ssh_is_key_only_with_a_connect_timeout(cfg):
    argv = remote.ssh_argv(cfg.host, ["bash", "-s"], connect=4)
    assert argv[0] == "ssh" and "BatchMode=yes" in argv and "ConnectTimeout=4" in argv
    assert argv[-3:] == ["sarah@192.168.0.68", "bash", "-s"]
    assert "-tt" in remote.ssh_argv(cfg.host, ["x"], tty=True)


def test_on_sends_the_script_on_stdin_and_runs_locally_for_a_local_machine(cfg, fake):
    remote.on(cfg.host, "echo hi\n")
    remote.on(cfg.machine("dev"), "echo hi\n")
    (ssh_argv, ssh_input, _), (local_argv, local_input, _) = fake.calls
    assert ssh_argv[0] == "ssh" and ssh_input == "echo hi\n"
    assert local_argv == ["bash", "-s"] and local_input == "echo hi\n"


def test_sh_path_expands_only_a_leading_tilde():
    assert remote.sh_path("~/animus-forge") == '"$HOME"/animus-forge'
    assert remote.sh_path("/srv/my dir") == "'/srv/my dir'"


def test_result_reasons():
    assert Result(124, timed_out=True).reason() == "timed out"
    assert "Permission denied" in Result(255, "", "x\nPermission denied (publickey).").reason()
    assert Result(3, "", "boom").reason() == "exit 3: boom"


def test_parallel_map_keeps_order_and_runs_concurrently():
    started = time.monotonic()
    out = remote.parallel_map(lambda n: (time.sleep(0.3), n)[1], [3, 1, 2])
    assert out == [3, 1, 2] and time.monotonic() - started < 0.8


# ---- cluster table ---------------------------------------------------------------------------------------------------

def probe_text(rev="abc1234", ws="Up 2 hours", u1=("3688", "169887744", "2425"), u2=("3689", "169902080", "2422"),
               age="3", gpu="3390/8192 MiB"):
    def line(u):
        return f"update {u[0]} | steps {u[1]} | {u[2]} sps | rollout 3.38s compute 4.81s" if u else ""
    return (f"rev={rev}\nws={ws}\nu1={line(u1)}\nu2={line(u2)}\nage={age}\nload=4.01 cpus=24\ndisk_kb=230715312\n"
            + (f"gpu={gpu}\n" if gpu else ""))


def test_probe_parsing_stepping_stalled_and_missing_log(cfg):
    m = cfg.host
    stepping = cluster.parse_probe(m, probe_text())
    assert (stepping.rev, stepping.learner.split()[0], stepping.step, stepping.sps) == (
        "abc1234", "stepping", 169902080, 2422.0)
    assert stepping.load == "4.01/24" and stepping.disk == "220 GB" and stepping.gpu == "3390/8192 MiB"
    # no advance in the sample but the log was written seconds ago: updates are further apart than the sample
    same = ("3689", "169902080", "2422")
    assert cluster.parse_probe(m, probe_text(u1=same, u2=same, age="6")).learner.startswith("stepping")
    assert cluster.parse_probe(m, probe_text(u1=same, u2=same, age="900")).learner.startswith("STALLED")
    assert cluster.parse_probe(m, probe_text(u1=None, u2=None)).learner == "no log"
    assert cluster.parse_probe(m, probe_text(ws="", gpu="")).worldserver == ""
    assert cluster.parse_probe(m, "nopath=1\n").problem.startswith("no checkout")


def test_update_line_is_anchored_so_a_traceback_is_not_an_update():
    assert cluster.parse_update("update 3 | steps 10 | 5 sps | rollout") == (3, 10, 5.0)
    assert cluster.parse_update("    stats = pending.result()  # an update that raised re-raises") is None


def test_unreachable_machine_shows_as_unreachable_not_a_hang(cfg, fake):
    fake.when(on_target("192.168.0.66"), Result(124, timed_out=True))
    fake.when(on_target("192.168.0.67"), Result(255, "", "ssh: connect to host: No route to host"))
    fake.when(lambda argv, input: True, Result(0, probe_text(rev="aaa1111")))
    status = cluster.probe(cfg, cfg.machine("spencer"))
    assert not status.reachable and status.problem == "timed out"
    assert "No route" in cluster.probe(cfg, cfg.machine("thomas")).problem
    assert cluster.probe(cfg, cfg.host).reachable
    # the ssh call carries a hard timeout longer than the sample, and a connect timeout
    assert all(timeout > cluster.SAMPLE_SECONDS for _, _, timeout in fake.calls)


def test_table_marks_a_revision_that_differs_and_down_containers(cfg):
    statuses = [cluster.parse_probe(cfg.host, probe_text(rev="aaa1111")),
                cluster.parse_probe(cfg.machine("spencer"), probe_text(rev="bbb2222", ws="")),
                cluster.MachineStatus(cfg.machine("thomas"), reachable=False, problem="timed out")]
    text = cluster.render(cfg, statuses)
    assert "bbb2222*" in text and "aaa1111*" not in text and "DOWN" in text and "UNREACHABLE: timed out" in text
    assert "differs from the host" in text


def test_refused_workers_are_read_from_the_host_and_stripped(cfg, fake):
    fake.when(on_target("192.168.0.68", "refused the worker"),
              Result(0, "Cluster: refused the worker at 192.168.0.66: fingerprint differs\n\n"))
    assert cluster.refused_lines(cfg) == ["Cluster: refused the worker at 192.168.0.66: fingerprint differs"]


def test_cluster_run_prints_the_table_and_refusals_and_fails_on_an_unreachable_member(cfg, fake, capsys):
    fake.when(on_target("192.168.0.66"), Result(255, "", "No route to host"))
    fake.when(on_target("192.168.0.68", "refused the worker"), Result(0, "Cluster: refused the worker at x\n"))
    fake.when(lambda argv, input: "rev=" not in "" and "git rev-parse" in (input or ""), Result(0, probe_text()))
    code = cluster.run(cfg)
    out = capsys.readouterr().out
    assert code == 1 and "UNREACHABLE" in out and "refused the worker at x" in out and "sarah" in out
    assert "eli" not in out  # out of the cluster: only with --all


# ---- console ---------------------------------------------------------------------------------------------------------

FAKE_CONSOLE = r'''
import os, sys, termios, time, tty
marker = sys.argv[1]
mode = sys.argv[2]
tty.setraw(sys.stdin.fileno())
out = sys.stdout
def w(s):
    out.write(s); out.flush()
open(marker + ".up", "w").write("up")
w("\x1b[0m\x1b[36mPress refused: class 1 noise before we typed\r\n")
buf = b""
eof = False
while True:
    data = os.read(0, 1024)
    if not data:
        eof = True
        break
    buf += data
    if b"\x10\x11" in buf:
        open(marker, "w").write("detached eof=%s" % eof)
        sys.exit(0)
    while b"\n" in buf:
        line, _, buf = buf.partition(b"\n")
        line = line.decode()
        if mode == "hang":
            open(marker + ".typed", "w").write(line)
        if mode in ("silent", "hang"):
            continue
        w(line + "\r\n")
        w("\x1b[?2004l\r\x1b[?2004h")
        w("\x1b[0m\x1b[33mWarning: log noise in the middle of the reply\r\n")
        w("AC> Forge: move2_seek | training\r\r\n  Metric  Value\r\r\n")
        w("\x1b[0m\x1b[36mPress refused: more noise\r\n")
        w("  learner  connected\r\r\n")
        w("\x1b[?2004hAC> ")
open(marker, "w").write("eof=%s" % eof)
'''


@pytest.fixture
def fake_console(tmp_path, monkeypatch):
    script = tmp_path / "fake_console.py"
    script.write_text(FAKE_CONSOLE)
    marker = tmp_path / "marker"

    def use(mode):
        monkeypatch.setattr(console, "attach_argv",
                            lambda config, machine: [sys.executable, str(script), str(marker), mode])
        return marker
    return use


def test_a_line_typed_into_the_console_returns_its_reply_without_log_noise_then_detaches(cfg, fake_console):
    marker = fake_console("echo")
    result = console.send(cfg, cfg.host, "forge status", timeout=10, settle=0.5)
    assert result.ok and result.started
    assert result.lines == ["Forge: move2_seek | training", "  Metric  Value", "  learner  connected"]
    deadline = time.time() + 5
    while not marker.exists() and time.time() < deadline:
        time.sleep(0.05)
    assert marker.read_text() == "detached eof=False"  # Ctrl-P Ctrl-Q arrived, and stdin was never closed first


def test_a_console_that_never_answers_times_out_and_still_detaches(cfg, fake_console):
    marker = fake_console("silent")
    started = time.monotonic()
    result = console.send(cfg, cfg.host, "forge status", timeout=1.5, settle=0.3)
    assert not result.ok and result.lines == []
    assert time.monotonic() - started < 10
    deadline = time.time() + 5
    while not marker.exists() and time.time() < deadline:
        time.sleep(0.05)
    assert marker.read_text() == "detached eof=False"


LIVE_SAMPLE = ("forge status\r\n\x1b[?2004l\r\x1b[?2004hAC> Forge: move2_seek | training | update 3,752\r\r\n"
               "  Metric   Value  Note\r\r\n\x1b[0m\x1b[36mPress refused: class 6\r\n  learner  connected\r\r\n"
               "\x1b[?2004hAC> ")


def test_parse_reply_on_a_capture_of_the_real_console():
    result = console.parse_reply(LIVE_SAMPLE, "forge status")
    assert result.prompt_seen and result.started
    assert result.lines == ["Forge: move2_seek | training | update 3,752", "  Metric   Value  Note",
                            "  learner  connected"]


def test_parse_reply_for_a_command_with_no_output_and_for_a_cut_reply():
    assert console.parse_reply("forge pause\r\n\x1b[?2004hAC> ", "forge pause").lines == []
    cut = console.parse_reply("forge status\r\nAC> Forge: x\r\r\n  a  b", "forge status")
    assert not cut.prompt_seen and cut.lines == ["Forge: x", "  a  b"]


def test_attach_command_is_safe_and_goes_over_ssh_for_a_remote_machine(cfg):
    local = console.attach_argv(cfg, cfg.machine("dev"))
    assert local[:2] == ["docker", "attach"] and "--sig-proxy=false" in local
    assert "--detach-keys=ctrl-p,ctrl-q" in local and local[-1] == "ac-animus-forge-worldserver"
    far = console.attach_argv(cfg, cfg.host)
    assert far[0] == "ssh" and "-tt" in far and "BatchMode=yes" in far and "--sig-proxy=false" in far


# ---- stage commands --------------------------------------------------------------------------------------------------

class FakeConsole:
    def __init__(self, replies=None, fail_on=()):
        self.sent = []
        self.replies = replies or {}
        self.fail_on = fail_on

    def __call__(self, config, machine, line, timeout=20, settle=1.5):
        self.sent.append((machine.name, line))
        if machine.name in self.fail_on:
            return console.ConsoleResult([], False, False)
        lines = self.replies.get(line, ["ok"])
        return console.ConsoleResult(lines, True, True)


@pytest.fixture
def sent(monkeypatch):
    recorder = FakeConsole()
    monkeypatch.setattr(console, "send", recorder)
    monkeypatch.setattr(stage, "existing_run", lambda config, name: stage.ExistingRun(name, False))
    return recorder


def test_stage_start_goes_to_the_host_only_after_confirmation(cfg, sent, monkeypatch, capsys):
    monkeypatch.setattr(ui, "ask", lambda prompt: "y")
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    assert stage.run(cfg, "start", ["move2_seek"], yes=False) == 0
    assert sent.sent == [("sarah", "forge start move2_seek")]
    assert "This will:" in capsys.readouterr().out


def test_declining_sends_nothing(cfg, sent, monkeypatch):
    monkeypatch.setattr(ui, "ask", lambda prompt: "n")
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    assert stage.run(cfg, "cancel", [], yes=False) == 1
    assert sent.sent == []


def test_without_a_terminal_and_without_yes_nothing_is_sent(cfg, sent, monkeypatch):
    monkeypatch.setattr(sys.stdin, "isatty", lambda: False)
    assert stage.run(cfg, "resume", ["move2_seek"], yes=False) == 1 and sent.sent == []


def test_pause_and_cancel_reach_the_host_and_every_worker(cfg, sent):
    for action in ("pause", "cancel"):
        sent.sent.clear()
        assert stage.run(cfg, action, [], yes=True) == 0
        assert sent.sent == [("sarah", f"forge {action}"), ("spencer", f"forge {action}"),
                             ("thomas", f"forge {action}"), ("moloch", f"forge {action}")]


def test_resume_and_start_do_not_touch_the_workers(cfg, sent):
    stage.run(cfg, "resume", ["move2_seek"], yes=True)
    assert [name for name, _ in sent.sent] == ["sarah"]


def test_a_worker_that_does_not_answer_is_reported_and_the_exit_is_non_zero(cfg, monkeypatch, capsys):
    recorder = FakeConsole(fail_on=("thomas",))
    monkeypatch.setattr(console, "send", recorder)
    assert stage.run(cfg, "pause", [], yes=True) == 1
    out = capsys.readouterr().out
    assert "FAILED thomas" in out and "thomas" in out.split("Not sent or not answered on:")[1]
    assert ("moloch", "forge pause") in recorder.sent  # the others still got it


def test_a_host_that_does_not_answer_stops_before_the_workers(cfg, monkeypatch):
    recorder = FakeConsole(fail_on=("sarah",))
    monkeypatch.setattr(console, "send", recorder)
    assert stage.run(cfg, "cancel", [], yes=True) == 1
    assert recorder.sent == [("sarah", "forge cancel")]


def test_stage_names_cannot_smuggle_console_commands(cfg, sent):
    with pytest.raises(ui.Failure, match="not a stage name"):
        stage.run(cfg, "start", ["move2_seek; server shutdown 1"], yes=True)
    assert sent.sent == []


def test_start_needs_a_stage_and_pause_checks_a_named_stage_is_the_running_one(cfg, monkeypatch):
    recorder = FakeConsole(replies={"forge status": ["Forge: move2_seek | training"]})
    monkeypatch.setattr(console, "send", recorder)
    with pytest.raises(ui.Failure, match="needs a stage name"):
        stage.run(cfg, "start", [], yes=True)
    with pytest.raises(ui.Failure, match="does not mention other_stage"):
        stage.run(cfg, "pause", ["other_stage"], yes=True)
    assert stage.run(cfg, "pause", ["move2_seek"], yes=True) == 0


def test_learner_line_parsing_and_headline():
    line = ("update 3688 | steps 169887744 | 2425 sps | rollout 3.38s compute 4.81s | episode_damage 0, "
            "episode_score_outcome 1.143, value_loss 0.139, entropy 0.9564")
    data = stage.parse_learner_line(line)
    assert (data["update"], data["steps"], data["sps"]) == (3688, 169887744, 2425.0)
    assert data["entropy"] == "0.9564" and data["value_loss"] == "0.139" and data["timing"].startswith("rollout")
    head = stage.headline_from_line("sarah", line)
    assert "update 3,688" in head and "2,425 steps/s" in head and "entropy 0.9564" in head
    assert "episode_damage" not in head


def test_status_prints_the_console_table_and_the_learner_line(cfg, sent, fake, capsys):
    sent.replies["forge status"] = ["Forge: move2_seek | training", "  learner connected"]
    update = "update 5 | steps 10 | 7 sps | rollout 1s | entropy 0.5\n"
    fake.when(on_target("192.168.0.68", "update"), Result(0, update))
    assert stage.status(cfg) == 0
    out = capsys.readouterr().out
    assert "Forge: move2_seek" in out and "learner (sarah): update 5" in out and "entropy 0.5" in out


# ---- logs ------------------------------------------------------------------------------------------------------------

def test_problems_keep_tracebacks_and_drop_the_press_refused_chatter():
    lines = ["Press refused: class 9 level 1 spell 688", "Warning: Non-finite values from the learner: x",
             "all fine", "Traceback (most recent call last):", '  File "a.py", line 3, in f', "    boom()",
             "ValueError: no", "Error: disk nearly full"]
    found = logs.problems(lines)
    assert "Warning: Non-finite values from the learner: x" in found and "Error: disk nearly full" in found
    assert '  File "a.py", line 3, in f' in found and "    boom()" in found
    assert all("Press refused" not in line and line != "all fine" for line in found)


def test_logs_command_puts_problems_before_the_recent_lines(cfg, fake, capsys):
    out = ("##WORLDSERVER\nstarting\nWarning: Convergence is slow\n##ERRORS\n##LEARNER\nTraceback (most recent call "
           "last):\n  File \"x\", line 1\nKeyError: 3\n##LASTUPDATE\nupdate 7 | steps 70 | 3 sps | rollout 1s\n")
    fake.when(lambda argv, input: True, Result(0, out))
    assert logs.run(cfg, "thomas", errors_only=False, lines=10, wide=False) == 0
    text = capsys.readouterr().out
    recent = text.index("== worldserver (docker logs): the last")
    assert text.index("PROBLEMS") < text.index("Convergence is slow") < recent
    assert "KeyError: 3" in text and "learner (thomas): update 7" in text
    capsys.readouterr()
    logs.run(cfg, None, errors_only=True, lines=10, wide=False)
    assert "== worldserver (docker logs): the last" not in capsys.readouterr().out


def test_logs_of_an_unreachable_machine_is_an_error(cfg, fake):
    fake.when(lambda argv, input: True, Result(124, timed_out=True))
    with pytest.raises(ui.Failure, match="timed out"):
        logs.run(cfg, "thomas", False, 10, False)


# ---- conf-sync -------------------------------------------------------------------------------------------------------

HOST_CONF = ('# comment\nAnimusForge.Envs = 64\nAnimusForge.Cluster.Role = "host"\n'
             "AnimusForge.Curriculum.A = 1\nAnimusForge.Curriculum.B = 2\nAnimusForge.Curriculum.C = 3\n")
WORKER_CONF = ('# worker\nAnimusForge.Envs = 32\nAnimusForge.Cluster.Role = "worker"\n'
               "AnimusForge.Curriculum.A = 1\nAnimusForge.Curriculum.B = 99\nAnimusForge.Curriculum.OLD = 5\n"
               "# AnimusForge.Curriculum.C = commented out\n")


def test_curriculum_keys_ignore_comments_and_other_keys():
    assert confsync.curriculum_keys(WORKER_CONF) == {
        "AnimusForge.Curriculum.A": "1", "AnimusForge.Curriculum.B": "99", "AnimusForge.Curriculum.OLD": "5"}


def test_compare_finds_missing_extra_and_different():
    diff = confsync.compare(confsync.curriculum_keys(HOST_CONF), confsync.curriculum_keys(WORKER_CONF))
    assert (diff.missing, diff.extra, diff.different) == (
        ["AnimusForge.Curriculum.C"], ["AnimusForge.Curriculum.OLD"], ["AnimusForge.Curriculum.B"])
    assert not diff.same and diff.summary() == "1 missing, 1 extra, 1 different"


def test_synced_text_matches_the_host_and_leaves_the_rest_alone():
    new = confsync.synced_text(HOST_CONF, WORKER_CONF, "20261007-1")
    assert confsync.curriculum_keys(new) == confsync.curriculum_keys(HOST_CONF)
    assert "AnimusForge.Envs = 32" in new and 'AnimusForge.Cluster.Role = "worker"' in new and "# worker" in new
    assert "OLD" not in new and new.endswith("\n")
    assert confsync.compare(confsync.curriculum_keys(HOST_CONF), confsync.curriculum_keys(new)).same
    assert confsync.synced_text(HOST_CONF, new, "x") == new  # idempotent apart from nothing to add


def test_set_cluster_role_replaces_and_appends():
    text = confsync.set_cluster_role('AnimusForge.Cluster.Role = "host"\nAnimusForge.Cluster.Host = ""\nX = 1\n',
                                     "worker", "192.168.0.67:7700")
    assert 'AnimusForge.Cluster.Role = "worker"' in text and 'AnimusForge.Cluster.Host = "192.168.0.67:7700"' in text
    assert text.count("AnimusForge.Cluster.Role") == 1 and "X = 1" in text
    appended = confsync.set_cluster_role("X = 1\n", "host", "")
    assert 'AnimusForge.Cluster.Role = "host"' in appended and 'AnimusForge.Cluster.Host = ""' in appended


class ConfStore:
    """Fake conf files on the machines behind remote.execute: `cat` reads, a `cat >` script writes."""

    def __init__(self, fake, texts):
        self.texts, self.writes, self.backups = dict(texts), [], []
        fake.when(lambda argv, input: True, self.handle)

    def handle(self, argv, input):
        address = next(part.split("@")[1] for part in argv if "@" in part)
        script = input or ""
        if script.startswith("cat "):
            return Result(0, self.texts[address])
        if "cat >" in script:
            body = script.split("<<'FORGECTL_CONF_EOF'\n")[1].rsplit("FORGECTL_CONF_EOF", 1)[0]
            self.backups.append((address, [l for l in script.splitlines() if l.startswith("cp -p")][0]))
            self.writes.append(address)
            self.texts[address] = body
            return Result(0, "FORGECTL_WRITE=mv\n")
        return Result(0)


def conf_texts(cfg, worker_text=WORKER_CONF):
    return {cfg.host.address: HOST_CONF, "192.168.0.66": worker_text, "192.168.0.67": HOST_CONF,
            "192.168.0.117": HOST_CONF}


def test_check_reports_differences_and_never_writes(cfg, fake, capsys):
    store = ConfStore(fake, conf_texts(cfg))
    assert confsync.run(cfg, check_only=True, yes=True) == 1
    out = capsys.readouterr().out
    assert "spencer" in out and "1 missing, 1 extra, 1 different" in out and "thomas" in out
    assert store.writes == []


def test_check_passes_when_everything_matches(cfg, fake, capsys):
    ConfStore(fake, conf_texts(cfg, HOST_CONF))
    assert confsync.run(cfg, check_only=True, yes=False) == 0
    assert "All workers match" in capsys.readouterr().out


def test_sync_backs_up_writes_and_verifies(cfg, fake, capsys):
    store = ConfStore(fake, conf_texts(cfg))
    assert confsync.run(cfg, check_only=False, yes=True) == 0
    assert store.writes == ["192.168.0.66"]  # only the worker that differed
    address, copy = store.backups[0]
    assert ".bak-" in copy and "mod_animus_forge.conf" in copy
    assert confsync.compare(confsync.curriculum_keys(HOST_CONF), confsync.curriculum_keys(store.texts[address])).same
    assert "spencer: synced" in capsys.readouterr().out


def test_sync_declined_writes_nothing(cfg, fake, monkeypatch):
    store = ConfStore(fake, conf_texts(cfg))
    monkeypatch.setattr(sys.stdin, "isatty", lambda: False)
    assert confsync.run(cfg, check_only=False, yes=False) == 1 and store.writes == []


def test_sync_fails_when_the_verification_still_differs(cfg, fake, capsys):
    store = ConfStore(fake, conf_texts(cfg))
    original = store.handle

    def lossy(argv, input):  # the write "succeeds" but the file does not change
        if "cat >" in (input or ""):
            return Result(0, "FORGECTL_WRITE=mv\n")
        return original(argv, input)
    fake.rules.insert(0, (lambda argv, input: True, lossy))
    assert confsync.run(cfg, check_only=False, yes=True) == 1
    assert "still differs" in capsys.readouterr().out


def test_an_unreadable_worker_is_reported(cfg, fake, capsys):
    ConfStore(fake, conf_texts(cfg))
    fake.rules.insert(0, (on_target("192.168.0.67"), Result(255, "", "No route to host")))
    assert confsync.run(cfg, check_only=True, yes=True) == 1
    assert "UNREADABLE" in capsys.readouterr().out


# ---- build -----------------------------------------------------------------------------------------------------------

SHA = "0123456789abcdef0123456789abcdef01234567"


class FakeCluster:
    """The machines' side of a deploy: pull scripts answer with T0, the ready poll succeeds after `ready_after`
    polls."""

    def __init__(self, fake, ready_after=None, pull_fail=(), unreachable=()):
        self.polls, self.pulled = {}, []
        self.ready_after = ready_after or {}
        self.pull_fail, self.unreachable = pull_fail, unreachable
        fake.when(lambda argv, input: True, self.handle)

    def handle(self, argv, input):
        address = next((p.split("@")[1] for p in argv if "@" in p), "local")
        if address in self.unreachable:
            return Result(255, "", "ssh: No route to host")
        script = input or ""
        if "cluster-pull.sh" in script or "force-recreate" in script:
            self.pulled.append(address)
            if address in self.pull_fail:
                return Result(1, "T0=2026-10-07T10:00:00Z\nfatal: not possible to fast-forward\n")
            return Result(0, "T0=2026-10-07T10:00:00Z\nDone.\n")
        if "docker logs --since" in script and "ready" in script:
            self.polls[address] = self.polls.get(address, 0) + 1
            after = self.ready_after.get(address, 1)
            if after is not None and self.polls[address] >= after:
                return Result(0, f"AzerothCore rev. {SHA[:12]} (forge branch) (Animus Forge) ready...\n")
            return Result(0, "")
        return Result(0, "")


@pytest.fixture
def deployable(cfg, fake, monkeypatch):
    git_calls = []
    monkeypatch.setattr(deploy, "sleep", lambda s: None)

    def local_git(config, *args):
        git_calls.append(args)
        return {"rev-parse --abbrev-ref HEAD": "forge", "rev-parse HEAD": SHA}.get(" ".join(args), "")
    monkeypatch.setattr(deploy, "local_git", local_git)
    monkeypatch.setattr(console, "send", FakeConsole(replies={"forge status": ["Animus Forge is idle"]}))
    return git_calls


def test_cluster_build_pushes_pulls_everywhere_and_reports_each_machine(cfg, fake, deployable, capsys):
    cluster_fake = FakeCluster(fake, ready_after={"192.168.0.66": 3})
    assert deploy.build(cfg, cluster=True, yes=True, timeout_minutes=5) == 0
    assert ("push", "lan", "forge") in deployable
    assert sorted(cluster_fake.pulled) == ["192.168.0.117", "192.168.0.66", "192.168.0.67", "192.168.0.68"]
    assert cluster_fake.polls["192.168.0.66"] == 3  # waited for the slow one
    out = capsys.readouterr().out
    assert out.count("ready") >= 4 and "All 4 machines are ready" in out
    assert "every worldserver restarts: no stage is running now" in out


def test_a_machine_that_never_gets_ready_is_reported_and_fails_the_build(cfg, fake, deployable, monkeypatch, capsys):
    FakeCluster(fake, ready_after={"192.168.0.67": None})
    now = [0.0]
    monkeypatch.setattr(deploy, "clock", lambda: now[0])
    monkeypatch.setattr(deploy, "sleep", lambda s: now.__setitem__(0, now[0] + s))
    assert deploy.build(cfg, cluster=True, yes=True, timeout_minutes=2) == 1
    out = capsys.readouterr().out
    assert "TIMED OUT" in out and "thomas" in out and "NOT READY" in out and "Nothing was skipped silently" in out


def test_pull_failure_and_unreachable_machines_are_reported_not_skipped(cfg, fake, deployable, capsys):
    FakeCluster(fake, pull_fail=("192.168.0.66",), unreachable=("192.168.0.117",))
    assert deploy.build(cfg, cluster=True, yes=True, timeout_minutes=1) == 1
    out = capsys.readouterr().out
    assert "PULL FAILED" in out and "fast-forward" in out and "UNREACHABLE" in out
    assert "NOT READY on 012345678: spencer, moloch" in out


def test_build_asks_first_and_declining_pushes_nothing(cfg, fake, deployable, monkeypatch):
    cluster_fake = FakeCluster(fake)
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    monkeypatch.setattr(ui, "ask", lambda prompt: "no")
    assert deploy.build(cfg, cluster=True, yes=False, timeout_minutes=1) == 1
    assert cluster_fake.pulled == [] and not any(c[0] == "push" for c in deployable)


def test_cluster_build_refuses_to_push_the_wrong_branch(cfg, fake, monkeypatch):
    monkeypatch.setattr(deploy, "local_git", lambda config, *a: "wip" if "--abbrev-ref" in a else SHA)
    with pytest.raises(ui.Failure, match="check out forge"):
        deploy.build(cfg, cluster=True, yes=True, timeout_minutes=1)


def test_local_build_recreates_the_container_here_only(cfg, fake, deployable, capsys):
    cluster_fake = FakeCluster(fake)
    assert deploy.build(cfg, cluster=False, yes=True, timeout_minutes=1) == 0
    assert cluster_fake.pulled == ["local"] and not any(c[0] == "push" for c in deployable)


def test_ready_pattern_matches_the_real_log_line():
    import re
    line = "AzerothCore rev. 64b7c7dc564c+ 2026-10-07 04:58:14 -0600 (forge branch) (Unix) (Animus Forge) ready..."
    assert re.search(deploy.READY_RE.format(sha="64b7c7dc5"), line)
    assert not re.search(deploy.READY_RE.format(sha="0123456789"), line)


# ---- move-host -------------------------------------------------------------------------------------------------------

@pytest.fixture
def moves(cfg, monkeypatch, tmp_path):
    steps = []
    toml = tmp_path / "cluster.toml"
    toml.write_text(CLUSTER_TOML.read_text())
    moved_cfg = dataclasses.replace(cfg, file=toml)
    monkeypatch.setattr(deploy, "deploy_state", lambda config: ("forge", SHA))
    monkeypatch.setattr(deploy.stage_commands, "send_checked", lambda config, machine, line, timeout=25:
                        console.ConsoleResult(["Forge: move2_seek | training"], True, True))
    monkeypatch.setattr(deploy.stage_commands, "run", lambda config, action, stages, yes:
                        steps.append(("stage", config.host_name, action, tuple(stages))) or 0)
    monkeypatch.setattr(deploy, "wait_for_log", lambda config, machine, pattern, seconds, since=None:
                        steps.append(("wait", machine.name, pattern)) or "ok line")
    monkeypatch.setattr(deploy, "copy_run", lambda config, old, new, stage:
                        steps.append(("copy", old.name, new.name, stage)))
    monkeypatch.setattr(deploy.confsync, "rewrite",
                        lambda config, machine, transform, stamp: steps.append(("conf", machine.name, transform("")))
                        or "backup")
    monkeypatch.setattr(deploy, "build", lambda config, **kw: steps.append(("build", kw["push"])) or 0)
    monkeypatch.setattr(deploy, "wait_plan_ended", lambda config, machine: steps.append(("ended", machine.name)))
    monkeypatch.setattr(deploy.remote, "on", lambda machine, script, timeout=30, connect=6: Result(0, ""))
    return moved_cfg, steps, toml


def test_move_host_plan_is_printed_and_nothing_runs_when_declined(moves, monkeypatch, capsys):
    cfg, steps, toml = moves
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    monkeypatch.setattr(ui, "ask", lambda prompt: "n")
    assert deploy.move_host(cfg, "thomas", "move2_seek", yes=False, timeout_minutes=5) == 1
    out = capsys.readouterr().out
    assert "cancel the plan on sarah" in out and "copy runs/move2_seek" in out and "192.168.0.67:7700" in out
    assert steps == [] and 'host = "sarah"' in toml.read_text()


def test_move_host_runs_the_recipe_in_order(moves, capsys):
    cfg, steps, toml = moves
    assert deploy.move_host(cfg, "thomas", "move2_seek", yes=True, timeout_minutes=5) == 0
    kinds = [s[0] for s in steps]
    assert kinds[:3] == ["stage", "ended", "copy"]
    assert steps[0] == ("stage", "sarah", "cancel", ())
    assert steps[2] == ("copy", "sarah", "thomas", "move2_seek")
    confs = {s[1]: s[2] for s in steps if s[0] == "conf"}
    assert sorted(confs) == ["moloch", "sarah", "spencer", "thomas"]
    assert 'Role = "host"' in confs["thomas"] and 'Host = ""' in confs["thomas"]
    assert 'Role = "worker"' in confs["sarah"] and 'Host = "192.168.0.67:7700"' in confs["sarah"]
    assert kinds.index("build") > kinds.index("conf") and ("build", False) in steps  # no push
    assert steps[-2] == ("stage", "thomas", "resume", ("move2_seek",))  # on the NEW host
    assert steps[-1] == ("wait", "thomas", "worker learners join")
    assert 'host = "thomas"' in toml.read_text()


def test_move_host_refuses_bad_targets(moves):
    cfg, steps, _ = moves
    with pytest.raises(ui.Failure, match="already the host"):
        deploy.move_host(cfg, "sarah", None, True, 5)
    with pytest.raises(ui.Failure, match="not in the cluster"):
        deploy.move_host(cfg, "eli", None, True, 5)
    with pytest.raises(ConfigError):
        deploy.move_host(cfg, "nobody", None, True, 5)
    assert steps == []


def test_move_host_stops_at_the_failed_step_and_says_what_was_done(moves, monkeypatch, capsys):
    cfg, steps, toml = moves
    monkeypatch.setattr(deploy, "build", lambda config, **kw: 1)
    assert deploy.move_host(cfg, "thomas", "move2_seek", yes=True, timeout_minutes=5) == 1
    out = capsys.readouterr().out
    assert "STOPPED" in out and "cancelled, copied, confs" in out
    assert 'host = "sarah"' in toml.read_text()  # not switched: the move did not finish


def test_move_host_skips_the_cancel_on_an_idle_host_and_the_copy_without_a_stage(moves, monkeypatch):
    cfg, steps, _ = moves
    monkeypatch.setattr(deploy.stage_commands, "send_checked", lambda config, machine, line, timeout=25:
                        console.ConsoleResult(["Animus Forge is idle"], True, True))
    assert deploy.move_host(cfg, "thomas", None, yes=True, timeout_minutes=5) == 0
    assert not any(s[0] in ("copy", "ended") for s in steps) and steps[-1][0] == "build"


def test_set_host_in_toml_edits_only_the_cluster_host():
    text = CLUSTER_TOML.read_text()
    new = deploy.set_host_in_toml(text, "thomas")
    assert 'host = "thomas"' in new and new.count("thomas") == text.count("thomas") + 1


# ---- test command ----------------------------------------------------------------------------------------------------

GOOD_OUTPUT = """STEP configure
STEP build unit_tests
UNIT_EXIT 0
UNIT_LINE [==========] 812 tests from 150 test suites ran. (900 ms total)
UNIT_LINE [  PASSED  ] 810 tests.
UNIT_LINE [  SKIPPED ] 2 tests, listed below:
PYTEST_EXIT 0
PYTEST_LINE 1500 passed, 30 skipped, 2 deselected in 400.00s (0:06:40)
"""

BAD_OUTPUT = """UNIT_EXIT 1
UNIT_LINE [==========] 812 tests from 150 test suites ran. (900 ms total)
UNIT_LINE [  PASSED  ] 809 tests.
UNIT_LINE [  FAILED  ] 3 tests, listed below:
UNIT_FAILED Suite.One
UNIT_FAILED Suite.Two
PYTEST_EXIT 1
PYTEST_LINE 2 failed, 1498 passed, 30 skipped in 400.00s
PYTEST_FAILED tests/test_a.py::test_x
"""


def test_a_green_run_summarises_to_one_pass():
    summary = testcmd.parse_output(GOOD_OUTPUT)
    assert (summary.unit.passed, summary.unit.skipped, summary.unit.failed) == (810, 2, 0)
    assert (summary.pytest.passed, summary.pytest.skipped, summary.pytest.failed) == (1500, 30, 0)
    assert summary.ok and testcmd.render(summary).endswith("RESULT: PASS")


def test_a_red_run_names_the_failing_tests():
    summary = testcmd.parse_output(BAD_OUTPUT)
    assert not summary.ok and (summary.unit.failed, summary.pytest.failed) == (3, 2)
    text = testcmd.render(summary)
    assert "FAILED Suite.One" in text and "FAILED tests/test_a.py::test_x" in text
    assert text.endswith("RESULT: FAIL")


def test_a_build_failure_or_early_stop_is_a_fail_with_the_reason():
    text = testcmd.render(testcmd.parse_output("BUILD_ERROR x.cpp:1:1: error: nope\nBUILD_FAILED 2 (see log)\n"))
    assert "BUILD FAILED" in text and "x.cpp:1:1" in text and "GTests: NOT RUN" in text and text.endswith("FAIL")
    assert "STOPPED EARLY: link failed" in testcmd.render(testcmd.parse_output("FATAL link failed\n"))


def test_a_crashed_test_binary_is_not_a_pass():
    summary = testcmd.parse_output("UNIT_EXIT 139\nPYTEST_EXIT 0\nPYTEST_LINE 5 passed in 1s\n")
    assert not summary.ok and "crashed" in testcmd.render(summary)


def test_the_tree_is_found_from_the_containers_mounts(cfg, fake):
    fake.when(lambda argv, input: argv[:2] == ["docker", "inspect"],
              Result(0, "/home/x/other=/other\n/home/x/mlac/azerothcore=/azerothcore\n\n"))
    tree = Path("/home/x/mlac/azerothcore/.claude/worktrees/w1")
    assert testcmd.container_path(cfg, tree) == ("/azerothcore/.claude/worktrees/w1", "/azerothcore")
    with pytest.raises(ui.Failure, match="not inside any mount"):
        testcmd.container_path(cfg, Path("/elsewhere/tree"))
    fake.rules.clear()
    fake.when(lambda argv, input: True, Result(1, "", "No such container: claude-syntax"))
    with pytest.raises(ui.Failure, match="cannot inspect"):
        testcmd.container_path(cfg, tree)


def test_test_command_runs_the_script_in_the_container_with_paths_as_arguments(cfg, monkeypatch, capsys):
    captured = {}

    class FakeProcess:
        returncode = 0
        stdout = iter(GOOD_OUTPUT.splitlines(keepends=True))

        def wait(self):
            return 0

    def popen(command, **kwargs):
        captured["command"] = command
        return FakeProcess()
    monkeypatch.setattr(testcmd.subprocess, "Popen", popen)
    assert testcmd.run(cfg, "/azerothcore/t", "/azerothcore/var/b", gpu=True, jobs=4) == 0
    command = captured["command"]
    assert command[:4] == ["docker", "exec", "claude-syntax", "bash"] and command[4].endswith(testcmd.SCRIPT)
    assert command[command.index("--src") + 1] == "/azerothcore/t" and "--gpu" in command
    assert command[command.index("--python") + 1].endswith(".venv/bin/python")
    assert "RESULT: PASS" in capsys.readouterr().out


def test_the_test_script_is_tracked_and_executable():
    script = Path(__file__).resolve().parents[2] / "tools" / "forgectl-test.sh"
    assert script.is_file() and script.stat().st_mode & stat.S_IXUSR
    assert "HIP_VISIBLE_DEVICES" in script.read_text() and "llvm-17" in script.read_text()


# ---- videos, ui and the command line ---------------------------------------------------------------------------------

def test_videos_wraps_collect_videos_with_the_clusters_workers(cfg):
    command = videos.argv(cfg, "move2_seek", "check")
    assert command[0] == videos.SCRIPT and "--check" in command and command[-1] == "move2_seek"
    workers = command[command.index("--workers") + 1]
    assert workers == "spencer@192.168.0.66 thomas@192.168.0.67 moloch@192.168.0.117"
    with pytest.raises(ui.Failure):
        videos.run(cfg, "bad name; rm", False, False, False)


def test_videos_on_host_runs_the_script_on_the_host(cfg, fake, capsys):
    fake.when(lambda argv, input: True, Result(0, "Done: 0 of 3 workers failed\n"))
    assert videos.run(cfg, "move2_seek", check=False, dry_run=False, on_host=True, yes=True) == 0
    argv, script, _ = fake.calls[0]
    assert argv[0] == "ssh" and "192.168.0.68" in " ".join(argv) and "./apps/forge/tools/collect-videos.sh" in script


def test_videos_copy_asks_first_but_check_does_not(cfg, fake, monkeypatch):
    monkeypatch.setattr(sys.stdin, "isatty", lambda: False)
    assert videos.run(cfg, "move2_seek", check=False, dry_run=False, on_host=True) == 1
    assert fake.calls == []
    fake.when(lambda argv, input: True, Result(0, "listing\n"))
    assert videos.run(cfg, "move2_seek", check=True, dry_run=False, on_host=True) == 0
    assert "--check" in fake.calls[0][1]


def test_confirm_with_yes_does_not_ask(monkeypatch, capsys):
    monkeypatch.setattr(ui, "ask", lambda prompt: pytest.fail("asked"))
    ui.confirm(["do a thing"], yes=True)
    out = capsys.readouterr().out
    assert "do a thing" in out and "--yes" in out


def test_strip_ansi_and_table():
    assert ui.strip_ansi("\x1b[0m\x1b[36mred\x1b[0m \x1b[?2004h!") == "red !"
    assert ui.table(["a", "bb"], [["1", "2"], ["333", "4"]]).splitlines()[0] == "a    bb"


@pytest.mark.parametrize("command", ["cluster", "status", "stage", "logs", "build", "conf-sync", "test", "videos"])
def test_every_command_has_help_with_an_example(command, capsys):
    with pytest.raises(SystemExit) as exit_info:
        cli.main([command, "--help"])
    assert exit_info.value.code == 0
    assert "example" in capsys.readouterr().out.lower()


def test_move_host_help_has_an_example(capsys):
    with pytest.raises(SystemExit):
        cli.main(["cluster", "move-host", "--help"])
    assert "forgectl cluster move-host thomas move2_seek" in capsys.readouterr().out


def test_main_exit_codes(tmp_path, capsys, monkeypatch):
    assert cli.main([]) == 2
    assert cli.main(["--config", str(tmp_path / "missing.toml"), "status"]) == 1
    assert "not found" in capsys.readouterr().err
    with pytest.raises(SystemExit) as exit_info:
        cli.main(["stage", "explode"])
    assert exit_info.value.code == 2


def test_main_dispatches_and_turns_a_failure_into_exit_1(monkeypatch, capsys):
    real = config_module.load
    monkeypatch.setattr(cli.config_module, "load", lambda path=None: real(CLUSTER_TOML))
    monkeypatch.setattr(stage, "status", lambda config: (_ for _ in ()).throw(ui.Failure("boom")))
    assert cli.main(["status"]) == 1
    assert "boom" in capsys.readouterr().err
    monkeypatch.setattr(cluster, "run", lambda config, include_out=False: 0)
    assert cli.main(["cluster"]) == 0


def test_the_root_shim_runs_the_package():
    shim = Path(__file__).resolve().parents[4] / "forgectl"
    done = remote.execute([sys.executable, str(shim), "--help"], timeout=30)
    assert done.ok and "forgectl operates the forge training cluster" in done.out


# ---- the audit log ---------------------------------------------------------------------------------------------------

def audit_lines(home):
    path = home / "audit.log"
    return path.read_text().splitlines() if path.exists() else []


@pytest.fixture
def cli_cfg(monkeypatch):
    real = config_module.load
    monkeypatch.setattr(cli.config_module, "load", lambda path=None: real(CLUSTER_TOML))


def test_a_state_changing_command_leaves_one_audit_line_with_who_what_where_and_how(cli_cfg, sent, forgectl_home):
    assert cli.main(["stage", "cancel", "--yes"]) == 0
    (line,) = audit_lines(forgectl_home)
    assert f"user={audit.username()}" in line and "machines=sarah,spencer,thomas,moloch" in line
    assert "confirm=--yes" in line and "outcome=done" in line and 'cmd="forgectl stage cancel --yes"' in line
    assert re.match(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d[+-]\d{4} ", line)
    assert oct((forgectl_home / "audit.log").stat().st_mode & 0o777) == "0o600"


def test_a_person_confirming_a_declined_and_a_failed_command_are_told_apart(cli_cfg, monkeypatch, forgectl_home):
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    recorder = FakeConsole()
    monkeypatch.setattr(console, "send", recorder)
    monkeypatch.setattr(ui, "ask", lambda prompt: "y")
    assert cli.main(["stage", "resume", "move2_seek"]) == 0
    monkeypatch.setattr(ui, "ask", lambda prompt: "n")
    assert cli.main(["stage", "resume", "move2_seek"]) == 1
    monkeypatch.setattr(console, "send", FakeConsole(fail_on=("sarah",)))
    assert cli.main(["stage", "resume", "move2_seek", "--yes"]) == 1
    done, declined, failed = audit_lines(forgectl_home)
    assert "confirm=prompt outcome=done" in done and "machines=sarah" in done
    assert "confirm=declined outcome=declined" in declined and "machines=-" in declined
    assert "confirm=--yes outcome=failed" in failed


def test_no_terminal_and_no_yes_is_logged_as_declined(cli_cfg, sent, monkeypatch, forgectl_home):
    monkeypatch.setattr(sys.stdin, "isatty", lambda: False)
    assert cli.main(["stage", "resume", "move2_seek"]) == 1
    assert "outcome=declined" in audit_lines(forgectl_home)[0]


def test_read_only_commands_are_not_logged(cli_cfg, sent, fake, monkeypatch, forgectl_home):
    monkeypatch.setattr(cluster, "run", lambda config, include_out=False: 0)
    monkeypatch.setattr(stage, "status", lambda config: 0)
    monkeypatch.setattr(confsync, "run", lambda config, check, yes: 0)
    monkeypatch.setattr(videos, "run", lambda *a, **k: 0)
    for argv in (["cluster"], ["status"], ["stage", "status"], ["logs"], ["conf-sync", "--check"],
                 ["videos", "move2_seek", "--check"], ["videos", "move2_seek", "--dry-run"]):
        monkeypatch.setattr(logs, "run", lambda *a, **k: 0)
        assert cli.main(argv) == 0, argv
    assert not (forgectl_home / "audit.log").exists()
    for argv in (["conf-sync"], ["videos", "move2_seek", "--yes"]):
        cli.main(argv)
    assert len(audit_lines(forgectl_home)) == 2


def test_an_unwritable_audit_log_refuses_the_command_before_anything_is_sent(cli_cfg, sent, monkeypatch, tmp_path,
                                                                             capsys):
    blocker = tmp_path / "not-a-directory"
    blocker.write_text("x")
    monkeypatch.setenv("FORGECTL_HOME", str(blocker / "home"))
    assert cli.main(["stage", "cancel", "--yes"]) == 1
    assert sent.sent == [] and "audit log" in capsys.readouterr().err


def test_an_exception_still_leaves_a_failed_line(cli_cfg, monkeypatch, forgectl_home):
    monkeypatch.setattr(stage, "run", lambda *a, **k: (_ for _ in ()).throw(SystemExit(143)))
    with pytest.raises(SystemExit):
        cli.main(["stage", "cancel", "--yes"])
    assert "outcome=failed" in audit_lines(forgectl_home)[0] and "SystemExit" in audit_lines(forgectl_home)[0]


def test_two_runs_append_and_keep_both_lines(cli_cfg, sent, forgectl_home):
    cli.main(["stage", "pause", "--yes"])
    cli.main(["stage", "cancel", "--yes"])
    assert [l.split('cmd="')[1].split('"')[0] for l in audit_lines(forgectl_home)] == [
        "forgectl stage pause --yes", "forgectl stage cancel --yes"]


# ---- build --cluster under a running stage ---------------------------------------------------------------------------

RUNNING = ["Forge: move2_seek | training"]
IDLE = ["Animus Forge is idle"]


@pytest.fixture
def host_console(monkeypatch):
    """The host's console as the build sees it; replies["forge status"] is what decides running or idle."""
    recorder = FakeConsole(replies={"forge status": RUNNING})
    monkeypatch.setattr(console, "send", recorder)
    return recorder


@pytest.fixture
def plan_end(monkeypatch):
    """wait_for_log answers 'Plan ended' and remembers what it was asked."""
    seen = []
    monkeypatch.setattr(deploy, "wait_for_log", lambda config, machine, pattern, seconds, since=None:
                        seen.append((machine.name, pattern)) or "Plan ended: cancelled")
    return seen


def test_a_running_stage_makes_the_cluster_build_refuse_even_with_yes(cfg, fake, deployable, host_console, capsys):
    cluster_fake = FakeCluster(fake)
    with pytest.raises(ui.Failure, match="a stage is running.*stage cancel.*--stop-running"):
        deploy.build(cfg, cluster=True, yes=True, timeout_minutes=5)
    assert cluster_fake.pulled == [] and ("push", "lan", "forge") not in deployable
    assert [line for _, line in host_console.sent] == ["forge status"]  # nothing but the read-only status


def test_stop_running_cancels_everywhere_waits_for_plan_ended_then_builds(cfg, fake, deployable, host_console,
                                                                          monkeypatch, capsys):
    cluster_fake = FakeCluster(fake)
    ended = []
    monkeypatch.setattr(deploy, "wait_for_log", lambda config, machine, pattern, seconds, since=None:
                        ended.append((machine.name, pattern, list(cluster_fake.pulled), len(host_console.sent)))
                        or "Plan ended: cancelled")
    assert deploy.build(cfg, cluster=True, yes=True, timeout_minutes=5, stop_running=True) == 0
    cancels = [name for name, line in host_console.sent if line == "forge cancel"]
    assert cancels == ["sarah", "spencer", "thomas", "moloch"]
    assert ended == [("sarah", "Plan ended", [], 5)]  # 1 status + 4 cancels were sent, and nothing was pulled yet
    assert len(cluster_fake.pulled) == 4
    out = capsys.readouterr().out
    assert "--stop-running: cancel the running stage" in out and "wait for 'Plan ended' on sarah" in out


def test_stop_running_does_nothing_extra_on_an_idle_host(cfg, fake, deployable, host_console, plan_end):
    host_console.replies["forge status"] = IDLE
    FakeCluster(fake)
    assert deploy.build(cfg, cluster=True, yes=True, timeout_minutes=5, stop_running=True) == 0
    assert not any(line == "forge cancel" for _, line in host_console.sent) and plan_end == []


def test_a_cancel_that_does_not_reach_everyone_builds_nothing(cfg, fake, deployable, plan_end, monkeypatch):
    monkeypatch.setattr(console, "send", FakeConsole(replies={"forge status": RUNNING}, fail_on=("thomas",)))
    cluster_fake = FakeCluster(fake)
    with pytest.raises(ui.Failure, match="cancel was not accepted everywhere"):
        deploy.build(cfg, cluster=True, yes=True, timeout_minutes=5, stop_running=True)
    assert cluster_fake.pulled == [] and ("push", "lan", "forge") not in deployable


def test_a_console_that_does_not_answer_is_not_taken_for_an_idle_host(cfg, fake, deployable, monkeypatch):
    monkeypatch.setattr(console, "send", FakeConsole(fail_on=("sarah",)))
    cluster_fake = FakeCluster(fake)
    fake.rules.insert(0, (lambda argv, input: "docker ps" in (input or ""), Result(0, "abc123\n")))  # it is up
    with pytest.raises(ui.Failure, match="cannot tell whether a stage is running"):
        deploy.build(cfg, cluster=True, yes=True, timeout_minutes=5)
    assert cluster_fake.pulled == []


def test_a_host_whose_worldserver_is_down_can_be_rebuilt(cfg, fake, deployable, monkeypatch):
    monkeypatch.setattr(console, "send", FakeConsole(fail_on=("sarah",)))
    cluster_fake = FakeCluster(fake)  # answers the docker ps with nothing: no container is running
    assert deploy.build(cfg, cluster=True, yes=True, timeout_minutes=5) == 0
    assert len(cluster_fake.pulled) == 4


def test_the_cli_passes_stop_running_and_logs_the_cancel(cfg, fake, deployable, host_console, plan_end, forgectl_home,
                                                         monkeypatch):
    real = config_module.load
    monkeypatch.setattr(cli.config_module, "load", lambda path=None: real(CLUSTER_TOML))
    FakeCluster(fake)
    assert cli.main(["build", "--cluster", "--yes"]) == 1
    assert cli.main(["build", "--cluster", "--yes", "--stop-running"]) == 0
    refused, done = audit_lines(forgectl_home)
    assert "outcome=failed" in refused and "confirm=not-reached" in refused and "a stage is running" in refused
    assert "outcome=done" in done and "stopped the running stage first" in done


# ---- stage start shows what it archives ------------------------------------------------------------------------------

def run_at(steps, exists=True, why=""):
    return lambda config, name: stage.ExistingRun(name, exists, steps, why)


def test_start_plan_names_the_run_it_archives_with_its_step_count(cfg, sent, monkeypatch, capsys):
    monkeypatch.setattr(stage, "existing_run", run_at(178_000_000))
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    monkeypatch.setattr(ui, "ask", lambda prompt: "y")
    assert stage.run(cfg, "start", ["move2_seek"], yes=False) == 0  # a person at the prompt needs no flag
    assert "move2_seek has a run at 178M steps; start archives it" in capsys.readouterr().out


def test_yes_will_not_archive_a_big_run_without_archive_ok_and_says_what_it_would_have_archived(
        cfg, sent, monkeypatch, capsys, forgectl_home):
    real = config_module.load
    monkeypatch.setattr(cli.config_module, "load", lambda path=None: real(CLUSTER_TOML))
    monkeypatch.setattr(stage, "existing_run", run_at(178_000_000))
    assert cli.main(["stage", "start", "move2_seek", "--yes"]) == 1
    captured = capsys.readouterr()
    assert "move2_seek has a run at 178M steps; start archives it" in captured.out
    assert "--archive-ok" in captured.err and sent.sent == []
    (line,) = audit_lines(forgectl_home)
    assert "outcome=failed" in line and "move2_seek has a run at 178M steps; start archives it" in line


def test_yes_with_archive_ok_archives_and_the_line_is_in_output_and_audit_log(cfg, sent, monkeypatch, capsys,
                                                                             forgectl_home):
    real = config_module.load
    monkeypatch.setattr(cli.config_module, "load", lambda path=None: real(CLUSTER_TOML))
    monkeypatch.setattr(stage, "existing_run", run_at(178_000_000))
    assert cli.main(["stage", "start", "move2_seek", "--yes", "--archive-ok"]) == 0
    assert "move2_seek has a run at 178M steps; start archives it" in capsys.readouterr().out
    (line,) = audit_lines(forgectl_home)
    assert "outcome=done" in line and "confirm=--yes" in line and "178M steps; start archives it" in line
    assert sent.sent == [("sarah", "forge start move2_seek")]


@pytest.mark.parametrize("steps,needs_flag", [(0, False), (999_999, False), (1_000_000, False), (1_000_001, True),
                                              (None, True)])
def test_the_threshold_is_more_than_one_million_steps_and_an_unreadable_count_counts_as_big(cfg, sent, monkeypatch,
                                                                                         steps, needs_flag):
    monkeypatch.setattr(stage, "existing_run", run_at(steps, why="no env_steps in its metrics.csv"))
    if needs_flag:
        with pytest.raises(ui.Failure, match="--archive-ok"):
            stage.run(cfg, "start", ["move2_seek"], yes=True)
        assert sent.sent == []
    else:
        assert stage.run(cfg, "start", ["move2_seek"], yes=True) == 0


def test_no_run_to_archive_needs_no_flag_and_resume_never_looks(cfg, sent, monkeypatch, capsys):
    monkeypatch.setattr(stage, "existing_run", run_at(None, exists=False))
    assert stage.run(cfg, "start", ["move2_seek"], yes=True) == 0
    assert "has no run on sarah; nothing is archived" in capsys.readouterr().out
    monkeypatch.setattr(stage, "existing_run", lambda config, name: pytest.fail("resume reads no run"))
    assert stage.run(cfg, "resume", ["move2_seek"], yes=True) == 0


def test_an_unreadable_runs_directory_is_treated_as_possibly_big(cfg, fake, monkeypatch):
    monkeypatch.setattr(console, "send", FakeConsole())
    fake.when(lambda argv, input: True, Result(255, "", "ssh: No route to host"))
    with pytest.raises(ui.Failure, match="could not be read"):
        stage.run(cfg, "start", ["move2_seek"], yes=True)


def test_existing_run_reads_the_hosts_runs_directory(cfg, fake):
    fake.when(on_target("192.168.0.68", "metrics.csv"), Result(0, "exists=1\nsteps=177946624\n"))
    run = stage.existing_run(cfg, "move2_seek")
    assert (run.exists, run.steps) == (True, 177_946_624)
    script = fake.calls[0][1]
    assert '"$HOME"/animus-forge/var/animus-forge/shared/runs/move2_seek' in script
    fake.rules.clear()
    fake.when(lambda argv, input: True, Result(0, "exists=0\n"))
    assert stage.existing_run(cfg, "move2_seek").exists is False
    fake.rules.clear()
    fake.when(lambda argv, input: True, Result(0, "exists=1\nsteps=\n"))
    assert stage.existing_run(cfg, "move2_seek").steps is None


def test_the_run_script_reads_env_steps_by_column_name_on_a_real_shell(tmp_path):
    runs = tmp_path / "runs"
    (runs / "full").mkdir(parents=True)
    (runs / "full" / "metrics.csv").write_text("update,reward,env_steps,x\n1,0.1,100,1\n2,0.2,178000000,2\n")
    (runs / "headeronly").mkdir()
    (runs / "headeronly" / "metrics.csv").write_text("update,env_steps\n")
    (runs / "empty").mkdir()
    (runs / "nometrics").mkdir()
    (runs / "nometrics" / "latest.pt").write_text("x")

    def info(name):
        done = remote.execute(["bash", "-s"], input=stage.run_info_script(str(runs), name))
        return dict(line.split("=", 1) for line in done.out.splitlines())
    assert info("full") == {"exists": "1", "steps": "178000000"}
    assert info("empty") == {"exists": "0"} and info("missing") == {"exists": "0"}  # nothing to archive
    assert info("headeronly")["steps"] != "0" and not info("headeronly")["steps"].isdigit()
    assert info("nometrics") == {"exists": "1", "steps": ""}


# ---- signals during a console send -----------------------------------------------------------------------------------

SEND_RUNNER = '''
import sys
sys.path.insert(0, {apps!r})
from forgectl import config, console
fake, marker, settle = sys.argv[1], sys.argv[2], float(sys.argv[3])
console.attach_argv = lambda cfg, machine: [sys.executable, fake, marker, "hang"]
cfg = config.load({toml!r})
console.send(cfg, cfg.host, "forge status", timeout=60, settle=settle)
print("returned")
'''


def start_send_runner(tmp_path, settle):
    """A child process running console.send against the fake console (a real pty and a real attach-like client)."""
    import subprocess
    fake_script = tmp_path / "fake_console.py"
    fake_script.write_text(FAKE_CONSOLE)
    runner = tmp_path / "runner.py"
    runner.write_text(SEND_RUNNER.format(apps=str(CLUSTER_TOML.parent), toml=str(CLUSTER_TOML)))
    marker = tmp_path / "marker"
    child = subprocess.Popen([sys.executable, str(runner), str(fake_script), str(marker), str(settle)],
                             env={**os.environ, "FORGECTL_HOME": str(tmp_path / "home")}, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
    return child, marker


def wait_for(path, seconds=15):
    deadline = time.time() + seconds
    while not path.exists() and time.time() < deadline:
        time.sleep(0.05)
    assert path.exists(), f"{path.name} never appeared"


@pytest.mark.parametrize("signum", [15, 1], ids=["SIGTERM", "SIGHUP"])
def test_a_signal_while_waiting_for_the_reply_still_detaches_before_the_process_ends(tmp_path, signum):
    child, marker = start_send_runner(tmp_path, settle=0.3)
    try:
        wait_for(tmp_path / "marker.typed")  # the line is typed and the console never answers: the send is waiting
        child.send_signal(signum)
        out, err = child.communicate(timeout=20)
    finally:
        child.kill()
    assert child.returncode == 128 + signum and "returned" not in out
    wait_for(marker)
    assert marker.read_text() == "detached eof=False"  # Ctrl-P Ctrl-Q reached the console; stdin was never closed


@pytest.mark.parametrize("signum", [15, 1], ids=["SIGTERM", "SIGHUP"])
def test_a_signal_while_attaching_also_detaches(tmp_path, signum):
    child, marker = start_send_runner(tmp_path, settle=30)   # still in the settle read
    try:
        wait_for(tmp_path / "marker.up")
        child.send_signal(signum)
        child.communicate(timeout=20)
    finally:
        child.kill()
    assert child.returncode == 128 + signum
    wait_for(marker)
    assert marker.read_text() == "detached eof=False"


def test_a_signal_during_the_detach_is_held_until_the_detach_is_done():
    import signal
    with pytest.raises(console.Terminated) as raised:
        with console.SignalGuard() as guard:
            guard.shielded = True
            os.kill(os.getpid(), signal.SIGTERM)   # arrives "during the detach": nothing happens yet
            assert guard.pending == signal.SIGTERM
    assert raised.value.code == 143
    assert signal.getsignal(signal.SIGTERM) == signal.SIG_DFL   # the previous handlers are back


def test_the_guard_raises_once_in_the_body_and_restores_the_handlers():
    import signal
    before = signal.getsignal(signal.SIGHUP)
    with pytest.raises(console.Terminated):
        with console.SignalGuard():
            os.kill(os.getpid(), signal.SIGHUP)
            time.sleep(1)  # the handler runs before this returns
    assert signal.getsignal(signal.SIGHUP) == before


def test_a_terminated_command_still_leaves_its_audit_line(cli_cfg, monkeypatch, forgectl_home):
    def killed(config, machine, line, timeout=20, settle=1.5):
        raise console.Terminated(15)
    monkeypatch.setattr(console, "send", killed)
    with pytest.raises(SystemExit) as raised:
        cli.main(["stage", "pause", "--yes"])
    assert raised.value.code == 143
    assert "outcome=failed" in audit_lines(forgectl_home)[0] and "Terminated" in audit_lines(forgectl_home)[0]


# ---- one forgectl per console at a time ------------------------------------------------------------------------------

def hold_lock(machine):
    """Take the machine's console lock the way another forgectl run does (another open file: flock conflicts)."""
    import fcntl
    path = console.lock_path(machine)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    fcntl.flock(descriptor, fcntl.LOCK_EX)
    os.pwrite(descriptor, b"pid 4242", 0)
    return descriptor


def test_a_second_forgectl_waits_then_gives_up_with_a_clear_message_and_sends_nothing(cfg, monkeypatch, capsys,
                                                                                      forgectl_home):
    monkeypatch.setattr(console, "spawn", lambda argv: pytest.fail("attached while another run held the console"))
    holder = hold_lock(cfg.host)
    try:
        started = time.monotonic()
        with pytest.raises(ui.Failure, match=r"another forgectl \(holder: pid 4242\) is typing into sarah's console"
                                              r".*nothing was sent.*interleave"):
            console.send(cfg, cfg.host, "forge status", lock_timeout=0.6)
        assert 0.5 < time.monotonic() - started < 5
    finally:
        os.close(holder)
    assert "waiting up to 1 s" in capsys.readouterr().out
    assert (forgectl_home / "locks" / "sarah.lock").exists()


def test_a_waiting_forgectl_goes_ahead_when_the_first_one_finishes(cfg, fake_console):
    import threading
    marker = fake_console("echo")
    holder = hold_lock(cfg.host)
    threading.Timer(0.7, lambda: os.close(holder)).start()
    result = console.send(cfg, cfg.host, "forge status", timeout=10, settle=0.3, lock_timeout=10)
    assert result.ok and result.lines[0] == "Forge: move2_seek | training"


def test_other_machines_do_not_wait_for_each_others_lock(cfg, fake_console):
    fake_console("echo")
    holder = hold_lock(cfg.host)
    try:
        result = console.send(cfg, cfg.machine("thomas"), "forge status", timeout=10, settle=0.3, lock_timeout=0.5)
    finally:
        os.close(holder)
    assert result.ok


def test_two_sends_to_one_console_never_overlap(cfg, fake_console, monkeypatch):
    import threading
    fake_console("echo")
    events, real_spawn, real_detach = [], console.spawn, console.detach

    def spawn(argv):
        events.append("attach")
        return real_spawn(argv)

    def detach(pid, fd):
        real_detach(pid, fd)
        events.append("detach")
    monkeypatch.setattr(console, "spawn", spawn)
    monkeypatch.setattr(console, "detach", detach)
    results = []
    threads = [threading.Thread(target=lambda: results.append(
        console.send(cfg, cfg.host, "forge status", timeout=10, settle=0.4, lock_timeout=30))) for _ in range(2)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(30)
    assert events == ["attach", "detach", "attach", "detach"] and all(r.ok for r in results)


def test_a_killed_holder_releases_the_lock(cfg, fake_console, tmp_path):
    import subprocess
    fake_console("echo")
    script = ("import fcntl, os, sys, time\n"
              "fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT)\nfcntl.flock(fd, fcntl.LOCK_EX)\n"
              "print('held', flush=True)\ntime.sleep(60)\n")
    path = console.lock_path(cfg.host)
    path.parent.mkdir(parents=True, exist_ok=True)
    child = subprocess.Popen([sys.executable, "-c", script, str(path)], stdout=subprocess.PIPE, text=True)
    try:
        assert child.stdout.readline().strip() == "held"
        with pytest.raises(ui.Failure, match="another forgectl"):
            console.send(cfg, cfg.host, "forge status", lock_timeout=0.3)
        child.kill()
        child.wait()
        assert console.send(cfg, cfg.host, "forge status", timeout=10, settle=0.3, lock_timeout=5).ok
    finally:
        child.kill()


def test_a_stage_command_reports_a_busy_console_as_that_machines_failure(cfg, monkeypatch, capsys):
    def busy(config, machine, line, timeout=20, settle=1.5):
        raise ui.Failure(f"another forgectl is typing into {machine.name}'s console")
    monkeypatch.setattr(console, "send", busy)
    assert stage.run(cfg, "cancel", [], yes=True) == 1
    assert "FAILED another forgectl is typing into sarah's console" in capsys.readouterr().out


# ---- conf writes: temporary file, then mv ----------------------------------------------------------------------------

@pytest.fixture
def conf_box(tmp_path, cfg):
    """A conf file on this computer, a config whose sarah is local and has it as its conf, and a place for fake
    docker/mv programs in front of the real ones."""
    checkout = tmp_path / "checkout"
    conf = checkout / "env/dist/etc/modules/mod_animus_forge.conf"
    conf.parent.mkdir(parents=True)
    conf.write_text("old line\n")
    conf.chmod(0o640)
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    machine = dataclasses.replace(cfg.host, path=str(checkout), local=True)
    return types_namespace(conf=conf, bin=bin_dir, machine=machine, dir=conf.parent)


def types_namespace(**kwargs):
    import types
    return types.SimpleNamespace(**kwargs)


def run_write(cfg, box, text, stamp="20261007-120000"):
    script = confsync.write_script(cfg, box.machine, text, stamp)
    env = {**os.environ, "PATH": f"{box.bin}:{os.environ['PATH']}"}
    import subprocess
    return subprocess.run(["bash", "-s"], input=script, capture_output=True, text=True, env=env)


def fake_program(box, name, body):
    path = box.bin / name
    path.write_text("#!/bin/bash\n" + body + "\n")
    path.chmod(0o755)


def test_the_conf_is_written_to_a_temporary_file_and_moved_into_place(cfg, conf_box):
    inode_before = conf_box.conf.stat().st_ino
    done = run_write(cfg, conf_box, "new line\nsecond\n")
    assert done.returncode == 0 and done.stdout.strip().splitlines()[-1] == "FORGECTL_WRITE=mv"
    assert conf_box.conf.read_text() == "new line\nsecond\n"
    assert conf_box.conf.stat().st_ino != inode_before            # a rename, not a rewrite of the live file
    assert oct(conf_box.conf.stat().st_mode & 0o777) == "0o640"    # the mode survived
    assert (conf_box.dir / "mod_animus_forge.conf.bak-20261007-120000").read_text() == "old line\n"
    assert [p.name for p in conf_box.dir.iterdir() if "forgectl-new" in p.name] == []  # no temporary file left


def test_the_live_conf_is_never_opened_for_writing_before_the_rename(cfg, conf_box):
    # the connection drops after the temporary file is written and before the rename: the live conf is the old one
    script = confsync.write_script(cfg, conf_box.machine, "new line\n", "s1")
    before_mv = script.split('if [ "$how" = mv ] && ! mv')[0] + "exit 0\n"   # everything up to the rename
    import subprocess
    subprocess.run(["bash", "-s"], input=before_mv, capture_output=True, text=True)
    assert conf_box.conf.read_text() == "old line\n"
    assert any("forgectl-new" in p.name and p.read_text() == "new line\n" for p in conf_box.dir.iterdir())


def test_a_temporary_file_that_does_not_hold_the_new_text_is_never_moved_in(cfg, conf_box):
    script = confsync.write_script(cfg, conf_box.machine, "new line\n", "s1")
    tampered = script.replace(confsync.hashlib.sha256(b"new line\n").hexdigest(), "0" * 64)
    import subprocess
    done = subprocess.run(["bash", "-s"], input=tampered, capture_output=True, text=True)
    assert done.returncode == 3 and "was not touched" in done.stderr
    assert conf_box.conf.read_text() == "old line\n"
    assert [p.name for p in conf_box.dir.iterdir() if "forgectl-new" in p.name] == []


def test_a_failing_mv_falls_back_to_writing_in_place_after_the_temporary_file_was_checked(cfg, conf_box, capsys):
    fake_program(conf_box, "mv", "echo 'mv: Device or resource busy' >&2; exit 1")
    inode_before = conf_box.conf.stat().st_ino
    done = run_write(cfg, conf_box, "new line\n")
    assert done.returncode == 0 and "FORGECTL_WRITE=in-place-mv-failed" in done.stdout
    assert conf_box.conf.read_text() == "new line\n" and conf_box.conf.stat().st_ino == inode_before
    assert [p.name for p in conf_box.dir.iterdir() if "forgectl-new" in p.name] == []


def test_a_bind_mounted_conf_file_is_written_in_place_so_the_container_sees_it(cfg, conf_box):
    fake_program(conf_box, "docker", f'echo "{conf_box.conf.resolve()}"')   # `docker inspect` lists the mounted file
    fake_program(conf_box, "mv", "echo 'mv must not be used' >&2; exit 9")
    inode_before = conf_box.conf.stat().st_ino
    done = run_write(cfg, conf_box, "new line\n")
    assert done.returncode == 0 and "FORGECTL_WRITE=in-place-bind" in done.stdout
    assert conf_box.conf.read_text() == "new line\n" and conf_box.conf.stat().st_ino == inode_before


def test_a_directory_mount_is_not_mistaken_for_a_file_mount(cfg, conf_box):
    fake_program(conf_box, "docker", f'echo "{conf_box.dir.parent}"')
    assert "FORGECTL_WRITE=mv" in run_write(cfg, conf_box, "new line\n").stdout


def test_the_output_says_when_the_write_was_in_place(cfg, fake, capsys):
    for how, words in (("in-place-mv-failed", "mv over the conf failed"), ("in-place-bind", "bind-mounted file"),
                       ("mv", "")):
        fake.rules.clear()
        fake.when(lambda argv, input: True, Result(0, f"FORGECTL_WRITE={how}\n"))
        confsync.upload(cfg, cfg.machine("spencer"), "x\n", "s1")
        out = capsys.readouterr().out
        assert (words in out) if words else out == ""
        if words:
            assert "wrote it in place" in out and "spencer" in out


def test_a_write_that_does_not_report_or_fails_is_an_error_that_says_the_conf_is_as_it_was(cfg, fake):
    fake.when(lambda argv, input: True, Result(0, "no marker\n"))
    with pytest.raises(ui.Failure, match="did not report how it went"):
        confsync.upload(cfg, cfg.machine("spencer"), "x\n", "s1")
    fake.rules.clear()
    fake.when(lambda argv, input: True, Result(3, "", "the temporary file does not hold the new conf"))
    with pytest.raises(ui.Failure, match="the conf is as it was"):
        confsync.upload(cfg, cfg.machine("spencer"), "x\n", "s1")


def test_the_conf_text_cannot_carry_the_heredoc_marker(cfg):
    with pytest.raises(ui.Failure, match="heredoc marker"):
        confsync.write_script(cfg, cfg.machine("spencer"), "a\nFORGECTL_CONF_EOF\n", "s1")


# ---- move-host stopping part-way: the mixed-state report -------------------------------------------------------------

def test_a_stop_after_the_confs_were_rewritten_says_mixed_state_and_gives_each_restore_command(moves, monkeypatch,
                                                                                               capsys):
    cfg, steps, toml = moves
    monkeypatch.setattr(deploy, "build", lambda config, **kw: 1)
    assert deploy.move_host(cfg, "thomas", "move2_seek", yes=True, timeout_minutes=5) == 1
    out = capsys.readouterr().out
    assert "STOPPED" in out and "THE CLUSTER IS IN A MIXED STATE" in out
    assert "still hold the roles they started with (the host is sarah)" in out and "Do not resume" in out
    stamp = re.search(r"mod_animus_forge\.conf\.bak-(\d{8}-\d{6})", out).group(1)
    conf = "env/dist/etc/modules/mod_animus_forge.conf"
    for name, address in (("sarah", "192.168.0.68"), ("spencer", "192.168.0.66"), ("thomas", "192.168.0.67"),
                          ("moloch", "192.168.0.117")):
        assert f"  {name}: ~/animus-forge/{conf}.bak-{stamp}" in out
        user = name
        assert (f"      restore: ssh {user}@{address} 'cp -p \"$HOME\"/animus-forge/{conf}.bak-{stamp} "
                f"\"$HOME\"/animus-forge/{conf}'") in out
    assert 'host = "sarah"' in toml.read_text()


def test_a_conf_write_that_fails_part_way_lists_only_the_machines_it_reached(moves, monkeypatch, capsys):
    cfg, steps, toml = moves
    calls = []

    def rewrite(config, machine, transform, stamp):
        calls.append(machine.name)
        if machine.name == "thomas":
            raise ui.Failure("thomas: writing the conf failed (unreachable)")
        return "backup"
    monkeypatch.setattr(deploy.confsync, "rewrite", rewrite)
    assert deploy.move_host(cfg, "moloch", None, yes=True, timeout_minutes=5) == 1
    out = capsys.readouterr().out
    assert calls == ["sarah", "spencer", "thomas"]
    assert "MIXED STATE" in out and "The confs of 3 machine(s)" in out
    assert "  sarah:" in out and "  thomas:" in out and "  moloch:" not in out
    assert "may have no backup" in out


def test_a_stop_before_any_conf_was_touched_is_not_called_a_mixed_state(moves, monkeypatch, capsys):
    cfg, steps, toml = moves
    monkeypatch.setattr(deploy, "copy_run", lambda config, old, new, stage: (_ for _ in ()).throw(
        ui.Failure("copying runs/move2_seek failed")))
    assert deploy.move_host(cfg, "thomas", "move2_seek", yes=True, timeout_minutes=5) == 1
    out = capsys.readouterr().out
    assert "STOPPED" in out and "MIXED STATE" not in out and "restore:" not in out


def test_the_restore_command_really_restores_the_conf(cfg, tmp_path):
    home = tmp_path / "home"
    conf = home / "animus-forge/env/dist/etc/modules/mod_animus_forge.conf"
    conf.parent.mkdir(parents=True)
    conf.write_text("rewritten\n")
    (conf.parent / "mod_animus_forge.conf.bak-20261007-120000").write_text("original\n")
    import shlex
    machine = cfg.machine("spencer")
    words = shlex.split(confsync.restore_command(cfg, machine, "20261007-120000"))
    assert words[:2] == ["ssh", "spencer@192.168.0.66"] and len(words) == 3  # the remote command is one argument
    import subprocess
    subprocess.run(["bash", "-c", words[2]], env={**os.environ, "HOME": str(home)}, check=True)
    assert conf.read_text() == "original\n"
    local = dataclasses.replace(machine, local=True)
    conf.write_text("rewritten again\n")
    assert not confsync.restore_command(cfg, local, "20261007-120000").startswith("ssh")
    subprocess.run(["bash", "-c", confsync.restore_command(cfg, local, "20261007-120000")],
                   env={**os.environ, "HOME": str(home)}, check=True)
    assert conf.read_text() == "original\n"


def test_the_audit_line_of_a_stopped_move_says_it_was_mixed(moves, monkeypatch, forgectl_home):
    cfg, steps, toml = moves
    monkeypatch.setattr(deploy, "build", lambda config, **kw: 1)
    monkeypatch.setattr(cli.config_module, "load", lambda path=None: cfg)
    assert cli.main(["cluster", "move-host", "thomas", "move2_seek", "--yes"]) == 1
    (line,) = audit_lines(forgectl_home)
    assert "outcome=failed" in line and "stopped in a mixed state" in line
    assert "machines=sarah,spencer,thomas,moloch" in line
