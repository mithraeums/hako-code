/*
 * hako-code — standalone AI agent CLI (binary: `hako`).
 * Lifted from hako.c (in-editor AI panel). Same constraints:
 *   C99, libc + pthread + curl-on-PATH. No other deps.
 *   Single file. Tabs.
 *
 * Sections (mirror hako.c order where lifted):
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

/*** includes ***/
#define HAKO_VERSION "0.1.6"

/* GitHub Copilot OAuth + API constants. Public client_id from VS Code Copilot extension.
   Defined here so hkBuildCurlCmd (earlier in file) can reference the Editor-* headers. */
#define HAKO_COPILOT_EDITOR_VER   "vscode/1.95.0"
#define HAKO_COPILOT_PLUGIN_VER   "copilot-chat/0.22.0"
#define HAKO_REPO    "mithraeums/hako-code"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
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
#define getcwd _getcwd
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define read(fd, buf, n) _read(fd, buf, n)
#define write(fd, buf, n) _write(fd, buf, n)
/* mkdir(): <direct.h> declares 1-arg _mkdir; POSIX expects 2-arg. Map all 2-arg calls to _mkdir. */
#define mkdir(p, m) _mkdir(p)
/* realpath(): not in Windows libc. Use _fullpath. */
#define realpath(p, r) _fullpath((r), (p), PATH_MAX)
/* No <sys/wait.h> on Windows; _pclose returns the exit status directly. */
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
/* MinGW <stdio.h> doesn't always expose POSIX getline. Provide a fallback. */
#include <stdlib.h>
static long hk_getline(char **lineptr, size_t *n, FILE *stream) {
	if (!lineptr || !n || !stream) return -1;
	if (!*lineptr || *n == 0) { *n = 256; *lineptr = realloc(*lineptr, *n); if (!*lineptr) return -1; }
	size_t len = 0; int c;
	while ((c = fgetc(stream)) != EOF) {
		if (len + 1 >= *n) {
			size_t nn = *n * 2;
			char *t = realloc(*lineptr, nn);
			if (!t) return -1;
			*lineptr = t; *n = nn;
		}
		(*lineptr)[len++] = (char)c;
		if (c == '\n') break;
	}
	if (len == 0 && c == EOF) return -1;
	(*lineptr)[len] = '\0';
	return (long)len;
}
#define getline hk_getline
#endif
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif
#endif
#include <pthread.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define AI_HISTORY_MAX 1000

/* Theme tokens. Runtime-mutable so :theme can swap palettes without restart.
   color_enabled gate skips escapes on non-TTY; truecolor terminals render
   palettes as designed, 256-color terms approximate, 16-color degrades. */
#define ANSI_DIM     "\x1b[2m"
#define ANSI_BOLD    "\x1b[1m"
#define ANSI_RESET   "\x1b[0m"
#define ANSI_CLR_LINE "\r\x1b[K"

/* Theme color tokens — each token is a pointer into the active theme's strings.
   Render code references TH_* and gets the active theme. Swap via clThemeApply. */
static const char *TH_USER   = "\x1b[38;2;212;177;104m";  /* warm gold */
static const char *TH_AI     = "\x1b[38;2;234;227;212m";  /* paper */
static const char *TH_SYS    = "\x1b[38;2;130;121;108m";  /* chalk */
static const char *TH_ERR    = "\x1b[1;91m";             /* bold bright red — 16-color for tmux/tty compat */
static const char *TH_TOOL   = "\x1b[38;2;198;145;94m";   /* bronze */
static const char *TH_OK     = "\x1b[38;2;148;172;120m";  /* sage — success */
static const char *TH_ACCENT = "\x1b[38;2;220;188;124m";  /* bright gold accent */
static const char *TH_META   = "\x1b[38;2;110;104;94m";   /* footnote dim */
static const char *TH_GHOST  = "\x1b[38;2;90;84;76m";     /* darker dim — ghost text */

/* Theme presets. Each row = {user, ai, sys, err, tool, ok, accent, meta, ghost}.
   Mithraeum is the default — warm earthy Claude-leaning with sage success +
   bronze tools for variation. Add new themes here; bump TH_PRESET_COUNT. */
typedef struct { const char *name; const char *c[9]; } clTheme;
static const clTheme TH_PRESETS[] = {
	{"mithraeum", {
		"\x1b[38;2;212;177;104m", "\x1b[38;2;234;227;212m", "\x1b[38;2;150;141;128m",
		"\x1b[1;91m",             "\x1b[38;2;198;145;94m",  "\x1b[38;2;148;172;120m",
		"\x1b[38;2;220;188;124m", "\x1b[38;2;164;152;136m", "\x1b[38;2;108;100;88m"
	}},
	{"claude", {  /* warm mono + single amber accent */
		"\x1b[38;2;217;164;87m",  "\x1b[38;2;230;230;226m", "\x1b[38;2;128;128;128m",
		"\x1b[1;91m",             "\x1b[38;2;217;164;87m",  "\x1b[38;2;164;188;128m",
		"\x1b[38;2;232;188;106m", "\x1b[38;2;104;104;104m", "\x1b[38;2;80;80;80m"
	}},
	{"nord", {
		"\x1b[38;2;143;188;187m", "\x1b[38;2;229;233;240m", "\x1b[38;2;108;120;141m",
		"\x1b[1;91m",             "\x1b[38;2;208;135;112m", "\x1b[38;2;163;190;140m",
		"\x1b[38;2;136;192;208m", "\x1b[38;2;100;110;128m", "\x1b[38;2;76;86;106m"
	}},
	{"mono", {  /* no color save dim — terminals with bad themes */
		"", "", ANSI_DIM, "", "", "", "", ANSI_DIM, ANSI_DIM
	}},
};
static const int TH_PRESET_COUNT = sizeof(TH_PRESETS) / sizeof(TH_PRESETS[0]);
static char TH_ACTIVE[32] = "mithraeum";

static void clThemeApply(const char *name) {
	for (int i = 0; i < TH_PRESET_COUNT; i++) {
		if (!strcmp(TH_PRESETS[i].name, name)) {
			TH_USER   = TH_PRESETS[i].c[0]; TH_AI    = TH_PRESETS[i].c[1];
			TH_SYS    = TH_PRESETS[i].c[2]; TH_ERR   = TH_PRESETS[i].c[3];
			TH_TOOL   = TH_PRESETS[i].c[4]; TH_OK    = TH_PRESETS[i].c[5];
			TH_ACCENT = TH_PRESETS[i].c[6]; TH_META  = TH_PRESETS[i].c[7];
			TH_GHOST  = TH_PRESETS[i].c[8];
			snprintf(TH_ACTIVE, sizeof(TH_ACTIVE), "%s", name);
			return;
		}
	}
}

/* Aliases for legacy call sites (kept so existing render code compiles). */
#define ANSI_USER    TH_USER
#define ANSI_AI      TH_AI
#define ANSI_SYS     TH_SYS
#define ANSI_ERR     TH_ERR
#define ANSI_TOOL    TH_TOOL
#define ANSI_MAGENTA TH_ACCENT
#define ANSI_BLUE    TH_OK

#ifdef _WIN32
#define cl_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
static void cl_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

/*** enums ***/
enum aiProviderType {
	AI_PROVIDER_NONE,
	AI_PROVIDER_OLLAMA,
	AI_PROVIDER_ANTHROPIC,
	AI_PROVIDER_OPENAI,
	AI_PROVIDER_MITHRAEUM    /* local hakm-served models. Wire-compat with OLLAMA for v0.1.6;
	                            v0.1.7+ swaps to own hakm-server port + native wire. */
};

/* OLLAMA and MITHRAEUM share the wire format in v0.1.6 (both speak ollama HTTP).
   Use this for wire-format-specific branching so MITHRAEUM stays in sync as wire evolves. */
#define HK_IS_OLLAMA_WIRE(t) ((t) == AI_PROVIDER_OLLAMA || (t) == AI_PROVIDER_MITHRAEUM)

#define HK_ROLE_SYSTEM 0
#define HK_ROLE_USER   1
#define HK_ROLE_AI     2

/*** structs ***/
typedef struct aiMessage {
	char *role;
	char *content;
	int raw;
} aiMessage;

typedef struct aiData {
	/* visual history (kept for /clear, /sessions, tail load) */
	char **history;
	unsigned char *history_role;
	int history_count;

	/* current turn state */
	char *current_prompt;
	char *current_response;

	/* worker */
	int active;
	pthread_t worker_thread;
	int streaming;
	pthread_mutex_t lock;

	/* anim */
	pthread_t anim_thread;
	int animating;
	int anim_style;
	int anim_label;
	int turn_index;

	/* api message stack */
	aiMessage *messages;
	int message_count;
	int message_cap;
	char *system_prompt;

	/* usage */
	int last_in_tokens;
	int last_out_tokens;
	long total_in_tokens;
	long total_out_tokens;
	long last_turn_ms;
	long turn_start_ms;
} aiData;

struct clConfig {
	enum aiProviderType ai_provider_type;
	char *ai_api_key;
	char *ai_endpoint;
	char *ai_model;
	int ai_temperature;
	int ai_max_tokens;
	int ai_tools_enabled;
	int ai_tool_gate;       /* 1 = drop tools when last user msg lacks tool-keyword */
	int ai_toolmode;        /* 0 = native function-calling, 1 = ReAct (pseudo-XML in prose) */
	int ai_stream;
	int ai_autowrite;

	char *session_id;
	long session_started;
	long session_last_used;
	int session_turn_count;
	int session_resumed;

	int color_enabled;     /* 1 if stdout is a tty */
	int interrupt;         /* SIGINT flag */

	int anim_force_style;  /* -1 = rotate, 0..N = pin */
	int debug;
	int compact;           /* --compact: skip figlet + framed box on banner */
	int pipe_mode;         /* --pipe: JSONL I/O with hako editor (no REPL UI) */
	unsigned char last_role_shown; /* tracks last role rendered for turn separator */

	/* OAuth (device-flow). When ai_oauth_refresh non-NULL, ai_api_key holds access_token. */
	char *ai_oauth_provider;       /* "gemini" | "github-models" | NULL */
	char *ai_oauth_refresh;        /* refresh_token */
	long  ai_oauth_expires_at;     /* unix epoch when access_token expires */
};

struct clConfig E;
static aiData G_AI;

/*** anim tables ***/
typedef struct {
	const char *name;
	const char **frames;
	int frame_count;
	int delay_ms;
	const char **color;  /* indirect so runtime theme swap affects spinners */
} clAnim;

static const char *FRM_BRAILLE[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
static const char *FRM_DOTS[]    = {".  ", ".. ", "...", " ..", "  .", "   "};
static const char *FRM_BAR[]     = {"▏","▎","▍","▌","▋","▊","▉","█","▉","▊","▋","▌","▍","▎"};
static const char *FRM_PULSE[]   = {"⠂","⠆","⠇","⠧","⠷","⠿","⠷","⠧","⠇","⠆"};
static const char *FRM_BOUNCE[]  = {"◐","◓","◑","◒"};
static const char *FRM_GHOST[]   = {"ᗜˬᗜ","ᗜ◡ᗜ","ᗜ‿ᗜ","ᗜ◠ᗜ","ᗜ_ᗜ","ᗜ◡ᗜ"};
static const char *FRM_ARROWS[]  = {"←","↖","↑","↗","→","↘","↓","↙"};
static const char *FRM_BLOCKS[]  = {"▖","▘","▝","▗"};

static const clAnim CL_ANIMS[] = {
	{"braille", FRM_BRAILLE, 10, 80,  &TH_TOOL},
	{"dots",    FRM_DOTS,    6,  140, &TH_AI},
	{"bar",     FRM_BAR,     14, 70,  &TH_ACCENT},
	{"pulse",   FRM_PULSE,   10, 100, &TH_OK},
	{"bounce",  FRM_BOUNCE,  4,  150, &TH_USER},
	{"ghost",   FRM_GHOST,   6,  220, &TH_AI},
	{"arrows",  FRM_ARROWS,  8,  90,  &TH_TOOL},
	{"blocks",  FRM_BLOCKS,  4,  130, &TH_ACCENT},
};
static const int CL_ANIM_COUNT = sizeof(CL_ANIMS) / sizeof(CL_ANIMS[0]);

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
	"consulting the oracle",
	"boxing it up",
};
static const int CL_LABEL_COUNT = sizeof(CL_LABELS) / sizeof(CL_LABELS[0]);

/* Medium logo — outline silhouette. 10 rows × 28 cols. */
/* CL_LOGO_MEDIUM — 13 rows × 26 cells. Pure braille blanks (U+2800) for spacing;
   no ASCII spaces, no TABs. Mixing in a literal TAB previously bust the right
   border of the box because clCellWidth counts TAB as 1 cell but the terminal
   renders it as multiple. Keep this all-braille so width and render agree. */
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

/* Tiny logo — outline silhouette, smallest readable form. 4 rows × 12 cols. */
static const char *CL_LOGO_TINY[] = {
    " ______ ",
	"|      |",
	"|      |",
	"|      |",
	"|______|",
	NULL
};

/*** forward decls ***/
static void aiAddHistory(aiData *data, const char *text);
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
static const char *hkProviderName(enum aiProviderType t);
static enum aiProviderType hkParseProvider(const char *s);
static const char *hkProviderDefaultEndpoint(const char *s);
static void hkApplyProviderAlias(const char *val);
static int hkProjectTrusted(void);
static int hkGrantProjectTrust(void);
static void hkSaveSession(void);
static void hkLoadSession(void);
static void hkGenSessionId(void);
static void hkLogMessage(const char *role, const char *content);
static void hkLoadHistoryTail(aiData *data, int max_msgs);
static int hkLoadSkills(aiData *data);
static char *hkJsonUnescape(const char *s, int len);
static void hkAnnounceTool(aiData *data, const char *fname, const char *args_obj);
static void hkAnnounceToolResult(aiData *data, const char *result);
static char *hkExtractJsonString(const char *src, const char *key);
static int hkExtractJsonInt(const char *src, const char *key);
static char *hkExtractJsonObject(const char *src, const char *key);
static void hkUpdateUsage(aiData *data, const char *resp);
static char *aiBuildCurlCommand(aiData *data, enum aiProviderType type);
static int clOAuthAnthropic(aiData *data);
static int clOAuthGithubCopilot(aiData *data);
static int clOAuthCopilotExchange(aiData *data);
static int clOAuthGithubModels(aiData *data);
static int clOAuthOpenRouter(aiData *data);
static int clOAuthRefresh(aiData *data);
static void clOAuthEnsureFresh(aiData *data);
static char *clOAuthRandomVerifier(void);
static int clOAuthLoopbackListen(int *out_port);
static char *clOAuthLoopbackWait(int srv_fd, int timeout_sec);
static void clUrlEncodeInto(const char *s, char *out, size_t cap);

/* Set by /edit slash. Pre-fills the next clReadLineRaw buffer + cursor at end. */
static char *cl_preset_input = NULL;
static char *aiExtractResponse(const char *json, enum aiProviderType type);
static char *hkExecTool(const char *name, const char *input_json);
static char *hkExtractContentArray(const char *response);
static char *hkBuildToolResults(aiData *data, const char *content_array);
static int hkFnToolExecAll(aiData *data, const char *response);
static int hkReactToolExecAll(aiData *data, const char *content);
static void clStartAnim(aiData *data);
static void clStopAnim(aiData *data);

/*** json helpers ***/
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

static char *hkExtractJsonString(const char *src, const char *key) {
	/* Match `"key"` followed by optional whitespace, `:`, optional whitespace, `"`.
	   Some providers (Anthropic OAuth token endpoint) pretty-print with spaces. */
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = src;
	while ((p = strstr(p, pat)) != NULL) {
		const char *q = p + strlen(pat);
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		if (*q != ':') { p++; continue; }
		q++;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		if (*q != '"') { p++; continue; }
		q++;
		const char *end = q;
		while (*end && !(*end == '"' && *(end - 1) != '\\')) end++;
		if (!*end) return NULL;
		int len = (int)(end - q);
		char *out = malloc(len + 1);
		memcpy(out, q, len);
		out[len] = '\0';
		return out;
	}
	return NULL;
}

static int hkExtractJsonInt(const char *src, const char *key) {
	if (!src) return -1;
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = src;
	while ((p = strstr(p, pat)) != NULL) {
		const char *q = p + strlen(pat);
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		if (*q != ':') { p++; continue; }
		q++;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		if (*q < '0' || *q > '9') { p++; continue; }
		return atoi(q);
	}
	return -1;
}

static char *hkExtractJsonObject(const char *src, const char *key) {
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":{", key);
	const char *p = strstr(src, pat);
	if (!p) return NULL;
	p += strlen(pat) - 1;
	int depth = 0;
	const char *start = p;
	int in_str = 0, esc = 0;
	while (*p) {
		if (esc) { esc = 0; p++; continue; }
		if (*p == '\\') { esc = 1; p++; continue; }
		if (*p == '"') in_str = !in_str;
		else if (!in_str) {
			if (*p == '{') depth++;
			else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
		}
		p++;
	}
	int len = p - start;
	char *out = malloc(len + 1);
	memcpy(out, start, len);
	out[len] = '\0';
	return out;
}

/* USD per million tokens, (input, output). Model match is substring of E.ai_model.
   Updated 2026-05; coarse — pricing drifts and provider tiers shift. Order matters: longer
   substrings first so e.g. "claude-haiku" wins over "claude". */
