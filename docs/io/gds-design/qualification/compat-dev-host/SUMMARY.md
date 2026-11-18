# Phase 7a — compat-host qualification after review fixes

**Not GDS qualification. No full G-1..G-8 gate is signed off.** This host has no
nvidia-fs, RDMA, gdsio or T4. Everything ran sequentially, with no commit and no
`src/cudf/` edits. Default product routing remains legacy.

## 1. Environment and evidence

- Branch `io/cufile-async-engine`, HEAD `6f6b82e` plus uncommitted tooling/fixes.
- Linux 7.0.0-43, 24 CPUs, 125 GiB RAM, two RTX 6000 Ada GPUs, driver 595.99.02,
  CUDA 13.2, libcufile 1.17 compat, GCC 14.3, Catch2 3.8.1.
- `/mnt/disk_2` is absent. `/tmp/qual` is ext4/LVM on `/`; these are not raw NVMe
  or GDS performance claims. Scratch, ASan/TSan builds and raw logs are retained.
- `fixes/SUMMARY-before-fixes.md` preserves the original run summary verbatim
  below a supersession notice. Its gate claims and S-1/S-2 diagnoses are historical.
- Original evidence: `b10/`, `step3/`, `smoke/`, `soak/`, `sanitizers/` and
  `run-step3.sh`. Fix evidence: `fixes/{tests,sanitizers,repro,calibrated,remeasure,b10-smoke}/`.
- `/tmp/cucs-b10-old` was verified clean and removed. Historical old CMake flags
  are retained in `fixes/b10-smoke/historical-old-cmake-flags.txt`.

## 2. Updated gate table

| Gate | Current status | Basis |
|---|---|---|
| G-1 | **N/A** | No matched gdsio/direct evidence. |
| G-2 | **FAIL / incomplete evidence** | The warm-up timeout remains a failed case. All successful compat rows are N/A without baselines/direct proof; no GDS acceptance. Async-only, >=30 s/three passes and timed write flush required. |
| G-3 | **INFO (compat)** | All 14 affected registered cells remeasured with corrected backlog/refill accounting. Mean/Q_eff 0.648–0.986; some underfill remains, not a GDS scheduler verdict. |
| G-4 | **N/A (full gate)** | Historical legacy arm/dev-host comparison meets 0.95; paired 95% CI lower 0.9864/0.9910/0.9733. Old CSV lacks new explicit verify/provenance fields; not current automatic acceptance. Full-size/T4/two-arm matrix remains pending. |
| G-5a (AM-15 only) | **Successful new rows meet ceiling** | Reads 6.46–47.02 queries/op; timed-flush spills 20.67/15.46, stage 12.50/12.08. One pre-measurement timeout remains a failed attempt; retry does not hide it. |
| G-5 (NFR-3/NFR-4) | **N/A** | Query ceiling alone is not the CPU/residence/defined-operating-point budget. Exact mean residence now recorded; remaining measurements are explicit dependencies. |
| G-6 | **N/A** | Unsuppressed randomized ASan+UBSan passes: 934 cases, 933 passed/1 skipped, 3,031,983 assertions. TSan executes 17 fault tests/516 assertions, but eight CUDA-ordering reports prevent a clean T-29/T-32 claim. |
| G-7 | **N/A (short diagnostics)** | Calibrated four-minute reads: four epochs, 100% engine, zero demotions/sync/legacy bytes, stable fd/device/arena budgets. Short restore/spill diagnostics stable. Original-threshold repro **FAILS routing/progress**. Simulated soak is only a tooling self-test. |
| G-8 | **N/A** | External hardware/integrity approval; no default flip. |

Required-coverage aggregation now lists absent cells, including disabled writes.
Compat/simulator G-3 never produces PASS/FAIL. G-5a is explicitly a subcriterion,
not G-5 approval. Old resource-stable but engine-off soaks cannot pass.

## 3. Product fixes and regressions

