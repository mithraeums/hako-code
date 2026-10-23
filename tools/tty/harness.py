#!/usr/bin/env python3
"""pty harness for the terminal UI. Runs `hako` in a pseudo-terminal against
tools/tty/fakehakm.py, feeds a `pyte` screen, and asserts the things a human
keeps catching by eye:
  - nothing streamed ever lands on the input row
  - zero absolute cursor moves, scroll regions, DSR (the in-flow rule)
  - the bar is up in every idle frame while the model streams
  - the ledger shows the CALL line, survives resume, and the model gets the
    <observation> back after a resume
  - :theme / ctrl+o redraw in place: one copy of the chat, no clear
ORDER MATTERS: sections 4 and 5 inspect the session log and the project state
left behind by the section before them, and `start()` wipes the throwaway HOME.
So ANY section that calls start() must run BEFORE 4, or be appended at the very
end with 3c/3d/3i. Getting this wrong has produced three false "regressions".

Needs: python3 + `pip install pyte`. Usage: HAKO_BIN=./hako python3 tools/tty/harness.py
Everything runs under a throwaway HOME in $TMPDIR; your real ~/.hako is untouched."""
import os, pty, sys, time, select, struct, fcntl, termios, signal, re, json, shutil, tempfile
try:
    import pyte
except ImportError:
    sys.exit("pip install pyte")

HERE = os.path.dirname(os.path.abspath(__file__))
HAKO = os.environ.get("HAKO_BIN") or os.path.join(HERE, "..", "..", "hako")
WORK = tempfile.mkdtemp(prefix="hako-tty-")
HOME, PROJ, FAKE_LOG = (os.path.join(WORK, d) for d in ("home", "proj", "fake.log"))
COLS, ROWS = int(os.environ.get("COLS", 100)), int(os.environ.get("ROWS", 40))
ok = True
def check(cond, what, dump=None):
    global ok
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond and dump is not None:
        for i, l in enumerate(dump.screen.display):
            if l.strip(): print(f"       {i+1:3} {l.rstrip()[:100]}")
    ok = ok and bool(cond)

def fresh(files=(), auto=True):
    shutil.rmtree(HOME, ignore_errors=True); shutil.rmtree(PROJ, ignore_errors=True)
    os.makedirs(os.path.join(HOME, ".hako", "models", "hako-sho")); os.makedirs(PROJ)
    open(os.path.join(HOME, ".hako", "models", "hako-sho", "hako-sho.mlf2"), "w").write("fake")
    open(os.path.join(HOME, ".hakorc"), "w").write(
        "ai_provider=mithraeum\nai_model=hako-sho\ntheme=mithraeum\nshow_splash=0\n"
        + ("ai_auto_approve=1\n" if auto else "ai_auto_approve=0\n"))
    open(os.path.join(PROJ, "alpha.txt"), "w").write("a\n")
    for f in files: open(os.path.join(PROJ, f), "w").write("x\n")
    if os.path.exists(FAKE_LOG): os.remove(FAKE_LOG)

class Run:
    def __init__(self, engine="fakehakm.py"):
        # HistoryScreen, not Screen: a plain Screen DISCARDS rows that scroll off,
        # which is exactly where a duplicated chat hides. Three 'fixes' shipped
        # green against a Screen while the user was looking at the scrollback.
        self.screen = pyte.HistoryScreen(COLS, ROWS, history=4000)
        self.stream = pyte.ByteStream(self.screen)
        self.raw = b""; self.snaps = []
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(PROJ)
            env = dict(os.environ, HOME=HOME, HAKO_HAKM=os.path.join(HERE, engine), FAKE_LOG=FAKE_LOG,
                       TERM="xterm-256color", COLORTERM="truecolor", HAKO_TRUECOLOR="1")
            os.execve(HAKO, [HAKO], env)
        self.pid, self.fd = pid, fd
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    def pump(self, secs, idle_cb=None, idle=0.02):
        end = time.time() + secs; last = time.time(); busy = False
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.01)
            if r:
                try: data = os.read(self.fd, 65536)
                except OSError: return False
                if not data: return False
                self.raw += data; self.stream.feed(data); self.snaps.append(list(self.screen.display))
                last = time.time(); busy = True
            elif busy and idle_cb and time.time() - last >= idle:
                idle_cb(list(self.screen.display)); busy = False
        return True
    def wait_for(self, text, secs=20):
        end = time.time() + secs
        while time.time() < end:
            self.pump(0.2)
            if any(text in l for l in self.screen.display): return True
        raise SystemExit(f"timeout waiting for {text!r}\n" + "\n".join(self.screen.display))
    def send(self, s): os.write(self.fd, s.encode())
    def resize(self, cols, rows=ROWS):
        """Change the window: the pty (which raises SIGWINCH in hako) and the
        emulator. pyte does NOT reflow — a real terminal re-wraps its rows — so
        this exercises the erase, not the reflow arithmetic."""
        self.screen.resize(rows, cols)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    def lines(self): return [l.rstrip() for l in self.screen.display if l.strip()]
    def all_lines(self):
        """Everything the terminal holds: scrollback first, then the screen.
        What the user sees when they scroll up."""
        out = []
        for row in self.screen.history.top:
            out.append("".join(row[i].data for i in sorted(row)).rstrip())
        return [l for l in out if l.strip()] + self.lines()
    def field_rows(self, snap=None): return [l for l in (snap or self.screen.display) if l.startswith("> ")]
    def close(self):
        try: self.send(":q\r"); self.pump(1.0); os.kill(self.pid, signal.SIGTERM)
        except Exception: pass

