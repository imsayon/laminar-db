# Storage contract (V1)

Reviewed against [API](../include/laminar/db.h), [engine](../src/storage/db.cpp),
[codec](../src/storage/format.cpp), [file layer](../src/storage/file.cpp) and
[CLI](../src/cli/main.cpp) on 2026-11-11. [Tests](verification.md) establish the
stated evidence boundary. No format migration is provided.

## Operations and failures

Open creates only the leaf DB directory; its parent must exist. The directory is
dedicated to the store. `data.log` is authoritative; `LOCK` is a stable inode.
`create.tmp` and `compact.tmp` are reserved installation artifacts. Never remove
LOCK or promote a temp file yourself. Unknown files are preserved. Identifiable
stale temps can be removed by the engine under LOCK; an unidentifiable conflict
returns AlreadyExists. New directories use mode 0700 and files 0600, subject to
umask; existing permissions are preserved. Reserved files and the leaf directory
reject symlinks; this is no sandbox against hostile parent-path replacement.

One owning handle/process has a nonblocking exclusive flock. A second Open,
Inspect or Compact returns AlreadyExists while it is held. Get returns an owned
value; Delete writes a tombstone even for an absent key. Close releases ownership,
reports descriptor errors, and repeated Close is OK. Destructor cleanup cannot
report errors. No data operation succeeds after Close.

| Bound | Bytes/count |
|---|---:|
| Key | 65,536 bytes |
| Value | 8,388,608 bytes |
| Operations per batch | 1,024 |
| Batch payload, including 9-byte operation prefixes | 16,777,216 bytes |
| Encoded frame, including 32-byte header | 16,777,248 bytes |
| CLI batch input | 33,619,968 bytes |

Empty keys/values are valid. Paths reject empty input and embedded NUL. Offsets
are checked against signed 64-bit `off_t`; sequences cannot wrap. Nonempty writes
at UINT64_MAX return Full. Live keys and index metadata must fit RAM. Each Get
reads and validates its entire enclosing frame, then copies the selected value;
small values in large batches can be expensive. Startup scans the whole log;
Close does not persist an index. There is no record cache, scan or snapshot API.

| Status | Meaning |
|---|---|
| NotFound | Absent key, expired change history, or missing existing-store data for Inspect/Compact |
| InvalidArgument | Caller/path/CLI/size limits |
| Full | Allocation failure, exhausted sequence or offset space |
| Corruption | Invalid stored bytes or unexpected short read |
| NotSupported | Intact recognized file preamble with unsupported version/flags |
| AlreadyExists | Busy lock or reserved-temp conflict |
| IOError | Syscall failure, closed handle, or reopen-required state |

Messages are diagnostic and may be empty under allocation pressure. Pre-append
encoding/allocation failure leaves the handle healthy and state unchanged.
Append, sync, or post-sync index-publication failure disables data operations
before unlocking: **an error may follow a durable commit**. Close and reopen,
then reconcile the value; retry is no exactly-once protocol. A read I/O or
validation error also disables the handle. Get allocation failure leaves it
healthy. Limits apply to caller input and disk decoding in Release builds too.

## Durability and recovery

Mutation success follows append, `fdatasync(data.log)`, then ordered index
publication under the mutex. New-file creation syncs the temp file, installs it
without replacing an existing data.log, and syncs the directory and parent before
Open succeeds. Every successful Open validates, trims a permitted incomplete
mutation suffix, syncs the log, and syncs its directory and parent. A complete
unacknowledged mutation can be recovered and become committed at that sync.
Inspection takes LOCK and validates without creating, syncing or trimming data.
Its JSON fields are format_version, log_bytes, record_count, live_keys and
incomplete_tail_bytes. record_count counts mutation frames after the baseline.

| EOF/input | Outcome |
|---|---|
| Complete validated baseline and mutation frames | Replay all operations in physical order |
| 1–31 trailing bytes matching the available `LMBT` magic prefix | Open trims suffix; Inspect reports it |
| Valid next header with EOF inside declared payload | Same incomplete-suffix policy |
| Invalid complete header/payload, bad CRC, wrong sequence, junk suffix | Corruption; preserve bytes |
| Short/invalid file header, checkpoint rows or seal | Fail closed; never treat as append residue |