- **BUG-1 / R7:** standing-override cooldown returns to its initial value after
  1024 healthy submits since the previous trip; otherwise escalation remains.
  Fake-clock regression covers both. Gauges/logs identify trigger and elapsed
  submit time. See the calibrated repro below; product thresholds are unchanged.
- **BUG-2 / R8:** a watchdog-observed write that later returns success with a
  recorded event follows event/bytes_done validation, without replay or spurious
  EIO. Regressions cover 5-ms submit success and release after an actual trip,
  checking exact disk contents and single submission.
- **BUG-3 / R9:** stage polling shares caller-fence hints and latency-adaptive
  elapsed-time backoff, including cold starts. A singleton 100-ms caller fence
  must remain <=70 queries/op.
- **R5:** backlog means unsubmitted eligible work, not drained tails. Idle mounts
  cannot manufacture refill gaps. Held-completion regression proves tails do not
  increase backlog time. Corrected compat ratios still show some underfill.
- **BUG-6:** routing-only test uses a dummy non-null host pointer, passing Debug.
- **OBS-1:** instrumentation found off-thread Catch assertions hidden in the URL
  query helper, CUDA allocation/stream helpers and a transfer hook. Results now
  return/throw to the test thread; a listener guards against future off-thread
  assertions. Randomized full-suite and engine loops no longer corrupt redirects.
- **BUG-5 / R17:** sanitizer presets preserve pixi flags. The presumed toolchain
  and vendor allocator mismatches were actually the test allocation counter:
  ASan independently interposed nothrow new, then custom sized delete called free.
  Complete unaligned allocation/deallocation overloads fix both reports. A
  regression covers scalar/array/nothrow/fault injection. No alloc-dealloc switch
  or suppression is used. Custom scalar allocations retain malloc/free tags,
  so standard C++ family tagging is an explicit counter limitation.

Normative behavior and attribution are recorded in **AM-18**.

## 4. Validation

```bash
pixi run cmake --build build/release --target cucascade_io_tests cucascade_cufile_async_qd \
  cucascade_cufile_engine_bench cucascade_legacy_compat_bench -- -j 20
./build/release/test/cucascade_io_tests
# Final rebuild/run: 934 cases, 933 passed, 1 skipped; 3,046,231 assertions; zero failures
# [engine] --order rand --rng-seed 7101..7110:
# 10/10 passes, each 132 cases / 6,873 assertions (1,320 case executions)
pixi run python -m unittest discover tools/gds/tests
```

Test logs are in `fixes/tests/`. **34 analyzer/runner unit tests pass.** Coverage includes
routing/demotion, missing cells, idle epochs, exact byte budgets, verification,
direct-evidence restrictions, confidence/pair counts, wrong mounts, failed-case
exit propagation, dry-run non-mutation and sanitizer environment preservation.

ASan/UBSan full randomized seed 4153253458 passes with
`ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`: no test exclusions, no
`alloc_dealloc_mismatch=0`, no suppressions. Matching-runtime attempts did not
resolve the problem; fixing the test allocation hooks did. Raw final output is
`fixes/sanitizers/p7a-asan-final.log`.

TSan now runs without root via `setarch -R`, with `-Wno-tsan` for unsupported
atomic_thread_fence modeling. All eight reports pair simulator prepare/finish
callbacks with post-CUDA-event validation, argument reuse or recovery. CUDA
happens-before is invisible to TSan. Reports are retained in
`fixes/sanitizers/p7a-tsan-final-fault.log`, not suppressed or called a clean gate.
A sanitizer-aware CUDA event model remains needed before clean G-6 sign-off.

## 5. Four-minute circuit attribution and calibration

`fixes/repro/` uses the original 100-µs slow floor / 2-ms stuck threshold and
records five **slow-of-eight**, not stuck, trips. Triggering submits were
287–777 µs, healthy p99 48–81 µs. The first trip preceded the first five-second
sample. This disproves both whole-I/O-blocking and sampling-pause-only claims;
AM-10's fast-submit observation remains consistent. Healthy-only baseline
sampling excludes tails, making the default rule sensitive to compat contention.

