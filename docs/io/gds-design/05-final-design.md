<!-- counsel: winning plan from "gds-io-expert" (openai-codex/gpt-6-astra) selected via clear -->

# cuFile Async Reactor for cuCascade: Implementation Specification

Conventions: **[belief]** marks a claim that is not proven and must be verified as stated in §9. "Appendix A/B/C" refers to the design brief's appendices. Sizes use binary units.

---

## 1. Overview

This specification replaces the host-synchronous cuFile reactor, which is capped at four in-flight calls per process (Appendix C §2.8), with a shared per-device **async engine**. The engine drives `cuFileReadAsync`/`cuFileWriteAsync` on a pool of independently recycled non-blocking CUDA stream **lanes**. I/O is staged through a persistent, reservation-accounted, registered device **arena**, and each result is validated on the host before an exact D2D copy into the caller's unregistered pool memory. The goal is at least 80 % of real-GDS `gdsio` throughput for sustained scattered reads, DISK-tier restores, and spills on local NVMe, GPFS, WEKA, and VAST. A single large slice must be able to fill a sliding queue depth of 32–64. The default host-complete future contract, compat-mode behaviour, and the POSIX fallbacks stay unchanged. Each mount is promoted to the async path only after an automatic probe shows attributable direct-path evidence, and is demoted automatically if async submission starts to block. The engine is introduced behind the existing reactor seams in reversible phases, with an opt-in two-phase event API added last.

---

## 2. Goals and non-goals

### Goals

- Reach at least 80 % of the matched real-GDS `gdsio` baseline for **each** primary workload on each primary filesystem (local NVMe, GPFS, WEKA, VAST).
- Decouple storage queue depth from host thread count. QD slides per operation, with no generation gaps.
- Stripe a single large slice across the full QD window.
- Keep the `device_readv_async_io` host-complete contract, the write-source-lifetime rule, `enqueue noexcept`, and null/PTDS stream semantics.
- Select transport per mount and per device with automatic verification and automatic demotion. Never rely on process-wide `gds_available()` for routing a specific file.
- Use bounded, reservation-accounted, process-lifetime registered staging. A registration failure degrades the path and never fails a read.
- Preserve compat-mode behaviour: synchronous `cuFileRead` for reads, D2H plus `pwrite` for writes, four blocking workers.
- Use async direct writes for aligned bodies on verified mounts. Keep the existing head/body/tail split.
- Deliver incrementally. Each phase is shippable, measurable, and reversible through configuration.
- Provide an opt-in two-phase event API for pipelined consumers once the default path is qualified.

### Non-goals

- Batch API (`cuFileBatchIO*`) transport. The symbols are resolved for capability reporting only. Batch is revisited only if measurements show submission saturation.
- Registering arbitrary RMM or pool sub-ranges. The existing `register_min_bytes` knob remains "A/B only" on the legacy path.
- Changing the default completion contract or migrating any DISK-tier caller.
- An online adaptive throughput optimizer. Profiles are static and benchmark-qualified. Online policy changes only transport health and capacity.
- Prefetch-cache or fragmented-destination support in the cuFile reactor. `supports_vector_host_read=false` stays.
- Changing the DISK tier's automatic io_uring preference on non-GDS hosts.
- Durability guarantees on write futures.
- Application-level read-modify-write for write edges.
- `cudaLaunchHostFunc`-based completion in production code.
- Cancelling async operations that are already submitted. They are drained.
- Rewriting system `cufile.json` files.

---

## 3. Requirements

### 3.1 Functional requirements

**Contracts and submission**

- **FR-1 (read completion contract).** The future returned by `ioctx::device_readv_async_io` resolves successfully only after every requested byte is in the destination and no reactor-enqueued operation can still touch the destination or staging. The caller needs no stream synchronization afterwards. This holds on every transport.
- **FR-2 (write contract).**
  - A write future resolves only after every physical operation of the request is quiescent.
  - The source is never read after resolution.
  - `note_written(end)` is called only for logical ranges that completed successfully.
  - A resolved write future does not imply durability.
- **FR-3 (non-blocking enqueue).**
  - `enqueue` is `noexcept`. It never waits for slots, lanes, probes, thread creation, allocation of device memory, or registration.
  - When the bounded metadata budget is exhausted (`max_pending_slices_per_device`), the request's future completes exceptionally with `no_buffer_space`.
  - Exceptions inside `enqueue` are caught and settle each unpublished credit exactly once.
- **FR-4 (submit-time input fence).**
  - When `runtime::engine_possible()` is true and the request has device slices, the submitting thread resolves null/PTDS streams with the existing `resolve_default_streams`.
  - It then records one timing-disabled CUDA event per distinct `(device, resolved stream)`, retains the PTDS stand-in owner, and publishes the request only after every record succeeds.
  - Every engine operation orders itself after its fence with `cudaStreamWaitEvent` on its lane.
  - Work the caller queues after submission is not captured.
- **FR-5 (thread confinement).**
  - `cuFileReadAsync`/`cuFileWriteAsync` are called only on submitter threads or the control executor (for probes). They are never called on an application thread or on the scheduler/reaper.
  - The scheduler/reaper never calls a blocking API: no `cudaStreamSynchronize`, `cudaEventSynchronize`, synchronous cuFile, POSIX I/O, `cudaMalloc`, `cuFileBufRegister`, or thread creation.

**Transport selection**

- **FR-6 (policy identity and state).**
  - Transport policy is keyed by `{mount namespace, mount ID, fsid, device UUID, runtime generation}`.
  - States are `unknown`, `probing`, `verified_direct`, `degraded_async`, `known_compat`, `unsupported`, `circuit_open`, `cooldown`, and `operator_qualified` (explicit override, never reported as verified).
  - Read and write eligibility are tracked separately.
- **FR-7 (safe default).** While a mount is `unknown`, `probing`, `circuit_open`, or `cooldown`, application reads use the existing synchronous cuFile path on the existing reactor workers (default four). Writes use the existing write decision logic.
- **FR-8 (automatic probe).**
  - On first eligible use of an `unknown` mount, a bounded control executor reads aligned existing ranges of the actual opened file into an arena slab with `cuFileReadAsync`. The ranges total about 16 MiB, or less for short files, which then remain unverified.
  - The probe never writes to an application file. Its bytes are excluded from caller byte counts.
  - `verified_direct` requires **both** an attributable direct-path counter delta and a non-blocking submit: median call-return time at most `probe_submit_max` (default 100 µs **[belief]**).
  - A file handle registration, a filesystem support bit, or high throughput is not evidence.
- **FR-9 (runtime demotion).**
  - A submission is *slow* if its call time exceeds `max(100 µs, 4 × healthy_submit_p99)`.
  - Three slow submissions among the last eight, or one submission still inside libcufile after 2 ms, open the circuit for that `(mount, device)`.
  - Async admission stops at once. Unsubmitted pieces move to the blocking pool as sync cuFile reads. Submitted operations drain and are never replayed while their original call or DMA may be active.
  - A re-probe runs after a 30 s cooldown. The cooldown doubles on each consecutive failure, capped at 16 minutes.
  - All thresholds are **[belief]** and configurable.
- **FR-10 (decision ladder).** Routing follows the table in §4.5.4. In particular, `known_compat` mounts read with synchronous `cuFileRead` and write device sources with D2H plus `pwrite`, exactly as today (Appendix C §2.2, §3.5a).
- **FR-11 (optional symbols).**
  - The loader resolves five independent optional groups: async-read, async-write, stream-register, batch, and parameter setters/getters.
  - A missing group disables only that capability.
  - The existing eight required symbols remain required.
- **FR-12 (runtime initialization).**
  - Order: load library → resolve symbols → freeze `process_cufile_parameters` → apply supported pre-open setters → `cuFileDriverOpen` → query properties → allow file registration, probes, and transfers.
  - `cuFileDriverGetP2PFlags` is never called.
  - Unsupported requested parameters produce a startup diagnostic. In strict mode they fail context creation.
  - Parameters are a typed allowlist. No numeric enum value is guessed.

**Resources**

- **FR-13 (arena allocation and lifetime).**
  - Each device has one process-lifetime arena. The desired size is 256 MiB (configurable), made of 16 MiB slabs.
  - Each slab is allocated with `cudaMalloc` only after a non-blocking grant from the device reservation manager.
  - Allocation and registration run on the control executor, never on the transfer hot path.
  - Normal context teardown performs no arena `cudaFree`, `cudaFreeHost`, or `cuFileBufDeregister`. The reservation stays charged.
  - Re-creating a context reuses the arena.
- **FR-14 (registration target and degradation).**
  - `registered_target = min(allocation_target, 50 % × max(free_BAR1 − 64 MiB, 0), cuFile pinning headroom)`.
  - Without BAR1 telemetry, 64 MiB is registered first. Registration then continues one slab at a time, off the hot path, until the first failure or the target.
  - Slabs that registered successfully are kept after a later failure.
  - Slabs that failed registration remain in the same arena as **unregistered device slabs**. They are usable for async staging only on mounts with a separate successful *unregistered-async* qualification.
  - Registration failure alone never fails a read.
- **FR-15 (size-class slabs).**
  - Each slab holds slots of exactly one class: 1, 2, 4, or 8 MiB.
  - cuFile always receives the slab base plus an offset.
  - Reclassification drains at most one slab per mount at a time. Leases in flight are never revoked.
- **FR-16 (capacity fit).**
  - With `lanes(q) = q + ceil(q/4)`, the effective profile of a mount is the largest class `S ≤ S_pref` such that `lanes(Q) ≤ C_m` and `lanes(Q) × S ≤ B_m`.
  - If no class fits, `S = 1 MiB` and `Q_eff` is the largest `q ≤ Q` that fits.
  - If `Q_eff = 0`, the mount uses a fallback transport.
  - Slabs and lanes are distributed among active mounts by weighted water-filling under a 160-lane device cap.
- **FR-17 (lanes).**
  - Lanes are `cudaStreamNonBlocking` streams. Each holds at most one physical operation.
  - When the stream-register group is present, a lane is registered with `CU_FILE_STREAM_PAGE_ALIGNED_INPUTS` only.
  - Only an operation's I/O phase consumes a storage-QD token.
- **FR-18 (direct registered I/O).** The engine reads or writes directly to or from caller memory only when the slice carries an explicit `registered_buffer_lease`. The lease must prove device, registered extent, stable ownership, and lifetime, and the piece must be granule-aligned in file offset, size, and buffer offset. Pointer alignment or size alone never qualifies.

**Data path**

- **FR-19 (staged read).**
  - A staged read reads the enclosing granule-aligned window (clamped to EOF rounded up to the granule) into a staging slot.
  - It then copies exactly the logical bytes to the destination with D2D on the same lane.
  - The copy is enqueued only after the reaper has validated `bytes_done` (validate-before-copy).
  - Read-around is granule-sized and is never expanded to the filesystem block size.
- **FR-20 (short reads).**
  - A short physical read succeeds only if every logical byte is covered.
  - Otherwise the operation is reissued from the aligned boundary at or below the first missing byte, with the same lane, slot, and credit.
  - Progress is measured by extension of the valid interval.
  - Retries are bounded by `max_short_reads` (64). Zero progress or file shrink over the required range is an error.
  - If O_DIRECT retry is refused, the remainder is fetched with buffered `pread` into pinned scratch, then H2D into the slot.
- **FR-21 (striping and geometry).**
  - Slices are expanded lazily into physical pieces of at most `S_eff`.
  - Pieces are cut at absolute file offsets that are multiples of `S_eff` and at discovered filesystem geometry boundaries (GPFS block, Lustre stripe/component, BeeGFS chunk).
  - A single 1 GiB slice must be able to occupy the mount's entire effective QD.
- **FR-22 (write pipeline).**
  - Device-source writes are split into head, body, and tail at the write granule.
  - On a write-eligible verified mount the body is staged D2D into arena slots and written with `cuFileWriteAsync`. Staging and I/O are separate phases, and only the I/O phase holds a QD token.
  - Heads, tails, and all compat or unverified bodies use D2H into pinned scratch plus `pwrite`.
  - The existing host-source write policy is unchanged.
- **FR-23 (write ordering and short writes).**
  - Operations that touch the same granule of the same file identity with different I/O modes (buffered or direct) are serialized. The file identity `(st_dev, st_ino)` is shared across library handles.
  - Only a known positive short-write suffix is retried, through buffered staging if the suffix loses alignment.
  - A write with unknown progress or a timeout is never replayed.
- **FR-24 (flush).** Each write is assigned a monotonically increasing per-file submission sequence. A flush captures the last sequence, waits (without blocking a thread) until every write at or below it has retired, then runs `fdatasync` on the blocking pool.
- **FR-25 (reaper protocol).**
  - Completion is detected by `cudaEventQuery` on timing-disabled events.
  - A submitter publishes `{op, phase, event generation}` only after `cudaEventRecord` succeeds. The reaper queries only published generations.
  - Only the head event of each lane is polled. There is no global completion FIFO.
- **FR-26 (scheduling, priority, liveness).**
  - Scheduling is per physical piece: strict high-before-low, one low quantum after every four high starts (`low_service_interval`), and byte-deficit round-robin among mounts and requests.
  - One-eighth of each mount's lanes is reserved for the high tier and is borrowable when no high work is pending.
  - Only pending pieces are preempted.
  - While a request's fence is unready, at most one of its operations may hold a lane, and at most one-eighth of device lanes may be input-waiting.
  - A request blocked on its input never prevents other ready requests from completing.
- **FR-27 (exactly-once and lifetime).**
  - Each physical operation settles exactly one coordinator credit.
  - Its `io_object`, fence, argument block, lane, and staging lease are retained until the reaper has observed its final event, or until recovery has proven quiescence.

**Operations**

- **FR-28 (threads).**
  - One scheduler/reaper per active device.
  - One submitter per active async-enabled `(device, mount)`, capped at 8 per device and 16 per process. It is created on the control executor and retired after 60 s idle.
  - One control thread and two recovery threads.
  - A lazily started blocking pool of four threads for engine-originated blocking work.
  - Legacy reactor workers are unchanged (`cufile_n_reactors`, default 4).
  - A blocked submitter is never detached or replaced.
