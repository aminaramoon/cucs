# Instructions for a GDS-host agent: run the experiments and finish the missing pieces

You are an agent with shell access to a machine that has **real GPUDirect
Storage** (`nvidia_fs` loaded, `gdscheck -p` shows Supported) and at least one
GDS-capable filesystem: local NVMe, IBM Storage Scale/GPFS, WEKA or VAST.

Your job:
1. Measure.
2. Decide using the decision rules in §7.
3. **Fix the missing pieces listed in §2.**
4. Push your work and report.

You work on two repositories; the cuCascade one is a submodule of Sirius.

Read this whole file before starting. Where it says *[verify]*, the claim is
unconfirmed on real GDS and confirming it is part of your job.

---

## 0. Where things are

| Repo | Remote | Branch | Head at hand-off |
|---|---|---|---|
| Sirius | `github.com/aminaramoon/sirius` | `config/io-throughput` | pins the cucascade commit below |
| cuCascade | `github.com/aminaramoon/cucs` (Sirius's `.gitmodules` points at `NVIDIA/cuCascade`, which serves these commits through the fork network) | `io/cufile-async-engine` | the commit Sirius pins (this file's commit, after `c600a71`) |

```bash
git clone https://github.com/aminaramoon/sirius.git && cd sirius
git checkout config/io-throughput
git submodule update --init --recursive
pixi run make                      # full Sirius build (library, tests, DuckDB extension)
```

If the build fails with `fatbinary fatal: Could not open input file *.cubin`
or `sccache: Compiler killed by signal 1`, the sccache server is broken. Run
`pixi run sccache --stop-server`, then build again. It is not a code problem.

**cuCascade standalone build** (needed for its tests and the GDS tools), run
inside `sirius/cucascade/`:

```bash
cd cucascade
pixi run cmake --preset release
pixi run cmake --build build/release --target cucascade_io_tests \
  cucascade_cufile_async_qd cucascade_cufile_engine_bench cucascade_legacy_compat_bench -- -j"$(nproc)"
```

If `pixi run build`, the full cuCascade build, fails in `src/cudf/` because of
the cudf nightly in cuCascade's own pixi environment, ignore it. Only the
targets above are required. Sirius builds the cudf layer with its own
environment.

### Documents to read first (all under `cucascade/docs/io/`)
- `gds-design/08-phase-7-handoff.md`: **the qualification runbook.** It covers
  topology, gdsio baselines, `qualify.sh`, direct-evidence attestation, gate
  definitions, open questions and drills. This file does not repeat it.
