#!/usr/bin/env python3
"""Precache coverage check (docs/RENDERING.md, "Pipeline precache"): plays scripted routes with a
fresh cache (only the shipped list) under several F1 setting presets and counts the pipelines a
draw needed that the precache had not built ("MISS (not precached)" lines of LM_PIPELINE_LOG=1).

    python tools/pipelines/verify.py --work <scratch dir> [--presets vanilla,enh_low,enh_high,enh_ultra]
        [--routes opening,tour,hunt,...] [--user-ini <settings.ini>] [--lock <file>]
        [--exe build/windows-clang/lmrecomp.exe] [--disc orig/GLME01.iso] [--list <pipelines.bin>]

Runs one game at a time (each in its own scratch APPDATA / card folder, muted, no console window;
only the process it started is ended). --lock <file>: a lock file shared with other users of the
machine's one game window, taken (created exclusively, waiting while it exists) around every run
and deleted after it. The routes differ from the recording sweep's (sweep.py): rooms in reverse
order, ghost captures and the room-clear treasure, the lab, events, bosses, the ending.

Each run's pipeline_cache.db holds everything it used: merge them into the list with
`pipelines.py build -o data/pipelines.bin data/pipelines.bin <work>/*/*/app/*/pipeline_cache.db`.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rooms  # noqa: E402
import sweep  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BOOT, INGAME = sweep.BOOT, sweep.INGAME
BASE = {"LM_START_MAP": "2", "LM_TEST_STATE": "poltergust", "LM_SKIP_CUTSCENES": "1", "LM_SKIP_AUTO": "30"}

# Enhanced lighting at the quality presets (the parts at their defaults, as a player turning it on).
ENH_PRESET = {
    "enh_low": {"enhanced_lighting": {"enabled": 1, "preset": 0}},
    "enh_medium": {"enhanced_lighting": {"enabled": 1, "preset": 1}},
    "enh_high": {"enhanced_lighting": {"enabled": 1, "preset": 2}},
    "enh_ultra": {"enhanced_lighting": {"enabled": 1, "preset": 3, "ssao": 1, "realistic_flashlight": 3},
                  "ghost_lights": {"enabled": 1, "shadows": 1}, "world_lights": {"enabled": 1}},
}


def routes(map2: Path):
    pts = rooms.room_points(map2)
    out = {}
    # Boot, title, file select and the whole opening (not skipped), then the foyer.
    out["opening"] = ({}, ["300:START*10", "500:A*6"] + [f"{t}:A*3" for t in range(1200, 3600, 100)]
                      + sweep.explore(3600, 900), 4700)
    # Every room in reverse order with the fire element (fire effects in every room) and the GBH.
    tele, inp, end = sweep.tour(list(reversed(pts)), gbh_every=6)
    out["tour"] = ({**BASE, "LM_TEST_STATE": "poltergust+fire", "LM_SKIP_TEST": tele}, [BOOT] + inp, end + 100)
    # Ghost captures until the room is clear (lights on, the treasure chest, its contents): the
    # hunting aid (docs/RENDERING.md "Ghost lights") in the gold ghosts' room.
    out["hunt"] = ({"LM_TEST_STATE": "poltergust", "LM_TELEPORT": "0,550,-3300,100", "LM_GHOST_HUNT": "9",
                    "LM_GHOST_HUNT_BEAM": "89", "LM_GHOST_HUNT_DMAX": "300", "LM_GHOST_HUNT_R": "0"},
                   ["600:START*10", "1000:A*6"] + [f"{t}:A*3" for t in range(6000, 9000, 60)], 9000)
    # The lab (map 1): E. Gadd, the Portrificationizer room, menus.
    out["lab"] = ({**BASE, "LM_START_MAP": "1"}, [BOOT] + [f"{t}:A*3" for t in range(INGAME, INGAME + 2400, 50)]
                  + sweep.explore(INGAME + 2400, 1200), INGAME + 3700)
    # Entry events (King Boo at the altar, the Boos on the balcony), with arrival events.
    # (Played, not skipped: the cutscene skip fast-forwards without drawing.)
    for p in (9, 5):
        out[f"event{p}"] = ({"LM_START_MAP": "2", "LM_TEST_STATE": "poltergust", "LM_START_APPEAR": str(p)},
                            ["300:START*10", "500:A*6"] + [f"{t}:A*3" for t in range(1200, 5000, 60)]
                            + sweep.explore(5000, 2400), 7500)
    # Bosses: Chauncey, Bogmire (strong vacuum, no damage).
    for m in (10, 13):
        out[f"boss{m}"] = ({**BASE, "LM_START_MAP": str(m)}, [BOOT] + sweep.explore(INGAME, 3600), INGAME + 3700)
    # Ending: rank screen, ending scenes.
    out["ending"] = ({"LM_TEST_ENDING": "4@3700"}, ["300:START*10", "500:A*6"]
                     + [f"{t}:A*3" for t in range(1200, 3600, 100)] + [f"{t}:A*3" for t in range(4300, 11000, 240)],
                     11000)
    return out


PRESET_INI = {"vanilla": {}, **ENH_PRESET}
GAMEPLAY = {"gameplay": {"damage_scale": 0, "ghost_health_scale": 0.25, "vacuum_strength": 4}}


def read_user_ini(path: Path):
    """The user's settings.ini (sections -> keys), windowed (the window mode changes no pipeline)."""
    ini, sec = {}, None
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            sec = ini.setdefault(line[1:-1], {})
        elif "=" in line and sec is not None:
            k, v = line.split("=", 1)
            sec[k] = v
    ini.setdefault("video", {})["fullscreen"] = "false"
    return ini