- **FR-29 (affinity).** When topology is discoverable, the reaper is pinned to a permitted physical core on the GPU's NUMA node and each submitter to a different physical core on the selected NIC's node. When it is not, affinity is left unset and the reason is reported. Application-thread affinity is never changed.
- **FR-30 (failure handling).**
  - Failures are classified before any retry (§7).
  - If a copy or event record fails after a partial enqueue, the lane is removed from service and its resources are retained.
  - A recovery thread then synchronizes only that owned lane. The operation is retired after proven quiescence, or quarantined without completing the future falsely and without returning memory.
- **FR-31 (cancellation and shutdown).**
  - Cancelling pending work settles its credit with `cancelled`.
  - Submitted operations drain.
  - Reactor-facade shutdown closes that facade's admission, cancels its pending work, and waits for its handed-off operations to retire, without destroying the shared engine or arena.
  - A drain timeout reports "incomplete drain". It never reports a fictitious completion.
- **FR-32 (two-phase event API, opt-in).**
  - `device_readv_async_io_ev` returns `{enqueued, completed}`.
  - `enqueued` yields a `device_join_token` only after all pieces are expanded, all I/O is validated, every final accessor is enqueued, and the request's join event is successfully recorded.
  - `token.wait_on(stream)` runs on the consumer's thread.
  - `completed` is the authoritative status.
  - If no join capacity is available, or the request is served by a blocking transport, the token is published host-complete (`token.host_complete() == true`).
- **FR-33 (test seams).** Existing seams keep working with the semantics in §4.16. New seams are a completion observer, a deterministic transport simulator, and persistent-registration counters.
- **FR-34 (observability).** The gauges and counters in §4.15 are exported once per engine and are not duplicated per facade. Diagnostics never label a mount "GDS-verified" without probe evidence.
- **FR-35 (configuration, rollback, build).**
  - Every new behaviour is behind configuration (§4.14).
  - `engine_mode::legacy` reproduces current behaviour exactly.
  - `CUCASCADE_HAS_CUFILE` remains the build gate.
  - Builds without cuFile and hosts without libcufile fall back cleanly to POSIX.
- **FR-36 (multi-GPU).** Engines, arenas, lanes, fences, and policies are per device. A request with slices on several devices is served by several engines under one coordinator.
- **FR-37 (unchanged paths).** The following keep their current behaviour:
  - host-only reads and writes;
  - host+device pair reads;
  - the device POSIX fallback (buffered fd, double-buffered 2 × 8 MiB pinned scratch per worker);
  - identity-checked O_DIRECT reopen;
  - open-time behaviour.

### 3.2 Non-functional requirements

- **NFR-1 (throughput).** At least 80 % of the matched real-GDS `gdsio` baseline per primary workload per primary filesystem. The comparison uses the same I/O size and concurrency and has direct-path evidence on both sides.
- **NFR-2 (compat non-regression).** On hosts without nvidia-fs (including T4 CI), read and write throughput is within 5 % of the pre-change reactor, within measurement confidence. The default transport there is unchanged.
- **NFR-3 (CPU budget).** At 1 MiB × 10,000 ops/s and QD ≤ 64 on one device: at most 12 µs/op on the submitter, at most 10 µs/op of reaper bookkeeping, a target of 8 event queries/op (hard ceiling 70), and at most 5 µs/op of amortized timer/notification cost. The total is ≤ 1.0 core, with about 35 µs/op expected.
- **NFR-4 (lane residence).** Post-I/O lane residence is at most 65 µs mean at 10k ops/s **[belief]**. Measured storage QD is at least 95 % of `Q_eff` under saturating load.
- **NFR-5 (sliding window).** There are no generation-wide refill gaps. The time-weighted mean storage QD is at least 90 % of `Q_eff` while backlog exists.
- **NFR-6 (memory).** Engine device memory never exceeds the reserved arena. Arena bytes, registered bytes, and metadata are constant after warm-up across repeated context create/destroy cycles.
- **NFR-7 (thread bound).** The thread count never exceeds the bounds in FR-28, including under stalled submissions.
- **NFR-8 (teardown).** Context destruction issues no device-wide-stalling call on engine resources.
- **NFR-9 (engineering).** C++20. RAII for every handle: streams, events, stream registrations, leases, fences, file handles. Catch2 tests.
- **NFR-10 (reversibility).** Each phase can be disabled through configuration without a rebuild, restoring the previous phase's behaviour.
- **NFR-11 (integrity).** Under every injected fault in §8 the data is correct, credits settle exactly once, and no slot or lane is released while an accessor may be live.

---

## 4. Architecture

### 4.1 Terminology

| Term | Meaning |
|---|---|
| **Facade** | An existing `cufile_reactor` instance. It keeps its queue, worker thread, and legacy paths, and hands eligible device slices to the engine. |
| **Engine** | `device_engine`, one per device and process-lifetime. It owns scheduling, operation state, lanes, and the reaper loop. |
| **Lane** | A reactor-owned non-blocking CUDA stream with one I/O event and one copy event. It holds at most one physical operation. |
| **Arena / slab / slot** | Per-device persistent device memory; a 16 MiB `cudaMalloc` allocation, registered when possible; a 1/2/4/8 MiB lease inside a slab. |
| **Piece / operation** | One physical cuFile I/O of at most `S_eff`, with its stable argument block and one coordinator credit. |
| **Fence** | A CUDA event recorded on the caller's resolved stream at submit time. |
| **QD token** | A per-`(device, mount)` admission token held only while an operation's I/O is in flight. |
| **Granule `G`** | The direct-I/O alignment: `max(4096, page size, filesystem-reported logical block size)`. |

### 4.2 Components and data flow

```text
Application thread
  resolve_default_streams → record input fence(s) → dispatch_slices → facade.enqueue (noexcept)
                                                                         │
┌────────────────────────── existing cufile_reactor facade (×N, default 4) ───────────────────────────┐
│ tiered queue → request_scheduler → dispatch_slice                                                   │
│     ├─ eligible device slice on engine-routed mount ──── hand-off (non-blocking) ──────────────┐    │
│     └─ otherwise: legacy inline (sync cuFileRead / POSIX staged / staged write), unchanged     │    │
└────────────────────────────────────────────────────────────────────────────────────────────────┼────┘
                                                                                                 ▼
                     ┌──────────────────────── device_engine (per device) ────────────────────────────┐
                     │ scheduler + reaper thread:                                                     │
                     │   lazy piece expansion · DRR/priority admission · lane/slot/QD-token leasing   │
                     │   per-lane head-event polling · validation · D2D enqueue · credit settlement   │
                     │        │ commands (bounded ring)        ▲ RecordedEvent / results (ring)       │
                     │        ▼                                │                                      │
                     │  mount_submitter (per device × mount) ──┘    engine blocking pool (4, lazy)    │
                     │   WaitEvent(fence) → cuFile*Async → EventRecord   sync cuFile · pread/pwrite   │
                     │        │                                          · fdatasync                  │
                     │        ▼                                     recovery pool (2)                 │
                     │  lanes (≤160 non-blocking streams)            lane synchronize / quarantine    │
                     │  device_arena (16 MiB slabs, registered or unregistered)                       │
                     └────────────────────────────────────────────────────────────────────────────────┘
 control executor (1 thread, process): runtime init · arena warm-up · probes · topology · FS geometry ·
                                       submitter creation/retirement · lane replacement
 runtime (process): cufile_api optional groups · frozen parameters · driver-open ordering
 mount_policy registry (process): state machine · profile · quotas · circuit breaker
```

Ownership rules:

- The engine thread is the only mutator of scheduler containers and operation phases.
- Submitters consume immutable commands and publish results. They never touch scheduler state.
- Ownership of a lane's stream passes from the reaper to the submitter with a command, and back to the reaper with a publication. The two threads never enqueue on the same lane concurrently.
- Facade workers call the same extracted legacy functions as before. The engine's blocking pool calls those functions for engine-originated work only. On hosts where no mount is ever engine-routed, the blocking pool is never started, so compat keeps exactly the existing worker count.

### 4.3 Threads

| Thread | Count | Does | Never does |
|---|---|---|---|
| Application | any | Stream resolution, fence record, `enqueue` | cuFile calls, waiting for resources |
| Facade worker | `cufile_n_reactors` (4) | Legacy transfers; engine hand-off; `ensure_probe` trigger | Async cuFile |
| Scheduler/reaper | 1 per active device | Admission, `cudaEventQuery`, non-blocking CUDA enqueues (D2D, D2H, H2D, WaitEvent, EventRecord), bookkeeping | Any blocking call (FR-5) |
| Mount submitter | ≤ 8 per device, ≤ 16 per process | `cudaStreamWaitEvent(fence)`, `cuFile{Read,Write}Async`, `cudaEventRecord`, publish | Scheduler mutation |
| Blocking pool | 4 per process, lazy | Rerouted sync `cuFileRead`, buffered `pread`/`pwrite`, `fdatasync` | — |
| Recovery | 2 per process | `cudaStreamSynchronize` on an **owned lane** only | Sync on caller streams |
| Control | 1 per process | Init, warm-up, probes, discovery, thread lifecycle | Transfer hot path |

Submitter lifecycle:

- A submitter is created lazily when a mount first becomes engine-routed. Until it exists, the mount stays on legacy routing.
- An empty submitter parks at once and is retired after 60 s idle if it owns no command.
- Mounts beyond the cap stay on bounded fallback execution until a slot frees.
- A submitter stuck inside libcufile keeps its thread slot until the call returns.

### 4.4 Persistent arena

**Sizing.**

```text
allocation_target = floor16MiB(min(config.arena_bytes [256 MiB], immediately reservable device memory))
registered_target = min(allocation_target,
                        0.5 × max(free_BAR1 − 64 MiB, 0),            // [belief] fraction and headroom
                        cuFile pinning headroom (max_device_pinned_mem_size − already pinned))
without BAR1 telemetry: register 64 MiB first, then one slab at a time until failure or target
```

- BAR1 limits only the registration target. Reservation accounting covers the whole allocation.
- On a T4 (about 256 MiB BAR1 **[belief]**) this gives at most 96 MiB registered. The rest is unregistered tier or unused.
- Registration success is authoritative.

**Organization.**

- A slab is a 16 MiB `cudaMalloc` block with flags `registered`, a class in {1, 2, 4, 8} MiB, an owner mount, a free bitmap, and `draining`.
- All slots in a slab share one class, and every class divides 16 MiB exactly. There is no external fragmentation inside a slab.
- In steady state all slabs assigned to a mount have class `S_eff`. Every piece (at most `S_eff`) leases one slot.
- Capacity is provisioned as `lanes × S_eff ≤ B_m`, so a free lane implies a free slot. Rounding a small piece up to a full slot therefore never reduces QD.
- During reclassification mixed classes may exist. Allocation picks the smallest class at least as large as the piece among the mount's non-draining slabs.
- Quota rounding loses less than 16 MiB per mount. Reclassification withholds at most 16 MiB per mount.
- Arena registrations are tracked by the arena and never by `registration_table`. Arena memory cannot overlap caller destinations, so the `ALREADY_REGISTERED → conflict → POSIX` path (Appendix C §2.4) is never triggered by the arena.

**Unregistered-device tier.**

- Slabs that failed registration stay in the arena.
- A mount may use them (`cuFile internal bounce → unregistered slab → D2D → destination`) only after its own unregistered-async probe shows non-blocking submission and acceptable throughput.
- The tier has its own circuit breaker. Bytes through it are counted as `bounce_bytes`.
- If the tier is not qualified, the mount uses sync cuFile or POSIX.

**Lifetime.**

- The arena registry is a never-destroyed process singleton, mirroring `scratch_pool` (Appendix C §5(h)).
- An explicit maintenance call, `device_arena::release_for_maintenance()`, requires complete quiescence. It is the only path that deregisters or frees.

### 4.5 Transport selection

#### 4.5.1 Policy identity

The key is computed when the `io_object` is opened, where blocking is allowed, and cached in `cufile_io_object`. Its parts are:

- the mount namespace (`/proc/self/ns/mnt` inode);
- the mount ID (`statx(STATX_MNT_ID)`, falling back to `/proc/self/fdinfo`);
- `fstatfs().f_fsid`;
- the device UUID;
- `runtime::generation()`.

Filesystem geometry discovery is cached by `fsid`. Transport state is never shared across mounts, devices, or runtime generations.

#### 4.5.2 State machine

```text
(process: !gds_available() or forced compat)  ───────────────►  known_compat   (no probe)
(per-class dstatusflags bit absent / no O_DIRECT / handle not registered) ─► unsupported (file- or mount-level)

unknown ──first eligible use──► probing ──evidence + non-blocking submit──► verified_direct
                                   │                                             │ registration unavailable,
                                   │ compat-blocking submit                      │ unregistered tier qualified
                                   ├──────────────────────► known_compat        ▼
                                   └─ no telemetry / short file ─► unknown    degraded_async
verified_direct / degraded_async ──slow submissions──► circuit_open ──drain──► cooldown ──timer──► probing
operator override ──► operator_qualified   (reported distinctly; never "verified")
```

#### 4.5.3 Probe and evidence

The probe runs on the control executor:

1. Lease one arena slab. Use a registered slab for the direct probe and an unregistered slab for the unregistered-tier probe. Take one probe lane and retain the opened file.
2. Issue aligned `cuFileReadAsync` reads of existing bytes. Use several reads totaling about 16 MiB. Files shorter than one granule are skipped.
3. Record the call-return latency, the completion latency, and the delta of the direct-path evidence source.
4. Evidence sources, in order of preference:
   - (a) per-process cuFile statistics filtered to the probe;
   - (b) nvidia-fs counters taken during an *isolated qualification interval*, in which engine admission for that device is paused and no legacy cuFile call is in flight in this process;
   - (c) none. The mount stays `unknown`.
5. A global counter increase during unrelated traffic is not attribution.
6. Probes are rate-limited to one concurrent probe per device and one probe per mount per cooldown.

