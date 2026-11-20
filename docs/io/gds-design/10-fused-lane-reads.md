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
   The raw shape used 4 x 32 because its threads block inside cuFile. A non-blocking reactor
   wants more submitters: on a real GDS path `cuFileReadAsync` costs its caller a fraction of
   a millisecond per call (it submits to the kernel there: 0.28 ms per call measured at
   4 x 32, 0.62 ms at 8 x 16; about 0.03 ms in compatibility mode), so a reactor feeding 32
   lanes is busy submitting about 65% of the time and its lanes queue behind it. Configuration
   comparison on the same host in one hour (2026-11-20, 11:30-11:50 UTC, raw reference 79.0
   GiB/s): 8 lane reactors x 16 lanes, polled, 84.3 GiB/s; 4 x 32 polled 79.7 (mean lane
   cycle 9.8 ms); 4 x 32 with callbacks 65.1 (15.1 ms). The defaults are therefore 8 x 16. The
   final build with them measured 86.6 GiB/s (mean lane cycle 8.5 ms, lane reactors about 69%
   busy) against 84.4 for the raw reference minutes later.
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
  - **A `GetStatus` that returns fewer events than are in flight (a poll finding nothing, a
    timeout, a partial return) leaves a dangling pointer inside cuFile** (use-after-free): the
    next completion crashes the process once the calling thread's allocator reused the memory.
    Found during the integration (§7); it is what crashed every refill-while-pending shape. The
    wave policies reap each wave in one call with a one-hour timeout.

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
  std::size_t reactors_per_device{8};   // lane reactors per GPU (planned pools); see §1 item 3
  std::size_t lanes_per_reactor{16};    // 1..64; in flight per GPU = reactors x lanes = 128
  std::size_t max_lanes_per_device{128};// process-wide ceiling per GPU, all ioctxs together
  std::size_t slot_bytes{8UL << 20};    // registered slot = largest read; multiple of 64 KiB
  bool fuse{true};                      // false: split at the slot size only
  std::size_t fuse_gap_bytes{0};        // gap bridged inside one read (read and dropped)
  bool register_streams{true};          // cuFileStreamRegister(FIXED_* | PAGE_ALIGNED_INPUTS)
  lane_completion completion{lane_completion::poll};  // or ::callback (see §6)
  std::size_t max_gated_lanes{0};       // per lane set: two-phase reads at once; 0 = half
  unsigned max_resubmits{8};            // short-read continuations per read
  std::size_t max_parts_per_read{256};  // caller pieces per fused read; >= 1
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
  unsigned poll_us{200};                // bounds a lost helper wake-up (worker never blocks in cuFile)
  unsigned failure_limit{4};            // consecutive failed submits before batch is disabled
  batch_submit_policy submit_policy{batch_submit_policy::per_reader};  // per_reader | exclusive | continuous (§7)
  std::size_t max_wave_bytes{1UL << 30};    // most bytes per wave; 0 = no cap (§7)
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
  std::size_t window_bytes{0}; // lane slot size, a non-zero multiple of IO_BLOCK_SIZE
  std::size_t gap_bytes{0};
  bool fuse{true};
  std::size_t min_read_bytes{0};  // aligned windows smaller than this stay ordinary slices
};
struct fused_plan_stats {
  std::size_t reads{0};         // lane reads produced
  std::size_t parts{0};         // caller pieces they serve
  std::size_t split_slices{0};  // input slices emitted as more than one piece (tails included)
  std::size_t tail_slices{0};   // sub-block EOF tails peeled off
  std::size_t payload_bytes{0}; // bytes the caller asked for, in lane reads
  std::size_t issued_bytes{0};  // bytes the lane reads cover (aligned windows)
  bool operator==(fused_plan_stats const&) const = default;
};

/// Rewrite the lane-eligible device slices of one request, in place, as lane reads.
/// Strong guarantee: on an exception `slices` is unchanged.  Throws std::invalid_argument
/// for a zero or unaligned window_bytes, or gap_bytes >= window_bytes.
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

As implemented:

- Emit order: every slice that is not eligible first, in its original relative order, then
  the planned slices group by group (groups ordered by their key), in file order inside a
  group. When nothing is eligible `slices` is not touched.
