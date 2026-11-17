# Checkpoint: step 1, GDS workflow patterns doc

- Status: DONE
- Output: `docs/io/gds-design/01-gds-workflow-patterns.md`
- Sources: MagnumIO `gds/samples/3_cuFile_Batch/*`, `4_cuFile_Async/*`, `5_cuFile_MemMap/*` (README only), `cufile.h` (pixi env copy)
- Corrections to earlier conversation statements:
  - `cuFileBatchIOGetStatus` arg 2 is `min_nr`. Passing NULL as the timeout does not mean "return immediately".
  - Batch completions are matched by `cookie`, not by position.
- Open questions (unverified):
  - Can more entries be submitted to a live batch handle?
  - What is the real performance effect of `cuFileStreamRegister` flags on the target filesystems?
  - cufile.json key names for the target cuFile version.
- Next: step 2 (uring reactor design doc) and step 3 (cufile reactor gap analysis, run by an agent in parallel).
