// Reproducible native hot-path benchmark for Phase 0 performance work.
//
// The executable writes one self-contained JSON object per workload.  It uses
// only the public engine/search facades so the same source can be compiled
// against a baseline and a candidate tree without benchmark-only game logic.
#include "action_encoder.h"
#include "game.h"
#include "mcts.h"
#include "mcts_concurrent_tree.h"
#include "mcts_parallel_searcher.h"
#include "mcts_parallel_trace.h"
#include "mcts_root_parallel.h"
#include "perf_counters.h"
#include "reveal_solver_components.h"
#include "reveal_verified_solver.h"
#include "solver_tt_types.h"
#include "state_encoder.h"
#include "state_invariants.h"
#include "visible_only_solver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif
#if defined(__linux__)
#include <unistd.h>
#endif

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
namespace benchmark_allocation_instrumentation {

enum class Kind : size_t { Scalar, Array, AlignedScalar, AlignedArray, Count };

constexpr size_t kKindCount = static_cast<size_t>(Kind::Count);
std::atomic<bool> enabled{false};
std::array<std::atomic<uint64_t>, kKindCount> calls{};
std::array<std::atomic<uint64_t>, kKindCount> bytes{};

void record(Kind kind, size_t allocation_bytes) noexcept {
  if (!enabled.load(std::memory_order_relaxed))
    return;
  const size_t index = static_cast<size_t>(kind);
  calls[index].fetch_add(1, std::memory_order_relaxed);
  bytes[index].fetch_add(static_cast<uint64_t>(allocation_bytes),
                         std::memory_order_relaxed);
}

void reset_and_enable() noexcept {
  for (size_t index = 0; index < kKindCount; ++index) {
    calls[index].store(0, std::memory_order_relaxed);
    bytes[index].store(0, std::memory_order_relaxed);
  }
  enabled.store(true, std::memory_order_release);
}

void disable() noexcept { enabled.store(false, std::memory_order_release); }

} // namespace benchmark_allocation_instrumentation

void *operator new(std::size_t size) {
  const size_t actual_size = size == 0 ? 1 : size;
  void *memory = std::malloc(actual_size);
  if (!memory)
    throw std::bad_alloc();
  benchmark_allocation_instrumentation::record(
      benchmark_allocation_instrumentation::Kind::Scalar, size);
  return memory;
}

void *operator new[](std::size_t size) {
  const size_t actual_size = size == 0 ? 1 : size;
  void *memory = std::malloc(actual_size);
  if (!memory)
    throw std::bad_alloc();
  benchmark_allocation_instrumentation::record(
      benchmark_allocation_instrumentation::Kind::Array, size);
  return memory;
}

void *aligned_allocate(std::size_t size, std::align_val_t alignment,
                       benchmark_allocation_instrumentation::Kind kind) {
  const size_t align = static_cast<size_t>(alignment);
  const size_t actual_size = size == 0 ? 1 : size;
  if (actual_size > std::numeric_limits<size_t>::max() - (align - 1))
    throw std::bad_alloc();
  const size_t rounded_size = ((actual_size + align - 1) / align) * align;
  void *memory = std::aligned_alloc(align, rounded_size);
  if (!memory)
    throw std::bad_alloc();
  benchmark_allocation_instrumentation::record(kind, size);
  return memory;
}

void *operator new(std::size_t size, std::align_val_t alignment) {
  return aligned_allocate(
      size, alignment,
      benchmark_allocation_instrumentation::Kind::AlignedScalar);
}

void *operator new[](std::size_t size, std::align_val_t alignment) {
  return aligned_allocate(
      size, alignment,
      benchmark_allocation_instrumentation::Kind::AlignedArray);
}

// Keep the malloc/free boundary opaque to GCC's mismatched-new-delete warning:
// these are the matching deallocators for the benchmark-only replacement new
// overloads above, rather than frees of storage owned by the system new.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void deallocate_instrumented_memory(void *memory) noexcept {
  std::free(memory);
}

void operator delete(void *memory) noexcept {
  deallocate_instrumented_memory(memory);
}
void operator delete[](void *memory) noexcept {
  deallocate_instrumented_memory(memory);
}
void operator delete(void *memory, std::size_t) noexcept {
  deallocate_instrumented_memory(memory);
}
void operator delete[](void *memory, std::size_t) noexcept {
  deallocate_instrumented_memory(memory);
}
void operator delete(void *memory, std::align_val_t) noexcept {
  deallocate_instrumented_memory(memory);
}
void operator delete[](void *memory, std::align_val_t) noexcept {
  deallocate_instrumented_memory(memory);
}
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
  deallocate_instrumented_memory(memory);
}
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
  deallocate_instrumented_memory(memory);
}
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char *kSchema = "csplendor.engine_hotpath.v1";
constexpr uint64_t kDigestOffset = 1469598103934665603ULL;
constexpr uint64_t kDigestPrime = 1099511628211ULL;
constexpr size_t kTransitionCorpusSize = 256;
constexpr uint64_t kFormalTraceSimulations = 41;

volatile uint64_t benchmark_sink = 0;

const std::array<const char *, 23> kWorkloads = {
    "legal_count",      "legal_codes",
    "legal_actions",    "random_selfplay_apply",
    "apply_only",       "purchase_apply",
    "apply_exact_hash", "apply_observable_hash",
    "cold_hash",        "cached_hash",
    "clone_light",      "determinization_clone",
    "state_encoder",    "action_mask",
    "decode_apply",     "legacy_mcts",
    "shared_tree",      "parallel_scheduler",
    "root_parallel",    "board_copy_restore",
    "solver_state_key", "solver_tt",
    "visible_solver",
};

// exact_reveal is kept separate from the fixed-size array above to make the
// compile-time workload count obvious while retaining an exhaustive list in
// all_workloads().
std::vector<std::string> all_workloads() {
  std::vector<std::string> result;
  result.reserve(kWorkloads.size() + 1);
  for (const char *name : kWorkloads)
    result.emplace_back(name);
  result.emplace_back("exact_reveal");
  return result;
}

uint64_t digest_u64(uint64_t digest, uint64_t value) noexcept {
  for (int byte = 0; byte < 8; ++byte) {
    digest ^= value & 0xffU;
    digest *= kDigestPrime;
    value >>= 8;
  }
  return digest;
}

uint64_t digest_i64(uint64_t digest, int64_t value) noexcept {
  return digest_u64(digest, static_cast<uint64_t>(value));
}

uint64_t digest_float(uint64_t digest, float value) noexcept {
  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "unexpected float size");
  std::memcpy(&bits, &value, sizeof(bits));
  return digest_u64(digest, bits);
}

uint64_t digest_codes(const std::vector<uint64_t> &codes,
                      uint64_t digest = kDigestOffset) noexcept {
  digest = digest_u64(digest, codes.size());
  for (uint64_t code : codes)
    digest = digest_u64(digest, code);
  return digest;
}

uint64_t digest_string(uint64_t digest, const std::string &value) noexcept {
  digest = digest_u64(digest, value.size());
  for (unsigned char character : value) {
    digest ^= character;
    digest *= kDigestPrime;
  }
  return digest;
}

std::string hex_digest(uint64_t value) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

std::string json_string(const std::string &value) {
  std::ostringstream stream;
  stream << '"';
  for (unsigned char character : value) {
    switch (character) {
    case '"':
      stream << "\\\"";
      break;
    case '\\':
      stream << "\\\\";
      break;
    case '\b':
      stream << "\\b";
      break;
    case '\f':
      stream << "\\f";
      break;
    case '\n':
      stream << "\\n";
      break;
    case '\r':
      stream << "\\r";
      break;
    case '\t':
      stream << "\\t";
      break;
    default:
      if (character < 0x20) {
        stream << "\\u" << std::hex << std::setfill('0') << std::setw(4)
               << static_cast<unsigned int>(character) << std::dec;
      } else {
        stream << static_cast<char>(character);
      }
    }
  }
  stream << '"';
  return stream.str();
}

class JsonObject {
public:
  void raw(std::string key, std::string value) {
    fields_.emplace_back(std::move(key), std::move(value));
  }

  void string(const std::string &key, const std::string &value) {
    raw(key, json_string(value));
  }

  void boolean(const std::string &key, bool value) {
    raw(key, value ? "true" : "false");
  }

  template <typename Integer>
  void integer(const std::string &key, Integer value) {
    raw(key, std::to_string(value));
  }

  void number(const std::string &key, double value) {
    if (!std::isfinite(value)) {
      raw(key, "null");
      return;
    }
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    raw(key, stream.str());
  }

  std::string render() const {
    std::ostringstream stream;
    stream << '{';
    for (size_t index = 0; index < fields_.size(); ++index) {
      if (index)
        stream << ',';
      stream << json_string(fields_[index].first) << ':'
             << fields_[index].second;
    }
    stream << '}';
    return stream.str();
  }

private:
  std::vector<std::pair<std::string, std::string>> fields_;
};

struct Arguments {
  std::vector<std::string> workloads{"all"};
  std::string fixture = "midgame_250";
  uint64_t iterations = 10000;
  uint64_t warmup = 100;
  uint64_t seed = 42;
  uint32_t batch_size = 16;
  uint32_t threads = 4;
  uint32_t latency_us = 0;
  uint32_t observer = 0;
  int depth = 7;
  int attacker = -1;
  int fixture_plies = 12;
  double time_limit_seconds = 0.0;
  bool determinization = false;
  bool simple_payment_mode = false;
  bool persistent_reuse = false;
  bool proof_dag = false;
  bool retained_tree = false;
  std::string tree_backend = "sharded";
  bool list_workloads = false;
};

std::vector<std::string> split_list(const std::string &text) {
  std::vector<std::string> result;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (!item.empty())
      result.push_back(item);
  }
  if (result.empty())
    throw std::invalid_argument("comma-separated option cannot be empty");
  return result;
}

bool parse_bool(const std::string &text) {
  if (text == "1" || text == "true" || text == "on")
    return true;
  if (text == "0" || text == "false" || text == "off")
    return false;
  throw std::invalid_argument("boolean value must be 0/1/false/true/off/on");
}

void print_help() {
  std::cerr
      << "Usage: benchmark_engine_hotpaths [options]\n"
      << "  --workload NAME[,NAME...]  one workload or all\n"
      << "  --fixture NAME             initial/midgame_250/five_moves/random "
         "or\n"
      << "                             "
         "token_return/gold_payment/reserve_limit/\n"
      << "                             multi_noble/final_round/forced_pass/\n"
      << "                             "
         "editor_fallback/hidden_reserve/reveal_heavy\n"
      << "  --iterations N             calls, simulations, or solver node "
         "limit\n"
      << "  --warmup N                 untimed microbenchmark operations\n"
      << "  --seed N                   initial/random fixture seed\n"
      << "  --fixture-plies N          random fixture setup plies\n"
      << "  --batch-size N             legacy MCTS synthetic batch size\n"
      << "  --threads N                parallel/root workers (default 4)\n"
      << "  --latency-us N             synthetic inference latency (default "
         "0)\n"
      << "  --observer 0|1             observable-state perspective\n"
      << "  --determinization BOOL     legacy MCTS hidden-information mode\n"
      << "  --tree-backend NAME        shared_tree coarse/sharded backend\n"
      << "  --depth N                  exact reveal attacker-move depth\n"
      << "  --attacker -1|0|1          -1 selects fixture side to move\n"
      << "  --time-limit SECONDS       solver wall-clock limit; 0 disables\n"
      << "  --simple-payment BOOL      set rule-engine payment mode\n"
      << "  --persistent-reuse BOOL    prime/reuse exact reveal cache\n"
      << "  --proof-dag BOOL           build and validate exact proof DAG\n"
      << "  --retained-tree BOOL       reuse MCTS tree after warmup\n"
      << "  --list-workloads           print names and exit\n";
}

Arguments parse_arguments(int argc, char **argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option(argv[index]);
    if (option == "--help" || option == "-h") {
      print_help();
      std::exit(0);
    }
    if (option == "--list-workloads") {
      arguments.list_workloads = true;
      continue;
    }
    const size_t split = option.find('=');
    const std::string key = option.substr(0, split);
    std::string value;
    if (split != std::string::npos) {
      value = option.substr(split + 1);
    } else {
      if (index + 1 >= argc)
        throw std::invalid_argument("missing value for " + key);
      value = argv[++index];
    }

    if (key == "--workload")
      arguments.workloads = split_list(value);
    else if (key == "--fixture")
      arguments.fixture = value;
    else if (key == "--iterations")
      arguments.iterations = std::stoull(value);
    else if (key == "--warmup")
      arguments.warmup = std::stoull(value);
    else if (key == "--seed")
      arguments.seed = std::stoull(value);
    else if (key == "--batch-size")
      arguments.batch_size = static_cast<uint32_t>(std::stoul(value));
    else if (key == "--threads")
      arguments.threads = static_cast<uint32_t>(std::stoul(value));
    else if (key == "--latency-us")
      arguments.latency_us = static_cast<uint32_t>(std::stoul(value));
    else if (key == "--observer")
      arguments.observer = static_cast<uint32_t>(std::stoul(value));
    else if (key == "--depth")
      arguments.depth = std::stoi(value);
    else if (key == "--attacker")
      arguments.attacker = std::stoi(value);
    else if (key == "--fixture-plies")
      arguments.fixture_plies = std::stoi(value);
    else if (key == "--time-limit")
      arguments.time_limit_seconds = std::stod(value);
    else if (key == "--determinization")
      arguments.determinization = parse_bool(value);
    else if (key == "--simple-payment")
      arguments.simple_payment_mode = parse_bool(value);
    else if (key == "--persistent-reuse")
      arguments.persistent_reuse = parse_bool(value);
    else if (key == "--proof-dag")
      arguments.proof_dag = parse_bool(value);
    else if (key == "--retained-tree")
      arguments.retained_tree = parse_bool(value);
    else if (key == "--tree-backend")
      arguments.tree_backend = value;
    else
      throw std::invalid_argument("unknown option: " + key);
  }

  if (arguments.iterations == 0)
    throw std::invalid_argument("iterations must be positive");
  if (arguments.batch_size == 0)
    throw std::invalid_argument("batch-size must be positive");
  if (arguments.threads == 0 || arguments.threads > 256)
    throw std::invalid_argument("threads must be in [1, 256]");
  if (arguments.latency_us > 1000000)
    throw std::invalid_argument("latency-us must not exceed 1000000");
  if (arguments.observer >= Board::NUM_PLAYERS)
    throw std::invalid_argument("observer must be 0 or 1");
  if (arguments.depth < 0)
    throw std::invalid_argument("depth must be non-negative");
  if (arguments.attacker < -1 || arguments.attacker >= Board::NUM_PLAYERS)
    throw std::invalid_argument("attacker must be -1, 0, or 1");
  if (arguments.fixture_plies < 0)
    throw std::invalid_argument("fixture-plies must be non-negative");
  if (arguments.time_limit_seconds < 0.0)
    throw std::invalid_argument("time-limit must be non-negative");
  if (arguments.tree_backend != "coarse" && arguments.tree_backend != "sharded")
    throw std::invalid_argument("tree-backend must be coarse or sharded");
  return arguments;
}

uint64_t next_random(uint64_t &state) noexcept {
  state ^= state << 7;
  state ^= state >> 9;
  state ^= state << 8;
  return state;
}

struct Fixture {
  std::string name;
  std::string category = "rule_general";
  std::string construction = "seeded";
  uint64_t seed = 0;
  Game game{0};
  uint16_t legal_count = 0;
  uint64_t exact_hash = 0;
  std::array<uint64_t, Board::NUM_PLAYERS> observable_hashes{};
  uint64_t ordered_legal_digest = 0;
  size_t setup_actions = 0;
  uint64_t required_root_action = UINT64_MAX;
  bool canonical = false;
  bool editor_compatible = false;
  std::string reachable_invariant_report;
  std::string editor_invariant_report;
};

constexpr std::array<uint64_t, 12> kMidgame250Actions = {
    530ULL,  648ULL, 168ULL,     11ULL,      2592ULL,  2208ULL,
    2208ULL, 26ULL,  2361864ULL, 8389652ULL, 25128ULL, 19ULL};
constexpr std::array<uint64_t, 12> kFiveMoveActions = {
    648ULL, 65ULL,  506ULL, 19ULL,   2568ULL, 26ULL,
    666ULL, 168ULL, 18ULL,  2088ULL, 4228ULL, 9128905224ULL};
constexpr std::array<uint64_t, 25> kForcedPassActions = {
    19ULL,         386ULL,        498ULL,        442ULL,       2208ULL,
    610ULL,        8390772ULL,    2088ULL,       458ULL,       2592ULL,
    2592ULL,       33563296ULL,   17ULL,         67117704ULL,  1048844ULL,
    8623497864ULL, 648ULL,        8589943304ULL, 520ULL,       8589935104ULL,
    252ULL,        1075841184ULL, 2176ULL,       536872960ULL, 2048ULL};

template <size_t Size>
Game replay_fixture(const std::array<uint64_t, Size> &actions) {
  Game game(42);
  for (size_t index = 0; index < actions.size(); ++index) {
    if (!game.apply_action_code(actions[index], false)) {
      throw std::runtime_error("fixed fixture action " + std::to_string(index) +
                               " is not legal");
    }
  }
  return game;
}

template <size_t Size>
Game replay_fixture(uint64_t seed, const std::array<uint64_t, Size> &actions) {
  Game game(seed);
  for (size_t index = 0; index < actions.size(); ++index) {
    if (!game.apply_action_code(actions[index], false)) {
      throw std::runtime_error("fixed fixture action " + std::to_string(index) +
                               " is not legal");
    }
  }
  return game;
}

bool remove_card_from_public_storage(Board &board, uint8_t card_id) {
  for (auto &level : board.visible) {
    for (int8_t &visible : level) {
      if (visible == static_cast<int8_t>(card_id)) {
        visible = -1;
        return true;
      }
    }
  }
  for (auto &deck : board.decks) {
    for (auto iterator = deck.begin(); iterator != deck.end(); ++iterator) {
      if (*iterator == card_id) {
        deck.erase(iterator);
        return true;
      }
    }
  }
  return false;
}

