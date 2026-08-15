# hako-code — standalone AI agent CLI (binary: `hako`). Lifted from hake.c (editor AI panel).
# Same constraints as the editor: gcc, libc, pthread, curl on PATH. No build deps beyond those.
#
# Windows: icon embedded into .exe via windres (real OS icon).
# macOS:   icon attached via Rez/SetFile if Xcode CLT installed (best-effort).
# Linux:   ELF can't embed icons; icon/hako.png shipped alongside for .desktop.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall
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
ifneq ($(STUDIO_UI),)
hako_web.h: $(STUDIO_UI) ../hako-studio/tools/embed.sh
	@sh ../hako-studio/tools/embed.sh $(STUDIO_UI) $@ ../hako-studio/icon-180.png
WEB_H := hako_web.h
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

.PHONY: all hakm clean asan icons install uninstall sign win-check
# make deletes a target when its recipe is interrupted — which, with signing in
# the recipe, meant Ctrl-C (or a hung codesign) destroyed a perfectly good binary.
.PRECIOUS: $(BIN)

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
# Regenerate icon/hako.{icns,ico,png} from icon/hako.svg.
# Requires rsvg-convert or ImageMagick. iconutil (macOS) → .icns, magick → .ico.
# Safe to run on any host; skips formats whose tool is unavailable.
icons:
	@cd $(ICON_DIR) && bash build-icons.sh

# ---------- Windows: embed icon via resource (optional — skip if .ico missing) ----------
ifeq ($(PLATFORM),windows)

HAS_ICO := $(wildcard $(ICON_DIR)/hako.ico)

ifeq ($(HAS_ICO),)
# No icon — plain build.
$(BIN): $(SRC) $(WEB_H)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)
else
# Embed icon via windres.
hako.rc:
	@printf 'IDI_ICON1 ICON "$(ICON_DIR)/hako.ico"\n' > $@

hako.res: hako.rc $(ICON_DIR)/hako.ico
	windres $< -O coff -o $@

$(BIN): $(SRC) $(WEB_H) hako.res
	$(CC) $(CFLAGS) $(SRC) hako.res -o $@ $(LDLIBS)
endif

endif

# ---------- macOS: build, then attach icon if tools exist ----------
ifeq ($(PLATFORM),macos)

$(BIN): $(SRC) $(WEB_H)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
	@# Plain build by default: compile, attach the icon, done. No keychain, no
	@# prompts, nothing for someone cloning this to think about.
	@#
	@# `make sign` (or SIGN=1) is only needed for `--serve --lan`, because macOS
	@# silently blocks inbound non-loopback connections to an unsigned binary —
	@# handshake completes, socket dead by the first read. Signing and the Rez icon
	@# are mutually exclusive (codesign refuses a resource fork), so signing drops
	@# the icon. Details: hako-studio/README.
	@if [ "$(SIGN)" != "0" ]; then \
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

$(BIN): $(SRC) $(WEB_H)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
	@if [ -f "$(ICON_DIR)/hako.png" ]; then \
		echo "built $(BIN). copy $(ICON_DIR)/hako.png to ~/.local/share/icons/ for desktop entry."; \
	else \
		echo "built $(BIN)."; \
	fi

endif

# ---------- FreeBSD: plain build ----------
ifeq ($(PLATFORM),freebsd)

$(BIN): $(SRC) $(WEB_H)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

endif

# Sign on demand — ONLY needed to serve over a LAN on macOS (`--serve --lan`).
# Prefers a self-signed "hako-dev" identity, because the firewall then matches on
# the certificate and one allow entry survives every rebuild; an ad-hoc signature
# is a bare code hash, so its allow entry dies on the next build. Falls back to
# ad-hoc when no identity exists (still no password, still no prompt).
# Signing strips the icon: codesign refuses a binary carrying a resource fork.
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

asan: $(SRC)
	$(CC) -fsanitize=address,undefined -g -O1 -Wall $< -o hako_asan $(LDLIBS)

clean:
	rm -f hako hako.exe hako.sign hako.sign.cstemp *.cstemp hako_asan hako.rc hako.res .hako.r
	rm -rf hako_asan.dSYM

# ---------- install / uninstall ----------
# Auto-pick PREFIX: $(PREFIX) override → /usr/local if writable → ~/.local.
# Strips macOS quarantine xattr post-install.
# Drops a Linux .desktop entry + PNG icon when applicable (ICONS=0 to skip).

PREFIX ?=
ICONS  ?= 1
# Signing is what lets --serve --lan accept connections: macOS silently drops
# inbound non-loopback traffic to an unsigned binary. Once a hako-dev identity
# exists, sign by default — every unsigned rebuild otherwise breaks the phone
# with no error anywhere. SIGN=0 opts out.
SIGN   ?= $(shell security find-identity -v -p codesigning 2>/dev/null | grep -c '"hako-dev"')

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
