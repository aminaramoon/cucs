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

#include <cucascade/exec/thread_util.hpp>
#include <cucascade/io/cufile/placement.hpp>
#include <cucascade/memory/net_topology.hpp>
#include <cucascade/memory/topology_discovery.hpp>

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace cucascade::io::cufile {

namespace {

/// A PCI address, comparable whatever its spelling: NVML writes the domain with
/// eight hex digits ("00000000:1B:00.0"), CUDA and sysfs with four.
struct pci_address {
  unsigned domain{0};
  unsigned bus{0};
  unsigned device{0};
  unsigned function{0};

  bool operator==(pci_address const&) const = default;
};

[[nodiscard]] std::optional<pci_address> parse_pci(std::string_view text) noexcept
{
  // domain ':' bus ':' device '.' function, all hex.
  pci_address out;
  std::array<unsigned*, 4> const fields{&out.domain, &out.bus, &out.device, &out.function};
  std::array<char, 3> const separators{':', ':', '.'};
  auto const* cursor = text.data();
  auto const* end    = text.data() + text.size();
  for (std::size_t i = 0; i < fields.size(); ++i) {
    auto const [next, ec] = std::from_chars(cursor, end, *fields[i], 16);
    if (ec != std::errc{} || next == cursor) return std::nullopt;
    cursor = next;
    if (i < separators.size()) {
      if (cursor == end || *cursor != separators[i]) return std::nullopt;
      ++cursor;
    }
  }
  return out;
}

[[nodiscard]] memory::gpu_topology_info const* find_gpu(memory::system_topology_info const* topology,
                                                        std::string const& pci_bus_id) noexcept
{
  if (topology == nullptr) return nullptr;
  auto const wanted = parse_pci(pci_bus_id);
  if (!wanted) return nullptr;
  for (auto const& gpu : topology->gpus) {
    if (parse_pci(gpu.pci_bus_id) == wanted) return &gpu;
  }
  return nullptr;
}

/// Reactors per GPU and why.
[[nodiscard]] std::size_t reactors_per_device(config const& cfg, std::string& why)
{
  if (cfg.reactors_per_device != 0) {
    why = std::to_string(cfg.reactors_per_device) + " reactors (configured)";
    return cfg.reactors_per_device;
  }
  if (cfg.storage_hint_path.empty()) {
    why = std::to_string(default_reactors_per_device) +
          " reactors (storage unknown: no storage_hint_path)";
    return default_reactors_per_device;
  }
  auto const storage = memory::topology_discovery::discover_storage_for_path(cfg.storage_hint_path);
  switch (storage.kind) {
    case memory::storage_kind::network:
      why = std::to_string(network_reactors_per_device) + " reactors (" +
            storage.filesystem_type + " on " + storage.mount_point +
            " is a network filesystem: deep queues hide its latency)";
      return network_reactors_per_device;
    case memory::storage_kind::local_block:
      why = std::to_string(local_reactors_per_device) + " reactors (" + storage.filesystem_type +
            " on " + storage.mount_point + " is local block storage)";
      return local_reactors_per_device;
    default: break;
  }
  why = std::to_string(default_reactors_per_device) + " reactors (storage of '" +
        cfg.storage_hint_path + "' unknown" +
        (storage.reason.empty() ? std::string{} : ": " + storage.reason) + ")";
  return default_reactors_per_device;
}

[[nodiscard]] std::vector<int> intersect_in_order(std::vector<int> const& cpus,
                                                  std::vector<int> const& allowed)
{
  if (allowed.empty()) return cpus;  // unknown inherited affinity: trust the GPU's
  std::vector<int> out;
  for (int cpu : cpus) {
    if (std::find(allowed.begin(), allowed.end(), cpu) != allowed.end()) out.push_back(cpu);
  }
  return out;
}

}  // namespace

pool_plan plan_cufile_pool(config const& cfg,
                           memory::system_topology_info const* topology,
                           platform_info const& platform) noexcept
{
  pool_plan plan;
  try {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) {
      std::ignore        = cudaGetLastError();
      plan.flat_reactors = cfg.reactors_per_device != 0 ? cfg.reactors_per_device
                                                        : default_reactors_per_device;
      plan.reason = "no CUDA device visible: one flat pool of " +
                    std::to_string(plan.flat_reactors) + " reactors";
      return plan;
    }

    std::string sizing;
    auto const per_device = reactors_per_device(cfg, sizing);
    bool const gds        = platform.driver_opened && platform.properties_available &&
                     !platform.compat_only && !platform.forced_compat;
    bool const stage = cfg.staging_mode == staging::always ||
                       (cfg.staging_mode == staging::automatic && gds);
    auto const allowed        = exec::thread_util::get_current_thread_affinity();
    auto const slot_footprint = cfg.staging_slot_bytes() + (64UL << 10);

    for (int device = 0; device < count; ++device) {
      device_group_plan group;
      group.device     = device;
      group.n_reactors = per_device;
      std::array<char, 32> pci{};
      if (cudaDeviceGetPCIBusId(pci.data(), static_cast<int>(pci.size()), device) ==
          cudaSuccess) {
        group.pci_bus_id = pci.data();
      } else {
        std::ignore = cudaGetLastError();
      }
      auto const* gpu = find_gpu(topology, group.pci_bus_id);
      if (gpu != nullptr) group.numa_node = gpu->numa_node;

      std::string placement;
      if (!cfg.pin_to_device_cpus) {
        placement = "unpinned (pin_to_device_cpus off)";
      } else if (gpu == nullptr) {
        placement = topology == nullptr ? "unpinned (no topology)"
                                        : "unpinned (GPU not found in the topology)";
      } else if (gpu->cpu_cores.empty()) {
        placement = "unpinned (the GPU reports no local CPUs)";
      } else {
        group.cpus = intersect_in_order(gpu->cpu_cores, allowed);
        placement  = group.cpus.empty()
                       ? "unpinned (the GPU's CPUs " + memory::format_cpu_list(gpu->cpu_cores) +
                          " are outside the inherited affinity)"
                       : "pinned to CPUs " + memory::format_cpu_list(group.cpus) + " (NUMA node " +
                          std::to_string(gpu->numa_node) + ")";
      }

      std::string staging_why;
      if (!stage) {
        staging_why = cfg.staging_mode == staging::never
                        ? "no staging (staging_mode never)"
                        : "no staging (cuFile reports no GPUDirect Storage mode)";
      } else {
        std::size_t cap = cfg.max_staging_bytes_per_device;
        auto tighten    = [&cap](std::size_t limit) {
          if (limit != 0 && (cap == 0 || limit < cap)) cap = limit;
        };
        if (gpu != nullptr) tighten(gpu->bar1_free_bytes / 2);
        tighten(platform.max_device_pinned_mem_size_kb * 1024);
        group.staging_cap_bytes = cap;
        auto wanted             = per_device * cfg.staging_slots_per_reactor;
        if (cap != 0) wanted = std::min(wanted, cap / slot_footprint);
        group.staging_slots = wanted;
        staging_why         = std::to_string(wanted) + " staging slots of " +
                      std::to_string(cfg.staging_slot_bytes() >> 10) + " KiB" +
                      (cap != 0 ? " (cap " + std::to_string(cap >> 20) + " MiB)" : "");
        if (wanted < per_device * cfg.staging_slots_per_reactor) {
          staging_why += "; fewer than reactors x slots: the cap, BAR1 or cuFile's pinned limit "
                         "binds, and reactors without a slot read directly";
        }
      }
      group.reason = sizing + ", " + placement + ", " + staging_why;
      plan.groups.push_back(std::move(group));
    }
    plan.reason = std::to_string(plan.groups.size()) + " GPU group(s), " +
                  std::to_string(plan.total_reactors()) + " reactors";
  } catch (std::exception const& e) {
    plan.groups.clear();
    plan.flat_reactors = default_reactors_per_device;
    plan.reason        = std::string{"planning failed ("} + e.what() + "): one flat pool";
  } catch (...) {
    plan.groups.clear();
    plan.flat_reactors = default_reactors_per_device;
    plan.reason        = "planning failed: one flat pool";
  }
  return plan;
}

std::string describe(pool_plan const& plan)
{
  std::string out = "cufile pool: " + plan.reason;
  for (auto const& group : plan.groups) {
    out += "; GPU " + std::to_string(group.device) + " (" + group.pci_bus_id + "): " + group.reason;
  }
  return out;
}

}  // namespace cucascade::io::cufile
