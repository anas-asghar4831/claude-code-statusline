---
name: sl
description: Toggle compact statusline for THIS session only (hides cost/token/prayer/etc. rows, keeps dir, model, ctx bar, title, effort, rate limits).
disable-model-invocation: true
allowed-tools: Bash(bash *)
---

Run exactly this and report its one-line output, nothing else:

```bash
bash -c 'f="$HOME/.claude/statusline/compact/${CLAUDE_SESSION_ID}"; mkdir -p "$(dirname "$f")"; if [ -e "$f" ]; then rm "$f"; echo "compact OFF"; else : > "$f"; echo "compact ON"; fi'
```
