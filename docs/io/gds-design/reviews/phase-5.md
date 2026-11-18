# Phase 5 review: async device-source writes, `file_write_state`, flush sequencing

- Branch `io/cufile-async-engine`. I reviewed the working tree against HEAD `d11c9de`: `git diff HEAD` plus the untracked files.
- Scope:
  - **New:** `include/cucascade/io/cufile/file_write_state.hpp`, `src/io/cufile/file_write_state.cpp`, `test/io/cufile/test_engine_write.cpp`, `checkpoints/phase-5-implementation.md`.
  - **Modified:**
    - `include/cucascade/io/cufile/{config,cufile_ioctx,cufile_reactor,device_engine,mount_policy,operation,runtime}.hpp`
    - `include/cucascade/io/types.hpp`
    - `src/io/cufile/{device_engine,cufile_reactor,cufile_ioctx,mount_policy,runtime}.cpp`
    - `src/io/CMakeLists.txt`, `test/CMakeLists.txt`
    - `test/io/cufile/{test_mount_policy,test_runtime_params,transport_simulator}.*`
    - `tools/gds/{cufile_engine_bench.cpp,README.md}`
- Normative inputs:
  - `06-design-amendments.md` (all)
  - `05-final-design.md`: FR-2, FR-22–FR-24, FR-30, §4.10, §6.11, §6.13, §7
  - `reviews/phase-3.md`, `reviews/phase-4.md`
  - `AGENTS.md`

## Verification performed

All commands ran one after another. I modified no source; this file is the only one I wrote.

| Check | Result |
|---|---|
| `pixi run cmake --build build/release --target cucascade_io_tests cucascade_cufile_engine_bench -- -j 20` | Up to date (`ninja: no work to do`) |
| Full `./build/release/test/cucascade_io_tests` | **883 cases: 882 passed, 1 skipped** (the existing kTLS skip); 3,037,034 assertions; 1 min 36 s |
| `"[engine]" --order rand`, seeds 3101–3110, run sequentially | **10/10 pass**, 98 cases / 3,545 assertions each |

## Confirmed correct, no finding

**No replay of a write with unknown progress**
- `validate_write` (`device_engine.cpp:653-701`) fails the operation without any resubmission when the result is:
  - zero, negative or oversized (`done > size`);
  - the sentinel;
  - after the watchdog flagged the call (`unknown_progress`).
- `submit_failed` no longer sets `retry_after_recovery` for writes (`:1054`). `failed`/recovery of a write always ends in `finish(false, lane_synchronized)`.
- `sync_write` returns either the full count or a negative error, never a silent partial count. The fallback path therefore cannot replay a prefix either.
- `not_submitted` is published only before the transport call. `drain_refused_commands` pops only ring entries that a submitter has not taken. A queued write cannot be both submitted and redirected to fallback.

**Short-write suffix arithmetic**
- `end = file_offset + done`. The new arguments are:
  - size `physical.end() - end`
  - file offset `end`
  - buffer offset `buffer_offset_to_pass() + (end - physical.offset)`

  These are correct for both slot and arena registrations, and the same lease and QD path are reused.
- An unaligned `end` re-stages only the remainder from the **slot**, never from the user source: `stage_write` with `n.staged` uses `staging->data() + (file_offset - physical.offset)` into scratch, then a buffered `pwrite`.
- The simulator mirror writes exactly `item.result` bytes, so the 4096 and 4103 short-write tests do prove suffix correctness by full read-back.