Direct **writes** additionally require `config.async_writes`, a profile flag for the filesystem class and client version, and `verified_direct` reads. After the first 16 MiB of async writes on a mount, the write-evidence counter is checked. If there is no evidence, writes on that mount revert to D2H plus `pwrite` and the event is logged once.

#### 4.5.4 Decision ladder (per file, per mount, per device)

| Condition | Device read | Device write (aligned body) |
|---|---|---|
| `verified_direct`, registered slabs, async group present, engine ready, fence present | **Engine: async into registered slot + D2D** | **Engine: D2D into registered slot + `cuFileWriteAsync`** (if write-eligible) |
| `degraded_async` (registration unavailable, unregistered tier qualified) | Engine: async into unregistered slot + D2D | D2H + `pwrite` unless separately qualified |
| Async group missing, `circuit_open`, `cooldown`, `unknown`, `probing`, submitter cap reached, engine not ready | Legacy sync `cuFileRead` on facade workers (existing behaviour) | Existing write decision (`cuFileWrite` only when direct is verified; otherwise staged) |
| `known_compat` with a usable cuFile handle | **Legacy sync `cuFileRead`** (≤ 64 MiB pieces) | **D2H + `pwrite`** |
| No library, no handle, O_DIRECT refused, `unsupported`, explicit POSIX | Existing POSIX path | Existing POSIX path |
| Completed failure classified unsupported-direct | Per-file fallback one rung down | Fallback only when progress is known |

Engine-eligible slices are device-only reads of any non-zero size and device-only write bodies of at least `G`. The 256 KiB `gds_threshold` POSIX detour does not apply on engine-routed mounts, because read-around staging replaces it. Host-only and host+device slices always take the legacy paths.

### 4.6 Per-filesystem profiles and geometry

All numbers are starting hypotheses **[belief]**. Section 4.7 determines the effective values. Tuning uses a static profile table, qualified by QD sweeps (8–128) and op-size sweeps (1/2/4/8 MiB), plus the online health and capacity policy.

| Class | Read transport | Write transport | Target QD | Preferred op size `S_pref` | Lanes = `lanes(Q)` |
|---|---|---|---:|---|---:|
| Local NVMe (ext4/xfs) | async + lanes | async body | 32 | 4 MiB | 40 |
| GPFS / Storage Scale | async + lanes | async body if client/version qualified | 64 | FS block size: 1–8 MiB → that size; > 8 MiB → 8 MiB; < 1 MiB → 1 MiB | 80 |
| WEKA (wekafs) | async + lanes | async body if qualified | 64 | 2 MiB (client layout if exposed) | 80 |
| VAST / qualified NFS-RDMA | async + lanes | async body if qualified | 64 | 1 MiB | 80 |
| Lustre | async + lanes | async body if qualified | 64 | File stripe size capped at 8 MiB; 1 MiB if the layout is unavailable | 80 |
| BeeGFS | async + lanes | async body if qualified | 64 | File chunk size, otherwise 2 MiB | 80 |
| Compat | sync `cuFileRead` on 4 facade workers, ≤ 64 MiB pieces | D2H + `pwrite` | 4 | existing | existing copy streams |

Geometry rules:

- **GPFS.**
  - The block size comes from `fstatfs(fd).f_bsize`, validated in P0 against `mmlsfs -B` (§9).
  - Pieces are cut on the absolute `S_eff` file-offset grid, which coincides with block boundaries for power-of-two block sizes.
  - Under memory pressure the engine tiles within a block instead of expanding requests.
  - NSD spread is confirmed with `mmdiag --iohist`, `mmdiag --rdma`, and NSD activity. Round-robin placement is **[belief]**.
- **Lustre.** The layout comes from a supported layout API, including composite-layout component boundaries. No piece crosses a stripe or component boundary. `statfs` is not a substitute for the layout.
- **BeeGFS.** The chunk size comes from file metadata when available.
- Cut set: the multiples of `S_eff` together with the geometry boundaries. Pieces may be smaller than `S_eff`.

### 4.7 Effective capacity

Per mount, `B_m` is the assigned slab bytes, `C_m` the assigned lanes, `Q_m` the QD target, and `lanes(q) = q + ceil(q/4)`. The fit rule is FR-16.

| Available | Requested | Effective |
|---|---|---|
| 256 MiB | QD 32, 8 MiB | 4 MiB; 40 lanes use 160 MiB |
| 256 MiB | QD 64, 8 MiB | 2 MiB; 80 lanes use 160 MiB |
| 128 MiB each, two mounts | QD 64, 2 MiB each | 1 MiB each; 80 MiB per mount |
| explicit 8 MiB sweep | QD 32 / 64 | needs a 320 / 640 MiB arena |

Benchmarks must report the effective QD and op size, never the requested ones. Idle assigned capacity is borrowable. Leases in flight are never revoked. New quotas take effect as draining completes.

### 4.8 Stream dependency and default completion protocol

1. **Submit (caller thread).** Resolve streams, record a fence per `(device, resolved stream)`, then publish. For null → `cudaStreamLegacy`, the event recorded on the legacy stream captures blocking-stream dependencies (commit `469a0221`).
2. **Input wait (GPU-side).** Every engine operation enqueues `cudaStreamWaitEvent(lane, fence)` before its first access to caller memory or staging. No host thread synchronizes the caller stream.
3. **Admission hint.**
   - The reaper queries each unready fence at most once per 100 µs. This is only a hint that limits how many lanes wait (FR-26). It is never the correctness gate.
   - An input-waiting operation holds a lane and a slot but **no QD token**. When the hint turns ready, the operation is accounted as I/O in flight.
   - Storage QD may transiently exceed `Q_eff` by at most the waiter cap. Admission pauses until QD is back under the target.
4. **Output.** The default future is host-complete. It resolves when the reaper has observed the final copy event (or the I/O event for direct registered reads) of the last piece. No join onto the user stream is needed, and callers are unchanged.

### 4.9 Read data path

```text
Caller                 Scheduler/reaper              Submitter                    CUDA / storage
 record fence
 enqueue ───► facade ─► hand-off
                       expand piece (lazy)
                       lease lane+slot(+token)
                       command ───────────────────► WaitEvent(lane, fence) ─────►
                                                    cuFileReadAsync(slab base, &args.., lane) ─► DMA → slot
                                                    EventRecord(io_ev, lane) ───►
                       ◄──────────────── publish {op, io_inflight, gen}
                       poll lane head: EventQuery(io_ev)
                       release QD token
                       validate bytes_done vs required interval
                         ├─ incomplete → aligned retry (same lane/slot/credit)
                         └─ ok → cudaMemcpyAsync D2D exact bytes (lane) ───────► slot → destination
                                 EventRecord(copy_ev, lane) ───────────────────►
                       poll lane head: EventQuery(copy_ev)
                       release slot + lane; settle credit
 future ready ◄─────── last credit
```

A direct registered read (FR-18) omits the D2D copy and the slot. Validation and final-event lifetime are the same.

### 4.10 Write data path

```text
Caller                 Scheduler/reaper                         Submitter / blocking pool       CUDA / storage
 record fence (producer)
 enqueue ───► facade ─► hand-off; split head / body / tail; assign file write sequence
 BODY piece (write-eligible mount):
                       lease lane+slot
                       WaitEvent(lane, fence); D2D source → slot; EventRecord(stage_ev) ──────►
                       poll: stage_ev complete → acquire QD token (and granule interval)
                       command ───────────────────────────────► cuFileWriteAsync(slab base.., lane) ─► O_DIRECT
                                                                EventRecord(io_ev) ; publish
                       poll io_ev → validate bytes_done → (short suffix retry | finish)
 EDGE piece (head/tail) and unverified bodies on engine-routed mounts:
                       lease lane + pinned scratch cell
                       WaitEvent(lane, fence); D2H source → scratch; EventRecord ─────────────►
                       poll complete ─────────────────────────► blocking pool: pwrite (buffered fd) exact bytes
                       ◄──────────────── result → finish
 COMPAT / unknown mounts: whole slice stays on the facade's legacy staged-write path (unchanged).
 future ready ◄─────── all credits settled; note_written for successful ranges only
```

Source-lifetime semantics are unchanged: the source must remain valid until the future resolves. This holds even though the staging copy would technically allow earlier release.

### 4.11 Reaper mechanics and cost model

**Polling policy** (requested deadlines, kept in a min-heap keyed by due time):

| Event | First poll | Not-ready backoff |
|---|---|---|
| I/O event | `publish_time + max(10 µs, 0.5 × p50_completion(mount, class))`; 10 µs with no history | 10 → 25 → 50 → 100 µs, cap 100 µs |
| Copy / staging event | at once after enqueue + 2 µs | 2 → 5 → 10 µs, cap 10 µs |
| Fence hint | at request arrival | ≤ 1 query per 100 µs per waiting request |
| Submission watchdog | `call_start + 2 ms` | — |

Loop discipline:

- Process at most 16 due events, then service the control and completion rings and admission.
- When active, wait on an `eventfd` with a high-resolution timeout until the earliest deadline. Never spin continuously.
- When fully idle, block on the notification with a 20 ms health-check timeout.
- The reaper sets a 1 µs timer slack **[belief]**. Timer overshoot and round duration are gauges.

**Residence arithmetic at 1 MiB × 10k ops/s [belief until measured]:**

| Component | Time |
|---|---:|
| I/O completion discovery (100 µs polling) | 50 µs |
| Validation + copy enqueue | 4 µs |
| D2D execution | 6 µs |
| Copy-completion discovery | 5 µs |
| **Post-I/O lane/slot residence** | **65 µs** |

- By Little's law, 10,000 ops/s × 65 µs = 0.65 extra occupied lanes.
- With no headroom this would cost about 2.0 % of QD 32 and about 1.0 % of QD 64. The 25 % lane headroom (40 and 80 lanes) absorbs it.
- At 100k ops/s it is 6.5 lanes, which is still inside the headroom.

**CPU budget** (NFR-3):

- The expected predictive-polling case is about 35 µs/op.
- The conservative query-heavy case (70 queries × 1 µs) is about 97 µs/op: about 0.85 reaper core plus 0.12 submitter core at 10k ops/s.
- Submitter wake-ups are at most 10k/s unbatched and about 2.5k/s when draining four commands per wake. A submitter drains up to eight commands per wake and never waits to fill a batch. At 3–8 µs per wake this is 0.03–0.08 CPU-seconds per second **[belief]**.

These are budgets, not measurements. P0 measures them, and the engine is not enabled by default until they hold or are returned (§8.2).

### 4.12 Scheduling and backpressure

- **Hand-off.**
  - `dispatch_slice` on the facade worker transfers an eligible slice, with its credit, object reference, and fence, to the engine's control ring.
  - A hand-off does not count against the facade's per-pass `max_op_bytes` budget, so all eligible slices of a request are handed off in one pass.
  - The facade tracks `_engine_outstanding` for shutdown.
- **Lazy expansion.** The engine keeps a cursor per slice and expands pieces only while admission capacity or a small lookahead exists. Pending expanded pieces are capped at 2 × the mount's lanes. Operation records come from a stable-address pool of 4 × `device_lane_cap` records. Expansion pauses when the pool is empty.
- **Credits.** Each emitted non-final piece first allocates its record, then calls `coordinator.add_tasks(1)`. The final piece takes the slice's original credit.
- **Order.** High tier first. Within a tier, byte-deficit round-robin over mounts and then over requests, with quantum `S_eff`. After four high piece-starts on a mount with waiting low work, one low piece is admitted.
- **Demand reserve.** Low-tier admission on a mount needs `free_lanes > ceil(lanes/8)` or no pending high work on that mount.
- **Preemption.** Applies only to pending pieces. Submitted operations drain. The op-size cap bounds inversion.
- **Backpressure.** With no lane, slot, or token, pieces stay pending in the engine. Nothing blocks a caller. `enqueue` fails only on the metadata budget (FR-3).

### 4.13 Network affinity and process configuration

**Affinity algorithm** (run at engine or submitter creation on the control executor):

1. Get the GPU's PCI BDF and NUMA node (CUDA/NVML plus sysfs).
2. Get the permitted CPUs from the process cpuset.
3. Enumerate RDMA ports with their PCI BDF, NUMA node, addresses, link state, and route reachability to the mount's storage endpoints.
4. Rank the NICs per `(mount, device)` by:
   - vendor-supported path eligibility;
   - same NUMA node as the GPU;
   - shortest PCI topology distance;
   - link availability and configured preference.
5. Pin the reaper to a permitted physical core on the GPU node.
6. Pin the mount submitter to a different physical core on the selected NIC's node. If the GPU and NIC nodes differ, the reaper stays GPU-local and the submitter NIC-local. P4 measures this against GPU-local submission.
7. First-touch the blocking-pool pinned scratch on that node.
8. Allocate cores round-robin across engines, avoiding SMT siblings until physical cores run out.
9. If any input is missing, leave affinity unset and report why.

Explicit CPU and NIC overrides are honoured. CPU placement does not by itself steer libcufile or filesystem-client routing.

**Process parameters.** `process_cufile_parameters` is a typed allowlist. It is applied through adapters that are compiled only where the installed `cufile.h` declares the setter and the parameter ID. All names, IDs, and units need compile-time or probe validation **[belief]**.

| Setting | JSON key to validate | Application |
|---|---|---|
| Compat permission | `properties.allow_compat_mode` | bool setter where supported |
| Forced compat | `properties.force_compat_mode` | bool setter or pre-open JSON |
| Direct-I/O worker pool | `properties.max_direct_io_threads` | int setter where supported |
| Direct-I/O maximum | `properties.max_direct_io_size_kb` | size setter (explicit KiB) |
| Registered-memory ceiling | `properties.max_device_pinned_mem_size_kb` | size setter or JSON |
| Internal device cache | `properties.max_device_cache_size_kb` | size setter or JSON |
| Batch size (reporting only) | `properties.io_batchsize` | not set |
| NIC addresses | `properties.rdma_dev_addr_list` | release-supported list or JSON |
| RDMA policy | `properties.rdma_load_balancing_policy` | string setter or JSON |
| Routing | `properties.rdma_dynamic_routing`, `properties.rdma_dynamic_routing_order` | versioned JSON unless a setter exists |
| GPU bounce slabs | `CUFILE_PARAM_GPU_BOUNCE_BUFFER_SLAB_{SIZE_KB,COUNT}` | `cuFileSetParameterGpuBounceBufferSlabArray` where available |
| Diagnostics | `profile.cufile_stats`, `logging.level` | versioned JSON |

