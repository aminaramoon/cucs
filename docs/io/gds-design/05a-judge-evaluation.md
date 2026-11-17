# Judge evaluation — round 1

## gds-io-expert

**Pros**
- Architecture §7 states 'Each lane holds at most one physical operation' and provisions lanes above storage QD (40 lanes for QD 32, 80 for QD 64, device cap 160). This correctly reflects that cuFileReadAsync is stream-ordered (Appendix A §1), so storage QD equals lanes in the I/O phase and copy residence does not consume QD.
- §4 input fence: one timing-disabled event per distinct (device, resolved stream) is recorded on the submitting thread before the request is published, then waited on GPU-side by the internal lane. The liveness cap (at most one input-waiting lane per request, at most 1/8 of device lanes on unready inputs) keeps the 'worker never blocks on someone else's GPU work' property of Appendix C §5(h) without the 200 µs host poll.
- §5 validate-before-copy tracks physical-requested, logical-required and valid-returned bytes separately. It specifies aligned reissue of the missing prefix, buffered fallback for a remainder that loses O_DIRECT alignment, bounded retries, and rejection of zero progress or file shrink, all under one retained credit.
- §3 arena: 16 MiB registered slabs cut into 1 MiB units, with contiguous run leases up to 8 MiB passed as slab base plus offset. Successfully registered slabs are kept on partial failure, the arena is charged to the reservation manager by a nonblocking reservation, BAR1 sizing is advisory, and a process-lifetime registry is reused across context re-creation with no teardown cudaFree or cuFileBufDeregister.
- §1/§7 thread model: the reaper does only nonblocking CUDA calls and bookkeeping. A per-(device, mount) submitter isolates an unexpectedly synchronous cuFileReadAsync, POSIX and sync-cuFile work runs on bounded blocking executors, and a recovery executor synchronizes only the owned lane after a partial enqueue failure.
- §6 writes go beyond transport: per-file submission sequence so flush waits only for preceding writes, serialization of conflicting granules between buffered edges and O_DIRECT bodies, short-write suffix retry, no blind replay after a timeout, and compat kept on D2H plus pwrite (Appendix C §3.5a).
- §9 runtime state machine: optional symbol groups, then config freeze, SetParameter, DriverOpen. It never calls GetP2PFlags, reports rather than reopens when another component already opened the driver, and enables only PAGE_ALIGNED_INPUTS until the other stream-register flags are verified.
- §10/§11: per-mount policy states with a circuit breaker and a decision ladder that is not process-wide; a retry taxonomy (only completed retryable failures, never concurrent with a timed-out original, no pathname reopen on ESTALE); quarantine instead of false completion when quiescence cannot be proven.
- Validation lists concrete Catch2 fault cases (independent absence of each optional symbol group, reused-event generations, partial arena registration, cancellation at every state transition, a single 1 GiB slice reaching configured QD). Gates apply per primary workload, and six phases each have a rollback.

