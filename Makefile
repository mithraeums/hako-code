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

# macOS universal2 toggle: make UNIVERSAL=1 → fat binary (arm64 + x86_64).
ifeq ($(PLATFORM),macos)
    ifeq ($(UNIVERSAL),1)
        CFLAGS += -arch arm64 -arch x86_64
    endif
endif

.PHONY: all clean asan icons install uninstall

all: $(BIN)

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
$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)
else
# Embed icon via windres.
hako.rc:
	@printf 'IDI_ICON1 ICON "$(ICON_DIR)/hako.ico"\n' > $@

hako.res: hako.rc $(ICON_DIR)/hako.ico
	windres $< -O coff -o $@

$(BIN): $(SRC) hako.res
	$(CC) $(CFLAGS) $(SRC) hako.res -o $@ $(LDLIBS)
endif

endif

# ---------- macOS: build, then attach icon if tools exist ----------
ifeq ($(PLATFORM),macos)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
	@if [ -f "$(ICON_DIR)/hako.icns" ] && command -v Rez >/dev/null 2>&1 && command -v SetFile >/dev/null 2>&1; then \
		printf 'read %c%s%c (-16455) "%s/hako.icns";\n' "'" "icns" "'" "$(ICON_DIR)" > .hako.r; \
		Rez -append .hako.r -o $(BIN) && SetFile -a C $(BIN) && \
		echo "icon attached to $(BIN)" || echo "icon attach failed (non-fatal)"; \
		rm -f .hako.r; \
	else \
		echo "icon skip (no .icns or Rez/SetFile not found)"; \
	fi

endif

# ---------- Linux: plain build, icon shipped alongside ----------
ifeq ($(PLATFORM),linux)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
	@if [ -f "$(ICON_DIR)/hako.png" ]; then \
		echo "built $(BIN). copy $(ICON_DIR)/hako.png to ~/.local/share/icons/ for desktop entry."; \
	else \
		echo "built $(BIN)."; \
	fi

endif

# ---------- FreeBSD: plain build ----------
ifeq ($(PLATFORM),freebsd)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

endif

asan: $(SRC)
	$(CC) -fsanitize=address,undefined -g -O1 -Wall $< -o hako_asan $(LDLIBS)

clean:
	rm -f hako hako.exe hako_asan hako.rc hako.res .hako.r
	rm -rf hako_asan.dSYM

# ---------- install / uninstall ----------
# Auto-pick PREFIX: $(PREFIX) override → /usr/local if writable → ~/.local.
# Strips macOS quarantine xattr post-install.
# Drops a Linux .desktop entry + PNG icon when applicable (ICONS=0 to skip).

PREFIX ?=
ICONS  ?= 1

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
