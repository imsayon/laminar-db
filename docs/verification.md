# Reproduce verification

Use fresh directories under `build/` for clean gates. Ubuntu 24.04 packages for
the checked toolchain are `cmake make g++-14 clang-18 clang-tidy-18 clang-format-18
libc++-18-dev libc++abi-18-dev libclang-rt-18-dev python3 git redis-server redis-tools`.
Native compiler/runtime combinations must actually support C++23 `std::expected`.

```sh
cmake --preset gcc-debug -B build/check-gcc -DCMAKE_CXX_COMPILER=g++-14
cmake --build build/check-gcc -j4
ctest --test-dir build/check-gcc --output-on-failure -V
```

Repeat configure/build/CTest for `release`, `sanitizers`, and `thread-sanitizer`
using distinct directories and GCC 14. For `clang-debug` and `clang-tidy`, use
`-DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_CXX_FLAGS=-stdlib=libc++`; add
`-DCMAKE_CXX_CLANG_TIDY=clang-tidy-18` for static analysis. Use
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` and
`TSAN_OPTIONS=halt_on_error=1` for their corresponding test runs.

```sh
find include src tests bench -type f \( -name '*.h' -o -name '*.cpp' \) -print0 |
  xargs -0 clang-format-18 --dry-run --Werror
git diff --check
```

## Failure and shared-handle campaigns

CTest runs [API/model tests](../tests/test_db.cpp),
[file/allocator failures and real process kills](../tests/test_storage_failures.cpp),
[compaction tests](../tests/test_compaction.cpp),
[codec/fixture tests](../tests/test_format.cpp), [CLI checks](../tests/test_cli.cmake)
and the [runnable demo](../examples/storage_demo.sh), alongside the in-memory
learning-component tests. For focused reproduction:

```sh
ctest --test-dir build/check-gcc -R 'laminar_(db|storage_failure|compaction|cli|demo)_tests' --output-on-failure -V
```

The seeded restart model uses **0x4c4d4e52, 0x52454331, 70403, 70404**, each with
6,000 generated operations over 128 binary keys: **24,000 operations**. Every 500
operations it injects a failure, reopens, then kills another writer at a partial
append or complete pre-publication boundary: 48 injected failures, 48 SIGKILLs,
and 24 compactions across these campaigns. Failure classes cycle through
pre-append allocation failure, partial append/ENOSPC, sync EIO, and post-sync
publication allocation failure. The model allows complete unacknowledged frames
to survive, checks all keys after recovery, and checks inspection leaves bytes
unchanged. This extends the earlier two-seed model; it does not count historical
runs as new evidence.

Shared-key stress uses **0x42415443, 0x53484152, 70401, 70402**, four callers and
4,096 batch commits per seed: **16,384 commits / 32,768 mutations**, plus a Get
per commit. The existing under-mutex publication receipt orders the committed
history for final-state and reopen comparison. Separate small histories check
legal serializations respecting invocation/completion order; publication barriers
and concurrent Close tests remain enabled. Stress final-state checks do not
replace those small history checks.

Named fault tests additionally cover creation/recovery, EINTR/short transfers,
allocation boundaries, descriptor cleanup, compaction read/write/backfill/sync/
close/rename/directory-sync failures, corruption and temp ownership. Their
deterministic SIGKILLs and fixture truncations are distinct from the seeded
campaigns. Successful temporary DB directories are removed; failed directories
and seeds are printed and retained. Run within build/ so evidence stays ignored.

## Decoder fuzz budgets

[Seed generator](../tests/fuzz_seeds.cpp) reuses checked V1 fixtures and emits
file/header/seal, mixed/empty/1,024-operation frames, malformed lengths/tags and
truncations. The [target](../tests/fuzz_format.cpp) checks accepted-input round
trips and recomputes checksums on a second pass to reach semantic validation.
It performs no database writes and bounds accepted input by the format cap.

```sh
cmake --preset clang-debug -B build/fuzz -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_CXX_FLAGS=-stdlib=libc++
cmake --build build/fuzz --target laminar_format_fuzz laminar_fuzz_seeds -j4
build/fuzz/tests/laminar_fuzz_seeds build/fuzz-seeds
mkdir -p build/fuzz-corpus
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  build/fuzz/tests/laminar_format_fuzz -seed=70406 -runs=10000 -max_len=16384 -timeout=5 \
  -artifact_prefix=build/ build/fuzz-corpus build/fuzz-seeds
