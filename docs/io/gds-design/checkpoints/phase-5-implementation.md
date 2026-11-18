# Phase 5 implementation record (working tree on d11c9de)

## Implemented contract

- Engine device-source writes require non-legacy mode, async_writes=true,
  async-write symbols, a ready engine that accepts writes and a recorded source fence, plus
  verified-direct policy with write qualification or an explicit
  operator-qualified override with allow_writes=true. Read-only overrides,
  test-only read-probe promotion, compat/unknown/unsupported policies and host
  sources do not become async write bodies.
- Slot-mode registrations remain the default (AM-9/AM-11). Aligned bodies use
  fence -> D2D -> observed staging event -> interval/QD -> WriteAsync ->
  observed I/O event -> host validation. Lazy stripes own separate credits.
- Exact edges use one granule-sized pinned cell per lane allocated at bootstrap,
  fence -> D2H -> observed event -> blocking-pool buffered pwrite. Cells return
  to a size-aware process-lifetime bank; no per-operation pinned allocation or
  cudaFreeHost. Unstarted/refused bodies first retain the entire source in a
  device slot, then stream through those small cells on the blocking pool using
  O_DIRECT pwrite when aligned. Known-progress EINVAL/EOPNOTSUPP continuations
  switch to buffered pwrite without replaying a prefix.
- Known positive aligned suffixes reuse the same lease and registration-relative
  offset. Unaligned suffixes D2H only the remaining bytes and use buffered pwrite.
  Zero/negative/malformed/unknown progress, submit errors, stuck calls and
  recovery never replay writes. Recovery synchronizes only the owned lane;
  failed recovery retains/quarantines work without falsely settling its future.
- Write futures are always host-observed and all request siblings must be
  physically quiescent, including on errors. note_written advances only for
  successful physical ranges. Success is not durability.

## Inode sequencing and intervals

file_write_state is a weak process registry keyed by descriptor-derived (st_dev,
st_ino), with identity-checked eviction on final destruction. Native handles lazily cache its state. Each library write request
registers a monotonically increasing sequence BEFORE reactor dispatch, including
legacy and host writes, so another handle's engine work/flush cannot miss queued
prior work. Split children retain the same ticket until all children retire.

Before inode activation, legacy dispatches take only nonserializing activity
guards. Activation waits for those guards before engine interval admission;
later legacy dispatches take intervals. Release notifies engine/reactor waiters;
synchronous host writes use a condition variable. Quarantine retains intervals
and tickets, reports a gauge/warning and makes conflicting writes/flushes unable
to complete. Parked legacy writes do not consume the reactor in-flight limit.

After activation, cross-mode accesses sharing any granule serialize; same-mode non-overlapping
byte ranges can coexist. Overlapping byte ranges serialize as well. Legacy
head/body/tail operations conservatively hold a mixed interval for the entire
slice (an edge-only legacy slice uses buffered mode). Legacy transport selection,
thresholds, copies and pwrite/cuFile policy remain unchanged; a conflict parks
the slice rather than waiting on a reactor thread. Synchronous host writes may
wait on their calling thread, as their API is blocking.

Once a file has queued potentially engine-capable write work (a capability-only
pre-dispatch snapshot; actual policy routing stays on the reactor), flush on any cuFile handle
captures its current sequence, waits by callback for all prior tickets, then
queues fdatasync on the blocking pool. Later sequences do not gate that callback.
Files never served by the engine keep legacy flush. This tracking is per inode,
not per context/device. Process-wide flush_waits is explicitly not additive
across engine snapshots.

## Evidence and limits

The first 16 MiB of completed async writes checks the injected evidence source;
absent attributable write evidence revokes write admission and logs once, but
does not revoke reads. A standing write-enabled operator override is exempt.
Both production evidence implementations still return unavailable (AM-11).
Thus production automatic direct-write qualification is NOT claimed: in practice
only explicit write-enabled overrides make this path reachable. Independently,
non-transient negative completions revoke writes even for overrides (except
ENOSPC/EACCES/EDQUOT), without replay; a sentinel requires owned-lane recovery.