def start(files=(), engine="fakehakm.py", auto=True):
    fresh(files, auto)
    r = Run(engine); r.wait_for("Trust this directory", 10); r.send("y\r"); r.wait_for("▶▶", 10)
    return r

def settle(r, secs=4.0):
    """Wait until the turn has really finished. Sending the next prompt too
    early gets it QUEUED, which replays the text and looks exactly like a
    redraw duplicating the chat — a trap this harness fell into twice."""
    import time as _t
    end = _t.time() + secs
    while _t.time() < end:
        r.pump(0.15)
        if not any("queued" in l for l in r.lines()): return

def inflow(r, label):
    bad = [(i, l.rstrip()) for i, s in enumerate(r.snaps) for l in r.field_rows(s) if "◆" in l or "* `" in l]
    absolute = len(re.findall(rb"\x1b\[\d+;\d+H", r.raw)) + r.raw.count(b"\x1b[H")
    region = len(re.findall(rb"\x1b\[\d*;?\d*r", r.raw)); dsr = r.raw.count(b"\x1b[6n")
    check(not bad, f"{label}: nothing streamed on the input row ({len(bad)} frames)")
    check(absolute == 0 and region == 0 and dsr == 0, f"{label}: abs={absolute} region={region} dsr={dsr}")

# ---- 1. prose stream, bar present throughout, type-ahead survives
r = start()
idle = []
r.send("hello\r"); time.sleep(0.3); r.send("abc")
r.pump(8, idle_cb=idle.append)
r.wait_for("What next?", 30); r.pump(0.5)
inflow(r, "prose")
mid = [s for s in idle if any("hello" in l for l in s)]
check(mid and all(any(l.startswith("> ") for l in s) for s in mid), f"bar up in every idle frame mid-stream ({len(mid)} frames)")
check(any("· alpha.txt" in l for l in r.lines()), "bullets rendered")
check(any("abc" in l for l in r.field_rows()), "type-ahead restored in the field")
# ---- 2. tool turn: call line, result, no leak
r.send("\x15TOOL please\r"); r.wait_for("Done. The directory", 30); r.pump(0.5)
inflow(r, "tool turn")
check(any("list_dir(.)" in l for l in r.lines()), "ledger shows the call line")
# ---- 3. ctrl+o opens the TRANSCRIPT SCREEN; the chat itself never reflows.
# Expanding in place could not be made correct: a layout change taller than
# the terminal scrolls it, and every scrolled row is a second copy of the
# conversation in the scrollback. The viewer owns the alternate buffer, so the
# chat below is untouched and nothing is added to the history.
r = start(files=[f"f{i:03d}.txt" for i in range(60)])
r.send("TOOL please\r"); r.wait_for("Done. The directory", 30); settle(r); r.pump(0.5)
before_screen = [l.rstrip() for l in r.screen.display]
before_buffer = len(r.all_lines())
n = len(r.raw); r.send("\x0f"); r.pump(2.0)
seg = r.raw[n:]
check(b"\x1b[?1049h" in seg, "ctrl+o switches to the alternate screen")
check(any("transcript" in l for l in r.lines()), "the viewer draws its footer")
check(sum(1 for l in r.lines() if ".txt" in l) > 8, "the viewer shows the result in full")
n = len(r.raw); r.send("G"); r.pump(1.0); r.send("g"); r.pump(1.0)
check(any("transcript" in l for l in r.lines()), "g / G scroll inside the viewer")
n = len(r.raw); r.send("q"); r.pump(1.5)
check(b"\x1b[?1049l" in r.raw[n:], "q leaves the alternate screen")
# pyte does not emulate the alternate buffer, so it cannot show the restore a
# real terminal performs; what it CAN prove is that hako never reflowed the
# chat itself, which is the property that used to break.
check(b"\x1b[2J" not in seg.replace(b"\x1b[H\x1b[2J", b""), "no clear of the main screen")
r.close()


# ---- 4. session log + resume
slug = os.listdir(os.path.join(HOME, ".hako", "projects"))[0]
sess = os.path.join(HOME, ".hako", "projects", slug, "sessions")
roles = [json.loads(l)["role"] for f in os.listdir(sess) for l in open(os.path.join(sess, f)) if l.strip()]
check("tool" in roles and "result" in roles, f"log has tool+result rows: {roles}")
open(FAKE_LOG, "w").close()
r = Run(); r.wait_for("continue (resumed)", 15); r.send("\r"); r.wait_for("▶▶", 15); r.pump(1.0)
check(any("list_dir(.)" in l for l in r.lines()), "resume replays the ledger")
r.send("hello\r"); r.wait_for("What next?", 30)
check('<observation tool="list_dir">' in open(FAKE_LOG).read(), "resume hands the model the observation")
r.close()
# ---- 4b. name + color belong to the chat, not the project; esc at startup quits
r = start(); r.send(":name my chat\r"); r.pump(0.5); r.send(":color purple\r"); r.pump(0.5)
r.send("hello\r"); r.wait_for("What next?", 30); r.pump(0.3)
r.send(":theme\r"); r.pump(0.8)
check(any("[ my chat ]" in l for l in r.lines()) and any("│ Theme" in l for l in r.lines()), "popup sits above the bar (bar still visible)")
r.send("\x1b"); r.pump(0.6)
check(not any("│ Theme" in l or "esc cancel" in l for l in r.lines()), "popup fully erased on esc")
r.close()
r = Run(); r.wait_for("continue (resumed)", 10); r.send("\x1b[A\x1b[A\r"); r.wait_for("▶▶", 10); r.pump(0.5)
check(not any("my chat" in l for l in r.lines()), "a NEW session has no name")
check("38;2;150;110;190" not in r.raw[-800:].decode("utf-8", "replace"), "a NEW session has no color")
r.close()
r = Run(); r.wait_for("continue (resumed)", 10); r.send("\x1b[A\r"); r.pump(0.8)
check(any("my chat" in l for l in r.lines()), "resume list shows the chat name")
r.send("\r"); r.wait_for("▶▶", 10); r.pump(0.5)
check(any("[ my chat ]" in l for l in r.lines()), "resumed chat has its name back")
r.close()
r = Run(); r.wait_for("continue (resumed)", 10); r.send("\x1b"); alive = True
for _ in range(30):
    if not r.pump(0.1): alive = False; break