static int hkModelPriceUSDperM(const char *model, double *in_p, double *out_p) {
	if (!model || !*model) return 0;
	struct { const char *m; double in; double out; } table[] = {
		{ "claude-opus-4",    15.0,  75.0 },
		{ "claude-sonnet-4",   3.0,  15.0 },
		{ "claude-haiku-4",    1.0,   5.0 },
		{ "claude-3-5-sonnet", 3.0,  15.0 },
		{ "claude-3-5-haiku",  1.0,   5.0 },
		{ "claude-3-opus",    15.0,  75.0 },
		{ "claude",            3.0,  15.0 },  /* fallback Anthropic */
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

/* Returns 0 when on a flat-rate subscription (e.g. /login anthropic OAuth) or free tier.
   Returns -1 when the model has no price entry. */
static double hkSessionCostUSD(aiData *data) {
	if (!data) return 0.0;
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic")) return 0.0;
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-copilot")) return 0.0;
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-models")) return 0.0;
	if (HK_IS_OLLAMA_WIRE(E.ai_provider_type)) return 0.0;  /* local + ollamacloud + mithraeum free-tier-ish */
	double ip = 0, op = 0;
	if (!hkModelPriceUSDperM(E.ai_model, &ip, &op)) return -1.0;
	return ((double)data->total_in_tokens * ip + (double)data->total_out_tokens * op) / 1e6;
}

/* Free-tier / sub label for status line. Returns NULL when not flat-rate. */
static const char *hkFreeTierLabel(void) {
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic"))      return "sub";
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
	int out = hkExtractJsonInt(resp, "output_tokens");
	if (in < 0) in = hkExtractJsonInt(resp, "prompt_tokens");
	if (out < 0) out = hkExtractJsonInt(resp, "completion_tokens");
	if (in < 0) in = hkExtractJsonInt(resp, "prompt_eval_count");
	if (out < 0) out = hkExtractJsonInt(resp, "eval_count");
	if (in >= 0) { data->last_in_tokens = in; data->total_in_tokens += in; }
	if (out >= 0) { data->last_out_tokens = out; data->total_out_tokens += out; }
}

/*** file/shell helpers ***/
static char *hkReadFileAll(const char *path, long max_bytes) {
	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz < 0) { fclose(fp); return NULL; }
	if (max_bytes > 0 && sz > max_bytes) sz = max_bytes;
	char *buf = malloc(sz + 1);
	if (!buf) { fclose(fp); return NULL; }
	size_t got = fread(buf, 1, (size_t)sz, fp);
	buf[got] = '\0';
	fclose(fp);
	return buf;
}

/* Run cmd via tmp script file — sidesteps double-quote escaping nightmares with
   heredocs, embedded quotes, multi-line shell. Caller already JSON-unescaped cmd
   so newlines / quotes are real bytes. macOS `timeout` may be absent; fall back. */
static char *hkRunShellCapture(const char *cmd, long max_bytes) {
	char script[256];
	snprintf(script, sizeof(script), "/tmp/hako-cmd-%d.sh", (int)getpid());
	FILE *sf = fopen(script, "w");
	if (!sf) return strdup("error: cannot write tmp script");
	fputs(cmd, sf);
	fputc('\n', sf);
	fclose(sf);
	chmod(script, 0700);

	char full[512];
	/* Prefer GNU/BSD `timeout`; if missing, run without it (subprocess can hang). */
	if (system("command -v timeout >/dev/null 2>&1") == 0)
		snprintf(full, sizeof(full), "timeout 10 sh %s 2>&1", script);
	else
		snprintf(full, sizeof(full), "sh %s 2>&1", script);

	FILE *fp = popen(full, "r");
	if (!fp) { unlink(script); return strdup("error: popen failed"); }
	char *out = NULL;
	size_t total = 0;
	char buf[4096];
	while (fgets(buf, sizeof(buf), fp)) {
		int n = strlen(buf);
		if (max_bytes > 0 && (long)(total + n) > max_bytes) n = max_bytes - total;
		if (n <= 0) break;
		out = realloc(out, total + n + 1);
		memcpy(out + total, buf, n);
		total += n;
	}
	int rc = pclose(fp);
	unlink(script);
	if (!out) {
		/* Empty output but nonzero exit = error worth surfacing. */
		if (rc != 0) {
			char *e = malloc(64);
			snprintf(e, 64, "(no output, exit code %d)", WEXITSTATUS(rc));
			return e;
		}
		return strdup("(no output)");
	}
	out[total] = '\0';
	return out;
}

static char *hkListDir(const char *path) {
	DIR *d = opendir(path);
	if (!d) return NULL;
	char *out = NULL;
	size_t total = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		/* Directories get a `/` suffix so the model can tell them apart from
		   files without a follow-up stat. d_type works on macOS/Linux/BSD;
		   falls back to stat for filesystems that return DT_UNKNOWN. */
		int is_dir = 0;
#ifdef DT_DIR
		if (e->d_type == DT_DIR) is_dir = 1;
		else if (e->d_type == DT_UNKNOWN) {
			char full[PATH_MAX];
			snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
			struct stat st;
			if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = 1;
		}
#else
		char full[PATH_MAX];
		snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
		struct stat st;
		if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = 1;
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

/*** path + trust ***/

/* v0.1.6 rename: ~/.hakoc/ → ~/.hako/. One-shot migrator. If old dir exists and
   new dir does not, rename. Warn once, then proceed. Safe no-op otherwise. */
static void hkMigrateHakocToHako(void) {
	const char *home = getenv("HOME");
	if (!home) return;
	char old_path[512], new_path[512];
	snprintf(old_path, sizeof(old_path), "%s/.hakoc", home);
	snprintf(new_path, sizeof(new_path), "%s/.hako", home);
	struct stat st_old, st_new;
	if (stat(old_path, &st_old) != 0) return;          /* nothing to migrate */
	if (stat(new_path, &st_new) == 0) return;          /* new already exists, leave old alone */
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
	mkdir(out, 0755);
#endif
}

static int hkProjectDirPath(char *out, size_t n) {
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) return 0;
	snprintf(out, n, "%s/.hako", cwd);
	return 1;
}

/* Encode cwd path → CC-style dash-flattened key. /Users/zach/foo → -Users-zach-foo.
   Used to namespace per-project state under ~/.hako/projects/<enc>/. */
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

/* Per-project state dir under ~/.hako/projects/<enc>/. Created on demand. */
static int hkProjectStateDir(char *out, size_t n) {
	char home_dir[512]; hkClawDirPath(home_dir, sizeof(home_dir));
	if (!home_dir[0]) return 0;
	char projects[640];
	snprintf(projects, sizeof(projects), "%s/projects", home_dir);
	mkdir(projects, 0755);
	char enc[PATH_MAX];
	if (!hkEncodeCwd(enc, sizeof(enc))) return 0;
	snprintf(out, n, "%s/%s", projects, enc);
	mkdir(out, 0755);
	return 1;
}

static int hkProjectTrusted(void) {
	char dir[PATH_MAX];
	if (!hkProjectStateDir(dir, sizeof(dir))) return 0;
	char trust[PATH_MAX + 16];
	snprintf(trust, sizeof(trust), "%s/trust", dir);
	struct stat st;
	return (stat(trust, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

static int hkGrantProjectTrust(void) {
	char dir[PATH_MAX];
	if (!hkProjectStateDir(dir, sizeof(dir))) return 0;
	char trust[PATH_MAX + 16];
	snprintf(trust, sizeof(trust), "%s/trust", dir);
	FILE *fp = fopen(trust, "w");
	if (!fp) return 0;
	fprintf(fp, "granted=%ld\n", (long)time(NULL));
	fclose(fp);
	/* Drop HAKO.md stub in project dir as the sole on-disk artifact there. */
	char pdir[PATH_MAX];
	if (hkProjectDirPath(pdir, sizeof(pdir))) {
		mkdir(pdir, 0755);
		char hako[PATH_MAX + 16];
		snprintf(hako, sizeof(hako), "%s/HAKO.md", pdir);
		struct stat st;
		if (stat(hako, &st) != 0) {
			FILE *h = fopen(hako, "w");
			if (h) {
				fprintf(h, "# HAKO.md\n\nProject context for hako-code. Add notes/instructions/rules here. Loaded as system prompt context when present.\n");
				fclose(h);
			}
		}
	}
	return 1;
}

static void hkHistoryPath(char *out, size_t n) {
	char dir[PATH_MAX];
	if (hkProjectStateDir(dir, sizeof(dir))) {
		snprintf(out, n, "%s/history.jsonl", dir);
		return;
	}
	char fallback[512];
	hkClawDirPath(fallback, sizeof(fallback));
	snprintf(out, n, "%s/history.jsonl", fallback);
}

/* CC-style per-session log: ~/.hako/projects/<enc>/sessions/<sid>.jsonl. */
static void hkSessionLogPath(char *out, size_t n) {
	char dir[PATH_MAX];
	if (!hkProjectStateDir(dir, sizeof(dir)) || !E.session_id) {
		out[0] = '\0';
		return;
	}
	char sess_dir[PATH_MAX + 32];
	snprintf(sess_dir, sizeof(sess_dir), "%s/sessions", dir);
	mkdir(sess_dir, 0755);
	snprintf(out, n, "%s/%s.jsonl", sess_dir, E.session_id);
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
	if (filename_part && *filename_part)
		snprintf(out_full, out_cap, "%s/%s", resolved, filename_part);
	else
		snprintf(out_full, out_cap, "%s", resolved);
	return 0;
}

/*** provider resolution ***/
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
	if (strcmp(s, "ollama") == 0 || strcmp(s, "local") == 0
		|| strcmp(s, "ollamacloud") == 0 || strcmp(s, "ocloud") == 0
		|| strcmp(s, "ollama-cloud") == 0) return AI_PROVIDER_OLLAMA;
	if (strcmp(s, "anthropic") == 0 || strcmp(s, "claude") == 0) return AI_PROVIDER_ANTHROPIC;
	if (strcmp(s, "openai") == 0 || strcmp(s, "gpt") == 0 || strcmp(s, "groq") == 0) return AI_PROVIDER_OPENAI;
	if (strcmp(s, "deepseek") == 0 || strcmp(s, "mistral") == 0 || strcmp(s, "together") == 0
		|| strcmp(s, "fireworks") == 0 || strcmp(s, "openrouter") == 0
		|| strcmp(s, "xai") == 0 || strcmp(s, "grok") == 0
		|| strcmp(s, "gemini") == 0 || strcmp(s, "google") == 0
		|| strcmp(s, "cerebras") == 0
		|| strcmp(s, "github-copilot") == 0 || strcmp(s, "copilot") == 0
		|| strcmp(s, "github-models") == 0 || strcmp(s, "ghmodels") == 0
		|| strcmp(s, "custom") == 0) return AI_PROVIDER_OPENAI;
	/* hakm-served local models. v0.1.6 wire = ollama-compat HTTP; v0.1.7 = own port. */
	if (strcmp(s, "mithraeum") == 0 || strcmp(s, "hakm") == 0
		|| strcmp(s, "koi") == 0) return AI_PROVIDER_MITHRAEUM;
	return AI_PROVIDER_NONE;
}

static const char *hkProviderDefaultEndpoint(const char *s) {
	if (strcmp(s, "ollama") == 0 || strcmp(s, "local") == 0) return "http://localhost:11434";
	/* mithraeum: same port as ollama for v0.1.6 (transitional). v0.1.7 swaps to own port. */
	if (strcmp(s, "mithraeum") == 0 || strcmp(s, "hakm") == 0 || strcmp(s, "koi") == 0)
		return "http://localhost:11434";
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
	if (strcmp(s, "github-copilot") == 0 || strcmp(s, "copilot") == 0)
		return "https://api.githubcopilot.com";
	if (strcmp(s, "github-models") == 0 || strcmp(s, "ghmodels") == 0)
		return "https://models.inference.ai.azure.com";
	return NULL;
}

static void hkApplyProviderAlias(const char *val) {
	enum aiProviderType t = hkParseProvider(val);
	if (t == AI_PROVIDER_NONE) return;
	E.ai_provider_type = t;
	const char *ep = hkProviderDefaultEndpoint(val);
	/* Always swap endpoint when switching provider — a stale endpoint from a prior
	   provider (e.g. gemini's googleapis URL) would otherwise leak into the new one. */
	free(E.ai_endpoint);
	E.ai_endpoint = ep ? strdup(ep) : NULL;
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
	if (!strcmp(name, "ollamacloud") || !strcmp(name, "ocloud") || !strcmp(name, "ollama-cloud"))
		return "https://ollama.com/settings/keys";
	return NULL;
}

static void clOpenUrl(const char *url) {
	if (!url) return;
	char cmd[2048];
#ifdef __APPLE__
	snprintf(cmd, sizeof(cmd), "open '%s' >/dev/null 2>&1 &", url);
#elif defined(_WIN32)
	snprintf(cmd, sizeof(cmd), "start \"\" \"%s\"", url);
#else
	snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", url);
#endif
	int rc = system(cmd);
	(void)rc;
}

static void clReadHidden(char *buf, size_t cap) {
	buf[0] = '\0';
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
}

/* Apply env var as api_key when current provider is OpenAI-format. */
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
	/* OpenAI-compat aliases — apply key only when their provider is the active one. */
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

	/* Ollama cloud bearer key (provider type OLLAMA, not OpenAI-format). */
	if ((k = getenv("OLLAMA_API_KEY")) && *k && E.ai_provider_type == AI_PROVIDER_OLLAMA) {
		free(E.ai_api_key); E.ai_api_key = strdup(k);
	}

	/* HAKO_* always wins. CLAW_* (v0.1.5 names) read once with deprecation warning. */
	if ((k = getenv("CLAW_API_KEY")) && *k && !getenv("HAKO_API_KEY")) {
		fprintf(stderr, "! CLAW_API_KEY is deprecated since v0.1.6 — use HAKO_API_KEY\n");
		free(E.ai_api_key); E.ai_api_key = strdup(k);
	}
	if ((k = getenv("HAKO_API_KEY")) && *k) { free(E.ai_api_key); E.ai_api_key = strdup(k); }
	if ((k = getenv("HAKO_PROVIDER")) && *k) hkApplyProviderAlias(k);
	if ((k = getenv("HAKO_MODEL")) && *k) { free(E.ai_model); E.ai_model = strdup(k); }
	if ((k = getenv("HAKO_ENDPOINT")) && *k) { free(E.ai_endpoint); E.ai_endpoint = strdup(k); }
}

/*** credential store ***/
/* Per-provider obfuscated INI at ~/.hako/credentials. XOR-folded with a 32-byte
   key derived from hostname+uid+salt, then base64'd per-line. Mode 0600.
   Not encryption — defeats `cat`/`grep` and stops a copied file from leaking on
   another machine. For real secrecy use platform keychain (out of scope here). */

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
#ifndef _WIN32
		getuid();
#else
		0xC0FFEEUL;
#endif
	const char *salt = "hako-code/credentials/v1";
	/* FNV-1a 32-byte rolling hash. */
	unsigned long h = 0xcbf29ce484222325UL;
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
				h *= 0x100000001b3UL;
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
	/* Serialize plaintext. */
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
	FILE *fp = fopen(path, "w");
	if (!fp) { free(b64); return; }
	fprintf(fp, "%s\n", CL_CREDS_MAGIC);
	/* Line-wrap base64 at 72 cols for human-eye sanity. */
	size_t blen = strlen(b64);
	for (size_t i = 0; i < blen; i += 72) {
		size_t n = blen - i; if (n > 72) n = 72;
		fwrite(b64 + i, 1, n, fp);
		fputc('\n', fp);
	}
	fclose(fp);
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
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	char header[64];
	if (!fgets(header, sizeof(header), fp)) { fclose(fp); return; }
	if (strncmp(header, CL_CREDS_MAGIC, strlen(CL_CREDS_MAGIC)) != 0) { fclose(fp); return; }
	/* Read remaining as base64 blob. */
	size_t cap = 4096, len = 0;
	char *b64 = malloc(cap);
	int ch;
	while ((ch = fgetc(fp)) != EOF) {
		if (len + 1 >= cap) { cap *= 2; b64 = realloc(b64, cap); }
		if (ch != '\n' && ch != '\r') b64[len++] = (char)ch;
	}
	b64[len] = '\0';
	fclose(fp);
	size_t plain_len = 0;
	unsigned char *plain = clB64Decode(b64, &plain_len);
	free(b64);
	if (!plain) return;
	unsigned char key[32]; clCredsKey(key);
	clXorBuf(plain, plain_len, key);
	/* Parse INI. */
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

/* Snapshot E's current secrets into the cred map under the current provider name. */
static void clCredsCaptureCurrent(void) {
	const char *name = hkProviderName(E.ai_provider_type);
	if (!name || !strcmp(name, "none")) return;
	clCred *c = clCredsUpsert(name);
	if (!c) return;
	free(c->api_key); c->api_key = E.ai_api_key ? strdup(E.ai_api_key) : NULL;
	free(c->oauth_refresh); c->oauth_refresh = E.ai_oauth_refresh ? strdup(E.ai_oauth_refresh) : NULL;
	c->oauth_expires_at = E.ai_oauth_expires_at;
}

/* Populate E's secrets from cred map for the given provider. */
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
}

/*** session/state ***/
static void hkWriteSessionFile(const char *path, int include_secrets) {
	FILE *fp = fopen(path, "w");
	if (!fp) return;
	fprintf(fp, "ai_provider=%s\n", hkProviderName(E.ai_provider_type));
	if (E.ai_model) fprintf(fp, "ai_model=%s\n", E.ai_model);
	if (include_secrets && E.ai_endpoint) fprintf(fp, "ai_endpoint=%s\n", E.ai_endpoint);
	/* api_key + oauth_* live in ~/.hako/credentials (obfuscated), no longer in state. */
	fprintf(fp, "ai_tools_enabled=%d\n", E.ai_tools_enabled);
	fprintf(fp, "ai_tool_gate=%d\n", E.ai_tool_gate);
	fprintf(fp, "ai_toolmode=%d\n", E.ai_toolmode);
	fprintf(fp, "theme=%s\n", TH_ACTIVE);
	fprintf(fp, "ai_stream=%d\n", E.ai_stream);
	fprintf(fp, "ai_autowrite=%d\n", E.ai_autowrite);
	if (E.session_id) fprintf(fp, "session_id=%s\n", E.session_id);
	fprintf(fp, "session_started=%ld\n", E.session_started);
	fprintf(fp, "session_last_used=%ld\n", (long)time(NULL));
	fprintf(fp, "session_turn_count=%d\n", E.session_turn_count);
	fclose(fp);
#ifndef _WIN32
	/* Always 0600 — defense in depth even when no secret present this run. */
	chmod(path, 0600);
#endif
}

static void hkSaveSession(void) {
	char dir[512];
	hkClawDirPath(dir, sizeof(dir));
	if (dir[0]) {
		char path[640];
		snprintf(path, sizeof(path), "%s/state", dir);
		hkWriteSessionFile(path, 1);   /* global: holds prefs */
	}
	/* Per-project session state lives under ~/.hako/projects/<enc>/state.
	   Project dir itself only ever holds HAKO.md (created on trust grant). */
	char pdir[PATH_MAX];
	if (hkProjectStateDir(pdir, sizeof(pdir))) {
		char ppath[PATH_MAX + 8];
		snprintf(ppath, sizeof(ppath), "%s/state", pdir);
		hkWriteSessionFile(ppath, 1);
	}
}

static void hkLoadSessionFile(const char *path, int allow_session_fields) {
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, fp) != -1) {
		char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
		char *eq = strchr(line, '='); if (!eq) continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;
		if (strcmp(key, "ai_provider") == 0) {
			enum aiProviderType t = hkParseProvider(val);
			if (t != AI_PROVIDER_NONE) E.ai_provider_type = t;
		} else if (strcmp(key, "ai_model") == 0) {
			free(E.ai_model);
			E.ai_model = strdup(val);
		} else if (strcmp(key, "ai_endpoint") == 0) {
			free(E.ai_endpoint);
			E.ai_endpoint = strdup(val);
		} else if (strcmp(key, "ai_api_key") == 0) {
			free(E.ai_api_key);
			E.ai_api_key = strdup(val);
		} else if (strcmp(key, "ai_oauth_provider") == 0) {
			free(E.ai_oauth_provider);
			E.ai_oauth_provider = strdup(val);
		} else if (strcmp(key, "ai_oauth_refresh") == 0) {
			free(E.ai_oauth_refresh);
			E.ai_oauth_refresh = strdup(val);
		} else if (strcmp(key, "ai_oauth_expires_at") == 0) {
			E.ai_oauth_expires_at = atol(val);
		} else if (strcmp(key, "ai_tools_enabled") == 0) {
			E.ai_tools_enabled = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "ai_tool_gate") == 0) {
			E.ai_tool_gate = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "ai_toolmode") == 0) {
			E.ai_toolmode = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "theme") == 0) {
			clThemeApply(val);
		} else if (strcmp(key, "ai_stream") == 0) {
			E.ai_stream = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "ai_autowrite") == 0) {
			E.ai_autowrite = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "ai_max_tokens") == 0) {
			E.ai_max_tokens = atoi(val);
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
	free(line);
	fclose(fp);
}

static void hkGenSessionId(void) {
	free(E.session_id);
	E.session_id = malloc(17);
	long now = (long)time(NULL);
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
		struct stat st;
		snprintf(ppath, sizeof(ppath), "%s/state", pdir);
		if (stat(ppath, &st) == 0) {
			has_project = 1;
			hkLoadSessionFile(ppath, 1);
		}
	}

	long now = (long)time(NULL);
	int recent = E.session_last_used > 0 && (now - E.session_last_used) < 7 * 24 * 3600;
	if (has_project && E.session_id && recent) {
		E.session_resumed = 1;
	} else {
		E.session_resumed = 0;
		E.session_started = now;
		E.session_last_used = 0;
		E.session_turn_count = 0;
		hkGenSessionId();
	}
}

/*** history log + tail loader ***/
static void hkLogMessage(const char *role, const char *content) {
	char path[PATH_MAX];
	hkSessionLogPath(path, sizeof(path));
	if (!path[0]) return;
	FILE *fp = fopen(path, "a");
	if (!fp) return;
	int clen = content ? (int)strlen(content) : 0;
	int cap = clen * 6 + 32;
	char *esc = malloc(cap);
	if (!esc) { fclose(fp); return; }
	hkJsonEscapeInto(content ? content : "", esc, cap);
	fprintf(fp, "{\"ts\":%ld,\"role\":\"%s\",\"content\":\"%s\"}\n",
		(long)time(NULL), role, esc);
	free(esc);
	fclose(fp);
}

static void hkLoadHistoryTail(aiData *data, int max_msgs) {
	char path[PATH_MAX];
	hkSessionLogPath(path, sizeof(path));
	if (!path[0]) return;
	FILE *fp = fopen(path, "r");
	if (!fp) return;

	char **lines = NULL;
	int lcount = 0, lcap = 0;
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, fp)) != -1) {
		if (lcount >= lcap) { lcap = lcap ? lcap * 2 : 64; lines = realloc(lines, sizeof(char*) * lcap); }
		lines[lcount++] = strndup(line, n);
	}
	free(line);
	fclose(fp);

	/* Per-session file: every line belongs to this session, no sid filter needed. */
	int kept = lcount;
	int *keep_idx = malloc(sizeof(int) * (lcount + 1));
	for (int i = 0; i < lcount; i++) keep_idx[i] = i;
	int start = kept > max_msgs ? kept - max_msgs : 0;
	for (int k = start; k < kept; k++) {
		int i = keep_idx[k];
		char *l = lines[i];
		char *role = strstr(l, "\"role\":\"");
		char *content = strstr(l, "\"content\":\"");
		if (role && content) {
			role += 8;
			char *rend = strchr(role, '"');
			content += 11;
			char *cend = content;
			while (*cend) {
				if (*cend == '"' && *(cend - 1) != '\\') break;
				cend++;
			}
			if (rend && cend > content) {
				char rtag = *role;
				char *text = hkJsonUnescape(content, cend - content);
				if (text) {
					unsigned char r = (rtag == 'u') ? HK_ROLE_USER
						: (rtag == 'a') ? HK_ROLE_AI : HK_ROLE_SYSTEM;
					/* push raw — also rebuild api message stack */
					if (data->history_count < AI_HISTORY_MAX) {
						data->history[data->history_count] = strdup(text);
						data->history_role[data->history_count] = r;
						data->history_count++;
					}
					if (r == HK_ROLE_USER) aiPushMessage(data, "user", text);
					else if (r == HK_ROLE_AI) aiPushMessage(data, "assistant", text);
					free(text);
				}
			}
		}
	}
	free(keep_idx);
	for (int i = 0; i < lcount; i++) free(lines[i]);
	free(lines);
}

/* Append a skill block to (buf,total). Header tags include the skill name + optional root path so the
   model can call read_skill(skill="<name>", path="<rel>") to pull internal files on demand. */
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
	if (need > (1u << 22)) return; /* 4 MiB hard cap — protects iSh from runaway skill loads */
	char *nb = realloc(*buf, need);
	if (!nb) return;
	*buf = nb;
	memcpy(*buf + *total, header, hlen); *total += hlen;
	memcpy(*buf + *total, body, body_len); *total += body_len;
	memcpy(*buf + *total, tail, tlen); *total += tlen;
}

/* Walk a skill directory listing the .md files (relative paths up to MAX_DEPTH=3). Returns malloc'd string
   like "agents/CEO.md\nagents/DEV.md\n..." — used so the dispatcher knows what's pullable via read_skill. */