- The open window is closed before an EOF tail is emitted, so output stays in file order. A
  tail that covers a whole input slice is that slice moved as is. A window of exactly one whole
  input slice is moved as is too (every field kept). Built slices carry only `rng`,
  `d_buffer` (copied from the first part's input slice), `priority` and `kind = read`.
- A window below `min_read_bytes` emits whole inputs as is and split pieces as plain slices.
- The stats count lane reads only: tails and the parts of windows below `min_read_bytes` do
  not count in `reads`, `parts`, `payload_bytes` and `issued_bytes`.
- `is_lane_read` additionally requires `rng` to lie inside `[0, body_end)` (no saturated
  ends) and every fused part to be non-empty with a destination. It does not check
  `ready_gate_set` or `gds_threshold`; the reactor checks the threshold.

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
  std::vector<lane_copy> copies;     // ascending file offset, inside the window; empty with hold
  bool hold{false};                  // two-phase: read only, report `held`, wait for the owner
  std::size_t needed_bytes{};        // with hold: bytes of the window that must arrive
  std::uint64_t token{};             // the owner's, returned in the result
};

struct lane_result {
  enum class status : std::uint8_t { ok, io_failed, device_failed };
  std::uint64_t token{};
  status outcome{status::ok};
  ssize_t io_result{};               // last bytes_done (negative: cuFile error)
  cudaError_t cuda_status{cudaSuccess};
  unsigned resubmits{};
  unsigned lane{};                   // the lane that served it (diagnostics, tests)
  bool held{};                       // a hold read's I/O is done; the lane keeps the data
  std::int64_t completed_ns{};       // callback mode: when the completion callback ran
};

struct lane_set_options {
  int device{};
  std::size_t lanes{};               // wanted; at most 64
  std::size_t slot_bytes{};          // a multiple of 4 KiB
  std::size_t staging_cap_bytes{};   // staging_pool cap for this device (0 = none)
  std::size_t max_device_lanes{};    // process-wide budget: lanes are granted only while the
                                     // device's lanes in use are below this set's own value
  bool register_streams{true};
  lane_completion completion{lane_completion::callback};
  unsigned max_resubmits{8};
  void (*wake)(void*) noexcept {};   // called from the completion callback, and when a
                                     // read fails at submission
  void* wake_context{};
  std::atomic<std::uint64_t>* read_async_ns{};  // time inside cuFileReadAsync (instrumentation)
};

class fused_lane_set {
 public:
  explicit fused_lane_set(lane_set_options const& options) noexcept;  // capacity() may be 0
  ~fused_lane_set();                                          // requires in_flight() == 0

  [[nodiscard]] std::size_t capacity() const noexcept;
  [[nodiscard]] std::size_t in_flight() const noexcept;
  [[nodiscard]] std::size_t free_lanes() const noexcept;
  [[nodiscard]] std::size_t held() const noexcept;            // held, waiting for the owner
  [[nodiscard]] std::size_t active() const noexcept;          // work queued (in flight - held)
  [[nodiscard]] std::size_t slot_bytes() const noexcept;
  [[nodiscard]] int device() const noexcept;
  [[nodiscard]] std::size_t registered_streams() const noexcept;  // diagnostics

  /// Issue `read` on a free lane. False when none is free (`read` untouched). A refused
  /// read, an enqueue failure or a malformed read (unaligned, larger than the slot, copies
  /// outside the window) is reported by a later reap(), after the lane is known idle.
  [[nodiscard]] bool submit(lane_read&& read) noexcept;

  /// Second phase of a held read: queue its copies (then its completion) on the lane, or
  /// drop its data and free the lane.  Exactly one of them per held result.
  [[nodiscard]] bool copy_out(unsigned lane, std::vector<lane_copy>&& copies) noexcept;
  void release_held(unsigned lane) noexcept;

  /// Report every lane that finished since the last call; never blocks. `done(result)`.
  template <typename Done> std::size_t reap(Done&& done);

