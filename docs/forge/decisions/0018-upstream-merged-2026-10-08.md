# 0018: Upstream AzerothCore merged into the forge (2026-10-08)

**Decision.** The owner chose to merge upstream now ("Merge now. Tell me what the conflicts are") instead of freezing
the fork. `forge` now contains every commit of upstream `master` up to `bce7ed5a6` (2026-10-07), 494 commits.
Local `master` is a plain mirror of `upstream/master` (it was the fork's old import of 2026-09-15).

**Why now.** No training was running and the cluster was idle; the gap was growing by about 90 commits a week.

**Conflicts (6 files, 11 hunks) and how they were resolved.**

| File | Resolution |
|---|---|
| `Spell.cpp` (4) | upstream's `unitCaster` (`Spell::m_caster` is now a `WorldObject*`); the forge's cast hooks receive `unitCaster` (null for gameobject casts, which `EnvPool::Record*` ignore); the `HasClients` guards in `SendSpellStart`/`SendSpellGo` kept |
| `WorldSession.cpp` (2) | the forge's `m_simSession` combined with upstream's `_headless` and `OnPlayerCanMarkAccountOffline` |
| `Unit.cpp` (1) | followed upstream (`SendSpellMiss` moved to `WorldObject`) and re-added the guard there |
| `Player.cpp` (1) | upstream's `applyProcCooldown`, then changed to use `GameTime::Now()` (commit 108b51fb5) |
| `PlayerUpdates.cpp` (1) | the forge's no-persist early return kept; upstream's `IsLoggingOut` rename applied |
| `AGENTS.md` (3) | the forge's version (the e2e lines dropped) |

Modify/delete: upstream's `e2e/` suite, its workflow and its docs stay deleted (the fork deleted them earlier); `Main.cpp`
stays deleted (`ForgeMain.cpp` replaces it).

**Silent regressions found and fixed** (they merged cleanly and some would not have compiled or would have misbehaved):
`PCQueue::Reset()` defined twice (the forge copy removed; upstream now calls it from `DatabaseWorkerPool::Open`);
the `HasClients` guard lost from `WorldObject::SendSpellMiss` and `SendSpellNonMeleeDamageLog` (re-added);
upstream's equip-cooldown code used `steady_clock::now()` instead of the sim clock (fixed, 108b51fb5); upstream's new
creature cooldown packet to the controlling player had no guard (fixed, 721aed80a).

**Verified:** `worldserver` and every other target build and link with 0 errors (about 4 minutes with a hot ccache,
`-DBUILD_TESTING=OFF`). Not verified: the sim was not started, and nothing was run against a database.

**Open, accepted as is:**
- `m_simSession` stays (equivalent to `IsHeadless()` today; replacing it touches about 6 sites).
- `ForgeMain` does not call `SaveSessionEnd`, so the uptime row's end time stays NULL and each start logs "Previous
  session did not shut down cleanly" (the login DB is sealed at shutdown, so the write would be dropped anyway).
- Upstream's `OnPacketSent` runs for packets dropped to sim seats (one empty-vector test per packet); one weather packet per
  zone change is unguarded; upstream's `OnModuleDatabases*` hooks are not ported (no module has its own database pool).

**Consequence for the docs.** `reference/01-forge-core-delta.md` was measured against the old merge-base 37de65eb0. Its
list of changed upstream files is still the right list of forge edits, but its claim that `PCQueue::Reset` has "no caller"
is stale, and `git diff master..forge` is now the correct way to see the delta (master is upstream).
