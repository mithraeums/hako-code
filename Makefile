# hako-code — standalone AI agent CLI (binary: `hako`). Lifted from hake.c (editor AI panel).
# Same constraints as the editor: gcc, libc, pthread, curl on PATH. No build deps beyond those.
#
# Windows: icon embedded into .exe via windres (real OS icon).
# macOS:   icon attached via Rez/SetFile if Xcode CLT installed (best-effort).
# Linux:   ELF can't embed icons; icon/hako.png shipped alongside for .desktop.

.DEFAULT_GOAL := all
# Must be set before any rule: the hako_web.h rule below is the first explicit
# target in this file, so without this `make` with a hako-studio checkout next
# door regenerates the bundle and stops, never building the binary.

CC      ?= gcc
# -O0 by default. One translation unit means every build is a full build, and
# -O2 on 14k lines under an emulator (iSh) takes minutes. This program waits on
# the network and the disk, not the CPU — only grep/glob are compute-bound, and
# those are milliseconds either way. Releases are built with RELEASE=1 (below),
# so shipped binaries are still -O2.
CFLAGS  ?= -O0 -Wall
ifeq ($(RELEASE),1)
CFLAGS  := -O2 -Wall
endif
LDLIBS  ?= -lpthread

ICON_DIR = icon
SRC      = hako.c
BIN      = hako

# The embedded hako studio bundle (UI + home-screen icon), generated from
# hako-studio by `make embed` there. A prerequisite, not just an include: without
# it a UI or icon change silently kept shipping the previous build's bundle.
WEB_H     := $(wildcard hako_web.h)

# hako_web.h is generated, and generated files that are committed go stale. It
# is committed anyway so a clone of this repo alone builds a binary with the UI
# in it — no second repo, no network, no submodule. The drift is closed from the
# other side instead: if a hako-studio checkout is next door, regenerate before
# building, so the two can never disagree on a machine that has both.
STUDIO_UI := $(wildcard ../hako-studio/index.html)
ifeq ($(WEB),1)
CFLAGS += -DHAKO_EMBED_WEB
endif
ifneq ($(STUDIO_UI),)
ifeq ($(WEB),1)
hako_web.h: $(STUDIO_UI) ../hako-studio/tools/embed.sh
	@sh ../hako-studio/tools/embed.sh $(STUDIO_UI) $@ ../hako-studio/icon-180.png
WEB_H := hako_web.h
endif
endif

# Local hako-model inference runs through the standalone `hakm` engine binary as
# a subprocess — NOT linked into this agent. The agent build is therefore plain
# (`make` / `all`): there is no compile-time engine flag that could be omitted,
# so a build can never silently lose MITHRAEUM support (the v0.1.6 trap).
# `make hakm` is a convenience that builds the engine CLI in the hako repo (../hako)
# and installs it to ~/.hako/bin/hakm, where the agent's hkFindHakm() looks for it.
HAKM_DIR ?= ../hako

ifeq ($(OS),Windows_NT)
    PLATFORM = windows
    BIN := hako.exe
    LDLIBS += -lws2_32
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM = macos
    else ifeq ($(UNAME_S),FreeBSD)
        PLATFORM = freebsd
    else
        PLATFORM = linux
    endif
endif

# macOS universal2 build of the agent (UNIVERSAL=1). The engine binary has its
# own Makefile/arch handling in $(HAKM_DIR); this only affects the agent.
ifeq ($(PLATFORM),macos)
    ifeq ($(UNIVERSAL),1)
        CFLAGS += -arch arm64 -arch x86_64
    endif
endif

.PHONY: test-relay all hakm clean asan icons install uninstall sign win-check wasm-check themes-check test test-tty test-serve fuzz
# make deletes a target when its recipe is interrupted — which, with signing in
# the recipe, meant Ctrl-C (or a hung codesign) destroyed a perfectly good binary.
.PRECIOUS: $(BIN)

# A flag change (WEB=1, a -D) must rebuild; make tracks files, not flags.
# Checked at parse time, and a change drops the old binary: make 3.81 (macOS)
# never re-stats a FORCE'd stamp and compares mtimes to the second.
HAKO_FLAGS := $(CC) $(CFLAGS) $(LDLIBS)
$(shell echo '$(HAKO_FLAGS)' | cmp -s - .hako.flags 2>/dev/null || { echo '$(HAKO_FLAGS)' > .hako.flags; rm -f $(BIN) $(BIN).exe; })

all: $(BIN)


