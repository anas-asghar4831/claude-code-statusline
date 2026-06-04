#!/usr/bin/env node
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const curl = (url) => execFileSync('curl', ['-sL', '--max-time', '5', url], { stdio: ['ignore', 'pipe', 'ignore'] }).toString();

const HOME = os.homedir();
const SL_DIR = path.join(HOME, '.claude', 'statusline');
const PROJECTS_DIR = path.join(HOME, '.claude', 'projects');
const NOW = Math.floor(Date.now() / 1000);

// ── Read stdin ────────────────────────────────────────────────────────────────
let raw = '';
try { raw = fs.readFileSync(0, 'utf8'); } catch (_) {}
let J = {};
try { J = JSON.parse(raw); } catch (_) {}

// ── Catppuccin Mocha true-color ───────────────────────────────────────────────
const R = '\x1b[0m';
const C = {
  red: '\x1b[38;2;243;139;168m', green: '\x1b[38;2;166;227;161m',
  yellow: '\x1b[38;2;249;226;175m', blue: '\x1b[38;2;137;180;250m',
  magenta: '\x1b[38;2;203;166;247m', cyan: '\x1b[38;2;137;220;235m',
  orange: '\x1b[38;2;250;179;135m', teal: '\x1b[38;2;148;226;213m',
  gray: '\x1b[38;2;166;173;200m', white: '\x1b[38;2;205;214;244m',
  gold: '\x1b[38;2;249;226;175m', bold: '\x1b[1m',
  pink: '\x1b[38;2;245;194;231m',
};
const SEP = `${C.gray} | ${R}`;

// ── Field extraction ──────────────────────────────────────────────────────────
const g = (o, p, d) => p.split('.').reduce((a, k) => (a && a[k] !== undefined ? a[k] : undefined), o) ?? d;

// identity
const MODEL        = g(J, 'model.display_name', '?');
const MODEL_ID     = g(J, 'model.id', '');
const SESSION_ID   = g(J, 'session_id', '');
const SESSION_NAME = g(J, 'session_name', '');
const VERSION      = g(J, 'version', '');

// dirs
const DIR          = String(g(J, 'workspace.current_dir', '.')).replace(/\\/g, '/');
const PROJECT_DIR  = String(g(J, 'workspace.project_dir', '')).replace(/\\/g, '/');
const TRANSCRIPT   = String(g(J, 'transcript_path', '')).replace(/\\/g, '/');
const ADDED_DIRS   = g(J, 'workspace.added_dirs', []);
const GIT_WORKTREE = g(J, 'workspace.git_worktree', '');

// repo
const REPO_OWNER   = g(J, 'workspace.repo.owner', '');
const REPO_NAME    = g(J, 'workspace.repo.name', '');

// context window
const PCT          = Math.floor(Number(g(J, 'context_window.used_percentage', 0)) || 0);
const REM_PCT      = Math.floor(Number(g(J, 'context_window.remaining_percentage', 0)) || 0);
const CTX_TOTAL_IN = Number(g(J, 'context_window.total_input_tokens', 0)) || 0;
const CTX_TOTAL_OUT= Number(g(J, 'context_window.total_output_tokens', 0)) || 0;
const CTX_SIZE     = Number(g(J, 'context_window.context_window_size', 200000)) || 200000;
const EXCEEDS_200K = g(J, 'exceeds_200k_tokens', false);
const CTX_TOKENS   = (Number(g(J, 'context_window.current_usage.input_tokens', 0)) || 0)
  + (Number(g(J, 'context_window.current_usage.cache_creation_input_tokens', 0)) || 0)
  + (Number(g(J, 'context_window.current_usage.cache_read_input_tokens', 0)) || 0);

// cost & timing
const LIVE_COST    = Number(g(J, 'cost.total_cost_usd', 0)) || 0;
const LINES_ADD    = g(J, 'cost.total_lines_added', 0);
const LINES_DEL    = g(J, 'cost.total_lines_removed', 0);
const DURATION_MS  = Number(g(J, 'cost.total_duration_ms', 0)) || 0;
const API_DUR_MS   = Number(g(J, 'cost.total_api_duration_ms', 0)) || 0;

