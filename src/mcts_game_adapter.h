#ifndef CSPLENDOR_MCTS_GAME_ADAPTER_H
#define CSPLENDOR_MCTS_GAME_ADAPTER_H

#include "action_encoder.h"
#include "game.h"
#include "mcts_tree_key.h"
#include "mcts_types.h"
#include "state_encoder.h"
#include <array>
#include <cstdint>
#include <utility>

namespace mcts_internal {

// Non-virtual boundary between MCTS orchestration and the rule-engine Game.
// Each helper intentionally preserves the existing clone, hash, decode/apply,
// callback, and draw-value behavior of its original call site.
struct GameAdapter final {
  GameAdapter() = delete;

  static inline Game clone_with_history(const Game &game) {
    return game.clone();
  }

  static inline Game clone_light(const Game &game) {
    return game.clone_light();
  }

  static inline Game determinize(const Game &game, uint8_t observer,
                                 uint64_t seed) {
    return game.shuffled_clone(observer, seed);
  }

  static inline Game determinize_portable(const Game &game, uint8_t observer,
                                          uint64_t seed) {
    return game.shuffled_clone_portable(observer, seed);
  }

  static inline Game clone_for_batch(const Game &game, bool determinization,
                                     uint8_t observer, uint64_t seed) {
    return determinization ? determinize(game, observer, seed)
                           : clone_light(game);
  }

  static inline Game copy_current(const Game &game) {
    return clone_with_history(game);
  }

  static inline uint64_t hash(const Game &game, uint8_t observer,
                              bool determinization) {
    uint64_t state_hash = determinization ? game.board.observable_hash(observer)
                                          : game.board.hash();
    // These Game modes change either the legal-action set or the successor
    // state, so a reusable MCTS tree must not alias otherwise identical
    // Boards across modes. Keep the legacy false/false key unchanged.
    if (game.simple_payment_mode)
      state_hash ^= 0x6a09e667f3bcc909ULL;
    if (game.blank_refill_mode)
      state_hash ^= 0xbb67ae8584caa73bULL;
    return state_hash;
  }

  static inline TreeKey tree_key(const Game &game, uint8_t observer,
                                 bool determinization) {
    uint8_t mode_bits = 0;
    if (game.simple_payment_mode)
      mode_bits |= MCTS_MODE_SIMPLE_PAYMENT;
    if (game.blank_refill_mode)
      mode_bits |= MCTS_MODE_BLANK_REFILL;
    const uint64_t position_hash = determinization
                                       ? game.board.observable_hash(observer)
                                       : game.board.hash();
    // StateEncoder::encode_canonical is observer-relative in both exact and
    // observable searches. Keeping the observer in every native TreeKey
    // prevents a leaf expanded from player 0's perspective from being reused
    // as player 1's root with incompatible features/policy.
    return {position_hash, MCTS_TREE_KEY_VERSION, observer,
            determinization ? TreeDomain::Observable : TreeDomain::Exact,
            mode_bits};
  }

  static inline bool is_terminal(const Game &game) {
    return game.is_game_over();
  }

  static inline bool requires_forced_pass(const Game &game) {
    return game.requires_forced_pass();
  }

  static inline void resolve_forced_pass(Game &game) {
    if (requires_forced_pass(game) && !game.apply_forced_pass(false))
      throw std::logic_error("failed to apply a required forced pass");
  }

  static inline int current_player(const Game &game) {
    return game.current_player();
  }

  static inline std::array<float, NUM_PLAYERS>
  terminal_value(const Game &game, float draw_value) {
    std::array<float, NUM_PLAYERS> value = {0};
    const int winner = game.winner();
    if (winner == 0) {
      value[0] = 1.0f;
      value[1] = -1.0f;
    } else if (winner == 1) {
      value[0] = -1.0f;
      value[1] = 1.0f;
    } else if (winner == -2) {
      value[0] = draw_value;
      value[1] = draw_value;
    }
    return value;
  }

  template <typename Featurizer>
  static inline auto featurize(Featurizer &featurizer, const Game &game)
      -> decltype(featurizer.featurize(game)) {
    return featurizer.featurize(game);
  }

  template <typename Encoder>
  static inline auto action_mask(Encoder &encoder, const Game &game)
      -> decltype(encoder.get_action_mask(game)) {
    return encoder.get_action_mask(game);
  }

  template <typename Encoder>
  static inline auto action_mask_owned(Encoder &encoder, Game &&game)
      -> decltype(encoder.get_action_mask_owned(std::move(game))) {
    return encoder.get_action_mask_owned(std::move(game));
  }

  template <typename Encoder>
  static inline bool decode_and_apply(Game &game, Encoder &encoder,
                                      int action_index) {
    Action decoded = encoder.decode(action_index, game);
    return game.apply_trusted(decoded, false);
  }

  static inline bool decode_and_apply_native(Game &game, int action_index) {
    Action decoded = ActionEncoderCpp::decode(action_index, game);
    if (decoded.type == ACTION_TYPE_COUNT)
      return false;
    return game.apply_trusted(decoded, false);
  }

  static inline std::array<float, FEATURE_SIZE>
  native_features(const Game &game, uint8_t observer) {
    return StateEncoder::encode_canonical(game, 0, observer);
  }

  static inline std::array<uint8_t, MAX_ACTIONS>
  native_action_mask(const Game &game) {
    return ActionEncoderCpp::get_action_mask(game);
  }
};

} // namespace mcts_internal

#endif // CSPLENDOR_MCTS_GAME_ADAPTER_H
