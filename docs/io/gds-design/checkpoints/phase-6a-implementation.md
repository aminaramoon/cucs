# Phase 6a — engine performance and lifecycle hardening

Branch: `io/cufile-async-engine`; starting HEAD `bd2a235`. No commits, branch changes,
delegation, concurrent builds, or `src/cudf/` edits. Default mode remains `legacy`.
Started as `/gsd:execute-phase 6a`; this harness has no executable GSD command,
so execution was tracked with the task list and this checkpoint.

## Implementation

- `src/io/cufile/device_engine.cpp`, `include/cucascade/io/cufile/device_engine.hpp`:
  per-mount, read/write × slot-class completion histograms drive the AM-15 I/O
  ceiling; absolute event and fence-hint deadlines survive unrelated notifications
  and clocks starting at epoch zero. Query counters/gauges separate I/O, copy,
  stage and fence hints. Stale node generations are invalidated on every admission.
- The measurement-driven copy-tail correction is documented in AM-15: retain
  the 2 us first copy query, then use latency-adaptive backoff, bounded by
  max(250 us, configured copy floor). The 1 ms experiment got near 10 queries/op,
  but the tighter copy bound better protects lane residence/QD. This changes
  enabled-engine polling, not the default legacy reactor's existing event loops.
- Completed transient reads and read submit failures after owned-lane recovery
  have three total attempts, 10/20 ms base delays and bounded 25% jitter. ESTALE,
  EAGAIN, ETIMEDOUT and ENOTCONN qualify. Network EIO additionally needs a positive
  FS-specific classifier; production defaults to no EIO retry. Unknown-progress
  writes still fail without replay.
- `mount_policy.{hpp,cpp}` and `cufile_reactor.{hpp,cpp}`: weak live-inode file
  policies share directional demotion across handles, with identity-checked final
  eviction. Recognized unsupported-file errors no longer poison unrelated mount peers;
  other device/driver failures and slow/stuck submits remain mount-scoped
  after the Phase 6 review correction.
- Per-file geometry discovery has fake page/statfs/statx/stripe seams. Granules
  use max(4096, page size, reported DIO alignment), falling back only to
  max(4096, page size) after review (fstatfs block size is not alignment).
  A reported zero DIO alignment means legacy-only.
  GPFS geometry validates magic 0x47504653; Lustre tries the optional client
  header/ioctl and otherwise uses 1 MiB; BeeGFS uses 2 MiB. Mount profiles expose
  granule/geometry, probes receive the granule, and `range_plan.hpp` supports
  repeating geometry without materializing a file-sized boundary vector.
- Unsupported prewarmed granule/class combinations remain wholly legacy;
  writes also remain legacy when their granule exceeds the preallocated host
  cell. No per-transfer pinned growth was added.
- `lane_pool.{hpp,cpp}`: recovery quarantines, synchronizes and replaces the
  owned lane's stream and both events off-reaper; failure retains quarantine.
  A replacement is built before destroying the old lane, so failed construction
  cannot accidentally return the suspect lane to service.
- `device_arena.{hpp,cpp}`, `lane_pool.{hpp,cpp}`, `operation.hpp`: nonblocking
  metadata-return APIs leave ownership intact on contention. Lane → slot → op
  acquisition misses use bootstrap-bounded deferred return queues. Quiescent
  lane/slot/op metadata retires on the reaper; final file, scratch, interval and
  cursor owners still retire on the independent cleanup executor.
- `templated_ioctx.hpp` passes file identity to the caller fence hook. Known
  compat, legacy and non-routable policies skip event creation/recording and the
  fence pool. A counting regression verifies zero records on skipped paths.
  Policy lookup itself is still necessary to identify these paths.
- `io_request.hpp`: coordinator failure tokens register weakly, fire at most
  once per token, immediately after failure if registered late, and run outside
  coordinator locks. Callbacks must be nonblocking/noexcept and weak-lock their
  backend target; an acquired callback may race token destruction. Engine cursors
  retain notification tokens, whose callbacks hold only a weak engine impl and
  signal its eventfd. External failures cancel pending siblings without health
  polling; late failure after the real engine has retired/died is tested.
- `tools/gds/cufile_engine_bench.cpp`: additive query breakdown columns and
  post-I/O residence diagnostics. `config.hpp` validates polling floors <= 1 ms.
- Tests: new `test/io/cufile/test_engine_hardening.cpp`; updates in
  `test_engine_{ioctx,sched,write}.cpp`, `test_mount_policy.cpp`, and
  `test/CMakeLists.txt`. Normative contracts updated in `06-design-amendments.md`.

