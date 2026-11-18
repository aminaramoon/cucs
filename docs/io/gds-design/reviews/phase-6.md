# Phase 6 review — hardening (6a) and operations (6b)

Reviewed: the working tree against `bd2a235` (`git diff HEAD` plus untracked files)
on `io/cufile-async-engine`. Inputs: AM-15/16/17 and the Phase 6a additions in
`06-design-amendments.md`, reviews of phases 3–5, `05-final-design.md` (§4.6, §4.11,
§4.13, §7, FR-12, FR-29, NFR-3/4), `AGENTS.md`, and both implementation checkpoints.
I did not change any source code. This file is the only file I wrote.

## Validation re-run (serial, compat host, libcufile 1170, NOT GDS)

| Step | Result |
|---|---|
| `cmake --build build/release --target cucascade_io_tests cucascade_cufile_engine_bench -j 20` | pass (no rebuild needed) |
| `./build/release/test/cucascade_io_tests` | **922 cases: 921 passed, 1 existing skip**; 3,036,846 assertions |
| `[engine] --order rand` ×10 (seeds 22714, 28201, 25127, 24106, 26710, 12103, 10220, 6569, 24170, 4338) | **10/10 pass**, 123 cases / 5405 assertions each |
| AM-15 sustained bench, `/tmp/p6a-bench.dat` (ext4; `/mnt/disk_2` absent), `--qd 32 --op-mib 4 --drop-cache`, verify on, run 1 | **19.77 queries/op** (I/O 1.39, copy 17.91, fence 0.47), 6.597 GiB/s, mean QD 28.30, 8 GiB through the engine, 0 legacy bytes |
| same, run 2 | **19.52 queries/op** (I/O 1.48, copy 17.57, fence 0.47), 6.574 GiB/s, mean QD 28.53, 0 legacy bytes |

The checkpoint's AM-15 numbers reproduce. The 70-queries/op ceiling holds on this
compat host; the target of about 8 does not. These numbers do not count as G-5 evidence.

## Focus-area summary

| Area | Finding |
|---|---|
| Polling correctness | **No missed completion found.** The deadline heap pops only entries that are due. Every `reap()` re-arms itself: `poll_later`, the fence-hint re-push, or a phase change that re-arms through the result path. A stale generation is dropped safely: generations now increase on admission (`device_engine.cpp:1783`), which closes the node-reuse hole. Copy events behind an unready fence are delayed by at most one `fence_hint_us`, never lost. The eventfd wake from the failure token is lossless. Nits: P6-R10, P6-R11. |
| Transient retry | Only reads retry, at most 3 attempts with 10/20 ms backoff and ≤25 % jitter; the backoff is a deadline, not a sleep on the reaper. Writes never replay: `validate_write` has no retry branch, and a write `submit_failed` sets `retry_after_recovery=false`. Production reachability: see P6-R9. |
| Lane replacement | Safe. The suspect lane is quarantined and synchronized on the recovery executor. The replacement is built before the swap. The old lane (stream, both events, cuFile stream registration) is destroyed after `synchronize_lane` succeeds and outside the pool mutex. If synchronization or replacement fails, the lane stays quarantined. The lease and the facade keep `_state` alive across the `quarantine_owner.reset()` under the lock. Every later event use re-tracks the new lane's handles. Destroying an event that a user stream still waits on is legal in CUDA. |
| Granule / geometry | The STATX_DIOALIGN read is correct. GPFS magic `0x47504653` is checked. The Lustre ioctl is guarded by `__has_include`. **However, the `f_bsize` fallback is wrong: P6-R1 (BLOCKING).** Lustre ioctl robustness: P6-R5. |
| Non-blocking leases / acquisition order | No deadlock: try-locks only, with fixed lane → slot → op order. The deferred-return queues are sized at bootstrap and checked before any acquisition. A missed `try_release_empty` is idempotent, because the half-retired op is revisited with `n.op` still set. Teardown note: P6-R13. |
| Coordinator hook | Exactly once: the `_fired` exchange, combined with registering under `_state_mutex` and checking `_continue` after the CAS and lock-swap, covers both orderings. Callbacks run outside the lock. The token is owned by the cursor and the callback captures a weak impl, so the hook survives engine retirement (tested). Nit: P6-R12. |
| Fence skip | Safe. `engine_route` (`cufile_reactor.cpp:1081`) and `engine_write_candidate` both require `slice.d_buffer.fence`. A slice whose fence was skipped therefore cannot reach the engine, even if the mount, file or engine state changes between recording and routing. A skip only loses an opportunity. Caller-thread cost and exception safety: P6-R3. |
| Parameter ordering | Correct. Order is setters → single `DriverOpen` → properties → readback; there is no `cuFileDriverGetP2PFlags` call and no close/reopen. External `DRIVER_ALREADY_OPEN` is reported as unapplied. Setter calls go through the opaque slots, gated by symbol and ID detection. Strict-mode semantics: P6-R6. |
| Affinity | Off by default. Only the reaper and submitter threads call `pthread_setaffinity_np`. Planning respects the cpuset (the CPU list is filtered by the permitted set). Fake-sysfs tests cover the planner. Mount-plan defect: P6-R4. |
| ABI | `cufile_api` uses unconditional opaque slots, and `io_config`/`cufile::config` no longer depend on any macro. A macro-cleared TU checks the layout statically and compares sizes. Nit: P6-R16. |
| Docs | Mostly accurate to the code (defaults, gauges, routing, gates). Inaccuracies: P6-R2, P6-R9, P6-R14. |
| Default behaviour | Unchanged. Legacy mode does not compute mount identity, record fences or initialize eagerly. `affinity` changed from true to false, but it had no effect before. `io_config` layout changed on purpose for ABI stability. |

