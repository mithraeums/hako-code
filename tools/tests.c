/* SPDX-License-Identifier: GPL-3.0-or-later — Copyright (C) 2026 Zachary Blauser */
/* tools/tests.c — unit tests for the pieces of hako with real platform surface:
   the grep/glob matcher, the directory walker, URL validation, the HTML
   stripper, the diff preview and the shell timeout.
 *
 * Build and run with `make test`. It includes hako.c with the entry
 * point renamed, so the helpers are called directly with no model, no network
 * and no trust file.
 *
 * The point of it is portability as much as correctness: this is the piece of
 * v0.2.4 with the most platform surface (readdir, musl vs glibc, 32-bit size_t)
 * and it is meant to be run on every target — including iSh, where the agent
 * has to survive an emulated 32-bit i386 userland.
 */
#include "hako.c"
#undef main

static int fails = 0;
static void ck(int cond, const char *what) {
	if (!cond) { printf("FAIL %s\n", what); fails++; }
}
static void ckstr(const char *hay, const char *needle, const char *what) {
	if (!hay || !strstr(hay, needle)) { printf("FAIL %s (got: %.200s)\n", what, hay ? hay : "(null)"); fails++; }
}
static void cknot(const char *hay, const char *needle, const char *what) {
	if (hay && strstr(hay, needle)) { printf("FAIL %s\n", what); fails++; }
}


/* --- the same tools through hkExecTool: aliases, gates, wire schema --- */
static void dispatch_tests(void) {
	char cmd[512];
	char *r;

	char tmpl2[]="/tmp/hakodispXXXXXX"; char *root=mkdtemp(tmpl2);
	 snprintf(cmd,sizeof(cmd),"printf 'int zeta(void);\\n' > %s/z.c",root);
	if(system(cmd)){} if(chdir(root)){}

	/* Non-tty with no auto-approve: the approval gate denies before the tool
	   is ever reached. This is the existing order, shared with read_file. */
	r=hkExecTool("grep","{\"pattern\":\"zeta\"}");
	ckstr(r,"denied","grep denied when it cannot ask"); free(r);

	/* Past the gate, project trust must still bite before anything walks disk. */
	E.ai_auto_approve=1;
	r=hkExecTool("grep","{\"pattern\":\"zeta\"}");
	ckstr(r,"not trusted","grep refused without project trust"); free(r);
	r=hkExecTool("glob","{\"pattern\":\"*.c\"}");
	ckstr(r,"not trusted","glob refused without project trust"); free(r);

	hkGrantProjectTrust();
	hkRepeatReset();   /* the repeat guard counts a window, and the two refusal
	                      probes above used the same signature as the calls below */

	r=hkExecTool("grep","{\"pattern\":\"zeta\"}");      ckstr(r,"z.c:1:","grep via hkExecTool"); free(r);
	r=hkExecTool("search","{\"pattern\":\"zeta\"}");    ckstr(r,"z.c:1:","alias search->grep"); free(r);
	r=hkExecTool("rg","{\"query\":\"zeta\"}");          ckstr(r,"z.c:1:","alias rg + param query"); free(r);
	r=hkExecTool("glob","{\"pattern\":\"*.c\"}");       ckstr(r,"z.c","glob via hkExecTool"); free(r);
	r=hkExecTool("find_files","{\"pattern\":\"*.c\"}"); ckstr(r,"z.c","alias find_files->glob"); free(r);
	r=hkExecTool("web_fetch","{\"url\":\"ftp://x/y\"}");ckstr(r,"http://","web_fetch rejects scheme"); free(r);
	r=hkExecTool("fetch","{\"url\":\"ftp://x/y\"}");    ckstr(r,"http://","alias fetch->web_fetch"); free(r);

	/* The guard has to survive ALTERNATION: a model bouncing between two
	   useless calls used to reset the counter on every switch and loop until
	   the tool cap. */
	hkRepeatReset();
	for (int i = 0; i < 3; i++) {
		r = hkExecTool("read_file", "{\"path\":\"z.c\"}");  free(r);
		r = hkExecTool("list_dir", "{\"path\":\".\"}");      free(r);
	}
	r = hkExecTool("read_file", "{\"path\":\"z.c\"}");
	ckstr(r, "already run", "repeat guard catches alternating calls"); free(r);
	hkRepeatReset();
	r = hkExecTool("read_file", "{\"path\":\".\"}");
	ckstr(r, "is a directory", "read_file on a directory is an error, not 0 bytes"); free(r);

	/* both wire formats must carry the three new tools */
	for(int fmt=0;fmt<2;fmt++){
		char *sc=hkBuildToolsSchema(fmt);
		ckstr(sc,"\"grep\"","schema has grep"); ckstr(sc,"\"glob\"","schema has glob");
		ckstr(sc,"\"web_fetch\"","schema has web_fetch");
		cknot(sc,"\"\"","schema has no empty name");
		free(sc);
	}
	if(chdir("/tmp")==0){ snprintf(cmd,sizeof(cmd),"rm -rf %s",root); if(system(cmd)){} }
	}

/* --- diff preview --- */
static void diff_tests(void) {
	char tmpl[] = "/tmp/hakodiffXXXXXX";
	char *root = mkdtemp(tmpl);
	char cmd[512];
	if (!root) { printf("FAIL diff mkdtemp\n"); fails++; return; }
	snprintf(cmd, sizeof(cmd),
		"printf 'one\\ntwo\\nthree\\nfour\\n' > %s/f.txt", root);
	if (system(cmd)) {}
	if (chdir(root)) {}

	/* whole-file write: unchanged head and tail must be folded away */
	char *d = hkDiffPreview("write_file",
		"{\"path\":\"f.txt\",\"content\":\"one\\ntwo CHANGED\\nthree\\nfour\\n\"}", 0);
	ckstr(d, "- two",          "diff shows removed line");
	ckstr(d, "+ two CHANGED",  "diff shows added line");
	ckstr(d, "unchanged line above", "diff folds the common head");
	ckstr(d, "unchanged line", "diff folds the common tail");
	cknot(d, "- three",        "diff does not show unchanged lines");
	free(d);

	/* a brand new file is all additions, not a crash */
	d = hkDiffPreview("write_file", "{\"path\":\"new.txt\",\"content\":\"a\\nb\\n\"}", 0);
	ckstr(d, "+ a", "diff of a new file lists its lines");
	cknot(d, "- ",  "diff of a new file removes nothing");
	free(d);

	/* identical content must say so rather than print the file */
	d = hkDiffPreview("write_file",
		"{\"path\":\"f.txt\",\"content\":\"one\\ntwo\\nthree\\nfour\\n\"}", 0);
	ckstr(d, "identical", "diff reports an identical rewrite");
	free(d);

	d = hkDiffPreview("edit_file",
		"{\"path\":\"f.txt\",\"old\":\"two\",\"new\":\"2\"}", 0);
	ckstr(d, "- two", "edit_file diff shows the old snippet");
	ckstr(d, "+ 2",   "edit_file diff shows the new snippet");
	free(d);

	d = hkDiffPreview("edit_lines",
		"{\"path\":\"f.txt\",\"start\":2,\"end\":3,\"new\":\"II\\nIII\\n\"}", 0);
	ckstr(d, "- two",   "edit_lines diff shows the replaced range");
	ckstr(d, "+ II",    "edit_lines diff shows the replacement");
	free(d);

	/* reads are not writes: nothing to preview */
	d = hkDiffPreview("read_file", "{\"path\":\"f.txt\"}", 0);
	ck(d == NULL, "no diff for a read");
	free(d);

	if (chdir("/tmp") == 0) { snprintf(cmd, sizeof(cmd), "rm -rf %s", root); if (system(cmd)) {} }
}

/* --- shell timeout: the cap has to be real on every platform, not only where
   coreutils `timeout` happens to exist --- */
static void shell_tests(void) {
	long t0 = hk_time_ms();
	char *r = hkRunShellCapture("echo alive", 4096, 5);
	long dt = hk_time_ms() - t0;
	ckstr(r, "alive", "shell runs a normal command");
	ck(dt < 4000, "a fast command does not wait for the timeout");
	free(r);

	t0 = hk_time_ms();
	r = hkRunShellCapture("sleep 30", 4096, 2);
	dt = hk_time_ms() - t0;
	ckstr(r, "timed out", "a hung command is killed and reported");
	ck(dt < 12000, "the kill happens near the deadline");
	printf("  (sleep 30 with a 2s limit returned in %ldms)\n", dt);
	free(r);

	/* a child's background job dies with it: the kill reaches the whole group */
	t0 = hk_time_ms();
	r = hkRunShellCapture("sleep 30 & sleep 30; echo never", 4096, 2);
	dt = hk_time_ms() - t0;
	ck(dt < 12000 && (!r || !strstr(r, "never")), "a timeout kills the command's children too");
	free(r);

	/* a command never gets the keyboard: reading stdin is end-of-input, at once */
	t0 = hk_time_ms();
	r = hkRunShellCapture("read x; echo got:$x.", 4096, 5);
	dt = hk_time_ms() - t0;
	ckstr(r, "got:.", "a command reading stdin gets end-of-input, not the keyboard");
	ck(dt < 3000, "and does not wait for it");
	free(r);

	/* exit status survives the capture */
	r = hkRunShellCapture("exit 3", 4096, 5);
	ckstr(r, "exit code 3", "a silent failing command reports its exit code");
	free(r);

	/* the ceiling holds even when the model asks for more */
	r = hkRunShellCapture("echo capped", 4096, 99999);
	ckstr(r, "capped", "an over-large timeout is clamped, not rejected");
	free(r);
}

/* --- the --pipe approval handshake, which is what hake's Rei pane and the
   browser both speak. The names on both sides have to match exactly; they did
   not, and a write tool blocked the pane forever. --- */
/* Point stdin/stdout at files for a pipe-mode call, at the fd level: musl
   declares stdin/stdout const, so the old `stdout = fopen(...)` did not build. */
static int io_so = -1, io_si = -1;
static void io_redirect(const char *in, const char *out) {
	fflush(stdout);
	io_so = dup(1); io_si = dup(0);
	int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644), i = open(in, O_RDONLY);
	if (o >= 0) { dup2(o, 1); close(o); }
	if (i >= 0) { dup2(i, 0); close(i); }
	clearerr(stdin);
}
static void io_restore(void) {
	fflush(stdout);
	if (io_so >= 0) { dup2(io_so, 1); close(io_so); io_so = -1; }
	if (io_si >= 0) { dup2(io_si, 0); close(io_si); io_si = -1; }
	clearerr(stdin);
}

static void pipe_tests(void) {
	char tmpl[] = "/tmp/hakopipeXXXXXX";
	char *root = mkdtemp(tmpl);
	char cmd[512], outp[256], inp[256];
	if (!root) { printf("FAIL pipe mkdtemp\n"); fails++; return; }
	snprintf(cmd, sizeof(cmd), "printf 'alpha\\nbeta\\n' > %s/f.txt", root);
	if (system(cmd)) {}
	if (chdir(root)) {}
	hkGrantProjectTrust();

	snprintf(inp,  sizeof(inp),  "%s/in.jsonl",  root);
	snprintf(outp, sizeof(outp), "%s/out.jsonl", root);
	FILE *w = fopen(inp, "w");
	fputs("{\"type\":\"approve\",\"id\":1,\"answer\":\"y\"}\n", w);
	fclose(w);

	E.ai_auto_approve = 0;
	E.pipe_mode = 1;
	io_redirect(inp, outp);
	char *res = hkExecTool("write_file",
		"{\"path\":\"f.txt\",\"content\":\"alpha\\nBETA\\n\"}");
	io_restore();
	E.pipe_mode = 0;

	char *emitted = hkReadFileAll(outp, 100000);
	ckstr(emitted, "\"type\":\"tool_request\"", "pipe emits tool_request");
	ckstr(emitted, "\"name\":\"write_file\"",   "tool_request carries name");
	ckstr(emitted, "\"kind\":\"write\"",        "tool_request carries kind");
	ckstr(emitted, "\"diff\":\"",                "tool_request carries a diff");
	ckstr(emitted, "- beta",                      "pipe diff shows the removed line");
	ckstr(emitted, "+ BETA",                      "pipe diff shows the added line");
	/* the whole event must stay on one line or the reader splits it in half */
	ck(strchr(emitted, '\n') == emitted + strlen(emitted) - 1,
	   "tool_request is a single JSONL line");
	ckstr(res, "wrote",                            "an approved write goes through");
	ckstr(res, "wrote 11 bytes to f.txt (",        "the result names the file from the project, not the whole path");
	ck(res && !strstr(res, root),                  "the project's absolute path stays out of the result");
	free(emitted); free(res);

	/* and a denial must come back as a denial, not a hang */
	w = fopen(inp, "w");
	fputs("{\"type\":\"approve\",\"id\":2,\"answer\":\"n\"}\n", w);
	fclose(w);
	E.pipe_mode = 1;
	io_redirect(inp, outp);
	res = hkExecTool("write_file", "{\"path\":\"f.txt\",\"content\":\"nope\\n\"}");
	io_restore();
	E.pipe_mode = 0;
	ckstr(res, "denied", "a denied write is refused");
	free(res);

	if (chdir("/tmp") == 0) { snprintf(cmd, sizeof(cmd), "rm -rf %s", root); if (system(cmd)) {} }
}

/* --- OpenAI-compatible SSE: eleven of the thirteen providers speak this and
   none of them streamed before v0.2.4. The deltas must rebuild into exactly the
   body shape the non-streaming path already parses. --- */
static void sse_tests(void) {
	/* plain prose, arriving in pieces */
	hkSseAcc a;
	memset(&a, 0, sizeof(a));
	const char *prose[] = {
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":null}}]}",
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hel\"}}]}",
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"lo \\\"you\\\"\"}}]}",
		"data: {\"choices\":[{\"index\":0,\"delta\":{}}],\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":4}}",
		"data: [DONE]",
		NULL
	};
	char *seen = NULL; size_t sl = 0, sc = 0;
	for (int i = 0; prose[i]; i++) {
		char *c = hkSseFeed(&a, prose[i]);
		if (c) { hkSbAdd(&seen, &sl, &sc, c, strlen(c)); free(c); }
	}
	ck(a.done == 1, "sse sees [DONE]");
	ckstr(seen, "Hello \"you\"", "sse yields live text unescaped");
	char *body = hkSseFinish(&a);
	ckstr(body, "\"content\":\"Hello \\\"you\\\"\"", "sse re-escapes text for the rebuilt body");
	ckstr(body, "\"finish_reason\":\"stop\"", "sse marks a plain answer as stop");
	ckstr(body, "\"prompt_tokens\":11", "sse keeps usage");
	int api_err;
	char *extracted = aiExtractResponse(body, AI_PROVIDER_OPENAI, &api_err);
	ckstr(extracted, "Hello \"you\"", "the existing extractor reads the rebuilt body");
	ck(!api_err, "an answer is not an api error");
	free(extracted);
	/* groq's 401: was stored as the model's answer and reported ok */
	extracted = aiExtractResponse("{\"error\":{\"message\":\"Invalid API Key\",\"code\":\"invalid_api_key\"}}",
	                              AI_PROVIDER_OPENAI, &api_err);
	ck(api_err, "a provider error body is flagged as an error");
	free(extracted); free(body); free(seen); hkSseFree(&a);

	/* a tool call, with arguments split across chunks the way they really arrive */
	memset(&a, 0, sizeof(a));
	const char *calls[] = {
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_ab\",\"type\":\"function\",\"function\":{\"name\":\"grep\",\"arguments\":\"\"}}]}}]}",
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"pat\"}}]}}]}",
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"tern\\\":\\\"hkWalk\\\"}\"}}]}}]}",
		"data: [DONE]",
		NULL
	};
	for (int i = 0; calls[i]; i++) { char *c = hkSseFeed(&a, calls[i]); free(c); }
	ck(a.call_count == 1, "sse tracks one tool call");
	body = hkSseFinish(&a);
	ckstr(body, "\"tool_calls\":[", "rebuilt body has a tool_calls array");
	ckstr(body, "\"name\":\"grep\"", "tool name survives");
	ckstr(body, "\"id\":\"call_ab\"", "tool id survives");
	ckstr(body, "{\\\"pattern\\\":\\\"hkWalk\\\"}", "argument fragments concatenate correctly");
	ckstr(body, "\"finish_reason\":\"tool_calls\"", "a tool turn is marked as such");
	free(body); hkSseFree(&a);

	/* two parallel calls must not collapse into one: the choice's own "index"
	   is 0 for both, so the index has to be read from inside tool_calls */
	memset(&a, 0, sizeof(a));
	hkSseFeed(&a, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c0\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}}]}}]}");
	hkSseFeed(&a, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"c1\",\"function\":{\"name\":\"list_dir\",\"arguments\":\"{}\"}}]}}]}");
	ck(a.call_count == 2, "two parallel tool calls stay separate");
	body = hkSseFinish(&a);
	ckstr(body, "read_file", "first parallel call kept");
	ckstr(body, "list_dir",  "second parallel call kept");
	free(body); hkSseFree(&a);
}

