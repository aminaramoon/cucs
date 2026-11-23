# Phase 7 qualification (default remains legacy)

> **Superseded.** This harness qualified the async engine of the cuFile backend that the
> lane reactors replaced (`docs/io/cufile-gds.md`). `cucascade_cufile_engine_bench` and the
> B-10 runner (`run_b10.sh`) were removed with that backend, so the engine steps (B-3 to B-9,
> G-7), B-10 and the gates built on them cannot run. `topology`, `B-0`, `B-1` and `B-2`
> (`--only`) measure libcufile itself and still do.

Run from the repository root in the pixi environment. Compat/simulator results
are **not GDS evidence**. No default flip is authorized by these tools.

## Safety and matrix

```bash
cp tools/gds/qualify.example.conf /path/to/host.conf
pixi run bash tools/gds/qualify.sh --config /path/to/host.conf --dry-run
pixi run bash tools/gds/qualify.sh --config /path/to/host.conf --cleanup-success
# Repeat selected steps in a fresh result directory:
pixi run bash tools/gds/qualify.sh --config /path/to/host.conf --only B-0
pixi run bash tools/gds/qualify.sh --compat-smoke --only G-7
```

The config is **trusted, sourced Bash**. Root is neither needed nor recommended.
Direct I/O needs suitable group access to `/dev/nvidia-fs*`; non-root gdscheck and
nvidia-fs counters should be readable. Do not change modules, sysctls or global
cache state. `/mnt/disk_2` is the NVMe comparison path when available.

Each entry is `label|fs_class|actual_mountpoint|absolute_scratch_directory`.
The runner validates the actual findmnt target and filesystem type before writes.
All inputs are exclusive creations below `scratch/host-timestamp-pid/`;
results, TMPDIR and cuFile logs live below the unique results child. Scratch `/`
is rejected. `ALLOW_WRITES=0` disables measured writes, not input preparation.

Space preflight uses a conservative retained-matrix upper bound plus 1 GiB reserve;
`--cleanup-success` instead budgets the largest case and removes successful engine
case inputs (including task sidecars). Failed inputs are retained. Shared P0/gdsio
inputs are removed after a successful device matrix. For many devices sharing a
filesystem, reserve their **sum**; free-space preflight is not a reservation.
Per-case timeout defaults to 600 s with a 60 s kill grace; soak timeout is its
requested duration plus 300 s. Timeouts/failures are recorded and the matrix
continues, then exits nonzero. B-8 jobs are interrupted with their child timeout
processes; do not run B-8 when concurrent benchmarks are forbidden.
`--dry-run` writes nothing. A failed analyzer is also a failed runner case.

Non-smoke engine steps refuse `OPERATOR_OVERRIDE=0` (AM-11 automatic evidence is
unavailable). `--allow-legacy-engine-rows` explicitly permits diagnostic-only
runs, which cannot count as engine evidence. Set `OPERATOR_OVERRIDE=1` only after
validating the mount; it is never a `verified_direct` assertion.

Implemented cells: B-0 slot/arena QD and op-size sweeps; B-1 CUDA component costs;
B-2 sequential **and random** read/write gdsio; B-3 op-size sweep including 16 MiB;
B-4 restore; B-5 single; B-6 spill with **timed `--flush 1`**; B-8 cross-process
contention; B-9 legacy/registered/POSIX/forced-compat; G-7 sustained and restore,
plus spill when writes are enabled. Defaults are 30 s and at least three engine
passes (whole passes, not independent statistical repetitions).

Each case records command, stdout/stderr, matrix coordinates and exit status in
`manifest.tsv`. Raw nvidia-fs before/after snapshots are diagnostic only; no
invented attribution/parser is used. `metadata.json` records binary checksums,
git state, versions, cufile.json and only allowlisted CUDA/cuFile/pixi environment
variables plus PATH/LD_LIBRARY_PATH/CONDA_PREFIX. Still review paths and config
contents before sharing; never dump credentials into the result directory.

## Analyzer

```bash
pixi run python tools/gds/analyze_gates.py results/HOST-STAMP-PID
pixi run python -m unittest discover tools/gds/tests
```

`report.md` starts with failed commands and **aggregate required coverage**, then
individual cells and performance references. Missing writes are missing coverage,
not a silent success. `expected.json` (or `--expect FILE`) may specify
`{"b10_repetitions":5,"required":[{"gate":"G-2","mount":"nvme","device":"0","step":"B-6","engine":"async","direction":"write"}]}`.
An explicit table replaces the defaults: keep the full required matrix in it,
never remove cells to manufacture a pass. Defaults require B-3..B-6 for G-2/G-3,
B-1+B-3 for G-5, both B-10 arms/thread/inode/workload combinations for G-4 and
all three soak workloads for G-7, per mount/device. Hardware sign-off must also
cover every requested QD/op-size/thread/repetition coordinate via `--expect`.
Unclassified CSVs are errors; keep unrelated CSVs in subdirectories.