  /// Wait until no lane has work queued (shutdown), reporting as reap() does.  Held lanes
  /// have nothing queued and are not waited for: they are the owner's to release.
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
3. unless `hold`: the copies -- one `cudaMemcpyAsync(dst, slot.base + (offset - window.offset),
   bytes, D2D, stream)` per copy, or one `cudaMemcpyBatchAsync` (`cuda::device_copy_batch`) for
   a window with five or more;
4. `cudaStreamAddCallback(stream, on_done, &lane.callback, 0)`.

**A lane stream never waits on another stream's event.** A read whose destinations may not be
written yet (its caller's stream has not reached the gate) is submitted with `hold`: steps 1, 2
and 4 only.  When its I/O is done (`bytes_done >= needed_bytes`) `reap` reports it with
`held = true` and the lane stays reserved, slot intact.  The owner then calls exactly one of
`copy_out(lane, copies)` (steps 3 and 4 now; a final result follows) or `release_held(lane)`.
So `drain`, shutdown and cancellation never wait for a caller's stream.  A held read that came
back short is reported held after its first part; `copy_out` copies the bytes already in the
slot, then reads the rest of the window into the slot (offset 0) behind those copies, then
queues the remaining copies.

If step 2 fails nothing was queued: the lane is marked finished with `io_failed` at once. If
step 3 or 4 fails the read is already queued: the set synchronizes the lane's stream (its own
work only), gives it a fresh stream and reports `device_failed`.

`on_done(stream, status, ctx)` runs on a CUDA thread: store `status`, set bit `i` of
`_completed` (`fetch_or`), call `wake`. No CUDA call, no lock, no allocation.
`cudaStreamAddCallback` is used rather than `cudaLaunchHostFunc` because it is also invoked,
with the error, when the stream fails; a host function would never run and the lane would
hang.

`lane_completion::poll` is the default (measured at raw parity on GB200 + GPFS, where callback
completion was about 5% slower): the last step records the lane's event instead, `reap` queries
the events of every busy lane, and the reactor polls at `poll_interval_us` while lanes are in
flight.  (Skipping young lanes was tried and removed: the observed read time then feeds back
into the skip window, so a storage stall delays the following reads too.) Callback mode costs less
CPU but depends on CUDA's single host-callback thread: anything blocking it stalls every lane
completion.

`oldest_in_flight()` reports how long the oldest read has been in flight; the reactor logs a
warning once per read that exceeds `stall_warn_s` (a hung storage client is otherwise
invisible). Nothing is cancelled: the slot stays leased until the stream reports.

`reap` takes `_completed.exchange(0)` and for each bit:

- callback status not `cudaSuccess` → retire the lane's stream and create a new one (a lane
  that cannot get one leaves the set: `capacity()` shrinks), `device_failed`;
- `bytes_done < 0` → `io_failed`;
- `bytes_done >= needed`, where `needed` is the end of the last copy relative to the window →
  `ok`;
- otherwise a short read: `progress = align_down(bytes_done)`. With `progress == 0` or the
  resubmit budget spent → `io_failed`. Else advance the window by `progress`, clip the copies
  to it and resubmit on the same lane, **into the slot at offset 0** (never a non-zero buffer
  offset); the lane stays busy.  (A `hold` read is reported held instead; see above.)

`drain` also stops waiting for a lane whose stream reports an error without its completion
arriving (a sticky error stops callbacks): it is reported `device_failed`.

Lane streams are **retired, never deregistered or destroyed** (kept, registered and idle, for
the life of the process; one small object per lane of every set built), because of what
libcufile 1.17 does in compatibility mode:

- `cuFileStreamDeregister` synchronizes the whole device and, while waiting, holds a lock the
  process's other cuFile calls need: deregistering at shutdown, inline or on a background
  thread, waits for unrelated callers' GPU work and stalls all cuFile I/O meanwhile;
- a stream one thread used for `cuFileReadAsync` fails it (`-EIO`, `-ENOMEM`) from another
  thread once the first is gone, so streams cannot be pooled for later sets;
- the first `cuFileReadAsync` of the process synchronizes the device (once).

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
  std::size_t bytes{};     // delivered from the start (ok: rng.size)
  int error{};             // failed: cuFile entry status or cuFile error code
};
struct host_batch_options {
  unsigned depth{64}; unsigned failure_limit{4};
  batch_submit_policy policy{batch_submit_policy::per_reader};
  std::size_t max_wave_bytes{0};                      // 0 = no cap
  batch_wake_fn wake{}; void* wake_context{};        // runs on helper threads
};