## Findings

### P6-R1 — BLOCKING — `f_bsize` is not a direct-I/O alignment. Network filesystems get multi-MiB granules: GPFS falls back to legacy and NFS/VAST reads are inflated

`src/io/cufile/mount_policy.cpp:179` (`discover_file_geometry`), used by
`cufile_reactor.cpp:829-835,1003-1005,1093-1096,1470`. The behaviour is locked in by
`test/io/cufile/test_engine_hardening.cpp:222-223`.

**Problem.** When STATX_DIOALIGN is unavailable, the granule falls back to
`fstatfs().f_bsize`. `f_bsize` is the *preferred transfer size*, not an O_DIRECT
alignment. On the network filesystems this phase targets it is large:

- GPFS reports the filesystem block size, typically 4–16 MiB.
- NFS/VAST reports `wsize`, commonly 1 MiB.
- BeeGFS reports the chunk size.

None of these filesystems report STATX_DIOALIGN, and kernels older than 6.1 never
report it, so the fallback is the normal path on these mounts.

Consequences:

1. **GPFS loses the engine.** Take a block size above 1 MiB with the default arena,
   which prepares only 1 MiB slots. Then `supports_file_granule` fails, because
   1 MiB % granule ≠ 0, and every GPFS file is routed to legacy. The new test checks
   exactly this: an 8 MiB granule. This is a regression from Phase 5, which used a
   4096-byte read granule. It also makes the new GPFS geometry code dead in practice.
2. **NFS/VAST and BeeGFS reads are rounded out to 1 MiB / 512 KiB.** A 4 KiB column
   read becomes a 1 MiB physical read, up to 256× amplification.
3. **All writes on these mounts go to legacy**, because `granule > write_granule()`.

Separately, a STATX_DIOALIGN result of `{0,0}` means the file has no direct-I/O
support. Today that result also falls back to `f_bsize` instead of being treated as
"not direct-capable".

**Fix.**
- Never derive the alignment from `f_bsize`. Without DIOALIGN use
  `max(4096, page size)`, which matches Phase 5, or a small per-filesystem table of
  known O_DIRECT alignments (GPFS 512 → 4096; NFS byte-granular → 4096). Keep
  `f_bsize` only as GPFS *geometry*, behind the magic check.
