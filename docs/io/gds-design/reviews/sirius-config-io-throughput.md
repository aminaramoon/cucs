# Review: `config/io-throughput`: cuCascade I/O throughput configuration in Sirius YAML

- **Scope:** working tree vs `aeb027c89`, which is `git diff HEAD` plus the untracked
  `test/cpp/config/test_disk_io_config.cpp`. The dirty `experimental/doris/doris` submodule
  was ignored. cuCascade was reviewed at submodule HEAD `65996d6`.
- **Files:** `src/sirius_config.{cpp,hpp}`, `src/sirius_context.cpp`,
  `test/cpp/config/test_disk_io_config.cpp`, `test/cpp/config/test_scan_manager_config.cpp`,
  `cmake/sirius-test-sources.cmake`, `docs/super-sirius/configuration.md`.
- **Location of this file:** `docs/super-sirius/` holds curated design docs with a reading
  order and has no `reviews/` directory, so this review is at the repo root, as the task
  allowed.

## What I ran

| Run | Result |
|-----|--------|
| `pixi run cmake --build build/release --target sirius_unittest -- -j 20` | Up to date (the binary is newer than every edited source) |
| `sirius_unittest "[disk_io_config],[io_throughput_config]"` | 6 cases, 157 assertions, passed |
| `sirius_unittest "[config]"` | 164 cases, 2905 assertions, passed |
| `sirius_unittest "[scan_manager]"` | 249 cases, 20313 assertions, passed |
| `sirius_unittest "[sirius][context]"` | 20 cases, 402 assertions, passed |
| Runtime smoke 1: `integration.yaml` + `memory.disk.io {backend: uring, uring_n_reactors: 2}`, run with `SIRIUS_TEST_INTEGRATION_CONFIG`, one gpu_execution case | Passed. The log shows `SiriusContext: DISK io context: uring (1 spaces)` |
| Runtime smoke 2: same, with `backend: cufile` and `staging_mode: always` | Passed. Log: `DISK cuFile staging GPU 0: reserved 32/32 slots, 260 MiB allocated, 258 MiB registered`, then the cuCascade plan, `reserved 32 … (258 MiB)` and `DISK io context: cufile` |
| Runtime smoke 3: smoke 2 plus scan `backend: cufile` with `staging_mode: always` | Passed. The scan reserve gave `32 slots (258 MiB)`. The DISK reserve then gave **`reserved 64/64 slots, 520 MiB allocated, 516 MiB registered`**, so the slot sum works. No teardown errors |

I deleted the temporary smoke YAML files from `/tmp`. I did not modify any code.

## What checks out

- **The default is unchanged when the YAML is absent.** `_disk_io_options` stays `nullopt`
  for an absent or `null` `io` (`sirius_config.cpp:183,1090`). `reserve_disk_staging` and
  the context construction are both gated on it (`sirius_context.cpp:1036,1045`). The DISK
  spaces then keep `io_context == nullptr`, so `spill_io.hpp:35` / `resolve_disk_io_context`
  still adopt `default_disk_io_context()` lazily. `io: {}` opts in, as documented. The parser
  rewrite for `sirius.space.disk` keeps the same semantics; only the non-sequence error text
  changed. The new scan keys are additive, because before this change they were rejected as
  unknown keys.
- **Wiring order.** The context is built immediately after `memory_manager_` is constructed
  (`sirius_context.cpp:1042-1061`). That is before `cuvs_index_cache_`, the downgrade
  executors, the task scheduler and the scan manager exist, so no spill or conversion can
  run before `set_io_context`. The only owners are the DISK `memory_space` objects; the local
  `context` goes out of scope. `memory_space::set_io_context` swaps the context under the
  space lock, and `get_or_init_io_context` then returns the configured context. The cuFile
  context is created after the HOST spaces exist, which matters because its POSIX path
  borrows HOST scratch.
