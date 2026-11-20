# Fused stream-lane device reads and cuFile-batch host reads: design

Status: design for the `io/cufile-fused-lanes` branch (on top of `io/cufile-async-engine`).
Normative for the implementation; where the code later disagrees, the code is right and this
file is updated. The operating guide is [`../cufile-fused-lanes.md`](../cufile-fused-lanes.md).

## 1. Why

The per-device async engine (`device_engine`, `mode: async_auto`) did not reach the throughput
the hardware allows. A raw-cuFile study on the GB200 GDS host (GPFS over RoCE, two 400 Gb/s
NICs per GPU; `tpch_scan_bench/gds/docs/fused_async_reads.md`) found the shape that does:

| Shape (128 reads in flight, chunks cycling 1/2/5 MiB) | Payload |
|---|---|
| **fused 8 MiB `cuFileReadAsync` on registered streams + D2D copy-out, 4 threads x 32 streams** | **88.3 GiB/s (NIC line rate)** |
| same, one read per chunk | 76.7 GiB/s |
| slot/lane packing with one read per chunk, events and host callbacks | 45-53 GiB/s |
| host destinations (cuFile batch or io_uring, pinned NUMA-local memory) | 33-35 GiB/s |

What the measurements say:

1. A read costs about 4-5 ms at 128 in flight almost regardless of its size. **Fewer, larger
   reads win**; bytes, memory and copies are not the limit.
2. `cuFileReadAsync` blocks the caller while its stream already has a cuFile read pending.
   **A stream carries one read at a time**, and a thread must rotate over many streams.
3. A few submitting threads (4) feeding many streams (32 each) beat one thread per read.
4. Registered 8 MiB slots, registered streams (`cuFileStreamRegister` flags 15) and fully
   4 KiB-aligned inputs keep every read on the GDS fast path.
5. The benchmark's bookkeeping was naive: a mutex and condition variable per worker, a
   `notify_all` per completion, linear scans for a free lane, an event per slot. It measured
   the I/O shape, not a reactor.

This design carries that shape into `cufile_reactor` with the completion handling of
`uring_reactor`: a fixed slot table, O(1) completion lookup, batch reaping, one wait point,
no locks and no allocation on the completion path.

Separately, host destinations measured the same through cuFile's batch API as through
io_uring, so host reads of the cuFile reactor move from one blocking `pread` per reactor to
the batch API, which keeps many host reads in flight from one thread.

### Facts probed for this design (libcufile 1.17.1, compatibility mode, RTX 6000 Ada)

Probe sources: `/tmp/cucs/probe/probe{,2,3}.cpp` at design time; the tests of this branch pin
the behaviours that matter.

- `cuFileReadAsync` on a registered stream returns in about 30 us and completes later; a
  `cudaStreamAddCallback` queued behind the read and its copies fires after them, with the
  stream's status. `bytes_read` is the bytes available when the read crosses EOF.
- Batch API, host memory:
  - **A shared `devPtr_base` with a non-zero `devPtr_offset` is silently ignored on the aligned
    path: every entry lands at the base** (16 of 16 entries corrupt; same family as AM-9).
    Entries must use a distinct base and a zero offset.
  - Entries whose file offset, size or buffer are not 4 KiB-aligned take cuFile's POSIX pool
    and sporadically complete `CUFILE_FAILED` (1-2 of 16).
  - Semantics are libaio-like: `GetStatus(min_nr, timeout)` returns early with at least
    `min_nr` events or at the timeout with fewer; `min_nr = 0` polls; more submissions are
    accepted while earlier ones are pending as long as the total stays within the `SetUp`
    capacity; exceeding it fails the whole submit (5040), after which a `GetStatus` without a
    timeout never returns.
  - `Submit` of aligned entries can take milliseconds in compatibility mode.

## 2. Scope

In scope:

- **Fused lanes**: device-only reads of cuFile-registered files served by
  `cuFileReadAsync` into per-lane registered slots, fused up to the slot size on the
  submitting thread, copied out device-to-device on the lane's stream, completed through a
  stream callback. Host-complete futures (the generic `device_readv_async_io` contract, not
  AM-1).
- **Host batch**: host-only reads whose body is 4 KiB-aligned in file offset, size and
  destination address served by `cuFileBatchIOSubmit` straight into the destination.
