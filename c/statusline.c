// Claude Code Windows statusline — C port of statusline.js (max-perf build).
// Mirrors statusline.js output byte-for-byte. Deps: yyjson (JSON), uthash (dedup sets).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include "yyjson.h"
#include "uthash.h"

#ifdef _WIN32
#include <windows.h>
#define NULDEV ""
#else
#include <unistd.h>
#define NULDEV "2>/dev/null"
#endif

// ── Catppuccin Mocha true-color ───────────────────────────────────────────────
#define R       "\x1b[0m"
#define C_red   "\x1b[38;2;243;139;168m"
#define C_green "\x1b[38;2;166;227;161m"
#define C_yellow "\x1b[38;2;249;226;175m"
#define C_blue  "\x1b[38;2;137;180;250m"
#define C_magenta "\x1b[38;2;203;166;247m"
#define C_cyan  "\x1b[38;2;137;220;235m"
#define C_orange "\x1b[38;2;250;179;135m"
#define C_teal  "\x1b[38;2;148;226;213m"
#define C_gray  "\x1b[38;2;166;173;200m"
#define C_white "\x1b[38;2;205;214;244m"
#define C_gold  "\x1b[38;2;249;226;175m"
#define C_pink  "\x1b[38;2;245;194;231m"
#define C_bold  "\x1b[1m"
#define SEP     C_gray " | " R

static long NOW;
static char HOME[1024], SL_DIR[1100], PROJECTS_DIR[1100];

// ── ring buffers (so multiple fmt calls in one printf don't clobber) ──────────
static char g_ring[48][96];
static int g_ringi;
static char *rb(void) { char *p = g_ring[g_ringi++ & 47]; p[0] = 0; return p; }

// ── small string vector ───────────────────────────────────────────────────────
typedef struct { char **v; int n, cap; } vec;
static void vec_push(vec *a, const char *s) {
  if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 8; a->v = realloc(a->v, a->cap * sizeof(char *)); }
  a->v[a->n++] = strdup(s);
}
static int vec_has(vec *a, const char *s) { for (int i = 0; i < a->n; i++) if (!strcmp(a->v[i], s)) return 1; return 0; }

// ── hashset (string) via uthash ────────────────────────────────────────────────
typedef struct { char *key; UT_hash_handle hh; } sitem;
static int set_add(sitem **set, const char *k) { // returns 1 if newly added, 0 if existed
  if (!k || !*k) return 1;
  sitem *it; HASH_FIND_STR(*set, k, it);
  if (it) return 0;
  it = malloc(sizeof(sitem)); it->key = strdup(k);
  HASH_ADD_KEYPTR(hh, *set, it->key, strlen(it->key), it);
  return 1;
}
static void set_free(sitem **set) {
  sitem *it, *tmp; HASH_ITER(hh, *set, it, tmp) { HASH_DEL(*set, it); free(it->key); free(it); }
}