**Granule serialization**
- Cross-mode overlap is checked by granule; same-mode overlap by byte (`file_write_state.cpp:73-80`). Direct ranges are always whole granules, so "same mode, disjoint bytes" can never put two O_DIRECT writes in one granule.
- The direct-mode interval stays held while a buffered suffix or EINVAL fallback writes through the page cache. A buffered writer on that granule still conflicts (cross-mode), and a direct writer would overlap bytes. The mode mismatch is therefore harmless.
- Within one slice, head/body/tail and the op-size cuts never share a granule. Adjacent slices' edges are both buffered and byte-disjoint.
- Legacy slices hold a conservative `mixed` interval for the whole synchronous `dispatch_write`. All legacy device and host writes (staged D2H, `cuFileWrite`, O_DIRECT host bodies) finish inside that call (`cufile_reactor.cpp:1901-2018`), so the interval and the ticket in `slice.backend_owner` cover the physical I/O.
- `host_write_io` holds both a ticket and an interval around the synchronous reactor write.

**Ordering facts I verified**
- Intervals are acquired after staging and released on the cleanup executor, after `finish`. A later overlapping write submitted after the future resolved therefore waits for the old interval.
- Tickets are taken in `mixed_writev_async_io` before validation, splitting and dispatch. `split_write_slices` copies `backend_owner` (`auto part = slice`), and engine cursors retain the ticket until cursor cleanup.

**`note_written`**
- Called only in `finish(success)`, per successful physical piece (`:588-591`).
- Failed, cancelled, sentinel, unknown-progress and partially failed `sync_write` paths never advance it. The test matrix checks `size() == 0` on every no-replay path.

**Flush**
- `capture()` returns the last assigned sequence. `when_retired` waits while `min(pending) <= seq`. `retire()` fires every waiter whose captured sequence is below the new minimum, and runs callbacks outside the mutex.
- Later sequences never gate the callback. `fdatasync` runs on the blocking pool; no thread waits.
- Engine-path flush is stronger than the public contract ("writes resolved before this call"): it also covers writes that were in flight when the flush was captured.

**Source and resource lifetime**
- The user source is read only in the first `stage_write`. That function enqueues `cudaStreamWaitEvent(lane, fence)` before the D2D/D2H. The gate test proves this for both bodies and edges.
- Every later path reads the slot, never the source: not-submitted, eligibility drop and buffered suffix.
- Futures settle only after the I/O event is observed, after `sync_done`, or after lane synchronization.
- The slot, the lane-indexed scratch cell, the interval and the lane are released only in the cleanup task, after `finish`. A quarantined operation keeps all of them.
- Pinned scratch is allocated at bootstrap and never released per operation (but see P5-R7 for its size).

**Routing honesty**
- `write_eligible` requires all of the following (`mount_policy.cpp:351-359`):
  - non-legacy mode;
  - `async_writes`;
  - either `verified_direct` with profile qualification and no failed evidence, or an operator override with `allow_writes`.
- Test-only read-probe promotion yields `operator_qualified` without `_operator_override`, so it is never write-eligible.
- `engine_possible(async_auto, writes)` requires direct-capable properties, and the reactor additionally requires the async-write symbol group.
- Production evidence sources still return "unavailable", so automatic production write qualification is unreachable. The checkpoint states this honestly.

**Slice integrity**
- A prepared slice is wholly engine or wholly legacy.
- Inside the engine, a cursor's pieces fall back only to the engine's own blocking-pool `pwrite`, never back to the reactor.
- Metadata pressure and pre-handoff misses send the whole slice to the unchanged legacy path. Parked legacy slices are redispatched with `stream_ready = true`, so they stay legacy.

**Cancellation and failure**
- Cancellation before submission issues no storage call (tested). A submitted write drains and reports its real result (tested).
- Staging wait/copy/record failures go through owned-lane recovery before any release (tested).
- A failed sibling resolves the request only after every other submitted sibling is quiescent (tested).

---

## Findings

### BLOCKING

#### P5-R1: The `file_write_state` registry never evicts, so every written inode leaks forever, including in the default legacy mode

`src/io/cufile/file_write_state.cpp:24-40`; callers at `src/io/cufile/cufile_ioctx.cpp:145-160` and `cufile_reactor.cpp:1385-1386`

