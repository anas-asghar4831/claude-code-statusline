#!/usr/bin/env node
'use strict';
// Claude Code Enhanced Statusline (Node.js — single process, fast)
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const curl = (url) => execFileSync('curl', ['-sL', '--max-time', '5', url], { stdio: ['ignore', 'pipe', 'ignore'] }).toString();
const gitC = (args) => execFileSync('git', args, { cwd: DIR, stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim();

// ── Read stdin ────────────────────────────────────────────────────────────────
let raw = '';
try { raw = fs.readFileSync(0, 'utf8'); } catch (_) {}
let J = {};
try { J = JSON.parse(raw); } catch (_) {}
// DEBUG: dump full JSON once so we can inspect MCP fields — remove after

const HOME = os.homedir();
const SL_DIR = path.join(HOME, '.claude', 'statusline');
const PROJECTS_DIR = path.join(HOME, '.claude', 'projects');
const NOW = Math.floor(Date.now() / 1000);

// ── Catppuccin Mocha true-color ────────────────────────────────────────────────
const R = '\x1b[0m';
const C = {
  red: '\x1b[38;2;243;139;168m', green: '\x1b[38;2;166;227;161m',
  yellow: '\x1b[38;2;249;226;175m', blue: '\x1b[38;2;137;180;250m',
  magenta: '\x1b[38;2;203;166;247m', cyan: '\x1b[38;2;137;220;235m',
  orange: '\x1b[38;2;250;179;135m', teal: '\x1b[38;2;148;226;213m',
  gray: '\x1b[38;2;166;173;200m', white: '\x1b[38;2;205;214;244m',
  gold: '\x1b[38;2;249;226;175m', bold: '\x1b[1m',
};
const SEP = `${C.gray} | ${R}`;

// ── Field extraction ────────────────────────────────────────────────────────────
const g = (o, p, d) => p.split('.').reduce((a, k) => (a && a[k] !== undefined ? a[k] : undefined), o) ?? d;
const MODEL = g(J, 'model.display_name', '?');
const MODEL_ID = g(J, 'model.id', '');
const SESSION_ID = g(J, 'session_id', '');
const DIR = String(g(J, 'workspace.current_dir', '.')).replace(/\\/g, '/');
const TRANSCRIPT = String(g(J, 'transcript_path', '')).replace(/\\/g, '/');

// ── Decode launch dir from transcript project slug (FS-verified) ─────────────────
function decodeLaunchDir(transcriptPath) {
  const marker = '/.claude/projects/';
  const idx = transcriptPath.indexOf(marker);
  if (idx === -1) return '';
  let slug = transcriptPath.slice(idx + marker.length).split('/')[0];
  if (slug.startsWith('-')) slug = slug.slice(1); // strip leading artifact dash
  const driveM = slug.match(/^([A-Za-z])--(.+)$/);
  if (!driveM) return '';
  const base = driveM[1].toUpperCase() + ':/';
  const parts = driveM[2].split('-');
  // DFS: at each '-', try it as path separator OR literal hyphen
  function dfs(dir, seg, i) {
    if (i === parts.length) {
      const full = dir + seg;
      return fs.existsSync(full) ? full : null;
    }
    // Option 1: '-' was separator — commit seg as directory, start fresh
    const asDir = dir + seg + '/';
    try { if (fs.existsSync(dir + seg) && fs.statSync(dir + seg).isDirectory()) {
      const r = dfs(asDir, parts[i], i + 1);
      if (r) return r;
    } } catch (_) {}
    // Option 2: '-' was literal hyphen in folder name
    return dfs(dir, seg + '-' + parts[i], i + 1);
  }
  return dfs(base, parts[0], 1) || '';
}
const LAUNCH_DIR = decodeLaunchDir(TRANSCRIPT) || DIR;
const PCT = Math.floor(Number(g(J, 'context_window.used_percentage', 0)) || 0);
const LIVE_COST = Number(g(J, 'cost.total_cost_usd', 0)) || 0;
const LINES_ADD = g(J, 'cost.total_lines_added', 0);
const LINES_DEL = g(J, 'cost.total_lines_removed', 0);
const RATE_5H = g(J, 'rate_limits.five_hour.used_percentage', null);
const RATE_7D = g(J, 'rate_limits.seven_day.used_percentage', null);
const RATE_5H_RESET = g(J, 'rate_limits.five_hour.resets_at', null);
const RATE_7D_RESET = g(J, 'rate_limits.seven_day.resets_at', null);
const CTX_TOKENS = (Number(g(J, 'context_window.current_usage.input_tokens', 0)) || 0)
  + (Number(g(J, 'context_window.current_usage.cache_creation_input_tokens', 0)) || 0)
  + (Number(g(J, 'context_window.current_usage.cache_read_input_tokens', 0)) || 0);

// ── Pricing per million tokens [in, out, cacheWrite, cacheRead] ─────────────────
function pricing(id) {
  if (/^claude-opus-4-[5-8]/.test(id)) return [5, 25, 6.25, 0.5];
  if (/^claude-opus-4|^claude-3-opus/.test(id)) return [15, 75, 18.75, 1.5];
  if (/^claude-sonnet-4|^claude-3-7-sonnet|^claude-3-5-sonnet/.test(id)) return [3, 15, 3.75, 0.3];
  if (/^claude-haiku-4-5/.test(id)) return [1, 5, 1.25, 0.1];
  if (/^claude-haiku-3-5|^claude-3-5-haiku/.test(id)) return [0.8, 4, 1, 0.08];
  return [3, 15, 3.75, 0.3];
}
const P = pricing(MODEL_ID);

// ── Cache helpers ────────────────────────────────────────────────────────────────
function readCache(name) {
  try { return JSON.parse(fs.readFileSync(path.join(SL_DIR, name), 'utf8')); } catch (_) { return null; }
}
function writeCache(name, obj) {
  try { fs.writeFileSync(path.join(SL_DIR, name), JSON.stringify(obj)); } catch (_) {}
}

// ── List JSONL files recursively (no `find` spawn) ───────────────────────────────
function jsonlFiles(dir, maxAgeDays) {
  const out = [];
  const cutoff = maxAgeDays ? Date.now() - maxAgeDays * 86400000 : 0;
  let entries;
  try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch (_) { return out; }
  for (const e of entries) {
    const full = path.join(dir, e.name);
    if (e.isDirectory()) {
      out.push(...jsonlFiles(full, maxAgeDays));
    } else if (e.name.endsWith('.jsonl')) {
      if (cutoff) {
        try { if (fs.statSync(full).mtimeMs < cutoff) continue; } catch (_) { continue; }
      }
      out.push(full);
    }
  }
  return out;
}

// ── Parse a JSONL file, yield assistant usage rows ───────────────────────────────
function* usageRows(file) {
  let data;
  try { data = fs.readFileSync(file, 'utf8'); } catch (_) { return; }
  for (const line of data.split('\n')) {
    if (!line || line.indexOf('"assistant"') === -1) continue;
    let o;
    try { o = JSON.parse(line); } catch (_) { continue; }
    if (o.type !== 'assistant') continue;
    const u = o.message && o.message.usage;
    if (!u) continue;
    yield {
      ts: o.timestamp || '',
      model: (o.message && o.message.model) || 'default',
      in: u.input_tokens || 0,
      out: u.output_tokens || 0,
      cw: u.cache_creation_input_tokens || 0,
      cr: u.cache_read_input_tokens || 0,
      id: (o.message && o.message.id) || o.requestId || o.uuid || '',
    };
  }
}

const cost = (r) => (r.in * P[0] + r.out * P[1] + r.cw * P[2] + r.cr * P[3]) / 1e6;

// ── Cost calc (REPO + DAY/7D/30D), cached 5min ──────────────────────────────────
function calcCosts() {
  const slug = DIR.replace(/[:/\\]/g, '-');
  const projDir = path.join(PROJECTS_DIR, slug);

  // REPO: current project, all time
  let repo = 0;
  const seenR = new Set();
  for (const f of jsonlFiles(projDir, 0)) {
    for (const r of usageRows(f)) {
      if (r.id && seenR.has(r.id)) continue;
      if (r.id) seenR.add(r.id);
      repo += cost(r);
    }
  }

  // periods: all projects, files modified last 31 days
  const today = new Date(); today.setHours(0, 0, 0, 0);
  const todayISO = new Date(today.getTime() - today.getTimezoneOffset() * 60000).toISOString().slice(0, 19);
  const weekISO = new Date(Date.now() - 7 * 86400000).toISOString().slice(0, 19);
  const monthISO = new Date(Date.now() - 30 * 86400000).toISOString().slice(0, 19);
  let day = 0, week = 0, month = 0;
  const seenP = new Set();
  for (const f of jsonlFiles(PROJECTS_DIR, 31)) {
    for (const r of usageRows(f)) {
      if (r.id && seenP.has(r.id)) continue;
      if (r.id) seenP.add(r.id);
      const ts = r.ts.replace(/\.\d+Z?$/, '');
      const c = cost(r);
      if (ts >= monthISO) month += c;
      if (ts >= weekISO) week += c;
      if (ts >= todayISO) day += c;
    }
  }
  return { repo, month, week, day };
}

let costData;
const cc = readCache('cost-cache.json');
if (cc && cc.d && NOW - cc.ts < 300) costData = cc.d;
else { costData = calcCosts(); writeCache('cost-cache.json', { d: costData, ts: NOW }); }

// ── Burn rate (current session transcript, last 5min), cached 30s per session ────
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
    toks += r.in + r.out;
    cst += cost(r);
  }
  burn = { tpm: Math.round(toks / 5), cph: cst * 12 };
  writeCache('burn-cache.json', { d: burn, ts: NOW, sid: SESSION_ID });
}

