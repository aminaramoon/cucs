# Checkpoint: step 2, uring reactor design doc

- Status: DONE
- Output: `docs/io/gds-design/02-uring-reactor-design.md`
- Snapshot: branch `io/s2-write` @ `9eff1f9b`
- Key findings:
  1. A pool of N reactors (default 4). Each has 1 worker thread, 1 io_uring, and 64 × 1 MiB pinned staging blocks, registered as fixed buffers.
  2. Reads: 4K read-around → dynamic op size (256 KiB–16 MiB) → (fixed) read/readv into the pinned blocks → H2D on the **caller's stream** → event polled every 100 µs → future resolves.
  3. Writes: head/body/tail split at the granule. Device sources go D2H on the caller's stream into pinned staging, then O_DIRECT for the body and buffered I/O for the edges. Host sources go direct when congruent.
  4. QD is bounded by slots (≤64/reactor) and fan-out (2 reactors/request). Priority tiers use preemption plus bounded low service.
  5. Safety rule: never release staging while a device copy may touch it (sync the caller stream on partial failure).
- Open questions (for counsel):
  - Future-only completion versus stream/event handoff.
  - Which stream the D2D runs on.
  - Thread model for cuFile.
  - Staging block size coupled to the cache chunk size.
