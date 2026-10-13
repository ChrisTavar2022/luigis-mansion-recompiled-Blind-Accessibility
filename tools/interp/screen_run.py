"""Screen-capture judder test under real conditions (docs/INTERPOLATION.md, "Screen capture").

    screen_run.py <out dir> [--scenario foyer] [--settings INI] [--set section.key=value ...]
                  [--fullscreen | --window WxH] [--secs S] [--start FRAME] [--exe PATH]
                  [--keep-video] [--no-capture] [--env K=V ...]

Runs the game (build/windows-clang/lmrecomp.exe) visibly on the primary display with a scratch
APPDATA / LM_CARD_DIR under <out dir>, muted, with the settings file given (default: a copy of
the user's %APPDATA%\\Luigi's Mansion PC\\settings.ini, read only) plus --set overrides. When the
scenario's frame is reached it records the display with ffmpeg ddagrab (Desktop Duplication,
dup_frames=0: one captured frame per screen update, timestamped on arrival) for --secs seconds,
stops the game (only the process it started) and prints tools/interp/screen_judder.py's metrics
and the game's pacing log summary (tools/interp/judder.py).

Scenarios (US disc, LM_INPUT_CLOCK=frames):
  foyer    foyer by the front door: strafing left and right for a minute (camera follows:
           candlesticks, door, stairs, dust)
  turn     foyer: walking in circles / turning (C-stick and stick)
  nursery  nursery: walking, dust, spiderwebs
  ghosts   gold ghosts room: ghost fight (LM_GHOST_HUNT)
  vacuum   foyer: the vacuum running with the nozzle down while walking: heavy dust
  ballroom the ballroom (LM_TELEPORT), strafing
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FFMPEG = r"C:\Users\rmelv\AppData\Roaming\Python\Python314\site-packages\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v7.1.exe"
MENU = "300:START*10,450:START*10,600:A*6,750:A,900:A,1050:A,1200:A,1350:A,1500:A,1650:A"


def strafe(first, n, period=90, extra=""):
    s = []
    f = first
    for i in range(n):
        s.append(f"{f}:STICK{'LEFT' if i % 2 == 0 else 'RIGHT'}{extra}*{period // 2 - 2}")
        f += period // 2
    return ",".join(s)


def scenario(name):
    env = {"LM_INPUT_CLOCK": "frames", "LM_START_MAP": "2", "LM_START_APPEAR": "1",
           "LM_TEST_STATE": "poltergust"}  # (no cutscene skipper: slow, can get stuck)
    start = 2600
    if name == "foyer":
        env["LM_START_APPEAR"] = "6"
        inp = MENU + "," + strafe(2600, 60)
    elif name == "turn":
        env["LM_START_APPEAR"] = "6"
        parts = []
        f = 2600
        for i in range(30):
            d = ["STICKUP+STICKLEFT", "STICKLEFT", "STICKDOWN+STICKLEFT", "STICKDOWN", "STICKRIGHT", "STICKUP+STICKRIGHT"][i % 6]
            parts.append(f"{f}:{d}*28")
            f += 30
        inp = MENU + "," + ",".join(parts)
    elif name == "vacuum":
        # heavy dust: the Poltergust running (R) with the nozzle tilted down (C-stick up tilts it down) while
        # walking around the foyer: far more dust than walking kicks up
        env["LM_START_APPEAR"] = "6"
        parts = []
        f = 2600
        for i in range(40):
            d = ["STICKLEFT", "STICKUP", "STICKRIGHT", "STICKDOWN"][i % 4]
            parts.append(f"{f}:R+CUP+{d}*40")
            f += 42
        inp = MENU + "," + ",".join(parts)
    elif name == "nursery":
        inp = MENU + "," + strafe(2600, 60, extra="+STICKUP")
    elif name == "ballroom":
        env["LM_TELEPORT"] = "1800,0,-1400,100"
        inp = MENU + "," + strafe(2600, 60)
    elif name == "ghosts":
        # gold ghosts room (2F): the game's own ghost AI, LM_GHOST_HUNT points the flashlight and
        # the vacuum at them
        env.update({"LM_TELEPORT": "0,550,-3300,100", "LM_GHOST_HUNT": "9", "LM_GHOST_HUNT_BEAM": "89",
                    "LM_GHOST_HUNT_DMAX": "300", "LM_GHOST_HUNT_R": "0"})
        inp = MENU
        start = 2700
    else:
        raise SystemExit(f"unknown scenario {name}")
    return env, inp, start


def write_settings(src, dst, sets):
    lines = open(src, encoding="utf-8").read().splitlines() if src else []
    # apply overrides section.key=value
    for s in sets:
        key, val = s.split("=", 1)
        sec, k = key.split(".", 1)
        out, cur, done = [], None, False
        for line in lines:
            st = line.strip()
            if st.startswith("[") and st.endswith("]"):
                if cur == sec and not done:
                    out.append(f"{k}={val}")
                    done = True
                cur = st[1:-1]
            elif cur == sec and st.split("=", 1)[0] == k:
                if not done:
                    out.append(f"{k}={val}")
                    done = True
                continue
            out.append(line)
        if not done:
            if cur == sec:
                out.append(f"{k}={val}")
            else:
                out += [f"[{sec}]", f"{k}={val}"]
        lines = out
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--scenario", default="foyer")
    ap.add_argument("--settings", default=os.path.join(os.environ.get("APPDATA", ""), "Luigi's Mansion PC", "settings.ini"))
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--fullscreen", action="store_true")
    ap.add_argument("--window", default="")
    ap.add_argument("--secs", type=float, default=20)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "windows-clang", "lmrecomp.exe"))
    ap.add_argument("--disc", default=os.path.join(ROOT, "orig", "GLME01.iso"))
    ap.add_argument("--keep-video", action="store_true")
    ap.add_argument("--no-capture", action="store_true")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--output-idx", type=int, default=0)
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--crop", default="0,0.03,1,0.32",
                    help="analysed region (fractions): the top band holds static scenery (walls, candles), "
                         "away from Luigi, his flashlight beam and the dust he kicks up")
    ap.add_argument("--grid", default="10,3")
    ap.add_argument("--lock", default=os.environ.get("LM_GAME_LOCK", ""),
                    help="machine-wide lock file to hold while the game runs (LM_GAME_LOCK)")
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    if os.path.exists(out):
        shutil.rmtree(out, ignore_errors=True)
    appdata = os.path.join(out, "appdata")
    lmdir = os.path.join(appdata, "Luigi's Mansion PC")
    os.makedirs(lmdir, exist_ok=True)
    os.makedirs(os.path.join(out, "card"), exist_ok=True)
    write_settings(a.settings, os.path.join(lmdir, "settings.ini"), a.set)
    # The user's shader caches (read only copies) spare the first-run pipeline compile.
    user_lm = os.path.join(os.environ.get("APPDATA", ""), "Luigi's Mansion PC")
    for f in ("dawn_cache.db", "pipeline_cache.db"):
        p = os.path.join(user_lm, f)
        if os.path.exists(p):
            shutil.copyfile(p, os.path.join(lmdir, f))

    senv, inp, start = scenario(a.scenario)
    start = a.start or start
    env = dict(os.environ)
    env.update(senv)
    env.update({"APPDATA": appdata, "LM_CARD_DIR": os.path.join(out, "card"), "LM_AUDIO": "0",
                "LM_BACKGROUND": "1", "LM_TOPMOST": "1", "LM_FRAME_TAG": "1", "LM_INTERP_JUDDER": os.path.join(out, "judder.log")})
    for e in a.env:
        k, v = e.split("=", 1)
        env[k] = v
    args = [a.exe, "--disc", a.disc, "--input", inp]
    if a.fullscreen:
        args.append("--fullscreen")
    if a.window:
        args += ["--window", a.window]
    lock = acquire_lock(a.lock) if a.lock else None
    try:
        return run_game(a, args, env, out, start, lock)
    finally:
        if lock:
            try:
                os.remove(lock)
            except OSError:
                pass


GAMES = ("lmrecomp.exe", "luigismansionrecomp.exe", "smsrecomp.exe")


def others_running():
    r = subprocess.run(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True,
                       creationflags=subprocess.CREATE_NO_WINDOW)
    return [l.split(",")[0].strip('"') for l in r.stdout.splitlines() if l.split(",")[0].strip('"').lower() in GAMES]


def acquire_lock(path):
    """Machine-wide measurement lock (parallel game windows distort timing): a file created
    exclusively, holding the branch name; waits while it exists or another game runs."""
    branch = subprocess.run(["git", "-C", ROOT, "rev-parse", "--abbrev-ref", "HEAD"], capture_output=True,
                            text=True).stdout.strip() or "interp"
    while True:
        if not others_running():
            try:
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, branch.encode())
                os.close(fd)
                if not others_running():
                    return path
                os.remove(path)
            except FileExistsError:
                pass
        print("waiting for the game lock / other game processes", flush=True)
        time.sleep(30)


def run_game(a, args, env, out, start, lock):
    log = open(os.path.join(out, "log.txt"), "w", encoding="utf-8")
    game = subprocess.Popen(args, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    print(f"game pid {game.pid}", flush=True)
    rec = None
    video = os.path.join(out, "capture.mp4")
    try:
        # wait for the start frame ("[frame] N presents" lines every 30 presents)
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
        print(f"frame {start} reached, recording {a.secs} s", flush=True)
        if not a.no_capture:
            rec = subprocess.Popen([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                    f"ddagrab=output_idx={a.output_idx}:framerate=1000:dup_frames=0:draw_mouse=0",
                                    "-t", str(a.secs), "-c:v", "hevc_nvenc", "-preset", "p1", "-rc", "constqp", "-qp", "20",
                                    "-fps_mode", "passthrough", "-video_track_timescale", "1000000", video],
                                   stdin=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
            rec.wait(timeout=a.secs + 60)
        else:
            time.sleep(a.secs)
    finally:
        if game.poll() is None:
            game.kill()
        if rec and rec.poll() is None:
            rec.kill()
        log.close()
        if lock:  # analysis below does not need the machine
            try:
                game.wait(timeout=10)
                os.remove(lock)
            except (OSError, subprocess.TimeoutExpired):
                pass
    if not a.no_capture:
        subprocess.run([a.python, os.path.join(HERE, "screen_judder.py"), video, "--tag", "--crop", a.crop, "--grid", a.grid,
                        "--csv", os.path.join(out, "steps.csv")])
    jl = os.path.join(out, "judder.log")
    if os.path.exists(jl):
        subprocess.run([a.python, os.path.join(HERE, "judder.py"), jl, "--frames", f"{start},{start + 100000}"])
        subprocess.run([a.python, os.path.join(HERE, "judder.py"), jl, "--camera"])
    if not a.keep_video and os.path.exists(video):
        os.remove(video)
    return 0


if __name__ == "__main__":
    sys.exit(main())