`fixes/calibrated/` repeats the same four-minute workload with explicitly recorded
**`--slow-floor-us 2000 --stuck-ms 20`**. Four epochs moved 100% engine bytes,
zero legacy/sync bytes, zero demotions. After 60 s: 39 threads, 75 fds,
992,935,936 device-used bytes, exact 256-MiB arena/registration, no live ops,
quarantines or late copies. VmRSS rises about 518→617 MiB during initial pool
warm-up then stabilizes. This is not a one-hour soak or a GDS tuning recommendation.

Product thresholds remain unchanged. Calibrate real-GDS thresholds from B-0 for
each filesystem/client rather than globally weakening watchdogs. Example repro:

```bash
pixi run ./build/release/cucascade_cufile_engine_bench \
  --file /tmp/qual/UNIQUE.bin --create-file --engine async --require-direct-evidence 0 \
  --override mount_prefix:/ --override fs_type:ext4 --slow-floor-us 2000 --stuck-ms 20 \
  --size-mib 256 --task-mib 64 --single-mib 64 --threads 1 --tasks 4 \
  --qd 32 --op-mib 4 --arena-mib 256 --drop-cache --verify 1 \
  --soak-minutes 4 --recreate-every-s 60 --sample-every-s 5 --csv /tmp/qual/UNIQUE.csv
```

## 6. Only invalidated measurements rerun

`run-fixes.sh` records the sequential commands. `fixes/remeasure/` has 14 successful
registered B-3/B-4/B-5/B-6 cells (threads 1/4; B-3 op 1/2/4/8 MiB). Successful rows
verify all content, report zero legacy/sync bytes, zero demotions and zero resident
pages after eviction. Legacy/POSIX/B-1 and the unaffected historical B-10 baselines
were not rerun.

| Workload | New queries/op | Notes |
|---|---|---|
| B-3 | 6.46–28.19 | Tail-corrected backlog; some compat underfill remains. |
| B-4 | 32.53 / 33.80 | 1/4 threads, zero demotion. |
| B-5 | 42.81 / 47.02 | 1/4 threads. |
| B-6 | 20.67 / 15.46 | Stage 12.50 / 12.08; **timed flush=1**, 3.17 / 4.96 GiB/s, no EIO. Not comparable to old page-cache/host-complete timing. |

**One failed attempt remains:** B-3 1-thread/4-MiB timed out before measurement,
after arena warm-up, rc=124. Immediate read-only-input retry and resumed cell
succeeded. No stack was captured, so this pre-measurement hang is **unexplained**,
not declared fixed. Resume overwrote its two-line stdout; the failed manifest
entry remains. The script now preserves previous attempt logs on resume.

Short restore/spill recreation diagnostics (31.2/32.6 s) exercise new gauges and
successful-exit markers: exact arena/registration, 40 threads, 96 fds, stable device
used bytes, spill pinned pool 163,840 bytes without growth after the first epoch.
They are not one-hour G-7 evidence.

Eight new-only B-10 smoke cells exercise legacy/async_auto × shared/distinct
inodes × sustained/spill with eight callers, 256/64 MiB, verification/cache
observations. All succeed (`fixes/b10-smoke/`). No paired baseline/CI is claimed.
The revised runner records expected pairs, a paired Student-t 95% CI, dirty-state
hash/source/build flags and uses dedicated build/b10 directories.

Historical five-pair legacy means and 95% CIs (not full G-4):

| Workload | Mean new/old | 95% CI |
|---|---|---|
| sustained | 0.99618 | 0.98644–1.00592 |
| single | 0.99906 | 0.99101–1.00710 |
| spill | 0.99301 | 0.97331–1.01270 |

Original run-step3.sh applied fs_type and mount-prefix overrides even to legacy
rows; qualify.sh instead uses the validated mountpoint override. Neither provides
direct evidence. Full-size paired T4/non-GDS, mixed-direction writers, real-GDS
matrices, longer replicated cells and clean sanitizer modeling remain explicit
hardware handoff dependencies in `tools/gds/QUALIFICATION.md`; none is inferred
from these compat results.
