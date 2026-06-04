# Contributing

## How to contribute

1. Fork the repo
2. Create a branch: `git checkout -b feature/your-idea`
3. Make your changes
4. Open a Pull Request against `master`

All PRs require **1 approving review** before merge. Direct pushes to `master` are blocked.

## Guidelines

- Keep it Windows-compatible (no WSL dependencies)
- No new runtime dependencies — Node.js built-ins only
- Cache expensive calls (API, filesystem scans) — don't slow warm render below 0.5s
- Follow the existing Catppuccin Mocha color scheme

## Reporting issues

Open a GitHub Issue with:
- Your Windows version
- Node.js version (`node --version`)
- What you expected vs what you saw
