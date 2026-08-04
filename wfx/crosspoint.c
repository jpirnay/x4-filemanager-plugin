// CrossPoint USB serial file-system plugin for Total Commander / Double
// Commander (WFX). Browses and transfers files on the e-reader's SD card over
// the USB cable using the serial protocol (see cp_serial.c).
//
// Exports both the ANSI (Fs*) and wide-char (Fs*W) entry points. Commanders
// prefer the wide interface (UTF-16) for full Unicode filename support; the core
// works in UTF-8 and the wide functions convert at the boundary.
//
// Open the device's "File Transfer -> USB Transfer" screen first, then open the
// "CrossPoint USB" file system in the commander.
//
// Protocol/idea: CidVonHighwind/MicroReader (https://github.com/CidVonHighwind/microreader).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "../protocol/cp_serial.h"
#include "wfxplugin.h"

// --- serialization ------------------------------------------------------
// Total Commander can invoke WFX entry points from more than one thread at a
// time (e.g. its background transfer manager, or a directory refresh
// overlapping an in-flight download). g_conn is a single serial connection
// shared by every operation; without a lock, two concurrent commands can
// interleave on the wire — one call's port_flush_in() can discard the bytes
// of another call's in-flight response (observed as "no READY received"
// right after the device reports it sent the file). Double Commander appears
// to serialize these calls itself, which is why the bug is TC-specific.
#ifdef _WIN32
static CRITICAL_SECTION g_lock;
static LONG g_lock_init_flag = 0;  // 0 = not started, 1 = started/done
static void lock_init(void) {
  if (InterlockedCompareExchange(&g_lock_init_flag, 1, 0) == 0) InitializeCriticalSection(&g_lock);
}
static void lock_acquire(void) { EnterCriticalSection(&g_lock); }
static void lock_release(void) { LeaveCriticalSection(&g_lock); }
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_init(void) {}
static void lock_acquire(void) { pthread_mutex_lock(&g_lock); }
static void lock_release(void) { pthread_mutex_unlock(&g_lock); }
#endif

// --- callbacks / state ------------------------------------------------------
static int g_plugin_nr = 0;
static int g_unicode = 0;  // set when the host initialised us via FsInitW
static tProgressProc g_progA = NULL;
static tLogProc g_logA = NULL;
static tProgressProcW g_progW = NULL;
static tLogProcW g_logW = NULL;
static CpSerial* g_conn = NULL;
static int g_disconnected = 0;      // set by FsDisconnect; cleared when user re-enters root
static char g_ini_port[128] = {0};  // optional port override from the plugin ini

// --- UTF-8 <-> UTF-16 (portable; WCHAR is 2-byte on Windows and DC/Linux) ----
static size_t u8_to_u16(const char* s, WCHAR* out, size_t cap) {
  size_t o = 0;
  while (*s && o + 2 < cap) {
    unsigned char c = (unsigned char)*s;
    uint32_t cp;
    int n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c >> 5) == 0x6) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c >> 4) == 0xE) {
      cp = c & 0x0F;
      n = 3;
    } else if ((c >> 3) == 0x1E) {
      cp = c & 0x07;
      n = 4;
    } else {
      cp = 0xFFFD;
      n = 1;
    }
    for (int i = 1; i < n; i++) {
      if ((s[i] & 0xC0) != 0x80) {
        cp = 0xFFFD;
        n = 1;
        break;
      }
      cp = (cp << 6) | (s[i] & 0x3F);
    }
    s += n;
    if (cp <= 0xFFFF) {
      out[o++] = (WCHAR)cp;
    } else {
      cp -= 0x10000;
      out[o++] = (WCHAR)(0xD800 + (cp >> 10));
      out[o++] = (WCHAR)(0xDC00 + (cp & 0x3FF));
    }
  }
  out[o] = 0;
  return o;
}

static size_t u16_to_u8(const WCHAR* w, char* out, size_t cap) {
  size_t o = 0;
  while (*w && o + 5 < cap) {
    uint32_t cp = (uint16_t)*w++;
    if (cp >= 0xD800 && cp <= 0xDBFF && *w >= 0xDC00 && *w <= 0xDFFF)
      cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint16_t)*w++ - 0xDC00);
    if (cp < 0x80) {
      out[o++] = (char)cp;
    } else if (cp < 0x800) {
      out[o++] = (char)(0xC0 | (cp >> 6));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out[o++] = (char)(0xE0 | (cp >> 12));
      out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
      out[o++] = (char)(0xF0 | (cp >> 18));
      out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  out[o] = 0;
  return o;
}

