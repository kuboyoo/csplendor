#ifndef CSPLENDOR_ENCODING_SCHEMA_H
#define CSPLENDOR_ENCODING_SCHEMA_H

#include "types.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace csplendor::encoding {

struct ActionSectionDescriptor {
  const char *name;
  ActionType action_type;
  uint32_t offset;
  uint32_t size;
};

struct FeatureSectionDescriptor {
  const char *name;
  size_t offset;
  size_t size;
};

struct ActionSpaceV1 {
  static constexpr uint32_t VERSION = 1;
  static constexpr int OFFSET_TAKE_DIFFERENT = 0;
  static constexpr int NUM_TAKE_DIFFERENT = 10;
  static constexpr int OFFSET_TAKE_SAME = 10;
  static constexpr int NUM_TAKE_SAME = 5;
  static constexpr int OFFSET_RESERVE_VISIBLE = 15;
  static constexpr int NUM_RESERVE_VISIBLE = 12;
  static constexpr int OFFSET_RESERVE_DECK = 27;
  static constexpr int NUM_RESERVE_DECK = 3;
  static constexpr int OFFSET_PURCHASE_VISIBLE = 30;
  static constexpr int NUM_PURCHASE_VISIBLE = 12;
  static constexpr int OFFSET_PURCHASE_RESERVED = 42;
  static constexpr int NUM_PURCHASE_RESERVED = 3;
  static constexpr int OFFSET_VISIT_NOBLE = 45;
  static constexpr int NUM_VISIT_NOBLE = 3;
  static constexpr int SIZE = 48;

  inline static constexpr std::array<ActionSectionDescriptor, 7> SECTIONS = {{
      {"take_different", TAKE_DIFFERENT, OFFSET_TAKE_DIFFERENT,
       NUM_TAKE_DIFFERENT},
      {"take_same", TAKE_SAME, OFFSET_TAKE_SAME, NUM_TAKE_SAME},
      {"reserve_visible", RESERVE_VISIBLE, OFFSET_RESERVE_VISIBLE,
       NUM_RESERVE_VISIBLE},
      {"reserve_deck", RESERVE_DECK, OFFSET_RESERVE_DECK, NUM_RESERVE_DECK},
      {"purchase_visible", PURCHASE, OFFSET_PURCHASE_VISIBLE,
       NUM_PURCHASE_VISIBLE},
      {"purchase_reserved", PURCHASE, OFFSET_PURCHASE_RESERVED,
       NUM_PURCHASE_RESERVED},
      {"visit_noble", VISIT_NOBLE, OFFSET_VISIT_NOBLE, NUM_VISIT_NOBLE},
  }};

  static constexpr const char *fingerprint() noexcept {
    return "csplendor.action.v1;size=48;layout=10,5,12,3,12,3,3";
  }
};

struct ActionSpaceV2 {
  static constexpr uint32_t VERSION = 2;
  static constexpr int TAKE_DIFF_RETURN_PATTERNS = 84;
  static constexpr int TAKE_SAME_RETURN_PATTERNS = 28;
  static constexpr int RESERVE_RETURN_PATTERNS = 7;
  static constexpr int PURCHASE_PAYMENT_PATTERNS = 252;

  static constexpr int NUM_TAKE_DIFFERENT = 10;
  static constexpr int NUM_TAKE_SAME = 5;
  static constexpr int NUM_RESERVE_VISIBLE = 12;
  static constexpr int NUM_RESERVE_DECK = 3;
  static constexpr int NUM_PURCHASE_VISIBLE = 12;
  static constexpr int NUM_PURCHASE_RESERVED = 3;
  static constexpr int NUM_VISIT_NOBLE = 3;

  static constexpr int OFFSET_TAKE_DIFFERENT = 0;
  static constexpr int OFFSET_TAKE_SAME =
      OFFSET_TAKE_DIFFERENT + NUM_TAKE_DIFFERENT * TAKE_DIFF_RETURN_PATTERNS;
  static constexpr int OFFSET_RESERVE_VISIBLE =
      OFFSET_TAKE_SAME + NUM_TAKE_SAME * TAKE_SAME_RETURN_PATTERNS;
  static constexpr int OFFSET_RESERVE_DECK =
      OFFSET_RESERVE_VISIBLE + NUM_RESERVE_VISIBLE * RESERVE_RETURN_PATTERNS;
  static constexpr int OFFSET_PURCHASE_VISIBLE =
      OFFSET_RESERVE_DECK + NUM_RESERVE_DECK * RESERVE_RETURN_PATTERNS;
  static constexpr int OFFSET_PURCHASE_RESERVED =
      OFFSET_PURCHASE_VISIBLE +
      NUM_PURCHASE_VISIBLE * PURCHASE_PAYMENT_PATTERNS;
  static constexpr int OFFSET_VISIT_NOBLE =
      OFFSET_PURCHASE_RESERVED +
      NUM_PURCHASE_RESERVED * PURCHASE_PAYMENT_PATTERNS;
  static constexpr int OFFSET_PASS = OFFSET_VISIT_NOBLE + NUM_VISIT_NOBLE;
  static constexpr int SIZE = OFFSET_PASS + 1;

