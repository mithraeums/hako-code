#!/usr/bin/env python3
"""Like fakehakm.py but thinks for HAKO_FAKE_DELAY seconds (default 8) before
the first token — the long-turn case, where the spinner runs and the user is
still expected to be able to type, ctrl+o, or :q."""
import sys, os, time
inp, out = sys.stdin.buffer, sys.stdout.buffer
delay = float(os.environ.get("HAKO_FAKE_DELAY", "8"))
while True:
    hdr = inp.readline()
    if not hdr: break
    n, _ntok, _mt, stream = hdr.split()
    for _ in range(int(n)):
        inp.readline(); ln = int(inp.readline().decode().strip()); inp.read(ln)
    time.sleep(delay)
    rep = "Took a while. Here it is."
    if int(stream):
        for i in range(0, len(rep), 3):
            b = rep[i:i+3].encode()
            out.write(f"T {len(b)}\n".encode() + b); out.flush(); time.sleep(0.05)
    b = rep.encode()
    out.write(f"R {len(b)}\n".encode() + b); out.flush()
