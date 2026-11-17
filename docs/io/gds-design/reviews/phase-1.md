# Phase 1 review: foundations

- Branch: `io/cufile-async-engine`, working tree compared with HEAD `7641cf5`.
- Scope: plan `07-implementation-plan.md` Phase 1 items 1–5, checked against `05-final-design.md` (FR-11, FR-12, FR-35, §6.1, §6.2, §6.16), `06-design-amendments.md`, and AGENTS.md.
- Reviewed: every hunk of `git diff HEAD`, plus the untracked files:
  - `cmake/ConfigureCuFile.cmake`
  - `include/cucascade/io/range_plan.hpp`
  - the three new test files
  - `tools/gds/{cufile_async_qd.cpp,collect_topology.sh,README.md}`
  - the checkpoint note

## Verification performed

| Check | Result |
|---|---|
| `cmake --build build/release --target cucascade_io_tests cucascade_cufile_async_qd` | Up to date. The warnings-as-errors flag set applies to the harness. |
| `cucascade_io_tests` (full) | 639 cases: 638 passed, 1 skipped. The baseline was 610 cases, so 29 are new. No regressions. |
| `[range_plan]`, `[optional]`, `[cufile][config]` | All pass: 29 cases, 1562 assertions. |
| `ldd cucascade_cufile_async_qd` | No cudf, no cucascade, and no libcufile at link time (dlopen only). |
| Harness `--microbench` (RTX 6000 Ada) | OK. `cudaEventQuery` takes about 0.06 µs per head. Polling 160 heads takes about 8.3 µs. Record takes about 0.11 µs; 1 MiB D2D enqueue takes about 1.5 µs. |
| Harness I/O, ext4, compat-mode libcufile 1.17 | **Fails at QD ≥ 2 with `--register 1` (the default)**. See P1-R1. |
| `collect_topology.sh` as non-root, `OUT=/tmp/…` | 1.3 s, rc 0. The only new file is the report. `bash -n` is clean. |

## Confirmed correct, no finding

- **Symbol groups (FR-11, §6.1).**
  - The eight required symbols are still all-or-nothing, and the log text is unchanged.
  - Async-read and async-write are single-symbol groups and are independent.
  - Stream-register and batch are all-or-nothing, and a partial group is nulled.
  - Parameters are compiled per symbol: `has_parameters` is set only when every declared prototype resolved, and individual pointers are kept.
  - `cuFileGetVersion` is still resolved separately.
  - No resolved function is called, and no driver open or setter happens at load.
- **No default behaviour change.**
  - `cufile_api::get()`/`open_driver()`/`version()` behave as before, apart from extra side-effect-free `dlsym` lookups.
  - `config::async_engine` is the last data member and is not read by any runtime path.
  - `probe_path(..., config const& cfg = {}) noexcept` remains noexcept-safe, because a default `std::vector` does not allocate.
- **uring extraction is identical.**
  - The old `staged_physical_range(logical, file_size, use_odirect)` became `direct ? range_plan::staged_physical_range(rng, IO_BLOCK_SIZE, file_size) : rng`. Both use the same `io_utils::align_down/align_up`, the same `min` with the rounded EOF, and the same `end > start` guard.
  - `split_write_range` moved byte for byte. `uring::detail::split_write_range` now forwards to it.
  - The full uring suite passes.
- **`next_cut` core arithmetic.**
  - The absolute grid is `op - cursor % op` with a saturating add, so it cannot wrap.
  - `upper_bound(cursor)` correctly ignores boundaries at or before the cursor and duplicate boundaries.
  - A boundary beyond `slice_end` is harmless because of the `min`.
  - The logical range is never empty while `cursor < slice_end`.
  - Zero `op`, zero granule, and a reversed interval all throw.
- **Harness async discipline.**
  - The `{size, file_offset, buffer_offset, bytes_done}` arguments are pinned per lane and are not touched while an op is in flight.
  - The event is recorded *after* `cuFileReadAsync` returns success. `bytes_done` is read only after `cudaEventQuery == cudaSuccess`.
  - Streams are `cudaStreamNonBlocking` and events have timing disabled.
  - On a submit error the code throws before the record, and the lane destructor synchronizes, then deregisters the stream, before the args, the buffer registration, the arena, the handle, and the fd are released. Declaration order makes this correct.
  - The `pending_gate` is released before the head streams synchronize.
- **CMake.**
  - The harness compiles only `cufile_api.cpp` and links cudart, Threads and dl, with the project warning flags.
  - Header feature probes use `check_cxx_source_compiles` and never link libcufile.
  - The definitions ride the same INTERFACE target as `CUCASCADE_HAS_CUFILE`, so every library and test TU agrees on the `cufile_api` layout.
