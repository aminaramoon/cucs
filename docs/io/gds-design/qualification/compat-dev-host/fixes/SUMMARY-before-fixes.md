# Historical snapshot — superseded by ../SUMMARY.md

Gate claims and diagnoses below are the original run agent's, NOT current acceptance.
In particular G-5/G-7 and the S-1/S-2 attributions were corrected during the fix pass.

# Phase 7a — non-GDS (compat-only) qualification run

**Not GDS qualification.** This host has no nvidia-fs and no RDMA, so every
async-engine number below comes from libcufile *compat mode* with test-only admission
(`--require-direct-evidence 0`, `--override mount_prefix:/ --override fs_type:ext4`) or from the
simulator. None of it may be attested as direct-path evidence. Gates that need GDS stay N/A.

## 1. Environment

| item | value |
|---|---|
| repo / branch | `/home/aaramoon/workspace/oss/cucs`, `io/cufile-async-engine`, HEAD `6f6b82e` + uncommitted phase-7a tooling |
| old tree (B-10) | `7641cf5` in the detached worktree `/tmp/cucs-b10-old` |
| host | Linux 7.0.0-43-generic, 24 CPUs, 125 GiB RAM, 2× RTX 6000 Ada (49 GiB), driver 595.99.02, CUDA 13.2 |
| storage | `/tmp` is on `/` = ext4 on LVM (`/dev/mapper/ubuntu--vg-ubuntu--lv`), ~980 GiB free; **`/mnt/disk_2` does not exist** |
| GDS | none: `/proc/driver/nvidia-fs` absent, libcufile 1.17 in compat mode ("running in compatible mode"), no gdsio/gdscheck |
| toolchain | pixi env, GCC 14.3 (conda), libstdc++ 6.0.36, nvcc 13.2, Catch2 3.8.1 |
| scratch | `/tmp/qual/*` (files ≤ 4 GiB; raw logs stay there, summaries and CSVs are in this directory) |

Everything ran sequentially, one heavy job at a time.

## 2. Gate status

| Gate | Status | Basis |
|---|---|---|
| **G-4** (B-10 compat regression, ≥ 0.95 new/old) | **PASS** on all three workloads | paired mean new/old 0.996 / 0.999 / 0.993 (Δ −0.39 % / −0.10 % / −0.72 %; limit −5 %) |
| **G-5** (queries/op ≤ 70, AM-15) | **PASS for B-3/B-4/B-5/B-6 smoke and for step-3 reads (14–47)**. **FAIL for 1 of 5 B-6 spill runs (177.5; other runs 28.7–55.7)** | see BUG-3. The residence and CPU budgets still need review |
| **G-6** (sanitizers) | **N/A by tooling** (external approval needed). Evidence: ASan+UBSan clean on the full io suite in decl order (3 036 823 assertions, no UBSan reports). TSan could not run | see §6. The ASan result has caveats: BUG-5, BUG-6, ADM suppression, and the Catch abort |
| **G-7** (soak) | **simulated: PASS** with `--soak-min-s 1200`. **async/compat: analyzer says PASS, but it is vacuous / INCONCLUSIVE** | the engine was routed legacy for 13 of 20 epochs (see BUG-1). With the default 1 h threshold both are **N/A** (20 min soaks) |
| G-1, G-2 | N/A | need gdsio baselines and reviewed direct-path evidence (smoke: gdsio exit 127) |
| G-3 | analyzer emits PASS (smoke 5×; step-3 1×) and FAIL (step-3 12×) on compat data; **should be N/A here** | see BUG-4 |
| G-8 | N/A | external approval |

## 3. Commands (all from the repo root)

