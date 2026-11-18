# Phase 3 review: async engine reads (device_engine core + reactor/ioctx integration)

- Branch `io/cufile-async-engine`; working tree reviewed against HEAD `5f89013`.
- Scope:
  - New: `include/cucascade/io/cufile/device_engine.hpp`, `src/io/cufile/device_engine.cpp`, `src/io/cufile/engine_ring.hpp`.
  - Modified: `templated_ioctx.hpp`, `types.hpp`, `io_request.hpp`, `io_context.hpp`, `cufile_reactor.{hpp,cpp}`, `cufile_ioctx.{hpp,cpp}`, `async_transport.{hpp,cpp}`, `config.hpp`, `operation.hpp`, `detail.hpp`, `mount_policy.cpp`.
  - Tests: `test/io/cufile/test_engine_{read,failure,sched,ioctx}.cpp`, `engine_test_utils.hpp`, `test_null_stream_ptds.cpp`, simulator `read_sync`.
- Normative inputs: `06-design-amendments.md` (AM-1 … AM-11), `05-final-design.md` (FR-1..5, FR-9, FR-19..21, FR-25..31, FR-37, NFR-3..5, NFR-11, §4.8–§4.12, §6.6–§6.11, §7), `reviews/phase-{1,2}.md`, `AGENTS.md`.

## Verification performed

| Check | Result |
|---|---|
| `pixi run cmake --build build/release --target cucascade_io_tests -- -j 20` | Up to date (no work), zero warnings |
| Full `./build/release/test/cucascade_io_tests` | **830 cases: 829 passed, 1 skipped** (existing kTLS skip), 3,032,735 assertions |
| `"[engine]"`, random order, 25 runs, idle host | **25/25 pass** (46 cases, 1,227 assertions each) |
| `"[engine]"`, random order, 10 runs, all 24 CPUs busy (`while :; do :; done` × nproc) | **4/10 runs fail** (details in P3-R10) |
| `"[engine][ioctx]"`, random order, 20 runs, all CPUs busy | **5/20 runs fail** (details in P3-R10) |
| `"ioctx real compat engine uses persistent slot registrations"` alone, 20 runs, CPUs busy | **2/20 fail** (gauge lags future) |
| Failing seeds re-run on an idle host | pass, so the failures are load-dependent and not order-dependent |

I modified no source code. The only file written is this review.

## Confirmed correct, no finding

**Lifetime and exactly-once**
- `operation_credit` is exactly-once, using an atomic `_settled` exchange. Its destructor settles `canceled` as a safety net.
  - Every path that hands off, rejects, accepts, cuts, cancels or finishes a slice or child credit settles it once.
  - AM-1's second `finish_success()` in `finish()` (`device_engine.cpp:497`) is a harmless no-op.
  - Child credits call `add_tasks(1)` before the record holds the credit (`device_engine.cpp:1069`).
- AM-1 retention is honoured:
  - `copy()` (`:504-541`) enqueues the fence wait, the D2D, the `copy_ev` record and then `cudaStreamWaitEvent(user_stream, copy_ev)`, all on the reaper thread, before it settles the credit.
  - Slot, lane, args block, `io_object` and fence stay with the node and cursor until `reap()` observes `copy_ev` (`:622-628`), and are released only by the blocking-pool cleanup (`:697-706`).
- Every `finish()` call site is genuinely quiescent. I checked:
  - copy-time cancel, after the I/O event was observed;
  - validation errors;
  - `not_submitted`, where nothing was enqueued;
  - `size_checked` failure;
  - `recovered` success, after lane synchronization;
  - empty physical range;
  - retry or fallback cancelled under stop.

  The unconditional `prove_recovery_quiescence()` in `finish()` is therefore safe today. It does, however, remove the pool's independent guard; see P3-R16.
- PTDS reads are host-complete. They are never joined (`:531`), and their credit settles only in `finish(n, true)`, after `copy_ev` is observed. `test_engine_read.cpp:90` asserts `wait_calls == 4`, which proves there are no stand-in joins.
- A failure after a partial enqueue goes to recovery, which synchronizes only that lane before it settles. This covers record failure, copy failure, fence-wait failure, join failure and submit errors. The T-29-style matrix (`test_engine_failure.cpp:56-98`) exercises each injection point and asserts that only the owned lane is synchronized.
- When recovery fails, the operation is quarantined: no credit is settled, nothing is cleaned up, and drain reports incomplete (`:801-811`).
- AM-1 group retirement for errors works:
  - `grouped_coordinator::_async_accessors` holds a *failed* future until every engine cursor of the group has physically retired.
  - Success still resolves early.
  - `test_engine_ioctx.cpp:430` is a meaningful test of this.
- P2-R19 is resolved:
  - `operation_pool::try_release`, lane and slot releases, and cursor (file, fence, stream owner, ticket) releases all run on the blocking pool.
  - `n.request.reset()` on the reaper is never the last owner, because `requests[]` retains it until `retire_cursors`.
  - This is tested at `test_engine_failure.cpp:276`.
- Event reuse is safe:
  - `io_ev` and `copy_ev` are re-recorded only by the next generation on the same lane, after the previous node's cleanup.
  - Stale deadlines are filtered by node generation.
  - `cudaStreamWaitEvent` snapshots the latest record, so pooled fences and lane `copy_ev` can be reused immediately.

