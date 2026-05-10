# hakoCLAW — standalone AI agent CLI, lifted from hako.c.
# Same constraints as hako: gcc, libc, pthread, curl on PATH. No build deps beyond those.
#
# Windows: icon embedded into .exe via windres (real OS icon).
# macOS:   icon attached via Rez/SetFile if Xcode CLT installed (best-effort).
# Linux:   ELF can't embed icons; icon/hakoCLAW.png shipped alongside for .desktop.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall
LDLIBS  ?= -lpthread

ICON_DIR = icon
SRC      = hakoCLAW.c
BIN      = hakoc

ifeq ($(OS),Windows_NT)
    PLATFORM = windows
    BIN := hakoc.exe
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

.PHONY: all clean asan

all: $(BIN)

# ---------- Windows: embed icon via resource (optional — skip if .ico missing) ----------
ifeq ($(PLATFORM),windows)

HAS_ICO := $(wildcard $(ICON_DIR)/hakoCLAW.ico)

ifeq ($(HAS_ICO),)
# No icon — plain build.
$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)
else
# Embed icon via windres.
hakoCLAW.rc:
	@printf 'IDI_ICON1 ICON "$(ICON_DIR)/hakoCLAW.ico"\n' > $@

hakoCLAW.res: hakoCLAW.rc $(ICON_DIR)/hakoCLAW.ico
	windres $< -O coff -o $@

$(BIN): $(SRC) hakoCLAW.res
	$(CC) $(CFLAGS) $(SRC) hakoCLAW.res -o $@ $(LDLIBS)
endif

endif

# ---------- macOS: build, then attach icon if tools exist ----------
ifeq ($(PLATFORM),macos)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
	@if [ -f "$(ICON_DIR)/hakoCLAW.icns" ] && command -v Rez >/dev/null 2>&1 && command -v SetFile >/dev/null 2>&1; then \
		printf 'read %c%s%c (-16455) "%s/hakoCLAW.icns";\n' "'" "icns" "'" "$(ICON_DIR)" > .hakoclaw.r; \
		Rez -append .hakoclaw.r -o $(BIN) && SetFile -a C $(BIN) && \
		echo "icon attached to $(BIN)" || echo "icon attach failed (non-fatal)"; \
		rm -f .hakoclaw.r; \
	else \
		echo "icon skip (no .icns or Rez/SetFile not found)"; \
	fi

endif

# ---------- Linux: plain build, icon shipped alongside ----------
ifeq ($(PLATFORM),linux)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
	@if [ -f "$(ICON_DIR)/hakoCLAW.png" ]; then \
		echo "built $(BIN). copy $(ICON_DIR)/hakoCLAW.png to ~/.local/share/icons/ for desktop entry."; \
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
	$(CC) -fsanitize=address,undefined -g -O1 -Wall $< -o hakoc_asan $(LDLIBS)

clean:
	rm -f hakoc hakoc.exe hakoc_asan hakoCLAW.rc hakoCLAW.res .hakoclaw.r
	rm -rf hakoc_asan.dSYM
