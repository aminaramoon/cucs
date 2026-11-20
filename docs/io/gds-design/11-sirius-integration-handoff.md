# Handoff: integrating fused stream lanes and host batch reads into Sirius

For the agent (or engineer) wiring cuCascade's two new opt-in cuFile features into Sirius
and exposing their configuration. Everything here is the state of cuCascade branch
`io/cufile-fused-lanes` (fork `aminaramoon/cucs`); pin its tip (the handoff message names
the commit).

Read first, in this order:
1. [`../cufile-fused-lanes.md`](../cufile-fused-lanes.md): the operating guide (what the
   features do, every knob, memory, observability, measured results).
2. `include/cucascade/io/cufile/config.hpp`: `fused_read_config`, `host_batch_config`,
   `lane_completion`, `batch_submit_policy` (defaults and `validate()` are the contract).
3. [`10-fused-lane-reads.md`](10-fused-lane-reads.md): the design, only if you need the why.

## 1. What changed in cuCascade (and what did not)

- `cufile::config` gained two blocks, both **off by default**: `fused_reads`
  (`fused_read_config`) and `host_batch` (`host_batch_config`). With both off the cuFile
  backend behaves exactly as before.
- **Fused lanes** (`fused_reads.enabled`): device reads of cuFile-registered files are packed
  on the submitting thread into 4 KiB-aligned windows of at most `slot_bytes` (8 MiB) and
  issued as `cuFileReadAsync` on per-reactor lanes (registered slot + stream); the callers'
  ranges are copied out device-to-device. Futures stay **host-complete** (same contract as
  the synchronous path: when the future resolves the bytes are in place; no AM-1 caveat).
- **Host batch** (`host_batch.enabled`): host-only reads whose body is 4 KiB-aligned in file
  offset, size and destination address go through `cuFileBatchIOSubmit`; everything else
  keeps the synchronous path.
- No public read/write API changed. `reactor_stats` gained counters (`lane_*`, `batch_*`,
  `worker_*`) and `operator-`. `pool_plan` / `device_group_plan` gained fields
  (`lane_reactors`, `lanes_per_reactor`, `lane_slots`, `lane_slot_bytes`,
  `host_batch_reactors`). New helpers: `fused_lane_staging_bytes(cfg)` (placement.hpp),
  `reserve_staging_slots(device, slots, slot_bytes, cap_bytes, require_registered)`
  (staging.hpp), `cufile_ioctx::reactor_count()` / `reactor(i)`.
- `fused_reads.enabled` requires `async_engine.mode == legacy`; every `cufile_reactor`
  constructor throws `std::invalid_argument` otherwise, and on an invalid block.
- Base: the branch sits on `io/cufile-async-engine` (which contains `io/s2-write` @ 65996d6).
  Sirius `io/s2-write` builds against it without source changes (Sirius commit `c951747c0`
  on `config/io-throughput` only added configuration when it first pinned that lineage).

## 2. What Sirius has to do

### 2.1 Pin
Point the `cucascade` submodule at the tip of branch `io/cufile-fused-lanes` of
`https://github.com/aminaramoon/cucs.git`. Work on Sirius branch `io/s2-write`.

### 2.2 Configuration (`src/sirius_config.cpp`, `from_yaml(..., cucascade::io::cufile::config&)`)
Add two optional sub-blocks under every place the cuFile config is read
(`scan_manager.cufile`, and the DISK-tier `io.cufile` block where that exists on your branch):

```yaml
cufile:
  fused_reads:
    enabled: false
    reactors_per_device: 8      # lane reactors per GPU (planned pool)
    lanes_per_reactor: 16       # 1..64; in flight per GPU = reactors x lanes
    max_lanes_per_device: 128   # process-wide ceiling per GPU
    slot_bytes: 8MiB            # multiple of 64 KiB, 64 KiB..1 GiB
    fuse: true
    fuse_gap_bytes: 0           # < slot_bytes
    register_streams: true
    completion: poll            # poll | callback
    max_gated_lanes: 0          # 0 = half the lanes; <= lanes_per_reactor
    max_resubmits: 8
    max_parts_per_read: 256     # >= 1
    failure_limit: 8            # >= 1
    cooldown_s: 30
    stall_warn_s: 30            # 0 = never
  host_batch:
    enabled: false
    reactors: 2                 # batch reactors of a planned pool, 1..64
    depth: 64                   # entries in flight per reactor, 1..1024
    max_entry_bytes: 8MiB       # multiple of 4 KiB, 4 KiB..1 GiB
    max_wave_bytes: 1GiB        # 0 = no cap; multiple of 4 KiB
    min_bytes: 64KiB
    poll_us: 200                # 1..1000000
    failure_limit: 4            # >= 1
    submit_policy: per_reader   # per_reader | exclusive | continuous
```