- **Topology script.**
  - Writes go only to the report, a `mktemp -d` temp dir, `--probe-dir` scratch files (4 MiB O_DIRECT dd, removed), and the `--gdsio` scratch file (removed by the EXIT trap).
  - There is no sudo, modprobe, sysctl, or write to `/sys`, `/proc` or `/etc`.
  - Every probe goes through `command -v` and `timeout -k`.
  - `gdsio` runs only under `--gdsio DIR`, and its commands are recorded.

---

## Findings

### BLOCKING

#### P1-R1: the default harness mode cannot measure on a compat host, and the documented smoke test fails

**Files:** `tools/gds/cufile_async_qd.cpp:385-388,625`; `tools/gds/README.md:31-34,64-67`.

**Problem.** With `--register 1` (the default), a single `cuFileBufRegister` covers the whole arena, and each lane passes `buffer_offset = index * stride`. On this host (libcufile 1.17 in compat mode, ext4):

- Every async read fails once two or more operations are in flight. The reads complete with `bytes_done = -1`.
- `CUFILE_LOGFILE_PATH` shows the cause: `cufio-px-async: execute cufile posix IO READ ... failed ... Input/output error` and `cufio_async: IO verification failed`.

I isolated the cause with a scratch copy of the harness built in `/tmp`:

| Variant | Result |
|---|---|
| QD 1, nonzero `buffer_offset` | OK |
| QD 2/8/32, one arena registration (default) | **100 % failures** |
| QD 2/8/32, one arena registration, stream registration off or flags 0/7/8 | **100 % failures** |
| QD 2/8/32, **one registration per slot**, `buffer_offset = 0` | OK, 0 mismatches |
| `--register 0`, any QD | OK |
| `--mode sync`, one arena registration | OK |

So concurrent `cuFileReadAsync` calls into disjoint offsets of one registered buffer fail in compat mode.

- The harness detects this correctly: it reports mismatches and exits with status 1.
- However, the default sweep and the README's own smoke procedure (`--qd 1,4 --register 1 --mode async`) both fail on the development host.
- This observation is recorded nowhere.

It matters beyond the harness. The Phase 2 `device_arena` (§4.4) registers slabs and serves many concurrent slots by offset. AM-7 relies on compat-mode libcufile to exercise the engine. Real-GDS behaviour is unknown.

**Fix.**
1. Change the option to `--register 0|arena|slot`, keeping `1` as an alias for `arena`. `slot` registers each `stride` slot separately and passes `base = slot`, `buffer_offset = 0`. Print the mode in the `result` row.
2. Change the README smoke test to commands that pass on a compat host (`--register slot` and `--register 0`). Document the observed compat failure for `arena` at QD ≥ 2, with the libcufile version.
3. Record the observation in `06-design-amendments.md` (a new amendment or an open question). Phase 2 must:
   - make the registration granularity (arena, slab or slot) a `device_arena` parameter;
   - cover it with a compat-libcufile test;
   - have Phase 7/G-gates measure arena vs per-slot registration on real GDS before slab registration becomes the default.

### SHOULD

#### P1-R2: `next_cut` does not enforce `op_bytes % granule == 0`

**File:** `include/cucascade/io/range_plan.hpp:77-97`.

**Problem.** When `granule > op_bytes`, or when `op_bytes` is not a granule multiple, two things go wrong:

- `physical.size` can exceed `op_bytes`. This breaks the §4.4 invariant that a piece of at most `S_eff` leases exactly one slot.
- Adjacent pieces get overlapping physical windows. For reads that means duplicate I/O. For Phase 5 writes it means two in-flight writes to the same granule.

When `op_bytes` *is* a granule multiple, every grid cell starts and ends on a granule, so `physical ⊆ grid cell` and `physical.size ≤ op_bytes`.

There is no test with `granule > op_bytes`. The "larger granule" test uses `op = 100000 > granule`.

**Fix.** Throw `std::invalid_argument` when `op_bytes % granule != 0`, or document it as a hard precondition with a debug `assert`. Add tests for `granule > op_bytes` (rejected) and for "physical ≤ op_bytes over a sweep of cursors".

#### P1-R3: the split-equivalence test is tautological

**File:** `test/io/test_range_plan.cpp:108-129`.

**Problem.** `legacy = uring::detail::split_write_range(...)` now forwards to `range_plan::split_write_range`, so the test compares the function with itself. Nothing pins `staged_physical_range` against the old uring formula either. Equivalence is true today by inspection, but nothing guards it.

