# 2. The forge core

The `forge` branch of `Moloch17/azerothcore-wotlk` is the training host. It is AzerothCore 3.3.5a with a new `main()`,
a fixed-step world loop, a simulated clock, and the client and persistence paths removed. Its scope is deliberately
narrow. It knows nothing about bots, rewards or learners. It provides a world that runs as fast as the CPU allows,
never writes bot state to MySQL, and gives modules a small number of seams.

## 2.1 Rules the fork follows

**No gating.** No `Sim.Enabled` switch exists and no path falls back to stock behaviour. The binary is always a
simulator. A config flag would add branches that nobody runs and hide what the binary really does.

**Replace, don't rewrite.** When the fork changes a stock function substantially, it:

1. writes the replacement as a separate member in `src/server/game/Forge/Forge<Thing>.cpp` (for example
   `World::ForgeUpdate`),
2. declares it next to the original in the header, with a `// Forge:` comment,
3. adds `return ForgeUpdate(diff);` as the first statement of the original, and leaves the rest of the original body
   untouched.

The stock body becomes dead code but stays byte-for-byte identical, so upstream edits to it rebase without conflicts.
Defining a member of `World` or `MapMgr` in another translation unit also grants access to private state (`_timers`,
`i_maps`) without friend declarations or accessors.

**Small edits are marked in place.** One-line changes (an early `return;` in a packet builder, a clock read switched to
`GameTime`) sit in the stock file under a `// Forge:` comment that gives the reason. To list every divergence, run
`grep -rn "Forge:" src/`.

**`Main.cpp` is excluded from the build, not edited.** `src/server/apps/CMakeLists.txt` removes
`worldserver/Main.cpp` from the worldserver sources, and `worldserver/ForgeMain.cpp` defines `main()` instead. Upstream
can change `Main.cpp` freely without ever conflicting.

## 2.2 Startup and shutdown (`ForgeMain.cpp`)

`main()` keeps only what a headless simulator needs.

| Kept | Why |
|---|---|
| Config loading (`worldserver.conf`, module configs) | Database connection info, and the hundreds of values `SetInitialWorldSettings` reads |
| Module config, script and database loading | `DatabaseLoader` takes `AC_MODULES_LIST`, and module SQL updates matter for the schema |
| OpenSSL thread setup and PRNG seeding | BigNumber and crypto are linked and used even without a listener |
| `realm.Id.Realm = 1` | About 22 places scope GUIDs and account state by realm id |
| An IoContext with one thread | Only for SIGINT/SIGTERM handling (`World::StopNow`) |
| The console (`CliThread`) | Operators control training from it |
| SOAP, when `SOAP.Enabled` is set | The console's commands for a caller that has no terminal -- the dashboard's stage controls. Off by default, so the sim still opens no listener unless asked; it runs the same handler and the same `SEC_ADMINISTRATOR` check a typed command does |

| Removed | Why |
|---|---|
| World socket listener, Remote Access | Bots are in-process and there are no clients or remote operators |
| Metrics, AppenderDB, PID file, banner | Operational surface the simulator doesn't use |
| FreezeDetector | It aborts the process when a tick is slow, and a training tick may be slow |
| ToCloud9 / sidecar cluster plumbing | One process, never clustered |
| SecretMgr (TOTP), realmlist reads and writes, `WORLD_UPD_VERSION` stamp | Only the login flow and client handoff use them |
| Windows service logic | The simulator is Linux-only |

**The console starts only when stdin is a terminal** (`isatty(STDIN_FILENO)`). When the console reads end of file, the
server stops. A container started without a TTY or a batch job would therefore shut down immediately, so in those
cases the console is skipped and the server keeps running. Docker gives the worldserver a TTY (`stdin_open`, `tty`),
and `./forge.sh attach` connects to it.

**Shutdown order.** When the loop exits, the console thread is joined, then the IoContext stops, then
`sScriptMgr->OnShutdown()` runs (mod-animus-forge stops the learner here, and the learner saves `latest.pt`). The
remaining teardown runs through scoped handles in reverse order: battlegrounds, OutdoorPvP and maps are unloaded, then
the databases close, then scripts unload, then OpenSSL cleans up.

## 2.3 The fixed-tick loop