## Performance evidence (compat libcufile 1170, NOT GDS)

`/mnt/disk_2` is absent. `/tmp` is ext4; the workspace is btrfs and was not used
for the comparison. The patterned input is `/tmp/p6a-bench.dat` (4 GiB).

Common command:

```sh
./build/release/cucascade_cufile_engine_bench --file /tmp/p6a-bench.dat \
  --workload sustained --engine async --override fs_type:ext4 \
  --require-direct-evidence 0 --qd 32 --op-mib 4 --drop-cache
```

Before modification, verified runs stayed legacy during timing:
0 queries/op, 3.824–3.874 GiB/s, QD 0, all timed bytes legacy. The old claim
that verification *caused demotion* was not established: those CSV/logs did
not capture circuit state, demotion counts or the triggering submit latency,
so they cannot distinguish the slow-submit breaker from the stuck watchdog
(or prove either tripped). No root cause is claimed, and these rows must not
be used as a diagnosed regression or as **engine baselines**. The separately
measured `--verify 0` run preserved async routing and provided the baseline. Every comparison row below transferred
8 GiB asynchronously with zero legacy bytes and advisory eviction before timing.

| Measurement | queries/op | GiB/s | mean QD |
|---|---:|---:|---:|
| Before, `--verify 0` | 250.077 | 6.584 | 28.234 |
| After, `--verify 0`, run 1 | 28.803 | 6.560 | 24.190 |
| After, `--verify 0`, run 2 | 19.654 | 6.577 | 28.613 |
| After, `--verify 0`, run 3 | 19.935 | 6.547 | 28.392 |
| After, three-run arithmetic mean | **22.797** | **6.561** | **27.065** |
| After, default verification enabled | **19.480** | **6.578** | **28.451** |

Matched `--verify 0` means: queries down 90.9%, throughput down 0.34%, mean QD
down 4.14%. These satisfy the requested limits **on the three-run average**,
not a claim that each sample stays within 5% QD: run 1 is outside that bound.
QD varies with the completion histogram's 8.39/16.78 ms median buckets. The
single baseline and short windows are not a confidence interval or G-5 evidence.
The ceiling of 70 queries/op is met; the aspirational target of about 8 is not.

Final verified breakdown: I/O 1.470, copy 17.542, stage 0, fence hint 0.469
queries/op. The initial breakdown experiment showed about 248 copy queries/op
versus only 1.7 I/O queries/op, explaining why changing only the I/O cap was
insufficient. No inference of direct GDS throughput or default-mode eligibility.

Raw logs/CSV for this session: `/tmp/p6a-before-noverify.{log,csv}`,
`/tmp/p6a-250-{1,2,3}.{log,csv}`, `/tmp/p6a-250-verified.{log,csv}`.

## Validation

All commands ran serially.

- Requested release build of `cucascade_io_tests`, `cucascade_cufile_async_qd`
  and `cucascade_cufile_engine_bench`: passed, warnings-as-errors enabled.
- Final full suite: **907 cases: 906 passed, 1 existing kTLS skip**;
  **3,037,569 assertions passed**. The two initial failures were obsolete profile
  expectations that Lustre/BeeGFS geometry was zero; updated to the requested
  conservative geometry defaults.
- Final `[engine] --order rand --rng-seed $RANDOM`: **10/10 passed**,
  **121 cases / 5,359 assertions each**. Seeds: 27416, 14204, 7243, 4425, 8401,
  16234, 4581, 14538, 28889, 13103. An earlier complete ten-run loop also passed.
- New coverage includes fake-clock deadlines/epoch-zero hints, p50 cap scaling,
  completion/submit transient scripts and bounded failure, network EIO classifier
  gating, no replay of transient negative writes, per-file demotion, copy/record
  failure lane replacement, granule/geometry plumbing, nonblocking metadata return,
  counted fence skips, prompt external sibling cancellation, weak exactly-once
  callbacks and late failure after actual engine destruction.
- `pixi run python -m pre_commit run --files <all changed/new files>`: passed
  after automatic formatting; final checkpoint is included in the final rerun.
- `git diff --check`: clean. Refreshed the changed-file symwhere index; the
  pre-existing symhow database remains stale and was not relied on for new code.

## Remaining qualification boundaries

No real GPFS/Lustre/BeeGFS/GDS host was available. EIO has no speculative generic
network retry classifier. Large-granule writes/class mismatches use safe legacy
routing rather than growing resource budgets. AM-13/AM-14 Phase 7 scheduler,
notification batching and high-R CPU qualification work remains deferred. The
engine remains legacy by default and this work does not claim NFR-3/NFR-4 or GDS
acceptance qualification.