```bash
# 1. B-10 (default 4096/1024 MiB replaced by 2048/512; /mnt/disk_2 absent -> /tmp)
mkdir -p /tmp/qual/b10-scratch
pixi run bash tools/gds/run_b10.sh --scratch /tmp/qual/b10-scratch --results /tmp/qual/b10 \
  --repetitions 5 --size-mib 2048 --task-mib 512 --drop-cache

# 2. G-7 soaks, 20 min each, recreate 60 s, sample 5 s; mount_prefix:/ because /tmp is on /
pixi run ./build/release/cucascade_cufile_engine_bench --file /tmp/qual/soak-scratch/sim-N.bin --create-file \
  --engine simulated --require-direct-evidence 0 --override mount_prefix:/ \
  --size-mib 256 --task-mib 64 --single-mib 64 --threads 2 --tasks 4 --qd 32 --op-mib 4 --arena-mib 256 \
  --drop-cache --verify 1 --soak-minutes 20 --recreate-every-s 60 --sample-every-s 5 --csv /tmp/qual/soak-sim/engine.csv
pixi run ./build/release/cucascade_cufile_engine_bench ...same... --engine async \
  --override mount_prefix:/ --override fs_type:ext4 --csv /tmp/qual/soak-async/engine.csv
pixi run python tools/gds/analyze_gates.py --soak-min-s 1200 --warmup-s 60 /tmp/qual/soak-{sim,async}   # 20-min thresholds
pixi run python tools/gds/analyze_gates.py /tmp/qual/soak-{sim,async}-default                           # default 1 h -> N/A

# 3. B-1/B-3/B-4/B-5/B-6/B-9: script in this directory (same flags and manifest as qualify.sh, sizes reduced)
pixi run bash docs/io/gds-design/qualification/compat-dev-host/run-step3.sh /tmp/qual/step3 /tmp/qual/step3-scratch
#   1 GiB files, tasks 4 x 256 MiB, single 256 MiB, QD 32, arena 256 MiB, threads 1 and 4, op 1/2/4/8 MiB (B-3) else 4,
#   --drop-cache --verify 1 --min-seconds 5 --repetitions 3; variants legacy | registered (async+override) | posix | forced-compat
pixi run python tools/gds/analyze_gates.py /tmp/qual/step3

# 4. sanitizers (the documented preset needs the env flags added, see BUG-5)
pixi run bash -c 'cmake --preset asan -DCMAKE_CXX_FLAGS="$CXXFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS -fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS -fsanitize=address,undefined" && cmake --build build/asan --target cucascade_io_tests -- -j 20'
ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0:alloc_dealloc_mismatch=0:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 pixi run ./build/asan/test/cucascade_io_tests --order decl \
  "~ioctx coalescing routing preserves legacy mixed and large fan-out"        # BUG-6 excluded
#   sanitizers/asan/run.sh + shards.sh: 40 shards x fresh process, same result
# TSan: see section 6.

# 5. compat smoke
pixi run bash tools/gds/qualify.sh --compat-smoke --config docs/io/gds-design/qualification/compat-dev-host/compat-smoke.conf --dry-run
pixi run bash tools/gds/qualify.sh --compat-smoke --config docs/io/gds-design/qualification/compat-dev-host/compat-smoke.conf
```

## 4. Results

### 4.1 B-10: old `7641cf5` vs HEAD, legacy mode, forced libcufile compat, 5 alternating repetitions, drop-cache (`b10/`)

Each repetition runs 3 whole passes. 2 GiB file, 512 MiB tasks. Same benchmark source is injected into both trees; both builds use `build/release`.
Content comparison: 0 mismatches in every run. Builds: `b10/b10-builds.txt`.

| workload | old mean GiB/s | old σ | new mean GiB/s | new σ | Δ% (mean/mean) | paired mean ratio | G-4 (≥ 0.95) |
|---|---|---|---|---|---|---|---|
| sustained (scattered reads) | 6.451 | 0.036 | 6.426 | 0.018 | −0.39 % | 0.9962 | PASS |
| single (one large read) | 6.262 | 0.040 | 6.256 | 0.017 | −0.10 % | 0.9991 | PASS |
| spill (writes) | 4.772 | 0.096 | 4.737 | 0.033 | −0.72 % | 0.9930 | PASS |