**Problem**
- `get()` stores a strong `shared_ptr<file_write_state>` in a process-static `std::map<{dev, ino}, …>`. Entries are never erased.
- Every writable cuFile object that writes takes this path through `mixed_writev_async_io` or `host_write_io`. That includes `mode=legacy` contexts, i.e. the default DISK-tier spill path.
- Each entry costs roughly 300 B: a mutex, set, two maps, atomics, the control block and the map node.
- A spill-heavy process that creates and deletes millions of files grows without bound: about 5 GB after roughly 17 M files.
- Inode numbers are also recycled after unlink. A new file then inherits the old state, including the sticky `_engine` flag, which switches its flushes to the engine path.

**Fix**
- Make the registry hold `std::weak_ptr`.
- On lookup, reuse the entry if it locks; otherwise replace it.
- Erase entries from the `file_write_state` destructor, or with a custom deleter, under the registry mutex.

This is safe: a live state is always reachable through some open `io_object`, ticket or flush waiter, and an open inode cannot be recycled.

Add a regression that opens, writes, closes and unlinks N files and asserts the registry size returns to its baseline. Expose a test-only count.

### SHOULD

#### P5-R2: The default `mode=legacy` path now pays for, and is changed by, inode sequencing (FR-35)

`cufile_ioctx.cpp:127-160`, `cufile_reactor.cpp:1384-1442`, `file_write_state.cpp:69-70`

**Problem.** With the engine disabled, every write request now:
- runs `fstat` plus a global registry lookup;
- takes a ticket;
- acquires a per-slice interval on a shared per-inode mutex.

This changes the default path in three ways:
- **Serialization.** Slices whose aligned body is non-empty take a `mixed` interval. Two concurrently dispatched legacy slices that merely share a granule are now serialized, although they ran in parallel before and the public contract explicitly allows it ("disjoint ranges … also when they share a block or page").
- **Spurious parking.** `try_acquire` uses `try_to_lock`. Any momentary contention is reported as a conflict:
  - a reactor parks the slice in `_write_waiting` and only redispatches it on a later loop, after `poll_interval`;
  - synchronous `host_write_io` sleeps 100 µs per failed attempt.

  A large write split across four reactors hits this mutex from four threads at once.
- **No measurement.** FR-35 says legacy "reproduces current behaviour exactly", and NFR-2 bounds compat regressions. Nothing measures either.

**Fix**
- Engage intervals only while a write-capable engine exists in the process. Set an atomic flag in `device_engine::for_device` when `async_writes` is on. Before the flag is set, no engine write exists to conflict with.
- To close the bootstrap window, keep taking tickets, which are cheap, and have the engine's first activation of an inode wait resource-free for `when_retired(capture_at_activation)` before admitting its first piece.
- Make conflict detection blocking-lock (the critical section is tiny), so a lock miss is never reported as a conflict. Keep the "park instead of wait" behaviour only for real overlaps.
- Add a legacy-mode regression: concurrent granule-sharing slices on four reactors, asserting no parking and identical bytes.

#### P5-R3: A configuration mismatch between contexts turns valid writes into failures instead of routing them to legacy

`device_engine.cpp:1984`, `cufile_reactor.cpp:1031-1067`, `device_engine.cpp:2040-2049` ("first configuration wins")

**Problem**
- The engine is process-shared per device and frozen at the first bootstrap.
- Suppose context A, with `async_writes=false`, boots the device-0 engine, and context B, with `async_writes=true` and an `allow_writes` override, writes to a mount whose policy B created. Then:
  - `engine_route` accepts the write (B's config and B's policy);
  - `hand_off` throws `invalid_argument("invalid engine_slice")` because `!e.config.async_writes`;
  - the user's write fails.
- The engine has no write scratch in this case, so the guard is necessary. The routing should still never pick a target that will refuse.

**Fix**
- Expose `device_engine::accepts_writes()`, meaning `config.async_writes` and scratch allocated.
- Check it in `engine_route` (and `engine_write_candidate`) so the slice stays legacy.
- Add a two-context test.

#### P5-R4: A completed async write with a negative result never demotes the mount, so with an override every later write fails too

