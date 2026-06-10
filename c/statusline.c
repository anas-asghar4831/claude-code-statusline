// Claude Code Windows statusline — C port of statusline.js (max-perf build).
// Mirrors statusline.js output byte-for-byte. Deps: yyjson (JSON), uthash (dedup sets).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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

// ── profiler (STATUSLINE_PROF=1 → phase timings to stderr) ──
static int PROF_ON; static double PROF_T0, PROF_LAST;
#ifdef _WIN32
static double qpc_ms(void){ LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }
#else
#include <sys/time.h>
static double qpc_ms(void){ struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; }
#endif
static void PROF(const char *l){ if(!PROF_ON) return; double n = qpc_ms(); fprintf(stderr, "[prof] %-16s %7.2f ms  (cum %7.2f)\n", l, n - PROF_LAST, n - PROF_T0); PROF_LAST = n; }

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

// ── terminal width + ANSI-aware wrapping ──────────────────────────────────────
static int get_term_width(void) {
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
    return csbi.srWindow.Right - csbi.srWindow.Left + 1;
#else
  #include <sys/ioctl.h>
  struct winsize ws;
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
  return 120;
}
// visual column count for first n bytes (n=0 → full string); strips ANSI, emoji=2-wide
static int vis_len_n(const char *s, size_t n) {
  int w = 0; const char *end = n ? s + n : s + strlen(s);
  while (s < end && *s) {
    unsigned char c = (unsigned char)*s;
    if (c == 0x1b && s+1 < end && *(s+1) == '[') { s += 2; while (s < end && *s && *s != 'm') s++; if (s < end && *s) s++; }
    else if (c >= 0xF0) { w += 2; s += 4; }
    else if (c >= 0xE0) { w += 2; s += 3; }
    else if (c >= 0x80) { s++; }
    else { w++; s++; }
  }
  return w;
}
static int vis_len(const char *s) { return vis_len_n(s, 0); }
// puts() with SEP-boundary wrapping when line exceeds term_w
static void puts_wrapped(const char *line, int term_w) {
  static const char SEP_RAW[] = "\x1b[38;2;166;173;200m | \x1b[0m";
  static const size_t SEP_LEN = sizeof(SEP_RAW) - 1;
  if (term_w <= 40 || vis_len(line) <= term_w) { puts(line); return; }
  const char *p = line; int cur_w = 0;
  while (*p) {
    const char *sep = strstr(p, SEP_RAW);
    const char *chunk_end = sep ? sep + SEP_LEN : p + strlen(p);
    int chunk_w = vis_len_n(p, (size_t)(chunk_end - p));
    if (cur_w > 0 && cur_w + chunk_w > term_w) { printf("\n  "); cur_w = 2; }
    fwrite(p, 1, chunk_end - p, stdout); cur_w += chunk_w;
    p = chunk_end;
  }
  putchar('\n');
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

// ── async command: spawn all, then collect — runs subprocesses concurrently ──
typedef struct {
#ifdef _WIN32
  HANDLE proc, rd;
#else
  FILE *fp;
#endif
} acmd;
#ifdef _WIN32
static void ac_spawn(acmd *a, const char *cmd) {
  a->proc = NULL; a->rd = NULL;
  SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
  HANDLE rd, wr; if (!CreatePipe(&rd, &wr, &sa, 0)) return;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nin = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
  HANDLE nerr = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
  STARTUPINFOA si = { sizeof(si) }; si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr; si.hStdError = nerr; si.hStdInput = nin;
  PROCESS_INFORMATION pi; char *mut = strdup(cmd);
  BOOL ok = CreateProcessA(NULL, mut, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  free(mut); CloseHandle(wr);
  if (nin != INVALID_HANDLE_VALUE) CloseHandle(nin); if (nerr != INVALID_HANDLE_VALUE) CloseHandle(nerr);
  if (!ok) { CloseHandle(rd); return; }
  CloseHandle(pi.hThread); a->proc = pi.hProcess; a->rd = rd;
}
static char *ac_collect(acmd *a) {
  if (!a->rd) return NULL;
  size_t cap = 4096, len = 0; char *buf = malloc(cap); DWORD n;
  while (ReadFile(a->rd, buf + len, (DWORD)(cap - len), &n, NULL) && n > 0) { len += n; if (len == cap) { cap *= 2; buf = realloc(buf, cap); } }
  buf[len] = 0; CloseHandle(a->rd);
  if (a->proc) { WaitForSingleObject(a->proc, INFINITE); CloseHandle(a->proc); }
  rtrim(buf); return buf;
}
#else
static void ac_spawn(acmd *a, const char *cmd) { a->fp = popen(cmd, "r"); } // popen starts process immediately → concurrent until collected
static char *ac_collect(acmd *a) {
  if (!a->fp) return NULL;
  size_t cap = 4096, len = 0; char *buf = malloc(cap);
  size_t r; while ((r = fread(buf + len, 1, cap - len, a->fp)) > 0) { len += r; if (len == cap) { cap *= 2; buf = realloc(buf, cap); } }
  buf[len] = 0; pclose(a->fp); rtrim(buf); return buf;
}
#endif
#ifdef _WIN32
static int file_exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
static int is_dir(const char *path) { DWORD a = GetFileAttributesA(path); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
#else
static int file_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
static int is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && (st.st_mode & S_IFDIR); }
#endif
static long mtime_of(const char *path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_mtime : 0; }
// walk up from start looking for .git (dir or file) → repo root. No subprocess.
static int find_git_root(const char *start, char *out) {
  char cur[1024]; snprintf(cur, sizeof(cur), "%s", start);
  for (;;) {
    char dotgit[1100]; snprintf(dotgit, sizeof(dotgit), "%s/.git", cur);
    if (file_exists(dotgit)) { snprintf(out, 1024, "%s", cur); return 1; }
    char *slash = strrchr(cur, '/'); if (!slash) break;
    *slash = 0; if (!*cur) break; // reached root
  }
  return 0;
}
// ── git internals (read .git directly — zero subprocess) ─────────────────────
static uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
// resolve the real git dir for a repo root (handles .git file → "gitdir: <path>")
static void resolve_gitdir(const char *root, char *out, size_t outn) {
  char dg[1200]; snprintf(dg, sizeof(dg), "%s/.git", root);
  if (is_dir(dg)) { snprintf(out, outn, "%s", dg); return; }
  char *c = read_file(dg, NULL);
  if (c && !strncmp(c, "gitdir:", 7)) { char *p = c + 7; while (*p == ' ') p++; char *nl = strpbrk(p, "\r\n"); if (nl) *nl = 0;
    if (p[0] && (p[1] == ':' || p[0] == '/')) snprintf(out, outn, "%s", p);      // absolute
    else snprintf(out, outn, "%s/%s", root, p);                                   // relative
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/'; free(c); return; }
  free(c); snprintf(out, outn, "%s", dg);
}
// read a ref name → 40-char sha (loose ref file, else packed-refs). 1 ok.
static int resolve_ref(const char *gitdir, const char *refname, char out[64]) {
  char p[1300]; snprintf(p, sizeof(p), "%s/%s", gitdir, refname);
  char *c = read_file(p, NULL);
  if (c) { int ok = 0; if (strlen(c) >= 40) { memcpy(out, c, 40); out[40] = 0; ok = 1; } free(c); if (ok) return 1; }
  char pp[1300]; snprintf(pp, sizeof(pp), "%s/packed-refs", gitdir);
  char *pr = read_file(pp, NULL); if (!pr) return 0;
  int found = 0; char *save = NULL;
  for (char *line = strtok_r(pr, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    if (line[0] == '#' || line[0] == '^') continue;
    char *sp = strchr(line, ' '); if (!sp) continue;
    if (!strcmp(sp + 1, refname)) { *sp = 0; if (strlen(line) >= 40) { memcpy(out, line, 40); out[40] = 0; found = 1; } break; }
  }
  free(pr); return found;
}
// parse .git/config for [branch "<b>"] remote/merge → upstream ref name. 1 if found.
static int branch_upstream_ref(const char *gitdir, const char *branch, char out[300]) {
  char p[1300]; snprintf(p, sizeof(p), "%s/config", gitdir);
  char *c = read_file(p, NULL); if (!c) return 0;
  char want[300]; snprintf(want, sizeof(want), "[branch \"%s\"]", branch);
  char remote[128] = "", merge[256] = ""; int in = 0, found = 0; char *save = NULL;
  for (char *line = strtok_r(c, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char *s = line; while (*s == ' ' || *s == '\t') s++;
    if (*s == '[') { in = !strncmp(s, want, strlen(want)); continue; }
    if (!in) continue;
    char *eq = strchr(s, '='); if (!eq) continue; char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
    char *end = v + strlen(v); while (end > v && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
    if (!strncmp(s, "remote", 6)) snprintf(remote, sizeof(remote), "%s", v);
    else if (!strncmp(s, "merge", 5)) snprintf(merge, sizeof(merge), "%s", v);
  }
  free(c);
  if (*remote && !strncmp(merge, "refs/heads/", 11)) { snprintf(out, 300, "refs/remotes/%s/%s", remote, merge + 11); found = 1; }
  return found;
}
// dirty check by parsing .git/index (v2/v3) + stat per entry. early-exit.
// returns 1 dirty, 0 clean, -1 unknown (caller should fall back to `git status`).
static int index_dirty(const char *root, const char *gitdir) {
  char ip[1200]; snprintf(ip, sizeof(ip), "%s/index", gitdir);
  long n = 0; char *raw = read_file(ip, &n);
  if (!raw || n < 12) { free(raw); return -1; }
  unsigned char *b = (unsigned char *)raw;
  if (memcmp(b, "DIRC", 4)) { free(raw); return -1; }
  uint32_t ver = be32(b + 4), cnt = be32(b + 8);
  if (ver < 2 || ver > 3) { free(raw); return -1; }   // v4 path-compression → fall back
  size_t off = 12; int dirty = 0;
  char full[4300];
  for (uint32_t i = 0; i < cnt; i++) {
    if (off + 62 > (size_t)n) { dirty = -1; break; }
    unsigned char *e = b + off;
    uint32_t mtime_s = be32(e + 8), fsize = be32(e + 36);
    uint16_t flags = ((uint16_t)e[60] << 8) | e[61];
    int assume_valid = flags & 0x8000, extended = flags & 0x4000, namelen = flags & 0x0FFF;
    size_t hdr = 62; if (ver >= 3 && extended) hdr += 2;
    const char *namep = (const char *)(b + off + hdr);
    int L = namelen;
    if (namelen == 0x0FFF) { L = (int)strnlen(namep, (size_t)(n - (off + hdr))); }
    if (off + hdr + L + 1 > (size_t)n) { dirty = -1; break; }
    if (!assume_valid) {
      if (L < (int)sizeof(full) - (int)strlen(root) - 2) {
        snprintf(full, sizeof(full), "%s/%.*s", root, L, namep);
        struct stat st;
        if (stat(full, &st) != 0) { dirty = 1; break; }
        if ((uint32_t)st.st_size != fsize || (uint32_t)st.st_mtime != mtime_s) { dirty = 1; break; }
      }
    }
    size_t elen = hdr + L + 1; elen = (elen + 7) & ~(size_t)7;   // 8-byte padded (v2/3)
    off += elen;
  }
  free(raw);
  return dirty;
}

// ── fast directory iterator — Win32 FindFirstFile yields name+isdir+mtime inline
//    (no per-entry stat/GetFileAttributes), POSIX falls back to readdir+stat. ──
typedef struct {
#ifdef _WIN32
  HANDLE h; WIN32_FIND_DATAA fd; int first, valid;
#else
  DIR *d; char base[2048];
#endif
  long _sz; // size of last entry returned by di_next (inline from FindFirstFile/stat)
} dirit;
static long di_size(dirit *it) { return it->_sz; }
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
    it->_sz = (long)(((uint64_t)it->fd.nFileSizeHigh << 32) | it->fd.nFileSizeLow);
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
    *name = e->d_name; *isdir = (st.st_mode & S_IFDIR) ? 1 : 0; it->_sz = (long)st.st_size; if (mtime) *mtime = (long)st.st_mtime;
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
// atomic write: temp (pid-tagged) → rename over target. Safe across concurrent sessions.
static void write_file_atomic(const char *name, const void *data, size_t len) {
  unsigned pid =
#ifdef _WIN32
    (unsigned)GetCurrentProcessId();
#else
    (unsigned)getpid();
#endif
  char tmp[1300], dst[1200];
  snprintf(dst, sizeof(dst), "%s/%s", SL_DIR, name);
  snprintf(tmp, sizeof(tmp), "%s/%s.tmp.%u", SL_DIR, name, pid);
  FILE *f = fopen(tmp, "wb"); if (!f) return;
  size_t w = fwrite(data, 1, len, f); fclose(f);
  if (w != len) { remove(tmp); return; }
#ifdef _WIN32
  if (!MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING)) remove(tmp);
#else
  if (rename(tmp, dst) != 0) remove(tmp);
#endif
}
static void write_cache_raw(const char *name, const char *json) { write_file_atomic(name, json, strlen(json)); }

// ═══ incremental cost cache: binary format, mmap read, threaded build, atomic write ═══
// Re-parse only changed JSONL files; reuse cached parsed rows for the rest. Rows store raw
// token counts (pricing applied at aggregation, so model switches stay correct) + the ISO
// timestamp (so the rolling day/week/month windows recompute exactly). Global dedup by a
// 64-bit hash of the request id preserves the ~52% cross-file duplicate removal.
typedef struct { uint64_t idh; uint32_t in, out, cw, cr; char ts[20]; } crow;
typedef struct { char path[600]; long mt, sz; crow *rows; uint32_t nrows; int isRepo, owned; } cfile;

static uint64_t fnv1a(const char *s) { uint64_t h = 0xcbf29ce484222325ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 0x100000001b3ULL; } return h; }

// open-address uint64 set (idh==0 means "no id" → never deduped)
typedef struct { uint64_t *k; size_t cap, n; } u64set;
static void u64init(u64set *s, size_t hint) { size_t c = 16; while (c < hint * 2) c <<= 1; s->k = calloc(c, 8); s->cap = c; s->n = 0; }
static int u64add(u64set *s, uint64_t key) {
  if (key == 0) return 1;
  if ((s->n + 1) * 4 >= s->cap * 3) { size_t nc = s->cap << 1; uint64_t *nk = calloc(nc, 8);
    for (size_t i = 0; i < s->cap; i++) { uint64_t v = s->k[i]; if (v) { size_t j = v & (nc - 1); while (nk[j]) j = (j + 1) & (nc - 1); nk[j] = v; } }
    free(s->k); s->k = nk; s->cap = nc; }
  size_t i = key & (s->cap - 1); while (s->k[i]) { if (s->k[i] == key) return 0; i = (i + 1) & (s->cap - 1); } s->k[i] = key; s->n++; return 1;
}

// parse one JSONL file → malloc'd crow[] (deduped within file). caller frees.
static crow *parse_cost_file(const char *path, uint32_t *outn) {
  *outn = 0; char *buf = read_file(path, NULL); if (!buf) return NULL;
  crow *rows = NULL; size_t cap = 0, cnt = 0; u64set local; u64init(&local, 128);
  char *save = NULL;
  for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    if (!strstr(line, "\"assistant\"")) continue;
    yyjson_doc *d = yyjson_read(line, strlen(line), 0); if (!d) continue;
    yyjson_val *o = yyjson_doc_get_root(d);
    const char *type = yyjson_get_str(yyjson_obj_get(o, "type"));
    if (!type || strcmp(type, "assistant")) { yyjson_doc_free(d); continue; }
    yyjson_val *msg = yyjson_obj_get(o, "message");
    yyjson_val *u = msg ? yyjson_obj_get(msg, "usage") : NULL;
    if (!u) { yyjson_doc_free(d); continue; }
    const char *id = msg ? yyjson_get_str(yyjson_obj_get(msg, "id")) : NULL;
    if (!id) id = yyjson_get_str(yyjson_obj_get(o, "requestId"));
    if (!id) id = yyjson_get_str(yyjson_obj_get(o, "uuid"));
    uint64_t idh = (id && *id) ? fnv1a(id) : 0;
    if (!u64add(&local, idh)) { yyjson_doc_free(d); continue; }
    if (cnt == cap) { cap = cap ? cap * 2 : 128; rows = realloc(rows, cap * sizeof(crow)); }
    crow *r = &rows[cnt++];
    r->idh = idh;
    r->in = (uint32_t)yyjson_get_num(yyjson_obj_get(u, "input_tokens"));
    r->out = (uint32_t)yyjson_get_num(yyjson_obj_get(u, "output_tokens"));
    r->cw = (uint32_t)yyjson_get_num(yyjson_obj_get(u, "cache_creation_input_tokens"));
    r->cr = (uint32_t)yyjson_get_num(yyjson_obj_get(u, "cache_read_input_tokens"));
    const char *ts = yyjson_get_str(yyjson_obj_get(o, "timestamp"));
    r->ts[0] = 0; if (ts) { int k = 0; for (; ts[k] && k < 19; k++) { if (ts[k] == '.' || ts[k] == 'Z') break; r->ts[k] = ts[k]; } r->ts[k] = 0; }
    yyjson_doc_free(d);
  }
  free(buf); free(local.k); *outn = (uint32_t)cnt; return rows;
}

// cached-file lookup (path → mt/sz/rows-pointer-into-mmap)
typedef struct { char *path; long mt, sz; const unsigned char *rows; uint32_t nrows; UT_hash_handle hh; } centry;

// mmap a file read-only (leaked until process exit). returns base + len.
static const unsigned char *map_ro(const char *path, size_t *len) {
#ifdef _WIN32
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER sz; if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0) { CloseHandle(f); return NULL; }
  HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL); if (!m) { CloseHandle(f); return NULL; }
  void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
  *len = (size_t)sz.QuadPart; return (const unsigned char *)p; // handles leaked (short-lived)
#else
  long n; char *b = read_file(path, &n); *len = b ? (size_t)n : 0; return (const unsigned char *)b;
#endif
}

// threaded parse of the cache-miss files
typedef struct { cfile *files; int *idx; int start, end; } pjob;
#ifdef _WIN32
static DWORD WINAPI parse_worker(LPVOID vp) { pjob *j = vp; for (int i = j->start; i < j->end; i++) { cfile *cf = &j->files[j->idx[i]]; cf->rows = parse_cost_file(cf->path, &cf->nrows); cf->owned = 1; } return 0; }
#endif

// the whole incremental cost computation. writes cost-index.bin atomically.
static void compute_costs(const char *CWD, const char *todayISO, const char *weekISO, const char *monthISO,
                          double *o_repo, double *o_day, double *o_week, double *o_month) {
  char repoRoot[1024]; snprintf(repoRoot, sizeof(repoRoot), "%s", CWD);
  char *wt = strstr(repoRoot, "/.worktrees/"); if (wt) *wt = 0;
  char repoSlug[1024]; { int k = 0; for (const char *s = repoRoot; *s && k < 1023; s++) repoSlug[k++] = (*s==':'||*s=='/'||*s=='\\') ? '-' : *s; repoSlug[k] = 0; }
  char wtpfx[1100]; snprintf(wtpfx, sizeof(wtpfx), "%s--worktrees-", repoSlug);

  // 1) load existing binary cache into a path→entry map (rows point into mmap)
  centry *cmap = NULL; char idxpath[1200]; snprintf(idxpath, sizeof(idxpath), "%s/cost-index.bin", SL_DIR);
  size_t mlen = 0; const unsigned char *mp = map_ro(idxpath, &mlen);
  if (mp && mlen > 8 && !memcmp(mp, "CIB2", 4)) {
    size_t off = 4; uint32_t nf; memcpy(&nf, mp + off, 4); off += 4;
    for (uint32_t i = 0; i < nf && off + 22 <= mlen; i++) {
      uint16_t pl; memcpy(&pl, mp + off, 2); off += 2;
      if (off + pl + 20 > mlen) break;
      centry *e = calloc(1, sizeof(centry)); e->path = malloc(pl + 1); memcpy(e->path, mp + off, pl); e->path[pl] = 0; off += pl;
      memcpy(&e->mt, mp + off, 8); off += 8; memcpy(&e->sz, mp + off, 8); off += 8;
      memcpy(&e->nrows, mp + off, 4); off += 4;
      e->rows = mp + off; off += (size_t)e->nrows * sizeof(crow);
      if (off > mlen) { free(e->path); free(e); break; }
      HASH_ADD_KEYPTR(hh, cmap, e->path, strlen(e->path), e);
    }
  }

  // 2) enumerate relevant files (recurse projects; relevant = repo-file OR mtime<31d)
  double cutoff31 = (double)NOW - 31.0 * 86400.0;
  cfile *files = NULL; int nfiles = 0, fcap = 0;
  dirit top;
  if (di_open(&top, PROJECTS_DIR)) {
    const char *pn; int pdir; long pmt;
    while (di_next(&top, &pn, &pdir, &pmt)) {
      if (!pdir) continue;
      int isRepo = (!strcmp(pn, repoSlug) || !strncmp(pn, wtpfx, strlen(wtpfx)));
      // recurse this project dir (BFS) collecting .jsonl
      char projbase[1200]; snprintf(projbase, sizeof(projbase), "%s/%s", PROJECTS_DIR, pn);
      char (*stack)[1100] = malloc(2048 * sizeof(*stack)); int sp = 0;
      snprintf(stack[sp++], 1100, "%s", projbase);
      while (sp > 0) {
        char cur[1100]; snprintf(cur, sizeof(cur), "%s", stack[--sp]);
        dirit d; if (!di_open(&d, cur)) continue;
        const char *en; int eisdir; long emt;
        while (di_next(&d, &en, &eisdir, &emt)) {
          char full[1700]; snprintf(full, sizeof(full), "%s/%s", cur, en);
          if (eisdir) { if (sp < 2048) snprintf(stack[sp++], 1100, "%s", full); continue; }
          size_t ln = strlen(en); if (ln < 6 || strcmp(en + ln - 6, ".jsonl")) continue;
          if (!isRepo && (double)emt < cutoff31) continue;          // not relevant
          long esz = di_size(&d);
          if (nfiles == fcap) { fcap = fcap ? fcap * 2 : 256; files = realloc(files, fcap * sizeof(cfile)); }
          cfile *cf = &files[nfiles++]; snprintf(cf->path, sizeof(cf->path), "%s", full);
          cf->mt = emt; cf->sz = esz; cf->rows = NULL; cf->nrows = 0; cf->isRepo = isRepo; cf->owned = 0;
        }
        di_close(&d);
      }
      free(stack);
    }
    di_close(&top);
  }

  // 3) resolve each file: cache hit (reuse mmap rows) or mark for parse
  int *toParse = malloc((nfiles ? nfiles : 1) * sizeof(int)); int nParse = 0;
  for (int i = 0; i < nfiles; i++) {
    centry *e; HASH_FIND_STR(cmap, files[i].path, e);
    if (e && e->mt == files[i].mt && e->sz == files[i].sz) { files[i].rows = (crow *)e->rows; files[i].nrows = e->nrows; files[i].owned = 0; }
    else toParse[nParse++] = i;
  }

  // 4) parse the misses — multithreaded when many (first build), inline when few
  int NT = 1;
#ifdef _WIN32
  { SYSTEM_INFO si; GetSystemInfo(&si); NT = (int)si.dwNumberOfProcessors; }
#endif
  if (NT > 16) NT = 16; if (NT < 1) NT = 1;
  if (nParse >= 8 && NT > 1) {
#ifdef _WIN32
    HANDLE th[16]; pjob jobs[16]; int per = (nParse + NT - 1) / NT, t = 0;
    for (int s = 0; s < nParse; s += per) { jobs[t].files = files; jobs[t].idx = toParse; jobs[t].start = s; jobs[t].end = (s + per < nParse) ? s + per : nParse;
      th[t] = CreateThread(NULL, 0, parse_worker, &jobs[t], 0, NULL); t++; }
    WaitForMultipleObjects(t, th, TRUE, INFINITE); for (int i = 0; i < t; i++) CloseHandle(th[i]);
#endif
  } else {
    for (int i = 0; i < nParse; i++) { cfile *cf = &files[toParse[i]]; cf->rows = parse_cost_file(cf->path, &cf->nrows); cf->owned = 1; }
  }
  free(toParse);

  // 5) aggregate: global dedup for day/week/month; separate repo dedup
  u64set gseen, rseen; size_t est = 0; for (int i = 0; i < nfiles; i++) est += files[i].nrows;
  u64init(&gseen, est ? est : 16); u64init(&rseen, est ? est : 16);
  double repo = 0, day = 0, week = 0, month = 0;
  for (int i = 0; i < nfiles; i++) {
    cfile *cf = &files[i];
    for (uint32_t j = 0; j < cf->nrows; j++) {
      crow *r = &cf->rows[j];
      double c = (r->in * P[0] + r->out * P[1] + r->cw * P[2] + r->cr * P[3]) / 1e6;
      if (u64add(&gseen, r->idh)) {
        if (strcmp(r->ts, monthISO) >= 0) month += c;
        if (strcmp(r->ts, weekISO)  >= 0) week  += c;
        if (strcmp(r->ts, todayISO) >= 0) day   += c;
      }
      if (cf->isRepo && u64add(&rseen, r->idh)) repo += c;
    }
  }
  *o_repo = repo; *o_day = day; *o_week = week; *o_month = month;

  // 6) write the new binary cache (all current relevant files) atomically.
  //    Skip the 1.5MB write entirely when nothing was re-parsed (idle refresh) — the
  //    existing index is already current; a stale leftover entry is just ignored.
  if (nParse == 0) return;
  size_t total = 8; for (int i = 0; i < nfiles; i++) total += 2 + strlen(files[i].path) + 20 + (size_t)files[i].nrows * sizeof(crow);
  unsigned char *out = malloc(total); size_t off = 0;
  memcpy(out + off, "CIB2", 4); off += 4; uint32_t nf = (uint32_t)nfiles; memcpy(out + off, &nf, 4); off += 4;
  for (int i = 0; i < nfiles; i++) {
    uint16_t pl = (uint16_t)strlen(files[i].path); memcpy(out + off, &pl, 2); off += 2;
    memcpy(out + off, files[i].path, pl); off += pl;
    memcpy(out + off, &files[i].mt, 8); off += 8; memcpy(out + off, &files[i].sz, 8); off += 8;
    memcpy(out + off, &files[i].nrows, 4); off += 4;
    if (files[i].nrows) { memcpy(out + off, files[i].rows, (size_t)files[i].nrows * sizeof(crow)); off += (size_t)files[i].nrows * sizeof(crow); }
  }
  write_file_atomic("cost-index.bin", out, off);
  free(out);
  // (memory from files/rows/cmap left for process exit — short-lived)
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
  PROF_ON = getenv("STATUSLINE_PROF") != NULL; PROF_T0 = PROF_LAST = qpc_ms();
  NOW = (long)time(NULL);
  const char *up = getenv("USERPROFILE"); if (!up) up = getenv("HOME"); if (!up) up = ".";
  snprintf(HOME, sizeof(HOME), "%s", up); slashfix(HOME);
  snprintf(SL_DIR, sizeof(SL_DIR), "%s/.claude/statusline", HOME);
  snprintf(PROJECTS_DIR, sizeof(PROJECTS_DIR), "%s/.claude/projects", HOME);

  char *raw = read_stdin();
  J = yyjson_read(raw, strlen(raw), 0);
  PROF("stdin+parse");

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
    // incremental: re-parse only changed JSONL files, reuse cached rows for the rest
    compute_costs(CWD, todayISO, weekISO, monthISO, &cost_repo, &cost_day, &cost_week, &cost_month);
    char out[512]; snprintf(out, sizeof(out),
      "{\"d\":{\"repo\":%.10g,\"month\":%.10g,\"week\":%.10g,\"day\":%.10g},\"ts\":%ld}",
      cost_repo, cost_month, cost_week, cost_day, NOW);
    write_cache_raw("cost-cache.json", out);
  }
  if (cc) yyjson_doc_free(cc);
  PROF("costs");

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

  PROF("burn");
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
  PROF("session");

  // ── git (ZERO subprocess on the common path) ──
  // branch ← .git/HEAD; dirty ← parse .git/index + stat (gitstatusd technique);
  // commit-count + ahead/behind ← cached by HEAD/upstream sha (git-cache.json), so a
  // subprocess runs ONLY right after HEAD or upstream moves (commit/fetch/checkout).
  char gitBranch[256] = "", gitCommits[64] = "0"; const char *gitIcon = "✅";
  long gitAhead = 0, gitBehind = 0, gitSubmodules = 0; int gitSubDirty = 0; int gitHaveAB = 0;
  {
    char gitroot[1024];
    if (find_git_root(CWD, gitroot)) {
      char gitdir[1200]; resolve_gitdir(gitroot, gitdir, sizeof(gitdir));
      // HEAD → branch + sha (no spawn)
      char headsha[64] = ""; char hp[1300]; snprintf(hp, sizeof(hp), "%s/HEAD", gitdir);
      char *head = read_file(hp, NULL);
      if (head) {
        if (!strncmp(head, "ref:", 4)) { char *r = head + 4; while (*r == ' ') r++; char *nl = strpbrk(r, "\r\n"); if (nl) *nl = 0;
          snprintf(gitBranch, sizeof(gitBranch), "%s", !strncmp(r, "refs/heads/", 11) ? r + 11 : r);
          resolve_ref(gitdir, r, headsha);
        } else { char *nl = strpbrk(head, "\r\n"); if (nl) *nl = 0; if (strlen(head) >= 40) { memcpy(headsha, head, 40); headsha[40] = 0; } }
        free(head);
      }
      if (*gitBranch) {
        // dirty: (a) in-process index scan catches unstaged edits/deletes INSTANTLY (0 spawn).
        //   (b) staged + untracked need authoritative `git status`, but its result is cached
        //   keyed by .git/index mtime — re-runs only when the index changes (add/commit/
        //   checkout), so steady-state renders stay spawn-free. Also the fallback for a v4
        //   index that (a) can't parse. Caveat: an untracked file created with no other index
        //   change won't flip the icon until the next index write or any unstaged edit.
        int dirty = 0, d = index_dirty(gitroot, gitdir);
        if (d == 1) dirty = 1;
        else {
          char ixp[1300]; snprintf(ixp, sizeof(ixp), "%s/index", gitdir); long idxmt = mtime_of(ixp);
          int cached = -1;
          yyjson_doc *sc = read_cache_copy("status-cache.json");
          if (sc) { yyjson_val *r = yyjson_doc_get_root(sc); const char *sr = yyjson_get_str(yyjson_obj_get(r, "root"));
            long im = (long)yyjson_get_num(yyjson_obj_get(r, "im"));
            if (sr && !strcmp(sr, gitroot) && im == idxmt) cached = (int)yyjson_get_num(yyjson_obj_get(r, "dirty"));
            yyjson_doc_free(sc); }
          if (cached < 0) { char cmd[1400]; snprintf(cmd, sizeof(cmd), "git --no-optional-locks -C \"%s\" status --porcelain %s", CWD, NULDEV);
            char *st = run_cmd(cmd); cached = (st && *st) ? 1 : 0; free(st);
            char out[1200]; snprintf(out, sizeof(out), "{\"root\":\"%s\",\"im\":%ld,\"dirty\":%d}", gitroot, idxmt, cached); write_cache_raw("status-cache.json", out); }
          dirty = cached;
        }
        if (dirty) gitIcon = "⚠️";
        // upstream sha (no spawn)
        char upref[300] = "", upsha[64] = ""; int haveUp = branch_upstream_ref(gitdir, gitBranch, upref) && resolve_ref(gitdir, upref, upsha);
        // commit count + ahead/behind cached by sha
        long count = -1; int abA = 0, abB = 0, abOK = 0;
        yyjson_doc *gc = read_cache_copy("git-cache.json");
        if (gc) { yyjson_val *r = yyjson_doc_get_root(gc);
          const char *ch = yyjson_get_str(yyjson_obj_get(r, "head"));
          if (ch && headsha[0] && !strcmp(ch, headsha)) {
            count = (long)yyjson_get_num(yyjson_obj_get(r, "count"));
            const char *cu = yyjson_get_str(yyjson_obj_get(r, "up"));
            if (haveUp && cu && !strcmp(cu, upsha)) { abA = (int)yyjson_get_num(yyjson_obj_get(r, "a")); abB = (int)yyjson_get_num(yyjson_obj_get(r, "b")); abOK = 1; }
          }
          yyjson_doc_free(gc);
        }
        if (count < 0) { char cmd[1400]; snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-list --count HEAD %s", CWD, NULDEV);
          char *c = run_cmd(cmd); count = (c && *c) ? atol(c) : 0; free(c); }
        if (haveUp && !abOK) { char cmd[1400]; snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-list --count --left-right HEAD...@{u} %s", CWD, NULDEV);
          char *c = run_cmd(cmd); if (c && *c) { long a = 0, bb = 0; sscanf(c, "%ld\t%ld", &a, &bb); abA = (int)a; abB = (int)bb; } free(c); abOK = 1; }
        snprintf(gitCommits, sizeof(gitCommits), "%ld", count);
        gitAhead = abA; gitBehind = abB; gitHaveAB = abOK;
        { char out[256]; snprintf(out, sizeof(out), "{\"head\":\"%s\",\"count\":%ld,\"up\":\"%s\",\"a\":%d,\"b\":%d}", headsha, count, haveUp ? upsha : "", abA, abB); write_cache_raw("git-cache.json", out); }
        // submodules (rare; only when .gitmodules present)
        char gm[1200]; snprintf(gm, sizeof(gm), "%s/.gitmodules", gitroot);
        char *gmc = read_file(gm, NULL);
        if (gmc) { char *p = gmc; if (!strncmp(p, "[submodule ", 11)) gitSubmodules++;
          while ((p = strstr(p, "\n[submodule "))) { gitSubmodules++; p += 1; } free(gmc); }
        if (gitSubmodules > 0) { char cmd[1400]; snprintf(cmd, sizeof(cmd), "git -C \"%s\" submodule status %s", CWD, NULDEV);
          char *ss = run_cmd(cmd); if (ss) { for (char *q = ss; *q; ) { if (*q=='+'||*q=='-'||*q=='U') { gitSubDirty = 1; break; } char *nl = strchr(q, '\n'); if (!nl) break; q = nl + 1; } free(ss); } }
      }
    }
  }
  (void)gitHaveAB;

  PROF("git");
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

  PROF("launch+loc+pray");
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

  // skills count — cached, keyed by plugins/cache dir mtime (the 531-dir walk is the
  // single most expensive non-git step; invalidate when a plugin is added/removed).
  int skillCount = -1;
  char cacheDir[1200]; snprintf(cacheDir, sizeof(cacheDir), "%s/.claude/plugins/cache", HOME);
  long pcMtime = mtime_of(cacheDir);
  yyjson_doc *skc = read_cache_copy("skill-cache.json");
  if (skc) { yyjson_val *r = yyjson_doc_get_root(skc);
    long dmt = (long)yyjson_get_num(yyjson_obj_get(r, "dmt"));
    if (dmt == pcMtime) skillCount = (int)yyjson_get_num(yyjson_obj_get(r, "count"));
    yyjson_doc_free(skc); }
  if (skillCount < 0) {
    sitem *skills = NULL;
    char sdir[1200]; snprintf(sdir, sizeof(sdir), "%s/.claude/skills", HOME);
    dirit d; const char *nm; int isdir;
    if (di_open(&d, sdir)) { while (di_next(&d, &nm, &isdir, NULL)) { if (!strcmp(nm,".orphaned_at")||!strcmp(nm,"CLAUDE.md")) continue; set_add(&skills, nm); } di_close(&d); }
    typedef struct { char path[1600]; int depth; } qn;
    qn *stack = malloc(4096 * sizeof(qn)); int sp = 0;
    snprintf(stack[sp].path, sizeof(stack[sp].path), "%s", cacheDir); stack[sp].depth = 0; sp++;
    while (sp > 0) { qn cur = stack[--sp]; if (cur.depth > 4) continue;
      dirit dd; if (!di_open(&dd, cur.path)) continue;
      const char *en; int eisdir;
      while (di_next(&dd, &en, &eisdir, NULL)) { if (!eisdir) continue;
        char full[1700]; snprintf(full, sizeof(full), "%s/%s", cur.path, en);
        if (!strcmp(en, "skills")) { dirit sd; const char *sn; int sis; if (di_open(&sd, full)) { while (di_next(&sd, &sn, &sis, NULL)) { if (!strcmp(sn,".orphaned_at")||!strcmp(sn,"CLAUDE.md")) continue; set_add(&skills, sn); } di_close(&sd); } }
        else if (sp < 4096) { snprintf(stack[sp].path, sizeof(stack[sp].path), "%s", full); stack[sp].depth = cur.depth + 1; sp++; } }
      di_close(&dd); }
    skillCount = HASH_COUNT(skills); set_free(&skills); free(stack);
    char out[128]; snprintf(out, sizeof(out), "{\"dmt\":%ld,\"count\":%d}", pcMtime, skillCount);
    write_cache_raw("skill-cache.json", out);
  }
  PROF("plugins+skills");

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

  PROF("mcp");
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

  PROF("subagents");
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
  { int tw = get_term_width();
    char L1[4096] = "";
    strcat(L1, dirDisplay);
    if (*gitBranch) { char t[320]; snprintf(t, sizeof(t), " " C_teal "(%s)" R, gitBranch); strcat(L1, t); }
    { char t[64]; snprintf(t, sizeof(t), " %s", gitIcon); strcat(L1, t); }
    { char t[256]; snprintf(t, sizeof(t), " " SEP " \U0001f9e0 " C_magenta "%s" R, MODEL); strcat(L1, t); }
    if (FAST_MODE) strcat(L1, " " SEP " " C_yellow "⚡fast" R);
    if (EXCEEDS_200K) strcat(L1, " " SEP " " C_red C_bold "⚠ >200K" R);
    char ctxSizeStr[32]; if (CTX_SIZE >= 1000000) snprintf(ctxSizeStr, sizeof(ctxSizeStr), "%.0fM", CTX_SIZE/1e6); else snprintf(ctxSizeStr, sizeof(ctxSizeStr), "%.0fk", CTX_SIZE/1000.0);
    char ctxUsedStr[64]; if (CTX_TOTAL_IN > 0) snprintf(ctxUsedStr, sizeof(ctxUsedStr), "%s/%s", fmtTok(CTX_TOTAL_IN + CTX_TOTAL_OUT), ctxSizeStr); else snprintf(ctxUsedStr, sizeof(ctxUsedStr), "%s", fmtTok(CTX_TOKENS));
    { char t[256]; snprintf(t, sizeof(t), " " SEP " %s%s %d%%" R " " C_gray "(%s)" R, ctxC, ctxBar, PCT, ctxUsedStr); strcat(L1, t); }
    puts_wrapped(L1, tw);
    // ── LINE 1-title: session name + id (only when present) ──
    if (*SESSION_NAME || *SESSION_ID) {
      char Ltitle[768] = "";
      if (*SESSION_NAME) { char t[512]; snprintf(t, sizeof(t), C_pink C_bold "%s" R, SESSION_NAME); strcat(Ltitle, t); }
      if (*SESSION_ID) {
        int has_name = *SESSION_NAME != 0;
        char t[256]; snprintf(t, sizeof(t), "%s" C_gray "id:" R C_gray "%s" R, has_name ? "  " SEP "  " : "", SESSION_ID);
        strcat(Ltitle, t);
      }
      puts(Ltitle);
    }
  }

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

  PROF("output");
  return 0;
}
