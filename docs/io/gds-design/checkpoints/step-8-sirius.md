# Checkpoint: Sirius integration

## Can `io/cufile-async-engine` drop into Sirius? NO.

- Sirius (`io/s2-write` @ aeb027c89) pins the cuCascade submodule at `65996d6`. That is a rebased, newer `io/s2-write` line with its own GDS design:
  - registered staging slots
  - per-GPU reactor pools sized by Little's law (32 reactors per GPU for network storage, pinned to the GPU's CPUs)
  - small-slice fusing
  - staged writes
  - scratch borrowed from the HOST pools
  - per-transfer buffer registration dropped
- Sirius calls APIs that exist only there: `placement.hpp`, `staging.hpp`, `make_cufile_ioctx`, `plan()`, `staging_mode`, `reactors_per_device`, `fuse_gap_bytes`, `storage_hint_path`, `reserve_cufile_staging`.
- Our engine branch forked from the older base `9eff1f9`. Merge base with the pin: `a11ff66`. The pin carries 60+ commits we lack, plus about 7.3k changed lines in `src/io`, the cudf layer and the tests.

## Done

Sirius branch `config/io-throughput` @ `23a17b83b`, based on `io/s2-write`, cuCascade pin unchanged:
- DISK spill-tier I/O context configurable: `sirius.memory.disk.io` / `sirius.space.disk[].io` → `make_disk_io_context`, attached to every DISK space. With no block, cuCascade's default context is used, as before.
- Remaining scan knobs: `uring.use_odirect`, `low_service_interval`, `write_split_bytes`; `cufile.write_split_bytes`.
- The cuFile staging reservation is summed across the scan and DISK pools.
- Docs: max-throughput recipes for GDS on Storage Scale/WEKA/VAST, GDS on local NVMe, and non-GDS hosts; NIC steering via `CUFILE_ENV_PATH_JSON`.
- Review: approve-with-changes, no BLOCKING findings. All SHOULD findings and NITs fixed. See `reviews/sirius-config-io-throughput.md`.
- Tests (lead verified): `[config],[disk_io_config],[scan_manager]` 350 cases pass; `[multi_gpu_foundation]` 7 cases pass.

## Open decision

Whether and how to port the async engine onto the `65996d6` line. This needs reconciling two designs: the async-lanes engine versus the synchronous per-GPU reactor pools with staging slots.