/* --- context accounting and compaction --- */
static void compact_tests(void) {
	aiData d;
	memset(&d, 0, sizeof(d));
	pthread_mutex_init(&d.lock, NULL);
	d.system_prompt = strdup("system prompt here");

	ck(hkEstimateTokens("abcd") == 1, "token estimate is bytes/4");
	ck(hkContextWindow() > 0, "every provider reports a window");

	/* too short to be worth folding */
	aiPushMessage(&d, "user", "hello");
	aiPushMessage(&d, "assistant", "hi");
	ck(hkCompactHistory(&d) == 0, "a short conversation is left alone");

	/* a long one, with tool observations in the middle */
	for (int i = 0; i < 20; i++) {
		char m[64];
		snprintf(m, sizeof(m), "question number %d\nsecond line", i);
		aiPushMessage(&d, "user", m);
		aiPushMessage(&d, "assistant", "<observation tool=\"grep\">lots of output</observation>");
	}
	int before_count = d.message_count;
	int folded = hkCompactHistory(&d);
	ck(folded > 0, "a long conversation compacts");
	ck(d.message_count < before_count, "compaction removes messages");
	ck(d.message_count >= HK_CTX_KEEP_TAIL, "the recent tail survives");

	/* the opening request must still be there: it states the goal */
	ckstr(d.messages[0].content, "hello", "the first message is preserved");
	ckstr(d.messages[1].content, "compacted", "a digest replaces the middle");
	ckstr(d.messages[1].content, "ran grep", "the digest names tools that ran");
	ck(!strcmp(d.messages[1].role, "user"), "the digest is a user message");

	/* the tail must start on a plain user message, or a tool_result can be
	   orphaned from its tool_use and Anthropic rejects every later turn */
	ck(!strcmp(d.messages[2].role, "user"), "the tail begins on a user message");

	/* compacting again when there is nothing left to fold must be a no-op */
	int again = hkCompactHistory(&d);
	ck(again == 0 || d.message_count >= HK_CTX_KEEP_TAIL, "repeat compaction is safe");

	/* the real case: big tool output in the middle. This is what compaction is
	   for, and it has to actually reduce the token count. */
	aiData big;
	memset(&big, 0, sizeof(big));
	pthread_mutex_init(&big.lock, NULL);
	big.system_prompt = strdup("sys");
	char blob[4096];
	memset(blob, 'x', sizeof(blob) - 1);
	blob[sizeof(blob) - 1] = '\0';
	aiPushMessage(&big, "user", "find the bug in the parser");
	for (int i = 0; i < 12; i++) {
		aiPushMessage(&big, "assistant", "let me look");
		char obs[4200];
		snprintf(obs, sizeof(obs), "<observation tool=\"read_file\">%s</observation>", blob);
		aiPushMessage(&big, "user", obs);
	}
	long tok_before = hkContextUsed(&big);
	int  big_folded = hkCompactHistory(&big);
	long tok_after  = hkContextUsed(&big);
	ck(big_folded > 0, "a conversation full of tool output compacts");
	ck(tok_after < tok_before / 2, "compaction cuts the token count substantially");
	ckstr(big.messages[0].content, "find the bug", "the original task survives compaction");
	printf("  (compaction: %ld -> %ld tokens, %d messages folded)\n",
	       tok_before, tok_after, big_folded);
	for (int i = 0; i < big.message_count; i++) { free(big.messages[i].role); free(big.messages[i].content); }
	free(big.messages); free(big.system_prompt);
	pthread_mutex_destroy(&big.lock);

	/* a digest bigger than what it replaces is not a compaction */
	aiData t;
	memset(&t, 0, sizeof(t));
	pthread_mutex_init(&t.lock, NULL);
	for (int i = 0; i < 12; i++) aiPushMessage(&t, i % 2 ? "assistant" : "user", "ok");
	int tiny = hkCompactHistory(&t);
	ck(tiny == 0, "tiny messages are not replaced by a larger digest");
	for (int i = 0; i < t.message_count; i++) { free(t.messages[i].role); free(t.messages[i].content); }
	free(t.messages);
	pthread_mutex_destroy(&t.lock);

	for (int i = 0; i < d.message_count; i++) { free(d.messages[i].role); free(d.messages[i].content); }
	free(d.messages); free(d.system_prompt);
	pthread_mutex_destroy(&d.lock);
}

/* --- skills: descriptions in the prompt, bodies on demand --- */
static void skill_tests(void) {
	char tmpl[] = "/tmp/hakoskillXXXXXX";
	char *home = mkdtemp(tmpl);
	if (!home) { printf("FAIL skill mkdtemp\n"); fails++; return; }
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "mkdir -p %s/.hako/skills/demo", home);
	if (system(cmd)) {}

	/* a realistic skill: frontmatter with a folded description, then a long body */
	snprintf(cmd, sizeof(cmd),
		"{ printf -- '---\\nname: demo\\ndescription: >\\n  Does a demonstration thing.\\n"
		"  Use when the user asks for a demo.\\n---\\n\\n# Demo\\n'; "
		"for i in $(seq 1 120); do echo 'BODYLINE unique marker text for the body of the skill'; done; } > %s/.hako/skills/demo/SKILL.md",
		home);
	if (system(cmd)) {}

	char *saved_home = getenv("HOME");
	char *keep = saved_home ? strdup(saved_home) : NULL;
	setenv("HOME", home, 1);

	aiData d;
	memset(&d, 0, sizeof(d));
	pthread_mutex_init(&d.lock, NULL);
	int n = hkLoadSkills(&d);
	ck(n == 1, "the skill is found");
	ckstr(d.system_prompt, "Does a demonstration thing.", "the description reaches the prompt");
	ckstr(d.system_prompt, "Use when the user asks for a demo.", "folded description lines are joined");
	ckstr(d.system_prompt, "read_skill(skill=\"demo\"", "the prompt says how to load the body");
	cknot(d.system_prompt, "BODYLINE", "the skill body is NOT inlined");
	size_t big_prompt = strlen(d.system_prompt);

	/* a small skill is still inlined whole: the round trip would cost more */
	snprintf(cmd, sizeof(cmd),
		"mkdir -p %s/.hako/skills/tiny && printf -- '---\\nname: tiny\\ndescription: >\\n  A small one.\\n---\\nSHORTBODY\\n' > %s/.hako/skills/tiny/SKILL.md",
		home, home);
	if (system(cmd)) {}
	free(d.system_prompt); d.system_prompt = NULL;
	n = hkLoadSkills(&d);
	ck(n == 2, "both skills are found");
	ckstr(d.system_prompt, "SHORTBODY", "a small skill is still inlined in full");
	printf("  (system prompt with 1 summarised skill: %zu bytes)\n", big_prompt);

	/* :use turns a skill into a mode — the full body must come back, with a
	   marker telling the model it applies to every reply, and the OTHER skills
	   must stay summarised. That is the whole point: one mode, not nine. */
	free(d.system_prompt); d.system_prompt = NULL;
	E.ai_active_skills = strdup("demo");
	n = hkLoadSkills(&d);
	ckstr(d.system_prompt, "BODYLINE", "an active skill is inlined in full");
	ckstr(d.system_prompt, "ACTIVE MODE", "an active skill is marked as a mode");
	ckstr(d.system_prompt, "A small one.", "inactive skills stay summarised");
	size_t active_prompt = strlen(d.system_prompt);

	/* switching it off returns to the summary */
	free(E.ai_active_skills); E.ai_active_skills = NULL;
	free(d.system_prompt); d.system_prompt = NULL;
	n = hkLoadSkills(&d);
	cknot(d.system_prompt, "BODYLINE", "switching the mode off drops the body again");
	printf("  (prompt with mode on: %zu bytes, off: %zu)\n", active_prompt, strlen(d.system_prompt));

	/* name matching tolerates a .md suffix on either side */
	E.ai_active_skills = strdup("demo.md");
	ck(hkSkillActive("demo") == 1, "active match ignores a .md suffix");
	free(E.ai_active_skills); E.ai_active_skills = strdup("other,demo");
	ck(hkSkillActive("demo") == 1, "active match handles a list");
	ck(hkSkillActive("dem") == 0, "active match is not a prefix match");
	free(E.ai_active_skills); E.ai_active_skills = NULL;

	if (keep) { setenv("HOME", keep, 1); free(keep); } else unsetenv("HOME");
	free(d.system_prompt);
	pthread_mutex_destroy(&d.lock);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", home);
	if (system(cmd)) {}
}

/* --- project context discovery: hako should be a drop-in in a repo that is
   already set up for another agent, not demand its own file --- */
static void context_file_tests(void) {
	char tmpl[] = "/tmp/hakoctxXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL ctx mkdtemp\n"); fails++; return; }
	char cmd[512];
	char *keep = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", dir, 1);
	if (chdir(dir)) {}

	aiData d;
	memset(&d, 0, sizeof(d));
	pthread_mutex_init(&d.lock, NULL);

	/* nothing present: no context block at all */
	hkLoadSkills(&d);
	cknot(d.system_prompt, "project context", "no context file, no context block");
	free(d.system_prompt); d.system_prompt = NULL;

    /* CLAUDE.md alone is picked up */
	snprintf(cmd, sizeof(cmd), "printf 'CLAUDEMARKER\\n' > %s/CLAUDE.md", dir);
	if (system(cmd)) {}
	hkLoadSkills(&d);
	ckstr(d.system_prompt, "CLAUDEMARKER", "CLAUDE.md is read");
	ckstr(d.system_prompt, "CLAUDE.md (project context)", "the header names the file it came from");
	free(d.system_prompt); d.system_prompt = NULL;

	/* AGENTS.md outranks CLAUDE.md */
	snprintf(cmd, sizeof(cmd), "printf 'AGENTSMARKER\\n' > %s/AGENTS.md", dir);
	if (system(cmd)) {}
	hkLoadSkills(&d);
	ckstr(d.system_prompt, "AGENTSMARKER", "AGENTS.md is preferred over CLAUDE.md");
	cknot(d.system_prompt, "CLAUDEMARKER", "only one context file is loaded, not both");
	free(d.system_prompt); d.system_prompt = NULL;

	/* hako's own file outranks everything */
	snprintf(cmd, sizeof(cmd), "mkdir -p %s/.hako && printf 'HAKOMARKER\\n' > %s/.hako/HAKO.md", dir, dir);
	if (system(cmd)) {}
	hkLoadSkills(&d);
	ckstr(d.system_prompt, "HAKOMARKER", "HAKO.md wins when present");
	cknot(d.system_prompt, "AGENTSMARKER", "HAKO.md suppresses the fallbacks");
	free(d.system_prompt); d.system_prompt = NULL;

	/* a binary file must be refused, not pasted into the prompt */
	snprintf(cmd, sizeof(cmd), "rm -f %s/.hako/HAKO.md %s/AGENTS.md && printf 'bin\\000\\001stuff' > %s/CLAUDE.md", dir, dir, dir);
	if (system(cmd)) {}
	hkLoadSkills(&d);
	cknot(d.system_prompt, "stuff", "a binary context file is skipped");
	free(d.system_prompt);

	if (keep) { setenv("HOME", keep, 1); free(keep); } else unsetenv("HOME");
	pthread_mutex_destroy(&d.lock);
	if (chdir("/tmp") == 0) { snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); if (system(cmd)) {} }
}

/* --- Agent Skills: multi-root discovery. A repo already carrying skills for
   another client should work as-is, and project-local must win a name clash. --- */
static void skill_roots_tests(void) {
	char tmpl[] = "/tmp/hakorootsXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL roots mkdtemp\n"); fails++; return; }
	char cmd[1400];
	char *keep = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", dir, 1);
	if (chdir(dir)) {}

	snprintf(cmd, sizeof(cmd),
		"mkdir -p %s/.claude/skills/fromclaude %s/.hako/skills/fromproject %s/.hako/skills/dupe %s/.claude/skills/dupe && "
		"printf -- '---\\nname: fromclaude\\ndescription: >\\n  A skill that lives in the Claude Code directory.\\n---\\nCLAUDEBODY\\n' > %s/.claude/skills/fromclaude/SKILL.md && "
		"printf -- '---\\nname: fromproject\\ndescription: >\\n  A skill in the project hako directory.\\n---\\nPROJBODY\\n' > %s/.hako/skills/fromproject/SKILL.md && "
		"printf -- '---\\nname: dupe\\ndescription: >\\n  PROJECT WINS this clash.\\n---\\nBODY\\n' > %s/.hako/skills/dupe/SKILL.md && "
		"printf -- '---\\nname: dupe\\ndescription: >\\n  CLAUDE LOSES this clash.\\n---\\nBODY\\n' > %s/.claude/skills/dupe/SKILL.md",
		dir, dir, dir, dir, dir, dir, dir, dir);
	if (system(cmd)) {}
	/* the vendor-neutral root, and a one-file copy of a folder skill */
	snprintf(cmd, sizeof(cmd),
		"mkdir -p %s/.agents/skills/agentsone %s/.agents/skills/dupe && "
		"printf -- '---\\nname: agentsone\\ndescription: >\\n  From the AGENTS ROOT.\\n---\\nB\\n' > %s/.agents/skills/agentsone/SKILL.md && "
		"printf -- '---\\nname: dupe\\ndescription: >\\n  AGENTS LOSES too.\\n---\\nB\\n' > %s/.agents/skills/dupe/SKILL.md && "
		"printf -- '---\\nname: fromclaude\\ndescription: >\\n  FLAT COPY LOSES.\\n---\\nB\\n' > %s/.agents/skills/fromclaude.md",
		dir, dir, dir, dir, dir);
	if (system(cmd)) {}

	aiData d;
	memset(&d, 0, sizeof(d));
	pthread_mutex_init(&d.lock, NULL);
	int n = hkLoadSkills(&d);
	ck(n == 4, "skills load from every root, deduped by name");
	ckstr(d.system_prompt, "AGENTS ROOT", "a .agents/skills skill is picked up");
	cknot(d.system_prompt, "AGENTS LOSES", "a skill in two roots loads once, the nearest wins");
	cknot(d.system_prompt, "FLAT COPY LOSES", "x.md and x/ are the same skill");
	hkPromptInvalidate();
	hkSkillScan();
	int seen_agents = 0, dupes = 0;
	for (int i = 0; i < hk_skill_n; i++) { if (!strcmp(hk_skill_names[i], "agentsone")) seen_agents++; if (!strcmp(hk_skill_names[i], "dupe")) dupes++; }
	ck(seen_agents == 1 && dupes == 1, "the /<skill> list has each skill once");
	E.ai_auto_approve = 1;
	char *rs = hkExecTool("read_skill", "{\"skill\":\"agentsone\",\"path\":\"SKILL.md\"}");
	ckstr(rs, "AGENTS ROOT", "read_skill finds a skill outside ~/.hako/skills");
	free(rs);
	ckstr(d.system_prompt, "Claude Code directory", "a .claude/skills skill is picked up");
	ckstr(d.system_prompt, "project hako directory", "a project .hako/skills skill is picked up");
	ckstr(d.system_prompt, "PROJECT WINS", "project-local wins a name clash");
	cknot(d.system_prompt, "CLAUDE LOSES", "the shadowed copy is not loaded twice");
	free(d.system_prompt);
	pthread_mutex_destroy(&d.lock);

	if (keep) { setenv("HOME", keep, 1); free(keep); } else unsetenv("HOME");
	if (chdir("/tmp") == 0) { snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); if (system(cmd)) {} }
}