// --- logging (mode-aware) ---------------------------------------------------
static void logmsg(int type, const char* utf8) {
  if (g_unicode) {
    if (g_logW) {
      WCHAR w[256];
      u8_to_u16(utf8, w, 256);
      g_logW(g_plugin_nr, type, w);
    }
  } else if (g_logA) {
    g_logA(g_plugin_nr, type, (char*)utf8);
  }
}

// --- connection -------------------------------------------------------------
static CpSerial* conn(void) {
  if (g_conn) return g_conn;
  if (g_disconnected) return NULL;  // user disconnected; don't auto-reconnect
  logmsg(MSGTYPE_CONNECT, "CrossPoint USB: connecting...");
  const char* env = getenv("CROSSPOINT_PORT");
  const char* port = (env && env[0]) ? env : (g_ini_port[0] ? g_ini_port : NULL);
  g_conn = cp_open(port);  // NULL => auto-detect by USB VID:PID, then scan
  if (!g_conn)
    logmsg(MSGTYPE_IMPORTANTERROR, "CrossPoint USB: no device. Open 'USB Transfer' on the reader, then retry.");
  return g_conn;
}

static void drop_conn(void) {
  if (g_conn) {
    cp_close(g_conn);
    g_conn = NULL;
  }
}

static void disconnect(void) {
  lock_acquire();
  drop_conn();
  g_disconnected = 1;
  lock_release();
}

// Called by Total/Double Commander when the user disconnects (e.g. right-click
// → Disconnect, or closing the panel). Close the serial port so the device is
// released and the next open re-runs the handshake cleanly.
WFX_EXPORT BOOL FsDisconnect(char* DisconnectRoot) {
  (void)DisconnectRoot;
  disconnect();
  logmsg(MSGTYPE_DISCONNECT, "CrossPoint USB: disconnected.");
  return TRUE;
}

WFX_EXPORT BOOL FsDisconnectW(WCHAR* DisconnectRoot) {
  (void)DisconnectRoot;
  disconnect();
  logmsg(MSGTYPE_DISCONNECT, "CrossPoint USB: disconnected.");
  return TRUE;
}

// --- path conversion ("\books\foo" -> "/books/foo", UTF-8) ------------------
static void dev_path(const char* tc, char* out, size_t cap) {
  size_t j = 0;
  if (!tc || !tc[0]) {
    snprintf(out, cap, "/");
    return;
  }
  for (size_t i = 0; tc[i] && j + 1 < cap; i++) out[j++] = (tc[i] == '\\') ? '/' : tc[i];
  out[j] = '\0';
  if (out[0] == '\0') snprintf(out, cap, "/");
}

static void dev_path_w(const WCHAR* tcw, char* out, size_t cap) {
  char tmp[1024];
  u16_to_u8(tcw, tmp, sizeof(tmp));
  dev_path(tmp, out, cap);
}

// --- find data --------------------------------------------------------------
static void unix_to_filetime(uint32_t t, FILETIME* ft) {
  uint64_t ll = ((uint64_t)t + 11644473600ULL) * 10000000ULL;  // 100ns since 1601
  ft->dwLowDateTime = (DWORD)(ll & 0xFFFFFFFFULL);
  ft->dwHighDateTime = (DWORD)(ll >> 32);
}

static void fill_common(const CpEntry* e, DWORD* attr, DWORD* lo, DWORD* hi, FILETIME* ft) {
  *attr = e->is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  *lo = (DWORD)(e->size & 0xFFFFFFFFULL);
  *hi = (DWORD)(e->size >> 32);
  unix_to_filetime(e->mtime, ft);
}

static void fill_a(const CpEntry* e, WIN32_FIND_DATAA* fd) {
  memset(fd, 0, sizeof(*fd));
  fill_common(e, &fd->dwFileAttributes, &fd->nFileSizeLow, &fd->nFileSizeHigh, &fd->ftLastWriteTime);
  fd->ftCreationTime = fd->ftLastAccessTime = fd->ftLastWriteTime;
  snprintf(fd->cFileName, sizeof(fd->cFileName), "%s", e->name);
}

static void fill_w(const CpEntry* e, WIN32_FIND_DATAW* fd) {
  memset(fd, 0, sizeof(*fd));
  fill_common(e, &fd->dwFileAttributes, &fd->nFileSizeLow, &fd->nFileSizeHigh, &fd->ftLastWriteTime);
  fd->ftCreationTime = fd->ftLastAccessTime = fd->ftLastWriteTime;
  u8_to_u16(e->name, fd->cFileName, 260);
}

