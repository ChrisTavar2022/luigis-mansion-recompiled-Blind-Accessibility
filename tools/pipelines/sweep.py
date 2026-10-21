#!/usr/bin/env python3
"""Automated sweep that records the render pipelines the game uses (docs/RENDERING.md,
"Pipeline precache"), then writes data/pipelines.bin.

Runs the dev build (lmrecomp) through a set of scripted scenarios: boot/title/menus/opening,
a teleport tour of every mansion room (points from the map's furniture table, rooms.py) with
the Poltergust, flashlight, Game Boy Horror and elements, every other map, the Hidden Mansion
(USA and PAL rules, mirrored), the unused-content shader variants, Enhanced lighting (ghost
lights, world lights, colored-light shadow maps and screen-space shadows, bloom, volumetric
beam, AO), long boss fights, the mansion's event entry points, the ending, the 2-player split
screen and the model viewer. Every scenario gets its own scratch APPDATA (Aurora's
pipeline_cache.db records each config the moment it is first used; F1 options come from a
settings.ini written there), so scenarios run in parallel; the databases are merged with
pipelines.py.

    python tools/pipelines/sweep.py --work <scratch dir> [--jobs 4] [--only tour,maps]
        [--exe build/windows-clang/lmrecomp.exe] [--disc orig/GLME01.iso]
        [--map2 "Luigis Mansion US/files/Map/map2.szp"] [--out data/pipelines.bin]
        [--merge data/pipelines.bin] [--resume]

Runs that crash on their own are repeated (up to 3 times); --resume keeps the scenarios a
previous sweep into the same --work directory finished.

The European game: the same with --exe <PAL build>/lmrecomp.exe --disc orig/GLMP01.iso
(--only selects scenario name prefixes; `--list` prints the names).

GX accuracy toggles (integer TEV, EFB colour depth) need no runs of their own: Aurora derives
those variants from the recorded configs (patch 0013). Since Aurora 0217 the same holds for the
Enhanced-lighting variants (shader classes, glow, depth pre-pass, shadow casters, beam cone) and
the early-Z split: a sweep only needs to reach each material once, with any settings; configs
are canonical (export the known set with LM_PIPELINE_EXPORT to get a deduplicated list), and
tools/pipelines/verify.py measures what a fresh cache still misses. MSAA only changes the render target
layout, which Aurora rebuilds the whole set for. The CRT filter and Enhanced lighting's
scene-end passes (bloom, volumetric beam, AO, cube shadow-map clears) are their own pipelines
outside the cache; Enhanced lighting's GX-derived draws (enhanced shader classes, glow draws,
the depth pre-pass, light shadow-map draws) are cached and swept here.
"""
import argparse
import os
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pipelines  # noqa: E402
import rooms  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BOOT = "300:START*10,500:A*6"  # title -> new game (LM_INPUT_CLOCK=frames)
INGAME = 2150                  # guest frame by which LM_START_MAP gameplay has started

# ---- F1 options (settings.ini sections) ------------------------------------------------------