- **Teardown.** `release_resources()` stops the scheduler, the scan manager, the creators and
  the downgrade executors. It then synchronizes the devices, clears the repositories and
  resets `memory_manager_`. `~memory_reservation_manager` (`cucascade
  src/memory/memory_reservation_manager.cpp:144-154`) resets the DISK spaces before the HOST
  spaces, so the uring context's private pinned staging and the cuFile reactors are released
  while CUDA and the cuFile driver are live. That satisfies the `@note` in
  `disk_io_context.hpp`. Initialization rollback (`initialization_guard`) follows the same
  path, including when `make_disk_io_context` throws. No static owner is added, and
  `default_disk_io_context()` is never instantiated when the block is present, because every
  DISK space gets the configured context. The remaining hazard is a `SiriusContext`
  destroyed during static destruction. That predates this change and applies equally to the
  HOST spaces and the scan contexts.
- **Two cuFile pools, the staging budget and `reserve_cufile_staging`.** Staging slots come
  from one process-wide pool. `reserve_staging(device, N, cfg)` is "at least N in total of
  this slot size": it leases free slots first and then allocates against `allocated_bytes`
  versus the cap (`staging_pool.cpp:58-90,241-262`). The DISK reserve therefore asks for
  scan + DISK when the slot sizes match, and only the DISK count when they differ. That is
  the right formula: each reactor keeps its slots for its lifetime, so `max` would starve one
  pool. Smoke 3 confirms 64/64.
- **BAR1 double-charge.** Both Sirius pre-plans use the one `topo` snapshot taken at
  `initialize()`, before any registration. The `bar1_free/2` cap is therefore applied once,
  to the sum, so the reservation path does not double-charge. The double charge does exist
  at runtime. `make_cufile_ioctx` re-discovers topology (NVML BAR1 is always read,
  `topology_discovery.cpp:1228,1270`) after Sirius's registrations, so its
  `cap' = min(cap, (B − S − D)/2)` while `allocated_bytes` already includes S + D. Because
  `acquire()` serves free slots before it checks the cap, the pre-reserved slots stay
  usable; only allocations beyond the reservation are blocked. The eager summed reservation
  is what makes this harmless. The lazy default still suffers from it, and so did the code
  before this change.
- **Field coverage.** Every `cufile::config` and `uring::config` field is parsed for both
  surfaces. `cufile.n_max_concurrent_scans` is still rejected on purpose.
  `test_disk_io_config.cpp` asserts each field.
- **Docs defaults checked against the headers:** backend `automatic` and its resolution
  order; `uring_n_reactors` 4; `cufile_n_reactors` 0 (planned); 64 MiB of portable uring
  staging per reactor; uring `write_split_bytes` 32 MiB and `low_service_interval` 4; cuFile
  `write_split_bytes` 64 MiB; `fuse_gap_bytes` 0; `reactors_per_device` auto 32/8/16;
  `max_op_bytes` 64 MiB (compat-mode 6.83 vs 6.00 GB/s); `gds_threshold` 256 KiB (≤128 KiB
  regression on kernels ≥6.11); `staging_numa_node` −1. Little's law (12–31 reads of 8 MB)
  matches `cufile-gds.md`.
- **Cited measurements.** Commits `d5ea7d61` (fusion 52 → 77–81 and 16 → 54–58 GB/s,
  0.22 ms) and `1265be73` (150–350 ms/GiB) are both ancestors of the pinned cuCascade HEAD,
  and the numbers are quoted accurately. The recipes are labelled as starting points, not
  measured optima.
- **NIC steering.** The explanation is correct and consistent with `cufile-gds.md`:
  - cuFile picks the NIC from `rdma_dev_addr_list` and `rdma_load_balancing_policy`; thread
    CPU pinning does not.
  - The setting is process-wide, so the scan and DISK pools cannot be steered differently.
  - `CUFILE_ENV_PATH_JSON` must be set before the first cuFile use.
  - Storage Scale requires a non-empty list, `RoundRobin`/`RoundRobinMaxMin`, and no poll
    mode.
  - The key layering over `/etc/cufile.json` follows cuCascade's discovery model.
- **Anchors.** `#disk-io-siriusmemorydiskio` resolves, and the cuCascade doc anchors
  `#tuning-on-the-deployment-host` and `#sizing-the-pool-one-group-of-reactors-per-gpu`
  exist.

## Findings

