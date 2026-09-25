# 6. Animus

Animus (`mod-animus`, repository `Moloch17/animus`) brings the forge's trained models to an ordinary AzerothCore realm
with real clients. It offers two features:

- **Class companions.** A player summons up to four characters of any class at their level, asking what each one
  should be able to do. The
  companions join the player's party and play their class models in combat.
- **The stage viewer.** A game master runs any curriculum stage exactly as the forge trains it, in their own instance,
  and watches the seats play their models, a scripted baseline, or random actions.

## 6.1 Constraints

- **Stock core only.** mod-animus uses public AzerothCore APIs and animus-lib. It never calls forge-only APIs and
  installs no `CoreHooks`. Anything that would need one belongs behind a seam in animus-lib.
- **No external dependencies.** Inference is the hand-written C++ MLP in animus-lib (`MlpPolicy`), not ONNX or
  LibTorch.
- **The model makes every combat ability choice.** No hand-written rotations are mixed in.
- **Observations are exactly the training observations.** Companions and stage seats go through the same
  `SeatEncoder` and blocks as training seats. Features only training could supply, such as the share of an episode's
  time limit left, aren't observed. What live play can supply is filled in the same way: a companion party counts
  combat time, pull time and time into its own episodes.
- **Never built into the forge core.** A forge build disables it (`-DMODULE_MOD-ANIMUS=disabled`).

## 6.2 Source map

| File | Role |
|---|---|
| `src/animus_loader.cpp` | Registers animus-lib's scripts, then `AddSC_animus` |
| `src/Hooks/AnimusScripts.cpp` | `.animus` commands (game master, not from the console), `WorldScript` (config load, update, shutdown), `PlayerScript::OnPlayerLogout`, `UnitScript::DealDamage` |
| `src/AnimusMod.{h,cpp}` | Module root: config, `ModelLibrary`, layouts, parties by owner, viewers by game master |
| `src/AnimusConfig.{h,cpp}` | Settings and the viewer's `StageSettings` |
| `src/Companion/CompanionParty.{h,cpp}` | One player's companions |
| `src/Viewer/StageViewer.{h,cpp}` | One game master's stage |
| `conf/mod_animus.conf.dist` | Every key, documented |
| `mod-animus.cmake` | Installs `models/`, creates `mod_animus.conf` when missing, builds the bundled animus-lib |
| `animus-lib/` | This module's copy of the curriculum layer, left from when it was a shared repository. Nothing updates it now |
| `models/` | Where you may put models for the install step to copy |

## 6.3 Installing

1. Put the module in a stock AzerothCore's `modules/`. The curriculum layer comes with it in `animus-lib/`, so
   nothing is fetched at build time. Build static (the default). Do not enable mod-animus-forge in the same
   configure: it carries its own copy of those sources and the two would link twice, which the configure refuses.
2. Rebuild and install the worldserver.
3. Installing creates `mod_animus.conf` in the modules config directory from its `.dist` when there is none, and never
   overwrites one (under Docker the container copies it to the config volume on first start). AzerothCore reads a
   module's settings from the `.conf` only; without it every `Animus.*` key logs "Missing property" at startup.
4. Put the models in place (6.4).

The install step copies `models/*.amdl` and their `.json` manifests (which must sit beside them) to
`ANIMUS_MODELS_INSTALL_DIR` (default `<config dir>/modules/animus`: under Docker only the build's `bin/` and `etc/`
reach the runtime image). A relative `Animus.ModelDir` (default `animus`) is looked for under `DataDir` first (models
placed by hand), then the image's reference config directory (`env/ref/etc/modules/animus`, always the build's), then
`<config dir>/modules/animus`; the first holding a model is used and logged.

## 6.4 Models

`Animus.ModelDir` (default `animus`) is resolved against `DataDir` when relative, and used as-is when absolute.

- In Docker, the defaults line up: `AC_DATA_DIR` is `/azerothcore/env/dist/data`, and installing from `ac-dev-server`
  writes into the shared client-data volume.
