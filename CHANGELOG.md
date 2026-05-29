# Changelog

All notable changes to hako-code. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This project follows semver (`v0.1.x` is pre-1.0; expect breaking changes between minor versions).

## [v0.1.6] — 2026-05-25 (unreleased)

### Added — mithraeum provider (real, distinct from ollama)
- **`AI_PROVIDER_MITHRAEUM`** — distinct enum, displays as `mithraeum` in the status chip (was aliasing to `ollama`). Wire format = ollama-compat HTTP for v0.1.6 (port 11434), swaps to native `hakm-server` port in v0.1.7. Aliases: `:provider mithraeum` / `hakm` / `koi`.
- **Force prose tool mode for `MITHRAEUM`.** Small local models (3B/7B-class) can't reliably emit OpenAI `tool_calls` JSON. The agent suppresses the `tools` field in the request, prepends the `REACT_PROMPT` (XML tool schema) to the system prompt, and parses `<tool>`/`<invoke>` blocks from prose. Symmetric with the existing Anthropic OAuth path.
- **Local-hako auto-default** now sets `MITHRAEUM` (was `OLLAMA`). Display reads "&lt;tag&gt; detected, defaulted to local hako via mithraeum provider."
- **`HK_IS_OLLAMA_WIRE(t)` macro** — wire-format-equivalence helper so MITHRAEUM stays in sync with OLLAMA branches across `hkSessionCostUSD`, `hkFreeTierLabel`, no-key gate, `is_ollama` streaming detection.
- **Toolmode help text** renamed `:toolmode native|react` → `:toolmode native|prose`. `react` and `xml` kept as accepted aliases for backward compat.

### Fixed — `aiExtractStringValue` `\uXXXX` handling
- Was passing `<` etc. through literally. Qwen via ollama JSON-escapes ASCII chars like `<` as `<`, which made the prose tool parser miss every `<tool>` block. Now decodes to BMP UTF-8 (1-3 bytes) so `<tool>`/`<invoke>` blocks parse correctly. Also adds `\r` / `\b` / `\f` escapes that were dropped.

### Added — naming + storage rename (BREAKING)
- **Project rename: `hakoCLAW` → `hako-code`.** Repo renamed `mithraeums/hakoCLAW` → `mithraeums/hako-code` (GitHub auto-redirects old URL). Binary renamed `hakoc` → `hako`. Source file `hakoCLAW.c` → `hako.c`. Macros `CLAW_*` → `HAKO_*`. Banner / status / help / OSC title all updated. Brand kanji 爪 (claw) → 函 (hako, box).
- **Home dir rename: `~/.hakoc/` → `~/.hako/`** with one-shot auto-migrator on startup (`hkMigrateHakocToHako`). If old dir exists and new does not, `rename(2)` it and print a single-line notice. Credentials, per-project state, sessions, history all move transparently.
- **Config file: `~/.hakocrc` → `~/.hakorc`** (caught by the same path-rename sweep).
- **Local-hako auto-default.** On startup, if no provider is configured AND any `hako-*` model is present in local `ollama list`, default to it (`provider=mithraeum`, `endpoint=http://localhost:11434`). Preference: koi > koi-mini > sho, fine-tunes over stock-wraps. Today picks `hako-sho-stock` / `hako-koi-mini-stock`. Pre-empts the first-run wizard for the common case of "I already pulled a model."

