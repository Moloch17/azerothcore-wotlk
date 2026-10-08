"""Loading apps/forge/cluster.toml: the machines, the host, the remote and branch, ports, containers and the dev
container."""
from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .home import forgectl_home

ROLES = ("host", "worker", "dev")
# [doctor] in cluster.toml: thresholds for `forgectl doctor`; a key left out takes this default.
DOCTOR_DEFAULTS = {"disk_min_gb": 30, "learner_error_hours": 6, "partial_stale_minutes": 30, "refused_hours": 3,
                   "dev_gpu_busy_max_percent": 30}
# [notify] in cluster.toml (and ~/.forgectl/notify.toml, which overrides it): `forgectl watch` and its sinks. A sink
# whose value is empty/false is off; with none on, watch prints to stdout only.
NOTIFY_DEFAULTS = {"min_severity": "info", "poll_seconds": 60, "learner_silent_minutes": 10, "confirm_polls": 2,
                   "debounce_minutes": 30, "desktop": False, "command": "", "webhook_url": "", "webhook_header": "",
                   "file": ""}
SEVERITIES = ("info", "warn", "critical")


class ConfigError(Exception):
    """cluster.toml is missing, unreadable or wrong; the message says which key."""


@dataclass(frozen=True)
class Machine:
    name: str
    user: str
    address: str
    role: str
    path: str
    in_cluster: bool
    local: bool = False

    @property
    def target(self) -> str:
        return f"{self.user}@{self.address}"


@dataclass(frozen=True)
class Config:
    file: Path
    repo_root: Path
    host_name: str
    lan_remote: str
    branch: str
    control_port: int
    data_port: int
    weights_port: int
    worldserver: str
    paths: dict
    dev: dict
    machines: tuple = field(default_factory=tuple)
    doctor: dict = field(default_factory=dict)    # [doctor], defaults filled in (DOCTOR_DEFAULTS)
    notify: dict = field(default_factory=dict)    # [notify], defaults filled in (NOTIFY_DEFAULTS)

    def machine(self, name: str) -> Machine:
        for machine in self.machines:
            if machine.name == name:
                return machine
        names = ", ".join(m.name for m in self.machines)
        raise ConfigError(f"no machine named {name!r} in {self.file} (known: {names})")

    @property
    def host(self) -> Machine:
        return self.machine(self.host_name)

    @property
    def cluster(self) -> list[Machine]:
        """The machines in the cluster, host first."""
        members = [m for m in self.machines if m.in_cluster]
        members.sort(key=lambda m: (m.name != self.host_name,))
        return members

    @property
    def workers(self) -> list[Machine]:
        return [m for m in self.cluster if m.name != self.host_name]

    def path_of(self, machine: Machine, key: str) -> str:
        """A path from [paths] inside the machine's checkout, as a string with the checkout's own prefix."""
        return machine.path.rstrip("/") + "/" + self.paths[key]


def _section(given, name: str, defaults: dict, file) -> dict:
    """A [section] of the cluster file over its defaults: unknown keys and wrong types are errors naming the key."""
    if not isinstance(given, dict):
        raise ConfigError(f"{file}: [{name}] must be a table")
    out = dict(defaults)
    for key, value in given.items():
        if key not in defaults:
            raise ConfigError(f"{file}: [{name}]: unknown key {key!r} (known: {', '.join(defaults)})")
        kind = type(defaults[key])
        if kind is bool:
            ok = isinstance(value, bool)
        elif kind is str:
            ok = isinstance(value, str)
        else:
            ok = isinstance(value, (int, float)) and not isinstance(value, bool) and value >= 0
        if not ok:
            wanted = "a number >= 0" if kind in (int, float) else "a " + kind.__name__
            raise ConfigError(f"{file}: [{name}]: {key!r} must be {wanted}, got {value!r}")
        out[key] = value
    if name == "notify" and out["min_severity"] not in SEVERITIES:
        raise ConfigError(f"{file}: [notify]: min_severity must be one of {SEVERITIES}, got {out['min_severity']!r}")
    return out


