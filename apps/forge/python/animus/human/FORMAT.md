# Human play capture — file format and interfaces

The contract between mod-animus (which writes capture files on the live realm) and the forge (which reads them, in
`apps/forge/python/animus/human/`, and trains on what they yield). Plan: `.agents/plans/human-play-data/`.
mod-animus keeps a copy at `doc/capture-format.md`; this file is the source. Change both together and bump
`FORMAT_VERSION`.

## 1. Files

Two tiers share one writer and one record format (format 4):

- the **automatic tier**: every player from login, into hourly files, at the realm's capture rates
  (`Animus.Capture.Enable`, `Dir`, `Auto`);
- **manual recordings**: one player's own play from the moment they press Record (the addon's Record tab or bar,
  `.animus record on`) until they stop, into a folder of their own, at the recording rates (`Recording.*`: 100 ms
  snapshots, every nearby unit's state, raw packets both ways, chat). A recording player's records go to the
  recording only, not to the hourly files, until they stop.

```
<Animus.Capture.Dir>/<yyyy-mm-dd>/<hh>/<stream>-<map>.bin.gz      automatic tier: UTC date and hour of the records
<Animus.Capture.Dir>/<yyyy-mm-dd>/<hh>/index.json                  written when the hour closes
<Animus.Capture.Dir>/recordings/<id>/<stream>-<map>.bin.gz         one manual recording (no hour split)
<Animus.Capture.Dir>/recordings/<id>/manifest.json                 written when the recording closes
<Animus.Capture.Dir>/salt                                          32 random bytes, hex; created once, never changed
```

- `stream` is one of `session`, `move`, `action`, `snapshot`, `outcome`, `companion`, `note`, `packet` (the last two
  are written by recordings only).
- `map` is the map id the record's player was on (`session`, `companion`, `note` and `packet` files use `-all`).
- A recording's `id` is `<yyyy-mm-dd>T<hhmmss>Z-<character>-<6 hex>`.
- Each file is a sequence of **gzip members** (one per writer flush); concatenated members are one valid gzip stream
  (Python's `gzip` reads them as one). A file cut short by a crash loses at most its last member.
- Files are append-only and never rewritten. Nothing is ever deleted by the module.

### index.json (per hour)

```json
{"format": 4, "hour": "2026-10-05T14", "module_revision": "<git sha>", "realm_build": "<core revision>",
 "files": {"move-0.bin.gz": {"records": 123456, "bytes": 987654}},
 "players": 42, "sessions": 57, "dropped": {"move": 0, "snapshot": 0}, "paused": {"snapshot": false}}
```

`dropped` counts records lost to a full ring buffer; `paused` says the disk watchdog paused a stream.

### manifest.json (per recording)

```json
{"format": 4, "id": "2026-10-25T120000Z-Thrall-1a2b3c", "status": "complete", "verdict": "keep",
 "title": "Deadmines, solo, level 18",
 "character": {"name": "Thrall", "guid": 1, "class": 1, "race": 2, "gender": 0, "level": 18, "spec_points": [31, 0, 0],
               "item_level": 25},
 "start_utc": "2026-10-25T12:00:00Z", "end_utc": "2026-10-25T12:41:10Z", "duration_s": 2470, "end_reason": "stop",
 "maps": [0, 36], "party_at_start": [{"name": "...", "bot": true}], "flags": {"god": false, "gm": true, "party_fill": false},
 "notes": 2, "files": {"move-36.bin.gz": {"records": 12345, "bytes": 98765}}, "dropped": {"snapshot": 0}, "late": 0,
 "snapshot_ms": 100, "unit_ms": 100, "streams": 255, "packets": true, "chat_policy": "group", "pseudonymous": false,
 "player_id": 15289167984064978332, "module_revision": "<git sha>", "realm_build": "<core revision>"}
```

`status`: `recording` (manifest not yet written: the recording is open or the server died), `complete`, or
`interrupted` (logout, disconnect, shutdown, disk shortage, `Recording.MaxMinutes`). `verdict` is what the player
chose at Stop (`keep`, `discard`, or null); a discarded recording is marked, never deleted. A recording is
**identified**: it holds the character's name and guid, their group's chat and the raw packets.

## 2. Records

Little-endian, packed, no padding. Every record is framed:

```
u16 type     u16 length (bytes of payload)     payload
```

A reader skips any record type it does not know (by `length`), so new types can be added without breaking old
readers. A record may gain fields **at its end only**; readers take the fields they know and skip the rest.

Common field names: `ms` = server unix time in milliseconds (u64); `player` = pseudonymous id (u64, §4);
`unit` = pseudonymous id of any other unit (u64, same hashing); positions are f32 world yards; angles f32 radians.

### 2.1 Every file

| type | name | payload |
|---|---|---|
| 0 | FileHeader | `char[8] magic="ANCAP\0\0\0"`, `u16 format`, `u16 stream`, `u64 opened_ms`, `char[40] module_revision`, `char[40] realm_build` |

`stream`: 1 session, 2 move, 3 action, 4 snapshot, 5 outcome, 6 companion, 7 note, 8 packet (format 4).

### 2.2 session

| type | name | payload |
|---|---|---|
| 1 | SessionStart | `u64 ms, u64 player, u64 session, u8 class, u8 race, u8 gender, u8 level, u8 tree_points[3], u16 item_level, u32 map, u32 zone, u32 area, u16 latency_ms, u32 client_build, u8 kind` |
| 2 | SessionContext | same as SessionStart, on any change of level, talents, gear (item level), group, map/zone/area |
| 3 | SessionEnd | `u64 ms, u64 player, u64 session, u8 reason` (0 logout, 1 disconnect, 2 shutdown) |
| 4 | GroupState | `u64 ms, u64 player, u8 kind (0 solo,1 party,2 raid), u8 count, then count × {u64 unit, u8 class, u8 level, u8 role (0 dps,1 tank,2 heal,3 unknown), u8 is_companion}` |
| 5 | KnownSpells | `u64 ms, u64 player, u16 count, then count × u32 spell_id` (on login and when a spell is learned) |
| 6 | Latency | `u64 ms, u64 player, u16 latency_ms` (every 60 s) |
| 7 | Inventory | `u64 ms, u64 player, u16 count, then count × {u8 bag, u8 slot, u32 entry, u32 count, u32 perm_enchant, u32 temp_enchant, i32 random_property, u32 durability, u32 max_durability}` (format 4, recordings: at start and at most once a second after a change; bag 255 is the equipment and the backpack) |
| 8 | ActionButtons | `u64 ms, u64 player, u8 count, then count × {u8 button, u8 kind (ActionButtonType: 0 spell, 64 class, 128 item, 192 macro ...), u32 action}` (format 4, recordings) |
| 9 | Talents | `u64 ms, u64 player, u8 active_spec, u8 count, then count × {u32 talent, u8 rank}, u8 glyph_count, then glyph_count × u32 glyph` (format 4, recordings) |

`kind`: 0 human player, 1 Animus companion.

### 2.3 move (the primary stream: every movement packet, unquantised)

| type | name | payload |
|---|---|---|
| 10 | Move | `u64 ms, u64 player, u32 client_ms, u16 opcode, u32 move_flags, u16 move_flags2, f32 x, f32 y, f32 z, f32 o, f32 pitch, u32 fall_ms, f32 jump_zspeed, f32 jump_sin, f32 jump_cos, f32 jump_xyspeed, u32 map, u8 source, u32 server_ms` (`server_ms` format 3) |
| 11 | Speeds | `u64 ms, u64 player, f32 walk, f32 run, f32 run_back, f32 swim, f32 swim_back, f32 flight, f32 flight_back, f32 turn_rate, f32 pitch_rate` (at session start and on every change) |
| 12 | MotionEvent | `u64 ms, u64 player, u8 event, u32 arg, f32 x, f32 y, f32 z, u32 map` |
| 13 | MoverState | `u64 ms, u64 player, u8 kind, u8 class, u8 race, u8 level, u32 map, u32 zone, u32 mount, u32 form, u8 in_combat, u8 move_revision, char[32] model` (format 2) |
| 14 | MoveTally | `u64 ms, u64 player, u8 kind, u32 sent, u32 kept` (format 3) |
| 15 | MapUpdate | `u64 ms, u32 map, u32 instance, u32 diff_ms` (format 3) |

`opcode`: the client opcode (`MSG_MOVE_*` and the movement acks, as the server's movement handler received it).
`source`: 0 a player's client packet; 2 an Animus companion's packet (format 2): its player controller reports through
its session's own movement handlers exactly as a client does, so it is recorded at the same point (the handler's
`OnPlayerMove`) with the same fields, and `client_ms` is the companion client's own clock; 1 was format 1's synthesised
companion sample (its server position once a decision), no longer written -- a reader of format 1 files keeps it out
of the kinematics.

**Two clocks on every Move** (format 3). `client_ms` is the mover's own client clock, what the client claims: a
player's client's, a companion's client's (its CompanionClient clock, which starts one second ahead of the server's).
`server_ms` is the server's own monotonic clock (`getMSTime`, milliseconds since the worldserver started, wrapping at
2^32) when the server's movement handler took the packet -- the same clock for both kinds, so intervals and cadence
are compared on it. For a player it trails the packet's arrival by the time it waited in its session's queue (up to
one world/map update); a companion hands its packet to the handler as it sends it. `ms` stays the server's unix time
for joining streams. A format 1/2 Move has no `server_ms`; readers take it as 0.

MoveTally counts a mover's movement packets since its session began (cumulative): `sent`, those that reached a
movement handler (every opcode whose handler records a kept packet here); `kept`, those the handler kept and this
stream recorded. `sent - kept` is what the server refused or ignored (a spline under way, movement disabled, a
teleport pending, an ack's pre-check, an invalid position). Written every 60 s while the counts change, and at the
session's end, for players and companions alike.

MapUpdate records every tick of a map instance that holds at least one captured mover (player or companion), in that
map's move file: every one, not a sample -- one 28-byte record per occupied map instance per world tick. `diff_ms` is
the diff that tick's player updates got (`Player::Update`; a companion's controller tick runs in it with the same
diff), written by the first mover to update on that map that tick. It is the realm's movement tick; the map's full
updates of creatures and objects (`MapUpdate.Interval`, `t_diff`) run on their own, slower, cadence and are not this.

MoverState is written at a mover's first update and whenever a field changes, for players and companions alike:
`kind` (0 human, 1 companion, as SessionStart), class, race, level, map and zone, `mount` (the mount aura's spell, 0
on foot), `form` (ShapeshiftForm), `in_combat`, and for a companion the model it plays (`model`, NUL-padded) and its
move block's revision (`move_revision`; 0 and an empty model for a player). Changes are seen at the mover's update
(each map tick). It is what a mover's motion is compared under: §3's `mounted` and `in_combat` for both kinds, and
the model and revision a companion's motion belongs to.

MotionEvent `event`: 1 mount (arg spell), 2 dismount, 3 taxi start (arg path), 4 taxi end, 5 teleport (arg new map),
6 death, 7 resurrect, 8 root, 9 unroot, 10 stun start, 11 stun end, 12 fear/confuse start, 13 fear/confuse end,
14 knockback, 15 loading screen start, 16 loading screen end, 17 shapeshift (arg form), 18 vehicle enter, 19 vehicle
exit, 20 transport board (arg transport entry), 21 transport leave.

Movement between a cutting event (3, 5, 6, 15, 18, 20) and its end is not player-steered and is never used to learn
style; 8–14 mark involuntary motion.

### 2.4 action

| type | name | payload |
|---|---|---|
| 20 | CastRequest | `u64 ms, u64 player, u32 spell, u64 target, u8 target_kind, f32 tx, f32 ty, f32 tz, u8 gcd_active, u8 casting, u32 power, u8 power_type` |
| 21 | CastResult | `u64 ms, u64 player, u32 spell, u8 result` (SpellCastResult; 255 = success/go) |
| 22 | CastEnd | `u64 ms, u64 player, u32 spell, u8 how` (0 completed, 1 cancelled by moving, 2 cancelled other, 3 interrupted) |
| 23 | Select | `u64 ms, u64 player, u64 target, u8 target_kind, f32 distance` |
| 24 | ItemUse | `u64 ms, u64 player, u32 item, u32 spell, u64 target` |
| 25 | Attack | `u64 ms, u64 player, u64 target, u8 start` |
| 26 | Interact | `u64 ms, u64 player, u8 what, u32 entry, u64 target, u32 arg, u32 spawn_id` (`spawn_id` format 4: the target's world spawn, 0 before) |

`target_kind`: 0 none, 1 self, 2 friendly player, 3 friendly creature, 4 hostile player, 5 hostile creature,
6 neutral, 7 ground position, 8 game object. `what`: 1 gossip, 2 loot, 3 quest accept, 4 quest turn-in,
5 game object use, 6 vendor, 7 flight master, 8 mailbox, 9 auction house, 10 trainer, 11 bank, 12 innkeeper.

### 2.5 snapshot (every Animus.Capture.SnapshotMs while in combat or moving; IdleSnapshotMs otherwise)

| type | name | payload |
|---|---|---|
| 30 | Snapshot | `u64 ms, u64 player, Self, u8 n_units, n_units × Unit, u8 n_auras, n_auras × Aura, u8 n_cooldowns, n_cooldowns × Cooldown` |
| 31 | UnitState | `u64 ms, u64 player, u64 unit, u32 spawn_id, u32 entry, u8 unit_type (0 creature, 1 player, 2 pet/guardian), u8 kind, f32 x, f32 y, f32 z, f32 o, f32 vx, f32 vy, f32 vz, u32 move_flags, u32 health, u32 max_health, u8 power_type, u32 power, u32 max_power, u8 level, u8 reaction, u8 react_state, u8 rank, u8 boss, u32 unit_flags, u32 npc_flags, u32 dynamic_flags, u32 unit_state, u8 death_state, u64 target, u64 victim, u32 casting_spell, i32 cast_left_ms, u8 motion_type, f32 home_x, f32 home_y, f32 home_z, u32 formation_leader_spawn, u8 flags (1 combat, 2 evading, 4 party, 8 my pet, 16 companion, 32 engaged with my group, 64 tapped by me), u8 n_auras, n_auras × {u32 spell, u8 stacks, i32 remaining_ms, u8 positive, u64 caster}, u8 n_threats, n_threats × {u64 unit, f32 threat}` (format 4, recordings: one per visible unit per `Recording.UnitMs`, the nearest `Recording.MaxUnits` within `Recording.UnitRange`) |
| 32 | GameObjectState | `u64 ms, u64 player, u64 object, u32 spawn_id, u32 entry, u8 go_type, u8 go_state (0 active: a door open, 1 ready: shut, 2 active alternative), u8 loot_state, u32 flags, f32 x, f32 y, f32 z, f32 o, u8 event (0 seen, 1 changed, 2 gone)` (format 4, recordings: every visible object at start and at each map entry, then each change) |
| 33 | PlayerDetail | `u64 ms, u64 player, u32 stats[5] (str, agi, sta, int, spi), u32 armor, u32 resistances[6] (holy, fire, nature, frost, shadow, arcane), u32 attack_power, u32 ranged_attack_power, u32 spell_power, u32 heal_power, f32 ratings[11] (crit melee/ranged/spell, haste melee/ranged/spell, hit melee/spell, dodge, parry, block, percent), f32 expertise, u32 max_health, u32 max_power[7], u32 money, u32 xp, u32 next_xp, u8 combo_points, u64 combo_target, u32 cheat_flags (1 god, 2 casttime, 4 cooldown, 8 power ...), u8 game_master` (format 4, recordings: once a second) |
| 34 | MapSpawns | `u64 ms, u32 map, u16 part, u16 parts, u16 count, then count × {u32 spawn_id, u32 entry, f32 x, f32 y, f32 z, f32 o, u8 rank, u8 boss, u32 formation_leader_spawn, u8 kind (0 creature, 1 game object), u8 go_type}` (format 4, recordings: the map's whole spawn table, once per map entered, in parts of at most 1500) |
| 35 | InstanceCreatures | `u64 ms, u32 map, u32 instance, u16 part, u16 parts, u16 count, then count × {u32 spawn_id, u8 alive}` (format 4, recordings: at start, at each map entry and at the end) |

```
Self     = f32 x, f32 y, f32 z, f32 o, f32 pitch, u32 map, f32 health_pct, f32 power_pct, u8 power_type,
           u8 flags (1 combat, 2 swimming, 4 flying, 8 mounted, 16 casting, 32 dead, 64 falling), u32 casting_spell,
           f32 breath_pct, u64 target, u32 shapeshift_form
Unit     = u64 unit, u32 entry, u8 kind (target_kind values), f32 x, f32 y, f32 z, f32 o, f32 vx, f32 vy, f32 vz,
           f32 health_pct, f32 power_pct, u8 level, u8 reaction (0 hostile,1 neutral,2 friendly), u8 flags
           (1 combat, 2 casting, 4 party member, 8 pet of the player, 16 companion), u32 casting_spell, u64 target,
           f32 threat_on_player (0 if none)
Aura     = u32 spell, u8 stacks, i32 remaining_ms (-1 permanent), u8 positive
Cooldown = u32 spell, u32 remaining_ms
```

Units: up to 24 hostile and 10 friendly within 40 yd, nearest first; party members and the player's pet are always
included wherever they are.

### 2.6 outcome

| type | name | payload |
|---|---|---|
| 40 | Damage | `u64 ms, u64 source, u64 target, u32 spell (0 melee), u32 amount, u32 absorbed, u32 overkill, u8 school, u8 flags (1 crit, 2 periodic)` |
| 41 | Heal | `u64 ms, u64 source, u64 target, u32 spell, u32 amount, u32 overheal, u8 flags` |
| 42 | Kill | `u64 ms, u64 killer, u64 victim, u32 victim_entry, u8 victim_kind, u32 victim_spawn_id` (`victim_spawn_id` format 4: 0 for a player, a pet or a summon, and before format 4) |
| 43 | Death | `u64 ms, u64 player, u64 killer, u8 cause (0 creature,1 player,2 fall,3 drowning,4 fire/lava,5 other), f32 x, f32 y, f32 z, u32 map` |
| 44 | Quest | `u64 ms, u64 player, u32 quest, u8 event (0 accepted,1 completed,2 rewarded,3 abandoned)` |
| 45 | Encounter | `u64 ms, u64 player, u32 map, u32 instance, u32 boss_entry, u8 event (0 engaged,1 killed,2 wipe)` |
| 46 | PvP | `u64 ms, u64 player, u64 other, u8 event (0 kill,1 death,2 duel won,3 duel lost)` |
| 47 | Area | `u64 ms, u64 player, u32 map, u32 zone, u32 area` |
| 48 | CombatEdge | `u64 ms, u32 map, u32 instance, u64 unit, u32 spawn_id, u32 entry, u8 event (0 enter, 1 exit, 2 evade), u8 evade_reason, u64 other, u32 other_spawn_id, f32 x, f32 y, f32 z, f32 ox, f32 oy, f32 oz` (format 4, recordings: a unit entering, leaving or evading a fight on a recording's map, the other side when the hook knows it; the first creature engaged of a pull) |

Damage and Heal are written when either side is a captured player (or a companion).

### 2.7 companion

| type | name | payload |
|---|---|---|
| 50 | CompanionDecision | `u64 ms, u64 companion, u64 owner, char[32] model, u32 obs_hash, u16 n_actions, n_actions × u16 action, u16 goal, u16 goal2` |
| 51 | CompanionCommand | `u64 ms, u64 owner, u64 companion, u8 command, u32 arg` (1 summon, 2 dismiss, 3 follow, 4 assist, 5 guard, 6 stay, 7 other order) |
| 52 | CompanionRating | `u64 ms, u64 owner, u64 companion, i8 rating (+1/-1), u8 reason` (0 none, 1 movement, 2 combat, 3 healing, 4 tanking, 5 stuck, 6 other) |

### 2.8 note (format 4, recordings only)

| type | name | payload |
|---|---|---|
| 60 | Annotation | `u64 ms, u64 player, u32 seq, u16 kind (1 note), u32 map, u32 instance, u32 zone, u32 area, f32 x, f32 y, f32 z, f32 o, u64 target, u32 target_spawn_id, u32 target_entry, u8 target_kind, f32 target_health_pct, f32 target_distance, u8 state (1 combat, 2 dead, 4 mounted, 8 casting, 16 swimming, 32 falling, 64 god), f32 health_pct, f32 power_pct, u8 power_type, u8 threat_on_me, u8 hostiles_30yd, u8 party_alive, u8 party_size, u32 client_ms, u16 text_len, text_len × u8 text (UTF-8)` |
| 61 | RecordingStart | `u64 ms, u64 player, u64 session, char[64] id, u8 class, u8 race, u8 gender, u8 level, u32 map, u32 instance, u32 zone, u32 area, f32 x, f32 y, f32 z, f32 o, u16 snapshot_ms, u16 unit_ms, u32 streams, u8 flags (1 god, 2 game master, 4 party fill off), u16 name_len, name_len × u8 name` (the recording's first record; the full dumps -- Inventory, ActionButtons, Talents, KnownSpells, PlayerDetail, MapSpawns, InstanceCreatures, every visible GameObjectState -- follow it) |
| 62 | RecordingEnd | `u64 ms, u64 player, u64 session, u8 reason (0 stop, 1 logout, 2 disconnect, 3 shutdown, 4 disk, 5 max minutes), u8 verdict (0 none, 1 keep, 2 discard), u32 notes, u32 seconds, u16 title_len, title_len × u8 title` |
| 63 | Chat | `u64 ms, u64 player, u8 direction (0 sent, 1 received), u8 chat_type (ChatMsg), u32 language, u64 sender, u8 sender_kind (0 self, 1 group player, 2 companion, 3 creature, 4 other player, 5 system), u32 sender_entry, u64 receiver, u8 redacted, u16 text_len, text_len × u8 text` |

A note is the only thing the player types: free text with where they stood, what they targeted and the state of
the moment. Pulls, rests, doors and bosses are not tagged; `animus.human dungeon-run` reads them off the other
streams. Another real player's chat outside the recorder's group is redacted (the text replaced by spaces of the
same length, `redacted` 1) unless `Animus.Capture.Recording.OthersChat`; addon messages are never recorded.

### 2.9 packet (format 4, recordings only)

| type | name | payload |
|---|---|---|
| 70 | RawPacket | `u64 ms, u64 player, u8 direction (0 client to server, 1 server to client), u16 opcode, u32 server_ms, u32 total, u32 offset, u8 flags (1 truncated, 2 continued in the next chunk), u16 length, length × u8 bytes` |

Every opcode both ways as sent, but those `Animus.Capture.Recording.PacketSkip` names (by default the chat opcodes,
whose text is the Chat record's, and Warden's). A payload over 60000 bytes is written in chunks (`offset` into
`total`, flag 2 on all but the last; joined by `(player, direction, opcode, server_ms, total)`); one over
`Recording.PacketMaxBytes` is cut (flag 1). A compressed update object (`SMSG_COMPRESSED_UPDATE_OBJECT`) is stored
as sent: `u32 inflated size` then zlib; `reader.iter_packets` inflates it.

## 3. Kinematic samples (shared by humans and bots)

What the style reward and the realism score read, for humans (resampled from the move stream) and bots (sent by the
sim each decision) alike. Defined in `animus/human/motion.py`; nothing else may compute motion features.

```
SAMPLE = [t_seconds, x, y, z, yaw, pitch, mode, mounted, speed, in_combat]
mode: 0 ground, 1 swimming, 2 flying, 3 airborne (jumping/falling)
speed: the forward speed in force for the mode (yards/second): run/swim/flight, or the mount's
```

Bots are sampled once a decision (`AnimusForge.DecisionMs`, 250 ms); humans are resampled to the same rate
(`motion.resample`), so the two are compared like for like. On the realm (format 2) a companion's samples come from
its move records exactly as a human's do (source 2 packets, resampled), with `mounted` and `in_combat` from
MoverState for both kinds.

## 4. Identity

`player` / `unit` = first 8 bytes of SHA-256(salt ‖ "player" ‖ u64 guid), little-endian; creatures use
"creature" ‖ u64 guid (spawn-scoped, fine for within-session tracking). The salt lives only in
`<Capture.Dir>/salt` on the realm. In the automatic tier no names, chat, account data or IPs are written.

A **manual recording** is the player's own and is identified: its manifest and RecordingStart carry the
character's name and guid, its Chat records the group's lines (others' redacted unless `Recording.OthersChat`), and
its RawPackets whatever the client and server exchanged (nearby names and guids among them; `Recording.PacketSkip`
can drop the name queries). Unit ids inside it are still the salted hashes above, so a recording joins the hourly
files on `player` and `unit`.

## 5. Interfaces between the forge workstreams

| File | Written by | Read by | Content |
|---|---|---|---|
| `<out>/human_motion_windows.npz` | `animus.human` (offline) | learner style discriminator | `windows` f32 [N, W, F] (motion.windows), `context` i16 [N] (motion.context_id), `weight` f32 [N], `meta` json |
| `<out>/human_reference.json` | `animus.human` | forge eval realism score, `forge human` report | per context: per motion feature histogram (fixed bins from motion.HIST_BINS), plus episode metric percentiles per class/spec/level band |
| `<out>/human_trips.json` | `animus.human` | forge arenas (TravelEncounter trip pools) | per map: trips `{start:[x,y,z], end:[x,y,z], seconds, mode, path:[[x,y,z],...]}` |
| `<out>/human_hard_spots.json` | `animus.human` | forge start pools | per map: `{pos:[x,y,z], kind: death/stuck/fall/drown, count}` |
| `<out>/<recording>.dungeon_run.json` + `.clearway.csv` | `animus.human dungeon-run` | forge PointNav stages (`AnimusForge.Curriculum.PointNav.ClearWayFile`) | a recorded dungeon run: packs, pulls (first creature, pull_from, fight_spot), rests, interactions, bosses, skipped, deaths, notes, and the clear way as checkpoints `{k, kind: site|gate|boss|goal, x, y, z, spawn_ids, t_ms, rest_after_s}`; the CSV is `index,kind,x,y,z,spawn_ids` (`;`-joined) |
| `<run>/eval_motion.npz` | learner (each movement-stage eval) | `animus.human` realism report | bot windows and contexts in the same layout as human_motion_windows |

### human_trips.json and human_hard_spots.json

Both are wrapped as `{"format": 1, "maps": {"<map id as a string>": [entry, ...]}}` and hold nothing else; a reader
refuses the whole file over one malformed entry, so the writer validates every entry first. Build metadata (time,
source range, counts, death causes) goes to the sidecars `human_trips.meta.json` / `human_hard_spots.meta.json`.

```
trip      {"start": [x, y, z], "end": [x, y, z], "seconds": s (>= 0), "mode": "ground"|"swim"|"fly"|"mounted",
           "path": [[x, y, z], ...] (optional)}
hard spot {"pos": [x, y, z], "kind": "death"|"stuck"|"fall"|"drown", "count": n (>= 1)}
```

`mode` is the trip's dominant mode; a ride mostly mounted on the ground is `mounted` (a flight is `fly` mounted or
not).
