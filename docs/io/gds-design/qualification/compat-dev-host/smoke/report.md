# cuFile qualification report

N/A is not a pass. Compat/simulator results are NOT GDS evidence.
Raw nvidia-fs counters require isolated operator attribution (AM-11).

## Gates

| Gate | Result | Reason | Inputs |
|---|---|---|---|
| G-2 | N/A | lvm/sustained/device=0 ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=2.0, MiB=1.0 | lvm-d0-B-3-legacy-t1-op1.csv:2 |
| G-2 | N/A | lvm/sustained/device=0 ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=2.0, MiB=1.0 | lvm-d0-B-3-registered-t1-op1.csv:2 |
| G-2 | N/A | lvm/sustained/device=0 ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=2.0, MiB=2.0 | lvm-d0-B-3-legacy-t1-op2.csv:2 |
| G-2 | N/A | lvm/sustained/device=0 ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=2.0, MiB=2.0 | lvm-d0-B-3-registered-t1-op2.csv:2 |
| G-2 | N/A | lvm/restore/device=0 ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=2.0, MiB=1.0 | lvm-d0-B-4-legacy-t1-op1.csv:2 |
| G-2 | N/A | lvm/restore/device=0 ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=2.0, MiB=1.0 | lvm-d0-B-4-registered-t1-op1.csv:2 |
| G-2 | N/A | lvm/single/device=0 ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=2.0, MiB=1.0 | lvm-d0-B-5-legacy-t1-op1.csv:2 |
| G-2 | N/A | lvm/single/device=0 ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=2.0, MiB=1.0 | lvm-d0-B-5-registered-t1-op1.csv:2 |
| G-2 | N/A | lvm/spill/device=0 ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=2.0, MiB=1.0 | lvm-d0-B-6-legacy-t1-op1.csv:2 |
| G-2 | N/A | lvm/spill/device=0 ratio=None; requires >=0.8 and reviewed direct-path evidence; exact baseline QD=2.0, MiB=1.0 | lvm-d0-B-6-registered-t1-op1.csv:2 |
| G-1 | N/A | ('lvm', '0', 'slot', '1'): QD32/QD1=None; requires >=4 and direct evidence (GPFS blocking gate) | lvm-d0-B-0-async-slot-1-1.csv:2, lvm-d0-B-0-async-slot-2-1.csv:2 |
| G-1 | N/A | ('lvm', '0', 'slot', '2'): QD32/QD1=None; requires >=4 and direct evidence (GPFS blocking gate) | lvm-d0-B-0-async-slot-1-2.csv:2, lvm-d0-B-0-async-slot-2-2.csv:2 |
| G-1 | N/A | ('lvm', '0', '0', '1'): QD32/QD1=None; requires >=4 and direct evidence (GPFS blocking gate) | lvm-d0-B-0-async-0-1-1.csv:2, lvm-d0-B-0-async-0-2-1.csv:2 |
| G-1 | N/A | ('lvm', '0', '0', '2'): QD32/QD1=None; requires >=4 and direct evidence (GPFS blocking gate) | lvm-d0-B-0-async-0-1-2.csv:2, lvm-d0-B-0-async-0-2-2.csv:2 |
| G-3 | PASS | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.9905; requires >=0.9; backlog_ns=921297697.0 | lvm-d0-B-3-registered-t1-op1.csv:2 |
| G-5 | PASS | lvm/sustained/device=0 queries_per_op=2.762 (ceiling 70); residence_p99_us=131.072; CPU/residence budgets still require review | lvm-d0-B-3-registered-t1-op1.csv:2 |
| G-3 | PASS | lvm/sustained/device=0 time-integral backlog mean/Q_eff=0.9935; requires >=0.9; backlog_ns=903874725.0 | lvm-d0-B-3-registered-t1-op2.csv:2 |
| G-5 | PASS | lvm/sustained/device=0 queries_per_op=3.191 (ceiling 70); residence_p99_us=131.072; CPU/residence budgets still require review | lvm-d0-B-3-registered-t1-op2.csv:2 |
| G-3 | PASS | lvm/restore/device=0 time-integral backlog mean/Q_eff=0.98; requires >=0.9; backlog_ns=914284483.0 | lvm-d0-B-4-registered-t1-op1.csv:2 |
| G-5 | PASS | lvm/restore/device=0 queries_per_op=2.831 (ceiling 70); residence_p99_us=131.072; CPU/residence budgets still require review | lvm-d0-B-4-registered-t1-op1.csv:2 |
| G-3 | PASS | lvm/single/device=0 time-integral backlog mean/Q_eff=0.982; requires >=0.9; backlog_ns=805775813.0 | lvm-d0-B-5-registered-t1-op1.csv:2 |
| G-5 | PASS | lvm/single/device=0 queries_per_op=3.74 (ceiling 70); residence_p99_us=262.144; CPU/residence budgets still require review | lvm-d0-B-5-registered-t1-op1.csv:2 |
| G-3 | PASS | lvm/spill/device=0 time-integral backlog mean/Q_eff=0.9805; requires >=0.9; backlog_ns=865386139.0 | lvm-d0-B-6-registered-t1-op1.csv:2 |
| G-5 | PASS | lvm/spill/device=0 queries_per_op=2.666 (ceiling 70); residence_p99_us=0.000; CPU/residence budgets still require review | lvm-d0-B-6-registered-t1-op1.csv:2 |
| AM-9 | PASS | lvm/async-slot/device=0 registration=slot: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-async-slot-1-1.csv:2 |
| AM-9 | PASS | lvm/async-slot/device=0 registration=slot: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-async-slot-1-2.csv:2 |
| AM-9 | PASS | lvm/async-slot/device=0 registration=slot: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-async-slot-2-1.csv:2 |
| AM-9 | PASS | lvm/async-slot/device=0 registration=slot: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-async-slot-2-2.csv:2 |
| AM-9 | PASS | lvm/async-0/device=0 registration=0: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-async-0-1-1.csv:2 |
| AM-9 | PASS | lvm/async-0/device=0 registration=0: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-async-0-1-2.csv:2 |
| AM-9 | PASS | lvm/async-0/device=0 registration=0: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-async-0-2-1.csv:2 |
| AM-9 | PASS | lvm/async-0/device=0 registration=0: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-async-0-2-2.csv:2 |
| AM-9 | PASS | lvm/sync-slot/device=0 registration=slot: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-sync-slot-1-1.csv:2 |
| AM-9 | PASS | lvm/sync-slot/device=0 registration=slot: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-sync-slot-1-2.csv:2 |
| AM-9 | PASS | lvm/sync-slot/device=0 registration=slot: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-sync-slot-2-1.csv:2 |
| AM-9 | PASS | lvm/sync-slot/device=0 registration=slot: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-sync-slot-2-2.csv:2 |
| AM-9 | PASS | lvm/sync-0/device=0 registration=0: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-sync-0-1-1.csv:2 |
| AM-9 | PASS | lvm/sync-0/device=0 registration=0: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-sync-0-1-2.csv:2 |
| AM-9 | PASS | lvm/sync-0/device=0 registration=0: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-sync-0-2-1.csv:2 |
| AM-9 | PASS | lvm/sync-0/device=0 registration=0: content mismatches=0; full P0 verification required; this does not qualify shared registration for production | lvm-d0-B-0-sync-0-2-2.csv:2 |
| G-7 | N/A | ('lvm', '0', 'registered', 'lvm-d0-G-7-registered-t1-op1.csv.soak.csv'): duration=6.03221s, warmup=60s, samples=0, tolerance=0.01; instability=[], missing=['arena_bytes', 'registered_bytes', 'unregistered_bytes', 'pinned_host_bytes', 'op_pool_live', 'lane_count', 'thread_count'], errors=False, no_engine_progress=False | lvm-d0-G-7-registered-t1-op1.csv.soak.csv:2, lvm-d0-G-7-registered-t1-op1.csv.soak.csv:3, lvm-d0-G-7-registered-t1-op1.csv.soak.csv:4, lvm-d0-G-7-registered-t1-op1.csv.soak.csv:5, lvm-d0-G-7-registered-t1-op1.csv.soak.csv:6, lvm-d0-G-7-registered-t1-op1.csv.soak.csv:7 |
| G-4 | N/A | No applicable input rows |  |
| G-6 | N/A | Requires external test/sanitizer and multi-filesystem approval; tooling cannot authorize a default flip |  |
| G-8 | N/A | Requires external test/sanitizer and multi-filesystem approval; tooling cannot authorize a default flip |  |