static char *hkSkillListMd(const char *root) {
	char *out = malloc(2048);
	size_t cap = 2048, len = 0;
	out[0] = '\0';

	typedef struct { char path[512]; int depth; } walkent;
	walkent stack[64];
	int sp = 0;
	snprintf(stack[sp].path, sizeof(stack[sp].path), "%s", root);
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
			snprintf(full, sizeof(full), "%s/%s", cur.path, e->d_name);
			struct stat st;
			if (stat(full, &st) != 0) continue;
			if (S_ISDIR(st.st_mode)) {
				if (cur.depth + 1 < 4 && sp < (int)(sizeof(stack)/sizeof(stack[0]))) {
					snprintf(stack[sp].path, sizeof(stack[sp].path), "%s", full);
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

static int hkLoadSkills(aiData *data) {
	if (!data) return 0;
	char dir[512];
	hkClawDirPath(dir, sizeof(dir));
	if (!dir[0]) return 0;
	char skills[512];
	snprintf(skills, sizeof(skills), "%s/skills", dir);
	DIR *d = opendir(skills);
	if (!d) return 0;

	char *buf = NULL;
	size_t total = 0;
	int loaded = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", skills, e->d_name);
		struct stat st;
		if (stat(path, &st) != 0) continue;

		if (S_ISDIR(st.st_mode)) {
			/* Directory skill — look for SKILL.md (or <name>.md) as dispatcher. */
			char dispatcher[1024] = {0};
			char cand[1024];
			snprintf(cand, sizeof(cand), "%s/SKILL.md", path);
			if (access(cand, R_OK) == 0) snprintf(dispatcher, sizeof(dispatcher), "%s", cand);
			else {
				snprintf(cand, sizeof(cand), "%s/%s.md", path, e->d_name);
				if (access(cand, R_OK) == 0) snprintf(dispatcher, sizeof(dispatcher), "%s", cand);
			}
			if (!dispatcher[0]) continue;

			char *body = hkReadFileAll(dispatcher, 200000);
			if (!body) continue;

			/* Append manifest of inner .md files so the model knows what's loadable via read_skill. */
			char *manifest = hkSkillListMd(path);
			size_t blen = strlen(body);
			size_t mlen = manifest ? strlen(manifest) : 0;
			char *combined = malloc(blen + mlen + 256);
			int n = 0;
			memcpy(combined + n, body, blen); n += blen;
			n += snprintf(combined + n, 256, "\n\n<files>\n%s</files>\n", manifest ? manifest : "");
			combined[n] = '\0';
			free(body);
			free(manifest);

			hkAppendSkillBlock(&buf, &total, e->d_name, path, combined, n);
			free(combined);
			loaded++;
			continue;
		}

		int nlen = (int)strlen(e->d_name);
		if (nlen < 4 || strcmp(e->d_name + nlen - 3, ".md") != 0) continue;
		FILE *fp = fopen(path, "r");
		if (!fp) continue;
		fseek(fp, 0, SEEK_END);
		long sz = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		if (sz < 0 || sz > 200000) { fclose(fp); continue; }
		char *body = malloc(sz);
		if (!body) { fclose(fp); continue; }
		fread(body, 1, sz, fp);
		fclose(fp);
		hkAppendSkillBlock(&buf, &total, e->d_name, NULL, body, sz);
		free(body);
		loaded++;
	}
	closedir(d);
	if (buf) buf[total] = '\0';

	/* ReAct mode prepends a pseudo-XML tool schema for models that don't natively
	   emit OpenAI/Anthropic function-calling JSON. Loop in worker thread scans
	   responses for <tool>...</tool> blocks and appends <observation>...</observation>
	   as user turns. Toggle via /toolmode react. */
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
		"  write_file(path, content)      Create/overwrite a file. Needs trust.\n"
		"  run_shell(cmd: string)         Run non-interactive shell command. Needs trust. 10s.\n"
		"  read_skill(skill, path)        Read a file inside an installed skill.\n"
		"\n"
		"Tool names are EXACTLY as listed above. Do NOT invent names like `bash`, `create_file`, `list_files`, `view`, `edit`, `Write`, `Read`, `LS`, `Bash` — those will be silently remapped but you should use the canonical names. `write_file` REQUIRES both `path` AND `content` params; never call it with path alone.\n"
		"\n"
		"ORDERING RULE — STRICT. Emit the <tool> block FIRST when a tool is needed. Do not narrate ('I will create...', 'Done!', 'Perfect!') BEFORE the tool call — those sentences print to the user before the tool runs, which reads as a lie. Correct shape: (1) <tool>...</tool>, (2) wait for <observation>, (3) THEN one short sentence about what actually happened.\n"
		"\n"
		"For the final answer to the user, respond in plain text WITHOUT any <tool> tags.\n"
		"\n";

	/* Prepend tool-use guidance. Small models (llama3.2, qwen2.5-small) tend to
	   invoke tools on greetings / chit-chat without it. Anthropic-tuned models
	   are mostly fine but the extra line is cheap. */
	static const char *BASE_PROMPT =
		"You are hako-code, a terminal AI agent.\n"
		"\n"
		"RULE 1: Do NOT call any tool unless the user explicitly asks to read, list, write, or run something. Greetings, questions, and explanations get a plain text reply with ZERO tool calls.\n"
		"\n"
		"RULE 2: All paths are RELATIVE to the current project. Use \".\" for the project root. NEVER use absolute paths like /home/..., /Users/..., /root/..., /tmp/.... Those paths do NOT exist here and every call will fail.\n"
		"\n"
		"Examples — call tools:\n"
		"  user: \"read README.md\"           -> read_file(path=\"README.md\")\n"
		"  user: \"what files are here?\"     -> list_dir(path=\".\")\n"
		"  user: \"create test.txt with hi\" -> write_file(path=\"test.txt\", content=\"hi\\n\")\n"
		"\n"
		"Examples — do NOT call tools:\n"
		"  user: \"hello\"                     -> plain text reply\n"
		"  user: \"who are you?\"              -> plain text reply\n"
		"  user: \"explain recursion\"         -> plain text reply\n"
		"  user: \"what is 2+2?\"              -> plain text reply\n"
		"\n"
		"If a tool returns \"error: path outside project\", do NOT retry with another absolute path. Either use \".\" or stop and reply in text.\n";
	/* Probe common interpreters / build tools once per process. Embed in system
	   prompt so the model picks the right binary on first call (python3 vs
	   python on macOS) instead of trial-and-erroring through a "not found"
	   round-trip. Cached: hkLoadSkills is called on every slash cmd. */
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
		int off = snprintf(env_probe, sizeof(env_probe), "\n# ENVIRONMENT\n\nAvailable on this system (prefer these names exactly):\n");
		for (int i = 0; names[i] && off < (int)sizeof(env_probe) - 80; i++) {
			char cmd[128];
			snprintf(cmd, sizeof(cmd), "command -v %s 2>/dev/null", names[i]);
			FILE *p = popen(cmd, "r");
			if (!p) continue;
			char buf[256] = {0};
			if (fgets(buf, sizeof(buf), p)) {
				int n = strlen(buf);
				while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) buf[--n] = '\0';
				if (n > 0) off += snprintf(env_probe + off, sizeof(env_probe) - off, "  %s -> %s\n", names[i], buf);
			}
			pclose(p);
		}
		off += snprintf(env_probe + off, sizeof(env_probe) - off,
			"\nIf 'python' is absent but 'python3' is present (common on macOS), call python3 directly — do NOT try 'python' first.\n");
	}

	/* Load project HAKO.md if present (CLAUDE.md equivalent — project context).
	   Refuse binary content (NUL byte in scan window) and warn on truncation. */
	char *hako_body = NULL; size_t hako_len = 0;
	{
		char pdir[PATH_MAX];
		if (hkProjectDirPath(pdir, sizeof(pdir))) {
			char hp[PATH_MAX + 16];
			snprintf(hp, sizeof(hp), "%s/HAKO.md", pdir);
			struct stat hst;
			if (stat(hp, &hst) == 0 && S_ISREG(hst.st_mode)) {
				const long HAKO_CAP = 200000;
				int truncated = hst.st_size > HAKO_CAP;
				hako_body = hkReadFileAll(hp, HAKO_CAP);
				if (hako_body) {
					hako_len = strlen(hako_body);
					/* Binary guard: scan up to 4KB for NUL. strlen above already
					   stopped at first NUL, so reported length < cap is suspicious
					   only when file size > reported length (binary embedded NUL). */
					size_t scan = hako_len < 4096 ? hako_len : 4096;
					int binary = 0;
					if ((long)hako_len < hst.st_size && hst.st_size <= HAKO_CAP) binary = 1;
					for (size_t i = 0; i < scan && !binary; i++) {
						unsigned char c = (unsigned char)hako_body[i];
						/* Allow TAB/LF/CR; reject other control chars below 0x20 */
						if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') binary = 1;
					}
					if (binary) {
						if (!E.pipe_mode) fprintf(stderr, "! HAKO.md skipped: binary or control-char content\n");
						free(hako_body); hako_body = NULL; hako_len = 0;
					} else if (truncated && !E.pipe_mode) {
						fprintf(stderr, "! HAKO.md truncated at %ld bytes (file is %ld)\n", HAKO_CAP, (long)hst.st_size);
					}
				}
			}
		}
	}
	const char *hako_hdr = "\n# HAKO.md (project context)\n\n";
	size_t hako_hlen = hako_body ? strlen(hako_hdr) : 0;

	/* Prose tool prompt required when the wire can't carry native function calling:
	   - Anthropic OAuth strips the `tools` field server-side.
	   - MITHRAEUM (small local models): 1.5B-class can't emit OpenAI tool_calls JSON. */
	int force_prose = (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic"))
		|| (E.ai_provider_type == AI_PROVIDER_MITHRAEUM);
	size_t blen = strlen(BASE_PROMPT);
	size_t rlen = (E.ai_toolmode == 1 || force_prose) ? strlen(REACT_PROMPT) : 0;
	size_t elen = strlen(env_probe);
	size_t need = rlen + blen + elen + total + hako_hlen + hako_len + 1;
	char *combined = malloc(need);
	if (combined) {
		size_t off = 0;
		if (rlen > 0) { memcpy(combined + off, REACT_PROMPT, rlen); off += rlen; }
		memcpy(combined + off, BASE_PROMPT, blen); off += blen;
		if (elen > 0) { memcpy(combined + off, env_probe, elen); off += elen; }
		if (buf && total > 0) { memcpy(combined + off, buf, total); off += total; }
		if (hako_body) {
			memcpy(combined + off, hako_hdr, hako_hlen); off += hako_hlen;
			memcpy(combined + off, hako_body, hako_len); off += hako_len;
		}
		combined[off] = '\0';
		free(buf); free(hako_body);
		free(data->system_prompt);
		data->system_prompt = combined;
	} else {
		free(data->system_prompt);
		data->system_prompt = buf;
	}
	return loaded;
}

/*** pipe emit helpers (--pipe mode) ***/

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

static void clPipeEmitMsg(const char *role, const char *text) {
	char esc[4096];
	clPipeEscape(text ? text : "", esc, sizeof(esc));
	printf("{\"type\":\"message\",\"role\":\"%s\",\"text\":\"%s\"}\n", role, esc);
	fflush(stdout);
}

static void clPipeEmitDisplay(const char *type, const char *display) {
	char esc[512];
	clPipeEscape(display ? display : "", esc, sizeof(esc));
	printf("{\"type\":\"%s\",\"display\":\"%s\"}\n", type, esc);
	fflush(stdout);
}

static void clPipeEmitRaw(const char *json) {
	puts(json);
	fflush(stdout);
}

/*** history (CLI render) — aiAddHistory variants print + store ***/
/* Render a single line of markdown into ANSI. Patterns:
   **bold**, *italic* / _italic_, `code`, leading "# " / "## " / "### " = heading,
   leading "> " = block-quote, leading "- " / "* " = bullet (left-pad arrow). */
static void clRenderMarkdownInline(const char *in, char *out, size_t cap) {
	size_t j = 0;
	const char *p = in;
	/* Headings */
	if (p[0] == '#' && (p[1] == ' ' || (p[1] == '#' && (p[2] == ' ' || (p[2] == '#' && p[3] == ' '))))) {
		int level = 1;
		while (*p == '#') { level++; p++; }
		if (*p == ' ') p++;
		j += snprintf(out + j, cap - j, "\x1b[1m");
		(void)level;
	}
	/* Block quote */
	else if (p[0] == '>' && p[1] == ' ') {
		j += snprintf(out + j, cap - j, "%s│ ", ANSI_DIM);
		p += 2;
	}
	/* Bullet */
	else if ((p[0] == '-' || p[0] == '*') && p[1] == ' ') {
		j += snprintf(out + j, cap - j, "%s· %s", ANSI_TOOL, ANSI_RESET);
		p += 2;
	}
	while (*p && j + 16 < cap) {
		if (p[0] == '*' && p[1] == '*') {
			const char *end = strstr(p + 2, "**");
			if (end) {
				j += snprintf(out + j, cap - j, "\x1b[1m%.*s\x1b[22m", (int)(end - p - 2), p + 2);
				p = end + 2; continue;
			}
		}
		if ((p[0] == '*' || p[0] == '_') && p[1] && p[1] != ' ' && p[1] != *p) {
			char delim = p[0];
			const char *end = strchr(p + 1, delim);
			if (end && end > p + 1 && end[-1] != ' ') {
				j += snprintf(out + j, cap - j, "\x1b[3m%.*s\x1b[23m", (int)(end - p - 1), p + 1);
				p = end + 1; continue;
			}
		}
		if (p[0] == '`') {
			const char *end = strchr(p + 1, '`');
			if (end) {
				j += snprintf(out + j, cap - j, "%s%.*s%s", ANSI_TOOL, (int)(end - p - 1), p + 1, ANSI_RESET);
				p = end + 1; continue;
			}
		}
		out[j++] = *p++;
	}
	out[j] = '\0';
}

/* Cross-line state for fenced code blocks. AI text arrives as individual history
   entries (one per line), so we need a sticky flag between calls to clPrintRoleLine. */
static int cl_in_codefence = 0;

static void clPrintRoleLine(unsigned char role, const char *text) {
	if (E.pipe_mode) {
		const char *rname = (role == HK_ROLE_USER) ? "user" :
		                    (role == HK_ROLE_AI)   ? "ai"   : "system";
		clPipeEmitMsg(rname, text);
		return;
	}
	const char *prefix, *color;
	switch (role) {
	case HK_ROLE_USER: prefix = "›  "; color = TH_USER; break;
	case HK_ROLE_AI:   prefix = "◆  "; color = TH_AI; break;
	default:
		if (text && (!strncmp(text, "Error", 5) || !strncmp(text, "error", 5))) {
			prefix = "!  "; color = TH_ERR;
		} else {
			prefix = "·  "; color = TH_SYS;
		}
		break;
	}

	/* Code-fence handling: detect ``` opening/closing on AI lines.
	   Inside fence: render with bronze gutter + dim warm background (chalk on void). */
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
		if (E.color_enabled) {
			printf("%s▎ %s%s%s\n", ANSI_TOOL, ANSI_AI, text ? text : "", ANSI_RESET);
		} else {
			printf("| %s\n", text ? text : "");
		}
		fflush(stdout);
		E.last_role_shown = role;
		return;
	}

	if (E.color_enabled && role == HK_ROLE_AI && text && *text) {
		char rendered[8192];
		clRenderMarkdownInline(text, rendered, sizeof(rendered));
		printf("%s%s%s%s\n", color, prefix, rendered, ANSI_RESET);
	} else if (E.color_enabled) {
		printf("%s%s%s%s\n", color, prefix, text ? text : "", ANSI_RESET);
	} else {
		printf("%s%s\n", prefix, text ? text : "");
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

static void aiAddHistoryRole(aiData *data, const char *text, unsigned char role) {
	if (!data || !text) return;
	const char *p = text;
	while (1) {
		const char *nl = strchr(p, '\n');
		int seg = nl ? (int)(nl - p) : (int)strlen(p);
		char tmp[4096];
		int n = seg < (int)sizeof(tmp) - 1 ? seg : (int)sizeof(tmp) - 1;
		memcpy(tmp, p, n);
		tmp[n] = '\0';
		aiPushHistoryStore(data, tmp, role);
		clPrintRoleLine(role, tmp);
		if (!nl) break;
		p = nl + 1;
	}
}

static void aiAddHistory(aiData *data, const char *text) {
	aiAddHistoryRole(data, text, HK_ROLE_SYSTEM);
}

/*** api message stack ***/
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

/* raw=2: `body_fields` holds the trailing fields between role and closing brace,
   e.g. "\"content\":null,\"tool_calls\":[...]" or "\"tool_call_id\":\"x\",\"content\":\"y\"". */
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

/* Find last message with given role. Returns index or -1. */
static int aiFindLastMessageRole(aiData *data, const char *role) {
	if (!data || !role) return -1;
	for (int i = data->message_count - 1; i >= 0; i--) {
		if (data->messages[i].role && !strcmp(data->messages[i].role, role)) return i;
	}
	return -1;
}

/* Drop API messages from idx onward (inclusive). */
static void aiDropMessagesFrom(aiData *data, int idx) {
	if (!data || idx < 0 || idx >= data->message_count) return;
	for (int i = idx; i < data->message_count; i++) {
		free(data->messages[i].role); free(data->messages[i].content);
	}
	data->message_count = idx;
}

/* Drop trailing history entries whose role matches `role_to_drop`. */
static void hkDropTrailingHistory(aiData *data, unsigned char role_to_drop) {
	if (!data) return;
	while (data->history_count > 0 && data->history_role[data->history_count - 1] == role_to_drop) {
		free(data->history[--data->history_count]);
		data->history[data->history_count] = NULL;
	}
}

/* Mid-chat provider swap: tool-bearing messages (raw=1 Anthropic content array,
   raw=2 OpenAI tool_calls/tool replies) carry schemas the new provider can't parse.
   Flatten by extracting plain text and dropping the rest; pure-text turns survive. */
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
		/* raw=1 (Anthropic content array) or raw=2 (OpenAI body fields):
		   pull any "text":"..." substring as plain content. tool role messages
		   under raw=2 stored "tool" content text — keep as user-visible context. */
		const char *src = m->content ? m->content : "";
		char *text = NULL;
		const char *p = strstr(src, "\"text\":\"");
		if (p) {
			p += 8;
			text = aiExtractStringValue(p);
		} else if (m->raw == 2) {
			/* tool reply: "content":"..." */
			const char *cp = strstr(src, "\"content\":\"");
			if (cp) { cp += 11; text = aiExtractStringValue(cp); }
		}
		if (text && *text) {
			/* "tool" role → demote to "user" so the new provider accepts it. */
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
	for (int i = 0; i < data->message_count; i++) {
		const char *content = data->messages[i].content ? data->messages[i].content : "";
		int clen = strlen(content);
		int need = len + clen * 6 + 128;
		if (need >= cap) {
			while (cap < need) cap *= 2;
			out = realloc(out, cap);
			if (!out) return NULL;
		}
		if (i > 0) out[len++] = ',';
		if (data->messages[i].raw == 1) {
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

static char *aiWriteRequestFile(const char *json) {
	char path[256];
	snprintf(path, sizeof(path), "/tmp/hako-req-%d.json", (int)getpid());
	FILE *fp = fopen(path, "w");
	if (!fp) return NULL;
	fputs(json, fp);
	fclose(fp);
	return strdup(path);
}

/*** tools registry ***/
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
	 "Create or overwrite a file in the trusted project directory.",
	 "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}",
	 "\"path\",\"content\""},
	{"run_shell",
	 "Run a non-interactive shell command. 10s timeout. Requires project trust.",
	 "\"cmd\":{\"type\":\"string\"}",
	 "\"cmd\""},
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
		/* Escape description — only field carrying free-form text. name/props/required
		   are author-controlled JSON fragments and must remain literal. */
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
	if (len + 2 >= cap) { cap += 4; out = realloc(out, cap); }
	out[len++] = ']';
	out[len] = '\0';
	return out;
}

static char *hkExecTool(const char *name, const char *input_json) {
	/* Alias common CC / generic tool names to ours. Case-insensitive — small
	   models frequently emit lowercase variants (`bash`, `ls`) regardless of
	   schema. Add new aliases here as field reports come in. */
	struct { const char *from; const char *to; } aliases[] = {
		{"create_file",  "write_file"},
		{"write",        "write_file"},
		{"edit",         "write_file"},
		{"edit_file",    "write_file"},
		{"str_replace",  "write_file"},
		{"str_replace_based_edit_tool", "write_file"},
		{"str_replace_editor",          "write_file"},
		{"text_editor",                 "write_file"},
		{"text_edit",                   "write_file"},
		{"read",         "read_file"},
		{"view",         "read_file"},
		{"cat",          "read_file"},
		{"open_file",    "read_file"},
		{"ls",           "list_dir"},
		{"list_files",   "list_dir"},
		{"list",         "list_dir"},
		{"dir",          "list_dir"},
		{"bash",         "run_shell"},
		{"shell",        "run_shell"},
		{"sh",           "run_shell"},
		{"exec",         "run_shell"},
		{"run",          "run_shell"},
		{"run_command",  "run_shell"},
		{NULL, NULL}
	};
	for (int i = 0; aliases[i].from; i++) {
		if (strcasecmp(name, aliases[i].from) == 0) { name = aliases[i].to; break; }
	}
	if (strcmp(name, "read_file") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted — ask the user to run :trust before any file access");
		char *path = hkExtractJsonString(input_json, "path");
		if (!path) path = hkExtractJsonString(input_json, "file_path");
		if (!path) path = hkExtractJsonString(input_json, "filename");
		if (!path) return strdup("error: missing path");
		char full[PATH_MAX];
		if (hkResolveInProject(path, full, sizeof(full)) != 0) {
			free(path);
			return strdup("error: path outside project. Use \".\" for project root, or a relative path. Do NOT retry with another absolute path; reply to the user in text instead.");
		}
		free(path);
		char *c = hkReadFileAll(full, 100000);
		return c ? c : strdup("error: cannot read");
	}
	if (strcmp(name, "list_dir") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted — ask the user to run :trust before any file access");
		char *path = hkExtractJsonString(input_json, "path");
		if (!path) path = hkExtractJsonString(input_json, "file_path");
		if (!path) path = hkExtractJsonString(input_json, "directory");
		if (!path) path = hkExtractJsonString(input_json, "dir");
		if (!path) path = strdup(".");  /* default to project root when caller omits */
		char full[PATH_MAX];
		if (hkResolveInProject(path, full, sizeof(full)) != 0) {
			free(path);
			return strdup("error: path outside project. Use \".\" for project root, or a relative path. Do NOT retry with another absolute path; reply to the user in text instead.");
		}
		free(path);
		char *c = hkListDir(full);
		return c ? c : strdup("error: cannot list");
	}
	if (strcmp(name, "run_shell") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted");
		char *shcmd = hkExtractJsonString(input_json, "cmd");
		if (!shcmd) shcmd = hkExtractJsonString(input_json, "command");
		if (!shcmd) shcmd = hkExtractJsonString(input_json, "shell");
		if (!shcmd) shcmd = hkExtractJsonString(input_json, "script");
		if (!shcmd) return strdup("error: missing cmd (param must be one of: cmd, command, shell, script)");
		/* JSON-decode the cmd so heredocs / embedded quotes / newlines are real
		   bytes by the time the shell sees them. Without this, `\n` `\"` etc
		   stay literal backslash-n / backslash-quote and any non-trivial
		   command (heredoc, multi-line, quoted strings) silently breaks. */
		char *decoded = hkJsonUnescape(shcmd, (int)strlen(shcmd));
		free(shcmd);
		if (!decoded) return strdup("error: bad cmd escapes");
		char *c = hkRunShellCapture(decoded, 50000);
		free(decoded);
		return c;
	}
	if (strcmp(name, "read_skill") == 0) {
		char *skill = hkExtractJsonString(input_json, "skill");
		char *path  = hkExtractJsonString(input_json, "path");
		if (!skill || !path) {
			free(skill); free(path);
			return strdup("error: missing skill or path");
		}
		/* No / .. in skill name. */
		if (strchr(skill, '/') || strstr(skill, "..")) {
			free(skill); free(path);
			return strdup("error: invalid skill name");
		}
		/* No .. or absolute in path. */
		if (path[0] == '/' || strstr(path, "..")) {
			free(skill); free(path);
			return strdup("error: invalid path (must be relative, no ..)");
		}
		char dir[512]; hkClawDirPath(dir, sizeof(dir));
		char skill_root[1024];
		snprintf(skill_root, sizeof(skill_root), "%s/skills/%s", dir, skill);
		char full[PATH_MAX];
		snprintf(full, sizeof(full), "%s/%s", skill_root, path);
		/* Resolve and verify it stays under skill_root. */
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
		char *path = hkExtractJsonString(input_json, "path");
		if (!path) path = hkExtractJsonString(input_json, "file_path");
		if (!path) path = hkExtractJsonString(input_json, "filename");
		char *content_raw = hkExtractJsonString(input_json, "content");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "file_text");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "new_str");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "text");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "body");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "data");
		if (!path && !content_raw) return strdup("error: write_file needs both 'path' (or file_path) and 'content' params (got neither)");
		if (!path) { free(content_raw); return strdup("error: write_file missing 'path' param"); }
		if (!content_raw) { free(path); return strdup("error: write_file missing 'content' param — include the full file body as a string"); }
		/* JSON-decode escapes (\n \t \" \\) so file gets real bytes, not literals. */
		char *content = hkJsonUnescape(content_raw, (int)strlen(content_raw));
		free(content_raw);
		if (!content) { free(path); return strdup("error: bad content escapes"); }
		char full[PATH_MAX];
		if (hkResolveInProject(path, full, sizeof(full)) != 0) {
			free(path); free(content);
			return strdup("error: path outside trusted project");
		}
		size_t clen = strlen(content);
		int new_lines = 0;
		for (size_t i = 0; i < clen; i++) if (content[i] == '\n') new_lines++;
		if (clen > 0 && content[clen - 1] != '\n') new_lines++;
		long old_size = -1;
		int old_lines = 0;
		FILE *ef = fopen(full, "rb");
		if (ef) {
			fseek(ef, 0, SEEK_END);
			old_size = ftell(ef);
			fseek(ef, 0, SEEK_SET);
			int ch;
			while ((ch = fgetc(ef)) != EOF) if (ch == '\n') old_lines++;
			fclose(ef);
		}
		if (!E.ai_autowrite) {
			char pending[PATH_MAX + 16];
			snprintf(pending, sizeof(pending), "%s.hako-pending", full);
			FILE *fp = fopen(pending, "wb");
			if (!fp) { free(path); free(content); return strdup("error: cannot stage pending"); }
			fwrite(content, 1, clen, fp);
			fclose(fp);
			char *out = malloc(512);
			snprintf(out, 512, "preview staged at %s (new %zu bytes/%d lines, old %ld/%d). ai_autowrite=0; user must `mv` pending to apply.",
				pending, clen, new_lines, old_size, old_lines);
			free(path); free(content);
			return out;
		}
		FILE *fp = fopen(full, "wb");
		if (!fp) { free(path); free(content); return strdup("error: cannot open for write"); }
		size_t wrote = fwrite(content, 1, clen, fp);
		fclose(fp);
		char *out = malloc(256);
		snprintf(out, 256, "wrote %zu bytes to %s (new %d lines, old %d)", wrote, full, new_lines, old_lines);
		free(path); free(content);
		return out;
	}
	return strdup("error: unknown tool");
}

/* Category glyph for a tool name. Unicode in color mode, ASCII fallback otherwise. */
static const char *hkToolGlyph(const char *fname, int color) {
	if (!fname) return color ? "▸" : ">";
	if (!strcmp(fname, "read_file") || !strcmp(fname, "list_dir") ||
	    !strcmp(fname, "read_skill") || !strcmp(fname, "list_open_files") ||
	    !strcmp(fname, "read_open_file")) return color ? "◎" : "r";
	if (!strcmp(fname, "write_file")) return color ? "✎" : "w";
	if (!strcmp(fname, "run_shell"))   return color ? "❯" : "$";
	return color ? "▸" : ">";
}

static void hkAnnounceTool(aiData *data, const char *fname, const char *args_obj) {
	(void)data;
	char arg_summary[128] = "";
	/* Try every param-name variant the model might use — keep in sync with the
	   alias lists in hkExecTool's per-tool branches. */
	static const char *path_keys[] = {"path", "file_path", "filename", "directory", "dir", NULL};
	static const char *cmd_keys[]  = {"cmd", "command", "shell", "script", NULL};
	char *val = NULL;
	for (int i = 0; path_keys[i] && !val; i++) val = hkExtractJsonString(args_obj, path_keys[i]);
	if (!val) for (int i = 0; cmd_keys[i] && !val; i++) val = hkExtractJsonString(args_obj, cmd_keys[i]);
	if (val) { snprintf(arg_summary, sizeof(arg_summary), "%.80s", val); free(val); }
	const char *glyph = hkToolGlyph(fname, E.color_enabled);
	if (E.pipe_mode) {
		char display[256];
		snprintf(display, sizeof(display), "%s %s(%s)", glyph, fname, arg_summary);
		clPipeEmitDisplay("tool_start", display);
		return;
	}
	if (E.color_enabled) printf("%s%s %s(%s)%s\n", ANSI_TOOL, glyph, fname, arg_summary, ANSI_RESET);
	else printf("%s %s(%s)\n", glyph, fname, arg_summary);
	fflush(stdout);
}

static void hkAnnounceToolResult(aiData *data, const char *result) {
	(void)data;
	int len = result ? (int)strlen(result) : 0;
	int is_err = result && strncmp(result, "error:", 6) == 0;
	if (E.pipe_mode) {
		char display[256];
		if (is_err) snprintf(display, sizeof(display), "%s", result ? result : "error");
		else snprintf(display, sizeof(display), "  ← %d bytes", len);
		clPipeEmitDisplay("tool_end", display);
		return;
	}
	if (is_err) {
		if (E.color_enabled) printf("  %s✗ %s%s\n", ANSI_ERR, result, ANSI_RESET);
		else printf("  ! %s\n", result);
	} else {
		if (E.color_enabled) printf("  %s← %d bytes%s\n", ANSI_DIM, len, ANSI_RESET);
		else printf("  ← %d bytes\n", len);
	}
	fflush(stdout);
}

