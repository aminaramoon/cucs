# Phase 2 review: resources and policy

- Branch: `io/cufile-async-engine`, working tree compared with HEAD `32767a1`.
- Scope:
  - 2a: `runtime`, `device_arena`, `lane_pool`, `operation` / `operation_pool`, `capacity`.
  - 2b: `mount_policy`, the `cufile_transport` seam, `transport_simulator`.
  - Also the changes to `cufile_api` (`driver_open_state`) and `cufile_reactor` (cached mount identity).
- Checked against:
  - `05-final-design.md`: FR-6..FR-9, FR-12..FR-17, FR-27, FR-28, §4.4, §4.5, §4.7, §6.3–§6.5, §6.8, §6.10;
  - `06-design-amendments.md` (all of it, especially AM-1, AM-2, AM-4, AM-5, AM-9, AM-10);
  - `reviews/phase-1.md` and AGENTS.md.
- Reviewed: every hunk of `git diff HEAD` and every untracked source and test file.

## Verification performed

| Check | Result |
|---|---|
| `pixi run cmake --build build/release --target cucascade_io_tests -- -j 20` | Up to date. Builds clean under `-Werror -Wconversion -Wsign-conversion`. |
| Phase 2 tags (`[cufile][arena]`, `[lane_pool]`, `[operation]`, `[runtime]`, `[mount_policy]`, `[capacity]`, simulator, real transport) | 103 cases, 20 753 assertions, all pass. |
| Full `cucascade_io_tests` | **750 cases: 749 passed, 1 skipped; 3 028 785 assertions.** Matches the implementer's report. One earlier run aborted at startup with an rmm 2 GiB `out_of_memory`, while another process was using the GPU. A rerun on an idle GPU passed. Environmental. |
| `[integration]`: real libcufile, QD 4, per-slot registration | Passes. |
| `[arena_reproducer]`: real libcufile, QD 4, **shared arena registration** | **Fails 3 of 3 runs, deterministically, with silent data corruption.** `bytes_done == size`, but the slot contents are wrong in the first concurrent round. See P2-R1. |
| P0 harness `--register arena --qd 4 --op-mib 1` on the same host | Reports `bytes_done_mismatches = 0`. The harness never checks data contents. See P2-R1. |
| Scratch CUDA probe (`/tmp/p2rev/hb.cpp`) | `cudaMallocHost(1 MiB)` takes 357–715 µs. `cudaFreeHost` **blocked 3.04 s** behind a host function blocked on *another* non-blocking stream. A host function on a second stream still ran while the first was blocked. See P2-R2. |

## Confirmed correct, no finding

- **Lease lifetime.**
  - `staging_lease` and `lane_lease` are move-only.
  - Each lease holds a `shared_ptr` to its pool or arena state, so destroying the facade never invalidates a live lease.
  - Self-move is safe.
  - Releasing a lease is metadata only: no `cudaFree`, no deregistration.
  - `device_arena::~device_arena` is defaulted and `detail::device_arena_state` has no resource destructor. `release_for_maintenance()` is the only free or deregister path. It checks `live == 0` and sets `maintenance` under the same lock, deregisters before it frees, and keeps resources and admission closed on any callback failure.
- **Registration base/offset (AM-9).**
  - `arena`: base is the registration-group base; offset is `registration_offset + slot_offset`.
  - `slab`: base is the slab base; offset is `slot_offset`.
  - `slot`: base is the slot, offset 0. Unregistered slots also use base = slot, offset 0.
  - The incremental no-BAR1 arena mode keeps per-group bases. `test_device_arena.cpp:287` checks this.
  - Reformat never touches the geometry of a registered slot-mode slab (`can_reformat`).
- **Partial registration failure (FR-14).**
  - Registration stops globally at the first failed unit and keeps the earlier successes.
  - A partially registered slot-mode slab exposes its remaining slots only to explicit `need_registered=false` requests.
  - Throwing callbacks count as failures.
  - Metadata vectors are reserved before any device resource is acquired, so `push_back` cannot throw mid-warm-up.
- **BAR1 math.** `min(allocated, pin_headroom, fraction × max(free_BAR1 − headroom, 0))` matches §4.4 and FR-14. The `long double` conversion avoids rounding up to `SIZE_MAX+1`. AM-5 auto sizing is `floor16(min(1 GiB, 5 %, reservable))`. Unused reservation grants are returned, and the charge is retained if the rollback throws.
- **Quota and reclassification never revoke a lease.**
  - `progress()` changes the owner or class of a slab only when `live == 0`.
  - `spare()` requires `live == 0`, so a borrowed spare is never reassigned.
  - At most one slab per mount drains, across both tiers.
  - `try_lease` skips draining slabs.
  - Borrowing is limited to idle slabs or slabs the same borrower already holds.
