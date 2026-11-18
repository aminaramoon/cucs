# Phase 7 handoff: qualifying the cuFile async engine on a real GDS host

Audience: the engineer (human or agent) who will run Phase 7 on a machine with
real GPUDirect Storage (nvidia-fs loaded) and GDS-capable filesystems: local
NVMe, IBM Storage Scale/GPFS, WEKA, VAST, and optionally Lustre or BeeGFS.

Branch: `io/cufile-async-engine`. This document describes the state at the
commit that adds it. Everything here can be run from the repository root.

> **Default product behaviour is unchanged.** `async_engine_config.mode`
> defaults to `legacy`. Nothing in this branch enables the async engine unless a
> configuration asks for it. Phase 7 produces the evidence that decides whether,
> and where, it may be enabled.

---

## 0. TL;DR

1. Build the branch (§3) and run the unit tests on the host (§3.3).
2. Collect the topology and confirm that GDS is real on each mount (§4). Capture
   raw nvidia-fs counter samples (§4.4).
3. Steer cuFile's RDMA to the NICs of the GPU under test (§5).
4. Write one `qualify.conf` per filesystem class (§6) and dry-run it.
5. Run the matrix in the order of §7. **Stop after G-1** if async queue depth
   does not scale on a filesystem (§7.2).
6. Attest direct-path evidence for the runs you supervised (§8).
7. Run `analyze_gates.py` and read `report.md` (§9). Run the open-question
   experiments (§10) and the manual drills (§11).
8. Send back the result bundle (§13). The default flip is a separate decision
   (§12).

Time budget: about 10–12 h per (mount, GPU) pair for the full matrix, including
3 h of soaks. B-10 is separate, about 1–2 h. The B-0 + B-2 go/no-go subset is
about 2 h.

---

## 1. What exists on the branch

| Commit | Content |
|---|---|
| `600a3583` | Design research (`01`–`05`): cuFile patterns, uring design, cuFile gap analysis, counsel spec |
| `7641cf5` | Amendments (`06`) and implementation plan (`07`). **Pre-change baseline for B-10.** |
| `32767a1` | Phase 1: optional cuFile symbol groups, `range_plan`, `async_engine_config`, P0 harness `cucascade_cufile_async_qd`, `collect_topology.sh` |
| `5f89013` | Phase 2: `runtime` init ordering, `device_arena` (per-slot registration), lane pool, op pool, capacity fit, `mount_policy`, transport seam, simulator |
| `bf775d1` | Phase 3: `device_engine` reads (scheduler/reaper, submitter, staged reads, fence-before-D2D, enqueued-and-joined completion), facade hand-off |
| `d11c9de` | Phase 4: small-slice coalescing; engine benchmark `cucascade_cufile_engine_bench` |
| `bd2a235` | Phase 5: async device-source writes, inode sequencing, flush ordering, no replay |
| `6f6b82e` | Phase 6: latency-scaled polling, lifecycle hardening, cuFile process parameters, affinity, filesystem profiles, `docs/io/cufile-reactor.md` |
| `c137612` | Phase 7a: `qualify.sh`, `analyze_gates.py`, soak mode, B-10 tooling, sanitizer presets, fixes |
| `fc1c258` | Phase 7b: production `arena_slot_classes` (default {4 MiB}), `make_cufile_json.py`, `cufile_json_path`, NIC counters |

Read before running:

- `docs/io/cufile-reactor.md`: the operational reference (configuration,
  routing, gauges, troubleshooting).
- `tools/gds/QUALIFICATION.md`: runner and analyzer contracts.
- `tools/gds/README.md`: per-filesystem recipes.
- `docs/io/gds-design/06-design-amendments.md`: AM-1 … AM-19, the normative
  changes to the spec in `05`.
- `docs/io/gds-design/qualification/compat-dev-host/SUMMARY.md`: what was
  already measured on a compat-only host, and how.

### 1.1 Engine behaviour in one paragraph

A device read slice is routed to the per-GPU engine only if all of these hold:
- the engine is enabled and the mount is eligible (operator override; §8);
- the file is cuFile-registered and has an O_DIRECT fd;
- the caller's stream is not the null stream.