check(not alive, "esc at the startup menu quits")
# ---- 5. :q while the engine hangs
open(os.path.join(HERE, "_hang.py"), "w").write("#!/usr/bin/env python3\nimport sys,time\ni=sys.stdin.buffer\nh=i.readline()\n"
    "for _ in range(int(h.split()[0])):\n i.readline(); n=int(i.readline()); i.read(n)\ntime.sleep(600)\n")
os.chmod(os.path.join(HERE, "_hang.py"), 0o755)
r = start(engine="_hang.py"); r.send("hello\r"); time.sleep(1.5); t0 = time.time(); r.send(":q\r")
alive = True
for _ in range(50):
    if not r.pump(0.1): alive = False; break
check(not alive, f":q during a stuck turn exits ({time.time()-t0:.1f}s)")
os.remove(os.path.join(HERE, "_hang.py"))
# ---- 3c. keys during a tool turn must reach hako, not the terminal.
# Approval used to seize stdin on EVERY tool call, even in auto mode, dropping
# the reader to cooked mode while it waited for the handshake — keystrokes were
# echoed by the terminal ("^O" on screen) instead of read.
os.environ["HAKO_FAKE_DELAY"] = "3"
r = start(files=["a.txt"], engine="fakehakm.py")
r.send("TOOL please\r")
for _ in range(10):
    r.pump(0.25); r.send("\x0f")      # inside the 3s think, while the tool gate runs
r.wait_for("Done. The directory", 40); r.pump(1.0)
check(not any("^O" in l for l in r.lines()), "ctrl+o during a tool turn is not echoed by the terminal")
r.close()

# ---- 3d. Tab only completes a real word
r = start(files=["test.sh"])
def tab_rows(keys):
    r.send(keys); r.pump(0.8)
    rows = [l for l in r.lines() if "test.sh" in l and not l.startswith("> ")]
    r.send("\x15"); r.pump(0.2)
    return rows
check(not tab_rows("\t"), "Tab on an empty prompt lists nothing")
check(not tab_rows("hello \t"), "Tab after a trailing space lists nothing")
r.send("te\t"); r.pump(0.8)
check(any(l.startswith("> ") and "test.sh" in l for l in r.lines()), "Tab still completes a real word")
r.close()

# ---- 3j. the thinking mark, the tab title, and the "worked for" line
import re as _re
os.environ["HAKO_FAKE_DELAY"] = "5"
r = start()
r.send("think hard\r")
chat, titles = set(), set()
for _ in range(30):
    r.pump(0.2)
    for l in r.screen.display:
        for g in ("\u25ab", "\u25a3", "\u25aa"):
            if g in l and ("thinking" in l or "\u2026" in l): chat.add(g)
    for t in _re.findall(r"\x1b\]0;(.*?)\x07", r.raw.decode("utf-8", "replace")): titles.add(t)
r.wait_for("Okay, here is", 40); settle(r); r.pump(0.6)
check(chat == {"\u25ab", "\u25a3", "\u25aa"}, f"the mark fills and empties: {''.join(sorted(chat))}")
check(any("\u25e7" in t for t in titles) and any("\u25e8" in t for t in titles),
      "the tab title alternates between the half-filled squares")
check(any("worked for" in l for l in r.lines()), "a turn over 2s leaves a 'worked for' line")
r.close()
os.environ["HAKO_FAKE_DELAY"] = "0"

# ---- 3i. a plan renders as a checklist that ticks off, not as a count
r = start()
r.send("PLAN it out\r"); r.wait_for("task list", 30); settle(r); r.pump(0.5)
d = r.lines()
check(any("task list" in l and "1 of 3" in l for l in d), "the todo tool reports its progress")
check(any("\u2713 read the file" in l for l in d), "a done item shows a tick")
check(sum(1 for l in d if "\u25cb" in l) == 2, "pending items show an open circle")
r.close()