- **`lease()` never blocks.** `device_arena::try_lease`, `lane_pool::try_lease` and `operation_pool::try_acquire` all use `try_to_lock` and never allocate. `test_device_arena.cpp:595` proves leasing does not wait behind warm-up registration.
- **Stable-address op pool.**
  - Records are a fixed `unique_ptr<operation[]>`.
  - `async_args` uses exactly the cuFile pointer types.
  - Records self-retain (`live_owner`) until the last allocated record is retired.
  - Release is refused unless the op is quiescent (final event observed or recovery proof) and holds no QD token.
  - The event generation survives reuse, and stale `recorded_event`s are rejected by generation, handle and phase.
- **Exactly-once credit.**
  - `operation_credit` settles through `atomic<bool>::exchange`.
  - Moves transfer the settled flag atomically.
  - Destruction cancels an unsettled credit, never succeeds implicitly.
  - Settlement is decoupled from resource release, as AM-1 requires (`test_operation.cpp:291`).
  - The AM-6 per-target credit vector is already in place.
- **Runtime.**
  - One shared `driver_open_state` transaction serializes the legacy `open_driver()` and runtime initialization. There is never a second `cuFileDriverOpen` and never a close.
  - `cuFileDriverGetP2PFlags` is never referenced (grep clean; asserted at `test_runtime_params.cpp:185`).
  - Setters run strictly before the open. Readback is required before a setting is reported as `applied`.
  - The first request is frozen, and strict mode rejects any differing request.
  - `engine_possible(legacy)` is always false.
- **Capacity.**
  - `fit()` reproduces all three §4.7 rows and avoids overflow by dividing.
  - `water_fill` is correct max-min with largest-remainder rounding: grants ≤ demand, and the sum is `min(total, Σdemand)`.
- **Probe placement.**
  - `ensure_probe` only does `try_lock` + CAS + bounded `submit`. The process executor's `schedule` uses `try_to_lock`, so the call never blocks.
  - The probe runs only on the executor thread. Its blocking `cudaStreamSynchronize` is on a private lane.
  - If synchronization fails, the probe quarantines and keeps its arguments, slot and file permanently. It never frees possibly-live DMA targets.
- **Policy key.**
  - `{mnt-ns inode, mount id, fsid, uuid, generation}`.
  - `statx(STATX_MNT_ID)` falls back to `/proc/self/fdinfo/<fd>` `mnt_id:`. Mountinfo octal escapes are decoded.
  - Overrides use component-boundary prefix matching with fsid > longest prefix > fs_type precedence (AM-10).
- **Default behaviour.**
  - Nothing routes through the new code. `cufile_api::open_driver()` keeps its single-attempt semantics.
  - The control executor is created only when a `mount_policy` is constructed.
  - Legacy-mode `warm_up` / `lane_pool` have no side effects.
  - The only default-path change is P2-R10.

---

## Findings

### BLOCKING

#### P2-R1: shared registration silently corrupts data on compat libcufile; AM-9 and the harness under-report it, and `slab` granularity is unguarded and untested

**Files:**
- `test/io/cufile/test_device_arena.cpp:830` (reproducer)
- `include/cucascade/io/cufile/device_arena.hpp:38,81`
- `src/io/cufile/device_arena.cpp:556-562`
- `tools/gds/cufile_async_qd.cpp:414,542`
- `docs/io/gds-design/06-design-amendments.md:164-175`

**Problem.** `[arena_reproducer]` fails 3 of 3 runs. The assertion count shows where:
- the serialized QD 1 warm-up round passes with correct data;
- the first concurrent QD 4 round then fails the content comparison (`:815`), *after* `bytes_done == 1 MiB` passed (`:811`).

The same file offset is re-read into the same slot every round. A read that did nothing would therefore leave the round-0 bytes and pass. So libcufile **wrote wrong bytes into a slot and reported full success**. A plausible explanation is that compat-mode bounce state is shared per registered buffer and races across concurrent ops at different `buffer_offset`s. That would also explain the Phase 1 `-1` failures.

This is strictly worse than what AM-9 records ("`bytes_done = -1`"), for two reasons:
1. The recorded follow-up ("passed with no mismatches in arena mode") used the P0 harness, which validates only `bytes_done` (`cufile_async_qd.cpp:542`). It cannot detect this failure. My own harness run in arena mode at QD 4 also reports 0 mismatches.
2. AM-9 makes the Phase 7 "concurrent correctness" comparison depend on that harness.

Two further gaps:
- `registration_granularity::slab` puts up to 16 concurrent slots on one registration, so it has the same hazard. No real-libcufile test covers it.
- `device_arena_options.registration` accepts `arena` / `slab` without any qualification guard. A Phase 3 caller can pick it.