class host_batch_reader {
 public:
  static constexpr std::chrono::seconds wave_timeout{3600};
  explicit host_batch_reader(host_batch_options const&) noexcept;
  ~host_batch_reader();                                // requires in_flight() == 0
  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] bool poisoned() const noexcept;
  [[nodiscard]] std::size_t capacity(), in_flight(), staged(), pending_failures(),
                            free_entries() const noexcept;
  [[nodiscard]] batch_submit_policy policy() const noexcept;
  [[nodiscard]] bool completions_pending() const noexcept; // any thread
  [[nodiscard]] bool deferred() const noexcept;
  void add(batch_read const& read) noexcept;               // requires free_entries() > 0
  void flush() noexcept;                                   // hand staged entries over
  void discard_staged() noexcept;                          // staged -> failed, never submitted
  template <typename Pred> void discard_staged_if(Pred&&) noexcept;
  template <typename Done> std::size_t reap(std::chrono::microseconds wait, Done&& done);
  template <typename Done> void drain(Done&& done);
};
}
```

- One batch handle of `depth` entries (clamped to cuFile's `max_batch_io_size`); fixed
  parameter, event and bookkeeping arrays; the cookie is the entry index + 1. Outstanding plus
  staged never exceeds the capacity. Every entry is reported exactly once.
- **A helper thread per reader makes every `Submit` and `GetStatus` call.** The owner (the
  reactor worker) stages entries; `flush()` hands at most `max_wave_bytes` of them over
  (mutex-protected copy into a fixed buffer, one condition-variable notify; no allocation). The
  helper submits, reaps, and publishes; then it calls `wake` (the reactor's `notify_worker`).
  `reap()` collects what was published, never hands anything over (so the owner tops the next
  wave up with what it stages after reaping and flushes once per pass), and waits up to `wait`
  only on the hand-off condition variable. The owner never blocks in cuFile; the helper never
  allocates and never logs. Once the helper gave up (abort, poison, rejection limit, failed
  reset) `flush()` fails staged entries itself, checked under the mailbox lock.
- **Whole waves in one call.** Under `per_reader` (default) and `exclusive` a reader hands a
  wave over only when its previous wave was fully reported, and the helper reaps it with ONE
  `GetStatus(min_nr = the whole wave, timeout = 1 h)`. This avoids the libcufile use-after-free
  of §1. Should that call return less than the wave, or an event whose status is not
  `CUFILE_COMPLETE`, `CUFILE_FAILED` or `CUFILE_CANCELED`, the reader is **poisoned**: the
  completed entries are reported, the rest stay in flight for good (cuFile may still write their
  destinations: never settled, never freed), `GetStatus` is never called on the handle again,
  the handle is never destroyed, `available()` is false, an error is logged, and `drain` never
  returns (it logs "shutdown is blocked: cuFile returned a partial batch status and may still
  write N destinations" every 30 s). A signal cannot poison or abort a healthy reader: the
  helper blocks all signals (`pthread_sigmask`); an `EINTR` from `io_getevents`, injected with
  strace, makes compatibility-mode `GetStatus` return `CU_FILE_IO_ERROR`, never a short
  success. `exclusive` adds a process-wide gate: one wave in flight in the process, released by
  the helper as soon as the wave completed (or the reader was poisoned) -- not by the owner, so
  a reactor busy with a long synchronous transfer never holds other readers back -- and the
  release wakes the readers waiting for it. `continuous` hands entries over at once and reaps
  with `min_nr = 1` (crashes libcufile 1.17.1 in compatibility mode). The readers alive together
  share the first one's policy (§4).
- An event is `ok` when `status == CUFILE_COMPLETE` and `ret >= size`; `short_read` when
  complete with fewer bytes; `failed` when `CUFILE_FAILED` or `CUFILE_CANCELED`.
- A rejected `Submit` fails every entry of that call. The helper cancels, destroys and
  re-creates the handle once nothing is in flight on it, and only then publishes the rejected
  entries, so cuFile can no longer write their destinations when the owner sees them. After
  `failure_limit` consecutive rejected submits, or when the re-creation fails, the reader is
  unavailable for good.
- A `GetStatus` error: the helper cancels and destroys the handle; only after `Destroy`
  returned does it publish, and the owner then fails every entry in flight or staged; the reader
  is unavailable for good. Residual risk: this assumes `Cancel` + `Destroy` stop cuFile's
  compatibility-mode threads from writing the destinations (observed: untouched for 200 ms
  afterwards; not documented).
- `drain` hands over and reaps with a bounded wait until every entry reported, warning after
  5 s and logging an error every 30 s. It never abandons a destination cuFile may still write.
  The destructor stops the helper (only between waves) and destroys the handle.
- Measured (1 GiB file, 1 MiB entries, compatibility mode, local NVMe): 7.0-7.2 GB/s at depth
  32-128 against 3.0-3.2 GB/s for a single-threaded `O_DIRECT` `pread`; `Submit` takes about
  3 ms per wave (it does much of the I/O synchronously), on the helper thread. GDS host (GB200,
  GPFS, pinned NUMA-local memory, final build): 38.9 GiB/s (`per_reader`, 2 x 64, the
  default), 38.5 (`exclusive`, 2 x 64), 38.0 (`per_reader`, 4 x 32), against 41.2-41.5 for 32
  synchronous reactors.

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
collect host batch (non-blocking: what its helper published)
... cancellation checks, preempt_low (unchanged) ...
for slices of the active request, while the pass budget lasts and no backlog is pending:
    dispatch_slice()                  # may start a lane read / stage batch entries / run sync
flush host batch                      # hand the pass's entries to the helper (may be deferred)
if nothing more can be dispatched now:
    _wake.wait(timeout)               # lane callbacks and the batch helper signal it
    timeout = idle_wait_us when nothing is in flight or parked,
              poll_interval_us while legacy copies / parked slices need polling,
              1 ms while lanes are in flight, host_batch.poll_us while batch work is
              outstanding (both only bound a lost wake-up)
```

