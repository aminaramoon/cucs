# cuFile qualification report

N/A is not a pass. Compat/simulator results are NOT GDS evidence.
Raw nvidia-fs counters require isolated operator attribution (AM-11).

## Gates

| Gate | Result | Reason | Inputs |
|---|---|---|---|
| G-4 | PASS | ('unspecified', '0', 'sustained', '4', '536870912', '2147483648', '1', 'legacy'): paired mean new/old=0.996178223121194; requires >=0.95; complete alternating pairs=True | b10.csv:10, b10.csv:11, b10.csv:2, b10.csv:3, b10.csv:4, b10.csv:5, b10.csv:6, b10.csv:7, b10.csv:8, b10.csv:9 |
| G-4 | PASS | ('unspecified', '0', 'single', '4', '536870912', '2147483648', '1', 'legacy'): paired mean new/old=0.9990598107352255; requires >=0.95; complete alternating pairs=True | b10.csv:12, b10.csv:13, b10.csv:14, b10.csv:15, b10.csv:16, b10.csv:17, b10.csv:18, b10.csv:19, b10.csv:20, b10.csv:21 |
| G-4 | PASS | ('unspecified', '0', 'spill', '4', '536870912', '2147483648', '1', 'legacy'): paired mean new/old=0.9930071044611213; requires >=0.95; complete alternating pairs=True | b10.csv:22, b10.csv:23, b10.csv:24, b10.csv:25, b10.csv:26, b10.csv:27, b10.csv:28, b10.csv:29, b10.csv:30, b10.csv:31 |
| G-1 | N/A | No applicable input rows |  |
| G-2 | N/A | No applicable input rows |  |
| G-3 | N/A | No applicable input rows |  |
| G-5 | N/A | No applicable input rows |  |
| G-7 | N/A | No applicable input rows |  |
| G-6 | N/A | Requires external test/sanitizer and multi-filesystem approval; tooling cannot authorize a default flip |  |
| G-8 | N/A | Requires external test/sanitizer and multi-filesystem approval; tooling cannot authorize a default flip |  |

## Matched baseline throughput

| Mount | Workload | Variant | GiB/s | Baseline GiB/s | Ratio | Source |
|---|---|---|---|---|---|---|

## AM-5 comparison

| Mount/device | Workload/mode | Op MiB (effective) | Registration | QD | GiB/s | Content mismatches | Source |
|---|---|---|---|---|---|---|---|

## AM-9 comparison

| Mount/device | Workload/mode | Op MiB (effective) | Registration | QD | GiB/s | Content mismatches | Source |
|---|---|---|---|---|---|---|---|

## AM-10 comparison

| Mount/device | Workload/mode | Op MiB (effective) | Registration | QD | GiB/s | Content mismatches | Source |
|---|---|---|---|---|---|---|---|
| unspecified/0 | sustained/legacy/old | ? (0.0) |  | ? | 6.47888088 | 0 | b10.csv |
| unspecified/0 | sustained/legacy/new | ? (0.0) |  | ? | 6.4118269 | 0 | b10.csv |
| unspecified/0 | sustained/legacy/new | ? (0.0) |  | ? | 6.41543843 | 0 | b10.csv |
| unspecified/0 | sustained/legacy/old | ? (0.0) |  | ? | 6.4964404 | 0 | b10.csv |
| unspecified/0 | sustained/legacy/old | ? (0.0) |  | ? | 6.4172143 | 0 | b10.csv |
| unspecified/0 | sustained/legacy/new | ? (0.0) |  | ? | 6.43683056 | 0 | b10.csv |
| unspecified/0 | sustained/legacy/new | ? (0.0) |  | ? | 6.4529358 | 0 | b10.csv |
| unspecified/0 | sustained/legacy/old | ? (0.0) |  | ? | 6.41965429 | 0 | b10.csv |
| unspecified/0 | sustained/legacy/old | ? (0.0) |  | ? | 6.44219894 | 0 | b10.csv |
| unspecified/0 | sustained/legacy/new | ? (0.0) |  | ? | 6.41300467 | 0 | b10.csv |
| unspecified/0 | single/legacy/old | ? (0.0) |  | ? | 6.27026236 | 0 | b10.csv |
| unspecified/0 | single/legacy/new | ? (0.0) |  | ? | 6.25910997 | 0 | b10.csv |
| unspecified/0 | single/legacy/new | ? (0.0) |  | ? | 6.24471096 | 0 | b10.csv |
| unspecified/0 | single/legacy/old | ? (0.0) |  | ? | 6.3030516 | 0 | b10.csv |
| unspecified/0 | single/legacy/old | ? (0.0) |  | ? | 6.20530363 | 0 | b10.csv |
| unspecified/0 | single/legacy/new | ? (0.0) |  | ? | 6.26045348 | 0 | b10.csv |
| unspecified/0 | single/legacy/new | ? (0.0) |  | ? | 6.27990443 | 0 | b10.csv |
| unspecified/0 | single/legacy/old | ? (0.0) |  | ? | 6.29303963 | 0 | b10.csv |
| unspecified/0 | single/legacy/old | ? (0.0) |  | ? | 6.23759073 | 0 | b10.csv |
| unspecified/0 | single/legacy/new | ? (0.0) |  | ? | 6.23467983 | 0 | b10.csv |
| unspecified/0 | spill/legacy/old | ? (0.0) |  | ? | 4.69267277 | 0 | b10.csv |
| unspecified/0 | spill/legacy/new | ? (0.0) |  | ? | 4.7422511 | 0 | b10.csv |
| unspecified/0 | spill/legacy/new | ? (0.0) |  | ? | 4.68347484 | 0 | b10.csv |
| unspecified/0 | spill/legacy/old | ? (0.0) |  | ? | 4.69785583 | 0 | b10.csv |
| unspecified/0 | spill/legacy/old | ? (0.0) |  | ? | 4.74274465 | 0 | b10.csv |
| unspecified/0 | spill/legacy/new | ? (0.0) |  | ? | 4.74972079 | 0 | b10.csv |
| unspecified/0 | spill/legacy/new | ? (0.0) |  | ? | 4.73951417 | 0 | b10.csv |
| unspecified/0 | spill/legacy/old | ? (0.0) |  | ? | 4.80173285 | 0 | b10.csv |
| unspecified/0 | spill/legacy/old | ? (0.0) |  | ? | 4.9242104 | 0 | b10.csv |
| unspecified/0 | spill/legacy/new | ? (0.0) |  | ? | 4.77164995 | 0 | b10.csv |
