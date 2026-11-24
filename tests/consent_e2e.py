# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

"""Consent end to end: the built plugin (build/package) in the real makine-plugin-host.exe against the offline test
server. Lines may reach the GTA VI pool only with the player's consent, in either mode. Also: the settings a session
leaves (consent, a style drag, the login) are there in the next one, and shutdown fits the launcher's 1.5 s.

    py tests/consent_e2e.py        host: MAKINE_PLUGIN_HOST, else ../../Makine-Launcher/build/dev
"""
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.environ.get("MAKINE_PLUGIN_HOST") or os.path.join(REPO, "..", "..", "Makine-Launcher", "build", "dev",
                                                             "makine-plugin-host.exe")
PKG = os.path.join(REPO, "build", "package")
DATA = os.path.join(PKG, ".dev-data")
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, REPO)
import offline  # noqa: E402
from http.server import ThreadingHTTPServer  # noqa: E402

offline.say = lambda msg: None
st, port_ref = offline.State(), [0]
server = ThreadingHTTPServer(("127.0.0.1", 0), offline.make_handler(st, port_ref))
port_ref[0] = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
ENV = dict(os.environ, MAKINE_LIVE_SERVER=f"http://127.0.0.1:{port_ref[0]}", PYTHONIOENCODING="utf-8")
fails = 0


def check(cond, label):
    global fails
    print(("ok   " if cond else "FAIL ") + label)
    if not cond:
        fails += 1


class Session:
    """plugin_dev.py driving the real plugin host; every output line kept with its arrival time."""

    def __init__(self):
        self.p = subprocess.Popen([sys.executable, "-u", os.path.join(REPO, "tools", "plugin_dev.py"), PKG,
                                   "--host", HOST], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, env=ENV, encoding="utf-8", errors="replace")
        self.lines, self.lock = [], threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            with self.lock:
                self.lines.append((time.monotonic(), line.rstrip("\n")))

    def since(self, i=0):
        with self.lock:
            return [l for _, l in self.lines[i:]]

    def state(self, i=0):
        for line in reversed(self.since(i)):
            if "emit ui.state" in line:
                return json.loads(line.split("emit ui.state ", 1)[1])
        return None

    def cmd(self, text, wait=0.6):
        with self.lock:
            i = len(self.lines)
        self.p.stdin.write(text + "\n")
        self.p.stdin.flush()
        time.sleep(wait)
        return i

    def reply(self, i):
        return next((l[4:] for l in self.since(i) if l.startswith("  = ")), "")

    def wait_for(self, pred, timeout=40):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            s = self.state()
            if s and pred(s):
                return s
            time.sleep(0.25)
        return self.state()

    def quit(self):
        """Returns the host's shutdown time: from the shutdown request to the host's exit."""
        self.cmd("quit", 0)
        try:
            self.p.wait(15)
        except subprocess.TimeoutExpired:
            self.p.kill()
        with self.lock:
            t0 = next((t for t, l in self.lines if l == "shutdown"), None)
            t1 = next((t for t, l in self.lines if l.startswith("host exited with 0")), None)
        return None if t0 is None or t1 is None else t1 - t0


shutil.rmtree(DATA, ignore_errors=True)
s = Session()
time.sleep(1)
check(s.state()["reason"] == {"code": "signed_out", "text": "Makine hesabınla giriş yap.", "action": "login.start"},
      "reason.action is an invoke method the screen calls as is")
s.cmd("login.start {}", 1.5)
for line in s.since():
    m = re.search(r"http://127\.0\.0\.1:\d+/onay\?kod=[\d-]+", line)
    if m:
        urllib.request.urlopen(m.group(0)).read()
        break
check(s.wait_for(lambda st: st["login"]["state"] == "signedIn")["login"]["state"] == "signedIn",
      "signed in against the test server")
if fails:
    s.p.kill()
    sys.exit(1)

