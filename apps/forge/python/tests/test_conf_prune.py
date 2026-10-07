"""apps/forge/tools/conf_prune.py: the AnimusForge.* keys a build dropped, and a machine's conf cleaned of them."""

import io
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "apps" / "forge" / "tools"))

import conf_prune as cp  # noqa: E402

REAL_LIVE_STAGES = cp.live_stages
TAG = "pre-cleanup-2026-10-07"
TUNING = "src/server/game/Animus/Scenario/Curriculum/CurriculumTuning.h"

DIST_OLD = """\
# the template
AnimusForge.Enable = 1
AnimusForge.Curriculum.Pulls.Alpha = 1
AnimusForge.Curriculum.Pulls.Beta = 2
AnimusForge.Curriculum.Travel.Gamma = 3
AnimusForge.Human.Trips = 4
#    AnimusForge.Stage.<name>.Envs
#    AnimusForge.Curriculum.Arena.<stage>.<arena>.Weight
"""
DIST_NEW = """\
# the template
AnimusForge.Enable = 1
AnimusForge.Curriculum.Pulls.Alpha = 1
#    AnimusForge.Stage.<name>.Envs
#    AnimusForge.Curriculum.Arena.<stage>.<arena>.Weight
"""
CONF = """\
[worldserver]
AnimusForge.Enable = 1
AnimusForge.Curriculum.Pulls.Alpha = 5
AnimusForge.Curriculum.Pulls.Beta = 6
AnimusForge.Curriculum.Travel.Gamma=7
  AnimusForge.Human.Trips   =   8
# AnimusForge.Curriculum.Pulls.Commented = 9
AnimusForge.Stage.move2_seek.Envs = 16
AnimusForge.Stage.gone_stage_a.Envs = 16
AnimusForge.Curriculum.Arena.move2_seek.rooms.Weight = 3
AnimusForge.Curriculum.Arena.gone_stage_b.chain.Weight = 3
AnimusForge.Curriculum.Arena.dungeon2_ragefire.dungeon.WeightFinal = 2
Other.Setting = 1
"""
STAGES = {"move2_seek", "dungeon2_ragefire"}


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)


@pytest.fixture
def repo(tmp_path):
    """A throwaway repository with the template at two commits."""
    path = tmp_path / "repo"
    (path / "src/server/apps/worldserver").mkdir(parents=True)
    dist = path / cp.DIST
    # The C++ that names the arena and stage keys, as the checkout's does (StageScenario.cpp).
    source = path / "src/server/game/Animus/Scenario/Curriculum"
    source.mkdir(parents=True)
    (source / "StageScenario.cpp").write_text(
        'auto a = StringFormat("{}Arena.{}.{}.Weight", p); auto b = StringFormat("{}Arena.{}.{}.WeightFinal", p);\n')
    git(path, "init", "-q")
    git(path, "config", "user.email", "t@example.com")
    git(path, "config", "user.name", "t")
    dist.write_text(DIST_OLD)
    git(path, "add", "-A")
    git(path, "commit", "-q", "-m", "old")
    git(path, "tag", "old")
    dist.write_text(DIST_NEW)
    git(path, "commit", "-q", "-am", "new")
    return path


def families(dist_text: str = DIST_NEW) -> list:
    # The template's placeholders, and the Arena.{}.{}.Weight formats the checkout's C++ names (here the real ones).
    return cp.family_patterns(dist_text, ROOT / "src" / "server" / "game" / "Animus")


# ---------------------------------------------------------------------------------------------------- --removed

def test_removed_names_the_keys_the_new_template_lost_and_counts_them_by_group(repo):
    assert cp.removed_keys(repo, "old", "HEAD") == ["AnimusForge.Curriculum.Pulls.Beta",
                                                   "AnimusForge.Curriculum.Travel.Gamma", "AnimusForge.Human.Trips"]
    out = io.StringIO()
    assert cp.run(cp.parse(["--removed", "old", "HEAD", "--repo", str(repo)]), out=out) == 0
    text = out.getvalue()
    assert "3 key(s) in old and not in HEAD" in text
    assert "AnimusForge.Curriculum.Pulls: 1" in text and "AnimusForge.Human: 1" in text


def test_a_commented_example_is_not_a_key():
    assert cp.dist_keys("# AnimusForge.Foo = 1\nAnimusForge.Bar = 2\n") == {"AnimusForge.Bar"}