# ---- 3j. bracketed paste: the markers are consumed, CRLF is ONE break, a tab
# inside the paste is data. A terminal sends \r\n per line, and inserting a
# space per byte put two spaces in for every pasted line; a pasted tab reached
# the completion key and vanished.
r = start()
r.send("\x1b[200~one\r\ntwo\r\nthree\x1b[201~")
r.pump(0.6)
d = " ".join(r.lines())
check("one two three" in d, "a pasted CRLF is one space, not two")
check("one  two" not in d, "no doubled space from CRLF")
check("200~" not in d and "201~" not in d, "paste markers never reach the field")
r.send("\x01\x0b")                      # home, kill to end: clear the field
r.send("\x1b[200~tab\there\x1b[201~")
r.pump(0.6)
check("tab here" in " ".join(r.lines()), "a tab inside a paste stays in the text")
r.close()

# ---- 3k. TWO gated approvals in one turn. hkClaimStdin used to skip the claim
# whenever cl_composing read false — which it does in the gap between the reader
# being released and it marking itself composing again. The second tool call
# landed in that gap, worker and reader both read the tty, and the turn hung
# waiting for a "y" the other reader had eaten.
r = start(auto=False)
r.send("TWOTOOLS please\r")
r.wait_for("Allow?", 20); r.pump(0.4)
r.send("\r")                     # the gate is a picker: enter = allow once
r.wait_for("read_file", 20)
r.wait_for("Allow?", 20); r.pump(0.4)
# The approval header and diff were printed bare: with the bar up (the second
# approval of a turn) they landed on its rows, the bar's erase took the header,
# and the bar's top rule was left stranded above the popup.
_bar = [l for l in r.lines() if l.count("\u2500") >= 20 and not any(c in l for c in "\u256d\u256e\u2570\u256f\u251c\u2524")]
check(len(_bar) == 2, "the second approval leaves exactly the bar's two rules (%d)" % len(_bar))
check(any("\u25cf read_file" in l for l in r.lines()), "the second approval keeps its \u25cf read_file header")
r.send("\r")                     # approve the second — this is the one that hung
done = r.wait_for("alpha.txt", 20)
check(done, "a second gated tool call in the same turn still runs (no freeze)")
d = " ".join(r.lines())
check("denied" not in d, "no approval reports a denial the user never made")
r.close()

# ---- 3k2. the bar comes BACK after an approval. The claim wakes the reader with
# SIGUSR1, which also means "turn over"; the flag stayed set, so the reader quit
# the turn on its next read: no bar and no type-ahead for the rest of every turn
# that asked permission. The fake engine never paused, so nothing saw it.
os.environ["HAKO_FAKE_DELAY"] = "3"
r = start(auto=False)
r.send("TOOL please\r")
r.wait_for("Allow?", 20); r.pump(0.4); r.send("\r")
r.wait_for("alpha.txt", 20); r.pump(1.2)          # the follow-up call is still waiting
_bar = [l for l in r.lines() if l.count("\u2500") >= 20 and not any(c in l for c in "\u256d\u256e\u2570\u256f")]
check(len(_bar) == 2, "the bar is back while the model works after an approval (%d rules)" % len(_bar))
r.send("abc"); r.pump(0.6)
check(any(l.startswith(">") and "abc" in l for l in r.lines()), "and typing after an approval lands in the field")
r.close()
os.environ["HAKO_FAKE_DELAY"] = "0"

# ---- 3k3. a model that says nothing after its tool ran has finished, not failed.
os.environ["FAKE_QUIET_AFTER_TOOL"] = "1"
r = start()
r.send("TOOL please\r"); r.wait_for("alpha.txt", 20); settle(r); r.pump(0.8)
check("empty response" not in " ".join(r.all_lines()), "silence after a tool ran is not reported as an engine error")
r.close()
del os.environ["FAKE_QUIET_AFTER_TOOL"]

# ---- 3l. resize leaves ONE bar. The SIGWINCH path erased from the cursor row
# down, and the cursor sits on the field — so the top rule (and the blank row
# above it) survived every single resize step. Dragging a window wider left a
# staircase of rules up the screen, one per step. Nothing in this file tested a
# resize at all, which is how it shipped.
r = start()
r.send("hello there\r"); r.wait_for("Okay, here is", 30); settle(r); r.pump(0.5)
def rule_rows(lines):
    # a bar rule is a row of box-drawing horizontals with no corner glyphs
    # (the banner's box has corners; the bar's rules do not)
    return [l for l in lines if l.count("\u2500") >= 20 and not any(c in l for c in "\u256d\u256e\u2570\u256f")]
before_chat = [l for l in r.lines() if "alpha.txt" in l]
check(len(before_chat) >= 1, "a reply is on screen above the bar before resizing")
for w in (90, 100, 110, 120, 96):
    r.resize(w); r.pump(0.6)
allb = r.all_lines()
if len(rule_rows(allb)) != 2:
    for i, l in enumerate(allb):
        if l.count("\u2500") >= 20: print(f"       rule@{i:3d} width={len(l):3d} {l[:40]!r}")
check(len(rule_rows(allb)) == 2,
      f"after five resizes the buffer holds exactly one bar (rules seen: {len(rule_rows(allb))})")
check(any("alpha.txt" in l for l in r.lines()),
      "the chat above the bar survived the resize erase (no over-climb)")
r.send("still here"); r.pump(0.4)
check(any("still here" in l for l in r.lines()), "typing after a resize lands in the field")
r.close()