void install_purchased_cards(Game &game, uint8_t player_index,
                             const std::vector<uint8_t> &card_ids) {
  if (player_index >= Board::NUM_PLAYERS)
    throw std::invalid_argument("fixture player is out of range");
  Board &board = game.board.begin_editor_mutation();
  PlayerState &player = board.players[player_index];
  player.purchased_cards.clear();
  player.bonuses = {0, 0, 0, 0, 0};
  player.points = 0;
  for (uint8_t card_id : card_ids) {
    if (!remove_card_from_public_storage(board, card_id))
      throw std::runtime_error(
          "fixture purchased card is not in public storage");
    const Card &card = get_card(card_id);
    player.purchased_cards.push_back(card_id);
    ++player.bonuses[card.bonus];
    player.points = static_cast<uint8_t>(player.points + card.points);
  }
  player.purchased_count = static_cast<uint8_t>(player.purchased_cards.size());
  player.sync_packed();
}

std::vector<uint8_t> cards_for_noble_requirements(const Board &board) {
  if (board.nobles.size() < 2)
    throw std::runtime_error("multi-noble fixture needs two nobles");
  std::array<uint8_t, 5> required{};
  for (size_t noble_slot = 0; noble_slot < 2; ++noble_slot) {
    const Noble &noble = get_noble(board.nobles[noble_slot]);
    for (size_t color = 0; color < required.size(); ++color)
      required[color] = std::max(required[color], noble.requirement[color]);
  }

  std::vector<uint8_t> selected;
  for (size_t color = 0; color < required.size(); ++color) {
    std::vector<const Card *> candidates;
    for (const Card &card : CARDS) {
      if (card.bonus == color)
        candidates.push_back(&card);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Card *left, const Card *right) {
                if (left->points != right->points)
                  return left->points < right->points;
                if (left->level != right->level)
                  return left->level < right->level;
                return left->id < right->id;
              });
    if (candidates.size() < required[color])
      throw std::runtime_error("not enough cards for noble fixture");
    for (size_t index = 0; index < required[color]; ++index)
      selected.push_back(candidates[index]->id);
  }
  return selected;
}

std::vector<uint8_t> cards_for_score(int minimum_points) {
  std::vector<const Card *> candidates;
  candidates.reserve(CARD_COUNT);
  for (const Card &card : CARDS)
    candidates.push_back(&card);
  std::sort(candidates.begin(), candidates.end(),
            [](const Card *left, const Card *right) {
              if (left->points != right->points)
                return left->points > right->points;
              if (left->level != right->level)
                return left->level > right->level;
              return left->id < right->id;
            });
  std::vector<uint8_t> selected;
  int points = 0;
  for (const Card *card : candidates) {
    selected.push_back(card->id);
    points += card->points;
    if (points >= minimum_points)
      break;
  }
  if (points < minimum_points)
    throw std::runtime_error("not enough points for final-round fixture");
  return selected;
}

Action first_action_of_type(const Game &game, ActionType type) {
  const auto actions = game.legal_actions();
  const auto iterator = std::find_if(
      actions.begin(), actions.end(),
      [type](const Action &action) { return action.type == type; });
  if (iterator == actions.end())
    throw std::runtime_error("fixture action type is unavailable");
  return *iterator;
}

Game make_reserve_limit_fixture(size_t &setup_actions) {
  Game game(1);
  for (int reserve_number = 0; reserve_number < Board::MAX_RESERVED;
       ++reserve_number) {
    const Action reserve = first_action_of_type(game, RESERVE_VISIBLE);
    if (!game.apply(reserve, false))
      throw std::runtime_error("reserve-limit fixture reserve failed");
    ++setup_actions;
    const Action response = first_action_of_type(game, TAKE_DIFFERENT);
    if (!game.apply(response, false))
      throw std::runtime_error("reserve-limit fixture response failed");
    ++setup_actions;
  }
  if (game.current_player() != 0 ||
      game.board.players[0].reserved_count != Board::MAX_RESERVED)
    throw std::runtime_error("reserve-limit fixture did not reach its target");
  for (const Action &action : game.legal_actions()) {
    if (action.type == RESERVE_VISIBLE || action.type == RESERVE_DECK)
      throw std::runtime_error("reserve-limit fixture still permits reserve");
  }
  return game;
}

Game make_hidden_reserve_fixture(size_t &setup_actions) {
  Game game(20260726);
  const Action reserve = first_action_of_type(game, RESERVE_DECK);
  if (!game.apply(reserve, false))
    throw std::runtime_error("hidden-reserve fixture action failed");
  ++setup_actions;
  const PlayerState &owner = game.board.players[0];
  if (owner.reserved_count != 1 || !owner.reserved_is_hidden[0])
    throw std::runtime_error("hidden-reserve fixture did not hide its card");
  return game;
}

void validate_fixed_fixture(const Fixture &fixture, uint16_t expected_count,
                            uint64_t expected_hash,
                            uint64_t expected_observable_0,
                            uint64_t expected_observable_1) {
  if (fixture.legal_count != expected_count ||
      fixture.exact_hash != expected_hash ||
      fixture.observable_hashes[0] != expected_observable_0 ||
      fixture.observable_hashes[1] != expected_observable_1) {
    std::ostringstream message;
    message << fixture.name << " fixture drift: count=" << fixture.legal_count
            << " exact=" << fixture.exact_hash
            << " observable0=" << fixture.observable_hashes[0]
            << " observable1=" << fixture.observable_hashes[1];
    throw std::runtime_error(message.str());
  }
}

Fixture make_fixture(const Arguments &arguments) {
  Fixture fixture;
  fixture.name = arguments.fixture;
  fixture.seed = arguments.seed;
  if (arguments.fixture == "initial") {
    fixture.category = "initial";
    fixture.game = Game(arguments.seed);
  } else if (arguments.fixture == "midgame_250" || arguments.fixture == "250") {
    fixture.name = "midgame_250";
    fixture.category = "high_legal_count";
    fixture.construction = "fixed_legal_replay";
    fixture.seed = 42;
    fixture.game = replay_fixture(kMidgame250Actions);
    fixture.setup_actions = kMidgame250Actions.size();
  } else if (arguments.fixture == "five_moves" ||
             arguments.fixture == "legal_5" || arguments.fixture == "5") {
    fixture.name = "five_moves";
    fixture.category = "low_legal_count";
    fixture.construction = "fixed_legal_replay";
    fixture.seed = 42;
    fixture.game = replay_fixture(kFiveMoveActions);
    fixture.setup_actions = kFiveMoveActions.size();
  } else if (arguments.fixture == "random") {
    fixture.category = "random_midgame";
    fixture.construction = "seeded_legal_replay";
    fixture.game = Game(arguments.seed);
    uint64_t random = arguments.seed ^ 0xda942042e4dd58b5ULL;
    for (int ply = 0; ply < arguments.fixture_plies; ++ply) {
      if (fixture.game.is_game_over())
        break;
      // Transition workloads need at least one legal action.  If the requested
      // setup length reaches game-over, retain the last reachable non-terminal
      // state instead of later resetting a terminal fixture forever.
      Game previous = fixture.game.clone_light();
      if (!fixture.game.apply_random_action(next_random(random), false))
        throw std::runtime_error("random fixture construction failed");
      if (fixture.game.is_game_over()) {
        fixture.game = std::move(previous);
        break;
      }
      ++fixture.setup_actions;
    }
  } else if (arguments.fixture == "token_return") {
    fixture.category = "token_return";
    fixture.construction = "canonical_editor_setup";
    fixture.seed = 123;
    fixture.game = Game(fixture.seed);
    Board &board = fixture.game.board.begin_editor_mutation();
    board.bank = {2, 2, 2, 2, 2, 5};
    board.players[0].gems = {2, 2, 2, 2, 2, 0};
    board.players[0].sync_packed();
  } else if (arguments.fixture == "gold_payment") {
    fixture.category = "gold_payment";
    fixture.construction = "canonical_editor_setup";
    fixture.seed = 42;
    fixture.game = Game(fixture.seed);
    Board &board = fixture.game.board.begin_editor_mutation();
    board.bank = {3, 3, 3, 3, 3, 2};
    board.players[0].gems = {1, 1, 1, 1, 1, 3};
    board.players[0].sync_packed();
  } else if (arguments.fixture == "reserve_limit") {
    fixture.category = "reserve_limit";
    fixture.construction = "seeded_legal_replay";
    fixture.seed = 1;
    fixture.game = make_reserve_limit_fixture(fixture.setup_actions);
  } else if (arguments.fixture == "multi_noble") {
    fixture.category = "multi_noble";
    fixture.construction = "canonical_editor_setup";
    fixture.seed = 5;
    fixture.game = Game(fixture.seed);
    const auto cards = cards_for_noble_requirements(fixture.game.board);
    install_purchased_cards(fixture.game, 0, cards);
    fixture.game.board.begin_editor_mutation().waiting_noble = true;
  } else if (arguments.fixture == "final_round") {
    fixture.category = "final_round";
    fixture.construction = "canonical_editor_setup";
    fixture.seed = 9;
    fixture.game = Game(fixture.seed);
    install_purchased_cards(fixture.game, 0, cards_for_score(15));
    Board &board = fixture.game.board.begin_editor_mutation();
    board.final_round = true;
    board.current_player = 1;
    board.turn = 1;
  } else if (arguments.fixture == "forced_pass") {
    fixture.category = "forced_pass";
    fixture.construction = "fixed_legal_replay";
    fixture.seed = 10;
    fixture.game = replay_fixture(fixture.seed, kForcedPassActions);
    fixture.setup_actions = kForcedPassActions.size();
    if (!fixture.game.requires_forced_pass())
      throw std::runtime_error(
          "forced-pass fixture still has an ordinary move");
  } else if (arguments.fixture == "editor_fallback") {
    fixture.category = "editor_fallback";
    fixture.construction = "noncanonical_editor_setup";
    fixture.seed = 42;
    fixture.game = Game(fixture.seed);
    Board &board = fixture.game.board.begin_editor_mutation();
    // Deliberately exceed both token conservation and the ten-token player
    // limit.  This is the public-editor MAX_MOVES fallback contract used by
    // test_rule_move_generation.py, not a reachable game position.
    board.players[0].gems = {3, 3, 3, 3, 3, 3};
    board.players[0].sync_packed();
  } else if (arguments.fixture == "hidden_reserve") {
    fixture.category = "hidden_reserve";
    fixture.construction = "seeded_legal_replay";
    fixture.seed = 20260726;
    fixture.game = make_hidden_reserve_fixture(fixture.setup_actions);
  } else if (arguments.fixture == "reveal_heavy") {
    fixture.category = "reveal_heavy";
    fixture.construction = "canonical_editor_setup";
    fixture.seed = 5;
    fixture.game = Game(fixture.seed);
    install_purchased_cards(fixture.game, 1, cards_for_score(15));
    fixture.game.board.begin_editor_mutation().current_player = 1;
    const Action reserve = first_action_of_type(fixture.game, RESERVE_DECK);
    fixture.required_root_action = reserve.pack();
  } else {
    throw std::invalid_argument(
        "unknown fixture; use --help to list supported fixture names");
  }

  fixture.game.simple_payment_mode = arguments.simple_payment_mode;
  fixture.legal_count = fixture.game.legal_action_count();
  fixture.exact_hash = fixture.game.board.hash();
  for (uint8_t observer = 0; observer < Board::NUM_PLAYERS; ++observer)
    fixture.observable_hashes[observer] =
        fixture.game.board.observable_hash(observer);
  fixture.ordered_legal_digest =
      digest_codes(fixture.game.legal_action_codes());

  const auto reachable_report = csplendor::state::validate_invariants(
      fixture.game.board, csplendor::state::Profile::Reachable);
  const auto editor_report = csplendor::state::validate_invariants(
      fixture.game.board, csplendor::state::Profile::Editor);
  fixture.canonical = reachable_report.ok();
  fixture.editor_compatible = editor_report.ok();
  fixture.reachable_invariant_report =
      csplendor::state::describe_invariant_violations(reachable_report);
  fixture.editor_invariant_report =
      csplendor::state::describe_invariant_violations(editor_report);

  // The golden values apply to the canonical rule mode only.  Alternate
  // payment mode intentionally changes the legal action list, but not state.
  if (!arguments.simple_payment_mode && fixture.name == "midgame_250") {
    validate_fixed_fixture(fixture, 250, 14707374328533802645ULL,
                           11956935861795463816ULL, 4997747067739671915ULL);
  }
  if (!arguments.simple_payment_mode && fixture.name == "five_moves") {
    validate_fixed_fixture(fixture, 5, 13684725691275296037ULL,
                           11167490305115859042ULL, 13782163118936808622ULL);
  }
  if (fixture.name != "editor_fallback" && !fixture.canonical) {
    throw std::runtime_error(fixture.name +
                             " fixture failed reachable invariants: " +
                             fixture.reachable_invariant_report);
  }
  if (fixture.name == "editor_fallback" && fixture.canonical)
    throw std::runtime_error("editor fallback unexpectedly became canonical");
  if (!fixture.editor_compatible) {
    throw std::runtime_error(fixture.name +
                             " fixture failed editor invariants: " +
                             fixture.editor_invariant_report);
  }
  if (fixture.name == "token_return") {
    bool saw_return = false;
    for (const Action &action : fixture.game.legal_actions())
      saw_return =
          saw_return ||
          std::any_of(action.return_gems.begin(), action.return_gems.end(),
                      [](uint8_t value) { return value != 0; });
    if (!saw_return)
      throw std::runtime_error("token-return fixture has no returned tokens");
  }
  if (fixture.name == "gold_payment") {
    bool saw_gold_payment = false;
    for (const Action &action : fixture.game.legal_actions()) {
      if (action.type != PURCHASE)
        continue;
      const int gold =
          std::accumulate(action.gold_as.begin(), action.gold_as.end(), 0);
      saw_gold_payment = saw_gold_payment || gold > 0;
    }
    if (!saw_gold_payment)
      throw std::runtime_error("gold-payment fixture has no gold payment");
  }
  if (fixture.name == "multi_noble" && fixture.legal_count < 2)
    throw std::runtime_error("multi-noble fixture has fewer than two choices");
  if (fixture.name == "forced_pass") {
    const auto codes = fixture.game.legal_action_codes();
    if (codes.size() != 1 || codes.front() != 6)
      throw std::runtime_error(
          "forced-pass fixture did not emit canonical PASS");
  }
  if (fixture.name == "editor_fallback" && fixture.legal_count != MAX_MOVES)
    throw std::runtime_error("editor fallback did not reach MAX_MOVES cap");
  if (fixture.exact_hash != fixture.game.board.compute_hash_uncached())
    throw std::runtime_error("fixture cached exact hash failed oracle check");
  return fixture;
}

uint64_t current_rss_kib() {
#if defined(__linux__)
  std::ifstream status("/proc/self/statm");
  uint64_t total_pages = 0;
  uint64_t resident_pages = 0;
  if (status >> total_pages >> resident_pages) {
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size > 0)
      return resident_pages * static_cast<uint64_t>(page_size) / 1024ULL;
  }
#endif
#if defined(__linux__) || defined(__APPLE__)
  struct rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss) / 1024ULL;
#else
    return static_cast<uint64_t>(usage.ru_maxrss);
#endif
  }
#endif
  return 0;
}

struct PerfAccumulator {
  std::array<uint64_t, csplendor::perf::COUNTER_COUNT> values{};
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  std::array<uint64_t, benchmark_allocation_instrumentation::kKindCount>
      allocation_calls{};
  std::array<uint64_t, benchmark_allocation_instrumentation::kKindCount>
      allocation_bytes{};
#endif

  void add(const csplendor::perf::Snapshot &snapshot) noexcept {
    for (size_t index = 0; index < values.size(); ++index) {
      if (index ==
          static_cast<size_t>(
              csplendor::perf::Counter::ParallelReservationOccupancyMax))
        values[index] = std::max(values[index], snapshot.values[index]);
      else
        values[index] += snapshot.values[index];
    }
  }

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  void add_allocations() noexcept {
    for (size_t index = 0; index < allocation_calls.size(); ++index) {
      allocation_calls[index] +=
          benchmark_allocation_instrumentation::calls[index].load(
              std::memory_order_relaxed);
      allocation_bytes[index] +=
          benchmark_allocation_instrumentation::bytes[index].load(
              std::memory_order_relaxed);
    }
  }
#endif
};

struct Result {
  uint64_t operations = 0;
  uint64_t elapsed_ns = 0;
  uint64_t digest = kDigestOffset;
  JsonObject counters;
  JsonObject semantics;
  PerfAccumulator perf;
};

template <typename Function>
uint64_t time_once(Function &&function, PerfAccumulator &perf) {
  csplendor::perf::reset();
  const auto started = Clock::now();
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  benchmark_allocation_instrumentation::reset_and_enable();
  try {
    function();
  } catch (...) {
    benchmark_allocation_instrumentation::disable();
    throw;
  }
  benchmark_allocation_instrumentation::disable();
#else
  function();
#endif
  const auto finished = Clock::now();
  perf.add(csplendor::perf::snapshot());
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  perf.add_allocations();
#endif
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started)
          .count());
}

template <typename Function>
Result benchmark_loop(const Arguments &arguments, Function &&function) {
  uint64_t discarded = kDigestOffset;
  for (uint64_t index = 0; index < arguments.warmup; ++index)
    discarded = function(index, discarded);
  benchmark_sink ^= discarded;

  Result result;
  result.operations = arguments.iterations;
  result.elapsed_ns = time_once(
      [&] {
        uint64_t digest = kDigestOffset;
        for (uint64_t index = 0; index < arguments.iterations; ++index)
          digest = function(index, digest);
        result.digest = digest;
      },
      result.perf);
  benchmark_sink ^= result.digest;
  return result;
}

struct TransitionCase {
  Game game{0};
  uint64_t action_code = 0;
  int encoded_action = -1;
};

int first_action_index(ActionMaskBits mask, uint64_t random) {
  const size_t count = mcts_action_mask::popcount(mask);
  if (count == 0)
    return -1;
  size_t selected = static_cast<size_t>(random % count);
  int result = -1;
  mcts_action_mask::for_each(mask, [&](size_t action) {
    if (result < 0 && selected-- == 0)
      result = static_cast<int>(action);
  });
  return result;
}

