/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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
#include "engine_ring.hpp"

#include <cucascade/error.hpp>
#include <cucascade/io/cufile/cufile_reactor.hpp>
#include <cucascade/io/cufile/detail.hpp>
#include <cucascade/io/cufile/device_engine.hpp>
#include <cucascade/io/cufile/runtime.hpp>
#include <cucascade/log/logging.hpp>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <queue>
#include <semaphore>
#include <thread>
#include <unordered_map>

namespace cucascade::io::cufile {
namespace {
using namespace std::chrono_literals;
using time_point  = policy_clock::time_point;
using nanoseconds = std::chrono::nanoseconds;

std::uint64_t ns(nanoseconds value) noexcept
{
  return static_cast<std::uint64_t>(std::max<std::int64_t>(0, value.count()));
}
void sample(engine_histogram& histogram, nanoseconds value) noexcept
{
  auto const v = ns(value);
  ++histogram.buckets[std::min<unsigned>(63, static_cast<unsigned>(std::bit_width(v)))];
  ++histogram.count;
  histogram.total_ns += v;
}
nanoseconds median(engine_histogram const& histogram) noexcept
{
  std::uint64_t count{};
  for (unsigned i = 0; i < 63; ++i) {
    count += histogram.buckets[i];
    if (count > histogram.count / 2) return nanoseconds{std::uint64_t{1} << i};
  }
  return 0ns;
}

class notification {
 public:
  notification() : _fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
  {
    if (_fd < 0) throw std::system_error(errno, std::generic_category(), "engine eventfd");
  }
  void notify() noexcept
  {
    std::uint64_t const one = 1;
    auto const ignored      = ::write(_fd, &one, sizeof(one));
    static_cast<void>(ignored);
  }
  void wait(nanoseconds duration) noexcept
  {
    auto const value = std::max(0ns, duration);
    timespec timeout{static_cast<time_t>(value.count() / 1000000000),
                     static_cast<long>(value.count() % 1000000000)};
    pollfd fd{_fd, POLLIN, 0};
    static_cast<void>(ppoll(&fd, 1, &timeout, nullptr));
    std::uint64_t count;
    static_cast<void>(::read(_fd, &count, sizeof(count)));
  }
  ~notification() { ::close(_fd); }

 private:
  int _fd;
};

/** Bootstrap starts workers; semaphore permits cannot lose wake-ups or poll while idle. */
class lazy_pool final : public control_executor, public std::enable_shared_from_this<lazy_pool> {
 public:
  explicit lazy_pool(unsigned workers) : _workers(workers), _tasks(8192) {}
  bool submit(std::function<void()> task) noexcept override
  {
    if (!_tasks.push(std::move(task))) return false;
    _ready.release();
    return true;
  }
  bool schedule(time_point, std::function<void()>) noexcept override { return false; }
  static std::shared_ptr<control_executor> cleanup()
  {
    static auto* pool = new std::shared_ptr<control_executor>(make(2));
    return *pool;
  }
  static std::shared_ptr<control_executor> blocking()
  {
    static auto* pool = new std::shared_ptr<control_executor>(make(4));
    return *pool;
  }
  static std::shared_ptr<control_executor> recovery()
  {
    static auto* pool = new std::shared_ptr<control_executor>(make(2));
    return *pool;
  }

 private:
  static std::shared_ptr<lazy_pool> make(unsigned count)
  {
    auto pool = std::make_shared<lazy_pool>(count);
    pool->start();
    return pool;
  }
  void start()
  {
    // Process-lifetime workers, never detached or destroyed during CUDA/static teardown.
    for (unsigned i = 0; i < _workers; ++i) {
      _threads.emplace_back([this] {
        for (;;) {
          _ready.acquire();
          std::function<void()> task;
          while (!_tasks.pop(task))
            std::this_thread::yield();
          {
            try {
              task();
            } catch (...) { /* Engine tasks publish their own errors. */
            }
          }
        }
      });
    }
  }
  unsigned _workers;
  detail::engine_ring<std::function<void()>> _tasks;
  std::counting_semaphore<> _ready{0};
  std::vector<std::thread> _threads;
};

struct completion_retirement {
  explicit completion_retirement(std::shared_ptr<grouped_coordinator> value)
    : group(std::move(value))
  {
    group->retain_async_access();
  }
  ~completion_retirement() { group->release_async_access(); }
  std::shared_ptr<grouped_coordinator> group;
};

struct engine_registry {
  std::mutex mutex;
  std::unordered_map<int, std::unique_ptr<device_engine>> engines;
  std::atomic<std::shared_ptr<const std::vector<device_engine*>>> published;
  std::unordered_map<int, bool> warned;
  static engine_registry& instance()
  {
    static auto* registry = new engine_registry;
    return *registry;
  }
};
}  // namespace

struct device_engine::impl : std::enable_shared_from_this<impl> {
  struct mount;
  struct request_budget {
    std::array<std::int64_t, 2> deficit{};
  };
  struct cursor {
    explicit cursor(engine_slice value)
      : slice(std::move(value)), group(slice.credit.coordinator()), next(slice.logical.offset)
    {
    }
    engine_slice slice;
    std::shared_ptr<grouped_coordinator> group;
    mount* owner{};
    std::size_t next{}, active{}, index{};
    std::uint64_t group_id{};
    std::shared_ptr<request_budget> budget;
    bool finished{}, fence_ready{};
    time_point fence_queried{};
    [[nodiscard]] bool cancelled() const noexcept
    {
      return (slice.cancel && slice.cancel->load()) || !group->should_continue();
    }
  };
  struct node {
    operation* op{};
    std::shared_ptr<cursor> request;
    mount* owner{};
    recorded_event event;
    std::uint64_t generation{};
    unsigned delay_us{};
    time_point started{}, io_observed{};
    grouped_coordinator::error_type error{std::make_error_code(std::errc::io_error)};
    enum class work { none, retry, fallback, check_size, recovery, cleanup } pending{work::none};
    bool fallback{}, cleaning{}, gate_limited{}, retry_after_recovery{};
  };
  struct mount {
    mount(std::shared_ptr<mount_policy> value, std::size_t index, std::size_t capacity)
      : policy(std::move(value)), id(index + 1), commands(capacity)
    {
    }
    std::shared_ptr<mount_policy> policy;
    arena_mount_id id;
    detail::engine_ring<node*> commands;
    effective_profile effective{};
    std::size_t in_io{}, occupied{}, gated{}, submitter{}, max_io{}, max_occupied{};
    std::array<std::size_t, 2> request_next{};
    std::array<std::uint64_t, 2> last_group{}, current_group{};
    std::uint64_t next_group_id{};
    std::array<std::int64_t, 2> deficit{};
    engine_histogram completion;
    std::atomic<bool> refuse{};
    unsigned registered_submitters{};
    bool active{};
    time_point lane_wait_since{}, slot_wait_since{};
  };
  struct submitter {
    explicit submitter(std::size_t capacity) : registrations(capacity) { mounts.reserve(capacity); }
    detail::engine_ring<mount*> registrations;
    std::vector<mount*> mounts;
    std::size_t next{};
    notification wake;
    std::thread thread;
    std::atomic<mount*> calling{};
    std::atomic<std::int64_t> call_start{-1};
  };
  enum class result_kind {
    submit_failed,
    recorded,
    not_submitted,
    failed,
    sync_done,
    size_checked,
    recovered,
    cleaned,
    capacity
  };
  struct result {
    result_kind kind{};
    node* item{};
    std::uint64_t generation{};
    nanoseconds elapsed{};
    cudaError_t status{cudaSuccess};
    std::shared_ptr<std::vector<mount_capacity_grant>> grants{};
  };
  struct deadline {
    time_point due;
    node* item;
    std::uint64_t generation;
    bool operator<(deadline const& other) const noexcept { return due > other.due; }
  };