/* --- owner/repo[/path] parsing and the JSON array walker behind tap install --- */
static void tap_parse_tests(void) {
	char o[128], r[128], p[512];
	ck(hkParseRepoSpec("NousResearch/hermes-agent", o, sizeof o, r, sizeof r, p, sizeof p) == 0
	   && !strcmp(o, "NousResearch") && !strcmp(r, "hermes-agent") && !*p,
	   "owner/repo parses with an empty path");
	ck(hkParseRepoSpec("a/b/skills/k8s", o, sizeof o, r, sizeof r, p, sizeof p) == 0
	   && !strcmp(p, "skills/k8s"), "owner/repo/path keeps the path");
	ck(hkParseRepoSpec("https://example.com/x.md", o, sizeof o, r, sizeof r, p, sizeof p) != 0,
	   "a URL is not treated as a repo spec");
	ck(hkParseRepoSpec("a/b/../../etc", o, sizeof o, r, sizeof r, p, sizeof p) != 0,
	   "a path that climbs out is refused");
	ck(hkParseRepoSpec("a/b//etc", o, sizeof o, r, sizeof r, p, sizeof p) == 0,
	   "a doubled slash is harmless");
	ck(hkParseRepoSpec("noslash", o, sizeof o, r, sizeof r, p, sizeof p) != 0,
	   "a bare word is refused");
	ck(hkParseRepoSpec("a;rm -rf/b", o, sizeof o, r, sizeof r, p, sizeof p) != 0,
	   "shell characters in the owner are refused");

	/* the contents API nests _links inside every entry, so the walker has to be
	   depth-aware or it stops inside the first object */
	const char *arr =
		"[{\"name\":\"SKILL.md\",\"type\":\"file\",\"download_url\":\"https://x/1\","
		"\"_links\":{\"self\":\"a\",\"git\":\"b\"}},"
		"{\"name\":\"scripts\",\"type\":\"dir\",\"download_url\":null,"
		"\"_links\":{\"self\":\"c\"}}]";
	const char *cur = arr;
	size_t len = 0;
	int count = 0;
	const char *obj;
	char first_name[64] = "", second_type[64] = "";
	while ((obj = hkJsonArrayNext(&cur, &len)) != NULL) {
		char *slice = malloc(len + 1);
		memcpy(slice, obj, len); slice[len] = '\0';
		char *nm = hkExtractJsonString(slice, "name");
		char *ty = hkExtractJsonString(slice, "type");
		if (count == 0 && nm) snprintf(first_name, sizeof first_name, "%s", nm);
		if (count == 1 && ty) snprintf(second_type, sizeof second_type, "%s", ty);
		free(nm); free(ty); free(slice);
		count++;
	}
	ck(count == 2, "the array walker finds both entries past nested objects");
	ck(!strcmp(first_name, "SKILL.md"), "first entry reads correctly");
	ck(!strcmp(second_type, "dir"), "second entry reads correctly");
}

/* --- the task list: harness-held state for long jobs, kept in the system
   prompt so a compaction cannot fold it away --- */
static void todo_tests(void) {
	int total = 0, done = 0;
	/* whatever shape the model emits, one canonical form comes out */
	char *n = hkTodoNormalize(
		"1. [x] read the parser\n"
		"  - [ ] fix the off-by-one\n"
		"* [X] add a test\n"
		"plain line with no marker\n"
		"\n", &total, &done);
	ck(n != NULL, "normalize returns a list");
	ck(total == 4, "every item is kept");
	ck(done == 2, "done items are counted, case-insensitively");
	ckstr(n, "[x] read the parser", "numbering is stripped");
	ckstr(n, "[ ] fix the off-by-one", "bullets and indent are stripped");
	ckstr(n, "[ ] plain line with no marker", "an unmarked line defaults to pending");
	cknot(n, "1.", "the original numbering does not survive");
	free(n);

	/* the whole point: it rides in the system prompt, not the conversation */
	char tmpl[] = "/tmp/hakotodoXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL todo mkdtemp\n"); fails++; return; }
	char cmd[256];
	char *keep = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", dir, 1);
	if (chdir(dir)) {}

	aiData d;
	memset(&d, 0, sizeof(d));
	pthread_mutex_init(&d.lock, NULL);

	hkLoadSkills(&d);
	cknot(d.system_prompt, "Current task list", "no list, no section in the prompt");
	free(d.system_prompt); d.system_prompt = NULL;

	hk_todo = hkTodoNormalize("[ ] ship the release\n[x] write the tests\n", NULL, NULL);
	hkLoadSkills(&d);
	ckstr(d.system_prompt, "# Current task list", "the list is injected into the system prompt");
	ckstr(d.system_prompt, "[ ] ship the release", "pending items appear");
	ckstr(d.system_prompt, "[x] write the tests", "finished items appear");

	/* compaction folds messages; the list must be untouched by it */
	d.system_prompt = d.system_prompt;
	for (int i = 0; i < 24; i++) {
		char m[4200];
		memset(m, 'y', sizeof(m) - 1); m[sizeof(m) - 1] = '\0';
		aiPushMessage(&d, i % 2 ? "assistant" : "user", m);
	}
	int folded = hkCompactHistory(&d);
	ck(folded > 0, "the conversation compacts");
	ckstr(d.system_prompt, "[ ] ship the release", "the task list survives compaction");

	for (int i = 0; i < d.message_count; i++) { free(d.messages[i].role); free(d.messages[i].content); }
	free(d.messages); free(d.system_prompt);
	free(hk_todo); hk_todo = NULL;
	pthread_mutex_destroy(&d.lock);
	if (keep) { setenv("HOME", keep, 1); free(keep); } else unsetenv("HOME");
	if (chdir("/tmp") == 0) { snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); if (system(cmd)) {} }
}

/* --- memory: the thing that makes it grow across sessions. Project beats
   global, and it rides in the system prompt so compaction cannot lose it. --- */
static void memory_tests(void) {
	char tmpl[] = "/tmp/hakomemXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL mem mkdtemp\n"); fails++; return; }
	char cmd[512];
	char *keep = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", dir, 1);
	/* The project must NOT be the home directory, or ~/.hako/memory and
	   ./.hako/memory are the same path and the scoping test proves nothing. */
	char proj[PATH_MAX];
	snprintf(proj, sizeof(proj), "%s/work", dir);
	snprintf(cmd, sizeof(cmd), "mkdir -p %s", proj);
	if (system(cmd)) {}
	if (chdir(proj)) {}
	hkGrantProjectTrust();
	E.ai_auto_approve = 1;

	aiData d;
	memset(&d, 0, sizeof(d));
	pthread_mutex_init(&d.lock, NULL);

	hkLoadSkills(&d);
	cknot(d.system_prompt, "What you already know", "no memories, no section");
	free(d.system_prompt); d.system_prompt = NULL;

	char *r = hkExecTool("remember",
		"{\"title\":\"Build command\",\"text\":\"This project builds with make all, not make.\"}");
	ckstr(r, "remembered", "the remember tool saves");
	ckstr(r, "build-command", "the title becomes a readable slug");
	free(r);

	r = hkExecTool("remember",
		"{\"title\":\"Editor pref\",\"text\":\"User prefers tabs.\",\"global\":1}");
	ckstr(r, "global", "a global memory is saved globally");
	free(r);

	hkLoadSkills(&d);
	ckstr(d.system_prompt, "What you already know", "memories reach the system prompt");
	ckstr(d.system_prompt, "make all, not make", "the project memory is present");
	ckstr(d.system_prompt, "User prefers tabs", "the global memory is present");
	ckstr(d.system_prompt, "(project)", "memories are labelled by scope");

	/* survives a fold of the conversation, same as the task list */
	for (int i = 0; i < 24; i++) {
		char m[4200];
		memset(m, 'z', sizeof(m) - 1); m[sizeof(m) - 1] = '\0';
		aiPushMessage(&d, i % 2 ? "assistant" : "user", m);
	}
	ck(hkCompactHistory(&d) > 0, "the conversation compacts");
	ckstr(d.system_prompt, "make all, not make", "memory survives compaction");

	/* files on disk, one fact each, editable by hand */
	snprintf(cmd, sizeof(cmd), "test -f %s/.hako/memory/build-command.md", proj);
	ck(system(cmd) == 0, "a project memory is one file in the project .hako/memory");
	snprintf(cmd, sizeof(cmd), "test -f %s/.hako/memory/editor-pref.md", proj);
	ck(system(cmd) != 0, "a global memory does NOT land in the project directory");
	snprintf(cmd, sizeof(cmd), "test -f %s/.hako/memory/editor-pref.md", dir);
	ck(system(cmd) == 0, "a global memory lands under HOME");

	/* a slug cannot escape the directory */
	r = hkExecTool("remember", "{\"title\":\"../../etc/passwd\",\"text\":\"nope\"}");
	free(r);
	snprintf(cmd, sizeof(cmd), "test -f %s/etc/passwd", proj);
	ck(system(cmd) != 0, "a title cannot climb out of the memory directory");

	for (int i = 0; i < d.message_count; i++) { free(d.messages[i].role); free(d.messages[i].content); }
	free(d.messages); free(d.system_prompt);
	pthread_mutex_destroy(&d.lock);
	E.ai_auto_approve = 0;
	if (keep) { setenv("HOME", keep, 1); free(keep); } else unsetenv("HOME");
	if (chdir("/tmp") == 0) { snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); if (system(cmd)) {} }
}

/* --- rendering: cell widths, wrapping, markdown. The phone is the reason this
   matters — at 34 columns unwrapped output loses its gutter and the transcript
   becomes one left-aligned wall. --- */
static void render_tests(void) {
	/* cell width: CJK and fullwidth are two cells, escapes are zero */
	ck(clCellWidth("abc") == 3, "ascii is one cell each");
	ck(clCellWidth("\xE7\xAE\xB1") == 2, "a CJK glyph is two cells");
	ck(clCellWidth("\xE7\xAE\xB1 hako") == 7, "mixed width measures correctly");
	ck(clCellWidth("\x1b[1mbold\x1b[0m") == 4, "ANSI escapes cost no cells");
	ck(clCellWidth("\xE2\xA0\x8B") == 1, "braille stays one cell (Unicode width)");

	/* clipping respects cells and escapes */
	char buf[64];
	snprintf(buf, sizeof buf, "%s", "\xE7\xAE\xB1\xE7\xAE\xB1\xE7\xAE\xB1");
	clClipToCells(buf, 4);
	ck(clCellWidth(buf) == 4, "clip lands on a cell boundary, not a byte one");

	snprintf(buf, sizeof buf, "%s", "\x1b[1mabcdef\x1b[0m");
	clClipToCells(buf, 3);
	ck(clCellWidth(buf) == 3, "clip counts cells through escapes");

	/* markdown blocks */
	char out[512];
	clRenderMarkdownInline("# Heading", out, sizeof out);
	ckstr(out, "Heading", "heading text survives");
	cknot(out, "#", "heading hashes are stripped");

	clRenderMarkdownInline("- [ ] pending", out, sizeof out);
	ckstr(out, "\xE2\x97\x8B", "an unchecked task renders as a circle");
	cknot(out, "[ ]", "the raw checkbox is gone");

	clRenderMarkdownInline("- [x] done", out, sizeof out);
	ckstr(out, "\xE2\x9C\x93", "a checked task renders as a tick");

	clRenderMarkdownInline("3. third", out, sizeof out);
	ckstr(out, "3.", "an ordered marker keeps its number");
	ckstr(out, "third", "ordered item text survives");

	clRenderMarkdownInline("- plain bullet", out, sizeof out);
	ckstr(out, "\xC2\xB7", "a plain bullet renders as a middot");

    clRenderMarkdownInline("---", out, sizeof out);
	ckstr(out, "\xE2\x94\x80", "a horizontal rule draws a rule");

	clRenderMarkdownInline("| tool | purpose |", out, sizeof out);
	ckstr(out, "tool", "a table row keeps its first cell");
	ckstr(out, "purpose", "a table row keeps its later cells");
	cknot(out, "|", "the outer and inner pipes are gone");

	clRenderMarkdownInline("|---|:--:|", out, sizeof out);
	cknot(out, "-|-", "a table separator row is not printed as punctuation");
	ckstr(out, "\xE2\x94\x80", "it becomes a rule");

	clRenderMarkdownInline("see [the docs](https://example.com) for more", out, sizeof out);
	ckstr(out, "the docs", "a link keeps its text");
	ckstr(out, "https://example.com", "a link keeps its target");
	cknot(out, "](", "the markdown link punctuation is gone");

	clRenderMarkdownInline("an [unclosed link( stays put", out, sizeof out);
	ckstr(out, "unclosed link", "a malformed link is left alone rather than eaten");

	clRenderMarkdownInline("has **bold** and `code`", out, sizeof out);
	ckstr(out, "bold", "inline bold survives");
	ckstr(out, "code", "inline code survives");

	/* the streaming line buffer must split exactly on newlines */
	clStreamLine sl;
	memset(&sl, 0, sizeof sl);
	ck(sl.len == 0, "stream buffer starts empty");
	clStreamPush(&sl, "par");
	ck(sl.open == 0 && sl.parked == 1 && cl_out_depth == 0,
	   "after a delta the bracket is released again: the bar is UP between deltas");
	clStreamPush(&sl, "tial");
	ck(sl.len == 7, "partial chunks accumulate without emitting");
	clStreamPush(&sl, " line\n");
	ck(sl.len == 0, "a newline flushes the buffered line");
	ck(sl.shown == 0, "the echoed partial was withdrawn before the rendered line");
	clStreamPush(&sl, "trailing");
	ck(sl.len == 8, "text after the newline starts a new line");
	clStreamEnd(&sl);
	ck(sl.len == 0, "end empties the buffer");
	ck(sl.open == 0 && cl_out_depth == 0, "end leaves no bracket open");
	clStreamEnd(&sl);
	ck(cl_out_depth == 0, "a second end is a no-op");
}

/* --- model catalog: a wrong context window makes compaction fire early on a
   model with room to spare, and a missing price row silently misprices a run --- */
/* --- auto-orient: fires on a request about the project, never on chatter --- */
static void orient_tests(void) {
	ck( hkLooksActionable("fix the build"), "imperative at the start fires");
	ck( hkLooksActionable("can you add a readme"), "softener + verb fires");
	ck( hkLooksActionable("what's in styles.css"), "a file extension fires");
	ck( hkLooksActionable("lets list the files here"), "lets + verb fires");
	ck(!hkLooksActionable("you're a mithraeum model running inside your own agent and running on your own engine. how cool."),
	   "\"running\" mid-sentence does NOT fire (the unprompted list_dir/read_file spree)");
	ck(!hkLooksActionable("hey buddy"), "greeting does not fire");
	ck(!hkLooksActionable("this is so solid"), "praise does not fire");
	ck(!hkLooksActionable("i'll check back later"), "\"check\" mid-sentence does not fire");
	ck(!hkLooksActionable("what is a makefile"), "a question about a concept does not fire");
}

/* The orphan </tool_call>: observed live against anthropic on iSh, three of
   five tool turns. The model drops the OPENING tag, every downstream stage
   keys off it, and the turn ends having done nothing. */
static void orphan_tool_close_tests(void) {
	char *r = hkNormalizeToolDialect(
		"\n{\"name\": \"run_shell\", \"arguments\": {\"cmd\": \"uname -a\"}}\n</tool_call>");
	ck(r != NULL, "orphan </tool_call> is repaired");
	if (r) {
		ck(strstr(r, "<tool_call>") != NULL, "the opening tag is put back");
		ck(hkHasToolBlock(r), "the repaired text passes the tool-branch gate");
		free(r);
	}

	/* a well-formed call must come back untouched (NULL = no change) */
	r = hkNormalizeToolDialect("<tool_call>\n{\"name\": \"list_dir\"}\n</tool_call>");
	ck(r == NULL, "a well-formed call is not rewritten");
	free(r);

	/* prose that merely mentions the tag is NOT a tool call */
	r = hkNormalizeToolDialect("the parser looks for </tool_call> at the end.");
	ck(r == NULL, "prose containing the close tag is left alone");
	free(r);

	/* no name key -> not a tool object */
	r = hkNormalizeToolDialect("{\"x\": 1}</tool_call>");
	ck(r == NULL, "an object with no name is not repaired");
	free(r);
}

/* Anthropic reports a transient overload as an SSE error EVENT, which the
   delta loop ignored -> "empty response" with no cause and no retry. */
