# LaminarDB — Scope and Status

Aligned: 2026-10-03. This is a summary, not an implementation plan or behavioral authority.
Production code determines implemented behavior; tests supply verification evidence. Private
knowledge, design reasoning, and phase planning remain private and are not published here.

## Planned target

One append-only data log, an owned-key RAM hash index, durable atomic mutation batches,
blocking Linux I/O, one mutex-protected shared handle with exclusive process ownership,
offline compaction, and a binary-safe CLI for CRUD, batches, inspection, and compaction.
These capabilities remain planned until implemented and verified.

Accepted limits: live keys/index metadata fit in RAM, startup scans the log, operations
serialize during I/O, and offline compaction pauses access and requires temporary space.
Durability claims require stated filesystem/hardware assumptions and failure-test evidence.

## Implemented boundary

In-memory utilities, Arena, SkipList, internal keys, and MemTable remain implemented learning
components with existing tests. They need not participate in the persistent KV path and do
not prove persistence, batches, compaction, CLI behavior, or shared DB handle safety.
No implementation phase is completed or authorized by this documentation alignment.

## Superseded direction

The earlier LSM/WiscKey/async/sharded direction is superseded for V1. Historical reasoning
remains in private notes; it is not a competing active plan or a required future milestone.
Ordered scans, snapshots, SSTables/LSM levels, background compaction, WiscKey, async I/O,
Direct I/O, and sharding are outside V1. Further work needs explicit phase authorization.