// rate limits
const RATE_5H      = g(J, 'rate_limits.five_hour.used_percentage', null);
const RATE_7D      = g(J, 'rate_limits.seven_day.used_percentage', null);
const RATE_5H_RESET= g(J, 'rate_limits.five_hour.resets_at', null);
const RATE_7D_RESET= g(J, 'rate_limits.seven_day.resets_at', null);

// session flags
const EFFORT       = g(J, 'effort.level', '');
const THINKING     = g(J, 'thinking.enabled', false);
const FAST_MODE    = g(J, 'fast_mode', false);
const OUTPUT_STYLE = g(J, 'output_style.name', '');
const VIM_MODE     = g(J, 'vim.mode', '');
const AGENT_NAME   = g(J, 'agent.name', '');

// PR & worktree
const PR_NUM       = g(J, 'pr.number', null);
const PR_URL       = g(J, 'pr.url', '');
const PR_STATE     = g(J, 'pr.review_state', '');
const WT_NAME      = g(J, 'worktree.name', '') || GIT_WORKTREE;
const WT_BRANCH    = g(J, 'worktree.branch', '');

// ── Pricing [in, out, cacheWrite, cacheRead] per Mtok ────────────────────────
function pricing(id) {
  if (/^claude-opus-4-[5-8]/.test(id)) return [5, 25, 6.25, 0.5];
  if (/^claude-opus-4|^claude-3-opus/.test(id)) return [15, 75, 18.75, 1.5];
  if (/^claude-sonnet-4|^claude-3-7-sonnet|^claude-3-5-sonnet/.test(id)) return [3, 15, 3.75, 0.3];
  if (/^claude-haiku-4-5/.test(id)) return [1, 5, 1.25, 0.1];
  if (/^claude-haiku-3-5|^claude-3-5-haiku/.test(id)) return [0.8, 4, 1, 0.08];
  return [3, 15, 3.75, 0.3];
}
const P = pricing(MODEL_ID);

// ── Cache helpers ─────────────────────────────────────────────────────────────
function readCache(name) {
  try { return JSON.parse(fs.readFileSync(path.join(SL_DIR, name), 'utf8')); } catch (_) { return null; }
}
function writeCache(name, obj) {
  try { fs.writeFileSync(path.join(SL_DIR, name), JSON.stringify(obj)); } catch (_) {}
}

// ── JSONL helpers ─────────────────────────────────────────────────────────────
function jsonlFiles(dir, maxAgeDays) {
  const out = [], cutoff = maxAgeDays ? Date.now() - maxAgeDays * 86400000 : 0;
  let entries;
  try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch (_) { return out; }
  for (const e of entries) {
    const full = path.join(dir, e.name);
    if (e.isDirectory()) out.push(...jsonlFiles(full, maxAgeDays));
    else if (e.name.endsWith('.jsonl')) {
      if (cutoff) { try { if (fs.statSync(full).mtimeMs < cutoff) continue; } catch (_) { continue; } }
      out.push(full);
    }
  }
  return out;
}

function* usageRows(file) {
  let data;
  try { data = fs.readFileSync(file, 'utf8'); } catch (_) { return; }
  for (const line of data.split('\n')) {
    if (!line || line.indexOf('"assistant"') === -1) continue;
    let o; try { o = JSON.parse(line); } catch (_) { continue; }
    if (o.type !== 'assistant') continue;
    const u = o.message && o.message.usage; if (!u) continue;
    yield {
      ts: o.timestamp || '', model: (o.message && o.message.model) || 'default',
      in: u.input_tokens || 0, out: u.output_tokens || 0,
      cw: u.cache_creation_input_tokens || 0, cr: u.cache_read_input_tokens || 0,
      id: (o.message && o.message.id) || o.requestId || o.uuid || '',
    };
  }
}

const rowCost = (r) => (r.in * P[0] + r.out * P[1] + r.cw * P[2] + r.cr * P[3]) / 1e6;