# ---- 3l2. a DRAG is a burst of resizes. Each one redrew the bar, and on a real
# terminal a bar drawn at one width lands after the window has moved on to the
# next — the following erase climbs short and leaves that bar's top rule
# behind (Terminal.app: one stale rule per few drag steps). pyte cannot show
# the race, so pin the cure: a burst draws the bar ONCE, after it settles.
r = start()
r.send("hello there\r"); r.wait_for("Okay, here is", 30); settle(r); r.pump(0.5)
mark = len(r.raw)
for w in range(120, 84, -3):
    r.resize(w); r.pump(0.01)
r.pump(0.8)
draws = len(re.findall(rb"\x1b\[2K  \x1b\[[0-9;]*m\xe2\x94\x80", r.raw[mark:])) // 2   # two rules per bar
check(draws == 1, f"a burst of 12 resizes redraws the bar once, after it settles (drew {draws})")
check(len(rule_rows(r.all_lines())) == 2, "and the buffer holds exactly one bar")
r.send("still here"); r.pump(0.4)
check(any("still here" in l for l in r.lines()), "typing after a drag lands in the field")
r.close()

# ---- 3m. the field counts CELLS, not bytes. The cursor column was
# (prompt + byte offset), so after é (2 bytes, 1 cell) and an emoji (4 bytes,
# 2 cells) it sat three cells past the text — which reads as stray spaces.
r = start()
r.send("caf\u00e9 \U0001F3AE x"); r.pump(0.8)
row = next((i for i, l in enumerate(r.screen.display) if "caf" in l and "x" in l), None)
if row is not None:
    # measure on the cells: display strings drop a wide glyph's second cell
    cells = r.screen.buffer[row]
    xcol = max(c for c in range(r.screen.columns) if cells[c].data == "x")
    check(r.screen.cursor.y == row and r.screen.cursor.x == xcol + 1,
          f"the cursor sits right after the typed text with é and an emoji (cursor {r.screen.cursor.x}, text ends {xcol + 1})")
else:
    check(False, "typed é/emoji text is in the field", r)
r.send("\x7f\x7f\x7f"); r.pump(0.5)             # backspace over x, space and the emoji
line = r.screen.display[r.screen.cursor.y]
check(line.rstrip().endswith("caf\u00e9") or "caf\u00e9 " in line, "backspace over an emoji leaves no residue", r)
r.close()

# ---- 3m2. arrows and delete step over whole characters. The cursor moved one
# BYTE at a time: two presses to get past é, four past an emoji, and a key typed
# in between landed inside the character and broke it.
r = start()
r.send("a\U0001F3AEb\u00e9"); r.pump(0.5)
r.send("\x1b[D\x1b[D\x1b[D"); r.pump(0.3)        # back over é, b and the emoji
r.send("X"); r.pump(0.4)
check(any("aX\U0001F3AEb\u00e9" in l for l in r.lines()), "left arrow moves one character, not one byte (a🎮bé → aX🎮bé)", r)
r.send("\x1b[3~"); r.pump(0.4)                   # delete the emoji under the cursor
check(any("aXb\u00e9" in l for l in r.lines()), "delete removes the whole emoji", r)
r.send("\x1b[F\x7f"); r.pump(0.4)                # end, backspace over é
check(any(l.endswith("aXb") for l in r.lines()), "backspace removes the whole é", r)
r.close()

# ---- 3m3. a message typed during a turn waits in the BAR. It was a chat line,
# and the streaming answer climbing back to its own partial line wrote over it,
# so "1 queued" flashed for a second and was gone.
os.environ["HAKO_FAKE_DELAY"] = "2"
r = start(engine="slowhakm.py")
r.send("first one\r"); r.pump(0.6)
r.send("second one\r"); r.pump(0.6)
check(any("1 queued" in l for l in r.lines()), "a queued message shows in the bar", r)
r.wait_for("Here it is", 10); r.pump(0.3)
saw = any(any("1 queued" in l for l in s) for s in r.snaps[-20:])
check(saw, "and is still there while the first answer streams")
r.pump(5)
check(not any("queued" in l for l in r.lines()), "and goes once it has run", r)
r.close()
del os.environ["HAKO_FAKE_DELAY"]

# ---- 3n. ctrl+o while typing: the pager used to throw the half-typed line away.
r = start()
r.send("hello there\r"); r.wait_for("Okay, here is", 30); settle(r)
r.send("keep me please"); r.pump(0.5)
r.send("\x0f"); r.pump(0.8)
r.send("q"); r.pump(0.8)
check(any("keep me please" in l for l in r.lines()), "the typed line survives opening and closing the transcript", r)
r.send(" ok"); r.pump(0.4)
check(any("keep me please ok" in l for l in r.lines()), "and typing continues at its end", r)
r.close()

# ---- 3o. "always" is silent from then on — say so once, or ask mode looks broken.
r = start(auto=False)
r.send("TWOTOOLS\r")
r.wait_for("Allow?", 20); r.pump(0.4)
r.send("\x1b[B"); r.pump(0.2); r.send("\x1b[B"); r.pump(0.2); r.send("\r")   # third choice: always read
r.wait_for("Done. The directory", 20); settle(r)
d = " ".join(r.all_lines())
check("allowed without asking" in d, "a tool let through by an always rule says so", r)
check(d.count("Allow?") <= 1, "and the second read did not prompt again")
check(any("ask mode" in l and "always read" in l for l in r.lines()), "the bar shows the always rule next to the mode", r)
r.close()
logs = []
for root, _, files in os.walk(os.path.join(HOME, ".hako", "projects")):
    logs += [os.path.join(root, f) for f in files if f.endswith(".jsonl")]