static void stream_error_tests(void) {
	ck( hkRetryableStreamError("data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\"}}"),
	    "overloaded_error is retryable");
	ck( hkRetryableStreamError("{\"type\": \"error\", \"error\": {\"type\": \"api_error\"}}"),
	    "api_error is retryable (spaced JSON too)");
	ck(!hkRetryableStreamError(""), "an empty body is not a provider error");
	ck(!hkRetryableStreamError("data: {\"type\":\"content_block_delta\"}"),
	    "an ordinary delta is not an error");
}

/* An SSE line longer than the read buffer used to arrive in pieces: the
   first piece was truncated JSON and every continuation failed the `data: `
   test and was discarded. Silent text loss. */
static void sse_longline_tests(void) {
	char path[] = "/tmp/hakolineXXXXXX";
	int fd = mkstemp(path);
	ck(fd >= 0, "temp file for the long-line test");
	if (fd < 0) return;
	FILE *w = fdopen(fd, "w");
	size_t big = 20000;
	fputs("data: {\"text\":\"", w);
	for (size_t i = 0; i < big; i++) fputc('x', w);
	fputs("\"}\n", w);
	fputs("short line\n", w);
	fclose(w);

	hkHttpNative h;
	memset(&h, 0, sizeof h);
	h.fp = fopen(path, "r");
	ck(h.fp != NULL, "reopened the long-line fixture");
	if (h.fp) {
		char buf[8192], *heap = NULL;
		char *l1 = hkHttpReadLine(&h, buf, sizeof buf, &heap);
		ck(l1 && strlen(l1) > big, "an oversized SSE line is reassembled whole");
		ck(l1 && strncmp(l1, "data: ", 6) == 0, "the reassembled line keeps its data: prefix");
		free(heap); heap = NULL;
		char *l2 = hkHttpReadLine(&h, buf, sizeof buf, &heap);
		ck(l2 && strncmp(l2, "short line", 10) == 0, "the NEXT line is not swallowed by the reassembly");
		ck(heap == NULL, "a short line allocates nothing");
		free(heap);
		fclose(h.fp);
	}
	unlink(path);
}

/* The paren dialect double-escaped: content="a\\nb" means a NEWLINE, and
   emitting \\\\n put a literal backslash-n into every file written that way. */
static void paren_escape_tests(void) {
	char name[80];
	char *j = hkParenArgsToJson("write_file(path=\"a.md\", content=\"one\\ntwo\\n\\nthree\")",
	                            name, sizeof name);
	ck(j != NULL, "paren dialect parses");
	if (j) {
		char *c = hkJsonFirst(j, "content", NULL);
		char *u = c ? hkJsonUnescape(c, (int)strlen(c)) : NULL;
		ck(u && !strcmp(u, "one\ntwo\n\nthree"), "paren \\n becomes a real newline, not literal n");
		free(c); free(u); free(j);
	}
	/* a lone backslash is still escaped so the JSON stays valid */
	j = hkParenArgsToJson("write_file(path=\"b\", content=\"C:\\\\x\")", name, sizeof name);
	if (j) {
		char *c = hkJsonFirst(j, "content", NULL);
		char *u = c ? hkJsonUnescape(c, (int)strlen(c)) : NULL;
		ck(u && strchr(u, '\\') != NULL, "a real backslash survives the round trip");
		free(c); free(u); free(j);
	}
}

/* A JSON string value ending in a backslash. The old lookbehind scan could not
   tell \" from \\" and ran off the end, returning NULL — and in the SSE loop a
   NULL dropped the ENTIRE delta, so a backslash landing on a delta boundary
   silently deleted that chunk of the answer. That is how a model's \n reached
   a file as a bare n. */
static void json_escape_scan_tests(void) {
	char *r = hkExtractJsonString("{\"text\":\"chat\\\\\"}", "text");
	ck(r != NULL, "a value ending in a backslash still parses");
	ck(r && !strcmp(r, "chat\\\\"), "the trailing escaped backslash is kept");
	free(r);

	r = hkExtractJsonString("{\"text\":\"say \\\"hi\\\"\"}", "text");
	ck(r && !strcmp(r, "say \\\"hi\\\""), "an escaped quote does not end the string");
	free(r);

	/* the two halves of a split delta must rejoin into one escape */
	char *a = hkExtractJsonString("{\"text\":\"chat\\\\\"}", "text");
	char *b = hkExtractJsonString("{\"text\":\"nA dog\"}", "text");
	char *ua = a ? hkJsonUnescape(a, (int)strlen(a)) : NULL;
	char *ub = b ? hkJsonUnescape(b, (int)strlen(b)) : NULL;
	char joined[64]; snprintf(joined, sizeof joined, "%s%s", ua?ua:"", ub?ub:"");
	ck(!strcmp(joined, "chat\\nA dog"), "a backslash split across deltas rejoins as one escape");
	free(a); free(b); free(ua); free(ub);
}

/* The claim must NEVER be skipped on the strength of cl_composing. The reader
   clears that flag while it parks and sets it again after; a tool call landing
   in the gap used to skip the claim and read the tty while the reader was also
   reading it — the freeze after a successful edit, and the "denied" nobody
   pressed. (Pre-acking so the wait returns at once; this asserts the invariant,
   not the timing.) */
static void stdin_claim_tests(void) {
	int saved = cl_composing;
	cl_composing = 0;
	hk_stdin_free = 1;
	hk_need_stdin = 0;
	hkClaimStdin();
	ck(hk_need_stdin == 1, "the keyboard claim is raised even when cl_composing is false");
	hkReleaseStdin();
	ck(hk_need_stdin == 0, "releasing the keyboard lowers the claim");
	cl_composing = saved;
}

/* Truncation must never be silent: the model that cannot see the end of a file
   will happily rewrite it from what it did see, and that destroys the tail. */
/* --- the <invoke>/<parameter> dialect built its arguments in a fixed 8 KB
   buffer: a large write_file landed CUT SHORT with no error, and a long
   parameter name wrote past the end of the stack buffer. --- */
static void invoke_dialect_tests(void) {
	char tmpl[] = "/tmp/hakoinvXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL invoke mkdtemp\n"); fails++; return; }
	char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof cwd)) return;
	if (chdir(dir) != 0) return;
	hkGrantProjectTrust();
	int saved_auto = E.ai_auto_approve; E.ai_auto_approve = 1;
	aiData dd; clInitAI(&dd); aiData *d = &dd;

	size_t body_n = 20000;
	char *body = malloc(body_n + 1);
	for (size_t i = 0; i < body_n; i++) body[i] = "ab\"\\\ncd\t"[i % 8];
	body[body_n] = '\0';
	size_t cap = body_n + 256;
	char *msg = malloc(cap);
	snprintf(msg, cap, "<invoke name=\"write_file\"><parameter name=\"path\">big.txt</parameter>"
	                   "<parameter name=\"content\">%s</parameter></invoke>", body);
	char **seen = NULL; int sn = 0, sc = 0, dups = 0;
	int n = hkReactToolExecAll(d, msg, &seen, &sn, &sc, &dups, NULL);
	ck(n == 1, "an <invoke> write_file runs");
	char *got = hkReadFileAll("big.txt", 1 << 20);
	ck(got && strlen(got) == body_n && !memcmp(got, body, body_n),
	   "a 20 KB write through <invoke> lands whole, quotes/backslashes/tabs intact");
	free(got);

	char *longname = malloc(12000 + 256);
	int o = snprintf(longname, 64, "<invoke name=\"read_file\"><parameter name=\"");
	memset(longname + o, 'k', 11000); o += 11000;
	snprintf(longname + o, 64, "\">x</parameter></invoke>");
	(void)hkReactToolExecAll(d, longname, &seen, &sn, &sc, &dups, NULL);
	ck(1, "an 11 KB parameter name does not overrun");

	/* the escaped value can be six times its source; the old buffer was 2x */
	char ctl[600]; int co = snprintf(ctl, 64, "<parameter name=\"content\">");
	memset(ctl + co, 1, 400); co += 400; snprintf(ctl + co, 64, "</parameter>");
	/* an unterminated parameter after a valid one looped forever once */
	char *hz = hkNormalizeToolDialect("x</invoke><parameter name=\"k\">v</parameter><parameter name=\"");
	ck(hz != NULL, "an unterminated <parameter> after a valid one terminates");
	free(hz);
	char *nz = hkNormalizeToolDialect(ctl);
	ck(nz && strstr(nz, "\\u0001") != NULL, "control bytes in a parameter are escaped, not overrun");
	free(nz);
	free(longname);

	for (int i = 0; i < sn; i++) free(seen[i]);
	free(seen);
	free(body); free(msg);
	E.ai_auto_approve = saved_auto;
	if (chdir(cwd) != 0) { /* nothing useful to do */ }
}

/* --- ~/.hakorc: every key the README documents must actually be read. Two of
   them (prompt_cache, context_window) were only ever read from saved state. --- */
static void rc_tests(void) {
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/.hakorc", getenv("HOME"));
	FILE *f = fopen(path, "w");
	fputs("theme=gruvbox\nmouse=1\nprompt_cache=0\ncontext_window=12345\nshell_timeout=42\n", f);
	fclose(f);
	int pc = E.ai_prompt_cache; long cw = E.ai_context_window; int mo = E.mouse_enabled, st = E.ai_shell_timeout;
	E.ai_prompt_cache = 1; E.mouse_enabled = 0;   /* so "it applied" is not "it was already so" */
	clLoadRc();
	ck(!strcmp(TH_ACTIVE, "gruvbox"), "theme= in ~/.hakorc applies (one of the 18 shared names)");
	ck(E.mouse_enabled == 1, "mouse= in ~/.hakorc applies");
	ck(E.ai_prompt_cache == 0, "prompt_cache= in ~/.hakorc applies");
	ck(E.ai_context_window == 12345, "context_window= in ~/.hakorc applies");
	ck(E.ai_shell_timeout == 42, "shell_timeout= in ~/.hakorc applies");
	remove(path);
	E.ai_prompt_cache = pc; E.ai_context_window = cw; E.mouse_enabled = mo; E.ai_shell_timeout = st;
	clThemeApply("mithraeum");
}

/* --- the unified rc (DESIGN 2026-10-29 §1): search order, code. keys, the
   project file behind trust and its denylist, writing one key in place,
   custom themes, and the theme gone from per-project state --- */
static void put(const char *path, const char *text) {
	FILE *f = fopen(path, "w"); if (f) { fputs(text, f); fclose(f); }
}
static char *slurp(const char *path) { return hk_fs_read(path, 1 << 20, NULL); }

static void rc_unified_tests(void) {
	ck(!strcmp(TH_PRESETS[0].name, "terminal"), "terminal is the first preset (the default)");
	char home[PATH_MAX], rc[PATH_MAX], xdg[PATH_MAX + 32], alt[PATH_MAX + 16], cmd[PATH_MAX * 2];
	snprintf(home, sizeof home, "%s", getenv("HOME"));
	snprintf(rc, sizeof rc, "%s/.hakorc", home);
	snprintf(xdg, sizeof xdg, "%s/.config/hako/hakorc", home);
	snprintf(alt, sizeof alt, "%s/alt.rc", home);
	snprintf(cmd, sizeof cmd, "mkdir -p %s/.config/hako", home);
	if (system(cmd)) {}
	unsetenv("HAKORC"); unsetenv("XDG_CONFIG_HOME");
	remove(rc);

	/* the XDG file is found when there is no ~/.hakorc */
	put(xdg, "theme = nord \n");
	clLoadRc();
	ck(!strcmp(TH_ACTIVE, "nord"), "~/.config/hako/hakorc is read, key and value trimmed");
	ckstr(hk_rc_path, ".config/hako/hakorc", "writes go to the file that was found");
	ckstr(getenv("HAKORC"), ".config/hako/hakorc", "HAKORC is exported for the editor and the studio");
	unsetenv("HAKORC");
	/* ~/.hakorc comes before it, $HAKORC before both */
	put(rc, "theme=dracula\n");
	clLoadRc();
	ck(!strcmp(TH_ACTIVE, "dracula"), "~/.hakorc wins over ~/.config/hako/hakorc");
	put(alt, "theme=monokai\n");
	setenv("HAKORC", alt, 1);
	clLoadRc();
	ck(!strcmp(TH_ACTIVE, "monokai"), "$HAKORC wins over both");
	unsetenv("HAKORC");
	remove(xdg);

	/* code.<key> beats <key> in either order; edit./studio. are someone else's */
	put(rc, "# mine\ncode.theme=solarized\ntheme=dracula\nedit.theme=gruvbox\nstudio.theme=washi\nmouse=0\ncode.mouse=1\n");
	E.mouse_enabled = 0;
	clLoadRc();
	ck(!strcmp(TH_ACTIVE, "solarized"), "code.theme beats theme even when theme comes later");
	ck(E.mouse_enabled == 1, "code.mouse beats mouse");
	ck(!strcmp(hkThemeFor(HK_RC_STUDIO), "washi"), "studio.theme is the studio's");

	/* one key set in place: the line rewritten, comments and order kept */
	ck(hkRcSaveTheme(HK_RC_CODE, "nord") == 0, "the picker writes the rc");
	char *t = slurp(rc);
	ckstr(t, "# mine\ncode.theme=nord\ntheme=dracula\n", "code.theme rewritten in place, the rest kept");
	free(t);
	put(rc, "# only plain\ntheme=dracula\nmouse=0\n");
	clLoadRc();
	hkRcSaveTheme(HK_RC_CODE, "nord");
	t = slurp(rc);
	ckstr(t, "# only plain\ntheme=nord\nmouse=0\n", "without code.theme the plain key is the one written");
	cknot(t, "code.theme", "and no code.theme is invented");
	free(t);
	hkRcSet("voice", "1");
	t = slurp(rc);
	ckstr(t, "mouse=0\nvoice=1\n", "a new key is appended");
	free(t);

	/* custom themes: a preset base plus slot overrides, listed after the presets */
	put(rc, "theme=mine\ntheme.mine.base=nord\ntheme.mine.accent=#112233\ntheme.mine.fg=ansi3\n");
	clLoadRc();
	ck(!strcmp(TH_ACTIVE, "mine"), "a custom theme is applied by name");
	ckstr(TH_ACCENT, "17;34;51", "its #hex slot is used");
	ckstr(TH_AI, "[33m", "fg feeds the ai slot, ansi3 is the profile's yellow");
	const char *names[64];
	int nn = hkThemeNames(names, 64);
	ck(nn > 2 && !strcmp(names[0], "terminal") && !strcmp(names[nn - 1], "mine"), "custom names come after the presets");
	put(rc, "theme=nosuchtheme\n");
	clLoadRc();
	ck(!strcmp(TH_ACTIVE, "terminal"), "an unknown theme falls back to terminal");

	/* the theme left per-project state: an old file that still says theme= is ignored */
	char sp[PATH_MAX + 16];
	snprintf(sp, sizeof sp, "%s/oldstate", home);
	put(sp, "ai_tools_enabled=1\ntheme=dracula\n");
	hkLoadSessionFile(sp, 1);
	ck(!strcmp(TH_ACTIVE, "terminal"), "theme= in old per-project state is ignored");
	hkWriteSessionFile(sp, 0);
	t = slurp(sp);
	cknot(t, "theme=", "state no longer saves the theme");
	free(t);

	/* the project's own .hakorc: only once trusted, never for a denied key */
	char proj[PATH_MAX + 8], prc[PATH_MAX + 16], keep[PATH_MAX];
	if (!getcwd(keep, sizeof keep)) keep[0] = '\0';
	snprintf(proj, sizeof proj, "%s/proj", home);
	snprintf(cmd, sizeof cmd, "mkdir -p %s", proj);
	if (system(cmd)) {}
	if (chdir(proj)) {}
	snprintf(prc, sizeof prc, "%s/.hakorc", proj);
	put(rc, "theme=dracula\nai_endpoint=https://mine.example\n");
	put(prc, "theme=gruvbox\nai_endpoint=https://evil.example\nai_api_key=sk-evil\nvoice_cmd=rm -rf ~\nbuild_cmd=make\n");
	free(E.ai_endpoint); E.ai_endpoint = NULL;
	hk_rc_proj_done = 0; hk_rc_proj[0] = '\0';
	clLoadRc();
	aiData d; memset(&d, 0, sizeof d); clInitAI(&d);
	hkRcProjectLoad(&d);
	ck(!strcmp(TH_ACTIVE, "dracula"), "an untrusted folder's .hakorc is not read");
	hkGrantProjectTrust();
	hkRcProjectLoad(&d);
	ck(!strcmp(TH_ACTIVE, "gruvbox"), "once trusted, the project .hakorc beats the global one");
	ck(E.ai_endpoint && !strcmp(E.ai_endpoint, "https://mine.example"), "a project .hakorc cannot set ai_endpoint");
	ck(!E.voice_cmd, "nor voice_cmd");
	int said = 0;
	for (int i = 0; i < d.history_count; i++)
		if (strstr(d.history[i], "project .hakorc: ai_endpoint, ai_api_key, voice_cmd ignored (set it in ~/.hakorc)")) said++;
	ck(said == 1, "the refused keys are named once");
	ck(hkRcGet(HK_RC_EDIT, "build_cmd") != NULL, "hake's build_cmd stays allowed");

	/* a project ai_provider would carry the global rc's key to another provider */
	put(rc, "ai_provider=anthropic\nai_api_key=sk-mine\n");
	put(prc, "ai_provider=groq\n");
	hk_rc_proj_done = 0; hk_rc_proj[0] = '\0';
	clLoadRc();
	hkRcProjectLoad(&d);
	ck(E.ai_provider_type == AI_PROVIDER_ANTHROPIC, "with a global ai_api_key, a project cannot switch the provider");
	put(rc, "ai_provider=anthropic\n");
	hk_rc_proj_done = 0; hk_rc_proj[0] = '\0';
	clLoadRc();
	hkRcProjectLoad(&d);
	ck(E.ai_provider_type != AI_PROVIDER_ANTHROPIC, "without one it may (a local model for this repo)");
	free(E.ai_api_key); E.ai_api_key = NULL;
	clCleanupAI(&d);
	if (keep[0] && chdir(keep)) {}
	hk_rc_proj_done = 0; hk_rc_proj[0] = '\0';
	remove(prc); remove(rc); remove(alt); remove(sp);
	clLoadRc();
	ck(!strcmp(TH_ACTIVE, "terminal"), "no rc at all: terminal colors");
	clThemeApply("mithraeum");

	/* a phone (relay) may not widen what the desk allows: :editor and :e run a command there */
	hs_remote = 1;
	ck(hkSlashRefusedRemote(":editor sh -c x") && hkSlashRefusedRemote(":e") && hkSlashRefusedRemote("/e vim"),
	   "a phone cannot set or open the desk's editor");
	ck(!hkSlashRefusedRemote(":edit") && !hkSlashRefusedRemote(":name x"), "but :edit and :name still work");
	hs_remote = 0;
}