## Matched baseline throughput

| Mount | Workload | Variant | GiB/s | Baseline GiB/s | Ratio | Source |
|---|---|---|---|---|---|---|
| lvm | sustained | legacy | 2.331 | None | None | lvm-d0-B-3-legacy-t1-op1.csv |
| lvm | sustained | registered | 0.981 | None | None | lvm-d0-B-3-registered-t1-op1.csv |
| lvm | sustained | legacy | 2.324 | None | None | lvm-d0-B-3-legacy-t1-op2.csv |
| lvm | sustained | registered | 1.253 | None | None | lvm-d0-B-3-registered-t1-op2.csv |
| lvm | restore | legacy | 3.932 | None | None | lvm-d0-B-4-legacy-t1-op1.csv |
| lvm | restore | registered | 1.434 | None | None | lvm-d0-B-4-registered-t1-op1.csv |
| lvm | single | legacy | 4.656 | None | None | lvm-d0-B-5-legacy-t1-op1.csv |
| lvm | single | registered | 2.67 | None | None | lvm-d0-B-5-registered-t1-op1.csv |
| lvm | spill | legacy | 3.407 | None | None | lvm-d0-B-6-legacy-t1-op1.csv |
| lvm | spill | registered | 1.487 | None | None | lvm-d0-B-6-registered-t1-op1.csv |
| lvm | sustained | legacy | 2.213 | None | None | lvm-d0-B-9-legacy-t1-op1.csv |
| lvm | sustained | registered | 1.707 | None | None | lvm-d0-B-9-registered-t1-op1.csv |
| lvm | sustained | posix | 1.225 | None | None | lvm-d0-B-9-posix-t1-op1.csv |
| lvm | sustained | forced-compat | 1.698 | None | None | lvm-d0-B-9-forced-compat-t1-op1.csv |

