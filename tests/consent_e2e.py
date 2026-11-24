# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

"""Consent end to end: the built plugin (build/package) in the real makine-plugin-host.exe against the offline test
server. Lines may reach the GTA VI pool only with the player's consent, in either mode.

    py tests/consent_e2e.py        host: MAKINE_PLUGIN_HOST, else ../../Makine-Launcher/build/dev
"""
import json, os, re, shutil, subprocess, sys, threading, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.environ.get("MAKINE_PLUGIN_HOST") or os.path.join(REPO, "..", "..", "Makine-Launcher", "build", "dev",
                                                             "makine-plugin-host.exe")
PKG = os.path.join(REPO, "build", "package")
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, REPO)
import offline  # noqa: E402
from http.server import ThreadingHTTPServer  # noqa: E402

offline.say = lambda msg: None
st, port_ref = offline.State(), [0]
server = ThreadingHTTPServer(("127.0.0.1", 0), offline.make_handler(st, port_ref))
port_ref[0] = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

shutil.rmtree(os.path.join(PKG, ".dev-data"), ignore_errors=True)
env = dict(os.environ, MAKINE_LIVE_SERVER=f"http://127.0.0.1:{port_ref[0]}", PYTHONIOENCODING="utf-8")
p = subprocess.Popen([sys.executable, "-u", os.path.join(REPO, "tools", "plugin_dev.py"), PKG, "--host", HOST],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                     encoding="utf-8", errors="replace")
lines, lock = [], threading.Lock()


def reader():
    for line in p.stdout:
        with lock:
            lines.append(line.rstrip("\n"))


threading.Thread(target=reader, daemon=True).start()
fails = 0


def mark():
    with lock:
        return len(lines)


def since(i):
    with lock:
        return lines[i:]


def last_state(i):
    for line in reversed(since(i)):
        if "emit ui.state" in line:
            return json.loads(line.split("emit ui.state ", 1)[1])
    return None


def cmd(text, wait=0.6):
    i = mark()
    p.stdin.write(text + "\n")
    p.stdin.flush()
    time.sleep(wait)
    return i


def reply(i):
    for line in since(i):
        if line.startswith("  = "):
            return line[4:]
    return ""


def check(cond, label):
    global fails
    print(("ok   " if cond else "FAIL ") + label)
    if not cond:
        fails += 1


cmd("login.start {}", 1.5)
import urllib.request  # noqa: E402
for line in since(0):
    m = re.search(r"http://127\.0\.0\.1:\d+/onay\?kod=[\d-]+", line)
    if m:
        urllib.request.urlopen(m.group(0)).read()
        break
for _ in range(80):
    s = last_state(0)
    if s and s.get("login", {}).get("state") == "signedIn":
        break
    time.sleep(0.5)
check(last_state(0)["login"]["state"] == "signedIn", "signed in against the test server")
if fails:
    print((last_state(0) or {}).get("login")); p.kill(); sys.exit(1)

# console mode, no consent
cmd('mode.set {"mode":"console"}')
i = cmd("start {}")
check("consent_required" in reply(i), "console start without consent is refused")
s = last_state(0)
check(s["reason"]["code"] == "consent_required", "reason says consent_required")
cmd('consoleConsent.set {"granted":true}')
i = cmd("start {}")
check(last_state(i)["running"] is True, "console start with consent runs")
i = cmd('consoleConsent.set {"granted":false}')
check(last_state(i)["running"] is False, "taking consent back stops reading")

# PC mode, then PC -> console while running
cmd('mode.set {"mode":"pc"}')
cmd("region.pick {}", 1)
i = cmd("start {}")
check(last_state(i)["running"] is True, "PC start without consent runs (not the GTA VI pool)")
i = cmd('mode.set {"mode":"console"}')
check(last_state(i)["running"] is False, "switching to console without consent stops reading")

# PC mode with a console stream app: the GTA VI pool
cmd('mode.set {"mode":"pc"}')
i = cmd("start {}")
check(last_state(i)["running"] is True, "PC reading again")
i = cmd('!game.started {"gameId":"x","name":"PS Remote Play","exe":"C:\\\\Program Files\\\\RemotePlay.exe","pid":4}')
check(last_state(i)["running"] is False, "a Remote Play game in PC mode without consent stops reading")
i = cmd("start {}")
check("consent_required" in reply(i), "and start is refused")
cmd('consoleConsent.set {"granted":true}')
i = cmd("start {}")
check(last_state(i)["running"] is True, "with consent it runs")

cmd("quit", 3)
try:
    p.wait(10)
except subprocess.TimeoutExpired:
    p.kill()
out = since(0)
check(any("host exited with 0" in l for l in out), "host exits cleanly")
if fails:
    print("\n".join(out[-60:]))
server.shutdown()
shutil.rmtree(os.path.join(PKG, ".dev-data"), ignore_errors=True)
print("consent e2e:", "passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