### S-R1: SHOULD. The new code is not guarded by `CUCASCADE_HAS_CUFILE`

- **Where:**
  - `src/sirius_context.cpp:56-58,214-269` (`cf::gds_available`, `probe_platform`,
    `plan_cufile_pool`, `reserve_staging`, `options.io.cufile*`).
  - `src/sirius_config.cpp:599,605,1405-1408` (`opt.io.cufile_n_reactors`,
    `opt.io.cufile`).
  - `test/cpp/config/test_disk_io_config.cpp:82-83,97-118,134-161`.
- **Problem:** `io::io_config::cufile` and `cufile_n_reactors` exist only under
  `#ifdef CUCASCADE_HAS_CUFILE` (`cucascade/include/cucascade/io/config.hpp:90-104`). The
  cuFile platform, placement and staging sources are part of the cuFile backend. A build
  where `cufile.h` is not found (cuCascade `CMakeLists.txt:216-226`) would no longer compile
  Sirius. The existing code guards exactly these members (`scan_manager/config.hpp:359-362`,
  `sirius_scan_manager.cpp:4392`, `test_scan_manager_config.cpp:1080`), so the tree does
  maintain that configuration. The pixi environment ships `cufile.h`, so CI would not catch
  this.
- **Fix:** Wrap the parsing of `cufile_n_reactors` and `cufile` (reject them with a clear
  "built without cuFile" error when the macro is absent), the `resolved_cufile` step, and
  the body of `reserve_disk_staging` in `#ifdef CUCASCADE_HAS_CUFILE`, mirroring
  `reserve_cufile_staging`. Guard the cuFile assertions in the test the same way.

### S-R2: SHOULD. The docs now contradict themselves about scan `uring.use_odirect`

- **Where:** `docs/super-sirius/configuration.md:509` (new row) versus `:871-875`
  (unchanged); `docs/super-sirius/scan.md:851`.
- **Problem:** The new table row makes `uring.use_odirect` a YAML key that must agree with
  `cache.mode`. The cache section still says the derived settings, including
  `uring.use_odirect`, are "**not** individually settable from YAML". `scan.md` also still
  calls it derived only.
- **Fix:** Pick one of two options:
  - Preferred: keep the scan invariant by rejecting `use_odirect` in the scan `uring` block,
    the way `cufile.n_max_concurrent_scans` is rejected. It can only restate `cache.mode`,
    so it adds no capability. Keep it accepted in `disk.io.uring`.
  - Or update `:871-875` and `scan.md:851` to "derived; an explicit value must match".

### S-R3: SHOULD. Validation errors from the DISK sub-blocks do not say they came from `disk.io`

- **Where:** `src/sirius_config.cpp:604-605` (nested `from_yaml(*n, opt.io.uring|cufile)`
  outside `reader::optional`) and `:1405-1408` (`resolved_cufile`).
- **Problem:** The shared parsers throw `'uring.write_split_bytes': …`,
  `'cufile.max_inflight': must be positive`, `unknown config key: 'x' in uring` and
  `'cufile.devices': GPU 1 is not one Sirius reads into`. These are byte-identical to the
  scan-manager errors. With both blocks present, a user cannot tell which block is wrong.
  The tests only use `CHECK_THROWS` (`test_disk_io_config.cpp:193-194`), so message context
  is not covered.
- **Fix:** Catch and rethrow inside `read_disk_io` and around the `resolved_cufile` call
  with a `sirius.memory.disk.io.` (or `disk.io.`) prefix. Alternatively, construct the
  nested readers with a parent context. Add `CHECK_THROWS_WITH(…, ContainsSubstring("disk.io"))`
  for at least one uring, one cufile and one devices case.

### S-R4: SHOULD. The new runtime logic has no tests: context attachment and the summed staging reservation