**Fix.** Copy the HEAD algorithms (old `split_write_range` body and old `staged_physical_range(logical, file_size, use_odirect)`) into the test's anonymous namespace as oracles. Compare them exhaustively over small grids, for example:

- offsets 0..3·g
- sizes 0..3·g
- granules {0, 512, 4096, 65536}
- file sizes around the range end
- both `use_odirect` values

Also add saturation cases near `SIZE_MAX`.

#### P1-R4: the `next_cut` physical-window contract is under-specified at EOF and near `SIZE_MAX`

**File:** `include/cucascade/io/range_plan.hpp:62-97`.

**Problems.**
- With the EOF clamp, `physical` can end before `logical.end()`, as the test at `test_range_plan.cpp:93` shows. When the cursor is at or past `align_up(EOF)`, the result is an **empty physical with a non-empty logical**. Phase 3 callers need to know they must treat this as a short or EOF read, not as a submit.
- Near `SIZE_MAX`, `align_up` saturates, so `physical.end()` is `SIZE_MAX` and not granule-aligned. Offsets above `INT64_MAX` are not valid `off_t` values in any case.
- Neither case is documented or tested. The saturation test checks only `logical`.

**Fix.**
- Document on `piece_plan` that `physical` may stop short of `logical`, or be empty, only when EOF clamps it.
- Throw for `slice_end > std::numeric_limits<off_t>::max()`.
- Add tests:
  - cursor ≥ rounded EOF gives an empty physical;
  - a boundary list that lies entirely beyond `slice_end`;
  - the physical result at the saturation case.

#### P1-R5: partial parameter-group retention is untested

**Files:** `src/io/cufile/cufile_api.cpp:129-182`; `test/io/cufile/test_cufile_api_optional.cpp:312-338`.

**Problem.** The header promises that "parameter pointers remain individually usable when their group is partial". The per-symbol test checks the `has_*` flags, and nulling for groups 2 and 3, but never checks that the *other* parameter pointers stay non-null when one is missing.

**Fix.** In the missing-symbol test, when `missing.group == 4`, assert two things:
- the missing member is `nullptr`;
- every other declared parameter member is non-null.

This needs a small name-to-member table under the same `#ifdef`s.

#### P1-R6: cold-start submit latency pollutes the measurements

**File:** `tools/gds/cufile_async_qd.cpp:366-417`.

**Problem.** The first `cuFileReadAsync` per process or stream performs lazy initialization: the observed `submit_max` was about 13 ms, and `submits_over_100us ≥ 1` in every case. That sample counts in:
- `submit_p99` (which, for short cases, is drawn from a small number of samples);
- `submits_over_100us`, which is exactly the statistic that will calibrate `probe.submit_max_us` and demotion.

**Fix.**
- Issue one untimed warm-up op per lane, waited to completion, after stream registration and before the first case.
- Optionally add `--warmup-ops N`.
- Update README line 53.

#### P1-R7: duplicate or conflicting mount overrides are accepted with undefined precedence

**File:** `include/cucascade/io/cufile/config.hpp:165-188`.

**Problem.** Two entries can have the same `{match, value}` and different `state` values (for example `operator_qualified` and `known_compat`), and both validate. Overlapping prefixes (`/mnt` and `/mnt/disk_2`) also have no defined precedence. Routing must be deterministic, and AM-4 overrides bypass probe evidence.

**Fix.**
- Reject exact duplicate `{match, value}` pairs in `validate()`.
- Document the precedence now: for example, fsid > longest mount prefix > fs_type, with list order as the tiebreak.
- Add a test.

#### P1-R8: the harness is never built by default or in CI, so it will bit-rot

**File:** `CMakeLists.txt:653-668` (`EXCLUDE_FROM_ALL`).

**Problem.** The harness compiles `src/io/cufile/cufile_api.cpp` directly. That file and its header change again in Phases 2, 3 and 6. Nothing will notice when the harness stops compiling.

**Fix.** Do one of the following:
- add `cucascade_cufile_async_qd` as a dependency of `cucascade_io_tests` (it is cheap: one TU plus the loader);
- add it to the CI build step;
- add it to a `gds_tools` aggregate target that the io workflow builds.

Keep it out of `all` if you prefer.

### NIT

