# 2. The forge core

The forge core is AzerothCore 3.3.5a edited in place to be a headless, fixed-tick simulator host. Everything about how it
differs from upstream, section by section with the rationale and the merge risk, is in
[reference/01-forge-core-delta.md](reference/01-forge-core-delta.md). How its tick drives the sim is in
[reference/00-architecture.md](reference/00-architecture.md).

What is still true of the old chapter that stood here (all re-verified against the code, see the delta document for
the citations):

- The binary is always a simulator: `ForgeMain.cpp` is the only `main()`; there is no switch back to the stock startup.
  There is no exception: the playtest mode was deleted on 2026-10-08.
- The world steps by a fixed diff, never the wall clock (`ForgeUpdateLoop`), and the game clock follows it
  (`GameTime::AdvanceGameTimers`).
- Nothing is sent to clients; packet builders return early unless `ForgeCore::HasClients()`.
- Bots are never persisted; after startup the three MySQL pools are sealed.
- The console starts only when stdin is a terminal; SOAP is off unless `SOAP.Enabled`.
- Linux only.

What the old chapter claimed and the code no longer does: the "replacement member in `Forge/Forge<Thing>.cpp` with the
stock body kept as dead code" pattern (no such files exist; upstream functions are edited in place), `Main.cpp` being
"excluded but not edited" (it is deleted), `World::Update` dropping the auction house, LFG, outdoor PvP and world
states (it keeps them), the stock four-step map round robin (gone), `PCQueue::Reset` in the map updater (unused),
`CoreHooks` seams (gone; the core calls `Animus::Hooks` directly), `maxTicks`. The full list with citations is the first
section of the delta document.

Running it in Docker is in [07-operations.md](07-operations.md); the compose services and `forge.sh` are summarised in
section I of the delta document.