Caveat: `--drop-cache` is advisory (fsync + `POSIX_FADV_DONTNEED`); ~6.4 GiB/s suggests that some reads may still come from page cache. The old/new comparison is symmetric, so G-4 stays meaningful.

### 4.2 G-7 soak, 20 min each, recreate every 60 s (`soak/`)

| | simulated | async / compat (2 threads) |
|---|---|---|
| duration, epochs, samples | 1200.6 s, 20, 227 post-warm-up | 1200.5 s, 20, 227 |
| analyzer, `--soak-min-s 1200` | **PASS** | PASS (vacuous, see below) |
| analyzer, default 3600 s | N/A | N/A |
| threads / lanes constant | 40 / 40 | 40 / 40 |
| op-pool live | 0 | 0 |
| arena, registered bytes | 256 MiB constant | 256 MiB constant |
| VmRSS min–max | 517–569 MiB | 518–635 MiB (diagnostic only) |
| quarantines, late copies, content mismatches | 0 | 0 |
| engine bytes / legacy-routed bytes | 10 448 GB / 0 | **879 GB / 4 402 GB** |
| epochs with zero engine bytes | 0 | **13 of 20** (4, 6–8, 10–16, 18, 19) |
| throughput | 8.3 GiB/s (simulated) | 3.9–5.3 GiB/s |

A 4-minute single-thread async soak behaves the same (engine bytes per epoch: 70 GB, 18 GB, **0**, 80 GB): `soak/async-1thread-4min/`.

### 4.3 B-1 / B-3 / B-4 / B-5 / B-6 / B-9 (`step3/`)

B-1 microbench (reaper component costs, `step3/lvm-d0-B-1.csv`): `cudaEventQuery` ready 0.058 µs, not ready 0.059 µs; polling 32/64/160 heads costs 1.6/3.2/8.0 µs (0.050 µs per head, p99 ≤ 9.3 µs); event record 0.11 µs; 1 MiB D2D enqueue 1.5 µs.

Engine workloads (`registered` = async engine with test-only override; `legacy` = default; `engine GB` and `legacy GB` show where the bytes actually went). `queries/op` = AM-15 / G-5 (`io`+`copy`+`stage` queries/op; ceiling 70).

