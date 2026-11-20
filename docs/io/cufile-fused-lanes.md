# Fused stream lanes and host batch reads (cuFile backend, opt-in)

Two opt-in features of the cuFile backend (`cufile_ioctx`, see
[cufile-gds.md](cufile-gds.md)) that keep many reads in flight per thread instead of one:

- **Fused lanes** (`cufile::config::fused_reads`) serve device reads of cuFile-registered files.
  On the submitting thread, neighbouring slices are packed into 4 KiB-aligned windows of up to
  8 MiB. Each window is one `cuFileReadAsync` into a registered slot, followed by
  device-to-device copies into the caller's buffers on the lane's own stream.
- **Host batch** (`cufile::config::host_batch`) serves host reads whose body is 4 KiB-aligned.
  The body goes to `cuFileBatchIOSubmit`, straight into the destination, so one reactor keeps
  up to 64 host reads in flight instead of one blocking `pread`.

Both are **off by default**. With both off, the backend behaves exactly as described in
[cufile-gds.md](cufile-gds.md): the plan, routing, statistics and read paths are unchanged.

Enable fused lanes when device reads come from a network filesystem with GDS (IBM Storage
Scale over RDMA was the reference) and the synchronous pool does not reach the NICs' rate.
Enable host batch when host-memory reads of aligned ranges dominate and one blocking read per
reactor limits them. The design, with the reasoning behind every rule, is
[gds-design/10-fused-lane-reads.md](gds-design/10-fused-lane-reads.md).

One lane, as it runs on the GPU:

```
 lane i = registered 8 MiB slot B_i + reactor-owned stream S_i (cuFileStreamRegister, flags 15)
 S_i runs in FIFO order:
   cuFileReadAsync(file, B_i, size = aligned window, file_offset = window start, buf_offset = 0)
   [cudaStreamWaitEvent(S_i, caller's gate)]        only when the caller's stream was busy
   cudaMemcpyAsync(dst(part 0), B_i + rel0, n0, D2D)
   cudaMemcpyAsync(dst(part 1), B_i + rel1, n1, D2D)
   ...
   cudaStreamAddCallback(done)   -> marks lane i finished, wakes the reactor
 the next read into B_i is issued only after done fired, so a slot is never overwritten early
```

## Table of contents