Diagnostics expose async/direct-pwrite/buffered-pwrite physical bytes, short
writes, buffered suffixes, engine granule-wait episodes/time and process-wide
flush waits, quarantined intervals, engine pinned-cell bytes and process-wide
pinned-bank bytes including cached banks. Source-staging admission is bounded across the mount; an unready
write cursor retains at most one lane. AM-15 polling work remains Phase 6.

## Validation

Simulator tests mirror completed writes to a borrowed real fd before publishing
completion, permitting full pread verification of combined async bodies and
buffered edges/suffixes. Real compat libcufile is also exercised with slot
registrations. Benchmark B-6 supports legacy vs real-libcufile async, not the
simulator's single-image multi-file write model; retained NEW spill files permit
inspection. See tools/gds/README.md for reproducible NOT GDS smoke commands.

## Initial, pre-review validation (historical; superseded below)

- Requested three-target release build passed without compiler warnings.
- Full cucascade_io_tests: **883 cases, 882 passed, 1 existing kTLS skip**;
  **3,036,469 assertions passed**. This includes the original legacy write tests.
- Final randomized [engine] loop: **10/10 passed**, **98 cases / 3,545 assertions**
  each. Seeds: 28991, 9726, 11227, 1414, 17949, 1653, 4944, 15478, 23243, 18748.
- An earlier loop exposed a test-only timing assumption: live operations can still
  be staging when the breaker opens. The not_submitted test now waits for storage
  tokens proving queue publication and disables incidental watchdog expiry.
  Rechecked failing seed 21976 passed before the final ten-run loop.
- pre_commit passed for all implementation changes (clang-format, cmake-format,
  cmake-lint, codespell and other applicable hooks).
- Verified /tmp compat spill smoke, libcufile 1170, QD 4, 1 MiB slots, 16 MiB arena,
  2 x (8 MiB minus 74-byte) writes per pass, >=1 second measured:
  - legacy: 4,546,585,428 bytes / 1.002 s, **4.227 GiB/s**, all legacy bytes;
  - async operator override with TEST-ONLY compat admission: 3,154,088,784 bytes /
    1.002 s, **2.931 GiB/s**; 3,151,036,416 async bytes + 3,052,368 buffered-edge
    bytes, zero direct-pwrite fallback bytes and zero legacy bytes.
  These constant-source overwrite samples are **NOT B-6 evidence** and **NOT GDS**;
  they are historical warm/uncontrolled-cache smoke numbers, not qualification
  or a performance improvement claim. Legacy physical transport internals remain
  uninstrumented; their logical-byte total is reported explicitly.

## Phase 5 review fixes and final validation

AM-16 in `../06-design-amendments.md` is authoritative; per-ID resolutions are in
`../reviews/phase-5.md`. P5-R1–R10 and R12–R17 are fixed. R11 retains the explicit
§7 #29 conservative stuck-write failure policy; default mode remains legacy.

- All three requested release targets built successfully.
- Full suite: **893 cases, 892 passed, 1 existing kTLS skip**;
  **3,037,748 assertions passed**.
- Final randomized `[engine]` loop: **10/10 passed**, **107 cases / 4,838 assertions**
  each; seeds 29255, 17657, 5128, 14161, 28569, 17208, 9675, 28985, 3641, 9504.
- An earlier new test incorrectly required async transport after a transient
  error despite a legitimate temporary zero-capacity fallback. It now asserts
  retained eligibility, exact physical-byte accounting and full read-back;
  the failing seed 7839 passed before the final loop.
- Spill smoke passed full before/after verification for two measured passes:
  legacy new and overwrite, and real-libcufile async new with TEST-ONLY compat
  admission; all used flush and advisory cache eviction. These are NOT GDS or
  acceptance throughput evidence.
- Pre-commit passed all applicable hooks on changed implementation files.

No commits or src/cudf edits. The pre-existing step-6-impl.md worktree edit and
its unrelated editor lock were left as found.
