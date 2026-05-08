<p align="center">
  <img src="icon/hakoCLAW.svg" alt="hakoCLAW" width="160"/>
</p>

<p align="center">
  <strong>hakoCLAW</strong>
</p>

<p align="center">
  <em>The standalone AI agent CLI lifted from <a href="../hako">hako</a>'s 零 Rei panel. Single binary. Same C99 stack.</em>
</p>

<p align="center">
  <code>hakoc</code> — speaks Anthropic, OpenAI, Ollama, and any OpenAI-compatible endpoint. Multi-provider tool loop. Local models supported. ASan-clean.
</p>

<br>

```
  ▄█████▄    hakoCLAW v0.1.1
 ██ ███ ██   provider: anthropic
 █████████   model: claude-haiku-4-5-20251001
 ▀█▀▀█▀▀█▀   trust: granted
```

## Overview

- **One file, one binary.** `hakoCLAW.c` (~2400 LOC). Builds with `gcc -lpthread`. Curl on PATH for HTTP. Nothing else.
- **Multi-provider.** Anthropic native, Ollama native, OpenAI function-calling, plus ten aliases (Groq, Gemini, DeepSeek, Mistral, Together, Fireworks, OpenRouter, xAI, Cerebras, custom). Local Ollama works offline; future `koi` slot reserved for [hakoAI](../hakoAI) models.
- **Tool loop.** `read_file`, `list_dir`, `write_file` (with staging), `run_shell` (10s timeout). Schemas auto-generated for both Anthropic and function-calling formats.
- **Trust gate.** Untrusted dir = no tool access at all. The model sees the trust error and asks the user to grant. `/trust` once per project.
- **Sessions.** Per-cwd session id, 7-day resume rule, JSONL history log, append-only. Startup menu picks new vs resume.
- **Streaming + animations.** Anthropic SSE streams live tokens. While waiting: 8 spinner styles × 12 thinking labels, rotated per turn (`/braille /dots /bar /pulse /bounce /ghost /arrows /blocks`).
- **Mascot.** Default ghost lifted from hako. Drop a custom ASCII file with `--mascot <path>`.
- **Skills.** `~/.hakoc/skills/*.md` injected into the system prompt. `/skill install <url>` to grab one from anywhere.

## Install

**Curl one-liner** (after first release):
```sh
curl -fsSL https://raw.githubusercontent.com/<OWNER>/hakoCLAW/main/install.sh | sh
```

Detects OS/arch, drops `hakoc` into `/usr/local/bin` (or `~/.local/bin` if not root). Override `REPO=` or `PREFIX=` env vars.

## Build

```sh
# One-liner
gcc hakoCLAW.c -o hakoc -lpthread

# Or use the Makefile (also embeds icon on Windows, attaches on macOS)
make
```

ASan + UBSan:
```sh
make asan && ./hakoc_asan
```

> **Deps:** libc + pthread + `curl(1)` on PATH. No third-party libraries linked.

### Executable icon

`icon/build-icons.sh` rasterizes `icon/hakoCLAW.svg` into `.icns`, `.ico`, `.png`, and the macOS `.iconset/`. Needs `librsvg` or `imagemagick`. See `icon/README.md`.

## Run

Interactive REPL:
```sh
hakoc
```

One-shot:
```sh
hakoc -p "summarize README.md"
```

Custom mascot + pinned anim + debug response dump:
```sh
hakoc --mascot ~/my_ghost.txt --anim ghost --debug
```

## No-credit / free-tier providers

hakoCLAW does **not** bypass paid subscriptions (Claude Pro, ChatGPT Plus). Those are tied to first-party clients. Several real free tiers work today:

| Provider | Setup | Notes |
|---|---|---|
| **Gemini** (Google AI Studio) | `/login gemini` or `export GOOGLE_API_KEY=...` → aistudio.google.com/apikey | Most generous free quota. Models: `gemini-2.5-flash`, `gemini-2.5-pro`, `gemini-2.0-flash`, `gemini-1.5-flash` |
| **Groq** | `/login groq` → console.groq.com/keys | Fastest. `/model llama-3.3-70b-versatile` |
| **Cerebras** | `/login cerebras` | Free tier, very fast Llama |
| **OpenRouter** | `/login openrouter` | Has `:free` suffixed models. e.g. `meta-llama/llama-3.3-70b-instruct:free` |
| **Ollama** (local) | `ollama serve` + `/provider ollama` | 100% free, 0 quota, runs on your machine |
| **Mistral** | `/login mistral` | Free tier rate-limited |