// --- directory listing core (UTF-8) -----------------------------------------
typedef struct {
  CpEntry* items;
  int count;
  int cap;
  int idx;
} FindState;

static int collect_cb(const CpEntry* e, void* user) {
  FindState* st = (FindState*)user;
  if (st->count >= st->cap) {
    int newcap = st->cap ? st->cap * 2 : 32;
    CpEntry* p = (CpEntry*)realloc(st->items, (size_t)newcap * sizeof(CpEntry));
    if (!p) return 1;
    st->items = p;
    st->cap = newcap;
  }
  st->items[st->count++] = *e;
  return 0;
}

// Lists `utf8_dir` into a fresh FindState. Returns NULL on protocol error.
// Returns a valid (possibly empty) FindState for an empty directory so that
// Total Commander can enter it — FsFindFirst will synthesize a "." placeholder.
static FindState* do_list(const char* utf8_dir) {
  lock_acquire();
  // User is actively entering the plugin — clear the disconnect flag so
  // conn() will open a fresh connection.
  g_disconnected = 0;
  CpSerial* c = conn();
  if (!c) {
    lock_release();
    return NULL;
  }
  FindState* st = (FindState*)calloc(1, sizeof(FindState));
  if (!st) {
    lock_release();
    return NULL;
  }
  if (cp_list_dir(c, utf8_dir, collect_cb, st) != 0) {
    logmsg(MSGTYPE_IMPORTANTERROR, cp_last_error(c));
    drop_conn();
    free(st->items);
    free(st);
    lock_release();
    return NULL;
  }
  st->idx = 1;
  lock_release();
  return st;
}

// --- progress (mode-aware; src/dst are the host's original path pointers) ---
typedef struct {
  void* src;
  void* dst;
} ProgCtx;

// Total Commander's progress callback can pump its message loop and, from
// there, issue another WFX call on a different thread (e.g. a UI-thread
// dispatch triggered by a progress-dialog repaint). If that nested call
// blocks on g_lock while we're still holding it here, and TC's calling
// thread is itself waiting on that nested call to finish, the transfer
// deadlocks — the device keeps sending bytes into the OS receive buffer
// but nothing drains it, and the next read_exact() eventually times out
// partway through a chunk ("timeout: got N of 2048 bytes"). Release the
// lock for the duration of the callback so any reentrant call can proceed.
static int progress_cb(uint64_t done, uint64_t total, void* user) {
  ProgCtx* p = (ProgCtx*)user;
  int pct = total ? (int)((done * 100) / total) : 0;
  lock_release();
  int rc;
  if (g_unicode) rc = g_progW ? g_progW(g_plugin_nr, (WCHAR*)p->src, (WCHAR*)p->dst, pct) : 0;
  else rc = g_progA ? g_progA(g_plugin_nr, (char*)p->src, (char*)p->dst, pct) : 0;
  lock_acquire();
  return rc;
}

// --- transfer / op cores (UTF-8) --------------------------------------------
static int do_get(const char* remote, const char* local, void* psrc, void* pdst, int copyflags) {
  lock_acquire();
  CpSerial* c = conn();
  if (!c) {
    lock_release();
    return FS_FILE_READERROR;
  }
  if (!cp_download_supported(c)) {
    logmsg(MSGTYPE_IMPORTANTERROR, "CrossPoint USB: file download not supported by this firmware.");
    lock_release();
    return FS_FILE_NOTSUPPORTED;
  }
  if (!(copyflags & FS_COPYFLAGS_OVERWRITE) && cp_local_exists(local)) {
    lock_release();
    return FS_FILE_EXISTS;
  }
  ProgCtx ctx = {psrc, pdst};
  int rc = FS_FILE_OK;
  if (cp_download(c, remote, local, progress_cb, &ctx) != 0) {
    const char* err = cp_last_error(c);
    logmsg(MSGTYPE_IMPORTANTERROR, err);
    if (strstr(err, "aborted")) {
      rc = FS_FILE_USERABORT;
    } else {
      drop_conn();
      rc = FS_FILE_READERROR;
    }
  }
  lock_release();
  return rc;
}

static int do_put(const char* local, const char* remote, void* psrc, void* pdst) {
  lock_acquire();
  CpSerial* c = conn();
  if (!c) {
    lock_release();
    return FS_FILE_WRITEERROR;
  }
  ProgCtx ctx = {psrc, pdst};
  int rc = FS_FILE_OK;
  if (cp_upload(c, local, remote, progress_cb, &ctx) != 0) {
    const char* err = cp_last_error(c);
    logmsg(MSGTYPE_IMPORTANTERROR, err);
    if (strstr(err, "aborted")) {
      rc = FS_FILE_USERABORT;
    } else {
      drop_conn();
      rc = FS_FILE_WRITEERROR;
    }
  }
  lock_release();
  return rc;
}