- [Why this shape](#why-this-shape)
- [How a device read is served with fused lanes](#how-a-device-read-is-served-with-fused-lanes)
- [How a host read is served with host batch](#how-a-host-read-is-served-with-host-batch)
- [Pool layout and routing](#pool-layout-and-routing)
- [Memory and limits](#memory-and-limits)
- [Configuration reference](#configuration-reference)
- [Enabling it](#enabling-it)
- [Observability](#observability)
- [Verifying on a GDS host](#verifying-on-a-gds-host)
- [Known limitations](#known-limitations)

## Why this shape

A raw-cuFile study on a GB200 host measured the read shapes below. The host ran GPFS over
RoCE, with two 400 Gb/s NICs per GPU. Every run used one GPU, 128 reads in flight and
chunks cycling through 1, 2 and 5 MiB. The study is `tpch_scan_bench/gds/docs/fused_async_reads.md`.

| Shape | Payload |
|-------|---------|
| fused 8 MiB `cuFileReadAsync` on registered streams + D2D copy-out, 4 threads x 32 streams | **88.3 GiB/s** (the NICs' line rate) |
| the same, one read per chunk | 76.7 GiB/s |
| slot / lane packing with one read per chunk, events and host callbacks | 45–53 GiB/s |
| host destinations (cuFile batch or io_uring into pinned NUMA-local memory) | 33–35 GiB/s |

Four lessons shaped the feature:

1. **A read has a fixed cost.** At 128 in flight, a read took 4–5 ms almost regardless of its
   size, so fewer, larger reads win. Bytes, memory and copies were not the limit.
2. **One read per stream.** `cuFileReadAsync` blocks its caller while the stream already has a
   cuFile read pending. A lane therefore carries one read at a time, and a thread rotates over
   many lanes.
3. **A few threads, many streams.** Four submitting threads feeding 32 streams each beat one
   thread per read.
4. **Registered and aligned.** Registered slots, registered streams and fully 4 KiB-aligned
   inputs keep every read on the GDS fast path.

## How a device read is served with fused lanes

### Planning, on the submitting thread

`cufile_ioctx` resolves each device slice's GPU and stream gate as it always does. It then calls
`cufile::plan_fused_reads` (`fused_read_plan.hpp`) on the request's slices instead of the legacy
split (`staging_piece_bytes`) and fuse (`config::fuse_gap_bytes`):

- Eligible slices are device-only reads without a completion callback. They are grouped by
  (GPU, stream, gate, priority), and slices of different groups are never fused.
- Inside a group, slices are sorted by file offset. They are packed into windows
  `[align_down(offset), ...)` of at most `slot_bytes`, all ending inside the file's whole
  4 KiB blocks:
  - neighbours at most `fuse_gap_bytes` apart share a window (the gap is read and dropped);
  - overlapping slices never share one;
  - a slice longer than a window is split, and its last piece may share a window with what
    follows.
- A window holding one whole slice stays that slice; several pieces become one fused slice.
- The bytes in the file's partial last block (the sub-block EOF tail) are peeled off as an
  ordinary slice.

Fusion works within one request (one vectored read). Slices of different requests are never
fused.

### Routing and the lane

A request with lane reads for GPU `g` goes to `g`'s lane reactors (see
[routing](#pool-layout-and-routing)). A slice takes a lane when all of these hold:
- fused reads are enabled and usable on the reactor;
- the file is cuFile-registered;
- the slice is one lane read (`is_lane_read`);
- its aligned window is at least `gds_threshold`.

The reactor puts it on a free lane, round-robin over the free lanes so slots and NICs are used
evenly. The lane issues the sequence in the diagram above. Lane reactors issue reads and copies
and return. They never wait for a read. A reactor learns of completions from the stream callback,
reaps every finished lane in one pass, and refills free lanes from its queue. When no lane is free,
requests wait in the reactor's queue, where priorities still apply.

### Completion: host-complete futures

The callback fires after the read and every copy of the lane ran. A successful future therefore
means the bytes are in the destination and visible from any stream. This is the same contract
as the synchronous path. It differs from the async engine (`async_engine.mode = async_auto`, see
[cufile-reactor.md](cufile-reactor.md)), whose futures resolve once the copies are enqueued and
joined onto the caller's stream (AM-1).

### Ordering against the caller's stream

The read never writes the caller's memory; only the copies do. So the read may start while the
caller's stream is still busy, as long as the copies wait for it:

| Caller's gate (event recorded at submission) | What the lane does |
|---------------------------------------------|--------------------|
| none, already passed, or re-dispatched after parking | submit without a gate |
| pending, and fewer than `max_gated_lanes` gated reads in flight on this reactor | submit with the gate: the read starts now, the copies wait on the GPU (`cudaStreamWaitEvent`) |
| pending otherwise | park the slice until the gate fires (holds no lane), then submit |

The `max_gated_lanes` cap keeps a stalled caller from taking every lane.

### Short reads, failures, cooldown, stalls

- **Short read** (`bytes_done` below what the copies need): the window advances by the bytes read,
  rounded down to 4 KiB. It is resubmitted on the same lane at slot offset 0, up to
  `max_resubmits` times. A read that makes no progress, or exhausts its resubmits, fails.
- **Failure:** cuFile refused or failed the read, or a CUDA enqueue or the stream failed. A CUDA
  failure also recycles the lane's stream. The failed slice is re-served through the reactor's
  synchronous path (staged `cuFileRead` or POSIX), and its future fails only if that fails too.
  The failure counts in `lane_failures` and `cufile_failures` and is logged once per file.
- **Cooldown:** after `failure_limit` consecutive failed lane reads, the reactor stops using lanes
  for `cooldown_s` seconds and logs a warning. One success resets the count.
- **Stall warning:** a read in flight for `stall_warn_s` seconds is logged once. Nothing is
  cancelled: the slot stays leased until the stream reports, because cuFile may still write it.
  A hung storage client is otherwise invisible.
- **Cancellation and shutdown:** a cancelled read that is already on a lane settles when the lane
  finishes. `shutdown()` drains every lane before it tears the reactor down.

### What is not served by a lane

| Read | Where it goes |
|------|---------------|
| window below `gds_threshold` (256 KiB), for example a lone small slice | the synchronous reactor's POSIX path (pinned scratch + H2D) |
| file not registered with cuFile (`use_odirect` off, `compat_mode` on, registration failed) | POSIX, as without the feature |
| host+device pairs, slices with a completion callback | POSIX `pread`, unchanged |
| the sub-block EOF tail | POSIX + H2D, as today |
| writes and flushes | the synchronous reactors, unchanged |
| everything while lanes are unusable (no `cuFileReadAsync` in libcufile, no lane granted, cooldown) | the synchronous staged or direct `cuFileRead` |

## How a host read is served with host batch

A host-only read with a contiguous destination and no completion callback, on a
cuFile-registered file, uses the batch path when:
- its aligned body `[align_up(offset), align_down(min(end, file_size)))` is at least `min_bytes`
  (64 KiB);
- its destination is congruent with the file offset (the body's destination address is 4 KiB
  aligned).

Then:

- the head and tail (each under 4 KiB) are read synchronously, before any entry is submitted;
  if either fails the read fails and nothing is submitted;
- the body is cut into entries of at most `max_entry_bytes` (boundaries at multiples of it from
  the body's start), each reading straight into the destination. A reactor keeps up to `depth`
  entries staged or in flight; a read whose entries do not all fit waits in a one-element
  backlog, and the reactor takes no further slices until it is fully staged (like the lane
  backlog), so excess reads wait in the queue where priorities apply;
- the read holds one coordinator credit until its last entry settled. `batch_reads` counts such
  reads and `batch_bytes` their payload including head and tail; they never also count in
  `host_reads`/`host_bytes`, so the two say which path served a read;
- an entry that comes back short or failed is completed synchronously for its missing bytes
  (counted in `batch_failures`, logged once per file). The read fails (`io_error`) only if that
  fails too;
- after `failure_limit` consecutive rejected submits, a failed handle re-creation, or any
  `cuFileBatchIOGetStatus` error, the reactor stops batching for good. It logs this and serves
  host reads synchronously (the rest of a read already in progress included).

Each batch reactor owns one reader with a **helper thread** that makes every `Submit` and
`GetStatus` call: the worker stages entries, hands a wave over after each pass (one hand-off,
no allocation) and settles what the helper published; the helper wakes the worker when a wave
completed. The worker never blocks in cuFile for the batch (in compatibility mode `Submit`
alone takes about 3 ms per wave of 1 MiB entries), so the reactor's one wait point and request
latency are unchanged. The reader is created on the worker at the first eligible read and
destroyed on it at shutdown, after its entries drained; a reactor that never started never
touches cuFile.

Shutdown drops the backlog's unstaged entries, waits for the entries in flight and settles each
read with its real result (no synchronous re-read: a failed entry fails its read with the
shutdown error).

Every other host read (unaligned bodies, incongruent destinations, small reads, fragmented or
cache destinations) takes the existing synchronous path.

libcufile quirks probed with 1.17.1 shaped these rules:

- **Distinct base, zero offset.** A shared `devPtr_base` with a non-zero `devPtr_offset` is
  silently ignored on the aligned path: every entry lands at the base. So each entry passes its
  own destination address as the base, with offset 0.
- **Aligned only.** Entries whose offset, size or buffer are not 4 KiB-aligned take cuFile's
  POSIX pool and sporadically complete `CUFILE_FAILED`.
- **Whole waves only (libcufile 1.17.1 use-after-free).** A `cuFileBatchIOGetStatus` call that
  returns fewer events than the handle has in flight -- a poll that finds nothing, a timeout,
  a partial return with `min_nr` below the outstanding count -- leaves a dangling pointer inside
  cuFile. The next completion dereferences it and crashes the process (SIGSEGV in `GetStatus`)
  as soon as the calling thread's allocator reused that memory. Reproduced through the raw API
  in 5 of 5 runs; it is what crashed every "refill while pending" shape. So the helper submits a
  wave only when the previous one was fully reaped and waits for the whole wave in one call
  (`min_nr` = the wave, 1 s per call). Waves of different readers may overlap safely (measured
  with heap spraying).
- **Finite timeouts.** `cuFileBatchIOGetStatus` is always called with a finite timeout. A
  `GetStatus` without a timeout after a failed submit never returns.
- **Capacity.** Submitting more entries than the handle's `SetUp` capacity fails the whole
  submit. The reader never exceeds `depth`, and `depth` is clamped to cuFile's
  `max_batch_io_size`.

## Pool layout and routing

A **planned pool** (`io_config::cufile_n_reactors = 0`, or `make_cufile_ioctx(0, cfg)`) builds the
following, with roles kept apart so a synchronous write or POSIX fallback never delays lane
refills:

- **per GPU:**
  - `fused_reads.reactors_per_device` lane reactors (4), each owning `lanes_per_reactor` lanes (32);
  - then the synchronous reactors. With fused reads on, their automatic count is 8 (the lanes carry
    the device reads; the synchronous reactors serve writes and fallbacks). An explicit
    `config::reactors_per_device` is kept;
- **shared by every GPU:** `host_batch.reactors` batch reactors (2).

A **flat pool** (`cufile_n_reactors > 0`) gives every reactor every enabled capability.

Routing is a preference; any reactor can serve any slice through the synchronous paths:

| Request | Goes to |
|---------|---------|
| has a lane read for GPU `g` | `g`'s lane reactors; if none, `g`'s group |
| host-only read | the batch reactors; if none, every reactor |
| anything else (writes, flushes, small or mixed reads) | the synchronous reactors of the GPU's group; if none, the group |

The existing fan-out rule and backlog ranking apply among the candidates. A slice counts in
`queued_bytes()` until its lane read or batch entry settles.

Which knobs apply where:

| Knob | Applies to |
|------|------------|
| `config::reactors_per_device` | synchronous reactors per GPU (automatic: 8 with fused reads on; by storage type otherwise) |
| `fused_reads.reactors_per_device`, `lanes_per_reactor` | lane reactors per GPU, lanes per lane reactor (planned pool; a flat pool's reactors each own `lanes_per_reactor` lanes) |
| `fused_reads.slot_bytes`, `fuse`, `fuse_gap_bytes` | lane reads: largest window, fusion, bridged gap |
| `config::staging_piece_bytes`, `config::fuse_gap_bytes` | legacy staged reads only: split size and small-slice fusion. Not applied to device reads when fused lanes are on; the piece size still sizes the synchronous reactors' slots |
| `config::max_staging_bytes_per_device` | synchronous staging only; lanes are budgeted on top (see [memory](#memory-and-limits)) |
| `config::staging_mode`, `staging_slots_per_reactor` | synchronous reactors |
| `config::gds_threshold` | both: smaller lane windows and smaller reads go to POSIX |
| `config::max_inflight`, `poll_interval_us` | synchronous copies; `poll_interval_us` also paces `lane_completion::poll` |
| `config::devices`, `pin_to_device_cpus` | all reactors of the pool |
| `config::storage_hint_path` | sizing of the synchronous count when it is automatic and fused reads are off; warnings |
| `async_engine.mode` | must stay `legacy`: the engine and fused lanes would claim the same reads |
| `host_batch.*` | batch reactors only |

## Memory and limits

- **Registered memory.** Each GPU's lanes take `lanes x (slot_bytes + 64 KiB)` of registered
  device memory, **in addition to** `max_staging_bytes_per_device`, which now bounds only the
  synchronous staging. The default is 128 x (8 MiB + 64 KiB) = **1032 MiB** per GPU. With the
  1 GiB staging cap, that is up to 2056 MiB per GPU. Like all staging memory, it lives outside RMM
  and is never freed, so account for it when sizing the GPU memory pool.
  `cufile::fused_lane_staging_bytes(cfg)` returns the lane share. A flat pool adds it to the
  configured cap.
- **Hardware limits.** Half the GPU's free BAR1 and cuFile's `max_device_pinned_mem_size` (less
  its bounce cache) bound the total. The plan budgets lanes first within them and gives the
  synchronous staging the configured cap within what remains. It warns, naming the limit, when
  that limit cuts lanes ("... holds only K of N fused-read lanes ..., so fewer lanes run") or
  leaves the synchronous reactors fewer staging slots than reactors x `staging_slots_per_reactor`.
  The plan's `staging_cap_bytes` is the synchronous budget plus the lane bytes. On a GDS host a
  lane exists only if its slot registers. In compatibility mode an unregistered slot is accepted.
- **`max_lanes_per_device` (128)** is the process-wide guard per GPU. Lanes are counted over every
  ioctx of the process. A lane set takes lanes only while the process holds fewer than its own
  configured ceiling on that GPU; with different values in different ioctxs, each set applies
  its own. A set that gets fewer lanes runs with fewer, or with none (then the synchronous paths
  serve). The guard exists because the GPFS client on the reference host asserted under
  128 x 1 MiB asynchronous reads (`ShHashV.C`, `nvfs_ioctl -> gpfs_f_aio_read_internal`).
  The fused 8 MiB pattern at the same depth ran cleanly. Do not raise it on GPFS without
  qualifying the client.
- **Host batch depth** is clamped to cuFile's `max_batch_io_size` (`cufile.json`
  `properties.io_batchsize`; `platform_info::max_batch_io_size`). Host reads in flight =
  batch reactors x depth.

## Configuration reference

Every field is checked by the block's `validate()`, which throws `std::invalid_argument` naming
the first invalid field. Nothing is checked while the block is disabled. The pool planner logs an
invalid enabled block as a warning and plans without it.

`cufile::config::fused_reads` (`fused_read_config`):

| Field | Default | Valid | Meaning |
|-------|---------|-------|---------|
| `enabled` | false | | Serve eligible device reads on lanes |
| `reactors_per_device` | 4 | 1–64 | Lane reactors per GPU in a planned pool |
| `lanes_per_reactor` | 32 | 1–64 | Lanes a lane reactor owns per GPU; reads in flight per GPU = reactors x lanes |
| `max_lanes_per_device` | 128 | ≥ 1 | Process-wide ceiling on lanes per GPU (see [memory](#memory-and-limits)) |
| `slot_bytes` | 8 MiB | multiple of 64 KiB, 64 KiB–1 GiB | A lane's registered slot: the largest single read |
| `fuse` | true | | Fuse neighbouring slices; false splits only (each slice costs at least one read) |
| `fuse_gap_bytes` | 0 | < `slot_bytes` | Largest gap bridged inside one read (read and dropped); 0 = contiguous only |
| `register_streams` | true | | `cuFileStreamRegister` lane streams (fixed offsets and size, page-aligned inputs) |
| `completion` | `callback` | `callback`, `poll` | Stream callback, or polled events (a fallback for stacks where callbacks misbehave; more CPU, up to `poll_interval_us` more latency) |
| `max_gated_lanes` | 0 | ≤ `lanes_per_reactor` | Lanes per reactor whose copies may wait on a caller's stream; 0 = half the lanes (at least 1) |
| `max_resubmits` | 8 | any | Short-read continuations of one read before it falls back |
| `failure_limit` | 8 | ≥ 1 | Consecutive failed lane reads before the cooldown |
| `cooldown_s` | 30 | any | Seconds a reactor leaves its lanes unused after tripping |
| `stall_warn_s` | 30 | any | Warn once when a read is in flight this long; 0 = never |

`cufile::config::host_batch` (`host_batch_config`):

| Field | Default | Valid | Meaning |
|-------|---------|-------|---------|
| `enabled` | false | | Serve aligned host-read bodies through the batch API |
| `reactors` | 2 | 1–64 | Batch reactors of a planned pool, shared by every GPU |
| `depth` | 64 | 1–1024 | Entries in flight per reactor (clamped to `max_batch_io_size`) |
| `max_entry_bytes` | 8 MiB | multiple of 4 KiB, 4 KiB–1 GiB | Largest entry; a body is cut into entries of at most this |
| `min_bytes` | 64 KiB | any | Aligned bodies smaller than this keep the synchronous path |
| `poll_us` | 200 | 1–1,000,000 | Longest the worker sleeps with entries in flight without a wake-up from the helper (bounds a lost wake-up only) |
| `failure_limit` | 4 | ≥ 1 | Consecutive rejected submits after which a reactor stops batching for good |
| `submit_policy` | `exclusive` | `exclusive`, `per_reader`, `continuous` | When a reader submits; see below |

`submit_policy` (`batch_submit_policy`):

| Policy | Shape | libcufile 1.17.1, compatibility mode |
|--------|-------|--------------------------------------|
| `exclusive` (default) | whole waves; one wave in flight process-wide (a gate released when the wave completed) | 0 of 5 crash-probe runs crashed |
| `per_reader` | whole waves per reader; waves of different readers overlap (the raw GDS-host benchmark's shape) | 0 of 5 crashed; 7.16 GB/s with 2 batch reactors on the test box (device-bound) |
| `continuous` | refill while entries are pending, reap as they complete (`min_nr` 1) | 5 of 5 crashed |

Qualify anything but `exclusive` on the target stack first with the hidden test
`cucascade_io_tests "[host_batch_quirk]"`, which runs every policy and a raw-API reproducer in
child processes and prints their crash counts. Readers alive at the same time share one
policy: the first ones fix it, and a reactor configured differently logs a warning and uses
it.

## Enabling it

```cpp
#include <cucascade/io/cufile/cufile_ioctx.hpp>
#include <cucascade/io/cufile/placement.hpp>

namespace cufile = cucascade::io::cufile;

cufile::config cfg;
cfg.devices             = {0};                    // the GPUs the process reads into
cfg.storage_hint_path   = "/gpfs/fs1/warehouse/lineitem/part-0.parquet";
cfg.fused_reads.enabled = true;                   // 4 lane reactors x 32 lanes of 8 MiB per GPU
cfg.host_batch.enabled  = true;                   // 2 batch reactors x 64 entries
cfg.fused_reads.validate();                       // throws std::invalid_argument
cfg.host_batch.validate();

auto ctx = cufile::make_cufile_ioctx(0, cfg);     // 0 = plan per GPU
ctx->start();                                     // reserves staging and lane slots
// cufile::describe(ctx->plan()) is what the factory logs
```

Through the registry, set the same fields on `io_config::cufile`
(`config.cufile.fused_reads.enabled = true`). `async_engine.mode` must stay `legacy`.

Starting values:

- **IBM Storage Scale over RDMA (the measured case):** the defaults: 4 lane reactors x 32 lanes,
  8 MiB slots, `fuse_gap_bytes` 0, `max_lanes_per_device` 128, `register_streams` on, `callback`
  completion. Raise `fuse_gap_bytes` only where reading and dropping the gap costs less than a
  read's fixed latency (about 4 ms at load).
- **Local NVMe:** not measured. A read there costs about 0.1 ms, so far fewer lanes should
  suffice, and the synchronous pool may already saturate the drives. Start with 1–2 lane reactors
  x 8–16 lanes, compare against fused reads off with the same file set, and keep the feature
  only if it wins.

`cufile.json` prerequisites (see the [Storage Scale checklist](cufile-gds.md#ibm-storage-scale-checklist)):

- `properties.max_direct_io_size_kb` ≥ `slot_bytes` in KiB (8192 for 8 MiB slots; 16384 is the
  maximum). Otherwise cuFile moves a window as sequential chunks.
- `rdma_dev_addr_list` lists the IPs of the GPU's RDMA ports; it is never empty.
- `use_poll_mode` false on Storage Scale.
- `gpfs` not in a denylist.

## Observability

`cufile_ioctx::stats()` (or `stats(device)`) returns `reactor_stats`. The feature adds these
counters:

| Counter | Meaning |
|---------|---------|
| `lane_reads` | lane reads that succeeded |
| `lane_bytes` | their payload (bytes the callers asked for) |
| `lane_issued_bytes` | bytes their aligned windows covered (payload + alignment + bridged gaps) |
| `lane_parts` | caller pieces those reads served |
| `lane_resubmits` | short-read continuations |
| `lane_failures` | lane reads that failed and were re-served synchronously (also in `cufile_failures`) |
| `lane_gated_reads` | lane reads submitted with a pending caller gate |
| `batch_reads`, `batch_bytes` | host reads served by the batch path, and their payload (head and tail included); disjoint from `host_reads`/`host_bytes` |
| `batch_failures` | entries that came back short or failed (completed synchronously, or failing their read at shutdown) |

Reading them:

- **Fusion ratio** = `lane_parts / lane_reads`. On contiguous Parquet chunks it should be well
  above 1; 1 means nothing fused. Small reads per request, `fuse` off, or gaps above
  `fuse_gap_bytes` keep it at 1.
- **Waste** = `lane_issued_bytes / lane_bytes`. It should be close to 1.0 (1.000x in the measured
  run). A large value means bridged gaps or tiny unaligned slices.
- **Average read** = `lane_issued_bytes / lane_reads`. It should approach `slot_bytes`, because
  reads near the slot size are what reach line rate.
- **Health:** `lane_failures` and `batch_failures` should stay 0. A steady rise means storage
  errors or a broken path; look for the cooldown and "stops batching" warnings.
- **`lane_reads` stays 0:** find where the device reads went.
  - In `staged_reads` / `direct_reads`: lanes are unusable. Check the INFO log for
    "cuFileReadAsync unavailable ...; fused lane reads disabled", for "fused lane set on GPU g
    runs K of N lanes (budget granted B)" (B below N: the lane budget; K below B: slots that
    could not be leased or registered, or streams that could not be created), and the WARN log
    for the cooldown.
  - In `posix_device_reads`: the windows were below `gds_threshold`, or the file is not
    cuFile-registered.
- **`batch_reads` stays 0 with host reads in `host_reads`:** the bodies are unaligned, the
  destinations incongruent, or the bodies below `min_bytes`.

The plan is logged once at INFO, for example (abridged):

```
cufile pool: 1 GPU group(s), 14 reactors (4 lane, 8 synchronous, 2 host-batch), ...,
lane slots of 8192 KiB, 2 host-batch reactors serving every GPU's host reads;
GPU 0 (0000:1B:00.0): 8 synchronous reactors (fused reads on: the lanes carry the device reads,
the synchronous reactors serve writes and fallbacks), pinned to CPUs 0-71 (NUMA node 0), ...,
4 lane reactors x 32 lanes, 128 lane slots of 8192 KiB (1032 MiB registered, on top of the
staging cap); registered staging cap 2056 MiB, lanes included
```

Plan warnings are logged as `cufile: ...` at WARN, as in [cufile-gds.md](cufile-gds.md). The ones
this feature adds are:
- an invalid `fused_reads` / `host_batch` block, planned without;
- BAR1 or the pinned limit cutting lanes;
- lanes leaving the synchronous reactors fewer staging slots.

## Verifying on a GDS host

Check content first, then throughput. `tpch_scan_bench`'s `gds_chunk_read` drives `cufile_ioctx`
with these features over the same files, chunk mix and NIC sampler as the raw measurements.
`--mode cc` reads into device memory and `--mode cc-host` into pinned host memory; the options
and commands are in `tpch_scan_bench/gds/docs/cc_modes.md`.

1. **Content:** a small run with `--verify` and an unaligned chunk size (`--chunk 1001K,3M,5M`),
   expecting `0 mismatches -> ok`. For `cc-host` with batch, use aligned chunks.
2. **Throughput:** the full comparison run (20 files x 1000 MiB, 1/2/5 MiB chunks, 4 x 32 lanes,
   8 MiB slots, 3 passes + 1 warmup). Run it against a `--cc-fused off` baseline on fresh files.
3. **Path checks:**
   - lane reads ≈ 2,500 per pass, with synchronous `staged_reads`/`direct_reads` near 0;
   - NIC traffic only on the GPU's two NICs, about 50/50 and about 1.02x the payload (traffic on
     other NICs means the GDS fast path was not used);
   - cuFile's read-size histogram with every read in the 8–16 MiB bucket;
   - for a definitive answer, a few reads with `CUFILE_LOGGING_LEVEL=TRACE`, then grep
     `cufile.log` for `nvfs stream IO work` and `compat: 0`.

Safety rules of that host:
- one run at a time, with the owner's approval;
- at most 128 reads in flight;
- fresh, unread files for every run (GPFS caching inflates numbers);
- watch `/var/adm/ras/mmfs.log.latest` and stop on `logAssertFailed` or `mmfsd is shutting down`.

### Measured results

To be filled in after the GDS-host runs.

| Run | Median payload | Peak NIC | Lane reads / pass | Fusion ratio | Notes |
|-----|----------------|----------|-------------------|--------------|-------|
| raw `--mode async --fuse`, 4 x 32, 8 MiB (reference) | 88.3 GiB/s | ~99.6 GB/s | 2,500 (raw) | 3.0 | measured 2026-11-20 |
| `cc`, fused lanes 4 x 32, 8 MiB | TBD | TBD | TBD | TBD | |
| `cc --cc-fused off` (synchronous reactors) | TBD | TBD | n/a | n/a | |
| `cc-host`, host batch 2 x 64 | TBD | TBD | n/a | n/a | raw host APIs: 33–35 GiB/s |
| `cc-host --cc-batch off` | TBD | TBD | n/a | n/a | |

## Known limitations

- **Writes are unchanged:** they always take the synchronous reactors.
- **No cross-request fusion:** only slices of one vectored read are fused. Callers that issue one
  chunk per request get one read per chunk (still on lanes, without the fusion gain).
- **The prefetching cache is still refused** by this backend. Slices with a completion callback
  never take a lane or a batch entry.
- **Exclusive with the async engine:** `fused_reads.enabled` with `async_engine.mode != legacy`
  is rejected at reactor construction.
- **Compatibility mode:** lanes still run, and cuFile bounces each read through host memory. A
  lane's slot is registered when cuFile allows it and unregistered otherwise. Throughput there
  has not been measured against the synchronous path.
- **Device destinations do not use the batch API:** on the reference GPFS mount, cuFile batch
  entries for GPU memory always took the POSIX fallback.
- **Host batch needs aligned bodies:** unaligned or incongruent host reads keep the synchronous
  path.
- **Host batch on this test box (local NVMe, compatibility mode):** host reads of 16 MiB, 16 in
  flight, through `cufile_ioctx`: 7.1 GB/s with 1 or 2 batch reactors (any policy) against
  4.5 GB/s for the synchronous reactors of a planned pool and 7.2 GB/s for a flat pool of 4
  synchronous reactors -- the drive's ceiling. One batch reactor reaches it.