All acceptance inputs must be verified (`verify=1`; B-1 component micro-costs and
external gdsio have separate contracts). Nonzero command exits invalidate partial
CSV data. Engine routing requires engine bytes / (engine+legacy+sync bytes) >=99%
and **zero legacy bytes**. A demoted engine-only G-2 case fails, even without direct
proof. Otherwise missing routing/evidence is N/A, not PASS.

G-2 is async-only, never legacy or simulator. Baselines match mount, device,
direction, access pattern, effective QD and effective op size. G-2 also requires
30 measured seconds, three passes and timed flush for writes. Legacy/baseline
ratios are performance references only. G-3 requires direct evidence; compat and
simulator ratios are INFO. Its backlog integral excludes drained tails; the
refill-gap/backlog budget is <=10%, alongside mean-QD/Q_eff >=90%.

G-5a is **only** the AM-15 inclusive 70 queries/op ceiling. G-5 remains N/A until
role CPU costs, total CPU, 10k ops/s and saturated mean residence/storage-QD,
waiter scans, wakeups and inode contention are measured. B-1 costs are displayed,
not treated as the whole budget. Engine CSV includes exact sum/count mean
residence as well as coarse log2 histogram percentiles.

G-1/G-2/G-3 and arena AM-9 require reviewed direct-path evidence. Supply
`direct-evidence.json`, mapping relative CSV/log paths to
`{"qualified":true,"reason":"isolated operator-reviewed interval","artifacts":["evidence.md"]}`.
Never attest compat/simulator data. Attribution remains manual (AM-11).
G-6/G-8 need external approval; tooling cannot authorize the default flip.

## Soaks and cache observations

```bash
pixi run ./build/release/cucascade_cufile_engine_bench \
  --file /mnt/disk_2/cucs-qualification/UNIQUE.bin --create-file \
  --engine async --override mount_prefix:/mnt/disk_2 \
  --size-mib 256 --task-mib 64 --single-mib 64 --threads 2 --tasks 4 \
  --qd 32 --op-mib 4 --arena-mib 256 --drop-cache --verify 1 \
  --soak-minutes 60 --recreate-every-s 60 --sample-every-s 5 --csv results/soak/engine.csv
```

For compat tooling tests only add `--require-direct-evidence 0`. Select
`--workload sustained|restore|spill`; spill needs a write-qualified override.
Each epoch destroys/recreates the injected engine/context/arena in the same
process. Sampling and verification occur at drained whole-pass boundaries.
A verification failure exits nonzero. The final `engine.csv.ok` marker is
mandatory even for standalone runs; missing markers cannot pass.

G-7 checks every post-warm-up epoch's routing, positive completion deltas within
each epoch, no quarantine/late-copy errors, and stability of lanes, threads,
fd count, op-pool live, arena/registered/unregistered bytes and CUDA used bytes.
Arena and registration bytes have **zero tolerance**; only tracked pinned pool
bytes have the configurable tolerance. CUDA used bytes are device-wide and must
be measured on an isolated GPU. VmRSS is diagnostic; report its trend, not an
exact constancy claim. Driver allocations can require more warm-up; do not hide
growth by widening the arena tolerance. Simulator soaks are self-tests only.
Defaults: 60 s warm-up, >=3 later samples and 3600 s duration **per workload**.

Epoch and sample CSVs expose mount state, demotions, cooldown count/current
seconds, last trigger (`1=slow-of-eight, 2=stuck, 3=failure`), trigger elapsed µs
and healthy p99. `--slow-floor-us` and `--stuck-ms` permit **recorded** per-host
B-0 calibration; do not generalize compat calibration to GDS. Default product
thresholds remain 100 µs / 2 ms. See AM-18 for the measured compat trip cause.

Cache eviction is fsync+DONTNEED followed by mmap/mincore; CSV `resident_pages`
is the maximum observed after eviction. More than 1% residency warns. A low
residency sample is not proof of direct I/O and may race cache population.
B-6 `--flush 1` includes durability in its timer; B-10 remains the historical
host-complete API comparison (not a storage-write bandwidth claim).

## B-10 regression and provenance

```bash
pixi run bash tools/gds/run_b10.sh --scratch /mnt/disk_2/cucs-b10 \
  --results results/b10 --repetitions 5 --size-mib 4096 --task-mib 1024 --drop-cache
```

