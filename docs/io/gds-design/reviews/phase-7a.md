# Phase 7a review: non-GDS qualification tooling and compat-host results

**Scope:** `io/cufile-async-engine` at HEAD `6f6b82e` plus the working tree:
- `git diff HEAD`: `.gitignore`, `CMakeLists.txt`, the `asan` preset in `CMakePresets.json`, `tools/gds/README.md`, and the soak, `--posix-only` and CSV columns in `cufile_engine_bench.cpp`
- untracked `tools/gds/{qualify.sh, qualify.example.conf, run_b10.sh, analyze_gates.py, qualification_support.py, legacy_compat_bench.cpp, sanitizer.supp, QUALIFICATION.md, tests/}`
- the run agent's output in `docs/io/gds-design/qualification/compat-dev-host/`

**Reference:** `05-final-design.md` §8.2 (B-0..B-10, G-1..G-8), NFR-1..NFR-6, §9, and the amendments AM-4, AM-5, AM-9, AM-10, AM-11, AM-13, AM-15 and AM-16.

**Severity scale:**
- **BLOCKING** means fix before the Phase 7b hardware handoff. Otherwise hardware time produces invalid or wrongly-passing gate evidence.
- **SHOULD** means fix before the affected gate is signed off or before the G-8 default flip.
- **NIT** is cosmetic or documentation only.

I checked the analyzer by reading the code against the committed result CSVs. `python3 -m unittest discover tools/gds/tests` passes (15 tests), and `bash -n qualify.sh` is clean. No code was modified.

---

## 1. Summary

The plumbing is careful:
- The manifest records the exact command, stdout/stderr and exit code for every case.
- Inputs are created with O_EXCL.
- `--dry-run` really performs no writes.
- Partial data from a failed command fails the gate.
- G-1 and G-2 cannot pass without an operator attestation.
- The B-10 runner builds the same injected source in both trees and alternates the old/new order.

The **G-4 legacy-mode result is credible.**

However, the analyzer can still **report PASS for things it did not measure**:
- **Routing:** no gate checks that the "engine" row actually ran on the engine. The step-3 B-6 run labelled `registered` moved 0 engine bytes. The async soak was 83 % legacy and still got PASS.
- **G-2** scores legacy-variant rows.
- **G-5** turns a single sub-criterion into PASS.
- **G-3** passes on compat data.
- **Coverage:** gates are emitted per row and there is no required-coverage check, so a cell that never ran (e.g. B-6 with `ALLOW_WRITES=0`) is silently absent rather than N/A.

Two of the run agent's product bugs (BUG-1, BUG-2) would make a hardware run produce mostly legacy data, or abort the write runs. Both reproduce locally in minutes, so they should be root-caused before scarce GDS time is spent.

---

## 2. Findings

### P7a-R1: BLOCKING (High): no gate verifies that an engine row actually used the engine

**Problem.** `analyze_gates.py` treats every row whose variant/engine is async as engine evidence. It never reads `engine_logical_bytes`, `legacy_bytes` or `sync_bytes`. Evidence from this run:
- `step3/lvm-d0-B-6-registered-t4-op4.csv`: `admission=test_only_compat`, `transport=libcufile`, `engine_logical_bytes=0`, `legacy_bytes=12.9 GB`. This is a pure legacy run carrying the engine label.
- `lvm-d0-B-4-registered-t4-op4.csv`: 27 % legacy plus 2.4 MB of sync fallback.
- `soak/async`: 13 of 20 epochs had 0 engine bytes, and 879 GB engine vs 4 402 GB legacy overall. G-7 still reports PASS, because `no_progress` (`analyze_gates.py:414`) is a cumulative `all(completed_ops == 0)`. The per-epoch CSVs, which contain the routing split, are deliberately "consumed" and ignored.

The same hole exists on hardware. In `qualify.example.conf`, `OPERATOR_OVERRIDE=0` is the default. AM-11 makes automatic direct evidence unavailable, so the `registered` variant (`async_auto`) never qualifies a mount and **every engine row of a default hardware run is legacy**. G-2 would compare legacy throughput, measured at the requested QD, against the matched baseline. If an operator attests the CSV, it can PASS.

The `admission` and `transport` columns record configuration labels, not what actually happened (AM-14 already says so).

**Fix.**
1. Compute `engine_fraction = engine_logical_bytes / (engine_logical_bytes + legacy_bytes + sync_bytes)` for every async/simulated row.
   - G-2, G-3 and G-5 may PASS only if `engine_fraction >= 0.99` and `legacy_bytes == 0` (or a documented tolerance).
   - Otherwise the status is N/A with the reason "routed legacy/sync: X %". For G-2 it is FAIL if the row was meant to be engine-only and was demoted mid-run.
2. G-7 must:
   - read the `*.epoch-N.csv` summaries and require every post-warm-up epoch to have `engine_fraction >= 0.99`;
   - require a positive `completed_ops` **delta** between consecutive samples within an epoch.

   If either check fails, the status is FAIL (resource-stable but engine-off is not a soak).
