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

#include <cufile.h>

#include <optional>

namespace cucascade::io::cufile {

/**
 * @brief The cuFile entry points cuCascade calls, resolved at run time.
 *
 * libcufile.so.0 is loaded with `dlopen(RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE)` on first use and
 * never linked, so binaries run on hosts without it: @ref get() then returns null and every cuFile
 * path falls back to POSIX I/O.  `cufile.h` is a build-time dependency only.  `RTLD_NODELETE`
 * keeps the library mapped for the life of the process (it can crash at `dlclose` with statistics
 * enabled).
 *
 * The eight legacy entry points are required. Async reads, async writes, stream registration,
 * batch I/O and parameters are independent optional groups. Check the corresponding capability
 * before calling an optional member. Loading the table does not open the driver or call setters.
 *
 * This is a private detail of the cuFile backend; nothing outside the backend and its tests
 * should include this header.
 */
struct cufile_api {
  decltype(&cuFileDriverOpen) driver_open{nullptr};
  decltype(&cuFileDriverGetProperties) driver_get_properties{nullptr};
  decltype(&cuFileHandleRegister) handle_register{nullptr};
  decltype(&cuFileHandleDeregister) handle_deregister{nullptr};
  decltype(&cuFileBufRegister) buf_register{nullptr};
  decltype(&cuFileBufDeregister) buf_deregister{nullptr};
  decltype(&cuFileRead) read{nullptr};
  decltype(&cuFileWrite) write{nullptr};

  decltype(&cuFileReadAsync) read_async{nullptr};
  decltype(&cuFileWriteAsync) write_async{nullptr};
  decltype(&cuFileStreamRegister) stream_register{nullptr};
  decltype(&cuFileStreamDeregister) stream_deregister{nullptr};
  decltype(&cuFileBatchIOSetUp) batch_setup{nullptr};
  decltype(&cuFileBatchIOSubmit) batch_submit{nullptr};
  decltype(&cuFileBatchIOGetStatus) batch_get_status{nullptr};
  decltype(&cuFileBatchIOCancel) batch_cancel{nullptr};
  decltype(&cuFileBatchIODestroy) batch_destroy{nullptr};
  bool has_async_read{false};
  bool has_async_write{false};
  bool has_stream_register{false};
  bool has_batch{false};
  bool has_parameters{false};

  /// Resolver test seam: the context belongs to the caller; no global loader state is changed.
  using symbol_resolver = void* (*)(void* context, char const* name) noexcept;

  /**
   * @brief Resolve a table without opening a library or calling any resolved function.
   * @return nullopt for a null resolver or any missing required symbol. Optional groups are
   *         independent; parameter pointers remain individually usable when their group is partial.
   */
  [[nodiscard]] static std::optional<cufile_api> resolve_symbols(symbol_resolver resolver,
                                                                 void* context) noexcept;

  /**
   * @brief The process-wide table, loaded on first call.
   *
   * Thread-safe; never throws.
   *
   * @return The table, or null when libcufile cannot be loaded or lacks a required symbol (the
   *         reason is logged once).
   */
  [[nodiscard]] static cufile_api const* get() noexcept;

  /**
   * @brief Open the cuFile driver once per process.
   *
   * Later calls return the first call's outcome.  The driver is never closed: cuFile releases it
   * at process exit.  Thread-safe; never throws.
   *
   * @return Whether `cuFileDriverOpen` succeeded (false when the library is unavailable).
   */
  [[nodiscard]] static bool open_driver() noexcept;

  /**
   * @brief `cuFileGetVersion`, e.g. 1170 for libcufile 1.17.
   *
   * @return The version, or 0 when the library is unavailable or does not report one.
   */
  [[nodiscard]] static int version() noexcept;

  // Keep header-dependent members last so the common entry-point and capability offsets stay
  // stable. All TUs still need matching CUCASCADE_CUFILE_HAS_* definitions and compatible headers:
  // a stable prefix does not make differently configured struct definitions ODR-safe.
#ifdef CUCASCADE_CUFILE_HAS_CUFILESETPARAMETERSIZET
  decltype(&cuFileSetParameterSizeT) set_parameter_size_t{nullptr};
#endif
#ifdef CUCASCADE_CUFILE_HAS_CUFILESETPARAMETERBOOL
  decltype(&cuFileSetParameterBool) set_parameter_bool{nullptr};
#endif
#ifdef CUCASCADE_CUFILE_HAS_CUFILESETPARAMETERSTRING
  decltype(&cuFileSetParameterString) set_parameter_string{nullptr};
#endif
#ifdef CUCASCADE_CUFILE_HAS_CUFILEGETPARAMETERSIZET
  decltype(&cuFileGetParameterSizeT) get_parameter_size_t{nullptr};
#endif
#ifdef CUCASCADE_CUFILE_HAS_CUFILEGETPARAMETERBOOL
  decltype(&cuFileGetParameterBool) get_parameter_bool{nullptr};
#endif
#ifdef CUCASCADE_CUFILE_HAS_CUFILEGETPARAMETERSTRING
  decltype(&cuFileGetParameterString) get_parameter_string{nullptr};
#endif
#ifdef CUCASCADE_CUFILE_HAS_CUFILEGETPARAMETERMINMAXVALUE
  decltype(&cuFileGetParameterMinMaxValue) get_parameter_min_max_value{nullptr};
#endif
#ifdef CUCASCADE_CUFILE_HAS_CUFILESETPARAMETERPOSIXPOOLSLABARRAY
  decltype(&cuFileSetParameterPosixPoolSlabArray) set_parameter_posix_pool_slab_array{nullptr};
#endif
#ifdef CUCASCADE_CUFILE_HAS_CUFILEGETPARAMETERPOSIXPOOLSLABARRAY
  decltype(&cuFileGetParameterPosixPoolSlabArray) get_parameter_posix_pool_slab_array{nullptr};
#endif
#ifdef CUCASCADE_CUFILE_HAS_CUFILESETPARAMETERGPUBOUNCEBUFFERSLABARRAY
  decltype(&cuFileSetParameterGpuBounceBufferSlabArray) set_parameter_gpu_bounce_buffer_slab_array{
    nullptr};
#endif
#ifdef CUCASCADE_CUFILE_HAS_CUFILEGETPARAMETERGPUBOUNCEBUFFERSLABARRAY
  decltype(&cuFileGetParameterGpuBounceBufferSlabArray) get_parameter_gpu_bounce_buffer_slab_array{
    nullptr};
#endif
};

}  // namespace cucascade::io::cufile
