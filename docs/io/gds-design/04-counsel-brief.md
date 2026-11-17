# Counsel brief: a GDS/cuFile read (and write) reactor for sustained, high-queue-depth GPU I/O on local NVMe and network-attached parallel filesystems

You are one of three expert planners. You only have this document; there is no
repository access. Everything you need is here and in the appendices:

- **A**: cuFile/GDS patterns and lessons (`01`)
- **B**: how the existing io_uring reactor stages through pinned host memory (`02`)
- **C**: a line-by-line gap analysis of the existing cuFile reactor (`03`)

Cite appendix sections when you rely on them.

## Expert lenses

The three experts are named for the lens each must emphasize. Every plan must
still be complete and end-to-end.

- **`gds-io-expert`**: the cuFile/nvidia-fs internals. Covers the async versus
  batch versus sync API, registration and BAR1, bounce buffers, stream
  semantics, compat mode, CUDA stream/event correctness, and the reactor
  concurrency model.
- **`spectrum-scale-expert`**: IBM Storage Scale (GPFS) with GDS over RDMA.
  Covers NSD server striping, block size, the client page pool and prefetch,
  `mmdiag`, RDMA NIC/GPU NUMA affinity, `cufile.json` RDMA keys, and how QD
  maps to NSD parallelism.
- **`weka-vast-expert`**: other network-attached GDS filesystems: WEKA
  (wekafs, kernel client with GDS), VAST (NFS over RDMA, multipath, nconnect),
  and Lustre/BeeGFS. Covers per-filesystem QD and I/O-size sweet spots,
  multipath/NIC selection, and failure and timeout behaviour.

## 1. Goal

Design the next-generation **cuFile reactor** for cuCascade, a C++20/CUDA
library that tiers GPU data across GPU/host/disk. The reactor must reach
**≥ 80 % of raw hardware throughput** (gdsio baselines on a real GDS stack) for
the read patterns below. It must also not regress on hosts without GDS (compat
mode, CI on T4), and must keep the library's existing contracts unless the plan
explicitly argues for changing one and costs the migration.

## 2. Read patterns to optimise (primary)

1. **Sustained stream**:
   - Long, open-ended sequences of read tasks. Each task reads ~2 GiB as many
     scattered **1–8 MiB** chunks of a file into **stream-ordered pool
     allocations** (`cudaMallocAsync`/RMM pool). Destinations are not
     registered with cuFile and must not be assumed registrable.
   - The caller hands over **one CUDA stream** per task.
   - Many tasks may be in flight from several threads. The reactor must keep a
     **deep, sliding** queue rather than generations with gaps between them.
2. **DISK-tier restore** (existing consumer):
   - One vectored request per spilled batch file: one slice per column buffer,
     sizes from a few KiB to ≥ 1 GiB, file layout 4 KiB-aligned.
   - The destinations are pool allocations on the caller's stream.
   - The caller currently blocks on the returned future, then uses the data on
     the same stream with no `stream.sync()` (see the contract in Appendix C §2.7).
3. **Storage targets**:
   - local NVMe (ext4/xfs)
   - **IBM Storage Scale (GPFS) with GDS**
   - **WEKA**
   - **VAST**

   Also design so that Lustre/BeeGFS/NFS-RDMA fit. Network targets are
   RTT-bound and need **QD 32–64+** in flight per client [belief, Appendix A §7].
4. **Writes** (secondary but required): GPU→disk spill of large device buffers.
   The existing head/body/tail granule split is described in Appendix B §4 and
   Appendix C §3.

## 3. Current state (summary, details in Appendix C)

- The cuFile reactor is **host-synchronous**: 4 worker threads each issue a
  blocking `cuFileRead`/`cuFileWrite` of ≤ 64 MiB. QD is ≤ 4 per process.
- A single slice maps to a single reactor, with no striping.
- Async, batch, and stream-register APIs are not even loaded.
- Destinations are unregistered (cuFile bounces internally). There is no
  registered staging ring.
- The caller stream is a host-polled gate (200 µs), and futures resolve
  host-complete.
- All defaults were tuned in **compat mode**. No real-GDS baseline exists.
- The io_uring reactor (Appendix B) already has a good shape, just with host
  staging:
  - slot pool with leases
  - read-around
  - dynamic op size 256 KiB–16 MiB
  - priority tiers with preemption and bounded low service
  - event-polled device copies on the caller stream
  - exact-once coordinator credits

## 4. Hard constraints

- **Contracts**:
  - `ioctx::device_readv_async_io` returns `semi_future<size_t>`; today, "on
    success the bytes are in place, no further synchronization of stream is
    needed".
  - Writes: the source must stay valid until the future resolves.
    `enqueue` is `noexcept`.
  - Any API is callable from any thread concurrently.
  - Null and per-thread default streams are resolved on the submitting thread
    *before* a reactor sees them (null → `cudaStreamLegacy`, PTDS → stand-in).

  You may propose a contract extension, such as a stream-ordered or
  event-returning variant, but the default path must stay compatible, or you
  must list every caller to migrate.