  int device;
  async_engine_config config;
  device_engine_options options;
  operation_pool operations;
  detail::engine_ring<std::shared_ptr<cursor>> control;
  detail::engine_ring<result> completions;
  std::vector<std::shared_ptr<cursor>> requests;
  std::vector<std::unique_ptr<mount>> mounts;
  std::vector<node> nodes;
  std::vector<node*> free_nodes, active_nodes;
  std::vector<std::unique_ptr<submitter>> submitters;
  std::priority_queue<deadline> deadlines;
  std::thread reaper;
  notification wake;
  std::atomic<bool> stop{}, exited{}, stop_submitters{};
  std::atomic<std::size_t> outstanding{}, rejected{}, handoffs{}, capacity_active{};
  std::shared_ptr<impl> incomplete_owner;
  std::size_t mount_next{}, live{};
  unsigned high_since_low{};
  bool rebalance_needed{}, rebalance_running{};
  std::size_t rebalance_count{};
  std::uint8_t class_mask;
  std::size_t arena_bytes;
  engine_gauges stats;
  mutable std::mutex snapshot_mutex;
  engine_gauges snapshot;
  time_point origin, last_account, last_snapshot{};
  std::shared_ptr<control_executor> cleanup;
  bool retry_needed{};
  long double qd_area{}, backlog_area{};
  std::uint64_t backlog_ns{};

  impl(int d, async_engine_config cfg, device_engine_options opts)
    : device(d),
      config(std::move(cfg)),
      options(std::move(opts)),
      operations(config),
      control(config.max_pending_slices_per_device),
      completions(16 * config.device_lane_cap),
      nodes(4 * config.device_lane_cap),
      class_mask(options.arena->available_class_mask()),
      arena_bytes(options.arena->gauges().registered_bytes),
      origin(options.now()),
      last_account(origin)
  {
    free_nodes.reserve(nodes.size());
    active_nodes.reserve(nodes.size());
    for (auto& n : nodes)
      free_nodes.push_back(&n);
    requests.reserve(config.max_pending_slices_per_device);
    mounts.reserve(config.max_pending_slices_per_device);
    submitters.reserve(config.submitters_per_device);
    if (!options.control) options.control = control_executor::process_instance();
    if (!options.blocking) options.blocking = lazy_pool::blocking();
    if (!options.recovery) options.recovery = lazy_pool::recovery();
    cleanup = options.cleanup ? options.cleanup : lazy_pool::cleanup();
    if (!options.buffered_fd)
      options.buffered_fd = [](io_object const& object) {
        auto const* file = dynamic_cast<cufile_io_object const*>(&object);
        return file ? file->buffered_fd() : -1;
      };
    if (!options.file_handle)
      options.file_handle = [](io_object const& file) -> void* {
        auto const* f = dynamic_cast<cufile_io_object const*>(&file);
        return f ? f->cufile_handle() : nullptr;
      };
    std::vector<deadline> heap_storage;
    heap_storage.reserve(nodes.size());
    deadlines = decltype(deadlines)(std::less<deadline>{}, std::move(heap_storage));
    for (unsigned i = 0; i < config.submitters_per_device; ++i)
      submitters.push_back(std::make_unique<submitter>(config.max_pending_slices_per_device));
  }

  void start()
  {
    // Constructor/control thread only, never hand_off or the reaper.
    incomplete_owner = shared_from_this();
    try {
      for (auto& s : submitters)
        s->thread = std::thread([this, p = s.get()] { run_submitter(*p); });
      reaper = std::thread([self = shared_from_this()] { self->run(); });
    } catch (...) {
      stop_submitters.store(true);
      for (auto& s : submitters) {
        s->wake.notify();
        if (s->thread.joinable()) s->thread.join();
      }
      incomplete_owner.reset();
      throw;
    }
  }
  void publish(result value) noexcept
  {
    // Capacity exceeds all live operations/publications. Only producers can wait here.
    while (!completions.push(std::move(value)))
      std::this_thread::yield();
    wake.notify();
  }
  void run_submitter(submitter& s) noexcept
  {
    auto const device_status = cudaSetDevice(device);
    while (!stop_submitters.load()) {
      mount* m;
      while (s.registrations.pop(m))
        s.mounts.push_back(m);
      unsigned drained{};
      while (drained < 8 && !s.mounts.empty()) {
        node* n{};
        for (std::size_t count = 0; count < s.mounts.size(); ++count) {
          m = s.mounts[s.next++ % s.mounts.size()];
          if (m->commands.pop(n)) break;
        }
        if (n == nullptr) break;
        ++drained;
        if (m->refuse.load() || !m->policy->read_eligible() || stop.load() ||
            n->request->cancelled()) {
          publish({result_kind::not_submitted, n, n->generation});
          continue;
        }
        if (device_status != cudaSuccess) {
          publish({result_kind::failed, n, n->generation, {}, device_status});
          continue;
        }
        auto& op = *n->op;
        s.calling.store(m);
        auto const started = options.now();
        s.call_start.store(started.time_since_epoch().count(), std::memory_order_release);
        detail::before_engine_transfer_for_testing();
        auto const rc = options.transport->read_async(options.file_handle(*op.file),
                                                      op.staging->base_ptr_to_pass(),
                                                      &op.args.size,
                                                      &op.args.file_offset,
                                                      &op.args.buffer_offset,
                                                      &op.args.bytes_done,
                                                      (*op.lane)->stream());
        s.call_start.store(-1, std::memory_order_release);
        auto const elapsed = options.now() - started;
        auto status =
          rc.ok() ? options.transport->record_event((*op.lane)->io_event(), (*op.lane)->stream())
                  : cudaErrorUnknown;
        publish({!rc.ok()                ? result_kind::submit_failed
                 : status == cudaSuccess ? result_kind::recorded
                                         : result_kind::failed,
                 n,
                 n->generation,
                 elapsed,
                 status});
      }
      if (drained == 8) continue;
      s.wake.wait(20ms);
    }
  }

