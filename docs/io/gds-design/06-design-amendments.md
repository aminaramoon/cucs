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
  Explicit streams use the enqueued contract. Original null streams remain on
  the legacy host-complete route conservatively (P3-R20); an explicitly supplied
  `cudaStreamLegacy` may use the engine.
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
    - `include/cucascade/cudf/datasource.hpp` scalar/vector read contracts (updated
      in Phase 3; implementation call-site audit remains separate)
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
  therefore never gates the storage submission itself. Copies retain their lane
  and slot; AM-12 bounds that retention so unrelated callers keep making progress.
- The read path does not use the write-style `input_wait` phase or
  `ops_in_input_wait`. AM-12 supplies its separate admission hint cap. Writes keep them, because the staging copy *reads the
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
  cudf layer. Building `cucascade_io_tests` also compiles it when cuFile headers
  are available, so loader changes cannot silently break the diagnostic.

## AM-9: Registration granularity is a qualification dimension (Phase 1 review P1-R1)

- Phase 1 review observed on the development host (ext4, compat-mode libcufile
  1.17, no nvidia-fs) concurrent `cuFileReadAsync` calls into disjoint offsets of
  **one registered arena** completing with `bytes_done = -1` at QD ≥ 2. The libcufile log reports
  POSIX async read `Input/output error` and I/O verification failures. QD 1,
  synchronous reads, unregistered slots, and separately registered slots with
  `buffer_offset = 0` work. Changing stream-registration flags does not fix it.
  This is a reported compat limitation, not a conclusion about real GDS.
  The earlier follow-up on devices 0 and 1 at QD 2/8/32, 1/2 MiB checked
  **only `bytes_done`, never contents**. Its reported pass is void as evidence
  of correctness. Phase 2 review reproduced deterministic **silent corruption**
  on libcufile 1170, compat, ext4, QD 4: all `bytes_done` values reported the
  full size while concurrent slots contained the wrong bytes. The revised
  content-validating harness reproduced it again (QD 1: no mismatches;
  QD 4: 350 data mismatches / 361 completions, zero `bytes_done` mismatches).
  The hidden, slot-poisoning integration reproducer also fails for both arena
  and slab layouts at QD 4, with lane/offset/first-byte diagnostics.
  Shared arena and slab registrations are unqualified. Production must use
  `slot` unless the mount is `verified_direct` **and** content-validated G-gate
  evidence qualifies that particular shared granularity. An explicit unsafe
  opt-in exists only for qualification/reproduction; it is not qualification.
- The P0 harness exposes `--register 0|arena|slot` (`1` aliases `arena`) and
  defaults to `slot`. `--verify 1` (the default) performs a full D2H/reference
  comparison after every completion and reports `data_mismatches`; any mismatch
  fails the run. `--prepare-file` creates a new offset-dependent deterministic
  pattern file and refuses to overwrite an existing file. Use nonuniform
  patterned files for correctness; zero files hide cross-lane corruption.
  `--verify 0` is for throughput measurement only after a verified smoke run;
  verification overhead is included in elapsed time, not cuFile submit latency.
  CSV rows identify the selected registration mode. Compat
  smoke tests use `slot` and `0`; `arena` intentionally remains available to
  investigate the failure. Each lane performs a validated untimed I/O warmup
  before measured cases, so cold cuFile initialization does not bias submit p99.
  `--warmup-ops 0` disables this for cold-start comparisons; the follow-up cold
  run saw a first-submit maximum of about 15 ms, versus sub-100 µs with warmup.
- **Phase 2:** `device_arena` must parameterize arena/slab/slot registration
  granularity and retain the registered base plus offset appropriate to that
  choice. The compat-libcufile integration test must cover concurrent disjoint
  slots with per-slot registration (and retain hidden arena/slab reproducers).
  Compat testing must not depend on the failing shared-registration layout.
- **Phase 7 / G-1 through G-8:** compare arena/slab registration against per-slot
  registration on real GDS, including concurrent correctness, registration cost,
  BAR1 usage, and throughput. Slab registration cannot become the qualified
  default until these comparisons pass. The engine remains legacy by default.

## AM-10: Phase 1 configuration surface and policy contracts (P1-R7, R11, R16)

- Phase 1 exposes only the implementation-plan subset of §6.2. `poll` currently
  uses scalar `io_cap_us` and `copy_cap_us`, not the base spec's explicit backoff
  ladders. These are maximum delays, **not constant polling periods**. Phase 3
  must implement and test bounded adaptive backoff beneath those caps before
  the scheduler is enabled; exact ladders are not a frozen configuration API.
- Durations currently use unit-suffixed unsigned scalars, not `std::chrono`.
  The runtime phases must use checked conversions to chrono durations.