`device_engine.cpp:653-661`

**Problem**
- For reads, a negative completion opens the circuit and reissues through sync fallback. For writes, a negative `bytes_done` (for example `-EINVAL`/`-EOPNOTSUPP` for a file or layout async write cannot serve) fails the piece correctly, without replay, but changes nothing else.
- The mount stays write-eligible. The operator override is exempt from evidence revocation, so nothing will ever revoke it.
- Every later write on that mount therefore fails the same way, although the legacy path would serve it.

**Fix**
- Keep failing the affected piece; that is correct, since its progress is unknown.
- After a non-transient negative write completion (anything other than ENOSPC/EACCES/EDQUOT), revoke write eligibility for the mount (a write circuit or cooldown, or `_write_evidence_failed`) so later writes route to legacy.
- Log once.
- Test: two sequential writes with a `-EOPNOTSUPP` completion fault on the first. The first fails, the second succeeds via legacy, and `write_calls == 1`.

#### P5-R5: A write completion that still holds the sentinel skips owned-lane recovery (§7 #17)

`device_engine.cpp:658-661` versus reads at `:773-776`

**Problem**
- Reads route `done == pending_sentinel` to `fail()`, which quarantines the lane and synchronizes it on the recovery thread, as §7 #17 requires: "the stream state is suspect".
- Writes treat the sentinel like any other bad count and call `finish(…, io_observed)`. The cleanup task then releases the slot, lane, scratch and **granule interval**.
- If the event signal was not a faithful completion proof, which is the very situation the sentinel indicates, a later write to the same granule could be overtaken by the stale DMA or compat `pwrite` from the reused slot. AM-9's silent-corruption history makes compat-path trust a live concern.

**Fix**
- In `validate_write`, route `done == pending_sentinel` to `fail(n, io_error)`, matching reads. The future then settles after lane synchronization, or the operation is quarantined with the interval held.
- Extend the existing sentinel test to assert `recoveries == 1`.

#### P5-R6: Interval conflicts are polled, never notified, and a quarantined holder makes the polling permanent

Sites:
- `device_engine.cpp:733-752` with `:1858` (100 µs reaper wakeups)
- `cufile_reactor.cpp:2115-2124` (redispatch every loop; `_write_waiting` counts toward `max_inflight`)
- `cufile_ioctx.cpp:140-141` (100 µs sleep loop)

**Problem**
- `release()` wakes nobody, so a waiter only learns of a released interval by polling.
- **Reaper.** One engine node that is blocked behind a long holder sets `retry_needed`, which forces a full reaper iteration every 100 µs for the duration. A holder can be long-lived: a large legacy `cuFileWrite`, a slow write, or a quarantined operation that never releases.
- **Reactor.** Parked legacy writes count in `inflight_count()`. Once `max_inflight` of them are parked, the reactor stops dispatching unrelated reads and writes (head-of-line blocking).
- **Quarantine.** A quarantined write keeps its interval and ticket forever. That is intended for memory safety, but it means:
  - every flush on that inode hangs;
  - every conflicting `host_write` spins its caller forever;
  - the reaper never goes idle.

  None of this is surfaced.

**Fix**
- Give `file_write_state` a waiter list keyed by granule range. Have `release()` notify the owning engine (`device_engine::notify`), the reactor (its wake path) or a condition variable for `host_write_io`.
- Use bounded backoff, not a fixed 100 µs, as a fallback.
- Exclude `_write_waiting` from the `max_inflight` gate, or cap it separately.
- Report a gauge or diagnostic for intervals pinned by quarantined operations, and document that flush/write on that inode cannot complete.
- Note this in AM-13's G-5 CPU list.

#### P5-R7: Pinned write scratch is sized lanes × largest class: 160 MiB to 1.25 GiB per device, outside the reservation manager

`device_engine.cpp:367-370`, `:160-185`