- **Where:** `src/sirius_context.cpp:214-269,1031-1061`; `test/cpp/config/test_disk_io_config.cpp`.
- **Problem:** The tests cover only parsing and resolution. Untested:
  - that an `io` block results in **one** context shared by **all** DISK spaces;
  - that an absent block leaves `get_io_context()` null after `initialize()`;
  - that an explicitly requested backend that fails (for example, `uring` where it is
    forbidden) fails `initialize()` and rolls back cleanly;
  - the `reserve_disk_staging` arithmetic: planned vs flat pools, a scan sum only for equal
    slot sizes, and the per-device match.

  The implementer called a context fixture heavyweight. However, `test_context.cpp:576-617`
  and `:619-650` already initialize a `SiriusContext` from a YAML file under
  `[isolated_context]`, and smoke runs 1–3 above take about 2 s each.
- **Fix:**
  - Add a `[sirius][context][isolated_context]` case with a `data/disk_io.yaml` holding two
    low-level DISK spaces and `io: {backend: uring, uring_n_reactors: 1}`. Assert that both
    DISK spaces' `get_io_context()` are non-null, the **same pointer**, and of type uring.
  - Add the negative twin with no `io`, which asserts null.
  - For the reservation, factor the slot-count computation into a pure helper over
    `pool_plan` and the config, and unit-test it. Alternatively, use
    `cufile::detail::set_gds_available_for_testing` with `staging_mode: always` and assert
    `staging_usage_of(0).leased_slots + free_slots >= scan + disk`.

### S-R5: SHOULD. Recipe (a) puts the spill tier on GPFS, against the guidance in the same doc, and so creates the over-cap case it then warns about

