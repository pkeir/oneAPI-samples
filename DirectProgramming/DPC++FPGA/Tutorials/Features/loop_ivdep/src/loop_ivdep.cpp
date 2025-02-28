//==============================================================
// Copyright Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================
#include <CL/sycl.hpp>
#include <CL/sycl/INTEL/fpga_extensions.hpp>
#include <iomanip>
#include <iostream>

// dpc_common.hpp can be found in the dev-utilities include folder.
// e.g., $ONEAPI_ROOT/dev-utilities//include/dpc_common.hpp
#include "dpc_common.hpp"

constexpr size_t kRowLength = 128;
constexpr size_t kMinSafelen = 1;
constexpr size_t kMaxSafelen = kRowLength;
constexpr size_t kMatrixSize = kRowLength * kRowLength;

using namespace sycl;

// Forward declare the kernel name in the global scope.
// This FPGA best practice reduces name mangling in the optimization reports.
template <size_t safe_len> class KernelCompute;

template <size_t safe_len>
void TransposeAndFold(const device_selector &selector,
                      const std::array<float, kMatrixSize> &m_input,
                      std::array<float, kMatrixSize> &m_output) {
  double kernel_time = 0;
  try {
  queue q(selector, dpc_common::exception_handler,
          property::queue::enable_profiling{});

    buffer buffer_input(m_input);
    buffer buffer_output(m_output);

    event e = q.submit([&](handler &h) {
      accessor accessor_input(buffer_input, h, read_only);
      accessor accessor_output(buffer_output, h, write_only, no_init);

      h.single_task<KernelCompute<safe_len>>([=]()
                                             [[intel::kernel_args_restrict]] {
        float in_buffer[kRowLength][kRowLength];
        float temp_buffer[kRowLength][kRowLength];

        // Initialize local buffers
        for (size_t i = 0; i < kMatrixSize; i++) {
          in_buffer[i / kRowLength][i % kRowLength] = accessor_input[i];
          temp_buffer[i / kRowLength][i % kRowLength] = 0;
        }

        // No iterations of the following loop store data into the same memory
        // location that are less than kRowLength iterations apart.
        // The ivdep here instructs the compiler that it can safely assume no
        // loop-carried dependencies over safe_len consecutive iterations.
        [[intel::ivdep(safe_len)]]
        for (size_t j = 0; j < kMatrixSize * kRowLength; j++) {
          #pragma unroll
          for (size_t i = 0; i < kRowLength; i++) {
            temp_buffer[j % kRowLength][i] += in_buffer[i][j % kRowLength];
          }
        }

        // Write result to output
        for (size_t i = 0; i < kMatrixSize; i++) {
          accessor_output[i] = temp_buffer[i / kRowLength][i % kRowLength];
        }
      });
    });

    double start = e.get_profiling_info<info::event_profiling::command_start>();
    double end = e.get_profiling_info<info::event_profiling::command_end>();

    // unit is nano second, convert to ms
    kernel_time = (double)(end - start) * 1e-6;

  } catch (sycl::exception const &e) {
    // Catches exceptions in the host code
    std::cerr << "Caught a SYCL host exception:\n" << e.what() << "\n";

    // Most likely the runtime couldn't find FPGA hardware!
    if (e.get_cl_code() == CL_DEVICE_NOT_FOUND) {
      std::cerr << "If you are targeting an FPGA, please ensure that your "
                   "system has a correctly configured FPGA board.\n";
      std::cerr << "Run sys_check in the oneAPI root directory to verify.\n";
      std::cerr << "If you are targeting the FPGA emulator, compile with "
                   "-DFPGA_EMULATOR.\n";
    }
    std::terminate();
  }

  std::cout << "safe_len: " << safe_len << " -- kernel time : " << kernel_time
            << " ms\n";
  std::cout << "Throughput for kernel with safe_len " << safe_len << ": ";
  std::cout << std::fixed << std::setprecision(0)
            << (((double)kMatrixSize * sizeof(float) * 1e-3f) /
                (kernel_time * 1e-3f)) << "KB/s\n";
}

int main() {
  std::array<float, kMatrixSize> A, B, C;

  // Initialize input with random data
  for (size_t i = 0; i < kMatrixSize; i++) {
    A[i] = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
  }

#if defined(FPGA_EMULATOR)
  INTEL::fpga_emulator_selector selector;
#else
  INTEL::fpga_selector selector;
#endif

  // Instantiate kernel logic with the min and max correct safelen parameter
  // to compare performance.
  TransposeAndFold<kMinSafelen>(selector, A, B);
  TransposeAndFold<kMaxSafelen>(selector, A, C);
  // You can also try removing the ivdep from the kernel entirely and
  // recompiling to see what effect this has on performance.

  // Verify result
  for (size_t i = 0; i < kMatrixSize; i++) {
    if (B[i] != C[i]) {
      std::cout << "FAILED: The results are incorrect" << '\n';
      return 1;
    }
  }
  std::cout << "PASSED: The results are correct" << '\n';
  return 0;
}
