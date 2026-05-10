/*
 * hakoCLAW — standalone AI agent CLI (binary: `hakoc`).
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
 *   .hakocrc parser
 *   init / cleanup
 *   termios raw line editor (CLI input)
 *   REPL + main
 */

/*** includes ***/
#define CLAW_VERSION "0.1.2"
#define CLAW_REPO    "mithraeums/hakoCLAW"

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
#include <sys/types.h>
#include <sys/stat.h>
#define PATH_MAX MAX_PATH
#define popen _popen
#define pclose _pclose
#define getcwd _getcwd
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define read(fd, buf, n) _read(fd, buf, n)
#define write(fd, buf, n) _write(fd, buf, n)
#ifdef _MSC_VER
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
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
/* MinGW <direct.h> exports a 1-arg mkdir that conflicts with POSIX 2-arg form. Force ours. */
#define mkdir(p, m) _mkdir(p)
/* MinGW lacks POSIX realpath; _fullpath is the closest. */
#define realpath(p, r) _fullpath((r), (p), PATH_MAX)
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

/* ANSI for CLI render. */
#define ANSI_DIM     "\x1b[2m"
#define ANSI_BOLD    "\x1b[1m"
#define ANSI_RESET   "\x1b[0m"
#define ANSI_USER    "\x1b[36m"   /* cyan */
#define ANSI_AI      "\x1b[32m"   /* green */
#define ANSI_SYS     "\x1b[2m"    /* dim */
#define ANSI_ERR     "\x1b[31m"   /* red */
#define ANSI_TOOL    "\x1b[33m"   /* yellow */
#define ANSI_MAGENTA "\x1b[35m"
#define ANSI_BLUE    "\x1b[34m"
#define ANSI_CLR_LINE "\r\x1b[K"

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
	AI_PROVIDER_OPENAI
};

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
} aiData;

struct clConfig {
	enum aiProviderType ai_provider_type;
	char *ai_api_key;
	char *ai_endpoint;
	char *ai_model;
	int ai_temperature;
	int ai_max_tokens;
	int ai_tools_enabled;
	int ai_stream;
	int ai_autowrite;

	char *session_id;
	long session_started;
	long session_last_used;
	int session_turn_count;
	int session_resumed;

	int color_enabled;     /* 1 if stdout is a tty */
	int interrupt;         /* SIGINT flag */

	char **mascot_lines;
	int mascot_count;
	char *mascot_path;
	int anim_force_style;  /* -1 = rotate, 0..N = pin */
	int debug;
};

struct clConfig E;
static aiData G_AI;

/*** anim tables ***/
typedef struct {
	const char *name;
	const char **frames;
	int frame_count;
	int delay_ms;
	const char *color;
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
	{"braille", FRM_BRAILLE, 10, 80,  ANSI_TOOL},
	{"dots",    FRM_DOTS,    6,  140, ANSI_AI},
	{"bar",     FRM_BAR,     14, 70,  ANSI_MAGENTA},
	{"pulse",   FRM_PULSE,   10, 100, ANSI_BLUE},
	{"bounce",  FRM_BOUNCE,  4,  150, ANSI_USER},
	{"ghost",   FRM_GHOST,   6,  220, ANSI_AI},
	{"arrows",  FRM_ARROWS,  8,  90,  ANSI_TOOL},
	{"blocks",  FRM_BLOCKS,  4,  130, ANSI_MAGENTA},
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
	"sharpening the claws",
};
static const int CL_LABEL_COUNT = sizeof(CL_LABELS) / sizeof(CL_LABELS[0]);

/*** default mascot (4-line ghost, lifted from hako) ***/
static const char *CL_DEFAULT_MASCOT[] = {
	"  ▄█████▄ ",
	" ██ ███ ██",
	" █████████",
	" ▀█▀▀█▀▀█▀",
	NULL
};

/*** forward decls ***/
static void aiAddHistory(aiData *data, const char *text);
static void aiAddHistoryRole(aiData *data, const char *text, unsigned char role);
static void aiPushMessage(aiData *data, const char *role, const char *content);
static void aiPushMessageRaw(aiData *data, const char *role, const char *content_json);
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
static char *aiExtractResponse(const char *json, enum aiProviderType type);
static char *hkExecTool(const char *name, const char *input_json);
static char *hkExtractContentArray(const char *response);
static char *hkBuildToolResults(aiData *data, const char *content_array);
static int hkFnToolExecAll(aiData *data, const char *response);
static void clStartAnim(aiData *data);
static void clStopAnim(aiData *data);
static void clLoadMascot(const char *path);

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
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":\"", key);
	const char *p = strstr(src, pat);
	if (!p) return NULL;
	p += strlen(pat);
	const char *end = p;
	while (*end && !(*end == '"' && *(end - 1) != '\\')) end++;
	if (!*end) return NULL;
	int len = end - p;
	char *out = malloc(len + 1);
	memcpy(out, p, len);
	out[len] = '\0';
	return out;
}

static int hkExtractJsonInt(const char *src, const char *key) {
	if (!src) return -1;
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(src, pat);
	if (!p) return -1;
	p += strlen(pat);
	while (*p == ' ') p++;
	if (*p < '0' || *p > '9') return -1;
	return atoi(p);
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
	FILE *fp = fopen(path, "r");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz < 0) { fclose(fp); return NULL; }
	if (max_bytes > 0 && sz > max_bytes) sz = max_bytes;
	char *buf = malloc(sz + 1);
	if (!buf) { fclose(fp); return NULL; }
	fread(buf, 1, sz, fp);
	buf[sz] = '\0';
	fclose(fp);
	return buf;
}