- The stock `worldserver.conf` sets `DataDir = "."` (the worldserver's working directory). Either set `DataDir` to
  `<install prefix>/data`, configure with `-DANIMUS_MODELS_INSTALL_DIR=<DataDir>/animus`, or give `Animus.ModelDir` an
  absolute path.

For each layout it needs, the module looks for `<ModelDir>/<class><stage suffix>.amdl` and the `.json` manifest
beside it. `ModelLibrary` accepts a model only if the manifest file is exactly the manifest this server builds for that
layout (same stage, class, sizes, block offsets, actions, talents), and the `.amdl` header matches the layout's
dimensions. Failures are logged once and cached. Models load on first use, and again after `.reload config`, which
resets the library.

> **The 18 models currently committed under `mod-animus/models/` are `.amdl` version 1 and will not load.**
> They predate the memory and goal head, and the reader requires version 2 (3.8), so every one of them is
> refused with "has model version 1, expected 2" and the companions fall back to following. They are stale
> rather than broken: the curriculum is being retrained from scratch and every model has to be exported again
> when it finishes. Until then, companions on a stock install have no policy.

Where the models come from: `forge export <stage>` in the forge writes both files per class into
`AnimusForge.ModelDir`. Copy them to the realm. Build the realm with an animus-lib revision whose blocks, stages and
catalogs match the one the model trained with, or the manifests won't match. A layout's manifest doesn't depend on
which other classes the run trained (`AnimusForge.Classes`), only on its own stage, class and blocks.

## 6.5 Class companions

### Commands

All `.animus` commands need game master security and don't work from the console. Players use the module through
the Animus addon instead (below), which does the same things with no security at all.

| Command | Effect |
|---|---|
| `.animus create <name> <race> <class>` | Your one companion character, of that name, race and class, at your level. It joins your party |
| `.animus summon` | It comes back from the database to you |
| `.animus dismiss` | Saved and sent away |
| `.animus rename <name>` | A new name for the character, nothing else changed |
| `.animus reroll <race> <class>` | The character is deleted and a new one of the same name created |
| `.animus list` | Your companion, its class, level and spec, whether its model is loaded, whether it is with you |
| `.animus purge` | Administrator, from the console too: every `ANIMUS<guid>` account and its characters deleted (orphans of older runs included), every companion sent away unsaved, `animus_companion` dropped |
| `.animus life [<feature> on\|off]` | What the companions do outside the fight (below), and a switch for each until the next config reload |

### One companion per character

A player character owns at most one companion, and it is a character of its own (`CompanionRegistry`,
`animus_companion` in the characters database, a table the module creates with the first companion account and drops
on purge; nothing of the module's is in the database before or after): `characters` row, inventory, talents,
everything, on an account made for the owner on their first create (`ANIMUS<owner guid>`, random password, inserted
directly since `AccountMgr::CreateAccount` is asynchronous). Create builds it as a seat is built
(`BotFactory::Create`, `Configure` with a spec drawn with no demand) and saves it at once (`SaveToDB(create)`,
committed on the world thread); dismiss, the owner's logout and shutdown save it the same way (`CompanionParty::Save`,
with the registry's record: spec, edited, the owner's gear slots), and the core's autosave runs in between. Summon
loads it as a login does: `CompanionLoader` fills a copy of the core's `LoginQueryHolder` (35 queries; the class is
local to `CharacterHandler.cpp`), `DelayQueryHolder` runs it on the database thread, and `AnimusMod::OnUpdate`
finishes the ones that arrived -- a socketless session, `Player::LoadFromDB`, the social list -- and
`CompanionParty::Attach` places it beside the owner (`PlaceNear`), leaves a stale group, joins the owner's, reads its
build off it (`RefreshBuild`) and restocks it. Rename writes `characters.name` and the character cache (a summoned
companion is dismissed and summoned again, so the owner's client sees the new name); reroll deletes the character
(`Player::DeleteFromDB`, finally) and creates one of the same name. Deleting the owner's character deletes the
companion's and its account (`OnPlayerDelete`).

No mail and no achievements: `CompanionRegistry::Purge` deletes both (and mailed items) before every load and
after every save; `OnPlayerCanSendMail` refuses a companion as receiver, `OnPlayerCanGiveMailRewardAtGiveLevel`
refuses it a level reward, `CanCheckCriteria` and `OnPlayerBeforeAchievementComplete` refuse it every
achievement.

### The Animus addon

`interface_addon/animus_addon/Animus` in the module is a 3.3.5a client addon: a window (`/animus`, its one command,
or the minimap button) that creates the companion (name, race, class; the button goes once there is one), summons
and dismisses it, renames it or gives it a new race and class (behind a confirmation, since that resets it); a
"Dismiss companion" entry in its unit menu; and an inspect window that edits it. The Talents tab learns a rank on left
click and unlearns one on right click; a Pet tab, drawn by the addon (the client cannot read another player's
pet), does the same for a hunter pet's tree; an item dragged from the owner's bags onto the character pane goes
on the companion and what it wore comes back to the owner.

It whispers the player themselves with addon prefix `Animus` (`hello`, `list`, `create <name> <race> <class>`,
`summon`, `dismiss`, `rename <name>`, `reroll <race> <class>`, `talent <name> learn|unlearn <id>`, `pettalent
...`, `pet <name>`, `equip <name> <bag> <slot> <inv slot>`); `AnimusPlayerScript`'s private-chat hook swallows
those whispers and `Addon::Handle` answers them with addon whispers back (`HELLO`, `RACE`, `COMPANION`, `PET`,
`PETTALENT`, `OK`, `ERR`), calling `AnimusMod`. A summon's answer comes in two parts: `OK` now, and `OK` with the
`COMPANION` line again (`Addon::Push`) when the character has loaded. A hello sends the races of the player's
faction and the classes each can be (player info, cheap) and the companion. The realm needs `AddonChannel = 1`
(the default) and, for the inspect edits, `TalentsInspecting = 1`. `interface_addon/animus_addon/Animus/README.md`
lists every message.

**Edits** (`CompanionTalents`, `CompanionGear`). A talent rank is unlearned under the client's own rules (points
in the rows above, prerequisites) as `resetTalents` removes a talent, one rank at a time, the point refunded and
the rank below learned again as a command; a pet's through `Pet::unlearnSpell`, which refunds and relearns the
rank below itself. Unlearning leaves the core's private count of spent points off, which only
`InitTalentForLevel` reads, so an edited companion's level-up (`LevelUpEdited`) snapshots its talents and its
pet's, resets, learns them again at the new level, and spends the new points along the standard build from where
the old total left off; what the build cannot place stays for the owner. An item the owner gives leaves their
inventory in the database at once (as mail does) and the companion's save moves the row over; what it wore goes
into the owner's bags the same way (a never-saved item as a new one). The owner's items are taken off before
`Configure` rebuilds the gear at a level-up and put back after, and stay on the companion, saved with it.