def default_config_path() -> Path:
    return Path(os.environ.get("FORGECTL_CONFIG") or Path(__file__).resolve().parents[1] / "cluster.toml")


def _need(table: dict, key: str, where: str, kind: type):
    if key not in table:
        raise ConfigError(f"{where}: missing key {key!r}")
    value = table[key]
    if not isinstance(value, kind) or (kind is int and isinstance(value, bool)):
        raise ConfigError(f"{where}: {key!r} must be a {kind.__name__}, got {value!r}")
    return value


def parse(data: dict, file: Path, repo_root: Path) -> Config:
    cluster = data.get("cluster")
    if not isinstance(cluster, dict):
        raise ConfigError(f"{file}: missing the [cluster] table")
    machines = []
    for index, table in enumerate(data.get("machine", [])):
        where = f"{file}: [[machine]] #{index + 1}"
        role = _need(table, "role", where, str)
        if role not in ROLES:
            raise ConfigError(f"{where}: role must be one of {ROLES}, got {role!r}")
        machines.append(Machine(
            name=_need(table, "name", where, str), user=_need(table, "user", where, str),
            address=_need(table, "address", where, str), role=role, path=_need(table, "path", where, str),
            in_cluster=_need(table, "in_cluster", where, bool), local=bool(table.get("local", False))))
    names = [m.name for m in machines]
    if len(set(names)) != len(names):
        raise ConfigError(f"{file}: machine names must be unique, got {names}")
    containers = data.get("containers", {})
    paths = data.get("paths", {})
    for key in ("conf", "runs", "learner_log", "server_log", "errors_log"):
        _need(paths, key, f"{file}: [paths]", str)
    dev = data.get("dev", {})
    for key in ("container", "python", "build_dir_name"):
        _need(dev, key, f"{file}: [dev]", str)
    doctor = _section(data.get("doctor", {}), "doctor", DOCTOR_DEFAULTS, file)
    notify = _section(data.get("notify", {}), "notify", NOTIFY_DEFAULTS, file)
    extra = forgectl_home() / "notify.toml"
    if extra.is_file():   # webhook URLs and tokens belong here, not in the tracked cluster.toml
        try:
            with open(extra, "rb") as handle:
                notify.update(_section(tomllib.load(handle).get("notify", {}), "notify", NOTIFY_DEFAULTS, extra))
        except (OSError, tomllib.TOMLDecodeError) as error:
            raise ConfigError(f"{extra} cannot be read: {error}") from None
    config = Config(
        file=file, repo_root=repo_root, host_name=_need(cluster, "host", f"{file}: [cluster]", str),
        lan_remote=_need(cluster, "lan_remote", f"{file}: [cluster]", str),
        branch=_need(cluster, "branch", f"{file}: [cluster]", str),
        control_port=_need(cluster, "control_port", f"{file}: [cluster]", int),
        data_port=_need(cluster, "data_port", f"{file}: [cluster]", int),
        weights_port=_need(cluster, "weights_port", f"{file}: [cluster]", int),
        worldserver=_need(containers, "worldserver", f"{file}: [containers]", str), paths=dict(paths),
        dev=dict(dev), machines=tuple(machines), doctor=doctor,
        notify=notify)
    host = config.host
    if not host.in_cluster:
        raise ConfigError(f"{file}: the host {host.name!r} must have in_cluster = true")
    return config


def load(path: str | os.PathLike | None = None) -> Config:
    file = Path(path) if path else default_config_path()
    try:
        with open(file, "rb") as handle:
            data = tomllib.load(handle)
    except FileNotFoundError:
        raise ConfigError(f"cluster file not found: {file} (use --config or FORGECTL_CONFIG)") from None
    except tomllib.TOMLDecodeError as error:
        raise ConfigError(f"{file} is not valid TOML: {error}") from None
    return parse(data, file, file.resolve().parents[2])