**Stream semantics**
- AM-2 holds. The submitter issues `read_async` with no fence wait (`:383-391`). The fence wait is enqueued on the lane immediately before the D2D (`:516-518`).
  - `test_engine_ioctx.cpp:185` is a deterministic proof that the D2D is gated: a gated `memset 0xab` that ran after the D2D would fail it.
  - `test_engine_read.cpp:60` proves storage is not gated: all four I/Os complete while the fence is gated.
- The device context is correct:
  - The reaper, submitters, recovery, cleanup, fallback and cursor-release tasks all call `cudaSetDevice(device)`.
  - The fence pool uses `cuda_set_device_raii`.
  - Cross-device `cudaStreamWaitEvent` is legal.
- Null and legacy streams (commit 469a0221) keep their behaviour. An original null stream stays on the legacy route (`cufile_reactor.cpp:932`), and resolution to `cudaStreamLegacy` is unchanged. Lanes are non-blocking.
- `mode=legacy` has no new behaviour:
  - `submit_fence_enabled()` is false, so no fence or event is recorded.
  - `engine_route` returns at its first comparison.
  - No `engine_admission` exists, and shutdown does not wait.
  - `cufile_ioctx::start` returns early.
  - The worker pass accounting is equivalent.
  - The uring, REST and s3rdma backends take the `else` branch of an `if constexpr` and are unchanged, with no runtime check.

**Concurrency**
- `engine_ring` is a textbook Vyukov bounded MPMC ring with correct acquire/release on the cell sequences. Its capacities bound all producers:
  - completions: ≤ 2 per node + 1, against 16 × lane cap;
  - per-mount commands: ≤ leased lanes.
- Ownership is clean. Node and operation state is written only by the reaper, except the arguments, `bytes_done` and scratch, which a blocking or recovery worker writes while that node is parked with `pending=none`. The executor rings publish those writes.
  - The submitter reads only immutable or atomic state.
  - MPMC pops give "winning the pop proves unstarted" its meaning (`:1190-1222`).
- The eventfd protocol cannot lose a wake-up. The counter is sticky, `ppoll` returns while it is non-zero, and every consumer re-checks its rings after the `read`.
- The reaper uses only `try_lock` on the operation pool, arena, lane pool, control executor, snapshot and policy timers. NIT P3-R15 lists the two remaining `lock_guard` releases.
- Facade shutdown waits on retirement tickets, not on early futures. It cancels only its own pieces, through the per-facade flag, and never stops the shared engine (`test_engine_ioctx.cpp:298,323,368,400`).
- The watchdog and migration work:
  - The unstarted queue behind a stuck call is drained to sync fallback, and other mounts migrate when there are ≥ 2 submitters (`test_engine_failure.cpp:205,238`).
  - The `survivors` array of 160 is bounded by the validated `device_lane_cap ≤ 160`.

**Other requirements**
- Adaptive backoff (AM-10) is implemented and tested beneath both caps (`test_engine_sched.cpp:238`).
- The AM-11 class mask is passed to `rebalance`.
- Slot registration passes `buffer_offset == 0` (AM-9) and is asserted.
- Short-read retry arithmetic (`:589-596`) uses:
  - `size = physical.end − align_down(valid_end)`;
  - `file_offset = restart`;
  - `buffer_offset = slot_offset + (restart − physical.offset)`;
  - progress measured by `valid_end`;
  - the bound `max_short_reads`.

  This matches §6.11 and is asserted at `test_engine_failure.cpp:23`.

---

## Findings

### BLOCKING

#### P3-R1 — A fence-gated request occupies every lane and slot of its mount, so one busy caller stream starves every other request on that mount (FR-26, §7 #21; contradicts AM-2's own rationale)

**Where**
- `device_engine.cpp:504-541` (`copy`)
- `device_engine.cpp:994-1100` (`admit_one`, which has no fence check)
- Too-weak tests: `test_engine_ioctx.cpp:278`, `test_engine_sched.cpp:301`

**Problem**
- AM-2 removed the read-side input-waiter cap on the premise that "a busy caller stream therefore delays only copies, never storage queue depth".
- In this implementation, a validated piece immediately enqueues `wait(fence) → D2D → record(copy_ev)` on its lane. It keeps the lane and the staging slot until `copy_ev` completes, which is after the caller's stream drains.
- Admission does not look at the fence. A request whose stream is busy (a long kernel, a gated producer) therefore keeps being expanded until it holds `effective.lanes` lanes, or the mount's slot quota.
- Every other request on that mount, from other callers on idle streams, then gets `m.occupied >= effective.lanes` and stalls until the unrelated stream drains.
- `test_engine_sched.cpp:301` shows the occupancy directly: an 8 MiB read on a gated stream holds both lanes with `live_leases == 2`.
- `test_engine_ioctx.cpp:278` ("busy user stream does not block other requests") passes only because its blocked request is a single 1 MiB piece on a 5-lane mount.
- Legacy parks such a slice without holding resources (`park_if_stream_busy`), so enabling the engine regresses a property that legacy has and that FR-26's last bullet ("A request blocked on its input never prevents other ready requests from completing") still requires; AM-2 amended only the cap mechanism.

**Fix**
1. Reintroduce a read-side **fence readiness hint**. It changes admission only, not correctness: the D2D keeps its unconditional fence wait.
   - Add `cursor::fence_ready` and `cursor::fence_queried`. Query `cudaEventQuery(fence)` at most once per `poll.fence_hint_us` (100 µs) per fence, on the reaper, which is non-blocking.
   - While the hint is unready, cap the lanes that the request's pieces hold *after I/O validation* (copy-waiting), at 1 per request, or `ceil(lanes/8)` per mount for all unready fences. Further pieces of that request stay pending without a lane.
   - AM-2 still holds: the first piece's storage I/O is never gated.
