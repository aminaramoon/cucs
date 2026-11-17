# Checkpoint: step 3, cuFile reactor gap analysis

- Status: DONE (read-only analysis; no source changed)
- Output: `docs/io/gds-design/03-cufile-reactor-gap-analysis.md`
- Read in full: `src/io/cufile/*`, `include/cucascade/io/cufile/*`, shared io abstractions
  (`io_context`, `io_request`, `templated_ioctx`, `tiered_queue`, `types`), DISK-tier consumers
  (`src/cudf/disk_io*.cpp`, converters), tests/benchmarks (grep + targeted reads).

## Top 5 findings
1. The reactor is **host-synchronous**: sync `cuFileRead`/`cuFileWrite` on `cufile_n_reactors`=4 threads
   (`io/config.hpp:94`; `cufile_reactor.cpp:1189,1616`). Max process-wide QD is 4. A single slice goes to
   one reactor, device writes to 2. `max_inflight=32` is **not** a QD bound. No `cuFileReadAsync`,
   `cuFileBatchIO*` or `cuFileStreamRegister` is resolved (`cufile_api.cpp:72-79`).
2. **No registered staging ring; device buffers are unregistered by default** (`config.hpp:128`). Only a
   pinned *host* scratch (2 × 8 MiB per reactor) for the POSIX fallback. An opt-in per-transfer
   registration with a process-wide overlap table exists. It cannot go stale across transfers, but an
   externally registered ring would collide with it (`ALREADY_REGISTERED` → POSIX, `:385`).
3. **Both directions of the stream sync are host-side**: input gate = host `cudaStreamQuery` / event
   polling at 200 µs (`:1122-1156`, `:1910`); output = host-complete future. The io_context contract
   (`io_context.hpp:493-507`) and the DISK→GPU converter (`representation_converter_builtins.cpp:1960-1971`)
   **depend on host-complete**, so a GPU-side join is an API/contract decision, not a drop-in.
   The write contract already permits stream-ordered `cuFileWriteAsync` (`templated_ioctx.hpp:227-230`).
4. **All defaults were tuned in cuFile compat mode** (no nvidia-fs): 64 MiB op, registration off, staged
   writes, 256 KiB threshold, gdsio baselines (`config.hpp:62-65,99-103,122-125`;
   `benchmark_disk_converter.cpp:90-96`). Nothing is validated on real GDS. Async cuFile in compat runs
   synchronously in the submitter (`cufile_reactor.hpp:168-171`).
5. Keep-worthy scaffolding: null/PTDS stream pinning at dispatch (commit `469a0221`;
   `templated_ioctx.hpp:133-156`), coordinator exactly-once credits, priority scheduler, object-open
   robustness, never-destroyed scratch pool (avoids device-sync at teardown). Also: `cufile_api` treats
   every member as required (`cufile_api.hpp:34-35`), so new async/batch symbols must be optional or old
   libcufile disables cuFile entirely.

## Open questions
- Keep host-complete futures (resolve on event completion) or move to "enqueued + GPU-joined"?
- How to issue the cross-thread join: the user-stream wait must come after the reactor records the event.
  Two-phase API? Reaper callback? Null/PTDS binding to the submitter's resolved stream?
- Does `cuFileBufRegister` accept interior sub-ranges of pool chunks, and can two registrations share a
  GPU page?
- Is cuFile's unregistered-buffer bounce pinned-host or GPU-resident? (`cufile.h:430,658` vs step-1 §3.)
- Ring sizing vs BAR1 (T4 CI runner) and the `memory_reservation_manager` budget.
- Reaper signal and thread for async completions (short reads, credits, slot release, `io_object` lifetime).
- `cufile.json` keys and `cuFileSetParameter*` ordering vs the eager `cuFileDriverOpen` in `probe_platform`.
- Need a real-GDS host and real-GDS baselines (current gdsio reference is compat mode).

## Lead review note
- Open question "is the bounce buffer pinned-host or GPU?" is RESOLVED: it is GPU. In `cufile.h`, "pinned buffers" refers to pinned *GPU* memory (`CU_FILE_GPU_MEMORY_PINNING_FAILED`: "failed to allocate pinned GPU Memory"; `max_device_pinned_mem_size`; `CUFILE_PARAM_GPU_BOUNCE_BUFFER_SLAB_*`). `01` §3 has been updated with this evidence.