std::vector<TransitionCase> make_transition_corpus(const Fixture &fixture,
                                                   uint64_t seed) {
  std::vector<TransitionCase> corpus;
  corpus.reserve(kTransitionCorpusSize);
  Game game = fixture.game.clone_light();
  uint64_t random = seed ^ 0x9e3779b97f4a7c15ULL;
  uint64_t resets = 0;
  while (corpus.size() < kTransitionCorpusSize) {
    if (game.is_game_over()) {
      game = fixture.game.clone_light();
      if (++resets > kTransitionCorpusSize)
        throw std::runtime_error("fixture cannot produce transition corpus");
    }
    const auto codes = game.legal_action_codes();
    if (codes.empty())
      throw std::runtime_error("non-terminal transition fixture has no action");
    const uint64_t random_value = next_random(random);
    const uint64_t code =
        codes[static_cast<size_t>(random_value % codes.size())];
    const ActionMaskBits mask =
        ActionEncoderCpp::get_action_mask_bits_trusted(game);
    // Normalize the benchmark input independently of whether the engine under
    // test preserved a cache while constructing this trajectory. Individual
    // workloads explicitly prime it when a valid-cache precondition matters.
    game.board.invalidate_hash();
    corpus.push_back(
        {game.clone_light(), code, first_action_index(mask, random_value)});
    if (!game.apply_action_code_trusted(code, false))
      throw std::runtime_error("trusted corpus transition failed");
  }
  return corpus;
}

std::vector<TransitionCase>
make_purchase_transition_corpus(const Fixture &fixture, uint64_t seed) {
  std::vector<TransitionCase> corpus;
  corpus.reserve(kTransitionCorpusSize);
  Game game = fixture.game.clone_light();
  uint64_t random = seed ^ 0xd1b54a32d192ed03ULL;
  uint64_t trajectory_plies = 0;
  uint64_t attempts = 0;

  while (corpus.size() < kTransitionCorpusSize) {
    if (++attempts > 1000000)
      throw std::runtime_error("fixture cannot produce purchase corpus");
    if (game.is_game_over() || trajectory_plies >= 192) {
      game = fixture.game.clone_light();
      trajectory_plies = 0;
    }

    const auto codes = game.legal_action_codes();
    if (codes.empty())
      throw std::runtime_error("purchase corpus state has no legal action");

    std::vector<uint64_t> purchases;
    std::vector<uint64_t> progress;
    purchases.reserve(codes.size());
    progress.reserve(codes.size());
    for (uint64_t code : codes) {
      const Action action = Action::unpack(code);
      if (action.type == PURCHASE) {
        purchases.push_back(code);
      } else if (action.type == VISIT_NOBLE || action.type == TAKE_DIFFERENT ||
                 action.type == TAKE_SAME) {
        progress.push_back(code);
      }
    }

    const uint64_t random_value = next_random(random);
    const auto &choices =
        !purchases.empty() ? purchases : (!progress.empty() ? progress : codes);
    const uint64_t code =
        choices[static_cast<size_t>(random_value % choices.size())];
    if (!purchases.empty()) {
      game.board.invalidate_hash();
      corpus.push_back({game.clone_light(), code, -1});
    }
    if (!game.apply_action_code_trusted(code, false))
      throw std::runtime_error("purchase corpus transition failed");
    ++trajectory_plies;
  }
  return corpus;
}

template <typename Operation>
Result benchmark_transitions(const Arguments &arguments,
                             const std::vector<TransitionCase> &corpus,
                             Operation &&operation) {
  const size_t batch_capacity = 4096;
  Result result;
  result.operations = arguments.iterations;
  uint64_t warmed = 0;
  uint64_t discarded = kDigestOffset;
  while (warmed < arguments.warmup) {
    const size_t batch = static_cast<size_t>(
        std::min<uint64_t>(batch_capacity, arguments.warmup - warmed));
    std::vector<Game> games;
    games.reserve(batch);
    for (size_t index = 0; index < batch; ++index)
      games.push_back(
          corpus[(warmed + index) % corpus.size()].game.clone_light());
    for (size_t index = 0; index < batch; ++index) {
      const TransitionCase &entry = corpus[(warmed + index) % corpus.size()];
      discarded = operation(games[index], entry, discarded);
    }
    warmed += batch;
  }
  benchmark_sink ^= discarded;

  uint64_t completed = 0;
  uint64_t digest = kDigestOffset;
  while (completed < arguments.iterations) {
    const size_t batch = static_cast<size_t>(
        std::min<uint64_t>(batch_capacity, arguments.iterations - completed));
    std::vector<Game> games;
    games.reserve(batch);
    for (size_t index = 0; index < batch; ++index)
      games.push_back(
          corpus[(completed + index) % corpus.size()].game.clone_light());

    result.elapsed_ns += time_once(
        [&] {
          for (size_t index = 0; index < batch; ++index) {
            const TransitionCase &entry =
                corpus[(completed + index) % corpus.size()];
            digest = operation(games[index], entry, digest);
          }
        },
        result.perf);
    completed += batch;
  }
  result.digest = digest;
  benchmark_sink ^= digest;
  return result;
}

uint64_t direct_state_digest(uint64_t digest, const Game &game) noexcept {
  digest = digest_u64(digest, game.board.turn);
  digest = digest_u64(digest, game.board.current_player);
  digest = digest_i64(digest, game.board.winner);
  digest = digest_u64(digest, game.board.players[0].points);
  digest = digest_u64(digest, game.board.players[1].points);
  digest = digest_i64(digest, game.board.visible[0][0]);
  return digest;
}

void append_transition_correctness(Result &result, const Arguments &arguments,
                                   const TransitionCase &entry,
                                   bool decode_action) {
  Game game = entry.game.clone_light();
  uint64_t action_code = entry.action_code;
  bool applied = false;
  if (decode_action) {
    const Action action =
        ActionEncoderCpp::decode_trusted(entry.encoded_action, game);
    action_code = action.pack();
    applied =
        action.type != ACTION_TYPE_COUNT && game.apply_trusted(action, false);
    result.semantics.integer("selected_action_index", entry.encoded_action);
  } else {
    applied = game.apply_action_code(entry.action_code, false);
  }
  if (!applied)
    throw std::runtime_error("transition correctness replay failed");
  const uint64_t exact_hash = game.board.hash();
  const uint64_t recomputed_hash = game.board.compute_hash_uncached();
  if (exact_hash != recomputed_hash)
    throw std::runtime_error("transition exact hash oracle mismatch");
  result.semantics.string("selected_action_code", std::to_string(action_code));
  result.semantics.string("post_board_hash", std::to_string(exact_hash));
  result.semantics.string("post_recomputed_exact_hash",
                          std::to_string(recomputed_hash));
  result.semantics.string("post_observable_hash_0",
                          std::to_string(game.board.observable_hash(0)));
  result.semantics.string("post_observable_hash_1",
                          std::to_string(game.board.observable_hash(1)));
  result.semantics.integer("post_score_0", game.board.players[0].points);
  result.semantics.integer("post_score_1", game.board.players[1].points);
  result.semantics.integer("post_current_player", game.current_player());
  result.semantics.integer("post_turn", game.turn());
  result.semantics.integer("post_winner", game.winner());
  result.semantics.integer("post_legal_count", game.legal_action_count());
  result.semantics.integer("observer_checked", arguments.observer);
}

Result run_legal_count(const Arguments &arguments, const Fixture &fixture) {
  return benchmark_loop(arguments, [&](uint64_t, uint64_t digest) {
    return digest_u64(
        digest, MoveGenerator::count_all_fixed(
                    fixture.game.board, fixture.game.simple_payment_mode));
  });
}

Result run_legal_codes(const Arguments &arguments, const Fixture &fixture) {
  Result result = benchmark_loop(arguments, [&](uint64_t, uint64_t digest) {
    const auto codes = fixture.game.legal_action_codes();
    return digest_codes(codes, digest);
  });
  result.counters.integer("actions_per_call", fixture.legal_count);
  return result;
}

Result run_legal_actions(const Arguments &arguments, const Fixture &fixture) {
  Result result = benchmark_loop(arguments, [&](uint64_t, uint64_t digest) {
    const auto actions = MoveGenerator::generate_all(
        fixture.game.board, fixture.game.simple_payment_mode);
    digest = digest_u64(digest, actions.size());
    for (const Action &action : actions)
      digest = digest_u64(digest, action.pack());
    return digest;
  });
  result.counters.integer("actions_per_call", fixture.legal_count);
  return result;
}

Result run_random_selfplay(const Arguments &arguments) {
  auto perform = [&](uint64_t count, uint64_t seed, uint64_t &resets,
                     uint64_t *final_hash) {
    Game game(seed);
    game.simple_payment_mode = arguments.simple_payment_mode;
    uint64_t random = seed ^ 0xda942042e4dd58b5ULL;
    uint64_t digest = kDigestOffset;
    for (uint64_t index = 0; index < count; ++index) {
      if (game.is_game_over()) {
        game = Game(seed + ++resets);
        game.simple_payment_mode = arguments.simple_payment_mode;
      }
      if (!game.apply_random_action(next_random(random), false))
        throw std::runtime_error("random self-play transition failed");
      digest = direct_state_digest(digest, game);
    }
    if (final_hash)
      *final_hash = game.board.hash();
    return digest;
  };

  uint64_t warmup_resets = 0;
  benchmark_sink ^=
      perform(arguments.warmup, arguments.seed, warmup_resets, nullptr);

  Result result;
  result.operations = arguments.iterations;
  uint64_t resets = 0;
  uint64_t final_hash = 0;
  result.elapsed_ns = time_once(
      [&] {
        result.digest =
            perform(arguments.iterations, arguments.seed, resets, nullptr);
      },
      result.perf);
  uint64_t verification_resets = 0;
  const uint64_t verification_digest = perform(
      arguments.iterations, arguments.seed, verification_resets, &final_hash);
  if (verification_digest != result.digest || verification_resets != resets)
    throw std::runtime_error("random self-play replay is not deterministic");
  result.counters.integer("game_resets", resets);
  result.semantics.string("final_exact_hash", std::to_string(final_hash));
  benchmark_sink ^= result.digest ^ final_hash;
  return result;
}

Result run_apply_only(const Arguments &arguments,
                      const std::vector<TransitionCase> &corpus) {
  Result result = benchmark_transitions(
      arguments, corpus,
      [](Game &game, const TransitionCase &entry, uint64_t digest) {
        if (!game.apply_action_code_trusted(entry.action_code, false))
          throw std::runtime_error("apply_only transition failed");
        return direct_state_digest(digest, game);
      });
  append_transition_correctness(result, arguments, corpus.front(), false);
  return result;
}

Result run_purchase_apply(const Arguments &arguments,
                          const std::vector<TransitionCase> &corpus) {
  if (corpus.empty())
    throw std::runtime_error("purchase_apply corpus is empty");
  Result result = benchmark_transitions(
      arguments, corpus,
      [](Game &game, const TransitionCase &entry, uint64_t digest) {
        if (!game.apply_action_code_trusted(entry.action_code, false))
          throw std::runtime_error("purchase_apply transition failed");
        return direct_state_digest(digest, game);
      });
  append_transition_correctness(result, arguments, corpus.front(), false);
  result.counters.integer("purchase_transitions", corpus.size());
  return result;
}

Result run_apply_exact_hash(const Arguments &arguments,
                            const std::vector<TransitionCase> &corpus) {
  // Incremental maintenance is conditional on the incoming exact cache
  // already being valid. Prime outside the timed region so this workload
  // measures the MCTS-style apply+key path; apply_only and random self-play
  // retain invalid-cache inputs as the no-hash controls.
  std::vector<TransitionCase> primed = corpus;
  for (TransitionCase &entry : primed)
    (void)entry.game.board.hash();
  Result result = benchmark_transitions(
      arguments, primed,
      [](Game &game, const TransitionCase &entry, uint64_t digest) {
        if (!game.apply_action_code_trusted(entry.action_code, false))
          throw std::runtime_error("apply_exact_hash transition failed");
        return digest_u64(digest, game.board.hash());
      });
  append_transition_correctness(result, arguments, primed.front(), false);
  result.semantics.boolean("input_exact_hash_cache_valid", true);
  return result;
}

Result run_apply_observable_hash(const Arguments &arguments,
                                 const std::vector<TransitionCase> &corpus) {
  Result result = benchmark_transitions(
      arguments, corpus,
      [&](Game &game, const TransitionCase &entry, uint64_t digest) {
        if (!game.apply_action_code_trusted(entry.action_code, false))
          throw std::runtime_error("apply_observable_hash transition failed");
        return digest_u64(digest,
                          game.board.observable_hash(
                              static_cast<uint8_t>(arguments.observer)));
      });
  append_transition_correctness(result, arguments, corpus.front(), false);
  return result;
}

std::vector<Game> make_hash_corpus(const Fixture &fixture, uint64_t seed) {
  std::vector<Game> games;
  games.reserve(kTransitionCorpusSize);
  for (size_t index = 0; index < kTransitionCorpusSize; ++index) {
    Game game = fixture.game.shuffled_clone_portable(
        static_cast<uint8_t>(index % Board::NUM_PLAYERS), seed + index);
    // Observable shuffling can occasionally preserve exact order.  The seed
    // and observer still make this a stable, representative cache corpus.
    (void)game.board.hash();
    games.push_back(std::move(game));
  }
  return games;
}

Result run_cold_hash(const Arguments &arguments, const Fixture &fixture) {
  auto games = make_hash_corpus(fixture, arguments.seed);
  return benchmark_loop(arguments, [&](uint64_t index, uint64_t digest) {
    Board &board = games[index % games.size()].board;
    board.invalidate_hash();
    return digest_u64(digest, board.hash());
  });
}

Result run_cached_hash(const Arguments &arguments, const Fixture &fixture) {
  auto games = make_hash_corpus(fixture, arguments.seed);
  for (const Game &game : games)
    (void)game.board.hash();
  return benchmark_loop(arguments, [&](uint64_t index, uint64_t digest) {
    return digest_u64(digest, games[index % games.size()].board.hash());
  });
}

Result run_clone_light(const Arguments &arguments, const Fixture &fixture) {
  return benchmark_loop(arguments, [&](uint64_t, uint64_t digest) {
    const Game clone = fixture.game.clone_light();
    return direct_state_digest(digest, clone);
  });
}

Result run_determinization_clone(const Arguments &arguments,
                                 const Fixture &fixture) {
  return benchmark_loop(arguments, [&](uint64_t index, uint64_t digest) {
    const Game clone = fixture.game.shuffled_clone_portable(
        static_cast<uint8_t>(arguments.observer), arguments.seed + index);
    digest = direct_state_digest(digest, clone);
    for (int level = 0; level < 3; ++level) {
      digest = digest_u64(digest, clone.board.decks[level].size());
      if (!clone.board.decks[level].empty())
        digest = digest_u64(digest, clone.board.decks[level].back());
    }
    return digest;
  });
}

Result run_state_encoder(const Arguments &arguments, const Fixture &fixture) {
  return benchmark_loop(arguments, [&](uint64_t, uint64_t digest) {
    const auto features = StateEncoder::encode_canonical(
        fixture.game, fixture.game.current_player(),
        static_cast<int8_t>(arguments.observer));
    for (float value : features)
      digest = digest_float(digest, value);
    return digest;
  });
}

Result run_action_mask(const Arguments &arguments, const Fixture &fixture) {
  Result result = benchmark_loop(arguments, [&](uint64_t, uint64_t digest) {
    return digest_u64(
        digest, ActionEncoderCpp::get_action_mask_bits_trusted(fixture.game));
  });
  result.counters.integer(
      "mask_popcount",
      mcts_action_mask::popcount(
          ActionEncoderCpp::get_action_mask_bits_trusted(fixture.game)));
  return result;
}

Result run_decode_apply(const Arguments &arguments,
                        const std::vector<TransitionCase> &all_corpus) {
  std::vector<TransitionCase> corpus;
  for (const TransitionCase &entry : all_corpus) {
    if (entry.encoded_action >= 0)
      corpus.push_back(entry);
  }
  if (corpus.empty())
    throw std::runtime_error("fixture has no V1-encodable transition");
  Result result = benchmark_transitions(
      arguments, corpus,
      [](Game &game, const TransitionCase &entry, uint64_t digest) {
        const Action action =
            ActionEncoderCpp::decode_trusted(entry.encoded_action, game);
        if (action.type == ACTION_TYPE_COUNT ||
            !game.apply_trusted(action, false))
          throw std::runtime_error("decode_apply transition failed");
        digest =
            digest_u64(digest, static_cast<uint64_t>(entry.encoded_action));
        return direct_state_digest(digest, game);
      });
  append_transition_correctness(result, arguments, corpus.front(), true);
  return result;
}

MCTSConfig mcts_config(const Arguments &arguments) {
  MCTSConfig config;
  config.use_determinization = arguments.determinization;
  config.num_determinizations = 1;
  config.use_dirichlet_noise = false;
  config.forced_playouts = false;
  config.fpu = 0.0f;
  return config;
}

void apply_fake_inference(MCTS &mcts, const BatchSimulationRequest &request,
                          uint64_t &evaluated_boards) {
  std::array<float, MAX_ACTIONS> policy{};
  for (size_t action = 0; action < policy.size(); ++action)
    policy[action] = static_cast<float>(action + 1);
  const std::array<float, NUM_PLAYERS> value = {0.125f, -0.125f};
  const size_t boards = static_cast<size_t>(request.total_boards);
  mcts.apply_batch_results(
      request, std::vector<std::array<float, MAX_ACTIONS>>(boards, policy),
      std::vector<std::array<float, NUM_PLAYERS>>(boards, value));
  evaluated_boards += boards;
}

struct LegacyFormalTrace {
  uint64_t simulations = 0;
  uint64_t batches = 0;
  uint64_t expansion_requests = 0;
  uint64_t inference_requests = 0;
  uint64_t selected_actions = 0;
  uint64_t expanded_key_sequence_digest = kDigestOffset;
  uint64_t selected_action_sequence_digest = kDigestOffset;
  uint64_t inference_request_sequence_digest = kDigestOffset;
  uint64_t request_replay_trace_digest = kDigestOffset;
};