**Fix.**
1. **Amend AM-9.**
   - Record the silent-corruption observation: libcufile 1170, compat, ext4, deterministic at QD 4.
   - State that the earlier "passed" follow-up checked only `bytes_done` and is void.
   - State that no granularity other than `slot` may be used on any mount that is not `verified_direct` with content-validated G-gate evidence.
2. **Harness.** Add content verification (`--verify`, default on for smoke runs): a deterministic pattern file plus a sampled or full D2H compare after each completion, reported as `data_mismatches`. Redo the arena follow-up with it.
3. **Reproducer.** Parameterize it over `arena` and `slab` (`[.][arena_reproducer]`). Have it report the failing lane and offset, which helps the libcufile bug report.
4. **Guard.** Make non-`slot` granularity require an explicit option, for example `allow_shared_registration = false` by default, with `warm_up` throwing otherwise. Document on `registration_granularity` that shared registrations are unqualified and corrupt data in compat mode.

#### P2-R2: the simulator's submit path is slow and device-synchronizing, so it cannot model non-blocking submission

**File:** `test/io/cufile/transport_simulator.cpp:354,406,54`.

**Problem.** Every `submit()` does two things on the submitting thread:

1. **`cudaMallocHost(count)` per op** (`:406`). I measured 357–715 µs for 1 MiB. Every simulated 1 MiB submit is therefore above the 100 µs `slow_floor`. Three of the last eight are "slow", so the FR-9 breaker opens on healthy simulated traffic. The probe fixture already had to raise `submit_max_us` to 100 ms (`test_mount_policy.cpp:217`, "Simulator cold CUDA/pinned allocations are not production cuFile submit latency").
2. **`collect_completed()`** (`:354`), which destroys completed transfers. `~transfer` calls **`cudaFreeHost`** (`:54`). Measured: `cudaFreeHost` blocked **3.04 s** behind a host function blocked on a *different* non-blocking stream. It is a device-wide implicit synchronization.

Consequences for Phase 3, which the plan says is tested with this simulator:
- Every submit waits for all outstanding simulated I/O, including `completion_latency` sleeps and `stuck_completion` gates. Effective QD collapses to about 1. The "submit never blocks" property, and AM-10's compat observation, cannot be represented.
- Any test that holds one op at a completion gate while another op with nonzero bytes is submitted on the submitter thread will hang or measure a false stuck submit. The current suite avoids this only by using zero-byte ops (`test_transport_simulator.cpp:359`, "Zero-byte operations isolate accounting from pinned-allocation synchronization behavior").
- The watchdog, breaker, head-of-line and quarantine scenarios (FR-9, T-29) need exactly that shape.

**Fix.**
- Allocate all pinned memory at construction:
  - either keep the image itself in pinned memory, sized with a configurable cap, and do H2D/D2H directly against `image + offset`. This also gives realistic stream-time file reads and writes;
  - or keep a fixed pinned slab pool sized for `max_inflight × max_op`.
- Free only in the destructor. Never call `cudaMallocHost` / `cudaFreeHost` on the submit path.
- If a per-op buffer is unavoidable, recycle it through a free list that the `finish` callback fills; it must never be freed.
- Add tests:
  - with 32 ops of 1 MiB in flight and one completion gated, submit p99 stays below 50 µs;
  - a nonzero-byte op submitted while another op's completion is gated completes before the gate opens.

### SHOULD

#### P2-R3: the simulator and seam miss what Phase 3 must test (argument timing, registration extents, copy and join faults)

**Files:** `test/io/cufile/transport_simulator.cpp:359-360,429`; `include/cucascade/io/cufile/async_transport.hpp:39-77`.

**Problems.**
1. **Arguments are sampled at submit time**, not in stream order. `*size`, `*file_offset` and `*buffer_offset` are copied in `submit()`. libcufile's stream API reads them when the operation executes in the stream. A Phase 3 bug that mutates or reuses the `async_args` block (retry, slot reuse) before the I/O event would pass the simulator and corrupt data on hardware.
2. **No registration model.** `register_buffer` ignores the pointer and size, and `read_async` never checks that `base` is a registered base or that `offset + size` lies within its extent. T-16 ("cuFile receives slab base + offset — simulator assertion") and AM-9 base/offset correctness therefore cannot be asserted end to end. The arena also registers through `device_arena_hooks`, not through the transport, so the two paths are disjoint.
3. **No seam for D2D copy or stream wait.** AM-1 (`cudaStreamWaitEvent(user_stream, copy_ev)`) and AM-2 (`cudaStreamWaitEvent(lane, fence)` before the D2D) must settle a piece through recovery when the enqueue fails. T-29 lists "copy enqueue failure". Only `record_event` is injectable.