```cpp
uint32 const decisionMs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.DecisionMs", 250));
uint32 const ticksPerDecision =
    std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.TicksPerDecision", 1));
uint32 const tickMs = std::max<uint32>(1, decisionMs / ticksPerDecision);
while (!World::IsStopped())
{
    ++World::m_worldLoopCounter;
    sWorld->Update(tickMs);       // fixed diff, never measured from the wall clock
}
```

- **The diff is constant.** Stock AzerothCore measures how much wall time passed and sleeps to hold a minimum tick.
  The forge never sleeps and always passes the same diff, so one iteration advances the world by exactly `tickMs` of
  game time, however long the CPU took.
- **The tick is the decision cut into `TicksPerDecision`.** The loop reads both of mod-animus-forge's keys. At the
  default of 1 a world update is an agent decision and everything lines up without sub-stepping. Above 1 the world
  runs several times between decisions, which is how movement gets finer than 250 ms: the module holds the decision
  back until the count comes round, so the intervening ticks move splines, auras and the fight and nothing else.
  Integer division truncates, and the module rounds `DecisionMs` down to a whole number of ticks at startup, so both
  sides land on `floor(DecisionMs / TicksPerDecision)`. If the module sees a different diff (a worldserver built
  before this change, or a conf edit without a restart) it logs an error once and asks for `./forge.sh --build`.
- **Idle cost is the module's concern.** An idle or paused sim would spin a core at full speed. mod-animus-forge sleeps
  50 ms per tick while nothing runs.
- **Synchronous query warnings are on** for all three databases during the loop, so an accidental sync query in a hot
  path shows up in the log.
- `maxTicks` (a `constexpr`, 0) is a hook for batch runs that should stop after N ticks.

Before the 64-bit clock work, the loop also stopped the server when the game clock approached UINT32_MAX minus
`infinityCooldownDelay` (about 19.7 game days). At 100 ms per tick and thousands of ticks per second,
that limit arrived within hours. The budget check is gone now that the timestamps it protected are 64-bit (see 2.4).

## 2.4 The game clock

Stock `GameTime::UpdateGameTimers()` refills four cached values from the wall clock every tick: `GameTime` (seconds),
`GameMSTime`, `GameTimeSystemPoint` and `GameTimeSteadyPoint`. Cast bars, aura durations and swing timers advance by
the tick diff, while cooldowns, the GCD, proc cooldowns and respawns compare against those cached wall-clock values. At
simulation speed a 10-second cooldown would last many thousands of game seconds.

`GameTime::ForgeAdvanceGameTimers(diff)` (`Forge/ForgeGameTime.cpp`) replaces that call inside `World::ForgeUpdate`:

- On the first call it seeds all four values from the wall clock, so absolute times loaded from the database (respawn
  times, instance saves) are measured against the real start date.
- After that it only adds `diff` to each value. Whole seconds are derived from the system time point, so sub-second
  remainders carry across ticks.
- `UpdateGameTimers()` itself is unchanged. The sim never calls it, and its unit tests keep wall-clock semantics.
- The four variables are defined in `GameTime.cpp` with external linkage. `ForgeGameTime.cpp` declares them `extern`
  rather than editing the upstream file. If upstream ever makes them `static`, the build fails at link time instead of
  silently misbehaving.

**`getMSTime()` stays on the wall clock.** Database timing, logging and load measurements need real time. Gameplay code
that read the raw clock was converted to game time in place:

| File | Change |
|---|---|
| `Entities/Player/Player.cpp` | `HasSpellCooldown`, `HasSpellItemCooldown`, `GetSpellCooldownDelay`: `getMSTime()` to `GameTime::GetGameTimeMS()`; item proc cooldown: `steady_clock::now()` to `GameTime::Now()` |
| `Entities/Unit/CharmInfo.cpp` | `GlobalCooldownMgr::GetGlobalCooldown`: to game time |
| `Entities/Unit/Unit.cpp` | `GetProcAurasTriggeredOnEvent`: to `GameTime::Now()` |
| `Spells/Auras/SpellAuras.cpp` | `Aura::ResetProcCooldown`: to `GameTime::Now()` |
| `scripts/Spells/spell_paladin.cpp` | Sacred Shield internal cooldown: to `GameTime::Now()` |
| `scripts/Northrend/Ulduar/Ulduar/boss_xt002.cpp` | Two `getMSTime()` reads to game time |
| `scripts/.../boss_jeklik.cpp`, `scripts/Northrend/zone_howling_fjord.cpp` | `_scheduler.Update()` (wall clock) to `_scheduler.Update(diff)` |
| `common/Utilities/TaskScheduler.cpp` | `GetNextGroupOccurrence`: `clock_t::now()` to the scheduler's own `_now` |