# Enhanced lighting with every part on at its highest quality, ghost lights and world lights,
# colored-light shadows as cube shadow maps.
ENH = {
    "enhanced_lighting": {
        "enabled": 1, "preset": 4, "per_pixel_lighting": 1, "flashlight_quality": 1, "soft_shadows": 1,
        "soft_shadow_quality": 3, "shadow_map_size": 1, "extra_lights": 1, "extra_light_shadows": 1,
        "extra_shadow_quality": 2, "extra_shadow_mode": 1, "extra_shadow_lights": 3, "extra_shadow_map_size": 1,
        "volumetric_beam": 1, "volumetric_quality": 2, "bloom": 1, "ssao": 1,
    },
    "ghost_lights": {"enabled": 1, "shadows": 1, "dramatic": 1, "invisible_hint": 0.3},
    "world_lights": {"enabled": 1},
}
# The cheaper variants: screen-space colored-light shadows, low quality, shadow map at render size.
ENH_LOW = {
    "enhanced_lighting": {
        "enabled": 1, "preset": 4, "per_pixel_lighting": 1, "flashlight_quality": 1, "soft_shadows": 1,
        "soft_shadow_quality": 0, "shadow_map_size": 0, "extra_lights": 1, "extra_light_shadows": 1,
        "extra_shadow_quality": 0, "extra_shadow_mode": 0, "extra_shadow_lights": 1,
        "volumetric_beam": 1, "volumetric_quality": 0, "bloom": 1, "ssao": 0,
    },
    "ghost_lights": {"enabled": 1, "shadows": 1},
    "world_lights": {"enabled": 1},
}
# Ghosts die quickly and Luigi does not: captures, capture bursts and boss phases are reached.
STRONG = {"gameplay": {"damage_scale": 0, "ghost_health_scale": 0.25, "vacuum_strength": 4}}
# Unused content (F1 > Enhancements > Unused content / Debug) and the CRT filter.
UNUSED_A = {
    "enhancements": {k: 1 for k in (
        "gbh_camera", "gbh_tracker", "gbh_clock", "early_ghost_lighting", "beta_skybox", "heart_door",
        "star_door", "beta_map_luigi", "early_luigi", "air_water_nozzles", "early_poltergust", "green_toad")},
    "unused2": {k: 1 for k in (
        "bordered_view", "poison_state", "crawl_fear", "overheat", "ghost_placements", "pink_mice",
        "blue_ghost_guys", "extra_speedy_spirits")},
    "debug": {"depth_blur": 1, "particle_report": 1, "info_display": 1, "scan_boxes": 1, "axes": 1},
    "crt": {"enabled": 1, "preset": 0, "scanlines": 0},
}
UNUSED_B = {
    "enhancements": {"early_ghost_lighting": 1, "air_water_nozzles": 1, "gbh_camera": 1},
    "unused2": {"play_as_mario": 1, "ghost_placements": 1},
    "crt": {"enabled": 1, "preset": 3, "scanlines": 2},
}


def merge(*dicts):
    out = {}
    for d in dicts:
        for sec, kv in d.items():
            out.setdefault(sec, {}).update(kv)
    return out


def room_actions(t, port=""):
    """Input for one room visit starting at guest frame t (teleport at t)."""
    return [f"{port}{t + 15}:R*45", f"{port}{t + 65}:STICKLEFT*12", f"{port}{t + 80}:STICKUP*10",
            f"{port}{t + 92}:A*3", f"{port}{t + 100}:L*20", f"{port}{t + 125}:STICKRIGHT*12"]


def tour(points, step=150, start=INGAME + 50, gbh_every=8, p2=False):
    """LM_SKIP_TEST teleports + input for a tour of the given room points."""
    tele, inp = [], []
    t = start
    for i, (_room, x, y, z) in enumerate(points):
        tele.append(f"{t}:pos:{x},{y},{z}")
        inp += room_actions(t)
        if p2:
            inp += room_actions(t + 20, "P2:")
        if gbh_every and i % gbh_every == gbh_every - 1:
            # Game Boy Horror: open, page through its screens, close.
            inp += [f"{t + 140}:Y*3", f"{t + 180}:RIGHT*3", f"{t + 210}:RIGHT*3", f"{t + 240}:LEFT*3",
                    f"{t + 270}:B*3", f"{t + 300}:B*3"]
            t += 180
        t += step
    return ";".join(tele), inp, t


def explore(start=INGAME, frames=1800, port=""):
    """Walk around, vacuum, blow, press A: for single-room maps and boss arenas."""
    inp = []
    moves = ["STICKUP", "STICKLEFT", "STICKDOWN", "STICKRIGHT"]
    for i, t in enumerate(range(start, start + frames, 120)):
        inp += [f"{port}{t}:{moves[i % 4]}*40", f"{port}{t + 45}:R*40", f"{port}{t + 90}:A*3",
                f"{port}{t + 100}:L*15"]
    return inp


