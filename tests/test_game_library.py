"""
Oyun kütüphanesi birim testleri (game_library.py): yalnızca oyunlar algılanır.
Gerçek kayıt defteri / Steam kullanılmaz; sahte Steam ve Epic klasörleri geçici dizinde kurulur.

    py tests/test_game_library.py
"""

import json
import os
import struct
import sys
import tempfile

from _root import ROOT  # noqa: E402,F401
from game_library import (GAME, NOT_GAME, UNKNOWN, GameEntry, GameLibrary, appinfo_verdict, parse_vdf,
                          read_epic, read_steam, read_steam_appinfo, slugify)

FAILS = 0


def check(cond, label):
    global FAILS
    print(("  ok   " if cond else "  HATA ") + label)
    if not cond:
        FAILS += 1


def write(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if isinstance(content, bytes):
        with open(path, "wb") as fh:
            fh.write(content)
    else:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(content)


def acf(appid, name, installdir):
    return f'"AppState"\n{{\n\t"appid"\t\t"{appid}"\n\t"name"\t\t"{name}"\n\t"installdir"\t\t"{installdir}"\n}}\n'


def appinfo_v29(apps):
    """Küçük bir appinfo.vdf (v29) üretir: apps = {appid: {"type": .., "primary_genre": ..}}"""
    strings = ["appinfo", "common", "type", "primary_genre", "name"]
    sid = {k: i for i, k in enumerate(strings)}
    body = b""
    for appid, common in apps.items():
        kv = b"\x00" + struct.pack("<i", sid["appinfo"]) + b"\x00" + struct.pack("<i", sid["common"])
        for k, v in common.items():
            kv += b"\x01" + struct.pack("<i", sid[k]) + str(v).encode() + b"\0"
        kv += b"\x08\x08\x08"
        header = struct.pack("<IIQ", 2, 0, 0) + b"\0" * 20 + struct.pack("<I", 0) + b"\0" * 20
        payload = header + kv
        body += struct.pack("<II", appid, len(payload)) + payload
    body += struct.pack("<I", 0)
    table_off = 16 + len(body)
    table = struct.pack("<I", len(strings)) + b"".join(s.encode() + b"\0" for s in strings)
    return struct.pack("<IIq", 0x07564429, 1, table_off) + body + table


def main():
    tmp = tempfile.mkdtemp(prefix="makine_lib_")
    steam = os.path.join(tmp, "Steam")
    lib2 = os.path.join(tmp, "SteamLibrary")
    common = os.path.join(steam, "steamapps", "common")
    write(os.path.join(steam, "steamapps", "libraryfolders.vdf"),
          '"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n'
          % (steam.replace("\\", "\\\\"), lib2.replace("\\", "\\\\")))
    write(os.path.join(steam, "steamapps", "appmanifest_960910.acf"), acf(960910, "Heavy Rain", "HEAVY RAIN"))
    write(os.path.join(steam, "steamapps", "appmanifest_629520.acf"), acf(629520, "Soundpad", "Soundpad"))
    write(os.path.join(steam, "steamapps", "appmanifest_456610.acf"), acf(456610, "Pro Evolution Soccer 2017", "PES"))
    write(os.path.join(steam, "steamapps", "appmanifest_228980.acf"),
          acf(228980, "Steamworks Common Redistributables", "Steamworks Shared"))
    write(os.path.join(lib2, "steamapps", "appmanifest_3097560.acf"), acf(3097560, "Liar's Bar", "Liar's Bar"))
    write(os.path.join(lib2, "steamapps", "appmanifest_777.acf"), acf(777, "Mystery App", "Mystery"))
    write(os.path.join(steam, "appcache", "appinfo.vdf"), appinfo_v29({
        960910: {"type": "Game", "primary_genre": "25"},
        629520: {"type": "Application", "primary_genre": "57"},
        456610: {"type": "Game", "primary_genre": "18"},       # mağazadan kaldırılmış ama yerelde "Game"
        3097560: {"type": "Game", "primary_genre": "1"},
    }))
    epic = os.path.join(tmp, "Epic")
    write(os.path.join(epic, "a.item"), json.dumps({"DisplayName": "Rocket League", "AppCategories": ["public", "games"],
                                                   "InstallLocation": os.path.join(tmp, "Epic Games", "rocketleague")}))
    write(os.path.join(epic, "b.item"), json.dumps({"DisplayName": "Unreal Engine", "AppCategories": ["applications"],
                                                   "InstallLocation": os.path.join(tmp, "Epic Games", "UE_5.4")}))
    zula = os.path.join(tmp, "Zula", "Game", "zula.exe")
    pes = os.path.join(common, "PES", "PES2017.exe")
    store_calls = []

    def store(appid):
        store_calls.append(appid)
        return {"777": UNKNOWN}.get(appid, NOT_GAME)

    data = os.path.join(tmp, "data")

    def make():
        return GameLibrary(
            data,
            sources={
                "steam": lambda: read_steam(steam),
                "epic": lambda: read_epic(epic),
                "windows": lambda: [GameEntry("zula", "windows", exe=zula), GameEntry("PES2017", "windows", exe=pes)],
            },
            steam_classifier=store,
            appinfo_reader=lambda ids: read_steam_appinfo(steam, ids),
            known_exes={"heavyrain.exe": ("Heavy Rain", "heavy-rain"), "bg3.exe": ("Baldur's Gate 3", "baldurs-gate-3")},
            request_gap_s=0)

    print("[1] Biçimler")
    v = parse_vdf('"a"\n{\n\t"path"\t\t"C:\\\\Games\\\\Steam"\n\t"q"\t"say \\"hi\\""\n}\n')
    check(v == {"a": {"path": "C:\\Games\\Steam", "q": 'say "hi"'}}, "VDF: kaçışlı ters bölü ve tırnak")
    check(len(read_steam(steam)) == 6, "Steam: iki kütüphane klasöründeki 6 uygulama okundu")
    check([e.name for e in read_epic(epic)] == ["Rocket League"], "Epic: yalnızca 'games' kategorisi")
    info = read_steam_appinfo(steam, ["960910", "629520", "999"])
    check(info == {"960910": ("Game", "25"), "629520": ("Application", "57")}, "appinfo.vdf v29 ayrıştırıldı")
    check(appinfo_verdict("Game", "25") == GAME and appinfo_verdict("Application", "57") == NOT_GAME
          and appinfo_verdict("Tool") == NOT_GAME and appinfo_verdict("Game", "57") == NOT_GAME
          and appinfo_verdict("") is None, "appinfo türü → karar")
    check(slugify("Baldur's Gate 3") == "baldurs-gate-3" and slugify("Liar’s Bar") == "liars-bar", "slug: kesme işareti")

    print("[2] Karar")
    lib = make()
    lib.refresh()
    lib.wait_idle(5)
    m = lib.match(os.path.join(common, "HEAVY RAIN", "HeavyRain.exe"))
    check(m is not None and m.name == "Heavy Rain" and m.store == "steam" and m.slug == "heavy-rain", "Steam oyunu")
    check(lib.match(os.path.join(common, "HEAVY RAIN", "UnityCrashHandler64.exe")) is None, "oyun klasöründeki çökme raporlayıcı")
    check(lib.match(os.path.join(common, "HEAVY RAIN", "EasyAntiCheat_EOS_Setup.exe")) is None, "hile koruması kurulumu")
    check(lib.match(os.path.join(common, "Soundpad", "Soundpad.exe")) is None, "Steam uygulaması (Application) oyun değil")
    check(lib.match(os.path.join(common, "Steamworks Shared", "_CommonRedist", "x.exe")) is None, "Steamworks paketleri")
    m = lib.match(pes)
    check(m is not None and m.name == "Pro Evolution Soccer 2017", "mağazadan kaldırılmış oyun (appinfo: Game)")
    m = lib.match(os.path.join(lib2, "steamapps", "common", "Liar's Bar", "Liar's Bar.exe"))
    check(m is not None and m.slug == "liars-bar", "ikinci Steam kütüphanesi")
    check(sorted(store_calls) == ["777"], f"mağazaya yalnızca yerelde türü olmayan uygulama soruldu ({store_calls})")
    check(lib.match(os.path.join(lib2, "steamapps", "common", "Mystery", "Mystery.exe")) is None,
          "türü bilinmeyen Steam uygulaması klasör eşleşmesiyle oyun sayılmaz")
    m = lib.match(os.path.join(tmp, "Epic Games", "rocketleague", "Binaries", "Win64", "RocketLeague.exe"))
    check(m is not None and m.store == "epic", "Epic oyunu")
    check(lib.match(os.path.join(tmp, "Epic Games", "UE_5.4", "Engine", "Binaries", "Win64", "UnrealEditor.exe")) is None,
          "Epic uygulaması (Unreal Engine) oyun değil")
    m = lib.match(zula)
    check(m is not None and m.store == "windows" and m.name == "Zula", "Windows oyun listesi (mağazasız oyun)")
    check(lib.match(r"C:\Program Files\Zen Browser\zen.exe") is None, "tarayıcı (Zen) algılanmaz")
    check(lib.match(r"C:\Users\x\AppData\Roaming\Spotify\Spotify.exe") is None, "Spotify algılanmaz")
    check(lib.match(r"C:\Program Files (x86)\Steam\steam.exe") is None, "Steam istemcisi algılanmaz")
    m = lib.match(r"D:\Oyunlar\BG3\bin\bg3.exe")
    check(m is not None and m.slug == "baldurs-gate-3", "bilinen exe adı (mağaza bilgisi yok)")
    check(lib.summary().get("steam") == 3, f"özet: 3 Steam oyunu ({lib.summary()})")

    print("[3] Kullanıcı işaretleri (kalıcı)")
    tool = r"C:\Tools\MyEmu\emu.exe"
    check(lib.match(tool) is None, "listede olmayan program algılanmaz")
    lib.add_custom(tool, "Benim Emülatörüm")
    lib.mark_not_game(zula)
    lib2_ = make()
    lib2_.refresh()
    m = lib2_.match(tool)
    check(m is not None and m.store == "custom" and m.name == "Benim Emülatörüm", "elle eklenen oyun yeniden açılışta duruyor")
    check(lib2_.match(zula) is None, "'oyun değil' işareti yeniden açılışta duruyor")
    lib2_.add_custom(zula)
    check(lib2_.match(zula) is not None, "yeniden eklenince işaret kalkar")

    print(f"\n{'BAŞARILI' if FAILS == 0 else f'{FAILS} HATA'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
