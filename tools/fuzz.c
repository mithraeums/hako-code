/* SPDX-License-Identifier: GPL-3.0-or-later — Copyright (C) 2026 Zachary Blauser */
/* tools/fuzz.c — mutation fuzzer for the parsers that read MODEL OUTPUT and
   provider JSON: the tool-call dialects, the JSON scanners, SSE, markdown,
   HTML, todo lists, diffs. Every one of them takes text nobody controls.

   No libFuzzer (Apple's toolchain does not ship it), so this is a small
   seeded mutator run under ASan + UBSan: `make fuzz` (FUZZ_ITERS=N to change
   the budget, FUZZ_SEED=N to replay a run). A crash prints the seed and the
   iteration, and the input is written to hako-fuzz-crash.txt. */
#include "hako.c"
#undef main

static const char *SEEDS[] = {
	"ok <tool name=\"write_file\">{\"path\": \"a.py\", \"content\": \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\\n\"}</tool>",
	"{\"tool_calls\":[{\"function\":{\"name\":\"edit_file\",\"arguments\":\"{\\\"path\\\":\\\"c\\\",\\\"new\\\":\\\"yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\\\\\\\"\\\"}\"}}]}",
	"<write_file path=\"d\">zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz</write_file> # h\n```\ncode\n```\n[a](b) | c",
	"<tool_call>{\"name\": \"write_file\", \"arguments\": {\"path\": \"a.txt\", \"content\": \"x\\ny\\\\\"}}</tool_call>",
	"{\"name\": \"run_shell\", \"arguments\": {\"command\": \"ls\"}}</tool_call>",
	"<write_file path=\"a.c\">int main(void){return 0;}\n</write_file>",
	"read_file(path=\"src/x.c\")",
	"edit_file(path=\"a\", old=\"q\\\"\", new=\"\\\\n\")",
	"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"chat\\\\\"}}",
	"data: {\"choices\":[{\"delta\":{\"content\":\"hi\",\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"grep\",\"arguments\":\"{\\\"pattern\\\":\\\"x\\\"}\"}}]}}]}",
	"data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}",
	"{\"content\":[{\"type\":\"text\",\"text\":\"a\\u00e9\\ud83d\\ude00\"},{\"type\":\"tool_use\",\"name\":\"glob\",\"input\":{\"pattern\":\"*.c\"}}]}",
	"{\"data\":[{\"id\":\"claude-haiku-4-5\"},{\"id\":\"gpt-4o\"},{\"id\":\"x\"}]}",
	"# Title\n## sub\n- [ ] one\n- [x] two\n1. **bold** `code` *em* [l](http://x)\n```c\nint x;\n```\n---\n",
	"<html><head><script>x</script></head><body><p>a &amp; b &#x41; &lt;</p><br/></body></html>",
	"[{\"content\":\"one\",\"status\":\"completed\"},{\"content\":\"two\",\"status\":\"pending\"}]",
	"{\"path\":\"a.txt\",\"old_string\":\"a\\nb\",\"new_string\":\"c\\n\",\"start_line\":1,\"end_line\":3}",
	"owner/repo/skills/caveman",
	"```json\n{\"a\":1}\n```\ntext\n```\nmore\n```",
	"{\"mcpServers\":{\"fs\":{\"command\":\"npx\",\"args\":[\"-y\",\"x\"],\"env\":{\"K\":\"V\"}}}}",
	"\\\\\\\"\\\"\\u00\\u12345\\",
	"<invoke name=\"write_file\"><parameter name=\"path\">a</parameter><parameter name=\"content\">x</parameter></invoke>",
	"<tool_name=\"grep\"><invoke><parameter name=\"pattern\">x</parameter></invoke></tool_name>",
	"<invoke name=\"write_file\"><parameter name=\"path\">a</parameter><parameter name=\"content\">x</parameter></invoke>",
	"<tool_name=\"grep\"><invoke><parameter name=\"pattern\">x</parameter></invoke></tool_name>",
};
#define NSEEDS (int)(sizeof(SEEDS) / sizeof(SEEDS[0]))

static const char *DICT[] = {
	"<tool_call>", "</tool_call>", "<write_file", "</write_file>", "\"name\"", "\"arguments\"",
	"{", "}", "[", "]", "\"", "\\", "\\\"", "\\\\", "\\u", "\\ud83d", ":", ",", "(", ")", "=",
	"data: ", "\n", "\r\n", "```", "**", "`", "#", "- [ ] ", "&amp;", "&#", "<", ">", "\t",
	"\xE7\xAE\xB1", "\xF0\x9F", "\xC3", "\"content\"", "\"text\"", "\"input\"", "\"type\"",
};
#define NDICT (int)(sizeof(DICT) / sizeof(DICT[0]))

static unsigned long long rng;
static unsigned rnd(void) {
	rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
	return (unsigned)(rng >> 16);
}