The engine then:
1. cuts the slice into pieces of `S_eff` (default arena slot class 4 MiB);
2. reads each piece with `cuFileReadAsync` into a **separately registered**
   staging slot, on one of 160 non-blocking, stream-registered lanes;
3. validates `bytes_done`;
4. enqueues `wait(fence) → D2D copy → record copy event → cudaStreamWaitEvent(user_stream)`;
5. settles the piece's credit.

The future therefore means *enqueued and ordered on your stream* (AM-1). Slots
and lanes are recycled when the reaper observes the copy event.

Writes stage the aligned body device-to-device into a slot, then call
`cuFileWriteAsync`. Head and tail go D2H then `pwrite`. Writes are never
replayed when their progress is unknown.

### 1.2 Known open issues entering Phase 7

| Issue | Where | Impact on Phase 7 |
|---|---|---|
| Shared (arena or slab) registration **silently corrupts data** under concurrent async reads on compat libcufile 1.17. Per-slot registration is clean. | AM-9, AM-11 | Default is `slot`. B-0 `arena` cells are a correctness experiment on real GDS (§10.3). Never accept a shared layout without content verification. |
| Automatic `verified_direct` is unavailable: the nvidia-fs stats parser is not implemented, because no real-format samples exist. | AM-11 | The engine only runs on mounts with an **operator override**. You must capture stats samples (§4.4) and attest evidence manually (§8). |
| The circuit-breaker defaults (100 µs slow floor, 2 ms stuck) tripped on compat contention. | AM-18 | Calibrate per filesystem from B-0 submit latencies (§10.6). Record any `--slow-floor-us` / `--stuck-ms` you use. |
| One unexplained pre-measurement timeout (rc=124) on the compat host; the retry succeeded. | 7a SUMMARY §6 | If it recurs, capture stacks (`gdb -p` / `eu-stack`) before killing the process. |
| TSan is not clean: CUDA event happens-before is invisible to it. | 7a SUMMARY §4 | G-6 stays N/A unless you model CUDA ordering. ASan+UBSan is clean. |
| `pixi run build` (the full build) fails on a pre-existing cudf-nightly API break in `src/cudf/`. | checkpoint | Build the I/O targets only (§3.2). |
| Callers that assume host-complete device reads have not been migrated to AM-1 (`src/cudf/disk_io.cpp` read path, `check_transferred`, benchmarks). | AM-1 | **Do not enable `async_reads` for the DISK tier in production** until they are migrated. Qualification uses the benchmark tools, which handle AM-1 correctly. |

---

## 2. Target host prerequisites

| Item | Requirement / check |
|---|---|
| GPU | Datacenter GPU with large BAR1 (`nvidia-smi -q -d MEMORY`). Use an **isolated** GPU for soaks (G-7 checks device-wide CUDA memory). |
| Driver / CUDA | CUDA 12.9+ or 13.x, matching the pixi environment (`pixi.toml`). The driver must support the GDS release in use. |
| GDS | `lsmod \| grep nvidia_fs` is loaded. `gdscheck -p` reports the filesystems as **Supported**. `nvidia_peermem` is loaded for RDMA filesystems. |
| libcufile | 1.17 or newer. The version is recorded automatically in `metadata.json`. |
| Permissions | Non-root, with read/write access to `/dev/nvidia-fs*` (group). Read access to `/proc/driver/nvidia-fs/stats`. **Do not run as root.** |
| Filesystems | Each mount under test: O_DIRECT supported, a writable scratch directory, enough free space (§6.3). |
| Tools | `gdsio` and `gdscheck` (usually `/usr/local/cuda/gds/tools/`), `pixi`, `git`, and `perfquery`/`ibv_devinfo` for RDMA. For GPFS: `mmdiag`, `mmlsfs`. For WEKA: `weka`. For VAST: `nfsstat`. |
| Quiet host | No other GPU or storage jobs during measurements. The runner never runs benchmarks concurrently, except B-8 (contention). |

---

## 3. Build and smoke test

### 3.1 Checkout

```bash
git clone <remote> cucs && cd cucs
git checkout io/cufile-async-engine
pixi install
```

### 3.2 Build the I/O targets

The full build is broken by an unrelated cudf issue. Build only these targets:

```bash
pixi run cmake --preset release
pixi run cmake --build build/release --target \
  cucascade_io_tests cucascade_cufile_async_qd cucascade_cufile_engine_bench \
  cucascade_legacy_compat_bench -- -j"$(nproc)"
```

If the configure step reports the cuFile header missing, `CUCASCADE_HAS_CUFILE`
is off. Fix the environment first. Nothing GDS-related builds without it.

### 3.3 Unit tests on the GDS host

```bash
./build/release/test/cucascade_io_tests 2>&1 | tee /tmp/io-tests.log | tail -5
pixi run python -m unittest discover tools/gds/tests
```

- The reference is 942 cases (941 passed, 1 skipped), measured on a compat
  host.
- Some tests probe the real library. On a real GDS host, compat-specific
  expectations may behave differently. **Record, don't fix:** send any failure
  back with its log, because it is information about real GDS behaviour.
  Do not skip tests to get a green run.

### 3.4 Thirty-second smoke test of each binary

```bash
F=/<mount>/<scratch>/smoke.bin
./build/release/cucascade_cufile_async_qd --file "$F" --prepare-file --size-mib 1024 \
  --qd 1,8,32 --op-mib 1,4 --seconds 3 --register slot --verify 1
./build/release/cucascade_cufile_engine_bench --file "$F" --size-mib 1024 \
  --engine async --override mount_prefix:/<mount> --workload sustained \
  --threads 1 --tasks 2 --task-mib 256 --qd 32 --op-mib 4 --arena-mib 256 --verify 1 --csv -
```

Expected:
- the harness: `bytes_done_mismatches=0`, and no `blocking_indicator=1` on a
  real GDS mount;
- the engine bench: `legacy_bytes=0` and `engine_logical_bytes` > 0.

If `legacy_bytes` > 0, see §14.

---

## 4. Topology and GDS stack verification

### 4.1 Topology report

```bash
OUT=results/topology-$(hostname).md tools/gds/collect_topology.sh \
  --probe-dir /mnt/gpfs/scratch --probe-dir /mnt/weka/scratch   # add each mount's scratch
```

The script is read-only and needs no root. It records:
- GPUs, BAR1 and `nvidia-smi topo -m`;
- the PCIe tree and NUMA layout;
- RDMA NICs (state, rate, NUMA), loaded modules, `gdscheck -p`, and the
  `cufile.json` in use;
- the libcufile version;
- each mount's fs type and block size, and an O_DIRECT probe of each probe dir;
- GPFS `mmlsfs -B`, WEKA `weka fs`, VAST `nfsstat -m`, Lustre/BeeGFS layouts.

### 4.2 GPU↔NIC map

From `nvidia-smi topo -m`, note for each GPU the NIC(s) marked **PIX/PXB**
(same PCIe switch). Then note those marked NODE (same socket). The
`make_cufile_json.py` mapping table in §5 must agree with this.

### 4.3 Confirm GDS per mount (not compat)

```bash
/usr/local/cuda/gds/tools/gdscheck -p                    # each FS class: Supported
cat /proc/driver/nvidia-fs/stats > /tmp/nvfs.before
/usr/local/cuda/gds/tools/gdsio -f /<mount>/<scratch>/g.bin -d 0 -w 8 -s 4G -i 1M -x 0 -I 0 -T 20
cat /proc/driver/nvidia-fs/stats > /tmp/nvfs.after
diff /tmp/nvfs.before /tmp/nvfs.after                    # read counters MUST grow by ~gdsio bytes
```

- gdsio `-x 0` is the GPU-direct path.
- If the nvidia-fs read counters do not move, that mount is **not** doing GDS
  (compat/bounce). Stop and fix the stack before qualifying it.

### 4.4 Capture nvidia-fs samples (needed for AM-11)

The automatic `verified_direct` path needs a parser for the real stats format.
None exists yet. For each mount, save:
1. idle before/after snapshots;
2. a snapshot during a gdsio read, then a gdsio write;
3. a snapshot during a `cucascade_cufile_async_qd` run at QD 32.

Put them in `results/nvfs-samples/<mount>/`. These samples are the input for
implementing the parser later. Include `uname -r`, the nvidia-fs version
(`cat /proc/driver/nvidia-fs/version` if present), and the libcufile version.

---