# ---------- local-model engine (standalone subprocess, ollama-free) ----------
# Builds the `hakm` engine CLI in $(HAKM_DIR) and installs it to ~/.hako/bin,
# where hkFindHakm() looks first. This is decoupled from the agent build: update
# the engine independently, no relink. The agent itself does NOT need this to
# build — only to run local hako models.
hakm:
	$(MAKE) -C $(HAKM_DIR) hakm
	@mkdir -p "$$HOME/.hako/bin"
	@install -m 0755 "$(HAKM_DIR)/hakm" "$$HOME/.hako/bin/hakm"
	@echo "installed engine: $$HOME/.hako/bin/hakm (subprocess runtime, no ollama). models: ~/.hako/models/*.mlf2"

# ---------- icons ----------
# icon/ is generated from the suite's shared masters (one chassis, one glyph per
# product), which live beside the repos, not in them. Needs rsvg-convert.
icons:
	@[ -f ../assets/icons/build.sh ] && sh ../assets/icons/build.sh hako \
	  || echo "icons: the masters live in ../assets/icons (the suite checkout), not in this repo"

# ---------- Windows: embed icon via resource (optional — skip if .ico missing) ----------
ifeq ($(PLATFORM),windows)

HAS_ICO := $(wildcard $(ICON_DIR)/hako.ico)

ifeq ($(HAS_ICO),)
# No icon — plain build.
$(BIN): $(SRC) $(WEB_H) .hako.flags
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)
else
# Embed icon via windres.
hako.rc:
	@printf 'IDI_ICON1 ICON "$(ICON_DIR)/hako.ico"\n' > $@

hako.res: hako.rc $(ICON_DIR)/hako.ico
	windres $< -O coff -o $@

$(BIN): $(SRC) $(WEB_H) .hako.flags hako.res
	$(CC) $(CFLAGS) $(SRC) hako.res -o $@ $(LDLIBS)
endif

endif

# ---------- macOS: build, then attach icon if tools exist ----------
ifeq ($(PLATFORM),macos)

$(BIN): $(SRC) $(WEB_H) .hako.flags
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
	@# Plain build by default: compile, attach the icon, done. No keychain, no
	@# prompts, nothing for someone cloning this to think about.
	@#
	@# `make sign` (or SIGN=1) is only needed for `--serve --lan`, because macOS
	@# silently blocks inbound non-loopback connections to an unsigned binary —
	@# handshake completes, socket dead by the first read. Signing and the Rez icon
	@# are mutually exclusive (codesign refuses a resource fork), so signing drops
	@# the icon. Details: hako-studio/README.
	@# The condition below used to be `!= "0"`, i.e. sign unless told not to,
	@# which contradicted the paragraph above and prompted for the keychain on
	@# every single build. Opt IN instead: `make sign` or `make SIGN=1`.
	@if [ "$(SIGN)" = "1" ]; then \
		$(MAKE) --no-print-directory sign; \
	elif [ -f "$(ICON_DIR)/hako.icns" ] && command -v Rez >/dev/null 2>&1 && command -v SetFile >/dev/null 2>&1; then \
		printf 'read %c%s%c (-16455) "%s/hako.icns";\n' "'" "icns" "'" "$(ICON_DIR)" > .hako.r; \
		Rez -append .hako.r -o $(BIN) && SetFile -a C $(BIN) && \
		echo "built $(BIN) (icon attached)" || echo "built $(BIN) (icon attach failed — harmless)"; \
		rm -f .hako.r; \
	else \
		echo "built $(BIN)."; \
	fi

endif

# ---------- Linux: plain build, icon shipped alongside ----------
ifeq ($(PLATFORM),linux)

$(BIN): $(SRC) $(WEB_H) .hako.flags
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
	@if [ -f "$(ICON_DIR)/hako.png" ]; then \
		echo "built $(BIN). copy $(ICON_DIR)/hako.png to ~/.local/share/icons/ for desktop entry."; \
	else \
		echo "built $(BIN)."; \
	fi

endif

# ---------- FreeBSD: plain build ----------
ifeq ($(PLATFORM),freebsd)

$(BIN): $(SRC) $(WEB_H) .hako.flags
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

endif