2. Optionally let a validated-but-unready piece release its lane and keep only its slot, re-leasing a lane when the hint turns ready. This costs a second lane lease but frees QD.
3. Strengthen the test: a gated 16 MiB request on stream A, then a 1 MiB request on stream B to the same mount. Assert that B resolves while A's gate is still closed. Repeat with the gated request first and with QD 4.
4. Record the mechanism in a new amendment, AM-12, so AM-2's premise is accurate.

---

### SHOULD

#### P3-R2 — Reaper cost is O(nodes + mounts × requests) per loop iteration and per admitted piece (NFR-3 / NFR-4 / G-5 hazard)

**Where** (`device_engine.cpp`)

Every loop iteration (`:1243-1308`) does all of the following:
- `service_work()`: scans all `4 × device_lane_cap` nodes (640 by default) (`:672-760`).
- `update_snapshot()`: scans all nodes, rebuilds `stats.mounts`, and copies the ~3 KB `engine_gauges` (`:1161-1188`).
- `drain_refused_commands()`: runs `any_of(nodes)` *per mount* (`:1194`).
- `capacity_update()`: runs `any_of(requests)` per mount (`:870-876`).
- The idle check runs `any_of(requests)` and `any_of(nodes)` (`:1301-1305`).
- `retire_cursors()`: O(R).
- Every `token()` calls `account()`, which scans all requests (`:410-429`).

Every admitted piece pays for:
- `admit()` recomputing `has_work` for all mounts × all requests twice per round (`:1106-1109`);
- `next_request` (three O(R) passes);
- a linear search for a free node (`:1030`);
- a linear search for the request's position (`:1059`).

Wake-ups are also more frequent than they need to be:
- While any request is unfinished, the loop wakes every 100 µs (`:1306`), even when it is only waiting for lanes or slots that the cleanup result will signal. Under saturating backlog it therefore runs ≥ 10 k iterations/s regardless of events.
- Each operation costs about 4–5 eventfd `write` syscalls:
  - one notify per command (`:460`);
  - two separate submitter publications (`call_sample` + `recorded`, `:395-403`), each with its own notify (`:354`);
  - a cleanup publication;
  - a cursor release.

**Problem**

With R in the thousands (a DISK restore hands off one slice per column buffer, and `max_pending_slices_per_device` is 4096), every piece costs O(10⁴) operations. With only 640 nodes, each loop already does about 3,000 node visits. NFR-3's 10 µs/op reaper budget and §4.11's "process at most 16 due events, then service rings" are unlikely to hold. This must be fixed before the Phase 7 G-5 measurements, or they will measure bookkeeping instead of polling.

**Fix**
- Use intrusive lists:
  - a free-node stack;
  - a "node has pending work" list, filled by the code that sets `pending`;
  - per-(mount, tier) ready-request queues, maintained at accept, finish and cursor-retire;
  - a cancelled-commands flag per mount, set when cancellation is detected, instead of the node scan.
- Maintain the backlog and has-work counters incrementally.
- Rebuild the snapshot at most once every N ms.
- Remove the unconditional 100 µs poll. Re-arm only on rejected executor submits, using a dedicated "retry_due" deadline.
- Merge `call_sample` and `recorded` into one publication.
- Notify the reaper or submitter only when it is parked: an atomic `sleeping` flag, set before `ppoll` and re-checked after.
- Add a cheap reaper CPU gauge (ns per iteration, iterations per op) so B-1 and B-3 can check NFR-3.

#### P3-R3 — Every operation's cleanup round-trips through a shared, 1 ms-polling blocking pool that also runs long sync I/O (NFR-4, FR-28)

**Where**
- `device_engine.cpp:97-155` (`lazy_pool`)
- `:697-706` (per-operation cleanup task)
- `:1129-1158` (per-cursor release)

**Problem**
1. The lane is reusable only after a cross-thread round-trip: reaper → `lazy_pool` worker (`try_release`) → completion ring → reaper `cleaned`, where `m.occupied` is decremented. This adds tens of µs per operation to the 65 µs residence budget.
2. `lazy_pool` workers block on `wait_for(1 ms)` and are notified without holding the mutex (`:116`). A push that lands between `pop()` failing and `wait_for` therefore loses its wake-up, so the task waits up to 1 ms.
3. After first use, six process-lifetime threads (blocking 4 + recovery 2) wake every 1 ms forever: about 6,000 wake-ups/s on an idle process.
4. The same four workers serve:
   - latency-critical cleanups;
   - cursor releases (which may synchronously deregister an `io_object` or destroy a PTDS stand-in stream);
   - `fstat` per short read;
   - multi-ms sync `cuFileRead` / `pread` fallbacks.

   When the circuit opens, fallback reads occupy all four workers, cleanups queue behind them, and lanes stop recycling. This is head-of-line blocking that amplifies a demotion into a stall.
5. Pool start is lazy and is delegated to the single control thread, which also runs warm-up for all devices (P3-R11) and probes (which call `cudaStreamSynchronize`). The first cleanup on a published device can wait for an unrelated device's warm-up. If a probe is stuck, cleanups never start.
6. Each cleanup allocates a `std::function`, because its captures are larger than the small-buffer optimization.