def take_lock(lock: Path, name: str):
    while True:
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, name.encode())
            os.close(fd)
            return
        except FileExistsError:
            time.sleep(15)


def run(a, preset, route, ini, spec):
    env_extra, inp, frames = spec
    d = Path(a.work) / preset / route
    if d.exists():
        shutil.rmtree(d)
    cfg = d / "app" / "Luigi's Mansion PC"
    cfg.mkdir(parents=True)
    (d / "card").mkdir()
    full = sweep.merge(ini, GAMEPLAY) if route in ("hunt", "boss10", "boss13") else ini
    if route.startswith("event"):
        full = sweep.merge(full, {"debug": {"warp_skip_events": 0}})
    if full:
        sweep.write_ini(cfg / "settings.ini", full)
    e = dict(os.environ)
    e.update({"APPDATA": str(d / "app"), "LM_CARD_DIR": str(d / "card"), "LM_BACKGROUND": "1", "LM_AUDIO": "0",
              "LM_INPUT_CLOCK": "frames", "LM_PIPELINE_LOG": "1", "LM_LOG_TIME": "1"})
    if a.list:
        e["LM_PIPELINE_LIST"] = a.list
    e.update(env_extra)
    seconds = frames / 30 * 1.4 + 120  # + the startup shader preparation
    cmd = [a.exe, "--disc", a.disc, "--input", ",".join(inp)]
    if a.lock:
        take_lock(Path(a.lock), a.lock_name)
    try:
        t0 = time.time()
        with open(d / "log.txt", "w", encoding="utf-8") as log:
            flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            p = subprocess.Popen(cmd, env=e, stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
            try:
                p.wait(timeout=seconds)
            except subprocess.TimeoutExpired:
                p.kill()  # only the process this run started
                p.wait()
    finally:
        if a.lock:
            Path(a.lock).unlink(missing_ok=True)
            time.sleep(a.gap)  # let others waiting for the window take their turn
    text = (d / "log.txt").read_text(encoding="utf-8", errors="replace")
    misses = [l for l in text.splitlines() if "MISS (not precached)" in l]
    start = re.search(r"game starts after ([0-9.]+) s of shader preparation \((\d+) pipelines", text)
    frames_seen = re.findall(r"\[frame\] (\d+) presents", text)
    r = {"preset": preset, "route": route, "misses": len(misses), "seconds": round(time.time() - t0),
         "startup_s": float(start.group(1)) if start else None, "startup_pipelines": int(start.group(2)) if start else None,
         "presents": int(frames_seen[-1]) if frames_seen else 0}
    (d / "misses.txt").write_text("\n".join(misses) + "\n", encoding="utf-8")
    (d / "result.json").write_text(json.dumps(r), encoding="utf-8")
    print(json.dumps(r), flush=True)
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--exe", default=str(ROOT / "build/windows-clang/lmrecomp.exe"))
    ap.add_argument("--data-root", default=str(ROOT), help="checkout holding orig/ and 'Luigis Mansion US/'")
    ap.add_argument("--disc")
    ap.add_argument("--list", help="precache list (LM_PIPELINE_LIST; default: the one next to the exe)")
    ap.add_argument("--presets", default="vanilla,enh_low,enh_high,enh_ultra")
    ap.add_argument("--routes", default="")
    ap.add_argument("--user-ini", help="adds the preset 'user' with this settings.ini")
    ap.add_argument("--lock")
    ap.add_argument("--lock-name", default="pipelines-verify")
    ap.add_argument("--gap", type=float, default=60, help="seconds to wait after a run before taking the lock again")
    ap.add_argument("--resume", action="store_true", help="keep runs an earlier check into --work finished")
    a = ap.parse_args()
    root = Path(a.data_root)
    a.disc = a.disc or str(root / "orig/GLME01.iso")
    rs = routes(root / "Luigis Mansion US/files/Map/map2.szp")
    want = [r for r in a.routes.split(",") if r] or list(rs)
    presets = dict(PRESET_INI)
    if a.user_ini:
        presets["user"] = read_user_ini(Path(a.user_ini))
    results = []
    for preset in [p for p in a.presets.split(",") if p]:
        for route in want:
            done = Path(a.work) / preset / route / "result.json"
            if a.resume and done.exists():
                results.append(json.loads(done.read_text(encoding="utf-8")))
                continue
            results.append(run(a, preset, route, presets[preset], rs[route]))
    (Path(a.work) / "results.json").write_text(json.dumps(results, indent=1), encoding="utf-8")
    for preset in sorted({r["preset"] for r in results}):
        rr = [r for r in results if r["preset"] == preset]
        print(f"{preset}: {sum(r['misses'] for r in rr)} misses over {len(rr)} routes "
              f"({', '.join(f'{r['route']} {r['misses']}' for r in rr)})")


if __name__ == "__main__":
    main()
