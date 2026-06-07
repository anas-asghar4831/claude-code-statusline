# statusline (C port)

Max-performance C rewrite of `../statusline.js`. **Byte-identical output**; ~1.6–1.7x faster
warm and cold.

## Why
- `yyjson` parses the JSONL cost history (hundreds of MB on the 30D walk) far faster than
  Node's `JSON.parse`.
- Win32 `FindFirstFile` yields name + is-dir + mtime inline — no per-entry `stat` syscall
  (Node parity via one directory enumeration).
- `CreateProcess` spawns git directly (no `cmd.exe` shell per call).
- `git status --porcelain=v2 --branch` collapses branch + ahead/behind + dirty into one
  spawn (6 git calls → 4).

## Performance (this machine, 360MB / 1252 JSONL)
| | C | Node |
|---|---|---|
| warm (cached, real repo) | ~173 ms | ~284 ms |
| cold (full cost re-parse) | ~1.4 s | ~2.5 s |

The warm floor is dominated by the remaining git subprocess spawns (~40 ms each), paid by
both implementations.

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