**Fix.**
- Read the three input arguments inside the `prepare` host function, which runs in stream order. Optionally add a "poison check" counter that fails if the arguments changed between submit and execution.
- Track registrations in a `std::map<base, size>`. In strict mode, fail submits whose `base` is not registered or whose range overflows the extent, and count unregistered-base submits as `bounce`. Add a helper that wires `device_arena_hooks` to the simulator's registration hooks.
- Add `memcpy_async(dst, src, bytes, kind, stream)` and `stream_wait_event(stream, event)` to `cufile_transport` (or a sibling `cuda_ops` seam), with scripted per-call failures in the simulator, and real forwarding in `cufile_async_transport`.

#### P2-R4: `observe_submit` copies and sorts 1024 samples per submission under a blocking mutex

**File:** `src/io/cufile/mount_policy.cpp:627-655` (`percentile99` at `:647`); also `:357-387`.

**Problem.** `observe_submit` is meant for the reaper (§6.8) and runs once per submission. Every non-slow sample:
- copies an 8 KiB `std::array` and sorts up to 1024 entries;
- does this inside `std::lock_guard(_mutex)`.

That is tens of µs per submission. At QD 64 with small ops (≥ 50 k submits/s per mount) it saturates the reaper. `write_eligible()`, `effective()` and `base_profile()`, which run on the facade route, block on the same mutex.

**Fix.**
- Keep the slow-of-eight window as an atomic 8-bit mask.
- Recompute p99 every N samples (for example 64), or keep a fixed log-bucket histogram.
- Make `observe_submit` single-writer (reaper-only), with the healthy baseline published through an atomic.
- Keep the mutex for control-plane state only, and never hold it on the reaper or facade path.

#### P2-R5: a file-level condition marks the whole mount `unsupported` permanently

**File:** `src/io/cufile/mount_policy.cpp:476-479`.

**Problem.** If the first probed file has no cuFile handle (for example a transient `cuFileHandleRegister` failure) or no O_DIRECT fd, `finish_probe(unsupported)` is called. Nothing leaves `unsupported`. One bad file therefore disables the engine for that mount, device and generation. §4.5.2 distinguishes file-level from mount-level `unsupported`.

**Fix.** For file-level conditions, publish `unknown` with `_next_probe` set; optionally report `unsupported_file` on the object. Reserve mount-level `unsupported` for the storage-class `compatibility_only` path at `:490-494`. Add a test: an unregistered first file is followed by a successful probe of a registered file.

#### P2-R6: state-machine liveness — operator overrides are lost after one demotion, and two paths can strand a mount

**File:** `src/io/cufile/mount_policy.cpp:342-353,437,657-705`.

**Problems.**
1. **Override lost.** An `operator_qualified` override can be demoted (`observe_submit` → `circuit_open`). After cooldown, `poll_timers` sets `unknown`.
   - No probe context was ever retained, because `ensure_probe` never ran for an overridden mount.
   - The next facade-triggered probe needs evidence (`require_direct_evidence=true`), which an override mount by definition lacks.
   - So the mount stays `unknown`, or becomes `known_compat`, for the life of the process.
   - AM-4 intends the override to be a standing operator decision.
2. **Circuit opened during a probe.** If the circuit opens while the state is `probing` (`open_circuit_locked` does not exclude `probing` or `unknown`), `finish_probe` returns early at `:437`. Nobody calls `begin_cooldown`, so the mount stays in `circuit_open` with no traffic to drain.
3. **Lost timer.** `poll_timers` uses `try_to_lock` and returns on contention. The scheduled timer fires only once, so with no engine loop (the situation today) a lost race strands the mount in `cooldown`.

**Fix.**
1. Remember the override state. On cooldown expiry, return to `operator_qualified`, optionally after a submit-latency-only probe.
2. Make `open_circuit_locked` a no-op unless the state is active. Alternatively, have `finish_probe` call `begin_cooldown` when it finds `circuit_open`; the probe has already drained.
3. On `try_lock` failure in `poll_timers`, reschedule a short retry timer.

Add a test for each.

#### P2-R7: the nvidia-fs evidence source cannot parse real nvidia-fs output, so `verified_direct` is unreachable in production

**File:** `src/io/cufile/mount_policy.cpp:275-321,609-611`; test `test_mount_policy.cpp:960`.

**Problem.** The parser looks for `read_bytes` / `write_bytes` / `bytes` tokens. Real `/proc/driver/nvidia-fs/stats` lines look like:
- `Reads : n=… ok=… err=… readMiB=… io_state_err=…`
- `Writes : … writeMiB=…`

None of those match, so `available` is always false. The unit fixture uses a synthetic `read_bytes: 4096`.

Even if parsed:
- the counters are MiB-granular, so `after − before == completed` (exact bytes) cannot hold;
- `isolated_interval` defaults to false, so the result is never attributable.

`cufile_stats_evidence` is a stub. The net effect is that no mount can reach `verified_direct` outside tests.