static BOOL do_delete(const char* remote) {
  lock_acquire();
  CpSerial* c = conn();
  if (!c) {
    lock_release();
    return FALSE;
  }
  BOOL ok = TRUE;
  if (cp_remove(c, remote) != 0) {
    logmsg(MSGTYPE_IMPORTANTERROR, cp_last_error(c));
    ok = FALSE;
  }
  lock_release();
  return ok;
}

static BOOL do_mkdir(const char* path) {
  lock_acquire();
  CpSerial* c = conn();
  if (!c) {
    lock_release();
    return FALSE;
  }
  BOOL ok = TRUE;
  if (cp_mkdir(c, path) != 0) {
    logmsg(MSGTYPE_IMPORTANTERROR, cp_last_error(c));
    ok = FALSE;
  }
  lock_release();
  return ok;
}

static int do_rename(const char* a, const char* b) {
  lock_acquire();
  CpSerial* c = conn();
  if (!c) {
    lock_release();
    return FS_FILE_WRITEERROR;
  }
  int rc = FS_FILE_OK;
  if (cp_rename(c, a, b) != 0) {
    logmsg(MSGTYPE_IMPORTANTERROR, cp_last_error(c));
    drop_conn();
    rc = FS_FILE_WRITEERROR;
  }
  lock_release();
  return rc;
}

// --- ANSI exports -----------------------------------------------------------
WFX_EXPORT HANDLE FsFindFirst(char* Path, WIN32_FIND_DATAA* FindData) {
  char path[1024];
  dev_path(Path, path, sizeof(path));
  FindState* st = do_list(path);
  if (!st) return INVALID_HANDLE_VALUE;
  if (st->count == 0) {
    // Empty directory: synthesize a "." placeholder so TC can enter the dir.
    memset(FindData, 0, sizeof(*FindData));
    FindData->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    snprintf(FindData->cFileName, sizeof(FindData->cFileName), ".");
    return (HANDLE)st;
  }
  fill_a(&st->items[0], FindData);
  return (HANDLE)st;
}

WFX_EXPORT BOOL FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA* FindData) {
  FindState* st = (FindState*)Hdl;
  if (!st || st->idx >= st->count) return FALSE;
  fill_a(&st->items[st->idx++], FindData);
  return TRUE;
}

WFX_EXPORT int FsFindClose(HANDLE Hdl) {
  FindState* st = (FindState*)Hdl;
  if (st) {
    free(st->items);
    free(st);
  }
  return 0;
}

WFX_EXPORT int FsGetFile(char* RemoteName, char* LocalName, int CopyFlags, void* ri) {
  (void)ri;
  char remote[1024];
  dev_path(RemoteName, remote, sizeof(remote));
  return do_get(remote, LocalName, RemoteName, LocalName, CopyFlags);
}

WFX_EXPORT int FsPutFile(char* LocalName, char* RemoteName, int CopyFlags) {
  (void)CopyFlags;
  char remote[1024];
  dev_path(RemoteName, remote, sizeof(remote));
  return do_put(LocalName, remote, LocalName, RemoteName);
}

WFX_EXPORT BOOL FsDeleteFile(char* RemoteName) {
  char remote[1024];
  dev_path(RemoteName, remote, sizeof(remote));
  return do_delete(remote);
}

WFX_EXPORT BOOL FsRemoveDir(char* RemoteName) { return FsDeleteFile(RemoteName); }

WFX_EXPORT BOOL FsMkDir(char* Path) {
  char path[1024];
  dev_path(Path, path, sizeof(path));
  return do_mkdir(path);
}

WFX_EXPORT int FsRenMovFile(char* OldName, char* NewName, BOOL Move, BOOL OverWrite, void* ri) {
  (void)Move;
  (void)OverWrite;
  (void)ri;
  char a[1024], b[1024];
  dev_path(OldName, a, sizeof(a));
  dev_path(NewName, b, sizeof(b));
  return do_rename(a, b);
}

// --- wide (Unicode) exports -------------------------------------------------
WFX_EXPORT HANDLE FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData) {
  char path[1024];
  dev_path_w(Path, path, sizeof(path));
  FindState* st = do_list(path);
  if (!st) return INVALID_HANDLE_VALUE;
  if (st->count == 0) {
    // Empty directory: synthesize a "." placeholder so TC can enter the dir.
    memset(FindData, 0, sizeof(*FindData));
    FindData->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    FindData->cFileName[0] = L'.';
    FindData->cFileName[1] = L'\0';
    return (HANDLE)st;
  }
  fill_w(&st->items[0], FindData);
  return (HANDLE)st;
}