void digest_legacy_path(const std::vector<PathEntry> &path,
                        LegacyFormalTrace &trace) {
  trace.selected_action_sequence_digest =
      digest_u64(trace.selected_action_sequence_digest, path.size());
  trace.request_replay_trace_digest =
      digest_u64(trace.request_replay_trace_digest, path.size());
  for (const PathEntry &entry : path) {
    trace.selected_action_sequence_digest =
        digest_i64(trace.selected_action_sequence_digest, entry.action);
    trace.request_replay_trace_digest =
        digest_u64(trace.request_replay_trace_digest, entry.hash);
    trace.request_replay_trace_digest =
        digest_i64(trace.request_replay_trace_digest, entry.action);
    trace.request_replay_trace_digest =
        digest_i64(trace.request_replay_trace_digest, entry.player);
    ++trace.selected_actions;
  }
}

void observe_legacy_request(const BatchSimulationRequest &request,
                            LegacyFormalTrace &trace) {
  ++trace.batches;
  trace.expanded_key_sequence_digest =
      digest_u64(trace.expanded_key_sequence_digest, request.leaves.size());
  trace.selected_action_sequence_digest =
      digest_u64(trace.selected_action_sequence_digest, request.leaves.size());
  trace.selected_action_sequence_digest = digest_u64(
      trace.selected_action_sequence_digest, request.terminals.size());
  trace.inference_request_sequence_digest = digest_u64(
      trace.inference_request_sequence_digest, request.leaves.size());
  trace.request_replay_trace_digest =
      digest_u64(trace.request_replay_trace_digest, request.tree_generation);
  trace.request_replay_trace_digest =
      digest_i64(trace.request_replay_trace_digest, request.total_boards);
  trace.request_replay_trace_digest =
      digest_u64(trace.request_replay_trace_digest, request.leaves.size());

  for (const BatchLeafData &leaf : request.leaves) {
    ++trace.expansion_requests;
    ++trace.inference_requests;
    trace.expanded_key_sequence_digest =
        digest_u64(trace.expanded_key_sequence_digest, leaf.hash);
    trace.inference_request_sequence_digest =
        digest_u64(trace.inference_request_sequence_digest, leaf.hash);
    trace.inference_request_sequence_digest =
        digest_i64(trace.inference_request_sequence_digest, leaf.num_worlds);
    trace.inference_request_sequence_digest = digest_u64(
        trace.inference_request_sequence_digest, leaf.encoded_boards.size());
    for (const auto &features : leaf.encoded_boards) {
      for (float feature : features)
        trace.inference_request_sequence_digest =
            digest_float(trace.inference_request_sequence_digest, feature);
    }
    trace.inference_request_sequence_digest = digest_u64(
        trace.inference_request_sequence_digest, leaf.valid_actions.size());
    for (const auto &mask : leaf.valid_actions) {
      trace.inference_request_sequence_digest =
          digest_u64(trace.inference_request_sequence_digest,
                     mcts_action_mask::from_dense(mask));
    }
    trace.request_replay_trace_digest =
        digest_u64(trace.request_replay_trace_digest, leaf.hash);
    trace.request_replay_trace_digest =
        digest_i64(trace.request_replay_trace_digest, leaf.num_worlds);
    digest_legacy_path(leaf.path, trace);
  }

  trace.request_replay_trace_digest =
      digest_u64(trace.request_replay_trace_digest, request.terminals.size());
  for (const auto &terminal : request.terminals) {
    digest_legacy_path(terminal.first, trace);
    for (float value : terminal.second)
      trace.request_replay_trace_digest =
          digest_float(trace.request_replay_trace_digest, value);
  }
}

LegacyFormalTrace legacy_formal_trace(const Arguments &arguments,
                                      const Fixture &fixture) {
  LegacyFormalTrace trace;
  trace.simulations =
      std::min<uint64_t>(arguments.iterations, kFormalTraceSimulations);
  MCTS mcts(mcts_config(arguments), arguments.seed);
  uint64_t completed = 0;
  uint64_t evaluated_boards = 0;
  while (completed < trace.simulations) {
    const int batch = static_cast<int>(std::min<uint64_t>(
        arguments.batch_size, trace.simulations - completed));
    const BatchSimulationRequest request = mcts.prepare_batch_simulations(
        fixture.game, static_cast<uint8_t>(arguments.observer), batch, 1,
        nullptr);
    observe_legacy_request(request, trace);
    apply_fake_inference(mcts, request, evaluated_boards);
    completed += static_cast<uint64_t>(batch);
  }
  trace.request_replay_trace_digest =
      digest_u64(trace.request_replay_trace_digest, evaluated_boards);
  return trace;
}

bool same_legacy_formal_trace(const LegacyFormalTrace &left,
                              const LegacyFormalTrace &right) noexcept {
  return left.simulations == right.simulations &&
         left.batches == right.batches &&
         left.expansion_requests == right.expansion_requests &&
         left.inference_requests == right.inference_requests &&
         left.selected_actions == right.selected_actions &&
         left.expanded_key_sequence_digest ==
             right.expanded_key_sequence_digest &&
         left.selected_action_sequence_digest ==
             right.selected_action_sequence_digest &&
         left.inference_request_sequence_digest ==
             right.inference_request_sequence_digest &&
         left.request_replay_trace_digest == right.request_replay_trace_digest;
}

struct LegacyRun {
  std::unique_ptr<MCTS> owned_mcts;
  MCTS *mcts = nullptr;
  uint64_t evaluated_boards = 0;
  uint64_t leaves = 0;
  uint64_t terminals = 0;
};

LegacyRun legacy_search(const Arguments &arguments, const Fixture &fixture,
                        uint64_t simulations, PerfAccumulator *perf,
                        uint64_t *elapsed_ns, MCTS *retained_mcts = nullptr) {
  LegacyRun run;
  if (retained_mcts) {
    run.mcts = retained_mcts;
  } else {
    run.owned_mcts =
        std::make_unique<MCTS>(mcts_config(arguments), arguments.seed);
    run.mcts = run.owned_mcts.get();
  }
  auto body = [&] {
    uint64_t completed = 0;
    while (completed < simulations) {
      const int batch = static_cast<int>(
          std::min<uint64_t>(arguments.batch_size, simulations - completed));
      const BatchSimulationRequest request =
          run.mcts->prepare_batch_simulations(
              fixture.game, static_cast<uint8_t>(arguments.observer), batch, 1,
              nullptr);
      run.leaves += request.leaves.size();
      run.terminals += request.terminals.size();
      apply_fake_inference(*run.mcts, request, run.evaluated_boards);
      completed += static_cast<uint64_t>(batch);
    }
  };
  if (perf && elapsed_ns)
    *elapsed_ns = time_once(body, *perf);
  else
    body();
  return run;
}

Result run_legacy_mcts(const Arguments &arguments, const Fixture &fixture) {
  if (fixture.game.requires_forced_pass())
    throw std::runtime_error("legacy MCTS fixture requires a forced pass");
  std::unique_ptr<MCTS> retained_mcts;
  if (arguments.retained_tree)
    retained_mcts =
        std::make_unique<MCTS>(mcts_config(arguments), arguments.seed);
  uint32_t root_visits_before = 0;
  uint64_t root_hash = arguments.determinization
                           ? fixture.game.board.observable_hash(
                                 static_cast<uint8_t>(arguments.observer))
                           : fixture.game.board.hash();
  if (fixture.game.simple_payment_mode)
    root_hash ^= 0x6a09e667f3bcc909ULL;
  if (fixture.game.blank_refill_mode)
    root_hash ^= 0xbb67ae8584caa73bULL;
  if (arguments.warmup) {
    const LegacyRun warmup =
        legacy_search(arguments, fixture, arguments.warmup, nullptr, nullptr,
                      retained_mcts.get());
    benchmark_sink ^= warmup.mcts->tree_size();
    if (arguments.retained_tree) {
      const auto root = warmup.mcts->get_node_snapshot(root_hash);
      if (root)
        root_visits_before = root->total_visits;
    }
  }

  Result result;
  result.operations = arguments.iterations;
  LegacyRun run =
      legacy_search(arguments, fixture, arguments.iterations, &result.perf,
                    &result.elapsed_ns, retained_mcts.get());
  const auto root = run.mcts->get_node_snapshot(root_hash);
  if (!root)
    throw std::runtime_error("legacy MCTS did not create its root node");
  if (root->total_visits < root_visits_before)
    throw std::runtime_error("legacy retained root visits regressed");
  const uint64_t measured_root_visits =
      static_cast<uint64_t>(root->total_visits - root_visits_before);

  uint64_t digest = digest_u64(kDigestOffset, root_hash);
  digest = digest_u64(digest, run.mcts->tree_size());
  digest = digest_u64(digest, root->total_visits);
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    digest = digest_u64(digest, root->N[action]);
    digest = digest_float(digest, root->Q[action]);
  }
  result.digest = digest;
  result.counters.integer("tree_size", run.mcts->tree_size());
  result.counters.integer("root_visits", root->total_visits);
  result.counters.integer("root_visits_before", root_visits_before);
  result.counters.integer("measured_root_visits", measured_root_visits);
  result.counters.integer("evaluated_boards", run.evaluated_boards);
  result.counters.integer("leaf_requests", run.leaves);
  result.counters.integer("terminal_paths", run.terminals);
  const LegacyFormalTrace formal = legacy_formal_trace(arguments, fixture);
  const LegacyFormalTrace repeated = legacy_formal_trace(arguments, fixture);
  if (!same_legacy_formal_trace(formal, repeated))
    throw std::runtime_error("legacy formal trace is not deterministic");
  result.semantics.boolean("virtual_loss_balanced", true);
  result.semantics.boolean("retained_tree", arguments.retained_tree);
  result.semantics.string("root_key", std::to_string(root_hash));
  result.semantics.boolean("formal_trace_available", true);
  result.semantics.string("formal_trace_scope",
                          "fresh_tree_untimed_apply_batch_consumption_order");
  result.semantics.string(
      "formal_trace_ordering_limitation",
      "each batch stores leaves before terminals; original mixed simulation "
      "interleave is not exposed");
  result.semantics.integer("formal_trace_simulations", formal.simulations);
  result.semantics.integer("formal_trace_batches", formal.batches);
  result.semantics.integer("formal_trace_expansion_requests",
                           formal.expansion_requests);
  result.semantics.integer("formal_trace_inference_requests",
                           formal.inference_requests);
  result.semantics.integer("formal_trace_selected_actions",
                           formal.selected_actions);
  result.semantics.string("formal_expanded_key_sequence_digest",
                          hex_digest(formal.expanded_key_sequence_digest));
  result.semantics.string("formal_selected_action_sequence_digest",
                          hex_digest(formal.selected_action_sequence_digest));
  result.semantics.string("formal_inference_request_sequence_digest",
                          hex_digest(formal.inference_request_sequence_digest));
  result.semantics.string("formal_request_replay_trace_digest",
                          hex_digest(formal.request_replay_trace_digest));
  result.semantics.boolean("formal_trace_repeatable", true);
  benchmark_sink ^= digest;
  return result;
}

TreeKey exact_tree_key(const Fixture &fixture) {
  uint8_t mode_bits = 0;
  if (fixture.game.simple_payment_mode)
    mode_bits |= MCTS_MODE_SIMPLE_PAYMENT;
  if (fixture.game.blank_refill_mode)
    mode_bits |= MCTS_MODE_BLANK_REFILL;
  return {fixture.game.board.hash(), MCTS_TREE_KEY_VERSION, MCTS_NO_OBSERVER,
          TreeDomain::Exact, mode_bits};
}

struct SharedRun {
  uint64_t digest = kDigestOffset;
  uint64_t total_visits = 0;
  size_t tree_size = 0;
};

SharedRun shared_tree_operations(const Arguments &arguments,
                                 const Fixture &fixture, uint64_t operations,
                                 PerfAccumulator *perf, uint64_t *elapsed_ns) {
  const auto backend = arguments.tree_backend == "coarse"
                           ? mcts_parallel::TreeBackend::Coarse
                           : mcts_parallel::TreeBackend::Sharded;
  mcts_parallel::ConcurrentTree tree(3, backend, 1024, 64);
  const TreeKey key = exact_tree_key(fixture);
  auto node = tree.find_or_create(key);
  mcts_parallel::Policy policy{};
  for (size_t action = 0; action < MAX_ACTIONS; ++action)
    policy[action] = static_cast<float>(action + 1);
  const ActionMaskBits mask =
      ActionEncoderCpp::get_action_mask_bits_trusted(fixture.game);
  if (mask == 0)
    throw std::runtime_error("shared tree fixture has an empty action mask");
  tree.expand_bits(node, policy, mcts_parallel::Value{0.125, -0.125}, mask);
  mcts_parallel::SelectionContext context;
  context.tree_generation = tree.generation();
  context.is_root = true;

  uint64_t digest = kDigestOffset;
  auto body = [&] {
    for (uint64_t index = 0; index < operations; ++index) {
      node = tree.find_or_create(key);
      context.simulation_ordinal = index;
      auto reservation = tree.select_and_reserve_bits(
          node, mask, context, fixture.game.board.current_player, nullptr);
      if (!reservation)
        throw std::runtime_error("shared tree selection returned no action");
      digest = digest_i64(digest, reservation->action());
      reservation->commit(0.125, tree.generation());
    }
  };
  if (perf && elapsed_ns)
    *elapsed_ns = time_once(body, *perf);
  else
    body();
  tree.validate_quiescent_full();
  const auto snapshot = tree.snapshot(node);
  if (snapshot.stats.total_visits != operations)
    throw std::runtime_error("shared tree visit total mismatch");
  digest = digest_u64(digest, snapshot.stats.total_visits);
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    digest = digest_u64(digest, snapshot.stats.N[action]);
    uint64_t q_bits = 0;
    static_assert(sizeof(q_bits) == sizeof(snapshot.stats.Q[action]),
                  "unexpected double size");
    std::memcpy(&q_bits, &snapshot.stats.Q[action], sizeof(q_bits));
    digest = digest_u64(digest, q_bits);
  }
  return {digest, snapshot.stats.total_visits, tree.size()};
}

Result run_shared_tree(const Arguments &arguments, const Fixture &fixture) {
  if (arguments.warmup) {
    const SharedRun warmup = shared_tree_operations(
        arguments, fixture, arguments.warmup, nullptr, nullptr);
    benchmark_sink ^= warmup.digest;
  }
  Result result;
  result.operations = arguments.iterations;
  const SharedRun run =
      shared_tree_operations(arguments, fixture, arguments.iterations,
                             &result.perf, &result.elapsed_ns);
  result.digest = run.digest;
  result.counters.integer("tree_size", run.tree_size);
  result.counters.integer("root_visits", run.total_visits);
  result.semantics.boolean("virtual_loss_balanced", true);
  result.semantics.string("tree_backend", arguments.tree_backend);
  result.semantics.string(
      "root_key", std::to_string(exact_tree_key(fixture).position_hash));
  benchmark_sink ^= result.digest;
  return result;
}

struct ParallelSchedulerRun {
  mcts_parallel::ParallelSearchResult search;
  uint64_t callback_batches = 0;
  uint64_t callback_items = 0;
};

ParallelSchedulerRun
parallel_scheduler_search(const Arguments &arguments, const Fixture &fixture,
                          uint64_t simulations, PerfAccumulator *perf,
                          uint64_t *elapsed_ns, MCTS *retained_mcts = nullptr) {
  std::unique_ptr<MCTS> owned_mcts;
  if (!retained_mcts)
    owned_mcts = std::make_unique<MCTS>(mcts_config(arguments), arguments.seed);
  MCTS &mcts = retained_mcts ? *retained_mcts : *owned_mcts;
  mcts_parallel::ParallelSearchOptions options;
  options.num_threads = arguments.threads;
  options.batch_size = arguments.batch_size;
  options.batch_wait_us = 200;
  options.max_inflight =
      std::max<uint32_t>(arguments.threads * 4, arguments.batch_size * 2);
  options.deterministic_epoch_size = arguments.batch_size;
  options.num_simulations = simulations;
  options.master_seed = arguments.seed;
  options.search_nonce = 0;
  options.evaluator_version = 0x4353504c454e444fULL;
  options.tree_backend = arguments.tree_backend == "coarse"
                             ? mcts_parallel::TreeBackend::Coarse
                             : mcts_parallel::TreeBackend::Sharded;
  options.shard_count = 64;

  std::atomic<uint64_t> callback_batches{0};
  std::atomic<uint64_t> callback_items{0};
  const mcts_parallel::ParallelInferenceFunction inference =
      [&](const std::vector<mcts_parallel::ParallelInferenceRequest>
              &requests) {
        callback_batches.fetch_add(1, std::memory_order_relaxed);
        callback_items.fetch_add(requests.size(), std::memory_order_relaxed);
        if (arguments.latency_us)
          std::this_thread::sleep_for(
              std::chrono::microseconds(arguments.latency_us));
        std::vector<mcts_parallel::ParallelInferenceResult> results(
            requests.size());
        for (auto &result : results) {
          for (size_t action = 0; action < MAX_ACTIONS; ++action)
            result.policy[action] = static_cast<float>(action + 1);
          result.value = {0.125f, -0.125f};
        }
        return results;
      };

  ParallelSchedulerRun run;
  auto body = [&] {
    mcts_parallel::ParallelMCTSSearcher searcher;
    run.search = searcher.run(mcts, fixture.game, options, inference);
  };
  if (perf && elapsed_ns)
    *elapsed_ns = time_once(body, *perf);
  else
    body();
  run.callback_batches = callback_batches.load(std::memory_order_relaxed);
  run.callback_items = callback_items.load(std::memory_order_relaxed);
  return run;
}

uint64_t digest_tree_key(uint64_t digest, const TreeKey &key) noexcept {
  digest = digest_u64(digest, key.position_hash);
  digest = digest_u64(digest, key.key_version);
  digest = digest_u64(digest, key.observer);
  digest = digest_u64(digest, static_cast<uint8_t>(key.domain));
  return digest_u64(digest, key.mode_bits);
}