Rules:

- The first `runtime::ensure_initialized` call freezes the parameters. Later differing requests are reported, and are errors in strict mode.
- If the driver was already opened by another component, pre-open settings are reported as unapplied or unverifiable. The driver is never closed and reopened.
- Versioned JSON examples ship under `docs/` and are selected through the release-supported configuration-file mechanism.
- Only `CU_FILE_STREAM_PAGE_ALIGNED_INPUTS` is used for stream registration. The fixed-offset and fixed-size flags stay off.

**Filesystem qualification (P4).**

- GPFS: `mmdiag --iohist`, `mmdiag --rdma`, NSD utilization, and RDMA counters. Page-pool and prefetch effects are tested separately.
- WEKA: the actual kernel-client and GDS combination, NIC selection, and queue utilization.
- VAST: the supported NFS-RDMA client, multipath, and `nconnect` combination. Extra TCP connections do not prove GDS.
- Lustre and BeeGFS: layout metadata and client-version support.
- All targets: path loss, server pause, stale handles, and recovery without pathname reopen.

### 4.14 API and configuration changes

- `io_context.hpp`: the default read and write contracts are unchanged. P5 adds `device_readv_async_io_ev`, returning `device_read_ticket` (§6.12).
- `types.hpp`: `d_buffer` gains an optional `registered_buffer_lease` (FR-18) and an optional fence reference.
- `cufile::config` gains `async_engine_config` (§6.2).
  - `mode` is one of `legacy`, `async_auto`, and `async_simulated_for_testing`. The default is `legacy` through P4. P5 changes the default to `async_auto` only after the gates pass.
  - The sub-flags are `async_reads`, `async_writes`, `unregistered_device_tier`, `direct_registered_io`, `stream_register`, `affinity`, and `event_api`. Each can be disabled independently.
- Existing knobs keep their meaning on the legacy path: `max_op_bytes`, `gds_threshold`, `register_min_bytes`, `poll_interval_us`, `low_service_interval`, `max_inflight`, `compat_mode`.

### 4.15 Observability

All of the following are per `(device, mount)` unless noted.

- **Queue and capacity:** storage QD (current, max, time-weighted mean), `Q_eff`, `S_eff`, assigned and borrowed lanes and slabs, lanes by phase (input_wait, staging, submission_queued, io, copy, recovery, quarantined), pending bytes per tier.
- **Backpressure:** slot-wait count and duration, lane-wait count and duration, refill-gap histogram (time a token was free while backlog existed), metadata rejections.
- **Latency:** submit-call histogram, I/O completion histogram, post-I/O residence histogram, queue delay per tier.
- **Reaper:** event queries per op, timer overshoot, round duration, wake-ups per second (reaper and submitters).
- **Correctness and health:** short reads, retries, buffered-suffix completions, copy/record failures, recoveries, quarantined lanes, cancellations.
- **Transport:** policy state, probes (run, passed, failed, and why), demotions, cooldowns, bytes by transport (async-registered, async-unregistered = `bounce_bytes`, sync-cuFile, POSIX), compat fallbacks.
- **Memory:** arena bytes, registered bytes, unregistered-slab bytes, reclassifications.
- **Runtime:** per-parameter applied / unapplied / unverified status, optional-group presence, affinity decisions and reasons.

### 4.16 Test-seam semantics

| Seam | Semantics in the new design |
|---|---|
| `set_cufile_transfer_hook_for_testing` | Fires on the actual transport thread immediately before every sync or async cuFile call, including retries. The context adds `{transport, phase, attempt, lane, generation}`. In async mode it means "about to enqueue I/O", not "input GPU work completed". |
| `set_completion_observer_for_testing` (new) | Fires on the reaper after validation with `{op id, bytes_done, outcome}`. |
| `set_staging_copy_delay_for_testing` | Delays the copy phase GPU-side without blocking the reaper, via a gated event that the test injects ahead of the copy on the lane. |
| `set_copy_event_record_failure_for_testing` | Forces a failure after the copy enqueue, which exercises recovery. |
| `cufile_write_calls_for_testing` | Counts sync and async write attempts. |
| `live_registrations_for_testing()` | Unchanged: transient `registration_table` entries only. |
| `persistent_registered_bytes_for_testing()`, `arena_slab_counts_for_testing()` (new) | Arena state. These intentionally do not return to zero on context destruction. |
| `transport_simulator` (new, test-only) | A replacement `cufile_api` table that provides deterministic async behaviour: gated completion, programmable `bytes_done`, blocking submit, and errors. It may use `cudaLaunchHostFunc` because it is test-only. |

Existing registration-sharing tests stay on the legacy backend.

### 4.17 Delivery phases

| Phase | Shippable result | Config gate / rollback |
|---|---|---|
| **P0 – transport proof** | Standalone async-QD harness (GPFS first, then NVMe, WEKA, VAST); real-GDS `gdsio` baselines; reaper-cost microbenchmark; optional symbol groups; `runtime` ordering | No behaviour change; `mode=legacy` |
| **P1 – seams and policy** | Extracted `range_plan` helpers and `fallback_executor` functions; `mount_policy` with probes (state is reported, routing unchanged); test-seam equivalents | Policy is advisory; legacy routing |
| **P2 – resources** | `device_arena`, lanes, operation pool, reservation wiring, warm-up, gauges | `arena_bytes=0` disables |
| **P3 – async reads** | Engine read path: fences, lazy striping, validate/copy, demotion, unregistered tier when qualified | `mode=async_auto` + `async_reads`; per-mount override to legacy |
| **P4 – writes, direct leases, network qualification** | Async write bodies, edges, flush sequencing, `registered_buffer_lease`, topology/affinity, FS profiles | `async_writes`, `direct_registered_io`, `affinity` independently |
| **P5 – event API and default flip** | `device_readv_async_io_ev`; default `async_auto` after the gates pass | `event_api=false`; `mode=legacy` |

No phase requires changing an existing DISK caller.

---

## 5. File layout

Paths follow the appendices. Exact placement is confirmed during implementation.

| Path | New / Modified | Purpose |
|---|---|---|
| `include/cucascade/io/cufile/cufile_api.hpp` | M | Add optional symbol groups and capability flags |
| `src/io/cufile/cufile_api.cpp` | M | Resolve groups independently; missing group ≠ missing library |
| `include/cucascade/io/cufile/runtime.hpp` | N | Process runtime: frozen parameters, init ordering, `engine_possible()`, generation |
| `src/io/cufile/runtime.cpp` | N | Typed setter adapters, driver open, readback/verification report |
| `include/cucascade/io/cufile/config.hpp` | M | `async_engine_config`, `process_cufile_parameters`, mount overrides |
| `include/cucascade/io/cufile/mount_policy.hpp` | N | Policy key, state machine, profile, quotas, circuit-breaker API |
| `src/io/cufile/mount_policy.cpp` | N | Probe execution, evidence sources, geometry discovery, demotion logic |
| `include/cucascade/io/cufile/device_arena.hpp` | N | Slabs, size classes, leases, quota and reclassification API |
| `src/io/cufile/device_arena.cpp` | N | Warm-up (reserve, allocate, register), never-destroyed registry |
| `include/cucascade/io/cufile/operation.hpp` | N | `AsyncArgs`, `Operation`, `Phase`, `RecordedEvent`, `SubmissionFence`, stable pool |
| `include/cucascade/io/cufile/device_engine.hpp` | N | Engine public interface, lane pool, gauges |
| `src/io/cufile/device_engine.cpp` | N | Scheduler/reaper loop, admission, validation, retirement, capacity fit |
| `include/cucascade/io/cufile/async_transport.hpp` | N | Submitter command/completion rings, lifecycle, watchdog fields |
| `src/io/cufile/async_transport.cpp` | N | Submitter thread body for read and write submission |
| `include/cucascade/io/cufile/fallback_executor.hpp` | N | Extracted sync-cuFile and POSIX functions; blocking and recovery pools |
| `src/io/cufile/fallback_executor.cpp` | N | Implementation moved from `cufile_reactor.cpp` without behaviour change |
| `include/cucascade/io/cufile/file_write_state.hpp` | N | Per-file-identity write sequences, granule interval table, flush waiters |
| `src/io/cufile/file_write_state.cpp` | N | Process-wide `(st_dev, st_ino)` registry |
| `include/cucascade/io/cufile/topology.hpp` | N | GPU/NIC/NUMA discovery types, affinity plan |
| `src/io/cufile/topology.cpp` | N | sysfs/NVML/RDMA enumeration, CPU assignment |
| `include/cucascade/io/cufile/cufile_reactor.hpp` | M | Facade: engine hand-off, `_engine_outstanding`, shared runtime in `reactor_context` |
| `src/io/cufile/cufile_reactor.cpp` | M | `dispatch_slice` routing, shutdown drain of handed-off work, legacy calls via `fallback_executor` |
| `include/cucascade/io/cufile/cufile_ioctx.hpp`, `src/io/cufile/cufile_ioctx.cpp` | M | Warm-up trigger, policy key at object open, aggregated gauges |
| `include/cucascade/io/cufile/platform.hpp`, `src/io/cufile/platform.cpp` | M | Route driver open through `runtime`; expose FS class and per-mount diagnostics |
| `include/cucascade/io/cufile/detail.hpp` | M | Test seams (§4.16) |
| `include/cucascade/io/range_plan.hpp` | N | Shared `staged_physical_range`, `split_write_range`, `next_cut` |
| `include/cucascade/io/uring/types.hpp`, `src/io/uring/uring_reactor.cpp` | M | Consume `range_plan.hpp` helpers with no behaviour change |
| `include/cucascade/io/templated_ioctx.hpp` | M | Submit-time fence recording; fence attached to slices |
| `include/cucascade/io/types.hpp` | M | `d_buffer` fence reference; optional `registered_buffer_lease` |
| `include/cucascade/io/io_request.hpp` | M | Credit hand-off helpers for lazy expansion |
| `include/cucascade/io/io_context.hpp` | M | Document the unchanged contract; declare the `_ev` API (P5) |
| `include/cucascade/io/device_io_event.hpp` | N | `device_read_ticket`, `device_join_token` |
| `include/cucascade/io/config.hpp`, `src/cudf/disk_io_context.cpp` | M | Wiring, reservation-manager hookup, warm-up at context creation |
| `test/io/cufile/test_cufile_api_optional.cpp` | N | Optional-group loading |
| `test/io/cufile/test_runtime_params.cpp` | N | Init ordering, parameter reporting |
| `test/io/cufile/test_device_arena.cpp` | N | Slabs, classes, quotas, degradation |
| `test/io/cufile/test_mount_policy.cpp` | N | Probe, evidence, demotion |
| `test/io/cufile/test_engine_read.cpp` | N | Read state machine, striping, short reads |
| `test/io/cufile/test_engine_write.cpp` | N | Write phases, conflicts, flush |
| `test/io/cufile/test_engine_sched.cpp` | N | Priority, liveness, backpressure |
| `test/io/cufile/test_engine_failure.cpp` | N | Recovery, quarantine, cancellation, shutdown |
| `test/io/cufile/test_event_api.cpp` | N | Two-phase join |
| `test/io/cufile/transport_simulator.{hpp,cpp}` | N | Deterministic fake `cufile_api` |
| `test/io/cufile/test_cufile_reactor.cpp`, `test_cufile_write.cpp`, `test/io/test_null_stream_ptds.cpp` | M | Re-express `:594`, `:660`, `:854`; keep the null/PTDS matrix |
| `benchmark/benchmark_cufile_async_qd.cpp` | N | P0 standalone transport and CPU-cost harness |
| `benchmark/benchmark_cufile_reactor.cpp` | N | Workload replay with gates |
| `benchmark/benchmark_disk_converter.cpp` | M | Real-GDS reference numbers alongside the compat ones |
| `docs/io/cufile-reactor.md`, `docs/io/cufile-profiles/*.json` | N | Operational policy; versioned `cufile.json` examples per filesystem |
| `CMakeLists.txt` (io, test, benchmark) | M | New sources under `CUCASCADE_HAS_CUFILE`; compile checks for setter adapters |

---

## 6. Pseudo-code

### 6.1 `cufile_api`: optional groups

```cpp
struct cufile_api {
    // Required (unchanged): driver_open, driver_get_properties, handle_register/deregister,
    // buf_register/deregister, read, write.  Missing any => library treated as absent.

    struct async_read_group  { CUfileError_t (*read_async)(CUfileHandle_t, void*, size_t*, off_t*, off_t*, ssize_t*, CUstream); };
    struct async_write_group { CUfileError_t (*write_async)(CUfileHandle_t, void*, size_t*, off_t*, off_t*, ssize_t*, CUstream); };
    struct stream_group      { CUfileError_t (*stream_register)(CUstream, unsigned);
                               CUfileError_t (*stream_deregister)(CUstream); };
    struct batch_group       { /* SetUp, Submit, GetStatus, Cancel, Destroy: capability reporting only */ };
    struct param_group       { /* only members whose prototypes exist in the installed cufile.h */ };

    std::optional<async_read_group>  async_read;
    std::optional<async_write_group> async_write;
    std::optional<stream_group>      stream;
    std::optional<batch_group>       batch;
    std::optional<param_group>       params;
};

cufile_api load_cufile_api() {
    auto lib = dlopen("libcufile.so.0");
    cufile_api api = resolve_required_or_absent(lib);          // existing rule
    api.async_read  = resolve_all_or_nullopt<async_read_group>(lib,  {"cuFileReadAsync"});
    api.async_write = resolve_all_or_nullopt<async_write_group>(lib, {"cuFileWriteAsync"});
    api.stream      = resolve_all_or_nullopt<stream_group>(lib, {"cuFileStreamRegister", "cuFileStreamDeregister"});
    api.batch       = resolve_all_or_nullopt<batch_group>(lib, batch_symbol_names);
    api.params      = resolve_declared_setters(lib);           // per-symbol optional inside the group
    return api;
}
```