// ── Session cumulative tokens + per-model usage (from transcript) ────────────────
let sIn = 0, sOut = 0, sCw = 0, sCr = 0;
const models = {}; // id → {cost, tokens, count}
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
    models[id].cost += c;
    models[id].tokens += r.in + r.out + r.cw + r.cr;
    models[id].count += 1;
  }
}

// Short display name from model id
function shortModel(id) {
  let m;
  if ((m = id.match(/opus-(\d)-(\d)/))) return `Opus ${m[1]}.${m[2]}`;
  if ((m = id.match(/sonnet-(\d)-(\d)/))) return `Sonnet ${m[1]}.${m[2]}`;
  if ((m = id.match(/haiku-(\d)-(\d)/))) return `Haiku ${m[1]}.${m[2]}`;
  return id;
}

// ── Subagents (agent-*.jsonl with matching sessionId, grouped by agent type) ─────
let subAgents = { count: 0, byType: {} }; // byType: name → {count, cost}
if (SESSION_ID) {
  const slug = DIR.replace(/[:/\\]/g, '-');
  const projDir = path.join(PROJECTS_DIR, slug);
  const agentFiles = jsonlFiles(projDir, 7).filter((f) => path.basename(f).startsWith('agent-'));
  for (const f of agentFiles) {
    let lines;
    try { lines = fs.readFileSync(f, 'utf8').split('\n').filter(Boolean); } catch (_) { continue; }
    // confirm session (line 0) then find agent type from any line carrying it
    try {
      const o0 = JSON.parse(lines[0]);
      if (o0.sessionId !== SESSION_ID) continue;
    } catch (_) { continue; }
    let agentType = 'general-purpose';
    for (const line of lines) {
      const idx = line.indexOf('"attributionAgent":"');
      if (idx !== -1) {
        const rest = line.slice(idx + 20);
        const end = rest.indexOf('"');
        if (end > 0) { agentType = rest.slice(0, end).split(':').pop(); break; }
      }
    }
    // sum this agent's cost
    let cost = 0;
    const seen = new Set();
    for (const line of lines) {
      if (line.indexOf('"assistant"') === -1) continue;
      let o; try { o = JSON.parse(line); } catch (_) { continue; }
      const u = o.message && o.message.usage; if (!u) continue;
      const id = (o.message && o.message.id) || o.requestId || o.uuid || '';
      if (id && seen.has(id)) continue; if (id) seen.add(id);
      const mp = pricing((o.message && o.message.model) || 'default');
      cost += ((u.input_tokens || 0) * mp[0] + (u.output_tokens || 0) * mp[1]
        + (u.cache_creation_input_tokens || 0) * mp[2] + (u.cache_read_input_tokens || 0) * mp[3]) / 1e6;
    }
    subAgents.count += 1;
    if (!subAgents.byType[agentType]) subAgents.byType[agentType] = { count: 0, cost: 0 };
    subAgents.byType[agentType].count += 1;
    subAgents.byType[agentType].cost += cost;
  }
}