3. Add mount-policy gauges to the epoch CSV and the soak sample: state, demotions, cooldowns, current cooldown, last trigger (slow-of-8 / stuck / failure), and healthy p99. This makes demotion attributable.
4. In `qualify.sh`, when not in smoke mode and `OPERATOR_OVERRIDE=0`, refuse the engine steps (B-3..B-6, B-8, B-9 `registered`, G-7) with an explicit message, or require `--allow-legacy-engine-rows`. Today the documentation only says "unqualified workloads can stay legacy".
5. Add analyzer unit tests for a demoted row, a fully legacy "registered" row, and a soak with idle epochs.

### P7a-R2: BLOCKING (High): G-2 is computed for legacy-variant rows

**Problem.** The G-2 loop (`analyze_gates.py:193-235`) gates every B-3..B-6 row and does not filter on variant or engine. In the smoke report, `lvm-d0-B-3-legacy-t1-op1.csv` gets its own G-2 row.
- For legacy rows `Q_eff_max=0`, so the match falls back to the requested QD (32). The comparison is then against a QD-32 gdsio baseline that the sync legacy path never ran at.
- On real GDS, the legacy cuFile sync path can reach 80 % at small QD. An attested legacy row would then show **G-2 PASS** next to the engine's FAIL or N/A.

G-2 (NFR-1) is a property of the async engine.

**Fix.**
- Emit G-2 only for `engine in {async}`, excluding simulated, and only for engine-routed rows (R1).
- Report legacy rows in the performance table as a reference column ("legacy / baseline"), with no gate status.

### P7a-R3: BLOCKING (High): G-5 reports PASS on one of about seven criteria

**Problem.** §8.2 G-5 says "B-1 and B-3 meet NFR-3 and NFR-4". The analyzer (`:288-305`) checks only `queries_per_op <= 70`, which is the AM-15 ceiling. The reason text says "CPU/residence budgets still require review", but the status is **PASS**, and `SUMMARY.md` §2 repeats "G-5 PASS". The following are not measured:
- NFR-3 submitter µs/op (≤ 12), reaper bookkeeping µs/op (≤ 10), timer/notification µs/op (≤ 5), the ≤ 1 core total, and the 8 queries/op target, all at the defined operating point (1 MiB × 10 k ops/s, QD ≤ 64).
- NFR-4 mean residence ≤ 65 µs at 10 k ops/s, and storage QD ≥ 95 % of `Q_eff` under saturation.
- The AM-13/AM-14/AM-16 contention items: waiter scans, publication wakeups, inode guards.

Residence is reported as `residence_p99_us`. `percentile()` in `cufile_engine_bench.cpp` returns the log2 bucket's upper bound (131.072 µs = 2^17 ns), which is a factor-2 resolution. It cannot be compared with a 65 µs **mean**. B-1 rows are never evaluated, because the engine filter requires an `engine` column that B-1 CSVs don't have.

**Fix.**
- Rename the current check to `G-5a (AM-15 queries/op)`.
- Keep `G-5` itself N/A until the NFR-3 and NFR-4 quantities are produced: per-thread CPU time from `/proc/self/task/*/stat` or `getrusage` per role, mean residence (sum/count), and a rate-limited 10 k ops/s mode in the engine bench.
- Evaluate B-1 rows.
- Correct the `SUMMARY.md` wording: "G-5a: PASS on reads (14–47), FAIL on one spill run (177.5)".

### P7a-R4: BLOCKING (Medium): G-3 passes or fails on compat and simulator data (run agent's BUG-4)

**Problem.** G-3 (`:270-287`) has no `direct_qualified` or compat filter. On this host it emits PASS ×5 (smoke) and FAIL ×12 (step 3). On a GDS host, a row from an unverified, compat-fallback or simulated mount can PASS. That is inconsistent with G-1 and G-2 and with the "compat is not GDS evidence" rule.

**Fix.**
- PASS requires `direct_qualified` (R1 routing plus the attestation).
- Compat or simulated rows get an `INFO` status with the ratio, which keeps the scheduler signal, and never PASS or FAIL.
- Add a unit test.

### P7a-R5: SHOULD (Medium): G-3's backlog window counts pass tails, so compat FAILs are probably an artifact

**Problem.** `device_engine.cpp:582-600` (`account`) counts time as backlog while **any request is unfinished**. It counts a refill gap whenever any mount has `in_io < effective.qd`. The bench drains at every pass boundary. Each 1 GiB pass at QD 32 × 4 MiB ends with a ramp-down tail in which no unsubmitted pieces exist, and that tail is counted as under-filled backlog.

With 0.15 s passes, the tail alone bounds the ratio near 0.85–0.9. That matches the observed 0.72–0.90 better than "real scheduler under-fill" does. NFR-5's "while backlog exists" means storage-eligible, unsubmitted work.