# console mode, no consent
s.cmd('mode.set {"mode":"console"}')
i = s.cmd("start {}")
check("consent_required" in s.reply(i), "console start without consent is refused")
check(s.state()["reason"]["code"] == "consent_required" and "action" not in s.state()["reason"],
      "reason says consent_required, no action (the screen asks)")
i = s.cmd('consoleConsent.set {"enabled":true}')   # the screen may send enabled instead of granted
check(s.state(i)["consoleConsent"] is True, "consent given as {enabled}")
i = s.cmd("start {}")
st1 = s.state(i)
check(st1["running"] is True, "console start with consent runs")
c = st1["counters"]
check(set(c) == {"lines", "poolHits", "hitPct", "avgMs", "startedAt"} and isinstance(c["startedAt"], int)
      and c["hitPct"] is None and c["avgMs"] is None, "counters while reading " + json.dumps(c))
i = s.cmd('consoleConsent.set {"granted":false}')
check(s.state(i)["running"] is False and s.state(i)["counters"]["startedAt"] is None,
      "taking consent back stops reading (startedAt null)")

# status.refresh, announcements read by number or string id, the sample on every output for 4 s
i = s.cmd("status.refresh {}")
check("Yenileniyor" in s.reply(i), "status.refresh answers")
i = s.cmd('announcement.read {"id":"12"}')
check("Okundu" in s.reply(i), "announcement id as a string")
i = s.cmd('announcement.read {"id":"x"}')
check("id gerekli" in s.reply(i), "a non-numeric id is refused")
s.cmd('mode.set {"mode":"pc"}')
i = s.cmd("sample {}", 0.8)
shown = [l for l in s.since(i) if "emit overlay.show" in l]
check(len(shown) == 1 and '"kind": "verified"' in shown[0], "sample shows one line")
time.sleep(4.2)
check(any("emit overlay.hide" in l for l in s.since(i)), "and hides it after 4 s")

# the subtitle finder: in the state, nothing to accept yet, reset searches again, PC start needs no region
st = s.state()
check(set(st["autoRegion"]) == {"state", "rect", "confidence"} and st["autoRegion"]["rect"] is None,
      "autoRegion in the state " + json.dumps(st["autoRegion"]))
i = s.cmd("autoRegion.accept {}")
check("bölgesi yok" in s.reply(i), "autoRegion.accept with nothing found is refused")
i = s.cmd("autoRegion.reset {}")
check("aranıyor" in s.reply(i), "autoRegion.reset searches again")
i = s.cmd("start {}")
check(s.state(i)["running"] is True, "PC start without a region runs (the finder looks for the subtitles)")
s.cmd("stop {}")

# PC mode, then PC -> console while running
s.cmd('mode.set {"mode":"pc"}')
s.cmd("region.pick {}", 1)
i = s.cmd("start {}")
check(s.state(i)["running"] is True, "PC start without consent runs (not the GTA VI pool)")
i = s.cmd('mode.set {"mode":"console"}')
check(s.state(i)["running"] is False, "switching to console without consent stops reading")

# PC mode with a console stream app: the GTA VI pool
s.cmd('mode.set {"mode":"pc"}')
i = s.cmd("start {}")
check(s.state(i)["running"] is True, "PC reading again")
i = s.cmd('!game.started {"gameId":"x","name":"PS Remote Play","exe":"C:\\\\Program Files\\\\RemotePlay.exe","pid":4}')
check(s.state(i)["running"] is False, "a Remote Play game in PC mode without consent stops reading")
i = s.cmd("start {}")
check("consent_required" in s.reply(i), "and start is refused")
s.cmd('consoleConsent.set {"granted":true}')
i = s.cmd("start {}")
check(s.state(i)["running"] is True, "with consent it runs")

# a slider drag: many style.set ticks, the last one quit before the 500 ms debounce
for size in range(30, 38):
    s.cmd('style.set {"size":%d}' % size, 0.05)
took = s.quit()
check(took is not None and took < 1.5, f"shutdown while signed in and reading fits 1.5 s ({took if took is None else round(took, 2)} s)")
if fails:
    print("\n".join(s.since()[-60:]))