- Deferred to the resource/scheduler implementation (Phases 2–3): `retry`,
  `blocking_workers`, `recovery_workers`, `max_pending_slices_per_device`,
  `max_join_streams_per_device`, `registration_first_step`, `bar1_fraction`,
  `bar1_headroom`, `max_submitters_process`, and `submitter_idle_retire`.
  Add each with its validation and consumer rather than unused placeholders.
- Deferred to Phase 6 parameter/affinity work: `process_params`,
  `strict_parameters`, and mount override `nic` / `cpus`. `event_api` remains
  removed by AM-1.
- Override precedence for Phase 2 policy is fsid, then longest matching mount
  prefix, then filesystem type; earlier list entries break equal-priority ties.
  Mount prefixes match normalized absolute paths at component boundaries, not
  arbitrary string prefixes (`/mnt/a` does not match `/mnt/abc`). Validation
  rejects duplicate `{match, value}` pairs, non-normalized prefixes, and
  filesystem types containing whitespace. Overlapping normalized prefixes are
  allowed because longest-prefix precedence is defined.

## AM-10: Compat-mode async submission does not block on libcufile 1.17 (lead observation, Phase 1 smoke)

- The P0 harness ran on the development host (libcufile 1170, compat, ext4, page-cached 256 MiB file, slot registration).
  - `cuFileReadAsync` submit p50 was about 6 µs at 1 MiB and about 17 µs at 4 MiB, against completion p50 of 0.44–9.3 ms.
  - QD 16 reached about 4× the throughput of QD 1 (1.66 → 6.72 GiB/s at 1 MiB).
- So the statement "async cuFile runs synchronously in the submitting thread in compat mode" (`cufile_reactor.hpp:168-171`, brief §4) does **not** hold for this version. It may hold for older ones.
- Consequences:
  - The FR-9 slow-submit breaker will not demote compat mounts on 1.17. The `known_compat` decision must come from `gds_available()` and the per-mount probe evidence, not from submit latency alone.
  - Compat async may be a legitimate QD lever on non-GDS hosts. B-9/B-10 must measure it against the legacy sync path before the compat default is reconsidered. The default stays legacy (NFR-2).

## AM-11: Phase 2 resource and qualification corrections

- **FR-16 / §4.7 / §6.9 (P2-R13).** Fit the lane constraint first:
  `Q_lane = max{q <= Q : lanes(q) <= C_m}`. At that QD choose the largest
  available prewarmed class `S <= S_pref` with `lanes(Q_lane) * S <= B_m`.
  Only if no class fits may bytes reduce QD further, using the smallest
  available class (normally 1 MiB). Lane contention must not force 1 MiB
  operations when the byte grant supports larger ones. Empty class sets force
  fallback. Scheduler integration in Phase 3 must pass the arena's registered
  class mask to capacity fitting; immutable slot geometry must never result in
  issuing requests for classes the arena cannot supply (P2-R9).
- **Slot-mode geometry (P2-R9).** `slab_classes` is the control-plane input for
  the expected profile set. Slot registrations cannot be reformatted in place;
  adding an unprepared class requires maintenance and re-registration. The
  default 1 MiB arena is safe when fitting is capped by `available_classes()`;
  it does not promise 4 MiB pieces just because a filesystem prefers them.
- **Registration budget (P2-R12).** Default warm-up samples live free BAR1 via
  optional dynamically loaded NVML (matched by CUDA PCI address) and caps by
  the runtime's cuFile pin limit minus default arena registrations. No NVML
  availability is assumed: unknown BAR1 stops at `registration_first_step`
  (default 64 MiB), not at the first registration failure. A failed shared bulk
  registration may step down to independent slab groups; that does not waive
  AM-9's explicit shared-registration qualification guard.
- **Direct evidence (P2-R7).** Automatic `nvidia_fs_evidence` qualification is
  deliberately unavailable in Phase 2. The former parser recognized invented
  byte counters, not real `Reads ... readMiB` / `Writes ... writeMiB` output.
  Real counters are coarse and process-global, and cannot prove attribution
  without the isolated interval deferred by AM-4. Implement a captured-host
  format parser, MiB rounding tolerance and isolated attribution together in
  Phase 7 / network qualification before enabling this evidence source. Neither
  its existence nor an unvalidated counter delta may imply `verified_direct`.
  `cufile_stats_evidence` is likewise unavailable. Explicit operator
  qualification and test-only overrides remain distinct.
- **Registration policy placement (P2-R20).** `registration_first_step`,
  `bar1_fraction`, and `bar1_headroom` are validated `device_arena_options`
  rather than fields of `async_engine_config`. They are consumed at warm-up,
  not transfer admission; the bootstrap owns and freezes these options.
