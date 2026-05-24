# ActionEncoderV2 Action Space Specification

> **Version**: V2.1 (4869 actions)
> **Header**: `src/action_encoder_v2.h`
> **Python**: `csplendor.ActionEncoderV2`

## Overview

ActionEncoderV2 maps every distinct legal Splendor action to a unique integer ID.
The encoding is **injective**: no two different actions share the same ID.
The action mask filters invalid IDs for each game state.

| Category | Offset | Size | Formula |
|---|---|---|---|
| TAKE_DIFFERENT | 0 | 840 | 10 combos x 84 return patterns |
| TAKE_SAME | 840 | 140 | 5 colors x 28 return patterns |
| RESERVE_VISIBLE | 980 | 84 | 12 slots x 7 return patterns |
| RESERVE_DECK | 1064 | 21 | 3 levels x 7 return patterns |
| PURCHASE_VISIBLE | 1085 | 3024 | 12 slots x 252 payment patterns |
| PURCHASE_RESERVED | 4109 | 756 | 3 slots x 252 payment patterns |
| VISIT_NOBLE | 4865 | 3 | 3 nobles |
| PASS | 4868 | 1 | - |
| **Total** | | **4869** | |

## Color Indices

| Index | Color | Gem Symbol | Note |
|---|---|---|---|
| 0 | White (Diamond) | W | |
| 1 | Blue (Sapphire) | B | |
| 2 | Green (Emerald) | G | |
| 3 | Red (Ruby) | R | |
| 4 | Black (Onyx) | K | |
| 5 | Gold | $ | return only (not in payment) |

## Core Algorithm: Multiset Composition Ranking

Both return encoding and payment encoding use the same ranking algorithm
over weak compositions (multisets). This section defines the shared math.

### Multiset Coefficient H(n, k)

H(n, k) = C(n+k-1, k) counts the number of ways to distribute k identical
items into n distinct bins (weak compositions of k into n parts).

```
H[n][k] table:
       k=0  k=1  k=2  k=3  k=4  k=5
n=0:     1    0    0    0    0    0
n=1:     1    1    1    1    1    1
n=2:     1    2    3    4    5    6
n=3:     1    3    6   10   15   21
n=4:     1    4   10   20   35   56
n=5:     1    5   15   35   70  126
n=6:     1    6   21   56  126  252
```

### Graded Reverse-Lexicographic Ordering

Compositions are ordered first by sum (grade), then within each grade
in reverse-lexicographic order: higher values at earlier positions come first.

Example for n=6 colors, sum=2 (21 patterns, indices 7-27):

```
Index  Pattern
  7    [2,0,0,0,0,0]
  8    [1,1,0,0,0,0]
  9    [1,0,1,0,0,0]
 10    [1,0,0,1,0,0]
 11    [1,0,0,0,1,0]
 12    [1,0,0,0,0,1]
 13    [0,2,0,0,0,0]
 14    [0,1,1,0,0,0]
 15    [0,1,0,1,0,0]
 16    [0,1,0,0,1,0]
 17    [0,1,0,0,0,1]
 18    [0,0,2,0,0,0]
 19    [0,0,1,1,0,0]
 20    [0,0,1,0,1,0]
 21    [0,0,1,0,0,1]
 22    [0,0,0,2,0,0]
 23    [0,0,0,1,1,0]
 24    [0,0,0,1,0,1]
 25    [0,0,0,0,2,0]
 26    [0,0,0,0,1,1]
 27    [0,0,0,0,0,2]
```

### Encode Algorithm

Given a vector `v[0..n-1]` with sum `s` over `n` colors:

```
encode(v[0..n-1]):
  s = sum(v)
  if s == 0: return 0

  offset = sum(H(n, k) for k = 0 to s-1)
  rank = 0
  remaining = s

  for i = 0 to n-2:
    for w = remaining down to v[i]+1:
      rank += H(n-1-i, remaining-w)
    remaining -= v[i]

  return offset + rank
```

### Decode Algorithm