- When DIOALIGN reports `{0,0}` with `STATX_DIOALIGN` set in `stx_mask`, mark the
  file not direct-capable (legacy) instead of falling back.
- Update the test to expect a 4096 granule plus 8 MiB GPFS geometry, and add an NFS
  case with `f_bsize = 1 MiB`.
- Update the AM text ("or fstatfs block size when unavailable") and
  `cufile-reactor.md` ("statfs block fallback").

### P6-R2 — SHOULD — Per-file demotion never escalates, so whole-mount or driver failures are paid once per file, each time with a lane rebuild

`device_engine.cpp:752, 910, 1247-1255`.

**Problem.** Every non-transient negative completion or submit failure now sets only
`file_policy->{read,write}_demoted`; the mount circuit no longer opens (the
`open_circuit()`/`revoke_writes()` calls were removed). This also covers errors that
are not file-specific:

- cuFile driver or CUDA status codes from submit (`rc.error` is the unmodified
  `CUfileOpError`);
- `-EIO` from a failing device or client;
- a broken nvidia-fs path.

On such a mount, every file pays one failed async submit plus a recovery. Recovery
now *recreates the stream and both events and calls `cuFileStreamRegister`* each
time. Only watchdog and slow-submit events still open the mount circuit. The
operations guide (`cufile-reactor.md:64`) says "file-specific unsupported operations
demote only that live inode". The code does this for **every** non-transient error.

**Fix.** Pick at least one, and make the docs match:
- classify submit codes (file-specific, such as `CU_FILE_IO_NOT_SUPPORTED`, versus
  driver/CUDA/device codes that should open the mount circuit);
- escalate to `open_circuit()` after N distinct-inode demotions within a window
  (for example 3 in 10 s).

### P6-R3 — SHOULD — `record_submit_fences` now runs mountinfo, statx, ioctl and policy creation on the caller thread, and it is not exception-safe

`cufile_reactor.cpp:980-1028`, called from `templated_ioctx.hpp:732` on the caller's
thread.

**Problem.**
- The first submit for a file runs all of the following on the application thread:
  `mount_identity_info()` (`/proc/self/mountinfo` parse, `statx`, `fstatfs`, the
  optional Lustre ioctl), `file_async_policy::get` (fstat plus the global registry
  lock), and `mount_policy_registry::get_or_create`.
- After that, every device slice repeats both registry lookups inside the loop.
- This contradicts the Phase 5 invariant written at `engine_write_candidate`: "no
  mountinfo, policy creation or probe scheduling on the caller thread".
- `file_async_policy::get` throws on an `fstat` failure (`mount_policy.cpp:146`), and
  `get_or_create` can throw. Either exception escapes `dispatch_slices` *before* the
  coordinator exists, so the user's I/O fails instead of taking the legacy path.
  `engine_route` already uses a try/catch that falls back to legacy.

**Fix.**
- Wrap the per-slice policy checks in `try { … } catch (...) { continue; }`. Skipping
  the fence is always safe.
- Hoist the per-file lookups out of the loop.
- If the file's identity has not been computed yet, keep the old behaviour (record
  the fence) instead of discovering it on the caller thread. Let the reactor worker
  compute the identity, for example by testing a computed-flag next to
  `_mount_identity_once`.

### P6-R4 — SHOULD — Mount affinity plans thrash and can index the wrong override

`device_engine.cpp:446-457, 485-499, 527`; `mount_policy.cpp:453`.

**Problem.**
1. `affinity_override()` returns the index of *any* matching override, including
   QD-only, legacy or known-compat entries. Every override gets its own plan, picked
   from a copy of the core-reservation list, so it gets different cores from plan 0.
   A shared submitter that serves one overridden mount and one plain mount therefore
   calls `pthread_setaffinity_np` and migrates on almost every submission. This is
   on the submit hot path.
