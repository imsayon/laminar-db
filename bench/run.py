#!/usr/bin/env python3
"""Run fixed storage workloads; keep raw samples and public-safe metadata."""

import argparse
import csv
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


def percentile(samples, percent):
    ordered = sorted(samples)
    return ordered[max(0, math.ceil(len(ordered) * percent / 100) - 1)]


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path, help="new directory below build/")
    parser.add_argument("--smoke", action="store_true", help="smaller self-check datasets")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    binary = args.binary.resolve()
    output = args.output.resolve()
    if not output.is_relative_to(repo / "build"):
        parser.error("output must be below build/")
    if args.smoke:
        output.parent.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix=output.name + "-", dir=output.parent))
    else:
        output.mkdir(parents=True, exist_ok=False)
    build = binary.parent.parent
    cache = {key.split(":", 1)[0]: value
             for line in (build / "CMakeCache.txt").read_text().splitlines()
             if "=" in line and not line.startswith(("#", "//"))
             for key, value in [line.split("=", 1)]}
    assert cache["CMAKE_BUILD_TYPE"] == "Release", "benchmarks require Release"
    compiler = cache["CMAKE_CXX_COMPILER"]
    cpu = next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
               if line.startswith("model name"))
    compile_entry = json.loads((build / "compile_commands.json").read_text())
    flags = next(entry["command"].split() for entry in compile_entry
                 if entry["file"].endswith("/bench/storage.cpp"))
    metadata = {
        "utc": datetime.now(timezone.utc).isoformat(),
        "revision": command("git", "-C", str(repo), "rev-parse", "HEAD"),
        "dirty": bool(command("git", "-C", str(repo), "status", "--porcelain")),
        "compiler": command(compiler, "--version").splitlines()[0],
        "cmake": command("cmake", "--version").splitlines()[0],
        "flags": [flag for flag in flags if flag.startswith(("-O", "-D", "-std=", "-fsanitize"))],
        "kernel": platform.release(), "architecture": platform.machine(), "cpu": cpu,
        "logical_cpus": int(command("getconf", "_NPROCESSORS_ONLN")),
        "memory": Path("/proc/meminfo").read_text().splitlines()[0],
        "filesystem": command("findmnt", "-n", "-o", "FSTYPE,OPTIONS", "-T", str(output)),
        "storage_models": command("lsblk", "-d", "-n", "-o", "MODEL,TRAN,ROTA"),
        "cache": "warm OS page cache; no cache dropping; fresh dataset per case/repetition",
        "durability": "one fdatasync per successful nonempty commit; no relaxed writes",
        "seed": 70405, "repetitions": 3, "smoke": args.smoke,
        "load_average_start": os.getloadavg(),
        "warmup": "128 writes; 16 Gets; recovery uses freshly prepared cached log; compaction uses freshly filled log",
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    assert percentile([4, 1, 3, 2], 50) == 2
    assert percentile([1, 2, 3], 99) == 3
    cases = []
    for size in (64, 1024, 65536):
        for batch in (1, 8, 32, 128):
            count = 128 if args.smoke else (1024 if size == 65536 else 2048)
            cases.append((f"write-{size}-{batch}", "write", count, size, batch))
    for batch, size in ((1, 64), (128, 64), (1024, 64), (128, 65536)):
        cases.append((f"read-{size}-{batch}", "read", 4 if args.smoke else 128, size, batch))
    for count, live in ((1024, 1024), (8192, 8192), (32768, 8192)):
        name = f"recovery-{count}-{live}"
        if args.smoke:
            count, live = min(count, 512), min(live, 256)
        cases.append((name, "recovery", count, 64, live))
    for rounds in (1, 4):
        cases.append((f"compact-{rounds}", "compact", 128 if args.smoke else 4096, 64, rounds))
    for callers in (1, 2, 4, 8):
        cases.append((f"contention-{callers}", "contention", 128 if args.smoke else 1024, 64, callers))
    summary = []
    with (output / "raw.csv").open("w") as raw_file:
        raw = csv.writer(raw_file)
        raw.writerow(("case", "repetition", "kind", "name", "value"))
        for name, mode, count, size, parameter in cases:
            all_samples = []
            repetitions = []
            for repetition in range(1, 4):
                scratch = Path(tempfile.mkdtemp(prefix="db-", dir=output))
                directory = scratch / "db"
                args_list = [str(binary), mode, str(directory), str(count), str(size), str(parameter)]
                if mode == "recovery":
                    subprocess.run([str(binary), "prepare", *args_list[2:]], check=True,
                                   stdout=subprocess.DEVNULL)
                result = subprocess.run(args_list, text=True, capture_output=True)
                (output / f"{name}-{repetition}.csv").write_text(result.stdout)
                if result.returncode:
                    (output / f"{name}-{repetition}.stderr").write_text(result.stderr)
                    result.check_returncode()
                metrics = {}
                samples = []
                for kind, key, value in csv.reader(result.stdout.splitlines()):
                    raw.writerow((name, repetition, kind, key, value))
                    if kind == "sample":
                        samples.append(float(value))
                    else:
                        assert kind == "metric" and key not in metrics
                        metrics[key] = float(value)
                assert samples and all(math.isfinite(value) and value >= 0 for value in samples)
                calls = count // parameter if mode == "write" else count
                if mode in ("recovery", "compact"):
                    calls = 1
                assert len(samples) == calls
                if mode in ("write", "contention"):
                    assert metrics["data_syncs"] == calls
                logical = metrics["live_keys"] * (16 + size)
                if mode == "read":
                    logical = parameter * 16 + 64 + (parameter - 1) * size
                metrics["logical_live_bytes"] = logical
                metrics["log_to_live_ratio"] = metrics["log_bytes"] / logical
                metrics["calls_per_second"] = calls * 1e6 / metrics["wall_us"]
                if mode in ("write", "contention"):
                    metrics["mutations_per_second"] = count * 1e6 / metrics["wall_us"]
                metrics.update({f"p{p}_us": percentile(samples, p) for p in (50, 95, 99)})
                repetitions.append(metrics)
                all_samples.extend(samples)
                shutil.rmtree(scratch)
                assert not scratch.exists()
                raw_file.flush()
            row = {"case": name, "mode": mode, "count": count, "value_bytes": size,
                   "parameter": parameter, "repetitions": repetitions}
            row.update({f"pooled_p{p}_us": percentile(all_samples, p) for p in (50, 95, 99)})
            summary.append(row)
            (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
            print(f"{name}: p50={row['pooled_p50_us']:.1f} us", flush=True)
    assert len(summary) == 25
    assert len({row["case"] for row in summary}) == len(summary)
    if args.smoke:
        shutil.rmtree(output)


if __name__ == "__main__":
    main()
