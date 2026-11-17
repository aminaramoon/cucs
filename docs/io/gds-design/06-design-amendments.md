# Amendments to `05-final-design.md` (authoritative)

`05-final-design.md` is the base specification. **Where this file conflicts with
it, this file wins.** Each amendment records its source: the user (decision log
`checkpoints/decisions.md`) or the counsel judge (`05a-judge-evaluation.md`, the
"fixes it should still make").

---

## AM-1: Read completion means "enqueued and joined onto the user stream" (user decision D1)

Replaces FR-1, §4.8 step 4, and the P5 two-phase event API (FR-32, §6.12,
`device_io_event.hpp`). The `_ev` API is dropped.

- **Engine-served device reads.** A successful future means all of the
  following:
  1. Every piece's I/O completed, and the reaper validated its `bytes_done`.
  2. Every final D2D copy into the destination is enqueued on its lane.
  3. The caller's resolved stream was made to wait on those copies with
     `cudaStreamWaitEvent(user_stream, copy_ev)`.

  The reaper issues the wait immediately after recording `copy_ev`. Because it
  is the thread that recorded the event, there is no cross-thread ordering
  hazard. `cudaStreamWaitEvent` captures the event's latest record at call time,
  so lane events can be reused afterwards. The caller must synchronize, or wait
  on, its stream before reading the data on the host or on another stream.
- **The future resolves after the last piece's copy is enqueued and joined.** It
  does not wait for the copy to execute. Staging slots, lanes, `io_object`
  references, and argument blocks are still retained internally until the
  reaper observes `copy_ev` complete.
- **PTDS callers.** The stand-in stream from `resolve_default_streams` is only
  ordered *after* the caller's per-thread stream. Joining back onto that stream
  is only possible on the caller's thread. Requests whose original stream was
  `cudaStreamPerThread` are therefore resolved **host-complete**: the reaper
  waits until it observes every `copy_ev` before settling the last credit.
  Null streams (pinned to `cudaStreamLegacy`) and explicit streams use the
  enqueued contract.
- **Legacy, compat, and POSIX paths** stay host-complete. That is stronger than
  the contract, so it conforms.
- **Errors.** The contract can only resolve after I/O validation. Async cuFile
  reports errors through `bytes_done`, which is readable only after the I/O
  event fires. A failure to enqueue the D2D copy or the join settles that piece
  with an error, through recovery (FR-30).
- **Writes are unchanged.** The future resolves after the bytes are in the file
  (host-observed).
- **Migration.**
  - The `io_context.hpp` contract text changes in the same phase that makes the
    engine reachable.
  - Callers that assume host-complete reads must synchronize the stream before
    touching the data from the host:
    - `src/cudf/disk_io.cpp` read path
    - `check_transferred`
    - the benchmarks
    - `test_cufile_reactor.cpp:660`

    Until those callers are audited, the engine stays off by default
    (`mode=legacy`).
  - The cudf layer does not currently build against the pinned nightly
    (pre-existing break at `representation_converter_builtins.cpp:736`). Its
    call-site updates are tracked, not compiled.

## AM-2: For reads, the input fence gates only the D2D copy (judge fix 1)

Replaces the read part of FR-4, §4.8 steps 2 and 3, the read part of FR-26's
input-waiter cap, and the read part of the submitter's `cudaStreamWaitEvent` in
§6.10.

- A staged read writes only into reactor-owned staging. It never waits on the
  fence: the submitter issues `cuFileReadAsync` with no fence wait.
- Before the D2D copy, the reaper enqueues `cudaStreamWaitEvent(lane, fence)` on
  the lane, then the copy. The fence orders the copy after the destination's
  stream-ordered allocation and any earlier caller work. A busy caller stream
  therefore delays only copies, never storage queue depth.
- The read path no longer needs `input_wait` lanes, `ops_in_input_wait`, or the
  device input-waiter cap. Writes keep them, because the staging copy *reads the
  caller's source*.
- Direct registered reads (FR-18) write into caller memory, so they keep a fence
  wait before the I/O.

## AM-3: One submitter per device by default (judge fix 5)

Replaces FR-28's per-`(device, mount)` submitters.

- The default is `submitters_per_device = 1`, configurable from 1 to 8. Mounts
  are assigned round-robin to a device's submitters.
- Each submitter keeps one command queue per mount, so the per-mount
  watchdog/circuit-breaker accounting, and `refuse_unstarted` for a single
  mount, still work.
- Known trade-off: while a submitter is blocked in a stuck call, it delays other
  mounts on the same submitter. The 2 ms watchdog opens the stuck mount's
  circuit. That mount's queued commands return as `not_submitted`. Other mounts'
  queued commands are moved to another submitter if one exists, otherwise they
  wait.

## AM-4: Fallback evidence for `unknown` mounts (judge fix 2)

Adds to FR-6 and FR-8.

- **Operator override.** `async_engine_config::mount_overrides` is a list of
  `{match: fsid | mount-point prefix | fs type, state: operator_qualified | known_compat | legacy, profile overrides}`.
  It moves a mount to `operator_qualified` without probe evidence. It is
  reported distinctly and never as "verified".
- **Test-only override.** `probe.require_direct_evidence` (default `true`).
  Setting it to `false` promotes a mount on non-blocking submit alone. This is
  used with the transport simulator and with compat-mode CI to exercise the
  engine.
- The isolated qualification interval (§4.5.3 (b)) is deferred to the
  network-qualification phase.

## AM-5: Arena sizing and the op-size comparison (judge fix 3)

Replaces the default in FR-13 and §4.4.

- `arena_bytes = 0` means *auto*:
  `auto = floor16MiB(min(1 GiB, 5 % of total device memory, reservable memory))`.
  Registration is then capped by FR-14's BAR1 rule. Any non-zero value is used
  as given.
- The first hardware benchmark must compare `S_eff` = 2 MiB (forced by a small
  arena) against an `S_eff` equal to the filesystem block size, for GPFS and
  NVMe, before profile defaults are frozen.
- Reservation-manager integration stays an open question (§9.11). Until it is
  resolved, the arena is allocated with `cudaMalloc` and reported in gauges.

## AM-6: Coalescing small slices is in scope, as a later phase (judge fix 4)

Promotes open question §9.15 into the plan.

- Adjacent device-read slices of the same request on the same file and device
  are coalesced into one physical piece when all of the following hold:
  - the gap between them is ≤ `merge_gap_size()`
  - the combined granule-aligned window is ≤ `S_eff`
  - each slice is smaller than `S_eff / 2`

  One physical read is followed by one D2D copy per slice. The operation holds
  **one credit per slice**: `Operation` gains a small vector of
  `{logical range, user_ptr, credit}`.
- This is required before DISK-restore acceptance (B-4), but comes after basic
  engine reads.

## AM-7: Environment and validation scope

- The development host is compat-only: no nvidia-fs, no RDMA, ext4 only, BAR1
  64 GiB. Engine logic is tested with `transport_simulator` (a deterministic
  fake `cufile_api`) and, where possible, with real libcufile in compat mode
  through AM-4's test-only override.
- Gates G-1 to G-8 need real GDS hosts. The default stays `mode=legacy` until
  they pass. Use `tools/gds/collect_topology.sh` to gather target-host
  information.

## AM-8: Build and targets

- Work is built and tested with
  `pixi run cmake --build build/release --target cucascade_io_tests` and
  `./build/release/test/cucascade_io_tests`.
- The full `pixi run build` fails for the pre-existing cudf reason above.
- The P0 harness is a standalone executable that does **not** depend on the
  cudf layer.