**Problem**
- `write_scratch_bank(scratch_cell_bytes * lanes->capacity())` allocates one cell of the largest registered class per lane, at bootstrap.
- Lanes default to `device_lane_cap = 160`. That is 160 MiB of pinned host memory per device with 1 MiB classes, 640 MiB with 4 MiB, and 1.25 GiB with 8 MiB. It is never returned, and the reservation manager does not account for it.
- Edges need at most one granule. Bodies need a host cell only on the fallback, suffix and capacity paths.
- The rest of cuCascade budgets pinned host memory explicitly; NUMA pinned pools are a scarce resource.

**Fix**
- Allocate granule-sized edge cells per lane, plus a small bounded pool of class-sized fallback cells (for example `max(Q_eff)` per device, or a config value) that a node leases when it needs a host body. A node that cannot lease waits resource-free, like a slot miss.
- Report the pinned total in the gauges.
- Document the budget in AM-16 (P5-R10).

#### P5-R8: The B-6 spill workload cannot detect misplaced or missing writes, and it does not measure a spill

`tools/gds/cufile_engine_bench.cpp:291-294`, `:342`, `:389-423`, `:237-255`, `:449-451`

**Problem**
- **Constant source.** Every source byte is `0x5a`. Wrong-offset writes, slot mix-ups across lanes (the AM-9 failure mode) and cross-file swaps write identical bytes and are invisible.
- **Sparse verification.** The pass reads three 256-byte samples of each ~8 MiB slice. With 1 MiB pieces, five of the eight pieces are never inspected, so a dropped body piece would pass.
- **Timed passes are unverified.** They rewrite identical bytes, so a write lost in any timed pass is undetectable.
- **Not a spill pattern.** `create_file` pre-writes and fsyncs the full pattern, so every timed write is an in-place **overwrite** of allocated blocks. The real spill path (`src/cudf/disk_io.cpp`) writes new, extending files. Extending O_DIRECT writes behave very differently on ext4/XFS: unwritten-extent conversion and `i_rwsem`. The legacy code itself notes that "staging measured faster for extending writes".
- **No durability point.** There is no flush or fdatasync in the timed window, and the working set is 2 × 8 MiB. Buffered edges and the legacy staged path are partly measured against the page cache. The README says this, but the workload as built cannot become the B-6 acceptance measurement.

**Fix**
- Fill each task's source with an offset- and file-dependent pattern, using the same generator as `create_file` but seeded differently from the pre-fill.
- Verify every byte of each written range (or a strong per-MiB checksum) in the untimed pass. Also verify after the final timed pass, with a per-pass seed so stale data cannot match.
- Add a `--spill-mode new|overwrite` switch. `new` creates (or truncates to 0) a fresh file per pass, outside the timed window, so writes extend.
- Add an optional `--flush 1` that awaits `flush_async` per file inside the timed window. Record both modes in the CSV.
- Default the README recipe to `/mnt/disk_2` with a working set well above RAM, or with `--drop-cache`.

#### P5-R9: Test gaps for the new ordering surfaces

`test/io/cufile/test_engine_write.cpp`

What is covered is good: read-back for bodies, edges and suffixes; no replay for every unknown-progress shape; cross-handle granule waits; flush capture; legacy-context flush. These cases are missing:
- synchronous `host_write_io` (via `ctx.host_write`) waiting behind an engine direct granule, and engine writes waiting behind it;
- a legacy **device** slice, whose interval is `mixed`, against an engine body or edge on a shared granule. Only a host buffered-only legacy slice is tested;
- the default legacy mode: no ticket or interval behaviour change (P5-R2), and registry eviction (P5-R1);
- the config-mismatch route (P5-R3), negative-completion demotion (P5-R4) and sentinel recovery (P5-R5);
- `not_submitted` (breaker) arriving **after** an aligned positive short write: the fallback must write exactly the remainder from the slot;
- a direct `sync_write` that gets `EINVAL` and continues buffered, verified by read-back with an O_DIRECT-refusing fd seam;
- a quarantined write holding an interval: flush stays pending, and the drain reports incomplete;
- the no-replay tests should also assert `direct_write_bytes == 0 && buffered_write_bytes == 0`, so a failed fallback replay cannot slip through.