- **P1-R9 (plan deviation).** The harness lives at `tools/gds/cufile_async_qd.cpp`, while plan item 4 says `benchmark/cufile_async_qd/`. Update `07-implementation-plan.md`, or move the file.
- **P1-R10 (`src/io/cufile/cufile_reactor.cpp:1506,1535`).** The cuFile reactor still calls `uring::detail::split_write_range`. Plan item 2 says cufile should use the extracted helpers. Switch it to `range_plan::split_write_range` and mark the uring forwarder as deprecated.
- **P1-R11 (`config.hpp:62-123` vs §6.2).**
  - `poll` uses scalar caps (`io_cap_us`, `copy_cap_us`) instead of §6.2's backoff ladders (`io_backoff_us{10,25,50,100}`, `copy_backoff_us{2,5,10}`).
  - Durations are raw `unsigned` with unit suffixes rather than `std::chrono`.
  - These §6.2 fields are absent:
    - `retry`
    - `blocking_workers` / `recovery_workers`
    - `max_pending_slices_per_device`
    - `max_join_streams_per_device`
    - `registration_first_step`
    - `bar1_fraction` / `bar1_headroom`
    - `max_submitters_process`
    - `submitter_idle_retire`
    - `process_params`
    - `strict_parameters`
    - `mount_override.{nic,cpus}`

  `event_api` is correctly dropped (AM-1). The plan item lists only a subset, so this is acceptable. Record the deliberate deviations (the ladder → cap change especially) in the amendments so Phase 2 does not have to rediscover them.
- **P1-R12 (naming).**
  - `override` is used as an identifier (`config.hpp:165`, and several tests). It is legal but it is a contextual keyword; rename it to `entry`.
  - `auto optional = [&]...` in `cufile_api.cpp:103` reads like `std::optional`; rename it to `resolve_optional`.
- **P1-R13 (`cufile_api.hpp:62-96`).** The struct layout depends on CMake-supplied `CUCASCADE_CUFILE_HAS_*` macros, and the `#ifdef` members come *before* the `has_*` flags. The header is installed (`install(DIRECTORY include/ … "*.hpp")`). A TU that includes it without the definitions silently gets a different layout, which is an ODR violation.
  - Fix: move the conditional members to the end of the struct.
  - Better: have `cufile_api.cpp` export a `constexpr unsigned compiled_parameter_mask` and `static_assert` it in the test.
- **P1-R14 (`cmake/ConfigureCuFile.cmake:44-46`).**
  - Forcing the probes to re-run with `unset(... CACHE)` costs 11 try-compiles on every configure. That is acceptable, but a comment on the cost would help.
  - The `CUCASCADE_CUFILE_HAS_*` definitions are exported to install consumers, because they are not wrapped in `$<BUILD_INTERFACE:>`. They describe the build host's `cufile.h`. They are harmless unless a consumer includes the private loader header.
- **P1-R15 (harness polish).**
  - An unknown flag reports "Missing value for --bogus". Check the key set before consuming the value.
  - Polling is a pure busy-spin: about 5 M `cudaEventQuery` calls in 0.3 s, which can starve the compat worker threads. Add `--poll-us` to emulate the reaper's caps.
  - There is no `--stream-register 0|1` toggle, so the design's `stream_register` flag cannot be measured.
  - The `d2d_enqueue_1mib` loop does 2000 back-to-back enqueues on one stream and can hit the launch-queue limit. Synchronize every 256.
  - The license header is abbreviated: it lacks the Apache boilerplate used by the other sources.
- **P1-R16 (`config.hpp:166-178`).**
  - `mount_prefix` is not normalized: `/mnt/disk_2/`, `//mnt`, `/mnt/../x` and a trailing slash are all accepted as distinct values.
  - `fs_type` accepts whitespace.

  Normalize the prefix (strip the trailing `/`, reject `..`), or document that matching is done on normalized paths in Phase 2.
- **P1-R17 (`collect_topology.sh`).**
  - The default `OUT` is `./topology-<host>-<ts>.md`. When run from the repo root, that leaves an untracked file that is not gitignored. Default to `${TMPDIR:-/tmp}`, or add `topology-*.md` to `.gitignore`.
  - `ROOT_RE` includes `login|authentication`, which is broad. It only produces a note, but expect false "needs root" entries from WEKA and GPFS.
  - The gdsio section does not say that `-x 0` on a host without nvidia-fs measures compat mode, not GDS. Print the nvidia-fs state in the sweep header.
- **P1-R18 (`range_plan.hpp:70-72`).** The sorted-boundaries precondition is unchecked. Add `assert(std::is_sorted(...))` for debug builds.

---

## Verdict: **CHANGES-REQUIRED**

The library changes (items 1–3) are correct, behaviour-neutral by default, and well tested. They would be APPROVE-WITH-FIXES on their own. The topology script (item 5) is sound.

P1-R1 blocks closing Phase 1 for three reasons:
- the P0 harness's default mode, and its documented smoke test, fail on the development host;
- the root cause (compat libcufile rejects concurrent async reads into one registered buffer) is unrecorded;
- that root cause bears directly on the Phase 2 `device_arena` design.