2. The plans are built from the engine's *first* configuration's `mount_overrides`.
   The index comes from the mount policy's own `_config`, which belongs to whichever
   context created that policy. If a later context has a different override list,
   the index selects another entry's plan, or silently falls back to plan 0 when out
   of range.

**Fix.**
- Build a mount plan only for overrides that set `nic` or `cpus`; all others use
  plan 0.
- Resolve the plan from the override's selector `{match, value}` against the
  engine's configuration, not from a positional index into another config.
- Optional: rate-limit or skip re-pinning when the CPU would not change.

### P6-R5 — SHOULD — The Lustre stripe ioctl has never been compiled and is unvalidated

`mount_policy.cpp:41-47, 185-193`.

**Problem.**
- The branch is compiled only when Lustre headers are present, which is not true on
  any CI or dev host, so it has never been built or run.
- It passes a 32-byte stack `lov_user_md_v1` to `LL_IOC_LOV_GETSTRIPE`. `llapi` sizes
  this buffer for `LOV_MAX_STRIPE_COUNT` (V3) because the kernel's copy-out size
  depends on the layout.
- It never checks the returned `lmm_magic`. For PFL/composite/DoM files the client
  may return a `lov_comp_md_v1` header, which would then be read as a stripe size.
- `lmm_stripe_size` is not checked against the granule: it should be positive and a
  multiple of the granule.

**Fix.**
- Use a heap buffer of `lov_user_md_size(LOV_MAX_STRIPE_COUNT, LOV_USER_MAGIC_V3)`,
  or `llapi_layout_get_by_fd` if linking `liblustreapi` is acceptable.
- Accept only `LOV_USER_MAGIC_V1/V3` with a stripe size that is nonzero and a
  multiple of the granule; otherwise use the 1 MiB fallback.
- Add a compile-only shim header in CI so the branch is built at least once.

### P6-R6 — SHOULD — Strict mode accepts readbacks that prove a parameter did not apply

`runtime.cpp:208-224, 264-276`; `cufile-reactor.md` "Strict mode … permits unverified values".

**Problem.** "Readback differs" is folded into `unverified`, and strict mode does not
reject `unverified`. On this host, `max_direct_io_size_kib` 16384 read back 1024 and
`max_device_cache_size_kib` 131072 read back 16896. That is positive evidence the
request did not take effect, yet `strict_parameters=true` passes. "Getter
unavailable" and "getter contradicts" are different facts.

**Fix.** Add a `differs` status, or keep the status and add a flag. Strict mode
should reject `differs`, or provide an opt-in `strict_readback`. Keep `unverified`
for getters that are missing or fail. Update AM-17 and the guide.

### P6-R7 — SHOULD — Small-slice coalescing is silently turned off on every filesystem that reports geometry

`device_engine.cpp:1702, 1718`.

**Problem.** Grouping now requires `!geometry_bytes`. GPFS (validated), Lustre and
BeeGFS always report geometry. The Phase 4 coalescing feature (AM-6) is therefore
disabled on the main network filesystem classes. Nothing documents this, and the
checkpoint has no benchmark showing its effect.

**Fix.** Allow grouping when the grouped physical range stays inside one geometry
unit, i.e. `offset / geometry == (end - 1) / geometry`. Alternatively, document it
in AM-6/AM-17 as a known performance limitation.

### P6-R8 — NIT — `fence_hint_us` now delays copy-completion detection but has no upper bound

`config.hpp:180, 247`; `device_engine.cpp:956-972`.

`fence_hint_us` used to gate only admission hints. It now also holds back copy-event
queries. The I/O and copy caps were bounded to [1, 1000] µs; `fence_hint_us` is only
checked for positivity. A large value, such as 1 s, would delay every copy completion
and lane return. **Fix:** validate `fence_hint_us ≤ 1000`, or cap the deferral at the
copy ceiling.

