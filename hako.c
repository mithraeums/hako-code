/* SPDX-License-Identifier: GPL-3.0-or-later — Copyright (C) 2026 Zachary Blauser */
/*
 * hako-code — standalone AI agent CLI (binary: `hako`).
 * Lifted from hake.c (the editor's AI panel). Same constraints:
 *   C99, libc + pthread + curl-on-PATH. No other deps.
 *   Single file. Tabs.
 *
 * Sections (mirror hake.c order where lifted):
 *   includes / defines / enums / structs / globals
 *   forward decls
 *   json helpers
 *   file/shell helpers
 *   path + trust
 *   provider resolution
 *   session/state
 *   history log + tail loader
 *   slash commands
 *   tools registry + exec
 *   build messages / curl / extract
 *   tool loop + worker thread
 *   .hakorc parser
 *   init / cleanup
 *   termios raw line editor (CLI input)
 *   REPL + main
 */

#define HAKO_VERSION "0.3.0"

#define HAKO_COPILOT_EDITOR_VER   "vscode/1.95.0"
#define HAKO_COPILOT_PLUGIN_VER   "copilot-chat/0.22.0"
#define HAKO_REPO    "mithraeums/hako-code"

#if defined(__wasi__) || defined(__EMSCRIPTEN__)
#define HAKO_WASM 1
#ifndef __EMSCRIPTEN__
typedef int sig_atomic_t;         /* wasi has no <signal.h> without emulation */
#endif
#endif

#ifndef _WIN32
#define _GNU_SOURCE
#else
#define _CRT_RAND_S                 /* rand_s(): the OS generator, for secrets */
#endif

#include <ctype.h>
#include <errno.h>
#ifdef __APPLE__
#include <sys/random.h>             /* getentropy */
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <conio.h>
#include <io.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#define popen _popen
#define pclose _pclose
/* Not in the Windows CRT. */
static char *strcasestr(const char *h, const char *n) {
	size_t nl = strlen(n);
	if (!nl) return (char *)h;
	for (; *h; h++) if (!_strnicmp(h, n, nl)) return (char *)h;
	return NULL;
}
#define getcwd _getcwd
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define read(fd, buf, n) _read(fd, buf, n)
#define write(fd, buf, n) _write(fd, buf, n)
#define mkdir(p, m) _mkdir(p)
#define realpath(p, r) _fullpath((r), (p), PATH_MAX)
#ifndef WEXITSTATUS
#define WEXITSTATUS(x) (x)
#endif
#ifdef _MSC_VER
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#define strcasecmp _stricmp
#endif
#if defined(__MINGW32__) || defined(__MINGW64__)
#include <dirent.h>
#include <unistd.h>
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#include <stdlib.h>
/* No Windows getline shim: every reader here walks a buffer from the
   platform seam instead (see hkForEachLine), so the shim was dead code. */
#endif
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <strings.h>
#include <sys/time.h>
#ifndef HAKO_WASM
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif
#endif
#include <pthread.h>

#define AI_HISTORY_MAX 1000

#define ANSI_DIM     "\x1b[2m"
#define ANSI_BOLD    "\x1b[1m"
/* Which binary is this? Two sessions of bug reports were spent on behavior
   that had already been fixed but not restarted into. The banner says. */
#define HK_BUILD_STAMP (__DATE__ " " __TIME__)

#define ANSI_RESET   "\x1b[0m"
#define ANSI_CLR_LINE "\r\x1b[K"

/* ---------- the palette is DATA ---------- */
#define HK_C_NONE     0x1000000u          /* leave whatever the terminal uses */
#define HK_C_DIM      0x2000000u          /* the dim attribute */
#define HK_C_BOLDRED  0x3000000u          /* errors, legible on any profile */
#define HK_C_ANSI(n)  (0x4000000u | (unsigned)(n))   /* a basic-16 code */

enum { TH_I_USER, TH_I_AI, TH_I_SYS, TH_I_ERR, TH_I_TOOL,
       TH_I_OK, TH_I_ACCENT, TH_I_META, TH_I_GHOST, TH_SLOTS };

static const char *TH_USER   = "";
static const char *TH_AI     = "";
static const char *TH_SYS    = "";
static const char *TH_ERR    = "";
static const char *TH_TOOL   = "";
static const char *TH_OK     = "";
static const char *TH_ACCENT = "";
static const char *TH_META   = "";
static const char *TH_GHOST  = "";

typedef struct { const char *name; unsigned c[TH_SLOTS]; } clTheme;
static const clTheme TH_PRESETS[] = {
	/* the default: your terminal's own 16 colors and its fg/bg, nothing of
	   ours. First, so a generated row of the same name can never shadow it. */
	{"terminal",  {HK_C_ANSI(33), HK_C_ANSI(39), HK_C_ANSI(90), HK_C_BOLDRED, HK_C_ANSI(36),
	               HK_C_ANSI(32), HK_C_ANSI(33), HK_C_ANSI(90), HK_C_ANSI(90)}},
/* THEMES:BEGIN — generated by hako-edit/tools/themes.py from hake.c; do not edit */
	{"mithraeum",    {0xe6e0d1, 0xe6e0d1, 0x9a9383, HK_C_BOLDRED, 0xdccfae, 0xaeb398, 0xb7bdca, 0x8a8473, 0x5f5a4e}},
	{"washi",        {0x5a4636, HK_C_NONE, 0x6e685a, HK_C_BOLDRED, 0x5c5230, 0x6e685a, 0x2f3a4f, 0x6e685a, 0xa39b88}},
	{"dark",         {0x4ec9b0, 0xd4d4d4, 0x6a9955, HK_C_BOLDRED, 0xce9178, 0x6a9955, 0x569cd6, 0x6a9955, 0x858585}},
	{"light",        {0x008080, HK_C_NONE, 0x008000, HK_C_BOLDRED, 0xa31515, 0x008000, 0x0000c8, 0x008000, 0x969696}},
	{"gruvbox",      {0xb8bb26, 0xebdbb2, 0x928374, HK_C_BOLDRED, 0xb8bb26, 0xb8bb26, 0xfb4934, 0x928374, 0x7c6f64}},
	{"nord",         {0x8fbcbb, 0xe5e9f0, 0x6c788d, HK_C_BOLDRED, 0xd08770, 0xa3be8c, 0x88c0d0, 0x646e80, 0x4c566a}},
	{"dracula",      {0x8be9fd, 0xf8f8f2, 0x6272a4, HK_C_BOLDRED, 0xf1fa8c, 0xf1fa8c, 0xff79c6, 0x6272a4, 0x6272a4}},
	{"monokai",      {0x66d9ef, 0xf8f8f2, 0x75715e, HK_C_BOLDRED, 0xe6db74, 0x75715e, 0xf92672, 0x75715e, 0x75715e}},
	{"solarized",    {0x2aa198, 0x839496, 0x586e75, HK_C_BOLDRED, 0x2aa198, 0x2aa198, 0x268bd2, 0x586e75, 0x586e75}},
	{"tokyonight",   {0x7dcfff, 0xc0caf5, 0x565f89, HK_C_BOLDRED, 0x9ece6a, 0x9ece6a, 0xbb9af7, 0x565f89, 0x3c4466}},
	{"catppuccin",   {0x89dceb, 0xcdd6f4, 0x6c7086, HK_C_BOLDRED, 0xa6e3a1, 0xa6e3a1, 0xcba6f7, 0x6c7086, 0x585b70}},
	{"onedark",      {0x56b6c2, 0xabb2bf, 0x5c6370, HK_C_BOLDRED, 0x98c379, 0x98c379, 0xc678dd, 0x5c6370, 0x4c5263}},
	{"material",     {0x82aaff, 0xeeffff, 0x546e7a, HK_C_BOLDRED, 0xc3e88d, 0xc3e88d, 0xc792ea, 0x546e7a, 0x3f5158}},
	{"everforest",   {0x7fbbb3, 0xd3c6aa, 0x859289, HK_C_BOLDRED, 0xa7c080, 0xa7c080, 0xe67e80, 0x859289, 0x566362}},
	{"rosepine",     {0x9ccfd8, 0xe0def4, 0x6e6a86, HK_C_BOLDRED, 0xf6c177, 0x9ccfd8, 0xc4a7e7, 0x6e6a86, 0x524f67}},
	{"github-dark",  {0x79c0ff, 0xc9d1d9, 0x8b949e, HK_C_BOLDRED, 0xa5d6ff, 0x8b949e, 0xff7b72, 0x8b949e, 0x484f58}},
	{"github-light", {0x0550ae, HK_C_NONE, 0x6a737d, HK_C_BOLDRED, 0x0a3069, 0x6a737d, 0xcf222e, 0x6a737d, 0x8c959f}},
	{"ayu",          {0x39bae6, 0xbfbdb6, 0x5c6773, HK_C_BOLDRED, 0xaad94c, 0xaad94c, 0xff8f40, 0x5c6773, 0x3c4b5a}},
	{"kanagawa",     {0x7fb4ca, 0xdcd7ba, 0x727169, HK_C_BOLDRED, 0x98bb6c, 0x98bb6c, 0x957fb8, 0x727169, 0x54546d}},
/* THEMES:END */
};
static const int TH_PRESET_COUNT = sizeof(TH_PRESETS) / sizeof(TH_PRESETS[0]);
static char TH_ACTIVE[32] = "terminal";
/* rc: theme.<name>.<slot>=<color> lines make a custom theme (below, with the rc) */
static int hkThemeCustom(const char *name, unsigned out[TH_SLOTS]);

static int cl_truecolor = 1;

static void clDetectTruecolor(void) {
	const char *force = getenv("HAKO_TRUECOLOR");
	if (force) { cl_truecolor = atoi(force) ? 1 : 0; return; }
	const char *ct = getenv("COLORTERM");
	if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) { cl_truecolor = 1; return; }
	const char *term = getenv("TERM");
	if (term && (strstr(term, "truecolor") || strstr(term, "direct"))) { cl_truecolor = 1; return; }
	cl_truecolor = 0;
}

static int clRgbTo256(int r, int g, int b) {
	static const int q[] = {0, 95, 135, 175, 215, 255};
	int ri = 0, gi = 0, bi = 0;
	for (int i = 0; i < 6; i++) {
		if (abs(r - q[i]) < abs(r - q[ri])) ri = i;
		if (abs(g - q[i]) < abs(g - q[gi])) gi = i;
		if (abs(b - q[i]) < abs(b - q[bi])) bi = i;
	}
	int cr = q[ri] - r, cg = q[gi] - g, cb = q[bi] - b;
	int v = (r + g + b) / 3, gs = v < 8 ? 0 : (v > 238 ? 23 : (v - 8) / 10);
	int gv = 8 + gs * 10, dr = gv - r, dg = gv - g, db = gv - b;
	/* the cube rounds each channel alone, so a warm paper lands on pink; a near
	   grey is often the closer colour, and that is the one to use */
	if (dr * dr + dg * dg + db * db < cr * cr + cg * cg + cb * cb) return 232 + gs;
	return 16 + 36 * ri + 6 * gi + bi;
}

/* The ONE place a colour escape is spelled. Everything else names a slot. */
static void clSeqInto(char *out, size_t cap, unsigned c) {
	const char esc = 27;
	if (c == HK_C_NONE)         { out[0] = 0; return; }
	if (c == HK_C_DIM)          { snprintf(out, cap, "%c[2m", esc); return; }
	if (c == HK_C_BOLDRED)      { snprintf(out, cap, "%c[1;91m", esc); return; }
	if (c & 0x4000000u)         { snprintf(out, cap, "%c[%um", esc, c & 0xffu); return; }
	int r = (int)((c >> 16) & 0xff), g = (int)((c >> 8) & 0xff), b = (int)(c & 0xff);
	if (cl_truecolor) snprintf(out, cap, "%c[38;2;%d;%d;%dm", esc, r, g, b);
	else              snprintf(out, cap, "%c[38;5;%dm", esc, clRgbTo256(r, g, b));
}

/* A short-lived sequence for one colour — for swatches and one-off prints. */
static const char *clSeq(unsigned c) {
	static char buf[12][24];
	static int slot = 0;
	char *out = buf[slot]; slot = (slot + 1) % 12;
	clSeqInto(out, sizeof(buf[0]), c);
	return out;
}

/* A theme's nine colors: a preset, else a custom theme from the rc. */
static int clThemeLookup(const char *name, unsigned out[TH_SLOTS]) {
	if (!name || !*name) return 0;
	for (int i = 0; i < TH_PRESET_COUNT; i++)
		if (!strcmp(TH_PRESETS[i].name, name)) { memcpy(out, TH_PRESETS[i].c, sizeof(unsigned) * TH_SLOTS); return 1; }
	return hkThemeCustom(name, out);
}

static void clThemeApply(const char *name) {
	static char slots[TH_SLOTS][24];
	unsigned c[TH_SLOTS];
	/* ~/.hakorc is shared, and the editor has themes this file does not
	   (gruvbox, dracula, ...). Their nearest match here is the terminal's own
	   palette, not silently staying on the default. */
	if (!clThemeLookup(name, c)) { name = "terminal"; memcpy(c, TH_PRESETS[0].c, sizeof c); }
	for (int k = 0; k < TH_SLOTS; k++) clSeqInto(slots[k], sizeof(slots[0]), c[k]);
	TH_USER   = slots[TH_I_USER];   TH_AI    = slots[TH_I_AI];
	TH_SYS    = slots[TH_I_SYS];    TH_ERR   = slots[TH_I_ERR];
	TH_TOOL   = slots[TH_I_TOOL];   TH_OK    = slots[TH_I_OK];
	TH_ACCENT = slots[TH_I_ACCENT]; TH_META  = slots[TH_I_META];
	TH_GHOST  = slots[TH_I_GHOST];
	if (name != TH_ACTIVE) snprintf(TH_ACTIVE, sizeof(TH_ACTIVE), "%s", name);
}

#define ANSI_AI      TH_AI
#define ANSI_ERR     TH_ERR
#define ANSI_TOOL    TH_TOOL

#ifdef _WIN32
#define cl_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
static void cl_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

enum aiProviderType {
	AI_PROVIDER_NONE,
	AI_PROVIDER_OLLAMA,
	AI_PROVIDER_ANTHROPIC,
	AI_PROVIDER_OPENAI,
	AI_PROVIDER_MITHRAEUM
};

#define HK_IS_OLLAMA_WIRE(t) ((t) == AI_PROVIDER_OLLAMA || (t) == AI_PROVIDER_MITHRAEUM)

#define HK_ROLE_SYSTEM 0
#define HK_ROLE_USER   1
#define HK_ROLE_AI     2

typedef struct aiMessage {
	char *role;
	char *content;
	int raw;
} aiMessage;

typedef struct aiData {
	char **history;
	unsigned char *history_role;
	int history_count;

	char *current_prompt;
	char *current_response;

	int active;
	pthread_t worker_thread;
	int streaming;
	pthread_mutex_t lock;

	pthread_t anim_thread;
	int animating;
	int anim_style;
	int anim_label;
	const char *anim_verb;
	long anim_t0;             /* when the spinner started, for "worked for x" */
	int turn_index;

	aiMessage *messages;
	int message_count;
	int message_cap;
	char *system_prompt;

	int last_in_tokens;
	int last_out_tokens;
	long total_in_tokens;
	long total_out_tokens;
	long last_turn_ms;
	long turn_start_ms;
	long total_cache_write;
	long total_cache_read;
	int  compact_floor;   /* do not retry compaction until the history grows past this */
	/* Set when a reply has already been shown token by token, so storing it
	   does not print the whole answer a second time. */
	int  suppress_next_echo;
} aiData;

struct clConfig {
	enum aiProviderType ai_provider_type;
	char *ai_api_key;
	char *ai_endpoint;
	char *ai_model;
	int ai_temperature;
	int ai_max_tokens;
	int ai_shell_timeout;
	int ai_prompt_cache;
	long ai_context_window;
	char *ai_active_skills;   /* comma-separated; these load in full, every turn */
	char *session_name;       /* what this conversation is about, set with :name */
	char *session_color;      /* per-chat accent, independent of the theme */
	int  output_json;         /* -p with --output-format json: one object on stdout, nothing else */
	int ai_tools_enabled;
	int ai_tool_gate;
	int ai_toolmode;
	int ai_stream;
	int ai_auto_approve;
	int ai_plan_mode;
	char *editor;             /* rc: editor=... for composing a long prompt */

	char *session_id;
	long session_started;
	long session_last_used;
	int session_turn_count;
	int session_resumed;

	int color_enabled;
	int interrupt;

	int anim_force_style;
	int debug;
	int compact;
	int pipe_mode;
	int serve_mode;
	int show_splash;
	int mouse_enabled;        /* tap/click to open a tool result; mouse=0 turns it off */
	int update_check;         /* rc update_check=0: never ask GitHub for a newer release */
	int voice;                /* rc voice=1 / :voice on: read each finished reply aloud */
	int voice_rate;           /* words per minute, 0 = the backend's own */
	char *voice_cmd;          /* rc voice_cmd: the speaker, text on stdin (global rc only) */
	unsigned char last_role_shown;

	char *ai_oauth_provider;
	char *ai_oauth_refresh;
	long  ai_oauth_expires_at;
};

struct clConfig E;
static aiData G_AI;


typedef struct { char *url, *headers, *body; int stream; } hkReqParts;

typedef struct {
	const char *method;
	const char *url;
	const char *headers;
	const char *body;
	int         stream;
	int         timeout_s;   /* 0 = no limit, as every provider call wants */
	int         follow;      /* follow redirects; only web_fetch asks for it */
} hkHttpReq;

static char *hk_http_fetch(const hkHttpReq *req);
static void *hk_http_open(const hkHttpReq *req);
static char *hk_http_gets(void *h, char *buf, int cap);
static void  hk_http_close(void *h);

static long hk_time_ms(void);
static long hk_time_unix(void);

static char *hk_fs_read(const char *path, long max_bytes, long *out_len);
static int   hk_fs_write(const char *path, const char *data, size_t len, int append);
static int   hk_fs_exists(const char *path, long *size, long *mtime, int *is_dir);
static int   hk_fs_mkdirp(const char *path);
static int   hk_fs_remove(const char *path);
static char *hk_fs_list(const char *path);

#if defined(_WIN32) || defined(HAKO_WASM)
static char *hk_shell_capture(const char *cmd, long max_bytes, int *exit_code);
#endif
static int hkSbAdd(char **buf, size_t *len, size_t *cap, const char *s, size_t n);

/* Format into a fixed buffer and say whether it FIT. */
static int hkFmtFits(char *dst, size_t cap, const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 3, 4)))
#endif
;
static int hkFmtFits(char *dst, size_t cap, const char *fmt, ...) {
	if (!dst || cap == 0) return 0;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, cap, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= cap) { dst[0] = '\0'; return 0; }
	return 1;
}

/* n bytes from the OS's entropy source. Returns 0 when there is none: callers
   refuse rather than fall back to rand(). */
static int hkRandomBytes(uint8_t *out, size_t n) {
	while (n > 0) {
		size_t want = n < 64 ? n : 64, got = 0;
#ifdef _WIN32
		for (; got < want; got++) { unsigned v; if (rand_s(&v) != 0) break; out[got] = (uint8_t)v; }
#else
		if (getentropy(out, want) == 0) got = want;
		else {
			FILE *fp = fopen("/dev/urandom", "rb");
			if (fp) { got = fread(out, 1, want, fp); fclose(fp); }
		}
#endif
		if (got == 0) return 0;
		out += got; n -= got;
	}
	return 1;
}

/* n characters from alpha (length < 256), with no modulo bias. Returns 0 when
   there is no entropy: a guessable token is worse than no token. */
static int hkRandomToken(char *out, int n, const char *alpha) {
	int al = (int)strlen(alpha), lim = 256 - 256 % al, k = 0;
	while (k < n) {
		uint8_t b[64];
		if (!hkRandomBytes(b, sizeof b)) { out[0] = '\0'; return 0; }
		for (int i = 0; i < (int)sizeof b && k < n; i++) if (b[i] < lim) out[k++] = alpha[b[i] % al];
	}
	out[n] = '\0';
	return 1;
}

/* ==== monocypher 4.0.2, vendored (RELAY.md §5) ================================
   https://monocypher.org, monocypher-4.0.2.tar.gz sha512 bf275d4c53ff94af…
   (matches the published checksum and the GitHub 4.0.2 tag). Upstream text,
   trimmed to what hako calls: Argon2, EdDSA, Elligator, scalar arithmetic and
   one unused helper removed, nothing else changed. */
// Monocypher version 4.0.2
//
// This file is dual-licensed.  Choose whichever licence you want from
// the two licences listed below.
//
// The first licence is a regular 2-clause BSD licence.  The second licence
// is the CC-0 from Creative Commons. It is intended to release Monocypher
// to the public domain.  The BSD licence serves as a fallback option.
//
// SPDX-License-Identifier: BSD-2-Clause OR CC0-1.0
//
// ------------------------------------------------------------------------
//
// Copyright (c) 2017-2020, Loup Vaillant
// All rights reserved.
//
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the
//    distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// ------------------------------------------------------------------------
//
// Written in 2017-2020 by Loup Vaillant
//
// To the extent possible under law, the author(s) have dedicated all copyright
// and related neighboring rights to this software to the public domain
// worldwide.  This software is distributed without any warranty.
//
// You should have received a copy of the CC0 Public Domain Dedication along
// with this software.  If not, see
// <https://creativecommons.org/publicdomain/zero/1.0/>


// --- from monocypher.h (trimmed to what hako calls) ---

// Constant time comparisons
// -------------------------

// Return 0 if a and b are equal, -1 otherwise
int crypto_verify16(const uint8_t a[16], const uint8_t b[16]);
int crypto_verify32(const uint8_t a[32], const uint8_t b[32]);
int crypto_verify64(const uint8_t a[64], const uint8_t b[64]);


// Erase sensitive data
// --------------------
void crypto_wipe(void *secret, size_t size);


// Authenticated encryption
// ------------------------
void crypto_aead_lock(uint8_t       *cipher_text,
                      uint8_t        mac  [16],
                      const uint8_t  key  [32],
                      const uint8_t  nonce[24],
                      const uint8_t *ad,         size_t ad_size,
                      const uint8_t *plain_text, size_t text_size);
int crypto_aead_unlock(uint8_t       *plain_text,
                       const uint8_t  mac  [16],
                       const uint8_t  key  [32],
                       const uint8_t  nonce[24],
                       const uint8_t *ad,          size_t ad_size,
                       const uint8_t *cipher_text, size_t text_size);

// Authenticated stream
// --------------------
typedef struct {
	uint64_t counter;
	uint8_t  key[32];
	uint8_t  nonce[8];
} crypto_aead_ctx;

void crypto_aead_init_x(crypto_aead_ctx *ctx,
                        const uint8_t key[32], const uint8_t nonce[24]);
void crypto_aead_init_djb(crypto_aead_ctx *ctx,
                          const uint8_t key[32], const uint8_t nonce[8]);
void crypto_aead_init_ietf(crypto_aead_ctx *ctx,
                           const uint8_t key[32], const uint8_t nonce[12]);

void crypto_aead_write(crypto_aead_ctx *ctx,
                       uint8_t         *cipher_text,
                       uint8_t          mac[16],
                       const uint8_t   *ad        , size_t ad_size,
                       const uint8_t   *plain_text, size_t text_size);
int crypto_aead_read(crypto_aead_ctx *ctx,
                     uint8_t         *plain_text,
                     const uint8_t    mac[16],
                     const uint8_t   *ad        , size_t ad_size,
                     const uint8_t   *cipher_text, size_t text_size);


// General purpose hash (BLAKE2b)
// ------------------------------

// Direct interface
void crypto_blake2b(uint8_t *hash,          size_t hash_size,
                    const uint8_t *message, size_t message_size);

void crypto_blake2b_keyed(uint8_t *hash,          size_t hash_size,
                          const uint8_t *key,     size_t key_size,
                          const uint8_t *message, size_t message_size);

// Incremental interface
typedef struct {
	// Do not rely on the size or contents of this type,
	// for they may change without notice.
	uint64_t hash[8];
	uint64_t input_offset[2];
	uint64_t input[16];
	size_t   input_idx;
	size_t   hash_size;
} crypto_blake2b_ctx;

void crypto_blake2b_init(crypto_blake2b_ctx *ctx, size_t hash_size);
void crypto_blake2b_keyed_init(crypto_blake2b_ctx *ctx, size_t hash_size,
                               const uint8_t *key, size_t key_size);
void crypto_blake2b_update(crypto_blake2b_ctx *ctx,
                           const uint8_t *message, size_t message_size);
void crypto_blake2b_final(crypto_blake2b_ctx *ctx, uint8_t *hash);


// Key exchange (X-25519)
// ----------------------

// Shared secrets are not quite random.
// Hash them to derive an actual shared key.
void crypto_x25519_public_key(uint8_t       public_key[32],
                              const uint8_t secret_key[32]);
void crypto_x25519(uint8_t       raw_shared_secret[32],
                   const uint8_t your_secret_key  [32],
                   const uint8_t their_public_key [32]);

// Conversion to EdDSA
void crypto_x25519_to_eddsa(uint8_t eddsa[32], const uint8_t x25519[32]);

// scalar "division"
// Used for OPRF.  Be aware that exponential blinding is less secure
// than Diffie-Hellman key exchange.
void crypto_x25519_inverse(uint8_t       blind_salt [32],
                           const uint8_t private_key[32],
                           const uint8_t curve_point[32]);

// "Dirty" versions of x25519_public_key().
// Use with crypto_elligator_rev().
// Leaks 3 bits of the private key.
void crypto_x25519_dirty_small(uint8_t pk[32], const uint8_t sk[32]);
void crypto_x25519_dirty_fast (uint8_t pk[32], const uint8_t sk[32]);


// Chacha20
// --------

// Specialised hash.
// Used to hash X25519 shared secrets.
void crypto_chacha20_h(uint8_t       out[32],
                       const uint8_t key[32],
                       const uint8_t in [16]);

// Unauthenticated stream cipher.
// Don't forget to add authentication.
uint64_t crypto_chacha20_djb(uint8_t       *cipher_text,
                             const uint8_t *plain_text,
                             size_t         text_size,
                             const uint8_t  key[32],
                             const uint8_t  nonce[8],
                             uint64_t       ctr);
uint32_t crypto_chacha20_ietf(uint8_t       *cipher_text,
                              const uint8_t *plain_text,
                              size_t         text_size,
                              const uint8_t  key[32],
                              const uint8_t  nonce[12],
                              uint32_t       ctr);
uint64_t crypto_chacha20_x(uint8_t       *cipher_text,
                           const uint8_t *plain_text,
                           size_t         text_size,
                           const uint8_t  key[32],
                           const uint8_t  nonce[24],
                           uint64_t       ctr);


// Poly 1305
// ---------

// This is a *one time* authenticator.
// Disclosing the mac reveals the key.
// See crypto_lock() on how to use it properly.

// Direct interface
void crypto_poly1305(uint8_t        mac[16],
                     const uint8_t *message, size_t message_size,
                     const uint8_t  key[32]);

// Incremental interface
typedef struct {
	// Do not rely on the size or contents of this type,
	// for they may change without notice.
	uint8_t  c[16];  // chunk of the message
	size_t   c_idx;  // How many bytes are there in the chunk.
	uint32_t r  [4]; // constant multiplier (from the secret key)
	uint32_t pad[4]; // random number added at the end (from the secret key)
	uint32_t h  [5]; // accumulated hash
} crypto_poly1305_ctx;

void crypto_poly1305_init  (crypto_poly1305_ctx *ctx, const uint8_t key[32]);
void crypto_poly1305_update(crypto_poly1305_ctx *ctx,
                            const uint8_t *message, size_t message_size);
void crypto_poly1305_final (crypto_poly1305_ctx *ctx, uint8_t mac[16]);

// --- from monocypher.c (Argon2, EdDSA, Elligator, scalar arithmetic removed) ---


/////////////////
/// Utilities ///
/////////////////
#define FOR_T(type, i, start, end) for (type i = (start); i < (end); i++)
#define FOR(i, start, end)         FOR_T(size_t, i, start, end)
#define COPY(dst, src, size)       FOR(_i_, 0, size) (dst)[_i_] = (src)[_i_]
#define ZERO(buf, size)            FOR(_i_, 0, size) (buf)[_i_] = 0
#define WIPE_CTX(ctx)              crypto_wipe(ctx   , sizeof(*(ctx)))
#define WIPE_BUFFER(buffer)        crypto_wipe(buffer, sizeof(buffer))
#define MIN(a, b)                  ((a) <= (b) ? (a) : (b))
#define MAX(a, b)                  ((a) >= (b) ? (a) : (b))

typedef int8_t   i8;
typedef uint8_t  u8;
typedef int16_t  i16;
typedef uint32_t u32;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint64_t u64;

static const u8 zero[128] = {0};

// returns the smallest positive integer y such that
// (x + y) % pow_2  == 0
// Basically, y is the "gap" missing to align x.
// Only works when pow_2 is a power of 2.
// Note: we use ~x+1 instead of -x to avoid compiler warnings
static size_t gap(size_t x, size_t pow_2)
{
	return (~x + 1) & (pow_2 - 1);
}

static u32 load24_le(const u8 s[3])
{
	return
		((u32)s[0] <<  0) |
		((u32)s[1] <<  8) |
		((u32)s[2] << 16);
}

static u32 load32_le(const u8 s[4])
{
	return
		((u32)s[0] <<  0) |
		((u32)s[1] <<  8) |
		((u32)s[2] << 16) |
		((u32)s[3] << 24);
}

static u64 load64_le(const u8 s[8])
{
	return load32_le(s) | ((u64)load32_le(s+4) << 32);
}

static void store32_le(u8 out[4], u32 in)
{
	out[0] =  in        & 0xff;
	out[1] = (in >>  8) & 0xff;
	out[2] = (in >> 16) & 0xff;
	out[3] = (in >> 24) & 0xff;
}

static void store64_le(u8 out[8], u64 in)
{
	store32_le(out    , (u32)in );
	store32_le(out + 4, in >> 32);
}

static void load32_le_buf (u32 *dst, const u8 *src, size_t size) {
	FOR(i, 0, size) { dst[i] = load32_le(src + i*4); }
}
static void load64_le_buf (u64 *dst, const u8 *src, size_t size) {
	FOR(i, 0, size) { dst[i] = load64_le(src + i*8); }
}
static void store32_le_buf(u8 *dst, const u32 *src, size_t size) {
	FOR(i, 0, size) { store32_le(dst + i*4, src[i]); }
}
static void store64_le_buf(u8 *dst, const u64 *src, size_t size) {
	FOR(i, 0, size) { store64_le(dst + i*8, src[i]); }
}

static u64 rotr64(u64 x, u64 n) { return (x >> n) ^ (x << (64 - n)); }
static u32 rotl32(u32 x, u32 n) { return (x << n) ^ (x >> (32 - n)); }

static int neq0(u64 diff)
{
	// constant time comparison to zero
	// return diff != 0 ? -1 : 0
	u64 half = (diff >> 32) | ((u32)diff);
	return (1 & ((half - 1) >> 32)) - 1;
}

static u64 x16(const u8 a[16], const u8 b[16])
{
	return (load64_le(a + 0) ^ load64_le(b + 0))
		|  (load64_le(a + 8) ^ load64_le(b + 8));
}
static u64 x32(const u8 a[32],const u8 b[32]){return x16(a,b)| x16(a+16, b+16);}
static u64 x64(const u8 a[64],const u8 b[64]){return x32(a,b)| x32(a+32, b+32);}
int crypto_verify16(const u8 a[16], const u8 b[16]){ return neq0(x16(a, b)); }
int crypto_verify32(const u8 a[32], const u8 b[32]){ return neq0(x32(a, b)); }
int crypto_verify64(const u8 a[64], const u8 b[64]){ return neq0(x64(a, b)); }

void crypto_wipe(void *secret, size_t size)
{
	volatile u8 *v_secret = (u8*)secret;
	ZERO(v_secret, size);
}

/////////////////
/// Chacha 20 ///
/////////////////
#define QUARTERROUND(a, b, c, d)	\
	a += b;  d = rotl32(d ^ a, 16); \
	c += d;  b = rotl32(b ^ c, 12); \
	a += b;  d = rotl32(d ^ a,  8); \
	c += d;  b = rotl32(b ^ c,  7)

static void chacha20_rounds(u32 out[16], const u32 in[16])
{
	// The temporary variables make Chacha20 10% faster.
	u32 t0  = in[ 0];  u32 t1  = in[ 1];  u32 t2  = in[ 2];  u32 t3  = in[ 3];
	u32 t4  = in[ 4];  u32 t5  = in[ 5];  u32 t6  = in[ 6];  u32 t7  = in[ 7];
	u32 t8  = in[ 8];  u32 t9  = in[ 9];  u32 t10 = in[10];  u32 t11 = in[11];
	u32 t12 = in[12];  u32 t13 = in[13];  u32 t14 = in[14];  u32 t15 = in[15];

	FOR (i, 0, 10) { // 20 rounds, 2 rounds per loop.
		QUARTERROUND(t0, t4, t8 , t12); // column 0
		QUARTERROUND(t1, t5, t9 , t13); // column 1
		QUARTERROUND(t2, t6, t10, t14); // column 2
		QUARTERROUND(t3, t7, t11, t15); // column 3
		QUARTERROUND(t0, t5, t10, t15); // diagonal 0
		QUARTERROUND(t1, t6, t11, t12); // diagonal 1
		QUARTERROUND(t2, t7, t8 , t13); // diagonal 2
		QUARTERROUND(t3, t4, t9 , t14); // diagonal 3
	}
	out[ 0] = t0;   out[ 1] = t1;   out[ 2] = t2;   out[ 3] = t3;
	out[ 4] = t4;   out[ 5] = t5;   out[ 6] = t6;   out[ 7] = t7;
	out[ 8] = t8;   out[ 9] = t9;   out[10] = t10;  out[11] = t11;
	out[12] = t12;  out[13] = t13;  out[14] = t14;  out[15] = t15;
}

static const u8 *chacha20_constant = (const u8*)"expand 32-byte k"; // 16 bytes

void crypto_chacha20_h(u8 out[32], const u8 key[32], const u8 in [16])
{
	u32 block[16];
	load32_le_buf(block     , chacha20_constant, 4);
	load32_le_buf(block +  4, key              , 8);
	load32_le_buf(block + 12, in               , 4);

	chacha20_rounds(block, block);

	// prevent reversal of the rounds by revealing only half of the buffer.
	store32_le_buf(out   , block   , 4); // constant
	store32_le_buf(out+16, block+12, 4); // counter and nonce
	WIPE_BUFFER(block);
}

u64 crypto_chacha20_djb(u8 *cipher_text, const u8 *plain_text,
                        size_t text_size, const u8 key[32], const u8 nonce[8],
                        u64 ctr)
{
	u32 input[16];
	load32_le_buf(input     , chacha20_constant, 4);
	load32_le_buf(input +  4, key              , 8);
	load32_le_buf(input + 14, nonce            , 2);
	input[12] = (u32) ctr;
	input[13] = (u32)(ctr >> 32);

	// Whole blocks
	u32    pool[16];
	size_t nb_blocks = text_size >> 6;
	FOR (i, 0, nb_blocks) {
		chacha20_rounds(pool, input);
		if (plain_text != 0) {
			FOR (j, 0, 16) {
				u32 p = pool[j] + input[j];
				store32_le(cipher_text, p ^ load32_le(plain_text));
				cipher_text += 4;
				plain_text  += 4;
			}
		} else {
			FOR (j, 0, 16) {
				u32 p = pool[j] + input[j];
				store32_le(cipher_text, p);
				cipher_text += 4;
			}
		}
		input[12]++;
		if (input[12] == 0) {
			input[13]++;
		}
	}
	text_size &= 63;

	// Last (incomplete) block
	if (text_size > 0) {
		if (plain_text == 0) {
			plain_text = zero;
		}
		chacha20_rounds(pool, input);
		u8 tmp[64];
		FOR (i, 0, 16) {
			store32_le(tmp + i*4, pool[i] + input[i]);
		}
		FOR (i, 0, text_size) {
			cipher_text[i] = tmp[i] ^ plain_text[i];
		}
		WIPE_BUFFER(tmp);
	}
	ctr = input[12] + ((u64)input[13] << 32) + (text_size > 0);

	WIPE_BUFFER(pool);
	WIPE_BUFFER(input);
	return ctr;
}

u32 crypto_chacha20_ietf(u8 *cipher_text, const u8 *plain_text,
                         size_t text_size,
                         const u8 key[32], const u8 nonce[12], u32 ctr)
{
	u64 big_ctr = ctr + ((u64)load32_le(nonce) << 32);
	return (u32)crypto_chacha20_djb(cipher_text, plain_text, text_size,
	                                key, nonce + 4, big_ctr);
}

u64 crypto_chacha20_x(u8 *cipher_text, const u8 *plain_text,
                      size_t text_size,
                      const u8 key[32], const u8 nonce[24], u64 ctr)
{
	u8 sub_key[32];
	crypto_chacha20_h(sub_key, key, nonce);
	ctr = crypto_chacha20_djb(cipher_text, plain_text, text_size,
	                          sub_key, nonce + 16, ctr);
	WIPE_BUFFER(sub_key);
	return ctr;
}

/////////////////
/// Poly 1305 ///
/////////////////

// h = (h + c) * r
// preconditions:
//   ctx->h <= 4_ffffffff_ffffffff_ffffffff_ffffffff
//   ctx->r <=   0ffffffc_0ffffffc_0ffffffc_0fffffff
//   end    <= 1
// Postcondition:
//   ctx->h <= 4_ffffffff_ffffffff_ffffffff_ffffffff
static void poly_blocks(crypto_poly1305_ctx *ctx, const u8 *in,
                        size_t nb_blocks, unsigned end)
{
	// Local all the things!
	const u32 r0 = ctx->r[0];
	const u32 r1 = ctx->r[1];
	const u32 r2 = ctx->r[2];
	const u32 r3 = ctx->r[3];
	const u32 rr0 = (r0 >> 2) * 5;  // lose 2 bits...
	const u32 rr1 = (r1 >> 2) + r1; // rr1 == (r1 >> 2) * 5
	const u32 rr2 = (r2 >> 2) + r2; // rr1 == (r2 >> 2) * 5
	const u32 rr3 = (r3 >> 2) + r3; // rr1 == (r3 >> 2) * 5
	const u32 rr4 = r0 & 3;         // ...recover 2 bits
	u32 h0 = ctx->h[0];
	u32 h1 = ctx->h[1];
	u32 h2 = ctx->h[2];
	u32 h3 = ctx->h[3];
	u32 h4 = ctx->h[4];

	FOR (i, 0, nb_blocks) {
		// h + c, without carry propagation
		const u64 s0 = (u64)h0 + load32_le(in);  in += 4;
		const u64 s1 = (u64)h1 + load32_le(in);  in += 4;
		const u64 s2 = (u64)h2 + load32_le(in);  in += 4;
		const u64 s3 = (u64)h3 + load32_le(in);  in += 4;
		const u32 s4 =      h4 + end;

		// (h + c) * r, without carry propagation
		const u64 x0 = s0*r0+ s1*rr3+ s2*rr2+ s3*rr1+ s4*rr0;
		const u64 x1 = s0*r1+ s1*r0 + s2*rr3+ s3*rr2+ s4*rr1;
		const u64 x2 = s0*r2+ s1*r1 + s2*r0 + s3*rr3+ s4*rr2;
		const u64 x3 = s0*r3+ s1*r2 + s2*r1 + s3*r0 + s4*rr3;
		const u32 x4 =                                s4*rr4;

		// partial reduction modulo 2^130 - 5
		const u32 u5 = x4 + (x3 >> 32); // u5 <= 7ffffff5
		const u64 u0 = (u5 >>  2) * 5 + (x0 & 0xffffffff);
		const u64 u1 = (u0 >> 32)     + (x1 & 0xffffffff) + (x0 >> 32);
		const u64 u2 = (u1 >> 32)     + (x2 & 0xffffffff) + (x1 >> 32);
		const u64 u3 = (u2 >> 32)     + (x3 & 0xffffffff) + (x2 >> 32);
		const u32 u4 = (u3 >> 32)     + (u5 & 3); // u4 <= 4

		// Update the hash
		h0 = u0 & 0xffffffff;
		h1 = u1 & 0xffffffff;
		h2 = u2 & 0xffffffff;
		h3 = u3 & 0xffffffff;
		h4 = u4;
	}
	ctx->h[0] = h0;
	ctx->h[1] = h1;
	ctx->h[2] = h2;
	ctx->h[3] = h3;
	ctx->h[4] = h4;
}

void crypto_poly1305_init(crypto_poly1305_ctx *ctx, const u8 key[32])
{
	ZERO(ctx->h, 5); // Initial hash is zero
	ctx->c_idx = 0;
	// load r and pad (r has some of its bits cleared)
	load32_le_buf(ctx->r  , key   , 4);
	load32_le_buf(ctx->pad, key+16, 4);
	FOR (i, 0, 1) { ctx->r[i] &= 0x0fffffff; }
	FOR (i, 1, 4) { ctx->r[i] &= 0x0ffffffc; }
}

void crypto_poly1305_update(crypto_poly1305_ctx *ctx,
                            const u8 *message, size_t message_size)
{
	// Avoid undefined NULL pointer increments with empty messages
	if (message_size == 0) {
		return;
	}

	// Align ourselves with block boundaries
	size_t aligned = MIN(gap(ctx->c_idx, 16), message_size);
	FOR (i, 0, aligned) {
		ctx->c[ctx->c_idx] = *message;
		ctx->c_idx++;
		message++;
		message_size--;
	}

	// If block is complete, process it
	if (ctx->c_idx == 16) {
		poly_blocks(ctx, ctx->c, 1, 1);
		ctx->c_idx = 0;
	}

	// Process the message block by block
	size_t nb_blocks = message_size >> 4;
	poly_blocks(ctx, message, nb_blocks, 1);
	message      += nb_blocks << 4;
	message_size &= 15;

	// remaining bytes (we never complete a block here)
	FOR (i, 0, message_size) {
		ctx->c[ctx->c_idx] = message[i];
		ctx->c_idx++;
	}
}

void crypto_poly1305_final(crypto_poly1305_ctx *ctx, u8 mac[16])
{
	// Process the last block (if any)
	// We move the final 1 according to remaining input length
	// (this will add less than 2^130 to the last input block)
	if (ctx->c_idx != 0) {
		ZERO(ctx->c + ctx->c_idx, 16 - ctx->c_idx);
		ctx->c[ctx->c_idx] = 1;
		poly_blocks(ctx, ctx->c, 1, 0);
	}

	// check if we should subtract 2^130-5 by performing the
	// corresponding carry propagation.
	u64 c = 5;
	FOR (i, 0, 4) {
		c  += ctx->h[i];
		c >>= 32;
	}
	c += ctx->h[4];
	c  = (c >> 2) * 5; // shift the carry back to the beginning
	// c now indicates how many times we should subtract 2^130-5 (0 or 1)
	FOR (i, 0, 4) {
		c += (u64)ctx->h[i] + ctx->pad[i];
		store32_le(mac + i*4, (u32)c);
		c = c >> 32;
	}
	WIPE_CTX(ctx);
}

void crypto_poly1305(u8     mac[16],  const u8 *message,
                     size_t message_size, const u8  key[32])
{
	crypto_poly1305_ctx ctx;
	crypto_poly1305_init  (&ctx, key);
	crypto_poly1305_update(&ctx, message, message_size);
	crypto_poly1305_final (&ctx, mac);
}

////////////////
/// BLAKE2 b ///
////////////////
static const u64 iv[8] = {
	0x6a09e667f3bcc908, 0xbb67ae8584caa73b,
	0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
	0x510e527fade682d1, 0x9b05688c2b3e6c1f,
	0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
};

static void blake2b_compress(crypto_blake2b_ctx *ctx, int is_last_block)
{
	static const u8 sigma[12][16] = {
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
		{ 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
		{ 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
		{  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
		{  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
		{  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
		{ 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
		{ 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
		{  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
		{ 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
		{ 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
	};

	// increment input offset
	u64   *x = ctx->input_offset;
	size_t y = ctx->input_idx;
	x[0] += y;
	if (x[0] < y) {
		x[1]++;
	}

	// init work vector
	u64 v0 = ctx->hash[0];  u64 v8  = iv[0];
	u64 v1 = ctx->hash[1];  u64 v9  = iv[1];
	u64 v2 = ctx->hash[2];  u64 v10 = iv[2];
	u64 v3 = ctx->hash[3];  u64 v11 = iv[3];
	u64 v4 = ctx->hash[4];  u64 v12 = iv[4] ^ ctx->input_offset[0];
	u64 v5 = ctx->hash[5];  u64 v13 = iv[5] ^ ctx->input_offset[1];
	u64 v6 = ctx->hash[6];  u64 v14 = iv[6] ^ (u64)~(is_last_block - 1);
	u64 v7 = ctx->hash[7];  u64 v15 = iv[7];

	// mangle work vector
	u64 *input = ctx->input;
#define BLAKE2_G(a, b, c, d, x, y)	\
	a += b + x;  d = rotr64(d ^ a, 32); \
	c += d;      b = rotr64(b ^ c, 24); \
	a += b + y;  d = rotr64(d ^ a, 16); \
	c += d;      b = rotr64(b ^ c, 63)
#define BLAKE2_ROUND(i)	\
	BLAKE2_G(v0, v4, v8 , v12, input[sigma[i][ 0]], input[sigma[i][ 1]]); \
	BLAKE2_G(v1, v5, v9 , v13, input[sigma[i][ 2]], input[sigma[i][ 3]]); \
	BLAKE2_G(v2, v6, v10, v14, input[sigma[i][ 4]], input[sigma[i][ 5]]); \
	BLAKE2_G(v3, v7, v11, v15, input[sigma[i][ 6]], input[sigma[i][ 7]]); \
	BLAKE2_G(v0, v5, v10, v15, input[sigma[i][ 8]], input[sigma[i][ 9]]); \
	BLAKE2_G(v1, v6, v11, v12, input[sigma[i][10]], input[sigma[i][11]]); \
	BLAKE2_G(v2, v7, v8 , v13, input[sigma[i][12]], input[sigma[i][13]]); \
	BLAKE2_G(v3, v4, v9 , v14, input[sigma[i][14]], input[sigma[i][15]])

#ifdef BLAKE2_NO_UNROLLING
	FOR (i, 0, 12) {
		BLAKE2_ROUND(i);
	}
#else
	BLAKE2_ROUND(0);  BLAKE2_ROUND(1);  BLAKE2_ROUND(2);  BLAKE2_ROUND(3);
	BLAKE2_ROUND(4);  BLAKE2_ROUND(5);  BLAKE2_ROUND(6);  BLAKE2_ROUND(7);
	BLAKE2_ROUND(8);  BLAKE2_ROUND(9);  BLAKE2_ROUND(10); BLAKE2_ROUND(11);
#endif

	// update hash
	ctx->hash[0] ^= v0 ^ v8;   ctx->hash[1] ^= v1 ^ v9;
	ctx->hash[2] ^= v2 ^ v10;  ctx->hash[3] ^= v3 ^ v11;
	ctx->hash[4] ^= v4 ^ v12;  ctx->hash[5] ^= v5 ^ v13;
	ctx->hash[6] ^= v6 ^ v14;  ctx->hash[7] ^= v7 ^ v15;
}

void crypto_blake2b_keyed_init(crypto_blake2b_ctx *ctx, size_t hash_size,
                               const u8 *key, size_t key_size)
{
	// initial hash
	COPY(ctx->hash, iv, 8);
	ctx->hash[0] ^= 0x01010000 ^ (key_size << 8) ^ hash_size;

	ctx->input_offset[0] = 0;  // beginning of the input, no offset
	ctx->input_offset[1] = 0;  // beginning of the input, no offset
	ctx->hash_size       = hash_size;
	ctx->input_idx       = 0;
	ZERO(ctx->input, 16);

	// if there is a key, the first block is that key (padded with zeroes)
	if (key_size > 0) {
		u8 key_block[128] = {0};
		COPY(key_block, key, key_size);
		// same as calling crypto_blake2b_update(ctx, key_block , 128)
		load64_le_buf(ctx->input, key_block, 16);
		ctx->input_idx = 128;
	}
}

void crypto_blake2b_init(crypto_blake2b_ctx *ctx, size_t hash_size)
{
	crypto_blake2b_keyed_init(ctx, hash_size, 0, 0);
}

void crypto_blake2b_update(crypto_blake2b_ctx *ctx,
                           const u8 *message, size_t message_size)
{
	// Avoid undefined NULL pointer increments with empty messages
	if (message_size == 0) {
		return;
	}

	// Align with word boundaries
	if ((ctx->input_idx & 7) != 0) {
		size_t nb_bytes = MIN(gap(ctx->input_idx, 8), message_size);
		size_t word     = ctx->input_idx >> 3;
		size_t byte     = ctx->input_idx & 7;
		FOR (i, 0, nb_bytes) {
			ctx->input[word] |= (u64)message[i] << ((byte + i) << 3);
		}
		ctx->input_idx += nb_bytes;
		message        += nb_bytes;
		message_size   -= nb_bytes;
	}

	// Align with block boundaries (faster than byte by byte)
	if ((ctx->input_idx & 127) != 0) {
		size_t nb_words = MIN(gap(ctx->input_idx, 128), message_size) >> 3;
		load64_le_buf(ctx->input + (ctx->input_idx >> 3), message, nb_words);
		ctx->input_idx += nb_words << 3;
		message        += nb_words << 3;
		message_size   -= nb_words << 3;
	}

	// Process block by block
	size_t nb_blocks = message_size >> 7;
	FOR (i, 0, nb_blocks) {
		if (ctx->input_idx == 128) {
			blake2b_compress(ctx, 0);
		}
		load64_le_buf(ctx->input, message, 16);
		message += 128;
		ctx->input_idx = 128;
	}
	message_size &= 127;

	if (message_size != 0) {
		// Compress block & flush input buffer as needed
		if (ctx->input_idx == 128) {
			blake2b_compress(ctx, 0);
			ctx->input_idx = 0;
		}
		if (ctx->input_idx == 0) {
			ZERO(ctx->input, 16);
		}
		// Fill remaining words (faster than byte by byte)
		size_t nb_words = message_size >> 3;
		load64_le_buf(ctx->input, message, nb_words);
		ctx->input_idx += nb_words << 3;
		message        += nb_words << 3;
		message_size   -= nb_words << 3;

		// Fill remaining bytes
		FOR (i, 0, message_size) {
			size_t word = ctx->input_idx >> 3;
			size_t byte = ctx->input_idx & 7;
			ctx->input[word] |= (u64)message[i] << (byte << 3);
			ctx->input_idx++;
		}
	}
}

void crypto_blake2b_final(crypto_blake2b_ctx *ctx, u8 *hash)
{
	blake2b_compress(ctx, 1); // compress the last block
	size_t hash_size = MIN(ctx->hash_size, 64);
	size_t nb_words  = hash_size >> 3;
	store64_le_buf(hash, ctx->hash, nb_words);
	FOR (i, nb_words << 3, hash_size) {
		hash[i] = (ctx->hash[i >> 3] >> (8 * (i & 7))) & 0xff;
	}
	WIPE_CTX(ctx);
}

void crypto_blake2b_keyed(u8 *hash,          size_t hash_size,
                          const u8 *key,     size_t key_size,
                          const u8 *message, size_t message_size)
{
	crypto_blake2b_ctx ctx;
	crypto_blake2b_keyed_init(&ctx, hash_size, key, key_size);
	crypto_blake2b_update    (&ctx, message, message_size);
	crypto_blake2b_final     (&ctx, hash);
}

void crypto_blake2b(u8 *hash, size_t hash_size, const u8 *msg, size_t msg_size)
{
	crypto_blake2b_keyed(hash, hash_size, 0, 0, msg, msg_size);
}

//////////////
////////////////////////////////////
/// Arithmetic modulo 2^255 - 19 ///
////////////////////////////////////
//  Originally taken from SUPERCOP's ref10 implementation.
//  A bit bigger than TweetNaCl, over 4 times faster.

// field element
typedef i32 fe[10];

// field constants
//
// sqrtm1      : sqrt(-1)
static const fe sqrtm1  = {
	-32595792, -7943725, 9377950, 3500415, 12389472,
	-272473, -25146209, -2005654, 326686, 11406482,
};

static void fe_0(fe h) {           ZERO(h  , 10); }
static void fe_1(fe h) { h[0] = 1; ZERO(h+1,  9); }

static void fe_copy(fe h,const fe f           ){FOR(i,0,10) h[i] =  f[i];      }
static void fe_neg (fe h,const fe f           ){FOR(i,0,10) h[i] = -f[i];      }
static void fe_add (fe h,const fe f,const fe g){FOR(i,0,10) h[i] = f[i] + g[i];}
static void fe_sub (fe h,const fe f,const fe g){FOR(i,0,10) h[i] = f[i] - g[i];}

static void fe_cswap(fe f, fe g, int b)
{
	i32 mask = -b; // -1 = 0xffffffff
	FOR (i, 0, 10) {
		i32 x = (f[i] ^ g[i]) & mask;
		f[i] = f[i] ^ x;
		g[i] = g[i] ^ x;
	}
}

static void fe_ccopy(fe f, const fe g, int b)
{
	i32 mask = -b; // -1 = 0xffffffff
	FOR (i, 0, 10) {
		i32 x = (f[i] ^ g[i]) & mask;
		f[i] = f[i] ^ x;
	}
}


// Signed carry propagation
// ------------------------
//
// Let t be a number.  It can be uniquely decomposed thus:
//
//    t = h*2^26 + l
//    such that -2^25 <= l < 2^25
//
// Let c = (t + 2^25) / 2^26            (rounded down)
//     c = (h*2^26 + l + 2^25) / 2^26   (rounded down)
//     c =  h   +   (l + 2^25) / 2^26   (rounded down)
//     c =  h                           (exactly)
// Because 0 <= l + 2^25 < 2^26
//
// Let u = t          - c*2^26
//     u = h*2^26 + l - h*2^26
//     u = l
// Therefore, -2^25 <= u < 2^25
//
// Additionally, if |t| < x, then |h| < x/2^26 (rounded down)
//
// Notations:
// - In C, 1<<25 means 2^25.
// - In C, x>>25 means floor(x / (2^25)).
// - All of the above applies with 25 & 24 as well as 26 & 25.
//
//
// Note on negative right shifts
// -----------------------------
//
// In C, x >> n, where x is a negative integer, is implementation
// defined.  In practice, all platforms do arithmetic shift, which is
// equivalent to division by 2^26, rounded down.  Some compilers, like
// GCC, even guarantee it.
//
// If we ever stumble upon a platform that does not propagate the sign
// bit (we won't), visible failures will show at the slightest test, and
// the signed shifts can be replaced by the following:
//
//     typedef struct { i64 x:39; } s25;
//     typedef struct { i64 x:38; } s26;
//     i64 shift25(i64 x) { s25 s; s.x = ((u64)x)>>25; return s.x; }
//     i64 shift26(i64 x) { s26 s; s.x = ((u64)x)>>26; return s.x; }
//
// Current compilers cannot optimise this, causing a 30% drop in
// performance.  Fairly expensive for something that never happens.
//
//
// Precondition
// ------------
//
// |t0|       < 2^63
// |t1|..|t9| < 2^62
//
// Algorithm
// ---------
// c   = t0 + 2^25 / 2^26   -- |c|  <= 2^36
// t0 -= c * 2^26           -- |t0| <= 2^25
// t1 += c                  -- |t1| <= 2^63
//
// c   = t4 + 2^25 / 2^26   -- |c|  <= 2^36
// t4 -= c * 2^26           -- |t4| <= 2^25
// t5 += c                  -- |t5| <= 2^63
//
// c   = t1 + 2^24 / 2^25   -- |c|  <= 2^38
// t1 -= c * 2^25           -- |t1| <= 2^24
// t2 += c                  -- |t2| <= 2^63
//
// c   = t5 + 2^24 / 2^25   -- |c|  <= 2^38
// t5 -= c * 2^25           -- |t5| <= 2^24
// t6 += c                  -- |t6| <= 2^63
//
// c   = t2 + 2^25 / 2^26   -- |c|  <= 2^37
// t2 -= c * 2^26           -- |t2| <= 2^25        < 1.1 * 2^25  (final t2)
// t3 += c                  -- |t3| <= 2^63
//
// c   = t6 + 2^25 / 2^26   -- |c|  <= 2^37
// t6 -= c * 2^26           -- |t6| <= 2^25        < 1.1 * 2^25  (final t6)
// t7 += c                  -- |t7| <= 2^63
//
// c   = t3 + 2^24 / 2^25   -- |c|  <= 2^38
// t3 -= c * 2^25           -- |t3| <= 2^24        < 1.1 * 2^24  (final t3)
// t4 += c                  -- |t4| <= 2^25 + 2^38 < 2^39
//
// c   = t7 + 2^24 / 2^25   -- |c|  <= 2^38
// t7 -= c * 2^25           -- |t7| <= 2^24        < 1.1 * 2^24  (final t7)
// t8 += c                  -- |t8| <= 2^63
//
// c   = t4 + 2^25 / 2^26   -- |c|  <= 2^13
// t4 -= c * 2^26           -- |t4| <= 2^25        < 1.1 * 2^25  (final t4)
// t5 += c                  -- |t5| <= 2^24 + 2^13 < 1.1 * 2^24  (final t5)
//
// c   = t8 + 2^25 / 2^26   -- |c|  <= 2^37
// t8 -= c * 2^26           -- |t8| <= 2^25        < 1.1 * 2^25  (final t8)
// t9 += c                  -- |t9| <= 2^63
//
// c   = t9 + 2^24 / 2^25   -- |c|  <= 2^38
// t9 -= c * 2^25           -- |t9| <= 2^24        < 1.1 * 2^24  (final t9)
// t0 += c * 19             -- |t0| <= 2^25 + 2^38*19 < 2^44
//
// c   = t0 + 2^25 / 2^26   -- |c|  <= 2^18
// t0 -= c * 2^26           -- |t0| <= 2^25        < 1.1 * 2^25  (final t0)
// t1 += c                  -- |t1| <= 2^24 + 2^18 < 1.1 * 2^24  (final t1)
//
// Postcondition
// -------------
//   |t0|, |t2|, |t4|, |t6|, |t8|  <  1.1 * 2^25
//   |t1|, |t3|, |t5|, |t7|, |t9|  <  1.1 * 2^24
#define FE_CARRY	\
	i64 c; \
	c = (t0 + ((i64)1<<25)) >> 26;  t0 -= c * ((i64)1 << 26);  t1 += c; \
	c = (t4 + ((i64)1<<25)) >> 26;  t4 -= c * ((i64)1 << 26);  t5 += c; \
	c = (t1 + ((i64)1<<24)) >> 25;  t1 -= c * ((i64)1 << 25);  t2 += c; \
	c = (t5 + ((i64)1<<24)) >> 25;  t5 -= c * ((i64)1 << 25);  t6 += c; \
	c = (t2 + ((i64)1<<25)) >> 26;  t2 -= c * ((i64)1 << 26);  t3 += c; \
	c = (t6 + ((i64)1<<25)) >> 26;  t6 -= c * ((i64)1 << 26);  t7 += c; \
	c = (t3 + ((i64)1<<24)) >> 25;  t3 -= c * ((i64)1 << 25);  t4 += c; \
	c = (t7 + ((i64)1<<24)) >> 25;  t7 -= c * ((i64)1 << 25);  t8 += c; \
	c = (t4 + ((i64)1<<25)) >> 26;  t4 -= c * ((i64)1 << 26);  t5 += c; \
	c = (t8 + ((i64)1<<25)) >> 26;  t8 -= c * ((i64)1 << 26);  t9 += c; \
	c = (t9 + ((i64)1<<24)) >> 25;  t9 -= c * ((i64)1 << 25);  t0 += c * 19; \
	c = (t0 + ((i64)1<<25)) >> 26;  t0 -= c * ((i64)1 << 26);  t1 += c; \
	h[0]=(i32)t0;  h[1]=(i32)t1;  h[2]=(i32)t2;  h[3]=(i32)t3;  h[4]=(i32)t4; \
	h[5]=(i32)t5;  h[6]=(i32)t6;  h[7]=(i32)t7;  h[8]=(i32)t8;  h[9]=(i32)t9

// Decodes a field element from a byte buffer.
// mask specifies how many bits we ignore.
// Traditionally we ignore 1. It's useful for EdDSA,
// which uses that bit to denote the sign of x.
// Elligator however uses positive representatives,
// which means ignoring 2 bits instead.
static void fe_frombytes_mask(fe h, const u8 s[32], unsigned nb_mask)
{
	u32 mask = 0xffffff >> nb_mask;
	i64 t0 =  load32_le(s);                    // t0 < 2^32
	i64 t1 =  load24_le(s +  4) << 6;          // t1 < 2^30
	i64 t2 =  load24_le(s +  7) << 5;          // t2 < 2^29
	i64 t3 =  load24_le(s + 10) << 3;          // t3 < 2^27
	i64 t4 =  load24_le(s + 13) << 2;          // t4 < 2^26
	i64 t5 =  load32_le(s + 16);               // t5 < 2^32
	i64 t6 =  load24_le(s + 20) << 7;          // t6 < 2^31
	i64 t7 =  load24_le(s + 23) << 5;          // t7 < 2^29
	i64 t8 =  load24_le(s + 26) << 4;          // t8 < 2^28
	i64 t9 = (load24_le(s + 29) & mask) << 2;  // t9 < 2^25
	FE_CARRY;                                  // Carry precondition OK
}

static void fe_frombytes(fe h, const u8 s[32])
{
	fe_frombytes_mask(h, s, 1);
}


// Precondition
//   |h[0]|, |h[2]|, |h[4]|, |h[6]|, |h[8]|  <  1.1 * 2^25
//   |h[1]|, |h[3]|, |h[5]|, |h[7]|, |h[9]|  <  1.1 * 2^24
//
// Therefore, |h| < 2^255-19
// There are two possibilities:
//
// - If h is positive, all we need to do is reduce its individual
//   limbs down to their tight positive range.
// - If h is negative, we also need to add 2^255-19 to it.
//   Or just remove 19 and chop off any excess bit.
static void fe_tobytes(u8 s[32], const fe h)
{
	i32 t[10];
	COPY(t, h, 10);
	i32 q = (19 * t[9] + (((i32) 1) << 24)) >> 25;
	//                 |t9|                    < 1.1 * 2^24
	//  -1.1 * 2^24  <  t9                     < 1.1 * 2^24
	//  -21  * 2^24  <  19 * t9                < 21  * 2^24
	//  -2^29        <  19 * t9 + 2^24         < 2^29
	//  -2^29 / 2^25 < (19 * t9 + 2^24) / 2^25 < 2^29 / 2^25
	//  -16          < (19 * t9 + 2^24) / 2^25 < 16
	FOR (i, 0, 5) {
		q += t[2*i  ]; q >>= 26; // q = 0 or -1
		q += t[2*i+1]; q >>= 25; // q = 0 or -1
	}
	// q =  0 iff h >= 0
	// q = -1 iff h <  0
	// Adding q * 19 to h reduces h to its proper range.
	q *= 19;  // Shift carry back to the beginning
	FOR (i, 0, 5) {
		t[i*2  ] += q;  q = t[i*2  ] >> 26;  t[i*2  ] -= q * ((i32)1 << 26);
		t[i*2+1] += q;  q = t[i*2+1] >> 25;  t[i*2+1] -= q * ((i32)1 << 25);
	}
	// h is now fully reduced, and q represents the excess bit.

	store32_le(s +  0, ((u32)t[0] >>  0) | ((u32)t[1] << 26));
	store32_le(s +  4, ((u32)t[1] >>  6) | ((u32)t[2] << 19));
	store32_le(s +  8, ((u32)t[2] >> 13) | ((u32)t[3] << 13));
	store32_le(s + 12, ((u32)t[3] >> 19) | ((u32)t[4] <<  6));
	store32_le(s + 16, ((u32)t[5] >>  0) | ((u32)t[6] << 25));
	store32_le(s + 20, ((u32)t[6] >>  7) | ((u32)t[7] << 19));
	store32_le(s + 24, ((u32)t[7] >> 13) | ((u32)t[8] << 12));
	store32_le(s + 28, ((u32)t[8] >> 20) | ((u32)t[9] <<  6));

	WIPE_BUFFER(t);
}

// Precondition
// -------------
//   |f0|, |f2|, |f4|, |f6|, |f8|  <  1.65 * 2^26
//   |f1|, |f3|, |f5|, |f7|, |f9|  <  1.65 * 2^25
//
//   |g0|, |g2|, |g4|, |g6|, |g8|  <  1.65 * 2^26
//   |g1|, |g3|, |g5|, |g7|, |g9|  <  1.65 * 2^25
static void fe_mul_small(fe h, const fe f, i32 g)
{
	i64 t0 = f[0] * (i64) g;  i64 t1 = f[1] * (i64) g;
	i64 t2 = f[2] * (i64) g;  i64 t3 = f[3] * (i64) g;
	i64 t4 = f[4] * (i64) g;  i64 t5 = f[5] * (i64) g;
	i64 t6 = f[6] * (i64) g;  i64 t7 = f[7] * (i64) g;
	i64 t8 = f[8] * (i64) g;  i64 t9 = f[9] * (i64) g;
	// |t0|, |t2|, |t4|, |t6|, |t8|  <  1.65 * 2^26 * 2^31  < 2^58
	// |t1|, |t3|, |t5|, |t7|, |t9|  <  1.65 * 2^25 * 2^31  < 2^57

	FE_CARRY; // Carry precondition OK
}

// Precondition
// -------------
//   |f0|, |f2|, |f4|, |f6|, |f8|  <  1.65 * 2^26
//   |f1|, |f3|, |f5|, |f7|, |f9|  <  1.65 * 2^25
//
//   |g0|, |g2|, |g4|, |g6|, |g8|  <  1.65 * 2^26
//   |g1|, |g3|, |g5|, |g7|, |g9|  <  1.65 * 2^25
static void fe_mul(fe h, const fe f, const fe g)
{
	// Everything is unrolled and put in temporary variables.
	// We could roll the loop, but that would make curve25519 twice as slow.
	i32 f0 = f[0]; i32 f1 = f[1]; i32 f2 = f[2]; i32 f3 = f[3]; i32 f4 = f[4];
	i32 f5 = f[5]; i32 f6 = f[6]; i32 f7 = f[7]; i32 f8 = f[8]; i32 f9 = f[9];
	i32 g0 = g[0]; i32 g1 = g[1]; i32 g2 = g[2]; i32 g3 = g[3]; i32 g4 = g[4];
	i32 g5 = g[5]; i32 g6 = g[6]; i32 g7 = g[7]; i32 g8 = g[8]; i32 g9 = g[9];
	i32 F1 = f1*2; i32 F3 = f3*2; i32 F5 = f5*2; i32 F7 = f7*2; i32 F9 = f9*2;
	i32 G1 = g1*19;  i32 G2 = g2*19;  i32 G3 = g3*19;
	i32 G4 = g4*19;  i32 G5 = g5*19;  i32 G6 = g6*19;
	i32 G7 = g7*19;  i32 G8 = g8*19;  i32 G9 = g9*19;
	// |F1|, |F3|, |F5|, |F7|, |F9|  <  1.65 * 2^26
	// |G0|, |G2|, |G4|, |G6|, |G8|  <  2^31
	// |G1|, |G3|, |G5|, |G7|, |G9|  <  2^30

	i64 t0 = f0*(i64)g0 + F1*(i64)G9 + f2*(i64)G8 + F3*(i64)G7 + f4*(i64)G6
	       + F5*(i64)G5 + f6*(i64)G4 + F7*(i64)G3 + f8*(i64)G2 + F9*(i64)G1;
	i64 t1 = f0*(i64)g1 + f1*(i64)g0 + f2*(i64)G9 + f3*(i64)G8 + f4*(i64)G7
	       + f5*(i64)G6 + f6*(i64)G5 + f7*(i64)G4 + f8*(i64)G3 + f9*(i64)G2;
	i64 t2 = f0*(i64)g2 + F1*(i64)g1 + f2*(i64)g0 + F3*(i64)G9 + f4*(i64)G8
	       + F5*(i64)G7 + f6*(i64)G6 + F7*(i64)G5 + f8*(i64)G4 + F9*(i64)G3;
	i64 t3 = f0*(i64)g3 + f1*(i64)g2 + f2*(i64)g1 + f3*(i64)g0 + f4*(i64)G9
	       + f5*(i64)G8 + f6*(i64)G7 + f7*(i64)G6 + f8*(i64)G5 + f9*(i64)G4;
	i64 t4 = f0*(i64)g4 + F1*(i64)g3 + f2*(i64)g2 + F3*(i64)g1 + f4*(i64)g0
	       + F5*(i64)G9 + f6*(i64)G8 + F7*(i64)G7 + f8*(i64)G6 + F9*(i64)G5;
	i64 t5 = f0*(i64)g5 + f1*(i64)g4 + f2*(i64)g3 + f3*(i64)g2 + f4*(i64)g1
	       + f5*(i64)g0 + f6*(i64)G9 + f7*(i64)G8 + f8*(i64)G7 + f9*(i64)G6;
	i64 t6 = f0*(i64)g6 + F1*(i64)g5 + f2*(i64)g4 + F3*(i64)g3 + f4*(i64)g2
	       + F5*(i64)g1 + f6*(i64)g0 + F7*(i64)G9 + f8*(i64)G8 + F9*(i64)G7;
	i64 t7 = f0*(i64)g7 + f1*(i64)g6 + f2*(i64)g5 + f3*(i64)g4 + f4*(i64)g3
	       + f5*(i64)g2 + f6*(i64)g1 + f7*(i64)g0 + f8*(i64)G9 + f9*(i64)G8;
	i64 t8 = f0*(i64)g8 + F1*(i64)g7 + f2*(i64)g6 + F3*(i64)g5 + f4*(i64)g4
	       + F5*(i64)g3 + f6*(i64)g2 + F7*(i64)g1 + f8*(i64)g0 + F9*(i64)G9;
	i64 t9 = f0*(i64)g9 + f1*(i64)g8 + f2*(i64)g7 + f3*(i64)g6 + f4*(i64)g5
	       + f5*(i64)g4 + f6*(i64)g3 + f7*(i64)g2 + f8*(i64)g1 + f9*(i64)g0;
	// t0 < 0.67 * 2^61
	// t1 < 0.41 * 2^61
	// t2 < 0.52 * 2^61
	// t3 < 0.32 * 2^61
	// t4 < 0.38 * 2^61
	// t5 < 0.22 * 2^61
	// t6 < 0.23 * 2^61
	// t7 < 0.13 * 2^61
	// t8 < 0.09 * 2^61
	// t9 < 0.03 * 2^61

	FE_CARRY; // Everything below 2^62, Carry precondition OK
}

// Precondition
// -------------
//   |f0|, |f2|, |f4|, |f6|, |f8|  <  1.65 * 2^26
//   |f1|, |f3|, |f5|, |f7|, |f9|  <  1.65 * 2^25
//
// Note: we could use fe_mul() for this, but this is significantly faster
static void fe_sq(fe h, const fe f)
{
	i32 f0 = f[0]; i32 f1 = f[1]; i32 f2 = f[2]; i32 f3 = f[3]; i32 f4 = f[4];
	i32 f5 = f[5]; i32 f6 = f[6]; i32 f7 = f[7]; i32 f8 = f[8]; i32 f9 = f[9];
	i32 f0_2  = f0*2;   i32 f1_2  = f1*2;   i32 f2_2  = f2*2;   i32 f3_2 = f3*2;
	i32 f4_2  = f4*2;   i32 f5_2  = f5*2;   i32 f6_2  = f6*2;   i32 f7_2 = f7*2;
	i32 f5_38 = f5*38;  i32 f6_19 = f6*19;  i32 f7_38 = f7*38;
	i32 f8_19 = f8*19;  i32 f9_38 = f9*38;
	// |f0_2| , |f2_2| , |f4_2| , |f6_2| , |f8_2|  <  1.65 * 2^27
	// |f1_2| , |f3_2| , |f5_2| , |f7_2| , |f9_2|  <  1.65 * 2^26
	// |f5_38|, |f6_19|, |f7_38|, |f8_19|, |f9_38| <  2^31

	i64 t0 = f0  *(i64)f0    + f1_2*(i64)f9_38 + f2_2*(i64)f8_19
	       + f3_2*(i64)f7_38 + f4_2*(i64)f6_19 + f5  *(i64)f5_38;
	i64 t1 = f0_2*(i64)f1    + f2  *(i64)f9_38 + f3_2*(i64)f8_19
	       + f4  *(i64)f7_38 + f5_2*(i64)f6_19;
	i64 t2 = f0_2*(i64)f2    + f1_2*(i64)f1    + f3_2*(i64)f9_38
	       + f4_2*(i64)f8_19 + f5_2*(i64)f7_38 + f6  *(i64)f6_19;
	i64 t3 = f0_2*(i64)f3    + f1_2*(i64)f2    + f4  *(i64)f9_38
	       + f5_2*(i64)f8_19 + f6  *(i64)f7_38;
	i64 t4 = f0_2*(i64)f4    + f1_2*(i64)f3_2  + f2  *(i64)f2
	       + f5_2*(i64)f9_38 + f6_2*(i64)f8_19 + f7  *(i64)f7_38;
	i64 t5 = f0_2*(i64)f5    + f1_2*(i64)f4    + f2_2*(i64)f3
	       + f6  *(i64)f9_38 + f7_2*(i64)f8_19;
	i64 t6 = f0_2*(i64)f6    + f1_2*(i64)f5_2  + f2_2*(i64)f4
	       + f3_2*(i64)f3    + f7_2*(i64)f9_38 + f8  *(i64)f8_19;
	i64 t7 = f0_2*(i64)f7    + f1_2*(i64)f6    + f2_2*(i64)f5
	       + f3_2*(i64)f4    + f8  *(i64)f9_38;
	i64 t8 = f0_2*(i64)f8    + f1_2*(i64)f7_2  + f2_2*(i64)f6
	       + f3_2*(i64)f5_2  + f4  *(i64)f4    + f9  *(i64)f9_38;
	i64 t9 = f0_2*(i64)f9    + f1_2*(i64)f8    + f2_2*(i64)f7
	       + f3_2*(i64)f6    + f4  *(i64)f5_2;
	// t0 < 0.67 * 2^61
	// t1 < 0.41 * 2^61
	// t2 < 0.52 * 2^61
	// t3 < 0.32 * 2^61
	// t4 < 0.38 * 2^61
	// t5 < 0.22 * 2^61
	// t6 < 0.23 * 2^61
	// t7 < 0.13 * 2^61
	// t8 < 0.09 * 2^61
	// t9 < 0.03 * 2^61

	FE_CARRY;
}

// Returns 1 if equal, 0 if not equal
static int fe_isequal(const fe f, const fe g)
{
	u8 fs[32];
	u8 gs[32];
	fe_tobytes(fs, f);
	fe_tobytes(gs, g);
	int isdifferent = crypto_verify32(fs, gs);
	WIPE_BUFFER(fs);
	WIPE_BUFFER(gs);
	return 1 + isdifferent;
}

// Inverse square root.
// Returns true if x is a square, false otherwise.
// After the call:
//   isr = sqrt(1/x)        if x is a non-zero square.
//   isr = sqrt(sqrt(-1)/x) if x is not a square.
//   isr = 0                if x is zero.
// We do not guarantee the sign of the square root.
//
// Notes:
// Let quartic = x^((p-1)/4)
//
// x^((p-1)/2) = chi(x)
// quartic^2   = chi(x)
// quartic     = sqrt(chi(x))
// quartic     = 1 or -1 or sqrt(-1) or -sqrt(-1)
//
// Note that x is a square if quartic is 1 or -1
// There are 4 cases to consider:
//
// if   quartic         = 1  (x is a square)
// then x^((p-1)/4)     = 1
//      x^((p-5)/4) * x = 1
//      x^((p-5)/4)     = 1/x
//      x^((p-5)/8)     = sqrt(1/x) or -sqrt(1/x)
//
// if   quartic                = -1  (x is a square)
// then x^((p-1)/4)            = -1
//      x^((p-5)/4) * x        = -1
//      x^((p-5)/4)            = -1/x
//      x^((p-5)/8)            = sqrt(-1)   / sqrt(x)
//      x^((p-5)/8) * sqrt(-1) = sqrt(-1)^2 / sqrt(x)
//      x^((p-5)/8) * sqrt(-1) = -1/sqrt(x)
//      x^((p-5)/8) * sqrt(-1) = -sqrt(1/x) or sqrt(1/x)
//
// if   quartic         = sqrt(-1)  (x is not a square)
// then x^((p-1)/4)     = sqrt(-1)
//      x^((p-5)/4) * x = sqrt(-1)
//      x^((p-5)/4)     = sqrt(-1)/x
//      x^((p-5)/8)     = sqrt(sqrt(-1)/x) or -sqrt(sqrt(-1)/x)
//
// Note that the product of two non-squares is always a square:
//   For any non-squares a and b, chi(a) = -1 and chi(b) = -1.
//   Since chi(x) = x^((p-1)/2), chi(a)*chi(b) = chi(a*b) = 1.
//   Therefore a*b is a square.
//
//   Since sqrt(-1) and x are both non-squares, their product is a
//   square, and we can compute their square root.
//
// if   quartic                = -sqrt(-1)  (x is not a square)
// then x^((p-1)/4)            = -sqrt(-1)
//      x^((p-5)/4) * x        = -sqrt(-1)
//      x^((p-5)/4)            = -sqrt(-1)/x
//      x^((p-5)/8)            = sqrt(-sqrt(-1)/x)
//      x^((p-5)/8)            = sqrt( sqrt(-1)/x) * sqrt(-1)
//      x^((p-5)/8) * sqrt(-1) = sqrt( sqrt(-1)/x) * sqrt(-1)^2
//      x^((p-5)/8) * sqrt(-1) = sqrt( sqrt(-1)/x) * -1
//      x^((p-5)/8) * sqrt(-1) = -sqrt(sqrt(-1)/x) or sqrt(sqrt(-1)/x)
static int invsqrt(fe isr, const fe x)
{
	fe t0, t1, t2;

	// t0 = x^((p-5)/8)
	// Can be achieved with a simple double & add ladder,
	// but it would be slower.
	fe_sq(t0, x);
	fe_sq(t1,t0);                     fe_sq(t1, t1);    fe_mul(t1, x, t1);
	fe_mul(t0, t0, t1);
	fe_sq(t0, t0);                                      fe_mul(t0, t1, t0);
	fe_sq(t1, t0);  FOR (i, 1,   5) { fe_sq(t1, t1); }  fe_mul(t0, t1, t0);
	fe_sq(t1, t0);  FOR (i, 1,  10) { fe_sq(t1, t1); }  fe_mul(t1, t1, t0);
	fe_sq(t2, t1);  FOR (i, 1,  20) { fe_sq(t2, t2); }  fe_mul(t1, t2, t1);
	fe_sq(t1, t1);  FOR (i, 1,  10) { fe_sq(t1, t1); }  fe_mul(t0, t1, t0);
	fe_sq(t1, t0);  FOR (i, 1,  50) { fe_sq(t1, t1); }  fe_mul(t1, t1, t0);
	fe_sq(t2, t1);  FOR (i, 1, 100) { fe_sq(t2, t2); }  fe_mul(t1, t2, t1);
	fe_sq(t1, t1);  FOR (i, 1,  50) { fe_sq(t1, t1); }  fe_mul(t0, t1, t0);
	fe_sq(t0, t0);  FOR (i, 1,   2) { fe_sq(t0, t0); }  fe_mul(t0, t0, x);

	// quartic = x^((p-1)/4)
	i32 *quartic = t1;
	fe_sq (quartic, t0);
	fe_mul(quartic, quartic, x);

	i32 *check = t2;
	fe_0  (check);          int z0 = fe_isequal(x      , check);
	fe_1  (check);          int p1 = fe_isequal(quartic, check);
	fe_neg(check, check );  int m1 = fe_isequal(quartic, check);
	fe_neg(check, sqrtm1);  int ms = fe_isequal(quartic, check);

	// if quartic == -1 or sqrt(-1)
	// then  isr = x^((p-1)/4) * sqrt(-1)
	// else  isr = x^((p-1)/4)
	fe_mul(isr, t0, sqrtm1);
	fe_ccopy(isr, t0, 1 - (m1 | ms));

	WIPE_BUFFER(t0);
	WIPE_BUFFER(t1);
	WIPE_BUFFER(t2);
	return p1 | m1 | z0;
}

// Inverse in terms of inverse square root.
// Requires two additional squarings to get rid of the sign.
//
//   1/x = x * (+invsqrt(x^2))^2
//       = x * (-invsqrt(x^2))^2
//
// A fully optimised exponentiation by p-1 would save 6 field
// multiplications, but it would require more code.
static void fe_invert(fe out, const fe x)
{
	fe tmp;
	fe_sq(tmp, x);
	invsqrt(tmp, tmp);
	fe_sq(tmp, tmp);
	fe_mul(out, tmp, x);
	WIPE_BUFFER(tmp);
}

// trim a scalar for scalar multiplication
void crypto_eddsa_trim_scalar(u8 out[32], const u8 in[32])
{
	COPY(out, in, 32);
	out[ 0] &= 248;
	out[31] &= 127;
	out[31] |= 64;
}

// get bit from scalar at position i
static int scalar_bit(const u8 s[32], int i)
{
	if (i < 0) { return 0; } // handle -1 for sliding windows
	return (s[i>>3] >> (i&7)) & 1;
}

///////////////
/// X-25519 /// Taken from SUPERCOP's ref10 implementation.
///////////////
static void scalarmult(u8 q[32], const u8 scalar[32], const u8 p[32],
                       int nb_bits)
{
	// computes the scalar product
	fe x1;
	fe_frombytes(x1, p);

	// computes the actual scalar product (the result is in x2 and z2)
	fe x2, z2, x3, z3, t0, t1;
	// Montgomery ladder
	// In projective coordinates, to avoid divisions: x = X / Z
	// We don't care about the y coordinate, it's only 1 bit of information
	fe_1(x2);        fe_0(z2); // "zero" point
	fe_copy(x3, x1); fe_1(z3); // "one"  point
	int swap = 0;
	for (int pos = nb_bits-1; pos >= 0; --pos) {
		// constant time conditional swap before ladder step
		int b = scalar_bit(scalar, pos);
		swap ^= b; // xor trick avoids swapping at the end of the loop
		fe_cswap(x2, x3, swap);
		fe_cswap(z2, z3, swap);
		swap = b;  // anticipates one last swap after the loop

		// Montgomery ladder step: replaces (P2, P3) by (P2*2, P2+P3)
		// with differential addition
		fe_sub(t0, x3, z3);
		fe_sub(t1, x2, z2);
		fe_add(x2, x2, z2);
		fe_add(z2, x3, z3);
		fe_mul(z3, t0, x2);
		fe_mul(z2, z2, t1);
		fe_sq (t0, t1    );
		fe_sq (t1, x2    );
		fe_add(x3, z3, z2);
		fe_sub(z2, z3, z2);
		fe_mul(x2, t1, t0);
		fe_sub(t1, t1, t0);
		fe_sq (z2, z2    );
		fe_mul_small(z3, t1, 121666);
		fe_sq (x3, x3    );
		fe_add(t0, t0, z3);
		fe_mul(z3, x1, z2);
		fe_mul(z2, t1, t0);
	}
	// last swap is necessary to compensate for the xor trick
	// Note: after this swap, P3 == P2 + P1.
	fe_cswap(x2, x3, swap);
	fe_cswap(z2, z3, swap);

	// normalises the coordinates: x == X / Z
	fe_invert(z2, z2);
	fe_mul(x2, x2, z2);
	fe_tobytes(q, x2);

	WIPE_BUFFER(x1);
	WIPE_BUFFER(x2);  WIPE_BUFFER(z2);  WIPE_BUFFER(t0);
	WIPE_BUFFER(x3);  WIPE_BUFFER(z3);  WIPE_BUFFER(t1);
}

void crypto_x25519(u8       raw_shared_secret[32],
                   const u8 your_secret_key  [32],
                   const u8 their_public_key [32])
{
	// restrict the possible scalar values
	u8 e[32];
	crypto_eddsa_trim_scalar(e, your_secret_key);
	scalarmult(raw_shared_secret, e, their_public_key, 255);
	WIPE_BUFFER(e);
}

void crypto_x25519_public_key(u8       public_key[32],
                              const u8 secret_key[32])
{
	static const u8 base_point[32] = {9};
	crypto_x25519(public_key, secret_key, base_point);
}

///////////////////////////
////////////////////////////////
/// Authenticated encryption ///
////////////////////////////////
static void lock_auth(u8 mac[16], const u8  auth_key[32],
                      const u8 *ad         , size_t ad_size,
                      const u8 *cipher_text, size_t text_size)
{
	u8 sizes[16]; // Not secret, not wiped
	store64_le(sizes + 0, ad_size);
	store64_le(sizes + 8, text_size);
	crypto_poly1305_ctx poly_ctx;           // auto wiped...
	crypto_poly1305_init  (&poly_ctx, auth_key);
	crypto_poly1305_update(&poly_ctx, ad         , ad_size);
	crypto_poly1305_update(&poly_ctx, zero       , gap(ad_size, 16));
	crypto_poly1305_update(&poly_ctx, cipher_text, text_size);
	crypto_poly1305_update(&poly_ctx, zero       , gap(text_size, 16));
	crypto_poly1305_update(&poly_ctx, sizes      , 16);
	crypto_poly1305_final (&poly_ctx, mac); // ...here
}

void crypto_aead_init_x(crypto_aead_ctx *ctx,
                        u8 const key[32], const u8 nonce[24])
{
	crypto_chacha20_h(ctx->key, key, nonce);
	COPY(ctx->nonce, nonce + 16, 8);
	ctx->counter = 0;
}

void crypto_aead_init_djb(crypto_aead_ctx *ctx,
                          const u8 key[32], const u8 nonce[8])
{
	COPY(ctx->key  , key  , 32);
	COPY(ctx->nonce, nonce,  8);
	ctx->counter = 0;
}

void crypto_aead_init_ietf(crypto_aead_ctx *ctx,
                           const u8 key[32], const u8 nonce[12])
{
	COPY(ctx->key  , key      , 32);
	COPY(ctx->nonce, nonce + 4,  8);
	ctx->counter = (u64)load32_le(nonce) << 32;
}

void crypto_aead_write(crypto_aead_ctx *ctx, u8 *cipher_text, u8 mac[16],
                       const u8 *ad,         size_t ad_size,
                       const u8 *plain_text, size_t text_size)
{
	u8 auth_key[64]; // the last 32 bytes are used for rekeying.
	crypto_chacha20_djb(auth_key, 0, 64, ctx->key, ctx->nonce, ctx->counter);
	crypto_chacha20_djb(cipher_text, plain_text, text_size,
	                    ctx->key, ctx->nonce, ctx->counter + 1);
	lock_auth(mac, auth_key, ad, ad_size, cipher_text, text_size);
	COPY(ctx->key, auth_key + 32, 32);
	WIPE_BUFFER(auth_key);
}

int crypto_aead_read(crypto_aead_ctx *ctx, u8 *plain_text, const u8 mac[16],
                     const u8 *ad,          size_t ad_size,
                     const u8 *cipher_text, size_t text_size)
{
	u8 auth_key[64]; // the last 32 bytes are used for rekeying.
	u8 real_mac[16];
	crypto_chacha20_djb(auth_key, 0, 64, ctx->key, ctx->nonce, ctx->counter);
	lock_auth(real_mac, auth_key, ad, ad_size, cipher_text, text_size);
	int mismatch = crypto_verify16(mac, real_mac);
	if (!mismatch) {
		crypto_chacha20_djb(plain_text, cipher_text, text_size,
		                    ctx->key, ctx->nonce, ctx->counter + 1);
		COPY(ctx->key, auth_key + 32, 32);
	}
	WIPE_BUFFER(auth_key);
	WIPE_BUFFER(real_mac);
	return mismatch;
}

void crypto_aead_lock(u8 *cipher_text, u8 mac[16], const u8 key[32],
                      const u8  nonce[24], const u8 *ad, size_t ad_size,
                      const u8 *plain_text, size_t text_size)
{
	crypto_aead_ctx ctx;
	crypto_aead_init_x(&ctx, key, nonce);
	crypto_aead_write(&ctx, cipher_text, mac, ad, ad_size,
	                  plain_text, text_size);
	crypto_wipe(&ctx, sizeof(ctx));
}

int crypto_aead_unlock(u8 *plain_text, const u8  mac[16], const u8 key[32],
                       const u8 nonce[24], const u8 *ad, size_t ad_size,
                       const u8 *cipher_text, size_t text_size)
{
	crypto_aead_ctx ctx;
	crypto_aead_init_x(&ctx, key, nonce);
	int mismatch = crypto_aead_read(&ctx, plain_text, mac, ad, ad_size,
	                                cipher_text, text_size);
	crypto_wipe(&ctx, sizeof(ctx));
	return mismatch;
}


#undef FOR_T
#undef FOR
#undef COPY
#undef ZERO
#undef WIPE_CTX
#undef WIPE_BUFFER
#undef MIN
#undef MAX
#undef QUARTERROUND
#undef BLAKE2_G
#undef BLAKE2_ROUND
#undef FE_CARRY
/* ==== end monocypher ======================================================== */

#if !defined(_WIN32) || defined(HAKO_RELAY)   /* the desktop link and the studio page */
/* ---- noise: XX and XXpsk3, 25519 / ChaChaPoly / BLAKE2b (noiseprotocol.org rev 34) ----
   The relay link (RELAY.md §5). XXpsk3 for sessions, the psk from the account
   key; plain XX for the six-digit fallback. Checked against the cacophony
   vectors in tools/tests.c. */
#define HK_NOISE_MAX 65535
typedef struct { uint8_t k[32]; uint64_t n; int has_k; } hkNoiseCipher;
typedef struct {
	hkNoiseCipher cs;
	uint8_t ck[64], h[64];
	uint8_t s[32], s_pub[32], e[32], e_pub[32], rs[32], re[32], psk[32];
	int has_s, has_e, has_rs, has_re, has_psk, initiator;
	const char *msgs;      /* tokens, "," between them, "|" between messages */
	int next;              /* index of the next handshake message */
} hkNoise;

/* HMAC over BLAKE2b (block 128), as Noise's HKDF wants it, over a || b. */
static void hkNoiseHmac(uint8_t out[64], const uint8_t *key, size_t kl,
                        const uint8_t *a, size_t al, const uint8_t *b, size_t bl) {
	uint8_t pad[128], inner[64];
	crypto_blake2b_ctx c;
	memset(pad, 0, sizeof pad);
	memcpy(pad, key, kl);
	for (int i = 0; i < 128; i++) pad[i] ^= 0x36;
	crypto_blake2b_init(&c, 64);
	crypto_blake2b_update(&c, pad, 128);
	crypto_blake2b_update(&c, a, al);
	crypto_blake2b_update(&c, b, bl);
	crypto_blake2b_final(&c, inner);
	for (int i = 0; i < 128; i++) pad[i] ^= 0x36 ^ 0x5c;
	crypto_blake2b_init(&c, 64);
	crypto_blake2b_update(&c, pad, 128);
	crypto_blake2b_update(&c, inner, 64);
	crypto_blake2b_final(&c, out);
	crypto_wipe(pad, sizeof pad);
	crypto_wipe(inner, sizeof inner);
}

static void hkNoiseHkdf(const uint8_t ck[64], const uint8_t *ikm, size_t il,
                        uint8_t o1[64], uint8_t o2[64], uint8_t *o3) {
	static const uint8_t one = 1, two = 2, three = 3;
	uint8_t t[64];
	hkNoiseHmac(t, ck, 64, ikm, il, NULL, 0);
	hkNoiseHmac(o1, t, 64, &one, 1, NULL, 0);
	hkNoiseHmac(o2, t, 64, o1, 64, &two, 1);
	if (o3) hkNoiseHmac(o3, t, 64, o2, 64, &three, 1);
	crypto_wipe(t, sizeof t);
}

static void hkNoiseMixHash(hkNoise *ns, const uint8_t *d, size_t n) {
	crypto_blake2b_ctx c;
	crypto_blake2b_init(&c, 64);
	crypto_blake2b_update(&c, ns->h, 64);
	crypto_blake2b_update(&c, d, n);
	crypto_blake2b_final(&c, ns->h);
}

static void hkNoiseMixKey(hkNoise *ns, const uint8_t *ikm, size_t n) {
	uint8_t o1[64], o2[64];
	hkNoiseHkdf(ns->ck, ikm, n, o1, o2, NULL);
	memcpy(ns->ck, o1, 64);
	memcpy(ns->cs.k, o2, 32);
	ns->cs.n = 0;
	ns->cs.has_k = 1;
	crypto_wipe(o1, sizeof o1);
	crypto_wipe(o2, sizeof o2);
}

static void hkNoiseMixPsk(hkNoise *ns) {
	uint8_t o1[64], o2[64], o3[64];
	hkNoiseHkdf(ns->ck, ns->psk, 32, o1, o2, o3);
	memcpy(ns->ck, o1, 64);
	hkNoiseMixHash(ns, o2, 64);
	memcpy(ns->cs.k, o3, 32);
	ns->cs.n = 0;
	ns->cs.has_k = 1;
	crypto_wipe(o1, sizeof o1);
	crypto_wipe(o2, sizeof o2);
	crypto_wipe(o3, sizeof o3);
}

/* ChaCha20-Poly1305 (RFC 8439): nonce = 4 zero bytes, then n little-endian.
   out holds n + 16 bytes. Returns 0, or -1 when the nonce is spent. */
static int hkNoiseSeal(hkNoiseCipher *c, const uint8_t *ad, size_t adl,
                       const uint8_t *in, size_t n, uint8_t *out) {
	if (c->n == UINT64_MAX) return -1;
	uint8_t nonce[12] = {0};
	for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(c->n >> (8 * i));
	crypto_aead_ctx ctx;
	crypto_aead_init_ietf(&ctx, c->k, nonce);
	crypto_aead_write(&ctx, out, out + n, ad, adl, in, n);
	crypto_wipe(&ctx, sizeof ctx);
	c->n++;
	return 0;
}

/* in holds n bytes, the last 16 the tag. Returns the plaintext length, or -1. */
static long hkNoiseOpen(hkNoiseCipher *c, const uint8_t *ad, size_t adl,
                        const uint8_t *in, size_t n, uint8_t *out) {
	if (n < 16 || c->n == UINT64_MAX) return -1;
	uint8_t nonce[12] = {0};
	for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(c->n >> (8 * i));
	crypto_aead_ctx ctx;
	crypto_aead_init_ietf(&ctx, c->k, nonce);
	int bad = crypto_aead_read(&ctx, out, in + n - 16, ad, adl, in, n - 16);
	crypto_wipe(&ctx, sizeof ctx);
	if (bad) return -1;
	c->n++;
	return (long)(n - 16);
}

/* A DH that lands on zero means the peer sent a low-order point: refuse it. */
static int hkNoiseDh(hkNoise *ns, const uint8_t priv[32], const uint8_t pub[32]) {
	uint8_t out[32], acc = 0;
	crypto_x25519(out, priv, pub);
	for (int i = 0; i < 32; i++) acc |= out[i];
	if (acc) hkNoiseMixKey(ns, out, 32);
	crypto_wipe(out, sizeof out);
	return acc ? 0 : -1;
}

/* pattern "XX" or "XXpsk3". Set s (and psk for XXpsk3) first; s_pub is derived here. */
static int hkNoiseInit(hkNoise *ns, const char *pattern, int initiator,
                       const uint8_t *prologue, size_t pl) {
	char name[64];
	snprintf(name, sizeof name, "Noise_%s_25519_ChaChaPoly_BLAKE2b", pattern);
	int psk = !strcmp(pattern, "XXpsk3");
	if (!psk && strcmp(pattern, "XX")) return -1;
	if (!ns->has_s || psk != ns->has_psk) return -1;
	ns->initiator = initiator;
	ns->msgs = psk ? "e|e,ee,s,es|s,se,psk" : "e|e,ee,s,es|s,se";
	ns->next = 0;
	memset(&ns->cs, 0, sizeof ns->cs);
	memset(ns->h, 0, 64);
	memcpy(ns->h, name, strlen(name));
	memcpy(ns->ck, ns->h, 64);
	crypto_x25519_public_key(ns->s_pub, ns->s);
	hkNoiseMixHash(ns, prologue, pl);
	return 0;
}

static const char *hkNoiseTokens(const hkNoise *ns) {
	const char *p = ns->msgs;
	for (int i = 0; i < ns->next && p; i++) { p = strchr(p, '|'); if (p) p++; }
	return p;
}

/* 0 while the handshake runs, 1 once it is complete. */
static int hkNoiseDone(const hkNoise *ns) { return hkNoiseTokens(ns) == NULL; }

/* The DH for token "ee", "es", "se" or "ss": the first letter names the
   initiator's key, the second the responder's. */
static int hkNoiseDhToken(hkNoise *ns, const char *t) {
	int mine_e = (ns->initiator ? t[0] : t[1]) == 'e';
	int peer_e = (ns->initiator ? t[1] : t[0]) == 'e';
	if ((mine_e && !ns->has_e) || (!mine_e && !ns->has_s)
	    || (peer_e && !ns->has_re) || (!peer_e && !ns->has_rs)) return -1;
	return hkNoiseDh(ns, mine_e ? ns->e : ns->s, peer_e ? ns->re : ns->rs);
}

/* Writes the next handshake message carrying payload. Returns its length or -1. */
static long hkNoiseWrite(hkNoise *ns, const uint8_t *payload, size_t pl, uint8_t *out, size_t cap) {
	const char *t = hkNoiseTokens(ns);
	if (!t || ((ns->next & 1) == 0) != ns->initiator) return -1;
	size_t o = 0;
	for (size_t tl; *t && *t != '|'; t += tl + (t[tl] == ',')) {
		tl = strcspn(t, ",|");
		if (tl == 1 && t[0] == 'e') {
			if (!ns->has_e && !hkRandomBytes(ns->e, 32)) return -1;
			ns->has_e = 1;
			crypto_x25519_public_key(ns->e_pub, ns->e);
			if (o + 32 > cap) return -1;
			memcpy(out + o, ns->e_pub, 32); o += 32;
			hkNoiseMixHash(ns, ns->e_pub, 32);
			if (ns->has_psk) hkNoiseMixKey(ns, ns->e_pub, 32);
		} else if (tl == 1 && t[0] == 's') {
			size_t n = 32 + (ns->cs.has_k ? 16 : 0);
			if (o + n > cap) return -1;
			if (ns->cs.has_k) { if (hkNoiseSeal(&ns->cs, ns->h, 64, ns->s_pub, 32, out + o)) return -1; }
			else memcpy(out + o, ns->s_pub, 32);
			hkNoiseMixHash(ns, out + o, n); o += n;
		} else if (tl == 3) hkNoiseMixPsk(ns);
		else if (hkNoiseDhToken(ns, t)) return -1;
	}
	size_t n = pl + (ns->cs.has_k ? 16 : 0);
	if (o + n > cap || o + n > HK_NOISE_MAX) return -1;
	if (ns->cs.has_k) { if (hkNoiseSeal(&ns->cs, ns->h, 64, payload, pl, out + o)) return -1; }
	else if (pl) memcpy(out + o, payload, pl);
	hkNoiseMixHash(ns, out + o, n);
	ns->next++;
	return (long)(o + n);
}

/* Reads the next handshake message. Returns the payload length or -1. */
static long hkNoiseRead(hkNoise *ns, const uint8_t *in, size_t n, uint8_t *payload, size_t cap) {
	const char *t = hkNoiseTokens(ns);
	if (!t || ((ns->next & 1) == 0) == ns->initiator || n > HK_NOISE_MAX) return -1;
	size_t o = 0;
	for (size_t tl; *t && *t != '|'; t += tl + (t[tl] == ',')) {
		tl = strcspn(t, ",|");
		if (tl == 1 && t[0] == 'e') {
			if (o + 32 > n) return -1;
			memcpy(ns->re, in + o, 32); o += 32;
			ns->has_re = 1;
			hkNoiseMixHash(ns, ns->re, 32);
			if (ns->has_psk) hkNoiseMixKey(ns, ns->re, 32);
		} else if (tl == 1 && t[0] == 's') {
			size_t sn = 32 + (ns->cs.has_k ? 16 : 0);
			uint8_t hold[64];
			if (o + sn > n) return -1;
			memcpy(hold, ns->h, 64);
			if (ns->cs.has_k) { if (hkNoiseOpen(&ns->cs, hold, 64, in + o, sn, ns->rs) != 32) return -1; }
			else memcpy(ns->rs, in + o, 32);
			ns->has_rs = 1;
			hkNoiseMixHash(ns, in + o, sn); o += sn;
		} else if (tl == 3) hkNoiseMixPsk(ns);
		else if (hkNoiseDhToken(ns, t)) return -1;
	}
	size_t rest = n - o;
	uint8_t hold[64];
	memcpy(hold, ns->h, 64);
	long pl;
	if (ns->cs.has_k) {
		if (rest < 16 || rest - 16 > cap) return -1;
		pl = hkNoiseOpen(&ns->cs, hold, 64, in + o, rest, payload);
		if (pl < 0) return -1;
	} else {
		if (rest > cap) return -1;
		if (rest) memcpy(payload, in + o, rest);
		pl = (long)rest;
	}
	hkNoiseMixHash(ns, in + o, rest);
	ns->next++;
	return pl;
}

/* After the last message: the two transport ciphers, send then receive for
   this side. The handshake's secrets are wiped; h stays for the link check. */
static void hkNoiseSplit(hkNoise *ns, hkNoiseCipher *tx, hkNoiseCipher *rx) {
	uint8_t o1[64], o2[64];
	hkNoiseHkdf(ns->ck, NULL, 0, o1, o2, NULL);
	memset(tx, 0, sizeof *tx);
	memset(rx, 0, sizeof *rx);
	memcpy(ns->initiator ? tx->k : rx->k, o1, 32);
	memcpy(ns->initiator ? rx->k : tx->k, o2, 32);
	tx->has_k = rx->has_k = 1;
	crypto_wipe(o1, sizeof o1);
	crypto_wipe(o2, sizeof o2);
	crypto_wipe(ns->ck, sizeof ns->ck);
	crypto_wipe(ns->e, sizeof ns->e);
	crypto_wipe(ns->psk, sizeof ns->psk);
	crypto_wipe(&ns->cs, sizeof ns->cs);
}

/* The session psk from the account key K (RELAY.md §4): only a holder of K
   completes XXpsk3, so the relay has nothing to guess. */
static void hkSessionPsk(uint8_t psk[32], const uint8_t k[32]) {
	crypto_blake2b_keyed(psk, 32, k, 32, (const uint8_t *)"hako session psk", 16);
}

/* A device name sealed under K (nonce 24, mac 16, text), so the relay lists
   "computer:<host>" for the account's devices without reading it. */
static void hkNameKey(uint8_t key[32], const uint8_t k[32]) {
	crypto_blake2b_keyed(key, 32, k, 32, (const uint8_t *)"hako device name", 16);
}

/* The link seal: K and the account id (48 bytes) under a key from a one-time
   secret that only travels in a URL fragment or a QR code, never via a server.
   out: nonce(24) mac(16) sealed(48). */
static int hkLinkSeal(uint8_t out[88], const uint8_t secret[32], const uint8_t pt[48]) {
	uint8_t key[32];
	if (!hkRandomBytes(out, 24)) return -1;
	crypto_blake2b_keyed(key, 32, secret, 32, (const uint8_t *)"hako link key", 13);
	crypto_aead_lock(out + 40, out + 24, key, out, NULL, 0, pt, 48);
	crypto_wipe(key, sizeof key);
	return 0;
}

static int hkLinkOpen(uint8_t pt[48], const uint8_t secret[32], const uint8_t m[88]) {
	uint8_t key[32];
	crypto_blake2b_keyed(key, 32, secret, 32, (const uint8_t *)"hako link key", 13);
	int bad = crypto_aead_unlock(pt, m + 24, key, m, NULL, 0, m + 40, 48);
	crypto_wipe(key, sizeof key);
	return bad ? -1 : 0;
}

/* ---- QR code (ISO 18004), for :remote pair and the studio page ----
   Byte mode, error correction M, versions 1-10 (up to 213 bytes): a link URL
   fits, and a phone camera reads it off the terminal. The layout follows the
   standard step by step: data + Reed-Solomon blocks, function patterns, the
   zigzag fill, then the mask with the lowest penalty. */
#define HK_QR_MAX 57                                  /* version 10: 4*10+17 modules */
typedef struct { int n; uint8_t m[HK_QR_MAX][HK_QR_MAX], fn[HK_QR_MAX][HK_QR_MAX]; } hkQr;

static uint8_t hkQrMul(uint8_t x, uint8_t y) {         /* GF(256), x^8+x^4+x^3+x^2+1 */
	int z = 0;
	for (int i = 7; i >= 0; i--) { z = (z << 1) ^ ((z >> 7) * 0x11D); z ^= ((y >> i) & 1) * x; }
	return (uint8_t)z;
}

static void hkQrSet(hkQr *q, int x, int y, int dark) { q->m[y][x] = (uint8_t)dark; q->fn[y][x] = 1; }

static void hkQrFormat(hkQr *q, int mask) {           /* level M = 00 */
	int d = mask, r = d;
	for (int i = 0; i < 10; i++) r = (r << 1) ^ ((r >> 9) * 0x537);
	int b = ((d << 10) | r) ^ 0x5412, n = q->n;
	for (int i = 0; i <= 5; i++) hkQrSet(q, 8, i, (b >> i) & 1);
	hkQrSet(q, 8, 7, (b >> 6) & 1); hkQrSet(q, 8, 8, (b >> 7) & 1); hkQrSet(q, 7, 8, (b >> 8) & 1);
	for (int i = 9; i < 15; i++) hkQrSet(q, 14 - i, 8, (b >> i) & 1);
	for (int i = 0; i < 8; i++) hkQrSet(q, n - 1 - i, 8, (b >> i) & 1);
	for (int i = 8; i < 15; i++) hkQrSet(q, 8, n - 15 + i, (b >> i) & 1);
	hkQrSet(q, 8, n - 8, 1);
}

static int hkQrMasked(int mask, int x, int y) {
	switch (mask) {
	case 0: return (x + y) % 2 == 0;
	case 1: return y % 2 == 0;
	case 2: return x % 3 == 0;
	case 3: return (x + y) % 3 == 0;
	case 4: return (x / 3 + y / 2) % 2 == 0;
	case 5: return x * y % 2 + x * y % 3 == 0;
	case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
	default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
	}
}

static void hkQrApply(hkQr *q, int mask) {
	for (int y = 0; y < q->n; y++)
		for (int x = 0; x < q->n; x++)
			if (!q->fn[y][x] && hkQrMasked(mask, x, y)) q->m[y][x] ^= 1;
}

static long hkQrPenalty(const hkQr *q) {
	int n = q->n;
	long p = 0, dark = 0;
	for (int t = 0; t < 2; t++)                       /* rows, then columns */
		for (int a = 0; a < n; a++) {
			int run = 0, prev = -1;
			unsigned hist = 0;
			for (int b = 0; b < n; b++) {
				int v = t ? q->m[b][a] : q->m[a][b];
				run = v == prev ? run + 1 : 1;
				prev = v;
				if (run == 5) p += 3; else if (run > 5) p++;
				hist = ((hist << 1) | (unsigned)v) & 0x7FF;   /* 1:1:3:1:1 with four light on a side */
				if (b >= 10 && (hist == 0x5D0 || hist == 0x05D)) p += 40;
			}
		}
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			dark += q->m[y][x];
			if (x + 1 < n && y + 1 < n && q->m[y][x] == q->m[y][x + 1] && q->m[y][x] == q->m[y + 1][x] && q->m[y][x] == q->m[y + 1][x + 1]) p += 3;
		}
	long k = (labs(dark * 20 - (long)n * n * 10) + (long)n * n - 1) / ((long)n * n) - 1;
	return p + (k > 0 ? k : 0) * 10;
}

/* The symbol for text, or 0 when it does not fit version 10. */
static int hkQrEncode(hkQr *q, const char *text) {
	static const uint8_t ecc[11] = { 0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
	static const uint8_t blocks[11] = { 0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 };
	size_t len = strlen(text);
	int ver, raw = 0, data = 0;
	for (ver = 1; ver <= 10; ver++) {
		raw = (16 * ver + 128) * ver + 64;
		if (ver >= 2) { int na = ver / 7 + 2; raw -= (25 * na - 10) * na - 55; if (ver >= 7) raw -= 36; }
		raw /= 8;
		data = raw - ecc[ver] * blocks[ver];
		if ((int)len * 8 + 4 + (ver < 10 ? 8 : 16) <= data * 8) break;
	}
	if (ver > 10) return 0;
	uint8_t buf[HK_QR_MAX * HK_QR_MAX / 8 + 8] = { 0 }, out[sizeof buf] = { 0 };
	int bit = 0;
#define HK_QR_PUT(v, nb) for (int i_ = (nb) - 1; i_ >= 0; i_--, bit++) buf[bit >> 3] |= (uint8_t)((((v) >> i_) & 1) << (7 - (bit & 7)))
	HK_QR_PUT(4, 4);
	HK_QR_PUT((int)len, ver < 10 ? 8 : 16);
	for (size_t i = 0; i < len; i++) HK_QR_PUT((uint8_t)text[i], 8);
	int term = data * 8 - bit < 4 ? data * 8 - bit : 4;
	HK_QR_PUT(0, term);
	bit = (bit + 7) & ~7;
	for (int pad = 0xEC; bit < data * 8; pad ^= 0xEC ^ 0x11) HK_QR_PUT(pad, 8);
#undef HK_QR_PUT
	/* split into blocks, each with its Reed-Solomon remainder, then interleave */
	int nb = blocks[ver], ne = ecc[ver], shortn = nb - raw % nb, slen = raw / nb;
	uint8_t div[32] = { 0 }, blk[5][160];
	div[ne - 1] = 1;
	for (int i = 0, root = 1; i < ne; i++, root = hkQrMul((uint8_t)root, 2))
		for (int j = 0; j < ne; j++) { div[j] = hkQrMul(div[j], (uint8_t)root); if (j + 1 < ne) div[j] ^= div[j + 1]; }
	for (int b = 0, k = 0; b < nb; b++) {
		int dl = slen - ne + (b < shortn ? 0 : 1);
		uint8_t *r = blk[b] + slen + 1 - ne;
		memset(blk[b], 0, sizeof blk[b]);
		memcpy(blk[b], buf + k, (size_t)dl);
		k += dl;
		for (int i = 0; i < dl; i++) {
			uint8_t f = blk[b][i] ^ r[0];
			memmove(r, r + 1, (size_t)ne - 1);
			r[ne - 1] = 0;
			for (int j = 0; j < ne; j++) r[j] ^= hkQrMul(div[j], f);
		}
		if (b < shortn) memmove(blk[b] + dl + 1, r, (size_t)ne);   /* short blocks skip one data column */
	}
	int o = 0;
	for (int i = 0; i <= slen; i++)
		for (int b = 0; b < nb; b++)
			if (i != slen - ne || b >= shortn) out[o++] = blk[b][i];
	/* function patterns */
	int n = q->n = 4 * ver + 17;
	memset(q->m, 0, sizeof q->m);
	memset(q->fn, 0, sizeof q->fn);
	for (int i = 0; i < n; i++) { hkQrSet(q, 6, i, i % 2 == 0); hkQrSet(q, i, 6, i % 2 == 0); }
	const int fc[3][2] = { { 3, 3 }, { n - 4, 3 }, { 3, n - 4 } };
	for (int f = 0; f < 3; f++)
		for (int dy = -4; dy <= 4; dy++)
			for (int dx = -4; dx <= 4; dx++) {
				int x = fc[f][0] + dx, y = fc[f][1] + dy, d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
				if (x >= 0 && x < n && y >= 0 && y < n) hkQrSet(q, x, y, d != 2 && d != 4);
			}
	if (ver >= 2) {
		int na = ver / 7 + 2, step = (ver * 4 + na * 2 + 1) / (na * 2 - 2) * 2, pos[7];
		pos[0] = 6;
		for (int i = na - 1, p = ver * 4 + 10; i >= 1; i--, p -= step) pos[i] = p;
		for (int i = 0; i < na; i++)
			for (int j = 0; j < na; j++) {
				if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0)) continue;
				for (int dy = -2; dy <= 2; dy++)
					for (int dx = -2; dx <= 2; dx++)
						hkQrSet(q, pos[i] + dx, pos[j] + dy, (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) != 1);
			}
	}
	hkQrFormat(q, 0);
	if (ver >= 7) {
		int r = ver;
		for (int i = 0; i < 12; i++) r = (r << 1) ^ ((r >> 11) * 0x1F25);
		long v = (long)ver << 12 | r;
		for (int i = 0; i < 18; i++) {
			int a = n - 11 + i % 3, b = i / 3, d = (v >> i) & 1;
			hkQrSet(q, a, b, d); hkQrSet(q, b, a, d);
		}
	}
	/* the zigzag fill, two columns at a time from the right */
	for (int right = n - 1, i = 0; right >= 1; right -= 2) {
		if (right == 6) right = 5;
		for (int v = 0; v < n; v++)
			for (int j = 0; j < 2; j++) {
				int x = right - j, y = ((right + 1) & 2) == 0 ? n - 1 - v : v;
				if (!q->fn[y][x] && i < o * 8) { q->m[y][x] = (out[i >> 3] >> (7 - (i & 7))) & 1; i++; }
			}
	}
	int best = 0;
	long low = -1;
	for (int mask = 0; mask < 8; mask++) {
		hkQrApply(q, mask);
		hkQrFormat(q, mask);
		long p = hkQrPenalty(q);
		if (low < 0 || p < low) { low = p; best = mask; }
		hkQrApply(q, mask);
	}
	hkQrApply(q, best);
	hkQrFormat(q, best);
	return n;
}

#ifdef HAKO_WASM
/* ---- the studio page's crypto (RELAY.md §5): the desktop's C, exported ----
   JS copies bytes into hkw_in, calls, and reads hkw_out. One session per page. */
#define HKW_IO 70000
#define HKW(name) __attribute__((export_name(#name)))
static uint8_t hkw_in[HKW_IO], hkw_out[HKW_IO];
static hkNoise hkw_ns;
static hkNoiseCipher hkw_tx, hkw_rx;
static int hkw_split;

HKW(hkw_in_ptr) uint8_t *hkw_in_ptr(void) { return hkw_in; }
HKW(hkw_out_ptr) uint8_t *hkw_out_ptr(void) { return hkw_out; }
HKW(hkw_random) int hkw_random(int n) { return n > 0 && n <= HKW_IO && hkRandomBytes(hkw_out, (size_t)n) ? 0 : -1; }

/* in: s(32) -> out: pub(32), device id(16) */
HKW(hkw_pub) void hkw_pub(void) {
	uint8_t h[64];
	crypto_x25519_public_key(hkw_out, hkw_in);
	crypto_blake2b(h, 64, hkw_out, 32);
	memcpy(hkw_out + 32, h, 16);
}

/* in: a device's pub(32) -> out: its relay id(16) */
HKW(hkw_id) void hkw_id(void) {
	uint8_t h[64];
	crypto_blake2b(h, 64, hkw_in, 32);
	memcpy(hkw_out, h, 16);
}

/* in: s(32) relay ephemeral(32) challenge(16) -> out: the sign-in MAC(32) */
HKW(hkw_proof) int hkw_proof(void) {
	uint8_t k[32], pub[32], acc = 0;
	crypto_x25519(k, hkw_in, hkw_in + 32);
	for (int i = 0; i < 32; i++) acc |= k[i];
	crypto_x25519_public_key(pub, hkw_in);
	crypto_blake2b_ctx c;
	crypto_blake2b_init(&c, 64);
	crypto_blake2b_update(&c, k, 32);
	crypto_blake2b_update(&c, hkw_in + 64, 16);
	crypto_blake2b_update(&c, pub, 32);
	crypto_blake2b_final(&c, hkw_out);
	crypto_wipe(k, sizeof k);
	return acc ? 0 : -1;
}

/* in: s(32) K(32). XXpsk3 as the initiator (the page dials the computer). */
HKW(hkw_session) int hkw_session(void) {
	memset(&hkw_ns, 0, sizeof hkw_ns);
	crypto_wipe(&hkw_tx, sizeof hkw_tx);
	crypto_wipe(&hkw_rx, sizeof hkw_rx);
	hkw_split = 0;
	memcpy(hkw_ns.s, hkw_in, 32); hkw_ns.has_s = 1;
	hkSessionPsk(hkw_ns.psk, hkw_in + 32); hkw_ns.has_psk = 1;
	return hkNoiseInit(&hkw_ns, "XXpsk3", 1, (const uint8_t *)"hako session", 12);
}

static void hkwMaybeSplit(void) {
	if (!hkw_split && hkNoiseDone(&hkw_ns)) { hkNoiseSplit(&hkw_ns, &hkw_tx, &hkw_rx); hkw_split = 1; }
}

/* payload in -> message out. Returns its length, or -1. */
HKW(hkw_hs_write) long hkw_hs_write(int plen) {
	long n = plen >= 0 && plen < 1024 ? hkNoiseWrite(&hkw_ns, hkw_in, (size_t)plen, hkw_out, HKW_IO) : -1;
	if (n > 0) hkwMaybeSplit();
	return n;
}

HKW(hkw_hs_read) long hkw_hs_read(int n) {
	long r = n > 0 && n <= HKW_IO ? hkNoiseRead(&hkw_ns, hkw_in, (size_t)n, hkw_out, HKW_IO) : -1;
	if (r >= 0) hkwMaybeSplit();
	return r;
}

HKW(hkw_ready) int hkw_ready(void) { return hkw_split; }

HKW(hkw_seal) long hkw_seal(int n) {
	if (!hkw_split || n < 0 || n + 16 > HKW_IO) return -1;
	return hkNoiseSeal(&hkw_tx, NULL, 0, hkw_in, (size_t)n, hkw_out) ? -1 : n + 16;
}

HKW(hkw_open) long hkw_open(int n) {
	return hkw_split && n >= 16 && n <= HKW_IO ? hkNoiseOpen(&hkw_rx, NULL, 0, hkw_in, (size_t)n, hkw_out) : -1;
}

/* The account key under a passkey's prf output. in: prf(32) K(32) ->
   out: nonce(24) mac(16) sealed K(32). */
HKW(hkw_wrap) int hkw_wrap(void) {
	uint8_t key[32];
	if (!hkRandomBytes(hkw_out, 24)) return -1;
	crypto_blake2b_keyed(key, 32, hkw_in, 32, (const uint8_t *)"hako account key", 16);
	crypto_aead_lock(hkw_out + 40, hkw_out + 24, key, hkw_out, NULL, 0, hkw_in + 32, 32);
	crypto_wipe(key, sizeof key);
	return 0;
}

/* in: prf(32) nonce(24) mac(16) sealed(32) -> out: K(32). 0, or -1. */
HKW(hkw_unwrap) int hkw_unwrap(void) {
	uint8_t key[32];
	crypto_blake2b_keyed(key, 32, hkw_in, 32, (const uint8_t *)"hako account key", 16);
	int bad = crypto_aead_unlock(hkw_out, hkw_in + 56, key, hkw_in + 32, NULL, 0, hkw_in + 72, 32);
	crypto_wipe(key, sizeof key);
	return bad ? -1 : 0;
}

/* in: K(32) sealed name(n) -> out: the name. Its length, or -1. */
HKW(hkw_name_open) long hkw_name_open(int n) {
	uint8_t key[32];
	if (n < 32 + 40 || n > 32 + 40 + 512) return -1;
	hkNameKey(key, hkw_in);
	long len = n - 32 - 40;
	int bad = crypto_aead_unlock(hkw_out, hkw_in + 32 + 24, key, hkw_in + 32, NULL, 0, hkw_in + 72, (size_t)len);
	crypto_wipe(key, sizeof key);
	return bad ? -1 : len;
}

/* in: K(32) name(n) -> out: nonce(24) mac(16) sealed name. Its length, or -1.
   A browser that holds K lists itself this way, like a computer does. */
HKW(hkw_name_seal) long hkw_name_seal(int n) {
	uint8_t key[32];
	if (n < 32 + 1 || n > 32 + 150 || !hkRandomBytes(hkw_out, 24)) return -1;
	hkNameKey(key, hkw_in);
	crypto_aead_lock(hkw_out + 40, hkw_out + 24, key, hkw_out, NULL, 0, hkw_in + 32, (size_t)n - 32);
	crypto_wipe(key, sizeof key);
	return 40 + (long)n - 32;
}

/* The link frame's body for a computer: in: secret(32) K(32) account id(16)
   -> out: nonce(24) mac(16) sealed(48). hrOnLink opens it. */
HKW(hkw_link) int hkw_link(void) { return hkLinkSeal(hkw_out, hkw_in, hkw_in + 32); }

/* in: text(n) -> out: its QR modules, row by row, a byte each (1 = dark).
   The side in modules, or -1. The page draws the pair code with it. */
HKW(hkw_qr) int hkw_qr(int n) {
	static hkQr q;
	char text[214];
	if (n <= 0 || n > 213) return -1;
	memcpy(text, hkw_in, (size_t)n);
	text[n] = '\0';
	if (!hkQrEncode(&q, text)) return -1;
	for (int y = 0; y < q.n; y++) memcpy(hkw_out + y * q.n, q.m[y], (size_t)q.n);
	return q.n;
}

/* :remote pair's answer, opened: in: secret(32) nonce(24) mac(16) sealed(48)
   -> out: K(32) account id(16). 0, or -1. */
HKW(hkw_link_open) int hkw_link_open(void) { return hkLinkOpen(hkw_out, hkw_in, hkw_in + 32); }
#endif

#ifdef HAKO_RELAY    /* the six-digit fallback: built by the tests until a link uses it */
/* The link check (RELAY.md §4): the side that answers commits to a nonce
   before it sees the other's, so a relay in the middle gets one random guess
   at six digits instead of grinding keys until both screens agree. */
static void hkLinkCommit(uint8_t out[32], const uint8_t nonce[32]) {
	crypto_blake2b_keyed(out, 32, (const uint8_t *)"hako link commit", 16, nonce, 32);
}

static void hkLinkDigits(char out[7], const uint8_t h[64], const uint8_t ni[32], const uint8_t nr[32]) {
	uint8_t d[8];
	crypto_blake2b_ctx c;
	crypto_blake2b_keyed_init(&c, 8, (const uint8_t *)"hako link digits", 16);
	crypto_blake2b_update(&c, h, 64);
	crypto_blake2b_update(&c, ni, 32);
	crypto_blake2b_update(&c, nr, 32);
	crypto_blake2b_final(&c, d);
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) v = (v << 8) | d[i];
	snprintf(out, 7, "%06u", (unsigned)(v % 1000000u));
}
#endif
#endif

/* Is an executable called `name` on PATH? A stat per directory, no shell. */
static int hkOnPath(const char *name) {
#ifdef HAKO_WASM
	(void)name;
	return 0;
#else
	const char *path = getenv("PATH");
	if (!path || !*path || strchr(name, '/')) return 0;
#ifdef _WIN32
	const char sep = ';';
	static const char *ext[] = { ".exe", ".cmd", ".bat", "", NULL };
#else
	const char sep = ':';
	static const char *ext[] = { "", NULL };
#endif
	for (const char *p = path; ; ) {
		const char *e = strchr(p, sep);
		size_t dl = e ? (size_t)(e - p) : strlen(p);
		for (int k = 0; dl && ext[k]; k++) {
			char full[PATH_MAX];
			struct stat st;
			if (hkFmtFits(full, sizeof(full), "%.*s/%s%s", (int)dl, p, name, ext[k])
			    && stat(full, &st) == 0 && !S_ISDIR(st.st_mode)
#ifndef _WIN32
			    && access(full, X_OK) == 0
#endif
			) return 1;
		}
		if (!e) break;
		p = e + 1;
	}
	return 0;
#endif
}

/* Append at out+j and return the new length, clamped: snprintf returns what it
   WOULD have written, so `j += snprintf(...)` runs past cap on long input. */
static size_t hkCatf(char *out, size_t j, size_t cap, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 4, 5)))
#endif
;
static size_t hkCatf(char *out, size_t j, size_t cap, const char *fmt, ...) {
	if (!out || cap == 0 || j >= cap - 1) return cap ? cap - 1 : 0;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out + j, cap - j, fmt, ap);
	va_end(ap);
	if (n < 0) { out[j] = '\0'; return j; }
	j += (size_t)n;
	return j < cap ? j : cap - 1;
}




typedef struct {
	const char *name;
	const char **frames;
	int frame_count;
	int delay_ms;
	const char **color;
} clAnim;

static const char *FRM_BRAILLE[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
static const char *FRM_DOTS[]    = {".  ", ".. ", "...", " ..", "  .", "   "};
static const char *FRM_BAR[]     = {"▏","▎","▍","▌","▋","▊","▉","█","▉","▊","▋","▌","▍","▎"};
static const char *FRM_PULSE[]   = {"⠂","⠆","⠇","⠧","⠷","⠿","⠷","⠧","⠇","⠆"};
static const char *FRM_BOUNCE[]  = {"◐","◓","◑","◒"};
static const char *FRM_GHOST[]   = {"ᗜˬᗜ","ᗜ◡ᗜ","ᗜ‿ᗜ","ᗜ◠ᗜ","ᗜ_ᗜ","ᗜ◡ᗜ"};
static const char *FRM_ARROWS[]  = {"←","↖","↑","↗","→","↘","↓","↙"};
/* One solid block walking the four edges of the cell. */
static const char *FRM_BLOCKS[]  = {"▌","▀","▐","▄"};
/* A single block breathing in place — no travel, nothing to fragment. */
/* The box filling and emptying: ▫ outline, ▣ partly filled, ▪ solid, and back down again. */
static const char *FRM_PUMP[]    = {"\xE2\x96\xAB","\xE2\x96\xA3","\xE2\x96\xAA","\xE2\x96\xA3"};
/* The tab title gets its own pair — half-filled squares read as motion at
   the small size a tab renders, where ▫/▪ are nearly identical. */
static const char *FRM_TITLE[]   = {"\xE2\x97\xA7","\xE2\x97\xA8"};

static const clAnim CL_ANIMS[] = {
	{"braille", FRM_BRAILLE, 10, 80,  &TH_TOOL},
	{"dots",    FRM_DOTS,    6,  140, &TH_AI},
	{"bar",     FRM_BAR,     14, 70,  &TH_ACCENT},
	{"pulse",   FRM_PULSE,   10, 100, &TH_OK},
	{"bounce",  FRM_BOUNCE,  4,  150, &TH_USER},
	{"ghost",   FRM_GHOST,   6,  220, &TH_AI},
	{"arrows",  FRM_ARROWS,  8,  90,  &TH_TOOL},
	{"blocks",  FRM_BLOCKS,  4,  160, &TH_ACCENT},
	{"pump",    FRM_PUMP,    4,  180, &TH_ACCENT},
};
static const int CL_ANIM_COUNT = sizeof(CL_ANIMS) / sizeof(CL_ANIMS[0]);

/* One stock spinner, chosen not rolled: the corner that travels the box, the… */
#define CL_ANIM_DEFAULT 8           /* "pump": one block growing small to big */
#define CL_LABEL_DEFAULT 0          /* "thinking" */

/* The mark that stands in for hako where a glyph has to do the work: window
   title, splash, anywhere the wordmark will not fit. */
#define HK_MARK "\xE2\x96\xA3"    /* ▣ a box with a block inside */

static const char *clStrCaseStr(const char *hay, const char *needle);

/* Which tool is executing right now, or NULL. A tool in flight says far more
   about what the turn is doing than the wording of the request does. */
static const char *hk_cur_tool = NULL;
static char hk_cur_subject[128] = "";   /* what the tool in flight is working on */

/* Pick the word the spinner shows. */
/* The spinner label is flavour, not a status readout — a literal "reading…"… */
static const char *hkVerbFlavour(const char *base, int matched) {
	static const char *idle[] = {
		"thinking", "pondering", "mulling", "chewing on it", "turning it over",
		"consulting the box", "rummaging", "lining it up", "considering",
		"working it out", "sizing it up", "sleeping on it"
	};
	static const struct { const char *base; const char *alt[4]; } fam[] = {
		{ "reading",   { "reading", "skimming", "poring over", "having a look" } },
		{ "searching", { "searching", "hunting", "casting about", "sniffing around" } },
		{ "digging",   { "digging", "excavating", "getting to the bottom of it", "prodding" } },
		{ "building",  { "building", "assembling", "putting it together", "hammering" } },
		{ "reasoning", { "reasoning", "working it through", "joining the dots", "explaining" } },
		{ "reshaping", { "reshaping", "tidying", "rearranging", "straightening up" } },
		{ "planning",  { "planning", "plotting", "mapping it out", "scheming" } },
		{ "writing",   { "writing", "drafting", "setting it down", "composing" } },
		{ "editing",   { "editing", "revising", "touching it up", "adjusting" } },
		{ "running",   { "running", "executing", "turning the crank", "kicking it off" } },
		{ "fetching",  { "fetching", "reaching out", "collecting", "retrieving" } },
		{ "listening", { "listening", "all ears", "here", "with you" } },
	};
	/* One roll per turn, not per frame: the label must not flicker. */
	unsigned r = (unsigned)(hk_time_unix() ^ (unsigned)E.session_turn_count * 2654435761u);
	if (!matched) return idle[r % (sizeof(idle)/sizeof(idle[0]))];
	for (size_t i = 0; i < sizeof(fam)/sizeof(fam[0]); i++)
		if (!strcmp(fam[i].base, base)) {
			if (r % 4 == 0) return fam[i].base;     /* plain often enough to stay legible */
			return fam[i].alt[r % 4];
		}
	return base;
}

static const char *hkTurnVerb(const char *tool, const char *prompt) {
	if (tool && *tool) {
		if (strstr(tool, "read") || strstr(tool, "list"))   return "reading";
		if (strstr(tool, "grep") || strstr(tool, "glob"))   return "searching";
		if (strstr(tool, "write"))                          return "writing";
		if (strstr(tool, "edit"))                           return "editing";
		if (strstr(tool, "shell") || strstr(tool, "run"))   return "running";
		if (strstr(tool, "fetch") || strstr(tool, "web"))   return "fetching";
		if (strstr(tool, "todo") || strstr(tool, "memory")) return "planning";
		if (strstr(tool, "ask"))                            return "waiting on you";
		if (strstr(tool, "skill"))                          return "reading";
		return "working";
	}
	if (!prompt || !*prompt) return hkVerbFlavour("thinking", 0);
	/* Shell habits typed at the agent. */
	if (!strncmp(prompt, "ls", 2) || !strncmp(prompt, "dir", 3) || !strncmp(prompt, "tree", 4)
	 || !strncmp(prompt, "cat ", 4) || !strncmp(prompt, "pwd", 3)) return hkVerbFlavour("reading", 1);
	if (!strncmp(prompt, "hi", 2) || !strncmp(prompt, "hey", 3) || !strncmp(prompt, "hello", 5)
	 || !strncmp(prompt, "yo", 2) || !strncmp(prompt, "thanks", 6)) return hkVerbFlavour("listening", 1);
	/* Verb first, since "fix the build" and "explain the build" are different
	   jobs that share a noun. */
	if (clStrCaseStr(prompt, "fix ")   || clStrCaseStr(prompt, "debug")
	 || clStrCaseStr(prompt, "broken") || clStrCaseStr(prompt, "error")
	 || clStrCaseStr(prompt, "crash")  || clStrCaseStr(prompt, "why does")) return hkVerbFlavour("digging", 1);
	if (clStrCaseStr(prompt, "explain") || clStrCaseStr(prompt, "how does")
	 || clStrCaseStr(prompt, "what is") || clStrCaseStr(prompt, "why ")) return hkVerbFlavour("reasoning", 1);
	if (clStrCaseStr(prompt, "test") || clStrCaseStr(prompt, "build")
	 || clStrCaseStr(prompt, "compile") || clStrCaseStr(prompt, "make ")) return hkVerbFlavour("building", 1);
	if (clStrCaseStr(prompt, "refactor") || clStrCaseStr(prompt, "clean up")
	 || clStrCaseStr(prompt, "rename") || clStrCaseStr(prompt, "tidy")) return hkVerbFlavour("reshaping", 1);
	if (clStrCaseStr(prompt, "write") || clStrCaseStr(prompt, "add ")
	 || clStrCaseStr(prompt, "create") || clStrCaseStr(prompt, "implement")
	 || clStrCaseStr(prompt, "make a")) return hkVerbFlavour("building", 1);
	if (clStrCaseStr(prompt, "find") || clStrCaseStr(prompt, "search")
	 || clStrCaseStr(prompt, "where ") || clStrCaseStr(prompt, "look for")) return hkVerbFlavour("searching", 1);
	if (clStrCaseStr(prompt, "read") || clStrCaseStr(prompt, "show ")
	 || clStrCaseStr(prompt, "list") || clStrCaseStr(prompt, "open ")) return hkVerbFlavour("reading", 1);
	if (clStrCaseStr(prompt, "plan") || clStrCaseStr(prompt, "design")
	 || clStrCaseStr(prompt, "should i") || clStrCaseStr(prompt, "approach")) return hkVerbFlavour("planning", 1);
	if (clStrCaseStr(prompt, "review") || clStrCaseStr(prompt, "check ")
	 || clStrCaseStr(prompt, "look at")) return hkVerbFlavour("reading", 1);
	return hkVerbFlavour("thinking", 1);
}

static const char *CL_LABELS[] = {
	"thinking",
	"pondering",
	"considering",
	"computing",
	"reasoning",
	"reading",
	"plotting",
	"musing",
	"weaving",
	"chewing on it",
	"consulting the model",
	"boxing it up",
};
static const int CL_LABEL_COUNT = sizeof(CL_LABELS) / sizeof(CL_LABELS[0]);

#define CL_WORD_CELLS 33
#define CL_WORD_FITS(cols) ((cols) >= CL_WORD_CELLS * 2 + 2)

static const char *CL_LOGO_WORD[] = {
	"██╗  ██╗ █████╗ ██╗  ██╗ ██████╗",
	"██║  ██║██╔══██╗██║ ██╔╝██╔═══██╗",
	"███████║███████║█████╔╝ ██║   ██║",
	"██╔══██║██╔══██║██╔═██╗ ██║   ██║",
	"██║  ██║██║  ██║██║  ██╗╚██████╔╝",
	"╚═╝  ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝",
	NULL
};

static const char *CL_LOGO_MEDIUM[] = {
	"⠀⠀⠀⠀⠀⠀⠀⢀⣀⡀⠀⠀⠀⠀⠀⠀⢀⣀⡀⠀⠀⠀⠀⠀⠀⠀",
	"⠀⠀⠀⢀⣠⣶⣾⣿⣿⣿⣦⡀⠀⠀⢀⣴⣿⣿⣿⣷⣶⣄⡀⠀⠀⠀",
	"⣠⣴⣾⣿⣿⣿⣿⣿⣿⠿⠛⠉⢠⡄⠉⠛⠿⣿⣿⣿⣿⣿⣿⣷⣦⣄",
	"⠀⠻⣿⣿⣿⡿⠟⠋⠁⠀⠀⠀⢸⡇⠀⠀⠀⠈⠙⠻⢿⣿⣿⣿⠟⠁",
	"⠀⠀⠈⠋⠁⠀⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠈⠙⠁⠀⠀",
	"⠀⠀⣰⣷⣦⣄⡀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⢀⣠⣴⣾⣆⠀⠀",
	"⢠⣾⣿⣿⣿⣿⣿⣷⣦⣄⠀⠀⢸⡇⠀⠀⣠⣴⣾⣿⣿⣿⣿⣿⣷⡄",
	"⠙⠿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠖⠀⠀⠲⣿⣿⣿⣿⣿⣿⣿⣿⣿⠿⠋",
	"⠀⠀⠀⠉⠛⠿⣿⣿⣿⠟⢁⣴⡇⢸⣦⡈⠻⣿⣿⣿⠿⠛⠉⠀⠀⠀",
	"⠀⠀⠀⢸⣷⣦⣄⡉⢁⣴⣿⣿⡇⢸⣿⣿⣦⡈⢉⣠⣴⣾⡇⠀⠀⠀",
	"⠀⠀⠀⠈⠙⠻⢿⣿⣿⣿⣿⣿⡇⢸⣿⣿⣿⣿⣿⡿⠟⠋⠁⠀⠀⠀",
	"⠀⠀⠀⠀⠀⠀⠀⠈⠙⠻⢿⣿⡇⢸⣿⡿⠟⠋⠁⠀⠀⠀⠀⠀⠀⠀",
	"⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⠁⠈⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
	NULL
};

static const char *CL_LOGO_TINY[] = {
    " ______ ",
	"|      |",
	"|      |",
	"|      |",
	"|______|",
	NULL
};

static void aiAddHistory(aiData *data, const char *text);
static void aiSayf(aiData *data, const char *fmt, ...);
static void aiAddHistoryRole(aiData *data, const char *text, unsigned char role);
static void aiPushMessage(aiData *data, const char *role, const char *content);
static void aiFlattenMessages(aiData *data);
static char *aiExtractStringValue(const char *p);
static void aiPushMessageRaw(aiData *data, const char *role, const char *content_json);
static void aiPushMessageBody(aiData *data, const char *role, const char *body_fields);
static void aiFreeMessages(aiData *data);
static void aiWorkerSend(aiData *data);
static void *aiWorkerThread(void *arg);
static int hkHandleSlash(aiData *data, const char *prompt);
static char *hkBuildToolsSchema(int provider_format);
static int clPromptYN(const char *q, int default_yes);
static int hkToolApproval(const char *name, const char *input_json);
typedef void (*clPopupPreview)(int idx, char *out, size_t cap);
/* Called when the highlight MOVES to a new row — never on a key that does not… */
typedef void (*clPopupOnMove)(int idx);
static int clPopupSelectLive(const char *title, const char **items, int n, int cur,
                             clPopupPreview preview, clPopupOnMove on_move);
static void hkThemePreviewApply(int idx);
enum { HK_RC_ANY, HK_RC_CODE, HK_RC_EDIT, HK_RC_STUDIO };   /* whose key: plain, code., edit., studio. */
#define HK_CF_TURN  1     /* works while a turn runs */
#define HK_CF_TTY   2     /* draws in the terminal */
#define HK_CF_ALIAS 4     /* another name for the row above: menus leave it out */
typedef struct { const char *n; char p; const char *c, *a, *d; unsigned f; } hkCmd;   /* a command (HK_CMDS) */
static int   hkCmdCount(void);
static hkCmd hkCmdAt(int i);
static int   hkCmdMatch(char typed_p, const char *w, size_t wl, int *out, int max);
static int hkThemeNames(const char **out, int max);
static int hkRcSaveTheme(int tool, const char *name);
static int hkRcSet(const char *key, const char *val);
static const char *hkRcGet(int tool, const char *key);
static void hkRcProjectLoad(aiData *data);
static const char *hk_theme_pick[64];     /* what the theme picker lists, for its live preview */
static int clPopupSelect(const char *title, const char **items, int n, int cur, clPopupPreview preview);
static const char *hkProviderName(enum aiProviderType t);
static const char *hkProviderLabel(void);
static enum aiProviderType hkParseProvider(const char *s);
static const char *hkProviderDefaultEndpoint(const char *s);
static void hkApplyProviderAlias(const char *val);
static int hkProjectTrusted(void);
static int hkGrantProjectTrust(void);
static void hkSaveSession(void);
static void hkLoadSession(void);
static void hkGenSessionId(void);
static void hkRestoreTerm(void);
static void hkTranscriptView(aiData *data);
static int  cl_raw_active = 0;
static int  clEnableRaw(void);
static void clDisableRaw(void);
static int  clReadByteTimeout(char *c, int ms);
static void hkSessionFresh(aiData *data);
static void aiClearHistory(aiData *data);
static int hkHealEndpoint(void);
static void hkServePush(const char *json);
static int hkServeAnswer(char *out, size_t cap);
static int hkServeInput(const char *prompt, int hidden, char *out, size_t cap);
static void hkLogMessage(const char *role, const char *content);
static void hkLoadHistoryTail(aiData *data, int max_msgs, int ctx_msgs);
static void hkReplayTranscript(aiData *data);
static char *hkStripToolBlocks(const char *src);
static void hkAbortTurn(void);
static void hkVoiceStop(void);
static void hkStateExtras(char **b, size_t *len, size_t *cap, int tool, int with_cmds);
static int  hkUpdateLatest(char *tag, size_t cap, int fetch);
static void hkUpdateNote(const char *tag);
static int  hkVoiceCommand(char *cmd, size_t cap, const char *file);
static int  hkVoiceSpeak(const char *md, char *why, size_t wcap);
static const char *hkLastReply(aiData *data);
static int hkLoadSkills(aiData *data);
static int hkBuildPrompt(aiData *data);
static void hkPromptInvalidate(void);
static char *hkJsonUnescape(const char *s, int len);
static void hkAnnounceTool(aiData *data, const char *fname, const char *args_obj);
static void hkAnnounceToolResult(aiData *data, const char *result);
static char *hkExtractJsonString(const char *src, const char *key);
static int hkExtractJsonInt(const char *src, const char *key);
static char *hkExtractJsonObject(const char *src, const char *key);
static void hkUpdateUsage(aiData *data, const char *resp);
static int aiBuildRequest(aiData *data, enum aiProviderType type, hkReqParts *out);
static int clOAuthAnthropic(aiData *data);
static int clOAuthAnthropicFinish(aiData *data, const char *code);
static int clOAuthGithubCopilot(aiData *data);
static int clOAuthCopilotExchange(aiData *data);
static int clOAuthGithubModels(aiData *data);
static int clOAuthOpenRouter(aiData *data);
static int clOAuthChatGPT(aiData *data);
static int clOAuthChatGPTRefresh(aiData *data);
static int clOAuthNous(aiData *data);
static void hkHealModelForProvider(aiData *data);
static int clOAuthRefresh(aiData *data);
static void clOAuthEnsureFresh(aiData *data);
static void clOAuthEnsureFresh(aiData *data);
static char *clOAuthRandomVerifier(void);
#ifndef _WIN32
static int clOAuthLoopbackListen(int *out_port);
static char *clOAuthLoopbackWait(int srv_fd, int timeout_sec);
#endif
static void clUrlEncodeInto(const char *s, char *out, size_t cap);

/* The task list is agent state, not config: it belongs to the session and is
   injected into the system prompt so compaction cannot fold it away. */
#define HK_TODO_MAX 4000
static char *hkMemCollect(int *out_count);
static int   hkMemDir(char *out, size_t cap, int global);

static char *hk_todo = NULL;

/* The last tool's full output, kept so `:o` can show what the summary elided. */
static char *hk_last_tool_out = NULL;
/* ctrl-o: show tool output in full instead of the first few lines. */
static int hk_expand_output = 0;

/* Every tool output this session, in order, kept whole. */
#define HK_TOOL_OUT_MAX 512
static char *hk_tool_outs[HK_TOOL_OUT_MAX];
static int   hk_tool_out_n = 0;
#define HK_TOOL_MARK "\x01T"          /* history line: \x01T<index> */
#define HK_CALL_MARK "\x01" "C"      /* history line: \x01C<name>(<arg>) — the ledger entry (split: \x01C is one byte) */
#define HK_RES_MARK  "\x01" "R"      /* history line: \x01R<one-line result> — under the entry */

static int hkToolOutAdd(const char *text) {
	if (hk_tool_out_n >= HK_TOOL_OUT_MAX) return -1;
	hk_tool_outs[hk_tool_out_n] = strdup(text ? text : "");
	return hk_tool_out_n++;
}
#define HK_TOOL_SHOW_LINES 8

static int  clTermCols(void);
static int  cl_cols_pinned = 0;   /* while stdout is captured, the terminal is not on fd 1 */
static int  clTermRows(void);
static double hkSessionCostUSD(aiData *data);
static const char *hkFreeTierLabel(void);
static int  clCellWidth(const char *s);
static void clClipToCells(char *s, int max_cells);
static int  clUtf8Next(const unsigned char *p, unsigned int *cp);
static int  clRuneWidth(unsigned int cp);
static int  clCellsN(const char *s, int n);

static int hk_bench_mode;
static int hk_bench_calls;
static char hk_bench_tool[80];
static char hk_bench_args[2048];

static char *clComposeInEditor(const char *seed);

/* ---------- the pinned bar ---------- */
/* A color that belongs to the conversation rather than to the theme, so two… */
static const struct { const char *name; unsigned rgb; } HK_CHAT_COLORS[] = {
	{ "gold",   0xb89656 }, { "green",  0x7a8a3a },
	{ "blue",   0x5c8cbe }, { "cyan",   0x56aaaa },
	{ "red",    0xbe5a50 }, { "purple", 0x966ebe },
	{ "orange", 0xc88246 }, { "pink",   0xc878a0 },
};
#define HK_CHAT_COLOR_COUNT ((int)(sizeof(HK_CHAT_COLORS)/sizeof(HK_CHAT_COLORS[0])))

static const char *hkChatColor(void) {
	if (E.session_color && *E.session_color)
		for (int i = 0; i < HK_CHAT_COLOR_COUNT; i++)
			if (!strcasecmp(E.session_color, HK_CHAT_COLORS[i].name))
				return clSeq(HK_CHAT_COLORS[i].rgb);
	return TH_ACCENT;
}

/* The chat name is HIGHLIGHTED — the color behind it, the text readable through it. */
static const char *hkChatBadge(void) {
	static char buf[64];
	snprintf(buf, sizeof(buf), "%s\x1b[7m\x1b[1m", hkChatColor());
	return buf;
}

/* The bar is drawn IN FLOW, immediately below whatever was printed last, using relative movement only. */
static int cl_bar_h = 0;         /* rows the bar currently occupies, 0 = hidden */
static int cl_bar_cols = 0;      /* width it was drawn at; a resize invalidates the count */
static int cl_input_rows = 1;    /* rows the typed line occupies once wrapped */
static int cl_field_rows = 1;    /* rows the bar reserves for the field; grows with the text */

/* While a turn runs the user can keep typing. */
static int cl_redraw_oldrows;
static int cl_redraw_oldrpos;
static pthread_mutex_t cl_tty_lock;
static pthread_once_t cl_tty_once = PTHREAD_ONCE_INIT;

static void clTtyLockMake(void) {
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&cl_tty_lock, &a);
	pthread_mutexattr_destroy(&a);
}

static void clTtyLock(void)   { pthread_once(&cl_tty_once, clTtyLockMake); pthread_mutex_lock(&cl_tty_lock); }
static void clTtyUnlock(void) { pthread_mutex_unlock(&cl_tty_lock); }
static int cl_composing = 0;
static const char *cl_line_prompt = NULL;
static const char *cl_line_buf = NULL;
static int cl_line_len = 0, cl_line_cur = 0;
static void clRedrawLine(const char *prompt, const char *buf, int len, int cursor);
static int cl_bar_field = 0;     /* rows from the top of the bar down to the field */
static int cl_bar_want = 0;      /* the bar should be up whenever output is idle */
static int cl_out_depth = 0;     /* >0 while something is printing into the flow */

/* Set while the viewport redraw is working, so the print brackets its own
   captures and painting go through are not mistaken for ordinary output. */
static int cl_in_vp = 0;

static volatile sig_atomic_t cl_winch = 0;
static int hk_queue_n;           /* messages typed during a turn, waiting to run */
/* --serve --token or :remote on: reachable from another device (the relay, LAN),
   so every tool asks: no auto mode, no "always" rules, set only at the desktop. */
static int hs_remote = 0;
static volatile int hs_attached = 0;       /* :remote on, inside this terminal session */
static volatile int hs_streams = 0;        /* devices watching (open event streams) */
static char hk_update_latest[32];          /* "0.2.11" when a newer release exists */
static volatile int hk_update_ready = 0;   /* set once hk_update_latest is written */
static void hkAllowSummary(char *out, size_t cap);

/* A drag sends resizes faster than a bar can be drawn: one drawn at one width
   lands after the terminal has moved to the next, and the following erase
   climbs short, leaving its top rule behind. So nothing redraws until the
   width has held still this long. */
#define CL_RESIZE_SETTLE_MS 150
static volatile long cl_resize_until = 0;
static int clResizing(void) { return cl_resize_until && hk_time_ms() < cl_resize_until; }

static int clBarUsable(void) {
	if (E.pipe_mode || E.output_json || !E.color_enabled) return 0;
	if (!isatty(STDOUT_FILENO)) return 0;
	return clTermRows() >= 8;        /* below this it is all bar and no room */
}

/* ---- the command menu: rows of the bar block itself, under the field, so
   the bar's hide/show/resize rules already cover it (DESIGN 2026-10-29 §2) ---- */
#define CL_MENU_ROWS 8
#define CL_MENU_CAP  128
static int  cl_menu_live = 0;        /* the line reader is up */
static int  cl_menu_n = 0;           /* rows it takes now, 0 = closed */
static int  cl_menu_idx[CL_MENU_CAP];/* matches, as hkCmdAt rows */
static int  cl_menu_count = 0;
static int  cl_menu_sel = 0, cl_menu_top = 0, cl_menu_moved = 0;
static int  cl_menu_hidden = 0;      /* esc: closed until the line changes */
static long cl_menu_drawn = 0;       /* what the bar last drew, 0 = none */
static char cl_menu_word[64];

static long clMenuCompute(const char *buf, int len) {
	cl_menu_n = 0;
	int rows = clTermRows(), cols = clTermCols();
	if (!cl_menu_live || cl_menu_hidden || !buf || len < 1 || rows < 12) return 0;
	if (buf[0] != ':' && buf[0] != '/') return 0;
	if (memchr(buf, ' ', (size_t)len) || len > 60) return 0;
	char word[64];
	snprintf(word, sizeof word, "%.*s", len - 1, buf + 1);
	if (strcmp(word, cl_menu_word)) {                /* a new word: back to the top */
		snprintf(cl_menu_word, sizeof cl_menu_word, "%s", word);
		cl_menu_sel = cl_menu_top = cl_menu_moved = 0;
	}
	cl_menu_count = hkCmdMatch(buf[0], word, strlen(word), cl_menu_idx, CL_MENU_CAP);
	if (cl_menu_count <= 0) return 0;
	int maxr = rows / 3 < CL_MENU_ROWS ? rows / 3 : CL_MENU_ROWS;
	if (cl_menu_sel >= cl_menu_count) cl_menu_sel = cl_menu_count - 1;
	if (cl_menu_sel < 0) cl_menu_sel = 0;
	cl_menu_n = cl_menu_count < maxr ? cl_menu_count : maxr;
	if (cl_menu_sel < cl_menu_top) cl_menu_top = cl_menu_sel;
	if (cl_menu_sel >= cl_menu_top + cl_menu_n) cl_menu_top = cl_menu_sel - cl_menu_n + 1;
	long sig = 1;
	for (const char *q = word; *q; q++) sig = sig * 131 + (unsigned char)*q;
	sig = sig * 1000003L + cl_menu_sel * 4099L + cl_menu_top * 97L + cl_menu_count * 13L + cl_menu_n
	    + (long)cols * 7919L + (long)rows * 31L;
	return sig ? sig : 1;
}

/* One menu row, full width: `:theme  [name]  pick a theme ... settings`. */
static void clMenuRow(int vi, int cols, int last) {
	hkCmd c = hkCmdAt(cl_menu_idx[vi]);
	int sel = vi == cl_menu_sel, wide = cols >= 50;
	int nw = 0, aw = 0;
	for (int k = cl_menu_top; k < cl_menu_top + cl_menu_n; k++) {
		hkCmd o = hkCmdAt(cl_menu_idx[k]);
		int l = 1 + (int)strlen(o.n), al = (int)strlen(o.a);
		if (l > nw) nw = l;
		if (al > aw) aw = al;
	}
	if (nw > 20) nw = 20;
	if (aw > 18) aw = 18;
	char name[64], row[512];
	snprintf(name, sizeof name, "%c%.40s", c.p, c.n);
	size_t o = 0;
	row[0] = '\0';
	o = hkCatf(row, o, sizeof row, "  %s%s%-*s%s", sel ? TH_ACCENT : TH_AI, sel ? "\x1b[7m" : "", nw, name, ANSI_RESET);
	int used = 2 + (nw > (int)strlen(name) ? nw : (int)strlen(name));
	if (wide) {
		char tail[200];
		snprintf(tail, sizeof tail, "  %-*.*s  %s", aw, aw, c.a, c.d);
		int room = cols - 1 - used - (int)strlen(c.c) - 2;
		if (room > 4) {
			clClipToCells(tail, room);
			int tw = clCellWidth(tail);
			o = hkCatf(row, o, sizeof row, "%s%s%s", TH_META, tail, ANSI_RESET);
			used += tw;
			int gap = cols - 1 - used - (int)strlen(c.c);
			if (gap >= 2) { o = hkCatf(row, o, sizeof row, "%*s%s%s%s", gap, "", TH_GHOST, c.c, ANSI_RESET); used += gap + (int)strlen(c.c); }
		}
	}
	if (used < cols - 1) o = hkCatf(row, o, sizeof row, "%*s", cols - 1 - used, "");
	if (clCellWidth(row) > cols - 1) clClipToCells(row, cols - 1);   /* never the last column */
	printf("\x1b[2K%s%s%s", row, ANSI_RESET, last ? "" : "\n");
}

static int clBarUpToTop(void) {
	int up = cl_bar_field;
	if (cl_composing && cl_redraw_oldrpos > 0) up += cl_redraw_oldrpos;
	return up;
}

/* Erase the bar and leave the cursor exactly where output should continue. */
static void clBarHideRaw(void) {
	if (!cl_bar_h) return;
	int up = clBarUpToTop();
	if (clTermCols() != cl_bar_cols)
		up = cl_bar_h * 2 + cl_input_rows * 2;   /* worst case: everything wrapped once */
	cl_redraw_oldrows = 0;                       /* the field went with it */
	cl_redraw_oldrpos = 0;
	printf("\r");
	if (up > 0) printf("\x1b[%dA", up);
	printf("\x1b[J");                /* nothing below the bar to protect */
	fflush(stdout);
	cl_bar_h = 0;
	cl_bar_field = 0;
	cl_bar_cols = 0;
}

/* Draw the bar starting at the cursor. Ends with the cursor on the field. */
static void clBarShowRaw(aiData *data) {
	if (cl_bar_h || !clBarUsable() || clResizing()) return;
	int cols = clTermCols();
	int rule_w = cols - 3; if (rule_w < 8) rule_w = 8;   /* never touch the last column */
	const char *nm = (E.session_name && *E.session_name) ? E.session_name : NULL;
	int nw = nm ? clCellWidth(nm) + 3 : 0;           /* " name " highlighted, one rule cell after it */
	if (nw > rule_w - 6) { nm = NULL; nw = 0; }

	int h = 0;
	const char *rule_c = (E.session_color && *E.session_color) ? hkChatColor() : TH_META;
	printf("\r\x1b[J");

	printf("\x1b[2K\n"); h++;                  /* breathing room above the bar; empty, so it never wraps */

	printf("\x1b[2K  %s", rule_c);            /* top rule, chat name riding it */
	for (int i = 0; i < rule_w - nw; i++) printf("\xE2\x94\x80");
	if (nm) printf("%s %s %s%s\xE2\x94\x80", hkChatBadge(), nm, ANSI_RESET, rule_c);
	{ int rp = cols - 3 - rule_w; printf("%s%*s\n", ANSI_RESET, rp > 0 ? rp : 0, ""); } h++;

	cl_bar_field = h;                          /* the field: the reader draws it */
	for (int i = 0; i < cl_field_rows; i++) { printf("\x1b[2K%*s\n", cols - 1, ""); h++; }

	printf("\x1b[2K  %s", rule_c);            /* bottom rule */
	for (int i = 0; i < rule_w; i++) printf("\xE2\x94\x80");
	{ int rp = cols - 3 - rule_w; printf("%s%*s\n", ANSI_RESET, rp > 0 ? rp : 0, ""); } h++;

	/* The command menu takes the status rows' place while it is open. */
	cl_menu_drawn = (cl_menu_live && cl_line_buf) ? clMenuCompute(cl_line_buf, cl_line_len) : 0;
	if (!cl_menu_drawn) cl_menu_n = 0;
	if (cl_menu_n > 0) {
		for (int k = cl_menu_top; k < cl_menu_top + cl_menu_n; k++) { clMenuRow(k, cols, k == cl_menu_top + cl_menu_n - 1); h++; }
		printf("\r\x1b[%dA", h - 1 - cl_bar_field);   /* back up onto the field */
		fflush(stdout);
		cl_bar_h = h;
		cl_bar_cols = cols;
		return;
	}

	if (E.ai_active_skills && *E.ai_active_skills) {
		char up[64];
		snprintf(up, sizeof(up), "%s", E.ai_active_skills);
		for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
		{ int sp = cols - 5 - (int)strlen(up);
		  printf("\x1b[2K  %s[%s]%s%*s\n", TH_OK, up, ANSI_RESET, sp > 0 ? sp : 0, ""); } h++;
	}

	const char *model = E.ai_model ? E.ai_model : "\xE2\x80\x94";
	if (!strncmp(model, "claude-", 7)) model += 7;
	else if (!strncmp(model, "gpt-", 4)) model += 4;
	else if (!strncmp(model, "gemini-", 7)) model += 7;
	char right[256]; right[0] = '\0';
	if (data && (data->total_in_tokens || data->total_out_tokens)) {
		double cost = hkSessionCostUSD(data);
		const char *flat = hkFreeTierLabel();
		char c[40]; c[0] = '\0';
		if (flat) snprintf(c, sizeof(c), "\xC2\xB7%s", flat);
		else if (cost > 0.00005) snprintf(c, sizeof(c), "\xC2\xB7$%.4f", cost);
		snprintf(right, sizeof(right), "\xE2\x86\x91%ld\xE2\x86\x93%ld%s",
		         data->total_in_tokens, data->total_out_tokens, c);
	}
	/* Three widths, because a phone in portrait is 30-something columns and a
	   bar row that wraps costs a row we did not count. */
	int eff_auto = E.ai_auto_approve && !hs_remote;    /* remote on: auto is ignored, so say ask */
	const char *mode_c = E.ai_plan_mode ? TH_TOOL : eff_auto ? TH_OK : TH_ACCENT;
	const char *mode_s = E.ai_plan_mode ? "plan" : eff_auto ? "auto" : "ask";
	char verb[64];
	snprintf(verb, sizeof(verb), "%s", hk_expand_output ? " \xC2\xB7 verbose" : "");
	if (!E.ai_plan_mode && !E.ai_auto_approve && !hs_remote) {   /* an "always" answer, visible where the mode is */
		char al[48]; hkAllowSummary(al, sizeof(al));
		if (*al) snprintf(verb + strlen(verb), sizeof(verb) - strlen(verb), " \xC2\xB7 always %s", al);
	}
	if (hk_queue_n > 0)   /* in the bar, not the chat: a chat line under a streaming answer is overwritten by it */
		snprintf(verb + strlen(verb), sizeof(verb) - strlen(verb), " \xC2\xB7 %d queued", hk_queue_n);
	/* remote and a newer release: at the right end, where they cannot be missed,
	   and kept when the token count has to go (Claude Code's /rc and "update") */
	char marks[128]; marks[0] = '\0';
	{
		size_t mo = 0;
		if (hs_attached) {
			if (hs_streams > 0) mo = hkCatf(marks, mo, sizeof marks, "%s\x1b[1mremote %d%s", TH_OK, hs_streams, ANSI_RESET);
			else mo = hkCatf(marks, mo, sizeof marks, "%s\x1b[1mremote%s", TH_OK, ANSI_RESET);
		}
		if (hk_update_ready)
			mo = hkCatf(marks, mo, sizeof marks, "%s%supdate %s%s", mo ? "  " : "", TH_ACCENT, hk_update_latest, ANSI_RESET);
	}
	char left[256];
	if (cols >= 64)
		snprintf(left, sizeof(left), "  %s\xE2\x96\xB6\xE2\x96\xB6 %s mode%s %s(shift+tab)%s  %s%s \xC2\xB7 %s \xC2\xB7 t%d%s%s",
		         mode_c, mode_s, ANSI_RESET, TH_META, ANSI_RESET,
		         TH_ACCENT, hkProviderLabel(), model,
		         data ? E.session_turn_count : 0, TH_META, verb);
	else if (cols >= 44)
		snprintf(left, sizeof(left), "  %s\xE2\x96\xB6\xE2\x96\xB6 %s%s  %s%s \xC2\xB7 t%d%s%s",
		         mode_c, mode_s, ANSI_RESET, TH_ACCENT, model,
		         data ? E.session_turn_count : 0, TH_META, verb);
	else
		snprintf(left, sizeof(left), "  %s\xE2\x96\xB6\xE2\x96\xB6 %s%s %s%s%s%s",
		         mode_c, mode_s, ANSI_RESET, TH_ACCENT, model, TH_META, hk_queue_n > 0 ? verb : "");
	if (marks[0]) {
		char both[256];
		snprintf(both, sizeof both, "%s%s%s", marks, right[0] ? "  " : "", right);
		if (clCellWidth(left) + clCellWidth(both) + 2 <= cols - 1) snprintf(right, sizeof right, "%s", both);
		else snprintf(right, sizeof right, "%s", marks);
		if (clCellWidth(left) + clCellWidth(right) + 2 > cols - 1) {   /* a phone: the marks win over the model */
			int keep = cols - 3 - clCellWidth(right);
			if (keep > 8) clClipToCells(left, keep);
		}
	}
	if (clCellWidth(left) + clCellWidth(right) + 2 > cols - 1) right[0] = '\0';
	if (clCellWidth(left) > cols - 1) clClipToCells(left, cols - 1);
	int gap = cols - 1 - clCellWidth(left) - clCellWidth(right);
	if (gap < 1) gap = 1;
	printf("\x1b[2K%s%*s%s%s", left, gap, "", right, ANSI_RESET); h++;

	printf("\r\x1b[%dA", h - 1 - cl_bar_field);   /* back up onto the field */
	fflush(stdout);
	cl_bar_h = h;
	cl_bar_cols = cols;
}

/* Everything that prints into the conversation brackets itself with these, so
   the bar is never in the way and is always back underneath afterwards. */
/* Paint one line into the bar's top row without disturbing the cursor.
   Returns 0 if there is no bar to paint into. */
static int cl_status_up = -1;    /* rows up to the spinner row on the last paint */

static int clBarStatus(const char *body) {
	if (!cl_bar_h) return 0;
	pthread_once(&cl_tty_once, clTtyLockMake);
	if (pthread_mutex_trylock(&cl_tty_lock) != 0) return 1;   /* output owns the tty this frame */
	int cols = clTermCols();
	char buf[192];
	snprintf(buf, sizeof(buf), "%s", body ? body : "");
	int w = clCellWidth(buf);
	if (w > cols - 1) { clClipToCells(buf, cols - 1); w = clCellWidth(buf); }
	printf("\x1b" "7");                          /* where the user is typing */
	printf("\r");
	int up = clBarUpToTop();
	if (cl_status_up >= 0 && cl_status_up != up) {
		if (cl_status_up > 0) printf("\x1b[%dA", cl_status_up);
		printf("\r%*s\r", cols - 1, "");
		if (cl_status_up > 0) printf("\x1b[%dB", cl_status_up);
	}
	if (up > 0) printf("\x1b[%dA", up);
	printf("%s%*s", buf, cols - 1 - w, "");
	cl_status_up = up;
	printf("\x1b" "8");
	fflush(stdout);
	clTtyUnlock();
	return 1;
}

/* Both take the tty lock themselves: the keyboard reader redraws the bar the
   moment an approval hands the keyboard back, while the worker is printing the
   tool's output — unlocked, the two interleaved and stranded half a bar. */
static void clBarShow(aiData *data) { clTtyLock(); clBarShowRaw(data); clTtyUnlock(); }
static void clBarHide(void)          { clTtyLock(); clBarHideRaw();     clTtyUnlock(); }

static void clOutBegin(void) {
	clTtyLock();                            /* recursive; released in clOutEnd */
	if (cl_out_depth++ == 0) clBarHide();
}

static void clOutEnd(aiData *data) {
	if (cl_out_depth > 0 && --cl_out_depth == 0) {
		if (cl_bar_want && !clResizing()) {
			clBarShow(data);
			/* Put back whatever was being typed when this output arrived. */
			if (cl_composing && cl_line_buf) {
				cl_redraw_oldrows = 0;
				cl_redraw_oldrpos = 0;
				clRedrawLine(cl_line_prompt, cl_line_buf, cl_line_len, cl_line_cur);
			}
		}
	}
	clTtyUnlock();
}

static void clBarTeardown(void) {
	cl_bar_want = 0;
	clBarHide();
}

/* Raised by the worker when a turn finishes, so a blocking read for the next… */
static volatile sig_atomic_t cl_turn_done = 0;
#if !defined(_WIN32) && !defined(HAKO_WASM)
/* A resize reflows the scrollback under us. Take the bar down and put it back:
   both halves are relative, so both are correct at the new size. */
static void clWinchHandler(int sig) { (void)sig; cl_winch = 1; }
static void clTurnDoneHandler(int sig) { (void)sig; cl_turn_done = 1; }
#endif

static pthread_t g_main_thread;
static int g_main_thread_set = 0;

static volatile sig_atomic_t hk_need_stdin = 0;    /* the worker wants the keyboard */
static volatile sig_atomic_t hk_stdin_free = 0;    /* the reader has let go of it */

/* Called on the worker thread around anything that reads from the user. */
static void hkClaimStdin(void) {
	/* Do NOT skip this when cl_composing looks false. The reader clears it
	   while it parks and sets it again after, so a tool call landing in that
	   gap used to skip the claim entirely and read the tty while the reader
	   was also reading it. Two readers on one fd: the user's "y" goes to the
	   wrong one, and the approval either waits forever (the freeze after a
	   successful edit) or sees something it did not ask for and reports a
	   denial nobody made. The reader tests hk_need_stdin before every read,
	   so it always answers. */
	hk_stdin_free = 0;
	hk_need_stdin = 1;
	/* No REPL means no keyboard reader to hand over (--pipe, --serve, -p): waiting
	   for its ack cost five seconds before every approval in the Rei pane and the
	   studio. g_main_thread_set never changes once set, so this cannot race. */
	if (!g_main_thread_set) { hk_stdin_free = 1; return; }
#if !defined(_WIN32) && !defined(HAKO_WASM)
	pthread_kill(g_main_thread, SIGUSR1);
#endif
	for (int i = 0; i < 500 && !hk_stdin_free; i++) cl_sleep_ms(10);
}

static void hkReleaseStdin(void) { hk_need_stdin = 0; }

/* Only the thread that reads the keyboard wants SIGWINCH. */
static void hkBlockWinch(void) {
#if !defined(_WIN32) && !defined(HAKO_WASM)
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGWINCH);
	sigaddset(&set, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
#endif
}

static void clWinchInstall(void) {
#if !defined(_WIN32) && !defined(HAKO_WASM)
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = clWinchHandler;   /* no SA_RESTART: read() must return EINTR */
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = clTurnDoneHandler;
	sigaction(SIGUSR1, &sa, NULL);
#endif
}

static char *cl_preset_input = NULL;
static char *aiExtractResponse(const char *json, enum aiProviderType type, int *is_err);
static char *hkExecTool(const char *name, const char *input_json);
static char *hkExtractContentArray(const char *response);
static char *hkExtractRawJsonArray(const char *src, const char *key);
static void  hkMcpInit(void);
static void  hkMcpShutdown(void);
static char *hkMcpCallTool(const char *qualified_name, const char *args_json);
static void  hkMcpList(aiData *data);
static char *hkMcpLocalToolsPrompt(void);
static char *hkBuildToolResults(aiData *data, const char *content_array);
static int hkFnToolExecAll(aiData *data, const char *response);
static int hkReactToolExecAll(aiData *data, const char *content,
	char ***xseen, int *xseen_n, int *xseen_cap, int *dups, int *truncated);
static void clStartAnim(aiData *data);
static void clStopAnim(aiData *data);

#ifndef HAKO_WASM

static long hk_time_ms(void) {
#ifdef _WIN32
	return (long)GetTickCount64();
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long)tv.tv_sec * 1000L + (long)(tv.tv_usec / 1000);
#endif
}

static long hk_time_unix(void) { return (long)time(NULL); }

static char *hk_slurp(FILE *fp, size_t cap_max) {
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) return NULL;
	size_t n;
	while ((n = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
		len += n;
		if (cap_max && len >= cap_max) break;
		if (len + 1 >= cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) { free(buf); return NULL; }
			buf = nb;
		}
	}
	buf[len] = '\0';
	return buf;
}

/* Headers carry keys and tokens; on curl's argv any local user could read them with ps.
   Each -H '...' goes to a 0600 file passed as -H @file; anything else stays on the line. */
static char *hkHeadersToFile(const char *hdrs, char *path, size_t cap) {
	path[0] = '\0';
	size_t n = strlen(hdrs);
	char *rest = malloc(n + cap + 8);
	if (!rest) return NULL;
	size_t r = 0;
	int fd = -1;
	const char *p = hdrs;
	while (*p) {
		if (!strncmp(p, "-H '", 4)) {
			const char *v = p + 4, *e = strchr(v, '\'');
			if (!e) break;
			if (fd < 0) {
				snprintf(path, cap, "/tmp/hako_hdr_XXXXXX");
				if ((fd = mkstemp(path)) < 0) { path[0] = '\0'; free(rest); return NULL; }
			}
			if (write(fd, v, (size_t)(e - v)) < 0 || write(fd, "\n", 1) < 0) {
				close(fd); unlink(path); path[0] = '\0'; free(rest); return NULL;
			}
			p = e + 1;
			continue;
		}
		rest[r++] = *p++;
	}
	if (fd >= 0) { close(fd); r += (size_t)snprintf(rest + r, cap + 8, " -H @%s ", path); }
	rest[r] = '\0';
	return rest;
}

static char *hk_http_curl_cmd(const hkHttpReq *req, char *body_tmp, size_t tmp_cap, char *hdr_tmp) {
	body_tmp[0] = '\0';
	hdr_tmp[0] = '\0';
	if (req->body) {
		snprintf(body_tmp, tmp_cap, "/tmp/hako_req_XXXXXX");
		int fd = mkstemp(body_tmp);
		if (fd < 0) return NULL;
		if (write(fd, req->body, strlen(req->body)) < 0) { close(fd); unlink(body_tmp); return NULL; }
		close(fd);
	}
	char *hdrs = NULL;
	if (req->headers && *req->headers) {
		hdrs = hkHeadersToFile(req->headers, hdr_tmp, tmp_cap);
		if (!hdrs) { if (body_tmp[0]) unlink(body_tmp); return NULL; }
	}
	size_t cap = strlen(req->url) + (hdrs ? strlen(hdrs) : 0) + 576;
	char *cmd = malloc(cap);
	if (!cmd) { free(hdrs); if (body_tmp[0]) unlink(body_tmp); if (hdr_tmp[0]) unlink(hdr_tmp); return NULL; }
	int has_accept = req->headers && (strstr(req->headers, "accept:") || strstr(req->headers, "Accept:"));
	int has_ctype = req->headers && strstr(req->headers, "Content-Type:");
	char limits[64];
	limits[0] = '\0';
	if (req->follow || req->timeout_s > 0)
		snprintf(limits, sizeof(limits), "%s%s%d ",
		         req->follow ? "-L " : "",
		         req->timeout_s > 0 ? "--max-time " : "",
		         req->timeout_s > 0 ? req->timeout_s : 0);
	snprintf(cmd, cap,
		"curl -s%s %s-X %s '%s' -A 'hako-code/%s' %s%s%s%s%s 2>/dev/null",
		req->stream ? "N" : "", limits,
		req->method ? req->method : "GET", req->url, HAKO_VERSION,
		has_accept ? "" : "-H 'Accept: application/json' ",
		hdrs ? hdrs : "",
		req->body ? (has_ctype ? " --data @" : " -H 'Content-Type: application/json' --data @") : "",
		req->body ? body_tmp : "",
		"");
	for (char *p = cmd; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
	/* An odd number of quotes means something was truncated or injected; running
	   it would hand the shell an unterminated string. */
	int quotes = 0;
	for (const char *p = cmd; *p; p++) if (*p == '\'') quotes++;
	if (quotes & 1) {
		fprintf(stderr, "hako: refusing a malformed request (credential too long or corrupt)\n");
		free(cmd); free(hdrs);
		if (body_tmp[0]) unlink(body_tmp);
		if (hdr_tmp[0]) unlink(hdr_tmp);
		return NULL;
	}
	free(hdrs);
	return cmd;
}

static char *hk_http_fetch(const hkHttpReq *req) {
	if (!req || !req->url) return NULL;
	char tmp[64], htmp[64];
	char *cmd = hk_http_curl_cmd(req, tmp, sizeof(tmp), htmp);
	if (!cmd) return NULL;
	FILE *fp = popen(cmd, "r");
	free(cmd);
	char *out = fp ? hk_slurp(fp, 0) : NULL;
	if (fp) pclose(fp);
	if (tmp[0]) unlink(tmp);
	if (htmp[0]) unlink(htmp);
	return out;
}

typedef struct { FILE *fp; char tmp[64], htmp[64]; } hkHttpNative;

static void *hk_http_open(const hkHttpReq *req) {
	if (!req || !req->url) return NULL;
	if (E.debug) fprintf(stderr, "[http] %s %s stream=%d body=%zu\n",
	                     req->method ? req->method : "GET", req->url, req->stream,
	                     req->body ? strlen(req->body) : (size_t)0);
	hkHttpNative *h = calloc(1, sizeof(*h));
	if (!h) return NULL;
	char *cmd = hk_http_curl_cmd(req, h->tmp, sizeof(h->tmp), h->htmp);
	if (!cmd) { free(h); return NULL; }
	h->fp = popen(cmd, "r");
	free(cmd);
	if (!h->fp) { if (h->tmp[0]) unlink(h->tmp); if (h->htmp[0]) unlink(h->htmp); free(h); return NULL; }
	return h;
}

static char *hk_http_gets(void *handle, char *buf, int cap) {
	hkHttpNative *h = (hkHttpNative *)handle;
	if (!h || !h->fp) return NULL;
	return fgets(buf, cap, h->fp);
}

static void hk_http_close(void *handle) {
	hkHttpNative *h = (hkHttpNative *)handle;
	if (!h) return;
	if (h->fp) pclose(h->fp);
	if (h->tmp[0]) unlink(h->tmp);
	if (h->htmp[0]) unlink(h->htmp);
	free(h);
}

static char *hk_fs_read(const char *path, long max_bytes, long *out_len) {
	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz < 0) { fclose(fp); return NULL; }
	if (max_bytes > 0 && sz > max_bytes) sz = max_bytes;
	char *buf = malloc((size_t)sz + 1);
	if (!buf) { fclose(fp); return NULL; }
	size_t got = fread(buf, 1, (size_t)sz, fp);
	buf[got] = '\0';
	fclose(fp);
	if (out_len) *out_len = (long)got;
	return buf;
}

static int hk_fs_write(const char *path, const char *data, size_t len, int append) {
	FILE *fp = fopen(path, append ? "ab" : "wb");
	if (!fp) return -1;
	size_t wrote = len ? fwrite(data, 1, len, fp) : 0;
	fclose(fp);
	return wrote == len ? 0 : -1;
}

static int hk_fs_exists(const char *path, long *size, long *mtime, int *is_dir) {
	struct stat st;
	if (stat(path, &st) != 0) return 0;
	if (size)   *size  = (long)st.st_size;
	if (mtime)  *mtime = (long)st.st_mtime;
	if (is_dir) *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
	return 1;
}

static int hk_fs_mkdirp(const char *path) {
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		hk_fs_mkdirp(tmp);
		*p = '/';
	}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int hk_fs_remove(const char *path) { return unlink(path); }

#ifdef _WIN32
static char *hk_shell_capture(const char *cmd, long max_bytes, int *exit_code) {
	if (exit_code) *exit_code = -1;
	FILE *fp = popen(cmd, "r");
	if (!fp) return NULL;
	char *out = hk_slurp(fp, max_bytes > 0 ? (size_t)max_bytes : 0);
	int rc = pclose(fp);
	if (exit_code) *exit_code = WEXITSTATUS(rc);
	return out;
}
#endif


#endif

#ifdef HAKO_WASM

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static long hk_time_ms(void) {
#ifdef __EMSCRIPTEN__
	return (long)emscripten_get_now();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
#endif
}
static long hk_time_unix(void) { return (long)time(NULL); }

extern char *hkjs_http_fetch(const char *method, const char *url,
                             const char *headers, const char *body, int stream);
extern int   hkjs_fs_read(const char *path, char **out, long *len);
extern int   hkjs_fs_write(const char *path, const char *data, int len, int append);
extern int   hkjs_fs_stat(const char *path, long *size, long *mtime, int *is_dir);
extern int   hkjs_fs_mkdirp(const char *path);
extern int   hkjs_fs_remove(const char *path);
extern char *hkjs_fs_list(const char *path);

static char *hk_http_fetch(const hkHttpReq *req) {
	if (!req || !req->url) return NULL;
	return hkjs_http_fetch(req->method ? req->method : "GET", req->url,
	                       req->headers ? req->headers : "", req->body ? req->body : "", 0);
}

typedef struct { char *buf; size_t pos; } hkHttpWasm;

static void *hk_http_open(const hkHttpReq *req) {
	if (!req || !req->url) return NULL;
	char *body = hkjs_http_fetch(req->method ? req->method : "GET", req->url,
	                             req->headers ? req->headers : "",
	                             req->body ? req->body : "", req->stream);
	if (!body) return NULL;
	hkHttpWasm *h = calloc(1, sizeof(*h));
	if (!h) { free(body); return NULL; }
	h->buf = body;
	return h;
}

static char *hk_http_gets(void *handle, char *buf, int cap) {
	hkHttpWasm *h = (hkHttpWasm *)handle;
	if (!h || !h->buf || !h->buf[h->pos]) return NULL;
	int i = 0;
	while (i < cap - 1 && h->buf[h->pos]) {
		buf[i++] = h->buf[h->pos++];
		if (buf[i - 1] == '\n') break;
	}
	buf[i] = '\0';
	return i ? buf : NULL;
}

static void hk_http_close(void *handle) {
	hkHttpWasm *h = (hkHttpWasm *)handle;
	if (!h) return;
	free(h->buf);
	free(h);
}

/* WASI has no working directory; keep one and resolve relative paths to it. */
static char hk_cwd[PATH_MAX] = "/";

static char *hk_abs(const char *path, char *buf, size_t cap) {
	if (!path || path[0] == '/') return (char *)path;
	snprintf(buf, cap, "%s%s%s", hk_cwd, hk_cwd[1] ? "/" : "", path);
	return buf;
}

int chdir(const char *path) {
	char tmp[PATH_MAX];
	const char *want = hk_abs(path, tmp, sizeof(tmp));
	if (!hkjs_fs_stat(want, NULL, NULL, NULL)) { errno = ENOENT; return -1; }
	snprintf(hk_cwd, sizeof(hk_cwd), "%s", want);
	return 0;
}

char *getcwd(char *buf, size_t size) {
	if (!buf) return strdup(hk_cwd);
	snprintf(buf, size, "%s", hk_cwd);
	return buf;
}

/* Lexical only: there are no symlinks in this world, and wasi-libc's realpath… */
char *realpath(const char *path, char *out) {
	if (!path || !out) return NULL;
	char work[PATH_MAX];
	if (path[0] == '/') snprintf(work, sizeof(work), "%s", path);
	else snprintf(work, sizeof(work), "%s%s%s", hk_cwd, hk_cwd[1] ? "/" : "", path);

	char *parts[128];
	int n = 0;
	for (char *tok = strtok(work, "/"); tok && n < 128; tok = strtok(NULL, "/")) {
		if (!strcmp(tok, ".")) continue;
		if (!strcmp(tok, "..")) { if (n) n--; continue; }
		parts[n++] = tok;
	}
	size_t len = 0;
	out[0] = '\0';
	for (int i = 0; i < n; i++) {
		int w = snprintf(out + len, PATH_MAX - len, "/%s", parts[i]);
		if (w < 0 || (size_t)w >= PATH_MAX - len) return NULL;
		len += (size_t)w;
	}
	if (!len) snprintf(out, PATH_MAX, "/");
	return out;
}

static char *hk_fs_read(const char *path, long max_bytes, long *out_len) {
	char *out = NULL;
	long len = 0;
	char abs[PATH_MAX];
	path = hk_abs(path, abs, sizeof(abs));
	if (hkjs_fs_read(path, &out, &len) != 0) return NULL;
	if (max_bytes > 0 && len > max_bytes) { out[max_bytes] = '\0'; len = max_bytes; }
	if (out_len) *out_len = len;
	return out;
}

static int hk_fs_write(const char *path, const char *data, size_t len, int append) {
	char abs[PATH_MAX];
	return hkjs_fs_write(hk_abs(path, abs, sizeof(abs)), data, (int)len, append);
}

static int hk_fs_exists(const char *path, long *size, long *mtime, int *is_dir) {
	char abs[PATH_MAX];
	return hkjs_fs_stat(hk_abs(path, abs, sizeof(abs)), size, mtime, is_dir);
}

static int hk_fs_mkdirp(const char *path) {
	char abs[PATH_MAX];
	return hkjs_fs_mkdirp(hk_abs(path, abs, sizeof(abs)));
}

static int hk_fs_remove(const char *path) {
	char abs[PATH_MAX];
	return hkjs_fs_remove(hk_abs(path, abs, sizeof(abs)));
}

/* WASI offers no readdir a browser can answer, so the listing comes from the
   host the same way reads and writes do. */
static char *hk_fs_list(const char *path) {
	char abs[PATH_MAX];
	return hkjs_fs_list(hk_abs(path, abs, sizeof(abs)));
}

static char *hk_shell_capture(const char *cmd, long max_bytes, int *exit_code) {
	(void)cmd; (void)max_bytes;
	if (exit_code) *exit_code = -1;
	return strdup("error: no shell on this platform");
}
#endif


static void hkJsonEscapeInto(const char *s, char *out, int cap) {
	int j = 0;
	for (int i = 0; s[i] && j < cap - 6; i++) {
		unsigned char ch = (unsigned char)s[i];
		if (ch == '"') { out[j++] = '\\'; out[j++] = '"'; }
		else if (ch == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
		else if (ch == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
		else if (ch == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
		else if (ch == '\t') { out[j++] = '\\'; out[j++] = 't'; }
		else if (ch < 0x20) { j += snprintf(out + j, cap - j, "\\u%04x", ch); }
		else out[j++] = s[i];
	}
	out[j] = '\0';
}

static char *hkJsonUnescape(const char *s, int len) {
	char *out = malloc(len + 1);
	if (!out) return NULL;
	int j = 0;
	for (int i = 0; i < len; i++) {
		if (s[i] == '\\' && i + 1 < len) {
			i++;
			switch (s[i]) {
			case 'n': out[j++] = '\n'; break;
			case 'r': out[j++] = '\r'; break;
			case 't': out[j++] = '\t'; break;
			case '"': out[j++] = '"'; break;
			case '\\': out[j++] = '\\'; break;
			case '/': out[j++] = '/'; break;
			default: out[j++] = s[i]; break;
			}
		} else out[j++] = s[i];
	}
	out[j] = '\0';
	return out;
}

/* Walk to the value of "key", skipping matches that are not really a key.
   `from` is advanced past the match so the caller can keep looking. */
static const char *hkJsonValue(const char **from, const char *key) {
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = *from;
	while ((p = strstr(p, pat)) != NULL) {
		const char *q = p + strlen(pat);
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		if (*q != ':') { p++; continue; }
		q++;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		*from = p + 1;
		return q;
	}
	*from = NULL;
	return NULL;
}

static char *hkExtractJsonString(const char *src, const char *key) {
	if (!src) return NULL;
	const char *p = src, *q;
	while ((q = hkJsonValue(&p, key)) != NULL) {
		if (*q != '"') continue;
		q++;
		/* Walk FORWARD honouring escapes. Looking one char back cannot tell
		   `\"` (escaped quote) from `\\"` (escaped backslash, then the real
		   closing quote): a value ending in a backslash ran off the end and
		   returned NULL. In the SSE loop that NULL dropped the whole delta
		   (`if (!raw) continue;`), so any backslash landing on a delta
		   boundary silently deleted that chunk of the answer — which is how
		   `chat\n` reached the file as `chatn`. */
		const char *end = q;
		while (*end) {
			if (*end == '\\') { if (!end[1]) break; end += 2; continue; }
			if (*end == '"') break;
			end++;
		}
		if (*end != '"') return NULL;
		int len = (int)(end - q);
		char *out = malloc(len + 1);
		memcpy(out, q, len);
		out[len] = '\0';
		return out;
	}
	return NULL;
}

/* The first value found under any of the given keys. Models and gateways spell
   the same argument a dozen ways, and every tool used to open-code the chain. */
static char *hkJsonFirst(const char *src, ...) {
	va_list ap;
	va_start(ap, src);
	char *out = NULL;
	const char *key;
	while (!out && (key = va_arg(ap, const char *)) != NULL)
		out = hkExtractJsonString(src, key);
	va_end(ap);
	return out;
}

/* The LAST value for a key, not the first. An SSE stream carries usage twice —
   a placeholder up front and the real figure at the end. */
static int hkExtractJsonIntLast(const char *src, const char *key) {
	if (!src) return -1;
	const char *p = src, *q;
	int last = -1;
	while ((q = hkJsonValue(&p, key)) != NULL) {
		if (*q >= '0' && *q <= '9') last = atoi(q);
	}
	return last;
}

static int hkExtractJsonInt(const char *src, const char *key) {
	if (!src) return -1;
	const char *p = src, *q;
	while ((q = hkJsonValue(&p, key)) != NULL) {
		if (*q < '0' || *q > '9') continue;
		return atoi(q);
	}
	return -1;
}

static char *hkExtractJsonObject(const char *src, const char *key) {
	if (!src) return NULL;
	const char *p = src, *q;
	while ((q = hkJsonValue(&p, key)) != NULL) {
		if (*q != '{') continue;
		int depth = 0, in_str = 0, esc = 0;
		const char *start = q;
		while (*q) {
			if (esc) { esc = 0; q++; continue; }
			if (*q == '\\') { esc = 1; q++; continue; }
			if (*q == '"') in_str = !in_str;
			else if (!in_str) {
				if (*q == '{') depth++;
				else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
			}
			q++;
		}
		int len = (int)(q - start);
		char *out = malloc(len + 1);
		if (!out) return NULL;
		memcpy(out, start, len);
		out[len] = '\0';
		return out;
	}
	return NULL;
}

static int hkModelPriceUSDperM(const char *model, double *in_p, double *out_p) {
	if (!model || !*model) return 0;
	struct { const char *m; double in; double out; } table[] = {
		{ "claude-fable-5",   10.0,  50.0 },
		{ "claude-opus-5",     5.0,  25.0 },
		{ "claude-opus-4-8",   5.0,  25.0 },
		{ "claude-opus-4-7",   5.0,  25.0 },
		{ "claude-opus-4-6",   5.0,  25.0 },
		{ "claude-sonnet-5",   2.0,  10.0 },
		{ "claude-sonnet-4-6", 3.0,  15.0 },
		{ "claude-haiku-4-5",  1.0,   5.0 },
		{ "claude-opus-4",    15.0,  75.0 },
		{ "claude-sonnet-4",   3.0,  15.0 },
		{ "claude-haiku-4",    1.0,   5.0 },
		{ "claude-3-5-sonnet", 3.0,  15.0 },
		{ "claude-3-5-haiku",  1.0,   5.0 },
		{ "claude-3-opus",    15.0,  75.0 },
		{ "claude",            3.0,  15.0 },
		{ "gpt-4o-mini",       0.15,  0.60 },
		{ "gpt-4o",            2.50, 10.0 },
		{ "gpt-4.1",           2.0,   8.0 },
		{ "gpt-4",            30.0,  60.0 },
		{ "o1-mini",           3.0,  12.0 },
		{ "o1",               15.0,  60.0 },
		{ "gemini-1.5-pro",    1.25,  5.0 },
		{ "gemini-2.5-pro",    1.25,  5.0 },
		{ "gemini-2.5-flash",  0.075, 0.30 },
		{ "gemini-1.5-flash",  0.075, 0.30 },
		{ "gemini",            0.075, 0.30 },
		{ "deepseek-chat",     0.27,  1.10 },
		{ "deepseek",          0.27,  1.10 },
		{ "mistral-large",     2.0,   6.0 },
		{ "grok",              5.0,  15.0 },
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (strstr(model, table[i].m)) { *in_p = table[i].in; *out_p = table[i].out; return 1; }
	}
	return 0;
}

static int hkOAuthIs(const char *p) {
	return E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, p);
}

/* A subscription sign-in: no native tool schema, tools travel as prose. */
static int hkSubProse(void) {
	return hkOAuthIs("anthropic") || hkOAuthIs("chatgpt");
}

static double hkSessionCostUSD(aiData *data) {
	if (!data) return 0.0;
	if (hkSubProse()) return 0.0;
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-copilot")) return 0.0;
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-models")) return 0.0;
	if (HK_IS_OLLAMA_WIRE(E.ai_provider_type)) return 0.0;
	double ip = 0, op = 0;
	if (!hkModelPriceUSDperM(E.ai_model, &ip, &op)) return -1.0;
	return ((double)data->total_in_tokens * ip + (double)data->total_out_tokens * op) / 1e6;
}

static const char *hkFreeTierLabel(void) {
	if (hkSubProse())                                                         return "sub";
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-copilot")) return "sub";
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-models")) return "free";
	if (E.ai_provider_type == AI_PROVIDER_MITHRAEUM) return "local";
	if (E.ai_provider_type == AI_PROVIDER_OLLAMA && E.ai_endpoint &&
		(strstr(E.ai_endpoint, "localhost") || strstr(E.ai_endpoint, "127.0.0.1"))) return "local";
	if (E.ai_provider_type == AI_PROVIDER_OLLAMA) return "ollama";
	return NULL;
}

static void hkUpdateUsage(aiData *data, const char *resp) {
	if (!data || !resp) return;
	int in = hkExtractJsonInt(resp, "input_tokens");
	int out = hkExtractJsonIntLast(resp, "output_tokens");
	if (in < 0) in = hkExtractJsonInt(resp, "prompt_tokens");
	if (out < 0) out = hkExtractJsonIntLast(resp, "completion_tokens");
	if (in < 0) in = hkExtractJsonInt(resp, "prompt_eval_count");
	if (out < 0) out = hkExtractJsonInt(resp, "eval_count");
	if (in >= 0) { data->last_in_tokens = in; data->total_in_tokens += in; }
	if (out >= 0) { data->last_out_tokens = out; data->total_out_tokens += out; }
	/* Anthropic reports these separately; they are the only evidence that
	   prompt caching is doing anything, so :usage has to be able to show them. */
	int cw = hkExtractJsonInt(resp, "cache_creation_input_tokens");
	int cr = hkExtractJsonInt(resp, "cache_read_input_tokens");
	if (cw > 0) data->total_cache_write += cw;
	if (cr > 0) data->total_cache_read  += cr;
}

/* Walk the lines of a buffer read through the seam. */
static char *hkNextLine(char **save) {
	char *p = *save;
	if (!p || !*p) return NULL;
	char *nl = strchr(p, '\n');
	if (nl) { *nl = '\0'; *save = nl + 1; }
	else *save = p + strlen(p);
	return p;
}

static char *hkReadFileAll(const char *path, long max_bytes) {
	return hk_fs_read(path, max_bytes, NULL);
}

static char *hkRunShellCapture(const char *cmd, long max_bytes, int timeout_s) {
#ifdef HAKO_WASM
	(void)cmd; (void)max_bytes; (void)timeout_s;
	return strdup("error: no shell on this platform");
#else
	if (timeout_s <= 0)  timeout_s = 120;
	if (timeout_s > 600) timeout_s = 600;
#ifndef _WIN32
	/* The deadline is kept here, not by the shell. The old way (`set -m`, then
	   kill the job's process group) needs job control, which dash and busybox
	   sh refuse without a tty: on Linux and on iSh the watchdog killed nothing
	   and a hung command ran to its own end (sleep 30 with a 2 s limit: 30 s). */
	char script[] = "/tmp/hako-cmd-XXXXXX";
	int sfd = mkstemp(script);                        /* not a guessable path */
	if (sfd < 0) return strdup("error: cannot write tmp script");
	{
		size_t cl = strlen(cmd);
		int ok = write(sfd, cmd, cl) == (ssize_t)cl && write(sfd, "\n", 1) == 1;
		close(sfd);
		if (!ok) { unlink(script); return strdup("error: cannot write tmp script"); }
	}
	int pfd[2];
	if (pipe(pfd) != 0) { unlink(script); return strdup("error: cannot run the command (pipe)"); }
	long began = hk_time_ms();
	pid_t pid = fork();
	if (pid < 0) { close(pfd[0]); close(pfd[1]); unlink(script); return strdup("error: cannot run the command (fork)"); }
	if (pid == 0) {
		setpgid(0, 0);                                /* its own group, so a kill reaches its children */
		int dn = open("/dev/null", O_RDONLY);         /* never the keyboard: hako owns it */
		if (dn >= 0) { dup2(dn, 0); close(dn); }
		dup2(pfd[1], 1); dup2(pfd[1], 2);
		close(pfd[0]); close(pfd[1]);
		execl("/bin/sh", "sh", script, (char *)NULL);
		_exit(127);
	}
	setpgid(pid, pid);
	close(pfd[1]);
	char *out = NULL; size_t olen = 0, ocap = 0;
	int timed_out = 0;
	long deadline = began + (long)timeout_s * 1000;
	for (;;) {
		long now = hk_time_ms();
		long left = timed_out ? 200 : deadline - now;
		if (!timed_out && left <= 0) { kill(-pid, SIGKILL); timed_out = 1; continue; }
		struct pollfd pf = { pfd[0], POLLIN, 0 };
		int pr = poll(&pf, 1, (int)(left > 1000 ? 1000 : left));
		if (pr < 0) { if (errno == EINTR) continue; break; }
		if (pr == 0) { if (timed_out) break; continue; }
		char buf[8192];
		ssize_t n = read(pfd[0], buf, sizeof(buf));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		if (max_bytes <= 0 || olen < (size_t)max_bytes) {      /* past the cap: drain, do not keep */
			size_t take = (size_t)n;
			if (max_bytes > 0 && olen + take > (size_t)max_bytes) take = (size_t)max_bytes - olen;
			hkSbAdd(&out, &olen, &ocap, buf, take);
		}
	}
	close(pfd[0]);
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	if (!timed_out) kill(-pid, SIGKILL);                     /* reap anything it left behind */
	unlink(script);
	long took = hk_time_ms() - began;
	int rc = timed_out ? 137 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
	if (out && !*out) { free(out); out = NULL; }
#else
	char script[256];
	snprintf(script, sizeof(script), "/tmp/hako-cmd-%d.sh", (int)getpid());
	FILE *sf = fopen(script, "w");
	if (!sf) return strdup("error: cannot write tmp script");
	fputs(cmd, sf);
	fputc('\n', sf);
	fclose(sf);
	chmod(script, 0700);
	char full[512];
	snprintf(full, sizeof(full),
		"{ set -m; sh %s 2>&1 & c=$!; ( sleep %d; kill -9 -$c 2>/dev/null ) "
		">/dev/null 2>&1 & w=$!; wait $c; ec=$?; kill $w 2>/dev/null; exit $ec; } 2>/dev/null",
		script, timeout_s);

	int rc = 0;
	long began = hk_time_ms();
	char *out = hk_shell_capture(full, max_bytes, &rc);
	long took = hk_time_ms() - began;
	unlink(script);
	if (out && !*out) { free(out); out = NULL; }
#endif

	/* SIGKILL from the watchdog is 128+9, but a command can be killed for other
	   reasons; the elapsed time is what distinguishes our deadline from theirs. */
	if (rc == 137 && took >= (long)timeout_s * 1000 - 250) {
		size_t have = out ? strlen(out) : 0;
		char *e = malloc(have + 176);
		if (e) {
			snprintf(e, have + 176,
				"%s%serror: command timed out after %ds. Narrow it, run it in the "
				"background, or pass a larger timeout (max 600).",
				out ? out : "", (have && out[have-1] != '\n') ? "\n" : "", timeout_s);
			free(out);
			return e;
		}
	}
	if (!out) {
		if (rc != 0) {
			char *e = malloc(64);
			snprintf(e, 64, "(no output, exit code %d)", rc);
			return e;
		}
		return strdup("(no output)");
	}
	/* The capture stops dead at max_bytes. Silently handing back the first
	   50 KB of a long command's output reads as the whole thing — the same
	   trap as a truncated read_file, one step removed. */
	{
		size_t have = strlen(out);
		if (max_bytes > 0 && have >= (size_t)max_bytes) {
			const char *note = "\n… [output truncated at the capture limit — "
			                   "narrow the command, or pipe it through head/grep]\n";
			size_t nl = strlen(note);
			char *w = realloc(out, have + nl + 1);
			if (!w) return out;
			memcpy(w + have, note, nl + 1);
			return w;
		}
	}
	return out;
#endif
}

static char *hkListDir(const char *path) {
	return hk_fs_list(path);
}

#ifndef HAKO_WASM
static char *hk_fs_list(const char *path) {
	DIR *d = opendir(path);
	if (!d) return NULL;
	char *out = NULL;
	size_t total = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		int is_dir = 0;
#ifdef DT_DIR
		if (e->d_type == DT_DIR) is_dir = 1;
		else if (e->d_type == DT_UNKNOWN) {
			char full[PATH_MAX];
			snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
			int st_dir = 0;
			if (hk_fs_exists(full, NULL, NULL, &st_dir) && st_dir) is_dir = 1;
		}
#else
		char full[PATH_MAX];
		if (!hkFmtFits(full, sizeof(full), "%s/%s", path, e->d_name)) continue;
		int st_dir = 0;
		if (hk_fs_exists(full, NULL, NULL, &st_dir) && st_dir) is_dir = 1;
#endif
		int n = strlen(e->d_name);
		out = realloc(out, total + n + 3);
		memcpy(out + total, e->d_name, n);
		total += n;
		if (is_dir) { out[total++] = '/'; }
		out[total++] = '\n';
	}
	closedir(d);
	if (!out) return strdup("");
	out[total] = '\0';
	return out;
}
#endif


static void hkMigrateHakocToHako(void) {
	const char *home = getenv("HOME");
	if (!home) return;
	char old_path[512], new_path[512];
	snprintf(old_path, sizeof(old_path), "%s/.hakoc", home);
	snprintf(new_path, sizeof(new_path), "%s/.hako", home);
	struct stat st_old, st_new;
	if (stat(old_path, &st_old) != 0) return;
	if (stat(new_path, &st_new) == 0) return;
	if (rename(old_path, new_path) == 0) {
		fprintf(stderr, "! migrated %s → %s (v0.1.6 rename)\n", old_path, new_path);
	} else {
		fprintf(stderr, "! could not rename %s → %s (errno %d) — falling back to %s\n",
			old_path, new_path, errno, old_path);
	}
}

static void hkClawDirPath(char *out, size_t n) {
	const char *home = getenv("HOME");
	if (!home) { out[0] = '\0'; return; }
	snprintf(out, n, "%s/.hako", home);
#ifdef _WIN32
	_mkdir(out);
#else
	hk_fs_mkdirp(out);
#endif
}

static int hkProjectDirPath(char *out, size_t n) {
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) return 0;
	if (!hkFmtFits(out, n, "%s/.hako", cwd)) return 0;
	return 1;
}

static int hkEncodeCwd(char *out, size_t n) {
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) return 0;
	size_t j = 0;
	for (size_t i = 0; cwd[i] && j + 1 < n; i++) {
		char c = cwd[i];
		out[j++] = (c == '/') ? '-' : c;
	}
	out[j] = '\0';
	return 1;
}

static int hkProjectStateDir(char *out, size_t n) {
	char home_dir[512]; hkClawDirPath(home_dir, sizeof(home_dir));
	if (!home_dir[0]) return 0;
	char projects[640];
	snprintf(projects, sizeof(projects), "%s/projects", home_dir);
	hk_fs_mkdirp(projects);
	char enc[PATH_MAX];
	if (!hkEncodeCwd(enc, sizeof(enc))) return 0;
	if (!hkFmtFits(out, n, "%s/%s", projects, enc)) return 0;
	hk_fs_mkdirp(out);
	return 1;
}

static int hkProjectTrusted(void) {
#ifdef HAKO_WASM
	return 1;
#else
	/* --bench records tool calls and never runs them; scoring a model with no
	   tools offered (8/43 for every model, silently) measures nothing */
	if (hk_bench_mode) return 1;
	char dir[PATH_MAX];
	if (!hkProjectStateDir(dir, sizeof(dir))) return 0;
	char trust[PATH_MAX + 16];
	snprintf(trust, sizeof(trust), "%s/trust", dir);
	int is_dir = 0;
	return (hk_fs_exists(trust, NULL, NULL, &is_dir) && !is_dir) ? 1 : 0;
#endif
}

static int hkGrantProjectTrust(void) {
	char dir[PATH_MAX];
	if (!hkProjectStateDir(dir, sizeof(dir))) return 0;
	char trust[PATH_MAX + 16];
	snprintf(trust, sizeof(trust), "%s/trust", dir);
	/* Through the seam, not fopen: there is no open() to call in a browser. */
	char stamp[64];
	int n = snprintf(stamp, sizeof(stamp), "granted=%ld\n", hk_time_unix());
	if (hk_fs_write(trust, stamp, (size_t)n, 0) != 0) return 0;
	hkPromptInvalidate();                   /* the prompt still says "no tools" */
	char pdir[PATH_MAX];
	if (hkProjectDirPath(pdir, sizeof(pdir))) {
		hk_fs_mkdirp(pdir);
		char hako[PATH_MAX + 16];
		snprintf(hako, sizeof(hako), "%s/HAKO.md", pdir);
		if (!hk_fs_exists(hako, NULL, NULL, NULL)) {
			static const char seed[] =
				"# HAKO.md\n\nProject context for hako-code. Add notes/instructions/rules here. "
				"Loaded as system prompt context when present.\n";
			hk_fs_write(hako, seed, sizeof(seed) - 1, 0);
		}
	}
	return 1;
}

static void hkHistoryPath(char *out, size_t n) {
	char dir[PATH_MAX];
	if (hkProjectStateDir(dir, sizeof(dir)) && hkFmtFits(out, n, "%s/history.jsonl", dir))
		return;                                   /* a clipped path falls back to the global one */
	char fallback[512];
	hkClawDirPath(fallback, sizeof(fallback));
	snprintf(out, n, "%s/history.jsonl", fallback);
}

static void hkSessionLogPath(char *out, size_t n) {
	char dir[PATH_MAX];
	if (!hkProjectStateDir(dir, sizeof(dir)) || !E.session_id) {
		out[0] = '\0';
		return;
	}
	char sess_dir[PATH_MAX + 32];
	snprintf(sess_dir, sizeof(sess_dir), "%s/sessions", dir);
	hk_fs_mkdirp(sess_dir);
	/* out[0] == '\0' is how every caller already spells "no log path". */
	hkFmtFits(out, n, "%s/%s.jsonl", sess_dir, E.session_id);
}

static int hkClearSessionsInDir(const char *pdir) {
	int removed = 0;
	char hp[PATH_MAX + 32];
	snprintf(hp, sizeof(hp), "%s/history.jsonl", pdir);
	if (unlink(hp) == 0) removed++;
	char sdir[PATH_MAX + 32];
	snprintf(sdir, sizeof(sdir), "%s/sessions", pdir);
	DIR *d = opendir(sdir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char f[PATH_MAX + 64];
			if (snprintf(f, sizeof(f), "%s/%s", sdir, e->d_name) >= (int)sizeof(f)) continue;
			if (unlink(f) == 0) removed++;
		}
		closedir(d);
	}
	return removed;
}

static int hkClearSessions(int all) {
	if (!all) {
		char dir[PATH_MAX];
		if (!hkProjectStateDir(dir, sizeof(dir))) return 0;
		return hkClearSessionsInDir(dir);
	}
	const char *home = getenv("HOME"); if (!home) home = ".";
	char base[PATH_MAX];
	snprintf(base, sizeof(base), "%s/.hako/projects", home);
	DIR *d = opendir(base);
	if (!d) return 0;
	int removed = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char pdir[PATH_MAX + 64];
		if (snprintf(pdir, sizeof(pdir), "%s/%s", base, e->d_name) >= (int)sizeof(pdir)) continue;
		removed += hkClearSessionsInDir(pdir);
	}
	closedir(d);
	return removed;
}

static int hkResolveInProject(const char *path, char *out_full, size_t out_cap) {
	if (!path || !*path) return -1;
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) return -1;
	char resolved[PATH_MAX];
	char parent[PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", path);
	char *slash = strrchr(parent, '/');
	const char *filename_part = NULL;
	if (slash) {
		*slash = '\0';
		filename_part = slash + 1;
		if (!realpath(parent[0] ? parent : ".", resolved)) return -1;
	} else {
		strncpy(resolved, cwd, sizeof(resolved));
		resolved[sizeof(resolved) - 1] = '\0';
		filename_part = path;
	}
	int cwd_len = strlen(cwd);
	if (strncmp(resolved, cwd, cwd_len) != 0 ||
		(resolved[cwd_len] != '\0' && resolved[cwd_len] != '/')) return -1;
	if (filename_part && *filename_part) {
		if (!hkFmtFits(out_full, out_cap, "%s/%s", resolved, filename_part)) return -1;
	} else {
		if (!hkFmtFits(out_full, out_cap, "%s", resolved)) return -1;
	}
	return 0;
}

/* A resolved path as the model and the screen should see it: relative to the project when it
   is inside it. A long absolute path (an iCloud project) was cut mid-name in tool results. */
static const char *hkProjectRel(const char *full) {
	char cwd[PATH_MAX];
	if (!full || !getcwd(cwd, sizeof(cwd))) return full;
	size_t n = strlen(cwd);
	if (!strncmp(full, cwd, n) && full[n] == '/' && full[n + 1]) return full + n + 1;
	return full;
}

static const char *hkProviderName(enum aiProviderType t) {
	switch (t) {
	case AI_PROVIDER_OLLAMA: return "ollama";
	case AI_PROVIDER_ANTHROPIC: return "anthropic";
	case AI_PROVIDER_OPENAI: return "openai";
	case AI_PROVIDER_MITHRAEUM: return "mithraeum";
	default: return "none";
	}
}

static enum aiProviderType hkParseProvider(const char *s) {
	char base[32];
	size_t n = strlen(s);
	if (n > 4 && n - 4 < sizeof(base) && !strcmp(s + n - 4, "-api")) {
		snprintf(base, sizeof(base), "%.*s", (int)(n - 4), s);
		s = base;
	}
	if (strcmp(s, "ollama") == 0 || strcmp(s, "local") == 0
		|| strcmp(s, "ollamacloud") == 0 || strcmp(s, "ocloud") == 0
		|| strcmp(s, "ollama-cloud") == 0) return AI_PROVIDER_OLLAMA;
	if (strcmp(s, "anthropic") == 0 || strcmp(s, "claude") == 0) return AI_PROVIDER_ANTHROPIC;
	if (strcmp(s, "openai") == 0 || strcmp(s, "gpt") == 0 || strcmp(s, "groq") == 0) return AI_PROVIDER_OPENAI;
	if (strcmp(s, "deepseek") == 0 || strcmp(s, "mistral") == 0 || strcmp(s, "together") == 0
		|| strcmp(s, "fireworks") == 0 || strcmp(s, "openrouter") == 0
		|| strcmp(s, "xai") == 0 || strcmp(s, "grok") == 0
		|| strcmp(s, "gemini") == 0 || strcmp(s, "google") == 0
		|| strcmp(s, "cerebras") == 0 || strcmp(s, "nous") == 0
		|| strcmp(s, "github-copilot") == 0 || strcmp(s, "copilot") == 0
		|| strcmp(s, "github-models") == 0 || strcmp(s, "ghmodels") == 0
		|| strcmp(s, "chatgpt") == 0 || strcmp(s, "codex") == 0
		|| strcmp(s, "custom") == 0) return AI_PROVIDER_OPENAI;
	if (strcmp(s, "mithraeum") == 0 || strcmp(s, "hakm") == 0
		|| strcmp(s, "koi") == 0) return AI_PROVIDER_MITHRAEUM;
	return AI_PROVIDER_NONE;
}

static const char *hkProviderDefaultEndpoint(const char *s) {
	/* Same "-api" tolerance as the parser: the endpoint is the provider's, the
	   suffix only says how you authenticate to it. */
	static char base[32];
	size_t n = strlen(s);
	if (n > 4 && n - 4 < sizeof(base) && !strcmp(s + n - 4, "-api")) {
		snprintf(base, sizeof(base), "%.*s", (int)(n - 4), s);
		s = base;
	}
	if (strcmp(s, "ollama") == 0 || strcmp(s, "local") == 0) return "http://localhost:11434";
	if (strcmp(s, "mithraeum") == 0 || strcmp(s, "hakm") == 0 || strcmp(s, "koi") == 0)
		return "hakm://subprocess";
	if (strcmp(s, "ollamacloud") == 0 || strcmp(s, "ocloud") == 0 || strcmp(s, "ollama-cloud") == 0)
		return "https://ollama.com";
	if (strcmp(s, "deepseek") == 0)   return "https://api.deepseek.com";
	if (strcmp(s, "mistral") == 0)    return "https://api.mistral.ai";
	if (strcmp(s, "together") == 0)   return "https://api.together.xyz";
	if (strcmp(s, "fireworks") == 0)  return "https://api.fireworks.ai/inference";
	if (strcmp(s, "openrouter") == 0) return "https://openrouter.ai/api";
	if (strcmp(s, "groq") == 0)       return "https://api.groq.com/openai";
	if (strcmp(s, "xai") == 0 || strcmp(s, "grok") == 0) return "https://api.x.ai";
	if (strcmp(s, "gemini") == 0 || strcmp(s, "google") == 0) return "https://generativelanguage.googleapis.com/v1beta/openai";
	if (strcmp(s, "cerebras") == 0)   return "https://api.cerebras.ai";
	if (strcmp(s, "nous") == 0)       return "https://inference-api.nousresearch.com";
	if (strcmp(s, "github-copilot") == 0 || strcmp(s, "copilot") == 0)
		return "https://api.githubcopilot.com";
	if (strcmp(s, "github-models") == 0 || strcmp(s, "ghmodels") == 0)
		return "https://models.inference.ai.azure.com";
	if (strcmp(s, "chatgpt") == 0 || strcmp(s, "codex") == 0)
		return "https://chatgpt.com/backend-api/codex";
	return NULL;
}

static void hkApplyProviderAlias(const char *val) {
	enum aiProviderType t = hkParseProvider(val);
	if (t == AI_PROVIDER_NONE) return;
	E.ai_provider_type = t;
	const char *ep = hkProviderDefaultEndpoint(val);
	free(E.ai_endpoint);
	E.ai_endpoint = ep ? strdup(ep) : NULL;
}

static const struct {
	const char *label, *model, *family;
} HK_FAMILY[] = {
	{ "anthropic",     "claude-haiku-4-5", "claude"   },
	{ "mithraeum",     "hako-sho",                  "hako-"    },
	{ "openai",        "gpt-4o-mini",               "gpt"      },
	{ "gemini",        "gemini-2.5-flash",          "gemini"   },
	{ "groq",          "llama-3.3-70b-versatile",   "llama"    },
	{ "deepseek",      "deepseek-chat",             "deepseek" },
	{ "mistral",       "mistral-large-latest",      "mistral"  },
	{ "cerebras",      "llama3.1-8b",               "llama"    },
	{ "xai",           "grok-2-latest",             "grok"     },
	{ "github-models", "gpt-4o-mini",               NULL       },
	{ "copilot",       "gpt-4o-mini",               NULL       },
	{ "chatgpt",       "gpt-5.5",                   "gpt"      },
	{ "openrouter",    NULL,                        NULL       },
	{ "nous",          "anthropic/claude-haiku-5.5", NULL       },
	{ "together",      NULL,                        NULL       },
	{ "fireworks",     NULL,                        NULL       },
};

static int hkFamilyIndex(const char *label) {
	if (!label) return -1;
	for (size_t i = 0; i < sizeof(HK_FAMILY) / sizeof(HK_FAMILY[0]); i++)
		if (!strcmp(label, HK_FAMILY[i].label)) return (int)i;
	return -1;
}

static const char *hkProviderKey(enum aiProviderType t) {
	if (t == E.ai_provider_type) return hkProviderLabel();
	return hkProviderName(t);
}

static const char *hkProviderDefaultModel(enum aiProviderType t) {
	int i = hkFamilyIndex(hkProviderKey(t));
	return i >= 0 ? HK_FAMILY[i].model : NULL;
}

static int hkModelFitsProvider(enum aiProviderType t, const char *model) {
	if (!model || !*model) return 0;
	int i = hkFamilyIndex(hkProviderKey(t));
	if (i < 0 || !HK_FAMILY[i].family) return 1;
	return strstr(model, HK_FAMILY[i].family) != NULL;
}

static const char *clProviderConsoleUrl(const char *name) {
	if (!name) return NULL;
	if (!strcmp(name, "anthropic") || !strcmp(name, "claude")) return "https://console.anthropic.com/settings/keys";
	if (!strcmp(name, "openai") || !strcmp(name, "gpt"))      return "https://platform.openai.com/api-keys";
	if (!strcmp(name, "groq"))       return "https://console.groq.com/keys";
	if (!strcmp(name, "deepseek"))   return "https://platform.deepseek.com/api_keys";
	if (!strcmp(name, "mistral"))    return "https://console.mistral.ai/api-keys";
	if (!strcmp(name, "together"))   return "https://api.together.ai/settings/api-keys";
	if (!strcmp(name, "fireworks"))  return "https://fireworks.ai/account/api-keys";
	if (!strcmp(name, "openrouter")) return "https://openrouter.ai/keys";
	if (!strcmp(name, "xai") || !strcmp(name, "grok")) return "https://console.x.ai/";
	if (!strcmp(name, "gemini") || !strcmp(name, "google")) return "https://aistudio.google.com/apikey";
	if (!strcmp(name, "cerebras")) return "https://cloud.cerebras.ai/?tab=api-keys";
	if (!strcmp(name, "nous"))     return "https://portal.nousresearch.com";
	if (!strcmp(name, "ollamacloud") || !strcmp(name, "ocloud") || !strcmp(name, "ollama-cloud"))
		return "https://ollama.com/settings/keys";
	return NULL;
}

/* The clipboard where it is a file or a command away: iSh's /dev/clipboard, macOS pbcopy.
   A link or code copied for you beats selecting it out of a wrapped, indented line. */
static int hkClipCopy(const char *s) {
#if defined(HAKO_WASM) || defined(_WIN32)
	(void)s;
	return 0;
#else
	int ok = 0;
	FILE *f = fopen("/dev/clipboard", "w");
	if (f) { ok = fputs(s, f) >= 0; return (fclose(f) == 0) && ok; }
#ifdef __APPLE__
	if ((f = popen("pbcopy", "w")) != NULL) { ok = fputs(s, f) >= 0; ok = (pclose(f) == 0) && ok; }
#endif
	return ok;
#endif
}

static int hkClipAvailable(void) {
#if defined(HAKO_WASM) || defined(_WIN32)
	return 0;
#elif defined(__APPLE__)
	return 1;
#else
	return access("/dev/clipboard", R_OK) == 0;
#endif
}

/* Codes and keys never hold whitespace: drop all of it, which also undoes a paste
   that picked up a wrapped line's indent. */
static void hkSqueezeSpace(char *s) {
	char *w = s;
	for (; *s; s++) if (!isspace((unsigned char)*s)) *w++ = *s;
	*w = '\0';
}

/* One word of 20+ chars, not a link: what a code or key looks like. */
static int hkLooksLikeToken(const char *a, size_t n) {
	if (n < 20) return 0;
	for (size_t i = 0; i < n; i++) {
		if (isspace((unsigned char)a[i])) return 0;
		if (i + 2 < n && a[i] == ':' && a[i + 1] == '/' && a[i + 2] == '/') return 0;
	}
	return 1;
}

/* Enter on an empty prompt: the clipboard, if what is there looks like a token,
   so a stray clipboard never becomes an API key. */
static void hkClipToken(char *buf, size_t cap) {
	buf[0] = '\0';
#if !defined(HAKO_WASM) && !defined(_WIN32)
	int piped = 0;
	FILE *f = fopen("/dev/clipboard", "r");
#ifdef __APPLE__
	if (!f) { f = popen("pbpaste", "r"); piped = 1; }
#endif
	if (!f) return;
	size_t n = fread(buf, 1, cap - 1, f);
	buf[n] = '\0';
	if (piped) pclose(f); else fclose(f);
	char *a = buf, *e = buf + strlen(buf);
	while (*a && isspace((unsigned char)*a)) a++;
	while (e > a && isspace((unsigned char)e[-1])) e--;
	if (!hkLooksLikeToken(a, (size_t)(e - a))) { buf[0] = '\0'; return; }
	memmove(buf, a, (size_t)(e - a));
	buf[e - a] = '\0';
#else
	(void)cap; (void)hkLooksLikeToken;
#endif
}

/* A browser here can show a page: not over ssh, and on Linux/BSD only with a
   display (xdg-open with none can start a text browser in this terminal). */
#ifndef HAKO_WASM
static int hkHasDesktop(void) {
	if (getenv("SSH_CONNECTION") || getenv("SSH_TTY")) return 0;
#if defined(__APPLE__) || defined(_WIN32)
	return 1;
#else
	return getenv("DISPLAY") || getenv("WAYLAND_DISPLAY");
#endif
}
#endif

static void clOpenUrl(const char *url) {
#ifdef HAKO_WASM
	(void)url;      /* the front end opens links; there is no launcher to call */
	return;
#else
	if (!url || !hkHasDesktop()) return;
	char cmd[2048];
	/* A truncated URL is a DIFFERENT url, and this one goes to a shell.
	   Refuse rather than open something the user did not ask for. */
#ifdef __APPLE__
	if (!hkFmtFits(cmd, sizeof(cmd), "open '%s' >/dev/null 2>&1 &", url)) return;
#elif defined(_WIN32)
	if (!hkFmtFits(cmd, sizeof(cmd), "start \"\" \"%s\"", url)) return;
#else
	if (!hkFmtFits(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", url)) return;
#endif
	int rc = system(cmd);
	(void)rc;
#endif
}

static void clReadHidden(char *buf, size_t cap) {
	buf[0] = '\0';
#ifdef HAKO_WASM
	hkServeInput("paste the value", 1, buf, cap);
	return;
#else
	if (E.serve_mode) { hkServeInput("paste the code", 1, buf, cap); hkSqueezeSpace(buf); return; }
#ifndef _WIN32
	struct termios old, neu;
	int isterm = (tcgetattr(STDIN_FILENO, &old) == 0);
	if (isterm) {
		neu = old;
		neu.c_lflag &= ~ECHO;
		tcsetattr(STDIN_FILENO, TCSANOW, &neu);
	}
	if (!fgets(buf, cap, stdin)) buf[0] = '\0';
	if (isterm) tcsetattr(STDIN_FILENO, TCSANOW, &old);
	printf("\n");
#else
	if (!fgets(buf, cap, stdin)) buf[0] = '\0';
#endif
	char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';
#endif
	hkSqueezeSpace(buf);
	if (!buf[0] && hkClipAvailable()) hkClipToken(buf, cap);
}

static void clEnvApplyOpenAIKey(const char *envname) {
	const char *k = getenv(envname);
	if (!k || !*k) return;
	if (E.ai_provider_type == AI_PROVIDER_NONE) E.ai_provider_type = AI_PROVIDER_OPENAI;
	if (E.ai_provider_type == AI_PROVIDER_OPENAI) {
		free(E.ai_api_key); E.ai_api_key = strdup(k);
	}
}

static void clApplyEnv(void) {
	const char *k;
	if ((k = getenv("ANTHROPIC_API_KEY")) && *k) {
		if (E.ai_provider_type == AI_PROVIDER_NONE) E.ai_provider_type = AI_PROVIDER_ANTHROPIC;
		if (E.ai_provider_type == AI_PROVIDER_ANTHROPIC) {
			free(E.ai_api_key); E.ai_api_key = strdup(k);
		}
	}
	if ((k = getenv("OPENAI_API_KEY")) && *k) {
		if (E.ai_provider_type == AI_PROVIDER_NONE) E.ai_provider_type = AI_PROVIDER_OPENAI;
		if (E.ai_provider_type == AI_PROVIDER_OPENAI) {
			free(E.ai_api_key); E.ai_api_key = strdup(k);
		}
	}
	clEnvApplyOpenAIKey("GOOGLE_API_KEY");
	clEnvApplyOpenAIKey("GEMINI_API_KEY");
	clEnvApplyOpenAIKey("GROQ_API_KEY");
	clEnvApplyOpenAIKey("CEREBRAS_API_KEY");
	clEnvApplyOpenAIKey("DEEPSEEK_API_KEY");
	clEnvApplyOpenAIKey("MISTRAL_API_KEY");
	clEnvApplyOpenAIKey("TOGETHER_API_KEY");
	clEnvApplyOpenAIKey("FIREWORKS_API_KEY");
	clEnvApplyOpenAIKey("OPENROUTER_API_KEY");
	clEnvApplyOpenAIKey("XAI_API_KEY");

	if ((k = getenv("OLLAMA_API_KEY")) && *k && E.ai_provider_type == AI_PROVIDER_OLLAMA) {
		free(E.ai_api_key); E.ai_api_key = strdup(k);
	}

	if ((k = getenv("CLAW_API_KEY")) && *k && !getenv("HAKO_API_KEY")) {
		fprintf(stderr, "! CLAW_API_KEY is deprecated since v0.1.6 — use HAKO_API_KEY\n");
		free(E.ai_api_key); E.ai_api_key = strdup(k);
	}
	if ((k = getenv("HAKO_API_KEY")) && *k) { free(E.ai_api_key); E.ai_api_key = strdup(k); }
	if ((k = getenv("HAKO_PROVIDER")) && *k) hkApplyProviderAlias(k);
	if ((k = getenv("HAKO_MODEL")) && *k) { free(E.ai_model); E.ai_model = strdup(k); }
	if ((k = getenv("HAKO_ENDPOINT")) && *k) { free(E.ai_endpoint); E.ai_endpoint = strdup(k); }
}


#define CL_CREDS_MAGIC "CLAWCREDv1"
#define CL_PROV_MAX 32

typedef struct clCred {
	char provider[32];
	char *api_key;
	char *oauth_refresh;
	long  oauth_expires_at;
} clCred;

static clCred cl_creds[CL_PROV_MAX];
static int cl_creds_n = 0;
static int cl_creds_loaded = 0;

static void clCredsKey(unsigned char out[32]) {
	char host[256] = "host";
	gethostname(host, sizeof(host)-1); host[sizeof(host)-1] = '\0';
	unsigned long uid = (unsigned long)
#if defined(_WIN32) || defined(HAKO_WASM)
		0xC0FFEEUL;
#else
		getuid();
#endif
	const char *salt = "hako-code/credentials/v1";
	unsigned long long h = 0xcbf29ce484222325ULL;
	for (int i = 0; i < 32; i++) out[i] = 0;
	int oi = 0;
	const char *parts[3] = {host, salt, NULL};
	char uidbuf[32]; snprintf(uidbuf, sizeof(uidbuf), "%lu", uid);
	parts[2] = uidbuf;
	for (int round = 0; round < 4; round++) {
		for (int p = 0; p < 3; p++) {
			const char *s = parts[p];
			for (int i = 0; s[i]; i++) {
				h ^= (unsigned char)s[i];
				h *= 0x100000001b3ULL;
				out[oi % 32] ^= (unsigned char)(h >> ((oi % 8) * 8));
				oi++;
			}
		}
	}
}

static void clXorBuf(unsigned char *buf, size_t len, const unsigned char key[32]) {
	for (size_t i = 0; i < len; i++) buf[i] ^= key[i % 32];
}

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *clB64Encode(const unsigned char *in, size_t inlen) {
	size_t outlen = ((inlen + 2) / 3) * 4;
	char *out = malloc(outlen + 1);
	if (!out) return NULL;
	size_t i = 0, o = 0;
	while (i + 3 <= inlen) {
		unsigned v = (in[i] << 16) | (in[i+1] << 8) | in[i+2];
		out[o++] = b64tab[(v >> 18) & 63];
		out[o++] = b64tab[(v >> 12) & 63];
		out[o++] = b64tab[(v >> 6) & 63];
		out[o++] = b64tab[v & 63];
		i += 3;
	}
	if (i < inlen) {
		unsigned v = in[i] << 16;
		if (i + 1 < inlen) v |= in[i+1] << 8;
		out[o++] = b64tab[(v >> 18) & 63];
		out[o++] = b64tab[(v >> 12) & 63];
		out[o++] = (i + 1 < inlen) ? b64tab[(v >> 6) & 63] : '=';
		out[o++] = '=';
	}
	out[o] = '\0';
	return out;
}

static unsigned char *clB64Decode(const char *in, size_t *outlen) {
	static signed char rev[256];
	static int rev_init = 0;
	if (!rev_init) {
		for (int i = 0; i < 256; i++) rev[i] = -1;
		for (int i = 0; i < 64; i++) rev[(int)b64tab[i]] = (signed char)i;
		rev_init = 1;
	}
	size_t inlen = strlen(in);
	unsigned char *out = malloc(inlen);
	if (!out) return NULL;
	size_t o = 0;
	unsigned v = 0;
	int bits = 0;
	for (size_t i = 0; i < inlen; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
		int d = rev[c];
		if (d < 0) { free(out); return NULL; }
		v = (v << 6) | d;
		bits += 6;
		if (bits >= 8) { bits -= 8; out[o++] = (v >> bits) & 0xff; }
	}
	*outlen = o;
	return out;
}

/* ---- images: "[image #N]" in a message is <session>-N.<ext> in the project's images dir ---- */
#define HK_IMAGE_MAX  (3750L * 1024)      /* base64 of it stays under the 5 MB providers take */
#define HK_IMAGE_KEEP 6                   /* the newest this many ride along with a request */
static const char *const hk_image_ext[]  = { "png", "jpg", "gif", "webp" };
static const char *const hk_image_mime[] = { "image/png", "image/jpeg", "image/gif", "image/webp" };
static pthread_mutex_t hk_image_lock = PTHREAD_MUTEX_INITIALIZER;

/* Which of hk_image_ext the bytes are, by their magic, or -1. */
static int hkImageKind(const unsigned char *b, size_t n) {
	if (n >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) return 0;
	if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) return 1;
	if (n >= 6 && (!memcmp(b, "GIF87a", 6) || !memcmp(b, "GIF89a", 6))) return 2;
	if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) return 3;
	return -1;
}

static int hkImageDir(char *out, size_t n, int make) {
	char dir[PATH_MAX];
	if (!hkProjectStateDir(dir, sizeof dir) || !hkFmtFits(out, n, "%s/images", dir)) return 0;
	return !make || hk_fs_mkdirp(out) == 0;
}

/* Image n of this session: its path (into path) and kind, or -1. */
static int hkImageFind(int n, char *path, size_t cap) {
	char dir[PATH_MAX];
	if (n <= 0 || !E.session_id || !hkImageDir(dir, sizeof dir, 0)) return -1;
	for (int k = 0; k < 4; k++) {
		int isdir = 0;
		if (hkFmtFits(path, cap, "%s/%s-%d.%s", dir, E.session_id, n, hk_image_ext[k])
		    && hk_fs_exists(path, NULL, NULL, &isdir) && !isdir) return k;
	}
	return -1;
}

#if !defined(_WIN32) && !defined(HAKO_WASM)
/* argv with no shell between, stdout to out_path when given. Its exit status, or -1. */
static int hkRunArgv(char *const argv[], const char *out_path) {
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		int out = out_path ? open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600) : nul;
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
		if (out >= 0) dup2(out, 1);
		execvp(argv[0], argv);
		_exit(127);
	}
	int st = 0;
	while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
#endif

/* Over the cap: macOS shrinks it to a 1568-pixel jpeg, the size providers scale to anyway. */
static int hkImageShrink(const char *path, const char *dir, int n) {
#if defined(__APPLE__) && !defined(HAKO_WASM)
	char jpg[PATH_MAX + 64];
	long sz = 0;
	if (!hkFmtFits(jpg, sizeof jpg, "%s/%s-%d.jpg", dir, E.session_id, n)) return 0;
	char *const av[] = { "sips", "-Z", "1568", "-s", "format", "jpeg", (char *)path, "--out", jpg, NULL };
	int ok = hkRunArgv(av, NULL) == 0 && hk_fs_exists(jpg, &sz, NULL, NULL) && sz > 0 && sz <= HK_IMAGE_MAX;
	if (strcmp(jpg, path)) hk_fs_remove(ok ? path : jpg);
	return ok;
#else
	(void)path; (void)dir; (void)n;
	return 0;
#endif
}

/* Bytes in, the next "[image #N]" of this session out (into mark). 0 with why in err. */
static int hkImageStore(const char *data, size_t len, char *mark, size_t mcap, char *err, size_t ecap) {
	char dir[PATH_MAX], path[PATH_MAX + 64], probe[PATH_MAX + 64];
	int k = hkImageKind((const unsigned char *)data, len);
	if (k < 0) { snprintf(err, ecap, "not an image hako can send (png, jpeg, gif or webp)"); return 0; }
	if (!E.session_id || !hkImageDir(dir, sizeof dir, 1)) { snprintf(err, ecap, "no place to keep the image"); return 0; }
	pthread_mutex_lock(&hk_image_lock);
	int n = 1;
	while (n < 100000 && hkImageFind(n, probe, sizeof probe) >= 0) n++;
	int ok = hkFmtFits(path, sizeof path, "%s/%s-%d.%s", dir, E.session_id, n, hk_image_ext[k])
	      && hk_fs_write(path, data, len, 0) == 0;
	pthread_mutex_unlock(&hk_image_lock);
	if (!ok) { snprintf(err, ecap, "could not save the image"); return 0; }
	if ((long)len > HK_IMAGE_MAX && !hkImageShrink(path, dir, n)) {
		hk_fs_remove(path);
		snprintf(err, ecap, "the image is %.1f MB, over the %.1f MB providers take: shrink it first",
		         len / 1048576.0, HK_IMAGE_MAX / 1048576.0);
		return 0;
	}
	snprintf(mark, mcap, "[image #%d]", n);
	return n;
}

/* ctrl+v: the clipboard's image, stored. 0 with why in err. */
static int hkClipImage(char *mark, size_t mcap, char *err, size_t ecap) {
#if defined(_WIN32) || defined(HAKO_WASM)
	(void)mark; (void)mcap;
	snprintf(err, ecap, "no clipboard images here: type or drop the image's path instead");
	return 0;
#else
	char dir[PATH_MAX], tmp[PATH_MAX + 32];
	if (!hkImageDir(dir, sizeof dir, 1) || !hkFmtFits(tmp, sizeof tmp, "%s/.clip-%d", dir, (int)getpid())) {
		snprintf(err, ecap, "no place to keep the image");
		return 0;
	}
	int rc = -1;
#ifdef __APPLE__
	char w[PATH_MAX + 96];
	if (!strchr(tmp, '"') && hkFmtFits(w, sizeof w, "set f to open for access (POSIX file \"%s\") with write permission", tmp)) {
		char *const av[] = { "osascript", "-e", w, "-e", "try", "-e", "write (the clipboard as «class PNGf») to f",
		                     "-e", "end try", "-e", "close access f", NULL };
		rc = hkRunArgv(av, NULL);
	}
#else
	if (getenv("WAYLAND_DISPLAY")) { char *const av[] = { "wl-paste", "--no-newline", "--type", "image/png", NULL }; rc = hkRunArgv(av, tmp); }
	if (rc != 0 && getenv("DISPLAY")) { char *const av[] = { "xclip", "-selection", "clipboard", "-t", "image/png", "-o", NULL }; rc = hkRunArgv(av, tmp); }
#endif
	long n = 0;
	char *data = rc == 0 ? hk_fs_read(tmp, HK_IMAGE_MAX * 8, &n) : NULL;
	hk_fs_remove(tmp);
	int ok = 0;
	if (data && n > 0) ok = hkImageStore(data, (size_t)n, mark, mcap, err, ecap);
	else {
#ifdef __APPLE__
		snprintf(err, ecap, "no image on the clipboard");
#else
		snprintf(err, ecap, "no image on the clipboard (read with wl-paste or xclip)");
#endif
	}
	free(data);
	return ok;
#endif
}

/* Image paths in a typed line become "[image #N]": absolute, ~/ or @relative, a
   dragged file's escaped spaces or quotes included. NULL when nothing changed. */
static char *hkImagesFromLine(aiData *data, const char *line) {
	char *out = NULL;
	size_t o = 0, cap = 0;
	const char *p = line, *copied = line;
	while (*p) {
		const char *s = p;
		char q = 0;
		if (p != line && p[-1] != ' ' && p[-1] != '\t') { p++; continue; }
		if (*s == '\'' || *s == '"') q = *s++;
		int at = *s == '@';
		if (!(*s == '/' || (s[0] == '~' && s[1] == '/') || (at && s[1] && s[1] != ' '))) { p++; continue; }
		if (at) s++;
		char path[PATH_MAX];
		size_t n = 0;
		if (*s == '~') {
			const char *h = getenv("HOME");
			n = (size_t)snprintf(path, sizeof path, "%s", h ? h : "");
			if (n >= sizeof path) { p++; continue; }
			s++;
		}
		const char *e = s;
		while (*e && n + 1 < sizeof path && (q ? *e != q : (*e != ' ' && *e != '\t'))) {
			if (!q && *e == '\\' && e[1]) e++;
			path[n++] = *e++;
		}
		if (q && *e == q) e++;
		while (!q && n > 0 && strchr(".,;:!?)", path[n - 1])) { n--; e--; }   /* "look at /x/a.png." */
		path[n] = '\0';
		const char *dot = strrchr(path, '.');
		int ext = 0, isdir = 1;
		if (dot) for (const char *x = "png\0jpg\0jpeg\0gif\0webp\0"; *x && !ext; x += strlen(x) + 1) ext = !strcasecmp(dot + 1, x);
		if (!ext || !hk_fs_exists(path, NULL, NULL, &isdir) || isdir) { p = e > p ? e : p + 1; continue; }
		long len = 0;
		char *bytes = hk_fs_read(path, HK_IMAGE_MAX * 8, &len);
		char mark[32], err[160];
		const char *base = strrchr(path, '/');
		if (!bytes || len <= 0 || !hkImageStore(bytes, (size_t)len, mark, sizeof mark, err, sizeof err)) {
			if (!bytes || len <= 0) snprintf(err, sizeof err, "could not read it");
			aiSayf(data, "%s not attached: %s", base ? base + 1 : path, err);
			free(bytes);
			p = e;
			continue;
		}
		free(bytes);
		hkSbAdd(&out, &o, &cap, copied, (size_t)(p - copied));
		hkSbAdd(&out, &o, &cap, mark, strlen(mark));
		copied = p = e;
	}
	if (!out) return NULL;
	hkSbAdd(&out, &o, &cap, copied, strlen(copied));
	return out;
}

/* How the provider takes an image: 0 not at all (the local engine), 1 Anthropic,
   2 OpenAI chat (and every compatible host), 3 OpenAI responses, 4 ollama. */
static int hkImageWire(void) {
	switch (E.ai_provider_type) {
	case AI_PROVIDER_ANTHROPIC: return 1;
	case AI_PROVIDER_OPENAI:    return hkOAuthIs("chatgpt") ? 3 : 2;
	case AI_PROVIDER_OLLAMA:    return 4;
	default:                    return 0;
	}
}

static int hkImageRefs(const char *s) {
	int n = 0;
	while ((s = strstr(s, "[image #"))) { n++; s += 8; }
	return n;
}

/* A user message with each [image #N] it names riding along as that wire's
   image block, or NULL when none of them is on disk. */
static char *hkUserImageMsg(const char *content, int wire) {
	char *imgs = NULL, *out = NULL;
	size_t il = 0, icap = 0, ol = 0, ocap = 0;
	int seen[16], ns = 0;
	for (const char *p = content; (p = strstr(p, "[image #")) && ns < 16; ) {
		char *end;
		long n = strtol(p + 8, &end, 10);
		p += 8;
		int dup = 0;
		for (int i = 0; i < ns; i++) dup |= seen[i] == n;
		char path[PATH_MAX + 64];
		int k = *end == ']' && n > 0 && !dup ? hkImageFind((int)n, path, sizeof path) : -1;
		long len = 0;
		char *bytes = k >= 0 ? hk_fs_read(path, HK_IMAGE_MAX, &len) : NULL;
		char *b64 = bytes && len > 0 ? clB64Encode((const unsigned char *)bytes, (size_t)len) : NULL;
		free(bytes);
		if (!b64) continue;
		seen[ns++] = (int)n;
		char head[160];
		const char *tail = wire == 4 ? "\"" : wire == 1 ? "\"}}" : wire == 2 ? "\"}}" : "\"}";
		if (wire == 1)      snprintf(head, sizeof head, ",{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"%s\",\"data\":\"", hk_image_mime[k]);
		else if (wire == 2) snprintf(head, sizeof head, ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:%s;base64,", hk_image_mime[k]);
		else if (wire == 3) snprintf(head, sizeof head, ",{\"type\":\"input_image\",\"image_url\":\"data:%s;base64,", hk_image_mime[k]);
		else                snprintf(head, sizeof head, ",\"");
		hkSbAdd(&imgs, &il, &icap, head, strlen(head));
		hkSbAdd(&imgs, &il, &icap, b64, strlen(b64));
		hkSbAdd(&imgs, &il, &icap, tail, strlen(tail));
		free(b64);
	}
	if (!imgs) return NULL;
	size_t clen = strlen(content);
	char *esc = malloc(clen * 6 + 8);
	if (!esc) { free(imgs); return NULL; }
	hkJsonEscapeInto(content, esc, clen * 6 + 8);
	const char *a = wire == 4 ? "{\"role\":\"user\",\"content\":\""
	              : wire == 3 ? "{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\""
	              :             "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"";
	hkSbAdd(&out, &ol, &ocap, a, strlen(a));
	hkSbAdd(&out, &ol, &ocap, esc, strlen(esc));
	if (wire == 4) {
		hkSbAdd(&out, &ol, &ocap, "\",\"images\":[", 12);
		hkSbAdd(&out, &ol, &ocap, imgs + 1, il - 1);
		hkSbAdd(&out, &ol, &ocap, "]}", 2);
	} else {
		hkSbAdd(&out, &ol, &ocap, "\"}", 2);
		hkSbAdd(&out, &ol, &ocap, imgs, il);
		hkSbAdd(&out, &ol, &ocap, "]}", 2);
	}
	free(esc);
	free(imgs);
	return out;
}

#if !defined(_WIN32) && !defined(HAKO_WASM)
/* ---- relay link (RELAY.md §7): this device on the relay ----
   A key-possession sign-in, then two curl processes, a stream down and a
   chunked upload up. A frame is "<device id> <base64>"; what is inside is
   Noise, sealed end to end, so the relay only routes. */
typedef struct {
	char url[256], token[65], id[33], acct[17];   /* acct: "" until the page binds this device */
	uint8_t s[32], s_pub[32];
	pid_t down_pid, up_pid;
	FILE *down, *up;
	char *line;                 /* getline buffer for down */
	size_t line_cap;
	pthread_mutex_t up_lock;
	time_t up_kept;             /* the proxy in front of the relay drops an upload idle for a minute */
} hkRelay;

static void hkHexEnc(char *out, const uint8_t *b, size_t n) {
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
	out[2 * n] = '\0';
}

static int hkHexDec(uint8_t *out, const char *h, size_t n) {
	for (size_t i = 0; i < n; i++) {
		unsigned v;
		if (!isxdigit((unsigned char)h[2 * i]) || !isxdigit((unsigned char)h[2 * i + 1])
		    || sscanf(h + 2 * i, "%2x", &v) != 1) return -1;
		out[i] = (uint8_t)v;
	}
	return 0;
}

static void hkRelayDeviceId(char out[33], const uint8_t pub[32]) {
	uint8_t h[64];
	crypto_blake2b(h, 64, pub, 32);
	hkHexEnc(out, h, 16);
}

/* curl with argv (no shell), its stdout (writing=0) or stdin (writing=1) on
   the returned stream. */
static FILE *hkRelayCurl(char *const argv[], int writing, pid_t *pid) {
	int p[2];
	if (pipe(p) != 0) return NULL;
	*pid = fork();
	if (*pid < 0) { close(p[0]); close(p[1]); return NULL; }
	if (*pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		dup2(writing ? p[0] : nul, 0);
		dup2(writing ? nul : p[1], 1);
		dup2(nul, 2);
		close(p[0]); close(p[1]);
		execvp("curl", argv);
		_exit(127);
	}
	close(writing ? p[0] : p[1]);
	return fdopen(writing ? p[1] : p[0], writing ? "w" : "r");
}

static char *hkRelayPost(const char *url, const char *body) {
	pid_t pid;
	char *argv[] = { "curl", "-s", "--max-time", "15", "-X", "POST", "--data-binary", (char *)body,
	                 "-H", "Content-Type: text/plain", (char *)url, NULL };
	FILE *fp = hkRelayCurl(argv, 0, &pid);
	if (!fp) return NULL;
	char *out = hk_slurp(fp, 0);
	fclose(fp);
	waitpid(pid, NULL, 0);
	return out;
}

/* Prove to the relay that this device holds s: the MAC is keyed by
   X25519(s, the relay's one-off key). Returns 0 and fills token and id. */
static int hkRelaySignIn(hkRelay *r) {
	char url[300], pubh[66], body[128];
	crypto_x25519_public_key(r->s_pub, r->s);
	hkHexEnc(pubh, r->s_pub, 32);
	snprintf(url, sizeof url, "%s/v1/auth/challenge", r->url);
	char *a = hkRelayPost(url, pubh);
	uint8_t cid[16], epub[32], k[32], mac[64];
	int ok = a && strlen(a) >= 97 && a[32] == ' ' && !hkHexDec(cid, a, 16) && !hkHexDec(epub, a + 33, 32);
	char cidh[33];
	if (ok) memcpy(cidh, a, 32), cidh[32] = '\0';
	free(a);
	if (!ok) return -1;
	uint8_t acc = 0;
	crypto_x25519(k, r->s, epub);
	for (int i = 0; i < 32; i++) acc |= k[i];
	if (!acc) return -1;
	crypto_blake2b_ctx c;
	crypto_blake2b_init(&c, 64);
	crypto_blake2b_update(&c, k, 32);
	crypto_blake2b_update(&c, cid, 16);
	crypto_blake2b_update(&c, r->s_pub, 32);
	crypto_blake2b_final(&c, mac);
	crypto_wipe(k, sizeof k);
	char mach[65];
	hkHexEnc(mach, mac, 32);
	snprintf(body, sizeof body, "%s %s", cidh, mach);
	snprintf(url, sizeof url, "%s/v1/auth/prove", r->url);
	char *t = hkRelayPost(url, body);
	char want[33];
	hkRelayDeviceId(want, r->s_pub);
	ok = t && strlen(t) >= 97 && t[64] == ' ' && !strncmp(t + 65, want, 32);
	if (ok) {
		memcpy(r->token, t, 64); r->token[64] = '\0'; memcpy(r->id, want, 33);
		r->acct[0] = '\0';
		if (t[97] == ' ' && strlen(t + 98) >= 16 && t[98] != '-') snprintf(r->acct, sizeof r->acct, "%.16s", t + 98);
	}
	free(t);
	return ok ? 0 : -1;
}

/* POST a route with this device's token. The reply body, or NULL. */
static char *hkRelayCall(hkRelay *r, const char *route, const char *body) {
	char url[320], auth[96];
	pid_t pid;
	snprintf(url, sizeof url, "%s%s", r->url, route);
	snprintf(auth, sizeof auth, "Authorization: Bearer %s", r->token);
	char *argv[] = { "curl", "-s", "--max-time", "10", "-X", "POST", "--data-binary", (char *)body,
	                 "-H", auth, "-H", "Content-Type: text/plain", url, NULL };
	FILE *fp = hkRelayCurl(argv, 0, &pid);
	if (!fp) return NULL;
	char *out = hk_slurp(fp, 0);
	fclose(fp);
	waitpid(pid, NULL, 0);
	return out;
}

static int hkRelayOpen(hkRelay *r) {
	char auth[96], down[300], up[300];
	snprintf(auth, sizeof auth, "Authorization: Bearer %s", r->token);
	snprintf(down, sizeof down, "%s/v1/down", r->url);
	snprintf(up, sizeof up, "%s/v1/up", r->url);
	char *dv[] = { "curl", "-sN", "-H", auth, down, NULL };
	char *uv[] = { "curl", "-sN", "-X", "POST", "-T", "-", "-H", auth,
	               "-H", "Content-Type: text/plain", up, NULL };
	pthread_mutex_init(&r->up_lock, NULL);
	r->down = hkRelayCurl(dv, 0, &r->down_pid);
	r->up = hkRelayCurl(uv, 1, &r->up_pid);
	return r->down && r->up ? 0 : -1;
}

/* Wakes a thread blocked in hkRelayRecv (its stream ends); it then closes. */
static void hkRelayStop(hkRelay *r) {
	if (r->down_pid > 0) kill(r->down_pid, SIGTERM);
}

static int hkRelaySend(hkRelay *r, const char to[33], const uint8_t *b, size_t n) {
	if (n == 0 || n > 256 * 1024) return -1;                         /* the relay's frame limit */
	char *enc = clB64Encode(b, n);
	if (!enc) return -1;
	pthread_mutex_lock(&r->up_lock);
	int ok = r->up && fprintf(r->up, "%s %s\n", to, enc) > 0 && fflush(r->up) == 0;
	pthread_mutex_unlock(&r->up_lock);
	free(enc);
	if (!ok) hkRelayStop(r);                                         /* ends the download too, so the link reconnects */
	return ok ? 0 : -1;
}

/* The next frame. Returns its length, -2 when the relay says a device is
   offline (its id in from), -1 when the stream ends. */
static long hkRelayRecv(hkRelay *r, char from[33], uint8_t *out, size_t cap) {
	for (;;) {
		ssize_t n = r->down ? getline(&r->line, &r->line_cap, r->down) : -1;
		if (n < 0) return -1;
		while (n > 0 && (r->line[n - 1] == '\n' || r->line[n - 1] == '\r')) r->line[--n] = '\0';
		if (n == 0) {                                                /* keepalive: answer it now and then */
			if (r->up_pid > 0 && waitpid(r->up_pid, NULL, WNOHANG) == r->up_pid) {
				r->up_pid = 0;                                       /* the upload died: replies would vanish, so start over */
				return -1;
			}
			time_t now = time(NULL);
			if (now - r->up_kept >= 20) {                           /* the relay sends one every 25 s */
				pthread_mutex_lock(&r->up_lock);
				if (r->up && fputc('\n', r->up) != EOF) fflush(r->up);
				pthread_mutex_unlock(&r->up_lock);
				r->up_kept = now;
			}
			continue;
		}
		if (!strncmp(r->line, "! offline ", 10) && n == 42) { memcpy(from, r->line + 10, 33); return -2; }
		if (n < 34 || r->line[32] != ' ') continue;
		memcpy(from, r->line, 32);
		from[32] = '\0';
		size_t dl = 0;
		uint8_t *d = clB64Decode(r->line + 33, &dl);
		if (!d) continue;
		if (dl > cap) { free(d); continue; }
		memcpy(out, d, dl);
		free(d);
		return (long)dl;
	}
}

static void hkRelayClose(hkRelay *r) {
	if (r->up) { fclose(r->up); r->up = NULL; }
	if (r->up_pid > 0) { kill(r->up_pid, SIGTERM); waitpid(r->up_pid, NULL, 0); r->up_pid = 0; }
	if (r->down_pid > 0) { kill(r->down_pid, SIGTERM); waitpid(r->down_pid, NULL, 0); r->down_pid = 0; }
	if (r->down) { fclose(r->down); r->down = NULL; }
	free(r->line);
	r->line = NULL;
	r->line_cap = 0;
	crypto_wipe(r->token, sizeof r->token);
}
#endif

/* The middle part of a JWT, decoded. Not verified: only read for claims the
   server checks anyway (expiry, account id). */
static char *hkJwtPayload(const char *jwt) {
	const char *a = jwt ? strchr(jwt, '.') : NULL;
	if (!a) return NULL;
	a++;
	const char *b = strchr(a, '.');
	size_t n = b ? (size_t)(b - a) : strlen(a);
	if (!n) return NULL;
	char *s = malloc(n + 1);
	if (!s) return NULL;
	for (size_t i = 0; i < n; i++) s[i] = a[i] == '-' ? '+' : a[i] == '_' ? '/' : a[i];
	s[n] = '\0';
	size_t pl = 0;
	unsigned char *p = clB64Decode(s, &pl);
	free(s);
	if (!p) return NULL;
	p[pl] = '\0';                   /* decoded is always shorter than the input */
	return (char *)p;
}

/* Headers for chatgpt.com/backend-api/codex. hako names itself (originator,
   User-Agent) instead of posing as the Codex CLI. */
static int hkChatGPTHeaders(char *hdr, size_t cap, const char *token, int stream) {
	char acct[96] = "";
	char *pl = hkJwtPayload(token);
	char *id = pl ? hkExtractJsonString(pl, "chatgpt_account_id") : NULL;
	if (id) {
		size_t j = 0;
		for (const char *c = id; *c && j + 1 < sizeof(acct); c++)
			if (isalnum((unsigned char)*c) || *c == '-' || *c == '_') acct[j++] = *c;
		acct[j] = '\0';
	}
	free(id); free(pl);
	const char *orig = getenv("HAKO_CHATGPT_ORIGINATOR");
	if (!orig || !*orig || strchr(orig, '\'')) orig = "hako";
	/* a cut header is a broken shell quote: refuse instead */
	return hkFmtFits(hdr, cap, "-H 'Authorization: Bearer %s' -H 'originator: %s' "
		"-H 'OpenAI-Beta: responses=experimental'%s%s%s%s",
		token ? token : "", orig,
		acct[0] ? " -H 'ChatGPT-Account-ID: " : "", acct, acct[0] ? "'" : "",
		stream ? " -H 'accept: text/event-stream'" : "");
}

static void clCredsPath(char *out, size_t cap) {
	char dir[512]; hkClawDirPath(dir, sizeof(dir));
	if (dir[0]) snprintf(out, cap, "%s/credentials", dir);
	else out[0] = '\0';
}

static clCred *clCredsFind(const char *provider) {
	if (!provider) return NULL;
	for (int i = 0; i < cl_creds_n; i++)
		if (strcmp(cl_creds[i].provider, provider) == 0) return &cl_creds[i];
	return NULL;
}

static clCred *clCredsUpsert(const char *provider) {
	clCred *c = clCredsFind(provider);
	if (c) return c;
	if (cl_creds_n >= CL_PROV_MAX) return NULL;
	c = &cl_creds[cl_creds_n++];
	memset(c, 0, sizeof(*c));
	snprintf(c->provider, sizeof(c->provider), "%s", provider);
	return c;
}

static void clCredsSave(void) {
	char path[640]; clCredsPath(path, sizeof(path));
	if (!path[0]) return;
	size_t cap = 4096, len = 0;
	char *plain = malloc(cap);
	if (!plain) return;
	for (int i = 0; i < cl_creds_n; i++) {
		clCred *c = &cl_creds[i];
		if (!c->api_key && !c->oauth_refresh) continue;
		size_t need = len + 256
			+ (c->api_key ? strlen(c->api_key) : 0)
			+ (c->oauth_refresh ? strlen(c->oauth_refresh) : 0);
		if (need >= cap) { while (cap < need) cap *= 2; plain = realloc(plain, cap); }
		len += snprintf(plain + len, cap - len, "[%s]\n", c->provider);
		if (c->api_key) len += snprintf(plain + len, cap - len, "api_key=%s\n", c->api_key);
		if (c->oauth_refresh) len += snprintf(plain + len, cap - len, "oauth_refresh=%s\n", c->oauth_refresh);
		if (c->oauth_expires_at) len += snprintf(plain + len, cap - len, "oauth_expires_at=%ld\n", c->oauth_expires_at);
		if (len + 1 < cap) plain[len++] = '\n';
	}
	unsigned char key[32]; clCredsKey(key);
	clXorBuf((unsigned char *)plain, len, key);
	char *b64 = clB64Encode((unsigned char *)plain, len);
	free(plain);
	if (!b64) return;
	/* magic line, then base64 wrapped at 72 columns */
	size_t blen = strlen(b64);
	size_t outcap = blen + blen / 72 + 128, outlen = 0;
	char *out = malloc(outcap);
	if (!out) { free(b64); return; }
	outlen += (size_t)snprintf(out, outcap, "%s\n", CL_CREDS_MAGIC);
	for (size_t i = 0; i < blen; i += 72) {
		size_t n = blen - i; if (n > 72) n = 72;
		memcpy(out + outlen, b64 + i, n); outlen += n;
		out[outlen++] = '\n';
	}
	hk_fs_write(path, out, outlen, 0);
	free(out);
	free(b64);
#ifndef _WIN32
	chmod(path, 0600);
#endif
}

static void clCredsLoad(void) {
	if (cl_creds_loaded) return;
	cl_creds_loaded = 1;
	char path[640]; clCredsPath(path, sizeof(path));
	if (!path[0]) return;
	char *credbuf = hk_fs_read(path, 1024 * 1024, NULL);
	if (!credbuf) return;
	char *credsave = credbuf;
	char header[64];
	char *hdr = hkNextLine(&credsave);
	if (!hdr) { free(credbuf); return; }
	snprintf(header, sizeof(header), "%s", hdr);
	if (strncmp(header, CL_CREDS_MAGIC, strlen(CL_CREDS_MAGIC)) != 0) { free(credbuf); return; }
	/* the rest of the file is base64, newlines are cosmetic */
	size_t len = 0;
	char *b64 = malloc(strlen(credsave) + 1);
	if (!b64) { free(credbuf); return; }
	for (const char *q = credsave; *q; q++)
		if (*q != '\n' && *q != '\r') b64[len++] = *q;
	b64[len] = '\0';
	free(credbuf);
	size_t plain_len = 0;
	unsigned char *plain = clB64Decode(b64, &plain_len);
	free(b64);
	if (!plain) return;
	unsigned char key[32]; clCredsKey(key);
	clXorBuf(plain, plain_len, key);
	char *cur = (char *)plain;
	char *endp = (char *)plain + plain_len;
	clCred *active = NULL;
	while (cur < endp) {
		char *eol = memchr(cur, '\n', endp - cur);
		if (!eol) eol = endp;
		*eol = '\0';
		char *line = cur;
		cur = eol + 1;
		if (!*line) continue;
		if (*line == '[') {
			char *rb = strchr(line, ']');
			if (rb) {
				*rb = '\0';
				active = clCredsUpsert(line + 1);
			}
			continue;
		}
		if (!active) continue;
		char *eq = strchr(line, '='); if (!eq) continue;
		*eq = '\0';
		char *val = eq + 1;
		if (!strcmp(line, "api_key"))            { free(active->api_key); active->api_key = strdup(val); }
		else if (!strcmp(line, "oauth_refresh")) { free(active->oauth_refresh); active->oauth_refresh = strdup(val); }
		else if (!strcmp(line, "oauth_expires_at")) active->oauth_expires_at = atol(val);
	}
	free(plain);
}

static void clCredsCaptureCurrent(void) {
	const char *name = hkProviderLabel();
	if (!name || !strcmp(name, "none")) return;
	if (E.ai_oauth_provider && strcmp(E.ai_oauth_provider, name) != 0) return;
	clCred *c = clCredsUpsert(name);
	if (!c) return;
	free(c->api_key); c->api_key = E.ai_api_key ? strdup(E.ai_api_key) : NULL;
	free(c->oauth_refresh); c->oauth_refresh = E.ai_oauth_refresh ? strdup(E.ai_oauth_refresh) : NULL;
	c->oauth_expires_at = E.ai_oauth_expires_at;
}

/* A credential arrives from a paste, a file or a token response, and any of them can carry a trailing newline. */
static void hkTrimSecret(char **s) {
	if (!*s) return;
	char *p = *s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if (p != *s) memmove(*s, p, strlen(p) + 1);
	size_t n = strlen(*s);
	while (n && ((*s)[n-1] == ' ' || (*s)[n-1] == '\t' ||
	             (*s)[n-1] == '\r' || (*s)[n-1] == '\n')) (*s)[--n] = '\0';
	for (char *q = *s; *q; q++) if (*q == '\r' || *q == '\n' || *q == '\'') *q = ' ';
}

static void clCredsRestoreFor(const char *provider) {
	if (!provider) return;
	clCred *c = clCredsFind(provider);
	free(E.ai_api_key); E.ai_api_key = NULL;
	free(E.ai_oauth_refresh); E.ai_oauth_refresh = NULL;
	E.ai_oauth_expires_at = 0;
	free(E.ai_oauth_provider); E.ai_oauth_provider = NULL;
	if (!c) return;
	if (c->api_key) E.ai_api_key = strdup(c->api_key);
	if (c->oauth_refresh) {
		E.ai_oauth_refresh = strdup(c->oauth_refresh);
		E.ai_oauth_provider = strdup(provider);
	}
	E.ai_oauth_expires_at = c->oauth_expires_at;
	hkTrimSecret(&E.ai_api_key);
	hkTrimSecret(&E.ai_oauth_refresh);
}

/* A pasted key replaces any sign-in: a ChatGPT one left set sent the key down the codex route (nous 404). */
static void hkUsePastedKey(const char *prov, const char *key) {
	free(E.ai_api_key); E.ai_api_key = strdup(key);
	free(E.ai_oauth_refresh); E.ai_oauth_refresh = NULL;
	free(E.ai_oauth_provider); E.ai_oauth_provider = NULL;
	E.ai_oauth_expires_at = 0;
	hkApplyProviderAlias(prov);
}

static void hkWriteSessionFile(const char *path, int include_secrets) {
	/* Built in memory, then handed to the seam in one write: a browser has no
	   FILE* to print into. */
	char buf[16384];
	size_t n = 0;
	#define SESS_PUT(...) (n = hkCatf(buf, n, sizeof(buf), __VA_ARGS__))
	SESS_PUT("ai_provider=%s\n", hkProviderName(E.ai_provider_type));
	if (E.ai_model) SESS_PUT("ai_model=%s\n", E.ai_model);
	if (include_secrets && E.ai_endpoint) SESS_PUT("ai_endpoint=%s\n", E.ai_endpoint);
	SESS_PUT("ai_tools_enabled=%d\n", E.ai_tools_enabled);
	SESS_PUT("ai_tool_gate=%d\n", E.ai_tool_gate);
	SESS_PUT("ai_toolmode=%d\n", E.ai_toolmode);
	SESS_PUT("ai_stream=%d\n", E.ai_stream);
	if (E.ai_active_skills && *E.ai_active_skills) SESS_PUT("active_skills=%s\n", E.ai_active_skills);
	SESS_PUT("ai_auto_approve=%d\n", E.ai_auto_approve);
	SESS_PUT("ai_plan_mode=%d\n", E.ai_plan_mode);
	if (E.session_name && *E.session_name) SESS_PUT("session_name=%s\n", E.session_name);
	if (E.session_color && *E.session_color) SESS_PUT("session_color=%s\n", E.session_color);
	if (E.session_id) SESS_PUT("session_id=%s\n", E.session_id);
	SESS_PUT("session_started=%ld\n", E.session_started);
	SESS_PUT("session_last_used=%ld\n", hk_time_unix());
	SESS_PUT("session_turn_count=%d\n", E.session_turn_count);
	/* last: the one line that may be clipped, never the keys above it */
	if (hk_todo && *hk_todo) {
		char enc[HK_TODO_MAX + 64];
		size_t ei = 0;
		for (const char *q = hk_todo; *q && ei + 3 < sizeof(enc); q++) {
			if (*q == '\n') { enc[ei++] = '\\'; enc[ei++] = 'n'; }
			else if (*q != '\r') enc[ei++] = *q;
		}
		enc[ei] = '\0';
		SESS_PUT("todo=%s\n", enc);
	}
	hk_fs_write(path, buf, n, 0);
	#undef SESS_PUT
#ifndef _WIN32
	chmod(path, 0600);
#endif
}

static void hkSaveSession(void) {
	char dir[512];
	hkClawDirPath(dir, sizeof(dir));
	if (dir[0]) {
		char path[640];
		snprintf(path, sizeof(path), "%s/state", dir);
		hkWriteSessionFile(path, 1);
	}
	char pdir[PATH_MAX];
	if (hkProjectStateDir(pdir, sizeof(pdir))) {
		char ppath[PATH_MAX + 8];
		snprintf(ppath, sizeof(ppath), "%s/state", pdir);
		hkWriteSessionFile(ppath, 1);
	}
}

static struct {
	unsigned provider : 1, model : 1, endpoint : 1, api_key : 1,
	         max_tokens : 1, tools : 1, stream : 1, auto_approve : 1;
} hk_rc_pin;

static void hkLoadSessionFile(const char *path, int allow_session_fields) {
	char *stbuf = hk_fs_read(path, 256 * 1024, NULL);
	if (!stbuf) return;
	char *stsave = stbuf, *line;
	while ((line = hkNextLine(&stsave)) != NULL) {
		char *eq = strchr(line, '='); if (!eq) continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;
		int rc_locked = !allow_session_fields;
		if (strcmp(key, "ai_provider") == 0) {
			if (rc_locked && hk_rc_pin.provider) continue;
			enum aiProviderType t = hkParseProvider(val);
			if (t != AI_PROVIDER_NONE) E.ai_provider_type = t;
		} else if (strcmp(key, "ai_model") == 0) {
			if (rc_locked && hk_rc_pin.model) continue;
			free(E.ai_model);
			E.ai_model = strdup(val);
		} else if (strcmp(key, "ai_endpoint") == 0) {
			if (rc_locked && hk_rc_pin.endpoint) continue;
			free(E.ai_endpoint);
			E.ai_endpoint = strdup(val);
		} else if (strcmp(key, "ai_api_key") == 0) {
			if (rc_locked && hk_rc_pin.api_key) continue;
			free(E.ai_api_key);
			E.ai_api_key = strdup(val);
			hkTrimSecret(&E.ai_api_key);
		} else if (strcmp(key, "ai_oauth_provider") == 0) {
			free(E.ai_oauth_provider);
			E.ai_oauth_provider = strdup(val);
		} else if (strcmp(key, "ai_oauth_refresh") == 0) {
			free(E.ai_oauth_refresh);
			E.ai_oauth_refresh = strdup(val);
		} else if (strcmp(key, "ai_oauth_expires_at") == 0) {
			E.ai_oauth_expires_at = atol(val);
		} else if (strcmp(key, "ai_tools_enabled") == 0) {
			if (rc_locked && hk_rc_pin.tools) continue;
			E.ai_tools_enabled = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "ai_tool_gate") == 0) {
			E.ai_tool_gate = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "ai_toolmode") == 0) {
			E.ai_toolmode = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "todo") == 0) {
			free(hk_todo);
			hk_todo = malloc(strlen(val) + 2);
			if (hk_todo) {
				size_t di = 0;
				for (const char *q = val; *q; q++) {
					if (q[0] == '\\' && q[1] == 'n') { hk_todo[di++] = '\n'; q++; }
					else hk_todo[di++] = *q;
				}
				hk_todo[di] = '\0';
			}
		} else if (strcmp(key, "active_skills") == 0) {
			free(E.ai_active_skills);
			E.ai_active_skills = strdup(val);
		} else if (strcmp(key, "ai_stream") == 0) {
			if (rc_locked && hk_rc_pin.stream) continue;
			E.ai_stream = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "ai_plan_mode") == 0) {
			E.ai_plan_mode = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "editor") == 0) {
			free(E.editor);
			E.editor = strdup(val);
		} else if (strcmp(key, "ai_auto_approve") == 0) {
			if (rc_locked && hk_rc_pin.auto_approve) continue;
			E.ai_auto_approve = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "context_window") == 0) {
			E.ai_context_window = atol(val);
		} else if (strcmp(key, "prompt_cache") == 0) {
			E.ai_prompt_cache = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "shell_timeout") == 0) {
			E.ai_shell_timeout = atoi(val);
		} else if (strcmp(key, "ai_max_tokens") == 0) {
			if (rc_locked && hk_rc_pin.max_tokens) continue;
			E.ai_max_tokens = atoi(val);
		} else if (strcmp(key, "session_color") == 0) {
			free(E.session_color);
			E.session_color = strdup(val);
		} else if (strcmp(key, "session_name") == 0) {
			free(E.session_name);
			E.session_name = strdup(val);
		} else if (allow_session_fields && strcmp(key, "session_id") == 0) {
			free(E.session_id);
			E.session_id = strdup(val);
		} else if (allow_session_fields && strcmp(key, "session_started") == 0) {
			E.session_started = atol(val);
		} else if (allow_session_fields && strcmp(key, "session_last_used") == 0) {
			E.session_last_used = atol(val);
		} else if (allow_session_fields && strcmp(key, "session_turn_count") == 0) {
			E.session_turn_count = atoi(val);
		}
	}
	free(stbuf);
}

static void hkGenSessionId(void) {
	free(E.session_id);
	E.session_id = malloc(17);
	long now = hk_time_unix();
	srand((unsigned)(now ^ getpid()));
	snprintf(E.session_id, 17, "%lx%04x", now & 0xffffffff, rand() & 0xffff);
}

static void hkLoadSession(void) {
	char dir[512];
	hkClawDirPath(dir, sizeof(dir));
	if (dir[0]) {
		char path[640];
		snprintf(path, sizeof(path), "%s/state", dir);
		hkLoadSessionFile(path, 0);
	}
	char pdir[PATH_MAX];
	int has_project = 0;
	char ppath[PATH_MAX + 8];
	if (hkProjectStateDir(pdir, sizeof(pdir))) {
		snprintf(ppath, sizeof(ppath), "%s/state", pdir);
		if (hk_fs_exists(ppath, NULL, NULL, NULL)) {
			has_project = 1;
			hkLoadSessionFile(ppath, 1);
		}
	}

	long now = hk_time_unix();
	int recent = E.session_last_used > 0 && (now - E.session_last_used) < 7 * 24 * 3600;
	if (has_project && E.session_id && recent) {
		E.session_resumed = 1;
	} else {
		hkSessionFresh(NULL);
	}
}

static void hkLogMessage(const char *role, const char *content) {
	char path[PATH_MAX];
	hkSessionLogPath(path, sizeof(path));
	if (!path[0]) return;
	int clen = content ? (int)strlen(content) : 0;
	int cap = clen * 6 + 32;
	char *esc = malloc(cap);
	if (!esc) return;
	hkJsonEscapeInto(content ? content : "", esc, cap);
	size_t lcap = (size_t)cap + 128;
	char *line = malloc(lcap);
	if (line) {
		int n = snprintf(line, lcap, "{\"ts\":%ld,\"role\":\"%s\",\"content\":\"%s\"}\n",
		                 hk_time_unix(), role, esc);
		hk_fs_write(path, line, (size_t)n, 1);      /* append */
		free(line);
	}
	free(esc);
}

/* The chat's name and color are properties of the chat: they go into its… */
static void hkLogSessionMeta(void) {
	char path[PATH_MAX];
	hkSessionLogPath(path, sizeof(path));
	if (!path[0]) return;
	char nm[256], co[64];
	hkJsonEscapeInto(E.session_name ? E.session_name : "", nm, (int)sizeof(nm));
	hkJsonEscapeInto(E.session_color ? E.session_color : "", co, (int)sizeof(co));
	char line[512];
	int n = snprintf(line, sizeof(line), "{\"ts\":%ld,\"role\":\"meta\",\"name\":\"%s\",\"color\":\"%s\"}\n",
	                 hk_time_unix(), nm, co);
	hk_fs_write(path, line, (size_t)n, 1);
}

/* Fresh chat: new id, no history, no name, no color. One place, so the
   three "new session" entry points cannot drift. */
static void hkSessionFresh(aiData *data) {
	if (data) aiClearHistory(data);
	E.session_started = hk_time_unix();
	E.session_last_used = 0;
	E.session_turn_count = 0;
	E.session_resumed = 0;
	free(E.session_name);  E.session_name = NULL;
	free(E.session_color); E.session_color = NULL;
	hkGenSessionId();
}

static void hkLoadHistoryTail(aiData *data, int max_msgs, int ctx_msgs) {
	char path[PATH_MAX];
	hkSessionLogPath(path, sizeof(path));
	if (!path[0]) return;
	char *histbuf = NULL;
	{
		long fsize = 0;
		const long TAIL = 256 * 1024;
		if (hk_fs_exists(path, &fsize, NULL, NULL) && fsize > TAIL) {
			FILE *fp = fopen(path, "rb");
			if (fp && fseek(fp, fsize - TAIL, SEEK_SET) == 0) {
				char *buf = malloc((size_t)TAIL + 1);
				if (buf) {
					size_t got = fread(buf, 1, (size_t)TAIL, fp);
					buf[got] = '\0';
					char *nl = strchr(buf, '\n');     /* drop the partial line */
					if (nl) memmove(buf, nl + 1, strlen(nl + 1) + 1);
					histbuf = buf;
				}
			}
			if (fp) fclose(fp);
		}
		if (!histbuf) histbuf = hk_fs_read(path, 8 * 1024 * 1024, NULL);
	}
	if (!histbuf) return;

	char **lines = NULL;
	int lcount = 0, lcap = 0;
	char *save = histbuf, *line;
	while ((line = hkNextLine(&save)) != NULL) {
		if (!*line) continue;
		if (lcount >= lcap) { lcap = lcap ? lcap * 2 : 64; lines = realloc(lines, sizeof(char*) * lcap); }
		lines[lcount++] = strdup(line);
	}
	free(histbuf);

	int kept = lcount;
	int *keep_idx = malloc(sizeof(int) * (lcount + 1));
	for (int i = 0; i < lcount; i++) keep_idx[i] = i;
	int start = kept > max_msgs ? kept - max_msgs : 0;
	/* Everything from here is shown; only the tail is sent to the model, and
	   that tail is counted in model rows: forty notices used to push every real
	   turn out of the window. */
	int ctx_start = start;
	if (ctx_msgs > 0) {
		int seen = 0;
		ctx_start = kept;
		for (int k = kept - 1; k >= 0 && seen < ctx_msgs; k--) {
			const char *l = lines[keep_idx[k]];
			if (strstr(l, "\"role\":\"user\"") || strstr(l, "\"role\":\"assistant\"") || strstr(l, "\"role\":\"result\""))
				seen++;
			ctx_start = k;
		}
		/* and it opens on a user turn, not on half of one */
		while (ctx_start < kept && seen >= ctx_msgs && !strstr(lines[keep_idx[ctx_start]], "\"role\":\"user\"")) ctx_start++;
	}
	int lo = ctx_start < start ? ctx_start : start;
	for (int i = 0; i < lcount; i++) {         /* every meta row, whole file: last wins */
		if (!strstr(lines[i], "\"role\":\"meta\"")) continue;
		char *v = hkExtractJsonString(lines[i], "name");
		free(E.session_name);  E.session_name  = (v && *v) ? v : (free(v), NULL);
		v = hkExtractJsonString(lines[i], "color");
		free(E.session_color); E.session_color = (v && *v) ? v : (free(v), NULL);
	}
	for (int k = lo; k < kept; k++) {
		int i = keep_idx[k];
		char *l = lines[i];
		if (strstr(l, "\"role\":\"meta\"")) continue;
		int show = k >= start;
		char *role = strstr(l, "\"role\":\"");
		char *content = strstr(l, "\"content\":\"");
		if (role && content) {
			role += 8;
			char *rend = strchr(role, '"');
			content += 11;
			char *cend = content;      /* forward scan: `\\"` ends the string, `\"` does not */
			while (*cend) {
				if (*cend == '\\') { if (!cend[1]) break; cend += 2; continue; }
				if (*cend == '"') break;
				cend++;
			}
			if (rend && cend > content) {
				char rtag = *role;
				char *text = hkJsonUnescape(content, cend - content);
				if (!text) continue;
				if (rtag == 't' || rtag == 'r') {
					char mark[520];
					if (rtag == 't') snprintf(mark, sizeof(mark), "%s%.300s", HK_CALL_MARK, text);
					else             snprintf(mark, sizeof(mark), "%s%d", HK_TOOL_MARK, show ? hkToolOutAdd(text) : -1);
					if (show && data->history_count < AI_HISTORY_MAX) {
						data->history[data->history_count] = strdup(mark);
						data->history_role[data->history_count] = HK_ROLE_SYSTEM;
						data->history_count++;
					}
					if (rtag == 'r' && k >= ctx_start) {
						char tname[80] = "";
						char *tn = strstr(l, "\"tool\":\"");
						if (tn) { tn += 8; snprintf(tname, sizeof(tname), "%.*s", (int)strcspn(tn, "\""), tn); }
						size_t need = strlen(text) + sizeof(tname) + 48;
						char *obs = malloc(need);
						if (obs) {
							snprintf(obs, need, "<observation tool=\"%s\">%s</observation>", tname, text);
							/* Providers that insist on alternation get it folded
							   into the user turn before it. */
							int last = data->message_count - 1;
							if (last >= 0 && !data->messages[last].raw
							    && !strcmp(data->messages[last].role, "user")) {
								size_t ol = strlen(data->messages[last].content);
								char *m = realloc(data->messages[last].content, ol + strlen(obs) + 2);
								if (m) { m[ol] = '\n'; strcpy(m + ol + 1, obs); data->messages[last].content = m; }
							} else aiPushMessage(data, "user", obs);
							free(obs);
						}
					}
					free(text);
					continue;
				}
				unsigned char r = (rtag == 'u') ? HK_ROLE_USER
					: (rtag == 'a') ? HK_ROLE_AI : HK_ROLE_SYSTEM;
				if (show && data->history_count < AI_HISTORY_MAX) {
					data->history[data->history_count] = strdup(text);
					data->history_role[data->history_count] = r;
					data->history_count++;
				}
				if (k >= ctx_start) {
					if (r == HK_ROLE_USER) aiPushMessage(data, "user", text);
					else if (r == HK_ROLE_AI) aiPushMessage(data, "assistant", text);
				}
				free(text);
			}
		}
	}
	free(keep_idx);
	for (int i = 0; i < lcount; i++) free(lines[i]);
	free(lines);
}

static void hkAppendSkillBlock(char **buf, size_t *total, const char *name, const char *root, const char *body, long body_len) {
	char header[512];
	int hlen;
	if (root && *root) {
		hlen = snprintf(header, sizeof(header),
			"\n<skill name=\"%s\" root=\"%s\">\n", name, root);
	} else {
		hlen = snprintf(header, sizeof(header), "\n<skill name=\"%s\">\n", name);
	}
	const char *tail = "\n</skill>\n";
	int tlen = (int)strlen(tail);
	size_t need = *total + (size_t)hlen + (size_t)body_len + (size_t)tlen + 1;
	if (need > (1u << 22)) return;
	char *nb = realloc(*buf, need);
	if (!nb) return;
	*buf = nb;
	memcpy(*buf + *total, header, hlen); *total += hlen;
	memcpy(*buf + *total, body, body_len); *total += body_len;
	memcpy(*buf + *total, tail, tlen); *total += tlen;
}

/* Below this, inlining the whole skill is cheaper than the round trip it would
   take to fetch it. */
/* An active skill is one the user has switched on with :use. */
static int hkSkillActive(const char *name) {
	if (!E.ai_active_skills || !*E.ai_active_skills || !name) return 0;
	size_t nl = strlen(name);
	for (const char *p = E.ai_active_skills; *p; ) {
		while (*p == ',' || *p == ' ') p++;
		const char *e = p;
		while (*e && *e != ',') e++;
		size_t seg = (size_t)(e - p);
		/* tolerate a trailing .md on either side */
		size_t cmp = seg;
		if (cmp > 3 && !strncmp(p + cmp - 3, ".md", 3)) cmp -= 3;
		size_t nn = nl;
		if (nn > 3 && !strcmp(name + nn - 3, ".md")) nn -= 3;
		if (cmp == nn && !strncasecmp(p, name, cmp)) return 1;
		p = *e ? e + 1 : e;
	}
	return 0;
}

#ifndef HK_SKILL_INLINE_MAX
#define HK_SKILL_INLINE_MAX 1600
#endif

/* Pull `description:` out of YAML frontmatter, including the folded `>` form
   these skills actually use. Returns 0 if there is no frontmatter. */
static int hkSkillDesc(const char *body, char *out, size_t cap) {
	out[0] = '\0';
	if (strncmp(body, "---", 3) != 0) return 0;
	const char *p = strchr(body, '\n');
	if (!p) return 0;
	p++;
	const char *end = strstr(p, "\n---");
	if (!end) return 0;

	const char *d = NULL;
	for (const char *q = p; q && q < end; ) {
		if (!strncmp(q, "description:", 12)) { d = q + 12; break; }
		const char *nl = strchr(q, '\n');
		q = nl ? nl + 1 : NULL;
	}
	if (!d) return 0;
	while (*d == ' ' || *d == '>' || *d == '|') d++;

	size_t o = 0;
	int space = 1;
	for (const char *q = d; q < end && o + 1 < cap; q++) {
		if (*q == '\n') {
			/* A line that is not indented ends the folded value. */
			const char *nx = q + 1;
			while (*nx == ' ') nx++;
			if (nx == q + 1) break;
			if (!space && o + 1 < cap) { out[o++] = ' '; space = 1; }
			q = nx - 1;
			continue;
		}
		if (*q == ' ' || *q == '\t') { if (!space) { out[o++] = ' '; space = 1; } continue; }
		out[o++] = *q;
		space = 0;
	}
	while (o && out[o-1] == ' ') o--;
	out[o] = '\0';
	return o > 0;
}

static char *hkSkillListMd(const char *root) {
	char *out = malloc(2048);
	size_t cap = 2048, len = 0;
	out[0] = '\0';

	typedef struct { char path[512]; int depth; } walkent;
	walkent stack[64];
	int sp = 0;
	/* A root that will not fit the walk entry: nothing to list, and `out` is
	   already allocated, so free it rather than leak on the way out. */
	if (!hkFmtFits(stack[sp].path, sizeof(stack[sp].path), "%s", root)) { free(out); return NULL; }
	stack[sp].depth = 0;
	sp++;

	while (sp > 0) {
		walkent cur = stack[--sp];
		DIR *d = opendir(cur.path);
		if (!d) continue;
		struct dirent *e;
		while ((e = readdir(d))) {
			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
			if (strcmp(e->d_name, ".git") == 0 || strcmp(e->d_name, "node_modules") == 0) continue;
			char full[1024];
			if (snprintf(full, sizeof(full), "%s/%s", cur.path, e->d_name) >= (int)sizeof(full))
				continue;                  /* deeper than we can name: skip it */
			struct stat st;
			if (stat(full, &st) != 0) continue;
			if (S_ISDIR(st.st_mode)) {
				if (cur.depth + 1 < 4 && sp < (int)(sizeof(stack)/sizeof(stack[0]))) {
					snprintf(stack[sp].path, sizeof(stack[sp].path), "%.*s",
					         (int)sizeof(stack[sp].path) - 1, full);
					stack[sp].depth = cur.depth + 1;
					sp++;
				}
				continue;
			}
			int nlen = (int)strlen(e->d_name);
			if (nlen < 4 || strcmp(e->d_name + nlen - 3, ".md") != 0) continue;
			const char *rel = full + strlen(root);
			while (*rel == '/') rel++;
			int rlen = (int)strlen(rel);
			if (len + rlen + 2 >= cap) { cap = (cap + rlen + 64) * 2; out = realloc(out, cap); }
			memcpy(out + len, rel, rlen); len += rlen;
			out[len++] = '\n';
			out[len] = '\0';
		}
		closedir(d);
	}
	return out;
}

/* Rebuilding the system prompt means walking four skill roots and two memory… */
static int  hk_prompt_dirty = 1;
static int  hk_prompt_skills = 0;

static int  hk_skill_scanned;           /* the /<skill> list, rescanned with the prompt */
static void hkPromptInvalidate(void) { hk_prompt_dirty = 1; hk_skill_scanned = 0; }

static int hkLoadSkills(aiData *data) {
	if (!hk_prompt_dirty && data && data->system_prompt) return hk_prompt_skills;
	hk_prompt_dirty = 0;
	return hkBuildPrompt(data);
}

/* Where skills live, nearest first: the project's, then yours. `.agents/skills`
   is the vendor-neutral place Codex, OpenCode, Cursor and Copilot read. */
#define HK_SKILL_ROOTS 6
static int hkSkillRoots(char roots[HK_SKILL_ROOTS][640]) {
	char dir[512];
	hkClawDirPath(dir, sizeof(dir));
	const char *home = getenv("HOME");
	int n = 0;
	snprintf(roots[n++], 640, ".hako/skills");
	snprintf(roots[n++], 640, ".claude/skills");
	snprintf(roots[n++], 640, ".agents/skills");
	if (dir[0]) snprintf(roots[n++], 640, "%.600s/skills", dir);
	if (home) snprintf(roots[n++], 640, "%.600s/.claude/skills", home);
	if (home) snprintf(roots[n++], 640, "%.600s/.agents/skills", home);
	return n;
}

/* A skill's name from its directory entry: "caveman.md" and "caveman/" are the same skill. */
static void hkSkillName(const char *entry, char *out, size_t cap) {
	size_t n = strlen(entry);
	if (n > 3 && !strcmp(entry + n - 3, ".md")) n -= 3;
	snprintf(out, cap, "%.*s", (int)(n < cap ? n : cap - 1), entry);
}

static int hkBuildPrompt(aiData *data) {
	if (!data) return 0;
	char roots[HK_SKILL_ROOTS][640];
	int nroots = hkSkillRoots(roots);

	char *buf = NULL;
	size_t total = 0;
	int loaded = 0;
	char seen[64][128];
	int nseen = 0;

	int local_only_active = (E.ai_provider_type == AI_PROVIDER_MITHRAEUM);
	for (int ri = 0; ri < nroots; ri++) {
	const char *skills = roots[ri];
	DIR *d = opendir(skills);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		/* One skill per name across all roots, the nearest root winning: a
		   skill installed for two agents loads once. */
		char sname[128];
		hkSkillName(e->d_name, sname, sizeof sname);
		{
			int dup = 0;
			for (int k = 0; k < nseen; k++) if (!strcasecmp(seen[k], sname)) { dup = 1; break; }
			if (dup) continue;
		}
		/* Local models pay for preamble in seconds, not fractions of a cent. */
		if (local_only_active && !hkSkillActive(e->d_name)) continue;
		if (e->d_name[0] == '.') continue;
		char path[1024];
		if (!hkFmtFits(path, sizeof(path), "%s/%s", skills, e->d_name)) continue;
		struct stat st;
		if (stat(path, &st) != 0) continue;

		if (S_ISDIR(st.st_mode)) {
			char dispatcher[1024] = {0};
			char cand[1024];
			if (snprintf(cand, sizeof(cand), "%s/SKILL.md", path) >= (int)sizeof(cand)) cand[0] = '\0';
			if (access(cand, R_OK) == 0) snprintf(dispatcher, sizeof(dispatcher), "%s", cand);
			else {
				if (snprintf(cand, sizeof(cand), "%s/%s.md", path, e->d_name) >= (int)sizeof(cand))
					continue;
				if (access(cand, R_OK) == 0) snprintf(dispatcher, sizeof(dispatcher), "%s", cand);
			}
			if (!dispatcher[0]) continue;

			char *body = hkReadFileAll(dispatcher, 200000);
			if (!body) continue;

			char *manifest = hkSkillListMd(path);
			size_t blen = strlen(body);
			size_t mlen = manifest ? strlen(manifest) : 0;
			char desc[768];
			int active = hkSkillActive(e->d_name);
			int summarised = !active && blen > HK_SKILL_INLINE_MAX && hkSkillDesc(body, desc, sizeof(desc));
			char *combined = malloc((summarised ? strlen(desc) + 512 : blen) + mlen + 512);
			int n = 0;
			if (summarised) {
				n += snprintf(combined + n, strlen(desc) + 512,
					"%s\n\nTo follow this skill, first read its instructions with "
					"read_skill(skill=\"%s\", path=\"SKILL.md\").\n", desc, e->d_name);
			} else {
				if (active) {
					const char *hdr = "ACTIVE MODE — follow these instructions in EVERY reply "
					                  "until the user turns this skill off.\n\n";
					size_t hl = strlen(hdr);
					memcpy(combined + n, hdr, hl); n += (int)hl;
				}
				memcpy(combined + n, body, blen); n += blen;
			}
			n += snprintf(combined + n, mlen + 256, "\n<files>\n%s</files>\n", manifest ? manifest : "");
			combined[n] = '\0';
			free(body);
			free(manifest);

			hkAppendSkillBlock(&buf, &total, e->d_name, path, combined, n);
			free(combined);
			if (nseen < 64) snprintf(seen[nseen++], sizeof(seen[0]), "%s", sname);
			loaded++;
			continue;
		}

		int nlen = (int)strlen(e->d_name);
		if (nlen < 4 || strcmp(e->d_name + nlen - 3, ".md") != 0) continue;
		long sz = 0;
		char *body = hk_fs_read(path, 200000, &sz);
		if (!body) continue;
		char fdesc[768];
		int factive = hkSkillActive(e->d_name);
		if (!factive && sz > HK_SKILL_INLINE_MAX && hkSkillDesc(body, fdesc, sizeof(fdesc))) {
			char summary[1024];
			int sn = snprintf(summary, sizeof(summary),
				"%s\n\nTo follow this skill, first read it with "
				"read_skill(skill=\"%s\", path=\"%s\").\n", fdesc, e->d_name, e->d_name);
			hkAppendSkillBlock(&buf, &total, e->d_name, NULL, summary, sn);
		} else if (factive) {
			char *withhdr = malloc((size_t)sz + 160);
			/* NOT the snprintf return: that is what it WOULD have written, so
			   editing this string past the buffer would memcpy off the end. */
			snprintf(withhdr, 160,
				"ACTIVE MODE — follow these instructions in EVERY reply until the user turns this skill off.\n\n");
			size_t wn = strlen(withhdr);
			memcpy(withhdr + wn, body, (size_t)sz);
			hkAppendSkillBlock(&buf, &total, e->d_name, NULL, withhdr, (int)wn + sz);
			free(withhdr);
		} else {
			hkAppendSkillBlock(&buf, &total, e->d_name, NULL, body, sz);
		}
		free(body);
		if (nseen < 64) snprintf(seen[nseen++], sizeof(seen[0]), "%s", sname);
		loaded++;
	}
	if (d) closedir(d);
	}
	if (buf) buf[total] = '\0';

	static const char *REACT_PROMPT =
		"You can use these tools. To call a tool, emit EXACTLY this in your response:\n"
		"\n"
		"<tool name=\"TOOL_NAME\">\n"
		"{\"arg1\": \"value1\", \"arg2\": \"value2\"}\n"
		"</tool>\n"
		"\n"
		"Stop right after the </tool> tag and wait. The system will reply with:\n"
		"<observation>...result...</observation>\n"
		"Then continue. You may chain multiple tools.\n"
		"\n"
		"Tools:\n"
		"  read_file(path: string)        Read a file. Path relative to project.\n"
		"  list_dir(path: string)         List directory entries. Use \".\" for project root.\n"
		"  write_file(path, content)      Create/overwrite a WHOLE file. Needs trust.\n"
		"  edit_file(path, old, new)      Replace an exact unique snippet in a file. Prefer for small fixes.\n"
		"  edit_lines(path, start, end, new)  Replace a 1-indexed line range. Use when you know line numbers.\n"
		"  run_shell(cmd: string)         Run non-interactive shell command. Needs trust. 10s.\n"
		"  read_skill(skill, path)        Read a file inside an installed skill.\n"
		"\n"
		"Tool names are EXACTLY as listed above. Do NOT invent names like `bash`, `create_file`, `list_files`, `view`, `Write`, `Read`, `LS`, `Bash` — those will be silently remapped but you should use the canonical names. `write_file` REQUIRES both `path` AND `content` params; never call it with path alone. To fix an existing file, prefer `edit_file`/`edit_lines` over rewriting it with `write_file`.\n"
		"\n"
		"ORDERING RULE — STRICT. Emit the <tool> block FIRST when a tool is needed. Do not narrate ('I will create...', 'Done!', 'Perfect!') BEFORE the tool call — those sentences print to the user before the tool runs, which reads as a lie. Correct shape: (1) <tool>...</tool>, (2) wait for <observation>, (3) THEN one short sentence about what actually happened.\n"
		"\n"
		"For the final answer to the user, respond in plain text WITHOUT any <tool> tags.\n"
		"\n";

	static const char *QWEN_TOOL_PROMPT =
		"# Tools\n"
		"\n"
		"You may call one or more functions to assist with the user query.\n"
		"\n"
		"You are provided with function signatures within <tools></tools> XML tags:\n"
		"<tools>\n"
		"{\"type\": \"function\", \"function\": {\"name\": \"read_file\", \"description\": \"Read a file from the project\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": \"string\", \"description\": \"path relative to the project root\"}}, \"required\": [\"path\"]}}}\n"
		"{\"type\": \"function\", \"function\": {\"name\": \"list_dir\", \"description\": \"List directory entries; use \\\".\\\" for the project root\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": \"string\"}}, \"required\": [\"path\"]}}}\n"
		"{\"type\": \"function\", \"function\": {\"name\": \"write_file\", \"description\": \"Create or overwrite a WHOLE file. For small changes to an existing file use edit_file instead\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": \"string\"}, \"content\": {\"type\": \"string\"}}, \"required\": [\"path\", \"content\"]}}}\n"
		"{\"type\": \"function\", \"function\": {\"name\": \"edit_file\", \"description\": \"Change PART of an existing file: replace the exact unique snippet 'old' with 'new'. Use this for fixes instead of rewriting the whole file\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": \"string\"}, \"old\": {\"type\": \"string\"}, \"new\": {\"type\": \"string\"}}, \"required\": [\"path\", \"old\", \"new\"]}}}\n"
		"{\"type\": \"function\", \"function\": {\"name\": \"run_shell\", \"description\": \"Run a non-interactive shell command in the project\", \"parameters\": {\"type\": \"object\", \"properties\": {\"cmd\": {\"type\": \"string\"}}, \"required\": [\"cmd\"]}}}\n"
		"{\"type\": \"function\", \"function\": {\"name\": \"grep\", \"description\": \"Search file contents across the project. Use this to FIND something instead of guessing a filename\", \"parameters\": {\"type\": \"object\", \"properties\": {\"pattern\": {\"type\": \"string\"}, \"glob\": {\"type\": \"string\", \"description\": \"optional file filter such as *.py\"}}, \"required\": [\"pattern\"]}}}\n"
		"{\"type\": \"function\", \"function\": {\"name\": \"glob\", \"description\": \"List project files matching a name pattern such as *.py\", \"parameters\": {\"type\": \"object\", \"properties\": {\"pattern\": {\"type\": \"string\"}}, \"required\": [\"pattern\"]}}}\n"
		"</tools>\n"
		"\n"
		"For each function call, return a json object with function name and arguments within <tool_call></tool_call> XML tags:\n"
		"<tool_call>\n"
		"{\"name\": <function-name>, \"arguments\": <args-json-object>}\n"
		"</tool_call>\n"
		"\n"
		"To CREATE OR OVERWRITE A FILE, do NOT put the body inside the json — escaping a whole file as a json string breaks. Instead emit the file body RAW between write_file tags:\n"
		"<write_file path=\"relative/path.ext\">\n"
		"the complete file content, exactly as it belongs on disk, with real newlines and no escaping\n"
		"</write_file>\n"
		"Use <write_file> for EVERY whole-file write. read_file, list_dir, run_shell, edit_file still use <tool_call>.\n"
		"\n"
		"To CHANGE an existing file (e.g. fix one line after an error), do NOT rewrite the whole file — call edit_file with the exact 'old' snippet and the 'new' replacement. Rewriting the whole file each time wastes space and loses your earlier work.\n"
		"\n"
		"To write a file, reply with ONLY the <write_file> block and put the content inside it exactly once. The block itself is how you show the code — no need to also paste it in a separate ``` block.\n"
		"\n"
		"# Tool-call SHAPE (structure only — always act on the user's real request, never these placeholders):\n"
		"# read a file or list a dir:\n"
		"<tool_call>\n"
		"{\"name\": \"list_dir\", \"arguments\": {\"path\": \".\"}}\n"
		"</tool_call>\n"
		"# create/overwrite a file — the body goes RAW between the tags, no JSON escaping:\n"
		"<write_file path=\"RELATIVE/PATH.ext\">\n"
		"...the complete file content...\n"
		"</write_file>\n"
		"After a tool runs, the system sends back <observation tool=\"...\">RESULT</observation>; only THEN reply, in one short line. Do NOT write 'user:' or 'assistant:' lines yourself, do NOT invent a filename from the examples, and NEVER say you created or ran a file unless you actually emitted its tool call this turn and saw its observation.\n"
		"\n"
		"When the user asks you to create, read, list, run, or change something in their project, CALL the matching function — do NOT describe manual steps instead. Emit the tool call FIRST, then stop and wait for the result before saying anything else.\n"
		"\n"
		"To CREATE a program or file (a game, a script, anything new), use write_file with the actual code as its body. That is the only way to make a new file — do not invent any other tool for it.\n"
		"\n";

	static const char *BASE_PROMPT =
		"You are hako-code, a terminal AI agent.\n"
		"\n"
		"RULE 1: Call a tool whenever the user wants you to read, list, write, or run something in the project — INCLUDING polite forms like \"can you create…\", \"could you read…\", \"would you make…\", \"please write…\". Those are requests to DO it, not questions. ALSO an action: any mention of a file by name, or asking why something isn't working / to debug / check / look at / find / search it — READ, LIST or SEARCH it first, don't ask the user about it. To FIND something (where a function is defined or called, a string, TODOs) call grep; to list files of a kind (*.c, *.md, the tests) call glob — list_dir only shows one folder's names. Asking you to work on / fix / improve / set up / style / showcase / build out \"the website / site / project / app / code / this\" is ALSO an action: your FIRST move is list_dir(\".\") to see what's there. The words \"here\", \"this folder\", \"this directory\", \"this project\", \"our website\" ALWAYS mean the current directory — call list_dir(\".\"); NEVER ask the user its name, where it is, or to list it for you. Only greetings and GENERAL concept questions with no project file involved (\"what is recursion?\", \"how does a hashmap work?\") get a plain-text reply with no tool call.\n"
		"\n"
		"RULE 2: All paths are RELATIVE to the current project. Use \".\" for the project root. NEVER use absolute paths like /home/..., /Users/..., /root/..., /tmp/.... Those paths do NOT exist here and every call will fail.\n"
		"\n"
		"Examples — call tools:\n"
		"  user: \"read README.md\"               -> read_file(path=\"README.md\")\n"
		"  user: \"what files are here?\"         -> list_dir(path=\".\")\n"
		"  user: \"create test.txt with hi\"     -> write_file(path=\"test.txt\", content=\"hi\\n\")\n"
		"  user: \"can you create a pong game?\"  -> write_file(path=\"pong.py\", ...)\n"
		"  user: \"could you make a hello.py?\"   -> write_file(path=\"hello.py\", ...)\n"
		"  user: \"why isn't pong.py working?\"   -> read_file(path=\"pong.py\")\n"
		"  user: \"it's in this directory\"       -> list_dir(path=\".\")\n"
		"  user: \"search the directory / use ls\" -> list_dir(path=\".\")\n"
		"  user: \"fix our website here\"          -> list_dir(path=\".\")\n"
		"  user: \"you're able to see it yourself\" -> list_dir(path=\".\")\n"
		"\n"
		"Examples — do NOT call tools:\n"
		"  user: \"hello\"                     -> plain text reply\n"
		"  user: \"who are you?\"              -> plain text reply\n"
		"  user: \"explain recursion\"         -> plain text reply\n"
		"  user: \"what is 2+2?\"              -> plain text reply\n"
		"\n"
		"RULE 3: Be brief. No preamble (\"Sure, I'll…\", \"Let me…\") and no narrating steps. Emit the tool call, and after it runs confirm in ONE short line — or just go straight to the next tool. Do NOT re-paste file contents or explain your work unless the user asks.\n"
		"\n"
		"RULE 4: You are a real agent with WORKING tools on THIS machine. NEVER ask the user to paste, provide, or share a file's contents — you have read_file, so read it yourself. NEVER say \"as an AI language model\", \"I don't have access to your files\", \"I can't see or interact with files on your system\", or \"I can't search\" — every one of those is FALSE here: you CAN, by calling read_file / list_dir / run_shell. When the user says you CAN see it / you're able to look yourself, they are right — that is your cue to call list_dir(\".\") or read_file NOW, not to deny it. If you already asked for contents and the user pushes back, stop apologizing and CALL THE TOOL.\n"
		"\n"
		"If a tool returns \"error: path outside project\", do NOT retry with another absolute path. Either use \".\" or stop and reply in text.\n";
	static char env_probe[1024];
	static int env_probed = 0;
	if (!env_probed) {
		env_probed = 1;
		static const char *names[] = {
			"python3", "python", "node", "deno", "bun", "ruby", "perl",
			"go", "cargo", "rustc", "make", "gcc", "clang", "java",
			"git", "curl", "wget", "jq",
			NULL
		};
		/* Names only, found by walking PATH here: this used to spawn a shell per name
		   (18 of them, ~115 ms on a Mac and far more under iSh's emulator), and it put
		   absolute paths in the prompt — tokens the model never needs, contradicting
		   RULE 2, and the user's home directory in every request. */
		int off = snprintf(env_probe, sizeof(env_probe), "\n# ENVIRONMENT\n\nInstalled here:");
		int have_py3 = 0, have_py = 0, any = 0;
		for (int i = 0; names[i] && off < (int)sizeof(env_probe) - 80; i++) {
			if (!hkOnPath(names[i])) continue;
			off = (int)hkCatf(env_probe, (size_t)off, sizeof(env_probe), "%s %s", any ? "," : "", names[i]);
			any = 1;
			if (!strcmp(names[i], "python3")) have_py3 = 1;
			if (!strcmp(names[i], "python"))  have_py = 1;
		}
		if (!any) off = (int)hkCatf(env_probe, (size_t)off, sizeof(env_probe), " (none of the usual tools found)");
		off = (int)hkCatf(env_probe, (size_t)off, sizeof(env_probe), "\n");
		if (have_py3 && !have_py)
			off = (int)hkCatf(env_probe, (size_t)off, sizeof(env_probe),
				"There is no 'python' here: call python3.\n");
	}

	char *hako_body = NULL; size_t hako_len = 0;
	char hako_src[64] = "HAKO.md";
	{
		char cands[3][PATH_MAX + 16];
		int ncand = 0;
		char pdir[PATH_MAX];
		if (hkProjectDirPath(pdir, sizeof(pdir)))
			snprintf(cands[ncand++], sizeof(cands[0]), "%s/HAKO.md", pdir);
		snprintf(cands[ncand++], sizeof(cands[0]), "AGENTS.md");
		snprintf(cands[ncand++], sizeof(cands[0]), "CLAUDE.md");

		for (int ci = 0; ci < ncand && !hako_body; ci++) {
			struct stat hst;
			if (stat(cands[ci], &hst) != 0 || !S_ISREG(hst.st_mode)) continue;
			const long HAKO_CAP = 200000;
			int truncated = hst.st_size > HAKO_CAP;
			char *body = hkReadFileAll(cands[ci], HAKO_CAP);
			if (!body) continue;
			size_t blen2 = strlen(body);
			size_t scan = blen2 < 4096 ? blen2 : 4096;
			int binary = 0;
			if ((long)blen2 < hst.st_size && hst.st_size <= HAKO_CAP) binary = 1;
			for (size_t i = 0; i < scan && !binary; i++) {
				unsigned char c = (unsigned char)body[i];
				if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') binary = 1;
			}
			const char *base = strrchr(cands[ci], '/');
			base = base ? base + 1 : cands[ci];
			if (binary) {
				if (!E.pipe_mode) fprintf(stderr, "! %s skipped: binary or control-char content\n", base);
				free(body);
				continue;
			}
			if (truncated && !E.pipe_mode)
				fprintf(stderr, "! %s truncated at %ld bytes (file is %ld)\n", base, HAKO_CAP, (long)hst.st_size);
			hako_body = body;
			hako_len = blen2;
			hkFmtFits(hako_src, sizeof(hako_src), "%s", base);   /* name only, empty if it cannot fit */
		}
	}
	static const char *PLAN_PROMPT =
		"# Plan mode\n\n"
		"You may read, search and fetch, but you may NOT write, edit or run "
		"anything — those tools will refuse. Work out what you would do and "
		"present it: the files involved, the change to each, and anything you "
		"are unsure about. Do not pretend the work is done.\n\n";

	const char *plan_block = (E.ai_plan_mode && E.ai_tools_enabled && hkProjectTrusted())
	                       ? PLAN_PROMPT : NULL;
	size_t plan_len = plan_block ? strlen(plan_block) : 0;

	char *mem_block = NULL;
	{
		int mcount = 0;
		char *mem = hkMemCollect(&mcount);
		if (mem && mcount > 0) {
			size_t mn = strlen(mem);
			mem_block = malloc(mn + 260);
			if (mem_block)
				snprintf(mem_block, mn + 260,
					"\n# What you already know\n\nRemembered from earlier sessions:\n%s\n\n"
					"Call the remember tool when you learn something durable and new.\n", mem);
		}
		free(mem);
	}
	size_t mem_len = mem_block ? strlen(mem_block) : 0;

	char *todo_block = NULL;
	if (hk_todo && *hk_todo) {
		size_t tn = strlen(hk_todo);
		todo_block = malloc(tn + 200);
		if (todo_block)
			snprintf(todo_block, tn + 200,
				"\n# Current task list\n\n%s\nKeep this current: call the todo tool with the whole "
				"list whenever a step is finished or the plan changes.\n", hk_todo);
	}
	size_t todo_len = todo_block ? strlen(todo_block) : 0;

	char hako_hdr_buf[96];
	snprintf(hako_hdr_buf, sizeof(hako_hdr_buf), "\n# %s (project context)\n\n", hako_src);
	const char *hako_hdr = hako_hdr_buf;
	size_t hako_hlen = hako_body ? strlen(hako_hdr) : 0;

	static const char *UNTRUSTED_PROMPT =
		"# Tools\n\n"
		"You have NO tools in this session: the user has not granted file access "
		"for this directory yet. Do not claim you can read, write, edit or run "
		"anything, and do not emit tool-call syntax — nothing will execute.\n\n"
		"If the user asks you to do something that needs the filesystem, tell them "
		"in one short line to run `:trust` in the terminal where hako runs (a phone "
		"or browser cannot grant it), then wait.\n\n";

	const char *tool_prompt = NULL;
	int tools_available = E.ai_tools_enabled && hkProjectTrusted();
	if (!tools_available && E.ai_tools_enabled) {
		/* Tools are on but unusable here — the honest thing is to say so rather
		   than describe a toolbox the request does not carry. */
		tool_prompt = UNTRUSTED_PROMPT;
	} else if (E.ai_provider_type == AI_PROVIDER_MITHRAEUM)
		tool_prompt = QWEN_TOOL_PROMPT;
	else if (E.ai_toolmode == 1 || hkSubProse())
		tool_prompt = REACT_PROMPT;
	char *mcp_tools = (E.ai_provider_type == AI_PROVIDER_MITHRAEUM) ? hkMcpLocalToolsPrompt() : NULL;
	size_t mlen = mcp_tools ? strlen(mcp_tools) : 0;
	size_t blen = strlen(BASE_PROMPT);
	size_t rlen = tool_prompt ? strlen(tool_prompt) : 0;
	size_t elen = strlen(env_probe);
	size_t need = rlen + mlen + blen + elen + total + hako_hlen + hako_len + todo_len + mem_len + plan_len + 1;
	char *combined = malloc(need);
	if (combined) {
		size_t off = 0;
		if (rlen > 0) { memcpy(combined + off, tool_prompt, rlen); off += rlen; }
		if (mlen > 0) { memcpy(combined + off, mcp_tools, mlen); off += mlen; }
		memcpy(combined + off, BASE_PROMPT, blen); off += blen;
		if (elen > 0) { memcpy(combined + off, env_probe, elen); off += elen; }
		if (buf && total > 0) { memcpy(combined + off, buf, total); off += total; }
		if (hako_body) {
			memcpy(combined + off, hako_hdr, hako_hlen); off += hako_hlen;
			memcpy(combined + off, hako_body, hako_len); off += hako_len;
		}
		if (plan_block) { memcpy(combined + off, plan_block, plan_len); off += plan_len; }
		if (mem_block)  { memcpy(combined + off, mem_block, mem_len);  off += mem_len; }
		if (todo_block) { memcpy(combined + off, todo_block, todo_len); off += todo_len; }
		combined[off] = '\0';
		free(buf); free(hako_body); free(todo_block); free(mem_block);
		free(data->system_prompt);
		data->system_prompt = combined;
	} else {
		free(todo_block);
		free(mem_block);
		free(data->system_prompt);
		data->system_prompt = buf;
	}
	free(mcp_tools);
	hk_prompt_skills = loaded;
	return loaded;
}


static void clPipeEscape(const char *src, char *dst, int dsz) {
	int d = 0;
	for (const char *p = src; *p && d < dsz - 2; p++) {
		unsigned char c = (unsigned char)*p;
		if      (c == '"')  { if (d < dsz-3) { dst[d++]='\\'; dst[d++]='"';  } }
		else if (c == '\\') { if (d < dsz-3) { dst[d++]='\\'; dst[d++]='\\'; } }
		else if (c == '\n') { if (d < dsz-3) { dst[d++]='\\'; dst[d++]='n';  } }
		else if (c == '\r') { if (d < dsz-3) { dst[d++]='\\'; dst[d++]='r';  } }
		else if (c == '\t') { if (d < dsz-3) { dst[d++]='\\'; dst[d++]='t';  } }
		else                { dst[d++] = (char)c; }
	}
	dst[d] = '\0';
}

/* :remote on runs the --serve listener inside a terminal session, so the phone
   sees it and can type into it. Everything the pipe would say goes to its ring. */
#if !defined(_WIN32) && !defined(HAKO_WASM)
#define HK_ATTACH 1
#else
#define HK_ATTACH 0
#endif
static volatile int hs_answer_armed = 0;   /* a terminal approval the phone may also answer */
static volatile int hs_listen_stop = 0;    /* :remote off: the listener and its streams end */
static void hkServePush(const char *json);

#if HK_ATTACH
static int hs_wake[2] = { -1, -1 };        /* a phone answer closes the terminal popup */

/* Block for a key or a phone answer; 1 = the phone answered. */
static int hkWaitKeyOrWake(void) {
	fd_set fs;
	FD_ZERO(&fs);
	FD_SET(STDIN_FILENO, &fs);
	FD_SET(hs_wake[0], &fs);
	int mx = hs_wake[0] > STDIN_FILENO ? hs_wake[0] : STDIN_FILENO;
	if (select(mx + 1, &fs, NULL, NULL, NULL) <= 0 || !FD_ISSET(hs_wake[0], &fs)) return 0;
	char d[16];
	if (read(hs_wake[0], d, sizeof(d)) < 0) {}
	return 1;
}
#endif

/* The reply as it streams, for a remote device. Batched so a long answer does
   not flood the replay ring; the final message replaces it. */
static char hk_delta[1024];
static size_t hk_delta_n = 0;
static long hk_delta_t = 0;
static pthread_mutex_t hk_delta_lock = PTHREAD_MUTEX_INITIALIZER;
static void hkDeltaFlush(void) {
	char text[sizeof hk_delta];
	pthread_mutex_lock(&hk_delta_lock);
	memcpy(text, hk_delta, hk_delta_n);
	text[hk_delta_n] = '\0';
	int had = hk_delta_n > 0;
	hk_delta_n = 0;
	hk_delta_t = hk_time_ms();
	pthread_mutex_unlock(&hk_delta_lock);
	if (!had) return;
	char esc[sizeof hk_delta * 6 + 8], line[sizeof esc + 32];
	clPipeEscape(text, esc, sizeof esc);
	snprintf(line, sizeof line, "{\"type\":\"delta\",\"text\":\"%s\"}", esc);
	hkServePush(line);
}
static void hkDeltaPush(const char *t) {
	if (!(E.serve_mode || hs_attached) || !t) return;
	for (; *t; t++) {
		if (*t == '\r') continue;
		/* cut only on a character boundary: half a UTF-8 sequence is not text */
		if (hk_delta_n >= sizeof hk_delta - 8 && ((unsigned char)*t & 0xC0) != 0x80) hkDeltaFlush();
		pthread_mutex_lock(&hk_delta_lock);
		if (hk_delta_n < sizeof hk_delta - 1) hk_delta[hk_delta_n++] = *t;
		pthread_mutex_unlock(&hk_delta_lock);
	}
	if (hk_delta_n && (unsigned char)hk_delta[hk_delta_n - 1] < 0x80 && hk_time_ms() - hk_delta_t >= 120)
		hkDeltaFlush();
}

static void clPipeEmitRaw(const char *json) {
	if (E.serve_mode || hs_attached) {
		if (hk_delta_n) hkDeltaFlush();          /* streamed text lands before what follows it */
		hkServePush(json);
		return;
	}
	puts(json);
	fflush(stdout);
}

static int hk_turn_spoke = 0;

static void clPipeEmitMsg(const char *role, const char *text) {
	char esc[4096], line[4608];
	if (text) {
		const char *q = text;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		if (!*q) return;
	}
	if (role && (!strcmp(role, "ai") || !strcmp(role, "system"))) hk_turn_spoke = 1;
	clPipeEscape(text ? text : "", esc, sizeof(esc));
	snprintf(line, sizeof(line), "{\"type\":\"message\",\"role\":\"%.16s\",\"text\":\"%.4095s\"}", role, esc);
	clPipeEmitRaw(line);
}

static void clPipeEmitDisplay(const char *type, const char *display) {
	char esc[512], line[640];
	hk_turn_spoke = 1;
	clPipeEscape(display ? display : "", esc, sizeof(esc));
	snprintf(line, sizeof(line), "{\"type\":\"%.32s\",\"display\":\"%.511s\"}", type, esc);
	clPipeEmitRaw(line);
}

static void clPipeEmitDone(aiData *data) {
	char json[256];
	snprintf(json, sizeof(json),
		"{\"type\":\"done\",\"turn\":%d,\"in\":%d,\"out\":%d,\"session\":\"%s\"}",
		E.session_turn_count,
		data ? data->last_in_tokens : 0, data ? data->last_out_tokens : 0,
		E.session_id ? E.session_id : "");
	clPipeEmitRaw(json);
}

/* The --pipe wire version hake reads from init. 2 = tool_request/approve plus
   choice_request/choice. Without it the pane assumed 1 and said so. */
#define HAKO_PIPE_PROTOCOL 2

static void clPipeEmitInit(void) {
	char cwd[PATH_MAX], esc[PATH_MAX * 2 + 8];
	if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
	clPipeEscape(cwd, esc, sizeof(esc));
	char json[PATH_MAX * 2 + 512];
	snprintf(json, sizeof(json),
		"{\"type\":\"init\",\"session\":\"%s\",\"resumed\":%d,\"turns\":%d,"
		"\"provider\":\"%s\",\"model\":\"%s\",\"dir\":\"%s\",\"version\":\"%s\",\"protocol\":%d",
		E.session_id ? E.session_id : "",
		E.session_resumed ? 1 : 0,
		E.session_turn_count,
		hkProviderLabel(),
		E.ai_model ? E.ai_model : "",
		esc, HAKO_VERSION, HAKO_PIPE_PROTOCOL);
	/* + session_name, theme, voice, update and the command table (§2, §4) */
	size_t len = strlen(json), cap = len + 1;
	char *b = malloc(cap);
	if (!b) { clPipeEmitRaw("{\"type\":\"init\"}"); return; }
	memcpy(b, json, cap);
	hkStateExtras(&b, &len, &cap, HK_RC_CODE, 1);
	hkSbAdd(&b, &len, &cap, "}", 1);
	clPipeEmitRaw(b);
	free(b);
}

/* Provider or model moved. Front ends show it in their own status line; a menu
   pick says nothing in the chat, so this is the only word they get. */
static void clPipeEmitState(void) {
	if (!E.pipe_mode && !hs_attached) return;      /* nobody on the wire to tell */
	char pe[96], me[320], json[512];
	clPipeEscape(hkProviderLabel(), pe, sizeof pe);
	clPipeEscape(E.ai_model ? E.ai_model : "", me, sizeof me);
	snprintf(json, sizeof json, "{\"type\":\"state\",\"provider\":\"%s\",\"model\":\"%s\",\"remote\":%d",
	         pe, me, hs_remote ? 1 : 0);
	size_t len = strlen(json), cap = len + 1;
	char *b = malloc(cap);
	if (!b) return;
	memcpy(b, json, cap);
	/* a browser (serve, :remote) draws the studio's theme; Rei, the code one */
	hkStateExtras(&b, &len, &cap, (E.serve_mode || hs_attached) ? HK_RC_STUDIO : HK_RC_CODE, -1);
	hkSbAdd(&b, &len, &cap, "}", 1);
	clPipeEmitRaw(b);
	free(b);
}

/* A picker over --pipe (the editor's Rei pane, or --serve). hako cannot draw a
   tty popup there, so it emits the SAME choice_request the ask_user path uses
   and blocks for the answer; the front end renders and replies. Returns the
   chosen 0-based index, or -1 for cancel. */
static long g_pipe_choice_seq = 0;
static int hkEmitChoiceRequest(const char *title, const char **items, int n) {
	size_t cap = (title ? strlen(title) : 0) * 6 + 256;
	for (int i = 0; i < n; i++) cap += strlen(items[i]) * 6 + 8;
	char *req = malloc(cap);
	if (!req) return 0;
	char te[2048];
	clPipeEscape(title ? title : "select", te, sizeof(te));
	int off = snprintf(req, cap,
		"{\"type\":\"choice_request\",\"id\":%ld,\"question\":\"%s\",\"options\":[",
		++g_pipe_choice_seq, te);
	for (int i = 0; i < n && off > 0 && (size_t)off < cap; i++) {
		char oe[512];
		clPipeEscape(items[i], oe, sizeof(oe));
		int w = snprintf(req + off, cap - off, "%s\"%s\"", i ? "," : "", oe);
		if (w < 0 || (size_t)(off + w) >= cap) break;
		off += w;
	}
	if (off > 0 && (size_t)off < cap) snprintf(req + off, cap - off, "]}");
	else snprintf(req, cap, "{\"type\":\"choice_request\",\"id\":%ld,\"options\":[]}", g_pipe_choice_seq);
	clPipeEmitRaw(req);
	free(req);
	return 1;
}

/* A 1-based index or the item's own text; -1 when it names none. */
static int hkChoiceIndex(const char *ans, const char **items, int n) {
	if (!ans || !ans[0]) return -1;
	if (ans[0] >= '1' && ans[0] <= '9') {
		int k = atoi(ans) - 1;
		if (k >= 0 && k < n) return k;
	}
	for (int i = 0; i < n; i++) if (!strcasecmp(ans, items[i])) return i;
	return -1;
}

static int hkPipeChoice(const char *title, const char **items, int n) {
	if (n <= 0 || !hkEmitChoiceRequest(title, items, n)) return -1;

	char ans[128] = {0};
	if (E.serve_mode) {
		hkServeAnswer(ans, sizeof(ans));
	} else {
		char line[4096];
		while (fgets(line, sizeof(line), stdin)) {
			char *ty = hkExtractJsonString(line, "type");
			if (!ty) continue;
			int ok = !strcmp(ty, "choice") || !strcmp(ty, "answer");
			free(ty);
			if (!ok) continue;
			char *a = hkExtractJsonString(line, "answer");
			if (!a) a = hkExtractJsonString(line, "text");
			if (a) { snprintf(ans, sizeof(ans), "%.127s", a); free(a); }
			break;
		}
	}
	return hkChoiceIndex(ans, items, n);
}

static void clRenderMarkdownInline(const char *in, char *out, size_t cap) {
	size_t j = 0;
	const char *p = in;
	/* Heading level used to be computed and thrown away, so every heading came
	   out as undifferentiated bold. */
	if (p[0] == '#' && (p[1] == ' ' || (p[1] == '#' && (p[2] == ' ' || (p[2] == '#' && p[3] == ' '))))) {
		int level = 0;
		while (*p == '#') { level++; p++; }
		if (*p == ' ') p++;
		if (level <= 2) j = hkCatf(out, j, cap, "%s\x1b[1m", TH_ACCENT);
		else            j = hkCatf(out, j, cap, "\x1b[1m");
	}
	else if (p[0] == '>' && p[1] == ' ') {
		j = hkCatf(out, j, cap, "%s│ ", ANSI_DIM);
		p += 2;
	}
	/* A horizontal rule is structure, so it gets drawn as one. */
	else if ((p[0] == '-' && p[1] == '-' && p[2] == '-' && !p[3]) ||
	         (p[0] == '*' && p[1] == '*' && p[2] == '*' && !p[3]) ||
	         (p[0] == '_' && p[1] == '_' && p[2] == '_' && !p[3])) {
		j = hkCatf(out, j, cap, "%s────────%s", ANSI_DIM, ANSI_RESET);
		out[j] = '\0';
		return;
	}
	else if ((p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') {
		const char *b = p + 2;
		while (*b == ' ') b++;
		/* Task list items are the one markdown construct the agent also emits
		   itself (the todo tool), so they share its glyphs. */
		if (b[0] == '[' && b[1] && b[2] == ']' && (b[3] == ' ' || !b[3])) {
			int done = (b[1] == 'x' || b[1] == 'X');
			j = hkCatf(out, j, cap, "%s%s %s",
			              done ? TH_OK : TH_META,
			              done ? "\xE2\x9C\x93" : "\xE2\x97\x8B", ANSI_RESET);
			p = b[3] ? b + 4 : b + 3;
		} else {
			j = hkCatf(out, j, cap, "%s\xC2\xB7 %s", ANSI_TOOL, ANSI_RESET);
			p += 2;
		}
	}
	/* A markdown table separator row (|---|:--:|) is pure layout for a renderer
	   that aligns columns. This one does not, so it carries no information. */
	{
		const char *q = p;
		int seen_pipe = 0, only_rule = 1;
		while (*q) {
			if (*q == '|') seen_pipe = 1;
			else if (*q != '-' && *q != ':' && *q != ' ' && *q != '\t') { only_rule = 0; break; }
			q++;
		}
		if (seen_pipe && only_rule && q != p) {
			j = hkCatf(out, j, cap, "%s\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80%s", ANSI_DIM, ANSI_RESET);
			out[j] = '\0';
			return;
		}
	}
	/* Table rows: drop the outer pipes and let the inner ones read as dividers
	   rather than as punctuation in the middle of a sentence. */
	if (p[0] == '|' && strchr(p + 1, '|')) {
		p++;
		while (*p == ' ') p++;
		size_t rl = strlen(p);
		char row[1024];
		snprintf(row, sizeof(row), "%.*s", (int)(rl < sizeof(row) - 1 ? rl : sizeof(row) - 1), p);
		rl = strlen(row);
		while (rl && (row[rl-1] == ' ' || row[rl-1] == '|')) row[--rl] = '\0';
		char cell[1024];
		size_t ci = 0;
		for (size_t k = 0; k < rl && ci + 24 < sizeof(cell); k++) {
			if (row[k] == '|') {
				while (ci && cell[ci-1] == ' ') ci--;
				ci = hkCatf(cell, ci, sizeof(cell), "%s \xC2\xB7 %s", ANSI_DIM, ANSI_RESET);
				while (row[k+1] == ' ') k++;
			} else {
				cell[ci++] = row[k];
			}
		}
		cell[ci] = '\0';
		j = hkCatf(out, j, cap, "%s", cell);
		p += strlen(p);
	}
	/* Ordered items keep their number but the marker reads as a marker. */
	else if (isdigit((unsigned char)p[0])) {
		const char *d = p;
		while (isdigit((unsigned char)*d)) d++;
		if ((*d == '.' || *d == ')') && d[1] == ' ') {
			j = hkCatf(out, j, cap, "%s%.*s%s%s ",
			              ANSI_TOOL, (int)(d - p), p, (*d == '.' ? "." : ")"), ANSI_RESET);
			p = d + 2;
		}
	}
	while (*p && j + 16 < cap) {
		if (p[0] == '*' && p[1] == '*') {
			const char *end = strstr(p + 2, "**");
			if (end) {
				j = hkCatf(out, j, cap, "\x1b[1m%.*s\x1b[22m", (int)(end - p - 2), p + 2);
				p = end + 2; continue;
			}
		}
		if ((p[0] == '*' || p[0] == '_') && p[1] && p[1] != ' ' && p[1] != *p) {
			char delim = p[0];
			char prev = (p > in) ? p[-1] : '\0';
			int open_ok = !(prev && (isalnum((unsigned char)prev) || prev == delim));
			const char *end = open_ok ? strchr(p + 1, delim) : NULL;
			if (end && end > p + 1 && end[-1] != ' ') {
				char after = end[1];
				int close_ok = !(after && (isalnum((unsigned char)after) || after == delim));
				if (close_ok) {
					j = hkCatf(out, j, cap, "\x1b[3m%.*s\x1b[23m", (int)(end - p - 1), p + 1);
					p = end + 1; continue;
				}
			}
		}
		if (p[0] == '[') {
			const char *tclose = strchr(p, ']');
			if (tclose && tclose[1] == '(') {
				const char *uclose = strchr(tclose + 2, ')');
				if (uclose && uclose > tclose + 2) {
					j = hkCatf(out, j, cap, "%.*s%s (%.*s)%s",
					              (int)(tclose - p - 1), p + 1, ANSI_DIM,
					              (int)(uclose - tclose - 2), tclose + 2, ANSI_RESET);
					p = uclose + 1;
					continue;
				}
			}
		}
		if (p[0] == '`') {
			const char *end = strchr(p + 1, '`');
			if (end) {
				j = hkCatf(out, j, cap, "%s%.*s%s", ANSI_TOOL, (int)(end - p - 1), p + 1, ANSI_RESET);
				p = end + 1; continue;
			}
		}
		out[j++] = *p++;
	}
	out[j] = '\0';
}

static int cl_in_codefence = 0;

/* Wrap to the terminal, keeping the gutter. */
static void clPutWrapped2(const char *color, const char *prefix, const char *cont,
                          const char *body) {
	int cols = clTermCols();
	int gut  = clCellWidth(prefix);
	int avail = cols - gut - 1;
	if (avail < 8) {            /* too narrow to be worth wrapping */
		if (E.color_enabled) printf("%s%s%s%s\n", color, prefix, body, ANSI_RESET);
		else                 printf("%s%s\n", prefix, body);
		return;
	}

	const unsigned char *p = (const unsigned char *)body;
	int first = 1;
	while (*p) {
		/* Measure forward to the wrap point, remembering the last space. */
		const unsigned char *scan = p, *lastsp = NULL;
		int cells = 0;
		while (*scan) {
			if (*scan == 0x1b && scan[1] == '[') {
				scan += 2;
				while (*scan && *scan != 'm') scan++;
				if (*scan) scan++;
				continue;
			}
			unsigned int cp;
			int adv = clUtf8Next(scan, &cp);
			int w = clRuneWidth(cp);
			if (cells + w > avail) break;
			if (cp == ' ' || cp == '\t') lastsp = scan;
			cells += w;
			scan += adv;
		}
		const unsigned char *end = *scan ? (lastsp ? lastsp : scan) : scan;
		if (end == p) end = scan;            /* one glyph wider than the window */

		int n = (int)(end - p);
		if (E.color_enabled)
			printf("%s%s%.*s%s\n", color, first ? prefix : cont, n, p, ANSI_RESET);
		else
			printf("%s%.*s\n", first ? prefix : cont, n, p);
		first = 0;

		p = end;
		while (*p == ' ' || *p == '\t') p++;   /* eat the break whitespace */
	}
	if (first) {                              /* body was empty */
		if (E.color_enabled) printf("%s%s%s\n", color, prefix, ANSI_RESET);
		else                 printf("%s\n", prefix);
	}
}

static void clPutWrapped(const char *color, const char *prefix, const char *body) {
	clPutWrapped2(color, prefix, "   ", body);
}

/* ---------- the rail ---------- */
#define HK_RAIL_KEYSTONE "\xE2\x97\x86 "   /* ◆  turn opens */
#define HK_RAIL_BODY     "\xE2\x94\x82 "   /* │  prose continues */
#define HK_RAIL_TEE      "\xE2\x94\x9C "   /* ├  a tool hangs off it */

static int clRailOn(void) {
	return E.color_enabled && !E.pipe_mode && !E.output_json && clTermCols() >= 28;
}

/* Streamed deltas arrive mid-word, but the renderer works on lines. */
typedef struct { char buf[8192]; size_t len; int shown; int open; int parked; } clStreamLine;

static void clPrintRoleLine(unsigned char role, const char *text);
static void clOutBegin(void);
static void clOutEnd(aiData *data);

/* Take the bar down and put the cursor back at the end of the partial line. */
static void clStreamResume(clStreamLine *sl) {
	if (sl->open) return;
	clOutBegin();                         /* bar down: cursor at col 0 of the row after the partial */
	sl->open = 1;
	if (sl->parked && sl->shown > 0) {
		int cols = clTermCols(); if (cols < 2) cols = 80;
		int col = sl->shown % cols;
		if (col > 0) printf("\x1b[A\x1b[%dC", col);
	}
	sl->parked = 0;
}

/* Break the partial line and put the bar back under it. */
static void clStreamPark(clStreamLine *sl) {
	if (!sl->open) return;
	if (sl->shown > 0) printf("\n");
	fflush(stdout);
	sl->parked = 1;
	sl->open = 0;
	clOutEnd(&G_AI);
}

/* Take back the raw text echoed so far for this line, so the rendered version
   can replace it. Leaves the bracket open with the cursor at the line start. */
static void clStreamUnecho(clStreamLine *sl) {
	clStreamResume(sl);
	if (sl->shown <= 0) return;
	int cols = clTermCols(); if (cols < 2) cols = 80;
	int up = sl->shown / cols;            /* rows below the partial's first row */
	if (up > 0) printf("\x1b[%dA", up);
	printf("\r\x1b[J");
	sl->shown = 0;
}

static void clStreamEmit(clStreamLine *sl) {
	sl->buf[sl->len] = '\0';
	clStreamUnecho(sl);
	clPrintRoleLine(HK_ROLE_AI, sl->buf);   /* nested: depth > 0, no bar */
	sl->len = 0;
}

static void clStreamPush(clStreamLine *sl, const char *chunk) {
	hkDeltaPush(chunk);
	clStreamResume(sl);
	size_t start = sl->len;
	for (const char *p = chunk; *p; p++) {
		if (*p == '\r') continue;
		if (*p == '\n') {
			clStreamEmit(sl);
			start = 0;
			continue;
		}
		if (sl->len + 2 >= sizeof(sl->buf)) { clStreamEmit(sl); start = 0; }
		sl->buf[sl->len++] = *p;
	}
	/* Show what just arrived, immediately. */
	if (sl->len > start) {
		if (sl->shown == 0) {
			const char *pre = clRailOn() ? HK_RAIL_KEYSTONE : "\xE2\x97\x86  ";
			printf("%s%s%s", E.color_enabled ? TH_AI : "", pre, E.color_enabled ? ANSI_RESET : "");
			sl->shown = clCellWidth(pre);
		}
		char tmp[8192];
		size_t n = sl->len - start;
		if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
		memcpy(tmp, sl->buf + start, n); tmp[n] = '\0';
		fputs(tmp, stdout);
		sl->shown += clCellWidth(tmp);
	}
	clStreamPark(sl);
}

/* Flush the partial line and give the flow back; the bar is already up. */
static void clStreamEnd(clStreamLine *sl) {
	if (sl->len) { clStreamResume(sl); clStreamEmit(sl); }
	else if (sl->shown) clStreamUnecho(sl);     /* withdrawn: nothing to render */
	if (sl->open) { sl->open = 0; sl->parked = 0; clOutEnd(&G_AI); }
}

/* Bracketed so the bar comes back underneath whatever this prints — that is
   what keeps it visible through a turn without pinning it to a fixed row. */
static void clPrintRoleLineRaw(unsigned char role, const char *text);

static void clPrintRoleLine(unsigned char role, const char *text) {
	clOutBegin();
	clPrintRoleLineRaw(role, text);
	clOutEnd(&G_AI);
}

static void clPrintRoleLineRaw(unsigned char role, const char *text) {
	if (E.output_json) return;
	if (text && text[0] == '\x01') return;   /* transcript marker, not content */
	if (E.pipe_mode) {
		const char *rname = (role == HK_ROLE_USER) ? "user" :
		                    (role == HK_ROLE_AI)   ? "ai"   : "system";
		clPipeEmitMsg(rname, text);
		return;
	}
	const char *prefix, *color;
	switch (role) {
	case HK_ROLE_USER: prefix = "›  "; color = TH_USER; break;
	case HK_ROLE_AI:
		prefix = clRailOn()
		       ? (E.last_role_shown == HK_ROLE_AI ? HK_RAIL_BODY : HK_RAIL_KEYSTONE)
		       : "◆  ";
		color = TH_AI;
		break;
	default:
		if (text && (!strncmp(text, "Error", 5) || !strncmp(text, "error", 5))) {
			prefix = "!  "; color = TH_ERR;
		} else {
			prefix = "·  "; color = TH_SYS;
		}
		break;
	}

	if (role == HK_ROLE_AI && text) {
		const char *t = text;
		while (*t == ' ' || *t == '\t') t++;
		if (t[0] == '`' && t[1] == '`' && t[2] == '`') {
			cl_in_codefence = !cl_in_codefence;
			if (E.color_enabled) {
				printf("%s%s%s%s%s\n", ANSI_TOOL, prefix, ANSI_DIM, cl_in_codefence ? "─── code ───" : "────────────", ANSI_RESET);
			} else {
				printf("%s%s\n", prefix, cl_in_codefence ? "--- code ---" : "------------");
			}
			fflush(stdout);
			E.last_role_shown = role;
			return;
		}
	}
	if (role == HK_ROLE_AI && cl_in_codefence) {
		/* Code wraps too — a long line used to run off the right edge and take
		   the rest of the block's alignment with it. */
		char bar[32];
		snprintf(bar, sizeof(bar), "%s%s ", E.color_enabled ? ANSI_TOOL : "",
		         E.color_enabled ? "\xE2\x96\x8E" : "|");
		clPutWrapped(E.color_enabled ? ANSI_AI : "", bar, text ? text : "");
		E.last_role_shown = role;
		return;
	}

	/* A blank line is spacing, not content — giving it a gutter turns every
	   paragraph break into a row of floating markers. */
	if (!text || !*text) {
		if (clRailOn() && role == HK_ROLE_AI && E.last_role_shown == HK_ROLE_AI)
			printf("%s%s%s\n", TH_ACCENT, HK_RAIL_BODY, ANSI_RESET);
		else
			printf("\n");
		fflush(stdout);
		E.last_role_shown = role;
		return;
	}
	if (E.color_enabled && role == HK_ROLE_AI) {
		char rendered[8192];
		clRenderMarkdownInline(text, rendered, sizeof(rendered));
		char pfx[48];
		if (clRailOn()) snprintf(pfx, sizeof(pfx), "%s%s%s", TH_ACCENT, prefix, ANSI_RESET);
		else            snprintf(pfx, sizeof(pfx), "%s", prefix);
		char cont[48];
		if (clRailOn()) snprintf(cont, sizeof(cont), "%s%s%s", TH_ACCENT, HK_RAIL_BODY, ANSI_RESET);
		else            snprintf(cont, sizeof(cont), "   ");
		clPutWrapped2(color, pfx, cont, rendered);
	} else {
		clPutWrapped(color, prefix, text);
	}
	fflush(stdout);
	E.last_role_shown = role;
}
static void aiPushHistoryStore(aiData *data, const char *text, unsigned char role) {
	if (data->history_count >= AI_HISTORY_MAX) return;
	data->history[data->history_count] = strdup(text ? text : "");
	data->history_role[data->history_count] = role;
	data->history_count++;
}

/* A menu opened and closed leaves no trace: its command line is echoed only
   once something prints, and a pick confirms itself in the bar, not the chat. */
static const char *hk_echo_pending = NULL;
static int hk_menu_pick = 0;
static void hkEchoFlush(aiData *data) {
	if (!hk_echo_pending) return;
	const char *l = hk_echo_pending;
	hk_echo_pending = NULL;
	clPrintRoleLine(HK_ROLE_USER, l);
	aiPushHistoryStore(data, l, HK_ROLE_USER);
}
/* Every front end can answer a picker: the tty draws it, --pipe and --serve send
   a choice_request. Only a plain non-tty run (-p, scripts) cannot. */
static int hkCanPick(void) { return isatty(STDIN_FILENO) || E.pipe_mode || E.serve_mode; }

static int hk_say_secret;
static int hk_pipe_quiet;        /* POST /api/cmd: a settings command, nothing said in the chat */
static void aiAddHistoryRole(aiData *data, const char *text, unsigned char role) {
	if (!data || !text) return;
	if (!E.pipe_mode) hkEchoFlush(data);
	if (E.pipe_mode) {
		const char *rname = (role == HK_ROLE_USER) ? "user" :
		                    (role == HK_ROLE_AI)   ? "ai"   : "system";
		if (!hk_pipe_quiet) clPipeEmitMsg(rname, text);
		const char *p2 = text;
		while (1) {
			const char *nl = strchr(p2, '\n');
			int seg = nl ? (int)(nl - p2) : (int)strlen(p2);
			char tmp[4096];
			int n = seg < (int)sizeof(tmp) - 1 ? seg : (int)sizeof(tmp) - 1;
			memcpy(tmp, p2, n);
			tmp[n] = '\0';
			aiPushHistoryStore(data, tmp, role);
			if (!nl) break;
			p2 = nl + 1;
		}
		return;
	}
	int echo = !(role == HK_ROLE_AI && data->suppress_next_echo);
	if (hs_attached && text[0] != '\x01' && echo && !hk_say_secret)    /* streamed replies reach the phone on their own path */
		clPipeEmitMsg(role == HK_ROLE_USER ? "user" : role == HK_ROLE_AI ? "ai" : "system", text);
	data->suppress_next_echo = 0;
	const char *p = text;
	while (1) {
		const char *nl = strchr(p, '\n');
		int seg = nl ? (int)(nl - p) : (int)strlen(p);
		char tmp[4096];
		int n = seg < (int)sizeof(tmp) - 1 ? seg : (int)sizeof(tmp) - 1;
		memcpy(tmp, p, n);
		tmp[n] = '\0';
		aiPushHistoryStore(data, tmp, role);
		if (echo) clPrintRoleLine(role, tmp);
		if (!nl) break;
		p = nl + 1;
	}
}

static void aiAddHistory(aiData *data, const char *text) {
	aiAddHistoryRole(data, text, HK_ROLE_SYSTEM);
	if (text && text[0] && text[0] != '\x01') hkLogMessage("system", text);
}

static void aiSayf(aiData *data, const char *fmt, ...) {
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	aiAddHistory(data, msg);
}

/* aiSayf for a line that carries a secret: shown here, never logged, never sent to a remote device. */
static void aiSayfSecret(aiData *data, const char *fmt, ...) {
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	hk_say_secret = 1;
	aiAddHistoryRole(data, msg, HK_ROLE_SYSTEM);
	hk_say_secret = 0;
}

static void aiClearHistory(aiData *data) {
	for (int i = 0; i < data->history_count; i++) free(data->history[i]);
	memset(data->history_role, 0, AI_HISTORY_MAX);
	data->history_count = 0;
	aiFreeMessages(data);
}

static void aiPushMessage(aiData *data, const char *role, const char *content) {
	if (!data || !role || !content) return;
	if (data->message_count >= data->message_cap) {
		data->message_cap = data->message_cap ? data->message_cap * 2 : 16;
		data->messages = realloc(data->messages, sizeof(aiMessage) * data->message_cap);
	}
	data->messages[data->message_count].role = strdup(role);
	data->messages[data->message_count].content = strdup(content);
	data->messages[data->message_count].raw = 0;
	data->message_count++;
}

static void aiPushMessageRaw(aiData *data, const char *role, const char *content_json) {
	if (!data || !role || !content_json) return;
	if (data->message_count >= data->message_cap) {
		data->message_cap = data->message_cap ? data->message_cap * 2 : 16;
		data->messages = realloc(data->messages, sizeof(aiMessage) * data->message_cap);
	}
	data->messages[data->message_count].role = strdup(role);
	data->messages[data->message_count].content = strdup(content_json);
	data->messages[data->message_count].raw = 1;
	data->message_count++;
}

static void aiPushMessageBody(aiData *data, const char *role, const char *body_fields) {
	if (!data || !role || !body_fields) return;
	if (data->message_count >= data->message_cap) {
		data->message_cap = data->message_cap ? data->message_cap * 2 : 16;
		data->messages = realloc(data->messages, sizeof(aiMessage) * data->message_cap);
	}
	data->messages[data->message_count].role = strdup(role);
	data->messages[data->message_count].content = strdup(body_fields);
	data->messages[data->message_count].raw = 2;
	data->message_count++;
}

static int aiFindLastMessageRole(aiData *data, const char *role) {
	if (!data || !role) return -1;
	for (int i = data->message_count - 1; i >= 0; i--) {
		if (data->messages[i].role && !strcmp(data->messages[i].role, role)) return i;
	}
	return -1;
}

static void aiDropMessagesFrom(aiData *data, int idx) {
	if (!data || idx < 0 || idx >= data->message_count) return;
	for (int i = idx; i < data->message_count; i++) {
		free(data->messages[i].role); free(data->messages[i].content);
	}
	data->message_count = idx;
}

static void hkDropTrailingHistory(aiData *data, unsigned char role_to_drop) {
	if (!data) return;
	while (data->history_count > 0 && data->history_role[data->history_count - 1] == role_to_drop) {
		free(data->history[--data->history_count]);
		data->history[data->history_count] = NULL;
	}
}

static void aiFlattenMessages(aiData *data) {
	if (!data || data->message_count == 0) return;
	int kept = 0;
	for (int i = 0; i < data->message_count; i++) {
		aiMessage *m = &data->messages[i];
		if (m->raw == 0) {
			if (i != kept) data->messages[kept] = *m;
			kept++;
			continue;
		}
		const char *src = m->content ? m->content : "";
		char *text = NULL;
		const char *p = strstr(src, "\"text\":\"");
		if (p) {
			p += 8;
			text = aiExtractStringValue(p);
		} else if (m->raw == 2) {
			const char *cp = strstr(src, "\"content\":\"");
			if (cp) { cp += 11; text = aiExtractStringValue(cp); }
		}
		if (text && *text) {
			int is_tool = m->role && !strcmp(m->role, "tool");
			free(m->role);
			free(m->content);
			data->messages[kept].role = strdup(is_tool ? "user" : "assistant");
			data->messages[kept].content = text;
			data->messages[kept].raw = 0;
			kept++;
		} else {
			free(m->role);
			free(m->content);
			free(text);
		}
	}
	data->message_count = kept;
}

static void aiFreeMessages(aiData *data) {
	if (!data || !data->messages) return;
	for (int i = 0; i < data->message_count; i++) {
		free(data->messages[i].role);
		free(data->messages[i].content);
	}
	free(data->messages);
	data->messages = NULL;
	data->message_count = 0;
	data->message_cap = 0;
}

static char *aiBuildMessagesJson(aiData *data) {
	int cap = 4096;
	char *out = malloc(cap);
	if (!out) return NULL;
	int len = 0;
	out[len++] = '[';
	/* images ride along from img_from on: the newest HK_IMAGE_KEEP, and all of the last message's */
	int wire = hkImageWire(), img_from = data->message_count;
	for (int i = data->message_count - 1, left = HK_IMAGE_KEEP; wire && i >= 0; i--) {
		const aiMessage *m = &data->messages[i];
		if (m->raw || !m->content || !m->role || strcmp(m->role, "user") || !strstr(m->content, "[image #")) continue;
		left -= hkImageRefs(m->content);
		if (left < 0 && img_from < data->message_count) break;
		img_from = i;
	}
	for (int i = 0; i < data->message_count; i++) {
		const char *content = data->messages[i].content ? data->messages[i].content : "";
		int clen = strlen(content);
		char *with_img = i >= img_from && !data->messages[i].raw && !strcmp(data->messages[i].role, "user")
		               ? hkUserImageMsg(content, wire) : NULL;
		int need = len + (with_img ? (int)strlen(with_img) : clen * 6) + 128;
		if (need >= cap) {
			while (cap < need) cap *= 2;
			out = realloc(out, cap);
			if (!out) { free(with_img); return NULL; }
		}
		if (i > 0) out[len++] = ',';
		if (with_img) {
			int wl = (int)strlen(with_img);
			memcpy(out + len, with_img, wl);
			len += wl;
			free(with_img);
		} else if (data->messages[i].raw == 1) {
			int need2 = len + clen + 64;
			if (need2 >= cap) { while (cap < need2) cap *= 2; out = realloc(out, cap); }
			len += snprintf(out + len, cap - len, "{\"role\":\"%s\",\"content\":", data->messages[i].role);
			memcpy(out + len, content, clen);
			len += clen;
			out[len++] = '}';
		} else if (data->messages[i].raw == 2) {
			int need2 = len + clen + 64;
			if (need2 >= cap) { while (cap < need2) cap *= 2; out = realloc(out, cap); }
			len += snprintf(out + len, cap - len, "{\"role\":\"%s\",", data->messages[i].role);
			memcpy(out + len, content, clen);
			len += clen;
			out[len++] = '}';
		} else {
			len += snprintf(out + len, cap - len, "{\"role\":\"%s\",\"content\":\"", data->messages[i].role);
			char *esc = malloc(clen * 6 + 8);
			hkJsonEscapeInto(content, esc, clen * 6 + 8);
			int elen = strlen(esc);
			if (len + elen + 8 >= cap) {
				cap = (len + elen) * 2;
				out = realloc(out, cap);
			}
			memcpy(out + len, esc, elen);
			len += elen;
			free(esc);
			out[len++] = '"';
			out[len++] = '}';
		}
	}
	out[len++] = ']';
	out[len] = '\0';
	return out;
}


typedef struct {
	int   server_idx;
	char *qualified;
	char *remote;
	char *description;
	char *schema;
} mcpTool;

typedef struct {
	char  *name;
	int    fd;
	pid_t  pid;
	int    next_id;
	char  *rbuf; size_t rlen, rcap;
} mcpServer;

static mcpTool   *g_mcp_tools   = NULL; static int g_mcp_tool_count   = 0;

/* From here to the end of the MCP block is the stdio JSON-RPC client, which… */
#if !defined(_WIN32) && !defined(HAKO_WASM)
static mcpServer *g_mcp_servers = NULL; static int g_mcp_server_count = 0;

static char **hkJsonArrayObjects(const char *arr, int *out_n) {
	*out_n = 0; char **out = NULL; int n = 0, cap = 0;
	const char *p = arr; while (*p && *p != '[') p++; if (*p == '[') p++;
	while (*p) {
		while (*p==' '||*p=='\n'||*p=='\r'||*p=='\t'||*p==',') p++;
		if (*p == ']' || !*p) break;
		if (*p != '{') { p++; continue; }
		const char *start = p; int depth = 0, instr = 0, esc = 0;
		while (*p) {
			char c = *p;
			if (esc) esc = 0;
			else if (c == '\\') esc = 1;
			else if (c == '"') instr = !instr;
			else if (!instr && c == '{') depth++;
			else if (!instr && c == '}') { depth--; if (depth == 0) { p++; break; } }
			p++;
		}
		int len = (int)(p - start);
		if (len > 0) {
			char *obj = malloc(len + 1); if (!obj) break;
			memcpy(obj, start, len); obj[len] = '\0';
			if (n >= cap) { cap = cap ? cap*2 : 8; char **t = realloc(out, cap*sizeof(char*)); if (!t) { free(obj); break; } out = t; }
			out[n++] = obj;
		}
	}
	*out_n = n; return out;
}

static char **hkJsonStringArray(const char *arr, int *out_n) {
	*out_n = 0; char **out = NULL; int n = 0, cap = 0;
	const char *p = arr; while (*p && *p != '[') p++; if (*p == '[') p++;
	while (*p) {
		while (*p==' '||*p=='\n'||*p=='\r'||*p=='\t'||*p==',') p++;
		if (*p == ']' || !*p) break;
		if (*p != '"') { p++; continue; }
		p++; const char *s = p; int esc = 0;
		while (*p && !(*p=='"' && !esc)) { esc = (!esc && *p=='\\'); p++; }
		int len = (int)(p - s);
		char *u = hkJsonUnescape(s, len);
		if (u) {
			if (n >= cap) { cap = cap ? cap*2 : 8; char **t = realloc(out, cap*sizeof(char*)); if (!t) { free(u); break; } out = t; }
			out[n++] = u;
		}
		if (*p == '"') p++;
	}
	*out_n = n; return out;
}

static int hkJsonObjectEntries(const char *obj, char ***keys, char ***vals) {
	*keys = NULL; *vals = NULL; int n = 0, cap = 0;
	const char *p = obj; while (*p && *p != '{') p++; if (*p == '{') p++;
	while (*p) {
		while (*p==' '||*p=='\n'||*p=='\r'||*p=='\t'||*p==',') p++;
		if (*p == '}' || !*p) break;
		if (*p != '"') break;
		p++; const char *ks = p; int esc = 0;
		while (*p && !(*p=='"' && !esc)) { esc = (!esc && *p=='\\'); p++; }
		int klen = (int)(p - ks); if (*p == '"') p++;
		while (*p==' '||*p=='\n'||*p=='\r'||*p=='\t') p++;
		if (*p == ':') p++;
		while (*p==' '||*p=='\n'||*p=='\r'||*p=='\t') p++;
		const char *vs = p;
		if (*p == '{' || *p == '[') {
			char open = *p, close = (open=='{') ? '}' : ']'; int depth = 0, instr = 0; esc = 0;
			while (*p) { char c = *p;
				if (esc) esc = 0; else if (c=='\\') esc = 1; else if (c=='"') instr = !instr;
				else if (!instr && c==open) depth++;
				else if (!instr && c==close) { depth--; if (depth==0) { p++; break; } }
				p++; }
		} else if (*p == '"') {
			p++; esc = 0; while (*p && !(*p=='"' && !esc)) { esc = (!esc && *p=='\\'); p++; } if (*p=='"') p++;
		} else { while (*p && *p!=',' && *p!='}') p++; }
		int vlen = (int)(p - vs);
		char *k = malloc(klen+1), *v = malloc(vlen+1);
		if (!k || !v) { free(k); free(v); break; }
		memcpy(k, ks, klen); k[klen] = '\0';
		memcpy(v, vs, vlen); v[vlen] = '\0';
		if (n >= cap) { cap = cap ? cap*2 : 8;
			char **tk = realloc(*keys, cap*sizeof(char*)), **tv = realloc(*vals, cap*sizeof(char*));
			if (!tk || !tv) { free(k); free(v); free(tk?tk:*keys); break; }
			*keys = tk; *vals = tv; }
		(*keys)[n] = k; (*vals)[n] = v; n++;
	}
	return n;
}

static char *hkJsonUnquote(const char *raw) {
	if (!raw) return NULL;
	if (raw[0] == '"') {
		int len = (int)strlen(raw); if (len >= 2 && raw[len-1] == '"') return hkJsonUnescape(raw+1, len-2);
	}
	return strdup(raw);
}

static int hkMcpWriteAll(int fd, const char *buf, size_t len) {
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(fd, buf + off, len - off);
		if (w <= 0) { if (errno == EINTR) continue; return -1; }
		off += (size_t)w;
	}
	return 0;
}

static char *hkMcpReadLine(mcpServer *s, int timeout_ms) {
	for (;;) {
		for (size_t i = 0; i < s->rlen; i++) {
			if (s->rbuf[i] == '\n') {
				char *line = malloc(i + 1); if (!line) return NULL;
				memcpy(line, s->rbuf, i); line[i] = '\0';
				memmove(s->rbuf, s->rbuf + i + 1, s->rlen - i - 1);
				s->rlen -= (i + 1);
				return line;
			}
		}
		struct pollfd pfd = { s->fd, POLLIN, 0 };
		int pr = poll(&pfd, 1, timeout_ms);
		if (pr <= 0) return NULL;
		if (s->rcap - s->rlen < 4096) {
			if (s->rcap >= 4u*1024*1024) return NULL;
			size_t nc = s->rcap ? s->rcap * 2 : 65536;
			char *nb = realloc(s->rbuf, nc); if (!nb) return NULL;
			s->rbuf = nb; s->rcap = nc;
		}
		ssize_t r = read(s->fd, s->rbuf + s->rlen, s->rcap - s->rlen);
		if (r <= 0) { if (r < 0 && errno == EINTR) continue; return NULL; }
		s->rlen += (size_t)r;
	}
}

static int hkMcpLineHasId(const char *line, int id) {
	return hkExtractJsonInt(line, "id") == id;
}

static char *hkMcpRequest(mcpServer *s, const char *method, const char *params, int timeout_ms) {
	if (!s || s->fd < 0) return NULL;
	int id = s->next_id++;
	char hdr[256];
	int hlen = snprintf(hdr, sizeof(hdr), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":", id, method);
	if (hkMcpWriteAll(s->fd, hdr, (size_t)hlen) != 0) return NULL;
	if (hkMcpWriteAll(s->fd, params, strlen(params)) != 0) return NULL;
	if (hkMcpWriteAll(s->fd, "}\n", 2) != 0) return NULL;
	for (int tries = 0; tries < 1000; tries++) {
		char *line = hkMcpReadLine(s, timeout_ms);
		if (!line) return NULL;
		if (hkMcpLineHasId(line, id)) return line;
		free(line);
	}
	return NULL;
}

static void hkMcpNotify(mcpServer *s, const char *method, const char *params) {
	if (!s || s->fd < 0) return;
	char hdr[256];
	int hlen = snprintf(hdr, sizeof(hdr), "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":", method);
	hkMcpWriteAll(s->fd, hdr, (size_t)hlen);
	hkMcpWriteAll(s->fd, params, strlen(params));
	hkMcpWriteAll(s->fd, "}\n", 2);
}

static void hkMcpRegisterTools(int srv_idx, const char *tools_arr) {
	int n = 0; char **objs = hkJsonArrayObjects(tools_arr, &n);
	for (int i = 0; i < n; i++) {
		char *nm = hkExtractJsonString(objs[i], "name");
		if (nm) {
			char *desc = hkExtractJsonString(objs[i], "description");
			char *sch  = hkExtractJsonObject(objs[i], "inputSchema");
			char qual[256];
			snprintf(qual, sizeof(qual), "mcp__%s__%s", g_mcp_servers[srv_idx].name, nm);
			mcpTool *nt = realloc(g_mcp_tools, (g_mcp_tool_count + 1) * sizeof(*nt));
			if (nt) {
				g_mcp_tools = nt;
				mcpTool *t = &g_mcp_tools[g_mcp_tool_count];
				t->server_idx  = srv_idx;
				t->qualified   = strdup(qual);
				t->remote      = nm;   nm   = NULL;
				t->description = desc ? desc : strdup("(no description)"); desc = NULL;
				t->schema      = sch  ? sch  : strdup("{\"type\":\"object\"}"); sch = NULL;
				g_mcp_tool_count++;
			}
			free(nm); free(desc); free(sch);
		}
		free(objs[i]);
	}
	free(objs);
}

static int hkMcpConnect(const char *name, const char *command, char **argv,
                        char **env_k, char **env_v, int env_n) {
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
	pid_t pid = fork();
	if (pid < 0) { close(sv[0]); close(sv[1]); return -1; }
	if (pid == 0) {
		close(sv[0]);
		dup2(sv[1], STDIN_FILENO);
		dup2(sv[1], STDOUT_FILENO);
		if (sv[1] > 2) close(sv[1]);
		if (!getenv("HAKO_MCP_DEBUG")) {
			int dn = open("/dev/null", O_WRONLY);
			if (dn >= 0) { dup2(dn, STDERR_FILENO); if (dn > 2) close(dn); }
		}
		for (int i = 0; i < env_n; i++) setenv(env_k[i], env_v[i], 1);
		execvp(command, argv);
		_exit(127);
	}
	close(sv[1]);
	mcpServer *ns = realloc(g_mcp_servers, (g_mcp_server_count + 1) * sizeof(*ns));
	if (!ns) { close(sv[0]); kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1; }
	g_mcp_servers = ns;
	int idx = g_mcp_server_count;
	mcpServer *s = &g_mcp_servers[idx];
	memset(s, 0, sizeof(*s));
	s->name = strdup(name); s->fd = sv[0]; s->pid = pid; s->next_id = 1;
	s->rcap = 65536; s->rbuf = malloc(s->rcap); s->rlen = 0;
	g_mcp_server_count++;
	if (!s->rbuf) { close(s->fd); s->fd = -1; kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1; }

	char *resp = hkMcpRequest(s, "initialize",
		"{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
		"\"clientInfo\":{\"name\":\"hako-code\",\"version\":\"" HAKO_VERSION "\"}}", 10000);
	if (!resp) {
		close(s->fd); s->fd = -1; kill(pid, SIGKILL); waitpid(pid, NULL, 0);
		return idx;
	}
	free(resp);
	hkMcpNotify(s, "notifications/initialized", "{}");
	char *tl = hkMcpRequest(s, "tools/list", "{}", 10000);
	if (tl) {
		char *arr = hkExtractRawJsonArray(tl, "tools");
		if (arr) { hkMcpRegisterTools(idx, arr); free(arr); }
		free(tl);
	}
	return idx;
}
#endif

#if !defined(_WIN32) && !defined(HAKO_WASM)
static void hkMcpInit(void) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	char path[1024]; snprintf(path, sizeof(path), "%s/.hako/mcp.json", home);
	char *txt = hkReadFileAll(path, 256*1024);
	if (!txt) return;
	char *servers = hkExtractJsonObject(txt, "mcpServers");
	if (servers) {
		char **keys = NULL, **vals = NULL;
		int n = hkJsonObjectEntries(servers, &keys, &vals);
		for (int i = 0; i < n; i++) {
			char *cmd = hkExtractJsonString(vals[i], "command");
			if (cmd) {
				char *argsarr = hkExtractRawJsonArray(vals[i], "args");
				int an = 0; char **as = argsarr ? hkJsonStringArray(argsarr, &an) : NULL;
				char **argv = malloc((an + 2) * sizeof(char*));
				if (argv) {
					argv[0] = cmd;
					for (int j = 0; j < an; j++) argv[j+1] = as[j];
					argv[an+1] = NULL;
					char *envobj = hkExtractJsonObject(vals[i], "env");
					char **ek = NULL, **ev = NULL, **evq = NULL; int en = 0;
					if (envobj) {
						en = hkJsonObjectEntries(envobj, &ek, &ev);
						evq = malloc(en * sizeof(char*));
						for (int j = 0; j < en; j++) evq[j] = hkJsonUnquote(ev[j]);
					}
					hkMcpConnect(keys[i], cmd, argv, ek, evq, en);
					for (int j = 0; j < en; j++) { free(ek[j]); free(ev[j]); free(evq[j]); }
					free(ek); free(ev); free(evq); free(envobj);
					free(argv);
				}
				for (int j = 0; j < an; j++) free(as[j]);
				free(as); free(argsarr); free(cmd);
			}
			free(keys[i]); free(vals[i]);
		}
		free(keys); free(vals); free(servers);
	}
	free(txt);
}

static char *hkMcpCallTool(const char *qualified_name, const char *args_json) {
	for (int i = 0; i < g_mcp_tool_count; i++) {
		if (strcmp(g_mcp_tools[i].qualified, qualified_name) != 0) continue;
		mcpServer *s = &g_mcp_servers[g_mcp_tools[i].server_idx];
		if (s->fd < 0) return strdup("error: MCP server is not connected");
		const char *a = (args_json && *args_json) ? args_json : "{}";
		size_t plen = strlen(g_mcp_tools[i].remote) + strlen(a) + 64;
		char *params = malloc(plen); if (!params) return strdup("error: out of memory");
		snprintf(params, plen, "{\"name\":\"%s\",\"arguments\":%s}", g_mcp_tools[i].remote, a);
		char *resp = hkMcpRequest(s, "tools/call", params, 60000);
		free(params);
		if (!resp) return strdup("error: MCP server did not respond (timeout)");
		char *arr = hkExtractRawJsonArray(resp, "content");
		char *out = NULL;
		if (arr) {
			int n = 0; char **objs = hkJsonArrayObjects(arr, &n);
			size_t cap = 256, len = 0; out = malloc(cap); if (out) out[0] = '\0';
			for (int j = 0; out && j < n; j++) {
				char *tx = hkExtractJsonString(objs[j], "text");
				if (tx) {
					char *u = hkJsonUnescape(tx, (int)strlen(tx));
					const char *use = u ? u : tx; size_t ul = strlen(use);
					if (len + ul + 2 > cap) { while (len + ul + 2 > cap) cap *= 2; char *t = realloc(out, cap); if (!t) { free(u); free(tx); break; } out = t; }
					if (len) out[len++] = '\n';
					memcpy(out + len, use, ul); len += ul; out[len] = '\0';
					free(u);
				}
				free(tx); free(objs[j]);
			}
			for (int j = 0; j < n; j++) (void)j;
			free(objs); free(arr);
			if (out && len == 0) { free(out); out = NULL; }
		}
		if (!out) {
			char *ro = hkExtractJsonObject(resp, "result");
			out = ro ? ro : strdup("(mcp: no text content in result)");
		}
		free(resp);
		return out;
	}
	return strdup("error: unknown MCP tool");
}

static void hkMcpShutdown(void) {
	for (int i = 0; i < g_mcp_server_count; i++) {
		if (g_mcp_servers[i].fd >= 0) close(g_mcp_servers[i].fd);
		if (g_mcp_servers[i].pid > 0) { kill(g_mcp_servers[i].pid, SIGTERM); waitpid(g_mcp_servers[i].pid, NULL, WNOHANG); }
		free(g_mcp_servers[i].name); free(g_mcp_servers[i].rbuf);
	}
	free(g_mcp_servers); g_mcp_servers = NULL; g_mcp_server_count = 0;
	for (int i = 0; i < g_mcp_tool_count; i++) {
		free(g_mcp_tools[i].qualified); free(g_mcp_tools[i].remote);
		free(g_mcp_tools[i].description); free(g_mcp_tools[i].schema);
	}
	free(g_mcp_tools); g_mcp_tools = NULL; g_mcp_tool_count = 0;
}


static void hkMcpList(aiData *data) {
	if (g_mcp_server_count == 0) { aiAddHistory(data, "no MCP servers (configure ~/.hako/mcp.json)"); return; }
	for (int i = 0; i < g_mcp_server_count; i++) {
		int tools = 0;
		for (int j = 0; j < g_mcp_tool_count; j++) if (g_mcp_tools[j].server_idx == i) tools++;
		char line[256];
		snprintf(line, sizeof(line), "%s %s — %d tool(s)",
			g_mcp_servers[i].fd >= 0 ? "[ok]" : "[failed]", g_mcp_servers[i].name, tools);
		aiAddHistory(data, line);
		for (int j = 0; j < g_mcp_tool_count; j++)
			if (g_mcp_tools[j].server_idx == i) {
				aiSayf(data, "    %s", g_mcp_tools[j].qualified);
			}
	}
}
#else
static void  hkMcpInit(void) {}
static void  hkMcpShutdown(void) {}
static char *hkMcpCallTool(const char *q, const char *a) { (void)q; (void)a; return strdup("error: MCP is POSIX-only in this build"); }
static void  hkMcpList(aiData *data) { aiAddHistory(data, "MCP is not supported on this platform (POSIX only)."); }
#endif

static char *hkMcpLocalToolsPrompt(void) {
	if (g_mcp_tool_count <= 0) return NULL;
	size_t cap = 512, len = 0;
	char *out = malloc(cap);
	if (!out) return NULL;
	len += (size_t)snprintf(out, cap, "\n# MCP tools — call these with the SAME <tool_call> format:\n<tools>\n");
	for (int i = 0; i < g_mcp_tool_count; i++) {
		const char *nm = g_mcp_tools[i].qualified;
		const char *ds = g_mcp_tools[i].description ? g_mcp_tools[i].description : "";
		const char *sc = g_mcp_tools[i].schema ? g_mcp_tools[i].schema : "{\"type\":\"object\"}";
		size_t dsl = strlen(ds);
		char *dse = malloc(dsl * 6 + 8);
		if (dse) hkJsonEscapeInto(ds, dse, (int)(dsl * 6 + 8));
		size_t need = strlen(nm) + (dse ? strlen(dse) : 0) + strlen(sc) + 96;
		if (len + need > cap) { while (len + need > cap) cap *= 2; char *t = realloc(out, cap); if (!t) { free(dse); free(out); return NULL; } out = t; }
		len += (size_t)snprintf(out + len, cap - len,
			"{\"type\": \"function\", \"function\": {\"name\": \"%s\", \"description\": \"%s\", \"parameters\": %s}}\n",
			nm, dse ? dse : "", sc);
		free(dse);
	}
	if (len + 12 > cap) { char *t = realloc(out, len + 12); if (t) { out = t; cap = len + 12; } }
	snprintf(out + len, cap - len, "</tools>\n");
	return out;
}

static const char *hkMcpResolveName(const char *name) {
	if (!name) return NULL;
	for (int i = 0; i < g_mcp_tool_count; i++)
		if (strcmp(g_mcp_tools[i].qualified, name) == 0) return g_mcp_tools[i].qualified;
	char want[256]; int w = 0;
	for (const char *p = name; *p && w < (int)sizeof(want) - 1; p++) {
		unsigned char c = (unsigned char)*p;
		if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) want[w++] = (char)c;
	}
	want[w] = '\0';
	if (w == 0) return NULL;
	for (int i = 0; i < g_mcp_tool_count; i++) {
		char have[256]; int h = 0;
		for (const char *p = g_mcp_tools[i].qualified; *p && h < (int)sizeof(have) - 1; p++) {
			unsigned char c = (unsigned char)*p;
			if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) have[h++] = (char)c;
		}
		have[h] = '\0';
		if (strcmp(want, have) == 0) return g_mcp_tools[i].qualified;
	}
	return NULL;
}

static const char HK_ERR_TRUST[] =
	"error: project not trusted — ask the user to run :trust before any file access";
static const char HK_ERR_OUTSIDE[] =
	"error: path outside project. Use \".\" for project root, or a relative path. "
	"Do NOT retry with another absolute path; reply to the user in text instead.";

/* ---------- search: path matching, directory walking, grep, glob ---------- */

/* Growable output buffer. Several call sites open-code this realloc dance; this
   is the one both search tools and the HTML stripper share. */
static int hkSbAdd(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
	if (*len + n + 1 > *cap) {
		size_t want = (*cap ? *cap : 1024);
		while (want < *len + n + 1) want *= 2;
		char *bigger = realloc(*buf, want);
		if (!bigger) return -1;
		*buf = bigger;
		*cap = want;
	}
	memcpy(*buf + *len, s, n);
	*len += n;
	(*buf)[*len] = '\0';
	return 0;
}

/* '*' spans anything but '/', '**' spans '/' as well, '?' is one non-'/' char.
   No character classes and no alternation: agents search for identifiers. */
static int hkGlobMatch(const char *pat, const char *str) {
	while (*pat) {
		if (*pat == '*') {
			int cross = (pat[1] == '*');
			while (*pat == '*') pat++;
			if (cross && *pat == '/') pat++;   /* a leading double-star matches zero directories too */
			if (!*pat) return cross || !strchr(str, '/');
			for (const char *s = str;; s++) {
				if (hkGlobMatch(pat, s)) return 1;
				if (!*s) return 0;
				if (*s == '/' && !cross) return 0;
			}
		}
		if (*pat == '?') {
			if (!*str || *str == '/') return 0;
			pat++; str++;
			continue;
		}
		if (*pat != *str) return 0;
		pat++; str++;
	}
	return !*str;
}

/* A pattern with no '/' is matched against the basename, which is what "*.c"
   is always meant to mean. */
static int hkPathMatch(const char *pat, const char *rel) {
	if (strchr(pat, '/')) return hkGlobMatch(pat, rel);
	const char *base = strrchr(rel, '/');
	return hkGlobMatch(pat, base ? base + 1 : rel);
}

/* Never descend into these. */
static const char *HK_WALK_SKIP[] = {
	".git", ".hg", ".svn", "node_modules", "__pycache__", ".venv", "venv",
	".cache", ".next", ".tox", "dist", "build", "target", "vendor",
	".mypy_cache", ".pytest_cache", ".gradle", ".idea", ".DS_Store", NULL
};

static int hkWalkSkip(const char *name) {
	for (int i = 0; HK_WALK_SKIP[i]; i++)
		if (strcmp(name, HK_WALK_SKIP[i]) == 0) return 1;
	return 0;
}

/* Returns non-zero to stop the walk. */
typedef int (*hkWalkFn)(void *ud, const char *rel, int is_dir);

typedef struct {
	const char *root;
	char       *rel;        /* one reusable relative path, on the heap */
	size_t      rel_cap;
	hkWalkFn    fn;
	void       *ud;
} hkWalkCtx;

/* Paths live on the heap, not in the frame: musl's default thread stack is… */
static int hkWalkDir(hkWalkCtx *c, size_t rel_len, int depth) {
	if (depth > 24) return 0;
	size_t dcap = strlen(c->root) + rel_len + 2;
	char *dir = malloc(dcap);
	if (!dir) return 0;
	if (rel_len) snprintf(dir, dcap, "%s/%s", c->root, c->rel);
	else         snprintf(dir, dcap, "%s", c->root);
	char *listing = hk_fs_list(dir);
	free(dir);
	if (!listing) return 0;

	int stop = 0;
	char *p = listing;
	while (*p && !stop) {
		char *nl = strchr(p, '\n');
		if (nl) *nl = '\0';
		size_t n = strlen(p);
		int is_dir = (n && p[n - 1] == '/');
		if (is_dir) p[--n] = '\0';
		if (n && strcmp(p, ".") != 0 && strcmp(p, "..") != 0 &&
		    !(is_dir && hkWalkSkip(p))) {
			size_t sep  = rel_len ? 1 : 0;
			size_t need = rel_len + sep + n + 1;
			if (need > c->rel_cap) {
				char *bigger = realloc(c->rel, need * 2);
				if (bigger) { c->rel = bigger; c->rel_cap = need * 2; }
			}
			if (need <= c->rel_cap) {
				if (sep) c->rel[rel_len] = '/';
				memcpy(c->rel + rel_len + sep, p, n + 1);
				if (c->fn(c->ud, c->rel, is_dir)) stop = 1;
				else if (is_dir && hkWalkDir(c, rel_len + sep + n, depth + 1)) stop = 1;
				c->rel[rel_len] = '\0';
			}
		}
		if (!nl) break;
		p = nl + 1;
	}
	free(listing);
	return stop;
}

static void hkWalk(const char *root, hkWalkFn fn, void *ud) {
	hkWalkCtx c;
	c.root = root; c.rel_cap = 512; c.rel = malloc(c.rel_cap);
	c.fn = fn; c.ud = ud;
	if (!c.rel) return;
	c.rel[0] = '\0';
	hkWalkDir(&c, 0, 0);
	free(c.rel);
}

#define HK_GLOB_MAX   500
#define HK_GREP_HITS  200
#define HK_GREP_BYTES 50000
#define HK_GREP_FILE  2000000L   /* skip anything bigger; nothing useful greps a blob */

typedef struct {
	const char *pat;
	int    count;
	char  *out;
	size_t len, cap;
} hkGlobCtx;

static int hkGlobVisit(void *ud, const char *rel, int is_dir) {
	hkGlobCtx *g = (hkGlobCtx *)ud;
	if (is_dir) return 0;
	if (!hkPathMatch(g->pat, rel)) return 0;
	hkSbAdd(&g->out, &g->len, &g->cap, rel, strlen(rel));
	hkSbAdd(&g->out, &g->len, &g->cap, "\n", 1);
	return ++g->count >= HK_GLOB_MAX;
}

typedef struct {
	const char *root;
	const char *pat;
	const char *file_glob;
	int    icase;
	int    wild;          /* pattern holds * or ? -> match the whole line as a glob */
	int    hits;
	int    files;
	char  *out;
	size_t len, cap;
} hkGrepCtx;

static int hkGrepLineHit(const hkGrepCtx *g, const char *line) {
	if (g->wild) {
		/* Implicit ".*" on both ends: "foo*bar" should hit mid-line. */
		size_t n = strlen(g->pat);
		char *p = malloc(n + 3);
		if (!p) return 0;
		snprintf(p, n + 3, "*%s*", g->pat);
		int hit = hkGlobMatch(p, line);
		free(p);
		return hit;
	}
	return g->icase ? (strcasestr(line, g->pat) != NULL)
	                : (strstr(line, g->pat) != NULL);
}

static int hkGrepVisit(void *ud, const char *rel, int is_dir) {
	hkGrepCtx *g = (hkGrepCtx *)ud;
	if (is_dir) return 0;
	if (g->file_glob && *g->file_glob && !hkPathMatch(g->file_glob, rel)) return 0;

	size_t flen = strlen(g->root) + strlen(rel) + 2;
	char *full = malloc(flen);
	if (!full) return 0;
	snprintf(full, flen, "%s/%s", g->root, rel);

	long size = 0;
	if (!hk_fs_exists(full, &size, NULL, NULL) || size > HK_GREP_FILE) { free(full); return 0; }
	long got = 0;
	char *body = hk_fs_read(full, HK_GREP_FILE, &got);
	free(full);
	if (!body) return 0;

	/* A NUL in the first kilobyte means binary; grepping it yields noise. */
	long probe = got < 1024 ? got : 1024;
	for (long i = 0; i < probe; i++) if (!body[i]) { free(body); return 0; }

	g->files++;
	int stop = 0;
	int lineno = 0;
	char *p = body;
	while (*p && !stop) {
		char *nl = strchr(p, '\n');
		if (nl) *nl = '\0';
		lineno++;
		if (hkGrepLineHit(g, p)) {
			char head[PATH_MAX + 32];
			snprintf(head, sizeof(head), "%s:%d: ", rel, lineno);
			hkSbAdd(&g->out, &g->len, &g->cap, head, strlen(head));
			size_t n = strlen(p);
			if (n > 200) n = 200;          /* minified lines must not eat the budget */
			hkSbAdd(&g->out, &g->len, &g->cap, p, n);
			hkSbAdd(&g->out, &g->len, &g->cap, "\n", 1);
			if (++g->hits >= HK_GREP_HITS || g->len >= HK_GREP_BYTES) stop = 1;
		}
		if (!nl) break;
		p = nl + 1;
	}
	free(body);
	return stop;
}

/* Shared by both tools: resolve the optional "path" argument to a directory
   inside the project, defaulting to the project root. */
static int hkSearchRoot(const char *input_json, char *out, size_t cap) {
	char *path = hkJsonFirst(input_json, "path", "dir", "directory", NULL);
	if (!path) path = strdup(".");
	if (!path) return -1;
	int rc = hkResolveInProject(path, out, cap);
	free(path);
	if (rc != 0) return rc;
	int is_dir = 0;
	if (!hk_fs_exists(out, NULL, NULL, &is_dir) || !is_dir) return 1;
	return 0;
}

static char *hkToolGlob(const char *input_json) {
	char *pat = hkJsonFirst(input_json, "pattern", "glob", "query", NULL);
	if (!pat || !*pat) { free(pat); return strdup("error: missing pattern"); }

	char root[PATH_MAX];
	int root_rc = hkSearchRoot(input_json, root, sizeof(root));
	if (root_rc < 0) {
		free(pat);
		return strdup(HK_ERR_OUTSIDE);
	}
	if (root_rc > 0) {
		free(pat);
		return strdup("error: that path is not a directory in this project. Use \".\" for the project root, or call list_dir first to see what is there.");
	}

	hkGlobCtx g;
	memset(&g, 0, sizeof(g));
	g.pat = pat;
	hkWalk(root, hkGlobVisit, &g);
	free(pat);

	if (!g.count) { free(g.out); return strdup("no files matched"); }
	char tail[96];
	snprintf(tail, sizeof(tail), "\n(%d file%s%s)\n", g.count, g.count == 1 ? "" : "s",
	         g.count >= HK_GLOB_MAX ? ", capped" : "");
	hkSbAdd(&g.out, &g.len, &g.cap, tail, strlen(tail));
	return g.out;
}

static char *hkToolGrep(const char *input_json) {
	char *pat = hkJsonFirst(input_json, "pattern", "query", "text", "search", NULL);
	if (!pat || !*pat) { free(pat); return strdup("error: missing pattern"); }

	char root[PATH_MAX];
	int root_rc = hkSearchRoot(input_json, root, sizeof(root));
	if (root_rc < 0) {
		free(pat);
		return strdup(HK_ERR_OUTSIDE);
	}
	if (root_rc > 0) {
		free(pat);
		return strdup("error: that path is not a directory in this project. Use \".\" for the project root, or call list_dir first to see what is there.");
	}

	char *fglob = hkJsonFirst(input_json, "glob", "include", "file_pattern", NULL);

	hkGrepCtx g;
	memset(&g, 0, sizeof(g));
	g.root = root;
	g.pat  = pat;
	g.file_glob = fglob;
	g.icase = hkExtractJsonInt(input_json, "ignore_case") > 0;
	g.wild  = (strchr(pat, '*') || strchr(pat, '?')) ? 1 : 0;

	hkWalk(root, hkGrepVisit, &g);

	char tail[160];
	if (!g.hits) {
		snprintf(tail, sizeof(tail), "no matches for \"%s\" in %d file%s",
		         pat, g.files, g.files == 1 ? "" : "s");
		free(pat); free(fglob); free(g.out);
		return strdup(tail);
	}
	snprintf(tail, sizeof(tail), "\n(%d match%s in %d file%s%s)\n",
	         g.hits, g.hits == 1 ? "" : "es", g.files, g.files == 1 ? "" : "s",
	         (g.hits >= HK_GREP_HITS || g.len >= HK_GREP_BYTES) ? ", capped" : "");
	hkSbAdd(&g.out, &g.len, &g.cap, tail, strlen(tail));
	free(pat); free(fglob);
	return g.out;
}

/* ---------- web_fetch ---------- */

#define HK_WEB_BYTES 50000

/* The URL comes from the model, and hk_http_curl_cmd hands its command to a shell. */
static int hkUrlSafe(const char *u) {
	if (!u || !*u) return 0;
	if (strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0) return 0;
	if (strlen(u) > 2000) return 0;
	for (const unsigned char *p = (const unsigned char *)u; *p; p++) {
		if (*p <= 0x20 || *p >= 0x7f) return 0;      /* space, control, non-ASCII */
		if (strchr("'\"\\`<>|", *p)) return 0;
	}
	return 1;
}

static void hkHtmlEntity(const char *ent, size_t n, char **out, size_t *len, size_t *cap) {
	static const struct { const char *name; const char *val; } tbl[] = {
		{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""},
		{"apos", "'"}, {"#39", "'"}, {"nbsp", " "}, {"mdash", "-"},
		{"ndash", "-"}, {"hellip", "..."}, {NULL, NULL}
	};
	for (int i = 0; tbl[i].name; i++)
		if (strlen(tbl[i].name) == n && strncmp(ent, tbl[i].name, n) == 0) {
			hkSbAdd(out, len, cap, tbl[i].val, strlen(tbl[i].val));
			return;
		}
	hkSbAdd(out, len, cap, " ", 1);
}

/* Tags out, script/style bodies out, runs of whitespace collapsed. Not a
   parser — a reader. */
static char *hkHtmlToText(const char *html) {
	char  *out = NULL;
	size_t len = 0, cap = 0;
	int    space = 1;                 /* leading whitespace is already collapsed */
	for (const char *p = html; *p; ) {
		if (*p == '<') {
			int drop = 0;
			const char *close = NULL;
			if (strncasecmp(p, "<script", 7) == 0)     { drop = 1; close = "</script"; }
			else if (strncasecmp(p, "<style", 6) == 0) { drop = 1; close = "</style";  }
			if (drop) {
				const char *end = strcasestr(p + 1, close);
				p = end ? end : p + strlen(p);
				if (*p) { const char *gt = strchr(p, '>'); p = gt ? gt + 1 : p + strlen(p); }
				continue;
			}
			/* Block-level tags become a line break so the text keeps its shape. */
			int brk = (strncasecmp(p, "<p", 2) == 0 || strncasecmp(p, "<br", 3) == 0 ||
			           strncasecmp(p, "<div", 4) == 0 || strncasecmp(p, "<li", 3) == 0 ||
			           strncasecmp(p, "<tr", 3) == 0 || strncasecmp(p, "</h", 3) == 0 ||
			           strncasecmp(p, "</p", 3) == 0 || strncasecmp(p, "</div", 5) == 0);
			const char *gt = strchr(p, '>');
			p = gt ? gt + 1 : p + strlen(p);
			if (brk && !space) { hkSbAdd(&out, &len, &cap, "\n", 1); space = 1; }
			continue;
		}
		if (*p == '&') {
			const char *semi = strchr(p, ';');
			if (semi && semi - p <= 10) {
				hkHtmlEntity(p + 1, (size_t)(semi - p - 1), &out, &len, &cap);
				p = semi + 1;
				space = 0;
				continue;
			}
		}
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			if (!space) { hkSbAdd(&out, &len, &cap, " ", 1); space = 1; }
			p++;
			continue;
		}
		hkSbAdd(&out, &len, &cap, p, 1);
		space = 0;
		p++;
		if (len >= HK_WEB_BYTES) break;
	}
	if (!out) return strdup("");
	return out;
}

static char *hkToolWebFetch(const char *input_json) {
	char *url = hkJsonFirst(input_json, "url", "uri", "link", NULL);
	if (!url || !*url) { free(url); return strdup("error: missing url"); }
	if (!hkUrlSafe(url)) {
		free(url);
		return strdup("error: url must be a plain http:// or https:// address with no quotes, spaces or shell characters");
	}

	hkHttpReq req;
	memset(&req, 0, sizeof(req));
	req.method    = "GET";
	req.url       = url;
	req.headers   = "-H 'Accept: text/html,text/plain,application/json;q=0.9' ";
	req.timeout_s = 20;
	req.follow    = 1;

	char *body = hk_http_fetch(&req);
	free(url);
	if (!body || !*body) {
		free(body);
		return strdup("error: fetch returned nothing (host unreachable, blocked, or — in the browser build — refused by CORS)");
	}

	/* JSON and plain text are already readable; only markup needs stripping. */
	const char *s = body;
	while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
	char *text;
	if (*s == '{' || *s == '[') {
		text = strdup(body);
	} else {
		text = hkHtmlToText(body);
	}
	free(body);
	if (!text) return strdup("error: out of memory");
	if (strlen(text) > HK_WEB_BYTES) {
		text[HK_WEB_BYTES] = '\0';
		char *capped = malloc(HK_WEB_BYTES + 64);
		if (capped) {
			snprintf(capped, HK_WEB_BYTES + 64, "%s\n(truncated at %d bytes)\n", text, HK_WEB_BYTES);
			free(text);
			return capped;
		}
	}
	return text;
}

/* Every file tool begins the same way: demand trust, take whatever spelling of… */
static char *hkToolPath(const char *input_json, const char *dflt, char *full, size_t cap) {
	if (!hkProjectTrusted()) return strdup(HK_ERR_TRUST);
	char *path = hkJsonFirst(input_json, "path", "file_path", "filename", "filepath",
	                         "file", "directory", "dir", NULL);
	if (!path && dflt) path = strdup(dflt);
	if (!path) return strdup("error: missing path");
	int rc = hkResolveInProject(path, full, cap);
	free(path);
	return rc == 0 ? NULL : strdup(HK_ERR_OUTSIDE);
}

/* ---------- memory ---------- */

#define HK_MEM_INLINE   6000   /* bytes of memory text carried in the prompt */
#define HK_MEM_MAX_FILE 4000
#define HK_MEM_MAX      64

/* Turn a title into a safe file name: lowercase, alnum and dashes only. */
static void hkMemSlug(const char *in, char *out, size_t cap) {
	size_t o = 0;
	int prev_dash = 1;
	for (const char *p = in; *p && o + 1 < cap; p++) {
		unsigned char c = (unsigned char)*p;
		if (isalnum(c)) { out[o++] = (char)tolower(c); prev_dash = 0; }
		else if (!prev_dash) { out[o++] = '-'; prev_dash = 1; }
	}
	while (o && out[o-1] == '-') o--;
	out[o] = '\0';
	if (!o) snprintf(out, cap, "note");
}

static int hkMemDir(char *out, size_t cap, int global) {
	if (global) {
		char dir[512];
		hkClawDirPath(dir, sizeof(dir));
		if (!dir[0]) return -1;
		snprintf(out, cap, "%s/memory", dir);
	} else {
		snprintf(out, cap, ".hako/memory");
	}
	return 0;
}

/* Every memory, project first, newest first within each root. Caller frees. */
static char *hkMemCollect(int *out_count) {
	char  *buf = NULL;
	size_t len = 0, cap = 0;
	int    count = 0;
	for (int g = 0; g < 2; g++) {
		char dir[640];
		if (hkMemDir(dir, sizeof(dir), g) != 0) continue;
		DIR *d = opendir(dir);
		if (!d) continue;
		struct dirent *e;
		while ((e = readdir(d)) && count < HK_MEM_MAX) {
			if (e->d_name[0] == '.') continue;
			size_t nl = strlen(e->d_name);
			if (nl < 4 || strcmp(e->d_name + nl - 3, ".md") != 0) continue;
			char path[PATH_MAX];
			if (!hkFmtFits(path, sizeof(path), "%s/%s", dir, e->d_name)) continue;
			char *body = hkReadFileAll(path, HK_MEM_MAX_FILE);
			if (!body) continue;
			if (len < HK_MEM_INLINE) {
				char head[256];
				int k = snprintf(head, sizeof(head), "\n- (%s) ", g ? "global" : "project");
				hkSbAdd(&buf, &len, &cap, head, (size_t)k);
				/* Trim trailing whitespace so the list stays one item per line. */
				size_t bl = strlen(body);
				while (bl && (body[bl-1] == '\n' || body[bl-1] == ' ')) body[--bl] = '\0';
				hkSbAdd(&buf, &len, &cap, body, bl);
			}
			free(body);
			count++;
		}
		closedir(d);
	}
	if (out_count) *out_count = count;
	return buf;
}

/* ---------- the task list ---------- */


/* Normalise to one `[ ] ` / `[x] ` line per item, dropping numbering and
   bullets the model may have added. Returns a malloc'd block, or NULL. */
static char *hkTodoNormalize(const char *raw, int *out_total, int *out_done) {
	char  *out = NULL;
	size_t len = 0, cap = 0;
	int total = 0, done = 0;
	for (const char *p = raw; *p; ) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p) : strlen(p);
		char line[512];
		if (n >= sizeof(line)) n = sizeof(line) - 1;
		memcpy(line, p, n);
		line[n] = '\0';

		char *t = line;
		while (*t == ' ' || *t == '\t' || *t == '\r') t++;
		/* strip "1." / "1)" / "-" / "*" */
		while (*t >= '0' && *t <= '9') t++;
		if (*t == '.' || *t == ')') t++;
		if (*t == '-' || *t == '*') t++;
		while (*t == ' ') t++;

		int is_done = 0;
		if (t[0] == '[' && t[1] && t[2] == ']') {
			is_done = (t[1] == 'x' || t[1] == 'X');
			t += 3;
			while (*t == ' ') t++;
		}
		size_t tl = strlen(t);
		while (tl && (t[tl-1] == ' ' || t[tl-1] == '\r')) t[--tl] = '\0';
		if (tl) {
			if (len + tl + 5 < HK_TODO_MAX) {
				hkSbAdd(&out, &len, &cap, is_done ? "[x] " : "[ ] ", 4);
				hkSbAdd(&out, &len, &cap, t, tl);
				hkSbAdd(&out, &len, &cap, "\n", 1);
				total++;
				if (is_done) done++;
			}
		}
		if (!nl) break;
		p = nl + 1;
	}
	if (out_total) *out_total = total;
	if (out_done)  *out_done = done;
	return out;
}

static void hkTodoRender(aiData *data) {
	if (!hk_todo || !*hk_todo) { aiAddHistory(data, "no task list"); return; }
	for (const char *p = hk_todo; *p; ) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p) : strlen(p);
		char row[560];
		if (n >= sizeof(row)) n = sizeof(row) - 1;
		memcpy(row, p, n);
		row[n] = '\0';
		int done = (row[1] == 'x');
		char line[600];
		snprintf(line, sizeof(line), "  %s %s", done ? "\xE2\x9C\x93" : "\xE2\x97\x8B", row + 4);
		aiAddHistory(data, line);
		if (!nl) break;
		p = nl + 1;
	}
}

/* ---------- ask_user ---------- */

#define HK_ASK_MAX 12

static char *hkToolAskUser(const char *input_json) {
	char *q_raw = hkJsonFirst(input_json, "question", "prompt", "text", "q", NULL);
	char *o_raw = hkJsonFirst(input_json, "options", "choices", "answers", NULL);
	if (!q_raw && !o_raw) { free(q_raw); free(o_raw); return strdup("error: missing question"); }

	char *question = q_raw ? hkJsonUnescape(q_raw, (int)strlen(q_raw)) : strdup("Which?");
	char *optblock = o_raw ? hkJsonUnescape(o_raw, (int)strlen(o_raw)) : NULL;
	free(q_raw); free(o_raw);
	if (!question) { free(optblock); return strdup("error: bad question escapes"); }

	/* Split, reusing the todo line conventions so `1. x`, `- x` and `[ ] x`
	   all arrive as plain option text. */
	char *opts[HK_ASK_MAX];
	int n = 0;
	if (optblock) {
		for (char *p = optblock; *p && n < HK_ASK_MAX; ) {
			char *nl = strchr(p, '\n');
			if (nl) *nl = '\0';
			char *t = p;
			while (*t == ' ' || *t == '\t' || *t == '\r') t++;
			while (*t >= '0' && *t <= '9') t++;
			if (*t == '.' || *t == ')') t++;
			if (*t == '-' || *t == '*') t++;
			while (*t == ' ') t++;
			if (t[0] == '[' && t[1] && t[2] == ']') { t += 3; while (*t == ' ') t++; }
			size_t tl = strlen(t);
			while (tl && (t[tl-1] == ' ' || t[tl-1] == '\r')) t[--tl] = '\0';
			if (tl) opts[n++] = t;
			if (!nl) break;
			p = nl + 1;
		}
	}
	if (n < 2) {
		free(question); free(optblock);
		return strdup("error: give at least two options, one per line in 'options'");
	}

	int pick = -1;
	int answered = 1;      /* did a human actually choose? */

#ifdef HAKO_WASM
	/* Same reasoning as the approval gate: a wasm turn cannot yield to ask. */
	pick = 0;
	answered = 0;
#else
	if (E.pipe_mode) {
		hkEmitChoiceRequest(question, (const char **)opts, n);
		char ans[64] = {0};
		if (E.serve_mode) {
			hkServeAnswer(ans, sizeof(ans));
		} else {
			char line[4096];
			while (fgets(line, sizeof(line), stdin)) {
				char *ty = hkExtractJsonString(line, "type");
				if (!ty) continue;
				int is_choice = !strcmp(ty, "choice") || !strcmp(ty, "answer");
				free(ty);
				if (!is_choice) continue;
				char *a = hkExtractJsonString(line, "answer");
				if (!a) a = hkExtractJsonString(line, "text");
				if (a) { snprintf(ans, sizeof(ans), "%.63s", a); free(a); }
				break;
			}
		}
		pick = hkChoiceIndex(ans, (const char **)opts, n);
	} else if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
		clStopAnim(&G_AI);
		if (E.color_enabled) printf("\n  %s?%s %s%s%s\n", TH_ACCENT, ANSI_RESET, TH_AI, question, ANSI_RESET);
		else                printf("\n  ? %s\n", question);
		pick = clPopupSelect(question, (const char **)opts, n, 0, NULL);
	} else {
		/* Non-interactive: show the question so it lands in the log, then take
		   the first option rather than hanging a scripted run forever. */
		fprintf(stderr, "hako: %s\n", question);
		for (int i = 0; i < n; i++) fprintf(stderr, "  %d) %s\n", i + 1, opts[i]);
		pick = 0;
		answered = 0;
	}
#endif

	if (pick < 0 || pick >= n) {
		free(question); free(optblock);
		return strdup("the user dismissed the question without choosing. Ask again only if you "
		              "cannot proceed; otherwise pick a sensible default and say which you chose.");
	}
	size_t olen = strlen(opts[pick]) + 200;
	char *out = malloc(olen);
	if (out) {
		if (answered)
			snprintf(out, olen, "the user chose: %s", opts[pick]);
		else
			snprintf(out, olen,
				"no terminal was available to ask, so this defaulted to the first option: %s. "
				"Say you assumed it; do not report it as the user's decision.", opts[pick]);
	}
	free(question); free(optblock);
	return out ? out : strdup("error: out of memory");
}

typedef struct hkToolDef {
	const char *name;
	const char *description;
	const char *props;
	const char *required;
} hkToolDef;

static const hkToolDef HK_TOOLS[] = {
	{"read_file",
	 "Read contents of a file inside the project directory.",
	 "\"path\":{\"type\":\"string\"}",
	 "\"path\""},
	{"list_dir",
	 "List entries in a directory inside the project.",
	 "\"path\":{\"type\":\"string\"}",
	 "\"path\""},
	{"write_file",
	 "Create or overwrite a WHOLE file. For small changes to an existing file prefer edit_file.",
	 "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}",
	 "\"path\",\"content\""},
	{"edit_file",
	 "Change part of an existing file: replace the exact unique snippet 'old' with 'new'. Cheaper and safer than rewriting the whole file. 'old' must match the file exactly (whitespace-tolerant) and be unique.",
	 "\"path\":{\"type\":\"string\"},\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"}",
	 "\"path\",\"old\",\"new\""},
	{"edit_lines",
	 "Replace an inclusive 1-indexed line range [start,end] of a file with 'new' text. Use when you know exact line numbers (e.g. from a traceback). Omit 'end' to replace a single line.",
	 "\"path\":{\"type\":\"string\"},\"start\":{\"type\":\"integer\"},\"end\":{\"type\":\"integer\"},\"new\":{\"type\":\"string\"}",
	 "\"path\",\"start\",\"new\""},
#ifndef HAKO_WASM
	/* Offered only where a process can be spawned. In the browser the model
	   would call it every turn and get an error back. */
	{"run_shell",
	 "Run a non-interactive shell command. Requires project trust. Times out after 120s by default; pass 'timeout' in seconds (max 600) for a build or test suite that needs longer.",
	 "\"cmd\":{\"type\":\"string\"},\"timeout\":{\"type\":\"integer\"}",
	 "\"cmd\""},
#endif
	{"grep",
	 "Search file CONTENTS across the project. Use this to find where something is defined or used before reading files. 'pattern' is literal text unless it contains * or ?, which then match as wildcards. Optional 'glob' limits which files are searched (e.g. \"*.c\"), 'path' limits the directory, 'ignore_case' 1 for case-insensitive. Returns file:line: text.",
	 "\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"glob\":{\"type\":\"string\"},\"ignore_case\":{\"type\":\"integer\"}",
	 "\"pattern\""},
	{"glob",
	 "Find files by NAME pattern. '*' and '?' match within one path segment, '**' crosses directories. A pattern with no '/' matches the file name alone, so \"*.c\" finds every C file. Optional 'path' limits the directory.",
	 "\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}",
	 "\"pattern\""},
	{"ask_user",
	 "Ask the user to choose between options when you are genuinely blocked on a decision only they can make — an ambiguous requirement, or two approaches with different consequences. 'options' is plain text, one option per line, two or more. Do NOT use it for things you can decide yourself, or to ask permission for a tool call (that already has its own prompt). Returns which option they chose.",
	 "\"question\":{\"type\":\"string\"},\"options\":{\"type\":\"string\"}",
	 "\"question\",\"options\""},
	{"remember",
	 "Save a durable fact so it is still known in later sessions: a decision and why, a convention this project follows, something the user told you about how they work, a trap you hit and how it was solved. One fact per call, written so it makes sense months from now. Do NOT save what the code or git already says, or anything that only matters in this conversation. Pass 'text' and a short 'title'. Set 'global' to 1 for something true everywhere, otherwise it is remembered for this project only.",
	 "\"text\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"global\":{\"type\":\"integer\"}",
	 "\"text\""},
	{"todo",
	 "Record or update your plan for a multi-step task, so it survives even if the conversation is compacted. Pass 'items' as plain text, one step per line, each starting with [ ] for pending or [x] for done. Send the WHOLE list every time — it replaces the previous one. Update it as you finish steps. Use it for any task with more than about three steps.",
	 "\"items\":{\"type\":\"string\"}",
	 "\"items\""},
	{"web_fetch",
	 "Fetch an http(s) URL and return it as readable text (markup stripped, JSON left alone). Use for documentation, changelogs and issues. In the browser build a cross-origin host may refuse the request; if it does, say so rather than retrying.",
	 "\"url\":{\"type\":\"string\"}",
	 "\"url\""},
	{"read_skill",
	 "Read a file from inside an installed skill. Use the <skill> blocks plus their <files> manifest in the system prompt to know what is available. Pass skill (the folder name) and path (relative to skill root, no .. or absolute paths). No trust gate; skills are user-installed.",
	 "\"skill\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}",
	 "\"skill\",\"path\""},
};
static const int HK_TOOL_COUNT = sizeof(HK_TOOLS) / sizeof(HK_TOOLS[0]);

static char *hkBuildToolsSchema(int provider_format) {
	size_t cap = 4096, len = 0;
	char *out = malloc(cap);
	out[len++] = '[';
	for (int i = 0; i < HK_TOOL_COUNT; i++) {
		const hkToolDef *t = &HK_TOOLS[i];
		if (i > 0) { if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); } out[len++] = ','; }
		size_t dlen = strlen(t->description);
		size_t desc_cap = dlen * 6 + 8;
		char *desc_esc = malloc(desc_cap);
		hkJsonEscapeInto(t->description, desc_esc, (int)desc_cap);
		size_t need = len + strlen(t->name) + strlen(desc_esc) + strlen(t->props) + strlen(t->required) + 256;
		if (need >= cap) { while (cap < need) cap *= 2; out = realloc(out, cap); }
		const char *req_close_end = (*t->required) ? "]" : "";
		if (provider_format == 0) {
			len += snprintf(out + len, cap - len,
				"{\"name\":\"%s\",\"description\":\"%s\",\"input_schema\":{\"type\":\"object\",\"properties\":{%s}%s%s%s}}",
				t->name, desc_esc, t->props,
				(*t->required) ? ",\"required\":[" : "",
				t->required, req_close_end);
		} else {
			len += snprintf(out + len, cap - len,
				"{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":\"%s\",\"parameters\":{\"type\":\"object\",\"properties\":{%s}%s%s%s}}}",
				t->name, desc_esc, t->props,
				(*t->required) ? ",\"required\":[" : "",
				t->required, req_close_end);
		}
		free(desc_esc);
	}
	for (int i = 0; i < g_mcp_tool_count; i++) {
		mcpTool *m = &g_mcp_tools[i];
		size_t dlen = strlen(m->description);
		char *desc_esc = malloc(dlen * 6 + 8);
		if (!desc_esc) continue;
		hkJsonEscapeInto(m->description, desc_esc, (int)(dlen * 6 + 8));
		size_t need = len + strlen(m->qualified) + strlen(desc_esc) + strlen(m->schema) + 128;
		if (need >= cap) { while (cap < need) cap *= 2; out = realloc(out, cap); }
		if (provider_format == 0)
			len += snprintf(out + len, cap - len,
				",{\"name\":\"%s\",\"description\":\"%s\",\"input_schema\":%s}",
				m->qualified, desc_esc, m->schema);
		else
			len += snprintf(out + len, cap - len,
				",{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":\"%s\",\"parameters\":%s}}",
				m->qualified, desc_esc, m->schema);
		free(desc_esc);
	}
	if (len + 2 >= cap) { cap += 4; out = realloc(out, cap); }
	out[len++] = ']';
	out[len] = '\0';
	return out;
}

typedef enum { ALLOW_READ, ALLOW_WRITE, ALLOW_SHELL, ALLOW_NET } hkAllowKind;
typedef struct { hkAllowKind kind; char *scope; } hkAllowRule;
static hkAllowRule *g_allow = NULL;
static int g_allow_count = 0;

static int hkAllowHas(hkAllowKind kind, const char *scope) {
	for (int i = 0; i < g_allow_count; i++) {
		if (g_allow[i].kind != kind) continue;
		if (kind == ALLOW_SHELL) {
			if (g_allow[i].scope && scope && strcmp(g_allow[i].scope, scope) == 0) return 1;
		} else {
			return 1;
		}
	}
	return 0;
}

static void hkAllowAdd(hkAllowKind kind, const char *scope) {
	if (hkAllowHas(kind, scope)) return;
	hkAllowRule *n = realloc(g_allow, (g_allow_count + 1) * sizeof(*g_allow));
	if (!n) return;
	g_allow = n;
	g_allow[g_allow_count].kind  = kind;
	g_allow[g_allow_count].scope = scope ? strdup(scope) : NULL;
	g_allow_count++;
}

/* "read+write", "shell" ...: what the user has said always to, for the bar. */
static void hkAllowSummary(char *out, size_t cap) {
	static const char *nm[] = { "read", "write", "shell", "web" };
	int have[4] = {0};
	for (int i = 0; i < g_allow_count; i++)
		if ((int)g_allow[i].kind >= 0 && (int)g_allow[i].kind < 4) have[g_allow[i].kind] = 1;
	out[0] = '\0';
	for (int k = 0; k < 4; k++)
		if (have[k]) snprintf(out + strlen(out), cap - strlen(out), "%s%s", *out ? "+" : "", nm[k]);
}

/* ---------- diff preview ---------- */

#define HK_DIFF_LINES 36
#define HK_DIFF_COLS  160

typedef struct { const char *p; size_t n; } hkLineRef;

static hkLineRef *hkDiffSplit(const char *s, int *out_count) {
	int cap = 64, n = 0;
	hkLineRef *v = malloc((size_t)cap * sizeof(*v));
	*out_count = 0;
	if (!v) return NULL;
	for (const char *p = s; *p; ) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (n == cap) {
			hkLineRef *b = realloc(v, (size_t)cap * 2 * sizeof(*v));
			if (!b) break;
			v = b; cap *= 2;
		}
		v[n].p = p; v[n].n = len; n++;
		if (!nl) break;
		p = nl + 1;
	}
	*out_count = n;
	return v;
}

static void hkDiffLine(char **b, size_t *l, size_t *c, int color, char sign,
                       const char *s, size_t n) {
	const char *col = !color ? "" : sign == '+' ? TH_OK : sign == '-' ? TH_ERR : TH_META;
	char head[64];
	snprintf(head, sizeof(head), "    %s%c ", col, sign);
	hkSbAdd(b, l, c, head, strlen(head));
	if (n > HK_DIFF_COLS) n = HK_DIFF_COLS;
	hkSbAdd(b, l, c, s, n);
	if (color) hkSbAdd(b, l, c, ANSI_RESET, strlen(ANSI_RESET));
	hkSbAdd(b, l, c, "\n", 1);
}

static void hkDiffNote(char **b, size_t *l, size_t *c, int color, const char *fmt, int v) {
	char note[64];
	int k = snprintf(note, sizeof(note), fmt, v, v == 1 ? "" : "s");
	hkDiffLine(b, l, c, color, ' ', note, (size_t)(k > 0 ? k : 0));
}

/* Common head and tail lines are folded away, so a whole-file rewrite shows the
   part that actually differs instead of the entire file. */
static void hkDiffBlock(char **b, size_t *l, size_t *c, int color,
                        const char *olds, const char *news) {
	int on = 0, nn = 0;
	hkLineRef *o = hkDiffSplit(olds ? olds : "", &on);
	hkLineRef *n = hkDiffSplit(news ? news : "", &nn);
	if (!o || !n) { free(o); free(n); return; }

	int head = 0;
	while (head < on && head < nn && o[head].n == n[head].n &&
	       memcmp(o[head].p, n[head].p, o[head].n) == 0) head++;
	int tail = 0;
	while (tail < on - head && tail < nn - head &&
	       o[on - 1 - tail].n == n[nn - 1 - tail].n &&
	       memcmp(o[on - 1 - tail].p, n[nn - 1 - tail].p, o[on - 1 - tail].n) == 0) tail++;

	int rem_o = on - head - tail, rem_n = nn - head - tail;
	if (!rem_o && !rem_n) {
		hkDiffLine(b, l, c, color, ' ', "(identical content)", 19);
		free(o); free(n);
		return;
	}
	if (head) hkDiffNote(b, l, c, color, "... %d unchanged line%s above", head);

	int lim = HK_DIFF_LINES / 2;
	for (int i = 0; i < rem_o && i < lim; i++)
		hkDiffLine(b, l, c, color, '-', o[head + i].p, o[head + i].n);
	if (rem_o > lim) hkDiffNote(b, l, c, color, "... %d more removed line%s", rem_o - lim);
	for (int i = 0; i < rem_n && i < lim; i++)
		hkDiffLine(b, l, c, color, '+', n[head + i].p, n[head + i].n);
	if (rem_n > lim) hkDiffNote(b, l, c, color, "... %d more added line%s", rem_n - lim);
	if (tail) hkDiffNote(b, l, c, color, "... %d unchanged line%s below", tail);

	free(o); free(n);
}

/* NULL when there is nothing worth showing (not a write, or the file cannot be
   read). Caller frees. */
static char *hkDiffPreview(const char *name, const char *input_json, int color) {
	char  *b = NULL;
	size_t l = 0, cap = 0;

	if (strcmp(name, "edit_file") == 0) {
		char *o_raw = hkJsonFirst(input_json, "old", "old_str", NULL);
		char *n_raw = hkJsonFirst(input_json, "new", "new_str", NULL);
		if (!o_raw || !n_raw) { free(o_raw); free(n_raw); return NULL; }
		char *o = hkJsonUnescape(o_raw, (int)strlen(o_raw));
		char *n = hkJsonUnescape(n_raw, (int)strlen(n_raw));
		free(o_raw); free(n_raw);
		if (o && n) hkDiffBlock(&b, &l, &cap, color, o, n);
		free(o); free(n);
		return b;
	}

	char *path = hkJsonFirst(input_json, "path", "file_path", "filename", "filepath", "file", NULL);
	if (!path) return NULL;
	char full[PATH_MAX];
	int resolved = hkResolveInProject(path, full, sizeof(full));
	free(path);
	if (resolved != 0) return NULL;
	long olen = 0;
	char *old = resolved == 0 ? hk_fs_read(full, 400000, &olen) : NULL;

	if (strcmp(name, "write_file") == 0) {
		char *c_raw = hkJsonFirst(input_json, "content", "contents", "file_text", "new_str", "text", NULL);
		if (!c_raw) { free(old); return NULL; }
		char *content = hkJsonUnescape(c_raw, (int)strlen(c_raw));
		free(c_raw);
		if (!content) { free(old); return NULL; }
		if (!old) {
			int nn = 0;
			hkLineRef *n = hkDiffSplit(content, &nn);
			if (n) {
				int lim = HK_DIFF_LINES;
				for (int i = 0; i < nn && i < lim; i++)
					hkDiffLine(&b, &l, &cap, color, '+', n[i].p, n[i].n);
				if (nn > lim) hkDiffNote(&b, &l, &cap, color, "... %d more line%s", nn - lim);
				free(n);
			}
		} else {
			hkDiffBlock(&b, &l, &cap, color, old, content);
		}
		free(content);
		free(old);
		return b;
	}

	if (strcmp(name, "edit_lines") == 0) {
		if (!old) return NULL;
		int start = hkExtractJsonInt(input_json, "start");
		int end   = hkExtractJsonInt(input_json, "end");
		if (end < start) end = start;
		char *n_raw = hkJsonFirst(input_json, "new", "new_str", NULL);
		char *n = n_raw ? hkJsonUnescape(n_raw, (int)strlen(n_raw)) : strdup("");
		free(n_raw);
		int on = 0;
		hkLineRef *o = hkDiffSplit(old, &on);
		if (o && n && start >= 1) {
			char *slice = NULL;
			size_t sl = 0, sc = 0;
			for (int i = start - 1; i < end && i < on; i++) {
				hkSbAdd(&slice, &sl, &sc, o[i].p, o[i].n);
				hkSbAdd(&slice, &sl, &sc, "\n", 1);
			}
			hkDiffBlock(&b, &l, &cap, color, slice ? slice : "", n);
			free(slice);
		}
		free(o); free(n); free(old);
		return b;
	}

	free(old);
	return NULL;
}


static int hkToolApprovalInner(const char *name, const char *input_json);


static int hkToolApproval(const char *name, const char *input_json) {
	if ((E.ai_auto_approve && !hs_remote) || strcmp(name, "read_skill") == 0 || strcmp(name, "ask_user") == 0)
		return hkToolApprovalInner(name, input_json);
	hkClaimStdin();
	int r = hkToolApprovalInner(name, input_json);
	hkReleaseStdin();
	return r;
}

/* The approval as JSON for whoever is not at this terminal: the Rei pane, the
   studio, or a phone attached with :remote on. */
static int hkEmitToolRequest(const char *name, const char *subject, hkAllowKind kind, const char *input_json) {
	static long ask_seq = 0;
	char en[128], es[1024];
	clPipeEscape(name ? name : "", en, sizeof(en));
	clPipeEscape(subject ? subject : (kind == ALLOW_SHELL ? "(command)" : kind == ALLOW_NET ? "(url)" : "."), es, sizeof(es));
	/* An extra field: a front end that does not know it drops it, which is
	   what hake's reader already does with anything unrecognised. */
	char *dp = (kind == ALLOW_WRITE) ? hkDiffPreview(name, input_json, 0) : NULL;
	char *ed = NULL;
	if (dp) {
		size_t dn = strlen(dp);
		if (dn > 4000) { dp[4000] = '\0'; dn = 4000; }
		ed = malloc(dn * 6 + 8);
		if (ed) clPipeEscape(dp, ed, (int)(dn * 6 + 8));
		free(dp);
	}
	size_t rcap = 1400 + (ed ? strlen(ed) : 0);
	char *req = malloc(rcap);
	if (!req) { free(ed); return 0; }
	snprintf(req, rcap,
		"{\"type\":\"tool_request\",\"id\":%ld,\"name\":\"%.127s\",\"subject\":\"%.1023s\",\"kind\":\"%s\"%s%s%s}",
		++ask_seq, en, es,
		kind == ALLOW_WRITE ? "write" : kind == ALLOW_SHELL ? "shell" :
		kind == ALLOW_NET   ? "net"   : "read",
		ed ? ",\"diff\":\"" : "", ed ? ed : "", ed ? "\"" : "");
	clPipeEmitRaw(req);
	free(req);
	free(ed);
	return 1;
}

#if HK_ATTACH
static int hsAttachSettle(int pick);
static int hsAttachSettleChoice(int pick, const char **items, int n);
static int hsAttachOn(void);
static void hsAttachOff(void);
static int hkInjectTake(char *out, size_t cap);
static void hsAttachInfo(int *port, const char **token);
static void hsAttachSetPort(int port);
static int hrStart(void);
static int hrLinked(void);
static void hrPairPrompt(aiData *data);
static const char *hrStudioUrl(void);
static void hrRenamed(void);
static volatile int hk_line_wait;
#endif

static int hkToolApprovalInner(const char *name, const char *input_json) {
	if (E.ai_auto_approve && !hs_remote) return 1;
	if (strcmp(name, "read_skill") == 0) return 1;
	/* Asking the user a question IS the interaction — gating it behind a
	   second prompt would ask permission to ask permission. */
	if (strcmp(name, "ask_user") == 0) return 1;
#ifdef HAKO_WASM
	(void)input_json;
	return 1;
#else

	hkAllowKind kind;
	if      (strcmp(name, "write_file") == 0) kind = ALLOW_WRITE;
	else if (strcmp(name, "edit_file")  == 0) kind = ALLOW_WRITE;
	else if (strcmp(name, "edit_lines") == 0) kind = ALLOW_WRITE;
	else if (strcmp(name, "run_shell")  == 0) kind = ALLOW_SHELL;
	else if (strcmp(name, "web_fetch")  == 0) kind = ALLOW_NET;
	else                                      kind = ALLOW_READ;

	char *subject = NULL;
	if (kind == ALLOW_SHELL) {
		subject = hkJsonFirst(input_json, "cmd", "command", "shell", "script", NULL);
	} else if (kind == ALLOW_NET) {
		subject = hkJsonFirst(input_json, "url", "uri", NULL);
	} else {
		subject = hkJsonFirst(input_json, "path", "file_path", "filename", "directory", "dir", NULL);
	}

	if (!hs_remote && hkAllowHas(kind, kind == ALLOW_SHELL ? subject : NULL)) {
		/* Said once per kind: a tool that runs with no prompt in ask mode
		   otherwise reads as the gate being broken. */
		static int said[4];
		if ((int)kind >= 0 && (int)kind < 4 && !said[kind] && !E.pipe_mode && !E.serve_mode) {
			said[kind] = 1;
			aiAddHistory(&G_AI, kind == ALLOW_WRITE ? "allowed without asking \xC2\xB7 you chose always write in this project" :
			                    kind == ALLOW_SHELL ? "allowed without asking \xC2\xB7 you chose always run this command" :
			                    kind == ALLOW_NET   ? "allowed without asking \xC2\xB7 you chose always fetch this session" :
			                                          "allowed without asking \xC2\xB7 you chose always read in this project");
		}
		free(subject);
		return 1;
	}

	if (E.pipe_mode) {
		if (!hkEmitToolRequest(name, subject, kind, input_json)) { free(subject); return 0; }

		char ans[8] = {0};
		if (E.serve_mode) {
			hkServeAnswer(ans, sizeof(ans));
		} else {
			char pbuf[4096];
			while (fgets(pbuf, sizeof(pbuf), stdin)) {
				char *ty = hkExtractJsonString(pbuf, "type");
				if (!ty) continue;
				int is_approve = (strcmp(ty, "approve") == 0);
				free(ty);
				if (!is_approve) continue;
				char *a = hkExtractJsonString(pbuf, "answer");
				if (a) { snprintf(ans, sizeof(ans), "%.7s", a); free(a); }
				break;
			}
		}
		int decision = (ans[0] == 'y' || ans[0] == 'Y') ? 1 : (ans[0] == 'a' || ans[0] == 'A') ? 2 : 0;
		if (hs_remote) {
			char lg[256];
			snprintf(lg, sizeof(lg), "approval (remote): %s %.160s \xE2\x86\x92 %s", name, subject ? subject : "",
			         decision ? "once" : "denied");
			hkLogMessage("system", lg);
		}
		if (decision == 2) { if (!hs_remote) hkAllowAdd(kind, kind == ALLOW_SHELL ? subject : NULL); decision = 1; }
		free(subject);
		return decision;
	}

	if (!isatty(STDIN_FILENO)) { free(subject); return 0; }

	clStopAnim(&G_AI);

	const char *always_label =
		(kind == ALLOW_WRITE) ? "always write to this project this session" :
		(kind == ALLOW_SHELL) ? "always run exactly this command this session" :
		(kind == ALLOW_NET)   ? "always fetch from the web this session" :
		                        "always read from this project this session";

	const char *disp = subject ? subject : (kind == ALLOW_SHELL ? "(command)" : kind == ALLOW_NET ? "(url)" : ".");
	/* Bracketed like every other print: bare, it landed on the bar's rows when the
	   bar was up (the second approval of a turn), and the bar's own erase took the
	   header and left the bar's top rule stranded above the popup. */
	clOutBegin();
	if (E.color_enabled)
		printf("\n  %s\xE2\x97\x8F %s%s  %s%s%s\n", TH_TOOL, name, ANSI_RESET,
		       TH_META, disp, ANSI_RESET);
	else
		printf("\n  * %s  %s\n", name, disp);
	if (kind == ALLOW_WRITE) {
		char *dp = hkDiffPreview(name, input_json, E.color_enabled);
		if (dp) { printf("%s", dp); free(dp); }
	}
	clOutEnd(&G_AI);
	int decision = 0;
	if (isatty(STDOUT_FILENO)) {
		const char *choices[3] = { "allow once", "no, skip it", always_label };
#if HK_ATTACH
		if (hs_attached && hkEmitToolRequest(name, subject, kind, input_json)) hs_answer_armed = 1;
#endif
		int pick = clPopupSelect("allow?", choices, hs_remote ? 2 : 3, 0, NULL);
#if HK_ATTACH
		if (hs_answer_armed) { hs_answer_armed = 0; pick = hsAttachSettle(pick); }
#endif
		if      (pick == 0) decision = 1;
		else if (pick == 2) decision = 2;
	} else {
		printf("  Allow?  [y] once   [n] no   [a] %s\n  > ", always_label);
		fflush(stdout);
		char buf[64];
		if (fgets(buf, sizeof(buf), stdin)) {
			if      (buf[0] == 'y' || buf[0] == 'Y') decision = 1;
			else if (buf[0] == 'a' || buf[0] == 'A') decision = 2;
		}
	}
	{
		char lg[256];
		snprintf(lg, sizeof(lg), "approval%s: %s %.160s \xE2\x86\x92 %s", hs_remote ? " (remote on)" : "",
		         name, subject ? subject : "", decision == 2 ? "always" : decision ? "once" : "denied");
		hkLogMessage("system", lg);
	}
	if (decision == 2) { if (!hs_remote) hkAllowAdd(kind, kind == ALLOW_SHELL ? subject : NULL); decision = 1; }
	free(subject);
	return decision;
#endif
}

static char hk_last_write_path[1024] = "";

/* The repeat guard used to remember only the LAST call, so a model bouncing… */
#define HK_READ_MAX 100000L   /* read_file cap; truncation is ANNOUNCED, never silent */
#define HK_REPEAT_RING  8
static char hk_repeat_ring[HK_REPEAT_RING][512];
static int  hk_repeat_head = 0;
static char hk_repeat_sig[512] = "";
static int  hk_repeat_count = 0;
#define HK_REPEAT_LIMIT 3

static void hkRepeatReset(void) {
	for (int i = 0; i < HK_REPEAT_RING; i++) hk_repeat_ring[i][0] = '\0';
	hk_repeat_head = 0;
	hk_repeat_sig[0] = '\0';
	hk_repeat_count = 0;
}

/* how many times this signature now appears in the window, self included… */
static int hkRepeatSeen(const char *sig) {
	int n = 1;
	for (int i = 0; i < HK_REPEAT_RING; i++)
		if (hk_repeat_ring[i][0] && !strcmp(hk_repeat_ring[i], sig)) n++;
	snprintf(hk_repeat_ring[hk_repeat_head], sizeof(hk_repeat_ring[0]), "%s", sig);
	hk_repeat_head = (hk_repeat_head + 1) % HK_REPEAT_RING;
	snprintf(hk_repeat_sig, sizeof(hk_repeat_sig), "%s", sig);
	hk_repeat_count = n;
	return n;
}

static int hkLineEqTrim(const char *a, const char *b) {
	size_t la = strlen(a), lb = strlen(b);
	while (la && (a[la-1] == ' ' || a[la-1] == '\t' || a[la-1] == '\r')) la--;
	while (lb && (b[lb-1] == ' ' || b[lb-1] == '\t' || b[lb-1] == '\r')) lb--;
	return la == lb && strncmp(a, b, la) == 0;
}

static int hkLineEqLoose(const char *a, const char *b) {
	size_t la = strlen(a), lb = strlen(b), sa = 0, sb = 0;
	while (la && (a[la-1] == ' ' || a[la-1] == '\t' || a[la-1] == '\r')) la--;
	while (lb && (b[lb-1] == ' ' || b[lb-1] == '\t' || b[lb-1] == '\r')) lb--;
	while (sa < la && (a[sa] == ' ' || a[sa] == '\t')) sa++;
	while (sb < lb && (b[sb] == ' ' || b[sb] == '\t')) sb++;
	return (la - sa) == (lb - sb) && strncmp(a + sa, b + sb, la - sa) == 0;
}

static size_t hkLeadWs(const char *s) {
	size_t i = 0;
	while (s[i] == ' ' || s[i] == '\t') i++;
	return i;
}

static char **hkSplitLines(const char *s, int *n, char **bufcopy) {
	*n = 0; *bufcopy = NULL;
	char *c = strdup(s);
	if (!c) return NULL;
	int cap = 64, cnt = 0;
	char **lines = malloc((size_t)cap * sizeof(char *));
	if (!lines) { free(c); return NULL; }
	lines[cnt++] = c;
	for (char *p = c; *p; p++) {
		if (*p == '\n') {
			*p = '\0';
			if (*(p+1)) {
				if (cnt == cap) {
					cap *= 2;
					char **t = realloc(lines, (size_t)cap * sizeof(char *));
					if (!t) break;
					lines = t;
				}
				lines[cnt++] = p + 1;
			}
		}
	}
	for (int i = 0; i < cnt; i++) {
		size_t L = strlen(lines[i]);
		if (L && lines[i][L-1] == '\r') lines[i][L-1] = '\0';
	}
	*n = cnt; *bufcopy = c;
	return lines;
}

static char *hkSpliceLines(const char *src, int a, int b, const char *rep, int *removed) {
	if (a < 1 || b < a) return NULL;
	const char *p = src; int line = 1;
	while (line < a && *p) { if (*p == '\n') line++; p++; }
	if (line < a) return NULL;
	const char *start = p;
	int rm = 0;
	while (line <= b && *p) {
		if (*p == '\n') { line++; rm++; p++; if (line > b) break; }
		else p++;
	}
	if (p > start && p[-1] != '\n') rm++;
	const char *tail = p;
	size_t pre = (size_t)(start - src), tl = strlen(tail), rl = strlen(rep);
	int ended_nl = (tail > src && tail[-1] == '\n');
	int need_nl = (rl > 0 && rep[rl-1] != '\n' && (tl > 0 || ended_nl));
	char *out = malloc(pre + rl + (need_nl ? 1u : 0u) + tl + 1);
	if (!out) return NULL;
	size_t o = 0;
	memcpy(out + o, src, pre); o += pre;
	memcpy(out + o, rep, rl);  o += rl;
	if (need_nl) out[o++] = '\n';
	memcpy(out + o, tail, tl); o += tl;
	out[o] = '\0';
	if (removed) *removed = rm;
	return out;
}

static char *hkReindentLike(const char *news, char **flines, int at, int on) {
	int nn = 0; char *nbuf = NULL;
	char **nlines = hkSplitLines(news, &nn, &nbuf);
	if (!nlines) return NULL;
	size_t cap = strlen(news) + (size_t)nn * 8 + 16;
	char *out = malloc(cap);
	if (!out) { free(nlines); free(nbuf); return NULL; }
	size_t o = 0;
	for (int k = 0; k < nn; k++) {
		if (nlines[k][0] && hkLeadWs(nlines[k]) == 0) {
			const char *fl = flines[at + (k < on ? k : on - 1)];
			size_t lw = hkLeadWs(fl);
			if (o + lw + 2 >= cap) { cap = o + lw + 32; char *t = realloc(out, cap); if (!t) break; out = t; }
			memcpy(out + o, fl, lw); o += lw;
		}
		size_t ll = strlen(nlines[k]);
		if (o + ll + 2 >= cap) { cap = o + ll + 16; char *t = realloc(out, cap); if (!t) break; out = t; }
		memcpy(out + o, nlines[k], ll); o += ll;
		if (k + 1 < nn) out[o++] = '\n';
	}
	size_t nl = strlen(news);
	if (nl && news[nl-1] == '\n') out[o++] = '\n';
	out[o] = '\0';
	free(nlines); free(nbuf);
	return out;
}

static char hk_turn_edited[16][1024];
static int  hk_turn_edited_n = 0;

static void hkMarkEdited(const char *rel) {
	for (int i = 0; i < hk_turn_edited_n; i++)
		if (strcmp(hk_turn_edited[i], rel) == 0) return;
	if (hk_turn_edited_n < 16)
		snprintf(hk_turn_edited[hk_turn_edited_n++], sizeof(hk_turn_edited[0]), "%s", rel);
}
static int hkWasEditedThisTurn(const char *rel) {
	for (int i = 0; i < hk_turn_edited_n; i++)
		if (strcmp(hk_turn_edited[i], rel) == 0) return 1;
	return 0;
}
static int hkContentLooseEqFile(const char *full, const char *content) {
	char *cur = hkReadFileAll(full, 4 * 1024 * 1024);
	if (!cur) return 0;
	int an = 0, bn = 0; char *ab = NULL, *bb = NULL;
	char **a = hkSplitLines(cur, &an, &ab);
	char **b = hkSplitLines(content, &bn, &bb);
	int eq = (a && b && an == bn);
	for (int i = 0; eq && i < an; i++) if (!hkLineEqLoose(a[i], b[i])) eq = 0;
	free(a); free(b); free(ab); free(bb); free(cur);
	return eq;
}

static char *hkWriteResolved(const char *full, const char *rel, const char *content) {
	size_t clen = strlen(content);
	int new_lines = 0;
	for (size_t i = 0; i < clen; i++) if (content[i] == '\n') new_lines++;
	if (clen > 0 && content[clen-1] != '\n') new_lines++;
	if (hk_fs_write(full, content, clen, 0) != 0) return strdup("error: cannot open for write");
	size_t wrote = clen;
	snprintf(hk_last_write_path, sizeof(hk_last_write_path), "%s", rel);
	char *out = malloc(256);
	if (out) snprintf(out, 256, "wrote %zu bytes to %.150s (%d lines)", wrote, hkProjectRel(full), new_lines);
	return out ? out : strdup("wrote file");
}

static char *hkExecToolInner(const char *name, const char *input_json);
static long hkContextWindow(void);

/* One tool result may take a quarter of the context window. A small local model
   has 8K tokens; one `grep -r` over a big folder filled it, and every turn after
   that failed with "exceeds context window". Cut it, and say how to see more. */
static char *hkFitToolResult(char *r) {
	if (!r) return r;
	long cap = hkContextWindow();               /* tokens/4 * ~4 chars per token */
	if (cap < 2000) cap = 2000;
	size_t n = strlen(r);
	if ((long)n <= cap) return r;
	size_t keep = (size_t)cap;
	while (keep > 0 && ((unsigned char)r[keep] & 0xC0) == 0x80) keep--;
	char note[200];
	snprintf(note, sizeof(note),
		"\n[... cut: showing %zu of %zu characters so it fits the model's context. "
		"Narrow it down (a path, a glob, a smaller range) to see the rest.]", keep, n);
	char *o = malloc(keep + strlen(note) + 1);
	if (!o) return r;
	memcpy(o, r, keep);
	memcpy(o + keep, note, strlen(note) + 1);
	free(r);
	return o;
}

static char *hkExecTool(const char *name, const char *input_json) {
	hk_cur_tool = name;
	char *r = hkFitToolResult(hkExecToolInner(name, input_json));
	hk_cur_tool = NULL;
	hk_cur_subject[0] = '\0';
	return r;
}

static char *hkExecToolInner(const char *name, const char *input_json) {
	struct { const char *from; const char *to; } aliases[] = {
		{"create_file",  "write_file"},
		{"writefile",    "write_file"},
		{"write_to_file","write_file"},
		{"save_file",    "write_file"},
		{"write",        "write_file"},
		{"insert_edit_into_file", "edit_file"},
		{"edit",         "edit_file"},
		{"editfile",     "edit_file"},
		{"replace",      "edit_file"},
		{"str_replace",  "edit_file"},
		{"str_replace_based_edit_tool", "edit_file"},
		{"str_replace_editor",          "edit_file"},
		{"text_editor",                 "edit_file"},
		{"text_edit",                   "edit_file"},
		{"edit_line",    "edit_lines"},
		{"editlines",    "edit_lines"},
		{"replace_lines","edit_lines"},
		{"readfile",     "read_file"},
		{"read_text_file","read_file"},
		{"read",         "read_file"},
		{"view",         "read_file"},
		{"cat",          "read_file"},
		{"open_file",    "read_file"},
		{"ls",           "list_dir"},
		{"listdir",      "list_dir"},
		{"list_files",   "list_dir"},
		{"list",         "list_dir"},
		{"dir",          "list_dir"},
		{"runshell",     "run_shell"},
		{"bash",         "run_shell"},
		{"shell",        "run_shell"},
		{"sh",           "run_shell"},
		{"exec",         "run_shell"},
		{"run",          "run_shell"},
		{"run_command",  "run_shell"},
		{"execute_command", "run_shell"},
		{"askuser",      "ask_user"},
		{"ask",          "ask_user"},
		{"choose",       "ask_user"},
		{"select_option","ask_user"},
		{"askuserquestion", "ask_user"},
		{"save_memory",  "remember"},
		{"memorize",     "remember"},
		{"todowrite",    "todo"},
		{"todo_write",   "todo"},
		{"update_plan",  "todo"},
		{"task_list",    "todo"},
		{"search",       "grep"},
		{"search_files", "grep"},
		{"grep_search",  "grep"},
		{"ripgrep",      "grep"},
		{"rg",           "grep"},
		{"find_in_files","grep"},
		{"glob_search",  "glob"},
		{"find_files",   "glob"},
		{"find",         "glob"},
		{"fetch",        "web_fetch"},
		{"web_get",      "web_fetch"},
		{"fetch_url",    "web_fetch"},
		{"read_url",     "web_fetch"},
		{NULL, NULL}
	};
	for (int i = 0; aliases[i].from; i++) {
		if (strcasecmp(name, aliases[i].from) == 0) { name = aliases[i].to; break; }
	}
	/* Plan mode: reads are fine, changes are not. Refused here rather than in
	   the approval gate so it holds in --pipe, --serve and --yolo too. */
	if (E.ai_plan_mode) {
		if (!strcmp(name, "write_file") || !strcmp(name, "edit_file") ||
		    !strcmp(name, "edit_lines") || !strcmp(name, "run_shell")) {
			char *d = malloc(220);
			if (d) snprintf(d, 220,
				"refused: plan mode is on, so '%s' cannot run. Describe what you would "
				"change and why; the user will switch to ask or auto mode when ready.", name);
			return d ? d : strdup("refused: plan mode");
		}
	}
	if (hk_bench_mode) {
		if (hk_bench_calls == 0) {
			snprintf(hk_bench_tool, sizeof(hk_bench_tool), "%s", name);
			snprintf(hk_bench_args, sizeof(hk_bench_args), "%s", input_json ? input_json : "");
		}
		hk_bench_calls++;
		return strdup("(benchmark: tool recorded, not executed)");
	}
	{
		char sig[512];
		snprintf(sig, sizeof(sig), "%s|%.400s", name, input_json ? input_json : "");
		if (hkRepeatSeen(sig) > HK_REPEAT_LIMIT)
			return strdup("error: this exact call has already run several times with the same "
			              "result. Do not repeat it. Change the arguments, try a different tool, "
			              "or tell the user what is blocking you.");
	}
	if (!hkToolApproval(name, input_json)) {
		char *d = malloc(160);
		if (d) snprintf(d, 160, "error: user denied tool '%s'. Do not retry it; explain or ask the user how to proceed.", name);
		return d ? d : strdup("error: user denied tool");
	}
	if (strncmp(name, "mcp", 3) == 0) {
		const char *q = hkMcpResolveName(name);
		if (q) return hkMcpCallTool(q, input_json);
	}
	if (strcmp(name, "read_file") == 0) {
		char full[PATH_MAX], *bad = hkToolPath(input_json, NULL, full, sizeof(full));
		if (bad) return bad;
		struct stat rst;
		if (stat(full, &rst) != 0) {
			char *d = malloc(256);
			if (d) snprintf(d, 256, "error: no such file '%.180s' (%s)", full, strerror(errno));
			return d ? d : strdup("error: cannot read");
		}
		if (S_ISDIR(rst.st_mode)) {
			char *d = malloc(320);
			if (d) snprintf(d, 320, "error: '%.180s' is a directory, not a file. "
			                "Use list_dir(path) to see what is in it.", full);
			return d ? d : strdup("error: path is a directory");
		}
		char *c = hkReadFileAll(full, HK_READ_MAX);
		if (!c) return strdup("error: cannot read");
		if (!*c) { free(c); return strdup("(the file exists and is empty — 0 bytes)"); }
		/* Truncation used to be SILENT: the model got the first 100 KB of a
		   large file with nothing to say so, and would then "edit" or rewrite
		   it believing it had seen the whole thing — which destroys the tail.
		   Say what was cut and how to get the rest. */
		if ((long)rst.st_size > HK_READ_MAX) {
			size_t have = strlen(c);
			char note[256];
			/* snprintf returns what it WOULD have written, not what it did:
			   using that as a length reads past the buffer when the text is
			   clipped. Measure the result. */
			snprintf(note, sizeof(note),
				"\n\n… [truncated: this is the FIRST %ld of %ld bytes. Do NOT rewrite the "
				"whole file from this — you have not seen the end. Use edit_file on an exact "
				"snippet, or run_shell with sed -n 'A,Bp' to read another range.]\n",
				(long)have, (long)rst.st_size);
			size_t nl = strlen(note);
			char *w = realloc(c, have + nl + 1);
			if (!w) return c;
			memcpy(w + have, note, nl + 1);
			return w;
		}
		return c;
	}
	if (strcmp(name, "list_dir") == 0) {
		char full[PATH_MAX], *bad = hkToolPath(input_json, ".", full, sizeof(full));
		if (bad) return bad;
		char *c = hkListDir(full);
		return c ? c : strdup("error: cannot list");
	}
	if (strcmp(name, "run_shell") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted");
		char *shcmd = hkJsonFirst(input_json, "cmd", "command", "shell", "script", NULL);
		if (!shcmd) return strdup("error: missing cmd (param must be one of: cmd, command, shell, script)");
		char *decoded = hkJsonUnescape(shcmd, (int)strlen(shcmd));
		free(shcmd);
		if (!decoded) return strdup("error: bad cmd escapes");
		int tmo = hkExtractJsonInt(input_json, "timeout");
		if (tmo <= 0) tmo = hkExtractJsonInt(input_json, "timeout_s");
		if (tmo <= 0) tmo = E.ai_shell_timeout;
		char *c = hkRunShellCapture(decoded, 50000, tmo);
		free(decoded);
		return c;
	}
	if (strcmp(name, "grep") == 0)
		return hkProjectTrusted() ? hkToolGrep(input_json) : strdup(HK_ERR_TRUST);
	if (strcmp(name, "glob") == 0)
		return hkProjectTrusted() ? hkToolGlob(input_json) : strdup(HK_ERR_TRUST);
	if (strcmp(name, "web_fetch") == 0) {
		return hkToolWebFetch(input_json);
	}
	if (strcmp(name, "ask_user") == 0) {
		return hkToolAskUser(input_json);
	}
	if (strcmp(name, "remember") == 0) {
		char *raw = hkJsonFirst(input_json, "text", "fact", "content", "memory", NULL);
		if (!raw) return strdup("error: missing text");
		char *text = hkJsonUnescape(raw, (int)strlen(raw));
		free(raw);
		if (!text || !*text) { free(text); return strdup("error: empty text"); }
		char *title_raw = hkJsonFirst(input_json, "title", "name", "key", NULL);
		char *title = title_raw ? hkJsonUnescape(title_raw, (int)strlen(title_raw)) : NULL;
		free(title_raw);
		int global = hkExtractJsonInt(input_json, "global") > 0;

		char dir[640];
		if (hkMemDir(dir, sizeof(dir), global) != 0) { free(text); free(title); return strdup("error: no memory directory"); }
		if (hk_fs_mkdirp(dir) != 0) { free(text); free(title); return strdup("error: cannot create memory directory"); }
		char slug[128];
		hkMemSlug(title && *title ? title : text, slug, sizeof(slug));
		char path[PATH_MAX];
		if (!hkFmtFits(path, sizeof(path), "%s/%s.md", dir, slug)) {
			free(text); free(title);
			return strdup("error: memory path too long");
		}
		size_t tl = strlen(text);
		if (tl > HK_MEM_MAX_FILE) tl = HK_MEM_MAX_FILE;
		int rc = hk_fs_write(path, text, tl, 0);
		free(text); free(title);
		if (rc != 0) return strdup("error: cannot write memory");
		hkPromptInvalidate();
		char *msg = malloc(256);
		if (msg) snprintf(msg, 256, "remembered (%s): %s", global ? "global" : "this project", slug);
		return msg ? msg : strdup("remembered");
	}
	if (strcmp(name, "todo") == 0) {
		char *raw = hkJsonFirst(input_json, "items", "todos", "list", "plan", "content", NULL);
		if (!raw) return strdup("error: missing items (plain text, one step per line, [ ] or [x])");
		char *dec = hkJsonUnescape(raw, (int)strlen(raw));
		free(raw);
		if (!dec) return strdup("error: bad items escapes");
		int total = 0, done = 0;
		char *norm = hkTodoNormalize(dec, &total, &done);
		free(dec);
		if (!norm || !total) { free(norm); return strdup("error: no task lines found"); }
		free(hk_todo);
		hk_todo = norm;
		hkPromptInvalidate();
		hkSaveSession();
		size_t cap = strlen(hk_todo) + 128;
		char *msg = malloc(cap);
		if (!msg) return strdup("task list updated");
		size_t o = (size_t)snprintf(msg, cap, "task list \xC2\xB7 %d of %d done\n", done, total);
		for (const char *q = hk_todo; *q && o < cap - 8; ) {
			const char *nl = strchr(q, '\n');
			size_t seg = nl ? (size_t)(nl - q) : strlen(q);
			const char *mark = "\xE2\x97\x8B";                  /* ○ pending */
			const char *text = q;
			if (seg >= 3 && q[0] == '[' && q[2] == ']') {
				if (q[1] == 'x' || q[1] == 'X') mark = "\xE2\x9C\x93";   /* ✓ done */
				text = q + 3;
				seg -= 3;
				while (seg && *text == ' ') { text++; seg--; }
			}
			o += (size_t)snprintf(msg + o, cap - o, "  %s %.*s\n", mark, (int)seg, text);
			if (!nl) break;
			q = nl + 1;
		}
		return msg;
	}
	if (strcmp(name, "read_skill") == 0) {
		char *skill = hkExtractJsonString(input_json, "skill");
		char *path  = hkExtractJsonString(input_json, "path");
		if (!skill || !path) {
			free(skill); free(path);
			return strdup("error: missing skill or path");
		}
		if (strchr(skill, '/') || strstr(skill, "..")) {
			free(skill); free(path);
			return strdup("error: invalid skill name");
		}
		if (path[0] == '/' || strstr(path, "..")) {
			free(skill); free(path);
			return strdup("error: invalid path (must be relative, no ..)");
		}
		/* the nearest root that has it, as the prompt was built: a skill from
		   .claude/skills or .agents/skills used to be "not installed" here */
		char roots[HK_SKILL_ROOTS][640], skill_root[1024] = "";
		int nr = hkSkillRoots(roots);
		for (int i = 0; i < nr && !skill_root[0]; i++) {
			char c[1024];
			int is_dir = 0;
			if (!hkFmtFits(c, sizeof c, "%s/%s", roots[i], skill)) continue;
			if (!hk_fs_exists(c, NULL, NULL, &is_dir)
			    && (!hkFmtFits(c, sizeof c, "%s/%s.md", roots[i], skill) || !hk_fs_exists(c, NULL, NULL, &is_dir)))
				continue;
			if (!is_dir) {                    /* a one-file skill: that file is all of it */
				free(skill); free(path);
				char *c2 = hkReadFileAll(c, 200000);
				return c2 ? c2 : strdup("error: cannot read");
			}
			snprintf(skill_root, sizeof skill_root, "%s", c);
		}
		if (!skill_root[0]) { free(skill); free(path); return strdup("error: skill not installed"); }
		char full[PATH_MAX];
		if (!hkFmtFits(full, sizeof(full), "%s/%s", skill_root, path)) {
			free(skill); free(path);
			return strdup("error: skill path too long");
		}
		char real_root[PATH_MAX], real_full[PATH_MAX];
		if (!realpath(skill_root, real_root)) {
			free(skill); free(path);
			return strdup("error: skill not installed");
		}
		if (!realpath(full, real_full)) {
			free(skill); free(path);
			return strdup("error: file not found in skill");
		}
		size_t rlen = strlen(real_root);
		if (strncmp(real_full, real_root, rlen) != 0 ||
			(real_full[rlen] != '/' && real_full[rlen] != '\0')) {
			free(skill); free(path);
			return strdup("error: path escapes skill root");
		}
		free(skill); free(path);
		char *c = hkReadFileAll(real_full, 200000);
		return c ? c : strdup("error: cannot read");
	}
	if (strcmp(name, "write_file") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted");
		char *path = hkJsonFirst(input_json, "path", "file_path", "filename", "filepath", "file", NULL);
		char *content_raw = hkJsonFirst(input_json,
			"content", "contents", "file_text", "new_str", "text", "body", "code", "data", NULL);
		if (!path && !content_raw) return strdup("error: write_file needs both 'path' (or file_path) and 'content' params (got neither)");
		if (!path) { free(content_raw); return strdup("error: write_file missing 'path' param"); }
		if (!content_raw) { free(path); return strdup("error: write_file missing 'content' param — include the full file body as a string"); }
		char *content = hkJsonUnescape(content_raw, (int)strlen(content_raw));
		free(content_raw);
		if (!content) { free(path); return strdup("error: bad content escapes"); }
		char full[PATH_MAX];
		if (hkResolveInProject(path, full, sizeof(full)) != 0) {
			free(path); free(content);
			return strdup("error: path outside trusted project");
		}
		if (hkWasEditedThisTurn(path) && hkContentLooseEqFile(full, content)) {
			char *out = malloc(192);
			if (out) snprintf(out, 192,
				"no change: %s already has your edit (whole-file rewrite skipped). "
				"The fix is applied — reply with a short confirmation, no more tools.", path);
			free(path); free(content);
			return out ? out : strdup("no change: already applied");
		}
		size_t clen = strlen(content);
		int new_lines = 0;
		for (size_t i = 0; i < clen; i++) if (content[i] == '\n') new_lines++;
		if (clen > 0 && content[clen - 1] != '\n') new_lines++;
		long old_size = -1;
		int old_lines = 0;
		{
			/* Through the seam: fopen reaches nothing in a browser. */
			long olen = 0;
			char *old = hk_fs_read(full, 0, &olen);
			if (old) {
				old_size = olen;
				for (long i = 0; i < olen; i++) if (old[i] == '\n') old_lines++;
				free(old);
			}
		}
		if (hk_fs_write(full, content, clen, 0) != 0) {
			free(path); free(content);
			return strdup("error: cannot open for write");
		}
		size_t wrote = clen;
		snprintf(hk_last_write_path, sizeof(hk_last_write_path), "%s", path);
		char *out = malloc(256);
		if (old_size < 0)
			snprintf(out, 256, "wrote %zu bytes to %.150s (new file, %d lines)", wrote, hkProjectRel(full), new_lines);
		else
			snprintf(out, 256, "wrote %zu bytes to %.120s (%d lines; replaced %ld bytes / %d lines)",
				wrote, hkProjectRel(full), new_lines, old_size, old_lines);
		free(path); free(content);
		return out;
	}
	if (strcmp(name, "edit_file") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted");
		char *path = hkJsonFirst(input_json, "path", "file_path", "filename", NULL);
		char *old_raw = hkJsonFirst(input_json, "old", "old_str", "old_string", "search", NULL);
		char *new_raw = hkJsonFirst(input_json, "new", "new_str", "new_string", "replace", NULL);
		if (!path || !old_raw || !new_raw) {
			free(path); free(old_raw); free(new_raw);
			return strdup("error: edit_file needs 'path', 'old' (exact snippet to replace), and 'new' string params");
		}
		char full[PATH_MAX];
		if (hkResolveInProject(path, full, sizeof(full)) != 0) {
			free(path); free(old_raw); free(new_raw);
			return strdup("error: path outside project");
		}
		char *olds = hkJsonUnescape(old_raw, (int)strlen(old_raw)); free(old_raw);
		char *news = hkJsonUnescape(new_raw, (int)strlen(new_raw)); free(new_raw);
		char *buf  = (olds && news) ? hkReadFileAll(full, 4 * 1024 * 1024) : NULL;
		if (!olds || !news || !buf) {
			free(path); free(olds); free(news); free(buf);
			return strdup("error: cannot read file / bad escapes in old or new");
		}
		int fn = 0, on = 0; char *fbuf = NULL, *obuf = NULL;
		char **flines = hkSplitLines(buf, &fn, &fbuf);
		char **olines = hkSplitLines(olds, &on, &obuf);
		char *result = NULL;
		if (flines && olines && on > 0 && on <= fn) {
			int found = 0, at = -1, loose = 0;
			for (int i = 0; i + on <= fn; i++) {
				int ok = 1;
				for (int j = 0; j < on; j++)
					if (!hkLineEqTrim(flines[i+j], olines[j])) { ok = 0; break; }
				if (ok) { found++; at = i; }
			}
			if (found == 0) {
				for (int i = 0; i + on <= fn; i++) {
					int ok = 1;
					for (int j = 0; j < on; j++)
						if (!hkLineEqLoose(flines[i+j], olines[j])) { ok = 0; break; }
					if (ok) { found++; at = i; }
				}
				loose = 1;
			}
			if (found == 1) {
				int rm = 0;
				char *rep = loose ? hkReindentLike(news, flines, at, on) : NULL;
				char *spliced = hkSpliceLines(buf, at + 1, at + on, rep ? rep : news, &rm);
				if (spliced) { result = hkWriteResolved(full, path, spliced); free(spliced); hkMarkEdited(path); }
				else result = strdup("error: splice failed");
				free(rep);
			} else if (found == 0) {
				result = strdup("error: 'old' not found — copy it EXACTLY from the file, or use edit_lines with line numbers");
			} else {
				result = malloc(160);
				if (result) snprintf(result, 160,
					"error: 'old' matches %d places — add more surrounding lines to make it unique", found);
			}
		} else {
			result = strdup("error: 'old' is empty or longer than the file");
		}
		free(flines); free(fbuf); free(olines); free(obuf);
		free(buf); free(olds); free(news); free(path);
		return result ? result : strdup("error: edit failed");
	}
	if (strcmp(name, "edit_lines") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted");
		char *path = hkJsonFirst(input_json, "path", "file_path", "filename", NULL);
		if (!path) return strdup("error: edit_lines missing 'path'");
		int a = hkExtractJsonInt(input_json, "start");
		if (a < 0) a = hkExtractJsonInt(input_json, "start_line");
		if (a < 0) a = hkExtractJsonInt(input_json, "line");
		if (a < 0) { char *s = hkExtractJsonString(input_json, "start"); if (s) { a = atoi(s); free(s); } }
		int b = hkExtractJsonInt(input_json, "end");
		if (b < 0) b = hkExtractJsonInt(input_json, "end_line");
		if (b < 0) { char *s = hkExtractJsonString(input_json, "end"); if (s) { b = atoi(s); free(s); } }
		if (b < 0) b = a;
		char *new_raw = hkJsonFirst(input_json, "new", "new_str", "content", "text", NULL);
		if (a < 1 || b < a || !new_raw) {
			free(path); free(new_raw);
			return strdup("error: edit_lines needs 'start' (>=1), optional 'end' (>=start), and 'new' text");
		}
		char full[PATH_MAX];
		if (hkResolveInProject(path, full, sizeof(full)) != 0) {
			free(path); free(new_raw);
			return strdup("error: path outside project");
		}
		char *news = hkJsonUnescape(new_raw, (int)strlen(new_raw)); free(new_raw);
		char *buf  = news ? hkReadFileAll(full, 4 * 1024 * 1024) : NULL;
		if (!news || !buf) { free(path); free(news); free(buf); return strdup("error: cannot read file / bad escapes in new"); }
		int rm = 0;
		char *spliced = hkSpliceLines(buf, a, b, news, &rm);
		char *result = spliced ? hkWriteResolved(full, path, spliced)
		                       : strdup("error: line range past end of file");
		if (spliced) hkMarkEdited(path);
		free(spliced); free(buf); free(news); free(path);
		return result;
	}
	return strdup("error: unknown tool");
}

static const char *hkToolGlyph(const char *fname, int color) {
	if (!fname) return color ? "▸" : ">";
	if (!strcmp(fname, "read_file") || !strcmp(fname, "list_dir") ||
	    !strcmp(fname, "read_skill") || !strcmp(fname, "list_open_files") ||
	    !strcmp(fname, "read_open_file")) return color ? "◎" : "r";
	if (!strcmp(fname, "write_file")) return color ? "✎" : "w";
	if (!strcmp(fname, "edit_file") || !strcmp(fname, "edit_lines")) return color ? "✐" : "e";
	if (!strcmp(fname, "run_shell"))   return color ? "❯" : "$";
	return color ? "▸" : ">";
}

/* Draw one ledger entry, "name(arg)". Shared by the live call and a repaint. */
static void hkRenderToolCallRaw(const char *call) {
	const char *lp = strchr(call, '(');
	char fname[80];
	snprintf(fname, sizeof(fname), "%.*s", lp ? (int)(lp - call) : 79, call);
	const char *glyph = hkToolGlyph(fname, E.color_enabled);
	/* Hangs off the rail as a ledger entry rather than floating at column 0. */
	if (clRailOn()) {
		char body[400];
		snprintf(body, sizeof(body), "%s%s %s%s", ANSI_TOOL, glyph, call, ANSI_RESET);
		char pfx[64], cont[64];
		snprintf(pfx,  sizeof(pfx),  "%s%s%s", TH_ACCENT, HK_RAIL_TEE, ANSI_RESET);
		snprintf(cont, sizeof(cont), "%s%s%s  ", TH_ACCENT, HK_RAIL_BODY, ANSI_RESET);
		clPutWrapped2("", pfx, cont, body);
	} else if (E.color_enabled) {
		printf("%s%s %s%s\n", ANSI_TOOL, glyph, call, ANSI_RESET);
	} else {
		printf("%s %s\n", glyph, call);
	}
	fflush(stdout);
	E.last_role_shown = HK_ROLE_AI;   /* the rail continues past a tool call */
}

static char hk_last_tool_name[80];    /* for the result's log line */

static void hkAnnounceTool(aiData *data, const char *fname, const char *args_obj) {
	char arg_summary[128] = "";
	static const char *path_keys[] = {"path", "file_path", "filename", "directory", "dir", NULL};
	static const char *cmd_keys[]  = {"cmd", "command", "shell", "script", NULL};
	char *val = NULL;
	for (int i = 0; path_keys[i] && !val; i++) val = hkExtractJsonString(args_obj, path_keys[i]);
	if (!val) for (int i = 0; cmd_keys[i] && !val; i++) val = hkExtractJsonString(args_obj, cmd_keys[i]);
	if (val) { snprintf(arg_summary, sizeof(arg_summary), "%.80s", val); free(val); }
	/* The spinner shows the subject too: "reading  test.sh" beats "reading". */
	snprintf(hk_cur_subject, sizeof(hk_cur_subject), "%s", arg_summary);
	if (hk_bench_mode) return;
	snprintf(hk_last_tool_name, sizeof(hk_last_tool_name), "%s", fname);
	char call[300];
	snprintf(call, sizeof(call), "%s(%s)", fname, arg_summary);
	{
		static int said_auto = 0;
		if (!said_auto && E.ai_auto_approve && !hs_remote && !E.pipe_mode) {
			said_auto = 1;
			aiAddHistory(data, "auto mode: tools run without asking \xC2\xB7 shift+tab to change");
		}
	}
	hkLogMessage("tool", call);
	if (E.pipe_mode || hs_attached) {
		char display[320];
		snprintf(display, sizeof(display), "%s %.300s", hkToolGlyph(fname, 0), call);
		clPipeEmitDisplay("tool_start", display);
		if (E.pipe_mode) return;
	}
	/* Kept in the transcript so a repaint (resize, ctrl-o) still shows it. */
	if (data && data->history_count < AI_HISTORY_MAX) {
		char mark[320];
		snprintf(mark, sizeof(mark), "%s%s", HK_CALL_MARK, call);
		data->history[data->history_count] = strdup(mark);
		data->history_role[data->history_count] = HK_ROLE_SYSTEM;
		data->history_count++;
	}
	/* Bracketed: this used to print straight onto the bar's rows, and the
	   next hide wiped it — the ledger showed output with no call above it. */
	clOutBegin();
	hkRenderToolCallRaw(call);
	clOutEnd(data);
}

/* Render one stored tool output. Split out so the live path and a re-render
   after ctrl-o produce byte-identical results. */
static void hkRenderToolOutRaw(int idx, const char *result);
static void hkRenderToolResLineRaw(const char *line);

/* Expansion is per block as well as global: ctrl+o opens everything at once,… */
static unsigned char hk_tool_open[HK_TOOL_OUT_MAX];

/* While a redraw captures how the screen looks NOW, per-block state must be
   read from the snapshot taken before the click flipped it. NULL = live. */
static const unsigned char *hk_open_view = NULL;

static int hkToolIsOpen(int idx) {
	if (hk_expand_output) return 1;
	if (idx < 0 || idx >= HK_TOOL_OUT_MAX) return 0;
	return (hk_open_view ? hk_open_view : hk_tool_open)[idx];
}

static void hkRenderToolOut(int idx, const char *result) {
	clOutBegin();
	hkRenderToolOutRaw(idx, result);
	clOutEnd(&G_AI);
}

static void hkRenderToolOutRaw(int idx, const char *result) {
	if (!result) return;
	int len = (int)strlen(result);
	char pfx[48];
	if (clRailOn()) snprintf(pfx, sizeof(pfx), "%s%s%s  ", TH_ACCENT, HK_RAIL_BODY, ANSI_RESET);
	else            snprintf(pfx, sizeof(pfx), "  ");

	int limit = hkToolIsOpen(idx) ? 100000 : HK_TOOL_SHOW_LINES;
	int shown = 0, total = 0;
	for (const char *q = result; *q; q++) if (*q == '\n') total++;
	if (len && result[len-1] != '\n') total++;
	const char *q = result;
	while (*q && shown < limit) {
		const char *nl = strchr(q, '\n');
		size_t n2 = nl ? (size_t)(nl - q) : strlen(q);
		char row[1024];
		if (n2 >= sizeof(row)) n2 = sizeof(row) - 1;
		memcpy(row, q, n2); row[n2] = '\0';
		char body[1100];
		snprintf(body, sizeof(body), "%s%s%s", ANSI_DIM, row, ANSI_RESET);
		clPutWrapped2("", pfx, pfx, body);
		shown++;
		if (!nl) break;
		q = nl + 1;
	}
	if (total > shown) {
		char more[128];
		snprintf(more, sizeof(more), "%s… +%d lines (ctrl-o to expand)%s",
		         TH_META, total - shown, ANSI_RESET);
		clPutWrapped2("", pfx, pfx, more);
	}
	fflush(stdout);
}

/* Repaint the visible transcript. Tool markers expand or collapse with the
   current setting, so ctrl-o changes the whole session and not just the tail. */
/* Render ONE stored transcript entry. */
static void hkRenderHistoryEntry(aiData *data, int i) {
	const char *h = data->history[i];
	unsigned char role = data->history_role[i];
	if (h[0] == '\x01') {                     /* a tool entry, saved with the chat */
		if (!strncmp(h, HK_TOOL_MARK, 2)) {
			int idx = atoi(h + 2);
			if (idx >= 0 && idx < hk_tool_out_n) hkRenderToolOut(idx, hk_tool_outs[idx]);
		} else if (!strncmp(h, HK_CALL_MARK, 2)) hkRenderToolCallRaw(h + 2);
		else if (!strncmp(h, HK_RES_MARK, 2))    hkRenderToolResLineRaw(h + 2);
		return;
	}
	char *body = (role == HK_ROLE_AI) ? hkStripToolBlocks(h) : NULL;
	const char *t = body ? body : h;
	if (!*t) { clPrintRoleLine(role, ""); free(body); return; }   /* a blank line is a row too */
	while (*t) {                              /* one line per line, as live */
		const char *nl = strchr(t, '\n');
		size_t seg = nl ? (size_t)(nl - t) : strlen(t);
		char lbuf[4096];
		if (seg >= sizeof(lbuf)) seg = sizeof(lbuf) - 1;
		memcpy(lbuf, t, seg); lbuf[seg] = '\0';
		clPrintRoleLine(role, lbuf);
		if (!nl) break;
		t = nl + 1;
	}
	free(body);
}

/* ---------- redraw the viewport in place ---------- */
/* Render history entries [from,to) into a buffer, exactly as the live screen… */
static char *clCaptureRange(aiData *data, int from, int to, int expand,
                            unsigned char last_before, int *rows_out) {
#ifdef HAKO_WASM
	(void)data; (void)from; (void)to; (void)expand; (void)last_before;
	*rows_out = 0;
	return NULL;
#else
	fflush(stdout);
	char path[PATH_MAX];
	const char *tmp = getenv("TMPDIR");
	snprintf(path, sizeof(path), "%s/hako-cap-XXXXXX", (tmp && *tmp) ? tmp : "/tmp");
	int fd = mkstemp(path);
	if (fd < 0) { *rows_out = 0; return NULL; }
	unlink(path);
	int saved = dup(STDOUT_FILENO);
	if (saved < 0) { close(fd); *rows_out = 0; return NULL; }

	int save_expand = hk_expand_output;
	unsigned char save_role = E.last_role_shown;
	int save_fence = cl_in_codefence;
	int save_want = cl_bar_want;
	cl_bar_want = 0;
	hk_expand_output = expand;
	E.last_role_shown = last_before;
	cl_in_codefence = 0;
	cl_cols_pinned = clTermCols();          /* wrap exactly as the live print did */

	dup2(fd, STDOUT_FILENO);
	for (int i = from; i < to; i++) hkRenderHistoryEntry(data, i);
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);

	cl_cols_pinned = 0;
	hk_expand_output = save_expand;
	E.last_role_shown = save_role;
	cl_in_codefence = save_fence;
	cl_bar_want = save_want;

	off_t len = lseek(fd, 0, SEEK_CUR);
	char *buf = malloc((size_t)len + 1);
	if (!buf) { close(fd); *rows_out = 0; return NULL; }
	lseek(fd, 0, SEEK_SET);
	size_t got = 0;
	while (got < (size_t)len) {
		ssize_t r = read(fd, buf + got, (size_t)len - got);
		if (r <= 0) break;
		got += (size_t)r;
	}
	close(fd);
	buf[got] = '\0';
	int rows = 0;
	for (const char *q = buf; *q; q++) if (*q == '\n') rows++;
	if (got > 0 && buf[got - 1] != '\n') rows++;   /* an unterminated tail is its own row */
	*rows_out = rows;
	return buf;
#endif
}

/* Redraw the transcript that is on screen, in place. */
static void clRedrawViewport(aiData *data, int old_expand, const unsigned char *old_open) {
	if (!data || E.pipe_mode || E.output_json || !E.color_enabled || !data->history_count) return;
	int n = data->history_count;
	int barh = cl_bar_h > 0 ? cl_bar_h : 6;
	int avail = clTermRows() - barh;
	if (avail < 3) return;
	int new_expand = hk_expand_output;

	/* Held for the whole routine: rendering into a capture still runs the
	   ordinary print brackets, and they must not be mistaken for output. */
	cl_in_vp = 1;

	int start = n, old_total = 0, overflow = 0;
	hk_open_view = old_open;
	while (start > 0) {
		unsigned char bf = start >= 2 ? data->history_role[start - 2] : HK_ROLE_SYSTEM;
		int ro = 0;
		char *bo = clCaptureRange(data, start - 1, n, old_expand, bf, &ro);
		free(bo);
		if (!bo) break;
		if (ro > avail) {
			overflow = 1;
			if (start == n) { old_total = ro; start--; }   /* one entry, bigger than the screen */
			break;
		}
		old_total = ro;
		start--;
	}
	hk_open_view = NULL;
	if (start >= n) { cl_in_vp = 0; return; }   /* history is empty */

	if (overflow) {
		while (start > 0) {
			unsigned char bf = start >= 2 ? data->history_role[start - 2] : HK_ROLE_SYSTEM;
			int rn = 0;
			char *bn = clCaptureRange(data, start - 1, n, new_expand, bf, &rn);
			free(bn);
			if (!bn || rn > avail) break;
			start--;
		}
	}

	unsigned char before = start >= 1 ? data->history_role[start - 1] : HK_ROLE_SYSTEM;
	int rows = 0;
	char *buf = clCaptureRange(data, start, n, new_expand, before, &rows);
	if (!buf) { cl_in_vp = 0; return; }

	int climb = old_total < avail ? old_total : avail;
	int skip  = rows > avail ? rows - avail : 0;   /* clip the head, keep the tail */
	if (overflow) climb = avail;           /* the region was full: reclaim all of it */

	clOutBegin();                          /* bar down: cursor just under the chat */
	if (climb > 0) printf("\r\x1b[%dA", climb);
	if (overflow) fputs("\x1b[1J", stdout);
	int row_i = 0;
	for (const char *q = buf; *q; row_i++) {
		const char *nl = strchr(q, '\n');
		size_t seg = nl ? (size_t)(nl - q) : strlen(q);
		if (row_i >= skip) {
			fputs("\x1b[2K", stdout);
			fwrite(q, 1, seg, stdout);
			if (nl) fputc('\n', stdout);
		}
		if (!nl) break;
		q = nl + 1;
	}
	printf("\x1b[J");                       /* whatever the old view had below */
	fflush(stdout);
	free(buf);
	clOutEnd(data);                         /* bar back underneath */
	cl_in_vp = 0;
}


/* A theme change applies to the bar at once and to everything printed from here on. */
static void hkThemeRefreshBar(aiData *data) {
	if (E.pipe_mode || E.output_json || !E.color_enabled) return;
	clRedrawViewport(data, hk_expand_output, NULL);   /* the chat on screen, then the bar */
}

/* ctrl+o changes how much of a tool result is shown. Applies to what comes
   next, and says so, rather than reprinting the session to re-level it. */
/* Which history entry is printed on absolute screen row `row` (1-based)? */
static int hkEntryAtRow(aiData *data, int row) {
	if (!data || data->history_count <= 0) return -1;
	int bottom = clTermRows() - (cl_bar_h > 0 ? cl_bar_h : 0);   /* last transcript row */
	if (row < 1 || row > bottom) return -1;
	int want_up = bottom - row;            /* rows between the click and the bottom */
	int acc = 0;
	for (int i = data->history_count - 1; i >= 0; i--) {
		int r = 0;
		unsigned char before = i >= 1 ? data->history_role[i - 1] : HK_ROLE_SYSTEM;
		char *b = clCaptureRange(data, i, i + 1, hk_expand_output, before, &r);
		free(b);
		if (!b) return -1;
		acc += r;
			if (want_up < acc) return i;
		if (acc > clTermRows() + 4) return -1;      /* off the top: stop looking */
	}
	return -1;
}

/* A tap on a tool result opens just that one; a tap inside an open one closes
   it. Anything else is ignored, so a stray click never disturbs the screen. */
static int hkClickRow(aiData *data, int row) {
	int i = hkEntryAtRow(data, row);
	if (i < 0) return 0;
	const char *h = data->history[i];
	if (strncmp(h, HK_TOOL_MARK, 2) != 0) return 0;
	int idx = atoi(h + 2);
	if (idx < 0 || idx >= HK_TOOL_OUT_MAX) return 0;
	(void)idx;
	hkTranscriptView(data);                /* same place ctrl+o goes */
	return 1;
}

/* ---------- the transcript screen ---------- */
static void hkTranscriptView(aiData *data) {
	if (!data || data->history_count <= 0) return;
	if (E.pipe_mode || E.output_json || !E.color_enabled) return;
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;

	int total_rows = 0;
	char *buf = clCaptureRange(data, 0, data->history_count, 1, HK_ROLE_SYSTEM, &total_rows);
	if (!buf) return;

	int cap = 512, n = 0;
	char **ln = malloc(sizeof(char *) * (size_t)cap);
	if (!ln) { free(buf); return; }
	for (char *q = buf;;) {
		if (n >= cap) {
			cap *= 2;
			char **g = realloc(ln, sizeof(char *) * (size_t)cap);
			if (!g) break;
			ln = g;
		}
		ln[n++] = q;
		char *nl = strchr(q, '\n');
		if (!nl) break;
		*nl = '\0';
		q = nl + 1;
		if (!*q) break;
	}

	int raw_was = cl_raw_active;
	if (!raw_was) clEnableRaw();
	if (write(STDOUT_FILENO, "\x1b[?1049h", 8) < 0) {}

	int top = n, quit = 0;
	while (!quit) {
		int rows = clTermRows(), cols = clTermCols();
		int page = rows - 1; if (page < 1) page = 1;
		if (top > n - page) top = n - page;
		if (top < 0) top = 0;

		printf("\x1b[H\x1b[2J");
		for (int i = 0; i < page; i++) {
			if (top + i >= n) { printf("\r\n"); continue; }
			char row[4096];
			snprintf(row, sizeof(row), "%s", ln[top + i]);
			if (clCellWidth(row) > cols) clClipToCells(row, cols);
			printf("\r%s\x1b[K\n", row);
		}
		{
			int last = top + page < n ? top + page : n;
			char foot[192];
			snprintf(foot, sizeof(foot),
			         "  transcript  %d-%d of %d  \xC2\xB7  \xE2\x86\x91\xE2\x86\x93 scroll  \xC2\xB7  "
			         "pgup/pgdn  \xC2\xB7  g/G ends  \xC2\xB7  q closes", top + 1, last, n);
			if (clCellWidth(foot) > cols - 1) clClipToCells(foot, cols - 1);
			printf("\r%s%s%s\x1b[K", TH_META, foot, ANSI_RESET);
		}
		fflush(stdout);

		char c;
		if (read(STDIN_FILENO, &c, 1) != 1) break;
		switch (c) {
		case 'q': case 3: case 15: quit = 1; break;
		case 'j': top++; break;
		case 'k': top--; break;
		case 'g': top = 0; break;
		case 'G': top = n; break;
		case ' ': top += page; break;
		case 'b': top -= page; break;
		case '\x1b': {
			char s1, s2;
			if (!clReadByteTimeout(&s1, 40)) { quit = 1; break; }   /* bare esc closes */
			if (s1 != '[' && s1 != 'O') break;
			if (!clReadByteTimeout(&s2, 40)) break;
			if      (s2 == 'A') top--;
			else if (s2 == 'B') top++;
			else if (s2 == 'H') top = 0;
			else if (s2 == 'F') top = n;
			else if (s2 >= '0' && s2 <= '9') {
				char s3;
				if (clReadByteTimeout(&s3, 40) && s3 == '~') {
					if      (s2 == '5') top -= page;
					else if (s2 == '6') top += page;
					else if (s2 == '1' || s2 == '7') top = 0;
					else if (s2 == '4' || s2 == '8') top = n;
				}
			}
			break;
		}
		default: break;
		}
	}

	if (write(STDOUT_FILENO, "\x1b[?1049l", 8) < 0) {}
	if (!raw_was) clDisableRaw();
	free(ln);
	free(buf);
}

/* ctrl+o is the transcript screen now. Nothing in the chat changes height, so
   nothing can scroll a second copy of it into the scrollback. */
static void hkExpandToggle(aiData *data) {
	hkTranscriptView(data);
}


/* {"role":"result","tool":NAME,"content":...} — what the tool said, capped,… */
static void hkLogToolResult(const char *result) {
	int len = result ? (int)strlen(result) : 0;
	int cap = len < 2000 ? len : 2000;
	char *cut = malloc((size_t)cap + 8);
	if (!cut) return;
	memcpy(cut, result ? result : "", (size_t)cap);
	cut[cap] = '\0';
	if (cap < len) strcat(cut, "\n\xE2\x80\xA6");
	char path[PATH_MAX];
	hkSessionLogPath(path, sizeof(path));
	if (path[0]) {
		char *esc = malloc((size_t)cap * 6 + 32);
		if (esc) {
			hkJsonEscapeInto(cut, esc, cap * 6 + 32);
			size_t lcap = strlen(esc) + 160;
			char *line = malloc(lcap);
			if (line) {
				int n = snprintf(line, lcap, "{\"ts\":%ld,\"role\":\"result\",\"tool\":\"%s\",\"content\":\"%s\"}\n",
				                 hk_time_unix(), hk_last_tool_name, esc);
				hk_fs_write(path, line, (size_t)n, 1);
				free(line);
			}
			free(esc);
		}
	}
	free(cut);
}

/* The one-line result ("← wrote x", "✗ error: …", "← 812 bytes"), already
   colored. Shared by the live path and a repaint. */
static void hkRenderToolResLineRaw(const char *line) {
	if (!E.color_enabled) { printf("  %s\n", line); fflush(stdout); return; }
	if (clRailOn()) {
		char pfx[48];
		snprintf(pfx, sizeof(pfx), "%s%s%s  ", TH_ACCENT, HK_RAIL_BODY, ANSI_RESET);
		clPutWrapped2("", pfx, pfx, line);
	} else {
		printf("  %s\n", line);
	}
	fflush(stdout);
}

static void hkAnnounceToolResult(aiData *data, const char *result) {
	if (hk_bench_mode) return;
	int len = result ? (int)strlen(result) : 0;
	int is_err = result && strncmp(result, "error:", 6) == 0;
	int is_summary = result && (strncmp(result, "wrote ", 6) == 0 ||
	                            strncmp(result, "no change", 9) == 0);
	hkLogToolResult(result);
	if (E.pipe_mode || hs_attached) {
		char display[256];
		if (is_err || is_summary) snprintf(display, sizeof(display), "%s", result ? result : "error");
		else snprintf(display, sizeof(display), "%d bytes", len);
		clPipeEmitDisplay("tool_end", display);
		if (!is_err && !is_summary && result && len > 0) {
			int cap = len < 2000 ? len : 2000;
			char *cut = malloc((size_t)cap + 8);
			if (cut) {
				memcpy(cut, result, (size_t)cap);
				cut[cap] = '\0';
				if (cap < len) strcat(cut, "\n…");
				char *esc = malloc((size_t)cap * 6 + 64);
				if (esc) {
					clPipeEscape(cut, esc, (int)((size_t)cap * 6 + 64));
					size_t lcap = strlen(esc) + 64;
					char *line = malloc(lcap);
					if (line) {
						snprintf(line, lcap, "{\"type\":\"tool_result\",\"text\":\"%s\"}", esc);
						clPipeEmitRaw(line);
						free(line);
					}
					free(esc);
				}
				free(cut);
			}
		}
		if (E.pipe_mode) return;
	}
	free(hk_last_tool_out);
	hk_last_tool_out = result ? strdup(result) : NULL;

	/* Errors and one-line summaries say everything already; real output is
	   registered so it can be re-rendered later at a different verbosity. */
	if (!is_err && !is_summary && result && len > 0) {
		int idx = hkToolOutAdd(result);
		if (idx >= 0 && data && data->history_count < AI_HISTORY_MAX) {
			char mark[24];
			snprintf(mark, sizeof(mark), "%s%d", HK_TOOL_MARK, idx);
			data->history[data->history_count] = strdup(mark);
			data->history_role[data->history_count] = HK_ROLE_SYSTEM;
			data->history_count++;
		}
		hkRenderToolOut(idx, result);
		return;
	}

	char line[512];
	if (!E.color_enabled) {
		if (is_err)          snprintf(line, sizeof(line), "! %s", result);
		else if (is_summary) snprintf(line, sizeof(line), "<- %s", result);
		else                 snprintf(line, sizeof(line), "<- %d bytes", len);
	}
	else if (is_err)     snprintf(line, sizeof(line), "%s\xE2\x9C\x97 %s%s", ANSI_ERR, result, ANSI_RESET);
	else if (is_summary) snprintf(line, sizeof(line), "%s\xE2\x86\x90 %s%s", ANSI_DIM, result, ANSI_RESET);
	else                 snprintf(line, sizeof(line), "%s\xE2\x86\x90 %d bytes%s", ANSI_DIM, len, ANSI_RESET);

	/* One line, but it goes through history so a repaint keeps it, and
	   through the bracket so the bar is never under it. */
	if (data && data->history_count < AI_HISTORY_MAX) {
		char mark[520];
		snprintf(mark, sizeof(mark), "%s%s", HK_RES_MARK, line);
		data->history[data->history_count] = strdup(mark);
		data->history_role[data->history_count] = HK_ROLE_SYSTEM;
		data->history_count++;
	}
	clOutBegin();
	hkRenderToolResLineRaw(line);
	clOutEnd(data);
}

static int aiBuildRequest(aiData *data, enum aiProviderType type, hkReqParts *out) {
	/* Whatever the credential's provenance — pasted, refreshed, read from a file
	   — it is about to be interpolated into a quoted shell argument. */
	hkTrimSecret(&E.ai_api_key);
	memset(out, 0, sizeof(*out));
	char *msgs = aiBuildMessagesJson(data);
	if (!msgs) return -1;

	const char *endpoint = E.ai_endpoint;
	const char *model = E.ai_model;
	const char *api_key = E.ai_api_key;
	int max_tokens = E.ai_max_tokens > 0 ? E.ai_max_tokens : 4096;
	if (E.ai_tools_enabled) {
		int floor = (type == AI_PROVIDER_ANTHROPIC) ? 8192 : 4096;
		if (max_tokens < floor) max_tokens = floor;
	}

	const char *sys = (data->system_prompt && *data->system_prompt) ? data->system_prompt : "";
	char *sys_esc = NULL;
	if (*sys) {
		int slen = strlen(sys);
		sys_esc = malloc(slen * 6 + 8);
		hkJsonEscapeInto(sys, sys_esc, slen * 6 + 8);
	}

	int bodycap = strlen(msgs) + (sys_esc ? strlen(sys_esc) : 0) + 4096;
	char *body = malloc(bodycap);
	if (!body) { free(msgs); free(sys_esc); return -1; }

	int tools_on = E.ai_tools_enabled;
	if (E.ai_toolmode == 1) tools_on = 0;
	if (hkSubProse()) tools_on = 0;
	int chatgpt = type == AI_PROVIDER_OPENAI && hkOAuthIs("chatgpt");
	if (type == AI_PROVIDER_MITHRAEUM) tools_on = 0;
	if (tools_on && E.ai_tool_gate && data->message_count > 0) {
		aiMessage *last = &data->messages[data->message_count - 1];
		if (last->role && !strcmp(last->role, "user") && last->raw == 0 && last->content) {
			static const char *keywords[] = {
				"read", "list", "write", "run", "exec", "file", "files", "dir", "folder",
				"directory", "ls", "cat", "show me", "open", "create", "edit", "save",
				"delete", "remove", "find", "search", "grep", "shell",
				"contents", "what's in", "whats in", "what is in", "what files",
				"this project", "this repo", "this directory", "this folder",
				"the project", "the repo", "the directory", "the folder",
				"source", "code base", "codebase", ".c", ".h", ".md", ".txt", ".json",
				".py", ".js", ".ts", ".go", ".rs", "./", "../", NULL
			};
			char *low = strdup(last->content);
			for (int i = 0; low[i]; i++) if (low[i] >= 'A' && low[i] <= 'Z') low[i] += 32;
			int hit = 0;
			for (int i = 0; keywords[i] && !hit; i++) if (strstr(low, keywords[i])) hit = 1;
			free(low);
			if (!hit) tools_on = 0;
		}
	}
	char *anth_tools = NULL, *fn_tools = NULL;
	if (tools_on) {
		anth_tools = hkBuildToolsSchema(0);
		fn_tools = hkBuildToolsSchema(1);
	}

	char *msgs_with_sys = NULL;
	if (sys_esc && *sys_esc && type != AI_PROVIDER_ANTHROPIC && !chatgpt) {
		size_t mlen = strlen(msgs);
		size_t elen = strlen(sys_esc);
		size_t cap = mlen + elen + 64;
		msgs_with_sys = malloc(cap);
		if (msgs_with_sys) {
			int empty = (mlen >= 2 && msgs[0] == '[' && msgs[1] == ']');
			if (empty) {
				snprintf(msgs_with_sys, cap,
					"[{\"role\":\"system\",\"content\":\"%s\"}]", sys_esc);
			} else {
				snprintf(msgs_with_sys, cap,
					"[{\"role\":\"system\",\"content\":\"%s\"},%s", sys_esc, msgs + 1);
			}
			free(msgs);
			msgs = msgs_with_sys;
		}
	}

	switch (type) {
	case AI_PROVIDER_MITHRAEUM:
	case AI_PROVIDER_OLLAMA: {
		if (!endpoint) endpoint = "http://localhost:11434";
		if (!model) model = (type == AI_PROVIDER_MITHRAEUM) ? "hako-sho" : "llama3.2";
		int tlen = tools_on ? strlen(fn_tools) + 16 : 0;
		int need = bodycap + tlen + 256;
		if (need > bodycap) { body = realloc(body, need); bodycap = need; }
		int npred = E.ai_max_tokens > 0 ? E.ai_max_tokens : 1024;
		if (tools_on && npred < 4096) npred = 4096;
		if (tools_on) {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"messages\":%s,\"stream\":false,\"keep_alive\":\"30m\",\"options\":{\"num_predict\":%d},\"tools\":%s}",
				model, msgs, npred, fn_tools);
		} else {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"messages\":%s,\"stream\":false,\"keep_alive\":\"30m\",\"options\":{\"num_predict\":%d}}",
				model, msgs, npred);
		}
		break;
	}
	case AI_PROVIDER_ANTHROPIC: {
		if (!endpoint) endpoint = "https://api.anthropic.com";
		if (!model) model = "claude-haiku-4-5";
		int anth_on = tools_on && hkProjectTrusted();
		int oauth_anth = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic");
		int stream_on = oauth_anth || (E.ai_stream && !anth_on);
		int tlen = anth_on ? strlen(anth_tools) + 16 : 0;
		int need = bodycap + tlen + 512;
		if (need > bodycap) { body = realloc(body, need); bodycap = need; }
		const char *stream_field = stream_on ? ",\"stream\":true" : "";

		char *oauth_block2 = NULL;
		if (oauth_anth) {
			const char *cli_lead =
				"You are an interactive CLI tool. The tools available to you operate "
				"on the user's real filesystem in the current project directory. When "
				"the user asks to read, list, create, edit, or run something, call the "
				"matching tool directly. Do not refuse, do not claim you cannot access "
				"files, do not suggest the user run shell commands themselves — invoke "
				"the tools.\\n\\n"
				"ORDERING RULE — STRICT. When a tool call is needed, EMIT THE TOOL "
				"CALL FIRST, before any prose. Do not write 'I'll create the file...' "
				"or 'Done!' or 'Perfect!' or any narration BEFORE the tool call — those "
				"sentences print to the user before the tool actually runs, which "
				"looks like you're lying. The correct shape is: (1) tool call, (2) "
				"wait for observation, (3) THEN one short sentence describing what "
				"actually happened based on the observation. Never claim success "
				"before the observation comes back.\\n\\n"
				"HALLUCINATION RULE — STRICT. You have no memory of previous file "
				"creations in this session. If the user just asked you to create "
				"a file, the file does NOT exist yet. Do not respond with "
				"'Created X' or 'X has been created' until you have emitted a "
				"tool call AND received an observation confirming success. Prefer "
				"write_file over bash heredocs/echo for file creation — it is "
				"path-safe and atomic.\\n\\n"
				"AVAILABLE TOOLS — write_file(path, content), edit_file(path, old, new), "
				"edit_lines(path, start, end, new), read_file(path), "
				"list_dir(path), run_shell(cmd). Prefer edit_file/edit_lines for small "
				"changes to an existing file instead of rewriting the whole thing. Do "
				"not invent tools like `str_replace_based_edit_tool`, `text_editor`, "
				"`create_file`; they are remapped but cost an extra round-trip. Use "
				"canonical names.\\n\\n"
				"All paths are relative to the project root. Use \\\".\\\" "
				"for the project root. Never use absolute paths.\\n\\n";
			size_t cl = strlen(cli_lead);
			size_t sl = sys_esc ? strlen(sys_esc) : 0;
			oauth_block2 = malloc(cl + sl + 16);
			if (oauth_block2) {
				memcpy(oauth_block2, cli_lead, cl);
				if (sys_esc) memcpy(oauth_block2 + cl, sys_esc, sl);
				oauth_block2[cl + sl] = '\0';
			}
		}
		size_t scap = (oauth_block2 ? strlen(oauth_block2) : (sys_esc ? strlen(sys_esc) : 0)) + 512;
		char *sys_field = malloc(scap); sys_field[0] = '\0';
		const char *cache_mark = E.ai_prompt_cache ? ",\"cache_control\":{\"type\":\"ephemeral\"}" : "";
		if (oauth_anth) {
			snprintf(sys_field, scap,
				",\"system\":[{\"type\":\"text\",\"text\":\"You are Claude Code, Anthropic's official CLI for Claude.\"},{\"type\":\"text\",\"text\":\"%s\"%s}]",
				oauth_block2 ? oauth_block2 : "", cache_mark);
		} else if (*sys) {
			if (E.ai_prompt_cache)
				snprintf(sys_field, scap, ",\"system\":[{\"type\":\"text\",\"text\":\"%s\"%s}]", sys_esc, cache_mark);
			else
				snprintf(sys_field, scap, ",\"system\":\"%s\"", sys_esc);
		}
		free(oauth_block2);

		need = bodycap + (int)strlen(sys_field) + 16;
		if (need > bodycap) { body = realloc(body, need); bodycap = need; }
		if (anth_on) {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"max_tokens\":%d%s,\"messages\":%s,\"tools\":%s%s}",
				model, max_tokens, sys_field, msgs, anth_tools, stream_field);
		} else {
			/* Prose tools: stop at the call, so the model cannot write on past it
			   and invent the result ("It looks like there's no project here"). */
			const char *stop = (E.ai_tools_enabled && hkProjectTrusted()
			                    && (oauth_anth || E.ai_toolmode == 1))
			                 ? ",\"stop_sequences\":[\"</tool>\"]" : "";
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"max_tokens\":%d%s,\"messages\":%s%s%s}",
				model, max_tokens, sys_field, msgs, stream_field, stop);
		}
		free(sys_field);
		break;
	}
	case AI_PROVIDER_OPENAI: {
		if (chatgpt) {
			/* Responses API: the system prompt is "instructions", and plain
			   {role, content} messages are valid input items as they are. */
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"instructions\":\"%s\",\"input\":%s,"
				"\"stream\":true,\"store\":false}",
				model ? model : "gpt-5.5", sys_esc ? sys_esc : "", msgs);
			break;
		}
		if (!endpoint) endpoint = "https://api.openai.com";
		if (!model) model = hkProviderDefaultModel(E.ai_provider_type);
		if (!model) model = "gpt-4o-mini";
		const char *reason = "";
		if (endpoint && strstr(endpoint, "generativelanguage") && strstr(model, "2.5"))
			reason = ",\"reasoning_effort\":\"none\"";
		const char *stream_field = E.ai_stream
			? ",\"stream\":true,\"stream_options\":{\"include_usage\":true}" : "";
		int tlen = tools_on ? strlen(fn_tools) + 16 : 0;
		int need = bodycap + tlen + 160 + (int)strlen(reason);
		if (need > bodycap) { body = realloc(body, need); bodycap = need; }
		if (tools_on) {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"max_tokens\":%d%s%s,\"messages\":%s,\"tools\":%s}",
				model, max_tokens, reason, stream_field, msgs, fn_tools);
		} else {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"max_tokens\":%d%s%s,\"messages\":%s}",
				model, max_tokens, reason, stream_field, msgs);
		}
		break;
	}
	default:
		free(body); free(msgs); free(sys_esc); free(anth_tools); free(fn_tools); return -1;
	}

	free(msgs);
	free(sys_esc);
	free(anth_tools);
	free(fn_tools);

	out->body = body;
	out->stream = 0;

	char url[1024], hdr[8192];
	switch (type) {
	case AI_PROVIDER_MITHRAEUM:
	case AI_PROVIDER_OLLAMA:
		snprintf(url, sizeof(url), "%s/api/chat", endpoint);
		if (api_key && *api_key) snprintf(hdr, sizeof(hdr), "-H 'Authorization: Bearer %s'", api_key);
		else hdr[0] = '\0';
		break;
	case AI_PROVIDER_ANTHROPIC: {
		if (!api_key) { free(body); out->body = NULL; return -1; }
		int oauth = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic");
		if (!api_key || !*api_key) {
			free(body);
			out->body = NULL;
			fprintf(stderr, oauth
				? "hako: Anthropic sign-in expired (token refresh failed) — run :login anthropic\n"
				: "hako: no Anthropic credential — run :login anthropic, or set an API key\n");
			return -1;
		}
		char auth[640];
		if (oauth) {
			snprintf(auth, sizeof(auth),
				"-H 'Authorization: Bearer %s' "
				"-H 'anthropic-beta: oauth-2025-04-20' "
				"-H 'User-Agent: claude-cli/1.0.40 (external, cli)' "
				"-H 'x-app: cli'", api_key);
		} else {
			snprintf(auth, sizeof(auth), "-H 'x-api-key: %s'", api_key);
		}
		out->stream = oauth || (E.ai_stream && !(E.ai_tools_enabled && hkProjectTrusted()));
		snprintf(url, sizeof(url), "%s/v1/messages", endpoint);
		snprintf(hdr, sizeof(hdr), "%s -H 'anthropic-version: 2023-06-01'%s%s",
		         auth, out->stream ? " -H 'accept: text/event-stream'" : "",
#ifdef HAKO_WASM
		         /* Anthropic blocks cross-origin calls unless this is present;
		            with it the preflight answers allow-origin: *. */
		         " -H 'anthropic-dangerous-direct-browser-access: true'"
#else
		         ""
#endif
		         );
		break;
	}
	case AI_PROVIDER_OPENAI: {
		if (!api_key) { free(body); out->body = NULL; return -1; }
		if (!endpoint) endpoint = chatgpt ? "https://chatgpt.com/backend-api/codex" : "https://api.openai.com";
		if (chatgpt) {
			snprintf(url, sizeof(url), "%s/responses", endpoint);
			out->stream = 1;          /* the codex backend only streams */
			if (!hkChatGPTHeaders(hdr, sizeof(hdr), api_key, 1)) {
				free(body); out->body = NULL; return -1;
			}
			break;
		}
		int copilot = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-copilot");
		int ghmodels = endpoint && strstr(endpoint, "models.inference.ai.azure.com");
		size_t elen = endpoint ? strlen(endpoint) : 0;
		int has_v1 = elen >= 3 && !strcmp(endpoint + elen - 3, "/v1");
		int compat_base = elen >= 14 && !strcmp(endpoint + elen - 14, "/v1beta/openai");   /* groq's /openai still wants /v1 */
		snprintf(url, sizeof(url), "%s%s", endpoint,
		         (copilot || ghmodels || has_v1 || compat_base)
		             ? "/chat/completions" : "/v1/chat/completions");
		out->stream = E.ai_stream ? 1 : 0;
		snprintf(hdr, sizeof(hdr), "-H 'Authorization: Bearer %s' %s", api_key, copilot
			? "-H 'Editor-Version: " HAKO_COPILOT_EDITOR_VER "' "
			  "-H 'Editor-Plugin-Version: " HAKO_COPILOT_PLUGIN_VER "' "
			  "-H 'Openai-Intent: conversation-panel' "
			  "-H 'Copilot-Integration-Id: vscode-chat' "
			: "");
		if (copilot && strstr(body, "{\"type\":\"image_url\""))
			hkFmtFits(hdr + strlen(hdr), sizeof(hdr) - strlen(hdr), "-H 'Copilot-Vision-Request: true' ");
		break;
	}
	default:
		free(body); out->body = NULL; return -1;
	}

	out->url = strdup(url);
	out->headers = strdup(hdr);
	return (out->url && out->headers) ? 0 : -1;
}

static char *aiExtractStringValue(const char *p) {
	int cap = 1024, len = 0;
	char *result = malloc(cap);
	while (*p && *p != '"') {      /* the escape branch below consumes `\\` as a pair */
		if (len >= cap - 8) { cap *= 2; result = realloc(result, cap); }
		if (*p == '\\' && *(p + 1)) {
			p++;
			switch (*p) {
			case 'n': result[len++] = '\n'; break;
			case 't': result[len++] = '\t'; break;
			case 'r': result[len++] = '\r'; break;
			case 'b': result[len++] = '\b'; break;
			case 'f': result[len++] = '\f'; break;
			case '"': result[len++] = '"'; break;
			case '\\': result[len++] = '\\'; break;
			case '/': result[len++] = '/'; break;
			case 'u': {
				if (!p[1] || !p[2] || !p[3] || !p[4]) { result[len++] = '\\'; result[len++] = 'u'; break; }
				unsigned cp = 0; int ok = 1;
				for (int i = 1; i <= 4; i++) {
					char c = p[i]; cp <<= 4;
					if      (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
					else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
					else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
					else { ok = 0; break; }
				}
				if (!ok) { result[len++] = '\\'; result[len++] = 'u'; break; }
				p += 4;
				if (cp < 0x80) {
					result[len++] = (char)cp;
				} else if (cp < 0x800) {
					result[len++] = (char)(0xC0 | (cp >> 6));
					result[len++] = (char)(0x80 | (cp & 0x3F));
				} else {
					result[len++] = (char)(0xE0 | (cp >> 12));
					result[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
					result[len++] = (char)(0x80 | (cp & 0x3F));
				}
				break;
			}
			default: result[len++] = '\\'; result[len++] = *p; break;
			}
		} else {
			result[len++] = *p;
		}
		p++;
	}
	result[len] = '\0';
	return result;
}

static char *aiExtractApiError(const char *json) {
	const char *err = strstr(json, "\"error\"");
	const char *msg = err ? strstr(err, "\"message\":\"") : NULL;
	if (msg) msg += 11;
	else if ((msg = strstr(json, "\"detail\":\"")) != NULL) msg += 10;   /* chatgpt.com errors */
	else return NULL;
	char *body = aiExtractStringValue(msg);
	if (!body) return NULL;
	int blen = strlen(body);
	char *result = malloc(blen + 8);
	snprintf(result, blen + 8, "Error: %s", body);
	free(body);
	return result;
}

static char *aiExtractAnthropicText(const char *json) {
	const char *p = json;
	int cap = 0, len = 0;
	char *out = NULL;
	while ((p = strstr(p, "\"type\":\"text\""))) {
		const char *block_start = p;
		while (block_start > json && *block_start != '{') block_start--;
		const char *tk = strstr(block_start, "\"text\":\"");
		if (!tk || tk > p + 256) { p++; continue; }
		tk += 8;
		char *seg = aiExtractStringValue(tk);
		if (!seg) { p++; continue; }
		int slen = strlen(seg);
		if (!out) { cap = slen + 64; out = malloc(cap); }
		else if (len + slen + 4 >= cap) { while (cap < len + slen + 4) cap *= 2; out = realloc(out, cap); }
		memcpy(out + len, seg, slen); len += slen;
		out[len] = '\0';
		free(seg);
		p++;
	}
	return out;
}

static char *aiExtractResponse(const char *json, enum aiProviderType type, int *is_err) {
	*is_err = 0;
	if (!json) return NULL;

	if (type == AI_PROVIDER_ANTHROPIC) {
		char *t = aiExtractAnthropicText(json);
		if (t && *t) return t;
		free(t);
		char *e = aiExtractApiError(json);
		*is_err = e != NULL;
		return e;
	}

	const char *start = strstr(json, "\"content\":\"");
	if (!start) {
		char *e = aiExtractApiError(json);
		*is_err = e != NULL;
		return e;
	}
	start += strlen("\"content\":\"");
	return aiExtractStringValue(start);
}

static char *hkExtractContentArray(const char *response) {
	const char *p = strstr(response, "\"content\":[");
	if (!p) return NULL;
	p += strlen("\"content\":");
	const char *start = p;
	int depth = 0, in_str = 0, esc = 0;
	while (*p) {
		if (esc) { esc = 0; p++; continue; }
		if (*p == '\\') { esc = 1; p++; continue; }
		if (*p == '"') in_str = !in_str;
		else if (!in_str) {
			if (*p == '[') depth++;
			else if (*p == ']') { depth--; if (depth == 0) { p++; break; } }
		}
		p++;
	}
	int len = p - start;
	char *out = malloc(len + 1);
	memcpy(out, start, len);
	out[len] = '\0';
	return out;
}

static char *hkBuildToolResults(aiData *data, const char *content_array) {
	char *out = malloc(32);
	int cap = 32, len = 0;
	out[0] = '[';
	len = 1;
	const char *p = content_array;
	int first = 1;
	while ((p = strstr(p, "\"type\":\"tool_use\""))) {
		const char *block_start = p;
		while (block_start > content_array && *block_start != '{') block_start--;
		char *id = hkExtractJsonString(block_start, "id");
		char *name = hkExtractJsonString(block_start, "name");
		char *input_obj = hkExtractJsonObject(block_start, "input");
		if (!id || !name || !input_obj) {
			free(id); free(name); free(input_obj);
			p++; continue;
		}
		hkAnnounceTool(data, name, input_obj);
		char *result = hkExecTool(name, input_obj);
		hkAnnounceToolResult(data, result);
		int rlen = strlen(result);
		char *esc = malloc(rlen * 6 + 8);
		hkJsonEscapeInto(result, esc, rlen * 6 + 8);
		int need = len + strlen(id) + strlen(esc) + 128;
		if (need >= cap) { while (cap < need) cap *= 2; out = realloc(out, cap); }
		if (!first) out[len++] = ',';
		first = 0;
		len += snprintf(out + len, cap - len,
			"{\"type\":\"tool_result\",\"tool_use_id\":\"%s\",\"content\":\"%s\"}",
			id, esc);
		free(id); free(name); free(input_obj); free(result); free(esc);
		p++;
	}
	if (len + 2 >= cap) { cap += 4; out = realloc(out, cap); }
	out[len++] = ']';
	out[len] = '\0';
	return out;
}

static char *hkExtractRawJsonArray(const char *src, const char *key) {
	char needle[64];
	snprintf(needle, sizeof(needle), "\"%s\"", key);
	const char *p = src;
	for (;;) {
		p = strstr(p, needle);
		if (!p) return NULL;
		const char *q = p + strlen(needle);
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		if (*q != ':') { p++; continue; }
		q++;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		if (*q != '[') { p++; continue; }
		p = q;
		break;
	}
	const char *start = p;
	int depth = 0, in_str = 0, esc = 0;
	while (*p) {
		if (esc) { esc = 0; p++; continue; }
		if (*p == '\\') { esc = 1; p++; continue; }
		if (*p == '"') { in_str = !in_str; p++; continue; }
		if (!in_str) {
			if (*p == '[') depth++;
			else if (*p == ']') { depth--; if (depth == 0) { p++; break; } }
		}
		p++;
	}
	size_t n = p - start;
	char *out = malloc(n + 1);
	memcpy(out, start, n);
	out[n] = '\0';
	return out;
}

static int hkDupCall(char ***seen, int *n, int *cap, const char *name, const char *args) {
	if (!name) return 0;
	if (!args) args = "";
	size_t need = strlen(name) + 1 + strlen(args) + 1;
	char *sig = malloc(need);
	if (!sig) return 0;
	snprintf(sig, need, "%s\037%s", name, args);
	for (int i = 0; i < *n; i++) if (!strcmp((*seen)[i], sig)) { free(sig); return 1; }
	if (*n == *cap) {
		int nc = *cap ? *cap * 2 : 8;
		char **t = realloc(*seen, (size_t)nc * sizeof(char *));
		if (!t) { free(sig); return 0; }
		*seen = t; *cap = nc;
	}
	(*seen)[(*n)++] = sig;
	return 0;
}

static int hkHasToolBlock(const char *s) {
	return s && (strstr(s, "<tool") || strstr(s, "<invoke name=") || strstr(s, "<write_file"));
}

/* ---------- OpenAI-compatible streaming ---------- */

#define HK_SSE_CALLS 16

typedef struct {
	char  *text;
	size_t text_len, text_cap;
	char  *call_id[HK_SSE_CALLS];
	char  *call_name[HK_SSE_CALLS];
	char  *call_args[HK_SSE_CALLS];      /* kept ESCAPED: fragments of a JSON string */
	size_t call_args_len[HK_SSE_CALLS], call_args_cap[HK_SSE_CALLS];
	int    call_count;
	int    prompt_tokens, completion_tokens;
	int    done;
} hkSseAcc;

static void hkSseFree(hkSseAcc *a) {
	free(a->text);
	for (int i = 0; i < HK_SSE_CALLS; i++) {
		free(a->call_id[i]); free(a->call_name[i]); free(a->call_args[i]);
	}
	memset(a, 0, sizeof(*a));
}

/* Returns the newly arrived plain text for live display, or NULL. Caller frees. */
static char *hkSseFeed(hkSseAcc *a, const char *line) {
	if (strncmp(line, "data:", 5) != 0) return NULL;
	const char *payload = line + 5;
	while (*payload == ' ') payload++;
	if (strncmp(payload, "[DONE]", 6) == 0) { a->done = 1; return NULL; }

	/* Usage may ride the final chunk; some gateways put it on every one. */
	const char *us = strstr(payload, "\"usage\"");
	if (us) {
		int p = hkExtractJsonInt(us, "prompt_tokens");
		int c = hkExtractJsonInt(us, "completion_tokens");
		if (p > 0) a->prompt_tokens = p;
		if (c > 0) a->completion_tokens = c;
	}

	const char *tc = strstr(payload, "\"tool_calls\"");
	if (tc) {
		int idx = hkExtractJsonInt(tc, "index");
		if (idx < 0) idx = 0;
		if (idx < HK_SSE_CALLS) {
			if (idx + 1 > a->call_count) a->call_count = idx + 1;
			char *id = hkExtractJsonString(tc, "id");
			if (id && !a->call_id[idx]) a->call_id[idx] = id; else free(id);
			char *nm = hkExtractJsonString(tc, "name");
			if (nm && !a->call_name[idx]) a->call_name[idx] = nm; else free(nm);
			char *ar = hkExtractJsonString(tc, "arguments");
			if (ar) {
				hkSbAdd(&a->call_args[idx], &a->call_args_len[idx],
				        &a->call_args_cap[idx], ar, strlen(ar));
				free(ar);
			}
		}
	}

	/* The first chunk carries "content":null, which is not a JSON string and so
	   never matches; only real text gets here. */
	const char *dl = strstr(payload, "\"delta\"");
	if (!dl) return NULL;
	char *raw = hkExtractJsonString(dl, "content");
	if (!raw) return NULL;
	char *plain = hkJsonUnescape(raw, (int)strlen(raw));
	free(raw);
	if (!plain) return NULL;
	size_t n = strlen(plain);
	if (n) hkSbAdd(&a->text, &a->text_len, &a->text_cap, plain, n);
	if (!n) { free(plain); return NULL; }
	return plain;
}

/* Rebuild the non-streaming response body the rest of the worker expects. */
static char *hkSseFinish(hkSseAcc *a) {
	size_t esc_cap = a->text_len * 6 + 16;
	char *esc = malloc(esc_cap);
	if (!esc) return NULL;
	hkJsonEscapeInto(a->text ? a->text : "", esc, (int)esc_cap);

	char  *out = NULL;
	size_t len = 0, cap = 0;
	static const char HEAD[] = "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"";
	hkSbAdd(&out, &len, &cap, HEAD, sizeof(HEAD) - 1);
	hkSbAdd(&out, &len, &cap, esc, strlen(esc));
	hkSbAdd(&out, &len, &cap, "\"", 1);
	free(esc);

	int emitted = 0;
	for (int i = 0; i < a->call_count; i++) {
		if (!a->call_name[i]) continue;
		static const char TC_OPEN[] = ",\"tool_calls\":[";
		if (emitted) hkSbAdd(&out, &len, &cap, ",", 1);
		else         hkSbAdd(&out, &len, &cap, TC_OPEN, sizeof(TC_OPEN) - 1);
		const char *id = a->call_id[i] ? a->call_id[i] : "call_0";
		hkSbAdd(&out, &len, &cap, "{\"id\":\"", strlen("{\"id\":\""));
		hkSbAdd(&out, &len, &cap, id, strlen(id));
		{ static const char M[] = "\",\"type\":\"function\",\"function\":{\"name\":\""; hkSbAdd(&out, &len, &cap, M, sizeof(M) - 1); }
		hkSbAdd(&out, &len, &cap, a->call_name[i], strlen(a->call_name[i]));
		{ static const char M[] = "\",\"arguments\":\""; hkSbAdd(&out, &len, &cap, M, sizeof(M) - 1); }
		if (a->call_args[i]) hkSbAdd(&out, &len, &cap, a->call_args[i], a->call_args_len[i]);
		hkSbAdd(&out, &len, &cap, "\"}}", 3);
		emitted++;
	}
	if (emitted) hkSbAdd(&out, &len, &cap, "]", 1);

	char tail[192];
	snprintf(tail, sizeof(tail),
		"},\"finish_reason\":\"%s\"}],\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d}}",
		emitted ? "tool_calls" : "stop", a->prompt_tokens, a->completion_tokens);
	hkSbAdd(&out, &len, &cap, tail, strlen(tail));
	return out;
}

/* ---------- context accounting and compaction ---------- */

#define HK_CTX_TRIGGER   70    /* percent of the window before compacting */
#define HK_CTX_KEEP_HEAD  1    /* the opening request states the goal */
#define HK_CTX_KEEP_TAIL  6    /* recent turns stay verbatim */
#define HK_CTX_DIGEST_MAX 24   /* lines of digest */

/* Four bytes per token is close enough to steer with, and costs nothing. Real
   tokenisers disagree with each other anyway. */
static long hkEstimateTokens(const char *s) {
	return s ? (long)((strlen(s) + 3) / 4) : 0;
}

static long hkContextWindow(void) {
	/* A custom endpoint can be fronting anything; the table below is a guess
	   and the user is allowed to correct it. */
	if (E.ai_context_window > 0) return E.ai_context_window;
	const char *m = E.ai_model ? E.ai_model : "";
	switch (E.ai_provider_type) {
	case AI_PROVIDER_MITHRAEUM: {             /* what the engine is started with */
		const char *ctx = getenv("HAKO_CTX");
		if (!ctx || atol(ctx) <= 0) ctx = getenv("HAKO_HAKM_CTX");
		long v = ctx ? atol(ctx) : 0;
		return v > 0 ? v : (sizeof(void *) == 4 ? 4096 : 8192);
	}
	case AI_PROVIDER_ANTHROPIC:
		if (strstr(m, "haiku")) return 200000;
		if (strstr(m, "claude-3") || strstr(m, "-4-5")) return 200000;
		return 1000000;
	case AI_PROVIDER_OLLAMA:    return 8192;
	case AI_PROVIDER_OPENAI:
		if (strstr(m, "gpt-3.5"))  return 16385;
		if (strstr(m, "gpt-4-"))   return 8192;
		if (strstr(m, "gemini"))   return 1000000;
		if (strstr(m, "claude")) {
			if (strstr(m, "haiku") || strstr(m, "claude-3")) return 200000;
			return 1000000;
		}
		return 128000;
	default: return 128000;
	}
}

static long hkContextUsed(aiData *data) {
	long t = hkEstimateTokens(data->system_prompt);
	for (int i = 0; i < data->message_count; i++)
		t += hkEstimateTokens(data->messages[i].content) + 4;
	return t;
}

static int hkContextPercent(aiData *data) {
	long w = hkContextWindow();
	if (w <= 0) return 0;
	return (int)((hkContextUsed(data) * 100) / w);
}

/* One line describing a message, for the digest. */
static void hkDigestLine(char **b, size_t *l, size_t *c, const aiMessage *m) {
	const char *role = m->role ? m->role : "?";
	const char *s = m->content ? m->content : "";
	while (*s == ' ' || *s == '\n' || *s == '\t') s++;
	/* Tool observations are the useful part of the middle: they say what was
	   actually looked at. Name the tool rather than quoting its output. */
	if (!strncmp(s, "<observation", 12)) {
		const char *q = strstr(s, "tool=\"");
		char tool[64] = "tool";
		if (q) {
			q += 6;
			size_t k = 0;
			while (*q && *q != '"' && k < sizeof(tool) - 1) tool[k++] = *q++;
			tool[k] = '\0';
		}
		char line[128];
		int n = snprintf(line, sizeof(line), "  - ran %s\n", tool);
		hkSbAdd(b, l, c, line, (size_t)n);
		return;
	}
	size_t n = 0;
	while (s[n] && s[n] != '\n' && n < 96) n++;
	if (!n) return;
	char line[192];
	int k = snprintf(line, sizeof(line), "  - %s: %.*s%s\n",
	                 !strcmp(role, "user") ? "you" : "me", (int)n, s,
	                 s[n] && s[n] != '\n' ? "..." : "");
	hkSbAdd(b, l, c, line, (size_t)k);
}

/* Returns the number of messages folded away, 0 if nothing was done. Caller
   must NOT hold data->lock. */
static int hkCompactHistory(aiData *data) {
	pthread_mutex_lock(&data->lock);
	int n = data->message_count;
	if (n < HK_CTX_KEEP_HEAD + HK_CTX_KEEP_TAIL + 3) {
		pthread_mutex_unlock(&data->lock);
		return 0;
	}
	int head = HK_CTX_KEEP_HEAD;
	int cut  = n - HK_CTX_KEEP_TAIL;

	while (cut < n && !(data->messages[cut].role &&
	                    !strcmp(data->messages[cut].role, "user") &&
	                    !data->messages[cut].raw)) cut++;
	if (cut >= n || cut <= head + 1) {
		pthread_mutex_unlock(&data->lock);
		return 0;
	}

	char  *dig = NULL;
	size_t dl = 0, dc = 0;
	char intro[96];
	int in = snprintf(intro, sizeof(intro),
	                  "[earlier conversation compacted — %d messages]\n", cut - head);
	hkSbAdd(&dig, &dl, &dc, intro, (size_t)in);
	int lines = 0;
	for (int i = head; i < cut && lines < HK_CTX_DIGEST_MAX; i++, lines++)
		hkDigestLine(&dig, &dl, &dc, &data->messages[i]);
	if (cut - head > HK_CTX_DIGEST_MAX) {
		char more[64];
		int mn = snprintf(more, sizeof(more), "  - ... %d more\n",
		                  (cut - head) - HK_CTX_DIGEST_MAX);
		hkSbAdd(&dig, &dl, &dc, more, (size_t)mn);
	}

	size_t original = 0;
	for (int i = head; i < cut; i++)
		original += data->messages[i].content ? strlen(data->messages[i].content) : 0;
	if (!dig || dl >= original) {
		free(dig);
		pthread_mutex_unlock(&data->lock);
		return 0;
	}

	for (int i = head; i < cut; i++) {
		free(data->messages[i].role);
		free(data->messages[i].content);
	}
	/* One digest message takes the place of everything folded away. */
	data->messages[head].role    = strdup("user");
	data->messages[head].content = dig;
	data->messages[head].raw     = 0;
	int removed = cut - head - 1;
	if (removed > 0) {
		memmove(data->messages + head + 1, data->messages + cut,
		        (size_t)(n - cut) * sizeof(aiMessage));
		data->message_count = n - removed;
	}
	pthread_mutex_unlock(&data->lock);
	return cut - head;
}

/* A model sometimes emits the tool JSON with only the CLOSING tag: */
static char *hkRepairOrphanToolClose(const char *s) {
	if (!s) return NULL;
	const char *close = strstr(s, "</tool_call>");
	if (!close) return NULL;
	const char *open = strstr(s, "<tool_call>");
	if (open && open < close) return NULL;        /* well-formed, leave it */
	const char *brace = memchr(s, '{', (size_t)(close - s));
	if (!brace) return NULL;
	const char *nm = strstr(brace, "\"name\"");
	if (!nm || nm > close) return NULL;
	const char *e = close;
	while (e > brace && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
	if (e == brace || e[-1] != '}') return NULL;
	size_t head = (size_t)(brace - s), rest = strlen(brace);
	char *out = malloc(head + 11 + rest + 1);
	if (!out) return NULL;
	memcpy(out, s, head);
	memcpy(out + head, "<tool_call>", 11);
	memcpy(out + head + 11, brace, rest + 1);
	return out;
}

/* The prose dialect asks the provider to stop AT `</tool>` (so the model cannot
   go on to invent the result), and a stop sequence is not part of the text it
   returns. Put the closing tag back on a call that was cut there. */
static char *hkCloseStoppedTool(const char *s) {
	const char *open = NULL;
	for (const char *p = strstr(s, "<tool name=\""); p; p = strstr(p + 1, "<tool name=\"")) open = p;
	if (!open || strstr(open, "</tool>") || !strchr(open, '}')) return NULL;
	size_t n = strlen(s);
	char *o = malloc(n + 9);
	if (!o) return NULL;
	memcpy(o, s, n);
	memcpy(o + n, "\n</tool>", 9);
	return o;
}

static char *hkNormalizeToolDialect(const char *s) {
	if (!s) return NULL;
	/* Repair first, then normalise — a repaired string still has to go
	   through the dialect rewrites below. */
	char *pre = NULL;
	for (int i = 0; i < 8; i++) {
		char *r = hkRepairOrphanToolClose(pre ? pre : s);
		if (!r) break;
		free(pre);
		pre = r;
	}
	{
		char *c = hkCloseStoppedTool(pre ? pre : s);
		if (c) { free(pre); pre = c; }
	}
	if (pre) s = pre;
	int needs = strstr(s, "<tool_name=") || strstr(s, "</tool_name>")
	         || strstr(s, "<observation") || strstr(s, "<parameter name=")
	         || strstr(s, "</invoke>");
	if (!needs) return pre;    /* NULL when there was nothing to do at all */

	/* Grown as it goes: escaping can make a value six times longer (\u00XX), so
	   no size computed up front is safe. */
	char *out = NULL; size_t o = 0, cap = 0;
	if (hkSbAdd(&out, &o, &cap, "", 0) != 0) { free(pre); return NULL; }
	int in_wrap = 0, inv_named = 0;
	for (const char *p = s; *p; ) {
		if (!strncmp(p, "<tool_name=", 11)) { hkSbAdd(&out, &o, &cap, "<tool name=", 11); in_wrap = 1; p += 11; continue; }
		if (!strncmp(p, "</tool_name>", 12)) { hkSbAdd(&out, &o, &cap, "</tool>", 7); in_wrap = 0; p += 12; continue; }
		if (!strncmp(p, "</invoke>", 9)) {
			if (inv_named) hkSbAdd(&out, &o, &cap, "</tool>", 7);
			inv_named = 0; p += 9; continue;
		}
		if (!strncmp(p, "<invoke ", 8) || !strncmp(p, "<invoke>", 8)) {
			const char *gt = strchr(p, '>');
			/* A bare <invoke name="x"> carries the only copy of the tool name. */
			const char *nm = !in_wrap ? strstr(p, "name=\"") : NULL;
			const char *ne = nm ? strchr(nm + 6, '"') : NULL;
			if (gt && nm && nm < gt && ne && ne < gt && ne - (nm + 6) <= 64) {
				hkSbAdd(&out, &o, &cap, "<tool name=\"", 12);
				hkSbAdd(&out, &o, &cap, nm + 6, (size_t)(ne - (nm + 6)));
				hkSbAdd(&out, &o, &cap, "\">", 2);
				inv_named = 1;
			}
			p = gt ? gt + 1 : p + 8;
			continue;
		}
		if (!strncmp(p, "<observation", 12)) {
			const char *e = strstr(p, "</observation>");
			p = e ? e + 14 : p + strlen(p);
			continue;
		}
		if (!strncmp(p, "<parameter name=\"", 17)) {
			const char *p0 = p;
			size_t o0 = o;
			int first = 1;
			hkSbAdd(&out, &o, &cap, "{", 1);
			while (!strncmp(p, "<parameter name=\"", 17)) {
				const char *k = p + 17;
				const char *ke = strchr(k, '"');
				if (!ke) break;
				const char *vs = strchr(ke, '>');
				if (!vs) break;
				vs++;
				const char *ve = strstr(vs, "</parameter>");
				if (!ve) break;
				if (!first) hkSbAdd(&out, &o, &cap, ", ", 2);
				first = 0;
				hkSbAdd(&out, &o, &cap, "\"", 1);
				hkSbAdd(&out, &o, &cap, k, (size_t)(ke - k));
				hkSbAdd(&out, &o, &cap, "\": \"", 4);
				size_t vl = (size_t)(ve - vs);
				char *val = malloc(vl + 1);
				char *esc = malloc(vl * 6 + 8);
				if (val && esc) {
					memcpy(val, vs, vl);
					val[vl] = '\0';
					hkJsonEscapeInto(val, esc, (int)(vl * 6 + 8));
					hkSbAdd(&out, &o, &cap, esc, strlen(esc));
				}
				free(val); free(esc);
				hkSbAdd(&out, &o, &cap, "\"", 1);
				p = ve + 12;
				while (*p == '\n' || *p == ' ' || *p == '\t' || *p == '\r') p++;
			}
			if (p == p0) {                       /* malformed: keep it as text, move on */
				o = o0; out[o] = '\0';
				hkSbAdd(&out, &o, &cap, p, 1);
				p++;
				continue;
			}
			hkSbAdd(&out, &o, &cap, "}", 1);
			continue;
		}
		hkSbAdd(&out, &o, &cap, p, 1);
		p++;
	}
	free(pre);
	return out;
}

static void hkPushDupNotice(aiData *data, const char *name) {
	char msg[256];
	snprintf(msg, sizeof(msg),
		"<observation tool=\"%s\">(repeat) you already ran this exact call this turn — "
		"its result is above. Do NOT repeat it. Use what you have or give your final answer."
		"</observation>", name ? name : "");
	pthread_mutex_lock(&data->lock);
	aiPushMessage(data, "user", msg);
	pthread_mutex_unlock(&data->lock);
}

static int hkRunOneTool(aiData *data, const char *name, const char *args,
	char ***seenp, int *seen_np, int *seen_capp, int *dups) {
	if (hkDupCall(seenp, seen_np, seen_capp, name, args)) {
		hkPushDupNotice(data, name); if (dups) (*dups)++;
		return 0;
	}
	hkAnnounceTool(data, name, args);
	char *result = hkExecTool(name, args);
	hkAnnounceToolResult(data, result);
	size_t rlen = result ? strlen(result) : 0;
	size_t nl = strlen(name);
	char *obs = malloc(rlen + nl + 64);
	if (obs) {
		snprintf(obs, rlen + nl + 64, "<observation tool=\"%s\">%s</observation>",
			name, result ? result : "");
		pthread_mutex_lock(&data->lock);
		aiPushMessage(data, "user", obs);
		pthread_mutex_unlock(&data->lock);
		free(obs);
	}
	free(result);
	return 1;
}

static char *hkParenArgsToJson(const char *s, char *nameout, size_t namecap) {
	const char *lp = strchr(s, '(');
	if (!lp) return NULL;
	const char *rp = strrchr(lp, ')');
	if (!rp || rp <= lp) return NULL;
	const char *ne = lp; while (ne > s && (ne[-1]==' '||ne[-1]=='\t')) ne--;
	const char *ns = s;  while (ns < ne && (*ns==' '||*ns=='\t'||*ns=='\n'||*ns=='\r')) ns++;
	size_t nlen = (size_t)(ne - ns);
	if (nlen == 0 || nlen >= namecap) return NULL;
	memcpy(nameout, ns, nlen); nameout[nlen] = '\0';

	size_t inlen = (size_t)(rp - lp - 1);
	size_t cap = inlen * 6 + 16;
	char *json = malloc(cap);
	if (!json) return NULL;
	size_t jl = 0;
	json[jl++] = '{';
	const char *p = lp + 1;
	int first = 1;
	while (p < rp) {
		while (p < rp && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p==',')) p++;
		if (p >= rp) break;
		const char *ks = p;
		while (p < rp && *p != '=' && *p != ',') p++;
		if (p >= rp || *p != '=') break;
		const char *ke = p; while (ke > ks && (ke[-1]==' '||ke[-1]=='\t')) ke--;
		p++;
		while (p < rp && (*p==' '||*p=='\t')) p++;
		const char *vs, *ve;
		if (p < rp && (*p=='"' || *p=='\'')) {
			char q = *p; p++; vs = p;
			while (p < rp && *p != q) { if (*p=='\\' && p+1 < rp) p++; p++; }
			ve = p; if (p < rp) p++;
		} else {
			vs = p;
			while (p < rp && *p != ',') p++;
			ve = p; while (ve > vs && (ve[-1]==' '||ve[-1]=='\t')) ve--;
		}
		if (ke == ks) continue;
		if (!first && jl < cap-1) json[jl++] = ',';
		first = 0;
		if (jl < cap-1) json[jl++] = '"';
		for (const char *k=ks; k<ke && jl<cap-2; k++) { char c=*k; if (c=='"'||c=='\\') json[jl++]='\\'; json[jl++]=c; }
		if (jl < cap-3) { json[jl++]='"'; json[jl++]=':'; }
		if (jl < cap-1) json[jl++] = '"';
		/* The value came from a QUOTED string, so a backslash in it is already
		   an escape the model meant: content="a\nb" is a newline, not the two
		   characters. Doubling it produced a literal \n in every file written
		   through this dialect. Pass a valid escape through untouched; only a
		   lone backslash (not introducing one) gets doubled. */
		for (const char *v=vs; v<ve && jl<cap-8; v++) {
			char c=*v;
			if (c=='\\' && v+1 < ve && strchr("ntrbf\"\\/u", v[1])) {
				json[jl++]='\\'; json[jl++]=v[1]; v++;
			}
			else if (c=='"'||c=='\\') { json[jl++]='\\'; json[jl++]=c; }
			else if (c=='\n') { json[jl++]='\\'; json[jl++]='n'; }
			else if (c=='\r') { json[jl++]='\\'; json[jl++]='r'; }
			else if (c=='\t') { json[jl++]='\\'; json[jl++]='t'; }
			else json[jl++]=c;
		}
		if (jl < cap-1) json[jl++] = '"';
	}
	if (jl < cap-1) json[jl++] = '}';
	json[jl] = '\0';
	if (first) { free(json); return NULL; }
	return json;
}

static int hkReactToolExecAll(aiData *data, const char *content,
	char ***xseen, int *xseen_n, int *xseen_cap, int *dups, int *truncated) {
	if (!data || !content) return 0;
	char *normalized = hkNormalizeToolDialect(content);
	if (normalized) content = normalized;
	int count = 0;
	char **seen = *xseen; int seen_n = *xseen_n, seen_cap = *xseen_cap;
	const char *cursor = content;
	while ((cursor = strstr(cursor, "<tool")) != NULL) {
		const char *name_attr = strstr(cursor, "name=\"");
		if (!name_attr || name_attr > cursor + 32) { cursor++; continue; }
		name_attr += 6;
		const char *name_end = strchr(name_attr, '"');
		if (!name_end) break;
		int nlen = (int)(name_end - name_attr);
		if (nlen <= 0 || nlen > 64) { cursor = name_end; continue; }
		char tname[80];
		memcpy(tname, name_attr, nlen); tname[nlen] = '\0';

		const char *open_end = strchr(name_end, '>');
		if (!open_end) break;
		const char *body = open_end + 1;
		const char *close_tag = strstr(body, "</tool>");
		if (!close_tag) break;

		while (body < close_tag && (*body == ' ' || *body == '\n' || *body == '\r' || *body == '\t')) body++;
		int blen = (int)(close_tag - body);
		while (blen > 0 && (body[blen-1] == ' ' || body[blen-1] == '\n' || body[blen-1] == '\r' || body[blen-1] == '\t')) blen--;
		char *args = malloc(blen + 1);
		if (!args) { cursor = close_tag + 7; continue; }
		memcpy(args, body, blen); args[blen] = '\0';

		if (hkDupCall(&seen, &seen_n, &seen_cap, tname, args)) {
			hkPushDupNotice(data, tname); if (dups) (*dups)++;
			free(args); cursor = close_tag + 7; continue;
		}
		hkAnnounceTool(data, tname, args);
		char *result = hkExecTool(tname, args);
		hkAnnounceToolResult(data, result);

		size_t rlen = result ? strlen(result) : 0;
		char *obs = malloc(rlen + nlen + 64);
		if (obs) {
			snprintf(obs, rlen + nlen + 64, "<observation tool=\"%s\">%s</observation>",
				tname, result ? result : "");
			pthread_mutex_lock(&data->lock);
			aiPushMessage(data, "user", obs);
			pthread_mutex_unlock(&data->lock);
			free(obs);
		}
		free(args); free(result);
		count++;
		cursor = close_tag + 7;
	}

	cursor = content;
	while ((cursor = strstr(cursor, "<tool>")) != NULL) {
		const char *name_start = cursor + 6;
		const char *name_end = strstr(name_start, "</tool>");
		if (!name_end) break;
		int rawlen = (int)(name_end - name_start);
		if (rawlen <= 0 || rawlen > 64) { cursor = name_end + 7; continue; }
		char tname[80];
		memcpy(tname, name_start, rawlen); tname[rawlen] = '\0';
		char *ts = tname;
		while (*ts == ' ' || *ts == '\n' || *ts == '\r' || *ts == '\t') ts++;
		char *te = ts + strlen(ts);
		while (te > ts && (te[-1]==' '||te[-1]=='\n'||te[-1]=='\r'||te[-1]=='\t')) te--;
		*te = '\0';

		if (strchr(ts, '(')) {
			char pname[80];
			char *pargs = hkParenArgsToJson(ts, pname, sizeof pname);
			if (pargs) {
				if (hkRunOneTool(data, pname, pargs, &seen, &seen_n, &seen_cap, dups)) count++;
				free(pargs);
				cursor = name_end + 7;
				continue;
			}
		}

		const char *p = name_end + 7;
		while (*p && *p != '{' && *p != '<') p++;
		if (*p != '{') { cursor = name_end + 7; continue; }
		int depth = 0, instr = 0, esc = 0;
		const char *jend = NULL;
		for (const char *q = p; *q; q++) {
			char c = *q;
			if (esc) { esc = 0; continue; }
			if (instr) { if (c == '\\') esc = 1; else if (c == '"') instr = 0; continue; }
			if (c == '"') instr = 1;
			else if (c == '{') depth++;
			else if (c == '}') { if (--depth == 0) { jend = q + 1; break; } }
		}
		if (!jend) { cursor = name_end + 7; continue; }
		int alen = (int)(jend - p);
		char *args = malloc((size_t)alen + 1);
		if (!args) { cursor = jend; continue; }
		memcpy(args, p, alen); args[alen] = '\0';

		if (!*ts) { free(args); cursor = jend; continue; }
		if (hkDupCall(&seen, &seen_n, &seen_cap, ts, args)) {
			hkPushDupNotice(data, ts); if (dups) (*dups)++;
			free(args); cursor = jend; continue;
		}
		hkAnnounceTool(data, ts, args);
		char *result = hkExecTool(ts, args);
		hkAnnounceToolResult(data, result);
		size_t rlen = result ? strlen(result) : 0;
		size_t nl = strlen(ts);
		char *obs = malloc(rlen + nl + 64);
		if (obs) {
			snprintf(obs, rlen + nl + 64, "<observation tool=\"%s\">%s</observation>",
				ts, result ? result : "");
			pthread_mutex_lock(&data->lock);
			aiPushMessage(data, "user", obs);
			pthread_mutex_unlock(&data->lock);
			free(obs);
		}
		free(args); free(result);
		count++;
		cursor = jend;
	}

	cursor = content;
	while ((cursor = strstr(cursor, "<invoke name=\"")) != NULL) {
		const char *name_start = cursor + 14;
		const char *name_end = strchr(name_start, '"');
		if (!name_end) break;
		int nlen = (int)(name_end - name_start);
		if (nlen <= 0 || nlen > 64) { cursor++; continue; }
		char tname[80];
		memcpy(tname, name_start, nlen); tname[nlen] = '\0';

		const char *inv_close = strstr(name_end, "</invoke>");
		if (!inv_close) break;

		/* Grown, not fixed: a fixed buffer cut a large write_file short, silently. */
		char *json = NULL; size_t jl = 0, jcap = 0;
		hkSbAdd(&json, &jl, &jcap, "{", 1);
		int first = 1;
		const char *pp = name_end;
		while (pp < inv_close) {
			const char *pn = strstr(pp, "<parameter name=\"");
			if (!pn || pn >= inv_close) break;
			pn += 17;
			const char *kend = strchr(pn, '"');
			const char *vstart = kend ? strchr(kend, '>') : NULL;
			const char *vend = vstart ? strstr(vstart, "</parameter>") : NULL;
			if (!kend || !vstart || !vend || vend > inv_close) break;
			vstart++;
			if (!first) hkSbAdd(&json, &jl, &jcap, ",", 1);
			hkSbAdd(&json, &jl, &jcap, "\"", 1);
			hkSbAdd(&json, &jl, &jcap, pn, (size_t)(kend - pn));
			hkSbAdd(&json, &jl, &jcap, "\":\"", 3);
			for (const char *v = vstart; v < vend; v++) {
				char c = *v;
				const char *e = c == '"' ? "\\\"" : c == '\\' ? "\\\\" : c == '\n' ? "\\n" :
				                c == '\r' ? "\\r" : c == '\t' ? "\\t" : NULL;
				if (e) hkSbAdd(&json, &jl, &jcap, e, 2);
				else   hkSbAdd(&json, &jl, &jcap, &c, 1);
			}
			hkSbAdd(&json, &jl, &jcap, "\"", 1);
			first = 0;
			pp = vend + 12;
		}
		if (hkSbAdd(&json, &jl, &jcap, "}", 1) != 0) { free(json); break; }

		if (hkDupCall(&seen, &seen_n, &seen_cap, tname, json)) {
			hkPushDupNotice(data, tname); if (dups) (*dups)++;
			free(json);
			cursor = inv_close + 9; continue;
		}
		hkAnnounceTool(data, tname, json);
		char *result = hkExecTool(tname, json);
		free(json);
		hkAnnounceToolResult(data, result);
		size_t rlen2 = result ? strlen(result) : 0;
		char *obs2 = malloc(rlen2 + nlen + 64);
		if (obs2) {
			snprintf(obs2, rlen2 + nlen + 64, "<observation tool=\"%s\">%s</observation>",
				tname, result ? result : "");
			pthread_mutex_lock(&data->lock);
			aiPushMessage(data, "user", obs2);
			pthread_mutex_unlock(&data->lock);
			free(obs2);
		}
		free(result);
		count++;
		cursor = inv_close + 9;
	}

	cursor = content;
	while ((cursor = strstr(cursor, "<tool_call>")) != NULL) {
		const char *body = cursor + 11;
		const char *close_tag = strstr(body, "</tool_call>");
		if (!close_tag) { if (truncated) *truncated = 1; break; }
		size_t blen2 = (size_t)(close_tag - body);
		char *blk = malloc(blen2 + 1);
		if (!blk) break;
		memcpy(blk, body, blen2); blk[blen2] = '\0';
		char *tname = hkExtractJsonString(blk, "name");
		char *targs = hkExtractJsonObject(blk, "arguments");
		if (tname && *tname && strlen(tname) <= 64) {
			const char *aj = targs ? targs : "{}";
			if (hkDupCall(&seen, &seen_n, &seen_cap, tname, aj)) {
				hkPushDupNotice(data, tname); if (dups) (*dups)++;
			} else {
				hkAnnounceTool(data, tname, aj);
				char *result = hkExecTool(tname, aj);
				hkAnnounceToolResult(data, result);
				size_t rl = result ? strlen(result) : 0;
				size_t nl = strlen(tname);
				char *obs3 = malloc(rl + nl + 64);
				if (obs3) {
					snprintf(obs3, rl + nl + 64, "<observation tool=\"%s\">%s</observation>",
						tname, result ? result : "");
					pthread_mutex_lock(&data->lock);
					aiPushMessage(data, "user", obs3);
					pthread_mutex_unlock(&data->lock);
					free(obs3);
				}
				free(result);
				count++;
			}
		} else if ((!tname || !*tname) && strchr(blk, '(')) {
			char pname[80];
			char *pargs = hkParenArgsToJson(blk, pname, sizeof pname);
			if (pargs) {
				if (hkRunOneTool(data, pname, pargs, &seen, &seen_n, &seen_cap, dups)) count++;
				free(pargs);
			}
		}
		free(tname); free(targs); free(blk);
		cursor = close_tag + 12;
	}

	cursor = content;
	while ((cursor = strstr(cursor, "<write_file")) != NULL) {
		const char *open_end = strchr(cursor, '>');
		if (!open_end) { if (truncated) *truncated = 1; break; }
		const char *close_tag = strstr(open_end + 1, "</write_file>");
		if (!close_tag) { if (truncated) *truncated = 1; break; }

		char *path = NULL;
		static const char *attrs[] = { "path=\"", "file=\"", "filename=\"", "name=\"" };
		for (int k = 0; k < 4 && !path; k++) {
			const char *pa = strstr(cursor, attrs[k]);
			if (!pa || pa >= open_end) continue;
			pa += strlen(attrs[k]);
			const char *pe = strchr(pa, '"');
			if (!pe || pe > open_end) continue;
			int pl = (int)(pe - pa);
			if (pl > 0 && pl <= 1024) { path = malloc((size_t)pl + 1); if (path) { memcpy(path, pa, pl); path[pl] = '\0'; } }
		}

		const char *body = open_end + 1;
		if (body + 1 < close_tag && body[0] == '\r' && body[1] == '\n') body += 2;
		else if (body < close_tag && *body == '\n') body++;

		if (!path) {
			const char *nl = memchr(body, '\n', (size_t)(close_tag - body));
			const char *e = nl ? nl : close_tag;
			while (e > body && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
			const char *s = body;
			while (s < e && (*s == ' ' || *s == '\t')) s++;
			int pl = (int)(e - s);
			if (pl > 0 && pl <= 1024) {
				path = malloc((size_t)pl + 1);
				if (path) { memcpy(path, s, pl); path[pl] = '\0'; body = nl ? nl + 1 : close_tag; }
			}
		}
		if (!path) { cursor = close_tag + 13; continue; }

		int plen = (int)strlen(path);
		int blen = (int)(close_tag - body);
		if (blen < 0) blen = 0;

		size_t jcap = (size_t)plen * 6 + (size_t)blen * 6 + 64;
		char *raw  = malloc((size_t)blen + 1);
		char *json = malloc(jcap);
		char *ep   = malloc((size_t)plen * 6 + 8);
		char *ec   = malloc((size_t)blen * 6 + 8);
		if (raw && json && ep && ec) {
			memcpy(raw, body, blen); raw[blen] = '\0';
			hkJsonEscapeInto(path, ep, plen * 6 + 8);
			hkJsonEscapeInto(raw,  ec, blen * 6 + 8);
			snprintf(json, jcap, "{\"path\":\"%s\",\"content\":\"%s\"}", ep, ec);
			if (hkDupCall(&seen, &seen_n, &seen_cap, "write_file", json)) {
				hkPushDupNotice(data, "write_file"); if (dups) (*dups)++;
			} else {
				hkAnnounceTool(data, "write_file", json);
				char *result = hkExecTool("write_file", json);
				hkAnnounceToolResult(data, result);
				size_t rl = result ? strlen(result) : 0;
				char *obs = malloc(rl + 80);
				if (obs) {
					snprintf(obs, rl + 80, "<observation tool=\"write_file\">%s</observation>",
						result ? result : "");
					pthread_mutex_lock(&data->lock);
					aiPushMessage(data, "user", obs);
					pthread_mutex_unlock(&data->lock);
					free(obs);
				}
				free(result);
				count++;
			}
		}
		free(path); free(raw); free(json); free(ep); free(ec);
		cursor = close_tag + 13;
	}

	*xseen = seen; *xseen_n = seen_n; *xseen_cap = seen_cap;
	free(normalized);
	return count;
}

static int hkFnToolExecAll(aiData *data, const char *response) {
	char *tool_calls_raw = hkExtractRawJsonArray(response, "tool_calls");
	if (!tool_calls_raw) return 0;

	pthread_mutex_lock(&data->lock);
	const char *txt = data->current_response;
	int has_txt = txt && *txt;
	size_t tlen = strlen(tool_calls_raw);
	size_t txtlen = has_txt ? strlen(txt) : 0;
	size_t bcap = tlen + txtlen * 6 + 128;
	char *body = malloc(bcap);
	if (has_txt) {
		char *esc = malloc(txtlen * 6 + 8);
		hkJsonEscapeInto(txt, esc, txtlen * 6 + 8);
		snprintf(body, bcap, "\"content\":\"%s\",\"tool_calls\":%s", esc, tool_calls_raw);
		free(esc);
	} else {
		snprintf(body, bcap, "\"content\":null,\"tool_calls\":%s", tool_calls_raw);
	}
	aiPushMessageBody(data, "assistant", body);
	pthread_mutex_unlock(&data->lock);
	free(body);
	free(tool_calls_raw);

	int count = 0;
	const char *p = strstr(response, "\"tool_calls\":[");
	if (!p) return 0;
	char *seen_ids[32] = {0};
	int n_seen = 0;
	while ((p = strstr(p, "\"function\""))) {
		const char *obj_start = p;
		while (obj_start > response && *obj_start != '{') obj_start--;
		char *tc_id = hkExtractJsonString(obj_start, "id");
		if (tc_id) {
			int dup = 0;
			for (int i = 0; i < n_seen; i++) if (!strcmp(seen_ids[i], tc_id)) { dup = 1; break; }
			if (dup) { free(tc_id); p++; continue; }
			if (n_seen < 32) seen_ids[n_seen++] = strdup(tc_id);
		}
		char *fname = hkExtractJsonString(p, "name");
		char *args_obj = hkExtractJsonObject(p, "arguments");
		if (!args_obj) {
			char *args_str = hkExtractJsonString(p, "arguments");
			if (args_str) {
				args_obj = hkJsonUnescape(args_str, (int)strlen(args_str));
				free(args_str);
			}
		}
		if (!fname || !args_obj) { free(tc_id); free(fname); free(args_obj); p++; continue; }
		hkAnnounceTool(data, fname, args_obj);
		char *result = hkExecTool(fname, args_obj);
		hkAnnounceToolResult(data, result);

		const char *r = result ? result : "";
		size_t rlen = strlen(r);
		char *resc = malloc(rlen * 6 + 8);
		hkJsonEscapeInto(r, resc, rlen * 6 + 8);
		size_t idlen = tc_id ? strlen(tc_id) : 0;
		size_t reslen = strlen(resc);
		size_t tcap = idlen + reslen + 96;
		char *tbody = malloc(tcap);
		if (tc_id) {
			snprintf(tbody, tcap, "\"tool_call_id\":\"%s\",\"content\":\"%s\"", tc_id, resc);
		} else {
			snprintf(tbody, tcap, "\"content\":\"%s\"", resc);
		}
		pthread_mutex_lock(&data->lock);
		aiPushMessageBody(data, "tool", tbody);
		pthread_mutex_unlock(&data->lock);
		free(tbody); free(resc);
		free(tc_id); free(fname); free(args_obj); free(result);
		count++;
		p++;
	}
	for (int i = 0; i < n_seen; i++) free(seen_ids[i]);
	return count;
}

static void *clAnimThread(void *arg) {
	hkBlockWinch();
	aiData *data = (aiData *)arg;
	const clAnim *a = &CL_ANIMS[data->anim_style % CL_ANIM_COUNT];
	const char *label0 = data->anim_verb ? data->anim_verb
	                                     : CL_LABELS[data->anim_label % CL_LABEL_COUNT];
	const char *col = (E.anim_force_style >= 0) ? *a->color : TH_META;
	/* The title carries the mark, and turns into progress while the model
	   works — a backgrounded window still shows the turn is alive. */
	int title_ok = isatty(STDOUT_FILENO) && !E.pipe_mode && !E.output_json;
	long t0 = hk_time_ms();
	int i = 0;
	while (data->animating) {
		const char *fr = a->frames[i % a->frame_count];
		/* Re-read every frame: a turn runs several tools, and the line should
		   name the one actually in flight rather than whatever it opened on. */
		char label[288];   /* verb + subject + two colour escapes */
		const char *verb = hk_cur_tool ? hkTurnVerb(hk_cur_tool, NULL) : label0;
		if (hk_cur_subject[0]) {
			char subj[128];
			snprintf(subj, sizeof(subj), "%s", hk_cur_subject);
			/* A long path is only useful at its tail. */
			if (clCellWidth(subj) > 44) {
				char *sl = strrchr(subj, '/');
				if (sl && sl[1]) memmove(subj, sl + 1, strlen(sl + 1) + 1);
				if (clCellWidth(subj) > 44) clClipToCells(subj, 44);
			}
			snprintf(label, sizeof(label), "%s %s%s%s", verb, TH_META, subj, col);
		} else {
			snprintf(label, sizeof(label), "%s", verb);
		}
		long secs = (hk_time_ms() - t0) / 1000;
		char el[32];
		el[0] = '\0';
		if (secs >= 60)     snprintf(el, sizeof(el), " %ldm %lds", secs / 60, secs % 60);
		else if (secs >= 2) snprintf(el, sizeof(el), " %lds", secs);
		int cols = clTermCols();
		char body[192];
		if (E.color_enabled)
			snprintf(body, sizeof(body), "  %s%s %.140s…%s%s", col, fr, label, el, ANSI_RESET);
		else
			snprintf(body, sizeof(body), "  %s %.140s…%s", fr, label, el);
		if (title_ok && (i & 1) == 0)
			printf("\x1b]0;%s hako\x07", FRM_TITLE[(i / 2) % 2]);
		pthread_once(&cl_tty_once, clTtyLockMake);
		if (!clResizing() && pthread_mutex_trylock(&cl_tty_lock) == 0) {
			if (!clBarStatus(body)) {
				int bw = clCellWidth(body);
				if (bw > cols - 1) { clClipToCells(body, cols - 1); bw = clCellWidth(body); }
				printf("\r%s%*s\r", body, cols - 1 - bw, "");
				fflush(stdout);
			}
			clTtyUnlock();
		}
		cl_sleep_ms(a->delay_ms);
		i++;
	}
	clTtyLock();
	if (!clBarStatus("") && !clResizing()) {
		int cols = clTermCols();
		printf("\r%*s\r", cols - 1, "");
	}
	clTtyUnlock();
	if (title_ok) printf("\x1b]0;" HK_MARK " hako\x07");
	fflush(stdout);
	return NULL;
}

static void clStartAnim(aiData *data) {
	if (!E.color_enabled) {
		return;
	}
	if (data->animating) return;
	if (E.anim_force_style >= 0) {
		data->anim_style = E.anim_force_style % CL_ANIM_COUNT;
	} else {
		data->anim_style = CL_ANIM_DEFAULT;
	}
	data->anim_label = CL_LABEL_DEFAULT;
	data->anim_verb = hkTurnVerb(hk_cur_tool, data->current_prompt);
	data->anim_t0 = hk_time_ms();
	data->animating = 1;
	if (pthread_create(&data->anim_thread, NULL, clAnimThread, data) != 0) {
		data->animating = 0;
	}
}

static void clStopAnim(aiData *data) {
	if (!data->animating) return;
	data->animating = 0;
	pthread_join(data->anim_thread, NULL);
	data->anim_t0 = 0;
}

/* Once per turn, after everything it printed: the spinner stops at every model
   round and before every approval, and a line each time buried the answer. */
static void clSayWorkedFor(aiData *data, long ms) {
	/* Every turn, fast ones too: a line that only sometimes appears reads as a
	   turn that did not finish. */
	if (ms < 0 || E.pipe_mode || E.serve_mode || E.output_json || !E.color_enabled) return;
	long secs = ms / 1000;
	char el[48];
	if (secs >= 60)     snprintf(el, sizeof(el), "%ldm %lds", secs / 60, secs % 60);
	else if (ms < 10000) snprintf(el, sizeof(el), "%.1fs", ms / 1000.0);
	else                snprintf(el, sizeof(el), "%lds", secs);
	aiSayf(data, "\xE2\x96\xAA worked for %s", el);
}

static long clWallMs(void) { return hk_time_ms(); }


static char *hkMithraeumModelPath(const char *model) {
	if (!model || !*model) model = "hako-sho";
	if (model[0] == '/') return strdup(model);
	const char *home = getenv("HOME"); if (!home) home = ".";
	size_t n = strlen(home) + 2 * strlen(model) + 40;
	char *p = malloc(n);
	if (p) snprintf(p, n, "%s/.hako/models/%s/%s.mlf2", home, model, model);
	return p;
}

static char *hkMithraeumFirstAvailable(void) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/.hako/models", home);
	DIR *d = opendir(dir);
	if (!d) return NULL;
	char *found = NULL;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') continue;
		char w[1300]; struct stat st;
		if (snprintf(w, sizeof(w), "%s/%s/%s.mlf2", dir, de->d_name, de->d_name) >= (int)sizeof(w))
			continue;
		if (stat(w, &st) != 0) continue;
		free(found); found = strdup(de->d_name);
		if (strncmp(de->d_name, "hako-sho", 8) == 0) break;
	}
	closedir(d);
	return found;
}

static const struct { const char *prov, *models; } HK_MODEL_SUGG[] = {
	{ "anthropic",      "claude-opus-5, claude-sonnet-5, claude-opus-4-8, claude-opus-4-7, claude-sonnet-4-6, claude-haiku-4-5" },
	{ "openai",         "gpt-4o, gpt-4o-mini, gpt-5, o1, o1-mini, gpt-4.1" },
	{ "chatgpt",        "gpt-5.5, gpt-5.4, gpt-5.4-mini" },
	{ "gemini",         "gemini-2.5-pro, gemini-2.5-flash, gemini-1.5-pro, gemini-1.5-flash" },
	{ "google",         "gemini-2.5-pro, gemini-2.5-flash, gemini-1.5-pro" },
	{ "groq",           "llama-3.3-70b-versatile, llama-3.1-8b-instant, mixtral-8x7b-32768" },
	{ "cerebras",       "llama3.1-70b, llama3.1-8b, llama-3.3-70b" },
	{ "deepseek",       "deepseek-chat, deepseek-reasoner" },
	{ "mistral",        "mistral-large-latest, mistral-small-latest, codestral-latest" },
	{ "together",       "meta-llama/Llama-3.3-70B-Instruct-Turbo, Qwen/Qwen2.5-72B-Instruct-Turbo" },
	{ "fireworks",      "accounts/fireworks/models/llama-v3p1-70b-instruct, accounts/fireworks/models/qwen2p5-72b-instruct" },
	{ "openrouter",     "anthropic/claude-3.5-sonnet, openai/gpt-4o, meta-llama/llama-3.3-70b-instruct:free, deepseek/deepseek-chat:free" },
	{ "nous",           "anthropic/claude-haiku-5.5, anthropic/claude-sonnet-5.5, deepseek/deepseek-v4-flash, moonshotai/kimi-k3.1-pro" },
	{ "xai",            "grok-2, grok-2-mini, grok-beta" },
	{ "grok",           "grok-2, grok-2-mini, grok-beta" },
};

static const char *hkProviderLabel(void) {
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-copilot")) return "copilot";
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-models")) return "github-models";
	const char *ep = E.ai_endpoint;
	if (ep && *ep && E.ai_provider_type == AI_PROVIDER_OPENAI) {
		static const struct { const char *host, *name; } M[] = {
			{ "openrouter",                   "openrouter"    },
			{ "groq",                         "groq"          },
			{ "deepseek",                     "deepseek"      },
			{ "mistral",                      "mistral"       },
			{ "together",                     "together"      },
			{ "fireworks",                    "fireworks"     },
			{ "cerebras",                     "cerebras"      },
			{ "nousresearch",                 "nous"          },
			{ "x.ai",                         "xai"           },
			{ "generativelanguage",           "gemini"        },
			{ "models.inference.ai.azure.com","github-models" },
			{ "githubcopilot",                "copilot"       },
			{ "/backend-api/codex",           "chatgpt"       },
		};
		for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++)
			if (strstr(ep, M[i].host)) return M[i].name;
	}
	return hkProviderName(E.ai_provider_type);
}

static const char *hkCuratedModels(void) {
	const char *match = hkProviderLabel();
	if (!strcmp(match, "copilot"))       return "gpt-4o, gpt-4o-mini, o1-mini, claude-3.5-sonnet, claude-3.7-sonnet";
	if (!strcmp(match, "github-models")) return "gpt-4o, gpt-4o-mini, meta-llama-3-70b-instruct, mistral-large, microsoft/phi-3.5-mini";
	for (size_t i = 0; i < sizeof(HK_MODEL_SUGG) / sizeof(HK_MODEL_SUGG[0]); i++)
		if (!strcmp(HK_MODEL_SUGG[i].prov, match)) return HK_MODEL_SUGG[i].models;
	return NULL;
}

#define HK_MODELS_TTL   (24 * 60 * 60)
#define HK_MODELS_MAX   512

static const char *HK_MODEL_SKIP[] = {
	"embed", "whisper", "tts", "dall-e", "moderation", "rerank", "audio",
	"image", "stable-diffusion", "sora", "clip", "guard", "bge-", "nomic", NULL
};

static int hkModelIsChat(const char *id) {
	char low[128];
	size_t i = 0;
	for (; id[i] && i < sizeof(low) - 1; i++) low[i] = (char)tolower((unsigned char)id[i]);
	low[i] = '\0';
	for (int k = 0; HK_MODEL_SKIP[k]; k++)
		if (strstr(low, HK_MODEL_SKIP[k])) return 0;
	return 1;
}

static void hkModelsCachePath(char *buf, size_t n) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	const char *ep = E.ai_endpoint ? E.ai_endpoint : "";
	unsigned long long h = 0xcbf29ce484222325ULL;
	for (int i = 0; ep[i]; i++) { h ^= (unsigned char)ep[i]; h *= 0x100000001b3ULL; }
	snprintf(buf, n, "%s/.hako/cache/models-%s-%08lx.list", home,
		hkProviderName(E.ai_provider_type), (unsigned long)(h & 0xffffffffULL));
}

/* The catalog request as a curl command is fine where a shell exists. */
/* catalogs anyone can read, so the picker works before a key is pasted */
static int hkCatalogPublic(const char *ep) {
	return strstr(ep, "openrouter") || strstr(ep, "nousresearch");
}

#ifdef HAKO_WASM
static char *hkModelListFetch(void) {
	const char *endpoint = E.ai_endpoint;
	const char *api_key = E.ai_api_key;
	if (!endpoint || !*endpoint) return NULL;
	char url[1024], hdr[8192];
	url[0] = hdr[0] = '\0';

	switch (E.ai_provider_type) {
	case AI_PROVIDER_OLLAMA:
	case AI_PROVIDER_MITHRAEUM:
		snprintf(url, sizeof(url), "%s/api/tags", endpoint);
		if (api_key && *api_key)
			snprintf(hdr, sizeof(hdr), "-H 'Authorization: Bearer %s'", api_key);
		break;
	case AI_PROVIDER_ANTHROPIC:
		if (!api_key || !*api_key) return NULL;
		snprintf(url, sizeof(url), "%s/v1/models?limit=200", endpoint);
		snprintf(hdr, sizeof(hdr),
			"-H 'x-api-key: %s' -H 'anthropic-version: 2023-06-01'"
			" -H 'anthropic-dangerous-direct-browser-access: true'", api_key);
		break;
	default: {
		if (!api_key || !*api_key) {
			if (!hkCatalogPublic(endpoint)) return NULL;
			snprintf(url, sizeof(url), "%s/v1/models", endpoint);
			break;
		}
		size_t elen = strlen(endpoint);
		int has_v1 = elen >= 3 && !strcmp(endpoint + elen - 3, "/v1");
		int compat = elen >= 14 && !strcmp(endpoint + elen - 14, "/v1beta/openai");
		snprintf(url, sizeof(url), "%s%s", endpoint,
		         (has_v1 || compat) ? "/models" : "/v1/models");
		snprintf(hdr, sizeof(hdr), "-H 'Authorization: Bearer %s'", api_key);
		break;
	}
	}
	if (!url[0]) return NULL;
	hkHttpReq req = { "GET", url, hdr[0] ? hdr : NULL, NULL, 0 };
	return hk_http_fetch(&req);
}
#endif

/* The listing request for the current provider: url + headers, sent through hk_http_fetch so keys stay off argv. */
static int hkModelListReq(char *url, size_t ucap, char *hdr, size_t hcap) {
	const char *endpoint = E.ai_endpoint;
	const char *api_key = E.ai_api_key;
	int keyed = api_key && *api_key;
	hdr[0] = '\0';
	if (!endpoint || !*endpoint) return -1;
	switch (E.ai_provider_type) {
	case AI_PROVIDER_OLLAMA:
		snprintf(url, ucap, "%s/api/tags", endpoint);
		if (keyed) snprintf(hdr, hcap, "-H 'Authorization: Bearer %s'", api_key);
		return 0;
	case AI_PROVIDER_ANTHROPIC:
		if (!keyed) return -1;
		snprintf(url, ucap, "%s/v1/models?limit=200", endpoint);
		snprintf(hdr, hcap, hkOAuthIs("anthropic")
			? "-H 'Authorization: Bearer %s' -H 'anthropic-beta: oauth-2025-04-20' "
			  "-H 'User-Agent: claude-cli/1.0.40 (external, cli)' -H 'x-app: cli' "
			  "-H 'anthropic-version: 2023-06-01'"
			: "-H 'x-api-key: %s' -H 'anthropic-version: 2023-06-01'", api_key);
		return 0;
	case AI_PROVIDER_OPENAI: {
		if (!keyed) {
			if (!hkCatalogPublic(endpoint)) return -1;
			snprintf(url, ucap, "%s/v1/models", endpoint);
			return 0;
		}
		if (hkOAuthIs("chatgpt")) {
			/* client_version gates the catalog; an old one gets a frozen list */
			snprintf(url, ucap, "%s/models?client_version=99.0.0", endpoint);
			return hkChatGPTHeaders(hdr, hcap, api_key, 0) ? 0 : -1;
		}
		if (strstr(endpoint, "generativelanguage")) {
			snprintf(url, ucap, "%s/v1beta/models?pageSize=200", endpoint);
			snprintf(hdr, hcap, "-H 'x-goog-api-key: %s'", api_key);
			return 0;
		}
		int copilot = hkOAuthIs("github-copilot");
		int ghmodels = strstr(endpoint, "models.inference.ai.azure.com") != NULL;
		snprintf(url, ucap, "%s%s", endpoint, (copilot || ghmodels) ? "/models" : "/v1/models");
		snprintf(hdr, hcap, "-H 'Authorization: Bearer %s' %s", api_key, copilot
			? "-H 'Editor-Version: " HAKO_COPILOT_EDITOR_VER "' -H 'Copilot-Integration-Id: vscode-chat'"
			: "");
		return 0;
	}
	default: return -1;
	}
}

static int hkParseModelList(const char *json, char out[][96], int max) {
	if (!json || !*json || max <= 0) return 0;
	long long *when = calloc((size_t)max, sizeof(long long));
	if (!when) return 0;
	int n = 0;

	for (int pass = 0; pass < 3 && n == 0; pass++) {
		/* codex lists "slug"; openai-shaped lists "id"; gemini/ollama "name" */
		const char *key = pass == 0 ? "\"slug\":\"" : pass == 1 ? "\"id\":\"" : "\"name\":\"";
		size_t klen = strlen(key);
		const char *p = json;
		while (n < max && (p = strstr(p, key)) != NULL) {
			p += klen;
			const char *e = p;
			while (*e && *e != '"') e++;
			if (!*e) break;
			int len = (int)(e - p);
			if (len > 0 && len < 96) {
				char id[96];
				snprintf(id, sizeof(id), "%.*s", len, p);
				const char *bare = strncmp(id, "models/", 7) == 0 ? id + 7 : id;
				if (hkModelIsChat(bare)) {
					int dup = 0;
					for (int i = 0; i < n; i++) if (!strcmp(out[i], bare)) { dup = 1; break; }
					if (!dup) {
						snprintf(out[n], 96, "%s", bare);
						const char *nxt = strstr(e, key);
						const char *cr = strstr(e, "\"created\":");
						if (cr && (!nxt || cr < nxt)) when[n] = atoll(cr + 10);
						n++;
					}
				}
			}
			p = e;
		}
	}

	for (int i = 1; i < n; i++) {
		char id[96]; long long w = when[i];
		snprintf(id, sizeof(id), "%s", out[i]);
		int j = i - 1;
		while (j >= 0 && when[j] < w) {
			memmove(out[j + 1], out[j], 96);   /* same array: snprintf here is UB */
			when[j + 1] = when[j];
			j--;
		}
		memcpy(out[j + 1], id, sizeof(id));
		when[j + 1] = w;
	}
	free(when);
	return n;
}

static int hkModelsLive(char out[][96], int max, int force, long *age) {
	if (age) *age = -1;
	char path[1024];
	hkModelsCachePath(path, sizeof(path));

	struct stat st;
	int have_cache = (stat(path, &st) == 0);
	long old = have_cache ? (long)(time(NULL) - st.st_mtime) : -1;
	if (have_cache && !force && old < HK_MODELS_TTL) {
		char *txt = hkReadFileAll(path, 256 * 1024);
		if (txt) {
			int n = 0;
			char *save = NULL, *line = strtok_r(txt, "\n", &save);
			while (line && n < max) {
				if (*line) { snprintf(out[n], 96, "%s", line); n++; }
				line = strtok_r(NULL, "\n", &save);
			}
			free(txt);
			if (n > 0) { if (age) *age = old; return n; }
		}
	}

	int n = 0;
#ifdef HAKO_WASM
	char *wresp = hkModelListFetch();
	if (wresp) {
		n = hkParseModelList(wresp, out, max);
		free(wresp);
	}
#else
	static char lurl[1024], lhdr[8192];
	int routed = hkModelListReq(lurl, sizeof lurl, lhdr, sizeof lhdr) == 0;
	if (!routed && E.debug)
		fprintf(stderr, "[models] no listing route for %s (no key?)\n", hkProviderLabel());
	if (routed) {
		hkHttpReq lr = { .method = "GET", .url = lurl, .headers = lhdr, .timeout_s = 8 };
		char *resp = hk_http_fetch(&lr);
		if (E.debug) {
			fprintf(stderr, "[models] GET %s\n", lurl);
			fprintf(stderr, "[models] %zu bytes back: %.200s\n",
				resp ? strlen(resp) : (size_t)0, resp ? resp : "(nothing)");
		}
		if (resp) {
			n = hkParseModelList(resp, out, max);
			free(resp);
		}
	}
#endif
	if (n > 0) {
		const char *home = getenv("HOME"); if (home) {
			char dir[1024];
			snprintf(dir, sizeof(dir), "%s/.hako/cache", home);
			hk_fs_mkdirp(dir);
		}
		{
			size_t need = 1;
			for (int i = 0; i < n; i++) need += strlen(out[i]) + 1;
			char *blob = malloc(need);
			if (blob) {
				size_t at = 0;
				for (int i = 0; i < n; i++)
					at += (size_t)snprintf(blob + at, need - at, "%s\n", out[i]);
				hk_fs_write(path, blob, at, 0);
				free(blob);
			}
		}
		if (age) *age = 0;
		return n;
	}

	if (have_cache) {
		char *txt = hkReadFileAll(path, 256 * 1024);
		if (txt) {
			char *save = NULL, *line = strtok_r(txt, "\n", &save);
			while (line && n < max) {
				if (*line) { snprintf(out[n], 96, "%s", line); n++; }
				line = strtok_r(NULL, "\n", &save);
			}
			free(txt);
			if (n > 0 && age) *age = old;
		}
	}
	return n;
}

/* Point the session at a model the CURRENT provider actually serves. Lived
   inside the :provider handler, so signing in with :login left the old
   provider's model selected and the next turn failed on a model the new
   provider has never heard of — the user hit this on iSh after :login
   anthropic. Both paths call it now. */
static void hkHealModelForProvider(aiData *data) {
	static char live1[HK_MODELS_MAX][96];
	clOAuthEnsureFresh(data);
	int lv = hkModelsLive(live1, HK_MODELS_MAX, 0, NULL);
	int known = 0;
	for (int i = 0; i < lv && E.ai_model; i++)
		if (!strcmp(E.ai_model, live1[i])) { known = 1; break; }
	if (hkModelFitsProvider(E.ai_provider_type, E.ai_model) && (lv == 0 || known)) return;

	const char *pref = hkProviderDefaultModel(E.ai_provider_type);
	const char *dm = NULL;
	for (int i = 0; i < lv && pref; i++)
		if (!strcmp(pref, live1[i])) { dm = live1[i]; break; }
	if (!dm && lv > 0) dm = live1[0];
	if (!dm) dm = pref;
	if (dm && (!E.ai_model || strcmp(dm, E.ai_model))) {
		free(E.ai_model); E.ai_model = strdup(dm);
		char mm[192];
		snprintf(mm, sizeof(mm), "model \xE2\x86\x92 %.60s (prior model not valid for %.40s; :model to change, :models to list)",
			dm, hkProviderLabel());
		if (!hk_menu_pick) aiAddHistory(data, mm);
		hkSaveSession();
	} else if (!dm) {
		aiAddHistory(data, "note: pick a model for this provider — :model <id> (:models to list)");
	}
}

static int hkGatherModels(char out[][96], int max) {
	int n = 0;
	if (E.ai_provider_type == AI_PROVIDER_MITHRAEUM) {
		const char *home = getenv("HOME"); if (!home) home = ".";
		char mdir[1024];
		snprintf(mdir, sizeof(mdir), "%s/.hako/models", home);
		DIR *d = opendir(mdir);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL && n < max) {
				if (de->d_name[0] == '.') continue;
				char w[1300]; struct stat ws;
				if (snprintf(w, sizeof(w), "%s/%s/%s.mlf2", mdir, de->d_name, de->d_name) >= (int)sizeof(w))
					continue;
				if (stat(w, &ws) != 0) continue;
				snprintf(out[n], 96, "%.95s", de->d_name);
				n++;
			}
			closedir(d);
		}
		return n;
	}
	n = hkModelsLive(out, max, 0, NULL);
	if (n > 0) return n;

	const char *models = hkCuratedModels();
	if (!models) return 0;
	const char *p = models;
	while (*p && n < max) {
		while (*p == ' ') p++;
		const char *e = strchr(p, ',');
		if (!e) e = p + strlen(p);
		int len = (int)(e - p);
		if (len > 95) len = 95;
		while (len > 0 && p[len - 1] == ' ') len--;
		if (len > 0) { snprintf(out[n], 96, "%.*s", len, p); n++; }
		if (!*e) break;
		p = e + 1;
	}
	return n;
}

static int hkMithraeumRelocate(const char *model) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	char canon[1024];
	snprintf(canon, sizeof(canon), "%s/.hako/models/%s/%s.mlf2", home, model, model);

	struct stat st;
	if (stat(canon, &st) == 0) return 1;

	char cand[6][1280];
	int nc = 0;
	snprintf(cand[nc++], 1280, "%s/.hako/models/%s.mlf2", home, model);
	snprintf(cand[nc++], 1280, "%s/.hako/blobs/%s.mlf2", home, model);
	snprintf(cand[nc++], 1280, "%s/Downloads/%s.mlf2", home, model);
	snprintf(cand[nc++], 1280, "./%s.mlf2", model);

	char mdir[1024];
	snprintf(mdir, sizeof(mdir), "%s/.hako/models/%s", home, model);
	for (int i = 0; i < nc; i++) {
		if (stat(cand[i], &st) != 0) continue;
		char base[1024];
		snprintf(base, sizeof(base), "%s/.hako/models", home);
		hk_fs_mkdirp(base);
		hk_fs_mkdirp(mdir);
#ifdef _WIN32
		if (CopyFileA(cand[i], canon, FALSE)) return 1;
#else
		if (symlink(cand[i], canon) == 0) return 1;
#endif
		if (stat(canon, &st) == 0) return 1;
	}
	return 0;
}

static int hkHealLocalModel(void) {
	if (E.ai_provider_type != AI_PROVIDER_MITHRAEUM || !E.ai_model || !*E.ai_model) return 0;
	char *p = hkMithraeumModelPath(E.ai_model);
	if (!p) return 0;
	struct stat st;
	if (stat(p, &st) == 0) { free(p); return 0; }
	if (hkMithraeumRelocate(E.ai_model)) { free(p); return 0; }
	free(p);

	char *avail = hkMithraeumFirstAvailable();
	if (!avail) return 0;
	if (isatty(STDOUT_FILENO)) {
		const char *R = E.color_enabled ? ANSI_RESET : "";
		const char *M = E.color_enabled ? TH_META : "";
		printf("  %s'%s' is not installed — switched to %s.%s\n", M, E.ai_model, avail, R);
	}
	free(E.ai_model);
	E.ai_model = avail;
	hkSaveSession();
	return 1;
}

static int hkPullModel(const char *model) {
#ifdef HAKO_WASM
	(void)model;
	return -1;
#else
	const char *home = getenv("HOME"); if (!home) home = ".";
	const char *base = getenv("HAKO_HF_BASE");
	if (!base || !*base) base = "https://huggingface.co/mithraeum";

	char mroot[1024], mdir[1280], dest[1320], tmp[1340], url[1536];
	snprintf(mroot, sizeof(mroot), "%s/.hako/models", home);
	snprintf(mdir,  sizeof(mdir),  "%s/%s", mroot, model);
	snprintf(dest,  sizeof(dest),  "%s/%s.mlf2", mdir, model);
	snprintf(tmp,   sizeof(tmp),   "%s/%s.mlf2.part", mdir, model);
	snprintf(url,   sizeof(url),   "%s/%s/resolve/main/%s.mlf2", base, model, model);

	struct stat ex;
	if (stat(dest, &ex) == 0) {
		if (!isatty(STDIN_FILENO)) { printf("  already installed: %s\n", dest); fflush(stdout); return 0; }
		char q[256];
		snprintf(q, sizeof(q), "'%s' already installed (%.1f GB). Re-download and overwrite?",
		         model, (double)ex.st_size / 1e9);
		if (!clPromptYN(q, 0)) { printf("  keeping existing.\n"); fflush(stdout); return 0; }
	}

	hk_fs_mkdirp(mroot);
	hk_fs_mkdirp(mdir);

	if (system("command -v curl >/dev/null 2>&1") != 0) {
		printf("  pull needs `curl` on PATH.\n"); fflush(stdout);
		return -1;
	}

	printf("  downloading %s\n  from %s\n", model, url); fflush(stdout);
	char cmd[3200];
	snprintf(cmd, sizeof(cmd), "curl -fL --progress-bar -o '%s' '%s'", tmp, url);
	int rc = system(cmd);

	struct stat st;
	if (rc != 0 || stat(tmp, &st) != 0 || st.st_size < 1024 * 1024) {
		unlink(tmp);
		printf("  pull failed (is the tier uploaded to %s yet?).\n", base);
		fflush(stdout);
		return -1;
	}
	if (rename(tmp, dest) != 0) { unlink(tmp); printf("  could not move into place.\n"); return -1; }
	printf("  installed: %s (%.1f GB)\n", dest, (double)st.st_size / 1e9);
	fflush(stdout);
	return 0;
#endif
}

static char *hkFindHakm(void) {
#ifdef HAKO_WASM
	return NULL;
#else
	const char *env = getenv("HAKO_HAKM");
	if (env && *env) return strdup(env);
	const char *home = getenv("HOME"); if (!home) home = ".";
	char p[1024];
	snprintf(p, sizeof(p), "%s/.hako/bin/hakm", home);
	struct stat st;
	if (stat(p, &st) == 0 && (st.st_mode & S_IXUSR)) return strdup(p);
	if (system("command -v hakm >/dev/null 2>&1") == 0) return strdup("hakm");
	return NULL;
#endif
}

#if !defined(_WIN32) && !defined(HAKO_WASM)
#ifndef HAKO_WASM
static pid_t g_hakm_pid = -1;
static FILE *g_hakm_w = NULL;
static FILE *g_hakm_r = NULL;
static char *g_hakm_weights = NULL;
static int   g_hakm_err = -1;    /* read end of the engine's stderr */

/* Whatever the engine has said on stderr since it started. */
static int hkHakmErrText(char *out, size_t cap) {
	if (g_hakm_err < 0 || cap < 2) return 0;
	size_t n = 0;
	for (;;) {
		ssize_t r = read(g_hakm_err, out + n, cap - 1 - n);
		if (r <= 0) break;
		n += (size_t)r;
		if (n >= cap - 1) break;
	}
	out[n] = 0;
	while (n && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = 0;
	return n > 0;
}

static void hkHakmKill(void) {
	if (g_hakm_w) { fclose(g_hakm_w); g_hakm_w = NULL; }
	if (g_hakm_r) { fclose(g_hakm_r); g_hakm_r = NULL; }
	if (g_hakm_pid > 0) {
		kill(g_hakm_pid, SIGTERM);
		waitpid(g_hakm_pid, NULL, 0);
		g_hakm_pid = -1;
	}
	if (g_hakm_err >= 0) { close(g_hakm_err); g_hakm_err = -1; }
	free(g_hakm_weights); g_hakm_weights = NULL;
}

static int hkHakmEnsure(const char *hakm, const char *weights) {
	if (g_hakm_w && g_hakm_weights && strcmp(g_hakm_weights, weights) == 0) {
		if (waitpid(g_hakm_pid, NULL, WNOHANG) == 0) return 0;
		hkHakmKill();
	} else if (g_hakm_w) {
		hkHakmKill();
	}
	int sv[2], ep[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
	if (pipe(ep) != 0) { close(sv[0]); close(sv[1]); return -1; }
	pid_t pid = fork();
	if (pid < 0) { close(sv[0]); close(sv[1]); close(ep[0]); close(ep[1]); return -1; }
	if (pid == 0) {
		dup2(sv[1], STDIN_FILENO);
		dup2(sv[1], STDOUT_FILENO);
		close(sv[0]); close(sv[1]);
		dup2(ep[1], STDERR_FILENO);
		close(ep[0]); close(ep[1]);
		const char *ctx = getenv("HAKO_CTX");
		if (!ctx || atoi(ctx) <= 0) ctx = (sizeof(void *) == 4) ? "4096" : "8192";
		execlp(hakm, hakm, weights, "--serve", "--ctx", ctx, (char *)NULL);
		_exit(127);
	}
	close(sv[1]);
	close(ep[1]);
	g_hakm_err = ep[0];
	{ int fl = fcntl(g_hakm_err, F_GETFL, 0); if (fl >= 0) fcntl(g_hakm_err, F_SETFL, fl | O_NONBLOCK); }
	g_hakm_pid = pid;
	g_hakm_w = fdopen(sv[0], "w");
	g_hakm_r = fdopen(dup(sv[0]), "r");
	if (!g_hakm_w || !g_hakm_r) { hkHakmKill(); return -1; }
	g_hakm_weights = strdup(weights);
	return 0;
}

static char *hkHakmRoundtrip(const char *frame, size_t flen, char **eng) {
	if (fwrite(frame, 1, flen, g_hakm_w) != flen) return NULL;
	if (fflush(g_hakm_w) != 0) return NULL;
	char hdr[64];
	char kind;
	long len;
	clStreamLine sline = { {0}, 0, 0, 0, 0 };
	int streamed = 0, quiet = 0;
	char seen[256]; size_t seen_n = 0; seen[0] = 0;
	/* "T" frames are tokens as they are sampled; "R"/"E" ends the reply. */
	for (;;) {
		if (E.interrupt) { if (streamed) clStreamEnd(&sline); return NULL; }
		if (!fgets(hdr, sizeof(hdr), g_hakm_r)) { if (streamed) clStreamEnd(&sline); return NULL; }
		kind = hdr[0];
		len = atol(hdr + 1);
		if (len < 0 || len > (1l << 20)) { if (streamed) clStreamEnd(&sline); return NULL; }
		if (kind != 'T') break;
		char tok[512];
		size_t want = (size_t)len < sizeof(tok) - 1 ? (size_t)len : sizeof(tok) - 1;
		if (fread(tok, 1, want, g_hakm_r) != want) { if (streamed) clStreamEnd(&sline); return NULL; }
		tok[want] = '\0';
		if (!quiet) {
			size_t tl = strlen(tok);
			if (seen_n + tl < sizeof(seen) - 1) { memcpy(seen + seen_n, tok, tl); seen_n += tl; seen[seen_n] = 0; }
			else { seen_n = 0; seen[0] = 0; }
			if (strstr(seen, "<tool") || strstr(seen, "<write_file") || strstr(seen, "<invoke")
			    || strstr(seen, "<observation")) {   /* a small model repeating a result back */
				quiet = 1;
				if (streamed) { clStreamUnecho(&sline); sline.len = 0; clStreamPark(&sline); }
			}
		}
		if (quiet) continue;
		if (E.pipe_mode) { hkDeltaPush(tok); continue; }
		if (!streamed) { streamed = 1; clStopAnim(&G_AI); }
		clStreamPush(&sline, tok);
	}
	if (streamed && quiet) { clStreamUnecho(&sline); sline.len = 0; }
	if (streamed) clStreamEnd(&sline);
	if (E.debug) {                     /* the engine's own notes, printed safely */
		char dbg[1024];
		if (hkHakmErrText(dbg, sizeof(dbg))) {
			for (char *q = dbg, *nl; q && *q; q = nl ? nl + 1 : NULL) {
				nl = strchr(q, '\n');
				if (nl) *nl = '\0';
				if (*q) aiSayf(&G_AI, "engine: %.200s", q);
			}
		}
	}
	/* What was shown live must not be printed again from the finished reply. */
	if (streamed && !quiet && kind == 'R') G_AI.suppress_next_echo = 1;
	if (kind != 'R' && kind != 'E') return NULL;
	char *out = malloc((size_t)len + 1);
	if (!out) return NULL;
	size_t got = len ? fread(out, 1, (size_t)len, g_hakm_r) : 0;
	out[got] = '\0';
	if (got != (size_t)len) { free(out); return NULL; }
	if (kind == 'E') {
		if (eng) {
			size_t n = got + 32;
			char *m = malloc(n);
			if (m) snprintf(m, n, "Error: hakm: %s", out);
			*eng = m;
		}
		free(out);
		return NULL;
	}
	return out;
}
#else
static void hkHakmKill(void) { }
static char *hkHakmRoundtrip(const char *frame, size_t flen, char **eng) {
	(void)frame; (void)flen;
	if (eng) *eng = NULL;
	return NULL;
}
#endif
#endif

/* Stop the turn that is in flight. */
static void hkAbortTurn(void) {
	E.interrupt = 1;
	hkVoiceStop();
#if !defined(_WIN32) && !defined(HAKO_WASM)
	if (g_hakm_pid > 0) kill(g_hakm_pid, SIGTERM);
#endif
}

static int hk_local_stream = 0;   /* set per request: only an interactive turn wants tokens */

static char *hkMithraeumFrame(aiData *data, const char *sys, int ntok, size_t *flen) {
	int n = sys ? 1 : 0;
	size_t need = 64 + (sys ? strlen(sys) + 40 : 0);
	for (int i = 0; i < data->message_count; i++) {
		aiMessage *m = &data->messages[i];
		if (m->raw) continue;
		n++;
		need += strlen(m->role ? m->role : "user")
		      + (m->content ? strlen(m->content) : 0) + 32;
	}
	if (n == 0 || (n == 1 && sys)) return NULL;
	char *fb = malloc(need);
	if (!fb) return NULL;
	int mt = E.ai_tools_enabled ? 0 : (E.ai_temperature > 0 ? E.ai_temperature * 10 : 0);
	size_t fl = (size_t)snprintf(fb, need, "%d %d %d %d\n", n, ntok, mt, hk_local_stream);
	if (sys) {
		size_t sl = strlen(sys);
		fl += (size_t)snprintf(fb + fl, need - fl, "system\n%zu\n", sl);
		memcpy(fb + fl, sys, sl);
		fl += sl;
	}
	for (int i = 0; i < data->message_count; i++) {
		aiMessage *m = &data->messages[i];
		if (m->raw) continue;
		const char *role = m->role ? m->role : "user";
		const char *content = m->content ? m->content : "";
		size_t cl = strlen(content);
		fl += (size_t)snprintf(fb + fl, need - fl, "%s\n%zu\n", role, cl);
		memcpy(fb + fl, content, cl);
		fl += cl;
	}
	*flen = fl;
	return fb;
}

static char *hkStripCodeFences(const char *src) {
	size_t len = strlen(src);
	char *out = malloc(len + 1);
	if (!out) return NULL;
	size_t o = 0;
	const char *p = src;
	int in_write = 0;
	while (*p) {
		if (!in_write && !strncmp(p, "<write_file", 11)) in_write = 1;
		else if (in_write && !strncmp(p, "</write_file>", 13)) {
			memcpy(out + o, p, 13); o += 13; p += 13; in_write = 0; continue;
		}
		if (!in_write && p[0] == '`' && p[1] == '`' && p[2] == '`') {
			const char *e = strstr(p + 3, "```");
			if (e) { p = e + 3; if (*p == '\n') p++; continue; }
		}
		out[o++] = *p++;
	}
	out[o] = '\0';
	return out;
}

static char *hkExtractBestFence(const char *src) {
	const char *best_body = NULL; size_t best_len = 0;
	const char *p = src;
	while ((p = strstr(p, "```")) != NULL) {
		const char *c = strstr(p + 3, "```");
		if (!c) break;
		const char *body = p + 3;
		const char *nl = memchr(body, '\n', (size_t)(c - body));
		if (nl) body = nl + 1;
		size_t len = (size_t)(c - body);
		while (len && (body[len-1] == '\n' || body[len-1] == '\r')) len--;
		if (len > best_len) { best_len = len; best_body = body; }
		p = c + 3;
	}
	if (!best_body || best_len == 0) return NULL;
	char *out = malloc(best_len + 1);
	if (!out) return NULL;
	memcpy(out, best_body, best_len); out[best_len] = '\0';
	return out;
}

static char *hkStripToolBlocks(const char *src) {
	size_t len = strlen(src);
	char *out = malloc(len + 1);
	if (!out) return NULL;
	size_t o = 0;
	const char *p = src;
	while (*p) {
		if (strncmp(p, "<tool_call>", 11) == 0) {
			const char *e = strstr(p + 11, "</tool_call>");
			if (e) { p = e + 12; continue; }
		}
		if (strncmp(p, "<tool ", 6) == 0) {
			const char *e = strstr(p + 6, "</tool>");
			if (e) { p = e + 7; continue; }
		}
		if (strncmp(p, "<write_file", 11) == 0) {
			const char *e = strstr(p + 11, "</write_file>");
			if (e) { p = e + 13; continue; }
		}
		out[o++] = *p++;
	}
	while (o && (out[o-1] == '\n' || out[o-1] == '\r' || out[o-1] == ' ' || out[o-1] == '\t')) o--;
	out[o] = '\0';
	return out;
}

#ifndef _WIN32
static int hkMithraeumTrimOldest(aiData *data) {
	pthread_mutex_lock(&data->lock);
	int n = data->message_count;
	int drop = n / 2;
	if (drop >= n) drop = n - 1;
	if (drop < 1) { pthread_mutex_unlock(&data->lock); return 0; }
	for (int i = 0; i < drop; i++) {
		free(data->messages[i].role);
		free(data->messages[i].content);
	}
	memmove(data->messages, data->messages + drop,
	        (size_t)(n - drop) * sizeof(aiMessage));
	data->message_count = n - drop;
	pthread_mutex_unlock(&data->lock);
	return drop;
}
#endif

#if !defined(_WIN32) && !defined(HAKO_WASM)
static pthread_t g_prewarm_th;
static int g_prewarm_on = 0;

static void hkPrewarmJoin(void) {
	if (!g_prewarm_on) return;
	g_prewarm_on = 0;
	pthread_join(g_prewarm_th, NULL);
}

static void *hkPrewarmThread(void *arg) {
	hkBlockWinch();
	aiData *data = (aiData *)arg;
	char *hakm = hkFindHakm();
	if (!hakm) return NULL;
	char *path = hkMithraeumModelPath(E.ai_model);
	if (path && access(path, R_OK) != 0) {
		char *avail = hkMithraeumFirstAvailable();
		free(path);
		path = avail ? hkMithraeumModelPath(avail) : NULL;
		free(avail);
	}
	if (E.debug) fprintf(stderr, "[prewarm] weights: %s\n", path ? path : "(none)");
	if (path && access(path, R_OK) == 0 && hkHakmEnsure(hakm, path) == 0) {
		const char *sys = (data->system_prompt && *data->system_prompt)
		                ? data->system_prompt : NULL;
		size_t fl = 0;
		hk_local_stream = 0;                              /* a warmup shows nothing */
		char *fb = hkMithraeumFrame(data, sys, 1, &fl);   /* one token: the prefill is the point */
		if (fb) {
			char *eng = NULL;
			free(hkHakmRoundtrip(fb, fl, &eng));
			free(eng);
			free(fb);
		}
	}
	free(path);
	free(hakm);
	return NULL;
}

static void hkPrewarmStart(aiData *data) {
	if (g_prewarm_on || hk_bench_mode) return;
	if (E.ai_provider_type != AI_PROVIDER_MITHRAEUM) return;
	if (E.pipe_mode || E.output_json) return;
	if (sizeof(void *) == 4) {
		const char *on = getenv("HAKO_PREWARM");
		if (!(on && *on == '1')) return;
	}
	const char *off = getenv("HAKO_NO_PREWARM");
	if (off && *off && strcmp(off, "0")) return;
	if (pthread_create(&g_prewarm_th, NULL, hkPrewarmThread, data) == 0) g_prewarm_on = 1;
}
#else
static void hkPrewarmJoin(void) { }
static void hkPrewarmStart(aiData *data) { (void)data; }
#endif

/* :remote on: a streamed reply skips the history echo, so the phone gets its
   finished text here (tool blocks out). */
static void hkPhoneFinal(const char *content) {
	if (!hs_attached || !content) return;
	char *pd = hkStripToolBlocks(content);
	if (pd) clPipeEmitMsg("ai", pd);
	free(pd);
}

static char *hkMithraeumChat(aiData *data, char **err) {
#ifdef HAKO_WASM
	/* The engine is a separate process; a browser tab cannot start one. */
	(void)data;
	if (err) *err = strdup("Error: local models are not in the browser yet — pick a cloud provider (:provider)");
	return NULL;
#else
	hkPrewarmJoin();
	const char *model = E.ai_model ? E.ai_model : "hako-sho";
	char *path = hkMithraeumModelPath(model);
	if (!path) { if (err) *err = strdup("Error: out of memory"); return NULL; }

	struct stat stbuf;
	if (stat(path, &stbuf) != 0) {
		if (hkMithraeumRelocate(model) && stat(path, &stbuf) == 0) {
			pthread_mutex_lock(&data->lock);
			aiSayf(data, "found '%s' weights elsewhere — linked into ~/.hako/models.", model);
			pthread_mutex_unlock(&data->lock);
		}
	}
	if (stat(path, &stbuf) != 0) {
		char *avail = hkMithraeumFirstAvailable();
		if (avail) {
			pthread_mutex_lock(&data->lock);
			char note[256];
			snprintf(note, sizeof(note),
				"model '%s' has no weights — falling back to '%s'. :model <name> to pick another.",
				model, avail);
			aiAddHistory(data, note);
			pthread_mutex_unlock(&data->lock);
			free(E.ai_model); E.ai_model = strdup(avail);
			hkSaveSession();
			free(path);
			path = hkMithraeumModelPath(avail);
			free(avail);
			if (!path) { if (err) *err = strdup("Error: out of memory"); return NULL; }
		} else {
			if (err) {
				size_t n = strlen(path) + 220;
				char *m = malloc(n);
				if (m) snprintf(m, n,
					"Error: no hako weights installed (looked for %s)\n"
					"  run  :pull %s  to download it from huggingface.co/mithraeum,\n"
					"  or convert a GGUF with hako/tools/gguf2mlf.py.",
					path, model);
				*err = m;
			}
			free(path);
			return NULL;
		}
	}

	char *hakm = hkFindHakm();
	if (!hakm) {
		if (err) *err = strdup(
			"Error: `hakm` engine binary not found.\n"
			"  build it: cd hako && make && cp hakm ~/.hako/bin/   (or from hako-code: make hakm)\n"
			"  or set HAKO_HAKM=/path/to/hakm.");
		free(path);
		return NULL;
	}

	const char *sys = (data->system_prompt && *data->system_prompt) ? data->system_prompt : NULL;
	int ntok = E.ai_max_tokens > 0 ? E.ai_max_tokens : 1024;
	if (E.ai_tools_enabled && ntok < 3072) ntok = 3072;
	hk_local_stream = ((!E.pipe_mode && !E.output_json && !hk_bench_mode
	                   && E.color_enabled && isatty(STDOUT_FILENO)) || E.serve_mode) ? 1 : 0;

#ifndef _WIN32
	size_t fl = 0;
	char *fb = hkMithraeumFrame(data, sys, ntok, &fl);
	if (!fb) {
		free(hakm); free(path);
		if (err) *err = strdup("Error: nothing to send");
		return NULL;
	}

	char *out = NULL;
	char engine_said[1024] = "";
	int trimmed = 0;
	for (int attempt = 0; attempt < 2 && !out; attempt++) {
#ifndef HAKO_WASM
		if (hkHakmEnsure(hakm, path) != 0) break;
#endif
		char *eng = NULL;
		out = hkHakmRoundtrip(fb, fl, &eng);
		if (eng) {
			if (!trimmed && strstr(eng, "context window")) {
				free(eng);
				int dropped = hkMithraeumTrimOldest(data);
				char *fb2 = NULL;
				if (dropped > 0) fb2 = hkMithraeumFrame(data, sys, ntok, &fl);
				if (fb2) {
					free(fb);
					fb = fb2;
					pthread_mutex_lock(&data->lock);
					char note[96];
					snprintf(note, sizeof(note),
						"(context full — dropped %d oldest message(s), retrying)", dropped);
					aiAddHistory(data, note);
					pthread_mutex_unlock(&data->lock);
					trimmed = 1;
					attempt = -1;
					continue;
				}
				free(fb); free(hakm); free(path);
				if (err) {
					long ptok = data->system_prompt ? (long)(strlen(data->system_prompt) / 4) : 0;
					char m[512];
					snprintf(m, sizeof(m),
						"Error: conversation exceeds the model's context window.\n"
						"  the system prompt alone is ~%ld tokens. If that is most of the window, "
						"a new session will not help — raise it with HAKO_CTX (e.g. HAKO_CTX=8192 hako) "
						"or turn tools off with :tools off to shrink the prompt.\n"
						"  otherwise :session new starts fresh.", ptok);
					*err = strdup(m);
				}
				return NULL;
			}
			free(fb); free(hakm); free(path);
			if (err) *err = eng; else free(eng);
			return NULL;
		}
		if (!out) {
			hkHakmErrText(engine_said, sizeof(engine_said));
			/* The pipe closes a beat before the child is reapable, so a single
			   WNOHANG probe usually saw "still running" and the exit status was
			   thrown away. Give it up to half a second; a normal exit is reported
			   too, not only a signal. */
			int st = 0, reaped = 0;
			for (int i = 0; g_hakm_pid > 0 && i < 50 && !reaped; i++) {
				pid_t w = waitpid(g_hakm_pid, &st, WNOHANG);
				if (w == g_hakm_pid) reaped = 1;
				else if (w < 0) break;
				else cl_sleep_ms(10);
			}
			if (reaped) {
				g_hakm_pid = -1;               /* already reaped; hkHakmKill must not wait again */
				size_t at = strlen(engine_said);
				if (WIFSIGNALED(st)) {
					int sg = WTERMSIG(st);
					snprintf(engine_said + at, sizeof(engine_said) - at,
					         "%s(killed by signal %d%s)", at ? "\n  " : "", sg,
					         sg == 9 ? " — out of memory" : sg == 15 && E.interrupt ? " — interrupted" : "");
				} else if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
					snprintf(engine_said + at, sizeof(engine_said) - at,
					         "%s(exited with status %d)", at ? "\n  " : "", WEXITSTATUS(st));
				}
			}
			hkHakmKill();
		}
	}
	free(fb); free(hakm); free(path);
	if (!out) {
		if (err) {
			char m[1024];
			if (E.interrupt && engine_said[0]) {
				/* We killed it: :q or ^C SIGTERMs the engine on purpose. That is
				   not an error and must not read like one. */
				snprintf(m, sizeof(m), "(interrupted — engine stopped)");
			} else if (engine_said[0]) {
				const char *said = engine_said;
				size_t sl = strlen(said);
				if (sl > 700) said += sl - 700;   /* the banner is first; the reason is LAST */
				snprintf(m, sizeof(m),
					"Error: the hako engine stopped.\n  it said: %s%.700s\n"
					"  (run `hako --debug` to watch it live)", sl > 700 ? "…" : "", said);
			}
			else
				snprintf(m, sizeof(m),
					"Error: hakm engine unavailable (spawn or transport failed), and it said "
					"nothing.\n  check it runs: hakm --version\n"
					"  rebuild: cd hako && make && cp hakm ~/.hako/bin/");
			*err = strdup(m);
		}
		return NULL;
	}
	size_t total = strlen(out);
	while (total && (out[total-1] == '\n' || out[total-1] == '\r')) out[--total] = '\0';
	return out;

#else
	size_t wfl = 0;
	char *wfb = hkMithraeumFrame(data, sys, ntok, &wfl);
	if (!wfb) {
		free(hakm); free(path);
		if (err) *err = strdup("Error: nothing to send");
		return NULL;
	}
	char frame[256];
	snprintf(frame, sizeof(frame), "/tmp/hako-frame-%d.txt", (int)getpid());
	FILE *ff = fopen(frame, "wb");
	if (!ff) { free(wfb); if (err) *err = strdup("Error: cannot write temp frame"); free(hakm); free(path); return NULL; }
	fwrite(wfb, 1, wfl, ff);
	fclose(ff);
	free(wfb);

	char cmd[2048];
	snprintf(cmd, sizeof(cmd),
		"'%s' '%s' --chat-stdin -n %d < '%s' 2>/dev/null",
		hakm, path, ntok, frame);
	free(hakm); free(path);

	FILE *fp = popen(cmd, "r");
	if (!fp) { unlink(frame); if (err) *err = strdup("Error: could not launch hakm"); return NULL; }

	char *out = NULL;
	size_t total = 0;
	char rbuf[4096];
	size_t r;
	while ((r = fread(rbuf, 1, sizeof(rbuf), fp)) > 0) {
		if (total + r > 1u << 20) r = (1u << 20) - total;
		if (r == 0) break;
		char *t = realloc(out, total + r + 1);
		if (!t) break;
		out = t;
		memcpy(out + total, rbuf, r);
		total += r;
		out[total] = '\0';
	}
	int rc = pclose(fp);
	unlink(frame);

	if (!out) {
		if (rc != 0 && err) {
			char *e = malloc(96);
			if (e) snprintf(e, 96, "Error: hakm exited %d (no output)", WEXITSTATUS(rc));
			*err = e;
			return NULL;
		}
		return strdup("");
	}
	while (total && (out[total-1] == '\n' || out[total-1] == '\r')) out[--total] = '\0';
	return out;
#endif
#endif
}

static int hkShouldNudgeWrite(const char *content, const char *ask) {
	if (!content || !ask) return 0;
	if (hkHasToolBlock(content)) return 0;
	if (!strstr(content, "```")) return 0;
	char low[1024]; size_t n = 0;
	for (const char *p = ask; *p && n < sizeof(low) - 1; p++) {
		char c = *p;
		low[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	low[n] = '\0';
	if (strstr(low, "error") || strstr(low, "traceback") || strstr(low, "exception")
	    || strstr(low, "fix") || strstr(low, " broke") || strstr(low, "doesn't")
	    || strstr(low, "does not") || strstr(low, "not work") || strstr(low, "fail")
	    || strstr(low, "wrong") || strstr(low, "edit") || strstr(low, "update")
	    || strstr(low, "change")) return 1;
	return strstr(low, "creat") || strstr(low, "write") || strstr(low, "save")
	    || strstr(low, "generat") || strstr(low, "make ") || strstr(low, "build ")
	    || strstr(low, "add ");
}

static int hkClaimedWriteNoAct(const char *content, const char *ask) {
	if (!content || !ask) return 0;
	if (hkHasToolBlock(content)) return 0;
	if (strstr(content, "```")) return 0;
	int claims = strstr(content, "has been") || strstr(content, "reated")
	          || strstr(content, "pdated")   || strstr(content, "ritten")
	          || strstr(content, "wrote ")   || strstr(content, "aved to")
	          || strstr(content, "as saved");
	if (!claims) return 0;
	char low[1024]; size_t n = 0;
	for (const char *p = ask; *p && n < sizeof(low) - 1; p++) {
		char c = *p; low[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	low[n] = '\0';
	return strstr(low, "creat") || strstr(low, "writ") || strstr(low, "save")
	    || strstr(low, "generat") || strstr(low, "make ") || strstr(low, "build ")
	    || strstr(low, "add ") || strstr(low, "error") || strstr(low, "traceback")
	    || strstr(low, "exception") || strstr(low, "fix") || strstr(low, "edit")
	    || strstr(low, "update") || strstr(low, "change") || strstr(low, "doesn't")
	    || strstr(low, "not work") || strstr(low, "fail") || strstr(low, "wrong");
}

static int hkLooksActionable(const char *ask) {
	if (!ask || !*ask) return 0;
	char low[1024]; size_t n = 0;
	for (const char *p = ask; *p && n < sizeof(low) - 1; p++) {
		char c = *p; low[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	low[n] = '\0';
	if (strstr(low, "what is") || strstr(low, "what are") || strstr(low, "how does")
	    || strstr(low, "how do ") || strstr(low, "explain") || strstr(low, "difference between"))
		return 0;
	/* Something in the project is named: a file-ish noun or an extension. */
	if (strstr(low, "file") || strstr(low, "director") || strstr(low, "folder")
	    || strstr(low, "project") || strstr(low, "readme") || strstr(low, "script")
	    || strstr(low, "this dir") || strstr(low, "the code") || strstr(low, "my code")
	    || strstr(low, ".py") || strstr(low, ".js") || strstr(low, ".html") || strstr(low, ".md")
	    || strstr(low, ".sh") || strstr(low, ".css") || strstr(low, ".c ") || (n > 2 && !strcmp(low + n - 2, ".c")))
		return 1;
	static const char *verbs[] = { "fix", "create", "make", "build", "add", "list", "run",
		"edit", "update", "change", "show", "look", "check", "read", "open", "debug",
		"improve", "write", "implement", "refactor", "ls", NULL };
	const char *q = low;
	while (*q == ' ') q++;
	for (const char *lead = q;;) {
		for (int i = 0; verbs[i]; i++) {
			size_t vl = strlen(verbs[i]);
			if (!strncmp(lead, verbs[i], vl) && (lead[vl] == ' ' || lead[vl] == '\0'))
				return 1;
		}
		/* allow one softener: "please fix", "can you add", "lets list" */
		const char *sp = strchr(lead, ' ');
		if (!sp || lead != q) break;
		if (strncmp(lead, "please", 6) && strncmp(lead, "can you", 7) && strncmp(lead, "could you", 9)
		    && strncmp(lead, "lets", 4) && strncmp(lead, "let's", 5) && strncmp(lead, "now", 3)
		    && strncmp(lead, "just", 4) && strncmp(lead, "go", 2)) break;
		lead = sp + 1;
		if (!strncmp(lead, "you ", 4)) lead += 4;
	}
	return 0;
}

/* The last failure text, so a non-interactive run can report WHY instead of… */
static char hk_last_error[512] = "";

/* Anthropic signals a transient overload as an SSE *error event* mid-stream: */
static int hkRetryableStreamError(const char *body) {
	if (!body || !*body) return 0;
	return strstr(body, "overloaded_error") != NULL
	    || strstr(body, "api_error") != NULL
	    || strstr(body, "\"type\":\"error\"") != NULL
	    || strstr(body, "\"type\": \"error\"") != NULL;
}

/* fgets stops at cap-1 bytes, so an SSE line longer than the read buffer… */
static char *hkHttpReadLine(void *http, char *buf, int cap, char **heap) {
	*heap = NULL;
	if (!hk_http_gets(http, buf, cap)) return NULL;
	size_t len = strlen(buf);
	if (len == 0 || len < (size_t)cap - 1 || buf[len-1] == '\n') return buf;

	size_t hcap = len * 2 + 1024, hlen = len;
	char *h = malloc(hcap);
	if (!h) return buf;                 /* degrade to the truncated line */
	memcpy(h, buf, len + 1);
	while (hk_http_gets(http, buf, cap)) {
		size_t n = strlen(buf);
		if (hlen + n + 1 > hcap) {
			size_t want = (hlen + n + 1) * 2;
			char *t = realloc(h, want);
			if (!t) break;
			h = t; hcap = want;
		}
		memcpy(h + hlen, buf, n + 1);
		hlen += n;
		if (n && buf[n-1] == '\n') break;
		if (n < (size_t)cap - 1) break;
	}
	*heap = h;
	return h;
}

static void hkReportEmpty(aiData *data, const char *body, const char *what) {
	char err[512];
	const char *label = hkProviderLabel();
	if (body && *body) {
		char *reason = hkExtractJsonString(body, "finish_reason");
		char *msg = hkExtractJsonString(body, "message");
		if (strstr(body, "\"choices\"") || strstr(body, "\"content\"")) {
			snprintf(err, sizeof(err),
				"Error: %s answered with no content%s%s%s. Raw: %.150s",
				label,
				reason ? " (finish_reason: " : "", reason ? reason : "", reason ? ")" : "",
				body);
		} else {
			snprintf(err, sizeof(err), "Error: %s — %.200s",
			         label, msg ? msg : body);
		}
		free(reason);
		free(msg);
	} else if (!E.ai_api_key && !E.ai_oauth_refresh
	           && E.ai_provider_type != AI_PROVIDER_MITHRAEUM
	           && E.ai_provider_type != AI_PROVIDER_OLLAMA) {
		snprintf(err, sizeof(err), "Not signed in to %s — run :login %s", label, label);
	} else {
		snprintf(err, sizeof(err),
			"Error: empty %s from %s (network, or the credential expired — :login %s)",
			what, label, label);
	}
	snprintf(hk_last_error, sizeof(hk_last_error), "%s", err);
	aiAddHistory(data, err);
}

/* ---------- what the model reads every turn (DESIGN 2026-10-29 §3) ----------
   A write_file or edit_* call carries the whole body, and it rode along in
   every later request. Once a later turn exists the file is on disk, so the
   body becomes a receipt: "(wrote N bytes to path)". Notices, command output
   and ledger lines never reach the model at all (only user, assistant and tool
   rows are model messages). */
#define HK_SLIM_MIN 200
static const char *hkSlimEnd(const char *v, int esc) {      /* past a JSON string's closing quote */
	for (const char *q = v; *q; q++) {
		if (!esc) {
			if (*q == '\\') { if (!q[1]) return NULL; q++; continue; }
			if (*q == '"') return q + 1;
		} else {                                             /* inside an escaped JSON string (openai arguments) */
			if (q[0] == '\\' && q[1] == '\\') { q += 2; if (*q == '\\' && q[1]) q++; continue; }
			if (q[0] == '\\' && q[1] == '"') return q + 2;
		}
	}
	return NULL;
}

static char *hkSlimOne(const char *s) {
	static const char *TOOLS[] = { "write_file", "edit_file", "edit_lines", "create_file", NULL };
	static const char *KEYS[]  = { "content", "contents", "file_text", "new_str", "new", "old_str", "old", "code", NULL };
	const char *first = NULL;
	for (int i = 0; TOOLS[i]; i++) { const char *t = strstr(s, TOOLS[i]); if (t && (!first || t < first)) first = t; }
	if (!first) return NULL;
	size_t cap = strlen(s) + 1, o = 0;
	char *out = malloc(cap);
	if (!out) return NULL;
	int changed = 0;
	const char *p = s;
	while (*p) {
		const char *best = NULL, *bk = NULL;
		int besc = 0;
		for (int e = 0; e < 2; e++) for (int k = 0; KEYS[k]; k++) {
			char pat[32];
			snprintf(pat, sizeof pat, e ? "\\\"%s\\\"" : "\"%s\"", KEYS[k]);
			const char *h = strstr(p, pat);
			if (h && h > first && (!best || h < best)) { best = h; bk = KEYS[k]; besc = e; }
		}
		if (!best) break;
		const char *v = best + strlen(bk) + (besc ? 4 : 2);
		while (*v == ' ' || *v == ':') v++;
		const char *open = v;
		if (besc ? strncmp(v, "\\\"", 2) : *v != '"') { size_t n = (size_t)(v - p); memcpy(out + o, p, n); o += n; p = v; continue; }
		v += besc ? 2 : 1;
		const char *end = hkSlimEnd(v, besc);
		size_t vlen = end ? (size_t)(end - v) : 0;
		if (!end || vlen < HK_SLIM_MIN) { size_t n = (size_t)(v - p); memcpy(out + o, p, n); o += n; p = v; continue; }
		/* the path this body went to: the nearest "path" before it */
		char path[160] = "file";
		for (const char *q = best; q > s; q--) {
			if (!strncmp(q, "\"path\"", 6) || !strncmp(q, "\\\"path\\\"", 8)) {
				const char *pv = q + (q[0] == '\\' ? 8 : 6);
				while (*pv == ' ' || *pv == ':' || *pv == '\\' || *pv == '"') pv++;
				size_t pl = 0;
				while (pv[pl] && pv[pl] != '"' && pv[pl] != '\\' && pl < sizeof path - 1) pl++;
				if (pl) snprintf(path, sizeof path, "%.*s", (int)pl, pv);
				break;
			}
		}
		size_t n = (size_t)(open - p);
		memcpy(out + o, p, n); o += n;
		size_t bytes = 0;                    /* the file's size, not the escaped string's */
		for (const char *q = v; q < end - (besc ? 2 : 1); q++, bytes++) {
			if (*q != '\\') continue;
			if (besc && q[1] == '\\') { q += 2; if (*q == '\\') q++; }
			else q++;
		}
		char rec[256];
		snprintf(rec, sizeof rec, "%s(wrote %zu bytes to %s)%s", besc ? "\\\"" : "\"", bytes, path, besc ? "\\\"" : "\"");
		size_t rl = strlen(rec);
		if (o + rl + 1 > cap) { cap = o + rl + strlen(end) + 1; char *nb = realloc(out, cap); if (!nb) { free(out); return NULL; } out = nb; }
		memcpy(out + o, rec, rl); o += rl;
		p = end;
		changed = 1;
	}
	size_t rest = strlen(p);
	if (o + rest + 1 > cap) { char *nb = realloc(out, o + rest + 1); if (!nb) { free(out); return NULL; } out = nb; }
	memcpy(out + o, p, rest + 1);
	/* the raw <write_file path="p">body</write_file> channel has no JSON to slim */
	for (char *w = strstr(out, "<write_file"); w; w = strstr(w + 11, "<write_file")) {
		char *gt = strchr(w, '>'), *close = gt ? strstr(gt, "</write_file>") : NULL;
		if (!close || (size_t)(close - gt - 1) < HK_SLIM_MIN) continue;
		char rec[64];
		int rn = snprintf(rec, sizeof rec, "(wrote %ld bytes)", (long)(close - gt - 1));
		memmove(gt + 1 + rn, close, strlen(close) + 1);
		memcpy(gt + 1, rec, (size_t)rn);
		changed = 1;
	}
	if (!changed) { free(out); return NULL; }
	return out;
}

static void hkSlimOldWrites(aiData *data) {
	for (int i = 0; i < data->message_count; i++) {
		aiMessage *m = &data->messages[i];
		if (!m->role || strcmp(m->role, "assistant") || !m->content) continue;
		char *slim = hkSlimOne(m->content);
		if (slim) { free(m->content); m->content = slim; }
	}
}

static int hk_turn_closed;   /* the turn reached its normal ending */
static void *aiWorkerTurn(void *arg) {
	hkBlockWinch();
	hk_turn_spoke = 0;
	aiData *data = (aiData *)arg;

#if !defined(_WIN32) && !defined(HAKO_WASM)
	if (g_prewarm_on) {
		data->anim_verb = "warming the model";
		clStartAnim(data);
		hkPrewarmJoin();
		clStopAnim(data);
		data->anim_verb = NULL;
	}
#endif
	hkPrewarmJoin();

	data->turn_start_ms = clWallMs();

	pthread_mutex_lock(&data->lock);
	char *prompt = data->current_prompt ? strdup(data->current_prompt) : NULL;
	if (prompt) hkSlimOldWrites(data);       /* earlier turns' file bodies are on disk now */
	if (prompt) aiPushMessage(data, "user", prompt);
	pthread_mutex_unlock(&data->lock);
	if (prompt && !hkImageWire() && strstr(prompt, "[image #")) {
		char probe[PATH_MAX + 64];
		const char *m = strstr(prompt, "[image #");
		if (hkImageFind(atoi(m + 8), probe, sizeof probe) >= 0)
			aiSayf(data, "%s cannot see images, so it gets the words only", E.ai_model ? E.ai_model : "this model");
	}
	free(prompt);

	int max_iters = 25;
	{ const char *mi = getenv("HAKO_MAX_ITERS"); if (mi && *mi) { int v = atoi(mi); if (v >= 1 && v <= 40) max_iters = v; } }
	/* One move per task: the bench asks what the model does FIRST, not whether
	   it eventually stumbles onto the right call after three tries. */
	if (hk_bench_mode) max_iters = 1;
	int iter = 0;
	int used_tool = 0;
	int repaired = 0;
	int oriented = 0;
	char **xseen = NULL; int xseen_n = 0, xseen_cap = 0;
	int loop_warned = 0;
	int token_retried = 0;
	int cache_retried = 0;
	int stream_retried = 0;
	hk_last_error[0] = '\0';   /* a stale reason must not be reported for a new turn */
	hkRepeatReset();
	/* Fence state is global; one reply that opens ``` and never closes it used
	   to leave every later turn rendered as code. */
	cl_in_codefence = 0;
	hk_turn_edited_n = 0;

	if (E.ai_tools_enabled && !hkProjectTrusted() && !E.pipe_mode && !E.output_json
	    && !hk_bench_mode && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)
	    && hkLooksActionable(data->current_prompt)) {
		clStopAnim(data);
		char cwd[PATH_MAX];
		if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), "this folder");
		char q[PATH_MAX + 96];
		snprintf(q, sizeof(q), "that needs file access to %s — allow it?", cwd);
		if (clPromptYN(q, 1) && hkGrantProjectTrust()) {
			hkPromptInvalidate();
			hkLoadSkills(data);          /* the prompt changes once tools exist */
			pthread_mutex_lock(&data->lock);
			aiAddHistory(data, "trusted. hako can read and edit files here (:trust revoke to undo).");
			pthread_mutex_unlock(&data->lock);
		} else {
			pthread_mutex_lock(&data->lock);
			aiAddHistory(data, "staying read-only. :trust when you want file access.");
			pthread_mutex_unlock(&data->lock);
		}
		clStartAnim(data);
	}

	if (E.ai_provider_type == AI_PROVIDER_MITHRAEUM && !E.pipe_mode && !E.output_json
	    && E.session_turn_count <= 1 && data->system_prompt) {
		long ptok = (long)((strlen(data->system_prompt) + 3) / 4);
		pthread_mutex_lock(&data->lock);
		aiSayf(data, "first turn reads the %ld-token system prompt into the model; "
		             "later turns reuse it and are much faster.", ptok);
		pthread_mutex_unlock(&data->lock);
	}

	while (iter++ < max_iters) {
		if (E.interrupt) break;          /* :q or ^C landed mid-turn */
		/* Nothing further will change if the model is asking for the same
		   thing again; spend the remaining iterations on nothing. */
		if (hk_repeat_count > HK_REPEAT_LIMIT) break;

		if (hkContextPercent(data) >= HK_CTX_TRIGGER &&
		    data->message_count > data->compact_floor) {
			int before = hkContextPercent(data);
			int folded = hkCompactHistory(data);
			data->compact_floor = data->message_count + 4;
			if (folded > 0) {
				pthread_mutex_lock(&data->lock);
				aiSayf(data, "(context was %d%% full — folded %d earlier messages into a summary; now %d%%)",
				       before, folded, hkContextPercent(data));
				pthread_mutex_unlock(&data->lock);
			}
		}
		data->turn_index++;
		clStartAnim(data);

		clOAuthEnsureFresh(data);

		if (E.ai_provider_type == AI_PROVIDER_MITHRAEUM) {
			char *herr = NULL;
			char *content = hkMithraeumChat(data, &herr);
			clStopAnim(data);
			if (content) {
				char *fixed = hkNormalizeToolDialect(content);
				if (fixed) { free(content); content = fixed; }
			}
			if (!content) {
				/* -p --output-format json reports this; it said "empty response" */
				snprintf(hk_last_error, sizeof(hk_last_error), "%s", herr ? herr : "Error: hako engine failed");
				pthread_mutex_lock(&data->lock);
				aiAddHistory(data, herr ? herr : "Error: hako engine failed");
				data->streaming = 0;
				pthread_mutex_unlock(&data->lock);
				free(herr);
				return NULL;
			}

			pthread_mutex_lock(&data->lock);
			if (*content) {
				char *disp = hkHasToolBlock(content) ? NULL : hkStripToolBlocks(content);
				if (data->suppress_next_echo || !disp || !*disp) hkPhoneFinal(content);
				if (disp && *disp) aiAddHistoryRole(data, disp, HK_ROLE_AI);
				free(disp);
				hkLogMessage("assistant", content);
				free(data->current_response);
				data->current_response = strdup(content);
			}
			pthread_mutex_unlock(&data->lock);

			if (*content && hkHasToolBlock(content)) {
				char *hist = hkStripCodeFences(content);
				pthread_mutex_lock(&data->lock);
				aiPushMessage(data, "assistant", hist ? hist : content);
				pthread_mutex_unlock(&data->lock);
				free(hist);
				int dups = 0, truncated = 0;
				int nt = hkReactToolExecAll(data, content, &xseen, &xseen_n, &xseen_cap, &dups, &truncated);
				free(content);
				if (nt == 0) {
					if (truncated && !repaired) {
						repaired = 1;
						pthread_mutex_lock(&data->lock);
						aiPushMessage(data, "user",
							"Your <write_file> block was cut off before </write_file> — it ran past the "
							"length limit. Reply with ONLY the write_file block (no explanation, no ``` "
							"fence before it), keep the body complete, and close it with </write_file>.");
						pthread_mutex_unlock(&data->lock);
						continue;
					}
					if (dups > 0) {
						if (!loop_warned) { loop_warned = 1; continue; }
						break;
					}
					if (!repaired) {
						repaired = 1;
						pthread_mutex_lock(&data->lock);
						aiPushMessage(data, "user",
							"Your tool call did not parse. Re-emit exactly ONE valid call and nothing else: "
							"<tool_call>{\"name\": \"...\", \"arguments\": {...}}</tool_call> with strict JSON, "
							"or to write a file <write_file path=\"...\">raw body</write_file>.");
						pthread_mutex_unlock(&data->lock);
						continue;
					}
					break;
				}
				used_tool = 1;
				repaired = 0;
				continue;
			}

			pthread_mutex_lock(&data->lock);
			if (*content) aiPushMessage(data, "assistant", content);
			/* After tools ran, saying nothing more is how a small model ends a
			   finished task; calling that an error put "Error" under a success. */
			else if (!used_tool) aiAddHistory(data, "Error: empty response from hako engine");
			pthread_mutex_unlock(&data->lock);

			if (!repaired && !used_tool && *content
			    && (strstr(content, "</tool_call>") || strstr(content, "<tool_call")
			        || (strstr(content, "\"name\"") && strstr(content, "\"arguments\"")))) {
				repaired = 1;
				pthread_mutex_lock(&data->lock);
				aiPushMessage(data, "user",
					"That did not parse as a tool call. Re-emit exactly ONE call and nothing else, "
					"with BOTH tags: <tool_call>{\"name\": \"...\", \"arguments\": {...}}</tool_call>");
				pthread_mutex_unlock(&data->lock);
				continue;
			}

			if (!repaired && !used_tool && *content
			    && hkShouldNudgeWrite(content, data->current_prompt)) {
				repaired = 1;
				char *fence = hk_last_write_path[0] ? hkExtractBestFence(content) : NULL;
				if (fence) {
					int lines = 1; for (const char *q = fence; *q; q++) if (*q == '\n') lines++;
					if (lines < 3 || strlen(fence) < 80) { free(fence); fence = NULL; }
				}
				if (fence) {
					size_t pn = strlen(hk_last_write_path), fn = strlen(fence);
					char *ep = malloc(pn * 6 + 8), *ec = malloc(fn * 6 + 8);
					char *json = malloc(pn * 6 + fn * 6 + 32);
					if (ep && ec && json) {
						hkJsonEscapeInto(hk_last_write_path, ep, (int)pn * 6 + 8);
						hkJsonEscapeInto(fence, ec, (int)fn * 6 + 8);
						snprintf(json, pn * 6 + fn * 6 + 32, "{\"path\":\"%s\",\"content\":\"%s\"}", ep, ec);
						hkAnnounceTool(data, "write_file", json);
						char *result = hkExecTool("write_file", json);
						hkAnnounceToolResult(data, result);
						size_t rl = result ? strlen(result) : 0;
						char *obs = malloc(rl + 80);
						if (obs) {
							snprintf(obs, rl + 80, "<observation tool=\"write_file\">%s</observation>", result ? result : "");
							pthread_mutex_lock(&data->lock);
							aiPushMessage(data, "user", obs);
							pthread_mutex_unlock(&data->lock);
							free(obs);
						}
						free(result);
						used_tool = 1;
					}
					free(ep); free(ec); free(json); free(fence);
					free(content);
					continue;
				}
				pthread_mutex_lock(&data->lock);
				aiPushMessage(data, "user",
					"You showed the code in a markdown block but did NOT create the file. "
					"Emit it now as a single <write_file path=\"FILENAME\"> block holding the "
					"complete file body (real newlines, no escaping, no ``` fences) and nothing else.");
				pthread_mutex_unlock(&data->lock);
				free(content);
				continue;
			}

			if (!repaired && !used_tool && *content
			    && hkClaimedWriteNoAct(content, data->current_prompt)) {
				repaired = 1;
				pthread_mutex_lock(&data->lock);
				aiPushMessage(data, "user",
					"You said the file was written/updated, but you did NOT call write_file — "
					"nothing was saved to disk. Emit the file now as a single "
					"<write_file path=\"FILENAME\">complete file body</write_file> block and nothing else.");
				pthread_mutex_unlock(&data->lock);
				free(content);
				continue;
			}

			if (!oriented && !used_tool && *content && hkProjectTrusted()
			    && hkLooksActionable(data->current_prompt)) {
				oriented = 1;
				if (hkRunOneTool(data, "list_dir", "{\"path\":\".\"}",
				                 &xseen, &xseen_n, &xseen_cap, NULL)) {
					used_tool = 1;
					free(content);
					continue;
				}
			}
			free(content);
			break;
		}

		pthread_mutex_lock(&data->lock);
		hkReqParts parts;
		int built = aiBuildRequest(data, E.ai_provider_type, &parts);
		pthread_mutex_unlock(&data->lock);

		if (built != 0) {
			clStopAnim(data);
			pthread_mutex_lock(&data->lock);
			char msg[384];
			const char *pname = hkProviderLabel();
			if (E.ai_provider_type == AI_PROVIDER_NONE) {
				snprintf(msg, sizeof(msg), "Error: no provider set. Run /provider <name> or /login <name>.");
			} else if (!E.ai_api_key && !HK_IS_OLLAMA_WIRE(E.ai_provider_type)) {
				snprintf(msg, sizeof(msg),
					"Error: missing api key for %s. Run /login %s, or set %s_API_KEY env var.",
					pname, pname,
					(E.ai_provider_type == AI_PROVIDER_ANTHROPIC) ? "ANTHROPIC" :
					(E.ai_provider_type == AI_PROVIDER_OPENAI) ? "OPENAI / GOOGLE / GROQ / etc"
					: "PROVIDER");
			} else {
				snprintf(msg, sizeof(msg), "Error: provider %s not configured (model=%s, endpoint=%s).",
					pname,
					E.ai_model ? E.ai_model : "(unset)",
					E.ai_endpoint ? E.ai_endpoint : "(default)");
			}
			aiAddHistory(data, msg);
			data->streaming = 0;
			pthread_mutex_unlock(&data->lock);
			return NULL;
		}

		hkHttpReq hreq = { .method = "POST", .url = parts.url, .headers = parts.headers,
		                   .body = parts.body, .stream = parts.stream };
		void *http = hk_http_open(&hreq);
		free(parts.url); free(parts.headers); free(parts.body);
		if (!http) {
			clStopAnim(data);
			pthread_mutex_lock(&data->lock);
			aiAddHistory(data, "Error: could not start the request");
			data->streaming = 0;
			pthread_mutex_unlock(&data->lock);
			return NULL;
		}

		int oauth_anth_resp = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic");
		/* ChatGPT's Responses stream rides the same prose path as Claude's */
		int chatgpt_resp = E.ai_provider_type == AI_PROVIDER_OPENAI && hkOAuthIs("chatgpt");
		int streaming_mode = chatgpt_resp || ((E.ai_provider_type == AI_PROVIDER_ANTHROPIC) &&
			(oauth_anth_resp || (E.ai_stream && !(E.ai_tools_enabled && hkProjectTrusted()))));
		int openai_stream = (E.ai_provider_type == AI_PROVIDER_OPENAI) && E.ai_stream && !chatgpt_resp;

		char buffer[8192];
		char *full_response = NULL;
		size_t total = 0, fr_cap = 0;   /* hkSbAdd doubles; growing by hand was O(n^2) */

		if (streaming_mode) {
			char *acc = calloc(1, 1);
			size_t acc_len = 0, acc_cap = 1;
			int printed_prefix = 0;
			/* Live, like the local engine: the text dialect (what Claude uses over
			   OAuth) goes quiet at its first tag and the partial line is taken back.
			   Holding the whole answer back instead showed nothing until the end,
			   then printed it off the rail — and dropped any prose before a tool. */
			int quiet = 0;
			clStreamLine sline = { {0}, 0, 0, 0, 0 };

			char *lheap = NULL, *line;
			for (;;) {
				free(lheap); lheap = NULL;          /* freed at the TOP, so every `continue` below is safe */
				if (E.interrupt) break;
				line = hkHttpReadLine(http, buffer, sizeof(buffer), &lheap);
				if (!line) break;
				hkSbAdd(&full_response, &total, &fr_cap, line, strlen(line));

				if (strncmp(line, "data: ", 6) != 0) continue;
				const char *payload = line + 6;
				if (!strstr(payload, chatgpt_resp ? "\"response.output_text.delta\"" : "content_block_delta"))
					continue;
				char *raw = hkExtractJsonString(payload, chatgpt_resp ? "delta" : "text");
				if (!raw) continue;
				char *text = hkJsonUnescape(raw, (int)strlen(raw));
				free(raw);
				if (!text) continue;

				size_t tl = strlen(text);
				hkSbAdd(&acc, &acc_len, &acc_cap, text, tl);

				if (!quiet) {
					const char *win = acc + (acc_len > tl + 24 ? acc_len - tl - 24 : 0);
					if (strstr(win, "<tool") || strstr(win, "<write_file") || strstr(win, "<invoke")
					 || strstr(win, "<function_calls")) {
						quiet = 1;
						if (printed_prefix) { clStreamUnecho(&sline); sline.len = 0; clStreamPark(&sline); }
					}
				}
				if (!quiet && !E.pipe_mode && !E.output_json) {
					if (!printed_prefix) { clStopAnim(data); printed_prefix = 1; }
					clStreamPush(&sline, text);
				} else if (!quiet && E.serve_mode) hkDeltaPush(text);
				free(text);
				/* no stop sequences on this API: hang up at the call ourselves,
				   before the model writes on and invents the result */
				if (chatgpt_resp && quiet && strstr(acc, "</tool>")) break;
			}
			free(lheap);
			hk_http_close(http);
			clStopAnim(data);
			if (full_response) full_response[total] = '\0';

			int has_tool_call_in_acc = acc_len > 0 && hkHasToolBlock(acc);
			if (!E.pipe_mode && printed_prefix) {
				if (quiet) { clStreamUnecho(&sline); sline.len = 0; }
				clStreamEnd(&sline);
			}
			if ((E.pipe_mode || hs_attached) && acc_len > 0) {   /* the pane gets the prose, tool blocks stripped */
				const char *render_start = acc;
				const char *render_end = acc + acc_len;

				size_t win = render_end - render_start;
				char *clean = malloc(win + 1);
				size_t ci = 0;
				const char *p = render_start;
				while (p < render_end) {
					if (!strncmp(p, "<function_calls>", 16)) {
						const char *fce = strstr(p, "</function_calls>");
						if (fce) { p = fce + 17; continue; }
						break;
					}
					if (!strncmp(p, "<tool", 5)) {
						const char *tce = strstr(p, "</tool>");
						if (tce) { p = tce + 7; continue; }
						break;
					}
					if (!strncmp(p, "<parameter", 10)) {
						const char *pce = strstr(p, "</parameter>");
						if (pce) { p = pce + 12; continue; }
						break;
					}
					if (!strncmp(p, "<observation", 12)) {
						const char *oce = strstr(p, "</observation>");
						if (oce) { p = oce + 14; continue; }
						break;
					}
					if (!strncmp(p, "<invoke", 7)) {
						const char *ice = strstr(p, "</invoke>");
						if (ice) { p = ice + 9; continue; }
						break;
					}
					clean[ci++] = *p++;
				}
				size_t cs = 0; while (cs < ci && (clean[cs] == ' ' || clean[cs] == '\n' || clean[cs] == '\t' || clean[cs] == '\r')) cs++;
				while (ci > cs && (clean[ci-1] == ' ' || clean[ci-1] == '\n' || clean[ci-1] == '\t' || clean[ci-1] == '\r')) ci--;
				if (ci > cs) {
					char save = clean[ci];
					clean[ci] = '\0';
					clPipeEmitMsg("ai", clean + cs);
					clean[ci] = save;
				}
				free(clean);
			}

			if (acc_len > 0) {
				char *fixed = hkNormalizeToolDialect(acc);
				if (fixed) { free(acc); acc = fixed; acc_len = strlen(acc); }
				pthread_mutex_lock(&data->lock);
				hkUpdateUsage(data, full_response);
				aiPushHistoryStore(data, acc, HK_ROLE_AI);
				aiPushMessage(data, "assistant", acc);
				hkLogMessage("assistant", acc);
				free(data->current_response);
				data->current_response = acc;
				pthread_mutex_unlock(&data->lock);

				int prose_active = (E.ai_toolmode == 1)
					|| hkSubProse()
					|| (E.ai_provider_type == AI_PROVIDER_MITHRAEUM);
				if (prose_active && hkHasToolBlock(acc)) {
					int dups = 0;
					int n = hkReactToolExecAll(data, acc, &xseen, &xseen_n, &xseen_cap, &dups, NULL);
					if (n > 0) {
						free(full_response);
						used_tool = 1;
						continue;
					}
					if (has_tool_call_in_acc && !dups) {
						pthread_mutex_lock(&data->lock);
						aiAddHistoryRole(data, acc, HK_ROLE_AI);
						aiAddHistory(data, "(that looked like a tool call but did not parse — "
						                   ":toolmode native may work better with this model)");
						pthread_mutex_unlock(&data->lock);
					}
				}
			} else {
				free(acc);
			if (E.ai_prompt_cache && !cache_retried && full_response &&
			    strstr(full_response, "cache_control")) {
				cache_retried = 1;
				E.ai_prompt_cache = 0;
				if (E.debug) fprintf(stderr, "[cache] host rejected cache_control; retrying without it\n");
				free(full_response);
				continue;
			}
				if (E.ai_oauth_refresh && !token_retried) {
					token_retried = 1;
					free(full_response);
					clOAuthRefresh(data);
					continue;
				}
				if (!stream_retried && hkRetryableStreamError(full_response)) {
					stream_retried = 1;
					if (E.debug) fprintf(stderr, "[stream] retryable provider error; retrying once\n");
					free(full_response);
					cl_sleep_ms(1200);
					continue;
				}
				pthread_mutex_lock(&data->lock);
				hkReportEmpty(data, full_response, "stream");
				pthread_mutex_unlock(&data->lock);
			}

			free(full_response);
			break;
		}

		if (openai_stream) {
			hkSseAcc acc;
			memset(&acc, 0, sizeof(acc));
			int printed_prefix = 0;
			clStreamLine sline = { {0}, 0, 0, 0, 0 };
			int saw_data = 0;
			char *oheap = NULL, *oline;
			for (;;) {
				free(oheap); oheap = NULL;      /* freed at the TOP: every `continue` below is safe */
				if (E.interrupt) break;
				oline = hkHttpReadLine(http, buffer, sizeof(buffer), &oheap);
				if (!oline) break;
				if (!strncmp(oline, "data:", 5)) saw_data = 1;
				else if (!saw_data) {
					hkSbAdd(&full_response, &total, &fr_cap, oline, strlen(oline));
					continue;
				}
				char *chunk = hkSseFeed(&acc, oline);
				if (!chunk) continue;
				if (!E.pipe_mode && !E.output_json) {
					if (!printed_prefix) { clStopAnim(data); printed_prefix = 1; }
					clStreamPush(&sline, chunk);
				} else if (E.serve_mode) hkDeltaPush(chunk);
				free(chunk);
			}
			free(oheap);
			hk_http_close(http);
			clStopAnim(data);
			if (printed_prefix) clStreamEnd(&sline);
			if (saw_data) {
				free(full_response);
				full_response = hkSseFinish(&acc);
				total = full_response ? strlen(full_response) : 0;
				fr_cap = total ? total + 1 : 0;
			} else if (full_response) {
				full_response[total] = '\0';
			}
			hkSseFree(&acc);
			/* The prose was already shown live; printing it again from the
			   rebuilt body would double every answer. */
			if (saw_data && printed_prefix) data->suppress_next_echo = 1;
		} else {
			while (!E.interrupt && hk_http_gets(http, buffer, sizeof(buffer)))
				hkSbAdd(&full_response, &total, &fr_cap, buffer, strlen(buffer));
			hk_http_close(http);
			clStopAnim(data);
			if (full_response) full_response[total] = '\0';
		}

		if (E.debug && full_response) {
			fprintf(stderr, "\n[debug] response (%zu bytes):\n%s\n[/debug]\n", total, full_response);
		}

		if (E.ai_prompt_cache && !cache_retried && full_response &&
		    strstr(full_response, "\"error\"") && strstr(full_response, "cache_control")) {
			cache_retried = 1;
			E.ai_prompt_cache = 0;
			if (E.debug) fprintf(stderr, "[cache] host rejected cache_control; retrying without it\n");
			free(full_response);
			continue;
		}

		int tool_use_anthropic = E.ai_provider_type == AI_PROVIDER_ANTHROPIC
			&& full_response
			&& strstr(full_response, "\"stop_reason\":\"tool_use\"");
		int tool_use_fn = (E.ai_provider_type == AI_PROVIDER_OLLAMA || E.ai_provider_type == AI_PROVIDER_OPENAI)
			&& full_response
			&& strstr(full_response, "\"tool_calls\":[");

		int api_err;
		char *content = aiExtractResponse(full_response, E.ai_provider_type, &api_err);
		if (api_err) { free(content); content = NULL; }   /* not the model's answer: hkReportEmpty says it */

		pthread_mutex_lock(&data->lock);
		hkUpdateUsage(data, full_response);
		if (content && *content) {
			if (data->suppress_next_echo) hkPhoneFinal(content);
			aiAddHistoryRole(data, content, HK_ROLE_AI);
			hkLogMessage("assistant", content);
			free(data->current_response);
			data->current_response = strdup(content);
		}
		pthread_mutex_unlock(&data->lock);

		if (tool_use_anthropic) {
			char *content_array = hkExtractContentArray(full_response);
			if (!content_array) { free(content); free(full_response); break; }

			pthread_mutex_lock(&data->lock);
			aiPushMessageRaw(data, "assistant", content_array);
			pthread_mutex_unlock(&data->lock);

			char *results = hkBuildToolResults(data, content_array);

			pthread_mutex_lock(&data->lock);
			aiPushMessageRaw(data, "user", results);
			pthread_mutex_unlock(&data->lock);

			free(content_array);
			free(results);
			free(content);
			free(full_response);
			used_tool = 1;
			continue;
		}

		if (tool_use_fn) {
			int n = hkFnToolExecAll(data, full_response);
			free(content);
			free(full_response);
			if (n == 0) break;
			used_tool = 1;
			continue;
		}

		int prose_active = (E.ai_toolmode == 1)
			|| hkSubProse()
			|| (E.ai_provider_type == AI_PROVIDER_MITHRAEUM);
		if (content && prose_active && hkHasToolBlock(content)) {
			pthread_mutex_lock(&data->lock);
			aiPushMessage(data, "assistant", content);
			pthread_mutex_unlock(&data->lock);
			int dups = 0;
			int n = hkReactToolExecAll(data, content, &xseen, &xseen_n, &xseen_cap, &dups, NULL);
			free(content);
			free(full_response);
			if (n == 0) break;
			used_tool = 1;
			continue;
		}

		if (content) {
			pthread_mutex_lock(&data->lock);
			aiPushMessage(data, "assistant", content);
			pthread_mutex_unlock(&data->lock);
		} else {
			pthread_mutex_lock(&data->lock);
			if (full_response && strstr(full_response, "llama runner process has terminated")) {
				aiAddHistory(data, "Error: Ollama runner crashed (usually OOM or model corrupt).");
				aiAddHistory(data, "  try: smaller model (/model llama3.2:3b), restart `ollama serve`,");
				aiAddHistory(data, "  or `ollama rm <model> && ollama pull <model>` to refresh.");
			} else if (full_response && strstr(full_response, "model requires more system memory")) {
				aiAddHistory(data, "Error: model too large for available RAM. Pick a smaller variant.");
			} else if (full_response && strstr(full_response, "model not found")) {
				aiAddHistory(data, "Error: model not installed. Run `ollama pull <name>` first.");
			} else {
				hkReportEmpty(data, full_response, "response");
			}
			pthread_mutex_unlock(&data->lock);
		}

		free(content);
		free(full_response);
		break;
	}

	for (int i = 0; i < xseen_n; i++) free(xseen[i]);
	free(xseen);

	pthread_mutex_lock(&data->lock);
	if (hk_repeat_count > HK_REPEAT_LIMIT)
		aiAddHistory(data, "(stopped: the same tool call was repeating with no progress)");
	else if (iter >= max_iters && used_tool) aiAddHistory(data, "(tool loop cap reached)");
	data->last_turn_ms = clWallMs() - data->turn_start_ms;
	data->streaming = 0;
	pthread_mutex_unlock(&data->lock);
	clSayWorkedFor(data, data->last_turn_ms);
#if !defined(_WIN32) && !defined(HAKO_WASM)
	/* Wake whoever is composing the next prompt: their read() is blocking and
	   has no other way to learn the turn is over. */
	if (g_main_thread_set) pthread_kill(g_main_thread, SIGUSR1);
#endif

	if (E.pipe_mode || hs_attached) {
		if (!hk_turn_spoke && data->current_response && *data->current_response) {
			char *norm = hkNormalizeToolDialect(data->current_response);
			clPipeEmitMsg("ai", norm ? norm : data->current_response);
			free(norm);
		}
		clPipeEmitDone(data);
	}

	hk_turn_closed = 1;
	return NULL;
}

/* A turn that errors out returns early, before the ending above: no "done" on the
   pipe (Rei and the studio waited at "Listening..." forever after any engine
   failure) and no wake for the keyboard reader. Every exit gets one. */
static void *aiWorkerThread(void *arg) {
	aiData *data = arg;
	hk_turn_closed = 0;
	aiWorkerTurn(arg);
	if (!hk_turn_closed) {
		pthread_mutex_lock(&data->lock);
		data->streaming = 0;
		pthread_mutex_unlock(&data->lock);
#if !defined(_WIN32) && !defined(HAKO_WASM)
		if (g_main_thread_set) pthread_kill(g_main_thread, SIGUSR1);
#endif
		if (E.pipe_mode || hs_attached) clPipeEmitDone(data);
	}
	return NULL;
}

static void aiWorkerSend(aiData *data) {
	if (!data || data->streaming) return;
	hkLoadSkills(data);                     /* no-op unless trust, mode or a skill marked it stale */
	data->streaming = 1;
#ifdef HAKO_WASM
	aiWorkerThread(data);
#else
	if (pthread_create(&data->worker_thread, NULL, aiWorkerThread, data) == 0) {
	} else {
		data->streaming = 0;
	}
#endif
}

static int hkPickModel(aiData *data, char *out, size_t cap) {
	static char names[HK_MODELS_MAX][96];
	static const char *items[HK_MODELS_MAX];
	int mn = hkGatherModels(names, HK_MODELS_MAX);
	if (mn <= 0) {
		if (E.ai_provider_type == AI_PROVIDER_MITHRAEUM)
			aiAddHistory(data, "no local models installed. :pull hako-sho to add one.");
		else
			aiSayf(data, "no model list for %s. :model <id> sets one", hkProviderLabel());
		return 0;
	}
	int cur = 0;
	for (int i = 0; i < mn; i++) {
		items[i] = names[i];
		if (E.ai_model && !strcmp(E.ai_model, names[i])) cur = i;
	}
	int pick = clPopupSelect("model", items, mn, cur, NULL);
	if (pick < 0) return 0;
	snprintf(out, cap, "%s", names[pick]);
	return 1;
}

static int hkPickProvider(char *out, size_t cap) {
	static const char *provs[] = {
		"mithraeum", "anthropic", "chatgpt", "openai", "gemini", "ollama", "groq",
		"cerebras", "deepseek", "mistral", "together", "fireworks",
		"openrouter", "nous", "xai", "custom"
	};
	int pn = (int)(sizeof(provs) / sizeof(provs[0]));
	int cur = 0;
	const char *active = hkProviderLabel();
	for (int i = 0; i < pn; i++) if (!strcmp(provs[i], active)) { cur = i; break; }
	int pick = clPopupSelect("provider", provs, pn, cur, NULL);
	if (pick < 0) return 0;
	snprintf(out, cap, "%s", provs[pick]);
	return 1;
}

static int hkToggle(aiData *data, const char *arg, int *flag, const char *label, const char *note) {
	int changed = 0;
	if (arg && !strcmp(arg, "on"))       { *flag = 1; changed = 1; }
	else if (arg && !strcmp(arg, "off")) { *flag = 0; changed = 1; }
	else if (!arg || !*arg)              { *flag = !*flag; changed = 1; }
	if (changed) hkSaveSession();
	aiSayf(data, "%s: %s%s%s", label, *flag ? "on" : "off",
	       changed ? " (saved)" : "", note ? note : "");
	return 1;
}

/* ---------- Agent Skills: install from a GitHub repo ---------- */

#define HK_TAP_MAX_FILES 24
#define HK_TAP_MAX_BYTES 200000

/* Walk the top-level objects of a JSON array. */
static const char *hkJsonArrayNext(const char **p, size_t *out_len) {
	const char *s = *p;
	while (*s && *s != '{') s++;
	if (!*s) { *p = s; return NULL; }
	const char *start = s;
	int depth = 0, in_str = 0, esc = 0;
	for (; *s; s++) {
		if (esc) { esc = 0; continue; }
		if (*s == '\\') { esc = 1; continue; }
		if (*s == '"') { in_str = !in_str; continue; }
		if (in_str) continue;
		if (*s == '{') depth++;
		else if (*s == '}') { depth--; if (!depth) { s++; break; } }
	}
	*out_len = (size_t)(s - start);
	*p = s;
	return start;
}

/* owner/repo[/path...] — the shape every skills client accepts. */
static int hkParseRepoSpec(const char *spec, char *owner, size_t ocap,
                           char *repo, size_t rcap, char *path, size_t pcap) {
	owner[0] = repo[0] = path[0] = '\0';
	if (!spec || !*spec) return -1;
	if (strstr(spec, "://")) return -1;              /* a plain URL, handled elsewhere */
	const char *a = strchr(spec, '/');
	if (!a || a == spec) return -1;
	const char *b = strchr(a + 1, '/');
	size_t olen = (size_t)(a - spec);
	size_t rlen = b ? (size_t)(b - a - 1) : strlen(a + 1);
	if (!olen || !rlen || olen >= ocap || rlen >= rcap) return -1;
	memcpy(owner, spec, olen); owner[olen] = '\0';
	memcpy(repo, a + 1, rlen); repo[rlen] = '\0';
	if (b && b[1]) {
		const char *pp = b + 1;
		while (*pp == '/') pp++;          /* a doubled slash is a typo, not an attack */
		snprintf(path, pcap, "%s", pp);
	}
	/* This becomes a GitHub API path, and `name` (validated separately) is what
	   reaches the filesystem — but a climbing path is never intended, so refuse. */
	if (strstr(path, "..")) return -1;
	for (const char *q = owner; *q; q++) if (!isalnum((unsigned char)*q) && *q != '-' && *q != '_' && *q != '.') return -1;
	for (const char *q = repo;  *q; q++) if (!isalnum((unsigned char)*q) && *q != '-' && *q != '_' && *q != '.') return -1;
	return 0;
}

static char *hkGhContents(const char *owner, const char *repo, const char *path) {
	char url[1024];
	snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/contents/%s",
	         owner, repo, path ? path : "");
	hkHttpReq r;
	memset(&r, 0, sizeof(r));
	r.method = "GET";
	r.url = url;
	r.headers = "-H 'Accept: application/vnd.github+json' ";
	r.timeout_s = 25;
	r.follow = 1;
	return hk_http_fetch(&r);
}

static int hkGhFetchTo(const char *url, const char *dest) {
	hkHttpReq r;
	memset(&r, 0, sizeof(r));
	r.method = "GET";
	r.url = url;
	r.timeout_s = 25;
	r.follow = 1;
	char *body = hk_http_fetch(&r);
	if (!body) return -1;
	size_t n = strlen(body);
	if (!n || n > HK_TAP_MAX_BYTES) { free(body); return -1; }
	int rc = hk_fs_write(dest, body, n, 0);
	free(body);
	return rc;
}

/* Install one skill directory: SKILL.md plus the files it bundles, one level
   down. Returns the number of files written, or -1. */
static int hkInstallSkillDir(const char *owner, const char *repo, const char *path,
                             const char *name, const char *skills_root) {
	char listing_path[512];
	snprintf(listing_path, sizeof(listing_path), "%s", path);
	char *listing = hkGhContents(owner, repo, listing_path);
	if (!listing) return -1;
	if (strstr(listing, "\"message\":\"Not Found\"")) { free(listing); return -1; }

	/* Decide before writing anything. Creating the folder first left debris
	   behind for every directory that turned out not to be a skill. */
	if (!strstr(listing, "\"name\":\"SKILL.md\"")) { free(listing); return -1; }

	char dest_dir[PATH_MAX];
	snprintf(dest_dir, sizeof(dest_dir), "%s/%s", skills_root, name);
	hk_fs_mkdirp(dest_dir);

	int wrote = 0, seen_skill_md = 0;
	const char *p = listing;
	size_t olen;
	const char *obj;
	/* First pass: files at the top of the skill folder. */
	while (wrote < HK_TAP_MAX_FILES && (obj = hkJsonArrayNext(&p, &olen)) != NULL) {
		char *slice = malloc(olen + 1);
		if (!slice) break;
		memcpy(slice, obj, olen); slice[olen] = '\0';
		char *type = hkExtractJsonString(slice, "type");
		char *fname = hkExtractJsonString(slice, "name");
		char *durl = hkExtractJsonString(slice, "download_url");
		if (type && fname && !strcmp(type, "file") && durl && strncmp(durl, "https://", 8) == 0
		    && !strchr(fname, '/') && !strstr(fname, "..")) {
			char dest[PATH_MAX];
			if (snprintf(dest, sizeof(dest), "%s/%s", dest_dir, fname) >= (int)sizeof(dest)) { free(fname); continue; }
			if (hkGhFetchTo(durl, dest) == 0) {
				wrote++;
				if (!strcmp(fname, "SKILL.md")) seen_skill_md = 1;
			}
		}
		free(type); free(fname); free(durl); free(slice);
	}
	free(listing);
	/* Listed but unfetchable: leave nothing half-installed. */
	if (!seen_skill_md) {
		char smd[PATH_MAX];
		if (snprintf(smd, sizeof(smd), "%s/SKILL.md", dest_dir) >= (int)sizeof(smd)) smd[0] = '\0';
		hk_fs_remove(smd);
		return -1;
	}
	return wrote;
}

/* ---------- commands: one table, three renderers (DESIGN 2026-10-29 §2) ----------
   `:` runs hako itself, `/` drives the model. The wrong prefix still works; the
   table only says where a command lives. The terminal's menu, the editor's
   Rei pane and the studio all draw from it (the "commands" field on the wire). */
static const hkCmd HK_CMDS[] = {
	{ "help",      ':', "app",      "[command]",       "what hako can do",                 HK_CF_TURN },
	{ "q",         ':', "app",      "",                "quit",                             HK_CF_TURN },
	{ "quit",      ':', "app",      "",                "quit",                             HK_CF_TURN | HK_CF_ALIAS },
	{ "exit",      ':', "app",      "",                "quit",                             HK_CF_TURN | HK_CF_ALIAS },
	{ "name",      ':', "session",  "[name]",          "name this chat",                   HK_CF_TURN },
	{ "color",     ':', "session",  "[color|off]",     "this chat's color",                HK_CF_TURN },
	{ "sessions",  ':', "session",  "[clear [all]]",   "past chats",                       HK_CF_TURN },
	{ "resume",    ':', "session",  "<id>",            "switch to a past chat",            0 },
	{ "session",   ':', "session",  "[new]",           "this chat, or start a new one",    0 },
	{ "history",   ':', "session",  "[local|global]",  "what you typed before",            HK_CF_TURN },
	{ "clear",     ':', "session",  "",                "clear the chat",                   0 },
	{ "undo",      ':', "session",  "",                "drop the last turn",               0 },
	{ "edit",      ':', "session",  "",                "edit your last message",           0 },
	{ "todo",      ':', "session",  "[clear]",         "the task list",                    HK_CF_TURN },
	{ "theme",     ':', "settings", "[name]",          "pick a theme",                     HK_CF_TURN },
	{ "mode",      ':', "settings", "[ask|auto|plan]", "when tools ask first",             HK_CF_TURN },
	{ "auto",      ':', "settings", "[on|off]",        "run tools without asking",         HK_CF_TURN },
	{ "tools",     ':', "settings", "[on|off]",        "let the model use tools",          HK_CF_TURN },
	{ "toolgate",  ':', "settings", "[on|off]",        "offer tools only when asked for",  HK_CF_TURN },
	{ "toolmode",  ':', "settings", "[native|prose]",  "how the model calls tools",        HK_CF_TURN },
	{ "trust",     ':', "settings", "[revoke]",        "let tools work in this folder",    HK_CF_TURN },
	{ "mouse",     ':', "settings", "[on|off]",        "tap a result to open it",          HK_CF_TURN },
	{ "editor",    ':', "settings", "[command]",       "the editor ctrl+g opens",          HK_CF_TURN },
	{ "voice",     ':', "settings", "[on|off]",        "read replies aloud",               HK_CF_TURN },
	{ "model",     ':', "model",    "[id]",            "pick a model",                     0 },
	{ "models",    ':', "model",    "[refresh]",       "the provider's models",            HK_CF_TURN },
	{ "provider",  ':', "model",    "[name]",          "pick a provider",                  0 },
	{ "providers", ':', "model",    "",                "every provider",                   HK_CF_ALIAS },
	{ "pull",      ':', "model",    "<model>",         "fetch local weights",              0 },
	{ "compact",   '/', "model",    "",                "shrink the context",               0 },
	{ "retry",     '/', "model",    "",                "run the last turn again",          0 },
	{ "login",     ':', "account",  "[provider|api]",  "sign in",                          0 },
	{ "logout",    ':', "account",  "[provider]",      "forget a sign-in",                 0 },
	{ "accounts",  ':', "account",  "",                "saved sign-ins",                   HK_CF_TURN },
	{ "usage",     ':', "account",  "",                "tokens and cost",                  HK_CF_TURN },
	{ "skills",    ':', "skill",    "[reload]",        "installed skills",                 HK_CF_TURN },
	{ "skill",     ':', "skill",    "install|uninstall", "add or remove skills",           0 },
	{ "use",       ':', "skill",    "<skill|none>",    "a skill as a mode (= /<skill>)",   0 },
	{ "memory",    ':', "app",      "[forget <name>]", "what it remembers",                HK_CF_TURN },
	{ "remote",    ':', "app",      "[on|off|pair]",   "use this session from a phone",    HK_CF_TURN },
	{ "mcp",       ':', "app",      "[reload]",        "MCP servers",                      HK_CF_TURN },
	{ "e",         ':', "app",      "[editor]",        "write the next message in $EDITOR", HK_CF_TURN | HK_CF_TTY },
	{ "o",         ':', "app",      "",                "the last tool output in full",     HK_CF_TURN },
	{ "expand",    ':', "app",      "",                "the last tool output in full",     HK_CF_TURN | HK_CF_ALIAS },
	{ "update",    ':', "app",      "",                "is there a newer hako",            HK_CF_TURN },
};
#define HK_NCMDS ((int)(sizeof HK_CMDS / sizeof HK_CMDS[0]))

/* Installed skills, each a /<name> command: scanned once, again after a change. */
#define HK_SKILLS_MAX 64
static char hk_skill_names[HK_SKILLS_MAX][64];
static char hk_skill_descs[HK_SKILLS_MAX][96];
static int  hk_skill_n = 0, hk_skill_scanned = 0;

static void hkSkillScan(void) {
	if (hk_skill_scanned) return;
	hk_skill_scanned = 1;
	hk_skill_n = 0;
	char roots[HK_SKILL_ROOTS][640];
	int nr = hkSkillRoots(roots);
	for (int ri = 0; ri < nr; ri++) {
		DIR *d = opendir(roots[ri]);
		struct dirent *e;
		while (d && (e = readdir(d)) && hk_skill_n < HK_SKILLS_MAX) {
			if (e->d_name[0] == '.') continue;
			char nm[64], path[1024], md[1100];
			hkSkillName(e->d_name, nm, sizeof nm);
			int dup = 0;
			for (int k = 0; k < hk_skill_n; k++) if (!strcasecmp(hk_skill_names[k], nm)) dup = 1;
			for (int k = 0; k < HK_NCMDS && !dup; k++) if (!strcasecmp(HK_CMDS[k].n, nm)) dup = 1;   /* a command wins */
			for (const char *q = nm; *q && !dup; q++) if ((unsigned char)*q < 0x20 || *q == 0x7f) dup = 1;   /* no escapes into the menu */
			if (dup || strchr(nm, ' ')) continue;
			if (!hkFmtFits(path, sizeof path, "%s/%s", roots[ri], e->d_name)) continue;
			int is_dir = 0;
			if (!hk_fs_exists(path, NULL, NULL, &is_dir)) continue;
			if (is_dir) {
				if (!hkFmtFits(md, sizeof md, "%s/SKILL.md", path) || !hk_fs_exists(md, NULL, NULL, NULL)) {
					if (!hkFmtFits(md, sizeof md, "%s/%s.md", path, e->d_name) || !hk_fs_exists(md, NULL, NULL, NULL)) continue;
				}
			} else {
				size_t l = strlen(e->d_name);
				if (l < 4 || strcmp(e->d_name + l - 3, ".md")) continue;
				snprintf(md, sizeof md, "%s", path);
			}
			char *body = hk_fs_read(md, 8192, NULL), desc[768] = "";
			if (body) { hkSkillDesc(body, desc, sizeof desc); free(body); }
			char *dot = strchr(desc, '.');                   /* the first sentence is enough for a menu */
			if (dot) dot[1] = '\0';
			for (char *q = desc; *q; q++) *q = ((unsigned char)*q < 0x20 || *q == 0x7f) ? ' ' : (char)tolower((unsigned char)*q);
			size_t dl = strlen(desc);
			if (dl && desc[dl - 1] == '.') desc[dl - 1] = '\0';
			snprintf(hk_skill_names[hk_skill_n], sizeof hk_skill_names[0], "%s", nm);
			snprintf(hk_skill_descs[hk_skill_n], sizeof hk_skill_descs[0], "%s", desc[0] ? desc : "a skill");
			clClipToCells(hk_skill_descs[hk_skill_n], 60);
			hk_skill_n++;
		}
		if (d) closedir(d);
	}
}

/* Row i of the combined list: the table, then the skills. */
static int hkCmdCount(void) { hkSkillScan(); return HK_NCMDS + hk_skill_n; }
static hkCmd hkCmdAt(int i) {
	if (i < HK_NCMDS) return HK_CMDS[i];
	hkCmd c = { hk_skill_names[i - HK_NCMDS], '/', "skill", "[text]", hk_skill_descs[i - HK_NCMDS], 0 };
	return c;
}
static int hkCmdFind(const char *name, size_t n) {
	int total = hkCmdCount();
	for (int i = 0; i < total; i++) {
		hkCmd c = hkCmdAt(i);
		if (strlen(c.n) == n && !strncmp(c.n, name, n)) return i;
	}
	return -1;
}

/* Matches for a typed command word: prefix before substring, the exact name
   first, the typed prefix's own commands before the other's. */
static int hkCmdMatch(char typed_p, const char *w, size_t wl, int *out, int max) {
	int total = hkCmdCount(), n = 0;
	char wz[64];
	snprintf(wz, sizeof wz, "%.*s", (int)(wl < 63 ? wl : 63), w);
	for (int pass = 0; pass < 6 && n < max; pass++) {
		int kind = pass / 2, own = !(pass % 2);         /* 0 exact, 1 prefix, 2 substring */
		for (int i = 0; i < total && n < max; i++) {
			hkCmd c = hkCmdAt(i);
			if ((c.p == typed_p) != own) continue;
			int exact = strlen(c.n) == wl && !strncmp(c.n, w, wl);
			int is_pre = !strncmp(c.n, w, wl);
			if (kind == 0 && !exact) continue;
			if (kind == 1 && (exact || !is_pre)) continue;
			if (kind == 2 && (!wl || is_pre || !strstr(c.n, wz))) continue;
			if ((c.f & HK_CF_ALIAS) && !exact && wl < 2) continue;
			out[n++] = i;
		}
	}
	return n;
}

/* The table on the wire: "commands":[{"n","p","c","a","d"},...]. */
static char *hkCmdsJson(void) {
	int total = hkCmdCount();
	size_t cap = (size_t)total * 512 + 64, o = 0;
	char *out = malloc(cap);
	if (!out) return NULL;
	o = hkCatf(out, o, cap, "[");
	for (int i = 0; i < total; i++) {
		hkCmd c = hkCmdAt(i);
		char d[200], nm[160];
		hkJsonEscapeInto(c.d, d, (int)sizeof d);
		hkJsonEscapeInto(c.n, nm, (int)sizeof nm);
		o = hkCatf(out, o, cap, "%s{\"n\":\"%s\",\"p\":\"%c\",\"c\":\"%s\",\"a\":\"%s\",\"d\":\"%s\"}",
		           i ? "," : "", nm, c.p, c.c, c.a, d);
	}
	hkCatf(out, o, cap, "]");
	return out;
}

/* The bare commands that open a menu (tty popup, or choice_request elsewhere). */
static int hkIsMenuCommand(const char *line) {
	static const char *menus[] = { "model", "models", "provider", "providers", "theme", "login", "login api", NULL };
	if (!hkCanPick() || (line[0] != ':' && line[0] != '/')) return 0;
	const char *c = line + 1;
	size_t n = strlen(c);
	while (n && c[n - 1] == ' ') n--;
	for (int i = 0; menus[i]; i++)
		if (strlen(menus[i]) == n && !strncmp(c, menus[i], n)) return 1;
	return 0;
}

/* :login is the front door: subscription sign-ins first, API keys one level
   down, each named for who bills you (OpenRouter's GPT is OpenRouter, not ChatGPT). */
static int hkSignedIn(const char *id) {
	clCred *c = clCredsFind(id);
	if (!c) return 0;
	if (!strcmp(id, "anthropic")) return c->oauth_refresh != NULL;
	return c->oauth_refresh || c->api_key;
}

typedef struct { const char *id, *name, *what; } hkLoginRow;
static const hkLoginRow HK_SIGNIN[] = {
	{ "anthropic",     "Claude",         "Pro or Max plan" },
	{ "chatgpt",       "ChatGPT",        "free or paid account" },
	{ "copilot",       "GitHub Copilot", "Copilot plan" },
	{ "github-models", "GitHub Models",  "free with a GitHub account" },
	{ "nous",          "Nous Portal",    "Nous account or subscription" },
	{ "api",           "API key",        "pay per token, any provider" },
}, HK_KEYS[] = {
	{ "anthropic-api", "Anthropic",      "Claude API, billed by Anthropic" },
	{ "openai",        "OpenAI",         "OpenAI API, separate from ChatGPT" },
	{ "openrouter",    "OpenRouter",     "many vendors' models, billed by OpenRouter" },
	{ "nous-api",      "Nous Portal",    "many vendors' models, billed by Nous" },
	{ "gemini",        "Google",         "Gemini API, free tier" },
	{ "groq",          "Groq",           "open models, free tier" },
	{ "cerebras",      "Cerebras",       "open models, free tier" },
	{ "deepseek",      "DeepSeek",       "DeepSeek API" },
	{ "mistral",       "Mistral",        "Mistral API" },
	{ "xai",           "xAI",            "Grok API" },
	{ "together",      "Together",       "open models" },
	{ "fireworks",     "Fireworks",      "open models" },
	{ "custom",        "custom",         "any OpenAI-compatible endpoint" },
};
#define HK_NSIGNIN ((int)(sizeof HK_SIGNIN / sizeof HK_SIGNIN[0]))
#define HK_NKEYS   ((int)(sizeof HK_KEYS / sizeof HK_KEYS[0]))

/* A saved API key for this menu id ("anthropic-api" files under "anthropic"),
   as opposed to a sign-in's token. */
static int hkKeySaved(const char *id) {
	char lab[32];
	snprintf(lab, sizeof lab, "%s", id);
	char *dash = strstr(lab, "-api");
	if (dash) *dash = '\0';
	clCred *c = clCredsFind(lab);
	return c && c->api_key && !c->oauth_refresh;
}

/* The menu id a mistyped :login most likely meant: edit distance 2 or less. */
static const char *hkNearestLogin(const char *typed) {
	const char *best = NULL;
	int best_d = 3;
	size_t tl = strlen(typed);
	if (tl > 30) return NULL;
	for (int i = 0; i < HK_NSIGNIN + HK_NKEYS; i++) {
		const char *id = i < HK_NSIGNIN ? HK_SIGNIN[i].id : HK_KEYS[i - HK_NSIGNIN].id;
		size_t il = strlen(id);
		int row[32];
		for (size_t j = 0; j <= il; j++) row[j] = (int)j;
		for (size_t a = 1; a <= tl; a++) {
			int diag = row[0];
			row[0] = (int)a;
			for (size_t j = 1; j <= il; j++) {
				int up = row[j];
				int sub = diag + (tolower((unsigned char)typed[a - 1]) != id[j - 1]);
				int d = up + 1 < row[j - 1] + 1 ? up + 1 : row[j - 1] + 1;
				row[j] = sub < d ? sub : d;
				diag = up;
			}
		}
		if (row[il] < best_d) { best_d = row[il]; best = id; }
	}
	return best;
}

static const char *hkPickLogin(int api) {
	const hkLoginRow *T = api ? HK_KEYS : HK_SIGNIN;
	static char rows[16][112];
	const char *items[16];
	int n = api ? HK_NKEYS : HK_NSIGNIN;
	for (int i = 0; i < n; i++) {
		const char *mark = "";
		if (!api && hkSignedIn(T[i].id)) mark = "  · signed in";
		if (api && hkKeySaved(T[i].id))   mark = "  · key saved";
		snprintf(rows[i], sizeof rows[i], "%-15s %s%s", T[i].name, T[i].what, mark);
		items[i] = rows[i];
	}
	int pick = clPopupSelect(api ? "API key" : "sign in", items, n, 0, NULL);
	return pick < 0 ? NULL : T[pick].id;
}

/* :help remote: everything :remote used to print at once. */
static void hkRemoteHelp(aiData *data) {
	aiAddHistory(data, ":remote on shares this session through your account, from any signed-in browser (invite beta)");
	aiAddHistory(data, ":remote pair adds a browser that signed in without a passkey (an emailed code): scan the code it shows");
	aiAddHistory(data, ":remote off ends it; the bar says remote while it is on");
	char tok[25];
	if (hkRandomToken(tok, 24, "abcdefghijklmnopqrstuvwxyz0123456789"))
		aiSayfSecret(data, "headless on the same wifi: hako --serve --lan --token %s, and open the printed address with ?t=%s", tok, tok);
	aiAddHistory(data, "the phone can prompt and approve, every tool asks, \"always\" counts once, and it cannot turn on auto or leave this folder");
#ifdef __APPLE__
	aiAddHistory(data, "macOS drops wifi connections to unsigned binaries: run `make sign` if the phone cannot connect");
#endif
}

static int hkHandleSlashCmd(aiData *data, const char *prompt);
static int hk_slash_depth = 0;
static char hk_slash_prompt[4096];       /* /<skill> <text>: the text, for the caller to send (return 3) */
static int hkHandleSlash(aiData *data, const char *prompt) {
	if (!hk_slash_depth) hk_menu_pick = 0;
	hk_slash_depth++;
	int r = hkHandleSlashCmd(data, prompt);
	hk_slash_depth--;
	return r;
}

static int hkHandleSlashCmd(aiData *data, const char *prompt) {
	if (prompt[0] != '/' && prompt[0] != ':') return 0;
	const char *cmd = prompt + 1;
	const char *arg = strchr(cmd, ' ');
	int cmdlen = arg ? (arg - cmd) : (int)strlen(cmd);
	if (arg) { while (*arg == ' ') arg++; }

	static char plural_pick[96];
	if ((!arg || !*arg) && hkCanPick()) {
		if (cmdlen == 6 && !strncmp(cmd, "models", 6)) {
			if (!hkPickModel(data, plural_pick, sizeof plural_pick)) return 1;
			cmd = "model"; cmdlen = 5; arg = plural_pick; hk_menu_pick = 1;
		} else if (cmdlen == 9 && !strncmp(cmd, "providers", 9)) {
			if (!hkPickProvider(plural_pick, sizeof plural_pick)) return 1;
			cmd = "provider"; cmdlen = 8; arg = plural_pick; hk_menu_pick = 1;
		}
	}

	if (strncmp(cmd, "help", cmdlen) == 0 && cmdlen == 4) {
		if (arg && *arg) {                         /* :help <command>: that one, in full */
			const char *w = arg;
			while (*w == ':' || *w == '/') w++;
			if (!strcmp(w, "remote")) { hkRemoteHelp(data); return 1; }
			int i = hkCmdFind(w, strlen(w));
			if (i < 0) { aiSayf(data, "no command called '%s' (:help lists them)", w); return 1; }
			hkCmd c = hkCmdAt(i);
			aiSayf(data, "%c%s %s  %s", c.p, c.n, c.a, c.d);
			if (c.p == '/') aiAddHistory(data, "  / commands go to the model; : commands run hako itself (either prefix works)");
			return 1;
		}
		/* One table, so the help can never miss a command again. */
		static const char *CATS[] = { "session", "settings", "model", "account", "skill", "app", NULL };
		static char rows[HK_NCMDS + HK_SKILLS_MAX + 16][112];
		const char *items[HK_NCMDS + HK_SKILLS_MAX + 16];
		int hn = 0, pane = E.pipe_mode || E.serve_mode;
		int total = hkCmdCount();
		snprintf(rows[hn], sizeof rows[0], ": runs hako  \xC2\xB7  / drives the model  \xC2\xB7  tab completes");
		items[hn] = rows[hn]; hn++;
		for (int k = 0; CATS[k]; k++) {
			for (int i = 0; i < total && hn < (int)(sizeof rows / sizeof rows[0]); i++) {
				hkCmd c = hkCmdAt(i);
				if (strcmp(c.c, CATS[k]) || (c.f & HK_CF_ALIAS) || (pane && (c.f & HK_CF_TTY))) continue;
				char lhs[64];
				snprintf(lhs, sizeof lhs, "%c%s %s", c.p, c.n, c.a);
				snprintf(rows[hn], sizeof rows[0], "%-24s %s", lhs, c.d);
				items[hn] = rows[hn]; hn++;
			}
		}
		if (!pane) {
			snprintf(rows[hn], sizeof rows[0], "keys  ctrl-g editor \xC2\xB7 ctrl-o transcript \xC2\xB7 ctrl-v image \xC2\xB7 shift-tab mode");
			items[hn] = rows[hn]; hn++;
		}
		if (isatty(STDIN_FILENO) && !pane) { clPopupSelect("help", items, hn, 0, NULL); return 1; }
		/* one message, not forty: a pane gets it in one event */
		size_t cap = (size_t)hn * 114 + 1, o = 0;
		char *all = malloc(cap);
		if (!all) return 1;
		all[0] = '\0';
		for (int i = 0; i < hn; i++) o = hkCatf(all, o, cap, "%s%s", i ? "\n" : "", items[i]);
		aiAddHistory(data, all);
		free(all);
		return 1;
	}
	if (strncmp(cmd, "retry", cmdlen) == 0 && cmdlen == 5) {
		int u = aiFindLastMessageRole(data, "user");
		if (u < 0) { aiAddHistory(data, "(no user message to retry)"); return 1; }
		aiDropMessagesFrom(data, u + 1);
		hkDropTrailingHistory(data, HK_ROLE_AI);
		aiWorkerSend(data);
		return 1;
	}
	if (strncmp(cmd, "edit", cmdlen) == 0 && cmdlen == 4) {
		int u = aiFindLastMessageRole(data, "user");
		if (u < 0) { aiAddHistory(data, "(no user message to edit)"); return 1; }
		free(cl_preset_input);
		cl_preset_input = strdup(data->messages[u].content ? data->messages[u].content : "");
		aiDropMessagesFrom(data, u);
		hkDropTrailingHistory(data, HK_ROLE_AI);
		hkDropTrailingHistory(data, HK_ROLE_USER);
		return 1;
	}
	if (strncmp(cmd, "undo", cmdlen) == 0 && cmdlen == 4) {
		int u = aiFindLastMessageRole(data, "user");
		if (u < 0) { aiAddHistory(data, "(nothing to undo)"); return 1; }
		aiDropMessagesFrom(data, u + 1);
		hkDropTrailingHistory(data, HK_ROLE_AI);
		aiAddHistory(data, "(undone — last AI turn dropped)");
		return 1;
	}
	if ((strncmp(cmd, "editor", cmdlen) == 0 && cmdlen == 6) ||
	    (strncmp(cmd, "e", cmdlen) == 0 && cmdlen == 1)) {
		if (arg && *arg) {
			free(E.editor);
			E.editor = strdup(arg);
			aiSayf(data, "editor: %s (add editor=%s to ~/.hakorc to keep it)", arg, arg);
			return 1;
		}
		char *body = clComposeInEditor(NULL);
		if (!body) { aiAddHistory(data, "nothing composed"); return 1; }
		free(cl_preset_input);
		cl_preset_input = body;      /* lands in the field, ready to send */
		aiAddHistory(data, "composed — press enter to send, or keep editing");
		return 1;
	}
	if (strncmp(cmd, "mode", cmdlen) == 0 && cmdlen == 4) {
		if (arg && *arg) {
			if      (!strcmp(arg, "plan")) { E.ai_plan_mode = 1; E.ai_auto_approve = 0; }
			else if (!strcmp(arg, "auto")) { E.ai_plan_mode = 0; E.ai_auto_approve = 1; }
			else if (!strcmp(arg, "ask"))  { E.ai_plan_mode = 0; E.ai_auto_approve = 0; }
			else { aiAddHistory(data, "modes: ask · auto · plan"); return 1; }
		} else {
			/* cycle ask -> auto -> plan -> ask */
			if      (E.ai_plan_mode)      { E.ai_plan_mode = 0; E.ai_auto_approve = 0; }
			else if (E.ai_auto_approve)   { E.ai_plan_mode = 1; E.ai_auto_approve = 0; }
			else                          { E.ai_auto_approve = 1; }
		}
		hkSaveSession();
		hkPromptInvalidate();
		aiSayf(data, "mode: %s", E.ai_plan_mode ? "plan (no changes)" :
		                          E.ai_auto_approve ? "auto (runs tools)" : "ask (prompts first)");
		return 1;
	}
	if (strncmp(cmd, "auto", cmdlen) == 0 && cmdlen == 4)
		return hkToggle(data, arg, &E.ai_auto_approve, "auto-approve",
			" — on runs tools without prompting; off prompts per call.");
	if (strncmp(cmd, "mcp", cmdlen) == 0 && cmdlen == 3) {
		if (arg && strcmp(arg, "reload") == 0) {
			hkMcpShutdown(); hkMcpInit();
			aiAddHistory(data, "MCP reloaded from ~/.hako/mcp.json.");
		}
		hkMcpList(data);
		return 1;
	}
	if (strncmp(cmd, "login", cmdlen) == 0 && cmdlen == 5) {
		const char *prov = (arg && *arg) ? arg : NULL;
		if (!prov || !strcmp(prov, "api")) {
			if (!hkCanPick()) {
				aiAddHistory(data, ":login          sign in: anthropic chatgpt copilot github-models");
				aiAddHistory(data, ":login api      API key: anthropic-api openai openrouter gemini groq");
				aiAddHistory(data, "                nous cerebras deepseek mistral xai together fireworks custom");
				return 1;
			}
			const char *id = hkPickLogin(prov != NULL);
			if (id && !strcmp(id, "api")) id = hkPickLogin(1);
			if (!id) return 1;                       /* closed: nothing changed, nothing said */
			hk_menu_pick = 1;
			if ((!strcmp(id, "anthropic") || !strcmp(id, "chatgpt")) && hkSignedIn(id)) {
				char sw[48];
				snprintf(sw, sizeof sw, ":provider %s", id);   /* already signed in: just switch */
				return hkHandleSlash(data, sw);
			}
			prov = id;
		}
		hkEchoFlush(data);                           /* a sign-in prints; show what started it */
		if (strcmp(prov, "ollama") == 0 || strcmp(prov, "local") == 0 || strcmp(prov, "koi") == 0) {
			hkApplyProviderAlias(prov);
			hkSaveSession();
			aiAddHistory(data, "ollama is local — no API key needed.");
			aiAddHistory(data, "ensure `ollama serve` is running, then :models to list, :model <id> to pick.");
			return 1;
		}
		if (strncmp(prov, "anthropic ", 10) == 0 || strncmp(prov, "claude ", 7) == 0) {
			const char *code = strchr(prov, ' ') + 1;
			while (*code == ' ') code++;
			hkApplyProviderAlias("anthropic");
			clOAuthAnthropicFinish(data, code);
			return 1;
		}
		if (strcmp(prov, "anthropic") == 0 || strcmp(prov, "claude") == 0) {
			hkApplyProviderAlias("anthropic");
			clOAuthAnthropic(data);
			return 1;
		}
		if (strcmp(prov, "anthropic-api") == 0 || strcmp(prov, "claude-api") == 0) {
			prov = "anthropic";
			hkApplyProviderAlias("anthropic");
		}
		if (strcmp(prov, "github-copilot") == 0 || strcmp(prov, "copilot") == 0) {
			clOAuthGithubCopilot(data);
			return 1;
		}
		if (strcmp(prov, "chatgpt") == 0 || strcmp(prov, "codex") == 0) {
			clOAuthChatGPT(data);
			return 1;
		}
		if (strcmp(prov, "github-models") == 0 || strcmp(prov, "ghmodels") == 0) {
			clOAuthGithubModels(data);
			return 1;
		}
		if (strcmp(prov, "openrouter") == 0) {
			if (clOAuthOpenRouter(data) == 0) return 1;
			aiAddHistory(data, "(browser sign-in did not finish; paste a key instead)");
			prov = "openrouter-api";
		}
		if (strcmp(prov, "nous") == 0) {
			if (clOAuthNous(data) == 0) return 1;
			aiAddHistory(data, "(browser sign-in did not finish; paste a key instead)");
			prov = "nous-api";
		}
		if (strcmp(prov, "nous-api") == 0) prov = "nous";
		if (strcmp(prov, "openrouter-api") == 0) {
			prov = "openrouter";
			hkApplyProviderAlias("openrouter");
		}
		const char *url = clProviderConsoleUrl(prov);
		if (!url && strcmp(prov, "custom") != 0) {
			const char *guess = hkNearestLogin(prov);
			if (guess) aiSayf(data, "no sign-in called '%s'. did you mean :login %s?", prov, guess);
			else aiSayf(data, "no sign-in called '%s'. :login lists them", prov);
			return 1;
		}
		if (url) {
			aiSayf(data, "opening %s", url);
			clOpenUrl(url);
		} else {
			aiAddHistory(data, "custom provider — set ai_endpoint via /provider or .hakorc");
		}
		printf("  paste API key (input hidden%s): ", hkClipAvailable() ? ", enter alone uses the clipboard" : "");
		fflush(stdout);
		char key[1024];
		clReadHidden(key, sizeof(key));
		if (!key[0]) { aiAddHistory(data, "(empty key, not saved)"); return 1; }
		hkUsePastedKey(prov, key);
		clCredsCaptureCurrent();
		clCredsSave();
		hkSaveSession();
		aiSayf(data, "key saved for %s (~/.hako/credentials, mode 0600, obfuscated)", hkProviderLabel());
		hkHealModelForProvider(data);
		return 1;
	}
	if (strncmp(cmd, "logout", cmdlen) == 0 && cmdlen == 6) {
		const char *prov = (arg && *arg) ? arg : hkProviderLabel();
		if (!prov || !strcmp(prov, "none")) { aiAddHistory(data, "no active provider"); return 1; }
		clCred *c = clCredsFind(prov);
		if (c) {
			free(c->api_key); c->api_key = NULL;
			free(c->oauth_refresh); c->oauth_refresh = NULL;
			c->oauth_expires_at = 0;
		}
		if (!strcmp(prov, hkProviderLabel())) {
			free(E.ai_api_key); E.ai_api_key = NULL;
			free(E.ai_oauth_refresh); E.ai_oauth_refresh = NULL;
			free(E.ai_oauth_provider); E.ai_oauth_provider = NULL;
			E.ai_oauth_expires_at = 0;
		}
		clCredsSave();
		aiSayf(data, "logged out: %s", prov);
		return 1;
	}
	if (strncmp(cmd, "accounts", cmdlen) == 0 && cmdlen == 8) {
		if (cl_creds_n == 0) { aiAddHistory(data, "(no saved logins — use :login <provider>)"); return 1; }
		aiAddHistory(data, "saved logins:");
		for (int i = 0; i < cl_creds_n; i++) {
			clCred *c = &cl_creds[i];
			if (!c->api_key && !c->oauth_refresh) continue;
			const char *kind = c->oauth_refresh ? "oauth" : "key";
			const char *active = !strcmp(c->provider, hkProviderLabel()) ? " *" : "";
			aiSayf(data, "  %s (%s)%s", c->provider, kind, active);
		}
		return 1;
	}
	if (strncmp(cmd, "clear", cmdlen) == 0 && cmdlen == 5) {
		aiClearHistory(data);
		printf("\x1b[2J\x1b[H");
		fflush(stdout);
		aiAddHistory(data, "(cleared)");
		return 1;
	}
	if (strncmp(cmd, "model", cmdlen) == 0 && cmdlen == 5) {
		char modelpick[96];
		if ((!arg || !*arg) && hkCanPick()) {
			if (!hkPickModel(data, modelpick, sizeof modelpick)) return 1;
			arg = modelpick; hk_menu_pick = 1;
		}
		if (arg && *arg) {
			if (!strncmp(arg, "hako-koi-v", 10)) {
				aiAddHistory(data, "hako-koi-v* is queued — real 14B/32B fine-tune lands after a rented-GPU run.");
				aiAddHistory(data, "available today: hako-sho (3B), hako-koi (7B).");
				return 1;
			}
			if (!strncmp(arg, "hako-samurai", 12)) {
				aiAddHistory(data, "hako-samurai is reserved — 50B+ max tier, waits on hardware.");
				aiAddHistory(data, "available today: hako-sho (3B), hako-koi (7B).");
				return 1;
			}
			free(E.ai_model);
			E.ai_model = strdup(arg);
			hkSaveSession();
			if (!hk_menu_pick) aiSayf(data, "model: %s (saved)", E.ai_model);

			if (E.ai_provider_type == AI_PROVIDER_MITHRAEUM && isatty(STDIN_FILENO)) {
				char *p = hkMithraeumModelPath(E.ai_model);
				struct stat ws;
				if (p && stat(p, &ws) != 0) hkMithraeumRelocate(E.ai_model);
				if (p && stat(p, &ws) != 0) {
					char q[128];
					snprintf(q, sizeof(q), "'%s' is not installed. Download from HuggingFace?", E.ai_model);
					if (clPromptYN(q, 1)) hkPullModel(E.ai_model);
					else aiAddHistory(data, "skipped — run :pull to download later.");
				}
				free(p);
			}
		} else {
			aiSayf(data, "model: %s", E.ai_model ? E.ai_model : "(unset)");
		}
		return 1;
	}
	if (strncmp(cmd, "pull", cmdlen) == 0 && cmdlen == 4) {
		const char *m = (arg && *arg) ? arg : E.ai_model;
		if (!m || !*m) { aiAddHistory(data, "usage: :pull <model>  (or set one with :model first)"); return 1; }
		if (hkMithraeumRelocate(m)) { aiAddHistory(data, "already installed (linked into ~/.hako/models)."); return 1; }
		hkPullModel(m);
		return 1;
	}
	if (strncmp(cmd, "models", cmdlen) == 0 && cmdlen == 6) {
		const char *endpoint = E.ai_endpoint;
		if (E.ai_provider_type == AI_PROVIDER_MITHRAEUM) {
			const char *home = getenv("HOME"); if (!home) home = ".";
			char mdir[1024];
			snprintf(mdir, sizeof(mdir), "%s/.hako/models", home);
			aiAddHistory(data, "hako family (mithraeum runtime — hakm subprocess, no ollama):");
			aiAddHistory(data, "  installed (~/.hako/models):");
			int count = 0;
			DIR *d = opendir(mdir);
			if (d) {
				struct dirent *de;
				while ((de = readdir(d)) != NULL) {
					if (de->d_name[0] == '.') continue;
					char w[1300]; struct stat ws;
					if (snprintf(w, sizeof(w), "%s/%s/%s.mlf2", mdir, de->d_name, de->d_name) >= (int)sizeof(w))
						continue;
					if (stat(w, &ws) != 0) continue;
					int active = E.ai_model && !strcmp(E.ai_model, de->d_name);
					aiSayf(data, "    %s %s", active ? "◎" : " ", de->d_name);
					count++;
				}
				closedir(d);
			}
			if (count == 0)
				aiAddHistory(data, "    (none — :pull hako-sho, or convert a GGUF with hako/tools/gguf2mlf.py)");
			else {
				aiSayf(data, "  %d installed. :model <name> to select.", count);
			}
			return 1;
		}
		const char *match_against = hkProviderLabel();
		if (E.ai_provider_type == AI_PROVIDER_OLLAMA && (!endpoint || !*endpoint))
			endpoint = "http://localhost:11434";

		int force = arg && *arg && (!strcmp(arg, "refresh") || !strcmp(arg, "-r"));
		clOAuthEnsureFresh(data);
		static char live[HK_MODELS_MAX][96];
		long age = -1;
		int ln = hkModelsLive(live, HK_MODELS_MAX, force, &age);
		if (ln > 0) {
			char hdr[192];
			if (age <= 0)
				snprintf(hdr, sizeof(hdr), "%d model(s) from %s (live):", ln, match_against);
			else
				snprintf(hdr, sizeof(hdr), "%d model(s) from %s (cached %ldh — :models refresh):",
					ln, match_against, age / 3600);
			aiAddHistory(data, hdr);
			int shown = ln > 40 ? 40 : ln;
			for (int i = 0; i < shown; i++) {
				int active = E.ai_model && !strcmp(E.ai_model, live[i]);
				aiSayf(data, "  %s %s", active ? "◎" : " ", live[i]);
			}
			if (ln > shown) {
				aiSayf(data, "  … %d more — :model to browse/filter them all", ln - shown);
			}
			aiAddHistory(data, ":model to pick from a list.  :providers to see all providers.");
			return 1;
		}
		if (E.ai_provider_type == AI_PROVIDER_OLLAMA) {
			aiSayf(data, ":models: no response from %s (is `ollama serve` running?)", endpoint);
			return 1;
		}
		const char *models = hkCuratedModels();
		char hdr[128];
		snprintf(hdr, sizeof(hdr), "suggested models for %s%s:",
			match_against,
			(E.ai_oauth_provider && (!strcmp(match_against, "copilot") || !strcmp(match_against, "github-models"))) ? " (OAuth)" : "");
		aiAddHistory(data, hdr);
		if (!models) {
			aiAddHistory(data, "  (no curated list — check provider docs)");
		} else {
			const char *p = models;
			while (*p) {
				const char *e = strchr(p, ',');
				if (!e) e = p + strlen(p);
				while (*p == ' ') p++;
				int active = E.ai_model && (e - p == (long)strlen(E.ai_model)) && !strncmp(E.ai_model, p, e - p);
				aiSayf(data, "  %s %.*s", active ? "◎" : " ", (int)(e - p), p);
				if (!*e) break;
				p = e + 1;
			}
		}
		aiAddHistory(data, ":model <id> to select.  :providers to see all providers.");
		return 1;
	}
	if (strncmp(cmd, "providers", cmdlen) == 0 && cmdlen == 9) {
		struct row { const char *name; const char *desc; };
		struct group { const char *header; struct row rows[12]; };
		struct group groups[] = {
#ifndef HAKO_WASM
			/* A browser can spawn nothing and its OAuth token endpoints refuse
			   cross-origin requests, so neither group is reachable there. */
			{ "Mithraeum (local-first, no auth, no cloud):", {
				{ "mithraeum",       "hako family — hako-sho (3B) / hako-koi (7B) / samurai" },
				{ NULL, NULL }
			} },
			{ "OAuth (subscription / account-bound):", {
				{ "anthropic",       "Claude Pro/Max — sign in with claude.ai" },
				{ "chatgpt",         "ChatGPT account — sign in with chatgpt.com" },
				{ "copilot",         "GitHub Copilot Pro/Business — device flow" },
				{ "github-models",   "free GitHub Models tier — device flow" },
				{ NULL, NULL }
			} },
			{ "Local (no auth):", {
				{ "ollama",          "run `ollama serve` first" },
				{ "ollamacloud",     "hosted Ollama (ollama.com)" },
				{ NULL, NULL }
			} },
#endif
			{ "Pay-per-token (API key paste):", {
				{ "anthropic-api",   "Claude API key (separate from sub)" },
				{ "openai",          "OpenAI API key (separate from ChatGPT)" },
				{ "gemini",          "Google AI Studio — generous free tier" },
				{ "groq",            "fastest Llama hosting — free tier" },
				{ "cerebras",        "ultra-fast inference — free tier" },
				{ "deepseek",        "DeepSeek API" },
				{ "xai",             "Grok via xAI API (api.x.ai)" },
				{ "openrouter",      "many vendors' models, billed by OpenRouter" },
				{ "nous",            "Nous Portal, billed by Nous" },
				{ "mistral, together, fireworks, custom", "more OpenAI-compat" },
				{ NULL, NULL }
			} },
		};
		const char *active_prov = hkProviderLabel();
		const char *active_oauth = E.ai_oauth_provider;
		for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
			aiAddHistory(data, groups[g].header);
			for (size_t r = 0; r < 12 && groups[g].rows[r].name; r++) {
				const char *n = groups[g].rows[r].name;
				int logged_in = 0, is_active = 0;
				char namebuf[64]; namebuf[0] = '\0';
				size_t nlen = 0;
				while (n[nlen] && n[nlen] != ',' && n[nlen] != ' ' && nlen + 1 < sizeof(namebuf)) {
					namebuf[nlen] = n[nlen]; nlen++;
				}
				namebuf[nlen] = '\0';
				if (clCredsFind(namebuf)) logged_in = 1;
				if (!strcmp(namebuf, active_prov)) is_active = 1;
				if (active_oauth && !strcmp(namebuf, active_oauth)) is_active = 1;
				char line[256];
				snprintf(line, sizeof(line), "  %s%s  %-22s %s",
					is_active ? "◎" : " ",
					logged_in ? "*" : " ",
					n,
					groups[g].rows[r].desc);
				aiAddHistory(data, line);
			}
		}
		aiAddHistory(data, ":login to sign in, :login api for a key.  :models for model suggestions.");
		aiAddHistory(data, "(◎ = active, * = saved login)");
		return 1;
	}
	if (strncmp(cmd, "provider", cmdlen) == 0 && cmdlen == 8) {
		char provpick[32];
		if ((!arg || !*arg) && hkCanPick()) {
			if (!hkPickProvider(provpick, sizeof provpick)) return 1;
			arg = provpick; hk_menu_pick = 1;
		}
		if (arg && *arg) {
			enum aiProviderType t = hkParseProvider(arg);
			if (t == AI_PROVIDER_NONE) {
				aiAddHistory(data, "unknown. valid: ollama, anthropic, openai, gemini/google,");
				aiAddHistory(data, "  groq, cerebras, deepseek, mistral, together, fireworks,");
				aiAddHistory(data, "  openrouter, nous, xai/grok, custom");
			} else {
				enum aiProviderType prev = E.ai_provider_type;
				clCredsCaptureCurrent();
				hkApplyProviderAlias(arg);
				clCredsRestoreFor(hkProviderLabel());
				if (prev != E.ai_provider_type) {
#if !defined(_WIN32) && !defined(HAKO_WASM)
					if (prev == AI_PROVIDER_MITHRAEUM) { hkPrewarmJoin(); hkHakmKill(); }
#endif
					pthread_mutex_lock(&data->lock);
					int before = data->message_count;
					aiFlattenMessages(data);
					int after = data->message_count;
					pthread_mutex_unlock(&data->lock);
					if (before != after && !hk_menu_pick) {
						aiSayf(data, "(flattened %d tool turn(s) for swap)", before - after);
					}
				}
				hkHealModelForProvider(data);
				clCredsRestoreFor(hkProviderLabel());
				clCredsSave();
				hkSaveSession();
				if (!hk_menu_pick) {
					char msg[256];
					snprintf(msg, sizeof(msg), "provider: %s (saved)%s%s",
						hkProviderLabel(),
						hkProviderDefaultEndpoint(arg) ? " endpoint=" : "",
						hkProviderDefaultEndpoint(arg) ? hkProviderDefaultEndpoint(arg) : "");
					aiAddHistory(data, msg);
				}
			}
		} else {
			aiSayf(data, "provider: %s", hkProviderLabel());
		}
		return 1;
	}
	if (strncmp(cmd, "history", cmdlen) == 0 && cmdlen == 7) {
		char p[512];
		hkHistoryPath(p, sizeof(p));
		aiAddHistory(data, p);
		return 1;
	}
	if (strncmp(cmd, "skills", cmdlen) == 0 && cmdlen == 6) {
		if (arg && strncmp(arg, "reload", 6) == 0) {
			hkPromptInvalidate();
			int n = hkLoadSkills(data);
			aiSayf(data, "reloaded %d skill(s)", n);
			return 1;
		}
		hkSkillScan();
		for (int i = 0; i < hk_skill_n; i++)
			aiSayf(data, "/%-16s %s%s", hk_skill_names[i], hk_skill_descs[i],
			       hkSkillActive(hk_skill_names[i]) ? "  (on)" : "");
		if (!hk_skill_n) aiAddHistory(data, "no skills yet. :skill install <owner/repo>");
		else aiAddHistory(data, "/<skill> turns one on as a mode, /<skill> <text> also sends the text");
		return 1;
	}
	if (strncmp(cmd, "skill", cmdlen) == 0 && cmdlen == 5) {
		if (arg && strncmp(arg, "uninstall ", 10) == 0) {
			const char *name = arg + 10;
			while (*name == ' ') name++;
			if (!*name) { aiAddHistory(data, "usage: /skill uninstall <name>"); return 1; }
			char dir[512];
			hkClawDirPath(dir, sizeof(dir));
			char path[1024];
			int nlen = strlen(name);
			if (nlen >= 4 && strcmp(name + nlen - 3, ".md") == 0) {
				snprintf(path, sizeof(path), "%s/skills/%s", dir, name);
			} else {
				snprintf(path, sizeof(path), "%s/skills/%s.md", dir, name);
			}
			if (hk_fs_remove(path) == 0) {
				hkPromptInvalidate();
				int n = hkLoadSkills(data);
				aiSayf(data, "uninstalled: %s (%d remain)", name, n);
			} else {
				aiAddHistory(data, "skill not found");
			}
			return 1;
		}
		if (!arg || strncmp(arg, "install ", 8) != 0) {
			aiAddHistory(data, "usage: /skill install <owner/repo[/path]> | <url>   ·   /skill uninstall <name>");
			return 1;
		}
		const char *url = arg + 8;
		while (*url == ' ') url++;
		if (!*url) { aiAddHistory(data, "usage: /skill install <owner/repo[/path]> | <url>"); return 1; }
		char dir[512];
		hkClawDirPath(dir, sizeof(dir));
		char skills[512];
		if (snprintf(skills, sizeof(skills), "%s/skills", dir) >= (int)sizeof(skills)) return 0;
		hk_fs_mkdirp(dir);
		hk_fs_mkdirp(skills);

		char gh_owner[128], gh_repo[128], gh_path[512];
		if (hkParseRepoSpec(url, gh_owner, sizeof(gh_owner), gh_repo, sizeof(gh_repo),
		                    gh_path, sizeof(gh_path)) == 0) {
			const char *want = gh_path[0] ? gh_path : "skills";
			char *listing = hkGhContents(gh_owner, gh_repo, want);
			int missing = !listing || strstr(listing, "\"message\":\"Not Found\"") != NULL;
			if (missing && !gh_path[0]) {
				free(listing);
				want = "";
				listing = hkGhContents(gh_owner, gh_repo, want);
				missing = !listing || strstr(listing, "\"message\":\"Not Found\"") != NULL;
			}
			if (missing) {
				free(listing);
				aiSayf(data, "no such repo or path: %s/%s%s%s", gh_owner, gh_repo,
				       want[0] ? "/" : "", want);
				/* Skill repos are laid out a dozen ways (caveman keeps its skills
				   under skills/, plugins/<name>/skills/ and .cursor/skills/). Rather
				   than make the user guess the path, ask the tree for every
				   directory that holds a SKILL.md and offer those. */
				char turl[512];
				snprintf(turl, sizeof(turl),
				         "https://api.github.com/repos/%s/%s/git/trees/HEAD?recursive=1",
				         gh_owner, gh_repo);
				hkHttpReq tr; memset(&tr, 0, sizeof(tr));
				tr.method = "GET"; tr.url = turl; tr.timeout_s = 25; tr.follow = 1;
				tr.headers = "-H 'Accept: application/vnd.github+json' ";
				char *tree = hk_http_fetch(&tr);
				if (tree) {
					int shown = 0;
					for (const char *q = tree; (q = strstr(q, "\"path\":\"")) != NULL; ) {
						q += 8;
						const char *e = strchr(q, '"');
						if (!e) break;
						size_t pl = (size_t)(e - q);
						if (pl > 9 && !memcmp(e - 9, "/SKILL.md", 9)) {
							if (!shown) aiAddHistory(data, "skills in this repo:");
							if (shown < 12)
								aiSayf(data, "  :skill install %s/%s/%.*s", gh_owner, gh_repo,
								       (int)(pl - 9), q);
							shown++;
						}
						q = e;
					}
					if (shown > 12) aiSayf(data, "  … and %d more", shown - 12);
					free(tree);
				}
				return 1;
			}
			/* A path holding a SKILL.md is one skill; otherwise treat its
			   subdirectories as a tap of many. */
			int single = strstr(listing, "\"name\":\"SKILL.md\"") != NULL;
			int installed = 0, failed = 0;
			char names[512] = "";               /* say WHICH, not only how many */
			size_t nl = 0;
			if (single) {
				const char *base = strrchr(want, '/');
				base = base ? base + 1 : want;
				if (hkInstallSkillDir(gh_owner, gh_repo, want, base, skills) > 0) {
					installed++;
					nl = hkCatf(names, nl, sizeof(names), "%s", base);
				}
				else failed++;
			} else {
				const char *p2 = listing;
				size_t olen;
				const char *obj;
				while ((obj = hkJsonArrayNext(&p2, &olen)) != NULL) {
					char *slice = malloc(olen + 1);
					if (!slice) break;
					memcpy(slice, obj, olen); slice[olen] = '\0';
					char *ty = hkExtractJsonString(slice, "type");
					char *nm = hkExtractJsonString(slice, "name");
					if (ty && nm && !strcmp(ty, "dir") && !strchr(nm, '/') && !strstr(nm, "..")) {
						char sub[640];
						if (want[0]) snprintf(sub, sizeof(sub), "%s/%s", want, nm);
						else         snprintf(sub, sizeof(sub), "%s", nm);
						if (hkInstallSkillDir(gh_owner, gh_repo, sub, nm, skills) > 0) {
							installed++;
							nl = hkCatf(names, nl, sizeof(names), "%s%s", nl ? ", " : "", nm);
						}
						else failed++;
					}
					free(ty); free(nm); free(slice);
				}
			}
			free(listing);
			hkPromptInvalidate();
			int total = hkLoadSkills(data);
			if (!installed) aiSayf(data, "nothing installed from %s/%s (no SKILL.md found)", gh_owner, gh_repo);
			else if (failed)  aiSayf(data, "installed %s \xC2\xB7 skipped %d without a SKILL.md (%d skills loaded)", names, failed, total);
			else {
				const char *hm = getenv("HOME");
				size_t hl = hm ? strlen(hm) : 0;
				int tilde = hl && !strncmp(skills, hm, hl);
				aiSayf(data, "installed %s from %s/%s into %s%s (%d skills loaded)", names, gh_owner, gh_repo,
				       tilde ? "~" : "", tilde ? skills + hl : skills, total);
			}
			return 1;
		}
		const char *slash = strrchr(url, '/');
		const char *name = slash ? slash + 1 : url;
		char outpath[1024];
		snprintf(outpath, sizeof(outpath), "%s/%s", skills, name);
		int nlen = strlen(name);
		if (nlen < 4 || strcmp(name + nlen - 3, ".md") != 0) {
			snprintf(outpath, sizeof(outpath), "%s/%s.md", skills, name);
		}
		hkHttpReq skreq = { .method = "GET", .url = url };
		char *skbody = hk_http_fetch(&skreq);
		if (!skbody || !*skbody) { free(skbody); aiAddHistory(data, "download failed"); return 1; }
		int wrote = hk_fs_write(outpath, skbody, strlen(skbody), 0);
		free(skbody);
		if (wrote != 0) { aiAddHistory(data, "download failed"); return 1; }
		hkPromptInvalidate();
		int n = hkLoadSkills(data);
		aiSayf(data, "installed: %s (%d total)", name, n);
		return 1;
	}
	if (strncmp(cmd, "tools", cmdlen) == 0 && cmdlen == 5)
		return hkToggle(data, arg, &E.ai_tools_enabled, "tools", NULL);
	if (strncmp(cmd, "toolgate", cmdlen) == 0 && cmdlen == 8)
		return hkToggle(data, arg, &E.ai_tool_gate, "toolgate",
			" — drops tool schema when the user message has no tool-keyword (helps small models).");
	if (strncmp(cmd, "theme", cmdlen) == 0 && cmdlen == 5) {
		int nn = hkThemeNames(hk_theme_pick, 64);
		if (!arg || !*arg) {
			int cur = 0;
			for (int i = 0; i < nn; i++) if (!strcmp(hk_theme_pick[i], TH_ACTIVE)) cur = i;
			char was[32];
			snprintf(was, sizeof(was), "%s", TH_ACTIVE);
			int pick = clPopupSelectLive("theme", hk_theme_pick, nn, cur, NULL, hkThemePreviewApply);
			if (pick < 0) {
				clThemeApply(was);
				hkThemeRefreshBar(data);
				return 1;
			}
			clThemeApply(hk_theme_pick[pick]);
			hkRcSaveTheme(HK_RC_CODE, TH_ACTIVE);
			hkThemeRefreshBar(data);
			clPipeEmitState();
			return 1;
		}
		int found = 0;
		for (int i = 0; i < nn; i++) if (!strcmp(hk_theme_pick[i], arg)) { found = 1; break; }
		if (!found) { aiAddHistory(data, "theme: unknown. try :theme without args to list."); return 1; }
		clThemeApply(arg);
		int wr = hkRcSaveTheme(HK_RC_CODE, TH_ACTIVE);
		hkThemeRefreshBar(data);
		clPipeEmitState();
		aiSayf(data, "theme: %s%s", TH_ACTIVE, wr == 0 ? " (saved to the rc)" : "");
		return 1;
	}
	if (strncmp(cmd, "toolmode", cmdlen) == 0 && cmdlen == 8) {
		int changed = 0;
		if (arg && (!strcmp(arg, "native") || !strcmp(arg, "fn"))) { E.ai_toolmode = 0; changed = 1; }
		else if (arg && (!strcmp(arg, "prose") || !strcmp(arg, "react") || !strcmp(arg, "xml"))) { E.ai_toolmode = 1; changed = 1; }
		hkPromptInvalidate();
		if (changed) { hkSaveSession(); hkLoadSkills(data); }
		char msg[256];
		snprintf(msg, sizeof(msg),
			"toolmode: %s%s — %s",
			E.ai_toolmode == 1 ? "prose" : "native",
			changed ? " (saved)" : "",
			E.ai_toolmode == 1
				? "models emit <tool>...</tool> blocks in prose (works with any instruct model)"
				: "uses provider's native function-calling schema (Anthropic/OpenAI/Qwen2.5/Llama3.1+)");
		aiAddHistory(data, msg);
		return 1;
	}
	if (strncmp(cmd, "trust", cmdlen) == 0 && cmdlen == 5) {
		if (arg && strcmp(arg, "revoke") == 0) {
			char dir[PATH_MAX];
			if (!hkProjectStateDir(dir, sizeof(dir))) { aiAddHistory(data, "no trust to revoke"); return 1; }
			char trust[PATH_MAX + 16];
			snprintf(trust, sizeof(trust), "%s/trust", dir);
			if (unlink(trust) == 0) aiAddHistory(data, "trust revoked");
			else aiAddHistory(data, "no trust to revoke");
			return 1;
		}
		if (hkProjectTrusted()) {
			aiAddHistory(data, "already trusted");
		} else if (hkGrantProjectTrust()) {
			aiAddHistory(data, "trusted. hako-code may edit files.");
			hkRcProjectLoad(data);           /* the folder's .hakorc may now be read */
		} else {
			aiAddHistory(data, "could not grant trust");
		}
		return 1;
	}
	if ((strncmp(cmd, "quit", cmdlen) == 0 && cmdlen == 4) ||
		(strncmp(cmd, "exit", cmdlen) == 0 && cmdlen == 4) ||
		(cmdlen == 1 && cmd[0] == 'q')) {
		return 2;
	}
	if (strncmp(cmd, "use", cmdlen) == 0 && cmdlen == 3) {
		hkSkillScan();                       /* every root, as the prompt and the / menu see them */
		char (*names)[64] = hk_skill_names;
		int  ncount = hk_skill_n;

		if (arg && (!strcmp(arg, "none") || !strcmp(arg, "off"))) {
			free(E.ai_active_skills);
			E.ai_active_skills = NULL;
			hkSaveSession();
			hkPromptInvalidate();
			hkLoadSkills(data);
			aiAddHistory(data, "no skill active");
			return 1;
		}
		if ((!arg || !*arg) && isatty(STDIN_FILENO) && ncount > 0) {
			const char *items[HK_SKILLS_MAX + 1];
			for (int i = 0; i < ncount; i++) items[i] = names[i];
			items[ncount] = "(none)";
			int pick = clPopupSelect("use skill", items, ncount + 1, 0, NULL);
			if (pick < 0) return 1;
			if (pick == ncount) {
				free(E.ai_active_skills);
				E.ai_active_skills = NULL;
				hkSaveSession();
				hkPromptInvalidate();
				hkLoadSkills(data);
				aiAddHistory(data, "no skill active");
				return 1;
			}
			free(E.ai_active_skills);
			E.ai_active_skills = strdup(names[pick]);
			hkSaveSession();
			hkPromptInvalidate();
			hkLoadSkills(data);
			aiSayf(data, "using '%s' — its instructions apply to every reply until :use none", names[pick]);
			return 1;
		}
		if (!arg || !*arg) {
			if (E.ai_active_skills && *E.ai_active_skills)
				aiSayf(data, "active: %s   (:use none to stop)", E.ai_active_skills);
			else if (ncount)
				aiSayf(data, "no skill active. :use <name> — installed: %d", ncount);
			else
				aiAddHistory(data, "no skills installed. :skill install <url>");
			return 1;
		}
		int found = -1;
		for (int i = 0; i < ncount; i++) if (!strcasecmp(names[i], arg)) found = i;
		if (found < 0) {
			aiSayf(data, "no skill named '%s'. :skills to list them", arg);
			return 1;
		}
		char picked[64];
		snprintf(picked, sizeof picked, "%s", names[found]);
		arg = picked;
		free(E.ai_active_skills);
		E.ai_active_skills = strdup(arg);
		hkSaveSession();
		hkPromptInvalidate();
		hkLoadSkills(data);
		aiSayf(data, "using '%s' — its instructions apply to every reply until :use none", arg);
		return 1;
	}
	if (strncmp(cmd, "remote", cmdlen) == 0 && cmdlen == 6) {
#ifdef HAKO_WASM
		aiAddHistory(data, "remote access needs the native hako — run :remote in a terminal");
		return 1;
#else
#if HK_ATTACH
		const char *ra = arg ? arg : "";
		if (!strcmp(ra, "off")) {
			if (!hs_attached) { aiAddHistory(data, "remote is not on"); return 1; }
			hsAttachOff();
			aiAddHistory(data, "remote: off, the link is closed");
			hkLogMessage("system", "remote off");
			return 1;
		}
		if (!strncmp(ra, "on", 2) && (!ra[2] || ra[2] == ' ')) {
			if (E.pipe_mode || E.serve_mode || !isatty(STDIN_FILENO)) {
				aiAddHistory(data, ":remote on shares a terminal session; run it in one");
				return 1;
			}
			int port; const char *tok;
			if (ra[2] && !hs_attached) hsAttachSetPort(atoi(ra + 3));
			hsAttachInfo(&port, &tok);
			if (!hs_attached && !hsAttachOn()) {
				aiSayf(data, "remote: could not listen on port %d (is a hako --serve running?)", port);
				return 1;
			}
			hsAttachInfo(&port, &tok);
			hkLogMessage("system", "remote on");
			int linked = hrStart() && hrLinked();
			/* two lines, not five: where to open it, and the one rule that matters */
			if (linked) aiSayf(data, "remote: on \xC2\xB7 phone %s (signed in)", hrStudioUrl());
			else aiAddHistory(data, "remote: on");
			aiSayfSecret(data, "  this computer http://127.0.0.1:%d/?t=%s", port, tok);
			aiAddHistory(data, "  every tool asks there \xC2\xB7 :remote off ends it \xC2\xB7 :help remote");
			return 1;
		}
		if (!strcmp(ra, "pair")) {
			if (E.pipe_mode || E.serve_mode || !isatty(STDIN_FILENO))
				aiAddHistory(data, ":remote pair shows a code in this terminal; run it in one");
			else if (!hs_attached || !hrLinked())
				aiAddHistory(data, "remote: run :remote on first, so this computer is linked to your account");
			else hrPairPrompt(data);
			return 1;
		}
#endif
		aiAddHistory(data, ":remote on    share this session with your signed-in phone (invite beta)");
		aiAddHistory(data, ":remote pair  add a browser that signed in by email");
		aiAddHistory(data, ":help remote  same wifi without an account, and what a phone may do");
		return 1;
#endif
	}
	if ((strncmp(cmd, "o", cmdlen) == 0 && cmdlen == 1) ||
	    (strncmp(cmd, "expand", cmdlen) == 0 && cmdlen == 6)) {
		if (!hk_last_tool_out || !*hk_last_tool_out) {
			aiAddHistory(data, "nothing to expand yet");
			return 1;
		}
		for (char *q = hk_last_tool_out; *q; ) {
			char *nl = strchr(q, '\n');
			if (nl) *nl = '\0';
			aiAddHistory(data, q);
			if (!nl) break;
			*nl = '\n';
			q = nl + 1;
		}
		return 1;
	}
	if (strncmp(cmd, "update", cmdlen) == 0 && cmdlen == 6) {
#ifdef HAKO_WASM
		aiAddHistory(data, "the page updates itself: reload it for the newest hako");
#else
		char tag[32] = "";
		if (!hk_update_ready && hkUpdateLatest(tag, sizeof tag, 1)) hkUpdateNote(tag);
		if (hk_update_ready) {
			aiSayf(data, "hako %s, and %s is out", HAKO_VERSION, hk_update_latest);
			aiAddHistory(data, "  hako --update replaces this binary (sha256 checked); or reinstall:");
			aiAddHistory(data, "  curl -fsSL https://mithraeums.github.io/hako.sh | sh");
		} else if (tag[0] || hkUpdateLatest(tag, sizeof tag, 0))
			aiSayf(data, "hako %s is the newest (%s)", HAKO_VERSION, tag);
		else aiSayf(data, "hako %s; could not reach GitHub to compare (try again later)", HAKO_VERSION);
#endif
		return 1;
	}
	if (strncmp(cmd, "voice", cmdlen) == 0 && cmdlen == 5) {
		int elsewhere = E.serve_mode || E.pipe_mode || hs_attached;
		if (arg && *arg) {
			int on = !strcmp(arg, "on") || !strcmp(arg, "1");
			if (!on && strcmp(arg, "off") && strcmp(arg, "0")) { aiAddHistory(data, "usage: :voice [on|off]"); return 1; }
			E.voice = on;
			if (!on) hkVoiceStop();
			hkRcSet("voice", on ? "1" : "0");
			clPipeEmitState();
			char cmdbuf[1400];
			if (on && !elsewhere && !hkVoiceCommand(cmdbuf, sizeof cmdbuf, "x"))
				aiAddHistory(data, "voice on, but no speaker here: install espeak-ng, or set voice_cmd in ~/.hakorc");
			else aiSayf(data, "voice %s%s", on ? "on" : "off",
			            on ? (elsewhere ? ": the browser reads each reply" : ": each reply is read aloud (esc stops it)") : "");
			return 1;
		}
		if (elsewhere) { aiAddHistory(data, "the browser reads replies when :voice is on"); return 1; }
		const char *r = hkLastReply(data);
		char why[200];
		if (!r) aiAddHistory(data, "nothing to read yet");
		else if (!hkVoiceSpeak(r, why, sizeof why)) aiAddHistory(data, why[0] ? why : "nothing to read");
		return 1;
	}
	if (strncmp(cmd, "mouse", cmdlen) == 0 && cmdlen == 5) {
		int was = E.mouse_enabled;
		if (arg && *arg) E.mouse_enabled = (!strcmp(arg, "on") || !strcmp(arg, "1")) ? 1 : 0;
		else             E.mouse_enabled = !was;
		if (was && !E.mouse_enabled)
			{ if (write(STDOUT_FILENO, "\x1b[?1006l\x1b[?1000l", 16) < 0) {} }
		aiSayf(data, "mouse %s%s", E.mouse_enabled ? "on" : "off",
		       E.mouse_enabled ? " \xE2\x80\x94 tap a result to open it; the wheel now goes to hako,"
		                         " so use shift+wheel to scroll" : " \xE2\x80\x94 the wheel scrolls again");
		return 1;
	}
	if (strncmp(cmd, "color", cmdlen) == 0 && cmdlen == 5) {
		if (!arg || !*arg) {
			char buf[512]; size_t o = 0;
			buf[0] = '\0';
			for (int i = 0; i < HK_CHAT_COLOR_COUNT; i++)
				o = hkCatf(buf, o, sizeof(buf), "%s%s%s ",
				              clSeq(HK_CHAT_COLORS[i].rgb), HK_CHAT_COLORS[i].name, ANSI_RESET);
			aiSayf(data, "chat color: %s", E.session_color && *E.session_color ? E.session_color : "theme default");
			aiAddHistory(data, buf);
			return 1;
		}
		if (!strcmp(arg, "none") || !strcmp(arg, "clear") || !strcmp(arg, "off")) {
			free(E.session_color); E.session_color = NULL;
			hkSaveSession(); hkLogSessionMeta();
			aiAddHistory(data, "chat color off");
			return 1;
		}
		int ok = 0;
		for (size_t i = 0; i < sizeof(HK_CHAT_COLORS)/sizeof(HK_CHAT_COLORS[0]); i++)
			if (!strcasecmp(arg, HK_CHAT_COLORS[i].name)) ok = 1;
		if (!ok) { aiSayf(data, "no color called '%s' — :color to list them", arg); return 1; }
		free(E.session_color); E.session_color = strdup(arg);
		hkSaveSession(); hkLogSessionMeta();
		aiSayf(data, "chat color: %s%s%s", hkChatColor(), arg, ANSI_RESET);
		return 1;
	}
	if (strncmp(cmd, "name", cmdlen) == 0 && cmdlen == 4) {
		if (!arg || !*arg) {
			if (E.session_name && *E.session_name) aiSayf(data, "chat: %s", E.session_name);
			else aiAddHistory(data, "this chat has no name — :name <something>");
			return 1;
		}
		free(E.session_name);
		E.session_name = strdup((!strcmp(arg, "none") || !strcmp(arg, "clear") || !strcmp(arg, "off")) ? "" : arg);
		hkSaveSession(); hkLogSessionMeta();
		clPipeEmitState();                   /* the studio and Rei show it at once */
#if HK_ATTACH
		hrRenamed();                         /* and the account's list, sealed */
#endif
		if (*E.session_name) aiSayf(data, "chat named: %s", E.session_name);
		else aiAddHistory(data, "chat name cleared");
		return 1;
	}
	if (strncmp(cmd, "memory", cmdlen) == 0 && cmdlen == 6) {
		if (arg && strncmp(arg, "forget ", 7) == 0) {
			const char *want = arg + 7;
			while (*want == ' ') want++;
			if (!*want || strchr(want, '/') || strstr(want, "..")) {
				aiAddHistory(data, "usage: :memory forget <name>");
				return 1;
			}
			int gone = 0;
			for (int g = 0; g < 2; g++) {
				char dir[640], path[PATH_MAX];
				if (hkMemDir(dir, sizeof(dir), g) != 0) continue;
				if (!hkFmtFits(path, sizeof(path), "%s/%s.md", dir, want)) continue;
				if (hk_fs_remove(path) == 0) gone++;
			}
			hkPromptInvalidate();
			hkLoadSkills(data);
			aiSayf(data, gone ? "forgot '%s'" : "no memory named '%s'", want);
			return 1;
		}
		int mcount = 0;
		char *mem = hkMemCollect(&mcount);
		if (!mcount) aiAddHistory(data, "nothing remembered yet");
		else {
			aiSayf(data, "%d memor%s:", mcount, mcount == 1 ? "y" : "ies");
			for (char *p2 = mem; p2 && *p2; ) {
				char *nl = strchr(p2, '\n');
				if (nl) *nl = '\0';
				if (*p2) aiAddHistory(data, p2);
				if (!nl) break;
				p2 = nl + 1;
			}
		}
		free(mem);
		return 1;
	}
	if (strncmp(cmd, "todo", cmdlen) == 0 && cmdlen == 4) {
		if (arg && (!strcmp(arg, "clear") || !strcmp(arg, "none"))) {
			free(hk_todo); hk_todo = NULL;
			hkSaveSession();
			hkPromptInvalidate();
			hkLoadSkills(data);
			aiAddHistory(data, "task list cleared");
			return 1;
		}
		hkTodoRender(data);
		return 1;
	}
	if (strncmp(cmd, "compact", cmdlen) == 0 && cmdlen == 7) {
		int before = hkContextPercent(data);
		int folded = hkCompactHistory(data);
		if (folded > 0)
			aiSayf(data, "folded %d messages: context %d%% -> %d%%",
			       folded, before, hkContextPercent(data));
		else
			aiSayf(data, "nothing to compact (context %d%% of %ld tokens)",
			       before, hkContextWindow());
		return 1;
	}
	if (strncmp(cmd, "usage", cmdlen) == 0 && cmdlen == 5) {
		if (arg && !strcmp(arg, "reset")) {
			data->last_in_tokens = data->last_out_tokens = 0;
			data->total_in_tokens = data->total_out_tokens = 0;
			data->total_cache_write = data->total_cache_read = 0;
			aiAddHistory(data, "(usage counters reset)");
			return 1;
		}
		char msg[256];
		aiSayf(data, "provider: %s", hkProviderLabel());
		aiSayf(data, "model:    %s", E.ai_model ? E.ai_model : "(unset)");
		aiSayf(data, "tools:    %s", E.ai_tools_enabled ? "on" : "off");
		aiSayf(data, "trust:    %s", hkProjectTrusted() ? "granted" : "not granted");
		hkPromptInvalidate();
		aiSayf(data, "skills:   %d loaded%s%s", hkLoadSkills(data),
		       (E.ai_active_skills && *E.ai_active_skills) ? ", active: " : "",
		       (E.ai_active_skills && *E.ai_active_skills) ? E.ai_active_skills : "");
		aiSayf(data, "stream:   %s", E.ai_stream ? "on" : "off");
		aiSayf(data, "cache:    %s", E.ai_prompt_cache ? "on" : "off");
		aiSayf(data, "context:  ~%ld of %ld tokens (%d%%), %d messages",
		       hkContextUsed(data), hkContextWindow(), hkContextPercent(data),
		       data->message_count);
		snprintf(msg, sizeof(msg), "session:  %s (%d turns)",
			E.session_id ? E.session_id : "(none)", E.session_turn_count);
		aiAddHistory(data, msg);
		char hpath[PATH_MAX];
		hkHistoryPath(hpath, sizeof(hpath));
		aiSayf(data, "history:  %s", hpath);
		snprintf(msg, sizeof(msg), "tokens:   last %d in / %d out  total %ld in / %ld out (cap %d)",
			data->last_in_tokens, data->last_out_tokens,
			data->total_in_tokens, data->total_out_tokens, E.ai_max_tokens);
		aiAddHistory(data, msg);
		if (data->total_cache_write || data->total_cache_read) {
			snprintf(msg, sizeof(msg), "cached:   %ld written / %ld read",
				data->total_cache_write, data->total_cache_read);
			aiAddHistory(data, msg);
		}
		double cost = hkSessionCostUSD(data);
		const char *flat = hkFreeTierLabel();
		if (flat && !strcmp(flat, "sub")) {
			aiAddHistory(data, "cost:     bundled in subscription");
		} else if (flat && !strcmp(flat, "free")) {
			aiAddHistory(data, "cost:     free tier (rate-limited by provider)");
		} else if (flat) {
			aiAddHistory(data, "cost:     $0 (local)");
		} else if (cost < 0) {
			aiSayf(data, "cost:     (no price entry for model '%s')", E.ai_model ? E.ai_model : "");
		} else {
			aiSayf(data, "cost:     $%.4f estimated (session-only; provider invoice is authoritative)", cost);
		}
		return 1;
	}
	if (strncmp(cmd, "sessions", cmdlen) == 0 && cmdlen == 8) {
		if (arg && strncmp(arg, "clear", 5) == 0) {
			int all = strstr(arg, "all") != NULL;
			int removed = hkClearSessions(all);
			hkSessionFresh(data);
			hkSaveSession();
			char msg[160];
			snprintf(msg, sizeof(msg), "cleared %d session log(s) %s — fresh session %s",
				removed, all ? "across ALL projects" : "in this project", E.session_id);
			aiAddHistory(data, msg);
			return 1;
		}
		char path[512];
		hkHistoryPath(path, sizeof(path));
		char *histbuf = hk_fs_read(path, 4 * 1024 * 1024, NULL);
		if (!histbuf) { aiAddHistory(data, "(no history)"); return 1; }
		char *histsave = histbuf;
		char ids[16][32];
		long lasts[16];
		int counts[16];
		char firsts[16][80];
		int n = 0;
		char *line;
		while ((line = hkNextLine(&histsave)) != NULL) {
			char *sidp = strstr(line, "\"sid\":\"");
			if (!sidp) continue;
			sidp += 7;
			char *send = strchr(sidp, '"');
			if (!send) continue;
			char id[32];
			int idlen = send - sidp;
			if (idlen >= (int)sizeof(id)) idlen = sizeof(id) - 1;
			memcpy(id, sidp, idlen); id[idlen] = '\0';
			if (!*id) continue;
			char *tsp = strstr(line, "\"ts\":");
			long ts = tsp ? atol(tsp + 5) : 0;
			int idx = -1;
			for (int i = 0; i < n; i++) if (strcmp(ids[i], id) == 0) { idx = i; break; }
			if (idx < 0) {
				if (n >= 16) continue;
				idx = n++;
				snprintf(ids[idx], sizeof(ids[idx]), "%s", id);
				firsts[idx][0] = '\0';
				counts[idx] = 0;
				char *role = strstr(line, "\"role\":\"user\"");
				if (role) {
					char *cp = strstr(line, "\"content\":\"");
					if (cp) {
						cp += 11;
						int j = 0;
						while (*cp && *cp != '"' && j < 60) firsts[idx][j++] = *cp++;
						firsts[idx][j] = '\0';
					}
				}
			}
			counts[idx]++;
			lasts[idx] = ts;
		}
		free(histbuf);
		if (n == 0) { aiAddHistory(data, "(no sessions)"); return 1; }
		long now = hk_time_unix();
		for (int i = 0; i < n; i++) {
			long age = now - lasts[i];
			char unit; long val;
			if (age < 3600) { val = age / 60; unit = 'm'; }
			else if (age < 86400) { val = age / 3600; unit = 'h'; }
			else { val = age / 86400; unit = 'd'; }
			const char *cur = (E.session_id && strcmp(E.session_id, ids[i]) == 0) ? "* " : "  ";
			aiSayf(data, "%s%s %ld%c %dt %.40s", cur, ids[i], val, unit, counts[i], firsts[i]);
		}
		aiAddHistory(data, "(:resume <id> to switch · :sessions clear [all] to wipe logs — keeps logins)");
		return 1;
	}
	if (strncmp(cmd, "resume", cmdlen) == 0 && cmdlen == 6) {
		if (!arg || !*arg) { aiAddHistory(data, "usage: /resume <id>"); return 1; }
		free(E.session_id);
		E.session_id = strdup(arg);
		E.session_resumed = 1;
		E.session_started = hk_time_unix();
		hkSaveSession();
		aiClearHistory(data);
		aiSayf(data, "resumed: %s", E.session_id);
		hkLoadHistoryTail(data, AI_HISTORY_MAX, 40);
		hkReplayTranscript(data);
		return 1;
	}
	if (strncmp(cmd, "session", cmdlen) == 0 && cmdlen == 7) {
		if (arg && strcmp(arg, "new") == 0) {
			hkSessionFresh(data);
			hkSaveSession();
			aiSayf(data, "new session: %s", E.session_id);
			return 1;
		}
		aiSayf(data, "id:      %s", E.session_id ? E.session_id : "(none)");
		long age = hk_time_unix() - E.session_started;
		aiSayf(data, "started: %ld min ago", age / 60);
		aiSayf(data, "turns:   %d", E.session_turn_count);
		aiSayf(data, "state:   %s", E.session_resumed ? "resumed" : "new");
		aiAddHistory(data, "(/session new to reset)");
		return 1;
	}
	/* /<skill> [text]: the skill as a mode, as :use does, and the text sent */
	if (cmdlen > 0 && cmdlen < 64) {
		hkSkillScan();
		for (int i = 0; i < hk_skill_n; i++) {
			if ((int)strlen(hk_skill_names[i]) != cmdlen || strncasecmp(hk_skill_names[i], cmd, cmdlen)) continue;
			char u[96];
			snprintf(u, sizeof u, ":use %s", hk_skill_names[i]);
			hkHandleSlashCmd(data, u);
			if (arg && *arg) {
				snprintf(hk_slash_prompt, sizeof hk_slash_prompt, "%s", arg);
				return 3;                       /* the caller sends it as a prompt */
			}
			return 1;
		}
	}
	{
		char guess[64] = "";            /* not `near`: windows.h defines that */
		int m[1];
		if (cmdlen > 0 && hkCmdMatch(prompt[0], cmd, (size_t)cmdlen, m, 1) == 1) {
			hkCmd c = hkCmdAt(m[0]);
			snprintf(guess, sizeof guess, " (did you mean %c%s?)", c.p, c.n);
		}
		aiSayf(data, "unknown command %.*s%s \xC2\xB7 :help lists them", cmdlen < 40 ? cmdlen + 1 : 41, prompt, guess);
	}
	return 1;
}

/* ---------- one rc for every tool (DESIGN 2026-10-29 §1) ----------
   Global: the first that exists of $HAKORC, ~/.hakorc, ~/.config/hako/hakorc.
   Project: <cwd>/.hakorc, read only once the folder is trusted, and never for
   a key that could run code or send the key somewhere else. `code.<key>` beats
   `<key>` within a file; the project file beats the global one. */
#define HK_RC_MAX 384
typedef struct { char *key, *val; unsigned char layer, tool; } hkRcEnt;
static hkRcEnt hk_rc[HK_RC_MAX];
static int  hk_rc_n = 0;
static char hk_rc_path[PATH_MAX];          /* the global file: read from, written to */
static char hk_rc_proj[PATH_MAX];          /* the project file once it is read, else "" */
static int  hk_rc_proj_done = 0;
static char hk_rc_denied[512];             /* project keys refused, said once */

static char *hkRcTrim(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\r') s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = '\0';
	return s;
}

/* The global file: found, or where a write would create it. 1 if it exists. */
static int hkRcFindGlobal(char *out, size_t cap) {
	const char *home = getenv("HOME");
	const char *env = getenv("HAKORC");
	char cand[3][PATH_MAX];
	int nc = 0;
	if (env && *env) snprintf(cand[nc++], PATH_MAX, "%s", env);
	if (home) snprintf(cand[nc++], PATH_MAX, "%s/.hakorc", home);
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) snprintf(cand[nc++], PATH_MAX, "%s/hako/hakorc", xdg);
	else if (home)   snprintf(cand[nc++], PATH_MAX, "%s/.config/hako/hakorc", home);
	for (int i = 0; i < nc; i++) {
		int is_dir = 0;
		if (hk_fs_exists(cand[i], NULL, NULL, &is_dir) && !is_dir && hkFmtFits(out, cap, "%s", cand[i])) return 1;
	}
	if (!home || !hkFmtFits(out, cap, "%s/.hakorc", home)) out[0] = '\0';
	return 0;
}

/* Keys a cloned repo may not set: they could run something or carry the key off. */
static int hkRcProjectDenied(const char *key) {
	static const char *DENY[] = { "ai_api_key", "ai_endpoint", "ai_auto_approve", "editor", "voice_cmd", NULL };
	for (int i = 0; DENY[i]; i++) if (!strcmp(key, DENY[i])) return 1;
	size_t n = strlen(key);
	if (n > 4 && !strcmp(key + n - 4, "_cmd"))
		return strcmp(key, "build_cmd") && strcmp(key, "test_cmd") && strcmp(key, "run_cmd");
	if (!strcmp(key, "ai_provider"))           /* the global key would follow it to that provider */
		for (int i = 0; i < hk_rc_n; i++) if (!hk_rc[i].layer && !strcmp(hk_rc[i].key, "ai_api_key")) return 1;
	return 0;
}

static void hkRcParse(const char *path, int layer) {
	if (!path || !*path) return;
	char *buf = hk_fs_read(path, 256 * 1024, NULL);
	if (!buf) return;
	char *save = buf, *line;
	while ((line = hkNextLine(&save)) != NULL && hk_rc_n < HK_RC_MAX) {
		line = hkRcTrim(line);
		if (!*line || *line == '#') continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char *key = hkRcTrim(line), *val = hkRcTrim(eq + 1);
		unsigned char tool = HK_RC_ANY;
		if      (!strncmp(key, "code.", 5))   { tool = HK_RC_CODE;   key += 5; }
		else if (!strncmp(key, "edit.", 5))   { tool = HK_RC_EDIT;   key += 5; }
		else if (!strncmp(key, "studio.", 7)) { tool = HK_RC_STUDIO; key += 7; }
		if (!*key) continue;
		if (layer && hkRcProjectDenied(key)) {
			if (!strstr(hk_rc_denied, key)) {
				size_t o = strlen(hk_rc_denied);
				hkCatf(hk_rc_denied, o, sizeof hk_rc_denied, "%s%s", o ? ", " : "", key);
			}
			continue;
		}
		hk_rc[hk_rc_n].key = strdup(key);
		hk_rc[hk_rc_n].val = strdup(val);
		hk_rc[hk_rc_n].layer = (unsigned char)layer;
		hk_rc[hk_rc_n].tool = tool;
		hk_rc_n++;
	}
	free(buf);
}

/* (Re)read both files. The project one only once it has been let in. */
static void hkRcReload(void) {
	for (int i = 0; i < hk_rc_n; i++) { free(hk_rc[i].key); free(hk_rc[i].val); }
	hk_rc_n = 0;
	hk_rc_denied[0] = '\0';
	hkRcParse(hk_rc_path, 0);
	if (hk_rc_proj[0]) hkRcParse(hk_rc_proj, 1);
}

/* Which entry decides `key` for `tool`: the project beats the global file, a
   tool's own prefixed key beats the plain one, and a later line beats an
   earlier one. NULL when the rc does not set it. */
static const hkRcEnt *hkRcFind(int tool, const char *key) {
	const hkRcEnt *best = NULL;
	int best_s = -1;
	for (int i = 0; i < hk_rc_n; i++) {
		const hkRcEnt *e = &hk_rc[i];
		if (e->tool != HK_RC_ANY && e->tool != tool) continue;
		if (strcmp(e->key, key)) continue;
		int sc = e->layer * 2 + (e->tool == tool);
		if (sc >= best_s) { best_s = sc; best = e; }
	}
	return best;
}

static const char *hkRcGet(int tool, const char *key) {
	const hkRcEnt *e = hkRcFind(tool, key);
	return e ? e->val : NULL;
}

/* "#rrggbb", "ansi0".."ansi15", "default", or hake's "r,g,b". */
static int hkRcColor(const char *v, unsigned *out) {
	if (!v) return 0;
	if (v[0] == '#' && strlen(v) == 7) {
		char *end; unsigned long x = strtoul(v + 1, &end, 16);
		if (*end) return 0;
		*out = (unsigned)x; return 1;
	}
	if (!strncasecmp(v, "ansi", 4) && isdigit((unsigned char)v[4])) {
		int n = atoi(v + 4);
		if (n < 0 || n > 15) return 0;
		*out = HK_C_ANSI(n < 8 ? 30 + n : 90 + n - 8); return 1;
	}
	if (!strcasecmp(v, "default")) { *out = HK_C_ANSI(39); return 1; }
	int r, g, b;
	if (sscanf(v, "%d,%d,%d", &r, &g, &b) == 3 && r >= 0 && r < 256 && g >= 0 && g < 256 && b >= 0 && b < 256) {
		*out = (unsigned)((r << 16) | (g << 8) | b); return 1;
	}
	return 0;
}

/* The shared slot names (§1) this file draws with, and where each falls back
   to when a custom theme names only the editor's slots. */
static const struct { const char *name, *from; } HK_THEME_SLOTS[TH_SLOTS] = {
	[TH_I_USER] = { "user", "keyword" }, [TH_I_AI] = { "ai", "fg" },
	[TH_I_SYS] = { "sys", "comment" },   [TH_I_ERR] = { "err", NULL },
	[TH_I_TOOL] = { "tool", "string" },  [TH_I_OK] = { "ok", "string" },
	[TH_I_ACCENT] = { "accent", "keyword" }, [TH_I_META] = { "meta", "comment" },
	[TH_I_GHOST] = { "ghost", "line_number" },
};

static const char *hkThemeSlotVal(int tool, const char *name, const char *slot) {
	char k[96];
	if (!hkFmtFits(k, sizeof k, "theme.%s.%s", name, slot)) return NULL;
	return hkRcGet(tool, k);
}

static int hkThemeIsCustom(int tool, const char *name) {
	if (!name || !*name) return 0;
	char pfx[64];
	if (!hkFmtFits(pfx, sizeof pfx, "theme.%s.", name)) return 0;
	size_t pl = strlen(pfx);
	for (int i = 0; i < hk_rc_n; i++)
		if ((hk_rc[i].tool == HK_RC_ANY || hk_rc[i].tool == tool) && !strncmp(hk_rc[i].key, pfx, pl)) return 1;
	return 0;
}

static int hkThemeCustom(const char *name, unsigned out[TH_SLOTS]) {
	if (!hkThemeIsCustom(HK_RC_CODE, name)) return 0;
	const char *base = hkThemeSlotVal(HK_RC_CODE, name, "base");
	int bi = 0;
	for (int i = 0; base && i < TH_PRESET_COUNT; i++) if (!strcmp(TH_PRESETS[i].name, base)) { bi = i; break; }
	memcpy(out, TH_PRESETS[bi].c, sizeof(unsigned) * TH_SLOTS);
	for (int k = 0; k < TH_SLOTS; k++) {
		unsigned c;
		const char *v = hkThemeSlotVal(HK_RC_CODE, name, HK_THEME_SLOTS[k].name);
		if (!v && HK_THEME_SLOTS[k].from) v = hkThemeSlotVal(HK_RC_CODE, name, HK_THEME_SLOTS[k].from);
		if (!v && k == TH_I_GHOST) v = hkThemeSlotVal(HK_RC_CODE, name, "comment");
		if (v && hkRcColor(v, &c)) out[k] = c;
	}
	return 1;
}

/* Every name a picker offers: the presets (terminal first), then the rc's own. */
static int hkThemeNames(const char **out, int max) {
	static char custom[16][40];
	int n = 0, nc = 0;
	for (int i = 0; i < TH_PRESET_COUNT && n < max; i++) {
		int dup = 0;
		for (int j = 0; j < n; j++) if (!strcmp(out[j], TH_PRESETS[i].name)) dup = 1;
		if (!dup) out[n++] = TH_PRESETS[i].name;
	}
	for (int i = 0; i < hk_rc_n && n < max && nc < 16; i++) {
		const char *k = hk_rc[i].key;
		if (strncmp(k, "theme.", 6) || (hk_rc[i].tool != HK_RC_ANY && hk_rc[i].tool != HK_RC_CODE)) continue;
		const char *dot = strchr(k + 6, '.');
		if (!dot || dot == k + 6 || dot - (k + 6) >= 40) continue;
		char nm[40];
		snprintf(nm, sizeof nm, "%.*s", (int)(dot - (k + 6)), k + 6);
		int dup = 0;
		for (int j = 0; j < n; j++) if (!strcmp(out[j], nm)) dup = 1;
		if (dup) continue;
		snprintf(custom[nc], sizeof custom[0], "%s", nm);
		out[n++] = custom[nc++];
	}
	return n;
}

/* One setting, wherever it came from. Returns 1 if this file knows the key. */
static void clRcApplyKey(const char *key, const char *val) {
	if (strcmp(key, "ai_provider") == 0) { hkApplyProviderAlias(val); hk_rc_pin.provider = 1; }
	else if (strcmp(key, "ai_api_key") == 0) { free(E.ai_api_key); E.ai_api_key = strdup(val); hk_rc_pin.api_key = 1; }
	else if (strcmp(key, "ai_endpoint") == 0) { free(E.ai_endpoint); E.ai_endpoint = strdup(val); hk_rc_pin.endpoint = 1; }
	else if (strcmp(key, "ai_model") == 0) { free(E.ai_model); E.ai_model = strdup(val); hk_rc_pin.model = 1; }
	else if (strcmp(key, "ai_max_tokens") == 0) { E.ai_max_tokens = atoi(val); hk_rc_pin.max_tokens = 1; }
	else if (strcmp(key, "shell_timeout") == 0) { E.ai_shell_timeout = atoi(val); }
	else if (strcmp(key, "editor") == 0) { free(E.editor); E.editor = strdup(val); }
	else if (strcmp(key, "prompt_cache") == 0) { E.ai_prompt_cache = atoi(val) ? 1 : 0; }
	else if (strcmp(key, "context_window") == 0) { E.ai_context_window = atol(val); }
	else if (strcmp(key, "ai_tools_enabled") == 0) { E.ai_tools_enabled = atoi(val) ? 1 : 0; hk_rc_pin.tools = 1; }
	else if (strcmp(key, "ai_stream") == 0) { E.ai_stream = atoi(val) ? 1 : 0; hk_rc_pin.stream = 1; }
	else if (strcmp(key, "show_splash") == 0) { E.show_splash = atoi(val) ? 1 : 0; }
	else if (strcmp(key, "mouse") == 0) { E.mouse_enabled = atoi(val) ? 1 : 0; }
	else if (strcmp(key, "ai_auto_approve") == 0) { E.ai_auto_approve = atoi(val) ? 1 : 0; hk_rc_pin.auto_approve = 1; }
	else if (strcmp(key, "update_check") == 0) { E.update_check = atoi(val) ? 1 : 0; }
	else if (strcmp(key, "voice") == 0) { E.voice = atoi(val) ? 1 : 0; }
	else if (strcmp(key, "voice_rate") == 0) { E.voice_rate = atoi(val); }
	else if (strcmp(key, "voice_cmd") == 0) { free(E.voice_cmd); E.voice_cmd = *val ? strdup(val) : NULL; }
	else if (strcmp(key, "anim_style") == 0) {
		E.anim_force_style = -1;
		for (int i = 0; i < CL_ANIM_COUNT; i++) {
			if (strcmp(val, CL_ANIMS[i].name) == 0) { E.anim_force_style = i; break; }
		}
	}
}

/* Apply what the table decides. layer_min 1 = only what the project file won. */
static void clRcApplyAll(int layer_min) {
	for (int i = 0; i < hk_rc_n; i++) {
		const hkRcEnt *e = &hk_rc[i];
		if (e->tool != HK_RC_ANY && e->tool != HK_RC_CODE) continue;
		if (!strcmp(e->key, "theme") || !strncmp(e->key, "theme.", 6)) continue;
		int first = 1;                          /* each key once, in first-seen order */
		for (int j = 0; j < i && first; j++)
			if ((hk_rc[j].tool == HK_RC_ANY || hk_rc[j].tool == HK_RC_CODE) && !strcmp(hk_rc[j].key, e->key)) first = 0;
		if (!first) continue;
		const hkRcEnt *w = hkRcFind(HK_RC_CODE, e->key);
		if (w && w->layer >= layer_min) clRcApplyKey(w->key, w->val);
	}
	const char *th = hkRcGet(HK_RC_CODE, "theme");
	clThemeApply(th && *th ? th : "terminal");
}

/* The theme a front end should draw with: studio asks for its own. */
static const char *hkThemeFor(int tool) {
	if (tool == HK_RC_CODE) return TH_ACTIVE;
	const char *th = hkRcGet(tool, "theme");
	return th && *th ? th : "terminal";
}

static void clLoadRc(void) {
	if (hkRcFindGlobal(hk_rc_path, sizeof hk_rc_path) && !getenv("HAKORC")) {
		/* children (ctrl+g's editor, the studio under --serve) read the same file */
#ifdef _WIN32
		_putenv_s("HAKORC", hk_rc_path);
#else
		setenv("HAKORC", hk_rc_path, 0);
#endif
	}
	hkRcReload();
	clRcApplyAll(0);
}

/* The project's .hakorc, once the folder is trusted. Called again after a
   :trust; a second call is free. */
static void hkRcProjectLoad(aiData *data) {
	if (hk_rc_proj_done || !hkProjectTrusted()) return;
	hk_rc_proj_done = 1;
	char cwd[PATH_MAX], path[PATH_MAX], rp1[PATH_MAX], rp2[PATH_MAX];
	if (!getcwd(cwd, sizeof cwd) || !hkFmtFits(path, sizeof path, "%s/.hakorc", cwd)) return;
	int is_dir = 0;
	if (!hk_fs_exists(path, NULL, NULL, &is_dir) || is_dir) return;
	if (realpath(path, rp1) && realpath(hk_rc_path, rp2) && !strcmp(rp1, rp2)) return;   /* ~ is the project */
	snprintf(hk_rc_proj, sizeof hk_rc_proj, "%s", path);
	hkRcReload();
	clRcApplyAll(1);
	if (hk_rc_denied[0] && data && data->history) aiSayf(data, "project .hakorc: %s ignored (set it in ~/.hakorc)", hk_rc_denied);
}

/* Set one key in the global rc, in place: its line rewritten, every other line
   and comment kept, appended when it is new. Returns 0 on success. */
static int hkRcSet(const char *key, const char *val) {
	if (!hk_rc_path[0]) hkRcFindGlobal(hk_rc_path, sizeof hk_rc_path);
	if (!hk_rc_path[0]) return -1;
	long len = 0;
	char *old = hk_fs_read(hk_rc_path, 256 * 1024, &len);
	size_t cap = (size_t)len + strlen(key) + strlen(val) + 8, n = 0;
	char *out = malloc(cap);
	if (!out) { free(old); return -1; }
	int done = 0;
	for (char *p = old; p && *p; ) {
		char *nl = strchr(p, '\n');
		size_t ll = nl ? (size_t)(nl - p) : strlen(p);
		char tmp[512];
		snprintf(tmp, sizeof tmp, "%.*s", (int)(ll < sizeof tmp ? ll : sizeof tmp - 1), p);
		char *t = hkRcTrim(tmp), *eq = strchr(t, '=');
		int hit = 0;
		if (*t != '#' && eq) { *eq = '\0'; hit = !strcmp(hkRcTrim(t), key); }
		if (hit && !done) { n = hkCatf(out, n, cap, "%s=%s\n", key, val); done = 1; }
		else if (!hit) { memcpy(out + n, p, ll); n += ll; out[n++] = '\n'; }
		p = nl ? nl + 1 : p + ll;
	}
	if (!done) n = hkCatf(out, n, cap, "%s=%s\n", key, val);
	free(old);
	int rc = hk_fs_write(hk_rc_path, out, n, 0);
	free(out);
	hkRcReload();
	return rc;
}

/* ---------- is there a newer hako? (DESIGN 2026-10-29 §1) ----------
   Once a day at most, in the background, cached at ~/.hako/cache/update. The
   bar and the state say so; nothing waits on it. Never in the browser build. */

static int hkVersionNewer(const char *a, const char *b) {
	int x[3] = {0}, y[3] = {0};
	if (*a == 'v') a++;
	if (*b == 'v') b++;
	if (sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]) < 2 || sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]) < 2) return 0;
	for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
	return 0;
}

/* Latest release tag: from the cache when it is under a day old (or `force`
   is 0 and the network is not wanted), else from GitHub. 1 when known. */
static int hkUpdateLatest(char *tag, size_t cap, int fetch) {
	tag[0] = '\0';
#ifdef HAKO_WASM
	(void)cap; (void)fetch;
	return 0;
#else
	char dir[512], path[640];
	hkClawDirPath(dir, sizeof dir);
	if (!dir[0] || !hkFmtFits(path, sizeof path, "%s/cache/update", dir)) return 0;
	long checked = 0;
	char *c = hk_fs_read(path, 4096, NULL);
	if (c) {
		char *l = strstr(c, "latest="), *t = strstr(c, "checked=");
		if (l) { l += 7; snprintf(tag, cap, "%.*s", (int)strcspn(l, "\r\n"), l); }
		if (t) checked = atol(t + 8);
		free(c);
	}
	if (!fetch || (checked && hk_time_unix() - checked < 86400)) return tag[0] != 0;
	char url[160];
	snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", HAKO_REPO);
	hkHttpReq req = { .method = "GET", .url = url, .timeout_s = 5 };
	char *resp = hk_http_fetch(&req);
	char *got = resp ? hkExtractJsonString(resp, "tag_name") : NULL;
	free(resp);
	if (got && *got && strlen(got) < 32) snprintf(tag, cap, "%s", got);
	free(got);
	char cdir[600], body[96];
	snprintf(cdir, sizeof cdir, "%s/cache", dir);
	hk_fs_mkdirp(cdir);
	int n = snprintf(body, sizeof body, "latest=%s\nchecked=%ld\n", tag, hk_time_unix());   /* a failure waits a day too */
	if (n > 0 && n < (int)sizeof body) hk_fs_write(path, body, (size_t)n, 0);
	return tag[0] != 0;
#endif
}

static void hkUpdateNote(const char *tag) {
	if (tag[0] && hkVersionNewer(tag, HAKO_VERSION)) {
		snprintf(hk_update_latest, sizeof hk_update_latest, "%s", tag[0] == 'v' ? tag + 1 : tag);
		hk_update_ready = 1;
	}
}

#ifndef HAKO_WASM
static void *hkUpdateThread(void *arg) {
	(void)arg;
	hkBlockWinch();
	char tag[32];
	if (hkUpdateLatest(tag, sizeof tag, 1)) hkUpdateNote(tag);
	return NULL;
}
#endif

static void hkUpdateCheckStart(void) {
#ifndef HAKO_WASM
	const char *ev = getenv("HAKO_UPDATE_CHECK");          /* tests and scripts: 0 */
	if (!E.update_check || E.output_json || (ev && !atoi(ev))) return;
	char tag[32];
	if (hkUpdateLatest(tag, sizeof tag, 0)) hkUpdateNote(tag);   /* the cache, at once */
	pthread_t t;
	if (pthread_create(&t, NULL, hkUpdateThread, NULL) == 0) pthread_detach(t);
#endif
}

/* The optional fields every front end may read (§4): session_name, theme,
   theme_slots for a custom theme, voice, update, and the command table
   (with_cmds 1 = always, -1 = only when the skills changed since last sent). */
static void hkStateExtras(char **b, size_t *len, size_t *cap, int tool, int with_cmds) {
	char tmp[600], esc[400];
	hkJsonEscapeInto(E.session_name ? E.session_name : "", esc, (int)sizeof esc);
	const char *th = hkThemeFor(tool);
	char the[96];
	hkJsonEscapeInto(th, the, (int)sizeof the);
	const char *mode = E.ai_plan_mode ? "plan" : (E.ai_auto_approve && !hs_remote) ? "auto" : "ask";
	int n = snprintf(tmp, sizeof tmp, ",\"session_name\":\"%s\",\"theme\":\"%s\",\"voice\":%d,\"voice_rate\":%d,"
	                 "\"mode\":\"%s\",\"shared\":%s", esc, the, E.voice ? 1 : 0, E.voice_rate, mode, hs_attached ? "true" : "false");
	if (n > 0) hkSbAdd(b, len, cap, tmp, strlen(tmp));
	if (hkThemeIsCustom(tool, th)) {
		static const char *SLOTS[] = { "bg", "fg", "comment", "keyword", "type", "string", "number", "line_number",
			"status_bg", "status_fg", "visual_bg", "visual_fg", "accent", "user", "ai", "sys", "err", "tool", "ok", "meta", "ghost", NULL };
		hkSbAdd(b, len, cap, ",\"theme_slots\":{", 16);
		int first = 1;
		for (int i = 0; SLOTS[i]; i++) {
			const char *v = hkThemeSlotVal(tool, th, SLOTS[i]);
			unsigned c;
			if (!v || !hkRcColor(v, &c) || (c & 0xff000000u)) continue;   /* hex only: ansi means "the profile's" */
			snprintf(tmp, sizeof tmp, "%s\"%s\":\"#%06x\"", first ? "" : ",", SLOTS[i], c);
			hkSbAdd(b, len, cap, tmp, strlen(tmp));
			first = 0;
		}
		hkSbAdd(b, len, cap, "}", 1);
	}
	if (hk_update_ready) {
		snprintf(tmp, sizeof tmp, ",\"update\":{\"current\":\"%s\",\"latest\":\"%s\"}", HAKO_VERSION, hk_update_latest);
		hkSbAdd(b, len, cap, tmp, strlen(tmp));
	}
	static long sent_sig = -1;
	long sig = hkCmdCount();
	for (int i = 0; i < hk_skill_n; i++) for (const char *q = hk_skill_names[i]; *q; q++) sig = sig * 31 + (unsigned char)*q;
	if (with_cmds > 0 || (with_cmds < 0 && sig != sent_sig)) {
		char *cj = hkCmdsJson();
		if (cj) {
			hkSbAdd(b, len, cap, ",\"commands\":", 12);
			hkSbAdd(b, len, cap, cj, strlen(cj));
			free(cj);
			sent_sig = sig;
		}
	}
}

/* The key that decides this tool's theme: `code.theme` if the file has one. */
static int hkRcSaveTheme(int tool, const char *name) {
	const char *pk = tool == HK_RC_STUDIO ? "studio.theme" : "code.theme";
	int own = 0;
	for (int i = 0; i < hk_rc_n; i++)
		if (hk_rc[i].layer == 0 && hk_rc[i].tool == tool && !strcmp(hk_rc[i].key, "theme")) own = 1;
	return hkRcSet(own ? pk : "theme", name);
}

static void clInitConfig(void) {
	memset(&E, 0, sizeof(E));
	clDetectTruecolor();
	clThemeApply(TH_ACTIVE);
	E.ai_provider_type = AI_PROVIDER_NONE;
	E.ai_temperature = 70;
	E.ai_max_tokens = 2048;
	E.ai_tools_enabled = 1;
	E.ai_tool_gate = 0;
	E.ai_stream = 1;
	E.ai_prompt_cache = 1;
	E.ai_auto_approve = 0;
	E.anim_force_style = -1;
	E.update_check = 1;
#ifndef _WIN32
	E.color_enabled = isatty(STDOUT_FILENO) ? 1 : 0;
	E.show_splash = 0;   /* off by default (2026-09-11); show_splash=1 in ~/.hakorc brings it back */
	E.mouse_enabled = 0;
#endif
}

static void clInitAI(aiData *data) {
	memset(data, 0, sizeof(*data));
	data->history = malloc(sizeof(char*) * AI_HISTORY_MAX);
	data->history_role = calloc(AI_HISTORY_MAX, 1);
	data->active = 1;
	pthread_mutex_init(&data->lock, NULL);
}

static void clCleanupAI(aiData *data) {
	if (!data) return;
	for (int i = 0; i < data->history_count; i++) free(data->history[i]);
	free(data->history);
	free(data->history_role);
	free(data->current_prompt);
	free(data->current_response);
	free(data->system_prompt);
	aiFreeMessages(data);
	pthread_mutex_destroy(&data->lock);
}

static void clCleanupConfig(void) {
	free(E.ai_api_key);
	free(E.ai_endpoint);
	free(E.ai_model);
	free(E.session_id);
	free(E.ai_oauth_provider);
	free(E.ai_oauth_refresh);
}

#ifndef _WIN32
static void clSigint(int sig) {
	(void)sig;
	E.interrupt = 1;
}
#endif



#if !defined(_WIN32) && !defined(HAKO_WASM)
static struct termios cl_orig_termios;

static int clEnableRaw(void) {
	if (!isatty(STDIN_FILENO)) return -1;
	if (tcgetattr(STDIN_FILENO, &cl_orig_termios) == -1) return -1;
	struct termios raw = cl_orig_termios;
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
	raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
	raw.c_cflag |= (CS8);
	raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) return -1;
	cl_raw_active = 1;
	if (write(STDOUT_FILENO, "\x1b[?2004h", 8) < 0) {}
	if (E.mouse_enabled && write(STDOUT_FILENO, "\x1b[?1000h\x1b[?1006h", 16) < 0) {}
	return 0;
}

static void clDisableRaw(void) {
	if (cl_raw_active) {
		if (E.mouse_enabled && write(STDOUT_FILENO, "\x1b[?1006l\x1b[?1000l", 16) < 0) {}
		if (write(STDOUT_FILENO, "\x1b[?2004l", 8) < 0) {}
		tcsetattr(STDIN_FILENO, TCSANOW, &cl_orig_termios);
		cl_raw_active = 0;
	}
}
#else
static int clEnableRaw(void) { return -1; }
static void clDisableRaw(void) { }
#endif

#define CL_INPUT_HIST_MAX 500
static char *cl_in_hist[CL_INPUT_HIST_MAX];
static int cl_in_hist_n = 0;
static int cl_in_hist_loaded = 0;

static void clInputHistPath(char *out, size_t cap) {
	const char *home = getenv("HOME");
	if (!home) home = ".";
	snprintf(out, cap, "%s/.hako/input_history", home);
}

static void clInputHistLoad(void) {
	if (cl_in_hist_loaded) return;
	cl_in_hist_loaded = 1;
	char p[512];
	clInputHistPath(p, sizeof(p));
	char *hbuf = hk_fs_read(p, 1024 * 1024, NULL);
	if (!hbuf) return;
	char *hsave = hbuf, *line;
	while ((line = hkNextLine(&hsave)) != NULL) {
		size_t n = strlen(line);
		if (!n) continue;
		if (cl_in_hist_n >= CL_INPUT_HIST_MAX) {
			free(cl_in_hist[0]);
			memmove(&cl_in_hist[0], &cl_in_hist[1], sizeof(cl_in_hist[0])*(CL_INPUT_HIST_MAX-1));
			cl_in_hist_n--;
		}
		cl_in_hist[cl_in_hist_n++] = strdup(line);
	}
	free(hbuf);
}

static void clInputHistAppend(const char *s) {
	if (!s || !*s) return;
	if (cl_in_hist_n > 0 && strcmp(cl_in_hist[cl_in_hist_n-1], s) == 0) return;
	if (cl_in_hist_n >= CL_INPUT_HIST_MAX) {
		free(cl_in_hist[0]);
		memmove(&cl_in_hist[0], &cl_in_hist[1], sizeof(cl_in_hist[0])*(CL_INPUT_HIST_MAX-1));
		cl_in_hist_n--;
	}
	cl_in_hist[cl_in_hist_n++] = strdup(s);

	char p[512]; clInputHistPath(p, sizeof(p));
	char dir[512]; snprintf(dir, sizeof(dir), "%s", p);
	char *slash = strrchr(dir, '/'); if (slash) *slash = '\0';
#ifdef _WIN32
	_mkdir(dir);
#else
	mkdir(dir, 0700);
#endif
	size_t cap = strlen(s) + 2;
	char *line = malloc(cap);
	if (line) {
		int n = snprintf(line, cap, "%s\n", s);
		if (n > 0) hk_fs_write(p, line, (size_t)n, 1);
		free(line);
	}
}

static int clVisibleLen(const char *s) {
	int n = 0;
	for (const char *p = s; *p; p++) {
		if (*p == '\x1b' && *(p+1) == '[') {
			while (*p && *p != 'm') p++;
			if (!*p) break;
		} else {
			n++;
		}
	}
	return n;
}

static int clTermCols(void) {
	if (cl_cols_pinned > 0) return cl_cols_pinned;
#ifdef HAKO_WASM
	return 80;
#else
#ifndef _WIN32
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col > 0) return ws.ws_col;
#endif
	return 80;
#endif
}

static int clTermRows(void) {
#ifdef HAKO_WASM
	return 24;
#else
#ifndef _WIN32
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_row > 0) return ws.ws_row;
#endif
	return 24;
#endif
}

static int cl_redraw_oldrows = 0;   /* declared above, with the bar that reads it */
static int cl_redraw_oldrpos = 0;

static void clRedrawReset(void) { cl_redraw_oldrows = 0; cl_redraw_oldrpos = 0; }

static void clAppend(char *ab, int *n, int cap, const char *s, int slen) {
	if (*n + slen >= cap) return;
	memcpy(ab + *n, s, slen);
	*n += slen;
}

static char *clGhostSuffix(const char *buf, int len) {
	if (len < 3) return NULL;
	if (buf[0] != ':' && buf[0] != '/') return NULL;
	const char *p = buf + 1;
	int pl = len - 1;
	if (memchr(p, ' ', (size_t)pl)) return NULL;
	const char *only = NULL; int matches = 0, total = hkCmdCount();
	for (int i = 0; i < total; i++) {
		hkCmd c = hkCmdAt(i);
		int cl = (int)strlen(c.n);
		if (cl > pl && memcmp(c.n, p, pl) == 0) { only = c.n; matches++; }
	}
	if (matches == 1) return strdup(only + pl);
	return NULL;
}

#define CL_COMP_MAX 512

static const char *HK_PROVIDER_WORDS[] = {
	"mithraeum", "anthropic", "anthropic-api", "claude", "claude-api", "openai",
	"github-copilot", "copilot", "github-models", "ghmodels", "chatgpt", "codex",
	"ollama", "ollamacloud", "ocloud", "local", "koi",
	"gemini", "google", "groq", "cerebras", "deepseek", "mistral",
	"together", "fireworks", "openrouter", "openrouter-api", "nous", "nous-api",
	"xai", "grok", "github", "custom", NULL
};

static int clCompAdd(char out[][160], int n, const char *word, const char *pfx, size_t plen) {
	if (n >= CL_COMP_MAX) return n;
	if (plen && strncmp(word, pfx, plen) != 0) return n;
	for (int i = 0; i < n; i++) if (!strcmp(out[i], word)) return n;
	snprintf(out[n], 160, "%s", word);
	return n + 1;
}

static int clCompPaths(const char *word, char out[][160], int max) {
	char dirpart[PATH_MAX] = ".", base[256] = "";
	const char *slash = strrchr(word, '/');
	if (slash) {
		size_t dl = (size_t)(slash - word);
		if (dl == 0) { dirpart[0] = '/'; dirpart[1] = '\0'; }
		else if (dl < sizeof(dirpart)) { memcpy(dirpart, word, dl); dirpart[dl] = '\0'; }
		snprintf(base, sizeof(base), "%s", slash + 1);
	} else {
		snprintf(base, sizeof(base), "%s", word);
	}
	char expanded[PATH_MAX];
	if (dirpart[0] == '~') {
		const char *home = getenv("HOME");
		snprintf(expanded, sizeof(expanded), "%s%s", home ? home : "", dirpart + 1);
	} else {
		snprintf(expanded, sizeof(expanded), "%s", dirpart);
	}
	DIR *d = opendir(expanded);
	if (!d) return 0;
	size_t blen = strlen(base);
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < max) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		if (blen == 0 && de->d_name[0] == '.') continue;
		if (blen && strncmp(de->d_name, base, blen) != 0) continue;
		char full[PATH_MAX + 300], cand[160];
		snprintf(full, sizeof(full), "%s/%s", expanded, de->d_name);
		struct stat st;
		int isdir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
		/* A completion is a display string; bound each part to the slot so a
		   very long filename cannot overrun it. */
		if (slash) snprintf(cand, sizeof(cand), "%.*s/%.120s%s", (int)(slash - word), word, de->d_name, isdir ? "/" : "");
		else snprintf(cand, sizeof(cand), "%.150s%s", de->d_name, isdir ? "/" : "");
		snprintf(out[n], 160, "%s", cand);
		n++;
	}
	closedir(d);
	return n;
}

static int clCompletions(const char *buf, int len, int *wstart,
                         int *is_path, char out[][160], int max) {
	(void)max;
	int ws = len;
	while (ws > 0 && buf[ws - 1] != ' ') ws--;
	*wstart = ws;
	*is_path = 0;
	const char *word = buf + ws;
	size_t wlen = (size_t)(len - ws);
	int n = 0;

	if (wlen == 0 && !(len > 0 && (buf[0] == ':' || buf[0] == '/'))) return 0;

	if ((buf[0] == ':' || buf[0] == '/') && ws == 0) {
		*wstart = 1;
		const char *pfx = buf + 1;
		size_t plen = (size_t)(len - 1);
		int total = hkCmdCount();
		for (int i = 0; i < total; i++) n = clCompAdd(out, n, hkCmdAt(i).n, pfx, plen);
		return n;
	}

	if (buf[0] == ':' || buf[0] == '/') {
		char cmd[32] = "";
		int ci = 0;
		for (int i = 1; i < len && buf[i] != ' ' && ci < (int)sizeof(cmd) - 1; i++) cmd[ci++] = buf[i];
		cmd[ci] = '\0';
		if (!strcmp(cmd, "provider") || !strcmp(cmd, "providers") || !strcmp(cmd, "login") || !strcmp(cmd, "logout")) {
			for (int i = 0; HK_PROVIDER_WORDS[i]; i++) n = clCompAdd(out, n, HK_PROVIDER_WORDS[i], word, wlen);
			return n;
		}
		if (!strcmp(cmd, "theme")) {
			const char *tn[64];
			int tc = hkThemeNames(tn, 64);
			for (int i = 0; i < tc; i++) n = clCompAdd(out, n, tn[i], word, wlen);
			return n;
		}
		if (!strcmp(cmd, "model") || !strcmp(cmd, "models") || !strcmp(cmd, "pull")) {
			static char names[HK_MODELS_MAX][96];
			int mn = hkGatherModels(names, HK_MODELS_MAX);
			for (int i = 0; i < mn; i++) n = clCompAdd(out, n, names[i], word, wlen);
			return n;
		}
		if (!strcmp(cmd, "tools") || !strcmp(cmd, "toolgate") || !strcmp(cmd, "auto")) {
			n = clCompAdd(out, n, "on", word, wlen);
			n = clCompAdd(out, n, "off", word, wlen);
			return n;
		}
		if (!strcmp(cmd, "toolmode")) {
			n = clCompAdd(out, n, "native", word, wlen);
			n = clCompAdd(out, n, "prose", word, wlen);
			return n;
		}
		if (!strcmp(cmd, "sessions")) {
			n = clCompAdd(out, n, "clear", word, wlen);
			return n;
		}
		if (!strcmp(cmd, "skills") || !strcmp(cmd, "mcp")) {
			n = clCompAdd(out, n, "reload", word, wlen);
			return n;
		}
		if (!strcmp(cmd, "skill")) {
			n = clCompAdd(out, n, "install", word, wlen);
			n = clCompAdd(out, n, "uninstall", word, wlen);
			return n;
		}
		if (!strcmp(cmd, "history")) {
			n = clCompAdd(out, n, "local", word, wlen);
			n = clCompAdd(out, n, "global", word, wlen);
			return n;
		}
		if (!strcmp(cmd, "trust")) {
			n = clCompAdd(out, n, "revoke", word, wlen);
			return n;
		}
	}

	*is_path = 1;
	return clCompPaths(word, out, CL_COMP_MAX);
}

static int clCompCommon(char cand[][160], int n) {
	if (n <= 0) return 0;
	int i = 0;
	for (;; i++) {
		char c = cand[0][i];
		if (!c) return i;
		for (int k = 1; k < n; k++) if (cand[k][i] != c) return i;
	}
}

/* The Tab listing goes into the transcript as a system line, not straight to… */
static void clCompList(char cand[][160], int n) {
	int cols = clTermCols() - 3;
	int w = 0;
	for (int i = 0; i < n; i++) { int l = (int)strlen(cand[i]); if (l > w) w = l; }
	w += 2;
	int percol = w > 0 ? cols / w : 1;
	if (percol < 1) percol = 1;
	char row[1024]; size_t o = 0;
	for (int i = 0; i < n; i++) {
		o += (size_t)snprintf(row + o, sizeof(row) - o, "%-*s", w, cand[i]);
		if ((i + 1) % percol == 0 || i == n - 1) {
			while (o > 0 && row[o - 1] == ' ') row[--o] = '\0';
			aiAddHistory(&G_AI, row);
			o = 0; row[0] = '\0';
		}
		if (o >= sizeof(row) - 200) { aiAddHistory(&G_AI, row); o = 0; row[0] = '\0'; }
	}
}

static char cl_last_ghost[512];

/* Ending the input line: the bar owns the row below the field, so a newline
   there would push the cursor out of the block the erase is measured against. */
static void clLineBreak(void) {
	if (cl_bar_h) { if (write(STDOUT_FILENO, "\r", 1) < 0) {} return; }
	if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
}

static void clRedrawLine(const char *prompt, const char *buf, int len, int cursor) {
	clTtyLock();
	if (cl_composing && cl_bar_want && !cl_bar_h && clBarUsable()) {
		cl_line_prompt = prompt; cl_line_buf = buf; cl_line_len = len; cl_line_cur = cursor;
		clTtyUnlock();
		return;
	}
	int plen = clVisibleLen(prompt);
	int cols = clTermCols();
	if (cols < 1) cols = 80;

	char *ghost = (cursor == len) ? clGhostSuffix(buf, len) : NULL;
	int glen = ghost ? (int)strlen(ghost) : 0;
	if (ghost) {
		if (glen >= (int)sizeof(cl_last_ghost)) glen = (int)sizeof(cl_last_ghost) - 1;
		memcpy(cl_last_ghost, ghost, glen);
		cl_last_ghost[glen] = '\0';
	} else {
		cl_last_ghost[0] = '\0';
	}

	int wl = clCellsN(buf, len), wc = clCellsN(buf, cursor);
	int gw = ghost ? clCellsN(cl_last_ghost, glen) : 0;
	int rows = (plen + wl + gw + cols - 1) / cols;
	if (rows < 1) rows = 1;
	cl_input_rows = rows;

	cl_line_buf = buf; cl_line_len = len;      /* the menu is computed from the line */
	long msig = clMenuCompute(buf, len);
	if (cl_bar_h && (rows != cl_field_rows || msig != cl_menu_drawn)) {
		char pre[64];
		int pn = 0;
		int back = cl_redraw_oldrows ? (cl_redraw_oldrows - 1 - cl_redraw_oldrpos) : 0;
		if (back > 0) pn += snprintf(pre + pn, sizeof(pre) - pn, "\x1b[%dB", back);
		int upc = cl_redraw_oldrows ? cl_redraw_oldrows - 1 : 0;
		if (upc > 0) pn += snprintf(pre + pn, sizeof(pre) - pn, "\r\x1b[0K\x1b[%dA", upc);
		pn += snprintf(pre + pn, sizeof(pre) - pn, "\r\x1b[0K");
		if (write(STDOUT_FILENO, pre, pn) < 0) {}
		cl_redraw_oldrows = 0;
		cl_redraw_oldrpos = 0;
		cl_field_rows = rows;
		clBarHide();
		clBarShow(&G_AI);
	}

	char ab[16384];
	int n = 0;
	char tmp[64];
	int t;

	clAppend(ab, &n, sizeof(ab), "\x1b[?25l", 6);

	int below = cl_redraw_oldrows ? (cl_redraw_oldrows - 1 - cl_redraw_oldrpos) : 0;
	if (below > 0) {
		t = snprintf(tmp, sizeof(tmp), "\x1b[%dB", below);
		clAppend(ab, &n, sizeof(ab), tmp, t);
	}
	int j;
	int upclears = cl_redraw_oldrows ? cl_redraw_oldrows - 1 : 0;
	for (j = 0; j < upclears; j++) {
		clAppend(ab, &n, sizeof(ab), "\r\x1b[0K\x1b[1A", 10);
	}
	clAppend(ab, &n, sizeof(ab), "\r\x1b[0K", 5);

	clAppend(ab, &n, sizeof(ab), prompt, (int)strlen(prompt));
	clAppend(ab, &n, sizeof(ab), buf, len);

	if (ghost && glen > 0) {
		clAppend(ab, &n, sizeof(ab), TH_GHOST, (int)strlen(TH_GHOST));
		clAppend(ab, &n, sizeof(ab), ghost, glen);
		clAppend(ab, &n, sizeof(ab), ANSI_RESET, (int)strlen(ANSI_RESET));
	}

	int rows_after = rows;
	if (cursor == len && (wl + gw) > 0 && (plen + wl + gw) % cols == 0) {
		clAppend(ab, &n, sizeof(ab), "\n\r", 2);
		rows_after++;
	}

	int rpos2 = (plen + wc) / cols;
	int up    = (rows_after - 1) - rpos2;
	if (up > 0) {
		t = snprintf(tmp, sizeof(tmp), "\x1b[%dA", up);
		clAppend(ab, &n, sizeof(ab), tmp, t);
	}
	int col = (plen + wc) % cols;
	clAppend(ab, &n, sizeof(ab), "\r", 1);
	if (col > 0) {
		t = snprintf(tmp, sizeof(tmp), "\x1b[%dC", col);
		clAppend(ab, &n, sizeof(ab), tmp, t);
	}

	clAppend(ab, &n, sizeof(ab), "\x1b[?25h", 6);

	cl_redraw_oldrows = rows_after;
	cl_redraw_oldrpos = rpos2;
	cl_line_prompt = prompt;
	cl_line_buf = buf;
	cl_line_len = len;
	cl_line_cur = cursor;

	if (write(STDOUT_FILENO, ab, n) < 0) {}
	free(ghost);
	clTtyUnlock();
}

/* Step over a whole UTF-8 character: the cursor is a byte offset, and moving it
   one byte at a time put it inside é and 🎮 (two and four presses to get past). */
static int clU8Prev(const char *b, int i) {
	if (i <= 0) return 0;
	i--;
	while (i > 0 && ((unsigned char)b[i] & 0xC0) == 0x80) i--;
	return i;
}
static int clU8Next(const char *b, int i, int len) {
	if (i >= len) return len;
	i++;
	while (i < len && ((unsigned char)b[i] & 0xC0) == 0x80) i++;
	return i;
}

/* ---------- voice: a finished reply, read aloud (DESIGN 2026-10-29 §1) ----------
   A background child in its own process group, the text in a file it reads, so
   nothing here ever waits on it. Under --serve or :remote hako stays quiet: the
   state says voice, and the browser speaks. */
#if !defined(_WIN32) && !defined(HAKO_WASM)
static volatile pid_t hk_voice_pid = 0;
#endif

static void hkVoiceStop(void) {
#if !defined(_WIN32) && !defined(HAKO_WASM)
	pid_t p = hk_voice_pid;
	hk_voice_pid = 0;
	if (p <= 0 || waitpid(p, NULL, WNOHANG) != 0) return;     /* gone already (and reaped) */
	kill(-p, SIGTERM);
	for (int i = 0; i < 20; i++) { if (waitpid(p, NULL, WNOHANG) != 0) return; cl_sleep_ms(5); }
	kill(-p, SIGKILL);
	waitpid(p, NULL, 0);
#endif
}

/* What a voice can say: no markdown marks, no code (one "code skipped" per block). */
static char *hkVoiceText(const char *md) {
	char *clean = hkStripToolBlocks(md ? md : "");
	if (!clean) return NULL;
	size_t cap = strlen(clean) + 64, o = 0;
	char *out = malloc(cap);
	if (!out) { free(clean); return NULL; }
	int fence = 0;
	for (char *line = clean; line && *line; ) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = '\0';
		char *t = line;
		while (*t == ' ' || *t == '\t') t++;
		if (!strncmp(t, "```", 3)) {
			if (!fence) o = hkCatf(out, o, cap, "code skipped. ");
			fence = !fence;
		} else if (!fence) {
			while (*t == '#' || *t == '>') t++;
			while (*t == ' ') t++;
			if ((*t == '-' || *t == '*' || *t == '+') && t[1] == ' ') t += 2;
			for (char *q = t; *q; q++) {
				if (*q == '[' && strstr(q, "](")) {           /* [text](url): the text */
					char *mid = strstr(q, "](");
					char *end = strchr(mid, ')');
					if (end) { o = hkCatf(out, o, cap, "%.*s", (int)(mid - q - 1), q + 1); q = end; continue; }
				}
				if (*q == '*' || *q == '`' || *q == '#' || (*q == '_' && (q == t || q[-1] == ' ' || q[1] == ' ' || !q[1]))) continue;
				if (*q == '|') { o = hkCatf(out, o, cap, ", "); continue; }
				if (*q == ' ' && (!o || out[o - 1] == ' ')) continue;   /* one space, never a run */
				if (o + 2 < cap) { out[o++] = *q; out[o] = '\0'; }
			}
			if (o && out[o - 1] != ' ' && o + 2 < cap) { out[o++] = ' '; out[o] = '\0'; }
		}
		line = nl ? nl + 1 : NULL;
	}
	free(clean);
	return out;
}

/* The speaker to use, as a shell command reading `file`; 0 when there is none. */
static int hkVoiceCommand(char *cmd, size_t cap, const char *file) {
	if (E.voice_cmd && *E.voice_cmd) return hkFmtFits(cmd, cap, "%s < '%s'", E.voice_cmd, file);
#if defined(_WIN32)
	int r = E.voice_rate > 0 ? (E.voice_rate - 180) / 20 : 0;
	if (r < -10) r = -10;
	if (r > 10) r = 10;
	return hkFmtFits(cmd, cap, "start \"\" /b powershell -NoProfile -Command \"Add-Type -AssemblyName System.Speech;"
	                 " $s=New-Object System.Speech.Synthesis.SpeechSynthesizer; $s.Rate=%d;"
	                 " $s.Speak([IO.File]::ReadAllText('%s'))\"", r, file);
#elif defined(__APPLE__)
	char rate[24] = "";
	if (E.voice_rate > 0) snprintf(rate, sizeof rate, " -r %d", E.voice_rate);
	return hkOnPath("say") && hkFmtFits(cmd, cap, "say%s < '%s'", rate, file);
#else
	if (hkOnPath("spd-say")) return hkFmtFits(cmd, cap, "spd-say -e < '%s'", file);
	char rate[24] = "";
	if (E.voice_rate > 0) snprintf(rate, sizeof rate, " -s %d", E.voice_rate);
	if (hkOnPath("espeak-ng")) return hkFmtFits(cmd, cap, "espeak-ng%s --stdin < '%s'", rate, file);
	if (hkOnPath("espeak")) return hkFmtFits(cmd, cap, "espeak%s --stdin < '%s'", rate, file);
	return 0;
#endif
}

/* Speak `md` in the background. 0 and a reason in `why` when it cannot. */
static int hkVoiceSpeak(const char *md, char *why, size_t wcap) {
	if (why && wcap) why[0] = '\0';
#ifdef HAKO_WASM
	(void)md;
	if (why) snprintf(why, wcap, "the browser reads replies itself");
	return 0;
#else
	hkVoiceStop();
	char dir[512], file[640], cmd[1400];
	hkClawDirPath(dir, sizeof dir);
	if (!dir[0] || !hkFmtFits(file, sizeof file, "%s/cache/voice.txt", dir) || strchr(file, '\'')) {
		if (why) snprintf(why, wcap, "voice: no place to put the text");
		return 0;
	}
	if (!hkVoiceCommand(cmd, sizeof cmd, file)) {
		if (why) snprintf(why, wcap, "voice: no speaker found (macOS say, spd-say, espeak-ng, espeak); set voice_cmd in ~/.hakorc");
		return 0;
	}
	char *text = hkVoiceText(md);
	if (!text || !*text) { free(text); return 0; }
	char cdir[600];
	snprintf(cdir, sizeof cdir, "%s/cache", dir);
	hk_fs_mkdirp(cdir);
	int wr = hk_fs_write(file, text, strlen(text), 0);
	free(text);
	if (wr != 0) { if (why) snprintf(why, wcap, "voice: could not write %.120s", file); return 0; }
#ifdef _WIN32
	int rc = system(cmd);
	(void)rc;
#else
	chmod(file, 0600);
	pid_t pid = fork();
	if (pid == 0) {
		setpgid(0, 0);
		int dn = open("/dev/null", O_RDWR);
		if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	if (pid < 0) { if (why) snprintf(why, wcap, "voice: could not start the speaker"); return 0; }
	setpgid(pid, pid);
	hk_voice_pid = pid;
#endif
	return 1;
#endif
}

/* The newest reply the model wrote, as text. */
static const char *hkLastReply(aiData *data) {
	for (int i = data->message_count - 1; i >= 0; i--) {
		aiMessage *m = &data->messages[i];
		if (m->role && !strcmp(m->role, "assistant") && !m->raw && m->content && *m->content) return m->content;
	}
	return NULL;
}

/* After a turn: read it out when voice is on and this terminal is the listener. */
static void hkVoiceAfterTurn(aiData *data) {
	if (!E.voice || E.interrupt || E.serve_mode || E.pipe_mode || hs_attached || !isatty(STDOUT_FILENO)) return;
	const char *r = hkLastReply(data);
	char why[200];
	if (r && !hkVoiceSpeak(r, why, sizeof why) && why[0]) aiAddHistory(data, why);
}

static int clReadLineRaw2(const char *prompt, char *out, size_t cap);
static int clReadLineRaw(const char *prompt, char *out, size_t cap) {
	clTtyLock();
	cl_menu_live = 1; cl_menu_hidden = 0; cl_menu_word[0] = '\0'; cl_menu_drawn = 0;
	clTtyUnlock();
	int r = clReadLineRaw2(prompt, out, cap);
	clTtyLock();
	/* the menu goes with the reader; its line buffer is about to be gone */
	cl_menu_live = 0; cl_menu_n = 0; cl_line_buf = NULL;
	clTtyUnlock();
	return r;
}

/* Up/down in the open menu, and what Tab or Enter take from it. */
static int clMenuMove(int d) {
	if (cl_menu_n <= 0) return 0;
	cl_menu_sel += d;
	if (cl_menu_sel < 0) cl_menu_sel = cl_menu_count - 1;          /* wraps, like the editor's */
	if (cl_menu_sel >= cl_menu_count) cl_menu_sel = 0;
	cl_menu_moved = 1;
	return 1;
}

/* The selected entry into the line. For Enter (run=1) only when the line is a
   prefix of it or the user moved to it; Tab always, with a space for args. */
static int clMenuTake(char *buf, int *len, int *cur, int cap, int run) {
	if (cl_menu_n <= 0 || cl_menu_sel >= cl_menu_count) return 0;
	hkCmd c = hkCmdAt(cl_menu_idx[cl_menu_sel]);
	if (run && !cl_menu_moved && strncmp(c.n, buf + 1, (size_t)(*len - 1))) return 0;
	int n = snprintf(buf, (size_t)cap, "%c%s%s", c.p, c.n, !run && c.a[0] ? " " : "");
	if (n < 0 || n >= cap) return 0;
	*len = *cur = n;
	return 1;
}

static int clReadLineRaw2(const char *prompt, char *out, size_t cap) {
	if (clEnableRaw() != 0) {
		printf("%s", prompt); fflush(stdout);
		if (!fgets(out, cap, stdin)) return -1;
		size_t n = strlen(out);
		if (n && out[n-1] == '\n') out[--n] = '\0';
		return (int)n;
	}
	clInputHistLoad();
	clRedrawReset();

	char buf[4096];
	int len = 0, cur = 0;
	int hist_idx = cl_in_hist_n;
	char saved[4096]; saved[0] = '\0';
	int in_paste = 0, paste_cr = 0;
	buf[0] = '\0';

	if (cl_preset_input) {
		size_t pl = strlen(cl_preset_input);
		if (pl >= sizeof(buf)) pl = sizeof(buf) - 1;
		memcpy(buf, cl_preset_input, pl);
		buf[pl] = '\0';
		len = cur = (int)pl;
		free(cl_preset_input); cl_preset_input = NULL;
	}

	clRedrawLine(prompt, buf, len, cur);

	while (1) {
		char c;
		ssize_t r;
		/* A signal that landed while this thread was waiting on the tty lock
		   interrupted no read; look before blocking in one. */
#if !defined(_WIN32) && !defined(HAKO_WASM)
		if (cl_resize_until && !hk_need_stdin && !cl_turn_done) {
			long left = cl_resize_until - hk_time_ms();
			struct pollfd pf = { STDIN_FILENO, POLLIN, 0 };
			int pr = left > 0 ? poll(&pf, 1, (int)left) : 0;
			if (pr < 0 && errno == EINTR) {
				if (cl_winch) {                  /* another resize: it restarts the wait */
					cl_winch = 0;
					cl_resize_until = hk_time_ms() + CL_RESIZE_SETTLE_MS;
				}
				continue;
			}
			if (pr == 0 && hk_time_ms() < cl_resize_until) continue;
			cl_resize_until = 0;                 /* settled, or a key: draw once, at this width */
			cl_redraw_oldrows = 0;
			cl_redraw_oldrpos = 0;
			if (cl_bar_want) clBarShow(&G_AI);
			clRedrawLine(prompt, buf, len, cur);
		}
#endif
		if (hk_need_stdin || cl_turn_done) { r = -1; errno = EINTR; }
		else r = read(STDIN_FILENO, &c, 1);
		if (r <= 0) {
			if (r < 0 && errno == EINTR && hk_need_stdin) {
				buf[len] = '\0';
				free(cl_preset_input);
				cl_preset_input = len > 0 ? strdup(buf) : NULL;
				clDisableRaw();
				/* Take the bar down BEFORE saying the keyboard is free: once it is,
				   the worker prints the approval, and a hide racing that print
				   climbed from the wrong row and left the bar's top rule behind. */
				clTtyLock();
				cl_field_rows = 1;
				clBarHide();
				clTtyUnlock();
				hk_stdin_free = 1;
				out[0] = '\0';
				return -6;                 /* the worker needs the keyboard */
			}
			if (r < 0 && errno == EINTR && cl_turn_done) {
				cl_turn_done = 0;
				buf[len] = '\0';
				free(cl_preset_input);
				cl_preset_input = len > 0 ? strdup(buf) : NULL;
				clDisableRaw();
				out[0] = '\0';
				return -5;
			}
			if (r < 0 && errno == EINTR && cl_winch) {
				cl_winch = 0;
				/* Erasing from the cursor down left the bar's top rule (and the
				   blank row above it) behind on EVERY resize — one stale rule per
				   drag step, stacking up the screen. The rows above the cursor
				   are knowable: every bar row ends in \n, so the terminal reflows
				   each on its own — a row drawn w cells wide now occupies
				   ceil(w / W) rows — and the field is one soft-wrapped line with
				   the cursor on row (prompt + cursor) / W of it. Climb that, then
				   erase. Widening: ceil is 1, the common case in the report. */
				int up = 0;
				if (cl_bar_h) {
					int W = clTermCols(); if (W < 1) W = 80;
					int w_old = (cl_bar_cols > 0 ? cl_bar_cols : W) - 1;
					int per = (w_old + W - 1) / W; if (per < 1) per = 1;
					up = (cl_bar_field > 0 ? 1 + (cl_bar_field - 1) * per : 0)   /* the blank row is empty */
					   + (clVisibleLen(prompt) + clCellsN(buf, cur)) / W;
				}
				cl_redraw_oldrows = 0;   /* the old wrap no longer describes the screen */
				cl_redraw_oldrpos = 0;
				cl_bar_h = 0;
				cl_bar_field = 0;
				cl_bar_cols = 0;
				cl_field_rows = 1;
				printf("\r");
				if (up > 0) printf("\x1b[%dA", up);
				fputs("\x1b[J", stdout);
				fflush(stdout);
				if (cl_bar_want && clBarUsable()) {
					cl_resize_until = hk_time_ms() + CL_RESIZE_SETTLE_MS;
					continue;                  /* drawn once the width holds still */
				}
				clRedrawLine(prompt, buf, len, cur);
				continue;
			}
			if (r < 0 && errno == EINTR) {
				clDisableRaw();
				clLineBreak();
				out[0] = '\0';
				return -2;
			}
			clDisableRaw();
			return -1;
		}

		if ((c == '\r' || c == '\n') && !in_paste) {
			if (clMenuTake(buf, &len, &cur, (int)sizeof(buf), 1)) {
				cl_menu_hidden = 1;                  /* the line shows what runs, without the menu */
				clRedrawLine(prompt, buf, len, cur);
			}
			clLineBreak();
			clDisableRaw();
			buf[len] = '\0';
			if ((size_t)len < cap) memcpy(out, buf, len + 1);
			else { memcpy(out, buf, cap - 1); out[cap-1] = '\0'; len = (int)cap - 1; }
			if (len > 0) clInputHistAppend(out);
			return len;
		}
		if ((c == '\r' || c == '\n') && in_paste) {
			/* Terminals send CRLF for a line break inside a paste, and a space
			   per BYTE put two spaces in for every line the user pasted. Treat
			   the pair as one break. */
			int was_cr = paste_cr;
			paste_cr = (c == '\r');
			if (c == '\n' && was_cr) continue;
			if (len + 1 < (int)sizeof(buf)) {
				memmove(&buf[cur+1], &buf[cur], len - cur);
				buf[cur++] = ' ';
				len++; buf[len] = '\0';
				clRedrawLine(prompt, buf, len, cur);
			}
			continue;
		}
		/* A tab INSIDE a paste is data, not the completion key. It used to
		   reach the ctrl+i handler below and vanish, so pasted text silently
		   lost characters. One space: a real tab would render to a tab stop
		   the cell accounting knows nothing about. */
		if (c == 9 && in_paste) {
			paste_cr = 0;
			if (len + 1 < (int)sizeof(buf)) {
				memmove(&buf[cur+1], &buf[cur], len - cur);
				buf[cur++] = ' ';
				len++; buf[len] = '\0';
				clRedrawLine(prompt, buf, len, cur);
			}
			continue;
		}

		paste_cr = 0;

		if (c == 3) {
			if (write(STDOUT_FILENO, "^C", 2) < 0) {}
			clLineBreak();
			clDisableRaw();
			out[0] = '\0';
			return -2;
		}
		if (c == 4) {
			if (len == 0) {
				clDisableRaw();
				clLineBreak();
				return -1;
			}
			if (cur < len) {
				memmove(&buf[cur], &buf[cur+1], len - cur - 1);
				len--; buf[len] = '\0';
				clRedrawLine(prompt, buf, len, cur);
			}
			continue;
		}
		if (c == 1) { cur = 0; clRedrawLine(prompt, buf, len, cur); continue; }
		if (c == 5) { cur = len; clRedrawLine(prompt, buf, len, cur); continue; }
		if (c == 11) { len = cur; buf[len] = '\0'; clRedrawLine(prompt, buf, len, cur); continue; }
		if (c == 21) {
			memmove(&buf[0], &buf[cur], len - cur);
			len -= cur; cur = 0; buf[len] = '\0';
			clRedrawLine(prompt, buf, len, cur); continue;
		}
		if (c == 23) {
			int i = cur;
			while (i > 0 && buf[i-1] == ' ') i--;
			while (i > 0 && buf[i-1] != ' ') i--;
			memmove(&buf[i], &buf[cur], len - cur);
			len -= (cur - i); cur = i; buf[len] = '\0';
			clRedrawLine(prompt, buf, len, cur); continue;
		}
		if (c == 9 && cl_menu_n > 0) {
			if (clMenuTake(buf, &len, &cur, (int)sizeof(buf), 0)) clRedrawLine(prompt, buf, len, cur);
			continue;
		}
		if (c == 9) {
			if (cur == len && cl_last_ghost[0]) {
				int gl = (int)strlen(cl_last_ghost);
				if (len + gl < (int)sizeof(buf)) {
					memcpy(buf + len, cl_last_ghost, gl);
					len += gl; cur = len; buf[len] = '\0';
					cl_last_ghost[0] = '\0';
					clRedrawLine(prompt, buf, len, cur);
				}
				continue;
			}
			if (cur != len) continue;
			{
				static char cand[CL_COMP_MAX][160];
				int wstart = 0, is_path = 0;
				int nc = clCompletions(buf, len, &wstart, &is_path, cand, CL_COMP_MAX);
				if (nc <= 0) continue;
				int common = clCompCommon(cand, nc);
				int wordlen = len - wstart;
				if (common > wordlen) {
					if (wstart + common < (int)sizeof(buf) - 2) {
						memcpy(buf + wstart, cand[0], (size_t)common);
						len = cur = wstart + common;
						buf[len] = '\0';
						if (nc == 1) {
							int isdir = len > 0 && buf[len - 1] == '/';
							if (!isdir && len + 1 < (int)sizeof(buf)) { buf[len++] = ' '; buf[len] = '\0'; cur = len; }
						}
					}
				} else if (nc > 1) {
					/* Into the flow, bracketed: printed straight from here it
					   lands on the bar's rows and every Tab stacks another. */
					clOutBegin();
					clCompList(cand, nc);
					clRedrawReset();
					clOutEnd(&G_AI);
				}
			}			clRedrawLine(prompt, buf, len, cur);
			continue;
		}
		if (c == 22) {                           /* ctrl-v: the clipboard's image, as [image #N] */
			char mark[32], err[160];
			int ml = hkClipImage(mark, sizeof mark, err, sizeof err) ? (int)strlen(mark) : 0;
			if (ml && len + ml + 1 < (int)sizeof(buf)) {
				memmove(&buf[cur + ml], &buf[cur], len - cur);
				memcpy(&buf[cur], mark, ml);
				len += ml; cur += ml; buf[len] = '\0';
			} else if (!ml) {
				clOutBegin();
				aiAddHistory(&G_AI, err);
				clRedrawReset();
				clOutEnd(&G_AI);
			}
			clRedrawLine(prompt, buf, len, cur);
			continue;
		}
		if (c == 7) {                            /* ctrl-g: keep writing in your editor */
			buf[len] = '\0';
			free(cl_preset_input);
			cl_preset_input = strdup(buf);       /* the editor opens on what is typed so far */
			clLineBreak();
			clDisableRaw();
			return -7;
		}
		if (c == 15) {                           /* ctrl-o: expand / collapse output */
			buf[len] = '\0';
			free(cl_preset_input);
			cl_preset_input = len > 0 ? strdup(buf) : NULL;   /* the typed line survives the pager */
			clLineBreak();
			clDisableRaw();
			return -3;
		}
		if (c == 12) {
			if (write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7) < 0) {}
			clRedrawReset();
			clRedrawLine(prompt, buf, len, cur);
			continue;
		}
		if (c == 18) {
			cl_menu_hidden = 1;
			char query[256] = {0};
			int qlen = 0;
			int match_idx = cl_in_hist_n - 1;
			char rprompt[1024];
			while (1) {
				const char *match = (match_idx >= 0 && match_idx < cl_in_hist_n) ? cl_in_hist[match_idx] : "";
				snprintf(rprompt, sizeof(rprompt), "(reverse-i-search)`%s': ", query);
				clRedrawLine(rprompt, match, (int)strlen(match), (int)strlen(match));
				char rc;
				if (read(STDIN_FILENO, &rc, 1) != 1) break;
				if (rc == '\r' || rc == '\n') {
					if (match && *match) {
						snprintf(buf, sizeof(buf), "%s", match);
						len = (int)strlen(buf); cur = len;
					}
					break;
				}
				if (rc == 27 || rc == 7) {
					break;
				}
				if (rc == 18) {
					int start = match_idx - 1;
					while (start >= 0) {
						if (qlen == 0 || strstr(cl_in_hist[start], query)) { match_idx = start; break; }
						start--;
					}
					continue;
				}
				if (rc == 127 || rc == 8) {
					if (qlen > 0) { query[--qlen] = '\0'; }
					match_idx = cl_in_hist_n - 1;
					if (qlen > 0) {
						while (match_idx >= 0 && !strstr(cl_in_hist[match_idx], query)) match_idx--;
					}
					continue;
				}
				if ((unsigned char)rc < 32) {
					break;
				}
				if (qlen + 1 < (int)sizeof(query)) {
					query[qlen++] = rc; query[qlen] = '\0';
					int start = match_idx;
					while (start >= 0 && !strstr(cl_in_hist[start], query)) start--;
					if (start >= 0) match_idx = start;
				}
			}
			clRedrawReset();
			clRedrawLine(prompt, buf, len, cur);
			continue;
		}
		if (c == 127 || c == 8) {
			cl_menu_hidden = 0;
			if (cur > 0) {
				int p = clU8Prev(buf, cur);
				memmove(&buf[p], &buf[cur], len - cur);
				len -= cur - p; cur = p; buf[len] = '\0';
				clRedrawLine(prompt, buf, len, cur);
			}
			continue;
		}

		if (c == '\x1b') {
			char s1, s2;
			/* A bare esc has nothing behind it. Read blocking, it swallowed the
			   next key the user typed. */
			if (!clReadByteTimeout(&s1, 50)) {
				if (cl_menu_n > 0) { cl_menu_hidden = 1; clRedrawLine(prompt, buf, len, cur); }   /* the menu only */
				else hkVoiceStop();
				continue;
			}
			if (s1 != '[' && s1 != 'O') continue;
			if (read(STDIN_FILENO, &s2, 1) != 1) continue;
			if (s2 == '<') {                 /* SGR mouse: \x1b[<b;col;rowM|m */
				char mb[32]; int mn = 0, mc;
				while (mn < (int)sizeof(mb) - 1) {
					char t;
					if (read(STDIN_FILENO, &t, 1) != 1) break;
					if (t == 'M' || t == 'm') { mc = t; mb[mn] = '\0'; goto got_mouse;
					}
					mb[mn++] = t;
				}
				continue;
			got_mouse:
				{
					int btn = 0, mx = 0, my = 0;
					if (mc == 'M' && sscanf(mb, "%d;%d;%d", &btn, &mx, &my) == 3
					    && (btn & 0x43) == 0) {          /* plain left press, no drag */
						(void)mx;
						if (hkClickRow(&G_AI, my)) clRedrawLine(prompt, buf, len, cur);
					}
				}
				continue;
			}
			if (s2 >= '0' && s2 <= '9') {
				char s3;
				if (read(STDIN_FILENO, &s3, 1) != 1) continue;
				if (s2 == '2' && s3 == '0') {
					char s4, s5;
					if (read(STDIN_FILENO, &s4, 1) != 1) continue;
					if (read(STDIN_FILENO, &s5, 1) != 1) continue;
					if (s4 == '0' && s5 == '~') { in_paste = 1; continue; }
					if (s4 == '1' && s5 == '~') { in_paste = 0; clRedrawLine(prompt, buf, len, cur); continue; }
					continue;
				}
				if (s2 == '3' && s3 == '~') {
					if (cur < len) {
						int n = clU8Next(buf, cur, len);
						memmove(&buf[cur], &buf[n], len - n);
						len -= n - cur; buf[len] = '\0';
						clRedrawLine(prompt, buf, len, cur);
					}
				} else if ((s2 == '1' || s2 == '7') && s3 == '~') {
					cur = 0; clRedrawLine(prompt, buf, len, cur);
				} else if ((s2 == '4' || s2 == '8') && s3 == '~') {
					cur = len; clRedrawLine(prompt, buf, len, cur);
				}
				continue;
			}
			if ((s2 == 'A' || s2 == 'B') && clMenuMove(s2 == 'A' ? -1 : 1)) {
				clRedrawLine(prompt, buf, len, cur);
				continue;
			}
			switch (s2) {
				case 'A':
					cl_menu_hidden = 1;              /* walking history: no menu over the old line */
					if (cl_in_hist_n == 0) break;
					if (hist_idx == cl_in_hist_n) {
						strncpy(saved, buf, sizeof(saved)-1); saved[sizeof(saved)-1] = '\0';
					}
					if (hist_idx > 0) {
						hist_idx--;
						snprintf(buf, sizeof(buf), "%s", cl_in_hist[hist_idx]);
						len = (int)strlen(buf); cur = len;
						clRedrawLine(prompt, buf, len, cur);
					}
					break;
				case 'B':
					cl_menu_hidden = 1;
					if (hist_idx < cl_in_hist_n) {
						hist_idx++;
						if (hist_idx == cl_in_hist_n) snprintf(buf, sizeof(buf), "%s", saved);
						else snprintf(buf, sizeof(buf), "%s", cl_in_hist[hist_idx]);
						len = (int)strlen(buf); cur = len;
						clRedrawLine(prompt, buf, len, cur);
					}
					break;
				case 'Z':                                /* shift-tab: cycle mode */
					clDisableRaw();
					return -4;
				case 'C': if (cur < len) { cur = clU8Next(buf, cur, len); clRedrawLine(prompt, buf, len, cur); } break;
				case 'D': if (cur > 0) { cur = clU8Prev(buf, cur); clRedrawLine(prompt, buf, len, cur); } break;
				case 'H': cur = 0; clRedrawLine(prompt, buf, len, cur); break;
				case 'F': cur = len; clRedrawLine(prompt, buf, len, cur); break;
			}
			continue;
		}

		if ((unsigned char)c >= 32) {
			if (len + 1 >= (int)sizeof(buf)) continue;
			memmove(&buf[cur+1], &buf[cur], len - cur);
			buf[cur] = c;
			cur++; len++; buf[len] = '\0';
			cl_menu_hidden = in_paste;              /* typing opens it again; a paste does not */
			clRedrawLine(prompt, buf, len, cur);
			continue;
		}
	}
}


#define HAKO_ANTHROPIC_CLIENT_ID  "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
#define HAKO_ANTHROPIC_REDIRECT   "https://console.anthropic.com/oauth/code/callback"
#define HAKO_ANTHROPIC_AUTH_URL   "https://claude.ai/oauth/authorize"
#define HAKO_ANTHROPIC_TOKEN_URL  "https://console.anthropic.com/v1/oauth/token"
#define HAKO_ANTHROPIC_SCOPE      "org:create_api_key user:profile user:inference"

#define HAKO_COPILOT_CLIENT_ID    "Iv1.b507a08c87ecfe98"
#define HAKO_COPILOT_DEVICE_URL   "https://github.com/login/device/code"
#define HAKO_COPILOT_TOKEN_URL    "https://github.com/login/oauth/access_token"
#define HAKO_COPILOT_EXCHANGE_URL "https://api.github.com/copilot_internal/v2/token"

#define HAKO_GHMODELS_CLIENT_ID   "178c6fc778ccc68e1d6a"
#define HAKO_GHMODELS_ENDPOINT    "https://models.inference.ai.azure.com"

#define HAKO_OR_AUTH_URL          "https://openrouter.ai/auth"
#define HAKO_OR_EXCHANGE_URL      "https://openrouter.ai/api/v1/auth/keys"

static const char *clOAuthClientId(const char *provider) {
	const char *k;
	if (!strcmp(provider, "anthropic")) {
		if ((k = getenv("HAKO_ANTHROPIC_CLIENT_ID")) && *k) return k;
		return HAKO_ANTHROPIC_CLIENT_ID;
	}
	if (!strcmp(provider, "github-copilot") || !strcmp(provider, "copilot")) {
		if ((k = getenv("HAKO_COPILOT_CLIENT_ID")) && *k) return k;
		return HAKO_COPILOT_CLIENT_ID;
	}
	return NULL;
}

static char *clCurlPost(const char *url, const char *json_body, const char *extra_headers) {
	hkHttpReq req = { .method = "POST", .url = url, .headers = extra_headers, .body = json_body };
	return hk_http_fetch(&req);
}

static char *clCurlGet(const char *url, const char *extra_headers) {
	hkHttpReq req = { .method = "GET", .url = url, .headers = extra_headers };
	char *buf = hk_http_fetch(&req);
	return buf;
}

static int clOAuthGithubCopilot(aiData *data) {
	const char *client_id = clOAuthClientId("github-copilot");
	if (!client_id) { aiAddHistory(data, "OAuth: no copilot client_id"); return -1; }

	char body[256];
	snprintf(body, sizeof(body), "{\"client_id\":\"%s\",\"scope\":\"read:user\"}", client_id);
	aiAddHistory(data, "asking GitHub for a sign-in code");   /* a slow network (iSh) is not silence */
	char *resp = clCurlPost(HAKO_COPILOT_DEVICE_URL, body, NULL);
	if (!resp) { aiAddHistory(data, "OAuth: device request failed"); return -1; }
	char *device_code = hkExtractJsonString(resp, "device_code");
	char *user_code = hkExtractJsonString(resp, "user_code");
	char *verify_uri = hkExtractJsonString(resp, "verification_uri");
	int interval = hkExtractJsonInt(resp, "interval");
	int expires_in = hkExtractJsonInt(resp, "expires_in");
	free(resp);
	if (!device_code || !user_code || !verify_uri) {
		aiAddHistory(data, "OAuth: malformed device response");
		free(device_code); free(user_code); free(verify_uri); return -1;
	}
	if (interval <= 0) interval = 5;
	if (expires_in <= 0) expires_in = 900;

	char line[512];
	snprintf(line, sizeof(line), "Visit: %s", verify_uri); aiAddHistory(data, line);
	snprintf(line, sizeof(line), "Code:  %s", user_code); aiAddHistory(data, line);
	if (hkClipCopy(user_code)) aiAddHistory(data, "the code is on your clipboard");
	aiAddHistory(data, "Polling... (Ctrl+C to abort)");
	clOpenUrl(verify_uri);

	long start = hk_time_unix();
	char *gh_token = NULL;
	const char *grant = "urn:ietf:params:oauth:grant-type:device_code";
	while (!E.interrupt && hk_time_unix() - start < expires_in) {
		sleep(interval);
		char poll[768];
		snprintf(poll, sizeof(poll),
			"{\"client_id\":\"%s\",\"device_code\":\"%s\",\"grant_type\":\"%s\"}",
			client_id, device_code, grant);
		char *tr = clCurlPost(HAKO_COPILOT_TOKEN_URL, poll, NULL);
		if (!tr) continue;
		gh_token = hkExtractJsonString(tr, "access_token");
		if (gh_token) { free(tr); break; }
		char *err = hkExtractJsonString(tr, "error");
		if (err && strcmp(err, "authorization_pending") != 0 && strcmp(err, "slow_down") != 0) {
			char *desc = hkExtractJsonString(tr, "error_description");
			aiSayf(data, "OAuth: %s%s%s", err, desc ? ": " : "", desc ? desc : "");
			free(err); free(desc); free(tr);
			free(device_code); free(user_code); free(verify_uri);
			return -1;
		}
		if (err && !strcmp(err, "slow_down")) interval += 5;
		free(err); free(tr);
	}
	free(device_code); free(user_code); free(verify_uri);
	if (!gh_token) { aiAddHistory(data, "OAuth: timed out / aborted"); return -1; }

	free(E.ai_oauth_refresh); E.ai_oauth_refresh = gh_token;
	free(E.ai_oauth_provider); E.ai_oauth_provider = strdup("github-copilot");
	hkApplyProviderAlias("github-copilot");
	if (clOAuthCopilotExchange(data) != 0) {
		aiAddHistory(data, "OAuth: GH token saved but Copilot exchange failed. Possibly no Copilot Pro on this GH account.");
		return -1;
	}
	aiAddHistory(data, "GitHub Copilot OAuth: signed in. Calls bill against your Copilot Pro subscription.");
	hkHealModelForProvider(data);
	return 0;
}

static int clOAuthGithubModels(aiData *data) {
	const char *client_id = HAKO_GHMODELS_CLIENT_ID;
	const char *k = getenv("HAKO_GHMODELS_CLIENT_ID");
	if (k && *k) client_id = k;

	char body[256];
	snprintf(body, sizeof(body), "{\"client_id\":\"%s\",\"scope\":\"read:user\"}", client_id);
	aiAddHistory(data, "asking GitHub for a sign-in code");   /* a slow network (iSh) is not silence */
	char *resp = clCurlPost(HAKO_COPILOT_DEVICE_URL, body, NULL);
	if (!resp) { aiAddHistory(data, "OAuth: device request failed"); return -1; }
	char *device_code = hkExtractJsonString(resp, "device_code");
	char *user_code = hkExtractJsonString(resp, "user_code");
	char *verify_uri = hkExtractJsonString(resp, "verification_uri");
	int interval = hkExtractJsonInt(resp, "interval");
	int expires_in = hkExtractJsonInt(resp, "expires_in");
	free(resp);
	if (!device_code || !user_code || !verify_uri) {
		aiAddHistory(data, "OAuth: malformed device response");
		free(device_code); free(user_code); free(verify_uri); return -1;
	}
	if (interval <= 0) interval = 5;
	if (expires_in <= 0) expires_in = 900;

	char line[512];
	snprintf(line, sizeof(line), "Visit: %s", verify_uri); aiAddHistory(data, line);
	snprintf(line, sizeof(line), "Code:  %s", user_code); aiAddHistory(data, line);
	if (hkClipCopy(user_code)) aiAddHistory(data, "the code is on your clipboard");
	aiAddHistory(data, "Polling... (Ctrl+C to abort)");
	clOpenUrl(verify_uri);

	long start = hk_time_unix();
	char *gh_token = NULL;
	const char *grant = "urn:ietf:params:oauth:grant-type:device_code";
	while (!E.interrupt && hk_time_unix() - start < expires_in) {
		sleep(interval);
		char poll[768];
		snprintf(poll, sizeof(poll),
			"{\"client_id\":\"%s\",\"device_code\":\"%s\",\"grant_type\":\"%s\"}",
			client_id, device_code, grant);
		char *tr = clCurlPost(HAKO_COPILOT_TOKEN_URL, poll, NULL);
		if (!tr) continue;
		gh_token = hkExtractJsonString(tr, "access_token");
		if (gh_token) { free(tr); break; }
		char *err = hkExtractJsonString(tr, "error");
		if (err && strcmp(err, "authorization_pending") != 0 && strcmp(err, "slow_down") != 0) {
			char *desc = hkExtractJsonString(tr, "error_description");
			aiSayf(data, "OAuth: %s%s%s", err, desc ? ": " : "", desc ? desc : "");
			free(err); free(desc); free(tr);
			free(device_code); free(user_code); free(verify_uri);
			return -1;
		}
		if (err && !strcmp(err, "slow_down")) interval += 5;
		free(err); free(tr);
	}
	free(device_code); free(user_code); free(verify_uri);
	if (!gh_token) { aiAddHistory(data, "OAuth: timed out / aborted"); return -1; }

	hkApplyProviderAlias("github-models");
	free(E.ai_endpoint); E.ai_endpoint = strdup(HAKO_GHMODELS_ENDPOINT);
	free(E.ai_api_key); E.ai_api_key = gh_token;
	free(E.ai_oauth_provider); E.ai_oauth_provider = strdup("github-models");
	free(E.ai_oauth_refresh); E.ai_oauth_refresh = NULL;
	E.ai_oauth_expires_at = 0;
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	aiAddHistory(data, "GitHub Models OAuth: signed in. Free tier, rate-limited by GitHub.");
	hkHealModelForProvider(data);
	aiAddHistory(data, "Try /model gpt-4o or /model meta-llama-3-70b-instruct");
	return 0;
}

static int clOAuthOpenRouter(aiData *data) {
#ifdef _WIN32
	(void)data; aiAddHistory(data, "OpenRouter OAuth: Windows loopback not wired (use :login openrouter-api for paste)"); return -1;
#else
	int port = 0;
	int srv = clOAuthLoopbackListen(&port);
	if (srv < 0) { aiAddHistory(data, "OAuth: cannot bind loopback port (1456-1499)"); return -1; }
	char *verifier = clOAuthRandomVerifier();
	if (!verifier) { close(srv); return -1; }
	char callback[64]; snprintf(callback, sizeof(callback), "http://127.0.0.1:%d", port);
	char cb_enc[128]; clUrlEncodeInto(callback, cb_enc, sizeof(cb_enc));
	char url[1024];
	snprintf(url, sizeof(url),
		"%s?callback_url=%s&code_challenge=%s&code_challenge_method=plain",
		HAKO_OR_AUTH_URL, cb_enc, verifier);
	aiSayf(data, "Opening browser. Waiting on %s ...", callback);
	clOpenUrl(url);
	char *code = clOAuthLoopbackWait(srv, 300);
	close(srv);
	if (!code) { aiAddHistory(data, "OAuth: timed out / no code returned"); free(verifier); return -1; }

	char body[2048];
	snprintf(body, sizeof(body),
		"{\"code\":\"%s\",\"code_verifier\":\"%s\",\"code_challenge_method\":\"plain\"}",
		code, verifier);
	free(code); free(verifier);
	char *resp = clCurlPost(HAKO_OR_EXCHANGE_URL, body, NULL);
	if (!resp) { aiAddHistory(data, "OAuth: exchange failed"); return -1; }
	char *key = hkExtractJsonString(resp, "key");
	free(resp);
	if (!key) { aiAddHistory(data, "OAuth: exchange returned no key"); return -1; }
	hkApplyProviderAlias("openrouter");
	free(E.ai_api_key); E.ai_api_key = key;
	free(E.ai_oauth_provider); E.ai_oauth_provider = NULL;
	free(E.ai_oauth_refresh); E.ai_oauth_refresh = NULL;
	E.ai_oauth_expires_at = 0;
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	aiAddHistory(data, "OpenRouter OAuth: key issued + saved.");
	hkHealModelForProvider(data);
	return 0;
#endif
}

/* A ChatGPT account over the Codex sign-in. OpenAI's device flow, not RFC 8628:
   the poll answers 403/404 until the user approves, then hands back a PKCE
   code + verifier that are exchanged like a normal authorization code. */
#define HAKO_CHATGPT_CLIENT_ID    "app_EMoamEEZ73f0CkXaXp7hrann"
#define HAKO_CHATGPT_ISSUER       "https://auth.openai.com"

static int hkChatGPTStore(const char *resp) {
	char *access = hkExtractJsonString(resp, "access_token");
	if (!access) return -1;
	char *refresh = hkExtractJsonString(resp, "refresh_token");
	free(E.ai_api_key); E.ai_api_key = access;
	if (refresh) { free(E.ai_oauth_refresh); E.ai_oauth_refresh = refresh; }
	free(E.ai_oauth_provider); E.ai_oauth_provider = strdup("chatgpt");
	char *pl = hkJwtPayload(access);
	int exp = pl ? hkExtractJsonInt(pl, "exp") : -1;
	int ttl = hkExtractJsonInt(resp, "expires_in");
	free(pl);
	E.ai_oauth_expires_at = exp > 0 ? exp - 120 : ttl > 0 ? hk_time_unix() + ttl - 120 : 0;
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	return 0;
}

static int clOAuthChatGPT(aiData *data) {
	aiAddHistory(data, "asking OpenAI for a sign-in code");
	char *resp = clCurlPost(HAKO_CHATGPT_ISSUER "/api/accounts/deviceauth/usercode",
	                        "{\"client_id\":\"" HAKO_CHATGPT_CLIENT_ID "\"}", NULL);
	if (!resp) { aiAddHistory(data, "ChatGPT: device request failed (network?)"); return -1; }
	char *dev = hkExtractJsonString(resp, "device_auth_id");
	char *code = hkExtractJsonString(resp, "user_code");
	if (!code) code = hkExtractJsonString(resp, "usercode");
	int interval = hkExtractJsonInt(resp, "interval");
	if (!dev || !code) {
		char *err = aiExtractApiError(resp);
		aiSayf(data, "ChatGPT: %s", err ? err : "unexpected device response");
		free(err); free(resp); free(dev); free(code);
		return -1;
	}
	free(resp);
	if (interval < 3) interval = 5;

	aiAddHistory(data, "Visit: " HAKO_CHATGPT_ISSUER "/codex/device");
	aiSayf(data, "Code:  %s", code);
	if (hkClipCopy(code)) aiAddHistory(data, "the code is on your clipboard");
	aiAddHistory(data, "Polling... (Ctrl+C to abort)");
	clOpenUrl(HAKO_CHATGPT_ISSUER "/codex/device");

	char poll[512];
	snprintf(poll, sizeof(poll), "{\"device_auth_id\":\"%s\",\"user_code\":\"%s\"}", dev, code);
	free(dev); free(code);
	long start = hk_time_unix();
	char *got = NULL;
	while (!E.interrupt && hk_time_unix() - start < 900) {
		sleep(interval);
		char *tr = clCurlPost(HAKO_CHATGPT_ISSUER "/api/accounts/deviceauth/token", poll, NULL);
		if (tr && strstr(tr, "\"authorization_code\"")) { got = tr; break; }
		free(tr);
	}
	if (!got) { aiAddHistory(data, "ChatGPT: timed out / aborted"); return -1; }
	char *ac = hkExtractJsonString(got, "authorization_code");
	char *cv = hkExtractJsonString(got, "code_verifier");
	free(got);
	if (!ac || !cv) {
		aiAddHistory(data, "ChatGPT: approval came back without a code");
		free(ac); free(cv); return -1;
	}

	char ac_enc[1024], cv_enc[256], form[2048];
	clUrlEncodeInto(ac, ac_enc, sizeof(ac_enc));
	clUrlEncodeInto(cv, cv_enc, sizeof(cv_enc));
	free(ac); free(cv);
	if (!hkFmtFits(form, sizeof(form),
		"grant_type=authorization_code&code=%s"
		"&redirect_uri=https%%3A%%2F%%2Fauth.openai.com%%2Fdeviceauth%%2Fcallback"
		"&client_id=" HAKO_CHATGPT_CLIENT_ID "&code_verifier=%s", ac_enc, cv_enc)) {
		aiAddHistory(data, "ChatGPT: authorization code too long");
		return -1;
	}
	hkHttpReq ex = { .method = "POST", .url = HAKO_CHATGPT_ISSUER "/oauth/token",
	                 .headers = "-H 'Content-Type: application/x-www-form-urlencoded'", .body = form };
	char *tok = hk_http_fetch(&ex);
	hkApplyProviderAlias("chatgpt");
	if (!tok || hkChatGPTStore(tok) != 0) {
		char *err = tok ? aiExtractApiError(tok) : NULL;
		aiSayf(data, "ChatGPT: token exchange failed%s%s", err ? ": " : "", err ? err : "");
		free(err); free(tok);
		return -1;
	}
	free(tok);
	aiAddHistory(data, "ChatGPT: signed in. Calls count against your ChatGPT plan's usage limits.");
	hkHealModelForProvider(data);
	return 0;
}

static int clOAuthChatGPTRefresh(aiData *data) {
	if (!E.ai_oauth_refresh) return -1;
	size_t cap = strlen(E.ai_oauth_refresh) + 192;
	char *body = malloc(cap);
	if (!body) return -1;
	snprintf(body, cap, "{\"client_id\":\"" HAKO_CHATGPT_CLIENT_ID "\",\"grant_type\":\"refresh_token\","
		"\"refresh_token\":\"%s\",\"scope\":\"openid profile email\"}", E.ai_oauth_refresh);
	char *tr = clCurlPost(HAKO_CHATGPT_ISSUER "/oauth/token", body, NULL);
	free(body);
	int rc = tr ? hkChatGPTStore(tr) : -1;
	free(tr);
	if (rc != 0 && data) aiAddHistory(data, "ChatGPT: sign-in expired; run :login chatgpt again");
	return rc;
}

/* Nous Portal: Hermes's public device-code client (as hako uses Codex's and Claude Code's); HAKO_NOUS_CLIENT_ID swaps it. */
#define HAKO_NOUS_PORTAL          "https://portal.nousresearch.com"

static const char *hkNousClientId(void) {
	const char *k = getenv("HAKO_NOUS_CLIENT_ID");
	return k && *k ? k : "hermes-cli";
}

static int hkNousStore(const char *resp) {
	char *access = hkExtractJsonString(resp, "access_token");
	if (!access) return -1;
	char *refresh = hkExtractJsonString(resp, "refresh_token");
	free(E.ai_api_key); E.ai_api_key = access;
	if (refresh) { free(E.ai_oauth_refresh); E.ai_oauth_refresh = refresh; }
	free(E.ai_oauth_provider); E.ai_oauth_provider = strdup("nous");
	char *pl = hkJwtPayload(access);
	int exp = pl ? hkExtractJsonInt(pl, "exp") : -1;
	int ttl = hkExtractJsonInt(resp, "expires_in");
	free(pl);
	E.ai_oauth_expires_at = exp > 0 ? exp - 120 : ttl > 0 ? hk_time_unix() + ttl - 120 : 0;
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	return 0;
}

static int clOAuthNous(aiData *data) {
	const char *cid = hkNousClientId();
	const char *form_hdr = "-H 'Content-Type: application/x-www-form-urlencoded'";
	char form[1024];
	snprintf(form, sizeof form, "client_id=%s&scope=inference%%3Ainvoke", cid);
	aiAddHistory(data, "asking Nous for a sign-in code");
	hkHttpReq dr = { .method = "POST", .url = HAKO_NOUS_PORTAL "/api/oauth/device/code",
	                 .headers = form_hdr, .body = form };
	char *resp = hk_http_fetch(&dr);
	if (!resp) { aiAddHistory(data, "Nous: device request failed (network?)"); return -1; }
	char *dev = hkExtractJsonString(resp, "device_code");
	char *code = hkExtractJsonString(resp, "user_code");
	char *uri = hkExtractJsonString(resp, "verification_uri_complete");
	if (!uri) uri = hkExtractJsonString(resp, "verification_uri");
	int interval = hkExtractJsonInt(resp, "interval");
	int expires_in = hkExtractJsonInt(resp, "expires_in");
	if (!dev || !code || !uri) {
		char *err = aiExtractApiError(resp);
		aiSayf(data, "Nous: %s", err ? err : "unexpected device response");
		free(err); free(resp); free(dev); free(code); free(uri);
		return -1;
	}
	free(resp);
	if (interval < 1) interval = 5;
	if (interval > 30) interval = 30;
	if (expires_in <= 0) expires_in = 900;

	aiSayf(data, "Visit: %s", uri);
	aiSayf(data, "Code:  %s", code);
	if (hkClipCopy(code)) aiAddHistory(data, "the code is on your clipboard");
	aiAddHistory(data, "Polling... (Ctrl+C to abort)");
	clOpenUrl(uri);
	free(uri); free(code);

	char dev_enc[512];
	clUrlEncodeInto(dev, dev_enc, sizeof dev_enc);
	free(dev);
	snprintf(form, sizeof form, "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code"
		"&client_id=%s&device_code=%s", cid, dev_enc);
	hkHttpReq tq = { .method = "POST", .url = HAKO_NOUS_PORTAL "/api/oauth/token",
	                 .headers = form_hdr, .body = form };
	long start = hk_time_unix();
	char *got = NULL;
	while (!E.interrupt && hk_time_unix() - start < expires_in) {
		sleep(interval);
		char *tr = hk_http_fetch(&tq);
		if (!tr) continue;
		if (strstr(tr, "\"access_token\"")) { got = tr; break; }
		char *err = hkExtractJsonString(tr, "error");
		if (err && strcmp(err, "authorization_pending") && strcmp(err, "slow_down")) {
			char *desc = hkExtractJsonString(tr, "error_description");
			aiSayf(data, "Nous: %s%s%s", err, desc ? ": " : "", desc ? desc : "");
			free(err); free(desc); free(tr);
			return -1;
		}
		if (err && !strcmp(err, "slow_down") && interval < 30) interval += 5;
		free(err); free(tr);
	}
	if (!got) { aiAddHistory(data, "Nous: timed out / aborted"); return -1; }
	hkApplyProviderAlias("nous");
	int rc = hkNousStore(got);
	free(got);
	if (rc != 0) { aiAddHistory(data, "Nous: approval came back without a token"); return -1; }
	aiAddHistory(data, "Nous Portal: signed in. Calls bill against your Nous account.");
	hkHealModelForProvider(data);
	return 0;
}

/* Nous refresh tokens are single-use and a reused one revokes the session, so take a newer one another hako saved first. */
static int clOAuthNousRefresh(aiData *data) {
	for (int i = 0; i < cl_creds_n; i++) { free(cl_creds[i].api_key); free(cl_creds[i].oauth_refresh); }
	cl_creds_n = 0; cl_creds_loaded = 0;
	clCredsLoad();
	clCred *c = clCredsFind("nous");
	if (c && c->oauth_refresh && c->api_key && c->oauth_expires_at > hk_time_unix()) {
		clCredsRestoreFor("nous");
		return 0;
	}
	if (!E.ai_oauth_refresh) return -1;
	char hdr[4096], form[256];
	if (!hkFmtFits(hdr, sizeof hdr, "-H 'Content-Type: application/x-www-form-urlencoded' "
		"-H 'x-nous-refresh-token: %s'", E.ai_oauth_refresh)) return -1;
	snprintf(form, sizeof form, "grant_type=refresh_token&client_id=%s", hkNousClientId());
	hkHttpReq rq = { .method = "POST", .url = HAKO_NOUS_PORTAL "/api/oauth/token", .headers = hdr, .body = form };
	char *tr = hk_http_fetch(&rq);
	int rc = tr ? hkNousStore(tr) : -1;
	free(tr);
	if (rc != 0 && data) aiAddHistory(data, "Nous: sign-in expired; run :login nous again");
	return rc;
}

static int clOAuthCopilotExchange(aiData *data) {
	if (!E.ai_oauth_refresh) return -1;
	char hdr[512];
	snprintf(hdr, sizeof(hdr),
		"-H 'Authorization: token %s' -H 'Editor-Version: %s' -H 'Editor-Plugin-Version: %s' -H 'User-Agent: GithubCopilot/%s'",
		E.ai_oauth_refresh, HAKO_COPILOT_EDITOR_VER, HAKO_COPILOT_PLUGIN_VER, HAKO_COPILOT_PLUGIN_VER);
	char *resp = clCurlGet(HAKO_COPILOT_EXCHANGE_URL, hdr);
	if (!resp) return -1;
	char *token = hkExtractJsonString(resp, "token");
	int expires_at = hkExtractJsonInt(resp, "expires_at");
	free(resp);
	if (!token) {
		if (data) aiAddHistory(data, "OAuth: Copilot token exchange returned no token");
		return -1;
	}
	free(E.ai_api_key); E.ai_api_key = token;
	E.ai_oauth_expires_at = expires_at > 0 ? (long)expires_at - 30 : hk_time_unix() + 1500;
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	return 0;
}

static void clUrlEncodeInto(const char *s, char *out, size_t cap) {
	static const char *hex = "0123456789ABCDEF";
	size_t j = 0;
	for (size_t i = 0; s[i] && j + 4 < cap; i++) {
		unsigned char c = (unsigned char)s[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
			|| c == '-' || c == '_' || c == '.' || c == '~') out[j++] = c;
		else { out[j++] = '%'; out[j++] = hex[c >> 4]; out[j++] = hex[c & 0xf]; }
	}
	out[j] = '\0';
}

static void hkSha256(const unsigned char *data, size_t len, unsigned char out[32]) {
	static const uint32_t K[64] = {
		0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
		0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
		0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
		0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
		0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
		0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
		0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
		0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
	uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
	                  0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
	size_t total = len + 1;
	while (total % 64 != 56) total++;
	size_t nblocks = (total + 8) / 64;
	for (size_t b = 0; b < nblocks; b++) {
		unsigned char blk[64];
		for (int i = 0; i < 64; i++) {
			size_t pos = b * 64 + (size_t)i;
			if (pos < len)              blk[i] = data[pos];
			else if (pos == len)        blk[i] = 0x80;
			else if (pos < total)       blk[i] = 0;
			else {
				unsigned long long bits = (unsigned long long)len * 8ULL;
				int shift = (int)(56 - 8 * (pos - total));
				blk[i] = (unsigned char)((bits >> shift) & 0xff);
			}
		}
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = ((uint32_t)blk[i*4] << 24) | ((uint32_t)blk[i*4+1] << 16)
			     | ((uint32_t)blk[i*4+2] << 8) | (uint32_t)blk[i*4+3];
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = (w[i-15] >> 7 | w[i-15] << 25) ^ (w[i-15] >> 18 | w[i-15] << 14) ^ (w[i-15] >> 3);
			uint32_t s1 = (w[i-2] >> 17 | w[i-2] << 15) ^ (w[i-2] >> 19 | w[i-2] << 13) ^ (w[i-2] >> 10);
			w[i] = w[i-16] + s0 + w[i-7] + s1;
		}
		uint32_t a=h[0],bb=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t S1 = (e >> 6 | e << 26) ^ (e >> 11 | e << 21) ^ (e >> 25 | e << 7);
			uint32_t ch = (e & f) ^ (~e & g);
			uint32_t t1 = hh + S1 + ch + K[i] + w[i];
			uint32_t S0 = (a >> 2 | a << 30) ^ (a >> 13 | a << 19) ^ (a >> 22 | a << 10);
			uint32_t maj = (a & bb) ^ (a & c) ^ (bb & c);
			uint32_t t2 = S0 + maj;
			hh=g; g=f; f=e; e=d+t1; d=c; c=bb; bb=a; a=t1+t2;
		}
		h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
	}
	for (int i = 0; i < 8; i++) {
		out[i*4]   = (unsigned char)(h[i] >> 24);
		out[i*4+1] = (unsigned char)(h[i] >> 16);
		out[i*4+2] = (unsigned char)(h[i] >> 8);
		out[i*4+3] = (unsigned char)(h[i]);
	}
}

/* PKCE S256. Was a pipe through openssl, which does not exist in a browser and
   is not guaranteed on Windows. */
static char *clSha256Base64Url(const char *in) {
	static const char *b64 =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	unsigned char h[32];
	hkSha256((const unsigned char *)in, strlen(in), h);
	char *out = malloc(45);
	if (!out) return NULL;
	int o = 0;
	for (int i = 0; i < 30; i += 3) {
		uint32_t v = ((uint32_t)h[i] << 16) | ((uint32_t)h[i+1] << 8) | h[i+2];
		out[o++] = b64[(v >> 18) & 63]; out[o++] = b64[(v >> 12) & 63];
		out[o++] = b64[(v >> 6) & 63];  out[o++] = b64[v & 63];
	}
	uint32_t v = ((uint32_t)h[30] << 16) | ((uint32_t)h[31] << 8);
	out[o++] = b64[(v >> 18) & 63];
	out[o++] = b64[(v >> 12) & 63];
	out[o++] = b64[(v >> 6) & 63];
	out[o] = '\0';
	return out;
}

/* The PKCE verifier, kept between the two halves of a paste-the-code sign-in.
   Front ends that cannot prompt inside a command send the code as a second one. */
static void hkOAuthPendingPath(char *out, size_t cap) {
	char dir[512];
	hkClawDirPath(dir, sizeof(dir));
	if (!dir[0]) { out[0] = '\0'; return; }
	snprintf(out, cap, "%s/oauth-pending", dir);
}

static void hkOAuthPendingSave(const char *verifier) {
	char path[600];
	hkOAuthPendingPath(path, sizeof(path));
	if (!path[0] || !verifier) return;
	hk_fs_write(path, verifier, strlen(verifier), 0);
}

static char *hkOAuthPendingTake(void) {
	char path[600];
	hkOAuthPendingPath(path, sizeof(path));
	if (!path[0]) return NULL;
	long len = 0;
	char *v = hk_fs_read(path, 4096, &len);
	if (v) {
		while (len > 0 && (v[len - 1] == '\n' || v[len - 1] == '\r')) v[--len] = '\0';
		hk_fs_remove(path);
	}
	return v;
}

static int clOAuthAnthropicExchange(aiData *data, char *paste, const char *verifier,
                                    const char *client_id);

static int clOAuthAnthropicFinish(aiData *data, const char *code) {
	const char *client_id = clOAuthClientId("anthropic");
	char *verifier = hkOAuthPendingTake();
	if (!client_id || !verifier) {
		free(verifier);
		aiAddHistory(data, "no sign-in waiting — run :login anthropic first");
		return -1;
	}
	char paste[512];
	snprintf(paste, sizeof(paste), "%s", code);
	int rc = clOAuthAnthropicExchange(data, paste, verifier, client_id);
	free(verifier);
	return rc;
}

static int clOAuthAnthropic(aiData *data) {
	const char *client_id = clOAuthClientId("anthropic");
	if (!client_id) { aiAddHistory(data, "OAuth: no anthropic client_id"); return -1; }

	char *verifier = clOAuthRandomVerifier();
	if (!verifier) { aiAddHistory(data, "OAuth: verifier gen failed"); return -1; }
	char *challenge = clSha256Base64Url(verifier);
	if (!challenge) {
		aiAddHistory(data, "OAuth: could not build the PKCE challenge (out of memory)");
		free(verifier); return -1;
	}

	char cid_enc[256], red_enc[256], scope_enc[256], chal_enc[128], ver_enc[64];
	clUrlEncodeInto(client_id, cid_enc, sizeof(cid_enc));
	clUrlEncodeInto(HAKO_ANTHROPIC_REDIRECT, red_enc, sizeof(red_enc));
	clUrlEncodeInto(HAKO_ANTHROPIC_SCOPE, scope_enc, sizeof(scope_enc));
	clUrlEncodeInto(challenge, chal_enc, sizeof(chal_enc));
	clUrlEncodeInto(verifier, ver_enc, sizeof(ver_enc));

	char url[2048];
	snprintf(url, sizeof(url),
		"%s?code=true&client_id=%s&response_type=code&redirect_uri=%s&scope=%s&code_challenge=%s&code_challenge_method=S256&state=%s",
		HAKO_ANTHROPIC_AUTH_URL, cid_enc, red_enc, scope_enc, chal_enc, ver_enc);

	free(challenge);

	aiAddHistory(data, "Sign in at this URL, then paste the code shown:");
	aiAddHistory(data, url);
	aiAddHistory(data, "(format: <code>#<state>  — copy the whole string)");
	if (hkClipCopy(url)) aiAddHistory(data, "the link is on your clipboard: paste it into a browser");
	clOpenUrl(url);

	printf("  paste code (input hidden%s): ", hkClipAvailable() ? ", enter alone uses the clipboard" : "");
	fflush(stdout);
	char paste[512];
	clReadHidden(paste, sizeof(paste));
	if (!paste[0]) {
		/* A front end that cannot prompt mid-command: keep the verifier so the
		   code can arrive as a command of its own. */
		hkOAuthPendingSave(verifier);
		aiAddHistory(data, "then run:  :login anthropic <code>");
		free(verifier);
		return -1;
	}

	int rc = clOAuthAnthropicExchange(data, paste, verifier, client_id);
	free(verifier);
	return rc;
}

static int clOAuthAnthropicExchange(aiData *data, char *paste, const char *verifier,
                                    const char *client_id) {
	char *hash = strchr(paste, '#');
	const char *code, *state;
	if (hash) { *hash = '\0'; code = paste; state = hash + 1; }
	else      { code = paste; state = verifier; }

	char body[3072];
	snprintf(body, sizeof(body),
		"{\"code\":\"%s\",\"state\":\"%s\",\"grant_type\":\"authorization_code\","
		"\"client_id\":\"%s\",\"redirect_uri\":\"%s\",\"code_verifier\":\"%s\"}",
		code, state, client_id, HAKO_ANTHROPIC_REDIRECT, verifier);

	hkHttpReq exch = { .method = "POST", .url = HAKO_ANTHROPIC_TOKEN_URL, .body = body };
	char *resp = hk_http_fetch(&exch);
	if (!resp) { aiAddHistory(data, "OAuth: token exchange failed"); return -1; }

	char *access = hkExtractJsonString(resp, "access_token");
	char *refresh = hkExtractJsonString(resp, "refresh_token");
	int expires = hkExtractJsonInt(resp, "expires_in");
	if (!access) {
		char *err = aiExtractApiError(resp);
		aiAddHistory(data, err ? err : "OAuth: exchange failed (no access_token)");
		size_t rl = strlen(resp);
		aiSayf(data, "raw: %.400s%s", resp, rl > 400 ? "..." : "");
		free(err); free(refresh); free(resp);
		return -1;
	}
	free(resp);

	free(E.ai_api_key); E.ai_api_key = access;
	free(E.ai_oauth_refresh); E.ai_oauth_refresh = refresh;
	free(E.ai_oauth_provider); E.ai_oauth_provider = strdup("anthropic");
	E.ai_oauth_expires_at = expires > 0 ? hk_time_unix() + expires - 30 : 0;
	if (!E.ai_model || !*E.ai_model) {
		free(E.ai_model); E.ai_model = strdup("claude-haiku-4-5");
	}
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	aiAddHistory(data, "Anthropic OAuth: signed in. Calls now bill against your Claude subscription.");
	hkHealModelForProvider(data);
	return 0;
}

/* A socket no child should inherit: a curl or shell left holding the listener keeps the port after hako quits. */
static int hkCloexec(int fd) {
#if defined(F_SETFD) && defined(FD_CLOEXEC)
	if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
	return fd;
}

static char *clOAuthRandomVerifier(void) {
	static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	char *out = malloc(48);
	if (!out) return NULL;
	if (!hkRandomToken(out, 43, alpha)) { free(out); return NULL; }
	return out;
}

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>

static int clOAuthLoopbackListen(int *out_port) {
#ifdef HAKO_WASM
	(void)out_port;
	return -1;
#else
	int fd = hkCloexec(socket(AF_INET, SOCK_STREAM, 0));
	if (fd < 0) return -1;
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	for (int p = 1456; p < 1500; p++) {
		addr.sin_port = htons(p);
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			if (listen(fd, 1) == 0) { *out_port = p; return fd; }
		}
	}
	close(fd);
	return -1;
#endif
}

static char *clOAuthLoopbackWait(int srv_fd, int timeout_sec) {
#ifdef HAKO_WASM
	(void)srv_fd; (void)timeout_sec;
	return NULL;
#else
	struct timeval tv; tv.tv_sec = timeout_sec; tv.tv_usec = 0;
	fd_set r; FD_ZERO(&r); FD_SET(srv_fd, &r);
	int n = select(srv_fd + 1, &r, NULL, NULL, &tv);
	if (n <= 0) return NULL;
	int c = hkCloexec(accept(srv_fd, NULL, NULL));
	if (c < 0) return NULL;
	char buf[4096];
	int got = (int)read(c, buf, sizeof(buf) - 1);
	if (got <= 0) { close(c); return NULL; }
	buf[got] = '\0';
	char *code = NULL;
	char *p = strstr(buf, "code=");
	if (p) {
		p += 5;
		char *e = p;
		while (*e && *e != '&' && *e != ' ' && *e != '\r' && *e != '\n') e++;
		int len = (int)(e - p);
		code = malloc(len + 1);
		memcpy(code, p, len);
		code[len] = '\0';
	}
	const char *resp =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/html\r\n"
		"Connection: close\r\n\r\n"
		"<!doctype html><meta charset=utf-8><title>hako-code</title>"
		"<style>body{background:#0a0a08;color:#e8dfc8;font-family:monospace;text-align:center;padding:4em}h1{color:#c9a961}</style>"
		"<h1>hako-code</h1><p>OAuth complete. Close this tab.</p>";
	if (write(c, resp, strlen(resp)) < 0) {}
	close(c);
	return code;
#endif
}
#endif

static int clOAuthRefresh(aiData *data) {
	if (!E.ai_oauth_provider) return -1;
	if (!strcmp(E.ai_oauth_provider, "github-copilot")) return clOAuthCopilotExchange(data);
	if (!strcmp(E.ai_oauth_provider, "chatgpt")) return clOAuthChatGPTRefresh(data);
	if (!strcmp(E.ai_oauth_provider, "nous")) return clOAuthNousRefresh(data);
	if (strcmp(E.ai_oauth_provider, "anthropic") != 0) return -1;
	if (!E.ai_oauth_refresh) return -1;
	const char *client_id = clOAuthClientId("anthropic");
	if (!client_id) return -1;
	char body[3072];
	snprintf(body, sizeof(body),
		"{\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\",\"client_id\":\"%s\"}",
		E.ai_oauth_refresh, client_id);
	hkHttpReq rfsh = { .method = "POST", .url = HAKO_ANTHROPIC_TOKEN_URL, .body = body };
	char *tr = hk_http_fetch(&rfsh);
	if (!tr) return -1;

	char *access = hkExtractJsonString(tr, "access_token");
	if (!access) {
		if (data) aiAddHistory(data, "OAuth: refresh failed; run :login anthropic again");
		free(tr); return -1;
	}
	int expires = hkExtractJsonInt(tr, "expires_in");
	char *new_refresh = hkExtractJsonString(tr, "refresh_token");
	free(E.ai_api_key); E.ai_api_key = access;
	if (new_refresh) { free(E.ai_oauth_refresh); E.ai_oauth_refresh = new_refresh; }
	E.ai_oauth_expires_at = expires > 0 ? hk_time_unix() + expires - 30 : 0;
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	free(tr);
	return 0;
}

static void clOAuthEnsureFresh(aiData *data) {
	if (!E.ai_oauth_provider || !E.ai_oauth_refresh) return;
	if (E.ai_oauth_expires_at == 0) return;
	if (hk_time_unix() < E.ai_oauth_expires_at) return;
	clOAuthRefresh(data);
}

static int clOwnPath(char *out, size_t cap) {
#ifdef __APPLE__
	uint32_t sz = (uint32_t)cap;
	char tmp[PATH_MAX];
	if (_NSGetExecutablePath(tmp, &sz) != 0) return -1;
	if (!realpath(tmp, out)) snprintf(out, cap, "%s", tmp);
	return 0;
#elif defined(__linux__)
	ssize_t n = readlink("/proc/self/exe", out, cap - 1);
	if (n <= 0) return -1;
	out[n] = '\0';
	return 0;
#elif defined(__FreeBSD__)
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
	size_t sz = cap;
	if (sysctl(mib, 4, out, &sz, NULL, 0) != 0) return -1;
	return 0;
#elif defined(_WIN32)
	DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
	return (n == 0 || n == cap) ? -1 : 0;
#else
	(void)out; (void)cap;
	return -1;
#endif
}

static const char *clPlatformAsset(void) {
#if defined(__APPLE__)
	return "hako-code-macos-universal.tar.gz";
#elif defined(__linux__)
  #if defined(__aarch64__) || defined(__arm64__)
	return "hako-code-linux-arm64.tar.gz";
  #else
	return "hako-code-linux-x86_64.tar.gz";
  #endif
#elif defined(__FreeBSD__)
	return "hako-code-freebsd-x86_64.tar.gz";
#elif defined(_WIN32)
	return "hako-code-windows-x86_64.zip";
#else
	return NULL;
#endif
}

static const char *clPlatformDir(void) {
#if defined(__APPLE__)
	return "hako-code-macos-universal";
#elif defined(__linux__)
  #if defined(__aarch64__) || defined(__arm64__)
	return "hako-code-linux-arm64";
  #else
	return "hako-code-linux-x86_64";
  #endif
#elif defined(__FreeBSD__)
	return "hako-code-freebsd-x86_64";
#elif defined(_WIN32)
	return "hako-code-windows-x86_64";
#else
	return NULL;
#endif
}

static int clCmdUpdate(int force) {
#ifdef HAKO_WASM
	(void)force;
	fprintf(stderr, "self-update is not available in this build\n");
	return 1;
#else
	const char *asset = clPlatformAsset();
	const char *pdir  = clPlatformDir();
	if (!asset || !pdir) {
		fprintf(stderr, "update: unsupported platform\n");
		return 1;
	}

	char tag[64] = {0};
	{
		char cmd[512];
		snprintf(cmd, sizeof(cmd),
			"curl -fsSL https://api.github.com/repos/%s/releases/latest 2>/dev/null", HAKO_REPO);
		FILE *fp = popen(cmd, "r");
		if (!fp) { fprintf(stderr, "update: curl spawn failed\n"); return 1; }
		char json[16384]; size_t n = fread(json, 1, sizeof(json)-1, fp); json[n] = '\0';
		pclose(fp);
		char *p = strstr(json, "\"tag_name\":\"");
		if (!p) {
			fprintf(stderr, "update: could not fetch latest release (network? rate limit?)\n");
			return 1;
		}
		p += 12;
		char *e = strchr(p, '"');
		if (!e) { fprintf(stderr, "update: malformed release json\n"); return 1; }
		int tlen = (int)(e - p);
		if (tlen >= (int)sizeof(tag)) tlen = sizeof(tag) - 1;
		memcpy(tag, p, tlen); tag[tlen] = '\0';
	}

	char cur_tag[64];
	snprintf(cur_tag, sizeof(cur_tag), "v%s", HAKO_VERSION);
	printf("latest: %s · current: %s\n", tag, cur_tag);
	if (!force && strcmp(tag, cur_tag) == 0) {
		printf("already up to date.\n");
		return 0;
	}

	char tmp_dir[256];
	snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/hako-update-%ld", hk_time_unix());
#ifdef _WIN32
	_mkdir(tmp_dir);
#else
	mkdir(tmp_dir, 0700);
#endif

	char tmp_archive[512];
	snprintf(tmp_archive, sizeof(tmp_archive), "%s/%s", tmp_dir, asset);

	char sha_name[128];
	snprintf(sha_name, sizeof(sha_name), "%s", asset);
	char *dot = strstr(sha_name, ".tar.gz"); if (!dot) dot = strstr(sha_name, ".zip");
	if (dot) *dot = '\0';
	strncat(sha_name, ".sha256", sizeof(sha_name) - strlen(sha_name) - 1);

	char tmp_sha[512];
	snprintf(tmp_sha, sizeof(tmp_sha), "%s/%s", tmp_dir, sha_name);

	{
		char cmd[1024];
		printf("downloading %s ...\n", asset);
		snprintf(cmd, sizeof(cmd),
			"curl -fsSL -o '%s' 'https://github.com/%s/releases/download/%s/%s'",
			tmp_archive, HAKO_REPO, tag, asset);
		if (system(cmd) != 0) { fprintf(stderr, "update: download failed\n"); return 1; }

		snprintf(cmd, sizeof(cmd),
			"curl -fsSL -o '%s' 'https://github.com/%s/releases/download/%s/%s'",
			tmp_sha, HAKO_REPO, tag, sha_name);
		int sha_ok = (system(cmd) == 0);

		if (sha_ok) {
			char vcmd[1024];
#ifdef __APPLE__
			snprintf(vcmd, sizeof(vcmd),
				"cd '%s' && tar xzf '%s' && cd '%s' && shasum -a 256 -c '../%s' >/dev/null 2>&1",
				tmp_dir, asset, pdir, sha_name);
#else
			snprintf(vcmd, sizeof(vcmd),
				"cd '%s' && tar xzf '%s' && cd '%s' && sha256sum -c '../%s' >/dev/null 2>&1",
				tmp_dir, asset, pdir, sha_name);
#endif
			if (system(vcmd) != 0) {
				fprintf(stderr, "update: sha256 verify FAILED — aborting\n");
				return 1;
			}
			printf("sha256 ok.\n");
		} else {
			fprintf(stderr, "update: sha sidecar missing — aborting (use --force-no-verify to override)\n");
			return 1;
		}
	}

	char new_bin[512];
#ifdef _WIN32
	snprintf(new_bin, sizeof(new_bin), "%s/%s/hako.exe", tmp_dir, pdir);
#else
	snprintf(new_bin, sizeof(new_bin), "%s/%s/hako", tmp_dir, pdir);
#endif

	char self[PATH_MAX];
	if (clOwnPath(self, sizeof(self)) != 0) {
		fprintf(stderr, "update: can't resolve own path; new binary at %s\n", new_bin);
		return 1;
	}

	if (rename(new_bin, self) != 0) {
		fprintf(stderr, "update: atomic replace failed (%s)\n", strerror(errno));
		fprintf(stderr, "  new binary: %s\n", new_bin);
		fprintf(stderr, "  target:     %s\n", self);
		fprintf(stderr, "  copy manually with sudo if cross-device or permission denied.\n");
		return 1;
	}
#ifndef _WIN32
	chmod(self, 0755);
#endif
	printf("updated: %s → %s\n", cur_tag, tag);

	char rmcmd[512];
#ifdef _WIN32
	snprintf(rmcmd, sizeof(rmcmd), "rmdir /s /q \"%s\"", tmp_dir);
#else
	snprintf(rmcmd, sizeof(rmcmd), "rm -rf '%s'", tmp_dir);
#endif
	if (system(rmcmd)) {}
	return 0;
#endif
}


/* Decode one UTF-8 sequence; returns bytes consumed and writes the codepoint. */
static int clUtf8Next(const unsigned char *p, unsigned int *cp) {
	if (*p < 0x80)          { *cp = *p; return 1; }
	if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
		*cp = ((unsigned)(*p & 0x1f) << 6) | (p[1] & 0x3f); return 2;
	}
	if ((*p & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
		*cp = ((unsigned)(*p & 0x0f) << 12) | ((unsigned)(p[1] & 0x3f) << 6) | (p[2] & 0x3f); return 3;
	}
	if ((*p & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
		*cp = ((unsigned)(*p & 0x07) << 18) | ((unsigned)(p[1] & 0x3f) << 12)
		    | ((unsigned)(p[2] & 0x3f) << 6) | (p[3] & 0x3f); return 4;
	}
	*cp = *p; return 1;   /* invalid byte: treat as one narrow cell */
}

/* Terminal cells a codepoint occupies. */
/* Cells taken by the first n BYTES of s (no escapes). The input field keeps a
   byte cursor; the screen counts cells, and é/emoji differ. */
static int clCellsN(const char *s, int n) {
	int w = 0, i = 0;
	while (i < n && s[i]) {
		unsigned int cp;
		int adv = clUtf8Next((const unsigned char *)s + i, &cp);
		if (adv < 1) adv = 1;
		if (i + adv > n) break;
		w += clRuneWidth(cp);
		i += adv;
	}
	return w;
}

static int clRuneWidth(unsigned int cp) {
	if (cp == 0) return 0;
	if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 0;      /* control */
	if ((cp >= 0x0300 && cp <= 0x036f) ||                       /* combining */
	    (cp >= 0x200b && cp <= 0x200f) || cp == 0xfeff) return 0;
	if ((cp >= 0x1100 && cp <= 0x115f) ||                       /* Hangul Jamo */
	    (cp >= 0x2e80 && cp <= 0x303e) ||                       /* CJK radicals/punct */
	    (cp >= 0x3041 && cp <= 0x33ff) ||                       /* kana, CJK compat */
	    (cp >= 0x3400 && cp <= 0x4dbf) ||                       /* CJK ext A */
	    (cp >= 0x4e00 && cp <= 0x9fff) ||                       /* CJK unified */
	    (cp >= 0xa000 && cp <= 0xa4cf) ||                       /* Yi */
	    (cp >= 0xac00 && cp <= 0xd7a3) ||                       /* Hangul syllables */
	    (cp >= 0xf900 && cp <= 0xfaff) ||                       /* CJK compat ideographs */
	    (cp >= 0xfe30 && cp <= 0xfe6f) ||                       /* CJK compat forms */
	    (cp >= 0xff00 && cp <= 0xff60) ||                       /* fullwidth forms */
	    (cp >= 0xffe0 && cp <= 0xffe6) ||
	    (cp >= 0x1f300 && cp <= 0x1f9ff) ||                     /* emoji */
	    (cp >= 0x20000 && cp <= 0x3fffd)) return 2;
	return 1;
}

static int clCellWidth(const char *s) {
	int n = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		if (*p == 0x1b && p[1] == '[') {
			p += 2;
			while (*p && *p != 'm') p++;
			if (*p) p++;
			continue;
		}
		unsigned int cp;
		p += clUtf8Next(p, &cp);
		n += clRuneWidth(cp);
	}
	return n;
}

static void clClipToCells(char *buf, int cells) {
	if (cells < 0) cells = 0;
	int n = 0;
	unsigned char *q = (unsigned char *)buf;
	while (*q) {
		if (*q == 0x1b && q[1] == '[') {          /* escapes cost no cells */
			unsigned char *e = q + 2;
			while (*e && *e != 'm') e++;
			if (*e) e++;
			q = e;
			continue;
		}
		unsigned int cp;
		int adv = clUtf8Next(q, &cp);
		int w = clRuneWidth(cp);
		if (n + w > cells) { *q = '\0'; return; }
		n += w; q += adv;
	}
}

static const char *clStrCaseStr(const char *hay, const char *needle) {
	if (!needle || !*needle) return hay;
	for (; *hay; hay++) {
		const char *h = hay, *n = needle;
		while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
		if (!*n) return hay;
	}
	return NULL;
}

static int clReadByteTimeout(char *c, int ms) {
#ifndef _WIN32
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
	if (r <= 0) return 0;
#else
	(void)ms;
#endif
	return read(STDIN_FILENO, c, 1) == 1;
}

#define CL_POPUP_VIS 18
#define CL_POPUP_TEXT_MAX  40
static int clPopupSelect(const char *title, const char **items, int n,
                         int cur, clPopupPreview preview) {
	return clPopupSelectLive(title, items, n, cur, preview, NULL);
}

static int clPopupSelectLive(const char *title, const char **items, int n,
                             int cur, clPopupPreview preview, clPopupOnMove on_move) {
	if (n <= 0) return -1;
	if (cur < 0 || cur >= n) cur = 0;

	/* Rei pane / --serve: a structured choice the front end renders, not a
	   list printed into the transcript that then blocks on a number. */
	if (E.pipe_mode || E.serve_mode)
		return hkPipeChoice(title, items, n);

	int no_stdin = !isatty(STDIN_FILENO);
	int no_color = !E.color_enabled;
	int no_raw   = (no_stdin || no_color) ? 0 : (clEnableRaw() != 0);
	if (no_stdin || no_color || no_raw) {
		if (E.debug)
			fprintf(stderr, "[popup] text fallback: stdin_tty=%d stdout_tty=%d raw=%s\n",
				!no_stdin, isatty(STDOUT_FILENO), no_raw ? "failed" : "not tried");
		printf("%s:\n", title ? title : "select");
		char pv[128];
		int shown = n > CL_POPUP_TEXT_MAX ? CL_POPUP_TEXT_MAX : n;
		for (int i = 0; i < shown; i++) {
			if (preview) { preview(i, pv, sizeof pv); printf("  %d) %-14s %s\n", i + 1, items[i], pv); }
			else printf("  %d) %s\n", i + 1, items[i]);
		}
		if (n > shown) printf("  … %d more\n", n - shown);
		char line[32];
		printf("  pick [1-%d]: ", shown); fflush(stdout);
		if (!fgets(line, sizeof line, stdin)) return -1;
		int pick = atoi(line);
		return (pick >= 1 && pick <= shown) ? pick - 1 : -1;
	}

	int had_bar = cl_bar_h != 0;
	if (had_bar) { cl_field_rows = 1; clBarHide(); }
	int want_bar = (had_bar || cl_bar_want) && clBarUsable();
#if HK_ATTACH
	/* :remote on: the phone gets the same menu; an approval arms its own. */
	int phone_too = hs_attached && !hs_answer_armed && hkEmitChoiceRequest(title, items, n);
	if (phone_too) hs_answer_armed = 1;
#endif

	int *vis = malloc(sizeof(int) * (size_t)n);
	if (!vis) { if (had_bar) clBarShow(&G_AI); return -1; }
	char flt[64] = "";
	int flen = 0, filtering = 0;
	int nv = n, vpos = cur, top = 0;
	for (int i = 0; i < n; i++) vis[i] = i;

	int term_cols, term_rows;
	char pv[128];
	int inner_nat = clCellWidth(title ? title : "") + 4;   /* "╭─ title ─╮" needs the two joints */
	for (int i = 0; i < n; i++) {
		int w = 4 + clCellWidth(items[i]);
		if (preview) { preview(i, pv, sizeof pv); w += 2 + clCellWidth(pv); }
		if (w > inner_nat) inner_nat = w;
	}

	/* The editor's picker look: top rule, title, divider, rows, bottom rule. */
	enum { CHROME = 4 };
	int inner, maxvis, compact, prev_rows = 0;
relayout:
	term_cols = clTermCols(); term_rows = clTermRows();
	inner = inner_nat;
	if (inner > 64) inner = 64;
	if (inner > term_cols - 4) inner = term_cols - 4;
	if (inner < 12) inner = 12;
	/* Box, hint and the bar under it must all fit on the screen. A box taller
	   than the screen scrolled its own top away, every redraw then climbed
	   short, and the boxes stacked down the window (the theme picker on a
	   small screen, endlessly on iSh). So the list scrolls inside a box that
	   fits, the bar steps aside when it would not, and with no room for a box
	   at all the choice is one line. */
	maxvis = term_rows - CHROME - 1 - (want_bar ? 7 : 0) - 1;
	if (maxvis < 3 && want_bar) { want_bar = 0; maxvis = term_rows - CHROME - 2; }
	if (maxvis > CL_POPUP_VIS) maxvis = CL_POPUP_VIS;
	compact = maxvis < 1 || term_cols < 16;
	if (compact) want_bar = 0;

	if (!compact) {
		int reserve = (n < maxvis ? n : maxvis) + CHROME + 1;
		for (int i = 0; i < reserve; i++) { if (write(STDOUT_FILENO, "\n", 1) < 0) {} }
		char up[16];
		int k = snprintf(up, sizeof up, "\x1b[%dA\r", reserve);
		if (write(STDOUT_FILENO, up, k) < 0) {}
	}

	int result = -1, refilter = 0;
	int last_fired = -1;             /* the row on_move last saw */
	long sig = -1;                   /* what the list looked like when last drawn */
	for (;;) {
		if (refilter) {
			int keep = (nv > 0 && vpos < nv) ? vis[vpos] : -1;
			nv = 0;
			for (int i = 0; i < n; i++) {
				if (!flen || clStrCaseStr(items[i], flt)) vis[nv++] = i;
			}
			vpos = 0;
			for (int i = 0; i < nv; i++) if (vis[i] == keep) { vpos = i; break; }
			top = 0;
			refilter = 0;
		}
		int rows = nv < maxvis ? nv : maxvis;
		if (rows < 1) rows = 1;
		if (compact) rows = 1;
		if (vpos < top) top = vpos;
		if (vpos >= top + rows) top = vpos - rows + 1;
		if (top < 0) top = 0;

		if (on_move && nv > 0 && vis[vpos] != last_fired) {
			last_fired = vis[vpos];
			on_move(last_fired);
			sig = -1;                       /* colours changed: repaint */
		}

		/* Nothing moved and nothing filtered: do not repaint. A keypress at
		   the end of the list should be visibly inert, not a flicker. */
		long now_sig = ((long)vpos * 1000003L) ^ ((long)top * 1009L) ^ ((long)nv * 17L)
		             ^ ((long)flen * 3L) ^ (long)filtering;
		if (sig == now_sig) goto read_key;
		sig = now_sig;

		if (compact) {                      /* one line: "Theme: < nord > 3/19" */
			char one[320], it[160];
			snprintf(it, sizeof it, "%s", nv > 0 ? items[vis[vpos]] : "(no match)");
			snprintf(one, sizeof one, "  %s%s%s: %s< %s >%s %d/%d%s%s", E.color_enabled ? TH_ACCENT : "",
			         title ? title : "select", ANSI_RESET, E.color_enabled ? ANSI_BOLD : "", it, ANSI_RESET,
			         nv > 0 ? vpos + 1 : 0, nv, flen ? " /" : "", flen ? flt : "");
			if (clCellWidth(one) > term_cols - 1) clClipToCells(one, term_cols - 1);
			printf("\r\x1b[2K%s%s", one, ANSI_RESET);
			fflush(stdout);
			goto read_key;
		}

		if (prev_rows) {
			char up[16]; int k = snprintf(up, sizeof up, "\r\x1b[%dA", prev_rows + CHROME);
			if (write(STDOUT_FILENO, up, k) < 0) {}
			if (prev_rows > rows) {
				for (int i = 0; i < prev_rows + CHROME + 1; i++) { if (write(STDOUT_FILENO, "\x1b[2K\n", 5) < 0) {} }
				char back[16]; int b = snprintf(back, sizeof back, "\x1b[%dA\r", prev_rows + CHROME + 1);
				if (write(STDOUT_FILENO, back, b) < 0) {}
			}
		}

		char head[96];
		if (nv != n) snprintf(head, sizeof head, "%s (%d/%d)", title ? title : "", nv, n);
		else snprintf(head, sizeof head, "%s", title ? title : "");
		if (E.color_enabled) fputs(TH_ACCENT, stdout);
		fputs("\r  ╭", stdout);
		for (int i = 0; i < inner; i++) fputs("─", stdout);
		fputs("╮\x1b[K\n\r  │ ", stdout);
		if (E.color_enabled) fputs(ANSI_BOLD, stdout);
		{
			char hc[96]; snprintf(hc, sizeof hc, "%s", head);
			if (hc[0] >= 'a' && hc[0] <= 'z') hc[0] = (char)(hc[0] - 32);   /* "Theme", as the editor titles it */
			clClipToCells(hc, inner - 2);
			fputs(hc, stdout);
			if (E.color_enabled) { fputs(ANSI_RESET, stdout); fputs(TH_ACCENT, stdout); }
			for (int i = clCellWidth(hc) + 1; i < inner; i++) putchar(' ');
		}
		fputs("│\x1b[K\n\r  ├", stdout);
		for (int i = 0; i < inner; i++) fputs("─", stdout);
		fputs("┤\x1b[K\n", stdout);

		if (nv == 0) {
			if (E.color_enabled) fputs(TH_ACCENT, stdout);
			fputs("\r  │", stdout);
			if (E.color_enabled) fputs(TH_META, stdout);
			fputs("   (no match)", stdout);
			if (E.color_enabled) fputs(ANSI_RESET, stdout);
			for (int p = 13; p < inner; p++) putchar(' ');
			if (E.color_enabled) fputs(TH_ACCENT, stdout);
			fputs("│\x1b[K\n", stdout);
		}
		for (int r = 0; r < rows && nv > 0; r++) {
			int vi = top + r;
			if (vi >= nv) break;
			int i = vis[vi];
			int sel = (vi == vpos);
			if (E.color_enabled) fputs(TH_ACCENT, stdout);
			fputs("\r  │", stdout);
			if (E.color_enabled) fputs(sel ? TH_ACCENT : ANSI_RESET, stdout);
			fputs(sel ? "▸ " : "  ", stdout);
			if (E.color_enabled) fputs(sel ? ANSI_BOLD : TH_AI, stdout);
			int room = inner - 2 - (preview ? 0 : 1);
			int ilen = clCellWidth(items[i]);
			if (ilen > room) {
				char clip[192];
				snprintf(clip, sizeof clip, "%s", items[i]);
				clClipToCells(clip, room - 1);
				printf("%s…", clip);
				ilen = room;
			} else fputs(items[i], stdout);
			if (E.color_enabled) fputs(ANSI_RESET, stdout);
			int used = 2 + ilen;
			if (preview) {
				preview(i, pv, sizeof pv);
				int gap = inner - used - clCellWidth(pv) - 2;
				for (int g = 0; g < gap; g++) putchar(' ');
				putchar(' ');
				fputs(pv, stdout);
				if (E.color_enabled) fputs(ANSI_RESET, stdout);
				used = inner - 1;
			}
			for (int p = used; p < inner; p++) putchar(' ');
			if (E.color_enabled) fputs(TH_ACCENT, stdout);
			fputs("│\x1b[K\n", stdout);
		}

		if (E.color_enabled) fputs(TH_ACCENT, stdout);
		fputs("\r  ╰", stdout);
		{
			/* where you are, when the list scrolls: "─ 3/18 ─" like the editor */
			char pos[48] = "";
			if (nv > rows) snprintf(pos, sizeof pos, "─ %d/%d ", vpos + 1, nv);
			int pw = clCellWidth(pos);
			if (pw && pw < inner) { fputs(pos, stdout); for (int i = pw; i < inner; i++) fputs("─", stdout); }
			else for (int i = 0; i < inner; i++) fputs("─", stdout);
		}
		fputs("╯\x1b[K\n", stdout);
		if (E.color_enabled) fputs(TH_META, stdout);
		{
			char hint[128];
			if (filtering)       snprintf(hint, sizeof hint, "  /%s", flt);
			else if (flen)       snprintf(hint, sizeof hint, "  /%s · ↑/↓ · enter select · esc clear", flt);
			else                 snprintf(hint, sizeof hint, "  ↑/↓ move · / filter · enter select · esc cancel");
			if (clCellWidth(hint) > term_cols - 1) {
				if (filtering)   snprintf(hint, sizeof hint, "  /%s", flt);
				else if (flen)   snprintf(hint, sizeof hint, "  /%s · esc clear", flt);
				else             snprintf(hint, sizeof hint, "  ↑/↓ · / filter · ⏎ · esc");
			}
			if (clCellWidth(hint) > term_cols - 1) clClipToCells(hint, term_cols - 1);
			printf("\r%s\x1b[K", hint);
		}
		if (E.color_enabled) fputs(ANSI_RESET, stdout);
		fflush(stdout);
		prev_rows = rows;

	read_key:
		if (want_bar) { putchar('\n'); fflush(stdout); clBarShow(&G_AI); }
		char c;
		ssize_t got;
#if HK_ATTACH
		if (hs_answer_armed && hs_wake[0] >= 0 && hkWaitKeyOrWake()) got = -3; else
#endif
		got = read(STDIN_FILENO, &c, 1);
		if (want_bar) { clBarHide(); fputs("\x1b[A", stdout); fflush(stdout); }   /* back onto the hint row; flushed, the teardown below uses write() */
		if (got == -3) { result = -3; break; }      /* answered on the phone */
		if (got < 0 && errno == EINTR && cl_winch) {  /* a resize (iSh: the keyboard) re-lays it out, never closes it */
			cl_winch = 0;
			if (compact) printf("\r\x1b[2K");
			else if (prev_rows) printf("\r\x1b[%dA\x1b[J", prev_rows + CHROME);
			fflush(stdout);
			prev_rows = 0;
			sig = -1;
			want_bar = (had_bar || cl_bar_want) && clBarUsable();
			goto relayout;
		}
		if (got != 1) { result = -1; break; }
		if (c == 3) { result = -1; break; }
		if (c == '\r' || c == '\n') {
			if (filtering) { filtering = 0; continue; }
			result = (nv > 0) ? vis[vpos] : -1;
			break;
		}
		if (c == '\x1b') {
			char s1;
			if (!clReadByteTimeout(&s1, 40) || (s1 != '[' && s1 != 'O')) {
				if (filtering || flen) { flt[0] = '\0'; flen = 0; filtering = 0; refilter = 1; continue; }
				result = -1; break;
			}
			char s2;
			if (!clReadByteTimeout(&s2, 40)) { result = -1; break; }
			if (s2 == 'A') { if (vpos > 0) vpos--; }
			else if (s2 == 'B') { if (vpos < nv - 1) vpos++; }
			continue;
		}
		if (filtering) {
			if (c == 127 || c == 8) {
				if (flen > 0) flt[--flen] = '\0'; else filtering = 0;
				refilter = 1;
			} else if ((unsigned char)c >= 32 && flen < (int)sizeof(flt) - 1) {
				flt[flen++] = c; flt[flen] = '\0';
				refilter = 1;
			}
			continue;
		}
		if (c == '/') { filtering = 1; continue; }
		if (c == 'q') { result = -1; break; }
		if (c == 'k') { if (vpos > 0) vpos--; continue; }
		if (c == 'j') { if (vpos < nv - 1) vpos++; continue; }
		/* Habits from the old prompts still work: a digit picks that row, and
		   a letter that begins exactly one visible item picks it (y / n). */
		if (c >= '1' && c <= '9' && c - '1' < nv) { result = vis[c - '1']; break; }
		if ((unsigned char)c >= 32) {
			int hit = -1, hits = 0;
			for (int i = 0; i < nv; i++)
				if (tolower((unsigned char)items[vis[i]][0]) == tolower((unsigned char)c)) { hit = vis[i]; hits++; }
			if (hits == 1) { result = hit; break; }
		}
	}

	free(vis);
	clDisableRaw();
	if (compact) { if (write(STDOUT_FILENO, "\r\x1b[2K", 5) < 0) {} }
	else {
		{ char up[16]; int k = snprintf(up, sizeof up, "\r\x1b[%dA", prev_rows + CHROME); if (write(STDOUT_FILENO, up, k) < 0) {} }
		for (int i = 0; i < prev_rows + CHROME + 1; i++) { if (write(STDOUT_FILENO, "\x1b[2K\n", 5) < 0) {} }
		{ char up[16]; int k = snprintf(up, sizeof up, "\x1b[%dA\r", prev_rows + CHROME + 1); if (write(STDOUT_FILENO, up, k) < 0) {} }
	}
#if HK_ATTACH
	if (phone_too) { hs_answer_armed = 0; result = hsAttachSettleChoice(result, items, n); }
#endif
	return result;
}

/* The picker's live preview: apply the theme under the highlight. Nothing is
   saved until enter; escape restores whatever was active. */
static void hkThemePreviewApply(int idx) {
	if (idx >= 0 && idx < 64 && hk_theme_pick[idx]) clThemeApply(hk_theme_pick[idx]);
}




static int hk_screen_fresh = 0;   /* the splash just cleared: start at the top */

static void clSplash(void) {
	if (!E.show_splash || E.pipe_mode || !E.color_enabled) return;
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;

	int cols = clTermCols(), rows = clTermRows();
	char version[64];
	snprintf(version, sizeof(version), "v%s", HAKO_VERSION);
	const char *press = "Press any key to start";

	int want_word  = (rows >= 15 && CL_WORD_FITS(cols));
	int want_crate = (rows >= 15 && cols >= 30) && (rows >= 26 || !want_word);

	int drawn = 0;                 /* rows printed, so they can be taken back */
	printf("\x1b[?25l");

	if (!want_word && !want_crate) {
		char line[96];
		snprintf(line, sizeof(line), HK_MARK " HAKO CODE %s", version);
		clClipToCells(line, cols);
		int pad = (cols - clCellWidth(line)) / 2; if (pad < 0) pad = 0;
		printf("%*s%s%s%s\n\n", pad, "", TH_ACCENT, line, ANSI_RESET); drawn += 2;
		const char *p2 = (cols >= (int)strlen(press) + 2) ? press : "press a key";
		pad = (cols - (int)strlen(p2)) / 2; if (pad < 0) pad = 0;
		printf("%*s%s\x1b[5m%s\x1b[25m%s", pad, "", TH_META, p2, ANSI_RESET);
	} else {
		const char **crate = (cols >= 42) ? CL_LOGO_MEDIUM : CL_LOGO_TINY;
		putchar('\n'); drawn++;
		for (int i = 0; want_word && CL_LOGO_WORD[i]; i++) {
			int pad = (cols - clCellWidth(CL_LOGO_WORD[i])) / 2; if (pad < 0) pad = 0;
			printf("%*s%s%s%s\n", pad, "", TH_ACCENT, CL_LOGO_WORD[i], ANSI_RESET); drawn++;
		}
		if (want_word && want_crate) { putchar('\n'); drawn++; }
		for (int i = 0; want_crate && crate[i]; i++) {
			int pad = (cols - clCellWidth(crate[i])) / 2; if (pad < 0) pad = 0;
			printf("%*s%s%s%s\n", pad, "", TH_AI, crate[i], ANSI_RESET); drawn++;
		}
		printf("\n%*s%s%sCODE%s\n", (cols - 4) / 2, "", TH_ACCENT, ANSI_BOLD, ANSI_RESET); drawn += 2;
		printf("%*s%s%s%s\n", (cols - (int)strlen(version)) / 2, "", TH_META, version, ANSI_RESET); drawn++;
		putchar('\n'); drawn++;
		int pad = (cols - (int)strlen(press)) / 2; if (pad < 0) pad = 0;
		printf("%*s%s\x1b[5m%s\x1b[25m%s", pad, "", TH_META, press, ANSI_RESET);
	}
	fflush(stdout);

	if (clEnableRaw() == 0) {
		char c;
		if (read(STDIN_FILENO, &c, 1) < 0) {}
		clDisableRaw();
	}
	/* Take the splash back off the screen: up over what was drawn, then erase
	   to the end. Relative moves only, so it cannot leave a hole behind. */
	printf("\r");
	if (drawn > 0) printf("\x1b[%dA", drawn);
	printf("\x1b[J\x1b[?25h");
	fflush(stdout);
	hk_screen_fresh = 1;
	fflush(stdout);
}

/* A frame around the three status lines only — not around the art. */
static void clBoxTop(int inner) {
	printf("  %s\xE2\x95\xAD", E.color_enabled ? TH_ACCENT : "");
	for (int i = 0; i < inner; i++) printf("\xE2\x94\x80");
	printf("\xE2\x95\xAE%s\n", E.color_enabled ? ANSI_RESET : "");
}

static void clBoxBot(int inner) {
	printf("  %s\xE2\x95\xB0", E.color_enabled ? TH_ACCENT : "");
	for (int i = 0; i < inner; i++) printf("\xE2\x94\x80");
	printf("\xE2\x95\xAF%s\n", E.color_enabled ? ANSI_RESET : "");
}

static void clBoxRow(int inner, const char *text, const char *color) {
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", text ? text : "");
	clClipToCells(buf, inner - 1);
	int pad = inner - 1 - clCellWidth(buf); if (pad < 0) pad = 0;
	printf("  %s\xE2\x94\x82%s %s%s%*s%s\xE2\x94\x82%s\n",
	       E.color_enabled ? TH_ACCENT : "", E.color_enabled ? ANSI_RESET : "",
	       (color && E.color_enabled) ? color : "", buf, pad, "",
	       E.color_enabled ? TH_ACCENT : "", E.color_enabled ? ANSI_RESET : "");
}

static void clBanner(aiData *data) {
	int cols = clTermCols();
	hkPromptInvalidate();
	int sk = hkLoadSkills(data);
	const char *trust_str = hkProjectTrusted() ? "on" : "off";
	const char *sess_state = E.session_resumed ? "resumed" : "new";

	int word = CL_WORD_FITS(cols);
	char row_a[64], row_b[64], row_c[64];
	snprintf(row_a, sizeof(row_a), " hako code %s", HAKO_VERSION);
	if (sk > 0) snprintf(row_b, sizeof(row_b), " trust %s \xC2\xB7 %d skills \xC2\xB7 %s", trust_str, sk, sess_state);
	else        snprintf(row_b, sizeof(row_b), " trust %s \xC2\xB7 session %s", trust_str, sess_state);
	snprintf(row_c, sizeof(row_c), " :help :providers :models");

	int max_row = word ? CL_WORD_CELLS : 0;
	int w;
	if ((w = clCellWidth(row_a)) > max_row) max_row = w;
	if ((w = clCellWidth(row_b)) > max_row) max_row = w;
	if ((w = clCellWidth(row_c)) > max_row) max_row = w;
	/* Under the wordmark the box interior IS the wordmark's width: borders sit
	   one cell outside its edges. Standing alone it gets a cell of air. */
	int inner = word ? max_row : max_row + 2;
	if (inner > cols - 4) inner = cols - 4;   /* two-space indent, then both borders */
	if (inner < 24) {
		printf("%s  %s\xE2\x97\x86 hako %s%s  %s\xC2\xB7 trust %s%s\n",
			hk_screen_fresh ? "" : "\n",
			E.color_enabled ? TH_ACCENT : "", HAKO_VERSION, E.color_enabled ? ANSI_RESET : "",
			E.color_enabled ? TH_META : "", trust_str, E.color_enabled ? ANSI_RESET : "");
		hk_screen_fresh = 0;
		fflush(stdout);
		return;
	}

	if (!hk_screen_fresh) putchar('\n');
	hk_screen_fresh = 0;
	if (word)
		for (int i = 0; CL_LOGO_WORD[i]; i++)
			printf("   %s%s%s\n", E.color_enabled ? TH_ACCENT : "",
			       CL_LOGO_WORD[i], E.color_enabled ? ANSI_RESET : "");
	clBoxTop(inner);
	clBoxRow(inner, row_a, TH_ACCENT);
	clBoxRow(inner, row_b, TH_META);
	clBoxRow(inner, row_c, TH_META);
	clBoxBot(inner);
	fflush(stdout);
}

typedef struct {
	char id[32];
	long last;
	int count;
	char first[80];
	char name[64];      /* from the chat's last meta row, or "" */
} clSessionInfo;

static int clEnumerateSessions(clSessionInfo *out, int max) {
	char pdir[PATH_MAX];
	if (!hkProjectStateDir(pdir, sizeof(pdir))) return 0;
	char sdir[PATH_MAX + 16];
	snprintf(sdir, sizeof(sdir), "%s/sessions", pdir);
	DIR *d = opendir(sdir);
	if (!d) return 0;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < max) {
		if (e->d_name[0] == '.') continue;
		const char *dot = strstr(e->d_name, ".jsonl");
		if (!dot) continue;
		int idlen = (int)(dot - e->d_name);
		if (idlen <= 0 || idlen >= (int)sizeof(out[0].id)) continue;
		char path[PATH_MAX + 64];
		if (snprintf(path, sizeof(path), "%s/%s", sdir, e->d_name) >= (int)sizeof(path)) continue;
		struct stat st;
		if (stat(path, &st) != 0) continue;
		int idx = n++;
		memcpy(out[idx].id, e->d_name, idlen); out[idx].id[idlen] = '\0';
		out[idx].last = (long)st.st_mtime;
		out[idx].count = 0;
		out[idx].first[0] = '\0';
		out[idx].name[0] = '\0';
		FILE *fp = fopen(path, "r");
		if (!fp) continue;
		char blk[65536];
		size_t got;
		int scanned_head = 0;
		while ((got = fread(blk, 1, sizeof(blk) - 1, fp)) > 0) {
			for (size_t i = 0; i < got; i++) if (blk[i] == '\n') out[idx].count++;
			blk[got] = '\0';
			/* A meta row (":name", ":color") can land anywhere; the last wins. */
			for (char *m = blk; (m = strstr(m, "\"role\":\"meta\"")) != NULL; m += 13) {
				char *v = hkExtractJsonString(m, "name");
				if (v) { snprintf(out[idx].name, sizeof(out[idx].name), "%s", v); free(v); }
			}
			if (!scanned_head) {
				scanned_head = 1;
				blk[got] = '\0';
				char *role = strstr(blk, "\"role\":\"user\"");
				if (role) {
					char *cp = strstr(role, "\"content\":\"");
					if (cp) {
						cp += 11;
						int j = 0;
						while (*cp && *cp != '"' && j < 60) out[idx].first[j++] = *cp++;
						out[idx].first[j] = '\0';
					}
				}
			}
		}
		fclose(fp);
	}
	closedir(d);
	return n;
}

/* "hey there my dude" → "hey there my…": up to `max` cells, cut on a word
   boundary, an ellipsis only when something was cut. */
static void clFirstWords(const char *src, int max, char *out, size_t cap) {
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", src ? src : "");
	int full = clCellWidth(buf);
	if (full <= max) { snprintf(out, cap, "%.*s", (int)cap - 1, buf); return; }
	clClipToCells(buf, max);
	char *sp = strrchr(buf, ' ');
	if (sp && sp > buf) *sp = '\0';
	snprintf(out, cap, "%.*s\xE2\x80\xA6", cap > 4 ? (int)cap - 4 : 0, buf);
}

static int clPromptYN(const char *q, int default_yes) {
	if (E.serve_mode) {
		char buf[32];
		if (hkServeInput(q, 0, buf, sizeof(buf)) != 0) return default_yes;
		if (!buf[0]) return default_yes;
		return (buf[0] == 'y' || buf[0] == 'Y');
	}
	/* Every choice in the program is the same selector; a typed y/N is the
	   fallback for pipes and plain terminals. Esc keeps the default. */
	if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && E.color_enabled) {
		const char *items[2] = { "yes", "no" };
		const char *t = q; while (*t == ' ') t++;
		int pick = clPopupSelect(t, items, 2, default_yes ? 0 : 1, NULL);
		if (pick < 0) return default_yes;
		return pick == 0;
	}
	if (E.color_enabled) printf("%s%s%s [%s] ", ANSI_BOLD, q, ANSI_RESET, default_yes ? "Y/n" : "y/N");
	else printf("%s [%s] ", q, default_yes ? "Y/n" : "y/N");
	fflush(stdout);
	char buf[64];
	if (!fgets(buf, sizeof(buf), stdin)) return default_yes;
	if (buf[0] == '\n' || buf[0] == '\0') return default_yes;
	return (buf[0] == 'y' || buf[0] == 'Y');
}

static void clStartupMenu(aiData *data) {
	if (!isatty(STDIN_FILENO)) return;

	if (!hkProjectTrusted()) {
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof(cwd))) {
			putchar('\n');
			clPutWrapped2(E.color_enabled ? ANSI_BOLD : "", "  ", "  ",
			              "Trust this directory for tool access?");
			{
				int cols = clTermCols();
				int avail = cols - 8;
				if (avail < 8) avail = 8;
				if (E.color_enabled) printf("  cwd: %s", ANSI_DIM);
				else printf("  cwd: ");
				const char *p = cwd;
				int rem = (int)strlen(cwd);
				int first = 1;
				while (rem > 0) {
					int chunk = rem > avail ? avail : rem;
					if (!first) printf("       ");
					fwrite(p, 1, chunk, stdout);
					putchar('\n');
					p += chunk;
					rem -= chunk;
					first = 0;
				}
				if (E.color_enabled) printf("%s", ANSI_RESET);
			}
			clPutWrapped2(E.color_enabled ? ANSI_DIM : "", "  ", "  ",
			              "(untrusted = no read_file, list_dir, write_file, run_shell)");
			if (clPromptYN("  grant trust?", 0)) {
				if (hkGrantProjectTrust()) printf("  %strusted.%s\n",
					E.color_enabled ? ANSI_AI : "", E.color_enabled ? ANSI_RESET : "");
				else printf("  %scould not grant.%s\n",
					E.color_enabled ? ANSI_ERR : "", E.color_enabled ? ANSI_RESET : "");
			} else {
				putchar('\n');
				clPutWrapped2(E.color_enabled ? ANSI_DIM : "", "  ", "  ",
				              "read-only mode. :trust to grant later.");
			}
		}
	}

	clSessionInfo sess[16];
	int nsess = clEnumerateSessions(sess, 16);

	if (nsess == 0) return;

	for (;;) {
		char cont[48];
		snprintf(cont, sizeof(cont), "continue%s", E.session_resumed ? " (resumed)" : "");
		char res[48];
		snprintf(res, sizeof(res), "resume one of %d", nsess);
		const char *items[3] = { "new", res, cont };
		int pick = clPopupSelect("session", items, 3, 2, NULL);
		if (pick < 0) { hkRestoreTerm(); exit(0); }
		if (pick == 0) {
			hkSessionFresh(data);
			hkSaveSession();
			printf("  new session: %s\n", E.session_id);
			return;
		}
		if (pick == 2) return;

		/* The list: id · age · turns · the chat's name, or the opening words. */
		static char rows[16][160];
		const char *ritems[16];
		long now = hk_time_unix();
		for (int i = 0; i < nsess; i++) {
			long age = now - sess[i].last;
			char unit; long val;
			if (age < 3600) { val = age / 60; unit = 'm'; }
			else if (age < 86400) { val = age / 3600; unit = 'h'; }
			else { val = age / 86400; unit = 'd'; }
			char label[80];
			if (sess[i].name[0]) snprintf(label, sizeof(label), "%s", sess[i].name);
			else clFirstWords(sess[i].first, 12, label, sizeof(label));
			const char *cur = (E.session_id && strcmp(E.session_id, sess[i].id) == 0) ? "*" : " ";
			snprintf(rows[i], sizeof(rows[i]), "%.1s%.40s  %ld%c %dt  %.80s", cur, sess[i].id, val, unit, sess[i].count, label);
			ritems[i] = rows[i];
		}
		int cur_i = 0;
		for (int i = 0; i < nsess; i++) if (E.session_id && !strcmp(E.session_id, sess[i].id)) cur_i = i;
		int sp = clPopupSelect("resume", ritems, nsess, cur_i, NULL);
		if (sp < 0) continue;                 /* esc here: back to the first menu */
		free(E.session_id);
		E.session_id = strdup(sess[sp].id);
		E.session_resumed = 1;
		E.session_started = hk_time_unix();
		free(E.session_name);  E.session_name = NULL;   /* the chat's own meta row sets these */
		free(E.session_color); E.session_color = NULL;
		aiClearHistory(data);
		hkLoadHistoryTail(data, AI_HISTORY_MAX, 40);
		hkSaveSession();
		printf("  resumed: %s (%d msgs loaded)\n", E.session_id, data->history_count);
		return;
	}
}


static void clWaitWorker(aiData *data) {
	if (!data->streaming) return;
#ifdef HAKO_WASM
	data->streaming = 0;          /* already finished: the turn ran inline */
#else
	pthread_join(data->worker_thread, NULL);
#endif
}

#define HK_QUEUE_MAX 8
static char *hk_queue[HK_QUEUE_MAX];
static int   hk_queue_n = 0;
static int   hk_quit_requested = 0;

/* Keep taking input while a turn runs. Returns when the worker signals it is
   done; anything submitted meanwhile is queued rather than dropped. */
static void clComposeWhileBusy(aiData *data, const char *prompt) {
	if (E.pipe_mode || E.output_json || !isatty(STDIN_FILENO)) return;
	char qline[4096];
	cl_composing = 1;
	while (data->streaming) {
		cl_bar_want = 1;
		if (!cl_bar_h) clBarShow(data);
		int qn = clReadLineRaw(prompt, qline, sizeof(qline));
		/* it pointed at the reader's own stack buffer, gone now: output printed
		   before the next read would redraw from dead memory (ASan, harness §queue) */
		cl_line_buf = NULL;
		if (qn == -5) break;                        /* the turn finished */
		if (qn == -6) {                             /* the worker is asking the user */
			cl_field_rows = 1;
			clBarHide();
			cl_composing = 0;
			/* Re-acknowledge while parked: the worker can claim AGAIN here
			   (a second tool in the same turn) and a claim nobody acks stalls
			   it for five seconds and then proceeds anyway — back to two
			   readers. */
			while (hk_need_stdin && data->streaming) {
				hk_stdin_free = 1;
				cl_sleep_ms(20);
			}
			/* The claim woke us with SIGUSR1 — the same signal that means "turn
			   over" — so cl_turn_done is set although the turn is still running.
			   Left set, the next read returned -5: the bar came down and typing
			   went unread for the rest of every turn that asked for approval. A
			   real finish still ends the loop: it clears streaming first. */
			if (data->streaming) cl_turn_done = 0;
			cl_composing = 1;
			continue;
		}
		if (qn == -1 || qn == -2) { hkAbortTurn(); break; }     /* ^C cancels it */
		if (qn == -3) {                             /* ctrl-o works mid-turn too */
			cl_field_rows = 1;
			clBarHide();
			hkExpandToggle(data);
			continue;
		}
		if (qn == -7) {                             /* ctrl-g mid-turn: compose while it thinks */
			cl_field_rows = 1;
			clBarHide();
			char *seed = cl_preset_input; cl_preset_input = NULL;
			char *body = clComposeInEditor(seed);
			if (body) { cl_preset_input = body; free(seed); } else cl_preset_input = seed;
			clRedrawReset();
			continue;
		}
		if (qn <= 0) continue;
		cl_field_rows = 1;
		clBarHide();
		/* A command is not a prompt: queueing `:q` behind a stuck turn is how
		   you end up unable to quit. Run it now. */
		if (qline[0] == ':' || qline[0] == '/') {
			clPrintRoleLine(HK_ROLE_USER, qline);
			aiPushHistoryStore(data, qline, HK_ROLE_USER);   /* on screen ⇒ in history: the redraw counts rows from it */
			clOutBegin();
			/* Commands that change the conversation or the prompt race the
			   worker that is using them (`:retry` started a second one). The
			   table says which can run now; the rest wait for the turn. */
			size_t cl = strcspn(qline + 1, " ");
			int ci = hkCmdFind(qline + 1, cl);
			int r = 1;
			if (ci >= 0 && !(hkCmdAt(ci).f & HK_CF_TURN) && hk_queue_n < HK_QUEUE_MAX) {
				hk_queue[hk_queue_n++] = strdup(qline);     /* runs the moment the turn ends */
				aiSayf(data, "%.40s runs when this turn ends (^C stops the turn)", qline);
			} else r = hkHandleSlash(data, qline);
			clOutEnd(data);
			if (r == 2) { hk_quit_requested = 1; hkAbortTurn(); break; }
			if (r == 3 && hk_queue_n < HK_QUEUE_MAX) {      /* /<skill> <text>: its text waits like any line */
				hk_queue[hk_queue_n++] = strdup(hk_slash_prompt);
				clOutBegin(); clOutEnd(data);
			}
			continue;
		}
		if (hk_queue_n < HK_QUEUE_MAX) {
			hk_queue[hk_queue_n++] = strdup(qline);
			char qm[48];
			snprintf(qm, sizeof(qm), "queued (%d waiting)", hk_queue_n);
			hkLogMessage("system", qm);
			clOutBegin(); clOutEnd(data);         /* redraw the bar: it carries the count */
		} else {
			aiSayf(data, "queue is full (%d) \xE2\x80\x94 wait for this turn", HK_QUEUE_MAX);
		}
	}
	cl_composing = 0;
	cl_line_buf = NULL;
	cl_field_rows = 1;
	clBarHide();
}

static void clEmitJsonResult(aiData *data, int ok, const char *err) {
	const char *body = err ? err : (data->current_response ? data->current_response : "");
	size_t cap = strlen(body) * 6 + 64;
	char *esc = malloc(cap);
	if (!esc) return;
	hkJsonEscapeInto(body, esc, (int)cap);
	char mesc[256], pesc[64];
	hkJsonEscapeInto(E.ai_model ? E.ai_model : "", mesc, (int)sizeof(mesc));
	hkJsonEscapeInto(hkProviderLabel(), pesc, (int)sizeof(pesc));
	printf("{\"ok\":%s,\"%s\":\"%s\",\"provider\":\"%s\",\"model\":\"%s\","
	       "\"usage\":{\"input_tokens\":%d,\"output_tokens\":%d},\"session\":\"%s\","
	       "\"version\":\"%s\"}\n",
	       ok ? "true" : "false", err ? "error" : "response", esc, pesc, mesc,
	       data->last_in_tokens, data->last_out_tokens,
	       E.session_id ? E.session_id : "", HAKO_VERSION);
	fflush(stdout);
	free(esc);
}

/* ---------- --bench: does the model call the right tool? ---------- */

typedef struct {
	char id[64];
	char prompt[1024];
	char want_tool[64];
	char want_key[64];
	char want_val[256];
} hkBenchTask;

static int hkBenchLoad(const char *path, hkBenchTask **out) {
	char *body = hkReadFileAll(path, 1 << 20);
	if (!body) return -1;
	int cap = 32, n = 0;
	hkBenchTask *v = calloc(cap, sizeof(*v));
	if (!v) { free(body); return -1; }
	for (char *p = body; p && *p; ) {
		char *nl = strchr(p, '\n');
		if (nl) *nl = '\0';
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '{') {
			if (n == cap) {
				hkBenchTask *b = realloc(v, (size_t)cap * 2 * sizeof(*v));
				if (!b) break;
				v = b; memset(v + cap, 0, (size_t)cap * sizeof(*v)); cap *= 2;
			}
			char *s;
			if ((s = hkExtractJsonString(p, "id")))     { snprintf(v[n].id, sizeof(v[n].id), "%s", s); free(s); }
			if ((s = hkExtractJsonString(p, "prompt"))) { char *d = hkJsonUnescape(s, (int)strlen(s));
			                                              snprintf(v[n].prompt, sizeof(v[n].prompt), "%s", d ? d : s);
			                                              free(d); free(s); }
			if ((s = hkExtractJsonString(p, "tool")))   { snprintf(v[n].want_tool, sizeof(v[n].want_tool), "%s", s); free(s); }
			if ((s = hkExtractJsonString(p, "key")))    { snprintf(v[n].want_key, sizeof(v[n].want_key), "%s", s); free(s); }
			if ((s = hkExtractJsonString(p, "value")))  { char *d = hkJsonUnescape(s, (int)strlen(s));
			                                              snprintf(v[n].want_val, sizeof(v[n].want_val), "%s", d ? d : s);
			                                              free(d); free(s); }
			if (v[n].prompt[0]) n++;
		}
		if (!nl) break;
		p = nl + 1;
	}
	free(body);
	*out = v;
	return n;
}

static int clBenchMode(aiData *data, const char *suite_path) {
	hkBenchTask *tasks = NULL;
	int n = hkBenchLoad(suite_path, &tasks);
	if (n <= 0) {
		fprintf(stderr, "hako: no tasks in %s\n", suite_path);
		free(tasks);
		return 2;
	}
	if (E.ai_provider_type == AI_PROVIDER_NONE) {
		fprintf(stderr, "hako: no provider configured\n");
		free(tasks);
		return 2;
	}

	hk_bench_mode = 1;
	E.ai_auto_approve = 1;
	E.color_enabled = 0;
	E.output_json = 1;      /* stdout is the report, not a transcript */
	hkPromptInvalidate();
	hkLoadSkills(data);

	int t1 = 0, t2 = 0;
	long began = hk_time_ms();
	printf("{\"suite\":\"%s\",\"provider\":\"%s\",\"model\":\"%s\",\"tasks\":[\n",
	       suite_path, hkProviderLabel(), E.ai_model ? E.ai_model : "");

	for (int i = 0; i < n; i++) {
		hk_bench_calls = 0;
		hk_bench_tool[0] = hk_bench_args[0] = '\0';

		pthread_mutex_lock(&data->lock);
		aiFreeMessages(data);
		pthread_mutex_unlock(&data->lock);
		free(data->current_prompt);
		data->current_prompt = strdup(tasks[i].prompt);

		aiWorkerSend(data);
		clWaitWorker(data);

		int expects_tool = tasks[i].want_tool[0] != '\0';
		int well_formed, correct;
		if (!expects_tool) {
			well_formed = correct = (hk_bench_calls == 0);
		} else {
			well_formed = (hk_bench_calls == 1);
			correct = well_formed && !strcmp(hk_bench_tool, tasks[i].want_tool);
			if (correct && tasks[i].want_val[0])
				correct = strstr(hk_bench_args, tasks[i].want_val) != NULL;
		}
		t1 += well_formed;
		t2 += correct;

		char esc[256];
		hkJsonEscapeInto(hk_bench_tool, esc, (int)sizeof(esc));
		printf("  {\"id\":\"%s\",\"calls\":%d,\"tool\":\"%s\",\"well_formed\":%s,\"correct\":%s}%s\n",
		       tasks[i].id, hk_bench_calls, esc,
		       well_formed ? "true" : "false", correct ? "true" : "false",
		       i + 1 < n ? "," : "");
		fflush(stdout);
	}

	printf("],\n \"total\":%d,\"well_formed\":%d,\"correct\":%d,\"elapsed_ms\":%ld,\"version\":\"%s\"}\n",
	       n, t1, t2, hk_time_ms() - began, HAKO_VERSION);
	fprintf(stderr, "\n%d/%d well-formed, %d/%d correct\n", t1, n, t2, n);

	hk_bench_mode = 0;
	free(tasks);
	return (t2 == n) ? 0 : 1;
}

/* Compose a prompt in a real editor. */
static char *clComposeInEditor(const char *seed) {
#ifdef HAKO_WASM
	return NULL;
#else
	const char *ed = E.editor;
	if (!ed || !*ed) ed = getenv("HAKO_EDITOR");
	if (!ed || !*ed) ed = getenv("VISUAL");
	if (!ed || !*ed) ed = getenv("EDITOR");
	if (!ed || !*ed) {
		if (system("command -v hake >/dev/null 2>&1") == 0) ed = "hake";
		else if (system("command -v nano >/dev/null 2>&1") == 0) ed = "nano";
		else ed = "vi";
	}

	char path[256];
	snprintf(path, sizeof(path), "/tmp/hako-compose-%d.md", (int)getpid());
	FILE *f = fopen(path, "w");
	if (!f) return NULL;
	fputs(seed ? seed : "", f);      /* what was already typed comes with you */
	fclose(f);

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "%s '%s'", ed, path);
	int rc = system(cmd);
	(void)rc;

	long n = 0;
	char *body = hk_fs_read(path, 1 << 20, &n);
	unlink(path);
	if (!body) return NULL;
	/* trailing newlines are an artefact of the editor, not of the message */
	while (n > 0 && (body[n-1] == '\n' || body[n-1] == '\r' || body[n-1] == ' ')) body[--n] = '\0';
	if (!n) { free(body); return NULL; }
	return body;
#endif
}

static int clOneShot(aiData *data, const char *prompt) {
	hkPromptInvalidate();
	hkLoadSkills(data);
	char *imaged = hkImagesFromLine(data, prompt);
	if (imaged) prompt = imaged;                /* lives for the process: a one-shot */
	aiAddHistoryRole(data, prompt, HK_ROLE_USER);
	hkLogMessage("user", prompt);
	E.session_turn_count++;
	hkSaveSession();
	free(data->current_prompt);
	data->current_prompt = strdup(prompt);
	if (E.ai_provider_type == AI_PROVIDER_NONE) {
		if (E.output_json) clEmitJsonResult(data, 0, "no provider configured");
		else fprintf(stderr, "error: no provider configured (~/.hakorc or /provider)\n");
		return 1;
	}
	aiWorkerSend(data);
	clWaitWorker(data);
	if (E.output_json) {
		int empty = !data->current_response || !*data->current_response;
		clEmitJsonResult(data, !empty,
			empty ? (hk_last_error[0] ? hk_last_error : "empty response") : NULL);
		return empty ? 1 : 0;
	}
	return 0;
}

static int hkSlashNeedsTerminal(const char *cmd) {
	while (*cmd == ':' || *cmd == '/') cmd++;
	static const char *tty_only[] = { NULL };
	for (int i = 0; tty_only[i]; i++) {
		size_t n = strlen(tty_only[i]);
		if (!strncmp(cmd, tty_only[i], n) && (!cmd[n] || cmd[n] == ' ')) return 1;
	}
	return 0;
}

/* What a remote client may never do: widen permissions or reach past this folder. */
static int hkSlashRefusedRemote(const char *cmd) {
	if (!hs_remote) return 0;
	while (*cmd == ':' || *cmd == '/') cmd++;
	static const char *widen[] = { "auto", "trust", "mcp", "skill install", "remote", "editor", "e", NULL };   /* :editor + :e run a command on the desk */
	for (int i = 0; widen[i]; i++) {
		size_t n = strlen(widen[i]);
		if (!strncmp(cmd, widen[i], n) && (!cmd[n] || cmd[n] == ' ')) return 1;
	}
	if (!strncmp(cmd, "mode", 4) && (!cmd[4] || cmd[4] == ' ')) {
		const char *a = cmd + 4;
		while (*a == ' ') a++;
		return strcmp(a, "ask") != 0 && strcmp(a, "plan") != 0;
	}
	return 0;
}

static void clPipeBegin(aiData *data) {
	hkRcProjectLoad(data);
	static int upd;
	if (!upd++) hkUpdateCheckStart();
	clPipeEmitInit();
	if (E.session_resumed && data->history_count == 0)
		hkLoadHistoryTail(data, 40, 40);
	hkPromptInvalidate();
	hkLoadSkills(data);
}

static int clPipeHandleLine(aiData *data, const char *line) {
	char *type = hkExtractJsonString(line, "type");
	if (!type) return 0;
	int quit = 0;

	if (strcmp(type, "quit") == 0) {
		quit = 1;
	} else if (strcmp(type, "prompt") == 0) {
		char *text = hkExtractJsonString(line, "text");
		char *imaged = text && *text && !E.serve_mode ? hkImagesFromLine(data, text) : NULL;   /* Rei's typed paths; studio uploads */
		if (imaged) { free(text); text = imaged; }
		if (text && *text) {
			free(data->current_prompt);
			data->current_prompt = text;
			if (E.ai_provider_type == AI_PROVIDER_NONE) {
				clPipeEmitMsg("system", "No provider set. :login <provider> or set ai_provider in ~/.hakorc");
				clPipeEmitDone(NULL);
			} else {
				hkLogMessage("user", text);
				E.session_turn_count++;
				hkSaveSession();
				aiWorkerSend(data);
				clWaitWorker(data);
			}
		} else {
			free(text);
		}
	} else if (strcmp(type, "slash") == 0) {
		char *cmd = hkExtractJsonString(line, "cmd");
		if (cmd && E.serve_mode && hkSlashNeedsTerminal(cmd)) {
			clPipeEmitMsg("system",
				"that command needs the terminal — run it in the shell running `hako --serve`, then reload");
			clPipeEmitDone(NULL);
			free(cmd);
			free(type);
			return 0;
		}
		if (cmd && E.serve_mode && hkSlashRefusedRemote(cmd)) {
			clPipeEmitMsg("system",
				"not from a remote device: only the desktop terminal can widen what the agent may do");
			clPipeEmitDone(NULL);
			free(cmd);
			free(type);
			return 0;
		}
		if (cmd) {
			char *pfx = NULL;
			if (cmd[0] != ':' && cmd[0] != '/') {
				pfx = malloc(strlen(cmd) + 2);
				if (pfx) { pfx[0] = ':'; strcpy(pfx + 1, cmd); }
			}
			char was_p[48], was_m[160];
			snprintf(was_p, sizeof was_p, "%s", hkProviderLabel());
			snprintf(was_m, sizeof was_m, "%s", E.ai_model ? E.ai_model : "");
			int r = hkHandleSlash(data, pfx ? pfx : cmd);
			if (!r) clPipeEmitMsg("system", "unknown command");
			if (strcmp(was_p, hkProviderLabel()) || strcmp(was_m, E.ai_model ? E.ai_model : ""))
				clPipeEmitState();
			free(pfx);
			free(cmd);
			clPipeEmitDone(NULL);
			if (r == 2) quit = 1;
			if (r == 3) {                       /* /<skill> <text>: the text runs as a prompt */
				size_t cap = strlen(hk_slash_prompt) * 6 + 64;
				char *esc = malloc(cap), *pl = malloc(cap + 32);
				if (esc && pl) {
					clPipeEscape(hk_slash_prompt, esc, (int)cap);
					snprintf(pl, cap + 32, "{\"type\":\"prompt\",\"text\":\"%s\"}", esc);
					free(type);
					int q = clPipeHandleLine(data, pl);
					free(esc); free(pl);
					return q;
				}
				free(esc); free(pl);
			}
		}
	} else if (strcmp(type, "open") == 0) {
		char *dir = hkExtractJsonString(line, "dir");
		if (dir && *dir && chdir(dir) != 0) {
			clPipeEmitMsg("system", "cannot open that folder");
		} else {
			hk_rc_proj_done = 0; hk_rc_proj[0] = '\0';   /* another folder, another .hakorc */
			hkRcReload();
			clRcApplyAll(0);
			aiClearHistory(data);
			hkLoadSession();
			clCredsRestoreFor(hkProviderLabel());
			hkHealEndpoint();
			clPipeBegin(data);
		}
		free(dir);
		clPipeEmitDone(NULL);
	} else if (strcmp(type, "quiet") == 0) {          /* POST /api/cmd: :name, :mode, :session new... out of the chat */
		char *cmd = hkExtractJsonString(line, "cmd");
		if (cmd && *cmd && !hkSlashRefusedRemote(cmd) && !hkIsMenuCommand(cmd) && (cmd[0] == ':' || cmd[0] == '/')) {
			hk_pipe_quiet = 1;
			int r = hkHandleSlash(data, cmd);
			hk_pipe_quiet = 0;
			(void)r;
			clPipeEmitState();
		}
		free(cmd);
	} else if (strcmp(type, "theme") == 0) {          /* a settings pick: applied and saved, nothing said */
		char *name = hkExtractJsonString(line, "name");
		const char *tn[64];
		int tc = hkThemeNames(tn, 64);
		for (int i = 0; name && i < tc; i++)
			if (!strcmp(tn[i], name)) {
				/* the studio's own pick: `studio.theme` when the rc has one, else the
				   shared key, and this process follows whatever now decides it */
				hkRcSaveTheme(HK_RC_STUDIO, name);
				const char *th = hkRcGet(HK_RC_CODE, "theme");
				clThemeApply(th && *th ? th : "terminal");
				clPipeEmitState();
				break;
			}
		free(name);
	}

	free(type);
	return quit;
}

static int clPipeMode(aiData *data) {
	clPipeBegin(data);

	char line[4096];
	while (fgets(line, sizeof(line), stdin)) {
		int len = (int)strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
		if (len == 0) continue;
		if (clPipeHandleLine(data, line)) break;
	}
	return 0;
}


#ifdef HAKO_WASM
static void hkServePush(const char *json) { (void)json; }
static int hkServeAnswer(char *out, size_t cap) { (void)out; (void)cap; return -1; }
static int hkServeInput(const char *prompt, int hidden, char *out, size_t cap) {
	(void)prompt; (void)hidden; (void)out; (void)cap; return -1;
}
static int clServeMode(aiData *data, const char *dir) {
	(void)data; (void)dir;
	fprintf(stderr, "hako: --serve is not available on this platform\n");
	return 2;
}
#else

/* Winsock is BSD sockets under other names; shim them and keep one server. */
#ifdef _WIN32
#define hs_close(fd)        closesocket((SOCKET)(fd))
#define hs_read(fd, b, n)   recv((SOCKET)(fd), (char *)(b), (int)(n), 0)
#define hs_write(fd, b, n)  send((SOCKET)(fd), (const char *)(b), (int)(n), 0)
#define hs_errno()          WSAGetLastError()
#define HS_EINTR            WSAEINTR
#define HS_ECONNABORTED     WSAECONNABORTED
#define HS_EWOULDBLOCK      WSAEWOULDBLOCK
#define HS_EMFILE           WSAEMFILE
#else
#define hs_close(fd)        close(fd)
#define hs_read(fd, b, n)   read((fd), (b), (n))
#define hs_write(fd, b, n)  write((fd), (b), (n))
#define hs_errno()          errno
#define HS_EINTR            EINTR
#define HS_ECONNABORTED     ECONNABORTED
#define HS_EWOULDBLOCK      EWOULDBLOCK
#define HS_EMFILE           EMFILE
#endif

#define HS_RING     2048
#define HS_QUEUE    32
#define HS_BODY_MAX (1024 * 256)

/* The studio UI is 340KB of byte array in this same translation unit, and there… */
#if defined(HAKO_EMBED_WEB) && defined(__has_include)
#  if __has_include("hako_web.h")
#    include "hako_web.h"
#    define HS_EMBEDDED 1
#  endif
#endif

static struct {
	int         port;
	const char *bind_addr;
	const char *web_dir;
	char        token[33];      /* when set, every request must present it */

	char *ring[HS_RING];
	long  ring_next, ring_floor;
	long  pending_tool_id, pending_choice_id;

	char *queue[HS_QUEUE];
	int   q_head, q_tail;

	char  answer[8];
	int   answer_ready, answer_waiting;

	char  input[1024];
	char  input_prompt[256];
	int   input_ready, input_waiting, input_hidden;

	int   busy;

	pthread_mutex_t ring_lock, q_lock, a_lock;
	pthread_cond_t  ring_cond, q_cond, a_cond;
} HS = {
	.port = 8787, .bind_addr = "127.0.0.1", .web_dir = NULL,
	.ring_lock = PTHREAD_MUTEX_INITIALIZER, .q_lock = PTHREAD_MUTEX_INITIALIZER,
	.a_lock    = PTHREAD_MUTEX_INITIALIZER,
	.ring_cond = PTHREAD_COND_INITIALIZER,  .q_cond = PTHREAD_COND_INITIALIZER,
	.a_cond    = PTHREAD_COND_INITIALIZER
};


static void hkServePush(const char *json) {
	if (!json || !*json) return;
	int is_tool = strstr(json, "\"type\":\"tool_request\"") != NULL;
	if (is_tool || strstr(json, "\"type\":\"choice_request\"")) {
		/* Armed before the browser can see the request, so an instant answer is kept. */
		const char *idp = strstr(json, "\"id\":");
		long id = idp ? atol(idp + 5) : 0;
		HS.pending_tool_id   = is_tool ? id : 0;
		HS.pending_choice_id = is_tool ? 0 : id;
		pthread_mutex_lock(&HS.a_lock);
		HS.answer_ready = 0;
		HS.answer_waiting = 1;
		pthread_mutex_unlock(&HS.a_lock);
	}
	pthread_mutex_lock(&HS.ring_lock);
	int slot = (int)(HS.ring_next % HS_RING);
	free(HS.ring[slot]);
	HS.ring[slot] = strdup(json);
	HS.ring_next++;
	pthread_cond_broadcast(&HS.ring_cond);
	pthread_mutex_unlock(&HS.ring_lock);
}

static void hsRingReset(void) {
	pthread_mutex_lock(&HS.ring_lock);
	for (int i = 0; i < HS_RING; i++) { free(HS.ring[i]); HS.ring[i] = NULL; }
	HS.ring_floor = HS.ring_next;
	pthread_cond_broadcast(&HS.ring_cond);
	pthread_mutex_unlock(&HS.ring_lock);
}


static void hsQueuePush(const char *json) {
	pthread_mutex_lock(&HS.q_lock);
	int next = (HS.q_tail + 1) % HS_QUEUE;
	if (next != HS.q_head) {
		HS.queue[HS.q_tail] = strdup(json);
		HS.q_tail = next;
		pthread_cond_signal(&HS.q_cond);
	}
	pthread_mutex_unlock(&HS.q_lock);
}

static char *hsQueuePop(void) {
	pthread_mutex_lock(&HS.q_lock);
	while (HS.q_head == HS.q_tail)
		pthread_cond_wait(&HS.q_cond, &HS.q_lock);
	char *line = HS.queue[HS.q_head];
	HS.queue[HS.q_head] = NULL;
	HS.q_head = (HS.q_head + 1) % HS_QUEUE;
	pthread_mutex_unlock(&HS.q_lock);
	return line;
}

static int hkServeAnswer(char *out, size_t cap) {
	/* An answer posted while nothing waited must not resolve the NEXT request. */
	pthread_mutex_lock(&HS.a_lock);
	HS.answer_waiting = 1;
	while (!HS.answer_ready)
		pthread_cond_wait(&HS.a_cond, &HS.a_lock);
	snprintf(out, cap, "%s", HS.answer);
	HS.answer_ready = 0;
	HS.answer_waiting = 0;
	pthread_mutex_unlock(&HS.a_lock);
	return 0;
}

static int hkServeInput(const char *prompt, int hidden, char *out, size_t cap) {
	char esc[512], req[700];
	clPipeEscape(prompt ? prompt : "", esc, sizeof(esc));
	snprintf(req, sizeof(req), "{\"type\":\"input_request\",\"prompt\":\"%.511s\",\"hidden\":%d}",
	         esc, hidden ? 1 : 0);

	pthread_mutex_lock(&HS.a_lock);
	HS.input_ready = 0;
	HS.input_waiting = 1;
	HS.input_hidden = hidden ? 1 : 0;
	snprintf(HS.input_prompt, sizeof(HS.input_prompt), "%s", prompt ? prompt : "");
	pthread_mutex_unlock(&HS.a_lock);
	clPipeEmitRaw(req);

	pthread_mutex_lock(&HS.a_lock);
	while (!HS.input_ready)
		pthread_cond_wait(&HS.a_cond, &HS.a_lock);
	snprintf(out, cap, "%s", HS.input);
	HS.input_ready = 0;
	HS.input_waiting = 0;
	HS.input_prompt[0] = '\0';
	memset(HS.input, 0, sizeof(HS.input));
	pthread_mutex_unlock(&HS.a_lock);
	return 0;
}

static void hsInputSet(const char *value) {
	pthread_mutex_lock(&HS.a_lock);
	snprintf(HS.input, sizeof(HS.input), "%s", value ? value : "");
	HS.input_ready = 1;
	pthread_cond_broadcast(&HS.a_cond);
	pthread_mutex_unlock(&HS.a_lock);
}

static int hsAnswerSet(const char *ans) {
	pthread_mutex_lock(&HS.a_lock);
	int took = HS.answer_waiting && !HS.answer_ready;
	if (took) {
		snprintf(HS.answer, sizeof(HS.answer), "%.7s", ans ? ans : "n");
		HS.answer_ready = 1;
		pthread_cond_broadcast(&HS.a_cond);
	}
	pthread_mutex_unlock(&HS.a_lock);
	return took;
}

/* Stop must unpark a turn waiting on the browser, or a client that never
   answers (curl, an older UI) wedges the serve loop for good. */
static void hsReleaseWaits(void) {
	hsAnswerSet("");
	HS.pending_tool_id = HS.pending_choice_id = 0;
	pthread_mutex_lock(&HS.a_lock);
	if (HS.input_waiting && !HS.input_ready) {
		HS.input[0] = '\0';
		HS.input_ready = 1;
		pthread_cond_broadcast(&HS.a_cond);
	}
	pthread_mutex_unlock(&HS.a_lock);
}


static int hsWriteAll(int fd, const char *buf, size_t len) {
	while (len > 0) {
		ssize_t n = hs_write(fd, buf, len);
		if (n < 0) { if (errno == EINTR) continue; return -1; }
		buf += n; len -= (size_t)n;
	}
	return 0;
}

static void hsSend(int fd, const char *status, const char *ctype, const char *body, size_t len) {
	char head[256];
	int n = snprintf(head, sizeof(head),
		"HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
		"Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, ctype, len);
	if (hsWriteAll(fd, head, (size_t)n) == 0 && len) hsWriteAll(fd, body, len);
}

static void hsSendJson(int fd, const char *body) {
	hsSend(fd, "200 OK", "application/json", body, strlen(body));
}

static void hsSendOk(int fd, int ok) {
	hsSend(fd, ok ? "200 OK" : "400 Bad Request", "application/json",
	       ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
}

typedef struct { char *buf; size_t len, cap; } hsBuf;

static void hsPut(hsBuf *b, const char *s) {
	size_t need = strlen(s);
	if (b->len + need + 1 >= b->cap) {
		size_t ncap = b->cap ? b->cap : 1024;
		while (ncap < b->len + need + 1) ncap *= 2;
		char *nb = realloc(b->buf, ncap);
		if (!nb) return;
		b->buf = nb; b->cap = ncap;
	}
	memcpy(b->buf + b->len, s, need);
	b->len += need;
	b->buf[b->len] = '\0';
}

static void hsPutEsc(hsBuf *b, const char *s) {
	size_t cap = (s ? strlen(s) : 0) * 6 + 8;
	char *esc = malloc(cap);
	if (!esc) return;
	clPipeEscape(s ? s : "", esc, (int)cap);
	hsPut(b, esc);
	free(esc);
}

static void hsPutFmt(hsBuf *b, const char *fmt, ...) {
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	hsPut(b, tmp);
}

static char *hsReadBodyMax(int fd, char *req, size_t used, long max, long *got);
static char *hsReadBody(int fd, char *req, size_t used) {
	long got;
	return hsReadBodyMax(fd, req, used, HS_BODY_MAX, &got);
}

static char *hsReadBodyMax(int fd, char *req, size_t used, long max, long *got) {
	long want = 0;
	const char *cl = strcasestr(req, "\r\nContent-Length:");
	if (cl) want = strtol(cl + 17, NULL, 10);
	if (want < 0 || want > max) want = 0;

	char *body = malloc((size_t)want + 1);
	if (!body) return NULL;
	size_t have = 0;
	char *start = strstr(req, "\r\n\r\n");
	if (start) {
		start += 4;
		have = used - (size_t)(start - req);
		if (have > (size_t)want) have = (size_t)want;
		memcpy(body, start, have);
	}
	while (have < (size_t)want) {
		ssize_t n = hs_read(fd, body + have, (size_t)want - have);
		if (n <= 0) break;
		have += (size_t)n;
	}
	body[have] = '\0';
	*got = (long)have;
	return body;
}

static void hsQueryParam(const char *req, const char *key, char *out, size_t cap) {
	out[0] = '\0';
	char pat[64];
	snprintf(pat, sizeof(pat), "%.32s=", key);
	const char *q = strstr(req, pat);
	if (!q) return;
	q += strlen(pat);
	size_t o = 0;
	while (*q && *q != ' ' && *q != '&' && o + 1 < cap) {
		if (*q == '%' && q[1] && q[2]) {
			char hex[3] = { q[1], q[2], 0 };
			out[o++] = (char)strtol(hex, NULL, 16);
			q += 3;
		} else if (*q == '+') { out[o++] = ' '; q++; }
		else out[o++] = *q++;
	}
	out[o] = '\0';
}

/* A host literal this server may be addressed by: loopback, a private IPv4
   address written as numbers, or the address it is bound to. Exact match on
   the whole host, never a prefix — "127.0.0.1.evil.example" used to pass. */
static int hsHostIsLocal(const char *h, size_t n) {
	if (n == 0 || n > 63) return 0;
	char host[64];
	memcpy(host, h, n); host[n] = '\0';
	if (!strcasecmp(host, "localhost") || !strcmp(host, "[::1]") || !strcmp(host, "127.0.0.1")) return 1;
	if (HS.bind_addr && !strcmp(host, HS.bind_addr)) return 1;
	unsigned q[4]; char tail;
	if (sscanf(host, "%u.%u.%u.%u%c", &q[0], &q[1], &q[2], &q[3], &tail) != 4) return 0;
	for (int i = 0; i < 4; i++) if (q[i] > 255) return 0;
	return q[0] == 127 || q[0] == 10 || (q[0] == 192 && q[1] == 168)
	    || (q[0] == 172 && q[1] >= 16 && q[1] <= 31);
}

/* host part of "scheme://host[:port][/...]" or of a bare "host[:port]" */
static int hsHeaderHostLocal(const char *v, int has_scheme) {
	while (*v == ' ') v++;
	if (has_scheme) {
		const char *sep = strstr(v, "://");
		if (!sep || sep - v > 5) return 0;
		v = sep + 3;
	}
	const char *e = v;
	if (*e == '[') { while (*e && *e != ']' && *e != '\r') e++; if (*e == ']') e++; }
	else while (*e && *e != ':' && *e != '/' && *e != '\r' && *e != '\n' && *e != ' ') e++;
	return hsHostIsLocal(v, (size_t)(e - v));
}

static int hsOriginOk(const char *req) {
	const char *o = strcasestr(req, "\r\nOrigin:");
	if (!o) return 1;                       /* not a browser cross-origin request */
	return hsHeaderHostLocal(o + 9, 1);
}

/* DNS rebinding: a page whose name now resolves to 127.0.0.1 is same-origin to
   itself, sends no Origin on a GET, and could read the chat or a file. Its Host
   header still carries its own name. */
static int hsHostOk(const char *req) {
	const char *h = strcasestr(req, "\r\nHost:");
	if (!h) return 1;
	return hsHeaderHostLocal(h + 7, 0);
}

static void hsPrintLanUrls(void) {
#ifdef _WIN32
	/* No getifaddrs; the host name resolves to the addresses worth printing. */
	char host[256];
	if (gethostname(host, sizeof(host) - 1) != 0) return;
	host[sizeof(host) - 1] = '\0';
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, NULL, &hints, &res) != 0) return;
	for (struct addrinfo *p = res; p; p = p->ai_next) {
		char ip[INET_ADDRSTRLEN];
		struct sockaddr_in *sin = (struct sockaddr_in *)(void *)p->ai_addr;
		if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
		if (!strncmp(ip, "127.", 4)) continue;
		printf("  phone    http://%s:%d\n", ip, HS.port);
	}
	freeaddrinfo(res);
#else
	struct ifaddrs *ifa = NULL;
	if (getifaddrs(&ifa) != 0) return;
	for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
		if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
		if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
		char ip[INET_ADDRSTRLEN];
		struct sockaddr_in *sin = (struct sockaddr_in *)(void *)p->ifa_addr;
		if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
		printf("  phone    http://%s:%d  (%s)\n", ip, HS.port, p->ifa_name);
	}
	freeifaddrs(ifa);
#endif
}

static int hsSessionPath(const char *sid, const char *ext, char *out, size_t cap) {
	if (!sid || !*sid || strchr(sid, '/') || strstr(sid, "..")) return 0;
	char pdir[PATH_MAX];
	if (!hkProjectStateDir(pdir, sizeof(pdir))) return 0;
	snprintf(out, cap, "%s/sessions/%s%s", pdir, sid, ext);
	return 1;
}

static void hsSessionTitle(const char *sid, const char *fallback, char *out, size_t cap) {
	snprintf(out, cap, "%s", fallback ? fallback : "");
	char path[PATH_MAX + 64];
	if (!hsSessionPath(sid, ".title", path, sizeof(path))) return;
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[256];
	if (fgets(line, sizeof(line), f)) {
		size_t l = strlen(line);
		while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
		if (l) snprintf(out, cap, "%s", line);
	}
	fclose(f);
}

static void hsServeSessions(int fd) {
	clSessionInfo info[64];
	int n = clEnumerateSessions(info, 64);

	hsBuf b = {0};
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
	hsPut(&b, "{\"dir\":\"");
	hsPutEsc(&b, cwd);
	hsPut(&b, "\",\"current\":\"");
	hsPutEsc(&b, E.session_id ? E.session_id : "");
	hsPut(&b, "\",\"sessions\":[");
	for (int i = 0; i < n; i++) {
		char title[200];
		if (info[i].name[0]) snprintf(title, sizeof title, "%s", info[i].name);   /* :name wins */
		else hsSessionTitle(info[i].id, info[i].first, title, sizeof(title));
		hsPut(&b, i ? ",{\"id\":\"" : "{\"id\":\"");
		hsPutEsc(&b, info[i].id);
		hsPut(&b, "\",\"title\":\"");
		hsPutEsc(&b, title);
		hsPutFmt(&b, "\",\"mtime\":%ld,\"turns\":%d}", info[i].last, info[i].count);
	}
	hsPut(&b, "]}");
	hsSendJson(fd, b.buf ? b.buf : "{\"sessions\":[]}");
	free(b.buf);
}

static void hsServeHistory(int fd, const char *sid) {
	char path[PATH_MAX + 64];
	if (!hsSessionPath(sid, ".jsonl", path, sizeof(path))) {
		hsSend(fd, "400 Bad Request", "application/json", "{\"messages\":[]}", 15);
		return;
	}
	hsBuf b = {0};
	hsPut(&b, "{\"messages\":[");
	FILE *f = fopen(path, "r");
	if (f) {
		char *line = malloc(HS_BODY_MAX);
		int first = 1;
		if (line) {
			while (fgets(line, HS_BODY_MAX, f)) {
				if (!strstr(line, "\"role\"")) continue;
				size_t l = strlen(line);
				while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
				if (!l) continue;
				if (!first) hsPut(&b, ",");
				hsPut(&b, line);
				first = 0;
			}
			free(line);
		}
		fclose(f);
	}
	hsPut(&b, "]}");
	hsSendJson(fd, b.buf ? b.buf : "{\"messages\":[]}");
	free(b.buf);
}

static char hs_root[PATH_MAX];
static volatile time_t hs_last_seen;      /* last request that was not the event stream */

/* A remote client stays inside the folder hako --serve was started in. */
static int hsInRoot(const char *path) {
	if (!hs_remote) return 1;
	const char *p = path && *path ? path : ".";
	char real[PATH_MAX];
	if (!realpath(p, real)) {             /* not made yet (mkdir): judge the name */
		if (strstr(p, "..")) return 0;
		if (p[0] != '/') return 1;
		snprintf(real, sizeof(real), "%s", p);
	}
	size_t n = strlen(hs_root);
	if (n <= 1) return 1;
	return !strncmp(real, hs_root, n) && (real[n] == '\0' || real[n] == '/');
}

static void hsServeDirs(int fd, const char *path) {
	char real[PATH_MAX];
	if (!hsInRoot(path)) path = hs_root;
	if (!realpath(path && *path ? path : ".", real)) snprintf(real, sizeof(real), "%s", path ? path : ".");

	hsBuf b = {0};
	hsPut(&b, "{\"path\":\"");
	hsPutEsc(&b, real);
	hsPut(&b, "\",\"dirs\":[");

	hsBuf files = {0};
	DIR *d = opendir(real);
	int first = 1, ffirst = 1;
	if (d) {
		struct dirent *de;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.') continue;
			char full[PATH_MAX + 256];
			struct stat st;
			if (!hkFmtFits(full, sizeof(full), "%s/%s", real, de->d_name)) continue;
			if (stat(full, &st) != 0) continue;
			if (S_ISDIR(st.st_mode)) {
				hsPut(&b, first ? "\"" : ",\"");
				hsPutEsc(&b, de->d_name);
				hsPut(&b, "\"");
				first = 0;
			} else if (S_ISREG(st.st_mode)) {
				hsPut(&files, ffirst ? "{\"name\":\"" : ",{\"name\":\"");
				hsPutEsc(&files, de->d_name);
				hsPutFmt(&files, "\",\"bytes\":%lld,\"mtime\":%ld}", (long long)st.st_size, (long)st.st_mtime);
				ffirst = 0;
			}
		}
		closedir(d);
	}
	hsPut(&b, "],\"files\":[");
	if (files.buf) hsPut(&b, files.buf);
	hsPut(&b, "]}");
	free(files.buf);

	hsSendJson(fd, b.buf ? b.buf : "{\"dirs\":[],\"files\":[]}");
	free(b.buf);
}

#define HS_UPLOAD_MAX (128L * 1024 * 1024)

static int hsSafeName(const char *in, char *out, size_t cap) {
	const char *slash = strrchr(in, '/');
	const char *base = slash ? slash + 1 : in;
	if (!*base || !strcmp(base, ".") || !strcmp(base, "..")) return 0;
	size_t o = 0;
	for (const char *p = base; *p && o + 1 < cap; p++) {
		unsigned char c = (unsigned char)*p;
		out[o++] = (c < 0x20 || c == '/' || c == '\\' || c == ':') ? '_' : (char)c;
	}
	out[o] = '\0';
	return o > 0;
}

/* Claims a free name by creating it (O_EXCL), so two uploads of one name at the
   same moment can never land on the same file. Returns an open FILE or NULL. */
static FILE *hsUniquePath(const char *name, char *out, size_t cap) {
	const char *dot = strrchr(name, '.');
	int stem = dot ? (int)(dot - name) : (int)strlen(name);
	for (int i = 0; i < 1000; i++) {
		if (i == 0) snprintf(out, cap, "%s", name);
		else        snprintf(out, cap, "%.*s-%d%s", stem, name, i, dot ? dot : "");
		int fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd >= 0) { FILE *f = fdopen(fd, "wb"); if (!f) close(fd); return f; }
		if (errno != EEXIST) return NULL;
	}
	return NULL;
}

static long hsRecvToFile(int sock, char *req, size_t used, const char *path, long want, FILE *f) {
	if (!f) return -1;

	long got = 0;
	char *start = strstr(req, "\r\n\r\n");
	if (start) {
		start += 4;
		size_t have = used - (size_t)(start - req);
		if (have > (size_t)want) have = (size_t)want;
		if (have && fwrite(start, 1, have, f) != have) { fclose(f); hk_fs_remove(path); return -1; }
		got = (long)have;
	}
	char buf[65536];
	while (got < want) {
		size_t chunk = (size_t)(want - got);
		if (chunk > sizeof(buf)) chunk = sizeof(buf);
		ssize_t n = hs_read(sock, buf, chunk);
		if (n <= 0) break;
		if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); hk_fs_remove(path); return -1; }
		got += n;
	}
	fclose(f);
	if (got != want) { hk_fs_remove(path); return -1; }
	return got;
}

static void hsServeModels(int fd, const char *search_csv) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	hsBuf b = {0};
	hsPut(&b, "{\"installed\":[");

	char mroot[PATH_MAX];
	snprintf(mroot, sizeof(mroot), "%s/.hako/models", home);
	DIR *d = opendir(mroot);
	int first = 1;
	if (d) {
		struct dirent *de;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.') continue;
			char w[PATH_MAX + 512];
			struct stat st;
			if (snprintf(w, sizeof(w), "%s/%s/%s.mlf2", mroot, de->d_name, de->d_name) >= (int)sizeof(w))
				continue;
			if (stat(w, &st) != 0) continue;
			hsPut(&b, first ? "{\"id\":\"" : ",{\"id\":\"");
			hsPutEsc(&b, de->d_name);
			hsPutFmt(&b, "\",\"bytes\":%lld}", (long long)st.st_size);
			first = 0;
		}
		closedir(d);
	}
	hsPut(&b, "],\"found\":[");

	char paths[PATH_MAX * 2];
	snprintf(paths, sizeof(paths), "%s/Downloads%s%s", home,
	         (search_csv && *search_csv) ? "," : "", (search_csv && *search_csv) ? search_csv : "");
	first = 1;
	char *save = NULL;
	for (char *p = strtok_r(paths, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
		while (*p == ' ') p++;
		if (!*p) continue;
		DIR *sd = opendir(p);
		if (!sd) continue;
		struct dirent *de;
		while ((de = readdir(sd))) {
			const char *dot = strrchr(de->d_name, '.');
			if (!dot || strcmp(dot, ".mlf2") != 0) continue;
			char full[PATH_MAX + 256], id[128];
			struct stat st;
			snprintf(full, sizeof(full), "%s/%s", p, de->d_name);
			if (stat(full, &st) != 0) continue;
			snprintf(id, sizeof(id), "%.*s", (int)(dot - de->d_name), de->d_name);
			hsPut(&b, first ? "{\"id\":\"" : ",{\"id\":\"");
			hsPutEsc(&b, id);
			hsPut(&b, "\",\"path\":\"");
			hsPutEsc(&b, full);
			hsPutFmt(&b, "\",\"bytes\":%lld}", (long long)st.st_size);
			first = 0;
		}
		closedir(sd);
	}

	hsPut(&b, "],\"known\":[\"hako-sho\",\"hako-koi\"],\"catalog\":[");
	{
		static char cat[256][96];
		int n = hkGatherModels(cat, 256);
		for (int i = 0; i < n; i++) {
			hsPut(&b, i ? ",\"" : "\"");
			hsPutEsc(&b, cat[i]);
			hsPut(&b, "\"");
		}
	}
	hsPut(&b, "],\"provider\":\"");
	hsPutEsc(&b, hkProviderLabel());
	hsPut(&b, "\",\"model\":\"");
	hsPutEsc(&b, E.ai_model ? E.ai_model : "");
	hsPut(&b, "\"}");
	hsSendJson(fd, b.buf ? b.buf : "{}");
	free(b.buf);
}

static void hsFindMatches(const char *root, const char *name, const char *child,
                          int depth, int *budget, hsBuf *b, int *first, int *found) {
	if (depth > 3 || *budget <= 0 || *found >= 8) return;
	DIR *d = opendir(root);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d)) && *budget > 0) {
		if (de->d_name[0] == '.') continue;
		static const char *skip[] = { "node_modules", "vendor", "target", "build",
		                              "Caches", "Containers", "Group Containers",
		                              "Application Support", "Photos Library.photoslibrary",
		                              "Music", "Movies", "Pictures", NULL };
		int skipped = 0;
		for (int k = 0; skip[k]; k++) if (!strcmp(de->d_name, skip[k])) { skipped = 1; break; }
		if (skipped) continue;
		char full[PATH_MAX];
		struct stat st;
		if (!hkFmtFits(full, sizeof(full), "%s/%s", root, de->d_name)) continue;
		if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		(*budget)--;
		if (!strcmp(de->d_name, name)) {
			int ok = 1;
			if (child && *child) {
				char probe[PATH_MAX + 256];
				struct stat cs;
				snprintf(probe, sizeof(probe), "%s/%s", full, child);
				ok = (stat(probe, &cs) == 0);
			}
			if (ok) {
				char quoted[PATH_MAX + 4];
				snprintf(quoted, sizeof(quoted), "\"%s\"", full);
				if (!b->buf || !strstr(b->buf, quoted)) {
					hsPut(b, *first ? "\"" : ",\"");
					hsPutEsc(b, full);
					hsPut(b, "\"");
					*first = 0;
					(*found)++;
				}
				continue;
			}
		}
		hsFindMatches(full, name, child, depth + 1, budget, b, first, found);
	}
	closedir(d);
}

static void hsServeState(int fd, const char *req) {
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
	hsBuf b = {0};
	hsPut(&b, "{\"version\":\"" HAKO_VERSION "\",\"dir\":\"");
	hsPutEsc(&b, cwd);
	hsPut(&b, "\",\"home\":\"");
	const char *home = getenv("HOME") ? getenv("HOME") : "/";
	hsPutEsc(&b, hs_remote ? hs_root : home);     /* a phone browses from the project, not ~ */
	hsPut(&b, "\",\"places\":[");
	if (!hs_remote) {
		static const struct { const char *label, *rel; } PL[] = {
			{ "home",         ""                                            },
			{ "iCloud Drive", "/Library/Mobile Documents/com~apple~CloudDocs" },
			{ "Documents",    "/Documents"                                  },
			{ "Desktop",      "/Desktop"                                    },
			{ "Downloads",    "/Downloads"                                  },
			{ "Developer",    "/Developer"                                  },
			{ "code",         "/code"                                       },
			{ "projects",     "/projects"                                   },
			{ "src",          "/src"                                        },
			{ "git",          "/git"                                        },
			{ "repos",        "/repos"                                      },
			{ "work",         "/work"                                       },
			{ NULL, NULL }
		};
		static const struct { const char *label, *abs; } PA[] = {
			{ "Volumes", "/Volumes" },
			{ "media",   "/media"   },
			{ "mnt",     "/mnt"     },
			{ "srv",     "/srv"     },
			{ "opt",     "/opt"     },
			{ "tmp",     "/tmp"     },
			{ NULL, NULL }
		};
		int first = 1;
		for (int i = 0; PA[i].label; i++) {
			struct stat st;
			if (stat(PA[i].abs, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
			hsPut(&b, first ? "{\"label\":\"" : ",{\"label\":\"");
			hsPutEsc(&b, PA[i].label);
			hsPut(&b, "\",\"path\":\"");
			hsPutEsc(&b, PA[i].abs);
			hsPut(&b, "\"}");
			first = 0;
		}
		for (int i = 0; PL[i].label; i++) {
			char full[PATH_MAX];
			struct stat st;
			snprintf(full, sizeof(full), "%s%s", home, PL[i].rel);
			if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
			hsPut(&b, first ? "{\"label\":\"" : ",{\"label\":\"");
			hsPutEsc(&b, PL[i].label);
			hsPut(&b, "\",\"path\":\"");
			hsPutEsc(&b, full);
			hsPut(&b, "\"}");
			first = 0;
		}
	}
	hsPut(&b, "],\"provider\":\"");
	hsPutEsc(&b, hkProviderLabel());
	hsPut(&b, "\",\"model\":\"");
	hsPutEsc(&b, E.ai_model ? E.ai_model : "");
	hsPut(&b, "\",\"session\":\"");
	hsPutEsc(&b, E.session_id ? E.session_id : "");
	hsPutFmt(&b, "\",\"turns\":%d,\"busy\":%d,\"remote\":%d,\"yolo\":%d,\"awaiting_input\":%d,\"awaiting_answer\":%d,\"input_hidden\":%d,\"input_prompt\":\"",
	         E.session_turn_count, HS.busy || (hs_attached && G_AI.streaming), hs_remote, E.ai_auto_approve && !hs_remote, HS.input_waiting, HS.answer_waiting, HS.input_hidden);
	hsPutEsc(&b, HS.input_prompt);
	hsPut(&b, "\",\"caps\":[");
	static const char *caps[] = {
		"prompt", "slash", "approve", "input", "stop", "events", "sessions", "history",
		"dirs", "files", "upload", "models", "providers", "places", "find", "link", "open", "rename", "delete",
		"mkdir", "shell", "image", "cmd", NULL
	};
	for (int i = 0, n = 0; caps[i]; i++) {
		if (hs_remote && (!strcmp(caps[i], "places") || !strcmp(caps[i], "find"))) continue;
		hsPut(&b, n++ ? ",\"" : "\"");
		hsPut(&b, caps[i]);
		hsPut(&b, "\"");
	}
	hsPut(&b, "]");
	{   /* §4: session_name, theme (?for=studio asks for the studio's), slots, voice, update, commands */
		char forw[16];
		hsQueryParam(req, "for", forw, sizeof forw);
		char *x = NULL;
		size_t xl = 0, xc = 0;
		hkStateExtras(&x, &xl, &xc, !strcmp(forw, "studio") ? HK_RC_STUDIO : HK_RC_CODE, 1);
		if (x) { x[xl] = '\0'; hsPut(&b, x); free(x); }
	}
	hsPut(&b, "}");
	hsSendJson(fd, b.buf ? b.buf : "{}");
	free(b.buf);
}

static void hsServeEventsLoop(int fd);
static void hsServeEvents(int fd) {
	__sync_fetch_and_add(&hs_streams, 1);
	hsServeEventsLoop(fd);
	__sync_fetch_and_sub(&hs_streams, 1);
}

static void hsServeEventsLoop(int fd) {
	const char *head =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
		"Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n";
	if (hsWriteAll(fd, head, strlen(head)) != 0) return;

	pthread_mutex_lock(&HS.ring_lock);
	long cursor = HS.ring_next > HS_RING ? HS.ring_next - HS_RING : 0;
	if (cursor < HS.ring_floor) cursor = HS.ring_floor;
	pthread_mutex_unlock(&HS.ring_lock);

	for (;;) {
		char *payload = NULL;
		pthread_mutex_lock(&HS.ring_lock);
		if (cursor < HS.ring_floor) cursor = HS.ring_floor;
		while (cursor >= HS.ring_next) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 15;
			if (hs_listen_stop) { pthread_mutex_unlock(&HS.ring_lock); return; }
			if (pthread_cond_timedwait(&HS.ring_cond, &HS.ring_lock, &ts) == ETIMEDOUT) {
				pthread_mutex_unlock(&HS.ring_lock);
				if (hsWriteAll(fd, ":ka\n\n", 5) != 0) return;
				pthread_mutex_lock(&HS.ring_lock);
			}
			if (cursor < HS.ring_floor) cursor = HS.ring_floor;
		}
		if (cursor < HS.ring_next - HS_RING) cursor = HS.ring_next - HS_RING;
		char *line = HS.ring[(int)(cursor % HS_RING)];
		if (line) payload = strdup(line);
		cursor++;
		pthread_mutex_unlock(&HS.ring_lock);

		if (payload) {
			size_t need = strlen(payload) + 16;
			char *frame = malloc(need);
			if (frame) {
				int n = snprintf(frame, need, "data: %s\n\n", payload);
				int bad = hsWriteAll(fd, frame, (size_t)n) != 0;
				free(frame);
				if (bad) { free(payload); return; }
			}
			free(payload);
		}
	}
}

static void hsServeIndex(int fd) {
	if (HS.web_dir) {
		char path[PATH_MAX + 16];
		snprintf(path, sizeof(path), "%s/index.html", HS.web_dir);
		FILE *f = fopen(path, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			char *buf = (sz > 0) ? malloc((size_t)sz) : NULL;
			size_t got = buf ? fread(buf, 1, (size_t)sz, f) : 0;
			fclose(f);
			if (buf) {
				hsSend(fd, "200 OK", "text/html; charset=utf-8", buf, got);
				free(buf);
				return;
			}
		}
	}
#ifdef HS_EMBEDDED
	hsSend(fd, "200 OK", "text/html; charset=utf-8", HAKO_WEB_INDEX, sizeof(HAKO_WEB_INDEX) - 1);
#else
	{
		static const char msg[] =
			"hako is serving, but this build has no web UI compiled in.\n\n"
			"The API is live — /api/state, /api/prompt and /api/events all work,\n"
			"so anything speaking the protocol can drive it right now.\n\n"
			"For the browser UI, either:\n"
			"  make WEB=1                 build with the UI embedded\n"
			"  hako --serve --web <dir>   serve it from a hako-studio checkout\n";
		hsSend(fd, "503 Service Unavailable", "text/plain; charset=utf-8", msg, sizeof(msg) - 1);
	}
#endif
}

#if HK_ATTACH
/* Lines from the phone wait here for the terminal's main loop, which runs them
   exactly like typed ones. The reader is woken only while it waits for a line. */
#define HS_INJECT_MAX 16
static char *hs_inject[HS_INJECT_MAX];
static int hs_inject_n;
static pthread_mutex_t hs_inject_lock = PTHREAD_MUTEX_INITIALIZER;

static void hsInjectPush(const char *line) {
	pthread_mutex_lock(&hs_inject_lock);
	int ok = hs_inject_n < HS_INJECT_MAX;
	if (ok) hs_inject[hs_inject_n++] = strdup(line);
	pthread_mutex_unlock(&hs_inject_lock);
	if (!ok) { clPipeEmitMsg("system", "16 lines are already waiting on the desktop"); return; }
	if (hk_line_wait && g_main_thread_set) pthread_kill(g_main_thread, SIGUSR1);
}

static int hkInjectTake(char *out, size_t cap) {
	pthread_mutex_lock(&hs_inject_lock);
	int got = hs_inject_n > 0;
	if (got) {
		snprintf(out, cap, "%s", hs_inject[0]);
		free(hs_inject[0]);
		memmove(hs_inject, hs_inject + 1, sizeof(hs_inject[0]) * (size_t)(--hs_inject_n));
	}
	pthread_mutex_unlock(&hs_inject_lock);
	return got;
}

/* The terminal popup closed: say who answered. -3 = the phone did. */
static int hsAttachSettle(int pick) {
	char ans[8] = {0};
	pthread_mutex_lock(&HS.a_lock);
	if (HS.answer_ready) snprintf(ans, sizeof(ans), "%s", HS.answer);
	HS.answer_ready = HS.answer_waiting = 0;
	pthread_mutex_unlock(&HS.a_lock);
	if (pick == -3) {
		hkLogMessage("system", "approval answered on a remote device");
		return (ans[0] == 'y' || ans[0] == 'Y' || ans[0] == 'a' || ans[0] == 'A') ? 0 : 1;
	}
	char d[16];
	while (read(hs_wake[0], d, sizeof(d)) > 0) {}     /* a late phone answer must not close the next popup */
	if (HS.pending_tool_id) {
		char line[128];
		snprintf(line, sizeof(line), "{\"type\":\"tool_decision\",\"id\":%ld,\"answer\":\"%c\"}",
		         HS.pending_tool_id, pick == 0 ? 'y' : 'n');
		hkServePush(line);
		HS.pending_tool_id = 0;
	}
	return pick;
}

/* Same for a menu: -3 = the phone picked (its 1-based index, or 0 = cancel). */
static int hsAttachSettleChoice(int pick, const char **items, int n) {
	char ans[8] = {0};
	pthread_mutex_lock(&HS.a_lock);
	if (HS.answer_ready) snprintf(ans, sizeof(ans), "%s", HS.answer);
	HS.answer_ready = HS.answer_waiting = 0;
	pthread_mutex_unlock(&HS.a_lock);
	if (pick == -3) {
		hkLogMessage("system", "choice answered on a remote device");
		return hkChoiceIndex(ans, items, n);
	}
	char d[16];
	while (read(hs_wake[0], d, sizeof(d)) > 0) {}
	if (HS.pending_choice_id) {
		char line[96];
		snprintf(line, sizeof(line), "{\"type\":\"choice_decision\",\"id\":%ld}", HS.pending_choice_id);
		hkServePush(line);
		HS.pending_choice_id = 0;
	}
	return pick;
}

static int hsRouteToTerminal(const char *type, const char *value) {
	if (!hs_attached) return 0;
	if (!strcmp(type, "open")) {
		clPipeEmitMsg("system", "a shared terminal session keeps its folder");
		return 1;
	}
	if (strcmp(type, "prompt") != 0 && strcmp(type, "slash") != 0) return 0;
	if (!value || !*value) return 1;
	char line[4096];
	int cmd = !strcmp(type, "slash") || value[0] == ':' || value[0] == '/';
	snprintf(line, sizeof(line), "%s%s", cmd && value[0] != ':' && value[0] != '/' ? ":" : "", value);
	if (cmd && hkSlashRefusedRemote(line))
		clPipeEmitMsg("system", "not from a remote device: only the desktop terminal can widen what the agent may do");
	else if (cmd && hkIsMenuCommand(line))
		clPipeEmitMsg("system", "menus open at the desktop. here, use the studio's menus or give the name (:model <id>)");
	else
		hsInjectPush(line);
	return 1;
}
#endif

static void hsEnqueueCmd(const char *type, const char *field, const char *value) {
#if HK_ATTACH
	if (hsRouteToTerminal(type, value)) return;
#endif
	size_t cap = (value ? strlen(value) : 0) * 6 + 128;
	char *esc = malloc(cap), *line = NULL;
	if (!esc) return;
	clPipeEscape(value ? value : "", esc, (int)cap);
	size_t lcap = strlen(esc) + 96;
	if ((line = malloc(lcap)) != NULL) {
		snprintf(line, lcap, "{\"type\":\"%s\",\"%s\":\"%s\"}", type, field, esc);
		hsQueuePush(line);
		if (strcmp(type, "prompt") == 0 || strcmp(type, "slash") == 0) {
			int bare = (strcmp(type, "slash") == 0 && esc[0] != ':' && esc[0] != '/');
			snprintf(line, lcap, "{\"type\":\"message\",\"role\":\"user\",\"text\":\"%s%s\"}",
			         bare ? ":" : "", esc);
			hkServePush(line);
		}
		free(line);
	}
	free(esc);
}

/* Constant-time-ish compare: a token is a secret and must not be guessable
   one character at a time. */
static int hsTokenEq(const char *a, const char *b, size_t n) {
	unsigned char diff = 0;
	for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

static int hsAuthOk(const char *req) {
	size_t tl = strlen(HS.token);
	const char *h = strcasestr(req, "\r\nX-Hako-Token:");
	if (h) {
		h += 16;
		while (*h == ' ') h++;
		if (strlen(h) >= tl && hsTokenEq(h, HS.token, tl)) return 1;
	}
	const char *c = strcasestr(req, "\r\nCookie:");
	while (c) {
		const char *k = strstr(c, "hako_token=");
		if (!k) break;
		k += 11;
		if (strlen(k) >= tl && hsTokenEq(k, HS.token, tl)) return 1;
		c = k;
	}
	return 0;
}

/* A connection the relay link made (a socketpair end): Noise already proved
   the sender holds the account key, so the token and host checks do not apply. */
#define HS_TRUSTED ((intptr_t)1 << 30)

static void *hsConnThread(void *arg) {
	int trusted = ((intptr_t)arg & HS_TRUSTED) != 0;
	int fd = (int)((intptr_t)arg & ~HS_TRUSTED);
	char req[8192];
	size_t used = 0;

	while (used + 1 < sizeof(req)) {
		ssize_t n = hs_read(fd, req + used, sizeof(req) - used - 1);
		if (n <= 0) {
			if (E.debug) fprintf(stderr, "[serve] read=%zd errno=%d after %zu bytes\n", n, errno, used);
			hs_close(fd);
			return NULL;
		}
		used += (size_t)n;
		req[used] = '\0';
		if (strstr(req, "\r\n\r\n")) break;
	}
	if (E.debug) {
		const char *eol = strpbrk(req, "\r\n");
		fprintf(stderr, "[serve] %.*s\n", eol ? (int)(eol - req) : 32, req);
	}

	/* A tunnel makes this reachable from anywhere, and the agent runs tools as
	   the user. The token is what stands between a URL and a shell. */
	if (!trusted && HS.token[0] && !hsAuthOk(req)) {
		const char *t = strstr(req, "?t=");
		if (t && !strncmp(req, "GET ", 4) && !strncmp(t + 3, HS.token, strlen(HS.token))) {
			/* Hand it to the browser as a cookie and get it out of the URL, so
			   it does not sit in history or leak through a referrer. */
			char head[512];
			int n = snprintf(head, sizeof(head),
				"HTTP/1.1 302 Found\r\nLocation: /\r\n"
				"Set-Cookie: hako_token=%s; Path=/; Max-Age=604800; SameSite=Strict; HttpOnly\r\n"
				"Content-Length: 0\r\n\r\n", HS.token);
			if (hs_write(fd, head, (size_t)n) < 0) {}
		} else {
			const char *body = "unauthorized\n";
			char head[256];
			int n = snprintf(head, sizeof(head),
				"HTTP/1.1 401 Unauthorized\r\nContent-Type: text/plain\r\n"
				"Content-Length: %zu\r\n\r\n", strlen(body));
			if (hs_write(fd, head, (size_t)n) < 0) {}
			if (hs_write(fd, body, strlen(body)) < 0) {}
		}
		hs_close(fd);
		return NULL;
	}

	/* With a token the request already proved itself (its cookie is SameSite=
	   Strict, so no other site can send it). Without one, only a local page may
	   write, and only a local name may read. */
	if (strncmp(req, "GET /api/events", 15) != 0) hs_last_seen = time(NULL);

	int is_post = (strncmp(req, "POST ", 5) == 0);
	if (!trusted && !HS.token[0] && ((is_post && !hsOriginOk(req)) || !hsHostOk(req))) {
		hsSend(fd, "403 Forbidden", "text/plain", "cross-origin denied\n", 20);
		hs_close(fd);
		return NULL;
	}

	if (!strncmp(req, "GET / ", 6) || !strncmp(req, "GET /index.html", 15)) {
		hsServeIndex(fd);
	} else if (!strncmp(req, "GET /icon-180.png", 17) || !strncmp(req, "GET /apple-touch-icon", 21)
	        || !strncmp(req, "GET /favicon", 12)) {
#ifdef HAKO_WEB_HAVE_ICON
		hsSend(fd, "200 OK", "image/png", (const char *)HAKO_WEB_ICON, sizeof(HAKO_WEB_ICON) - 1);
#else
		hsSend(fd, "404 Not Found", "text/plain", "no icon\n", 8);
#endif
	} else if (!strncmp(req, "GET /manifest.webmanifest", 25)) {
		static const char mf[] =
			"{\"name\":\"hako studio\",\"short_name\":\"hako\",\"start_url\":\"/\","
			"\"display\":\"standalone\",\"background_color\":\"#ece7da\",\"theme_color\":\"#ece7da\","
			"\"icons\":[{\"src\":\"/icon-180.png\",\"sizes\":\"180x180\",\"type\":\"image/png\","
			"\"purpose\":\"any maskable\"}]}";
		hsSend(fd, "200 OK", "application/manifest+json", mf, sizeof(mf) - 1);
	} else if (!strncmp(req, "GET /api/events", 15)) {
		hsServeEvents(fd);
	} else if (!strncmp(req, "GET /api/find", 13)) {
		char name[256], child[256];
		hsQueryParam(req, "name", name, sizeof(name));
		hsQueryParam(req, "child", child, sizeof(child));
		hsBuf b = {0};
		hsPut(&b, "{\"candidates\":[");
		if (name[0] && !strchr(name, '/') && !hs_remote) {
			const char *home = getenv("HOME");
			char cwd[PATH_MAX];
			if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
			char icloud[PATH_MAX];
			snprintf(icloud, sizeof(icloud), "%s/Library/Mobile Documents/com~apple~CloudDocs",
			         home ? home : "");
			const char *roots[] = { cwd, icloud, home, "/Volumes", "/mnt", "/srv", NULL };
			int first = 1, budget = 20000, found = 0;
			for (int i = 0; roots[i] && found < 8; i++) {
				if (!roots[i] || !*roots[i]) continue;
				struct stat st;
				if (stat(roots[i], &st) != 0) continue;
				hsFindMatches(roots[i], name, child, 0, &budget, &b, &first, &found);
			}
		}
		hsPut(&b, "]}");
		hsSendJson(fd, b.buf ? b.buf : "{\"candidates\":[]}");
		free(b.buf);
	} else if (!strncmp(req, "GET /api/providers", 18)) {
		/* The same rows :login shows, so the studio's menu cannot drift from the CLI's. */
		static struct { const char *name, *kind; char desc[96]; } P[40];
		int np = 0;
		static const char *LOCAL[][2] = {
			{ "mithraeum", "hako family, runs on this machine" },
			{ "ollama", "run `ollama serve` first" },
		};
		for (int i = 0; i < 2; i++) { P[np].name = LOCAL[i][0]; P[np].kind = "local"; snprintf(P[np].desc, sizeof P[np].desc, "%s", LOCAL[i][1]); np++; }
		for (int i = 0; i < HK_NSIGNIN; i++) {
			if (!strcmp(HK_SIGNIN[i].id, "api")) continue;
			P[np].name = HK_SIGNIN[i].id; P[np].kind = "oauth";
			snprintf(P[np].desc, sizeof P[np].desc, "%s · %s", HK_SIGNIN[i].name, HK_SIGNIN[i].what); np++;
		}
		for (int i = 0; i < HK_NKEYS; i++) {
			if (!strcmp(HK_KEYS[i].id, "custom")) continue;
			P[np].name = HK_KEYS[i].id; P[np].kind = "key";
			snprintf(P[np].desc, sizeof P[np].desc, "%s · %s", HK_KEYS[i].name, HK_KEYS[i].what); np++;
		}
		P[np].name = NULL;
		const char *active = hkProviderLabel();
		hsBuf b = {0};
		hsPut(&b, "{\"active\":\"");
		hsPutEsc(&b, active ? active : "");
		hsPut(&b, "\",\"providers\":[");
		for (int i = 0; P[i].name; i++) {
			int saved = !strcmp(P[i].kind, "oauth") ? hkSignedIn(P[i].name)
			          : !strcmp(P[i].kind, "key")   ? hkKeySaved(P[i].name) : 0;
			hsPut(&b, i ? ",{\"name\":\"" : "{\"name\":\"");
			hsPutEsc(&b, P[i].name);
			hsPut(&b, "\",\"kind\":\"");
			hsPutEsc(&b, P[i].kind);
			hsPut(&b, "\",\"desc\":\"");
			hsPutEsc(&b, P[i].desc);
			hsPutFmt(&b, "\",\"saved\":%d,\"active\":%d}",
			         saved ? 1 : 0, (active && !strcmp(active, P[i].name)) ? 1 : 0);
		}
		hsPut(&b, "]}");
		hsSendJson(fd, b.buf ? b.buf : "{\"providers\":[]}");
		free(b.buf);
	} else if (!strncmp(req, "GET /api/state", 14)) {
		hsServeState(fd, req);
	} else if (!strncmp(req, "GET /api/sessions", 17)) {
		hsServeSessions(fd);
	} else if (!strncmp(req, "GET /api/history", 16)) {
		char sid[128];
		hsQueryParam(req, "id", sid, sizeof(sid));
		hsServeHistory(fd, sid);
	} else if (!strncmp(req, "GET /api/dirs", 13)) {
		char path[PATH_MAX];
		hsQueryParam(req, "path", path, sizeof(path));
		hsServeDirs(fd, path);
	} else if (!strncmp(req, "GET /api/models", 15)) {
		char search[PATH_MAX];
		hsQueryParam(req, "search", search, sizeof(search));
		hsServeModels(fd, search);
	} else if (!strncmp(req, "POST /api/prompt", 16) || !strncmp(req, "POST /api/slash", 15)) {
		int slash = !strncmp(req, "POST /api/slash", 15);
		char *body = hsReadBody(fd, req, used);
		if (body && *body) hsEnqueueCmd(slash ? "slash" : "prompt", slash ? "cmd" : "text", body);
		free(body);
		hsSendOk(fd, 1);
	} else if (!strncmp(req, "POST /api/cmd", 13)) {   /* a menu's command: runs, emits state, says nothing */
		char *body = hsReadBody(fd, req, used);
		if (body && *body) hsEnqueueCmd(hs_attached ? "slash" : "quiet", "cmd", body);
		free(body);
		hsSendOk(fd, 1);
	} else if (!strncmp(req, "POST /api/theme", 15)) {
		char *body = hsReadBody(fd, req, used);
		if (body && *body) hsEnqueueCmd("theme", "name", body);
		free(body);
		hsSendOk(fd, 1);
	} else if (!strncmp(req, "POST /api/approve", 17)) {
		char *body = hsReadBody(fd, req, used);
		int took = hsAnswerSet(body && *body ? body : "n");
#if HK_ATTACH
		if (took && hs_attached && hs_answer_armed && write(hs_wake[1], "x", 1) < 0) {}
#endif
		if (took && HS.pending_tool_id) {
			char line[128];
			snprintf(line, sizeof(line), "{\"type\":\"tool_decision\",\"id\":%ld,\"answer\":\"%c\"}",
			         HS.pending_tool_id, body && *body ? body[0] : 'n');
			hkServePush(line);
			HS.pending_tool_id = 0;
		} else if (took && HS.pending_choice_id) {
			char line[96];
			snprintf(line, sizeof(line), "{\"type\":\"choice_decision\",\"id\":%ld}", HS.pending_choice_id);
			hkServePush(line);
			HS.pending_choice_id = 0;
		}
		free(body);
		hsSendOk(fd, took);
	} else if (!strncmp(req, "POST /api/image", 15)) {   /* a pasted or picked image: its "[image #N]" back */
		long n = 0;
		char *body = hsReadBodyMax(fd, req, used, HK_IMAGE_MAX * 4, &n);
		char mark[32], err[160], js[256];
		if (body && n > 0 && hkImageStore(body, (size_t)n, mark, sizeof mark, err, sizeof err))
			snprintf(js, sizeof js, "{\"ok\":true,\"marker\":\"%s\"}", mark);
		else snprintf(js, sizeof js, "{\"ok\":false,\"error\":\"%s\"}", body && n > 0 ? err : "empty, or over 15 MB");
		free(body);
		hsSendJson(fd, js);
	} else if (!strncmp(req, "POST /api/upload", 16)) {
		char raw[512], name[256], path[PATH_MAX];
		hsQueryParam(req, "name", raw, sizeof(raw));
		long want = 0;
		const char *cl = strcasestr(req, "\r\nContent-Length:");
		if (cl) want = strtol(cl + 17, NULL, 10);
		if (!hsSafeName(raw, name, sizeof(name)) || want <= 0 || want > HS_UPLOAD_MAX) {
			hsSendOk(fd, 0);
		} else {
			FILE *uf = hsUniquePath(name, path, sizeof(path));
			long got = hsRecvToFile(fd, req, used, path, want, uf);
			if (got > 0) {
				char msg[PATH_MAX + 96];
				if (got < 1024) snprintf(msg, sizeof(msg), "received %s (%ld B) into this folder", path, got);
				else            snprintf(msg, sizeof(msg), "received %s (%ld KB) into this folder", path, (got + 1023) / 1024);
				clPipeEmitMsg("system", msg);
			}
			hsSendOk(fd, got > 0);
		}
	} else if (!strncmp(req, "POST /api/mkdir", 15)) {
		char *body = hsReadBody(fd, req, used);
		int ok = body && *body && !strstr(body, "..")
		      && hsInRoot(body)
		      && hk_fs_mkdirp(body) == 0;
		free(body);
		hsSendOk(fd, ok);
	} else if (!strncmp(req, "POST /api/input", 15)) {
		char *body = hsReadBody(fd, req, used);
		hsInputSet(body ? body : "");
		free(body);
		hsSendOk(fd, 1);
	} else if (!strncmp(req, "POST /api/stop", 14)) {
		hkAbortTurn();                  /* as ^C: a local engine mid-prefill never reads the flag */
		hsReleaseWaits();
		hsSendOk(fd, 1);
	} else if (!strncmp(req, "POST /api/open", 14) || !strncmp(req, "POST /api/restart", 17)) {
		char *body = hsReadBody(fd, req, used);
		if (!hsInRoot(body ? body : "")) {
			free(body);
			hsSendOk(fd, 0);
		} else {
			hsRingReset();
			hsEnqueueCmd("open", "dir", body ? body : "");
			free(body);
			hsSendOk(fd, 1);
		}
	} else if (!strncmp(req, "POST /api/reset", 15)) {
		hsRingReset();
		hsSendOk(fd, 1);
	} else if (!strncmp(req, "POST /api/link", 14)) {
		char *body = hsReadBody(fd, req, used);
		char id[128] = {0};
		if (body && *body) {
			const char *slash = strrchr(body, '/');
			const char *base = slash ? slash + 1 : body;
			const char *dot = strrchr(base, '.');
			snprintf(id, sizeof(id), "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
		}
		free(body);
		hsSendOk(fd, id[0] && hkMithraeumRelocate(id));
	} else if (!strncmp(req, "POST /api/session/delete", 24)) {
		char *sid = hsReadBody(fd, req, used);
		char path[PATH_MAX + 64];
		int ok = 0;
		if (sid && hsSessionPath(sid, ".title", path, sizeof(path))) {
			hk_fs_remove(path);
			if (hsSessionPath(sid, ".jsonl", path, sizeof(path))) ok = (hk_fs_remove(path) == 0);
		}
		free(sid);
		hsSendOk(fd, ok);
	} else if (!strncmp(req, "POST /api/session/rename", 24)) {
		char *body = hsReadBody(fd, req, used);
		char path[PATH_MAX + 64];
		int ok = 0;
		char *nl = body ? strchr(body, '\n') : NULL;
		if (nl) {
			*nl = '\0';
			if (hsSessionPath(body, ".title", path, sizeof(path))) {
				const char *title = nl + 1;
				if (!*title) { hk_fs_remove(path); ok = 1; }
				else {
					FILE *f = fopen(path, "w");
					if (f) {
						for (int i = 0; title[i] && i < 200; i++)
							fputc((unsigned char)title[i] < 0x20 ? ' ' : title[i], f);
						fputc('\n', f);
						fclose(f);
						ok = 1;
					}
				}
			}
		}
		free(body);
		hsSendOk(fd, ok);
	} else {
		hsSend(fd, "404 Not Found", "text/plain", "not found\n", 10);
	}

	hs_close(fd);
	return NULL;
}

/* Remote reach is not left open by accident: two hours with nothing from a
   device and --serve --token stops. HAKO_REMOTE_IDLE=<minutes>, 0 = never. */

#if HK_ATTACH
static void hsAttachOff(void);
#endif

static void *hsIdleThread(void *arg) {
	long mins = (long)(intptr_t)arg;
	for (;;) {
		sleep(30);
		if (!E.serve_mode && !hs_attached) return NULL;
		if (HS.busy || G_AI.streaming || HS.answer_waiting || HS.input_waiting) { hs_last_seen = time(NULL); continue; }
		if (time(NULL) - hs_last_seen >= mins * 60) {
#if HK_ATTACH
			if (hs_attached) {
				hsAttachOff();
				aiSayf(&G_AI, "remote: idle for %ld min, off. :remote on to share again", mins);
				return NULL;
			}
#endif
			fprintf(stderr, "\nhako: remote idle for %ld min, stopped. run it again to reconnect\n", mins);
			fflush(stderr);
			exit(0);
		}
	}
	return NULL;
}

static void *hsAcceptThread(void *arg) {
	int srv = (int)(intptr_t)arg;
	for (;;) {
		int fd = hkCloexec((int)accept(srv, NULL, NULL));
		if (hs_listen_stop) {               /* :remote off woke us with a connect */
			if (fd >= 0) hs_close(fd);
			hs_close(srv);
			return NULL;
		}
		if (fd < 0) {
			int e = hs_errno();
			if (E.debug) fprintf(stderr, "[serve] accept errno=%d\n", e);
			if (e == HS_EINTR || e == HS_ECONNABORTED || e == HS_EWOULDBLOCK) continue;
			if (e == HS_EMFILE) { cl_sleep_ms(100); continue; }
			break;
		}
		pthread_t t;
		if (pthread_create(&t, NULL, hsConnThread, (void *)(intptr_t)fd) != 0) hs_close(fd);
		else pthread_detach(t);
	}
	fprintf(stderr, "hako: listener stopped (error %d)\n", hs_errno());
	return NULL;
}

static int hsOpenListener(void) {
	int srv = hkCloexec((int)socket(AF_INET, SOCK_STREAM, 0));
	if (srv < 0) { fprintf(stderr, "hako: socket failed (error %d)\n", hs_errno()); return -1; }
	int yes = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = htons((uint16_t)HS.port);
	if (inet_pton(AF_INET, HS.bind_addr, &sa.sin_addr) != 1) {
		fprintf(stderr, "hako: bad bind address %s\n", HS.bind_addr);
		hs_close(srv);
		return -1;
	}
	if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		fprintf(stderr, "hako: bind %s:%d failed (error %d)\n", HS.bind_addr, HS.port, hs_errno());
		hs_close(srv);
		return -1;
	}
	listen(srv, 16);
	return srv;
}

static int hsStartThreads(int srv) {
	pthread_t acc;
	if (pthread_create(&acc, NULL, hsAcceptThread, (void *)(intptr_t)srv) != 0) {
		fprintf(stderr, "hako: cannot start listener thread\n");
		hs_close(srv);
		return -1;
	}
	pthread_detach(acc);
	if (hs_remote) {
		const char *iv = getenv("HAKO_REMOTE_IDLE");
		long mins = iv && *iv ? atol(iv) : 120;
		pthread_t idle;
		hs_last_seen = time(NULL);
		if (mins > 0 && pthread_create(&idle, NULL, hsIdleThread, (void *)(intptr_t)mins) == 0)
			pthread_detach(idle);
	}
	return 0;
}

#if HK_ATTACH
static void hsAttachInfo(int *port, const char **token) { *port = HS.port; *token = HS.token; }
static void hsAttachSetPort(int port) { if (port > 0 && port < 65536) HS.port = port; }

/* :remote on. The same listener and gates as --serve --token, inside this session. */
static int hsAttachOn(void) {
	if (hs_attached) return 1;
	if (!hkRandomToken(HS.token, 24, "abcdefghijklmnopqrstuvwxyz0123456789")) return 0;
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd)) || !realpath(cwd, hs_root)) return 0;
	if (pipe(hs_wake) != 0) return 0;
	fcntl(hs_wake[0], F_SETFL, O_NONBLOCK);
	fcntl(hs_wake[1], F_SETFL, O_NONBLOCK);
	hs_listen_stop = 0;
	hs_remote = 1;                     /* before the first request can arrive */
	int srv = hsOpenListener();
	if (srv < 0) {
		hs_remote = 0;
		close(hs_wake[0]); close(hs_wake[1]);
		hs_wake[0] = hs_wake[1] = -1;
		return 0;
	}
	hsRingReset();
	hs_attached = 1;
	if (hsStartThreads(srv) != 0) { hs_attached = hs_remote = 0; return 0; }
	return 1;
}

static void hrStop(void);

static void hsAttachOff(void) {
	if (!hs_attached) return;
	hrStop();
	hs_attached = 0;
	hs_remote = 0;
	hs_listen_stop = 1;
	hsReleaseWaits();
	int s = (int)socket(AF_INET, SOCK_STREAM, 0);    /* wake the accept() so it can exit */
	if (s >= 0) {
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons((uint16_t)HS.port);
		inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
		if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {}
		hs_close(s);
	}
	pthread_mutex_lock(&HS.ring_lock);
	pthread_cond_broadcast(&HS.ring_cond);
	pthread_mutex_unlock(&HS.ring_lock);
	pthread_mutex_lock(&hs_inject_lock);
	while (hs_inject_n > 0) free(hs_inject[--hs_inject_n]);
	pthread_mutex_unlock(&hs_inject_lock);
}

/* Prints text as a QR code, dark on light whatever the theme, two module rows
   per line, with the four-module quiet zone around it. Secret lines: shown
   here only. */
static int hkQrShow(aiData *data, const char *text) {
	hkQr *q = calloc(1, sizeof *q);
	if (!q || !hkQrEncode(q, text) || clTermCols() < q->n + 8) { free(q); return 0; }
	char ink[24], line[512];
	snprintf(ink, sizeof ink, "%s%s", clSeq(0x4000000u | 30), clSeq(0x4000000u | 107));
	int n = q->n, z = 4;
	for (int y = -z; y < n + z; y += 2) {
		size_t o = (size_t)snprintf(line, sizeof line, "%s", ink);
		for (int x = -z; x < n + z && o + 8 < sizeof line; x++) {
			int a = x >= 0 && x < n && y >= 0 && y < n && q->m[y][x];
			int b = x >= 0 && x < n && y + 1 >= 0 && y + 1 < n && q->m[y + 1][x];
			const char *c = a && b ? "\xe2\x96\x88" : a ? "\xe2\x96\x80" : b ? "\xe2\x96\x84" : " ";
			memcpy(line + o, c, strlen(c));
			o += strlen(c);
		}
		snprintf(line + o, sizeof line - o, "%s", clSeq(0x4000000u));
		aiSayfSecret(data, "%s", line);
	}
	free(q);
	return 1;
}

/* ---- :remote over the relay (RELAY.md §4, §7) ----
   The page links this computer once: it binds this device key to the account
   and sends the account key K sealed under a secret from the link URL's
   fragment. Each phone then opens an XXpsk3 session (psk from K); inside it,
   numbered streams carry plain HTTP into hsConnThread through a socketpair,
   so the phone speaks the same /api as the LAN page. Two keys: the computer's
   own (~/.hako/remote, 0600, with K and the account id) is what the page links;
   each project folder then gets its own (its state dir, "remote"), which the
   computer's key binds to the account, so every shared folder is its own code
   session in the list. */
#define HR_PEERS   8
#define HR_STREAMS 32
#define HR_CHUNK   49152
enum { HR_HS = 1, HR_TP = 2, HR_LINK = 3, HR_PAIR = 4 };   /* frame kinds, the first byte */
enum { HR_DATA = 1, HR_EOF = 2, HR_RESET = 3 };      /* stream ops, after the 4-byte id */

typedef struct { int sid, fd; } hrStream;
typedef struct {
	int used, ready;
	unsigned gen;                  /* bumped on every new handshake */
	char id[33];
	hkNoise hs;
	hkNoiseCipher tx, rx;
	hrStream st[HR_STREAMS];
} hrPeer;

static hkRelay hr, hrm;            /* this folder's session; the computer itself */
static hkRelay *volatile hr_cur;    /* whichever is reading, for hrStop */
static hrPeer hr_peers[HR_PEERS];
static char hr_told[16][33];        /* devices the desk was told about, for this :remote on */
static int hr_ntold;
static pthread_mutex_t hr_lock = PTHREAD_MUTEX_INITIALIZER;  /* peers, streams, every send */
static pthread_t hr_thread;
static volatile int hr_on;
static int hr_has_k, hr_linking;
static uint8_t hr_k[32], hr_secret[32];
static uint8_t hr_pair[32];             /* :remote pair's one-use secret, live until hr_pair_until */
static time_t hr_pair_until;
static char hr_acct[17];
static void hrPostName(void);

static const char *hrRelayUrl(void) {
	const char *u = getenv("HAKO_RELAY_URL");
	return u && *u ? u : "https://relay.mithraeum.studio";
}

static const char *hrStudioUrl(void) {
	const char *u = getenv("HAKO_STUDIO_URL");
	return u && *u ? u : "https://hako.mithraeum.studio";
}

static void hrPath(char *out, size_t cap) {
	const char *h = getenv("HOME");
	snprintf(out, cap, "%s/.hako/remote", h ? h : ".");
}

/* key <hex>, k <hex>, acct <id>. A missing key is made here. */
static int hrLoad(void) {
	char path[PATH_MAX], line[160];
	int has_s = 0;
	hrPath(path, sizeof path);
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "key ", 4) && strlen(line) == 68 && !hkHexDec(hrm.s, line + 4, 32)) has_s = 1;
		else if (!strncmp(line, "k ", 2) && strlen(line) == 66 && !hkHexDec(hr_k, line + 2, 32)) hr_has_k = 1;
		else if (!strncmp(line, "acct ", 5) && strlen(line) == 21) snprintf(hr_acct, sizeof hr_acct, "%s", line + 5);
	}
	if (f) fclose(f);
	crypto_wipe(line, sizeof line);
	if (has_s) return 0;
	return hkRandomBytes(hrm.s, 32) ? 0 : -1;
}

/* This folder's session key, in its project state dir. new=1 replaces it. */
static int hrProjectKey(int fresh) {
	char dir[PATH_MAX], path[PATH_MAX + 16], line[80], hs[65];
	if (!hkProjectStateDir(dir, sizeof dir) || !hkFmtFits(path, sizeof path, "%s/remote", dir)) return -1;
	FILE *f = fresh ? NULL : fopen(path, "r");
	int ok = 0;
	if (f && fgets(line, sizeof line, f) && !strncmp(line, "key ", 4) && !hkHexDec(hr.s, line + 4, 32)) ok = 1;
	if (f) fclose(f);
	crypto_wipe(line, sizeof line);
	if (ok) return 0;
	if (!hkRandomBytes(hr.s, 32)) return -1;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return -1;
	hkHexEnc(hs, hr.s, 32);
	char out[80];
	int n = snprintf(out, sizeof out, "key %s\n", hs);
	ok = write(fd, out, (size_t)n) == n && fsync(fd) == 0;
	close(fd);
	crypto_wipe(hs, sizeof hs); crypto_wipe(out, sizeof out);
	return ok ? 0 : -1;
}

static int hrSave(void) {
	char path[PATH_MAX], tmp[PATH_MAX + 8], hs[65], hk[65];
	hrPath(path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (!f) { if (fd >= 0) close(fd); return -1; }
	hkHexEnc(hs, hrm.s, 32);
	fprintf(f, "key %s\n", hs);
	if (hr_has_k) { hkHexEnc(hk, hr_k, 32); fprintf(f, "k %s\nacct %s\n", hk, hr_acct); }
	crypto_wipe(hs, sizeof hs); crypto_wipe(hk, sizeof hk);
	int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
	return 0;
}

/* One sealed stream frame to p. Caller holds hr_lock. */
static int hrSendTp(hrPeer *p, int sid, int op, const uint8_t *d, size_t n) {
	uint8_t *plain = malloc(5 + n), *f = malloc(1 + 5 + n + 16);
	int r = -1;
	if (plain && f && p->ready) {
		plain[0] = (uint8_t)(sid >> 24); plain[1] = (uint8_t)(sid >> 16);
		plain[2] = (uint8_t)(sid >> 8); plain[3] = (uint8_t)sid; plain[4] = (uint8_t)op;
		if (n) memcpy(plain + 5, d, n);
		f[0] = HR_TP;
		if (!hkNoiseSeal(&p->tx, NULL, 0, plain, 5 + n, f + 1)) r = hkRelaySend(&hr, p->id, f, 1 + 5 + n + 16);
	}
	free(plain);
	free(f);
	return r;
}

/* Ends every stream of p (their pumps close the fds) and forgets its session. */
static void hrPeerReset(hrPeer *p) {
	for (int i = 0; i < HR_STREAMS; i++) if (p->st[i].fd > 0) shutdown(p->st[i].fd, SHUT_RDWR);
	crypto_wipe(&p->hs, sizeof p->hs);
	crypto_wipe(&p->tx, sizeof p->tx);
	crypto_wipe(&p->rx, sizeof p->rx);
	p->ready = 0;
	p->gen++;
}

typedef struct { int peer; unsigned gen; int sid, fd; } hrPump;

/* hsConnThread's reply, back to the phone in sealed chunks. */
static void *hrPumpThread(void *arg) {
	hrPump a = *(hrPump *)arg;
	free(arg);
	uint8_t *buf = malloc(HR_CHUNK);
	ssize_t n;
	while (buf && (n = read(a.fd, buf, HR_CHUNK)) > 0) {
		pthread_mutex_lock(&hr_lock);
		hrPeer *p = &hr_peers[a.peer];
		int live = p->used && p->gen == a.gen && hrSendTp(p, a.sid, HR_DATA, buf, (size_t)n) == 0;
		pthread_mutex_unlock(&hr_lock);
		if (!live) break;
	}
	pthread_mutex_lock(&hr_lock);
	hrPeer *p = &hr_peers[a.peer];
	if (p->used && p->gen == a.gen) hrSendTp(p, a.sid, HR_EOF, NULL, 0);
	for (int i = 0; i < HR_STREAMS; i++) if (p->st[i].fd == a.fd) p->st[i].fd = 0;
	pthread_mutex_unlock(&hr_lock);
	close(a.fd);
	free(buf);
	return NULL;
}

/* 1 the first time id connects while remote is on; a reconnect stays quiet. Caller holds hr_lock. */
static int hrFirstSight(const char *id) {
	for (int i = 0; i < hr_ntold && i < 16; i++) if (!strcmp(hr_told[i], id)) return 0;
	snprintf(hr_told[hr_ntold++ % 16], sizeof hr_told[0], "%s", id);
	return 1;
}

static hrPeer *hrPeerFor(const char *id, int make) {
	hrPeer *spare = NULL;
	for (int i = 0; i < HR_PEERS; i++) {
		if (hr_peers[i].used && !strcmp(hr_peers[i].id, id)) return &hr_peers[i];
		if (!hr_peers[i].used && !spare) spare = &hr_peers[i];
	}
	if (!make || !spare) return NULL;
	unsigned gen = spare->gen;
	memset(spare, 0, sizeof *spare);
	spare->used = 1;
	spare->gen = gen + 1;
	snprintf(spare->id, sizeof spare->id, "%s", id);
	return spare;
}

/* XXpsk3, this side answering. Message 1 starts (or restarts) a session; the
   peer's third message finishes it. */
static void hrOnHandshake(const char *from, const uint8_t *m, size_t n) {
	uint8_t out[512], pl[512];
	pthread_mutex_lock(&hr_lock);
	hrPeer *p = hrPeerFor(from, 1);
	if (!p) { pthread_mutex_unlock(&hr_lock); return; }
	if (!p->ready && p->hs.next == 2) {
		hkNoise keep = p->hs;
		if (hkNoiseRead(&p->hs, m, n, pl, sizeof pl) >= 0 && hkNoiseDone(&p->hs)) {
			hkNoiseSplit(&p->hs, &p->tx, &p->rx);
			p->ready = 1;
			int first = hrFirstSight(from);
			pthread_mutex_unlock(&hr_lock);
			if (first) aiSayfSecret(&G_AI, "remote: a device on your account connected");   /* the desk's to know, not the chat's */
			return;
		}
		p->hs = keep;                       /* not message 3: maybe a fresh message 1 */
		crypto_wipe(&keep, sizeof keep);
	}
	hrPeerReset(p);
	memcpy(p->hs.s, hr.s, 32); p->hs.has_s = 1;
	hkSessionPsk(p->hs.psk, hr_k); p->hs.has_psk = 1;
	long w = -1;
	if (hkNoiseInit(&p->hs, "XXpsk3", 0, (const uint8_t *)"hako session", 12) == 0
	    && hkNoiseRead(&p->hs, m, n, pl, sizeof pl) >= 0)
		w = hkNoiseWrite(&p->hs, NULL, 0, out + 1, sizeof out - 1);
	if (w > 0) { out[0] = HR_HS; hkRelaySend(&hr, from, out, (size_t)w + 1); }
	else hrPeerReset(p);
	pthread_mutex_unlock(&hr_lock);
}

static void hrOnTransport(const char *from, const uint8_t *m, size_t n) {
	uint8_t *pl = malloc(n);
	pthread_mutex_lock(&hr_lock);
	hrPeer *p = hrPeerFor(from, 0);
	long len = p && p->ready && pl ? hkNoiseOpen(&p->rx, NULL, 0, m, n, pl) : -1;
	if (len < 5) { pthread_mutex_unlock(&hr_lock); free(pl); return; }
	int sid = (int)((uint32_t)pl[0] << 24 | (uint32_t)pl[1] << 16 | (uint32_t)pl[2] << 8 | pl[3]), op = pl[4];
	hrStream *st = NULL, *spare = NULL;
	for (int i = 0; i < HR_STREAMS; i++) {
		if (p->st[i].fd > 0 && p->st[i].sid == sid) st = &p->st[i];
		else if (p->st[i].fd <= 0 && !spare) spare = &p->st[i];
	}
	if (!st && op == HR_DATA && spare) {
		int sv[2];
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
			hkCloexec(sv[0]); hkCloexec(sv[1]);
			pthread_t t;
			hrPump *a = malloc(sizeof *a);
			if (a) { a->peer = (int)(p - hr_peers); a->gen = p->gen; a->sid = sid; a->fd = sv[1]; }
			if (a && pthread_create(&t, NULL, hsConnThread, (void *)((intptr_t)sv[0] | HS_TRUSTED)) == 0) {
				pthread_detach(t);
				if (pthread_create(&t, NULL, hrPumpThread, a) == 0) {
					pthread_detach(t);
					spare->sid = sid;
					spare->fd = sv[1];
					st = spare;
				} else { free(a); shutdown(sv[1], SHUT_RDWR); close(sv[1]); }
			} else { free(a); close(sv[0]); close(sv[1]); }
		}
	}
	/* the pump may close its fd once we let go of the lock: write through a dup */
	int fd = st && op == HR_DATA ? dup(st->fd) : -1;
	if (st && op != HR_DATA) shutdown(st->fd, op == HR_EOF ? SHUT_WR : SHUT_RDWR);
	pthread_mutex_unlock(&hr_lock);
	for (long o = 5; fd >= 0 && o < len; ) {
		ssize_t w = write(fd, pl + o, (size_t)(len - o));
		if (w <= 0) { if (w < 0 && errno == EINTR) continue; break; }
		o += w;
	}
	if (fd >= 0) close(fd);
	free(pl);
}

/* The page's link frame: nonce(24) mac(16) then K(32) and the account id(16),
   sealed under a key from the secret in the link URL. */
static void hrOnLink(const uint8_t *m, size_t n) {
	uint8_t pt[48];
	if (!hr_linking || n != 24 + 16 + 48 || hkLinkOpen(pt, hr_secret, m)) return;
	memcpy(hr_k, pt, 32);
	snprintf(hr_acct, sizeof hr_acct, "%.16s", (const char *)pt + 32);
	crypto_wipe(pt, sizeof pt);
	crypto_wipe(hr_secret, sizeof hr_secret);
	hr_has_k = 1;
	hr_linking = 0;
	if (hrSave()) aiAddHistory(&G_AI, "remote: linked, but ~/.hako/remote could not be written");
	aiSayf(&G_AI, "remote: linked. on your phone, open %s/#signin", hrStudioUrl());
	hkLogMessage("system", "remote: this computer was linked to an account");
}

/* This session's name for the account's list, sealed under K:
   "code:<host>\t<folder>", then "\t<chat name>" once it has one. Only a device
   holding K can read any of it; the relay stores the sealed bytes (<= 152 of
   text: it takes 256 base64 characters). */
static void hrPostName(void) {
	char host[128] = "computer", text[160];
	memset(text, 0, sizeof text);
	uint8_t key[32], blob[24 + 16 + 160];
	if (gethostname(host, sizeof host) != 0) snprintf(host, sizeof host, "computer");
	host[sizeof host - 1] = '\0';
	char *dot = strchr(host, '.');
	if (dot) *dot = '\0';                                             /* "imac.local" -> "imac" */
	char cwd[PATH_MAX] = "";
	if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
	char nm[64] = "";
	snprintf(nm, sizeof nm, "%.48s", E.session_name ? E.session_name : "");
	{   /* cut at 48 bytes: never half a character */
		size_t L = strlen(nm), s = L;
		while (s && ((unsigned char)nm[s - 1] & 0xC0) == 0x80) s--;
		if (s && (unsigned char)nm[s - 1] >= 0xC0) {
			unsigned char lb = (unsigned char)nm[s - 1];
			if (L - (s - 1) < (size_t)(lb >= 0xF0 ? 4 : lb >= 0xE0 ? 3 : 2)) nm[s - 1] = '\0';
		}
	}
	for (char *q = nm; *q; q++) if (*q == '\t' || *q == '\n' || *q == '\r') *q = ' ';
	size_t keep = nm[0] ? 64 : 80;
	const char *dir = strlen(cwd) > keep ? cwd + strlen(cwd) - keep : cwd;     /* the tail names it */
	int n = nm[0] ? snprintf(text, sizeof text, "code:%.32s\t%s\t%s", host, dir, nm)
	              : snprintf(text, sizeof text, "code:%.60s\t%s", host, dir);
	if (n >= (int)sizeof text) n = (int)sizeof text - 1;
	if (n <= 0 || !hkRandomBytes(blob, 24)) return;
	hkNameKey(key, hr_k);
	crypto_aead_lock(blob + 40, blob + 24, key, blob, NULL, 0, (const uint8_t *)text, (size_t)n);
	crypto_wipe(key, sizeof key);
	char *b = clB64Encode(blob, 40 + (size_t)n);
	if (!b) return;
	size_t o = 0;
	for (char *c = b; *c && *c != '='; c++) b[o++] = *c == '+' ? '-' : *c == '/' ? '_' : *c;
	b[o] = '\0';
	free(hkRelayCall(&hr, "/v1/name", b));
	free(b);
}

/* :name while linked: the account's list shows the new name. In the
   background, since the relay may take seconds to answer. */
static void *hrRenameThread(void *a) { (void)a; hkBlockWinch(); hrPostName(); return NULL; }
static void hrRenamed(void) {
	if (!hr_on || !hr_has_k) return;
	pthread_t t;
	if (pthread_create(&t, NULL, hrRenameThread, NULL) == 0) pthread_detach(t);
}

/* A browser in the account opened :remote pair's code and asks for K: the
   answer is sealed under the code's secret, which the relay never saw. One use. */
static void hrOnPair(const char from[33]) {
	uint8_t pt[48], f[1 + 88];
	pthread_mutex_lock(&hr_lock);
	int open = hr_has_k && hr_pair_until && time(NULL) <= hr_pair_until;
	if (open) {
		memcpy(pt, hr_k, 32);
		memset(pt + 32, 0, 16);
		memcpy(pt + 32, hr_acct, strnlen(hr_acct, 16));
		open = hkLinkSeal(f + 1, hr_pair, pt) == 0;
		crypto_wipe(hr_pair, sizeof hr_pair);
		hr_pair_until = 0;
	}
	pthread_mutex_unlock(&hr_lock);
	crypto_wipe(pt, sizeof pt);
	if (!open) return;
	f[0] = HR_PAIR;
	hkRelaySend(&hr, from, f, sizeof f);
	aiAddHistory(&G_AI, "remote: paired, that browser now holds your account key");
	hkLogMessage("system", "remote: a browser was paired");
}

/* :remote pair: a QR code of <studio>/#pair=<this folder's device id>.<secret>
   for a browser that signed in without a passkey that unlocks K (email, SSO,
   no prf). It signs in, asks this session, and gets K back sealed. */
static void hrPairPrompt(aiData *data) {
	char id[33], sec[65], url[512];
	pthread_mutex_lock(&hr_lock);
	int up = hr.id[0] && hr.acct[0] && hkRandomBytes(hr_pair, 32);
	if (up) {
		hr_pair_until = time(NULL) + 600;
		snprintf(id, sizeof id, "%s", hr.id);
		hkHexEnc(sec, hr_pair, 32);
	}
	pthread_mutex_unlock(&hr_lock);
	if (!up) { aiAddHistory(data, "remote: not connected to your account yet, try again in a moment"); return; }
	int fit = hkFmtFits(url, sizeof url, "%s/#pair=%s.%s", hrStudioUrl(), id, sec);
	crypto_wipe(sec, sizeof sec);
	aiAddHistory(data, "remote: scan this on the device to add, then sign in there (one use, 10 minutes):");
	if (!fit || !hkQrShow(data, url)) aiAddHistory(data, "  (no room for the code here: widen this terminal, or open the link on that device)");
	if (fit) aiSayfSecret(data, "  %s", url);
	crypto_wipe(url, sizeof url);
}

/* Shows the link page once per :remote on, while not linked: as a QR code for
   a phone signed in to hako (no browser needed here), and opened here when
   this computer has a screen. */
static void hrLinkPrompt(void) {
	char pub[65], sec[65], url[512];
	if (hr_linking) return;
	if (!hkRandomBytes(hr_secret, 32)) return;
	hkHexEnc(pub, hrm.s_pub, 32);
	hkHexEnc(sec, hr_secret, 32);
	if (!hkFmtFits(url, sizeof url, "%s/#link=%s.%s", hrStudioUrl(), pub, sec)) return;
	hr_linking = 1;
	int here = !getenv("HAKO_NO_BROWSER") && hkHasDesktop();
	aiAddHistory(&G_AI, here ? "remote: link this computer to your account (once): sign in on the page that opened, or scan this with a phone signed in to hako"
	                         : "remote: link this computer to your account (once): scan this with a phone signed in to hako, or open the link");
	if (!hkQrShow(&G_AI, url)) aiAddHistory(&G_AI, "  (no room for the code here: widen this terminal)");
	aiSayfSecret(&G_AI, "  %s", url);
	if (here) clOpenUrl(url);
	crypto_wipe(sec, sizeof sec);
	crypto_wipe(url, sizeof url);
}

/* Frames for r until its stream ends (or, on the computer's key, a link). */
static void hrRead(hkRelay *r, uint8_t *buf, size_t cap) {
	char from[33];
	long n;
	hr_cur = r;
	while (hr_on && (n = hkRelayRecv(r, from, buf, cap)) != -1) {
		if (r == &hrm) {
			if (n > 1 && buf[0] == HR_LINK) hrOnLink(buf + 1, (size_t)n - 1);
			if (hr_has_k) break;
		} else if (n == -2) {
			pthread_mutex_lock(&hr_lock);
			hrPeer *p = hrPeerFor(from, 0);
			if (p) { hrPeerReset(p); p->used = 0; }
			pthread_mutex_unlock(&hr_lock);
		} else if (n == 1 && buf[0] == HR_PAIR) hrOnPair(from);
		else if (n > 1 && buf[0] == HR_HS) hrOnHandshake(from, buf + 1, (size_t)n - 1);
		else if (n > 1 && buf[0] == HR_TP) hrOnTransport(from, buf + 1, (size_t)n - 1);
	}
	hr_cur = NULL;
	hkRelayClose(r);
}

static void *hrThread(void *arg) {
	(void)arg;
	static uint8_t buf[300 * 1024];
	int wait = 1, said = 0;
	while (hr_on) {
		/* the computer's key links it (once), then vouches for this folder's key */
		int up = hkRelaySignIn(&hrm) == 0;
		if (up && (!hr_has_k || !hrm.acct[0])) {
			if (hkRelayOpen(&hrm) == 0) { hrLinkPrompt(); hrRead(&hrm, buf, sizeof buf); }
			else hkRelayClose(&hrm);
			continue;
		}
		char pub[65], *r = NULL;
		if (up) { hkHexEnc(pub, hr.s_pub, 32); r = hkRelayCall(&hrm, "/v1/bind", pub); }
		int bound = r && !strncmp(r, "ok", 2);
		if (r && !bound && hrProjectKey(1) == 0) crypto_x25519_public_key(hr.s_pub, hr.s);   /* held by another account */
		free(r);
		if (!bound || hkRelaySignIn(&hr) || !hr.acct[0] || hkRelayOpen(&hr)) {
			hkRelayClose(&hr);
			if (!said++) aiAddHistory(&G_AI, "remote: can't reach your account right now, still trying (the here link works)");
			for (int i = 0; i < wait * 10 && hr_on; i++) cl_sleep_ms(100);
			if (wait < 30) wait *= 2;
			continue;
		}
		wait = 1;
		hrPostName();
		hrRead(&hr, buf, sizeof buf);
		pthread_mutex_lock(&hr_lock);
		for (int i = 0; i < HR_PEERS; i++) if (hr_peers[i].used) { hrPeerReset(&hr_peers[i]); hr_peers[i].used = 0; }
		pthread_mutex_unlock(&hr_lock);
	}
	return NULL;
}

static int hrLinked(void) { return hr_has_k; }

static int hrStart(void) {
	if (hr_on) return 1;
	hr_ntold = 0;
	memset(&hr, 0, sizeof hr);
	memset(&hrm, 0, sizeof hrm);
	snprintf(hr.url, sizeof hr.url, "%s", hrRelayUrl());
	snprintf(hrm.url, sizeof hrm.url, "%s", hrRelayUrl());
	if (hrLoad() || hrSave() || hrProjectKey(0)) return 0;
	crypto_x25519_public_key(hr.s_pub, hr.s);
	crypto_x25519_public_key(hrm.s_pub, hrm.s);
	hr_on = 1;
	if (pthread_create(&hr_thread, NULL, hrThread, NULL) != 0) { hr_on = 0; return 0; }
	return 1;
}

static void hrStop(void) {
	if (!hr_on) return;
	hr_on = 0;
	hkRelay *r = hr_cur;
	if (r) hkRelayStop(r);
	pthread_join(hr_thread, NULL);
	crypto_wipe(hr_secret, sizeof hr_secret);
	hr_linking = 0;
}
#endif

static int clServeMode(aiData *data, const char *dir) {
	static char web_abs[PATH_MAX];
	if (HS.web_dir && realpath(HS.web_dir, web_abs)) HS.web_dir = web_abs;

	if (dir && *dir && chdir(dir) != 0) {
		fprintf(stderr, "hako: cannot open %s: %s\n", dir, strerror(errno));
		return 1;
	}
	if (dir && *dir) {
		int cli_yolo = E.ai_auto_approve, cli_debug = E.debug;
		hkLoadSession();
		clCredsRestoreFor(hkProviderLabel());
		hkHealEndpoint();
		if (cli_yolo) E.ai_auto_approve = 1;
		if (cli_debug) E.debug = 1;
	}

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "hako: winsock startup failed\n");
		return 1;
	}
#endif
	int srv = hsOpenListener();
	if (srv < 0) return 1;

	if (strcmp(HS.bind_addr, "127.0.0.1") != 0 && !HS.token[0]) {
		fprintf(stderr,
			"\n  WARNING: %s exposes an unauthenticated agent to the network.\n"
			"  Anyone who can reach this port can run tools as you.\n"
			"  Add --token to require a secret on every request.\n\n", HS.bind_addr);
#ifdef __APPLE__
		fprintf(stderr,
			"  macOS: if a device cannot connect, this binary needs a signature and\n"
			"  one firewall allow — run `make sign` and follow what it prints.\n\n");
#endif
	}
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
	hs_remote = HS.token[0] != '\0';
	if (hs_remote && !realpath(cwd, hs_root)) snprintf(hs_root, sizeof(hs_root), "%s", cwd);
	if (hs_remote)
		fprintf(stderr,
			"\n  remote: every tool asks%s, \"always\" answers count once, and the\n"
			"  phone stays inside %s\n\n", E.ai_auto_approve ? " (auto mode is ignored)" : "", hs_root);
	else if (E.ai_auto_approve)
		fprintf(stderr,
			"\n  WARNING: --yolo approves every tool call, including shell and file\n"
			"  writes, with no prompt.\n\n");

	printf("hako studio v%s — http://%s:%d\n", HAKO_VERSION, HS.bind_addr, HS.port);
#ifdef __APPLE__
	if (strcmp(HS.bind_addr, "127.0.0.1") != 0)
		printf("  note     if a phone cannot connect, this binary is unsigned — "
		       "run `make sign` (macOS drops inbound LAN to unsigned binaries)\n");
#endif
	printf("  dir      %s\n", cwd);
	printf("  provider %s · model %s\n",
	       hkProviderLabel(), E.ai_model ? E.ai_model : "—");
#ifdef HS_EMBEDDED
	printf("  ui       %s\n", HS.web_dir ? HS.web_dir : "embedded");
#else
	printf("  ui       %s\n", HS.web_dir ? HS.web_dir : "none (API only — make WEB=1, or --web <dir>)");
#endif
	if (HS.token[0]) {
		printf("  open     http://%s:%d/?t=%s\n", HS.bind_addr, HS.port, HS.token);
	}
	if (strcmp(HS.bind_addr, "127.0.0.1") != 0) hsPrintLanUrls();
	fflush(stdout);

	if (hsStartThreads(srv) != 0) return 1;

	clPipeBegin(data);

	for (;;) {
		char *line = hsQueuePop();
		if (!line) continue;
		HS.busy = 1;
		int quit = clPipeHandleLine(data, line);
		HS.busy = 0;
		E.interrupt = 0;
		free(line);
		if (quit) break;
	}
	hs_close(srv);
	return 0;
}
#endif

static int hkHealEndpoint(void) {
	if (!E.ai_endpoint || !*E.ai_endpoint) return 0;
	int local_ep = strstr(E.ai_endpoint, "hakm://") != NULL;
	int ollama_ep = strstr(E.ai_endpoint, "11434") != NULL;
	int wants_local = (E.ai_provider_type == AI_PROVIDER_MITHRAEUM);
	int wants_ollama = HK_IS_OLLAMA_WIRE(E.ai_provider_type) && !wants_local;

	int mismatched = (local_ep && !wants_local) || (ollama_ep && !wants_ollama && !wants_local);
	if (!mismatched) return 0;

	const char *fix = hkProviderDefaultEndpoint(hkProviderName(E.ai_provider_type));
	free(E.ai_endpoint);
	E.ai_endpoint = fix ? strdup(fix) : NULL;
	hkSaveSession();
	return 1;
}

static int clDetectKoiDefault(void) {
#ifdef HAKO_WASM
	return 0;
#else
	if (E.ai_provider_type != AI_PROVIDER_NONE) return 0;
	if (E.ai_api_key || E.ai_oauth_refresh) return 0;

	{
		const char *home = getenv("HOME"); if (!home) home = ".";
		char mdir[1024];
		snprintf(mdir, sizeof(mdir), "%s/.hako/models", home);
		char koi[128]={0}, sho[128]={0}, koi_s[128]={0}, sho_s[128]={0};
		DIR *md = opendir(mdir);
		if (md) {
			struct dirent *de;
			while ((de = readdir(md)) != NULL) {
				if (de->d_name[0] == '.') continue;
				char w[1300]; struct stat st;
				if (snprintf(w, sizeof(w), "%s/%s/%s.mlf2", mdir, de->d_name, de->d_name) >= (int)sizeof(w))
					continue;
				if (stat(w, &st) != 0) continue;
				const char *tag = de->d_name;
				int is_ver = (strstr(tag, "-v") != NULL);
				if (strstr(tag, "hako-koi")) {
					if (is_ver) { if (!koi[0]) hkFmtFits(koi, sizeof(koi), "%s", tag); }
					else if (!koi_s[0]) hkFmtFits(koi_s, sizeof(koi_s), "%s", tag);
				} else if (strstr(tag, "hako-sho")) {
					if (is_ver) { if (!sho[0]) hkFmtFits(sho, sizeof(sho), "%s", tag); }
					else if (!sho_s[0]) hkFmtFits(sho_s, sizeof(sho_s), "%s", tag);
				}
			}
			closedir(md);
		}
		const char *lpick = koi[0] ? koi : koi_s[0] ? koi_s : sho[0] ? sho : sho_s[0] ? sho_s : NULL;
		if (lpick) {
			hkApplyProviderAlias("mithraeum");
			free(E.ai_endpoint); E.ai_endpoint = strdup("hakm://subprocess");
			free(E.ai_model);    E.ai_model    = strdup(lpick);
			hkSaveSession();
			if (isatty(STDOUT_FILENO)) {
				const char *M = E.color_enabled ? TH_META   : "";
				{ char nb[160];
				  snprintf(nb, sizeof(nb), "%s detected locally, running via the hakm subprocess (no ollama).", lpick);
				  clPutWrapped2(M, "  ", "  ", nb); }
			}
			return 1;
		}
	}

	FILE *fp = popen("ollama list 2>/dev/null", "r");
	if (!fp) return 0;
	char line[512];
	char koi[128] = {0}, sho[128] = {0}, koi_b[128] = {0}, sho_b[128] = {0};
	while (fgets(line, sizeof(line), fp)) {
		const char *name = line;
		while (*name == ' ' || *name == '\t') name++;
		char tag[128] = {0};
		int i = 0;
		while (i < (int)sizeof(tag)-1 && name[i] && name[i] != ' ' && name[i] != '\t' && name[i] != '\n') {
			tag[i] = name[i]; i++;
		}
		tag[i] = '\0';
		if (!tag[0]) continue;
		int is_ver = (strstr(tag, "-v") != NULL);
		if (strstr(tag, "hako-koi")) {
			if (is_ver) { if (!koi[0]) snprintf(koi, sizeof(koi), "%s", tag); }
			else if (!koi_b[0]) snprintf(koi_b, sizeof(koi_b), "%s", tag);
		} else if (strstr(tag, "hako-sho")) {
			if (is_ver) { if (!sho[0]) snprintf(sho, sizeof(sho), "%s", tag); }
			else if (!sho_b[0]) snprintf(sho_b, sizeof(sho_b), "%s", tag);
		}
	}
	pclose(fp);
	const char *pick = koi[0] ? koi : koi_b[0] ? koi_b : sho[0] ? sho : sho_b[0] ? sho_b : NULL;	if (!pick) return 0;
	hkApplyProviderAlias("mithraeum");
	free(E.ai_endpoint); E.ai_endpoint = strdup("http://localhost:11434");
	free(E.ai_model);    E.ai_model    = strdup(pick);
	hkSaveSession();
	if (isatty(STDOUT_FILENO)) {
		const char *M = E.color_enabled ? TH_META   : "";
		{ char nb[160];
	  snprintf(nb, sizeof(nb), "%s detected, defaulted to local hako via mithraeum provider.", pick);
	  clPutWrapped2(M, "  ", "  ", nb); }
	}
	return 1;
#endif
}

static void clFirstRunWizard(aiData *data) {
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
	if (E.ai_provider_type != AI_PROVIDER_NONE || E.ai_api_key || E.ai_oauth_refresh) return;

	const char *R = E.color_enabled ? ANSI_RESET : "";
	const char *M = E.color_enabled ? TH_META : "";
	const char *A = E.color_enabled ? TH_ACCENT : "";

	printf("\n  %sfirst run.%s %ssign in:%s\n", A, R, M, R);
	/* The :login menu's own rows, plus the two ways to start without an account. */
	char rows[HK_NSIGNIN + 2][112];
	const char *items[HK_NSIGNIN + 2];
	int n = 0;
	for (int i = 0; i < HK_NSIGNIN; i++, n++) {
		snprintf(rows[n], sizeof rows[n], "%-15s %s", HK_SIGNIN[i].name, HK_SIGNIN[i].what);
		items[n] = rows[n];
	}
	snprintf(rows[n], sizeof rows[n], "%-15s %s", "Ollama", "local models, no account"); items[n] = rows[n]; n++;
	snprintf(rows[n], sizeof rows[n], "%-15s %s", "skip for now", ":login any time"); items[n] = rows[n]; n++;
	int sel = E.color_enabled ? clPopupSelect("sign in", items, n, 0, NULL) : -1;
	if (sel < 0 && !E.color_enabled) {
		for (int i = 0; i < n; i++) printf("  %s[%d] %s%s\n", M, i + 1, items[i], R);
		char buf[32];
		int got = clReadLineRaw("  pick [1]: ", buf, sizeof(buf));
		if (got == -2 || got == -1) { E.interrupt = 0; printf("\n"); return; }
		sel = ((got > 0) ? atoi(buf) : 1) - 1;
	}
	if (sel >= 0 && sel < HK_NSIGNIN) {
		char cmd[48];
		snprintf(cmd, sizeof cmd, ":login %s", HK_SIGNIN[sel].id);
		hkHandleSlash(data, cmd);
	} else if (sel == HK_NSIGNIN) {
		hkApplyProviderAlias("ollama");
		free(E.ai_endpoint); E.ai_endpoint = strdup("http://localhost:11434");
		if (!E.ai_model) E.ai_model = strdup("llama3.2");
		hkSaveSession();
		printf("  %sollama configured. start it: ollama serve%s\n", M, R);
	} else {
		clPutWrapped2(M, "  ", "  ", "skipped. :login to sign in \xC2\xB7 :login api for a key.");
	}
	printf("\n");
	fflush(stdout);
}

/* Reprint a resumed conversation: display only, straight from what was already
   saved, so it costs nothing to show. */
static void hkReplayTranscript(aiData *data) {
	if (!data || E.pipe_mode || E.output_json || !data->history_count) return;
	int want = cl_bar_want;
	cl_bar_want = 0;
	E.last_role_shown = HK_ROLE_SYSTEM;
	cl_in_codefence = 0;
	for (int i = 0; i < data->history_count; i++) hkRenderHistoryEntry(data, i);
	cl_in_codefence = 0;
	cl_bar_want = want;
	fflush(stdout);
}

/* Startup is the one part of a run a phone cannot watch. */
static long hk_boot_t0 = 0;

static void hkBootMark(const char *phase) {
	if (!E.debug) return;
	long now = hk_time_ms();
	int first = 0;
	if (!hk_boot_t0) { hk_boot_t0 = now; first = 1; }
	long ms = now - hk_boot_t0;
	fprintf(stderr, "[boot] +%ldms %s\n", ms, phase);
	const char *home = getenv("HOME");
	if (!home || !*home) return;
	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s/.hako/startup.log", home) >= (int)sizeof(path)) return;
	FILE *f = fopen(path, first ? "w" : "a");
	if (!f) return;
	if (first) fprintf(f, "--- hako %s boot %ld ---\n", HAKO_VERSION, hk_time_unix());
	fprintf(f, "+%ldms %s\n", ms, phase);
	fclose(f);
}

static void hkRestoreTerm(void) {
	clBarTeardown();
	clDisableRaw();
	fflush(stdout);
}

static int clRepl(aiData *data) {
	g_main_thread = pthread_self();
	g_main_thread_set = 1;
	clWinchInstall();
	hkBootMark("repl entered");
	hkHealLocalModel();
	hkBootMark("heal local model");
	hkLoadSkills(data);      /* warm the prompt cache before the splash blocks */
	hkBootMark("skills loaded");
	hkPrewarmStart(data);    /* spawn the engine and prefill while the splash is up */
	hkBootMark("prewarm started");
	clSplash();
	clBanner(data);
	hkBootMark("banner");
	if (!clDetectKoiDefault()) clFirstRunWizard(data);
	hkBootMark("local-model probe + first-run wizard");
	clStartupMenu(data);
	hkRcProjectLoad(data);   /* the folder's own .hakorc, now that it may be trusted */
	hkUpdateCheckStart();
	hkBootMark("startup menu");
	if (E.session_resumed && data->history_count == 0)
		hkLoadHistoryTail(data, AI_HISTORY_MAX, 40);
	hkBootMark("history tail");
	hkReplayTranscript(data);
	hkBootMark("transcript replayed — ready");

	if (!E.pipe_mode && E.color_enabled && data->history_count == 0
		&& E.ai_provider_type == AI_PROVIDER_NONE
		&& !E.ai_api_key && !E.ai_oauth_refresh && !E.ai_endpoint) {
		printf("\n  %sno provider configured.%s\n", ANSI_TOOL, ANSI_RESET);
		clPutWrapped2("", "  ", "  ",
		              ":providers to browse \xC2\xB7 :login <name> to authenticate "
		              "\xC2\xB7 :login ollama for local-only");
		putchar('\n');
		fflush(stdout);
	}

	char line[4096];
	const char *prompt_color = "\x1b[1m> \x1b[0m";
	const char *prompt_plain = "> ";

	while (1) {
		if (E.last_role_shown == HK_ROLE_AI) E.last_role_shown = HK_ROLE_SYSTEM;
		cl_bar_want = 1;
		if (!cl_bar_h) clBarShow(data);
		const char *prompt = E.color_enabled ? prompt_color : prompt_plain;
		int n, from_remote = 0;
#if HK_ATTACH
		if (hs_attached && hkInjectTake(line, sizeof(line))) {
			n = (int)strlen(line);
			from_remote = 1;                 /* its images come by /api/image, never by a path on this disk */
			hkLogMessage("system", "remote: the next line came from a remote device");
		} else {
			hk_line_wait = 1;
			n = clReadLineRaw(prompt, line, sizeof(line));
			hk_line_wait = 0;
		}
#else
		n = clReadLineRaw(prompt, line, sizeof(line));
#endif
		int input_rows = cl_redraw_oldrows > 0 ? cl_redraw_oldrows : 1;
		(void)input_rows;
		if (n == -4) {                       /* shift-tab cycles the mode */
			if      (E.ai_plan_mode)    { E.ai_plan_mode = 0; E.ai_auto_approve = 0; }
			else if (E.ai_auto_approve) { E.ai_plan_mode = 1; E.ai_auto_approve = 0; }
			else                        { E.ai_auto_approve = 1; }
			hkSaveSession();
			hkPromptInvalidate();
			clBarHide();
			clBarShow(data);                 /* the mode line just changed */
			continue;
		}
		/* Take the bar down so what follows prints where the bar was: the sent
		   message, then the answer, then the bar again underneath it. */
		if (n != -4) { cl_field_rows = 1; clBarHide(); }
		if (n == -1) { break; }
		if (n == -2) { E.interrupt = 0; hkVoiceStop(); continue; }
		if (n == -5 || n == -6) { cl_turn_done = 0; hk_need_stdin = 0; continue; }   /* a late wake-up, not a line */
		if (n == -3) {                       /* ctrl-o: expand / collapse all */
			hkExpandToggle(data);
			continue;
		}
		if (n == -7) {                       /* ctrl-g: the line continues in $EDITOR */
			char *seed = cl_preset_input; cl_preset_input = NULL;
			char *body = clComposeInEditor(seed);
			if (body) cl_preset_input = body;     /* edited text back in the field */
			else cl_preset_input = seed, seed = NULL;   /* emptied or aborted: keep what was there */
			free(seed);
			clRedrawReset();
			continue;
		}
		if (n == 0) continue;

		if (line[0] == '/' || line[0] == ':') {
			if (hkIsMenuCommand(line)) hk_echo_pending = line;   /* echoed only if it prints */
			else {
				clPrintRoleLine(HK_ROLE_USER, line);
				aiPushHistoryStore(data, line, HK_ROLE_USER);    /* on screen ⇒ in history */
				if (hs_attached) clPipeEmitMsg("user", line);
			}
			clOutBegin();
			int r = hkHandleSlash(data, line);
			hk_echo_pending = NULL;
			clOutEnd(data);
			if (hs_attached) { clPipeEmitState(); clPipeEmitDone(NULL); }
			if (r == 2) break;
			if (r != 3) continue;
			snprintf(line, sizeof(line), "%s", hk_slash_prompt);   /* /<skill> <text>: the text is the turn */
		}

		/* Wipe the composing area, then print what was written as part of the
		   conversation. */
		char *imaged = from_remote ? NULL : hkImagesFromLine(data, line);
		if (imaged) { snprintf(line, sizeof(line), "%s", imaged); free(imaged); }
		clPrintRoleLine(HK_ROLE_USER, line);
		aiPushHistoryStore(data, line, HK_ROLE_USER);
		if (hs_attached) clPipeEmitMsg("user", line);
		hkLogMessage("user", line);
		E.session_turn_count++;
		hkSaveSession();

		free(data->current_prompt);
		data->current_prompt = strdup(line);

		if (E.ai_provider_type == AI_PROVIDER_NONE) {
			aiAddHistory(data, "Set ai_provider in ~/.hakorc or /provider <name>");
			continue;
		}

		hkVoiceStop();                       /* a new turn ends the last one's reading */
		aiWorkerSend(data);
		clComposeWhileBusy(data, prompt);
		clWaitWorker(data);
		hkVoiceAfterTurn(data);

		if (E.interrupt) {
			E.interrupt = 0;
			aiAddHistory(data, "(interrupted)");
		}

		if (hk_quit_requested) break;
		/* Whatever was typed during the turn now becomes the next one. */
		while (hk_queue_n > 0 && !E.interrupt && !hk_quit_requested) {
			char *next = hk_queue[0];
			memmove(hk_queue, hk_queue + 1, sizeof(hk_queue[0]) * (size_t)(--hk_queue_n));
			if (next[0] == ':' || next[0] == '/') {      /* a command that waited for the turn */
				clOutBegin();
				int r = hkHandleSlash(data, next);
				clOutEnd(data);
				free(next);
				if (r == 2) { hk_quit_requested = 1; break; }
				if (r == 3 && hk_queue_n < HK_QUEUE_MAX) {   /* its text goes next */
					memmove(hk_queue + 1, hk_queue, sizeof(hk_queue[0]) * (size_t)hk_queue_n);
					hk_queue[0] = strdup(hk_slash_prompt);
					hk_queue_n++;
				}
				continue;
			}
			char *imaged = hkImagesFromLine(data, next);
			if (imaged) { free(next); next = imaged; }
			clPrintRoleLine(HK_ROLE_USER, next);
			aiPushHistoryStore(data, next, HK_ROLE_USER);
			if (hs_attached) clPipeEmitMsg("user", next);
			hkLogMessage("user", next);
			E.session_turn_count++;
			hkSaveSession();
			free(data->current_prompt);
			data->current_prompt = next;      /* takes ownership */
			hkVoiceStop();
			aiWorkerSend(data);
			clComposeWhileBusy(data, prompt);
			clWaitWorker(data);
			hkVoiceAfterTurn(data);
			if (E.interrupt) { E.interrupt = 0; aiAddHistory(data, "(interrupted)"); }
		}
		if (hk_quit_requested) break;
	}

	return 0;
}

static void clUsage(void) {
	printf("hako v%s — the hako agent (hako-code)\n", HAKO_VERSION);
	printf("usage: hako [options]\n");
	printf("  -p <prompt>     one-shot prompt, exit when done ('-' reads it from stdin)\n");
	printf("  --output-format json|text   machine-readable result on stdout (with -p)\n");
	printf("  --bench <suite.jsonl>       score tool-call reliability, JSON to stdout\n");
	printf("  --update        check GitHub for latest release, replace self if newer\n");
	printf("  --update-force  re-download latest even if same version\n");
	printf("  --anim <name>   pin animation: braille|dots|bar|pulse|bounce|ghost|arrows|blocks|pump\n");
	printf("  --no-color      disable ANSI color/animation\n");
	printf("  --no-splash     skip the startup splash (show_splash=0 in ~/.hakorc)\n");
	printf("  --pipe          JSONL I/O mode for hako editor integration\n");
	printf("  --serve         hako studio: same agent, browser UI on http://127.0.0.1:8787\n");
	printf("  --port N        --serve port (default 8787)\n");
	printf("  --bind ADDR     --serve bind address (default 127.0.0.1; LAN exposes the agent)\n");
	printf("  --lan           --serve to this network — prints the URL to open on a phone\n");
	printf("  --token [VAL]   require a token on every request; generated if VAL is omitted\n");
	printf("  --web DIR       --serve UI from DIR/index.html instead of the built-in bundle\n");
	printf("  --dir PATH      open PATH as the project (--serve)\n");
	printf("  --compact       slim banner only\n");
	printf("  --yolo          auto-approve every tool call (skip permission prompts)\n");
	printf("  --debug         dump raw API responses to stderr\n");
	printf("  -h --help       this help\n");
	printf("  -v --version    print version\n");
	printf("config: one rc for hako, hake and the studio: $HAKORC, ~/.hakorc or ~/.config/hako/hakorc\n");
	printf("  (the first that exists), then ./.hakorc once the folder is trusted. key=value:\n");
	printf("  theme, ai_provider, ai_model, voice, update_check, ...; code.<key> is hako's own\n");
	printf("  state: ~/.hako/ (per-project state, credentials, session log)\n");
}

int main(int argc, char **argv) {
	static char outbuf[1 << 16];
	setvbuf(stdout, outbuf, _IOFBF, sizeof outbuf);
	clInitConfig();
	clLoadRc();
	hkLoadSession();
	clApplyEnv();

	clCredsLoad();
	if (E.ai_api_key || E.ai_oauth_refresh) {
		clCredsCaptureCurrent();
		clCredsSave();
	}
	clCredsRestoreFor(hkProviderLabel());
	hkHealEndpoint();
	hkSaveSession();

	const char *one_shot = NULL;
	const char *bench_suite = NULL;
	const char *serve_bind = NULL, *serve_web = NULL, *serve_dir = NULL, *serve_token = NULL;
	int serve_port = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			clUsage(); return 0;
		}
		if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
			printf("hako v%s (build %s)\n", HAKO_VERSION, HK_BUILD_STAMP); return 0;
		}
		if (strcmp(a, "--update") == 0)       { return clCmdUpdate(0); }
		if (strcmp(a, "--update-force") == 0) { return clCmdUpdate(1); }
		if (strcmp(a, "-p") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "-p needs argument\n"); return 2; }
			one_shot = argv[++i];
			/* `-p -` reads the prompt from stdin, so a caller does not have to
			   fit a whole task into one shell-quoted argument. */
			if (!strcmp(one_shot, "-")) {
				size_t cap = 4096, len = 0;
				char *buf = malloc(cap);
				int ch;
				while (buf && (ch = fgetc(stdin)) != EOF) {
					if (len + 2 > cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
					buf[len++] = (char)ch;
				}
				if (buf) { buf[len] = '\0'; while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0'; }
				if (!buf || !*buf) { fprintf(stderr, "-p - got nothing on stdin\n"); free(buf); return 2; }
				one_shot = buf;
			}
			continue;
		}
		if (strcmp(a, "--bench") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "--bench needs a suite file\n"); return 2; }
			bench_suite = argv[++i];
			continue;
		}
		if (strcmp(a, "--output-format") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "--output-format needs json|text\n"); return 2; }
			const char *fmt = argv[++i];
			if (!strcmp(fmt, "json")) E.output_json = 1;
			else if (strcmp(fmt, "text") != 0) { fprintf(stderr, "--output-format: json|text\n"); return 2; }
			continue;
		}
		if (strcmp(a, "--anim") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "--anim needs name\n"); return 2; }
			const char *nm = argv[++i];
			E.anim_force_style = -1;
			for (int k = 0; k < CL_ANIM_COUNT; k++) {
				if (strcmp(nm, CL_ANIMS[k].name) == 0) { E.anim_force_style = k; break; }
			}
			if (E.anim_force_style < 0) { fprintf(stderr, "unknown anim: %s\n", nm); return 2; }
			continue;
		}
		if (strcmp(a, "--no-color") == 0) { E.color_enabled = 0; continue; }
		if (strcmp(a, "--no-splash") == 0) { E.show_splash = 0; continue; }
		if (strcmp(a, "--debug") == 0) { E.debug = 1; continue; }
		if (strcmp(a, "--compact") == 0) { E.compact = 1; continue; }
		if (strcmp(a, "--pipe") == 0) { E.pipe_mode = 1; continue; }
		if (strcmp(a, "--serve") == 0) { E.pipe_mode = 1; E.serve_mode = 1; continue; }
		if (strcmp(a, "--port") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "--port needs a number\n"); return 2; }
			serve_port = atoi(argv[++i]); continue;
		}
		if (strcmp(a, "--lan") == 0) { serve_bind = "0.0.0.0"; continue; }
		if (strcmp(a, "--token") == 0) {
			serve_token = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "";
			continue;
		}
		if (strcmp(a, "--bind") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "--bind needs an address\n"); return 2; }
			serve_bind = argv[++i]; continue;
		}
		if (strcmp(a, "--web") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "--web needs a directory\n"); return 2; }
			serve_web = argv[++i]; continue;
		}
		if (strcmp(a, "--dir") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "--dir needs a path\n"); return 2; }
			serve_dir = argv[++i]; continue;
		}
		if (strcmp(a, "--yolo") == 0) { E.ai_auto_approve = 1; continue; }
		fprintf(stderr, "unknown arg: %s (try --help)\n", a);
		return 2;
	}

#if !defined(_WIN32) && !defined(HAKO_WASM)
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, clSigint);
#endif

	/* json output owns stdout: no spinner, no color, no splash. Applied after
	   every flag has been parsed, so it does not depend on argument order. */
	if (E.output_json) { E.color_enabled = 0; E.show_splash = 0; }

	hkBootMark("main: flags parsed");   /* t0 — everything before this is the OS loading the binary */
	hkMigrateHakocToHako();
	hkBootMark("config migrated");
	clInitAI(&G_AI);
	hkBootMark("ai state init");
	hkMcpInit();
	hkBootMark("mcp init");

	if (!E.pipe_mode) {
		char pdir[PATH_MAX], hdir[PATH_MAX];
		const char *hm = getenv("HOME");
		int at_home = 0;
		if (hm && *hm) {
			snprintf(hdir, sizeof(hdir), "%s/.hako", hm);
			struct stat a, b;
			at_home = (stat(hdir, &a) == 0);
			if (at_home && hkProjectDirPath(pdir, sizeof(pdir)) && stat(pdir, &b) == 0)
				at_home = (a.st_dev == b.st_dev && a.st_ino == b.st_ino);
			else
				at_home = 0;
		}
		if (!at_home && hkProjectDirPath(pdir, sizeof(pdir))) {
			static const char *legacy[] = {"trust", "state", "history", NULL};
			int hit = 0;
			for (int i = 0; legacy[i] && !hit; i++) {
				char lp[PATH_MAX + 16];
				snprintf(lp, sizeof(lp), "%s/%s", pdir, legacy[i]);
				struct stat st;
				if (stat(lp, &st) == 0 && S_ISREG(st.st_mode)) hit = 1;
			}
			if (hit) {
				fprintf(stderr,
					"! legacy %s/{trust,state,history} ignored — per-project state moved to ~/.hako/projects/<enc>/ in v0.1.6\n"
					"  safe to: rm %s/{trust,state,history}\n",
					pdir, pdir);
			}
		}
	}

	if (!E.serve_mode && E.pipe_mode && serve_dir && *serve_dir) {
		if (chdir(serve_dir) == 0) {
			hkLoadSession();
			clCredsRestoreFor(hkProviderLabel());
			hkHealEndpoint();
		}
	}

	int rc;
	if (E.serve_mode) {
#ifndef HAKO_WASM
		if (serve_port) HS.port = serve_port;
		if (serve_bind) HS.bind_addr = serve_bind;
		if (serve_web)  HS.web_dir = serve_web;
		if (serve_token) {
			if (*serve_token) snprintf(HS.token, sizeof(HS.token), "%s", serve_token);
			else if (!hkRandomToken(HS.token, 24, "abcdefghijklmnopqrstuvwxyz0123456789")) {
				fprintf(stderr, "hako: no secure random source — pass --token <secret> yourself\n");
				return 1;
			}
		}
#endif
		rc = clServeMode(&G_AI, serve_dir);
	} else if (E.pipe_mode) {
		rc = clPipeMode(&G_AI);
	} else if (bench_suite) {
		rc = clBenchMode(&G_AI, bench_suite);
	} else if (one_shot) {
		rc = clOneShot(&G_AI, one_shot);
	} else {
		atexit(hkRestoreTerm);           /* :q, a fatal path, anything: leave the tty sane */
		printf("\x1b]0;" HK_MARK " hako\x07");
		fflush(stdout);
		rc = clRepl(&G_AI);
		printf("\x1b]0;\x07");
		fflush(stdout);
	}

	hkPrewarmJoin();
	hkMcpShutdown();
	hkRestoreTerm();
	clCleanupAI(&G_AI);
	clCleanupConfig();
	return rc;
}
