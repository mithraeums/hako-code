<p align="center">
  <a href="https://mithraeums.github.io">
    <img src="https://mithraeums.github.io/assets/banner-hako-code-dark.svg" alt="hako-code — a standalone agent for the inner chamber" width="100%"/>
  </a>
</p>

<p align="center">
  <em>A standalone terminal AI agent in a single C file. Quiet, model-agnostic, skill-driven.</em>
</p>

<p align="center">
  <a href="https://github.com/mithraeums/hako-code/releases"><img src="https://img.shields.io/badge/version-v0.1.6-b89656?style=flat-square&labelColor=14130f" alt="v0.1.6"/></a>
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
    <td align="center" width="50%">
      <!-- SCREENSHOT: hako-code/screenshot-splash.png — hako agent banner box at startup, koi auto-detected, mithraeum provider. -->
      <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-code/screenshot-splash.png?raw=true" alt="hako banner with koi defaulted" width="100%"/><br/>
      <sub>auto-defaults to a local <b>hako</b> model via mithraeum</sub>
    </td>
    <td align="center" width="50%">
      <!-- SCREENSHOT: hako-code/screenshot-models.png — `:models` output showing the catalog (mithraeum koi + cloud providers). -->
      <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-code/screenshot-models.png?raw=true" alt=":models output" width="100%"/><br/>
      <sub><code>:models</code> · local hako + 13 cloud providers</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <!-- REUSED from hako-edit — Rei pane shows the agent embedded in the editor, talking to koi. Same binary, different host. -->
      <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-edit/screenshot-rei.png?raw=true" alt="hako inside hake editor" width="100%"/><br/>
      <sub>also runs embedded inside <a href="https://github.com/mithraeums/hako-edit"><b>hake</b></a> (the editor)</sub>
    </td>
    <td align="center" width="50%">
      <!-- REUSED from hako-edit themes capture — same palette tokens drive both editor + agent. -->
      <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-edit/screenshot-themes.png?raw=true" alt="theme palette" width="100%"/><br/>
      <sub>themes · mithraeum · claude · nord · mono</sub>
    </td>
  </tr>
</table>

<br>

<!-- KEEP: ASCII mockup of the in-terminal banner. Renders fine in monospace; do not replace with a screenshot. Mirrors the cleaned 13-row logo (no TABs, no mixed ASCII spaces) — keep this and `CL_LOGO_MEDIUM` in lockstep when either changes. -->

```
╭───────────────────────────────────────────╮
│        ⠀⠀⠀⠀⠀⠀⠀⢀⣀⡀⠀⠀⠀⠀⠀⠀⢀⣀⡀⠀⠀⠀⠀⠀⠀⠀         │
│        ⠀⠀⠀⢀⣠⣶⣾⣿⣿⣿⣦⡀⠀⠀⢀⣴⣿⣿⣿⣷⣶⣄⡀⠀⠀⠀         │
│        ⣠⣴⣾⣿⣿⣿⣿⣿⣿⠿⠛⠉⢠⡄⠉⠛⠿⣿⣿⣿⣿⣿⣿⣷⣦⣄         │
│        ⠀⠻⣿⣿⣿⡿⠟⠋⠁⠀⠀⠀⢸⡇⠀⠀⠀⠈⠙⠻⢿⣿⣿⣿⠟⠁         │
│        ⠀⠀⠈⠋⠁⠀⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠈⠙⠁⠀⠀         │
│        ⠀⠀⣰⣷⣦⣄⡀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⢀⣠⣴⣾⣆⠀⠀         │
│        ⢠⣾⣿⣿⣿⣿⣿⣷⣦⣄⠀⠀⢸⡇⠀⠀⣠⣴⣾⣿⣿⣿⣿⣿⣷⡄         │
│        ⠙⠿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠖⠀⠀⠲⣿⣿⣿⣿⣿⣿⣿⣿⣿⠿⠋         │
│        ⠀⠀⠀⠉⠛⠿⣿⣿⣿⠟⢁⣴⡇⢸⣦⡈⠻⣿⣿⣿⠿⠛⠉⠀⠀⠀         │
│        ⠀⠀⠀⢸⣷⣦⣄⡉⢁⣴⣿⣿⡇⢸⣿⣿⣦⡈⢉⣠⣴⣾⡇⠀⠀⠀         │
│        ⠀⠀⠀⠈⠙⠻⢿⣿⣿⣿⣿⣿⡇⢸⣿⣿⣿⣿⣿⡿⠟⠋⠁⠀⠀⠀         │
│        ⠀⠀⠀⠀⠀⠀⠀⠈⠙⠻⢿⣿⡇⢸⣿⡿⠟⠋⠁⠀⠀⠀⠀⠀⠀⠀         │
│        ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⠁⠈⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀         │
│                                           │
│ hako 0.1.6 · mithraeum · hako-sho-stock   │
│ trust on · session resumed                │
│ :help :providers :models :login :theme    │
╰───────────────────────────────────────────╯
```