**Fix.** Do one of:
- implement the real format, using a captured fixture from a GDS host (see `tools/gds/collect_topology.sh`) and a MiB tolerance;
- remove the parser claim, have `nvidia_fs_evidence` return `available=false` with a documented TODO, and record in AM-4/AM-7 that automatic qualification is non-functional until the isolated-interval phase.

Do not ship a parser tested only against an invented format.

#### P2-R8: a probe cannot run without a pre-existing arena quota, and the probe context cannot hold the production arena

**File:** `src/io/cufile/mount_policy.cpp:503-510`; `include/cucascade/io/cufile/mount_policy.hpp:143-146`; `src/io/cufile/device_arena.cpp:518-523`.

**Problems.**
- `try_lease` returns `nullopt` for a mount without a quota. Quotas come from `rebalance()` over *active* (engine-routed) mounts, and an `unknown` mount is not active. In an integrated engine, every first probe would end with `no_resources`. The tests pass only because the fixture calls `set_quota(1, …)` up front (`test_mount_policy.cpp:233`).
- `mount_probe_context::arena` is a `shared_ptr<device_arena>`, but the production arena is a registry-owned reference (`device_arena::for_device`).

**Fix.**
- Add a probe lease path, for example `try_lease_probe(bytes, need_registered)`. It borrows an idle unowned slot without a quota, or uses a dedicated probe slab reserved at warm-up, and is counted in gauges.
- Make the context hold `device_arena&` or a non-owning handle.
- Add a test with no quota.

#### P2-R9: the default slot granularity with `initial_class = mib1` pins the arena to pieces of at most 1 MiB

**File:** `include/cucascade/io/cufile/device_arena.hpp:81-84`; `src/io/cufile/device_arena.cpp:149-152,207-213`.

**Problem.**
- Slot registrations have immutable geometry, and reclassification needs a prewarmed spare of the target class.
- With the defaults (`slot`, `mib1`, empty `slab_classes`), every slab is 1 MiB. The local-NVMe profile's `S_pref = 4 MiB` can never be served: `try_lease(4 MiB)` returns `nullopt`, and quota changes stay `pending` forever.
- `rebalance` / `fit` do not know which classes exist.

**Fix.**
- Derive the warm-up classes from the expected profile set: the config, or mount profiles known at warm-up.
- Expose `available_classes()` and cap `fit()`'s class loop by it.
- Document that in slot mode, changing `S_eff` beyond the prewarmed classes requires maintenance.
- Add a test: a 4 MiB profile on the default arena gives a 1 MiB effective size, never a `nullopt` lease storm.

#### P2-R10: every `cufile_io_object` now parses `/proc/self/mountinfo` at open, even in legacy mode

**File:** `src/io/cufile/cufile_reactor.cpp:764`; `src/io/cufile/mount_policy.cpp:75-131`.

**Problem.** Each open now performs:
- a `stat` of `/proc/self/ns/mnt`;
- `fstatfs` and `statx`, plus possibly reading `/proc/self/fdinfo/<fd>`;
- a full read of `/proc/self/mountinfo`.

This includes writes, `probe_path`, and the cudf disk tier, which opens one file per batch. On container hosts mountinfo has thousands of lines, so this adds file-system work to the default legacy path for no benefit. NFR-2 and FR-35 require legacy to reproduce current behaviour.

**Fix.**
- Compute lazily, under `std::call_once`, on the first `mount_identity_info()` / `policy_key()` call. Those calls happen only from engine routing or the control executor.
- Alternatively, cache the mountinfo lookup per `(st_dev, mnt_id)` process-wide.
- Keep the cheap `fstatfs` / `statx` part eager only if the engine is enabled.

#### P2-R11: init order — the arena and lanes open the driver through the legacy path, bypassing `runtime`

**File:** `src/io/cufile/device_arena.cpp:282`; `src/io/cufile/lane_pool.cpp:42`; `src/io/cufile/platform.cpp:66`; `src/io/cufile/cufile_reactor.cpp:223`.

**Problem.** The shared `driver_open_state` transaction correctly prevents a double open. But:
- §6.3 requires `probe_platform` and `register_with_cufile` to call `runtime::ensure_initialized`. They still call `cufile_api::open_driver()`.
- The new default arena registrar and `lane_pool` do the same.

Whichever legacy path runs first, permanently and silently, causes the frozen runtime parameters to be "unapplied: driver opening already attempted". Today that is harmless, because no production adapters exist yet (Phase 6). But it means the documented init order is not enforced anywhere.

**Fix.**
- Route the default hooks, `lane_pool`, `probe_platform` and `register_with_cufile` through `runtime::instance().ensure_initialized(frozen_or_default)`.
- Alternatively, record in AM-10 that engine bootstrap must call the runtime before the first reactor open, and add an assertion or diagnostic when the runtime initializes after a legacy open.
- Add a test: arena warm-up through the defaults leaves `runtime::instance()` initialized first.