- Pool planning and routing for both, statistics, configuration, tests, documentation.

Out of scope: writes (unchanged), the prefetching cache (still refused by this backend), the
`device_engine` (untouched; mutually exclusive with fused lanes, see §4), the cuFile batch API
for device destinations (it never takes the GDS path on GPFS).

Both features are **off by default**. With both off the reactor behaves exactly as before.

## 3. Architecture

```
 submitting thread (cufile_ioctx)                       reactor worker (cufile_reactor)
 ─────────────────────────────────                      ───────────────────────────────────────────
 device slices                                           ┌── fused_lane_set (per device) ───────────┐
   resolve GPU, record stream gate (unchanged)           │ lane i: registered slot + own stream     │
   plan_fused_reads():                                   │   cuFileReadAsync(window -> slot, off 0) │
     sort by offset, pack runs into aligned              │   [cudaStreamWaitEvent(gate)]            │
     windows <= slot, split long slices,       ──────►   │   cudaMemcpyAsync(part -> dst) x n       │
     peel the sub-block EOF tail                queue    │   cudaStreamAddCallback(done, i)         │
   one prepared_io_slice == one lane read                │ done: completed |= 1<<i; wake()          │
 select_reactors(): lane reactors of the GPU             └──────────────────────────────────────────┘
                                                          reap: bits = completed.exchange(0)
 host slices                                               validate bytes, resubmit short reads,
   select_reactors(): host-batch reactors      ──────►    settle, free lane, refill from backlog
                                                          ┌── host_batch_reader ───────────────────┐
                                                          │ one batch handle, `depth` entries       │
                                                          │ Submit(aligned body -> dst, offset 0)   │
                                                          │ GetStatus(min 0 | min 1, timeout)       │
                                                          └─────────────────────────────────────────┘
                                                          one wait point: `_wake` semaphore, signalled by
                                                          enqueue(), lane callbacks and write wakeups
```

Everything not served by a lane or a batch entry takes the paths the reactor already has
(synchronous cuFile, POSIX through the pinned scratch), on the same worker.

### 3.1 Reactor roles

A reactor may own lanes, a batch reader, both, or neither; the capability is fixed at
construction (`reactor_placement::fused_lanes`, `reactor_placement::host_batch`) and only
honoured when the matching configuration block is enabled.

- A planned pool (`make_cufile_ioctx` with `cufile_n_reactors == 0`) builds, per GPU,
  `fused_reads.reactors_per_device` lane reactors followed by the usual synchronous reactors,
  and `host_batch.reactors` batch reactors that serve every GPU. Roles are exclusive there, so
  a synchronous write or POSIX fallback never stalls lane refills.
- A flat pool (reactor count given) gives every reactor every enabled capability.

Routing is a preference, not a restriction: any reactor can serve any slice through the
synchronous paths. `cufile_ioctx::select_reactors` sends a request to

| Request | Candidates |
|---|---|
| has a lane read for GPU `g` | lane reactors of `g`; if none, `g`'s group |
| host-only read | batch reactors; if none, every reactor |
| anything else (writes, flushes, small or mixed reads) | synchronous reactors of the GPU's group; if none, the group |

and keeps the existing fan-out rule (every candidate for more than two slices, else the two
least backlogged) and backlog ranking. `queued_bytes()` keeps counting a slice until its lane
read or batch entry settles.

## 4. Configuration

Added to `cufile::config` (defaults shown):