struct SchedulerFormalTrace {
  uint64_t simulations = 0;
  uint64_t events = 0;
  uint64_t inference_batches = 0;
  uint64_t expansion_requests = 0;
  uint64_t expansion_owners = 0;
  uint64_t selected_actions = 0;
  uint64_t inference_requests = 0;
  uint64_t expanded_key_sequence_digest = kDigestOffset;
  uint64_t selected_action_sequence_digest = kDigestOffset;
  uint64_t inference_request_sequence_digest = kDigestOffset;
  uint64_t trace_chain_digest = kDigestOffset;
  uint64_t replay_tree_digest = kDigestOffset;
  uint64_t initial_tree_digest = kDigestOffset;
};

SchedulerFormalTrace scheduler_formal_trace_once(const Arguments &arguments,
                                                 const Fixture &fixture) {
  SchedulerFormalTrace summary;
  summary.simulations =
      std::min<uint64_t>(arguments.iterations, kFormalTraceSimulations);
  MCTS mcts(mcts_config(arguments), arguments.seed);
  mcts_parallel::ParallelSearchOptions options;
  options.num_threads = 1;
  options.batch_size = arguments.batch_size;
  options.batch_wait_us = 200;
  options.max_inflight =
      std::max<uint32_t>(options.num_threads * 4, options.batch_size * 2);
  // This is the exact epoch size selected internally by the ordinary 1T
  // throughput entry path before it delegates to deterministic epoch mode.
  options.deterministic_epoch_size = options.max_inflight;
  options.num_simulations = summary.simulations;
  options.master_seed = arguments.seed;
  options.search_nonce = 0;
  options.evaluator_version = 0x4353504c454e444fULL;
  options.tree_backend = arguments.tree_backend == "coarse"
                             ? mcts_parallel::TreeBackend::Coarse
                             : mcts_parallel::TreeBackend::Sharded;
  options.shard_count = 64;
  options.mode = mcts_parallel::ParallelSearchMode::DeterministicEpoch;

  const mcts_parallel::ParallelInferenceFunction inference =
      [&summary](const std::vector<mcts_parallel::ParallelInferenceRequest>
                     &requests) {
        ++summary.inference_batches;
        summary.expanded_key_sequence_digest =
            digest_u64(summary.expanded_key_sequence_digest, requests.size());
        summary.inference_request_sequence_digest = digest_u64(
            summary.inference_request_sequence_digest, requests.size());
        for (const auto &request : requests) {
          ++summary.expansion_requests;
          ++summary.inference_requests;
          summary.expanded_key_sequence_digest = digest_tree_key(
              summary.expanded_key_sequence_digest, request.key);
          summary.inference_request_sequence_digest =
              digest_u64(summary.inference_request_sequence_digest,
                         request.owner_simulation_id);
          summary.inference_request_sequence_digest = digest_u64(
              summary.inference_request_sequence_digest, request.pending_id);
          summary.inference_request_sequence_digest = digest_tree_key(
              summary.inference_request_sequence_digest, request.key);
          for (float feature : request.features)
            summary.inference_request_sequence_digest = digest_float(
                summary.inference_request_sequence_digest, feature);
          summary.inference_request_sequence_digest = digest_u64(
              summary.inference_request_sequence_digest,
              mcts_action_mask::from_dense(request.owner_world_mask));
        }
        std::vector<mcts_parallel::ParallelInferenceResult> results(
            requests.size());
        for (auto &result : results) {
          for (size_t action = 0; action < MAX_ACTIONS; ++action)
            result.policy[action] = static_cast<float>(action + 1);
          result.value = {0.125f, -0.125f};
        }
        return results;
      };

  mcts_parallel::DeterministicTrace trace;
  mcts_parallel::ParallelMCTSSearcher searcher;
  const auto search =
      searcher.run(mcts, fixture.game, options, inference, 1.0f, &trace);
  if (search.partial ||
      search.stop_reason != mcts_parallel::SearchStopReason::Completed ||
      search.ledger.completed() != summary.simulations)
    throw std::runtime_error("scheduler formal trace did not complete");
  trace.verify();
  summary.events = trace.events.size();
  summary.initial_tree_digest = trace.initial_tree_digest;
  summary.trace_chain_digest = trace.previous_chain;
  summary.replay_tree_digest = mcts_parallel::replay_deterministic_trace(trace);
  if (!trace.events.empty() &&
      summary.replay_tree_digest != trace.events.back().tree_digest)
    throw std::runtime_error("scheduler formal trace replay digest mismatch");

  summary.selected_action_sequence_digest =
      digest_u64(summary.selected_action_sequence_digest, trace.events.size());
  for (const auto &event : trace.events) {
    summary.selected_action_sequence_digest = digest_u64(
        summary.selected_action_sequence_digest, event.simulation_id);
    summary.selected_action_sequence_digest =
        digest_u64(summary.selected_action_sequence_digest, event.path.size());
    for (const auto &step : event.path) {
      summary.selected_action_sequence_digest =
          digest_i64(summary.selected_action_sequence_digest, step.action);
      ++summary.selected_actions;
    }
    if (event.ticket.leaf_role == mcts_parallel::TraceLeafRole::Owner) {
      ++summary.expansion_owners;
    }
  }
  summary.expanded_key_sequence_digest = digest_u64(
      summary.expanded_key_sequence_digest, summary.expansion_requests);
  summary.inference_request_sequence_digest = digest_u64(
      summary.inference_request_sequence_digest, summary.inference_requests);
  return summary;
}

bool same_scheduler_formal_trace(const SchedulerFormalTrace &left,
                                 const SchedulerFormalTrace &right) noexcept {
  return left.simulations == right.simulations && left.events == right.events &&
         left.inference_batches == right.inference_batches &&
         left.expansion_requests == right.expansion_requests &&
         left.expansion_owners == right.expansion_owners &&
         left.selected_actions == right.selected_actions &&
         left.inference_requests == right.inference_requests &&
         left.expanded_key_sequence_digest ==
             right.expanded_key_sequence_digest &&
         left.selected_action_sequence_digest ==
             right.selected_action_sequence_digest &&
         left.inference_request_sequence_digest ==
             right.inference_request_sequence_digest &&
         left.trace_chain_digest == right.trace_chain_digest &&
         left.replay_tree_digest == right.replay_tree_digest &&
         left.initial_tree_digest == right.initial_tree_digest;
}

Result run_parallel_scheduler(const Arguments &arguments,
                              const Fixture &fixture) {
  if (fixture.game.requires_forced_pass())
    throw std::runtime_error(
        "parallel scheduler fixture requires a forced pass");
  std::unique_ptr<MCTS> retained_mcts;
  if (arguments.retained_tree)
    retained_mcts =
        std::make_unique<MCTS>(mcts_config(arguments), arguments.seed);
  uint64_t root_visits_before = 0;
  if (arguments.warmup) {
    const ParallelSchedulerRun warmup =
        parallel_scheduler_search(arguments, fixture, arguments.warmup, nullptr,
                                  nullptr, retained_mcts.get());
    benchmark_sink ^= warmup.search.ledger.completed();
    if (arguments.retained_tree) {
      root_visits_before =
          std::accumulate(warmup.search.visits.begin(),
                          warmup.search.visits.end(), uint64_t{0});
    }
  }

  Result result;
  const ParallelSchedulerRun run = parallel_scheduler_search(
      arguments, fixture, arguments.iterations, &result.perf,
      &result.elapsed_ns, retained_mcts.get());
  const auto &ledger = run.search.ledger;
  const uint64_t root_visits = std::accumulate(
      run.search.visits.begin(), run.search.visits.end(), uint64_t{0});
  if (ledger.completed() != arguments.iterations)
    throw std::runtime_error("parallel scheduler completion total mismatch");
  if (!ledger.virtual_loss_balanced())
    throw std::runtime_error("parallel scheduler retained virtual loss");
  if (ledger.failed != 0 || ledger.cancelled != 0 ||
      ledger.integrity_errors != 0 || ledger.invalid_replay != 0 ||
      ledger.duplicate_result != 0 || ledger.stale_result != 0)
    throw std::runtime_error("parallel scheduler ledger integrity failure");
  if (root_visits < root_visits_before ||
      root_visits - root_visits_before != ledger.completed())
    throw std::runtime_error("parallel scheduler root visit total mismatch");
  const uint64_t measured_root_visits = root_visits - root_visits_before;
  if (run.search.stop_reason != mcts_parallel::SearchStopReason::Completed ||
      run.search.partial)
    throw std::runtime_error("parallel scheduler returned a partial result");
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  const auto perf_value = [&](csplendor::perf::Counter counter) {
    return result.perf.values[static_cast<size_t>(counter)];
  };
  const uint64_t ledger_field_increment_sum =
      ledger.issued + ledger.selected + ledger.evaluation_owner +
      ledger.evaluation_waiter + ledger.evaluation_requested +
      ledger.evaluated_boards + ledger.completed_evaluated +
      ledger.completed_terminal + ledger.completed_max_depth +
      ledger.cancelled + ledger.failed + ledger.virtual_loss_added +
      ledger.virtual_loss_released + ledger.reservations_committed +
      ledger.reservations_aborted + ledger.expansion_claimed +
      ledger.expansion_published + ledger.expansion_waited +
      ledger.stale_result + ledger.duplicate_result + ledger.invalid_replay +
      ledger.integrity_errors;
  const uint64_t ledger_group_increment_sum =
      perf_value(
          csplendor::perf::Counter::ParallelLedgerIssuanceAtomicIncrements) +
      perf_value(
          csplendor::perf::Counter::ParallelLedgerSelectionAtomicIncrements) +
      perf_value(
          csplendor::perf::Counter::ParallelLedgerEvaluationAtomicIncrements) +
      perf_value(
          csplendor::perf::Counter::ParallelLedgerCompletionAtomicIncrements) +
      perf_value(
          csplendor::perf::Counter::ParallelLedgerReservationAtomicIncrements) +
      perf_value(csplendor::perf::Counter::ParallelLedgerErrorAtomicIncrements);
  if (perf_value(csplendor::perf::Counter::ParallelLedgerAtomicIncrements) !=
          ledger_group_increment_sum ||
      ledger_group_increment_sum != ledger_field_increment_sum) {
    throw std::runtime_error(
        "parallel scheduler instrumentation ledger totals disagree");
  }
  if (perf_value(csplendor::perf::Counter::ParallelBatchCount) !=
          run.callback_batches ||
      perf_value(csplendor::perf::Counter::ParallelBatchItems) !=
          run.callback_items) {
    throw std::runtime_error(
        "parallel scheduler instrumentation batch totals disagree");
  }
#endif

  uint64_t visit_digest = kDigestOffset;
  uint64_t q_digest = kDigestOffset;
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    visit_digest = digest_u64(visit_digest, run.search.visits[action]);
    uint64_t q_bits = 0;
    std::memcpy(&q_bits, &run.search.q_values[action], sizeof(q_bits));
    q_digest = digest_u64(q_digest, q_bits);
  }
  uint64_t digest = digest_u64(kDigestOffset, ledger.completed());
  digest = digest_u64(digest, measured_root_visits);
  digest = digest_u64(
      digest, ActionEncoderCpp::get_action_mask_bits_trusted(fixture.game));
  const uint64_t root_position_hash =
      arguments.determinization
          ? fixture.game.board.observable_hash(
                static_cast<uint8_t>(fixture.game.current_player()))
          : fixture.game.board.hash();
  digest = digest_u64(digest, root_position_hash);
  digest = digest_u64(digest, arguments.determinization ? 1 : 0);
  digest = digest_u64(digest, arguments.threads);
  digest = digest_u64(digest, arguments.batch_size);
  digest = digest_u64(digest, arguments.latency_us);
  digest = digest_u64(digest, run.search.resolved_seed);
  digest = digest_u64(digest, run.search.search_nonce);
  digest = digest_u64(digest, run.search.tree_generation);
  digest = digest_u64(digest, static_cast<uint64_t>(run.search.stop_reason));
  result.operations = ledger.completed();
  result.digest = digest;

  result.counters.integer("tree_size", run.search.tree_size);
  result.counters.integer("root_visits", root_visits);
  result.counters.integer("root_visits_before", root_visits_before);
  result.counters.integer("measured_root_visits", measured_root_visits);
  result.counters.string("root_visit_digest", hex_digest(visit_digest));
  result.counters.string("root_q_digest", hex_digest(q_digest));
  result.counters.integer("issued", ledger.issued);
  result.counters.integer("completed", ledger.completed());
  result.counters.integer("completed_evaluated", ledger.completed_evaluated);
  result.counters.integer("completed_terminal", ledger.completed_terminal);
  result.counters.integer("completed_max_depth", ledger.completed_max_depth);
  result.counters.integer("selected", ledger.selected);
  result.counters.integer("evaluation_owners", ledger.evaluation_owner);
  result.counters.integer("inference_requests", ledger.evaluation_requested);
  result.counters.integer("inference_waiters", ledger.evaluation_waiter);
  result.counters.integer("evaluated_boards", ledger.evaluated_boards);
  result.counters.integer("callback_batches", run.callback_batches);
  result.counters.integer("callback_items", run.callback_items);
  result.counters.integer("max_inflight", ledger.max_inflight_observed);
  result.counters.integer("virtual_loss_added", ledger.virtual_loss_added);
  result.counters.integer("virtual_loss_released",
                          ledger.virtual_loss_released);
  result.counters.integer("reservations_committed",
                          ledger.reservations_committed);
  result.counters.integer("reservations_aborted", ledger.reservations_aborted);
  result.counters.integer("expansion_claimed", ledger.expansion_claimed);
  result.counters.integer("expansion_published", ledger.expansion_published);
  result.counters.integer("expansion_waited", ledger.expansion_waited);
  result.counters.integer("cancelled", ledger.cancelled);
  result.counters.integer("failed", ledger.failed);
  result.counters.integer("stale_result", ledger.stale_result);
  result.counters.integer("duplicate_result", ledger.duplicate_result);
  result.counters.integer("invalid_replay", ledger.invalid_replay);
  result.counters.integer("integrity_errors", ledger.integrity_errors);

  result.semantics.boolean("completed_exact", true);
  result.semantics.boolean("virtual_loss_balanced", true);
  result.semantics.boolean("ledger_integrity_ok", true);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  result.semantics.boolean("ledger_instrumentation_totals_match", true);
#endif
  result.semantics.boolean("retained_tree", arguments.retained_tree);
  result.semantics.boolean("partial", false);
  result.semantics.integer("stop_reason",
                           static_cast<uint64_t>(run.search.stop_reason));
  result.semantics.integer("threads", arguments.threads);
  result.semantics.integer("batch_size", arguments.batch_size);
  result.semantics.integer("latency_us", arguments.latency_us);
  result.semantics.string("tree_backend", arguments.tree_backend);
  result.semantics.string("tree_domain",
                          arguments.determinization ? "observable" : "exact");
  result.semantics.string("root_key", std::to_string(root_position_hash));
  if (arguments.threads == 1) {
    const SchedulerFormalTrace formal =
        scheduler_formal_trace_once(arguments, fixture);
    const SchedulerFormalTrace repeated =
        scheduler_formal_trace_once(arguments, fixture);
    if (!same_scheduler_formal_trace(formal, repeated))
      throw std::runtime_error("scheduler formal trace is not deterministic");
    result.semantics.boolean("formal_trace_available", true);
    result.semantics.string(
        "formal_trace_scope",
        "fresh_tree_untimed_deterministic_epoch_1t_owner_leaf_order");
    result.semantics.string(
        "formal_trace_latency_model",
        "latency_sleep_omitted_evaluator_outputs_identical");
    result.semantics.integer("formal_trace_simulations", formal.simulations);
    result.semantics.integer("formal_trace_events", formal.events);
    result.semantics.integer("formal_trace_inference_batches",
                             formal.inference_batches);
    result.semantics.integer("formal_trace_expansion_requests",
                             formal.expansion_requests);
    result.semantics.integer("formal_trace_expansion_owners",
                             formal.expansion_owners);
    result.semantics.integer("formal_trace_selected_actions",
                             formal.selected_actions);
    result.semantics.integer("formal_trace_inference_requests",
                             formal.inference_requests);
    result.semantics.string("formal_expanded_key_sequence_digest",
                            hex_digest(formal.expanded_key_sequence_digest));
    result.semantics.string("formal_selected_action_sequence_digest",
                            hex_digest(formal.selected_action_sequence_digest));
    result.semantics.string(
        "formal_inference_request_sequence_digest",
        hex_digest(formal.inference_request_sequence_digest));
    result.semantics.string("formal_deterministic_trace_chain_digest",
                            hex_digest(formal.trace_chain_digest));
    result.semantics.string("formal_replay_tree_digest",
                            hex_digest(formal.replay_tree_digest));
    result.semantics.string("formal_initial_tree_digest",
                            hex_digest(formal.initial_tree_digest));
    result.semantics.boolean("formal_trace_repeatable", true);
  } else {
    result.semantics.boolean("formal_trace_available", false);
    result.semantics.string(
        "formal_trace_unavailable_reason",
        "formal deterministic trace is defined only for threads=1");
  }
  benchmark_sink ^= result.digest;
  return result;
}

struct RootParallelRun {
  mcts_parallel::RootParallelResult search;
  uint64_t callback_batches = 0;
  uint64_t callback_items = 0;
};