def test_group_of_goes_three_deep_in_the_curriculum_and_two_elsewhere():
    assert cp.group_of("AnimusForge.Curriculum.Pulls.Alpha") == "AnimusForge.Curriculum.Pulls"
    assert cp.group_of("AnimusForge.Stage.move2_seek.TicksPerDecision") == "AnimusForge.Stage"
    assert cp.group_of("AnimusForge.TravelPools") == "AnimusForge.TravelPools"


# ---------------------------------------------------------------------------------------------- what is unknown

def test_unknown_keys_are_the_ones_the_template_and_its_families_do_not_name():
    found = cp.unknown_assignments(CONF, cp.dist_keys(DIST_NEW), families(), STAGES)
    assert {key: why for _, key, why in found} == {
        "AnimusForge.Curriculum.Pulls.Beta": "not in worldserver.conf.dist",
        "AnimusForge.Curriculum.Travel.Gamma": "not in worldserver.conf.dist",
        "AnimusForge.Human.Trips": "not in worldserver.conf.dist",
        "AnimusForge.Stage.gone_stage_a.Envs": "stage gone_stage_a no longer exists",
        "AnimusForge.Curriculum.Arena.gone_stage_b.chain.Weight": "stage gone_stage_b no longer exists"}
    # Line numbers point at the file.
    assert [CONF.splitlines()[number - 1].split("=")[0].strip() for number, _, _ in found][0] == \
        "AnimusForge.Curriculum.Pulls.Beta"


def test_the_families_come_from_the_checkouts_own_template_and_cpp():
    """The real template and sources: a stage's Envs and TicksPerDecision, an arena's Weight, WeightFinal,
    StandInShare and MaxRung, a stage's GoalPlaces are read by name, not listed in the template key by key."""
    dist = (ROOT / cp.DIST).read_text()
    patterns = cp.family_patterns(dist)
    for key in ("AnimusForge.Stage.move2_seek.Envs", "AnimusForge.Stage.move2_seek.TicksPerDecision",
                "AnimusForge.Curriculum.Arena.dungeon2_ragefire.dungeon.Weight",
                "AnimusForge.Curriculum.Arena.dungeon2_ragefire.dungeon.WeightFinal",
                "AnimusForge.Curriculum.Arena.dungeon2_ragefire.dungeon.StandInShare",
                "AnimusForge.Curriculum.Arena.dungeon2_ragefire.dungeon.MaxRung",
                "AnimusForge.Curriculum.Stage.dungeon2_ragefire.GoalPlaces"):
        assert any(pattern.fullmatch(key) for pattern in patterns), key
    assert not any(pattern.fullmatch("AnimusForge.Stage.move2_seek.Nonsense") for pattern in patterns)


def test_the_checkouts_own_template_is_clean_against_itself():
    dist = (ROOT / cp.DIST).read_text()
    assert cp.unknown_assignments(dist, cp.dist_keys(dist), cp.family_patterns(dist), REAL_LIVE_STAGES()) == []


def test_live_stages_are_read_from_stages_cpp():
    stages = REAL_LIVE_STAGES()
    assert {"move2_seek", "dungeon3_deadmines"} <= stages and len(stages) == 12


# ----------------------------------------------------------------------------------------- --check and --prune

@pytest.fixture(autouse=True)
def stages(monkeypatch):
    """The throwaway repositories have no Stages.cpp: the live stages are these."""
    monkeypatch.setattr(cp, "live_stages", lambda source_root=None: STAGES)


def args_for(tmp_path: Path, repo: Path, mode: str, conf: Path) -> list[str]:
    dist = tmp_path / "new.dist"
    dist.write_text(DIST_NEW)
    return [mode, str(conf), "--dist", str(dist), "--repo", str(repo)]


def test_check_lists_the_unknown_keys_and_exits_one(tmp_path, repo):
    conf = tmp_path / "mod_animus_forge.conf"
    conf.write_text(CONF)
    out = io.StringIO()
    assert cp.run(cp.parse([*args_for(tmp_path, repo, "--check", conf), "--old", "old"]), out=out) == 1
    text = out.getvalue()
    assert "5 unknown key(s) of 10 AnimusForge.* assignments" in text
    assert "Pulls.Beta  [not in worldserver.conf.dist]  (removed by this build)" in text
    assert conf.read_text() == CONF                                  # --check touches nothing


