# Movement: the player controller (src/server/game/Animus/Movement/)

Reference for a manual review. Everything below was read from the code at `forge` bd32b9dc8. Paths are relative to
`src/server/game/Animus/` unless they start with `src/`. Line numbers are of that commit. Where a comment and the code
disagree it is said. Companions: [00-architecture.md](00-architecture.md), [01-forge-core-delta.md](01-forge-core-delta.md),
[cpp-runtime.md](cpp-runtime.md), [cpp-vision.md](cpp-vision.md), [cpp-blocks.md](cpp-blocks.md),
[protocol.md](protocol.md), [tests.md](tests.md), [known-issues.md](known-issues.md).

## Purpose and scope

A seat (a sessionless bot `Player`) moves the way a 3.3.5a client moves a player. The policy holds keys and mouse rates
(`ControlState`); every world tick the controller steps a private `BodyState` under them with its own reimplementation of
the client physics (gravity, jump, step-up, slopes, swimming, flight, fall), then reports the result to the server as a
client would (movement opcodes with the client's cadence), and the server applies it through the stock movement rules
(`ClientMovement::Apply`: verification, anticheat hooks, fall damage, relocation). No spline, teleport or navmesh move
exists for a bot (principle 3, decision 0019: vision-only movement). Motion the core imposes (root, stun, fear, spline, knockback, teleport) is yielded to
and resynced from.

Not in this area: the policy's action space (`Scenario/Curriculum/Blocks/MoveControls.h`, `MoveBlock.*`; see
cpp-blocks.md), the core `ClientMovement` (`src/server/game/Movement/ClientMovement.*`,
see 01-forge-core-delta.md). `forge controller probe` (src/server/scripts/Commands/cs_forge.cpp) prints the controller's
view of a point through `MapWorldQuery`; it is the only "probe" tied to this directory.

## Map table

| path | lines | role |
|---|---|---|
| Movement/PlayerController.h | 249 | Pure types (ControlState, Speeds, Body, BodyState, WorldQuery) and the client's physics constants; declares Step, Resync, Launch, CanJump, CanSteerVertically, MovementFlags. |
| Movement/PlayerController.cpp | 817 | The integrator: sub-stepping, turn, ground/fall/swim/fly steps, wall sliding, terrain rules, Resync. |
| Movement/MapWorldQuery.h | 81 | `WorldQuery` over a live `Map` (floor, normal, liquid, sweep, ceiling, terrain); static ray counters; `SweepRays`/`SweepShare`. |
| Movement/MapWorldQuery.cpp | 168 | Implementation of the above on `Map::GetHeight`, `GetLiquidData`, the two collision trees. |
| Movement/UnitBody.h | 37 | Declares `SpeedsOf(Unit)` and `ShapeOf(Unit)`: the only place controller types are filled from the core. |
| Movement/UnitBody.cpp | 50 | Reads `Unit::GetSpeed`, auras, collision radius/height. |
| Movement/Client.h | 207 | `Client`: the controller as a client (reports, orders, yielding); `Report`, `ServerState`, `ServerLink`, `Counters`. |
| Movement/Client.cpp | 409 | Start, Order, Tick (sub-steps plus exact-moment reports), Finish. |
| Movement/ReportCadence.h | 175 | Client opcode values and when a client sends what: change opcodes, heartbeat, mouse-facing crossings. |
| Movement/ClientOrders.h | 441 | Server-to-client movement orders: opcodes, `Decode`, `AckOpcode`, `FlagsAfter`, `Clock`, `Inbox`. Namespace `Animus::Client`. |
| Movement/FlagRules.h | 100 | `SanitizeFlags`: ReadMovementInfo's flag-stripping rules, shared by real packets and bot reports. |
| Movement/PlayerLink.h | 80 | `PlayerLink : ServerLink` over `ClientMovement::Apply`; process counters; `QueueOrder`. |
| Movement/PlayerLink.cpp | 290 | Builds `MovementInfo` from a `Report`, handles acks, refusals, relay, fall tallies, `State()`. |
| Movement/LinkMemory.h | 43 | Per-seat memory of a link (last good position, invalid streak, fall tallies). |
| Movement/ControllerCost.h | 40 | Process-wide atomics for the status line (thread ns, seat ticks). |
| Movement/CastWatch.h | 69 | Thread-local watch that catches `SMSG_CAST_FAILED` sent to a sim session. |
| Movement/CastWatch.cpp | 39 | The thread-local pointer and `NoteCastFailed`. |
| Movement/Seek.h | 54 | `Seek`: keys that take a body to a point (for a scripted actor). |
| Movement/Capture.h | 100 | Human-capture move stream records (Move 10, Speeds 11), encode/decode/file I/O. |
| Movement/Capture.cpp | 280 | Implementation (gzip via zlib). |
| Movement/Replay.h | 107 | Replay of a recorded player through the controller; drift report types. |
| Movement/Replay.cpp | 414 | `Follow`, `Run`, calibration measures, `Report::Text`. |