#### P2-R12: production warm-up registers until failure, and can exhaust BAR1 and the cuFile pin budget

**File:** `src/io/cufile/device_arena.cpp:266-295,439-450,463-476`.

**Problem.**
- `defaults()` provides neither `free_bar1` nor `pinning_headroom`. The registration target is therefore the whole allocation, which by AM-5 auto can be up to 1 GiB.
- On a T4 (about 256 MiB BAR1), registration consumes BAR1 until cuFile fails. That starves caller-registered buffers (`registration_table`) and other processes. FR-14's 50 % fraction exists precisely to prevent this.
- Arena mode with known BAR1 registers `limit` in one call. One failure leaves zero registered, with no step-down.

**Fix.**
- Supply `pinning_headroom` from `CUfileDrvProps_t::max_device_pinned_mem_size` (KiB) minus the bytes already registered. The runtime already queries the properties.
- Supply `free_bar1` through a `dlopen`ed NVML `nvmlDeviceGetBAR1MemoryInfo`.
- When BAR1 is unknown, stop after `registration_first_step` until the G-gates qualify more.
- In arena mode, fall back to slab-sized groups after a failed bulk registration.

#### P2-R13: `fit()` drops to 1 MiB whenever lanes are the binding constraint, even when the bytes allow larger ops

**File:** `include/cucascade/io/cufile/capacity.hpp:56-82` (spec FR-16 / §6.9).

**Problem.** The implementation follows the spec literally. When `lanes(Q) > C_m`, no class can fit, so the result is `{Q_eff, 1 MiB}`, even with ample `B_m`.
- With three QD-64 mounts under the 160-lane cap, each gets 53 lanes and every one falls to `QD 42 × 1 MiB`, although `42 × 4 MiB` fits in their slab grants.
- The test at `test_capacity.cpp:138` shows the same effect: 33 lanes and 128 MiB give `QD 26 × 1 MiB`.

This reduces op size, and so throughput, on exactly the multi-mount GPFS shape that the profiles target.

**Fix.**
- Amend FR-16 to compute `Q_eff = max{q ≤ Q : lanes(q) ≤ C_m}` first, then choose the largest class `S ≤ S_pref` with `lanes(Q_eff) × S ≤ B_m`, then reduce q further only if bytes bind at 1 MiB.
- Add the 1/2/5-mount cases under the 160-lane cap that T-17 calls for.

#### P2-R14: the process control executor is destroyed at exit, and dropped probe tasks strand the probe state

**File:** `src/io/cufile/mount_policy.cpp:238-246,264-269,407-410`.

**Problems.**
- `process_instance()` holds a function-static `shared_ptr`. Its destructor `join()`s the worker during static destruction. A probe blocked in libcufile or `cudaStreamSynchronize` hangs process exit, and a probe that runs after the CUDA runtime has been torn down crashes.
- The arena, runtime and registry singletons are all deliberately never destroyed.
- If a task is ever dropped un-run (the executor stops while a timer is pending, or an injected executor discards it), the busy flag and the `probing` state are never reset, because the reset lives in the task body.

**Fix.**
- Use `static auto* const executor = new …` (never destroyed). Detach semantics belong to process exit.
- Capture an RAII guard in the probe task whose destructor releases `_device_probe_busy` and reverts `probing → unknown` if the task did not run.

### NIT

- **P2-R15 (`device_arena.cpp:518,728-737`; `lane_pool.cpp`; `operation.hpp`).**
  - `try_to_lock` gives spurious empties under contention. Document that the scheduler must treat this as "retry next tick", not "capacity exhausted".
  - `staging_lease::reset()` takes a blocking lock and runs `progress()`, which is O(quotas × slabs) and up to O(slabs²) while draining, on every release on the reaper. Add a dirty flag so `progress()` runs only when a quota or drain is pending.
- **P2-R16 (`runtime.cpp:179-186`, `cufile_api.cpp:198-221`).**
  - `CU_FILE_DRIVER_ALREADY_OPEN` sets `opened=false`, which disables both legacy GDS and the engine. This is pre-existing legacy semantics. Consider treating it as "opened, parameters unverified".
  - `open_driver()` now takes a process mutex on every file open. Add an atomic fast path once `attempted` is set.
- **P2-R17 (`mount_policy.cpp:530-547,567`).**
  - The probe creates a private `lane_pool` by forcing `mode = async_simulated_for_testing`.
  - It registers its stream with flags `0`, while production lanes use `CU_FILE_STREAM_PAGE_ALIGNED_INPUTS` (FR-17). The probe therefore measures a different submit configuration from the one it qualifies.
  - `bytes_done` starts at 0 instead of `pending_sentinel`.