A slice that is a lane read but finds no free lane is parked in a one-element lane backlog and
the pass stops expanding requests until it is issued; the same for a host slice whose entries
do not all fit (its cursor is kept). This is the reactor's backpressure: the queue, not the
slot table, holds the excess, and priorities keep working because requests stay in the queue.

### 8.3 Lane path in `dispatch_slice`

A slice takes a lane when: fused reads are enabled and usable on this reactor; the slice is a
read with a device destination only and no completion callback; the file is cuFile-registered;
`is_lane_read(slice, file_size, slot_bytes)`; and its window is at least `gds_threshold`.

Gate handling (the copies write the caller's memory, the read does not) -- **two-phase**:

| Gate | Action |
|---|---|
| ready, or re-dispatched after parking | submit with its copies |
| pending, and fewer than `gated_lane_limit()` two-phase reads taken on that lane set | submit with `hold`: the read starts now (I/O overlaps the caller's compute); on the `held` result the worker queries the gate: fired → `copy_out`, pending → the op joins the reactor's held list, polled every pass (one query per distinct gate; the wait timeout is `poll_interval_us` while the list is non-empty) and copied out when its gate fires |
| pending otherwise | `park_on_gate` (existing); re-dispatched when the gate fires |
| unchecked (no submission-time gate) | `park_if_stream_busy` (existing), else submit |

A two-phase read counts against the limit from the moment it takes a lane (or the backlog)
until its copies are queued or it is released (a per-set count on the reactor), so at most
`gated_lane_limit()` lanes of a set ever wait for callers' streams, whatever the burst; the
rest park until their gates fire, and reads on idle streams keep the other lanes.  A lane read
that finds every lane held (possible only when the limit equals the lanes) is served
synchronously instead of waiting.  `lane_gated_reads` counts reads that were held.

Completion (`reap` result) for the slice's `io_op_request`:

- `ok` → `finish_success()`; counters `lane_reads`, `lane_bytes`, `lane_issued_bytes`,
  `lane_parts`, `lane_resubmits`, `lane_gated_reads`.
- `io_failed` / `device_failed` → `lane_failures` and `cufile_failures`, log once per file,
  then **fall back**: the slice's destinations are re-served through the synchronous path of
  this reactor (staged `cuFileRead` or POSIX), each taking a coordinator credit before the
  slice returns its own, exactly as the existing "fused slice that cannot be staged" path does.
  The worker never waits for the gate: when it has not fired (a read refused at submission or
  failed in its first phase), the destinations are parked behind it (`waiting_slice` with
  `no_lanes`, re-dispatched without lanes). A fallback that also fails settles the read with
  its error.
- A held read whose coordinator stopped is released (its data dropped) and settles cancelled
  at once; a gate query error settles it with that error.
- The backlog's read is served synchronously (same credit pattern, parked behind a pending
  gate) when its lane set has lost every lane, or every lane is held.
- After `failure_limit` consecutive failures the reactor stops using lanes for `cooldown_s`
  seconds and logs a warning; a success resets the count.
- A cancelled coordinator does not stop a read already in flight; it settles when the lane
  finishes, with the cancellation.

Shutdown: cancel the backlogs (nothing was issued), release the held reads and settle them
with the terminal error (without waiting for any caller stream), `drain` the lane sets (only
lanes with I/O or copies queued, which wait on nothing foreign) and the batch reader, settle
with real results, destroy both on the worker, then run the existing teardown.

Routing follows usability: each reactor publishes whether it can serve lane reads on a GPU
(`lanes_usable(device)`: not cooling down, its set there has lanes; a GPU it has not tried
counts as usable).  The ioctx plans lane reads (`plan_fused_reads`) only when some lane
reactor of every destination GPU is usable, and only such reactors are lane candidates;
otherwise the request takes the legacy path end to end.  Inside one request, lane reads go to
lane reactors and every other slice to the reactors its own role picks, under the request's
one coordinator (`templated_ioctx::slice_class`).  Lane sets take at most the plan's lane
quota of the GPU (`lane_slots`; unplanned GPUs: `min(reactors_per_device x
lanes_per_reactor, max_lanes_per_device)`).  Every part is its own copy: parts contiguous in
the file and in the destination are not merged, because two touching destinations can be
separate allocations (one buffer per Parquet column chunk from a `cudaMallocAsync` pool) and
CUDA rejects a copy that spans two of them with `cudaErrorInvalidValue`.  A window holds at
most `max_parts_per_read` parts.

Instrumentation (`reactor_stats`, always on, relaxed): `lane_submit_ns` (time in
`submit`/`copy_out`), `lane_read_async_ns` (of it, in `cuFileReadAsync`), `lane_flight_ns`
(reap minus first submit, per finished read), `lane_notify_ns` (callback mode: reap minus the
completion callback), `lane_idle_ns` (of that, while lane work was waiting), `worker_wait_ns`
and `worker_passes`.

### 8.4 Host batch path in `dispatch_slice`

On a reactor with `host_batch.enabled && placement.host_batch`, a slice takes the batch path
when it is a read with a host destination only, contiguous, without `on_complete`, on a
cuFile-registered file with an O_DIRECT descriptor; its aligned body
`[align_up(offset), align_down(min(end, file_size)))` is at least `min_bytes`; its destination is
congruent (`dst + (body.offset - offset)` is 4 KiB-aligned); the reader is available; and the
batch backlog is empty. Everything else takes the synchronous path, unchanged.

- The reader is created on the worker at the first such slice (`depth`, `failure_limit`,
  `submit_policy`, `max_wave_bytes`, wake = `notify_worker`); one that cannot be set up is
  retried after 30 s, at most 5 times (logged at info), then the reactor stays synchronous. The
  effective policy is logged once per ioctx.
- Head `[offset, body.offset)` and tail `[body.end, end)` (each under 4 KiB, possibly empty) are
  owed reads with `read_host_direct`, run after the pass handed the wave over (they overlap it);
  a failure or short read fails the slice once its entries drained.
- The body becomes entries of at most `max_entry_bytes` (cut at multiples of it from
  `body.offset`), each reading into `dst + (entry.offset - offset)`. The slice's state (request,
  destination, cursor, entries outstanding, first error) lives in a slab with a free list; an
  entry's token is (slab index << 32 | entry number). **One coordinator credit per slice**,
  given back when the last entry settled.