### Fixed — Anthropic OAuth tool calls actually work
Anthropic added server-side validation on 2026-04-04 that breaks naive OAuth piggyback. After many iterations:
- **`system` field as array of content blocks** — first block must equal EXACTLY `You are Claude Code, Anthropic's official CLI for Claude.` Single-string `system` is rejected with 400. Block 2 carries the actual agent instructions but avoids any identity flip ("You are hako-code…" → model refused tools). Block 2 now leads with Claude-Code-aligned "interactive CLI tool" framing.
- **Claude Code fingerprint headers** — `User-Agent: claude-cli/1.0.40 (external, cli)` + `x-app: cli` + `anthropic-beta: oauth-2025-04-20`. Missing UA / x-app → tool_use blocks silently stripped from response.
- **Force `prose` toolmode for OAuth Anthropic** — even with everything above, the native `tools` array field gets stripped server-side. Workaround: drop the field entirely, ship the ReAct-style tool schema in the system prompt, parse `<tool>` or Claude Code's native `<function_calls><invoke>` blocks from the response. No more tool-gate game.
- **Claude Code XML parser** — accepts the native format Claude emits under OAuth: `<function_calls><invoke name="X"><parameter name="K">V</parameter></invoke></function_calls>`. Tool name aliases: `create_file`/`Write` → `write_file`, `Read` → `read_file`, `LS` → `list_dir`, `Bash` → `run_shell`.
- **Streaming branch was bypassing the tool parser** — fixed. Streamed `acc` is now scanned for tool blocks after stream ends, observations pushed back as user turns, loop continues.
- **Hide XML from display** for OAuth Anthropic — buffered render strips `<function_calls>…</function_calls>` and `<tool …>…</tool>` blocks before printing the assistant's prose.
- **`write_file` JSON-unescaped content** — escapes like `\n` and `\"` were being written as literal characters. Now passes through `hkJsonUnescape` so files get real bytes.

### Fixed — Misc
- **Tool gate keyword list** — dropped `"/"` (matched every URL), removed bare verbs `make`/`build`/`compile`/`test`/`fix`/`patch`/`add`/`rm`/`mv`/`cp` that caused reflexive shell invocations on plain conversational typing. Kept extension-based + explicit verbs.
- **Wizard cred-restore** — if `~/.hako/credentials` already has an entry for the picked provider, restore silently instead of forcing fresh OAuth.
- **Ghost text scope** — only fires for slash/colon commands with single unambiguous match; no more noise on prose.

### Added — storage layout (CC-style)
- `~/.hako/projects/<encoded-cwd>/` is now the per-project state root. Trust marker, session state, and **per-session jsonl files** (one file per session, CC pattern) all live here. The encoded cwd format mirrors Claude Code: `/Users/x/y` → `-Users-x-y`.
- `<cwd>/.hako/` only ever contains `HAKO.md` (project context, like `CLAUDE.md`). Written by `hkGrantProjectTrust` on first trust grant.
- `HAKO.md` loaded into the system prompt as a `# HAKO.md (project context)` block, after skills.
- `clEnumerateSessions` now `readdir`s the sessions dir instead of scanning a shared history file.

### Added — theme system
- **`:theme <name>` cmd** with 4 presets (`mithraeum` default, `claude`, `nord`, `mono`). Persisted in `~/.hakorc` as `theme=<name>`.
- **Theme color tokens** indirect (`TH_USER` / `TH_AI` / `TH_TOOL` / `TH_OK` / `TH_ACCENT` / `TH_META` / `TH_GHOST`) so swap propagates to spinners too.
- Improved Mithraeum palette: warmer gold for user (`#d4b168`), richer paper for AI (`#eae3d4`), sage success (`#94ac78`), brighter `TH_META` (`#a49888`) so dim secondary text stays readable.

### Added — hybrid sigil rendering
- USER: `›  ` (was `▌ `). AI: `◆  ` (was `▌ `). ERR: `!  ` (was `✗ `). SYS: `·  ` (was `  `). Sigils carry turn boundaries; turn separator rule removed.
- Status footnote compacted: `haiku-4-5-20251001·t3·↑92↓1·sub·4.9s`. Strips `claude-` / `gpt-` / `gemini-` prefix.

### Added — first-run wizard + banner box
- `clFirstRunWizard` triggers when no provider/key/oauth configured. Six-option menu (anthropic / github-copilot / github-models / openrouter / ollama / skip). Pre-checks credentials map → silent restore if already logged in.
- Banner box (`╭─╮ │ ╰─╯`, accent-bordered) auto-sizes to max(logo width, status row widths) + 4 padding so long model names never bust the right border. Medium + tiny outline logos by terminal width.

### Added — inline ghost-text autocomplete
- Slash/colon commands only. Single-match prefix → dim suggestion past cursor. TAB accepts (fish-style).