def dpad(start, end, every=400):
    """D-pad presses for the unused GBH camera / tracker / clock toggles."""
    out = []
    for i, t in enumerate(range(start, end, every)):
        out.append(f"{t}:{('UP', 'DOWN', 'LEFT')[i % 3]}*3")
    return out


def scenarios(map2: Path, only):
    pts = rooms.room_points(map2)
    out = []

    def add(name, env, inp, frames, ini=None, seconds=None):
        if not only or any(name.startswith(o) for o in only):
            out.append((name, env, inp, frames, ini or {}, seconds))

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

    # ---- Enhanced lighting (F1 > Visuals > Enhanced lighting, all parts; ghost lights) ----
    tele, inp, end = tour(pts, gbh_every=10)
    add("enh_tour", {**base, "LM_SKIP_TEST": tele}, [BOOT] + inp, end + 100, merge(ENH, STRONG))
    for i, el in enumerate(("fire", "water", "ice")):
        tele, inp, end = tour(pts[i::3], gbh_every=0)
        add(f"enh_{el}", {**base, "LM_TEST_STATE": f"poltergust+{el}", "LM_SKIP_TEST": tele}, [BOOT] + inp,
            end + 100, merge(ENH_LOW if el == "water" else ENH, STRONG))
    tele, inp, end = tour(pts[1::2], gbh_every=0)
    add("enh_lowq", {**base, "LM_SKIP_TEST": tele}, [BOOT] + inp, end + 100, merge(ENH_LOW, STRONG))
    for v in ("us", "pal"):
        tele, inp, end = tour(pts[1::2] if v == "us" else pts[::2], gbh_every=0)
        add(f"enh_hm_{v}", {**base, "LM_HM_FORCE": "1", "LM_HIDDEN_MANSION_VARIANT": v,
                             "LM_HM_OVERLAY": str(ROOT_MAIN / "orig" / "hidden_mansion"), "LM_SKIP_TEST": tele},
            [BOOT] + inp, end + 100, merge(ENH, STRONG))
    add("enh_boot", {}, ["300:START*10", "500:A*6"] + [f"{t}:A*3" for t in range(1200, 3600, 100)]
        + explore(3600, 900), 4700, ENH)
    for m in (0, 1, 3, 4, 5, 6, 7, 8, 12):
        add(f"enh_map{m:02}", {**base, "LM_START_MAP": str(m)}, [BOOT] + explore(), INGAME + 1900,
            merge(ENH, STRONG))

    # ---- Boss fights, played longer (strong vacuum, no damage): plain and with Enhanced lighting ----
    # 9 roof (Bowser), 10 Chauncey, 11 balcony (Boolossus: ice pops the Boos), 13 graveyard (Bogmire).
    for m, state in ((9, "poltergust"), (10, "poltergust"), (11, "poltergust+ice"), (13, "poltergust")):
        for tag, ini in (("", STRONG), ("enh_", merge(ENH, STRONG))):
            add(f"{tag}boss{m:02}", {**base, "LM_START_MAP": str(m), "LM_TEST_STATE": state},
                [BOOT] + explore(INGAME, 6600), INGAME + 6700, ini)
    # Training room (map 3): A on the controller settings screen and through E. Gadd's text, then
    # the training ghosts are vacuumed.
    for tag, ini in (("", STRONG), ("enh_", merge(ENH, STRONG))):
        add(f"{tag}training", {**base, "LM_START_MAP": "3"},
            [BOOT] + [f"{t}:A*3" for t in range(INGAME, INGAME + 1800, 40)] + explore(INGAME + 1800, 4200),
            INGAME + 6100, ini)
    # Mansion entry points whose arrival plays an event: 9 Secret Altar (King Boo), 7 Sealed Room,
    # 5 Balcony (Boos), 3 roof.
    for p in (9, 7, 5, 3):
        add(f"appear{p}", {**base, "LM_START_APPEAR": str(p)}, [BOOT] + explore(INGAME, 3000), INGAME + 3100,
            merge(STRONG, {"debug": {"warp_skip_events": 0}}))

    # ---- Ending (rank screen, credits, unused ending pictures) ----
    for rank, ini in ((8, {"unused2": {"ending_pictures": 1}}), (1, {})):
        add(f"ending_r{rank}", {"LM_TEST_ENDING": f"{rank}@3700"},
            ["300:START*10", "500:A*6"] + [f"{t}:A*3" for t in range(1200, 3600, 100)]
            + [f"{t}:A*3" for t in range(4300, 12000, 240)], 12000, ini)

    # ---- Unused content: every toggle, the CRT filter on top ----
    tele, inp, end = tour(pts[0::3], gbh_every=4)
    add("unused_a", {**base, "LM_TEST_STATE": "poltergust+water", "LM_SKIP_TEST": tele},
        [BOOT] + inp + dpad(INGAME + 120, end), end + 100, merge(UNUSED_A, STRONG))
    tele, inp, end = tour(pts[2::3], gbh_every=4)
    add("unused_b", {**base, "LM_TEST_STATE": "poltergust+fire", "LM_SKIP_TEST": tele},
        [BOOT] + inp + dpad(INGAME + 120, end), end + 100, merge(UNUSED_B, STRONG))
    add("unused_bogmire", {**base, "LM_START_MAP": "13"}, [BOOT] + explore(INGAME, 3600), INGAME + 3700,
        merge({"enhancements": {"early_bogmire": 1}}, STRONG))
    add("unused_portraits", {**base, "LM_START_MAP": "4"}, [BOOT] + explore(), INGAME + 1900,
        {"enhancements": {"unused_portrait_models": 1}})
    add("unused_enh", {**base, "LM_TEST_STATE": "poltergust+ice", "LM_SKIP_TEST": tour(pts[1::4])[0]},
        [BOOT] + tour(pts[1::4])[1] + dpad(INGAME + 120, tour(pts[1::4])[2]), tour(pts[1::4])[2] + 100,
        merge(UNUSED_A, ENH, STRONG))

    # ---- 2-player split screen (unused 2-player mode + co-op improvements; Mario as P2) ----
    coop = {**base, "LM_COOP": "1", "LM_COOP_FIXES": "1"}
    tele, inp, end = tour(pts[::3], gbh_every=0, p2=True)
    add("coop", {**coop, "LM_SKIP_TEST": tele}, [BOOT] + inp, end + 100, STRONG)
    tele, inp, end = tour(pts[1::3], gbh_every=0, p2=True)
    add("coop_mario", {**coop, "LM_MARIO_P2": "1", "LM_SKIP_TEST": tele}, [BOOT] + inp, end + 100,
        merge(ENH, STRONG, {"unused2": {"mario_p2": 1}}))
    for m in (10, 13):
        add(f"coop_map{m:02}", {**coop, "LM_START_MAP": str(m)},
            [BOOT] + explore(INGAME, 3000) + explore(INGAME + 30, 3000, "P2:"), INGAME + 3100, STRONG)

    # ---- Model viewer (F4): every disc model, ghost effects; whole maps ----
    add("viewer", {"LM_DEV_VIEWER_SWEEP": "@6", "LM_DEV_VIEWER_SWEEP_EXIT": "1"}, ["300:START*10"], 0,
        seconds=2400)
    for m in (2, 9, 10, 11, 13):
        add(f"viewer_map{m:02}", {"LM_DEV_VIEWER_MAP": str(m)}, ["300:START*10"], 1500)
    return out


