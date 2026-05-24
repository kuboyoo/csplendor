#!/usr/bin/env python3
"""
Analyze the number of valid payment patterns per card based on actual card costs.

For each card, enumerate all possible gold_as[5] payment patterns where:
- gold_as[c] <= cost[c] (can't use more gold than the cost for that color)
- sum(gold_as) <= 5 (max gold in 2-player game)

This gives the theoretical maximum number of payment patterns that could ever
be valid for each specific card, regardless of player state.
"""

from itertools import product
from dataclasses import dataclass
from typing import List, Tuple

# Card data extracted from card_data.h
# Format: (id, level, points, bonus, [W, B, G, R, K])
CARDS_RAW = [
    # Level 1 - Sapphire bonus (8 cards, id 0-7)
    (0, 1, 0, "SAPPHIRE", [0, 0, 0, 0, 3]),
    (1, 1, 0, "SAPPHIRE", [1, 0, 0, 0, 2]),
    (2, 1, 0, "SAPPHIRE", [0, 0, 2, 0, 2]),
    (3, 1, 0, "SAPPHIRE", [1, 0, 2, 2, 0]),
    (4, 1, 0, "SAPPHIRE", [0, 1, 3, 1, 0]),
    (5, 1, 0, "SAPPHIRE", [1, 0, 1, 1, 1]),
    (6, 1, 0, "SAPPHIRE", [1, 0, 1, 2, 1]),
    (7, 1, 1, "SAPPHIRE", [0, 0, 0, 4, 0]),

    # Level 1 - Ruby bonus (8 cards, id 8-15)
    (8, 1, 0, "RUBY", [3, 0, 0, 0, 0]),
    (9, 1, 0, "RUBY", [0, 2, 1, 0, 0]),
    (10, 1, 0, "RUBY", [2, 0, 0, 2, 0]),
    (11, 1, 0, "RUBY", [2, 0, 1, 0, 2]),
    (12, 1, 0, "RUBY", [1, 0, 0, 1, 3]),
    (13, 1, 0, "RUBY", [1, 1, 1, 0, 1]),
    (14, 1, 0, "RUBY", [2, 1, 1, 0, 1]),
    (15, 1, 1, "RUBY", [4, 0, 0, 0, 0]),

    # Level 1 - Onyx bonus (8 cards, id 16-23)
    (16, 1, 0, "ONYX", [0, 0, 3, 0, 0]),
    (17, 1, 0, "ONYX", [0, 0, 2, 1, 0]),
    (18, 1, 0, "ONYX", [2, 0, 2, 0, 0]),
    (19, 1, 0, "ONYX", [2, 2, 0, 1, 0]),
    (20, 1, 0, "ONYX", [0, 0, 1, 3, 1]),
    (21, 1, 0, "ONYX", [1, 1, 1, 1, 0]),
    (22, 1, 0, "ONYX", [1, 2, 1, 1, 0]),
    (23, 1, 1, "ONYX", [0, 4, 0, 0, 0]),

    # Level 1 - Diamond bonus (8 cards, id 24-31)
    (24, 1, 0, "DIAMOND", [0, 3, 0, 0, 0]),
    (25, 1, 0, "DIAMOND", [0, 0, 0, 2, 1]),
    (26, 1, 0, "DIAMOND", [0, 2, 0, 0, 2]),
    (27, 1, 0, "DIAMOND", [0, 2, 2, 0, 1]),
    (28, 1, 0, "DIAMOND", [3, 1, 0, 0, 1]),
    (29, 1, 0, "DIAMOND", [0, 1, 1, 1, 1]),
    (30, 1, 0, "DIAMOND", [0, 1, 2, 1, 1]),
    (31, 1, 1, "DIAMOND", [0, 0, 4, 0, 0]),

    # Level 1 - Emerald bonus (8 cards, id 32-39)
    (32, 1, 0, "EMERALD", [0, 0, 0, 3, 0]),
    (33, 1, 0, "EMERALD", [2, 1, 0, 0, 0]),
    (34, 1, 0, "EMERALD", [0, 2, 0, 2, 0]),
    (35, 1, 0, "EMERALD", [0, 1, 0, 2, 2]),
    (36, 1, 0, "EMERALD", [1, 3, 1, 0, 0]),
    (37, 1, 0, "EMERALD", [1, 1, 0, 1, 1]),
    (38, 1, 0, "EMERALD", [1, 1, 0, 1, 2]),
    (39, 1, 1, "EMERALD", [0, 0, 0, 0, 4]),

    # Level 2 - Sapphire bonus (6 cards, id 40-45)
    (40, 2, 1, "SAPPHIRE", [0, 2, 2, 3, 0]),
    (41, 2, 1, "SAPPHIRE", [0, 2, 3, 0, 3]),
    (42, 2, 2, "SAPPHIRE", [0, 5, 0, 0, 0]),
    (43, 2, 2, "SAPPHIRE", [5, 3, 0, 0, 0]),
    (44, 2, 2, "SAPPHIRE", [2, 0, 0, 1, 4]),
    (45, 2, 3, "SAPPHIRE", [0, 6, 0, 0, 0]),

    # Level 2 - Ruby bonus (6 cards, id 46-51)
    (46, 2, 1, "RUBY", [2, 0, 0, 2, 3]),
    (47, 2, 1, "RUBY", [0, 3, 0, 2, 3]),
    (48, 2, 2, "RUBY", [0, 0, 0, 0, 5]),
    (49, 2, 2, "RUBY", [3, 0, 0, 0, 5]),
    (50, 2, 2, "RUBY", [1, 4, 2, 0, 0]),
    (51, 2, 3, "RUBY", [0, 0, 0, 6, 0]),

    # Level 2 - Onyx bonus (6 cards, id 52-57)
    (52, 2, 1, "ONYX", [3, 2, 2, 0, 0]),
    (53, 2, 1, "ONYX", [3, 0, 3, 0, 2]),
    (54, 2, 2, "ONYX", [5, 0, 0, 0, 0]),
    (55, 2, 2, "ONYX", [0, 0, 5, 3, 0]),
    (56, 2, 2, "ONYX", [0, 1, 4, 2, 0]),
    (57, 2, 3, "ONYX", [0, 0, 0, 0, 6]),

    # Level 2 - Diamond bonus (6 cards, id 58-63)
    (58, 2, 1, "DIAMOND", [0, 0, 3, 2, 2]),
    (59, 2, 1, "DIAMOND", [2, 3, 0, 3, 0]),
    (60, 2, 2, "DIAMOND", [0, 0, 0, 5, 0]),
    (61, 2, 2, "DIAMOND", [0, 0, 0, 5, 3]),
    (62, 2, 2, "DIAMOND", [0, 0, 1, 4, 2]),
    (63, 2, 3, "DIAMOND", [6, 0, 0, 0, 0]),

    # Level 2 - Emerald bonus (6 cards, id 64-69)
    (64, 2, 1, "EMERALD", [2, 3, 0, 0, 2]),
    (65, 2, 1, "EMERALD", [3, 0, 2, 3, 0]),
    (66, 2, 2, "EMERALD", [0, 0, 5, 0, 0]),
    (67, 2, 2, "EMERALD", [0, 5, 3, 0, 0]),
    (68, 2, 2, "EMERALD", [4, 2, 0, 0, 1]),
    (69, 2, 3, "EMERALD", [0, 0, 6, 0, 0]),

    # Level 3 - Sapphire bonus (4 cards, id 70-73)
    (70, 3, 3, "SAPPHIRE", [3, 0, 3, 3, 5]),
    (71, 3, 4, "SAPPHIRE", [7, 0, 0, 0, 0]),
    (72, 3, 4, "SAPPHIRE", [6, 3, 0, 0, 3]),
    (73, 3, 5, "SAPPHIRE", [7, 3, 0, 0, 0]),

    # Level 3 - Ruby bonus (4 cards, id 74-77)
    (74, 3, 3, "RUBY", [3, 5, 3, 0, 3]),
    (75, 3, 4, "RUBY", [0, 0, 7, 0, 0]),
    (76, 3, 4, "RUBY", [0, 3, 6, 3, 0]),
    (77, 3, 5, "RUBY", [0, 0, 7, 3, 0]),

    # Level 3 - Onyx bonus (4 cards, id 78-81)
    (78, 3, 3, "ONYX", [3, 3, 5, 3, 0]),
    (79, 3, 4, "ONYX", [0, 0, 0, 7, 0]),
    (80, 3, 4, "ONYX", [0, 0, 3, 6, 3]),
    (81, 3, 5, "ONYX", [0, 0, 0, 7, 3]),

    # Level 3 - Diamond bonus (4 cards, id 82-85)
    (82, 3, 3, "DIAMOND", [0, 3, 3, 5, 3]),
    (83, 3, 4, "DIAMOND", [0, 0, 0, 0, 7]),
    (84, 3, 4, "DIAMOND", [3, 0, 0, 3, 6]),
    (85, 3, 5, "DIAMOND", [3, 0, 0, 0, 7]),

    # Level 3 - Emerald bonus (4 cards, id 86-89)
    (86, 3, 3, "EMERALD", [5, 3, 0, 3, 3]),
    (87, 3, 4, "EMERALD", [0, 7, 0, 0, 0]),
    (88, 3, 4, "EMERALD", [3, 6, 3, 0, 0]),
    (89, 3, 5, "EMERALD", [0, 7, 3, 0, 0]),
]

