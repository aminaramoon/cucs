# Checkpoint: step 7, hardware qualification handoff

- Status: HANDED OFF. Waiting for a real GDS host.
- Runbook: `docs/io/gds-design/08-phase-7-handoff.md`.
- Non-GDS parts done on the compat dev host:
  - Phase 7a: tooling, B-10 legacy arm, soak diagnostics, sanitizers, measurements on the compat host.
  - Phase 7b: production slot classes, NIC steering.
  - `qualify.sh` now passes `ARENA_CLASSES` (`--arena-classes`) to every engine run (lead fix, verified with a dry run).
- Gates needing GDS: G-1, G-2, G-3, G-5 (full), G-7 (1 h), G-8; plus G-4 on a T4.
- G-6 is blocked by TSan CUDA modelling.
- Open blockers for production enablement:
  - AM-1 caller migration in the cudf layer (`src/cudf/disk_io.cpp`, `check_transferred`, benchmarks). Blocked by the pre-existing cudf nightly build break.
  - An nvidia-fs stats parser (AM-11), once real samples are returned.