/* --- commands (§2), the lean context (§3), voice text, the update compare --- */
static void cmd_and_context_tests(void) {
	int m[CL_MENU_CAP];
	int n = hkCmdMatch(':', "e", 1, m, CL_MENU_CAP);
	ck(n > 3 && !strcmp(hkCmdAt(m[0]).n, "e"), "an exact name comes first (:e before :edit)");
	int ix_edit = -1, ix_help = -1;
	for (int i = 0; i < n; i++) { if (!strcmp(hkCmdAt(m[i]).n, "edit")) ix_edit = i; if (!strcmp(hkCmdAt(m[i]).n, "help")) ix_help = i; }
	ck(ix_edit > 0 && ix_help > ix_edit, "prefix matches before substring ones (edit before help)");
	n = hkCmdMatch(':', "", 0, m, CL_MENU_CAP);
	int alias = 0;
	for (int i = 0; i < n; i++) if (hkCmdAt(m[i]).f & HK_CF_ALIAS) alias++;
	ck(alias == 0, "the bare menu leaves aliases out");
	ck(!strcmp(hkCmdAt(m[0]).n, "help"), "and opens on :help");
	n = hkCmdMatch(':', "compa", 5, m, CL_MENU_CAP);
	ck(n == 1 && hkCmdAt(m[0]).p == '/', "a /command is found from a : too, and says where it lives");
	char *js = hkCmdsJson();
	ckstr(js, "{\"n\":\"theme\",\"p\":\":\",\"c\":\"settings\",\"a\":\"[name]\",\"d\":\"pick a theme\"}", "the wire shape of a command");
	ckstr(js, "{\"n\":\"compact\",\"p\":\"/\"", "/compact is the model's");
	free(js);
	int ci = hkCmdFind("retry", 5);
	ck(ci >= 0 && !(hkCmdAt(ci).f & HK_CF_TURN), "/retry waits for the turn (it would start a second worker)");
	ci = hkCmdFind("theme", 5);
	ck(ci >= 0 && (hkCmdAt(ci).f & HK_CF_TURN), ":theme runs mid-turn");

	/* a write's body becomes a receipt once a later turn exists */
	char big[1200];
	memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = '\0';
	char prose[1500], raw[1500], esc[1600], chan[1500];
	snprintf(prose, sizeof prose, "Writing.\n<tool name=\"write_file\">\n{\"path\": \"a.py\", \"content\": \"%s\\n\"}\n</tool>", big);
	snprintf(raw, sizeof raw, "[{\"type\":\"text\",\"text\":\"ok\"},{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"edit_file\",\"input\":{\"path\":\"b.c\",\"old\":\"q\",\"new\":\"%s\"}}]", big);
	snprintf(esc, sizeof esc, "{\"content\":\"keep me\",\"tool_calls\":[{\"function\":{\"name\":\"write_file\",\"arguments\":\"{\\\"path\\\":\\\"c.txt\\\",\\\"content\\\":\\\"%s\\\"}\"}}]}", big);
	snprintf(chan, sizeof chan, "<write_file path=\"d.txt\">%s</write_file>", big);
	char *a = hkSlimOne(prose), *b = hkSlimOne(raw), *c = hkSlimOne(esc), *d = hkSlimOne(chan);
	ckstr(a, "\"content\": \"(wrote 1200 bytes to a.py)\"", "a prose write_file body becomes (wrote N bytes to path)");
	ckstr(b, "\"new\":\"(wrote 1199 bytes to b.c)\"", "a native edit_file body too, the JSON still whole");
	ckstr(b, "\"old\":\"q\"", "a short value is left alone");
	ckstr(c, "\\\"content\\\":\\\"(wrote 1199 bytes to c.txt)\\\"", "an escaped openai arguments body too");
	ckstr(c, "\"content\":\"keep me\"", "the reply's own text is never touched");
	ckstr(d, ">(wrote 1199 bytes)</write_file>", "the raw <write_file> channel too");
	ck(hkSlimOne("just talking about write_file") == NULL, "nothing to slim, nothing changed");
	free(a); free(b); free(c); free(d);

	/* what voice says: no marks, no code */
	char *v = hkVoiceText("# Title\n**bold** and `x` [link](http://e)\n```c\nint x;\n```\n- item | cell\n");
	ckstr(v, "Title bold and x link code skipped. item , cell", "voice reads the words, skips the code");
	cknot(v, "int x", "and never the code itself");
	free(v);

	ck(hkVersionNewer("v0.2.11", "0.2.10") && !hkVersionNewer("v0.2.10", "0.2.10") && !hkVersionNewer("0.1.9", "0.2.10")
	   && hkVersionNewer("1.0.0", "0.9.99"), "release tags compare by number");
}

/* --- the ENVIRONMENT block: found by walking PATH (it spawned a shell per name)
   and names only (it printed absolute paths into every prompt). --- */
static void env_probe_tests(void) {
	ck(hkOnPath("sh"), "hkOnPath finds sh");
	ck(!hkOnPath("hako-surely-not-a-real-command"), "hkOnPath rejects a name that is not installed");
	ck(!hkOnPath("/bin/sh"), "hkOnPath takes names, not paths");
	aiData d; memset(&d, 0, sizeof(d)); pthread_mutex_init(&d.lock, NULL);
	hkPromptInvalidate();
	hkLoadSkills(&d);
	const char *env = d.system_prompt ? strstr(d.system_prompt, "# ENVIRONMENT") : NULL;
	ck(env != NULL, "the prompt has an ENVIRONMENT block");
	if (env) {
		const char *end = strstr(env + 1, "\n#");
		size_t n = end ? (size_t)(end - env) : strlen(env);
		ck(memchr(env, '/', n) == NULL, "the ENVIRONMENT block carries no absolute paths");
	}
	free(d.system_prompt);
	pthread_mutex_destroy(&d.lock);
}

static void truncation_notice_tests(void) {
	char tmpl[] = "/tmp/hakotruncXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL trunc mkdtemp\n"); fails++; return; }
	char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof cwd)) return;
	if (chdir(dir) != 0) return;
	hkGrantProjectTrust();
	int saved_auto = E.ai_auto_approve; E.ai_auto_approve = 1;

	FILE *f = fopen("big.txt", "w");
	for (long i = 0; i < 30000; i++) fputs("0123456789\n", f);
	fclose(f);
	char *r = hkExecTool("read_file", "{\"path\":\"big.txt\"}");
	ck(r && strstr(r, "truncated") != NULL, "an over-long read_file says it was truncated");
	ck(r && strstr(r, "Do NOT rewrite the whole file") != NULL,
	   "and warns against rewriting the file from a partial read");
	free(r);

	f = fopen("small.txt", "w"); fputs("hi\n", f); fclose(f);
	r = hkExecTool("read_file", "{\"path\":\"small.txt\"}");
	ck(r && !strcmp(r, "hi\n"), "a small file is returned untouched");
	free(r);

	E.ai_auto_approve = saved_auto;
	if (chdir(cwd) != 0) { /* nothing useful to do */ }
}

static void catalog_tests(void) {
	enum aiProviderType saved_p = E.ai_provider_type;
	char *saved_m = E.ai_model;

	E.ai_provider_type = AI_PROVIDER_ANTHROPIC;
	struct { const char *model; long want; const char *what; } ctx[] = {
		{ "claude-opus-5",     1000000, "Opus 5 is 1M, not 200K" },
		{ "claude-sonnet-5",   1000000, "Sonnet 5 is 1M" },
		{ "claude-opus-4-7",   1000000, "Opus 4.7 is 1M" },
		{ "claude-haiku-4-5",   200000, "Haiku 4.5 is the 200K exception" },
		{ "claude-3-5-sonnet",  200000, "older models stay 200K" },
		{ NULL, 0, NULL }
	};
	for (int i = 0; ctx[i].model; i++) {
		E.ai_model = (char *)ctx[i].model;
		long got = hkContextWindow();
		if (got != ctx[i].want) {
			printf("FAIL %s (got %ld want %ld)\n", ctx[i].what, got, ctx[i].want);
			fails++;
		}
	}

	/* every id in the curated fallback must price, or :usage lies */
	struct { const char *model; double in; } price[] = {
		{ "claude-opus-5",    5.0 }, { "claude-sonnet-5",  2.0 },
		{ "claude-opus-4-8",  5.0 }, { "claude-haiku-4-5", 1.0 },
		{ "claude-fable-5",  10.0 }, { NULL, 0 }
	};
	for (int i = 0; price[i].model; i++) {
		double in = 0, out = 0;
		hkModelPriceUSDperM(price[i].model, &in, &out);
		if (in != price[i].in) {
			printf("FAIL price for %s (got %.2f want %.2f)\n", price[i].model, in, price[i].in);
			fails++;
		}
	}

	/* current ids are bare — a date suffix is an older convention */
	/* count-terminated, not NULL-terminated — bound it the way the code does */
	for (size_t i = 0; i < sizeof(HK_MODEL_SUGG) / sizeof(HK_MODEL_SUGG[0]); i++)
		if (!strcmp(HK_MODEL_SUGG[i].prov, "anthropic")) {
			ckstr(HK_MODEL_SUGG[i].models, "claude-opus-5", "Opus 5 is in the curated list");
			ckstr(HK_MODEL_SUGG[i].models, "claude-sonnet-5", "Sonnet 5 is in the curated list");
			cknot(HK_MODEL_SUGG[i].models, "-2025", "no date-suffixed ids in the curated list");
		}

	/* nous: a named openai-compat provider with its own default and a public catalog */
	char *saved_ep = E.ai_endpoint;
	E.ai_endpoint = NULL;
	hkApplyProviderAlias("nous");
	ck(E.ai_provider_type == AI_PROVIDER_OPENAI, "nous is openai-compatible");
	ck(E.ai_endpoint && !strcmp(E.ai_endpoint, "https://inference-api.nousresearch.com"), "nous endpoint");
	ck(!strcmp(hkProviderLabel(), "nous"), "the endpoint names nous");
	const char *dm = hkProviderDefaultModel(E.ai_provider_type);
	ck(dm && !strcmp(dm, "anthropic/claude-haiku-5.5"), "nous has its own default model, not gpt-4o-mini");
	ck(hkCatalogPublic(E.ai_endpoint), "nous lists models without a key");
	E.ai_oauth_provider = strdup("chatgpt"); E.ai_oauth_refresh = strdup("r");
	hkUsePastedKey("nous", "sk-test");
	ck(!E.ai_oauth_provider && !E.ai_oauth_refresh, "a pasted key drops an earlier sign-in");
	ck(!hkOAuthIs("chatgpt") && E.ai_api_key && !strcmp(E.ai_api_key, "sk-test"), "the nous key is the one sent");

	/* keys never reach curl's argv: headers go to a 0600 file */
	{
		char hp[64];
		char *rest = hkHeadersToFile("-H 'Authorization: Bearer sekrit' -H 'x-a: 1' --compressed", hp, sizeof hp);
		ck(rest && !strstr(rest, "sekrit") && strstr(rest, "-H @") && strstr(rest, "--compressed"), "headers leave the command line");
		struct stat hs;
		ck(hp[0] && stat(hp, &hs) == 0 && (hs.st_mode & 077) == 0, "the header file is private");
		char *body = hp[0] ? hkReadFileAll(hp, 4096) : NULL;
		ck(body && !strcmp(body, "Authorization: Bearer sekrit\nx-a: 1\n"), "one header per line, unquoted");
		free(body); free(rest);
		if (hp[0]) unlink(hp);
		hkHttpReq hr = { .method = "GET", .url = "https://x.test/v1/models", .headers = "-H 'x-api-key: sekrit'" };
		char bt[64], ht[64];
		char *cmd = hk_http_curl_cmd(&hr, bt, sizeof bt, ht);
		ck(cmd && !strstr(cmd, "sekrit"), "a provider request carries no key on argv");
		free(cmd);
		if (ht[0]) unlink(ht);
	}
	free(E.ai_endpoint);
	E.ai_endpoint = saved_ep;

	E.ai_provider_type = saved_p;
	E.ai_model = saved_m;
}

/* --- ask_user: the model can put a real choice in front of the user --- */
static void ask_user_tests(void) {
	char tmpl[] = "/tmp/hakoaskXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL ask mkdtemp\n"); fails++; return; }
	char cmd[512];
	if (chdir(dir)) {}

	/* not a tty here, so it announces and takes the first option rather than
	   hanging a scripted run */
	char *r = hkExecTool("ask_user",
		"{\"question\":\"Which approach?\",\"options\":\"1. rewrite it\\n2. patch it\"}");
	ckstr(r, "rewrite it", "numbering is stripped from the option text");
	/* with no tty nobody chose, and the result must not pretend otherwise */
	ckstr(r, "defaulted", "a headless run reports a default, not a choice");
	cknot(r, "the user chose", "it never claims a choice that was not made");
	free(r);

	/* alias other agents use */
	r = hkExecTool("AskUserQuestion",
		"{\"question\":\"Pick\",\"options\":\"- alpha\\n- beta\"}");
	ckstr(r, "alpha", "alias resolves and bullets are stripped");
	free(r);

	/* one option is not a choice */
	r = hkExecTool("ask_user", "{\"question\":\"Only one?\",\"options\":\"just this\"}");
	ckstr(r, "at least two options", "a single option is refused");
	free(r);

	r = hkExecTool("ask_user", "{\"options\":\"a\\nb\"}");
	ckstr(r, "a", "a missing question still works if options are given");
	free(r);

	r = hkExecTool("ask_user", "{}");
	ckstr(r, "error", "no question and no options is an error");
	free(r);

	/* it must be exempt from the approval gate: with no tty and no auto-approve,
	   every other tool is denied, and asking is the interaction itself */
	int saved = E.ai_auto_approve;
	E.ai_auto_approve = 0;
	r = hkExecTool("ask_user", "{\"question\":\"Q\",\"options\":\"x\\ny\"}");
	cknot(r, "denied", "ask_user is not blocked by the approval gate");
	free(r);
	E.ai_auto_approve = saved;

	/* the schema must advertise it in both wire formats */
	for (int fmt = 0; fmt < 2; fmt++) {
		char *sc = hkBuildToolsSchema(fmt);
		ckstr(sc, "\"ask_user\"", "ask_user is in the tool schema");
		free(sc);
	}

	if (chdir("/tmp") == 0) { snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); if (system(cmd)) {} }
}