**Fix**
- Release lanes and slots on the reaper when they are provably not the last owner of external resources. The lane pool and arena are retained by `options`, so `lane_lease` and `staging_lease` resets are cheap and safe there.
- Hand only the *final file / fence / stream-owner / scratch references* to a cleanup queue. Batch them: one task drains a lock-free list.
- Separate a small cleanup executor from the fallback-I/O executor.
- Use a proper condition-variable or eventfd handshake, with no 1 ms polling.
- Start the pools when the engine starts, on the control thread inside `for_device`, not lazily from the reaper.

#### P3-R4 — A short-read retry hangs forever when its mount's grant drops to `qd == 0`

**Where**
- `device_engine.cpp:677-683`: `retry` is dispatched only while `in_io < effective.qd`.
- `:445-450`: `dispatch` defers to `retry`.
- `:911-921`: the capacity task deliberately grants `qd = 0` to active mounts when lanes or slabs are scarce. The code comment cites "two mounts sharing only two lanes".

**Problem**
- Suppose a piece takes a mid-file short read while its mount has `qd ≥ 1`, and a second mount then becomes active and rebalance gives the first mount the capacity-fallback grant (`qd=0`).
- The node stays `retry`. It holds its lane and slot, and the 100 µs idle poll keeps the reaper spinning, but it never dispatches.
- Because the mount stays active, the grant does not change. The read never completes and drain never finishes.

**Fix**
- In `service_work`, when `effective.qd == 0`, convert a `retry` node into a blocking-pool fallback (`n.fallback = true; pending = fallback`), reusing `sync_read` with the already-prepared `args`.
- Add a test: a short-read fault plus a held executor that publishes a zero grant between I/O and retry.

#### P3-R5 — Recoverable conditions become user-visible read failures that legacy would have served (FR-30, §7 #16–18)

**Where** (`device_engine.cpp`)
- `:557-571` (`validate`)
- `:782` (`failed`)
- `:801-805` (`recovered` → `finish(false)`)

**Problem**
- Only a *retry* that returns `-EINVAL` or `-EOPNOTSUPP` falls back to buffered reads.
- A first attempt returning `-EINVAL`, `-EOPNOTSUPP`, `-ENOSYS` or `-1` is surfaced to the caller as `io_error`. So is a submit-time `rc` error, after recovery. Libcufile's `bytes_done` uses `-1` in the AM-9 compat case.
- §7 #18 requires that an unsupported-direct error "demotes the file one rung and re-issues the piece through fallback after quiescence". FR-30 requires classification before retry.
- With the engine enabled, a file or mount where async O_DIRECT is refused therefore fails reads that `mode=legacy` serves.
- §7 #33 (bounded backoff for transient ESTALE-class errors) is also absent and untracked.

**Fix**
- Classify errors. Terminal errors are `ENOSPC`, `EACCES`, `EINVAL` caused by the caller's arguments, and `file_shrunk`.
- For everything else, after quiescence (the event has been observed, or recovery has succeeded), set `pending = fallback`. The credit stays unsettled until the sync fallback result.
- Record a per-mount demotion sample (`observe_unsupported`).
- Track the transient-error backoff in AM, or implement it.
- Add tests: first-attempt `-EINVAL`, first-attempt `-1`, and submit `rc` error. Each must end with correct contents and `sync_bytes > 0`.

#### P3-R6 — Engine metadata exhaustion fails reads that the facade could still serve on the legacy path

**Where**: `device_engine.cpp:1374-1381`, `cufile_reactor.cpp:1254-1273`.

**Problem**
- When `outstanding >= max_pending_slices_per_device` (4096), `hand_off` settles the slice with `no_buffer_space`.
- The facade has already committed the slice to the engine, so a large vectored read with more than 4096 slices across callers fails outright. Examples are a parquet `device_read_ranges_async` or a wide-table DISK restore. Under `mode=legacy` the same read succeeds.
- FR-3's "fail on metadata budget" was written for the reactor-level `enqueue`. At the facade→engine boundary, a functioning fallback exists.

**Fix**
- Add `bool device_engine::try_reserve_slice() noexcept`, performing the CAS on `outstanding`, and call it in `engine_route`.
- On failure, route the slice legacy and count it as `metadata_fallbacks`.
- `hand_off` then consumes the reservation.
- Keep `no_buffer_space` only for direct `hand_off` callers.

#### P3-R7 — The buffered fallback allocates and frees pinned memory per piece; `cudaFreeHost` synchronizes the device

**Where**: `device_engine.cpp:646-669`.

**Problem**
- `sync_read`'s buffered path calls `cudaMallocHost` and `cudaFreeHost` for every piece.
- The existing reactor documents (`cufile_reactor.hpp`, `shutdown()`) that `cudaFreeHost` "would synchronize the device and make shutdown wait for other callers' GPU work".
- On the engine, a fallback worker therefore waits for *all* device work, including gated user streams and fence-blocked lanes. That stalls one of the four shared workers, and through P3-R3 also stalls cleanups.
- A stream blocked on a host condition that depends on this read could deadlock.
- It is also a per-operation allocation on the fallback path.

**Fix**
- Lease pinned scratch from the existing process-wide pinned scratch pool, or from a small engine-owned pool preallocated at bootstrap, sized `S_max × blocking workers`.
- Return it to the pool; never call `cudaFreeHost` on a transfer path.

#### P3-R8 — The contract text implies callers can tell which read was engine-served; the cudf-layer header still promises host-complete

**Where**
- `io_context.hpp:502-515`
- `cufile_reactor.hpp:208-212`
- `include/cucascade/cudf/datasource.hpp:171-175`