### 6.2 Configuration

```cpp
enum class engine_mode { legacy, async_auto, async_simulated_for_testing };

struct async_engine_config {
    engine_mode mode = engine_mode::legacy;       // P5 flips the default to async_auto
    bool async_reads = true, async_writes = false;
    bool unregistered_device_tier = true, direct_registered_io = false;
    bool stream_register = true, affinity = true, event_api = false;

    size_t   arena_bytes = 256_MiB;               // per device; multiple of 16 MiB; 0 disables the engine
    size_t   registration_first_step = 64_MiB;    // without BAR1 telemetry
    double   bar1_fraction = 0.5;  size_t bar1_headroom = 64_MiB;                 // [belief]
    unsigned device_lane_cap = 160;
    unsigned max_submitters_per_device = 8, max_submitters_process = 16;
    std::chrono::seconds submitter_idle_retire{60};
    unsigned blocking_workers = 4, recovery_workers = 2;
    size_t   max_pending_slices_per_device = 1u << 16;
    unsigned max_join_streams_per_device = 32;

    struct { std::chrono::microseconds submit_max{100}; size_t bytes = 16_MiB; } probe;      // [belief]
    struct { std::chrono::microseconds slow_floor{100}; unsigned p99_multiple = 4;
             unsigned slow_of_last_eight = 3; std::chrono::milliseconds stuck{2};
             std::chrono::seconds cooldown{30}; std::chrono::seconds cooldown_max{960}; } demotion;
    struct { std::array<unsigned,4> io_backoff_us{10,25,50,100};
             std::array<unsigned,3> copy_backoff_us{2,5,10};
             unsigned fence_hint_us = 100; unsigned due_events_per_round = 16;
             std::chrono::milliseconds idle_health{20}; } poll;
    struct { unsigned transient_attempts = 3; std::chrono::milliseconds base{10}, cap{1000}; } retry;  // [belief]

    std::vector<mount_override> mount_overrides;  // {path prefix, forced state, qd, op_bytes, nic, cpus}
    process_cufile_parameters   process_params;   // typed allowlist (§4.13)
    bool strict_parameters = false;
};
```

### 6.3 `runtime`

```cpp
class runtime {                                   // never-destroyed process singleton
public:
    static runtime& instance();
    init_report ensure_initialized(const process_cufile_parameters& p, bool strict);  // idempotent
    bool engine_possible() const noexcept;        // async_read present && gds_available() && mode != legacy
    uint64_t generation() const noexcept;
    const cufile_api& api() const noexcept;
};

init_report runtime::ensure_initialized(const process_cufile_parameters& p, bool strict) {
    std::call_once(_once, [&] {
        _api = load_cufile_api();
        if (!_api.present()) { _report.library_absent = true; return; }
        _frozen = p;
        for (auto& s : allowlisted_settings(_frozen)) {
            if (!adapter_compiled_for(s) || !_api.params) { _report.mark(s, status::unsupported); continue; }
            _report.mark(s, apply_setter(s) ? status::applied : status::rejected);
        }
        _report.open_status = _api.driver_open();            // never call cuFileDriverGetP2PFlags
        _props = _api.driver_get_properties();
        for (auto& s : _report.applied())                    // read back where a getter exists
            if (getter_available(s) && readback(s) != s.value) _report.mark(s, status::unverified);
        ++_generation;
    });
    if (p != _frozen) _report.note("parameters frozen by earlier initialization; differing request ignored");
    if (strict && (_report.has_unsupported_or_rejected() || p != _frozen)) throw configuration_error(_report);
    return _report;
}
```

`platform.cpp::probe_platform` and `register_with_cufile` must call `runtime::ensure_initialized` instead of opening the driver themselves.

### 6.4 `device_arena`

```cpp
enum class slot_class : uint8_t { mib1, mib2, mib4, mib8 };

struct slab {
    void* base; bool registered; slot_class cls; mount_id owner;
    std::bitset<16> free; uint8_t live; bool draining;
};
struct staging_lease {                  // move-only RAII; release() is reaper-only
    slab* s; uint8_t slot;
    void*  slab_base() const;           // pointer passed to cuFile
    off_t  slab_offset() const;         // slot * class_bytes
    void*  data() const;                // slab_base + slab_offset
    size_t capacity() const; bool registered() const;
};

class device_arena {
public:
    static device_arena& for_device(int device);            // never destroyed
    warmup_report warm_up(const async_engine_config&, reservation_manager&);   // control executor only
    std::optional<staging_lease> try_lease(mount_id, size_t bytes, bool need_registered) noexcept;
    void release(staging_lease&&) noexcept;
    void set_quota(mount_id, unsigned slabs_registered, unsigned slabs_unregistered, slot_class);
    size_t registered_bytes() const noexcept; size_t total_bytes() const noexcept;
    void release_for_maintenance();                         // requires quiescence; only free path
};

warmup_report device_arena::warm_up(const async_engine_config& c, reservation_manager& rm) {
    if (_warmed) return _report;                            // reuse across contexts
    size_t granted = rm.try_reserve_up_to(c.arena_bytes);   // non-blocking; retained for process lifetime
    size_t target  = round_down(granted, 16_MiB);
    for (size_t i = 0; i < target / 16_MiB; ++i) {
        void* p = nullptr;
        if (cudaMalloc(&p, 16_MiB) != cudaSuccess) { rm.release(granted - i * 16_MiB); break; }
        _slabs.push_back(slab{p, false, slot_class::mib1, no_mount, all_free, 0, false});
    }
    auto bar1 = query_free_bar1(_device);                   // optional
    size_t reg_target = bar1
        ? std::min({total_bytes(), size_t(c.bar1_fraction * double(sat_sub(*bar1, c.bar1_headroom))), pin_headroom()})
        : std::min(total_bytes(), pin_headroom());
    size_t first_step = bar1 ? reg_target : std::min(reg_target, c.registration_first_step);
    register_slabs_until(first_step);                       // stop at first failure; keep successes
    if (!bar1 && registered_bytes() == first_step)
        schedule_incremental_registration(reg_target);      // control executor; one slab at a time
    _warmed = true; return _report;
}

std::optional<staging_lease> device_arena::try_lease(mount_id m, size_t bytes, bool need_reg) noexcept {
    slab* best = nullptr;                                   // smallest class >= bytes among m's slabs
    for (auto& s : slabs_of(m))
        if (!s.draining && s.registered == need_reg && s.free.any() && class_bytes(s.cls) >= bytes)
            if (!best || class_bytes(s.cls) < class_bytes(best->cls)) best = &s;
    if (!best) return std::nullopt;
    return take_slot(*best);
}
```

Reclassification (`set_quota` with a new class): mark at most one slab per mount as `draining`. When its `live` reaches 0, reformat it and clear `draining`, then proceed to the next slab.

### 6.5 `operation.hpp`: stable state

```cpp
inline constexpr ssize_t pending_sentinel = std::numeric_limits<ssize_t>::min();

struct AsyncArgs { size_t size; off_t file_offset; off_t buffer_offset; ssize_t bytes_done = pending_sentinel; };

struct SubmissionFence {
    int device; EventLease event;                // recorded on the caller thread before publication
    std::shared_ptr<void> stream_owner;          // PTDS stand-in keep-alive
    bool ready_hint = false; time_point last_query;
};

enum class Phase { pending, input_wait, staging_copy, submission_queued, submitting, io_inflight,
                   retry_pending, host_io, copy_inflight, draining_error, quarantined, terminal };
enum class OpKind { read_staged, read_direct, write_body_staged, write_body_direct, write_edge };

struct Operation {                               // stable-address pool; never moved while live
    OpKind kind; Phase phase = Phase::pending;
    Credit credit;                               // exactly one terminal settlement
    std::shared_ptr<const io_object> file;       // keeps CUfileHandle_t registered
    std::shared_ptr<SubmissionFence> input;
    RequestState* request;                       // coordinator, priority, cancel flag, optional join
    mount_policy* mount;
    std::optional<registered_buffer_lease> direct;
    std::optional<staging_lease> staging;
    std::optional<scratch_lease> scratch;        // edges / buffered suffix
    std::optional<LaneLease> lane;
    bool holds_qd_token = false;

    AsyncArgs args;                              // pointers handed to cuFile
    byte_range physical;                         // granule-aligned window
    byte_range logical;  void* user_ptr;         // exact caller range and pointer
    uint64_t valid_end;                          // file offset up to which staging holds valid bytes
    uint32_t attempts = 0; uint64_t event_generation = 0; uint64_t write_seq = 0;
    bool cancelled = false;
};

struct RecordedEvent { Operation* op; cudaEvent_t event; uint64_t generation; Phase phase; };
```

In version 1 each operation has exactly one logical interval. Coalescing across slices is deferred (§9).

### 6.6 Submission: fences and `enqueue`

```cpp
// templated_ioctx<R>::dispatch_slices: caller thread
semi_future<size_t> dispatch_slices(Request req) {
    try {
        resolve_default_streams(req);                                    // existing
        if (runtime::instance().engine_possible() && req.has_device_slices()) {
            for (auto [device, stream, owner] : distinct_device_streams(req)) {
                scoped_device guard(device);
                auto fence = std::make_shared<SubmissionFence>(device, fence_events(device).acquire(), owner);
                CUDA_CHECK(cudaEventRecord(fence->event.get(), stream)); // before publication
                req.attach_fence(device, stream, std::move(fence));
            }
        }
        return partition_and_enqueue(std::move(req));                    // existing grouped coordinator
    } catch (...) { return make_exceptional_semi_future<size_t>(std::current_exception()); }
}

// cufile_reactor::enqueue: any thread
void cufile_reactor::enqueue(std::unique_ptr<grouped_io_request> r) noexcept {
    try {
        if (_closed) return r->finish_error(errc::operation_canceled);
        if (!_metadata_budget.try_acquire(r->slice_count())) return r->finish_error(errc::no_buffer_space);
        _requests.push(std::move(r));                                    // never waits for engine resources
    } catch (...) { settle_unpublished_credits_once(r, std::current_exception()); }
}
```

### 6.7 Facade routing in `dispatch_slice`

```cpp
void cufile_reactor::dispatch_slice(prepared_io_slice& s, request_ref req) {
    if (auto route = engine_route(s); route != route_kind::legacy) {
        device_engine::for_device(s.d_buffer.device_id).hand_off(
            engine_slice{s, req.take_credit_for(s), req.object(), s.fence(), req.priority(), this});
        ++_engine_outstanding;                       // decremented by the engine's retirement callback
        return;                                      // non-blocking; not charged to the per-pass byte budget
    }
    legacy_dispatch_slice(s, req);                   // park_if_stream_busy, read_cufile / posix / write: unchanged
}

route_kind cufile_reactor::engine_route(const prepared_io_slice& s) const noexcept {
    if (_cfg.engine.mode == engine_mode::legacy || !s.is_device_only() || !s.fence()) return route_kind::legacy;
    auto& file = static_cast<const cufile_io_object&>(*s.object);
    if (!file.cufile_registered() || !file.has_direct_fd()) return route_kind::legacy;
    auto* engine = device_engine::try_get(s.d_buffer.device_id);
    if (!engine || !engine->ready()) return route_kind::legacy;
    mount_policy& mp = file.policy(s.d_buffer.device_id);
    mp.ensure_probe(file.lease(), s.d_buffer.device_id);                 // non-blocking; no-op unless unknown
    return s.is_read() ? mp.read_route() : mp.write_route(s);            // atomic state read
}
```

### 6.8 `mount_policy`

```cpp
class mount_policy : public std::enable_shared_from_this<mount_policy> {
public:
    route_kind read_route() const noexcept;      // verified_direct / degraded_async / operator_qualified -> engine
    route_kind write_route(const prepared_io_slice&) const noexcept;
    void ensure_probe(file_lease, int device) noexcept;
    void observe_submit(const call_sample&) noexcept;        // reaper thread
    void observe_stuck_call(Operation&) noexcept;            // reaper watchdog
    profile base_profile() const;                // FS class + geometry (§4.6)
    effective_profile effective() const;         // set by the engine capacity fit
};

void mount_policy::ensure_probe(file_lease f, int dev) noexcept {
    state expected = state::unknown;
    if (!_state.compare_exchange_strong(expected, state::probing)) return;
    if (!probe_budget(dev).try_acquire()) { _state = state::unknown; return; }
    control_executor().submit([self = shared_from_this(), f, dev] {
        probe_result r = run_probe(*self, f, dev, /*registered=*/true);
        if (r.direct_evidence && r.median_submit <= self->_cfg.probe.submit_max) {
            self->seed_healthy_submit(r.submit_samples); self->publish(state::verified_direct);
        } else if (r.submit_blocked_for_io_duration || !runtime::instance().gds_available()) {
            self->publish(state::known_compat);
        } else if (r.registration_unavailable && self->_cfg.unregistered_device_tier) {
            probe_result u = run_probe(*self, f, dev, /*registered=*/false);
            self->publish(u.median_submit <= self->_cfg.probe.submit_max && u.throughput_acceptable
                              ? state::degraded_async : state::unknown);
        } else {
            self->publish(state::unknown);       // no telemetry / short file: stay on sync reads
        }
        probe_budget(dev).release();
    });
}

void mount_policy::observe_submit(const call_sample& s) noexcept {
    auto limit = std::max(_cfg.demotion.slow_floor, _cfg.demotion.p99_multiple * _healthy_p99.value());
    bool slow = s.elapsed > limit;
    _last8.push(slow);
    if (!slow) _healthy_p99.add(s.elapsed);      // rolling window of 1024 non-slow samples
    if (_last8.count_true() >= _cfg.demotion.slow_of_last_eight) open_circuit();
}

void mount_policy::open_circuit() noexcept {
    _state = state::circuit_open;                // facades now route new work to legacy
    _submitter->refuse_unstarted();              // unstarted commands return as `not_submitted`
    schedule_cooldown(next_cooldown());          // 30 s doubling, cap 16 min; then probing
}
```

`run_probe` implements §4.5.3. It never runs on an application thread or the reaper.

