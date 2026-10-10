#!/usr/bin/env python3
"""Automated sweep that records the render pipelines the game uses (docs/RENDERING.md,
"Pipeline precache"), then writes data/pipelines.bin.

Runs the dev build (lmrecomp) through a set of scripted scenarios: boot/title/menus/opening,
a teleport tour of every mansion room (points from the map's furniture table, rooms.py) with
the Poltergust, flashlight, Game Boy Horror and elements, every other map, the Hidden Mansion
(USA and PAL rules, mirrored), and the unused-content shader variants. Every scenario gets its
own scratch APPDATA (Aurora's pipeline_cache.db records each config the moment it is first
used), so scenarios run in parallel; the databases are merged with pipelines.py.

    python tools/pipelines/sweep.py --work <scratch dir> [--jobs 4] [--only tour,maps]
        [--exe build/windows-clang/lmrecomp.exe] [--disc orig/GLME01.iso]
        [--map2 "Luigis Mansion US/files/Map/map2.szp"] [--out data/pipelines.bin]
        [--merge data/pipelines.bin]

GX accuracy toggles (integer TEV, EFB colour depth) need no runs of their own: Aurora derives
those variants from the recorded configs (patch 0013). MSAA only changes the render target
layout, which Aurora rebuilds the whole set for.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pipelines  # noqa: E402
import rooms  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BOOT = "300:START*10,500:A*6"  # title -> new game (LM_INPUT_CLOCK=frames)
INGAME = 2150                  # guest frame by which LM_START_MAP gameplay has started


def room_actions(t):
    """Input for one room visit starting at guest frame t (teleport at t)."""
    return [f"{t + 15}:R*45", f"{t + 65}:STICKLEFT*12", f"{t + 80}:STICKUP*10", f"{t + 92}:A*3",
            f"{t + 100}:L*20", f"{t + 125}:STICKRIGHT*12"]


def tour(points, step=150, start=INGAME + 50, gbh_every=8):
    """LM_SKIP_TEST teleports + input for a tour of the given room points."""
    tele, inp = [], []
    t = start
    for i, (_room, x, y, z) in enumerate(points):
        tele.append(f"{t}:pos:{x},{y},{z}")
        inp += room_actions(t)
        if gbh_every and i % gbh_every == gbh_every - 1:
            # Game Boy Horror: open, page through its screens, close.
            inp += [f"{t + 140}:Y*3", f"{t + 180}:RIGHT*3", f"{t + 210}:RIGHT*3", f"{t + 240}:LEFT*3",
                    f"{t + 270}:B*3", f"{t + 300}:B*3"]
            t += 180
        t += step
    return ";".join(tele), inp, t


def explore(start=INGAME, frames=1800):
    """Walk around, vacuum, blow, press A: for single-room maps and boss arenas."""
    inp = []
    moves = ["STICKUP", "STICKLEFT", "STICKDOWN", "STICKRIGHT"]
    for i, t in enumerate(range(start, start + frames, 120)):
        inp += [f"{t}:{moves[i % 4]}*40", f"{t + 45}:R*40", f"{t + 90}:A*3", f"{t + 100}:L*15"]
    return inp


def scenarios(map2: Path, only):
    pts = rooms.room_points(map2)
    out = []

    def add(name, env, inp, frames):
        if not only or any(name.startswith(o) for o in only):
            out.append((name, env, inp, frames))

    # Boot: title, menus, file select, the whole opening (not skipped), foyer.
    add("boot", {}, ["300:START*10", "500:A*6"] + [f"{t}:A*3" for t in range(1200, 3600, 100)] + explore(3600, 900),
        4700)
    add("boot_skip", {"LM_SKIP_CUTSCENES": "1", "LM_SKIP_AUTO": "30"},
        ["300:START*10", "500:A*6"] + explore(2600, 900), 3600)
    # Mansion tour, normal and with each element.
    tele, inp, end = tour(pts)
    base = {"LM_START_MAP": "2", "LM_TEST_STATE": "poltergust", "LM_SKIP_CUTSCENES": "1", "LM_SKIP_AUTO": "30"}
    add("tour", {**base, "LM_SKIP_TEST": tele}, [BOOT] + inp, end + 100)
    for el in ("fire", "water", "ice"):
        tele, inp, end = tour(pts[{"fire": 0, "water": 1, "ice": 2}[el]::3], gbh_every=0)
        add(f"tour_{el}", {**base, "LM_TEST_STATE": f"poltergust+{el}", "LM_SKIP_TEST": tele}, [BOOT] + inp, end + 100)
    # Hidden Mansion, both rule sets (PAL: mirrored mansion, other lighting).
    for v in ("us", "pal"):
        tele, inp, end = tour(pts[::2], gbh_every=0)
        add(f"hm_{v}", {**base, "LM_HM_FORCE": "1", "LM_HIDDEN_MANSION_VARIANT": v,
                         "LM_HM_OVERLAY": str(ROOT_MAIN / "orig" / "hidden_mansion"), "LM_SKIP_TEST": tele},
            [BOOT] + inp, end + 100)
    # Unused content shader variants: early ghost shading, Game Boy Horror live camera.
    tele, inp, end = tour(pts[1::3], gbh_every=4)
    add("unused", {**base, "LM_EARLY_GHOSTS": "1", "LM_GBH_CAM": "1", "LM_SKIP_TEST": tele},
        [BOOT] + inp + [f"{t}:UP*3" for t in range(INGAME + 120, end, 400)], end + 100)
    # Every other map (lab, training, Portrificationizer, unused 5/7/8, gallery, bosses, graveyard).
    for m in (1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13):
        add(f"map{m:02}", {**base, "LM_START_MAP": str(m)}, [BOOT] + explore(), INGAME + 1900)
    return out


def run(args, scenario):
    name, env, inp, frames = scenario
    d = Path(args.work) / name
    if d.exists():
        shutil.rmtree(d)
    (d / "app").mkdir(parents=True)
    (d / "card").mkdir()
    e = dict(os.environ)
    e.update({"APPDATA": str(d / "app"), "LM_CARD_DIR": str(d / "card"), "LM_BACKGROUND": "1",
              "LM_INPUT_CLOCK": "frames", "LM_PRECACHE": "0", "LM_DEV_STATUS_LOG": "300"})
    e.update(env)
    seconds = frames / 30 + 40  # boot + compile stalls
    cmd = [args.exe, "--disc", args.disc, "--input", ",".join(inp)]
    t0 = time.time()
    with open(d / "log.txt", "w", encoding="utf-8") as log:
        p = subprocess.Popen(cmd, env=e, stdout=log, stderr=subprocess.STDOUT)
        try:
            p.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    db = d / "app" / "Luigi's Mansion PC" / "pipeline_cache.db"
    n = sum(1 for _ in pipelines.read_db(db)) if db.exists() else 0
    print(f"{name}: {n} pipelines, {time.time() - t0:.0f} s", flush=True)
    return db if db.exists() else None


ROOT_MAIN = ROOT


def main():
    global ROOT_MAIN
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True, help="scratch directory (APPDATA/card per scenario)")
    ap.add_argument("--exe", default=str(ROOT / "build/windows-clang/lmrecomp.exe"))
    ap.add_argument("--data-root", default=str(ROOT),
                    help="checkout holding orig/ and 'Luigis Mansion US/' (game data)")
    ap.add_argument("--disc")
    ap.add_argument("--map2")
    ap.add_argument("--out", default=str(ROOT / "data/pipelines.bin"))
    ap.add_argument("--merge", nargs="*", default=[], help="existing lists/databases to include")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", default="", help="comma-separated scenario name prefixes")
    a = ap.parse_args()
    ROOT_MAIN = Path(a.data_root)
    a.disc = a.disc or str(ROOT_MAIN / "orig/GLME01.iso")
    map2 = Path(a.map2 or ROOT_MAIN / "Luigis Mansion US/files/Map/map2.szp")
    only = [o for o in a.only.split(",") if o]
    sc = scenarios(map2, only)
    print(f"{len(sc)} scenarios, {a.jobs} at a time", flush=True)
    with ThreadPoolExecutor(a.jobs) as ex:
        dbs = [db for db in ex.map(lambda s: run(a, s), sc) if db]
    pipelines.build([*a.merge, *map(str, dbs)], Path(a.out))


if __name__ == "__main__":
    main()