```cpp
struct fused_read_config {
  bool enabled{false};
  std::size_t reactors_per_device{4};   // lane reactors per GPU (planned pools)
  std::size_t lanes_per_reactor{32};    // 1..64; in flight per GPU = reactors x lanes
  std::size_t max_lanes_per_device{128};// process-wide ceiling per GPU, all ioctxs together
  std::size_t slot_bytes{8UL << 20};    // registered slot = largest read; multiple of 64 KiB
  bool fuse{true};                      // false: split at the slot size only
  std::size_t fuse_gap_bytes{0};        // gap bridged inside one read (read and dropped)
  bool register_streams{true};          // cuFileStreamRegister(FIXED_* | PAGE_ALIGNED_INPUTS)
  lane_completion completion{lane_completion::callback};  // or ::poll (see §6)
  std::size_t max_gated_lanes{0};       // lanes that may wait on a caller stream; 0 = half
  unsigned max_resubmits{8};            // short-read continuations per read
  unsigned failure_limit{8};            // consecutive failed reads before the cooldown
  unsigned cooldown_s{30};              // lanes unused for this long after tripping
  unsigned stall_warn_s{30};            // warn once when a read is in flight this long; 0 = off
  void validate() const;                // throws std::invalid_argument
};

struct host_batch_config {
  bool enabled{false};
  std::size_t reactors{2};              // batch reactors of a planned pool
  unsigned depth{64};                   // entries in flight per reactor, 1..1024
  std::size_t max_entry_bytes{8UL << 20};  // body cut into entries of at most this; 4 KiB multiple
  std::size_t min_bytes{64UL << 10};    // smaller aligned bodies keep the synchronous path
  unsigned poll_us{200};                // longest the worker blocks in GetStatus
  unsigned failure_limit{4};            // consecutive failed submits before batch is disabled
  void validate() const;
};

struct config {
  ...
  fused_read_config fused_reads{};
  host_batch_config host_batch{};
};
```

Rules:

- `fused_reads.enabled` with `async_engine.mode != legacy` is rejected at reactor construction
  (`std::invalid_argument`): both would claim the same device reads.
- Lanes need `cuFileReadAsync` in the loaded library; `register_streams` additionally needs the
  stream-registration pair. Without them the reactor logs once and serves through the existing
  paths. Batch needs the complete batch group.
- On a host where `gds_available()` is false (compatibility mode) lanes still run, with slots
  registered when cuFile allows it and unregistered otherwise. With GDS, a lane exists only if
  its slot registered.
- `max_lanes_per_device` guards the storage: the GPFS client on the reference host asserted
  under 128 x 1 MiB asynchronous reads. Lanes are counted per GPU over the whole process; a
  lane set takes lanes only while the process holds fewer than its own configured ceiling on
  that GPU (with different values in different ioctxs, each set applies its own). A set that
  cannot get lanes runs with fewer, or with none.
- Registered memory: lanes take `lanes x (slot_bytes + 64 KiB)` per GPU (1032 MiB for the
  default 128 lanes of 8 MiB) from the staging pool, **in addition to**
  `max_staging_bytes_per_device`, which now bounds the synchronous staging only; account for
  it when sizing a GPU memory pool. Half the free BAR1 and cuFile's pinned-memory limit (less
  its bounce cache) bound the total: the plan budgets lanes first within them, gives the
  synchronous staging the configured cap within what they leave, and warns, naming the limit,
  when they cut lanes or leave the synchronous reactors without slots. A plan's
  `staging_cap_bytes` (what the staging pool enforces) is the synchronous budget plus the
  lane bytes; a flat pool adds `fused_lane_staging_bytes(cfg)` to the configured cap.

## 5. Planning fused reads (`fused_read_plan.hpp`)

```cpp
struct fused_plan_options {
  std::size_t window_bytes;    // lane slot size, a multiple of IO_BLOCK_SIZE
  std::size_t gap_bytes{0};
  bool fuse{true};
  std::size_t min_read_bytes{0};  // aligned windows smaller than this stay ordinary slices
};
struct fused_plan_stats {
  std::size_t reads{0};         // lane reads produced
  std::size_t parts{0};         // caller pieces they serve
  std::size_t split_slices{0};  // input slices cut across reads
  std::size_t tail_slices{0};   // sub-block EOF tails peeled off
  std::size_t payload_bytes{0}; // bytes the caller asked for, in lane reads
  std::size_t issued_bytes{0};  // bytes the lane reads cover (aligned windows)
};

/// Rewrite the lane-eligible device slices of one request, in place, as lane reads.
/// Strong guarantee: on an exception `slices` is unchanged.
fused_plan_stats plan_fused_reads(std::vector<prepared_io_slice>& slices,
                                  std::size_t file_size,
                                  fused_plan_options const& options);

/// Whether `slice` is exactly one lane read for a slot of `window_bytes`.
[[nodiscard]] bool is_lane_read(prepared_io_slice const& slice,
                                std::size_t file_size,
                                std::size_t window_bytes) noexcept;

/// The 4 KiB-aligned window a lane read issues: [align_down(offset), align_up(end)).
[[nodiscard]] range lane_window(prepared_io_slice const& slice) noexcept;
```

