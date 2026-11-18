# Phase 7b review: production arena slot classes and cuFile NIC steering

**Scope:** `io/cufile-async-engine` at HEAD `c137612` plus the working tree:
- `git diff HEAD`: `capacity.hpp`, `config.hpp`, `device_arena.{hpp,cpp}`, `device_engine.{hpp,cpp}`, `runtime.{hpp,cpp}`, `parameters.cpp`, `cufile_engine_bench.cpp`, `qualify.sh`, `qualify.example.conf`, the four C++ test files, `docs/io/cufile-reactor.md`, `06-design-amendments.md` (AM-19) and `tools/gds/README.md`
- the untracked files `tools/gds/{make_cufile_json.py, nic_counters.py}` and `tools/gds/tests/{test_make_cufile_json.py, test_nic_counters.py}`

**Gaps under review:**
1. `async_engine_config::arena_slot_classes` (default `{4}`) and how it is wired into the automatic bootstrap (`device_engine::for_device`).
2. NIC steering: `make_cufile_json.py`, `process_params.cufile_json_path` (`setenv(CUFILE_ENV_PATH_JSON)` before driver open), and the docs.

**Severity scale** (same as Phase 7a):
- **BLOCKING** means fix before Phase 7b is signed off or the hardware handoff.
- **SHOULD** means fix before the affected feature is used for qualification evidence, or before the G-8 default flip.
- **NIT** is cosmetic, documentation or a robustness improvement.

**Validation I ran** (sequentially; no repository code was modified):
- `cmake --build build/release --target cucascade_tests cucascade_cufile_engine_bench cucascade_io_tests`: clean.
- `build/release/test/cucascade_io_tests "[cufile]"`: **377 cases / 38,261 assertions passed**.
- `python3 -m unittest discover -s tools/gds/tests`: 49 passed.
- Scratch programs, all outside the repo:
  - `/tmp/p7b/fit.cpp` drives `rebalance()`/`fit()` from `capacity.hpp` directly.
  - `/tmp/p7b/freeze.cpp` links `libcucascade_io` and reproduces the automatic-probe ordering.
  - A fake-sysfs script runs the generator against a two-level PCIe switch.
  - The generator's `uncomment()` was run on the stock libcufile 1.14 `cufile.json`.

---

## 1. Summary

**The arena half is mostly sound.**
- One shared translation: `device_arena_options::from_config` is used by both `device_engine::for_device` and the bench, so the config is the single source of truth.
- Validation of `arena_slot_classes` is strict: the list must be nonempty and contain unique members of {1,2,4,8}.
- `fit()` now charges the physical backing slot against byte capacity.
- The registration and BAR1 arithmetic is right:
  - AM-9 slot registration gives 16/S registrations per 16-MiB slab.
  - Registered bytes stay the sum of the actual registration lengths.
  - The unknown-BAR1 64-MiB first step gives 16 registrations with `{4}`.
  - A target remainder smaller than a slot is skipped without failing.
- The new gauges (`registrations`, registered and unregistered class masks) are useful and are tested.

Two capacity-model gaps remain:
- **Mixed classes:** `{4,8}`, which the docs recommend for GPFS, over-fits QD against bytes the chosen class does not own.
- **Demand vs fit:** `rebalance()` computes slab demand from the clamped *preference*, not from the fitted piece.

**The NIC-steering half has one blocking ordering defect.**
- `make_disk_io_context(automatic)` and `default_disk_io_context()` call `gds_available()` first, which runs `runtime::ensure_initialized()` with an **empty** request.
- So `process_params.cufile_json_path` (and every other process parameter) supplied through `disk_io_context_options.io.cufile` is silently frozen out on the product's default bootstrap. Strict mode throws instead.
- The new docs describe this field as the runtime alternative to an export without mentioning this.

In the generator:
- PCIe ranking cannot tell PIX from PXB. I reproduced it selecting a NIC on the sibling leaf switch.
- It emits IPv6 addresses into a list that NVIDIA documents as IPv4.
- Its default `--policy` overwrites the base JSON's policy, despite the "preserves the base" claim.

