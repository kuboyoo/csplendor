// Isolated buffer comparison, NOT an engine correctness or performance gate.
// No fixed wide-Action array is installed in the engine.
#include "reveal_solver_components.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

using Clock = std::chrono::steady_clock;
using csplendor::solver_internal::ActionOrderKey;
using csplendor::solver_internal::OracleActionMetadata;
struct WideAction : ActionOrderKey, OracleActionMetadata {};

template <int Mode> void sample(int count, int repeat) {
  std::vector<int> reused;
  uint64_t digest = 0;
  const auto start = Clock::now();
  for (int iteration = 0; iteration < repeat; ++iteration) {
    std::vector<int> fresh;
    std::array<int, 40> fixed;
    auto &values = Mode == 0 ? fresh : reused;
    values.clear();
    for (int i = 0; i < count; ++i) {
      const int value = (i * 17 + iteration * 7) % 97;
      if constexpr (Mode == 2)
        fixed[i] = value;
      else
        values.push_back(value);
    }
    auto *data = Mode == 2 ? fixed.data() : values.data();
    std::sort(data, data + count);
    for (int i = 0; i < count; ++i)
      digest += static_cast<uint64_t>(data[i]) * (i + 1);
  }
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
  std::cout << "{\"mode\":" << Mode << ",\"count\":" << count
            << ",\"repeat\":" << repeat << ",\"elapsed_ns\":" << ns
            << ",\"digest\":" << digest << "}\n";
}

int main() {
  std::cout << "{\"wide_action_bytes\":" << sizeof(WideAction)
            << ",\"fixed_2048_action_bytes_per_frame\":" << sizeof(WideAction) * 2048
            << ",\"fixed_40_id_bytes\":" << sizeof(int) * 40 << "}\n";
  for (int batch = 0; batch < 6; ++batch)
    for (int count : {1, 16, 40}) {
      if (batch % 2 == 0) {
        sample<0>(count, 100000);
        sample<1>(count, 100000);
        sample<2>(count, 100000);
      } else {
        sample<2>(count, 100000);
        sample<1>(count, 100000);
        sample<0>(count, 100000);
      }
    }
}
