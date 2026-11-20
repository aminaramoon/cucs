# cuCascade Documentation

Comprehensive documentation for the cuCascade GPU memory management library.

## Quick Navigation

| Document | Description |
|----------|-------------|
| [Architecture Overview](ARCHITECTURE.md) | High-level system design, component relationships, and data flow |
| [Memory Management](memory-management.md) | Deep dive into the three-tier memory system, reservations, and allocators |
| [Data Management](data-management.md) | Data batch lifecycle, repositories, and representation conversion |
| [Topology & Configuration](topology-and-configuration.md) | Hardware discovery, NUMA awareness, and system configuration |
| [cuFile / GPUDirect Storage backend](io/cufile-gds.md) | GDS read paths, per-GPU pool planning, registered staging, Storage Scale checklist |
| [cuFile fused stream lanes and host batch reads](io/cufile-fused-lanes.md) | Opt-in: fused `cuFileReadAsync` device reads on stream lanes, cuFile-batch host reads; configuration, memory, observability |
| [Fused lane reads: design](io/gds-design/10-fused-lane-reads.md) | Design of the fused-lane planner, lane set, host batch reader, reactor integration and pool plan |
| [Fused lanes: Sirius integration handoff](io/gds-design/11-sirius-integration-handoff.md) | What an embedding engine must do to pin, configure, account for and test fused lanes and host batch reads |
| [REST transport](io/rest-transport.md) | The io_uring S3 / REST read reactor |
| [Development Guide](development-guide.md) | Building, testing, benchmarking, and contributing |
| [Data Batch State Transitions](data_batch_state_transitions.md) | State machine reference for `data_batch` |

## Reading Paths

**"I want to understand what cuCascade does"**
1. [Architecture Overview](ARCHITECTURE.md) -- start here for the big picture
2. [Memory Management](memory-management.md) -- understand the core memory tier system
3. [Data Management](data-management.md) -- understand how data moves between tiers

**"I want to use cuCascade in my project"**
1. [Architecture Overview](ARCHITECTURE.md) -- understand the design
2. [Topology & Configuration](topology-and-configuration.md) -- learn how to configure the system
3. [Development Guide](development-guide.md) -- build and integration instructions

**"I want to contribute to cuCascade"**
1. [Development Guide](development-guide.md) -- build, test, and code quality setup
2. [Architecture Overview](ARCHITECTURE.md) -- understand the component relationships
3. Deep dive into [Memory Management](memory-management.md) or [Data Management](data-management.md) as needed

## Module Overview

cuCascade is organized into two core modules:

- **Memory Module** (`include/cucascade/memory/`, `src/memory/`): Manages a three-tier memory hierarchy (GPU, HOST, DISK) with reservation-based allocation, per-stream tracking, NUMA-aware host allocation, and pluggable OOM/limit policies.

- **Data Module** (`include/cucascade/data/`, `src/data/`): Manages data lifecycle across memory tiers with a state machine for batch processing, type-indexed representation converters, and partitioned repository storage for multi-pipeline coordination.

## Related Resources

- [Main README](../README.md) -- project overview, quick start, and usage examples
- [RAPIDS cuDF](https://github.com/rapidsai/cudf) -- GPU DataFrame library (core dependency)
- [RMM](https://github.com/rapidsai/rmm) -- RAPIDS Memory Manager (memory allocation backend)
- [Pixi](https://pixi.sh/) -- Package management tool used for builds