  void account(time_point now)
  {
    auto const elapsed = ns(now - last_account);
    qd_area += static_cast<long double>(stats.storage_qd) * elapsed;
    bool backlog = false;
    for (auto const& r : requests)
      if (r && !r->finished) {
        backlog = true;
        break;
      }
    if (backlog) {
      backlog_area += static_cast<long double>(stats.storage_qd) * elapsed;
      backlog_ns += elapsed;
      for (auto const& m : mounts) {
        if (m->in_io < m->effective.qd) {
          stats.refill_gap_ns += elapsed;
          break;
        }
      }
    }
    last_account = now;
  }
  void token(node& n, bool take)
  {
    account(options.now());
    if (take) {
      ++n.owner->in_io;
      n.owner->max_io = std::max(n.owner->max_io, n.owner->in_io);
      ++stats.storage_qd;
      stats.max_storage_qd = std::max(stats.max_storage_qd, stats.storage_qd);
    } else if (n.op->holds_qd_token) {
      --n.owner->in_io;
      --stats.storage_qd;
    }
    n.op->holds_qd_token = take;
  }
  void dispatch(node& n)
  {
    if (n.owner->in_io >= n.owner->effective.qd) {
      n.pending = node::work::retry;
      return;
    }
    n.pending = node::work::none;
    n.op->set_phase(operation_phase::submission_queued);
    ++n.generation;
    n.started = options.now();
    token(n, true);
    auto* p                            = &n;
    [[maybe_unused]] bool const queued = n.owner->commands.push(std::move(p));
    if (!queued) {
      token(n, false);
      n.error = std::make_error_code(std::errc::no_buffer_space);
      finish(n, false, proof::nothing_enqueued);
      return;
    }
    submitters[n.owner->submitter]->wake.notify();
  }
  void poll_later(node& n, bool first)
  {
    bool const io  = n.event.phase == operation_phase::io_inflight;
    auto const cap = io ? config.poll.io_cap_us : config.poll.copy_cap_us;
    if (first) {
      auto const prediction = ns(median(n.owner->completion)) / 2000;
      n.delay_us =
        io ? static_cast<unsigned>(std::min<std::uint64_t>(std::numeric_limits<unsigned>::max(),
                                                           std::max<std::uint64_t>(10, prediction)))
           : std::min(2U, cap);
    } else {
      n.delay_us = std::min(cap,
                            n.delay_us < 2    ? 2U
                            : n.delay_us < 5  ? 5U
                            : n.delay_us < 10 ? 10U
                            : n.delay_us < 25 ? 25U
                            : n.delay_us < 50 ? 50U
                                              : cap);
    }
    auto const delay = std::chrono::microseconds(n.delay_us);
    sample(io ? stats.io_poll_delay : stats.copy_poll_delay, delay);
    deadlines.push({options.now() + delay, &n, n.generation});
  }
  void fail(node& n, grouped_coordinator::error_type error)
  {
    if (n.op->phase() == operation_phase::copy_inflight && n.op->targets[0].credit.is_settled())
      ++stats.late_copy_failures;
    n.error = std::move(error);
    n.op->set_phase(operation_phase::draining_error);
    n.op->lane->quarantine();
    n.pending = node::work::recovery;
    ++stats.recoveries;
  }
  enum class proof { event_observed, io_observed, nothing_enqueued, lane_synchronized };
  void finish(node& n, bool success, proof reason)
  {
    token(n, false);
    if (success)
      static_cast<void>(n.op->targets[0].credit.finish_success());
    else
      static_cast<void>(n.op->targets[0].credit.finish_error(n.error));
    if (reason != proof::event_observed) n.op->prove_recovery_quiescence();
    n.op->set_phase(operation_phase::terminal);
    n.pending = node::work::cleanup;
  }
  void copy(node& n)
  {
    auto& op = *n.op;
    auto& r  = *n.request;
    if (r.cancelled()) {
      ++stats.cancellations;
      n.error = std::make_error_code(std::errc::operation_canceled);
      finish(n, false, proof::io_observed);
      return;
    }
    auto const stream = (*op.lane)->stream();
    op.mark_work_started();
    auto status = detail::delay_engine_copy_for_testing(stream);
    if (status == cudaSuccess)
      status = options.transport->stream_wait_event(stream, r.slice.fence->event);
    if (status == cudaSuccess)
      status =
        options.transport->memcpy_async(op.targets[0].user_ptr,
                                        static_cast<std::byte*>(op.staging->data()) +
                                          (op.targets[0].logical.offset - op.physical.offset),
                                        op.targets[0].logical.size,
                                        cudaMemcpyDeviceToDevice,
                                        stream);
    if (status == cudaSuccess)
      status = detail::engine_copy_record_failure_for_testing()
                 ? cudaErrorNotPermitted
                 : options.transport->record_event((*op.lane)->copy_event(), stream);
    if (status == cudaSuccess && !r.slice.original_stream_was_ptds)
      status = options.transport->stream_wait_event(r.slice.user_stream, (*op.lane)->copy_event());
    if (status != cudaSuccess) {
      fail(n, status);
      return;
    }
    n.event = op.track_event((*op.lane)->copy_event(), operation_phase::copy_inflight, true);
    ++n.generation;
    poll_later(n, true);
    // All destination enqueues and the join precede the credit decrement (AM-1).
    if (!r.slice.original_stream_was_ptds) static_cast<void>(op.targets[0].credit.finish_success());
  }
  void validate(node& n)
  {
    auto& op        = *n.op;
    auto const done = op.args.bytes_done;
    auto const outcome =
      done == pending_sentinel || done < 0 || static_cast<std::size_t>(done) > op.args.size
        ? detail::completion_outcome::error
      : static_cast<std::size_t>(op.args.file_offset) + static_cast<std::size_t>(done) <
          op.targets[0].logical.end()
        ? detail::completion_outcome::short_read
        : detail::completion_outcome::validated;
    detail::observe_engine_completion_for_testing(
      {reinterpret_cast<std::uintptr_t>(&op), n.generation, done, outcome});
    if (done == pending_sentinel) {
      fail(n, std::make_error_code(std::errc::io_error));
      return;
    }
    if (done < 0 || static_cast<std::size_t>(done) > op.args.size) {
      // The I/O event proved quiescence. Retry recoverable backend errors once via sync.
      // Never feed a negative sync result back into fallback (including pread EINVAL).
      if (!n.fallback && done < 0 && done != -ENOSPC && done != -EACCES) {
        n.owner->policy->open_circuit();
        ++stats.unsupported_fallbacks;
        n.fallback = true;
        n.pending  = node::work::fallback;
        return;
      }
      n.error = std::make_error_code(std::errc::io_error);
      finish(n, false, proof::io_observed);
      return;
    }
    if (!n.fallback) {
      if (op.staging->registered())
        stats.async_registered_bytes += static_cast<std::size_t>(done);
      else
        stats.async_unregistered_bytes += static_cast<std::size_t>(done);
    }
    auto const end = static_cast<std::size_t>(op.args.file_offset) + static_cast<std::size_t>(done);
    bool const progress = end > op.valid_end;
    op.valid_end        = std::max(op.valid_end, end);
    if (op.valid_end >= op.targets[0].logical.end()) {
      copy(n);
      return;
    }
    ++stats.short_reads;
    if (!progress || ++op.attempts > config.max_short_reads) {
      n.error = std::make_error_code(std::errc::io_error);
      finish(n, false, proof::io_observed);
      return;
    }
    ++stats.retries;
    auto const restart = io_utils::align_down(op.valid_end, n.request->slice.granule);
    op.args            = {
      op.physical.end() - restart,
      static_cast<off_t>(restart),
      op.staging->buffer_offset_to_pass() + static_cast<off_t>(restart - op.physical.offset),
      pending_sentinel};
    op.set_phase(operation_phase::retry_pending);
    n.pending = options.buffered_fd(*op.file) >= 0 ? node::work::check_size
                : n.fallback                       ? node::work::fallback
                                                   : node::work::retry;
  }
  void reap(deadline const& d)
  {
    auto& n = *d.item;
    if (!n.op || n.generation != d.generation || n.pending != node::work::none) return;
    ++stats.queries;
    auto const status = options.query_event(n.event.event);
    if (status == cudaErrorNotReady) {
      poll_later(n, false);
      return;
    }
    if (status != cudaSuccess) {
      fail(n, status);
      return;
    }
    if (n.event.phase == operation_phase::io_inflight) {
      token(n, false);
      n.io_observed = options.now();
      sample(stats.completion, n.io_observed - n.started);
      sample(n.owner->completion, n.io_observed - n.started);
      validate(n);
    } else {
      [[maybe_unused]] bool const observed = n.op->observe_final_event(n.event, status);
      assert(observed);
      sample(stats.residence, options.now() - n.io_observed);
      // AM-1 already settled non-PTDS pieces. Cancellation cannot revoke an enqueued success.
      finish(n, true, proof::event_observed);
    }
  }

