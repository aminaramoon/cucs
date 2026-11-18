# Phase 6b implementation checkpoint

Implemented on branch `io/cufile-async-engine`, on top of the existing uncommitted
Phase 6a work. **No commit, branch change, delegation, concurrent build, or
`src/cudf/` edit. Default `mode=legacy` remains unchanged.**

## Delivered

- Ten-field typed process allowlist in cuFile config; production setter/readback
  adapters gated by detected declarations **and** parameter IDs. Context creation
  freezes requests and initializes before file registration. Default legacy
  construction still does not initialize eagerly; explicit legacy process
  requests are supported. Strict unsupported/unapplied/conflicting requests throw.
  Accepted settings with unavailable readback stay unverified; after review,
  contradicting readback has status differs and strict mode rejects it. Driver
  open remains single-attempt and process-lifetime; no P2P-flags query occurs.
- Optional affinity, now correctly default **off**. Root-injected sysfs discovery,
  physical-core/cpuset planning, GPU-local reaper, NIC-local submitters, GPU-node
  fallback, mount NIC/BDF and CPU overrides, and per-thread result/reason gauges.
  Only engine-owned threads pin themselves. Submitters apply precomputed plans
  as they serve mounts; missing-input plans restore the inherited permitted mask.
- Static filesystem capacity table for NVMe, GPFS, WEKA, VAST/NFS, Lustre,
  BeeGFS and compat. Validated per-file Lustre/BeeGFS geometry updates preferred
  sizes. Mount lane ceilings integrate with fitting/water-filling; diagnostics
  retain requested and effective profiles. Profiles never grant qualification.
- Unconditional opaque optional function-pointer ABI slots. cuFile configuration
  and top-level `io_config` no longer change layout with detection macros.
  Static layout assertions plus a macro-cleared consumer TU compare API size
  and config sizes/alignments against normally compiled code.
- `docs/io/cufile-reactor.md`: architecture diagram, complete config reference,
  enabling/routing, AM-1/AM-9, gauges, recovery troubleshooting, G-1..G-8.
  Five versioned commented libcufile JSON examples and deployment instructions.
  A narrow pre-commit JSONC validator checks the commented examples instead of
  excluding them from validation altogether.

## Phase 6b files

Existing Phase 6a modifications were retained. This step changed/added:

- Build/hooks: `.pre-commit-config.yaml`, `CMakeLists.txt` (ABI comment),
  `cmake/ConfigureCuFile.cmake`,
  `src/io/CMakeLists.txt`, `test/CMakeLists.txt`,
  `scripts/check_cufile_profiles.py`.
- Public config/API: `include/cucascade/io/config.hpp`,
  `include/cucascade/io/cufile/{capacity,config,cufile_api,device_engine,mount_policy,runtime,topology}.hpp`.
- Implementation: `src/io/cufile/{cufile_api,cufile_reactor,device_engine,mount_policy,runtime,parameters,topology}.cpp`.
- Tests: `test/io/cufile/{test_async_engine_config,test_runtime_params,test_mount_policy,test_parameters_production,test_cufile_abi,test_cufile_abi_reference,test_topology}.cpp`.
- Docs: `docs/io/cufile-reactor.md`, `docs/io/cufile-profiles/README.md`,
  `docs/io/cufile-profiles/{nvme,gpfs,weka,vast,lustre}-libcufile-1.17-v1.json`,
  AM-17 appended to `docs/io/gds-design/06-design-amendments.md`, this checkpoint.

## Host process-parameter results

Measured with a separate fresh-process probe linked against the built runtime,
not the test process. libcufile **1170**, driver opened, properties available,
`nvfs.dstatusflags=0` (compat-only). Compat was explicitly permitted and forced
for this diagnostic process only. No application/system configuration was changed.

