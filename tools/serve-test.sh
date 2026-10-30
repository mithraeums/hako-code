#!/bin/sh
# The --serve answer contract, driven with curl. No model, no network.
# A request the browser never answers must not wedge the serve loop, and an
# answer posted while nothing waits must not resolve the next request.
BIN=${HAKO_BIN:-./hako}
export HAKO_UPDATE_CHECK=0          # never ask GitHub from a test
T=$(mktemp -d "${TMPDIR:-/tmp}/hako-serve.XXXXXX") || exit 1
P=${HAKO_TEST_PORT:-18787}
U=http://127.0.0.1:$P
fail=0
ok() { echo "  ok   $1"; }
no() { echo "  FAIL $1"; fail=1; }

start() {
	rm -rf "$T/h" "$T/p"; mkdir -p "$T/h" "$T/p"
	[ -f "$T/rc" ] && cp "$T/rc" "$T/h/.hakorc"
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
[ "$(code -H 'Host: x.ts.net' -H 'Tailscale-User-Login: stranger@example.com' "$U/api/state")" = 401 ] \
	&& ok "a made-up tailscale identity cannot read" || no "a forged Tailscale-User-Login got in"
kill $pid 2>/dev/null; wait $pid 2>/dev/null

# --token means reachable from another device (the relay, LAN): every tool asks,
# even with auto mode in the rc, an "always" answer counts once, nothing widens
# permissions, and the folder routes stay inside the folder it was started in.
FAKE=$(cd "$(dirname "$0")" && pwd)/tty/fakehakm.py
rm -rf "$T/h" "$T/p"; mkdir -p "$T/h/.hako/models/hako-sho" "$T/p" "$T/out"
echo fake >"$T/h/.hako/models/hako-sho/hako-sho.mlf2"
printf 'ai_provider=mithraeum\nai_model=hako-sho\nshow_splash=0\nai_auto_approve=1\n' >"$T/h/.hakorc"
K='Cookie: hako_token=t0ken42'
HOME="$T/h" HAKO_HAKM="$FAKE" "$BIN" --serve --port $P --token t0ken42 --dir "$T/p" </dev/null >"$T/serve.out" 2>&1 &
pid=$!; i=0; until curl -s -o /dev/null -H "$K" "$U/api/state" || [ $i -ge 50 ]; do sleep 0.1; i=$((i+1)); done
curl -s -N -H "$K" "$U/api/events" >"$T/ev" 2>/dev/null &
cp=$!; sleep 0.3
post() { curl -s -H "$K" -X POST --data "$2" "$U/api/$1"; }
case $(curl -s -H "$K" "$U/api/state") in *'"remote":1'*'"yolo":0'*) ok "state says remote, auto off";; *) no "state did not report remote";; esac
post prompt 'TOOL list it' >/dev/null; sleep 1.2
[ "$(grep -c '"type":"tool_request"' "$T/ev")" = 1 ] && ok "auto mode from the rc still asks" || no "a tool ran without asking"
post approve a >/dev/null; sleep 1.2
post prompt 'TOOL again' >/dev/null; sleep 1.2
[ "$(grep -c '"type":"tool_request"' "$T/ev")" = 2 ] && ok "\"always\" counted once" || no "\"always\" was remembered for a remote device"
post approve n >/dev/null; sleep 0.8
for c in ':auto on' ':mode auto' ':mode' ':trust' ':remote'; do
	n0=$(grep -c 'only the desktop terminal' "$T/ev"); post slash "$c" >/dev/null; sleep 0.4
	[ "$(grep -c 'only the desktop terminal' "$T/ev")" -gt "$n0" ] && ok "refused: $c" || no "a remote device ran $c"
done
case $(curl -s -H "$K" "$U/api/state") in *'"yolo":0'*) ok "still not auto";; *) no "auto mode came on";; esac
st=$(curl -s -H "$K" "$U/api/state")
case $st in *'"places","'*|*'"find","'*) no "a remote device is offered places or find";;
	*'"places":[]'*) ok "no places or find offered remotely";; *) no "places listed for a remote device";; esac
case $(post open "$T") in *false*) ok "cannot open a folder outside";; *) no "opened a folder outside the root";; esac
case $(post mkdir "$T/out/x") in *false*) ok "cannot mkdir outside";; *) no "made a folder outside the root";; esac
case $(post mkdir "sub") in *true*) ok "can mkdir inside";; *) no "mkdir inside was refused";; esac
case $(curl -s -H "$K" "$U/api/dirs?path=/") in *"\"path\":\"$(cd "$T/p" && pwd -P)\""*) ok "listing / shows the root instead";; *) no "listed outside the root";; esac
post prompt 'hello' >/dev/null; sleep 1.5
grep -q '"type":"delta"' "$T/ev" && ok "a headless reply streams as deltas" || no "no deltas from a headless turn"
d=$(grep -n '"type":"delta"' "$T/ev" | tail -1 | cut -d: -f1); m=$(grep -n '"role":"ai"' "$T/ev" | tail -1 | cut -d: -f1)
[ -n "$d" ] && [ -n "$m" ] && [ "$d" -lt "$m" ] && ok "and the finished message follows them" || no "a delta arrived after the finished message"
kill $cp $pid 2>/dev/null; wait $cp $pid 2>/dev/null

# DESIGN 2026-10-29 §4: the optional state fields, and a rename that reaches the page
printf 'theme=nord\nstudio.theme=washi\n' > "$T/rc"
start
st=$(curl -s "$U/api/state")
case $st in *'"commands":[{"n":"help","p":":","c":"app"'*'"n":"compact","p":"/"'*) ok "state carries the command table, : and /";; *) no "no commands in state";; esac
case $st in *'"theme":"nord"'*) ok "state names the theme";; *) no "state has no theme: $st";; esac
case $(curl -s "$U/api/state?for=studio") in *'"theme":"washi"'*) ok "?for=studio gets studio.theme";; *) no "studio.theme not resolved";; esac
case $st in *'"session_name":""'*'"voice":0'*) ok "session_name and voice are there";; *) no "no session_name/voice";; esac
slash ':name pumpkin'; sleep 0.6
grep -q '"type":"state".*"session_name":"pumpkin"' "$T/ev" && ok ":name emits state with the name" || no ":name did not emit state"
case $(curl -s "$U/api/state") in *'"session_name":"pumpkin"'*) ok "and state keeps it";; *) no "state lost the name";; esac
case $st in *'"mode":"ask","shared":false'*) ok "state has mode and shared";; *) no "no mode/shared in state";; esac
n0=$(grep -c '"role":"system"' "$T/ev")
curl -s -o /dev/null -X POST --data ':name quiet one' "$U/api/cmd"; sleep 0.6
case $(curl -s "$U/api/state") in *'"session_name":"quiet one"'*) ok "/api/cmd runs the command";; *) no "/api/cmd did not rename";; esac
[ "$(grep -c '"role":"system"' "$T/ev")" = "$n0" ] && ! grep -q '"text":":name quiet' "$T/ev" && ok "and says nothing in the chat" || no "/api/cmd echoed into the chat"
finish
rm -f "$T/rc"
init=$(printf '{"type":"quit"}\n' | HOME="$T/h" "$BIN" --pipe 2>/dev/null | head -1)
case $init in *'"type":"init"'*'"commands":[{'*'"n":"theme"'*) ok "--pipe init carries the command table";; *) no "init has no commands: $init";; esac

rm -rf "$T"
[ $fail = 0 ] && echo "RESULT: PASS" || { echo "RESULT: FAIL"; exit 1; }