At maximum sequence any suffix is corruption. There is no search for a later magic
and no silent repair of complete corruption. Deliberate truncation or loss of an
entire valid suffix cannot be detected without independent trusted state.

Compact requires an existing store and no owning handle. It validates source
bytes, ignores only a permitted incomplete append suffix, writes one baseline
Put per live key into exclusive `compact.tmp`, backfills the header, validates the
whole output, fsyncs and explicitly closes it, renames over data.log, then fsyncs
the directory before success. It preserves identity and frontier W, setting
baseline/floor B=F=W even when all keys were deleted. The next mutation is W+1.
Before rename, errors preserve old authority; a directory-sync error after rename
reports uncertain installation. A later cleanup/close error can report failure
with replacement already installed. Open ignores orphan compact.tmp.
Compaction discards mutation history and requires extra temporary space roughly
equal to output size plus filesystem overhead. Grouped live records can grow
when rewritten one row per key. See [measurements](../bench/README.md).

The supported operating model is a local Linux filesystem with working flock,
append, data/file/directory sync and atomic same-directory rename, plus storage
that honors those syncs and preserves previously synced bytes. Network
filesystems, hostile concurrent modification, media corruption and hardware
lying about persistence are outside that model. Process SIGKILL leaves the OS
page cache alive. **Physical power-loss behavior is unverified.**

## Exact wire layout

All integers are unsigned little-endian; no native structs or padding. CRCs are
unmasked CRC32C, seeded with zero. The file is a 64-byte header, baseline frames,
a mandatory 12-byte seal, then mutation frames.

| File-header offset | Field |
|---:|---|
| 0 | 8-byte `LMNRDATA` |
| 8, 12 | u32 version=1, u32 header bytes=64 |
| 16 | 16 opaque nonzero identity bytes from getrandom |
| 32, 40, 48 | u64 baseline sequence/floor, row count, encoded row bytes |
| 56, 60 | u32 flags=0, u32 CRC of bytes [0,60) |

A zero baseline sequence requires zero rows; nonzero baseline permits zero rows.
Each baseline frame is one Put at sequence zero; duplicate baseline keys fail.
Its seal is `LMNRSEAL` plus u32 CRC of concatenated row **payloads**, excluding
frame headers. An empty baseline still has the seal, with CRC zero: a fresh or
all-deleted compacted file is 76 bytes.

| Frame-header offset | Field |
|---:|---|
| 0 | 4-byte `LMBL` baseline or `LMBT` mutation magic |
| 4, 8, 12 | u32 payload bytes, operation count, flags=0 |
| 16 | u64 sequence |
| 24, 28 | u32 payload CRC, u32 CRC of header bytes [0,28) |

An operation is u8 tag (1 Put, 2 Delete), u32 key bytes, u32 value bytes, key,
value. Delete requires zero value bytes. Validate bounded header lengths and
CRC before allocating payload, then validate all payload bytes before applying.
Mutation sequences begin at B+1 and increase by one per nonempty batch, regardless
of its operation count. Repeat-key operations apply in order. The
[golden fixtures](../tests/v1_fixtures.h) and [parser tests](../tests/test_format.cpp)
are executable byte examples. Internal format definitions are not a public API.

## Committed changes

`LatestSequence()` returns the persistent database identity and current u64 sequence.
`ReadChangesAfter(cursor, max_batches, max_bytes)` uses an exclusive cursor and returns
owned, consecutive, whole mutation batches. Both limits must be positive; bytes count the
32-byte frame header plus encoded payload. An empty page with `required_bytes > 0` means
the first batch does not fit. An empty page with zero required bytes means the cursor is
at the frontier. Any read/validation error returns no page.

Foreign/future cursors return InvalidArgument. A cursor below the replay floor returns
NotFound (history expired); compaction sets that floor to the captured frontier, including
an all-deleted store. Baseline rows are state, not changes. Persist identity and sequence,
never file offsets. Complete recovered frames become visible only after Open syncs the
recovered frontier; this includes writes whose caller did not receive success.