#### P5-R10: Phase 5 contracts are not recorded in `06-design-amendments.md`

`docs/io/gds-design/06-design-amendments.md` (unchanged in the working tree)

**Problem.** Phase 5 departs from or extends §6.13 and §4.10 in ways that downstream phases depend on:
- try-lock interval acquisition with resource-free parking;
- the conservative `mixed` legacy intervals and blocking `host_write_io` waits;
- tickets on every writable handle, including legacy ones;
- the sticky per-inode `has_engine_work` choosing the flush path, with engine-path flush waiting for in-flight writes too;
- write evidence that exempts standing overrides and revokes only writes;
- `allow_writes` as the only production route;
- the stuck-call "unknown progress means fail" rule;
- the pinned scratch bank;
- the simulator's restriction on spill.

These live only in the checkpoint file. P4-R11 set the precedent that such contracts belong in the authoritative amendments.

**Fix.** Add **AM-16** with these points, plus the P5-R2/R6/R7 resolutions.

### NIT

#### P5-R11: A slow-but-successful write is reported as failed
`device_engine.cpp:1817-1819`, `:1067-1070`. The watchdog sets `unknown_progress` after `stuck_ms` (2 ms), and `recorded` then fails the write after lane recovery, even though `bytes_done` becomes authoritative once the event completes. This conforms to §7 #29 ("timeout → no replay, `finish_error`"), but it turns slow submits into user-visible errors. Consider validating `bytes_done` after the event and only *forbidding resubmission*. Document whichever rule is kept in AM-16.

#### P5-R12: Read admission now runs `next_request` before the QD check
`device_engine.cpp:1396-1398`. When QD is full, `next_request` still mutates DRR state: the group switch plus one deficit quantum. Any pending write on the mount is also skipped whenever a read is selected while QD is full, so writes cannot stage while reads saturate QD. Check QD only after confirming that the selected request is a read, and fall through to a write request if one exists.

#### P5-R13: `engine_route` now runs on the application thread
`cufile_ioctx.cpp:115-120`. On the submitting thread, `select_reactors` → `engine_write_candidate` → `engine_route` may:
- compute mount identity, which parses mountinfo on first use;
- create a policy;
- schedule a read probe of the file being written.

It also runs twice per engine write slice. This contradicts the spirit of FR-3. Cache the identity and policy lookups, or make the activation snapshot policy-only.

#### P5-R14: Write-only mounts never migrate off a stuck submitter
`device_engine.cpp:1762`. `migrate_healthy_mounts` filters on `read_eligible()` only. A mount with `async_reads=false` and `async_writes=true` never migrates. Use `read_eligible() || write_eligible()`.

#### P5-R15: `allow_writes` on a non-operator override is silently ignored
`config.hpp:66`, `mount_policy.cpp:337-344`. Make `validate()` reject it, or warn.

#### P5-R16: Small API and accounting points
- `prepared_io_slice::backend_owner` is a public `shared_ptr<void>`. Unrelated values are hard-cast with `static_pointer_cast<ticket>` (`cufile_reactor.cpp:1394`, `:1434`). Use a typed, private tag.
- `try_acquire` takes `granule` per call while storing granule indices. Store the granule once in the state and assert that it matches.
- `granule_conflicts` and `file_write_state::_conflicts` also count `try_lock` misses.
- The bench says "short logical read" for writes.

#### P5-R17: `sync_write` gives up instead of continuing buffered after a partial direct write
`device_engine.cpp:909-915`. An O_DIRECT `EINVAL`/`EOPNOTSUPP` after partial progress (`written > 0`) fails the piece, although a known-progress buffered continuation is safe under the held interval.

---

## Responses to the review charge

