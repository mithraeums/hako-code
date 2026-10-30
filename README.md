<p align="center">
  <a href="https://mithraeums.github.io">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://mithraeums.github.io/assets/banner-hako-code-night.png"/>
      <img src="https://mithraeums.github.io/assets/banner-hako-code.png" alt="hako-code — a standalone agent for the inner chamber" width="100%"/>
    </picture>
  </a>
</p>

<p align="center">
  <em>A coding agent in one C file. ~800 KB, no runtime, no package manager.<br/>Runs offline on a model you own — and on a phone.</em>
</p>

<p align="center">
  <a href="https://github.com/mithraeums/hako-code/releases"><img src="https://img.shields.io/badge/version-v0.3.0-e2402e?style=flat-square&labelColor=14130f" alt="v0.3.0"/></a>
  <img src="https://img.shields.io/badge/license-GPL--3.0-2636c4?style=flat-square&labelColor=14130f" alt="GPL-3.0"/>
  <img src="https://img.shields.io/badge/C99-single%20file-2636c4?style=flat-square&labelColor=14130f" alt="C99 single file"/>
  <img src="https://img.shields.io/badge/providers-14-2636c4?style=flat-square&labelColor=14130f" alt="14 providers"/>
  <img src="https://img.shields.io/badge/platforms-5-2636c4?style=flat-square&labelColor=14130f" alt="5 platforms"/>
</p>

<p align="center">
  <sub><a href="https://mithraeums.github.io">site</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako-code/releases">releases</a> &nbsp;·&nbsp; <a href="CHANGELOG.md">changelog</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako">hako (models)</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako-edit">hako-edit</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako-studio">hako-studio</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums">org</a></sub>
</p>

<br>

<!-- terminal media: real sessions through a pty, drawn in the night terminal at 2x (Code/mithraeums/.claude/web/look/media.py).
     one image per row, width only, never a height: GitHub can shrink them but never stretch them -->
<p align="center">
  <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-code/hero.gif?raw=true" alt="hako on hako-sho, the local model: it writes hello.py, asks before each tool, runs it and prints the first five primes" width="100%"/>
</p>

<p align="center">
  <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-code/menu.png?raw=true" alt="typing a colon opens the command menu under the line" width="100%"/><br/>
  <sub><code>:</code> opens the command menu under the line &nbsp;·&nbsp; <code>/</code> drives the model &nbsp;·&nbsp; every skill is <code>/&lt;skill&gt;</code></sub>
</p>

<p align="center">
  <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-code/theme-picker.gif?raw=true" alt="the theme picker previewing each theme as the highlight moves" width="100%"/><br/>
  <sub><code>:theme</code> previews as you move; the default is your terminal's own colors</sub>
</p>

<p align="center">
  <img src="https://github.com/mithraeums/mithraeums.github.io/blob/main/assets/readme-screenshots/hako-code/models.png?raw=true" alt="the model picker on openrouter, narrowed to claude with a slash" width="100%"/><br/>
  <sub><code>:model</code> lists the provider's own catalog, narrowed with <code>/</code></sub>
</p>


<table>
  <tr>
    <td width="33%" valign="top"><b>Local first.</b><br/><sub>Auto-defaults to any installed <a href="https://github.com/mithraeums/hako">hako</a> model — <code>hako-sho</code> (3B) or <code>hako-koi</code> (7B), runs on your device, no key, no cloud. The agent detects it and just works.</sub></td>
    <td width="33%" valign="top"><b>Single binary.</b><br/><sub>One C file. Builds with <code>gcc -lpthread</code>. Curl on PATH for HTTP. Nothing else linked.</sub></td>
    <td width="33%" valign="top"><b>14 cloud providers when you want them.</b><br/><sub>Anthropic OAuth (Claude Pro/Max sub), GitHub Copilot, GH Models (free), OpenRouter, plus 9 more over OpenAI-compat. Set a key or run a one-line <code>:login</code> — your call.</sub></td>
  </tr>
