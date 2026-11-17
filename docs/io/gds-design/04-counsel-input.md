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


---

# Appendix A: 01-gds-workflow-patterns.md


Audience: agents and engineers designing or changing the cuFile (GDS) and io_uring
disk/network-storage reactors in cuCascade. This note distills a design
discussion grounded in the NVIDIA MagnumIO GDS samples. Statements are tagged:

- **[sample]**: shown directly by a MagnumIO sample (path given).
- **[header]**: from `cufile.h` (shipped with CUDA; a copy lives in any pixi env
  under `targets/x86_64-linux/include/cufile.h`).
- **[belief]**: engineering knowledge or vendor documentation that we did not
  verify in this repo. Verify on the target system before relying on it.

Sample root: `MagnumIO/gds/samples/` (`3_cuFile_Batch/`, `4_cuFile_Async/`,
`5_cuFile_MemMap/`).

---

## 1. The two cuFile submission models

| Aspect | Async API (`cuFileReadAsync` / `cuFileWriteAsync`) | Batch API (`cuFileBatchIO*`) |
|---|---|---|
| Submit | One call per I/O, takes a `cudaStream_t` | `cuFileBatchIOSubmit(handle, n, params[], flags)` submits a vector |
| Ordering | **Stream-ordered**: runs after earlier work on the stream; later work waits for it | **Not stream-ordered**: no stream argument; races with all CUDA streams |
| Completion | `cudaStreamSynchronize`, or an event recorded on the stream | CPU-side `cuFileBatchIOGetStatus` (poll, or block with a timeout) |
| Args lifetime | `size`, `file_offset`, `buf_offset`, `bytes_done` are passed **by pointer** and read/written asynchronously. They must stay alive until the stream passes the I/O | `CUfileIOParams_t[]` is consumed at submit [belief]. Results come back as `CUfileIOEvents_t` |
| Composes with kernels, `cudaMallocAsync`, `cudaFreeAsync` | Natively, through the stream | Only through explicit CPU barriers |
| Best for | Sustained, open-ended streams of I/O pipelined into GPU work | Discrete bulk fan-out with a clear submit, wait, consume phase |

[sample] `4_cuFile_Async/async_rw_default.cc` (stream 0), `async_rw_custom.cc`
(non-blocking custom stream), `async_rw_batch_custom.cc` (N async I/Os on N
streams, which is **not** the Batch API), `3_cuFile_Batch/batch_read.cc`
(setup → submit → poll → destroy).

### 1.1 Batch API signatures [header]

```c
CUfileError_t cuFileBatchIOSetUp(CUfileBatchHandle_t *batch_idp, unsigned nr);      // nr = capacity
CUfileError_t cuFileBatchIOSubmit(CUfileBatchHandle_t, unsigned nr, CUfileIOParams_t *iocbp, unsigned flags);
CUfileError_t cuFileBatchIOGetStatus(CUfileBatchHandle_t, unsigned min_nr, unsigned *nr,
                                     CUfileIOEvents_t *iocbp, struct timespec *timeout);
CUfileError_t cuFileBatchIOCancel(CUfileBatchHandle_t);
void          cuFileBatchIODestroy(CUfileBatchHandle_t);

typedef struct CUfileIOParams {
  CUfileBatchMode_t mode;            // CUFILE_BATCH, must be first
  union { struct { void *devPtr_base; off_t file_offset; off_t devPtr_offset; size_t size; } batch; } u;
  CUfileHandle_t fh; CUfileOpcode_t opcode; void *cookie;
} CUfileIOParams_t;
typedef struct CUfileIOEvents { void *cookie; CUfileStatus_t status; size_t ret; } CUfileIOEvents_t;
```

Gotchas:
- The second argument of `GetStatus` is **`min_nr`**, the minimum number of
  completions to wait for. `nr` is in/out: max events in, events returned out.
  `batch_read.cc` passes `batch_size` as `min_nr`, which waits for all.
  `batch_get_status.cc` passes `min_nr = 0`, which returns whatever is done so far.
- **Completions are not returned in submit order.** Identify each one by
  `CUfileIOEvents_t.cookie`, which echoes `CUfileIOParams_t.cookie`. The
  samples index `params[i]` by event position, which is fine only for their
  verification.
- `devPtr_base` may be device **or host** memory [header comment]. One batch
  may mix memory kinds [sample `batch_various.cc`: cudaMalloc, cudaMallocHost,
  malloc, mmap].
- Per-batch capacity is limited by `cufile.json` `properties.io_batchsize`
  (default 128) [belief]. The samples cap at 128.
- **Topping up a live handle**: the header and samples never submit more
  entries to a handle that still has I/Os in flight. Treat a handle as one
  generation: submit, reap everything, then reuse it. Check the API reference
  for your cuFile version before relying on anything else.

### 1.2 Async API extras [header]

```c
cuFileReadAsync (CUfileHandle_t, void *buf, size_t *size_p, off_t *file_off_p, off_t *buf_off_p, ssize_t *bytes_read_p, CUstream);
cuFileWriteAsync(CUfileHandle_t, void *buf, size_t *size_p, off_t *file_off_p, off_t *buf_off_p, ssize_t *bytes_written_p, CUstream);
cuFileStreamRegister(CUstream, unsigned flags);   // cuFileStreamDeregister to undo
// flags: CU_FILE_STREAM_FIXED_BUF_OFFSET, CU_FILE_STREAM_FIXED_FILE_OFFSET,
//        CU_FILE_STREAM_FIXED_FILE_SIZE, CU_FILE_STREAM_PAGE_ALIGNED_INPUTS
```

`cuFileStreamRegister` tells cuFile that the size and offsets are known at
submit time and 4K-aligned, so it can avoid deferred work on every request
[belief: performance effect]. This fits a staging ring, where every
offset and size is aligned by construction.

---

## 2. Data path versus control path (two independent axes)

```
                     data path ->  fast (GDS P2P DMA into HBM)  |  slow (bounce / compat)
control path
  async (stream-ordered)                    ok                  |        ok
  batch (CPU-polled)                        ok                  |        ok
```

- "Not stream-ordered" describes **synchronization only**. The batch API
  reaches the GDS fast path just like async does. It is the canonical way to
  drive high queue depth (QD) with one syscall [belief].
- The fast path requires all of the following:
  - a registered device buffer
  - block-aligned file offset and size
  - a file opened with `O_DIRECT`
  - a GDS-capable filesystem/mount
  - enough BAR1 space
  - `CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WITH_CUDA_VMM_SUPPORTED`, if the
    memory is VMM-backed (see `5_cuFile_MemMap/`, `common/cufile_sample_memmap.cc`)

## 3. What `cuFileBufRegister` does (it is not a bounce buffer)

[belief, the standard GDS model] Registration on device memory:
1. pins the GPU pages
2. maps them through BAR1 so the NVMe controller or NIC can DMA peer-to-peer
3. pre-builds the DMA scatter-gather list (in nvidia-fs)
4. sets up IOMMU mappings when needed

BAR1 is a finite resource (see `nvidia-smi -q -d MEMORY`).

The **bounce buffer** is a separate pool of *pinned GPU memory* that cuFile owns
internally (sized with `cuFileSetParameterGpuBounceBufferSlabArray`, cuFile
1.17+). [header] The `cuFileRead` note says unregistered buffers are "buffered
through preallocated pinned buffers". Here "pinned" means device memory: see
`CU_FILE_GPU_MEMORY_PINNING_FAILED` ("failed to allocate pinned GPU Memory"),
`max_device_pinned_mem_size`, and `CUFILE_PARAM_GPU_BOUNCE_BUFFER_SLAB_{SIZE_KB,COUNT}`.
Total registration is capped by `cuFileDriverSetMaxPinnedMemSize`. In compat
mode (no nvidia-fs) cuFile instead goes through POSIX plus host copies. cuFile uses
it when the direct path is impossible: unregistered buffer, unaligned edges,
no BAR1, non-GDS filesystem, or VMM memory without RDMA support. In that case
the data goes into the bounce buffer and is then copied device-to-device (D2D)
into the user buffer.

