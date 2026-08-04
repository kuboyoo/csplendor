#include "action_encoder.h"
#include "mcts_concurrent_tree.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
volatile uint64_t benchmark_sink = 0;

TreeKey exact_key(uint64_t position_hash) {
  return {position_hash, MCTS_TREE_KEY_VERSION, MCTS_NO_OBSERVER,
          TreeDomain::Exact, 0};
}

std::vector<Game> make_corpus(size_t requested) {
  std::vector<Game> corpus;
  corpus.reserve(requested);
  uint64_t random = 0xda942042e4dd58b5ULL;
  uint64_t seed = 0;
  while (corpus.size() < requested) {
    Game game(seed++);
    for (int ply = 0; ply < 80 && corpus.size() < requested; ++ply) {
      corpus.push_back(game.clone_light());
      if (game.is_game_over())
        break;
      if (game.requires_forced_pass()) {
        if (!game.apply_forced_pass(false))
          break;
        continue;
      }
      random ^= random << 7;
      random ^= random >> 9;
      random ^= random << 8;
      if (!game.apply_random_action(random, false))
        break;
    }
  }
  return corpus;
}

template <typename Function>
double nanoseconds_per_operation(size_t operations, Function &&function) {
  const auto started = Clock::now();
  function();
  const double nanoseconds =
      std::chrono::duration<double, std::nano>(Clock::now() - started).count();
  return nanoseconds / static_cast<double>(operations);
}

void benchmark_action_encoder(const std::vector<Game> &corpus) {
  constexpr size_t mask_repeats = 200;
  const size_t mask_operations = corpus.size() * mask_repeats;
  const double reference_mask_ns =
      nanoseconds_per_operation(mask_operations, [&] {
        uint64_t checksum = 0;
        for (size_t repeat = 0; repeat < mask_repeats; ++repeat) {
          for (const Game &game : corpus) {
            const auto mask =
                ActionEncoderCpp::get_action_mask_reference_for_testing(game);
            checksum += mcts_action_mask::from_dense(mask);
          }
        }
        benchmark_sink ^= checksum;
      });
  const double direct_mask_ns = nanoseconds_per_operation(mask_operations, [&] {
    uint64_t checksum = 0;
    for (size_t repeat = 0; repeat < mask_repeats; ++repeat) {
      for (const Game &game : corpus)
        checksum += ActionEncoderCpp::get_action_mask_bits_trusted(game);
    }
    benchmark_sink ^= checksum;
  });

  std::vector<std::pair<size_t, int>> decodes;
  for (size_t state = 0; state < corpus.size(); ++state) {
    const uint64_t mask =
        ActionEncoderCpp::get_action_mask_bits_trusted(corpus[state]);
    mcts_action_mask::for_each(mask, [&](size_t action) {
      decodes.emplace_back(state, static_cast<int>(action));
    });
  }
  constexpr size_t decode_repeats = 8;
  const size_t decode_operations = decodes.size() * decode_repeats;
  const double reference_decode_ns =
      nanoseconds_per_operation(decode_operations, [&] {
        uint64_t checksum = 0;
        for (size_t repeat = 0; repeat < decode_repeats; ++repeat) {
          for (const auto &[state, action] : decodes) {
            checksum += ActionEncoderCpp::decode_reference_for_testing(
                            action, corpus[state])
                            .pack();
          }
        }
        benchmark_sink ^= checksum;
      });
  const double direct_decode_ns =
      nanoseconds_per_operation(decode_operations, [&] {
        uint64_t checksum = 0;
        for (size_t repeat = 0; repeat < decode_repeats; ++repeat) {
          for (const auto &[state, action] : decodes)
            checksum +=
                ActionEncoderCpp::decode_trusted(action, corpus[state]).pack();
        }
        benchmark_sink ^= checksum;
      });

  std::cout << "1 direct action mask/decode\n"
            << "  mask reference/trusted ns: " << reference_mask_ns << " / "
            << direct_mask_ns << " (" << reference_mask_ns / direct_mask_ns
            << "x)\n"
            << "  decode reference/trusted ns: " << reference_decode_ns << " / "
            << direct_decode_ns << " ("
            << reference_decode_ns / direct_decode_ns << "x)\n";
}