/* --- the bench suite loader --- */
static void bench_tests(void) {
	char tmpl[] = "/tmp/hakobenchXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL bench mkdtemp\n"); fails++; return; }
	char path[PATH_MAX], cmd[512];
	snprintf(path, sizeof(path), "%s/suite.jsonl", dir);

	FILE *f = fopen(path, "w");
	fputs("{\"id\":\"a\",\"prompt\":\"find hkWalk\",\"tool\":\"grep\",\"key\":\"pattern\",\"value\":\"hkWalk\"}\n", f);
	fputs("\n", f);                                   /* blank lines are skipped */
	fputs("# a comment line, also skipped\n", f);
	fputs("{\"id\":\"b\",\"prompt\":\"say hi\",\"tool\":\"\"}\n", f);
	/* a properly escaped quote inside the value: the file gets \" , not \\" .
	   The old fixture wrote \\" — an escaped BACKSLASH followed by the real
	   closing quote — and only parsed "correctly" because the scanner was
	   broken in the matching way. */
	fputs("{\"id\":\"c\",\"prompt\":\"line with \\\"quotes\\\" in it\",\"tool\":\"read_file\"}\n", f);
	fclose(f);

	hkBenchTask *t = NULL;
	int n = hkBenchLoad(path, &t);
	ck(n == 3, "loader skips blanks and comments");
	if (n == 3) {
		ck(!strcmp(t[0].id, "a"), "id parses");
		ck(!strcmp(t[0].want_tool, "grep"), "expected tool parses");
		ck(!strcmp(t[0].want_val, "hkWalk"), "expected value parses");
		ck(t[1].want_tool[0] == '\0', "a restraint task has no expected tool");
		ckstr(t[2].prompt, "quotes", "escaped prompt text is unescaped");
	}
	free(t);

	/* a file with nothing usable must report that, not pretend to run */
	snprintf(path, sizeof(path), "%s/empty.jsonl", dir);
	f = fopen(path, "w"); fputs("\n\n", f); fclose(f);
	t = NULL;
	ck(hkBenchLoad(path, &t) == 0, "an empty suite loads zero tasks");
	free(t);

	t = NULL;
	ck(hkBenchLoad("/nonexistent/suite.jsonl", &t) < 0, "a missing suite is an error");
	free(t);

	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	if (system(cmd)) {}
}

/* --- an untrusted project must not be told it has tools ---
   Anthropic only receives the tools schema when the project is trusted, but the
   prose tool prompt used to go out regardless. The model then believes it can
   read and edit files, emits tool syntax that nothing executes, and the user
   watches turns go by with no output — which is what a first session on a new
   machine looked like. */
/* --- images: a typed path becomes [image #N], and the request carries it per wire --- */
static int count_of(const char *hay, const char *needle) {
	int n = 0;
	for (const char *p = hay; p && (p = strstr(p, needle)); p += strlen(needle)) n++;
	return n;
}

static void image_tests(void) {
	char tmpl[] = "/tmp/hakoimgXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL image mkdtemp\n"); fails++; return; }
	char path[PATH_MAX], line[PATH_MAX * 2], cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "/tmp");
	char *keep = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	char *keep_sid = E.session_id, *keep_oauth = E.ai_oauth_provider;
	int keep_prov = E.ai_provider_type;
	setenv("HOME", dir, 1);
	snprintf(path, sizeof path, "%s/proj", dir); mkdir(path, 0755);
	if (chdir(path)) {}
	E.session_id = "imgtest";
	E.ai_oauth_provider = NULL;

	static const char png[] = "\x89PNG\r\n\x1a\n" "pixels";
	static const char jpg[] = "\xff\xd8\xff\xe0" "jfif";
	snprintf(path, sizeof path, "%s/shots", dir); mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/shots/a b.png", dir); hk_fs_write(path, png, sizeof png - 1, 0);
	snprintf(path, sizeof path, "%s/shots/c.jpg", dir);   hk_fs_write(path, jpg, sizeof jpg - 1, 0);
	snprintf(path, sizeof path, "%s/shots/not.png", dir); hk_fs_write(path, "text", 4, 0);

	ck(hkImageKind((const unsigned char *)png, sizeof png - 1) == 0, "png bytes are a png");
	ck(hkImageKind((const unsigned char *)jpg, sizeof jpg - 1) == 1, "jpeg bytes are a jpeg");
	ck(hkImageKind((const unsigned char *)"RIFF0000WEBP", 12) == 3, "webp bytes are a webp");
	ck(hkImageKind((const unsigned char *)"text", 4) < 0, "text is not an image");

	aiData d;
	memset(&d, 0, sizeof(d));
	pthread_mutex_init(&d.lock, NULL);

	/* a dragged file: the terminal escapes its spaces */
	snprintf(line, sizeof line, "look at %s/shots/a\\ b.png now", dir);
	char *r = hkImagesFromLine(&d, line);
	ck(r && !strcmp(r, "look at [image #1] now"), "a dragged image path becomes [image #1]");
	free(r);
	ck(hkImageFind(1, path, sizeof path) == 0 && strstr(path, "/.hako/projects/") && strstr(path, "/images/imgtest-1.png"),
	   "the image is kept in the project's images dir, by session and number");
	snprintf(line, sizeof line, "compare '%s/shots/a b.png' and %s/shots/c.jpg.", dir, dir);
	r = hkImagesFromLine(&d, line);
	ck(r && !strcmp(r, "compare [image #2] and [image #3]."), "a quoted path and one before a full stop both attach");
	free(r);
	ck(hkImageFind(3, path, sizeof path) == 1, "a jpeg keeps its kind");
	snprintf(line, sizeof line, "fix %s/shots/missing.png and size.png and @nothing", dir);
	ck(hkImagesFromLine(&d, line) == NULL, "a missing file, a bare name and a stray @ change nothing");
	snprintf(line, sizeof line, "x=%s/shots/a\\ b.png", dir);
	ck(hkImagesFromLine(&d, line) == NULL, "a path glued to other text is not a path");

	/* the request: each wire's own image block, the words kept */
	aiPushMessage(&d, "user", "what is [image #1]?");
	char *b64 = clB64Encode((const unsigned char *)png, sizeof png - 1);
	E.ai_provider_type = AI_PROVIDER_ANTHROPIC;
	char *j = aiBuildMessagesJson(&d);
	ckstr(j, "{\"type\":\"text\",\"text\":\"what is [image #1]?\"},{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"", "anthropic gets a base64 image block after the words");
	ckstr(j, b64, "and the bytes are the image's");
	free(j);
	E.ai_provider_type = AI_PROVIDER_OPENAI;
	j = aiBuildMessagesJson(&d);
	ckstr(j, "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,", "openai-compatible gets an image_url data url");
	free(j);
	E.ai_oauth_provider = "chatgpt";
	j = aiBuildMessagesJson(&d);
	ckstr(j, "{\"type\":\"input_text\",\"text\":\"what is [image #1]?\"},{\"type\":\"input_image\",\"image_url\":\"data:image/png;base64,", "a ChatGPT sign-in gets input_image");
	free(j);
	E.ai_oauth_provider = NULL;
	E.ai_provider_type = AI_PROVIDER_OLLAMA;
	j = aiBuildMessagesJson(&d);
	ckstr(j, "\"content\":\"what is [image #1]?\",\"images\":[\"", "ollama gets an images array");
	free(j);
	E.ai_provider_type = AI_PROVIDER_MITHRAEUM;
	j = aiBuildMessagesJson(&d);
	ckstr(j, "\"content\":\"what is [image #1]?\"}", "the local engine gets the words only");
	cknot(j, b64, "and no image bytes");
	free(j);

	/* only the newest few ride along on every request */
	E.ai_provider_type = AI_PROVIDER_ANTHROPIC;
	for (int i = 0; i < 9; i++) aiPushMessage(&d, "user", "again [image #1]");
	j = aiBuildMessagesJson(&d);
	ck(count_of(j, "\"type\":\"image\"") == HK_IMAGE_KEEP, "only the newest HK_IMAGE_KEEP images are sent");
	free(j);
	free(b64);

	E.session_id = keep_sid;
	E.ai_oauth_provider = keep_oauth;
	E.ai_provider_type = keep_prov;
	if (keep) { setenv("HOME", keep, 1); free(keep); } else unsetenv("HOME");
	if (chdir(cwd)) {}
	snprintf(line, sizeof line, "rm -rf '%s'", dir);
	if (system(line)) {}
}

static void trust_prompt_tests(void) {
	char tmpl[] = "/tmp/hakotrustXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL trust mkdtemp\n"); fails++; return; }
	char cmd[512];
	char *keep = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", dir, 1);
	if (chdir(dir)) {}

	int saved_tools = E.ai_tools_enabled;
	E.ai_tools_enabled = 1;

	aiData d;
	memset(&d, 0, sizeof(d));
	pthread_mutex_init(&d.lock, NULL);

	/* untrusted: say so, and do not list a toolbox */
	ck(hkProjectTrusted() == 0, "a fresh directory is not trusted");
	hkLoadSkills(&d);
	ckstr(d.system_prompt, "NO tools in this session", "an untrusted project says tools are unavailable");
	ckstr(d.system_prompt, ":trust", "it names the command that fixes it");
	cknot(d.system_prompt, "<tool name=", "it does not teach tool syntax that cannot run");
	ckstr(d.system_prompt, "terminal where hako runs", "and says where, since a phone cannot run it");

	/* trusted mid-session: the built prompt is kept, so the grant itself must
	   mark it stale (`:trust` and the startup prompt never did, and the model
	   asked for :trust all session) */
	hkGrantProjectTrust();
	ck(hkProjectTrusted() == 1, "trust is granted");
	E.ai_toolmode = 1;                       /* force the prose prompt */
	hkLoadSkills(&d);                        /* what the next turn does */
	cknot(d.system_prompt, "NO tools in this session", "a trusted project drops the warning");
	ckstr(d.system_prompt, "<tool name=", "and teaches the tool syntax again");
	free(d.system_prompt);

	E.ai_toolmode = 0;
	E.ai_tools_enabled = saved_tools;
	pthread_mutex_destroy(&d.lock);
	if (keep) { setenv("HOME", keep, 1); free(keep); } else unsetenv("HOME");
	if (chdir("/tmp") == 0) { snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); if (system(cmd)) {} }
}

/* A tool result may take a quarter of the context window, and no more. */
static void fit_tests(void) {
	int saved = E.ai_provider_type;
	E.ai_provider_type = AI_PROVIDER_MITHRAEUM;
	setenv("HAKO_CTX", "4096", 1);
	ck(hkContextWindow() == 4096, "the local window is what the engine is started with (HAKO_CTX)");
	char *small = strdup("short result");
	ck(hkFitToolResult(small) == small, "a small result passes through untouched");
	free(small);
	size_t n = 20000;
	char *big = malloc(n + 1);
	for (size_t i = 0; i + 1 < n; i += 2) { big[i] = (char)0xC3; big[i + 1] = (char)0xA9; }   /* é é é ... */
	big[n] = '\0';
	char *fit = hkFitToolResult(big);
	const char *cut = strstr(fit, "\n[... cut");
	ck(cut != NULL, "a 20 KB result is cut, and says so");
	ck(cut && cut - fit <= 4096, "to a quarter of a 4K-token window");
	ck(cut && ((cut - fit) % 2) == 0, "on a character boundary, not inside one");
	ck(strstr(fit, "Narrow it down") != NULL, "and tells the model how to see the rest");
	free(fit);
	unsetenv("HAKO_CTX");
	E.ai_provider_type = saved;

	/* a prose call cut at its stop sequence gets its closing tag back */
	char *c = hkNormalizeToolDialect("Let me look.\n<tool name=\"list_dir\">\n{\"path\": \".\"}\n");
	ck(c && strstr(c, "</tool>"), "a call stopped at </tool> is closed again");
	ck(c && hkHasToolBlock(c), "and is seen as a tool call");
	free(c);
	c = hkNormalizeToolDialect("<tool name=\"list_dir\">\n{\"path\": \".\"}\n</tool>\n");
	ck(!c || strstr(strstr(c, "</tool>") + 1, "</tool>") == NULL, "a closed call is not closed twice");
	free(c);
	c = hkNormalizeToolDialect("I can use <tool name=\"x\"> tags in prose");
	ck(!c || !strstr(c, "</tool>"), "prose that names the tag without a call is left alone");
	free(c);
}

/* --- crypto: monocypher against RFC vectors, noise against cacophony --- */
static size_t unhex(uint8_t *out, const char *h) {
	size_t n = 0;
	for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); out[n++] = (uint8_t)v; }
	return n;
}
static int eqhex(const uint8_t *b, size_t n, const char *h) {
	uint8_t w[512];
	return unhex(w, h) == n && memcmp(w, b, n) == 0;
}

typedef struct { const char *payload, *cipher; } hkTvMsg;
typedef struct { const char *pattern, *prologue, *is, *ie, *rs, *re, *hash; int nmsg; hkTvMsg m[8]; const char *psk; } hkTv;
static const hkTv noise_vectors[] = {
	{ "XX", "4a6f686e2047616c74", "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1", "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a", "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893", "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b", "8cf47d7b3cb5804c0109d48e8bcdbee2cbb65687d8ea2c92994ca361fb86151ad93627b98936cbb32de56e8abb21def3925011ac3e35db9cbeea73ab9a4392c2", 6, {
		{ "4c756477696720766f6e204d69736573", "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79444c756477696720766f6e204d69736573" },
		{ "4d757272617920526f746862617264", "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f1448088430505b6745ce64a5f33f0e8e3b83f11ce8802bca507f4f2d8b564dbe277e1966116e132faa2dfd70b8b077b9f94b913df5056ae1319469b824a98d54bbaa82c325595587064f978c4b6d104f7596e6f" },
		{ "462e20412e20486179656b", "99579e1c1ee15e422a57ddd6b16d37087b17558e8369c18991b4b2ca3a824abf904cdcf5458b5431a75af034ca9e9b982de039eaaf156775e2d580cd4e5ebae89c3f8cb2594b556d8a8169" },
		{ "4361726c204d656e676572", "fc56eea290b3f3a21aac0c70cd5787b5ee99be37d2f4d751329b55" },
		{ "4a65616e2d426170746973746520536179", "bb31c9da10d5639a4cdb88a12f5c61de41bbc7df09bf75d94f8184fe4157f5c68f" },
		{ "457567656e2042f6686d20766f6e2042617765726b", "f6199cadb152fb27f82be0a0891ec76a33598ae92a46cab2fb5a8ed5bf48b7f267f8370af7" } }, NULL },
	{ "XXpsk3", "4a6f686e2047616c74", "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1", "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a", "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893", "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b", "46b5997dfa51492ae9be5b611748455e83aebdbc2ea3c47d264ec6dd408cc076e962ff056890c60398fe3e7a430bbe057f34383a84df9ff99391266dcfd638b0", 6, {
		{ "4c756477696720766f6e204d69736573", "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79440aa831d1e3bbd16bd7ffda87e9f7b6eeca2510006499a573ac27af3392ed1b69" },
		{ "4d757272617920526f746862617264", "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f14480884381a78489d56c9e2ac962826f25684980b25244dbfd505b7d903a94d4f4fd84004a1c7fbe94d686bc4c1f67259364f6f2a41cb40bf5198b9b3975be2cdd49d3e065a554e12c477d82029fabc6555bc1" },
		{ "462e20412e20486179656b", "291dfeff6eff386e29adb0bdbd5a1ae9e09de4449c8695d109adcb63e80e489555792a3e9150eff75d6d45571f824ed231173cb66e31e7da914241f86d0cbb678335ffe768a7445d2f5bc1" },
		{ "4361726c204d656e676572", "6579dd60eb5e6a8b29dd60004289f5d3c2f69fcdbf683a869f59a8" },
		{ "4a65616e2d426170746973746520536179", "24715fd39c3c7606e81b26e69e47b54d043d9d87c512f9417bae51ab48ebb17722" },
		{ "457567656e2042f6686d20766f6e2042617765726b", "e49194a013d873712318643acbf0e3851086326bb168c500a766439ff543a7a1e2deb3215c" } },
		"54686973206973206d7920417573747269616e20706572737065637469766521" },
};


