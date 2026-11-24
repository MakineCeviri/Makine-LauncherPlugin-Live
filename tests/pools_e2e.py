# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

"""Game pools, maintenance and the support log end to end: the built plugin (build/package) in the real
makine-plugin-host.exe against the offline test server (tools/offline.py, pools in a temporary folder).

  - ui.state pools: the library (games.list), GTA VI, states, order; pools.refresh / download / downloadAll;
    new approvals of a pool this PC has come down by themselves after a refresh
  - maintenance stops reading and reading starts again when it ends
  - the log the team asked for reaches the server when Sentry took it but the server has no log_sent

    py tests/pools_e2e.py        host: MAKINE_PLUGIN_HOST, else ../../Makine-Launcher or C:\Makine-Launcher build/dev
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.environ.get("MAKINE_PLUGIN_HOST") or next(
    (h for h in (os.path.join(REPO, "..", "..", "Makine-Launcher", "build", "dev", "makine-plugin-host.exe"),
                 r"C:\Makine-Launcher\build\dev\makine-plugin-host.exe") if os.path.isfile(h)), "")
PKG = os.path.join(REPO, "build", "package")
DATA = os.path.join(PKG, ".dev-data")
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, REPO)
import offline  # noqa: E402
from http.server import ThreadingHTTPServer  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8")
if not os.path.isfile(HOST):
    print("pools e2e: skipped (no makine-plugin-host.exe; set MAKINE_PLUGIN_HOST)")
    sys.exit(0)

offline.say = lambda msg: None
offline.POOL = tempfile.mkdtemp(prefix="makine_pools_e2e_")
MAINT = {"on": False}
LOGS = []   # (action, size) the server got


def write_pool(slug, name, rev, n):
    items = [[i + 1, f"line {i + 1} of {slug}", f"Line {i + 1} of {name}.", f"{name} satırı {i + 1}."] for i in range(n)]
    with open(os.path.join(offline.POOL, slug + ".json"), "w", encoding="utf-8") as f:
        json.dump({"rev": rev, "name": name, "items": items}, f, ensure_ascii=False)


st, port_ref = offline.State(), [0]
Base = offline.make_handler(st, port_ref)


class Handler(Base):
    def _route(self, method, body):
        u = urllib.parse.urlsplit(self.path)
        action = dict(urllib.parse.parse_qsl(u.query)).get("action", "")
        if u.path == "/api/live/app" and action == "status" and MAINT["on"]:
            return self._send(200, {"success": True, "maintenance": {"on": True, "message": "Test bakımı."},
                                    "update": {"latest": None, "available": False, "mandatory": False},
                                    "announcements": []})
        if u.path == "/api/live/app" and action in ("log_sent", "log_upload"):
            LOGS.append((action, len(body.get("_raw", b""))))
            if action == "log_sent":   # Makine-WEB has no log_sent: "Geçersiz işlem."
                return self._send(400, {"success": False, "error": "Geçersiz işlem."})
        return super()._route(method, body)


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port_ref[0] = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
ENV = dict(os.environ, MAKINE_LIVE_SERVER=f"http://127.0.0.1:{port_ref[0]}", PYTHONIOENCODING="utf-8",
           MAKINE_DEV_GAMES=json.dumps([{"id": "1", "name": "Heavy Rain"}, {"id": "2", "name": "Alan Wake"},
                                        {"id": "3", "name": "1234"}]),
           MAKINE_DEV_SENTRY_LOGS="1")
fails = 0


def check(cond, label):
    global fails
    print(("ok   " if cond else "FAIL ") + label)
    if not cond:
        fails += 1


class Session:
    def __init__(self):
        self.p = subprocess.Popen([sys.executable, "-u", os.path.join(REPO, "tools", "plugin_dev.py"), PKG,
                                   "--host", HOST], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, env=ENV, encoding="utf-8", errors="replace")
        self.lines, self.lock = [], threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            with self.lock:
                self.lines.append(line.rstrip("\n"))

    def since(self, i=0):
        with self.lock:
            return self.lines[i:]

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
        # plugin_dev prints host calls from another thread: the answer may share a line with one
        line = next((l for l in self.since(i) if "  = {" in l), "")
        return line[line.find("  = {") + 4:].split("  -> ", 1)[0]

    def wait_for(self, pred, timeout=30):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            s = self.state()
            if s and pred(s):
                return s
            time.sleep(0.2)
        return self.state()

    def quit(self):
        self.cmd("quit", 0)
        try:
            self.p.wait(15)
        except subprocess.TimeoutExpired:
            self.p.kill()


def item(state, slug):
    return next((x for x in state["pools"]["items"] if x["slug"] == slug), None)


write_pool("heavy-rain", "Heavy Rain", "r2", 3)
write_pool("alan-wake", "Alan Wake", "r1", 2)
shutil.rmtree(DATA, ignore_errors=True)
s = Session()
time.sleep(1.5)
first = s.state()
check(first is not None and set(first["pools"]) == {"refreshing", "updatedAt", "items"}
      and first["pools"]["updatedAt"] is None, "pools in ui.state before signing in")
slugs = [x["slug"] for x in first["pools"]["items"]]
check("heavy-rain" in slugs and "alan-wake" in slugs and "grand-theft-auto-vi" in slugs and len(slugs) == 3,
      "candidates: the launcher's library (a name without letters skipped) and GTA VI " + json.dumps(slugs))
check(all(x["remote"] is None and x["local"] == 0 and x["state"] == "missing" for x in first["pools"]["items"]),
      "nothing asked yet: remote null")
i = s.cmd("pools.refresh {}")
check("giriş yap" in s.reply(i), "pools.refresh signed out: refused in Turkish")

s.cmd("login.start {}", 1.5)
for line in s.since():
    m = re.search(r"http://127\.0\.0\.1:\d+/onay\?kod=[\d-]+", line)
    if m:
        urllib.request.urlopen(m.group(0)).read()
        break
check(s.wait_for(lambda x: x["login"]["state"] == "signedIn")["login"]["state"] == "signedIn", "signed in")
auto = s.wait_for(lambda x: x["pools"]["updatedAt"] is not None and not x["pools"]["refreshing"])
check(auto["pools"]["updatedAt"] is not None and item(auto, "grand-theft-auto-vi")["state"] == "empty"
      and item(auto, "heavy-rain")["remote"] is None,
      "automatic check after sign-in: the pools on this PC and GTA VI only, not the whole library")

i = s.cmd("pools.refresh {}")
check(s.reply(i) == '{"message": "Havuzlar yenileniyor."}', "pools.refresh answers " + s.reply(i))
full = s.wait_for(lambda x: not x["pools"]["refreshing"] and item(x, "heavy-rain")["remote"] is not None)
order = [(x["slug"], x["state"], x["remote"]) for x in full["pools"]["items"]]
check(order == [("heavy-rain", "missing", 3), ("alan-wake", "missing", 2), ("grand-theft-auto-vi", "empty", 0)],
      "head syncs: missing by server count first, then the rest " + json.dumps(order))
check(not os.path.isfile(os.path.join(DATA, "pool", "alan-wake.json")), "a missing pool is never downloaded by itself")

i = s.cmd('pools.download {"slug":"alan-wake"}')
check("Alan Wake havuzu indiriliyor" in s.reply(i), "pools.download answers " + s.reply(i))
got = s.wait_for(lambda x: item(x, "alan-wake")["state"] == "current")
check(item(got, "alan-wake") == {"slug": "alan-wake", "name": "Alan Wake", "local": 2, "remote": 2, "state": "current",
                                 "playing": False}, "downloaded: current " + json.dumps(item(got, "alan-wake")))
i = s.cmd('pools.download {"slug":"no-such-game"}')
check("listede yok" in s.reply(i), "an unknown slug is refused")

write_pool("alan-wake", "Alan Wake", "r2", 5)   # 3 more approved on the server
s.cmd("pools.refresh {}", 0.2)
upd = s.wait_for(lambda x: not x["pools"]["refreshing"] and item(x, "alan-wake")["local"] == 5, 30)
check(item(upd, "alan-wake")["state"] == "current" and item(upd, "alan-wake")["local"] == 5,
      "new approvals of a pool this PC has come down by themselves " + json.dumps(item(upd, "alan-wake")))
check(item(upd, "heavy-rain")["local"] == 0, "…and a pool never chosen stays missing")

i = s.cmd("pools.downloadAll {}")
check("1 havuz sırayla indiriliyor" in s.reply(i), "pools.downloadAll: the missing one " + s.reply(i))
done = s.wait_for(lambda x: item(x, "heavy-rain")["state"] == "current")
check(item(done, "heavy-rain")["local"] == 3, "downloadAll fetched it")
i = s.cmd("pools.downloadAll {}")
check("hepsi güncel" in s.reply(i), "nothing left to download " + s.reply(i))

# the support log: Sentry (telemetry.logs) took it, the server has no log_sent → uploaded as well
sup = s.wait_for(lambda x: x.get("support") is not None, 20)
check(sup.get("support") is not None and any(a == "log_sent" for a, n in LOGS)
      and any(a == "log_upload" and n > 20 for a, n in LOGS),
      "support log: log_sent refused → uploaded, the card shows " + json.dumps(LOGS))

# maintenance: reading stops and starts again by itself
s.cmd('mode.set {"mode":"console"}')
s.cmd('consoleConsent.set {"granted":true}')
i = s.cmd("start {}")
check(s.state(i)["running"] is True, "reading")
MAINT["on"] = True
s.cmd("status.refresh {}", 1.0)
m = s.state()
check(m["running"] is False and m["server"]["maintenance"] is True and m["reason"]["code"] == "maintenance"
      and m["reason"]["text"] == "Test bakımı.", "maintenance: reading stops, the server's text shows")
i = s.cmd("start {}")
check("Test bakımı" in s.reply(i), "no start during maintenance")
i = s.cmd("pools.refresh {}")
check("bakımda" in s.reply(i), "no pool refresh during maintenance")
MAINT["on"] = False
s.cmd("status.refresh {}", 1.0)
r = s.wait_for(lambda x: x["running"], 5)
check(r["running"] is True and r["server"]["maintenance"] is False, "maintenance over: reading starts again by itself")
s.cmd("stop {}")
s.quit()

with open(os.path.join(DATA, "settings.cfg"), encoding="utf-8") as f:
    check(re.search(r"^poolsCheckedAt=\d{10}$", f.read(), re.M) is not None, "the last check is kept in settings")
server.shutdown()
shutil.rmtree(DATA, ignore_errors=True)
shutil.rmtree(offline.POOL, ignore_errors=True)
print("pools e2e:", "passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
