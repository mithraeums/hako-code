<p align="center">
  <a href="https://mithraeums.github.io">
    <img src="https://mithraeums.github.io/assets/banner-hako-code-dark.svg" alt="hako-code — a standalone agent for the inner chamber" width="100%"/>
  </a>
</p>

<p align="center">
  <em>A standalone terminal AI agent in a single C file. Quiet, model-agnostic, skill-driven.</em>
</p>

<p align="center">
  <a href="https://github.com/mithraeums/hako-code/releases"><img src="https://img.shields.io/badge/version-v0.2.0-b89656?style=flat-square&labelColor=14130f" alt="v0.2.0"/></a>
  <img src="https://img.shields.io/badge/license-GPL--3.0-c8c2b2?style=flat-square&labelColor=14130f" alt="GPL-3.0"/>
  <img src="https://img.shields.io/badge/C99-single%20file-c8c2b2?style=flat-square&labelColor=14130f" alt="C99 single file"/>
  <img src="https://img.shields.io/badge/providers-13-c8c2b2?style=flat-square&labelColor=14130f" alt="13 providers"/>
  <img src="https://img.shields.io/badge/platforms-5-c8c2b2?style=flat-square&labelColor=14130f" alt="5 platforms"/>
</p>

<p align="center">
  <sub><a href="https://mithraeums.github.io">site</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako-code/releases">releases</a> &nbsp;·&nbsp; <a href="CHANGELOG.md">changelog</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako">hako (models)</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako-edit">hako-edit</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums">org</a></sub>
</p>

<br>

<!-- HERO: screenrecord-hako.gif — animated agent demo. 11s @ 12fps, 960px wide, ~1.2MB. Auto-loops on GitHub. -->
<p align="center">
  <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-code/screenrecord-hako.gif?raw=true" alt="hako agent live demo" width="82%"/>
</p>

<table align="center">
  <tr>
    <td align="center" width="33%">
      <!-- hako-code/vhs/splash.tape — startup banner, mithraeum/sho auto-default. -->
      <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-code/screenshot-splash.png?raw=true" alt="hako startup banner" width="100%"/><br/>
      <sub>local <b>hako</b> model by default, via mithraeum</sub>
    </td>
    <td align="center" width="33%">
      <!-- hako-code/vhs/models.tape — `:models` catalog. -->
      <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-code/screenshot-models.png?raw=true" alt=":models output" width="100%"/><br/>
      <sub><code>:models</code> · local hako + 13 cloud providers</sub>
    </td>
    <td align="center" width="33%">
      <!-- hako-code/vhs/theme-picker.tape — bare :theme popup with live color swatches. -->
      <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-code/theme-picker.gif?raw=true" alt="theme picker popup with live swatches" width="100%"/><br/>
      <sub>arrow-key <code>:theme</code> picker · live swatches</sub>
    </td>
  </tr>
</table>


<table>
  <tr>
    <td width="33%" valign="top"><b>Local first.</b><br/><sub>Auto-defaults to any installed <a href="https://github.com/mithraeums/hako">hako</a> model — <code>hako-sho</code> (3B) or <code>hako-koi</code> (7B), runs on your device, no key, no cloud. The agent detects it and just works.</sub></td>
    <td width="33%" valign="top"><b>Single binary.</b><br/><sub>One C file. Builds with <code>gcc -lpthread</code>. Curl on PATH for HTTP. Nothing else linked.</sub></td>
    <td width="33%" valign="top"><b>13 cloud providers when you want them.</b><br/><sub>Anthropic OAuth (Claude Pro/Max sub), GitHub Copilot, GH Models (free), OpenRouter, plus 9 more over OpenAI-compat. Set a key or run a one-line <code>:login</code> — your call.</sub></td>
  </tr>
</table>

<p align="center"><sub><b>—— I ——</b></sub></p>

## Overview