- Take defaults from the cuCascade structs (do not duplicate numbers); after parsing call
  `fused_reads.validate()` / `host_batch.validate()` and surface their message with the YAML
  path, as the `async_engine` block on `config/io-throughput` does
  (`read_async_engine` in `src/sirius_config.cpp` there is a good template).
- Reject unknown keys (`reject_unknown`), as the surrounding code does.
- Cross-checks worth doing in Sirius: `fused_reads.enabled` with a non-legacy
  `async_engine.mode` (if that block is exposed on your branch) is an error;
  `reactors_per_device x lanes_per_reactor > max_lanes_per_device` is legal but deserves a
  warning (the excess lanes never exist); `continuous` should carry a warning in the docs
  (crashes libcufile 1.17.1 in compatibility mode).

### 2.3 GPU memory accounting and early reservation
Lanes hold registered device memory **outside RMM**, on top of
`max_staging_bytes_per_device`:

    fused_lane_staging_bytes(cfg) = min(reactors_per_device x lanes_per_reactor,
                                        max_lanes_per_device) x (slot_bytes + 64 KiB)

(1032 MiB per GPU with the defaults.)

- `reserve_cufile_staging()` in `src/scan_manager/sirius_scan_manager.cpp` reserves the
  synchronous staging ahead of the GPU memory pool. Do the same for the lanes: for each
  `group` of the plan with `lane_slots != 0` call
  `cufile::reserve_staging_slots(group.device, group.lane_slots, plan.lane_slot_bytes,
  group.staging_cap_bytes, cufile::gds_available())` and log what was reserved. Without it
  the lanes lease their slots at the first read, after RMM may have taken the device.
- Wherever Sirius sizes the GPU memory pool or reports cuFile staging (the summed
  reservation on `config/io-throughput`, docs), add `fused_lane_staging_bytes(cfg)` per GPU.
- `group.staging_cap_bytes` of the plan already includes the lanes; pass the plan's caps
  through unchanged when building the ioctx (or keep using `make_cufile_ioctx`, which does).

### 2.4 Pool construction
Nothing to do if Sirius builds the ioctx with `make_cufile_ioctx(n_reactors, cfg)` or
`cufile_ioctx(plan, ctx)`: the plan carries the lane and batch reactors and the ioctx routes
by role. Notes:
- `cufile_n_reactors > 0` (flat pool): every reactor owns lanes and batches; in flight is
  still capped by `max_lanes_per_device`. Prefer the planned pool (`0`).
- With fused reads on, the automatic synchronous reactor count per GPU drops from
  32/8/16 to 8 (the lanes carry the device reads). An explicit `reactors_per_device` is kept.
- Build **one** cuFile ioctx per process and keep it: lane streams of a destroyed ioctx stay
  registered with cuFile for the life of the process.
- Several ioctxs in one process (scan + DISK) share `max_lanes_per_device` per GPU: the
  first to read takes the lanes; give the DISK context `fused_reads.enabled: false` unless
  it really reads into device memory at scan-like volume.

### 2.5 Callers
No caller changes. Device reads keep the generic contract. Two things worth knowing:
- Fusion happens inside one vectored call: issue the ranges of a row group / a batch of
  column chunks in ONE `device_readv_async_io` (or `device_read_ranges_async`) rather than
  one call per chunk, on one stream. Ranges that are contiguous in the file and in the
  destination cost one copy per 8 MiB window.
- A read whose stream still has work queued starts its I/O at once and only its copies wait
  (up to `max_gated_lanes` lanes per reactor hold such data); nothing to change, but an idle
  or lightly loaded read stream gives the best lane turnover.

### 2.6 Observability
`cufile_ioctx::stats()` now also reports, per pool (sum) or per reactor (`reactor(i)`):
`lane_reads, lane_bytes, lane_issued_bytes, lane_parts, lane_resubmits, lane_failures,
lane_gated_reads, lane_submit_ns, lane_read_async_ns, lane_flight_ns, lane_notify_ns,
lane_idle_ns, batch_reads, batch_bytes, batch_failures, batch_waves, batch_entries,
batch_rejected_submits, batch_partial_status, batch_handle_resets, batch_deferred,
batch_submit_ns, worker_wait_ns, worker_passes`. If Sirius logs or exports cuFile stats,
add them (`reactor_stats::fields()` lists every member, so a generic dump needs no names).
Useful derived values: fusion = `lane_parts / lane_reads`, waste =
`lane_issued_bytes / lane_bytes`, mean lane cycle = `lane_flight_ns / lane_reads`, wave fill
= `batch_entries / batch_waves`. `lane_failures` or `batch_partial_status` above zero need
attention.

