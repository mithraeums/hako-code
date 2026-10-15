#!/usr/bin/env python3
"""A stand-in for `hakm --serve`: speaks the agent's frame protocol and streams
3-char `T` frames with a small delay, so the terminal UI can be exercised with
no weights, no model and no GPU. The reply is chosen by the LAST user message:
  "TOOL"        -> one list_dir tool call
  "TWOTOOLS"    -> two tool calls in one reply (tests the approval handover)
  <observation> -> a one-line wrap-up
  anything else -> a short bulleted answer
Every frame received is appended to $FAKE_LOG, so a test can see what the
model was given (e.g. that a resumed session carried its <observation>s)."""
import os, sys, time
log = open(os.environ.get("FAKE_LOG", "/dev/null"), "a")
inp, out = sys.stdin.buffer, sys.stdout.buffer
def reply_for(last):
    if "PLAN" in last:
        return ('<tool_call>{"name": "todo", "arguments": {"items": '
                '"[x] read the file\\n[ ] change it\\n[ ] run the tests"}}</tool_call>')
    if "TWOTOOLS" in last:
        # two calls in ONE reply: the second approval is where the stdin
        # handover used to race and hang the turn
        return ('<tool_call>{"name": "list_dir", "arguments": {"path": "."}}</tool_call>\n'
                '<tool_call>{"name": "read_file", "arguments": {"path": "alpha.txt"}}</tool_call>')
    if "TOOL" in last:
        return '<tool_call>{"name": "list_dir", "arguments": {"path": "."}}</tool_call>'
    if "observation" in last:
        if os.environ.get("FAKE_QUIET_AFTER_TOOL"): return ""   # a small model ending a finished task
        return "Done. The directory has what you see above."
    return "Okay, here is the list:\n\n* `alpha.txt`\n* `beta.txt`\n* `gamma/`\n\nWhat next?"
while True:
    hdr = inp.readline()
    if not hdr: break
    n, _ntok, _mt, stream = hdr.split()
    last = ""
    for _ in range(int(n)):
        role = inp.readline().decode().strip()
        ln = int(inp.readline().decode().strip())
        body = inp.read(ln).decode("utf-8", "replace")
        log.write(f"[{role}] {body}\n")
        if role == "user": last = body
    log.write("---\n"); log.flush()
    time.sleep(float(os.environ.get("HAKO_FAKE_DELAY", "0")))   # a slow turn, on demand
    rep = reply_for(last)
    if int(stream):
        for line in rep.split("\n"):
            for i in range(0, max(len(line), 1), 3):
                b = line[i:i+3].encode()
                out.write(f"T {len(b)}\n".encode() + b); out.flush(); time.sleep(0.03)
            b = b"\n"; out.write(f"T 1\n".encode() + b); out.flush()
    b = rep.encode()
    out.write(f"R {len(b)}\n".encode() + b); out.flush()