### 6.9 `device_engine`: lanes, capacity, loop, admission

```cpp
struct Lane { cudaStream_t stream; cudaEvent_t io_ev, copy_ev;       // timing disabled
              bool stream_registered; Operation* occupant; bool quarantined; };

unsigned lane_need(unsigned q) { return q + (q + 3) / 4; }

effective_profile fit(const profile& p, size_t bytes, unsigned lanes) {
    for (size_t s : {8_MiB, 4_MiB, 2_MiB, 1_MiB}) {
        if (s > p.preferred_op_bytes) continue;
        if (lane_need(p.qd) <= lanes && lane_need(p.qd) * s <= bytes) return {p.qd, s, lane_need(p.qd)};
    }
    unsigned q = p.qd;
    while (q && (lane_need(q) > lanes || lane_need(q) * 1_MiB > bytes)) --q;
    return q ? effective_profile{q, 1_MiB, lane_need(q)} : effective_profile::fallback();
}

// Weighted water-filling over active mounts (lanes, then slabs with the same routine).
void device_engine::rebalance() {
    auto fill = [](auto& mounts, unsigned total, auto demand, auto grant) {
        std::vector<mount_policy*> unsat(mounts.begin(), mounts.end());
        unsigned remaining = total;
        while (!unsat.empty() && remaining) {
            unsigned share = std::max(1u, remaining / unsigned(unsat.size()));
            for (auto it = unsat.begin(); it != unsat.end() && remaining;) {
                unsigned g = std::min({share, demand(*it) - grant(*it), remaining});
                grant(*it) += g; remaining -= g;
                it = (grant(*it) == demand(*it)) ? unsat.erase(it) : it + 1;
            }
        }
    };
    fill(_active, _cfg.device_lane_cap, demand_lanes, granted_lanes);       // demand = lane_need(Q_m)
    fill(_active, registered_slabs(),   demand_slabs, granted_slabs);       // demand = ceil(lanes*S_pref/16MiB)
    for (auto* m : _active) {
        auto eff = fit(m->base_profile(), granted_slabs(m) * 16_MiB, granted_lanes(m));
        m->set_effective(eff);
        _arena.set_quota(m->id(), granted_slabs(m), unregistered_grant(m), class_of(eff.op_bytes));
    }
}

void device_engine::run() {                            // scheduler/reaper thread
    set_current_device(_device); apply_affinity();
    while (!_stop || has_live_ops()) {
        drain(_control_ring);                          // hand-offs, policy and quota changes, cancellations
        drain(_completion_ring);                       // RecordedEvent, not_submitted, blocking/recovery results
        for (unsigned n = 0; n < _cfg.poll.due_events_per_round && _deadlines.due(now()); ++n)
            handle_due(_deadlines.pop());              // reap / fence hint / watchdog
        admit();
        wait_on_eventfd_until(_deadlines.empty() ? now() + _cfg.poll.idle_health : _deadlines.next_due());
    }
}

void device_engine::admit() {
    for (tier t : {tier::high, tier::low}) {
        while (mount_sched* m = _drr.next_mount_with_work(t)) {
            if (!m->lane_available()) { _drr.skip(m); continue; }
            if (t == tier::low && !low_allowed(*m)) { _drr.skip(m); continue; }
            Operation* op = m->next_piece(t);          // lazy expand_one(); null if the op pool is empty
            if (!op) { _drr.skip(m); continue; }
            if (!op->request->coordinator().should_continue()) { finish_quiescent(*op, errc::operation_canceled); continue; }
            if (!fence_admissible(*op)) { m->defer(op); _drr.skip(m); continue; }
            bool input_ready = op->input->ready_hint;
            if (input_ready && needs_token_now(*op) && !m->try_take_qd_token()) { m->defer(op); _drr.skip(m); continue; }
            if (!lease_resources(*op, *m)) { m->return_token_if(op); m->defer(op); _drr.skip(m); continue; }
            op->holds_qd_token = input_ready && needs_token_now(*op);
            start(*op, input_ready);                   // read: command to submitter; write: enqueue staging copy
            m->account_start(t);                       // bounded low service counters, DRR deficit
        }
    }
}

bool device_engine::low_allowed(const mount_sched& m) const {
    if (m.high_starts_since_low >= _cfg.low_service_interval && m.has_pending(tier::low)) return true;
    return m.free_lanes() > ceil_div(m.lanes(), 8u) || !m.has_pending(tier::high);
}

bool device_engine::fence_admissible(Operation& op) {
    auto& f = *op.input;
    if (!f.ready_hint && now() - f.last_query >= microseconds(_cfg.poll.fence_hint_us)) {
        f.ready_hint = (cudaEventQuery(f.event.get()) == cudaSuccess); f.last_query = now();
    }
    if (f.ready_hint) return true;
    return op.request->ops_in_input_wait == 0 && _input_waiters < _cfg.device_lane_cap / 8;
}

void device_engine::expand_one(SliceCursor& c, const effective_profile& p) {
    piece_plan pc = range_plan::next_cut(c, p.op_bytes, c.geometry(), granule(c.file()));
    Operation* op = _ops.allocate(pc);                 // allocate before transferring a credit
    op->credit = pc.last ? c.take_original_credit() : c.coordinator().add_one_credit();
    c.advance(pc); c.mount().pending(c.priority()).push(op);
}
```

`start()` for reads sends `{op}` to the mount submitter. For write bodies and edges the reaper itself enqueues `cudaStreamWaitEvent(lane, fence)`, the staging copy (D2D or D2H), and `cudaEventRecord`, then schedules a copy-event poll. If the hint was not ready at admission, the operation enters `input_wait` and takes its token when the hint turns ready (§4.8).

### 6.10 `async_transport`: mount submitter

```cpp
void mount_submitter::run() {
    apply_affinity();
    while (auto batch = _commands.wait_pop_up_to(8, _idle_retire)) {      // parks when empty; no fill-waiting
        for (Operation* op : *batch) {
            if (_refuse_unstarted.load()) { _completions.publish(not_submitted{op}); continue; }
            submit(*op);
        }
        _engine.notify_if_ring_was_empty();                               // coalesced wake-up
    }
}

void mount_submitter::submit(Operation& op) {
    scoped_device guard(op.lane->device());
    op.phase = Phase::submitting;
    try {
        if (op.kind == OpKind::read_staged || op.kind == OpKind::read_direct)
            CUDA_CHECK(cudaStreamWaitEvent(op.lane->stream, op.input->event.get(), 0));
        // write bodies: the fence wait and D2D staging were enqueued earlier by the reaper

        transfer_test_hook(op);                                           // "about to enqueue I/O"
        _call_start_ns.store(now_ns());                                   // watched by the reaper (2 ms)
        void* base = op.direct ? op.direct->base() : op.staging->slab_base();
        CUfileError_t rc = is_read(op)
            ? _api.async_read->read_async(op.file->cufile_handle(), base, &op.args.size,
                                          &op.args.file_offset, &op.args.buffer_offset,
                                          &op.args.bytes_done, op.lane->stream)
            : _api.async_write->write_async(op.file->cufile_handle(), base, &op.args.size,
                                            &op.args.file_offset, &op.args.buffer_offset,
                                            &op.args.bytes_done, op.lane->stream);
        auto elapsed = now_ns() - _call_start_ns.exchange(0);
        _completions.publish(call_sample{&op, elapsed});

        if (rc.err != CU_FILE_SUCCESS) return publish_recovery_required(op, rc);  // lane may hold partial work
        CUDA_CHECK(cudaEventRecord(op.lane->io_ev, op.lane->stream));
        _completions.publish(RecordedEvent{&op, op.lane->io_ev, ++op.event_generation, Phase::io_inflight});
    } catch (...) { publish_recovery_required(op, std::current_exception()); }
}
```

### 6.11 Reaping, validation, retry, retirement

```cpp
void device_engine::reap(RecordedEvent e) {
    Operation& op = *e.op;
    assert(e.generation == op.event_generation);
    cudaError_t st = cudaEventQuery(e.event);
    if (st == cudaErrorNotReady) return reschedule_with_backoff(e);
    if (st != cudaSuccess)       return move_to_recovery(op, st);

    switch (e.phase) {
    case Phase::staging_copy:                               // write: source -> slot (or scratch) done
        if (op.kind == OpKind::write_edge) return blocking_pool().pwrite_exact(op);     // -> host_io result
        if (!acquire_token_and_granules(op)) return park_until_token(op);
        return op.mount->submitter().send(op);              // cuFileWriteAsync

    case Phase::io_inflight: {
        release_qd_token(op);
        observe_completion_for_testing(op);
        outcome o = validate(op);
        if (o.kind == outcome::retry_aligned)  return resubmit_same_lease(op);
        if (o.kind == outcome::retry_buffered) return blocking_pool().buffered_suffix(op);
        if (o.kind == outcome::error)          return finish_quiescent(op, o.error);
        if (op.kind == OpKind::read_staged && !op.cancelled) {
            try {
                CUDA_CHECK(cudaMemcpyAsync(op.user_ptr,
                           static_cast<char*>(op.staging->data()) + (op.logical.begin - op.physical.begin),
                           op.logical.size(), cudaMemcpyDeviceToDevice, op.lane->stream));
                copy_event_record_failure_hook();
                CUDA_CHECK(cudaEventRecord(op.lane->copy_ev, op.lane->stream));
                schedule(RecordedEvent{&op, op.lane->copy_ev, ++op.event_generation, Phase::copy_inflight});
                if (op.request->join) op.request->join->attach_recorded_final_event(op);
            } catch (...) { move_to_recovery(op, std::current_exception()); }
            return;
        }
        return finish_quiescent(op, op.cancelled ? errc::operation_canceled : success);
    }
    case Phase::copy_inflight:
        return finish_quiescent(op, op.cancelled ? errc::operation_canceled : success);
    default: unreachable();
    }
}

outcome device_engine::validate(Operation& op) {
    ssize_t done = op.args.bytes_done;
    if (done == pending_sentinel) return outcome::fail(errc::io_error);   // completion without status
    if (done < 0)                 return classify_cufile_error(op, done); // may request per-file fallback
    uint64_t new_valid = op.args.file_offset + uint64_t(done);
    bool progressed = new_valid > op.valid_end;
    op.valid_end = std::max(op.valid_end, new_valid);

    if (is_read(op)) {
        if (op.valid_end >= op.logical.end) return outcome::ok();         // omitted tail is EOF padding
        if (file_shrunk_below(op, op.logical.end)) return outcome::fail(errc::file_shrunk);
        if (!progressed || ++op.attempts > max_short_reads) return outcome::fail(errc::short_io);
        uint64_t restart = align_down(op.valid_end, granule(op));         // overlap retry from aligned boundary
        op.args = {op.physical.end - restart, off_t(restart),
                   off_t(base_buffer_offset(op) + (restart - op.physical.begin)), pending_sentinel};
        return outcome::retry_aligned();
    }
    // write
    if (uint64_t(done) == op.args.size) return outcome::ok();
    if (done <= 0 || ++op.attempts > max_short_reads) return outcome::fail(errc::short_io);  // never blind replay
    uint64_t suffix = op.args.file_offset + uint64_t(done);
    if (suffix % granule(op) == 0) {
        op.args = {op.args.size - size_t(done), off_t(suffix), op.args.buffer_offset + done, pending_sentinel};
        return outcome::retry_aligned();
    }
    return outcome::retry_buffered();                                     // D2H remainder -> pwrite (buffered fd)
}

void device_engine::finish_quiescent(Operation& op, result r) {
    // Precondition: no enqueued accessor can still touch staging, destination, source, or args.
    if (r.ok() && is_write(op)) op.file->note_written(op.logical.end);
    if (is_write(op)) op.file->write_state().retire(op.write_seq, op.granules());
    release_scratch_slot_lane(op);                    // slot before lane; both reusable at once
    op.credit.finish_once(r);                         // may resolve the caller's future
    if (op.request->join) op.request->join->piece_retired(op);
    op.request->facade->engine_piece_retired();       // decrements _engine_outstanding
    _ops.retire(op);                                  // drops io_object, fence, args
}
```

- A `not_submitted{op}` publication releases the operation's lane, slot, and token. The piece then waits for its fence hint and goes to the blocking pool as a sync `cuFileRead` straight into the destination (the legacy code path). It keeps its credit.
- `base_buffer_offset(op)` is the slot's offset within the slab for staged operations, or the lease-relative offset for direct operations.

### 6.12 Two-phase event API (P5)

```cpp
struct device_join_token {
    void wait_on(cudaStream_t consumer) const;        // call on the consumer's thread
    bool host_complete() const noexcept;              // true => wait_on is a no-op
private: std::shared_ptr<EventGenerationLease> _ev;   // immutable while any token/join owns it
};
struct device_read_ticket { semi_future<device_join_token> enqueued; semi_future<size_t> completed; };

device_read_ticket ioctx::device_readv_async_io_ev(io_object&, std::span<device_read_slice>, cudaStream_t);

class RequestJoin {                                   // engine thread only
    cudaStream_t join_stream; cudaEvent_t request_ev; // from a bounded per-device pool (32)
    unsigned expected = 0, attached = 0; bool all_expanded = false, published = false;
public:
    void attach_recorded_final_event(Operation& op) { // only after op's final EventRecord succeeded
        CUDA_CHECK(cudaStreamWaitEvent(join_stream, final_event_of(op), 0));
        ++attached; maybe_publish();
    }
    void piece_served_by_blocking_transport_done() { ++attached; maybe_publish(); }
    void maybe_publish() {
        if (published || !all_expanded || attached != expected) return;
        CUDA_CHECK(cudaEventRecord(request_ev, join_stream));
        enqueued_promise.set_value(device_join_token{lease(request_ev)});   // only after the record succeeded
        published = true;
    }
};

void device_join_token::wait_on(cudaStream_t consumer) const {
    if (!_ev) return;                                 // host-complete token
    CUDA_CHECK(cudaStreamWaitEvent(consumer, _ev->get(), 0));
}
```

Rules:

- If no join stream is free at hand-off, or any slice of the request is legacy-routed, `enqueued` is fulfilled with a host-complete token when `completed` resolves.
- A failure before publication fails `enqueued`. `completed` always carries the authoritative result.
- A lane event stays leased, and is not re-recorded, until the join stream's wait on it has been enqueued. The wait is enqueued synchronously in `attach_recorded_final_event`, before `finish_quiescent` can run.
- For PTDS consumers, `wait_on(cudaStreamPerThread)` binds to the calling thread's PTDS.

### 6.13 `file_write_state`: ordering and flush

```cpp
class file_write_state {                               // shared by (st_dev, st_ino) across handles
    std::mutex _m; uint64_t _next_seq = 1; std::set<uint64_t> _outstanding;
    interval_map<granule_range, io_mode> _inflight;    // buffered vs direct
    std::multimap<uint64_t, unique_function<void()>> _flush_waiters;
public:
    uint64_t assign_sequence();                        // at hand-off; inserts into _outstanding
    bool try_acquire(granule_range r, io_mode m);      // false if an overlapping in-flight op has another mode
    void retire(uint64_t seq, granule_range r);        // erases; fires waiters whose captured seq < min(_outstanding)
    void on_writes_through(uint64_t seq, unique_function<void()> fn);
};

void device_engine::submit_flush(file_write_state& fs, FlushOp f) {
    uint64_t upto = fs.last_assigned_sequence();
    fs.on_writes_through(upto, [f = std::move(f)]() mutable {
        blocking_pool().submit([f = std::move(f)]() mutable { f.finish_once(fdatasync_result(f.file->buffered_fd())); });
    });
}
```

An operation that fails `try_acquire` stays in `staging_copy`-complete state without a token. It is retried when a conflicting interval retires. It never blocks.

### 6.14 `fallback_executor` and recovery

```cpp
namespace fallback {                                   // bodies moved verbatim from cufile_reactor.cpp
    bool   read_cufile_sync(const cufile_io_object&, byte_range, void* dst, const config&);
    result read_device_staged_posix(...);              // buffered fd, 2 x 8 MiB scratch, unchanged
    result write_device_staged(...);                   // D2H + pwrite, unchanged
    result write_host(...); result read_host_direct(...);
}

class blocking_pool {                                  // 4 threads, lazily started, engine-originated work only
public:
    void sync_read_piece(Operation&);                  // fence already observed complete by the engine
    void buffered_suffix(Operation&);                  // read: pread -> pinned scratch, engine then H2D into slot
                                                       // write: (D2H done by engine) -> pwrite on buffered fd
    void pwrite_exact(Operation&); void submit(unique_function<void()>);
};                                                     // every call publishes a result to the engine's completion ring

void device_engine::move_to_recovery(Operation& op, error e) {
    op.phase = Phase::draining_error; op.lane->quarantined = true;      // lane leaves service
    recovery_pool().submit([this, &op, e] {
        bool quiescent = cudaStreamSynchronize(op.lane->stream) == cudaSuccess;   // owned lane only
        _completion_ring.publish(recovery_result{&op, quiescent, e});
    });
}
// On recovery_result: quiescent -> finish_quiescent(op, e); request lane replacement on the control executor.
//                     not quiescent -> op.phase = quarantined; keep lane, slot, args, object, and credit; gauge++.
```

### 6.15 `topology`

```cpp
struct affinity_plan { std::optional<int> reaper_cpu, submitter_cpu; int host_numa = -1;
                       std::optional<std::string> nic_address; std::string reason; };

affinity_plan plan_affinity(int device, const mount_policy& m, const mount_override* ov) {
    if (ov && ov->has_affinity()) return from_override(*ov);
    auto gpu = gpu_pci_and_numa(device); auto cpus = permitted_cpus();
    if (!gpu || cpus.empty()) return {.reason = "GPU topology or cpuset unavailable"};
    auto nics = rdma_ports_reaching(m.storage_endpoints());
    std::ranges::sort(nics, [&](auto& a, auto& b) {
        return std::tuple(!a.vendor_eligible, a.numa != gpu->numa, pci_distance(a, *gpu), !a.link_up, a.pref)
             < std::tuple(!b.vendor_eligible, b.numa != gpu->numa, pci_distance(b, *gpu), !b.link_up, b.pref); });
    affinity_plan p;
    p.reaper_cpu = core_allocator().take_physical_core(gpu->numa, cpus);
    int sub_node = nics.empty() ? gpu->numa : nics.front().numa;
    p.submitter_cpu = core_allocator().take_physical_core(sub_node, cpus, /*not=*/p.reaper_cpu);
    p.host_numa = sub_node; if (!nics.empty()) p.nic_address = nics.front().address;
    return p;
}
```

### 6.16 `range_plan.hpp`

```cpp
namespace range_plan {
byte_range staged_physical_range(byte_range logical, size_t granule, uint64_t eof);   // align out, clamp to ceil(eof)
write_split split_write_range(byte_range, size_t granule);                            // head / body / tail (moved)
piece_plan  next_cut(const SliceCursor&, size_t op_bytes, const geometry&, size_t granule);
// next_cut: end = min(slice_end, next multiple of op_bytes above cursor, next geometry boundary above cursor);
//           logical = [cursor, end); physical = staged_physical_range(logical, granule, eof); last = (end == slice_end)
}
```

---

## 7. Edge cases and failure modes

| # | Situation | Handling |
|---|---|---|
| 1 | libcufile absent, or built without `CUCASCADE_HAS_CUFILE` | Existing POSIX path. The engine is never created. |
| 2 | libcufile lacks async or setter symbols | Only that group is disabled. Sync cuFile is still used. `engine_possible()` is false without async-read. |
| 3 | Compat mode (no nvidia-fs, T4 CI) | All mounts are `known_compat` with no probes. Fences are not recorded, and the blocking pool and submitters never start. Behaviour is identical to today. |
| 4 | Async call blocks, for example a mount that silently falls to compat | The probe sees call time close to I/O time and sets `known_compat`. At run time the slow-call or 2 ms watchdog opens the circuit. Unsubmitted pieces reroute to sync. The blocked submitter stays isolated and is never replaced. |
| 5 | Unrelated traffic inflates the nvidia-fs counters during a probe | Not accepted as evidence. The probe needs filtered statistics or an isolated interval. Otherwise the mount stays `unknown` on sync reads. |
| 6 | File shorter than one granule, or the probe is impossible | The mount stays `unknown`. Reads are correct through the sync path. |
| 7 | `cuFileBufRegister` fails for some or all slabs (small BAR1, pin limit) | Registered slabs are kept. The rest become unregistered tier. If that tier is not qualified, the mount uses sync cuFile or POSIX. No read fails. |
| 8 | Reservation manager grants less than 256 MiB, or nothing | The arena shrinks in 16 MiB steps and `fit()` lowers `S` and then `Q`. With zero arena the engine is disabled for that device and routing is legacy. |
| 9 | `cudaMalloc` fails during warm-up | The ungranted remainder is released and the engine continues with the slabs obtained. |
| 10 | Many mounts contend for the arena and lanes | Water-filling plus `fit()`. Mounts whose `Q_eff` is 0 use a fallback transport. Effective values are reported in gauges. |
| 11 | Reclassification while busy | One draining slab per mount. Old-class slabs keep serving. Pieces that find no slot stay pending. |
| 12 | Short read at EOF | Success if all logical bytes are covered. The aligned over-read past EOF is padding. |
| 13 | Short read mid-file | Aligned overlap retry on the same lane, slot, and credit. Progress is measured by `valid_end`. Zero progress or more than 64 attempts gives `short_io`. |
| 14 | File shrinks under a read | `fstat` shows the required range is beyond EOF. The result is `file_shrunk` and is never retried. |
| 15 | Sparse hole | The bytes are returned as zeros with a full count. This is valid data, not EOF. |
| 16 | O_DIRECT refused on a retry (`EINVAL`/`EOPNOTSUPP`) | Buffered `pread` suffix into pinned scratch, then H2D into the slot, then the normal D2D. |
| 17 | `bytes_done` still holds the sentinel when the event completes | Treated as `io_error`. The lane goes to recovery, since the stream state is suspect. |
| 18 | Async submit returns an error | The lane may hold a partial enqueue (the fence wait), so the operation goes to recovery, the lane is synchronized, and the error is classified. An unsupported-direct error demotes the file one rung and re-issues the piece through fallback after quiescence. |
| 19 | D2D enqueue or event record fails after a partial enqueue | The lane is quarantined from service and a recovery thread synchronizes only that lane. The operation retires with the error after quiescence, or is quarantined indefinitely. |
| 20 | Lane cannot be proven quiescent (stuck kernel or filesystem) | The operation, lane, slot, arguments, object, and credit stay quarantined. The future stays pending. A gauge and a log record it. Shutdown reports an incomplete drain. |
| 21 | Caller stream busy for a long time | Only one operation per such request waits on a lane, and at most one-eighth of the lanes wait device-wide. Other requests proceed. No host thread waits. |
| 22 | Null stream | The fence is on `cudaStreamLegacy`, recorded on the caller thread. Lanes are non-blocking, so there is no implicit barrier beyond the fence. |
| 23 | PTDS | The fence is on the stand-in stream. The owner is retained until the last operation retires. `wait_on` of the event API binds to the consumer thread's PTDS. |
| 24 | `io_object` dropped by the caller while operations are in flight | Each operation holds a `shared_ptr<const io_object>`. `cuFileHandleDeregister` runs only after the last retirement. |
| 25 | Request cancelled or failed mid-flight | Pending pieces finish with `cancelled`. Submitted I/O drains. Its D2D is skipped when `op.cancelled`. Credits settle once each. |
| 26 | Expansion allocation fails mid-slice | The cursor finishes its retained original credit with an error. Children already emitted drain. The coordinator reports the first error. |
| 27 | Operation pool or metadata budget exhausted | Expansion pauses, which is engine-side backpressure. `enqueue` returns `no_buffer_space` only on the slice-count budget. |
| 28 | Write short with a positive count | The suffix is retried with an aligned async write, or with buffered D2H plus `pwrite`. |
| 29 | Write error or timeout with unknown progress | No replay. `finish_error`. `note_written` is not called for that range. |
| 30 | Two operations touch the same granule with different modes | `file_write_state::try_acquire` serializes them. The later one waits without blocking. |
| 31 | Flush concurrent with later writes | The flush waits only for sequences at or below the captured one. Later writes are not awaited, though the filesystem may flush more. |
| 32 | `ENOSPC`, permissions, invalid arguments | Terminal. No retry. |
| 33 | `ESTALE`, path loss, server pause | Completed transient read errors retry with bounded exponential backoff and jitter (3 attempts, 10 ms base, 1 s cap **[belief]**). The pathname is never reopened. A stuck submission triggers demotion. |
| 34 | Submitter cap reached | Extra mounts stay on legacy routing. A gauge records it. |
| 35 | Facade shutdown with handed-off operations | Admission is closed, the facade's pending engine pieces are cancelled, and it waits for `_engine_outstanding == 0`. The arena and engine persist. |
| 36 | Process exit | Engine singletons are intentionally never destroyed. No `cudaFree` or `cuFileBufDeregister` is issued. |
| 37 | Driver already opened by another component | Pre-open parameters are reported as unapplied or unverified. Strict mode fails. Otherwise the engine continues with probes as the arbiter. |
| 38 | Different contexts request different process parameters | The first one wins. A diagnostic is issued, and strict mode raises an error. |
| 39 | GPU and NIC on different NUMA nodes, or topology unknown | The reaper is GPU-local and the submitter NIC-local, or affinity is left unset with a reported reason. |
| 40 | Event API with no join stream free, or legacy-routed slices | A host-complete token is published at completion. This is documented and never blocks. |
| 41 | QD overshoot from input-wait transitions | Bounded by the waiter cap. Admission is withheld until QD is back at or below `Q_eff`. The overshoot is reported. |
| 42 | Stream error on a registered lane | `cuFileStreamDeregister`, destroy the stream, and build a replacement lane on the control executor. |
| 43 | Multi-GPU request | Fences per device and one engine per device, under a shared coordinator. Arenas and policies are independent. |

---

## 8. Test plan

### 8.1 Catch2 tests

Tests marked **SIM** use `transport_simulator` and run on T4 CI without nvidia-fs. Tests marked **GDS** need a real GDS host.

