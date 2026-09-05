#ifndef CSPLENDOR_SOLVER_REVEAL_ORDER_H
#define CSPLENDOR_SOLVER_REVEAL_ORDER_H

#include "board.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <vector>

namespace csplendor::solver_internal {

#ifdef CSPLENDOR_CACHE_REVEAL_SCORES
inline constexpr bool cache_reveal_scores_enabled = true;
#else
inline constexpr bool cache_reveal_scores_enabled = false;
#endif

// score must be a pure, action-local function. In particular, reserve return
// colours change after-gems: no cache may be shared with another action.
template <bool Cache = cache_reveal_scores_enabled, typename Score>
void sort_reveal_cards_by_score(std::vector<int> &cards, Score score) {
  if (cards.size() <= 1)
    return;
  if constexpr (!Cache) {
    std::sort(cards.begin(), cards.end(), [&](int left, int right) {
      const int left_score = score(left);
      const int right_score = score(right);
      return left_score != right_score ? left_score > right_score : left < right;
    });
  } else {
#ifdef CSPLENDOR_VERIFY_REVEAL_SCORE_ORDER
    auto reference = cards;
    sort_reveal_cards_by_score<false>(reference, score);
#endif
    struct ScoredCard {
      int score;
      int card_id;
    };
    // Every production candidate list comes from one FixedStack<40> deck.
    // Retain a checked fallback for larger internal/editor test inputs.
    std::array<ScoredCard, Board::MAX_DECK_SIZE> local;
    std::vector<ScoredCard> overflow;
    ScoredCard *scored = local.data();
    if (cards.size() > local.size()) {
      overflow.resize(cards.size());
      scored = overflow.data();
    }
    for (size_t i = 0; i < cards.size(); ++i) {
      scored[i] = {score(cards[i]), cards[i]};
#ifdef CSPLENDOR_VERIFY_REVEAL_SCORE_ORDER
      if (scored[i].score != score(cards[i]))
        std::abort();
#endif
    }
    std::sort(scored, scored + cards.size(),
              [](const ScoredCard &left, const ScoredCard &right) {
                return left.score != right.score ? left.score > right.score
                                                  : left.card_id < right.card_id;
              });
    for (size_t i = 0; i < cards.size(); ++i)
      cards[i] = scored[i].card_id;
#ifdef CSPLENDOR_VERIFY_REVEAL_SCORE_ORDER
    if (cards != reference)
      std::abort();
#endif
  }
}

} // namespace csplendor::solver_internal
#endif
