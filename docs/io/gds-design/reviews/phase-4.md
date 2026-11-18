# Phase 4 review: small-slice coalescing (AM-6) and the engine benchmark (B-3/B-4/B-5)

- Branch `io/cufile-async-engine`; working tree reviewed against HEAD `bf775d1` (`git diff HEAD` plus untracked files).
- Scope:
  - Modified: `include/cucascade/io/cufile/{config,operation,device_engine,cufile_reactor}.hpp`, `src/io/cufile/{device_engine,cufile_reactor,cufile_ioctx}.cpp`, `CMakeLists.txt`, `test/CMakeLists.txt`, `test/io/cufile/{test_async_engine_config,test_engine_ioctx}.cpp`, `tools/gds/README.md`, `docs/io/gds-design/checkpoints/step-6-impl.md`.
  - New: `test/io/cufile/test_engine_coalesce.cpp`, `tools/gds/cufile_engine_bench.cpp`.
- Normative inputs: `06-design-amendments.md` (AM-1, AM-2, AM-6, AM-11, AM-12, AM-13), `07-implementation-plan.md` (Phase 4), `reviews/phase-3.md`, `AGENTS.md`.

## Verification performed

| Check | Result |
|---|---|
| `pixi run cmake --build build/release --target cucascade_io_tests cucascade_cufile_engine_bench -- -j 20` | Up to date (no work), no warnings |
| Full `./build/release/test/cucascade_io_tests` | **852 cases: 851 passed, 1 skipped** (the existing kTLS skip), 3,036,020 assertions |
| `"[engine]" --order rand`, seeds 1001–1010, run one after another | **10/10 pass**, 68 cases / 2,677 assertions each |
| Bench CLI negative cases (`--file` without a value, `--override fs_type`, `--qd 0`, `--op-mib 3`) | All rejected with clear messages, rc=1 |
| Bench B-4 (`restore`, 128 × 1..16 KiB columns, 2 threads × 3 tasks, QD 4, 1 MiB) on the existing `/tmp` smoke files, `--verify 1` vs `--verify 0` | legacy **0.363 → 1.093** GiB/s; simulated **0.607 → 1.378** GiB/s. The timed windows were 5–17 ms. See P4-R7. |

I did not modify any source code. This review is the only file I wrote.

## Confirmed correct, no finding

**Credit accounting**
- Each coalesced target's credit is *moved* from its cursor's `slice.credit` into `op->targets` (`device_engine.cpp:1229-1240`). There is no `add_tasks` call.
- The primary keeps Phase 3's rule: `piece.last ? move(slice.credit) : add_task(group)`. Grouping forces `piece.last`, so a grouped primary always moves its own credit and never creates a child.
- The test fixture's own `add_tasks(1)` for its extra large slice is test-side coordinator setup, not engine misuse.
- `finish()` (`:518`) and `copy()` (`:569-572`) settle every target. The repeated settlement in `finish()` after an early AM-1 success is the existing no-op on an already-settled credit.
- Coalesced cursors are marked `finished` with `active` incremented. That means:
  - `retire_cursors` never settles their moved-from credit;
  - the run-loop catch-all skips them;
  - they stay in `requests[]` (with their `stream_owner`, fence, `completion_retirement` and retirement ticket) until `cleaned` decrements `active` for every node target (`:871-873`).
- Everything that can fail before credit adoption does so before any credit moves: free node, lane, slot, operation, DRR deficit, and both `reserve` calls. The `reserve` failure handling has its own problem; see P4-R3.

**Short reads, retries and size checks**
- `validate()`, the retry loop and `check_size` use `required_end(op)`, the maximum target end (`:575-581`, `:590`, `:622`, `:762`).
- Retry arithmetic still uses `op.physical`, so a retry covers every target and gap. The test at `test_engine_coalesce.cpp:178` checks the restart offset and the staging buffer offset.
- Sync fallback (`not_submitted`, unsupported or zero-QD) re-reads the whole physical window, because `sync_read` uses `op.args` and `op.physical`.
- If any target fails, the whole op fails. This matches the request-level semantics: every target in a group belongs to the same `grouped_coordinator`, and any error already fails that request.