def write_ini(path: Path, ini):
    lines = []
    for sec, kv in ini.items():
        lines.append(f"[{sec}]")
        for k, v in kv.items():
            lines.append(f"{k}={'true' if v is True else v}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run(args, scenario):
    d = Path(args.work) / scenario[0]
    db = d / "app" / "Luigi's Mansion Recompiled" / "pipeline_cache.db"
    if args.resume and (d / "done").exists() and db.exists():
        print(f"{scenario[0]}: kept from an earlier sweep", flush=True)
        return db
    # A run that crashes on its own (e.g. out of memory with many instances) is repeated.
    for attempt in range(3):
        db, code, killed = run_once(args, scenario)
        # Exit 0 before the time limit: only the model viewer sweep ends by itself.
        if db and (killed or (code == 0 and scenario[5])):
            (d / "done").write_text("", encoding="utf-8")
            break
        print(f"{scenario[0]}: exited with {code}, attempt {attempt + 1}", flush=True)
        time.sleep(10)
    return db


def run_once(args, scenario):
    name, env, inp, frames, ini, seconds = scenario
    d = Path(args.work) / name
    if d.exists():
        shutil.rmtree(d)
    cfg = d / "app" / "Luigi's Mansion Recompiled"
    cfg.mkdir(parents=True)
    (d / "card").mkdir()
    if ini:
        write_ini(cfg / "settings.ini", ini)
    e = dict(os.environ)
    e.update({"APPDATA": str(d / "app"), "LM_CARD_DIR": str(d / "card"), "LM_BACKGROUND": "1",
              "LM_INPUT_CLOCK": "frames", "LM_PRECACHE": "0", "LM_DEV_STATUS_LOG": "300"})
    e.update(env)
    seconds = seconds or frames / 30 * 1.3 + 60  # boot + compile stalls + slower frames when busy
    cmd = [args.exe, "--disc", args.disc, "--input", ",".join(inp)]
    killed = False
    with open(d / "log.txt", "w", encoding="utf-8") as log:
        # Staggered starts: several instances creating their GPU devices and caches in the
        # same moment sometimes failed (abort, or the cache databases failed to open).
        with START_LOCK:
            global LAST_START
            time.sleep(max(0.0, LAST_START + 8 - time.time()))
            LAST_START = time.time()
        t0 = time.time()
        # No console window per run (closing one would end that run).
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        p = subprocess.Popen(cmd, env=e, stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
        try:
            p.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            p.kill()  # only the process this sweep started
            p.wait()
            killed = True
    db = cfg / "pipeline_cache.db"
    if "SQLite DB init failed" in (d / "log.txt").read_text(encoding="utf-8", errors="replace"):
        db = d / "missing"  # caches not recorded: run again
    n = sum(1 for _ in pipelines.read_db(db)) if db.exists() else 0
    print(f"{name}: {n} pipelines, {time.time() - t0:.0f} s, exit {'timeout' if killed else p.returncode}",
          flush=True)
    return (db if db.exists() else None), p.returncode, killed


ROOT_MAIN = ROOT
START_LOCK = threading.Lock()
LAST_START = 0.0


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
    ap.add_argument("--list", action="store_true", help="print the scenario names and exit")
    ap.add_argument("--resume", action="store_true", help="keep scenarios an earlier sweep in --work finished")
    a = ap.parse_args()
    ROOT_MAIN = Path(a.data_root)
    a.disc = a.disc or str(ROOT_MAIN / "orig/GLME01.iso")
    map2 = Path(a.map2 or ROOT_MAIN / "Luigis Mansion US/files/Map/map2.szp")
    only = [o for o in a.only.split(",") if o]
    sc = scenarios(map2, only)
    if a.list:
        for s in sc:
            print(s[0], s[3])
        return
    print(f"{len(sc)} scenarios, {a.jobs} at a time", flush=True)
    # Longest first, so the pool is not left waiting on one long run at the end.
    order = sorted(sc, key=lambda s: -(s[5] or s[3] / 30))
    with ThreadPoolExecutor(a.jobs) as ex:
        dbs = [db for db in ex.map(lambda s: run(a, s), order) if db]
    pipelines.build([*a.merge, *map(str, dbs)], Path(a.out))


if __name__ == "__main__":
    main()
