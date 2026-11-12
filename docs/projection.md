# Owning-process Redis projection

The adapter uses a local Unix socket and one exclusively owned standalone Redis
namespace: `laminar:<name>:data` and `laminar:<name>:meta`. Data is a hash of binary
keys/values. Names contain 1–64 ASCII letters, digits, `_`, `-`, or `.`. Configure
no eviction, no TTL and no other writers. All application reads must go through
`RedisProjection::Get`; raw Redis readers are outside the admission promise.

`Open` starts unready on every owner restart, regardless of Redis's ready marker.
An admitted Get holds the same mutex as Apply; acquiring that mutex drains prior
reads before dispatch. Unresolved timeout, outage, script/resource error or an
unexpected marker closes admission until verified recovery. Redis script atomicity
[prevents interleaving](https://redis.io/docs/latest/develop/programmability/), but
an error after a write can leave partial data. This adapter promises neither
rollback nor exactly-once delivery.

Feed Apply only whole batches returned by the source's ReadChangesAfter. It
validates identity, predecessor, all types, operation/key/value/payload limits and
encoded size before mutation. Lua repeats boundary validation, sets ready=0,
applies absolute Put/Delete operations in order, and updates sequence/ready last.
Database identity is 32 hex characters; sequences are exactly 16 lower-case hex
characters, so Lua never converts u64 sequences to doubles. Earlier/equal batches
are duplicates only inside the verified owner epoch; gaps fail explicitly.

There is one in-flight batch and no internal queue. Pending source bytes are at
most 16,777,248 (16 MiB payload plus frame header); request buffers and Lua/Redis
copies cost additional memory. Each flat RESP2 command uses a fresh Unix connection
and a two-second deadline, including send/read. Apply performs at most three
identical dispatches with 10/20/40 ms backoff, classifying the marker before every
retry. A successful marker can resolve a lost reply; ready=0 or an unexpected
cursor requires repair. The source limits are preserved and atomic batches are
never split. No network authentication, Cluster, generic connectors or durable
consumer journal are provided.

## Recovery and maintenance

On startup call `Reconcile(source)`. It closes/drains projection reads, pauses source
writes with the existing DB mutex, captures W, requires matching identity/cursor,
and compares every binary key/value and exact cardinality at W. Missing, stale or
foreign metadata, missing/wrong/extra data, a wrong Redis type, or interrupted work
leaves the handle unready. There is no trusted stale-ready shortcut.

Explicit `Rebuild(source)` takes the same two locks, removes only the two owned
keys, streams one owned source row at a time, then repeats the exact comparison.
It publishes identity, cursor and ready only after comparison succeeds. A crash
mid-rebuild starts closed and requires repair again. An empty live set still
publishes its nonzero source watermark. Source writes and projection reads resume
when maintenance releases the locks; they remain independent during ordinary
replay. Source writes can continue while Redis is unavailable, and offline
compaction can expire the projection's retained history.

The export is internal, uses the existing index/validated value reader, and holds
one source row plus the enclosing frame scratch buffer. A large enclosing frame
can be reread per key during maintenance. RAM must hold the live index and derived
Redis dataset; rebuild deliberately pauses writes for its full duration. Source
Get and Close also wait for that mutex. There is no total dataset size or
maintenance duration guarantee.

## Reproduce the slice

Install Redis 7+ (`redis-server`, not a production instance), then build with tests:

```sh
cmake --preset release
cmake --build --preset release
ctest --test-dir build/release -R 'changes|projection' --output-on-failure
build/release/bench/laminar_projection_demo
python3 bench/projection_run.py build/release/bench/laminar_projection_demo build/projection-evidence
```

The executable starts and cleans its own Redis with no TCP port, persistence or
shared data. It demonstrates durable commits, bounded ordered replay, duplicate
delivery after owner restart, history expiry after compaction and exact rebuild.
It records 64 apply/replay samples after eight warmup batches, sequence/byte lag,
per-batch peak pending bytes, reconciliation/rebuild times, maximum-batch apply latency, an
observed concurrent Get's blocking time, owner RSS and Redis memory/peak allocation.
The runner performs three fresh repetitions and stores raw CSV, source hashes,
revision, seed, compiler flags and environment under ignored build/. It reuses the
storage runner's percentile calculation. Maximum-batch timings have three samples;
the shape is two Puts plus 1,022 empty-key Deletes, at all four source size/count caps.
These timings do not establish a tail bound or service-level target. RSS includes allocator,
source batch/page copies and runtime costs; it is not exact queue heap usage.

[Apply tests](../tests/test_projection.cpp) exercise real Lua partial failure and
owner death before its reply. [Recovery tests](../tests/test_projection_recovery.cpp)
cover exact repair, ghost/deleted keys, lost checkpoint/dataset, interrupted rebuild,
source/read admission, and owner death followed by exact convergence. CI requires
Redis and runs these tests; it does not silently skip a missing service. Process
kills are not physical power-loss certification.