// ── Cost calc (REPO + DAY/7D/30D), cached 5min ───────────────────────────────
function calcCosts() {
  // use LAUNCH_DIR for REPO so worktrees show main project cost, not worktree-specific cost
  const repoBase = LAUNCH_DIR || DIR;
  const slug = repoBase.replace(/[:/\\]/g, '-');
  const projDir = path.join(PROJECTS_DIR, slug);
  let repo = 0;
  const seenR = new Set();
  for (const f of jsonlFiles(projDir, 0)) {
    for (const r of usageRows(f)) {
      if (r.id && seenR.has(r.id)) continue;
      if (r.id) seenR.add(r.id);
      repo += rowCost(r);
    }
  }
  const today = new Date(); today.setHours(0, 0, 0, 0);
  const todayISO = new Date(today.getTime() - today.getTimezoneOffset() * 60000).toISOString().slice(0, 19);
  const weekISO  = new Date(Date.now() - 7 * 86400000).toISOString().slice(0, 19);
  const monthISO = new Date(Date.now() - 30 * 86400000).toISOString().slice(0, 19);
  let day = 0, week = 0, month = 0;
  const seenP = new Set();
  for (const f of jsonlFiles(PROJECTS_DIR, 31)) {
    for (const r of usageRows(f)) {
      if (r.id && seenP.has(r.id)) continue;
      if (r.id) seenP.add(r.id);
      const ts = r.ts.replace(/\.\d+Z?$/, ''), c = rowCost(r);
      if (ts >= monthISO) month += c;
      if (ts >= weekISO)  week  += c;
      if (ts >= todayISO) day   += c;
    }
  }
  return { repo, month, week, day };
}

let costData;
const cc = readCache('cost-cache.json');
if (cc && cc.d && NOW - cc.ts < 300) costData = cc.d;
else { costData = calcCosts(); writeCache('cost-cache.json', { d: costData, ts: NOW }); }

// ── Burn rate (last 5min, session-scoped, 30s cache) ─────────────────────────
let burn = { tpm: 0, cph: 0 };
const bc = readCache('burn-cache.json');
if (bc && bc.d && bc.sid === SESSION_ID && NOW - bc.ts < 30) burn = bc.d;
else if (TRANSCRIPT && fs.existsSync(TRANSCRIPT)) {
  const fiveMinAgo = new Date(Date.now() - 300000).toISOString().slice(0, 19);
  let toks = 0, cst = 0;
  const seen = new Set();
  for (const r of usageRows(TRANSCRIPT)) {
    const ts = r.ts.replace(/\.\d+Z?$/, '');
    if (ts < fiveMinAgo) continue;
    if (r.id && seen.has(r.id)) continue;
    if (r.id) seen.add(r.id);
    toks += r.in + r.out; cst += rowCost(r);
  }
  burn = { tpm: Math.round(toks / 5), cph: cst * 12 };
  writeCache('burn-cache.json', { d: burn, ts: NOW, sid: SESSION_ID });
}

// ── Session tokens + per-model usage ─────────────────────────────────────────
let sIn = 0, sOut = 0, sCw = 0, sCr = 0;
const models = {};
if (TRANSCRIPT && fs.existsSync(TRANSCRIPT)) {
  const seen = new Set();
  for (const r of usageRows(TRANSCRIPT)) {
    if (r.id && seen.has(r.id)) continue;
    if (r.id) seen.add(r.id);
    sIn += r.in; sOut += r.out; sCw += r.cw; sCr += r.cr;
    const id = r.model || 'default';
    if (id === '<synthetic>') continue;
    const mp = pricing(id);
    const c = (r.in * mp[0] + r.out * mp[1] + r.cw * mp[2] + r.cr * mp[3]) / 1e6;
    if (!models[id]) models[id] = { cost: 0, tokens: 0, count: 0 };
    models[id].cost += c; models[id].tokens += r.in + r.out + r.cw + r.cr; models[id].count++;
  }
}

function shortModel(id) {
  let m;
  if ((m = id.match(/opus-(\d)-(\d)/)))   return `Opus ${m[1]}.${m[2]}`;
  if ((m = id.match(/sonnet-(\d)-(\d)/))) return `Sonnet ${m[1]}.${m[2]}`;
  if ((m = id.match(/haiku-(\d)-(\d)/)))  return `Haiku ${m[1]}.${m[2]}`;
  return id;
}

