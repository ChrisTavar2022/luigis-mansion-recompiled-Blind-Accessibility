#!/bin/sh
# Starts Luigi's Mansion Recompiled and keeps a log of the run (README.txt, "Reporting problems").
# All arguments are passed on, e.g.  ./LuigisMansionRecompiled.sh --setup
HERE=$(dirname "$(readlink -f "$0")")
DATA_BASE="${XDG_DATA_HOME:-$HOME/.local/share}"
DATA="$DATA_BASE/Luigi's Mansion Recompiled"
# Up to 1.0.1 the folder was "Luigi's Mansion PC": move it before the log is written (the game
# moves the config and cache folders itself, and copies this one if the move fails).
if [ ! -e "$DATA" ] && [ -d "$DATA_BASE/Luigi's Mansion PC" ]; then
    mv "$DATA_BASE/Luigi's Mansion PC" "$DATA" 2>/dev/null
fi
mkdir -p "$DATA"
LOG="$DATA/last_run.log"
[ -f "$LOG" ] && mv -f "$LOG" "$DATA/previous_run.log"
# Libraries shipped next to the game (none by default; everything is linked in) come first.
if [ -d "$HERE/lib" ]; then
    LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi
# Unpacking a .zip can drop the executable bits.
chmod +x "$HERE/LuigisMansionRecomp" "$HERE/llvm/bin/clang" "$HERE/llvm/bin/ld.lld" 2>/dev/null
{
    echo "Luigi's Mansion Recompiled - $(date)"
    uname -srm
    grep -E '^PRETTY_NAME=' /etc/os-release 2>/dev/null
    ldd --version 2>/dev/null | head -n 1
    echo "XDG_SESSION_TYPE=${XDG_SESSION_TYPE} WAYLAND_DISPLAY=${WAYLAND_DISPLAY} DISPLAY=${DISPLAY}"
    echo "args: $*"
    echo "----"
} > "$LOG"
STATUS_FILE="$DATA/.last_exit_status"
rm -f "$STATUS_FILE"
{ "$HERE/LuigisMansionRecomp" "$@"; echo $? > "$STATUS_FILE"; } 2>&1 | tee -a "$LOG"
STATUS=$(cat "$STATUS_FILE" 2>/dev/null || echo 1)
if [ "$STATUS" != "0" ]; then
    echo "Luigi's Mansion Recompiled exited with status $STATUS. Log: $LOG" >&2
fi
exit "$STATUS"