**Cons**
- No automatic per-mount verification of real GDS. §10 leaves mounts 'eligible/unverified' unless proven externally, yet ladder rung 3 sends eligible mounts to async staging. If such a mount is actually compat, cuFileReadAsync runs synchronously on the single per-mount submitter, giving QD 1 against today's 4. Add a runtime demotion trigger such as submit-call latency or an nvidia-fs counter delta on a probe read.
- The compat read path changes without evidence. The table's last row routes known-compat files to pinned-host staging plus POSIX, whereas today compat reads use sync cuFileRead (Appendix C §2.2) and the POSIX device path reads through the buffered fd with 2 × 8 MiB scratch (C §2.5–2.6). Either keep sync cuFileRead as the compat read default or cite a measurement; a 5% gate alone only detects the regression.
- Validate-before-copy adds a host round trip between I/O and D2D for every op, and polling is described only as 'latency-aware' with 'short adaptive backoff'. Give numeric poll bounds, the expected extra slot and lane residence at 1 MiB ops, and the resulting QD loss. The Q4 answer ('under one core' plus 'one additional submission core', both belief) needs a per-op microsecond budget and a microbenchmark.
- No stream-ordered or event-returning variant, even opt-in. §4 handles the cross-thread join problem by avoidance, so pattern-1 callers must host-wait per task before queuing consumers or cudaFreeAsync. Sketch a two-phase API as a later phase, or show by measurement that the host wake-up is negligible for 2 GiB tasks.
- Network-FS content is generic 'inspect/verify' guidance. GPFS and Lustre op size is a fixed 2 MiB rather than derived from statfs block or stripe size. No concrete cuFileSetParameter* calls or cufile.json keys are enumerated for process_cufile_parameters. NIC/GPU/NUMA affinity is 'where possible' behind a worker_affinity knob, with no device-to-NUMA-to-thread mapping algorithm.
- Arena arithmetic under contention is unspecified. 256 MiB cannot hold the plan's own sweep points (8 MiB × QD 32–64 needs 256–512 MiB), nor two active network mounts at 80 lanes × 2 MiB (320 MiB). State the formula for effective per-mount QD and op size when the arena is shared, and bound buddy fragmentation for mixed 1–8 MiB runs in 16 MiB slabs.
- Thread and hand-off cost is not accounted for. Each op crosses scheduler to submitter to reaper to copy enqueue to reaper, and submitter threads scale with device × mount. Specify submitter creation, idle and teardown lifecycle, and the wake-up cost at 10k ops/s.
- Turning the existing reactors into 'admission facades' over a shared device engine is a large refactor with no seam-by-seam mapping. Say where set_cufile_transfer_hook_for_testing fires in the async path, what live_registrations_for_testing means with a process-lifetime arena, and how the sync-semantics tests in Appendix C §5(h).6 (:594, :660, :854) are re-expressed.

**Unique strengths**
- One physical op per stream lane, with lanes provisioned above the storage QD target. The competing plan binds slot k to stream k % M with M below its own QD target.
- A cap on input-waiting lanes (one per request, 1/8 of the device), so a busy caller stream cannot occupy the shared stream pool.
- Validate-before-copy, so unvalidated staging bytes are never copied into the destination, with an explicit aligned-retry and buffered-remainder protocol.
- Separation of blocking work from the reaper: per-mount submitter threads plus dedicated POSIX/sync executors and a recovery executor.
- Variable-size contiguous leases inside multiple registered slabs, keeping partial registration success, and coalescing nearby small slices into one physical read.
- Write-side semantics: per-file flush sequence fence, serialization of conflicting granules, short-write suffix retry, and a rule against replaying writes with unknown progress.
- Timeout and retry taxonomy with a per-mount circuit breaker, plus quarantine of lane, staging, args and file handle when quiescence cannot be established.
- A striped synchronous cuFile transport over the registered arena, used both as shippable Phase 2 and as the fallback (QD 8 local / 16 network) when async symbols are absent.
- Handling of a libcufile driver already opened by another component, and an event-publication protocol that guards against stale generations of pooled events.

## spectrum-scale-expert

**Pros**
- The GPFS lens section turns filesystem facts into planner rules: op boundaries snap to the FS block size (statfs f_bsize / mmlsfs -B) so each op maps to one NSD, and striping a 1 GiB slice into 128 × 8 MiB ops is tied explicitly to NSD parallelism. Verification uses mmdiag --iohist, mmdiag --rdma and perfquery.
- It gives a concrete automatic per-mount GDS check: a registered-ring probe read plus /proc/driver/nvidia-fs/stats deltas, cached per fsid in mount_profile and consulted by the write path. This directly replaces the process-wide gds_available() decision of Appendix C §3.2.
- Completion protocol: submit_event is recorded on the caller thread before enqueue, so the reactor's wait is never a no-op. The host-complete future stays the default, and P5 adds an opt-in device_readv_async_io_ev where the caller issues cudaStreamWaitEvent on its own thread, which is the correct shape for null/PTDS (Appendix C §6.2).
- The reaper keeps a per-stream FIFO and queries only the head event of each of M streams, O(M) per pass. Q4 gets a quantified answer (~10 µs per op, ~10% of a core at 10k ops/s) and an explicit idle policy (100 µs / 20 ms).
- The ring_slot pseudo-code holds persistent arg blocks and obj_keepalive per slot and releases the slot only in reap() after done_ev. That matches Appendix A §9 and the handle-lifetime invariant.
- The degradation ladder has a middle tier: an unregistered device ring that keeps alignment, read-around, scatter and async QD when cuFileBufRegister fails. Ring size is BAR1-clamped (min(0.5 × BAR1_free, 1 GiB), T4 at 16 × 4 MiB).
- It reuses uring pieces directly (slot_pool, staged_physical_range, dynamic_io_target with per-class clamps, request_scheduler), with a concrete new/modified file list including named test files. Low tier is capped at 25% of slots when high work is pending.
- P0 includes a standalone async-QD harness on GPFS before any reactor work, which de-risks the central [belief] (cuFileReadAsync at QD 64) early.