WFX_EXPORT BOOL FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW* FindData) {
  FindState* st = (FindState*)Hdl;
  if (!st || st->idx >= st->count) return FALSE;
  fill_w(&st->items[st->idx++], FindData);
  return TRUE;
}

WFX_EXPORT int FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, void* ri) {
  (void)ri;
  char remote[1024], local[1024];
  dev_path_w(RemoteName, remote, sizeof(remote));
  u16_to_u8(LocalName, local, sizeof(local));
  return do_get(remote, local, RemoteName, LocalName, CopyFlags);
}

WFX_EXPORT int FsPutFileW(WCHAR* LocalName, WCHAR* RemoteName, int CopyFlags) {
  (void)CopyFlags;
  char remote[1024], local[1024];
  dev_path_w(RemoteName, remote, sizeof(remote));
  u16_to_u8(LocalName, local, sizeof(local));
  return do_put(local, remote, LocalName, RemoteName);
}

WFX_EXPORT BOOL FsDeleteFileW(WCHAR* RemoteName) {
  char remote[1024];
  dev_path_w(RemoteName, remote, sizeof(remote));
  return do_delete(remote);
}

WFX_EXPORT BOOL FsRemoveDirW(WCHAR* RemoteName) { return FsDeleteFileW(RemoteName); }

WFX_EXPORT BOOL FsMkDirW(WCHAR* Path) {
  char path[1024];
  dev_path_w(Path, path, sizeof(path));
  return do_mkdir(path);
}

WFX_EXPORT int FsRenMovFileW(WCHAR* OldName, WCHAR* NewName, BOOL Move, BOOL OverWrite, void* ri) {
  (void)Move;
  (void)OverWrite;
  (void)ri;
  char a[1024], b[1024];
  dev_path_w(OldName, a, sizeof(a));
  dev_path_w(NewName, b, sizeof(b));
  return do_rename(a, b);
}

// --- plugin lifecycle -------------------------------------------------------
WFX_EXPORT int FsInit(int PluginNr, tProgressProc pProgress, tLogProc pLog, tRequestProc pRequest) {
  (void)pRequest;
  lock_init();
  g_unicode = 0;
  g_plugin_nr = PluginNr;
  g_progA = pProgress;
  g_logA = pLog;
  return 0;
}

WFX_EXPORT int FsInitW(int PluginNr, tProgressProcW pProgress, tLogProcW pLog, tRequestProcW pRequest) {
  (void)pRequest;
  lock_init();
  g_unicode = 1;
  g_plugin_nr = PluginNr;
  g_progW = pProgress;
  g_logW = pLog;
  return 0;
}

WFX_EXPORT void FsGetDefRootName(char* DefRootName, int maxlen) {
  snprintf(DefRootName, (size_t)maxlen, "CrossPoint USB");
}

// Case-insensitive equality for short ASCII keys (portable; avoids strcasecmp
// header differences between glibc and mingw).
static int ieq(const char* a, const char* b) {
  for (; *a && *b; a++, b++) {
    char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (ca != cb) return 0;
  }
  return *a == *b;
}

static char* trim(char* s) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
  return s;
}

// Read an optional "Port=" from the [crosspoint] section of the plugin's ini, so
// users (especially on Windows, where env vars are awkward) can pin the port.
static void read_ini_port(const char* ini_path) {
  if (!ini_path || !ini_path[0]) return;
  FILE* f = fopen(ini_path, "r");
  if (!f) return;
  char line[512];
  int in_section = 0;
  while (fgets(line, sizeof(line), f)) {
    char* p = trim(line);
    if (p[0] == ';' || p[0] == '#' || p[0] == '\0') continue;
    if (p[0] == '[') {
      in_section = ieq(p, "[crosspoint]");
      continue;
    }
    if (!in_section) continue;
    char* eq = strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    if (ieq(trim(p), "port")) {
      char* val = trim(eq + 1);
      snprintf(g_ini_port, sizeof(g_ini_port), "%s", val);
    }
  }
  fclose(f);
}

WFX_EXPORT void FsSetDefaultParams(FsDefaultParamStruct* dps) {
  if (dps) read_ini_port(dps->DefaultIniName);
}

WFX_EXPORT int FsExecuteFile(HANDLE MainWin, char* RemoteName, char* Verb) {
  (void)MainWin;
  (void)RemoteName;
  (void)Verb;
  return 0;
}
