# GDS diagnostics

## Standalone cuFile async-QD harness

Phase 1 / P0 (B-0 and initial B-1 components), per
`docs/io/gds-design/{05-final-design,06-design-amendments,07-implementation-plan}.md`.
The read-only harness uses the shared cuFile loader, not the reactor or cuDF.
The target requires `CUCASCADE_BUILD_IO=ON` and `cufile.h`. It is excluded from
`all`, but built with `cucascade_io_tests` to guard against loader/API bit-rot;
it compiles the repository's cuFile loader and links CUDA/dl/threads only, not
`cucascade_cudf` or Google Benchmark.
A cudf-free configuration can use `-DCUCASCADE_BUILD_CUDF=OFF`.

```sh
pixi run cmake --preset release
pixi run cmake --build build/release --target cucascade_cufile_async_qd
# Materialize a scratch file; never use sparse/truncated files for baselines.
dd if=/dev/zero of=/mnt/disk_2/cufile-qd.bin bs=1M count=4096 oflag=direct status=progress
pixi run ./build/release/cucascade_cufile_async_qd \
  --file /mnt/disk_2/cufile-qd.bin --size-mib 4096 \
  --qd 1,2,4,8,16,32,64,128 --op-mib 1,2,4,8 \
  --seconds 5 --device 0 --register slot --mode async
# CUDA-only microbench; no file or functioning libcufile required:
pixi run ./build/release/cucascade_cufile_async_qd --microbench --device 0
```

`--help` lists defaults. `--seconds` is per QD/size pair. The existing regular
file is opened `O_RDONLY | O_DIRECT` and must be at least `--size-mib` MiB.
Reads wrap through that prefix in operation-sized windows (a short remainder is
unused). The tool never creates, truncates, or writes the file.

One `cudaMalloc` arena, sized `max(qd) * max(op-mib)` MiB, persists across
the sweep. Registrations are persistent and outside timing:

- `--register slot` (default) registers each `max(op-mib)` MiB slot separately;
  calls use that registered slot base and buffer offset zero.
- `--register arena` (`1` is an alias) registers the arena once; calls use its
  registered base plus a slot offset.
- `--register 0` skips buffer registration; calls use the slot pointer and
  buffer offset zero.

Partial registration failure is fatal and unwinds all successful registrations.
Each lane has one nonblocking stream, one timing-disabled event, and stable
pinned arguments. `--stream-register 0|1` controls optional cuFile stream
registration (default 1, used only when supported); actual use is reported
separately. Streams drain before RAII cleanup and buffer deregistration.
Every event is recorded after its cuFile call; only after a successful event
query does the host read/validate `bytes_done` and immediately refill that lane.
Admission stops at the deadline; outstanding operations drain.

CSV `result` rows report successful-byte GiB/s (including final drain), submit
p50/p99/max and host-observed completion p50/p99 in microseconds, the
`registration` mode (`0`, `arena`, or `slot`), and `bytes_done_mismatches`.
Any short/error/unset result after event completion counts as a mismatch and
causes nonzero exit. This is completion accounting,
not content-integrity verification. Submit latency measures only the cuFile
call; completion includes that call, event record, other submissions, and
polling. Percentiles use nearest rank over all measured operations.
`blocking_indicator=1` means **median submit >= 50% of median completion**;
it is a heuristic, not direct-path evidence. Slow-submit (>100 us) and query
counters are also printed. `--mode sync` is a single-host-thread serial
`cuFileRead` baseline: larger QD does not make its calls concurrent.
Before each QD/size case, every participating lane performs one untimed I/O,
waits for its event, and validates `bytes_done`. `--warmup-ops N` controls this
count per lane (default 1, range 0..1024). Use 0 only to study cold initialization:
its first submit can take milliseconds and is included in the measurements.
The default excludes lazy per-stream cuFile initialization from submit latency
and slow-submit counts. There is no cache eviction. `--poll-us N` sleeps N microseconds between polling sweeps
(default 0 / busy-spin, maximum 1000000); nonzero values reduce CPU contention
but add host-observed completion latency. The shared loader retains its
library/driver until process exit.

CSV `micro` rows measure timer overhead, ready/not-ready `cudaEventQuery`,
polling **32/64/160** ready and pending heads, event-record enqueue, and 1 MiB
D2D enqueue. Values include timer overhead; sweeps also report p50/head.
The D2D stream drains every 256 enqueues outside the timed samples, avoiding
launch-queue backpressure in the enqueue measurement.
A gated host callback makes pending events deterministic (it calls no CUDA API
and is released before synchronization), **not** a busy-compute simulation.
Run under representative external GPU load too. These initial B-1 components
do not yet measure validate-then-copy residence, timer overshoot, or QD loss.

Development smoke tests (both async variants pass on the compat host):