static void noise_vector(const hkTv *tv) {
	hkNoise I, R;
	uint8_t pro[64], buf[1024], pl[1024];
	char what[96];
	memset(&I, 0, sizeof I); memset(&R, 0, sizeof R);
	size_t pn = unhex(pro, tv->prologue);
	unhex(I.s, tv->is); I.has_s = 1; unhex(I.e, tv->ie); I.has_e = 1;
	unhex(R.s, tv->rs); R.has_s = 1; unhex(R.e, tv->re); R.has_e = 1;
	if (tv->psk) { unhex(I.psk, tv->psk); unhex(R.psk, tv->psk); I.has_psk = R.has_psk = 1; }
	ck(hkNoiseInit(&I, tv->pattern, 1, pro, pn) == 0 && hkNoiseInit(&R, tv->pattern, 0, pro, pn) == 0, "noise init");
	hkNoiseCipher itx, irx, rtx, rrx;
	int split = 0;
	for (int i = 0; i < tv->nmsg; i++) {
		hkNoise *snd = (i & 1) ? &R : &I, *rcv = (i & 1) ? &I : &R;
		size_t n = unhex(pl, tv->m[i].payload);
		long cl, got;
		if (!split) {
			cl = hkNoiseWrite(snd, pl, n, buf, sizeof buf);
			got = cl < 0 ? -1 : hkNoiseRead(rcv, buf, (size_t)cl, pl, sizeof pl);
		} else {
			cl = (long)n + 16;
			hkNoiseSeal((i & 1) ? &rtx : &itx, NULL, 0, pl, n, buf);
			got = hkNoiseOpen((i & 1) ? &irx : &rrx, NULL, 0, buf, (size_t)cl, pl);
		}
		snprintf(what, sizeof what, "noise %s message %d ciphertext", tv->pattern, i);
		ck(cl > 0 && eqhex(buf, (size_t)cl, tv->m[i].cipher), what);
		snprintf(what, sizeof what, "noise %s message %d payload", tv->pattern, i);
		ck(got == (long)n && eqhex(pl, n, tv->m[i].payload), what);
		if (!split && hkNoiseDone(&I) && hkNoiseDone(&R)) {
			snprintf(what, sizeof what, "noise %s handshake hash", tv->pattern);
			ck(eqhex(I.h, 64, tv->hash) && eqhex(R.h, 64, tv->hash), what);
			hkNoiseSplit(&I, &itx, &irx);
			hkNoiseSplit(&R, &rtx, &rrx);
			split = 1;
		}
	}
	snprintf(what, sizeof what, "noise %s completes", tv->pattern);
	ck(split, what);
}

/* A pasted code or key: whitespace from a wrapped, indented line is dropped,
   and only something token-shaped is taken off the clipboard. */
static void paste_tests(void) {
	char b[96];
	snprintf(b, sizeof b, "  abc123\n   def#456 \t\r\n");
	hkSqueezeSpace(b);
	ck(!strcmp(b, "abc123def#456"), "a wrapped, indented paste comes back as one code");
	ck(hkLooksLikeToken("sk-ant-api03-abcdefghijklmnop", 29), "an API key is token-shaped");
	ck(hkLooksLikeToken("Xy12abcdefghij#state0123456789", 30), "a <code>#<state> is token-shaped");
	ck(!hkLooksLikeToken("https://example.com/abcdefghijkl", 32), "a link is not taken as a key");
	ck(!hkLooksLikeToken("two words, both long enough", 27), "a sentence is not taken as a key");
	ck(!hkLooksLikeToken("short", 5), "a short word is not taken as a key");
}

/* :remote pair's QR. Every version was decoded by zxing when this was written;
   here: what a reader looks for, and that the half-block lines the terminal
   prints carry exactly the modules. Then the seal its answer travels in. */