**Failure and cancellation**
- These paths settle every target, through `finish()` after quiescence:
  - copy-time cancellation;
  - validation error;
  - a fallback or retry cancelled under stop;
  - `size_checked` failure;
  - recovery after a partial copy enqueue (copy failure at index 0 or 3);
  - empty physical range.
- Tests cover these paths, check `tasks_remaining()==0`, and assert that destinations not yet copied are left untouched.
- Every cancellation check uses `n.request->cancelled()`. Eligibility requires equal `cancel` flags and equal `group`, so the primary's state stands for every target. This also holds for submitter refusal and `drain_refused_commands`.

**Eligibility (AM-6)**
- A target is eligible only if all of these match the primary (`device_engine.cpp:1110-1118`):
  - mount (`owner`), request (`group`), file object, device, priority, user stream, fence, PTDS origin, cancel flag, granule and merge gap;
  - the target is unstarted (`next == offset`);
  - its size is `< S_eff/2`;
  - it has no geometry boundaries.
- The test at `test_engine_coalesce.cpp:107` covers different stream, fence, file, priority, request and gap.
- Window limits:
  - gap ≤ `merge_gap_size()` (4096);
  - window end ≤ file size;
  - granule-aligned window ≤ `effective.op_bytes`;
  - the slot is leased at `effective.op_bytes`;
  - target count ≤ `coalesce_max_targets`.

  Targets are sorted by file offset, and the window starts at the aligned lowest offset, so `target.offset - physical.offset` is never negative and the copy range never passes the end of the window.

**Gap bytes, AM-1/AM-2 and PTDS**
- Gap bytes are never copied. Each D2D copies exactly `target.logical` from its staging offset into `target.user_ptr`. Tests check 0xcc canaries in gaps and destination padding, with destinations in reverse order.
- AM-1/AM-2 ordering holds for multi-target ops, all on the reaper thread (`:540-572`):
  1. one lane wait on the shared fence;
  2. N copies;
  3. one `copy_ev` record;
  4. one join on the shared user stream;
  5. settlement of all targets.

  Storage submission is still not gated by the fence. The ioctx test shows *one* fence wait plus *one* join per group (`wait_calls == 4` for 2 groups).
