# claude-code-statusline

A fast, feature-rich status line for [Claude Code](https://claude.ai/code) on **Windows 11** (Node.js, works in Windows Terminal / Git Bash).

```
D:/Dev/Octacer/project → .worktrees/ui-reorg  (main) ✅  |  🧠 Opus 4.8 (1M context)  |  ██░░░░░░░░ 22% (224.2k)
In:8.4k  |  Out:105.6k  |  Write:483.9k  |  Read:2.1M  |  Total:2.7M
⊙ 5H 2:10 AM (3h 0m) 47% • 7DAY Tue 4:00 AM (4d 4h) 57%  |  +73/-82  |  Commits:127  |  🕐 11:09 PM
REPO $0.00  |  30D $2358.65  |  7D $1118.07  |  DAY $248.75  |  🔥 LIVE $9.99  |  $3.64/hr  |  Cache hit: 98%
18 Dhū al-Ḥijjah 1447  |  Thu, Jun 4, 2026  |  🤖 Opus 4.8 $8.02 (61%) 11.4M ×59  |  Sonnet 4.6 $5.06 (39%) 10.7M ×91
🕌 Fajr 3:20 AM ✓  |  Dhuhr 12:01 PM ✓  |  Asr 3:40 PM ✓  |  Maghrib 7:04 PM ✓  |  Isha 8:42 PM ✓
📍 Lahore, Pakistan  |  Skills:15  |  MCP:3
   caveman, superpowers, frontend-design, context7, claude-md-management
   typescript-lsp, security-guidance, claude-code-setup, atlassian, csharp-lsp
   remember, firecrawl, netlify-skills, impeccable, skill-creator
🔌 atlassian, octa-portal, google-drive
```

## Features

| Line | Content |
|------|---------|
| 1 | **Launch dir → workspace** (FS-decoded, relative if subdir) · branch · git status · model · context bar |
| 2 | Session tokens: In / Out / Write (cache) / Read (cache) / Total |
| 3 | Rate limits (5H + 7DAY) with reset times · lines added/removed · commits · clock |
| 4 | Costs: REPO / 30D / 7D / DAY · live session cost · burn rate/hr · cache hit % |
| 5 | Islamic (Hijri) date · Gregorian date · per-model usage (cost, % share, tokens, call count) |
| 6 | Prayer times — colored: ✓ gray = passed · yellow = next (with countdown) · cyan = upcoming |
| 7 | Location · Skills count · MCP count |
| 8+ | Skill plugin names (5 per line, cyan) |
| 9+ | MCP server names (5 per line, magenta) — only if any configured |
| +1 | Subagent breakdown by type (only when subagents spawned) |

**Performance:** 0.18 s warm (4-file cache), ~3.6 s cold  
**Theme:** [Catppuccin Mocha](https://github.com/catppuccin/catppuccin) true-color  
**Inspired by:** [rz1989s/claude-code-statusline](https://github.com/rz1989s/claude-code-statusline) — JSONL cost parsing + burn-rate approach

---

## Requirements

- Windows 11
- [Node.js](https://nodejs.org/) (any recent LTS)
- `curl` on PATH (ships with Windows 10+)
- Claude Code

---

## Installation

**1. Copy the script**

```powershell
# Clone or download statusline.js to your Claude config dir
Copy-Item statusline.js "$env:USERPROFILE\.claude\statusline\statusline.js"
```

**2. Point Claude Code at it**

Edit `%USERPROFILE%\.claude\settings.json`:

```json
{
  "statusLine": {
    "command": "node C:/Users/<YOU>/.claude/statusline/statusline.js"
  }
}
```

Replace `<YOU>` with your Windows username. Use forward slashes.

**3. Restart Claude Code**

The status line appears at the bottom of every session.

---

## How costs work

- **REPO** — current project, all time (scanned from `~/.claude/projects/<slug>/`)
- **30D / 7D / DAY** — all projects, rolling windows
- **LIVE** — current session (from Claude Code's JSON)
- **Burn/hr** — last 5 minutes extrapolated, session-scoped (won't inherit prior session rate)
- All values deduplicated by `requestId` to avoid double-counting retries

Pricing table (per million tokens, `[in, out, cacheWrite, cacheRead]`):

| Model | In | Out | Write | Read |
|-------|----|-----|-------|------|
| Opus 4.5–4.8 | $5 | $25 | $6.25 | $0.50 |
| Opus 4 / 3 | $15 | $75 | $18.75 | $1.50 |
| Sonnet 4.x / 3.5 / 3.7 | $3 | $15 | $3.75 | $0.30 |
| Haiku 4.5 | $1 | $5 | $1.25 | $0.10 |
| Haiku 3.5 | $0.80 | $4 | $1.00 | $0.08 |

---

## Launch dir decoding

Claude Code encodes the project path as a slug in `~/.claude/projects/` (e.g. `D--Dev-myproject`). This script decodes it back using a **filesystem DFS** — at each `-` it tries separator vs literal hyphen and checks if the resulting path exists. This correctly handles folder names with hyphens (e.g. `my-project`).

If the current workspace is a subdirectory of the launch dir, only the relative part is shown:

```
D:/Dev/myproject → .worktrees/feature-x  (feature-x) ✅
```

---

## Prayer times

Fetched from [aladhan.com](https://aladhan.com/prayer-times-api) using your IP-geolocated coordinates (method 1 — University of Islamic Sciences, Karachi). Cached daily. Location cached 6 hours.

To use a fixed location instead of IP geolocation, edit `location-cache.json`:

```json
{ "lat": 31.5204, "lon": 74.3587, "city": "Lahore", "country": "Pakistan", "cached_at": 9999999999 }
```

---

## Cache files

All in `~/.claude/statusline/` — delete any to force a refresh:

| File | TTL | Content |
|------|-----|---------|
| `cost-cache.json` | 5 min | REPO + 30D/7D/DAY costs |
| `burn-cache.json` | 30 s | Burn rate (session-scoped) |
| `location-cache.json` | 6 h | IP geolocation |
| `prayer-cache.json` | 1 day | Prayer times |

---

## Config file locations

Claude Code splits configuration across multiple files — this matters for skills, plugins, and MCP servers:

| File | What lives there |
|------|-----------------|
| `~/.claude/settings.json` | Skills/plugins (`enabledPlugins`), statusLine command, permissions, model |
| `~/.claude.json` | **MCP servers** (`mcpServers`) — global, all projects |
| `.mcp.json` | MCP servers — project-level, overrides/extends global |
| `~/.claude/plugins/installed_plugins.json` | Installed plugin metadata + versions |

> **Note:** Despite what the official docs say, `mcpServers` in `~/.claude/settings.json` is **ignored** by Claude Code ([issue #4976](https://github.com/anthropics/claude-code/issues/4976)). The real global MCP config is `~/.claude.json`.

This statusline reads from all correct locations automatically.

---

## Customization

**Change prayer calculation method** — edit the `method=1` in the API URL (line ~279). See [aladhan.com/calculation-methods](https://aladhan.com/calculation-methods).

**Change colors** — edit the Catppuccin palette near the top of `statusline.js`.

**Disable prayer times / location** — remove or comment out the Location and Prayer sections.

---

## License

MIT