"No generation-wide refill gaps", the other half of G-3, is not evaluated at all, even though `refill_gap_ns` is in the CSV.

**Fix.**
- Accumulate the backlog integral only while at least one admitted piece is waiting for a lane or slot (or `pending_pieces > 0`).
- Report `refill_gap_ns / backlog_ns` and define a pass criterion for it.
- Re-measure the compat FAILs before treating them as scheduler defects.

### P7a-R6: BLOCKING (Medium): no aggregate or required-coverage check, so missing cells look like a pass

**Problem.** Gates are emitted per row. A reader sees a list of PASS and N/A rows, with no statement that the gate as defined (G-2 = B-3, B-4, B-5 **and** B-6 on each FS; G-5 = B-1 **and** B-3; G-7 = 1 h per FS) is complete.
- With `ALLOW_WRITES=0` (the default) there are no B-6 rows, so no G-2 or G-5 rows exist for writes. Nothing flags this.
- `failures.log` and non-zero exits only influence the rows they belong to.
- G-4 sets `complete=True` when the rows it *has* are paired. If repetitions 3–5 crash before writing any row, 2 pairs give PASS, and `run_b10.sh`'s failures are not read.
- The fallback `else "unknown"` step classification lets stray CSVs in the results directory join the analysis silently.

**Fix.**
- Add a `required` table to the analyzer: per (mount, device), the step × variant × direction cells each gate needs, derived from `manifest.tsv`/config or from a CLI `--expect` file.
- Emit one aggregate row per gate per mount: PASS only if every required cell is PASS. Otherwise FAIL, or N/A listing the missing cells.
- Put the manifest's failed cases at the top of `report.md`.
- G-4 must require the expected number of pairs (`--repetitions`, written by `run_b10.sh`) and read `failures.log`.
- Rows with `step == "unknown"` should be an error.

### P7a-R7: BLOCKING (High): run agent's BUG-1, spurious circuit opening and a cooldown that never decays under an operator override

**Assessment.**
- **Evidence.** The doubling pattern is clear. Engine-active epochs are 0–3, 5, 9 and 17, which matches cooldowns of 30, 60, 120, 240 and 480 s. Code inspection confirms the mechanism:
  - `mount_policy::begin_cooldown` doubles `_gauges.cooldown` up to 960 s (`mount_policy.cpp:899-901`).
  - `poll_timers` returns an override mount to `operator_qualified` **without** resetting the cooldown (`:931-935`). Only a passing probe resets it (`:617`).

  So in a long-lived process with an override, a handful of trips leaves the engine off for 16 minutes per trip, permanently. That is a product behavior, not just a compat artifact, and AM-4 overrides are the **only** production route (AM-11/AM-16).
- **Trigger.** The run agent's suspected trigger ("compat submit blocks for the whole I/O") contradicts AM-10, which measured compat read-submit p50 at 6–17 µs. Their own control is the strongest evidence: a 40 s run with no soak sampling stays 100 % on the engine. That points instead to the drained snapshot and `verify_contents` pause (blocking D2H, stream syncs) interacting with:
  - the stuck watchdog (`device_engine.cpp:2120-2131`, `stuck_ms=2`);
  - the slow-of-8 rule (100 µs floor, ×4 p99).

  For example, a CUDA-side lock held by the application's synchronous copy makes `cuFileReadAsync` exceed 2 ms. The same thing can happen on GDS hosts whenever the application does synchronous CUDA work.
- **Why it blocks the handoff.** Without R1, the hardware runs would be invalid. Even with R1, they would be mostly N/A.

**Fix.**
1. Before handoff:
   - Run the 4-minute repro with the policy gauges (R1.3) and an NVTX range or log line naming the trip reason and the elapsed submit time.
   - State in AM-form which threshold fires.
2. Decide the cooldown decay rule. For example, reset to `cooldown_s` after N healthy minutes or M healthy submits under an operator override, and test it.
3. Calibrate `stuck_ms` and the slow floor (§9.6) from the B-0 submit distributions on each FS. Until then, consider setting `stuck_ms` from the measured healthy p99 (≥ 10× p99, with a floor well above 2 ms) for override mounts.

### P7a-R8: BLOCKING for write qualification (High): run agent's BUG-2, a successfully submitted write is failed with EIO

**Assessment.**
1. The submitter watchdog marks a **write** node `unknown_progress` whenever the call has been running ≥ `stuck_ms` (`device_engine.cpp:2128-2130`).
2. When that same call then returns `recorded`, the write is failed with `errc::io_error` (`:1296-1299`). It is never tracked through its event.

The fail goes through recovery, so the slot is not released early. This is a **spurious, user-visible write failure** rather than corruption. The spill aborts in 2 of 7 4-thread runs.

"Unknown progress" is only true when the call never returns, or returns an error after a partial enqueue. A call that returned success with the event recorded has a known outcome: wait for the event and check `bytes_done`. On GDS, any submit-thread preemption or CUDA lock wait ≥ 2 ms would trigger this.