### Where companions go with you

Companions follow you through every loading screen (`CompanionParty::Update`, `UpdateMember`):

- **Another map or an instance.** As soon as you arrive, each companion is brought to you, into the same instance with
  your group's difficulty, in or out of combat. Entry requirements don't apply to it. Only an instance that refuses
  anyone at that moment (full, or a raid encounter in progress) keeps it out until it takes it.
- **Transports.** A companion boards a transport when you do and steps off when you step off, through a map change
  too.
- **Flight paths and vehicles.** While you are on one, the companions leave the world (they are *parked*). When you
  are off it, they come back beside you, wherever you landed.
- **Battlegrounds and arenas.** Companions wait where you left them and rejoin you when you come back.
- **Other teleports** a companion starts itself (a summoning spell, a transport changing maps) complete as a client
  would acknowledge them.

A companion never answers an instance's lock warning, so it is only ever bound to an instance temporarily.

### The party as an env

`CompanionParty` stands in for a training env. **You are the owner, the companions are the seats, and whatever is
fighting any of you is the current pull.** Every world update:

**Tracking the pull** (`UpdatePull`). Enemies are units that attack you, a companion, or any of your pets, plus the
units you and the companions attack. Only living, valid attack targets on your map count.

- A new enemy takes a free slot (up to 4). Once all slots are taken, it replaces a slot whose enemy is dead or gone.
- The first enemy of a pull starts a new **episode** if none has started yet, or if the party has been quiet for at
  least 20 s (`NEW_EPISODE_QUIET_MS`). A new episode resets pulls cleared, restarts the episode clock the models
  observe, and restocks every living companion, just as training starts every episode with full bags.
- The pull ends once no enemy in it is alive and still fighting (dead, gone, or evaded home). Then pulls cleared is
  incremented, slots and target selections reset, and the quiet timer starts.

**Each companion** (`UpdateMember`):

- **Levelling.** When you have levelled past it, the companion follows once the party is quiet and it is alive: it
  takes your level (`GiveLevel`), its talents are reset and spent again from all its points, and `Configure` gives it
  the trainer spells and gear of the new level, as a forge seat of that level has. Its pet comes back out if it had
  one, its bags are restocked, and you are told its new level.
- **Dead.** It accepts a pending resurrection at once, as a client would. Otherwise, once the party is quiet, you are
  alive and out of combat, and it has been dead 10 s, it stands up with half health.