- PTDS: groups never mix PTDS and non-PTDS origins. There is no stand-in join, and all targets settle in `finish(…, event_observed)` after `copy_ev` is observed. The test at `:247` gates the producer and asserts `wait_calls == 1`.
- AM-12: a group holds one lease, so it counts once against the gated-hint cap (`gate_limited` uses the primary's hint, and all targets share the same fence event).

**Default behaviour**
- In `mode=legacy` nothing new happens:
  - the ioctx routing branch, the reactor publication flag and the engine paths are all guarded by `mode != legacy`;
  - `dispatch_slice` gets a defaulted parameter;
  - `coalesce_max_targets` validation accepts the default;
  - config equality is defaulted, so the new fields are compared.
- The new bench target is `EXCLUDE_FROM_ALL` and needs no cudf, following AM-8's "tests build the harness" pattern.

**Benchmark (things done right)**
- Labelling is honest:
  - "NOT GDS numbers";
  - `test_only_compat_admission=1` on stderr;
  - simulated and compat caveats;
  - the README says what the timed window includes;
  - engine-only columns are zero for legacy.
- The future is consumed, then the task stream is synchronized (AM-1), before any inspection or free.
- The task destructor observes a failed future before `cudaFreeAsync`.
- Warm-up gauges are subtracted, with additive integrals for QD.
- `--create-file` refuses to overwrite. Physical lane warm-up verifies the full window.

---

## Findings

### BLOCKING

None.

### SHOULD

#### P4-R1: The reaper allocates on every admission attempt, and on every coalesced op
- **Where:** `src/io/cufile/device_engine.cpp:1102`, `:1203`; `include/cucascade/io/cufile/operation.hpp:160-164` together with `operation_pool::try_release` (`std::destroy_at` / `construct_at`).
- **Problem:**
  - `std::vector<cursor*> targets{r};` heap-allocates on every `admit_one` call that finds a request. That includes large striped pieces that never coalesce, and attempts that then return early because of DRR deficit, lane wait, slot wait or a missing node.
  - Under lane or slot pressure, `admit()` repeats such attempts on every reaper wake.
  - In addition, `op->targets.reserve(n>2)` allocates the overflow vector on the reaper for every coalesced op. `try_release` destroys and reconstructs the operation, so that capacity is never reused.
  - Phase 3 and AM-13 deliberately removed per-piece reaper work. This adds a malloc/free on the hot path, and the comment "No allocations after credit adoption" is only half the requirement.
- **Fix:**
  - Use a member scratch `std::vector<cursor*>`, reserved to `max(coalesce_max_targets, 1)` in the `impl` constructor, and `clear()` it per call.
  - Give `operation` a preallocated target capacity: reserve `coalesce_max_targets - 2` once per pool record and keep it across reuse (reset the targets rather than destroying the record), or use a fixed-capacity array sized by the validated cap (≤ 4096).

#### P4-R2: Grouping costs O(R + G log G) per admission attempt, which is quadratic in group size, and repeats fence queries per candidate
- **Where:** `device_engine.cpp:1105-1125`.
- **Problem:**
  - Every attempt with a small, unstarted primary scans all of `requests[]` (up to `max_pending_slices_per_device`, 4096 by default) and sorts the group.
  - One op takes at most 64 targets. A G-slice request therefore costs ≈ (G/64) × (R + G log G). That is ≈ 64 × (4096 + 4096·12) comparisons for one 4096-column restore. Every *failed* attempt (deficit, lane or slot wait) redoes the scan and sort.
  - `fence_admissible(c, true)` runs for every matching candidate. Each unready cursor can issue `cudaEventQuery` on the *same* shared fence event every `fence_hint_us`.
  - This adds to the P3-R2 costs that AM-13 deferred, without recording it there.
- **Fix:**
  - At publication or accept time, keep a per-(mount, group) offset-sorted list of pending small cursors, and pop from its head.
  - Or at least:
    - do the grouping only after the resource and deficit checks pass;
    - stop scanning once `coalesce_max_targets` targets that fit the window are found;
    - compute the fence hint once per fence, not per cursor.
  - Record the remaining cost in AM-13's Phase 7 G-5 list.

#### P4-R3: An allocation failure in grouping either fails a read that could be served, or fails *every* pending request on the device
- **Where:** `device_engine.cpp:1175` (`free->targets.reserve`, outside any `try`), `:1202-1207` (`op->targets.reserve` … `throw;`), `:1108-1119` (candidate `push_back` inside the `try` whose catch fails `r`), and the run-loop catch-all at `:1482-1488`.
- **Problem:**
  - A `bad_alloc` from either `reserve` leaves `admit_one`/`admit` and reaches the run-loop catch-all. That handler calls `finish_error` on **every unfinished request on every mount**. It also releases the local `lane`/`slot` leases on the reaper.
  - A `bad_alloc` while collecting candidates fails the primary's read, even though coalescing is only an optimization.
- **Fix:**
  - Treat a failed grouping allocation as "do not coalesce": fall back to `targets = {r}` and the original `next_cut` piece. Or treat it as backpressure (`return false`, set `retry_needed`).
  - Never rethrow out of `admit_one`. P4-R1's preallocation removes most of these throw sites.

#### P4-R4: Every reactor worker iteration allocates a publication flag and wakes every engine, even when idle
- **Where:** `src/io/cufile/cufile_reactor.cpp:2166-2180`, `:2209`.
- **Problem:**
  - In non-legacy mode with `coalesce`, `publication.ready` is created on every loop iteration, whether or not anything was dispatched or handed off. `publish()` then always stores the flag and calls `notify_engines()`.
  - The worker loops every 2 ms when idle (`idle_wait_us`) and every 200 µs when legacy work is in flight (`poll_interval_us`). Each reactor therefore wakes every engine reaper 500–5000 times/s. Each wake runs a full reaper iteration, including the request and mount scans from P3-R2.
  - AM-13 lists "parked-only eventfd writes" as a Phase 7 prerequisite. This adds an unconditional new wake source.
- **Fix:** create the flag lazily on the first slice that is actually handed off with it. Call `notify_engines()` only if at least one slice consumed the flag. Better still, notify only the engines that received slices.

#### P4-R5: Engine slices stay invisible until the pass's *synchronous* legacy reads finish
- **Where:** `cufile_reactor.cpp:2183-2209` together with `dispatch_slice` (`:1324-1347`). As the worker comment says, "a cuFile or POSIX read completes inside `dispatch_slice`".
- **Problem:**
  - If one pass mixes engine handoffs with legacy-routed slices, the engine cannot schedule any handed-off slice until the pass ends. A pass covers up to `max_inflight` (32) legacy slices or `max_op_bytes` of legacy reads.
  - Slices go legacy because of metadata pressure (P3-R6's `try_reserve_slice` miss), mixed host/device slices, or a parked slice.
  - So under load the async engine idles behind blocking legacy I/O, and storage QD collapses exactly when the device is busy. The barrier was meant for batching only, and needs no such coupling.
- **Fix:** publish the current flag before any non-handed-off dispatch performs I/O. For example, `dispatch_slice` stores `true` on the legacy branch, or the loop publishes and re-arms after a `!handed_off` return. Add a test in which a held legacy read shares a pass with engine slices.

#### P4-R6: In non-legacy mode, every device vectored read is pinned to a single reactor
- **Where:** `src/io/cufile/cufile_ioctx.cpp:78-85`.
- **Problem:**
  - The branch applies to every `host_vector_async` with a device slice whenever `mode != legacy && coalesce`, which includes:
    - requests on mounts that are not (yet) eligible, which therefore run entirely on the synchronous legacy path;
    - mixed host+device requests;
    - requests made only of large slices that can never coalesce.
  - Phase 3 fanned those out across *all* reactors, least backlogged first. Pinning them to one reactor is a throughput regression for every legacy-routed vectored read in an engine-enabled context, which is exactly the qualification and fallback window.
  - The comment "independent tasks still spread out" does not cover one large task.
  - This routing change is not recorded in the amendments (see P4-R11).
- **Fix:** use a request-level publication barrier and keep the fan-out. `dispatch_slices` creates one flag per request, and a countdown over the selected reactors sets it after each reactor's worker has dispatched its share. Alternatively, restrict single-reactor routing to requests whose device slices are coalescing candidates (`n_slices > 1` and some slice < smallest-class/2) and whose mount is currently engine-eligible.

#### P4-R7: The benchmark's timed window is dominated by verification and per-task setup, so the README's B-4 table reports verification cost, not I/O
- **Where:** `tools/gds/cufile_engine_bench.cpp:314-337` (`consume`, called inside the timed loop), `:285-289` (`cudaStreamCreateWithFlags` and `cudaMallocAsync` per task, inside the window), `:526`; `tools/gds/README.md` (Phase 4 smoke table).
- **Problem:**
  - Verification is on by default. For each slice it performs 3 × (`pread` + D2H + `cudaStreamSynchronize`) inside the window: 384 host round trips for a 128-column restore task.
  - I reproduced the README's B-4 configuration (table above):
    - legacy went from 0.363 to 1.093 GiB/s and simulated from 0.607 to 1.378 GiB/s;
    - the legacy/simulated ratio changed from 1.67× to 1.26×;
    - the timed windows were 5–17 ms, too short to resolve either figure.
  - The implementer report presents "B-4 0.346 → 0.644", which overstates the effect of coalescing.
  - The window also contains:
    - per-task stream create/destroy;
    - `cudaFreeAsync` followed by a sync;
    - first-use pool growth. Warm-up allocates only one task's worth, while the pipeline needs `threads × pipeline` destinations; for B-5 that is +1 GiB of pool growth inside the timed window.
  - The README does say verification is included, but the table and the report still read as throughput comparisons.
- **Fix:**
  - Measure with verification *outside* the window. Either keep destinations and verify after `seconds` is taken, or run a verified pass and then a timed `--verify 0` pass in the same process.
  - Pre-grow the pool to `threads × pipeline × max(logical_bytes)` and pre-create the task streams before `start`.
  - Enforce a minimum duration or a repetition count, for example `--min-seconds`.
  - Regenerate the README table, or label its GiB/s column "verification-bound smoke; not comparable".

#### P4-R8: The CSV row does not identify how a run was admitted or configured, so compat rows look like async rows
- **Where:** `cufile_engine_bench.cpp:583-604`, `:372-377`.
- **Problem:**
  - The `engine` column reads `async` for both of these runs:
    - real `async_auto` with direct-evidence policy;
    - the TEST-ONLY `async_simulated_for_testing` capability bypass on compat hosts.
  - The bypass is recorded only on stderr.
  - The row also leaves out `coalesce`, operator overrides, `qd`/`op_mib`/`arena_mib`, `pipeline`/`sync_each`, `columns`/`col-dist`, the chunk range, the file path, the libcufile version and the GDS/compat state.
  - The README asks operators to sweep `--coalesce 0/1`, distributions and thread counts on several mounts and compare the CSVs. Rows from those sweeps cannot be told apart, and a compat-bypass row can be mistaken for a qualified async result.
- **Fix:** add the columns `admission` (`direct_policy | operator_override | test_only_compat | simulated`), `transport` (`libcufile | simulator | legacy`), `coalesce`, `qd`, `op_bytes`, `arena_bytes`, `pipeline`, `columns`, `col_dist` and `cache_state` (see P4-R9). Put the run configuration on the CSV, not only on stderr.

#### P4-R9: Cache state is uncontrolled, and the warm-up reads every measured restore file
- **Where:** `cufile_engine_bench.cpp:488-494` (warm-up reads *every* `.batch-N` object in restore), `create_file` (`fsync` without eviction).
- **Problem:**
  - The warm-up's stated purpose is to trigger policy probes, which needs one request per mount, not per file. In restore mode it re-reads every measured file, so legacy, compat and POSIX fallback runs measure the page cache.
  - Runs with `--create-file` start with the whole file in the page cache.
  - The project baseline is NVMe-fair gdsio/dd on `/mnt/disk_2`. The README recipes for `/mnt/disk_2` will compare page-cache legacy numbers with O_DIRECT GDS numbers.
  - The README notes "no automatic cache eviction", but none of its recipes evicts the cache.
- **Fix:**
  - Warm up only on a dedicated region or file outside the measured layout.
  - Add `--drop-cache` (`posix_fadvise(POSIX_FADV_DONTNEED)` on every measured file after warm-up and creation, after `fsync`). Report the cache state in the CSV.
  - Make cold-cache runs the default in the NVMe recipes.

#### P4-R10: Tests do not cover several of the new multi-target paths
- **Where:** `test/io/cufile/test_engine_coalesce.cpp`.
- **Problem:** the following behaviours are untested:
  - **Publication barrier.** Nothing proves deterministically that a batch which is not yet published is not scheduled. Every test hands off all slices and then publishes, so an early single-slice schedule would only show up probabilistically as a changed `read_calls`.
  - **Multi-target fallback.** No test sends a coalesced op through `not_submitted`, an unsupported completion, a submit error leading to sync fallback, the zero-QD capacity fallback, or `check_size` with a real `buffered_fd`.
  - **AM-1 with several targets.** P3-R9 added a delayed-copy test where a consumer synchronizes only the caller stream; there is no multi-target version of it.
  - **Error paths from P4-R3**, and the mixed legacy/engine pass from P4-R5.
- **Fix:** add these tests:
  1. hand off a batch whose flag stays false, wait, and assert `submitted == 0`; then publish and assert one read;
  2. a fault-injected coalesced `not_submitted`, an `-EOPNOTSUPP` completion, and a `qd == 0` grant; each must still produce exact contents across targets and gaps;
  3. a delayed copy across 8 targets, where synchronizing the caller stream alone yields exact contents.

#### P4-R11: Phase 4 design decisions are not recorded in the authoritative amendments
- **Where:** `docs/io/gds-design/06-design-amendments.md` is unchanged.
- **Problem:** several behaviours go beyond AM-6 and are recorded nowhere normative:
  - the per-pass publication barrier;
  - single-reactor ioctx routing in non-legacy mode;
  - the `coalesce_max_targets` cap (default 64, ≤ 4096);
  - `coalesce` defaulting to `true` for enabled modes;
  - grouped small slices skipping the striping grid;
  - whole-group failure semantics;
  - the benchmark's compat admission labelling.

  Phases 2 and 3 recorded comparable decisions (AM-11 to AM-13).
- **Fix:** add AM-14, "Coalescing implementation contract", covering the decisions above. Add to AM-13's Phase 7 list the grouping costs (P4-R1, P4-R2) and the publication wakeups (P4-R4) if they are not fixed now.

### NIT

#### P4-R12: Candidates read the live publication flag, contrary to the comment
- **Where:** `device_engine.cpp:1034-1036` vs `:890-891`.
- **Problem:** a candidate whose flag turns true mid-iteration can be grouped while its pass siblings are still in the control ring. This costs only grouping efficiency, not correctness.
- **Fix:** use `c.publication_visible` for candidates. The live load is then unnecessary.

#### P4-R13: `targets.front()` relies on an implicit invariant
- **Where:** `device_engine.cpp:1125`.
- **Problem:** it is only true implicitly that the primary passes its own candidate predicate. The cached fence hint, the unchanged `gated` value and the sticky visibility make it hold today. If it ever failed, `front()` would be called on an empty vector.
- **Fix:** seed `targets` with `r` and skip `r` in the scan. Or assert that the result is non-empty and fall back to `{r}`.

#### P4-R14: The whole-slice override applies even when nothing merges
- **Where:** `device_engine.cpp:1133-1138`.
- **Problem:** if siblings exist but none fits (`count == 1` after the loop), the single slice still skips lazy striping. Its piece shape then depends on whether unrelated siblings are pending.
- **Fix:** restore the original `next_cut` piece when `count == 1`.

#### P4-R15: Simulated mode serves every restore file from the largest file's image
- **Where:** `cufile_engine_bench.cpp:400-410`.
- **Problem:** verification passes only because the SplitMix pattern depends on the offset alone. User-supplied batch files with different contents would fail, or with uniform contents would silently mis-verify.
- **Fix:** document this limitation, or key the simulator image by file.

#### P4-R16: Bench verification cannot detect copy overruns
- **Where:** `cufile_engine_bench.cpp:290-294`, `:321-336`.
- **Problem:** restore destinations are packed in file order, and coalesced copies are enqueued in that same order. An overrun from target *i* into *i+1* is overwritten by the copy for *i+1*, and an overrun from the last target lands outside the allocation. Unit tests cover this case with canaries; the bench does not.
- **Fix:** for restore, optionally allocate padded destinations with canaries, permute the destination order, and verify full contents for columns of 64 KiB or less.

#### P4-R17: Two bench usability issues
- **Where:** `cufile_engine_bench.cpp:438`, `:511-522`.
- **Problems and fixes:**
  - `--threads` sets both the number of submitting threads and the number of `cufile_ioctx` reactors. Add `--reactors`, or document the coupling.
  - The sampler copies the full `engine_gauges` (histograms and mount vector) under `snapshot_mutex` every 100 µs. That makes the reaper's `try_lock` publication miss, and it reports only the *last* `Q_eff`/`S_eff`. Sample every 1–10 ms and report min and max.

#### P4-R18: Some gauges count operations where readers may expect targets
- **Where:** `device_engine.cpp:509-510`, `:536`.
- **Problem:** `late_copy_failures` and `cancellations` count once per coalesced *op*, while `coalesced_targets` counts targets.
- **Fix:** document the unit in `engine_gauges`.

---

## Responses to the review charge

| Area | Assessment |
|---|---|
| Credit accounting: exactly once, no `add_tasks` misuse, coordinator invariants | Verified. Credits move, never duplicate. Primary keeps the Phase 3 rule. Retained cursors keep owners until `cleaned`. |
| Short-read coverage and retry across targets | Verified (`required_end`, physical-window retry, `check_size`, sync fallback). Fallback paths untested: P4-R10. |
| Failure and cancellation settle every target | Verified on every `finish()` and `copy()` path. Allocation-failure blast radius: P4-R3. |
| Eligibility (stream, fence, device, file, priority, request; window ≤ `S_eff`; slot class) | Verified, and tested for every identity except device (enforced by `hand_off`) and PTDS origin. |
| Gap bytes never copied | Verified; canary-tested. |
| AM-1/AM-2 multi-target ordering | Verified: fence, then N copies, then record, then join, then settle, all on the reaper. One fence wait and one join per group. |
| PTDS | Verified: host-complete, no join, no mixing. |
| Performance: hot-path allocations, O(n²) grouping | P4-R1, P4-R2, P4-R4, P4-R5, P4-R6. |
| Bench measurement validity | Verification and setup inside the window dominate B-4 (P4-R7). Cache state uncontrolled (P4-R9). Stream sync placement correct (AM-1). |
| Bench does not mislead | README labelling is good. The CSV does not identify compat admission or configuration (P4-R8). Smoke table ratios are verification-bound (P4-R7). |
| CLI correctness | Parsing and validation correct, including override ordering, the `off_t` bound, and pre-create validation. Minor usability items: P4-R17. |
| Default behaviour unchanged | Verified for `mode=legacy`. Non-legacy routing changed (P4-R6). |

## Verdict: **APPROVED WITH REQUIRED FOLLOW-UPS** (no BLOCKING findings)

The coalescing core is correct. I found no defect in:
- double or missing settlement;
- stream ordering;
- gap-byte leakage;
- lifetime;
- PTDS handling.

It follows AM-1, AM-2, AM-6 and AM-12. The suite and 10 randomized `[engine]` runs pass. Phase 4 may be committed with the engine still `mode=legacy` by default.

Fix these before the engine benchmark is used for any B-3, B-4 or B-5 numbers, or for the default decision:
- reaper-side costs: **P4-R1, P4-R2, P4-R3, P4-R4**;
- routing and latency: **P4-R5, P4-R6**;
- benchmark validity and reporting: **P4-R7, P4-R8, P4-R9**;
- test coverage: **P4-R10**;
- documentation: **P4-R11** (AM-14).

## Resolution

- **P4-R1 — fixed:** bootstrap-reserved engine scratch/node targets and operation overflow; pool reuse preserves target capacity, covered by a repeated-reuse regression.
- **P4-R2 — fixed:** sort the per-request offset index once per arrival batch, advance consumed heads, group only after initial DRR/resource admission, stop at target/window limits, and reuse the primary fence hint; remaining high-R costs are explicitly tracked in AM-14 / Phase 7 G-5.
- **P4-R3 — fixed:** removed admission-time grouping allocations/reserves; optional planning exceptions retain the original single-slice plan and credits, never escape to the device-wide catch-all.
- **P4-R4 — fixed:** publication flags are created lazily only for real engine handoffs; idle and legacy-only passes allocate no flag and emit no publication wakeup.
- **P4-R5 — fixed:** publish the current batch BEFORE legacy parking/I/O; a held synchronous legacy-read regression proves the same pass's engine handoff continues.
- **P4-R6 — fixed:** retain Phase 3 fan-out for unqualified, mixed, null-origin and all-large reads; single-reactor batching is restricted to currently eligible device-only requests containing small slices, with routing regressions.
- **P4-R7 — fixed:** separate untimed verification, precreated pipeline streams/destinations/slice metadata and completed pool growth; whole passes satisfy minimum duration/repetitions, and the historical README table is explicitly verification-bound/non-comparable.
- **P4-R8 — fixed:** CSV records admission separately from transport, requested/effective configuration, coalescing, layout/chunks, file/overrides, platform/libcufile state, cache policy and repetitions; quoted path/override fields and compat admission are checked in smoke CSVs.
- **P4-R9 — fixed:** dedicated same-mount warm-up file; optional fsync + DONTNEED before EVERY measured pass, after verification/creation; advisory cache state is recorded and the NVMe recipe enables it.
- **P4-R10 — fixed:** added unpublished-batch, coalesced submit-error/unsupported/not-submitted/zero-QD fallback, real-fd short-read/file-shrink, retained-target-storage, multi-target delayed-copy, mixed legacy-pass and fan-out regressions; grouping allocation-failure sites no longer exist.
- **P4-R11 — fixed:** AM-14 records configuration, geometry, credits/failures, publication, restricted routing, benchmark contracts and residual Phase 7 G-5 work.
- **P4-R12 — fixed:** candidates use cached visibility; each shared flag is snapshotted exactly once BEFORE draining control, and newly encountered flags defer until the next iteration. This also fixes a mid-drain publication split exposed by the first randomized validation attempt.
- **P4-R13 — fixed:** scratch is always seeded with the primary; grouping never calls front on a possibly empty candidate set.
- **P4-R14 — fixed:** only groups with at least two targets override lazy striping; singleton plans retain the original next_cut.
- **P4-R15 — fixed:** README and AM-14 explicitly restrict simulator inputs to the harness's identical offset-dependent pattern; one largest-file image is not arbitrary multi-file storage.
- **P4-R16 — fixed:** optional reversed/padded destinations with per-target canaries, plus full-content checks for columns <= 64 KiB; guarded restore smoke passes for all three transports.
- **P4-R17 — fixed:** independent --reactors; sampler runs every 5 ms and reports observed Q_eff/S_eff minima and maxima.
- **P4-R18 — fixed:** engine_gauges documents physical-operation units for late copy failures and operation/unstarted-cursor units for cancellations.

### Fix-pass validation

All commands ran sequentially, without delegation, commits or changes to `src/cudf/`.

- Requested three-target release build: **passed, no compiler warnings**.
- Full `./build/release/test/cucascade_io_tests`: **860 cases, 859 passed, 1 existing kTLS skip; 3,039,817 assertions passed**.
- Final randomized `[engine]` loop: **10/10 passed**, **76 cases / 3,001 assertions** each; seeds **19840, 24577, 1191, 12737, 24327, 20776, 13866, 3003, 30749, 3384**.
- Benchmark checks: **9/9** sustained/restore/single × legacy/simulated/compat-libcufile verified smokes; **3/3** guarded restore modes; **1/1** guarded, non-coalescing, sync-each two-pass run. CSV admission/cache/layout/count checks passed; invalid reactor/repetition/duration/QD/op-size/override/missing-value arguments were rejected.
- `pixi run python -m pre_commit run --files <all changed files>`: **passed** after formatting.
- No GDS qualification or default-mode change. Broadcast/parked-only notifications and remaining scheduler/index-lookup costs remain explicitly scoped to **Phase 7 G-5**, not claimed solved by this fix pass.
