// df-pn table and pn/dn arithmetic (doc/mate_dfpn_cpp_design.md 第 5 節).
#include "dfpn_mate_solver.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

using csplendor::solver_internal::DFPN_INF;
using csplendor::solver_internal::DFPN_LIMIT;
using csplendor::solver_internal::DfpnTable;
using csplendor::solver_internal::dfpn_add;
using csplendor::solver_internal::dfpn_second_bound;
using csplendor::solver_internal::dfpn_share;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Key {
  uint64_t value = 0;
  bool empty() const noexcept { return value == 0; }
  bool operator==(const Key &other) const noexcept {
    return value == other.value;
  }
};

struct IdentityHash {
  size_t operator()(const Key &key) const noexcept { return key.value; }
};

struct ConstantHash {
  size_t operator()(const Key &) const noexcept { return 7; }
};

void test_arithmetic() {
  require(dfpn_add(1, 2) == 3, "finite sum");
  require(dfpn_add(DFPN_INF, 0) == DFPN_INF, "INF absorbs");
  require(dfpn_add(DFPN_LIMIT, DFPN_LIMIT) == DFPN_LIMIT,
          "finite sums saturate below INF");
  require(dfpn_share(DFPN_INF, 10, 3) == DFPN_INF, "unbounded threshold");
  require(dfpn_share(20, 10, 3) == 13, "a child's share of the threshold");
  require(dfpn_second_bound(DFPN_INF) == DFPN_INF, "no sibling");
  require(dfpn_second_bound(8) == 11, "1 + epsilon bound");
  require(dfpn_second_bound(DFPN_LIMIT) == DFPN_INF,
          "a saturated sibling leaves the threshold open");
}

void test_capacity_follows_the_byte_budget() {
  using Table = DfpnTable<Key, IdentityHash>;
  const size_t budget = 1 << 20;
  Table table(budget);
  require(table.memory_bytes() == 0, "no allocation before the first store");
  for (uint64_t value = 1; value <= 200000; ++value)
    table.store(Key{value}, 1, 1, static_cast<uint32_t>(value));
  require(table.bucket_count() == table.max_bucket_count(),
          "the table grows to its fixed capacity");
  require(table.memory_bytes() <= budget, "never beyond the budget");
  require(table.size() <= table.max_bucket_count() * Table::WAYS,
          "entries fit the fixed capacity");
  require(table.replacements() > 0, "a full table replaces entries");

  Table small(1 << 20);
  small.store(Key{1}, 3, 4, 1);
  require(small.bucket_count() == Table::MIN_BUCKETS,
          "a small search keeps a small table");
  const auto *slot = small.find(Key{1});
  require(slot != nullptr && slot->pn == 3 && slot->dn == 4, "stored values");
  small.store(Key{1}, 5, 6, 2);
  require(small.size() == 1 && small.find(Key{1})->pn == 5, "updated in place");
  small.erase(Key{1});
  require(small.find(Key{1}) == nullptr && small.size() == 0, "erased");
}

void test_replacement_keeps_the_most_work() {
  // One bucket: every key lands in it.
  DfpnTable<Key, ConstantHash> table(DfpnTable<Key, ConstantHash>::WAYS *
                                     sizeof(DfpnTable<Key, ConstantHash>::Slot));
  require(table.max_bucket_count() == 1, "a one-bucket budget");
  for (uint64_t value = 1; value <= 4; ++value)
    table.store(Key{value}, 1, 1, static_cast<uint32_t>(value * 10));
  table.store(Key{5}, 1, 1, 25);
  require(table.find(Key{1}) == nullptr, "the least work is evicted");
  require(table.find(Key{2}) != nullptr && table.find(Key{5}) != nullptr,
          "the rest stays");
  require(table.replacements() == 1 && table.size() == 4, "one replacement");
}

} // namespace

int main() {
  try {
    test_arithmetic();
    test_capacity_follows_the_byte_budget();
    test_replacement_keeps_the_most_work();
  } catch (const std::exception &exc) {
    std::cerr << "dfpn_table_unit failed: " << exc.what() << '\n';
    return 1;
  }
  std::cout << "dfpn_table_unit passed\n";
  return 0;
}
