# Storage self-benchmark

Requires Python 3.9+, Git, Linux `/proc`, `findmnt`, `lsblk`, and a Release build.
No Python packages or privileged cache dropping are needed. From the repository:

```sh
cmake --preset release -DCMAKE_CXX_COMPILER=g++-14
cmake --build --preset release
python3 bench/run.py build/release/bench/laminar_storage_bench build/measurements
```

Choose a new output directory for each full run. Raw per-call samples, per-run
metrics, `metadata.json` and `summary.json` remain under ignored `build/`. The
runner stops and retains the dataset and diagnostics on failure; successful
datasets are removed. `--smoke` runs the same 25 cases with small datasets and
checks results, sync counts, percentiles and cleanup, then removes its artifacts.
The Release CTest suite includes this smoke check when Python is available.

## Fixed workload

All cases use seed **70405**, 16-byte binary keys, a fixed pool of 128 pseudorandom
values, and **three independent repetitions** in fresh processes/directories.
The pool repeats across records; Btrfs compression can affect results. No disk
cache flush is attempted. These are warm OS page-cache runs on freshly written
data, with no application record cache. Run without other tests/benchmarks for
comparability, and record unrelated machine load.

| Case | Work and warmup |
|---|---|
| Durable writes | Put singletons versus Write batches of 8/32/128; 64 B/1 KiB values use 2,048 mutations, 64 KiB values use 1,024. Warm up with 128 mutations in one batch. |
| Read amplification | Return the same 64 B target from a singleton, 128 or 1,024 small operations, or a 128-operation frame whose other values are 64 KiB. Warm up with 16 Gets, then measure 128. |
| Recovery/index memory | Prepare 1,024/8,192/32,768 mutations using batches of 128, retaining 1,024/8,192/8,192 live keys respectively; time one Open in a fresh process. Preparation warms the log; no prior Open in that process. |
| Compaction | 4,096 keys of 64 B: clean grouped fill, or four overwrite passes followed by deleting every fourth key. Time one offline Compact after closing the handle. Preparation warms the log. |
| Caller contention | 1/2/4/8 callers perform 1,024 durable singleton Puts over the same 64 keys. Warm up with 128 mutations. Thread creation precedes timing; elapsed time includes admission/barrier and join. |

Every nonempty commit is sync-before-success. The existing syscall seam counts
actual data-sync calls and asserts one per successful measured write; it does not
replace real I/O. Batch ownership/preparation is outside individual call latency
but inside write throughput. Get latency includes frame validation and the owned
result copy. Contention latency includes mutex wait. Post-run value checks sample
each written batch and its final key (all keys for singletons), every recovery/
compaction key and each contention key. Failures invalidate the run.

The binary emits raw CSV. The runner reports per-repetition throughput and
nearest-rank p50/p95/p99, plus pooled latency percentiles over all repetitions.
Batch latency is **per batch**, throughput is **mutations/second**; dividing batch
latency by batch size is not an individual mutation's response time. Recovery and
compaction have one timing per repetition, so their pooled percentiles describe
only three observations. Large batches also have few samples: retain the sample
counts and avoid interpreting their tails as stable production estimates.

`frame_bytes_per_get` is the enclosing validated frame length derived from log
size; the seam verifies two Read calls per Get. It describes application traffic,
not physical device reads. Recovery records resident RSS before/after Open and
process peak RSS: the delta includes index, allocator, scratch and runtime pages,
and is **not an exact index allocation measurement**. No byte quota or allocator
instrumentation is added. Compaction observes the complete temporary file before
rename and reports peak **logical** source+temp bytes; Btrfs physical allocation,
compression and device write amplification are not measured. CPU time uses the
process CPU clock. Wall time uses the steady clock.

The runner records revision/dirty state, workload, seed, compiler/flags,
CPU/RAM/storage model, kernel/filesystem/mount options, cache and sync policy.
Keep raw machine-local output under build/; publish only reviewed synthetic data
and metadata. There is no required speedup and no external-engine comparison.

## Recorded result

The [2026-11-11 evidence](results/2026-11-11.md) reports all cases, including clean
compaction growth and serialized caller throughput. Measurements are local
observations, not performance or hardware-durability guarantees.