</table>

<p align="center"><sub><b>—— I ——</b></sub></p>

## Why this exists

Every serious coding agent ships a runtime with it. Hermes pulls Python 3.11, Node, `uv`,
ripgrep and ffmpeg — on Windows it downloads a portable Git too. Claude Code and its
neighbours want Node and npm. They are good tools; they are also a language ecosystem you
have to keep alive on every machine you want to think on.

hako is the same idea built the other way. One C file. It compiles with

```sh
gcc hako.c -o hako -lpthread
```

and the result is a **~800 KB binary (the browser UI inside) that links libc and nothing else**. It talks to thirteen
cloud providers when you want them, and to a 3B model on your own disk when you do not.
It runs on a laptop, on a FreeBSD box, on Windows, and in a 32-bit Linux emulator on an
iPhone — the same source, the same behaviour.

|                        | hako                                   | agents that ship a runtime          |
|------------------------|----------------------------------------|-------------------------------------|
| install                | one binary, ~800 KB                    | interpreter + package tree          |
| runtime deps           | libc, pthread, `curl(1)`               | Python and/or Node, plus packages   |
| build from source      | `gcc hako.c -o hako -lpthread`         | a package manager and a lockfile    |
| works with no network  | yes, on local weights                  | usually only as a client            |
| your data leaves       | only to the provider you chose         | varies                              |
| license                | GPL-3.0                                | varies                              |

The wager is simple: an agent is mostly a loop, some JSON, and careful text handling. None of
that needs a runtime. Keeping it in C is what makes it small enough to live anywhere — and
being small enough to live anywhere is the whole point.

<p align="center"><sub><b>—— II ——</b></sub></p>

## Overview

- **Local models run on the native engine — no ollama, no server.** The agent
  starts the [hako engine](https://github.com/mithraeums/hako) as a `hakm --serve`
  child that keeps the model resident and the system prompt's KV prefix warm (on
  disk too, so a cold first turn costs seconds, not minutes). Answers stream token
  by token. No in-process link, no compile flag, no daemon, no socket. Install the
  engine once (`curl -fsSL https://mithraeums.github.io/hakm.sh | sh`, or `make hakm`
  next to a checkout), `hakm pull hako-sho`, and `hako` defaults to the
  **mithraeum** provider on first launch — no key, no `:login`, no config. Small
  models speak their native Qwen `<tool_call>` dialect, and the harness repairs the
  shapes they get wrong. (ollama is also supported as a *separate* optional
  provider — `:login ollama` — for non-hako models.)