check(any("approval: list_dir" in open(f).read() for f in logs), "the approval decision is in the session log")

# ---- 3p. "worked for" is ONE line, after the turn — it used to print at every
# spinner stop, so a tool turn showed it above the tool calls and the approval.
os.environ["HAKO_FAKE_DELAY"] = "3"
r = start()
r.send("TWOTOOLS\r"); r.wait_for("Done. The directory", 40); settle(r); r.pump(0.8)
L = r.all_lines()
wf = [i for i, l in enumerate(L) if "worked for" in l]
last_tool = max([i for i, l in enumerate(L) if "read_file" in l or "Done. The directory" in l] or [-1])
check(len(wf) == 1, f"one 'worked for' line per turn (saw {len(wf)})", r)
check(bool(wf) and wf[-1] > last_tool, "and it comes after the turn's tools and answer", r)
r.close()
os.environ["HAKO_FAKE_DELAY"] = "0"
r = start()
r.send("hello there\r"); r.wait_for("Okay, here is", 30); settle(r); r.pump(0.6)
check(any("worked for" in l for l in r.lines()), "a fast turn says how long it took too (it only did over 2 s)", r)
r.close()

# ---- 3q. the CLOUD path (Claude over OAuth), against a local fake of the
# Anthropic SSE API. It used to hold the whole answer back, print it off the
# rail with no wrapping or markdown, and drop any prose before a tool call.
import subprocess, atexit
def anth_home(port):
    fresh()
    open(os.path.join(HOME, ".hakorc"), "w").write(
        "ai_provider=anthropic\nai_model=claude-haiku-4-5\nai_endpoint=http://127.0.0.1:%d\n"
        "theme=mithraeum\nshow_splash=0\nai_auto_approve=1\n" % port)
    open(os.path.join(HOME, ".hako", "state"), "w").write(
        "ai_provider=anthropic\nai_oauth_provider=anthropic\nai_api_key=fake-token\n"
        "ai_oauth_refresh=r\nai_oauth_expires_at=4102444800\n")
APORT = 18911
srv = subprocess.Popen([sys.executable, os.path.join(HERE, "fakeanthropic.py"), str(APORT),
                        os.path.join(WORK, "anth.log")])
atexit.register(srv.kill)
time.sleep(0.5)
anth_home(APORT)
r = Run(); r.wait_for("Trust this directory", 10); r.send("y\r"); r.wait_for("\u25b6\u25b6", 10)
live = []
r.send("hello there\r")
end = time.time() + 20
while time.time() < end and not any("Done." in l for l in r.screen.display):
    r.pump(0.05)
    if any("Here is a longer" in l for l in r.screen.display) and not any("Done." in l for l in r.screen.display):
        live.append(1)
settle(r)
L = r.all_lines()
check(bool(live), "a Claude answer streams in live, not all at once at the end")
i = next((k for k, l in enumerate(L) if "Here is a longer" in l), None)
check(i is not None and L[i + 1].startswith("\u2502"), "its wrapped continuation stays on the rail", r)
check(any("\u00b7 one" in l for l in L), "markdown is rendered (list bullets)", r)
r.send("CLAUDETOOL\r"); r.wait_for("everything here", 20); settle(r)
L = r.all_lines()
check(any("Let me check what is in the project" in l for l in L), "prose before a text-dialect tool call is shown", r)
check(not any("<tool" in l for l in L), "and the tool markup itself is not", r)
r.send("CLAUDEGOESON\r"); r.wait_for("everything here", 20); settle(r)
L = r.all_lines()
sent = open(os.path.join(WORK, "anth.log"), errors="replace").read()
check("stop_sequences" in sent, "prose tool calls ask Claude to stop at </tool>")
check("no project directory" not in sent and not any("no project directory" in l for l in L),
      "so an invented result never reaches the screen or the next request", r)
check(any("list_dir" in l for l in L), "and the call still runs", r)
r.close()
anth_home(APORT)
os.makedirs(PROJ, exist_ok=True)
pp = subprocess.run([HAKO, "--pipe"], input=b'{"type":"prompt","text":"CLAUDETOOL"}\n', capture_output=True,
                    cwd=PROJ, env=dict(os.environ, HOME=HOME), timeout=30)
check(b"Let me check what is in the project" in pp.stdout, "--pipe (the Rei pane) gets that prose too")
srv.kill()

# ---- 3q2. ChatGPT sign-in: the Responses stream rides the same prose path as
# Claude's sign-in. That API has no stop sequences, so hako hangs up at </tool>.
import base64
def b64u(o):
    return base64.urlsafe_b64encode(json.dumps(o).encode()).decode().rstrip("=")
GPT_JWT = "%s.%s.sig" % (b64u({"alg": "none"}), b64u({"exp": 4102444800,
          "https://api.openai.com/auth": {"chatgpt_account_id": "acct-123'; rm -rf x"}}))
def gpt_home(port):
    fresh()
    open(os.path.join(HOME, ".hakorc"), "w").write(
        "ai_provider=openai\nai_model=gpt-5.5\nai_endpoint=http://127.0.0.1:%d/backend-api/codex\n"
        "theme=mithraeum\nshow_splash=0\nai_auto_approve=1\n" % port)
    open(os.path.join(HOME, ".hako", "state"), "w").write(
        "ai_provider=openai\nai_oauth_provider=chatgpt\nai_api_key=%s\n"
        "ai_oauth_refresh=r\nai_oauth_expires_at=4102444800\n" % GPT_JWT)
