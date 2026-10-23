"""Run the native crashRetry executable twice against an isolated SQLite DB."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

executable = Path(sys.argv[1]).resolve()
if os.name != "nt":
    raise SystemExit("These native exception-exit tests require Windows")
with tempfile.TemporaryDirectory(prefix="fah-crash-retry-") as directory:
    environment = os.environ.copy()
    environment.pop("FAH_CRASH_TEST_RELOAD", None)
    for phase in ("first", "reload"):
        if phase == "reload":
            environment["FAH_CRASH_TEST_RELOAD"] = "1"
        result = subprocess.run(
            [str(executable), "--log-to-screen=false"], cwd=directory,
            env=environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            print(result.stdout + result.stderr)
            log = Path(directory) / "log.txt"
            if log.exists():
                print(log.read_text(errors="replace")[-6000:])
            raise SystemExit(f"{phase} failed: {result.returncode}")
        print(result.stdout.strip())
