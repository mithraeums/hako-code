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

#define HAKO_VERSION "0.2.3"

#define HAKO_COPILOT_EDITOR_VER   "vscode/1.95.0"
#define HAKO_COPILOT_PLUGIN_VER   "copilot-chat/0.22.0"
#define HAKO_REPO    "mithraeums/hako-code"

#if defined(__wasi__) || defined(__EMSCRIPTEN__)
#define HAKO_WASM 1
#endif

#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
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

#define CTRL_KEY(k) ((k) & 0x1f)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define AI_HISTORY_MAX 1000

#define ANSI_DIM     "\x1b[2m"
#define ANSI_BOLD    "\x1b[1m"
#define ANSI_RESET   "\x1b[0m"
#define ANSI_CLR_LINE "\r\x1b[K"

static const char *TH_USER   = "\x1b[38;2;184;150;86m";
static const char *TH_AI     = "\x1b[38;2;200;194;178m";
static const char *TH_SYS    = "\x1b[38;2;130;121;108m";
static const char *TH_ERR    = "\x1b[1;91m";
static const char *TH_TOOL   = "\x1b[38;2;198;145;94m";
static const char *TH_OK     = "\x1b[38;2;122;138;58m";
static const char *TH_ACCENT = "\x1b[38;2;184;150;86m";
static const char *TH_META   = "\x1b[38;2;110;104;94m";
static const char *TH_GHOST  = "\x1b[38;2;90;84;76m";

typedef struct { const char *name; const char *c[9]; } clTheme;
static const clTheme TH_PRESETS[] = {
	{"mithraeum", {
		"\x1b[38;2;184;150;86m",  "\x1b[38;2;200;194;178m", "\x1b[38;2;150;141;128m",
		"\x1b[1;91m",             "\x1b[38;2;198;145;94m",  "\x1b[38;2;122;138;58m",
		"\x1b[38;2;184;150;86m",  "\x1b[38;2;164;152;136m", "\x1b[38;2;108;100;88m"
	}},
	{"claude", {
		"\x1b[38;2;217;164;87m",  "\x1b[38;2;230;230;226m", "\x1b[38;2;128;128;128m",
		"\x1b[1;91m",             "\x1b[38;2;217;164;87m",  "\x1b[38;2;164;188;128m",
		"\x1b[38;2;232;188;106m", "\x1b[38;2;104;104;104m", "\x1b[38;2;80;80;80m"
	}},
	{"nord", {
		"\x1b[38;2;143;188;187m", "\x1b[38;2;229;233;240m", "\x1b[38;2;108;120;141m",
		"\x1b[1;91m",             "\x1b[38;2;208;135;112m", "\x1b[38;2;163;190;140m",
		"\x1b[38;2;136;192;208m", "\x1b[38;2;100;110;128m", "\x1b[38;2;76;86;106m"
	}},
	{"mono", {
		"", "", ANSI_DIM, "", "", "", "", ANSI_DIM, ANSI_DIM
	}},
};
static const int TH_PRESET_COUNT = sizeof(TH_PRESETS) / sizeof(TH_PRESETS[0]);
static char TH_ACTIVE[32] = "mithraeum";

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
	int gray = (r - g < 8 && g - r < 8 && g - b < 8 && b - g < 8);
	if (gray) {
		int v = (r + g + b) / 3;
		if (v < 8) return 16;
		if (v > 248) return 231;
		return 232 + (v - 8) * 24 / 240;
	}
	int ri = 0, gi = 0, bi = 0;
	for (int i = 0; i < 6; i++) {
		if (abs(r - q[i]) < abs(r - q[ri])) ri = i;
		if (abs(g - q[i]) < abs(g - q[gi])) gi = i;
		if (abs(b - q[i]) < abs(b - q[bi])) bi = i;
	}
	return 16 + 36 * ri + 6 * gi + bi;
}

static const char *clColorToken(const char *src) {
	if (cl_truecolor || !src) return src;
	int r, g, b;
	if (sscanf(src, "\x1b[38;2;%d;%d;%dm", &r, &g, &b) != 3) return src;
	static char buf[9][16];
	static int slot = 0;
	char *out = buf[slot]; slot = (slot + 1) % 9;
	snprintf(out, sizeof(buf[0]), "\x1b[38;5;%dm", clRgbTo256(r, g, b));
	return out;
}

static void clThemeApply(const char *name) {
	for (int i = 0; i < TH_PRESET_COUNT; i++) {
		if (!strcmp(TH_PRESETS[i].name, name)) {
			TH_USER   = clColorToken(TH_PRESETS[i].c[0]); TH_AI    = clColorToken(TH_PRESETS[i].c[1]);
			TH_SYS    = clColorToken(TH_PRESETS[i].c[2]); TH_ERR   = clColorToken(TH_PRESETS[i].c[3]);
			TH_TOOL   = clColorToken(TH_PRESETS[i].c[4]); TH_OK    = clColorToken(TH_PRESETS[i].c[5]);
			TH_ACCENT = clColorToken(TH_PRESETS[i].c[6]); TH_META  = clColorToken(TH_PRESETS[i].c[7]);
			TH_GHOST  = clColorToken(TH_PRESETS[i].c[8]);
			snprintf(TH_ACTIVE, sizeof(TH_ACTIVE), "%s", name);
			return;
		}
	}
}

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
} aiData;

struct clConfig {
	enum aiProviderType ai_provider_type;
	char *ai_api_key;
	char *ai_endpoint;
	char *ai_model;
	int ai_temperature;
	int ai_max_tokens;
	int ai_tools_enabled;
	int ai_tool_gate;
	int ai_toolmode;
	int ai_stream;
	int ai_auto_approve;

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

static char *hk_shell_capture(const char *cmd, long max_bytes, int *exit_code);



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
	"consulting the model",
	"boxing it up",
};
static const int CL_LABEL_COUNT = sizeof(CL_LABELS) / sizeof(CL_LABELS[0]);

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
static int clPopupSelect(const char *title, const char **items, int n, int cur, clPopupPreview preview);
static void clThemePreview(int idx, char *out, size_t cap);
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
static int hkHealEndpoint(void);
static void hkServePush(const char *json);
static int hkServeAnswer(char *out, size_t cap);
static int hkServeInput(const char *prompt, int hidden, char *out, size_t cap);
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
static int aiBuildRequest(aiData *data, enum aiProviderType type, hkReqParts *out);
static int clOAuthAnthropic(aiData *data);
static int clOAuthAnthropicFinish(aiData *data, const char *code);
static int clOAuthGithubCopilot(aiData *data);
static int clOAuthCopilotExchange(aiData *data);
static int clOAuthGithubModels(aiData *data);
static int clOAuthOpenRouter(aiData *data);
static int clOAuthRefresh(aiData *data);
static void clOAuthEnsureFresh(aiData *data);
static void clOAuthEnsureFresh(aiData *data);
static char *clOAuthRandomVerifier(void);
#ifndef _WIN32
static int clOAuthLoopbackListen(int *out_port);
static char *clOAuthLoopbackWait(int srv_fd, int timeout_sec);
#endif
static void clUrlEncodeInto(const char *s, char *out, size_t cap);

static char *cl_preset_input = NULL;
static char *aiExtractResponse(const char *json, enum aiProviderType type);
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