| step | variant | threads | op MiB | GiB/s | engine GB | legacy GB | sync MB | queries/op | io q/op | copy q/op | stage q/op | mismatches |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| B-3 | legacy | 1 | 1 | 3.96 | 0.0 | 21.5 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-3 | legacy | 4 | 1 | 3.85 | 0.0 | 21.5 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-3 | registered | 1 | 1 | 6.62 | 36.5 | 0.0 | 0 | 14.2 | 2.4 | 11.7 | 0.0 | 0 |
| B-3 | registered | 4 | 1 | 6.66 | 38.7 | 0.0 | 0 | 14.5 | 2.4 | 11.9 | 0.0 | 0 |
| B-3 | legacy | 1 | 2 | 3.88 | 0.0 | 21.5 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-3 | legacy | 4 | 2 | 3.86 | 0.0 | 21.5 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-3 | registered | 1 | 2 | 6.46 | 35.4 | 0.0 | 0 | 17.7 | 2.0 | 15.4 | 0.0 | 0 |
| B-3 | registered | 4 | 2 | 6.62 | 38.7 | 0.0 | 0 | 18.1 | 1.9 | 15.9 | 0.0 | 0 |
| B-3 | legacy | 1 | 4 | 3.88 | 0.0 | 21.5 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-3 | legacy | 4 | 4 | 3.83 | 0.0 | 21.5 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-3 | registered | 1 | 4 | 6.28 | 34.4 | 0.0 | 0 | 18.5 | 1.6 | 16.4 | 0.0 | 0 |
| B-3 | registered | 4 | 4 | 6.57 | 38.7 | 0.0 | 0 | 28.0 | 4.3 | 23.2 | 0.0 | 0 |
| B-3 | legacy | 1 | 8 | 3.81 | 0.0 | 21.5 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-3 | legacy | 4 | 8 | 4.00 | 0.0 | 25.8 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-3 | registered | 1 | 8 | 6.39 | 34.4 | 0.0 | 0 | 28.5 | 5.3 | 22.5 | 0.0 | 0 |
| B-3 | registered | 4 | 8 | 6.59 | 38.7 | 0.0 | 0 | 30.0 | 5.2 | 24.1 | 0.0 | 0 |
| B-4 | legacy | 1 | 4 | 5.22 | 0.0 | 29.2 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-4 | legacy | 4 | 4 | 5.21 | 0.0 | 30.7 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-4 | registered | 1 | 4 | 6.33 | 35.3 | 0.0 | 0 | 32.2 | 3.9 | 27.8 | 0.0 | 0 |
| B-4 | registered | 4 | 4 | 6.17 | 27.0 | 9.8 | 2412 | 30.9 | 3.3 | 27.1 | 0.0 | 0 |
| B-5 | legacy | 1 | 4 | 6.31 | 0.0 | 34.4 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-5 | legacy | 4 | 4 | 6.29 | 0.0 | 34.4 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-5 | registered | 1 | 4 | 6.27 | 34.4 | 0.0 | 0 | 42.6 | 6.8 | 35.8 | 0.0 | 0 |
| B-5 | registered | 4 | 4 | 6.63 | 38.7 | 0.0 | 0 | 47.0 | 6.6 | 40.3 | 0.0 | 0 |
| B-6 | legacy | 1 | 4 | 4.92 | 0.0 | 26.8 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-6 | legacy | 4 | 4 | 1.36 | 0.0 | 12.9 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-6 | registered | 1 | 4 | 1.26 | 7.5 | 0.0 | 0 | 177.5 | 32.2 | 0.0 | 145.4 | 0 |
| B-6 | registered | 4 | 4 | 1.51 | 0.0 | 12.9 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-9 | legacy | 1 | 4 | 3.60 | 0.0 | 19.3 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-9 | legacy | 4 | 4 | 3.15 | 0.0 | 17.2 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-9 | registered | 1 | 4 | 5.28 | 29.0 | 0.0 | 0 | 26.1 | 2.4 | 23.2 | 0.0 | 0 |
| B-9 | registered | 4 | 4 | 5.43 | 30.1 | 0.0 | 0 | 27.1 | 2.4 | 24.2 | 0.0 | 0 |
| B-9 | posix | 1 | 4 | 1.49 | 0.0 | 8.6 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-9 | posix | 4 | 4 | 2.78 | 0.0 | 17.2 | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0 |
| B-9 | forced-compat | 1 | 4 | 5.24 | 29.0 | 0.0 | 0 | 26.5 | 2.5 | 23.5 | 0.0 | 0 |
| B-9 | forced-compat | 4 | 4 | 5.37 | 30.1 | 0.0 | 0 | 27.6 | 2.4 | 24.7 | 0.0 | 0 |

Observations (compat only; not performance claims):
* `registered` stayed on the engine for all B-3/B-5 runs (0 legacy bytes) with 0 content mismatches; throughput 6.3–6.7 GiB/s vs legacy 3.8–4.0 on this ext4/LVM volume. Legacy compat is buffered, while the engine path probably is not; do not read this as an engine speed-up.
* B-4 `registered` ×4 threads fell back mid-run (9.8 GB legacy, 2.4 MB sync): the circuit demotion of BUG-1.
* B-6 `registered`: ×1 thread **1.26 GiB/s with 177.5 queries/op** vs legacy 4.9 GiB/s (BUG-3). Repeats: 2.98 GiB/s / 28.7 q/op and 3.52 GiB/s / 55.7 q/op. ×4 threads (7 runs): 2 failed with EIO (BUG-2), 3 were routed 100 % legacy, 2 ran on the engine (42.6 and 43.4 q/op).
* B-9 `posix` (--posix-only) 1.5–2.8 GiB/s, `forced-compat` (CUFILE_FORCE_COMPAT_MODE) 5.2–5.4 GiB/s with the engine still admitted via override.
* The analyzer verdict for this run is `step3/report.md`. G-1/G-2 were N/A (no baselines). The G-3 and G-5 rows are discussed under BUG-3/BUG-4.

