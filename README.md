# LaminarDB

LaminarDB is a Linux/C++23 embedded key-value storage learning project.
The planned store uses one append-only data log, a RAM hash index, durable atomic mutation
batches, one mutex-protected shared handle, offline compaction, and a binary-safe CLI.

## Current behavior

The repository implements in-memory learning components: binary-safe Slice, Status/Result,
encoding and CRC32C utilities, Arena, SkipList, internal keys, and MemTable. Tests cover these
components, including randomized MemTable model parity and one writer with concurrent readers.
They remain learning components and need not participate in the planned persistent KV path.

Persistence, recovery, atomic batches, the shared DB handle, offline compaction, and CLI are
**planned, not implemented or verified**. Existing tests do not prove database durability or
production suitability. Compiler and sanitizer results require dated evidence; configured CI
jobs alone do not establish a pass.

## Build and test

Requirements: Linux x86-64, CMake 3.25+, GCC 14+ or Clang 18+, and a C++23 standard library.

```bash
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

Other build/test presets: `release`, `clang-debug`, `sanitizers`, and `thread-sanitizer`.
Static analysis uses the `clang-tidy` configure/build preset. Generated output belongs under
`build/` and must not be committed.

## Scope and ownership

Production code is the authority for implemented behavior; tests provide verification evidence.
These documents summarize implemented behavior and the planned target; they do not authorize
implementation. [ROADMAP.md](ROADMAP.md) summarizes scope/status; [AGENTS.md](AGENTS.md) guides
repository work.

The target accepts RAM-bound live keys, log-scan startup, serialized operations, and offline
maintenance requiring temporary disk space. Ordered scans, snapshots, SSTables/LSM levels,
background compaction, WiscKey, async I/O, Direct I/O, and sharding are outside V1.