Eligible input slice: a read, device destination only, no host buffer, no `on_complete`,
`ready_gate_set`, not already fused, non-empty, starting inside the file. Everything else
passes through untouched, in its original relative order.

Algorithm (per group of eligible slices with equal device, stream handle, gate pointer and
priority, sorted by offset; `body_end = align_down(file_size)`):

```
window = none
for slice in group:
  pos, end = slice.offset, min(slice.end, file_size)
  while pos < end:
    if pos >= body_end:                      # the file's partial last block
      emit ordinary slice [pos, end)         # served by POSIX + H2D, as today
      break
    if window and (not fuse or pos < window.data_end or pos - window.data_end > gap
                   or pos >= window.limit):
      close(window)
    if not window:
      window.start = align_down(pos)
      window.limit = min(window.start + window_bytes, body_end)
    take = min(end, window.limit) - pos      # > 0 here
    window.parts += {range{pos, take}, slice.data + (pos - slice.offset)}
    window.data_end = pos + take
    pos += take
    if window.data_end == window.limit: close(window)
close(window)
```

`close` emits one `prepared_io_slice`: the original slice moved as is when the window holds
exactly one whole input slice, otherwise a fused slice (`rng` from the first part's offset to
the last part's end, `fused` = the parts, `d_buffer` = the group's with `data` of the first
part, same priority). A window smaller than `min_read_bytes` is emitted as its parts' ordinary
slices instead. Post-conditions, which `is_lane_read` checks and the reactor relies on:

- `lane_window(slice).size <= window_bytes`, and the window ends at or before `body_end`;
- parts are sorted, non-overlapping and inside `rng`;
- the sum of emitted payload equals the eligible input payload clamped to the file.

The legacy split (`staging_piece_bytes`) and fuse (`fuse_gap_bytes`, parts of at most half a
piece) are not applied when fused lanes are on: this planner replaces both.

## 6. The lane set (`src/io/cufile/fused_lanes.hpp`)

One per reactor and device, created on the worker thread with that device current, on the
first lane read for it (the ioctx reserves the slots at `start()`, so creation only leases).
Worker-thread only, except the completion callback.

```cpp
namespace cucascade::io::cufile::detail {

struct lane_copy { std::uint8_t* dst; std::size_t offset; std::size_t bytes; };  // file offset

struct lane_read {
  void* handle{};                    // CUfileHandle_t
  range window;                      // aligned, <= slot
  std::vector<lane_copy> copies;     // ascending file offset, inside the window
  cudaEvent_t gate{};                // event the copies must follow, or null
  std::uint64_t token{};             // the owner's, returned in the result
};

struct lane_result {
  enum class status : std::uint8_t { ok, io_failed, device_failed };
  std::uint64_t token{};
  status outcome{status::ok};
  ssize_t io_result{};               // last bytes_done (negative: cuFile error)
  cudaError_t cuda_status{cudaSuccess};
  unsigned resubmits{};
};

struct lane_set_options {
  int device{};
  std::size_t lanes{};               // wanted; 1..64
  std::size_t slot_bytes{};
  std::size_t staging_cap_bytes{};   // staging_pool cap for this device
  std::size_t max_device_lanes{};    // process-wide budget
  bool register_streams{true};
  lane_completion completion{lane_completion::callback};
  unsigned max_resubmits{8};
  void (*wake)(void*) noexcept {};   // called from the completion callback
  void* wake_context{};
};

class fused_lane_set {
 public:
  explicit fused_lane_set(lane_set_options const& options);   // never throws; capacity() may be 0
  ~fused_lane_set();                                          // requires in_flight() == 0

  [[nodiscard]] std::size_t capacity() const noexcept;
  [[nodiscard]] std::size_t in_flight() const noexcept;
  [[nodiscard]] std::size_t free_lanes() const noexcept;
  [[nodiscard]] std::size_t gated() const noexcept;           // in flight with a gate
  [[nodiscard]] std::size_t slot_bytes() const noexcept;

  /// Issue `read` on a free lane. False when none is free (`read` untouched). An enqueue
  /// failure is reported by a later reap(), after the lane is known idle.
  [[nodiscard]] bool submit(lane_read&& read);

  /// Report every lane that finished since the last call; never blocks. `done(result)`.
  template <typename Done> std::size_t reap(Done&& done);

  /// Wait until nothing is in flight (shutdown), reporting as reap() does.
  template <typename Done> void drain(Done&& done);

  [[nodiscard]] bool completions_pending() const noexcept;    // callback fired, not reaped
  [[nodiscard]] std::chrono::steady_clock::duration oldest_in_flight() const noexcept;
};
}
```

