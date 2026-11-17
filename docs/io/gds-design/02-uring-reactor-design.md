# io_uring reactor: design of the pinned-host staging pipeline (read and write)

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
