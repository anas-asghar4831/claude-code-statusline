#!/bin/bash
# Claude Code Enhanced Statusline
# Techniques: JSONL native cost calc + Catppuccin true-color + burn rate

input=$(cat)
NOW_TS=$(date +%s)
TODAY=$(date +%Y-%m-%d)
CLOCK=$(date +"%I:%M %p")
DATE_FULL=$(date +"%a %b %-d, %Y" 2>/dev/null || date +"%a %b %d, %Y")

# ── Catppuccin Mocha true-color theme ────────────────────────────────────────
R=$'\033[0m'
RED=$'\033[38;2;243;139;168m'       # #f38ba8
GREEN=$'\033[38;2;166;227;161m'     # #a6e3a1
YELLOW=$'\033[38;2;249;226;175m'    # #f9e2af
BLUE=$'\033[38;2;137;180;250m'      # #89b4fa
MAGENTA=$'\033[38;2;203;166;247m'   # #cba6f7
CYAN=$'\033[38;2;137;220;235m'      # #89dceb
ORANGE=$'\033[38;2;250;179;135m'    # #fab387
TEAL=$'\033[38;2;148;226;213m'      # #94e2d5
GRAY=$'\033[38;2;166;173;200m'      # #a6adc8
WHITE=$'\033[38;2;205;214;244m'     # #cdd6f4
GOLD=$'\033[38;2;249;226;175m'      # #f9e2af
PINK=$'\033[38;2;245;194;231m'      # #f5c2e7
BOLD=$'\033[1m'
DIM=$'\033[2m'