Lane state (stable addresses for the set's lifetime): the staging slot, an owned
non-blocking stream (registered with flags 15 when asked and available), the four arguments
cuFile reads at execution (`size`, `file_offset`, `buffer_offset = 0`, `bytes_done`), the
callback context `{set, index, status}`, the read in progress and its resubmit count.

`submit` on lane `i` (round-robin over the free mask, so slots and therefore NICs are used
evenly under partial load):

1. fill the arguments; `bytes_done` starts at a sentinel;
2. `api->read_async(handle, slot.base, &size, &file_offset, &buffer_offset, &bytes_done, stream)`;
3. `cudaStreamWaitEvent(stream, gate)` when gated;
4. one `cudaMemcpyAsync(dst, slot.base + (offset - window.offset), bytes, D2D, stream)` per copy;
5. `cudaStreamAddCallback(stream, on_done, &lane.callback, 0)`.

If step 2 fails nothing was queued: the lane is marked finished with `io_failed` at once. If
step 3, 4 or 5 fails the read is already queued: the set synchronizes the lane's stream
(blocking, rare) and reports `device_failed`.

`on_done(stream, status, ctx)` runs on a CUDA thread: store `status`, set bit `i` of
`_completed` (`fetch_or`), call `wake`. No CUDA call, no lock, no allocation.
`cudaStreamAddCallback` is used rather than `cudaLaunchHostFunc` because it is also invoked,
with the error, when the stream fails; a host function would never run and the lane would
hang.

`lane_completion::poll` is a field fallback for a stack where stream callbacks misbehave:
step 5 records the lane's event instead, `reap` queries the events of the busy lanes, and
the reactor polls at `poll_interval_us` while lanes are in flight. Same results, more CPU and
up to one poll interval of refill latency.

`oldest_in_flight()` reports how long the oldest read has been in flight; the reactor logs a
warning once per read that exceeds `stall_warn_s` (a hung storage client is otherwise
invisible). Nothing is cancelled: the slot stays leased until the stream reports.

`reap` takes `_completed.exchange(0)` and for each bit:

- callback status not `cudaSuccess` → recycle the lane's stream (deregister, destroy, create,
  register), `device_failed`;
- `bytes_done < 0` → `io_failed`;
- `bytes_done >= needed`, where `needed` is the end of the last copy relative to the window →
  `ok`;
- otherwise a short read: `progress = align_down(bytes_done)`. With `progress == 0` or the
  resubmit budget spent → `io_failed`. Else advance the window by `progress`, clip the copies
  to it and resubmit on the same lane, **into the slot at offset 0** (never a non-zero buffer
  offset) and without the gate (it has passed); the lane stays busy.

Slots come from `detail::staging_pool` (`acquire(device, slot_bytes, cap, require_registered =
gds_available())`), go back to it on destruction, and are never freed. A process-wide
per-device counter enforces `max_device_lanes`.

Invariants:

- one read at a time per lane, so `cuFileReadAsync` never blocks behind a pending read;
- a slot is rewritten only after the callback of its previous read fired, which is after its
  copies ran;
- every input of every read is 4 KiB-aligned and inside the file's whole blocks;
- the set outlives every callback: it is destroyed only when idle.

## 7. The host batch reader (`src/io/cufile/host_batch.hpp`)