## Data flow in one page

Per world tick, per seat, on the seat's map thread inside the map update: `StageScenario::SubTick`
(`Scenario/Curriculum/StageScenario.cpp:2837`). It builds a `PlayerLink(bot, seat.Link)`, a `MapWorldQuery(bot->GetMap(),
phase)` and `ShapeOf(bot)` on the stack, starts the `Client` if not started (`seat.Mover.Start`), drains the session's
order inbox (`bot->GetSession()->MovementOrders()`, filled by `WorldSession::SendPacket` through `QueueOrder` from
whichever thread sent the packet; `Server/WorldSession.cpp:309-321`) and calls `Mover.Order` for each, then
`Mover.Tick(seat.Controls.Held, SpeedsOf(bot), shape, world, diffMs, nowMs, link)`. The held controls (`SeatControls::Held`,
`Blocks/MoveControls.h`) are written by the policy's actions at decision time; the controller reads them every tick.
The owner slot is ticked too when it is played through its row or when it holds the party-follow leader
(`StageScenario.cpp:2872-2876`). The cost is added to `ControllerCost` once per tick call (`:2878`). `Mover.Finish` is
called at an episode's end (`:4662`). `Mover.Stop()` runs in `SeatState::ResetEpisode` (`StageState.h:502`), so each
episode starts the client again from the server's body.

Threading: everything but the inbox is on the seat's map thread. `Inbox` is a mutexed vector (`ClientOrders.h:418`).
`CastWatch` is thread-local (`CastWatch.cpp:23`). The process-wide atomics (`MapWorldQuery::Rays/Heights`,
`PlayerLink::Applied/Refused/Unsticks/OrderPackets/Relayed`, `ControllerCost`) are relaxed atomics read by the status
line (`AnimusForge.cpp:1234-1252`).

## PlayerController (PlayerController.h/.cpp)

### Public API and types (PlayerController.h)

- Constants, all in yd, yd/s, rad. `GRAVITY 19.2911053` (:39), `TERMINAL_VELOCITY 60.148` and `SAFE_FALL_VELOCITY 7.0`
  (slow fall), `JUMP_SPEED 7.9555473` (:46), `SWIM_JUMP_SPEED 9.0967484`, `WALKABLE_NORMAL_Z cos 50deg = 0.6427876` (:50),
  `DIAGONAL 0.7071068`, `KEYBOARD_TURN_WHILE_MOVING 0.75` (:55), `PITCH_LIMIT pi/2`, `STEP_UP = tan 50deg = 1.1917536` (:65),
  `VERTICAL_SHARE 0.7071068` (:68), `SWIM_ENTER 0.75`, `SWIM_LEAVE 0.4` (share of body height), `FLOAT_DEPTH 0.5` (:76),
  `MAX_SUBSTEP 0.05 s` (:79), `SLIDE_ANGLES {30,-30,60,-60}` degrees (:82). Header comments give the Wow.exe virtual address
  of each client constant. The "calibrated (C6)" ones are marked as interpreted, not measured (see Observed issues).