# the next session finds what this one left
s = Session()
st2 = s.wait_for(lambda st: st["login"]["state"] == "signedIn", 20)
check(st2["login"]["state"] == "signedIn", "the login is kept")
check(st2.get("consoleConsent") is True, "consent is kept")
check(st2.get("style", {}).get("size") == 37, "the last style tick is kept (saved at shutdown)")
check(st2.get("running") is False, "nothing reads before the player starts it")
check(st2.get("history") == [], "an empty session history")

# hotkeys: taken only with a game (or console mode) while signed in; F9 by default
check(st2["hotkeys"]["armed"] is False and st2["hotkeys"]["keys"]["start"]["key"] == "F9", "no game: keys left alone")
i = s.cmd('!game.started {"gameId":"g","name":"Test Game","pid":1234,"exe":"C:\\\\Games\\\\game.exe"}', 1)
hk = s.state(i)["hotkeys"]
check(hk["armed"] is True and all(k["active"] or k["error"] for k in hk["keys"].values()),
      "a game: keys taken, or the reason they are not " + json.dumps(hk["keys"], ensure_ascii=False))
i = s.cmd("box.move {}")
check("desteklemiyor" in s.reply(i) and s.state()["boxMove"]["supported"] is False,
      "box move without overlay.setMovable: unsupported, said plainly")
i = s.cmd('!game.stopped {"gameId":"g"}')
check(s.state(i)["hotkeys"]["armed"] is False, "game closed: keys given back")

# games outside the catalogue (launcher 0.3: gameId "exe:<exe>", catalog false)
i = s.cmd('!game.started {"gameId":"exe:foo bar.exe","name":"C:\\\\Games\\\\Foo Bar.exe","pid":77,'
          '"exe":"C:\\\\Games\\\\Foo Bar.exe","catalog":false}')
g = s.state(i)["game"]
check(g == {"name": "Foo Bar", "slug": "foo-bar"}, "a path for a name pools under its stem, never the path " + json.dumps(g))
i = s.cmd('!game.started {"gameId":"exe:x.exe","name":"  ","pid":78,"exe":"x.exe","catalog":false}')
st4 = s.state(i)
check(st4["game"]["slug"] == "" and (st4["reason"] or {}).get("code") == "game_unidentified",
      "no name: not read, reason game_unidentified " + json.dumps([st4["game"], st4["reason"]], ensure_ascii=False))
i = s.cmd('!game.started {"gameId":"exe:remoteplay.exe","name":"RemotePlay","pid":79,'
          '"exe":"C:\\\\Program Files\\\\Sony\\\\RemotePlay.exe","catalog":false}')
check(s.state(i)["game"]["slug"] == "grand-theft-auto-vi", "a console stream is never a PC game of its own")
s.cmd('!game.stopped {"gameId":"exe:remoteplay.exe"}')
i = s.cmd('!overlay.moved {"x":100,"y":200,"width":400,"height":100}')
style = s.state(i)["style"]
check(0 < style.get("boxX", -1) < 1 and 0 < style.get("boxY", -1) < 1, "overlay.moved keeps the box centre in style")

# team account: username + password, the password never written
s.cmd("logout {}", 1.5)
check(s.wait_for(lambda st: st["login"]["state"] == "signedOut", 10)["login"]["state"] == "signedOut", "signed out")
PASSWORD = "parola-e2e-7319"
s.cmd('login.team {"username":"ekip","password":"%s"}' % PASSWORD, 1)
st3 = s.wait_for(lambda st: st["login"]["state"] == "signedIn", 10)
check(st3["login"]["state"] == "signedIn" and st3["login"]["user"] == "ekip" and st3["login"]["role"] == "admin",
      "team login with username and password")
s.quit()
stored = open(os.path.join(DATA, "settings.cfg"), "rb").read()
check(PASSWORD.encode() not in stored and "ekip".encode() in stored, "the password is not stored, the session is")

server.shutdown()
shutil.rmtree(DATA, ignore_errors=True)
print("consent e2e:", "passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