  // Buffered fallback is exclusively blocking-pool work; no user-stream synchronization.
  ssize_t sync_read(node& n)
  {
    auto& op          = *n.op;
    auto const handle = options.file_handle(*op.file);
    detail::before_engine_transfer_for_testing();
    auto done = options.transport->read_sync(handle,
                                             op.staging->base_ptr_to_pass(),
                                             op.args.size,
                                             op.args.file_offset,
                                             op.args.buffer_offset);
    if (done != -ENOSYS && done != -EINVAL && done != -EOPNOTSUPP) return done;
    auto const fd = options.buffered_fd(*op.file);
    if (fd < 0) return -EIO;
    auto scratch_owner = detail::lease_pinned_scratch();
    auto* scratch      = scratch_owner.get();
    ssize_t count;
    do {
      count = pread(fd, scratch, op.args.size, op.args.file_offset);
    } while (count < 0 && errno == EINTR);
    if (count < 0) count = -errno;
    if (count > 0) {
      auto status =
        cudaMemcpyAsync(static_cast<std::byte*>(op.staging->data()) +
                          (static_cast<std::size_t>(op.args.file_offset) - op.physical.offset),
                        scratch,
                        static_cast<std::size_t>(count),
                        cudaMemcpyHostToDevice,
                        (*op.lane)->stream());
      // Even enqueue errors can leave work: never free scratch without a quiescence proof.
      auto const sync = options.synchronize_lane((*op.lane)->stream());
      if (sync != cudaSuccess) {
        op.scratch = std::move(scratch_owner);  // Retain until recovery proves quiescence.
        return pending_sentinel;
      }
      if (status != cudaSuccess) count = -EIO;
    }
    return count;
  }
  void service_work()
  {
    for (auto* item : active_nodes) {
      auto& n         = *item;
      auto const work = n.pending;
      if (work == node::work::retry) {
        if (stop.load() || n.request->cancelled()) {
          n.error = std::make_error_code(std::errc::operation_canceled);
          finish(n, false, proof::io_observed);
        } else if (n.owner->effective.qd == 0) {
          n.fallback   = true;
          n.pending    = node::work::fallback;
          retry_needed = true;
        } else if (n.owner->in_io < n.owner->effective.qd)
          dispatch(n);
        continue;
      }
      if (work == node::work::none) continue;
      if (work == node::work::fallback && (stop.load() || n.request->cancelled())) {
        n.error = std::make_error_code(std::errc::operation_canceled);
        finish(n, false, proof::io_observed);
        continue;
      }
      auto* executor = work == node::work::cleanup    ? cleanup.get()
                       : work == node::work::recovery ? options.recovery.get()
                                                      : options.blocking.get();
      auto* p        = &n;
      auto self      = shared_from_this();
      bool accepted  = false;
      try {
        if (work == node::work::cleanup) {
          accepted = executor->submit([self, p] {
            static_cast<void>(cudaSetDevice(self->device));
            // P2-R19: ALL final file/input/request/scratch/lane-state releases are off reaper.
            auto const index                     = (*p->op->lane)->index();
            [[maybe_unused]] bool const released = operation_pool::try_release(*p->op);
            assert(released);
            static_cast<void>(self->options.lanes->release_quarantined_after_quiescence(index));
            self->publish({result_kind::cleaned, p, p->generation});
          });
        } else if (work == node::work::recovery) {
          accepted = executor->submit([self, p] {
            auto status = cudaSetDevice(self->device);
            if (status == cudaSuccess)
              status = self->options.synchronize_lane((*p->op->lane)->stream());
            self->publish({result_kind::recovered, p, p->generation, {}, status});
          });
        } else if (work == node::work::check_size) {
          accepted = executor->submit([self, p] {
            struct stat st{};
            bool const valid =
              fstat(self->options.buffered_fd(*p->op->file), &st) == 0 && st.st_size >= 0 &&
              static_cast<std::uint64_t>(st.st_size) >= p->op->targets[0].logical.end();
            self->publish({result_kind::size_checked,
                           p,
                           p->generation,
                           {},
                           valid ? cudaSuccess : cudaErrorUnknown});
          });
        } else {
          accepted = executor->submit([self, p] {
            auto const status = cudaSetDevice(self->device);
            try {
              p->op->args.bytes_done = status == cudaSuccess ? self->sync_read(*p) : -EIO;
            } catch (...) {
              p->op->args.bytes_done = -EIO;
            }
            self->publish({result_kind::sync_done, p, p->generation});
          });
        }
      } catch (...) { /* Queue/allocation pressure is backpressure; retry next round. */
      }
      if (accepted) {
        n.pending  = node::work::none;
        n.cleaning = work == node::work::cleanup;
      } else
        retry_needed = true;
    }
  }

  void handle(result value)
  {
    if (value.kind == result_kind::capacity) {
      if (!value.grants) {
        rebalance_running = false;
        rebalance_needed  = true;
        return;
      }
      for (std::size_t i = 0; i < value.grants->size(); ++i) {
        mounts[i]->effective = (*value.grants)[i].effective;
      }
      rebalance_running = false;
      if (mounts.size() != rebalance_count) rebalance_needed = true;
      return;
    }
    auto& n = *value.item;
    assert(n.op && value.generation == n.generation);
    switch (value.kind) {
      case result_kind::not_submitted:
        ++stats.not_submitted;
        token(n, false);
        n.fallback = true;
        if (stop.load() || n.request->cancelled()) {
          n.error = std::make_error_code(std::errc::operation_canceled);
          finish(n, false, proof::io_observed);
        } else {
          n.op->set_phase(operation_phase::host_io);
          n.pending = node::work::fallback;
        }
        break;
      case result_kind::submit_failed:
        n.retry_after_recovery = true;
        n.owner->policy->open_circuit();
        ++stats.unsupported_fallbacks;
        [[fallthrough]];
      case result_kind::failed:
        sample(stats.submit, value.elapsed);
        fail(n, value.status);
        break;
      case result_kind::recorded:
        sample(stats.submit, value.elapsed);
        n.owner->policy->observe_submit(value.elapsed);
        if (!n.owner->policy->read_eligible()) n.owner->refuse.store(true);
        n.event = n.op->track_event((*n.op->lane)->io_event(), operation_phase::io_inflight, false);
        poll_later(n, true);
        break;
      case result_kind::sync_done:
        n.io_observed = options.now();
        if (n.op->args.bytes_done > 0)
          stats.sync_bytes += static_cast<std::size_t>(n.op->args.bytes_done);
        validate(n);
        break;
      case result_kind::size_checked:
        if (value.status == cudaSuccess)
          n.pending = n.fallback ? node::work::fallback : node::work::retry;
        else {
          n.error = std::make_error_code(std::errc::io_error);
          finish(n, false, proof::io_observed);
        }
        break;
      case result_kind::recovered:
        token(n, false);
        if (value.status == cudaSuccess) {
          n.op->prove_recovery_quiescence();
          if (n.retry_after_recovery && !stop.load() && !n.request->cancelled()) {
            n.retry_after_recovery = false;
            n.fallback             = true;
            n.pending              = node::work::fallback;
          } else
            finish(n, false, proof::lane_synchronized);
        } else {
          ++stats.quarantines;
          n.op->quarantine();
          // Deliberately no credit settlement and no cleanup. Drain must report incomplete.
        }
        break;
      case result_kind::cleaned:
        if (n.gate_limited) --n.owner->gated;
        --n.owner->occupied;
        --n.request->active;
        n.op       = nullptr;
        n.cleaning = false;
        n.pending  = node::work::none;
        // requests[] retains the cursor. It is handed to blocking cleanup separately below.
        n.request.reset();
        --live;
        std::erase(active_nodes, &n);
        free_nodes.push_back(&n);
        ++stats.completed_ops;
        break;
      case result_kind::capacity: break;
    }
  }