```
decode(pattern, n):
  if pattern == 0: return [0, ..., 0]

  determine s such that offset[s] <= pattern < offset[s+1]
  local_rank = pattern - offset[s]
  remaining = s
  result = [0, ..., 0]

  for i = 0 to n-2:
    for v = remaining down to 0:
      count = H(n-1-i, remaining-v)
      if local_rank < count:
        result[i] = v
        remaining -= v
        break
      local_rank -= count

  result[n-1] = remaining
  return result
```

## Return Encoding (6 colors)

Used for TAKE_DIFFERENT, TAKE_SAME, RESERVE_VISIBLE, RESERVE_DECK.

When a player's gem count exceeds 10 after taking gems, they must return
the excess. The return vector `ret[6]` specifies how many gems of each
color (including gold) to return.

**Parameters**: n=6 colors (W,B,G,R,K,$), variable max sum.

**Cumulative offsets**:

```
RETURN_OFFSET[0] = 0     (sum=0: 1 pattern)
RETURN_OFFSET[1] = 1     (sum=1: 6 patterns, H(6,1)=6)
RETURN_OFFSET[2] = 7     (sum=2: 21 patterns, H(6,2)=21)
RETURN_OFFSET[3] = 28    (sum=3: 56 patterns, H(6,3)=56)
```

**Pattern counts by max return**:

| Action Type | Max Return | Total Patterns |
|---|---|---|
| RESERVE | 1 | 7 (= 1 + 6) |
| TAKE_SAME | 2 | 28 (= 1 + 6 + 21) |
| TAKE_DIFFERENT | 3 | 84 (= 1 + 6 + 21 + 56) |

Max return is determined by gems taken minus available capacity:
- TAKE_DIFFERENT: takes 3, max excess = 3 (when player had 10)
- TAKE_SAME: takes 2, max excess = 2 (when player had 10)
- RESERVE: takes 1 gold, max excess = 1 (when player had 10)

All 6 colors may be returned, including colors that were just taken.
The action mask ensures only feasible returns are marked legal.

## Payment Encoding (5 colors)

Used for PURCHASE_VISIBLE, PURCHASE_RESERVED.

The payment vector `gold_as[5]` specifies how many gold tokens to use
as substitutes for each of the 5 gem colors. The remaining cost for each
color is paid with regular gems of that color.

**Parameters**: n=5 colors (W,B,G,R,K), max sum = 5 (2-player gold limit).

**Cumulative offsets**:

```
PAYMENT_OFFSET[0] = 0     (sum=0: 1 pattern)
PAYMENT_OFFSET[1] = 1     (sum=1: 5 patterns, H(5,1)=5)
PAYMENT_OFFSET[2] = 6     (sum=2: 15 patterns, H(5,2)=15)
PAYMENT_OFFSET[3] = 21    (sum=3: 35 patterns, H(5,3)=35)
PAYMENT_OFFSET[4] = 56    (sum=4: 70 patterns, H(5,4)=70)
PAYMENT_OFFSET[5] = 126   (sum=5: 126 patterns, H(5,5)=126)
```

**Total**: 252 patterns per card slot.