/*** build curl + extract ***/
static char *aiBuildCurlCommand(aiData *data, enum aiProviderType type) {
	char *msgs = aiBuildMessagesJson(data);
	if (!msgs) return NULL;

	const char *endpoint = E.ai_endpoint;
	const char *model = E.ai_model;
	const char *api_key = E.ai_api_key;
	int max_tokens = E.ai_max_tokens > 0 ? E.ai_max_tokens : 2048;

	const char *sys = (data->system_prompt && *data->system_prompt) ? data->system_prompt : "";
	char *sys_esc = NULL;
	if (*sys) {
		int slen = strlen(sys);
		sys_esc = malloc(slen * 6 + 8);
		hkJsonEscapeInto(sys, sys_esc, slen * 6 + 8);
	}

	int bodycap = strlen(msgs) + (sys_esc ? strlen(sys_esc) : 0) + 4096;
	char *body = malloc(bodycap);
	if (!body) { free(msgs); free(sys_esc); return NULL; }

	int tools_on = E.ai_tools_enabled;
	/* ReAct mode: don't include native function-calling schema in the request body.
	   Instead the model emits <tool>...</tool> blocks in prose which we parse + execute
	   in the worker loop. The ReAct system prompt is prepended in hkLoadSkills. */
	if (E.ai_toolmode == 1) tools_on = 0;
	/* Anthropic OAuth strips the `tools` field server-side for non-Claude-Code
	   clients. Force prose fallback: no tools field, model emits <tool> blocks
	   in prose, hako parses + executes. Works around the OAuth gate entirely. */
	if (type == AI_PROVIDER_ANTHROPIC && E.ai_oauth_provider
	    && !strcmp(E.ai_oauth_provider, "anthropic")) tools_on = 0;
	/* MITHRAEUM: 1.5B-class local models can't emit OpenAI tool_calls JSON. Force prose
	   XML tool path. Modelfile SYSTEM prompt teaches the <tool> schema. */
	if (type == AI_PROVIDER_MITHRAEUM) tools_on = 0;
	/* Auto tool-gate: small / non-tool-tuned models often hallucinate tool calls on
	   greetings. If the most recent message is a user turn lacking any tool keyword,
	   drop the schema for this call. Once a tool has been used in this turn (last msg
	   isn't user), keep tools so the loop can continue. */
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

	/* OpenAI / Ollama wires carry system as messages[0] role=system, not a top-level
	   field. Splice it in here so BASE_PROMPT / REACT_PROMPT / skills / HAKO.md
	   actually reach Copilot / GH Models / OpenRouter / Gemini / Ollama. msgs is
	   "[...]" — replace leading '[' with '[<sys-msg>,' (or '[<sys-msg>' if empty). */
	char *msgs_with_sys = NULL;
	if (sys_esc && *sys_esc && type != AI_PROVIDER_ANTHROPIC) {
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
		if (!model) model = (type == AI_PROVIDER_MITHRAEUM) ? "hako-sho-stock" : "llama3.2";
		int tlen = tools_on ? strlen(fn_tools) + 16 : 0;
		int need = bodycap + tlen + 256;
		if (need > bodycap) { body = realloc(body, need); bodycap = need; }
		/* keep_alive: keep the model resident in RAM for 30 min between calls
		   (Ollama default unloads after 5 min idle → multi-second cold start).
		   num_predict: cap generation so a runaway model doesn't burn minutes
		   on a single turn. num_ctx kept at server default (model-dependent). */
		int npred = E.ai_max_tokens > 0 ? E.ai_max_tokens : 1024;
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
		if (!model) model = "claude-haiku-4-5-20251001";
		int anth_on = tools_on && hkProjectTrusted();
		int oauth_anth = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic");
		int stream_on = oauth_anth || (E.ai_stream && !anth_on);
		int tlen = anth_on ? strlen(anth_tools) + 16 : 0;
		int need = bodycap + tlen + 512;
		if (need > bodycap) { body = realloc(body, need); bodycap = need; }
		const char *stream_field = stream_on ? ",\"stream\":true" : "";

		/* Anthropic OAuth: system MUST be array with first block = exact CC line.
		   Block 2 must not contradict the Claude Code identity — our hako-code
		   BASE_PROMPT starts "You are hako-code" which flips identity and the
		   model refuses tool use. Substitute a Claude-Code-aligned block 2 that
		   only carries the tool-use rules + skills, no identity claim. */
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
				"AVAILABLE TOOLS — write_file(path, content), read_file(path), "
				"list_dir(path), run_shell(cmd). Do not invent tools like "
				"`str_replace_based_edit_tool`, `text_editor`, `create_file`; "
				"they are remapped but cost an extra round-trip. Use canonical "
				"names.\\n\\n"
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
		if (oauth_anth) {
			snprintf(sys_field, scap,
				",\"system\":[{\"type\":\"text\",\"text\":\"You are Claude Code, Anthropic's official CLI for Claude.\"},{\"type\":\"text\",\"text\":\"%s\"}]",
				oauth_block2 ? oauth_block2 : "");
		} else if (*sys) {
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
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"max_tokens\":%d%s,\"messages\":%s%s}",
				model, max_tokens, sys_field, msgs, stream_field);
		}
		free(sys_field);
		break;
	}
	case AI_PROVIDER_OPENAI: {
		if (!endpoint) endpoint = "https://api.openai.com";
		if (!model) model = "gpt-4o-mini";
		int tlen = tools_on ? strlen(fn_tools) + 16 : 0;
		int need = bodycap + tlen + 96;
		if (need > bodycap) { body = realloc(body, need); bodycap = need; }
		if (tools_on) {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"max_tokens\":%d,\"messages\":%s,\"tools\":%s}",
				model, max_tokens, msgs, fn_tools);
		} else {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"max_tokens\":%d,\"messages\":%s}",
				model, max_tokens, msgs);
		}
		break;
	}
	default:
		free(body); free(msgs); free(sys_esc); free(anth_tools); free(fn_tools); return NULL;
	}

	free(msgs);
	free(sys_esc);
	free(anth_tools);
	free(fn_tools);

	char *reqfile = aiWriteRequestFile(body);
	free(body);
	if (!reqfile) return NULL;

	char *cmd = malloc(4096);
	if (!cmd) { free(reqfile); return NULL; }

	switch (type) {
	case AI_PROVIDER_MITHRAEUM:
	case AI_PROVIDER_OLLAMA:
		if (api_key && *api_key) {
			/* Ollama cloud (ollama.com) or any auth-gated Ollama-compat endpoint. */
			snprintf(cmd, 4096,
				"curl -s -X POST %s/api/chat -H 'Content-Type: application/json' -H 'Authorization: Bearer %s' --data @%s 2>/dev/null",
				endpoint, api_key, reqfile);
		} else {
			snprintf(cmd, 4096,
				"curl -s -X POST %s/api/chat -H 'Content-Type: application/json' --data @%s 2>/dev/null",
				endpoint, reqfile);
		}
		break;
	case AI_PROVIDER_ANTHROPIC:
		if (!api_key) { free(cmd); free(reqfile); return NULL; }
		{
			int oauth = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic");
			/* Claude Code fingerprint headers — Anthropic's OAuth validator checks
			   User-Agent + x-app + anthropic-beta. Missing any → tool_use stripped
			   even when prefix lands. UA is plausible Claude Code build string. */
			char hdr[512];
			if (oauth) {
				snprintf(hdr, sizeof(hdr),
					"-H 'Authorization: Bearer %s' "
					"-H 'anthropic-beta: oauth-2025-04-20' "
					"-H 'User-Agent: claude-cli/1.0.40 (external, cli)' "
					"-H 'x-app: cli'",
					api_key);
			} else {
				snprintf(hdr, sizeof(hdr), "-H 'x-api-key: %s'", api_key);
			}
			int will_stream = oauth || (E.ai_stream && !(E.ai_tools_enabled && hkProjectTrusted()));
			snprintf(cmd, 4096,
				will_stream
					? "curl -sN -X POST %s/v1/messages -H 'Content-Type: application/json' %s -H 'anthropic-version: 2023-06-01' -H 'accept: text/event-stream' --data @%s 2>/dev/null"
					: "curl -s -X POST %s/v1/messages -H 'Content-Type: application/json' %s -H 'anthropic-version: 2023-06-01' --data @%s 2>/dev/null",
				endpoint, hdr, reqfile);
		}
		break;
	case AI_PROVIDER_OPENAI:
		if (!api_key) { free(cmd); free(reqfile); return NULL; }
		{
			int copilot = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-copilot");
			int ghmodels = endpoint && strstr(endpoint, "models.inference.ai.azure.com");
			const char *path = (copilot || ghmodels) ? "/chat/completions" : "/v1/chat/completions";
			const char *copilot_hdrs = copilot
				? "-H 'Editor-Version: " HAKO_COPILOT_EDITOR_VER "' "
				  "-H 'Editor-Plugin-Version: " HAKO_COPILOT_PLUGIN_VER "' "
				  "-H 'Openai-Intent: conversation-panel' "
				  "-H 'Copilot-Integration-Id: vscode-chat' "
				: "";
			snprintf(cmd, 4096,
				"curl -s -X POST %s%s -H 'Content-Type: application/json' -H 'Authorization: Bearer %s' %s--data @%s 2>/dev/null",
				endpoint, path, api_key, copilot_hdrs, reqfile);
		}
		break;
	default: break;
	}

	free(reqfile);
	return cmd;
}

