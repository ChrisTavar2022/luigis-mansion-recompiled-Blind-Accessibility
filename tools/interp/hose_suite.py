"""Heavy-movement suite for Luigi's hose and nozzle (docs/INTERPOLATION.md "The hose nozzle").

    hose_suite.py <out dir> [--scenario NAME ...] [--fps 120 ...] [--secs 180] [--user-settings]
                  [--set section.key=value ...] [--env K=V ...] [--exe PATH] [--disc ISO] [--window WxH]

Runs each scenario at each frame rate (one game at a time; hidden window off screen, muted, below-normal
priority, scratch APPDATA / LM_CARD_DIR under <out dir>/<scenario>_<fps>), with the path check
(LM_TRACK_LOG + LM_JUDDER_LOG, every in-between judged), from the scenario's start frame for --secs
seconds, then prints tools/interp/hose_check.py's summary and a table of all runs.

Scenarios (US disc; game frames from 2600 on, scripted with LM_INPUT_CLOCK=frames):
  run      nursery: running in circles, sharp reversals, strafing with the vacuum on and off, vacuum
           taps, nozzle aiming with the C stick (R held and not), flashlight on/off, idle spells
  foyer    the same mix in the foyer (front door, stairs, long runs up and down: room changes at
           the doors it reaches)
  fire / water / ice   the run mix with that element (LM_TEST_STATE=poltergust+<element>): L bursts
           and taps while turning
  hit      the run mix, Luigi hit every 150 game frames (LM_TEST_HIT: hurt reaction, knock-back)
  ghosts   gold ghosts room: LM_GHOST_HUNT stuns and vacuums the ghosts (Luigi dragged around by
           them), the scripted mix on top
  boss     Boolossus' arena (map 11), the run mix (one quick check)
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import screen_run  # noqa: E402

MENU = screen_run.MENU
DIRS = ["STICKUP", "STICKUP+STICKRIGHT", "STICKRIGHT", "STICKDOWN+STICKRIGHT", "STICKDOWN", "STICKDOWN+STICKLEFT", "STICKLEFT",
        "STICKUP+STICKLEFT"]


def heavy_mix(first, last, element=False, long_runs=False):
    """A cycle of hard movements from game frame `first` to `last` (input items "frame:BUTTONS*n")."""
    out = []
    f = first

    def add(buttons, n, gap=0):
        nonlocal f
        out.append(f"{f}:{buttons}*{n}")
        f += n + gap

    cycle = 0
    while f < last:
        # running in circles, both ways, fast (4 frames per eighth of a turn)
        for lap in range(3):
            for k in range(8):
                add(DIRS[(k if cycle % 2 == 0 else -k) % 8], 4)
        # sharp reversals: left/right and up/down
        for k in range(6):
            add("STICKLEFT" if k % 2 == 0 else "STICKRIGHT", 7)
        for k in range(4):
            add("STICKUP" if k % 2 == 0 else "STICKDOWN", 9)
        # strafing with the vacuum on (Luigi keeps facing while it runs), reversals, then off
        for k in range(6):
            add("R+" + ("STICKLEFT" if k % 2 == 0 else "STICKRIGHT"), 12)
        for k in range(4):
            add("R+" + DIRS[(2 * k + 1) % 8], 10)
        # vacuum taps while running in circles
        for k in range(8):
            add("R+" + DIRS[k], 3)
            add(DIRS[(k + 1) % 8], 3)
        # nozzle aiming: C stick sweeps with the vacuum on, then without
        for c in ("CUP", "CLEFT", "CDOWN", "CRIGHT", "CLEFT", "CRIGHT"):
            add("R+" + c, 8)
        for c in ("CLEFT", "CRIGHT", "CUP", "CDOWN"):
            add(c, 6)
        # running with the vacuum on and the C stick against the run
        for k in range(8):
            add("R+" + DIRS[k] + "+" + ("CLEFT" if k % 2 else "CRIGHT"), 6)
        # flashlight off and on while running
        for k in range(4):
            add("A+" + DIRS[(4 * k) % 8], 2)
            add(DIRS[(4 * k) % 8], 12)
        if element:  # element bursts and taps while turning
            for k in range(6):
                add("L+" + DIRS[(3 * k) % 8], 14)
            for k in range(8):
                add("L", 3)
                add(DIRS[(5 * k) % 8], 3)
            add("L+R", 10)
        if long_runs:  # long straight runs (stairs, doors): up and back
            add("STICKUP", 90)
            add("STICKLEFT", 30)
            add("STICKDOWN", 90)
            add("STICKRIGHT", 30)
        # standing still: idle (and scared) animations, then a quick start
        f += 75
        add(DIRS[cycle % 8], 20)
        cycle += 1
    return ",".join(out)


def scenario(name):
    env = {"LM_INPUT_CLOCK": "frames", "LM_START_MAP": "2", "LM_START_APPEAR": "1", "LM_TEST_STATE": "poltergust"}
    start, end = 2600, 2600 + 30 * 60 * 20
    if name == "run":
        inp = heavy_mix(start, end)
    elif name == "foyer":
        env["LM_START_APPEAR"] = "6"
        inp = heavy_mix(start, end, long_runs=True)
    elif name in ("fire", "water", "ice"):
        env["LM_TEST_STATE"] = "poltergust+" + name
        inp = heavy_mix(start, end, element=True)
    elif name == "hit":
        env["LM_TEST_HIT"] = "2650,150,1"
        inp = heavy_mix(start, end)
    elif name == "ghosts":
        env.update({"LM_TELEPORT": "0,550,-3300,100", "LM_GHOST_HUNT": "9", "LM_GHOST_HUNT_BEAM": "89",
                    "LM_GHOST_HUNT_DMAX": "300", "LM_GHOST_HUNT_R": "0"})
        start = 2700
        # the hunt drives the stick and R while it has a target; in between, the mix
        inp = heavy_mix(start, end)
    elif name == "boss":
        env["LM_START_MAP"] = "11"
        env.pop("LM_START_APPEAR", None)
        start = 3200
        inp = heavy_mix(start, end)
    else:
        raise SystemExit(f"unknown scenario {name}")
    return env, MENU + "," + inp, start


def find_disc():
    d = ROOT
    for _ in range(5):
        p = os.path.join(d, "orig", "GLME01.iso")
        if os.path.exists(p):
            return p
        d = os.path.dirname(d)
    return os.path.join(ROOT, "orig", "GLME01.iso")


def last_frame(path):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            n = f.tell()
            f.seek(max(0, n - 200000))
            txt = f.read().decode("utf-8", "replace")
    except OSError:
        return 0
    best = 0
    for line in txt.splitlines():
        if line.startswith("HT "):
            try:
                best = max(best, int(line.split()[2][1:]))
            except (ValueError, IndexError):
                pass
    return best


def run_one(a, name, fps, out):
    if os.path.exists(out):
        shutil.rmtree(out, ignore_errors=True)
    appdata = os.path.join(out, "appdata")
    lmdir = os.path.join(appdata, "Luigi's Mansion Recompiled")
    os.makedirs(lmdir, exist_ok=True)
    os.makedirs(os.path.join(out, "card"), exist_ok=True)
    user_lm = os.path.join(os.environ.get("APPDATA", ""), "Luigi's Mansion Recompiled")
    sets = [f"video.target_fps={fps}", "video.match_display_refresh=false", "video.fullscreen=false"] + a.set
    src = os.path.join(user_lm, "settings.ini") if a.user_settings else None
    screen_run.write_settings(src, os.path.join(lmdir, "settings.ini"), sets)
    for f in ("dawn_cache.db", "pipeline_cache.db"):
        p = os.path.join(user_lm, f)
        if os.path.exists(p):
            shutil.copyfile(p, os.path.join(lmdir, f))
    senv, inp, start = scenario(name)
    env = dict(os.environ)
    env.update(senv)
    track = os.path.join(out, "track.log")
    env.update({"APPDATA": appdata, "LM_CARD_DIR": os.path.join(out, "card"), "LM_AUDIO": "0", "LM_BACKGROUND": "1",
                "LM_OFFSCREEN": "1", "LM_JUDDER_LOG": os.path.join(out, "judder.log"), "LM_JUDDER_EVERY": "1",
                "LM_JUDDER_PX": "100000", "LM_TRACK_LOG": f"{track},{a.radius}"})
    for e in a.env:
        k, v = e.split("=", 1)
        env[k] = v
    inp_file = os.path.join(out, "input.txt")
    open(inp_file, "w", encoding="utf-8").write(inp)
    args = [a.exe, "--disc", a.disc, "--input", "@" + inp_file]
    if a.window:
        args += ["--window", a.window]
    log = open(os.path.join(out, "log.txt"), "w", encoding="utf-8")
    game = subprocess.Popen(args, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
    print(f"[{name} {fps}] game pid {game.pid}", flush=True)
    ok = False
    try:
        deadline = time.time() + 900
        while time.time() < deadline and game.poll() is None:
            time.sleep(2)
            if last_frame(track) >= start:
                ok = True
                break
        if ok:
            end = time.time() + a.secs
            while time.time() < end and game.poll() is None:
                time.sleep(2)
        else:
            print(f"[{name} {fps}] start frame {start} not reached", flush=True)
    finally:
        if game.poll() is None:
            game.kill()  # (the process this script started)
        try:
            game.wait(timeout=20)
        except subprocess.TimeoutExpired:
            pass
        log.close()
    r = subprocess.run([sys.executable, os.path.join(HERE, "hose_check.py"), track, "--skip-tapes", "30"], capture_output=True, text=True)
    open(os.path.join(out, "summary.txt"), "w", encoding="utf-8").write(r.stdout)
    print(r.stdout, flush=True)
    return r.stdout.splitlines()[0] if r.stdout else "no output"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--scenario", action="append", default=[])
    ap.add_argument("--fps", action="append", type=int, default=[])
    ap.add_argument("--secs", type=float, default=180)
    ap.add_argument("--user-settings", action="store_true", help="start from the user's settings.ini (read only)")
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "windows-clang", "lmrecomp.exe"))
    ap.add_argument("--disc", default=find_disc())
    ap.add_argument("--window", default="1280x720")
    ap.add_argument("--radius", type=float, default=300, help="judge vertices this close to Luigi (1e9: the whole scene)")
    a = ap.parse_args()
    rows = []
    for name in a.scenario or ["run"]:
        for fps in a.fps or [120]:
            rows.append((name, fps, run_one(a, name, fps, os.path.join(os.path.abspath(a.out), f"{name}_{fps}"))))
    print("\n== suite")
    for name, fps, line in rows:
        print(f"{name:8} {fps:4}  {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