- **P2-R18 (`mount_policy.cpp:84-88`).** `fsid` is hex of the raw `f_fsid` bytes. On little-endian hosts this is not the spelling `stat -f -c %i` prints, so operators copying that output get silent override misses. Document the format with an example, or accept both spellings.
- **P2-R19 (`operation.hpp:393-416`; `lane_pool.cpp` state teardown).**
  - `try_release` destroys the last `io_object` reference, which deregisters the cuFile handle and closes fds.
  - The last `lane_lease` reset can destroy the lane pool state, which does `cuFileStreamDeregister` and `cudaStreamDestroy`.
  - Both run on the releasing thread, which will be the reaper (FR-5). Phase 3 should hand final references to the blocking pool.
- **P2-R20 (conventions).**
  - `mount_policy.{hpp,cpp}` use an abbreviated SPDX-only license header, unlike the other new files.
  - They have dense formatting with no blank lines between functions and braceless `else`.
  - `mount_policy_registry` declares its public constructor after the private section.
  - `profile_for_filesystem` maps plain TCP `nfs` / `nfs4` to the `nfs_rdma` class.
  - `"compat"` is not a real fs type.
  - `bar1_fraction`, `bar1_headroom` and `registration_first_step` live in `device_arena_options`, not in `async_engine_config` as AM-10 / §6.2 imply. Record the deviation.
- **P2-R21 (tests).**
  - `test_capacity.cpp:138` is titled "never revokes leases" but checks only arithmetic.
  - T-17's 1/2/5-mount cases under the 160-lane cap are missing.
  - A failed lane stream registration throws for the whole pool (`lane_pool.cpp:97`). Consider degrading to an unregistered stream and reporting it.
- **P2-R22 (`cufile_reactor.hpp:23`).** The installed reactor header now pulls in `mount_policy.hpp`, and through it `capacity.hpp`, `device_arena.hpp`, `<functional>` and `<chrono>`. A forward declaration plus an out-of-line accessor would keep the public header light.

---

## Seam fitness for Phase 3

The lane, lease and op-pool ownership model fits AM-1 well:
- credits settle early;
- the lane, slot, file, input and args stay until `observe_final_event`;
- `cufile_transport` carries pointer arguments unchanged, verified against the real table at `test_transport_simulator.cpp:729`.

Before Phase 3 builds on the seam and simulator, three things are needed:
- **P2-R2.** The simulator must actually be non-blocking.
- **P2-R3.**
  - Stream-time argument reads.
  - Registration-extent checks.
  - Injectable D2D and stream-wait calls for AM-1 and AM-2 failure paths.
- **P2-R8.** A probe-lease path.

AM-2 needs nothing from Phase 2 beyond P2-R3 item 3. The read op enters no input-wait phase, and `operation::set_phase` enforces this.

## Verdict: **CHANGES-REQUIRED**

The resource layer is careful and well tested: leases, maintenance, base/offset, partial registration, BAR1 math, quota draining, the op pool and credits.

Two findings block closing Phase 2:
- **P2-R1.** Shared registration produces silent corruption with a successful `bytes_done`. The design record and the G-gate harness cannot see it, and `slab` mode is unguarded.
- **P2-R2.** The transport simulator, a Phase 2 deliverable that Phase 3 depends on, makes every submit allocate pinned memory and device-synchronize. It cannot model non-blocking submission, QD, or the breaker.

Fix P2-R1 and P2-R2 before commit.

The following SHOULDs need to land before Phase 3's engine relies on the components they touch:
- R3 and R8 (Phase 3 seams);
- R4 (reaper hot path);
- R5 and R6 (policy liveness);
- R10 (default-path cost).

R7, R9, R11–R14 can be tracked in the amendments and fixed in Phase 3 or Phase 6.

## Resolution