**Cons**
- QD is overstated. Slots bind to streams as k % M with M = 16 (NVMe) or 32 (GPFS/WEKA/VAST), while the table targets 32 and 48–64 'slots in flight'. cuFileReadAsync is stream-ordered (Appendix A §1), so ops on one stream serialize and storage QD is at most M, about half the target. Set M at or above target QD with one op per stream, or justify.
- Slot size contradicts the op clamps. Slots are a fixed 8 MiB (4 MiB on T4), but clamps reach 16 MiB (NVMe, GPFS snapped to FS block, Lustre). cuFile needs one contiguous buffer and the reused uring slot_pool leases non-contiguous blocks. Conversely, a 512 KiB VAST op or a few-KiB DISK-restore column pins a whole 8 MiB slot, and there is no coalescing. Add a contiguous run allocator or cap ops at slot_bytes, and coalesce small adjacent slices.
- Head-of-line blocking on the shared stream pool. The gate cudaStreamWaitEvent(io_stream, submit_event) is enqueued on per-device shared streams, so one request behind a busy user stream spreads across all M streams and stalls every other request on that device. The stated mitigation (re-count gated bytes so next_reactor steers away) cannot help because ring and streams are shared across reactors. This regresses the parked-slice liveness property (Appendix C §5(h), test :854). Query submit_event before admission and cap gated streams.
- Blocking work runs on the single submit+reap thread: fallback_posix(op) on submit error, the 'unchanged' staged D2H plus pwrite for write heads and tails (which host-blocks on slot events, C §3.3), and the sync cuFileRead fallback. Each stalls reaping and slot recycling for all in-flight async ops. The design also assumes cuFileReadAsync never blocks at submit on real GDS or in the unregistered-ring bounce tier. Move blocking paths to a separate executor.
- The D2D copy is speculative and retry is under-specified. It is enqueued before bytes_done is validated, so a short read copies stale staging bytes into the destination. resubmit_remainder does not say how a non-4 KiB-aligned remainder is handled, and has no retry bound, zero-progress rule or buffered fallback. The write pseudo-code never checks bytes_done or handles short writes.
- The concurrency model for the process-wide per-device stream pool is missing. Several reactor workers lease slots and enqueue read, memcpy and eventRecord on the same streams and FIFOs, with no ownership or locking. Calls can interleave and FIFO order can diverge from record order. Partition streams per reactor or define a per-stream lock and say which worker reaps.
- Compat regression risk and a ladder gap. Architecture says 'per reactor (default 2 threads)' while the compat row says QD = worker threads (4); halving workers halves sync-fallback QD on non-GDS hosts. The ladder also has no rung for async symbols present but mount not gds-verified, which is the T4 CI case.
- Timeouts, retries and error classification for network filesystems (Q6) are absent, and WEKA/VAST get one table row each. Add a watchdog, retryable versus fatal classification, and path-loss behaviour.
- The test plan is five file names and one sentence. Enumerate fault-injection cases (each optional symbol absent, ring registration failure, short read, event-record failure, shutdown mid-flight, a 1 GiB single slice reaching QD) and map every seam in Appendix C §5(h).6, not just the transfer hook.
- Shutdown 'leak the slots' after a logged timeout does not say what happens to the credit and future, the arg block, or the io_object/CUfileHandle whose deregistration with I/O in flight is undefined. State that all are quarantined and the future is never settled as success.
- The verification probe is racy and incomplete. nvidia-fs stats are system-wide, so concurrent I/O contaminates the delta, and the probe needs a readable file on a possibly empty spill mount. Specify serialization and a temp-file strategy.
- Several claims are stated as fact but are unverified: the per-op microsecond breakdown, 'sub-block reads cost a full block server-side', and '~2 ms on GPFS'. Mark them [belief] with a verification step.
- The P5 'final done_ev' is ill-defined when a request is striped lazily across M streams, and it resolves before short reads are detected. Specify a join stream or event set and the error semantics.
- The ring is registered with a single cuFileBufRegister, so one failure makes the whole ring unregistered. Add shrink-and-retry or slab registration. Flush ordering against in-flight async writes is also unspecified.