- `enum class Mode : uint8_t { Ground, Falling, Swimming, Flying }`.
- `ControlState` (:94): `int8 Forward/Strafe/Vertical`, `float TurnRate` (rad/s, + left), `PitchRate` (+ up), `bool Jump`
  (one-shot), `Walk`, `KeyboardTurn`, one-shot `FaceTurn` (rad, + left; the camera's "turn to camera"), `FaceTurnApplied`
  (what the client actually turned, consumed by `FreeLook::Advance`).
- `Speeds` (:118): Walk 2.5, Run 7, RunBack 4.5, Swim 4.7222, SwimBack 2.5, Flight 7, FlightBack 4.5, TurnRate pi, PitchRate
  3.14, `CanFly`, `SlowFall`, `WaterWalk`, `CeilingAboveGround 150`.
- `Body {Radius 0.389, Height 2}`; `BodyState` (:141): position (feet), `Yaw` 0..2pi, `Pitch`, `Vx,Vy,Vz` (Vz up positive),
  `Kind`, `FallApexZ`, `FallMs`, and per-step results reset by every `Step`: `AgainstWall`, `SteepSlope`, `Landed`,
  `LandedInWater`, `Jumped`, `FallHeight`, `OverVoid`, `Moved`, `Commanded`; `Unburied` is set by Resync only.
- `Liquid {Present, Level, Deadly}`; `WorldQuery` (:176) pure virtual: `FloorBelow(x,y,z,search)`, `FloorNormalZ`, `LiquidAt`,
  `Sweep(p0,p1,body)` (share 0..1), `Ceiling`, `InTerrain`, `TerrainHeight` (default `INVALID_FLOOR`). `INVALID_FLOOR = -200000`.
- `Step(body, control, speeds, shape, world, dt)` (cpp:663), `Resync(body,x,y,z,yaw,shape,world)` (:713), `Launch(body,vx,vy,vz)`
  (:708), `CanJump` (:756: Ground or Swimming only), `CanSteerVertically` (:763), `MovementFlags` (:768).
- `Flag::*` mirrors the core's `MOVEMENTFLAG_*` bits (:225) to stay core-free.

### The integrator (PlayerController.cpp)

`Step` clears the per-step flags, returns on `dt <= 0`, computes `steps = max(1, ceil(dt/0.05 - 1e-4))` equal sub-steps and runs
`SubStep` for each, OR-ing `landed/jumped/wall/steep/overVoid` and taking the max `FallHeight` across them (:663-706). So a
250 ms tick equals five 50 ms ticks (test `OneLongTickIsTheSameAsFiveShortOnes`). Note `FallMs`, `Moved`, `Commanded` are
accumulated across sub-steps (not reset per sub-step), while `Landed` and friends are re-cleared after each sub-step.

`SubStep` (:636): `Turn(0.5 dt)`, then the mode's step with the whole `dt`, then `Turn(0.5 dt)`. The body therefore moves along the
middle heading of the sub-step. `Turn` (:277) adds `TurnRate*dt` to the wrapped yaw (the keyboard-turn slowdown 0.75 applies
only when `KeyboardTurn` and a movement key is held); pitch changes only when Swimming or Flying, clamped to +-pi/2.
`Speeds::TurnRate/PitchRate` are NOT used to cap the policy's mouse rates (see Observed issues).

`WishOf` (:64): forward/strafe each times 0.7071 when both held; water/air tilt forward by pitch; a vertical key multiplies the
horizontal part by 0.7071 and adds 0.7071 vertical; the vector is normalised if longer than 1 and the speed scaled by its length.
Speed by mode: Swimming `min(Swim,SwimBack)` when holding back else `Swim`; Flying likewise; ground: `Walk` held gives
`min(Run, Walk)`, holding back gives `min(Run, RunBack)`, else `Run`.

`GroundStep` (:291) in order:
1. A held Jump becomes a fall launched with the run frozen at `wish*speed` and `Vz = JUMP_SPEED` (no air control), `Jumped=true`, and
   the same sub-step continues as a `FallStep` (the jump's first slice). The press is spent.
2. `CanFly && Vertical>0` switches to Flying with zero velocity.
3. `SweptMove` along the wish. It uses `FreeShare` = `min(world.Sweep, TerrainShare)`. If not free, `AgainstWall` is set and the move is
   shortened to `free*length - SKIN(0.02)`; then the four `SLIDE_ANGLES` are tried in order: a turned move (shortened by cos) must be
   free all the way (`f >= 1`), and is adopted only if its progress along the wanted direction beats the best by `SLIDE_GAIN 0.01`;
   the first such angle wins (break). Six sweep rays per `FreeShare`.
4. Floor under the new place: `FloorBelow(nx,ny, Z+STEP_UP, 2*STEP_UP)`; if none or at least `FOOTPRINT_DROP 0.5` below the feet the
   `FootprintFloor` (8 points at the body radius) is read for a supporting floor between feet+0.1 and feet-STEP_UP. Water walkers take
   the liquid level as floor when it is not deadly and within a step.
5. Refusals, each setting `AgainstWall` and returning without moving: feet would be inside the terrain (`IntoTerrain`); a rise over a
   floor higher than feet+0.1 whose `FloorNormalZ < 0.6428` (`SteepSlope`); no floor and the knee inside terrain while the body is not;
   no floor and `OverVoid` (nothing at all below: no floor within 1000 yd, no terrain at or below, no liquid) which sets `OverVoid`.
6. Otherwise move; no floor means `StartFalling` with the run; with a floor `Z = floor` (the body snaps to any floor within the search
   window, up or down; there is no separate "stairs" rule beyond STEP_UP in the search); and deep enough liquid
   (`SwimDepth(..., SWIM_ENTER)`) switches to Swimming unless water walking or deadly liquid.

`FallStep` (:382): clears Jump; flying switch as above; trapezoid integration of `Vz` (exact under constant gravity), clamped at
terminal (or 7 with `SlowFall`); a horizontal run is swept and stopped by walls (the run velocity is replaced by the achieved
move); a rise is limited by `Ceiling` (and zeroes Vz); sideways into terrain or over a void stops the run; landing search
`FloorBelow(nx,ny, max(Z,nz)+STEP_UP, ...)` plus a footprint floor only when no centre floor, Vz<=0 and the feet go down past
`Z-0.1`. Fall time `FallMs` accumulates `dt` and, at a landing, is pulled back to the exact moment the feet reach the level
(`landedAt`, :468: solves `z0 + vz0 t - g t^2/2 = level`, clamped to the step). Branches: water landing (on the way down, liquid
not deadly, deep enough) gives `Landed`, `LandedInWater`, `FallHeight = apex - level`, mode Swimming, no damage by the controller;
a floor risen under rising feet carries them up (still rising); a floor at or above the feet while falling gives `Landed`,
`FallHeight = apex - floor`, mode Ground. The controller computes no damage: the core does, from the reported `FallMs` and the
FALL_LAND report (`ClientMovement::Apply` calls `Player::HandleFall`, `src/server/game/Movement/ClientMovement.cpp:151-153`).

`SwimStep` (:511): a jump at any depth launches `SWIM_JUMP_SPEED` upward (client behaviour: no depth test) and continues as a fall.
Otherwise moves 3D along the wish; refuses a bank (no bed, knee in terrain); clamps height to `Level - 0.5*Height` and above the bed;
leaves the water when the liquid is absent or shallower than `SWIM_LEAVE` (0.4 of height): onto the ground if the bed is within a step,
else falls.

`FlyStep` (:578): needs `speeds.CanFly` else falls; 3D move; capped at `ground + CeilingAboveGround (150)`; ceiling check on climbs;
into terrain refused; deep liquid below float depth switches to Swimming; descending to within 0.1 of the ground lands (Ground mode).

`Resync` (:713): body takes the server's x,y,z,yaw, zero velocity; mode by priority: swimming (liquid present, not deadly, `z <= level`,
deep enough), ground if a floor within STEP_UP (searched from `z+STEP_UP` downward, `2*STEP_UP`), else falling. Last rung: if falling,
the point is inside terrain and there is no floor within 1000 yd below, the body stands on the terrain surface (`Unburied=true`).
`MovementFlags` (:768): FORWARD/BACKWARD, STRAFE_*, LEFT/RIGHT only when `KeyboardTurn` (mouse turns send no LEFT/RIGHT), PITCH_*
only when steered and `KeyboardTurn`, ASCENDING/DESCENDING only when steered, WALKING, FALLING/SWIMMING/FLYING by mode, CAN_FLY,
WATERWALKING, FALLING_SLOW (from `SlowFall`).

### Invariants and contracts

- Pure and deterministic: no core types; depends only on `WorldQuery`. Sub-step length is at most 50 ms whatever the tick.
- `Jump` is cleared by whichever step takes it, or by the step that refuses it (air, fly, `FallStep` entry).
- The controller never writes the server; `Client` and `PlayerLink` do.

### Tests

`PlayerControllerTest.cpp` (27 tests, e.g. `OneLongTickIsTheSameAsFiveShortOnes`, `AHillsideIsAWall`, `ASwimJumpAtTheSurface...`,
`MovementFlagsAreThePlayersOwn`) on fake worlds; `ReplayTest.cpp` replays the controller against itself (drift exactly 0).

### Reviewer notes

- Hundreds of lines of terrain special cases (`TerrainShare`, `IntoTerrain`, `OverVoid`, `FootprintFloor`) exist because terrain is in no
  collision tree; each cites a dated dry-check failure. Removing one without the world that exposed it re-opens a bug. The
  `WorldQuery` is the seam: any refactor should keep `PlayerControllerTest` green and add a live dry check.
- Ground handling snaps to any floor in `[Z-STEP_UP.. Z+STEP_UP]` window (search 2*STEP_UP from Z+STEP_UP): descending ground within a
  step never becomes a fall; is that the client's behaviour? UNVERIFIED: C6 recording replays (`forge controller replay`) are the check.
- `STEP_UP` doubles as the knee ray height (+0.05, `MapWorldQuery.cpp:44`) and the search window; changing it moves all three.

## MapWorldQuery (MapWorldQuery.h/.cpp), UnitBody

`MapWorldQuery(Map*, phaseMask, counted=true)`. `counted=false` is used by the camera (`MapVisionWorld` holds one) so the controller's
cost line (`Rays`, `Heights`) counts only the controller.

- `FloorBelow` (cpp:67): `Map::GetHeight(phase, x,y,z, true, search)`; returns `INVALID_FLOOR` if invalid, above `z+0.05`, or below
  `z-search`. One `Heights` count.
- `FloorNormalZ` (:78): four floor samples at +-0.3 yd (each searched from `z+0.75` over 1.5 yd); an invalid sample counts as level (an
  edge reads flat, not as a wall); normal z = `1/sqrt(1+gx^2+gy^2)`. Four height queries.
- `LiquidAt` (:95): `Map::GetLiquidData(phase,x,y,z,2.0,{})`; `Deadly` iff magma or slime flags.
- `RayFree` (:107): length of the first hit along a segment against the static tree (`GetObjectHitPos`) and the dynamic tree, whichever is
  nearer; one `Rays` count per call (two tree casts).
- `Sweep` (:122): six rays (`SweepRays`: centre, left edge and right edge of the body across the move, at knee height `STEP_UP+0.05` and
  chest `0.8*Height`), each cast `length + radius` long; the nearest hit feeds `SweepShare(free, length, radius)`: 1 if
  `free >= length + radius - 0.001` (the slack exists because float rounding made every open-ground sweep read blocked, comment :57-60),
  else `clamp((free - radius)/length, 0, 1)`. Note: Sweep is 3D (length includes dz).
- `Ceiling`, `InTerrain` (`GetGridHeight`, `z < terrain - GROUND_HEIGHT_TOLERANCE`), `TerrainHeight` (counted as a Heights).
Camera accessors on upstream code are used from here: `Map::GetMapCollisionData()`, the static/dynamic tree `GetObjectHitPos` (upstream);
see 01-forge-core-delta.md for what the forge added.

`SpeedsOf` (UnitBody.cpp:22) reads `Unit::GetSpeed` for the nine move types, `CanFly()`, `SlowFall = FEATHER_FALL || HOVER aura`
(comment in the header says "levitate"; levitate is not tested), `WaterWalk = HasWaterWalkAura()`. `ShapeOf` (:42) reads
`GetCollisionRadius/Height`.

Tests: `MapWorldQueryTest.cpp` covers only the pure `SweepRays` and `SweepShare` (2 tests). `MapWorldQuery` over a real `Map` and
`SpeedsOf/ShapeOf` have no unit test (live dry checks and `forge controller probe`).

## Client (Client.h/.cpp), ReportCadence

`Client` (Client.h:80) holds `BodyState Body` (the truth: self observations read it), `Counts` (`Reports, Refused, Changes, Acks,
YieldTicks, Resyncs, Unburied, OverVoid`), per-tick `TickMoved/TickCommanded/TickWall/TickJumps/TickLandings/TickFallHeight`, the last
order and time, and `Unburied`/`Void*` diagnostics. Private: reported position/yaw/pitch, last flags/send time, granted flags, rooted,
jump info for FALLING reports. A `WorldScope` (Client.h:160) points `_shape/_world` at the caller's stack objects for the duration of a
call and restores them (nested Tick -> Start works).

- `Start` (Client.cpp:122): `TakeFromServer(link.State())` (Resync, remember reported = server), sets started, sends one HEARTBEAT
  carrying granted/ROOT/mode flags.
- `Order` (:139): TimeSync ignored (the realm answers); `Teleport` order resyncs; flags = `Animus::Client::FlagsAfter(order, _lastFlags)`;
  granted = `flags & GRANTED_MASK`, rooted = ROOT bit; Root zeroes horizontal velocity; Knockback launches
  `(cos*speedXY, sin*speedXY, -speedZ)` and records the jump info; then an acknowledgement report with `AckOpcode`, counter, move type, value,
  `Applies` flag is sent.
- `Tick` (:188): resets tick tallies; returns if not started or `diffMs==0`. If `state.Imposed || _rooted`: take the body from the server,
  clear Jump and FaceTurn, `_lastFlags = granted|ROOT`, `YieldTicks++`, return (a rooted client yields even though ROOT is the client's own
  flag). If the server moved the seat more than `TELEPORT_YARDS 5` from the last reported position it `Start`s again (`Resyncs++`). Then
  for each sub-step (`ceil(dt/0.05)`, equal): (1) if control flags changed, one change opcode per changed group (`Cadence::Changes(...,
  false, false)`: the jump and landing are handled below), (2) a held `FaceTurn` snaps yaw and sends one SET_FACING, adding to
  `FaceTurnApplied`, (3) a Jump press that `CanJump` sends MSG_MOVE_JUMP with jump info (`-JUMP_SPEED` or the swim speed, run direction),
  (4) `Step(h)`, (5) walked-off-an-edge fall gets zero-launch jump info (:289), (6) inside the step, in time order: facing crossings
  (every 0.1 rad of raw yaw difference since the last packet, `NextFacingCrossing`), pitch crossings (steered, non-keyboard), and the 500 ms
  heartbeat while moving/falling/ascending, each with interpolated position (exact at step ends); up to 256 per step; (7) a landing sends
  FALL_LAND at the step end with `FallMs = Body.FallMs` (reports inside a landing step are flagged FALLING, and none is sent at the very
  end before FALL_LAND, comment :285-287: otherwise the server took a heartbeat as the fall's new top and a 30 yd fall cost nothing),
  (8) a SWIMMING flag change sends START_SWIM/STOP_SWIM. Any refused `Send` aborts the rest of the tick (`return`), after
  `Send` has already put the body back where the server holds it.
- `Send` (:100): counts, stamps `_lastSendMs`; a refusal calls `TakeFromServer(link.State())`, keeps cadence, returns false.
- `Finish` (:400): if the body is more than 0.01 yd or 0.001 rad from the last report, one HEARTBEAT is sent so the last stretch is credited.

`ReportCadence.h` (namespace `Animus::Movement::Cadence`): opcode constants (START_FORWARD 0x0B5 ... START_DESCEND 0x3A7), `HEARTBEAT_MS 500`,
`HEARTBEAT_FLAGS 0x00c0100f`, `MOUSE_FACING_THRESHOLD 0.1`, `Changes` (:80; ordering of the client's checks), `NextFacingCrossing` (:120;
the raw unwrapped difference means crossing 0/2pi always sends), `NextPitchCrossing` (:146). (`HeartbeatDue` and `Changes`'s unused `jumped`/`landed` parameters were deleted 2026-10-08.) Tests:
`ClientTest.cpp` (29 tests: cadence independent of tick, refusal handling, knockback, orders queue across threads, flag stripping
parity, turn to camera), `ReportCadenceTest.cpp` (2 tests).

Reviewer notes: (a, b) fixed 2026-10-08: `HeartbeatDue` (the heartbeat is computed inline in Tick) and `Changes`'s `jumped`/`landed` parameters were deleted. (c) The nested
`Start` inside `Tick` passes `nowMs - diffMs`. (d) `Client` namespace names collide conceptually: `Animus::Movement::Client` (this
class) vs `Animus::Client` (orders namespace) vs `ClientMovement::Client` (core's session interface) vs `Replay::Report`/`Client::Report`.

## ClientOrders (ClientOrders.h)

Namespace `Animus::Client`, though it lives in Movement/. Contents: opcode constants (server-to-client orders: root, unroot, nine force
speed changes, knockback, can-fly, water walk, feather fall, hover, gravity, teleport ack, time sync; client acks), `MoveFlag` bits,
`OrderKind` (16 kinds incl. `Teleport`), `SpeedType` (the core's `UnitMoveType` order), `Order`, `Answers(opcode)` (a cheap switch, called for
every packet to every session), `Decode(opcode, data, size, self)` (:244; packed GUID must equal `self`, then counter, then per-kind payload;
`SMSG_FORCE_RUN_SPEED_CHANGE` has an extra `u8`), `AckOpcode`, `Applies` (can-fly, water walk, feather fall, hover set the "applied" flag),
`FlagsAfter` (:364: Root clears MASK_MOVING and sets ROOT; Knockback clears ROOT and sets FALLING; CanFly clears FALLING; etc.),
`Clock` (steady clock starting at 1,000,000 ms so a time is never 0), `Inbox` (mutex + vector, `Push`/`Drain`).
Tests: the header (line 38) claims `tests/ClientOrdersTest.cpp`; no such file exists. `ClientTest` covers `Inbox` ordering
(`OrdersQueueAcrossThreads...`), `FlagsAfter` with `SanitizeFlags` (`TheServerStripsTheSameFlagsFromEveryReport`). `Decode` and `Clock` have
no direct test (grep for `Decode(` in src/test finds only `Capture::Decode`). Note `Clock` is referenced as used by "CompanionClient" (a
companion client that no longer exists in this tree); UNVERIFIED: grep for a `Clock` user outside this header finds none in Animus/.

## FlagRules (FlagRules.h)

`SanitizeFlags(flags, FlagFacts)` (:65) is `WorldSession::ReadMovementInfo`'s REMOVE_VIOLATING_FLAGS rules in order: ROOT stripped
unconditionally from a client's report (so `Client::FlagsOf` setting ROOT on a rooted client is dropped by the server on the bot path
too; the core's root rests on `SendMoveRoot`); HOVER without aura; opposite pairs (ASC/DESC, LEFT/RIGHT, STRAFE_L/R, PITCH_UP/DOWN,
FWD/BACK) both stripped; WATERWALKING without aura or ghost; FALLING_SLOW without feather fall aura; FLYING|CAN_FLY without privilege or
fly aura; FALLING stripped when CAN_FLY|DISABLE_GRAVITY; SPLINE_ENABLED without a running spline. Used by
`WorldSession::SanitizeMovementFlags` (`Server/WorldSession.cpp:1131-1149`), which both real packets and `PlayerLink::Apply`
(`PlayerLink.cpp:90`) call. Tested via `ClientTest.TheServerStripsTheSameFlagsFromEveryReport` (no separate FlagRules test).

## PlayerLink, LinkMemory

`PlayerLink::Apply(Report)` (PlayerLink.cpp:71): false if the bot is not in the world or is being removed. Builds a `MovementInfo` (flags2 0,
no transport, pitch, fall time, jump info), sanitises flags, then per opcode: `MSG_MOVE_TELEPORT_ACK` while `IsBeingTeleportedNear()` calls
`HandleMoveTeleportAck` and returns true (movement info not applied); root/unroot ack that matches the current state returns true without
applying; the toggle acks set `AnticheatSetCanFlybyServer` and copy flags into `m_movementInfo`; speed acks decrement
`m_forced_speed_changes[type]`. Then `ClientMovement::Apply(bot, info, bot, bot, opcode, BotMovementClient)` (a `ClientMovement::Client`
whose `Synchronize` stamps `GameTime` ms and whose `Kick` only logs). A refusal is counted per reason, logged at most once a minute per reason
(`REFUSAL_LOG_MS 60000`), and after `UNSTICK_AFTER 20` consecutive `InvalidPosition` refusals the seat is `NearTeleportTo` its last good
position (the only server-side move of a bot by this code; it is a bug-recovery path, `Unsticks` counts it). After success: the set-can-fly
pending change is cleared; knock-back ack re-enables teleport; a FALL_LAND adds the lost health share to `LinkMemory::FallDamage`; a death
during the report counts a `FallDeaths` (and `VoidDeaths` when `PLAYER_FLAGS_IS_OUT_OF_BOUNDS`, i.e. the core's kill below the map floor);
`Relay` sends what each handler sends to nearby players, only `if (ForgeCore::HasClients())` (`:175`); good position is remembered.
`State()` (:261): position/yaw of the bot and `Imposed` = not in world, being teleported, ROOT/STUNNED/CONFUSED/FLEEING unit state,
DISABLE_MOVE, unfinished spline, charmed, or dead-and-not-ghost.
`QueueOrder` (:284) counts `OrderPackets` and pushes a decoded order.
`LinkMemory` is per-seat state (SeatState::Link): good position, invalid streak, `FallDamage` (sum of health shares), `FallDeaths`,
`VoidDeaths`. Fall damage itself is the core's `Player::HandleFall` via `ClientMovement::Apply`; the controller only supplies `FallMs`.
No unit test for `PlayerLink` (needs a core `Player`); behaviour is covered by live runs.

## CastWatch

`ScopedCastWatch(session)` installs a thread-local `CastWatch*`; `Spell::SendCastResult` (the sim branch, `Spells/Spell.cpp:4685`) calls
`NoteCastFailed(session, packet)`, which, if a watch for that session is open, counts a failure and parses spell id (bytes 1-4 LE) and result
(byte 5) of SMSG_CAST_FAILED (needs `size >= 6`). Users: `Character/EntityActions.cpp:272,292,441`. Not a movement unit; it lives here because
it was added in the same plan. No test.

## ControllerCost, Seek

`ControllerCost::Ns/SeatTicks` (relaxed atomics, `Add(ns, seatTicks)`), one add per `SubTick` call. `Seek(body, x, y, tuning)` returns a
`ControlState`: nothing within `ArriveYards 2`; else `TurnRate = clamp(error/0.25 s, +-2pi)`, `Forward=1` iff `|error| <= 45 deg`.
Used by `Encounters/PartyFollowEncounter.cpp:428` for the party-follow leader (the M4 leader, the one scripted mover left; the header says
"scripted actors only" and the tension with principle 14 is the owner's). Tests: `SeekTest.cpp` (2), plus `SeekEncounterTest/SeekFlagTest`
which test the encounter, not this file.

## Capture and Replay

`Capture` is the human-play capture format's move stream (spec in `apps/forge/python/animus/human/FORMAT.md`; see
py-human-and-misc.md and file-formats.md). Frame: `u16 type, u16 length`, little-endian, gzip members. Header record (type 0, 100 bytes):
`"ANCAP"` padded to 8, `u16 version=1`, `u16 stream=2`, `u64 openedMs`, 40-byte producer, 40-byte realm tag (cpp:138-). Move (type 10,
`MOVE_BYTES = 73`): `u64 Ms, u64 Player, u32 ClientMs, u16 Opcode, u32 Flags, u16 Flags2, f32 X,Y,Z,O,Pitch, u32 FallMs, f32 JumpZSpeed,
JumpSin, JumpCos, JumpXYSpeed, u32 Map, u8 Source` (8+8+4+2+4+2+20+4+16+4+1). Speeds (type 11, `SPEEDS_BYTES = 52`): `u64 Ms, u64 Player`, nine
f32 (Walk, Run, RunBack, Swim, SwimBack, Flight, FlightBack, TurnRate, PitchRate). `Decode` skips unknown types, accepts longer records, stops
at a truncated tail (earlier records stand), errors on bad magic (`length < 12` or not "ANCAP") or no header. `ReadFile` reads all gzip
members. (`FromReport`, which filled `Ms = ClientMs = report.TimeMs`, had no caller and was deleted 2026-10-08.)

`Replay` feeds recorded packets' flags (as `ControlsOf`) and facing through the controller from the first packet's position and compares
positions: `Follow` (drift list), `Run` (segments every 5 s on ground packets, drift at 1/2/5/10 s all and flat, jump launch/apex/landing,
step-ups, steep descents, wall disagreements, and "calibration" measures of each client constant vs recording). Used only by
`forge controller replay` (`cs_forge.cpp:822-886`) and `forge controller record` (`:763`). `Run`'s `speedsAt` applies every Speeds record with
`Ms <= packet.Ms` and always the first (Replay.cpp:178). Tests: `ReplayTest.cpp` (round trip of every field, the controller replaying itself
with zero drift, a displaced recording drifting). No test with a real human recording is in the tree; the C6 calibration remains open (see
below).

## Config keys

None are read in Movement/. Related: `AnimusForge.Cluster.*`, `AnimusForge.Vision.*` are elsewhere; `CurriculumTuning::Seek` supplies the
seek stage's costs, not `SeekTuning` (that struct, `Seek.h:31`, takes defaults). See cpp-tuning-keys.md.

## Observed issues

1. `Speeds::TurnRate` and `PitchRate` are read from the unit (`UnitBody.cpp:34-35`) but never limit stepping; the policy sets `TurnRate` up to
   360 deg/s (`MoveControls.h`) against the client's 180 deg/s keyboard rate. Only Replay/Capture use them. (A mouse is not rate-limited in
   the real client, but the comment in `Speeds` implies the unit rate.)
2. `PlayerController.cpp:125-127`: the doc comment of `SweptMove` sits above `FootprintFloor` (misplaced).
3. `ClientOrders.h` is in Movement/ but its namespace is `Animus::Client`; its header cites a nonexistent `tests/ClientOrdersTest.cpp` and a
   `CompanionClient` that is not in the tree.
4. (fixed 2026-10-08) `ReportCadence::HeartbeatDue` and the `jumped/landed` arguments of `Changes` were deleted.
5. Constants the code itself marks as interpreted and not yet confirmed by a recording (C6): `STEP_UP`'s role (`PlayerController.h:61-64`),
   `KEYBOARD_TURN_WHILE_MOVING`'s condition (:53-55), `FLOAT_DEPTH` (:75), the vertical-alone share (`PlayerController.cpp:85-86`). No
   recording-based test exists.
6. `Client::Tick` returns from the middle of the sub-step loop on any refused report (:204 etc.), skipping the rest of the tick's
   steps; the body was already put back on the server's by `Send`.
7. `UnitBody.h` comment says SlowFall covers levitate; the code tests only FEATHER_FALL and HOVER auras.
8. `FlagsOf` sets ROOT for a rooted client but the server strips a client's ROOT (`FlagRules.h:56-57`); intentional, but the bot path leans on
   `PlayerLink::Apply`'s ack handling instead.
9. `Replay::Report` and `Movement::Report` (Client.h) are different types with the same name in sibling namespaces.
10. (`Protocol.h` said images were 4 bytes a pixel while the code was five; since protocol 26 they are four. See cpp-vision.md.)
11. `Seek.h` scripted mover for the M4 leader: principle 14 forbids scripted players; the known-fact list records it as still scripted.