- **Out of combat.** Following you is the model's job: its `follow` press runs it to just behind you and keeps
  re-aiming at where you are until it is there and you have stopped, and it re-presses to keep following, exactly as
  it trained beside a scripted owner that runs off between pulls. Nothing on the module's side leashes or teleports a
  companion that has fallen behind on your map; only a loading screen or a transport moves it for you (above).
  Without a model: it stays within 6 yd of you, since there is nothing else it can do.
- **Deciding.** Every `Animus.Curriculum.DecisionMs` (250) of accumulated update time, if its model is available,
  the companion decides. It keeps a `SeatMemory` as a forge seat does: the observation's memory features, and the
  pacing and locks (`Animus.Curriculum.Actions.*`) masked out of its choices, so it plays with the mask it trained with.

**A decision** (`Decide`) mirrors a training seat:

1. Track combat start (the combat-time feature).
2. Fill the last-step features as the forge's reward step would: damage dealt since the last decision divided by the
   level's damage scale, damage taken divided by max health (both from the module's `DealDamage` hook, stored in
   atomics because map threads write them), and the power change. They are counted as animus-lib counts a seat's:
   damage dealt by the companion or its pets, guardians and totems, only on an enemy of the current pull, and damage
   taken by the companion itself.
3. Choose the target: the selected enemy slot, or the nearest living enemy (which becomes the selection).
4. Build the `SeatView`: you as the owner, the other companions as teammates, the enemy slots, pull timing, the
   episode clock (time since the episode started / 5 min), supplies, stable. Run `SeatEncoder::Observe`.
5. With no target and a layout that can't act without one, stop.
6. `MlpPolicy::Decide` picks the highest-scoring allowed action. `SeatEncoder::Apply` performs it as a client would:
   casts, item uses, movement, target selection, pet commands, heals and resurrections on party members. A called
   hunter beast is summoned.

**Logout and dismissal.** When you log out, your companions are removed (`OnPlayerLogout`). A companion that
disappears any other way is dropped from the party and taken out of the group. A companion is removed from
the group and logged out without saving (`BotFactory::Destroy`). Because a stock core has no sim groups, companions'
group membership is written to the database like any member's and removed on dismissal. Rows left behind by a crash
are cleaned up by the core at startup (group members without a character).

### Life outside the fight

A companion whose model carries the **world block** -- exported from `stage20_quest` or later, the crossroads
included -- lives a little on its own. The block is the one the forge's life stages trained (manual 4, stages
20-22): the nearest corpse it may loot, quest giver it has business with, gathering node and vendor, its own
bags, gold, durability, food, drink and whether something in the bags rates higher than what it wears; and the
six presses -- INTERACT, LOOT_ALL, EQUIP_UPGRADE, SELL_JUNK, REPAIR, BUY_SUPPLIES. `LifeService::Sense` fills
those features from the real world (no phasing, everybody's NPCs; the quest reported is the one in its log
furthest along), and the presses run through the same `WorldActions` the sim ran, so the model loots, talks,
gathers, dresses and trades exactly as it learned to. Nothing on the module's side decides when.

What is scripted is what is a lookup, or has no sim to learn it in, each a switch in `Animus.Life.*` and
`.animus life`:

- **Quests** mirror yours: when you take a quest the companion can take, it takes it (`OnPlayerQuestAccept`);
  when you drop one, it drops it. It does the objectives with you, loots the items, and hands the quest in
  itself when it stands at the turn-in, choosing the reward that rates highest for its build (`GearScore`).
- **The auction house.** Idle beside an auctioneer, it lists its unbound greens and better that are no upgrade
  for it, at the house's median price for the item (else four times the vendor's), and buys one upgrade for
  its build at buyout under `AuctionBudgetGold`. The house's mail -- sales, purchases, returns -- it collects
  when idle; a save keeps that mail (every other mail to a companion is purged, and none can be sent to one).
- **A flight path** when you are more than `TaxiBeyondYards` away on the same map and the path between the
  nearest flight points would close most of it. It pays. Nothing is decided in the air.
- **The corpse run.** Dead, it releases once you are out of the fight, walks its ghost back to the corpse and
  reclaims it as a player does; a corpse on another map, or five minutes of walking, and it takes the spirit
  healer's terms. Off, it stands up where it fell after ten quiet seconds, as before.
- **Crafting.** Idle, it makes what it knows a recipe for from what it carries (what it gathered, mostly).