| Field | Diagnostic request | Readback | Runtime status |
|---|---|---|---|
| `allow_compat_mode` | true | true | applied |
| `force_compat_mode` | true | true | applied |
| `max_direct_io_size_kib` | 16384 | 1024 | unverified (differs) |
| `max_device_pinned_mem_size_kib` | 1048576 | 1048576 | applied |
| `max_device_cache_size_kib` | 131072 | 16896 | unverified (differs) |
| `rdma_dev_addr_list` | `["192.0.2.1"]` | no setter/ID | unsupported |
| `rdma_load_balancing_policy` | `RoundRobin` | no setter/ID | unsupported |
| `gpu_bounce_buffer_slabs` | sizes KiB `[4,1024]`, counts `[128,16]` | identical arrays | applied |
| `stats_level` | 0 | 0 | applied |
| `logging_level` | `ERROR` | `ERROR` | applied |

The two scalar mismatches were originally reported as unverified rather than
applied; the Phase 6 review fix now reports differs and rejects them in strict
mode. The table preserves the historical probe output; no inference about other
host/release modes follows. There were no unapplied entries
in this fresh-process probe. Fake-API tests cover pre-existing/external driver
opens, setter rejection, absent setters/getters, matching/differing/failed
readback, strict failure, freeze/repeat behavior and ordering. Pre-opened-driver
requests remain unapplied; no close/reopen workaround is attempted.

## Host affinity and scope limits

Both CUDA GPUs (`0000:02:00.0`, `0000:03:00.0`) expose sysfs `numa_node=-1`.
The process allowed CPU list is `0-23`; `/sys/class/infiniband` is empty.
Default affinity is off. With affinity requested, the planner leaves CPU `-1`,
`pinned=false`, reason **GPU NUMA node unavailable**. Fake-sysfs tests cover
same/split NUMA, NIC name/BDF selection, CPU overrides/cpuset intersection,
physical cores before SMT, cross-engine core reservations, missing/malformed
inputs and GPU fallback. A CUDA-backed integration test pins actual owned
threads using fake topology and confirms caller affinity is unchanged.

Deliberate limitations/deviations:

- No RDMA parameter setter IDs exist in this installed header. Those typed
  requests report unsupported; examples use documented JSON keys with
  release/client applicability marked `[verify]` instead of guessed IDs.
- IO already links the topology library through core. The new reader reuses its
  CPU-list parser, but keeps the narrow GPU/RDMA physical-core discovery in IO:
  the broad NVML discovery API lacks the required injectable GPU sysfs planner.
- NIC ranking implements same-NUMA then deterministic selection, not vendor
  path eligibility, endpoint reachability or routing steering. Hotplug/cpuset
  replanning and shared blocking-scratch first-touch remain outside this placement
  implementation. Mount placement gauges describe the latest selected plan.
- Profile values are static operational starting preferences, not frozen
  hardware-qualified tuning. AM-5's real-GDS op-size comparison and G-1..G-8
  qualification remain outstanding on this compat host. No throughput or
  default-flip qualification is claimed. Existing direct-caller-buffer and
  automatic attributable-evidence limitations remain documented.
- Templates contain full-line `//` comments. Deployment instructions strip them
  into strict JSON before using `CUFILE_ENV_PATH_JSON`; no undocumented comment
  or schema-version keys are sent to libcufile.

## Validation

All commands ran serially:

1. `pixi run cmake --preset release` — passed; all installed parameter
   declaration and requested enum-ID probes passed.
2. `pixi run cmake --build build/release --target cucascade_io_tests cucascade_cufile_async_qd cucascade_cufile_engine_bench -- -j 20` — passed.
3. `./build/release/test/cucascade_io_tests` — **922 cases: 921 passed,
   1 existing kTLS skip**; 3,041,820 assertions passed in the final full run.
4. `[engine] --order rand` — **10/10 passed**, **123 cases / 5405 assertions
   per run**. Seeds: 3136202813, 2529162614, 47680305, 3522795606, 182319661,
   919963840, 149367823, 3449618907, 290783467, 2385916552.
5. Focused `[runtime],[topology],[abi] --order rand` — **29 cases,
   419 assertions passed**.
6. `parameters.cpp` compiled syntax-only with all detection macros absent,
   `-Wall -Wextra -Werror` — passed.
7. `pixi run python -m pre_commit run --files <Phase-6b changed files>` — passed,
   including commented-profile JSON validation, formatting, spelling and CMake
   lint. `git diff --check` passed.

Temporary build/test/probe logs are under `/tmp/p6b-*`; they are not committed
artifacts or qualification evidence for another host.