## AM-5 comparison

| Mount/device | Workload/mode | Op MiB (effective) | Registration | QD | GiB/s | Content mismatches | Source |
|---|---|---|---|---|---|---|---|
| lvm/0 | async/async-slot | 1 (0.0) | slot | 1 | 1.438 | 0 | lvm-d0-B-0-async-slot-1-1.csv |
| lvm/0 | async/async-slot | 2 (0.0) | slot | 1 | 1.306 | 0 | lvm-d0-B-0-async-slot-1-2.csv |
| lvm/0 | async/async-slot | 1 (0.0) | slot | 2 | 2.690 | 0 | lvm-d0-B-0-async-slot-2-1.csv |
| lvm/0 | async/async-slot | 2 (0.0) | slot | 2 | 2.259 | 0 | lvm-d0-B-0-async-slot-2-2.csv |
| lvm/0 | async/async-0 | 1 (0.0) | 0 | 1 | 1.383 | 0 | lvm-d0-B-0-async-0-1-1.csv |
| lvm/0 | async/async-0 | 2 (0.0) | 0 | 1 | 1.257 | 0 | lvm-d0-B-0-async-0-1-2.csv |
| lvm/0 | async/async-0 | 1 (0.0) | 0 | 2 | 2.237 | 0 | lvm-d0-B-0-async-0-2-1.csv |
| lvm/0 | async/async-0 | 2 (0.0) | 0 | 2 | 2.390 | 0 | lvm-d0-B-0-async-0-2-2.csv |
| lvm/0 | sync/sync-slot | 1 (0.0) | slot | 1 | 1.583 | 0 | lvm-d0-B-0-sync-slot-1-1.csv |
| lvm/0 | sync/sync-slot | 2 (0.0) | slot | 1 | 1.673 | 0 | lvm-d0-B-0-sync-slot-1-2.csv |
| lvm/0 | sync/sync-slot | 1 (0.0) | slot | 2 | 1.683 | 0 | lvm-d0-B-0-sync-slot-2-1.csv |
| lvm/0 | sync/sync-slot | 2 (0.0) | slot | 2 | 1.715 | 0 | lvm-d0-B-0-sync-slot-2-2.csv |
| lvm/0 | sync/sync-0 | 1 (0.0) | 0 | 1 | 1.625 | 0 | lvm-d0-B-0-sync-0-1-1.csv |
| lvm/0 | sync/sync-0 | 2 (0.0) | 0 | 1 | 1.579 | 0 | lvm-d0-B-0-sync-0-1-2.csv |
| lvm/0 | sync/sync-0 | 1 (0.0) | 0 | 2 | 1.610 | 0 | lvm-d0-B-0-sync-0-2-1.csv |
| lvm/0 | sync/sync-0 | 2 (0.0) | 0 | 2 | 1.655 | 0 | lvm-d0-B-0-sync-0-2-2.csv |
| lvm/0 | sustained/legacy/legacy | 1.0 (0.0) | slot | 2 | 2.331 | 0 | lvm-d0-B-3-legacy-t1-op1.csv |
| lvm/0 | sustained/async/registered | 1.0 (1.0) | slot | 2 | 0.981 | 0 | lvm-d0-B-3-registered-t1-op1.csv |
| lvm/0 | sustained/legacy/legacy | 2.0 (0.0) | slot | 2 | 2.324 | 0 | lvm-d0-B-3-legacy-t1-op2.csv |
| lvm/0 | sustained/async/registered | 2.0 (2.0) | slot | 2 | 1.253 | 0 | lvm-d0-B-3-registered-t1-op2.csv |
| lvm/0 | restore/legacy/legacy | 1.0 (0.0) | slot | 2 | 3.932 | 0 | lvm-d0-B-4-legacy-t1-op1.csv |
| lvm/0 | restore/async/registered | 1.0 (1.0) | slot | 2 | 1.434 | 0 | lvm-d0-B-4-registered-t1-op1.csv |
| lvm/0 | single/legacy/legacy | 1.0 (0.0) | slot | 2 | 4.656 | 0 | lvm-d0-B-5-legacy-t1-op1.csv |
| lvm/0 | single/async/registered | 1.0 (1.0) | slot | 2 | 2.670 | 0 | lvm-d0-B-5-registered-t1-op1.csv |
| lvm/0 | spill/legacy/legacy | 1.0 (0.0) | slot | 2 | 3.407 | 0 | lvm-d0-B-6-legacy-t1-op1.csv |
| lvm/0 | spill/async/registered | 1.0 (1.0) | slot | 2 | 1.487 | 0 | lvm-d0-B-6-registered-t1-op1.csv |