### P6-R9 — NIT — Transient-errno retry is unverified against real cuFile error encoding

`device_engine.cpp:860-865, 1247-1255, 1296`; `async_transport.hpp:43`.

The submit `rc.error` is the "unmodified cuFile error", a positive `CUfileOpError`,
so `-ESTALE`/`-EAGAIN` never match on a real submit failure; only the
simulator or seam produces those. Async `bytes_done` failures can be `-1`/`-CU_FILE_*`
rather than `-errno`. As a result, the bounded transient retry is exercised only by
the simulator and by the compat or sync-fallback `pread`. The AM and the guide
describe it without that caveat. **Fix:** document this limitation, or map the
relevant `CU_FILE_*` codes and add a test with real error encodings.

### P6-R10 — NIT — The defensive `reap()` early return drops a deadline without re-arming it

`device_engine.cpp:955`. With a monotonic clock this line cannot trigger, because the
caller pops only deadlines that are due. With a fake clock that runs backwards it
would drop the node's only deadline and stall the node. **Fix:** re-push
`{d.due, &n, n.generation}` instead of returning.

### P6-R11 — NIT — Per-class completion histograms put direct (unstaged) ops in the 1 MiB read/write class

`device_engine.cpp:627-632`. `completion_history` uses `capacity_mib` when there is no
staging slot, so an 8 MiB direct write updates the 1 MiB write median. This only
affects the poll cap. **Fix:** use `op->physical.size` for classification.

### P6-R12 — NIT — Failure-token registration is O(n²) per group

`io_request.hpp:103`. Every registration runs `erase_if(expired)` over all tokens
under `_state_mutex`. Tokens stay live while their cursors exist, so a request with
thousands of slices does quadratic work on the hand-off thread. **Fix:** prune only
when the size passes a doubling threshold.

### P6-R13 — NIT — The final-owner refusal in the lease `try_reset` can livelock teardown

`device_arena.cpp:893`, `lane_pool.cpp:199`, and the run-loop exit condition
`device_engine.cpp:2119-2121`.

If the arena facade is gone (`options.arena` is a raw pointer) while a slot sits in
`slot_returns`, `try_reset` refuses forever. The reaper then spins every 100 µs and
never satisfies `*_returns.empty()` at stop. Production arenas live for the whole
process, but nothing asserts that. **Fix:** document or assert that the arena
outlives the engine, or hand final-owner leases to the cleanup executor.

### P6-R14 — NIT — Doc and code mismatches

- `cufile-reactor.md:138`: "The no-argument overload joins the frozen runtime." If it
  runs first (legacy file open at `cufile_reactor.cpp:227`, `platform.cpp:67`, or
  arena/lane bootstrap), it *freezes an empty request*. Later process parameters are
  then ignored, and strict mode throws. Say so.
- `cufile-reactor.md:314`: `nfs-rdma` is not a kernel filesystem type, so this row
  cannot be reached from mountinfo.
- Mount profile `granule`/`geometry_bytes`/Lustre-BeeGFS `preferred_op_bytes` come
  from the *first file* that creates the mount policy (`mount_policy.cpp:431-449`).
  Lustre stripe size is per file. Describe these values as the first-file sample, or
  derive them only from the filesystem default.
- Checkpoint 6a reports that before the change, verification "caused demotion before
  timing", but gives no root cause. Record which breaker tripped, so that later work
  does not rely on a symptom that disappeared without explanation.

### P6-R15 — NIT — Affinity bookkeeping robustness

- `device_engine.cpp:446`: the process-static core reservation list `used` never
  shrinks. Engines that are created and retired (tests, generation bumps) use up
  physical cores, so later plans report "SMT/shared".
- `topology.cpp:114`: one broken `/sys/class/infiniband/*/device` symlink makes
  `canonical()` throw and turns off **all** placement. Skip that NIC instead.
- Discovery reads the *bootstrapping thread's* mask (`sched_getaffinity(0)`), not the
  process cpuset. This is conservative and consistent with thread inheritance, but
  the guide says "process-allowed mask".