```cpp
namespace cucascade::io::cufile::detail {

struct batch_read {
  void* handle{};          // CUfileHandle_t
  std::uint8_t* dst{};     // 4 KiB-aligned host address; passed as devPtr_base, offset 0
  range rng;               // 4 KiB-aligned file range, inside the file's whole blocks
  std::uint64_t token{};
};
struct batch_result {
  enum class status : std::uint8_t { ok, short_read, failed };
  std::uint64_t token{};
  status outcome{status::ok};
  std::size_t bytes{};     // delivered (ok: rng.size)
  int error{};             // cuFile status / error for diagnostics
};

class host_batch_reader {
 public:
  host_batch_reader(unsigned depth, unsigned failure_limit);  // never throws; available() false on failure
  ~host_batch_reader();                                       // requires in_flight() == 0
  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] std::size_t in_flight() const noexcept;
  [[nodiscard]] std::size_t free_entries() const noexcept;    // capacity - in flight - staged
  /// Stage one entry for the next flush(). Requires free_entries() > 0.
  void add(batch_read const& read) noexcept;
  /// Submit the staged entries in one call. Rejected entries are reported by reap().
  void flush() noexcept;
  /// Collect completions; waits up to `wait` for the first one when it is non-zero.
  template <typename Done> std::size_t reap(std::chrono::microseconds wait, Done&& done);
  template <typename Done> void drain(Done&& done);
};
}
```