// ── Git (read .git directly, minimal spawn) ──────────────────────────────────────
let gitBranch = '', gitCommits = '0', gitIcon = '✅';
try {
  const branch = gitC(['branch', '--show-current']);
  if (branch) {
    gitBranch = branch;
    gitCommits = gitC(['rev-list', '--count', 'HEAD']) || '0';
    gitIcon = gitC(['status', '--porcelain']) ? '⚠️' : '✅';
  }
} catch (_) {}

// ── Location (cached 6h) ─────────────────────────────────────────────────────────
let loc = readCache('location-cache.json');
if (!loc || NOW - (loc.cached_at || 0) > 21600) {
  try {
    const p = JSON.parse(curl('http://ip-api.com/json/'));
    if (p.status === 'success') { p.cached_at = NOW; writeCache('location-cache.json', p); loc = p; }
  } catch (_) {}
}
loc = loc || {};

// ── Prayer times (cached per day) ────────────────────────────────────────────────
const todayStr = new Date().toISOString().slice(0, 10);
let prayer = readCache('prayer-cache.json');
if ((!prayer || prayer.date !== todayStr) && loc.lat) {
  try {
    const lat = encodeURIComponent(loc.lat), lon = encodeURIComponent(loc.lon);
    const p = JSON.parse(curl(`https://api.aladhan.com/v1/timings?latitude=${lat}&longitude=${lon}&method=1`));
    if (p.data && p.data.timings) { p.cached_at = NOW; p.date = todayStr; writeCache('prayer-cache.json', p); prayer = p; }
  } catch (_) {}
}

