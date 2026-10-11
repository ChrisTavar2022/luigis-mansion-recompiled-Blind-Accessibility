#!/bin/bash
# Frame-interpolation test scenarios (docs/INTERPOLATION.md, "Everything interpolates").
#
#   tools/interp/scenarios.sh <out dir> <scenario> [fps] [steps]
#
# Runs build/windows-clang/lmrecomp.exe with a scratch APPDATA and memory card under <out dir>,
# LM_INTERP_TEST=<steps> (default 3) and LM_INTERP_TRACE=1, dumps the scenario's frames to
# <out dir>/<scenario>/dump and prints tools/interp/metrics.py's summary. Needs the disc at
# orig/GLME01.iso (or LM_DISC=<path>) and Python with numpy + Pillow (LM_PYTHON=<python>).
#
# Scenarios (US):
#   nursery  walking, fast turns, nozzle aiming, flashlight, lightning with shadows, GBH open/close,
#            dust, spiderwebs, the door to the next room          (frames 2580-2960)
#   ghosts   gold ghosts room: appear/hover/attack/vanish, stun, vacuum, capture (3280-3600)
#   opening  title -> opening cutscene (crows, camera cuts) -> foyer, nothing skipped (1880-2120)
#   fire     nursery with the fire element: blowing fire, vacuum air stream, turns (2580-2920)
#   water    as fire with the water element (2580-2920)
#   ballroom ballroom (dancing ghosts, chairs, flashlight), turning (2580-2900)
#   call     E. Gadd's Game Boy Horror call: call screen, portrait, message windows (2590-2900)
#   pause    the pause screen and its pages, back to the game (2590-2880)
#   ending   the ending scene, rank A (2610-2900)
# Env: LM_RANGE=<first>,<last> dumps part of it, LM_SECS stops earlier, LM_EXTRA_INI adds settings,
# LM_EXE runs another build (e.g. main's, for the Original 30 byte comparison).
set -u
out=${1:?out dir}; scen=${2:?scenario}; fps=${3:-120}; steps=${4:-3}
root=$(cd "$(dirname "$0")/../.." && pwd)
py=${LM_PYTHON:-python}
disc=${LM_DISC:-$root/orig/GLME01.iso}
run="$out/$scen"
rm -rf "$run"; mkdir -p "$run/appdata/Luigi's Mansion PC" "$run/card" "$run/dump"
printf '[video]\ntarget_fps=%s\nmatch_display_refresh=false\n' "$fps" > "$run/appdata/Luigi's Mansion PC/settings.ini"
# LM_EXTRA_INI='[section]\nkey=value...': more settings (e.g. enhanced lighting, world lights)
if [ -n "${LM_EXTRA_INI:-}" ]; then printf '%b\n' "$LM_EXTRA_INI" >> "$run/appdata/Luigi's Mansion PC/settings.ini"; fi
user_appdata=${APPDATA:-}  # Python finds user-installed packages through it
win() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else echo "$1"; fi; }
export APPDATA="$(win "$run/appdata")" LM_CARD_DIR="$(win "$run/card")" LM_BACKGROUND=1
export LM_INTERP_TEST=$steps LM_INTERP_TRACE=1
[ "$steps" = 0 ] && unset LM_INTERP_TEST  # steps 0: a real-time run (pacing, alpha, cost)
menu="300:START*10,450:START*10,600:A*6,750:A,900:A,1050:A,1200:A,1350:A,1500:A,1650:A"
nursery() { export LM_INPUT_CLOCK=frames LM_START_MAP=2 LM_START_APPEAR=1 LM_SKIP_CUTSCENES=1 LM_SKIP_AUTO=30; }
case $scen in
nursery)
    nursery; export LM_TEST_STATE=poltergust LM_LIGHTNING=0,90,1; range=2580,2960; secs=700
    input="$menu,2600:STICKUP*20,2620:STICKDOWN*6,2626:STICKUP*6,2632:STICKLEFT*20,2652:STICKRIGHT*8,2660:STICKDOWN*30,2690:CLEFT*10,2700:CRIGHT*10,2710:Y*2,2760:Y*2,2790:STICKLEFT*40,2830:STICKUP+STICKRIGHT*30,2860:STICKRIGHT*40,2900:STICKUP*80" ;;
fire|water)
    nursery; export LM_TEST_STATE=poltergust+$scen; range=2580,2920; secs=700
    input="$menu,2600:STICKLEFT*10,2620:L*40,2670:STICKRIGHT*12,2690:L*40,2740:R*50,2800:STICKUP*20,2820:R+STICKLEFT*40,2870:L+STICKDOWN*30" ;;
ghosts)
    export LM_TEST_STATE=poltergust LM_TELEPORT=0,550,-3300,100 LM_GHOST_HUNT=9 LM_GHOST_HUNT_BEAM=89 LM_GHOST_HUNT_DMAX=300 LM_GHOST_HUNT_R=0
    range=3280,3600; secs=700; input="600:START*10,1000:A*6" ;;
opening)
    range=1880,2120; secs=700; input="600:START*10,1000:A*6" ;;
ballroom)
    nursery; export LM_TEST_STATE=poltergust LM_TELEPORT=1800,0,-1400,100; range=2580,2900; secs=700
    input="$menu,2600:STICKLEFT*8,2612:STICKRIGHT*8,2624:STICKLEFT*8,2636:STICKRIGHT*8,2660:STICKUP*40,2720:CLEFT*20,2760:CRIGHT*20,2800:STICKDOWN*40" ;;
call)  # E. Gadd's Game Boy Horror call ("first Boo", event flag 15): call screen, portrait, message windows
    nursery; export LM_TEST_STATE=poltergust LM_SKIP_AUTO=30:1 LM_SKIP_TEST=2600:flag:15; range=2590,2900; secs=700
    a=""; for f in $(seq 2640 24 2900); do a="$a,$f:A*4"; done
    input="$menu$a" ;;
pause)  # the pause screen (START) and its pages (L/R), back to the game
    nursery; export LM_TEST_STATE=poltergust; range=2590,2880; secs=700
    input="$menu,2610:STICKLEFT*20,2630:START*3,2670:R*3,2700:R*3,2730:L*3,2760:L*3,2790:B*3,2810:START*3,2830:STICKRIGHT*30" ;;
ending)  # the ending scene (house, rank, caption fades), rank A
    nursery; export LM_TEST_STATE=poltergust LM_TEST_ENDING=8@2620; range=2610,2900; secs=700
    a=""; for f in $(seq 2700 40 2900); do a="$a,$f:A*4"; done
    input="$menu$a" ;;
*) echo "unknown scenario $scen"; exit 1 ;;
esac
range=${LM_RANGE:-$range}  # LM_RANGE=<first>,<last>: dump only part of the scenario
export LM_FRAME_DUMP="$(win "$run/dump"),$range"
cd "$root"
secs=${LM_SECS:-$secs}  # LM_SECS: stop earlier (with LM_RANGE)
timeout "$secs" "${LM_EXE:-build/windows-clang/lmrecomp.exe}" --disc "$disc" --input "$input" > "$run/log.txt" 2>&1
APPDATA="$user_appdata" "$py" "$root/tools/interp/metrics.py" "$run" "$steps"