  inline static constexpr std::array<ActionSectionDescriptor, 8> SECTIONS = {{
      {"take_different", TAKE_DIFFERENT, OFFSET_TAKE_DIFFERENT,
       NUM_TAKE_DIFFERENT *TAKE_DIFF_RETURN_PATTERNS},
      {"take_same", TAKE_SAME, OFFSET_TAKE_SAME,
       NUM_TAKE_SAME *TAKE_SAME_RETURN_PATTERNS},
      {"reserve_visible", RESERVE_VISIBLE, OFFSET_RESERVE_VISIBLE,
       NUM_RESERVE_VISIBLE *RESERVE_RETURN_PATTERNS},
      {"reserve_deck", RESERVE_DECK, OFFSET_RESERVE_DECK,
       NUM_RESERVE_DECK *RESERVE_RETURN_PATTERNS},
      {"purchase_visible", PURCHASE, OFFSET_PURCHASE_VISIBLE,
       NUM_PURCHASE_VISIBLE *PURCHASE_PAYMENT_PATTERNS},
      {"purchase_reserved", PURCHASE, OFFSET_PURCHASE_RESERVED,
       NUM_PURCHASE_RESERVED *PURCHASE_PAYMENT_PATTERNS},
      {"visit_noble", VISIT_NOBLE, OFFSET_VISIT_NOBLE, NUM_VISIT_NOBLE},
      {"pass", PASS, OFFSET_PASS, 1},
  }};

  static constexpr const char *fingerprint() noexcept {
    return "csplendor.action.v2;size=4869;layout=840,140,84,21,3024,756,3,1";
  }
};

struct ActionSpaceV3 {
  static constexpr uint32_t VERSION = 3;
  static constexpr int TAKE_DIFF_RETURN_PATTERNS = 84;
  static constexpr int TAKE_SAME_RETURN_PATTERNS = 28;
  static constexpr int RESERVE_RETURN_PATTERNS = 7;

  static constexpr int NUM_TAKE_DIFFERENT = 10;
  static constexpr int NUM_TAKE_SAME = 5;
  static constexpr int NUM_RESERVE_VISIBLE = 12;
  static constexpr int NUM_RESERVE_DECK = 3;
  static constexpr int NUM_CARDS = 90;
  static constexpr int NUM_NOBLES = 12;
  static constexpr int MAX_GOLD = 5;
  static constexpr int TOTAL_PURCHASE = 2035;

  static constexpr int OFFSET_TAKE_DIFFERENT = 0;
  static constexpr int OFFSET_TAKE_SAME =
      OFFSET_TAKE_DIFFERENT + NUM_TAKE_DIFFERENT * TAKE_DIFF_RETURN_PATTERNS;
  static constexpr int OFFSET_RESERVE_VISIBLE =
      OFFSET_TAKE_SAME + NUM_TAKE_SAME * TAKE_SAME_RETURN_PATTERNS;
  static constexpr int OFFSET_RESERVE_DECK =
      OFFSET_RESERVE_VISIBLE + NUM_RESERVE_VISIBLE * RESERVE_RETURN_PATTERNS;
  static constexpr int OFFSET_PURCHASE =
      OFFSET_RESERVE_DECK + NUM_RESERVE_DECK * RESERVE_RETURN_PATTERNS;
  static constexpr int OFFSET_VISIT_NOBLE = OFFSET_PURCHASE + TOTAL_PURCHASE;
  static constexpr int OFFSET_PASS = OFFSET_VISIT_NOBLE + NUM_NOBLES;
  static constexpr int SIZE = OFFSET_PASS + 1;

