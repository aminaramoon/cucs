/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cucascade/exec/config.hpp>
#include <cucascade/io/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cucascade::io::cufile {

/**
 * @brief Per-file policy for using cuFile.
 */
enum class compat : std::uint8_t {
  automatic,  ///< try cuFile; a file that cannot be registered falls back to POSIX I/O
  on,         ///< never use cuFile: every transfer of the file goes through POSIX I/O
  off,        ///< enforce cuFile: opening a file that cannot be registered throws
};

/** @brief Opt-in transport mode; legacy remains the default until hardware qualification. */
enum class engine_mode : std::uint8_t { legacy, async_auto, async_simulated_for_testing };

/** @brief Which mount identity an operator override matches. */
enum class mount_match : std::uint8_t { fsid, mount_prefix, fs_type };

/** @brief An operator's routing decision, never evidence of verified direct I/O. */
enum class mount_override_state : std::uint8_t { operator_qualified, known_compat, legacy };

/**
 * @brief Mount routing and optional preferred profile, interpreted by the future policy layer.
 *
 * The fsid is an opaque string, keeping the configuration independent of platform fsid types.
 * A mount prefix must already be a normalized absolute path: root is allowed, but trailing
 * slashes, repeated slashes, and @c . or @c .. components are not. Filesystem types must contain
 * no whitespace. Profile values are preferences: capacity fitting may reduce QD and choose a
 * smaller supported staging class than @c op_bytes.
 */
struct mount_override {
  mount_match match{mount_match::mount_prefix};
  std::string value;
  mount_override_state state{mount_override_state::legacy};
  std::optional<unsigned> qd;
  std::optional<std::size_t> op_bytes;
  bool operator==(mount_override const&) const = default;
};

/**
 * @brief Configuration for the opt-in per-device async engine.
 *
 * This is a configuration-only interface: construction and validation perform no probing,
 * CUDA work, or runtime initialization. Only non-legacy contexts bootstrap the engine.
 */
struct async_engine_config {
  engine_mode mode{engine_mode::legacy};
  bool async_reads{true};
  bool async_writes{false};
  bool unregistered_device_tier{true};
  bool direct_registered_io{false};
  bool stream_register{true};
  bool affinity{true};
  bool coalesce{true};  ///< Engine-only small-slice grouping; legacy routing is unchanged.
  unsigned coalesce_max_targets{64};

  /// Mounts are assigned round-robin to 1..8 submitters on each device.
  unsigned submitters_per_device{1};

  /// Per-device allocation target. Zero means auto, not disabled: round down to 16 MiB
  /// the minimum of 1 GiB, 5% of device memory, and reservable memory. Explicit nonzero
  /// targets are accepted as given; actual allocation/registration is a resource concern.
  std::size_t arena_bytes{0};
  unsigned device_lane_cap{160};
  std::size_t max_pending_slices_per_device{4096};
  unsigned low_service_interval{4};
  unsigned max_short_reads{64};

  /// Routing precedence: fsid, then longest matching mount_prefix, then fs_type. Prefixes
  /// match whole path components of normalized absolute paths; the first entry in list order
  /// wins any tie. Exact duplicate {match, value} selectors are invalid, regardless of state.
  std::vector<mount_override> mount_overrides;

  struct probe_config {
    unsigned submit_max_us{100};
    /// False permits non-blocking-submit-only qualification for simulator/compat tests.
    bool require_direct_evidence{true};
    std::size_t bytes{16UL << 20};
    bool operator==(probe_config const&) const = default;
  } probe;

  struct demotion_config {
    unsigned slow_floor_us{100};
    unsigned p99_multiple{4};
    unsigned slow_of_last_eight{3};
    unsigned stuck_ms{2};
    unsigned cooldown_s{30};
    unsigned cooldown_cap_s{960};
    bool operator==(demotion_config const&) const = default;
  } demotion;

  struct poll_config {
    unsigned io_cap_us{100};
    unsigned copy_cap_us{10};
    unsigned fence_hint_us{100};
    unsigned due_events_per_round{16};
    unsigned idle_health_ms{20};
    bool operator==(poll_config const&) const = default;
  } poll;

  bool operator==(async_engine_config const&) const = default;