static void qr_tests(void) {
	static hkQr q;
	const char *url = "https://hako.mithraeum.studio/#pair=0123456789abcdef0123456789abcdef."
	                  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	char big[215];
	memset(big, 'x', 214);
	big[214] = '\0';
	ck(!hkQrEncode(&q, big), "qr: past version 10 is refused");
	ck(hkQrEncode(&q, "A") == 21, "qr: one byte is version 1");
	ck(q.m[0][0] && q.m[3][3] && !q.m[1][1] && q.m[0][20] && q.m[20][0] && !q.m[7][7], "qr: finder patterns");
	ck(q.m[13][8], "qr: the dark module");
	int ok = 1;
	for (int i = 8; i < 13; i++) ok &= q.m[6][i] == (i % 2 == 0) && q.m[i][6] == (i % 2 == 0);
	ck(ok, "qr: timing patterns");
	ck(hkQrEncode(&q, "https://hako.mithraeum.studio/#link="
	                  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef."
	                  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 53, "qr: a link url is version 9");
	ck(hkQrEncode(&q, url) == 49, "qr: a pair url is version 8");
	if (!G_AI.history) clInitAI(&G_AI);       /* the lines land in the history, as on screen */
	aiClearHistory(&G_AI);
	ck(hkQrShow(&G_AI, url), "qr: shows");
	int lines = G_AI.history_count;
	ok = lines == (q.n + 8 + 1) / 2;
	for (int l = 0; l < lines && ok; l++) {
		const unsigned char *c = (const unsigned char *)G_AI.history[l];
		int x = -4, y = -4 + 2 * l;
		while (*c && ok) {
			if (*c == 27) { while (*c && *c != 'm') c++; if (*c) c++; continue; }
			int top = 0, bot = 0;
			if (*c == ' ') c++;
			else if (c[0] == 0xe2 && c[1] == 0x96) { top = c[2] == 0x88 || c[2] == 0x80; bot = c[2] == 0x88 || c[2] == 0x84; c += 3; }
			else { ok = 0; break; }
			int wt = x >= 0 && x < q.n && y >= 0 && y < q.n && q.m[y][x];
			int wb = x >= 0 && x < q.n && y + 1 >= 0 && y + 1 < q.n && q.m[y + 1][x];
			ok &= top == wt && bot == wb;
			x++;
		}
		ok &= x == q.n + 4;
	}
	ck(ok, "qr: the terminal lines carry the modules, quiet zone included");
	aiClearHistory(&G_AI);
	cl_cols_pinned = q.n + 7;
	ck(!hkQrShow(&G_AI, url) && G_AI.history_count == 0, "qr: a terminal too narrow for it gets no half code");
	cl_cols_pinned = 0;
	const char *ssh = getenv("SSH_TTY");
	char keep[256];
	snprintf(keep, sizeof keep, "%s", ssh ? ssh : "");
	setenv("SSH_TTY", "/dev/pts/0", 1);
	ck(!hkHasDesktop(), "open url: not over ssh (the page would open on the far screen)");
	if (ssh) setenv("SSH_TTY", keep, 1); else unsetenv("SSH_TTY");
	uint8_t sec[32], pt[48], m[88], back[48];
	for (int i = 0; i < 32; i++) sec[i] = (uint8_t)i;
	for (int i = 0; i < 48; i++) pt[i] = (uint8_t)(200 - i);
	ck(!hkLinkSeal(m, sec, pt) && !hkLinkOpen(back, sec, m) && !memcmp(back, pt, 48), "link seal: opens with its secret");
	m[50] ^= 1;
	ck(hkLinkOpen(back, sec, m) != 0, "link seal: a changed byte is refused");
	m[50] ^= 1;
	sec[0] ^= 1;
	ck(hkLinkOpen(back, sec, m) != 0, "link seal: another secret is refused");
}

static void crypto_tests(void) {
	uint8_t a[32], u[32], out[64];
	/* RFC 7748 5.2 */
	unhex(a, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
	unhex(u, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
	crypto_x25519(out, a, u);
	ck(eqhex(out, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"), "x25519 rfc7748");
	/* BLAKE2b-512("abc"), RFC 7693 appendix A */
	crypto_blake2b(out, 64, (const uint8_t *)"abc", 3);
	ck(eqhex(out, 64, "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
	                  "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"), "blake2b abc");
	/* RFC 8439 2.8.2 */
	{
		static const char pt[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
		                         "for the future, sunscreen would be it.";
		uint8_t key[32], nonce[12], ad[12], ct[sizeof pt], mac[16];
		unhex(key, "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
		unhex(nonce, "070000004041424344454647");
		unhex(ad, "50515253c0c1c2c3c4c5c6c7");
		crypto_aead_ctx c;
		crypto_aead_init_ietf(&c, key, nonce);
		crypto_aead_write(&c, ct, mac, ad, 12, (const uint8_t *)pt, sizeof pt - 1);
		ck(eqhex(mac, 16, "1ae10b594f09e26a7e902ecbd0600691"), "chacha20-poly1305 rfc8439 tag");
		ck(eqhex(ct, 16, "d31a8d34648e60db7b86afbc53ef7ec2"), "chacha20-poly1305 rfc8439 ciphertext");
	}
	for (size_t i = 0; i < sizeof noise_vectors / sizeof noise_vectors[0]; i++) noise_vector(&noise_vectors[i]);

	/* a flipped bit anywhere is refused */
	{
		hkNoiseCipher c1 = { {1}, 0, 1 }, c2 = c1;
		uint8_t m[21], pl[5];
		hkNoiseSeal(&c1, NULL, 0, (const uint8_t *)"hello", 5, m);
		m[3] ^= 1;
		ck(hkNoiseOpen(&c2, NULL, 0, m, sizeof m, pl) < 0, "noise refuses a tampered message");
		ck(c2.n == 0, "and does not advance the nonce");
	}
	/* a low-order point as the peer's key fails the handshake */
	{
		hkNoise R;
		uint8_t m[64] = {0}, pl[64];
		memset(&R, 0, sizeof R);
		hkRandomBytes(R.s, 32); R.has_s = 1;
		hkNoiseInit(&R, "XX", 0, NULL, 0);
		ck(hkNoiseRead(&R, m, 32, pl, sizeof pl) == 0, "an all-zero ephemeral is read");
		ck(hkNoiseWrite(&R, NULL, 0, m, sizeof m) < 0, "and the DH with it is refused");
	}
	/* the link check: both ends agree, a different nonce changes the digits */
	{
		uint8_t h[64] = {7}, ni[32] = {1}, nr[32] = {2}, c1[32], c2[32];
		char d1[7], d2[7], d3[7];
		hkLinkDigits(d1, h, ni, nr); hkLinkDigits(d2, h, ni, nr);
		nr[0] = 3; hkLinkDigits(d3, h, ni, nr);
		ck(strlen(d1) == 6 && !strcmp(d1, d2), "link digits: six, and stable");
		ck(strcmp(d1, d3) != 0, "link digits change with the nonce");
		hkLinkCommit(c1, nr); nr[0] = 2; hkLinkCommit(c2, nr);
		ck(memcmp(c1, c2, 32) != 0, "link commit binds the nonce");
	}
}

/* --- relay: two devices through a running relay (make test-relay) --- */
static int relay_status(const char *url, const char *extra) {
	char cmd[512];
	snprintf(cmd, sizeof cmd, "curl -s -o /dev/null -w '%%{http_code}' %s '%s' 2>/dev/null", extra, url);
	FILE *fp = popen(cmd, "r");
	int code = 0;
	if (fp) { if (fscanf(fp, "%d", &code) != 1) code = 0; pclose(fp); }
	return code;
}

static long relay_recv_from(hkRelay *r, const char *want, uint8_t *buf, size_t cap) {
	char from[33];
	long n;
	while ((n = hkRelayRecv(r, from, buf, cap)) == -2) {}
	if (n >= 0 && strcmp(from, want)) return -3;
	return n;
}

static void relay_timeout(int sig) {
	(void)sig;
	static const char m[] = "FAIL relay: timed out waiting for a frame\n";
	if (write(1, m, sizeof m - 1) < 0) {}
	_exit(1);
}

/* XXpsk3 between I (on a) and R (on b), the psk from each side's K. Returns 1
   when both finish; the transport ciphers land in itx/irx, rtx/rrx. */
static int relay_session(hkRelay *a, hkRelay *b, const uint8_t ka[32], const uint8_t kb[32],
                         hkNoiseCipher *itx, hkNoiseCipher *irx, hkNoiseCipher *rtx, hkNoiseCipher *rrx) {
	static uint8_t m[4096], pl[4096];
	hkNoise I, R;
	memset(&I, 0, sizeof I); memset(&R, 0, sizeof R);
	memcpy(I.s, a->s, 32); I.has_s = 1; memcpy(R.s, b->s, 32); R.has_s = 1;
	hkSessionPsk(I.psk, ka); hkSessionPsk(R.psk, kb); I.has_psk = R.has_psk = 1;
	hkNoiseInit(&I, "XXpsk3", 1, (const uint8_t *)"hako session", 12);
	hkNoiseInit(&R, "XXpsk3", 0, (const uint8_t *)"hako session", 12);
	for (int i = 0; i < 3; i++) {
		hkRelay *from = (i & 1) ? b : a, *to = (i & 1) ? a : b;
		long n = hkNoiseWrite((i & 1) ? &R : &I, NULL, 0, m, sizeof m);
		if (n < 0 || hkRelaySend(from, to->id, m, (size_t)n)) return 0;
		n = relay_recv_from(to, from->id, m, sizeof m);
		if (n < 0 || hkNoiseRead((i & 1) ? &I : &R, m, (size_t)n, pl, sizeof pl) < 0) return 0;
	}
	if (!hkNoiseDone(&I) || !hkNoiseDone(&R) || memcmp(I.rs, b->s_pub, 32) || memcmp(R.rs, a->s_pub, 32)) return 0;
	hkNoiseSplit(&I, itx, irx); hkNoiseSplit(&R, rtx, rrx);
	return 1;
}

/* invite and log come from `make test-relay`: one invite code, and the --dev
   relay's log, where it prints the email code. */
static void relay_tests(const char *url, const char *invite, const char *log) {
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGPIPE, SIG_IGN);                     /* as hako's main does */
	signal(SIGALRM, relay_timeout);
	alarm(30);                                    /* a lost frame must fail, not hang */
	hkRelay A, B;
	memset(&A, 0, sizeof A); memset(&B, 0, sizeof B);
	snprintf(A.url, sizeof A.url, "%s", url); snprintf(B.url, sizeof B.url, "%s", url);
	hkRandomBytes(A.s, 32); hkRandomBytes(B.s, 32);
	ck(hkRelaySignIn(&A) == 0 && hkRelaySignIn(&B) == 0, "relay: both devices sign in");
	ck(strlen(A.id) == 32 && strcmp(A.id, B.id), "relay: ids are distinct hashes of the keys");
	ck(!A.acct[0] && !B.acct[0], "relay: neither is on an account yet");

	/* A is the page: signs in by email with the invite, then binds B (the computer) */
	char *r = hkRelayCall(&A, "/v1/email/start", "me@example.com");
	ck(r && !strcmp(r, "sent\n"), "relay: the page asks for an email code");
	free(r);
	char code[8] = "", line[256], body[96];
	FILE *lf = fopen(log, "r");
	while (lf && fgets(line, sizeof line, lf)) { const char *c = strstr(line, "sign-in code "); if (c) snprintf(code, sizeof code, "%.6s", c + 13); }
	if (lf) fclose(lf);
	snprintf(body, sizeof body, "%s %s", code, invite);
	r = hkRelayCall(&A, "/v1/email/verify", body);
	ck(r && strlen(r) == 21 && !strcmp(r + 16, " new\n"), "relay: code + invite make the account");
	free(r);
	char pubh[66];
	hkHexEnc(pubh, B.s_pub, 32);
	r = hkRelayCall(&A, "/v1/bind", pubh);
	ck(r && !strcmp(r, "ok\n"), "relay: the page binds the computer's key");
	free(r);
	ck(hkRelaySignIn(&A) == 0 && hkRelaySignIn(&B) == 0 && A.acct[0] && !strcmp(A.acct, B.acct),
	   "relay: both now sign in to the same account");
	ck(hkRelayOpen(&A) == 0 && hkRelayOpen(&B) == 0, "relay: streams open");
	usleep(400000);

	/* a session: XXpsk3, the psk from the account key */
	uint8_t K[32], K2[32], m[70000], pl[70000];
	long n;
	hkRandomBytes(K, 32);
	memcpy(K2, K, 32); K2[0] ^= 1;
	hkNoiseCipher itx, irx, rtx, rrx;
	/* the psk enters at message 3, so the wrong key fails there and nothing is left queued */
	ck(!relay_session(&A, &B, K, K2, &itx, &irx, &rtx, &rrx), "relay: a different account key cannot open a session");
	ck(relay_session(&A, &B, K, K, &itx, &irx, &rtx, &rrx), "relay: the same account key opens one, each side knows the other");

	/* a big frame both ways */
	for (size_t i = 0; i < 60000; i++) pl[i] = (uint8_t)(i * 7);
	hkNoiseSeal(&itx, NULL, 0, pl, 60000, m);
	hkRelaySend(&A, B.id, m, 60016);
	n = relay_recv_from(&B, A.id, m, sizeof m);
	ck(n == 60016 && hkNoiseOpen(&rrx, NULL, 0, m, 60016, pl) == 60000 && pl[59999] == (uint8_t)(59999 * 7),
	   "relay: a 60 KB frame crosses intact");

	/* the relay says when nobody is there */
	{
		char from[33];
		uint8_t z[1] = {0};
		hkRelaySend(&A, "0123456789abcdef0123456789abcdef", z, 1);
		ck(hkRelayRecv(&A, from, m, sizeof m) == -2 && !strcmp(from, "0123456789abcdef0123456789abcdef"),
		   "relay: a frame for an offline device comes back as offline");
	}

	/* the gates */
	char u[320];
	snprintf(u, sizeof u, "%s/v1/down", url);
	ck(relay_status(u, "--max-time 3") == 401, "relay: no token, no stream");
	ck(relay_status(u, "--max-time 3 -H 'Authorization: Bearer 00000000000000000000000000000000"
	                   "00000000000000000000000000000000'") == 401, "relay: a made-up token, no stream");
	{
		char pubh[66], body[128];
		hkHexEnc(pubh, A.s_pub, 32);
		snprintf(u, sizeof u, "%s/v1/auth/challenge", url);
		char *c = hkRelayPost(u, pubh);
		ck(c && strlen(c) >= 97, "relay: a challenge is issued");
		if (c && strlen(c) >= 97) {
			snprintf(body, sizeof body, "%.32s %064d", c, 0);
			snprintf(u, sizeof u, "%s/v1/auth/prove", url);
			char *t = hkRelayPost(u, body);
			ck(t && !strncmp(t, "no", 2), "relay: a wrong proof is refused");
			free(t);
			t = hkRelayPost(u, body);
			ck(t && !strncmp(t, "no", 2), "relay: and the challenge is spent");
			free(t);
		}
		free(c);
	}
	/* the desktop: :remote on over the relay. A plays the page (links it) and
	   then the phone (a session, one HTTP request through a stream). */
	{
		hkRelayClose(&B);
		char home[] = "/tmp/hakorelayhomeXXXXXX";
		if (mkdtemp(home)) {
			char hd[64];
			snprintf(hd, sizeof hd, "%s/.hako", home);
			mkdir(hd, 0700);
			setenv("HOME", home, 1);
		}
		setenv("HAKO_RELAY_URL", url, 1);
		setenv("HAKO_NO_BROWSER", "1", 1);
		clInitAI(&G_AI);                          /* as main does; the link prints into it */
		hsAttachSetPort(18797);
		ck(hsAttachOn() && hrStart(), "desktop: remote on starts the relay link");
		for (int i = 0; i < 50 && !hr_linking; i++) usleep(100000);
		ck(hr_linking && !hr_has_k, "desktop: not linked, so it shows the link page");
		hkHexEnc(pubh, hrm.s_pub, 32);
		r = hkRelayCall(&A, "/v1/bind", pubh);
		ck(r && !strcmp(r, "ok\n"), "desktop: the page binds it");
		free(r);
		uint8_t lk[32], pt[48], fr[1 + 88];
		crypto_blake2b_keyed(lk, 32, hr_secret, 32, (const uint8_t *)"hako link key", 13);
		memcpy(pt, K, 32); memcpy(pt + 32, A.acct, 16);
		fr[0] = HR_LINK;
		hkRandomBytes(fr + 1, 24);
		crypto_aead_lock(fr + 41, fr + 25, lk, fr + 1, NULL, 0, pt, 48);
		uint8_t bad[sizeof fr];
		memcpy(bad, fr, sizeof fr); bad[50] ^= 1;
		char mid[33];
		hkRelayDeviceId(mid, hrm.s_pub);
		hkRelaySend(&A, mid, bad, sizeof bad);
		usleep(300000);
		ck(!hr_has_k, "desktop: a tampered link frame is ignored");
		hkRelaySend(&A, mid, fr, sizeof fr);
		for (int i = 0; i < 50 && !hr_has_k; i++) usleep(100000);
		for (int i = 0; i < 50 && !(hr.acct[0] && hr_cur == &hr); i++) usleep(100000);
		ck(hr.acct[0] && !strcmp(hr.acct, A.acct) && strcmp(hr.id, mid), "desktop: this folder gets its own key, bound by the computer's");
		usleep(300000);
		ck(hr_has_k && !memcmp(hr_k, K, 32) && !strcmp(hr_acct, A.acct), "desktop: linked, the account key arrives");
		{
			char rp[PATH_MAX];
			hrPath(rp, sizeof rp);
			struct stat st;
			ck(stat(rp, &st) == 0 && (st.st_mode & 077) == 0, "desktop: ~/.hako/remote is private");
		}

		/* the phone's session */
		hkNoise I;
		memset(&I, 0, sizeof I);
		memcpy(I.s, A.s, 32); I.has_s = 1;
		hkSessionPsk(I.psk, K); I.has_psk = 1;
		hkNoiseInit(&I, "XXpsk3", 1, (const uint8_t *)"hako session", 12);
		m[0] = HR_HS;
		n = hkNoiseWrite(&I, NULL, 0, m + 1, sizeof m - 1);
		hkRelaySend(&A, hr.id, m, (size_t)n + 1);
		n = relay_recv_from(&A, hr.id, m, sizeof m);
		ck(n > 1 && m[0] == HR_HS && hkNoiseRead(&I, m + 1, (size_t)n - 1, pl, sizeof pl) >= 0, "desktop: answers the session");
		n = hkNoiseWrite(&I, NULL, 0, m + 1, sizeof m - 1);
		hkRelaySend(&A, hr.id, m, (size_t)n + 1);
		hkNoiseCipher ptx, prx;
		hkNoiseSplit(&I, &ptx, &prx);
		static const char rq[] = "GET /api/state HTTP/1.1\r\nHost: relay\r\n\r\n";
		uint8_t plain[128];
		plain[0] = plain[1] = plain[2] = 0; plain[3] = 7; plain[4] = HR_DATA;
		memcpy(plain + 5, rq, sizeof rq - 1);
		m[0] = HR_TP;
		hkNoiseSeal(&ptx, NULL, 0, plain, 5 + sizeof rq - 1, m + 1);
		hkRelaySend(&A, hr.id, m, 1 + 5 + sizeof rq - 1 + 16);
		static char resp[65536];
		size_t rl = 0;
		int eof = 0;
		while (!eof && (n = relay_recv_from(&A, hr.id, m, sizeof m)) > 1) {
			long l = m[0] == HR_TP ? hkNoiseOpen(&prx, NULL, 0, m + 1, (size_t)n - 1, pl) : -1;
			if (l < 5 || pl[3] != 7) break;
			if (pl[4] == HR_EOF) eof = 1;
			else if (rl + (size_t)l - 5 < sizeof resp) { memcpy(resp + rl, pl + 5, (size_t)l - 5); rl += (size_t)l - 5; }
		}
		resp[rl] = '\0';
		ck(eof && !strncmp(resp, "HTTP/1.1 200", 12) && strstr(resp, "\"version\""),
		   "desktop: /api/state answers through the encrypted stream, no token needed");
		hsAttachOff();
		ck(!hr_on, "desktop: remote off ends the link");
	}
	hkRelayClose(&A);
	alarm(0);
}

int main(int argc, char **argv) {
	if (argc == 5 && !strcmp(argv[1], "relay")) {
		relay_tests(argv[2], argv[3], argv[4]);
		printf(fails ? "%d FAILURES\n" : "relay tests passed\n", fails);
		return fails ? 1 : 0;
	}
	/* A throwaway HOME: the tests grant project trust and write state, and doing
	   that under the real one left a ~/.hako/projects entry per temp dir, forever. */
	char home_tmpl[] = "/tmp/hakotesthomeXXXXXX";
	char *test_home = mkdtemp(home_tmpl);
	if (test_home) setenv("HOME", test_home, 1);
	setenv("HAKO_UPDATE_CHECK", "0", 1);   /* no GitHub calls from a test */
	/* --- matcher --- */
	struct { const char *pat, *str; int want; } m[] = {
		{"*.c",        "hako.c",        1},
		{"*.c",        "a/hako.c",      0},   /* '*' must not cross a slash */
		{"**/*.c",     "a/b/hako.c",    1},
		{"**/*.c",     "hako.c",        1},   /* leading ** matches zero dirs */
		{"src/*.c",    "src/x.c",       1},
		{"src/*.c",    "src/d/x.c",     0},
		{"?ako.c",     "hako.c",        1},
		{"?ako.c",     "hhako.c",       0},
		{"*",          "x",             1},
		{"*",          "a/x",           0},
		{"**",         "a/x",           1},
		{"hako.c",     "hako.c",        1},
		{"hako.c",     "hako.cc",       0},
		{"*test*",     "my_test_file",  1},
		{NULL, NULL, 0}
	};
	for (int i = 0; m[i].pat; i++) {
		int got = hkGlobMatch(m[i].pat, m[i].str);
		if (got != m[i].want) {
			printf("FAIL glob %s vs %s -> %d want %d\n", m[i].pat, m[i].str, got, m[i].want);
			fails++;
		}
	}
	/* basename rule: a pattern with no '/' matches the file name alone */
	ck(hkPathMatch("*.c", "deep/nested/hako.c") == 1, "hkPathMatch basename");
	ck(hkPathMatch("src/*.c", "deep/hako.c") == 0, "hkPathMatch rooted");

	/* --- a real tree --- */
	char tmpl[] = "/tmp/hakotestXXXXXX";
	char *root = mkdtemp(tmpl);
	ck(root != NULL, "mkdtemp");
	if (!root) return 1;
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
		"mkdir -p %s/src %s/node_modules/junk %s/.git/objects && "
		"printf 'int alpha(void);\\nint beta(void);\\n' > %s/src/a.c && "
		"printf 'void alpha(void){}\\n' > %s/src/b.c && "
		"printf 'ALPHA in caps\\n' > %s/README.md && "
		"printf 'alpha should not be found\\n' > %s/node_modules/junk/skip.c && "
		"printf 'alpha in git\\n' > %s/.git/objects/skip.c && "
		"printf 'bin\\0\\0alpha\\n' > %s/blob.bin",
		root, root, root, root, root, root, root, root, root);
	ck(system(cmd) == 0, "fixture");
	ck(chdir(root) == 0, "chdir");

	char *g = hkToolGlob("{\"pattern\":\"*.c\"}");
	ckstr(g, "src/a.c", "glob finds src/a.c");
	ckstr(g, "src/b.c", "glob finds src/b.c");
	ck(strstr(g, "node_modules") == NULL, "glob skips node_modules");
	ck(strstr(g, ".git") == NULL, "glob skips .git");
	free(g);

	char *r = hkToolGrep("{\"pattern\":\"alpha\"}");
	ckstr(r, "src/a.c:1:", "grep hits a.c line 1");
	ckstr(r, "src/b.c:1:", "grep hits b.c line 1");
	ck(strstr(r, "node_modules") == NULL, "grep skips node_modules");
	ck(strstr(r, "blob.bin") == NULL, "grep skips binary");
	ck(strstr(r, "README.md") == NULL, "grep is case-sensitive by default");
	free(r);

	r = hkToolGrep("{\"pattern\":\"alpha\",\"ignore_case\":1}");
	ckstr(r, "README.md:1:", "grep ignore_case finds ALPHA");
	free(r);

	r = hkToolGrep("{\"pattern\":\"caps\",\"glob\":\"*.md\"}");
	ckstr(r, "README.md", "grep glob filter");
	ck(strstr(r, "src/a.c") == NULL, "grep glob filter excludes .c");
	free(r);

	r = hkToolGrep("{\"pattern\":\"al*ha\"}");
	ckstr(r, "src/a.c", "grep wildcard pattern");
	free(r);

	r = hkToolGrep("{\"pattern\":\"zzzznothing\"}");
	ckstr(r, "no matches", "grep miss reports cleanly");
	free(r);

	/* path jail: a multi-segment absolute path resolves outside and is refused;
	   a single-segment one is re-rooted into the project by hkResolveInProject,
	   which is contained, so it must simply report that it is not there. */
	r = hkToolGrep("{\"pattern\":\"x\",\"path\":\"/etc/ssl\"}");
	ckstr(r, "outside project", "grep refuses an absolute path");
	free(r);
	r = hkToolGrep("{\"pattern\":\"x\",\"path\":\"nope\"}");
	ckstr(r, "not a directory", "grep names a missing directory");
	free(r);
	char *g2 = hkToolGlob("{\"pattern\":\"*\",\"path\":\"nope\"}");
	ckstr(g2, "not a directory", "glob names a missing directory");
	free(g2);

	/* --- url safety --- */
	ck(hkUrlSafe("https://example.com/a?b=1&c=2") == 1, "url plain ok");
	ck(hkUrlSafe("http://x.dev/p") == 1, "url http ok");
	ck(hkUrlSafe("file:///etc/passwd") == 0, "url scheme rejected");
	ck(hkUrlSafe("https://x.com/'; rm -rf ~; echo '") == 0, "url quote rejected");
	ck(hkUrlSafe("https://x.com/`id`") == 0, "url backtick rejected");
	ck(hkUrlSafe("https://x.com/a b") == 0, "url space rejected");

	/* --- html stripping --- */
	char *t = hkHtmlToText("<html><head><style>p{color:red}</style>"
	                       "<script>var x='<b>no</b>';</script></head>"
	                       "<body><h1>Title</h1><p>Hello &amp; welcome</p>"
	                       "<div>second</div></body></html>");
	ckstr(t, "Title", "html keeps text");
	ckstr(t, "Hello & welcome", "html decodes entity");
	ck(strstr(t, "color:red") == NULL, "html drops style body");
	ck(strstr(t, "var x") == NULL, "html drops script body");
	free(t);

	if (chdir("/tmp") == 0) { snprintf(cmd, sizeof(cmd), "rm -rf %s", root); if (system(cmd)) {} }

	dispatch_tests();
	diff_tests();
	pipe_tests();
	sse_tests();
	compact_tests();
	skill_tests();
	context_file_tests();
	skill_roots_tests();
	tap_parse_tests();
	todo_tests();
	memory_tests();
	render_tests();
	orient_tests();
	orphan_tool_close_tests();
	stream_error_tests();
	paren_escape_tests();
	json_escape_scan_tests();
	stdin_claim_tests();
	rc_tests();
	rc_unified_tests();
	cmd_and_context_tests();
	env_probe_tests();
	truncation_notice_tests();
	invoke_dialect_tests();
	sse_longline_tests();
	catalog_tests();
	ask_user_tests();
	bench_tests();
	trust_prompt_tests();
	image_tests();
	shell_tests();
	fit_tests();
	paste_tests();
	crypto_tests();
	qr_tests();

	printf(fails ? "\n%d FAILURES\n" : "\nall tests passed\n", fails);
	if (test_home) {
		char rm[128];
		snprintf(rm, sizeof(rm), "rm -rf '%s'", test_home);
		if (chdir("/tmp") == 0 && system(rm)) {}
	}
	return fails ? 1 : 0;
}
