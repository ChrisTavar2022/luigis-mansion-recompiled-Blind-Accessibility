"""Runs a command while holding the machine-wide game lock (LM_GAME_LOCK, see screen_run.py).

    lockrun.py <command> [args...]

Waits while the lock file exists or another lmrecomp/LuigisMansionRecomp/smsrecomp runs, creates
the lock with the branch name, runs the command, removes the lock. Without LM_GAME_LOCK it just
runs the command.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screen_run import acquire_lock  # noqa: E402


def main():
    lock = os.environ.get("LM_GAME_LOCK", "")
    if lock:
        acquire_lock(lock)
    try:
        return subprocess.run(sys.argv[1:]).returncode
    finally:
        if lock:
            try:
                os.remove(lock)
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main())