### P6-R16 — NIT — The ABI check compares two test TUs, not the library

`test_cufile_abi*.cpp`. Only `cufile_api::abi_size()` comes from the compiled
library; the config size and alignment reference is another TU inside the test
binary. This is fine today, because it uses the same INTERFACE definitions. **Fix:**
for a stronger check, export `cufile_config_abi_reference()` from the library.

## Verdict

**CHANGES REQUESTED: 1 BLOCKING, 6 SHOULD.**

- **BLOCKING:** P6-R1 (`f_bsize` used as the DIO granule: GPFS goes to legacy,
  NFS/BeeGFS reads are rounded out to 1 MiB, network writes go to legacy).
- **SHOULD:**
  - P6-R2: per-file demotion never escalates to the mount, and every recovery
    rebuilds a lane.
  - P6-R3: the fence path does caller-thread mountinfo/policy work and is not
    exception-safe.
  - P6-R4: affinity plans thrash and can select the wrong override.
  - P6-R5: the Lustre ioctl has never been compiled and is unvalidated.
  - P6-R6: strict mode accepts readbacks that differ.
  - P6-R7: coalescing is off on geometry filesystems.

Everything else in Phase 6 holds up on review and in the re-run tests:

- polling correctness;
- write no-replay;
- the read transient bound;
- lane replacement quiescence;
- lease acquisition order;
- the exactly-once coordinator hook and its lifetime;
- fence-skip safety;
- parameter ordering, with no P2P-flags call;
- owned-thread-only affinity;
- macro-independent ABI layout;
- default legacy behaviour.

The AM-15 ceiling (≤70 queries/op) reproduces at about 19.5–19.8 on compat. Once
P6-R1 is fixed and the test expectations are updated, Phase 6 can be accepted, with
the SHOULD items tracked into Phase 7 if they are not fixed now.

## Resolution

- P6-R1 — **fixed**: unavailable DIO alignment uses max(4096, page size), never f_bsize; reported zero disables direct routing. GPFS keeps validated 8-MiB geometry with a 4-KiB granule; NFS 1-MiB wsize regression coverage added; AM/guide corrected.
- P6-R2 — **fixed**: recognized EINVAL/EOPNOTSUPP/CU_FILE_IO_NOT_SUPPORTED refusals remain inode/direction-local; other non-transient device/driver/CUDA failures open the mount circuit, including CUDA submit/event-record failures. Real vendor-code and errno regressions check fresh-inode peers do not pay repeated failures; writes still never replay.
- P6-R3 — **fixed**: release/acquire cached-identity publication, lookup-only mount registry, hoisted file/per-device policy checks and exception-safe fence recording. Cold files record conservatively without discovery; workers discover/create policy. Caller-side coalescing also uses cached metadata only; cold/warm routing tests updated.
- P6-R4 — **fixed**: only nic/cpus overrides allocate plans; cross-context selection resolves match/value selectors against the engine configuration. Non-placement overrides use plan zero, unchanged CPUs avoid re-pinning, and reordered/different-list regression tests cover lookup.
- P6-R5 — **fixed**: heap-allocated maximum-stripe V3 response buffer, V1/V3-only layout acceptance and nonzero granule-multiple stripe validation. A per-TU external-UAPI shim compiles and exercises the production adapter, including a maximum-size copy-out, composite rejection and ioctl failures. Real Lustre client qualification remains Phase 7 hardware work, not evidence supplied by the shim.
- P6-R6 — **fixed**: contradicting readback reports differs and strict mode rejects it; missing/failed getters remain unverified. Injected and production-adapter tests plus AM-17/guide cover the distinction.
- P6-R7 — **fixed**: small reads coalesce within one matching repeating-geometry unit, never across its boundary; explicit boundary vectors still disable grouping. Exact-destination tests cover 64-KiB and 8-MiB geometry, including boundary splits; AM-14/guide updated.
- P6-R8 — **fixed**: fence_hint_us validates in [1, 1000] us, with endpoint/out-of-range tests.
- P6-R9 — **fixed (documentation)**: AM/guide explicitly limit transient matching to negative errno-producing paths; raw positive CUfileOpError submits and negative CU_FILE_* completions are not mapped to transient errno. Real vendor/client error-encoding qualification remains Phase 7 hardware work.
- P6-R10 — **fixed**: the defensive backwards-clock reap path re-pushes the same deadline instead of dropping it.
- P6-R11 — **fixed**: unstaged completion-history classification uses physical operation size, with a safe sub-MiB lower bound.
- P6-R12 — **fixed**: expired weak failure tokens are pruned only at a doubling threshold, amortizing registration scans; exactly-once/lifetime tests remain passing.
- P6-R13 — **fixed (documented lifetime contract)**: options and operations guide explicitly require the arena facade to outlive the engine, including incomplete drain/quarantine, and explain the final-owner deferred-return teardown hazard. Production arenas are process-lifetime.
- P6-R14 — **fixed (documentation)**: default-freezing no-argument initialization, reachable NFS type names and first-file profile sampling are clarified. The unsupported historical claim that verification caused demotion is withdrawn: the old CSV/logs did not capture the triggering breaker, so no root cause is invented.
- P6-R15 — **fixed**: weak engine-owned core reservations are pruned/reusable after retirement (incomplete engines retain ownership), broken NIC symlinks are skipped independently, and the bootstrapping-thread mask is documented accurately; retirement and broken-symlink regressions added.
- P6-R16 — **fixed**: config size/alignment reference now comes from an exported function compiled into the library, rather than another test TU's sizeof expressions; the macro-cleared consumer comparison passes.