- **14 cloud providers when you want them.** Anthropic native (SSE + OAuth via Claude Pro/Max), ChatGPT sign-in (any ChatGPT plan, free included), OpenAI function-calling, GitHub Copilot, GitHub Models (free), OpenRouter (PKCE), Nous Portal (device code), and OpenAI-compat aliases for Gemini, Groq, Cerebras, DeepSeek, Mistral, Together, Fireworks, xAI/Grok, custom. Bare `:login` is a menu: sign-ins first, API keys one level down in `:login api`; `:login <name>` goes straight there.
- **An input bar that stays out of the way.** Drawn in the normal flow of the terminal — no absolute cursor moves, no scroll region — so a resize or a phone keyboard reflows cleanly. Keep typing while the model works; lines queue and run in order. History, `^R` search, kill-word, bracketed paste, `ctrl+g` to finish the line in `$EDITOR`, `ctrl+o` for the whole transcript in a pager, `ctrl+v` to paste an image (or drop its file in) for any provider that can see one.
- **Your terminal's colors by default, the suite's themes when you want them.** `theme=terminal` uses the profile's own 16 colors; `:theme <name>` swaps to one of the shared presets or a custom `theme.<name>.<slot>` theme from the rc, and bare `:theme` opens a picker that recolors as you move (enter keeps it, esc puts the old one back, and it fits even a phone-sized window). The pick is written to the rc, so hake and the studio see it too. Truecolor where supported, 256-color fallback otherwise.
- **HAKO.md project context.** Per-project `<cwd>/.hako/HAKO.md` is auto-loaded into the system prompt — the CLAUDE.md equivalent for hako-code. Binary-safe + size-capped (200KB).
- **Trust-gated tools.** `read_file`, `list_dir`, `grep`, `glob`, `write_file`, `edit_file` (str_replace), `edit_lines` (line range), `run_shell` (120s, up to 600 per call), `web_fetch`, `read_skill`. Per-call `[y]/[n]/[a]` approval, and writes show a diff of what they are about to change before you answer. Untrusted dir = all tools refused; `:trust` once per project.
- **Persistent sessions + [Agent Skills](https://agentskills.io).** Per-cwd session id, 7-day resume, append-only JSONL history. Skills follow the open standard — a folder, a `SKILL.md` with `name` and `description`, loaded by progressive disclosure — so a skill written for any compatible agent works here. Read from `./.hako/skills`, `./.claude/skills`, `./.agents/skills`, `~/.hako/skills`, `~/.claude/skills` and `~/.agents/skills`, nearest first; a skill in two places loads once. Install a whole repository of them with `:skill install owner/repo`.
- **Drop-in in a repo you already set up.** `AGENTS.md` or `CLAUDE.md` in the working directory is read as project context (after `.hako/HAKO.md`), and skills already sitting in `.claude/skills` are picked up as-is. Nothing to duplicate, nothing to migrate.
- **Drive it from your phone.** `:remote on` shares the session you are in. Sign in at hako.mithraeum.studio on any device and ☰ → code lists your sessions, one per folder; open one and you are in the same chat as the terminal, end to end encrypted (Noise XXpsk3) through a relay that only routes ciphertext and keeps no chats. Tools still ask, and the phone gets less than the desk: no `:auto`, no `:trust`, nothing outside the folder. Invite beta: the first `:remote on` links this computer to your account. Signed in on a device by email instead of a passkey? `:remote pair` shows a code; scan it there and that device holds your account key.
- **Scriptable.** `hako -p "task" --output-format json` prints one JSON object and exits 0 or 1; `-p -` takes the prompt on stdin. Enough for another tool to drive hako as a subprocess.
- **Measurable.** `hako --bench tools/bench/tools-v1.jsonl` scores tool-call reliability against any provider and prints JSON — *well-formed* (exactly one parseable call) and *correct* (right tool, right argument), including restraint tasks where calling nothing is the right answer. It runs the real parser on a real turn with execution stubbed, so the number is about hako rather than about the benchmark.
- **Asks when it should.** `ask_user` puts a real choice in front of you, rendered through the same arrow-key picker as `:model` and `:theme`. Tool approvals use it too.
- **Self-update.** `hako --update` pulls the latest GitHub release, verifies sha256, atomically replaces the binary. No reinstall, no rebuild.
- **Streaming everywhere.** Anthropic SSE, every OpenAI-compatible provider and the local engine stream live tokens, including through tool calls. The spinner names the tool in flight (`reading main.c`); `anim_style=` picks one of eight.
- **Context that does not run out.** Token accounting per provider, and at 70% of the window the middle of the conversation folds into a digest while the opening request and recent turns stay verbatim. `:compact` to force it, `:usage` to see it.
- **Skills as modes.** Skills load as one-line descriptions and fetch their body on demand, so installing ten costs almost nothing. `:use <skill>` promotes one to a mode: its full instructions go into every reply until `:use none`, and it survives a restart.

<p align="center"><sub><b>—— III ——</b></sub></p>

## Build & Run

### Build

```sh
gcc hako.c -o hako -lpthread        # one-liner

make                                      # cross-OS Makefile (-O0: fast to build, the agent waits on I/O)
make RELEASE=1                            # -O2, what releases ship
make UNIVERSAL=1                          # macOS arm64 + x86_64 fat binary
make WEB=1                                # embed hako studio, for `hako --serve`
make test                                 # the gate: no model, no network, no credentials
make asan                                 # ASan + UBSan build → ./hako_asan
```

> Deps: libc + pthread + `curl(1)` on PATH. No third-party libraries linked.
> More gates: `make test-tty` (a pty harness with a fake engine; python3 + pyte), `make test-serve`, `make fuzz` (the parsers under ASan), `make win-check`, `make wasm-check`.

### Install

```sh
curl -fsSL https://mithraeums.github.io/hako.sh | sh       # the agent
curl -fsSL https://mithraeums.github.io/install.sh | sh    # the whole suite: agent, editor, engine
```

Detects OS and arch, downloads the latest release, verifies the sha256 sidecar, drops `hako` into `/usr/local/bin` (or `~/.local/bin`). Where there is no release binary — iSh is 32-bit x86 — it builds from source instead (`apk add build-base git curl` first). No package manager. No daemon. No telemetry.

<table align="center">
  <tr>
    <td align="center"><b>macOS</b><br/><sub>universal2 · arm64 + x86_64</sub></td>
    <td align="center"><b>Linux</b><br/><sub>x86_64 · arm64</sub></td>
    <td align="center"><b>FreeBSD</b><br/><sub>x86_64</sub></td>
    <td align="center"><b>Windows</b><br/><sub>x86_64 · MinGW</sub></td>
    <td align="center"><b>iSh</b><br/><sub>i686 · built from source</sub></td>
  </tr>
</table>

### First turn

**Local, zero config (recommended)** — the engine, one model, launch:

```sh
curl -fsSL https://mithraeums.github.io/hakm.sh | sh   # the engine → ~/.hako/bin/hakm
hakm pull hako-sho                                     # 3B weights, ~1.9 GB, resumable
hako                                                   # finds them and runs locally
> :trust                                               # grant tool access in this project
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

<p align="center"><sub><b>—— IV ——</b></sub></p>

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
| `^C` | cancel the line, or stop the turn in flight |
| `^D` (empty) | EOF / exit |
| `^G` | finish the line in `$EDITOR` (`editor=` in `~/.hakorc`) |
| `^O` | the whole transcript, expanded, in a pager (`q` closes) |
| Shift+Tab | cycle ask → auto → plan |
| Tab | complete commands, names and paths; with the menu open, take the selected entry |
| Esc | close the command menu (the line stays); stop a reply being read aloud |

*Bracketed paste — multi-line pastes land as one entry.*

### Commands (inside prompt):

`:` runs hako itself; `/` drives the model. Typing either opens a menu under the line
(↑/↓ to move, Tab to take, Esc to close); the wrong prefix still works.

```
:help [cmd]   :q   :update   :remote [on|off|pair]   :mcp [reload]   :memory   :e   :o
:name <chat>   :color <name|off>   :sessions [clear [all]]   :resume <id>   :session [new]
:history   :clear   :undo   :edit   :todo [clear]
:theme [<name>]   :mode ask|auto|plan   :auto   :tools   :toolgate   :toolmode   :trust   :mouse   :editor   :voice [on|off]
:model [<id>]   :models [refresh]   :provider [<name>]   :pull <model>
:login [<prov>|api]   :logout [<prov>]   :accounts   :usage
:skills [reload]   :skill install <url>   :skill uninstall <name>   :use <skill|none>
/compact   /retry   /<skill> [text]      (every installed skill is a / command)
```

Bare `:provider` / `:model` / `:theme` (no argument) open an arrow-key picker; give an argument to set directly. `:help` lists every command from the same table the menu, the editor's Rei pane and the studio use. A command that would race a running turn (`/retry`, `:clear`, `:model`) runs as soon as the turn ends.

<p align="center"><sub><b>—— V ——</b></sub></p>

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
~/.hakorc or ~/.config/hako/hakorc         user config ($HAKORC first)
<cwd>/.hakorc                              per-project config, read once the folder is trusted
<cwd>/.hako/HAKO.md                        per-project context (auto-loaded into system prompt)
```

### `~/.hakorc`

One file for the whole suite (hako, hake, the studio): the first that exists of `$HAKORC`,
`~/.hakorc` and `~/.config/hako/hakorc`, then the project's own `.hakorc` once the folder is
trusted (it may not set `ai_api_key`, `ai_endpoint`, `ai_auto_approve`, `editor` or a
`*_cmd`). `code.<key>` is hako's own and beats `<key>`; `edit.` and `studio.` are the
others'. Unknown keys are left alone. The pickers write the one key they change, in place.

```
ai_provider=mithraeum        # or anthropic, openai, gemini, ollama, groq, … (:providers)
ai_model=hako-sho
theme=terminal               # the default: your terminal's own colors. code.theme=nord for hako only
theme.mine.base=nord         # a custom theme: a preset to start from,
theme.mine.accent=#b89656    # then any slot as #rrggbb, ansi0-15 or default
voice=0                      # 1 reads each finished reply aloud (voice_rate, voice_cmd)
update_check=1               # 0: never ask GitHub for a newer release
mouse=0                      # shared with hake; default off here, it takes the scroll wheel
ai_auto_approve=0            # 0 = ask before tools (shift+tab cycles ask/auto/plan)
show_splash=0
anim_style=pump              # braille dots bar pulse bounce ghost arrows blocks pump
editor=vim                   # for ctrl+g; else $EDITOR
shell_timeout=120            # run_shell deadline in seconds (max 600)
ai_max_tokens=4096
ai_stream=1
prompt_cache=1               # Anthropic prompt caching
context_window=0             # override the model's window (0 = from the catalog)
```

A key set here beats a stale value in `~/.hako/state`; a project's own saved state still
wins inside that project.

**Three auth paths:**

- **OAuth (subscription / account-bound)** — `:login anthropic` (Claude Pro/Max), `:login copilot` (GitHub Copilot Pro/Business), `:login github-models` (free for any GitHub user), `:login openrouter` (PKCE, auto-issue an OR API key), `:login nous` (Nous Portal, device code). Inference bills against your subscription where applicable; no per-token API key needed.
- **API-key paste** — `:login openai`, `:login gemini`, `:login groq`, `:login cerebras`, `:login deepseek`, `:login mistral`, `:login together`, `:login fireworks`, `:login xai`, `:login anthropic-api`, `:login openrouter-api`, `:login nous-api`, `:login custom`. Opens provider console in your browser, prompts with input hidden, persists into the cred store.
- **Local** — hako's own models need no login: `hakm pull hako-sho`, then `:provider mithraeum`. Ollama works too: `:login ollama` (ensure `ollama serve` is running) or `:login ollamacloud` (paste an Ollama key).

Run `:providers` for the full grouped list with `◎` (active) and `*` (saved login) markers. `:models` lists the active provider's live catalog (its own listing endpoint, cached 24h — `:models refresh` refetches; installed `.mlf2` weights for **mithraeum**), falling back to a built-in list when offline. Bare `:model` opens the picker, where `/` filters as you type. `:accounts` lists saved logins; `:logout [<provider>]` wipes one. Mid-chat `:provider X` swaps secrets and flattens wire-format-specific message bodies so the conversation survives.

Resolution order: `HAKO_API_KEY` env → `<PROVIDER>_API_KEY` env → `~/.hako/credentials` (per provider). Legacy `CLAW_*` / `HAKOC_*` env names are read as fallback with a one-shot deprecation warning.

First run in any directory asks for trust. Untrusted = no tool access at all.

### Connection Matrix

| Provider          | id (`:provider <id>`)            | Auth          | Cost     | Wire format       | Notes                                        |
|---                |---                               |---            |---       |---                |---                                           |
| Anthropic         | `anthropic` · `claude`           | OAuth         | sub      | native + SSE      | Claude Pro/Max subscription; live-tested     |
| ChatGPT           | `chatgpt` · `codex`              | OAuth         | sub      | OpenAI Responses  | any ChatGPT plan, free too; device sign-in   |
| Anthropic API     | `anthropic-api` · `claude-api`   | paste         | $/tok    | native + SSE      | regular API key, separate from sub           |
| GitHub Copilot    | `copilot` · `github-copilot`    | OAuth         | sub      | OpenAI-compat     | Copilot Pro/Business; device flow            |
| GitHub Models     | `github-models` · `ghmodels`    | OAuth         | free     | OpenAI-compat     | rate-limited; any GH account                 |
| OpenRouter        | `openrouter`                     | OAuth (PKCE)  | $/tok    | OpenAI-compat     | auto-issues user-scoped key; `:free` tier    |
| OpenRouter API    | `openrouter-api`                 | paste         | $/tok    | OpenAI-compat     | paste existing key                           |
| Nous Portal       | `nous`                           | device code   | $/tok    | OpenAI-compat     | many vendors' models; public catalog         |
| Nous Portal API   | `nous-api`                       | paste         | $/tok    | OpenAI-compat     | paste a key from portal.nousresearch.com     |
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

*Three wire formats (native Anthropic, native Ollama, OpenAI-compat).  Quickest paths: (a) **Claude Pro/Max** → `:login anthropic`. (b) **ChatGPT** (free or paid) → `:login chatgpt`. (c) **Copilot Pro** → `:login copilot`. (d) **Free, no card** → `:login github-models`, `:login gemini`, `:login groq`, or `:login ollama`. (e) **Pay-as-you-go** → any paste row.*

### Tool modes

Some smaller / non-tool-tuned models (Mistral 7B, Phi-4, DeepSeek-R1 distills, smaller Gemma / Llama variants) don't reliably emit OpenAI/Anthropic function-calling JSON. Toggle ReAct mode:

```sh
:toolmode prose        # model emits <tool name="X">{...}</tool> blocks in prose (aliases: react, xml)
:toolmode native       # back to native function-calling (default)
```

ReAct mode injects a tool schema in the system prompt and parses `<tool>...</tool>` blocks from the model's response. Each tool call is executed and the result is appended as `<observation tool="X">...</observation>` for the next turn. Works with any instruct-tuned model.

<p align="center"><sub><b>—— VI ——</b></sub></p>

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

```
:skill install mithraeums/skills      # every skill in the repo, into ~/.hako/skills
:skills                               # what is loaded
:use brief                            # one of them, as a mode, until :use none
```

Browse the catalog: [mithraeums/skills](https://github.com/mithraeums/skills).

<p align="center"><sub><b>—— VII ——</b></sub></p>

## Change Log

### Latest — v0.3.0
- **One rc for the whole suite.** The first of `$HAKORC`, `~/.hakorc` and `~/.config/hako/hakorc`, with the project's own `.hakorc` on top once the folder is trusted. `code.`, `edit.` and `studio.` keys belong to one tool; custom themes live there too.
- **A command menu.** Type `:` or `/` and a list opens under the line. `:` runs hako, `/` drives the model, and every installed skill is `/<skill>`. hake's Rei pane and the studio draw the same menu.
- **Your terminal's colors by default.** `terminal` is the default theme, and a theme you pick is written to the rc, so every tool sees it.
- **`:voice`** reads each reply aloud with the computer's own voice; under `:remote` the phone speaks instead.
- **The bar says more.** `update <version>` when a newer release is out (`:update` says how), and `remote` while the session is shared.
- **A leaner model context.** A file written in an earlier turn goes to the model as one line, not the whole file on every request.
- **A safer remote.** A phone can no longer set `:editor` and run it on the computer.
- Ships with **hako studio v0.1.0** (a new menu, a landing for an empty chat, replies in markdown, read aloud).

Every earlier release, and the full detail, is in **[CHANGELOG.md](CHANGELOG.md)**.

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