**Unique strengths**
- GPFS-specific planning: op boundaries snapped to the filesystem block size discovered via statfs, with an NSD round-robin rationale and mmdiag --iohist / --rdma checks.
- An automatic runtime per-mount GDS verification (probe read plus nvidia-fs stats delta, cached per fsid) that feeds both read and write path selection.
- An opt-in two-phase event-returning API (device_readv_async_io_ev) that lets the caller do the GPU-side join on its own thread.
- Head-of-FIFO-only event polling with a quantified per-op cost estimate.
- An unregistered-device-ring degradation tier that keeps async QD and alignment when BAR1 registration fails, instead of dropping straight to host staging.
- A standalone async-QD harness on GPFS in P0, to validate the transport assumption before building the reactor.

## weka-vast-expert

**Pros**
- (none)

**Cons**
- The plan body is empty. None of the §6 deliverables are present (requirements, architecture, per-filesystem table, read/write sequence diagrams, completion/join protocol, failure handling, API/config changes, file-level change list, test plan, benchmark gates, phases, risks). Submit a complete end-to-end plan.
- None of the eleven §5 questions is answered. At minimum, state transport per storage class with default QD, op size and stream counts; staging ring sizing and BAR1 fallback; the completion contract and cross-thread join; reaper signal and per-op cost; striping and priority; writes; the fallback ladder; shutdown; validation; phasing.
- The assigned lens is uncovered, and neither other plan fills it in depth. Provide wekafs kernel-client GDS specifics; VAST NFS-over-RDMA multipath, nconnect and NIC selection; Lustre/BeeGFS stripe alignment; per-filesystem QD and I/O-size sweet spots; and failure, timeout and path-loss behaviour, with [belief] markers and verification steps.

**Unique strengths**
- (none unique)

## Preliminary verdict

**Clear winner so far:** none

No clear winner yet, though gds-io-expert is the provisional leader. Its concurrency model is the only one that is internally consistent: one op per lane with lanes above QD, capped input waiters, validate-before-copy, blocking work kept off the reaper, and full write, flush, timeout and quarantine semantics. Its gaps are no automatic per-mount GDS verification, an unevidenced change to the compat read path, an unquantified reaper cost, no opt-in stream-ordered API, and generic network-FS guidance. spectrum-scale-expert contributes what gds-io-expert lacks: block-size-snapped GPFS planning, an automatic per-mount verification probe, a two-phase event API, O(M) reaping, and an early async-QD harness. But it has defects that undercut its headline numbers: storage QD is capped at M streams (16/32) rather than the stated 32–64, 8 MiB slots conflict with 16 MiB op clamps, the submit-event gate can stall the whole shared stream pool, and POSIX fallbacks block the single submit/reap thread. Those are fixable in revision. weka-vast-expert submitted nothing, so the WEKA/VAST/Lustre lens is unaddressed. A winner should be chosen after revisions, ideally one that combines the gds-io-expert engine with the spectrum-scale-expert mount-specific planning and verification.