def test_check_on_a_clean_conf_exits_zero(tmp_path, repo):
    conf = tmp_path / "clean.conf"
    conf.write_text("AnimusForge.Enable = 1\nAnimusForge.Curriculum.Pulls.Alpha = 1\n")
    assert cp.run(cp.parse(args_for(tmp_path, repo, "--check", conf)), out=io.StringIO()) == 0


def test_prune_comments_the_lines_out_after_a_backup_and_deletes_nothing(tmp_path, repo):
    conf = tmp_path / "mod_animus_forge.conf"
    conf.write_text(CONF)
    out = io.StringIO()
    assert cp.run(cp.parse(args_for(tmp_path, repo, "--prune", conf)), out=out) == 0
    backups = list(tmp_path.glob("mod_animus_forge.conf.bak-*"))
    assert len(backups) == 1 and backups[0].read_text() == CONF
    pruned = conf.read_text()
    assert len(pruned.splitlines()) == len(CONF.splitlines())
    assert re.search(r"^#pruned \d{8}-\d{6} \(not in worldserver.conf.dist\): AnimusForge.Curriculum.Pulls.Beta = 6$",
                     pruned, re.M)
    assert re.search(r"^#pruned .*\(stage gone_stage_a no longer exists\): AnimusForge.Stage.gone_stage_a.Envs",
                     pruned, re.M)
    # What the new build reads stays, comments and other apps' keys stay, and a second pass finds nothing.
    for kept in ("AnimusForge.Enable = 1", "AnimusForge.Curriculum.Pulls.Alpha = 5", "AnimusForge.Stage.move2_seek.Envs = 16",
                 "Other.Setting = 1", "# AnimusForge.Curriculum.Pulls.Commented = 9",
                 "AnimusForge.Curriculum.Arena.dungeon2_ragefire.dungeon.WeightFinal = 2"):
        assert kept in pruned.splitlines()
    assert cp.run(cp.parse(args_for(tmp_path, repo, "--check", conf)), out=io.StringIO()) == 0


def test_prune_of_a_clean_conf_writes_no_backup(tmp_path, repo):
    conf = tmp_path / "clean.conf"
    conf.write_text("AnimusForge.Enable = 1\n")
    cp.run(cp.parse(args_for(tmp_path, repo, "--prune", conf)), out=io.StringIO())
    assert list(tmp_path.glob("clean.conf.bak-*")) == []


def test_the_cli_refuses_ambiguous_arguments(tmp_path):
    for bad in (["--check"], ["--check", "--prune", "x.conf"], ["--check", "x.conf", "--ssh", "u@h:/p"],
                ["--removed", "a", "b", "--check", "x.conf"], ["--check", "--ssh", "no-colon"]):
        with pytest.raises(SystemExit):
            cp.parse(bad)


# ---------------------------------------------------------------------------------------------------------- ssh

class FakeMachine:
    """A cluster machine's shell, for conf_prune's ssh runner: one file, every command recorded, nothing run."""

    def __init__(self, path: str, content: str):
        self.files = {path: content}
        self.commands: list[list[str]] = []

    def __call__(self, command, input_text):
        self.commands.append(command)
        remote = command[-1]
        if remote.startswith("cat "):
            return 0, self.files[self.path_of(remote)], ""
        # The write script: cp -p backup; cat > path.new; mv path.new path.
        paths = re.findall(r"'([^']+)'", remote)
        original, backup, new, final = paths[0], paths[1], paths[2], paths[4]
        self.files[backup] = self.files[original]
        self.files[new] = input_text
        self.files[final] = self.files.pop(new)
        return 0, "", ""

    @staticmethod
    def path_of(remote: str) -> str:
        return re.search(r"'([^']+)'", remote).group(1)