static char *hkRunShellCapture(const char *cmd, long max_bytes) {
	char full[2048];
	snprintf(full, sizeof(full), "timeout 10 sh -c %c%s%c 2>&1", '"', cmd, '"');
	FILE *fp = popen(full, "r");
	if (!fp) return strdup("error: popen failed");
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
	pclose(fp);
	if (!out) return strdup("");
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
		int n = strlen(e->d_name);
		out = realloc(out, total + n + 2);
		memcpy(out + total, e->d_name, n);
		out[total + n] = '\n';
		total += n + 1;
	}
	closedir(d);
	if (!out) return strdup("");
	out[total] = '\0';
	return out;
}

/*** path + trust ***/
static void hkClawDirPath(char *out, size_t n) {
	const char *home = getenv("HOME");
	if (!home) { out[0] = '\0'; return; }
	snprintf(out, n, "%s/.hakoc", home);
#ifdef _WIN32
	_mkdir(out);
#else
	mkdir(out, 0755);
#endif
}

static int hkProjectDirPath(char *out, size_t n) {
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) return 0;
	snprintf(out, n, "%s/.hakoc", cwd);
	return 1;
}

static int hkProjectTrusted(void) {
	char dir[PATH_MAX];
	if (!hkProjectDirPath(dir, sizeof(dir))) return 0;
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
	char trust[PATH_MAX + 16];
	snprintf(trust, sizeof(trust), "%s/trust", dir);
	return (stat(trust, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

static int hkGrantProjectTrust(void) {
	char dir[PATH_MAX];
	if (!hkProjectDirPath(dir, sizeof(dir))) return 0;
	mkdir(dir, 0755);
	char trust[PATH_MAX + 16];
	snprintf(trust, sizeof(trust), "%s/trust", dir);
	FILE *fp = fopen(trust, "w");
	if (!fp) return 0;
	fprintf(fp, "granted=%ld\n", (long)time(NULL));
	fclose(fp);
	return 1;
}

static void hkHistoryPath(char *out, size_t n) {
	if (hkProjectTrusted()) {
		char dir[PATH_MAX];
		hkProjectDirPath(dir, sizeof(dir));
		snprintf(out, n, "%s/history", dir);
		return;
	}
	char cwd[PATH_MAX];
	if (getcwd(cwd, sizeof(cwd))) {
		char local[PATH_MAX + 16];
		snprintf(local, sizeof(local), "%s/.hakoc_history", cwd);
		struct stat st;
		if (stat(local, &st) == 0 && S_ISREG(st.st_mode)) {
			snprintf(out, n, "%s", local);
			return;
		}
	}
	char dir[512];
	hkClawDirPath(dir, sizeof(dir));
	snprintf(out, n, "%s/history", dir);
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
	default: return "none";
	}
}

static enum aiProviderType hkParseProvider(const char *s) {
	if (strcmp(s, "ollama") == 0 || strcmp(s, "local") == 0) return AI_PROVIDER_OLLAMA;
	if (strcmp(s, "anthropic") == 0 || strcmp(s, "claude") == 0) return AI_PROVIDER_ANTHROPIC;
	if (strcmp(s, "openai") == 0 || strcmp(s, "gpt") == 0 || strcmp(s, "groq") == 0) return AI_PROVIDER_OPENAI;
	if (strcmp(s, "deepseek") == 0 || strcmp(s, "mistral") == 0 || strcmp(s, "together") == 0
		|| strcmp(s, "fireworks") == 0 || strcmp(s, "openrouter") == 0
		|| strcmp(s, "xai") == 0 || strcmp(s, "grok") == 0
		|| strcmp(s, "gemini") == 0 || strcmp(s, "google") == 0
		|| strcmp(s, "cerebras") == 0
		|| strcmp(s, "custom") == 0) return AI_PROVIDER_OPENAI;
	/* future: koi (local hakoAI model) */
	if (strcmp(s, "koi") == 0) return AI_PROVIDER_OLLAMA;  /* placeholder until engine ready */
	return AI_PROVIDER_NONE;
}

static const char *hkProviderDefaultEndpoint(const char *s) {
	if (strcmp(s, "deepseek") == 0)   return "https://api.deepseek.com";
	if (strcmp(s, "mistral") == 0)    return "https://api.mistral.ai";
	if (strcmp(s, "together") == 0)   return "https://api.together.xyz";
	if (strcmp(s, "fireworks") == 0)  return "https://api.fireworks.ai/inference";
	if (strcmp(s, "openrouter") == 0) return "https://openrouter.ai/api";
	if (strcmp(s, "groq") == 0)       return "https://api.groq.com/openai";
	if (strcmp(s, "xai") == 0 || strcmp(s, "grok") == 0) return "https://api.x.ai";
	if (strcmp(s, "gemini") == 0 || strcmp(s, "google") == 0) return "https://generativelanguage.googleapis.com/v1beta/openai";
	if (strcmp(s, "cerebras") == 0)   return "https://api.cerebras.ai";
	return NULL;
}

static void hkApplyProviderAlias(const char *val) {
	enum aiProviderType t = hkParseProvider(val);
	if (t == AI_PROVIDER_NONE) return;
	E.ai_provider_type = t;
	const char *ep = hkProviderDefaultEndpoint(val);
	if (ep && !E.ai_endpoint) E.ai_endpoint = strdup(ep);
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

	/* CLAW_* always wins. */
	if ((k = getenv("CLAW_API_KEY")) && *k) { free(E.ai_api_key); E.ai_api_key = strdup(k); }
	if ((k = getenv("CLAW_PROVIDER")) && *k) hkApplyProviderAlias(k);
	if ((k = getenv("CLAW_MODEL")) && *k) { free(E.ai_model); E.ai_model = strdup(k); }
	if ((k = getenv("CLAW_ENDPOINT")) && *k) { free(E.ai_endpoint); E.ai_endpoint = strdup(k); }
}

/*** session/state ***/
static void hkWriteSessionFile(const char *path, int include_secrets) {
	FILE *fp = fopen(path, "w");
	if (!fp) return;
	fprintf(fp, "ai_provider=%s\n", hkProviderName(E.ai_provider_type));
	if (E.ai_model) fprintf(fp, "ai_model=%s\n", E.ai_model);
	if (include_secrets && E.ai_endpoint) fprintf(fp, "ai_endpoint=%s\n", E.ai_endpoint);
	if (include_secrets && E.ai_api_key)  fprintf(fp, "ai_api_key=%s\n", E.ai_api_key);
	fprintf(fp, "ai_tools_enabled=%d\n", E.ai_tools_enabled);
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
		hkWriteSessionFile(path, 1);   /* global: holds key */
	}
	char pdir[PATH_MAX];
	if (hkProjectDirPath(pdir, sizeof(pdir))) {
		char ppath[PATH_MAX + 8];
		snprintf(ppath, sizeof(ppath), "%s/state", pdir);
		struct stat st;
		if (stat(pdir, &st) == 0 && S_ISDIR(st.st_mode)) {
			hkWriteSessionFile(ppath, 0);  /* per-project: NEVER write api_key/endpoint */
		}
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
		} else if (strcmp(key, "ai_tools_enabled") == 0) {
			E.ai_tools_enabled = atoi(val) ? 1 : 0;
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
	if (hkProjectDirPath(pdir, sizeof(pdir))) {
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
	char path[512];
	hkHistoryPath(path, sizeof(path));
	if (!path[0]) return;
	FILE *fp = fopen(path, "a");
	if (!fp) return;
	int clen = content ? (int)strlen(content) : 0;
	int cap = clen * 6 + 32;
	char *esc = malloc(cap);
	if (!esc) { fclose(fp); return; }
	hkJsonEscapeInto(content ? content : "", esc, cap);
	fprintf(fp, "{\"ts\":%ld,\"sid\":\"%s\",\"role\":\"%s\",\"content\":\"%s\"}\n",
		(long)time(NULL), E.session_id ? E.session_id : "", role, esc);
	free(esc);
	fclose(fp);
}

static void hkLoadHistoryTail(aiData *data, int max_msgs) {
	char path[512];
	hkHistoryPath(path, sizeof(path));
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

	int kept = 0;
	int *keep_idx = malloc(sizeof(int) * (lcount + 1));
	for (int i = 0; i < lcount; i++) {
		char *l = lines[i];
		if (E.session_id) {
			char tag[64];
			snprintf(tag, sizeof(tag), "\"sid\":\"%s\"", E.session_id);
			if (!strstr(l, tag)) continue;
		}
		keep_idx[kept++] = i;
	}
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
	*buf = realloc(*buf, *total + hlen + body_len + tlen + 1);
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

	free(data->system_prompt);
	data->system_prompt = buf;
	return loaded;
}

/*** history (CLI render) — aiAddHistory variants print + store ***/
static void clPrintRoleLine(unsigned char role, const char *text) {
	const char *prefix, *color;
	switch (role) {
	case HK_ROLE_USER: prefix = "▌ "; color = ANSI_USER; break;
	case HK_ROLE_AI:   prefix = "▌ "; color = ANSI_AI; break;
	default:           prefix = "  "; color = ANSI_SYS; break;
	}
	if (E.color_enabled) printf("%s%s%s%s\n", color, prefix, text ? text : "", ANSI_RESET);
	else printf("%s%s\n", prefix, text ? text : "");
	fflush(stdout);
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
		if (data->messages[i].raw) {
			int need2 = len + clen + 64;
			if (need2 >= cap) { while (cap < need2) cap *= 2; out = realloc(out, cap); }
			len += snprintf(out + len, cap - len, "{\"role\":\"%s\",\"content\":", data->messages[i].role);
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
	snprintf(path, sizeof(path), "/tmp/hakoc-req-%d.json", (int)getpid());
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
		size_t need = len + strlen(t->name) + strlen(t->description) + strlen(t->props) + strlen(t->required) + 256;
		if (need >= cap) { while (cap < need) cap *= 2; out = realloc(out, cap); }
		const char *req_close_end = (*t->required) ? "]" : "";
		if (provider_format == 0) {
			len += snprintf(out + len, cap - len,
				"{\"name\":\"%s\",\"description\":\"%s\",\"input_schema\":{\"type\":\"object\",\"properties\":{%s}%s%s%s}}",
				t->name, t->description, t->props,
				(*t->required) ? ",\"required\":[" : "",
				t->required, req_close_end);
		} else {
			len += snprintf(out + len, cap - len,
				"{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":\"%s\",\"parameters\":{\"type\":\"object\",\"properties\":{%s}%s%s%s}}}",
				t->name, t->description, t->props,
				(*t->required) ? ",\"required\":[" : "",
				t->required, req_close_end);
		}
	}
	if (len + 2 >= cap) { cap += 4; out = realloc(out, cap); }
	out[len++] = ']';
	out[len] = '\0';
	return out;
}

static char *hkExecTool(const char *name, const char *input_json) {
	if (strcmp(name, "read_file") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted — ask the user to run /trust before any file access");
		char *path = hkExtractJsonString(input_json, "path");
		if (!path) return strdup("error: missing path");
		char full[PATH_MAX];
		if (hkResolveInProject(path, full, sizeof(full)) != 0) {
			free(path);
			return strdup("error: path outside project");
		}
		free(path);
		char *c = hkReadFileAll(full, 100000);
		return c ? c : strdup("error: cannot read");
	}
	if (strcmp(name, "list_dir") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted — ask the user to run /trust before any file access");
		char *path = hkExtractJsonString(input_json, "path");
		if (!path) return strdup("error: missing path");
		char full[PATH_MAX];
		if (hkResolveInProject(path, full, sizeof(full)) != 0) {
			free(path);
			return strdup("error: path outside project");
		}
		free(path);
		char *c = hkListDir(full);
		return c ? c : strdup("error: cannot list");
	}
	if (strcmp(name, "run_shell") == 0) {
		if (!hkProjectTrusted()) return strdup("error: project not trusted");
		char *shcmd = hkExtractJsonString(input_json, "cmd");
		if (!shcmd) return strdup("error: missing cmd");
		char *c = hkRunShellCapture(shcmd, 50000);
		free(shcmd);
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
		char *content = hkExtractJsonString(input_json, "content");
		if (!path || !content) {
			free(path); free(content);
			return strdup("error: missing path or content");
		}
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
		FILE *ef = fopen(full, "r");
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
			snprintf(pending, sizeof(pending), "%s.hakoc-pending", full);
			FILE *fp = fopen(pending, "w");
			if (!fp) { free(path); free(content); return strdup("error: cannot stage pending"); }
			fwrite(content, 1, clen, fp);
			fclose(fp);
			char *out = malloc(512);
			snprintf(out, 512, "preview staged at %s (new %zu bytes/%d lines, old %ld/%d). ai_autowrite=0; user must `mv` pending to apply.",
				pending, clen, new_lines, old_size, old_lines);
			free(path); free(content);
			return out;
		}
		FILE *fp = fopen(full, "w");
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

static void hkAnnounceTool(aiData *data, const char *fname, const char *args_obj) {
	(void)data;
	char arg_summary[128] = "";
	char *path = hkExtractJsonString(args_obj, "path");
	char *cmd = hkExtractJsonString(args_obj, "cmd");
	if (path) { snprintf(arg_summary, sizeof(arg_summary), "%s", path); free(path); }
	else if (cmd) { snprintf(arg_summary, sizeof(arg_summary), "%.80s", cmd); free(cmd); }
	if (E.color_enabled) printf("%s→ %s(%s)%s\n", ANSI_TOOL, fname, arg_summary, ANSI_RESET);
	else printf("→ %s(%s)\n", fname, arg_summary);
	fflush(stdout);
}

static void hkAnnounceToolResult(aiData *data, const char *result) {
	(void)data;
	int len = result ? (int)strlen(result) : 0;
	int is_err = result && strncmp(result, "error:", 6) == 0;
	if (is_err) {
		if (E.color_enabled) printf("  %s%s%s\n", ANSI_ERR, result, ANSI_RESET);
		else printf("  %s\n", result);
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

	int bodycap = strlen(msgs) + 4096;
	char *body = malloc(bodycap);
	if (!body) { free(msgs); return NULL; }

	const char *sys = (data->system_prompt && *data->system_prompt) ? data->system_prompt : "";
	char *sys_esc = NULL;
	if (*sys) {
		int slen = strlen(sys);
		sys_esc = malloc(slen * 6 + 8);
		hkJsonEscapeInto(sys, sys_esc, slen * 6 + 8);
	}

	int tools_on = E.ai_tools_enabled;
	char *anth_tools = NULL, *fn_tools = NULL;
	if (tools_on) {
		anth_tools = hkBuildToolsSchema(0);
		fn_tools = hkBuildToolsSchema(1);
	}

	switch (type) {
	case AI_PROVIDER_OLLAMA: {
		if (!endpoint) endpoint = "http://localhost:11434";
		if (!model) model = "llama3.2";
		int tlen = tools_on ? strlen(fn_tools) + 16 : 0;
		int need = bodycap + tlen + 96;
		if (need > bodycap) { body = realloc(body, need); bodycap = need; }
		if (tools_on) {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"messages\":%s,\"stream\":false,\"tools\":%s}",
				model, msgs, fn_tools);
		} else {
			snprintf(body, bodycap,
				"{\"model\":\"%s\",\"messages\":%s,\"stream\":false}",
				model, msgs);
		}
		break;
	}
	case AI_PROVIDER_ANTHROPIC: {
		if (!endpoint) endpoint = "https://api.anthropic.com";
		if (!model) model = "claude-sonnet-4-20250514";
		int anth_on = tools_on && hkProjectTrusted();
		int stream_on = E.ai_stream && !anth_on;
		int tlen = anth_on ? strlen(anth_tools) + 16 : 0;
		int need = bodycap + tlen + 96;
		if (need > bodycap) { body = realloc(body, need); bodycap = need; }
		const char *stream_field = stream_on ? ",\"stream\":true" : "";
		if (*sys) {
			if (anth_on) {
				snprintf(body, bodycap,
					"{\"model\":\"%s\",\"max_tokens\":%d,\"system\":\"%s\",\"messages\":%s,\"tools\":%s%s}",
					model, max_tokens, sys_esc, msgs, anth_tools, stream_field);
			} else {
				snprintf(body, bodycap,
					"{\"model\":\"%s\",\"max_tokens\":%d,\"system\":\"%s\",\"messages\":%s%s}",
					model, max_tokens, sys_esc, msgs, stream_field);
			}
		} else {
			if (anth_on) {
				snprintf(body, bodycap,
					"{\"model\":\"%s\",\"max_tokens\":%d,\"messages\":%s,\"tools\":%s%s}",
					model, max_tokens, msgs, anth_tools, stream_field);
			} else {
				snprintf(body, bodycap,
					"{\"model\":\"%s\",\"max_tokens\":%d,\"messages\":%s%s}",
					model, max_tokens, msgs, stream_field);
			}
		}
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
	case AI_PROVIDER_OLLAMA:
		snprintf(cmd, 4096,
			"curl -s -X POST %s/api/chat -H 'Content-Type: application/json' --data @%s 2>/dev/null",
			endpoint, reqfile);
		break;
	case AI_PROVIDER_ANTHROPIC:
		if (!api_key) { free(cmd); free(reqfile); return NULL; }
		snprintf(cmd, 4096,
			(E.ai_stream && !E.ai_tools_enabled)
				? "curl -sN -X POST %s/v1/messages -H 'Content-Type: application/json' -H 'x-api-key: %s' -H 'anthropic-version: 2023-06-01' -H 'accept: text/event-stream' --data @%s 2>/dev/null"
				: "curl -s -X POST %s/v1/messages -H 'Content-Type: application/json' -H 'x-api-key: %s' -H 'anthropic-version: 2023-06-01' --data @%s 2>/dev/null",
			endpoint, api_key, reqfile);
		break;
	case AI_PROVIDER_OPENAI:
		if (!api_key) { free(cmd); free(reqfile); return NULL; }
		snprintf(cmd, 4096,
			"curl -s -X POST %s/v1/chat/completions -H 'Content-Type: application/json' -H 'Authorization: Bearer %s' --data @%s 2>/dev/null",
			endpoint, api_key, reqfile);
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
		if (len >= cap - 4) { cap *= 2; result = realloc(result, cap); }
		if (*p == '\\' && *(p + 1)) {
			p++;
			switch (*p) {
			case 'n': result[len++] = '\n'; break;
			case 't': result[len++] = '\t'; break;
			case '"': result[len++] = '"'; break;
			case '\\': result[len++] = '\\'; break;
			case '/': result[len++] = '/'; break;
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

static int hkFnToolExecAll(aiData *data, const char *response) {
	const char *p = strstr(response, "\"tool_calls\":[");
	if (!p) return 0;
	int count = 0;
	while ((p = strstr(p, "\"function\""))) {
		char *fname = hkExtractJsonString(p, "name");
		char *args_obj = hkExtractJsonObject(p, "arguments");
		if (!args_obj) {
			char *args_str = hkExtractJsonString(p, "arguments");
			if (args_str) {
				args_obj = hkJsonUnescape(args_str, (int)strlen(args_str));
				free(args_str);
			}
		}
		if (!fname || !args_obj) { free(fname); free(args_obj); p++; continue; }
		hkAnnounceTool(data, fname, args_obj);
		char *result = hkExecTool(fname, args_obj);
		hkAnnounceToolResult(data, result);
		pthread_mutex_lock(&data->lock);
		aiPushMessage(data, "tool", result ? result : "");
		pthread_mutex_unlock(&data->lock);
		free(fname); free(args_obj); free(result);
		count++;
		p++;
	}
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
			printf(ANSI_CLR_LINE "%s%s %s...%s", a->color, fr, label, ANSI_RESET);
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

/*** mascot ***/
static void clFreeMascot(void) {
	if (E.mascot_lines) {
		for (int i = 0; i < E.mascot_count; i++) free(E.mascot_lines[i]);
		free(E.mascot_lines);
		E.mascot_lines = NULL;
		E.mascot_count = 0;
	}
}

static void clSetDefaultMascot(void) {
	clFreeMascot();
	int n = 0;
	while (CL_DEFAULT_MASCOT[n]) n++;
	E.mascot_lines = malloc(sizeof(char*) * n);
	for (int i = 0; i < n; i++) E.mascot_lines[i] = strdup(CL_DEFAULT_MASCOT[i]);
	E.mascot_count = n;
}

static void clLoadMascot(const char *path) {
	if (!path || !*path) { clSetDefaultMascot(); return; }
	FILE *fp = fopen(path, "r");
	if (!fp) { clSetDefaultMascot(); return; }
	clFreeMascot();
	char *line = NULL;
	size_t cap = 0;
	int count = 0, lcap = 0;
	while (getline(&line, &cap, fp) != -1) {
		char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
		if (count >= lcap) { lcap = lcap ? lcap * 2 : 8; E.mascot_lines = realloc(E.mascot_lines, sizeof(char*) * lcap); }
		E.mascot_lines[count++] = strdup(line);
	}
	free(line);
	fclose(fp);
	E.mascot_count = count;
	if (count == 0) clSetDefaultMascot();
}

static void clPrintMascot(void) {
	if (!E.mascot_lines || E.mascot_count == 0) return;
	for (int i = 0; i < E.mascot_count; i++) {
		if (E.color_enabled) printf("  %s%s%s\n", ANSI_AI, E.mascot_lines[i], ANSI_RESET);
		else printf("  %s\n", E.mascot_lines[i]);
	}
}

/*** worker thread ***/
static void *aiWorkerThread(void *arg) {
	aiData *data = (aiData *)arg;

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
			} else if (!E.ai_api_key && E.ai_provider_type != AI_PROVIDER_OLLAMA) {
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

		int streaming_mode = (E.ai_provider_type == AI_PROVIDER_ANTHROPIC
			&& E.ai_stream && !E.ai_tools_enabled);

		char buffer[8192];
		char *full_response = NULL;
		size_t total = 0;

		if (streaming_mode) {
			char *acc = calloc(1, 1);
			size_t acc_len = 0;
			int printed_prefix = 0;

			while (fgets(buffer, sizeof(buffer), fp)) {
				int blen = strlen(buffer);
				full_response = realloc(full_response, total + blen + 1);
				memcpy(full_response + total, buffer, blen);
				total += blen;

				if (strncmp(buffer, "data: ", 6) != 0) continue;
				const char *payload = buffer + 6;
				if (!strstr(payload, "content_block_delta")) continue;
				char *text = hkExtractJsonString(payload, "text");
				if (!text) continue;

				size_t tlen = strlen(text);
				acc = realloc(acc, acc_len + tlen + 1);
				memcpy(acc + acc_len, text, tlen);
				acc_len += tlen;
				acc[acc_len] = '\0';

				/* live print */
				if (!printed_prefix) {
					clStopAnim(data);
					if (E.color_enabled) printf("%s▌ ", ANSI_AI);
					else printf("▌ ");
					printed_prefix = 1;
				}
				fwrite(text, 1, tlen, stdout);
				fflush(stdout);
				free(text);
			}
			pclose(fp);
			clStopAnim(data);
			if (full_response) full_response[total] = '\0';
			if (printed_prefix) {
				if (E.color_enabled) printf("%s\n", ANSI_RESET);
				else printf("\n");
				fflush(stdout);
			}

			if (acc_len > 0) {
				pthread_mutex_lock(&data->lock);
				hkUpdateUsage(data, full_response);
				/* store for /clear etc, but don't reprint */
				aiPushHistoryStore(data, acc, HK_ROLE_AI);
				aiPushMessage(data, "assistant", acc);
				hkLogMessage("assistant", acc);
				free(data->current_response);
				data->current_response = acc;
				pthread_mutex_unlock(&data->lock);
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
			pthread_mutex_lock(&data->lock);
			aiPushMessage(data, "assistant", content && *content ? content : "(calling tools)");
			pthread_mutex_unlock(&data->lock);

			int n = hkFnToolExecAll(data, full_response);

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
			char err[256];
			if (full_response) {
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
	data->streaming = 0;
	pthread_mutex_unlock(&data->lock);

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
	if (prompt[0] != '/') return 0;
	const char *cmd = prompt + 1;
	const char *arg = strchr(cmd, ' ');
	int cmdlen = arg ? (arg - cmd) : (int)strlen(cmd);
	if (arg) { while (*arg == ' ') arg++; }

	if (strncmp(cmd, "help", cmdlen) == 0 && cmdlen == 4) {
		aiAddHistory(data, "/clear  /help  /model <id>  /provider <name>  /login [<provider>]");
		aiAddHistory(data, "/history [local|global]  /skills [reload]");
		aiAddHistory(data, "/skill install <url>  /skill uninstall <name>");
		aiAddHistory(data, "/tools on|off  /trust [revoke]  /usage  /quit");
		aiAddHistory(data, "/sessions  /resume <id>  /session [new]");
		return 1;
	}
	if (strncmp(cmd, "login", cmdlen) == 0 && cmdlen == 5) {
		const char *prov = (arg && *arg) ? arg : hkProviderName(E.ai_provider_type);
		if (!prov || strcmp(prov, "none") == 0) {
			aiAddHistory(data, "usage: /login <provider>  (anthropic, openai, ollama, groq, deepseek, mistral, together, fireworks, openrouter, xai, custom)");
			return 1;
		}
		if (strcmp(prov, "ollama") == 0 || strcmp(prov, "local") == 0 || strcmp(prov, "koi") == 0) {
			hkApplyProviderAlias(prov);
			hkSaveSession();
			aiAddHistory(data, "ollama is local — no API key needed.");
			aiAddHistory(data, "ensure `ollama serve` is running, then /model <id> (e.g. llama3.2)");
			return 1;
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
			aiAddHistory(data, "custom provider — set ai_endpoint via /provider or .hakocrc");
		}
		printf("  paste API key (input hidden): ");
		fflush(stdout);
		char key[1024];
		clReadHidden(key, sizeof(key));
		if (!key[0]) { aiAddHistory(data, "(empty key, not saved)"); return 1; }
		free(E.ai_api_key);
		E.ai_api_key = strdup(key);
		hkApplyProviderAlias(prov);
		hkSaveSession();
		char msg[128];
		snprintf(msg, sizeof(msg), "key saved for %s (~/.hakoc/state, mode 0600)", hkProviderName(E.ai_provider_type));
		aiAddHistory(data, msg);
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
	if (strncmp(cmd, "provider", cmdlen) == 0 && cmdlen == 8) {
		if (arg && *arg) {
			enum aiProviderType t = hkParseProvider(arg);
			if (t == AI_PROVIDER_NONE) {
				aiAddHistory(data, "unknown. valid: ollama, anthropic, openai, gemini/google,");
				aiAddHistory(data, "  groq, cerebras, deepseek, mistral, together, fireworks,");
				aiAddHistory(data, "  openrouter, xai/grok, custom");
			} else {
				hkApplyProviderAlias(arg);
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
		if (arg && strcmp(arg, "local") == 0) {
			char cwd[PATH_MAX];
			if (!getcwd(cwd, sizeof(cwd))) { aiAddHistory(data, "cwd error"); return 1; }
			char local[PATH_MAX + 16];
			snprintf(local, sizeof(local), "%s/.hakoc_history", cwd);
			struct stat st;
			if (stat(local, &st) != 0) {
				FILE *fp = fopen(local, "w");
				if (!fp) { aiAddHistory(data, "cannot create .hakoc_history"); return 1; }
				fclose(fp);
				aiAddHistory(data, "created .hakoc_history in cwd");
			} else {
				aiAddHistory(data, ".hakoc_history already exists");
			}
			char msg[PATH_MAX + 32];
			snprintf(msg, sizeof(msg), "using: %s", local);
			aiAddHistory(data, msg);
			return 1;
		}
		if (arg && strcmp(arg, "global") == 0) {
			char cwd[PATH_MAX];
			if (getcwd(cwd, sizeof(cwd))) {
				char local[PATH_MAX + 16];
				snprintf(local, sizeof(local), "%s/.hakoc_history", cwd);
				if (unlink(local) == 0) aiAddHistory(data, "removed local .hakoc_history");
				else aiAddHistory(data, "no local .hakoc_history to remove");
			}
			char p[512];
			hkHistoryPath(p, sizeof(p));
			char msg[600];
			snprintf(msg, sizeof(msg), "using: %s", p);
			aiAddHistory(data, msg);
			return 1;
		}
		char p[512];
		hkHistoryPath(p, sizeof(p));
		aiAddHistory(data, p);
		aiAddHistory(data, "(use /history local | /history global)");
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
		if (!d) { aiAddHistory(data, "no skills dir (~/.hakoc/skills)"); return 1; }
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
	if (strncmp(cmd, "trust", cmdlen) == 0 && cmdlen == 5) {
		if (arg && strcmp(arg, "revoke") == 0) {
			char dir[PATH_MAX];
			hkProjectDirPath(dir, sizeof(dir));
			char trust[PATH_MAX + 16];
			snprintf(trust, sizeof(trust), "%s/trust", dir);
			if (unlink(trust) == 0) aiAddHistory(data, "trust revoked");
			else aiAddHistory(data, "no trust to revoke");
			return 1;
		}
		if (hkProjectTrusted()) {
			aiAddHistory(data, "already trusted");
		} else if (hkGrantProjectTrust()) {
			aiAddHistory(data, "trusted. hakoCLAW may edit files.");
		} else {
			aiAddHistory(data, "could not grant trust");
		}
		return 1;
	}
	if ((strncmp(cmd, "quit", cmdlen) == 0 && cmdlen == 4) ||
		(strncmp(cmd, "exit", cmdlen) == 0 && cmdlen == 4)) {
		return 2;
	}
	if (strncmp(cmd, "usage", cmdlen) == 0 && cmdlen == 5) {
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

/*** .hakocrc parser ***/
static void clLoadRc(void) {
	const char *home = getenv("HOME");
	if (!home) return;
	char path[512];
	snprintf(path, sizeof(path), "%s/.hakocrc", home);
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
		else if (strcmp(key, "mascot_path") == 0) {
			free(E.mascot_path);
			E.mascot_path = strdup(val);
			clLoadMascot(val);
		} else if (strcmp(key, "anim_style") == 0) {
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
	E.ai_stream = 1;
	E.ai_autowrite = 1;
	E.anim_force_style = -1;
#ifndef _WIN32
	E.color_enabled = isatty(STDOUT_FILENO) ? 1 : 0;
#endif
	clSetDefaultMascot();
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
	free(E.mascot_path);
	clFreeMascot();
}

/*** signal ***/
static void clSigint(int sig) {
	(void)sig;
	E.interrupt = 1;
}

/*** termios raw line editor ***/
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
	snprintf(out, cap, "%s/.hakoc/input_history", home);
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

static void clRedrawLine(const char *prompt, const char *buf, int len, int cursor) {
	int plen = clVisibleLen(prompt);
	int cols = clTermCols();
	if (cols < 1) cols = 80;

	int rows = (plen + len + cols - 1) / cols;
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

	/* If cursor at end and content lands exactly at a column-edge, emit \n\r so terminal scrolls a fresh row. */
	int rows_after = rows;
	if (cursor == len && len > 0 && (plen + len) % cols == 0) {
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

	if (write(STDOUT_FILENO, ab, n) < 0) return;
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
			/* Insert literal newline during paste (rendered as space — claw treats prompts as single-line). */
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
	return "hakoCLAW-macos-universal.tar.gz";
#elif defined(__linux__)
  #if defined(__aarch64__) || defined(__arm64__)
	return "hakoCLAW-linux-arm64.tar.gz";
  #else
	return "hakoCLAW-linux-x86_64.tar.gz";
  #endif
#elif defined(__FreeBSD__)
	return "hakoCLAW-freebsd-x86_64.tar.gz";
#elif defined(_WIN32)
	return "hakoCLAW-windows-x86_64.zip";
#else
	return NULL;
#endif
}

static const char *clPlatformDir(void) {
#if defined(__APPLE__)
	return "hakoCLAW-macos-universal";
#elif defined(__linux__)
  #if defined(__aarch64__) || defined(__arm64__)
	return "hakoCLAW-linux-arm64";
  #else
	return "hakoCLAW-linux-x86_64";
  #endif
#elif defined(__FreeBSD__)
	return "hakoCLAW-freebsd-x86_64";
#elif defined(_WIN32)
	return "hakoCLAW-windows-x86_64";
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
			"curl -fsSL https://api.github.com/repos/%s/releases/latest 2>/dev/null", CLAW_REPO);
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
	snprintf(cur_tag, sizeof(cur_tag), "v%s", CLAW_VERSION);
	printf("latest: %s · current: %s\n", tag, cur_tag);
	if (!force && strcmp(tag, cur_tag) == 0) {
		printf("already up to date.\n");
		return 0;
	}

	/* tmp dir */
	char tmp_dir[256];
	snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/hakoc-update-%ld", (long)time(NULL));
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
			tmp_archive, CLAW_REPO, tag, asset);
		if (system(cmd) != 0) { fprintf(stderr, "update: download failed\n"); return 1; }

		snprintf(cmd, sizeof(cmd),
			"curl -fsSL -o '%s' 'https://github.com/%s/releases/download/%s/%s'",
			tmp_sha, CLAW_REPO, tag, sha_name);
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
	snprintf(new_bin, sizeof(new_bin), "%s/%s/hakoc.exe", tmp_dir, pdir);
#else
	snprintf(new_bin, sizeof(new_bin), "%s/%s/hakoc", tmp_dir, pdir);
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
static void clBanner(aiData *data) {
	clPrintMascot();
	if (E.color_enabled) {
		printf("  %shakoCLAW v%s%s  %sprovider:%s %s  %smodel:%s %s  %strust:%s %s\n",
			ANSI_BOLD, CLAW_VERSION, ANSI_RESET,
			ANSI_DIM, ANSI_RESET, hkProviderName(E.ai_provider_type),
			ANSI_DIM, ANSI_RESET, E.ai_model ? E.ai_model : "(unset)",
			ANSI_DIM, ANSI_RESET, hkProjectTrusted() ? "granted" : "off");
	} else {
		printf("  hakoCLAW v%s — provider: %s, model: %s, trust: %s\n",
			CLAW_VERSION,
			hkProviderName(E.ai_provider_type),
			E.ai_model ? E.ai_model : "(unset)",
			hkProjectTrusted() ? "granted" : "off");
	}
	if (E.session_resumed) {
		long ago = (long)time(NULL) - E.session_last_used;
		const char *unit; long n;
		if (ago < 3600) { n = ago / 60; unit = "m"; }
		else if (ago < 86400) { n = ago / 3600; unit = "h"; }
		else { n = ago / 86400; unit = "d"; }
		printf("  %ssession:%s resumed (%ld%s, %d turns)\n",
			E.color_enabled ? ANSI_DIM : "", E.color_enabled ? ANSI_RESET : "",
			n, unit, E.session_turn_count);
	} else {
		printf("  %ssession:%s new (%s)\n",
			E.color_enabled ? ANSI_DIM : "", E.color_enabled ? ANSI_RESET : "",
			E.session_id ? E.session_id : "?");
	}
	if (E.ai_provider_type == AI_PROVIDER_NONE) {
		printf("  (set ai_provider in ~/.hakocrc, or use /provider <name>)\n");
	}
	int sk = hkLoadSkills(data);
	if (sk > 0) printf("  loaded %d skill(s)\n", sk);
	printf("  %s/help for commands · Ctrl-C cancels stream · /quit to exit%s\n",
		E.color_enabled ? ANSI_DIM : "", E.color_enabled ? ANSI_RESET : "");
	if (E.color_enabled) printf("  %s───────────────────────────────────────────────%s\n", ANSI_DIM, ANSI_RESET);
	else printf("---\n");
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
	char path[512];
	hkHistoryPath(path, sizeof(path));
	FILE *fp = fopen(path, "r");
	if (!fp) return 0;
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
		for (int i = 0; i < n; i++) if (strcmp(out[i].id, id) == 0) { idx = i; break; }
		if (idx < 0) {
			if (n >= max) continue;
			idx = n++;
			snprintf(out[idx].id, sizeof(out[idx].id), "%s", id);
			out[idx].first[0] = '\0';
			out[idx].count = 0;
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
		out[idx].count++;
		out[idx].last = ts;
	}
	free(line);
	fclose(fp);
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
			printf("  cwd: %s%s%s\n",
				E.color_enabled ? ANSI_DIM : "", cwd, E.color_enabled ? ANSI_RESET : "");
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
		fprintf(stderr, "error: no provider configured (~/.hakocrc or /provider)\n");
		return 1;
	}
	aiWorkerSend(data);
	clWaitWorker(data);
	return 0;
}

static int clRepl(aiData *data) {
	clBanner(data);
	clStartupMenu(data);
	if (E.session_resumed && data->history_count == 0) {
		hkLoadHistoryTail(data, 40);
	}

	char line[4096];
	const char *prompt_color = "\x1b[1m> \x1b[0m";
	const char *prompt_plain = "> ";

	while (1) {
		const char *prompt = E.color_enabled ? prompt_color : prompt_plain;
		int n = clReadLineRaw(prompt, line, sizeof(line));
		if (n == -1) { break; }                    /* EOF */
		if (n == -2) { E.interrupt = 0; continue; }/* Ctrl-C cancel */
		if (n == 0) continue;

		if (line[0] == '/') {
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
			aiAddHistory(data, "Set ai_provider in ~/.hakocrc or /provider <name>");
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
	printf("hakoCLAW v%s — standalone AI agent CLI\n", CLAW_VERSION);
	printf("usage: hakoc [options]\n");
	printf("  -p <prompt>     one-shot prompt, exit when done\n");
	printf("  --update        check GitHub for latest release, replace self if newer\n");
	printf("  --update-force  re-download latest even if same version\n");
	printf("  --mascot <path> load custom ASCII mascot from file\n");
	printf("  --anim <name>   pin animation: braille|dots|bar|pulse|bounce|ghost|arrows|blocks\n");
	printf("  --no-color      disable ANSI color/animation\n");
	printf("  --debug         dump raw API responses to stderr\n");
	printf("  -h --help       this help\n");
	printf("  -v --version    print version\n");
	printf("config: ~/.hakocrc (ai_provider, ai_api_key, ai_model, mascot_path, anim_style, ...)\n");
}

int main(int argc, char **argv) {
	clInitConfig();
	clLoadRc();
	hkLoadSession();
	clApplyEnv();

	const char *one_shot = NULL;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			clUsage(); return 0;
		}
		if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
			printf("hakoCLAW v%s\n", CLAW_VERSION); return 0;
		}
		if (strcmp(a, "--update") == 0)       { return clCmdUpdate(0); }
		if (strcmp(a, "--update-force") == 0) { return clCmdUpdate(1); }
		if (strcmp(a, "-p") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "-p needs argument\n"); return 2; }
			one_shot = argv[++i]; continue;
		}
		if (strcmp(a, "--mascot") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "--mascot needs path\n"); return 2; }
			free(E.mascot_path); E.mascot_path = strdup(argv[++i]);
			clLoadMascot(E.mascot_path);
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
		if (strcmp(a, "--debug") == 0) { E.debug = 1; continue; }
		fprintf(stderr, "unknown arg: %s (try --help)\n", a);
		return 2;
	}

#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, clSigint);
#endif

	clInitAI(&G_AI);
	int rc;
	if (one_shot) {
		rc = clOneShot(&G_AI, one_shot);
	} else {
		rc = clRepl(&G_AI);
	}

	clCleanupAI(&G_AI);
	clCleanupConfig();
	return rc;
}
