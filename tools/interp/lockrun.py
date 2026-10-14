"""Runs a command while holding the machine-wide game lock (LM_GAME_LOCK, see screen_run.py).

    lockrun.py <command> [args...]

Gives up at once (exit 2) if the lock file exists or another lmrecomp/LuigisMansionRecomp/smsrecomp
runs (a waiting runner would start later on its own); otherwise creates the lock with the branch
name, runs the command and removes the lock if it still holds that name. Without LM_GAME_LOCK it
just runs the command.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screen_run import acquire_lock, release_lock  # noqa: E402


def main():
    lock = os.environ.get("LM_GAME_LOCK", "")
    held = acquire_lock(lock) if lock else None
    if lock and not held:
        print("game lock held or another game running: not started")
        return 2
    try:
        return subprocess.run(sys.argv[1:]).returncode
    finally:
        release_lock(held)


if __name__ == "__main__":
    sys.exit(main())
