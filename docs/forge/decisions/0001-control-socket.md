# 0001: a control socket in the worldserver for stage commands

**Status: PROPOSAL. Nothing in this document is built. It needs the owner's decision (decision 3 of the
human-operable plan: "add a real control socket, or keep driving the console").**

## What is weak today

`forgectl` sends `forge start|resume|pause|cancel|status` by typing a line into the worldserver's console through
`docker attach` in a pty (over `ssh -tt` for a worker), and recovering the reply from a stream that also carries the
sim's log output. It works against the live host, but it depends on:

- **keystrokes into a terminal**: the line is echoed, the reply arrives mixed with log lines (told apart by their
  colour escape), and the end of the reply is guessed from the prompt `AC> ` coming back;
- **the detach sequence** (Ctrl-P Ctrl-Q): if it fails, the attach client lingers, and closing the pty early is
  end-of-file on the console, which shuts the server down;
- **docker**: the user must be allowed to run `docker attach` on the machine, and the container's name is configuration;
- **no structure**: no exit status, no error type, no way to ask "is a plan running?" except parsing a table that is
  meant for people;
- **nothing for workers**: `pause` and `cancel` reach a worker only because forgectl visits each worker's console
  itself.

## What it would replace

Only the transport. The commands stay what they are (`CommandStart`, `CommandResume`, `CommandPause`,
`CommandCancel`, `CommandStatus` in `AnimusForge`), and the in-game `forge ...` console commands stay for people.
`forgectl stage ...` and `forgectl status` would call the socket first and keep `docker attach` as a fallback for a
worldserver that has no socket. `forgectl cluster move-host` would wait on an event instead of a log line.

## Sketch

A request/response line protocol over TCP (not HTTP; there is nothing to route), one JSON object per line.

```
-> {"id": 7, "cmd": "stage.resume", "args": {"stages": ["move2_seek"]}, "token": "..."}
<- {"id": 7, "ok": true, "result": {"plan": ["move2_seek"], "message": "Resuming move2_seek from update 3802"}}
<- {"id": 8, "ok": false, "error": {"code": "no_plan", "message": "nothing is running"}}
```

- Commands: `status` (structured: plan, stage, state, update, steps, sps, warnings, workers with their state and
  last-seen time), `stage.start`, `stage.resume`, `stage.pause`, `stage.cancel`, `plan.wait` (return when the plan
  ends or after N seconds, replacing the log polling in move-host), and `events.subscribe` (a stream of the run
  events that Phase 4 writes to `events.jsonl`).
- **Cluster-wide pause and cancel**: the host already holds a control connection (port 7700) to each worker for
  registration and orders. `stage.pause` and `stage.cancel` on the host would send the order down it, so a client
  talks to the host only, and the "does not reach the workers" gap is closed in C++ instead of in forgectl.
- Replies are produced by the same code that fills the console's tables (`LineSink`), so the console and the socket
  cannot disagree; `status` additionally returns the numbers the table shows as JSON.
- One request at a time per connection; a command that cannot run in the current state returns an error code instead
  of printing a message.

## Security

The socket can stop a training run and start another, so it is not something to leave open.

- **Off by default**: `AnimusForge.Control.Enable = 0`.
- **Where it listens**: a Unix socket in the run directory (`<OutputDir>/control.sock`, mode 0600, reached over ssh
  or by a client in the same container) is the default and needs no other protection. A TCP listener is opt-in
  (`AnimusForge.Control.Listen = "192.168.0.68:7703"`) and refuses to bind to anything but a private (RFC 1918 or
  loopback) address, so it is LAN-only by construction, never a public interface.
- **A shared token** for the TCP listener (`AnimusForge.Control.Token`, a long random string kept in the untracked
  conf, compared in constant time), sent with each request. No token configured: the TCP listener does not start.
  The cluster's own control channel has the same trust model today (a fingerprint, no secret), so this is a step up
  rather than a new risk, but it is not encryption: on a hostile LAN, tunnel it over ssh.
- Requests are length-limited (64 KB) and parsed with the JSON reader the sim already has; stage names are validated
  against the stage list, never passed to a shell.
- Every request and its result is written to the log with the peer address.

## The work in C++

1. `ForgeConfig`: the `Control.*` keys above (read through the existing config manager).
2. A `ControlServer` in `src/server/game/Animus` (Asio, which the core already links): accept loop, line framing,
   JSON parse, a table of handlers. It must not run commands on its own thread: it posts them to the world thread
   the same way the console's `forge` commands run (they touch sim state), and replies when the command returns.
3. Structured `status`: expose what `CommandStatus` computes as a struct, with the table as one rendering of it.
4. Cluster fan-out of pause and cancel through the existing worker control connections, with each worker's
   acknowledgement collected into the reply.
5. Events: reuse the Phase 4 event writer; `events.subscribe` tails it.
6. The Python side (forgectl) gets a client. No tests (removed 2026-10-07; see [tests.md](../reference/tests.md)).
7. A protocol-version bump: the control channel is part of the cluster fingerprint, so this lands in a rebuild of
   every machine, which the plan already schedules once for the other C++ changes.

Rough size: about 600 lines of C++ plus tests, and two to three days with the fan-out; the Python client is small.

## Alternatives considered

- **Keep the console** (what forgectl does today). Costs nothing, works, and its failure modes are known and handled;
  it leaves the weaknesses above and the workers' pause gap, which forgectl papers over.
- **The worldserver's existing SOAP or RA (remote administration) listener.** It already exists in stock
  AzerothCore, but the forge builds `ForgeMain.cpp` and never starts it, and its protocol is the console's text with a
  login; it would solve transport but not structure.
- **Signals or a command file** the sim polls: simple, but one-way, with no reply.

## Decision needed

Build it (recommended in the plan), or keep the console and accept forgectl's `docker attach` path as the supported
one. Also: Unix socket only, or the opt-in LAN listener with a token.
