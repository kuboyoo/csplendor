#ifndef CSPLENDOR_SOLVER_MEMORY_BUDGET_H
#define CSPLENDOR_SOLVER_MEMORY_BUDGET_H

// Memory budget of the searches' tables (doc/mate_usage.md「探索のメモリ
// 予算」): a quarter of the free memory, clamped and split across jobs, turned
// into a transposition-table cap once at the start of a search. Header-only so
// that every build (Python module, WASM, native tests) compiles it unchanged.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#if defined(__EMSCRIPTEN__)
// No portable way to see the device's memory: the JS caller passes a budget.
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#include <string>
#endif

namespace csplendor {

inline constexpr uint64_t MEMORY_BUDGET_FRACTION_DEN = 4;
inline constexpr uint64_t MEMORY_BUDGET_MIN_BYTES = uint64_t{64} << 20;
inline constexpr uint64_t MEMORY_BUDGET_MAX_BYTES = uint64_t{2} << 30;
// Budget bytes per transposition-table entry: the exact and blank tables at
// load 0.7 (2 x 91), the last rehash (C/8) and the two line tables
// (bit_floor(N) 16-byte slots each, with their doubling) stay below it.
inline constexpr uint64_t MEMORY_BYTES_PER_MEMO_STATE = 256;
// Free memory unknown: 2,000,000 states, the former fixed default. WASM: a
// quarter of the default 2 GiB linear-memory maximum, beside the net and tree.
#if defined(__EMSCRIPTEN__)
inline constexpr uint64_t MEMORY_BUDGET_FALLBACK_BYTES = uint64_t{256} << 20;
#else
inline constexpr uint64_t MEMORY_BUDGET_FALLBACK_BYTES =
    2'000'000 * MEMORY_BYTES_PER_MEMO_STATE;
#endif

namespace memory_budget_detail {

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
// First number in `path` after `prefix` at the start of a line (or of the
// file when prefix is empty); false when absent or not a number ("max").
inline bool read_number(const std::string &path, const std::string &prefix,
                        uint64_t &value) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.compare(0, prefix.size(), prefix) != 0)
      continue;
    const size_t begin = line.find_first_of("0123456789", prefix.size());
    if (begin == std::string::npos)
      return false;
    value = std::stoull(line.substr(begin));
    return true;
  }
  return false;
}

// Least memory.max - memory.current over this process's cgroup v2 and its
// ancestors up to the mount root (a container's own limit sits there);
// UINT64_MAX when none is limited.
inline uint64_t cgroup_headroom_bytes() {
  std::ifstream in("/proc/self/cgroup");
  std::string line, path;
  bool found = false;
  while (std::getline(in, line))
    if (line.compare(0, 3, "0::") == 0) {
      path = line.substr(3);
      found = true;
    }
  uint64_t headroom = UINT64_MAX;
  if (!found)
    return headroom;
  if (path == "/")
    path.clear();
  for (;;) {
    const std::string dir = "/sys/fs/cgroup" + path;
    uint64_t max = 0, current = 0;
    if (read_number(dir + "/memory.max", "", max) &&
        read_number(dir + "/memory.current", "", current))
      headroom = std::min(headroom, max > current ? max - current : 0);
    if (path.empty())
      return headroom;
    path.erase(path.find_last_of('/'));
  }
}
#endif

} // namespace memory_budget_detail

// Free memory this process may use, 0 when unknown.
inline uint64_t available_memory_bytes() noexcept {
#if defined(__EMSCRIPTEN__)
  return 0;
#elif defined(_WIN32)
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
#elif defined(__APPLE__)
  vm_statistics64_data_t vm;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS)
    return 0;
  vm_size_t page = 0;
  if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS)
    return 0;
  return (uint64_t{vm.free_count} + vm.inactive_count + vm.purgeable_count) * page;
#elif defined(__linux__)
  try {
    uint64_t kib = 0;
    if (!memory_budget_detail::read_number("/proc/meminfo", "MemAvailable:", kib))
      return 0;
    // Known, so at least 1 (0 would read as unknown and pick the fallback).
    return std::max<uint64_t>(
        1, std::min(kib * 1024, memory_budget_detail::cgroup_headroom_bytes()));
  } catch (...) {
    return 0;
  }
#else
  return 0;
#endif
}

// A quarter of `available_bytes` clamped to [MIN, MAX] (the fallback when 0),
// split across `jobs`.
inline uint64_t solver_memory_budget_bytes(uint64_t available_bytes,
                                           unsigned jobs = 1) noexcept {
  const uint64_t total =
      available_bytes == 0
          ? MEMORY_BUDGET_FALLBACK_BYTES
          : std::clamp(available_bytes / MEMORY_BUDGET_FRACTION_DEN,
                       MEMORY_BUDGET_MIN_BYTES, MEMORY_BUDGET_MAX_BYTES);
  return total / std::max(1u, jobs);
}

// The cap a search uses: a positive `max_memo_states` as given, else the
// budget (`memory_budget_bytes`, or the detected one when 0, split across
// `jobs`) in states.
inline size_t resolve_max_memo_states(size_t max_memo_states,
                                      uint64_t memory_budget_bytes = 0,
                                      unsigned jobs = 1) noexcept {
  if (max_memo_states > 0)
    return max_memo_states;
  const uint64_t budget =
      memory_budget_bytes > 0
          ? memory_budget_bytes / std::max(1u, jobs)
          : solver_memory_budget_bytes(available_memory_bytes(), jobs);
  return static_cast<size_t>(
      std::max<uint64_t>(1, budget / MEMORY_BYTES_PER_MEMO_STATE));
}

} // namespace csplendor

#endif // CSPLENDOR_SOLVER_MEMORY_BUDGET_H