These remain on the wall clock on purpose, because the sim drops them from the tick or only clients use them: session
time sync, movement client sync, LFG, arena spectator, WorldState, GameEventMgr, Battlefield, transport first-departure
sync, scourge invasion, midsummer, `cs_mmaps`, UpdateTime.

**64-bit timestamps.** Absolute timestamps compared against game time are now `uint64` milliseconds,
so no clock budget is needed:

- player and creature spell cooldown ends (`SpellCooldown::end`, `CreatureSpellCooldown::end`), including the
  "infinite" ones that add `infinityCooldownDelay`
- creature school lockouts (`m_ProhibitSchoolTime`)
- gameobject cooldowns (`m_cooldownTime`)
- Sanctuary (`m_lastSanctuaryTime`)
- Strand of the Ancients demolisher respawns
- the Eclipse and turkey-marker script timers
- pet and player cooldown saving and initialisation

Relative timers compared through `getMSTimeDiff` stay 32-bit because they are wrap-safe. Battleground queue timestamps
and client packet fields stay as they are, because the sim runs neither queues nor clients.

## 2.5 The world tick (`Forge/ForgeWorld.cpp`)

`World::Update` returns `ForgeUpdate(diff)`. The replacement keeps:

1. **Game time and shutdown timer.** This is stock `_UpdateGameTime` with the clock advanced by the fixed diff.
2. **Interval timers.** All of them are stepped so anything that reads one sees a sane value, but only
   `WUPDATE_PINGDB` is acted on (MySQL keep-alive, so a long run doesn't lose its connection pools to `wait_timeout`).
3. **`sMapMgr->Update(diff)`**, the simulation itself.
4. **`sBattlegroundMgr->Update(diff)`**, because battlegrounds are instances the sim may run.
5. **`ProcessQueryCallbacks()`**. Without it the async callback queue grows forever and character loads never
   finish.
6. **`sInstanceSaveMgr->Update()`**, instance reset bookkeeping.
7. **`ProcessCliCommands()`**, the console queue. It runs before the module hook, so a command the module defers takes
   effect on the same tick.
8. **`sScriptMgr->OnWorldUpdate(diff)`**, where mod-animus-forge does all its work.

It drops these, with reasons:

| Dropped | Reason |
|---|---|
| `sAuctionMgr`, `sLFGMgr` (x2), `sOutdoorPvPMgr`, `sWorldState`, `sBattlefieldMgr` | Every-tick calls the sim has no use for, and most of the speedup in an unthrottled loop |
| Metrics, `sWorldUpdateTime`, ToCloud9 | Never initialised, or telemetry only the sidecar reads |
| `sWorldSessionMgr->UpdateSessions` | No session is ever registered there. A socketless session registered there would be deleted, and its player saved, on the next update. Bots are driven by `Map::Update` through `MapSessionFilter` instead |
| `WUPDATE_5_SECS`, who list, uptime, clean DB, autobroadcast | They advance on game time, so at simulation speed the 5-second tasks would issue hundreds of DB statements per wall second |
| Quest, battleground, calendar and guild resets; mail expiry | Wall-clock deadlines. Dropped to reduce surface area rather than for speed |
| `WUPDATE_EVENTS` (GameEventMgr) | It changes which creatures exist. That is a content decision; to restore it, add the block back |
| `DynamicVisibilityMgr::Update` | See below |

**Visibility is pinned to tier 0.** `DynamicVisibilityMgr::visibilitySettingsIndex` starts at 0 and only rises once the
session count reaches 500. The sim registers no sessions, so skipping the update pins small-realm settings: 300 ms
visibility notify, 150 ms AI notify, a required move distance of 1.0 squared.

## 2.6 The map tick (`Forge/ForgeMapMgr.cpp`)

`MapMgr::ForgeUpdate` reproduces the stock four-step round robin exactly: continents, then battlegrounds and arenas,
then dungeons, then none. Each step decides whether a map gets its accumulated timer (a full update) or only a
session update, so bots see the same update cadence as on a stock server. It also drops the LFG update.

The replacement **skips non-instanceable maps that have no players**. An instance-only sim still creates every
continent at startup, and without this they would tick forever with nobody on them.

`MapInstanced::ForgeUpdate` handles the one case that must not be "optimised". A `MapInstanced` is a container keyed by
map id. It never holds players itself, because they are in its child instances. So:

- containers are never skipped,
- `CanUnload(t)` runs for every child on every tick, since it counts down the unload timer and skipping it would leak
  every finished instance,
- only an empty child's `Update()` is skipped (it is waiting to unload, or for bots to arrive).

**The map update pool can be restarted.** `forge bench` switches `MapUpdate.Threads` between trials by deactivating
the `MapUpdater` pool and activating it again at the next count. Stock `MapUpdater::deactivate()` leaves its
cancellation token set and its queue cancelled, so a reactivated pool would look running while dropping every
request and the world thread would wait forever for a decision's map updates. `MapUpdater::activate` now clears the
token and calls `PCQueue::Reset` (`common/Threading/PCQueue.h`) before starting its workers.

## 2.7 No clients

No client sockets exist, so everything that only builds packets returns immediately. Each site carries
`// Forge: no client sockets exist in the sim host`.

- **Object updates.** `Map::SendObjectUpdates` becomes `Map::ForgeSendObjectUpdates` (`Forge/ForgeMap.cpp`). Stock
  code builds a values-update block for every changed object and every player that can see it: roughly changed
  objects times visible players of wasted work per tick, because bots see each other and health changes on every regen
  tick. The only state those builders change is the object's update mask. The replacement drains the queue and calls
  `ClearUpdateMask(false)` directly, so field-change tracking still works.
- **Create and destroy blocks.** `Object::BuildCreateUpdateBlockForPlayer`, `SendUpdateToPlayer`,
  `BuildOutOfRangeUpdateBlock`, `DestroyForPlayer`, the `Player` and `Bag` overrides, and
  `Player::GetInitialVisiblePackets`.
- **Broadcasts.** `WorldObject::SendMessageToSet` and its variants, the `Player` versions, and
  `Player::SendDirectMessage`.
- **Combat logs.** `Unit::SendSpellNonMeleeDamageLog` (both), `SendPeriodicAuraLog`, `SendSpellMiss`,
  `SendSpellDamageResist`, `SendSpellDamageImmune`, `SendAttackStateUpdate` (both), `SendHealSpellLog`,
  `SendEnergizeSpellLog`.
- **Spells and auras.** `Spell::SendSpellStart`, `Spell::SendSpellGo`, `AuraApplication::ClientUpdate` (the flag reset
  stays).
- **Movement.** `MoveSplineInit::Launch` initialises the spline (the actual movement) and returns before building
  `SMSG_MONSTER_MOVE`. `MoveSplineInit::Stop` does the same.
- **Groups.** `Player::SendUpdateToOutOfRangeGroupMembers` keeps resetting its masks but no longer builds the party
  stats packet.

## 2.8 No persistence

At training speed a bot is rebuilt many times per wall second. Any write per rebuild outpaces MySQL and the async queue
grows without bound.

- **Player saves.** `Player::SaveToDB` (both overloads) returns immediately unless `create` is true. It also zeroes
  `m_nextSave`, which switches off the periodic save branch in `Player::Update` for good. Without this,
  `PlayerSaveInterval` measured in game time would save every bot several times per wall second.
  `Player::UpdateAdditionalSaves` drops queued partial saves (inventory, quests, achievements).
- **Achievements.** Every `AchievementMgr` entry point returns immediately (update, criteria, timed achievements,
  completion, save, packets). Bots don't use achievements.
- **Pets.** `Pet::SavePetToDB` returns after its own guards, before any database work. `Player::RemovePet` calls it
  with `PET_SAVE_AS_DELETED` whenever a bot with a pet is torn down, which is a large share of the characters
  rebuilt each episode: without this it opened a transaction for auras, spells and cooldowns and then a second one
  through `Pet::DeleteFromDB`, about 1.2 transactions per decision. Nothing reads any of it back -- the curriculum
  summons a pet outright rather than loading one. The aura wipe a stable save does is kept, since it is the one
  effect in the function that is not a write. `Pet::DeleteFromDB` itself is untouched: its other caller is
  character deletion, which the sim never reaches.
- **Sim sessions.** `WorldSession::SetSimSession(true)` marks a session whose account and characters
  exist only in memory. On such a session, logout skips marking the account's characters offline
  (`CHAR_UPD_ACCOUNT_ONLINE`), the destructor skips the `account.totaltime` update, and
  `InstanceSaveMgr::PlayerBindToInstance` skips the instance bind rows. The bind statements are now built
  only when executed, because an unexecuted prepared statement would leak.
- **Sim groups.** `Group::SetSimGroup(true)` (set before `Create`) makes a group that works like a
  normal party or raid (membership, party spells, shared kills) but has no group or member rows and no character
  cache entries. Joining never resets instance binds, and leaving or disbanding never starts homebind timers or
  deletes instance save data. `Group::IsPersisted()` is the single test for "not a battleground, battlefield or sim
  group". In cluster mode a sim group always disbands locally.

## 2.9 Seams for modules

The core provides a few small APIs that a stock core lacks. animus-lib reaches them through `CoreHooks` function
pointers, and mod-animus-forge fills them in at load (`AddSC_animus_forge`), so the library still builds on a stock
core.

| Core API | animus-lib seam | Used for |
|---|---|---|
| `WorldSession::SetSimSession(bool)` | `CoreHooks::MarkSimSession` | Every bot session (`BotFactory::Create`) |
| `Group::SetSimGroup(bool)` | `CoreHooks::MarkSimGroup` | The party stage's group, rebuilt every episode |
| `rand_seed(uint32)` in `common/Utilities/RandomSeed.h` | `CoreHooks::SeedRandom` | Seeded evaluation episodes |
| none | (no seam) | Bots never run `LoadFromDB`, which normally attaches the social list. The core has no setter for it, so `BotFactory::Create` assigns the private `Player::m_social` through an explicit template instantiation, which works on any core |

`rand_seed(seed)` calls `SFMTRand::Seed` on the calling thread's generator, so `urand`, `irand`, `frand` and
`rand_norm` restart from `seed`. Seed 0 reseeds from `std::random_device`. Other threads are unaffected. Resets run on
the world thread, so reseeding the world thread makes an episode's setup reproducible while combat rolls on map threads
stay random.

## 2.10 Running it: Docker and `forge.sh`

The branch's `docker-compose.yml` defines the Compose project `ac-animus-forge`. Every container, volume, network and
image carries that prefix, and host ports differ from stock, so it can run beside a normal AzerothCore checkout.

| Service | Profile | Role |
|---|---|---|
| `ac-database` | default | MySQL 8.4, published on `127.0.0.1:13306` only (the default root password isn't meant for a network) |
| `ac-client-data-init` | default | Fills the `ac-animus-forge-client-data` volume with maps, vmaps and mmaps |
| `ac-worldserver` | default | The training server: the dev image running the worldserver built from the bind-mounted source, with a TTY console, `init: true`, `restart: "no"`, `shm_size: 8gb` and TensorBoard on `127.0.0.1:16006` |
| `ac-dev-server` | `dev` | Same image and build volumes, no ports. For VS Code (`.devcontainer`, `shutdownAction: none` so closing VS Code doesn't stop training) or a shell |
| `ac-db-import`, `ac-authserver` | `stock` | Stock images, not needed for training (the worldserver runs `Updates.AutoSetup` itself) |
| `ac-tools` | `tools` | Client data extractors |

`ac-worldserver` runs `apps/docker/forge-worldserver.sh` on every start:

1. **Build if needed.** It runs `acore.sh compiler configure` and then `compile` (so added and removed source files
   are picked up) when `env/dist/bin/worldserver` doesn't exist, or when
   `./forge.sh --build` left `env/dist/.forge-build`. The request file is removed only after a successful build.
2. **Restore config files.** It copies mod-animus-forge's `.conf.dist` into `env/dist/etc/modules/` if it is missing,
   and creates any missing `.conf` from its `.dist`. Existing files are never overwritten.
3. **Prepare the Python venv.** `apps/docker/animus-venv.sh` checks `modules/mod-animus-forge/python/.venv` on
   every start and installs whatever is missing: torch (from `ANIMUS_TORCH_INDEX_URL` if set, otherwise PyPI) and the
   learner with `[tensorboard,dev]`. The venv lives on the bind mount, so both containers share it. `ac-dev-server`
   runs the same script on start, so `pytest` works there.
4. **Start TensorBoard** on `<AC_ANIMUS_FORGE_OUTPUT_DIR>/runs` (`/azerothcore/var/animus-forge/runs` by default).
5. **`exec ./worldserver`**, in the foreground so its console is the container's terminal.

`forge.sh` wraps Compose:

| Command | Effect |
|---|---|
| `./forge.sh` | `docker compose up -d`, then attach to the console |
| `./forge.sh --build` | Leave the build request, recreate `ac-worldserver` (a running learner saves first), attach |
| `./forge.sh attach` | Attach to a running server's console. Detach with Ctrl+P Ctrl+Q. Ctrl+C stops the server |
| `./forge.sh dev` | Start `ac-dev-server` |
| `./forge.sh stop` | `docker compose stop ac-worldserver` (the learner saves a checkpoint) |

Plain `docker compose up` only streams logs, because Compose never forwards the keyboard. `docker compose up --build`
rebuilds images, never the worldserver binary.

Machine-specific settings go in a gitignored `docker-compose.override.yml`: GPU passthrough (commented NVIDIA and AMD
examples are in `docker-compose.yml`), a ROCm `ANIMUS_TORCH_INDEX_URL`, and
`CCUSTOMOPTIONS: "-DMODULE_MOD-ANIMUS=disabled"` so a checked-out mod-animus is never built into the forge.

## 2.11 Divergence map

| Area | Files | Nature |
|---|---|---|
| Entry point | `apps/CMakeLists.txt`, `apps/worldserver/ForgeMain.cpp` | `Main.cpp` excluded, new `main()` |
| World tick | `World/World.{h,cpp}`, `Forge/ForgeWorld.cpp` | Replacement member |
| Map tick | `Maps/MapMgr.{h,cpp}`, `Maps/MapInstanced.{h,cpp}`, `Forge/ForgeMapMgr.cpp` | Replacement members |
| Map update pool | `Maps/MapUpdater.cpp`, `common/Threading/PCQueue.h` | Restartable after `deactivate` |
| Object updates | `Maps/Map.{h,cpp}`, `Forge/ForgeMap.cpp` | Replacement member |
| Game clock | `Time/GameTime.h`, `Forge/ForgeGameTime.cpp`, plus the sites in 2.4 | New function, in-place conversions |
| 64-bit timestamps | Creature, CreatureData, GameObject, Pet, Player, Unit, Spell, BattlegroundSA, spell_druid, spell_generic | Type changes |
| Packets | Object, Player, PlayerUpdates, Bag, Unit, Spell, SpellAuras, MoveSplineInit | Early returns |
| Persistence | PlayerStorage, PlayerUpdates, AchievementMgr | Early returns |
| Sim sessions and groups | WorldSession, Group, InstanceSaveMgr | New flags, guarded writes |
| Seeding | `common/Utilities/RandomSeed.h`, `Random.cpp`, `SFMTRand.{h,cpp}` | New API |
| Scripts | TaskScheduler, boss_jeklik, zone_howling_fjord, boss_xt002, spell_paladin | Clock fixes |
| Ops | `docker-compose.yml`, `forge.sh`, `apps/docker/forge-worldserver.sh`, `apps/docker/animus-venv.sh`, `Dockerfile.dev-server`, `.devcontainer/devcontainer.json`, `design-doc.md` | New or rewritten |

## 2.12 Maintaining the fork

**Rebasing on upstream.** Replacement members and early returns leave upstream function bodies untouched, so
conflicts are rare. After a rebase:

- Check that upstream didn't add a new every-tick manager call to `World::Update` that the sim should also drop or keep
  (compare the dead body with `ForgeUpdate`).
- Check new `getMSTime()` or `steady_clock::now()` reads in gameplay code and decide whether they belong on game time.
- Check new packet builders on hot paths.
- Check new database writes on player logout, group membership or instance binds.
- If upstream made the `GameTime` globals `static`, the link step fails. Update `ForgeGameTime.cpp`.

**Adding a change.** Follow the pattern in 2.1. Don't add a config key to switch the change. Keep the upstream body.
Record what was dropped and why in the replacement file's header comment. That comment is the only place that explains
the fork's choices.

**Known gaps.**

- Some gameplay timers not listed in 2.4 may still read the wall clock. A clock shim behind `getMSTime()` would close
  that gap completely, but `getMSTime()` also serves logging and database timing, so it has not been done.
- The simulator is Linux-only (Unix sockets, `fork`/`exec` for the learner, `isatty`).
- Nothing in the core enforces "no database writes". A new upstream write path appears as a growing async queue or as
  synchronous query warnings in the log.