- **Local models run on the native engine — no ollama, no server.** Plain `make`
  is all the agent needs; it runs Qwen-class models by spawning the
  [hako engine](https://github.com/mithraeums/hako) as a one-shot `hakm`
  subprocess per turn (`hakm <model.mlf2> --chat-stdin`) over mmap'd MLF2 weights.
  No in-process link, no compile flag, no daemon, no socket. Install the engine
  CLI once with `make hakm` (→ `~/.hako/bin/hakm`). Drop a `.mlf2` in
  `~/.hako/models/` and `hako` auto-defaults to the **mithraeum** provider on
  first launch — no key, no `:login`, no config (preference: koi > sho,
  fine-tunes over stock-wraps). Force prose tool mode is on so
  `<tool name="...">{...}</tool>` calls parse on small models. Convert a GGUF
  with the engine's `tools/gguf2mlf.py`, or `:pull <tier>` it from HuggingFace.
  (ollama is also supported as a *separate* optional provider — `:login ollama` —
  for non-hako models.)
- **13 cloud providers when you want them.** Anthropic native (SSE + OAuth via Claude Pro/Max), OpenAI function-calling, GitHub Copilot, GitHub Models (free), OpenRouter (PKCE), and OpenAI-compat aliases for Gemini, Groq, Cerebras, DeepSeek, Mistral, Together, Fireworks, xAI/Grok, custom. `:login <name>` walks you through OAuth or hands you to the provider's console for an API key.
- **Terminal-class line editor.** Termios raw mode, cursor keys, history (↑ ↓), `^R` reverse-search, Home/End, kill-word, kill-line, bracketed paste. Multi-row aware redraw, no flicker.
- **Theme presets + arrow-key picker.** `:theme <name>` swaps the palette directly; bare `:theme` opens a popup you arrow through, each row showing that theme's colors as live swatches. Same picker on bare `:provider`. Persisted in `~/.hakorc`. Truecolor where supported, 16-color fallback for the error chip in tmux without `RGB` passthrough.
- **HAKO.md project context.** Per-project `<cwd>/.hako/HAKO.md` is auto-loaded into the system prompt — the CLAUDE.md equivalent for hako-code. Binary-safe + size-capped (200KB).
- **Trust-gated tools.** `read_file`, `list_dir`, `write_file`, `edit_file` (str_replace), `edit_lines` (line range), `run_shell` (10s timeout), `read_skill`. Per-call `[y]/[n]/[a]` approval; untrusted dir = all tools refused. `:trust` once per project.
- **Persistent sessions + skills.** Per-cwd session id, 7-day resume, append-only JSONL history. Skills are markdown — flat or directory dispatchers ([corp](https://github.com/mithraeums/skills/tree/main/corp)-style) — pulled on demand via the `read_skill` tool. Notes you keep on disk for the agent to find.
- **Self-update.** `hako --update` pulls the latest GitHub release, verifies sha256, atomically replaces the binary. No reinstall, no rebuild.
- **Streaming + spinners.** Anthropic SSE streams live tokens. 8 spinner styles × 12 thinking labels rotate per turn (`⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏`).

<p align="center"><sub><b>—— II ——</b></sub></p>

## Build & Run

### Build

```sh
gcc hako.c -o hako -lpthread        # one-liner

make                                      # cross-OS Makefile
make UNIVERSAL=1                          # macOS arm64 + x86_64 fat binary
make asan                                 # ASan + UBSan build
```

> Deps: libc + pthread + `curl(1)` on PATH. No third-party libraries linked.

### Install

```sh
curl -fsSL https://mithraeums.github.io/install.sh | sh
```

Detects OS and arch, downloads the latest signed release, verifies the sha256 sidecar, drops `hako` into `/usr/local/bin` (or `~/.local/bin`). No package manager. No daemon. No telemetry.

<table align="center">
  <tr>
    <td align="center"><b>macOS</b><br/><sub>universal2 · arm64 + x86_64</sub></td>
    <td align="center"><b>Linux</b><br/><sub>x86_64 · arm64</sub></td>
    <td align="center"><b>FreeBSD</b><br/><sub>x86_64</sub></td>
    <td align="center"><b>Windows</b><br/><sub>x86_64 · MinGW</sub></td>
    <td align="center"><b>iSh</b><br/><sub>linux-x86_64 binary</sub></td>
  </tr>
</table>

### First turn

**Local, zero config (recommended)** — build with the engine, drop in a weight, launch:

```sh
make                                                   # plain build — the agent
make hakm                                              # installs the engine CLI → ~/.hako/bin/hakm
# convert a Qwen2.5-Coder GGUF once → ~/.hako/models/hako-sho.mlf2
#   (python3 ../hako/tools/gguf2mlf.py model.gguf ~/.hako/models/hako-sho.mlf2)
hako                                                   # auto-detects the .mlf2, runs it via the hakm subprocess (mithraeum)
> :trust                                                # grant tool access in this project
> list files in this directory
```

**Or wire a cloud provider:**

```sh
hako
> :login anthropic       # OAuth — uses your Claude Pro / Max subscription
> :login github-models   # free for any GitHub account
> :login gemini          # paste a free-tier API key
> :model gemini-2.5-flash
```

Or one-shot:

```sh
hako -p "summarize README.md"
```

Self-update:

```sh
hako --update           # check + atomic-replace if newer
```

<p align="center"><sub><b>—— III ——</b></sub></p>

## Key Bindings & Commands

### Normal Mode

| Key | Action |
|---|---|
| ← → | move cursor |
| ↑ ↓ | history prev / next |
| Home / End · `^A` / `^E` | line start / end |
| `^U` / `^K` / `^W` | kill to start / end / word |
| `^R` | reverse-incremental history search |
| `^L` | clear screen |
| Backspace · Delete | delete back / forward at cursor |
| `^C` | cancel current line |
| `^D` (empty) | EOF / exit |

*Bracketed paste enabled in raw mode — multi-line pastes batch-insert.*

### Commands (inside prompt):

`:` is the primary prefix (vim / hako style). `/` works as a legacy alias.

```
:help     :clear     :retry     :edit     :undo     :usage     :q
:providers          :models      :provider <name>      :model <id>
:login [<prov>]     :logout [<prov>]     :accounts
:history [local|global]
:skills [reload]    :skill install <url>     :skill uninstall <name>
:tools on|off       :toolgate on|off     :toolmode native|react     :trust [revoke]
:sessions           :resume <id>     :session [new]
```

**TAB** completes commands and provider names after `:login` / `:provider` / `:logout`.

<p align="center"><sub><b>—— IV ——</b></sub></p>

## Configuration

### Auth · Trust · State

```
~/.hako/state                              provider, model, settings (mode 0600, no secrets)
~/.hako/credentials                        per-provider api_key / oauth tokens (obfuscated, 0600)
~/.hako/skills/                            markdown behaviors (flat or dir-dispatcher)
~/.hako/input_history                      line editor history (last 500)
~/.hako/projects/<encoded-cwd>/state       per-project session id, turn count
~/.hako/projects/<encoded-cwd>/trust       sentinel: tools allowed in this cwd
~/.hako/projects/<encoded-cwd>/sessions/   append-only JSONL session history
~/.hakorc                                  user config
<cwd>/.hako/HAKO.md                        per-project context (auto-loaded into system prompt)
```

**Three auth paths:**

- **OAuth (subscription / account-bound)** — `:login anthropic` (Claude Pro/Max), `:login copilot` (GitHub Copilot Pro/Business), `:login github-models` (free for any GitHub user), `:login openrouter` (PKCE, auto-issue an OR API key). Inference bills against your subscription where applicable; no per-token API key needed.
- **API-key paste** — `:login openai`, `:login gemini`, `:login groq`, `:login cerebras`, `:login deepseek`, `:login mistral`, `:login together`, `:login fireworks`, `:login xai`, `:login anthropic-api`, `:login openrouter-api`, `:login custom`. Opens provider console in your browser, prompts with input hidden, persists into the cred store.
- **Local** — `:login ollama` / `:login ollamacloud`. Local: ensure `ollama serve` is running. Cloud: paste an Ollama key.

Run `:providers` for the full grouped list with `◎` (active) and `*` (saved login) markers. `:models` lists installed local models on Ollama or curated suggestions per provider. `:accounts` lists saved logins; `:logout [<provider>]` wipes one. Mid-chat `:provider X` swaps secrets and flattens wire-format-specific message bodies so the conversation survives.

Resolution order: `HAKO_API_KEY` env → `<PROVIDER>_API_KEY` env → `~/.hako/credentials` (per provider). Legacy `CLAW_*` / `HAKOC_*` env names are read as fallback with a one-shot deprecation warning.

First run in any directory asks for trust. Untrusted = no tool access at all.

### Connection Matrix

| Provider          | id (`:provider <id>`)            | Auth          | Cost     | Wire format       | Notes                                        |
|---                |---                               |---            |---       |---                |---                                           |
| Anthropic         | `anthropic` · `claude`           | OAuth         | sub      | native + SSE      | Claude Pro/Max subscription; live-tested     |
| Anthropic API     | `anthropic-api` · `claude-api`   | paste         | $/tok    | native + SSE      | regular API key, separate from sub           |
| GitHub Copilot    | `copilot` · `github-copilot`    | OAuth         | sub      | OpenAI-compat     | Copilot Pro/Business; device flow            |
| GitHub Models     | `github-models` · `ghmodels`    | OAuth         | free     | OpenAI-compat     | rate-limited; any GH account                 |
| OpenRouter        | `openrouter`                     | OAuth (PKCE)  | $/tok    | OpenAI-compat     | auto-issues user-scoped key; `:free` tier    |
| OpenRouter API    | `openrouter-api`                 | paste         | $/tok    | OpenAI-compat     | paste existing key                           |
| OpenAI            | `openai` · `gpt`                 | paste         | $/tok    | function-calling  |                                              |
| Gemini            | `gemini` · `google`              | paste         | free/$   | OpenAI-compat     | generous free tier on AI Studio              |
| Groq              | `groq`                           | paste         | free     | OpenAI-compat     | fastest hosting for Llama family             |
| Cerebras          | `cerebras`                       | paste         | free     | OpenAI-compat     | ultra-fast inference                         |
| DeepSeek          | `deepseek`                       | paste         | $/tok    |  OpenAI-compat    | DeepSeek-Chat / Reasoner                     |
| Mistral           | `mistral`                        | paste         | $/tok    | OpenAI-compat     | Mistral Large / Small / Codestral            |
| Together          | `together`                       | paste         | $/tok    | OpenAI-compat     | aggregator                                   |
| Fireworks         | `fireworks`                      | paste         | $/tok    | OpenAI-compat     | aggregator                                   |
| xAI               | `xai` · `grok`                   | paste         | $/tok    | OpenAI-compat     | Grok API at api.x.ai                         |
| Ollama (local)    | `ollama` · `local`               | none          | local    | native            | needs `ollama serve` running                 |
| Ollama Cloud      | `ollamacloud` · `ocloud`         | paste         | $/tok    | native            | ollama.com hosted                            |
| custom            | `custom`                         | paste         | depends  | OpenAI-compat     | set `ai_endpoint` in `.hakorc`              |

*Three wire formats (native Anthropic, native Ollama, OpenAI-compat).  Quickest paths: (a) **Claude Pro/Max** → `:login anthropic`. (b) **Copilot Pro** → `:login copilot`. (c) **Free, no card** → `:login github-models`, `:login gemini`, `:login groq`, or `:login ollama`. (d) **Pay-as-you-go** → any paste row. ChatGPT Plus piggyback deferred — no public OAuth surface yet.*

### Tool modes

Some smaller / non-tool-tuned models (Mistral 7B, Phi-4, DeepSeek-R1 distills, smaller Gemma / Llama variants) don't reliably emit OpenAI/Anthropic function-calling JSON. Toggle ReAct mode:

```sh
:toolmode react        # model emits <tool name="X">{...}</tool> blocks in prose
:toolmode native       # back to native function-calling (default)
```

ReAct mode injects a tool schema in the system prompt and parses `<tool>...</tool>` blocks from the model's response. Each tool call is executed and the result is appended as `<observation tool="X">...</observation>` for the next turn. Works with any instruct-tuned model.

<p align="center"><sub><b>—— V ——</b></sub></p>

## Skills

```
~/.hako/skills/
├── style.md                  flat skill — whole file injected
└── corp/                     directory skill — SKILL.md is the dispatcher
    ├── SKILL.md
    └── agents/
        ├── CEO.md
        ├── DEV.md
        └── QA.md
```

Directory skills inject only `SKILL.md` plus a `<files>` manifest. The agent reads inner files on demand via `read_skill(skill, path)` — no trust gate (skills are user-installed), path-traversal blocked.

```sh
git clone https://github.com/mithraeums/skills ~/.hako/skills/_tmp
cp -r ~/.hako/skills/_tmp/corp ~/.hako/skills/
rm -rf ~/.hako/skills/_tmp
hako        # "loaded 1 skill(s)"
```

Browse the catalog: [mithraeums/skills](https://github.com/mithraeums/skills).

<p align="center"><sub><b>—— VI ——</b></sub></p>

## Change Log

### v0.2.0 (Latest)

- **`edit_file` + `edit_lines`** — change part of a file without rewriting it; indentation-tolerant match + an over-run guard so a small model can't clobber its own edit.
- **Local-model tool reliability** — greedy tool turns, cross-turn dedup, raw `<write_file>` channel, write/edit nudges, and brevity + anti-refusal prompt rules so small models actually use their tools.
- **Arrow-key popup picker** — bare `:theme` (live color swatches) and `:provider`; numbered fallback when piped.
- **256-color fallback** — detects truecolor and maps the brand palette to 256 itself on terminals without it (e.g. macOS Terminal.app), so colors stay on-brand.
- **Per-call tool permission** — `[y]/[n]/[a]` at the single exec chokepoint, every provider; `:auto` / `--yolo` to bypass.
- **MCP client** — stdio JSON-RPC (`~/.hako/mcp.json`, `mcp__server__tool`); local models see and fuzzy-resolve MCP tools.
- **Context + display** — `HAKO_CTX` default 8192; honest write chips; CommonMark underscores; spinner tracks the active theme.

See [CHANGELOG.md](CHANGELOG.md) for the full history (v0.1.4 → present).

<p align="center"><sub><b>—— VII ——</b></sub></p>

## Roadmap

- [x] Termios line editor
- [x] `--update`
- [x] directory skills + `read_skill`
- [x] universal2, Linux arm64, FreeBSD x86_64
- [x] 4 OAuth providers (Anthropic, Copilot, GH Models, OpenRouter)
- [x] Anthropic OAuth tool calls (CC-fingerprint prose mode)
- [x] local hako auto-default (`hako-sho` 3B / `hako-koi` 7B) via the hakm subprocess
- [x] [hako](https://github.com/mithraeums/hako) native engine wired as a **`hakm` subprocess** (`--chat-stdin`, one-shot per turn) — no ollama, no in-process link
- [x] `edit_file` (str_replace) + `edit_lines` (line-range) tools
- [x] local-model tool reliability — greedy tool turns, cross-turn dedup, raw `<write_file>` channel, write nudges + fence-autowrite, `HAKO_CTX`
- [x] MCP client (stdio JSON-RPC, `mcp__server__tool`, local-model visibility + fuzzy resolve)
- [x] arrow-key popup picker (`:theme` / `:provider`, live swatches) — reusable for `:model` next
- [ ] MCP client mode with Dynamic Client Registration
- [ ] Inline SHA-256 to drop openssl runtime dep
- [ ] Vim-style error codes + Buddy BLE companion approval gateway — v0.2

<p align="center"><sub><b>—— VIII ——</b></sub></p>

## Contributing
If you share the belief that simplicity empowers creativity, feel free to contribute.

### Contribution is welcome in the form of:
- Forking this repo
- Submitting a Pull Request
- Bug reports and feature requests

Please ensure your code follows the existing style.

### Thank you for your attention.
This project started out of curiosity and as a branch of [hako-edit](https://github.com/mithraeums/hako-edit), a C-based modal text editor. If you hit any issues, feel free to open an issue on GitHub.
Pull requests, suggestions, or even thoughtful discussions are welcome.

<p align="center"><sub><a href="LICENSE">— SEE LICENSE —</a> &nbsp;·&nbsp; GPL-3.0</sub></p>

<p align="center"><sub><em>— deus sol invictus mithras —</em></sub></p>