### 4.4 `qualify.sh --compat-smoke` (`smoke/`)

Config: `compat-smoke.conf` (one mount, `/` → `/tmp/qual/smoke-scratch`, `ALLOW_WRITES=1`). 65 s. 49 cases: 41 exit 0 and 8 exit 127 (B-2 `gdsio` not installed, expected). B-8 was skipped: it needs two mounts. `analyze_gates.py` ran at the end.

| Gate | Smoke result |
|---|---|
| G-1 ×4, G-2 ×10 | N/A (no baselines / direct evidence) |
| G-3 ×5 | **PASS** (0.98–0.99). BUG-4: should be N/A |
| G-4 | N/A (no B-10 rows in this directory) |
| G-5 ×5 | PASS (2.7–3.7 queries/op) |
| G-6, G-8 | N/A (external approval) |
| G-7 | N/A (6 s smoke, 0 post-warm-up samples) |

GDS-dependent gates (G-1, G-2) are N/A, never PASS or FAIL. The exception is G-3 (BUG-4).

## 5. Sanitizers: ASan + UBSan (`sanitizers/asan/`)

Debug build, `build/asan`, `cucascade_io_tests` (928 test cases: includes all `[engine]` (129) and `[cufile]` tests).
Final result: **full suite in one process, `--order decl`, `ASAN_OPTIONS=…alloc_dealloc_mismatch=0`: 928 test cases, 3 036 823 assertions, 1 skipped, 0 failures, 0 ASan reports, 0 UBSan `runtime error` lines**
(`full-suite-decl-order-clean.log`). 40 fresh-process shards give the same result (`shards-summary.txt`). The BUG-6 test is excluded in both. LSan is off by design (`detect_leaks=0`).

| # | Finding | Class | Severity | Raw evidence |
|---|---|---|---|---|
| S-1 | `alloc-dealloc-mismatch (operator new vs free)`, stack entirely inside `libcufile.so.0` / `libcuda.so.1`: allocation `operator new(nothrow)` in libcufile, `free` in libcufile. Triggered by `engine reads preserve the PTDS-built stream matrix` (`test_null_stream_ptds.cpp:182`, `cudaLaunchHostFunc`) | **external (vendor libcufile)**. No cuCascade frame in either stack | Info | `io-tests.unsuppressed-run1.head60.log` |
| S-2 | The same report class with **only cuCascade/libstdc++ frames**: `std::stable_sort` temporary buffer in `water_fill` (`capacity.hpp`, reached from `device_engine.cpp:1508`, test `test_engine_coalesce.cpp:432`), and `tls_support::release_rekey` (`tls_stream.cpp:710`, test `test_rest_https.cpp:166`). `new(nothrow)` is ASan's; the sized `operator delete(void*, unsigned long)` ends in `free`. I.e. the sized delete resolves to libstdc++ (GCC 15 runtime 6.0.36 with a GCC 14.3 compiler/libasan mix) instead of ASan's interceptor | **toolchain false positive** (no genuine malloc/free mixing in the sources: `delete` pairs with `new (std::nothrow)`) | Low (tooling) | `run-F1.head60.log`, `run-A-no-ptds.head60.log` |
| S-3 | `sanitizer.supp` (`interceptor_via_lib:libcufile.so`) **does not suppress S-1**: ASan's alloc-dealloc check ignores suppressions (verified: run `io-tests.log`, rc=1 unchanged). The only remedy is the global `alloc_dealloc_mismatch=0`, which also disables genuine new/free mismatch detection for cuCascade code. Run S-2 without it first (done: only S-2-type reports remain, none in cuCascade logic) | tooling gap | Low | this file |
| S-4 | Debug assert `dst != nullptr` in `host_buffer(uint8_t*)` (`include/cucascade/io/types.hpp:317`), from `test_engine_ioctx.cpp:767` `host_buffer{nullptr}` | **real defect in a test** (deterministic in Debug builds, hidden by NDEBUG in release) = BUG-6 | Low | `coalesce-1.head60.log` |
| S-5 | `Catch::OutputRedirect::activate(): Assertion '!m_redirectActive' failed` → SIGABRT. **Intermittent, only in the default random order** (5 of 7 random-order full-suite runs, plus 1 `[cufile]` subset run). The victim test is random: `sigv4_presigned_authorizer is safe under concurrent presigning` (≥ 3×) and `cufile concurrent writers of disjoint ranges sharing blocks`. The previous test had printed a SKIP or WARN message. 0 of 40 `--order decl` shards, and 0 of 2 `--order decl`/`rand` full runs after that | **test-harness (Catch2 3.8.1 debug assertion)**, root cause not found | Low–Med (blocks reliable Debug/ASan runs) | `run-B2.head60.log`, `run-C2.head60.log` |
| — | UBSan | no reports | — | `shards-summary.txt` |

