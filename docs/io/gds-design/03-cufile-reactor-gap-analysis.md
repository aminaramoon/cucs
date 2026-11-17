# cuFile reactor: gap analysis against the GDS target design

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