- **P2-R1 — fixed:** shared arena/slab warm-up requires explicit unsafe qualification opt-in; hidden reproducer covers both with poisoned slots and lane/offset diagnostics; harness defaults to full content verification and can create nonuniform deterministic files without overwriting. AM-9 voids the old bytes-only pass and requires content-validated direct-GDS qualification.
- **P2-R2 — fixed:** simulator preallocates its bounded mapped pinned image, descriptors and transfer records; submit/retirement never allocate or free pinned memory. Device-side completion gates preserve independent-stream progress. Nonzero independent completion and 32 × 1 MiB submit-p99 < 50 µs regressions pass.
- **P2-R3 — fixed:** default input sampling/validation occurs in stream order; strict registered-base/extent checks and arena-hook wiring cover all three granularities; real/simulated D2D and stream-wait seams inject failures. Submit-time snapshots require explicit opt-in with a stronger initialized-arguments contract. First-enqueue errors recycle safely; partial work requires explicit stream recovery.
- **P2-R4 — fixed:** single-writer rolling histogram replaces per-submit sorting; p99 is refreshed every 64 healthy samples and published atomically. Facade/reaper paths avoid the control mutex, including deferred cooldown initiation.
- **P2-R5 — fixed:** file registration/O_DIRECT failures return the mount to retryable unknown; a later qualifying file succeeds.
- **P2-R6 — fixed:** standing operator overrides survive cooldown; inactive/probing mounts cannot open a traffic circuit; contended timers retry, and deferred drain proofs survive a full control queue.
- **P2-R7 — fixed:** removed the invented-format parser claim; built-in production evidence explicitly returns unavailable. AM-11 tracks captured real-format parsing, MiB tolerance and isolated attribution together in Phase 7/network qualification; no false automatic verification is possible.
- **P2-R8 — fixed:** quota-free probe leasing reserves an idle slab without revoking live leases, is counted in gauges, and the probe context holds a non-owning production-arena pointer with its lifetime contract documented.
- **P2-R9 — fixed:** arena exposes available classes/mask, capacity fitting and rebalance accept the mask, and the default 4 MiB preference fits to a leaseable 1 MiB class. AM-11 records immutable slot geometry and Phase 3's obligation to pass the prewarmed mask.
- **P2-R10 — fixed:** mount identity is lazily cached with call_once; legacy opens do not parse mountinfo. Lazy and concurrent-access regressions pass.
- **P2-R11 — fixed:** platform probing, file registration, default arena registration and lane creation join runtime initialization. No-argument joins retain already-frozen parameters; defaults cannot silently replace an earlier explicit request.
- **P2-R12 — fixed:** default warm-up dynamically samples NVML BAR1 and runtime pin headroom, unknown BAR1 stops at the conservative first-step cap, and failed bulk registration steps down to slab groups. The pin ledger covers default arena registrations; live BAR1 includes other consumers.
- **P2-R13 — fixed:** AM-11 amends FR-16 to fit lane-limited QD before choosing op size; 1/2/3/5-mount tests under the 160-lane cap retain larger classes when bytes permit.
- **P2-R14 — fixed:** process executor/registry are immortal; captured probe-task RAII releases the device budget and reverts probing even when an executor discards the closure.
- **P2-R15 — fixed:** try-lock misses explicitly mean retry-next-tick; arena quota progress runs only while quota/drain work is dirty.
- **P2-R16 — fixed:** externally already-open drivers remain usable with parameters unverified/unapplied; a published atomic fast path bypasses the process mutex only after the complete open/properties transaction ends.
- **P2-R17 — fixed:** probes use page-aligned stream registration and pending_sentinel without forcing production probes through simulated mode.
- **P2-R18 — fixed:** documented the raw-byte hexadecimal fsid spelling and a little-endian example for operator overrides.
- **P2-R19 — deferred to Phase 3:** the scheduler/blocking cleanup pool does not exist yet; AM-11 and operation retirement documentation require handing final owners off the reaper before enabling scheduling. Changing standalone pool teardown now would invent that missing executor.
- **P2-R20 — fixed:** full license headers, function spacing/braced else and registry declaration order normalized; plain NFS and invented compat fs types no longer imply qualified storage classes; AM-11 records arena-option placement.
- **P2-R21 — fixed:** arithmetic-only test renamed and missing multi-mount lane-cap cases added. Optional silent stream-registration degradation is declined: construction failure safely unwinds an uncertain partially registered pool instead of claiming usable cuFile state.
- **P2-R22 — fixed:** installed reactor header uses forward declarations and out-of-line lazy accessors instead of importing mount policy and resource headers.

### Resolution verification

- Required build targets succeed: `cucascade_io_tests` and `cucascade_cufile_async_qd` (`-j 20`).
- Focused Phase 2 regression run: **137 cases, 21,811 assertions, all pass**.
- Full `./build/release/test/cucascade_io_tests`: **784 cases: 783 passed, 1 skipped; 3,032,365 assertions passed**. The existing skip is the kTLS-negative test on a host with kTLS loaded.
- Content-validating harness, libcufile 1170/compat/ext4, 64 MiB patterned file, 1 MiB ops: per-slot QD 1/4 passes with no byte-count or data mismatches. Shared-arena QD 1 passes; QD 4 intentionally fails with **350 data mismatches / 361 completions and zero byte-count mismatches**.
- Explicit hidden shared-registration reproducer intentionally fails for **both arena and slab**, reporting round 1/lane 0/offset and first differing byte. These unsafe opt-in diagnostics are not included in the passing default suite and do not qualify either layout.
- `/mnt/disk_2` is absent on this host, so diagnostic files were under `/tmp`; these are correctness checks, not NVMe/GDS throughput qualification.
- `pixi run python -m pre_commit run --files <all changed files>` passes every applicable hook. The required build and full suite were repeated successfully after formatting.
- No commits and no `src/cudf/` edits. The required GSD entry point was attempted but is not installed in this harness.