// ── Subagents ─────────────────────────────────────────────────────────────────
let subAgents = { count: 0, byType: {} };
if (SESSION_ID) {
  const slug = DIR.replace(/[:/\\]/g, '-');
  const projDir = path.join(PROJECTS_DIR, slug);
  const agentFiles = jsonlFiles(projDir, 7).filter((f) => path.basename(f).startsWith('agent-'));
  for (const f of agentFiles) {
    let lines;
    try { lines = fs.readFileSync(f, 'utf8').split('\n').filter(Boolean); } catch (_) { continue; }
    try { const o0 = JSON.parse(lines[0]); if (o0.sessionId !== SESSION_ID) continue; } catch (_) { continue; }
    let agentType = 'general-purpose';
    for (const line of lines) {
      const idx = line.indexOf('"attributionAgent":"');
      if (idx !== -1) {
        const rest = line.slice(idx + 20), end = rest.indexOf('"');
        if (end > 0) { agentType = rest.slice(0, end).split(':').pop(); break; }
      }
    }
    let agCost = 0;
    const seen = new Set();
    for (const line of lines) {
      if (line.indexOf('"assistant"') === -1) continue;
      let o; try { o = JSON.parse(line); } catch (_) { continue; }
      const u = o.message && o.message.usage; if (!u) continue;
      const id = (o.message && o.message.id) || o.requestId || o.uuid || '';
      if (id && seen.has(id)) continue; if (id) seen.add(id);
      const mp = pricing((o.message && o.message.model) || 'default');
      agCost += ((u.input_tokens || 0) * mp[0] + (u.output_tokens || 0) * mp[1]
        + (u.cache_creation_input_tokens || 0) * mp[2] + (u.cache_read_input_tokens || 0) * mp[3]) / 1e6;
    }
    subAgents.count++;
    if (!subAgents.byType[agentType]) subAgents.byType[agentType] = { count: 0, cost: 0 };
    subAgents.byType[agentType].count++; subAgents.byType[agentType].cost += agCost;
  }
}