## 5. NIC steering for the GPU under test

Goal: run on one GPU (for example GPU 2 on a 4-GPU / 4-NIC / 2-NUMA node)
without pinning the process to a device or core, while making sure storage
traffic uses that GPU's NIC.

1. **Select the GPU per request.** The engine is per device and starts lazily.
   Benchmarks take `--device N`. Do **not** set `CUDA_VISIBLE_DEVICES` or
   `numactl` for this purpose.
2. **cuFile-managed RDMA.** libcufile 1.17 has no typed setter for
   `rdma_dev_addr_list`, so generate a per-process `cufile.json`:
   ```bash
   pixi run python tools/gds/make_cufile_json.py --gpu 2 --base /etc/cufile.json \
     --out /tmp/cufile-gpu2.json --policy RoundRobin
   # prints the GPU→NIC ranking (same switch > same NUMA > other) and:
   export CUFILE_ENV_PATH_JSON=/tmp/cufile-gpu2.json
   ```
   - The export must happen **before** the process opens the cuFile driver.
   - In-library alternative:
     `async_engine_config.process_params.cufile_json_path`, applied by
     `runtime::instance().ensure_initialized(params, strict)` **before** any
     DISK setup or `gds_available()` probe (AM-19).
   - Multi-GPU semantics of the list and `rdma_dynamic_routing` are
     **[verify]**: record what you observe.
3. **Filesystem-client RDMA.** For these, the filesystem client chooses the
   NIC, not libcufile. Configure it on the filesystem side:
   - GPFS: `verbsPorts` and the Storage Scale GPU/NIC mapping (`mmlsconfig`,
     `mmdiag --rdma`);
   - WEKA: the client `net=` interfaces;
   - VAST NFS-RDMA: `localports=` / `remoteports=` and multipath mount options.
4. **Optional engine thread placement.** Set `async_engine_config.affinity=true`
   and a mount override `nic=mlx5_X`. This pins only the engine's own reaper
   (to the GPU's node) and submitter (to the NIC's node). Application threads
   are never pinned. It does **not** route traffic.
5. **Verify:**
   ```bash
   pixi run python tools/gds/nic_counters.py --out /tmp/nic.before
   # ... run the workload ...
   pixi run python tools/gds/nic_counters.py --before /tmp/nic.before --out /tmp/nic.delta
   ```
   The expected NIC's `port_rcv_data` (reads) or `port_xmit_data` (writes)
   should carry about all of the bytes. `qualify.sh` records these deltas
   around every engine run when `RECORD_NIC_COUNTERS=1`. Shared host traffic
   pollutes the deltas, so keep the host quiet.

---

## 6. Configuring the runner

### 6.1 One config per filesystem class

```bash
cp tools/gds/qualify.example.conf /path/to/gpfs.conf   # edit; it is trusted, sourced bash
```

`MOUNTS=("label|fs_class|actual_mountpoint|absolute_scratch_dir")`. The runner
validates `findmnt -T` and the filesystem type before writing anything.

### 6.2 Recommended settings per class (starting points, not qualified values)

| Class | `ENGINE_QD` | `ENGINE_OP_MIB` | `ARENA_CLASSES` | `ARENA_MIB` | Notes |
|---|---:|---:|---|---:|---|
| Local NVMe (ext4/xfs) | 32 | 4 | 4 | 256 (≥160 needed) | Compare with `/mnt/disk_2`-style raw NVMe |
| GPFS, block ≤ 4 MiB | 64 | block size | 4 | 512 (≥320) | Check `mmlsfs -B` against the `stat -f` block size (Q9) |
| GPFS, 8 MiB block | 64 | 8 | 8 | 1024 (≥640) | Explicit 8 MiB class required (AM-19) |
| GPFS, 16 MiB block | 64 | 8 | 8 | 1024 | Op size is capped at 8 MiB; AM-5 compares 2 MiB vs 8 MiB |
| WEKA | 64 | 2 | 2 | 512 (≥160) | Then sweep QD 32–128 (Q20) |
| VAST NFS-RDMA | 64 | 1 | 1 | 256 (≥80) | `{4}` would waste 4× slot bytes per 1 MiB piece |
| Lustre | 64 | stripe ≤ 8 | stripe | 1024 | Check `lfs getstripe` |