No cuCascade memory-safety bug (heap/stack overflow, UAF, double free) and no UB was found by ASan+UBSan in the io tests. This does not cover CUDA kernels, device memory, or code paths that the compat-only host cannot reach (real GDS, nvidia-fs).

## 6. TSan attempt (once): failed before running (`sanitizers/tsan/`)

1. As documented in `tools/gds/QUALIFICATION.md` (plus the env flags of BUG-5): build **fails**: `host.h:77: error: 'atomic_thread_fence' is not supported with '-fsanitize=thread' [-Werror=tsan]` (from `libcudacxx`/rmm headers under `-Werror`). `build-attempt1-werror-tsan.log`.
2. With `-Wno-tsan` added, the build succeeds (432/432). Run `TSAN_OPTIONS=halt_on_error=1 build/tsan/test/cucascade_io_tests "[engine]"` → **exit 66 before `main`**: `FATAL: ThreadSanitizer: unexpected memory mapping 0x60c74365d000-0x60c7436c8000` (kernel 7.0, `vm.mmap_rnd_bits` unreadable; the same failure as the Phase-7a probe). `sanitizers/tsan/run.log`. As instructed, ASLR was not disabled.

**Outcome: no TSan coverage on this host.** `build/tsan` was deleted afterwards. A retry needs a lower `vm.mmap_rnd_bits` (root), another host, or a different TSan runtime.

## 7. Bugs found (for the fix agent; none were fixed here)