| Area | Assessment |
|---|---|
| Never replay unknown progress | Verified on every path. Sentinel recovery gap: P5-R5. Over-conservative stuck handling: P5-R11. |
| Short-write suffix arithmetic | Verified, both aligned (same lease) and buffered (slot remainder). Test read-back is exact. |
| Granule conflict serialization | Verified for engine↔engine, engine↔legacy (mixed) and engine↔`host_write`. Polling and quarantine: P5-R6. Untested surfaces: P5-R9. |
| `note_written` only on success | Verified. |
| Flush covers writes ≤ its sequence | Verified: callback-driven `fdatasync` on the blocking pool; later sequences never gate it. |
| Ordering versus legacy writes | Verified. Legacy writes are fully synchronous inside their interval. Default-mode cost: P5-R2. |
| Source read only after the fence | Verified (fence wait precedes the first copy; later paths read the slot). |
| Future resolves after quiescence | Verified (I/O event, `sync_done` or lane sync; siblings drain). |
| Scratch/slot never released while DMA may be live | Verified for normal and recovery paths. Sentinel exception: P5-R5. |
| No per-operation pinned alloc/free | Verified. Bank size: P5-R7. |
| Registry and identity across handles/opens | `(st_dev, st_ino)` sharing is correct and tested. Leak and inode reuse: **P5-R1**. |
| Flush versus new writes race | Correct: tickets are taken before dispatch, `capture` reads `_last`. |
| Default behaviour unchanged | Data path unchanged. Sequencing overhead and serialization in legacy mode: P5-R2. Leak: P5-R1. |
| Write eligibility honest | Verified. Only production route is an operator override with `allow_writes`. |
| No slice split across legacy and engine | Verified. Config mismatch fails rather than falling back: P5-R3. |
| Failure paths | Recovery, not-submitted, cancellation and breaker verified and tested. Missing demotion: P5-R4. |
| Tests prove integrity and no replay | Mostly yes (full read-back, `write_calls`, `size()`). Gaps: P5-R9. |
| Benchmark spill workload validity | Not yet valid for B-6 acceptance: P5-R8. The README labelling (NOT GDS, not durable) is honest. |

## Verdict: **CHANGES-REQUIRED**

The write engine's integrity core is sound. I found no path that:
- replays unknown progress;
- miscomputes a suffix;
- reads the source before its fence or after resolution;
- advances `note_written` on failure;
- releases a slot or scratch cell while a copy or DMA may still be running (the sentinel case is the one exception: P5-R5).

Granule serialization and flush sequencing behave as specified. The suite and 10 randomized `[engine]` runs pass.

What remains:
- **Commit blocker:** **P5-R1**. The registry leaks one entry per written inode forever, on the default path.
- **SHOULD-fix before the engine write path is used for any B-6 number, or enabled outside tests:**
  - routing and fallback: **P5-R2, P5-R3, P5-R4, P5-R5**;
  - liveness, CPU and memory: **P5-R6, P5-R7**;
  - benchmark validity: **P5-R8**;
  - tests: **P5-R9**;
  - amendments: **P5-R10**.
- NITs: P5-R11 to P5-R17.

## Resolution

