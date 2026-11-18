# Checkpoint: step 6, implementation

- Status: phase 1 RUNNING (graph `phase-1-foundations`)
- Branch: `io/cufile-async-engine`
  - `600a3583` design docs
  - `7641cf5` amendments and plan
- The user approved proceeding (Q&A closed). AGENTS.md's GSD slash-command workflow is unavailable in this harness. The user asked to proceed, so work happens directly on the branch, with a review gate per phase.
- Environment facts:
  - `pixi run build` fails on a pre-existing cudf-nightly API break: `src/cudf/representation_converter_builtins.cpp:736`, the `null_mask_buffer` type.
  - Work builds the `cucascade_io_tests` target (about 17 s incremental).
  - The old `build/release` was configured from `/mnt/workspace`. It was moved to `build/release.stale-mnt` and reconfigured.
  - Baseline: `cucascade_io_tests` has 610 cases: 609 passed, 1 skipped (91 s).
- Models:

  | Role | Model |
  |---|---|
  | Implementer | `openai-codex/gpt-6.1-sol` @ high |
  | Reviewer | `anthropic/claude-opus-5-5` @ high |
  | Mechanical agents | `anthropic/claude-sonnet-5-5` |
- Phase log:
  - **Phase 1 (foundations): DONE**, commit `32767a1`.
    - Review: CHANGES-REQUIRED (1 blocking, 17 SHOULD/NIT findings), all resolved; see `reviews/phase-1.md`.
    - Tests: 647 cases, 646 passed, 1 skipped (lead verified independently).
    - Pre-commit clean.
    - New findings:
      - AM-9: concurrent async reads into one shared arena registration failed in compat mode during review, then passed on rerun. The cause is unexplained, so registration granularity becomes a qualification dimension.
      - AM-10: compat-mode async submission does not block on libcufile 1.17.
    - Note: the `pre-commit` launcher has a stale `/mnt` shebang. Use `pixi run python -m pre_commit`.
  - **Phase 2 (resources and policy): DONE**, commit `5f89013` (about 10.7k lines).
    - Review: CHANGES-REQUIRED (22 findings). 21 fixed; P2-R19 (blocking cleanup pool) deferred to Phase 3.
    - Tests: 784 cases, 783 passed, 1 skipped (lead verified; zero build warnings). Pre-commit clean.
    - Key finding (P2-R1): on compat libcufile 1.17, a SHARED registration (whole arena or per slab) silently corrupts data under concurrent async reads.
      - Example: 350 of 361 reads had wrong bytes, while `bytes_done` was correct.
      - Per-slot registration is clean.
      - Default granularity is `slot`. Shared layouts need an explicit unsafe opt-in.
      - The harness now verifies content by default.
    - AM-11 records the fit-order correction, evidence-source honesty (nvidia-fs parsing deferred to Phase 7), and immutable slot geometry.
    - Process note: agents' own helper sub-agents ran concurrent builds and corrupted the ninja metadata (later repaired). Phase 3+ prompts forbid delegation.
  - **Phase 3 (async engine reads): DONE**, commit `bf775d1`.
    - Review: CHANGES-REQUIRED (26 findings).
      - No memory-safety, double-settlement or stream-ordering defects.
      - Fixed: R1, R3–R13, R16–R17, R19–R26.
      - Partly fixed; remainder deferred: R2 to Phase 7 (CPU qualification); R14, R15, R18 to Phase 6.
    - Tests: 841 cases, 840 passed, 1 skipped. Zero build warnings.
    - Lead race check: 57 `[engine]` cases in 2 parallel instances, random order and seeds, 10 rounds: 20 of 20 runs passed.
    - AM-12: bounded admission of fence-blocked reads.
    - AM-13: tracked follow-ups.
    - The `cudf/datasource.hpp` change is doc-only (verified).
  - **Phase 5 (async writes): DONE** (commit follows).
    - Review: CHANGES-REQUIRED (17 findings). Integrity core sound: no path replays a write of unknown progress.
      - Fixed: R1-R10, R12-R17.
      - Declined: R11, keeping the conservative stuck-write failure policy (recorded in AM-16).
    - Tests: 893 cases, 892 passed, 1 skipped. Zero warnings.
    - Lead race loop: 12 of 12 passed.
    - `types.hpp` gained a typed `backend_owner` ticket on slices, so inode write sequencing survives slice splitting.
  - **Phase 6 (polling, hardening, params, affinity, profiles, docs): DONE** (commit follows).
    - Review: CHANGES REQUESTED (1 BLOCKING, 6 SHOULD, plus NITs). R1-R16 resolved.
    - Tests: 929 cases, 928 passed, 1 skipped. Zero warnings.
    - Lead race loop: 10 of 10 passed.
    - AM-15 polling, compat B-3: `queries_per_op` dropped from 400 to 19.6 (ceiling 70, aspiration 8) at 6.57 GiB/s, mean QD 28.3 (was 24.1).
    - Added:
      - process cuFile parameters (typed allowlist, pre-open, strict mode)
      - opt-in affinity
      - filesystem profiles and `docs/io/cufile-profiles`
      - macro-independent ABI
      - `docs/io/cufile-reactor.md`
  - Phase 7a (non-GDS qualification: runner/analysis tooling, B-10 compat regression, soak, sanitizers, B-1 here): after Phase 6
  - **Phase 4 (coalescing + engine benchmark): DONE**, commit `d11c9de`.
    - Review: no BLOCKING findings; P4-R1 to R18 all fixed.
    - Tests: 860 cases, 859 passed, 1 skipped. Zero warnings.
    - Lead race loop: 12 of 12 passed.
    - AM-14 records the coalescing, publication and routing contracts.
    - Lead smoke on the compat dev box (NOT GDS: ext4, cuFile compat, page cache dropped, QD 32, 4 MiB ops, 256 MiB arena):

      | Workload | Legacy | Async engine |
      |---|---|---|
      | Sustained (B-3) | 3.99 GiB/s | 6.56 GiB/s (+64%), mean QD 24.1 |
      | Single 1 GiB (B-5) | 6.38 GiB/s | 6.63 GiB/s (device-bound) |
    - **Tracked perf issue (Phase 6):** `queries_per_op` = 400, against an NFR-3 target of 8 and a ceiling of 70.
      - With a completion p50 of 8.4 ms, the poll cap of 100 us alone accounts for about 80 queries per op. The rest suggests wake-driven re-polling of lane heads.
      - The poll cadence must scale with the observed completion latency.