Builds sequentially in dedicated `build/b10` directories, with identical saved
source/header in old `7641cf5` and new trees. Both run forced compat, alternating
old/new order per iteration. Compares new legacy **and async_auto** against old
default, 1/8 callers, shared/distinct inodes, read/single/spill. Each caller has
its own stream/buffer; spill callers write identical contents to overlapping
shared-inode ranges. Thread launch cost is symmetric and included; this is not
a per-role CPU benchmark or mixed read/write integrity drill.

Dirty src/include is refused unless `--allow-dirty` explicitly requests a recorded
development run. SHA, status, binary diff hash, binaries, CMake compiler flags,
build type and CUDAARCHS are recorded. Untracked product files must be staged or
captured separately before a sign-off run; a tracked diff hash does not hash them.
Expected pair count and `failures.log` are mandatory; G-4 requires the lower bound
of a paired Student-t 95% CI >=0.95. Legacy-only historical evidence is labeled
**G-4 legacy arm (dev host)**, never full G-4. The script prints the old-worktree
cleanup command; remove only its own detached tree after retaining provenance.

## Sanitizers

```bash
pixi run cmake --preset asan
pixi run cmake --build build/asan --target cucascade_io_tests -- -j 20
ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/asan/test/cucascade_io_tests
pixi run cmake --preset tsan
pixi run cmake --build build/tsan --target cucascade_io_tests -- -j 20
TSAN_OPTIONS=halt_on_error=1 setarch "$(uname -m)" -R \
  ./build/tsan/test/cucascade_io_tests '[engine][fault]'
```

Presets preserve CXXFLAGS/LDFLAGS, instrument host code and use separate dirs.
Do not run concurrent sanitizer builds/tests. ASan does not check CUDA device
memory. LSan is disabled for process-lifetime driver pools. For allocator reports,
first check the test allocation-counter replacements and the compiler/runtime.
The Phase 7a mismatch was **not** established as a vendor/toolchain bug: the
counter replaced scalar new/delete but ASan independently intercepted nothrow new.
Its complete unaligned overload family now uses consistent malloc/free, with a
regression covering nothrow/array/fault injection. The full randomized ASan/UBSan
suite passes with alloc-dealloc checking enabled. Custom scalar allocations use
malloc tags rather than standard C++ family tags; document that limitation.
`interceptor_via_lib` **does not suppress** alloc-dealloc-mismatch. Never globally
set `alloc_dealloc_mismatch=0` for acceptance. Also note that pixi's old-style
RPATH can take precedence over LD_LIBRARY_PATH; verify actual loader resolution.

TSan `setarch -R` affects only that process and needs no root. `-Wno-tsan` is in
the preset because TSan does not model atomic_thread_fence. CUDA events/DMA are
also invisible to TSan: review event-ordered host callbacks manually, retain
reports, never suppress all cuCascade races. The Phase 7a retry now reaches tests
but reports a simulator bytes_done callback vs post-event validation race;
no claim of clean T-29/T-32 is made until CUDA happens-before is modeled.

## Required Phase 7 hardware follow-up (not implemented evidence)

Before G-2..G-8 sign-off, explicitly schedule:

- B-7 mixed priorities plus concurrent GPU kernels/D2D contention; B-8 in-process
  multi-mount DRR fairness (cross-process B-8 is not a substitute).
- Full B-1 NFR-3/NFR-4: role CPU, rate-limited 10k ops/s, mean residence,
  validate-then-copy vs pre-enqueue, timers/notifications, saturation QD loss,
  waiter scans/publication wakeups/inode guard contention.
- Engine QD/lane sweep including non-64 QD and 160 lanes; unregistered staging.
- AM-5 small-arena vs normal-arena effective sizes, using measured filesystem
  block size (including 16 MiB GPFS), not just the requested size.
- AM-9 isolated direct-verified arena vs slot registration, BAR1 before/after and
  registration cost; stream-register cost; hand-off latency; bytes_done faults.
- T-40 100 GiB mixed read/write integrity, controlled path-loss/server-pause drills
  with timeouts and recovery evidence; NIC/RDMA counters with operator tooling.
- >=30 s, >=3 **independent runs** per gate cell and throughput confidence bounds
  (three engine passes alone are not statistical replication).
- Full B-10 on a T4 and this non-GDS class, default 4 GiB/1 GiB sizes and concurrent
  mixed-direction variants; full one-hour read/restore/spill soaks per filesystem.

These need unavailable filesystems/GDS/T4 or a separate measurement implementation;
they are explicit N/A dependencies, not inferred passes from compat data.