- **P5-R1 — fixed:** weak inode registry, identity-checked O(log N) final-owner eviction, allocation-unwind safety, test-only count and 128-file cleanup regression.
- **P5-R2 — fixed:** preactivation legacy dispatches take nonserializing activity guards; first engine admission waits for those guards, avoiding whole-ticket mixed-request deadlocks. Mutex misses no longer park. Four-reactor legacy read-back/conflict regression added; performance qualification remains AM-13 Phase 7.
- **P5-R3 — fixed:** shared-engine `accepts_writes()` gates both routing and submission-time activation; the reads-only-first-context regression routes writes to legacy.
- **P5-R4 — fixed:** non-transient negative completions revoke write eligibility even with overrides and warn once; ENOSPC/EACCES/EDQUOT retain eligibility. Affected writes still fail without replay; sequential-write tests cover demotion and exceptions.
- **P5-R5 — fixed:** write sentinel takes owned-lane recovery; tests assert recovery and zero fallback bytes, including failed-recovery quarantine retention.
- **P5-R6 — fixed:** range-overlapping release callbacks wake engine/reactor waiters; synchronous host writes use a condition variable. Parked legacy writes no longer consume the in-flight gate. Quarantine diagnostics/gauges document pinned inode writes/flushes; idle-health fallback and G-5 contention qualification are recorded in AM-13/AM-16.
- **P5-R7 — fixed:** one granule-sized cell per lane, with blocking fallback streaming only retained-slot contents through it; no largest-class host pool is needed. Engine and process-pool pinned-byte gauges expose both active budgets and cached allocations. Process-pool reuse avoids cudaFreeHost device-wide synchronization; reservation integration remains the existing AM-5 open item.
- **P5-R8 — fixed:** nonconstant file/offset/pass-dependent sources, full spill-byte verification before timing and after the final timed pass, fresh per-pass seeds, `--spill-mode new|overwrite`, timed optional `--flush 1`, CSV labels and `/mnt/disk_2` cache-evicting recipes. Historical constant-source smoke is explicitly not B-6 evidence.
- **P5-R9 — fixed:** added registry/legacy concurrency, host/device cross-mode waits, preactivation barrier, config mismatch, demotion, sentinel/quarantine, aligned-short fallback, queued not-submitted-after-short, and direct EINVAL continuation regressions. No-replay tests assert zero direct/buffered fallback bytes.
- **P5-R10 — fixed:** AM-16 records Phase 5 contracts, alternatives and remaining qualification boundaries; AM-13 adds interval-waiter CPU measurements.
- **P5-R11 — declined:** retain the conservative §7 #29 stuck-write failure policy: delayed successful submission does not undo watchdog-classified unknown progress. No replay; rule documented in AM-16.
- **P5-R12 — fixed:** QD-full reads are excluded before request/DRR selection; available writes can still stage and ineligible reads earn no quantum.
- **P5-R13 — fixed:** submission-time activation is capability-only; mount identity/policy creation and probe scheduling stay on the reactor route.
- **P5-R14 — fixed:** healthy write-only mounts can migrate off a stuck submitter.
- **P5-R15 — fixed:** validation rejects `allow_writes` outside operator-qualified overrides, with a regression for all override states.
- **P5-R16 — fixed:** slice ownership is typed, no hard casts; inode granule is fixed/validated; only real overlap conflicts count; benchmark reports short logical transfers rather than reads.
- **P5-R17 — fixed:** known direct progress continues buffered after EINVAL/EOPNOTSUPP; read-back seams cover both initial refusal and refusal after a granule prefix.

### Fix validation

- Requested release build of `cucascade_io_tests`, `cucascade_cufile_async_qd` and `cucascade_cufile_engine_bench`: passed, sequentially with no concurrent build.
- Full suite: **893 cases: 892 passed, 1 existing kTLS skip; 3,037,748 assertions passed**.
- Final sequential `[engine] --order rand --rng-seed $RANDOM` loop: **10/10 passed**, **107 cases / 4,838 assertions each**. Seeds: 29255, 17657, 5128, 14161, 28569, 17208, 9675, 28985, 3641, 9504.
- The initial randomized run exposed an over-strict new transient-error test: retained eligibility does not forbid a temporary zero-capacity blocking fallback. The corrected test checks eligibility, exact byte accounting and content; failing seed 7839 passed on recheck before the final loop.
- Verified spill smoke (two measured passes each): legacy new, legacy overwrite, real-libcufile async new (TEST-ONLY compat admission); flush enabled, advisory cache eviction, full byte verification before timing and after the final timed pass. NOT GDS qualification.
- `pixi run python -m pre_commit run --files <changed>`: all applicable hooks passed (unrelated live editor lock excluded).
- No commits, delegation, concurrent builds or `src/cudf/` edits.
