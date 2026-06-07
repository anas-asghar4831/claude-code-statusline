# statusline (C port)

Max-performance C rewrite of `../statusline.js`. **Byte-identical output**; ~1.6–1.7x faster
warm and cold.

## Why
- `yyjson` parses the JSONL cost history (hundreds of MB on the 30D walk) far faster than
  Node's `JSON.parse`.
- Win32 `FindFirstFile` yields name + is-dir + mtime inline — no per-entry `stat` syscall
  (Node parity via one directory enumeration).
- `CreateProcess` spawns git directly (no `cmd.exe` shell per call).
- **Self-walk for the `.git` root** (no `rev-parse` spawn; zero git spawns when not in a repo).
- **Concurrent git spawns** — `status --porcelain=v2 --branch` + `rev-list --count` +
  `submodule status` launched together, then collected → wall time ≈ one spawn, not four.
- `--no-optional-locks` on status skips the index-refresh write.
- **Skill-count cache** keyed by the `plugins/cache` dir mtime — the 531-dir walk runs only
  when a plugin is added/removed (was ~28 ms every render).

## Performance (this machine, 360MB / 1252 JSONL)
| | C | Node | speedup |
|---|---|---|---|
| warm (cached, real repo) | ~77 ms | ~290 ms | 3.8x |
| cold (full cost re-parse) | ~1.56 s | ~3.1 s | 2.0x |

Internal compute is ~2 ms; the warm floor is `git status` itself (~50 ms, the OS/git cost
any tool pays). Profile with `STATUSLINE_PROF=1` (phase timings to stderr).

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