### BUG-1 (High for qualification validity): compat engine circuit-opens repeatedly and the G-7 analyzer passes anyway
* **Symptom**: in the 20-min async soak 13/20 epochs had 0 engine bytes (all legacy); epochs 0–3, 5, 9 and 17 were partially engine-routed. In the active epochs the engine stalls after 16–46 s: `sync_bytes` jumps, `completed_ops` stops, and bytes keep flowing as legacy (`soak/async/engine.csv.soak.csv` cols 16, 29; the stall is visible, e.g. at 16–46 s and 87–122 s). Active epochs are at doubling distances (4, 8…), matching `cooldown 30 s → ×2 up to 960 s` (`config.hpp:174-176`, `mount_policy.cpp:880-903`) held in a **process-wide** `mount_policy_registry` (`mount_policy.cpp:979`) that survives the benchmark's context recreation. All verifications pass, content mismatches stay 0.
* **Suspected cause (not proven)**: in compat mode the submit call blocks for the whole I/O, so `observe_stuck_call` (`mount_policy.cpp:855-857`, `stuck_ms=2` in `config.hpp:174`; submitter watch at `device_engine.cpp:2120-2145`) and/or `slow_of_last_eight` (`mount_policy.cpp:851`) opens the circuit. It is **not** triggered in a plain 40 s run without the soak sampling (`--min-seconds 40`, 494 passes, 100 % engine), so the sample/verify pause (`cufile_engine_bench.cpp` ~L850-860) is involved.
* **Analyzer defect**: `tools/gds/analyze_gates.py:418` `no_progress = all(completed_ops == 0)` is cumulative, so any early progress gives PASS even though the engine was off for most of the hour. It should require progress per sample interval or per epoch (e.g. ≥ X % of epochs or samples with `completed_ops` delta > 0), and it should treat the `legacy_bytes`-dominated epochs as N/A/FAIL.
* **Repro** (≈ 4 min): `pixi run ./build/release/cucascade_cufile_engine_bench --file /tmp/qual/x.bin --create-file --engine async --require-direct-evidence 0 --override mount_prefix:/ --override fs_type:ext4 --size-mib 256 --task-mib 64 --single-mib 64 --threads 1 --tasks 4 --qd 32 --op-mib 4 --arena-mib 256 --drop-cache --verify 1 --soak-minutes 4 --recreate-every-s 60 --sample-every-s 5 --csv /tmp/qual/x.csv` and then compare `engine_logical_bytes` and `legacy_bytes` in `/tmp/qual/x.csv.epoch-N.csv`. Observed: 70 GB / 18 GB / **0** / 80 GB engine bytes (`soak/async-1thread-4min/`).

### BUG-2 (Medium–High): async spill (B-6, compat, 4 threads) intermittently fails with EIO and aborts the run
* **Symptom**: `engine benchmark: System error at include/cucascade/io/cufile/operation.hpp:107: Input/output error` (exit 1). 2 of 7 runs with 4 threads; 3 more of 7 ran fully legacy (admission lost); 2 of 7 were fine. 1-thread runs: 0 failures out of 3. Logs: `step3/b6-repeats/log.4`, `t4-2.log`.
* **Suspected cause**: a write whose submit call blocks ≥ `stuck_ms` (2 ms) is marked `unknown_progress` (`device_engine.cpp:2126-2131`) and then fails with `io_error` under the no-replay write policy (`device_engine.cpp:1297-1300`). In compat mode, `cuFileWriteAsync` blocks during the I/O. On real GDS, a 2 ms descheduling of the submit thread could also produce a hard EIO: please check whether the threshold should apply there.
* **Repro** (loop until it happens, ~30 s each): `pixi run ./build/release/cucascade_cufile_engine_bench --file /tmp/qual/b6.bin --create-file --workload spill --engine async --size-mib 1024 --device 0 --tasks 4 --task-mib 256 --single-mib 256 --drop-cache --verify 1 --min-seconds 5 --repetitions 3 --qd 32 --arena-mib 256 --op-mib 4 --require-direct-evidence 0 --override mount_prefix:/ --override fs_type:ext4 --threads 4 --csv /tmp/qual/b6.csv`.

### BUG-3 (Medium): `queries_per_op` of the spill path is unbounded relative to the G-5 ceiling
* One engine-routed B-6 run (1 thread) measured **177.5 queries/op** (stage 145.4, io 32.2; ceiling 70; 1.26 GiB/s, completion p50 67 ms): G-5 FAIL (`step3/report.md`). Other engine-routed B-6 runs: 28.7, 55.7, 42.6, 43.4. The stage-copy query count grows with the wait time. Read paths stay at 14–47. In the smoke: 2.7.
* **Repro**: the BUG-2 command with `--threads 1`; the result varies a lot from run to run (≈ 1.3–3.5 GiB/s), so run it a few times and read `queries_per_op`, `stage_queries_per_op` in the CSV. Where to look: stage-event polling cadence for staged writes (`device_engine.cpp`, `queries`/`stage` counters).

