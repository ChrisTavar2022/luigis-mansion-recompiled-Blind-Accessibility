"""Frame-rate lock test (docs/INTERPOLATION.md, "Pass 5: locked frame rates").

    pacing_run.py <out dir> [--scenario foyer] [--set section.key=value ...] [--fullscreen | --window WxH]
                  [--secs S] [--exe PATH] [--env K=V ...] [--lock FILE]

Runs the game (muted, scratch APPDATA / LM_CARD_DIR under <out dir>, a copy of the user's settings.ini
plus --set overrides; scenarios from screen_run.py) with LM_PACING_LOG, waits for the scenario's
start frame, lets it run --secs seconds, stops the process it started and prints
tools/interp/pacing_stats.py's summary of the log.
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


def scenario(name):
    """screen_run.py's scenarios plus:
    menus  foyer: strafing, then the pause screen (START), the Game Boy Horror (Z) and back, repeated:
           the game runs its menus at 60 Hz and gameplay at 30 Hz
    mixed  foyer: strafing, vacuuming, turning and menus in turn for a long session"""
    if name not in ("menus", "mixed"):
        return screen_run.scenario(name)
    env, inp, start = screen_run.scenario("foyer")
    menu = screen_run.MENU
    parts = []
    f = 2600
    for i in range(400):
        k = i % 8 if name == "mixed" else (i % 2) * 7
        if k in (0, 2, 4):
            parts.append(screen_run.strafe(f, 4))
            f += 180
        elif k == 1:
            for j in range(4):
                parts.append(f"{f}:R+CUP+{['STICKLEFT', 'STICKUP', 'STICKRIGHT', 'STICKDOWN'][j]}*40")
                f += 42
        elif k == 3:
            for j in range(6):
                parts.append(f"{f}:{['STICKUP+STICKLEFT', 'STICKLEFT', 'STICKDOWN+STICKLEFT', 'STICKDOWN', 'STICKRIGHT', 'STICKUP+STICKRIGHT'][j]}*28")
                f += 30
        elif k in (5, 7):
            # pause screen: open, stay (60 Hz menu logic: guest frames come twice as fast), close
            parts.append(f"{f}:START*4")
            f += 240
            parts.append(f"{f}:START*4")
            f += 120
            # Game Boy Horror
            parts.append(f"{f}:Z*4")
            f += 240
            parts.append(f"{f}:B*4")
            f += 60
            parts.append(f"{f}:Z*4")
            f += 120
        else:
            parts.append(screen_run.strafe(f, 2))
            f += 90
    return env, menu + "," + ",".join(parts), start


def acquire(path, tag):
    while True:
        if not screen_run.others_running():
            try:
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, tag.encode())
                os.close(fd)
                if not screen_run.others_running():
                    return path
                os.remove(path)
            except FileExistsError:
                pass
        print("waiting for the game lock / other game processes", flush=True)
        time.sleep(20)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--scenario", default="foyer")
    ap.add_argument("--settings", default=os.path.join(os.environ.get("APPDATA", ""), "Luigi's Mansion Recompiled", "settings.ini"))
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--fullscreen", action="store_true")
    ap.add_argument("--window", default="")
    ap.add_argument("--secs", type=float, default=60)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "windows-clang", "lmrecomp.exe"))
    ap.add_argument("--disc", default=os.path.join(ROOT, "orig", "GLME01.iso"))
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--input", default="", help="replaces the scenario's input script")
    ap.add_argument("--lock", default=os.environ.get("LM_GAME_LOCK", ""))
    ap.add_argument("--tag", default="fps-lock")
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    if os.path.exists(out):
        shutil.rmtree(out, ignore_errors=True)
    appdata = os.path.join(out, "appdata")
    lmdir = os.path.join(appdata, "Luigi's Mansion Recompiled")
    os.makedirs(lmdir, exist_ok=True)
    os.makedirs(os.path.join(out, "card"), exist_ok=True)
    screen_run.write_settings(a.settings, os.path.join(lmdir, "settings.ini"), a.set)
    user_lm = os.path.join(os.environ.get("APPDATA", ""), "Luigi's Mansion Recompiled")
    for f in ("dawn_cache.db", "pipeline_cache.db"):
        p = os.path.join(user_lm, f)
        if os.path.exists(p):
            shutil.copyfile(p, os.path.join(lmdir, f))
    senv, inp, start = scenario(a.scenario)
    start = a.start or start
    env = dict(os.environ)
    env.update(senv)
    plog = os.path.join(out, "pacing.log")
    env.update({"APPDATA": appdata, "LM_CARD_DIR": os.path.join(out, "card"), "LM_AUDIO": "0", "LM_BACKGROUND": "1",
                "LM_TOPMOST": "1", "LM_PACING_LOG": plog})
    for e in a.env:
        k, v = e.split("=", 1)
        env[k] = v
    args = [a.exe, "--disc", a.disc, "--input", a.input or inp]
    if a.fullscreen:
        args.append("--fullscreen")
    if a.window:
        args += ["--window", a.window]
    lock = acquire(a.lock, a.tag) if a.lock else None
    t_start = None
    try:
        log = open(os.path.join(out, "log.txt"), "w", encoding="utf-8")
        game = subprocess.Popen(args, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        t_launch = time.time()
        print(f"game pid {game.pid}", flush=True)
        try:
            deadline = time.time() + 600
            reached = False
            while time.time() < deadline and game.poll() is None:
                time.sleep(0.5)
                try:
                    txt = open(os.path.join(out, "log.txt"), encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                idx = txt.rfind("[frame] ")
                if idx >= 0:
                    try:
                        n = int(txt[idx + 8:].split()[0])
                    except ValueError:
                        n = 0
                    if n >= start:
                        reached = True
                        break
            if not reached:
                print("start frame not reached")
                return 1
            t_start = time.time()
            print(f"frame {start} reached, running {a.secs} s", flush=True)
            end = time.time() + a.secs
            while time.time() < end and game.poll() is None:
                time.sleep(1)
        finally:
            if game.poll() is None:
                game.kill()
            try:
                game.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
            log.close()
    finally:
        if lock:
            try:
                os.remove(lock)
            except OSError:
                pass
    if t_start:
        subprocess.run([sys.executable, os.path.join(HERE, "pacing_stats.py"), plog, "--skip", f"{t_start - t_launch:.1f}"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
