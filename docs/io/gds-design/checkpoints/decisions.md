# Design decisions (user-confirmed)

## D1: Read completion contract is "enqueued on the user stream", not host-complete

Decided: 2026-11-17 by the user. This supersedes `05-final-design.md` FR-1 / §4.8 step 4. The two-phase `_ev` API (P5) becomes unnecessary.

- A successful `device_readv_async_io` future means:
  - every physical I/O has completed and its `bytes_done` has been validated on the host, AND
  - every final D2D copy into the destination is **enqueued**, AND
  - the user's stream is ordered after those copies.
- The future does **not** mean the bytes are visible yet. The caller synchronizes or waits on its stream before touching the data from the host or another stream.
- An earlier resolution is not possible without losing error reporting. Async cuFile reports errors only through `bytes_done`, and that value is readable only after the I/O event fires. What we save is the D2D and its poll, plus being decoupled from user-stream backlog.
- Open sub-decisions:
  - Where the copy runs and how the user stream joins it: a lane copy plus a per-request fan-in join event, or the copy directly on the user stream.
  - PTDS callers.
  - Updating callers that relied on host-complete:
    - `io_context.hpp:493-507` (contract doc)
    - `representation_converter_builtins.cpp:1960-1971` (comment and reasoning)
    - `disk_io.cpp` read path
    - `check_transferred`, the benchmarks, and `test_cufile_reactor.cpp:660`
- Writes are unchanged: the future resolves after the bytes are in the file.
