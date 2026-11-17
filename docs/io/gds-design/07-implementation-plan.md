# Implementation plan: cuFile async engine

Sources:
- `05-final-design.md`: the base spec
- `06-design-amendments.md`: overrides the base spec
- `checkpoints/decisions.md`

Branch: `io/cufile-async-engine`, starting at `600a3583`.

## Roles (user-selected)

| Role | Model |
|---|---|
| Implementer | `openai-codex/gpt-6.1-sol` @ high |
| Code reviewer | `anthropic/claude-opus-5-5` @ high |
| Tool-call / mechanical agents | `anthropic/claude-sonnet-5-5` |
| Lead (orchestration, verification, commits, checkpoints) | this session |

## Per-phase workflow

```
implement (gpt-6.1-sol) ──► review (opus-5.5, writes docs/io/gds-design/reviews/phase-N.md)
        ▲                                │
        └──────── fix (gpt-6.1-sol) ◄────┘ (blocking findings only)
lead: build cucascade_io_tests + full io test run + pre-commit on changed files → commit → checkpoints/phase-N.md
```

- **Tests at every phase.** The 609/610 baseline (1 skipped) must still pass,
  and the new tests must pass.
- **Default behaviour.** Behaviour stays unchanged by default (`mode=legacy`)
  until the hardware gates pass.

## Phases

### Phase 1: foundations (no behaviour change)
1. **`cufile_api` optional symbol groups.** Five groups: async-read,
   async-write, stream-register, batch, parameter setters/getters. A missing
   group disables only that capability; the 8 required symbols stay required.
   Tests use an injectable resolver.
2. **`include/cucascade/io/range_plan.hpp`.** Extract `staged_physical_range`,
   `split_write_range`, and the new `next_cut` (absolute-grid cuts plus
   geometry boundaries plus granule widening). uring and cufile use the
   extracted helpers with no behaviour change. Unit tests.
3. **`async_engine_config` in `cufile/config.hpp`.** Includes `mode` (default
   `legacy`), the sub-flags, `submitters_per_device`, `arena_bytes` (0 = auto),
   `mount_overrides`, `probe`, `demotion`, and `poll`. The config only parses
   and validates at this stage.
4. **P0 harness `benchmark/cufile_async_qd/` (standalone, no cudf).**
   - Persistent registered buffers, one op per stream, QD 1–128, ops of
     1/2/4/8 MiB.
   - Reports throughput, submit-call latency, and `bytes_done` timing.
   - Also measures the B-1 reaper-cost microbenchmarks: `cudaEventQuery`
     latency, record/D2D enqueue cost, and polling N lane heads.
5. **`tools/gds/collect_topology.sh`** (Sonnet agent). A read-only collector
   for target-host topology, GDS and filesystem information. It runs `gdsio`
   only when explicitly asked.

### Phase 2: resources and policy
`runtime` (init ordering, frozen params, `engine_possible`), `device_arena`
(injectable allocator and registrar, slabs, size classes, quotas, BAR1 sizing,
degradation), lanes, operation pool, capacity fit and water-filling,
`mount_policy` (key, state machine, overrides, probe with injectable evidence,
circuit breaker), `transport_simulator`. Tests: T-arena, T-policy, T-capacity.

### Phase 3: async engine reads
`device_engine` scheduler/reaper, a single submitter per device (AM-3), facade
hand-off in `dispatch_slice`, lazy striping, validate-before-copy,
short-read retry, AM-2 fence-before-D2D, AM-1 enqueued-and-joined completion
(host-complete for PTDS), cancellation, shutdown drain, recovery and
quarantine, priority/DRR scheduling, gauges, and `io_context.hpp` contract text.
Tests use the simulator, plus compat-mode libcufile through the AM-4 override.

### Phase 4: small-slice coalescing (AM-6) and the DISK-restore benchmark shape

### Phase 5: async write bodies, edges, flush sequencing, granule serialization

### Phase 6: topology and affinity, `cufile.json` parameter setters, filesystem profiles, docs (`docs/io/cufile-reactor.md`)

### Phase 7 (hardware): real-GDS gates G-1 to G-8 per filesystem, then a decision on the default flip

## Phases 1–3 explicitly do NOT touch

- The cudf layer (`src/cudf/*`), because it does not build. Its call-site
  migration is tracked in AM-1.
- The uring reactor's behaviour (only the helpers are extracted).
- Host-only and host+device cuFile paths.