  /**
   * @brief Check syntactic ranges without consulting device or filesystem capabilities.
   *
   * Flags are independently selectable, including in legacy mode. An override's QD need
   * not fit the lane cap: effective capacity is determined later by resource fitting.
   * Byte counts used for direct-I/O probes/profile preferences must be positive multiples
   * of @c io::IO_BLOCK_SIZE; device-specific granules are checked by the future policy layer.
   *
   * @throws std::invalid_argument naming the first invalid field.
   */
  void validate() const
  {
    auto const positive = [](auto value, char const* field) {
      if (value == 0) { throw std::invalid_argument(std::string{field} + " must be positive"); }
    };
    auto const aligned_bytes = [](std::size_t value, char const* field) {
      if (value == 0 || value % io::IO_BLOCK_SIZE != 0) {
        throw std::invalid_argument(std::string{field} +
                                    " must be a positive multiple of IO_BLOCK_SIZE");
      }
    };

    switch (mode) {
      case engine_mode::legacy:
      case engine_mode::async_auto:
      case engine_mode::async_simulated_for_testing: break;
      default: throw std::invalid_argument("mode is invalid");
    }
    if (submitters_per_device < 1 || submitters_per_device > 8) {
      throw std::invalid_argument("submitters_per_device must be in [1, 8]");
    }
    positive(device_lane_cap, "device_lane_cap");
    positive(max_pending_slices_per_device, "max_pending_slices_per_device");
    positive(low_service_interval, "low_service_interval");
    positive(max_short_reads, "max_short_reads");
    // coalesce is independently selectable, including in legacy mode.
    positive(coalesce_max_targets, "coalesce_max_targets");
    if (coalesce_max_targets > 4096)
      throw std::invalid_argument("coalesce_max_targets must be in [1, 4096]");
    if (device_lane_cap > 160 || max_pending_slices_per_device > (1UL << 20)) {
      throw std::invalid_argument("engine metadata/lane capacity exceeds supported limit");
    }
    positive(probe.submit_max_us, "probe.submit_max_us");
    aligned_bytes(probe.bytes, "probe.bytes");
    positive(demotion.slow_floor_us, "demotion.slow_floor_us");
    positive(demotion.p99_multiple, "demotion.p99_multiple");
    if (demotion.slow_of_last_eight < 1 || demotion.slow_of_last_eight > 8) {
      throw std::invalid_argument("demotion.slow_of_last_eight must be in [1, 8]");
    }
    positive(demotion.stuck_ms, "demotion.stuck_ms");
    positive(demotion.cooldown_s, "demotion.cooldown_s");
    if (demotion.cooldown_cap_s < demotion.cooldown_s) {
      throw std::invalid_argument("demotion.cooldown_cap_s must be >= demotion.cooldown_s");
    }
    positive(poll.io_cap_us, "poll.io_cap_us");
    positive(poll.copy_cap_us, "poll.copy_cap_us");
    positive(poll.fence_hint_us, "poll.fence_hint_us");
    positive(poll.due_events_per_round, "poll.due_events_per_round");
    positive(poll.idle_health_ms, "poll.idle_health_ms");

    for (std::size_t index = 0; index < mount_overrides.size(); ++index) {
      auto const& entry = mount_overrides[index];
      switch (entry.match) {
        case mount_match::fsid: break;
        case mount_match::fs_type:
          if (entry.value.find_first_of(" \t\n\r\f\v") != std::string::npos) {
            throw std::invalid_argument("mount_overrides.value fs_type must contain no whitespace");
          }
          break;
        case mount_match::mount_prefix:
          if (entry.value.empty() || entry.value.front() != '/' ||
              (entry.value.size() > 1 && entry.value.back() == '/') ||
              entry.value.find("//") != std::string::npos ||
              entry.value.find("/./") != std::string::npos ||
              entry.value.find("/../") != std::string::npos || entry.value.ends_with("/.") ||
              entry.value.ends_with("/..")) {
            throw std::invalid_argument(
              "mount_overrides.value must be a normalized absolute mount_prefix");
          }
          break;
        default: throw std::invalid_argument("mount_overrides.match is invalid");
      }
      if (entry.value.empty() || entry.value.find('\0') != std::string::npos) {
        throw std::invalid_argument("mount_overrides.value must be nonempty and contain no NUL");
      }
      switch (entry.state) {
        case mount_override_state::operator_qualified:
        case mount_override_state::known_compat:
        case mount_override_state::legacy: break;
        default: throw std::invalid_argument("mount_overrides.state is invalid");
      }
      if (entry.qd) { positive(*entry.qd, "mount_overrides.qd"); }
      if (entry.op_bytes) { aligned_bytes(*entry.op_bytes, "mount_overrides.op_bytes"); }
      for (std::size_t earlier = 0; earlier < index; ++earlier) {
        if (mount_overrides[earlier].match == entry.match &&
            mount_overrides[earlier].value == entry.value) {
          throw std::invalid_argument(
            "mount_overrides contains duplicate {match, value} selectors");
        }
      }
    }
  }
};