void benchmark_action_bitset(const std::vector<Game> &corpus) {
  std::vector<ActionMaskBits> bits;
  std::vector<DenseActionMask> dense;
  bits.reserve(corpus.size());
  dense.reserve(corpus.size());
  for (const Game &game : corpus) {
    const ActionMaskBits mask =
        ActionEncoderCpp::get_action_mask_bits_trusted(game);
    bits.push_back(mask);
    dense.push_back(mcts_action_mask::to_dense(mask));
  }

  constexpr size_t repeats = 20000;
  const size_t operations = corpus.size() * repeats;
  const double dense_ns = nanoseconds_per_operation(operations, [&] {
    uint64_t checksum = 0;
    for (size_t repeat = 0; repeat < repeats; ++repeat) {
      for (const auto &mask : dense) {
        for (size_t action = 0; action < MAX_ACTIONS; ++action) {
          if (mask[action])
            checksum += action + 1;
        }
      }
    }
    benchmark_sink ^= checksum;
  });
  const double bits_ns = nanoseconds_per_operation(operations, [&] {
    uint64_t checksum = 0;
    for (size_t repeat = 0; repeat < repeats; ++repeat) {
      for (ActionMaskBits mask : bits) {
        mcts_action_mask::for_each(
            mask, [&](size_t action) { checksum += action + 1; });
      }
    }
    benchmark_sink ^= checksum;
  });
  std::cout << "3 uint64 action mask iteration\n"
            << "  dense/bitset ns per mask: " << dense_ns << " / " << bits_ns
            << " (" << dense_ns / bits_ns << "x)\n";
}

struct BuiltTree {
  mcts_parallel::ConcurrentTree tree;
  std::vector<mcts_parallel::NodeHandle> nodes;

  explicit BuiltTree(size_t count)
      : tree(3, mcts_parallel::TreeBackend::Sharded, count + 1, 64) {
    nodes.reserve(count);
    mcts_parallel::Policy policy{};
    for (size_t action = 0; action < MAX_ACTIONS; ++action)
      policy[action] = static_cast<float>(action + 1);
    for (size_t index = 0; index < count; ++index) {
      auto node = tree.find_or_create(exact_key(index + 1));
      ActionMaskBits mask = 0;
      for (size_t edge = 0; edge < 12; ++edge)
        mask |= mcts_action_mask::bit((index * 7 + edge * 11) % MAX_ACTIONS);
      tree.expand_bits(node, policy, mcts_parallel::Value{0.1, -0.1}, mask);
      nodes.push_back(std::move(node));
    }
  }
};

void benchmark_quiescence(BuiltTree &built) {
  constexpr size_t fast_repeats = 1000000;
  constexpr size_t full_repeats = 5;
  const double fast_ns = nanoseconds_per_operation(fast_repeats, [&] {
    for (size_t repeat = 0; repeat < fast_repeats; ++repeat)
      built.tree.validate_quiescent_fast();
  });
  const double full_ns = nanoseconds_per_operation(full_repeats, [&] {
    for (size_t repeat = 0; repeat < full_repeats; ++repeat)
      built.tree.validate_quiescent_full();
  });
  std::cout << "2 O(1) quiescence validation\n"
            << "  full us / fast ns: " << full_ns / 1000.0 << " / " << fast_ns
            << " (" << full_ns / fast_ns << "x)\n";
}

struct DenseNodePayload {
  mcts_parallel::NodeStats64 stats{};
  mcts_parallel::Policy policy{};
  mcts_parallel::ActionMask mask{};
  std::array<uint64_t, MAX_ACTIONS> availability{};
};

void benchmark_compact_edges(BuiltTree &built) {
  size_t compact_bytes =
      built.nodes.size() * sizeof(mcts_parallel::detail::NodeRecord);
  size_t edges = 0;
  for (const auto &node : built.nodes) {
    compact_bytes +=
        node->edges.capacity() * sizeof(mcts_parallel::detail::EdgeStats64);
    edges += node->edges.size();
  }
  const size_t dense_bytes = built.nodes.size() * sizeof(DenseNodePayload);

  auto selected_node = built.nodes.front();
  const ActionMaskBits mask = selected_node->information_set_union;
  mcts_parallel::SelectionContext context;
  context.tree_generation = built.tree.generation();
  constexpr size_t repeats = 200000;
  const double selection_ns = nanoseconds_per_operation(repeats, [&] {
    uint64_t checksum = 0;
    for (size_t repeat = 0; repeat < repeats; ++repeat) {
      auto reservation = built.tree.select_and_reserve_bits(
          selected_node, mask, context, 0, nullptr);
      checksum += static_cast<uint64_t>(reservation->action() + 1);
      reservation->abort();
    }
    benchmark_sink ^= checksum;
  });

  std::cout << "4 compact legal-edge nodes\n"
            << "  mean edges/node: "
            << static_cast<double>(edges) / built.nodes.size() << '\n'
            << "  dense payload/compact estimated MiB: "
            << static_cast<double>(dense_bytes) / (1024.0 * 1024.0) << " / "
            << static_cast<double>(compact_bytes) / (1024.0 * 1024.0) << " ("
            << static_cast<double>(dense_bytes) / compact_bytes << "x)\n"
            << "  compact reserve+abort ns: " << selection_ns << '\n';
}

} // namespace

int main() {
  std::cout << std::fixed << std::setprecision(2);
  const std::vector<Game> corpus = make_corpus(256);
  benchmark_action_encoder(corpus);
  BuiltTree built(20000);
  benchmark_quiescence(built);
  benchmark_action_bitset(corpus);
  benchmark_compact_edges(built);
  std::cout << "checksum: " << benchmark_sink << '\n';
  return 0;
}
