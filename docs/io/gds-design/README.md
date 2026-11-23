# GDS design record (superseded backend)

The design, reviews, checkpoints and qualification results of the cuFile backend that preceded
the lane reactors: per-GPU groups of single-read reactors, the opt-in async engine, fused stream
lanes and host batch reads. That code is gone; the current backend is described in
[../cufile-gds.md](../cufile-gds.md).

The record is kept because its measurements and what it found out about libcufile and the
filesystems still hold: what `cuFileReadAsync` and the batch API do on each filesystem, what
registration costs, how NICs are chosen. Class, file and configuration names in it refer to the
removed code.