/* Walk a JSON string value (post-`"`). Returns malloc'd unescaped string. */
static char *aiExtractStringValue(const char *p) {
	int cap = 1024, len = 0;
	char *result = malloc(cap);
	while (*p && !(*p == '"' && *(p - 1) != '\\')) {
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
				/* \uXXXX: 4 hex digits → codepoint → UTF-8. Qwen/ollama uses this
				   for ASCII chars like '<' (<) — without this the prose tool
				   parser never matches '<tool' because we emit literal '<'. */
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
					/* BMP only. Surrogate pairs not handled; rare in code chat. */
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

/* Look for top-level error: {"error":{"message":"..."}}. Return "Error: ..." or NULL. */
static char *aiExtractApiError(const char *json) {
	const char *err = strstr(json, "\"error\"");
	if (!err) return NULL;
	const char *msg = strstr(err, "\"message\":\"");
	if (!msg) return NULL;
	msg += 11;
	char *body = aiExtractStringValue(msg);
	if (!body) return NULL;
	int blen = strlen(body);
	char *result = malloc(blen + 8);
	snprintf(result, blen + 8, "Error: %s", body);
	free(body);
	return result;
}

/* Anthropic: walk content[] array, concat all {"type":"text","text":"..."} blocks. */
static char *aiExtractAnthropicText(const char *json) {
	const char *p = json;
	int cap = 0, len = 0;
	char *out = NULL;
	while ((p = strstr(p, "\"type\":\"text\""))) {
		const char *block_start = p;
		while (block_start > json && *block_start != '{') block_start--;
		const char *tk = strstr(block_start, "\"text\":\"");
		/* skip if "text" key is too far away (means it's a different block's field) */
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

static char *aiExtractResponse(const char *json, enum aiProviderType type) {
	if (!json) return NULL;

	if (type == AI_PROVIDER_ANTHROPIC) {
		char *t = aiExtractAnthropicText(json);
		if (t && *t) return t;
		free(t);
		return aiExtractApiError(json);
	}

	/* OpenAI / Ollama / OpenAI-compat: first "content":"..." */
	const char *start = strstr(json, "\"content\":\"");
	if (!start) {
		char *e = aiExtractApiError(json);
		if (e) return e;
		/* Gemini may send {"choices":[{"message":{"content":null}}]} when blocked. */
		return NULL;
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

/* Extract raw JSON array value for `key` (e.g. "tool_calls"). Returns malloc'd "[...]". */
static char *hkExtractRawJsonArray(const char *src, const char *key) {
	char needle[64];
	snprintf(needle, sizeof(needle), "\"%s\":[", key);
	const char *p = strstr(src, needle);
	if (!p) return NULL;
	p += strlen(needle) - 1; /* point at '[' */
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

/* OpenAI/Ollama function-calling tool loop. Preserves tool_calls on assistant
   message and emits tool_call_id on each tool reply — required by strict
   validators (Gemini OpenAI-compat returns 400 INVALID_ARGUMENT otherwise). */
/* Parse <tool name="X">{...json...}</tool> blocks from ReAct response content,
   execute each, push an <observation> back as a user message. Returns count of
   tools executed (0 = no tool blocks found = model produced final answer). */
static int hkReactToolExecAll(aiData *data, const char *content) {
	if (!data || !content) return 0;
	int count = 0;
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

		/* Trim args JSON body. */
		while (body < close_tag && (*body == ' ' || *body == '\n' || *body == '\r' || *body == '\t')) body++;
		int blen = (int)(close_tag - body);
		while (blen > 0 && (body[blen-1] == ' ' || body[blen-1] == '\n' || body[blen-1] == '\r' || body[blen-1] == '\t')) blen--;
		char *args = malloc(blen + 1);
		if (!args) { cursor = close_tag + 7; continue; }
		memcpy(args, body, blen); args[blen] = '\0';

		hkAnnounceTool(data, tname, args);
		char *result = hkExecTool(tname, args);
		hkAnnounceToolResult(data, result);

		/* Push observation as user turn so model sees result on next API call. */
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

	/* Also parse Claude Code's native XML format that OAuth-trained Claude
	   defaults to:
	     <function_calls>
	       <invoke name="X">
	         <parameter name="K">V</parameter>
	       </invoke>
	     </function_calls>
	   Build a JSON args object from parameter children, then exec like normal. */
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

		/* Walk <parameter name="K">V</parameter> children, emit JSON. */
		char json[8192]; int jl = 0;
		jl += snprintf(json + jl, sizeof(json) - jl, "{");
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
			if (!first && jl < (int)sizeof(json) - 4) json[jl++] = ',';
			jl += snprintf(json + jl, sizeof(json) - jl, "\"%.*s\":\"", (int)(kend - pn), pn);
			for (const char *v = vstart; v < vend && jl < (int)sizeof(json) - 8; v++) {
				char c = *v;
				if      (c == '"')  { json[jl++]='\\'; json[jl++]='"'; }
				else if (c == '\\') { json[jl++]='\\'; json[jl++]='\\'; }
				else if (c == '\n') { json[jl++]='\\'; json[jl++]='n'; }
				else if (c == '\r') { json[jl++]='\\'; json[jl++]='r'; }
				else if (c == '\t') { json[jl++]='\\'; json[jl++]='t'; }
				else json[jl++] = c;
			}
			if (jl < (int)sizeof(json) - 1) json[jl++] = '"';
			first = 0;
			pp = vend + 12;
		}
		if (jl < (int)sizeof(json) - 1) json[jl++] = '}';
		json[jl] = '\0';

		hkAnnounceTool(data, tname, json);
		char *result = hkExecTool(tname, json);
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
	return count;
}

static int hkFnToolExecAll(aiData *data, const char *response) {
	char *tool_calls_raw = hkExtractRawJsonArray(response, "tool_calls");
	if (!tool_calls_raw) return 0;

	/* Push assistant message body with content (text or null) + tool_calls preserved. */
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
	/* Dedupe seen tool_call_ids — some models (gpt-4o on GH Models free tier) emit
	   the same id multiple times in one tool_calls array, which would push duplicate
	   tool replies and 400 the next API call with "Duplicate value for tool_call_id". */
	char *seen_ids[32] = {0};
	int n_seen = 0;
	while ((p = strstr(p, "\"function\""))) {
		/* Walk back to the enclosing tool_call object to grab its id. */
		const char *obj_start = p;
		while (obj_start > response && *obj_start != '{') obj_start--;
		char *tc_id = hkExtractJsonString(obj_start, "id");
		/* Dedupe by id. */
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

/*** anim ***/
static void *clAnimThread(void *arg) {
	aiData *data = (aiData *)arg;
	const clAnim *a = &CL_ANIMS[data->anim_style % CL_ANIM_COUNT];
	const char *label = CL_LABELS[data->anim_label % CL_LABEL_COUNT];
	int i = 0;
	while (data->animating) {
		const char *fr = a->frames[i % a->frame_count];
		if (E.color_enabled) {
			printf(ANSI_CLR_LINE "%s%s %s...%s", *a->color, fr, label, ANSI_RESET);
		} else {
			printf("\r%s %s...   ", fr, label);
		}
		fflush(stdout);
		cl_sleep_ms(a->delay_ms);
		i++;
	}
	if (E.color_enabled) printf(ANSI_CLR_LINE);
	else printf("\r                                                  \r");
	fflush(stdout);
	return NULL;
}

static void clStartAnim(aiData *data) {
	if (!E.color_enabled) {
		/* no-tty: skip animation, would just spew chars */
		return;
	}
	if (data->animating) return;
	if (E.anim_force_style >= 0) {
		data->anim_style = E.anim_force_style % CL_ANIM_COUNT;
	} else {
		data->anim_style = (data->turn_index + (int)(time(NULL) & 7)) % CL_ANIM_COUNT;
	}
	data->anim_label = (data->turn_index * 3 + 7) % CL_LABEL_COUNT;
	data->animating = 1;
	if (pthread_create(&data->anim_thread, NULL, clAnimThread, data) != 0) {
		data->animating = 0;
	}
}

static void clStopAnim(aiData *data) {
	if (!data->animating) return;
	data->animating = 0;
	pthread_join(data->anim_thread, NULL);
}

/* Wall-clock millis since arbitrary epoch (used for turn duration). */
static long clWallMs(void) {
#ifdef _WIN32
	return (long)GetTickCount64();
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long)tv.tv_sec * 1000L + (long)(tv.tv_usec / 1000);
#endif
}

/*** worker thread ***/
static void *aiWorkerThread(void *arg) {
	aiData *data = (aiData *)arg;

	data->turn_start_ms = clWallMs();

	pthread_mutex_lock(&data->lock);
	char *prompt = data->current_prompt ? strdup(data->current_prompt) : NULL;
	if (prompt) aiPushMessage(data, "user", prompt);
	pthread_mutex_unlock(&data->lock);
	free(prompt);

	int max_iters = 6;
	int iter = 0;
	int used_tool = 0;

	while (iter++ < max_iters) {
		data->turn_index++;
		clStartAnim(data);

		clOAuthEnsureFresh(data);

		pthread_mutex_lock(&data->lock);
		char *cmd = aiBuildCurlCommand(data, E.ai_provider_type);
		pthread_mutex_unlock(&data->lock);

		if (!cmd) {
			clStopAnim(data);
			pthread_mutex_lock(&data->lock);
			char msg[384];
			const char *pname = hkProviderName(E.ai_provider_type);
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

		FILE *fp = popen(cmd, "r");
		free(cmd);
		if (!fp) {
			clStopAnim(data);
			pthread_mutex_lock(&data->lock);
			aiAddHistory(data, "Error: Could not execute curl");
			data->streaming = 0;
			pthread_mutex_unlock(&data->lock);
			return NULL;
		}

		/* Mirror the body/curl stream gate exactly. OAuth Anthropic forces SSE; otherwise
		   stream when ai_stream is on AND we're not sending tools (tools need a complete
		   response so we can extract tool_use blocks). */
		int oauth_anth_resp = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic");
		int streaming_mode = (E.ai_provider_type == AI_PROVIDER_ANTHROPIC) &&
			(oauth_anth_resp || (E.ai_stream && !(E.ai_tools_enabled && hkProjectTrusted())));

		char buffer[8192];
		char *full_response = NULL;
		size_t total = 0;

		if (streaming_mode) {
			char *acc = calloc(1, 1);
			size_t acc_len = 0;
			int printed_prefix = 0;
			/* Suppress live print for Anthropic OAuth: model emits raw XML
			   tool-call tags that would render visibly. Buffer everything, strip
			   tags, then print clean text after stream ends. */
			int suppress_live = oauth_anth_resp;

			while (fgets(buffer, sizeof(buffer), fp)) {
				int blen = strlen(buffer);
				full_response = realloc(full_response, total + blen + 1);
				memcpy(full_response + total, buffer, blen);
				total += blen;

				if (strncmp(buffer, "data: ", 6) != 0) continue;
				const char *payload = buffer + 6;
				if (!strstr(payload, "content_block_delta")) continue;
				char *raw = hkExtractJsonString(payload, "text");
				if (!raw) continue;
				char *text = hkJsonUnescape(raw, (int)strlen(raw));
				free(raw);
				if (!text) continue;

				size_t tlen = strlen(text);
				acc = realloc(acc, acc_len + tlen + 1);
				memcpy(acc + acc_len, text, tlen);
				acc_len += tlen;
				acc[acc_len] = '\0';

				if (!suppress_live) {
					if (!printed_prefix) {
						clStopAnim(data);
						if (E.color_enabled) printf("%s◆  ", TH_AI);
						else printf("◆  ");
						printed_prefix = 1;
					}
					fwrite(text, 1, tlen, stdout);
					fflush(stdout);
				}
				free(text);
			}
			pclose(fp);
			clStopAnim(data);
			if (full_response) full_response[total] = '\0';

			/* If this response contained a tool call, skip rendering its prose
			   entirely. Tools execute below, chips print, next iteration's
			   response (which lands AFTER the observation) carries the real
			   summary. This matches Claude Code's behavior: no narration shown
			   for the planning/calling iteration, only the post-observation
			   summary. Without this, "Done!" / "Perfect!" prints BEFORE the
			   chip — reads as a lie. */
			int has_tool_call_in_acc = acc_len > 0 &&
				(strstr(acc, "<function_calls>") || strstr(acc, "<invoke name=") ||
				 strstr(acc, "<tool "));
			if (suppress_live && acc_len > 0 && !has_tool_call_in_acc) {
				const char *render_start = acc;
				const char *render_end = acc + acc_len;

				/* Find last </function_calls> or </tool>. */
				const char *last_close = NULL;
				for (const char *q = acc; q < acc + acc_len - 6; q++) {
					if (!strncmp(q, "</function_calls>", 17)) { last_close = q + 17; q += 16; }
					else if (!strncmp(q, "</tool>", 7))       { last_close = q + 7;  q += 6; }
				}
				if (last_close) render_start = last_close;

				/* Strip any stray tags inside the render window (shouldn't happen
				   if last_close logic correct, but defend). */
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
					/* Stray child tags model sometimes emits outside any wrapper —
					   model hallucinates a fake observation echoing what it expects
					   the system to reply with. Strip them silently. */
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
					clStopAnim(data);
					if (E.color_enabled) printf("%s◆  ", TH_AI);
					else printf("◆  ");
					fwrite(clean + cs, 1, ci - cs, stdout);
					if (E.color_enabled) printf("%s\n", ANSI_RESET);
					else printf("\n");
					fflush(stdout);
				}
				free(clean);
			} else if (printed_prefix) {
				if (E.color_enabled) printf("%s\n", ANSI_RESET);
				else printf("\n");
				fflush(stdout);
			}

			if (acc_len > 0) {
				pthread_mutex_lock(&data->lock);
				hkUpdateUsage(data, full_response);
				aiPushHistoryStore(data, acc, HK_ROLE_AI);
				aiPushMessage(data, "assistant", acc);
				hkLogMessage("assistant", acc);
				free(data->current_response);
				data->current_response = acc;
				pthread_mutex_unlock(&data->lock);

				/* Prose tool path: tools schema is suppressed in the wire, so model
				   emits tool calls as text. Scan acc for <tool> or <invoke> blocks. */
				int prose_active = (E.ai_toolmode == 1)
					|| (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic"))
					|| (E.ai_provider_type == AI_PROVIDER_MITHRAEUM);
				if (prose_active && (strstr(acc, "<tool") || strstr(acc, "<invoke name="))) {
					int n = hkReactToolExecAll(data, acc);
					if (n > 0) {
						free(full_response);
						used_tool = 1;
						continue;
					}
				}
			} else {
				free(acc);
				pthread_mutex_lock(&data->lock);
				aiAddHistory(data, "Error: Empty stream");
				pthread_mutex_unlock(&data->lock);
			}

			free(full_response);
			break;
		}

		while (fgets(buffer, sizeof(buffer), fp)) {
			int blen = strlen(buffer);
			full_response = realloc(full_response, total + blen + 1);
			memcpy(full_response + total, buffer, blen);
			total += blen;
		}
		pclose(fp);
		clStopAnim(data);
		if (full_response) full_response[total] = '\0';

		if (E.debug && full_response) {
			fprintf(stderr, "\n[debug] response (%zu bytes):\n%s\n[/debug]\n", total, full_response);
		}

		int tool_use_anthropic = E.ai_provider_type == AI_PROVIDER_ANTHROPIC
			&& full_response
			&& strstr(full_response, "\"stop_reason\":\"tool_use\"");
		/* Native function-calling: Ollama proper + OpenAI-compat. MITHRAEUM uses prose XML
		   tools because 1.5B models can't reliably emit OpenAI tool_calls JSON. */
		int tool_use_fn = (E.ai_provider_type == AI_PROVIDER_OLLAMA || E.ai_provider_type == AI_PROVIDER_OPENAI)
			&& full_response
			&& strstr(full_response, "\"tool_calls\":[");

		char *content = aiExtractResponse(full_response, E.ai_provider_type);

		pthread_mutex_lock(&data->lock);
		hkUpdateUsage(data, full_response);
		if (content && *content) {
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

		/* Prose tool mode: scan plain-text content for <tool>/<invoke> blocks and execute.
		   Forced when Anthropic OAuth or MITHRAEUM (small local models). */
		int prose_active = (E.ai_toolmode == 1)
			|| (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic"))
			|| (E.ai_provider_type == AI_PROVIDER_MITHRAEUM);
		if (content && prose_active && (strstr(content, "<tool") || strstr(content, "<invoke name="))) {
			pthread_mutex_lock(&data->lock);
			aiPushMessage(data, "assistant", content);
			pthread_mutex_unlock(&data->lock);
			int n = hkReactToolExecAll(data, content);
			free(content);
			free(full_response);
			if (n == 0) break;  /* no parsable tool blocks — treat as final answer */
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
			} else if (full_response) {
				char err[256];
				snprintf(err, sizeof(err), "Error: empty response (first 180b: %.180s)", full_response);
				aiAddHistory(data, err);
			} else {
				aiAddHistory(data, "Error: empty response");
			}
			pthread_mutex_unlock(&data->lock);
		}

		free(content);
		free(full_response);
		break;
	}

	pthread_mutex_lock(&data->lock);
	if (iter >= max_iters && used_tool) aiAddHistory(data, "(tool loop cap reached)");
	data->last_turn_ms = clWallMs() - data->turn_start_ms;
	data->streaming = 0;
	pthread_mutex_unlock(&data->lock);

	if (E.pipe_mode) {
		char json[256];
		snprintf(json, sizeof(json),
			"{\"type\":\"done\",\"turn\":%d,\"in\":%d,\"out\":%d,\"session\":\"%s\"}",
			E.session_turn_count,
			data->last_in_tokens, data->last_out_tokens,
			E.session_id ? E.session_id : "");
		clPipeEmitRaw(json);
	}

	return NULL;
}

static void aiWorkerSend(aiData *data) {
	if (!data || data->streaming) return;
	data->streaming = 1;
	if (pthread_create(&data->worker_thread, NULL, aiWorkerThread, data) == 0) {
		/* joinable so REPL can wait for completion */
	} else {
		data->streaming = 0;
	}
}

/*** slash commands ***/
static int hkHandleSlash(aiData *data, const char *prompt) {
	/* Accept both `:` (primary, vim/hako style) and `/` (legacy alias). */
	if (prompt[0] != '/' && prompt[0] != ':') return 0;
	const char *cmd = prompt + 1;
	const char *arg = strchr(cmd, ' ');
	int cmdlen = arg ? (arg - cmd) : (int)strlen(cmd);
	if (arg) { while (*arg == ' ') arg++; }

	if (strncmp(cmd, "help", cmdlen) == 0 && cmdlen == 4) {
		aiAddHistory(data, ":help  :clear  :retry  :edit  :undo  :usage  :q");
		aiAddHistory(data, ":providers  :models  :provider <name>  :model <id>");
		aiAddHistory(data, ":login [<provider>]  :logout [<provider>]  :accounts");
		aiAddHistory(data, ":history [local|global]  :skills [reload]");
		aiAddHistory(data, ":skill install <url>  :skill uninstall <name>");
		aiAddHistory(data, ":tools on|off  :toolgate on|off  :toolmode native|prose  :trust [revoke]");
		aiAddHistory(data, ":sessions  :resume <id>  :session [new]");
		aiAddHistory(data, "(`/` still works as alias for muscle memory)");
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
	if (strncmp(cmd, "login", cmdlen) == 0 && cmdlen == 5) {
		const char *prov = (arg && *arg) ? arg : hkProviderName(E.ai_provider_type);
		if (!prov || strcmp(prov, "none") == 0) {
			aiAddHistory(data, "usage: :login <provider>");
			aiAddHistory(data, "  OAuth (subscription/account-bound):");
			aiAddHistory(data, "    anthropic          Claude Pro/Max");
			aiAddHistory(data, "    copilot            GitHub Copilot Pro/Business");
			aiAddHistory(data, "    github-models      free GitHub Models tier");
			aiAddHistory(data, "    openrouter         PKCE — auto-issue OR API key");
			aiAddHistory(data, "  paste API key (input hidden):");
			aiAddHistory(data, "    anthropic-api, openai, gemini, groq, cerebras,");
			aiAddHistory(data, "    deepseek, mistral, together, fireworks, xai, openrouter-api, custom");
			aiAddHistory(data, "  local (no auth):");
			aiAddHistory(data, "    ollama, ollamacloud");
			return 1;
		}
		if (strcmp(prov, "ollama") == 0 || strcmp(prov, "local") == 0 || strcmp(prov, "koi") == 0) {
			hkApplyProviderAlias(prov);
			hkSaveSession();
			aiAddHistory(data, "ollama is local — no API key needed.");
			aiAddHistory(data, "ensure `ollama serve` is running, then :models to list, :model <id> to pick.");
			return 1;
		}
		/* Anthropic OAuth — sign in with Claude Pro / Max subscription. No paste fallback.
		   For API-key paste use `/login anthropic-api`. */
		if (strcmp(prov, "anthropic") == 0 || strcmp(prov, "claude") == 0) {
			hkApplyProviderAlias("anthropic");
			clOAuthAnthropic(data);
			return 1;
		}
		if (strcmp(prov, "anthropic-api") == 0 || strcmp(prov, "claude-api") == 0) {
			/* Force-route to API-key paste flow below by retargeting provider name. */
			prov = "anthropic";
			hkApplyProviderAlias("anthropic");
		}
		/* GitHub Copilot OAuth — device flow + Copilot session token exchange. */
		if (strcmp(prov, "github-copilot") == 0 || strcmp(prov, "copilot") == 0) {
			clOAuthGithubCopilot(data);
			return 1;
		}
		/* GitHub Models OAuth — device flow, GH token used as Bearer (free tier). */
		if (strcmp(prov, "github-models") == 0 || strcmp(prov, "ghmodels") == 0) {
			clOAuthGithubModels(data);
			return 1;
		}
		/* OpenRouter PKCE — auto-issue user-scoped API key. */
		if (strcmp(prov, "openrouter") == 0) {
			if (clOAuthOpenRouter(data) == 0) return 1;
			aiAddHistory(data, "(OAuth aborted — try :login openrouter-api to paste an existing key)");
			return 1;
		}
		if (strcmp(prov, "openrouter-api") == 0) {
			prov = "openrouter";
			hkApplyProviderAlias("openrouter");
		}
		const char *url = clProviderConsoleUrl(prov);
		if (!url && strcmp(prov, "custom") != 0) {
			char msg[128];
			snprintf(msg, sizeof(msg), "no console URL for '%s'", prov);
			aiAddHistory(data, msg);
			return 1;
		}
		if (url) {
			char msg[256];
			snprintf(msg, sizeof(msg), "opening %s", url);
			aiAddHistory(data, msg);
			clOpenUrl(url);
		} else {
			aiAddHistory(data, "custom provider — set ai_endpoint via /provider or .hakorc");
		}
		printf("  paste API key (input hidden): ");
		fflush(stdout);
		char key[1024];
		clReadHidden(key, sizeof(key));
		if (!key[0]) { aiAddHistory(data, "(empty key, not saved)"); return 1; }
		free(E.ai_api_key);
		E.ai_api_key = strdup(key);
		hkApplyProviderAlias(prov);
		clCredsCaptureCurrent();
		clCredsSave();
		hkSaveSession();
		char msg[160];
		snprintf(msg, sizeof(msg), "key saved for %s (~/.hako/credentials, mode 0600, obfuscated)", hkProviderName(E.ai_provider_type));
		aiAddHistory(data, msg);
		return 1;
	}
	if (strncmp(cmd, "logout", cmdlen) == 0 && cmdlen == 6) {
		const char *prov = (arg && *arg) ? arg : hkProviderName(E.ai_provider_type);
		if (!prov || !strcmp(prov, "none")) { aiAddHistory(data, "no active provider"); return 1; }
		clCred *c = clCredsFind(prov);
		if (c) {
			free(c->api_key); c->api_key = NULL;
			free(c->oauth_refresh); c->oauth_refresh = NULL;
			c->oauth_expires_at = 0;
		}
		if (!strcmp(prov, hkProviderName(E.ai_provider_type))) {
			free(E.ai_api_key); E.ai_api_key = NULL;
			free(E.ai_oauth_refresh); E.ai_oauth_refresh = NULL;
			free(E.ai_oauth_provider); E.ai_oauth_provider = NULL;
			E.ai_oauth_expires_at = 0;
		}
		clCredsSave();
		char msg[128];
		snprintf(msg, sizeof(msg), "logged out: %s", prov);
		aiAddHistory(data, msg);
		return 1;
	}
	if (strncmp(cmd, "accounts", cmdlen) == 0 && cmdlen == 8) {
		if (cl_creds_n == 0) { aiAddHistory(data, "(no saved logins — use :login <provider>)"); return 1; }
		aiAddHistory(data, "saved logins:");
		for (int i = 0; i < cl_creds_n; i++) {
			clCred *c = &cl_creds[i];
			if (!c->api_key && !c->oauth_refresh) continue;
			const char *kind = c->oauth_refresh ? "oauth" : "key";
			const char *active = !strcmp(c->provider, hkProviderName(E.ai_provider_type)) ? " *" : "";
			char line[128];
			snprintf(line, sizeof(line), "  %s (%s)%s", c->provider, kind, active);
			aiAddHistory(data, line);
		}
		return 1;
	}
	if (strncmp(cmd, "clear", cmdlen) == 0 && cmdlen == 5) {
		for (int i = 0; i < data->history_count; i++) free(data->history[i]);
		memset(data->history_role, 0, AI_HISTORY_MAX);
		data->history_count = 0;
		printf("\x1b[2J\x1b[H");
		fflush(stdout);
		aiAddHistory(data, "(cleared)");
		return 1;
	}
	if (strncmp(cmd, "model", cmdlen) == 0 && cmdlen == 5) {
		if (arg && *arg) {
			/* Guard rails for hako tiers not yet shipped. Picking them sets the model
			   but inference would 404; warn instead so the user knows it's queued. */
			if (!strncmp(arg, "hako-koi-v", 10) && strncmp(arg, "hako-koi-mini", 13)) {
				aiAddHistory(data, "hako-koi-v* is queued — real 14B/32B fine-tune lands after rented-GPU run.");
				aiAddHistory(data, "available today: hako-sho-stock (3B), hako-koi-mini-stock (7B).");
				return 1;
			}
			if (!strncmp(arg, "hako-samurai", 12)) {
				aiAddHistory(data, "hako-samurai is reserved — 50B+ max tier, waits on hardware.");
				aiAddHistory(data, "available today: hako-sho-stock (3B), hako-koi-mini-stock (7B).");
				return 1;
			}
			free(E.ai_model);
			E.ai_model = strdup(arg);
			hkSaveSession();
			char msg[256];
			snprintf(msg, sizeof(msg), "model: %s (saved)", E.ai_model);
			aiAddHistory(data, msg);
		} else {
			char msg[256];
			snprintf(msg, sizeof(msg), "model: %s", E.ai_model ? E.ai_model : "(unset)");
			aiAddHistory(data, msg);
		}
		return 1;
	}
	if (strncmp(cmd, "models", cmdlen) == 0 && cmdlen == 6) {
		const char *prov = hkProviderName(E.ai_provider_type);
		const char *endpoint = E.ai_endpoint;
		/* Ollama / local: live query against /api/tags. Otherwise: curated suggestions. */
		int is_ollama = HK_IS_OLLAMA_WIRE(E.ai_provider_type);
		if (is_ollama) {
			if (!endpoint || !*endpoint) endpoint = "http://localhost:11434";
			char curlcmd[512];
			snprintf(curlcmd, sizeof(curlcmd),
				"curl -s --max-time 3 %s/api/tags 2>/dev/null", endpoint);
			FILE *fp = popen(curlcmd, "r");
			if (!fp) { aiAddHistory(data, "/models: popen failed"); return 1; }
			size_t cap = 8192, len = 0;
			char *buf = malloc(cap);
			if (!buf) { pclose(fp); return 1; }
			size_t got;
			while ((got = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
				len += got;
				if (len + 1 >= cap) {
					if (cap >= (1u << 22)) break;
					cap *= 2;
					char *nb = realloc(buf, cap);
					if (!nb) break;
					buf = nb;
				}
			}
			buf[len] = '\0';
			pclose(fp);
			if (len == 0) {
				char msg[256];
				snprintf(msg, sizeof(msg), "/models: no response from %s (is `ollama serve` running?)", endpoint);
				aiAddHistory(data, msg);
				free(buf); return 1;
			}
			int is_mithraeum = (E.ai_provider_type == AI_PROVIDER_MITHRAEUM);
			int count = 0;
			const char *p = buf;
			if (is_mithraeum) {
				aiAddHistory(data, "hako family (mithraeum runtime):");
				aiAddHistory(data, "  available now:");
			} else {
				char hdr[128];
				snprintf(hdr, sizeof(hdr), "installed locally on %s:", endpoint);
				aiAddHistory(data, hdr);
			}
			while ((p = strstr(p, "\"name\":\"")) != NULL) {
				p += 8;
				const char *e = p;
				while (*e && *e != '"') e++;
				if (!*e) break;
				int n = (int)(e - p);
				if (n > 200) n = 200;
				/* Mithraeum: only surface hako-* models; hide the unrelated ollama installs. */
				if (is_mithraeum && (n < 5 || strncmp(p, "hako-", 5) != 0)) {
					p = e;
					continue;
				}
				int active = E.ai_model && (int)strlen(E.ai_model) == n && !strncmp(E.ai_model, p, n);
				char line[256];
				snprintf(line, sizeof(line), "    %s %.*s", active ? "◎" : " ", n, p);
				aiAddHistory(data, line);
				count++;
				p = e;
			}
			if (is_mithraeum) {
				if (count == 0) {
					aiAddHistory(data, "    (none — `hakm pull sho` or `hakm pull koi-mini` to install)");
				}
				aiAddHistory(data, "  queued (need rented GPU):");
				aiAddHistory(data, "    hako-sho-v0.0.1         first fine-tune (mithraeum docs + C/Zig/asm)");
				aiAddHistory(data, "    hako-koi-mini-v0.0.1    same recipe at 7B");
				aiAddHistory(data, "    hako-koi-v0.0.1         14B/32B fine-tune");
				aiAddHistory(data, "  reserved:");
				aiAddHistory(data, "    hako-samurai-v0.0.1     50B+ max tier, waits on hardware");
			} else if (count == 0) {
				aiAddHistory(data, "  (none — `ollama pull <model>` to add one)");
			} else {
				char msg[64];
				snprintf(msg, sizeof(msg), "%d local model(s). /model <name> to select.", count);
				aiAddHistory(data, msg);
			}
			free(buf);
			return 1;
		}
		/* Curated suggestions per provider. Not exhaustive; meant as starting points. */
		struct { const char *prov; const char *models; } sugg[] = {
			{ "anthropic",      "claude-opus-4-7, claude-sonnet-4-6, claude-haiku-4-5-20251001, claude-opus-4-0, claude-sonnet-4-0" },
			{ "openai",         "gpt-4o, gpt-4o-mini, gpt-5, o1, o1-mini, gpt-4.1" },
			{ "gemini",         "gemini-2.5-pro, gemini-2.5-flash, gemini-1.5-pro, gemini-1.5-flash" },
			{ "google",         "gemini-2.5-pro, gemini-2.5-flash, gemini-1.5-pro" },
			{ "groq",           "llama-3.3-70b-versatile, llama-3.1-8b-instant, mixtral-8x7b-32768" },
			{ "cerebras",       "llama3.1-70b, llama3.1-8b, llama-3.3-70b" },
			{ "deepseek",       "deepseek-chat, deepseek-reasoner" },
			{ "mistral",        "mistral-large-latest, mistral-small-latest, codestral-latest" },
			{ "together",       "meta-llama/Llama-3.3-70B-Instruct-Turbo, Qwen/Qwen2.5-72B-Instruct-Turbo" },
			{ "fireworks",      "accounts/fireworks/models/llama-v3p1-70b-instruct, accounts/fireworks/models/qwen2p5-72b-instruct" },
			{ "openrouter",     "anthropic/claude-3.5-sonnet, openai/gpt-4o, meta-llama/llama-3.3-70b-instruct:free, deepseek/deepseek-chat:free" },
			{ "xai",            "grok-2, grok-2-mini, grok-beta" },
			{ "grok",           "grok-2, grok-2-mini, grok-beta" },
		};
		const char *models = NULL;
		const char *match_against = prov;
		if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-copilot")) match_against = "copilot";
		if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-models")) match_against = "github-models";
		/* Special-case OAuth providers: */
		if (!strcmp(match_against, "copilot")) {
			models = "gpt-4o, gpt-4o-mini, o1-mini, claude-3.5-sonnet, claude-3.7-sonnet";
		} else if (!strcmp(match_against, "github-models")) {
			models = "gpt-4o, gpt-4o-mini, meta-llama-3-70b-instruct, mistral-large, microsoft/phi-3.5-mini";
		}
		if (!models) {
			for (size_t i = 0; i < sizeof(sugg) / sizeof(sugg[0]); i++) {
				if (!strcmp(sugg[i].prov, match_against)) { models = sugg[i].models; break; }
			}
		}
		char hdr[128];
		snprintf(hdr, sizeof(hdr), "suggested models for %s%s:",
			match_against,
			(E.ai_oauth_provider && (!strcmp(match_against, "copilot") || !strcmp(match_against, "github-models"))) ? " (OAuth)" : "");
		aiAddHistory(data, hdr);
		if (!models) {
			aiAddHistory(data, "  (no curated list — check provider docs)");
		} else {
			/* Split by ", " and indent each. */
			const char *p = models;
			while (*p) {
				const char *e = strchr(p, ',');
				if (!e) e = p + strlen(p);
				while (*p == ' ') p++;
				int active = E.ai_model && (e - p == (long)strlen(E.ai_model)) && !strncmp(E.ai_model, p, e - p);
				char line[256];
				snprintf(line, sizeof(line), "  %s %.*s", active ? "◎" : " ", (int)(e - p), p);
				aiAddHistory(data, line);
				if (!*e) break;
				p = e + 1;
			}
		}
		aiAddHistory(data, ":model <id> to select.  :providers to see all providers.");
		return 1;
	}
	if (strncmp(cmd, "providers", cmdlen) == 0 && cmdlen == 9) {
		struct row { const char *name; const char *desc; };
		struct group { const char *header; struct row rows[10]; };
		struct group groups[] = {
			{ "Mithraeum (local-first, no auth, no cloud):", {
				{ "mithraeum",       "hako family — sho / koi-mini / koi / samurai" },
				{ NULL, NULL }
			} },
			{ "OAuth (subscription / account-bound):", {
				{ "anthropic",       "Claude Pro/Max — sign in with claude.ai" },
				{ "copilot",         "GitHub Copilot Pro/Business — device flow" },
				{ "github-models",   "free GitHub Models tier — device flow" },
				{ "openrouter",      "PKCE — auto-issue OR API key" },
				{ NULL, NULL }
			} },
			{ "Local (no auth):", {
				{ "ollama",          "run `ollama serve` first" },
				{ "ollamacloud",     "hosted Ollama (ollama.com)" },
				{ NULL, NULL }
			} },
			{ "Pay-per-token (API key paste):", {
				{ "anthropic-api",   "Claude API key (separate from sub)" },
				{ "openai",          "ChatGPT API" },
				{ "gemini",          "Google AI Studio — generous free tier" },
				{ "groq",            "fastest Llama hosting — free tier" },
				{ "cerebras",        "ultra-fast inference — free tier" },
				{ "deepseek",        "DeepSeek API" },
				{ "xai",             "Grok via xAI API (api.x.ai)" },
				{ "openrouter-api",  "paste existing OR key (vs PKCE issue)" },
				{ "mistral, together, fireworks, custom", "more OpenAI-compat" },
				{ NULL, NULL }
			} },
		};
		const char *active_prov = hkProviderName(E.ai_provider_type);
		const char *active_oauth = E.ai_oauth_provider;
		for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
			aiAddHistory(data, groups[g].header);
			for (size_t r = 0; r < 10 && groups[g].rows[r].name; r++) {
				const char *n = groups[g].rows[r].name;
				int logged_in = 0, is_active = 0;
				/* Strip aliases (only check the first word before comma) for status checks. */
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
		aiAddHistory(data, ":login <provider> to authenticate.  :models for model suggestions.");
		aiAddHistory(data, "(◎ = active, * = saved login)");
		return 1;
	}
	if (strncmp(cmd, "provider", cmdlen) == 0 && cmdlen == 8) {
		if (arg && *arg) {
			enum aiProviderType t = hkParseProvider(arg);
			if (t == AI_PROVIDER_NONE) {
				aiAddHistory(data, "unknown. valid: ollama, anthropic, openai, gemini/google,");
				aiAddHistory(data, "  groq, cerebras, deepseek, mistral, together, fireworks,");
				aiAddHistory(data, "  openrouter, xai/grok, custom");
			} else {
				enum aiProviderType prev = E.ai_provider_type;
				/* Snapshot outgoing provider's secrets so a later swap back finds them. */
				clCredsCaptureCurrent();
				hkApplyProviderAlias(arg);
				/* Restore (or clear) secrets for the incoming provider. */
				clCredsRestoreFor(hkProviderName(E.ai_provider_type));
				/* Wire-format change → flatten tool-bearing messages so the new
				   provider sees a clean role/content history it can parse. */
				if (prev != E.ai_provider_type) {
					pthread_mutex_lock(&data->lock);
					int before = data->message_count;
					aiFlattenMessages(data);
					int after = data->message_count;
					pthread_mutex_unlock(&data->lock);
					if (before != after) {
						char fmsg[128];
						snprintf(fmsg, sizeof(fmsg), "(flattened %d tool turn(s) for swap)", before - after);
						aiAddHistory(data, fmsg);
					}
				}
				clCredsSave();
				hkSaveSession();
				char msg[256];
				snprintf(msg, sizeof(msg), "provider: %s (saved)%s%s",
					hkProviderName(t),
					hkProviderDefaultEndpoint(arg) ? " endpoint=" : "",
					hkProviderDefaultEndpoint(arg) ? hkProviderDefaultEndpoint(arg) : "");
				aiAddHistory(data, msg);
			}
		} else {
			char msg[256];
			snprintf(msg, sizeof(msg), "provider: %s", hkProviderName(E.ai_provider_type));
			aiAddHistory(data, msg);
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
			int n = hkLoadSkills(data);
			char msg[64];
			snprintf(msg, sizeof(msg), "reloaded %d skill(s)", n);
			aiAddHistory(data, msg);
			return 1;
		}
		char dir[512];
		hkClawDirPath(dir, sizeof(dir));
		char skills[512];
		snprintf(skills, sizeof(skills), "%s/skills", dir);
		DIR *d = opendir(skills);
		if (!d) { aiAddHistory(data, "no skills dir (~/.hako/skills)"); return 1; }
		struct dirent *e;
		int n = 0;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			aiAddHistory(data, e->d_name);
			n++;
		}
		closedir(d);
		if (n == 0) aiAddHistory(data, "(no skills)");
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
			if (unlink(path) == 0) {
				int n = hkLoadSkills(data);
				char msg[128];
				snprintf(msg, sizeof(msg), "uninstalled: %s (%d remain)", name, n);
				aiAddHistory(data, msg);
			} else {
				aiAddHistory(data, "skill not found");
			}
			return 1;
		}
		if (!arg || strncmp(arg, "install ", 8) != 0) {
			aiAddHistory(data, "usage: /skill install <url>  |  /skill uninstall <name>");
			return 1;
		}
		const char *url = arg + 8;
		while (*url == ' ') url++;
		if (!*url) { aiAddHistory(data, "usage: /skill install <url>"); return 1; }
		char dir[512];
		hkClawDirPath(dir, sizeof(dir));
		char skills[512];
		snprintf(skills, sizeof(skills), "%s/skills", dir);
		mkdir(dir, 0755);
		mkdir(skills, 0755);
		const char *slash = strrchr(url, '/');
		const char *name = slash ? slash + 1 : url;
		char outpath[1024];
		snprintf(outpath, sizeof(outpath), "%s/%s", skills, name);
		int nlen = strlen(name);
		if (nlen < 4 || strcmp(name + nlen - 3, ".md") != 0) {
			snprintf(outpath, sizeof(outpath), "%s/%s.md", skills, name);
		}
		char cmdbuf[2048];
		snprintf(cmdbuf, sizeof(cmdbuf), "curl -sfL -o %s %s", outpath, url);
		int rc = system(cmdbuf);
		if (rc != 0) { aiAddHistory(data, "download failed"); return 1; }
		int n = hkLoadSkills(data);
		char msg[128];
		snprintf(msg, sizeof(msg), "installed: %s (%d total)", name, n);
		aiAddHistory(data, msg);
		return 1;
	}
	if (strncmp(cmd, "tools", cmdlen) == 0 && cmdlen == 5) {
		int changed = 0;
		if (arg && strcmp(arg, "on") == 0) { E.ai_tools_enabled = 1; changed = 1; }
		else if (arg && strcmp(arg, "off") == 0) { E.ai_tools_enabled = 0; changed = 1; }
		if (changed) hkSaveSession();
		char msg[64];
		snprintf(msg, sizeof(msg), "tools: %s%s", E.ai_tools_enabled ? "on" : "off", changed ? " (saved)" : "");
		aiAddHistory(data, msg);
		return 1;
	}
	if (strncmp(cmd, "toolgate", cmdlen) == 0 && cmdlen == 8) {
		int changed = 0;
		if (arg && strcmp(arg, "on") == 0) { E.ai_tool_gate = 1; changed = 1; }
		else if (arg && strcmp(arg, "off") == 0) { E.ai_tool_gate = 0; changed = 1; }
		if (changed) hkSaveSession();
		char msg[128];
		snprintf(msg, sizeof(msg),
			"toolgate: %s%s — drops tool schema when user msg has no tool-keyword (helps small models).",
			E.ai_tool_gate ? "on" : "off", changed ? " (saved)" : "");
		aiAddHistory(data, msg);
		return 1;
	}
	if (strncmp(cmd, "theme", cmdlen) == 0 && cmdlen == 5) {
		if (!arg || !*arg) {
			char msg[256];
			int o = snprintf(msg, sizeof(msg), "theme: %s — available:", TH_ACTIVE);
			for (int i = 0; i < TH_PRESET_COUNT && o < (int)sizeof(msg) - 16; i++) {
				o += snprintf(msg + o, sizeof(msg) - o, " %s", TH_PRESETS[i].name);
			}
			aiAddHistory(data, msg);
			return 1;
		}
		int found = 0;
		for (int i = 0; i < TH_PRESET_COUNT; i++) {
			if (!strcmp(TH_PRESETS[i].name, arg)) { found = 1; break; }
		}
		if (!found) { aiAddHistory(data, "theme: unknown. try :theme without args to list."); return 1; }
		clThemeApply(arg);
		hkSaveSession();
		char msg[64]; snprintf(msg, sizeof(msg), "theme: %s (saved)", TH_ACTIVE);
		aiAddHistory(data, msg);
		return 1;
	}
	if (strncmp(cmd, "toolmode", cmdlen) == 0 && cmdlen == 8) {
		int changed = 0;
		if (arg && (!strcmp(arg, "native") || !strcmp(arg, "fn"))) { E.ai_toolmode = 0; changed = 1; }
		else if (arg && (!strcmp(arg, "prose") || !strcmp(arg, "react") || !strcmp(arg, "xml"))) { E.ai_toolmode = 1; changed = 1; }
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
	if (strncmp(cmd, "usage", cmdlen) == 0 && cmdlen == 5) {
		if (arg && !strcmp(arg, "reset")) {
			data->last_in_tokens = data->last_out_tokens = 0;
			data->total_in_tokens = data->total_out_tokens = 0;
			aiAddHistory(data, "(usage counters reset)");
			return 1;
		}
		char msg[256];
		snprintf(msg, sizeof(msg), "provider: %s", hkProviderName(E.ai_provider_type));
		aiAddHistory(data, msg);
		snprintf(msg, sizeof(msg), "model:    %s", E.ai_model ? E.ai_model : "(unset)");
		aiAddHistory(data, msg);
		snprintf(msg, sizeof(msg), "tools:    %s", E.ai_tools_enabled ? "on" : "off");
		aiAddHistory(data, msg);
		snprintf(msg, sizeof(msg), "trust:    %s", hkProjectTrusted() ? "granted" : "not granted");
		aiAddHistory(data, msg);
		snprintf(msg, sizeof(msg), "skills:   %d loaded", hkLoadSkills(data));
		aiAddHistory(data, msg);
		snprintf(msg, sizeof(msg), "stream:   %s", E.ai_stream ? "on" : "off");
		aiAddHistory(data, msg);
		snprintf(msg, sizeof(msg), "session:  %s (%d turns)",
			E.session_id ? E.session_id : "(none)", E.session_turn_count);
		aiAddHistory(data, msg);
		char hpath[PATH_MAX];
		hkHistoryPath(hpath, sizeof(hpath));
		snprintf(msg, sizeof(msg), "history:  %s", hpath);
		aiAddHistory(data, msg);
		snprintf(msg, sizeof(msg), "tokens:   last %d in / %d out  total %ld in / %ld out (cap %d)",
			data->last_in_tokens, data->last_out_tokens,
			data->total_in_tokens, data->total_out_tokens, E.ai_max_tokens);
		aiAddHistory(data, msg);
		double cost = hkSessionCostUSD(data);
		const char *flat = hkFreeTierLabel();
		if (flat && !strcmp(flat, "sub")) {
			aiAddHistory(data, "cost:     bundled in subscription");
		} else if (flat && !strcmp(flat, "free")) {
			aiAddHistory(data, "cost:     free tier (rate-limited by provider)");
		} else if (flat) {
			aiAddHistory(data, "cost:     $0 (local)");
		} else if (cost < 0) {
			snprintf(msg, sizeof(msg), "cost:     (no price entry for model '%s')", E.ai_model ? E.ai_model : "");
			aiAddHistory(data, msg);
		} else {
			snprintf(msg, sizeof(msg), "cost:     $%.4f estimated (session-only; provider invoice is authoritative)", cost);
			aiAddHistory(data, msg);
		}
		return 1;
	}
	if (strncmp(cmd, "sessions", cmdlen) == 0 && cmdlen == 8) {
		char path[512];
		hkHistoryPath(path, sizeof(path));
		FILE *fp = fopen(path, "r");
		if (!fp) { aiAddHistory(data, "(no history)"); return 1; }
		char ids[16][32];
		long lasts[16];
		int counts[16];
		char firsts[16][80];
		int n = 0;
		char *line = NULL;
		size_t cap = 0;
		while (getline(&line, &cap, fp) != -1) {
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
		free(line); fclose(fp);
		if (n == 0) { aiAddHistory(data, "(no sessions)"); return 1; }
		long now = (long)time(NULL);
		for (int i = 0; i < n; i++) {
			long age = now - lasts[i];
			char unit; long val;
			if (age < 3600) { val = age / 60; unit = 'm'; }
			else if (age < 86400) { val = age / 3600; unit = 'h'; }
			else { val = age / 86400; unit = 'd'; }
			char msg[200];
			const char *cur = (E.session_id && strcmp(E.session_id, ids[i]) == 0) ? "* " : "  ";
			snprintf(msg, sizeof(msg), "%s%s %ld%c %dt %.40s", cur, ids[i], val, unit, counts[i], firsts[i]);
			aiAddHistory(data, msg);
		}
		aiAddHistory(data, "(/resume <id> to switch)");
		return 1;
	}
	if (strncmp(cmd, "resume", cmdlen) == 0 && cmdlen == 6) {
		if (!arg || !*arg) { aiAddHistory(data, "usage: /resume <id>"); return 1; }
		free(E.session_id);
		E.session_id = strdup(arg);
		E.session_resumed = 1;
		E.session_started = (long)time(NULL);
		hkSaveSession();
		for (int i = 0; i < data->history_count; i++) free(data->history[i]);
		memset(data->history_role, 0, AI_HISTORY_MAX);
		data->history_count = 0;
		aiFreeMessages(data);
		char msg[128];
		snprintf(msg, sizeof(msg), "resumed: %s", E.session_id);
		aiAddHistory(data, msg);
		hkLoadHistoryTail(data, 200);
		return 1;
	}
	if (strncmp(cmd, "session", cmdlen) == 0 && cmdlen == 7) {
		if (arg && strcmp(arg, "new") == 0) {
			for (int i = 0; i < data->history_count; i++) free(data->history[i]);
			memset(data->history_role, 0, AI_HISTORY_MAX);
			data->history_count = 0;
			aiFreeMessages(data);
			E.session_started = (long)time(NULL);
			E.session_turn_count = 0;
			E.session_resumed = 0;
			hkGenSessionId();
			hkSaveSession();
			char msg[128];
			snprintf(msg, sizeof(msg), "new session: %s", E.session_id);
			aiAddHistory(data, msg);
			return 1;
		}
		char msg[256];
		snprintf(msg, sizeof(msg), "id:      %s", E.session_id ? E.session_id : "(none)");
		aiAddHistory(data, msg);
		long age = (long)time(NULL) - E.session_started;
		snprintf(msg, sizeof(msg), "started: %ld min ago", age / 60);
		aiAddHistory(data, msg);
		snprintf(msg, sizeof(msg), "turns:   %d", E.session_turn_count);
		aiAddHistory(data, msg);
		snprintf(msg, sizeof(msg), "state:   %s", E.session_resumed ? "resumed" : "new");
		aiAddHistory(data, msg);
		aiAddHistory(data, "(/session new to reset)");
		return 1;
	}
	aiAddHistory(data, "unknown command (/help)");
	return 1;
}

/*** .hakorc parser ***/
static void clLoadRc(void) {
	const char *home = getenv("HOME");
	if (!home) return;
	char path[512];
	snprintf(path, sizeof(path), "%s/.hakorc", home);
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, fp) != -1) {
		char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
		if (line[0] == '#' || line[0] == '\0') continue;
		char *eq = strchr(line, '='); if (!eq) continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;
		if (strcmp(key, "ai_provider") == 0) hkApplyProviderAlias(val);
		else if (strcmp(key, "ai_api_key") == 0) { free(E.ai_api_key); E.ai_api_key = strdup(val); }
		else if (strcmp(key, "ai_endpoint") == 0) { free(E.ai_endpoint); E.ai_endpoint = strdup(val); }
		else if (strcmp(key, "ai_model") == 0) { free(E.ai_model); E.ai_model = strdup(val); }
		else if (strcmp(key, "ai_max_tokens") == 0) E.ai_max_tokens = atoi(val);
		else if (strcmp(key, "ai_tools_enabled") == 0) E.ai_tools_enabled = atoi(val) ? 1 : 0;
		else if (strcmp(key, "ai_stream") == 0) E.ai_stream = atoi(val) ? 1 : 0;
		else if (strcmp(key, "ai_autowrite") == 0) E.ai_autowrite = atoi(val) ? 1 : 0;
		else if (strcmp(key, "anim_style") == 0) {
			E.anim_force_style = -1;
			for (int i = 0; i < CL_ANIM_COUNT; i++) {
				if (strcmp(val, CL_ANIMS[i].name) == 0) { E.anim_force_style = i; break; }
			}
		}
	}
	free(line);
	fclose(fp);
}

/*** init / cleanup ***/
static void clInitConfig(void) {
	memset(&E, 0, sizeof(E));
	E.ai_provider_type = AI_PROVIDER_NONE;
	E.ai_temperature = 70;
	E.ai_max_tokens = 2048;
	E.ai_tools_enabled = 1;
	E.ai_tool_gate = 1;
	E.ai_stream = 1;
	E.ai_autowrite = 1;
	E.anim_force_style = -1;
#ifndef _WIN32
	E.color_enabled = isatty(STDOUT_FILENO) ? 1 : 0;
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

/*** signal ***/
static void clSigint(int sig) {
	(void)sig;
	E.interrupt = 1;
}

/*** termios raw line editor ***/

/* Defined as forward decl earlier for slash command access. */

#ifndef _WIN32
static struct termios cl_orig_termios;
static int cl_raw_active = 0;

static int clEnableRaw(void) {
	if (!isatty(STDIN_FILENO)) return -1;
	if (tcgetattr(STDIN_FILENO, &cl_orig_termios) == -1) return -1;
	struct termios raw = cl_orig_termios;
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
	raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;
	cl_raw_active = 1;
	if (write(STDOUT_FILENO, "\x1b[?2004h", 8) < 0) {}   /* enable bracketed paste */
	return 0;
}

static void clDisableRaw(void) {
	if (cl_raw_active) {
		if (write(STDOUT_FILENO, "\x1b[?2004l", 8) < 0) {} /* disable bracketed paste */
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &cl_orig_termios);
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
	FILE *fp = fopen(p, "r");
	if (!fp) return;
	char *line = NULL; size_t cap = 0;
	while (getline(&line, &cap, fp) != -1) {
		size_t n = strlen(line);
		if (n && line[n-1] == '\n') line[--n] = '\0';
		if (!n) continue;
		if (cl_in_hist_n >= CL_INPUT_HIST_MAX) {
			free(cl_in_hist[0]);
			memmove(&cl_in_hist[0], &cl_in_hist[1], sizeof(cl_in_hist[0])*(CL_INPUT_HIST_MAX-1));
			cl_in_hist_n--;
		}
		cl_in_hist[cl_in_hist_n++] = strdup(line);
	}
	free(line);
	fclose(fp);
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
	FILE *fp = fopen(p, "a");
	if (fp) { fprintf(fp, "%s\n", s); fclose(fp); }
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
#ifndef _WIN32
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col > 0) return ws.ws_col;
#endif
	return 80;
}

/* Multi-row aware redraw (linenoise-style). Tracks rows used + cursor row across calls. */
static int cl_redraw_oldrows = 0;
static int cl_redraw_oldrpos = 0;

static void clRedrawReset(void) { cl_redraw_oldrows = 0; cl_redraw_oldrpos = 0; }

static void clAppend(char *ab, int *n, int cap, const char *s, int slen) {
	if (*n + slen >= cap) return;
	memcpy(ab + *n, s, slen);
	*n += slen;
}

/* Ghost-text: prefix-match input history (newest first) + slash/colon command
   list. Returns malloc'd suffix to display past cursor, or NULL.  Caller frees. */
static char *clGhostSuffix(const char *buf, int len) {
	/* Only suggest for slash/colon commands. Regular prose: no ghost. Avoids
	   flicker on every keystroke during normal chat. */
	if (len < 2) return NULL;
	if (buf[0] != ':' && buf[0] != '/') return NULL;
	static const char *cmds[] = {
		"accounts","clear","edit","help","history","login","logout","model","models",
		"provider","providers","quit","resume","retry","session","sessions","skill","skills",
		"theme","tools","toolgate","toolmode","trust","undo","usage", NULL
	};
	const char *p = buf + 1;
	int pl = len - 1;
	/* Only one matching command? Show ghost. Multiple matches → ambiguous, no ghost. */
	const char *only = NULL; int matches = 0;
	for (int i = 0; cmds[i]; i++) {
		int cl = (int)strlen(cmds[i]);
		if (cl > pl && memcmp(cmds[i], p, pl) == 0) { only = cmds[i]; matches++; }
	}
	if (matches == 1) return strdup(only + pl);
	return NULL;
}

/* Most recent ghost so the TAB handler can accept it without recomputing. */
static char cl_last_ghost[512];

static void clRedrawLine(const char *prompt, const char *buf, int len, int cursor) {
	int plen = clVisibleLen(prompt);
	int cols = clTermCols();
	if (cols < 1) cols = 80;

	/* Compute ghost suffix only when cursor sits at end of buffer. */
	char *ghost = (cursor == len) ? clGhostSuffix(buf, len) : NULL;
	int glen = ghost ? (int)strlen(ghost) : 0;
	if (ghost) {
		if (glen >= (int)sizeof(cl_last_ghost)) glen = (int)sizeof(cl_last_ghost) - 1;
		memcpy(cl_last_ghost, ghost, glen);
		cl_last_ghost[glen] = '\0';
	} else {
		cl_last_ghost[0] = '\0';
	}

	int rows = (plen + len + glen + cols - 1) / cols;
	if (rows < 1) rows = 1;

	char ab[16384];
	int n = 0;
	char tmp[64];
	int t;

	clAppend(ab, &n, sizeof(ab), "\x1b[?25l", 6);   /* hide cursor while redrawing */

	/* From wherever cursor sits (we tracked old cursor row), drop to last row of prior edit area, then walk up
	   clearing each row. */
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

	/* Emit prompt + buffer. */
	clAppend(ab, &n, sizeof(ab), prompt, (int)strlen(prompt));
	clAppend(ab, &n, sizeof(ab), buf, len);

	/* Emit ghost suffix in dim color (TAB to accept). Visible chars count for cursor math. */
	if (ghost && glen > 0) {
		clAppend(ab, &n, sizeof(ab), TH_GHOST, (int)strlen(TH_GHOST));
		clAppend(ab, &n, sizeof(ab), ghost, glen);
		clAppend(ab, &n, sizeof(ab), ANSI_RESET, (int)strlen(ANSI_RESET));
	}

	/* If trailing content lands exactly at a column-edge, emit \n\r so terminal scrolls a fresh row. */
	int rows_after = rows;
	if (cursor == len && (len + glen) > 0 && (plen + len + glen) % cols == 0) {
		clAppend(ab, &n, sizeof(ab), "\n\r", 2);
		rows_after++;
	}

	/* Position cursor. */
	int rpos2 = (plen + cursor) / cols;
	int up    = (rows_after - 1) - rpos2;
	if (up > 0) {
		t = snprintf(tmp, sizeof(tmp), "\x1b[%dA", up);
		clAppend(ab, &n, sizeof(ab), tmp, t);
	}
	int col = (plen + cursor) % cols;
	clAppend(ab, &n, sizeof(ab), "\r", 1);
	if (col > 0) {
		t = snprintf(tmp, sizeof(tmp), "\x1b[%dC", col);
		clAppend(ab, &n, sizeof(ab), tmp, t);
	}

	clAppend(ab, &n, sizeof(ab), "\x1b[?25h", 6);   /* show cursor */

	cl_redraw_oldrows = rows_after;
	cl_redraw_oldrpos = rpos2;

	if (write(STDOUT_FILENO, ab, n) < 0) { free(ghost); return; }
	free(ghost);
}

/* Returns: >=0 = length of line, -1 = EOF, -2 = SIGINT/cancel (empty line). */
static int clReadLineRaw(const char *prompt, char *out, size_t cap) {
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
	int in_paste = 0;
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
		ssize_t r = read(STDIN_FILENO, &c, 1);
		if (r <= 0) {
			if (r < 0 && errno == EINTR) {
				clDisableRaw();
				if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
				out[0] = '\0';
				return -2;
			}
			clDisableRaw();
			return -1;
		}

		if ((c == '\r' || c == '\n') && !in_paste) {
			if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
			clDisableRaw();
			buf[len] = '\0';
			if ((size_t)len < cap) memcpy(out, buf, len + 1);
			else { memcpy(out, buf, cap - 1); out[cap-1] = '\0'; len = (int)cap - 1; }
			if (len > 0) clInputHistAppend(out);
			return len;
		}
		if ((c == '\r' || c == '\n') && in_paste) {
			/* Insert literal newline during paste (rendered as space — hako treats prompts as single-line). */
			if (len + 1 < (int)sizeof(buf)) {
				memmove(&buf[cur+1], &buf[cur], len - cur);
				buf[cur++] = ' ';
				len++; buf[len] = '\0';
				clRedrawLine(prompt, buf, len, cur);
			}
			continue;
		}

		if (c == 3) {  /* Ctrl-C */
			if (write(STDOUT_FILENO, "^C\r\n", 4) < 0) {}
			clDisableRaw();
			out[0] = '\0';
			return -2;
		}
		if (c == 4) {  /* Ctrl-D */
			if (len == 0) {
				clDisableRaw();
				if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
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
		if (c == 9) {
			/* TAB: first accept ghost-text if active (fish-style), else fall back
			   to slash-command / provider name completion. */
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
			/* TAB completion: commands + provider names after :login / :provider / :logout. */
			if ((buf[0] != '/' && buf[0] != ':') || cur != len) continue;
			static const char *slash_cmds[] = {
				"accounts", "clear", "edit", "help", "history", "login", "logout",
				"model", "models", "provider", "providers", "quit", "resume", "retry", "session",
				"sessions", "skill", "skills", "theme", "tools", "toolgate", "toolmode", "trust", "undo", "usage", NULL
			};
			static const char *provs[] = {
				"anthropic", "anthropic-api", "claude", "claude-api", "openai",
				"github-copilot", "copilot", "github-models", "ghmodels",
				"ollama", "ollamacloud", "ocloud", "local", "koi",
				"gemini", "google", "groq", "cerebras", "deepseek", "mistral",
				"together", "fireworks", "openrouter", "openrouter-api",
				"xai", "grok", "github", "custom", NULL
			};
			char prefix_char = buf[0];  /* preserve `:` or `/` user typed */
			const char *sp = strchr(buf, ' ');
			if (!sp) {
				const char *partial = buf + 1;
				size_t plen2 = strlen(partial);
				const char *only = NULL; int nm = 0;
				for (int i = 0; slash_cmds[i]; i++) {
					if (strncmp(slash_cmds[i], partial, plen2) == 0) { only = slash_cmds[i]; nm++; }
				}
				if (nm == 1) {
					size_t mlen = strlen(only);
					if (1 + mlen + 1 < sizeof(buf)) {
						buf[0] = prefix_char; memcpy(buf + 1, only, mlen);
						buf[1 + mlen] = ' '; buf[1 + mlen + 1] = '\0';
						len = cur = (int)(1 + mlen + 1);
					}
				}
			} else {
				int wants = !strncmp(buf + 1, "login ", 6) || !strncmp(buf + 1, "provider ", 9) || !strncmp(buf + 1, "logout ", 7);
				if (wants) {
					const char *arg2 = sp + 1; while (*arg2 == ' ') arg2++;
					size_t alen = strlen(arg2);
					const char *only = NULL; int nm = 0;
					for (int i = 0; provs[i]; i++) {
						if (strncmp(provs[i], arg2, alen) == 0) { only = provs[i]; nm++; }
					}
					if (nm == 1) {
						int prefix_len = (int)(arg2 - buf);
						size_t mlen = strlen(only);
						if (prefix_len + mlen + 1 < (int)sizeof(buf)) {
							memcpy(buf + prefix_len, only, mlen);
							buf[prefix_len + mlen] = '\0';
							len = cur = prefix_len + (int)mlen;
						}
					}
				}
			}
			clRedrawLine(prompt, buf, len, cur);
			continue;
		}
		if (c == 12) {
			if (write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7) < 0) {}
			clRedrawReset();
			clRedrawLine(prompt, buf, len, cur);
			continue;
		}
		if (c == 18) {
			/* Ctrl-R reverse-incremental search through input history. */
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
					/* Accept match into editor. */
					if (match && *match) {
						snprintf(buf, sizeof(buf), "%s", match);
						len = (int)strlen(buf); cur = len;
					}
					break;
				}
				if (rc == 27 || rc == 7) { /* Esc / ^G — cancel, keep current buf */
					break;
				}
				if (rc == 18) { /* ^R — find next older match */
					int start = match_idx - 1;
					while (start >= 0) {
						if (qlen == 0 || strstr(cl_in_hist[start], query)) { match_idx = start; break; }
						start--;
					}
					continue;
				}
				if (rc == 127 || rc == 8) {
					if (qlen > 0) { query[--qlen] = '\0'; }
					/* Restart search from end. */
					match_idx = cl_in_hist_n - 1;
					if (qlen > 0) {
						while (match_idx >= 0 && !strstr(cl_in_hist[match_idx], query)) match_idx--;
					}
					continue;
				}
				if ((unsigned char)rc < 32) {
					/* Other control char — exit search, do not deliver (keep simple). */
					break;
				}
				if (qlen + 1 < (int)sizeof(query)) {
					query[qlen++] = rc; query[qlen] = '\0';
					/* Search from match_idx downward for substring. */
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
			if (cur > 0) {
				memmove(&buf[cur-1], &buf[cur], len - cur);
				cur--; len--; buf[len] = '\0';
				clRedrawLine(prompt, buf, len, cur);
			}
			continue;
		}

		if (c == '\x1b') {
			char s1, s2;
			if (read(STDIN_FILENO, &s1, 1) != 1) continue;
			if (s1 != '[' && s1 != 'O') continue;
			if (read(STDIN_FILENO, &s2, 1) != 1) continue;
			if (s2 >= '0' && s2 <= '9') {
				char s3;
				if (read(STDIN_FILENO, &s3, 1) != 1) continue;
				/* Bracketed paste: \x1b[200~ ... \x1b[201~ */
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
						memmove(&buf[cur], &buf[cur+1], len - cur - 1);
						len--; buf[len] = '\0';
						clRedrawLine(prompt, buf, len, cur);
					}
				} else if ((s2 == '1' || s2 == '7') && s3 == '~') {
					cur = 0; clRedrawLine(prompt, buf, len, cur);
				} else if ((s2 == '4' || s2 == '8') && s3 == '~') {
					cur = len; clRedrawLine(prompt, buf, len, cur);
				}
				continue;
			}
			switch (s2) {
				case 'A':
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
					if (hist_idx < cl_in_hist_n) {
						hist_idx++;
						if (hist_idx == cl_in_hist_n) snprintf(buf, sizeof(buf), "%s", saved);
						else snprintf(buf, sizeof(buf), "%s", cl_in_hist[hist_idx]);
						len = (int)strlen(buf); cur = len;
						clRedrawLine(prompt, buf, len, cur);
					}
					break;
				case 'C': if (cur < len) { cur++; clRedrawLine(prompt, buf, len, cur); } break;
				case 'D': if (cur > 0) { cur--; clRedrawLine(prompt, buf, len, cur); } break;
				case 'H': cur = 0; clRedrawLine(prompt, buf, len, cur); break;
				case 'F': cur = len; clRedrawLine(prompt, buf, len, cur); break;
			}
			continue;
		}

		/* printable + UTF-8 multi-byte: insert at cursor */
		if ((unsigned char)c >= 32) {
			if (len + 1 >= (int)sizeof(buf)) continue;
			memmove(&buf[cur+1], &buf[cur], len - cur);
			buf[cur] = c;
			cur++; len++; buf[len] = '\0';
			clRedrawLine(prompt, buf, len, cur);
			continue;
		}
	}
}

/*** OAuth (Anthropic Claude Pro / Max subscription) ***/
/* PKCE auth-code flow with manual code paste. Lifted from Claude Code's own
   public client_id (also used by opencode + other community CLIs). No registration
   needed on user's side — they sign in with their existing Anthropic account.
   Access token used as Bearer on api.anthropic.com with `anthropic-beta:
   oauth-2025-04-20` header. Refresh token rotates the access token automatically
   via clOAuthEnsureFresh before each turn. */

#define HAKO_ANTHROPIC_CLIENT_ID  "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
#define HAKO_ANTHROPIC_REDIRECT   "https://console.anthropic.com/oauth/code/callback"
#define HAKO_ANTHROPIC_AUTH_URL   "https://claude.ai/oauth/authorize"
#define HAKO_ANTHROPIC_TOKEN_URL  "https://console.anthropic.com/v1/oauth/token"
#define HAKO_ANTHROPIC_SCOPE      "org:create_api_key user:profile user:inference"

/* GitHub Copilot — VS Code Copilot extension's public client_id. Device-flow OAuth on
   github.com, then exchange the GH access token for a short-lived Copilot session token
   at api.github.com/copilot_internal/v2/token. Copilot session token expires ~25 min;
   GH access token is long-lived and stored in ai_oauth_refresh.
   EDITOR_VER + PLUGIN_VER live at top of file so hkBuildCurlCmd can reference them. */
#define HAKO_COPILOT_CLIENT_ID    "Iv1.b507a08c87ecfe98"
#define HAKO_COPILOT_DEVICE_URL   "https://github.com/login/device/code"
#define HAKO_COPILOT_TOKEN_URL    "https://github.com/login/oauth/access_token"
#define HAKO_COPILOT_EXCHANGE_URL "https://api.github.com/copilot_internal/v2/token"

/* GitHub Models — free for all GitHub users (rate-limited). gh CLI public client_id
   (broad scope). Token used as Bearer against the Azure-hosted models endpoint. */
#define HAKO_GHMODELS_CLIENT_ID   "178c6fc778ccc68e1d6a"
#define HAKO_GHMODELS_ENDPOINT    "https://models.inference.ai.azure.com"

/* OpenRouter PKCE — no client_id needed; just a code_challenge + callback URL.
   Exchange at openrouter.ai/api/v1/auth/keys returns a user-scoped API key. */
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

/* curl POST helper for OAuth/exchange flows. Returns malloc'd response or NULL.
   extra_headers: additional `-H 'Key: Val'` blocks (may be NULL). */
static char *clCurlPost(const char *url, const char *json_body, const char *extra_headers) {
	char tmpl[] = "/tmp/hako-curl-XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) return NULL;
	if (write(fd, json_body, strlen(json_body)) < 0) { close(fd); unlink(tmpl); return NULL; }
	close(fd);
	char cmd[2048];
	snprintf(cmd, sizeof(cmd),
		"curl -s -X POST %s -A 'hako-code/%s' -H 'Content-Type: application/json' -H 'Accept: application/json' %s --data @%s 2>/dev/null",
		url, HAKO_VERSION, extra_headers ? extra_headers : "", tmpl);
	FILE *fp = popen(cmd, "r");
	if (!fp) { unlink(tmpl); return NULL; }
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	size_t n;
	while ((n = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
		len += n;
		if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
	}
	buf[len] = '\0';
	pclose(fp);
	unlink(tmpl);
	return buf;
}

/* curl GET helper. Returns malloc'd response or NULL. */
static char *clCurlGet(const char *url, const char *extra_headers) {
	char cmd[2048];
	snprintf(cmd, sizeof(cmd),
		"curl -s -X GET %s -A 'hako-code/%s' -H 'Accept: application/json' %s 2>/dev/null",
		url, HAKO_VERSION, extra_headers ? extra_headers : "");
	FILE *fp = popen(cmd, "r");
	if (!fp) return NULL;
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	size_t n;
	while ((n = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
		len += n;
		if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
	}
	buf[len] = '\0';
	pclose(fp);
	return buf;
}

/* GitHub Copilot device-flow OAuth + Copilot session token exchange. */
static int clOAuthGithubCopilot(aiData *data) {
	const char *client_id = clOAuthClientId("github-copilot");
	if (!client_id) { aiAddHistory(data, "OAuth: no copilot client_id"); return -1; }

	/* Step 1: device authorization. */
	char body[256];
	snprintf(body, sizeof(body), "{\"client_id\":\"%s\",\"scope\":\"read:user\"}", client_id);
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
	aiAddHistory(data, "Polling... (Ctrl+C to abort)");
	clOpenUrl(verify_uri);

	/* Step 2: poll token endpoint. */
	long start = (long)time(NULL);
	char *gh_token = NULL;
	const char *grant = "urn:ietf:params:oauth:grant-type:device_code";
	while (!E.interrupt && (long)time(NULL) - start < expires_in) {
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
			snprintf(line, sizeof(line), "OAuth: %s%s%s", err, desc ? ": " : "", desc ? desc : "");
			aiAddHistory(data, line);
			free(err); free(desc); free(tr);
			free(device_code); free(user_code); free(verify_uri);
			return -1;
		}
		if (err && !strcmp(err, "slow_down")) interval += 5;
		free(err); free(tr);
	}
	free(device_code); free(user_code); free(verify_uri);
	if (!gh_token) { aiAddHistory(data, "OAuth: timed out / aborted"); return -1; }

	/* Step 3: exchange GH access token for Copilot session token.
	   Store GH token in ai_oauth_refresh (long-lived); Copilot session in ai_api_key. */
	free(E.ai_oauth_refresh); E.ai_oauth_refresh = gh_token;
	free(E.ai_oauth_provider); E.ai_oauth_provider = strdup("github-copilot");
	hkApplyProviderAlias("github-copilot");
	if (clOAuthCopilotExchange(data) != 0) {
		aiAddHistory(data, "OAuth: GH token saved but Copilot exchange failed. Possibly no Copilot Pro on this GH account.");
		return -1;
	}
	aiAddHistory(data, "GitHub Copilot OAuth: signed in. Calls bill against your Copilot Pro subscription.");
	return 0;
}

/* GitHub Models — same device-flow harness as Copilot, different scope + endpoint.
   No second exchange step; GH access token is used directly as Bearer. */
static int clOAuthGithubModels(aiData *data) {
	const char *client_id = HAKO_GHMODELS_CLIENT_ID;
	const char *k = getenv("HAKO_GHMODELS_CLIENT_ID");
	if (k && *k) client_id = k;

	char body[256];
	snprintf(body, sizeof(body), "{\"client_id\":\"%s\",\"scope\":\"read:user\"}", client_id);
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
	aiAddHistory(data, "Polling... (Ctrl+C to abort)");
	clOpenUrl(verify_uri);

	long start = (long)time(NULL);
	char *gh_token = NULL;
	const char *grant = "urn:ietf:params:oauth:grant-type:device_code";
	while (!E.interrupt && (long)time(NULL) - start < expires_in) {
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
			snprintf(line, sizeof(line), "OAuth: %s%s%s", err, desc ? ": " : "", desc ? desc : "");
			aiAddHistory(data, line);
			free(err); free(desc); free(tr);
			free(device_code); free(user_code); free(verify_uri);
			return -1;
		}
		if (err && !strcmp(err, "slow_down")) interval += 5;
		free(err); free(tr);
	}
	free(device_code); free(user_code); free(verify_uri);
	if (!gh_token) { aiAddHistory(data, "OAuth: timed out / aborted"); return -1; }

	/* GH token IS the API key for GH Models. No second exchange. */
	hkApplyProviderAlias("github-models");
	free(E.ai_endpoint); E.ai_endpoint = strdup(HAKO_GHMODELS_ENDPOINT);
	free(E.ai_api_key); E.ai_api_key = gh_token;
	free(E.ai_oauth_provider); E.ai_oauth_provider = strdup("github-models");
	free(E.ai_oauth_refresh); E.ai_oauth_refresh = NULL;
	E.ai_oauth_expires_at = 0;  /* GH tokens are long-lived */
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	aiAddHistory(data, "GitHub Models OAuth: signed in. Free tier, rate-limited by GitHub.");
	aiAddHistory(data, "Try /model gpt-4o or /model meta-llama-3-70b-instruct");
	return 0;
}

/* OpenRouter PKCE — loopback callback, plain code_challenge. Exchange returns a
   user-scoped API key (not an OAuth token; it's a real OR API key). */
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
	char line[256];
	snprintf(line, sizeof(line), "Opening browser. Waiting on %s ...", callback);
	aiAddHistory(data, line);
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
	free(E.ai_oauth_provider); E.ai_oauth_provider = NULL;  /* OR key behaves like a paste key */
	free(E.ai_oauth_refresh); E.ai_oauth_refresh = NULL;
	E.ai_oauth_expires_at = 0;
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	aiAddHistory(data, "OpenRouter OAuth: key issued + saved.");
	return 0;
#endif
}

/* Exchange the stored GH access token for a fresh Copilot session token. */
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
	E.ai_oauth_expires_at = expires_at > 0 ? (long)expires_at - 30 : (long)time(NULL) + 1500;
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

/* SHA-256 + base64url via openssl shell-out. Returns malloc'd string or NULL.
   openssl ships on macOS/Linux/iSh by default; mingw includes it. Avoids
   inlining ~120 LOC of SHA-256 + base64url for one call site. */
static char *clSha256Base64Url(const char *in) {
	char tmpl[] = "/tmp/hako-pkce-XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) return NULL;
	if (write(fd, in, strlen(in)) < 0) { close(fd); unlink(tmpl); return NULL; }
	close(fd);
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
		"openssl dgst -sha256 -binary < %s 2>/dev/null | openssl base64 2>/dev/null | tr '+/' '-_' | tr -d '=\\n'",
		tmpl);
	FILE *fp = popen(cmd, "r");
	if (!fp) { unlink(tmpl); return NULL; }
	char buf[128]; size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
	pclose(fp); unlink(tmpl);
	if (got == 0) return NULL;
	buf[got] = '\0';
	/* Strip trailing whitespace just in case. */
	while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r' || buf[got - 1] == ' ')) buf[--got] = '\0';
	return strdup(buf);
}

/* Anthropic OAuth — PKCE auth-code with manual code paste.
   Flow: build authorize URL with S256 challenge, open browser, user signs in,
   Anthropic console shows `<code>#<state>` string, user pastes back, hako
   exchanges code+verifier for access+refresh tokens. */
static int clOAuthAnthropic(aiData *data) {
	const char *client_id = clOAuthClientId("anthropic");
	if (!client_id) { aiAddHistory(data, "OAuth: no anthropic client_id"); return -1; }

	char *verifier = clOAuthRandomVerifier();
	if (!verifier) { aiAddHistory(data, "OAuth: verifier gen failed"); return -1; }
	char *challenge = clSha256Base64Url(verifier);
	if (!challenge) {
		aiAddHistory(data, "OAuth: openssl missing — needed for PKCE S256. Install openssl or use API-key paste.");
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
	clOpenUrl(url);  /* best-effort browser-open; iSh / headless skip silently */

	printf("  paste code (input hidden): ");
	fflush(stdout);
	char paste[512];
	clReadHidden(paste, sizeof(paste));
	if (!paste[0]) { aiAddHistory(data, "(empty, not saved)"); free(verifier); return -1; }

	/* Anthropic returns `<code>#<state>` — split on '#'. State is verifier. */
	char *hash = strchr(paste, '#');
	char *code, *state;
	if (hash) { *hash = '\0'; code = paste; state = hash + 1; }
	else      { code = paste; state = verifier; }

	/* Exchange code → tokens (JSON POST). */
	char body[3072];
	snprintf(body, sizeof(body),
		"{\"code\":\"%s\",\"state\":\"%s\",\"grant_type\":\"authorization_code\","
		"\"client_id\":\"%s\",\"redirect_uri\":\"%s\",\"code_verifier\":\"%s\"}",
		code, state, client_id, HAKO_ANTHROPIC_REDIRECT, verifier);

	char tmpl[] = "/tmp/hako-anth-XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) { aiAddHistory(data, "OAuth: tmp file failed"); free(verifier); return -1; }
	if (write(fd, body, strlen(body)) < 0) { close(fd); unlink(tmpl); free(verifier); return -1; }
	close(fd);
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
		"curl -s -X POST %s -A 'hako-code/%s (Claude OAuth client)' -H 'Content-Type: application/json' --data @%s 2>/dev/null",
		HAKO_ANTHROPIC_TOKEN_URL, HAKO_VERSION, tmpl);
	FILE *fp = popen(cmd, "r");
	if (!fp) { unlink(tmpl); free(verifier); aiAddHistory(data, "OAuth: exchange popen failed"); return -1; }
	size_t cap = 4096, len = 0;
	char *resp = malloc(cap);
	size_t n;
	while ((n = fread(resp + len, 1, cap - len - 1, fp)) > 0) {
		len += n;
		if (len + 1 >= cap) { cap *= 2; resp = realloc(resp, cap); }
	}
	resp[len] = '\0';
	pclose(fp);
	unlink(tmpl);
	free(verifier);

	char *access = hkExtractJsonString(resp, "access_token");
	char *refresh = hkExtractJsonString(resp, "refresh_token");
	int expires = hkExtractJsonInt(resp, "expires_in");
	if (!access) {
		char *err = aiExtractApiError(resp);
		aiAddHistory(data, err ? err : "OAuth: exchange failed (no access_token)");
		/* Dump raw response for debugging — trim to first 400 chars to keep history clean. */
		char dbg[480];
		size_t rl = strlen(resp);
		snprintf(dbg, sizeof(dbg), "raw: %.400s%s", resp, rl > 400 ? "..." : "");
		aiAddHistory(data, dbg);
		free(err); free(refresh); free(resp);
		return -1;
	}
	free(resp);

	free(E.ai_api_key); E.ai_api_key = access;
	free(E.ai_oauth_refresh); E.ai_oauth_refresh = refresh;
	free(E.ai_oauth_provider); E.ai_oauth_provider = strdup("anthropic");
	E.ai_oauth_expires_at = expires > 0 ? (long)time(NULL) + expires - 30 : 0;
	/* Auto-pick a sensible default model so the user doesn't land on a stale ID
	   inherited from before login. Haiku 4.5 is the fast/cheap default; user
	   can :model switch any time. */
	if (!E.ai_model || !*E.ai_model) {
		free(E.ai_model); E.ai_model = strdup("claude-haiku-4-5-20251001");
	}
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	aiAddHistory(data, "Anthropic OAuth: signed in. Calls now bill against your Claude subscription.");
	return 0;
}

/* Random URL-safe verifier from /dev/urandom (or rand fallback). 43 chars (≥256 bits). */
static char *clOAuthRandomVerifier(void) {
	static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	char *out = malloc(48);
	if (!out) return NULL;
	unsigned char buf[43];
	int got = 0;
#ifndef _WIN32
	FILE *fp = fopen("/dev/urandom", "rb");
	if (fp) { got = (int)fread(buf, 1, sizeof(buf), fp); fclose(fp); }
#endif
	if (got != (int)sizeof(buf)) {
		srand((unsigned)(time(NULL) ^ getpid()));
		for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (unsigned char)(rand() & 0xff);
	}
	for (int i = 0; i < 43; i++) out[i] = alpha[buf[i] & 63];
	out[43] = '\0';
	return out;
}

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#endif

static int clOAuthLoopbackListen(int *out_port) {
#ifdef _WIN32
	(void)out_port; return -1;
#else
	int fd = socket(AF_INET, SOCK_STREAM, 0);
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
#ifdef _WIN32
	(void)srv_fd; (void)timeout_sec; return NULL;
#else
	struct timeval tv; tv.tv_sec = timeout_sec; tv.tv_usec = 0;
	fd_set r; FD_ZERO(&r); FD_SET(srv_fd, &r);
	int n = select(srv_fd + 1, &r, NULL, NULL, &tv);
	if (n <= 0) return NULL;
	int c = accept(srv_fd, NULL, NULL);
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
	write(c, resp, strlen(resp));
	close(c);
	return code;
#endif
}

/* Refresh access token. Dispatches by provider:
   - anthropic: refresh_token grant against console.anthropic.com
   - github-copilot: re-exchange GH access token for fresh Copilot session token
*/
static int clOAuthRefresh(aiData *data) {
	if (!E.ai_oauth_provider) return -1;
	if (!strcmp(E.ai_oauth_provider, "github-copilot")) return clOAuthCopilotExchange(data);
	if (strcmp(E.ai_oauth_provider, "anthropic") != 0) return -1;
	if (!E.ai_oauth_refresh) return -1;
	const char *client_id = clOAuthClientId("anthropic");
	if (!client_id) return -1;
	char body[3072];
	snprintf(body, sizeof(body),
		"{\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\",\"client_id\":\"%s\"}",
		E.ai_oauth_refresh, client_id);
	char tmpl[] = "/tmp/hako-rfsh-XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) return -1;
	if (write(fd, body, strlen(body)) < 0) { close(fd); unlink(tmpl); return -1; }
	close(fd);
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
		"curl -s -X POST %s -A 'hako-code/%s (Claude OAuth client)' -H 'Content-Type: application/json' --data @%s 2>/dev/null",
		HAKO_ANTHROPIC_TOKEN_URL, HAKO_VERSION, tmpl);
	FILE *fp = popen(cmd, "r");
	if (!fp) { unlink(tmpl); return -1; }
	size_t cap = 4096, len = 0;
	char *tr = malloc(cap);
	size_t n;
	while ((n = fread(tr + len, 1, cap - len - 1, fp)) > 0) {
		len += n;
		if (len + 1 >= cap) { cap *= 2; tr = realloc(tr, cap); }
	}
	tr[len] = '\0';
	pclose(fp);
	unlink(tmpl);

	char *access = hkExtractJsonString(tr, "access_token");
	if (!access) {
		if (data) aiAddHistory(data, "OAuth: refresh failed; run :login anthropic again");
		free(tr); return -1;
	}
	int expires = hkExtractJsonInt(tr, "expires_in");
	char *new_refresh = hkExtractJsonString(tr, "refresh_token");
	free(E.ai_api_key); E.ai_api_key = access;
	if (new_refresh) { free(E.ai_oauth_refresh); E.ai_oauth_refresh = new_refresh; }
	E.ai_oauth_expires_at = expires > 0 ? (long)time(NULL) + expires - 30 : 0;
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	free(tr);
	return 0;
}

/* Called before each API turn. Refreshes access token if expired. */
static void clOAuthEnsureFresh(aiData *data) {
	if (!E.ai_oauth_provider || !E.ai_oauth_refresh) return;
	if (E.ai_oauth_expires_at == 0) return;
	if ((long)time(NULL) < E.ai_oauth_expires_at) return;
	clOAuthRefresh(data);
}

/*** self-update (Phase 3) ***/
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
	const char *asset = clPlatformAsset();
	const char *pdir  = clPlatformDir();
	if (!asset || !pdir) {
		fprintf(stderr, "update: unsupported platform\n");
		return 1;
	}

	/* fetch latest tag */
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

	/* tmp dir */
	char tmp_dir[256];
	snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/hako-update-%ld", (long)time(NULL));
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

	/* find new binary */
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
	(void)system(rmcmd);
	return 0;
}

/*** REPL ***/

/* Visible cell width — strips ANSI escapes, counts UTF-8 continuation bytes
   as zero. Box-drawing + block glyphs = 1 cell each. */
static int clCellWidth(const char *s) {
	int n = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == 0x1b && p[1] == '[') {
			while (*p && *p != 'm') p++;
			if (!*p) return n;
		} else if ((*p & 0xc0) != 0x80) n++;
	}
	return n;
}

/* Themed box helpers — accent border, theme color content. */
static void clBoxTop(int inner) {
	if (E.color_enabled) fputs(TH_ACCENT, stdout);
	fputs("╭", stdout);
	for (int i = 0; i < inner; i++) fputs("─", stdout);
	fputs("╮", stdout);
	if (E.color_enabled) fputs(ANSI_RESET, stdout);
	fputc('\n', stdout);
}
static void clBoxBot(int inner) {
	if (E.color_enabled) fputs(TH_ACCENT, stdout);
	fputs("╰", stdout);
	for (int i = 0; i < inner; i++) fputs("─", stdout);
	fputs("╯", stdout);
	if (E.color_enabled) fputs(ANSI_RESET, stdout);
	fputc('\n', stdout);
}
static void clBoxRow(int inner, const char *content, const char *content_color) {
	if (E.color_enabled) fputs(TH_ACCENT, stdout);
	fputs("│", stdout);
	if (E.color_enabled && content_color) fputs(content_color, stdout);

	/* Truncate content to inner cells if too wide — content overflowing inner
	   would push the right border to the next line and wrap. */
	int cw = clCellWidth(content);
	if (cw <= inner) {
		fputs(content, stdout);
	} else {
		/* Walk content, emit cell-by-cell until inner-1 cells, then '…'. */
		int emitted = 0;
		const unsigned char *p = (const unsigned char *)content;
		while (*p && emitted < inner - 1) {
			if (*p == 0x1b && p[1] == '[') {
				/* Pass through ANSI SGR untouched. */
				fputc(*p++, stdout);
				while (*p && *p != 'm') fputc(*p++, stdout);
				if (*p) fputc(*p++, stdout);
				continue;
			}
			/* Emit one cell — leading byte + any UTF-8 continuation bytes. */
			fputc(*p++, stdout);
			while ((*p & 0xc0) == 0x80) fputc(*p++, stdout);
			emitted++;
		}
		fputs("…", stdout);  /* 1 cell */
		cw = inner;  /* fully consumed */
	}

	if (E.color_enabled) fputs(ANSI_RESET, stdout);
	int pad = inner - (cw > inner ? inner : cw);
	for (int i = 0; i < pad; i++) putchar(' ');
	if (E.color_enabled) fputs(TH_ACCENT, stdout);
	fputs("│", stdout);
	if (E.color_enabled) fputs(ANSI_RESET, stdout);
	fputc('\n', stdout);
}

static void clBanner(aiData *data) {
	int cols = clTermCols();
	int sk = hkLoadSkills(data);
	const char *prov = hkProviderName(E.ai_provider_type);
	const char *model = E.ai_model ? E.ai_model : "—";
	const char *trust_str = hkProjectTrusted() ? "on" : "off";
	const char *sess_state = E.session_resumed ? "resumed" : "new";
	const char *sess_id = E.session_id ? E.session_id : "?";

	/* Pick logo by width. Box inner sizes to max of logo + each status row,
	   plus 4 col padding. Build rows first then measure with clCellWidth so
	   multibyte chars (·) and any future ANSI are counted correctly — strlen
	   would undercount the dot-separators and bust the right border. */
	const char **logo = (cols >= 42) ? CL_LOGO_MEDIUM : CL_LOGO_TINY;
	int logo_w = 0;
	for (int i = 0; logo[i]; i++) { int w = clCellWidth(logo[i]); if (w > logo_w) logo_w = w; }

	(void)sess_id;  /* shown via :sessions, not banner */
	char row_a_buf[256], row_b_buf[256], row_c_buf[256];
	snprintf(row_a_buf, sizeof(row_a_buf), " hako %s · %s · %s", HAKO_VERSION, prov, model);
	if (sk > 0)
		snprintf(row_b_buf, sizeof(row_b_buf), " trust %s · skills %d · session %s",
			trust_str, sk, sess_state);
	else
		snprintf(row_b_buf, sizeof(row_b_buf), " trust %s · session %s",
			trust_str, sess_state);
	snprintf(row_c_buf, sizeof(row_c_buf), " :help :providers :models :login :theme");

	int row_a = clCellWidth(row_a_buf);
	int row_b = clCellWidth(row_b_buf);
	int row_c = clCellWidth(row_c_buf);
	int max_row = logo_w;
	if (row_a > max_row) max_row = row_a;
	if (row_b > max_row) max_row = row_b;
	if (row_c > max_row) max_row = row_c;
	int inner = max_row + 4;
	if (inner > cols - 2) inner = cols - 2;
	if (inner < 24) {
		/* Terminal way too narrow — fall back to single line. */
		printf("\n  %s◆ hako %s%s  %s· %s · %s · trust %s%s\n",
			E.color_enabled ? TH_ACCENT : "", HAKO_VERSION, E.color_enabled ? ANSI_RESET : "",
			E.color_enabled ? TH_META : "", prov, model, trust_str, E.color_enabled ? ANSI_RESET : "");
		fflush(stdout);
		return;
	}

	putchar('\n');
	clBoxTop(inner);
	/* Logo rows — centered, AI color. */
	char rowbuf[256];
	for (int i = 0; logo[i]; i++) {
		int w = clCellWidth(logo[i]);
		int lpad = (inner - w) / 2; if (lpad < 0) lpad = 0;
		snprintf(rowbuf, sizeof(rowbuf), "%*s%s", lpad, "", logo[i]);
		clBoxRow(inner, rowbuf, TH_AI);
	}
	/* Spacer + status rows. */
	clBoxRow(inner, "", NULL);
	clBoxRow(inner, row_a_buf, TH_ACCENT);
	clBoxRow(inner, row_b_buf, TH_META);
	clBoxRow(inner, row_c_buf, TH_META);
	clBoxBot(inner);
	fflush(stdout);
}

/*** startup menu ***/
typedef struct {
	char id[32];
	long last;
	int count;
	char first[80];
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
		snprintf(path, sizeof(path), "%s/%s", sdir, e->d_name);
		struct stat st;
		if (stat(path, &st) != 0) continue;
		int idx = n++;
		memcpy(out[idx].id, e->d_name, idlen); out[idx].id[idlen] = '\0';
		out[idx].last = (long)st.st_mtime;
		out[idx].count = 0;
		out[idx].first[0] = '\0';
		FILE *fp = fopen(path, "r");
		if (!fp) continue;
		char *line = NULL; size_t cap = 0;
		while (getline(&line, &cap, fp) != -1) {
			out[idx].count++;
			if (!out[idx].first[0]) {
				char *role = strstr(line, "\"role\":\"user\"");
				if (role) {
					char *cp = strstr(line, "\"content\":\"");
					if (cp) {
						cp += 11;
						int j = 0;
						while (*cp && *cp != '"' && j < 60) out[idx].first[j++] = *cp++;
						out[idx].first[j] = '\0';
					}
				}
			}
		}
		free(line);
		fclose(fp);
	}
	closedir(d);
	return n;
}

static int clPromptYN(const char *q, int default_yes) {
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
			printf("\n  %sTrust this directory for tool access?%s\n",
				E.color_enabled ? ANSI_BOLD : "", E.color_enabled ? ANSI_RESET : "");
			{
				/* Wrap cwd line at terminal width so long iCloud paths don't trail off. */
				int cols = clTermCols();
				int avail = cols - 8; /* "  cwd: " indent + safety */
				if (avail < 24) avail = 24;
				if (E.color_enabled) printf("  cwd: %s", ANSI_DIM);
				else printf("  cwd: ");
				const char *p = cwd;
				int rem = (int)strlen(cwd);
				int first = 1;
				while (rem > 0) {
					int chunk = rem > avail ? avail : rem;
					if (!first) printf("       "); /* align under "cwd: " */
					fwrite(p, 1, chunk, stdout);
					putchar('\n');
					p += chunk;
					rem -= chunk;
					first = 0;
				}
				if (E.color_enabled) printf("%s", ANSI_RESET);
			}
			printf("  (untrusted = no read_file, list_dir, write_file, run_shell)\n");
			if (clPromptYN("  grant trust?", 0)) {
				if (hkGrantProjectTrust()) printf("  %strusted.%s\n",
					E.color_enabled ? ANSI_AI : "", E.color_enabled ? ANSI_RESET : "");
				else printf("  %scould not grant.%s\n",
					E.color_enabled ? ANSI_ERR : "", E.color_enabled ? ANSI_RESET : "");
			} else {
				printf("  %sread-only mode. /trust to grant later.%s\n",
					E.color_enabled ? ANSI_DIM : "", E.color_enabled ? ANSI_RESET : "");
			}
		}
	}

	clSessionInfo sess[16];
	int nsess = clEnumerateSessions(sess, 16);

	if (nsess == 0) return;

	printf("\n  %ssession:%s\n", E.color_enabled ? ANSI_BOLD : "", E.color_enabled ? ANSI_RESET : "");
	printf("    1) new\n");
	printf("    2) resume one of %d\n", nsess);
	printf("    3) continue current%s\n", E.session_resumed ? " (resumed)" : "");
	if (E.color_enabled) printf("  %spick [3]:%s ", ANSI_BOLD, ANSI_RESET);
	else printf("  pick [3]: ");
	fflush(stdout);
	char buf[32];
	if (!fgets(buf, sizeof(buf), stdin)) return;
	int pick = atoi(buf);
	if (pick == 1) {
		for (int i = 0; i < data->history_count; i++) free(data->history[i]);
		memset(data->history_role, 0, AI_HISTORY_MAX);
		data->history_count = 0;
		aiFreeMessages(data);
		E.session_started = (long)time(NULL);
		E.session_turn_count = 0;
		E.session_resumed = 0;
		hkGenSessionId();
		hkSaveSession();
		printf("  new session: %s\n", E.session_id);
	} else if (pick == 2) {
		printf("\n");
		long now = (long)time(NULL);
		for (int i = 0; i < nsess; i++) {
			long age = now - sess[i].last;
			char unit; long val;
			if (age < 3600) { val = age / 60; unit = 'm'; }
			else if (age < 86400) { val = age / 3600; unit = 'h'; }
			else { val = age / 86400; unit = 'd'; }
			const char *cur = (E.session_id && strcmp(E.session_id, sess[i].id) == 0) ? "* " : "  ";
			printf("    %d)%s%s %ld%c %dt %.40s\n",
				i + 1, cur, sess[i].id, val, unit, sess[i].count, sess[i].first);
		}
		if (E.color_enabled) printf("  %spick [1]:%s ", ANSI_BOLD, ANSI_RESET);
		else printf("  pick [1]: ");
		fflush(stdout);
		if (!fgets(buf, sizeof(buf), stdin)) return;
		int s = atoi(buf);
		if (s < 1 || s > nsess) s = 1;
		free(E.session_id);
		E.session_id = strdup(sess[s - 1].id);
		E.session_resumed = 1;
		E.session_started = (long)time(NULL);
		hkSaveSession();
		for (int i = 0; i < data->history_count; i++) free(data->history[i]);
		memset(data->history_role, 0, AI_HISTORY_MAX);
		data->history_count = 0;
		aiFreeMessages(data);
		hkLoadHistoryTail(data, 200);
		printf("  resumed: %s (%d msgs loaded)\n", E.session_id, data->history_count);
	}
	/* pick 3 / anything else: keep current */
}