Quickest path with no card:
```sh
hakoc
> /login gemini
> /model gemini-2.0-flash
```

## Providers (full list)

- **Anthropic**: `anthropic` / `claude` (native API, SSE streaming when no tools)
- **Ollama**: `ollama` / `local` (local server, native API)
- **OpenAI**: `openai` / `gpt` (function-calling)
- **Aliases** (OpenAI-compat): `groq` `gemini` `google` `cerebras` `deepseek` `mistral` `together` `fireworks` `openrouter` `xai` `grok` `custom`
- **Future**: `koi` (hakoAI local model — currently aliases to ollama)

## Auth

Key sources, priority order:
1. `CLAW_API_KEY` env var (always wins)
2. Provider-specific env vars (apply when their provider is active):
   - Anthropic: `ANTHROPIC_API_KEY`
   - OpenAI: `OPENAI_API_KEY`
   - Gemini/Google: `GOOGLE_API_KEY` or `GEMINI_API_KEY`
   - Groq: `GROQ_API_KEY`
   - Cerebras: `CEREBRAS_API_KEY`
   - DeepSeek: `DEEPSEEK_API_KEY`
   - Mistral: `MISTRAL_API_KEY`
   - Together: `TOGETHER_API_KEY`
   - Fireworks: `FIREWORKS_API_KEY`
   - OpenRouter: `OPENROUTER_API_KEY`
   - xAI/Grok: `XAI_API_KEY`
3. `~/.hakoc/state` (written by `/login`, mode 0600)
4. `~/.hakocrc` (`ai_api_key=...`)

`/login <provider>` opens the provider's console in your browser, prompts for the key (input hidden via termios), persists to `~/.hakoc/state`.

## Trust

First run in a directory shows:
```
  Trust this directory for tool access?
  cwd: /path/to/project
  (untrusted = no read_file, list_dir, write_file, run_shell)
  grant trust? [y/N]
```

Untrusted = **all** tools refused. The model sees the trust error in tool output and asks you to grant. `/trust` to grant later, `/trust revoke` to drop.

## Slash commands

```
/help            /clear
/login [<prov>]  /provider <name>   /model <id>
/history [local|global]
/skills [reload]    /skill install <url>    /skill uninstall <name>
/tools on|off       /trust [revoke]         /usage
/sessions           /resume <id>            /session [new]
/quit
```

## Animations

Eight thinking-spinner styles, rotated per turn. Pin one with `--anim <name>` or `anim_style=<name>` in `.hakocrc`:

| name    | preview |
|---------|---------|
| braille | `⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏` |
| dots    | `.   ..  ... ` |
| bar     | `▏▎▍▌▋▊▉█` |
| pulse   | `⠂⠆⠇⠧⠷⠿` |
| bounce  | `◐◓◑◒` |
| ghost   | `ᗜˬᗜ ᗜ◡ᗜ ᗜ‿ᗜ` |
| arrows  | `←↖↑↗→↘↓↙` |
| blocks  | `▖▘▝▗` |

12 rotating labels: thinking, pondering, considering, computing, reasoning, reading, plotting, musing, weaving, chewing on it, consulting the oracle, sharpening the claws.

## State

| Path | Purpose |
|---|---|
| `~/.hakoc/state` | provider, model, api key (mode 0600), session defaults |
| `<cwd>/.hakoc/state` | per-project session_id, turn count, timestamps |
| `<cwd>/.hakoc/trust` | sentinel: tools allowed in this cwd |
| `~/.hakoc/history` | append-only JSONL chat log |
| `~/.hakoc/skills/*.md` | system-prompt skills (loaded at launch) |
| `~/.hakocrc` | user config (provider, model, mascot_path, anim_style, ...) |

## Security

- `read_file` / `list_dir`: scoped to cwd, path-traversal blocked, **gated by trust**.
- `write_file` / `run_shell`: gated by trust. With `ai_autowrite=0`, `write_file` stages to `<path>.hakoc-pending` for review.
- `run_shell` runs under `timeout 10 sh -c` — 10s wall clock cap, no interactive stdin.
- API keys at `~/.hakoc/state` mode 0600.

## Roadmap

v0.1.x — first standalone release. Hako editor still on v0.1.0; AI extraction next session.
- v0.2: editor integration (build-time `make AI=0|1`), auto-update from GitHub Releases, pluggable local model backend.
- v0.3+: hakoAI/koi engine plugin, `/login` OAuth where providers add it.

See `../.claude/claw/PLAN.md` for the full milestone tracker.