**Problem**
- Routing is per slice and *dynamic*: probe state, circuit, cooldown, `try_lock` misses (P3-R12), and parked slices that are re-dispatched after their stream gate (`cufile_reactor.cpp:1993`, which hands them to the engine; see P3-R10).
- A caller of a non-legacy context therefore cannot know whether its read was "legacy host-complete" or "engine enqueued". The text "Legacy, compat and POSIX paths are host-complete … When the opt-in … engine serves a read …" invites per-path reasoning that callers cannot apply.
- The datasource contract ("on success the bytes are in place without further synchronization of `stream`") is now false for engine contexts. It is not in the AM-1 migration list.
- The text also does not mention three things:
  - the join is inserted into the caller's stream at an arbitrary later point;
  - an active CUDA graph capture on that stream from another thread will be invalidated by the reaper's `cudaStreamWaitEvent`;
  - an early success can be followed by a failed copy, which only shows up as a stream or context error when the caller synchronizes (P3-R18).

**Fix**
- State the rule as a property of the context: "When `async_engine.mode != legacy`, any device-only read whose original stream is not `cudaStreamPerThread` may complete *enqueued*. Synchronize or wait on the stream before host or foreign-stream access. Do not capture that stream while such a read is in flight."
- Add `datasource.hpp:171-175` (and the scalar `device_read_async` doc) to the AM-1 migration list, and update its text now.

#### P3-R9 — The AM-1 join ordering is not deterministically proven

**Where**: `test_engine_read.cpp:60`, `test_engine_ioctx.cpp:117,139,339`.

**Problem**
- The "enqueued not host-complete" tests gate the *user* stream. After the gate is released, the consumer D2H and the lane's fence-waiting D2D start together. A missing join therefore shows up only if the D2H happens to win the race.
- `:117` (no gate) usually sees copies that already finished.
- The test that uses the copy-delay seam (`:339`) never checks contents. It synchronizes the stream first.
- None of these would deterministically fail if line 531's join were deleted.

**Fix**

Add a test with `set_staging_copy_delay_for_testing(50ms)`, an explicit non-PTDS stream and no gate:
1. `get()` the future and assert that it resolved in well under 50 ms.
2. Immediately enqueue `cudaMemcpyAsync(D2H, stream)` and synchronize *that stream only*.
3. Assert the exact contents.

Without the join, the D2H reads `0xcc`. Run the same test as a mutation check: remove the join locally and confirm that it fails.

#### P3-R10 — The engine tests are flaky under CPU load (5 of 20 randomized runs fail)

**Evidence**: all CPUs busy; full output under *Verification performed*.

- **Gauge snapshot lags the AM-1 future.** Failures:
  - `test_engine_ioctx.cpp:506`: `async_registered_bytes` was 1 MiB or 3 MiB, expected ≥ 4 MiB;
  - `test_null_stream_ptds.cpp:264`: `after` was 13 MiB, expected ≥ 16 MiB.

  The credit settles in `copy()`. `snapshot = stats` happens only at the end of that loop iteration (`device_engine.cpp:1185`), behind a `try_lock`. The test thread wakes first.
- **Parked slice re-routed to the engine.** Failure: `test_engine_ioctx.cpp:182`, `submitted == 3` (expected 2). The first read is legacy-routed while probing, but is *parked* by the stream gate because the fixture's `memset` is still running under load. When it is re-dispatched (`cufile_reactor.cpp:1993`), the probe has finished, so `engine_route` hands it to the engine.
- **Timing-based QD assertion.** Failure: `test_engine_sched.cpp:42` measured `backlog_mean_storage_qd` 6.98 and 7.16, against the 7.2 threshold.

**Fix**
- Wait with `until(...)` for gauge-based expectations, or have `gauges()` document "eventually consistent" and offer a `flush_for_testing`.
- In the probe test, synchronize the fixture stream before the first read, or count only engine-routed pieces (via the completion observer).
- Decide explicitly whether a re-dispatched parked slice should consult `engine_route`. It is safe either way, but document it.
- Loosen the QD gate, or mark it `[.perf]` and run it in the benchmark job.

#### P3-R11 — `cufile_ioctx::start` eagerly warms every visible device in one control task, and silently drops the warm-up when the control executor rejects it

**Where**: `cufile_ioctx.cpp:43-87`.

**Problem**
- Any non-legacy context, including one that only ever reads on device 0, allocates and registers an arena and starts a reaper and submitter on *every* GPU. AM-5 allows up to 1 GiB per device. This conflicts with FR-28 ("per *active* device").
- All devices warm up sequentially inside one task on the single control thread. Capacity-rebalance tasks and the lazy pool start for an already-published device 0 queue behind device 1..N warm-up, so device 0's first engine reads can stall for seconds.
- `std::ignore = …submit(...)` discards a `false` return, which can come from `try_lock` contention or a full queue (capacity 64). The context then never binds an engine, and nothing is logged.

**Fix**
- Warm up a device lazily, the first time `engine_route` sees a device without a binding: schedule a per-device task, guarded by an atomic "warming" bit.
- Submit one task per device.
- Retry or log rejected submissions.
- Start the device's pools inside that task (P3-R3).

#### P3-R12 — Lookups that use `try_lock` make routing non-deterministic under contention

**Where**: `cufile_reactor.cpp:874` (`engine_for_device`), `device_engine.cpp:1434` (`try_get`).