static void clWaitWorker(aiData *data) {
	if (!data->streaming) return;
	pthread_join(data->worker_thread, NULL);
}

static int clOneShot(aiData *data, const char *prompt) {
	aiAddHistoryRole(data, prompt, HK_ROLE_USER);
	hkLogMessage("user", prompt);
	E.session_turn_count++;
	hkSaveSession();
	free(data->current_prompt);
	data->current_prompt = strdup(prompt);
	if (E.ai_provider_type == AI_PROVIDER_NONE) {
		fprintf(stderr, "error: no provider configured (~/.hakorc or /provider)\n");
		return 1;
	}
	aiWorkerSend(data);
	clWaitWorker(data);
	return 0;
}

/* --pipe mode: hako editor talks JSONL over stdin/stdout.
   stdin  ← {"type":"prompt","text":"..."} | {"type":"slash","cmd":"..."} | {"type":"quit"}
   stdout → {"type":"init",...} | {"type":"message",...} | {"type":"tool_start/end",...} | {"type":"done",...} */
static int clPipeMode(aiData *data) {
	/* emit init so hako knows session state */
	char init_json[512];
	snprintf(init_json, sizeof(init_json),
		"{\"type\":\"init\",\"session\":\"%s\",\"resumed\":%d,\"turns\":%d,\"provider\":\"%s\",\"model\":\"%s\"}",
		E.session_id ? E.session_id : "",
		E.session_resumed ? 1 : 0,
		E.session_turn_count,
		hkProviderName(E.ai_provider_type),
		E.ai_model ? E.ai_model : "");
	clPipeEmitRaw(init_json);

	if (E.session_resumed && data->history_count == 0)
		hkLoadHistoryTail(data, 40);
	hkLoadSkills(data);

	char line[4096];
	while (fgets(line, sizeof(line), stdin)) {
		int len = (int)strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
		if (len == 0) continue;

		char *type = hkExtractJsonString(line, "type");
		if (!type) continue;

		if (strcmp(type, "quit") == 0) {
			free(type); break;
		}

		if (strcmp(type, "prompt") == 0) {
			char *text = hkExtractJsonString(line, "text");
			if (text && *text) {
				free(data->current_prompt);
				data->current_prompt = text;
				if (E.ai_provider_type == AI_PROVIDER_NONE) {
					clPipeEmitMsg("system", "No provider set. Configure ~/.hakorc (ai_provider=gemini/ollama/anthropic)");
					char done[128];
					snprintf(done, sizeof(done),
						"{\"type\":\"done\",\"turn\":%d,\"in\":0,\"out\":0,\"session\":\"%s\"}",
						E.session_turn_count, E.session_id ? E.session_id : "");
					clPipeEmitRaw(done);
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
			if (cmd) {
				int r = hkHandleSlash(data, cmd);
				free(cmd);
				/* emit done so hako knows the slash is processed */
				char done[128];
				snprintf(done, sizeof(done),
					"{\"type\":\"done\",\"turn\":%d,\"in\":0,\"out\":0,\"session\":\"%s\"}",
					E.session_turn_count, E.session_id ? E.session_id : "");
				clPipeEmitRaw(done);
				if (r == 2) { free(type); break; }
			}
		}

		free(type);
	}
	return 0;
}

/* v0.1.6: if any hako-* model is available via local ollama and no provider/key
   is configured, default to it. Preference order: koi > koi-mini > sho.
   Within each tier, prefer real fine-tunes (hako-*-v*) over stock-wraps (hako-*-stock).
   Skips the first-run wizard on systems with a hako model already pulled.
   Returns 1 if defaulted, 0 otherwise. */
static int clDetectKoiDefault(void) {
	if (E.ai_provider_type != AI_PROVIDER_NONE) return 0;
	if (E.ai_api_key || E.ai_oauth_refresh) return 0;
	FILE *fp = popen("ollama list 2>/dev/null", "r");
	if (!fp) return 0;
	char line[512];
	char koi[128] = {0}, mini[128] = {0}, sho[128] = {0};
	char koi_stock[128] = {0}, mini_stock[128] = {0}, sho_stock[128] = {0};
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
		/* Strip ":latest" or ":<tag>" suffix for matching ergonomics. Keep full tag for use. */
		int is_stock = (strstr(tag, "-stock") != NULL);
		int is_ver   = (strstr(tag, "-v") != NULL);
		if (strstr(tag, "hako-koi-mini-")) {
			if (is_ver && !mini[0])         { strncpy(mini,       tag, sizeof(mini)-1); }
			else if (is_stock && !mini_stock[0]) { strncpy(mini_stock, tag, sizeof(mini_stock)-1); }
		} else if (strstr(tag, "hako-koi-")) {
			if (is_ver && !koi[0])          { strncpy(koi,        tag, sizeof(koi)-1); }
			else if (is_stock && !koi_stock[0])  { strncpy(koi_stock,  tag, sizeof(koi_stock)-1); }
		}
		if (strstr(tag, "hako-sho-")) {
			if (is_ver && !sho[0])          { strncpy(sho,        tag, sizeof(sho)-1); }
			else if (is_stock && !sho_stock[0])  { strncpy(sho_stock,  tag, sizeof(sho_stock)-1); }
		}
	}
	pclose(fp);
	const char *pick = NULL;
	if      (koi[0])        pick = koi;
	else if (mini[0])       pick = mini;
	else if (sho[0])        pick = sho;
	else if (koi_stock[0])  pick = koi_stock;
	else if (mini_stock[0]) pick = mini_stock;
	else if (sho_stock[0])  pick = sho_stock;
	if (!pick) return 0;
	hkApplyProviderAlias("mithraeum");
	free(E.ai_endpoint); E.ai_endpoint = strdup("http://localhost:11434");
	free(E.ai_model);    E.ai_model    = strdup(pick);
	hkSaveSession();
	if (isatty(STDOUT_FILENO)) {
		const char *R = E.color_enabled ? ANSI_RESET : "";
		const char *M = E.color_enabled ? TH_META   : "";
		printf("  %s%s detected, defaulted to local hako via mithraeum provider.%s\n", M, pick, R);
	}
	return 1;
}

/* First-run wizard. Triggered when ~/.hako is absent (truly fresh) and no
   provider/key/oauth is configured. Goal: zero-to-chatting in under a minute. */
static void clFirstRunWizard(aiData *data) {
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
	if (E.ai_provider_type != AI_PROVIDER_NONE || E.ai_api_key || E.ai_oauth_refresh) return;

	const char *R = E.color_enabled ? ANSI_RESET : "";
	const char *M = E.color_enabled ? TH_META : "";
	const char *A = E.color_enabled ? TH_ACCENT : "";

	printf("\n  %sfirst run.%s %ssign in:%s [1]anthropic [2]copilot [3]gh-models [4]openrouter [5]ollama [6]skip\n",
		A, R, M, R);
	char buf[32];
	int n = clReadLineRaw("  pick [1]: ", buf, sizeof(buf));
	if (n == -2 || n == -1) { E.interrupt = 0; printf("\n"); return; }
	int pick = (n > 0) ? atoi(buf) : 1;
	if (pick <= 0 || pick > 6) pick = 1;

	/* If creds already exist in ~/.hako/credentials for this provider, just
	   restore them and skip the OAuth dance. */
	#define _HAKO_PICK_CREDED(prov, oauthfn) do { \
		hkApplyProviderAlias(prov); \
		clCred *_c = clCredsFind(prov); \
		if (_c && (_c->oauth_refresh || _c->api_key)) { \
			clCredsRestoreFor(prov); hkSaveSession(); \
			printf("  %srestored saved %s credentials.%s\n", M, prov, R); \
		} else { oauthfn(data); } \
	} while (0)
	switch (pick) {
	case 1: _HAKO_PICK_CREDED("anthropic",      clOAuthAnthropic); break;
	case 2: _HAKO_PICK_CREDED("github-copilot", clOAuthGithubCopilot); break;
	case 3: _HAKO_PICK_CREDED("github-models",  clOAuthGithubModels); break;
	case 4: _HAKO_PICK_CREDED("openrouter",     clOAuthOpenRouter); break;
	case 5:
		hkApplyProviderAlias("ollama");
		free(E.ai_endpoint); E.ai_endpoint = strdup("http://localhost:11434");
		if (!E.ai_model) E.ai_model = strdup("llama3.2");
		hkSaveSession();
		printf("  %sollama configured. start it: ollama serve%s\n", M, R);
		break;
	case 6:
	default:
		printf("  %sskipped. :providers to browse · :login <name> to authenticate.%s\n", M, R);
		break;
	}
	printf("\n");
	fflush(stdout);
}

