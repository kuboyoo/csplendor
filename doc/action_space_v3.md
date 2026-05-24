# ActionEncoderV3 Action Space Specification

> **Version**: V3.1 (3133 actions)
> **Header**: `src/action_encoder_v3.h`
> **Python**: `csplendor.ActionEncoderV3`

## Overview

ActionEncoderV3 maps every distinct legal Splendor action to a unique integer ID.
Key differences from V2:
- **PURCHASE actions are indexed by card ID** (0-89) instead of slot position
- **VISIT_NOBLE actions are indexed by noble ID** (0-11) instead of slot position

This enables neural networks to learn card/noble-specific strategies without
redundant position-dependent representations.

| Category | Offset | Size | Formula |
|---|---|---|---|
| TAKE_DIFFERENT | 0 | 840 | 10 combos x 84 return patterns |
| TAKE_SAME | 840 | 140 | 5 colors x 28 return patterns |
| RESERVE_VISIBLE | 980 | 84 | 12 slots x 7 return patterns |
| RESERVE_DECK | 1064 | 21 | 3 levels x 7 return patterns |
| PURCHASE | 1085 | 2035 | 90 cards x card-specific patterns |
| VISIT_NOBLE | 3120 | 12 | 12 noble IDs |
| PASS | 3132 | 1 | - |
| **Total** | | **3133** | |

### Comparison with V2

| Metric | V2 | V3 | Change |
|---|---|---|---|
| PURCHASE actions | 3780 | 2035 | **-46%** |
| VISIT_NOBLE actions | 3 | 12 | +9 (ID-based) |
| Total actions | 4869 | 3133 | **-36%** |

## Design Rationale

### Problem with Slot-Based Indexing (V2)

In V2, the same card appearing in different slots maps to different action IDs:
- Card #5 in slot 0 -> action ID 1085 + 0x252 + pay_pattern
- Card #5 in slot 1 -> action ID 1085 + 1x252 + pay_pattern

This forces the NN to learn the same card value 12 times (once per slot).
Similarly, the same noble in different board positions maps to different IDs.

### ID-Based Indexing (V3)

In V3, each card and noble maps to a unique range of action IDs regardless of position:
- Card #5 -> action ID 1085 + CARD_OFFSET[5] + pay_pattern
- Noble #7 -> action ID 3120 + 7

Benefits:
1. **Semantic consistency**: Same card/noble = same action IDs
2. **Reduced action space**: 3780 -> 2035 PURCHASE actions
3. **Better generalization**: NN learns card/noble value directly

## Payment Pattern Encoding

### Card-Specific Pattern Constraints