### Fix validation (serial, libcufile 1170 compat host, NOT GDS)

Continued the existing `/gsd:execute-phase 6` hardening workflow with the task list;
this harness has no executable GSD command. No delegation, concurrent builds,
commits, branch changes or `src/cudf/` edits. Default mode remains legacy.

- Requested build: `pixi run cmake --build build/release --target cucascade_io_tests cucascade_cufile_async_qd cucascade_cufile_engine_bench -- -j 20` — passed after formatting.
- Final full suite: **929 cases, 928 passed, 1 existing kTLS skip; 3,039,701 assertions passed**. An earlier run caught one cold-file coalescing fixture assumption; that test now explicitly warms metadata, and an additional regression proves cold caller fencing never discovers it.
- Randomized `[engine] --order rand --rng-seed $RANDOM`: **10/10 passed**, **129 cases / 6,834 assertions each** (1,290 case executions; 68,340 assertions). Seeds: 19211, 26798, 10868, 13759, 230, 18578, 20368, 9199, 6106, 12218.
- Final verified AM-15 sustained benchmark: **19.638 queries/op** = I/O **1.428**, copy **17.741**, stage **0**, fence hints **0.469**; **6.574 GiB/s**, mean QD **28.313**, 8 GiB engine bytes, **0 legacy bytes**. The 70 ceiling passes; the approximate target of 8 is not met. `/mnt/disk_2` is absent; used `/tmp/p6a-bench.dat` on ext4 with `--workload sustained --engine async --override fs_type:ext4 --require-direct-evidence 0 --qd 32 --op-mib 4 --drop-cache`, default verification enabled. This is not G-5 or direct-I/O evidence.
- `pixi run python -m pre_commit run --files <all changed/new files>` — passed on final rerun; initial pass applied clang-format/cmake-format. `git diff --check` — clean.
- Logs: `/tmp/p6-fix-build-final.log`, `/tmp/p6-fix-tests-final.log`, `/tmp/p6-fix-engine-loops.log`, `/tmp/p6-fix-engine-{1..10}.log`, `/tmp/p6-fix-bench.log`, `/tmp/p6-fix-precommit-final.log`.
