# cuFile / GDS workflow patterns: what we learned

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