# Sign on demand — ONLY needed to serve over a LAN on macOS (`--serve --lan`).
# Prefers a self-signed "hako-dev" identity, because the firewall then matches on
# the certificate and one allow entry survives every rebuild; an ad-hoc signature
# is a bare code hash, so its allow entry dies on the next build. Falls back to
# ad-hoc when no identity exists (still no password, still no prompt).
# Signing strips the icon: codesign refuses a binary carrying a resource fork.
# Needed only for `--serve --lan` — macOS silently drops inbound non-loopback
# connections to an unsigned binary. Everything else works unsigned.
sign: $(BIN)
	@xattr -c $(BIN) 2>/dev/null || true
	@id=$$(security find-identity -v -p codesigning 2>/dev/null | grep -o '"hako-dev"' | head -1 | tr -d '"'); \
	cp $(BIN) $(BIN).sign; \
	xattr -c $(BIN).sign 2>/dev/null || true; \
	echo "signing (trust evaluation can take ~30s the first time)…"; \
	if [ -n "$$id" ]; then \
		perl -e 'alarm 120; exec @ARGV' codesign -s "$$id" -f --timestamp=none -i com.mithraeum.hako $(BIN).sign 2>/dev/null \
			&& mv $(BIN).sign $(BIN) && echo "signed with $$id — firewall allow survives rebuilds" \
			|| { rm -f $(BIN).sign; echo "codesign timed out/failed; $(BIN) left as-is"; }; \
	else \
		perl -e 'alarm 120; exec @ARGV' codesign -s - -f --timestamp=none -i com.mithraeum.hako $(BIN).sign 2>/dev/null \
			&& mv $(BIN).sign $(BIN) && echo "ad-hoc signed — re-run 'make sign' + re-allow after each build" \
			|| { rm -f $(BIN).sign; echo "codesign failed; $(BIN) left as-is"; }; \
	fi
	@echo "then allow it once:"
	@echo "  sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add $(PWD)/$(BIN)"
	@echo "  sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp $(PWD)/$(BIN)"

# Unit tests for the search tools. Worth running on every target you can reach —
# this is the code with the most platform surface in the agent, and the whole
# point of it is that a 32-bit musl box behaves like the desk machine.
# The THEMES block in hako.c is generated from the editor's table (hako-edit/tools/themes.py).
THEMES_PY := $(wildcard ../hako-edit/tools/themes.py)
themes-check:
	@if [ -n "$(THEMES_PY)" ]; then python3 $(THEMES_PY) --check; else echo "themes: no ../hako-edit checkout — skipped"; fi

test: tools/tests.c $(SRC) themes-check
	$(CC) -O1 -Wall -Wextra -DHAKO_RELAY -Dmain=hako_cli_main -I. tools/tests.c -o /tmp/hako_tests $(LDLIBS)
	@/tmp/hako_tests

# Two devices through a real relay on loopback: sign-in, Noise XX + the six
# digits, a big frame, offline notices, refused tokens and proofs.
RELAY      ?= ../hako-studio/relay/relay
RELAY_PORT ?= 18790
test-relay: tools/tests.c $(SRC)
	@[ -x "$(RELAY)" ] || $(MAKE) -C $(dir $(RELAY)) relay
	$(CC) -O1 -Wall -Wextra -DHAKO_RELAY -Dmain=hako_cli_main -I. tools/tests.c -o /tmp/hako_tests $(LDLIBS)
	@d=$$(mktemp -d /tmp/hako-relay.XXXXXX); inv=$$($(RELAY) invite 1 -d $$d); \
	$(RELAY) -p $(RELAY_PORT) -d $$d --dev 2>$$d/log & rp=$$!; sleep 0.3; \
	/tmp/hako_tests relay http://127.0.0.1:$(RELAY_PORT) $$inv $$d/log; r=$$?; kill $$rp; wait $$rp 2>/dev/null; rm -rf $$d; exit $$r

asan: $(SRC)
	$(CC) -fsanitize=address,undefined -g -O1 -Wall $< -o hako_asan $(LDLIBS)

clean:
	rm -f hako hako.exe hako.sign hako.sign.cstemp *.cstemp hako_asan hako.rc hako.res .hako.r .hako.flags
	rm -rf hako_asan.dSYM

# ---------- install / uninstall ----------
# Auto-pick PREFIX: $(PREFIX) override → /usr/local if writable → ~/.local.
# Strips macOS quarantine xattr post-install.
# Drops a Linux .desktop entry + PNG icon when applicable (ICONS=0 to skip).

PREFIX ?=
ICONS  ?= 1
# Signing is what lets --serve --lan accept connections: macOS silently drops
# inbound non-loopback traffic to an unsigned binary.
#
# Default OFF. This used to auto-enable whenever a `hako-dev` identity existed,
# so anyone who had ever set one up got a keychain prompt on every build —
# including the vast majority that never serve over the LAN. Signing also
# strips the icon. `make sign` (or `make SIGN=1`) when you actually need it;
# the `--serve --lan` banner says so if a phone cannot connect.
SIGN   ?= 0

_uname_s := $(shell uname -s 2>/dev/null)
_resolve_prefix = $(if $(PREFIX),$(PREFIX),$(if $(shell test -w /usr/local/bin && echo y),/usr/local,$(HOME)/.local))

