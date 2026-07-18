#ifndef CSPLENDOR_MCTS_RNG_H
#define CSPLENDOR_MCTS_RNG_H

#include "mcts_tree_key.h"
#include "portable_rng.h"

#include <cstdint>
#include <random>

static constexpr uint32_t MCTS_RNG_VERSION = 1;

inline uint64_t resolve_mcts_entropy_seed() {
  // Called only by a coordinator/session constructor. Worker threads derive
  // stateless domain seeds and never consult random_device themselves.
  std::random_device entropy;
  uint64_t seed = 0xcbf29ce484222325ULL;
  for (int draw = 0; draw < 4; ++draw) {
    seed ^= static_cast<uint64_t>(entropy());
    seed *= 0x100000001b3ULL;
    seed ^= seed >> 32;
  }
  return seed;
}

enum class SearchRandomDomain : uint64_t {
  RootDeterminization = 0x524f4f545f444554ULL,
  ExtraWorld = 0x45585452415f5752ULL,
  RootDirichlet = 0x524f4f545f444952ULL,
  PuctTieBreak = 0x505543545f544945ULL,
  FinalTemperature = 0x46494e414c5f544dULL,
  StressScheduler = 0x5354524553535f53ULL,
};

inline uint64_t mcts_rng_mix(uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

inline void mcts_rng_absorb(uint64_t &state, uint64_t field) noexcept {
  state = mcts_rng_mix(state ^ mcts_rng_mix(field));
}

inline uint64_t
derive_search_seed(uint64_t master_seed, const TreeKey &root_key,
                   uint64_t search_nonce, SearchRandomDomain domain,
                   uint64_t simulation_id, uint32_t world_id = 0,
                   uint32_t sub_index = 0) noexcept {
  uint64_t state = mcts_rng_mix(master_seed ^ 0x4353504c454e444fULL);
  mcts_rng_absorb(state, root_key.position_hash);
  mcts_rng_absorb(state, root_key.key_version);
  mcts_rng_absorb(state, root_key.observer);
  mcts_rng_absorb(state, static_cast<uint8_t>(root_key.domain));
  mcts_rng_absorb(state, root_key.mode_bits);
  mcts_rng_absorb(state, search_nonce);
  mcts_rng_absorb(state, static_cast<uint64_t>(domain));
  mcts_rng_absorb(state, simulation_id);
  mcts_rng_absorb(state, world_id);
  mcts_rng_absorb(state, sub_index);
  mcts_rng_absorb(state, MCTS_RNG_VERSION);
  return state;
}

struct SearchRandomContext {
  uint64_t resolved_master_seed = 0;
  TreeKey root_key{};
  uint64_t search_nonce = 0;
  uint64_t simulation_id_base = 0;
  uint32_t rng_version = MCTS_RNG_VERSION;

  uint64_t seed_for(SearchRandomDomain domain, uint64_t simulation_id,
                    uint32_t world_id = 0,
                    uint32_t sub_index = 0) const noexcept {
    return derive_search_seed(resolved_master_seed, root_key, search_nonce,
                              domain, simulation_id, world_id, sub_index);
  }
};

#endif // CSPLENDOR_MCTS_RNG_H
