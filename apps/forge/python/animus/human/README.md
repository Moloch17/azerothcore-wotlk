# animus.human — offline tools over human play captures

mod-animus records every player on the live realm (FORMAT.md is the contract); these tools read the files and
produce what the forge trains and judges with. Plan: `.agents/plans/human-play-data/`. `motion.py` is the one
definition of motion features: nothing here computes them any other way.

```
python -m animus.human <command> --capture <Animus.Capture.Dir> --out <dir> [--from yyyy-mm-dd[Thh]] [--to ...]
```

| Command | Writes | What |
|---|---|---|
| `summary` | stdout | files, bytes, records per type, players, header revisions, dropped/paused streams, open hours (`--index-only`: sizes and index.json alone) |
| `build` | `human_motion_windows.npz`, `human_reference.json`, `human_trips.json` (+ `.meta.json`), `human_hard_spots.json` (+ `.meta.json`) | the four files training uses, in one pass (`--stride`, `--max-windows`, `--trips-per-map`, `--seed`) |
| `fit` | `human_fit.json`, `human_fit.md` | the executor-fit study (`--max-clips`, `--beam`, `--spaces`) |
| `mapper-validate` | `human_mapper_validation.json` | inverse mapper accuracy on emulated sequences; casts too with `--ranks` and `--manifest` |
| `spell-ranks` | `spell_ranks.csv` | `spell_ranks` from the forge DB container `ac-animus-forge-database` (`--env-file .env` for DOCKER_DB_ROOT_PASSWORD, else the container's own password), or `--sql-dump` |
| `prices` | `human_prices.json`, `human_prices.md` | proposed Jitter/Effort/Repeat/Fidget/JitterDecayMs/SettleGraceMs (`--budget`); never writes config |
| `companions` | `human_companions.json` | per-model ratings, dismissals, overrides, deaths |
| `realism` | `realism.json` | a run's `eval_motion.npz` (`--run <dir>` or `--bot <npz>`) against `human_reference.json` (`--reference`, default `<out>/`): EMD per context per histogram feature and the mean |

## Modules

- `reader.py` — framing, record prefixes as numpy structured dtypes (vectorised per batch), gzip members read one
  at a time, the hourly layout and `index.json`. Unknown record types and longer records are skipped by length; a
  truncated final gzip member is dropped whole and counted; records may straddle members.
- `tracks.py` — move packets to tracks on the 250 ms grid: mode from flags, mount, speed in force, combat; cuts at
  taxi, death, loading, vehicle, transport, teleport, map change; involuntary spans flagged; stands filled.
- `segment.py` — clips (with context tags), trips with hindsight destinations, fights, deaths and corpse runs,
  idle and stuck stretches.
- `dataset.py`, `reference.py`, `trips.py`, `hard_spots.py`, `build.py` — the four §5 files.
- `fit.py` — the MoveBlock emulator and the beam search; `mapper.py` — the inverse mapper; `prices.py`;
  `companions.py`.

## Choices worth knowing

- **Companions are never human data.** A session of kind 1 or a Move with source 1 is a bot; every `human_*` output
  leaves them out.
- **Tracks are cut at the end of each hour shard** (files are processed one hour of one map at a time) and use server
  receive time (`ms`), the clock every stream shares. Latency is handled by down-weighting windows (1 up to 150 ms,
  falling to 0.25 at 500 ms), not by re-timing packets.
- **Standing still**: a client sends nothing while it stands, so the gap after a packet with no movement key is
  filled with held samples (0.5 s apart) when the next packet is within 0.5 yd and at most 10 s later; a longer
  stand is idle and cuts the track; anything else (a knockback, a lost stretch) cuts it too.
- **Metrics** are per unit (one player in one hour shard) and their percentiles are across units; definitions are in
  `reference.py`. Movement metrics are computed from motion features so they apply to bot windows unchanged; jump
  rate uses jump packets (human only).
- **The fit ignores terrain** (human tracks are feasible already), compares planar error on the ground and 3D in
  water and air, leaves FACE_TARGET out (no target positions), and assumes a held bearing is refreshed for free.
- **trips/hard spots** files hold exactly `{"format": 1, "maps": {...}}` with validated entries (FORMAT.md §5); build
  metadata goes to the `.meta.json` sidecars.
- **Bounded memory**: one shard's records at a time; the dataset is a bottom-k uniform sample of at most
  `--max-windows`; trips at most `--trips-per-map` per map; fit and prices sample clips.

## Tests

`tests/test_human_*.py`, with `tests/human_capture_writer.py`: a struct-based writer built from FORMAT.md (not from
the reader's dtypes). The reader test also reads W1's C++ sample when it exists (`apps/forge/tools/capture-sample.bin`
with its `.json`, or `ANIMUS_CAPTURE_SAMPLE=<path>`), and skips otherwise.