```

Smoke budget: **10,000 executions**, seed **70406** (also in Clang CI).
Stress: use a fresh corpus directory, **1,000,000 executions**, seed **70407**,
with the same flags. Neither campaign explores the entire 16 MiB input range:
this budget caps generated inputs at 16 KiB. Exact key/value/payload caps have
deterministic API/decoder tests. Keep corpus, logs and any crash artifacts under
build/; replay a failing artifact as the fuzzer's file argument. Fuzzing is
bounded evidence, not proof of decoder correctness.

## Walkthrough and readiness

Start with the [demo](../examples/storage_demo.sh), then trace the shared commit
path in [db.cpp](../src/storage/db.cpp): append → sync → index publication under
one mutex. Trace Open's private replay before handle publication, the
[format checks](../src/storage/format.cpp), and Compact's validated, synced
replacement under stable LOCK. Explain an ambiguous error using
`AmbiguousWrites` and `PublicationBarrier`, and show pre/post-rename authority
using `CompactionFailuresAndKills`. Finally compare the
[benchmark evidence](../bench/results/2026-11-11.md): sync amortization, read
amplification, RAM-bound index, startup scan and offline maintenance are explicit
tradeoffs. The skip list/MemTable are separate learning components, not the
persistent engine's write path.

| V1 success criterion | Evidence/status |
|---|---|
| Reference-map parity after restart, overwrite, delete and compaction | SATISFIED; seeded model, API and compaction tests |
| Interrupted batches recover wholly or not at all | SATISFIED; every-byte cuts, injected failures and named process-kill boundaries |
| Append/sync/replacement failures have explicit outcomes | SATISFIED; fault tests and [storage failure contract](storage.md#operations-and-failures) |
| Bounded parsing of malformed/truncated/corrupt input | SATISFIED; fixtures, exact-cap tests and dated decoder fuzz budgets below |
| Shared callers observe a serialized history | SATISFIED; small legal-history checks, publication barriers, shared-key stress and TSan |
| Runnable persistence/batch/error/compaction demonstration | SATISFIED; CLI tests and [storage demo](../examples/storage_demo.sh) |
| Reproducible measurements with raw output/environment | SATISFIED; [storage report](../bench/results/2026-11-11.md) and [projection runner](projection.md#reproduce-the-slice) |
| Ordered committed batches and duplicate-safe projection progress | SATISFIED; [change-stream](../tests/test_changes.cpp) and [Redis apply](../tests/test_projection.cpp) tests |
| History expiry/checkpoint loss require exact same-watermark repair | SATISFIED; [recovery tests](../tests/test_projection_recovery.cpp) and isolated projection demo |
| Compiler, sanitizer, static and format gates | PASS; dated receipts below, with exact-SHA hosted verification |
| Physical power loss / hardware certification | UNKNOWN; no experiment performed |
| Network filesystem / hostile directory mutation | Unsupported operating model |
| Scans/snapshots, background maintenance, arbitrary Redis writers/raw-reader admission | Unsupported V1 scope |
| Production-workload suitability / owner acceptance for a deployment | UNKNOWN; no production evaluation or deployment acceptance observed |
| Release tag / published distribution / deployment | Not performed by this milestone |

Storage verification assumes the documented filesystem behavior. The projection
additionally requires an exclusively owned standalone namespace and application
reads through the adapter. These checks do not establish general production suitability.

## 2026-11-11 receipt

Fresh local checks cover tooling revision
`be98940ba6e61f26f2cd5ef489f767ceb163db97`, following the verified storage baseline
`3c68a02380f3a370457e6a1805c84061dd906095`. The latter's
[six-job push run](https://github.com/imsayon/laminar-db/actions/runs/42979273040)
is inherited evidence; it is not counted as a fresh Phase 04 run.

| Fresh check | Result |
|---|---|
| Native GCC 16.2.1 Release | PASS; full suite plus focused final failure/demo/benchmark reruns |
| Ubuntu GCC 14.2 Debug | PASS 9/9, using the corrected failure-campaign rerun |
| Ubuntu GCC 14.2 Release | PASS 10/10, using the final benchmark-smoke rerun |
| Clang/libc++ 18.1.3 Debug | PASS 9/9 |
| GCC 14.2 ASan/UBSan with leak/halt settings | PASS 9/9; no findings |
| GCC 14.2 TSan with halt-on-error | PASS 9/9; no race reports |
| clang-tidy 18.1.3 and clang-format 18.1.3 | PASS build/analysis and formatting |
| Decoder smoke / stress | PASS 10,000 / 1,000,000 executions; 1 / 262 seconds; seeds 70406 / 70407 |
| API snippet, CLI/demo and successful temporary-directory cleanup | PASS |

Two initial tooling failures were corrected before this receipt: an existing-key
update did not guarantee the intended publication allocation, and the benchmark
runner assumed CMake's compiler cache entry was always FILEPATH rather than
also STRING. The final campaign deletes/reinserts the key; the runner handles
both cache types. The disposable Ubuntu toolchain also needed the documented Git
prerequisite for benchmark metadata. No storage-engine behavior changed.

Local build/test logs, generated fuzz corpora and raw measurements are retained
under ignored build/. See [benchmark results](../bench/results/2026-11-11.md) for
the workload, environment and measurement limits. Hosted verification is
available in [Actions](https://github.com/imsayon/laminar-db/actions/workflows/ci.yml);
match the full commit SHA and require gcc, clang, sanitizers, thread-sanitizer,
formatting and static-analysis all successful.

## Committed stream and Redis recovery

The change-stream tests cover owned whole batches, limits and required_bytes,
persistent exclusive cursors, order, >2^53/maximum sequences, concurrent pull,
uncertain writes recovered only after sync, restart and compaction expiry.
Projection tests require an isolated real Redis instance launched by the harness.
They cover duplicate/gap handling, lost replies, timeout/outage, partial Lua errors,
read admission, and SIGKILL before observing an error. Recovery tests cover exact
same-watermark comparison, missing/wrong/extra/deleted data, checkpoint/dataset
loss, partial rebuild, paused writes/reads and restart. The runnable projection
demo is also a CTest gate. [Commands and measurement limits](projection.md) remain
separate from physical power-loss evidence.

## 2026-11-12 stream/projection local checks

GCC 14 Debug, Clang/libc++ 18, ASan/UBSan and TSan passed all 13 CTest entries;
GCC 14 Release passed all 14, including storage benchmark smoke. Final affected
CDC/projection/demo checks passed again in all five builds after review, together
with clang-tidy/format 18. The exact-byte-page regression verifies that a completed
page does not inspect the following frame. An invalid later operation cannot
publish an earlier valid operation. A real pre-mutation Lua ACL error exercises
three bounded retries; a post-mutation error requires exact repair.

Decoder checks passed 10,000 executions (seed 70506), a preliminary 100,000, and
1,000,000 (seed 70507), with ASan/UBSan, max_len=16384 and timeout=5. These reuse
the unchanged V1 codec target; full-size source caps remain deterministic tests.
The isolated Redis 7.0.15 demonstration and three-repetition Release runner passed.
Raw logs, machine/source metadata, latency samples and memory measurements stay
under ignored build/. Match the final commit SHA against all six hosted Actions
jobs before calling a pushed revision verified.

## 2026-11-14 technical closeout

The bounded V1 implementation is technically ready for demonstration under the
[storage](storage.md) and [projection](projection.md) assumptions. This is a
development closeout, not a release or production acceptance. The implementation
revision is `5f433591c6e64dc0610f03c80402e944b9577073`;
[its push workflow](https://github.com/imsayon/laminar-db/actions/runs/43242173804)
was independently checked: gcc, clang, sanitizers, thread-sanitizer, formatting
and static-analysis all completed successfully on that exact SHA.

Fresh checks used a clean Release build with Ubuntu 24.04, GCC 14.2.0, CMake
3.28.3 and Redis 7.0.15 on Linux x86-64. The six focused CTest entries for changes,
projection apply/recovery, CLI and both demos passed. Both demos also ran directly,
and the README C++ example compiled and ran with a fresh synthetic DB path.
The projection runner passed three fresh repetitions with `dirty=false`, all 42
recorded source/tool hashes matching that commit, exact final convergence and
zero sequence/byte lag. Successful test/demo fixtures and Redis processes were
removed; logs and raw CSV/metadata remain under ignored `build/closeout-2026-11-14/`.

```sh
cmake --preset release -B build/closeout-2026-11-14/release -DCMAKE_CXX_COMPILER=g++-14
cmake --build build/closeout-2026-11-14/release -j4
ctest --test-dir build/closeout-2026-11-14/release -R 'changes|projection|cli|demo' --output-on-failure
bash examples/storage_demo.sh build/closeout-2026-11-14/release/src/laminar
build/closeout-2026-11-14/release/bench/laminar_projection_demo
python3 bench/projection_run.py build/closeout-2026-11-14/release/bench/laminar_projection_demo build/closeout-2026-11-14/projection-evidence
```

Use a new runner output directory when repeating. The November 12 full compiler,
sanitizer, static-analysis and decoder campaigns above are inherited evidence;
the validated source/tool bytes are unchanged. They were not repeated as fresh
local campaigns for this documentation closeout. The 24,000-operation model and
16,384-commit shared-key campaigns originate at `be98940` and remain in the final
regression suites. The million-execution decoder run used seed 70507 and generated
inputs up to 16 KiB; it does not exhaust the full record-size range.

Maximum legal batch Apply took **56.86–60.13 ms** in the three fresh repetitions,
versus **60.64–63.36 ms** in the separate November 12 run on the same source SHA.
These are workstation observations for two Puts plus 1,022 empty-key Deletes at
the source caps, not a latency bound or speedup claim. The RAM index, log-scan
startup, whole-record reads, serialized operations, offline compaction, paused-write
rebuild, exclusively owned Redis namespace and adapter-only read admission remain
explicit limits. Physical power loss and production-workload suitability remain
UNKNOWN.
