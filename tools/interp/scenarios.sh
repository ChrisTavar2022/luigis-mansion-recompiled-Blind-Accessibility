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
set -u
out=${1:?out dir}; scen=${2:?scenario}; fps=${3:-120}; steps=${4:-3}
root=$(cd "$(dirname "$0")/../.." && pwd)
py=${LM_PYTHON:-python}
disc=${LM_DISC:-$root/orig/GLME01.iso}
run="$out/$scen"
rm -rf "$run"; mkdir -p "$run/appdata/Luigi's Mansion PC" "$run/card" "$run/dump"
printf '[video]\ntarget_fps=%s\nmatch_display_refresh=false\n' "$fps" > "$run/appdata/Luigi's Mansion PC/settings.ini"
user_appdata=${APPDATA:-}  # Python finds user-installed packages through it
win() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else echo "$1"; fi; }
export APPDATA="$(win "$run/appdata")" LM_CARD_DIR="$(win "$run/card")" LM_BACKGROUND=1
export LM_INTERP_TEST=$steps LM_INTERP_TRACE=1
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
*) echo "unknown scenario $scen"; exit 1 ;;
esac
export LM_FRAME_DUMP="$(win "$run/dump"),$range"
cd "$root"
timeout "$secs" build/windows-clang/lmrecomp.exe --disc "$disc" --input "$input" > "$run/log.txt" 2>&1
APPDATA="$user_appdata" "$py" "$root/tools/interp/metrics.py" "$run" "$steps"