Rule: `ARENA_MIB ≥ (ENGINE_QD + ⌈ENGINE_QD/4⌉) × class_MiB`. Otherwise the
capacity fit silently reduces QD. Always read the effective `Q_eff_*` and
`S_eff_*` columns in the CSVs, never the requested values.

Also set:
- `OPERATOR_OVERRIDE=1`, only after §4.3 passes for that mount. Without it,
  non-smoke engine steps refuse to run.
- `ALLOW_WRITES=1`, only if write qualification is in scope for that
  filesystem client version (§10.10).
- `SECONDS_PER_CASE=30` (minimum for G-2), `SOAK_MINUTES=60`.
- `RECORD_NIC_COUNTERS=1`.
- `GDSIO_BIN` and `GDSCHECK_BIN` paths.
- `ENGINE_THREADS=(1 4 16)`. Add 32 for network filesystems if CPU allows.

### 6.3 Disk space

Preflight checks a conservative bound plus 1 GiB. Plan on at least
`FILE_MIB × ~6` per mount for retained inputs; the default `FILE_MIB=4096` needs
about 24 GiB. Use `--cleanup-success` to keep only failed inputs. When several
GPUs share one filesystem, reserve the **sum**.

### 6.4 Dry run (writes nothing)

```bash
pixi run bash tools/gds/qualify.sh --config /path/to/gpfs.conf --dry-run | less
```

Check the arena classes, QD, op size and overrides on the engine command lines
before spending hardware time. One mount × one GPU is about 380 commands.

---

## 7. Run order

Run each step into a fresh results directory with `--only`, so you can analyze
between steps. Each run creates `results/<sha>-<host>-<stamp>-<pid>/`.

### 7.1 Baselines and topology

```bash
pixi run bash tools/gds/qualify.sh --config gpfs.conf --only topology
pixi run bash tools/gds/qualify.sh --config gpfs.conf --only B-2     # gdsio seq+random, read(+write)
```

### 7.2 Go/no-go: G-1 (async queue-depth scaling)

```bash
pixi run bash tools/gds/qualify.sh --config gpfs.conf --only B-0     # QD 1..128 × op × {slot,arena}
pixi run python tools/gds/analyze_gates.py results/<B-0 run dir>
```

- **G-1 passes** when, with direct evidence, throughput at QD 32 is at least
  4× QD 1 for slot registration on that filesystem.
- **If G-1 fails, stop on that filesystem.** The transport choice (async cuFile
  on many streams) is then wrong for it. Report back with B-0, B-2 and the
  `cufile.log`. Do not run the engine matrix.
- Also check B-0's submit-latency columns. Median submit at or above half the
  median completion (`blocking_indicator=1`) means async is synchronous on that
  stack (§10.6).

### 7.3 Costs, engine workloads, transports

```bash
pixi run bash tools/gds/qualify.sh --config gpfs.conf --only B-1     # reaper/CUDA micro-costs
pixi run bash tools/gds/qualify.sh --config gpfs.conf --only B-3     # sustained scattered reads, op sweep
pixi run bash tools/gds/qualify.sh --config gpfs.conf --only B-4     # DISK-restore column mix
pixi run bash tools/gds/qualify.sh --config gpfs.conf --only B-5     # single 1 GiB buffer
pixi run bash tools/gds/qualify.sh --config gpfs.conf --only B-6     # spill writes (ALLOW_WRITES=1)
pixi run bash tools/gds/qualify.sh --config gpfs.conf --only B-9     # legacy / registered / posix / forced-compat
```

### 7.4 Contention, then soak

```bash
pixi run bash tools/gds/qualify.sh --config multi.conf --only B-8    # ≥2 mounts; concurrent by design
pixi run bash tools/gds/qualify.sh --config gpfs.conf --only G-7     # 1 h each: sustained, restore, (spill)
```

### 7.5 B-10 compat regression (non-GDS host class, ideally also a T4)

```bash
pixi run bash tools/gds/run_b10.sh --scratch /<local-nvme>/cucs-b10 --results results/b10 \
  --repetitions 5 --size-mib 4096 --task-mib 1024 --drop-cache
```