RootParallelRun root_parallel_search(const Arguments &arguments,
                                     const Fixture &fixture,
                                     uint64_t simulations,
                                     PerfAccumulator *perf,
                                     uint64_t *elapsed_ns) {
  MCTSConfig config = mcts_config(arguments);
  mcts_parallel::ParallelSearchOptions options;
  options.batch_size = arguments.batch_size;
  options.batch_wait_us = 200;
  options.deterministic_epoch_size = arguments.batch_size;
  options.master_seed = arguments.seed;
  options.search_nonce = 0;
  options.evaluator_version = 0x4353504c454e444fULL;
  // Root parallelism uses independent 1T worker trees. Keep the worker
  // backend aligned with the established CSV benchmark rather than coupling
  // this workload to the shared-tree backend argument.
  options.tree_backend = mcts_parallel::TreeBackend::Coarse;
  options.shard_count = 64;

  std::atomic<uint64_t> callback_batches{0};
  std::atomic<uint64_t> callback_items{0};
  const mcts_parallel::ParallelEvaluatorFactory factory =
      [&](uint32_t) -> mcts_parallel::ParallelInferenceFunction {
    return [&](const std::vector<mcts_parallel::ParallelInferenceRequest>
                   &requests) {
      callback_batches.fetch_add(1, std::memory_order_relaxed);
      callback_items.fetch_add(requests.size(), std::memory_order_relaxed);
      if (arguments.latency_us)
        std::this_thread::sleep_for(
            std::chrono::microseconds(arguments.latency_us));
      std::vector<mcts_parallel::ParallelInferenceResult> results(
          requests.size());
      for (auto &inference : results) {
        for (size_t action = 0; action < MAX_ACTIONS; ++action)
          inference.policy[action] = static_cast<float>(action + 1);
        inference.value = {0.1f, -0.1f};
      }
      return results;
    };
  };

  RootParallelRun run;
  auto body = [&] {
    run.search = mcts_parallel::run_root_parallel(
        config, fixture.game, simulations, arguments.threads, options, factory);
  };
  if (perf && elapsed_ns)
    *elapsed_ns = time_once(body, *perf);
  else
    body();
  run.callback_batches = callback_batches.load(std::memory_order_relaxed);
  run.callback_items = callback_items.load(std::memory_order_relaxed);
  return run;
}

void validate_root_parallel_run(const RootParallelRun &run,
                                uint64_t simulations,
                                uint32_t requested_workers) {
  const auto &merged = run.search.merged;
  const auto &ledger = merged.ledger;
  if (run.search.workers.size() != requested_workers)
    throw std::runtime_error("root-parallel worker result count mismatch");
  if (ledger.completed() != simulations || ledger.issued != simulations)
    throw std::runtime_error("root-parallel completion total mismatch");
  if (!ledger.virtual_loss_balanced())
    throw std::runtime_error("root-parallel retained virtual loss");
  if (ledger.failed != 0 || ledger.cancelled != 0 ||
      ledger.integrity_errors != 0 || ledger.invalid_replay != 0 ||
      ledger.duplicate_result != 0 || ledger.stale_result != 0)
    throw std::runtime_error("root-parallel ledger integrity failure");
  if (merged.stop_reason != mcts_parallel::SearchStopReason::Completed ||
      merged.partial)
    throw std::runtime_error("root-parallel returned a partial result");

  const uint64_t root_visits =
      std::accumulate(merged.visits.begin(), merged.visits.end(), uint64_t{0});
  if (root_visits != simulations)
    throw std::runtime_error("root-parallel root visit total mismatch");

  uint64_t worker_tree_size = 0;
  uint64_t worker_completed = 0;
  const uint64_t quotient = simulations / requested_workers;
  const uint64_t remainder = simulations % requested_workers;
  for (uint32_t worker_index = 0; worker_index < requested_workers;
       ++worker_index) {
    const auto &worker = run.search.workers[worker_index];
    const uint64_t expected =
        quotient + (worker_index < remainder ? uint64_t{1} : uint64_t{0});
    if (worker.ledger.completed() != expected ||
        !worker.ledger.virtual_loss_balanced())
      throw std::runtime_error("root-parallel worker budget mismatch");
    if (expected != 0 &&
        (worker.stop_reason != mcts_parallel::SearchStopReason::Completed ||
         worker.partial))
      throw std::runtime_error("root-parallel worker returned partial result");
    const uint64_t worker_root_visits = std::accumulate(
        worker.visits.begin(), worker.visits.end(), uint64_t{0});
    if (worker_root_visits != expected)
      throw std::runtime_error("root-parallel worker root visits mismatch");
    worker_completed += worker.ledger.completed();
    worker_tree_size += worker.tree_size;
  }
  if (worker_completed != simulations || worker_tree_size != merged.tree_size)
    throw std::runtime_error("root-parallel worker merge mismatch");

  const uint64_t active_workers =
      std::min<uint64_t>(requested_workers, simulations);
  if (run.search.duplicate_root_evaluations_avoided + 1 != active_workers)
    throw std::runtime_error(
        "root-parallel duplicate-root accounting mismatch");
}

Result run_root_parallel(const Arguments &arguments, const Fixture &fixture) {
  if (fixture.game.requires_forced_pass())
    throw std::runtime_error("root-parallel fixture requires a forced pass");
  if (arguments.retained_tree)
    throw std::invalid_argument(
        "root_parallel uses independent warmup and does not retain trees");

  if (arguments.warmup) {
    const RootParallelRun warmup = root_parallel_search(
        arguments, fixture, arguments.warmup, nullptr, nullptr);
    validate_root_parallel_run(warmup, arguments.warmup, arguments.threads);
    benchmark_sink ^= warmup.search.merged.tree_size ^
                      warmup.search.merged.ledger.completed();
  }

  Result result;
  const RootParallelRun run =
      root_parallel_search(arguments, fixture, arguments.iterations,
                           &result.perf, &result.elapsed_ns);
  validate_root_parallel_run(run, arguments.iterations, arguments.threads);

  const auto &merged = run.search.merged;
  const auto &ledger = merged.ledger;
  const uint64_t root_visits =
      std::accumulate(merged.visits.begin(), merged.visits.end(), uint64_t{0});
  const auto best =
      std::max_element(merged.visits.begin(), merged.visits.end());
  const size_t selected_action =
      static_cast<size_t>(std::distance(merged.visits.begin(), best));
  const ActionMaskBits root_mask =
      ActionEncoderCpp::get_action_mask_bits_trusted(fixture.game);
  if (root_visits == 0 ||
      !mcts_action_mask::contains(root_mask, selected_action))
    throw std::runtime_error("root-parallel selected an illegal root action");
  const Action selected = ActionEncoderCpp::decode_trusted(
      static_cast<int>(selected_action), fixture.game);
  if (selected.type == ACTION_TYPE_COUNT || !fixture.game.is_legal(selected))
    throw std::runtime_error("root-parallel selected action failed decoding");

  uint64_t visit_digest = kDigestOffset;
  uint64_t q_digest = kDigestOffset;
  uint64_t probability_digest = kDigestOffset;
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    visit_digest = digest_u64(visit_digest, merged.visits[action]);
    uint64_t q_bits = 0;
    std::memcpy(&q_bits, &merged.q_values[action], sizeof(q_bits));
    q_digest = digest_u64(q_digest, q_bits);
    probability_digest =
        digest_float(probability_digest, merged.probabilities[action]);
  }

  uint64_t worker_digest = kDigestOffset;
  for (size_t worker_index = 0; worker_index < run.search.workers.size();
       ++worker_index) {
    const auto &worker = run.search.workers[worker_index];
    worker_digest = digest_u64(worker_digest, worker_index);
    worker_digest = digest_u64(worker_digest, worker.ledger.completed());
    worker_digest = digest_u64(worker_digest, worker.tree_size);
    worker_digest =
        digest_u64(worker_digest, worker.ledger.evaluation_requested);
    worker_digest = digest_u64(worker_digest, worker.ledger.evaluation_waiter);
    for (size_t action = 0; action < MAX_ACTIONS; ++action) {
      worker_digest = digest_u64(worker_digest, worker.visits[action]);
      uint64_t q_bits = 0;
      std::memcpy(&q_bits, &worker.q_values[action], sizeof(q_bits));
      worker_digest = digest_u64(worker_digest, q_bits);
    }
  }

  const uint8_t root_observer =
      static_cast<uint8_t>(fixture.game.current_player());
  const uint64_t root_position_hash =
      arguments.determinization
          ? fixture.game.board.observable_hash(root_observer)
          : fixture.game.board.hash();
  uint64_t digest = digest_u64(kDigestOffset, ledger.completed());
  digest = digest_u64(digest, merged.tree_size);
  digest = digest_u64(digest, ledger.evaluation_requested);
  digest = digest_u64(digest, ledger.evaluation_waiter);
  digest = digest_u64(digest, root_visits);
  digest = digest_u64(digest, selected_action);
  digest = digest_u64(digest, selected.pack());
  digest = digest_u64(digest, visit_digest);
  digest = digest_u64(digest, q_digest);
  digest = digest_u64(digest, probability_digest);
  digest = digest_u64(digest, worker_digest);
  digest = digest_u64(digest, root_position_hash);
  digest = digest_u64(digest, arguments.determinization ? 1 : 0);
  digest = digest_u64(digest, arguments.threads);
  digest = digest_u64(digest, arguments.batch_size);
  digest = digest_u64(digest, arguments.latency_us);
  digest = digest_u64(digest, arguments.seed);
  digest = digest_u64(digest, merged.resolved_seed);
  digest = digest_u64(digest, merged.search_nonce);
  digest = digest_u64(digest, merged.tree_generation);
  digest = digest_u64(digest, run.search.duplicate_root_evaluations_avoided);

  result.operations = ledger.completed();
  result.digest = digest;
  result.counters.integer("tree_size", merged.tree_size);
  result.counters.integer("root_visits", root_visits);
  result.counters.integer("root_visits_before", 0);
  result.counters.integer("measured_root_visits", root_visits);
  result.counters.string("root_visit_digest", hex_digest(visit_digest));
  result.counters.string("root_q_digest", hex_digest(q_digest));
  result.counters.integer("issued", ledger.issued);
  result.counters.integer("completed", ledger.completed());
  result.counters.integer("completed_evaluated", ledger.completed_evaluated);
  result.counters.integer("completed_terminal", ledger.completed_terminal);
  result.counters.integer("completed_max_depth", ledger.completed_max_depth);
  result.counters.integer("selected", ledger.selected);
  result.counters.integer("evaluation_owners", ledger.evaluation_owner);
  result.counters.integer("inference_requests", ledger.evaluation_requested);
  result.counters.integer("inference_waiters", ledger.evaluation_waiter);
  result.counters.integer("evaluated_boards", ledger.evaluated_boards);
  result.counters.integer("callback_batches", run.callback_batches);
  result.counters.integer("callback_items", run.callback_items);
  result.counters.integer("max_inflight", ledger.max_inflight_observed);
  result.counters.integer("virtual_loss_added", ledger.virtual_loss_added);
  result.counters.integer("virtual_loss_released",
                          ledger.virtual_loss_released);
  result.counters.integer("reservations_committed",
                          ledger.reservations_committed);
  result.counters.integer("reservations_aborted", ledger.reservations_aborted);
  result.counters.integer("expansion_claimed", ledger.expansion_claimed);
  result.counters.integer("expansion_published", ledger.expansion_published);
  result.counters.integer("expansion_waited", ledger.expansion_waited);
  result.counters.integer("cancelled", ledger.cancelled);
  result.counters.integer("failed", ledger.failed);
  result.counters.integer("stale_result", ledger.stale_result);
  result.counters.integer("duplicate_result", ledger.duplicate_result);
  result.counters.integer("invalid_replay", ledger.invalid_replay);
  result.counters.integer("integrity_errors", ledger.integrity_errors);

  result.semantics.boolean("completed_exact", true);
  result.semantics.boolean("virtual_loss_balanced", true);
  result.semantics.boolean("ledger_integrity_ok", true);
  result.semantics.boolean("worker_budgets_exact", true);
  result.semantics.boolean("worker_tree_merge_exact", true);
  result.semantics.boolean("warmup_independent", true);
  result.semantics.boolean("retained_tree", false);
  result.semantics.boolean("partial", false);
  result.semantics.integer("stop_reason",
                           static_cast<uint64_t>(merged.stop_reason));
  result.semantics.integer("threads", arguments.threads);
  result.semantics.integer(
      "active_workers",
      std::min<uint64_t>(arguments.threads, arguments.iterations));
  result.semantics.integer("batch_size", arguments.batch_size);
  result.semantics.integer("latency_us", arguments.latency_us);
  result.semantics.integer("master_seed", arguments.seed);
  result.semantics.integer("resolved_seed", merged.resolved_seed);
  result.semantics.integer("search_nonce", merged.search_nonce);
  result.semantics.integer("rng_version", merged.rng_version);
  result.semantics.integer("tree_generation", merged.tree_generation);
  result.semantics.integer("duplicate_root_evaluations_avoided",
                           run.search.duplicate_root_evaluations_avoided);
  result.semantics.integer("selected_action_index", selected_action);
  result.semantics.string("selected_action_code",
                          std::to_string(selected.pack()));
  result.semantics.integer("selected_action_visits", *best);
  result.semantics.boolean("selected_action_legal", true);
  result.semantics.string("root_probability_digest",
                          hex_digest(probability_digest));
  result.semantics.string("worker_result_digest", hex_digest(worker_digest));
  result.semantics.string("tree_backend", "coarse_independent_worker_trees");
  result.semantics.string("tree_domain",
                          arguments.determinization ? "observable" : "exact");
  result.semantics.integer("root_observer", root_observer);
  result.semantics.string("root_key", std::to_string(root_position_hash));
  benchmark_sink ^= result.digest;
  return result;
}

Result run_board_copy_restore(const Arguments &arguments,
                              const Fixture &fixture) {
  const auto codes = fixture.game.legal_action_codes();
  if (codes.empty())
    throw std::runtime_error("copy/restore fixture has no legal action");
  Game game = fixture.game.clone_light();
  const uint64_t expected_hash = game.board.hash();
  Result result =
      benchmark_loop(arguments, [&](uint64_t index, uint64_t digest) {
        CSPLENDOR_PERF_INC(BoardSnapshotCopies);
        const Board previous = game.board;
        const uint64_t code = codes[index % codes.size()];
        if (!game.apply_action_code_trusted(code, false))
          throw std::runtime_error("copy/restore transition failed");
        digest = direct_state_digest(digest, game);
        CSPLENDOR_PERF_INC(BoardRestores);
        CSPLENDOR_PERF_INC(SolverBoardRollbacks);
        game.board = previous;
        return digest;
      });
  const bool restored = game.board.hash() == expected_hash &&
                        game.board.compute_hash_uncached() == expected_hash;
  if (!restored)
    throw std::runtime_error("Board copy/restore did not restore fixture");
  result.semantics.boolean("restored_exactly", true);
  return result;
}

using SolverBenchmarkKey = csplendor::solver_internal::RevealDepthStateKey;
using SolverBenchmarkKeyHash =
    csplendor::solver_internal::RevealDepthStateKeyHash;
using SolverBenchmarkExactKey =
    csplendor::solver_internal::RevealExactDepthStateKey;
using SolverBenchmarkExactKeyHash =
    csplendor::solver_internal::RevealExactDepthStateKeyHash;
using SolverBenchmarkEntry = csplendor::solver_internal::RevealPersistentEntry;

SolverBenchmarkKey make_solver_benchmark_key(
    const Game &game,
    const csplendor::solver_internal::HiddenOutcomeCatalog &catalog, int depth,
    const csplendor::solver_internal::RevealSearchState *search_state = nullptr,
    bool root_independent = false) {
  CSPLENDOR_PERF_INC(SolverStateKeyCalls);
  const Board &board = game.board;
  csplendor::solver_internal::CardIdSet unseen;
  csplendor::solver_internal::CardIdSet acquired_hidden;
  uint64_t board_hash = 0;
  if (search_state != nullptr && search_state->active()) {
    CSPLENDOR_PERF_INC(SolverRevealStateFastKeyReads);
    unseen = search_state->remaining_all();
    acquired_hidden = root_independent ? csplendor::solver_internal::CardIdSet{}
                                       : search_state->acquired_hidden();
    board_hash = search_state->rule_hash();
  } else {
    unseen = catalog.unseen_cards(board);
    acquired_hidden = root_independent ? csplendor::solver_internal::CardIdSet{}
                                       : catalog.acquired_hidden_cards(board);
    board_hash = board.compute_set_deck_search_hash();
  }
  return {csplendor::solver_internal::RevealStateKey{
              csplendor::solver_internal::StateKeyCore{
                  board_hash, board.players[0].points, board.players[1].points,
                  board.players[0].purchased_count,
                  board.players[1].purchased_count, board.final_round,
                  board.winner},
              unseen.low, unseen.high, acquired_hidden.low,
              acquired_hidden.high, board.players[0].reserved_count,
              board.players[1].reserved_count},
          depth};
}

uint64_t digest_solver_key(uint64_t digest,
                           const SolverBenchmarkKey &key) noexcept {
  digest = digest_u64(digest, key.state.board_hash());
  digest = digest_u64(digest, key.state.metadata_bits());
  digest = digest_u64(digest, key.state.unseen_low());
  digest = digest_u64(digest, key.state.unseen_high());
  digest = digest_u64(digest, key.state.acquired_hidden_low());
  digest = digest_u64(digest, key.state.acquired_hidden_high());
  return digest_i64(digest, key.depth);
}

Result run_solver_state_key(const Arguments &arguments,
                            const Fixture &fixture) {
  csplendor::solver_internal::HiddenOutcomeCatalog catalog;
  catalog.remember_initial_position(fixture.game);
  csplendor::solver_internal::RevealSearchState search_state;
  search_state.initialize(fixture.game, catalog);
  const SolverBenchmarkKey oracle = make_solver_benchmark_key(
      fixture.game, catalog, arguments.depth, nullptr);
  const SolverBenchmarkKey repeated = make_solver_benchmark_key(
      fixture.game, catalog, arguments.depth, &search_state);
  if (!(oracle == repeated) ||
      SolverBenchmarkKeyHash{}(oracle) != SolverBenchmarkKeyHash{}(repeated))
    throw std::runtime_error("solver state-key oracle is unstable");

  Result result = benchmark_loop(arguments, [&](uint64_t, uint64_t digest) {
    const SolverBenchmarkKey key = make_solver_benchmark_key(
        fixture.game, catalog, arguments.depth, &search_state);
    return digest_solver_key(digest, key);
  });
  result.semantics.boolean("state_key_repeatable", true);
  result.semantics.integer("state_key_depth", arguments.depth);
  result.semantics.string("state_key_hash",
                          hex_digest(SolverBenchmarkKeyHash{}(oracle)));
  result.counters.integer("deck_cards", fixture.game.board.decks[0].size() +
                                            fixture.game.board.decks[1].size() +
                                            fixture.game.board.decks[2].size());
  result.counters.integer(
      "purchased_cards",
      fixture.game.board.players[0].purchased_cards.size() +
          fixture.game.board.players[1].purchased_cards.size());
  return result;
}