**Not done.** Group loot rolls: a companion cannot see a roll it is asked to join (the roll list is the group's
own and no hook announces one), so it passes by not answering and the item goes to whoever rolled. Free-for-all
or round-robin loot lets it loot as it learned to. Free-roam travel without you, banks and guilds are later
plans.

None of this has run against a live realm yet: the first smoke is a companion beside a game master character
that takes a quest, does it, loots, equips, sells, repairs, lists a green and collects the mail.

## 6.6 The stage viewer

### Commands

| Command | Effect |
|---|---|
| `.animus stage list` | Every stage with its summary and arenas |
| `.animus stage open <stage> [policy] [arena]` | Teleport to the stage's spawn point, build it there and spawn its first episode, frozen |
| `.animus stage spawn [tier] [class_role] [level]` | Remove the episode and spawn a new one, frozen, with the choices given |
| `.animus stage start` | Let it play: episodes follow one another until `stop` |
| `.animus stage stop` | Freeze everything where it is |
| `.animus stage status` | The open stage: frozen or playing, its episode, arena, spawn choices, seats and models |
| `.animus stage close` | Remove the stage |

Policies: `model` (each seat's exported model; the default `Animus.Stage.Policy`), `random`, `greedy`, `fight`.

`spawn`'s choices hold for every episode after it until the next `spawn`; each is `any`, or left out, for the
curriculum's own:

- **tier**: the difficulty tier of a stage that fights one creature (stage 1's `duel`), 0 to
  `Animus.Curriculum.Difficulty.MaxTier`, as 4.5 describes it (below `EliteTier` a normal creature that many levels
  above the character, from it an elite). `any` is the class and role's own training tier, which starts at 0 on every
  server start and climbs as it wins. Other stages refuse a tier.
- **class_role**: what the first seat plays (`warlock_dps`), one of the stage's classes.
- **level**: every character's level, raised to what its class can be (a death knight is at least 55).

### Lifecycle

**Begin** (`StageViewer::Begin`). The stage, policy and arena are checked. A named arena forces that arena for every
episode; otherwise arenas are drawn by weight. The spawn map must be instanceable. GM mode is turned on as `.gm on`
would (the reply says so, and it stays on after the stage stops), and the game master is teleported to
`Animus.Stage.SpawnPoint.*` (default: the forge's own spawn point, the Old Hillsbrad Foothills entrance),
which creates or enters an instance of that map. Opening a stage replaces the one you already have open. At most
`Animus.Stage.MaxViewers` (4) stages run at once, and each takes the lowest free env id, which keeps bot accounts and
names apart.

**Travelling.** The viewer waits up to 60 s for you to arrive. If you land anywhere other than an instance of the spawn
map, the stage ends.

**Build.** A `StageScenario` is created with the viewer's `StageSettings`:

- one env, the viewer's env id, `Animus.Stage.*` decision interval, episode length, classes, spawn point and level
- the `Animus.Curriculum.` tuning prefix
- report means over each episode
- no layouts directory

A baseline policy is checked against the stage. The arena is forced if one was named. The env pool is told to build
env 0 **in your instance** (`EnvPool::PlaceEnv`), then `Setup`, `ResetAll` and `PoolRegistry::Register`, so animus-lib's
own hooks count the seats' damage and healing. The first episode is frozen. The first build of a class's assets
stalls the world for a few seconds.

**Frozen.** Every living unit within 150 yd of you and of each seat gets the GM freeze aura (9454, what `.freeze` puts
on a unit): the seats, their pets and summons, the creatures and the owner, but no player that isn't the stage's. Nobody
decides and the episode's clock stands still. Every 500 ms, units that turned up since (a pet arriving) are frozen
too. `spawn` passes its choices to the scenario (`StageScenario::ForceTier`, `ForceLayout`, `ForceLevel`; a forced tier
is what `CreatureEncounter` fights at, and doesn't move the class's own), lifts the freeze, calls `ResetAll()` and
freezes the new episode. `start` lifts the freeze; `stop` freezes whatever is there, mid-fight too (a cast in progress
is interrupted by the stun).

**Running.** Each world update:

- if you left the instance or logged out, the stage ends,
- while frozen, only the freeze sweep,
- `AdvanceClock(diff)`,
- every `Animus.Stage.DecisionMs` of accumulated time, **one** decision. A long world update doesn't queue up extra
  decisions:
  1. `Collect()` (rewards, episode end, auto-reset, observe). When an episode ended, a chat report shows the arena,
     length and whether it ended or hit the time limit, and per present seat: level, class, damage and DPS, damage
     taken, kill, died, health left and total reward (the sum of the `reward_*` columns),
  2. actions: `model` asks `ModelLibrary` for each present seat's layout and calls `Decide`. A seat with a missing or
     refused model does nothing, and you are told once per model. Any other policy calls
     `EnvPool::ChooseLocalActions`,
  3. `ApplyActions()`.

**Close.** Lift the freeze, unregister the pool, tear down the env (seats, owner, enemy players, creatures, group). It
is safe to call more than once.

### Watching a stage as it was trained

- **GM mode is on.** `.animus stage open` turns it on, so the stage's creatures and enemy players, which choose their
  targets among the seats and the owner, leave you alone. Turning it off in the middle of a pull lets them attack you.
  Your presence changes nothing the seats observe.
- **Match the settings** to the forge run: `Animus.Stage.DecisionMs`, `EpisodeSeconds`, `Level` and `SpawnPoint.*`
  like the run's `AnimusForge.*` keys, and `Animus.Curriculum.*` like the run's `stage.json` `"tuning"`.
  `Animus.Stage.Classes` only chooses which characters appear. Unlike the forge's list, it doesn't change what the
  models were trained on.
- **Nothing is saved.** Seats, owners and enemy players have no character rows. The rows a stock core writes for their
  instance binds and groups are removed when they go.

## 6.7 What it costs

The question a server operator asks first is whether this makes the world tick worse. Measured on the real
`MlpPolicy` against an exported model, one companion decision is:

| | |
|---|---|
| Forward pass | **94 us** (`-O3` with the reassociation flags, 3.8); 310 us if that file is built plain `-O2` |
| Build the observation | ~28 us |
| Apply the action | ~9 us |
| **One decision** | **~130 us** |

A companion decides every `Animus.Curriculum.DecisionMs` (250 ms), so four times a second: **about 0.05% of one
core per companion.** A player with a full party of four costs ~2 ms of CPU per second of play. Two hundred
companions across fifty players cost about 10% of one core.

Two things about *where* that lands matter more than the number:

- **It runs on the world thread.** `AnimusWorldScript::OnUpdate` is `WORLDHOOK_ON_UPDATE`, so every companion's
  decision is serial with everything else the world does, and adds to the tick rather than running beside it. At
  these magnitudes that is fine; it is the thread to keep an eye on if the count ever grows.
- **Decisions are not spread across ticks on purpose.** Each companion accumulates `SinceDecisionMs` and fires when
  it passes `DecisionMs`, and `SinceDecisionMs %= DecisionMs` preserves whatever phase it started with. With
  `MinWorldUpdateTime` at 1 ms and a 250 ms period there are enough distinct phases that companions summoned at
  different moments spread themselves; companions summoned *in the same tick* stay aligned for their whole
  lifetime. It costs nothing at party sizes. If hundreds are ever summoned together, seed `SinceDecisionMs` with
  `urand(0, DecisionMs)` so they scatter.

**Responsiveness is set by the cadence, not by the compute.** A companion reacts up to 250 ms after something
happens, which is roughly human reaction time and reads as natural rather than sluggish. It is not a free knob:
`DecisionMs` must match what the models were trained with (`AnimusForge.DecisionMs`), so making companions
twitchier means retraining, not reconfiguring.

**Memory** is one loaded model per class rather than per companion -- about 2.4 MB each, so under 45 MB with
all eighteen resident -- plus each companion's `MlpPolicy::State`, which is the GRU vector and a couple of
integers.

## 6.8 Differences from the forge

| Aspect | Forge | Stock core with mod-animus |
|---|---|---|
| Clock | Fixed ticks of `DecisionMs` game time | Real time. Decisions happen when `DecisionMs` of update time has accumulated |
| Bot sessions | Sim sessions: no logout or bind writes | Normal: a few rows written on logout and instance bind, bind rows deleted on destroy |
| Party group | Sim group, memory only | Normal group rows, removed on dismissal or stop |
| Evaluation seeds | Reproducible (`rand_seed`) | Not available (`SeedRandom` returns false) |
| Packets | None | Everything is visible to real clients: movement, casts, combat log |
| Episodes | Defined by the stage | Stage viewer: defined by the stage. Companions: a pull after 20 s of quiet starts one |
| Owner | Scripted player | You |
| Life outside the fight | The life encounters' phase: one quest, one field, one town an episode | The real world: your quests, the house, the mail, flight paths, the corpse run (`Animus.Life.*`) |
