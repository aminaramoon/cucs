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
  - Phase 3 (async engine reads): running
