"""The tests' fake sims run in threads: a failing test must fail, not hang. A fake sim's sockets time out (SIM_TIMEOUT),
its thread is a daemon (a sim left blocked does not keep pytest alive) and a join that times out fails the test,
naming the thread."""

import socket
import threading

SIM_TIMEOUT = 30.0


def sim_thread(target, *args, name: str | None = None, **kwargs) -> threading.Thread:
    """A daemon thread for a fake sim (not started)."""
    return threading.Thread(target=target, args=args, kwargs=kwargs, name=name or getattr(target, "__name__", None),
                            daemon=True)


def accept(listener: socket.socket) -> socket.socket:
    """The learner's connection, both ends of the wait bounded by SIM_TIMEOUT."""
    listener.settimeout(SIM_TIMEOUT)
    conn, _ = listener.accept()
    conn.settimeout(SIM_TIMEOUT)
    return conn


def joined(thread: threading.Thread, timeout: float = 10.0) -> None:
    thread.join(timeout=timeout)
    assert not thread.is_alive(), f"fake sim thread {thread.name!r} still running after {timeout:g} s"
