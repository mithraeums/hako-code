#!/bin/sh
# The --serve answer contract, driven with curl. No model, no network.
# A request the browser never answers must not wedge the serve loop, and an
# answer posted while nothing waits must not resolve the next request.
BIN=${HAKO_BIN:-./hako}
T=$(mktemp -d "${TMPDIR:-/tmp}/hako-serve.XXXXXX") || exit 1
P=${HAKO_TEST_PORT:-18787}
U=http://127.0.0.1:$P
fail=0
ok() { echo "  ok   $1"; }
no() { echo "  FAIL $1"; fail=1; }

start() {
	rm -rf "$T/h" "$T/p"; mkdir -p "$T/h" "$T/p"
	HOME="$T/h" "$BIN" --serve --port $P --dir "$T/p" </dev/null >"$T/serve.out" 2>&1 &
	pid=$!
	i=0; until curl -s -o /dev/null "$U/api/state" || [ $i -ge 50 ]; do sleep 0.1; i=$((i+1)); done
	curl -s -N "$U/api/events" >"$T/ev" 2>/dev/null &
	cp=$!
	sleep 0.3
}
finish() { kill $cp $pid 2>/dev/null; wait $cp $pid 2>/dev/null; }
slash() { curl -s -o /dev/null -X POST --data "$1" "$U/api/slash"; }
dones() { grep -c '"type":"done"' "$T/ev"; }

start
r=$(curl -s -X POST --data a "$U/api/approve")
case $r in *false*) ok "an answer with nothing waiting is refused";; *) no "stale answer accepted: $r";; esac
slash ':theme'; sleep 0.6
grep -q '"type":"choice_request"' "$T/ev" && ok "a picker emits choice_request" || no "no choice_request"
case $(curl -s "$U/api/state") in *'"awaiting_answer":1'*) ok "state reports the pending answer";; *) no "awaiting_answer not set";; esac
case $(curl -s -X POST --data 2 "$U/api/approve") in *true*) ok "the answer is taken";; *) no "the answer was refused";; esac
sleep 0.5
grep -q '"type":"choice_decision"' "$T/ev" && ok "choice_decision is pushed" || no "no choice_decision"
slash ':help'; sleep 0.8
[ "$(dones)" -ge 2 ] && ok "the next command runs after the pick" || no "wedged after the pick"
finish

start
slash ':provider'; sleep 0.6
curl -s -o /dev/null -X POST "$U/api/stop"; sleep 0.4
slash ':help'; sleep 0.8
[ "$(dones)" -ge 2 ] && ok "stop releases an unanswered picker" || no "stop did not unpark the picker"
case $(curl -s "$U/api/state") in *'"awaiting_answer":0'*) ok "nothing pending after stop";; *) no "awaiting_answer still set";; esac
finish

# Another site must not be able to drive the agent. The Origin check matched
# prefixes, so http://127.0.0.1.evil.example passed; and a DNS-rebinding page
# (Host: its own name) could read with a plain GET.
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
start
[ "$(code -X POST -H 'Origin: http://127.0.0.1.evil.example' --data ':usage' "$U/api/slash")" = 403 ] \
	&& ok "a look-alike origin cannot post" || no "a look-alike origin was accepted"
[ "$(code -X POST -H 'Origin: http://localhost.evil.example' --data ':usage' "$U/api/slash")" = 403 ] \
	&& ok "localhost.<anything> cannot post" || no "localhost.<anything> was accepted"
[ "$(code -H 'Host: evil.example' "$U/api/state")" = 403 ] \
	&& ok "a rebinding Host cannot read" || no "a rebinding Host could read"
[ "$(code -X POST -H "Origin: http://127.0.0.1:$P" --data ':usage' "$U/api/slash")" = 200 ] \
	&& ok "the page's own origin still posts" || no "the page's own origin was refused"
[ "$(code -X POST -H 'Origin: http://192.168.1.20:8787' --data ':usage' "$U/api/slash")" = 200 ] \
	&& ok "a LAN origin (phone over --lan) still posts" || no "a LAN origin was refused"
finish

# With --token, a tunnel's https origin is fine once the token cookie is shown.
rm -rf "$T/h" "$T/p"; mkdir -p "$T/h" "$T/p"
HOME="$T/h" "$BIN" --serve --port $P --token t0ken42 --dir "$T/p" </dev/null >"$T/serve.out" 2>&1 &
pid=$!; i=0; until curl -s -o /dev/null "$U/" || [ $i -ge 50 ]; do sleep 0.1; i=$((i+1)); done
[ "$(code -X POST -H 'Origin: https://abc.trycloudflare.com' -H 'Cookie: hako_token=t0ken42' --data ':usage' "$U/api/slash")" = 200 ] \
	&& ok "a tunnelled page with the token can post" || no "a tunnelled page with the token was refused"
[ "$(code -X POST -H 'Origin: https://abc.trycloudflare.com' --data ':usage' "$U/api/slash")" = 401 ] \
	&& ok "and without the token it cannot" || no "a tunnelled page without the token got in"
kill $pid 2>/dev/null; wait $pid 2>/dev/null

rm -rf "$T"
[ $fail = 0 ] && echo "RESULT: PASS" || { echo "RESULT: FAIL"; exit 1; }