| ID | Test | Verifies |
|---|---|---|
| T-01 | Delay the final D2D with the staging-copy delay seam. The future stays pending. After `.get()` the bytes are correct with no stream sync. (Re-expresses `test_cufile_reactor.cpp:660`.) **SIM** | FR-1 |
| T-02 | Device write: the source is mutated right after future resolution and the file content equals the pre-mutation data. `note_written` equals the logical end only on success. **SIM** | FR-2 |
| T-03 | `enqueue` under exhausted lanes and slots returns at once. Exceeding `max_pending_slices_per_device` yields `no_buffer_space`. An injected `bad_alloc` in enqueue settles credits once. **SIM** | FR-3 |
| T-04 | Producer kernel or `cudaMallocAsync` is queued on the caller stream and gated. Submit a read. Assert that the destination and slot are untouched until the gate opens. Do not assert the transfer hook waits. (Re-expresses `:594`.) **SIM** | FR-4 |
| T-05 | Null/legacy/PTDS matrix (`test_null_stream_ptds.cpp:142`, `test_cufile_reactor.cpp:1459-1495`, `test_cufile_write.cpp:584`) passes in both `legacy` and `async_simulated` modes. | FR-4, FR-37 |
| T-06 | The simulator records the thread ID of every async call: never an application thread, never the reaper. A debug-build interposer on the reaper asserts no blocking API is called. **SIM** | FR-5 |
| T-07 | Two mounts and two devices get distinct policy objects. State does not leak across the key. A runtime generation bump resets to `unknown`. | FR-6 |
| T-08 | With the mount `unknown`, reads use sync `cuFileRead` (transfer hook transport = sync) on at most `cufile_n_reactors` threads. | FR-7 |
| T-09 | Probe: evidence plus fast submit → `verified_direct`. Fast submit without evidence → `unknown`. A counter bump from unrelated traffic → not verified. Blocking submit → `known_compat`. Short file → `unknown`. Probe bytes are not counted toward the caller. The probe issues no write. **SIM** | FR-8 |
| T-10 | Inject 3 slow submissions of 8 → circuit opens. Inject one call stuck for more than 2 ms → circuit opens before the call returns. Unsubmitted pieces finish through sync with correct data. Submitted operations are not replayed. Cooldown leads to a re-probe. The other mount is unaffected. **SIM** | FR-9 |
| T-11 | Table-driven ladder: each row of §4.5.4 selects the expected transport. Compat reads use sync cuFile and compat writes use D2H + `pwrite` (`cufile_write_calls_for_testing == 0`). | FR-10 |
| T-12 | The loader, against stub libraries that lack each optional group in turn, disables only the corresponding capability. Lacking a required symbol means the library is absent. | FR-11 |
| T-13 | Mock API call log: setters come strictly before `driver_open`. `GetP2PFlags` is never called. An unsupported parameter produces a diagnostic. Strict mode throws. An externally opened driver is reported as unverified. | FR-12 |
| T-14 | Arena warm-up charges the reservation manager. Repeated context create/destroy leaves `arena_slab_counts` and `persistent_registered_bytes` constant with no free or deregister calls (mock counters). | FR-13, NFR-6, NFR-8 |
| T-15 | Registration: failure injected at slab k keeps slabs before k registered. Total failure plus a qualified unregistered tier keeps reads on async with `bounce_bytes > 0`. Total failure with an unqualified tier sends reads through sync successfully. A BAR1 mock checks the target formula. | FR-14 |
| T-16 | Size classes: every lease lies inside a single-class slab. cuFile receives slab base + offset (simulator assertion). Reclassification drains at most one slab per mount with no revocation. | FR-15 |
| T-17 | `fit()` reproduces the table in §4.7. Water-filling with 1, 2, and 5 mounts respects the 160-lane cap. `Q_eff = 0` gives the fallback route. | FR-16 |
| T-18 | A lane never has two occupants. Streams are non-blocking. Stream-register is called with `PAGE_ALIGNED_INPUTS` only. Copy residence holds no QD token (gauge). | FR-17 |
| T-19 | Direct lease: an aligned piece with a valid lease performs no D2D. A misaligned piece, or one with no lease, is staged. Pointer alignment alone never selects direct. | FR-18 |
| T-20 | Unaligned offset and length: the simulator sees a granule-aligned window, and the destination gets exactly `[off, off+n)`, checked with guard bytes. The D2D is enqueued only after the completion observer fires. | FR-19 |
| T-21 | Short reads: EOF padding → success. Mid-file short → overlap retry with the same lane and slot. Zero progress → error. Shrink → `file_shrunk`. Hole → zeros. `EINVAL` on retry → buffered suffix completes. The attempts cap is enforced. | FR-20 |
| T-22 | A single 1 GiB slice reaches a peak storage-QD gauge equal to `Q_eff`. Cuts fall on the `S_eff` grid and on injected stripe boundaries. No piece crosses a boundary. | FR-21 |
| T-23 | Write phases: body goes D2D → async write, edges go D2H → `pwrite`. A QD token is held only during I/O (gauge trace). Compat mount: no async write calls. | FR-22 |
| T-24 | Conflicting granules across two handles to the same inode are serialized. A positive short write retries its suffix, both aligned and buffered. An unknown-progress error causes no second write call. | FR-23 |
| T-25 | A flush issued between writes W1 and W2 runs `fdatasync` after W1 retires and does not wait for a gated W2. | FR-24 |
| T-26 | Event generations: the reaper never queries an unpublished generation (simulator delays the record). A lane event reused across ops never completes the wrong op. A slow lane does not delay retirement of ready lanes. | FR-25 |
| T-27 | Priority: high pieces preempt pending low pieces. Low gets one piece per four high starts (re-expresses `:1238-1264`). The demand reserve is honoured and borrowable. | FR-26 |
| T-28 | Liveness (re-expresses `:854`, `:892`, `:1086`): request A's fence is gated and floods slices. Ready request B completes. A occupies at most one lane. Device input waiters stay at or below cap/8. | FR-26 |
| T-29 | Fault matrix over every phase transition (submit error, copy enqueue failure, record failure, cancel at each phase): each credit settles exactly once. The `io_object` destructor runs only after the last retirement. No slot is released before its final event. | FR-27, FR-30, NFR-11 |
| T-30 | Thread census: 20 mounts → at most 8 submitters per device and 16 per process. Idle retirement after the configured interval. A stalled submitter causes no thread growth. Compat-only run: zero engine threads beyond the reaper. | FR-28, NFR-7 |
| T-31 | Affinity with a fake sysfs tree: same-NUMA NIC preferred. Split NUMA gives a GPU-local reaper and a NIC-local submitter. Missing topology gives unset affinity with a reason. Application thread affinity is unchanged. | FR-29 |
| T-32 | Recovery: `set_copy_event_record_failure_for_testing` leads to lane sync on the recovery thread and an error result with resources released only afterwards. Unprovable quiescence leads to quarantine with a pending future and intact memory. | FR-30 |
| T-33 | Shutdown: facade shutdown with pending and submitted operations cancels the pending ones, drains the submitted ones, and leaves the arena intact. A gated lane makes shutdown report an incomplete drain after the timeout. | FR-31 |
| T-34 | Event API: the token is published only after the record. `wait_on` followed by a consumer kernel sees complete data without a host wait. An I/O error surfaces through `completed`. With no join stream the token is host-complete. PTDS consumer on its own thread. **SIM** | FR-32 |
| T-35 | Seams: the transfer hook fires per attempt with context. The copy-delay seam does not block the reaper (another request completes in the meantime). The write-call counter counts async attempts. `live_registrations_for_testing` is unaffected by the arena. | FR-33 |
| T-36 | Gauges: each documented gauge is present and correct in a scripted scenario, and reported once with 4 facades. A mount without evidence is never labelled verified. | FR-34 |
| T-37 | `mode=legacy` gives a call trace identical to the pre-change reactor (golden trace on simulator-free compat). Each sub-flag disables its feature. A build without cuFile compiles and passes the POSIX tests. | FR-35, NFR-10 |
| T-38 | Two-device request: independent arenas and lanes, one future, correct data on both devices. | FR-36 |
| T-39 | Host-only, host+device, sub-threshold-on-compat, and registration-sharing tests (`test_cufile_write.cpp:827-1030`) pass unchanged on the legacy backend. | FR-37 |
| T-40 | Real-GDS smoke test per filesystem: probe → `verified_direct`, data integrity over 100 GiB of mixed reads and writes, fault drills (path loss, server pause). **GDS** | FR-8, FR-9, FR-19–23 |

### 8.2 Benchmarks and acceptance gates

| ID | Benchmark | Reports |
|---|---|---|
| B-0 | `benchmark_cufile_async_qd` (P0, standalone, no reactor): persistent registered buffers, one op per stream, QD 1–128, 1/2/4/8 MiB; GPFS first, then NVMe, WEKA, VAST | Throughput against QD; whether async gives parallel direct I/O; submit-call latency; `bytes_done` visibility |
| B-1 | Reaper-cost microbenchmark: `cudaEventQuery` ready and not-ready latency on idle and busy GPUs; record and D2D enqueue cost; polling 32/64/160 lane heads; validate-then-copy against pre-enqueued copy (full successful reads only) | µs/op by component, queries/op, timer overshoot, lane residence, QD loss at 10k ops/s and at saturation |
| B-2 | `gdsio` baselines per filesystem with matched sizes and threads, and direct-path evidence captured | Baseline GiB/s, read and write |
| B-3 | Sustained stream: tasks of 2 GiB as scattered 1–8 MiB chunks into `cudaMallocAsync`/RMM pool destinations, 1–16 submitting threads, one stream per task | GiB/s, time-weighted QD, refill gaps, slot waits |
| B-4 | DISK restore: column-size distributions (KiB to ≥ 1 GiB), 4 KiB-aligned layout, existing blocking caller | GiB/s; open and register cost separately |
| B-5 | Single 1 GiB buffer read | GiB/s, peak QD |
| B-6 | Spill writes: aligned bodies and unaligned edges; async against D2H + `pwrite` | GiB/s, bytes by transport |
| B-7 | Mixed priority (demand and readahead) with concurrent GPU kernels | High-tier latency percentiles, low-tier progress |
| B-8 | Multi-mount contention | Effective QD and op size per mount, fairness |
| B-9 | Transport comparison: registered staging, unregistered staging, sync cuFile, POSIX, forced compat | GiB/s each |
| B-10 | Compat regression on T4 and a non-GDS host, old against new, `mode=legacy` and `async_auto` | Δ % |

**Gates**

- **G-1:** B-0 shows async QD scaling on GPFS: throughput at QD 32 is at least 4 × QD 1, **[belief]** threshold. This is required before P2 starts. If it fails, revisit the transport choice.
- **G-2:** B-3, B-4, B-5, and B-6 each reach at least 80 % of the matched B-2 baseline on local NVMe, GPFS, WEKA, and VAST (NFR-1), with direct-path evidence recorded.
- **G-3:** No generation-wide refill gaps. Time-weighted QD is at least 90 % of `Q_eff` (NFR-5).
- **G-4:** B-10 is within 5 % (NFR-2). The compat default transport is unchanged.
- **G-5:** B-1 and B-3 meet NFR-3 and NFR-4, or the polling and lane parameters are returned and documented before the default flips.
- **G-6:** Every test in §8.1 passes, including the T-29 and T-32 fault matrices under a thread sanitizer where supported.
- **G-7:** Resource gauges are stable after warm-up across a 1-hour soak (NFR-6).
- **G-8:** The default changes to `async_auto` (P5) only after G-2 through G-7 pass on at least local NVMe and one network filesystem. Each other filesystem class stays behind a `mount_override` until it is qualified.

---

## 9. Open questions

Resolve these before or during the phase noted. Each records a **[belief]** together with its verification step.

1. **(P0, blocking)** Does `cuFileReadAsync` on N distinct non-blocking streams give N parallel direct I/Os on each target filesystem, or does libcufile serialize internally through `max_direct_io_threads` or per-handle locks? Verify with B-0.
2. **(P0)** Is `bytes_done` guaranteed to be written before an event recorded after the async call on the same stream completes? What is the error encoding (negative errno or cuFile code)? Verify in B-0 with fault injection, and consult the installed `cufile.h`.
3. **(P0)** Does passing a registered slab base plus `buf_offset` keep the fast path for async calls? Is 16 MiB a good registration granularity (BAR1 mapping granularity, registration-count limits)?
4. **(P0/P1)** Which direct-path evidence source is attributable per process or per device on the target cuFile versions: `cuFileStats*`, `/proc/driver/nvidia-fs/stats`, or per-GPU counters? If none is, is an isolated qualification interval acceptable to operators?
5. **(P0)** For unregistered buffers under async, does cuFile bounce through GPU-resident slabs or host-pinned buffers (Appendix C §6.4)? This determines whether the unregistered-device tier is worth keeping.
6. **(P0)** Are the thresholds right: 100 µs slow floor, 4 × p99, 3 of 8, 2 ms stuck, 30 s cooldown, 100 µs `probe.submit_max`? Calibrate from the measured healthy submit distributions per filesystem.
7. **(P0)** `cuFileStreamRegister`: what is the cost and benefit, is registering up to 160 streams acceptable, and what is the exact contract for deregistering on stream error?
8. **(P1)** Exact setter prototypes, parameter IDs, and units in each supported libcufile version. Is there a reliable way to detect that the driver was already open before our setters ran?
9. **(P1)** Does `fstatfs().f_bsize` equal the GPFS filesystem block size on the supported Storage Scale releases? Check against `mmlsfs -B`.
10. **(P1)** Which Lustre and BeeGFS layout APIs may be linked or `dlopen`ed (`llapi`, ioctl), and what is the fallback op size when they are unavailable? The spec uses 1 MiB and 2 MiB.
11. **(P2)** Reservation-manager integration: which API reserves device memory for a non-RMM `cudaMalloc`? How does a process-lifetime reservation interact with the reservation manager's own lifetime and with multiple managers per device?
12. **(P2)** BAR1 telemetry source (NVML BAR1 info), its availability in containers, and whether the 50 % fraction and 64 MiB headroom are safe when several processes share a GPU.
13. **(P2)** Do 160 lanes per device scale in CUDA and libcufile (per-stream state, scheduler overhead)? Should the cap be lower on small GPUs?
14. **(P3)** Routing point: the spec hands off in `dispatch_slice` on the facade worker. If B-3 shows hand-off latency or head-of-line blocking behind a legacy transfer, move routing into `enqueue`. This needs a non-throwing split of mixed requests.
15. **(P3)** Tiny-slice efficiency for DISK restores: should adjacent slices of one request be coalesced into one physical read with several D2D copies? This needs multi-credit operations. It is deferred, and the decision should follow B-4.
16. **(P3)** D2D copy contention with concurrent kernels. Does `cudaMemcpyBatchAsync` help for multi-destination copies? Should the copy stay on the lane, as specified, or move to a dedicated copy stream?
17. **(P3)** Is the predictive first-poll fraction (0.5 × p50) and the 100 µs cap the right trade between queries per op and residence? Decide from B-1.
18. **(P4)** Per-filesystem async **write** support through GDS on GPFS, WEKA, and VAST for each client version. Which classes get the write-eligible profile flag, and which write-evidence counter is used?
19. **(P4)** Do the cross-NUMA placement rule (reaper GPU-local, submitter NIC-local) and `rdma_dev_addr_list` selection actually steer traffic on GPFS, WEKA, and VAST? Measure with `mmdiag --rdma` and NIC counters.
20. **(P4)** VAST multipath and `nconnect`, and WEKA client queue limits: do they call for QD targets other than 64?
21. **(P5)** Event API details: final naming, a write-side equivalent, join-stream pool size (32), and whether a host-complete token on capacity exhaustion is preferable to an exceptional `enqueued` for the first adopters.
22. **(P5)** Is PTDS stand-in destruction safe while lane waits on its fence event are still outstanding (Appendix C §5(g)), across the supported CUDA versions?