**Fix.**
- On `recorded`, ignore the watchdog's `unknown_progress` flag and proceed with event plus `bytes_done` validation.
- Reserve no-replay failure for calls that returned an error, or that are abandoned past a hard stuck limit (seconds, not 2 ms).
- Add a simulator test: write submit delayed 5 ms, then success. Expect success and the bytes written.
- Until this is fixed, the hardware handoff must run with `ALLOW_WRITES=0` for engine writes, and B-6 and G-2 writes stay N/A.

### P7a-R9: SHOULD (Medium): run agent's BUG-3, spill stage-event polling is not latency-scaled

**Assessment.**
- In the 177.5 queries/op run, 145.4 were stage queries and completion p50 was 67 ms. Read paths, which use the AM-15 I/O and copy latency scaling, stay at 14–47.
- The stage-copy events in the staged-write path evidently still poll at a near-fixed cadence while the copy is gated behind the user's stream.

This is a real G-5a FAIL candidate on any slow write target (GPFS/NFS). It does not block the read handoff.

**Fix.**
- Apply the AM-15 latency-adaptive ceiling to stage events. Use a hint deadline derived from the observed stage-copy latency, as was done for the fence hint.
- Add a simulator regression with 50–100 ms stage latency that asserts ≤ 70 queries/op.

### P7a-R10: SHOULD (Medium): B-10 covers only the legacy arm of NFR-2

**Problem.** §8.2 B-10 is "old against new, `mode=legacy` **and** `async_auto`, on T4 **and** a non-GDS host". `legacy_compat_bench.cpp` is intentionally restricted to the old API ("Do not enable async_engine here"), so:
- the `async_auto` arm, where a compat host must stay `known_compat` → legacy with no overhead, is unmeasured;
- T4 was not run;
- the run used 2 GiB/512 MiB instead of the documented 4 GiB/1 GiB.

All three workloads are single-thread, single-object and single-stream. AM-16 explicitly assigns the new inode write-sequencing mutex/guard overhead to NFR-2 qualification. That overhead only shows up with **concurrent writers** to the same or different inodes, which B-10 never exercises.

The G-4 PASS is therefore valid as "**G-4 (legacy arm, dev host): PASS**", not as G-4.

**Methodology I checked, and found sound:**
- the same file per workload, shared by old and new;
- O_EXCL creation by the first run;
- reversed order on even repetitions;
- the same injected source, release build type, `CUFILE_FORCE_COMPAT_MODE` and `--drop-cache` on both sides;
- a full untimed content check before and after timing.

The deltas (−0.1 to −0.7 %, σ ≈ 0.3–2 %) are far inside 5 %.

**Fix.**
- Add an `async_auto` arm. This needs a new-only flag: compare new/`async_auto` against old/default under the same alternation.
- Add a 4–16 thread variant (shared and distinct inodes, mixed read/write).
- Run on T4.
- Label the current gate row.

### P7a-R11: SHOULD (Low): B-10 statistics and provenance

**Problem.**
- G-4 uses the mean paired ratio with no confidence bound. NFR-2 says "within measurement confidence".
- No minimum pair count (R6).
- `b10-builds.txt` records `git rev-parse HEAD` only. The "new" binary is HEAD **plus the working tree**, and no dirty-state hash is recorded.
- Neither tree's `CMakeCache` flags are recorded.
- `run_b10.sh` re-runs `cmake --preset release` on the developer's `build/release`, which mutates it as a side effect.
- The worktree `/tmp/cucs-b10-old` is left registered.

**Fix.**
- Report a 95 % CI (t-interval on the paired ratios) and require the lower bound ≥ 0.95, or at least report it.
- Record `git status --porcelain` and `git diff | sha256sum`, and refuse to run if `src/` or `include/` are dirty.
- Record `CMAKE_BUILD_TYPE`, `CMAKE_CXX_FLAGS*` and `CUDAARCHS` for both builds.
- Use a dedicated `build/b10` directory.
- Print a `git worktree remove` hint.

### P7a-R12: SHOULD (Low): cache eviction is advisory and unverified, and spill timing excludes durability

**Problem.**
- `fsync + POSIX_FADV_DONTNEED` is best-effort, and the run agent notes that 6.4 GiB/s on LVM suggests cache hits. B-10 stays symmetric, so G-4 holds. For B-3..B-6 compat numbers and any G-2 cell on a buffered or fallback path, the source of the data is unknown.
- In both benches the per-pass `fsync` runs *before* the timer, and the last pass is never synced. Spill throughput therefore measures page-cache absorption, not storage write throughput. B-6's `--flush` defaults to 0 in `qualify.sh`.

**Fix.**
- After eviction, sample residency with `mincore` over an `mmap` of the file, record `resident_pages` in the CSV, and warn above 1 %.
- For G-2 writes, run B-6 with `--flush 1`, or include the final `fdatasync` in the timed region, and record which one was used.