/**
 * @brief Tunables for the cuFile / GPUDirect Storage local-file reactor.
 *
 * This backend registers each file with @c cuFileHandleRegister and serves
 * device destinations with synchronous @c cuFileRead calls issued from the
 * reactor's worker thread.  A
 * device read starts once the work already queued on the caller's CUDA stream
 * has finished and completes host-observed; with a real GDS stack cuFile DMAs
 * from NVMe straight into GPU memory with no host-side bounce of its own.
 *
 * Host-only reads go O_DIRECT (through the reactor's pinned scratch when not
 * block-aligned); mixed host+device requests use POSIX @c pread on the buffered
 * fd.
 *
 * @note Host-source writes whose aligned body is below 4 MiB or does not line up
 *       with its source address (e.g. the DISK tier's pinned pool blocks) are
 *       written through the page cache with writeback started at once
 *       (@c sync_file_range), which measured faster than O_DIRECT from a
 *       synchronous reactor.  Spilled host data can therefore transiently occupy
 *       the page cache; the pages are clean, hence reclaimable, once written
 *       back, and durability still needs a flush (@c fdatasync).  Larger
 *       congruent bodies go O_DIRECT from the caller's buffer.
 *
 * Writes of device sources use @c cuFileWrite for their aligned body only when
 * cuFile reports a real GDS mode; on a compat-only platform every device write
 * is staged through pinned host memory and @c pwrite, which measured faster
 * there (see @c cufile_reactor).  No setting changes that.
 *
 * @note libcufile is loaded at run time (@c cufile_api); a host without it runs
 *       every transfer through POSIX I/O.  The backend is built when @c cufile.h is
 *       found at configure time (@c CUCASCADE_HAS_CUFILE).
 */
struct config {
  /// How many scan tasks the readahead manager may keep in flight against this
  /// backend at once.  Zero disables readahead for it entirely.  Local NVMe /
  /// GDS saturates at modest queue depth, so one scan per pipeline thread is
  /// enough to keep the decoders fed without over-committing the device.
  std::size_t n_max_concurrent_scans{
    static_cast<std::size_t>(exec::default_gpu_pipeline_num_threads)};

  /// Whether the config named @c n_max_concurrent_scans explicitly.  Same role as
  /// in the uring config: distinguish an explicit value equal to the struct
  /// default from a defaulted one, so a derived default (which follows the
  /// pipeline width) can override the latter but not the former.
  bool n_max_concurrent_scans_explicit{false};

  /// Maximum asynchronous items per reactor: device slices parked until their
  /// stream is ready plus fallback host-to-device copies in flight.  cuFile and
  /// staged transfers are synchronous on the worker and never count; the reactor
  /// stops dispatching new slices once this many items await their CUDA event,
  /// so slices parked behind busy streams can fill the window; parked slices that
  /// became ready are served (in about @ref max_op_bytes portions) before new ones.
  std::size_t max_inflight{32};

  /// Largest single @c cuFileRead the reactor issues; a larger device slice is
  /// read in pieces of at most this size whose boundaries fall on 4 KiB file
  /// offsets.  One call this large lets cuFile split it internally across its
  /// own threads, which is what saturates a local NVMe from one reactor.  Must be
  /// at least @c io::IO_BLOCK_SIZE.
  ///
  /// @note 64 MiB was measured in cuFile compatibility mode (no nvidia-fs; one
  ///       thread reached 6.83 GB/s at 64 MiB vs 6.00 at 16 MiB against a
  ///       7.1 GB/s device).  Re-sweep it on a
  ///       host with a real GDS stack, where cuFile chunks by its own
  ///       @c max_direct_io_size (16 MiB) anyway.
  std::size_t max_op_bytes{64UL << 20};

