---
name: sl
description: Toggle expanded statusline for THIS session only (compact is the default; expanded adds cost/token/prayer/etc. rows).
disable-model-invocation: true
allowed-tools: Bash(bash *)
---

Run exactly this and report its one-line output, nothing else:

```bash
bash -c 'f="$HOME/.claude/statusline/expanded/${CLAUDE_SESSION_ID}"; mkdir -p "$(dirname "$f")"; if [ -e "$f" ]; then rm "$f"; echo "compact (default)"; else : > "$f"; echo "expanded"; fi'
```