GPORT = 18912
GLOG = os.path.join(WORK, "gpt.log")
srv = subprocess.Popen([sys.executable, os.path.join(HERE, "fakechatgpt.py"), str(GPORT), GLOG])
atexit.register(srv.kill)   # a failed check must not leave it holding the port
time.sleep(0.5)
gpt_home(GPORT)
r = Run(); r.wait_for("Trust this directory", 10); r.send("y\r"); r.wait_for("▶▶", 10)
r.send("hello there\r"); r.wait_for("Done.", 20); settle(r)
L = r.all_lines()
check(any("Hello from the fake ChatGPT" in l for l in L), "a ChatGPT answer streams in", r)
reqs = [json.loads(l) for l in open(GLOG)]
post = [q for q in reqs if q["body"]][-1]
body, hdr = post["body"], {k.lower(): v for k, v in post["headers"].items()}
check(post["path"].endswith("/backend-api/codex/responses"), "it posts to the Responses endpoint")
check(body.get("store") is False and body.get("instructions"), "store is false and the system prompt is instructions")
check(not any(i.get("role") == "system" for i in body.get("input", [])), "and not a system message in input")
check(hdr.get("originator") == "hako", "hako names itself as the originator")
check(hdr.get("chatgpt-account-id") == "acct-123rm-rfx", "the account id comes from the token, shell-safe")
r.send("GPTTOOL\r"); r.wait_for("everything here", 20); settle(r)
L = r.all_lines()
check(any("Let me check what is in the project" in l for l in L), "prose before a ChatGPT tool call is shown", r)
check(not any("<tool" in l for l in L), "and the tool markup itself is not", r)
r.send("GPTGOESON\r"); r.wait_for("everything here", 20); settle(r)
L = r.all_lines()
sent = open(GLOG, errors="replace").read()
check("no project directory" not in sent and not any("no project directory" in l for l in L),
      "hako hangs up at </tool>, so an invented result goes nowhere", r)
r.send(":models refresh\r"); r.wait_for("gpt-5.4-mini", 10); settle(r)
check(any("gpt-5.4-mini" in l for l in r.all_lines()), "the model list reads the codex catalog's slugs", r)
r.close()
srv.kill()

# ---- 3r. -p --output-format json names an engine failure. It reported a bare
# "empty response" and threw away the engine's own words and its exit status.
fresh()
dead = os.path.join(WORK, "deadhakm.sh")
open(dead, "w").write("#!/bin/sh\necho 'hakm: model file is not MLF2 (bad magic)' >&2\nexit 3\n")
os.chmod(dead, 0o755)
pp = subprocess.run([HAKO, "-p", "hi", "--output-format", "json"], capture_output=True, cwd=PROJ,
                    env=dict(os.environ, HOME=HOME, HAKO_HAKM=dead), timeout=30)
out = pp.stdout.decode("utf-8", "replace")
check("not MLF2" in out and "status 3" in out, "a dead engine's message and exit status reach -p json")
# ...and --pipe (Rei, the studio) still gets "done" after it: the error path
# returned before the turn's ending, so the pane sat at "Listening..." forever.
pp = subprocess.run([HAKO, "--pipe"], input=b'{"type":"prompt","text":"hi"}\n', capture_output=True, cwd=PROJ,
                    env=dict(os.environ, HOME=HOME, HAKO_HAKM=dead), timeout=30)
out = pp.stdout.decode("utf-8", "replace")
check("not MLF2" in out and '"type":"done"' in out, "--pipe ends a turn with done even when the engine died")

# ---- 3s. A menu comes and goes without a trace: no echoed command, no
# "theme: unchanged", nothing in scrollback. :login opens the sign-ins; API keys
# sit one level down, named for who bills you.
r = start()
def count(r, words): return sum(l.count(w) for l in r.all_lines() for w in words)
WORDS = (":theme", ":model", ":provider", ":login", "theme:", "model:", "provider:")
before = count(r, WORDS)
for c in (":theme", ":model", ":provider"):
    r.send(c + "\r"); r.pump(0.8); r.send("\x1b"); r.pump(0.6)
check(count(r, WORDS) == before, "a menu closed with esc leaves nothing in the chat", r)
r.send(":login\r"); r.pump(0.8); L = r.lines()
check(any("Sign in" in l for l in L) and any("ChatGPT" in l and "free or paid" in l for l in L)
      and any("API key" in l for l in L), ":login opens the sign-in menu", r)
check(not any("OpenRouter" in l for l in L), "with no API-key vendors mixed in", r)
r.send("\x1b"); r.pump(0.6)
r.send(":login api\r"); r.pump(0.8); L = r.lines()
check(any("billed by OpenRouter" in l for l in L) and any("separate from ChatGPT" in l for l in L),
      ":login api lists keys by who bills you", r)
r.send("\x1b"); r.pump(0.6)
check(count(r, WORDS) == before, "and both close without a trace", r)
r.send(":theme\r"); r.pump(0.8); r.send("\x1b[B\r"); r.pump(0.8)
check(count(r, WORDS) == before, "a theme picked from the menu prints no chat line either", r)
r.close()
pp = subprocess.run([HAKO, "--pipe"], input=b'{"type":"slash","cmd":":model"}\n{"type":"choice","id":1,"answer":"1"}\n',
                    capture_output=True, cwd=PROJ, env=dict(os.environ, HOME=HOME), timeout=30)