For each card with cost `[c0, c1, c2, c3, c4]`, the valid `gold_as[5]` patterns satisfy:
- `gold_as[i] <= cost[i]` (can't substitute more gold than needed for each color)
- `sum(gold_as) <= 5` (max gold tokens in 2-player game)

This reduces patterns significantly for low-cost cards.

### Pattern Counts by Card

```
Level 1 cards: 4 - 24 patterns per card (avg 12.2)
Level 2 cards: 6 - 38 patterns per card (avg 21.0)
Level 3 cards: 6 - 111 patterns per card (avg 45.8)
```

### Card Offset Table

Each card has a pre-computed offset into the PURCHASE action range:

```cpp
// CARD_PAYMENT_OFFSET[card_id] = cumulative offset into PURCHASE range
// CARD_PATTERN_COUNT[card_id] = number of valid patterns for this card
constexpr uint16_t CARD_PAYMENT_OFFSET[90] = {
    0,    4,   10,   19,   37,   53,   69,   93,   // Cards 0-7
   98,  102,  108,  117,  135,  151,  167,  191,   // Cards 8-15
  196,  200,  206,  215,  233,  249,  265,  289,   // Cards 16-23
  294,  298,  304,  313,  331,  347,  363,  387,   // Cards 24-31
  392,  396,  402,  411,  429,  445,  461,  485,   // Cards 32-39
  490,  522,  560,  566,  584,  610,  616,  648,   // Cards 40-47
  686,  692,  710,  736,  742,  774,  812,  818,   // Cards 48-55
  836,  862,  868,  900,  938,  944,  962,  988,   // Cards 56-63
  994, 1026, 1064, 1070, 1088, 1114, 1120, 1231,   // Cards 64-71
 1237, 1285, 1303, 1414, 1420, 1468, 1486, 1597,   // Cards 72-79
 1603, 1651, 1669, 1780, 1786, 1834, 1852, 1963,   // Cards 80-87
 1969, 2017                                        // Cards 88-89
};

constexpr uint8_t CARD_PATTERN_COUNT[90] = {
    4,  6,  9, 18, 16, 16, 24,  5,   // Cards 0-7
    4,  6,  9, 18, 16, 16, 24,  5,   // Cards 8-15
    4,  6,  9, 18, 16, 16, 24,  5,   // Cards 16-23
    4,  6,  9, 18, 16, 16, 24,  5,   // Cards 24-31
    4,  6,  9, 18, 16, 16, 24,  5,   // Cards 32-39
   32, 38,  6, 18, 26,  6, 32, 38,   // Cards 40-47
    6, 18, 26,  6, 32, 38,  6, 18,   // Cards 48-55
   26,  6, 32, 38,  6, 18, 26,  6,   // Cards 56-63
   32, 38,  6, 18, 26,  6,111,  6,   // Cards 64-71
   48, 18,111,  6, 48, 18,111,  6,   // Cards 72-79
   48, 18,111,  6, 48, 18,111,  6,   // Cards 80-87
   48, 18                            // Cards 88-89
};
```

### Payment Pattern Index Encoding

Within each card's pattern range, patterns are ordered by total gold usage (graded lexicographic):

```
Pattern 0: gold_as = [0,0,0,0,0] (sum=0, no gold used)
Pattern 1-N1: sum(gold_as) = 1 patterns
Pattern N1+1-N2: sum(gold_as) = 2 patterns
...
```

The encoding uses constrained multiset composition ranking:
for a card with cost upper[5], count valid compositions where each
part[i] <= upper[i] and rank them in graded lexicographic order.

## Action ID Calculation

### TAKE_DIFFERENT (offset 0, size 840)

Same as V2. Take 1 gem each of 3 different colors from the bank.

```
combo_idx = index in TAKE_DIFF_COMBOS (0-9)
ret_pattern = encode_return(return_gems, n=6)  // 0-83
action_id = 0 + combo_idx * 84 + ret_pattern
```

### TAKE_SAME (offset 840, size 140)

Same as V2. Take 2 gems of the same color from the bank.

```
color = taken color index (0-4)
ret_pattern = encode_return(return_gems, n=6)  // 0-27
action_id = 840 + color * 28 + ret_pattern
```

### RESERVE_VISIBLE (offset 980, size 84)

Same as V2. Reserve a face-up card from the board.

```
slot = level * 4 + position  // 0-11
ret_pattern = encode_return(return_gems, n=6)  // 0-6
action_id = 980 + slot * 7 + ret_pattern
```

### RESERVE_DECK (offset 1064, size 21)

Same as V2. Reserve the top card from a deck.

```
level = deck level (0-2)
ret_pattern = encode_return(return_gems, n=6)  // 0-6
action_id = 1064 + level * 7 + ret_pattern
```

### PURCHASE (offset 1085, size 2035)

Purchase any card (from board or reserved hand). Indexed by card ID.

```
Encode:
  card_id = ID of card being purchased (0-89)
  pay_pattern = encode_payment_for_card(gold_as, card_id)
  action_id = 1085 + CARD_PAYMENT_OFFSET[card_id] + pay_pattern

Decode:
  local_idx = action_id - 1085
  card_id = binary_search(CARD_PAYMENT_OFFSET, local_idx)
  pay_pattern = local_idx - CARD_PAYMENT_OFFSET[card_id]
  gold_as = decode_payment_for_card(pay_pattern, card_id)
  from_reserved = is_in_reserved(board, card_id)
```

### VISIT_NOBLE (offset 3120, size 12)

Choose a noble to visit. Indexed by noble ID (0-11) directly.

```
Encode:
  action_id = 3120 + noble_id

Decode:
  noble_id = action_id - 3120
```

Noble IDs correspond to the global NOBLES[12] array:

| Noble ID | Points | Requirement [W,B,G,R,K] |
|---|---|---|
| 0 | 3 | [0,0,4,4,0] |
| 1 | 3 | [0,0,0,4,4] |
| 2 | 3 | [0,4,4,0,0] |
| 3 | 3 | [4,0,0,0,4] |
| 4 | 3 | [4,4,0,0,0] |
| 5 | 3 | [4,0,0,4,0] |
| 6 | 3 | [3,0,0,3,3] |
| 7 | 3 | [3,3,3,0,0] |
| 8 | 3 | [0,0,3,3,3] |
| 9 | 3 | [0,3,3,3,0] |
| 10 | 3 | [3,3,0,0,3] |
| 11 | 3 | [0,3,3,0,3] |

### PASS (offset 3132, size 1)

Pass the turn (only when no other action is legal).

```
action_id = 3132
```

## Migration Notes

### From V2 to V3

1. Action space size: 4869 -> 3133
2. PURCHASE_VISIBLE and PURCHASE_RESERVED are **merged** into single PURCHASE category
3. Card position (slot) is **not** encoded in PURCHASE action ID
4. `from_reserved` must be inferred from board state during decode
5. VISIT_NOBLE uses noble ID (0-11) instead of board slot index (0-2)

### Model Compatibility

V3 is **not compatible** with V2-trained models. New models must be trained from scratch.

## Appendix: Full Card Pattern Counts

| Card ID | Level | Cost | Patterns |
|---|---|---|---|
| 0-7 | 1 | varies | 4,6,9,18,16,16,24,5 |
| 8-15 | 1 | varies | 4,6,9,18,16,16,24,5 |
| 16-23 | 1 | varies | 4,6,9,18,16,16,24,5 |
| 24-31 | 1 | varies | 4,6,9,18,16,16,24,5 |
| 32-39 | 1 | varies | 4,6,9,18,16,16,24,5 |
| 40-45 | 2 | varies | 32,38,6,18,26,6 |
| 46-51 | 2 | varies | 32,38,6,18,26,6 |
| 52-57 | 2 | varies | 32,38,6,18,26,6 |
| 58-63 | 2 | varies | 32,38,6,18,26,6 |
| 64-69 | 2 | varies | 32,38,6,18,26,6 |
| 70-73 | 3 | varies | 111,6,48,18 |
| 74-77 | 3 | varies | 111,6,48,18 |
| 78-81 | 3 | varies | 111,6,48,18 |
| 82-85 | 3 | varies | 111,6,48,18 |
| 86-89 | 3 | varies | 111,6,48,18 |

**Total PURCHASE patterns: 2035**