// ── Git ───────────────────────────────────────────────────────────────────────
const gitC = (args) => execFileSync('git', args, { cwd: DIR, stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim();
let gitBranch = '', gitCommits = '0', gitIcon = '✅', gitAhead = 0, gitBehind = 0;
try {
  const branch = gitC(['branch', '--show-current']);
  if (branch) {
    gitBranch = branch;
    gitCommits = gitC(['rev-list', '--count', 'HEAD']) || '0';
    gitIcon = gitC(['status', '--porcelain']) ? '⚠️' : '✅';
    try {
      const ab = gitC(['rev-list', '--count', '--left-right', 'HEAD...@{u}']);
      const parts = ab.split('\t');
      gitAhead = parseInt(parts[0]) || 0; gitBehind = parseInt(parts[1]) || 0;
    } catch (_) {}
  }
} catch (_) {}

// ── Launch dir — prefer workspace.project_dir, fallback DFS ──────────────────
function decodeLaunchDir(transcriptPath) {
  const marker = '/.claude/projects/';
  const idx = transcriptPath.indexOf(marker);
  if (idx === -1) return '';
  let slug = transcriptPath.slice(idx + marker.length).split('/')[0];
  if (slug.startsWith('-')) slug = slug.slice(1);
  const driveM = slug.match(/^([A-Za-z])--(.+)$/);
  if (!driveM) return '';
  const base = driveM[1].toUpperCase() + ':/';
  const parts = driveM[2].split('-');
  function dfs(dir, seg, i) {
    if (i === parts.length) { const full = dir + seg; return fs.existsSync(full) ? full : null; }
    const asDir = dir + seg + '/';
    try { if (fs.existsSync(dir + seg) && fs.statSync(dir + seg).isDirectory()) {
      const r = dfs(asDir, parts[i], i + 1); if (r) return r;
    } } catch (_) {}
    return dfs(dir, seg + '-' + parts[i], i + 1);
  }
  return dfs(base, parts[0], 1) || '';
}
const LAUNCH_DIR = PROJECT_DIR || decodeLaunchDir(TRANSCRIPT) || DIR;

// ── Location (6h cache) ───────────────────────────────────────────────────────
let loc = readCache('location-cache.json');
if (!loc || NOW - (loc.cached_at || 0) > 21600) {
  try {
    const p = JSON.parse(curl('http://ip-api.com/json/'));
    if (p.status === 'success') { p.cached_at = NOW; writeCache('location-cache.json', p); loc = p; }
  } catch (_) {}
}
loc = loc || {};

// ── Prayer times (daily cache) ────────────────────────────────────────────────
const todayStr = new Date().toISOString().slice(0, 10);
let prayer = readCache('prayer-cache.json');
if ((!prayer || prayer.date !== todayStr) && loc.lat) {
  try {
    const lat = encodeURIComponent(loc.lat), lon = encodeURIComponent(loc.lon);
    const p = JSON.parse(curl(`https://api.aladhan.com/v1/timings?latitude=${lat}&longitude=${lon}&method=1`));
    if (p.data && p.data.timings) { p.cached_at = NOW; p.date = todayStr; writeCache('prayer-cache.json', p); prayer = p; }
  } catch (_) {}
}

// ── Plugins + Skills + MCP ────────────────────────────────────────────────────
let plugins = [];
try {
  const s = JSON.parse(fs.readFileSync(path.join(HOME, '.claude', 'settings.json'), 'utf8'));
  plugins = Object.entries(s.enabledPlugins || {}).filter(([, v]) => v === true).map(([k]) => k.split('@')[0]);
} catch (_) {}

function countSkills() {
  const seen = new Set(), skip = new Set(['.orphaned_at', 'CLAUDE.md']);
  try { fs.readdirSync(path.join(HOME, '.claude', 'skills'), { withFileTypes: true }).forEach(e => { if (!skip.has(e.name)) seen.add(e.name); }); } catch (_) {}
  function walk(dir, depth) {
    if (depth > 4) return;
    try {
      fs.readdirSync(dir, { withFileTypes: true }).forEach(e => {
        if (e.isDirectory() && e.name === 'skills') {
          try { fs.readdirSync(path.join(dir, 'skills'), { withFileTypes: true }).forEach(s => { if (!skip.has(s.name)) seen.add(s.name); }); } catch (_) {}
        } else if (e.isDirectory()) walk(path.join(dir, e.name), depth + 1);
      });
    } catch (_) {}
  }
  walk(path.join(HOME, '.claude', 'plugins', 'cache'), 0);
  return seen.size;
}
const skillCount = countSkills();

let mcpServers = [];
try { const cj = JSON.parse(fs.readFileSync(path.join(HOME, '.claude.json'), 'utf8')); mcpServers = Object.keys(cj.mcpServers || {}); } catch (_) {}
for (const base of [LAUNCH_DIR, DIR]) {
  try {
    const pm = JSON.parse(fs.readFileSync(path.join(base.replace(/\//g, path.sep), '.mcp.json'), 'utf8'));
    const extra = Object.keys(pm.mcpServers || pm || {}).filter(k => !mcpServers.includes(k));
    mcpServers = [...mcpServers, ...extra];
  } catch (_) {}
}

// ── Formatting ────────────────────────────────────────────────────────────────
const fmtTok = (t) => t >= 1e6 ? (t / 1e6).toFixed(1) + 'M' : t >= 1000 ? (t / 1000).toFixed(1) + 'k' : String(t);
const usd = (n) => '$' + Number(n).toFixed(2);
function fmtDur(ms) {
  if (!ms) return '';
  const s = Math.floor(ms / 1000), m = Math.floor(s / 60), h = Math.floor(m / 60);
  return h > 0 ? `${h}h ${m % 60}m` : m > 0 ? `${m}m` : `${s}s`;
}
function to12h(t) {
  let [h, m] = t.split(':').map(Number);
  const ap = h >= 12 ? 'PM' : 'AM'; h = h % 12 || 12;
  return `${h}:${String(m).padStart(2, '0')} ${ap}`;
}
function clock() {
  const d = new Date(); let h = d.getHours(); const m = d.getMinutes();
  const ap = h >= 12 ? 'PM' : 'AM'; h = h % 12 || 12;
  return `${h}:${String(m).padStart(2, '0')} ${ap}`;
}
function fullDate() {
  return new Date().toLocaleDateString('en-US', { weekday: 'short', month: 'short', day: 'numeric', year: 'numeric' });
}

// ── Context bar ───────────────────────────────────────────────────────────────
const filled = Math.floor(PCT / 10);
const ctxBar = '█'.repeat(filled) + '░'.repeat(10 - filled);
const ctxC = EXCEEDS_200K ? C.red : PCT >= 90 ? C.red : PCT >= 70 ? C.yellow : C.green;
const burnC = burn.cph > 8 ? C.red : burn.cph > 3 ? C.orange : C.green;

// ── Dir display ───────────────────────────────────────────────────────────────
let dirDisplay;
if (DIR === LAUNCH_DIR || !DIR.startsWith(LAUNCH_DIR + '/')) {
  dirDisplay = DIR === LAUNCH_DIR
    ? `${C.white}${C.bold}${LAUNCH_DIR}${R}`
    : `${C.white}${C.bold}${LAUNCH_DIR}${R} ${C.gray}→${R} ${C.yellow}${DIR}${R}`;
} else {
  dirDisplay = `${C.white}${C.bold}${LAUNCH_DIR}${R} ${C.gray}→${R} ${C.yellow}${DIR.slice(LAUNCH_DIR.length + 1)}${R}`;
}

// ── LINE 1: path | model | flags | ctx ───────────────────────────────────────
const branchStr = gitBranch ? ` ${C.teal}(${gitBranch})${R}` : '';
let L1 = '';
if (SESSION_NAME) L1 += `${C.pink}${C.bold}${SESSION_NAME}${R}  `;
L1 += `${dirDisplay}${branchStr} ${gitIcon}`;
L1 += ` ${SEP} 🧠 ${C.magenta}${MODEL}${R}`;
if (FAST_MODE)  L1 += ` ${SEP} ${C.yellow}⚡fast${R}`;
if (EXCEEDS_200K) L1 += ` ${SEP} ${C.red}${C.bold}⚠ >200K${R}`;
const ctxSizeStr = CTX_SIZE >= 1e6 ? (CTX_SIZE / 1e6).toFixed(0) + 'M' : (CTX_SIZE / 1000).toFixed(0) + 'k';
const ctxUsedStr = CTX_TOTAL_IN > 0 ? fmtTok(CTX_TOTAL_IN + CTX_TOTAL_OUT) + '/' + ctxSizeStr : fmtTok(CTX_TOKENS);
L1 += ` ${SEP} ${ctxC}${ctxBar} ${PCT}%${R} ${C.gray}(${ctxUsedStr})${R}`;
console.log(L1);

// ── LINE 1b: session tokens ───────────────────────────────────────────────────
const sTotal = sIn + sOut + sCw + sCr;
let L1b = `${C.gray}In:${R}${C.blue}${fmtTok(sIn)}${R}`;
L1b += ` ${SEP} ${C.gray}Out:${R}${C.magenta}${fmtTok(sOut)}${R}`;
L1b += ` ${SEP} ${C.gray}Write:${R}${C.orange}${fmtTok(sCw)}${R}`;
L1b += ` ${SEP} ${C.gray}Read:${R}${C.teal}${fmtTok(sCr)}${R}`;
L1b += ` ${SEP} ${C.gray}Total:${R}${C.white}${fmtTok(sTotal)}${R}`;
if (EFFORT)   L1b += ` ${SEP} ${C.gray}effort:${R}${C.orange}${EFFORT}${R}`;
if (THINKING) L1b += ` ${SEP} ${C.cyan}💭 thinking${R}`;
console.log(L1b);

// ── LINE 2: rate limits | git changes | ahead/behind | commits | duration | API% | clock ──
let rateStr = `${C.gray}No rate limit data${R}`;
if (RATE_5H_RESET) {
  const resetT = to12h(new Date(RATE_5H_RESET * 1000).toTimeString().slice(0, 5));
  const mins = Math.floor((RATE_5H_RESET - NOW) / 60), hrs = Math.floor(mins / 60), mr = mins % 60;
  let s = `⊙ 5H ${resetT} (${hrs}h ${mr}m) ${Math.round(RATE_5H || 0)}%`;
  if (RATE_7D_RESET) {
    const d = new Date(RATE_7D_RESET * 1000);
    const t7 = `${d.toLocaleDateString('en-US', { weekday: 'short' })} ${to12h(d.toTimeString().slice(0, 5))}`;
    const m7 = Math.floor((RATE_7D_RESET - NOW) / 60), d7 = Math.floor(m7 / 1440), h7 = Math.floor((m7 % 1440) / 60);
    s += ` • 7DAY ${t7} (${d7}d ${h7}h) ${Math.round(RATE_7D || 0)}%`;
  } else s += ` • 7DAY ${Math.round(RATE_7D || 0)}%`;
  rateStr = `${C.yellow}${s}${R}`;
}
let L2 = rateStr;
L2 += ` ${SEP} ${C.green}+${LINES_ADD}${R}/${C.red}-${LINES_DEL}${R}`;
if (gitAhead || gitBehind) L2 += ` ${SEP} ${C.teal}↑${gitAhead}${R}${C.gray}/${R}${C.orange}↓${gitBehind}${R}`;
L2 += ` ${SEP} ${C.gray}Commits:${R}${C.gold}${gitCommits}${R}`;
if (DURATION_MS) {
  const dur = fmtDur(DURATION_MS);
  const apiPct = DURATION_MS > 0 ? Math.round(API_DUR_MS * 100 / DURATION_MS) : 0;
  L2 += ` ${SEP} ${C.gray}⏱ ${R}${C.cyan}${dur}${R} ${C.gray}(${apiPct}% API)${R}`;
}
L2 += ` ${SEP} ${C.bold}🕐 ${clock()}${R}`;
console.log(L2);

// ── LINE 3: costs ─────────────────────────────────────────────────────────────
const cacheHit = (sIn + sCw + sCr) > 0 ? Math.round(sCr * 100 / (sIn + sCw + sCr)) : 0;
let L3 = `${C.gray}REPO${R} ${C.yellow}${usd(costData.repo)}${R}`;
L3 += ` ${SEP} ${C.gray}30D${R} ${C.yellow}${usd(costData.month)}${R}`;
L3 += ` ${SEP} ${C.gray}7D${R} ${C.yellow}${usd(costData.week)}${R}`;
L3 += ` ${SEP} ${C.gray}DAY${R} ${C.yellow}${usd(costData.day)}${R}`;
L3 += ` ${SEP} 🔥 ${C.orange}LIVE ${usd(LIVE_COST)}${R}`;
L3 += ` ${SEP} ${burnC}${usd(burn.cph)}/hr${R}`;
L3 += ` ${SEP} ${C.gray}Cache hit:${R} ${C.green}${cacheHit}%${R}`;
console.log(L3);

// ── Subagents ─────────────────────────────────────────────────────────────────
if (subAgents.count > 0) {
  const segs = Object.entries(subAgents.byType).sort((a, b) => b[1].cost - a[1].cost)
    .map(([name, v]) => `${C.magenta}${name}${R} ${C.gray}×${v.count}${R} ${C.yellow}${usd(v.cost)}${R}`);
  console.log(`🧩 ${C.gray}Subagents:${R}${C.cyan}${subAgents.count}${R} ${C.gray}spawned${R} ${SEP} ${segs.join(SEP)}`);
}

// ── LINE 4: hijri | date | model usage ───────────────────────────────────────
const hasPrayer = prayer && prayer.data && prayer.data.timings;
let modelStr = '';
const modelEntries = Object.entries(models).sort((a, b) => b[1].cost - a[1].cost);
const totalModelCost = modelEntries.reduce((s, [, v]) => s + v.cost, 0) || 1;
if (modelEntries.length) {
  const mcolors = [C.orange, C.magenta, C.teal, C.blue];
  modelStr = '🤖 ' + modelEntries.map(([id, v], i) => {
    const pct = Math.round(v.cost * 100 / totalModelCost), col = mcolors[i % mcolors.length];
    return `${col}${shortModel(id)}${R} ${C.yellow}${usd(v.cost)}${R} ${C.gray}(${pct}%)${R} ${C.teal}${fmtTok(v.tokens)}${R} ${C.gray}×${v.count}${R}`;
  }).join(SEP);
}
let L4 = hasPrayer
  ? `${C.gray}${prayer.data.date.hijri.day} ${prayer.data.date.hijri.month.en} ${prayer.data.date.hijri.year}${R} ${SEP} ${C.white}${fullDate()}${R}`
  : `${C.white}${fullDate()}${R}`;
if (modelStr) L4 += ` ${SEP} ${modelStr}`;
console.log(L4);

// ── LINE 5: PR | worktree | repo | agent | output_style | added_dirs | vim | version ──
const L5parts = [];
if (PR_NUM)    {
  const stateColor = PR_STATE === 'approved' ? C.green : PR_STATE === 'changes_requested' ? C.red : PR_STATE === 'draft' ? C.gray : C.yellow;
  L5parts.push(`🔀 ${C.blue}PR #${PR_NUM}${R}${PR_STATE ? ` ${stateColor}· ${PR_STATE}${R}` : ''}`);
}
if (WT_NAME)   L5parts.push(`🌿 ${C.teal}${WT_NAME}${WT_BRANCH ? ` (${WT_BRANCH})` : ''}${R}`);
if (REPO_OWNER && REPO_NAME) L5parts.push(`📦 ${C.gray}${REPO_OWNER}/${REPO_NAME}${R}`);
if (AGENT_NAME) L5parts.push(`🤖 ${C.magenta}agent:${AGENT_NAME}${R}`);
if (OUTPUT_STYLE && OUTPUT_STYLE !== 'default') L5parts.push(`🎨 ${C.gray}${OUTPUT_STYLE}${R}`);
if (ADDED_DIRS && ADDED_DIRS.length) L5parts.push(`📁 ${C.gray}+${ADDED_DIRS.length} dirs${R}`);
if (VIM_MODE)  L5parts.push(`${C.gold}[${VIM_MODE}]${R}`);
if (L5parts.length) console.log(L5parts.join(SEP));

// ── LINE 6: prayers ───────────────────────────────────────────────────────────
if (hasPrayer) {
  const t = prayer.data.timings;
  const names = ['Fajr', 'Dhuhr', 'Asr', 'Maghrib', 'Isha'];
  const times = [t.Fajr, t.Dhuhr, t.Asr, t.Maghrib, t.Isha];
  const epoch = (hm) => { const [h, m] = hm.split(':').map(Number); const d = new Date(); d.setHours(h, m, 0, 0); return Math.floor(d.getTime() / 1000); };
  let nextIdx = -1;
  for (let i = 0; i < 5; i++) if (epoch(times[i]) > NOW) { nextIdx = i; break; }
  const parts = names.map((nm, i) => {
    const pe = epoch(times[i]), t12 = to12h(times[i]), diff = pe - NOW;
    if (pe <= NOW) return `${C.gray}${nm} ${t12} ✓${R}`;
    if (i === nextIdx) {
      const dm = Math.floor(diff / 60), dh = Math.floor(diff / 3600), dr = Math.floor((diff % 3600) / 60);
      return `${C.yellow}${nm} ${t12} ${dh > 0 ? `(${dh}h ${dr}m)` : `(${dm}m)`}${R}`;
    }
    return `${C.cyan}${nm} ${t12}${R}`;
  });
  console.log('🕌 ' + parts.join(`${C.gray} | ${R}`));
}

// ── LINE 7: location | plugins | skills | MCP ────────────────────────────────
const locDisp = [loc.city, loc.country].filter(Boolean).join(', ') || 'Unknown';
let L7 = `📍 ${C.gray}${locDisp}${R} ${SEP} ${C.gray}Plugins:${R}${C.cyan}${plugins.length}${R}`;
L7 += ` ${SEP} ${C.gray}Skills:${R}${C.green}${skillCount}${R}`;
if (mcpServers.length) L7 += ` ${SEP} ${C.gray}MCP:${R}${C.magenta}${mcpServers.length}${R}`;
if (VERSION) L7 += ` ${SEP} ${C.gray}v${VERSION}${R}`;
console.log(L7);

// ── Plugin names (5/line) ─────────────────────────────────────────────────────
const PER = 5;
for (let i = 0; i < plugins.length; i += PER) {
  console.log('   ' + plugins.slice(i, i + PER).map(p => `${C.teal}${p}${R}`).join(`${C.gray}, ${R}`));
}

// ── MCP server names ──────────────────────────────────────────────────────────
if (mcpServers.length) {
  for (let i = 0; i < mcpServers.length; i += PER) {
    console.log('🔌 ' + (i === 0 ? '' : '   ') + mcpServers.slice(i, i + PER).map(s => `${C.magenta}${s}${R}`).join(`${C.gray}, ${R}`));
  }
}
