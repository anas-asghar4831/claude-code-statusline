# statusline (C port)

Max-performance C rewrite of `../statusline.js`. **Byte-identical output**; ~1.6–1.7x faster
warm and cold.

## Why
- `yyjson` parses the JSONL cost history (hundreds of MB on the 30D walk) far faster than
  Node's `JSON.parse`.
- Win32 `FindFirstFile` yields name + is-dir + mtime inline — no per-entry `stat` syscall.
- **Git read entirely in-process — zero subprocess on the common path** (gitstatusd technique):
  - branch ← `.git/HEAD`; HEAD/upstream sha ← ref files / `packed-refs`.
  - dirty ← parse `.git/index` (v2/v3) + `stat` each entry, early-exit on first change.
    Unstaged edits/deletes detected instantly. Staged + untracked come from a `git status`
    result cached by `.git/index` mtime (re-runs only after add/commit/checkout).
  - commit count + ahead/behind cached by HEAD/upstream sha (`git-cache.json`); a subprocess
    runs only right after HEAD or upstream moves.
- **Skill-count cache** keyed by the `plugins/cache` dir mtime — the 531-dir walk runs only
  when a plugin is added/removed.

## Incremental cost cache (binary, mmap, threaded, atomic)
The cost figures (REPO/30D/7D/DAY) require summing token costs across **all** project
history (~360MB / 1264 JSONL files), with global dedup (~52% of rows are cross-file
duplicates from session resume/fork). A 5-min summary (`cost-cache.json`) covers warm
renders; on expiry it rebuilds — and that rebuild is now incremental:

- **`cost-index.bin`** — a binary per-file cache of parsed rows `(idHash, tokens, ISO ts)`,
  keyed by each file's path + mtime + size. Re-parse only files that changed; reuse the rest.
- **mmap read** (`MapViewOfFile`) — load the index with no copy/JSON-parse.
- **Multithreaded first build** — parse the misses across all cores (only the cold first run,
  or when many files changed).
- **Atomic writes** (temp + rename) — safe across concurrent Claude Code sessions.
- Rows store raw tokens (pricing applied at aggregation → correct under model switches) and
  the ISO timestamp (so the rolling windows recompute exactly). Dedup by 64-bit id hash.

## Performance (this machine, 360MB / 1264 JSONL)
| | C | Node | speedup |
|---|---|---|---|
| internal compute (warm) | **~3.7 ms** | — | — |
| warm end-to-end (real exe) | ~12 ms | ~264 ms | ~22x |
| **cold cost rebuild (incremental)** | **~40 ms** | ~3.1 s | **~75x** |
| cold first-ever build (threaded) | ~0.5 s | ~3.1 s | ~6x |

Before the incremental cache, a cold rebuild re-parsed all 360MB (~1.3 s). Now it re-parses
only the active file. Warm compute is ~3.7 ms; the rest of the ~12 ms is Windows process
creation + exe load (OS cost — only a persistent daemon avoids it, but the statusline is
spawned fresh each render). Profile with `STATUSLINE_PROF=1` (phase timings to stderr).

## Dirty-flag caveat
The fast path detects modified/deleted tracked files instantly. Staged and untracked states
come from a `git status` cached by `.git/index` mtime, so an untracked file created with no
other index change may not flip the ⚠️ icon until the next index write (add/commit/checkout)
or any unstaged edit. Matches `git status` in every case tested otherwise.

## Build
```bash
./build.sh                 # gcc -O3 -march=native -flto statusline.c yyjson.c
```
Needs gcc (MSYS2 / mingw-w64 on Windows). `yyjson` (MIT) and `uthash` (BSD) are vendored.

## Use
Point `~/.claude/settings.json` `statusLine.command` at the binary:
```json
{ "statusLine": { "type": "command", "command": "C:/Users/<you>/.claude/statusline/c/statusline.exe" } }
```
The JS version stays the maintained reference; this is a drop-in fast path. Both share the
same cache files in `..` (`cost-cache.json`, `burn-cache.json`, `location-cache.json`,
`prayer-cache.json`).

## Parity note
Output matches `statusline.js` exactly for real inputs. The only divergence is the
rate-limit countdown on *past* reset timestamps (test mocks): JS floors toward −∞, C
truncates toward 0. Real reset times are always in the future, where the two agree.