MAX_GOLD = 5  # 2-player game has 5 gold tokens


def count_valid_patterns_for_card(cost: List[int]) -> Tuple[int, List[Tuple[int, ...]]]:
    """
    Count valid gold_as patterns for a card with given cost.
    
    Constraints:
    - gold_as[c] <= cost[c] for each color c
    - sum(gold_as) <= MAX_GOLD
    
    Returns: (count, list of valid patterns)
    """
    patterns = []
    
    # For each color, the valid range of gold usage is [0, min(cost[c], available_gold_remaining)]
    # We iterate through all combinations
    ranges = [range(min(c + 1, MAX_GOLD + 1)) for c in cost]
    
    for gold_as in product(*ranges):
        if sum(gold_as) <= MAX_GOLD:
            # All constraints satisfied
            patterns.append(gold_as)
    
    return len(patterns), patterns


def main():
    print("=" * 80)
    print("Payment Pattern Analysis for Card-Based Action Space")
    print("=" * 80)
    print()
    print(f"Constraints:")
    print(f"  - gold_as[c] <= cost[c] (can't substitute more than needed)")
    print(f"  - sum(gold_as) <= {MAX_GOLD} (max gold in 2-player game)")
    print()
    
    total_patterns = 0
    patterns_by_level = {1: 0, 2: 0, 3: 0}
    cards_by_level = {1: 0, 2: 0, 3: 0}
    
    # Group cards by cost pattern to find unique cost structures
    cost_to_cards = {}
    
    print("-" * 80)
    print(f"{'ID':>3} {'Lvl':>3} {'Cost':>20} {'Patterns':>10} {'Notes'}")
    print("-" * 80)
    
    for card_id, level, points, bonus, cost in CARDS_RAW:
        count, patterns = count_valid_patterns_for_card(cost)
        total_patterns += count
        patterns_by_level[level] += count
        cards_by_level[level] += 1
        
        cost_key = tuple(cost)
        if cost_key not in cost_to_cards:
            cost_to_cards[cost_key] = []
        cost_to_cards[cost_key].append(card_id)
        
        cost_str = str(cost)
        total_cost = sum(cost)
        note = f"sum={total_cost}"
        print(f"{card_id:>3} {level:>3} {cost_str:>20} {count:>10} {note}")
    
    print("-" * 80)
    print()
    
    # Summary
    print("=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print()
    print(f"Total cards: 90")
    print(f"Total payment patterns (sum of all cards): {total_patterns}")
    print()
    print("By Level:")
    for level in [1, 2, 3]:
        avg = patterns_by_level[level] / cards_by_level[level] if cards_by_level[level] > 0 else 0
        print(f"  Level {level}: {cards_by_level[level]} cards, {patterns_by_level[level]} patterns (avg {avg:.1f}/card)")
    print()
    
    # Compare with current slot-based approach
    current_visible = 12 * 252  # 12 slots * 252 patterns
    current_reserved = 3 * 252  # 3 slots * 252 patterns
    current_total = current_visible + current_reserved
    
    print("Comparison with current slot-based approach:")
    print(f"  Current PURCHASE_VISIBLE: 12 slots * 252 patterns = {current_visible}")
    print(f"  Current PURCHASE_RESERVED: 3 slots * 252 patterns = {current_reserved}")
    print(f"  Current total: {current_total}")
    print()
    
    # Card-based approach (all 90 cards, both visible and reserved scenarios)
    # Each card can appear either on the board OR in reserved hand
    card_based_total = total_patterns  # Each card_id maps to its patterns
    
    print("Card ID-based approach (assuming same card = same patterns anywhere):")
    print(f"  Total: 90 cards * their specific patterns = {total_patterns}")
    print()
    print(f"  Reduction: {current_total} -> {total_patterns} ({100 * (current_total - total_patterns) / current_total:.1f}% less)")
    print()
    
    # Unique cost patterns
    print(f"Unique cost patterns: {len(cost_to_cards)}")
    print()
    
    # Calculate action space with card-ID based approach
    # TAKE_DIFFERENT: 840 (unchanged)
    # TAKE_SAME: 140 (unchanged)
    # RESERVE_VISIBLE: 84 (unchanged)
    # RESERVE_DECK: 21 (unchanged)
    # PURCHASE: 90 cards * per-card patterns (variable)
    # VISIT_NOBLE: 3 (unchanged)
    # PASS: 1 (unchanged)
    
    other_actions = 840 + 140 + 84 + 21 + 3 + 1  # = 1089
    new_total = other_actions + total_patterns
    old_total = 4869
    
    print("=" * 80)
    print("NEW ACTION SPACE (Card ID-based)")
    print("=" * 80)
    print()
    print(f"| Category         | Size      |")
    print(f"|------------------|-----------|")
    print(f"| TAKE_DIFFERENT   | 840       |")
    print(f"| TAKE_SAME        | 140       |")
    print(f"| RESERVE_VISIBLE  | 84        |")
    print(f"| RESERVE_DECK     | 21        |")
    print(f"| PURCHASE (90 cards) | {total_patterns} |")
    print(f"| VISIT_NOBLE      | 3         |")
    print(f"| PASS             | 1         |")
    print(f"|------------------|-----------|")
    print(f"| **Total**        | **{new_total}** |")
    print()
    print(f"Comparison:")
    print(f"  Old (slot-based): {old_total}")
    print(f"  New (card-based): {new_total}")
    print(f"  Difference: {new_total - old_total:+d} ({100 * (new_total - old_total) / old_total:+.1f}%)")
    print()
    
    # Build offset table for each card
    print("=" * 80)
    print("CARD OFFSET TABLE (for implementation)")
    print("=" * 80)
    print()
    
    # First, calculate pattern count for each card once
    card_pattern_counts = []
    for card_id, level, points, bonus, cost in CARDS_RAW:
        count, _ = count_valid_patterns_for_card(cost)
        card_pattern_counts.append(count)
    
    # Calculate cumulative offsets
    cumulative = 0
    print(f"{'Card ID':>8} {'Patterns':>10} {'Offset':>10}")
    print("-" * 30)
    for i, count in enumerate(card_pattern_counts):
        print(f"{i:>8} {count:>10} {cumulative:>10}")
        cumulative += count
    print("-" * 30)
    print(f"{'Total':>8} {sum(card_pattern_counts):>10}")
    print()


if __name__ == "__main__":
    main()