- Entries are staged while `free_entries() > 0`; the rest keeps a cursor in a one-element
  backlog, continued after every collect. While it is occupied the worker holds back only
  further batch reads (`batch_front_blocked`: the active request's next slice is batch-eligible);
  every other slice dispatches past it. A stopped coordinator stages nothing more, its staged
  entries are discarded before the hand-off, the entries already submitted finish and the slice
  settles with the cancellation.
- The pass ends with one `flush()` (deferred and new entries in one wave), then runs the owed
  synchronous reads -- head and tail, missing bytes of failed or short entries, the rest of a body
  the reader can no longer take -- at most `max_op_bytes` per pass.
- Results: `ok` -> bytes; `short_read` / `failed` -> the missing bytes are owed synchronously
  (`batch_failures`, logged once per file); if that fails too, the slice fails with `io_error`.
  If the reader became unavailable with a slice half staged, the rest of the body is owed.
- Settlement: success -> `finish_success()`, `batch_reads` += 1, `batch_bytes` += the clamped
  payload (head and tail included); never also `host_reads`/`host_bytes`. `_queued_bytes` keeps
  the slice until it settles (as for lanes).
- Shutdown (`drain_async`): staged and backlogged entries are dropped without submission, owed
  reads fail, the reader is drained (forever if poisoned), a failed entry fails its slice with
  the terminal error (no synchronous re-read), the reader is destroyed on the worker.
- The worker waits on its semaphore: the helper wakes it after each wave and the `exclusive`
  gate's release wakes its waiters; `host_batch.poll_us` only bounds a lost wake-up.
- `reactor_stats` exports the helper counters: `batch_waves`, `batch_entries`, `batch_deferred`,
  `batch_submit_ns`, `batch_rejected_submits`, `batch_handle_resets`, `batch_partial_status`.
- The process gate cannot be held by a busy reactor (the helper releases it), so a flat pool
  mixing batch reads with writes and device reads needs no extra rule.

## 9. Pool plan and ioctx

- `device_group_plan` gains `lane_reactors`, `lanes_per_reactor`, `lane_slots`;
  `pool_plan` gains `host_batch_reactors` and `lane_slot_bytes`; `total_reactors()` of a
  grouped plan counts lane, synchronous and batch reactors. With `fused_reads.enabled` the
  automatic synchronous reactor count per GPU is `local_reactors_per_device` (8) unless
  `reactors_per_device` is set: lanes carry the device reads, the synchronous reactors serve
  writes and fallbacks.
- Registered memory (§4): `lane_slots = min(wanted, hard / (slot_bytes + 64 KiB))`, where
  `hard` is the smaller of half the free BAR1 and cuFile's pinned limit less its bounce cache
  (no limit when neither is known) and `wanted = min(reactors x lanes, max_lanes_per_device)`.
  The synchronous staging budget is `max_staging_bytes_per_device`, further limited by
  `hard` less the lane bytes. `staging_cap_bytes` = that budget + the lane bytes, at most
  `hard` (0 when there is neither a configured cap nor a hardware limit).
  `fused_lane_staging_bytes(cfg)` is the lane share a flat pool adds to its cap.
- An enabled block that fails `validate()` is reported as a plan warning and planned as
  disabled; with both blocks off the plan is unchanged.
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
| `test_fused_pool_plan.cpp` | `[fused_plan][placement]` | `validate()` of both blocks, lane and batch reactors in the plan, the lane budget on top of the staging cap, hardware limits cutting lanes, warnings, legacy plan unchanged when off, `describe` |
| `test_fused_lanes.cpp` | `[fused_lanes]` | lane set against libcufile: content, concurrency up to capacity, round-robin, gate ordering, short-read resubmission and failures through a scripted `cufile_api`, callback failure, budget, drain |
| `test_host_batch.cpp` | `[host_batch]` | batch reader against libcufile: content at distinct bases, capacity, scripted failures (rejected submit, failed and short entries, `GetStatus` error), drain |
| `test_fused_ioctx.cpp` | `[fused_ioctx]` | end to end through `cufile_ioctx`: random vectored device reads content-checked against the file, EOF, busy caller stream, cancellation and shutdown with reads in flight, fallback after injected failures, cooldown, host batch reads on every alignment, routing by role, statistics, legacy behaviour unchanged when off |

## 11. Validation on the GDS host

`tpch_scan_bench` `gds_chunk_read` gains modes that drive `cufile_ioctx` with fused lanes and
host batch over the same files, chunk mix and NIC sampler as the raw modes, so the numbers are
comparable line by line with the raw result (79.0-88.3 GiB/s over two days, so compare rows
measured in the same hour; results in [`../cufile-fused-lanes.md`](../cufile-fused-lanes.md#measured-results)). Rules of that host apply: one run at a
time, at most 128 reads in flight, fresh files, watch `mmfs.log.latest`.