- **Reaper cleanup (P2-R19, Phase 3 integration).** Operation retirement and
  the final lane-state reference may synchronously deregister/close/destroy
  external resources. Before releasing an operation on the reaper, transfer
  final file, lane-pool, input and scratch owners to the blocking cleanup pool.
  Phase 2's standalone resource APIs are not themselves nonblocking final-owner
  destructors. Test this handoff before enabling the Phase 3 scheduler (FR-5).


## AM-12: Bound fence-blocked read admission (Phase 3 review P3-R1)

- A nonblocking, reaper-only fence readiness hint is cached per cursor. A pending
  hint is queried no more often than `poll.fence_hint_us`; readiness is sticky.
  It is a scheduling hint, never a correctness fence.
- Across a mount, at most `ceil(effective.lanes / 8)` pieces admitted with an
  unready hint may retain a lane/slot. The count covers the entire lease, including
  I/O, fence-waiting copy and cleanup. This is deliberately more conservative
  than counting only validated copies. The hint cap applies across slices and
  logical requests, so many slices sharing one busy stream cannot bypass it.
- Further pieces remain resource-free. Ready requests of either priority remain
  schedulable; a gated high-priority cursor does not suppress ready low work.
- The first admitted piece still submits storage without waiting for the fence.
  Every D2D retains its unconditional fence wait. Every non-PTDS success retains
  its caller-stream join. Original PTDS requests still complete host-observed.
- A gated 16 MiB request and a ready 1 MiB request on the same mount must both
  progress independently, with QD 1 and 4 and in either arrival order.

## AM-13: Phase 3 review integration and explicitly tracked follow-up

- **Context-level completion (P3-R8/R19).** In a non-legacy cuFile context,
  device-only reads on original non-PTDS streams may resolve enqueued. Routing is
  dynamic; callers must always wait on the stream before host/foreign-stream
  access and retain destinations through that wait. The reaper inserts the join
  later, from another thread: graph capture on that stream while a read is in
  flight is unsupported. Later CUDA copy errors cannot revoke early success;
  stream/context synchronization and `late_copy_failures` expose them.
- **Legacy redispatch (P3-R10).** A legacy slice parked on its input stream stays
  legacy when redispatched, even if qualification finishes in the meantime.
  A request observing an unqualified policy before scheduling its probe also
  stays legacy even if the worker is preempted until after probe completion.
- **Metadata pressure (P3-R6).** Facades reserve engine metadata before credit
  adoption; exhaustion preserves the original credit and uses legacy. Direct
  engine callers retain the explicit bounded-admission error.
- **Fallback (P3-R4/R5/R24).** Retry nodes whose grant becomes zero use sync
  staging. Backend refusal/unsupported first completions and submit errors are
  reissued through sync fallback after event observation or owned-lane recovery.
  Unsupported async operation conservatively opens the mount circuit, rather
  than repeatedly failing user reads. Terminal ENOSPC/EACCES, malformed counts,
  nonprogress, exhausted short-read retries and file shrink still fail. Negative
  fallback results are terminal; no EINVAL loop is allowed.
- **Phase 6 error/lifecycle hardening.** Add bounded transient ESTALE-class
  backoff and finer per-file demotion; Phase 3 uses a single sync fallback instead
  of replaying async transient errors. Replace an errored CUDA lane rather than
  reusing it after a successful owned-lane synchronization (§7 #42). Wire
  per-file granules and geometry boundaries, currently the conservative 4096-byte
  granule and no explicit geometry boundaries. Extend nonblocking lease-release
  APIs before moving resource retirement onto the reaper (P3-R15).
- **Phase 7 CPU/performance qualification (P3-R2/R3).** Phase 3 now uses a free-node
  stack, active-node iteration, constant-time cursor lookup, merged submit results,
  throttled live snapshots, deadline/notification-driven backlog service, and
  reaper active-ns/iteration gauges. Cleanup has an independently started,
  semaphore-driven executor, isolated from sync I/O; final owners stay off-reaper.
  Per-mount/tier intrusive ready queues, incremental request accounting, batched
  cleanup and parked-only eventfd writes remain Phase 7 prerequisites for G-5.
  Replacing the DRR containers is a separate scheduler change requiring high-R
  fairness/CPU benchmarks; this correction does not claim the NFR-3 budget passes.
  Do not enable the engine by default or use it for acceptance measurements
  until these remaining costs are bounded and measured.
- **Phase 7 qualification (P3-R26).** Seed the healthy submit-latency baseline
  from an isolated read probe (FR-9). The current probe threshold is
  `submit_max_us`; absent real-host evidence it must not imply baseline seeding
  or direct qualification.