Result run_solver_tt(const Arguments &arguments,
                     const std::vector<TransitionCase> &transitions,
                     const Fixture &fixture) {
  csplendor::solver_internal::HiddenOutcomeCatalog catalog;
  catalog.remember_initial_position(fixture.game);
  std::vector<SolverBenchmarkExactKey> keys;
  keys.reserve(transitions.size());
  for (size_t index = 0; index < transitions.size(); ++index) {
    const SolverBenchmarkKey full_key = make_solver_benchmark_key(
        transitions[index].game, catalog,
        arguments.depth - static_cast<int>(index % 3), nullptr, true);
    keys.emplace_back(full_key.state, full_key.depth);
  }
  using SolverBenchmarkTable =
      std::unordered_map<SolverBenchmarkExactKey, SolverBenchmarkEntry,
                         SolverBenchmarkExactKeyHash>;
  SolverBenchmarkTable table;
  table.reserve(keys.size() * 2);
  const auto make_entry = [](uint64_t index) {
    SolverBenchmarkEntry entry{
        index % 3 == 0 ? csplendor::solver_internal::ForceStatus::PROVEN
                       : csplendor::solver_internal::ForceStatus::REFUTED,
        index + 1,
        static_cast<int>(index % CARD_COUNT),
        true,
        static_cast<size_t>(index % MAX_MOVES) + 1,
        true};
    entry.set_generation(index / 7 + 1);
    entry.set_last_touched(index + 1);
    return entry;
  };
  for (size_t index = 0; index < keys.size(); ++index)
    table.insert_or_assign(keys[index], make_entry(index));
  if (table.empty())
    throw std::runtime_error("solver TT corpus is empty");

  uint64_t hit_count = 0;
  Result result =
      benchmark_loop(arguments, [&](uint64_t index, uint64_t digest) {
        const SolverBenchmarkExactKey &lookup_key = keys[index % keys.size()];
        CSPLENDOR_PERF_TT_PROBE_SCOPE(probe_scope);
        const auto found = table.find(lookup_key);
        CSPLENDOR_PERF_TT_PROBE_FINISH(probe_scope);
        if (found == table.end())
          throw std::runtime_error("solver TT expected lookup miss");
        CSPLENDOR_PERF_INC(SolverTtHits);
        ++hit_count;
        const SolverBenchmarkEntry &entry = found->second;
        digest = digest_u64(digest, static_cast<uint8_t>(entry.status()));
        digest = digest_u64(digest, entry.action_code());
        digest = digest_i64(digest, entry.reveal_card());
        digest = digest_u64(digest, entry.has_action() ? 1 : 0);
        digest = digest_u64(digest, entry.action_count());
        digest = digest_u64(digest, entry.replayable() ? 1 : 0);
        digest = digest_u64(digest, entry.generation());
        digest = digest_u64(digest, entry.last_touched());

        const SolverBenchmarkExactKey &store_key =
            keys[(index * 17 + 3) % keys.size()];
        CSPLENDOR_PERF_INC(SolverTtStores);
        table.insert_or_assign(store_key, make_entry(index + keys.size()));
        return digest_u64(digest, table.size());
      });
  // benchmark_loop includes warmup calls in the local total.
  const uint64_t expected_hits = arguments.warmup + arguments.iterations;
  if (hit_count != expected_hits)
    throw std::runtime_error("solver TT hit total mismatch");
  for (const SolverBenchmarkExactKey &key : keys) {
    if (table.find(key) == table.end())
      throw std::runtime_error("solver TT lost a corpus key");
  }
  result.counters.integer("tt_entries", table.size());
  result.counters.integer("tt_probes", arguments.iterations);
  result.counters.integer("tt_hits", arguments.iterations);
  result.counters.integer("tt_stores", arguments.iterations);
  result.semantics.boolean("all_tt_lookups_hit", true);
  result.semantics.string("tt_key_kind", "exact_reveal_depth_state");
  return result;
}

uint64_t digest_visible_result(const VisibleOnlySearchResult &search) {
  uint64_t digest = digest_i64(kDigestOffset, search.winner);
  digest = digest_i64(digest, search.forced_win_depth);
  digest = digest_u64(digest, search.stats.nodes);
  digest = digest_u64(digest, search.stats.legal_moves);
  digest = digest_u64(digest, search.stats.memo_hits);
  digest = digest_u64(digest, search.stats.terminal_nodes);
  digest = digest_u64(digest, search.memoized_states);
  for (const auto &entry : search.line) {
    digest = digest_u64(digest, entry.action_code);
    digest = digest_i64(digest, entry.winner);
    digest = digest_u64(digest, entry.action_count);
  }
  return digest;
}

uint64_t
digest_visible_principal_line_actions(const VisibleOnlySearchResult &search) {
  uint64_t digest = digest_u64(kDigestOffset, search.line.size());
  for (const auto &entry : search.line)
    digest = digest_u64(digest, entry.action_code);
  return digest;
}

Result run_visible_solver(const Arguments &arguments, const Fixture &fixture) {
  if (arguments.warmup) {
    VisibleOnlySolver warmup_solver(arguments.warmup,
                                    arguments.time_limit_seconds);
    const auto warmup = warmup_solver.solve(fixture.game);
    benchmark_sink ^= digest_visible_result(warmup);
  }
  Result result;
  VisibleOnlySearchResult search;
  result.elapsed_ns = time_once(
      [&] {
        VisibleOnlySolver solver(arguments.iterations,
                                 arguments.time_limit_seconds);
        search = solver.solve(fixture.game);
      },
      result.perf);
  result.operations = search.stats.nodes;
  result.digest = digest_visible_result(search);
  result.counters.integer("requested_node_limit", arguments.iterations);
  result.counters.integer("nodes", search.stats.nodes);
  result.counters.integer("legal_moves", search.stats.legal_moves);
  result.counters.integer("terminal_nodes", search.stats.terminal_nodes);
  result.counters.integer("memo_hits", search.stats.memo_hits);
  result.counters.integer("memoized_states", search.memoized_states);
  result.semantics.integer("winner", search.winner);
  result.semantics.integer("forced_win_depth", search.forced_win_depth);
  result.semantics.string("reason", search.winner_reason);
  result.semantics.string("unknown_reason", search.unknown_reason);
  result.semantics.boolean("limit_reached", !search.unknown_reason.empty());
  result.semantics.integer("principal_line_length", search.line.size());
  result.semantics.string(
      "principal_line_action_sequence_digest",
      hex_digest(digest_visible_principal_line_actions(search)));
  result.semantics.string("ordered_action_observable_scope",
                          "public_principal_line_only");
  result.semantics.boolean("full_solver_action_order_available", false);
  result.semantics.string(
      "full_solver_action_order_unavailable_reason",
      "visible solver does not expose internal ordered-action candidates");
  result.semantics.string(
      "winning_root_action",
      search.line.empty() ? ""
                          : std::to_string(search.line.front().action_code));
  benchmark_sink ^= result.digest;
  return result;
}

uint64_t digest_reveal_result(const RevealVerifiedSearchResult &search) {
  uint64_t digest = digest_u64(kDigestOffset, search.proven ? 1 : 0);
  digest = digest_i64(digest, search.attacker);
  digest = digest_i64(digest, search.depth);
  digest = digest_u64(digest, search.stats.nodes);
  digest = digest_u64(digest, search.stats.legal_moves);
  digest = digest_u64(digest, search.stats.memo_hits);
  digest = digest_u64(digest, search.stats.terminal_nodes);
  digest = digest_u64(digest, search.stats.reveal_branches);
  digest = digest_u64(digest, search.memoized_states);
  for (const auto &entry : search.line) {
    digest = digest_u64(digest, entry.action_code);
    digest = digest_i64(digest, entry.reveal_card);
    digest = digest_u64(digest, entry.action_count);
  }
  const auto &dag = search.proof_dag;
  digest = digest_u64(digest, dag.requested ? 1 : 0);
  digest = digest_u64(digest, dag.complete ? 1 : 0);
  digest = digest_u64(digest, dag.validated ? 1 : 0);
  digest = digest_u64(digest, dag.root);
  digest = digest_string(digest, dag.omitted_reason);
  digest = digest_u64(digest, dag.nodes.size());
  for (const auto &node : dag.nodes) {
    digest = digest_u64(digest, node.id);
    digest = digest_i64(digest, node.player);
    digest = digest_i64(digest, node.depth);
    digest = digest_i64(digest, node.winner);
    digest = digest_string(digest, node.kind);
    digest = digest_string(digest, node.resolution);
    digest = digest_u64(digest, node.children.size());
    for (const auto &edge : node.children) {
      digest = digest_u64(digest, edge.action_code);
      digest = digest_i64(digest, edge.reveal_card);
      digest = digest_i64(digest, edge.oracle_card);
      digest = digest_u64(digest, edge.oracle_reserve ? 1 : 0);
      digest = digest_i64(digest, edge.oracle_reserve_card);
      digest = digest_i64(digest, edge.oracle_return_color);
      for (uint8_t gold : edge.oracle_gold_as)
        digest = digest_u64(digest, gold);
      digest = digest_u64(digest, edge.child);
    }
  }
  return digest;
}

struct RevealOrderObservables {
  size_t ordered_actions = 0;
  size_t ordered_outcomes = 0;
  size_t reveal_candidates = 0;
  uint64_t ordered_action_sequence_digest = kDigestOffset;
  uint64_t ordered_outcome_sequence_digest = kDigestOffset;
  uint64_t reveal_candidate_sequence_digest = kDigestOffset;
};

RevealOrderObservables
observe_reveal_root_order(const RevealVerifiedFrontierResult &frontier) {
  RevealOrderObservables observable;
  observable.ordered_outcomes = frontier.edges.size();
  observable.ordered_outcome_sequence_digest = digest_u64(
      observable.ordered_outcome_sequence_digest, frontier.edges.size());
  uint64_t previous_action = 0;
  bool have_previous_action = false;
  for (const RevealVerifiedFrontierEdge &edge : frontier.edges) {
    if (!have_previous_action || edge.action_code != previous_action) {
      ++observable.ordered_actions;
      observable.ordered_action_sequence_digest = digest_u64(
          observable.ordered_action_sequence_digest, edge.action_code);
      previous_action = edge.action_code;
      have_previous_action = true;
    }
    observable.ordered_outcome_sequence_digest = digest_u64(
        observable.ordered_outcome_sequence_digest, edge.action_code);
    observable.ordered_outcome_sequence_digest = digest_i64(
        observable.ordered_outcome_sequence_digest, edge.reveal_card);
    observable.ordered_outcome_sequence_digest = digest_i64(
        observable.ordered_outcome_sequence_digest, edge.child_depth);
    if (edge.reveal_card >= 0) {
      ++observable.reveal_candidates;
      observable.reveal_candidate_sequence_digest = digest_u64(
          observable.reveal_candidate_sequence_digest, edge.action_code);
      observable.reveal_candidate_sequence_digest = digest_i64(
          observable.reveal_candidate_sequence_digest, edge.reveal_card);
    }
  }
  observable.ordered_action_sequence_digest = digest_u64(
      observable.ordered_action_sequence_digest, observable.ordered_actions);
  observable.reveal_candidate_sequence_digest =
      digest_u64(observable.reveal_candidate_sequence_digest,
                 observable.reveal_candidates);
  return observable;
}

uint64_t
digest_reveal_principal_line_actions(const RevealVerifiedSearchResult &search) {
  uint64_t digest = digest_u64(kDigestOffset, search.line.size());
  for (const auto &entry : search.line)
    digest = digest_u64(digest, entry.action_code);
  return digest;
}

uint64_t digest_reveal_principal_line_outcomes(
    const RevealVerifiedSearchResult &search) {
  uint64_t digest = digest_u64(kDigestOffset, search.line.size());
  for (const auto &entry : search.line) {
    digest = digest_u64(digest, entry.action_code);
    digest = digest_i64(digest, entry.reveal_card);
  }
  return digest;
}

Result run_exact_reveal(const Arguments &arguments, const Fixture &fixture) {
  const int attacker = arguments.attacker < 0 ? fixture.game.current_player()
                                              : arguments.attacker;
  const std::vector<uint64_t> reusable_preferences =
      fixture.required_root_action == UINT64_MAX
          ? std::vector<uint64_t>{}
          : std::vector<uint64_t>{fixture.required_root_action};
  const auto make_solver = [&](uint64_t node_limit) {
    return std::make_unique<RevealVerifiedSolver>(
        attacker, arguments.depth, node_limit, arguments.time_limit_seconds,
        std::vector<uint64_t>{}, arguments.proof_dag, 100000, 500000,
        fixture.required_root_action, false, 0, false, true);
  };

  std::unique_ptr<RevealVerifiedSolver> solver;
  RevealVerifiedSearchResult prime;
  size_t cache_states_before = 0;
  if (arguments.persistent_reuse) {
    solver = make_solver(arguments.iterations);
    prime = solver->solve_reusing_exact_cache(
        fixture.game, arguments.depth, arguments.iterations,
        arguments.time_limit_seconds, reusable_preferences);
    cache_states_before = solver->exact_cache_size();
    benchmark_sink ^= digest_reveal_result(prime);
  } else {
    if (arguments.warmup) {
      auto warmup_solver = make_solver(arguments.warmup);
      const auto warmup = warmup_solver->solve(fixture.game);
      benchmark_sink ^= digest_reveal_result(warmup);
    }
    solver = make_solver(arguments.iterations);
  }
  Result result;
  RevealVerifiedSearchResult search;
  result.elapsed_ns = time_once(
      [&] {
        if (arguments.persistent_reuse) {
          search = solver->solve_reusing_exact_cache(
              fixture.game, arguments.depth, arguments.iterations,
              arguments.time_limit_seconds, reusable_preferences);
        } else {
          search = solver->solve(fixture.game);
        }
      },
      result.perf);
  // The public split_root API materializes proof_ordered_actions followed by
  // every proof outcome.  Probe it with an independent unlimited solver so
  // neither the timed solver nor its persistent cache/statistics are changed.
  RevealVerifiedSolver root_order_solver(
      attacker, arguments.depth, 0, 0.0, std::vector<uint64_t>{}, false, 100000,
      500000, fixture.required_root_action, false, 0, false, true);
  const RevealVerifiedFrontierResult root_order =
      root_order_solver.split_root(fixture.game, 1000000);
  if (!root_order.complete)
    throw std::runtime_error("exact solver root order probe failed: " +
                             root_order.unknown_reason);
  const RevealOrderObservables order_observables =
      observe_reveal_root_order(root_order);
  result.operations = search.stats.nodes;
  result.digest = digest_reveal_result(search);
  result.counters.integer("requested_node_limit", arguments.iterations);
  result.counters.integer("nodes", search.stats.nodes);
  result.counters.integer("legal_moves", search.stats.legal_moves);
  result.counters.integer("terminal_nodes", search.stats.terminal_nodes);
  result.counters.integer("memo_hits", search.stats.memo_hits);
  result.counters.integer("persistent_memo_hits",
                          search.stats.persistent_memo_hits);
  result.counters.integer("memoized_states", search.memoized_states);
  result.counters.integer("persistent_cache_states_before",
                          cache_states_before);
  result.counters.integer("persistent_cache_states_after",
                          solver->exact_cache_size());
  result.counters.integer("reveal_branches", search.stats.reveal_branches);
  result.counters.integer("deck_reserve_candidates",
                          search.stats.deck_reserve_candidates);
  result.counters.integer("deck_reserve_branches",
                          search.stats.deck_reserve_branches);
  result.semantics.boolean("proven", search.proven);
  result.semantics.integer("attacker", search.attacker);
  result.semantics.integer("depth", search.depth);
  result.semantics.string("reason", search.reason);
  result.semantics.string("unknown_reason", search.unknown_reason);
  result.semantics.boolean("limit_reached", !search.unknown_reason.empty());
  result.semantics.boolean("exact_reveal_search", true);
  result.semantics.boolean("persistent_reuse", arguments.persistent_reuse);
  result.semantics.boolean("proof_dag_requested", arguments.proof_dag);
  result.semantics.boolean("proof_dag_complete", search.proof_dag.complete);
  result.semantics.boolean("proof_dag_validated", search.proof_dag.validated);
  const std::string proof_dag_omitted_reason =
      arguments.proof_dag && arguments.persistent_reuse &&
              search.proof_dag.omitted_reason.empty()
          ? "persistent reuse API disables proof DAG materialization"
          : search.proof_dag.omitted_reason;
  result.semantics.string("proof_dag_omitted_reason", proof_dag_omitted_reason);
  result.semantics.integer("proof_dag_nodes", search.proof_dag.nodes.size());
  size_t proof_edges = 0;
  for (const auto &node : search.proof_dag.nodes)
    proof_edges += node.children.size();
  result.semantics.integer("proof_dag_edges", proof_edges);
  if (arguments.persistent_reuse) {
    result.semantics.boolean("prime_proven", prime.proven);
    result.semantics.string("prime_reason", prime.reason);
    result.semantics.string("prime_unknown_reason", prime.unknown_reason);
    result.semantics.integer("prime_nodes", prime.stats.nodes);
  }
  result.semantics.string("required_root_action",
                          fixture.required_root_action == UINT64_MAX
                              ? ""
                              : std::to_string(fixture.required_root_action));
  result.semantics.string("required_root_action_mode",
                          fixture.required_root_action == UINT64_MAX ? "none"
                          : arguments.persistent_reuse
                              ? "preferred_public_reuse_api"
                              : "strict");
  result.semantics.integer("principal_line_length", search.line.size());
  result.semantics.string(
      "principal_line_action_sequence_digest",
      hex_digest(digest_reveal_principal_line_actions(search)));
  result.semantics.string(
      "principal_line_reveal_outcome_sequence_digest",
      hex_digest(digest_reveal_principal_line_outcomes(search)));
  result.semantics.boolean("root_order_probe_complete", true);
  result.semantics.integer("root_ordered_action_count",
                           order_observables.ordered_actions);
  result.semantics.integer("root_ordered_outcome_count",
                           order_observables.ordered_outcomes);
  result.semantics.integer("root_reveal_candidate_count",
                           order_observables.reveal_candidates);
  result.semantics.string(
      "root_ordered_action_sequence_digest",
      hex_digest(order_observables.ordered_action_sequence_digest));
  result.semantics.string(
      "root_ordered_outcome_sequence_digest",
      hex_digest(order_observables.ordered_outcome_sequence_digest));
  result.semantics.string(
      "root_reveal_candidate_sequence_digest",
      hex_digest(order_observables.reveal_candidate_sequence_digest));
  result.semantics.string(
      "root_order_observable_scope",
      "public_split_root_proof_ordered_actions_then_proof_outcomes");
  result.semantics.string(
      "winning_root_action",
      search.line.empty() ? ""
                          : std::to_string(search.line.front().action_code));
  benchmark_sink ^= result.digest;
  return result;
}

