# LaminarDB Repository Instructions

## Authority and current boundary

- Production code determines implemented behavior; tests provide verification evidence.
  README.md and ROADMAP.md are descriptive summaries, never behavioral or planning authorities.
- Keep this repository self-contained. Do not add personal notes or unrelated project material.
  Establish implementation contracts and an authorized phase before coding; do not guess when
  a required contract is missing.
- Only in-memory foundations are implemented: utilities, Arena, SkipList, internal keys, and MemTable.
  Keep them as learning components; do not force them into the new KV path.
- Persistence, recovery, batches, a mutex-protected shared DB handle, offline compaction, and the
  binary-safe CLI remain planned until verified. The planned foundation review is distinct from
  existing in-memory work.
  No phase is authorized by documentation alone.
- The reduced target is one append-only data log and an owned-key RAM hash index, blocking Linux
  I/O, durable atomic mutation batches, one shared handle protected by one mutex, an exclusive
  process lock, offline compaction, and a binary-safe CLI. Follow contracts authorized for the task.
- LSM levels/SSTables, snapshots/scans, WiscKey, async I/O, Direct I/O, sharding, and background
  compaction are outside V1.

## Engineering rules

- Use C++23 conservatively; supported compiler floors are GCC 14 and Clang 18.
- Reuse existing code, then stdlib and native Linux facilities. Prefer a small concrete POSIX layer
  with the narrow fault seam required by tests; no speculative abstractions or dependencies.
- Preserve explicit Status/Result errors; exceptions must not cross future public DB APIs.
  Required allocation/length failures, including error reporting, need an allocation-free error path.
- Keys/values are binary. Own stored keys and returned values; never rely on null termination.
- Check integer bounds before allocation, offset addition, narrowing, or packing formats.
- Persistent formats must be versioned, checksummed, bounded, fuzzed, and documented before use.
  Validate checksummed headers before trusting lengths; never serialize native structs or padding.
- Sync before acknowledging mutations. Prevent partial batch visibility; disable data operations
  after ambiguous append/sync or post-sync index failures until reopening. Explicit Close reports
  errors; destructor cleanup is nonthrowing and best-effort. Do not retry Linux close errors.
- Offline replacement requires temp-file sync, same-directory rename, and directory sync under
  the stable process lock. Never remove LOCK, promote orphan temps, or silently repair corruption.
- Arena has one owner. SkipList/MemTable have exactly one writer and may have concurrent readers
  while their owner keeps storage alive. Do not call them multi-writer or generally lock-free.
- The planned DB mutex protects operations/state/Close. Callers finish before handle destruction.
- No performance or durability claim without reproducible evidence and stated limits.

## Required workflow and validation

1. Inspect Git state, relevant contracts/tests, and every affected caller; preserve unrelated work.
2. Read the authorized phase and prerequisites. State persistence/concurrency invariants and
   failure behavior before implementation. Stop for material design contradictions; resolve routine choices.
3. Add the phase's required boundary, malformed-input, model, resource-failure, crash/concurrency,
   and parser-fuzz checks. Fakes prove control flow; process-kill tests do not certify power loss.
4. For implementation, run clean out-of-tree presets below; record exact evidence privately.
   Required local gaps need verified CI evidence. Never count unavailable checks as passed.
5. Review the diff and update only relevant task/phase status and audit evidence. Complete a phase
   only when every required exit gate has evidence; do not start another phase without authorization.

```bash
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
cmake --preset thread-sanitizer
cmake --build --preset thread-sanitizer
ctest --preset thread-sanitizer
cmake --preset clang-tidy
cmake --build --preset clang-tidy
```

Also require formatting/static-analysis evidence for implementation. Docs-only changes require
scope, consistency, link, and diff checks; they do not prove engine gates.

## Repository hygiene

- Build only below build/; generated artifacts are disposable and must not be committed.
- Preserve dirty/unrelated work. Do not reset, stash, overwrite, or force-push it.
- Keep commits limited to implementation, tests, build configuration, and relevant repository docs.
  Do not add unrelated files or task prompts.
- Use primary sources for technical claims and date time-sensitive guidance.
- Commits, pushes, PRs, deletion, merge, and release require task-scoped authorization. Stage only
  authorized files; a phase-scoped PR does not authorize merge, release, or the next phase.
