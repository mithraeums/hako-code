#!/usr/bin/env python3
"""A stand-in for `hakm --serve`: speaks the agent's frame protocol and streams
3-char `T` frames with a small delay, so the terminal UI can be exercised with
no weights, no model and no GPU. The reply is chosen by the LAST user message:
  "TOOL"        -> one list_dir tool call
  "DELEGATE"    -> a delegate call (NAMED: a hako's name as its model, PICKED: another model, WRITE: its task writes a file, TWICE: it delegates again,
                   LONG: its task is SLOW); "SUBWRITE" -> a write_file call
  "TWOTOOLS"    -> two tool calls in one reply (tests the approval handover)
  "ASK"         -> an ask_user question with two options
  "SLOW"        -> the same short answer after 30 s (a cold local model)
  "ECHO"        -> a reply that repeats an <observation> before its sentence (small models do)
  "DRIP"        -> one line streamed slowly, so another hako can write while it is half shown
  "GREET"       -> a word, then a line to the other hako on the chat ("@sho2 hi from sho")
  "PINGPONG"    -> a line back to the other hako, every time (models answering models)
  <observation> -> a one-line wrap-up
  anything else -> a short bulleted answer
Every frame received is appended to $FAKE_LOG, so a test can see what the
model was given (e.g. that a resumed session carried its <observation>s)."""
import os, sys, time
log = open(os.environ.get("FAKE_LOG", "/dev/null"), "a")
inp, out = sys.stdin.buffer, sys.stdout.buffer
def reply_for(last, system=""):
    import re
    me = (re.search(r"You are ([a-z0-9-]+)\.", system) or [None, "me"])[1]
    other = (re.search(r"Also on this chat: ([a-z0-9-]+)", system) or [None, "nobody"])[1]
    if "GREET" in last:
        return "Sure.\n@%s hi from %s" % (other, me)
    if "PINGPONG" in last:
        return "@%s PINGPONG from %s" % (other, me)
    if "DRIP" in last:
        return "one two three four five six seven eight nine ten\nend of drip"
    if "PLAN" in last:
        return ('<tool_call>{"name": "todo", "arguments": {"items": '
                '"[x] read the file\\n[ ] change it\\n[ ] run the tests"}}</tool_call>')
    if "DELEGATE" in last:
        task = ("SUBWRITE please" if "WRITE" in last else "DELEGATE again" if "TWICE" in last
                else "SLOW one" if "LONG" in last else "SUBTASK what files are here")
        model = (', "model": "sho2"' if "NAMED" in last           # a hako's name where a model id goes
                 else ', "model": "hako-koi"' if "PICKED" in last else "")
        return '<tool_call>{"name": "delegate", "arguments": {"task": "%s"%s}}</tool_call>' % (task, model)
    if "SUBWRITE" in last:
        return '<tool_call>{"name": "write_file", "arguments": {"path": "sub.txt", "content": "from the subagent"}}</tool_call>'
    if "TWOTOOLS" in last:
        # two calls in ONE reply: the second approval is where the stdin
        # handover used to race and hang the turn
        return ('<tool_call>{"name": "list_dir", "arguments": {"path": "."}}</tool_call>\n'
                '<tool_call>{"name": "read_file", "arguments": {"path": "alpha.txt"}}</tool_call>')
    if "ASK" in last:
        return ('<tool_call>{"name": "ask_user", "arguments": {"question": "which one?", '
                '"options": "red\\nblue"}}</tool_call>')
    if "ECHO" in last:
        return '<observation tool="run_shell">[2, 3, 5, 7, 11]</observation>\nDone, it printed the primes.'
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
    last = system = ""
    for _ in range(int(n)):
        role = inp.readline().decode().strip()
        ln = int(inp.readline().decode().strip())
        body = inp.read(ln).decode("utf-8", "replace")
        log.write(f"[{role}] {body}\n")
        if role == "user": last = body
        if role == "system": system = body
    log.write("---\n"); log.flush()
    time.sleep(float(os.environ.get("HAKO_FAKE_DELAY", "0")))   # a slow turn, on demand
    if "SLOW" in last: time.sleep(30)
    rep = reply_for(last, system)
    drip = 0.4 if "DRIP" in last else 0.03
    if int(stream):
        for line in rep.split("\n"):
            for i in range(0, max(len(line), 1), 3):
                b = line[i:i+3].encode()
                out.write(f"T {len(b)}\n".encode() + b); out.flush(); time.sleep(drip)
            b = b"\n"; out.write(f"T 1\n".encode() + b); out.flush()
    b = rep.encode()
    out.write(f"R {len(b)}\n".encode() + b); out.flush()