/* One to eight mutations of a seed or of the last input. */
static size_t mutate(char *buf, size_t len, size_t cap) {
	int rounds = 1 + rnd() % 8;
	while (rounds--) {
		size_t at = len ? rnd() % (len + 1) : 0;
		switch (rnd() % 8) {
		case 0: if (len) buf[rnd() % len] ^= (char)(1 << (rnd() % 8)); break;
		case 1: if (len) buf[rnd() % len] = (char)(rnd() & 0xff); break;
		case 2: if (len > 1) { size_t n = 1 + rnd() % (len - at + 1 > 16 ? 16 : len - at + 1);
				if (at + n > len) n = len - at; memmove(buf + at, buf + at + n, len - at - n); len -= n; } break;
		case 3: { const char *d = DICT[rnd() % NDICT]; size_t n = strlen(d);
				if (len + n < cap) { memmove(buf + at + n, buf + at, len - at); memcpy(buf + at, d, n); len += n; } } break;
		case 4: len = at; break;
		case 5: { const char *s = SEEDS[rnd() % NSEEDS]; size_t sl = strlen(s), from = rnd() % (sl + 1), n = sl - from;
				if (len + n < cap) { memmove(buf + at + n, buf + at, len - at); memcpy(buf + at, s + from, n); len += n; } } break;
		case 6: if (len && len * 2 < cap) { memcpy(buf + len, buf, len); len *= 2; } break;
		case 7: { size_t n = 1 + rnd() % 600; char c = (char)(rnd() % 3 ? rnd() % 32 : rnd() & 0xff);
				if (len + n < cap) { memmove(buf + at + n, buf + at, len - at); memset(buf + at, c, n); len += n; } } break;
		}
	}
	buf[len] = '\0';
	return len;
}

static void on_hang(int sig) {
	(void)sig;
	static const char m[] = "fuzz: HANG (>5s on one input) — input in hako-fuzz-crash.txt\n";
	if (write(2, m, sizeof(m) - 1) < 0) {}
	_exit(1);
}

static void run_one(const char *s, size_t len) {
	static const char *keys[] = { "name", "arguments", "text", "type", "content", "path", "id", "input" };
	for (int k = 0; k < 8; k++) { free(hkExtractJsonString(s, keys[k])); free(hkExtractJsonObject(s, keys[k])); }
	(void)hkExtractJsonInt(s, "index");
	free(hkJsonUnescape(s, (int)len));
	free(hkJsonUnquote(s));
	free(hkExtractContentArray(s));
	free(hkExtractRawJsonArray(s, "content"));
	(void)hkHasToolBlock(s);
	free(hkRepairOrphanToolClose(s));
	free(hkNormalizeToolDialect(s));
	char nm[64]; free(hkParenArgsToJson(s, nm, sizeof(nm)));
	free(hkStripCodeFences(s));
	free(hkExtractBestFence(s));
	free(hkStripToolBlocks(s));
	free(hkSlimOne(s));                    /* an old write's body -> its receipt */
	free(hkVoiceText(s));
	free(hkHtmlToText(s));
	int t, d; free(hkTodoNormalize(s, &t, &d));
	free(hkDiffPreview("edit_file", s, 0));
	free(hkDiffPreview("write_file", s, 1));
	(void)hkLooksActionable(s);
	char o[128], rp[128], pa[256]; (void)hkParseRepoSpec(s, o, sizeof(o), rp, sizeof(rp), pa, sizeof(pa));
	char models[16][96]; (void)hkParseModelList(s, models, 16);
	{ char out[512]; clRenderMarkdownInline(s, out, sizeof(out)); }
	{ int n = 0; char **v = hkJsonArrayObjects(s, &n); for (int i = 0; i < n; i++) free(v[i]); free(v); }
	{ int n = 0; char **v = hkJsonStringArray(s, &n); for (int i = 0; i < n; i++) free(v[i]); free(v); }
	{ char **k = NULL, **v = NULL; int n = hkJsonObjectEntries(s, &k, &v);
	  for (int i = 0; i < n; i++) { free(k[i]); free(v[i]); } free(k); free(v); }
	{ const char *p = s; size_t l; int guard = 0; while (hkJsonArrayNext(&p, &l) && ++guard < 4096) {} }
	{ hkSseAcc a; memset(&a, 0, sizeof(a));
	  char line[4096]; const char *p = s;
	  while (*p) { const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n >= sizeof(line)) n = sizeof(line) - 1; memcpy(line, p, n); line[n] = '\0';
		free(hkSseFeed(&a, line)); p += n; if (*p == '\n') p++; else if (!e) break; }
	  free(hkSseFinish(&a)); hkSseFree(&a); }
}

int main(void) {
	long iters = getenv("FUZZ_ITERS") ? atol(getenv("FUZZ_ITERS")) : 200000;
	rng = getenv("FUZZ_SEED") ? strtoull(getenv("FUZZ_SEED"), NULL, 10) : (unsigned long long)time(NULL);
	unsigned long long seed = rng;
	if (!rng) rng = 88172645463325252ULL;
	signal(SIGALRM, on_hang);
	int devnull = open("/dev/null", O_WRONLY);
	fflush(stdout);
	int keep = dup(1);
	if (devnull >= 0) dup2(devnull, 1);             /* renderers print; keep the report readable */
	static char buf[1 << 16];
	size_t len = 0;
	for (long i = 0; i < iters; i++) {
		if (i % 16 == 0 || !len) { const char *s = SEEDS[rnd() % NSEEDS]; len = strlen(s); memcpy(buf, s, len + 1); }
		len = mutate(buf, len, sizeof(buf) - 1);
		FILE *f = fopen("hako-fuzz-crash.txt", "wb");
		if (f) { fwrite(buf, 1, len, f); fclose(f); }
		alarm(5);
		run_one(buf, len);
		alarm(0);
	}
	dup2(keep, 1);
	remove("hako-fuzz-crash.txt");
	printf("fuzz: %ld inputs, seed %llu, no sanitizer reports\n", iters, seed);
	return 0;
}