static char *hk_http_curl_cmd(const hkHttpReq *req, char *body_tmp, size_t tmp_cap) {
	body_tmp[0] = '\0';
	if (req->body) {
		snprintf(body_tmp, tmp_cap, "/tmp/hako_req_XXXXXX");
		int fd = mkstemp(body_tmp);
		if (fd < 0) return NULL;
		if (write(fd, req->body, strlen(req->body)) < 0) { close(fd); unlink(body_tmp); return NULL; }
		close(fd);
	}
	size_t cap = strlen(req->url) + (req->headers ? strlen(req->headers) : 0) + 512;
	char *cmd = malloc(cap);
	if (!cmd) { if (body_tmp[0]) unlink(body_tmp); return NULL; }
	int has_accept = req->headers && (strstr(req->headers, "accept:") || strstr(req->headers, "Accept:"));
	snprintf(cmd, cap,
		"curl -s%s -X %s %s -A 'hako-code/%s' %s%s%s%s%s 2>/dev/null",
		req->stream ? "N" : "",
		req->method ? req->method : "GET", req->url, HAKO_VERSION,
		has_accept ? "" : "-H 'Accept: application/json' ",
		req->headers ? req->headers : "",
		req->body ? " -H 'Content-Type: application/json' --data @" : "",
		req->body ? body_tmp : "",
		"");
	/* Nothing in a header may break out of its quotes. A newline in a pasted
	   credential ends the shell line mid-string and the whole request dies with
	   no output, which reads as "empty response" three layers away. */
	for (char *p = cmd; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
	/* An odd number of quotes means something was truncated or injected; running
	   it would hand the shell an unterminated string. */
	int quotes = 0;
	for (const char *p = cmd; *p; p++) if (*p == '\'') quotes++;
	if (quotes & 1) {
		fprintf(stderr, "hako: refusing a malformed request (credential too long or corrupt)\n");
		free(cmd);
		if (body_tmp[0]) unlink(body_tmp);
		return NULL;
	}
	return cmd;
}

static char *hk_http_fetch(const hkHttpReq *req) {
	if (!req || !req->url) return NULL;
	char tmp[64];
	char *cmd = hk_http_curl_cmd(req, tmp, sizeof(tmp));
	if (!cmd) return NULL;
	FILE *fp = popen(cmd, "r");
	free(cmd);
	if (!fp) { if (tmp[0]) unlink(tmp); return NULL; }
	char *out = hk_slurp(fp, 0);
	pclose(fp);
	if (tmp[0]) unlink(tmp);
	return out;
}

typedef struct { FILE *fp; char tmp[64]; } hkHttpNative;

static void *hk_http_open(const hkHttpReq *req) {
	if (!req || !req->url) return NULL;
	if (E.debug) fprintf(stderr, "[http] %s %s stream=%d body=%zu\n",
	                     req->method ? req->method : "GET", req->url, req->stream,
	                     req->body ? strlen(req->body) : (size_t)0);
	hkHttpNative *h = calloc(1, sizeof(*h));
	if (!h) return NULL;
	char *cmd = hk_http_curl_cmd(req, h->tmp, sizeof(h->tmp));
	if (!cmd) { free(h); return NULL; }
	h->fp = popen(cmd, "r");
	free(cmd);
	if (!h->fp) { if (h->tmp[0]) unlink(h->tmp); free(h); return NULL; }
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

/* Lexical only: there are no symlinks in this world, and wasi-libc's realpath
   needs an openable directory, which the seam does not provide. Without it any
   path carrying a directory component fails to resolve. */
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

static char *hkExtractJsonString(const char *src, const char *key) {
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
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = src;
	while ((p = strstr(p, pat)) != NULL) {
		const char *q = p + strlen(pat);
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		if (*q != ':') { p++; continue; }
		q++;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
		if (*q != '{') { p++; continue; }
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

static double hkSessionCostUSD(aiData *data) {
	if (!data) return 0.0;
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic")) return 0.0;
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-copilot")) return 0.0;
	if (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-models")) return 0.0;
	if (HK_IS_OLLAMA_WIRE(E.ai_provider_type)) return 0.0;
	double ip = 0, op = 0;
	if (!hkModelPriceUSDperM(E.ai_model, &ip, &op)) return -1.0;
	return ((double)data->total_in_tokens * ip + (double)data->total_out_tokens * op) / 1e6;
}

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

/* Walk the lines of a buffer read through the seam. Replaces fgets/getline on a
   FILE*, which a browser build does not have: config, credentials and session
   state all parse line-by-line and now work wherever hk_fs does. Modifies the
   buffer in place; the caller frees it. */
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

static char *hkRunShellCapture(const char *cmd, long max_bytes) {
#ifdef HAKO_WASM
	(void)cmd; (void)max_bytes;
	return strdup("error: no shell on this platform");
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
	if (system("command -v timeout >/dev/null 2>&1") == 0)
		snprintf(full, sizeof(full), "timeout 10 sh %s 2>&1", script);
	else
		snprintf(full, sizeof(full), "sh %s 2>&1", script);

	int rc = 0;
	char *out = hk_shell_capture(full, max_bytes, &rc);
	unlink(script);
	if (out && !*out) { free(out); out = NULL; }
	if (!out) {
		if (rc != 0) {
			char *e = malloc(64);
			snprintf(e, 64, "(no output, exit code %d)", rc);
			return e;
		}
		return strdup("(no output)");
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
		snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
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
	snprintf(out, n, "%s/.hako", cwd);
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
	snprintf(out, n, "%s/%s", projects, enc);
	hk_fs_mkdirp(out);
	return 1;
}

static int hkProjectTrusted(void) {
#ifdef HAKO_WASM
	/* The folder is either this origin's own storage or one the user handed
	   over through the browser's picker, and there is no shell to reach past
	   it. Trust was granted by the act of choosing it. */
	return 1;
#else
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
	if (hkProjectStateDir(dir, sizeof(dir))) {
		snprintf(out, n, "%s/history.jsonl", dir);
		return;
	}
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
	snprintf(out, n, "%s/%s.jsonl", sess_dir, E.session_id);
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
			snprintf(f, sizeof(f), "%s/%s", sdir, e->d_name);
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
		snprintf(pdir, sizeof(pdir), "%s/%s", base, e->d_name);
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
	if (filename_part && *filename_part)
		snprintf(out_full, out_cap, "%s/%s", resolved, filename_part);
	else
		snprintf(out_full, out_cap, "%s", resolved);
	return 0;
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
	/* `:login`, `:providers` and the picker all speak the "-api" forms — the
	   key-paste variant of a provider that also has an OAuth flow. `:provider`
	   rejecting them meant picking Anthropic in the browser did nothing at all. */
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
		|| strcmp(s, "cerebras") == 0
		|| strcmp(s, "github-copilot") == 0 || strcmp(s, "copilot") == 0
		|| strcmp(s, "github-models") == 0 || strcmp(s, "ghmodels") == 0
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
	free(E.ai_endpoint);
	E.ai_endpoint = ep ? strdup(ep) : NULL;
}

static const struct {
	const char *label, *model, *family;
} HK_FAMILY[] = {
	{ "anthropic",     "claude-haiku-4-5-20251001", "claude"   },
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
	{ "openrouter",    NULL,                        NULL       },
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
	if (!strcmp(name, "ollamacloud") || !strcmp(name, "ocloud") || !strcmp(name, "ollama-cloud"))
		return "https://ollama.com/settings/keys";
	return NULL;
}

static void clOpenUrl(const char *url) {
#ifdef HAKO_WASM
	(void)url;      /* the front end opens links; there is no launcher to call */
	return;
#else
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
#endif
}

static void clReadHidden(char *buf, size_t cap) {
	buf[0] = '\0';
#ifdef HAKO_WASM
	hkServeInput("paste the value", 1, buf, cap);
	return;
#else
	if (E.serve_mode) { hkServeInput("paste the code", 1, buf, cap); return; }
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
	/* An OAuth access token belongs to the provider that issued it. If the label
	   has moved on — the endpoint changed, or a switch is in progress — writing
	   it here files one provider's token as another's API key, and every later
	   request sends the wrong secret to the wrong host. */
	if (E.ai_oauth_provider && strcmp(E.ai_oauth_provider, name) != 0) return;
	clCred *c = clCredsUpsert(name);
	if (!c) return;
	free(c->api_key); c->api_key = E.ai_api_key ? strdup(E.ai_api_key) : NULL;
	free(c->oauth_refresh); c->oauth_refresh = E.ai_oauth_refresh ? strdup(E.ai_oauth_refresh) : NULL;
	c->oauth_expires_at = E.ai_oauth_expires_at;
}

/* A credential arrives from a paste, a file or a token response, and any of
   them can carry a trailing newline. It ends up inside a single-quoted curl
   header, where a newline splits the command and the shell dies on an
   unterminated quote — reported far away as "empty response". */
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

static void hkWriteSessionFile(const char *path, int include_secrets) {
	/* Built in memory, then handed to the seam in one write: a browser has no
	   FILE* to print into. */
	char buf[8192];
	int n = 0;
	#define SESS_PUT(...) do { if (n < (int)sizeof(buf)) n += snprintf(buf + n, sizeof(buf) - n, __VA_ARGS__); } while (0)
	SESS_PUT("ai_provider=%s\n", hkProviderName(E.ai_provider_type));
	if (E.ai_model) SESS_PUT("ai_model=%s\n", E.ai_model);
	if (include_secrets && E.ai_endpoint) SESS_PUT("ai_endpoint=%s\n", E.ai_endpoint);
	SESS_PUT("ai_tools_enabled=%d\n", E.ai_tools_enabled);
	SESS_PUT("ai_tool_gate=%d\n", E.ai_tool_gate);
	SESS_PUT("ai_toolmode=%d\n", E.ai_toolmode);
	SESS_PUT("theme=%s\n", TH_ACTIVE);
	SESS_PUT("ai_stream=%d\n", E.ai_stream);
	SESS_PUT("ai_auto_approve=%d\n", E.ai_auto_approve);
	if (E.session_id) SESS_PUT("session_id=%s\n", E.session_id);
	SESS_PUT("session_started=%ld\n", E.session_started);
	SESS_PUT("session_last_used=%ld\n", hk_time_unix());
	SESS_PUT("session_turn_count=%d\n", E.session_turn_count);
	hk_fs_write(path, buf, (size_t)n, 0);
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
		} else if (strcmp(key, "theme") == 0) {
			clThemeApply(val);
		} else if (strcmp(key, "ai_stream") == 0) {
			if (rc_locked && hk_rc_pin.stream) continue;
			E.ai_stream = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "ai_auto_approve") == 0) {
			if (rc_locked && hk_rc_pin.auto_approve) continue;
			E.ai_auto_approve = atoi(val) ? 1 : 0;
		} else if (strcmp(key, "ai_max_tokens") == 0) {
			if (rc_locked && hk_rc_pin.max_tokens) continue;
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
		E.session_resumed = 0;
		E.session_started = now;
		E.session_last_used = 0;
		E.session_turn_count = 0;
		hkGenSessionId();
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

static void hkLoadHistoryTail(aiData *data, int max_msgs) {
	char path[PATH_MAX];
	hkSessionLogPath(path, sizeof(path));
	if (!path[0]) return;
	char *histbuf = hk_fs_read(path, 8 * 1024 * 1024, NULL);
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
	char skills[512] = {0};
	if (dir[0]) snprintf(skills, sizeof(skills), "%s/skills", dir);
	DIR *d = skills[0] ? opendir(skills) : NULL;

	char *buf = NULL;
	size_t total = 0;
	int loaded = 0;
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", skills, e->d_name);
		struct stat st;
		if (stat(path, &st) != 0) continue;

		if (S_ISDIR(st.st_mode)) {
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
		long sz = 0;
		char *body = hk_fs_read(path, 200000, &sz);
		if (!body) continue;
		hkAppendSkillBlock(&buf, &total, e->d_name, NULL, body, sz);
		free(body);
		loaded++;
	}
	if (d) closedir(d);
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
		"{\"type\": \"function\", \"function\": {\"name\": \"run_shell\", \"description\": \"Run a non-interactive shell command in the project, 10 second timeout\", \"parameters\": {\"type\": \"object\", \"properties\": {\"cmd\": {\"type\": \"string\"}}, \"required\": [\"cmd\"]}}}\n"
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
		"RULE 1: Call a tool whenever the user wants you to read, list, write, or run something in the project — INCLUDING polite forms like \"can you create…\", \"could you read…\", \"would you make…\", \"please write…\". Those are requests to DO it, not questions. ALSO an action: any mention of a file by name, or asking why something isn't working / to debug / check / look at / find / search it — READ or LIST it first, don't ask the user about it. Asking you to work on / fix / improve / set up / style / showcase / build out \"the website / site / project / app / code / this\" is ALSO an action: your FIRST move is list_dir(\".\") to see what's there. The words \"here\", \"this folder\", \"this directory\", \"this project\", \"our website\" ALWAYS mean the current directory — call list_dir(\".\"); NEVER ask the user its name, where it is, or to list it for you. Only greetings and GENERAL concept questions with no project file involved (\"what is recursion?\", \"how does a hashmap work?\") get a plain-text reply with no tool call.\n"
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
		int off = snprintf(env_probe, sizeof(env_probe), "\n# ENVIRONMENT\n\nAvailable on this system (prefer these names exactly):\n");
		for (int i = 0; names[i] && off < (int)sizeof(env_probe) - 80; i++) {
#ifdef HAKO_WASM
			break;
#else
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
#endif
		}
		off += snprintf(env_probe + off, sizeof(env_probe) - off,
			"\nIf 'python' is absent but 'python3' is present (common on macOS), call python3 directly — do NOT try 'python' first.\n");
	}

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
					size_t scan = hako_len < 4096 ? hako_len : 4096;
					int binary = 0;
					if ((long)hako_len < hst.st_size && hst.st_size <= HAKO_CAP) binary = 1;
					for (size_t i = 0; i < scan && !binary; i++) {
						unsigned char c = (unsigned char)hako_body[i];
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

	const char *tool_prompt = NULL;
	if (E.ai_provider_type == AI_PROVIDER_MITHRAEUM)
		tool_prompt = QWEN_TOOL_PROMPT;
	else if (E.ai_toolmode == 1
	      || (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic")))
		tool_prompt = REACT_PROMPT;
	char *mcp_tools = (E.ai_provider_type == AI_PROVIDER_MITHRAEUM) ? hkMcpLocalToolsPrompt() : NULL;
	size_t mlen = mcp_tools ? strlen(mcp_tools) : 0;
	size_t blen = strlen(BASE_PROMPT);
	size_t rlen = tool_prompt ? strlen(tool_prompt) : 0;
	size_t elen = strlen(env_probe);
	size_t need = rlen + mlen + blen + elen + total + hako_hlen + hako_len + 1;
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
		combined[off] = '\0';
		free(buf); free(hako_body);
		free(data->system_prompt);
		data->system_prompt = combined;
	} else {
		free(data->system_prompt);
		data->system_prompt = buf;
	}
	free(mcp_tools);
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

static void clPipeEmitRaw(const char *json) {
	if (E.serve_mode) { hkServePush(json); return; }
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

static void clPipeEmitInit(void) {
	char cwd[PATH_MAX], esc[PATH_MAX * 2 + 8];
	if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
	clPipeEscape(cwd, esc, sizeof(esc));
	char json[PATH_MAX * 2 + 512];
	snprintf(json, sizeof(json),
		"{\"type\":\"init\",\"session\":\"%s\",\"resumed\":%d,\"turns\":%d,"
		"\"provider\":\"%s\",\"model\":\"%s\",\"dir\":\"%s\",\"version\":\"%s\"}",
		E.session_id ? E.session_id : "",
		E.session_resumed ? 1 : 0,
		E.session_turn_count,
		hkProviderName(E.ai_provider_type),
		E.ai_model ? E.ai_model : "",
		esc, HAKO_VERSION);
	clPipeEmitRaw(json);
}

static void clRenderMarkdownInline(const char *in, char *out, size_t cap) {
	size_t j = 0;
	const char *p = in;
	if (p[0] == '#' && (p[1] == ' ' || (p[1] == '#' && (p[2] == ' ' || (p[2] == '#' && p[3] == ' '))))) {
		int level = 1;
		while (*p == '#') { level++; p++; }
		if (*p == ' ') p++;
		j += snprintf(out + j, cap - j, "\x1b[1m");
		(void)level;
	}
	else if (p[0] == '>' && p[1] == ' ') {
		j += snprintf(out + j, cap - j, "%s│ ", ANSI_DIM);
		p += 2;
	}
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
			char prev = (p > in) ? p[-1] : '\0';
			int open_ok = !(prev && (isalnum((unsigned char)prev) || prev == delim));
			const char *end = open_ok ? strchr(p + 1, delim) : NULL;
			if (end && end > p + 1 && end[-1] != ' ') {
				char after = end[1];
				int close_ok = !(after && (isalnum((unsigned char)after) || after == delim));
				if (close_ok) {
					j += snprintf(out + j, cap - j, "\x1b[3m%.*s\x1b[23m", (int)(end - p - 1), p + 1);
					p = end + 1; continue;
				}
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
	if (E.pipe_mode) {
		const char *rname = (role == HK_ROLE_USER) ? "user" :
		                    (role == HK_ROLE_AI)   ? "ai"   : "system";
		clPipeEmitMsg(rname, text);
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

static void aiSayf(aiData *data, const char *fmt, ...) {
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	aiAddHistory(data, msg);
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

static mcpServer *g_mcp_servers = NULL; static int g_mcp_server_count = 0;
static mcpTool   *g_mcp_tools   = NULL; static int g_mcp_tool_count   = 0;

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

#if !defined(_WIN32) && !defined(HAKO_WASM)
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
	 "Run a non-interactive shell command. 10s timeout. Requires project trust.",
	 "\"cmd\":{\"type\":\"string\"}",
	 "\"cmd\""},
#endif
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

typedef enum { ALLOW_READ, ALLOW_WRITE, ALLOW_SHELL } hkAllowKind;
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

static int hkToolApproval(const char *name, const char *input_json) {
	if (E.ai_auto_approve) return 1;
	if (strcmp(name, "read_skill") == 0) return 1;
#ifdef HAKO_WASM
	/* Nothing here can wait for an answer: the turn runs to completion with no
	   way to yield, so an asked question is a denied one. The browser is
	   already the boundary — no shell exists, and the only reachable files are
	   the ones the user handed over — so the gate has nothing left to protect.
	   Approving in the front end and denying in the core is the worst of both. */
	(void)input_json;
	return 1;
#else

	hkAllowKind kind;
	if      (strcmp(name, "write_file") == 0) kind = ALLOW_WRITE;
	else if (strcmp(name, "edit_file")  == 0) kind = ALLOW_WRITE;
	else if (strcmp(name, "edit_lines") == 0) kind = ALLOW_WRITE;
	else if (strcmp(name, "run_shell")  == 0) kind = ALLOW_SHELL;
	else                                      kind = ALLOW_READ;

	char *subject = NULL;
	if (kind == ALLOW_SHELL) {
		subject = hkExtractJsonString(input_json, "cmd");
		if (!subject) subject = hkExtractJsonString(input_json, "command");
		if (!subject) subject = hkExtractJsonString(input_json, "shell");
		if (!subject) subject = hkExtractJsonString(input_json, "script");
	} else {
		subject = hkExtractJsonString(input_json, "path");
		if (!subject) subject = hkExtractJsonString(input_json, "file_path");
		if (!subject) subject = hkExtractJsonString(input_json, "filename");
		if (!subject) subject = hkExtractJsonString(input_json, "directory");
		if (!subject) subject = hkExtractJsonString(input_json, "dir");
	}

	if (hkAllowHas(kind, kind == ALLOW_SHELL ? subject : NULL)) { free(subject); return 1; }

	if (E.pipe_mode) {
		static long ask_seq = 0;
		char en[128], es[1024], req[1400];
		clPipeEscape(name ? name : "", en, sizeof(en));
		clPipeEscape(subject ? subject : (kind == ALLOW_SHELL ? "(command)" : "."), es, sizeof(es));
		snprintf(req, sizeof(req),
			"{\"type\":\"tool_request\",\"id\":%ld,\"name\":\"%.127s\",\"subject\":\"%.1023s\",\"kind\":\"%s\"}",
			++ask_seq, en, es,
			kind == ALLOW_WRITE ? "write" : kind == ALLOW_SHELL ? "shell" : "read");
		clPipeEmitRaw(req);

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
		if (decision == 2) { hkAllowAdd(kind, kind == ALLOW_SHELL ? subject : NULL); decision = 1; }
		free(subject);
		return decision;
	}

	if (!isatty(STDIN_FILENO)) { free(subject); return 0; }

	clStopAnim(&G_AI);

	const char *always_label =
		(kind == ALLOW_WRITE) ? "always write to this project this session" :
		(kind == ALLOW_SHELL) ? "always run exactly this command this session" :
		                        "always read from this project this session";

	const char *disp = subject ? subject : (kind == ALLOW_SHELL ? "(command)" : ".");
	if (E.color_enabled)
		printf("\n  %s\xE2\x97\x8F %s%s  %s%s%s\n", TH_TOOL, name, ANSI_RESET,
		       TH_META, disp, ANSI_RESET);
	else
		printf("\n  * %s  %s\n", name, disp);
	printf("  Allow?  [y] once   [n] no   [a] %s\n  > ", always_label);
	fflush(stdout);

	int decision = 0;
	char buf[64];
	if (fgets(buf, sizeof(buf), stdin)) {
		if      (buf[0] == 'y' || buf[0] == 'Y') decision = 1;
		else if (buf[0] == 'a' || buf[0] == 'A') decision = 2;
	}
	if (decision == 2) { hkAllowAdd(kind, kind == ALLOW_SHELL ? subject : NULL); decision = 1; }
	free(subject);
	return decision;
#endif
}

static char hk_last_write_path[1024] = "";

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
	if (out) snprintf(out, 256, "wrote %zu bytes to %s (%d lines)", wrote, full, new_lines);
	return out ? out : strdup("wrote file");
}

static char *hkExecTool(const char *name, const char *input_json) {
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
		{NULL, NULL}
	};
	for (int i = 0; aliases[i].from; i++) {
		if (strcasecmp(name, aliases[i].from) == 0) { name = aliases[i].to; break; }
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
		if (!path) path = strdup(".");
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
		if (strchr(skill, '/') || strstr(skill, "..")) {
			free(skill); free(path);
			return strdup("error: invalid skill name");
		}
		if (path[0] == '/' || strstr(path, "..")) {
			free(skill); free(path);
			return strdup("error: invalid path (must be relative, no ..)");
		}
		char dir[512]; hkClawDirPath(dir, sizeof(dir));
		char skill_root[1024];
		snprintf(skill_root, sizeof(skill_root), "%s/skills/%s", dir, skill);
		char full[PATH_MAX];
		snprintf(full, sizeof(full), "%s/%s", skill_root, path);
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
		if (!path) path = hkExtractJsonString(input_json, "filepath");
		if (!path) path = hkExtractJsonString(input_json, "file");
		char *content_raw = hkExtractJsonString(input_json, "content");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "contents");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "file_text");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "new_str");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "text");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "body");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "code");
		if (!content_raw) content_raw = hkExtractJsonString(input_json, "data");
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
			snprintf(out, 256, "wrote %zu bytes to %s (new file, %d lines)", wrote, full, new_lines);
		else
			snprintf(out, 256, "wrote %zu bytes to %s (%d lines; replaced %ld bytes / %d lines)",
				wrote, full, new_lines, old_size, old_lines);
		free(path); free(content);
		return out;
	}
	if (strcmp(name, "edit_file") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted");
		char *path = hkExtractJsonString(input_json, "path");
		if (!path) path = hkExtractJsonString(input_json, "file_path");
		if (!path) path = hkExtractJsonString(input_json, "filename");
		char *old_raw = hkExtractJsonString(input_json, "old");
		if (!old_raw) old_raw = hkExtractJsonString(input_json, "old_str");
		if (!old_raw) old_raw = hkExtractJsonString(input_json, "old_string");
		if (!old_raw) old_raw = hkExtractJsonString(input_json, "search");
		char *new_raw = hkExtractJsonString(input_json, "new");
		if (!new_raw) new_raw = hkExtractJsonString(input_json, "new_str");
		if (!new_raw) new_raw = hkExtractJsonString(input_json, "new_string");
		if (!new_raw) new_raw = hkExtractJsonString(input_json, "replace");
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
		char *path = hkExtractJsonString(input_json, "path");
		if (!path) path = hkExtractJsonString(input_json, "file_path");
		if (!path) path = hkExtractJsonString(input_json, "filename");
		if (!path) return strdup("error: edit_lines missing 'path'");
		int a = hkExtractJsonInt(input_json, "start");
		if (a < 0) a = hkExtractJsonInt(input_json, "start_line");
		if (a < 0) a = hkExtractJsonInt(input_json, "line");
		if (a < 0) { char *s = hkExtractJsonString(input_json, "start"); if (s) { a = atoi(s); free(s); } }
		int b = hkExtractJsonInt(input_json, "end");
		if (b < 0) b = hkExtractJsonInt(input_json, "end_line");
		if (b < 0) { char *s = hkExtractJsonString(input_json, "end"); if (s) { b = atoi(s); free(s); } }
		if (b < 0) b = a;
		char *new_raw = hkExtractJsonString(input_json, "new");
		if (!new_raw) new_raw = hkExtractJsonString(input_json, "new_str");
		if (!new_raw) new_raw = hkExtractJsonString(input_json, "content");
		if (!new_raw) new_raw = hkExtractJsonString(input_json, "text");
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

static void hkAnnounceTool(aiData *data, const char *fname, const char *args_obj) {
	(void)data;
	char arg_summary[128] = "";
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
		/* Logged too, or a reloaded transcript shows an answer with no sign of
		   the work behind it. */
		char logline[300];
		snprintf(logline, sizeof(logline), "%s(%s)", fname, arg_summary);
		hkLogMessage("tool", logline);
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
	int is_summary = result && (strncmp(result, "wrote ", 6) == 0 ||
	                            strncmp(result, "no change", 9) == 0);
	if (E.pipe_mode) {
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
		return;
	}
	if (is_err) {
		if (E.color_enabled) printf("  %s✗ %s%s\n", ANSI_ERR, result, ANSI_RESET);
		else printf("  ! %s\n", result);
	} else if (is_summary) {
		if (E.color_enabled) printf("  %s← %s%s\n", ANSI_DIM, result, ANSI_RESET);
		else printf("  ← %s\n", result);
	} else {
		if (E.color_enabled) printf("  %s← %d bytes%s\n", ANSI_DIM, len, ANSI_RESET);
		else printf("  ← %d bytes\n", len);
	}
	fflush(stdout);
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
	if (type == AI_PROVIDER_ANTHROPIC && E.ai_oauth_provider
	    && !strcmp(E.ai_oauth_provider, "anthropic")) tools_on = 0;
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
		if (!model) model = "claude-haiku-4-5-20251001";
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
		/* Gemini 2.5 thinks before it answers and charges the thinking to
		   max_tokens. Left alone it spends the whole allowance reasoning and
		   returns finish_reason "stop" with no content field at all. */
		const char *reason = "";
		if (endpoint && strstr(endpoint, "generativelanguage") && strstr(model, "2.5"))
			reason = ",\"reasoning_effort\":\"none\"";
		int tlen = tools_on ? strlen(fn_tools) + 16 : 0;
		int need = bodycap + tlen + 96 + (int)strlen(reason);
		if (need > bodycap) { body = realloc(body, need); bodycap = need; }
		if (tools_on) {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"max_tokens\":%d%s,\"messages\":%s,\"tools\":%s}",
				model, max_tokens, reason, msgs, fn_tools);
		} else {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"max_tokens\":%d%s,\"messages\":%s}",
				model, max_tokens, reason, msgs);
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

	/* OAuth access tokens run well past a kilobyte. A header that does not fit
	   used to be truncated mid-value, losing its closing quote and handing the
	   shell an unterminated string — which surfaced as "empty response". */
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
		int copilot = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-copilot");
		int ghmodels = endpoint && strstr(endpoint, "models.inference.ai.azure.com");
		/* Endpoints differ in how much of the path they already carry: gemini's
		   compat base ends in /openai, others in /v1, others in neither. Append
		   only what is missing, or the request goes to /v1beta/openai/v1/... and
		   the provider answers nothing useful. */
		size_t elen = endpoint ? strlen(endpoint) : 0;
		int has_v1 = elen >= 3 && !strcmp(endpoint + elen - 3, "/v1");
		int compat_base = elen >= 7 && !strcmp(endpoint + elen - 7, "/openai");
		snprintf(url, sizeof(url), "%s%s", endpoint,
		         (copilot || ghmodels || has_v1 || compat_base)
		             ? "/chat/completions" : "/v1/chat/completions");
		snprintf(hdr, sizeof(hdr), "-H 'Authorization: Bearer %s' %s", api_key, copilot
			? "-H 'Editor-Version: " HAKO_COPILOT_EDITOR_VER "' "
			  "-H 'Editor-Plugin-Version: " HAKO_COPILOT_PLUGIN_VER "' "
			  "-H 'Openai-Intent: conversation-panel' "
			  "-H 'Copilot-Integration-Id: vscode-chat' "
			: "");
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

static char *aiExtractResponse(const char *json, enum aiProviderType type) {
	if (!json) return NULL;

	if (type == AI_PROVIDER_ANTHROPIC) {
		char *t = aiExtractAnthropicText(json);
		if (t && *t) return t;
		free(t);
		return aiExtractApiError(json);
	}

	const char *start = strstr(json, "\"content\":\"");
	if (!start) {
		char *e = aiExtractApiError(json);
		if (e) return e;
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

static char *hkNormalizeToolDialect(const char *s) {
	if (!s) return NULL;
	int needs = strstr(s, "<tool_name=") || strstr(s, "</tool_name>")
	         || strstr(s, "<observation") || strstr(s, "<parameter name=")
	         || strstr(s, "</invoke>");
	if (!needs) return NULL;

	size_t cap = strlen(s) * 2 + 128;
	char *out = malloc(cap);
	if (!out) return NULL;
	size_t o = 0;

	for (const char *p = s; *p && o + 64 < cap; ) {
		if (!strncmp(p, "<tool_name=", 11)) { memcpy(out + o, "<tool name=", 11); o += 11; p += 11; continue; }
		if (!strncmp(p, "</tool_name>", 12)) { memcpy(out + o, "</tool>", 7); o += 7; p += 12; continue; }
		if (!strncmp(p, "</invoke>", 9)) { p += 9; continue; }
		if (!strncmp(p, "<invoke ", 8) || !strncmp(p, "<invoke>", 8)) {
			const char *gt = strchr(p, '>');
			p = gt ? gt + 1 : p + 8;
			continue;
		}
		if (!strncmp(p, "<observation", 12)) {
			const char *e = strstr(p, "</observation>");
			p = e ? e + 14 : p + strlen(p);
			continue;
		}
		if (!strncmp(p, "<parameter name=\"", 17)) {
			int first = 1;
			out[o++] = '{';
			while (!strncmp(p, "<parameter name=\"", 17)) {
				const char *k = p + 17;
				const char *ke = strchr(k, '"');
				if (!ke) break;
				const char *vs = strchr(ke, '>');
				if (!vs) break;
				vs++;
				const char *ve = strstr(vs, "</parameter>");
				if (!ve) break;
				if (!first) { out[o++] = ','; out[o++] = ' '; }
				first = 0;
				int klen = (int)(ke - k);
				o += (size_t)snprintf(out + o, cap - o, "\"%.*s\": \"", klen, k);
				char *val = malloc((size_t)(ve - vs) + 1);
				if (val) {
					memcpy(val, vs, (size_t)(ve - vs));
					val[ve - vs] = '\0';
					char *esc = malloc((size_t)(ve - vs) * 6 + 8);
					if (esc) {
						hkJsonEscapeInto(val, esc, (int)((ve - vs) * 6 + 8));
						o += (size_t)snprintf(out + o, cap - o, "%s", esc);
						free(esc);
					}
					free(val);
				}
				out[o++] = '"';
				p = ve + 12;
				while (*p == '\n' || *p == ' ' || *p == '\t' || *p == '\r') p++;
			}
			out[o++] = '}';
			continue;
		}
		out[o++] = *p++;
	}
	out[o] = '\0';
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
		for (const char *v=vs; v<ve && jl<cap-8; v++) {
			char c=*v;
			if (c=='"'||c=='\\') { json[jl++]='\\'; json[jl++]=c; }
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

		if (hkDupCall(&seen, &seen_n, &seen_cap, tname, json)) {
			hkPushDupNotice(data, tname); if (dups) (*dups)++;
			cursor = inv_close + 9; continue;
		}
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
	aiData *data = (aiData *)arg;
	const clAnim *a = &CL_ANIMS[data->anim_style % CL_ANIM_COUNT];
	const char *label = CL_LABELS[data->anim_label % CL_LABEL_COUNT];
	const char *col = (E.anim_force_style >= 0) ? *a->color : TH_ACCENT;
	int i = 0;
	while (data->animating) {
		const char *fr = a->frames[i % a->frame_count];
		if (E.color_enabled) {
			printf(ANSI_CLR_LINE "%s%s %s...%s", col, fr, label, ANSI_RESET);
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
		snprintf(w, sizeof(w), "%s/%s/%s.mlf2", dir, de->d_name, de->d_name);
		if (stat(w, &st) != 0) continue;
		free(found); found = strdup(de->d_name);
		if (strncmp(de->d_name, "hako-sho", 8) == 0) break;
	}
	closedir(d);
	return found;
}

static const struct { const char *prov, *models; } HK_MODEL_SUGG[] = {
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
			{ "x.ai",                         "xai"           },
			{ "generativelanguage",           "gemini"        },
			{ "models.inference.ai.azure.com","github-models" },
			{ "githubcopilot",                "copilot"       },
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

/* The catalog request as a curl command is fine where a shell exists. Where one
   does not, the same request has to go through the seam — otherwise every
   provider's model list comes back empty and only the curated fallback shows. */
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
			if (!strstr(endpoint, "openrouter")) return NULL;
			snprintf(url, sizeof(url), "%s/v1/models", endpoint);
			break;
		}
		size_t elen = strlen(endpoint);
		int has_v1 = elen >= 3 && !strcmp(endpoint + elen - 3, "/v1");
		int compat = elen >= 7 && !strcmp(endpoint + elen - 7, "/openai");
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

static char *hkModelListCmd(void) {
	const char *endpoint = E.ai_endpoint;
	const char *api_key = E.ai_api_key;
	if (!endpoint || !*endpoint) return NULL;
	char *cmd = malloc(2048);
	if (!cmd) return NULL;
	switch (E.ai_provider_type) {
	case AI_PROVIDER_OLLAMA:
		if (api_key && *api_key)
			snprintf(cmd, 2048, "curl -s --max-time 8 -H 'Authorization: Bearer %s' %s/api/tags 2>/dev/null",
				api_key, endpoint);
		else
			snprintf(cmd, 2048, "curl -s --max-time 8 %s/api/tags 2>/dev/null", endpoint);
		break;
	case AI_PROVIDER_ANTHROPIC: {
		if (!api_key || !*api_key) { free(cmd); return NULL; }
		int oauth = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic");
		snprintf(cmd, 2048, oauth
			? "curl -s --max-time 8 -H 'Authorization: Bearer %s' "
			  "-H 'anthropic-beta: oauth-2025-04-20' "
			  "-H 'User-Agent: claude-cli/1.0.40 (external, cli)' -H 'x-app: cli' "
			  "-H 'anthropic-version: 2023-06-01' '%s/v1/models?limit=200' 2>/dev/null"
			: "curl -s --max-time 8 -H 'x-api-key: %s' "
			  "-H 'anthropic-version: 2023-06-01' '%s/v1/models?limit=200' 2>/dev/null",
			api_key, endpoint);
		break;
	}
	case AI_PROVIDER_OPENAI: {
		if ((!api_key || !*api_key) && strstr(endpoint, "openrouter")) {
			snprintf(cmd, 2048, "curl -s --max-time 8 %s/v1/models 2>/dev/null", endpoint);
			break;
		}
		if (!api_key || !*api_key) { free(cmd); return NULL; }
		if (strstr(endpoint, "generativelanguage")) {
			snprintf(cmd, 2048,
				"curl -s --max-time 8 '%s/v1beta/models?pageSize=200&key=%s' 2>/dev/null",
				endpoint, api_key);
			break;
		}
		int copilot = E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "github-copilot");
		int ghmodels = strstr(endpoint, "models.inference.ai.azure.com") != NULL;
		const char *path = (copilot || ghmodels) ? "/models" : "/v1/models";
		const char *copilot_hdrs = copilot
			? "-H 'Editor-Version: " HAKO_COPILOT_EDITOR_VER "' "
			  "-H 'Copilot-Integration-Id: vscode-chat' "
			: "";
		snprintf(cmd, 2048,
			"curl -s --max-time 8 -H 'Authorization: Bearer %s' %s%s%s 2>/dev/null",
			api_key, copilot_hdrs, endpoint, path);
		break;
	}
	default: free(cmd); return NULL;
	}
	return cmd;
}

static int hkParseModelList(const char *json, char out[][96], int max) {
	if (!json || !*json || max <= 0) return 0;
	long long *when = calloc((size_t)max, sizeof(long long));
	if (!when) return 0;
	int n = 0;

	for (int pass = 0; pass < 2 && n == 0; pass++) {
		const char *key = pass == 0 ? "\"id\":\"" : "\"name\":\"";
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
			snprintf(out[j + 1], 96, "%s", out[j]);
			when[j + 1] = when[j];
			j--;
		}
		snprintf(out[j + 1], 96, "%s", id);
		when[j + 1] = w;
	}
	free(when);
	return n;
}

static char *hkPopenCapture(const char *cmd, size_t cap_max) {
	return hk_shell_capture(cmd, (long)cap_max, NULL);
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
	char *cmd = NULL;
#else
	char *cmd = hkModelListCmd();
	if (!cmd && E.debug)
		fprintf(stderr, "[models] no listing route for %s (no key?)\n", hkProviderLabel());
#endif
	if (cmd) {
		char *resp = hkPopenCapture(cmd, 1u << 22);
		if (E.debug) {
			fprintf(stderr, "[models] GET %s\n", strstr(cmd, "http") ? strstr(cmd, "http") : cmd);
			fprintf(stderr, "[models] %zu bytes back: %.200s\n",
				resp ? strlen(resp) : (size_t)0, resp ? resp : "(nothing)");
		}
		free(cmd);
		if (resp) {
			n = hkParseModelList(resp, out, max);
			free(resp);
		}
	}
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
				snprintf(w, sizeof(w), "%s/%s/%s.mlf2", mdir, de->d_name, de->d_name);
				if (stat(w, &ws) != 0) continue;
				snprintf(out[n], 96, "%s", de->d_name);
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

#ifndef _WIN32
#ifndef HAKO_WASM
static pid_t g_hakm_pid = -1;
static FILE *g_hakm_w = NULL;
static FILE *g_hakm_r = NULL;
static char *g_hakm_weights = NULL;

static void hkHakmKill(void) {
	if (g_hakm_w) { fclose(g_hakm_w); g_hakm_w = NULL; }
	if (g_hakm_r) { fclose(g_hakm_r); g_hakm_r = NULL; }
	if (g_hakm_pid > 0) {
		kill(g_hakm_pid, SIGTERM);
		waitpid(g_hakm_pid, NULL, 0);
		g_hakm_pid = -1;
	}
	free(g_hakm_weights); g_hakm_weights = NULL;
}

static int hkHakmEnsure(const char *hakm, const char *weights) {
	if (g_hakm_w && g_hakm_weights && strcmp(g_hakm_weights, weights) == 0) {
		if (waitpid(g_hakm_pid, NULL, WNOHANG) == 0) return 0;
		hkHakmKill();
	} else if (g_hakm_w) {
		hkHakmKill();
	}
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
	pid_t pid = fork();
	if (pid < 0) { close(sv[0]); close(sv[1]); return -1; }
	if (pid == 0) {
		dup2(sv[1], STDIN_FILENO);
		dup2(sv[1], STDOUT_FILENO);
		close(sv[0]); close(sv[1]);
		if (!E.debug) {
			int dn = open("/dev/null", O_WRONLY);
			if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
		}
		const char *ctx = getenv("HAKO_CTX");
		if (!ctx || atoi(ctx) <= 0) ctx = "8192";
		execlp(hakm, hakm, weights, "--serve", "--ctx", ctx, (char *)NULL);
		_exit(127);
	}
	close(sv[1]);
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
	if (!fgets(hdr, sizeof(hdr), g_hakm_r)) return NULL;
	char kind = hdr[0];
	long len = atol(hdr + 1);
	if ((kind != 'R' && kind != 'E') || len < 0 || len > (1l << 20)) return NULL;
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
	size_t fl = (size_t)snprintf(fb, need, "%d %d %d\n", n, ntok, mt);
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

static char *hkMithraeumChat(aiData *data, char **err) {
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

#ifndef _WIN32
	size_t fl = 0;
	char *fb = hkMithraeumFrame(data, sys, ntok, &fl);
	if (!fb) {
		free(hakm); free(path);
		if (err) *err = strdup("Error: nothing to send");
		return NULL;
	}

	char *out = NULL;
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
				if (err) *err = strdup("Error: conversation exceeds context window (could not trim further) — :session new");
				return NULL;
			}
			free(fb); free(hakm); free(path);
			if (err) *err = eng; else free(eng);
			return NULL;
		}
		if (!out) hkHakmKill();
	}
	free(fb); free(hakm); free(path);
	if (!out) {
		if (err) *err = strdup(
			"Error: hakm engine unavailable (spawn or transport failed).\n"
			"  rebuild it: cd hako && make && cp hakm ~/.hako/bin/   (or from hako-code: make hakm)");
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
	return strstr(low, "file") || strstr(low, "director") || strstr(low, "folder")
	    || strstr(low, "project") || strstr(low, "website") || strstr(low, "site")
	    || strstr(low, " code") || strstr(low, " app") || strstr(low, "readme")
	    || strstr(low, "script") || strstr(low, " here") || strstr(low, "this dir")
	    || strstr(low, "this folder") || strstr(low, "fix") || strstr(low, "creat")
	    || strstr(low, "make ") || strstr(low, "build") || strstr(low, "add ")
	    || strstr(low, "list") || strstr(low, " run") || strstr(low, "edit")
	    || strstr(low, "update") || strstr(low, "change") || strstr(low, "show")
	    || strstr(low, "look") || strstr(low, "check") || strstr(low, "read ")
	    || strstr(low, "open ") || strstr(low, "debug") || strstr(low, "improve")
	    || strstr(low, "style") || strstr(low, "showcase") || strstr(low, ".py")
	    || strstr(low, ".js") || strstr(low, ".html") || strstr(low, ".md")
	    || strstr(low, ".sh") || strstr(low, ".css") || strstr(low, ".c");
}

static void hkReportEmpty(aiData *data, const char *body, const char *what) {
	char err[512];
	const char *label = hkProviderLabel();
	if (body && *body) {
		/* A body that parsed but yielded nothing is a different problem from a
		   body that never arrived, and the useful detail is why the model stopped
		   — a reasoning model can burn its whole output budget and return an
		   empty content field with finish_reason "length". */
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
	aiAddHistory(data, err);
}

static void *aiWorkerThread(void *arg) {
	hk_turn_spoke = 0;
	aiData *data = (aiData *)arg;

	data->turn_start_ms = clWallMs();

	pthread_mutex_lock(&data->lock);
	char *prompt = data->current_prompt ? strdup(data->current_prompt) : NULL;
	if (prompt) aiPushMessage(data, "user", prompt);
	pthread_mutex_unlock(&data->lock);
	free(prompt);

	int max_iters = 8;
	{ const char *mi = getenv("HAKO_MAX_ITERS"); if (mi && *mi) { int v = atoi(mi); if (v >= 1 && v <= 40) max_iters = v; } }
	int iter = 0;
	int used_tool = 0;
	int repaired = 0;
	int oriented = 0;
	char **xseen = NULL; int xseen_n = 0, xseen_cap = 0;
	int loop_warned = 0;
	int token_retried = 0;
	hk_turn_edited_n = 0;

	while (iter++ < max_iters) {
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
			else aiAddHistory(data, "Error: empty response from hako engine");
			pthread_mutex_unlock(&data->lock);

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

		hkHttpReq hreq = { "POST", parts.url, parts.headers, parts.body, parts.stream };
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
		int streaming_mode = (E.ai_provider_type == AI_PROVIDER_ANTHROPIC) &&
			(oauth_anth_resp || (E.ai_stream && !(E.ai_tools_enabled && hkProjectTrusted())));

		char buffer[8192];
		char *full_response = NULL;
		size_t total = 0;

		if (streaming_mode) {
			char *acc = calloc(1, 1);
			size_t acc_len = 0;
			int printed_prefix = 0;
			int suppress_live = oauth_anth_resp;

			while (hk_http_gets(http, buffer, sizeof(buffer))) {
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

				if (!suppress_live && !E.pipe_mode) {
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
			hk_http_close(http);
			clStopAnim(data);
			if (full_response) full_response[total] = '\0';

			int has_tool_call_in_acc = acc_len > 0 &&
				(strstr(acc, "<function_calls>") || strstr(acc, "<invoke name=") ||
				 strstr(acc, "<tool "));
			if ((suppress_live || E.pipe_mode) && acc_len > 0 && !has_tool_call_in_acc) {
				const char *render_start = acc;
				const char *render_end = acc + acc_len;

				const char *last_close = NULL;
				for (const char *q = acc; q < acc + acc_len - 6; q++) {
					if (!strncmp(q, "</function_calls>", 17)) { last_close = q + 17; q += 16; }
					else if (!strncmp(q, "</tool>", 7))       { last_close = q + 7;  q += 6; }
				}
				if (last_close) render_start = last_close;

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
					if (E.pipe_mode) {
						char save = clean[ci];
						clean[ci] = '\0';
						clPipeEmitMsg("ai", clean + cs);
						clean[ci] = save;
					} else {
						clStopAnim(data);
						if (E.color_enabled) printf("%s◆  ", TH_AI);
						else printf("◆  ");
						fwrite(clean + cs, 1, ci - cs, stdout);
						if (E.color_enabled) printf("%s\n", ANSI_RESET);
						else printf("\n");
						fflush(stdout);
					}
				}
				free(clean);
			} else if (printed_prefix) {
				if (E.color_enabled) printf("%s\n", ANSI_RESET);
				else printf("\n");
				fflush(stdout);
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
					|| (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic"))
					|| (E.ai_provider_type == AI_PROVIDER_MITHRAEUM);
				if (prose_active && hkHasToolBlock(acc)) {
					int dups = 0;
					int n = hkReactToolExecAll(data, acc, &xseen, &xseen_n, &xseen_cap, &dups, NULL);
					if (n > 0) {
						free(full_response);
						used_tool = 1;
						continue;
					}
				}
			} else {
				free(acc);
				if (E.ai_oauth_refresh && !token_retried) {
					token_retried = 1;
					free(full_response);
					clOAuthRefresh(data);
					continue;
				}
				pthread_mutex_lock(&data->lock);
				hkReportEmpty(data, full_response, "stream");
				pthread_mutex_unlock(&data->lock);
			}

			free(full_response);
			break;
		}

		while (hk_http_gets(http, buffer, sizeof(buffer))) {
			int blen = strlen(buffer);
			full_response = realloc(full_response, total + blen + 1);
			memcpy(full_response + total, buffer, blen);
			total += blen;
		}
		hk_http_close(http);
		clStopAnim(data);
		if (full_response) full_response[total] = '\0';

		if (E.debug && full_response) {
			fprintf(stderr, "\n[debug] response (%zu bytes):\n%s\n[/debug]\n", total, full_response);
		}

		int tool_use_anthropic = E.ai_provider_type == AI_PROVIDER_ANTHROPIC
			&& full_response
			&& strstr(full_response, "\"stop_reason\":\"tool_use\"");
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

		int prose_active = (E.ai_toolmode == 1)
			|| (E.ai_oauth_provider && !strcmp(E.ai_oauth_provider, "anthropic"))
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
	if (iter >= max_iters && used_tool) aiAddHistory(data, "(tool loop cap reached)");
	data->last_turn_ms = clWallMs() - data->turn_start_ms;
	data->streaming = 0;
	pthread_mutex_unlock(&data->lock);

	if (E.pipe_mode) {
		if (!hk_turn_spoke && data->current_response && *data->current_response) {
			char *norm = hkNormalizeToolDialect(data->current_response);
			clPipeEmitMsg("ai", norm ? norm : data->current_response);
			free(norm);
		}
		clPipeEmitDone(data);
	}

	return NULL;
}

static void aiWorkerSend(aiData *data) {
	if (!data || data->streaming) return;
	data->streaming = 1;
#ifdef HAKO_WASM
	/* No threads here. The turn runs inline, which is also what the caller wants:
	   every front end on this target drives one turn at a time and waits for it.
	   The spinner a terminal would show is the front end's job instead. */
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
		"mithraeum", "anthropic", "openai", "gemini", "ollama", "groq",
		"cerebras", "deepseek", "mistral", "together", "fireworks",
		"openrouter", "xai", "custom"
	};
	int pn = (int)(sizeof(provs) / sizeof(provs[0]));
	int cur = 0;
	const char *active = hkProviderName(E.ai_provider_type);
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
	if (changed) hkSaveSession();
	aiSayf(data, "%s: %s%s%s", label, *flag ? "on" : "off",
	       changed ? " (saved)" : "", note ? note : "");
	return 1;
}

static int hkHandleSlash(aiData *data, const char *prompt) {
	if (prompt[0] != '/' && prompt[0] != ':') return 0;
	const char *cmd = prompt + 1;
	const char *arg = strchr(cmd, ' ');
	int cmdlen = arg ? (arg - cmd) : (int)strlen(cmd);
	if (arg) { while (*arg == ' ') arg++; }

	static char plural_pick[96];
	if ((!arg || !*arg) && isatty(STDIN_FILENO)) {
		if (cmdlen == 6 && !strncmp(cmd, "models", 6)) {
			if (hkPickModel(data, plural_pick, sizeof plural_pick)) {
				cmd = "model"; cmdlen = 5; arg = plural_pick;
			}
		} else if (cmdlen == 9 && !strncmp(cmd, "providers", 9)) {
			if (hkPickProvider(plural_pick, sizeof plural_pick)) {
				cmd = "provider"; cmdlen = 8; arg = plural_pick;
			}
		}
	}

	if (strncmp(cmd, "help", cmdlen) == 0 && cmdlen == 4) {
		static const char *HELP[] = {
			":help   this box · / filters · esc closes",
			":clear  :retry  :edit  :undo  :usage  :q",
			"",
			":model   :models [refresh]   bare = picker",
			":provider  :providers        bare = picker",
			":theme [<name>]              bare = picker",
			":pull <model>                fetch local weights",
			"",
			":login [<provider>]  :logout [<provider>]  :accounts",
			":history [local|global]      :skills [reload]",
			":skill install <url>         :skill uninstall <name>",
			"",
			":tools on|off                :toolgate on|off",
			":toolmode native|prose       :trust [revoke]",
			":auto on|off                 skip permission prompts",
			":mcp [reload]",
			"",
			":sessions [clear [all]]  :resume <id>  :session [new]",
			"",
			"TAB completes · `/` works as a command prefix too",
		};
		int hn = (int)(sizeof(HELP) / sizeof(HELP[0]));
		if (isatty(STDIN_FILENO)) { clPopupSelect("help", HELP, hn, 0, NULL); return 1; }
		for (int i = 0; i < hn; i++) aiAddHistory(data, HELP[i]);
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
		if (strcmp(prov, "github-models") == 0 || strcmp(prov, "ghmodels") == 0) {
			clOAuthGithubModels(data);
			return 1;
		}
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
			aiSayf(data, "no console URL for '%s'", prov);
			return 1;
		}
		if (url) {
			aiSayf(data, "opening %s", url);
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
		aiSayf(data, "key saved for %s (~/.hako/credentials, mode 0600, obfuscated)", hkProviderLabel());
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
			const char *active = !strcmp(c->provider, hkProviderName(E.ai_provider_type)) ? " *" : "";
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
		if ((!arg || !*arg) && isatty(STDIN_FILENO)
			&& hkPickModel(data, modelpick, sizeof modelpick)) arg = modelpick;
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
			aiSayf(data, "model: %s (saved)", E.ai_model);

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
					snprintf(w, sizeof(w), "%s/%s/%s.mlf2", mdir, de->d_name, de->d_name);
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
		struct group { const char *header; struct row rows[10]; };
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
#endif
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
		char provpick[32];
		if ((!arg || !*arg) && hkPickProvider(provpick, sizeof provpick)) arg = provpick;
		if (arg && *arg) {
			enum aiProviderType t = hkParseProvider(arg);
			if (t == AI_PROVIDER_NONE) {
				aiAddHistory(data, "unknown. valid: ollama, anthropic, openai, gemini/google,");
				aiAddHistory(data, "  groq, cerebras, deepseek, mistral, together, fireworks,");
				aiAddHistory(data, "  openrouter, xai/grok, custom");
			} else {
				enum aiProviderType prev = E.ai_provider_type;
				clCredsCaptureCurrent();
				hkApplyProviderAlias(arg);
				clCredsRestoreFor(hkProviderLabel());
				if (prev != E.ai_provider_type) {
#ifndef _WIN32
					if (prev == AI_PROVIDER_MITHRAEUM) hkHakmKill();
#endif
					pthread_mutex_lock(&data->lock);
					int before = data->message_count;
					aiFlattenMessages(data);
					int after = data->message_count;
					pthread_mutex_unlock(&data->lock);
					if (before != after) {
						aiSayf(data, "(flattened %d tool turn(s) for swap)", before - after);
					}
				}
				{
					static char live1[HK_MODELS_MAX][96];
					clOAuthEnsureFresh(data);
					int lv = hkModelsLive(live1, HK_MODELS_MAX, 0, NULL);
					int known = 0;
					for (int i = 0; i < lv && E.ai_model; i++)
						if (!strcmp(E.ai_model, live1[i])) { known = 1; break; }
					if (!hkModelFitsProvider(E.ai_provider_type, E.ai_model) || (lv > 0 && !known)) {
						const char *pref = hkProviderDefaultModel(E.ai_provider_type);
						const char *dm = NULL;
						for (int i = 0; i < lv && pref; i++)
							if (!strcmp(pref, live1[i])) { dm = live1[i]; break; }
						if (!dm && lv > 0) dm = live1[0];
						if (!dm) dm = pref;
						if (dm && (!E.ai_model || strcmp(dm, E.ai_model))) {
							free(E.ai_model); E.ai_model = strdup(dm);
							char mm[192];
							snprintf(mm, sizeof(mm), "model \xE2\x86\x92 %s (prior model not valid for %s; :model to change, :models to list)",
								dm, hkProviderLabel());
							aiAddHistory(data, mm);
						} else if (!dm) {
							aiAddHistory(data, "note: pick a model for this provider — :model <id> (:models to list)");
						}
					}
				}
				clCredsSave();
				hkSaveSession();
				char msg[256];
				snprintf(msg, sizeof(msg), "provider: %s (saved)%s%s",
					hkProviderLabel(),
					hkProviderDefaultEndpoint(arg) ? " endpoint=" : "",
					hkProviderDefaultEndpoint(arg) ? hkProviderDefaultEndpoint(arg) : "");
				aiAddHistory(data, msg);
			}
		} else {
			aiSayf(data, "provider: %s", hkProviderName(E.ai_provider_type));
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
			aiSayf(data, "reloaded %d skill(s)", n);
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
			if (hk_fs_remove(path) == 0) {
				int n = hkLoadSkills(data);
				aiSayf(data, "uninstalled: %s (%d remain)", name, n);
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
		hk_fs_mkdirp(dir);
		hk_fs_mkdirp(skills);
		const char *slash = strrchr(url, '/');
		const char *name = slash ? slash + 1 : url;
		char outpath[1024];
		snprintf(outpath, sizeof(outpath), "%s/%s", skills, name);
		int nlen = strlen(name);
		if (nlen < 4 || strcmp(name + nlen - 3, ".md") != 0) {
			snprintf(outpath, sizeof(outpath), "%s/%s.md", skills, name);
		}
		hkHttpReq skreq = { "GET", url, NULL, NULL, 0 };
		char *skbody = hk_http_fetch(&skreq);
		if (!skbody || !*skbody) { free(skbody); aiAddHistory(data, "download failed"); return 1; }
		int wrote = hk_fs_write(outpath, skbody, strlen(skbody), 0);
		free(skbody);
		if (wrote != 0) { aiAddHistory(data, "download failed"); return 1; }
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
		if (!arg || !*arg) {
			const char *names[16];
			int cur = 0;
			for (int i = 0; i < TH_PRESET_COUNT && i < 16; i++) {
				names[i] = TH_PRESETS[i].name;
				if (!strcmp(TH_PRESETS[i].name, TH_ACTIVE)) cur = i;
			}
			int pick = clPopupSelect("theme", names, TH_PRESET_COUNT, cur, clThemePreview);
			if (pick < 0) { aiAddHistory(data, "theme: unchanged"); return 1; }
			clThemeApply(names[pick]);
			hkSaveSession();
			aiSayf(data, "theme: %s (saved)", TH_ACTIVE);
			return 1;
		}
		int found = 0;
		for (int i = 0; i < TH_PRESET_COUNT; i++) {
			if (!strcmp(TH_PRESETS[i].name, arg)) { found = 1; break; }
		}
		if (!found) { aiAddHistory(data, "theme: unknown. try :theme without args to list."); return 1; }
		clThemeApply(arg);
		hkSaveSession();
		aiSayf(data, "theme: %s (saved)", TH_ACTIVE);
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
		aiSayf(data, "provider: %s", hkProviderName(E.ai_provider_type));
		aiSayf(data, "model:    %s", E.ai_model ? E.ai_model : "(unset)");
		aiSayf(data, "tools:    %s", E.ai_tools_enabled ? "on" : "off");
		aiSayf(data, "trust:    %s", hkProjectTrusted() ? "granted" : "not granted");
		aiSayf(data, "skills:   %d loaded", hkLoadSkills(data));
		aiSayf(data, "stream:   %s", E.ai_stream ? "on" : "off");
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
			aiClearHistory(data);
			E.session_started = hk_time_unix();
			E.session_turn_count = 0;
			E.session_resumed = 0;
			hkGenSessionId();
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
		hkLoadHistoryTail(data, 200);
		return 1;
	}
	if (strncmp(cmd, "session", cmdlen) == 0 && cmdlen == 7) {
		if (arg && strcmp(arg, "new") == 0) {
			aiClearHistory(data);
			E.session_started = hk_time_unix();
			E.session_turn_count = 0;
			E.session_resumed = 0;
			hkGenSessionId();
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
	aiAddHistory(data, "unknown command (/help)");
	return 1;
}

static void clLoadRc(void) {
	const char *home = getenv("HOME");
	if (!home) return;
	char path[512];
	snprintf(path, sizeof(path), "%s/.hakorc", home);
	char *rcbuf = hk_fs_read(path, 256 * 1024, NULL);
	if (!rcbuf) return;
	char *rcsave = rcbuf, *line;
	while ((line = hkNextLine(&rcsave)) != NULL) {
		if (line[0] == '#' || line[0] == '\0') continue;
		char *eq = strchr(line, '='); if (!eq) continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;
		if (strcmp(key, "ai_provider") == 0) { hkApplyProviderAlias(val); hk_rc_pin.provider = 1; }
		else if (strcmp(key, "ai_api_key") == 0) { free(E.ai_api_key); E.ai_api_key = strdup(val); hk_rc_pin.api_key = 1; }
		else if (strcmp(key, "ai_endpoint") == 0) { free(E.ai_endpoint); E.ai_endpoint = strdup(val); hk_rc_pin.endpoint = 1; }
		else if (strcmp(key, "ai_model") == 0) { free(E.ai_model); E.ai_model = strdup(val); hk_rc_pin.model = 1; }
		else if (strcmp(key, "ai_max_tokens") == 0) { E.ai_max_tokens = atoi(val); hk_rc_pin.max_tokens = 1; }
		else if (strcmp(key, "ai_tools_enabled") == 0) { E.ai_tools_enabled = atoi(val) ? 1 : 0; hk_rc_pin.tools = 1; }
		else if (strcmp(key, "ai_stream") == 0) { E.ai_stream = atoi(val) ? 1 : 0; hk_rc_pin.stream = 1; }
		else if (strcmp(key, "show_splash") == 0) { E.show_splash = atoi(val) ? 1 : 0; }
		else if (strcmp(key, "ai_auto_approve") == 0) { E.ai_auto_approve = atoi(val) ? 1 : 0; hk_rc_pin.auto_approve = 1; }
		else if (strcmp(key, "anim_style") == 0) {
			E.anim_force_style = -1;
			for (int i = 0; i < CL_ANIM_COUNT; i++) {
				if (strcmp(val, CL_ANIMS[i].name) == 0) { E.anim_force_style = i; break; }
			}
		}
	}
	free(rcbuf);
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
	E.ai_auto_approve = 0;
	E.anim_force_style = -1;
#ifndef _WIN32
	E.color_enabled = isatty(STDOUT_FILENO) ? 1 : 0;
	E.show_splash = 1;
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
	if (write(STDOUT_FILENO, "\x1b[?2004h", 8) < 0) {}
	return 0;
}

static void clDisableRaw(void) {
	if (cl_raw_active) {
		if (write(STDOUT_FILENO, "\x1b[?2004l", 8) < 0) {}
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

static int cl_redraw_oldrows = 0;
static int cl_redraw_oldrpos = 0;

static void clRedrawReset(void) { cl_redraw_oldrows = 0; cl_redraw_oldrpos = 0; }

static void clAppend(char *ab, int *n, int cap, const char *s, int slen) {
	if (*n + slen >= cap) return;
	memcpy(ab + *n, s, slen);
	*n += slen;
}

static const char *HK_COLON_CMDS[] = {
	"accounts","auto","clear","edit","exit","help","history","login","logout","mcp",
	"model","models","provider","providers","pull","q","quit","resume","retry","session",
	"sessions","skill","skills","theme","toolgate","toolmode","tools","trust","undo","usage", NULL
};

static char *clGhostSuffix(const char *buf, int len) {
	if (len < 3) return NULL;
	if (buf[0] != ':' && buf[0] != '/') return NULL;
	const char *p = buf + 1;
	int pl = len - 1;
	const char *only = NULL; int matches = 0;
	for (int i = 0; HK_COLON_CMDS[i]; i++) {
		int cl = (int)strlen(HK_COLON_CMDS[i]);
		if (cl > pl && memcmp(HK_COLON_CMDS[i], p, pl) == 0) { only = HK_COLON_CMDS[i]; matches++; }
	}
	if (matches == 1) return strdup(only + pl);
	return NULL;
}

#define CL_COMP_MAX 512

static const char *HK_PROVIDER_WORDS[] = {
	"mithraeum", "anthropic", "anthropic-api", "claude", "claude-api", "openai",
	"github-copilot", "copilot", "github-models", "ghmodels",
	"ollama", "ollamacloud", "ocloud", "local", "koi",
	"gemini", "google", "groq", "cerebras", "deepseek", "mistral",
	"together", "fireworks", "openrouter", "openrouter-api",
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
		if (slash) snprintf(cand, sizeof(cand), "%.*s/%s%s", (int)(slash - word), word, de->d_name, isdir ? "/" : "");
		else snprintf(cand, sizeof(cand), "%s%s", de->d_name, isdir ? "/" : "");
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

	if ((buf[0] == ':' || buf[0] == '/') && ws == 0) {
		*wstart = 1;
		const char *pfx = buf + 1;
		size_t plen = (size_t)(len - 1);
		for (int i = 0; HK_COLON_CMDS[i]; i++) n = clCompAdd(out, n, HK_COLON_CMDS[i], pfx, plen);
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
			for (int i = 0; i < TH_PRESET_COUNT; i++) n = clCompAdd(out, n, TH_PRESETS[i].name, word, wlen);
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

static void clCompList(char cand[][160], int n) {
	int cols = clTermCols();
	int w = 0;
	for (int i = 0; i < n; i++) { int l = (int)strlen(cand[i]); if (l > w) w = l; }
	w += 2;
	int percol = w > 0 ? cols / w : 1;
	if (percol < 1) percol = 1;
	putchar('\n');
	for (int i = 0; i < n; i++) {
		printf("%s%-*s%s", TH_META, w, cand[i], ANSI_RESET);
		if ((i + 1) % percol == 0) putchar('\n');
	}
	if (n % percol) putchar('\n');
	fflush(stdout);
}

static char cl_last_ghost[512];

static void clRedrawLine(const char *prompt, const char *buf, int len, int cursor) {
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

	int rows = (plen + len + glen + cols - 1) / cols;
	if (rows < 1) rows = 1;

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
	if (cursor == len && (len + glen) > 0 && (plen + len + glen) % cols == 0) {
		clAppend(ab, &n, sizeof(ab), "\n\r", 2);
		rows_after++;
	}

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

	clAppend(ab, &n, sizeof(ab), "\x1b[?25h", 6);

	cl_redraw_oldrows = rows_after;
	cl_redraw_oldrpos = rpos2;

	if (write(STDOUT_FILENO, ab, n) < 0) { free(ghost); return; }
	free(ghost);
}

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
			if (len + 1 < (int)sizeof(buf)) {
				memmove(&buf[cur+1], &buf[cur], len - cur);
				buf[cur++] = ' ';
				len++; buf[len] = '\0';
				clRedrawLine(prompt, buf, len, cur);
			}
			continue;
		}

		if (c == 3) {
			if (write(STDOUT_FILENO, "^C\r\n", 4) < 0) {}
			clDisableRaw();
			out[0] = '\0';
			return -2;
		}
		if (c == 4) {
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
					clCompList(cand, nc);
					clRedrawReset();
				}
			}			clRedrawLine(prompt, buf, len, cur);
			continue;
		}
		if (c == 12) {
			if (write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7) < 0) {}
			clRedrawReset();
			clRedrawLine(prompt, buf, len, cur);
			continue;
		}
		if (c == 18) {
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
	hkHttpReq req = { "POST", url, extra_headers, json_body, 0 };
	return hk_http_fetch(&req);
}

static char *clCurlGet(const char *url, const char *extra_headers) {
	hkHttpReq req = { "GET", url, extra_headers, NULL, 0 };
	char *buf = hk_http_fetch(&req);
	return buf;
}

static int clOAuthGithubCopilot(aiData *data) {
	const char *client_id = clOAuthClientId("github-copilot");
	if (!client_id) { aiAddHistory(data, "OAuth: no copilot client_id"); return -1; }

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
	return 0;
}

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
	return 0;
#endif
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
	clOpenUrl(url);

	printf("  paste code (input hidden): ");
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

	hkHttpReq exch = { "POST", HAKO_ANTHROPIC_TOKEN_URL, NULL, body, 0 };
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
		free(E.ai_model); E.ai_model = strdup("claude-haiku-4-5-20251001");
	}
	clCredsCaptureCurrent();
	clCredsSave();
	hkSaveSession();
	aiAddHistory(data, "Anthropic OAuth: signed in. Calls now bill against your Claude subscription.");
	return 0;
}

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

static int clOAuthLoopbackListen(int *out_port) {
#ifdef HAKO_WASM
	(void)out_port;
	return -1;
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
#ifdef HAKO_WASM
	(void)srv_fd; (void)timeout_sec;
	return NULL;
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
#endif

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
	hkHttpReq rfsh = { "POST", HAKO_ANTHROPIC_TOKEN_URL, NULL, body, 0 };
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
	(void)system(rmcmd);
	return 0;
#endif
}


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

static void clClipToCells(char *buf, int cells) {
	if (cells < 0) cells = 0;
	int n = 0;
	unsigned char *q = (unsigned char *)buf;
	while (*q) {
		int adv = 1;
		while (q[adv] && (q[adv] & 0xc0) == 0x80) adv++;
		if (n + 1 > cells) { *q = '\0'; return; }
		n++; q += adv;
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

#define CL_POPUP_VIS       12
#define CL_POPUP_TEXT_MAX  40
static int clPopupSelect(const char *title, const char **items, int n,
                         int cur, clPopupPreview preview) {
	if (n <= 0) return -1;
	if (cur < 0 || cur >= n) cur = 0;

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

	int *vis = malloc(sizeof(int) * (size_t)n);
	if (!vis) return -1;
	char flt[64] = "";
	int flen = 0, filtering = 0;
	int nv = n, vpos = cur, top = 0;
	for (int i = 0; i < n; i++) vis[i] = i;

	int term_cols = clTermCols(), term_rows = clTermRows();
	char pv[128];
	int inner = clCellWidth(title ? title : "") + 2;
	for (int i = 0; i < n; i++) {
		int w = 4 + clCellWidth(items[i]);
		if (preview) { preview(i, pv, sizeof pv); w += 2 + clCellWidth(pv); }
		if (w > inner) inner = w;
	}
	if (inner > 64) inner = 64;
	if (inner > term_cols - 2) inner = term_cols - 2;
	if (inner < 12) inner = 12;

	int maxvis = term_rows - 5;
	if (maxvis > CL_POPUP_VIS) maxvis = CL_POPUP_VIS;
	if (maxvis < 3) maxvis = 3;

	{
		int reserve = (n < maxvis ? n : maxvis) + 3;
		for (int i = 0; i < reserve; i++) { if (write(STDOUT_FILENO, "\n", 1) < 0) {} }
		char up[16];
		int k = snprintf(up, sizeof up, "\x1b[%dA\r", reserve);
		if (write(STDOUT_FILENO, up, k) < 0) {}
	}

	int prev_rows = 0, result = -1, refilter = 0;
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
		if (vpos < top) top = vpos;
		if (vpos >= top + rows) top = vpos - rows + 1;
		if (top < 0) top = 0;

		if (prev_rows) {
			char up[16]; int k = snprintf(up, sizeof up, "\r\x1b[%dA", prev_rows + 2);
			if (write(STDOUT_FILENO, up, k) < 0) {}
			if (prev_rows > rows) {
				for (int i = 0; i < prev_rows + 3; i++) { if (write(STDOUT_FILENO, "\x1b[2K\n", 5) < 0) {} }
				char back[16]; int b = snprintf(back, sizeof back, "\x1b[%dA\r", prev_rows + 3);
				if (write(STDOUT_FILENO, back, b) < 0) {}
			}
		}

		char head[96];
		if (nv != n) snprintf(head, sizeof head, "%s (%d/%d)", title ? title : "", nv, n);
		else snprintf(head, sizeof head, "%s", title ? title : "");
		if (E.color_enabled) fputs(TH_ACCENT, stdout);
		fputs("\r╭─ ", stdout);
		if (E.color_enabled) fputs(ANSI_BOLD, stdout);
		fputs(head, stdout);
		if (E.color_enabled) { fputs(ANSI_RESET, stdout); fputs(TH_ACCENT, stdout); }
		fputc(' ', stdout);
		for (int i = clCellWidth(head) + 3; i < inner; i++) fputs("─", stdout);
		fputs("╮\x1b[K\n", stdout);

		if (nv == 0) {
			if (E.color_enabled) fputs(TH_ACCENT, stdout);
			fputs("\r│", stdout);
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
			fputs("\r│", stdout);
			if (E.color_enabled) fputs(sel ? TH_ACCENT : ANSI_RESET, stdout);
			fputs(sel ? " › " : "   ", stdout);
			if (E.color_enabled) fputs(sel ? ANSI_BOLD : TH_AI, stdout);
			int room = inner - 3 - (preview ? 0 : 1);
			int ilen = clCellWidth(items[i]);
			if (ilen > room) {
				char clip[192];
				snprintf(clip, sizeof clip, "%s", items[i]);
				clClipToCells(clip, room - 1);
				printf("%s…", clip);
				ilen = room;
			} else fputs(items[i], stdout);
			if (E.color_enabled) fputs(ANSI_RESET, stdout);
			int used = 3 + ilen;
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
		fputs("\r╰", stdout);
		for (int i = 0; i < inner; i++) fputs("─", stdout);
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

		char c;
		if (read(STDIN_FILENO, &c, 1) != 1) { result = -1; break; }
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
	}

	free(vis);
	clDisableRaw();
	{ char up[16]; int k = snprintf(up, sizeof up, "\r\x1b[%dA", prev_rows + 2); if (write(STDOUT_FILENO, up, k) < 0) {} }
	for (int i = 0; i < prev_rows + 3; i++) { if (write(STDOUT_FILENO, "\x1b[2K\n", 5) < 0) {} }
	{ char up[16]; int k = snprintf(up, sizeof up, "\x1b[%dA\r", prev_rows + 3); if (write(STDOUT_FILENO, up, k) < 0) {} }
	return result;
}

static void clThemePreview(int idx, char *out, size_t cap) {
	if (!cap) return;
	if (idx < 0 || idx >= TH_PRESET_COUNT || !E.color_enabled) { out[0] = '\0'; return; }
	char accent[16], ok[16], tool[16];
	snprintf(accent, sizeof accent, "%s", clColorToken(TH_PRESETS[idx].c[6]));
	snprintf(ok,     sizeof ok,     "%s", clColorToken(TH_PRESETS[idx].c[5]));
	snprintf(tool,   sizeof tool,   "%s", clColorToken(TH_PRESETS[idx].c[4]));
	snprintf(out, cap, "%s██%s%s██%s%s██%s", accent, ANSI_RESET, ok, ANSI_RESET, tool, ANSI_RESET);
}

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

	int cw = clCellWidth(content);
	if (cw <= inner) {
		fputs(content, stdout);
	} else {
		int emitted = 0;
		const unsigned char *p = (const unsigned char *)content;
		while (*p && emitted < inner - 1) {
			if (*p == 0x1b && p[1] == '[') {
				fputc(*p++, stdout);
				while (*p && *p != 'm') fputc(*p++, stdout);
				if (*p) fputc(*p++, stdout);
				continue;
			}
			fputc(*p++, stdout);
			while ((*p & 0xc0) == 0x80) fputc(*p++, stdout);
			emitted++;
		}
		fputs("…", stdout);
		cw = inner;
	}

	if (E.color_enabled) fputs(ANSI_RESET, stdout);
	int pad = inner - (cw > inner ? inner : cw);
	for (int i = 0; i < pad; i++) putchar(' ');
	if (E.color_enabled) fputs(TH_ACCENT, stdout);
	fputs("│", stdout);
	if (E.color_enabled) fputs(ANSI_RESET, stdout);
	fputc('\n', stdout);
}


static void clSplash(void) {
	if (!E.show_splash || E.pipe_mode || !E.color_enabled) return;
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;

	int cols = clTermCols(), rows = clTermRows();
	char version[64];
	snprintf(version, sizeof(version), "v%s", HAKO_VERSION);
	const char *tagline = "standalone llm agent";
	const char *press = "Press any key to start";

	int want_word  = (rows >= 15 && cols >= 36);
	int want_crate = (rows >= 15 && cols >= 30) && (rows >= 26 || !want_word);

	printf("\x1b[?25l\x1b[2J\x1b[H");

	if (!want_word && !want_crate) {
		int top = (rows - 3) / 2; if (top < 0) top = 0;
		for (int i = 0; i < top; i++) putchar('\n');
		char line[96];
		snprintf(line, sizeof(line), "\xe7\xae\xb1 HAKO CODE %s", version);
		int pad = (cols - clCellWidth(line)) / 2; if (pad < 0) pad = 0;
		printf("%*s%s%s%s\n\n", pad, "", TH_ACCENT, line, ANSI_RESET);
		pad = (cols - (int)strlen(press)) / 2; if (pad < 0) pad = 0;
		printf("%*s%s\x1b[5m%s\x1b[25m%s", pad, "", TH_META, press, ANSI_RESET);
	} else {
		const char **crate = (cols >= 42) ? CL_LOGO_MEDIUM : CL_LOGO_TINY;
		int crate_rows = 0;
		if (want_crate) for (int i = 0; crate[i]; i++) crate_rows++;
		int word_rows = 0;
		if (want_word) for (int i = 0; CL_LOGO_WORD[i]; i++) word_rows++;

		int block = word_rows + crate_rows + 6;
		int top = (rows - block) / 2; if (top < 0) top = 0;
		for (int i = 0; i < top; i++) putchar('\n');

		for (int i = 0; want_word && CL_LOGO_WORD[i]; i++) {
			int pad = (cols - clCellWidth(CL_LOGO_WORD[i])) / 2; if (pad < 0) pad = 0;
			printf("%*s%s%s%s\n", pad, "", TH_ACCENT, CL_LOGO_WORD[i], ANSI_RESET);
		}
		if (want_word && want_crate) putchar('\n');
		for (int i = 0; want_crate && crate[i]; i++) {
			int pad = (cols - clCellWidth(crate[i])) / 2; if (pad < 0) pad = 0;
			printf("%*s%s%s%s\n", pad, "", TH_AI, crate[i], ANSI_RESET);
		}
		printf("\n%*s%s%sCODE%s\n", (cols - 4) / 2, "", TH_ACCENT, ANSI_BOLD, ANSI_RESET);
		printf("%*s%s%s%s\n", (cols - (int)strlen(version)) / 2, "", TH_META, version, ANSI_RESET);
		printf("%*s%s%s%s\n\n", (cols - (int)strlen(tagline)) / 2, "", TH_META, tagline, ANSI_RESET);
		int pad = (cols - (int)strlen(press)) / 2; if (pad < 0) pad = 0;
		printf("%*s%s\x1b[5m%s\x1b[25m%s", pad, "", TH_META, press, ANSI_RESET);
	}
	fflush(stdout);

	if (clEnableRaw() == 0) {
		char c;
		if (read(STDIN_FILENO, &c, 1) < 0) {}
		clDisableRaw();
	}
	printf("\x1b[?25h\x1b[2J\x1b[H");
	fflush(stdout);
}

static void clBanner(aiData *data) {
	int cols = clTermCols();
	int sk = hkLoadSkills(data);
	const char *prov = hkProviderName(E.ai_provider_type);
	const char *model = E.ai_model ? E.ai_model : "—";
	const char *trust_str = hkProjectTrusted() ? "on" : "off";
	const char *sess_state = E.session_resumed ? "resumed" : "new";
	const char *sess_id = E.session_id ? E.session_id : "?";

	const char **logo = (cols >= 42) ? CL_LOGO_MEDIUM : CL_LOGO_TINY;
	int logo_w = 0;
	for (int i = 0; logo[i]; i++) { int w = clCellWidth(logo[i]); if (w > logo_w) logo_w = w; }

	(void)sess_id;
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
	int max_row = E.show_splash ? 0 : logo_w;
	if (row_a > max_row) max_row = row_a;
	if (row_b > max_row) max_row = row_b;
	if (row_c > max_row) max_row = row_c;
	int inner = max_row + 4;
	if (inner > cols - 2) inner = cols - 2;
	if (inner < 24) {
		printf("\n  %s◆ hako %s%s  %s· %s · %s · trust %s%s\n",
			E.color_enabled ? TH_ACCENT : "", HAKO_VERSION, E.color_enabled ? ANSI_RESET : "",
			E.color_enabled ? TH_META : "", prov, model, trust_str, E.color_enabled ? ANSI_RESET : "");
		fflush(stdout);
		return;
	}

	putchar('\n');
	if (cols >= 36) {
		for (int i = 0; CL_LOGO_WORD[i]; i++) {
			int pad = (inner + 2 - clCellWidth(CL_LOGO_WORD[i])) / 2; if (pad < 0) pad = 0;
			printf("%*s%s%s%s\n", pad, "", E.color_enabled ? TH_ACCENT : "",
				CL_LOGO_WORD[i], E.color_enabled ? ANSI_RESET : "");
		}
		putchar('\n');
	}
	clBoxTop(inner);
	char rowbuf[256];
	for (int i = 0; logo[i] && !E.show_splash; i++) {
		int w = clCellWidth(logo[i]);
		int lpad = (inner - w) / 2; if (lpad < 0) lpad = 0;
		snprintf(rowbuf, sizeof(rowbuf), "%*s%s", lpad, "", logo[i]);
		clBoxRow(inner, rowbuf, TH_AI);
	}
	if (!E.show_splash) clBoxRow(inner, "", NULL);
	clBoxRow(inner, row_a_buf, TH_ACCENT);
	clBoxRow(inner, row_b_buf, TH_META);
	clBoxRow(inner, row_c_buf, TH_META);
	clBoxBot(inner);
	fflush(stdout);
}

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
	if (E.serve_mode) {
		char buf[32];
		if (hkServeInput(q, 0, buf, sizeof(buf)) != 0) return default_yes;
		if (!buf[0]) return default_yes;
		return (buf[0] == 'y' || buf[0] == 'Y');
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
			printf("\n  %sTrust this directory for tool access?%s\n",
				E.color_enabled ? ANSI_BOLD : "", E.color_enabled ? ANSI_RESET : "");
			{
				int cols = clTermCols();
				int avail = cols - 8;
				if (avail < 24) avail = 24;
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
		aiClearHistory(data);
		E.session_started = hk_time_unix();
		E.session_turn_count = 0;
		E.session_resumed = 0;
		hkGenSessionId();
		hkSaveSession();
		printf("  new session: %s\n", E.session_id);
	} else if (pick == 2) {
		printf("\n");
		long now = hk_time_unix();
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
		E.session_started = hk_time_unix();
		hkSaveSession();
		aiClearHistory(data);
		hkLoadHistoryTail(data, 200);
		printf("  resumed: %s (%d msgs loaded)\n", E.session_id, data->history_count);
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

static int clOneShot(aiData *data, const char *prompt) {
	hkLoadSkills(data);
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

static int hkSlashNeedsTerminal(const char *cmd) {
	while (*cmd == ':' || *cmd == '/') cmd++;
	static const char *tty_only[] = { NULL };
	for (int i = 0; tty_only[i]; i++) {
		size_t n = strlen(tty_only[i]);
		if (!strncmp(cmd, tty_only[i], n) && (!cmd[n] || cmd[n] == ' ')) return 1;
	}
	return 0;
}

static void clPipeBegin(aiData *data) {
	clPipeEmitInit();
	if (E.session_resumed && data->history_count == 0)
		hkLoadHistoryTail(data, 40);
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
		if (cmd) {
			int r = hkHandleSlash(data, cmd);
			free(cmd);
			clPipeEmitDone(NULL);
			if (r == 2) quit = 1;
		}
	} else if (strcmp(type, "open") == 0) {
		char *dir = hkExtractJsonString(line, "dir");
		if (dir && *dir && chdir(dir) != 0) {
			clPipeEmitMsg("system", "cannot open that folder");
		} else {
			aiClearHistory(data);
			hkLoadSession();
			clCredsRestoreFor(hkProviderLabel());
			hkHealEndpoint();
			clPipeBegin(data);
		}
		free(dir);
		clPipeEmitDone(NULL);
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

#if defined(__has_include)
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
	long  pending_tool_id;

	char *queue[HS_QUEUE];
	int   q_head, q_tail;

	char  answer[8];
	int   answer_ready;

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
	if (strstr(json, "\"type\":\"tool_request\"")) {
		const char *idp = strstr(json, "\"id\":");
		HS.pending_tool_id = idp ? atol(idp + 5) : 0;
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
	pthread_mutex_lock(&HS.a_lock);
	while (!HS.answer_ready)
		pthread_cond_wait(&HS.a_cond, &HS.a_lock);
	snprintf(out, cap, "%s", HS.answer);
	HS.answer_ready = 0;
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

static void hsAnswerSet(const char *ans) {
	pthread_mutex_lock(&HS.a_lock);
	snprintf(HS.answer, sizeof(HS.answer), "%.7s", ans ? ans : "n");
	HS.answer_ready = 1;
	pthread_cond_broadcast(&HS.a_cond);
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

static char *hsReadBody(int fd, char *req, size_t used) {
	long want = 0;
	const char *cl = strcasestr(req, "\r\nContent-Length:");
	if (cl) want = strtol(cl + 17, NULL, 10);
	if (want < 0 || want > HS_BODY_MAX) want = 0;

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

static int hsOriginOk(const char *req) {
	const char *o = strcasestr(req, "\r\nOrigin:");
	if (!o) return 1;
	o += 9;
	while (*o == ' ') o++;
	if (!strncmp(o, "http://127.0.0.1", 16) || !strncmp(o, "http://localhost", 16)
	 || !strncmp(o, "http://[::1]", 12)     || !strncmp(o, "http://192.168.", 15)
	 || !strncmp(o, "http://10.", 10)) return 1;
	if (!strncmp(o, "http://172.", 11)) {
		int b = atoi(o + 11);
		return b >= 16 && b <= 31;
	}
	return 0;
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
		hsSessionTitle(info[i].id, info[i].first, title, sizeof(title));
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

static void hsServeDirs(int fd, const char *path) {
	char real[PATH_MAX];
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
			snprintf(full, sizeof(full), "%s/%s", real, de->d_name);
			if (stat(full, &st) != 0) continue;
			if (S_ISDIR(st.st_mode)) {
				hsPut(&b, first ? "\"" : ",\"");
				hsPutEsc(&b, de->d_name);
				hsPut(&b, "\"");
				first = 0;
			} else if (S_ISREG(st.st_mode)) {
				hsPut(&files, ffirst ? "{\"name\":\"" : ",{\"name\":\"");
				hsPutEsc(&files, de->d_name);
				hsPutFmt(&files, "\",\"bytes\":%lld}", (long long)st.st_size);
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

static void hsUniquePath(const char *name, char *out, size_t cap) {
	struct stat st;
	snprintf(out, cap, "%s", name);
	if (stat(out, &st) != 0) return;
	const char *dot = strrchr(name, '.');
	int stem = dot ? (int)(dot - name) : (int)strlen(name);
	for (int i = 1; i < 1000; i++) {
		snprintf(out, cap, "%.*s-%d%s", stem, name, i, dot ? dot : "");
		if (stat(out, &st) != 0) return;
	}
}

static long hsRecvToFile(int sock, char *req, size_t used, const char *path, long want) {
	FILE *f = fopen(path, "wb");
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
			snprintf(w, sizeof(w), "%s/%s/%s.mlf2", mroot, de->d_name, de->d_name);
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
	hsPutEsc(&b, hkProviderName(E.ai_provider_type));
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
		snprintf(full, sizeof(full), "%s/%s", root, de->d_name);
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

static void hsServeState(int fd) {
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
	hsBuf b = {0};
	hsPut(&b, "{\"version\":\"" HAKO_VERSION "\",\"dir\":\"");
	hsPutEsc(&b, cwd);
	hsPut(&b, "\",\"home\":\"");
	const char *home = getenv("HOME") ? getenv("HOME") : "/";
	hsPutEsc(&b, home);
	hsPut(&b, "\",\"places\":[");
	{
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
	hsPutFmt(&b, "\",\"turns\":%d,\"busy\":%d,\"yolo\":%d,\"awaiting_input\":%d,\"input_hidden\":%d,\"input_prompt\":\"",
	         E.session_turn_count, HS.busy, E.ai_auto_approve, HS.input_waiting, HS.input_hidden);
	hsPutEsc(&b, HS.input_prompt);
	hsPut(&b, "\",\"caps\":[");
	static const char *caps[] = {
		"prompt", "slash", "approve", "input", "stop", "events", "sessions", "history",
		"dirs", "files", "upload", "models", "providers", "places", "find", "link", "open", "rename", "delete",
		"mkdir", "shell", NULL
	};
	for (int i = 0; caps[i]; i++) {
		hsPut(&b, i ? ",\"" : "\"");
		hsPut(&b, caps[i]);
		hsPut(&b, "\"");
	}
	hsPut(&b, "]}");
	hsSendJson(fd, b.buf ? b.buf : "{}");
	free(b.buf);
}

static void hsServeEvents(int fd) {
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
	hsSend(fd, "500 Internal Server Error", "text/plain",
	       "no UI bundle: rebuild with hako_web.h, or run with --web <dir>\n", 62);
#endif
}

static void hsEnqueueCmd(const char *type, const char *field, const char *value) {
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

static void *hsConnThread(void *arg) {
	int fd = (int)(intptr_t)arg;
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
	if (HS.token[0] && !hsAuthOk(req)) {
		const char *t = strstr(req, "?t=");
		if (t && !strncmp(req, "GET ", 4) && !strncmp(t + 3, HS.token, strlen(HS.token))) {
			/* Hand it to the browser as a cookie and get it out of the URL, so
			   it does not sit in history or leak through a referrer. */
			char head[512];
			int n = snprintf(head, sizeof(head),
				"HTTP/1.1 302 Found\r\nLocation: /\r\n"
				"Set-Cookie: hako_token=%s; Path=/; Max-Age=604800; SameSite=Strict; HttpOnly\r\n"
				"Content-Length: 0\r\n\r\n", HS.token);
			hs_write(fd, head, (size_t)n);
		} else {
			const char *body = "unauthorized\n";
			char head[256];
			int n = snprintf(head, sizeof(head),
				"HTTP/1.1 401 Unauthorized\r\nContent-Type: text/plain\r\n"
				"Content-Length: %zu\r\n\r\n", strlen(body));
			hs_write(fd, head, (size_t)n);
			hs_write(fd, body, strlen(body));
		}
		hs_close(fd);
		return NULL;
	}

	int is_post = (strncmp(req, "POST ", 5) == 0);
	if (is_post && !hsOriginOk(req)) {
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
			"\"display\":\"standalone\",\"background_color\":\"#131209\",\"theme_color\":\"#131209\","
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
		if (name[0] && !strchr(name, '/')) {
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
		static const struct { const char *name, *kind, *desc; } P[] = {
			{ "mithraeum",      "local",  "hako family — runs on this machine" },
			{ "anthropic",      "oauth",  "Claude Pro/Max — sign in with claude.ai" },
			{ "copilot",        "oauth",  "GitHub Copilot Pro/Business" },
			{ "github-models",  "oauth",  "free GitHub Models tier" },
			{ "openrouter",     "oauth",  "PKCE — auto-issue OR API key" },
			{ "ollama",         "local",  "run `ollama serve` first" },
			{ "ollamacloud",    "local",  "hosted Ollama" },
			{ "anthropic-api",  "key",    "Claude API key" },
			{ "openai",         "key",    "ChatGPT API" },
			{ "gemini",         "key",    "Google AI Studio — generous free tier" },
			{ "groq",           "key",    "fastest Llama hosting — free tier" },
			{ "cerebras",       "key",    "ultra-fast inference — free tier" },
			{ "deepseek",       "key",    "DeepSeek API" },
			{ "xai",            "key",    "Grok via xAI API" },
			{ "mistral",        "key",    "OpenAI-compat" },
			{ "together",       "key",    "OpenAI-compat" },
			{ "fireworks",      "key",    "OpenAI-compat" },
			{ NULL, NULL, NULL }
		};
		const char *active = hkProviderLabel();
		hsBuf b = {0};
		hsPut(&b, "{\"active\":\"");
		hsPutEsc(&b, active ? active : "");
		hsPut(&b, "\",\"providers\":[");
		for (int i = 0; P[i].name; i++) {
			clCred *c = clCredsFind(P[i].name);
			int saved = c && (c->api_key || c->oauth_refresh);
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
		hsServeState(fd);
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
	} else if (!strncmp(req, "POST /api/approve", 17)) {
		char *body = hsReadBody(fd, req, used);
		hsAnswerSet(body && *body ? body : "n");
		if (HS.pending_tool_id) {
			char line[128];
			snprintf(line, sizeof(line), "{\"type\":\"tool_decision\",\"id\":%ld,\"answer\":\"%c\"}",
			         HS.pending_tool_id, body && *body ? body[0] : 'n');
			hkServePush(line);
			HS.pending_tool_id = 0;
		}
		free(body);
		hsSendOk(fd, 1);
	} else if (!strncmp(req, "POST /api/upload", 16)) {
		char raw[512], name[256], path[PATH_MAX];
		hsQueryParam(req, "name", raw, sizeof(raw));
		long want = 0;
		const char *cl = strcasestr(req, "\r\nContent-Length:");
		if (cl) want = strtol(cl + 17, NULL, 10);
		if (!hsSafeName(raw, name, sizeof(name)) || want <= 0 || want > HS_UPLOAD_MAX) {
			hsSendOk(fd, 0);
		} else {
			hsUniquePath(name, path, sizeof(path));
			long got = hsRecvToFile(fd, req, used, path, want);
			if (got > 0) {
				char msg[PATH_MAX + 96];
				snprintf(msg, sizeof(msg), "received %s (%ld KB) into this folder", path, (got + 1023) / 1024);
				clPipeEmitMsg("system", msg);
			}
			hsSendOk(fd, got > 0);
		}
	} else if (!strncmp(req, "POST /api/mkdir", 15)) {
		char *body = hsReadBody(fd, req, used);
		int ok = body && *body && !strstr(body, "..") && hk_fs_mkdirp(body) == 0;
		free(body);
		hsSendOk(fd, ok);
	} else if (!strncmp(req, "POST /api/input", 15)) {
		char *body = hsReadBody(fd, req, used);
		hsInputSet(body ? body : "");
		free(body);
		hsSendOk(fd, 1);
	} else if (!strncmp(req, "POST /api/stop", 14)) {
		E.interrupt = 1;
		hsSendOk(fd, 1);
	} else if (!strncmp(req, "POST /api/open", 14) || !strncmp(req, "POST /api/restart", 17)) {
		char *body = hsReadBody(fd, req, used);
		hsRingReset();
		hsEnqueueCmd("open", "dir", body ? body : "");
		free(body);
		hsSendOk(fd, 1);
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

static void *hsAcceptThread(void *arg) {
	int srv = (int)(intptr_t)arg;
	for (;;) {
		int fd = (int)accept(srv, NULL, NULL);
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
	int srv = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) { fprintf(stderr, "hako: socket failed (error %d)\n", hs_errno()); return 1; }
	int yes = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = htons((uint16_t)HS.port);
	if (inet_pton(AF_INET, HS.bind_addr, &sa.sin_addr) != 1) {
		fprintf(stderr, "hako: bad bind address %s\n", HS.bind_addr);
		hs_close(srv);
		return 1;
	}
	if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		fprintf(stderr, "hako: bind %s:%d failed (error %d)\n", HS.bind_addr, HS.port, hs_errno());
		hs_close(srv);
		return 1;
	}
	listen(srv, 16);

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
	if (E.ai_auto_approve)
		fprintf(stderr,
			"\n  WARNING: --yolo approves every tool call, including shell and file\n"
			"  writes, with no prompt.\n\n");

	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
	printf("hako studio v%s — http://%s:%d\n", HAKO_VERSION, HS.bind_addr, HS.port);
	printf("  dir      %s\n", cwd);
	printf("  provider %s · model %s\n",
	       hkProviderName(E.ai_provider_type), E.ai_model ? E.ai_model : "—");
	printf("  ui       %s\n", HS.web_dir ? HS.web_dir : "embedded");
	if (HS.token[0])
		printf("  open     http://%s:%d/?t=%s\n", HS.bind_addr, HS.port, HS.token);
	if (strcmp(HS.bind_addr, "127.0.0.1") != 0) hsPrintLanUrls();
	fflush(stdout);

	pthread_t acc;
	if (pthread_create(&acc, NULL, hsAcceptThread, (void *)(intptr_t)srv) != 0) {
		fprintf(stderr, "hako: cannot start listener thread\n");
		hs_close(srv);
		return 1;
	}
	pthread_detach(acc);

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
				snprintf(w, sizeof(w), "%s/%s/%s.mlf2", mdir, de->d_name, de->d_name);
				if (stat(w, &st) != 0) continue;
				const char *tag = de->d_name;
				int is_ver = (strstr(tag, "-v") != NULL);
				if (strstr(tag, "hako-koi")) {
					if (is_ver) { if (!koi[0]) strncpy(koi, tag, sizeof(koi)-1); }
					else if (!koi_s[0]) strncpy(koi_s, tag, sizeof(koi_s)-1);
				} else if (strstr(tag, "hako-sho")) {
					if (is_ver) { if (!sho[0]) strncpy(sho, tag, sizeof(sho)-1); }
					else if (!sho_s[0]) strncpy(sho_s, tag, sizeof(sho_s)-1);
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
				const char *R = E.color_enabled ? ANSI_RESET : "";
				const char *M = E.color_enabled ? TH_META   : "";
				printf("  %s%s detected locally, running via the hakm subprocess (no ollama).%s\n", M, lpick, R);
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
			if (is_ver) { if (!koi[0]) strncpy(koi, tag, sizeof(koi)-1); }
			else if (!koi_b[0]) strncpy(koi_b, tag, sizeof(koi_b)-1);
		} else if (strstr(tag, "hako-sho")) {
			if (is_ver) { if (!sho[0]) strncpy(sho, tag, sizeof(sho)-1); }
			else if (!sho_b[0]) strncpy(sho_b, tag, sizeof(sho_b)-1);
		}
	}
	pclose(fp);
	const char *pick = koi[0] ? koi : koi_b[0] ? koi_b : sho[0] ? sho : sho_b[0] ? sho_b : NULL;	if (!pick) return 0;
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
#endif
}

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
	hkHealLocalModel();
	clSplash();
	clBanner(data);
	if (!clDetectKoiDefault()) clFirstRunWizard(data);
	clStartupMenu(data);
	if (E.session_resumed && data->history_count == 0) {
		hkLoadHistoryTail(data, 40);
	}

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
		if (E.last_role_shown == HK_ROLE_AI) E.last_role_shown = HK_ROLE_SYSTEM;
		if (E.color_enabled && !E.pipe_mode) {
			const char *model = E.ai_model ? E.ai_model : "—";
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
		if (n == -1) { break; }
		if (n == -2) { E.interrupt = 0; continue; }
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

static void clUsage(void) {
	printf("hako-code v%s — standalone AI agent CLI\n", HAKO_VERSION);
	printf("usage: hako [options]\n");
	printf("  -p <prompt>     one-shot prompt, exit when done\n");
	printf("  --update        check GitHub for latest release, replace self if newer\n");
	printf("  --update-force  re-download latest even if same version\n");
	printf("  --anim <name>   pin animation: braille|dots|bar|pulse|bounce|ghost|arrows|blocks\n");
	printf("  --no-color      disable ANSI color/animation\n");
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
	printf("config: ~/.hakorc (ai_provider, ai_api_key, ai_model, theme, anim_style, ...)\n");
	printf("  state: ~/.hako/ (per-project state, credentials, session log)\n");
}

int main(int argc, char **argv) {
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
	const char *serve_bind = NULL, *serve_web = NULL, *serve_dir = NULL, *serve_token = NULL;
	int serve_port = 0;
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

	hkMigrateHakocToHako();
	clInitAI(&G_AI);
	hkMcpInit();

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

	/* --dir with --pipe: the front end picks the working folder, and per-project
	   state (sessions, trust, HAKO.md) follows the cwd exactly as it does for a
	   terminal user who cd'd there first. */
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
			else {
				static const char *al = "abcdefghijklmnopqrstuvwxyz0123456789";
				srand((unsigned)(hk_time_unix() ^ (long)getpid()));
				for (int k = 0; k < 24; k++) HS.token[k] = al[rand() % 36];
				HS.token[24] = '\0';
			}
		}
#endif
		rc = clServeMode(&G_AI, serve_dir);
	} else if (E.pipe_mode) {
		rc = clPipeMode(&G_AI);
	} else if (one_shot) {
		rc = clOneShot(&G_AI, one_shot);
	} else {
		printf("\x1b]0;函 hako\x07");
		fflush(stdout);
		rc = clRepl(&G_AI);
		printf("\x1b]0;\x07");
		fflush(stdout);
	}

	hkMcpShutdown();
	clCleanupAI(&G_AI);
	clCleanupConfig();
	return rc;
}
