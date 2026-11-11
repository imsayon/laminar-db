# Reproduce verification

Use fresh directories under `build/` for clean gates. Ubuntu 24.04 packages for
the checked toolchain are `cmake make g++-14 clang-18 clang-tidy-18 clang-format-18
libc++-18-dev libc++abi-18-dev libclang-rt-18-dev python3 git`. Native compiler/runtime
combinations must actually support C++23 `std::expected`.

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

| Readiness item | Evidence/status |
|---|---|
| Binary CRUD, owned batches, restart, Inspect, Compact | Implemented; API/CLI/fixture/fault tests above |
| Compiler, sanitizer, static and format gates | Dated run receipt below |
| Seeded failures/shared keys and decoder fuzzing | Dated budgets and receipt below |
| Reproducible performance/space observations | [Benchmark report](../bench/results/2026-11-11.md) |
| Physical power loss / hardware certification | UNKNOWN; no experiment performed |
| Network filesystem / hostile directory mutation | Unsupported operating model |
| CDC, Redis projection and reconciliation | Deferred; no public cursor/projection API yet |
| Release tag / published distribution / deployment | Not performed by this milestone |

Completion here establishes the bounded storage milestone under the documented
filesystem assumptions. It does not establish the combined projection milestone
or production suitability.

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