```sh
dd if=/dev/zero of=/tmp/cufile-qd-smoke.bin bs=1M count=256 oflag=direct status=progress
pixi run ./build/release/cucascade_cufile_async_qd \
  --file /tmp/cufile-qd-smoke.bin --size-mib 256 --qd 1,4 --op-mib 1,2 \
  --seconds 0.25 --device 0 --register slot --mode async --microbench
pixi run ./build/release/cucascade_cufile_async_qd \
  --file /tmp/cufile-qd-smoke.bin --size-mib 256 --qd 1,4 --op-mib 1,2 \
  --seconds 0.25 --device 0 --register 0 --mode async
# Optional serial baseline: repeat with --mode sync.
```

**Reported compat limitation:** Phase 1 review observed libcufile **1.17**,
ext4, no nvidia-fs returning `bytes_done = -1` (POSIX I/O verification errors)
for async `--register arena` (or `1`) at QD >= 2, despite disjoint slots. QD 1,
per-slot registration, unregistered async reads, and the sync arena baseline
passed; disabling stream registration did not fix it. Follow-up runs of the
revised harness on devices 0 and 1 at QD 2/8/32, 1/2 MiB, completed without
mismatches in arena mode, both with and without warmup. The discrepancy remains
unexplained: neither warmup nor this local pass proves shared-registration
safety. Real-GDS arena behaviour is unverified; see AM-9 for Phase 2 tests and
hardware gates. The harness preserves arena mode to measure
this distinction and exits nonzero on failed completion validation.

Small cached/compat-mode results are **not** GDS qualification. Use `/mnt/disk_2`
for NVMe comparisons and capture matched `gdsio` and attributable direct-path
evidence on real-GDS filesystems.

## Host topology collector (`collect_topology.sh`)

Read-only, best-effort host survey for GDS/cuFile performance planning
(`06-design-amendments.md` AM-7). Run it on every target host before benchmarking.
It needs no root, writes no system files and loads no kernel modules. Every probe is
guarded by `command -v`, runs under `timeout`, and records its output or
`unavailable: <reason>`; probes that would need root are listed in the report.

```sh
# Default: report to ${TMPDIR:-/tmp}/topology-<host>-<timestamp>.md.
tools/gds/collect_topology.sh

# Add O_DIRECT / stripe probes for specific directories (the only dirs it ever writes to).
OUT=/tmp/topology.md tools/gds/collect_topology.sh --probe-dir /mnt/disk_2,/gpfs/scratch

# Opt-in gdsio read+write sweep against a scratch file created in DIR (removed on exit).
tools/gds/collect_topology.sh --gdsio /mnt/disk_2 \
    --gdsio-sizes 1M,2M,4M,8M --gdsio-threads 1,8,32,64
```

Run `tools/gds/collect_topology.sh --help` for all options. Without `--probe-dir` and
`--gdsio` the script performs no writes except the report itself and a private temp dir.
The gdsio sweep takes about `2 x sizes x threads x --gdsio-duration` seconds (default
10 s per run) and records the exact `gdsio` command lines in the report.

Sections: host (kernel, CPU, NUMA, cpuset, memory), GPUs (`nvidia-smi`, BAR1,
`topo -m`, per-GPU NUMA), PCIe tree (per GPU/NIC/NVMe BDF, NUMA node, parent bridge or
switch, GPU-to-peer proximity), RDMA/NICs, GDS stack (nvidia-fs, `gdscheck -p`,
`cufile.json` with comments stripped, libcufile/`cufile.h`, CUDA versions),
filesystems (GPFS, WEKA, NFS/VAST, Lustre, BeeGFS, per-mount `stat -f` and O_DIRECT),
local NVMe/md, optional gdsio sweep.

What the output feeds:

- **Profile table, `05-final-design.md` §4.6:** filesystem class, `f_bsize` vs GPFS
  `mmlsfs -B`, Lustre stripe / BeeGFS chunk sizes, NFS `proto=rdma`/`nconnect`, and
  O_DIRECT support decide each class's `S_pref`, target QD and lane count. It also
  answers the §9 open questions on GPFS block size (Q9) and Lustre/BeeGFS layout (Q10).
- **AM-5 arena sizing:** GPU memory and BAR1 total/free bound the registration arena and
  the S_eff = 2 MiB vs filesystem-block-size comparison.
- **Gates G-1 / G-2:** the GDS-stack section shows whether the host can run real-GDS
  gates at all (nvidia-fs loaded, libcufile and `cufile.h` versions, `cufile.json`), and
  the optional gdsio sweep supplies the matched B-2 baseline for the 80 % check.
- **§4.13 affinity:** NUMA nodes, cpuset, NIC/GPU/NVMe PCI distance and RDMA port state.