- **Where:** `docs/super-sirius/configuration.md:992-1003,1031-1035`, versus `:183` ("Use a
  fast local mount (NVMe preferred)").
- **Problem:** The recipe spills to `/gpfs/spill` with a GPFS hint. The planner therefore
  sizes a second **32-reactor** group per GPU, giving 64 cuFile threads on the GPU's CPUs.
  It also asks for 128 slots, which exceeds the shared 1 GiB cap, as the text itself admits.
  Spilling to shared network storage competes with the scan traffic on the same NICs and
  storage servers. Nothing measured supports this as a throughput recipe, and it
  contradicts the table above it.
- **Fix:** Either point the DISK block of recipe (a) at local NVMe (`/nvme/spill`, with an
  NVMe hint so it plans 8 reactors), or keep GPFS but state why, set an explicit smaller
  `reactors_per_device` for DISK, and size `max_staging_bytes_per_device` so that the two
  pools fit.

### S-R6: NIT. The staging arithmetic was changed from the allocated size to the registered size

- **Where:** `docs/super-sirius/configuration.md:760,768,1033`.
- **Problem:** The paragraph is about `cudaMalloc`ed memory that must fit in the
  `usage_limit_*` headroom. The allocation per slot is `staging_slot_bytes() + 64 KiB`
  (`staging_pool.cpp:62`, `placement.cpp:239`), which is 8 MiB + 128 KiB. The original
  "≈ 520 MiB" was therefore correct. The new 516 MiB and 258 MiB figures are the
  *registered* sizes; smoke run 3 logs `520 MiB allocated, 516 MiB registered`. Likewise,
  "1032 MiB" is 1040 MiB in terms of the cap, which compares the allocated footprint.
- **Fix:** Restore 520 and 260 MiB (or state both the allocated and registered sizes), and
  use 1040 MiB.

### S-R7: NIT. `memory-management.md` was not updated

- **Where:** `docs/super-sirius/memory-management.md:74-90`.
- **Problem:** It still says the default is adopted "unless a context is supplied through
  the C++ API", and that the staging is reserved "at the first spill". Neither statement
  mentions the YAML path, which now reserves the staging ahead of the GPU pools.
- **Fix:** Add a sentence that links to `configuration.md#disk-io-siriusmemorydiskio`.

### S-R8: NIT. An `io` block with no DISK spaces is silently ignored

- **Where:** `src/sirius_context.cpp:1036,1045`; `src/sirius_config.cpp:1095`.
- **Problem:** `memory.disk.io` without `downgrade_root_dirs`, or with
  `capacity_bytes: 0`, parses, resolves and validates, but then builds nothing and gives no
  hint.
- **Fix:** In `initialize()`, if `get_disk_io_options()` is set and there are no DISK
  spaces, log a WARN such as "sirius.memory.disk.io ignored: no DISK space configured", or
  reject it in `resolve()`.

### S-R9: NIT. The validation is asymmetric

- **Where:** `src/sirius_config.cpp:600-611`.
- **Problem:**
  - `staging_numa_node` is rejected with `backend: cufile`, but a `uring:` sub-block or
    `uring_n_reactors` under `backend: cufile` is accepted, as is a `cufile:` block under
    `backend: uring`.
  - `staging_numa_node` is also accepted, and possibly ignored, under `automatic`.
  - The DISK `uring.n_max_concurrent_scans` (readahead only) is accepted, while the cuFile
    equivalent is rejected.
- **Fix:** Either reject sub-blocks of the backend that was not selected under an explicit
  backend, or stop rejecting `staging_numa_node` and document "ignored by the unselected
  backend" uniformly.

### S-R10: NIT. Messages and logging

- **Where:** `src/sirius_context.cpp:254-256,1048`.
- **Problem:**
  - The INFO line always says "(includes shared scan staging)", even when the scan backend
    is native or the slot sizes differ (seen in smoke 2).
  - A failure in `make_disk_io_context` (for example `std::system_error` for a forbidden
    io_uring) propagates without naming the YAML key that requested it.
- **Fix:** Print the scan share only when it was added. Wrap the call in try/catch and
  rethrow as `"SiriusContext: building the DISK io context (sirius.memory.disk.io): "` plus
  `e.what()`.

### S-R11: NIT. Recipe and doc precision

- **Where:** `docs/super-sirius/configuration.md:1165-1167,1036-1043,1146-1152,1066`.
- **Problems and fixes:**
  - **Spill-backend choice.** "tiny/noncongruent writes use buffered writeback" understates
    the header. `cufile/config.hpp` says that host-source bodies below **4 MiB**, *or* not
    congruent with the source address, "**e.g. the DISK tier's pinned pool blocks**", go
    through the page cache. In practice, most HOST→DISK spills under cuFile are buffered,
    which is the real argument for uring. Quote that.
  - **Fusion evidence.** The doc does not say whether the GB200 / Storage Scale run was
    real GDS or used `staging_mode: always`. Lines 801-804 of the same doc say IBM lists GDS
    clients as x86 only. State the mode, or add "platform mode unverified".
  - **Recipe (c).** It sets scan `uring.write_split_bytes`, which the doc itself says has
    no effect on read-only scan contexts. Drop it from the scan block.
  - **cuObject.** The "notably cuObject" remark is unsourced. Cite it or drop it.

### S-R12: NIT. Test robustness

- **Where:** `test/cpp/config/test_disk_io_config.cpp:270-277`;
  `test/cpp/config/test_scan_manager_config.cpp:1185-1186`.
- **Problems:**
  - The recipe test only calls `from_node`. It never calls `resolve()` against a topology,
    so the `devices: [0]` and `staging_numa_node` checks are not exercised for the
    documented YAML.
  - `count == 3` and the cuFile-example filter keyed on a literal GPFS path are brittle to
    doc edits.
- **Fix:** Also `resolve()` each recipe against the one-GPU topology helper. Key the
  extraction on an HTML comment marker in the doc rather than on content.

## AGENTS.md conventions

- The work targets Super Sirius code, and the docs were updated under `docs/super-sirius/`.
  `memory-management.md` was missed (S-R7).
- Builds and tests were run through `pixi run`. The implementer reports pre-commit and
  `git diff --check` passing; I confirmed `git diff --check`.
- No commits, branch changes or submodule changes were made.
- The new test is registered in `cmake/sirius-test-sources.cmake`. The list is not strictly
  sorted, so its placement is fine.
- The new test file's licence header follows the repo pattern.
- Not applicable to a working tree: the PR process (Draft first, and the reviewability
  checklist).

## Verdict

**APPROVE WITH CHANGES. There are no BLOCKING findings.**

- **Correct:** the default when no YAML is given, the wiring order, ownership, teardown
  ordering, and the summed staging reservation. Runtime smokes confirmed all of these,
  including 64/64 slots for scan + DISK.
- **Fix before merging:**
  - S-R1: the `CUCASCADE_HAS_CUFILE` guard; a build regression.
  - S-R2: the contradiction about scan `use_odirect`.
  - S-R3: DISK error context.
  - S-R4: tests for runtime attachment and reservation.
  - S-R5: the GPFS spill recipe.

BLOCKING: none. SHOULD: S-R1, S-R2, S-R3, S-R4, S-R5.

## Resolution

All five SHOULD findings and all seven NITs are addressed. No BLOCKING findings were
reported. No commits or submodule changes were made; `experimental/doris/doris` was ignored.

| Finding | Resolution |
|---------|------------|
| S-R1 | Guarded DISK cuFile members, staging code/includes, resolution and test assertions with `CUCASCADE_HAS_CUFILE`. Explicit cuFile backend/settings now report "built without cuFile support" when unavailable. |
| S-R2 | Kept matching explicit scan `uring.use_odirect` values supported. Updated both the cache section and `scan.md` to say the value is derived and must agree with `cache.mode`. |
| S-R3 | Added `sirius.memory.disk.io` context to nested parser and device-resolution errors. Invalid-input tests require `disk.io` in both YAML surfaces, including uring, cuFile and device errors. |
| S-R4 | Added a two-DISK-space fixture and isolated runtime tests for shared pointer identity, uring backend, absent-block laziness, and explicit startup failure/rollback/recovery. The Linux failure test denies `io_uring_setup` only in a disposable thread and its children. Factored pure staging-count helpers and tested planned/flat counts, matching and mismatched slot sizes, absent scans, zero slots and device matching. |
| S-R5 | Recipe (a) now spills to local `/nvme/spill` with a local hint and eight DISK reactors, rather than a second 32-reactor network pool. Its shared staging budget is about 650 MiB allocated per GPU. |
| S-R6 | Distinguished allocated versus registered staging sizes: 520/516 MiB and 260/258 MiB; two 32-reactor pools need 1040 MiB allocated. |
| S-R7 | Linked the YAML path from `memory-management.md` and documented eager context attachment/staging versus lazy defaults. |
| S-R8 | Added a WARN when DISK I/O options are configured but no DISK spaces exist. |
| S-R9 | Removed the asymmetric cuFile/staging-NUMA rejection; documented that settings of the unselected backend are ignored, including staging NUMA placement. Added acceptance coverage. |
| S-R10 | Log scan-sharing only when scan slots are actually added. Context factory exceptions now identify the DISK YAML key; the startup-failure test checks this context. |
| S-R11 | Clarified buffered host writes below 4 MiB or with noncongruent addresses, labelled fusion platform mode unverified, removed the ineffective scan write-split recipe key and the unsupported cuObject reference. |
| S-R12 | Recipe tests now resolve against a one-GPU topology. HTML markers delimit the recipes and identify the scan example; extraction no longer relies on a GPFS path or exactly three recipes. |

### Validation after fixes

- `pixi run cmake --build build/release --target sirius_unittest -- -j 20`: passed,
  including a rebuild after clang-format.
- Combined regression run: **469 cases, 23,616 assertions passed**. Filters:

  ```text
  [config],[scan_manager],[sirius][context],[config_opt],[context_config],
  [chunked_spill_copy],[downgrade_disk],[downgrade_executor],[downgrade_lifecycle],
  [downgrade_acquisition_failure],[gpu_pipeline_disk],[spill],
  [spill_converter_ordering],[spill_policy],[io]
  ```

- Additional cuFile-disabled compile coverage: syntax-only compilation of
  `src/sirius_config.cpp`, `src/sirius_context.cpp`,
  `test/cpp/config/test_disk_io_config.cpp` and `test/cpp/config/test_context.cpp`
  using their release compile commands with `-UCUCASCADE_HAS_CUFILE`: passed.
  This is not a full separately configured cuFile-disabled link/runtime build.
- File-scoped `pixi run pre-commit run --files ...`: clang-format updated files on the
  first pass; final rerun passed. `git diff --check` passed.

Validation logs: `/tmp/sirius-io-review-build.log`, `/tmp/sirius-io-review-tests.log`,
`/tmp/sirius-io-review-no-cufile.log`, `/tmp/sirius-io-review-precommit.log`.