Before Phase 1 closes:
- Fix R1.
- Fix R2 and R4, which Phase 3 depends on.
- Fix R3 and R8, which are cheap.

R5–R7 can land with Phase 2.

## Resolution

- **P1-R1 — fixed:** added `0|arena|slot` registration (`1` aliases arena), default slot, per-slot RAII registrations and CSV mode; README smoke uses slot/0; AM-9 requires Phase 2 granularity tests and Phase 7 real-GDS comparisons. Follow-up arena runs passed with and without warmup on devices 0/1; the original review failure remains recorded and unexplained, not claimed resolved in libcufile.
- **P1-R2 — fixed:** reject non-granule-multiple op sizes (including granule > op); cursor/geometry sweeps assert physical windows fit one op slot.
- **P1-R3 — fixed:** frozen pre-extraction split/staging algorithms replace the self-comparison; exhaustive small grids, realistic boundary neighbours, direct/buffered cases, EOF neighbours and SIZE_MAX cases pin equivalence.
- **P1-R4 — fixed:** documented short/empty EOF windows and legacy widening saturation; reject slice ends beyond off_t; test empty EOF windows, out-of-slice boundaries, the off_t edge and the saturated physical result.
- **P1-R5 — fixed:** guarded name-to-member table verifies the missing parameter pointer is null while every other declared parameter pointer remains non-null.
- **P1-R6 — fixed:** validated untimed I/O per lane before each case; `--warmup-ops` defaults to 1, with 0 available for cold-start comparison; README explains timing.
- **P1-R7 — fixed:** reject duplicate match/value selectors; document fsid > longest component-boundary mount prefix > fs_type, with earlier-list tie-breaking; add validation tests.
- **P1-R8 — fixed:** cucascade_io_tests depends on the standalone harness when both targets exist, keeping EXCLUDE_FROM_ALL but covering normal I/O builds.
- **P1-R9 — fixed:** implementation plan points at tools/gds/cufile_async_qd.cpp.
- **P1-R10 — fixed:** cuFile calls range_plan directly; mark the uring forwarder deprecated and migrate the uring split tests to the shared helper.
- **P1-R11 — fixed (documentation):** AM-10 records scalar polling caps versus ladders, duration units, and missing fields; resource/scheduler consumers are explicitly staged to Phases 2–3 and parameter/affinity fields to Phase 6.
- **P1-R12 — fixed:** rename override locals to entry and the loader lambda to resolve_optional.
- **P1-R13 — fixed (requested mitigation):** move conditional pointers to the struct tail; explicitly document that compatible feature definitions/headers are still required. A fully macro-independent installed loader ABI is deferred to Phase 6 rather than falsely claiming the stable prefix eliminates ODR risk.
- **P1-R14 — fixed (documentation):** explain the eleven configure-time try-compiles and why exported header-feature definitions are retained for matching private-loader layouts.
- **P1-R15 — fixed:** validate flags before consuming values; add --poll-us and --stream-register; drain D2D every 256 outside timing; restore the full Apache header.
- **P1-R16 — fixed:** require normalized absolute prefixes (root allowed, no trailing slash, repeated separators or dot components), reject filesystem-type whitespace, and test edge cases.
- **P1-R17 — fixed:** default reports to TMPDIR (/tmp fallback), narrow root-diagnostic matching, and print nvidia-fs state plus the compat-mode warning in the gdsio header.
- **P1-R18 — fixed:** debug-assert sorted geometry boundaries.

### Fix verification

- Both requested release targets build with `-j 20`; the I/O target also builds the harness dependency.
- Full I/O suite: **647 cases, 646 passed, 1 skipped; 3,011,102 assertions passed**, no failures (baseline: 610 cases, 609 passed, 1 skipped).
- Slot and unregistered async smoke: QD 1/4, 1/2 MiB, zero mismatches on devices 0 and 1. Default slot, stream-registration-off/poll-delay toggles, alias 1 at QD 1, sync arena and CUDA microbench also pass.
- Arena follow-up: QD 2/8/32, 1/2 MiB, devices 0/1, warmup 0/1 all passed; this does not invalidate the original review observation or qualify real GDS. Cold submit maximum reached about 15 ms; warmed default smoke submit maxima stayed below 100 µs.
- Topology collector: bash syntax clean, non-root report run returned 0 and included the no-nvidia-fs warning.
- Pre-commit: all applicable checks passed on all changed files via `pixi run python -m pre_commit`, because the requested `pixi run pre-commit` invocation hits a stale `/mnt/workspace/...` interpreter shebang in the installed launcher; no repository workaround or environment-file edits.
