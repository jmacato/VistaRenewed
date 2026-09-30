"""Reserve native-test evidence; interrupted runs cannot be overwritten."""
from contextlib import contextmanager
import os
import json
import time

@contextmanager
def reserve_output(out, run, parser):
    """Never reuse a started run, including one killed before its first result."""
    lock_path = out / "runner-lock.json"
    try:
        lock = lock_path.open("x")
    except FileExistsError:
        parser.error("output is reserved by an active/interrupted invocation; use a fresh --output")
    try:
        with lock:
            reservation = {"pid": os.getpid(), "start_unix": time.time(), "gpu_run_requested": run}
            lock.write(json.dumps(reservation) + "\n")
            lock.flush()
            os.fsync(lock.fileno())
            existing = [p for p in (out / "run-start.json", out / "results.json") if p.exists()]
            existing += list(out.glob("*.stdout.jsonl")) + list(out.glob("*.stderr.log"))
            if existing:
                parser.error("run evidence already exists; use a fresh --output directory")
            if run:
                # Exclusive and permanent: a crash, signal, parse failure, or
                # stale --no-build rejection must not authorize overwriting.
                with (out / "run-start.json").open("x") as marker:
                    marker.write(json.dumps(reservation) + "\n")
                    marker.flush()
                    os.fsync(marker.fileno())
            yield
    finally:
        lock_path.unlink()