- This builds the pre-change commit `7641cf5` in a detached worktree and
  alternates old/new runs. It refuses dirty trees unless `--allow-dirty` is
  given; do not use that flag for sign-off.
- G-4 requires the lower bound of the paired 95 % confidence interval of
  new/old to be ≥ 0.95.

---

## 8. Direct-path evidence attestation

The analyzer will not grant G-1, G-2 or G-3, or accept AM-9 arena rows, without
operator-reviewed evidence. For each run you supervised:

1. Run it **isolated**: no other cuFile users on the host. Snapshot nvidia-fs
   stats before and after (the runner does this around B-0; do it manually
   around engine runs you want to attest).
2. Check:
   - the nvidia-fs read (or write) byte delta ≈ the run's logical bytes
     (staging read-around included);
   - the NIC counter delta shows the bytes on the expected NIC;
   - `cufile.log` shows no compat fallback;
   - the CSV shows `legacy_bytes=0`, `sync_bytes=0`, and no demotions.
3. Write `results/<run>/evidence.md` describing the above, then create
   `results/<run>/direct-evidence.json`:
   ```json
   {"B-3/gpfs-d0-B-3-registered-t4-op4.csv":
     {"qualified": true,
      "reason": "isolated interval; nvidia-fs read delta 64.2 GiB vs 64.0 GiB logical; mlx5_2 carried 99.6%",
      "artifacts": ["evidence.md", "nvfs.before", "nvfs.after", "nic.delta"]}}
   ```
   The keys are CSV paths relative to the results directory. Never attest
   compat or simulator data.

---

## 9. Analysis and gates

```bash
pixi run python tools/gds/analyze_gates.py results/<run dir> [--expect expected.json]
```

Outputs are `report.md` (failed commands first, then required coverage, cells,
and references) and `gates.json`. Build `expected.json` listing every required
(gate, mount, device, step, engine, direction) cell. Never remove a cell to
manufacture a pass.

| Gate | Pass criterion (§8.2 plus amendments) | Inputs |
|---|---|---|
| G-1 | With direct evidence: B-0 slot QD 32 ≥ 4× QD 1 | B-0 |
| G-2 | B-3/B-4/B-5/B-6 async engine rows ≥ 80 % of the matched gdsio baseline (same mount, device, direction, access pattern, **effective** QD and op size). Also: ≥ 30 s, ≥ 3 passes, timed flush for writes, ≥ 99 % engine bytes, **zero legacy bytes**, direct evidence. | B-2, B-3..B-6 |
| G-3 | Time-weighted mean QD ≥ 90 % of Q_eff under backlog. Refill-gap/backlog ≤ 10 %. Direct evidence. | B-3..B-6 |
| G-4 | B-10 paired 95 % CI lower bound of new/old ≥ 0.95, for both arms (legacy, async_auto), 1 and 8 callers, shared and distinct inodes | B-10 |
| G-5a | `queries_per_op` ≤ 70 (AM-15) | B-3..B-6 |
| G-5 | NFR-3/NFR-4: ≤ 1 core total at 10k ops/s of 1 MiB; submitter ≤ 12 µs/op; reaper ≤ 10 µs/op; mean post-I/O residence ≤ 65 µs; storage QD ≥ 95 % of Q_eff at saturation | B-1 plus manual (§11) |
| G-6 | Every test passes, including the fault matrices, under TSan where supported | §3.3; TSan currently N/A |
| G-7 | Per workload, a 1 h soak: each post-warm-up epoch 100 % engine-routed with progress; zero tolerance on arena and registered bytes; lanes, threads, fds, op-pool and device memory stable | G-7 |
| G-8 | Default flip only after G-2..G-7 pass on local NVMe **and** at least one network filesystem. Other classes stay behind `mount_overrides`. | decision (§12) |

---

## 10. Open-question experiments

Each item maps to spec §9 or an amendment. Record the results in
`results/open-questions.md`.

1. **Q1, async parallelism.** Does N streams × `cuFileReadAsync` give N
   parallel direct I/Os? B-0 slot sweep plus `mmdiag --iohist` (GPFS) or the
   client queue stats during QD 32/64.
2. **Q2, `bytes_done` visibility and error encoding.** In B-0, fault-inject:
   read past EOF, then a file truncated mid-run. Record the values observed
   after the event.