install: $(BIN)
	@dest="$(_resolve_prefix)"; \
	mkdir -p "$$dest/bin"; \
	install -m 0755 $(BIN) "$$dest/bin/$(BIN)"; \
	echo "installed: $$dest/bin/$(BIN)"; \
	if [ "$(_uname_s)" = "Darwin" ] && command -v xattr >/dev/null 2>&1; then \
		xattr -d com.apple.quarantine "$$dest/bin/$(BIN)" 2>/dev/null || true; \
	fi; \
	if [ "$(_uname_s)" = "Linux" ] && [ "$(ICONS)" = "1" ] && [ -f icon/hako.png ]; then \
		mkdir -p "$$HOME/.local/share/applications" "$$HOME/.local/share/icons/hicolor/256x256/apps"; \
		install -m 0644 icon/hako.png "$$HOME/.local/share/icons/hicolor/256x256/apps/hako.png"; \
		printf "[Desktop Entry]\nType=Application\nName=hako\nComment=Mithraeum terminal AI agent\nExec=$$dest/bin/$(BIN)\nIcon=hako\nTerminal=true\nCategories=Development;Utility;\n" > "$$HOME/.local/share/applications/hako.desktop"; \
		echo "installed: icon + .desktop entry"; \
	fi; \
	case ":$$PATH:" in *":$$dest/bin:"*) ;; *) echo "note: $$dest/bin not in PATH";; esac

uninstall:
	@for prefix in $(PREFIX) /usr/local $$HOME/.local /opt/local /opt; do \
		[ -z "$$prefix" ] && continue; \
		path="$$prefix/bin/$(BIN)"; \
		if [ -e "$$path" ] || [ -L "$$path" ]; then rm -f "$$path" && echo "removed: $$path"; fi; \
	done
	@rm -f "$$HOME/.local/share/applications/hako.desktop" 2>/dev/null || true
	@for d in $$HOME/.local/share/icons/hicolor/*/apps; do \
		[ -d "$$d" ] && rm -f "$$d/hako.png" 2>/dev/null; \
	done
	@echo "(use \`rm -rf ~/.hako ~/.hakorc\` to purge state/credentials)"

# Cross-compile for Windows without leaving the desk. This build has broken on
# other platforms before — a missing feature macro, a POSIX name the Windows CRT
# does not ship — and each time it was found by CI on a tag, which is the worst
# place to find it. Needs mingw-w64 (brew install mingw-w64); skipped if absent.
win-check:
	@if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then \
		x86_64-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra $(SRC) -o /tmp/hako-wincheck.exe -lws2_32 -lpthread && \
		echo "windows: builds"; \
	else echo "windows: skipped (no mingw-w64)"; fi

# The terminal UI over a real pty against a fake streaming engine. Catches the
# bugs a human catches by eye: text on the input row, bar gone mid-stream,
# stacked rules, raw <tool_call> after a repaint, :q that does not quit.
# Needs python3 + pyte (pip install pyte). No weights, no network.
test-tty: hako
	HAKO_BIN="$(CURDIR)/hako" python3 tools/tty/harness.py

# The browser build (hako studio) is this same file compiled for wasm. Nothing
# else compiles it, and it broke for two months unnoticed. Needs wasi-sdk
# (`make toolchain` in ../hako-studio); skipped with a note when it is absent.
WASI_CC ?= $(firstword $(wildcard $(HOME)/.hako/toolchains/wasi-sdk-*/bin/clang))
wasm-check:
	@if [ -z "$(WASI_CC)" ]; then echo "wasm-check: no wasi-sdk (make toolchain in ../hako-studio) — skipped"; else \
		$(WASI_CC) --target=wasm32-wasip1 --sysroot=$(dir $(WASI_CC))../share/wasi-sysroot -O2 \
		  -D_WASI_EMULATED_GETPID -Werror=implicit-function-declaration -fsyntax-only $(SRC) \
		  && echo "wasm-check: hako.c compiles for the browser"; fi

test-serve: hako
	HAKO_BIN="$(CURDIR)/hako" sh tools/serve-test.sh

# Mutation fuzz of the model-output parsers under ASan+UBSan. FUZZ_ITERS / FUZZ_SEED.
fuzz: tools/fuzz.c $(SRC)
	$(CC) -fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1 -Wall -Wextra \
	  -Dmain=hako_cli_main -I. tools/fuzz.c -o /tmp/hako_fuzz $(LDLIBS)
	@cd /tmp && ASAN_OPTIONS=detect_leaks=0 /tmp/hako_fuzz