<table>
  <tr>
    <td width="33%" valign="top"><b>Local first.</b><br/><sub>Auto-defaults to any installed <a href="https://github.com/mithraeums/hako">hako</a> model — <code>hako-sho-stock</code> (3B) or <code>hako-koi-mini-stock</code> (7B), runs on your device, no key, no cloud. The agent detects it and just works.</sub></td>
    <td width="33%" valign="top"><b>Single binary.</b><br/><sub>One C file. Builds with <code>gcc -lpthread</code>. Curl on PATH for HTTP. Nothing else linked.</sub></td>
    <td width="33%" valign="top"><b>13 cloud providers when you want them.</b><br/><sub>Anthropic OAuth (Claude Pro/Max sub), GitHub Copilot, GH Models (free), OpenRouter, plus 9 more over OpenAI-compat. Set a key or run a one-line <code>:login</code> — your call.</sub></td>
  </tr>
</table>

<p align="center"><sub><b>—— I ——</b></sub></p>

## Overview

- **Mithraeum local model as the default.** If any `hako-*` model is in your local list, `hako` auto-defaults to the **mithraeum** provider on first launch — no key, no `:login`, no config (preference: koi > koi-mini > sho, fine-tunes over stock-wraps). Force prose tool mode is on for these models so `<tool name="...">{...}</tool>` calls parse correctly. Pull one with `hakm pull sho` (or `koi-mini`) from [mithraeums/hako](https://github.com/mithraeums/hako), or use the one-line installer `curl -fsSL https://mithraeums.github.io/hakm.sh | sh`.
- **13 cloud providers when you want them.** Anthropic native (SSE + OAuth via Claude Pro/Max), OpenAI function-calling, GitHub Copilot, GitHub Models (free), OpenRouter (PKCE), and OpenAI-compat aliases for Gemini, Groq, Cerebras, DeepSeek, Mistral, Together, Fireworks, xAI/Grok, custom. `:login <name>` walks you through OAuth or hands you to the provider's console for an API key.
- **Terminal-class line editor.** Termios raw mode, cursor keys, history (↑ ↓), `^R` reverse-search, Home/End, kill-word, kill-line, bracketed paste. Multi-row aware redraw, no flicker.
- **Theme presets.** `:theme mithraeum|claude|nord|mono` swaps the full palette live (persisted in `~/.hakorc`). Truecolor where supported, 16-color fallback for error chip in tmux without `RGB` passthrough.
- **HAKO.md project context.** Per-project `<cwd>/.hako/HAKO.md` is auto-loaded into the system prompt — the CLAUDE.md equivalent for hako-code. Binary-safe + size-capped (200KB).
- **Trust-gated tools.** `read_file`, `list_dir`, `write_file` (with staging), `run_shell` (10s timeout). Untrusted dir = all tools refused. `:trust` once per project.
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

**Local, zero config (recommended)** — pull a Mithraeum hako model, then launch:

```sh
curl -fsSL https://mithraeums.github.io/hakm.sh | sh   # installs hako-sho-stock (3B) + hako-koi-mini-stock (7B)
hako                                                   # auto-detects a hako model, defaults to mithraeum provider
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
    └── .claude/agents/
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

### v0.1.6 (Latest)<br>
- **Anthropic OAuth tool calls work.** System-as-array, CC fingerprint headers, prose-mode tool calls (Claude Code XML parser), JSON-unescape on `write_file` content. Verified live against Claude Pro/Max.
- **Storage rewrite — CC-style.** Per-project state at `~/.hako/projects/<encoded-cwd>/{trust,state,sessions/<sid>.jsonl}`. `<cwd>/.hako/` only holds `HAKO.md` (project context, system-prompt-loaded). Old per-project files trigger startup warn.
- **Theme system** — `:theme mithraeum|claude|nord|mono` swap + persist.
- **Visual overhaul** — sigils (`›` user, `◆` AI, `!` err, `·` sys), box banner, first-run wizard, ghost-text autocomplete for slash/colon cmds.
- **Tool resolution hardened** — 20+ tool-name aliases case-insensitive (`bash`, `create_file`, `str_replace_based_edit_tool`, etc), param-name aliases (`command`→cmd, `file_path`→path, `file_text`→content) so CC-trained Claude works first try.
- **`run_shell` rewrite** — tmp-script execution + JSON-unescape. Heredocs and embedded quotes now work.
- **OPENAI + OLLAMA wires now carry `system_prompt`** — was silently dropped; BASE_PROMPT / skills / HAKO.md now reach Copilot / GH Models / OpenRouter / Gemini / Ollama / koi.
- **Pre-tool prose suppression** — matches Claude Code chip-first ordering; no more "Done!" before the tool runs.
- **Cached env probe** in system prompt — model knows `python3` vs `python` etc on first try.
- **Ollama perf knobs** — `keep_alive: 30m` + `num_predict` cap. Big win on local koi turns.
- **Error chip** — bold 16-color bright red + `✗` glyph (was rendering as invisible reverse-video bar in tmux).
- **Banner box auto-sizes** correctly (was undercounting multibyte `·`). Rows truncate with `…`.

### v0.1.5<br>
- **4 OAuth providers** — `:login anthropic` (Claude Pro/Max), `:login copilot` (Copilot Pro), `:login github-models` (free for any GH user), `:login openrouter` (PKCE auto-issue).
- **Discovery** — `:providers` grouped catalog, `:models` live (Ollama) or curated suggestions (other providers), TAB completion.
- **Workflow parity** — `:retry`, `:edit`, `:undo`, up-arrow recall.
- **ReAct tool fallback** — `:toolmode react` for smaller / non-tool-tuned models (Mistral 7B, Phi-4, DeepSeek-R1 distills).
- **Cost tracking** — per-provider USD/M-token price table, session `$` in status line, `:usage reset`.
- **Rendering** — tool category glyphs, fenced code blocks with bronze gutter, last-turn latency in status line.
- **Per-provider cred store** — `~/.hako/credentials` (XOR + base64, 0600). `:accounts`, `:logout`.
- **Mid-chat provider swap** — `:provider X` flattens wire-format-specific bodies so conversation survives Anthropic↔OpenAI↔Ollama jumps.

### v0.1.4<br>
- **`--pipe` mode** for hako integration — JSONL I/O over stdin/stdout.
- **Mithraeum palette by default** — gold / paper / rust / dim chalk truecolor.

See [CHANGELOG.md](CHANGELOG.md) for full history.

<p align="center"><sub><b>—— VII ——</b></sub></p>

## Roadmap

- [x] Termios line editor
- [x] `--update`
- [x] directory skills + `read_skill`
- [x] universal2, Linux arm64, FreeBSD x86_64
- [x] 4 OAuth providers (Anthropic, Copilot, GH Models, OpenRouter)
- [x] Anthropic OAuth tool calls (CC-fingerprint prose mode)
- [x] local hako auto-default (sho-stock / koi-mini-stock) + tool-aware Modelfile
- [ ] [hako](https://github.com/mithraeums/hako) direct-wire `koi` engine (today routes through ollama) — v0.1.7
- [ ] MCP client mode with Dynamic Client Registration — v0.1.7
- [ ] Inline SHA-256 to drop openssl runtime dep — v0.1.7
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