**Problem**
- Several reactor workers share one `reactor_context`; the default is four workers.
- A concurrent `engine_for_device` call returns `nullptr` on a contended `try_lock`, which routes that slice to legacy.
- Throughput and completion semantics then vary per slice at random. This also feeds the P3-R10 flakiness and benchmark noise.

**Fix**
- Publish bindings through an immutable snapshot: `std::atomic<std::shared_ptr<const std::vector<…>>>`, or a fixed `std::array<std::atomic<engine_binding*>, max_devices>`, since bindings are append-only.
- Readers become wait-free and never miss.

#### P3-R13 — Fault paths that the tests do not reach

The following are untested:
1. `check_size` (`fstat`) and the `file_shrunk` path. It is only reachable for `cufile_io_object`, and the simulator file type skips it (`device_engine.cpp:598`).
2. The buffered `pread` → H2D fallback after a retry returns `-EINVAL` (`:557-571`, `:640-670`), including the "lane sync failed, retain scratch" branch.
3. AM-1 early success followed by `copy_ev` query failure. The future has already succeeded and the recovery path's `finish_error` is a no-op; assert the observable behaviour.
4. Retry when the grant is `qd==0` (P3-R4).
5. A short read with more than one retry, and a short read on a piece whose `physical.offset` is not the slice start.
6. A request on a busy stream with more pieces than the mount has lanes (P3-R1).

**Fix**
- Add a simulator-backed `io_object` test double that is accepted by `file_handle` and by a test hook for `buffered_fd()`, or use real temporary files with an injected transport.
- Add the cases above.

---

### NIT

- **P3-R14 (fences are recorded where they cannot be used).**
  - **Where**: `cufile_reactor.cpp:888-911`, `templated_ioctx.hpp:720-733`.
  - **Problem**: in non-legacy mode, fences are recorded for:
    - writes;
    - host+device pairs;
    - original-null slices;
    - callback-bearing slices;
    - reads on `known_compat` or unbound devices.

    Each costs an event record, a mutex and two allocations, with no consumer.
  - **Fix**: record only for device-only reads without `on_complete` whose original stream was not null and whose device has a binding. §7 #3 says compat mounts record no fences.
- **P3-R15 (lease order and reaper-side releases).**
  - **Where**: `device_engine.cpp:1030-1053`.
  - **Problem**:
    - The lane and slot are leased before `operations.try_acquire()`. A transient pool miss then releases both leases on the reaper, taking two blocking mutexes (`lane_lease::reset`, `staging_lease::reset`).
    - `lane_waits` is incremented on every loop while waiting, which inflates the gauge.
  - **Fix**: acquire the node and operation first. Count waits once per wait episode.
- **P3-R16 (`finish()` always calls `prove_recovery_quiescence()`).**
  - **Where**: `device_engine.cpp:493-501`.
  - **Problem**: the call removes the pool's independent "final event observed" guard. All call sites are quiescent today, but a future path will not be caught.
  - **Fix**: pass an explicit proof reason, `finish(n, ok, proof::event_observed | proof::nothing_enqueued | proof::lane_synchronized)`. Call the recovery proof only for the latter two.