The filesystem caveats are correctly hedged; nothing claims GPFS/WEKA/VAST client RDMA is controlled. The scope of the `properties`-level list is still under-explained (P7b-R7).

---

## 2. Findings

### P7b-R1 — BLOCKING — `cufile_json_path` is silently ignored on the automatic DISK bootstrap

**Problem.** `make_disk_io_context()` → `resolve_backend(automatic)` → `detail::gds_is_available()` → `io::cufile::gds_available()` → `compute_platform()` → `runtime::instance().ensure_initialized()`. This is the no-argument overload, so it freezes `process_cufile_parameters{}`. Only afterwards does `build_context()` construct `cufile_reactor`, whose constructor calls `ensure_initialized(process_params, strict)` with the user's request.

At that point `_initialized` is already true, so:
- `cufile_json_path` is never examined;
- `setenv` never runs;
- the report's `parameters` vector does not contain the entry at all.

In non-strict mode the only signal is the generic WARN log "differing request ignored". `default_disk_io_context()` (the process-shared default) follows the same path.

Reproduced with `/tmp/p7b/freeze.cpp`, which calls `gds_available()`, then `ensure_initialized({cufile_json_path="/tmp/p7b/gpu0.json"}, false)`:
```text
gds=0 params_reported=0 differ=1 env=(unset)
strict throws: cuFile process parameters differ from the frozen initialization request
```

This is partly a pre-existing Phase 6 property of all `process_params`. But Phase 7b presents `cufile_json_path` as "the runtime alternative to an external export". `docs/io/cufile-reactor.md` §"Selecting NICs"/process-parameters says that "the driver transaction sets `CUFILE_ENV_PATH_JSON` before its first open" and that a late request reports "**unapplied: driver already open**". On the default bootstrap neither statement holds:
- no per-entry status is produced;
- the existing "If it runs first (… legacy file open, arena or lane bootstrap)" list does not mention the automatic-backend probe.

NIC steering configured this way silently does nothing on the most common construction path.

**Fix (any one, plus a test):**
1. **Preferred:** before `resolve_backend()` probes GDS, initialize the runtime with the caller's `options.io.cufile.async_engine.{process_params,strict_parameters}` when they are non-default. Do this through a cufile-layer helper (e.g. `io::cufile::prime_runtime(params, strict)`) so `src/cudf/` only gains a single call.
2. **Or** make `gds_available()`/`compute_platform()` not initialize the runtime. Probe the library only and defer driver open to the first reactor. This is a larger change.
3. **Minimum if code is out of scope:** document in the process-parameters section, the NIC section and AM-19 that `make_disk_io_context(automatic)`, `default_disk_io_context()` and any `gds_available()` call freeze defaults. Say that callers must either export `CUFILE_ENV_PATH_JSON` before launch, or call `runtime::instance().ensure_initialized(params, strict)` before building any DISK context. Also correct the "reports unapplied: driver already open" wording: an earlier *runtime initialization* yields `parameters_differ` with no entry, not `unapplied`.

**Test:** in `test_runtime_params.cpp`, a no-argument `ensure_initialized()` followed by a `cufile_json_path` request must show `parameters_differ`, the environment unchanged, and a strict throw. If option 1 is chosen, add an automatic-backend test asserting the env was set before the fake open.

### P7b-R2 — SHOULD — Mixed slot classes (`{4,8}`) over-fit QD; docs recommend this for GPFS

**Problem.**
- `from_config({4,8})` provisions slabs round-robin (`mib4, mib8, mib4, …`). With `registration=slot` a registered slab cannot be reformatted (`can_reformat` is false), so only ~half the registered bytes are 8-MiB slots.
- `rebalance()`/`fit()` receive `arena_bytes = registered_bytes` (all classes) and size QD as if every granted slab could hold the chosen class.
- `set_quota(..., mib8)` then leaves the extra slabs **pending**. 8-MiB pieces cannot lease 4-MiB slots, so the overstated lanes surface only as `slot_waits`.
- `mounts[].effective.qd` therefore overstates what the arena can serve.