def test_ssh_check_and_prune_use_batch_mode_and_leave_a_backup_on_the_machine(tmp_path, repo):
    machine = FakeMachine("/home/u/animus-forge/mod_animus_forge.conf", CONF)
    dist = tmp_path / "new.dist"
    dist.write_text(DIST_NEW)
    base = ["--dist", str(dist), "--repo", str(repo), "--ssh", "u@10.0.0.9:/home/u/animus-forge/mod_animus_forge.conf"]
    out = io.StringIO()
    assert cp.run(cp.parse(["--check", *base]), runner=machine, out=out) == 1
    assert "u@10.0.0.9:/home/u/animus-forge/mod_animus_forge.conf:" in out.getvalue()
    assert cp.run(cp.parse(["--prune", *base]), runner=machine, out=io.StringIO()) == 0
    for command in machine.commands:
        assert command[:5] == ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"] and command[5] == "u@10.0.0.9"
    backups = [path for path in machine.files if ".bak-" in path]
    assert len(backups) == 1 and machine.files[backups[0]] == CONF
    assert "#pruned" in machine.files["/home/u/animus-forge/mod_animus_forge.conf"]
    assert cp.run(cp.parse(["--check", *base]), runner=machine, out=io.StringIO()) == 0


def test_ssh_keeps_a_leading_tilde_as_the_remote_home():
    assert cp.quote("~/animus-forge/conf") == "\"$HOME\"/'animus-forge/conf'"
    assert cp.quote("/abs/it's") == "'/abs/it'\\''s'"


def test_an_unreachable_machine_is_an_error_not_an_empty_conf(tmp_path, repo):
    dist = tmp_path / "new.dist"
    dist.write_text(DIST_NEW)
    with pytest.raises(SystemExit, match="could not read"):
        cp.run(cp.parse(["--check", "--ssh", "u@h:/p", "--dist", str(dist), "--repo", str(repo)]),
               runner=lambda command, input_text: (255, "", "Permission denied (publickey)"))


# ---------------------------------------------------------------------- the conf.dist diff is the Visit diff

def tuning_keys(text: str) -> set[str]:
    return set(re.findall(r'f\("([A-Za-z0-9.]+)"', text))


def curriculum_keys(dist_text: str) -> set[str]:
    return {key[len("AnimusForge.Curriculum."):] for key in cp.dist_keys(dist_text)
            if key.startswith("AnimusForge.Curriculum.")}


def checkout_git_env() -> dict:
    """The environment git needs to read this checkout. A git worktree's .git file names the host's path of its
    gitdir, which does not exist inside the dev container (the checkout is mounted at /azerothcore): there the main
    checkout's .git/worktrees/<name> is found by walking up. A plain checkout needs nothing."""
    marker = ROOT / ".git"
    if marker.is_file():
        target = Path(marker.read_text().split("gitdir:", 1)[1].strip())
        if not target.exists():
            for parent in ROOT.parents:
                if (parent / ".git").is_dir() and (parent / ".git" / "worktrees" / target.name).is_dir():
                    return {"GIT_DIR": str(parent / ".git" / "worktrees" / target.name)}
    return {}


def tag_problem() -> str:
    """Why the tag cannot be read here, or ""."""
    result = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "-q", "--verify", f"{TAG}^{{commit}}"],
                            capture_output=True, text=True, env={**os.environ, **checkout_git_env()})
    return "" if result.returncode == 0 else f"git cannot read tag {TAG} here: {result.stderr.strip() or 'no such tag'}"


@pytest.mark.skipif(bool(tag_problem()), reason=tag_problem())
def test_between_the_tag_and_head_the_template_diff_is_the_tuning_visit_diff(monkeypatch):
    """tests/test_conf_covers_tuning.py keeps conf.dist and CurriculumTuning::Visit in step at HEAD; this checks the
    same at the tag, so the removed keys the tool prints are exactly the keys the sim stopped reading."""
    for name, value in checkout_git_env().items():
        monkeypatch.setenv(name, value)
    old_dist, new_dist = cp.git_show(ROOT, TAG), (ROOT / cp.DIST).read_text()
    old_visit = tuning_keys(cp.git_show(ROOT, TAG, TUNING))
    new_visit = tuning_keys((ROOT / TUNING).read_text())
    assert curriculum_keys(old_dist) == old_visit, "the tag's template and Visit disagree"
    assert curriculum_keys(new_dist) == new_visit
    removed_now = {key for key in cp.dist_keys(old_dist) - cp.dist_keys(new_dist)
                   if key.startswith("AnimusForge.Curriculum.")}
    assert {key[len("AnimusForge.Curriculum."):] for key in removed_now} == old_visit - new_visit
    assert len(removed_now) > 300
    # Nothing was added: every key the new build reads is one the deployed conf files may already have.
    assert cp.dist_keys(new_dist) <= cp.dist_keys(old_dist)
