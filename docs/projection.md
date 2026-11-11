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