- One batch handle of `depth` entries (clamped to cuFile's `max_batch_io_size`); fixed
  parameter, event and bookkeeping arrays; the cookie is the entry index. Outstanding plus
  staged never exceeds the capacity.
- `GetStatus` is always called with a finite timeout: zero for a poll, `wait` otherwise.
- An event is `ok` when `status == CUFILE_COMPLETE` and `ret == size`; `short_read` when
  complete with fewer bytes; `failed` otherwise.
- A rejected `Submit` fails every entry of that call (reported through `reap`). After
  `failure_limit` consecutive rejected submits, or any `GetStatus` error, the reader cancels
  and destroys its handle once nothing is in flight and reports `available() == false` for
  good; the reactor then uses the synchronous path.
- `drain` polls with a bounded timeout until every outstanding entry reported, then destroys
  the handle. It never abandons a destination cuFile may still write.

## 8. Reactor integration (`cufile_reactor`)

### 8.1 One wait point

The worker stops blocking in `_requests.wait_dequeue_timed`. It waits on
`lightweight_semaphore _wake`, with `std::atomic<bool> _wake_pending` coalescing signals:

```cpp
void notify_worker() noexcept {            // any thread, including CUDA callback threads
  if (!_wake_pending.exchange(true, std::memory_order_acq_rel)) _wake.signal();
}
```

`enqueue()` (after the request is visible), lane callbacks, the write wakeup and `shutdown()`
call it. Before sleeping the worker clears `_wake_pending`, drops stale permits, re-checks
every source (queue non-empty, lane completions pending) and only then waits. A notification
that races the check either leaves the flag set with a permit, or is caught by the re-check.
Requests are taken with `try_dequeue` only.

### 8.2 Worker pass

```
poll_completions()                    # unchanged: parked gates, H2D copies, early staged reads
reap lanes                            # settle, fall back, free; then refill from the backlog
reap host batch (poll)
... cancellation checks, preempt_low (unchanged) ...
for slices of the active request, while the pass budget lasts and no backlog is pending:
    dispatch_slice()                  # may start a lane read / stage batch entries / run sync
flush host batch
if nothing more can be dispatched now:
    choose the wait:
      batch in flight and no lane in flight   -> batch reap(min(timeout, poll_us))
      otherwise                               -> _wake.wait(timeout)
    timeout = idle_wait_us when nothing is in flight or parked,
              poll_interval_us while legacy copies / parked slices need polling
              (also while batch entries fly together with lanes),
              1 ms while only lanes are in flight (a lost wake-up costs at most this)
```

A slice that is a lane read but finds no free lane is parked in a one-element lane backlog and
the pass stops expanding requests until it is issued; the same for a host slice whose entries
do not all fit (its cursor is kept). This is the reactor's backpressure: the queue, not the
slot table, holds the excess, and priorities keep working because requests stay in the queue.

### 8.3 Lane path in `dispatch_slice`

A slice takes a lane when: fused reads are enabled and usable on this reactor; the slice is a
read with a device destination only and no completion callback; the file is cuFile-registered;
`is_lane_read(slice, file_size, slot_bytes)`; and its window is at least `gds_threshold`.

Gate handling (the copies write the caller's memory, the read does not):

| Gate | Action |
|---|---|
| ready, or re-dispatched after parking | submit without a gate |
| pending, and `gated() < max_gated_lanes` | submit with the gate: the read starts now, its copies wait on the GPU |
| pending otherwise | `park_on_gate` (existing); re-dispatched when the gate fires |
| unchecked (no submission-time gate) | `park_if_stream_busy` (existing), else submit |

Completion (`reap` result) for the slice's `io_op_request`:

- `ok` → `finish_success()`; counters `lane_reads`, `lane_bytes`, `lane_issued_bytes`,
  `lane_parts`, `lane_resubmits`, `lane_gated_reads`.
- `io_failed` / `device_failed` → `lane_failures` and `cufile_failures`, log once per file,
  then **fall back**: the slice's destinations are re-served through the synchronous path of
  this reactor (staged `cuFileRead` or POSIX), each taking a coordinator credit before the
  slice returns its own, exactly as the existing "fused slice that cannot be staged" path does.
  The gate has passed by then. A fallback that also fails settles the read with its error.
- After `failure_limit` consecutive failures the reactor stops using lanes for `cooldown_s`
  seconds and logs a warning; a success resets the count.
- A cancelled coordinator does not stop a read already in flight; it settles when the lane
  finishes, with the cancellation.

Shutdown: cancel the backlogs (nothing was issued), `drain` the lane sets and the batch reader,
settle with real results, destroy both on the worker, then run the existing teardown.

### 8.4 Host batch path in `dispatch_slice`

A host-only read with a contiguous destination and no completion callback, on a
cuFile-registered file, whose aligned body `[align_up(offset), align_down(min(end, file_size)))`
is at least `min_bytes` and whose destination is congruent (`dst + (body.offset - offset)` is
4 KiB-aligned) is served as: head and tail (each under 4 KiB) through the existing
`read_host_direct`, synchronously; the body as entries of at most `max_entry_bytes` reading
straight into the destination. Each entry takes its own coordinator credit. An entry that comes
back `short_read` or `failed` is completed synchronously with `read_host_direct` for the
missing bytes (counted in `batch_failures`); only if that fails does the read fail.
Counters: `batch_reads`, `batch_bytes`, `batch_failures`.

## 9. Pool plan and ioctx

- `device_group_plan` gains `lane_reactors`, `lanes_per_reactor`, `lane_slots`;
  `pool_plan` gains `host_batch_reactors`. With `fused_reads.enabled` the automatic
  synchronous reactor count per GPU is `local_reactors_per_device` (8) unless
  `reactors_per_device` is set: lanes carry the device reads, the synchronous reactors serve
  writes and fallbacks.
- `cufile_ioctx` builds reactors in the order lane, synchronous (per GPU), then batch, records
  the ranges, reserves lane slots at `start()` and routes as in §3.1.
- `prepare_device_slices` resolves GPUs and gates as today, then calls `plan_fused_reads`
  instead of the legacy split and fuse when fused reads are enabled and usable.
- `reactor_stats` gains the counters named above; `cufile_ioctx::stats()` sums them.

## 10. Tests

All in `cucascade_io_tests`, tagged `[cufile]` plus the tag shown. They run in compatibility
mode on a host without GDS; the GDS host re-runs them on GPFS.

| File | Tag | Covers |
|---|---|---|
| `test_fused_read_plan.cpp` | `[fused_plan]` | packing, gaps, splits, EOF tail, groups by stream/gate, pass-through, min size, invariants on random inputs, exception safety |
| `test_fused_lanes.cpp` | `[fused_lanes]` | lane set against libcufile: content, concurrency up to capacity, round-robin, gate ordering, short-read resubmission and failures through a scripted `cufile_api`, callback failure, budget, drain |
| `test_host_batch.cpp` | `[host_batch]` | batch reader against libcufile: content at distinct bases, capacity, scripted failures (rejected submit, failed and short entries, `GetStatus` error), drain |
| `test_fused_ioctx.cpp` | `[fused_ioctx]` | end to end through `cufile_ioctx`: random vectored device reads content-checked against the file, EOF, busy caller stream, cancellation and shutdown with reads in flight, fallback after injected failures, cooldown, host batch reads on every alignment, routing by role, statistics, legacy behaviour unchanged when off |

## 11. Validation on the GDS host

`tpch_scan_bench` `gds_chunk_read` gains modes that drive `cufile_ioctx` with fused lanes and
host batch over the same files, chunk mix and NIC sampler as the raw modes, so the numbers are
comparable line by line with the 88.3 GiB/s raw result. Rules of that host apply: one run at a
time, at most 128 reads in flight, fresh files, watch `mmfs.log.latest`.