Measured with `rebalance()` at 64 MiB and `{4,8}` (the new engine test's geometry), GPFS 8 MiB at QD 4 yields `slabs=3 qd=4 op=8MiB lanes=5`. That is 5 × 8 MiB = 40 MiB of 8-MiB slots, but only 2 slabs × 2 = 4 slots (32 MiB) exist. At the auto 1-GiB arena, QD 64 demands 40 slabs of 8 MiB while only 32 exist. If the GPFS mount is alone, the 4-MiB half of the arena is unusable.

The engine test `which == 2` checks only `effective.op_bytes == 8 MiB` and a single slice, so it cannot see this.

**Fix.**
- Pass per-class registered capacity (slab counts by class) into `rebalance()`. Fit each mount against the bytes of the backing class it will lease, or clamp `effective` to the slabs `set_quota` actually assigned (`quota_report().assigned`).
- Document that `{4,8}` splits capacity per class. Recommend `{8}` for GPFS-only processes and `{4,8}` only for mixed GPFS + local deployments.
- Extend the `{4,8}` engine test to assert `effective.lanes × 8 MiB ≤ registered 8-MiB slot bytes`.

### P7b-R3 — SHOULD — `rebalance()` demand charges the preference's backing, not the fitted piece's

**Problem.** Demand uses `size = clamp(preferred_op_bytes)` and `backing_slot_bytes(size, mask)`. If no class backs the preference it falls back to `size`, but `fit()` then chooses a smaller piece. Measured cases:

| Profile | Mask | Demand (slabs) | Actual need | Over-charge |
|---|---|---|---|---|
| GPFS 8 MiB, QD 64 | `{4}`, 1 GiB | 40 | 80 × 4 MiB = 20 | 2× |
| WEKA 2 MiB | `{1,4}` | backing(2 MiB) = 4 MiB per lane | `fit` picks exact 1 MiB | 4× |

The excess slabs are owned-but-idle (borrowable only while `live == 0`). Under contention, water-filling then gives the over-demanding mount a larger share than it can use. The docs' sentence "Fitting charges the physical backing slot" is true for `fit()` but not for the demand side of the same function.

**Fix.** Compute the demand from the piece that `fit(profile, SIZE_MAX, lane_cap, mask)` would choose: `need × backing_slot_bytes(fitted.op_bytes, mask)`. Add `test_capacity` cases for GPFS-8/`{4}` and 2 MiB/`{1,4}` demand, and direct `backing_slot_bytes` tests.

### P7b-R4 — SHOULD — PCIe ranking treats any shared bridge as best (PIX ≡ PXB)

**Problem.** `rank()` returns 0 if *any* BDF-named ancestor is shared, so a NIC under the same leaf switch ties with a NIC under a sibling leaf of the same upstream switch. A fake-sysfs reproduction:
- Topology: root port `a0:01.0` → switch `a1:00.0` → leafs `a2:00.0`/`a2:01.0`.
- GPU `a3:00.0` and `mlx5_0` sit under `a2:00.0`; `mlx5_1` sits under `a2:01.0`.
- Result: both are marked "shared upstream PCIe bridge" and the output is `['10.0.0.1', '10.0.0.2']`. `nvidia-smi topo` would report PIX for `mlx5_0` and PXB for `mlx5_1`.

On DGX-class two-level switch trees this over-selects a NIC per GPU. The doc promise "shared upstream PCIe bridge first … all equal-best NICs are included" is satisfied literally but not in intent.

The switch detection itself is otherwise correct:
- The `pciDDDD:BB` host root is excluded.
- Endpoints are excluded because only `parents` are scanned.
- Root-port-only sharing implies a switch.
- Flat VM topologies fall through to NUMA.

**Fix.**
- Score by the depth of the deepest common BDF ancestor, or equivalently the hop count to it, and treat only equal depths as ties. This maps to PIX < PXB < (NUMA) < SYS.
- Make the `--nvidia-smi-topo` fallback rank PIX ahead of PXB as well.
- Add a two-level-switch fixture test.

### P7b-R5 — SHOULD — IPv6 addresses are emitted into `rdma_dev_addr_list`

**Problem.** `address()` keeps global/ULA IPv6, and `test_same_numa_tie_selects_both_ipv4_and_ipv6` asserts `["10.0.0.3", "2001:db8::2"]`. NVIDIA's GDS configuration documentation describes `rdma_dev_addr_list` as a list of **IPv4** addresses, and every stock `cufile.json` example (the 1.14 file on this host included) is IPv4. **[verify]** whether libcufile parses IPv6 here. If it doesn't, the generated file may be rejected, or entries may be silently dropped. The IPv4-mapped GID → IPv4 conversion is correct, and link-local/loopback/multicast/unspecified are correctly dropped.

**Fix.** Emit IPv4 only by default. Print IPv6 candidates as diagnostics, and add `--allow-ipv6` with a **[verify]** note. Update the test to assert IPv6 is excluded by default.

### P7b-R6 — SHOULD — The base JSON's policy is overwritten, and policy values are not validated

**Problem.** `--policy` defaults to `"RoundRobin"`, and `main()` always writes `properties.rdma_load_balancing_policy`. A base that an administrator set to `RoundRobinMaxMin` is silently reverted, which contradicts "preserves all unrelated JSON keys" / "preserving the base JSON".

Any nonempty string is accepted. The stock JSON documents `RoundRobin` and `RoundRobinMaxMin`, yet the multi-GPU test uses `"MaxMinFit"`, which is not a documented value.

Other key preservation is fine. All top-level and `properties` keys survive `uncomment()` + `json.dumps` on the stock 1.14 file; only comments are lost, which the docs disclose.

**Fix.**
- Default `--policy` to unset: keep the base value, and set `RoundRobin` only if the key is absent.
- Warn (or reject without `--force-policy`) for values outside the documented set, with a **[verify]** note.
- Change the test to `RoundRobinMaxMin`, and add a test where a base policy survives.

### P7b-R7 — SHOULD — The scope of the generated list is under-documented

**Problem.** There are no false claims: README, cufile-reactor.md, AM-19 and the tool output all say the JSON does **not** control GPFS `verbsPorts`, WEKA `net=` or VAST `localports=`, and that those must be checked with the client administrator. But the stock `cufile.json` matters here:
- It documents `properties.rdma_dev_addr_list` as the "client-side rdma addr list for **user-space** file-systems".
- Kernel clients (`fs.nfs`, `fs.lustre`, `fs.beegfs`) have their own `rdma_dev_addr_list` / `mount_table` lists, used "when dynamic routing is enabled".

The tool sets only the `properties`-level list. It never warns that a base file with `fs.*.rdma_dev_addr_list`/`mount_table` entries, or `rdma_dynamic_routing: true`, will steer differently. The phrase "best addressable NIC set for cuFile-managed RDMA" leaves readers to assume NFS/Lustre traffic is steered.

Also, "It reads sysfs, RoCE/IPoIB GIDs" is inaccurate. IPoIB GIDs carry no ndev association and are never used. IPoIB addresses come from `ip -j addr` on the netdev.

**Fix.**
- In the NIC section, state which consumers read the `properties` list (user-space clients, per the stock JSON) versus the `fs.<type>` lists (kernel clients, dynamic routing).
- Have the tool print a warning when the base contains fs-level lists, a `mount_table`, or enabled dynamic routing.
- Fix the GID wording.

### P7b-R8 — SHOULD — Test coverage gaps on the new contracts

**Problem.**
- (a) Nothing asserts that the `cufile_json_path` setenv precedes the other `cuFileSetParameter*` calls. The order is correct today only because `requested_parameters()` adds it first. If libcufile loads JSON on the first setter, reordering would silently break the override semantics. Have the fake setter record `getenv("CUFILE_ENV_PATH_JSON")` and combine the path with `logging_level`.
- (b) There is no test for the R1 freeze path.
- (c) The production wiring (`device_engine::for_device` → `from_config` → `warm_up`) is untested. The new engine test calls `from_config` itself, so a regression that drops it from `for_device` would pass. Extract a small `bootstrap_arena(arena, config)` helper used by `for_device` and test it.
- (d) `test_capacity` changed one assertion only. It needs direct tests of `backing_slot_bytes`, the "keep the larger piece with the same backing" rule, rebalance demand (R3), and mixed-class capacity (R2).
- (e) Generator tests need PIX/PXB (R4), IPv4-only (R5), base-policy preservation (R6), and fs-level-list warning (R7) cases.

The existing runtime test is otherwise good. Its five scenarios check the environment value at the fake `open`, the conflict non-overwrite, the late open, the external open, and the strict throws, and it restores the environment.

### P7b-R9 — NIT — setenv side effects and path checks

**Problem.**
- When the driver is detected as externally opened (`CU_FILE_DRIVER_ALREADY_OPEN`), the entry becomes `unapplied`, but the variable this call just set stays in the process environment. It is inherited by child processes (e.g. `gdsio`/`gdscheck` launched by tooling).
- When libcufile is absent (`api == nullptr`), the variable is still set and reported `unverified`.
- `getenv` then `setenv(…, 0)` is a TOCTOU window: a racing writer wins, but the report still says "selected".
- The path is not checked for being absolute or readable, so a typo yields `unverified` and libcufile's own fallback behaviour.

The documented thread-safety caveat (setenv vs concurrent `getenv` is unsafe on older glibc) is adequate. CUDA/RMM threads may already exist by bootstrap; the docs' advice to prefer a shell export covers that.

**Fix.**
- `unsetenv` on the external-open path when this call created the variable.
- Report `unsupported` when the library is unavailable.
- Re-read after `setenv` and compare.
- Require an absolute, `access(R_OK)`-readable path. Otherwise mark the entry `unapplied: "json not readable"`, which is a strict-mode error.
- Optionally set the variable before `cufile_api::get()` (dlopen) as well, in case library constructors read it. **[verify]**

### P7b-R10 — NIT — The bench cannot reproduce the production geometry for 1/2-MiB operations

**Problem.** The bench maps `--op-mib N` to `arena_slot_classes = {N}`. The translation is shared (good), but `--op-mib 1` benchmarks `{1}` while production defaults to `{4}`, which charges 4 MiB per 1-MiB piece. The CSV does not record the classes; only stderr has the mask.

**Fix.** Add `--arena-classes LIST`, defaulting to the config default or `{op}` for legacy compatibility, and emit `arena_slot_classes` / `arena_registered_class_mask` / `registrations` CSV columns.

### P7b-R11 — NIT — First warm-up wins silently

**Problem.** `device_arena::warm_up` returns the cached report when the arena is already warmed. A second context whose `arena_slot_classes` differ gets the first geometry, and the only trace is the INFO mask log.

**Fix.** Keep the first options in state and log a WARN, or set a `differs` flag in the report, when a later request's options differ.

### P7b-R12 — NIT — The cost of the `{4}` default for network filesystem profiles is undocumented

**Problem.** `{4}` is the right default for the project's local NVMe target: ext4/xfs move from 1-MiB to 4-MiB pieces, and the registration count drops 4×. The cost lands on the 1/2-MiB profiles (nfs*, gpfs default, lustre, wekafs, beegfs), where every piece occupies a 4-MiB slot:
- With unknown BAR1 (64-MiB registration cap), NFS QD 64 fits **QD 12** under `{4}`, versus **QD 51** under `{1}` (measured).
- The zero-grant sync fallback uses a 4-MiB piece for 1-MiB-preferring mounts.

The docs mention "charges the physical backing slot" but give no numbers or guidance.

**Fix.** Add one sentence with the 4×/2× byte cost and the unknown-BAR1 example, and recommend `{1}` (or `{2}`) for network-FS-only processes.

### P7b-R13 — NIT — Tooling polish

**Problem / fix:**
- **`nic_counters.py`:** the note says "Process-global"; it should say **host-global**.
- **`make_cufile_json.py`:**
  - Writes `--out` non-atomically, and permits `--out == --base`. Use a temp file + `os.replace`, and refuse to overwrite the base.
  - Skips mlx5 SF/auxiliary RDMA devices silently (`device` resolves to a non-BDF name). Print them as skipped.
  - The `topo_links()` header match (`cols[0] == "GPU0"`) breaks if the saved `nvidia-smi topo -m` output includes ANSI underline codes. Strip `\x1b[...m` first.

---

## 3. Checklist outcome

| Check | Result |
|---|---|
| Capacity fit with larger per-slot classes | `fit()` is correct for single-class masks and charges backing bytes. Mixed classes over-fit (R2); demand is inconsistent with the fitted piece (R3). |
| Registration / BAR1 math | Correct: per-slot registrations, byte-based BAR1/pin budgets, 64-MiB first step = 16 × 4 MiB, sub-slot remainder skipped. Gauges are accurate. |
| Single source of truth (bench ↔ config) | Yes for the translation (`from_config`). The bench's class choice differs from the production default for non-4-MiB operations (R10). |
| Default choice `{4}` | Justified for local NVMe. Network-FS cost undocumented (R12). |
| setenv safety / ordering | Correct relative to driver open and the other setters inside `initialize()`. Strict/conflict handling is correct. **Broken on the automatic bootstrap (R1)**; residual side effects (R9). |
| PCIe proximity ranking | Bridge detection is right; depth is not ranked (R4). |
| IPv4/IPv6 mapping | IPv4-mapped GIDs are converted correctly and link-local is filtered. IPv6 emission is questionable (R5). |
| Base JSON preservation | Keys preserved except the policy, which is overwritten (R6). |
| No false claims about filesystem-managed RDMA | No false claims; scope under-documented (R7). |
| Test quality | Good runtime and engine scenarios; gaps listed in R8. |
| Docs accuracy | Mostly accurate. Wrong or incomplete on the automatic freeze (R1), `{4,8}` capacity (R2), IPoIB GIDs (R7) and policy preservation (R6). |

---

## 4. Verdict

**CHANGES REQUESTED.**

- **BLOCKING:** P7b-R1.
- **SHOULD:** P7b-R2, P7b-R3, P7b-R4, P7b-R5, P7b-R6, P7b-R7, P7b-R8.
- **NIT:** P7b-R9, P7b-R10, P7b-R11, P7b-R12, P7b-R13.

The arena geometry work can land once R2/R3 are addressed or explicitly documented as limits. NIC steering through `process_params.cufile_json_path` must not be advertised as a runtime alternative until R1 is fixed or documented with the required pre-initialization step. The shell-export workflow (`make_cufile_json.py` → `export CUFILE_ENV_PATH_JSON=…` before launch) is unaffected by R1 and is usable now, subject to R4–R6.

## Resolution

Applied in the working tree without commits, delegation, concurrent builds, or
changes under `src/cudf/`.

| Finding | Resolution |
|---|---|
| P7b-R1 | Used the review's documentation/pre-initialization option because `src/cudf/` is out of scope. Reactor docs (process parameters and NIC selection), tooling README and AM-19 now require an external export or explicit `runtime::instance().ensure_initialized(params, strict)` **before** automatic DISK setup, the process-shared default, or any `gds_available()` probe. Configuring only the automatic context remains too late. Corrected frozen-runtime diagnostics: `parameters_differ`, original report, no new per-field entry, strict throw. Added a regression test matching the automatic probe's no-argument initialization sequence. |
| P7b-R2 | Added actual matching-class, non-draining registered bytes to quota reports (including partial registrations). The engine clamps effective QD to those assigned bytes, never pending/wrong-class slab grants. Added mixed `{4,8}` arena and engine checks: the 8-MiB half owns 32 MiB, fits QD 3 / 4 lanes, not QD 4 / 5 lanes. Docs recommend `{8}` for GPFS-only and explain capacity splitting for mixed deployments. |
| P7b-R3 | Rebalance demand now uses the piece selected by unlimited-byte `fit()` and its physical backing slot. Tests assert GPFS-8/`{4}` needs 20 slabs and 2-MiB/`{1,4}` needs 5 slabs at QD 64. |
| P7b-R4 | Rank shared PCIe ancestors by hop distance rather than a boolean; captured-topology fallback ranks PIX before PXB. Two-level-switch and fallback fixtures pass. |
| P7b-R5 | Emit IPv4 only by default, diagnose excluded IPv6, and provide an explicitly unverified `--allow-ipv6` opt-in. Tests cover both modes. |
| P7b-R6 | Preserve base policy unless explicitly overridden; default RoundRobin only when absent. Explicit choices are RoundRobin/RoundRobinMaxMin; undocumented base values warn. Removed the invalid test policy and added preservation/override/rejection coverage. |
| P7b-R7 | Document user-space consumers of the properties-level list versus kernel NFS/Lustre/BeeGFS filesystem lists/mount tables. Warn about filesystem lists, mount tables and enabled dynamic routing without modifying them. Corrected RoCE GID versus IPoIB netdev address discovery wording. |
| P7b-R8 | Added setter-time environment observation, late-default-freeze regression, shared production `device_arena::bootstrap(config)` used by `for_device` and tests, backing-slot/larger-piece/demand/mixed-capacity tests, partial/wrong-class/draining quota checks, and the requested generator fixtures. |
| P7b-R9 | Reject relative/unreadable JSON paths as unapplied; absent libcufile is unsupported without environment mutation. Re-read after non-overwriting setenv; remove a value created here if driver open reports an external owner. Added invalid-path/missing-library tests and external-open cleanup assertion. Library loading still precedes setenv; constructor-time JSON consumption is explicitly unverified in the docs. |
| P7b-R10 | Added independent `--arena-classes LIST`, default `{4}` like production; CSV records requested classes, actual registered class mask, and registration count. Duplicate/invalid classes are rejected before file creation. |
| P7b-R11 | Later warm-ups with different options WARN and retain the first geometry. Added a bootstrap test retaining the initial mixed mask. |
| P7b-R12 | Documented 4×/2× backing cost, unknown-BAR1 NFS QD 12 versus 51, sync fallback cost, and `{1}`/`{2}` guidance for network-FS-only processes. |
| P7b-R13 | Counters say host-global. Generator writes via a temporary file plus atomic replacement, refuses base overwrite, reports auxiliary/non-PCI RDMA skips, and strips ANSI formatting from saved topology. Added tests including failed-replacement cleanup/output preservation. |

### Validation after fixes

All requested commands ran sequentially:

- `pixi run cmake --build build/release --target cucascade_io_tests cucascade_cufile_async_qd cucascade_cufile_engine_bench -- -j 20`: passed.
- `./build/release/test/cucascade_io_tests`: **942 cases: 941 passed, 1 skipped; 3,039,792 assertions passed**. The skip is the negative kTLS test because this host has the kernel TLS module loaded. cuFile reports compatibility mode on this host; these results are not direct-GDS qualification.
- `./build/release/test/cucascade_io_tests '[engine]' --order rand`, five runs: **133 cases / 6,975 assertions passed each**. Seeds: 1265668472, 1340056596, 2000414496, 2425093861, 915819266.
- `pixi run python -m unittest discover tools/gds/tests`: **55 tests passed**.
- `pixi run python -m pre_commit run --files $(git ls-files -m -o --exclude-standard)`: passed on all changed/untracked files after clang-format's initial corrections. Module invocation avoids the environment's relocated pre-commit script shebang.
- `git diff --check`: passed.
- Benchmark CLI smoke: help exposes `--arena-classes`; duplicate `{4,4}` fails validation before creating a file.

Logs: `/tmp/p7b-fix-build.log`, `/tmp/p7b-fix-full.log`,
`/tmp/p7b-fix-engine-rand-{1..5}.log`, `/tmp/p7b-fix-python.log`,
`/tmp/p7b-fix-precommit-final.log`.