static int clRepl(aiData *data) {
	clBanner(data);
	if (!clDetectKoiDefault()) clFirstRunWizard(data);
	clStartupMenu(data);
	if (E.session_resumed && data->history_count == 0) {
		hkLoadHistoryTail(data, 40);
	}

	/* First-launch hint: no provider set + no API key + no Ollama configured = blank slate.
	   Point user at /providers so they discover the catalog without having to know /login. */
	if (!E.pipe_mode && E.color_enabled && data->history_count == 0
		&& E.ai_provider_type == AI_PROVIDER_NONE
		&& !E.ai_api_key && !E.ai_oauth_refresh && !E.ai_endpoint) {
		printf("\n  %sno provider configured.%s\n", ANSI_TOOL, ANSI_RESET);
		printf("  %s:providers%s to browse · %s:login <name>%s to authenticate · %s:login ollama%s for local-only\n\n",
			ANSI_BOLD, ANSI_RESET, ANSI_BOLD, ANSI_RESET, ANSI_BOLD, ANSI_RESET);
		fflush(stdout);
	}

	char line[4096];
	const char *prompt_color = "\x1b[1m> \x1b[0m";
	const char *prompt_plain = "> ";

	while (1) {
		/* Sigils alone (◆ ›) signal turn boundaries — no separator rule. */
		if (E.last_role_shown == HK_ROLE_AI) E.last_role_shown = HK_ROLE_SYSTEM;
		/* Compact status footnote: model · turn · tokens · cost · ms — short. */
		if (E.color_enabled && !E.pipe_mode) {
			const char *model = E.ai_model ? E.ai_model : "—";
			/* Short model: strip "claude-" / "gpt-" / "gemini-" prefix for screen room. */
			const char *short_m = model;
			if (!strncmp(short_m, "claude-", 7)) short_m += 7;
			else if (!strncmp(short_m, "gpt-", 4)) short_m += 4;
			else if (!strncmp(short_m, "gemini-", 7)) short_m += 7;
			char stat[256];
			char extra[96]; extra[0] = '\0';
			char ms_chunk[24]; ms_chunk[0] = '\0';
			if (data->last_turn_ms > 0) {
				if (data->last_turn_ms < 1000) snprintf(ms_chunk, sizeof(ms_chunk), "·%ldms", data->last_turn_ms);
				else snprintf(ms_chunk, sizeof(ms_chunk), "·%.1fs", data->last_turn_ms / 1000.0);
			}
			if (data->total_in_tokens || data->total_out_tokens) {
				double cost = hkSessionCostUSD(data);
				const char *flat = hkFreeTierLabel();
				char cost_chunk[40]; cost_chunk[0] = '\0';
				if (flat) snprintf(cost_chunk, sizeof(cost_chunk), "·%s", flat);
				else if (cost > 0.00005) snprintf(cost_chunk, sizeof(cost_chunk), "·$%.4f", cost);
				snprintf(extra, sizeof(extra), "·↑%ld↓%ld%s%s",
					data->total_in_tokens, data->total_out_tokens, cost_chunk, ms_chunk);
			} else if (ms_chunk[0]) {
				snprintf(extra, sizeof(extra), "%s", ms_chunk);
			}
			snprintf(stat, sizeof(stat), "%s%s·t%d%s%s",
				TH_META, short_m, E.session_turn_count, extra, ANSI_RESET);
			printf("%s\n", stat);
			fflush(stdout);
		}
		const char *prompt = E.color_enabled ? prompt_color : prompt_plain;
		int n = clReadLineRaw(prompt, line, sizeof(line));
		if (n == -1) { break; }                    /* EOF */
		if (n == -2) { E.interrupt = 0; continue; }/* Ctrl-C cancel */
		if (n == 0) continue;

		if (line[0] == '/' || line[0] == ':') {
			int r = hkHandleSlash(data, line);
			if (r == 2) break;
			continue;
		}

		aiPushHistoryStore(data, line, HK_ROLE_USER);
		hkLogMessage("user", line);
		E.session_turn_count++;
		hkSaveSession();

		free(data->current_prompt);
		data->current_prompt = strdup(line);

		if (E.ai_provider_type == AI_PROVIDER_NONE) {
			aiAddHistory(data, "Set ai_provider in ~/.hakorc or /provider <name>");
			continue;
		}

		aiWorkerSend(data);
		clWaitWorker(data);

		if (E.interrupt) {
			E.interrupt = 0;
			aiAddHistory(data, "(interrupted)");
		}
	}

	return 0;
}