3. **Q3 / AM-9, shared vs per-slot registration on real GDS.** B-0 `arena`
   rows with `--verify 1`. **Any content mismatch disqualifies shared
   registration.** Also record BAR1 before and after
   (`nvidia-smi -q -d MEMORY`) and registration time (engine warm-up log).
4. **Q4 / AM-11, evidence source.** The §4.4 samples. Note whether
   `cuFileStats*` or a per-process counter exists in this libcufile.
5. **Q5, where unregistered async buffers bounce.**
   `cucascade_cufile_async_qd --register 0`, comparing throughput and GPU
   memory use against `slot`.
6. **Q6 / AM-18, breaker thresholds.** Take B-0 submit p50/p99/max per
   filesystem. Recommend `slow_floor_us ≥ 4 × healthy p99` and a stuck
   threshold well above the max. Re-run G-7 with the calibrated values passed
   via `--slow-floor-us` / `--stuck-ms`, and record them.
7. **Q7, `cuFileStreamRegister` cost and benefit.** B-0 with
   `--stream-register 0` vs `1` at QD 32 and 64.
8. **Q9, GPFS block size.** `stat -f -c %S <file>` vs `mmlsfs <fs> -B`.
9. **Q12, BAR1 telemetry in containers.** Does NVML
   `nvmlDeviceGetBAR1MemoryInfo` work inside the container? Check the engine
   warm-up report (`bar1_known`).
10. **Q18, async writes per filesystem client and version.** B-6 with
    `ALLOW_WRITES=1` on a dedicated scratch area. Read back and verify
    (`--verify 1`). Record which classes get `allow_writes`.
11. **Q19, placement.** B-3 with `affinity` off vs on (mount override `nic`).
    Compare GiB/s, p99, and the NIC counters.
12. **Q20, WEKA/VAST queue depth.** Engine B-3 at `ENGINE_QD` = 32/64/96/128
    (adjust `ARENA_MIB`).
13. **AM-5, op size.** Small arena (forces 2 MiB) vs arena sized for the block
    size, on GPFS and NVMe. Use `ENGINE_OP_MIB` and `ARENA_CLASSES`.
14. **AM-10, compat async.** On a compat-mode mount, legacy vs engine (B-9
    `forced-compat` arm).
15. **AM-15, `queries_per_op`.** It must be ≤ 70 everywhere. Note the polling
    caps reported per filesystem.
16. **Q13, 160 lanes.** Run B-3 with `ENGINE_QD=128`; lanes(128) = 160. Watch
    CPU and the gauges.

---

## 11. Manual drills not automated by the runner

- **B-7 mixed priority.** Demand reads (high) alongside readahead (low) plus
  concurrent GPU kernels. Check high-tier latency percentiles and low-tier
  progress.