# ── Parse stdin JSON (single jq call → all fields, tab-separated) ─────────────
IFS=$'\t' read -r MODEL MODEL_ID VERSION DIR SESSION_ID TRANSCRIPT PCT \
    LIVE_COST DUR_MS LINES_ADD LINES_DEL IN_TOK OUT_TOK CACHE_CR CACHE_RD \
    RATE_5H RATE_7D RATE_5H_RESET RATE_7D_RESET <<< "$(echo "$input" | jq -r '[
    (.model.display_name // "?"),
    (.model.id // ""),
    (.version // "?"),
    (.workspace.current_dir // "."),
    (.session_id // "x"),
    (.transcript_path // ""),
    ((.context_window.used_percentage // 0) | floor),
    (.cost.total_cost_usd // 0),
    (.cost.total_duration_ms // 0),
    (.cost.total_lines_added // 0),
    (.cost.total_lines_removed // 0),
    (.context_window.current_usage.input_tokens // 0),
    (.context_window.current_usage.output_tokens // 0),
    (.context_window.current_usage.cache_creation_input_tokens // 0),
    (.context_window.current_usage.cache_read_input_tokens // 0),
    (.rate_limits.five_hour.used_percentage // ""),
    (.rate_limits.seven_day.used_percentage // ""),
    (.rate_limits.five_hour.resets_at // ""),
    (.rate_limits.seven_day.resets_at // "")
    ] | @tsv')"
DIR=$(echo "$DIR" | sed 's|\\|/|g')

# ── Model pricing (per million tokens: input output cache_write cache_read) ───
get_pricing() {
    case "$1" in
        claude-opus-4-8*|claude-opus-4-7*|claude-opus-4-6*|claude-opus-4-5*)
            echo "5.00 25.00 6.25 0.50" ;;
        claude-opus-4*|claude-3-opus*)
            echo "15.00 75.00 18.75 1.50" ;;
        claude-sonnet-4*|claude-3-7-sonnet*|claude-3-5-sonnet*)
            echo "3.00 15.00 3.75 0.30" ;;
        claude-haiku-4-5*)
            echo "1.00 5.00 1.25 0.10" ;;
        claude-haiku-3-5*|claude-3-5-haiku*)
            echo "0.80 4.00 1.00 0.08" ;;
        *)
            echo "3.00 15.00 3.75 0.30" ;;
    esac
}

# ── JSONL cost calculation ────────────────────────────────────────────────────
PROJECTS_DIR="$HOME/.claude/projects"
COST_CACHE="$HOME/.claude/statusline/cost-cache.json"

calc_jsonl_costs() {
    local today_start week_start month_start
    today_start=$(date -d "$(date +%Y-%m-%d)" -u "+%Y-%m-%dT%H:%M:%S" 2>/dev/null || \
                  date -u -r "$(date -j -f '%Y-%m-%d' "$(date +%Y-%m-%d)" '+%s' 2>/dev/null)" "+%Y-%m-%dT%H:%M:%S" 2>/dev/null || \
                  echo "${TODAY}T00:00:00")
    week_start=$(date -d "7 days ago" -u "+%Y-%m-%dT%H:%M:%S" 2>/dev/null || echo "")
    month_start=$(date -d "30 days ago" -u "+%Y-%m-%dT%H:%M:%S" 2>/dev/null || echo "")

    # Pricing map for awk (sonnet default)
    local pricing; pricing=$(get_pricing "$MODEL_ID")
    read -r P_IN P_OUT P_CW P_CR <<< "$pricing"

    # Derive current project dir from workspace path (C:/foo/bar → C--foo-bar)
    local proj_slug; proj_slug=$(echo "$DIR" | sed 's|[:/\\]|-|g')
    local proj_dir="$PROJECTS_DIR/$proj_slug"

    # jq filter shared by both scans (single process via xargs, not per-file)
    local JQF='select(.type=="assistant") | select(.message.usage) |
        [.timestamp,
         (.message.usage.input_tokens // 0),
         (.message.usage.output_tokens // 0),
         (.message.usage.cache_creation_input_tokens // 0),
         (.message.usage.cache_read_input_tokens // 0),
         (.message.id // .requestId // .uuid // "")] | @tsv'

    # REPO: current project JSONL only (all time) — single jq via xargs
    local repo_cost="0.00"
    if [ -d "$proj_dir" ]; then
        repo_cost=$(find "$proj_dir" -name "*.jsonl" -print0 2>/dev/null | \
        xargs -0 jq -r "$JQF" 2>/dev/null | \
        awk -F'\t' -v p_in="$P_IN" -v p_out="$P_OUT" -v p_cw="$P_CW" -v p_cr="$P_CR" \
        'BEGIN{c=0} {id=$6; if(id!=""&&seen[id]++)next; c+=($2*p_in+$3*p_out+$4*p_cw+$5*p_cr)/1000000}
         END{printf "%.2f",c}')
    fi

    # DAY/7DAY/30DAY: all projects (last 31 days of files) — single jq via xargs
    local period_costs
    period_costs=$(find "$PROJECTS_DIR" -name "*.jsonl" -mtime -31 -print0 2>/dev/null | \
    xargs -0 jq -r "$JQF" 2>/dev/null | \
    awk -F'\t' \
        -v today="$today_start" \
        -v week="$week_start" \
        -v month="$month_start" \
        -v p_in="$P_IN" -v p_out="$P_OUT" -v p_cw="$P_CW" -v p_cr="$P_CR" \
    'BEGIN { day=0; week_c=0; month_c=0 }
    {
        id=$6; if (id != "" && seen[id]++) next
        ts=$1; gsub(/\.[0-9]+Z?$/, "", ts)
        cost=($2*p_in + $3*p_out + $4*p_cw + $5*p_cr) / 1000000
        if (month != "" && ts >= month) month_c += cost
        if (week != "" && ts >= week) week_c += cost
        if (ts >= today) day += cost
    }
    END { printf "%.2f:%.2f:%.2f", month_c, week_c, day }')

    echo "${repo_cost}:${period_costs}"
}

# Load/refresh cost cache (5min TTL — period costs change slowly)
NEED_COST=1
if [ -f "$COST_CACHE" ]; then
    CACHE_TS=$(jq -r '.ts // 0' "$COST_CACHE" 2>/dev/null || echo 0)
    [ $((NOW_TS - CACHE_TS)) -lt 300 ] && NEED_COST=0
fi

if [ "$NEED_COST" = "1" ] && [ -d "$PROJECTS_DIR" ]; then
    COST_DATA=$(calc_jsonl_costs)
    jq -n --arg d "$COST_DATA" --argjson ts "$NOW_TS" '{data:$d, ts:$ts}' > "$COST_CACHE" 2>/dev/null
else
    COST_DATA=$(jq -r '.data // "0:0:0:0"' "$COST_CACHE" 2>/dev/null || echo "0:0:0:0")
fi

IFS=':' read -r REPO_COST D30_COST D7_COST DAY_COST <<< "$COST_DATA"
REPO_FMT=$(printf '$%.2f' "$REPO_COST")
D30_FMT=$(printf '$%.2f' "$D30_COST")
D7_FMT=$(printf '$%.2f' "$D7_COST")
DAY_FMT=$(printf '$%.2f' "$DAY_COST")
LIVE_FMT=$(printf '$%.2f' "$LIVE_COST")

# ── Burn rate (tokens/min + cost/hr from JSONL last 5min) ────────────────────
BURN_CACHE="$HOME/.claude/statusline/burn-cache.json"
NEED_BURN=1
if [ -f "$BURN_CACHE" ]; then
    B_TS=$(jq -r '.ts // 0' "$BURN_CACHE" 2>/dev/null || echo 0)
    [ $((NOW_TS - B_TS)) -lt 30 ] && NEED_BURN=0
fi

if [ "$NEED_BURN" = "1" ] && [ -n "$TRANSCRIPT" ] && [ -f "$TRANSCRIPT" ]; then
    FIVE_MIN_AGO=$(date -d "5 minutes ago" -u "+%Y-%m-%dT%H:%M:%S" 2>/dev/null || \
                   date -u -d "@$((NOW_TS - 300))" "+%Y-%m-%dT%H:%M:%S" 2>/dev/null || echo "")
    if [ -n "$FIVE_MIN_AGO" ]; then
        pricing=$(get_pricing "$MODEL_ID")
        read -r P_IN P_OUT P_CW P_CR <<< "$pricing"
        # Use current session transcript only — accurate burn rate for this session
        BURN_DATA=$(jq -r 'select(.type=="assistant") | select(.message.usage) |
                select(.timestamp >= "'"$FIVE_MIN_AGO"'") |
                [(.message.usage.input_tokens // 0),
                 (.message.usage.output_tokens // 0),
                 (.message.usage.cache_creation_input_tokens // 0),
                 (.message.usage.cache_read_input_tokens // 0),
                 (.message.id // .requestId // .uuid // "")] | @tsv' "$TRANSCRIPT" 2>/dev/null | \
            awk -F'\t' \
                -v p_in="$P_IN" -v p_out="$P_OUT" -v p_cw="$P_CW" -v p_cr="$P_CR" \
                -v window=300 \
            'BEGIN { toks=0; cost=0 }
            {
                id=$5; if (id != "" && seen[id]++) next
                toks += $1+0 + $2+0
                cost += ($1*p_in + $2*p_out + $3*p_cw + $4*p_cr) / 1000000
            }
            END {
                tpm = toks / (window/60)
                cph = cost * 12
                printf "%.0f:%.2f", tpm, cph
            }')
        jq -n --arg d "$BURN_DATA" --argjson ts "$NOW_TS" '{data:$d, ts:$ts}' > "$BURN_CACHE" 2>/dev/null
    fi
fi
BURN_DATA=$(jq -r '.data // "0:0.00"' "$BURN_CACHE" 2>/dev/null || echo "0:0.00")
IFS=':' read -r BURN_TPM BURN_CPH <<< "$BURN_DATA"
BURN_FMT=$(printf '$%.2f' "$BURN_CPH")

# ── Git info ──────────────────────────────────────────────────────────────────
GIT_BRANCH=""; GIT_COMMITS="0"; GIT_ICON="✅"
if git -C "$DIR" rev-parse --git-dir >/dev/null 2>&1; then
    GIT_BRANCH=$(git -C "$DIR" branch --show-current 2>/dev/null)
    GIT_COMMITS=$(git -C "$DIR" rev-list --count HEAD 2>/dev/null || echo "0")
    DIRTY=$(git -C "$DIR" status --porcelain 2>/dev/null | wc -l | tr -d ' ')
    [ "$DIRTY" -gt 0 ] && GIT_ICON="⚠️" || GIT_ICON="✅"
fi

# ── Cache hit % ───────────────────────────────────────────────────────────────
CACHE_HIT=$(awk -v rd="$CACHE_RD" -v cr="$CACHE_CR" -v it="$IN_TOK" \
    'BEGIN{ t=it+cr+rd; if(t>0) printf "%d", rd*100/t; else print "0" }')
CTX_TOTAL=$((IN_TOK+CACHE_CR+CACHE_RD))
TOK_DISPLAY=$(awk -v t="$CTX_TOTAL" 'BEGIN{ if(t>=1000000) printf "%.1fM", t/1000000; else printf "%dk", t/1000 }')

# ── Colors based on thresholds ────────────────────────────────────────────────
[ "$PCT" -ge 90 ] && CTX_C="$RED" || { [ "$PCT" -ge 70 ] && CTX_C="$YELLOW" || CTX_C="$GREEN"; }
BURN_C="$GREEN"
awk -v x="$BURN_CPH" 'BEGIN{exit !(x>3)}' && BURN_C="$ORANGE"
awk -v x="$BURN_CPH" 'BEGIN{exit !(x>8)}' && BURN_C="$RED"

# ── Rate limits ───────────────────────────────────────────────────────────────
RATE_STR=""
if [ -n "$RATE_5H_RESET" ]; then
    RESET_T=$(date -d "@$RATE_5H_RESET" "+%I:%M %p" 2>/dev/null || echo "?")
    MINS_LEFT=$(( (RATE_5H_RESET - NOW_TS) / 60 ))
    HRS=$((MINS_LEFT/60)); MREM=$((MINS_LEFT%60))
    R5=$(printf '%.0f' "${RATE_5H:-0}")
    R7=$(printf '%.0f' "${RATE_7D:-0}")
    RATE_STR="⊙ 5H ${RESET_T} (${HRS}h ${MREM}m) ${R5}%"
    if [ -n "$RATE_7D_RESET" ]; then
        R7_T=$(date -d "@$RATE_7D_RESET" "+%a %I:%M %p" 2>/dev/null || echo "?")
        D7MINS=$(( (RATE_7D_RESET - NOW_TS) / 60 ))
        D7H=$((D7MINS/60)); D7D=$((D7H/24)); D7HR=$((D7H%24))
        RATE_STR="${RATE_STR} • 7DAY ${R7_T} (${D7D}d ${D7HR}h) ${R7}%"
    else
        RATE_STR="${RATE_STR} • 7DAY ${R7}%"
    fi
fi

# ── Location (cached 6hr) ────────────────────────────────────────────────────
LOC_CACHE="$HOME/.claude/statusline/location-cache.json"
NEED_LOC=1
if [ -f "$LOC_CACHE" ]; then
    L_AGE=$(( NOW_TS - $(jq -r '.cached_at // 0' "$LOC_CACHE" 2>/dev/null || echo 0) ))
    [ "$L_AGE" -lt 21600 ] && NEED_LOC=0
fi
if [ "$NEED_LOC" = "1" ]; then
    LOC_JSON=$(curl -sL --max-time 4 "http://ip-api.com/json/" 2>/dev/null)
    echo "$LOC_JSON" | jq -e '.status == "success"' >/dev/null 2>&1 && \
        echo "$LOC_JSON" | jq --argjson ts "$NOW_TS" '. + {cached_at: $ts}' > "$LOC_CACHE"
fi
LOC_CITY=$(jq -r '.city // ""' "$LOC_CACHE" 2>/dev/null)
LOC_COUNTRY=$(jq -r '.country // ""' "$LOC_CACHE" 2>/dev/null)
LOC_LAT=$(jq -r '.lat // ""' "$LOC_CACHE" 2>/dev/null)
LOC_LON=$(jq -r '.lon // ""' "$LOC_CACHE" 2>/dev/null)

# ── Prayer times (cached per day) ────────────────────────────────────────────
PRAYER_CACHE="$HOME/.claude/statusline/prayer-cache.json"
NEED_PRAYER=1
[ -f "$PRAYER_CACHE" ] && \
    [ "$(jq -r '.date // ""' "$PRAYER_CACHE" 2>/dev/null)" = "$TODAY" ] && NEED_PRAYER=0

if [ "$NEED_PRAYER" = "1" ] && [ -n "$LOC_LAT" ]; then
    PJSON=$(curl -sL --max-time 5 \
        "https://api.aladhan.com/v1/timings?latitude=${LOC_LAT}&longitude=${LOC_LON}&method=1" 2>/dev/null)
    echo "$PJSON" | jq -e '.data.timings' >/dev/null 2>&1 && \
        echo "$PJSON" | jq --argjson ts "$NOW_TS" --arg d "$TODAY" \
            '. + {cached_at: $ts, date: $d}' > "$PRAYER_CACHE"
fi

# ── Prayer formatting with colors ────────────────────────────────────────────
# Returns: color + name + 12h time + status
# States: passed=gray+✓  next=yellow+countdown  future=cyan

to12h() {
    # Convert 24h "HH:MM" to 12h "H:MM AM/PM"
    local t="$1"
    local h=${t%%:*} m=${t##*:}
    local th=${h#0}; [ -z "$th" ] && th=0
    local ampm="AM"
    [ "$th" -ge 12 ] && ampm="PM"
    [ "$th" -gt 12 ] && th=$((th-12))
    [ "$th" -eq 0 ] && th=12
    printf "%d:%s %s" "$th" "$m" "$ampm"
}

prayer_ts() {
    # Return epoch for today HH:MM
    local t="$1"
    date -d "today $t" +%s 2>/dev/null || echo 0
}

PRAYER_LINE=""
if [ -f "$PRAYER_CACHE" ] && jq -e '.data.timings' "$PRAYER_CACHE" >/dev/null 2>&1; then
    FAJR=$(jq -r '.data.timings.Fajr' "$PRAYER_CACHE")
    DHUHR=$(jq -r '.data.timings.Dhuhr' "$PRAYER_CACHE")
    ASR=$(jq -r '.data.timings.Asr' "$PRAYER_CACHE")
    MAGHRIB=$(jq -r '.data.timings.Maghrib' "$PRAYER_CACHE")
    ISHA=$(jq -r '.data.timings.Isha' "$PRAYER_CACHE")
    HIJ=$(jq -r '"\(.data.date.hijri.day) \(.data.date.hijri.month.en) \(.data.date.hijri.year)"' "$PRAYER_CACHE")

    # Find next upcoming prayer index (0=fajr 1=dhuhr 2=asr 3=maghrib 4=isha)
    PRAYER_NAMES=("Fajr" "Dhuhr" "Asr" "Maghrib" "Isha")
    PRAYER_TIMES=("$FAJR" "$DHUHR" "$ASR" "$MAGHRIB" "$ISHA")
    NEXT_IDX=-1
    for i in 0 1 2 3 4; do
        pts=$(prayer_ts "${PRAYER_TIMES[$i]}")
        if [ "$pts" -gt "$NOW_TS" ] 2>/dev/null; then
            NEXT_IDX=$i
            break
        fi
    done

    pfmt_colored() {
        local idx="$1" t="$2" name="$3"
        local t12; t12=$(to12h "$t")
        local pts; pts=$(prayer_ts "$t")
        local diff=$(( pts - NOW_TS ))

        if [ "$pts" -le "$NOW_TS" ] 2>/dev/null; then
            # Passed — gray + checkmark
            printf "%s%s %s ✓%s" "$GRAY" "$name" "$t12" "$R"
        elif [ "$idx" = "$NEXT_IDX" ]; then
            # Next upcoming — yellow + countdown
            local dm=$((diff/60)) dh=$((diff/3600)) drem=$(( (diff%3600)/60 ))
            if [ "$dh" -gt 0 ]; then
                printf "%s%s %s (%dh %dm)%s" "$YELLOW" "$name" "$t12" "$dh" "$drem" "$R"
            else
                printf "%s%s %s (%dm)%s" "$YELLOW" "$name" "$t12" "$dm" "$R"
            fi
        else
            # Future — cyan
            printf "%s%s %s%s" "$CYAN" "$name" "$t12" "$R"
        fi
    }

    P0=$(pfmt_colored 0 "$FAJR"   "Fajr")
    P1=$(pfmt_colored 1 "$DHUHR"  "Dhuhr")
    P2=$(pfmt_colored 2 "$ASR"    "Asr")
    P3=$(pfmt_colored 3 "$MAGHRIB" "Maghrib")
    P4=$(pfmt_colored 4 "$ISHA"   "Isha")
    PSEP="${GRAY} | ${R}"
    PRAYER_LINE="${P0}${PSEP}${P1}${PSEP}${P2}${PSEP}${P3}${PSEP}${P4}"
fi

# ── MCP plugins ───────────────────────────────────────────────────────────────
MCP_SETTINGS="$HOME/.claude/settings.json"
MCP_COUNT=$(jq '[.enabledPlugins | to_entries[] | select(.value==true)] | length' "$MCP_SETTINGS" 2>/dev/null || echo "0")
# Array of plugin names for wrapping
mapfile -t MCP_ARR < <(jq -r '.enabledPlugins | to_entries[] | select(.value==true) | .key | split("@")[0]' "$MCP_SETTINGS" 2>/dev/null)

# ── Separator (plain | avoids Windows Terminal rendering issues with │+ANSI) ──
SEP="${GRAY} | ${R}"

# ── Token display helpers ─────────────────────────────────────────────────────
fmt_tok() { awk -v t="$1" 'BEGIN{ if(t>=1000000) printf "%.1fM",t/1000000; else if(t>=1000) printf "%.1fk",t/1000; else printf "%d",t }'; }
# ── Cumulative session totals (sum all turns from this session's transcript) ──
SESS_IN=0; SESS_OUT=0; SESS_CW=0; SESS_CR=0
if [ -n "$TRANSCRIPT" ] && [ -f "$TRANSCRIPT" ]; then
    SESS_DATA=$(jq -r 'select(.type=="assistant") | select(.message.usage) |
        [(.message.usage.input_tokens // 0),
         (.message.usage.output_tokens // 0),
         (.message.usage.cache_creation_input_tokens // 0),
         (.message.usage.cache_read_input_tokens // 0),
         (.message.id // .requestId // .uuid // "")] | @tsv' "$TRANSCRIPT" 2>/dev/null | \
        awk -F'\t' 'BEGIN{i=0;o=0;w=0;r=0}
            {id=$5; if(id!="" && seen[id]++) next; i+=$1; o+=$2; w+=$3; r+=$4}
            END{printf "%d:%d:%d:%d", i, o, w, r}')
    IFS=':' read -r SESS_IN SESS_OUT SESS_CW SESS_CR <<< "$SESS_DATA"
fi
IN_D=$(fmt_tok "$SESS_IN")
OUT_D=$(fmt_tok "$SESS_OUT")
CW_D=$(fmt_tok "$SESS_CW")
CR_D=$(fmt_tok "$SESS_CR")
SESS_TOTAL=$((SESS_IN+SESS_OUT+SESS_CW+SESS_CR))
TOTAL_D=$(fmt_tok "$SESS_TOTAL")

# Context bar
BAR_FILLED=$((PCT/10)); BAR_EMPTY=$((10-BAR_FILLED))
printf -v BAR_F "%${BAR_FILLED}s"; printf -v BAR_E "%${BAR_EMPTY}s"
CTX_BAR="${BAR_F// /█}${BAR_E// /░}"

# ── Line 1: folder (branch) │ ctx bar │ In Out Cache Read Total ──────────────
[ "$PCT" -ge 90 ] && CTX_C="$RED" || { [ "$PCT" -ge 70 ] && CTX_C="$YELLOW" || CTX_C="$GREEN"; }
BRANCH_STR=""; [ -n "$GIT_BRANCH" ] && BRANCH_STR=" ${TEAL}(${GIT_BRANCH})${R}"
FOLDER="${DIR##*/}"
L1="${WHITE}${BOLD}${FOLDER}${R}${BRANCH_STR} ${GIT_ICON}"
L1+=" ${SEP} 🧠 ${MAGENTA}${MODEL}${R}"
L1+=" ${SEP} ${CTX_C}${CTX_BAR} ${PCT}%${R}"
L1+=" ${SEP} ${GRAY}In:${R}${BLUE}${IN_D}${R}"
L1+=" ${SEP} ${GRAY}Out:${R}${MAGENTA}${OUT_D}${R}"
L1+=" ${SEP} ${GRAY}Write:${R}${ORANGE}${CW_D}${R}"
L1+=" ${SEP} ${GRAY}Read:${R}${TEAL}${CR_D}${R}"
L1+=" ${SEP} ${GRAY}Total:${R}${WHITE}${TOTAL_D}${R}"
printf '%s\n' "$L1"

# ── Line 2: rate limits | changes ────────────────────────────────────────────
RATE_LINE="${GRAY}No rate limit data${R}"
[ -n "$RATE_STR" ] && RATE_LINE="${YELLOW}${RATE_STR}${R}"
L2="${RATE_LINE}"
L2+=" ${SEP} ${GREEN}+${LINES_ADD}${R}/${RED}-${LINES_DEL}${R}"
L2+=" ${SEP} ${GRAY}Commits:${R}${GOLD}${GIT_COMMITS}${R}"
L2+=" ${SEP} ${BOLD}🕐 ${CLOCK}${R}"
printf '%s\n' "$L2"

# ── Line 3: all money ─────────────────────────────────────────────────────────
BURN_C="$GREEN"
awk -v x="$BURN_CPH" 'BEGIN{exit !(x>3)}' && BURN_C="$ORANGE"
awk -v x="$BURN_CPH" 'BEGIN{exit !(x>8)}' && BURN_C="$RED"
L3="${GRAY}REPO${R} ${YELLOW}${REPO_FMT}${R}"
L3+=" ${SEP} ${GRAY}30D${R} ${YELLOW}${D30_FMT}${R}"
L3+=" ${SEP} ${GRAY}7D${R} ${YELLOW}${D7_FMT}${R}"
L3+=" ${SEP} ${GRAY}DAY${R} ${YELLOW}${DAY_FMT}${R}"
L3+=" ${SEP} 🔥 ${ORANGE}LIVE ${LIVE_FMT}${R}"
L3+=" ${SEP} ${BURN_C}${BURN_FMT}/hr${R}"
L3+=" ${SEP} ${GRAY}Cache hit:${R} ${GREEN}${CACHE_HIT}%${R}"
printf '%s\n' "$L3"

# ── Line 4: date ──────────────────────────────────────────────────────────────
if [ -n "$PRAYER_LINE" ]; then
    printf '%s%s%s %s %s%s%s\n' \
        "$GRAY" "$HIJ" "$R" "$SEP" "$WHITE" "$DATE_FULL" "$R"
else
    printf '%s%s%s\n' "$WHITE" "$DATE_FULL" "$R"
fi

# ── Line 5: prayer times ──────────────────────────────────────────────────────
[ -n "$PRAYER_LINE" ] && printf '🕌 %s\n' "$PRAYER_LINE"

# ── Line 6: location + MCP count ─────────────────────────────────────────────
LOC_DISPLAY="${LOC_CITY}${LOC_CITY:+, }${LOC_COUNTRY}"
[ -z "$LOC_DISPLAY" ] && LOC_DISPLAY="Unknown"
L6="📍 ${GRAY}${LOC_DISPLAY}${R} ${SEP} ${GRAY}Plugins:${R}${CYAN}${MCP_COUNT}${R}"
printf '%s\n' "$L6"

# ── Line 7+: plugin names wrapped (5 per line) ───────────────────────────────
PER_LINE=5
total=${#MCP_ARR[@]}
i=0
while [ "$i" -lt "$total" ]; do
    chunk=""
    for j in $(seq 0 $((PER_LINE-1))); do
        idx=$((i+j))
        [ "$idx" -ge "$total" ] && break
        [ -n "$chunk" ] && chunk="${chunk}${GRAY}, ${R}"
        chunk="${chunk}${TEAL}${MCP_ARR[$idx]}${R}"
    done
    printf '   %s\n' "$chunk"
    i=$((i+PER_LINE))
done
