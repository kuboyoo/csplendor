#ifndef CSPLENDOR_CARD_DATA_H
#define CSPLENDOR_CARD_DATA_H

#include "types.h"
#include <stdexcept>

static constexpr int CARD_COUNT = 90;

// All 90 cards in Splendor
// Cost order: [White (D), Blue (S), Green (E), Red (R), Black (O)]
// Bonus: DIAMOND=0, SAPPHIRE=1, EMERALD=2, RUBY=3, ONYX=4
constexpr Card CARDS[CARD_COUNT] = {
    // Level 1 - Sapphire bonus (8 cards, id 0-7)
    {0, 1, 0, SAPPHIRE, {0, 0, 0, 0, 3}, 844424930131968ULL},
    {1, 1, 0, SAPPHIRE, {1, 0, 0, 0, 2}, 562949953421313ULL},
    {2, 1, 0, SAPPHIRE, {0, 0, 2, 0, 2}, 562949986975744ULL},
    {3, 1, 0, SAPPHIRE, {1, 0, 2, 2, 0}, 137472507905ULL},
    {4, 1, 0, SAPPHIRE, {0, 1, 3, 1, 0}, 68769812480ULL},
    {5, 1, 0, SAPPHIRE, {1, 0, 1, 1, 1}, 281543712964609ULL},
    {6, 1, 0, SAPPHIRE, {1, 0, 1, 2, 1}, 281612432441345ULL},
    {7, 1, 1, SAPPHIRE, {0, 0, 0, 4, 0}, 274877906944ULL},

    // Level 1 - Ruby bonus (8 cards, id 8-15)
    {8, 1, 0, RUBY, {3, 0, 0, 0, 0}, 3ULL},
    {9, 1, 0, RUBY, {0, 2, 1, 0, 0}, 16785408ULL},
    {10, 1, 0, RUBY, {2, 0, 0, 2, 0}, 137438953474ULL},
    {11, 1, 0, RUBY, {2, 0, 1, 0, 2}, 562949970198530ULL},
    {12, 1, 0, RUBY, {1, 0, 0, 1, 3}, 844493649608705ULL},
    {13, 1, 0, RUBY, {1, 1, 1, 0, 1}, 281474993491969ULL},
    {14, 1, 0, RUBY, {2, 1, 1, 0, 1}, 281474993491970ULL},
    {15, 1, 1, RUBY, {4, 0, 0, 0, 0}, 4ULL},

    // Level 1 - Onyx bonus (8 cards, id 16-23)
    {16, 1, 0, ONYX, {0, 0, 3, 0, 0}, 50331648ULL},
    {17, 1, 0, ONYX, {0, 0, 2, 1, 0}, 68753031168ULL},
    {18, 1, 0, ONYX, {2, 0, 2, 0, 0}, 33554434ULL},
    {19, 1, 0, ONYX, {2, 2, 0, 1, 0}, 68719484930ULL},
    {20, 1, 0, ONYX, {0, 0, 1, 3, 1}, 281681151918080ULL},
    {21, 1, 0, ONYX, {1, 1, 1, 1, 0}, 68736258049ULL},
    {22, 1, 0, ONYX, {1, 2, 1, 1, 0}, 68736262145ULL},
    {23, 1, 1, ONYX, {0, 4, 0, 0, 0}, 16384ULL},

    // Level 1 - Diamond bonus (8 cards, id 24-31)
    {24, 1, 0, DIAMOND, {0, 3, 0, 0, 0}, 12288ULL},
    {25, 1, 0, DIAMOND, {0, 0, 0, 2, 1}, 281612415664128ULL},
    {26, 1, 0, DIAMOND, {0, 2, 0, 0, 2}, 562949953429504ULL},
    {27, 1, 0, DIAMOND, {0, 2, 2, 0, 1}, 281475010273280ULL},
    {28, 1, 0, DIAMOND, {3, 1, 0, 0, 1}, 281474976714755ULL},
    {29, 1, 0, DIAMOND, {0, 1, 1, 1, 1}, 281543712968704ULL},
    {30, 1, 0, DIAMOND, {0, 1, 2, 1, 1}, 281543729745920ULL},
    {31, 1, 1, DIAMOND, {0, 0, 4, 0, 0}, 67108864ULL},

    // Level 1 - Emerald bonus (8 cards, id 32-39)
    {32, 1, 0, EMERALD, {0, 0, 0, 3, 0}, 206158430208ULL},
    {33, 1, 0, EMERALD, {2, 1, 0, 0, 0}, 4098ULL},
    {34, 1, 0, EMERALD, {0, 2, 0, 2, 0}, 137438961664ULL},
    {35, 1, 0, EMERALD, {0, 1, 0, 2, 2}, 563087392378880ULL},
    {36, 1, 0, EMERALD, {1, 3, 1, 0, 0}, 16789505ULL},
    {37, 1, 0, EMERALD, {1, 1, 0, 1, 1}, 281543696191489ULL},
    {38, 1, 0, EMERALD, {1, 1, 0, 1, 2}, 563018672902145ULL},
    {39, 1, 1, EMERALD, {0, 0, 0, 0, 4}, 1125899906842624ULL},

    // Level 2 - Sapphire bonus (6 cards, id 40-45)
    {40, 2, 1, SAPPHIRE, {0, 2, 2, 3, 0}, 206191992832ULL},
    {41, 2, 1, SAPPHIRE, {0, 2, 3, 0, 3}, 844424980471808ULL},
    {42, 2, 2, SAPPHIRE, {0, 5, 0, 0, 0}, 20480ULL},
    {43, 2, 2, SAPPHIRE, {5, 3, 0, 0, 0}, 12293ULL},
    {44, 2, 2, SAPPHIRE, {2, 0, 0, 1, 4}, 1125968626319362ULL},
    {45, 2, 3, SAPPHIRE, {0, 6, 0, 0, 0}, 24576ULL},

    // Level 2 - Ruby bonus (6 cards, id 46-51)
    {46, 2, 1, RUBY, {2, 0, 0, 2, 3}, 844562369085442ULL},
    {47, 2, 1, RUBY, {0, 3, 0, 2, 3}, 844562369097728ULL},
    {48, 2, 2, RUBY, {0, 0, 0, 0, 5}, 1407374883553280ULL},
    {49, 2, 2, RUBY, {3, 0, 0, 0, 5}, 1407374883553283ULL},
    {50, 2, 2, RUBY, {1, 4, 2, 0, 0}, 33570817ULL},
    {51, 2, 3, RUBY, {0, 0, 0, 6, 0}, 412316860416ULL},

    // Level 2 - Onyx bonus (6 cards, id 52-57)
    {52, 2, 1, ONYX, {3, 2, 2, 0, 0}, 33562627ULL},
    {53, 2, 1, ONYX, {3, 0, 3, 0, 2}, 562950003752963ULL},
    {54, 2, 2, ONYX, {5, 0, 0, 0, 0}, 5ULL},
    {55, 2, 2, ONYX, {0, 0, 5, 3, 0}, 206242316288ULL},
    {56, 2, 2, ONYX, {0, 1, 4, 2, 0}, 137506066432ULL},
    {57, 2, 3, ONYX, {0, 0, 0, 0, 6}, 1688849860263936ULL},

    // Level 2 - Diamond bonus (6 cards, id 58-63)
    {58, 2, 1, DIAMOND, {0, 0, 3, 2, 2}, 563087442706432ULL},
    {59, 2, 1, DIAMOND, {2, 3, 0, 3, 0}, 206158442498ULL},
    {60, 2, 2, DIAMOND, {0, 0, 0, 5, 0}, 343597383680ULL},
    {61, 2, 2, DIAMOND, {0, 0, 0, 5, 3}, 844768527515648ULL},
    {62, 2, 2, DIAMOND, {0, 0, 1, 4, 2}, 563224848105472ULL},
    {63, 2, 3, DIAMOND, {6, 0, 0, 0, 0}, 6ULL},

    // Level 2 - Emerald bonus (6 cards, id 64-69)
    {64, 2, 1, EMERALD, {2, 3, 0, 0, 2}, 562949953433602ULL},
    {65, 2, 1, EMERALD, {3, 0, 2, 3, 0}, 206191984643ULL},
    {66, 2, 2, EMERALD, {0, 0, 5, 0, 0}, 83886080ULL},
    {67, 2, 2, EMERALD, {0, 5, 3, 0, 0}, 50352128ULL},
    {68, 2, 2, EMERALD, {4, 2, 0, 0, 1}, 281474976718852ULL},
    {69, 2, 3, EMERALD, {0, 0, 6, 0, 0}, 100663296ULL},

    // Level 3 - Sapphire bonus (4 cards, id 70-73)
    {70, 3, 3, SAPPHIRE, {3, 0, 3, 3, 5}, 1407581092315139ULL},
    {71, 3, 4, SAPPHIRE, {7, 0, 0, 0, 0}, 7ULL},
    {72, 3, 4, SAPPHIRE, {6, 3, 0, 0, 3}, 844424930144262ULL},
    {73, 3, 5, SAPPHIRE, {7, 3, 0, 0, 0}, 12295ULL},

    // Level 3 - Ruby bonus (4 cards, id 74-77)
    {74, 3, 3, RUBY, {3, 5, 3, 0, 3}, 844424980484099ULL},
    {75, 3, 4, RUBY, {0, 0, 7, 0, 0}, 117440512ULL},
    {76, 3, 4, RUBY, {0, 3, 6, 3, 0}, 206259105792ULL},
    {77, 3, 5, RUBY, {0, 0, 7, 3, 0}, 206275870720ULL},

    // Level 3 - Onyx bonus (4 cards, id 78-81)
    {78, 3, 3, ONYX, {3, 3, 5, 3, 0}, 206242328579ULL},
    {79, 3, 4, ONYX, {0, 0, 0, 7, 0}, 481036337152ULL},
    {80, 3, 4, ONYX, {0, 0, 3, 6, 3}, 844837297324032ULL},
    {81, 3, 5, ONYX, {0, 0, 0, 7, 3}, 844905966469120ULL},

    // Level 3 - Diamond bonus (4 cards, id 82-85)
    {82, 3, 3, DIAMOND, {0, 3, 3, 5, 3}, 844768577859584ULL},
    {83, 3, 4, DIAMOND, {0, 0, 0, 0, 7}, 1970324836974592ULL},
    {84, 3, 4, DIAMOND, {3, 0, 0, 3, 6}, 1689056018694147ULL},
    {85, 3, 5, DIAMOND, {3, 0, 0, 0, 7}, 1970324836974595ULL},

    // Level 3 - Emerald bonus (4 cards, id 86-89)
    {86, 3, 3, EMERALD, {5, 3, 0, 3, 3}, 844631088574469ULL},
    {87, 3, 4, EMERALD, {0, 7, 0, 0, 0}, 28672ULL},
    {88, 3, 4, EMERALD, {3, 6, 3, 0, 0}, 50356227ULL},
    {89, 3, 5, EMERALD, {0, 7, 3, 0, 0}, 50360320ULL}};

inline bool is_valid_card_id(int id) { return id >= 0 && id < CARD_COUNT; }

inline const Card &get_card(int id) {
  if (!is_valid_card_id(id))
    throw std::out_of_range("card id out of range");
  return CARDS[id];
}

#endif // CSPLENDOR_CARD_DATA_H