## AM-9 comparison

| Mount/device | Workload/mode | Op MiB (effective) | Registration | QD | GiB/s | Content mismatches | Source |
|---|---|---|---|---|---|---|---|
| lvm/0 | async/async-slot | 1 (0.0) | slot | 1 | 1.438 | 0 | lvm-d0-B-0-async-slot-1-1.csv |
| lvm/0 | async/async-slot | 2 (0.0) | slot | 1 | 1.306 | 0 | lvm-d0-B-0-async-slot-1-2.csv |
| lvm/0 | async/async-slot | 1 (0.0) | slot | 2 | 2.690 | 0 | lvm-d0-B-0-async-slot-2-1.csv |
| lvm/0 | async/async-slot | 2 (0.0) | slot | 2 | 2.259 | 0 | lvm-d0-B-0-async-slot-2-2.csv |
| lvm/0 | async/async-0 | 1 (0.0) | 0 | 1 | 1.383 | 0 | lvm-d0-B-0-async-0-1-1.csv |
| lvm/0 | async/async-0 | 2 (0.0) | 0 | 1 | 1.257 | 0 | lvm-d0-B-0-async-0-1-2.csv |
| lvm/0 | async/async-0 | 1 (0.0) | 0 | 2 | 2.237 | 0 | lvm-d0-B-0-async-0-2-1.csv |
| lvm/0 | async/async-0 | 2 (0.0) | 0 | 2 | 2.390 | 0 | lvm-d0-B-0-async-0-2-2.csv |
| lvm/0 | sync/sync-slot | 1 (0.0) | slot | 1 | 1.583 | 0 | lvm-d0-B-0-sync-slot-1-1.csv |
| lvm/0 | sync/sync-slot | 2 (0.0) | slot | 1 | 1.673 | 0 | lvm-d0-B-0-sync-slot-1-2.csv |
| lvm/0 | sync/sync-slot | 1 (0.0) | slot | 2 | 1.683 | 0 | lvm-d0-B-0-sync-slot-2-1.csv |
| lvm/0 | sync/sync-slot | 2 (0.0) | slot | 2 | 1.715 | 0 | lvm-d0-B-0-sync-slot-2-2.csv |
| lvm/0 | sync/sync-0 | 1 (0.0) | 0 | 1 | 1.625 | 0 | lvm-d0-B-0-sync-0-1-1.csv |
| lvm/0 | sync/sync-0 | 2 (0.0) | 0 | 1 | 1.579 | 0 | lvm-d0-B-0-sync-0-1-2.csv |
| lvm/0 | sync/sync-0 | 1 (0.0) | 0 | 2 | 1.610 | 0 | lvm-d0-B-0-sync-0-2-1.csv |
| lvm/0 | sync/sync-0 | 2 (0.0) | 0 | 2 | 1.655 | 0 | lvm-d0-B-0-sync-0-2-2.csv |