### BUG-4 (Medium, tooling): G-3 is not N/A for compat/simulated data
`analyze_gates.py:280-300`: G-3 only goes N/A when `ratio is None`, so compat rows give PASS (smoke, 5×) or FAIL (step3, 12×: ratios 0.72–0.90 at 4 MiB/QD 32, e.g. `lvm-d0-B-3-registered-t4-op4.csv` 0.767). The requirement for this run is that gates which need GDS must not report PASS/FAIL. Either require `direct-evidence.json` for G-3, or label the row as compat-informational (also decide whether G-3 is meant to be GDS-only). Related and separate: G-3 FAIL ratios in compat could also reflect real scheduler under-fill (backlog mean/Q_eff 0.72–0.82 at 1–2 MiB ops) and deserve a look on real hardware.

### BUG-5 (Low–Med, build config): `asan` preset drops the conda env flags
`CMakePresets.json:59-62` set `CMAKE_CXX_FLAGS` / `CMAKE_EXE_LINKER_FLAGS` / `CMAKE_SHARED_LINKER_FLAGS`, which replaces the pixi `CXXFLAGS`/`LDFLAGS`. Result with the documented commands: configure OK, **build fails** (`fatal error: nvtx3/nvtx3.hpp: No such file`, `sanitizers/asan/build-attempt1-preset-as-documented.log`), and after fixing includes the binary does not start (`librmm.so: cannot open shared object file`, no `-rpath`). Same issue for a TSan configuration. Workaround (used here): pass `$CXXFLAGS`/`$LDFLAGS` on the cmake command line (§3). Fix: the preset should append to the environment flags (e.g. `-DCMAKE_CXX_FLAGS_INIT`, or add `$env{CXXFLAGS}` / `$env{LDFLAGS}`). Also `QUALIFICATION.md` must add `alloc_dealloc_mismatch=0` (S-2/S-3) to the ASan command, and the TSan section needs `-Wno-tsan` (§6).

### BUG-6 (Low, test): `host_buffer{nullptr}` trips the Debug assert
`test/io/cufile/test_engine_ioctx.cpp:767`: `prepared_io_slice(range{0, 1024}, host_buffer{nullptr})` → `types.hpp:317` `assert(dst != nullptr)` → SIGABRT in Debug/ASan builds, in test `ioctx coalescing routing preserves legacy mixed and large fan-out` (both `eligible` generator cases, deterministic: 3/3). Release (NDEBUG) hides it, which is why Phase 7a's 928-pass release run was clean. **Repro**: `pixi run ./build/asan/test/cucascade_io_tests "ioctx coalescing routing preserves legacy mixed and large fan-out"`. Fix: give it a dummy non-null pointer.

### OBS-1 (Low, test harness): intermittent Catch2 abort in random order (S-5), see §5.

## 8. Files here

| path | content |
|---|---|
| `SUMMARY.md` | this file |
| `run-step3.sh`, `compat-smoke.conf` | the exact scripts / config used |
| `b10/` | `b10.csv`, `report.md`, `gates.json`, `b10-stats.md`, `b10-builds.txt`, runner log |
| `soak/{simulated,async,async-1thread-4min}/` | `engine.csv.soak.csv` (time series), per-epoch CSVs, analyzer reports (20-min and default thresholds) |
| `step3/` | all B-1/B-3/B-4/B-5/B-6/B-9 CSVs and logs, `manifest.tsv`, `commands.log`, `report.md`, `gates.json`, `b6-repeats/` |
| `smoke/` | `qualify.sh --compat-smoke` manifest, report, gates, failures, commands, runner stdout |
| `sanitizers/asan/` | runner scripts, build logs, first 60 lines of each distinct report, clean full-suite log, shard summary |
| `sanitizers/tsan/` | both build attempts and the startup failure |

Large raw logs (`/tmp/qual/asan/*.log`, 300 MB; `/tmp/qual/smoke-results/*/cufile.log`; `metadata.json`, which contains the environment) were deliberately kept out of the repo.
Not done: no commit, no product-source or `src/cudf/` change, no `sanitizer.supp` change. `build/asan` is kept for the fix agent; the scratch files under `/tmp/qual` are retained.