- **Registration**: we cannot assume RMM/pool memory is registered. We may
  register long-lived reactor-owned memory. Per-transfer registration of pool
  sub-ranges is unverified, and its interior-page semantics are unknown.
- **Memory**: the library budgets memory through reservation managers. A large
  `cudaMalloc` ring outside RMM must be justified and sized.
  - BAR1 can be small (T4: about 256 MiB [belief]).
  - Registration failure must **degrade**, never fail reads.
  - `cudaFree`, `cudaFreeHost`, and `cuFileBufDeregister` at teardown can
    stall device-wide. The existing code keeps scratch in a
    never-destroyed pool.
- **Compat mode**: `cuFileReadAsync` runs synchronously in the submitting
  thread in compat mode. Submission must never happen on a user thread.
- **libcufile versions**: new symbols (async, batch, StreamRegister,
  SetParameter*) must be **optional**. Today a missing symbol disables cuFile
  entirely.
  - `cuFileSetParameter*` must precede driver open.
  - The probe avoids `cuFileDriverGetP2PFlags`, which resets parameters in
    libcufile 1.19.
- **Correctness invariants**:
  - Never release staging, or deregister memory, while DMA or a copy may still
    touch it.
  - Settle exactly one coordinator credit per op.
  - Detect short reads.
  - Keep the `io_object` (and the `CUfileHandle_t` it deregisters on
    destruction) alive until the stream passes the op.
  - Multi-GPU: rings and stream pools are per device.
- **Engineering**:
  - C++20, RAII for every handle.
  - Tests with Catch2; existing test seams must get equivalents.
  - The `CUCASCADE_HAS_CUFILE` build gate stays.
  - Clean fallback to the existing POSIX path.

## 5. Questions every plan must answer

1. **Transport per storage class**: async plus an internal stream pool,
   batch-handle pool with cookie reaping, sync on N threads, or a hybrid?
   Choose per filesystem class (local NVMe, GPFS, WEKA, VAST, compat) and
   justify it. Give default QD, I/O size, and stream or handle counts per
   class, and say how they are tuned (static table, probe, or adaptive).
2. **Staging**:
   - Registered device ring: size, slot size, per device, BAR1-aware sizing,
     placement within the memory budget, fallback when registration fails.
   - When to read **directly** into the destination instead (large, aligned,
     registrable, or reactor-owned memory).
   - Read-around and alignment policy.
   - How slots are leased with backpressure, without blocking `enqueue` or
     user threads.
3. **Completion and stream semantics**:
   - Keep host-complete futures, add a stream-ordered/event-returning API, or
     both?
   - Solve the **cross-thread join ordering** problem: a
     `cudaStreamWaitEvent` on the user stream is a no-op if the reactor has
     not recorded the event yet, and null/PTDS streams are meaningful only on
     the submitting thread.
   - Specify the input dependency: how I/O streams wait for the user stream
     (an event recorded at submit on the caller thread, then a GPU-side wait).
4. **Reaper / completion detection**:
   - Which signal: event polling, `cudaLaunchHostFunc`, a batch `GetStatus`
     thread, or other?
   - Which thread reaps; how it checks `bytes_done`, retries short reads,
     settles credits, releases slots, and keeps object lifetime.
   - What it costs per op at 1 MiB × 10k ops/s.
5. **Striping and scheduling**:
   - Split large slices across slots and streams (QD for a single 1 GiB
     buffer).
   - Carry over priority tiers (demand versus readahead), preemption, and
     bounded low service when ops are already enqueued on streams.
6. **Network-FS specifics**:
   - NIC/GPU/NUMA affinity.
   - `cufile.json` keys and `cuFileSetParameter*`, and their ordering with
     driver open.
   - Per-mount QD.
   - Timeouts, errors, and retries.
   - How to verify that real GDS (not compat) is active per mount.
7. **Writes**: transport, staging (D2D into the registered ring, then
   `cuFileWriteAsync` O_DIRECT), head/tail handling, flush, and the
   compat-mode path (staged D2H plus `pwrite` measured faster in compat).
8. **Fallbacks and compat**: decision ladder per file and per mount (not the
   process-wide `gds_available()`), CI on T4 without nvidia-fs, and
   libcufile without async symbols.
9. **Shutdown / cancellation**: draining stream-enqueued ops that cannot be
   cancelled, and process-lifetime resources.
10. **Validation plan**:
    - Real-GDS baselines (gdsio per filesystem).
    - Microbenchmarks for each pattern in §2.
    - Gauges and counters (QD, slot waits, bounce bytes, compat fallbacks).
    - Acceptance gates.
11. **Phasing**: an incremental delivery plan where each phase is shippable,
    measurable, and reversible behind config.

## 6. Output expected from each expert

A concrete plan with:
- requirements
- architecture (components, threads, streams, memory)
- per-filesystem parameter table
- data-path sequence diagrams for read and write
- the completion/join protocol
- failure handling
- API and config changes
- file-level change list (new and modified files under `src/io/cufile/`,
  `include/cucascade/io/cufile/`, `include/cucascade/io/`, tests, benchmarks)
- test plan
- benchmark plan with gates
- phases
- risks and open questions

Mark anything you believe but cannot prove as **[belief]**, and say how to
verify it.