// ── Plugins ──────────────────────────────────────────────────────────────────────
// ── Skills (enabledPlugins) + MCP servers (project or global settings) ───────────
let skills = [];
try {
  const s = JSON.parse(fs.readFileSync(path.join(HOME, '.claude', 'settings.json'), 'utf8'));
  skills = Object.entries(s.enabledPlugins || {}).filter(([, v]) => v === true).map(([k]) => k.split('@')[0]);
} catch (_) {}

let mcpServers = [];
// ~/.claude.json is the actual MCP config (settings.json is wrong per GH #4976)
try {
  const cj = JSON.parse(fs.readFileSync(path.join(HOME, '.claude.json'), 'utf8'));
  mcpServers = Object.keys(cj.mcpServers || {});
} catch (_) {}
// Also merge project-level .mcp.json if present
for (const base of [LAUNCH_DIR, DIR]) {
  try {
    const pm = JSON.parse(fs.readFileSync(path.join(base.replace(/\//g, path.sep), '.mcp.json'), 'utf8'));
    const extra = Object.keys(pm.mcpServers || pm || {}).filter(k => !mcpServers.includes(k));
    mcpServers = [...mcpServers, ...extra];
  } catch (_) {}
}

// ── Formatting helpers ────────────────────────────────────────────────────────────
const fmtTok = (t) => t >= 1e6 ? (t / 1e6).toFixed(1) + 'M' : t >= 1000 ? (t / 1000).toFixed(1) + 'k' : String(t);
const usd = (n) => '$' + Number(n).toFixed(2);
function to12h(t) {
  let [h, m] = t.split(':').map(Number);
  const ap = h >= 12 ? 'PM' : 'AM';
  h = h % 12 || 12;
  return `${h}:${String(m).padStart(2, '0')} ${ap}`;
}
function clock() {
  const d = new Date();
  let h = d.getHours(); const m = d.getMinutes();
  const ap = h >= 12 ? 'PM' : 'AM'; h = h % 12 || 12;
  return `${h}:${String(m).padStart(2, '0')} ${ap}`;
}
function fullDate() {
  return new Date().toLocaleDateString('en-US', { weekday: 'short', month: 'short', day: 'numeric', year: 'numeric' });
}

// ── Context bar + colors ───────────────────────────────────────────────────────────
const filled = Math.floor(PCT / 10);
const ctxBar = '█'.repeat(filled) + '░'.repeat(10 - filled);
const ctxC = PCT >= 90 ? C.red : PCT >= 70 ? C.yellow : C.green;
const burnC = burn.cph > 8 ? C.red : burn.cph > 3 ? C.orange : C.green;

// ── Line 1 ──────────────────────────────────────────────────────────────────────
const folder = DIR.split('/').pop();
const branchStr = gitBranch ? ` ${C.teal}(${gitBranch})${R}` : '';
const sTotal = sIn + sOut + sCw + sCr;
// workspace relative to launch dir, or full if outside
let dirDisplay;
if (DIR === LAUNCH_DIR || DIR.startsWith(LAUNCH_DIR + '/') === false) {
  // same OR unrelated — show launch only (or launch + full workspace if unrelated)
  dirDisplay = DIR === LAUNCH_DIR
    ? `${C.white}${C.bold}${LAUNCH_DIR}${R}`
    : `${C.white}${C.bold}${LAUNCH_DIR}${R} ${C.gray}→${R} ${C.yellow}${DIR}${R}`;
} else {
  const rel = DIR.slice(LAUNCH_DIR.length + 1); // strip common prefix + slash
  dirDisplay = `${C.white}${C.bold}${LAUNCH_DIR}${R} ${C.gray}→${R} ${C.yellow}${rel}${R}`;
}
let L1 = `${dirDisplay}${branchStr} ${gitIcon}`;
L1 += ` ${SEP} 🧠 ${C.magenta}${MODEL}${R}`;
L1 += ` ${SEP} ${ctxC}${ctxBar} ${PCT}%${R} ${C.gray}(${fmtTok(CTX_TOKENS)})${R}`;
console.log(L1);

// ── Line 1b: session tokens ──────────────────────────────────────────────────────
let L1b = `${C.gray}In:${R}${C.blue}${fmtTok(sIn)}${R}`;
L1b += ` ${SEP} ${C.gray}Out:${R}${C.magenta}${fmtTok(sOut)}${R}`;
L1b += ` ${SEP} ${C.gray}Write:${R}${C.orange}${fmtTok(sCw)}${R}`;
L1b += ` ${SEP} ${C.gray}Read:${R}${C.teal}${fmtTok(sCr)}${R}`;
L1b += ` ${SEP} ${C.gray}Total:${R}${C.white}${fmtTok(sTotal)}${R}`;
console.log(L1b);

// ── Line 2: rate limits | changes | commits | clock ─────────────────────────────
let rateStr = `${C.gray}No rate limit data${R}`;
if (RATE_5H_RESET) {
  const resetT = to12h(new Date(RATE_5H_RESET * 1000).toTimeString().slice(0, 5));
  const mins = Math.floor((RATE_5H_RESET - NOW) / 60);
  const hrs = Math.floor(mins / 60), mr = mins % 60;
  let s = `⊙ 5H ${resetT} (${hrs}h ${mr}m) ${Math.round(RATE_5H || 0)}%`;
  if (RATE_7D_RESET) {
    const d = new Date(RATE_7D_RESET * 1000);
    const t7 = `${d.toLocaleDateString('en-US', { weekday: 'short' })} ${to12h(d.toTimeString().slice(0, 5))}`;
    const m7 = Math.floor((RATE_7D_RESET - NOW) / 60);
    const d7 = Math.floor(m7 / 1440), h7 = Math.floor((m7 % 1440) / 60);
    s += ` • 7DAY ${t7} (${d7}d ${h7}h) ${Math.round(RATE_7D || 0)}%`;
  } else s += ` • 7DAY ${Math.round(RATE_7D || 0)}%`;
  rateStr = `${C.yellow}${s}${R}`;
}
let L2 = rateStr;
L2 += ` ${SEP} ${C.green}+${LINES_ADD}${R}/${C.red}-${LINES_DEL}${R}`;
L2 += ` ${SEP} ${C.gray}Commits:${R}${C.gold}${gitCommits}${R}`;
L2 += ` ${SEP} ${C.bold}🕐 ${clock()}${R}`;
console.log(L2);

// ── Line 3: money ─────────────────────────────────────────────────────────────────
const cacheHit = (sIn + sCw + sCr) > 0 ? Math.round(sCr * 100 / (sIn + sCw + sCr)) : 0;
let L3 = `${C.gray}REPO${R} ${C.yellow}${usd(costData.repo)}${R}`;
L3 += ` ${SEP} ${C.gray}30D${R} ${C.yellow}${usd(costData.month)}${R}`;
L3 += ` ${SEP} ${C.gray}7D${R} ${C.yellow}${usd(costData.week)}${R}`;
L3 += ` ${SEP} ${C.gray}DAY${R} ${C.yellow}${usd(costData.day)}${R}`;
L3 += ` ${SEP} 🔥 ${C.orange}LIVE ${usd(LIVE_COST)}${R}`;
L3 += ` ${SEP} ${burnC}${usd(burn.cph)}/hr${R}`;
L3 += ` ${SEP} ${C.gray}Cache hit:${R} ${C.green}${cacheHit}%${R}`;
console.log(L3);

// ── Subagent line (own line, grouped by agent type — only if any spawned) ────────
if (subAgents.count > 0) {
  const segs = Object.entries(subAgents.byType).sort((a, b) => b[1].cost - a[1].cost)
    .map(([name, v]) => `${C.magenta}${name}${R} ${C.gray}×${v.count}${R} ${C.yellow}${usd(v.cost)}${R}`);
  let LS = `🧩 ${C.gray}Subagents:${R}${C.cyan}${subAgents.count}${R} ${C.gray}spawned${R}`;
  LS += ` ${SEP} ` + segs.join(SEP);
  console.log(LS);
}

// ── Model usage string (per-session: cost (%) tokens ×count) ─────────────────────
let modelStr = '';
const modelEntries = Object.entries(models).sort((a, b) => b[1].cost - a[1].cost);
const totalModelCost = modelEntries.reduce((s, [, v]) => s + v.cost, 0) || 1;
if (modelEntries.length) {
  const mcolors = [C.orange, C.magenta, C.teal, C.blue];
  const segs = modelEntries.map(([id, v], i) => {
    const pctSpend = Math.round(v.cost * 100 / totalModelCost);
    const col = mcolors[i % mcolors.length];
    return `${col}${shortModel(id)}${R} ${C.yellow}${usd(v.cost)}${R} ${C.gray}(${pctSpend}%)${R} ${C.teal}${fmtTok(v.tokens)}${R} ${C.gray}×${v.count}${R}`;
  });
  modelStr = '🤖 ' + segs.join(SEP);
}

// ── Line 4: hijri | date | model usage ───────────────────────────────────────────
const hasPrayer = prayer && prayer.data && prayer.data.timings;
let L4 = '';
if (hasPrayer) {
  const h = prayer.data.date.hijri;
  L4 = `${C.gray}${h.day} ${h.month.en} ${h.year}${R} ${SEP} ${C.white}${fullDate()}${R}`;
} else {
  L4 = `${C.white}${fullDate()}${R}`;
}
if (modelStr) L4 += ` ${SEP} ${modelStr}`;
console.log(L4);

// ── Line 5: prayers ──────────────────────────────────────────────────────────────
if (hasPrayer) {
  const t = prayer.data.timings;
  const names = ['Fajr', 'Dhuhr', 'Asr', 'Maghrib', 'Isha'];
  const times = [t.Fajr, t.Dhuhr, t.Asr, t.Maghrib, t.Isha];
  const epoch = (hm) => { const [h, m] = hm.split(':').map(Number); const d = new Date(); d.setHours(h, m, 0, 0); return Math.floor(d.getTime() / 1000); };
  let nextIdx = -1;
  for (let i = 0; i < 5; i++) if (epoch(times[i]) > NOW) { nextIdx = i; break; }
  const parts = names.map((nm, i) => {
    const pe = epoch(times[i]); const t12 = to12h(times[i]); const diff = pe - NOW;
    if (pe <= NOW) return `${C.gray}${nm} ${t12} ✓${R}`;
    if (i === nextIdx) {
      const dm = Math.floor(diff / 60), dh = Math.floor(diff / 3600), dr = Math.floor((diff % 3600) / 60);
      const cd = dh > 0 ? `(${dh}h ${dr}m)` : `(${dm}m)`;
      return `${C.yellow}${nm} ${t12} ${cd}${R}`;
    }
    return `${C.cyan}${nm} ${t12}${R}`;
  });
  console.log('🕌 ' + parts.join(`${C.gray} | ${R}`));
}

// ── Line 6: location + skills count + mcp count ──────────────────────────────────
const locDisp = [loc.city, loc.country].filter(Boolean).join(', ') || 'Unknown';
let L6 = `📍 ${C.gray}${locDisp}${R} ${SEP} ${C.gray}Skills:${R}${C.cyan}${skills.length}${R}`;
if (mcpServers.length) L6 += ` ${SEP} ${C.gray}MCP:${R}${C.magenta}${mcpServers.length}${R}`;
console.log(L6);

// ── Line 7+: skill names wrapped 5/line ──────────────────────────────────────────
const PER = 5;
for (let i = 0; i < skills.length; i += PER) {
  const chunk = skills.slice(i, i + PER).map((p) => `${C.teal}${p}${R}`).join(`${C.gray}, ${R}`);
  console.log('   ' + chunk);
}

// ── MCP server names (only if found) ────────────────────────────────────────────
if (mcpServers.length) {
  for (let i = 0; i < mcpServers.length; i += PER) {
    const chunk = mcpServers.slice(i, i + PER).map((s) => `${C.magenta}${s}${R}`).join(`${C.gray}, ${R}`);
    console.log('🔌 ' + (i === 0 ? '' : '   ') + chunk);
  }
}
