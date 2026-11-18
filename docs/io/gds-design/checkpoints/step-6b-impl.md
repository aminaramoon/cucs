# Phase 3b implementation: reactor/ioctx wiring

No commit, branch change, helper agents, concurrent builds, or `src/cudf/` edits.
Execution context: `phase-3-engine-reads` / `p3b-impl`. The requested
`/gsd:execute-phase 3b` entry point was attempted; this harness has no GSD command.

## Integration

- `templated_ioctx::dispatch_slices` uses a compile-time `wants_submit_fence`
  capability (cuFile only), then a runtime mode/capability gate. Original PTDS
  and null identities are captured before the unchanged default-stream resolver.
  Timing-disabled events come from a process-lifetime per-device pool; all
  distinct resolved device/stream dependencies are recorded before publication.
- `device_buffer` retains the optional shared fence and original stream identity.
  `engine_fence` aliases that backend-neutral fence type, preserving Phase 3a APIs.
- `cufile_ioctx::start` schedules initialization, arena warm-up, lane/submitter
  construction and per-device binding publication on the control executor.
  A binding is published only after bootstrap succeeds. Failures retain legacy
  routing. Explicit simulator bindings use isolated resources without replacing
  the production runtime or shared engine registry.
- Facade handoff precedes the legacy stream gate. It transfers the existing
  credit, file, priority, stream owner, fence and per-facade cancellation flag.
  Handoffs consume neither synchronous byte nor slice pass budgets.
- Shared retirement tickets track physical retirement, not AM-1 early success.
  Shutdown closes facade admission, cancels its pending pieces and waits up to
  30 seconds; an incomplete drain is logged and resources remain retained.
  Repeated shutdown retries the wait. The shared engine is never stopped.

## Routing decision table

| Condition | Result |
| --- | --- |
| `mode=legacy`, or `async_reads=false` | Existing legacy path |
| Write/flush, host-only, host+device pair | Existing legacy path |
| No fence, original null stream, or callback-bearing prepared slice | Existing legacy path |
| File lacks cuFile registration or a direct fd | Existing legacy/POSIX decision |
| Binding unpublished, engine stopped/unready, policy setup fails | Existing legacy path |
| Mount unknown/probing, known-compat, unsupported, circuit-open/cooldown, or explicit legacy override | Existing legacy path; `ensure_probe` only schedules control work |
| Eligible device-only read on an engine-routable policy | Shared engine |
| Original PTDS caller on engine route | Engine, host-complete future |
| Explicit resolved stream on engine route | Engine, enqueued-and-joined future |

Callback-bearing device slices conservatively retain their existing legacy
callback/completion ordering; they are not silently handed off without a callback.

## Lifetime/cancellation corrections exposed by integration

1. AM-1 successes can settle credits before their destination copies execute.
   A later sibling error must not resolve a failed future while those copies
   still touch caller memory. Engine handoff now installs group accessor-
   retirement guards. Success remains early; errors additionally await all
   engine guards, including guards on other devices/facades in the same group.
2. A facade must cancel its unstarted commands even when a submitter is stuck in
   another facade's call. The reaper removes cancelled commands from the MPMC
   mount queue and requeues healthy survivors. Winning the pop proves that the
   command never started. The submitter also checks per-request cancellation.

## Contract and seams

`io_context.hpp` now says legacy, compat and POSIX reads are host-complete;
engine reads validate I/O, enqueue destination copies and join the resolved user
stream before successful resolution. Host/foreign-stream consumers must wait on
that stream and keep destinations alive through that wait. Original PTDS callers
are host-complete. Failed/cancelled work drains. The AM-1 migration note names
`src/cudf/disk_io.cpp`, `check_transferred`, benchmarks and the legacy
complete-at-resolve test. No cudf call site was modified.

The transfer hook runs on engine submitters, blocking fallback workers and probe
control work immediately before each transport call, including retries. The copy
record-failure hook reaches engine copy records. Copy delay uses pre-created
per-device test gate streams/events, with no host sleep/allocation on the reaper.
A completion observer reports operation identity, generation, bytes and validation
outcome before destination copying. Persistent registered-byte/slab counters
query the per-device registry arena; transient registration counters are unchanged.

## Validation

- Release `cucascade_io_tests` build with `-j 20`, serial build invocations.
- Full suite: 830 cases, 829 passed, one existing kTLS skip.
- Engine suite: 46 cases, 1,227 assertions.
- Public ioctx engine coverage: 18 cases, including the PTDS-built stream matrix;
  randomized serial runs cover varied test order.
- Simulator and real compat libcufile tests validate nonuniform contents, user-
  stream ordering, enqueued completion, PTDS host completion, null/disabled
  routing, mixed host/device slices and pairs, probe fallback/promotion,
  independent busy streams, facade isolation, queued/submitted cancellation,
  late sibling errors, persistent slot registrations and the existing test seams.
- Existing cuFile reactor/write test bodies remain unchanged.
- Pre-commit and `git diff --check` pass after formatting.

## Review/follow-up

- Real-GDS G-gates, throughput and CPU-budget qualification remain outstanding;
  default mode is still legacy. Phase 3a retry/demotion/resource limits remain.
- Callback-bearing prepared slices deliberately stay legacy.
- Public simulator tests still require an opened cuFile-registered file; they
  skip if compat libcufile/file registration is unavailable.
- The existing caller-side PTDS stand-in creation can serialize behind PTDS GPU
  work on this CUDA driver after real cuFile stream registration. Its regression
  test uses a finite producer rather than an infinite GPU gate. Resolution and
  legacy semantics were not changed as part of this phase.
- Production persistent engines/arenas and pooled fence events are intentionally
  process-lifetime. Shutdown's incomplete-drain timeout is diagnostic, not a
  fictitious completion or resource reclamation.

## Phase 3 review-fix execution (p3-fix)

Continued through `/gsd:execute-phase 3` in the existing
`phase-3-engine-reads` workflow; no executable/tool GSD entry point is installed
in this harness. No commit, delegation, concurrent builds or src/cudf/ edits.

The review-fix update supersedes the earlier bootstrap/contract description:
warm-up is per-active-device on first use, not eager at start; non-legacy
completion is a context-level enqueued contract, not a per-route promise.
AM-12 adds a mount-wide cap on fence-blocked leases. AM-13 records fallback,
legacy redispatch, cancellation, CPU qualification and lifecycle follow-up.
Every finding P3-R1 through P3-R26 has a resolution in
`reviews/phase-3.md`; R2's remaining scheduler-container/notification work is
explicitly Phase 7 G-5 work, and remaining R14/R15/R18 NIT portions are Phase 6.

Final validation: release target built serially; full suite 841 cases
(840 passed, one existing kTLS skip), 3,035,489 passing assertions. Final engine
loop passed 20/20 randomized runs with all 24 CPUs loaded, 57 cases / 1,664
assertions each. An initial 19/20 loaded loop found an additional probe
qualification/preemption race, fixed before repeating the entire loop.
A missing-join mutation failed the new exact-content consumer test as expected;
the join was restored and rebuilt. Pre-commit and diff whitespace checks passed.