### P7a-R13: SHOULD (Medium): G-7 coverage gaps

**Problem.**
1. The `qualify.sh` G-7 cell soaks only `sustained` reads. Write-path resources are never exercised: pinned write-bank, inode registry, write intervals and tickets. `pinned_host_bytes` stays 0 throughout, so that NFR-6 dimension is vacuous.
2. NFR-6 says "engine device memory never exceeds the reserved arena". There is no device-memory sample, such as `cudaMemGetInfo` free bytes or NVML per-process used memory. There is also no `/proc/self/fd` count, and cuFile handle or fd leaks across recreation are a classic failure.
3. `data_mismatches` is a hard-coded `0` in both the soak sample and the main row (`cufile_engine_bench.cpp:1000` and the soak writer). A standalone soak (the documented `results/soak` flow) has no exit status and no completion marker, so a crash or verify failure is detected only through a short duration.
4. VmRSS grows 522 → 613 MiB in epoch 0–1 and is then flat. Fine, but the analyzer does not show the trend.

**Fix.**
- Add a spill soak (`ALLOW_WRITES=1`) and a restore soak.
- Sample device free bytes, the fd count and the pinned-pool bytes, and gate on device used-bytes stability.
- Write a final `complete` row (or an `.ok` file) on successful exit, and make the analyzer require it.
- Drop the constant `data_mismatches` column, or populate it.

### P7a-R14: SHOULD (Medium): `qualify.sh` safety and robustness on a production GDS host

**What I verified:**
- Writes are confined to `$scratch/$STAMP/` and `RESULTS_ROOT/<host>-<ts>-<pid>/`.
- `TMPDIR` and the cuFile log point inside the results directory.
- The topology write probe is `mktemp` + `dd oflag=direct` + `rm` in the scratch root.
- Scratch `/` is rejected.
- There are no `rm`, sysctl, `drop_caches`, module, `nvidia-smi` reset or privileged commands in `qualify.sh`.
- gdsio writes only to its own `gdsio-d$dev.bin`, and the P0 pattern file is only ever read by gdsio.
- Every engine input uses O_EXCL `--create-file`.
- Failures do not abort the matrix.

**Gaps:**
1. **No per-case timeout.** A paused NFS/GPFS server, a quarantined lane (incomplete drain), or a hung gdsio stalls the entire unattended matrix forever. Wrap each command in `timeout --kill-after=60 <limit>`, record rc 124/137, and continue.
2. **No disk-space preflight, and everything is retained.**
   - B-4 and B-6 create `threads × tasks` files per case. With `ENGINE_THREADS` up to 16, that is 64 × 2 GiB per spill case.
   - Each B-3 case creates its own `FILE_MIB` file.
   - None are deleted, on what may be a shared production filesystem.

   Estimate the footprint from the config, compare it with `df`, and add `--cleanup-success` to delete each case's inputs after a zero exit (and keep failures).
3. **No check that scratch is on the declared mount.** A typo puts the data on a different FS. The `mount_prefix:$mountpoint` override then doesn't match and the engine silently stays legacy (R1). Require `findmnt -n -o TARGET -T "$scratch" == "$mountpoint"` and an `fs_class` consistent with `findmnt -o FSTYPE`.
4. **The exit status is always 0.** It should exit non-zero when `failures.log` is non-empty, for CI and automation.
5. **Root is undocumented.** State that root is not required and not recommended. Files are created 0600, and the config is sourced as Bash. State that `/dev/nvidia-fs*` access (group) is required for direct I/O, and that non-root `gdscheck` and nvidia-fs stats are readable.
6. `metadata.json` dumps the whole environment, which may contain secrets. Use an allowlist (`CUFILE_*`, `CUDA_*`, `LD_LIBRARY_PATH`, `PATH`, pixi variables) by default.
7. B-8 starts background jobs with no `trap` handling, so an interrupted run leaves orphaned processes.

### P7a-R15: SHOULD (Medium): matrix completeness against §8.2 and §9

These are missing or only partial. None are needed to *start* Phase 7b, but the handoff plan must list them, because G-2..G-8 cannot be signed off without them:

| Item | Status |
|---|---|
| **B-7** (mixed priority + concurrent GPU kernels; §9.16 D2D contention) | absent |
| **B-1** | only the CUDA micro-costs. Missing: validate-then-copy vs pre-enqueued copy, timer overshoot, QD loss at 10 k ops/s and at saturation (R3) |
| **B-8** | cross-process only. In-process multi-mount fairness (the scheduler's DRR) is absent |
| **B-9** | unregistered staging is absent, so §9.5 is unanswered |
| **Engine QD / lanes sweep** | `ENGINE_QD` is fixed at 32 with no engine QD sweep, so §9.13 (160 lanes) and §9.20 (QD targets ≠ 64) are not covered |
| **AM-5** | manual. It needs a second run with a small arena. `OP_LIST` lacks 16 MiB, a common GPFS block size; derive it from the topology `f_bsize` / `mmlsfs -B` |
| **AM-9** | B-0 arena vs slot is there, but BAR1 used before and after registration and the registration cost per case are not captured (`nvidia-smi -q -d MEMORY` per case) |
| **§9.2** `bytes_done` under fault injection, **§9.7** `cuFileStreamRegister` cost, **§9.14** hand-off latency, **§9.19** NIC/RDMA counters during runs (`mmdiag --rdma`, `ethtool -S`) | absent |
| **T-40** (100 GiB mixed read/write integrity, path-loss and server-pause fault drills) | absent; G-8 depends on it |
| **gdsio access pattern** | `-I 0/1` is sequential, while B-3/B-4 are scattered. Add `-I 2/3` (random) baselines and match by pattern |
| **Run length** | `SECONDS_PER_CASE=3`, one repetition per cell. Network FS warm-up and prefetch make this too short for an 80 % gate. Use ≥ 30 s and ≥ 3 repetitions for gate cells, and report a CI |

### P7a-R16: SHOULD (Low): smaller analyzer PASS-integrity items

- No PASS requires `verify == 1`, although the bench supports `--verify 0`. Require it for any gate input.
- The AM-9 row reports PASS for `arena` registration without direct evidence. AM-9 requires `verified_direct` plus content validation before a shared granularity is qualified. Use N/A unless the row is direct-qualified.
- The G-7 byte tolerance is `max(1, min) × 1 %`, about 2.5 MiB on a 256 MiB arena. Arena and registered bytes should be **exactly** constant (NFR-6). Use 0 tolerance for those, and keep the tolerance for host pools only.

### P7a-R17: SHOULD (Medium): sanitizer conclusions are valid within stated limits, but not G-6 evidence

**Assessment.**
- **ASan + UBSan.** The clean run (928 cases, 3.04 M assertions) used `alloc_dealloc_mismatch=0` globally, `--order decl`, and excluded the BUG-6 test.
  - S-1 (stack entirely in libcufile/libcuda) is correctly classified as external.
  - S-2 is attributed to a toolchain mix: GCC 14.3 compiler and libasan, libstdc++ 6.0.36 from GCC 15, a sized `operator delete` that reaches `free`. That is plausible but not proven, and the global switch also disables detection of real new/free mismatches in cuCascade code.
  - S-3 is right: `interceptor_via_lib` does not apply to the alloc-dealloc check. `QUALIFICATION.md`'s suggestion to use `sanitizer.supp` for it is therefore wrong.
- **TSan.** No coverage. `QUALIFICATION.md` forbids "disabling ASLR", but `setarch "$(uname -m)" -R ./cucascade_io_tests` disables it **for that process only**. It needs no root, hides no races, and is the standard workaround for "unexpected memory mapping" with high `mmap_rnd_bits`. Newer LLVM TSan runtimes re-exec this way automatically.

**Fix.**
1. Re-run ASan with a matching runtime. Point `LD_LIBRARY_PATH` at GCC 14's libstdc++, or build with a matching GCC 15 or with clang. Then re-enable `alloc_dealloc_mismatch`.
2. Correct `QUALIFICATION.md` §ASan (S-3) and add `alloc_dealloc_mismatch` guidance.
3. Permit `setarch -R` for TSan, and document `-Wno-tsan`. TSan does not model `atomic_thread_fence`, so fence-ordered reports need manual review.
4. Run T-29 and T-32 under TSan as §8.2 G-6 requires.

### P7a-R18: SHOULD (Low–Medium): test-suite hygiene needed before G-6 (BUG-5, BUG-6, OBS-1)

| Bug | Severity | Problem | Fix |
|---|---|---|---|
| BUG-5 | Low | The `asan` preset overwrites `CMAKE_CXX_FLAGS` and the linker flags, dropping pixi's `$CXXFLAGS`/`$LDFLAGS`. The documented build fails on nvtx3 includes and then on the `librmm` rpath | Use `"$env{CXXFLAGS} -fsanitize=…"` and `"$env{LDFLAGS} …"` in the preset (or `*_INIT` variables), and add a TSan preset with `-Wno-tsan` |
| BUG-6 | Low | `test_engine_ioctx.cpp:767` passes `host_buffer{nullptr}`, which violates the `types.hpp:317` precondition. It aborts deterministically in Debug, and Release hides it | Use a dummy non-null buffer. G-6 ("every test passes") is impossible until this is fixed |
| OBS-1 | Low–Medium | Catch2 `OutputRedirect` asserts in random order only, after a test that printed SKIP or WARN. The suite is not reliably green, which is a G-6 prerequisite | Bisect the minimal ordered pair with `--order rand --rng-seed <failing seed>`. Check for SKIP/WARN emitted from non-main threads, or after a test's threads outlive it. Check the Catch2 3.8.1 changelog for redirect fixes |

### P7a-R19: NIT: `SUMMARY.md` and housekeeping

- Gate table:
  - "G-4 PASS" should read "G-4 legacy arm PASS (dev host)" (R10).
  - "G-5 PASS" should read "G-5a" (R3).
  - The G-7 simulated PASS was with `--soak-min-s 1200`. It is a tooling self-test only (the simulator plus reads only), not a G-7 result.
- BUG-1's suspected cause should note the AM-10 contradiction and the 40 s control (R7).
- `run-step3.sh` adds `--override fs_type:ext4` and `--require-direct-evidence 0` for every variant, including legacy. Harmless, but it differs from `qualify.sh`; note it.
- Leftovers to clean up or document for the next agent: the `/tmp/cucs-b10-old` worktree is still registered (`git worktree remove`), `build/asan` is still present, and so is `/tmp/qual`.

---

## 3. Disposition of the run agent's bugs

| Bug | My severity | Blocks hardware handoff? | Review ID |
|---|---|---|---|
| BUG-1: circuit opens, cooldown persists and doubles, analyzer passes anyway | **High** | **Yes**: root-cause and instrument it, and fix the analyzer (R1) | R1, R7 |
| BUG-2: EIO on a successfully submitted async write | **High** | **Yes for writes**: fix, or hand off with `ALLOW_WRITES=0` | R8 |
| BUG-3: spill stage polling up to 177 queries/op | Medium | No. It blocks G-5a sign-off | R9 |
| BUG-4: G-3 PASS/FAIL on compat | Medium (tooling) | **Yes** (analyzer semantics) | R4 (+R5) |
| BUG-5: `asan` preset drops env flags; ASan/TSan doc gaps | Low | No (G-6 prerequisite) | R17, R18 |
| BUG-6: `host_buffer{nullptr}` in a test | Low | No (G-6 prerequisite) | R18 |
| OBS-1: Catch2 random-order abort | Low–Medium | No (G-6 prerequisite) | R18 |

## 4. Answers to the review questions

- **Is B-10 fair?** Yes, for the legacy arm: same files, alternating order, cache eviction on both sides, identical injected source and build type, and forced compat on both. Caveats: the eviction is advisory (R12), the provenance is incomplete (R11), and the `async_auto` arm, T4 and the concurrency variants are missing (R10).
- **Does the analyzer compute G-1..G-7 per §8.2 and the amendments?**
  - G-1 and G-4: yes, modulo R6 (pair count) and R11 (CI).
  - G-2: no (legacy rows, no routing check).
  - G-3: partially (no direct-evidence check, tail-polluted backlog, refill gaps ignored).
  - G-5: no (one sub-criterion).
  - G-7: partially (cumulative progress, reads only, no device-memory or fd sampling).
- **Can it report PASS wrongly?** Yes, through:
  - legacy-routed engine rows (R1);
  - G-2 legacy rows (R2);
  - G-5 (R3);
  - G-3 on compat (R4);
  - missing required cells (R6);
  - too-short G-4 series (R6);
  - an unverified `--verify 0` row (R16).

  I found no unit errors. The gdsio GB→GiB and MiB factors and the S_eff byte→MiB conversion are correct, and baseline matching on mount, device, direction, Q_eff and S_eff is exact.
- **Is `qualify.sh` safe on a production host?** It is non-destructive and its writes are confined. It lacks timeouts, a space preflight, cleanup, a scratch-on-mount check, a non-zero exit status and root guidance (R14).
- **Are the soak and sanitizer conclusions correct?**
  - The simulated soak passes as a tooling self-test.
  - The async soak is correctly called vacuous.
  - ASan + UBSan are clean only under the stated suppressions and ordering.
  - TSan has no coverage, and it is fixable without root (R17).

---

## 5. Verdict

**CHANGES REQUIRED: not ready for the Phase 7b hardware handoff.**

**BLOCKING:**
- **P7a-R1:** routing check in every gate, and G-7 per-epoch progress
- **P7a-R2:** G-2 only on engine rows
- **P7a-R3:** G-5 must not PASS on queries/op alone
- **P7a-R4:** G-3 requires direct evidence
- **P7a-R6:** required-coverage aggregation and pair counts
- **P7a-R7:** root-cause BUG-1 and decide the cooldown decay
- **P7a-R8:** BUG-2 spurious write EIO (or hand off with `ALLOW_WRITES=0`)

**SHOULD:** P7a-R5, R9, R10, R11, R12, R13, R14, R15, R16, R17, R18.

**NIT:** P7a-R19.

The plumbing (`qualify.sh`, `run_b10.sh`, manifest and evidence attestation) is a sound base, and the G-4 legacy-arm result stands.

## Resolution

- **P7a-R1 — fixed:** engine/legacy/sync routing checks, per-epoch soak evidence/progress, policy gauges/trigger logs, and explicit override refusal; regression tests cover demotion, pure legacy and idle epochs.
- **P7a-R2 — fixed:** only async engine rows receive G-2; legacy is a performance reference and simulated rows are excluded.
- **P7a-R3 — fixed (claims); deferred to Phase 7 hardware (full budgets):** query ceiling is G-5a, B-1 is evaluated, exact mean residence recorded; full G-5 remains N/A pending role CPU/10k-ops/s/saturation/contention measurements.
- **P7a-R4 — fixed:** compat/simulator G-3 is INFO; acceptance requires direct qualification and engine routing, with regression coverage.
- **P7a-R5 — fixed:** eligible-unsubmitted backlog excludes drain tails/idle peers, refill-gap fraction is checked (<=10%); held-tail regression and all 14 affected registered cells remeasured. Compat underfill remains diagnostic, not declared solved.
- **P7a-R6 — fixed:** configured/explicit required-cell tables and per-mount aggregates, failed commands first, unknown CSV rejection, expected B-10 pairs/failure log and confidence checks. Missing writes cannot vanish from coverage.
- **P7a-R7 — fixed:** four-minute instrumented repro attributes five trips to slow-of-eight (287–777 µs), not stuck/whole-I/O blocking or a necessary sampling pause; healthy-window cooldown decay tested. Explicit compat-only 2-ms/20-ms calibration runs four minutes with zero demotions/fallback. AM-18 records evidence; production defaults remain conservative pending per-FS hardware calibration.
- **P7a-R8 — fixed:** returned submit/event success clears watchdog uncertainty and follows normal bytes_done validation, never replay. Both delayed-5-ms and trip-then-release disk-content regressions pass; new B-6 writes have no EIO.
- **P7a-R9 — fixed:** stage/fence polling uses elapsed/history-adaptive backoff; 100-ms singleton-fence regression <=70 queries/op; new spills are 20.67/15.46 total and 12.50/12.08 stage queries/op.
- **P7a-R10 — fixed (tooling); deferred to Phase 7 hardware (acceptance):** B-10 adds async_auto and 1/8 callers on shared/distinct inodes; eight new-only smoke cells pass. Full-size paired T4/non-GDS and mixed-direction acceptance remain explicit N/A dependencies; no T4 is available here.
- **P7a-R11 — fixed:** paired Student-t 95% lower bound, expected pair count, dirty-tree refusal by default (explicit recorded development override), source/status/diff/binary/build-flag provenance, dedicated build/b10 and cleanup hint.
- **P7a-R12 — fixed:** mmap/mincore residency observations and >1% warning, B-6 timed flush=1; new affected rows observe zero resident pages. Historical B-10 remains a symmetric host-complete API comparison, not durable storage throughput.
- **P7a-R13 — fixed (tooling); deferred to Phase 7 hardware (one-hour matrix):** restore and spill soaks, fd/CUDA-used/pinned-pool samples, final success marker, no fabricated mismatch-zero column. Short read/restore/spill diagnostics validate new resources; no full G-7 PASS.
- **P7a-R14 — fixed:** per-case timeout/kill grace, conservative free-space/mount/type checks, success cleanup, failed exit propagation, child interruption handling, allowlisted environment and non-root/group-access guidance; runner safety regressions pass.
- **P7a-R15 — deferred to Phase 7 hardware:** QUALIFICATION.md explicitly lists every remaining B-1/B-7/B-8/unregistered/QD-lane/AM-5/AM-9/T-40/NIC/RDMA/replicated-CI measurement dependency. Random gdsio patterns, 16-MiB ops and >=30-s/three-pass defaults are implemented; three passes are explicitly not independent-run confidence evidence.
- **P7a-R16 — fixed:** verified inputs required, arena AM-9 requires direct proof, exact arena/registration budgets; simulator soak can only be INFO/self-test. Regressions prevent false PASS.
- **P7a-R17 — fixed (run/tooling); deferred to Phase 7 hardware (clean G-6):** allocator issue root-caused to incomplete test global allocation hooks, not assumed vendor/toolchain fault; full randomized ASan/UBSan passes with checking enabled. Non-root setarch TSan executes T-29/T-32 fault tests (17 cases/516 assertions) but eight CUDA-event happens-before reports remain retained/unqualified, without race suppressions.
- **P7a-R18 — fixed:** environment-preserving ASan and TSan presets, non-null routing fixture, complete allocation-hook family regression and main-thread Catch assertion discipline; full Debug/ASan and ten randomized engine loops pass.
- **P7a-R19 — fixed:** SUMMARY gate claims relabeled, original results preserved as historical evidence, source/override differences noted; clean detached /tmp/cucs-b10-old removed, sanitizer builds/raw scratch explicitly retained.

Validation/evidence and the updated gate table are in
`qualification/compat-dev-host/SUMMARY.md`. No commit, no src/cudf edits, no
concurrent builds or independent benchmark processes. One new pre-measurement
warm-up timeout remains unexplained (manifest rc=124); immediate retry/resume
succeeded, and this failed attempt is not hidden or declared fixed.