/*** main ***/
static void clUsage(void) {
	printf("hako-code v%s — standalone AI agent CLI\n", HAKO_VERSION);
	printf("usage: hako [options]\n");
	printf("  -p <prompt>     one-shot prompt, exit when done\n");
	printf("  --update        check GitHub for latest release, replace self if newer\n");
	printf("  --update-force  re-download latest even if same version\n");
	printf("  --anim <name>   pin animation: braille|dots|bar|pulse|bounce|ghost|arrows|blocks\n");
	printf("  --no-color      disable ANSI color/animation\n");
	printf("  --pipe          JSONL I/O mode for hako editor integration\n");
	printf("  --compact       slim banner only\n");
	printf("  --debug         dump raw API responses to stderr\n");
	printf("  -h --help       this help\n");
	printf("  -v --version    print version\n");
	printf("config: ~/.hakorc (ai_provider, ai_api_key, ai_model, theme, anim_style, ...)\n");
	printf("  state: ~/.hako/ (per-project state, credentials, session log)\n");
}

int main(int argc, char **argv) {
	clInitConfig();
	clLoadRc();
	hkLoadSession();
	clApplyEnv();

	/* Load obfuscated per-provider creds. If hkLoadSession migrated a legacy
	   plaintext api_key from state, snapshot it into the map then resave state
	   so the legacy field is dropped on next write. */
	clCredsLoad();
	if (E.ai_api_key || E.ai_oauth_refresh) {
		clCredsCaptureCurrent();
		clCredsSave();
	}
	clCredsRestoreFor(hkProviderName(E.ai_provider_type));
	hkSaveSession();  /* rewrite state with secrets stripped */

	const char *one_shot = NULL;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			clUsage(); return 0;
		}
		if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
			printf("hako-code v%s\n", HAKO_VERSION); return 0;
		}
		if (strcmp(a, "--update") == 0)       { return clCmdUpdate(0); }
		if (strcmp(a, "--update-force") == 0) { return clCmdUpdate(1); }
		if (strcmp(a, "-p") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "-p needs argument\n"); return 2; }
			one_shot = argv[++i]; continue;
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
		if (strcmp(a, "--debug") == 0) { E.debug = 1; continue; }
		if (strcmp(a, "--compact") == 0) { E.compact = 1; continue; }
		if (strcmp(a, "--pipe") == 0) { E.pipe_mode = 1; continue; }
		fprintf(stderr, "unknown arg: %s (try --help)\n", a);
		return 2;
	}

#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, clSigint);
#endif

	hkMigrateHakocToHako();
	clInitAI(&G_AI);

	/* Legacy layout warnings. v0.1.6 moved per-project state to
	   ~/.hako/projects/<enc>/ and renamed home dir ~/.hakoc/ → ~/.hako/ (auto-migrated
	   above). Old <cwd>/.hako/{trust,state,history} files are ignored silently —
	   warn once so users can clean up. */
	if (!E.pipe_mode) {
		char pdir[PATH_MAX];
		if (hkProjectDirPath(pdir, sizeof(pdir))) {
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

	int rc;
	if (E.pipe_mode) {
		rc = clPipeMode(&G_AI);
	} else if (one_shot) {
		rc = clOneShot(&G_AI, one_shot);
	} else {
		/* Terminal title — kanji-led brand mark (函 = hako, box). OSC 0 sets icon + window
		   title; unsupported terminals silently ignore. Restored on exit below. */
		printf("\x1b]0;函 hako\x07");
		fflush(stdout);
		rc = clRepl(&G_AI);
		printf("\x1b]0;\x07");
		fflush(stdout);
	}

	clCleanupAI(&G_AI);
	clCleanupConfig();
	return rc;
}