**Constraints applied by action mask** (not by the encoder):
- `gold_as[c] <= effective_cost[c]` (can't use more gold than needed)
- `sum(gold_as) <= player_gold` (can't spend more gold than owned)
- `effective_cost[c] - gold_as[c] <= player_gems[c]` (must have enough regular gems)

### Payment encoding examples

```
gold_as=[0,0,0,0,0]  sum=0  ->  pattern 0
gold_as=[1,0,0,0,0]  sum=1  ->  pattern 1
gold_as=[0,0,0,0,1]  sum=1  ->  pattern 5
gold_as=[2,0,0,0,0]  sum=2  ->  pattern 6
gold_as=[1,1,0,0,0]  sum=2  ->  pattern 7
gold_as=[1,1,1,1,1]  sum=5  ->  pattern 175
gold_as=[5,0,0,0,0]  sum=5  ->  pattern 126
gold_as=[0,0,0,0,5]  sum=5  ->  pattern 251
```

## Action ID Calculation

### TAKE_DIFFERENT (offset 0, size 840)

Take 1 gem each of 3 different colors from the bank.

```
combo_idx = index in TAKE_DIFF_COMBOS (0-9)
ret_pattern = encode_return(return_gems, n=6)  // 0-83
action_id = 0 + combo_idx * 84 + ret_pattern
```

**TAKE_DIFF_COMBOS** (C(5,3) = 10 combinations):

| Index | Colors Taken |
|---|---|
| 0 | W(0), B(1), G(2) |
| 1 | W(0), B(1), R(3) |
| 2 | W(0), B(1), K(4) |
| 3 | W(0), G(2), R(3) |
| 4 | W(0), G(2), K(4) |
| 5 | W(0), R(3), K(4) |
| 6 | B(1), G(2), R(3) |
| 7 | B(1), G(2), K(4) |
| 8 | B(1), R(3), K(4) |
| 9 | G(2), R(3), K(4) |

### TAKE_SAME (offset 840, size 140)

Take 2 gems of the same color from the bank (requires >= 4 in bank).

```
color = taken color index (0-4)
ret_pattern = encode_return(return_gems, n=6)  // 0-27
action_id = 840 + color * 28 + ret_pattern
```

### RESERVE_VISIBLE (offset 980, size 84)

Reserve a face-up card from the board and receive 1 gold from the bank.

```
slot = level * 4 + position  // 0-11
ret_pattern = encode_return(return_gems, n=6)  // 0-6
action_id = 980 + slot * 7 + ret_pattern
```

### RESERVE_DECK (offset 1064, size 21)

Reserve the top card from a deck (face-down) and receive 1 gold.

```
level = deck level (0-2)
ret_pattern = encode_return(return_gems, n=6)  // 0-6
action_id = 1064 + level * 7 + ret_pattern
```

### PURCHASE_VISIBLE (offset 1085, size 3024)

Purchase a face-up card from the board.

```
slot = level * 4 + position  // 0-11
pay_pattern = encode_payment(gold_as, n=5)  // 0-251
action_id = 1085 + slot * 252 + pay_pattern
```

### PURCHASE_RESERVED (offset 4109, size 756)

Purchase a previously reserved card from hand.

```
slot = reserved slot index (0-2)
pay_pattern = encode_payment(gold_as, n=5)  // 0-251
action_id = 4109 + slot * 252 + pay_pattern
```

### VISIT_NOBLE (offset 4865, size 3)

Choose a noble to visit (when multiple are eligible after a purchase).

```
noble_idx = index in board.nobles array (0-2)
action_id = 4865 + noble_idx
```

### PASS (offset 4868, size 1)

Pass the turn (only when no other action is legal).

```
action_id = 4868
```

## Implementation Notes

### MoveGenerator

`MoveGenerator::generate_all_fixed()` in `move_generator.h` already generates
all distinct actions with full payment and return variants when
`simple_payment_mode = false`. No changes are needed.

### MAX_MOVES

`MAX_MOVES = 2048` in `action.h`. Stress testing over 1000 random games
found a maximum of 607 legal actions per state. 2048 is sufficient.

### Action Mask

`get_action_mask()` iterates over all legal actions from MoveGenerator,
encodes each, and sets `mask[action_id] = 1`. The PASS action is set to 1
only when no other action is legal.

### MCTS

The current MCTS (`mcts.h`) uses the V1 encoder with `MAX_ACTIONS = 48`.
Switching MCTS to V2 requires updating `MAX_ACTIONS` to 4869, which
significantly increases memory per node (~83 KB vs ~0.8 KB). This is a
separate task that may require sparse node representations.

## Compatibility

| Encoder | Actions | Payment | Returns | Use Case |
|---|---|---|---|---|
| ActionEncoderCpp (V1) | 48 | heuristic | heuristic | MCTS (current) |
| ActionEncoderV2 | 4869 | full 252 | full 84/28/7 | ML training |
| ori (genbu.pt) | 406 | different scheme | different scheme | Legacy model |