  inline static constexpr std::array<ActionSectionDescriptor, 7> SECTIONS = {{
      {"take_different", TAKE_DIFFERENT, OFFSET_TAKE_DIFFERENT,
       NUM_TAKE_DIFFERENT *TAKE_DIFF_RETURN_PATTERNS},
      {"take_same", TAKE_SAME, OFFSET_TAKE_SAME,
       NUM_TAKE_SAME *TAKE_SAME_RETURN_PATTERNS},
      {"reserve_visible", RESERVE_VISIBLE, OFFSET_RESERVE_VISIBLE,
       NUM_RESERVE_VISIBLE *RESERVE_RETURN_PATTERNS},
      {"reserve_deck", RESERVE_DECK, OFFSET_RESERVE_DECK,
       NUM_RESERVE_DECK *RESERVE_RETURN_PATTERNS},
      {"purchase", PURCHASE, OFFSET_PURCHASE, TOTAL_PURCHASE},
      {"visit_noble", VISIT_NOBLE, OFFSET_VISIT_NOBLE, NUM_NOBLES},
      {"pass", PASS, OFFSET_PASS, 1},
  }};

  static constexpr const char *fingerprint() noexcept {
    return "csplendor.action.v3;size=3133;layout=840,140,84,21,2035,12,1";
  }
};

struct StateFeatureV1 {
  static constexpr uint32_t VERSION = 1;
  static constexpr size_t CARD_FEATURE_SIZE = 8;
  static constexpr size_t NOBLE_FEATURE_SIZE = 6;
  static constexpr size_t PLAYER_FEATURE_SIZE = 36;
  static constexpr size_t PUBLIC_CARD_LEVEL_FEATURE_SIZE = 39;
  static constexpr size_t PUBLIC_CARD_FEATURE_SIZE =
      3 * PUBLIC_CARD_LEVEL_FEATURE_SIZE;

  static constexpr size_t OFFSET_BANK = 0;
  static constexpr size_t SIZE_BANK = 6;
  static constexpr size_t OFFSET_PLAYER_0 = OFFSET_BANK + SIZE_BANK;
  static constexpr size_t OFFSET_PLAYER_1 =
      OFFSET_PLAYER_0 + PLAYER_FEATURE_SIZE;
  static constexpr size_t OFFSET_VISIBLE_CARDS =
      OFFSET_PLAYER_1 + PLAYER_FEATURE_SIZE;
  static constexpr size_t SIZE_VISIBLE_CARDS = 12 * CARD_FEATURE_SIZE;
  static constexpr size_t OFFSET_DECK_COUNTS =
      OFFSET_VISIBLE_CARDS + SIZE_VISIBLE_CARDS;
  static constexpr size_t SIZE_DECK_COUNTS = 3;
  static constexpr size_t OFFSET_NOBLES = OFFSET_DECK_COUNTS + SIZE_DECK_COUNTS;
  static constexpr size_t SIZE_NOBLES = 3 * NOBLE_FEATURE_SIZE;
  static constexpr size_t OFFSET_CURRENT_PLAYER = OFFSET_NOBLES + SIZE_NOBLES;
  static constexpr size_t SIZE = OFFSET_CURRENT_PLAYER + 1;

  inline static constexpr std::array<size_t, 1> SHAPE = {{SIZE}};
  inline static constexpr std::array<uint8_t, 6> GEM_COLOR_IDS = {
      {DIAMOND, SAPPHIRE, EMERALD, RUBY, ONYX, GOLD}};
  inline static constexpr std::array<const char *, 6> GEM_COLOR_NAMES = {{
      "diamond",
      "sapphire",
      "emerald",
      "ruby",
      "onyx",
      "gold",
  }};
  inline static constexpr std::array<FeatureSectionDescriptor, 7> SECTIONS = {{
      {"bank", OFFSET_BANK, SIZE_BANK},
      {"player_0", OFFSET_PLAYER_0, PLAYER_FEATURE_SIZE},
      {"player_1", OFFSET_PLAYER_1, PLAYER_FEATURE_SIZE},
      {"visible_cards", OFFSET_VISIBLE_CARDS, SIZE_VISIBLE_CARDS},
      {"deck_counts", OFFSET_DECK_COUNTS, SIZE_DECK_COUNTS},
      {"nobles", OFFSET_NOBLES, SIZE_NOBLES},
      {"current_player", OFFSET_CURRENT_PLAYER, 1},
  }};

  static constexpr const char *fingerprint() noexcept {
    return "csplendor.state.v1;base=196;public=117;gems="
           "diamond,sapphire,emerald,ruby,onyx,gold";
  }
};

} // namespace csplendor::encoding

#endif // CSPLENDOR_ENCODING_SCHEMA_H