- `cufile-gds.md`: the **reactor-pool design** (Sirius's existing cuFile path).
- `cufile-reactor.md`: the **async engine** reference (configuration, routing,
  gauges, troubleshooting).
- `gds-design/06-design-amendments.md`: AM-1 … AM-19, the normative changes to
  the spec in `05-final-design.md`.
- `gds-design/qualification/compat-dev-host/SUMMARY.md`: what was measured
  without GDS.
- Sirius `docs/super-sirius/configuration.md`: the `scan_manager.cufile`,
  `cufile.async_engine` and DISK `io` blocks, and the throughput recipes.

### What the code is
- **One cuFile reactor class, `cufile_reactor`**: the reactor-pool design from
  cuCascade `65996d6`.
  - It plans per-GPU reactor groups (`plan_cufile_pool`, sized by Little's law),
    with one blocking `cuFileRead`/`cuFileWrite` per reactor thread.
  - Data goes through registered staging slots (`staging_pool`).
  - Ordering against the caller's stream uses `ready_gate`.
  - It has `fuse_gap_bytes` fusing and staged GDS writes.
  - This is the default path: **`cufile.async_engine.mode: legacy`**, which
    means "engine off".
- **`device_engine`**: an optional per-GPU async engine.
  - It issues `cuFileReadAsync`/`cuFileWriteAsync` on non-blocking stream lanes,
    through a registered arena with per-slot registration.
  - Polling is deadline-driven.
  - Device reads complete as *enqueued and ordered on the caller's stream* (AM-1).
  - It is reachable only with `mode: async_auto`, and only for mounts with a
    `mount_overrides` entry in state `operator_qualified`.
  - The reactor hands it eligible device-only slices and serves everything else
    itself.

---

## 1. Rules

- Work on branches off the heads above:
  - cuCascade: `io/cufile-async-engine-gds-<topic>`
  - Sirius: `config/io-throughput-gds-<topic>`
- Commit small, with conventional commit messages. **Never force-push.**
- Push to the `aminaramoon` forks only. If a Sirius change needs a cuCascade
  change, push the cuCascade commit first, then bump the submodule pin in the
  Sirius branch.
- **Do not change product defaults** (`mode: legacy`, `async_reads` default for
  DISK, profile defaults) without the evidence the decision rules in §7 require.
  Record every default change in an amendment, AM-21 or later.
- **Verify a branch before calling it done**, in this order:
  1. a fresh clone of the Sirius branch, `git submodule update --init --recursive`;
  2. `pixi run make`;
  3. `pixi run make test` (or at least `sirius_unittest "[config],[disk_io_config],[scan_manager]"`);
  4. cuCascade `cucascade_io_tests`.
- **Do not run as root**, change modules or sysctls, or write outside the
  configured scratch directories. Keep the host quiet during measurements: one
  benchmark at a time, except B-8.
- **Report in** `cucascade/docs/io/gds-design/qualification/<host>/`, following
  08 §13. Keep raw logs out of git.

---

## 2. Missing pieces you must finish

They are ordered by dependency. M1–M4 need no GDS and come first: they make
the measurements trustworthy.

### M1. Validate the merge (no GDS needed)
Commit `46ae33b` merged the engine onto `65996d6`. After it, only the Sirius
build plus `[config],[disk_io_config],[scan_manager]` and
`[multi_gpu_foundation]` were checked. The full suites were not re-run.

1. Run the full `cucascade_io_tests` and loop `[engine]` 10× with
   `--order rand`.
   - The last partial run, before the final fixes, was 643/645.
   - One failure was a SIGABRT from an off-thread-assertion guard that the
     merge reverted to the `65996d6` version of `test/unittest.cpp`. **Find
     which test asserts off-thread** (test name, file:line) and fix the test.
     Then restore the guard (phase 7a OBS-1).
2. Run the full Sirius unit suite (`pixi run make test`). Fix regressions.
3. Smoke-tested on a compat host after the merge (B-3, 2 threads):
   - async engine: 5.9 GiB/s, mean QD 26.8, 21.8 queries/op, content-verified;
   - legacy (flat 1-reactor bench arm, see M3): 3.2 GiB/s.

### M2. Review the engine wiring and write AM-20
Diff `65996d6..io/cufile-async-engine` for `src/io/cufile/cufile_reactor.cpp`,
`cufile_ioctx.cpp` and `include/cucascade/io/cufile/cufile_reactor.hpp`: about
785 added lines of hand-off code. Confirm, and fix if wrong:

- **Legacy mode is byte-identical to `65996d6`:** no extra fence recording, no
  inode sequencing cost, same routing, same defaults.
- **The engine consumes `device_buffer::ready_gate` as its input fence.** It
  records its own event only when no gate was recorded. PTDS-origin reads
  complete host-complete; null-stream reads stay on the reactor.
- **Inode write sequencing** covers only contexts with `async_writes` enabled,
  including their reactor fallbacks. Two contexts writing the same inode
  concurrently (one legacy, one engine) is unsupported.
- **The engine's arena and the reactor's `staging_pool` share one GPU.** Both
  register device memory. Check that their BAR1/pin budgets cannot jointly
  over-commit (`staging_usage_of`, arena `registration_target`). If they can,
  make the arena subtract the pool's registered bytes.
- **`fuse_gap_bytes` (reactor) and `coalesce` (engine) never group the same
  request twice.** Define the precedence.

Write **AM-20 "Merge onto 65996d6"** in `06-design-amendments.md` with these
decisions.

### M3. Make the engine benchmark's legacy arm fair
`cucascade_cufile_engine_bench --engine legacy` builds
`cufile_reactor::reactor_context` with `--reactors N` (default 1), a flat pool.
Sirius's real legacy path is the **planned per-GPU pool**
(`cufile::make_cufile_ioctx(0, cfg)` with `storage_hint_path`, 8/16/32 reactors
per GPU, staging slots). Add:
- `--reactors 0`, meaning the planned pool, as the default for `legacy`;
- `--storage-hint PATH`;
- pass-through options for `reactors_per_device`, `staging_piece_bytes`,
  `staging_slots_per_reactor`, `fuse_gap_bytes` and `staging_mode`;
- the plan in the CSV (`total_reactors`, staging bytes).

Update `qualify.sh` so B-3..B-9 compare the engine against the **planned**
reactor pool, plus a `reactors_per_device` sweep. Until this is fixed, every
legacy-vs-engine number is unfair to the reactor.

### M4. AM-1 caller migration in the cudf layer
Engine-served device reads resolve when the copies are *enqueued* on the
caller's stream. These callers assume the data is already in place (AM-1 list):
- `src/cudf/disk_io.cpp` read path (`read_disk_file_device`)
- `check_transferred`
- the disk benchmarks
- any host-side consumer

Make each one synchronize, or wait on, the stream before host or foreign-stream
access. Add a test that runs the DISK→GPU restore with `mode: async_auto`
through the simulator (`async_simulated_for_testing`) and checks the bytes
without an extra sync from the test. **Until this lands, DISK-tier
`async_reads` must stay off.**

### M5. Automatic qualification (needs GDS)
Today the engine routes only `operator_qualified` mounts, because automatic
evidence is unavailable (AM-11). Two routes, in order:

1. **Collect real `/proc/driver/nvidia-fs/stats` samples** (08 §4.4): idle,
   during gdsio read and write, during `cucascade_cufile_async_qd` at QD 32.
   Implement the stats parser behind the `mount_policy` evidence-source
   interface (AM-11). Use isolated-interval attribution, and treat the
   snapshot as a whole-host counter.
2. **Topology-driven qualification** (proposed by the user). `probe_path()`
   already returns `path_capability::direct_candidate` when every one of these
   holds:
   - the driver reports a GDS mode for the storage class (`dstatusflags`);
   - the filesystem is supported;
   - the file is O_DIRECT and cuFile-registered.

   **Experiment:** on each mount, check whether `direct_candidate` matches the
   nvidia-fs counter evidence (direct bytes ≈ logical bytes) for gdsio and for
   the engine.
   - **If they match**, make `mount_policy` promote a mount on
     `direct_candidate`, in a new state `driver_qualified` that is routable and
     keeps the demotion breaker. Then make `mount_overrides` optional:
     - derive `qd`/`op_bytes`/`lanes` from the filesystem profile, geometry and
       capacity fit (already implemented);
     - derive `nic`/`cpus` from `topology_discovery` (`network_devices`, GPU
       CPU affinity);
     - optionally generate the `cufile.json` NIC list (the
       `make_cufile_json.py` logic).
   - The user-facing config should become just
     `async_engine: {mode: async_auto}`. Update the Sirius docs and recipes.
   - **If they don't match**, document where they diverge, and keep overrides
     required.

### M6. Calibrate per filesystem (needs GDS)
From B-0 and B-3, per filesystem class:
- **Profile table** in `mount_policy.cpp::profile_for_filesystem`: QD and op
  size. Also compare against the reactor planner's 8/16/32 so both paths agree.
- **Demotion thresholds**: `slow_floor_us`, `stuck_ms`. The compat defaults
  tripped falsely (AM-18). Use about 4× the healthy p99 submit latency and well
  above the max.
- **Default `arena_slot_classes`** per class: GPFS block size, VAST 1 MiB.

Update the profile defaults only with the evidence §7 requires.

### M7. Shared registration on real GDS (AM-9)
B-0 `--register arena` with `--verify 1`, per filesystem. Any content mismatch
means shared registration stays forbidden. If it is clean on every filesystem,
also measure the BAR1 and registration-time savings, and report them. Do not
change the `slot` default without user approval.

### M8. NIC steering on a multi-NIC node
On a node with several GPUs and NICs:
- generate a per-GPU `cufile.json` with `tools/gds/make_cufile_json.py --gpu N`;
- run the engine bench and the Sirius scan on GPU N;
- check with `tools/gds/nic_counters.py` that ≥95 % of the bytes cross that
  GPU's PCIe-local NIC(s).

Test both cuFile-managed RDMA (`rdma_dev_addr_list`) and the filesystem
client's own setting (GPFS `verbsPorts`, WEKA `net=`, VAST `localports=`).
Record what libcufile actually does with multiple GPUs per process
*[verify]*.

---

## 3. cuCascade-level experiments (needs GDS)
Follow `08-phase-7-handoff.md` §4–§11 for each available filesystem. With M3
done, every B-3..B-9 comparison has **three arms**:

| Arm | How |
|---|---|
| Reactor, planner default | `--engine legacy --reactors 0 --storage-hint <file>` |
| Reactor, tuned | the best `reactors_per_device` × `staging_piece_bytes` × `fuse_gap_bytes` from the reactor sweep below |
| Engine | `--engine async --override mount_prefix:<mount>`, with `--qd/--op-mib/--arena-mib/--arena-classes` per 08 §6.2 |

**Reactor-pool sweep.** The tuning sweep shipped with the reactor-pool design:

```bash
CUCASCADE_IO_TUNE_FILE=/<mount>/<big existing file> \
CUCASCADE_IO_TUNE_DEPTHS=1,2,4,8,16,32,64 CUCASCADE_IO_TUNE_PIECES_MIB=4,8,16 \
CUCASCADE_IO_TUNE_FUSE_GAP_KIB=0,64,1024 \
./build/release/test/cucascade_io_tests "[.cufile-tune]"
```

(See the header of `test/io/cufile/test_cufile_tune.cpp` for every variable.)

Collect the gates G-1..G-7 per 08 §9. Attest direct evidence per 08 §8.

---

## 4. Sirius-level experiments (needs GDS)
Run Parquet scans and TPC-H through Sirius with each config arm below. Use
Sirius's existing performance scripts (look under `test/tpch_performance/`,
`bench/` and `scripts/`), with `SIRIUS_CONFIG_FILE=<arm>.yaml`. Use the same
data, warm-up and best-of-3 for every arm. Record per-query time, scan GiB/s,
GPU utilisation, and the cuFile and engine gauges from the logs.

Arm A, reactor defaults, a network filesystem:

```yaml
sirius:
  executor:
    scan_manager:
      backend: cufile
      cufile_n_reactors: 0
      cache: {mode: none}
      cufile:
        storage_hint_path: /<mount>/<existing data file>
```

- **Arm B**: reactor tuned. Arm A plus the best values from the §3 sweep
  (`reactors_per_device`, `staging_piece_bytes`, `staging_slots_per_reactor`,
  `max_staging_bytes_per_device`, `fuse_gap_bytes`), and the recipe in
  `configuration.md`.
- **Arm C**: engine. Arm B plus:

```yaml
      cufile:
        async_engine:
          mode: async_auto
          arena_slot_classes: [4]          # per 08 §6.2
          mount_overrides:
            - {match: mount_prefix, value: /<mount>, state: operator_qualified, qd: 64, op_bytes: 4Mi}
          demotion: {slow_floor_us: <calibrated>, stuck_ms: <calibrated>}
          process_params: {cufile_json_path: /<per-gpu cufile.json>}
```

- **Spill tier**: with GPU memory limited enough to force spills, compare
  `sirius.memory.disk.io.backend` set to `uring` against `cufile`, each with
  default and tuned reactor counts.
  - Measure both GPU→DISK and HOST→DISK spill throughput, and restore time.
  - Expectation to test *[verify]*: cuFile wins on device spills on GDS hosts,
    and uring wins on host-buffer spills.
  - Keep `async_engine` off for the DISK tier until M4 lands. After M4, add an
    arm with `io.cufile.async_engine: {mode: async_auto, async_reads: true,
    async_writes: <only if write-qualified>}`.
- **Multi-GPU**: repeat the best arm on all GPUs at once (shared NIC, PCIe or
  storage limits).

Check every arm's correctness: query results must be byte-identical to the
reactor-default arm.

---

## 5. Correctness drills (needs GDS)
- **T-40 integrity:** 100 GiB of mixed reads and writes through the engine and
  through the reactor, with content verification.
- **Network fault drills,** with the storage team: NIC link down, storage
  server pause of 30–120 s, stale handle. Expected behaviour:
  - bounded retries, then the reactor fallback or a clean error;
  - no hang beyond the timeouts;
  - no corruption;
  - gauges show no leaked slots or lanes.
- **Sanitizers:** ASan/UBSan presets (`tools/gds/QUALIFICATION.md`) on the GDS
  host, `[engine]` plus the cuFile tests.

---

## 6. What to send back
Write `cucascade/docs/io/gds-design/qualification/<host>/SUMMARY.md`, plus the
artifacts listed in 08 §13. It must contain:
1. The environment: GPU, BAR1, NICs, filesystems, `gdscheck -p`, libcufile
   version, `cufile.json`.
2. M1–M8 status: done / blocked (why), with commits.
3. Gate table G-1..G-8 per filesystem, with the evidence links.
4. Three-arm results (reactor default / reactor tuned / engine), for the
   cuCascade benches and for the Sirius queries.
5. Recommended defaults per filesystem class: reactor knobs, engine knobs,
   whether the engine should be the default for that class, and the DISK-tier
   backend.
6. Everything that surprised you, and every *[verify]* item resolved.

---

## 7. Decision rules
- **Engine default for a filesystem class**
  (`mode: async_auto` + automatic qualification): only if, on that class, the
  engine
  - is ≥ 10 % faster than the **tuned** reactor on B-3/B-4/B-5 *and* on the
    Sirius TPC-H scan-heavy queries;
  - passes G-2, G-3, G-5a and G-7;
  - shows no correctness differences.
- **Otherwise the reactor stays the default for that class.** Record the
  engine as an opt-in, with the measured trade-off.
- **Shared registration:** never, unless M7 shows zero mismatches on every
  filesystem, and the user approves.
- **Any default change** needs an amendment (AM-21 or later), updated docs
  (`cufile-reactor.md`, Sirius `configuration.md`) and a test pinning the new
  default.
