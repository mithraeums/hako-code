#!/usr/bin/env python3
"""A stand-in for chatgpt.com/backend-api/codex (the Responses API behind a
ChatGPT sign-in), streaming SSE, for the pty harness. Usage: fakechatgpt.py PORT LOGFILE

Replies are chosen from the last input item:
  "GPTTOOL"   -> prose, then a text-dialect tool call, then prose on the follow-up
  "GPTGOESON" -> a tool call, then prose that invents its result; this API has no
                 stop sequences, so hako has to hang up at </tool> itself
  anything else -> a short answer, streamed in 3-character deltas
GET /models answers with a codex-shaped catalog ("slug" keys). Each request is
appended to LOGFILE as one JSON line: path, the headers hako sent, and the body."""
import sys, json, time
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT, LOG = int(sys.argv[1]), sys.argv[2]

def chunks(s, n=3):
    return [s[i:i + n] for i in range(0, len(s), n)]

def text_of(item):
    c = item.get("content", "")
    if isinstance(c, list):
        c = " ".join(p.get("text", "") for p in c if isinstance(p, dict))
    return c

def call(name, args):
    return '<tool name="%s">\n%s\n</tool>' % (name, json.dumps(args))

def repeat_scene(prompt, c, first):
    """Calls repeated inside one turn (r.txt holds FIRSTLINE):
      GPTREREAD:  read r.txt, write SECONDLINE, read r.txt again (same call, file changed)
      GPTDUP:     read r.txt twice with nothing between; answers the repeat notice
      GPTDUPLOOP: reads r.txt whatever it is told"""
    read = call("read_file", {"path": "r.txt"})
    if "GPTDUPLOOP" in prompt:
        return read
    if first:
        return read
    if "(repeat)" in c:
        return "REPEAT NOTICE READ" if "GPTDUP" in prompt else "REREAD REFUSED"
    if "GPTDUP" in prompt:
        return read
    if "wrote" in c:
        return read
    if "SECONDLINE" in c:
        return "REREAD SAW SECONDLINE"
    return call("write_file", {"path": "r.txt", "content": "SECONDLINE\n"})

def reply_for(body):
    items = body.get("input", [])
    last = items[-1] if items else {}
    c = text_of(last)
    prompts = [text_of(i) for i in items if i.get("role") == "user" and "<observation" not in text_of(i)]
    prompt = prompts[-1] if prompts else ""
    if "GPTDELEGATE" in prompt and c == prompt:    # a subagent that will end on a call that never runs
        return call("delegate", {"task": "GPTDUPLOOP read r.txt"})
    if "GPTDELEGATE" in prompt:
        return "DELEGATE CAME BACK"
    if any(k in prompt for k in ("GPTREREAD", "GPTDUP")):
        return repeat_scene(prompt, c, c == prompt)
    if "observation" in c:
        return "Found alpha.txt in the folder.\nThat is everything here."
    if "GPTGOESON" in c:
        return ('<tool name="list_dir">\n{"path": "."}\n</tool>\n\n'
                "It looks like there's no project directory set up here.")
    if "GPTTOOL" in c:
        return 'Let me check what is in the project:\n<tool name="list_dir">\n{"path": "."}\n</tool>'
    return "Hello from the fake ChatGPT.\n\n- one\n- two\n\nDone."

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def log_req(self, body):
        with open(LOG, "a") as f:
            f.write(json.dumps({"path": self.path, "headers": dict(self.headers), "body": body}) + "\n")

    def do_GET(self):
        self.log_req(None)
        if "/models" in self.path:
            # spaced as json.dumps writes it; slugs no built-in list has, so the
            # check cannot pass on the fallback list
            out = json.dumps({"models": [{"slug": "gpt-fake-hidden", "visibility": "hide", "priority": 0},
                                         {"slug": "gpt-fake-mini", "visibility": "list", "priority": 2},
                                         {"slug": "gpt-5.5", "visibility": "list", "priority": 1}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)
            return
        self.send_response(404); self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or b"{}")
        self.log_req(body)
        if not self.path.endswith("/responses"):
            self.send_response(404); self.end_headers(); return
        if body.get("store") is not False or "instructions" not in body:
            out = json.dumps({"detail": "Store must be false and instructions are required"}).encode()
            self.send_response(400)
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)
            return
        if "GPTLIMIT" in json.dumps(body.get("input", [])):    # the plan's usage limit, as the codex backend says it
            out = json.dumps({"error": {"type": "usage_limit_reached", "message": "The usage limit has been reached",
                                        "plan_type": "plus", "resets_in_seconds": 11520}}).encode()
            self.send_response(429)
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()

        def ev(obj):
            self.wfile.write(("event: %s\ndata: %s\n\n" % (obj["type"], json.dumps(obj))).encode())
            self.wfile.flush()

        try:
            ev({"type": "response.created", "response": {"id": "resp_1", "usage": None}})
            for piece in chunks(reply_for(body)):
                ev({"type": "response.output_text.delta", "item_id": "msg_1",
                    "output_index": 0, "content_index": 0, "delta": piece})
                time.sleep(0.02)
            ev({"type": "response.completed",
                "response": {"id": "resp_1", "usage": {"input_tokens": 42, "output_tokens": 7}}})
        except (BrokenPipeError, ConnectionResetError):
            pass        # hako hung up at </tool>, as it should

HTTPServer(("127.0.0.1", PORT), H).serve_forever()