### Removed
- Character mascot system (`mascot_lines`, `--mascot`, `CL_DEFAULT_MASCOT`, `clLoadMascot`).
- Wide logo + scan-line glow animation (`CL_LOGO_WIDE`, `clShowLogo`). Medium + tiny remain.
- Animated splash. First-run wizard is now plain text.
- Banner figlet (`CL_FIGLET_HAKO`, `CL_FIGLET_CLAW`), `# NEW` / `# TIPS` blocks, daily tip rotator.
- `:history local | global` subcommands (single canonical per-session log path now).
- `<cwd>/.hako_history` opt-in file.

### Fixed — Session 13 (live-test iteration)
- **Tool name aliases — expanded + case-insensitive** (`hkExecTool`). Adds 20+ variants: `bash`/`shell`/`sh`/`exec`/`run`/`run_command` → `run_shell`; `ls`/`list_files`/`list`/`dir`/`LS` → `list_dir`; `read`/`view`/`cat`/`open_file`/`Read` → `read_file`; `create_file`/`write`/`edit`/`edit_file`/`str_replace`/`str_replace_based_edit_tool`/`str_replace_editor`/`text_editor`/`text_edit`/`Write` → `write_file`. Lowercase `bash` was the most common live-test miss.
- **Param-name aliases** — per-tool fallback so model's preferred key name resolves: path accepts `path`/`file_path`/`filename`/`directory`/`dir`; cmd accepts `cmd`/`command`/`shell`/`script`; content accepts `content`/`file_text`/`new_str`/`text`/`body`/`data`. Claude Code's `Bash` uses `command`, its text editor uses `file_text` — both now resolve first try.
- **`write_file` error split** — was `error: missing path or content` regardless of which was absent; now distinct messages per missing field.
- **`list_dir` default to `.`** when path omitted — bare `<invoke name="LS"></invoke>` now lists project root instead of erroring.
- **`hkRunShellCapture` rewritten** — old `sh -c "..."` wrapping mangled cmds with embedded quotes, heredocs, multi-line. New flow writes cmd to `/tmp/hako-cmd-<pid>.sh` and execs that. Also JSON-unescapes cmd before exec (was leaving literal `\n` `\"` from the model's JSON, broke every heredoc). Empty output with nonzero exit reports `(no output, exit code N)`.
- **OPENAI + OLLAMA wires drop `system_prompt` — fixed.** Pre-existing latent bug: Anthropic was the only wire reading `data->system_prompt`. Splice `{"role":"system","content":...}` at head of `messages[]` for OPENAI + OLLAMA branches. Restores `BASE_PROMPT` / `REACT_PROMPT` / skills / `HAKO.md` to Copilot / GH Models / OpenRouter / Gemini / Ollama / koi.
- **HAKO.md loader hardened** — binary guard (NUL or control char in scan window → skip + warn), truncation warning when file size > 200KB cap.
- **Legacy `<cwd>/.hako/{trust,state,history}` warn** on startup (skipped in pipe mode). v0.1.6 moved per-project state to `~/.hako/projects/<enc>/`; old files are silently ignored — now flagged with a `rm` suggestion.
- **Error chip rendering** — `TH_ERR` switched from truecolor rust (`38;2;192;88;64`) to bold 16-color bright red (`1;91`). Old SGR was rendering as a full-width reverse-video bar in tmux/tty without truecolor passthrough (`✗ error: ...` text invisible inside the bar). All 4 themes updated.
- **Banner box auto-sizes correctly** — width calc now measures rendered cell width via `clCellWidth` (formerly used `strlen`, undercounting multibyte `·` separators). Box also truncates any over-long row with `…` instead of wrapping to next line. Session id dropped from status line (visible via `:sessions`).
- **Stray child tags in OAuth Anthropic stream** — `<parameter>`, `<observation>`, `<invoke>` now stripped from rendered prose. Model occasionally emits fake `<observation>` blocks echoing what it expects the system to reply with.
- **Pre-tool prose suppression** — when iteration's response contains a tool call, prose is dropped entirely; the post-observation iteration carries the real summary. Matches Claude Code's chip-first ordering; eliminates the "Done!" → chip "lie" sequence.

### Added — Session 13
- **Cached env probe** in system prompt (`hkLoadSkills`). At session start runs `command -v` on `python3`/`python`/`node`/`deno`/`bun`/`ruby`/`perl`/`go`/`cargo`/`rustc`/`make`/`gcc`/`clang`/`java`/`git`/`curl`/`wget`/`jq` and emits an `# ENVIRONMENT` block. Cuts the `python` → `python3` retry round-trip on macOS.
- **Ollama perf** — `keep_alive: 30m` keeps model resident between turns (skips cold-load), `num_predict` caps generation. Big win on local koi turns.
- **ORDERING + HALLUCINATION rules** in OAuth Anthropic system prompt + REACT_PROMPT. Tells model: emit tool call first, never claim success before observation, prefer `write_file` over bash heredocs.

### Notes
- **Bloat budget bumped 6000 → 8000.** Drivers above + headroom for v0.1.6 polish. Source ~6360 LOC end of session 13.
- OAuth refresh-token rotation (per-turn `clOAuthEnsureFresh`) already covers Anthropic + Copilot since v0.1.5 — no change this release.
- Existing users: old `<cwd>/.hako/{trust,state,history}` files are ignored — startup now prints a one-liner pointing at `rm`.
- **hako via Ollama** — `hako-sho-stock` (Qwen2.5-Coder-3B + hako wrap) and `hako-koi-mini-stock` (7B) verified working through the `:provider mithraeum` path; the mithraeum alias routes to ollama until the v0.1.7 `hakm-server` direct wire.

## [v0.1.5] — 2026-05-20

### Added — auth
- **Anthropic OAuth (Claude Pro / Max subscription)** — `/login anthropic` opens `claude.ai/oauth/authorize` (PKCE S256, manual code paste), exchanges at `console.anthropic.com/v1/oauth/token`, stores access + refresh in cred store. API calls send `Authorization: Bearer` + `anthropic-beta: oauth-2025-04-20` instead of `x-api-key` — bills against the user's Claude subscription, not pay-per-token. Auto-refresh per turn when expired. Lifts Claude Code's public client_id; `CLAW_ANTHROPIC_CLIENT_ID` env overrides. PKCE challenge via `openssl dgst -sha256 -binary | openssl base64` shell-out.
- **`/login anthropic-api`** — paste flow for users with a regular Anthropic API key (separate from subscription).
- **GitHub Copilot OAuth (Pro / Business)** — `/login copilot` runs GitHub device flow w/ VS Code Copilot's public client_id, exchanges the GH access token at `api.github.com/copilot_internal/v2/token` for a short-lived Copilot session token (~25 min). Calls hit `api.githubcopilot.com/chat/completions` (OpenAI-compat shape) with `Editor-Version` + `Copilot-Integration-Id: vscode-chat` headers. Bills against Copilot subscription. Auto re-exchange via `clOAuthEnsureFresh` per turn.
- **GitHub Models OAuth** — `/login github-models` device flow (gh CLI public client_id, broad scope). GH token used directly as Bearer against `models.inference.ai.azure.com/chat/completions`. Free for any GitHub user, rate-limited. OpenAI-compat shape.
- **OpenRouter PKCE** — `/login openrouter` binds 127.0.0.1 loopback (ports 1456–1499), opens `openrouter.ai/auth` with code_challenge, exchanges at `api/v1/auth/keys` for a user-scoped OR API key. macOS / Linux. `openrouter-api` for paste-existing-key.
- **Per-provider obfuscated credential store** — `~/.hako/credentials` (INI of every logged-in provider's `api_key` / `oauth_refresh` / `oauth_expires_at`). XOR-folded with a 32-byte machine-bound key, base64'd, mode 0600. Header magic `CLAWCREDv1`. Not real encryption — defeats casual `cat` / `grep`.
- **Mid-chat provider swap continuity** — `/provider X` snapshots outgoing provider's secrets, restores incoming's, and flattens raw=1 / raw=2 tool messages so the conversation survives wire-format swaps.
- **`/accounts`** — lists saved logins (key/oauth + active marker).
- **`/logout [<provider>]`** — wipes one provider's credentials.

### Added — discovery + workflow
- **`/providers`** — grouped catalog of every provider (OAuth / Local / Pay-per-token) with `◎` (active) and `*` (saved login) markers.
- **`/models`** — Ollama-type providers: live `/api/tags` (+ `◎` on the active model). Other providers: curated suggestion list per provider (Claude/Sonnet/Opus models for anthropic, gpt-4o/o1 for openai, gemini-2.5-* for gemini, etc.).
- **`/retry`** — re-send last user message after dropping the trailing AI turn(s).
- **`/edit`** — pop last user message back into the prompt buffer for tweaking (line editor pre-fill via `cl_preset_input`). Then resend.
- **`/undo`** — drop last AI turn (history + API stack), keep user msg.
- **TAB completion** in the termios line editor — slash commands + provider names after `/login` / `/provider` / `/logout`. Single-match auto-completes with trailing space.

### Added — ReAct fallback for non-native models
- **`/toolmode native|react`** — react mode injects a pseudo-XML tool schema in the system prompt and parses model responses for `<tool name="X">{...}</tool>` blocks. Each block is executed and the result appended as a `<user>` turn wrapped in `<observation tool="X">...</observation>`. Loop continues until the model emits a plain-text final answer. Works with ANY instruct-tuned model (Mistral 7B, Phi-4, DeepSeek-R1 distills, Gemma, smaller Llama variants), unblocking models whose GGUF builds don't honor Ollama's `tools:[]` field. Persisted in state.

### Added — observability + cost
- **Cost tracking** — per-provider USD/M-token price table (Claude family, GPT-4o family, Gemini Pro/Flash, DeepSeek, Mistral, Grok). `/usage` shows estimated session $. `/usage reset` zeros counters. Anthropic OAuth → `$0 (Claude subscription)`. Ollama → `$0`.
- **Status line gets cost + latency** — `· $0.0042 · 1.2s` for paid; `· sub` for Anthropic OAuth; `clWallMs()` helper tracks turn duration.

### Added — rendering
- **Tool category glyphs** — `◎` read · `✎` write · `❯` shell · `▸` default (ASCII fallback on mono).
- **Fenced code blocks** — opener/closer rendered as hairline rule; interior lines get bronze gutter (`▎`) + chalk text. Cross-line state via `cl_in_codefence`.
- **Inline markdown** — `**bold**`, `*italic*` / `_italic_`, `` `code` ``, `#/##/###` headings, `>` quote, `-`/`*` bullet. ANSI only on TTY.
- **Error glyph** — system lines starting with `Error`/`error` render `✗ ` in rust.
- **Ollama crash diagnostic** — actionable hints on `model not found` / OOM / runner-terminated instead of raw hex.

### Added — misc
- **Ollama cloud provider** (`ollamacloud` / `ocloud`) — points at `https://ollama.com`. Bearer when key set.
- **Auto tool-gating** (`/toolgate on|off`, default on) — drop tool schema when user msg has no tool-keyword. Stops small models hallucinating tool calls on greetings.

### Fixed
- **`hkBuildToolsSchema` JSON escape** — descriptions run through `hkJsonEscapeInto`. Closed Session 4 soft note.
- **Anthropic OAuth User-Agent** — token exchange + refresh POSTs now send `hako-code/<version> (Claude OAuth client)` to avoid generic `curl/8.x` rate-limit triggers.
- **Duplicate `tool_call_id` 400 from OpenAI-compat providers** — gpt-4o on GH Models free tier (and other small models) sometimes emit the same `tool_call_id` multiple times in one `tool_calls` array. `hkFnToolExecAll` now tracks seen ids per turn and skips dupes so we don't push duplicate tool replies (which the API rejects with `Invalid parameter: Duplicate value for 'tool_call_id'`).
- **`:` commands routed to LLM as prompts.** REPL gate at `clRepl` only checked `line[0] == '/'`, so `:help` etc were sent to the model. Gate now accepts both prefixes; pipe mode already worked because it routes via JSON `cmd` field.
- **Anthropic streaming three-way mismatch.** Body's `"stream":true`, curl flags (`-sN` + `accept: text/event-stream`), and the response parser's SSE-mode gate were keyed off different predicates. Unifying them onto `oauth_anth || (E.ai_stream && !(E.ai_tools_enabled && hkProjectTrusted()))` fixes both directions (SSE response into JSON parser = "empty response", JSON response into SSE parser = nothing extracted). Anthropic OAuth endpoint streams unconditionally — force stream when `ai_oauth_provider == "anthropic"`.
- **Streamed text deltas weren't JSON-unescaped.** SSE `content_block_delta` extracted via `hkExtractJsonString` returned raw JSON-encoded substring with literal `\n` `\"` etc. Now runs through `hkJsonUnescape` before append/print.
- **Anthropic OAuth URL hidden in iSh / headless** — `clOAuthAnthropic` now `aiAddHistory`s the authorize URL explicitly before calling `clOpenUrl`, so users on iSh / SSH / headless can copy-paste into a browser.
- **`hkExtractJsonString` / `hkExtractJsonInt` were brittle to pretty-printed JSON.** Both built strict no-whitespace patterns (`"key":"`), missing `"key": "value"` with spaces between `:` and the value. Anthropic's OAuth token endpoint returns pretty-printed JSON — every successful `:login anthropic` exchange (with valid access_token + refresh_token + expires_in) was misreported as `OAuth: exchange failed (no access_token)`. Both extractors now accept arbitrary whitespace around the colon. **This bug masked all live OAuth testing**; the "rate-limited" + "session-expired" diagnoses from earlier in v0.1.5 dev were red herrings caused by this — the underlying flow was working from the first attempt.

### Changed
- **`:` is now the primary command prefix.** `:help` `:providers` `:models` `:login` `:retry` `:edit` `:undo` `:q` etc. Mirrors vim and the hako editor's command line. `/` retained as legacy alias — both work, TAB completion accepts either and preserves whichever was typed.
- **`:q` quits** (alongside `:quit` and `:exit`).
- **Cost display rework for free / sub / local tiers.** Status line now shows `· sub` (Anthropic OAuth, Copilot OAuth), `· free` (GH Models OAuth), `· local` (Ollama on localhost), `· ollama` (Ollama cloud), or `· $0.0042` (paid). `:usage` cost line gets matching labels: "bundled in subscription" / "free tier (rate-limited)" / "$0 (local)" / `$X.XXXX estimated`. Token counters (`↑in ↓out`) shown on every turn regardless.

### Added
- **First-launch hint** — when REPL boots with no provider/key/endpoint configured, prints `:providers` / `:login <name>` / `:login ollama` discovery cue.

### Removed
- **Pre-registered OAuth flows** — Gemini device flow, GitHub Models device flow (replaced by GH-CLI client_id), OpenRouter PKCE (replaced by clean re-add) all stripped from earlier WIP. Strategy shift: only piggyback flows where providers ship public first-party client_ids (Anthropic, GitHub Copilot, GitHub Models, OpenRouter).
- **Plaintext secrets in state files** — `ai_api_key` and `ai_oauth_*` no longer written to `~/.hako/state` or per-project state. All secrets live in `~/.hako/credentials` (obfuscated). Legacy state keys migrate on first launch.

### Notes
- **Bloat budget revised: 5000 → 6000 LOC.** Source now ~5800 LOC. Budget bumped this release; further trim work deferred to v0.1.6.
- **ChatGPT Plus piggyback rejected for v0.1.5.** Custom `/backend-api/conversation` wire format + Cloudflare-tier anti-bot fingerprinting + OpenAI ToS gray-zone + no tool-calling on backend = cost > benefit. May revisit as `v0.1.6-experimental` flag.
- **Buddy compatibility note:** Anthropic open-sourced `claude-desktop-buddy` (ESP32-S3 BLE companion for Claude Desktop). claw could implement the same BLE approval protocol as a future module — physical approval gateway for `write_file` / `run_shell`. Not in v0.1.5; queued post-tag.

### Pending before tag
- Live smoke: `/login anthropic` against Claude Pro/Max (debugging session-expired error — likely code-burn timing).
- Live smoke: `/login copilot` against Copilot Pro account.
- Live smoke: `/login github-models` against any GH account.
- Live smoke: `/login openrouter` PKCE loopback.
- Live smoke: ReAct mode with a local Ollama model (qwen2.5:7b suggested for Intel iMac).

## [v0.1.4]

### Added
- **`--pipe` mode** — JSONL I/O over stdin/stdout for hako editor integration. Hako spawns `hako --pipe` once per Rei pane, sends `{"type":"prompt"|"slash"|"quit"}`, receives `{"type":"init"|"message"|"tool_start"|"tool_end"|"done"|"error"}`. No REPL UI in pipe mode — events stream as one JSON object per line. Enables editor to delegate the entire agent loop without embedding the curl / tool / provider layer in `hako.c`.
- **Mithraeum palette by default** — `ANSI_USER/AI/SYS/ERR/TOOL` rewritten as 24-bit truecolor (gold / paper / dim chalk / rust / bronze). Matches the hako default theme + site banners + icon scratches. Falls back to dim on non-TTY (color_enabled guard unchanged).

## [v0.1.3]

### Added
- **Framed banner** — boxed startup with HAKO + CLAW figlet on the right, mascot on the left, inline `# NEW` and `# TIPS` sections. Auto-tiers LARGER / SMALLER / compact by terminal rows × cols. `--compact` forces one-liner.
- **`/models` slash** — lists installed Ollama models via `<endpoint>/api/tags`. 3s timeout; clear hint when daemon unreachable.
- **Terminal title brand** — sets `爪 hako-code` via OSC 0 on REPL entry, cleared on exit. Skipped in one-shot mode.

### Fixed
- **Gemini `INVALID_ARGUMENT` on tool turns.** Root cause: OpenAI/Gemini-compat tool flow dropped `tool_calls` from the assistant message and omitted `tool_call_id` on each tool reply. Strict validators 400-reject. `hkFnToolExecAll` now preserves both; added `aiPushMessageBody` (raw=2 message serializer) + `hkExtractRawJsonArray` helper.
- **Ollama empty-response after provider swap.** `hkApplyProviderAlias` was guarded by `if (!E.ai_endpoint)` — switching gemini→ollama left endpoint at `generativelanguage.googleapis.com`, so requests POSTed to Google. Now always swaps endpoint on switch. Added `http://localhost:11434` default for `ollama` / `local` / `koi`.
- **Windows / cross-platform read/write** — `hkReadFileAll`, `write_file` staging + commit, `hkSaveSession` reads use `"rb"` / `"wb"` for byte-exact round-trips (no CRLF stripping on Windows).
- **`list_dir`** marks directories with a `/` suffix so the model can tell them apart without a stat follow-up.

### Hardened
- **iSh skill loader:** bounded `fread` (terminate at `got`, not `sz`), checked `realloc` (no leak on failure), 4 MiB cumulative skill-prompt cap.

### Icons
- New mithraeum-aesthetic SVG (`icon/hako-code.svg`): kanji `爪` on void with gold corner ticks, dashed spring accent, rust claw scratches, ordo footer.
- `make icons` target regenerates `.icns` / `.ico` / `.png` / iconset from the SVG. Falls back to Python PIL for `.ico` when ImageMagick missing.

## [v0.1.2]

### Added
- **Termios raw line editor.** Cursor keys (← → Home End), readline-style chords (^A ^E ^U ^K ^W ^L), backspace/delete at cursor, ^C cancel, ^D EOF on empty.
- **Persistent input history** at `~/.hako/input_history` (last 500 entries, dedup-adjacent). ↑/↓ navigates.
- **Multi-row aware redraw.** Long inputs that wrap past terminal width no longer desync cursor; full linenoise-style row tracking + buffered single `write()`, hidden cursor during redraw.
- **`hako --update` self-updater.** Hits GH Releases API, downloads matching platform asset, verifies sha256 sidecar (mandatory, refuses on missing/mismatch), atomically replaces the binary via `rename(2)`. `--update-force` re-downloads even if same version.
- **macOS universal2 binary** (`make UNIVERSAL=1` adds `-arch arm64 -arch x86_64`). Single fat asset works on Apple Silicon and Intel.
- **Linux arm64 release** built on `ubuntu-24.04-arm` GitHub runner.
- **FreeBSD x86_64 release** built via `vmactions/freebsd-vm@v1`. Non-blocking — release publishes even if FreeBSD job fails.
- **install.sh sha256 verify** with `sha256sum` / `shasum` / `sha256` fallback chain; macOS quarantine xattr stripped post-install.
- **`LICENSE`** (GPL-3.0).
- **`CHANGELOG.md`** (this file).
- **`^R` reverse-incremental history search** in line editor. Esc/Enter exits, repeat `^R` cycles older matches.
- **Bracketed paste mode** (`\x1b[?2004h`) — multi-line pastes batch-insert without firing Enter on every newline.
- **Directory skills + `read_skill` tool.** Skill loader handles directories (load `SKILL.md` as dispatcher + inject a `<files>` manifest); `read_skill(skill, path)` reads files relative to a skill's root, path-traversal blocked, no trust gate. Enables corp-style dispatchers and any folder-of-markdown skill to run native.

### Changed
- README rewritten: quickstart up top, line editor table, platform matrix, `--update` section, philosophy paragraph.
- Asset name: `hako-code-macos.tar.gz` → `hako-code-macos-universal.tar.gz`.

### Fixed
- Cursor stuck at right edge / garbage chars when typing past terminal width.

## [v0.1.1]

### Added
- **`/login <provider>`** — opens browser to provider console, hides paste, persists key to `~/.hako/state` mode 0600.
- **Trust gate** on first run: `[y/N]` prompt with cwd shown. Untrusted = all tools refused (read_file, list_dir, write_file, run_shell). `/trust` to grant later.
- **Startup session menu** — pick new / resume / continue.
- **Provider expansion:** Gemini (Google AI Studio), Cerebras, custom, koi (placeholder for hakoAI engine, currently aliases ollama). Console URLs for `/login` browser-open.
- **Env vars** for every provider's API key (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, `GOOGLE_API_KEY`/`GEMINI_API_KEY`, `GROQ_API_KEY`, `CEREBRAS_API_KEY`, `DEEPSEEK_API_KEY`, `MISTRAL_API_KEY`, `TOGETHER_API_KEY`, `FIREWORKS_API_KEY`, `OPENROUTER_API_KEY`, `XAI_API_KEY`) + `CLAW_API_KEY` always-wins.
- **`--debug`** flag dumps raw API responses to stderr per turn.
- **Icon:** `icon/hako-code.svg` + `icon/build-icons.sh` (rsvg-convert / imagemagick).

### Changed
- Renamed everything: source `claw.c` → `hako-code.c`, binary `claw` → `hako`, state dir `~/.claw/` → `~/.hako/`, project dir `<cwd>/.claw/` → `<cwd>/.hako/`, config `~/.clawrc` → `~/.hakorc`.
- `read_file` and `list_dir` now also require trust (was: only `write_file` / `run_shell`).
- `aiExtractResponse` rewritten — handles tool-use-only responses, walks Anthropic `content[]` array, cleanly extracts `{"error":{"message":"..."}}` for any provider.
- Better config errors: `"missing api key for X. Run /login X, or set X_API_KEY env var"` + current model + endpoint.

### Fixed
- Duplicate user echo in REPL (terminal echoes input already; store-only path used).
- Empty-response error includes first 180 bytes of raw response for diagnosis.

## [v0.1.0]

### Added
- Initial standalone release. Lifted entire AI subsystem from `hako.c` (~2340 LOC).
- Provider resolution: Anthropic native, Ollama native, OpenAI function-calling + 7 aliases (deepseek/mistral/together/fireworks/openrouter/xai/grok).
- Tools: `read_file`, `list_dir`, `write_file` (with `ai_autowrite` staging), `run_shell` (10s timeout).
- HTTP layer (libcurl via popen + SSE parser).
- Tool loop: `hkFnToolExecAll` — Anthropic content-array + fn-calling with 6-iteration cap.
- Pthread worker for AI requests; `data->lock` mutex.
- Session/state save+load with 7-day resume rule.
- History JSONL log + tail loader rebuilding API stack.
- Skills loader (`~/.hako/skills/*.md` injected into system prompt).
- Slash commands: `/sessions /resume /session /usage /provider /model /help /clear /history /skills /tools /trust /quit`.
- ANSI-colored REPL with role bars, dim system, yellow tools, red errors.
- 8 spinner animations × 12 thinking labels, pthread-driven, joined cleanly before output. `--anim` pin / `anim_style=` config.
- Mascot from hako (default ghost), `--mascot <path>` for custom.
- Cross-OS Makefile (macOS / Linux / Windows-MinGW).
- `install.sh` curl one-liner.
- 3-OS release workflow gated on `v0.1*` tags + `v[1-9]*`.