// ── file + cmd helpers ─────────────────────────────────────────────────────────
static char *read_file(const char *path, long *outlen) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  char *buf = malloc(n + 1); if (!buf) { fclose(f); return NULL; }
  size_t rd = fread(buf, 1, n, f); fclose(f); buf[rd] = 0; if (outlen) *outlen = (long)rd;
  return buf;
}
static char *read_stdin(void) {
  size_t cap = 65536, len = 0; char *buf = malloc(cap);
  size_t r; while ((r = fread(buf + len, 1, cap - len, stdin)) > 0) {
    len += r; if (len == cap) { cap *= 2; buf = realloc(buf, cap); }
  }
  buf[len] = 0; return buf;
}
static void rtrim(char *s) { long n = (long)strlen(s); while (n > 0 && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n] = 0; }
// run command, return trimmed stdout (malloc'd) or NULL.
// Windows: CreateProcess directly (NO cmd.exe shell) — avoids per-call shell spawn tax.
#ifdef _WIN32
static char *run_cmd(const char *cmd) {
  SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
  HANDLE rd, wr; if (!CreatePipe(&rd, &wr, &sa, 0)) return NULL;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nulOut = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, NULL);
  HANDLE nulIn = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, NULL);
  STARTUPINFOA si = { sizeof(si) }; si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr; si.hStdError = nulOut; si.hStdInput = nulIn;
  PROCESS_INFORMATION pi; char *mut = strdup(cmd);
  BOOL ok = CreateProcessA(NULL, mut, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  free(mut); CloseHandle(wr);
  if (!ok) { CloseHandle(rd); if (nulOut != INVALID_HANDLE_VALUE) CloseHandle(nulOut); if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn); return NULL; }
  size_t cap = 4096, len = 0; char *buf = malloc(cap); DWORD n;
  while (ReadFile(rd, buf + len, (DWORD)(cap - len), &n, NULL) && n > 0) { len += n; if (len == cap) { cap *= 2; buf = realloc(buf, cap); } }
  buf[len] = 0; CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  if (nulOut != INVALID_HANDLE_VALUE) CloseHandle(nulOut); if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn);
  rtrim(buf); return buf;
}
#else
static char *run_cmd(const char *cmd) {
  FILE *p = popen(cmd, "r"); if (!p) return NULL;
  size_t cap = 4096, len = 0; char *buf = malloc(cap);
  size_t r; while ((r = fread(buf + len, 1, cap - len, p)) > 0) { len += r; if (len == cap) { cap *= 2; buf = realloc(buf, cap); } }
  buf[len] = 0; pclose(p); rtrim(buf); return buf;
}
#endif
#ifdef _WIN32
static int file_exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
static int is_dir(const char *path) { DWORD a = GetFileAttributesA(path); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
#else
static int file_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
static int is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && (st.st_mode & S_IFDIR); }
#endif
// ── fast directory iterator — Win32 FindFirstFile yields name+isdir+mtime inline
//    (no per-entry stat/GetFileAttributes), POSIX falls back to readdir+stat. ──
typedef struct {
#ifdef _WIN32
  HANDLE h; WIN32_FIND_DATAA fd; int first, valid;
#else
  DIR *d; char base[2048];
#endif
} dirit;
static int di_open(dirit *it, const char *path) {
#ifdef _WIN32
  char pat[2048]; snprintf(pat, sizeof(pat), "%s\\*", path);
  it->h = FindFirstFileA(pat, &it->fd); it->first = 1; it->valid = (it->h != INVALID_HANDLE_VALUE);
  return it->valid;
#else
  it->d = opendir(path); if (it->d) snprintf(it->base, sizeof(it->base), "%s", path); return it->d != NULL;
#endif
}
static int di_next(dirit *it, const char **name, int *isdir, long *mtime) {
#ifdef _WIN32
  for (;;) {
    if (!it->valid) return 0;
    if (!it->first && !FindNextFileA(it->h, &it->fd)) return 0;
    it->first = 0;
    const char *n = it->fd.cFileName;
    if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
    *name = n; *isdir = (it->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    if (mtime) { ULARGE_INTEGER u; u.LowPart = it->fd.ftLastWriteTime.dwLowDateTime; u.HighPart = it->fd.ftLastWriteTime.dwHighDateTime;
      *mtime = (long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL); }
    return 1;
  }
#else
  struct dirent *e;
  while ((e = readdir(it->d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char full[2200]; snprintf(full, sizeof(full), "%s/%s", it->base, e->d_name);
    struct stat st; if (stat(full, &st) != 0) continue;
    *name = e->d_name; *isdir = (st.st_mode & S_IFDIR) ? 1 : 0; if (mtime) *mtime = (long)st.st_mtime;
    return 1;
  }
  return 0;
#endif
}
static void di_close(dirit *it) {
#ifdef _WIN32
  if (it->valid) FindClose(it->h);
#else
  if (it->d) closedir(it->d);
#endif
}

// ── JSON main doc accessors ────────────────────────────────────────────────────
static yyjson_doc *J;
static yyjson_val *gp(const char *ptr) { return J ? yyjson_doc_ptr_get(J, ptr) : NULL; }
static const char *gstr(const char *ptr, const char *def) { const char *s = yyjson_get_str(gp(ptr)); return s ? s : def; }
static double gnum(const char *ptr, double def) { yyjson_val *v = gp(ptr); return (v && yyjson_is_num(v)) ? yyjson_get_num(v) : def; }
static int gbool(const char *ptr, int def) { yyjson_val *v = gp(ptr); return (v && yyjson_is_bool(v)) ? yyjson_get_bool(v) : def; }

static void slashfix(char *s) { for (; *s; s++) if (*s == '\\') *s = '/'; }

// ── pricing [in,out,cacheWrite,cacheRead] per Mtok ─────────────────────────────
static void pricing(const char *id, double p[4]) {
  if (!id) id = "";
  if (!strncmp(id, "claude-opus-4-", 14) && id[14] >= '5' && id[14] <= '8') { p[0]=5;p[1]=25;p[2]=6.25;p[3]=0.5; return; }
  if (!strncmp(id, "claude-opus-4", 13) || !strncmp(id, "claude-3-opus", 13)) { p[0]=15;p[1]=75;p[2]=18.75;p[3]=1.5; return; }
  if (!strncmp(id, "claude-sonnet-4", 15) || !strncmp(id, "claude-3-7-sonnet", 17) || !strncmp(id, "claude-3-5-sonnet", 17)) { p[0]=3;p[1]=15;p[2]=3.75;p[3]=0.3; return; }
  if (!strncmp(id, "claude-haiku-4-5", 16)) { p[0]=1;p[1]=5;p[2]=1.25;p[3]=0.1; return; }
  if (!strncmp(id, "claude-haiku-3-5", 16) || !strncmp(id, "claude-3-5-haiku", 16)) { p[0]=0.8;p[1]=4;p[2]=1;p[3]=0.08; return; }
  p[0]=3;p[1]=15;p[2]=3.75;p[3]=0.3;
}
static double P[4]; // pricing for current MODEL_ID (used by repo/day/week/month/burn)

// ── usage row ──────────────────────────────────────────────────────────────────
typedef struct { const char *ts; const char *model; long in, out, cw, cr; const char *id; } urow;
typedef void (*rowcb)(const urow *, void *);

// iterate assistant usage rows of a .jsonl file
static void file_usage(const char *path, rowcb cb, void *ctx) {
  char *buf = read_file(path, NULL); if (!buf) return;
  char *save = NULL, *line = strtok_r(buf, "\n", &save);
  for (; line; line = strtok_r(NULL, "\n", &save)) {
    if (!strstr(line, "\"assistant\"")) continue;
    yyjson_doc *d = yyjson_read(line, strlen(line), 0); if (!d) continue;
    yyjson_val *o = yyjson_doc_get_root(d);
    const char *type = yyjson_get_str(yyjson_obj_get(o, "type"));
    if (!type || strcmp(type, "assistant")) { yyjson_doc_free(d); continue; }
    yyjson_val *msg = yyjson_obj_get(o, "message");
    yyjson_val *u = msg ? yyjson_obj_get(msg, "usage") : NULL;
    if (!u) { yyjson_doc_free(d); continue; }
    urow r;
    r.ts = yyjson_get_str(yyjson_obj_get(o, "timestamp")); if (!r.ts) r.ts = "";
    r.model = msg ? yyjson_get_str(yyjson_obj_get(msg, "model")) : NULL; if (!r.model) r.model = "default";
    r.in = (long)yyjson_get_num(yyjson_obj_get(u, "input_tokens"));
    r.out = (long)yyjson_get_num(yyjson_obj_get(u, "output_tokens"));
    r.cw = (long)yyjson_get_num(yyjson_obj_get(u, "cache_creation_input_tokens"));
    r.cr = (long)yyjson_get_num(yyjson_obj_get(u, "cache_read_input_tokens"));
    r.id = msg ? yyjson_get_str(yyjson_obj_get(msg, "id")) : NULL;
    if (!r.id) r.id = yyjson_get_str(yyjson_obj_get(o, "requestId"));
    if (!r.id) r.id = yyjson_get_str(yyjson_obj_get(o, "uuid"));
    if (!r.id) r.id = "";
    cb(&r, ctx);
    yyjson_doc_free(d);
  }
  free(buf);
}
static double row_cost(const urow *r, const double p[4]) {
  return (r->in * p[0] + r->out * p[1] + r->cw * p[2] + r->cr * p[3]) / 1e6;
}

// recursive .jsonl walk with optional max-age filter (days). 0 = no filter.
static void walk_jsonl(const char *dir, int maxAgeDays, void (*onfile)(const char *, void *), void *ctx) {
  dirit it; if (!di_open(&it, dir)) return;
  double cutoff = maxAgeDays ? ((double)NOW - (double)maxAgeDays * 86400.0) : 0;
  const char *name; int isdir; long mt; char full[2048];
  while (di_next(&it, &name, &isdir, &mt)) {
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    if (isdir) { walk_jsonl(full, maxAgeDays, onfile, ctx); continue; }
    size_t ln = strlen(name);
    if (ln < 6 || strcmp(name + ln - 6, ".jsonl")) continue;
    if (cutoff && (double)mt < cutoff) continue;
    onfile(full, ctx);
  }
  di_close(&it);
}

// ── cache read/write (cost-cache etc) ──────────────────────────────────────────
static yyjson_doc *read_cache(const char *name) {
  char path[1200]; snprintf(path, sizeof(path), "%s/%s", SL_DIR, name);
  char *buf = read_file(path, NULL); if (!buf) return NULL;
  yyjson_doc *d = yyjson_read(buf, strlen(buf), YYJSON_READ_INSITU | YYJSON_READ_ALLOW_TRAILING_COMMAS);
  // INSITU mutates buf; keep buf alive by leaking (short-lived process). Simpler: copy parse.
  return d;
}
static yyjson_doc *read_cache_copy(const char *name) {
  char path[1200]; snprintf(path, sizeof(path), "%s/%s", SL_DIR, name);
  char *buf = read_file(path, NULL); if (!buf) return NULL;
  yyjson_doc *d = yyjson_read(buf, strlen(buf), 0); free(buf); return d;
}
static void write_cache_raw(const char *name, const char *json) {
  char path[1200]; snprintf(path, sizeof(path), "%s/%s", SL_DIR, name);
  FILE *f = fopen(path, "wb"); if (!f) return; fputs(json, f); fclose(f);
}

// ── formatting ──────────────────────────────────────────────────────────────────
static const char *fmtTok(long t) {
  char *o = rb();
  if (t >= 1000000) snprintf(o, 96, "%.1fM", t / 1e6);
  else if (t >= 1000) snprintf(o, 96, "%.1fk", t / 1000.0);
  else snprintf(o, 96, "%ld", t);
  return o;
}
static const char *usd(double n) { char *o = rb(); snprintf(o, 96, "$%.2f", n); return o; }
static const char *to12h(const char *hhmm) {
  int h = 0, m = 0; sscanf(hhmm, "%d:%d", &h, &m);
  const char *ap = h >= 12 ? "PM" : "AM"; int h12 = h % 12; if (!h12) h12 = 12;
  char *o = rb(); snprintf(o, 96, "%d:%02d %s", h12, m, ap); return o;
}
static const char *clock_now(void) {
  time_t t = NOW; struct tm *lt = localtime(&t);
  int h = lt->tm_hour; const char *ap = h >= 12 ? "PM" : "AM"; int h12 = h % 12; if (!h12) h12 = 12;
  char *o = rb(); snprintf(o, 96, "%d:%02d %s", h12, lt->tm_min, ap); return o;
}
static const char *WD[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *MO[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
static const char *fullDate(void) {
  time_t t = NOW; struct tm *lt = localtime(&t);
  char *o = rb(); snprintf(o, 96, "%s, %s %d, %d", WD[lt->tm_wday], MO[lt->tm_mon], lt->tm_mday, lt->tm_year + 1900);
  return o;
}
static const char *fmtDur(double ms) {
  if (!ms) return "";
  long s = (long)(ms / 1000), m = s / 60, h = m / 60;
  char *o = rb();
  if (h > 0) snprintf(o, 96, "%ldh %ldm", h, m % 60);
  else if (m > 0) snprintf(o, 96, "%ldm", m);
  else snprintf(o, 96, "%lds", s);
  return o;
}
static const char *shortModel(const char *id) {
  int a, b; char *o = rb();
  const char *p;
  if ((p = strstr(id, "opus-")) && sscanf(p, "opus-%d-%d", &a, &b) == 2) { snprintf(o, 96, "Opus %d.%d", a, b); return o; }
  if ((p = strstr(id, "sonnet-")) && sscanf(p, "sonnet-%d-%d", &a, &b) == 2) { snprintf(o, 96, "Sonnet %d.%d", a, b); return o; }
  if ((p = strstr(id, "haiku-")) && sscanf(p, "haiku-%d-%d", &a, &b) == 2) { snprintf(o, 96, "Haiku %d.%d", a, b); return o; }
  snprintf(o, 96, "%s", id); return o;
}

// ── globals for accumulation ────────────────────────────────────────────────────
typedef struct { sitem *seen; double sum; } costctx;
static void cb_repo(const urow *r, void *vp) { costctx *c = vp; if (!set_add(&c->seen, r->id)) return; c->sum += row_cost(r, P); }
static void onfile_repo(const char *f, void *vp) { file_usage(f, cb_repo, vp); }

typedef struct { sitem *seen; double day, week, month; const char *todayISO, *weekISO, *monthISO; } dwmctx;
static void cb_dwm(const urow *r, void *vp) {
  dwmctx *c = vp; if (!set_add(&c->seen, r->id)) return;
  char ts[40]; snprintf(ts, sizeof(ts), "%s", r->ts);
  char *dot = strpbrk(ts, ".Z"); if (dot) *dot = 0; // strip .\d+Z?
  double cost = row_cost(r, P);
  if (strcmp(ts, c->monthISO) >= 0) c->month += cost;
  if (strcmp(ts, c->weekISO)  >= 0) c->week  += cost;
  if (strcmp(ts, c->todayISO) >= 0) c->day   += cost;
}
static void onfile_dwm(const char *f, void *vp) { file_usage(f, cb_dwm, vp); }

typedef struct { sitem *seen; const char *cutoff; double toks, cst; } burnctx;
static void cb_burn(const urow *r, void *vp) {
  burnctx *c = vp; char ts[40]; snprintf(ts, sizeof(ts), "%s", r->ts);
  char *dot = strpbrk(ts, ".Z"); if (dot) *dot = 0;
  if (strcmp(ts, c->cutoff) < 0) return;
  if (!set_add(&c->seen, r->id)) return;
  c->toks += r->in + r->out; c->cst += row_cost(r, P);
}

// per-model accumulation
typedef struct { char id[96]; double cost; long tokens; long count; } mstat;
typedef struct {
  sitem *seen; long sIn, sOut, sCw, sCr;
  mstat models[16]; int nmodels;
  int haveFirst; long firstBase; // firstRow.cr+cw
  long lastIn, lastCw, lastCr, lastOut; int haveLast;
} sessctx;
static void cb_sess(const urow *r, void *vp) {
  sessctx *c = vp; if (!set_add(&c->seen, r->id)) return;
  c->sIn += r->in; c->sOut += r->out; c->sCw += r->cw; c->sCr += r->cr;
  if (!strcmp(r->model, "<synthetic>")) return;
  if (!c->haveFirst) { c->haveFirst = 1; c->firstBase = r->cr + r->cw; }
  c->lastIn = r->in; c->lastCw = r->cw; c->lastCr = r->cr; c->lastOut = r->out; c->haveLast = 1;
  double mp[4]; pricing(r->model, mp);
  double cost = (r->in*mp[0] + r->out*mp[1] + r->cw*mp[2] + r->cr*mp[3]) / 1e6;
  mstat *m = NULL;
  for (int i = 0; i < c->nmodels; i++) if (!strcmp(c->models[i].id, r->model)) { m = &c->models[i]; break; }
  if (!m && c->nmodels < 16) { m = &c->models[c->nmodels++]; snprintf(m->id, sizeof(m->id), "%s", r->model); m->cost = 0; m->tokens = 0; m->count = 0; }
  if (m) { m->cost += cost; m->tokens += r->in + r->out + r->cw + r->cr; m->count++; }
}

// ── launch-dir DFS decode (fallback) ────────────────────────────────────────────
static int dfs_launch(const char *base, char parts[][256], int nparts, char *seg, int i, char *out) {
  if (i == nparts) {
    char full[2048]; snprintf(full, sizeof(full), "%s%s", base, seg);
    if (file_exists(full)) { strcpy(out, full); return 1; }
    return 0;
  }
  char trydir[2048]; snprintf(trydir, sizeof(trydir), "%s%s", base, seg);
  if (is_dir(trydir)) {
    char nb[2048]; snprintf(nb, sizeof(nb), "%s%s/", base, seg);
    if (dfs_launch(nb, parts, nparts, parts[i], i + 1, out)) return 1;
  }
  char nseg[1024]; snprintf(nseg, sizeof(nseg), "%s-%s", seg, parts[i]);
  return dfs_launch(base, parts, nparts, nseg, i + 1, out);
}
static void decode_launch(const char *transcript, char *out) {
  out[0] = 0;
  const char *marker = "/.claude/projects/";
  const char *m = strstr(transcript, marker); if (!m) return;
  char slug[1024]; snprintf(slug, sizeof(slug), "%s", m + strlen(marker));
  char *sl = strchr(slug, '/'); if (sl) *sl = 0;
  char *s = slug; if (*s == '-') s++;
  // s = "X--rest"
  if (strlen(s) < 3 || s[1] != '-' || s[2] != '-') return;
  char drive = s[0]; char base[8]; snprintf(base, sizeof(base), "%c:/", drive >= 'a' ? drive - 32 : drive);
  char rest[1024]; snprintf(rest, sizeof(rest), "%s", s + 3);
  // split rest by '-'
  static char parts[128][256]; int nparts = 0;
  char *save = NULL, *tok = strtok_r(rest, "-", &save);
  while (tok && nparts < 128) { snprintf(parts[nparts++], 256, "%s", tok); tok = strtok_r(NULL, "-", &save); }
  if (nparts == 0) return;
  dfs_launch(base, parts, nparts, parts[0], 1, out);
}

int main(void) {
  NOW = (long)time(NULL);
  const char *up = getenv("USERPROFILE"); if (!up) up = getenv("HOME"); if (!up) up = ".";
  snprintf(HOME, sizeof(HOME), "%s", up); slashfix(HOME);
  snprintf(SL_DIR, sizeof(SL_DIR), "%s/.claude/statusline", HOME);
  snprintf(PROJECTS_DIR, sizeof(PROJECTS_DIR), "%s/.claude/projects", HOME);

  char *raw = read_stdin();
  J = yyjson_read(raw, strlen(raw), 0);

  // ── field extraction ──
  const char *MODEL = gstr("/model/display_name", "?");
  char MODEL_ID[256]; snprintf(MODEL_ID, sizeof(MODEL_ID), "%s", gstr("/model/id", ""));
  const char *SESSION_ID = gstr("/session_id", "");
  const char *SESSION_NAME = gstr("/session_name", "");
  const char *VERSION = gstr("/version", "");

  char CWD[1024]; snprintf(CWD, sizeof(CWD), "%s", gstr("/workspace/current_dir", ".")); slashfix(CWD);
  char PROJECT_DIR[1024]; snprintf(PROJECT_DIR, sizeof(PROJECT_DIR), "%s", gstr("/workspace/project_dir", "")); slashfix(PROJECT_DIR);
  char TRANSCRIPT[1024]; snprintf(TRANSCRIPT, sizeof(TRANSCRIPT), "%s", gstr("/transcript_path", "")); slashfix(TRANSCRIPT);
  yyjson_val *ADDED_DIRS = gp("/workspace/added_dirs");
  int addedDirsN = (ADDED_DIRS && yyjson_is_arr(ADDED_DIRS)) ? (int)yyjson_arr_size(ADDED_DIRS) : 0;
  const char *GIT_WORKTREE = gstr("/workspace/git_worktree", "");

  const char *REPO_OWNER = gstr("/workspace/repo/owner", "");
  const char *REPO_NAME = gstr("/workspace/repo/name", "");

  int PCT = (int)gnum("/context_window/used_percentage", 0);
  long CTX_TOTAL_IN = (long)gnum("/context_window/total_input_tokens", 0);
  long CTX_TOTAL_OUT = (long)gnum("/context_window/total_output_tokens", 0);
  long CTX_SIZE = (long)gnum("/context_window/context_window_size", 200000); if (!CTX_SIZE) CTX_SIZE = 200000;
  int EXCEEDS_200K = gbool("/exceeds_200k_tokens", 0);
  long CU_IN = (long)gnum("/context_window/current_usage/input_tokens", 0);
  long CU_CW = (long)gnum("/context_window/current_usage/cache_creation_input_tokens", 0);
  long CU_CR = (long)gnum("/context_window/current_usage/cache_read_input_tokens", 0);
  long CU_OUT = (long)gnum("/context_window/current_usage/output_tokens", 0);
  long CTX_TOKENS = CU_IN + CU_CW + CU_CR;

  double LIVE_COST = gnum("/cost/total_cost_usd", 0);
  long LINES_ADD = (long)gnum("/cost/total_lines_added", 0);
  long LINES_DEL = (long)gnum("/cost/total_lines_removed", 0);
  double DURATION_MS = gnum("/cost/total_duration_ms", 0);
  double API_DUR_MS = gnum("/cost/total_api_duration_ms", 0);

  yyjson_val *vR5 = gp("/rate_limits/five_hour/resets_at");
  yyjson_val *vR7 = gp("/rate_limits/seven_day/resets_at");
  long RATE_5H_RESET = (vR5 && yyjson_is_num(vR5)) ? (long)yyjson_get_num(vR5) : 0;
  long RATE_7D_RESET = (vR7 && yyjson_is_num(vR7)) ? (long)yyjson_get_num(vR7) : 0;
  double RATE_5H = gnum("/rate_limits/five_hour/used_percentage", 0);
  double RATE_7D = gnum("/rate_limits/seven_day/used_percentage", 0);

  const char *EFFORT = gstr("/effort/level", "");
  int THINKING = gbool("/thinking/enabled", 0);
  int FAST_MODE = gbool("/fast_mode", 0);
  const char *OUTPUT_STYLE = gstr("/output_style/name", "");
  const char *VIM_MODE = gstr("/vim/mode", "");
  const char *AGENT_NAME = gstr("/agent/name", "");

  yyjson_val *vPR = gp("/pr/number");
  int PR_NUM = (vPR && yyjson_is_num(vPR)) ? (int)yyjson_get_num(vPR) : 0;
  const char *PR_STATE = gstr("/pr/review_state", "");
  const char *WT_NAME = gstr("/worktree/name", ""); if (!*WT_NAME) WT_NAME = GIT_WORKTREE;
  const char *WT_BRANCH = gstr("/worktree/branch", "");

  pricing(MODEL_ID, P);

  // ── ISO cutoffs ──
  char todayISO[40], weekISO[40], monthISO[40];
  { time_t t = NOW; struct tm lt = *localtime(&t); lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0;
    strftime(todayISO, sizeof(todayISO), "%Y-%m-%dT00:00:00", &lt); }
  { time_t t = NOW - 7 * 86400; struct tm gt = *gmtime(&t); strftime(weekISO, sizeof(weekISO), "%Y-%m-%dT%H:%M:%S", &gt); }
  { time_t t = NOW - 30 * 86400; struct tm gt = *gmtime(&t); strftime(monthISO, sizeof(monthISO), "%Y-%m-%dT%H:%M:%S", &gt); }

  // ── costs (5min cache) ──
  double cost_repo = 0, cost_month = 0, cost_week = 0, cost_day = 0;
  int cost_cached = 0;
  yyjson_doc *cc = read_cache_copy("cost-cache.json");
  if (cc) {
    yyjson_val *cr = yyjson_doc_get_root(cc);
    long ts = (long)yyjson_get_num(yyjson_obj_get(cr, "ts"));
    yyjson_val *dd = yyjson_obj_get(cr, "d");
    if (dd && NOW - ts < 300) {
      cost_repo = yyjson_get_num(yyjson_obj_get(dd, "repo"));
      cost_month = yyjson_get_num(yyjson_obj_get(dd, "month"));
      cost_week = yyjson_get_num(yyjson_obj_get(dd, "week"));
      cost_day = yyjson_get_num(yyjson_obj_get(dd, "day"));
      cost_cached = 1;
    }
  }
  if (!cost_cached) {
    // REPO = main project + worktrees (slug prefix from DIR repoRoot)
    char repoRoot[1024]; snprintf(repoRoot, sizeof(repoRoot), "%s", CWD);
    char *wt = strstr(repoRoot, "/.worktrees/"); if (wt) *wt = 0;
    char repoSlug[1024]; { int k = 0; for (const char *s = repoRoot; *s && k < 1023; s++) repoSlug[k++] = (*s==':'||*s=='/'||*s=='\\') ? '-' : *s; repoSlug[k] = 0; }
    costctx rc = {0};
    dirit pit;
    if (di_open(&pit, PROJECTS_DIR)) {
      char wtpfx[1100]; snprintf(wtpfx, sizeof(wtpfx), "%s--worktrees-", repoSlug);
      const char *name; int isdir;
      while (di_next(&pit, &name, &isdir, NULL)) {
        if (strcmp(name, repoSlug) && strncmp(name, wtpfx, strlen(wtpfx))) continue;
        char pf[1200]; snprintf(pf, sizeof(pf), "%s/%s", PROJECTS_DIR, name);
        if (isdir) walk_jsonl(pf, 0, onfile_repo, &rc);
      }
      di_close(&pit);
    }
    cost_repo = rc.sum; set_free(&rc.seen);
    dwmctx dc = {0}; dc.todayISO = todayISO; dc.weekISO = weekISO; dc.monthISO = monthISO;
    walk_jsonl(PROJECTS_DIR, 31, onfile_dwm, &dc);
    cost_day = dc.day; cost_week = dc.week; cost_month = dc.month; set_free(&dc.seen);
    char out[512]; snprintf(out, sizeof(out),
      "{\"d\":{\"repo\":%.10g,\"month\":%.10g,\"week\":%.10g,\"day\":%.10g},\"ts\":%ld}",
      cost_repo, cost_month, cost_week, cost_day, NOW);
    write_cache_raw("cost-cache.json", out);
  }
  if (cc) yyjson_doc_free(cc);

  // ── burn rate (5min window, session-scoped 30s cache) ──
  double burn_tpm = 0, burn_cph = 0;
  int burn_cached = 0;
  yyjson_doc *bc = read_cache_copy("burn-cache.json");
  if (bc) {
    yyjson_val *br = yyjson_doc_get_root(bc);
    const char *sid = yyjson_get_str(yyjson_obj_get(br, "sid"));
    long ts = (long)yyjson_get_num(yyjson_obj_get(br, "ts"));
    yyjson_val *dd = yyjson_obj_get(br, "d");
    if (dd && sid && !strcmp(sid, SESSION_ID) && NOW - ts < 30) {
      burn_tpm = yyjson_get_num(yyjson_obj_get(dd, "tpm"));
      burn_cph = yyjson_get_num(yyjson_obj_get(dd, "cph"));
      burn_cached = 1;
    }
    yyjson_doc_free(bc);
  }
  if (!burn_cached && *TRANSCRIPT && file_exists(TRANSCRIPT)) {
    char cutoff[40]; { time_t t = NOW - 300; struct tm gt = *gmtime(&t); strftime(cutoff, sizeof(cutoff), "%Y-%m-%dT%H:%M:%S", &gt); }
    burnctx bx = {0}; bx.cutoff = cutoff;
    file_usage(TRANSCRIPT, cb_burn, &bx);
    burn_tpm = (double)(long)(bx.toks / 5); burn_cph = bx.cst * 12;
    set_free(&bx.seen);
    char out[256]; snprintf(out, sizeof(out), "{\"d\":{\"tpm\":%.10g,\"cph\":%.10g},\"ts\":%ld,\"sid\":\"%s\"}", burn_tpm, burn_cph, NOW, SESSION_ID);
    write_cache_raw("burn-cache.json", out);
  }

  // ── session tokens + per-model + ctx breakdown ──
  sessctx sx = {0};
  if (*TRANSCRIPT && file_exists(TRANSCRIPT)) file_usage(TRANSCRIPT, cb_sess, &sx);
  long sIn = sx.sIn, sOut = sx.sOut, sCw = sx.sCw, sCr = sx.sCr;
  set_free(&sx.seen);
  long cbIn = CU_IN ? CU_IN : (sx.haveLast ? sx.lastIn : 0);
  long cbCw = CU_CW ? CU_CW : (sx.haveLast ? sx.lastCw : 0);
  long cbCr = CU_CR ? CU_CR : (sx.haveLast ? sx.lastCr : 0);
  long cbOut = CU_OUT ? CU_OUT : (sx.haveLast ? sx.lastOut : 0);
  long baseOverhead = sx.haveFirst ? sx.firstBase : 0;

  // ── git ──
  // PERF: one `status --porcelain=v2 --branch` yields branch + ahead/behind + dirty
  //       in a single spawn (replaces branch/status/rev-list-left-right = 3 calls).
  char gitBranch[256] = "", gitCommits[64] = "0"; const char *gitIcon = "✅";
  long gitAhead = 0, gitBehind = 0, gitSubmodules = 0; int gitSubDirty = 0;
  {
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" status --porcelain=v2 --branch %s", CWD, NULDEV);
    char *stv = run_cmd(cmd);
    int isRepo = 0, dirty = 0;
    if (stv && *stv) {
      char *save = NULL;
      for (char *line = strtok_r(stv, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#') {
          if (!strncmp(line, "# branch.head ", 14)) { isRepo = 1; const char *h = line + 14;
            if (strcmp(h, "(detached)")) snprintf(gitBranch, sizeof(gitBranch), "%s", h); }
          else if (!strncmp(line, "# branch.ab ", 12)) { long a = 0, b = 0; sscanf(line + 12, "+%ld -%ld", &a, &b); gitAhead = a; gitBehind = b; }
        } else if (line[0]) { dirty = 1; }
      }
    }
    free(stv);
    if (isRepo && *gitBranch) {
      if (dirty) gitIcon = "⚠️";
      snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-list --count HEAD %s", CWD, NULDEV);
      char *cnt = run_cmd(cmd); if (cnt && *cnt) snprintf(gitCommits, sizeof(gitCommits), "%s", cnt); free(cnt);
      snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-parse --show-toplevel %s", CWD, NULDEV);
      char *top = run_cmd(cmd);
      if (top && *top) {
        char gm[1200]; snprintf(gm, sizeof(gm), "%s/.gitmodules", top);
        char *gmc = read_file(gm, NULL);
        if (gmc) {
          // count occurrences of "[submodule " at line start
          char *p = gmc; if (!strncmp(p, "[submodule ", 11)) gitSubmodules++;
          while ((p = strstr(p, "\n[submodule "))) { gitSubmodules++; p += 1; }
          free(gmc);
          if (gitSubmodules > 0) {
            snprintf(cmd, sizeof(cmd), "git -C \"%s\" submodule status %s", CWD, NULDEV);
            char *ss = run_cmd(cmd);
            if (ss) { for (char *q = ss; *q; ) { if (*q=='+'||*q=='-'||*q=='U') { gitSubDirty = 1; break; } char *nl = strchr(q, '\n'); if (!nl) break; q = nl + 1; } free(ss); }
          }
        }
      }
      free(top);
    }
  }

  // ── launch dir ──
  char LAUNCH_DIR[1024];
  if (*PROJECT_DIR) snprintf(LAUNCH_DIR, sizeof(LAUNCH_DIR), "%s", PROJECT_DIR);
  else { char dec[2048]; decode_launch(TRANSCRIPT, dec); snprintf(LAUNCH_DIR, sizeof(LAUNCH_DIR), "%s", *dec ? dec : CWD); }

  // ── location (6h cache) ──
  char locCity[256] = "", locCountry[256] = ""; double locLat = 0, locLon = 0; int haveLatLon = 0;
  yyjson_doc *lc = read_cache_copy("location-cache.json"); int locFresh = 0;
  if (lc) {
    yyjson_val *lr = yyjson_doc_get_root(lc);
    long ca = (long)yyjson_get_num(yyjson_obj_get(lr, "cached_at"));
    if (NOW - ca <= 21600) locFresh = 1;
  }
  if (!locFresh) {
    char *out = run_cmd("curl -sL --max-time 5 \"http://ip-api.com/json/\" " NULDEV);
    if (out && *out) {
      yyjson_doc *d = yyjson_read(out, strlen(out), 0);
      if (d) {
        yyjson_val *r = yyjson_doc_get_root(d);
        const char *status = yyjson_get_str(yyjson_obj_get(r, "status"));
        if (status && !strcmp(status, "success")) {
          char buf[1024]; snprintf(buf, sizeof(buf), "%.*s", (int)(strlen(out) - 1), out);
          // re-emit with cached_at: simplest is to store raw + separate; just store fields we need
          // Write augmented json: take original, inject cached_at by rebuilding minimal
          const char *city = yyjson_get_str(yyjson_obj_get(r, "city"));
          const char *country = yyjson_get_str(yyjson_obj_get(r, "country"));
          yyjson_val *lat = yyjson_obj_get(r, "lat"), *lon = yyjson_obj_get(r, "lon");
          char w[1500]; snprintf(w, sizeof(w),
            "{\"status\":\"success\",\"city\":\"%s\",\"country\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"cached_at\":%ld}",
            city?city:"", country?country:"", lat?yyjson_get_num(lat):0, lon?yyjson_get_num(lon):0, NOW);
          write_cache_raw("location-cache.json", w);
          if (lc) yyjson_doc_free(lc);
          lc = read_cache_copy("location-cache.json");
          (void)buf;
        }
        yyjson_doc_free(d);
      }
    }
    free(out);
  }
  if (lc) {
    yyjson_val *lr = yyjson_doc_get_root(lc);
    const char *city = yyjson_get_str(yyjson_obj_get(lr, "city"));
    const char *country = yyjson_get_str(yyjson_obj_get(lr, "country"));
    yyjson_val *lat = yyjson_obj_get(lr, "lat"), *lon = yyjson_obj_get(lr, "lon");
    if (city) snprintf(locCity, sizeof(locCity), "%s", city);
    if (country) snprintf(locCountry, sizeof(locCountry), "%s", country);
    if (lat && yyjson_is_num(lat)) { locLat = yyjson_get_num(lat); haveLatLon = 1; }
    if (lon && yyjson_is_num(lon)) locLon = yyjson_get_num(lon);
  }

  // ── prayer times (daily cache) ──
  char todayStr[16]; { time_t t = NOW; struct tm gt = *gmtime(&t); strftime(todayStr, sizeof(todayStr), "%Y-%m-%d", &gt); }
  yyjson_doc *pc = read_cache_copy("prayer-cache.json"); int prayerFresh = 0;
  if (pc) {
    yyjson_val *pr = yyjson_doc_get_root(pc);
    const char *dt = yyjson_get_str(yyjson_obj_get(pr, "date"));
    if (dt && !strcmp(dt, todayStr)) prayerFresh = 1;
  }
  if (!prayerFresh && haveLatLon) {
    char url[512]; snprintf(url, sizeof(url), "curl -sL --max-time 5 \"https://api.aladhan.com/v1/timings?latitude=%.6f&longitude=%.6f&method=1\" %s", locLat, locLon, NULDEV);
    char *out = run_cmd(url);
    if (out && *out) {
      yyjson_doc *d = yyjson_read(out, strlen(out), 0);
      if (d) {
        yyjson_val *r = yyjson_doc_get_root(d);
        yyjson_val *data = yyjson_obj_get(r, "data");
        yyjson_val *timings = data ? yyjson_obj_get(data, "timings") : NULL;
        if (timings) {
          // store raw out but add date field — wrap: {"date":"..","data":{...}}
          yyjson_val *date = yyjson_obj_get(data, "date");
          (void)date;
          // Simplest: write original 'out' but we need 'date' top-level. Rebuild minimal needed subset.
          // Keep full data by writing: {"date":"todayStr","data": <data-object-json>}
          size_t dl; char *dataj = yyjson_val_write(data, 0, &dl);
          if (dataj) {
            char *w = malloc(dl + 64);
            snprintf(w, dl + 64, "{\"date\":\"%s\",\"data\":%s}", todayStr, dataj);
            write_cache_raw("prayer-cache.json", w);
            free(w); free(dataj);
            if (pc) yyjson_doc_free(pc);
            pc = read_cache_copy("prayer-cache.json");
          }
        }
        yyjson_doc_free(d);
      }
    }
    free(out);
  }
  int hasPrayer = 0;
  const char *hijriDay = "", *hijriMonth = "", *hijriYear = "";
  char pFajr[16]="", pDhuhr[16]="", pAsr[16]="", pMaghrib[16]="", pIsha[16]="";
  if (pc) {
    yyjson_val *pr = yyjson_doc_get_root(pc);
    yyjson_val *data = yyjson_obj_get(pr, "data");
    yyjson_val *timings = data ? yyjson_obj_get(data, "timings") : NULL;
    if (timings) {
      hasPrayer = 1;
      yyjson_val *date = yyjson_obj_get(data, "date");
      yyjson_val *hijri = date ? yyjson_obj_get(date, "hijri") : NULL;
      if (hijri) {
        const char *hd = yyjson_get_str(yyjson_obj_get(hijri, "day"));
        yyjson_val *mon = yyjson_obj_get(hijri, "month");
        const char *hm = mon ? yyjson_get_str(yyjson_obj_get(mon, "en")) : NULL;
        const char *hy = yyjson_get_str(yyjson_obj_get(hijri, "year"));
        hijriDay = hd ? hd : ""; hijriMonth = hm ? hm : ""; hijriYear = hy ? hy : "";
      }
      #define GT(name,buf) { const char *x = yyjson_get_str(yyjson_obj_get(timings, name)); if (x) snprintf(buf, sizeof(buf), "%.5s", x); }
      GT("Fajr", pFajr) GT("Dhuhr", pDhuhr) GT("Asr", pAsr) GT("Maghrib", pMaghrib) GT("Isha", pIsha)
      #undef GT
    }
  }

  // ── plugins / skills / mcp ──
  vec plugins = {0};
  { char path[1200]; snprintf(path, sizeof(path), "%s/.claude/settings.json", HOME);
    char *buf = read_file(path, NULL);
    if (buf) { yyjson_doc *d = yyjson_read(buf, strlen(buf), 0);
      if (d) { yyjson_val *r = yyjson_doc_get_root(d); yyjson_val *ep = yyjson_obj_get(r, "enabledPlugins");
        if (ep && yyjson_is_obj(ep)) { yyjson_val *k, *v; yyjson_obj_iter it; yyjson_obj_iter_init(ep, &it);
          while ((k = yyjson_obj_iter_next(&it))) { v = yyjson_obj_iter_get_val(k);
            if (yyjson_is_bool(v) && yyjson_get_bool(v)) { char nm[256]; snprintf(nm, sizeof(nm), "%s", yyjson_get_str(k)); char *at = strchr(nm, '@'); if (at) *at = 0; vec_push(&plugins, nm); } } }
        yyjson_doc_free(d); }
      free(buf); } }

  // skills count
  int skillCount = 0;
  { sitem *skills = NULL;
    char sdir[1200]; snprintf(sdir, sizeof(sdir), "%s/.claude/skills", HOME);
    dirit d; const char *nm; int isdir;
    if (di_open(&d, sdir)) { while (di_next(&d, &nm, &isdir, NULL)) { if (!strcmp(nm,".orphaned_at")||!strcmp(nm,"CLAUDE.md")) continue; set_add(&skills, nm); } di_close(&d); }
    // walk plugins/cache depth<=4 for "skills" dirs
    char cache[1200]; snprintf(cache, sizeof(cache), "%s/.claude/plugins/cache", HOME);
    typedef struct { char path[1600]; int depth; } qn;
    qn *stack = malloc(4096 * sizeof(qn)); int sp = 0;
    snprintf(stack[sp].path, sizeof(stack[sp].path), "%s", cache); stack[sp].depth = 0; sp++;
    while (sp > 0) { qn cur = stack[--sp]; if (cur.depth > 4) continue;
      dirit dd; if (!di_open(&dd, cur.path)) continue;
      const char *en; int eisdir;
      while (di_next(&dd, &en, &eisdir, NULL)) { if (!eisdir) continue;
        char full[1700]; snprintf(full, sizeof(full), "%s/%s", cur.path, en);
        if (!strcmp(en, "skills")) { dirit sd; const char *sn; int sis; if (di_open(&sd, full)) { while (di_next(&sd, &sn, &sis, NULL)) { if (!strcmp(sn,".orphaned_at")||!strcmp(sn,"CLAUDE.md")) continue; set_add(&skills, sn); } di_close(&sd); } }
        else if (sp < 4096) { snprintf(stack[sp].path, sizeof(stack[sp].path), "%s", full); stack[sp].depth = cur.depth + 1; sp++; } }
      di_close(&dd); }
    skillCount = HASH_COUNT(skills); set_free(&skills); free(stack); }

  // mcp servers
  vec mcp = {0};
  { char path[1200]; snprintf(path, sizeof(path), "%s/.claude.json", HOME);
    char *buf = read_file(path, NULL);
    if (buf) { yyjson_doc *d = yyjson_read(buf, strlen(buf), 0);
      if (d) { yyjson_val *r = yyjson_doc_get_root(d); yyjson_val *ms = yyjson_obj_get(r, "mcpServers");
        if (ms && yyjson_is_obj(ms)) { yyjson_val *k; yyjson_obj_iter it; yyjson_obj_iter_init(ms, &it); while ((k = yyjson_obj_iter_next(&it))) vec_push(&mcp, yyjson_get_str(k)); }
        yyjson_doc_free(d); } free(buf); } }
  for (int b = 0; b < 2; b++) { const char *base = b == 0 ? LAUNCH_DIR : CWD;
    char path[1200]; snprintf(path, sizeof(path), "%s/.mcp.json", base);
    char *buf = read_file(path, NULL); if (!buf) continue;
    yyjson_doc *d = yyjson_read(buf, strlen(buf), 0);
    if (d) { yyjson_val *r = yyjson_doc_get_root(d); yyjson_val *ms = yyjson_obj_get(r, "mcpServers"); if (!ms) ms = r;
      if (ms && yyjson_is_obj(ms)) { yyjson_val *k; yyjson_obj_iter it; yyjson_obj_iter_init(ms, &it); while ((k = yyjson_obj_iter_next(&it))) { const char *nm = yyjson_get_str(k); if (nm && !vec_has(&mcp, nm)) vec_push(&mcp, nm); } }
      yyjson_doc_free(d); } free(buf); }

  // ── subagents ──
  int subCount = 0;
  typedef struct { char type[96]; int count; double cost; } sub_t;
  sub_t subs[32]; int nsubs = 0;
  if (*SESSION_ID) {
    char slug[1024]; { int k = 0; for (const char *s = CWD; *s && k < 1023; s++) slug[k++] = (*s==':'||*s=='/'||*s=='\\') ? '-' : *s; slug[k] = 0; }
    char projDir[1200]; snprintf(projDir, sizeof(projDir), "%s/%s", PROJECTS_DIR, slug);
    // collect agent-*.jsonl within 7 days
    // reuse walk to find files, but need filename filter agent- and within projDir tree
    // do manual recursion
    typedef struct { char path[1700]; } qn2; qn2 *stack = malloc(4096 * sizeof(qn2)); int sp = 0;
    snprintf(stack[sp++].path, sizeof(stack[0].path), "%s", projDir);
    double cutoff = (double)NOW - 7.0 * 86400.0;
    while (sp > 0) { qn2 cur = stack[--sp]; dirit dd; if (!di_open(&dd, cur.path)) continue;
      const char *en; int eisdir; long emt;
      while (di_next(&dd, &en, &eisdir, &emt)) {
        char full[1800]; snprintf(full, sizeof(full), "%s/%s", cur.path, en);
        if (eisdir) { if (sp < 4096) snprintf(stack[sp++].path, sizeof(stack[0].path), "%s", full); continue; }
        size_t ln = strlen(en);
        if (ln < 6 || strcmp(en + ln - 6, ".jsonl")) continue;
        if (strncmp(en, "agent-", 6)) continue;
        if ((double)emt < cutoff) continue;
        char *fb = read_file(full, NULL); if (!fb) continue;
        // first line sessionId check
        char *save = NULL; char *first = strtok_r(fb, "\n", &save);
        int match = 0; char agentType[96]; snprintf(agentType, sizeof(agentType), "general-purpose");
        if (first) { yyjson_doc *d0 = yyjson_read(first, strlen(first), 0); if (d0) { const char *sid = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(d0), "sessionId")); if (sid && !strcmp(sid, SESSION_ID)) match = 1; yyjson_doc_free(d0); } }
        if (!match) { free(fb); continue; }
        // find attributionAgent in any line — re-read whole (strtok consumed). Re-read file.
        free(fb); fb = read_file(full, NULL); if (!fb) continue;
        { char *p = strstr(fb, "\"attributionAgent\":\""); if (p) { p += 20; char *end = strchr(p, '"'); if (end) { char tmp[128]; int tl = (int)(end - p); if (tl > 127) tl = 127; memcpy(tmp, p, tl); tmp[tl] = 0; char *colon = strrchr(tmp, ':'); snprintf(agentType, sizeof(agentType), "%s", colon ? colon + 1 : tmp); } } }
        // cost: iterate assistant lines
        double agCost = 0; sitem *seen = NULL;
        char *save2 = NULL; for (char *ln2 = strtok_r(fb, "\n", &save2); ln2; ln2 = strtok_r(NULL, "\n", &save2)) {
          if (!strstr(ln2, "\"assistant\"")) continue;
          yyjson_doc *dd2 = yyjson_read(ln2, strlen(ln2), 0); if (!dd2) continue;
          yyjson_val *o = yyjson_doc_get_root(dd2); yyjson_val *msg = yyjson_obj_get(o, "message"); yyjson_val *u = msg ? yyjson_obj_get(msg, "usage") : NULL;
          if (u) { const char *id = msg ? yyjson_get_str(yyjson_obj_get(msg, "id")) : NULL; if (!id) id = yyjson_get_str(yyjson_obj_get(o, "requestId")); if (!id) id = yyjson_get_str(yyjson_obj_get(o, "uuid"));
            if (set_add(&seen, id ? id : "")) { double mp[4]; pricing(msg ? yyjson_get_str(yyjson_obj_get(msg, "model")) : "default", mp);
              agCost += ((long)yyjson_get_num(yyjson_obj_get(u,"input_tokens"))*mp[0] + (long)yyjson_get_num(yyjson_obj_get(u,"output_tokens"))*mp[1] + (long)yyjson_get_num(yyjson_obj_get(u,"cache_creation_input_tokens"))*mp[2] + (long)yyjson_get_num(yyjson_obj_get(u,"cache_read_input_tokens"))*mp[3]) / 1e6; } }
          yyjson_doc_free(dd2);
        }
        set_free(&seen); free(fb);
        subCount++;
        sub_t *st = NULL; for (int i = 0; i < nsubs; i++) if (!strcmp(subs[i].type, agentType)) { st = &subs[i]; break; }
        if (!st && nsubs < 32) { st = &subs[nsubs++]; snprintf(st->type, sizeof(st->type), "%s", agentType); st->count = 0; st->cost = 0; }
        if (st) { st->count++; st->cost += agCost; }
      }
      di_close(&dd); }
    free(stack);
  }

  // ════════════════════════════════════════ OUTPUT ════════════════════════════════
  // context bar
  int filled = PCT / 10; if (filled > 10) filled = 10; if (filled < 0) filled = 0;
  char ctxBar[64] = ""; for (int i = 0; i < filled; i++) strcat(ctxBar, "█"); for (int i = 0; i < 10 - filled; i++) strcat(ctxBar, "░");
  const char *ctxC = EXCEEDS_200K ? C_red : PCT >= 90 ? C_red : PCT >= 70 ? C_yellow : C_green;
  const char *burnC = burn_cph > 8 ? C_red : burn_cph > 3 ? C_orange : C_green;

  // dir display
  char dirDisplay[3072];
  size_t ldl = strlen(LAUNCH_DIR);
  int dirInside = (strcmp(CWD, LAUNCH_DIR) != 0) && !strncmp(CWD, LAUNCH_DIR, ldl) && CWD[ldl] == '/';
  if (!strcmp(CWD, LAUNCH_DIR)) snprintf(dirDisplay, sizeof(dirDisplay), C_white C_bold "%s" R, LAUNCH_DIR);
  else if (!dirInside) snprintf(dirDisplay, sizeof(dirDisplay), C_white C_bold "%s" R " " C_gray "→" R " " C_yellow "%s" R, LAUNCH_DIR, CWD);
  else snprintf(dirDisplay, sizeof(dirDisplay), C_white C_bold "%s" R " " C_gray "→" R " " C_yellow "%s" R, LAUNCH_DIR, CWD + ldl + 1);

  // ── LINE 1 ──
  { char L1[4096] = "";
    if (*SESSION_NAME) { char t[512]; snprintf(t, sizeof(t), C_pink C_bold "%s" R "  ", SESSION_NAME); strcat(L1, t); }
    strcat(L1, dirDisplay);
    if (*gitBranch) { char t[320]; snprintf(t, sizeof(t), " " C_teal "(%s)" R, gitBranch); strcat(L1, t); }
    { char t[64]; snprintf(t, sizeof(t), " %s", gitIcon); strcat(L1, t); }
    { char t[256]; snprintf(t, sizeof(t), " " SEP " \U0001f9e0 " C_magenta "%s" R, MODEL); strcat(L1, t); }
    if (FAST_MODE) strcat(L1, " " SEP " " C_yellow "⚡fast" R);
    if (EXCEEDS_200K) strcat(L1, " " SEP " " C_red C_bold "⚠ >200K" R);
    char ctxSizeStr[32]; if (CTX_SIZE >= 1000000) snprintf(ctxSizeStr, sizeof(ctxSizeStr), "%.0fM", CTX_SIZE/1e6); else snprintf(ctxSizeStr, sizeof(ctxSizeStr), "%.0fk", CTX_SIZE/1000.0);
    char ctxUsedStr[64]; if (CTX_TOTAL_IN > 0) snprintf(ctxUsedStr, sizeof(ctxUsedStr), "%s/%s", fmtTok(CTX_TOTAL_IN + CTX_TOTAL_OUT), ctxSizeStr); else snprintf(ctxUsedStr, sizeof(ctxUsedStr), "%s", fmtTok(CTX_TOKENS));
    { char t[256]; snprintf(t, sizeof(t), " " SEP " %s%s %d%%" R " " C_gray "(%s)" R, ctxC, ctxBar, PCT, ctxUsedStr); strcat(L1, t); }
    puts(L1); }

  // ── LINE 1b ──
  { long sTotal = sIn + sOut + sCw + sCr; char L1b[1024];
    snprintf(L1b, sizeof(L1b),
      C_gray "In:" R C_blue "%s" R " " SEP " " C_gray "Out:" R C_magenta "%s" R " " SEP " " C_gray "Write:" R C_orange "%s" R " " SEP " " C_gray "Read:" R C_teal "%s" R " " SEP " " C_gray "Total:" R C_white "%s" R,
      fmtTok(sIn), fmtTok(sOut), fmtTok(sCw), fmtTok(sCr), fmtTok(sTotal));
    if (*EFFORT) { char t[128]; snprintf(t, sizeof(t), " " SEP " " C_gray "effort:" R C_orange "%s" R, EFFORT); strcat(L1b, t); }
    if (THINKING) strcat(L1b, " " SEP " " C_cyan "\U0001f4ad thinking" R);
    puts(L1b); }

  // ── LINE 1c ──
  { long cbTotal = cbIn + cbCw + cbCr;
    if (cbTotal > 0) { char L1c[1024];
      snprintf(L1c, sizeof(L1c),
        C_gray "\U0001f9ee ctx:" R " " C_gray "Reused" R " " C_teal "%s" R " " SEP " " C_gray "+Cache" R " " C_orange "%s" R " " SEP " " C_gray "Fresh" R " " C_blue "%s" R " " SEP " " C_gray "Out" R " " C_magenta "%s" R,
        fmtTok(cbCr), fmtTok(cbCw), fmtTok(cbIn), fmtTok(cbOut));
      if (baseOverhead > 0) { long SYS_EST = 4600, TOOLS_EST = 16700; long mcpPlus = baseOverhead - SYS_EST - TOOLS_EST; if (mcpPlus < 0) mcpPlus = 0;
        char t[512]; snprintf(t, sizeof(t),
          " " SEP " " C_gray "base~" R C_yellow "%s" R " " C_gray "(sys~" R C_yellow "%s" R " " C_gray "· tools~" R C_yellow "%s" R " " C_gray "· mcp+~" R C_yellow "%s" R C_gray ")" R,
          fmtTok(baseOverhead), fmtTok(SYS_EST), fmtTok(TOOLS_EST), fmtTok(mcpPlus)); strcat(L1c, t); }
      puts(L1c); } }

  // ── LINE 2 ──
  { char L2[2048];
    if (RATE_5H_RESET) { char s[1024];
      const char *resetT = to12h((char*)({ static char hm[8]; time_t t = RATE_5H_RESET; struct tm *lt = localtime(&t); snprintf(hm, sizeof(hm), "%02d:%02d", lt->tm_hour, lt->tm_min); hm; }));
      long mins = (RATE_5H_RESET - NOW) / 60, hrs = mins / 60, mr = mins % 60;
      snprintf(s, sizeof(s), "⊙ 5H %s (%ldh %ldm) %d%%", resetT, hrs, mr, (int)(RATE_5H + 0.5));
      if (RATE_7D_RESET) { time_t t = RATE_7D_RESET; struct tm *lt = localtime(&t); char hm[8]; snprintf(hm, sizeof(hm), "%02d:%02d", lt->tm_hour, lt->tm_min);
        char t7[64]; snprintf(t7, sizeof(t7), "%s %s", WD[lt->tm_wday], to12h(hm));
        long m7 = (RATE_7D_RESET - NOW) / 60, d7 = m7 / 1440, h7 = (m7 % 1440) / 60;
        char a[256]; snprintf(a, sizeof(a), " • 7DAY %s (%ldd %ldh) %d%%", t7, d7, h7, (int)(RATE_7D + 0.5)); strcat(s, a);
      } else { char a[64]; snprintf(a, sizeof(a), " • 7DAY %d%%", (int)(RATE_7D + 0.5)); strcat(s, a); }
      snprintf(L2, sizeof(L2), C_yellow "%s" R, s);
    } else snprintf(L2, sizeof(L2), C_gray "No rate limit data" R);
    { char t[128]; snprintf(t, sizeof(t), " " SEP " " C_green "+%ld" R "/" C_red "-%ld" R, LINES_ADD, LINES_DEL); strcat(L2, t); }
    if (gitAhead || gitBehind) { char t[128]; snprintf(t, sizeof(t), " " SEP " " C_teal "↑%ld" R C_gray "/" R C_orange "↓%ld" R, gitAhead, gitBehind); strcat(L2, t); }
    { char t[128]; snprintf(t, sizeof(t), " " SEP " " C_gray "Commits:" R C_gold "%s" R, gitCommits); strcat(L2, t); }
    if (DURATION_MS) { int apiPct = DURATION_MS > 0 ? (int)(API_DUR_MS * 100 / DURATION_MS + 0.5) : 0;
      char t[256]; snprintf(t, sizeof(t), " " SEP " " C_gray "⏱ " R C_cyan "%s" R " " C_gray "(%d%% API)" R, fmtDur(DURATION_MS), apiPct); strcat(L2, t); }
    { char t[64]; snprintf(t, sizeof(t), " " SEP " " C_bold "\U0001f550 %s" R, clock_now()); strcat(L2, t); }
    puts(L2); }

  // ── LINE 3 ──
  { long denom = sIn + sCw + sCr; int cacheHit = denom > 0 ? (int)(sCr * 100.0 / denom + 0.5) : 0;
    char L3[1024];
    snprintf(L3, sizeof(L3),
      C_gray "REPO" R " " C_yellow "%s" R " " SEP " " C_gray "30D" R " " C_yellow "%s" R " " SEP " " C_gray "7D" R " " C_yellow "%s" R " " SEP " " C_gray "DAY" R " " C_yellow "%s" R,
      usd(cost_repo), usd(cost_month), usd(cost_week), usd(cost_day));
    { char t[256]; snprintf(t, sizeof(t), " " SEP " \U0001f525 " C_orange "LIVE %s" R, usd(LIVE_COST)); strcat(L3, t); }
    { char t[128]; snprintf(t, sizeof(t), " " SEP " %s%s/hr" R, burnC, usd(burn_cph)); strcat(L3, t); }
    { char t[128]; snprintf(t, sizeof(t), " " SEP " " C_gray "Cache hit:" R " " C_green "%d%%" R, cacheHit); strcat(L3, t); }
    puts(L3); }

  // ── Subagents ──
  if (subCount > 0) {
    // sort by cost desc
    for (int i = 0; i < nsubs; i++) for (int j = i + 1; j < nsubs; j++) if (subs[j].cost > subs[i].cost) { sub_t tmp = subs[i]; subs[i] = subs[j]; subs[j] = tmp; }
    char line[4096]; snprintf(line, sizeof(line), "\U0001f9e9 " C_gray "Subagents:" R C_cyan "%d" R " " C_gray "spawned" R, subCount);
    for (int i = 0; i < nsubs; i++) { char t[512]; snprintf(t, sizeof(t), " " SEP " " C_magenta "%s" R " " C_gray "×%d" R " " C_yellow "%s" R, subs[i].type, subs[i].count, usd(subs[i].cost)); strcat(line, t); }
    puts(line); }

  // ── LINE 4: hijri | date | model usage ──
  { // sort models by cost desc
    for (int i = 0; i < sx.nmodels; i++) for (int j = i + 1; j < sx.nmodels; j++) if (sx.models[j].cost > sx.models[i].cost) { mstat tmp = sx.models[i]; sx.models[i] = sx.models[j]; sx.models[j] = tmp; }
    double totalModelCost = 0; for (int i = 0; i < sx.nmodels; i++) totalModelCost += sx.models[i].cost; if (totalModelCost == 0) totalModelCost = 1;
    char L4[4096];
    if (hasPrayer) snprintf(L4, sizeof(L4), C_gray "%s %s %s" R " " SEP " " C_white "%s" R, hijriDay, hijriMonth, hijriYear, fullDate());
    else snprintf(L4, sizeof(L4), C_white "%s" R, fullDate());
    if (sx.nmodels > 0) {
      const char *mcolors[] = { C_orange, C_magenta, C_teal, C_blue };
      char ms[3072] = "\U0001f916 ";
      for (int i = 0; i < sx.nmodels; i++) { int pct = (int)(sx.models[i].cost * 100.0 / totalModelCost + 0.5); const char *col = mcolors[i % 4];
        char t[512]; snprintf(t, sizeof(t), "%s%s%s" R " " C_yellow "%s" R " " C_gray "(%d%%)" R " " C_teal "%s" R " " C_gray "×%ld" R, i ? SEP " " : "", col, shortModel(sx.models[i].id), usd(sx.models[i].cost), pct, fmtTok(sx.models[i].tokens), sx.models[i].count);
        strcat(ms, t); }
      char t[64]; snprintf(t, sizeof(t), " " SEP " "); strcat(L4, t); strcat(L4, ms);
    }
    puts(L4); }

  // ── LINE 5 ──
  { vec parts = {0}; char t[1024];
    if (PR_NUM) { const char *sc = !strcmp(PR_STATE,"approved")?C_green:!strcmp(PR_STATE,"changes_requested")?C_red:!strcmp(PR_STATE,"draft")?C_gray:C_yellow;
      if (*PR_STATE) snprintf(t, sizeof(t), "\U0001f500 " C_blue "PR #%d" R " %s· %s" R, PR_NUM, sc, PR_STATE);
      else snprintf(t, sizeof(t), "\U0001f500 " C_blue "PR #%d" R, PR_NUM); vec_push(&parts, t); }
    if (*WT_NAME) { char sm[256] = "";
      if (gitSubmodules > 0) snprintf(sm, sizeof(sm), " " SEP " %s⧉ %ld sub%s" R, gitSubDirty ? C_orange : C_teal, gitSubmodules, gitSubDirty ? " ⚠" : "");
      if (*WT_BRANCH) snprintf(t, sizeof(t), "\U0001f33f " C_teal "%s (%s)" R "%s", WT_NAME, WT_BRANCH, sm);
      else snprintf(t, sizeof(t), "\U0001f33f " C_teal "%s" R "%s", WT_NAME, sm); vec_push(&parts, t); }
    if (*REPO_OWNER && *REPO_NAME) { snprintf(t, sizeof(t), "\U0001f4e6 " C_gray "%s/%s" R, REPO_OWNER, REPO_NAME); vec_push(&parts, t); }
    if (*AGENT_NAME) { snprintf(t, sizeof(t), "\U0001f916 " C_magenta "agent:%s" R, AGENT_NAME); vec_push(&parts, t); }
    if (*OUTPUT_STYLE && strcmp(OUTPUT_STYLE, "default")) { snprintf(t, sizeof(t), "\U0001f3a8 " C_gray "%s" R, OUTPUT_STYLE); vec_push(&parts, t); }
    if (addedDirsN > 0) { snprintf(t, sizeof(t), "\U0001f4c1 " C_gray "+%d dirs" R, addedDirsN); vec_push(&parts, t); }
    if (*VIM_MODE) { snprintf(t, sizeof(t), C_gold "[%s]" R, VIM_MODE); vec_push(&parts, t); }
    if (parts.n) { char L5[8192] = ""; for (int i = 0; i < parts.n; i++) { if (i) strcat(L5, SEP); strcat(L5, parts.v[i]); } puts(L5); } }

  // ── LINE 6: prayers ──
  if (hasPrayer) {
    const char *names[] = {"Fajr","Dhuhr","Asr","Maghrib","Isha"};
    const char *times[] = {pFajr, pDhuhr, pAsr, pMaghrib, pIsha};
    long ep[5];
    for (int i = 0; i < 5; i++) { int h=0,m=0; sscanf(times[i], "%d:%d", &h, &m); time_t t = NOW; struct tm lt = *localtime(&t); lt.tm_hour=h; lt.tm_min=m; lt.tm_sec=0; ep[i] = (long)mktime(&lt); }
    int nextIdx = -1; for (int i = 0; i < 5; i++) if (ep[i] > NOW) { nextIdx = i; break; }
    char L6[2048] = "\U0001f54c ";
    for (int i = 0; i < 5; i++) { char seg[256]; const char *t12 = to12h(times[i]); long diff = ep[i] - NOW;
      if (ep[i] <= NOW) snprintf(seg, sizeof(seg), C_gray "%s %s ✓" R, names[i], t12);
      else if (i == nextIdx) { long dm = diff/60, dh = diff/3600, dr = (diff%3600)/60;
        if (dh > 0) snprintf(seg, sizeof(seg), C_yellow "%s %s (%ldh %ldm)" R, names[i], t12, dh, dr);
        else snprintf(seg, sizeof(seg), C_yellow "%s %s (%ldm)" R, names[i], t12, dm); }
      else snprintf(seg, sizeof(seg), C_cyan "%s %s" R, names[i], t12);
      if (i) strcat(L6, C_gray " | " R); strcat(L6, seg); }
    puts(L6); }

  // ── LINE 7: location | plugins | skills | mcp | version ──
  { char loc[600]; if (*locCity && *locCountry) snprintf(loc, sizeof(loc), "%s, %s", locCity, locCountry);
    else if (*locCity) snprintf(loc, sizeof(loc), "%s", locCity); else if (*locCountry) snprintf(loc, sizeof(loc), "%s", locCountry); else snprintf(loc, sizeof(loc), "Unknown");
    char L7[1024]; snprintf(L7, sizeof(L7), "\U0001f4cd " C_gray "%s" R " " SEP " " C_gray "Plugins:" R C_cyan "%d" R " " SEP " " C_gray "Skills:" R C_green "%d" R, loc, plugins.n, skillCount);
    if (mcp.n) { char t[128]; snprintf(t, sizeof(t), " " SEP " " C_gray "MCP:" R C_magenta "%d" R, mcp.n); strcat(L7, t); }
    if (*VERSION) { char t[128]; snprintf(t, sizeof(t), " " SEP " " C_gray "v%s" R, VERSION); strcat(L7, t); }
    puts(L7); }

  // ── plugin names (5/line) ──
  for (int i = 0; i < plugins.n; i += 5) { char line[2048] = "   ";
    for (int j = i; j < i + 5 && j < plugins.n; j++) { if (j > i) strcat(line, C_gray ", " R); char t[256]; snprintf(t, sizeof(t), C_teal "%s" R, plugins.v[j]); strcat(line, t); }
    puts(line); }

  // ── MCP names ──
  if (mcp.n) for (int i = 0; i < mcp.n; i += 5) { char line[2048]; snprintf(line, sizeof(line), "\U0001f50c %s", i == 0 ? "" : "   ");
    for (int j = i; j < i + 5 && j < mcp.n; j++) { if (j > i) strcat(line, C_gray ", " R); char t[256]; snprintf(t, sizeof(t), C_magenta "%s" R, mcp.v[j]); strcat(line, t); }
    puts(line); }

  return 0;
}