- **P3-R17 (the reaper's catch-all strands nodes).**
  - **Where**: `device_engine.cpp:1286-1292`.
  - **Problem**: it fails every unfinished cursor but leaves in-flight nodes as they were. If `track_event` or `poll_later` throws in `copy()` after the join, that node has no deadline and no settlement, and drain never ends.
  - **Fix**: catch per node and `fail(n, …)` into recovery.
- **P3-R18 (cancellation does not wake the engine).**
  - **Where**: `cufile_reactor.cpp:1014`, coordinator failures.
  - **Problem**: facade shutdown and sibling failures are noticed only on the next poll, up to 20 ms when `live == 0`.
  - **Fix**: notify all bound engines after setting `cancel`.
- **P3-R19 (early success with a later copy failure is undocumented).**
  - **Where**: `device_engine.cpp:612-614` → `fail` → `finish(false)`, whose `finish_error` is a no-op.
  - **Problem**: a resolved success whose copy later fails only shows up as a CUDA error when the caller synchronizes.
  - **Fix**: document it (P3-R8) and count it in a gauge (`late_copy_failures`).
- **P3-R20 (original null streams stay legacy, contrary to AM-1).**
  - **Where**: `cufile_reactor.cpp:932`.
  - **Problem**: AM-1 lists null (pinned to `cudaStreamLegacy`) under the enqueued contract, but the implementation keeps null on legacy. That is conservative and fine.
  - **Fix**: record the deviation in AM-1.
- **P3-R21 (first configuration wins silently).**
  - **Where**: `device_engine.cpp:1441-1465`, policy registry.
  - **Problem**: a later context's different `async_engine_config` is ignored without the diagnostic that §7 #38 requires. Tests that boot the *process* engine inherit whichever test ran first: `test_null_stream_ptds.cpp:207`, `test_engine_ioctx.cpp:478`, `test_engine_read.cpp:156`. This creates order coupling.
  - **Fix**: compare configs and log once. Give tests isolated bindings.
- **P3-R22 (a reaper-driven probe can drop the last file reference on the reaper).**
  - **Where**: `mount_policy.cpp:781-790`.
  - **Problem**: `poll_timers()` runs on the reaper. After a cooldown it locks the probe's weak file reference and calls `ensure_probe`. When the local `context` dies, the reaper may hold the last `io_object` reference, which reopens the P2-R19 problem (deregister on the reaper) in an edge case.
  - **Fix**: hand the context to the control executor without materializing the file reference on the reaper.
- **P3-R23 (`assert(queued)` guards for ring pushes vanish in release builds).**
  - **Where**: `device_engine.cpp:459,851,1212`.
  - **Problem**: in release builds, a broken bound silently strands a node or mount forever.
  - **Fix**: on failure, `fail(n, no_buffer_space)` or finish with an error.
- **P3-R24 (`sync_read` can loop on `-EINVAL`).**
  - **Where**: `device_engine.cpp:557-566`.
  - **Problem**: `sync_read` can return `-EINVAL` from `pread`. With `attempts != 0`, `validate` then re-selects `fallback`, which can loop.
  - **Fix**: once `n.fallback` is set, treat any negative result as terminal.
- **P3-R25 (the quarantine test leaks a live reaper).**
  - **Where**: `test_engine_failure.cpp:328`.
  - **Problem**: the test deliberately leaks a fixture whose reaper keeps waking every 1 ms for the rest of the test process.
  - **Fix**: acceptable, but tag it, or provide a test-only "abandon" that parks the reaper at the idle timeout.
- **P3-R26 (unimplemented spec items should be tracked in AM).**
  - Lane replacement after a stream error (§7 #42). Lanes are reused after a successful sync.
  - Geometry boundaries and per-file granule are not wired. `engine_slice::granule` stays 4096 and `geometry_boundaries` stays empty.
  - The read-side probe uses `submit_max_us` without the FR-9 healthy-baseline seeding.

---

## Responses to the review charge

| Area | Assessment |
|---|---|
| Slot, lane, args or `io_object` released while a DMA, D2D or wait may touch it | No defect found. Retention lasts until the final event or a recovery proof, and the cleanup proof is off-reaper. Hardening suggested in NIT P3-R16 and P3-R17. |
| Credit exactly once | Verified on every path. |
| AM-1 retention until `copy_ev` | Verified. Errors also await sibling retirement. |
| PTDS host-complete | Verified, and tested with a GPU-side copy delay. |
| AM-2 fence placement | Verified. The fence gates only the D2D, never storage. **But see P3-R1**: the gate still consumes lanes and slots unboundedly. |
| Join issued after the `copy_ev` record, by the recording thread, before settlement | Verified (`device_engine.cpp:525-541`). Test strength: P3-R9. |
| Device context, event reuse, non-blocking lanes, null/legacy semantics | Verified. |
| Reaper vs submitter ownership, ring ordering, eventfd wake-ups | Verified. No data race found. |
| Facade shutdown vs engine drain | No deadlock found. Bounded 30 s with "incomplete drain". The cancellation wake-up is slow (P3-R18). |
| Reaper never blocks | Only `try_lock`s on hot paths. Two `lock_guard` releases on admission miss (P3-R15). |
| Recovery, quarantine, `not_submitted` fallback | Correct. Fallback coverage and classification gaps: P3-R5, P3-R7, P3-R13. |
| Short-read math | Correct per §6.11 / AM-9. Livelock when `qd==0`: P3-R4. |
| Cancellation of pending vs submitted work | Correct and tested. |
| `mode=legacy` unchanged | Verified, including no fences for the uring, REST and s3rdma backends. |
| Contract text | Needs a context-level rule and the datasource migration: P3-R8. |
| Tests | Strong T-29-style matrix. Ordering proof is weak (P3-R9). Flaky under load (P3-R10). Gaps listed in P3-R13. |
| NFR-3/4/5 hazards | P3-R2 and P3-R3 are the main risks for G-5. |

## Verdict: **CHANGES-REQUIRED**

No memory-safety, double-settlement or stream-ordering defect was found. The lifetime model is sound:
- AM-1 retention until `copy_ev`;
- error futures awaiting sibling retirement;
- off-reaper final releases;
- PTDS host-completion.

One blocking defect remains:
- **P3-R1**: a busy caller stream monopolizes its mount's lanes and slots, starving unrelated requests. This violates FR-26's liveness requirement, and AM-2's premise does not hold in this implementation.

Fix P3-R1 with an admission-side fence hint cap plus a real multi-request test, and record the mechanism as AM-12, before committing Phase 3.

Should-fix before the engine is used for measurements or the default is reconsidered:
- reaper scalability: P3-R2, P3-R3;
- liveness and fallback correctness: P3-R4, P3-R5, P3-R6, P3-R7;
- contract text: P3-R8;
- test determinism and flakiness: P3-R9, P3-R10;
- bootstrap and routing behaviour: P3-R11, P3-R12;
- coverage: P3-R13.


## Resolution

- **P3-R1 — fixed:** AM-12 fence-readiness hints bound gated leases across each mount; 16 MiB busy/1 MiB ready regressions cover both arrival orders, QD 1/4, and ready high/low priority.
- **P3-R2 — deferred to Phase 7 G-5:** implemented free/active node sets, O(1) cursor lookup, merged submit publications, throttled snapshots, deadline-driven backlog and CPU gauges; remaining request-index/DRR redesign, incremental accounting and parked-only notifications require high-R fairness/CPU benchmarks, not an unmeasured scheduler replacement in a correctness fix (AM-13).
- **P3-R3 — fixed:** bootstrap starts separate semaphore-driven cleanup/recovery/fallback executors; no lost wake-ups or idle 1 ms polling; held-fallback regression proves independent cleanup progress. Final resources remain off-reaper because existing release APIs take mutexes; batching/round-trip optimization belongs to R2's measured follow-up.
- **P3-R4 — fixed:** zero-QD retry switches to synchronous staging; deterministic two-mount rebalance regression covers the transition after a short read.
- **P3-R5 — fixed:** first unsupported/error completions and submit failures fall back after quiescence, opening the mount circuit; terminal errors stay terminal. Tests validate bytes and sync accounting for EINVAL/EOPNOTSUPP/ENOSYS/-1/EIO and submit failures; bounded transient backoff/finer per-file demotion is explicitly tracked for Phase 6 in AM-13.
- **P3-R6 — fixed:** facade reserves metadata before credit adoption and retains legacy routing on exhaustion; direct engine handoff keeps its bounded rejection contract.
- **P3-R7 — fixed:** buffered fallback leases the existing process-wide portable pinned scratch pool and never frees pinned memory on transfer completion; failed synchronization retains the scratch through recovery.
- **P3-R8 — fixed:** context-level dynamic-routing contract, graph-capture restriction, late-error semantics and datasource scalar/vector documentation updated; AM-1 migration list corrected. No src/cudf/ changes.
- **P3-R9 — fixed:** delayed-copy immediate consumer checks exact contents after synchronizing only the caller stream; removing the join was tested and failed the new regression. Delay is 200 ms rather than 50 ms to tolerate host scheduling without adding a fragile latency assertion.
- **P3-R10 — fixed:** gauge checks wait for eventual publication, probe fixture synchronizes before its first read, qualification eligibility is snapshotted before scheduling a probe (including worker preemption inside ensure_probe), parked legacy slices stay legacy, and timing-dependent QD mean is diagnostic rather than a CI throughput gate.
- **P3-R11 — fixed:** first device use schedules one guarded warm-up task for that device; start allocates no GPU resources; rejected submissions are logged and retried on subsequent use; pools start during bootstrap.
- **P3-R12 — fixed:** immutable atomic binding snapshots remove try-lock routing misses in both registries; concurrent context lookup regression checks 16,000 lookups with no misses.
- **P3-R13 — fixed:** injected buffered-fd extraction plus real temporary files cover shrink/fstat, retry-refusal pread/H2D, failed synchronization with retained scratch, terminal buffered errors, late copy-query recovery, repeated/nonzero-offset short reads, zero-QD retry and multi-piece busy streams.
- **P3-R14 — deferred to Phase 6 (remaining compat filter):** cheap write/pair/null/callback/unbound fence filters are fixed; known-compat filtering requires file/policy identity in the caller fence hook, which currently has only slices, so that hook/API extension is deferred.
- **P3-R15 — deferred to Phase 6 resource API hardening:** wait gauges now count episodes, not iterations; reordering acquisition alone would exchange lane/slot resets for a blocking operation-pool reset on misses, so nonblocking release/reservation APIs must precede that change (AM-13).
- **P3-R16 — fixed:** finish requires an explicit proof reason; final-event retirement retains the operation pool's independent observed-event guard instead of overwriting it.
- **P3-R17 — fixed:** per-node deadline and completion handling catches enqueue/tracking/deadline exceptions and enters owned-lane recovery instead of stranding the node.
- **P3-R18 — deferred to Phase 6 (sibling notification hook):** facade cancellation now wakes every bound engine; coordinator failures outside the engine still use bounded health polling because adding a cross-backend coordinator callback requires a separate lifetime/registration contract.
- **P3-R19 — fixed:** late copy-query failures are counted in late_copy_failures and covered by regression; context/datasource contracts document irreversible early success and synchronization-time CUDA errors.
- **P3-R20 — fixed:** AM-1 explicitly records original-null legacy routing; explicit cudaStreamLegacy may use the engine.
- **P3-R21 — fixed:** complete engine configurations are compared and differing later device/mount configurations log once; simulator policies retain isolated generations, while production integration tests intentionally exercise the shared process registry.
- **P3-R22 — fixed:** cooldown re-probe materializes the weak file owner only inside control work, never on the reaper.
- **P3-R23 — fixed:** bounded command, registration and survivor ring push failures now have release-build error/fallback handling instead of assert-only progress.
- **P3-R24 — fixed:** any negative synchronous fallback result is terminal; buffered pread error regression proves no re-entry loop.
- **P3-R25 — fixed:** quarantined/no-deadline reapers use the configured idle-health wait rather than waking every 1 ms; process-lifetime quarantine retention remains intentional.
- **P3-R26 — fixed:** AM-13 tracks lane replacement and geometry/granule wiring to Phase 6, and read-probe baseline seeding to Phase 7 qualification.

### Resolution verification

- Serial release build: `pixi run cmake --build build/release --target cucascade_io_tests -- -j 20` passed.
- Final full suite: **841 cases, 840 passed, one existing kTLS skip; 3,035,489 assertions passed**.
- Final randomized engine loop, seeds 1–20, with all **24 CPUs loaded**: **20/20 runs passed**, each **57 cases / 1,664 assertions** (1,140 case executions / 33,280 assertions total).
- An earlier loaded loop was 19/20 and exposed the additional qualification/preemption race described in R10; the full 20-run loop was repeated successfully after fixing it.
- Join mutation: removing only the caller-stream wait made the delayed-copy immediate-consumer regression fail on contents as expected; the join was restored and rebuilt before final validation.
- `pixi run python -m pre_commit run --files <changed>` passed after formatting; `git diff --check` passed.
- No commits, helper agents, concurrent builds or src/cudf/ edits. Default engine mode remains legacy; real-GDS and G-5 qualification are not claimed.