### 2.7 Documentation (`docs/super-sirius/configuration.md`, `scan.md`)
- A `cufile.fused_reads` and a `cufile.host_batch` section (key, type, default, meaning),
  and the recipe below in the throughput recipes.
- State the interplay: `cufile.reactors_per_device`, `staging_piece_bytes`,
  `fuse_gap_bytes` (top level) only concern the synchronous path;
  `fused_reads.reactors_per_device`, `slot_bytes`, `fuse_gap_bytes` the lanes.

### 2.8 Tests
- Config tests (`test/cpp/config/test_scan_manager_config.cpp`): defaults equal cuCascade's,
  every key round-trips, each `validate()` rejection surfaces with its YAML path, unknown
  keys rejected.
- An end-to-end scan through the cuFile backend with `fused_reads.enabled: true` (it runs in
  compatibility mode without GDS) asserting correct results and `lane_reads > 0`; and one
  with `host_batch.enabled: true` if Sirius has a host-read path through the cuFile ioctx.
- Run `sirius_unittest "[config],[disk_io_config],[scan_manager]"` and the full
  `pixi run make test` before marking the PR ready.

## 3. Recommended settings

GPFS / IBM Storage Scale over RDMA (measured on GB200, two 400 Gb/s NICs per GPU):

```yaml
scan_manager:
  cufile_n_reactors: 0
  cufile:
    storage_hint_path: /path/on/the/gpfs/mount/some.parquet
    fused_reads:
      enabled: true          # 8 x 16 lanes of 8 MiB, poll completion: the defaults
    host_batch:
      enabled: true          # only if host reads go through this context
```

Measured on that host on 2026-11-20 with `gds_chunk_read --mode cc|cc-host`
(`tpch_scan_bench`, branch `gds-chunk-read`): 20 files x 1000 MiB per pass, chunks cycling
1/2/5 MiB read as one vectored call per 15 chunks, GPU 0, medians over the steady passes,
content verified against `pread`, NIC bytes 1.02x payload on the two GPU-local NICs:

| Path | Payload |
|---|---|
| raw `cuFileReadAsync` reference (4 threads x 32 streams, fused 8 MiB) | 79.0 GiB/s that hour (79.5-88 over two days) |
| **fused lanes, defaults (8 x 16, poll)** | **84.3 GiB/s** |
| fused lanes, 4 x 32, poll | 79.7 GiB/s |
| fused lanes, 4 x 32, callback completion | 65.1 GiB/s |
| fused lanes, 256K/512K/1M chunks (12.5 pieces per read) | 77.7 GiB/s |
| synchronous pool (fused reads off, 32 reactors, one read per chunk) | 77.5 GiB/s |
| host batch, 2 reactors x 64 (`per_reader` / `exclusive`) | 34.6-37.9 / 37.5-38.0 GiB/s |
| host reads, synchronous pool (32 reactors) | 41.2-41.5 GiB/s |

What they mean for Sirius: on 1-5 MiB chunks the lanes match or beat the raw pattern with 8
threads per GPU instead of 32, and they keep that rate when the chunks are small (a
read-per-chunk pool cannot: its rate is reads/s x chunk size). Host batch reaches about 90%
of 32 blocking threads with two reactors.

Operational rules learned on that host:
- Keep at most 128 device reads in flight per GPU (`max_lanes_per_device`): a deeper run of
  1 MiB asynchronous reads crashed the GPFS client there.
- After a GDS process exits (and especially after one is killed with reads in flight) the
  GDS path of the node can stall for minutes; GPFS logs `VERBS RDMA closed connection ...
  error 733` when a process was killed. Do not kill scans mid-read on that stack, and leave
  a few minutes between benchmark processes.
- cufile.json: `max_direct_io_size_kb >= 8192`, `rdma_dev_addr_list` set, poll mode off.

## 4. Known limitations
- Fusion is per vectored call (no fusion across calls).
- Writes are unchanged (synchronous, staged).
- The prefetching cache is still refused by the cuFile backend.
- Host batch only takes fully aligned, congruent host reads; parquet column chunks read into
  arbitrary host buffers mostly keep the synchronous path.
- libcufile 1.17.1 batch API hazards shaped the reader (whole waves only); `continuous`
  crashes it in compatibility mode.
