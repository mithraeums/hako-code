#!/usr/bin/env python3
"""A stand-in for the Anthropic Messages API, streaming SSE, for the pty harness.
Lets the CLOUD render path (the one Claude takes) be tested with no network and
no account. Usage: fakeanthropic.py PORT LOGFILE

Replies are chosen from the last user message, like fakehakm.py:
  "CLAUDETOOL" -> prose, then a text-dialect tool call (what Claude emitted in
                  the dogfood session), then prose on the follow-up turn
  "CLAUDEWRITE" -> a write_file call with a 4 KB body, then prose (the payload
                  that should not ride along in every later request)
  "CLAUDEGOESON" -> a tool call, then prose that invents its result (what Haiku
                  did); the request's stop_sequences cut it like the real API
  anything else -> a multi-line answer, streamed in 3-character deltas, with a
                  delta that ends in a backslash on purpose
Each request body is appended to LOGFILE."""
import sys, json, time
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT, LOG = int(sys.argv[1]), sys.argv[2]

def chunks(s, n=3):
    return [s[i:i + n] for i in range(0, len(s), n)]

def reply_for(body):
    msgs = body.get("messages", [])
    last = msgs[-1] if msgs else {}
    c = last.get("content", "")
    if isinstance(c, list):
        c = " ".join(p.get("text", "") or json.dumps(p.get("content", "")) for p in c if isinstance(p, dict))
    if "observation" in c or "tool_result" in json.dumps(last):
        return "Found alpha.txt in the folder.\nThat is everything here."
    if "CLAUDEWRITE" in c:
        body = "x = 1\\n" * 680
        return 'Writing it now.\n<tool name="write_file">\n{"path": "big.py", "content": "%s"}\n</tool>' % body
    if "CLAUDEGOESON" in c:
        return ('<tool name="list_dir">\n{"path": "."}\n</tool>\n\n'
                "It looks like there's no project directory set up here.")
    if "CLAUDETOOL" in c:
        return 'Let me check what is in the project:\n<tool name="list_dir">\n{"path": "."}\n</tool>'
    return ("Here is a longer answer that wraps across more than one line of the terminal, "
            "so the rail has to carry on under it.\nSecond line: a path C:\\\\temp\\\\ and a "
            "backslash at a chunk edge.\n\n- one\n- two\n\nDone.")

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n)
        with open(LOG, "ab") as f: f.write(raw + b"\n")
        try: body = json.loads(raw)
        except Exception: body = {}
        text = reply_for(body)
        stop_reason = "end_turn"
        for sq in body.get("stop_sequences") or []:      # as the API does: cut, and drop the sequence
            k = text.find(sq)
            if k >= 0: text, stop_reason = text[:k], "stop_sequence"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        def ev(obj):
            self.wfile.write(("event: %s\ndata: %s\n\n" % (obj["type"], json.dumps(obj))).encode())
            self.wfile.flush()
        ev({"type": "message_start", "message": {"id": "m1", "role": "assistant", "content": [],
            "usage": {"input_tokens": 10, "output_tokens": 0}}})
        ev({"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}})
        for c in chunks(text):
            ev({"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": c}})
            time.sleep(0.01)
        ev({"type": "content_block_stop", "index": 0})
        ev({"type": "message_delta", "delta": {"stop_reason": stop_reason}, "usage": {"output_tokens": 20}})
        ev({"type": "message_stop"})

HTTPServer(("127.0.0.1", PORT), H).serve_forever()