Registration is optional [sample comment: "For unregistered buffers, this call
is not required"]. Without it cuFile either registers on the fly (expensive)
or bounces.

Host memory: registering pinned host memory uses no BAR1. On local NVMe it
is close to a no-op. On RDMA-backed storage (NVMe-oF, Spectrum Scale, WEKA,
VAST over RDMA) it lets cuFile reuse the NIC memory registration [belief].
Pinned, page-aligned host memory with `O_DIRECT` already gets zero-copy DMA
from the kernel. That is the ordinary Linux DIO path, not GDS.

## 4. Alignment

- `O_DIRECT` normally requires the buffer address, file offset, and size to be
  block-aligned.
- `cudaMalloc` and `cudaMallocAsync` pointers satisfy the buffer-address part
  (≥256 B or 512 B guaranteed; pool chunks are usually 2 MiB-granular)
  [belief].
- cuFile accepts an unaligned size or offset and handles the edges internally
  through bounce or read-modify-write [sample `batch_write_unaligned.cc`, which
  writes 6000-byte entries with `O_DIRECT`]. The cost is that those edges fall
  off the fast path.
- **Read-around**: read the enclosing aligned window into staging, then
  `cudaMemcpyAsync` only the exact bytes out. A staging buffer makes unaligned
  user requests cheap.

## 5. Stream-ordered pool memory (`cudaMallocAsync` / mempools)

- The physical backing of a pool allocation can be recycled across
  allocations. Registering a pool allocation and then letting the pool reuse
  its pages leaves a **stale registration**, which is a bug.
- Options:
  - (A) Register per allocation: wait until the allocation is ready, register,
    deregister before `cudaFreeAsync`. This is costly per allocation.
  - (B) Pre-register a reserved VA range (`cuMemAddressReserve` plus your own
    mapping, see `5_cuFile_MemMap/memmap_preregister.cc`).
  - (C) Don't register; let cuFile bounce or register on the fly.
  - (D) **Preferred**: a staging ring, see §6.
- Async cuFile composes with `cudaMallocAsync` and `cudaFreeAsync` on the same
  stream. **The Batch API does not**: you must synchronize (event or stream)
  before submit and before the free.

## 6. Pattern: self-managed registered staging ring (preferred)

```
startup:  cudaMalloc(staging, 2 GiB); cuFileBufRegister(staging, 2 GiB)   # once, for process lifetime
          slots = 2 GiB / SLOT (SLOT = max request, e.g. 8 MiB -> 256 slots)
          io_streams[M] (M = 16..32), slot k bound to io_streams[k % M]
          per-slot persistent cuFile arg block {size, file_off, buf_off, bytes_done}
          optional: cuFileStreamRegister(io_streams[i], PAGE_ALIGNED_INPUTS | ...)

read(file_off, n, dst, user_stream):
  k = next slot; s = io_streams[k % M]
  wait for slot_free[k] (host; returns immediately in steady state, otherwise backpressure)
  aligned window: a_off = floor(file_off), a_len = ceil(head + n)
  cuFileReadAsync(fh, staging + k*SLOT, &a_len, &a_off, &zero, &done[k], s)
  cudaMemcpyAsync(dst, staging + k*SLOT + head, n, D2D, s)
  record ev on s; cudaStreamWaitEvent(user_stream, ev)      # GPU-side join, no CPU poll
  record slot_free[k] on s
```

Why it wins:
- Every I/O is on the fast path (registered, aligned).
- BAR1 use is bounded.
- Destinations can be unregistered pool memory with any alignment.
- One read can scatter into many destinations.
- Slot reuse is free: the same slot on the same stream gives an implicit chain.

When to skip it: destinations that are large, long-lived, and stable, such as
one-shot weight loading. Register those and read directly.

The cost is one extra D2D copy (HBM at about 3 TB/s versus storage at tens of
GB/s, usually negligible). That copy competes with kernels for copy engines
and HBM bandwidth.

## 7. Queue depth is the main throughput lever

- One stream means QD = 1, a small fraction of NVMe or network bandwidth.
  Network-attached parallel filesystems (Spectrum Scale/GPFS, WEKA, VAST,
  Lustre) need **QD 32–64+** to cover per-request RTT and to spread load
  across storage servers [belief].
- **Internal stream pool pattern**: the user hands over one `user_stream`.
  1. The reactor fans the work out across M internal I/O streams.
  2. Each internal stream runs its read and the D2D copy.
  3. The reactor records an event per stream.
  4. `user_stream` waits on those events.

  This keeps the single-stream API while delivering high QD.
- Also tune `cufile.json`:
  - `properties.max_direct_io_threads` (worker pool)
  - `rdma_dev_addr_list` (pin to the right NICs)
  - `rdma_load_balancing_policy` (NUMA affinity)

  [belief: key names vary by release, so check your `cufile.json`.]
- Measure:
  - GPFS: `mmdiag --iohist`
  - PCIe: `nvidia-smi dmon -s put`
  - NIC: `perfquery`
  - backpressure: how often and how long the reactor blocks waiting on a slot

## 8. Batch API for sustained streams: handle pool, sliding window

A single handle has a gap between generations: the next submit waits for the
whole drain. For an open-ended stream, use a pool of K handles of capacity B
each (for example K = 4–8, B = 32), which allows up to K·B I/Os in flight:

```
submitter: take a free handle (wait if none) -> fill <= B params (cookie = task) -> Submit
reaper thread: for each in-flight handle: GetStatus(h, min_nr=0 or 1, &nr, ev, &timeout)
   per event: check status and ret -> D2D copy staging->dst on copy_stream[slot % M]
              -> record slot_free[slot] (slot recycles PER ENTRY)
              -> cudaStreamWaitEvent(user_stream, ...)
   outstanding[h] -= nr; when 0 the handle returns to the pool (handle recycles PER GENERATION)
```

QD stays between (K−1)·B and K·B. Use a real `timespec` timeout so the reaper
blocks instead of spinning.

**Conclusion of the discussion**: for a long, sustained stream of 1–8 MiB
requests, **async plus an internal stream pool plus a registered staging ring**
is simpler and keeps per-request QD sliding. Move to batch micro-batching only
if profiling shows submit cost as the bottleneck. At 1–8 MiB per I/O, the
transfer itself dominates.

## 9. Batch versus stream hazards (checklist)

- [ ] Batch on a buffer produced by stream work: synchronize first (event or stream).
- [ ] Stream work consuming batch output: only after GetStatus reports the entry complete.
- [ ] Reusing a staging slot after a batch entry: wait for its D2D copy event.
- [ ] Async arg blocks (`size`, `off`, `buf_off`, `bytes_done`) outlive the stream point.
- [ ] Never pass the null stream to a reactor that fans out (see cuCascade commit `469a0221`).
- [ ] Check `bytes_done` / `ev.ret` against the requested size: short reads happen (EOF, holes).


---

# Appendix B: 02-uring-reactor-design.md


Audience: agents designing the GDS/cuFile reactor. The uring reactor already
implements most of the pattern in `01-gds-workflow-patterns.md` §6–7. The
difference is that its staging ring lives in **pinned host memory** and the
device hop is H2D/D2H instead of D2D. This note describes exactly how it does
that, so a cuFile design can reuse its contracts.

Snapshot: branch `io/s2-write`, HEAD `9eff1f9b`. Line numbers refer to that
commit.

Files:
- `include/cucascade/io/uring/{uring_reactor.hpp, config.hpp, types.hpp, uring_ioctx.hpp}`
- `src/io/uring/{uring_reactor.cpp, uring_ioctx.cpp}`
- Shared: `include/cucascade/io/{templated_ioctx.hpp, io_request.hpp, types.hpp}`,
  `src/cuda/device_copy_batch.cpp`, `src/cudf/disk_io_context.cpp`

---

## 1. Topology

```
caller ──ioctx::mixed_readv_async_io / mixed_writev_async_io (prepared_io_slice[])──►
templated_ioctx<uring_reactor>::dispatch_slices          (templated_ioctx.hpp:710)
   ├─ resolve_default_streams: null → cudaStreamLegacy, PerThread → stand-in stream  (:133)
   ├─ next_reactor: rotate start, pick dispatch_fanout()=2 least-queued_bytes reactors (:428,:456)
   ├─ one grouped_coordinator (1 credit / slice) → semi_future<size_t>               (io_request.hpp:67)
   ├─ partition slices across chosen reactors by bytes; host-only reads are cut into
   │  entries of ≤ range_batch_slices (8) slices
   └─ reactor->enqueue(grouped_io_request)   (noexcept)                              (uring_reactor.cpp:815)

uring_ioctx = pool of N reactors (io.uring_n_reactors, default 4; config.hpp:73)
each uring_reactor = 1 worker thread + 1 io_uring + up to 64 pinned staging blocks (64 MiB budget)
```

- **Staging memory**: `disk_io_context.cpp:47–67`. The pool has
  `n_reactors × 64 MiB` of NUMA-local pinned memory (`numa_region_pinned_host_memory_resource`,
  portable). It is exposed through a `fixed_size_host_memory_resource` with
  1 MiB blocks.
- Each reactor takes its share at `start()` (`uring_reactor.cpp:736–760`):
  `slot_count = clamp(64 MiB / block_size, 1, 64)` (`:84–85`), which is
  64 × 1 MiB with the defaults.
- **Ring**:
  - `make_ring(2*slot_count)` (`:146`). It prefers `SINGLE_ISSUER | COOP_TASKRUN | DEFER_TASKRUN`.
  - The staging blocks are registered as io_uring fixed buffers
    (`register_buffers`, `:1074`). If registration fails, the reactor uses
    plain reads and writes.
- **No CUDA streams of its own.** Every device copy is enqueued on the
  **caller's stream** (`d_buffer.stream`), and the reactor polls a per-slot
  CUDA event.

## 2. Unit of work and state machine

- `prepared_io_slice` (`types.hpp:421`) is the logical window `rng`, with:
  - a destination or source: `h_buffer` (monostate = "stage it", contiguous
    host pointer, or cache fragments) and/or `d_buffer` (device ptr, stream,
    device id)
  - `kind` (read, write, or flush) and `priority` (high or low)
- The worker takes one **active** `grouped_io_request` at a time and expands
  its slices in order. Each slice becomes one or more physical `uring_io_op`s
  (`types.hpp:48`) via `plan_slice` (`:538`) or `plan_write_slice` (`:444`).
  The coordinator gets `add_tasks(N-1)` credits for the extra ops.
- Ops wait in `pending` until `dispatch_pending` (`:1533`) can lease
  `staging_blocks` slots from a lock-free `slot_pool`. The first token's index
  is the **leader slot**: it owns the op, the SQE `user_data`, and the copy event.
- Slot states (`:248`):

```
read  : idle ─lease─► reading (SQE in kernel) ─cqe ok─► copying (H2D on caller stream, event) ─event done─► idle
write : idle ─lease─► staging (D2H on caller stream, event) ─event done─► reading (SQE) ─cqe ok─► idle
host-src write / host read : idle ─► reading ─► idle   (no device hop)
```

- An op completes through `io_op_request::finish_success` or `finish_error`
  (`io_request.hpp:487`). Each of these settles exactly one coordinator credit.
  When credits reach 0 the future resolves with `bytes_requested`, or with the
  first error.

## 3. Read path (device destination, staged)

1. **Plan** (`plan_slice`, `:559–583`):
   - The physical range is aligned with `staged_physical_range` (`:359`):
     4 KiB outward, clamped to EOF rounded up. This is the read-around.
   - O_DIRECT is used only if a direct fd exists and the block size is a
     multiple of 4 KiB.
   - The range is cut into ops of `dynamic_io_target(backlog, free_slots, block)`
     (`types.hpp:76`). That is backlog clamped to [256 KiB, 16 MiB], rounded to
     whole blocks, and capped by free slots. Large backlog plus free slots gives
     big I/Os; a small backlog gives small, low-latency ones.
   - `staging_blocks = ceil(bytes / block)`.
2. **Lease and submit** (`dispatch_pending`):
   - Lease the slots and build `iovecs` over the (non-contiguous) staging
     blocks.
   - `prepare_sqe` (`:288`) picks a variant:
     - `read_fixed` for a single block on a fixed-buffer ring
     - `read` for a single iovec
     - `readv` for multiple blocks
   - SQEs are batched in `prepared` and published with one `io_uring_submit`
     per pass (`submit_prepared`).
3. **Reap** (`reap_completions`, `:1296`):
   - Peek a batch of CQEs.
   - A short read records `bytes_read`. The op is resubmitted for the
     remainder (`resubmit_incomplete`, `:1401`), switching to buffered I/O if
     the remainder is no longer O_DIRECT-compatible.
   - `EINVAL`/`EOPNOTSUPP` on O_DIRECT falls back to the buffered fd
     (`fallback_to_buffered`, `:1287`). A fixed-buffer error disables fixed
     buffers for that slot.
   - Reaching EOF (`available` clamp) counts as done.
4. **H2D** (`start_device_copy`, `:1217`):
   - `device_cpy_request::copy_async` (`io_request.hpp:322`) skips the aligned
     over-read (`intersect(req_rng, io_rng)`). It batches one copy per staging
     block into a `device_copy_batch`, which uses `cudaMemcpyBatchAsync`, or a
     `cudaMemcpyAsync` loop on legacy/per-thread streams
     (`device_copy_batch.cpp:33,71`).
   - The copy is enqueued **on the caller's stream**, then
     `cudaEventRecord(slot.copy_event, caller_stream)`.
   - The slot moves to `copying`.
5. **Copy completion** (`poll_copy_completions`, `:1263`):
   - `cudaEventQuery` is called on each pass. The worker never blocks on a
     caller stream.
   - When there are no CQEs, the ring wait shrinks to `COPY_POLL_INTERVAL` =
     100 µs (`:92`).
   - When the event is done: `finish_success` → slot back to idle →
     credit settles.
6. **Caller visibility**:
   - The future resolves only after every H2D event has fired, so after the
     future is ready the device bytes are valid with no further sync.
   - The copy runs on the caller's stream, so it is also stream-ordered after
     whatever the caller had queued there before the worker enqueued it. Work
     the caller queues *after* `read_async` returns is **not** ordered after
     the copy. The caller must wait on the future, because there is no
     event-based handoff.
7. **Host and cache reads**: no staging. A contiguous host buffer gets I/O
   directly (O_DIRECT if pointer, offset, and length are all 4 KiB-aligned,
   `is_odirect_compatible`, `types.hpp:90`). Cache fragments are coalesced into
   `readv` ops up to the dynamic target and `IOV_MAX`.

## 4. Write path

1. **Dispatch** (`templated_ioctx::mixed_writev_async_io`, `templated_ioctx.hpp:619`):
   - `validate_write_slices`: one source each, no overlap.
   - Slices larger than `write_split_bytes` (32 MiB, `config.hpp:76`) are cut
     on granule boundaries (`split_write_slices`, `:652`), so pieces spread
     across reactors and pipeline. Then `dispatch_slices` runs as for reads.
2. **Plan** (`plan_write_slice`, `:444`). `split_write_range(rng, write_granule())`
   (`types.hpp:115`) splits each slice into **head, body, tail**. The granule
   is max(4 KiB, page size).

   | Source | Piece | Path |
   |---|---|---|
   | device | aligned body | staged + O_DIRECT (direct fd and block size % 4K == 0) |
   | device | head / tail | staged + buffered fd |
   | host | head / tail, or no direct fd | plain buffered write straight from the user pointer |
   | host | body, pointer congruent with offset (4K-aligned) | plain O_DIRECT straight from the user pointer |
   | host | body, pointer not aligned | staged (memcpy into the pinned block) + O_DIRECT |

   Staged pieces are cut into dynamic-target ops. Plain pieces are capped at
   1 GiB each (`MAX_PLAIN_READ_SIZE`).
3. **Device source, D2H** (`start_staging_copy`, `:1466`):
   - `copy_to_host_async` (`io_request.hpp:380`) copies from the device source
     into the leased pinned blocks **on the caller's stream**, so it runs after
     whatever the caller queued that produces the data. Then an event is
     recorded.
   - The slot moves to `staging`. Its bytes are added back to `queued_bytes`,
     so `next_reactor` sees a reactor stalled on a slow caller stream.
   - `poll_staging` (`:1500`): when the event is done, `prepare_staged_write`
     checks `coordinator->should_continue()`, then queues the write SQE.
4. **Host-source staged write**: `copy_host_bounce` (`:1522`) memcpys on the
   worker thread before the SQE.
5. **Completion**:
   - A short write is resubmitted.
   - `finish_success` calls `obj->note_written(end)`, which atomically raises
     the object's size, then settles the credit.
   - Writes are never clamped to EOF.
6. **Flush**:
   - Async: one `IORING_OP_FSYNC` with `IORING_FSYNC_DATASYNC` on the buffered
     fd, as one flush slice to the least-loaded reactor.
   - Sync: `flush()` calls `fdatasync`.
7. **Sync host write** (`host_write`, `:950`): same head/body/tail placement on
   the calling thread, `pwrite_all` with an O_DIRECT→buffered retry on
   `EINVAL`/`EOPNOTSUPP`.
8. **Open for write** (`create_io_object_for_write`, `:900`):
   - A buffered `O_RDWR` fd, plus an O_DIRECT fd reopened from the buffered fd
     via `/proc/self/fd`, so a concurrent rename cannot split the pair.
   - Best-effort `fallocate(KEEP_SIZE)` using `size_hint`.

## 5. Scheduling, queue depth, and fairness

- **QD bound per reactor**: ≤ slot_count ops (64), and fewer when ops span
  multiple blocks. With 4 reactors and a fan-out of 2 per request, one request
  sees ≤ 2 × 64 blocks in flight.
- **Expansion pacing**: `slices_per_pass` (4, `config.hpp:62`). Each pass
  expands at most 4 slices of the active request. A pass only starts after
  progress, i.e. a CQE, a copy done, or a new request (`may_expand`).
- **Priority** (`io_priority`):
  - The reactor serves the high tier first.
  - An active low request is **parked** at a slice boundary when high work
    arrives (`preempt_low`, `:1197`).
  - Bounded service: after `low_service_interval` (4) high starts, low gets
    one quantum (`grant_low_service`, `:1158`).
- **Backpressure**: when no slot is free, ops stay in `pending`. Nothing blocks
  the caller: `enqueue` is noexcept and the queue is unbounded unless it
  rejects with `no_buffer_space`.
- **Idle/wait policy** (`:1718–1750`):
  - Ops in flight: wait for a CQE with a timeout of 20 ms, or 100 µs if copies
    are pending.
  - Only copies pending: poll every 100 µs.
  - Fully idle: block on the request queue, waking every 20 ms.
- **Gauges**: `take_gauges()` returns inflight, max_inflight, pending, queued
  bytes, preemptions, and queue-delay histograms per tier.

## 6. Failure and shutdown invariants (must be preserved by any new reactor)

- Each op calls exactly one of `finish_success` or `finish_error` (guarded by
  `_terminal`). The cache `on_complete` callback runs before the credit
  settles.
- **Staging may never return to the pool while a device copy can still touch
  it.** If `copy_async`, `copy_to_host_async`, or the event record fails
  part-way, the reactor calls `cudaStreamSynchronize(caller_stream)` before
  settling (`:1242,:1487,:1495`). A test hook forces this:
  `set_copy_event_record_failure_for_testing`.
- **Shutdown** (`:1758–1880`):
  1. Close admission under `_enqueue_mutex`.
  2. Cancel pending, active, parked, and queued requests.
  3. Drain in-flight SQEs. On a fatal error, use `cancel_all_sync`, then
     zero-submit `GETEVENTS` so stray prepared SQEs are never published.
  4. Synchronize the remaining `staging` and `copying` events.
  5. Settle every slot.
- A ring setup failure (seccomp, `io_uring_disabled`) surfaces from `start()`.
  Later requests fail with that same error.
- A null stream is never handed to the reactor (commit `469a0221`), because
  `resolve_default_streams` pins it.

## 7. Mapping to the GDS staging-ring plan (`01` §6–8)

| Concern | uring reactor (host staging) | GDS plan (device staging) | Reuse? |
|---|---|---|---|
| Staging memory | pinned host, 1 MiB blocks, 64 per reactor, from a shared fixed-size resource | `cudaMalloc` + `cuFileBufRegister` once, sliced into blocks | Same *slot pool / lease* abstraction; different resource |
| Device hop | H2D (read) / D2H (write) on **caller's** stream + event poll | D2D on an **internal** I/O stream, or the caller's stream | Same event-poll lifecycle; choose the stream |
| Transport | io_uring SQE, one worker thread, kernel async | `cuFileReadAsync` on stream, or batch API, or sync `cuFileRead` on worker threads | Different |
| QD source | ≤64 SQEs per reactor × reactors | internal stream pool (16–32) or a batch handle pool | Concept identical: QD = slots in flight |
| Read-around / alignment | `staged_physical_range`, `copy_async` skip | same | **Reuse directly** |
| Write head/body/tail | granule split; body O_DIRECT, edges buffered | cuFile handles unaligned edges, but they leave the fast path; same split helps | **Reuse split** |
| Dynamic I/O size | `dynamic_io_target` 256 KiB–16 MiB by backlog | same idea, bigger for network FS (1–8 MiB+) | Reuse with a different clamp |
| Priority / preemption / bounded low service | yes | needed for the same demand-versus-readahead reasons | **Reuse** |
| Completion to caller | host `semi_future` after event; no stream-event handoff | plan proposed `cudaStreamWaitEvent(user_stream, ev)` | API change; see open question |
| Failure safety | sync the caller stream before releasing staging | same rule, plus never deregister while DMA may be live | **Reuse rule** |

### Open questions this raises for the GDS design

1. **Stream handoff**: should device reads keep the future-only contract
   (wait on the host), or add an event/stream-ordered completion so callers
   can avoid a host wait? The uring reactor deliberately enqueues on the
   caller stream and polls, so it never blocks on that stream.
2. **Whose stream does the D2D run on?** On the caller's stream, a busy caller
   stream delays slot recycling, which hurts QD for everyone. That is why
   `staging` bytes are re-counted in `queued_bytes`. An internal copy stream
   decouples them, but then it needs an event join to the caller.
3. **One worker thread per reactor** suits io_uring (kernel async). For cuFile,
   the transport is either stream-async (`cuFileReadAsync`) or batch, and
   neither needs a thread per in-flight op. Thread count does not drive QD
   there.
4. **The staging block size invariant**: `staging_block_size()` must equal the
   prefetch cache chunk size (`uring_reactor.hpp`, `staging_block_size` doc).
   A device-staging reactor that also feeds the host cache has the same
   coupling.


---

# Appendix C: 03-cufile-reactor-gap-analysis.md


Scope: `src/io/cufile/*`, `include/cucascade/io/cufile/*`, and the shared I/O abstractions they
implement. Target design: `01-gds-workflow-patterns.md` (§6 staging ring, §7 QD, §8 batch handle
pool, §9 hazards). Repo constraints: `AGENTS.md`. Read-only analysis; no source was modified.

Citation convention: `file:line` is relative to the repo root unless it starts with a bare
`cufile_reactor.cpp` / `.hpp`, which means `src/io/cufile/cufile_reactor.cpp` /
`include/cucascade/io/cufile/cufile_reactor.hpp`. `[belief]` marks an engineering claim not
verified in this repo.

---

## 0. One-paragraph summary

The reactor is a **host-synchronous** design: N worker threads (default 4), each running one
blocking `cuFileRead`/`cuFileWrite` (or POSIX `pread`/`pwrite` plus a pinned-scratch copy) at a time.
Device buffers are **not registered by default**, there is **no staging ring**, **no async or batch
cuFile API**, and **no `cuFileStreamRegister`**. The caller's CUDA stream is only a *gate*: the
reactor polls it from the host until it is idle, then performs a blocking transfer and settles a
host future. The result is that sustained queue depth is bounded by the reactor-thread count (default
4), not by anything stream-related. The target design (async + I/O stream pool + registered ring +
GPU-side join) replaces nearly every part of the data path. The surrounding scaffolding (request
coordinator, priority queues, object lifetime, cancellation, null-stream handling, test seams) is
worth keeping, but the **io_context completion contract** is the main design constraint: it promises
host-complete bytes when the future resolves.

---

## 1. Architecture map

### 1.1 Classes

| Class | Role | Where |
|---|---|---|
| `cufile_api` | `dlopen("libcufile.so.0")` function table; only 8 symbols: `driver_open`, `driver_get_properties`, `handle_register/deregister`, `buf_register/deregister`, `read`, `write` (+ optional `cuFileGetVersion`) | `include/cucascade/io/cufile/cufile_api.hpp:42-49`, `src/io/cufile/cufile_api.cpp:72-83` |
| `cufile_io_object` | Passive handle bag: buffered fd, O_DIRECT fd, `CUfileHandle_t`, atomic size; deregisters the handle in its destructor | `cufile_reactor.hpp:69-126`, `cufile_reactor.cpp:778-782` |
| `cufile_reactor` | One worker thread + two-tier request queue + per-device copy stream + pinned scratch | `cufile_reactor.hpp:209-506` |
| `cufile_reactor::reactor_context` | Shared immutable `config` only | `cufile_reactor.hpp:218-226` |
| `cufile_ioctx : templated_ioctx<cufile_reactor>` | Pool of `n` reactors; overrides `next_reactor`, `create_io_object[_for_write]`; `path_capability()` | `include/cucascade/io/cufile/cufile_ioctx.hpp:57`, `src/io/cufile/cufile_ioctx.cpp:32-96` |
| `templated_ioctx<R>` | Generic front end: slices → partitions → `grouped_io_request` per reactor; resolves null / per-thread default streams on the submitting thread | `include/cucascade/io/templated_ioctx.hpp:296`, `:710-808`, `:133-156` |
| `grouped_io_request` / `grouped_coordinator` | Queue entry (slices) and the fan-in that fulfils the caller's future once every slice credit is settled | `include/cucascade/io/io_request.hpp:67-212`, `:222-308` |
| `io_op_request` | Backend-neutral physical op; `finish_success/finish_error` settle exactly one credit | `io_request.hpp:487-518` |
| `registration_table`, `transfer_registration` | Process-wide refcounted table for optional per-transfer `cuFileBufRegister` | `cufile_reactor.cpp:337-477`, `:492-539` |
| `scratch_pool` | Never-destroyed pool of pinned host scratch (avoids `cudaFreeHost` device sync) | `cufile_reactor.cpp:589-625` |
| `request_scheduler` | Strict high/low priority with bounded low service | `cufile_reactor.cpp:653-743` |
| `owned_stream` | RAII non-blocking CUDA stream (one per device, per reactor) | `cufile_reactor.hpp:361-371`, `cufile_reactor.cpp:1447-1486` |
| `platform_info`, `probe_path`, `probe_object` | Advisory GDS capability probing (never gates a read) | `platform.hpp:44-181`, `platform.cpp:57-438` |

### 1.2 Threads: who submits, who reaps

- **Submit:** the caller's thread, inside `templated_ioctx::dispatch_slices`. It builds partitions,
  makes the coordinator and future, then `reactor->enqueue()`. Enqueue takes `_enqueue_mutex` and
  pushes onto a moodycamel queue. No CUDA or cuFile call happens here, except stream resolution
  (`io/templated_ioctx.hpp:720`, `:133-156`) and the stand-in stream for PTDS (`:78-117`).
- **Execute and "reap":** the *same* reactor worker thread (`std::jthread`, `cufile_reactor.hpp:477`,
  started at `cufile_reactor.cpp:819`). It runs `worker_loop` (`:1835-1945`). It performs the blocking
  cuFile/POSIX I/O inline in `dispatch_slice` (`:1048-1120`), and it polls GPU events in
  `poll_completions` (`:1740-1809`). There is **no separate reaper thread**, because there is nothing
  asynchronous to reap on the GDS path.
- **Number of threads:** `io_config::cufile_n_reactors{4}` (`include/cucascade/io/config.hpp:94`).
  The DISK tier builds one process-shared context (`src/cudf/disk_io_context.cpp:228-240`), so **all
  spill traffic of the process shares those 4 threads**.
- **Synchronous caller-thread paths:** `host_read` (`cufile_reactor.cpp:1036-1042`),
  `host_write` (`:1492-1514`), `flush` (`:1516-1526`). Each runs on the calling thread, not on a worker.
- No thread pinning or NUMA affinity beyond a thread name (`cufile_reactor.cpp:825-828`).

### 1.3 Queues

- `tiered_blocking_queue<unique_ptr<grouped_io_request>> _requests` per reactor
  (`cufile_reactor.hpp:478`). Two moodycamel MPMC queues (high and low) share one
  `lightweight_semaphore` (`include/cucascade/io/tiered_queue.hpp:45-116`). An `io_priority::automatic`
  request is treated as high (`io/types.hpp:408-415`).
- **Worker-local state:** `request_scheduler` with one `active` and one `parked` request
  (`cufile_reactor.cpp:653-743`); `_waiting` (device slices parked behind a busy stream) and
  `_inflight_copy` (H2D copies of POSIX-fallback pairs) (`cufile_reactor.hpp:498-499`). Both are
  `reserve`d to `max_inflight` (`cufile_reactor.cpp:803-804`).
- Partitioning across reactors is by bytes, smallest partition first, **at slice granularity only**
  (`io/templated_ioctx.hpp:756-762`). A single large slice is never split across reactors.

### 1.4 Request lifecycle (read into device memory)

```
caller thread                         reactor worker thread (1 of N)                CUDA / cuFile
─────────────                         ──────────────────────────────                ─────────────
ioctx.device_readv_async_io(obj,
   slices, stream)                    (io_context.cpp:249-267)
 └ mixed_readv_async_io               (templated_ioctx.hpp:492-565)
    clamp to EOF, resolve device_id
 └ dispatch_slices                    (templated_ioctx.hpp:710-808)
    resolve_default_streams           null → cudaStreamLegacy;
       (:133-156)                      cudaStreamPerThread → stand-in stream
    next_reactor() → 1..N reactors    (cufile_ioctx.cpp:63-96)
    coordinator(total_bytes, n)       one credit per slice
    reactor->enqueue(req)  ─────────► _requests (high|low)  (cufile_reactor.cpp:860-886)
 └ returns semi_future                worker_loop (:1835)
caller blocks: future.get()             poll_completions()  ◄── cudaEventQuery on parked slices (:1740)
 (src/cudf/disk_io.cpp:156)             scheduler.take_next / activate (:686)
                                        take_front slice, dispatch_slice (:1048)
                                         ├ park_if_stream_busy (:1122)  ── cudaStreamQuery(stream)
                                         │     busy → record event, push to _waiting, return
                                         │            (re-dispatched with stream_ready=true by poll_completions, :1782)
                                         ├ clamp to EOF; size 0 → finish_success
                                         ├ registered && device_only && size ≥ gds_threshold?
                                         │     read_cufile (:1158) ──── loop ≤64 MiB pieces ─► cuFileRead(sync) ──► NVMe → GPU
                                         │       (cuFile fail → return false → POSIX path)       (host thread blocks)
                                         └ else dispatch_posix → read_device_staged (:1337)
                                               pread(buffered fd) → pinned scratch[2 × 8 MiB]
                                               cudaMemcpyAsync H2D on reactor copy stream; wait slot event on host
                                        request->finish_success()  (io_request.hpp:498)
                                          → coordinator->on_complete() → last credit → promise.set_value
 ◄───────────────────────────────────  future resolves; bytes are in GPU memory (host-complete)
```

Writes follow the same path through `dispatch_write` (`cufile_reactor.cpp:1528-1592`), described in §3.

### 1.5 Config knobs and defaults

`include/cucascade/io/cufile/config.hpp` (struct `config`), plus the `io_config` wrapper:

| Knob | Default | What it really controls | Line |
|---|---|---|---|
| `io_config::cufile_n_reactors` | **4** | Worker threads = the *maximum number of concurrent cuFile calls in the process* | `io/config.hpp:94` |
| `max_inflight` | 32 | Parked slices + in-flight H2D copies per reactor. **Synchronous cuFile/POSIX transfers never count** → not a QD knob | `config.hpp:85-91` |
| `max_op_bytes` | 64 MiB | Largest single `cuFileRead`/`cuFileWrite`; also the per-pass dispatch budget in `worker_loop` | `config.hpp:104`, `cufile_reactor.cpp:1877` |
| `register_min_bytes` | **0 = never register** | Per-transfer `cuFileBufRegister` threshold | `config.hpp:128` |
| `write_split_bytes` | **0 = never split** | Cuts large write slices for multi-reactor spread (`templated_ioctx.hpp:863-896`) | `config.hpp:134` |
| `gds_threshold` | 256 KiB | Device reads/writes below this go POSIX + staging | `config.hpp:141` |
| `use_odirect` | true | Open the O_DIRECT fd that cuFile registers against | `config.hpp:150` |
| `compat_mode` | unset = `automatic` | Per-file cuFile policy (`on`/`off`/`automatic`) | `config.hpp:155` |
| `poll_interval_us` | 200 | Sleep between completion polls while parked slices / copies exist | `config.hpp:161`, `cufile_reactor.cpp:1910` |
| `low_service_interval` | 4 | Bounded low-priority service | `config.hpp:170` |
| `n_max_concurrent_scans` | 4 (`default_gpu_pipeline_num_threads`) | Readahead scheduler hint; unused by the DISK tier | `config.hpp:76-77`, `include/cucascade/exec/config.hpp:27` |
| `min_alignment_requirement()` / `merge_gap_size()` | 4096 / 4096 | Advertised to callers so they align and merge up front | `config.hpp:175,179` |
| compile-time: scratch slots × bytes | 2 × 8 MiB per reactor | Pinned POSIX-fallback scratch | `cufile_reactor.hpp:492-494` |
| compile-time: `idle_wait_us`, `max_short_reads`, `min_direct_host_write` | 2 ms, 64, 4 MiB | Worker idle wait; short-read tolerance; host-write O_DIRECT threshold | `cufile_reactor.cpp:81,641,630` |

**No knob exists for:** I/O stream count, staging size, async vs sync, batch handle count,
`cuFileStreamRegister` flags, NIC/NUMA affinity, or any `cufile.json` parameter. The code never
reads `cufile.json` and never calls `cuFileSetParameter*` (confirmed by grep over
`src/io/cufile`, `include/cucascade/io/cufile`, `src/cudf`, and `test/io/cufile`). It only mentions
`cufile.json` in doc comments (`platform.hpp:91`, `include/cucascade/cudf/disk_io_context.hpp:73`).

**Provenance of the defaults:** the comments say they were tuned in **cuFile compatibility mode**
(no nvidia-fs): 64 MiB op size (`config.hpp:99-103`), registration "measured no gain" (`:122-125`),
staged writes faster than `cuFileWrite` (`:62-65`, `cufile_reactor.hpp:162-166`). The disk benchmark's
reference numbers are gdsio in compat mode, 6.73 GiB/s write and 13.35 GiB/s read, "NOT a
GPUDirect-DMA figure" (`benchmark/benchmark_disk_converter.cpp:90-96`). **No default has been
validated on a real GDS stack.**

### 1.6 Who consumes it (the DISK tier)

- The converters build **one vectored request per batch file** (one slice per column buffer) through
  the DISK space's `io::ioctx` (`src/cudf/representation_converter_builtins.cpp:1309`).
- GPU→DISK: `write_disk_file_device` calls `device_writev_async(...)` and then `request.get()`
  (`src/cudf/disk_io.cpp:110-125`).
- DISK→GPU: destinations are **stream-ordered RMM pool allocations on the caller's stream**
  (`allocate_disk_column`, `representation_converter_builtins.cpp:1802-1830`). The converter then
  blocks on `read_disk_file_device` (`disk_io.cpp:142-158`) and relies on the host-complete contract
  to skip a `stream.sync()` (comment at `representation_converter_builtins.cpp:1960-1971`).
- Each call opens an `io_object` (`cuFileHandleRegister` per file, `cufile_reactor.cpp:236`).

---

## 2. Read path in detail

**2.1 cuFile API used.** Only synchronous `cuFileRead` (`cufile_api.hpp:48`; call at
`cufile_reactor.cpp:1189-1196`). Not used: `cuFileReadAsync`, `cuFileBatchIO*`, `cuFileStreamRegister`.
None of these symbols is even resolved (`cufile_api.cpp:72-79`). The header explains the
choice: *"cuFileReadAsync serialises a stream's operations, runs synchronously in the submitting
thread in compatibility mode, and keeps per-stream state keyed by caller stream handles the reactor
neither registers nor deregisters"* (`cufile_reactor.hpp:168-171`).

**2.2 Eligibility.** A slice goes to cuFile iff the file registered with cuFile, the slice is
device-only, and `clamped >= gds_threshold` (`cufile_reactor.cpp:1105-1109`). The test is
`file->cufile_registered()`, **not** `gds_available()`. So reads use `cuFileRead` even in
compatibility mode (where libcufile bounces through host memory itself). Writes make the opposite
choice (`:1577-1580`). Any `cuFileRead` error returns `false` and the whole range retries through the
POSIX path (`:1197-1208`); the first failure is logged once per process.

**2.3 Threads and streams.** The cuFile call runs on the reactor worker thread, issued on **no CUDA
stream** (sync API). The caller's stream is used for exactly two things:
1. **Input gate:** `park_if_stream_busy` (`:1122-1156`). For an explicit stream it does
   `cudaStreamQuery`. If the stream is idle the slice proceeds at once. Otherwise it records an event
   on the stream, moves the slice to `_waiting`, and `poll_completions` re-dispatches it with
   `stream_ready=true` when the event completes (`:1745-1791`). For `cudaStreamLegacy` it records an
   event and queries the *event*. A bare legacy-stream query does not capture blocking-stream
   dependencies (commit `469a0221`, `:1140-1153`).
2. **H2D copies of the host+device pair path** run on the caller's stream (`:1287-1288`). The
   device-only POSIX fallback uses a reactor-owned stream (`:1356`, `:1411`).

The reactor-owned stream is one `cudaStreamNonBlocking` stream per device per reactor, created lazily
(`:1447-1460`). It is used only for staged copies, never for cuFile.

**2.3a Sleep-poll cost.** While anything is parked, the worker wakes every `poll_interval_us`
(200 µs) to query CUDA events (`:1902-1904`, `:1910`). If the in-flight window is full it
`sleep_for`s. This is host polling in place of a GPU-side dependency.

**2.4 Buffer registration policy.**
- Default: never register (`register_min_bytes=0`, `config.hpp:128`; `want` is false at
  `cufile_reactor.cpp:500`). cuFile receives the exact destination pointer and bounces internally.
- Opt-in: when `register_min_bytes>0`, the transfer is ≥ that size, and the **destination pointer
  and file offset are 4 KiB-aligned** (`:500-501`, `:1177`), the reactor calls `cuFileBufRegister`
  on `[dst, dst+slice size)` **for the duration of this one slice** and deregisters in the
  `transfer_registration` destructor, before the request settles (`:513-521`, `:1176-1223`). It
  passes the registered base plus an offset (`:1191-1195`).
- **Every** cuFile transfer, registered or not, goes through the process-wide `registration_table`
  (`:1176`, `:1603`). That means a global mutex plus `std::map`/`multimap` operations, with a linear
  scan of in-flight unregistered transfers (`:466-471`), across all reactors. This is cheap relative
  to 64 MiB I/O but is shared serialization on the hot path.
- A buffer that overlaps a live registration, or that cuFile reports as
  `CU_FILE_MEMORY_ALREADY_REGISTERED`, is a *conflict*: `read_cufile` returns false and the slice
  takes the **POSIX path** (`:361-367`, `:385`, `:1178`). `cuFileBufRegister` failure on VMM or
  `cudaMallocAsync` memory falls back to unregistered, logged once (`:374-388`).

**2.5 Staging / bounce buffers.**
- *GDS path:* none of ours. Any bounce is cuFile-internal for unregistered buffers.
  Per `cufile.h` (`cuFileRead` note at line 430, `cuFileReadAsync` at line 658):
  *"If the bufPtr is not registered ... the data will be buffered through preallocated pinned buffers"*. The step-1 doc
  describes the bounce pool as GPU-resident (`01-gds-workflow-patterns.md` §3). The header says "pinned" without saying host or device, so the two may disagree; see §6.
- *POSIX path:* **pinned host** scratch, 2 slots × 8 MiB per reactor
  (`cufile_reactor.hpp:492-494`), taken from a process-wide pool on first use (`:1434-1440`). The
  scratch is double-buffered. `pread` of piece *k+1* overlaps the H2D copy of piece *k*
  (`read_device_staged`, `:1337-1432`). The worker **host-blocks** on slot events
  (`:1386`) and on the final drain (`:1427`).
- There is **no device staging ring** anywhere.

**2.6 Chunking, slot size, alignment, O_DIRECT.**
- Pieces of at most `max_op_bytes` (64 MiB), cut at 4 KiB file boundaries. An unaligned EOF tail is
  split off so the aligned body stays eligible for direct DMA (`detail::next_piece`,
  `:1949-1962`). Pieces are issued **sequentially** by one thread (`:1181-1217`).
- The slice is never subdivided across reactors; the partitioner works per slice
  (`templated_ioctx.hpp:756-762`).
- Alignment: the reactor does **not** read-around device destinations. An unaligned first-piece file
  offset or an unaligned device pointer is passed straight to cuFile, which handles the edges
  internally (the cost is that they leave the fast path, `01-...md` §4). Callers are told to align
  and merge to 4 KiB (`config.hpp:175,179`), and the DISK file layout is 4 KiB-aligned by
  construction (`include/cucascade/data/disk_file_format.hpp:26`,
  `representation_converter_builtins.cpp:1419-1429`).
- **Read-around exists only for host-only O_DIRECT reads**: whole blocks through the pinned
  scratch, then `memcpy` (`read_host_direct`, `:1302-1335`).
- O_DIRECT: the direct fd is opened by reopening the owned buffered fd via `/proc/self/fd`
  (`open_direct_fd`, `:168-200`) and is what cuFile registers (`:232-236`). The **device-only POSIX
  fallback reads through the *buffered* fd** (`:1396`), so sub-256 KiB device reads and
  not-registered files pollute the page cache (a host-only read goes O_DIRECT, `:1258-1262`).

**2.7 Completion signalling to the caller's stream.** None on the GDS path:
- The read completes **inside** `dispatch_slice`; the future resolves host-side after
  `request->finish_success()` (`:1222`). There is no event recorded, no `cudaStreamWaitEvent`, and no
  host callback. The contract in `io/io_context.hpp:493-507` is explicit: *"On success the bytes are
  in place: no further synchronization of stream is needed."* The DISK→GPU converter relies on this
  (`representation_converter_builtins.cpp:1960-1971`).
- POSIX path: H2D copies are queued on the reactor copy stream, and the slice settles only after a
  host `synchronize_no_throw` on the slot event (`:1365`, `:1386`, `:1427`).
- Host+device pair path: the copy is queued on the **caller's** stream with an event, and settled by
  `poll_completions` (`:1286-1299`, `:1794-1808`).

**2.8 Queue depth achieved, and what bounds it.**
- Each reactor runs one blocking transfer at a time, so **QD (cuFile calls in flight) ≤
  `cufile_n_reactors` = 4** for the whole process (default context). Inside one `cuFileRead` of ≤ 64 MiB,
  cuFile may split by its own `max_direct_io_size` (16 MiB per the comment, `config.hpp:101-103`)
  [belief: internally parallel]. We do not control or measure that.
- Reactors per request: `cufile_ioctx::next_reactor` gives *every* reactor for a vectored read with
  > 2 slices and > 2 reactors (`cufile_ioctx.cpp:79-95`). Otherwise the base class's fan-out of 2
  applies (`templated_ioctx.hpp:428-463`; the cufile config has no `fan_out`, so 2). A *single*
  slice maps to **one** reactor (`partition_count = min(reactors, slices)`,
  `templated_ioctx.hpp:749`).
- Slice-granular balancing: a table with one 1 GiB column buffer and many tiny ones keeps one
  reactor busy for the big buffer while others idle.
- `max_inflight=32` bounds parked + copy-in-flight items (`config.hpp:85-91`). It bounds neither
  cuFile nor POSIX I/O, which are never counted.
- Each caller thread also holds at most one request: DISK converters `.get()` immediately
  (`disk_io.cpp:156-157`).

**2.9 POSIX / compat fallback ladder.**
1. File not registered (no O_DIRECT, `compat::on`, registration failed, library absent) → POSIX
   (`:1107`).
2. Device read < `gds_threshold` (256 KiB) → POSIX buffered pread + pinned scratch + H2D. A comment
   records the known gap: 64 KiB reads ≈ 1.3 GB/s from 4 reactors vs ≈ 6 GB/s for uring
   (`cufile_reactor.hpp:202-205`).
3. Registration conflict (§2.4) → POSIX.
4. `cuFileRead` returns an error → POSIX for the whole range (`:1207`).
5. Host-only destination → O_DIRECT pread (direct or via scratch) (`:1258-1262`).
6. `CUFILE_FORCE_COMPAT_MODE`, `compat::on`: probing only; reads still call `cuFileRead` if registered
   (`platform.cpp:60`, `:97-102`).
7. The DISK tier's `automatic` backend selection: GDS really available → cuFile; else io_uring; else
   the cuFile reactor's POSIX path (`src/cudf/disk_io_context.cpp:117-136`).

---

## 3. Write path in detail

**3.1 API.** `cuFileWrite` (synchronous) for the aligned body of **device** sources only; everything
else is `pwrite`. No `cuFileWriteAsync`, no batch (`cufile_reactor.cpp:1594-1645`;
`cufile_api.hpp:49`).

**3.2 Splitting.** `dispatch_write` splits the range into head / body / tail at the write granule
(`max(4096, page size)`, `io/types.hpp:63-70`) via `uring::detail::split_write_range`
(`cufile_reactor.cpp:1535`).
- *Host source:* a congruent body ≥ 4 MiB goes O_DIRECT straight from the caller's buffer; anything
  else goes through the page cache with `sync_file_range` writeback kicked off at once
  (`:1537-1561`). The comments justify this by benchmark (5.3 vs 3.0–4.3 GB/s).
- *Device source:* the head and tail are **always staged**. The body goes to `cuFileWrite` only if
  `file.cufile_registered() && (gds_available() || test override) && body.size >= gds_threshold`
  (`:1577-1589`). Otherwise the body is staged too. `gds_available()` is a process-wide flag
  (`dstatusflags != 0 && !forced_compat`), not a per-filesystem check
  (`platform.cpp:113-118`).

**3.3 Threads and streams.** Same worker-thread, synchronous model. The input gate is the same
`park_if_stream_busy` (`:1067-1068`; device-only slices include writes). Staged writes use the
reactor's copy stream for D2H copies (`copy_stream`, `:1667`). They are **double-buffered**: D2H of
chunk *k+1* overlaps `pwrite` of chunk *k* (`:1715-1737`). The worker host-blocks on each slot event
(`:1724`).

**3.4 Registration policy.** As for reads: optional per-transfer, off by default. `aligned=true` is
passed, so only the **pointer** alignment is checked (`:500-501`, `:1603-1604`).

**3.5 Staging / bounce.** Pinned host scratch, 2 × 8 MiB per reactor, shared with reads
(`:1647-1738`). Chunks never cross a piece boundary, so a granule-aligned piece yields O_DIRECT-capable
chunks (`:1652-1661`). `write_host_piece` falls back to the buffered fd if the filesystem refuses
O_DIRECT at run time (`:310-325`).

**3.6 Chunking.** `cuFileWrite` pieces ≤ `max_op_bytes` (`:1601`, `:1612`), with a short-write guard
(reusing `max_short_reads` = 64, `:641`, `:1637-1640`). Staged chunks ≤ 8 MiB.

**3.7 Completion.** Synchronous; `request.finish_success()` after the last `pwrite`/`cuFileWrite`
(`:1591`). `note_written` raises the object's size in `finish_success`
(`io_request.hpp:501`). Flush is `fdatasync` (`:1522`). The source must stay valid until the future
resolves (`io/io_context.hpp:396-401`).

**3.8 QD.**
- Device write slices: `write_vector_async` with a resolved `device_id` falls through to the base
  `next_reactor`, i.e. **fan-out 2** (`cufile_ioctx.cpp:69,79-80`).
- No `write_split_bytes` by default → a single large device buffer is **one slice on one reactor**
  (QD = 1).
- Host-only vectored writes are deliberately pinned to **one** reactor (`cufile_ioctx.cpp:69-78`).

**3.5a Compat-mode asymmetry.** In compat mode (`!gds_available()`), device writes bypass cuFile
entirely and go D2H → `pwrite` (`:1577-1580`), while reads still go through `cuFileRead`.

**3.9 Open-time costs.** Each batch file does `open` + `fallocate(KEEP_SIZE)` hint + reopen O_DIRECT +
`cuFileHandleRegister` (`:980-1025`, `:232-236`), once per `open_io_object_for_write` call, i.e. per
converter invocation (`disk_io.cpp:122`).

---

## 4. Gap table

Legend: **Fits** / **Partial** / **No**. "Pattern" refers to `01-gds-workflow-patterns.md`.

| # | Pattern (01 doc) | Current implementation | Verdict | Consequence | What would need to change |
|---|---|---|---|---|---|
| 1 | Registered device staging ring (register once at startup, §6) | None. Only a pinned **host** scratch, 2 × 8 MiB per reactor (`cufile_reactor.hpp:492-494`, `cpp:1434-1440`) for POSIX fallback. Device buffers unregistered by default (`config.hpp:128`). | **No** | Every GDS read targets arbitrary unregistered pool memory, so cuFile bounces internally (`cufile.h:430,658`) and the direct P2P path is not used. Per-transfer registration exists but is off and unmeasured on real GDS. BAR1 use is unbounded if enabled (one registration per in-flight transfer ≤ 64 MiB). | New per-device ring: `cudaMalloc` + `cuFileBufRegister` once; slot allocator with backpressure; ring sizing from BAR1; graceful degrade if registration fails (see §6). Process-lifetime, never freed (mirror `scratch_pool`). |
| 2 | Async cuFile on an internal pool of 16–32 I/O streams; slot k → stream k % M (§6, §7) | Sync `cuFileRead`/`Write` on 4 worker threads (`cpp:1189`, `:1616`). The only streams are one non-blocking copy stream per device per reactor (`cpp:1447-1460`), used for H2D/D2H copies only. | **No** | QD is capped at n_reactors (4). Raising QD means more threads, each blocking in cuFile. | Add `cuFileReadAsync/WriteAsync` (new optional `cufile_api` members) and an I/O stream pool owned by the reactor/ioctx; slot→stream binding; make `cufile_api` tolerate libcufile versions without async symbols (§5(d)). |
| 3 | D2D copy staging → user destination on the I/O stream (§6) | None for GDS. The POSIX path does pinned-host → device H2D on the copy stream (`cpp:1411`). | **No** | Unregistered destinations are exposed to cuFile's bounce path. | Enqueue `cudaMemcpyAsync(dst, staging+head, n, D2D, s)` after the async read on the same I/O stream. Reuse the `device_copy_batch` machinery (`io_request.hpp:322-370`) for scatter. |
| 4 | GPU-side join onto the user's single stream via `cudaStreamWaitEvent` (§7) | Both directions are **host-side**: input gate = host `cudaStreamQuery` / event polling at 200 µs (`cpp:1122-1156`, `:1745-1791`, `:1910`); output = host-complete blocking I/O (`cpp:1222`). No `cudaStreamWaitEvent` anywhere in the reactor. | **No** (host sync in both directions) | The worker burns wake-ups polling; the caller thread blocks on `future.get()`; the user stream cannot be continued past the I/O without the host. | Record an event on the user stream at submit time (submitting thread, to keep null/PTDS semantics, see (g)); make I/O streams `cudaStreamWaitEvent` on it; record a completion event per op and have the user stream wait on it. **Requires an io_context contract decision**, see §5(b), §5(h). |
| 5 | High sustained QD 32–64+ for network storage (§7) | ≤ 4 concurrent cuFile calls per process (`io/config.hpp:94`); a single-slice request → 1 reactor; vectored writes with device slices → 2; host-only vectored writes → 1 (`cufile_ioctx.cpp:69-95`). `max_inflight=32` is not a QD bound (`config.hpp:85-91`). | **No** | Throughput bounded by 4 × (single-call latency) regardless of the device. Fine for local NVMe with large ops, badly under-provisioned for GPFS/WEKA/VAST. | Async + I/O-stream pool; per-ioctx queue-depth knob (target 32–64); admission by slot availability; sub-slice striping so one big buffer fans out across slots (today a slice is never split across reactors). |
| 6 | Batch API only via a handle pool with cookie-based reaping (§8) | Not used (`cufile_api.cpp:72-79` resolves no `BatchIO*`). | **N/A / No** (nothing to violate, nothing to build on) | None today. | If adopted: add optional symbols, handle pool (K × B) + reaper thread (the reactor has no reaper today, §1.2); cookie = slot/task id. Doc §8 itself concludes async + stream pool is simpler. |
| 7 | Read-around for unaligned requests (§4) | Device reads: **no** read-around; unaligned first-piece offset/pointer are handed to cuFile (`cpp:1186-1196`); `next_piece` only protects piece boundaries and the EOF tail (`cpp:1949-1962`). Host-only O_DIRECT reads: yes, through scratch (`cpp:1302-1335`). Callers are asked to align (`config.hpp:175`). | **Partial** | Unaligned device reads fall off the direct path, per cuFile. Sub-256 KiB device reads take the slow POSIX buffered path (`cufile_reactor.hpp:202-205`). | Ring slot read of the enclosing aligned window and copy out `[head, head+n)`; this also removes the need for the 256 KiB `gds_threshold` fallback to POSIX. |
| 8 | Pool-allocated destinations never registered directly (§5) | Default **never registers** (`config.hpp:128`), which matches. But there is an opt-in per-transfer register/deregister of *pool* sub-ranges with a process-wide overlap table (`cpp:337-539`) that conflicts with the "never register pool memory" rule and with VMM-backed `cudaMallocAsync` memory. | **Partial** (fits at defaults) | If a user sets `register_min_bytes>0`, each transfer pays `cuFileBufRegister` + `Deregister` on a pool sub-range (cost; interior-of-chunk semantics unverified, see §6). The table prevents stale registrations *across* transfers (deregistered before the future resolves, `cpp:513-521`, `:1219`). | Keep the default; mark the knob as "A/B only". Route the ring's registration through (or exclude it from) `registration_table`, since an externally registered range makes overlapping transfers hit the `ALREADY_REGISTERED → conflict → POSIX` path (`cpp:385`). |
| 9 | Persistent per-slot async arg blocks (§1.2, §9) | None; sync `cuFileRead` takes size/offset by value (`cufile_api.hpp:48`), so no lifetime hazard exists today. | **N/A** (becomes **No** once async is adopted) | None today. | Slot struct with `{size, file_off, buf_off, bytes_done}` and a stable address for the life of the stream point (`cufile.h` async: pointers read asynchronously). Check `bytes_done` for short reads (§9 checklist). |
| 10 | Optional `cuFileStreamRegister` flags (§1.2) | Not resolved, not called. `cufile.h` says registration "reduces IO setup and teardown overhead". Existing comment cites per-stream state as a reason to avoid async (`cufile_reactor.hpp:168-171`). | **No** | None today. | Optional `cufile_api` members; register the pool streams once at pool creation with `PAGE_ALIGNED_INPUTS` (ring offsets/sizes are aligned by construction); deregister at teardown (header: on stream errors, deregister to release resources). |
| 11 | (§9) Never pass the null stream to a reactor that fans out | **Fits.** Null → `cudaStreamLegacy` and `cudaStreamPerThread` → stand-in, both on the submitting thread before any reactor sees the slice (`io/templated_ioctx.hpp:133-156`, called at `:720`; commit `469a0221`). | **Fits** | n/a | Keep this; the new fan-out must consume `slice.d_buffer.stream` only after `resolve_default_streams`. |
| 12 | (§9) Check `bytes_done` vs requested; short reads | **Fits** for sync: `got == 0 || got > want || ++short_reads > 64` (`cpp:1211-1215`); EOF clamp up front (`:1097-1103`); shrink detection (`:1267`, `:1403`). | **Fits** | n/a | Must be reproduced host-side in a reaper for async completions (the stream cannot report it). |
| 13 | (§9) Staging slot reuse waits for the D2D copy event | Analogous and correct for the pinned scratch: per-slot events, drain on error (`cpp:1352-1372`, `:1671-1689`). | **Fits** (for the existing scratch) | n/a | Reuse the per-slot event discipline for ring slots. |

---

## 5. Specific mismatches with our plan

### (a) Single caller stream with no fan-out → low QD

Today's reality is subtler than the plan assumes. **The caller's stream is not what bounds QD.**
The GDS path does not issue any I/O on a CUDA stream at all. QD is bounded by *host threads*:
`cufile_n_reactors = 4` blocking calls (`io/config.hpp:94`, `cufile_reactor.cpp:1189`), further
limited by slice-granular partitioning (a single slice → one reactor; vectored with ≤ 2 slices →
≤ 2 reactors; writes → ≤ 2, `cufile_ioctx.cpp:63-95`). So the plan's QD problem exists, but its
cause is "one blocking call per thread", not "one stream".

Matching the plan means replacing the thread-per-I/O model with async submission, and **also**
sub-slice striping: today `split_write_slices` (`templated_ioctx.hpp:863-896`) exists for writes only,
is off (`write_split_bytes=0`), and reads have no equivalent. A single 1 GiB device read is one slice,
one reactor, sequential 64 MiB `cuFileRead`s (`cufile_reactor.cpp:1181-1217`).

Because the whole process shares 4 reactors, concurrent spills of several data batches
(`default_disk_io_context`, `disk_io_context.cpp:228-240`) queue behind each other rather than
overlap.

### (b) Host-side synchronization where a GPU-side join would do

Three places:
1. **Input gate** polls the caller stream from the host (`cudaStreamQuery`, parked-slice event
   polling every 200 µs; `cufile_reactor.cpp:1122-1156`, `:1745-1791`, `:1910`). A GPU-side
   `cudaStreamWaitEvent(io_stream, ev_recorded_on_user_stream)` removes this wait and the parked-slice
   machinery.
2. **Output**: the caller thread blocks on `future.get()` (`src/cudf/disk_io.cpp:156-157`) and the
   worker host-blocks on staging-slot events (`cufile_reactor.cpp:1386`, `:1427`, `:1724`).
3. **Staging-copy fallback** syncs per slot on the worker thread.

**This is not a drop-in change.** The io_context read contract promises host-complete bytes at
future resolution: *"On success the bytes are in place: no further synchronization of stream is
needed"* (`io/io_context.hpp:493-507`), and the DISK→GPU converter depends on it to skip
`stream.sync()` (`representation_converter_builtins.cpp:1960-1971`). Options:
- (i) keep the contract: resolve the future when the join event is **host-complete** (event query / a
  host callback from the reaper). Gains: no input polling, QD via async. Loses: nothing the
  converters see.
- (ii) amend the contract: resolve when work is **enqueued and joined** on the user stream. Callers
  that read the data from other streams or the host must then wait on the stream themselves. The
  converters mostly already enqueue on the same stream, but the host-side consumers
  (`check_transferred`, benchmarks, `disk_io.cpp`) and any foreign-stream consumer need an audit.
- The **write contract already anticipates async**: a device source may be read by
  "`cuFileWriteAsync` on that stream ... with every argument block / staging buffer kept alive until
  the stream passed it" (`io/templated_ioctx.hpp:227-230`). So (ii) for writes needs no contract change
  other than the "source valid until the future resolves" rule (`io_context.hpp:396-401`), which
  staging actually **relaxes** (the source is free as soon as its D2D copy-in completes).

**Thread-affinity caveat for the join.** For a null or PTDS caller stream, `cudaStreamWaitEvent` /
`cudaEventRecord` against that handle has meaning only on the submitting thread
(`templated_ioctx.hpp:66-77`, `:93`). The event on the user stream must therefore be recorded at submit
time on the caller's thread. The join *after* the I/O must be enqueued on the user's resolved stream
(legacy or stand-in) *after* the reactor has recorded the completion event; CUDA treats a
`cudaStreamWaitEvent` on a not-yet-recorded event as a no-op. That implies either a blocking
"enqueued" handshake in the submit path, or a two-phase API (submit → future-of-event → caller
joins). This is the core API-shape risk, see §6.

### (c) Per-transfer registration of RMM / pool memory, or stale-registration risk

Present but **off by default**: the code paths exist and are tested (`cufile_reactor.cpp:492-539`,
`test_cufile_write.cpp:827-1030`).
- **Stale registration:** it cannot occur across transfers. The registration is released in the
  `transfer_registration` destructor before the request settles, so the caller may free the pool
  allocation as soon as the future resolves (`:513-521`, `:1219-1223`). The table also refuses to
  register a range that overlaps a live registration or an in-flight unregistered transfer
  (`:361-370`).
- **Residual risks:**
  - a range registered *outside* the table (e.g. a future staging ring or an application) triggers
    `CU_FILE_MEMORY_ALREADY_REGISTERED` → `conflict` → POSIX (`:385`, `:1178`);
  - registering an *interior sub-range of a pool chunk* is unverified. cuFile/nvidia-fs registers at
    GPU page granularity [belief], so two adjacent pool allocations may share a page. The table keys
    on exact base address, not pages;
  - `cudaMallocAsync`/VMM destinations may fail `cuFileBufRegister` (handled: unregistered fallback,
    `:374-388`).
- **Plan fit:** the plan ("pool destinations never registered directly") agrees with the default. The
  plan's ring is a fixed, long-lived registration, which is exactly what the table was **not**
  designed for (it assumes transient, per-transfer registrations).

### (d) Batch API without handle pool / cookie reaping

**Not applicable: the batch API is not used at all.** There is no `cuFileBatchIO*` symbol in
`cufile_api` and no reaper thread. Note the loader's rule: *"Every member is required; a library that
lacks one is treated as absent"* (`cufile_api.hpp:34-35`, `cufile_api.cpp:80`). Adding async/batch
symbols as required members would make a libcufile that lacks them look like "no cuFile" and silently
push *everything* to POSIX. They must be optional, with a per-feature capability flag.

### (e) Unbounded or unaligned I/O off the fast path

1. **Sub-threshold device I/O (<256 KiB)** → POSIX buffered pread + pinned scratch + H2D
   (`:1107`, `:1337-1432`). The code documents it as a known gap (≈ 1.3 vs ≈ 6 GB/s,
   `cufile_reactor.hpp:202-205`). The *buffered* fd also thrashes the page cache (`:1396`).
2. **Unregistered destinations** are never on the direct path (by cuFile's own docs). This is the
   default for *all* traffic.
3. **Write head/tail**: always staged D2H + `pwrite` (`:1573`); chunks that are not granule-aligned
   go through the buffered fd (`:1728-1731`).
4. **Compat-mode writes** are entirely staged (`:1577-1580`).
5. **Unbounded op size**: a slice of any size is processed as sequential ≤ 64 MiB pieces by one
   thread (`:1181-1217`); no striping, no cap on total bytes per slice.
6. **Host-only unaligned reads**: through scratch, whole blocks (fine, O_DIRECT-correct).
7. **Host sources**: anything under 4 MiB or not congruent goes through the page cache (`:1542-1559`).
   The measured rationale is a *synchronous-reactor* artifact and would change with async.

### (f) Network-attached storage (QD, NIC affinity, `cufile.json`)

- **QD**: ≤ 4 sync calls per process (§2.8). For a network FS with RTT-dominated latency this is the
  dominant limiter.
- **NIC / NUMA affinity**: none. Reactor threads are not pinned (`cpp:825-828`), there is no
  `rdma_dev_addr_list` or load-balancing policy handling, and there are no `cuFileSetParameter*`
  calls (grep: none). The platform probe classifies network filesystems (`gpfs`, `wekafs`, `nfs`,
  `vastnfs`, `lustre`, `beegfs`; `platform.cpp:163-172`) and checks the per-class `dstatusflags` bit
  (`:359-368`), but this is **advisory** ("Reads never depend on it", `cufile_ioctx.hpp:342-350`) and does not
  influence QD, stream choice or buffer policy.
- **`cufile.json`**: untouched. `cuFileDriverOpen` happens implicitly in `probe_platform` /
  `register_with_cufile` (`cufile_api.cpp:101-116`), which "retains a CUDA context on each visible
  GPU" (`platform.hpp:67-70`). The probe deliberately avoids `cuFileDriverGetP2PFlags` because
  it resets cuFile's running parameters in libcufile 1.19 (`platform.hpp:55-56`). The plan's
  `cuFileSetParameter*` use must account for that quirk and for *ordering relative to the driver
  open that the existing code does eagerly*.
- **Open semantics for network FS**: O_DIRECT may be refused → POSIX path (`:182-197`). Registration
  per file occurs on every open (`:236`).
- **`gds_available()` is global**, so a host where GDS works for one mount class (e.g. GPFS) but the
  spill directory is on another is classified by `dstatusflags != 0` alone for the write decision
  (`:1577`).

### (g) Null-stream handling (commit `469a0221`)

- The fix lives in two layers: `resolve_default_streams` pins null → `cudaStreamLegacy` and
  per-thread → a stand-in non-blocking stream ordered after the submitter's per-thread stream
  (`io/templated_ioctx.hpp:119-156`, applied at `:720`), and the cuFile reactor's gate records a
  **legacy-stream event** because a bare legacy `cudaStreamQuery` does not capture blocking-stream
  dependencies (`cufile_reactor.cpp:1140-1153`).
- **Implications for the plan:**
  1. The fan-out must consume only the resolved stream (`d_buffer.stream`) and the `stream_owner`
     keep-alive (`io/types.hpp:367-370`). In the new design, an I/O stream that waits on a stand-in
     must hold the owner until the wait is enqueued, and the stand-in's async-safe destruction relies
     on CUDA semantics (`templated_ioctx.hpp:73-76`).
  2. **I/O pool streams must be `cudaStreamNonBlocking`** (as the existing copy streams are,
     `cufile_reactor.cpp:1456`), otherwise they implicitly synchronize with the legacy stream and with
     every other blocking stream on the device.
  3. A join targeting `cudaStreamLegacy` (the pinned null stream) is a device-wide barrier on
     blocking streams. This is semantically what the old null-stream contract means, but it serializes
     the GPU. The commit message itself notes the over-ordering and recommends callers pass an explicit
     stream.
  4. Tests that pin this behavior must keep passing: `test/io/test_null_stream_ptds.cpp:142`,
     `test_cufile_reactor.cpp:1459-1495` (per-thread / legacy / null × threshold, i.e. both the cuFile
     and staged paths), and `test_cufile_write.cpp:584`.

### (h) Things the current design does better, or constraints the plan ignores

**Better / worth keeping**
- **The worker never blocks on someone else's GPU work.** Slices behind a busy stream are parked and
  other requests proceed (`cufile_reactor.hpp:177-180`; test `test_cufile_reactor.cpp:854`). The new
  design must not wait for a user stream on a thread that other requests depend on. A GPU-side wait
  achieves this naturally.
- **Strict-with-bounded-service priority scheduling** (`cufile_reactor.cpp:653-743`; tests
  `test_cufile_reactor.cpp:1238-1264`). Ring-slot and I/O-stream allocation must respect the
  priority tiers or demand reads can queue behind readahead.
- **Exactly-once credit settlement and "never resolve while an op can still touch the buffer"**
  (`io_request.hpp:57-66`, `:498-512`; `templated_ioctx.hpp:230-234`). With async I/O the
  completion is no longer in program order, so every success/failure/cancel path needs an explicit
  reaper.
- **Robust object opening**: identity-checked O_DIRECT reopen via the owned fd, retry on
  rename races, `descriptor_mismatch_error` (`cufile_reactor.cpp:168-200`, `:918-960`). Keep it.
- **Shutdown without device-wide syncs**: the pinned scratch goes back to a never-destroyed pool
  because `cudaFreeHost` synchronizes the whole device (`cufile_reactor.hpp:259-261`,
  `cpp:583-588`). A registered staging ring must follow the same rule: `cudaFree` and
  `cuFileBufDeregister` at ioctx teardown would stall behind other callers' GPU work.
- **Per-file POSIX fallback ladder** (§2.9) and run-without-libcufile behaviour
  (`cufile_api.hpp:21-23`). Ring registration failure (BAR1 shortage) must degrade, not fail.
- **Measured per-mode decisions** (staging faster in compat for writes; page-cache + writeback for
  small host writes). They are measured only in compat mode, but they are evidence the new design
  must not regress on non-GDS hosts, which include the T4 CI runner (`AGENTS.md`, Stack).

**Constraints the plan ignores**
1. **io_context completion contract** (host-complete at resolution, `io_context.hpp:493-507`) and the
   write-source-lifetime rule (`:396-401`); see (b).
2. **Handle lifetime**: `cuFileHandleDeregister` happens in `~cufile_io_object`
   (`cufile_reactor.cpp:778-782`). Async ops in flight on a handle when its `io_object` dies are
   undefined. The `grouped_io_request`/`io_op_request` hold `shared_ptr<const io_object>`
   (`io_request.hpp:287`, `:489`), so slot records must hold the object until the I/O's stream point
   passes.
3. **Multi-GPU**: reactors serve any device (`device_id` per slice, copy streams indexed by device,
   `cufile_reactor.hpp:501`, `cpp:1447`); `cuFileBufRegister` and stream pools are per device, so the
   ring and stream pool are per (reactor/ioctx, device). `cuFileDriverOpen` retains a context per
   visible GPU (`platform.hpp:67-70`).
4. **Memory budget**: the repo's thesis is reservation-managed memory tiers
   (`AGENTS.md` Architecture). A 2 GiB `cudaMalloc` staging ring sits outside RMM and the
   `memory_reservation_manager`. It either needs accounting or has to be sized
   conservatively. **T4 (CI runner) BAR1 is small** [belief: 256 MiB], so a 2 GiB registration
   would fail there.
5. **Compat-mode semantics**: async cuFile "runs synchronously in the submitting thread in compat
   mode" (`cufile_reactor.hpp:168-171`). The submit call must therefore never run on a user thread,
   or the whole benefit (and the non-blocking `semi_future` contract) is lost on non-GDS hosts.
6. **Test seams** (`detail.hpp:67-89`) are coupled to the synchronous design:
   `set_cufile_transfer_hook_for_testing` (runs before each `cuFileRead/Write`),
   `set_staging_copy_delay_for_testing`, `set_copy_event_record_failure_for_testing`,
   `cufile_write_calls_for_testing`, `live_registrations_for_testing`. A redesign has to
   re-establish equivalents. Tests that encode sync semantics: stream-wait ordering
   (`test_cufile_reactor.cpp:594`), complete-at-resolve (`:660`), parked-slice liveness
   (`:854`, `:892`, `:1086`), registration-sharing (`test_cufile_write.cpp:827-1030`).
7. **RAII**: existing `owned_stream`, `cuda_event` and `scratch_pool` are RAII with
   exception-safe destruction (`cufile_reactor.cpp:1477-1486`). Ring, slot, and stream-registration
   handles need the same (and `cuFileStreamDeregister` on stream error, per the header).
8. **Thread-safety**: `ioctx` read/write APIs are callable from any thread concurrently, and
   `enqueue` is `noexcept` by contract (`templated_ioctx.hpp:180`, `:791`). A slot allocator that
   blocks on exhaustion must block on the reactor/reaper side, not in `enqueue` or the submitting
   thread's critical section (or it deadlocks the DISK converters, which hold reservations).
9. **The cufile reactor opts out of the prefetching cache**
   (`reactor_traits ... supports_vector_host_read=false`, `cufile_reactor.hpp:522-529`); fragmented
   destinations are rejected (`cpp:1233-1236`). A ring-based reader must not change that.

---

## 6. Risks / open questions for a redesign

1. **Completion contract.** Does the new design keep host-complete futures (cheap, compatible) or move
   to "enqueued + GPU-joined"? The latter changes `io_context.hpp:493-507` and requires auditing
   DISK→GPU and every `device_read*` caller. The decision drives the API (event-returning vs
   future-only).
2. **Cross-thread join ordering.** A `cudaStreamWaitEvent` on a user stream must be issued after the
   reactor has recorded the event on the I/O stream. The reactor runs asynchronously from the submit
   call. Options: block `enqueue`'s caller until recorded (bad, and not `noexcept`-friendly), a
   two-phase API, or join on the reaper side with a host callback. For a PTDS or null stream the join
   must be bound to the submitter's resolved stream, not the reactor thread's.
3. **Async cuFile in compat mode runs synchronously in the submitter** (`cufile_reactor.hpp:168-171`).
   All development and CI hosts without nvidia-fs would run the new path as blocking calls, so QD
   gains are invisible there. We need a real-GDS box to validate. All current defaults were tuned in
   compat mode.
4. **`cuFileReadAsync` bounce semantics.** The `cufile.h` async note says unregistered buffers are
   "buffered through preallocated pinned buffers", whereas `01-...md` §3 describes the cuFile bounce
   pool as GPU-resident (`CuFileSetParameterGpuBounceBufferSlabArray`). Which one applies to *our
   current unregistered reads*, and does it change the value of the registered ring? Verify on the
   target stack.
5. **Registration granularity/semantics.** What does `cuFileBufRegister` do for an *interior* sub-range
   of a larger `cudaMalloc`/pool chunk, and may two registrations share a GPU page? This decides
   whether `register_min_bytes>0` is even safe on pool memory.
6. **BAR1 and ring size** (T4 CI runner; multi-GPU; other processes). Query BAR1 at startup; size the
   ring to a fraction; degrade to unregistered-slot (cuFile bounce) or the existing POSIX path on
   failure. Registration failure must not fail reads.
7. **Reaper design.** The reactor has no reaper (§1.2). Per-slot completion needs a host-visible signal
   (event poll, `cudaLaunchHostFunc`, or `cuFileStreamRegister` + status) to (a) check `bytes_done`
   for short reads and EOF, (b) settle `grouped_coordinator` credits exactly once, (c) release slots
   and keep `io_object` alive, (d) retry the remainder of a short read. Which signal, and on which
   thread?
8. **Cancellation and shutdown.** Async ops already enqueued on streams cannot be cancelled. Shutdown
   must drain/synchronize the I/O streams (cf. `drain_inflight`, `cufile_reactor.cpp:1822-1829`)
   without stalling on other callers' GPU work, and avoid `cudaFree`/`cuFileBufDeregister` of the ring
   at teardown (process-lifetime ring).
9. **Priority and fairness** with slot/stream contention: where does `io_priority::low` wait, and
   how does preemption work when ops are already on a stream?
10. **Interaction with `cuFileDriverOpen` ordering** and `cuFileSetParameter*` (must be set before
    driver open; probe avoids `GetP2PFlags`, `platform.hpp:55-56`). Needs a config path that
    precedes `probe_platform()`.
11. **Network-FS behaviour**: how QD should scale (per ioctx? per mount?), whether to align reactor
    threads / I/O stream pools with NIC NUMA affinity, and which `cufile.json` keys apply for the
    target cuFile version (still unverified from step 1).
12. **Slice-level vs piece-level scheduling.** To use QD 32–64 a single large buffer must be striped
    across ring slots; the existing partitioner (`templated_ioctx.hpp:756-762`) and `coordinator` credits
    (`add_tasks(N-1)`) support expansion, but the expansion must be done by the reactor "after queue
    and slot pressure are known" (`io_context.hpp:514-516`).
13. **Writes in compat/non-GDS hosts.** The staged D2H path is *faster* than `cuFileWrite` in compat
    mode per measurements (`config.hpp:62-65`). A ring + `cuFileWriteAsync` design must keep that
    fallback and the gds/compat decision per file system, not process-wide (`gds_available()`).
14. **Benchmark baselines.** The 80%-of-hardware goal (`AGENTS.md`) is measured against gdsio in
    **compat** mode (6.73 / 13.35 GiB/s, `benchmark_disk_converter.cpp:90-96`). Real-GDS baselines do not
    exist in the repo.
15. **Per-file open overhead** (`open` + `fallocate` + reopen + `cuFileHandleRegister`, per converter
    call). It is amortized for multi-GiB batches but is visible for the 1 MiB benchmark cases.