- **B-8 in-process multi-mount fairness** (the runner's B-8 is cross-process).
- **Full G-5 CPU budget.** Per-role CPU (`pidstat -t`) at a rate-limited 10k
  ops/s of 1 MiB, and at saturation.
- **T-40 integrity.** 100 GiB of mixed reads and writes with content
  verification across all filesystems.
- **Fault drills on network filesystems** (with the storage team): NIC link
  down during reads, a storage server pause of 30–120 s, a stale file handle.
  Expect bounded transient retries, then failure or legacy fallback. Never data
  corruption, never hangs beyond the configured timeouts, no leaked slots or
  lanes (check the gauges afterwards).
- **Statistics.** At least 3 **independent** runs per gate cell, for confidence
  bounds. The runner's 3 passes are not independent runs.

---

## 12. Enabling in production (after the gates)

Per mount, only for the filesystem classes that passed:

```cpp
using namespace cucascade::io::cufile;
config cfg;
auto& a = cfg.async_engine;
a.mode = engine_mode::async_auto;
a.async_reads = true;                      // after G-1..G-3, G-5a, G-7 for this class
a.arena_slot_classes = {4};                // per §6.2 ({8} for 8 MiB GPFS, {1} for VAST)
// a.arena_bytes = 1UL << 30;              // otherwise auto: min(1 GiB, 5 % device mem)
mount_override m;
m.match = mount_match::mount_prefix;  m.value = "/gpfs/data";
m.state = mount_override_state::operator_qualified;
m.qd = 64;  m.op_bytes = 4UL << 20;
// m.allow_writes = true; a.async_writes = true;   // only after Q18 write qualification
a.mount_overrides.push_back(m);
// a.process_params.cufile_json_path = "/etc/cucascade/cufile-gpu-local.json";  // before driver open
```

Preconditions:
- The AM-1 caller migration is done for every consumer of that context (§1.2).
- `docs/io/cufile-reactor.md` "Troubleshooting" has been reviewed with ops.

Rollback is a configuration change: `mode = legacy`, or remove the mount's
override.

---

## 13. What to send back

```
docs/io/gds-design/qualification/<host>/
  SUMMARY.md            # environment, exact commands/configs, gate table, deviations, surprises
  topology.md           # collect_topology.sh output
  configs/*.conf        # every qualify config used
  nvfs-samples/**       # §4.4 raw nvidia-fs snapshots (AM-11 parser input)
  open-questions.md     # §10 results
  drills.md             # §11 results
  results/<run>/{report.md,gates.json,manifest.tsv,metadata.json,*.csv,evidence.md,direct-evidence.json}
```

- Keep large raw logs out of git. Tar them and reference the location.
- Include every failed command (`failures.log`) and any stack captured for
  hangs.

---

## 14. Troubleshooting

| Symptom | Likely cause / action |
|---|---|
| Engine rows show `legacy_bytes > 0` or all legacy | Missing `OPERATOR_OVERRIDE=1` or override prefix; file not O_DIRECT / not cuFile-registered; null-stream caller; mount demoted (look at `mount state`, `demotions` and `last trigger` columns) |
| Demotions / `circuit_open` during runs | Submit latency above the slow floor: calibrate per §10.6. A stuck call (> 2 ms in libcufile) points to a filesystem-client issue; capture `cufile.log` |
| `S_eff` lower than requested | Arena classes don't include the size, or the arena is too small for QD × class. Fix `ARENA_CLASSES` / `ARENA_MIB` (§6.2) |
| `Q_eff` lower than requested | Arena bytes, registered bytes, or the 160-lane cap bind. Check `registered_bytes` and BAR1 |
| Registration failures at warm-up | BAR1 exhausted (other processes?), or `max_device_pinned_mem_size_kb` too small in `cufile.json` |
| `bytes_done_mismatches` / content mismatches | **Stop.** Save the files and CSV. If on `arena` rows, it is the AM-9 class of issue. On `slot` rows it is a blocker bug |
| `configuration differs; first configuration wins` | Two contexts in one process with different engine configs. Use one config per process |
| `/dev/nvidia-fs*` permission denied | Group membership; do not switch to root |
| Throughput fine but nvidia-fs counters flat | Compat/bounce path. Not GDS evidence; fix the stack |
| Hang / timeout | Capture `eu-stack -p <pid>` or `gdb -batch -ex 'thread apply all bt' -p <pid>` before the runner kills it, and send it back |

---

## 15. File map

| Path | Purpose |
|---|---|
| `tools/gds/qualify.sh`, `qualify.example.conf` | Matrix runner and config template |
| `tools/gds/analyze_gates.py`, `qualification_support.py` | Gate analyzer and metadata capture |
| `tools/gds/cufile_async_qd.cpp` → `build/release/cucascade_cufile_async_qd` | B-0 / B-1 standalone harness |
| `tools/gds/cufile_engine_bench.cpp` → `build/release/cucascade_cufile_engine_bench` | B-3..B-6, B-8, B-9, G-7 engine benchmark |
| `tools/gds/legacy_compat_bench.cpp`, `run_b10.sh` | B-10 |
| `tools/gds/collect_topology.sh`, `make_cufile_json.py`, `nic_counters.py` | Topology, NIC steering, NIC counters |
| `tools/gds/QUALIFICATION.md`, `README.md` | Tool contracts and recipes |
| `docs/io/cufile-reactor.md`, `docs/io/cufile-profiles/*.json` | Operational reference; example `cufile.json` per filesystem |
| `src/io/cufile/*`, `include/cucascade/io/cufile/*` | Engine implementation |
| `docs/io/gds-design/*` | Design, amendments, reviews, checkpoints, compat-host results |