  /// Register device buffers with @c cuFileBufRegister for the duration of one
  /// transfer when the transfer is at least this many bytes and the buffer (and,
  /// for a read, the file offset) is 4 KiB-aligned; 0 (the default) never
  /// registers.  The registration is dropped before the transfer's future
  /// resolves.
  ///
  /// Registrations are shared through a process-wide table keyed by base
  /// address: a transfer whose buffer starts at a live registration's base and
  /// fits inside it reuses that registration (the last user deregisters); one
  /// whose buffer otherwise overlaps a live registration takes the staged POSIX
  /// path, and a range an unregistered cuFile transfer is using is not registered
  /// until it completes, so no cuFile call ever addresses memory a concurrent
  /// transfer may deregister.  Every cuFile transfer takes part in that table,
  /// also with this knob at 0 (which only stops this context from creating
  /// registrations), so contexts with different settings stay safe together.
  ///
  /// @note Off by default on purpose: NVIDIA advises against registering a
  ///       one-shot destination (the cost is not amortised and BAR1 space is
  ///       shared), and in compatibility mode it measured no gain here.  It
  ///       exists to A/B on a host with
  ///       a real GDS stack, where an aligned registered buffer is DMA'd directly
  ///       instead of through cuFile's GPU bounce buffers.
  std::size_t register_min_bytes{0};

  /// Write slices larger than this are cut by @c templated_ioctx into pieces of
  /// this size (rounded down to the write granule) and spread over the selected
  /// reactors; 0 = never cut.  cuFile prefers large submissions (one call splits
  /// internally across cuFile's own threads), so the default is no split.
  std::size_t write_split_bytes{0};

  /// Device reads below this size use POSIX pread (through the reactor's pinned
  /// scratch) instead of cuFile.  256 KiB
  /// sits above the <= 128 KiB small-I/O regression NVIDIA documents for real GDS
  /// on kernels >= 6.11, and in compatibility mode a 256 KiB @c cuFileRead from
  /// four threads already reached 4 GB/s here.
  std::size_t gds_threshold{256UL << 10};

  /// Whether the reactor opens the O_DIRECT fd needed to register with cuFile
  /// (and used for aligned host reads and aligned POSIX writes).  If false,
  /// cuFile registration is skipped and every read and write goes through POSIX
  /// and the page cache --
  /// effectively disabling the reactor's fast path.  Kept as a knob so a
  /// filesystem that rejects O_DIRECT can still exercise the reactor end-to-end
  /// via the fallback.
  bool use_odirect{true};

  /// Per-file cuFile policy (see @ref compat).  Unset means @c compat::automatic.
  /// Independently, @c CUFILE_FORCE_COMPAT_MODE=true in the environment makes
  /// cuFile itself run in compatibility mode (see @c probe_platform()).
  std::optional<compat> compat_mode;

  /// How long the worker sleeps between completion polls while fallback copies
  /// are in flight and it has nothing to submit.  A completion is only noticed at
  /// a poll, so this bounds the latency a finished copy adds; it is not a wait the
  /// reactor takes when it has work to dispatch.
  std::size_t poll_interval_us{200};

  /// Bounded service for the low-priority tier.  Priority is otherwise strict (a
  /// waiting high request is taken first and parks an active low one), so a
  /// demand read that waits on in-flight low work could starve behind a
  /// continuous stream of high requests.  After this many high requests have
  /// started while low work was waiting, the reactor grants the low tier one
  /// turn (at least @c max_inflight slices) even though high work is waiting.
  /// 0 restores strict priority.
  std::size_t low_service_interval{4};

  /// Opt-in staged device-read engine. Legacy preserves existing routing and completion.
  async_engine_config async_engine;

  /// cuFile can only DMA whole pages, so a read is widened to a page boundary
  /// either way -- reporting it lets the caller align once, up front, instead of
  /// every layer rediscovering it.
  [[nodiscard]] std::size_t min_alignment_requirement() const noexcept { return io::IO_BLOCK_SIZE; }

  /// A local read is a syscall against NVMe, so bridging is only worth it when
  /// the bridged bytes cost less than the extra request -- one page.
  [[nodiscard]] std::size_t merge_gap_size() const noexcept { return io::IO_BLOCK_SIZE; }
};

}  // namespace cucascade::io::cufile
