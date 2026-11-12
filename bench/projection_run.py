#!/usr/bin/env python3
"""Run the fixed Redis demonstration three times, retaining raw evidence under build/."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

sys.dont_write_bytecode = True
from run import command, percentile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path, help="new directory below build/")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    binary, output = args.binary.resolve(), args.output.resolve()
    if not output.is_relative_to(repo / "build"):
        parser.error("output must be below build/")
    output.mkdir(parents=True, exist_ok=False)
    build = binary.parent.parent
    cache = {key.split(":", 1)[0]: value
             for line in (build / "CMakeCache.txt").read_text().splitlines()
             if "=" in line and not line.startswith(("#", "//"))
             for key, value in [line.split("=", 1)]}
    assert cache["CMAKE_BUILD_TYPE"] == "Release", "measurements require Release"
    compile_commands = json.loads((build / "compile_commands.json").read_text())
    flags = next(entry["command"].split() for entry in compile_commands
                 if entry["file"].endswith("/bench/projection.cpp"))
    tracked = command("git", "-C", str(repo), "ls-files", "--cached", "--others", "--exclude-standard")
    metadata = {
        "utc": datetime.now(timezone.utc).isoformat(),
        "revision": command("git", "-C", str(repo), "rev-parse", "HEAD"),
        "dirty": bool(command("git", "-C", str(repo), "status", "--porcelain")),
        "source_sha256": {name: hashlib.sha256((repo / name).read_bytes()).hexdigest()
                          for name in tracked.splitlines() if name.endswith((".cpp", ".h", ".py"))},
        "compiler": command(cache["CMAKE_CXX_COMPILER"], "--version").splitlines()[0],
        "flags": [flag for flag in flags if flag.startswith(("-O", "-D", "-std=", "-fsanitize"))],
        "redis": command(cache["REDIS_SERVER"], "--version"),
        "kernel": platform.release(), "architecture": platform.machine(),
        "cpu": next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                    if line.startswith("model name")),
        "logical_cpus": os.cpu_count(), "memory": Path("/proc/meminfo").read_text().splitlines()[0],
        "filesystem": command("findmnt", "-n", "-o", "FSTYPE,OPTIONS", "-T", str(output)),
        "storage_models": command("lsblk", "-d", "-n", "-o", "MODEL,TRAN,ROTA"),
        "load_average_start": os.getloadavg(), "seed": 70505, "repetitions": 3,
        "workload": "8 warmup then 64 batches x 32 operations; 128 binary 16-byte keys; 1024-byte values; 1/5 deletes",
        "maximum": "2 Puts + 1022 empty-key Deletes; 16777216 payload bytes, 65536-byte key and 8388608-byte value",
        "cache": "warm OS page cache, fresh source and isolated Redis per repetition, no cache dropping",
        "durability": "source sync-before-success; Redis persistence disabled, no eviction, no network port",
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    results = []
    for repetition in range(1, 4):
        result = subprocess.run([str(binary)], cwd=output, text=True, capture_output=True)
        (output / f"run-{repetition}.csv").write_text(result.stdout)
        (output / f"run-{repetition}.stderr").write_text(result.stderr)
        result.check_returncode()
        samples = {"apply_us": [], "replay_us": []}
        metrics, events = {}, []
        for row in csv.reader(result.stdout.splitlines()):
            if row[0] == "sample":
                samples[row[1]].append(float(row[2]))
            elif row[0] == "metric":
                assert row[1] not in metrics
                metrics[row[1]] = float(row[2])
            elif row[0] == "event":
                assert len(row[2]) == 16 and all(c in "0123456789abcdef" for c in row[2])
                events.append(dict(zip(("stage", "sequence_hex", "sequence_lag", "byte_lag", "peak_pending_bytes"), row[1:])))
            else:
                assert row[0] == "config"
        assert all(len(values) == 64 for values in samples.values())
        assert sum(event["stage"] == "replay" for event in events) == 64
        assert events[0]["sequence_lag"] == "64"
        assert events[-1]["stage"] == "maximum_exact_convergence"
        assert events[-1]["sequence_lag"] == events[-1]["byte_lag"] == "0"
        assert metrics["maximum_pending_bytes"] == 16777248
        metrics.update({f"{name}_p{p}": percentile(values, p)
                        for name, values in samples.items() for p in (50, 95, 99)})
        results.append({"repetition": repetition, "metrics": metrics, "events": events})
        (output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"run {repetition}: apply p50={metrics['apply_us_p50']:.1f} us; maximum={metrics['maximum_apply_us']:.1f} us", flush=True)


if __name__ == "__main__":
    main()