## AM-10 comparison

| Mount/device | Workload/mode | Op MiB (effective) | Registration | QD | GiB/s | Content mismatches | Source |
|---|---|---|---|---|---|---|---|
| lvm/0 | async/async-slot | 1 (0.0) | slot | 1 | 1.438 | 0 | lvm-d0-B-0-async-slot-1-1.csv |
| lvm/0 | async/async-slot | 2 (0.0) | slot | 1 | 1.306 | 0 | lvm-d0-B-0-async-slot-1-2.csv |
| lvm/0 | async/async-slot | 1 (0.0) | slot | 2 | 2.690 | 0 | lvm-d0-B-0-async-slot-2-1.csv |
| lvm/0 | async/async-slot | 2 (0.0) | slot | 2 | 2.259 | 0 | lvm-d0-B-0-async-slot-2-2.csv |
| lvm/0 | async/async-0 | 1 (0.0) | 0 | 1 | 1.383 | 0 | lvm-d0-B-0-async-0-1-1.csv |
| lvm/0 | async/async-0 | 2 (0.0) | 0 | 1 | 1.257 | 0 | lvm-d0-B-0-async-0-1-2.csv |
| lvm/0 | async/async-0 | 1 (0.0) | 0 | 2 | 2.237 | 0 | lvm-d0-B-0-async-0-2-1.csv |
| lvm/0 | async/async-0 | 2 (0.0) | 0 | 2 | 2.390 | 0 | lvm-d0-B-0-async-0-2-2.csv |
| lvm/0 | sync/sync-slot | 1 (0.0) | slot | 1 | 1.583 | 0 | lvm-d0-B-0-sync-slot-1-1.csv |
| lvm/0 | sync/sync-slot | 2 (0.0) | slot | 1 | 1.673 | 0 | lvm-d0-B-0-sync-slot-1-2.csv |
| lvm/0 | sync/sync-slot | 1 (0.0) | slot | 2 | 1.683 | 0 | lvm-d0-B-0-sync-slot-2-1.csv |
| lvm/0 | sync/sync-slot | 2 (0.0) | slot | 2 | 1.715 | 0 | lvm-d0-B-0-sync-slot-2-2.csv |
| lvm/0 | sync/sync-0 | 1 (0.0) | 0 | 1 | 1.625 | 0 | lvm-d0-B-0-sync-0-1-1.csv |
| lvm/0 | sync/sync-0 | 2 (0.0) | 0 | 1 | 1.579 | 0 | lvm-d0-B-0-sync-0-1-2.csv |
| lvm/0 | sync/sync-0 | 1 (0.0) | 0 | 2 | 1.610 | 0 | lvm-d0-B-0-sync-0-2-1.csv |
| lvm/0 | sync/sync-0 | 2 (0.0) | 0 | 2 | 1.655 | 0 | lvm-d0-B-0-sync-0-2-2.csv |
| lvm/0 | sustained/legacy/legacy | 1.0 (0.0) | slot | 2 | 2.213 | 0 | lvm-d0-B-9-legacy-t1-op1.csv |
| lvm/0 | sustained/async/registered | 1.0 (1.0) | slot | 2 | 1.707 | 0 | lvm-d0-B-9-registered-t1-op1.csv |
| lvm/0 | sustained/legacy/posix | 1.0 (0.0) | slot | 2 | 1.225 | 0 | lvm-d0-B-9-posix-t1-op1.csv |
| lvm/0 | sustained/async/forced-compat | 1.0 (1.0) | slot | 2 | 1.698 | 0 | lvm-d0-B-9-forced-compat-t1-op1.csv |