  void accept_requests()
  {
    std::shared_ptr<cursor> r;
    while (control.pop(r)) {
      auto slot = std::find(requests.begin(), requests.end(), nullptr);
      if (slot == requests.end()) {
        requests.push_back(std::move(r));
        slot = std::prev(requests.end());
      } else
        *slot = std::move(r);
      auto& request = **slot;
      request.index = static_cast<std::size_t>(slot - requests.begin());
      try {
        auto const found = std::find_if(mounts.begin(), mounts.end(), [&](auto const& m) {
          return m->policy->key() == request.slice.policy->key();
        });
        if (found == mounts.end()) {
          auto m       = std::make_unique<mount>(request.slice.policy, mounts.size(), nodes.size());
          m->submitter = mounts.size() % submitters.size();
          m->registered_submitters = 1U << m->submitter;
          request.owner            = m.get();
          mounts.push_back(std::move(m));
          auto* p = request.owner;
          [[maybe_unused]] bool const queued =
            submitters[p->submitter]->registrations.push(std::move(p));
          if (!queued) {
            request.owner->policy->open_circuit();
            request.owner->refuse.store(true);
            throw std::runtime_error("engine mount registration ring full");
          }
          submitters[request.owner->submitter]->wake.notify();
          rebalance_needed = true;
        } else
          request.owner = found->get();
        auto const sibling = std::find_if(requests.begin(), requests.end(), [&](auto const& p) {
          return p && p.get() != &request && p->owner == request.owner && p->group == request.group;
        });
        request.group_id =
          sibling == requests.end() ? ++request.owner->next_group_id : (*sibling)->group_id;
        request.budget =
          sibling == requests.end() ? std::make_shared<request_budget>() : (*sibling)->budget;
      } catch (...) {
        request.finished = true;
        static_cast<void>(request.slice.credit.finish_error(std::current_exception()));
      }
    }
  }
  void capacity_update()
  {
    for (auto const& m : mounts) {
      bool const active = std::any_of(requests.begin(), requests.end(), [&](auto const& r) {
        return r && r->owner == m.get() && (!r->finished || r->active != 0);
      });
      if (active != m->active) {
        m->active        = active;
        rebalance_needed = true;
      }
    }
    if (!rebalance_needed || rebalance_running) return;
    std::vector<mount_capacity_demand> demands;
    std::vector<arena_mount_id> ids;
    std::vector<std::shared_ptr<mount_policy>> policies;
    for (auto const& m : mounts) {
      auto profile = m->policy->base_profile().capacity;
      if (!m->active) profile.qd = 0;
      demands.push_back({profile, 1});
      ids.push_back(m->id);
      policies.push_back(m->policy);
    }
    auto self = shared_from_this();
    auto task =
      [self, demands = std::move(demands), ids = std::move(ids), policies = std::move(policies)] {
        self->capacity_active.fetch_add(1);
        struct activity_guard {
          impl& engine;
          ~activity_guard()
          {
            engine.capacity_active.fetch_sub(1);
            engine.wake.notify();
          }
        } activity{*self};
        // A queued but not started control task may outlive an explicit test facade. It must
        // never dereference that facade's arena after stop/drain has released the bootstrap.
        if (self->stop.load()) return;
        try {
          auto grants = std::make_shared<std::vector<mount_capacity_grant>>(
            rebalance(demands,
                      self->arena_bytes,
                      static_cast<unsigned>(self->options.lanes->capacity()),
                      self->class_mask));
          for (std::size_t i = 0; i < grants->size(); ++i) {
            auto& g = (*grants)[i];
            if (demands[i].profile.qd != 0 && g.effective.qd == 0 && self->class_mask != 0) {
              // A zero async grant must NOT deadlock a handed-off slice (e.g. two mounts
              // sharing only two lanes). Sync fallback borrows one idle lane/slot, with no
              // async QD token. Physical leases still enforce the device's hard limits.
              auto const smallest = static_cast<unsigned>(std::countr_zero(self->class_mask));
              g.effective         = {0, capacity_mib << smallest, 1};
              g.slabs             = 1;
            }
            policies[i]->set_effective(g.effective);
            auto const bit =
              g.effective.op_bytes == 0
                ? 0U
                : static_cast<unsigned>(std::countr_zero(g.effective.op_bytes / capacity_mib));
            static_cast<void>(
              self->options.arena->set_quota(ids[i], g.slabs, 0, static_cast<slot_class>(bit)));
          }
          result r;
          r.kind   = result_kind::capacity;
          r.grants = std::move(grants);
          self->publish(std::move(r));
        } catch (...) {
          result failure;
          failure.kind = result_kind::capacity;
          self->publish(std::move(failure));
        }
      };
    if (options.control->submit(std::move(task))) {
      rebalance_count   = mounts.size();
      rebalance_running = true;
      rebalance_needed  = false;
    } else
      retry_needed = true;
  }
  bool has_work(mount const& m, unsigned tier)
  {
    for (auto const& r : requests) {
      if (r && !r->finished && r->owner == &m &&
          (r->slice.priority == io_priority::low ? 1U : 0U) == tier && fence_admissible(*r))
        return true;
    }
    return false;
  }
  bool fence_admissible(cursor& r)
  {
    if (!r.fence_ready &&
        (r.fence_queried == time_point{} ||
         options.now() - r.fence_queried >= std::chrono::microseconds(config.poll.fence_hint_us))) {
      r.fence_queried = options.now();
      // Hint only: the unconditional lane wait remains the correctness barrier.
      r.fence_ready = cudaEventQuery(r.slice.fence->event) != cudaErrorNotReady;
    }
    return r.fence_ready || r.owner->gated < (r.owner->effective.lanes + 7) / 8;
  }
  cursor* next_request(mount& m, unsigned tier)
  {
    auto const eligible = [&](auto const& r) {
      return r && !r->finished && r->owner == &m &&
             (r->slice.priority == io_priority::low ? 1U : 0U) == tier && fence_admissible(*r);
    };
    auto first = std::numeric_limits<std::uint64_t>::max();
    auto next  = first;
    for (auto const& r : requests)
      if (eligible(r)) {
        first = std::min(first, r->group_id);
        if (r->group_id > m.last_group[tier]) next = std::min(next, r->group_id);
      }
    bool const continuing = std::any_of(requests.begin(), requests.end(), [&](auto const& r) {
      return eligible(r) && r->group_id == m.current_group[tier];
    });
    auto const group      = continuing ? m.current_group[tier]
                            : next == std::numeric_limits<std::uint64_t>::max() ? first
                                                                                : next;
    for (std::size_t i = 0; i < requests.size(); ++i) {
      auto const index = (m.request_next[tier] + i) % requests.size();
      auto const& r    = requests[index];
      if (eligible(r) && r->group_id == group) {
        if (!continuing) {
          r->budget->deficit[tier] += static_cast<std::int64_t>(m.effective.op_bytes);
          m.last_group[tier]    = group;
          m.current_group[tier] = group;
        }
        return r.get();
      }
    }
    return nullptr;
  }
  struct admission {
    admission(bool value) : progress(value), started(value) {}
    admission(bool progress, bool started) : progress(progress), started(started) {}
    bool progress, started;
  };
  admission admit_one(mount& m, unsigned tier)
  {
    auto const effective         = m.effective;
    bool const capacity_fallback = effective.qd == 0 && effective.op_bytes != 0;
    if (effective.op_bytes == 0 || (!capacity_fallback && m.in_io >= effective.qd)) return false;
    if (m.occupied >= effective.lanes) {
      if (m.lane_wait_since == time_point{}) {
        ++stats.lane_waits;
        m.lane_wait_since = options.now();
      }
      return false;
    }
    if (tier == 1 && high_since_low < config.low_service_interval && has_work(m, 0) &&
        effective.lanes - m.occupied <= (effective.lanes + 7) / 8)
      return false;
    auto* r = next_request(m, tier);
    if (!r) return false;
    range_plan::piece_plan piece;
    try {
      piece = range_plan::next_cut(r->next,
                                   r->slice.logical.end(),
                                   effective.op_bytes,
                                   r->slice.granule,
                                   r->slice.geometry_boundaries,
                                   r->slice.file->size());
    } catch (...) {
      r->finished = true;
      static_cast<void>(r->slice.credit.finish_error(std::current_exception()));
      return true;
    }
    auto const cost = static_cast<std::int64_t>(piece.physical.size);
    if (m.deficit[tier] < cost) {
      m.deficit[tier] += static_cast<std::int64_t>(effective.op_bytes);
      return {true, false};  // New quantum, yield to the next mount before spending it.
    }
    if (r->budget->deficit[tier] < cost) {
      m.current_group[tier] = 0;
      return {true, false};  // Request's remainder carries to its NEXT DRR visit.
    }
    if (free_nodes.empty()) return false;
    auto* free = free_nodes.back();
    auto lane  = options.lanes->try_lease();
    if (!lane) {
      if (m.lane_wait_since == time_point{}) {
        ++stats.lane_waits;
        m.lane_wait_since = options.now();
      }
      return false;
    }
    auto slot = options.arena->try_lease(m.id, effective.op_bytes, true);
    if (!slot) {
      if (m.slot_wait_since == time_point{}) {
        ++stats.slot_waits;
        m.slot_wait_since = options.now();
      }
      return false;
    }
    if (m.lane_wait_since != time_point{}) {
      stats.lane_wait_ns += ns(options.now() - m.lane_wait_since);
      m.lane_wait_since = {};
    }
    if (m.slot_wait_since != time_point{}) {
      stats.slot_wait_ns += ns(options.now() - m.slot_wait_since);
      m.slot_wait_since = {};
    }
    auto* op = operations.try_acquire();
    if (!op) return false;
    auto& n = *free;
    free_nodes.pop_back();
    active_nodes.push_back(&n);
    n.op    = op;
    n.owner = &m;
    // Caller slice stays owned until all node cleanups have returned.
    n.request              = requests[r->index];
    m.request_next[tier]   = r->index + 1;
    n.fallback             = capacity_fallback;
    n.retry_after_recovery = false;
    n.gate_limited         = !r->fence_ready;
    if (n.gate_limited) ++m.gated;
    op->file      = r->slice.file;
    op->lane      = std::move(lane);
    op->staging   = std::move(slot);
    op->physical  = piece.physical;
    op->valid_end = piece.physical.offset;
    auto credit   = piece.last ? std::move(r->slice.credit) : operation_credit::add_task(r->group);
    // Keep coordinator available on cursor after moving its final credit.
    op->targets.emplace_back(
      piece.logical,
      static_cast<std::byte*>(r->slice.user_ptr) + (piece.logical.offset - r->slice.logical.offset),
      std::move(credit));
    op->args    = {piece.physical.size,
                   static_cast<off_t>(piece.physical.offset),
                   op->staging->buffer_offset_to_pass(),
                   pending_sentinel};
    r->next     = piece.logical.end();
    r->finished = piece.last;
    ++r->active;
    ++m.occupied;
    m.max_occupied = std::max(m.max_occupied, m.occupied);
    ++live;
    m.deficit[tier] -= cost;
    r->budget->deficit[tier] -= cost;
    if (r->budget->deficit[tier] == 0) m.current_group[tier] = 0;
    if (tier == 0) {
      if (high_since_low < config.low_service_interval) ++high_since_low;
    } else {
      high_since_low = 0;
    }
    if (piece.physical.empty()) {
      n.error = std::make_error_code(std::errc::io_error);
      finish(n, false, proof::nothing_enqueued);
    } else if (capacity_fallback) {
      op->set_phase(operation_phase::host_io);
      n.pending = node::work::fallback;
    } else
      dispatch(n);
    return true;
  }
  void admit()
  {
    // Piece-level mount DRR, then request round robin. Quantum is S_eff: all interior
    // pieces of a class cost exactly a quantum; head/tail pieces retain byte deficits.
    for (std::size_t round = 0; round < nodes.size(); ++round) {
      bool progressed = false;
      bool const any_high =
        std::any_of(mounts.begin(), mounts.end(), [&](auto const& m) { return has_work(*m, 0); });
      bool const any_low =
        std::any_of(mounts.begin(), mounts.end(), [&](auto const& m) { return has_work(*m, 1); });
      bool const low_due = any_low && high_since_low >= config.low_service_interval;
      // Do not let another high start indefinitely steal the low service opportunity
      // while the previous copy is releasing its reserve lane.
      auto const tier = (!any_high || low_due) ? 1U : 0U;
      for (std::size_t i = 0; i < mounts.size(); ++i) {
        auto& m           = *mounts[mount_next++ % mounts.size()];
        auto const result = admit_one(m, tier);
        if (result.progress) {
          progressed = true;
          // Spend a byte quantum, not one operation: small pieces may use its remainder.
          if (result.started && m.deficit[tier] > 0) --mount_next;
          break;
        }
      }
      if (!progressed) break;
    }
  }
  void retire_cursors()
  {
    for (auto& r : requests) {
      if (!r) continue;
      if (!r->finished && (stop.load() || r->cancelled())) {
        r->finished = true;
        ++stats.cancellations;
        static_cast<void>(
          r->slice.credit.finish_error(std::make_error_code(std::errc::operation_canceled)));
      }
      if (!r->finished || r->active != 0) continue;
      auto self = shared_from_this();
      // Move ownership BEFORE publication: even an immediately running cleanup must be
      // the last owner, never a racing reaper reset (P2-R19).
      std::shared_ptr<std::shared_ptr<cursor>> owner;
      try {
        owner  = std::make_shared<std::shared_ptr<cursor>>();
        *owner = std::move(r);
        if (!cleanup->submit([self, owner] {
              static_cast<void>(cudaSetDevice(self->device));
              owner->reset();
              self->outstanding.fetch_sub(1);
              self->wake.notify();
            }))
          r = std::move(*owner);
      } catch (...) {
        if (owner) r = std::move(*owner);
      }
    }
    while (!requests.empty() && !requests.back())
      requests.pop_back();
  }
  void update_snapshot()
  {
    if (live != 0 && options.now() - last_snapshot < 1ms) return;
    last_snapshot             = options.now();
    stats.outstanding_slices  = outstanding.load();
    stats.metadata_rejections = rejected.load();
    stats.metadata_fallbacks  = rejected.load();
    stats.live_operations     = live;
    stats.mean_storage_qd     = ns(options.now() - origin) == 0
                                  ? 0
                                  : static_cast<double>(qd_area / ns(options.now() - origin));
    stats.backlog_mean_storage_qd =
      backlog_ns == 0 ? 0 : static_cast<double>(backlog_area / backlog_ns);
    stats.lanes_by_phase.fill(0);
    for (auto const* n : active_nodes)
      if (!n->cleaning) ++stats.lanes_by_phase[static_cast<std::size_t>(n->op->phase())];
    std::unique_lock lock(snapshot_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
      try {
        stats.mounts.clear();
        for (auto const& m : mounts)
          stats.mounts.push_back({m->policy->key().mount_id,
                                  m->effective,
                                  m->in_io,
                                  m->max_io,
                                  m->occupied,
                                  m->max_occupied});
        snapshot = stats;
      } catch (...) { /* Diagnostics must not stop the reaper on host allocation failure. */
      }
    }
  }
  void drain_refused_commands(mount& m)
  {
    node* n{};
    if (!m.refuse.load() && !stop.load()) {
      bool const cancellation =
        std::any_of(active_nodes.begin(), active_nodes.end(), [&](auto const* item) {
          return !item->cleaning && item->owner == &m &&
                 item->op->phase() == operation_phase::submission_queued &&
                 item->request->cancelled();
        });
      if (!cancellation) return;
      // A facade must be able to cancel its queued commands even while this mount's
      // submitter is stuck in ANOTHER facade's call. Only the reaper produces commands,
      // so the bounded survivors always fit back into the ring.
      std::array<node*, 160> survivors{};
      std::size_t count{};
      for (unsigned scanned = 0; scanned < config.device_lane_cap && m.commands.pop(n); ++scanned) {
        if (n->request->cancelled())
          handle({result_kind::not_submitted, n, n->generation});
        else
          survivors[count++] = n;
      }
      for (std::size_t i = 0; i < count; ++i) {
        [[maybe_unused]] bool const queued = m.commands.push(std::move(survivors[i]));
        if (!queued) handle({result_kind::not_submitted, survivors[i], survivors[i]->generation});
      }
      if (count) submitters[m.submitter]->wake.notify();
      return;
    }
    // The ring is MPMC. Winning this pop proves the submitter has NOT started that command.
    // Even a submitter stuck forever in libcufile cannot strand its unstarted queue.
    while (m.commands.pop(n))
      handle({result_kind::not_submitted, n, n->generation});
  }
  void migrate_healthy_mounts(std::size_t blocked)
  {
    if (submitters.size() == 1) return;
    for (std::size_t i = 0; i < submitters.size(); ++i) {
      if (i == blocked || submitters[i]->call_start.load() >= 0) continue;
      for (auto const& m : mounts) {
        if (m->submitter != blocked || !m->policy->read_eligible()) continue;
        if ((m->registered_submitters & (1U << i)) == 0) {
          auto* p = m.get();
          if (!submitters[i]->registrations.push(std::move(p))) continue;
          m->registered_submitters |= 1U << i;
        }
        // Queue ownership migrates, not the blocked thread or any started operation.
        // Old membership is harmless: a returning submitter can help drain the same MPMC ring.
        m->submitter = i;
        submitters[i]->wake.notify();
      }
      return;
    }
  }
  void run() noexcept
  {
    static_cast<void>(prctl(PR_SET_TIMERSLACK, 1000UL, 0UL, 0UL, 0UL));
    auto const status = cudaSetDevice(device);
    if (status != cudaSuccess) stop.store(true);
    for (;;) {
      auto const iteration_start = options.now();
      retry_needed               = false;
      try {
        account(options.now());
        accept_requests();
        result value;
        while (completions.pop(value)) {
          try {
            handle(std::move(value));
          } catch (...) {
            if (value.item && value.item->op && !value.item->cleaning)
              fail(*value.item, std::current_exception());
            else
              throw;
          }
        }
        auto const now = options.now();
        for (unsigned count = 0; count < config.poll.due_events_per_round && !deadlines.empty() &&
                                 deadlines.top().due <= now;
             ++count) {
          auto d = deadlines.top();
          deadlines.pop();
          try {
            reap(d);
          } catch (...) {
            fail(*d.item, std::current_exception());
          }
        }
        for (auto const& s : submitters) {
          auto const start = s->call_start.load(std::memory_order_acquire);
          auto* const m    = s->calling.load();
          if (start >= 0 && m && start == s->call_start.load(std::memory_order_acquire)) {
            m->policy->observe_stuck_call(now.time_since_epoch() - policy_clock::duration(start));
            if (!m->policy->read_eligible()) {
              m->refuse.store(true);
              auto const position = static_cast<std::size_t>(&s - submitters.data());
              migrate_healthy_mounts(position);
            }
          }
        }
        for (auto const& m : mounts) {
          m->policy->poll_timers();
          m->refuse.store(!m->policy->read_eligible());
          drain_refused_commands(*m);
          if (m->in_io == 0 && m->policy->state() == mount_state::circuit_open)
            m->policy->begin_cooldown();
        }
        retire_cursors();
        if (!stop.load()) capacity_update();
        service_work();
        if (!stop.load()) admit();
        ++stats.reaper_iterations;
        stats.reaper_active_ns += ns(options.now() - iteration_start);
        update_snapshot();
      } catch (...) {
        auto const error = std::current_exception();
        for (auto const& r : requests)
          if (r && !r->finished) {
            r->finished = true;
            static_cast<void>(r->slice.credit.finish_error(error));
          }
      }
      if (stop.load() && handoffs.load() == 0 && outstanding.load() == 0 && live == 0 &&
          capacity_active.load() == 0)
        break;
      auto due = options.now() + std::chrono::milliseconds(config.poll.idle_health_ms);
      for (auto const& s : submitters)
        if (s->call_start.load() >= 0) due = std::min(due, options.now() + 1ms);
      if (!deadlines.empty()) due = std::min(due, deadlines.top().due);
      // Pending capacity/cleanup tasks may have been rejected transiently. Retry without spin.
      // Ordinary backlog waits for completion. Only rejected executor submits need 100 us.
      if (retry_needed) due = std::min(due, options.now() + 100us);
      wake.wait(due - options.now());
    }
    stop_submitters.store(true);
    for (auto const& s : submitters)
      s->wake.notify();
    exited.store(true, std::memory_order_release);
  }
};

device_engine::device_engine(int device, async_engine_config config, device_engine_options options)
{
  config.validate();
  if (device < 0 || config.mode == engine_mode::legacy || !options.arena || !options.lanes ||
      !options.transport || !options.now || !options.query_event || !options.synchronize_lane ||
      options.lanes->capacity() > config.device_lane_cap)
    throw std::invalid_argument("device_engine requires enabled mode and prewarmed resources");
  if (options.lanes->capacity() < 2 || options.arena->available_class_mask() == 0 ||
      options.arena->gauges().registered_bytes < device_arena::slab_bytes)
    throw std::runtime_error("device_engine lacks staged-read capacity; keep legacy routing");
  _impl = std::make_shared<impl>(device, std::move(config), std::move(options));
  _impl->start();
}

device_engine::~device_engine()
{
  request_stop();
  if (!drain(std::chrono::milliseconds(100))) {
    // The reaper's self-reference retains all state, including joinable stuck submitters.
    // No detach, no free, no fake completion. Production instances are immortal regardless.
    std::fprintf(
      stderr, "cuFile engine device %d: incomplete drain; resources retained\n", _impl->device);
    return;
  }
  if (_impl->reaper.joinable()) _impl->reaper.join();
  for (auto& s : _impl->submitters)
    if (s->thread.joinable()) s->thread.join();
  _impl->incomplete_owner.reset();
}

bool device_engine::ready() const noexcept
{
  return _impl && !_impl->stop.load(std::memory_order_acquire);
}

bool device_engine::try_reserve_slice() noexcept
{
  auto& e    = *_impl;
  auto count = e.outstanding.load();
  do {
    if (count >= e.config.max_pending_slices_per_device) {
      e.rejected.fetch_add(1);
      return false;
    }
  } while (!e.outstanding.compare_exchange_weak(count, count + 1));
  return true;
}
void device_engine::release_slice_reservation() noexcept
{
  _impl->outstanding.fetch_sub(1);
  notify();
}
void device_engine::hand_off(engine_slice slice, bool reserved) noexcept
{
  auto& e = *_impl;
  if (!reserved && !try_reserve_slice()) {
    static_cast<void>(slice.credit.finish_error(std::make_error_code(std::errc::no_buffer_space)));
    return;
  }
  e.handoffs.fetch_add(1);
  struct exit_guard {
    impl& engine;
    ~exit_guard()
    {
      engine.handoffs.fetch_sub(1);
      engine.wake.notify();
    }
  } guard{e};
  try {
    if (auto group = slice.credit.coordinator())
      slice.completion_retirement = std::make_shared<completion_retirement>(std::move(group));
  } catch (...) {
    static_cast<void>(slice.credit.finish_error(std::current_exception()));
    release_slice_reservation();
    return;
  }
  if (e.stop.load()) {
    static_cast<void>(
      slice.credit.finish_error(std::make_error_code(std::errc::operation_canceled)));
    release_slice_reservation();
    return;
  }
  try {
    if (!slice.credit.coordinator() || !slice.file || !slice.policy || !slice.fence ||
        !slice.fence->event || !slice.user_ptr || slice.device_id != e.device ||
        slice.user_stream == nullptr || slice.user_stream == cudaStreamPerThread ||
        slice.granule == 0 || slice.logical.empty() ||
        slice.logical.offset > static_cast<std::size_t>(std::numeric_limits<off_t>::max()) ||
        slice.logical.size >
          static_cast<std::size_t>(std::numeric_limits<off_t>::max()) - slice.logical.offset ||
        !std::is_sorted(slice.geometry_boundaries.begin(), slice.geometry_boundaries.end()))
      throw std::invalid_argument("invalid engine_slice");
    auto r = std::make_shared<impl::cursor>(std::move(slice));
    if (!e.control.push(std::move(r))) {
      e.rejected.fetch_add(1);
      static_cast<void>(
        r->slice.credit.finish_error(std::make_error_code(std::errc::no_buffer_space)));
      e.outstanding.fetch_sub(1);
      return;
    }
    e.wake.notify();
  } catch (...) {
    static_cast<void>(slice.credit.finish_error(std::current_exception()));
    e.outstanding.fetch_sub(1);
  }
}
void device_engine::notify() noexcept { _impl->wake.notify(); }
void device_engine::request_stop() noexcept
{
  _impl->stop.store(true);
  notify();
}
bool device_engine::drain(std::chrono::milliseconds timeout) noexcept
{
  auto const end = policy_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
  do {
    if (_impl->outstanding.load() == 0 &&
        (!_impl->stop.load() || _impl->exited.load(std::memory_order_acquire)))
      return true;
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  } while (policy_clock::now() < end);
  return false;
}
engine_gauges device_engine::gauges() const
{
  std::lock_guard lock(_impl->snapshot_mutex);
  return _impl->snapshot;
}
device_engine* device_engine::try_get(int device) noexcept
{
  try {
    auto& registry = engine_registry::instance();
    auto bindings  = registry.published.load();
    return device >= 0 && bindings && static_cast<std::size_t>(device) < bindings->size()
             ? (*bindings)[static_cast<std::size_t>(device)]
             : nullptr;
  } catch (...) {
    return nullptr;
  }
}
device_engine& device_engine::for_device(int device, async_engine_config const& config)
{
  auto& registry = engine_registry::instance();
  std::lock_guard lock(registry.mutex);
  auto const found = registry.engines.find(device);
  if (found != registry.engines.end()) {
    if (!(found->second->_impl->config == config) && !registry.warned[device]) {
      registry.warned[device] = true;
      CUCASCADE_LOG_WARN("cuFile engine device {}: configuration differs; first configuration wins",
                         device);
    }
    return *found->second;
  }
  config.validate();
  if (config.mode == engine_mode::legacy)
    throw std::invalid_argument("device_engine bootstrap requires an enabled engine mode");
  auto& rt = runtime::instance();
  static_cast<void>(rt.ensure_initialized());
  if (!rt.engine_possible(config.mode)) throw std::runtime_error("cuFile async engine unavailable");
  auto& arena = device_arena::for_device(device);
  static_cast<void>(arena.warm_up(config));
  device_engine_options options;
  options.arena     = &arena;
  options.lanes     = std::make_shared<lane_pool>(device, config);
  options.transport = make_cufile_transport();
  auto engine       = std::make_unique<device_engine>(device, config, std::move(options));
  auto* ptr         = engine.get();
  auto old          = registry.published.load();
  auto bindings =
    std::make_shared<std::vector<device_engine*>>(old ? *old : std::vector<device_engine*>{});
  if (bindings->size() <= static_cast<std::size_t>(device))
    bindings->resize(static_cast<std::size_t>(device) + 1);
  (*bindings)[static_cast<std::size_t>(device)] = ptr;
  registry.engines.emplace(device, std::move(engine));
  registry.published.store(std::move(bindings));
  return *ptr;
}
}  // namespace cucascade::io::cufile