out = pp.stdout.decode("utf-8", "replace")
check('"choice_request"' in out, "bare :model opens the picker over --pipe too (it printed the model)")
check('"model:' not in out and '"text":"model:' not in out, "and the pick says nothing in the chat")

print("§3t :remote on shares the terminal session with a device")
import threading, urllib.request, re as _re
r = start(auto=True)                   # auto in the rc: ignored while remote is on
RP = 18931
r.send(f":remote on {RP}\r"); r.wait_for("?t=", 15)
tok = next((m.group(1) for l in r.all_lines() for m in [_re.search(r"\?t=([a-z0-9]+)", l)] if m), "")
check(len(tok) == 24, ":remote on prints a fresh token", r)
r.pump(0.6)
check(any("remote" in l and "▶▶" in l for l in r.lines()), "the bar says remote", r)
U = f"http://127.0.0.1:{RP}"
H = {"Cookie": "hako_token=" + tok}
def http(path, data=None):
    req = urllib.request.Request(U + path, data=data.encode() if data is not None else None, headers=H,
                                 method="POST" if data is not None else "GET")
    try: return urllib.request.urlopen(req, timeout=3).read().decode()
    except Exception as e: return "ERR " + str(e)
ev = []
def reader():
    try:
        with urllib.request.urlopen(urllib.request.Request(U + "/api/events", headers=H), timeout=60) as f:
            for raw in f:
                if raw.startswith(b"data: "): ev.append(raw[6:].decode())
    except Exception: pass
threading.Thread(target=reader, daemon=True).start()
r.pump(0.8)
check('"remote":1' in http("/api/state"), "the device sees a remote session")
http("/api/prompt", "hello from the phone")
r.wait_for("hello from the phone", 10); settle(r)
check(any('"role":"user"' in e and "hello from the phone" in e for e in ev), "a device prompt runs in the terminal and is mirrored back")
import json as _json
_tdone = lambda: any('"type":"done"' in e and '"turn":0,' not in e for e in ev)
for _ in range(100):
    if _tdone(): break
    r.pump(0.1)
check(_tdone(), "the turn's end reaches the device")
_ds = [_json.loads(e)["text"] for e in ev if '"type":"delta"' in e]
check(len(_ds) >= 1 and "alpha.txt" in "".join(_ds), "the reply streams to the device as it is written")
_mi = next((i for i, e in enumerate(ev) if '"role":"ai"' in e), -1)
check(_mi > 0 and any('"type":"delta"' in e for e in ev[:_mi]), "and the deltas land before the finished message")
check(sum('"role":"ai"' in e for e in ev) == 1, "the finished reply reaches the device once")
http("/api/prompt", "TOOL now")
r.wait_for("llow", 10); r.pump(0.5)
check(any('"type":"tool_request"' in e for e in ev), "the approval goes to the device too (auto is ignored)")
check(not any("│" in l and "always" in l for l in r.lines()), "and the terminal popup offers no always", r)
check(not any("auto mode: tools run" in l for l in r.all_lines()), "nor claims auto mode", r)
check("true" in http("/api/approve", "y"), "the device answers it")
r.pump(1.5); settle(r)
check(any('"type":"tool_end"' in e for e in ev), "the terminal popup closed and the tool ran", r)
n0 = sum("only the desktop terminal" in e for e in ev)
http("/api/slash", ":auto on"); r.pump(0.6)
check(sum("only the desktop terminal" in e for e in ev) > n0, "a device cannot turn on auto")
http("/api/slash", ":model"); r.pump(0.6)
check(any("menus open at the desktop" in e for e in ev), "a device's bare menu is refused, not opened on the desktop")
check(not any("Model" in l and "esc" in l for l in r.lines()), "and no picker opened in the terminal", r)
r.send("TOOL desk\r"); r.wait_for("llow", 10); r.send("\r"); r.pump(1.2); settle(r)
check(any('"type":"tool_decision"' in e and '"answer":"y"' in e for e in ev), "a terminal answer settles the device's box")
http("/api/prompt", "ASK me")
r.wait_for("hich one", 10); r.pump(0.6)
check(any('"type":"choice_request"' in e and "which one" in e for e in ev), "a model's question reaches the device")
check("true" in http("/api/approve", "2"), "the device picks an answer")
r.pump(1.5); settle(r)
check(any("blue" in e for e in ev if '"tool_' in e), "the terminal popup closed with the device's pick", r)
check(not any("│" in l and "hich one" in l for l in r.lines()), "and left no box behind", r)
n0 = sum('"type":"choice_request"' in e for e in ev)
r.send(":theme\r"); r.wait_for("Theme", 10); r.pump(0.5)
check(sum('"type":"choice_request"' in e for e in ev) > n0, "a menu opened at the desk shows on the device too")
r.send("\x1b"); r.pump(0.8)
check(any('"type":"choice_decision"' in e for e in ev), "closing it at the desk settles the device's box")
r.send(":remote off\r"); r.wait_for("remote: off", 10); r.pump(0.5)
check(http("/api/state").startswith("ERR"), ":remote off closes the port")
check(not any("remote" in l and "▶▶" in l for l in r.lines()), "and the bar drops it", r)
r.close()

shutil.rmtree(WORK, ignore_errors=True)
print("RESULT:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