struct MoveListBoundaryObservables {
  uint64_t sampled_states = 0;
  size_t max_retained = 0;
  uint64_t saturated_states = 0;
  size_t editor_retained = 0;
  bool editor_saturated = false;
};

MoveListBoundaryObservables observe_move_list_boundaries() {
  constexpr uint64_t corpus_seeds = 64;
  constexpr int max_plies = 100;
  MoveListBoundaryObservables observable;
  for (uint64_t seed = 0; seed < corpus_seeds; ++seed) {
    Game game(seed);
    uint64_t random = seed ^ 0x517cc1b727220a95ULL;
    for (int ply = 0; ply <= max_plies && !game.is_game_over(); ++ply) {
      const MoveList moves = MoveGenerator::generate_all_fixed(
          game.board, game.simple_payment_mode);
      ++observable.sampled_states;
      observable.max_retained = std::max(observable.max_retained, moves.size());
      observable.saturated_states += moves.size() == MAX_MOVES ? 1 : 0;
      if (ply == max_plies)
        break;
      if (!game.apply_random_action(next_random(random), false))
        throw std::runtime_error(
            "layout probe reachable corpus transition failed");
    }
  }

  Game editor(42);
  Board &board = editor.board.begin_editor_mutation();
  board.players[0].gems = {3, 3, 3, 3, 3, 3};
  board.players[0].sync_packed();
  const MoveList editor_moves =
      MoveGenerator::generate_all_fixed(editor.board, false);
  observable.editor_retained = editor_moves.size();
  observable.editor_saturated = editor_moves.size() == MAX_MOVES;
  return observable;
}

struct CloneAllocationObservables {
  bool available = false;
  uint64_t full_calls = 0;
  uint64_t full_bytes = 0;
  uint64_t light_calls = 0;
  uint64_t light_bytes = 0;
};

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
template <typename CloneFunction>
std::pair<uint64_t, uint64_t>
measure_clone_allocations(CloneFunction &&clone_function) {
  benchmark_allocation_instrumentation::reset_and_enable();
  try {
    {
      const Game clone = clone_function();
      benchmark_sink ^= clone.board.hash();
    }
  } catch (...) {
    benchmark_allocation_instrumentation::disable();
    throw;
  }
  benchmark_allocation_instrumentation::disable();
  uint64_t calls = 0;
  uint64_t bytes = 0;
  for (size_t index = 0;
       index < benchmark_allocation_instrumentation::kKindCount; ++index) {
    calls += benchmark_allocation_instrumentation::calls[index].load(
        std::memory_order_relaxed);
    bytes += benchmark_allocation_instrumentation::bytes[index].load(
        std::memory_order_relaxed);
  }
  return {calls, bytes};
}
#endif

CloneAllocationObservables observe_clone_allocations(const Fixture &fixture) {
  CloneAllocationObservables observable;
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  observable.available = true;
  const auto full =
      measure_clone_allocations([&fixture] { return fixture.game.clone(); });
  observable.full_calls = full.first;
  observable.full_bytes = full.second;
  const auto light = measure_clone_allocations(
      [&fixture] { return fixture.game.clone_light(); });
  observable.light_calls = light.first;
  observable.light_bytes = light.second;
#else
  (void)fixture;
#endif
  return observable;
}

Result run_layout_probe(const Arguments &, const Fixture &fixture) {
  const MoveListBoundaryObservables move_lists = observe_move_list_boundaries();
  const csplendor::solver_internal::SolverTtLayoutMetrics solver_tt =
      csplendor::solver_internal::solver_tt_layout_metrics();
  Result result;
  result.operations = 1;
  result.elapsed_ns = time_once(
      [&] {
        uint64_t digest = kDigestOffset;
        digest = digest_u64(digest, sizeof(Action));
        digest = digest_u64(digest, sizeof(MoveList));
        digest = digest_u64(digest, sizeof(PlayerState));
        digest = digest_u64(digest, sizeof(Board));
        digest = digest_u64(digest, sizeof(Game));
        digest = digest_u64(digest, sizeof(MCTSNode));
        digest = digest_u64(digest, solver_tt.reveal_state_key);
        digest = digest_u64(digest, solver_tt.reveal_depth_key);
        digest = digest_u64(digest, solver_tt.reveal_exact_state_key);
        digest = digest_u64(digest, solver_tt.reveal_exact_depth_key);
        digest = digest_u64(digest, solver_tt.reveal_memo_entry);
        digest = digest_u64(digest, solver_tt.reveal_persistent_entry);
        digest = digest_u64(digest, solver_tt.reveal_memo_value);
        digest = digest_u64(digest, solver_tt.reveal_persistent_value);
        digest = digest_u64(digest, solver_tt.reveal_proof_node_value);
        digest = digest_u64(digest, solver_tt.visible_state_key);
        digest = digest_u64(digest, solver_tt.visible_depth_key);
        digest = digest_u64(digest, solver_tt.visible_memo_entry);
        digest = digest_u64(digest, solver_tt.visible_force_entry);
        digest = digest_u64(digest, solver_tt.visible_force_bounds);
        digest = digest_u64(digest, solver_tt.visible_memo_value);
        digest = digest_u64(digest, solver_tt.visible_force_value);
        digest = digest_u64(digest, solver_tt.visible_bounds_value);
        digest = digest_u64(digest, move_lists.sampled_states);
        digest = digest_u64(digest, move_lists.max_retained);
        digest = digest_u64(digest, move_lists.saturated_states);
        digest = digest_u64(digest, move_lists.editor_retained);
        digest = digest_u64(digest, move_lists.editor_saturated ? 1 : 0);
        result.digest = digest;
      },
      result.perf);

  const CloneAllocationObservables clones = observe_clone_allocations(fixture);
  result.digest = digest_u64(result.digest, clones.available ? 1 : 0);
  if (clones.available) {
    result.digest = digest_u64(result.digest, clones.full_calls);
    result.digest = digest_u64(result.digest, clones.full_bytes);
    result.digest = digest_u64(result.digest, clones.light_calls);
    result.digest = digest_u64(result.digest, clones.light_bytes);
  }

  result.semantics.integer("sizeof_action", sizeof(Action));
  result.semantics.integer("sizeof_move_list", sizeof(MoveList));
  result.semantics.integer("sizeof_player_state", sizeof(PlayerState));
  result.semantics.integer("sizeof_board", sizeof(Board));
  result.semantics.integer("sizeof_game", sizeof(Game));
  result.semantics.integer("sizeof_mcts_node", sizeof(MCTSNode));
  result.semantics.integer("sizeof_reveal_state_key",
                           solver_tt.reveal_state_key);
  result.semantics.integer("sizeof_reveal_depth_key",
                           solver_tt.reveal_depth_key);
  result.semantics.integer("sizeof_reveal_exact_state_key",
                           solver_tt.reveal_exact_state_key);
  result.semantics.integer("sizeof_reveal_exact_depth_key",
                           solver_tt.reveal_exact_depth_key);
  result.semantics.integer("sizeof_reveal_memo_entry",
                           solver_tt.reveal_memo_entry);
  result.semantics.integer("sizeof_reveal_persistent_entry",
                           solver_tt.reveal_persistent_entry);
  result.semantics.integer("sizeof_reveal_memo_value",
                           solver_tt.reveal_memo_value);
  result.semantics.integer("sizeof_reveal_persistent_value",
                           solver_tt.reveal_persistent_value);
  result.semantics.integer("sizeof_reveal_proof_node_value",
                           solver_tt.reveal_proof_node_value);
  result.semantics.integer("sizeof_visible_state_key",
                           solver_tt.visible_state_key);
  result.semantics.integer("sizeof_visible_depth_key",
                           solver_tt.visible_depth_key);
  result.semantics.integer("sizeof_visible_memo_entry",
                           solver_tt.visible_memo_entry);
  result.semantics.integer("sizeof_visible_force_entry",
                           solver_tt.visible_force_entry);
  result.semantics.integer("sizeof_visible_force_bounds",
                           solver_tt.visible_force_bounds);
  result.semantics.integer("sizeof_visible_memo_value",
                           solver_tt.visible_memo_value);
  result.semantics.integer("sizeof_visible_force_value",
                           solver_tt.visible_force_value);
  result.semantics.integer("sizeof_visible_bounds_value",
                           solver_tt.visible_bounds_value);
  result.semantics.integer("move_list_capacity", MAX_MOVES);
  result.semantics.integer("reachable_random_corpus_seeds", 64);
  result.semantics.integer("reachable_random_corpus_max_plies", 100);
  result.semantics.integer("reachable_move_list_sampled_states",
                           move_lists.sampled_states);
  result.semantics.integer("reachable_move_list_max_retained",
                           move_lists.max_retained);
  result.semantics.integer("reachable_move_list_saturated_states",
                           move_lists.saturated_states);
  result.semantics.integer("editor_move_list_retained",
                           move_lists.editor_retained);
  result.semantics.boolean("editor_move_list_saturated",
                           move_lists.editor_saturated);
  result.semantics.boolean("move_list_overflow_attempts_available", false);
  result.semantics.string(
      "move_list_overflow_attempts_unavailable_reason",
      "production enumeration stops at MAX_MOVES before post-cap attempts");
  result.semantics.boolean("clone_allocation_counts_available",
                           clones.available);
  if (clones.available) {
    result.semantics.integer("clone_full_allocation_calls", clones.full_calls);
    result.semantics.integer("clone_full_allocation_bytes", clones.full_bytes);
    result.semantics.integer("clone_light_allocation_calls",
                             clones.light_calls);
    result.semantics.integer("clone_light_allocation_bytes",
                             clones.light_bytes);
  } else {
    result.semantics.string("clone_allocation_counts_unavailable_reason",
                            "requires CSPLENDOR_PERF_INSTRUMENTATION build");
  }
  benchmark_sink ^= result.digest;
  return result;
}

void append_perf_counters(Result &result) {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  result.counters.boolean("instrumentation_enabled", true);
  constexpr std::array<const char *,
                       benchmark_allocation_instrumentation::kKindCount>
      allocation_names = {"global_new", "global_new_array",
                          "global_aligned_new", "global_aligned_new_array"};
  uint64_t total_allocation_calls = 0;
  uint64_t total_allocation_bytes = 0;
  for (size_t index = 0; index < allocation_names.size(); ++index) {
    result.counters.integer(std::string(allocation_names[index]) + "_calls",
                            result.perf.allocation_calls[index]);
    result.counters.integer(std::string(allocation_names[index]) + "_bytes",
                            result.perf.allocation_bytes[index]);
    total_allocation_calls += result.perf.allocation_calls[index];
    total_allocation_bytes += result.perf.allocation_bytes[index];
  }
  result.counters.integer("global_allocation_calls", total_allocation_calls);
  result.counters.integer("global_allocation_bytes", total_allocation_bytes);
  for (size_t index = 0; index < result.perf.values.size(); ++index) {
    const auto counter = static_cast<csplendor::perf::Counter>(index);
    result.counters.integer(csplendor::perf::counter_name(counter),
                            result.perf.values[index]);
  }
#else
  result.counters.boolean("instrumentation_enabled", false);
#endif
}

void emit_result(const std::string &workload, const Arguments &arguments,
                 const Fixture &fixture, Result result) {
  append_perf_counters(result);
  result.semantics.boolean("correct", true);
  result.semantics.string("fixture_category", fixture.category);
  result.semantics.string("fixture_construction", fixture.construction);
  result.semantics.boolean("fixture_canonical", fixture.canonical);
  result.semantics.boolean("fixture_editor_compatible",
                           fixture.editor_compatible);
  result.semantics.string("fixture_reachable_invariants",
                          fixture.reachable_invariant_report);
  result.semantics.string("fixture_editor_invariants",
                          fixture.editor_invariant_report);
  result.semantics.string("fixture_exact_hash",
                          std::to_string(fixture.exact_hash));
  result.semantics.string("fixture_observable_hash_0",
                          std::to_string(fixture.observable_hashes[0]));
  result.semantics.string("fixture_observable_hash_1",
                          std::to_string(fixture.observable_hashes[1]));
  result.semantics.integer("fixture_legal_count", fixture.legal_count);
  result.semantics.string("fixture_ordered_legal_digest",
                          hex_digest(fixture.ordered_legal_digest));
  result.semantics.integer("fixture_setup_actions", fixture.setup_actions);
  result.semantics.integer("observer", arguments.observer);
  result.semantics.boolean("simple_payment_mode",
                           arguments.simple_payment_mode);
  result.semantics.boolean("determinization", arguments.determinization);
  result.semantics.integer("warmup_operations", arguments.warmup);
  result.semantics.string("rss_kind", "current_resident_set");

  const double rate = result.elapsed_ns == 0
                          ? 0.0
                          : static_cast<double>(result.operations) * 1.0e9 /
                                static_cast<double>(result.elapsed_ns);
  JsonObject output;
  output.string("schema", kSchema);
  output.string("workload", workload);
  output.string("fixture", fixture.name);
  output.integer("seed", fixture.seed);
  output.integer("operations", result.operations);
  output.integer("elapsed_ns", result.elapsed_ns);
  output.number("rate_per_second", rate);
  output.integer("rss_kib", current_rss_kib());
  output.string("digest", hex_digest(result.digest));
  output.raw("counters", result.counters.render());
  output.raw("semantics", result.semantics.render());
  std::cout << output.render() << '\n';
}

bool known_workload(const std::string &name) {
  // layout_probe is an opt-in characterization workload.  Keep it out of the
  // formal `all` performance suite so adding structural diagnostics cannot
  // silently change an existing baseline command.
  if (name == "layout_probe")
    return true;
  const auto names = all_workloads();
  return std::find(names.begin(), names.end(), name) != names.end();
}

Result dispatch(const std::string &workload, const Arguments &arguments,
                const Fixture &fixture,
                const std::vector<TransitionCase> &transitions,
                const std::vector<TransitionCase> &purchase_transitions) {
  if (workload == "legal_count")
    return run_legal_count(arguments, fixture);
  if (workload == "legal_codes")
    return run_legal_codes(arguments, fixture);
  if (workload == "legal_actions")
    return run_legal_actions(arguments, fixture);
  if (workload == "random_selfplay_apply")
    return run_random_selfplay(arguments);
  if (workload == "apply_only")
    return run_apply_only(arguments, transitions);
  if (workload == "purchase_apply")
    return run_purchase_apply(arguments, purchase_transitions);
  if (workload == "apply_exact_hash")
    return run_apply_exact_hash(arguments, transitions);
  if (workload == "apply_observable_hash")
    return run_apply_observable_hash(arguments, transitions);
  if (workload == "cold_hash")
    return run_cold_hash(arguments, fixture);
  if (workload == "cached_hash")
    return run_cached_hash(arguments, fixture);
  if (workload == "clone_light")
    return run_clone_light(arguments, fixture);
  if (workload == "determinization_clone")
    return run_determinization_clone(arguments, fixture);
  if (workload == "state_encoder")
    return run_state_encoder(arguments, fixture);
  if (workload == "action_mask")
    return run_action_mask(arguments, fixture);
  if (workload == "decode_apply")
    return run_decode_apply(arguments, transitions);
  if (workload == "legacy_mcts")
    return run_legacy_mcts(arguments, fixture);
  if (workload == "shared_tree")
    return run_shared_tree(arguments, fixture);
  if (workload == "parallel_scheduler")
    return run_parallel_scheduler(arguments, fixture);
  if (workload == "root_parallel")
    return run_root_parallel(arguments, fixture);
  if (workload == "board_copy_restore")
    return run_board_copy_restore(arguments, fixture);
  if (workload == "solver_state_key")
    return run_solver_state_key(arguments, fixture);
  if (workload == "solver_tt")
    return run_solver_tt(arguments, transitions, fixture);
  if (workload == "visible_solver")
    return run_visible_solver(arguments, fixture);
  if (workload == "exact_reveal")
    return run_exact_reveal(arguments, fixture);
  if (workload == "layout_probe")
    return run_layout_probe(arguments, fixture);
  throw std::invalid_argument("unknown workload: " + workload);
}

} // namespace

int main(int argc, char **argv) {
  try {
    Arguments arguments = parse_arguments(argc, argv);
    if (arguments.list_workloads) {
      for (const std::string &name : all_workloads())
        std::cout << name << '\n';
      std::cout << "layout_probe\n";
      return 0;
    }

    std::vector<std::string> workloads;
    for (const std::string &requested : arguments.workloads) {
      if (requested == "all") {
        const auto names = all_workloads();
        workloads.insert(workloads.end(), names.begin(), names.end());
      } else {
        if (!known_workload(requested))
          throw std::invalid_argument("unknown workload: " + requested);
        workloads.push_back(requested);
      }
    }
    std::vector<std::string> unique;
    for (const std::string &name : workloads) {
      if (std::find(unique.begin(), unique.end(), name) == unique.end())
        unique.push_back(name);
    }

    const Fixture fixture = make_fixture(arguments);
    const auto transitions = make_transition_corpus(fixture, arguments.seed);
    std::vector<TransitionCase> purchase_transitions;
    if (std::find(unique.begin(), unique.end(), "purchase_apply") !=
        unique.end()) {
      purchase_transitions =
          make_purchase_transition_corpus(fixture, arguments.seed);
    }
    for (const std::string &workload : unique) {
      Result result = dispatch(workload, arguments, fixture, transitions,
                               purchase_transitions);
      emit_result(workload, arguments, fixture, std::move(result));
    }
    return benchmark_sink == std::numeric_limits<uint64_t>::max() ? 2 : 0;
  } catch (const std::exception &error) {
    std::cerr << "benchmark_engine_hotpaths: " << error.what() << '\n';
    return 1;
  }
}
