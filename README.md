# LaminarDB

A Linux/C++23 embedded binary key-value store built around one append-only log,
an owned-key RAM hash index, and one mutex. Durable single writes, atomic batches,
restart recovery, read-only inspection, offline compaction, bounded committed changes,
and one owning-process Redis projection with exact rebuild are implemented.
See [verification and readiness](docs/verification.md) for evidence and limits.

## Build and try

Requires Linux x86-64, CMake 3.25+, Make, Git, and GCC 14+ with a C++23 standard library.
Clang 18 with libc++ 18 is also checked. Tests require Redis 7+ and start isolated
disposable instances. On Ubuntu 24.04, install `cmake make g++-14 redis-server redis-tools`
and configure with `-DCMAKE_CXX_COMPILER=g++-14`.

```sh
cmake --preset release -DCMAKE_CXX_COMPILER=g++-14
cmake --build --preset release
ctest --preset release --output-on-failure
bash examples/storage_demo.sh build/release/src/laminar
```

The self-checking demo creates synthetic data below `build/demo/`, checks restart
and batch results, verifies inspection leaves log bytes unchanged, compacts, and
cleans up after success. A failed run preserves its directory.

For manual CLI use, the parent directory must already exist:

```sh
mkdir -p build/play
build/release/src/laminar put build/play/db 6100 00ff
build/release/src/laminar get build/play/db 6100       # 00ff
printf 'put\t62\tbeef\nput\t6100\t\ndelete\t62\n' > build/play/batch.tsv
build/release/src/laminar batch build/play/db build/play/batch.tsv
build/release/src/laminar get build/play/db 6100       # empty value: newline
build/release/src/laminar inspect build/play/db
build/release/src/laminar compact build/play/db
build/release/src/laminar delete build/play/db 6100
```

Every CLI invocation closes and reopens the store. Keys and values are even-length,
case-insensitive hex; `''` represents empty bytes. `get` prints lowercase hex and
a newline. A missing key exits 2. Errors go to stderr; exit 3 means invalid input
or usage, exit 1 means another operational failure. Successful commands exit 0.
`get` also opens/creates the DB, so use `inspect` to check an existing store without
creation or recovery writes.

Batch files contain `put<TAB>KEY_HEX<TAB>VALUE_HEX` or `delete<TAB>KEY_HEX`.
LF, CRLF and an unterminated final line are accepted; trailing empty hex fields
are meaningful. Blank lines, extra fields and invalid hex are rejected before
the database is opened. An empty input submits an empty batch.

## Embed

Link the CMake target `laminar::core`, or use the built static library:

```cpp
#include "laminar/db.h"

int main() {
  auto opened = laminar::DB::Open("build/api-db");
  if (!opened) return 1;
  auto& db = **opened;
  const std::string key("a\0", 2);
  if (!db.Put(key, "first").ok()) return 1;
  laminar::WriteBatch batch;
  if (!batch.Delete(key).ok() || !batch.Put(key, std::string("\0\xff", 2)).ok()) return 1;
  if (!db.Write(batch).ok()) return 1;
  const auto value = db.Get(key); // owns its bytes; NotFound differs from an empty value
  if (!value || *value != std::string("\0\xff", 2)) return 1;
  if (!db.Delete(key).ok()) return 1;
  const auto absent = db.Get(key);
  if (absent || !absent.error().IsNotFound()) return 1;
  return db.Close().ok() ? 0 : 1;
}
```

Save as `build/example.cpp`, then run from the repository root:

```sh
g++-14 -std=c++23 -Iinclude build/example.cpp build/release/src/liblaminar_core.a -pthread -o build/example
build/example
```

The API returns `Status` or `std::expected<T, Status>` and catches exceptions at
storage boundaries. Check every result. The shared DB serializes operations and
Close; callers must finish before destruction. A batch owns its inputs, retains
earlier additions after a failed addition, and can be cleared/reused. Keep it
alive and unchanged during Write. Nonempty Write publishes in batch order only
after one successful data sync; an empty batch is a healthy-handle no-op.

[Storage contract and bytes](docs/storage.md) ·
[Tests, failures, fuzzing and readiness](docs/verification.md) ·
[Reproducible benchmarks](bench/README.md)

The [committed stream and Redis projection guide](docs/projection.md) documents bounded
change cursors, guarded replay, exact paused-write rebuild and the isolated runnable demo.
