"""
Test token exchange (gem return) implementation in csplendor.

Verifies all 6 exchange scenarios have no duplicates and no omissions:
1. TAKE_DIFFERENT (3 taken), return 3 (player at 10 gems)
2. TAKE_DIFFERENT (3 taken), return 2 (player at 9 gems)
3. TAKE_DIFFERENT (3 taken), return 1 (player at 8 gems)
4. TAKE_SAME (2 taken), return 2 (player at 10 gems)
5. TAKE_SAME (2 taken), return 1 (player at 9 gems)
6. RESERVE + gold (1 taken), return 1 (player at 10 gems)
"""

from collections import defaultdict
from itertools import product

import pytest

from csplendor import Game, Action, ActionType, ActionEncoderV2


# ─── Helper functions ───

def setup_game_with_gems(gems, seed=42, bank=None):
    """Create a game and set player 0's gems to the given distribution."""
    game = Game(seed=seed)
    if bank is not None:
        game.board.bank = list(bank)
    p0 = game.board.players[0]
    p0.gems = list(gems)
    game.board.set_player(0, p0)
    return game


def return_tuple(action):
    """Return gems as a hashable tuple."""
    return tuple(action.return_gems)


def take_tuple(action):
    """Take gems as a hashable tuple."""
    return tuple(action.take)


def action_base_key(action):
    """Return the action identity without return_gems."""
    if action.type in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME):
        return action.type, tuple(action.take)
    if action.type == ActionType.RESERVE_VISIBLE:
        return action.type, action.card_id
    if action.type == ActionType.RESERVE_DECK:
        return action.type, action.deck_level
    return action.type, action.card_id, action.from_reserved


def gem_distributions(total=None):
    """Enumerate plausible two-player token holdings: colors <=4, gold <=5."""
    for colors in product(range(5), repeat=5):
        for gold in range(6):
            gems = colors + (gold,)
            if total is None:
                if sum(gems) <= 10:
                    yield gems
            elif sum(gems) == total:
                yield gems


def compute_expected_return_combos(available, excess):
    """
    Compute all valid return combinations given available[6] gems and excess count.
    Returns a set of tuples. This is the ground-truth reference implementation.
    """
    results = set()
    buf = [0] * 6

    def recurse(remaining, color_idx):
        if remaining == 0:
            results.add(tuple(buf))
            return
        if color_idx == 6:
            return
        max_return = min(remaining, available[color_idx])
        for i in range(max_return + 1):
            buf[color_idx] = i
            recurse(remaining - i, color_idx + 1)
            buf[color_idx] = 0

    recurse(excess, 0)
    return results


def simulate_next_gems(player_gems, action, board):
    """Simulate the player's gems after taking/reserving (before return)."""
    next_g = list(player_gems)
    if action.type in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME):
        for i in range(5):
            next_g[i] += action.take[i]
    elif action.type in (ActionType.RESERVE_VISIBLE, ActionType.RESERVE_DECK):
        if board.bank[5] > 0:  # Gold in bank
            next_g[5] += 1
    return next_g


def verify_all_return_groups(game, action_types=None, label=""):
    """
    Verify every generated gem-return group against a ground-truth return set.

    Unlike verify_exchange_actions(), this does not assume one fixed excess
    value. It is suitable for broad state enumeration where each base action
    may require 0, 1, 2, or 3 returned tokens.
    """
    player_gems = list(game.board.players[game.board.current_player].gems)
    groups = defaultdict(list)
    for action in game.legal_actions:
        if action_types is not None and action.type not in action_types:
            continue
        groups[action_base_key(action)].append(action)

    checked_groups = 0
    checked_actions = 0
    for key, actions in groups.items():
        next_gems = simulate_next_gems(player_gems, actions[0], game.board)
        excess = max(0, sum(next_gems) - 10)
        expected = compute_expected_return_combos(next_gems, excess)
        actual = {return_tuple(action) for action in actions}

        assert actual == expected, (
            f"[{label}] key={key}: return set mismatch. "
            f"missing={sorted(expected - actual)[:8]} "
            f"extra={sorted(actual - expected)[:8]} "
            f"player_gems={player_gems} next_gems={next_gems}"
        )

        assert len(actual) == len(actions), (
            f"[{label}] key={key}: duplicate return_gems generated"
        )

        for action in actions:
            assert sum(action.return_gems) == excess, (
                f"[{label}] key={key}: expected return sum {excess}, "
                f"got {sum(action.return_gems)} for {list(action.return_gems)}"
            )
            assert sum(next_gems) - sum(action.return_gems) <= 10

        checked_groups += 1
        checked_actions += len(actions)

    return checked_groups, checked_actions


# ─── Core verification function ───

def verify_exchange_actions(game, action_type, expected_excess, label=""):
    """
    For the given action_type, verify:
    - All generated actions have return_gems sum == expected_excess
    - No duplicate (base_action, return_gems) pairs
    - No missing return combos (compare with ground-truth)
    - Post-action total gems <= 10
    """
    legals = game.legal_actions
    player_gems = list(game.board.players[0].gems)
    board = game.board

    # Filter by action type
    filtered = [a for a in legals if a.type == action_type]
    assert len(filtered) > 0, f"[{label}] No {action_type} actions generated"

    # Group by base action key
    # For TAKE_DIFFERENT/TAKE_SAME: key = take tuple
    # For RESERVE_VISIBLE: key = card_id
    # For RESERVE_DECK: key = deck_level
    groups = defaultdict(list)
    for a in filtered:
        if action_type in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME):
            key = take_tuple(a)
        elif action_type == ActionType.RESERVE_VISIBLE:
            key = ("vis", a.card_id)
        elif action_type == ActionType.RESERVE_DECK:
            key = ("deck", a.deck_level)
        else:
            key = "other"
        groups[key].append(a)

    total_actions = 0
    total_missing = 0
    total_duplicates = 0

    for key, actions in groups.items():
        # Get next_gems for this base action (all actions in group have same take)
        next_g = simulate_next_gems(player_gems, actions[0], board)
        total_after_take = sum(next_g)
        excess = max(0, total_after_take - 10)

        # Verify expected excess
        assert excess == expected_excess, (
            f"[{label}] key={key}: expected excess={expected_excess}, got {excess} "
            f"(player_gems={player_gems}, next_gems={next_g})"
        )

        # Check return_gems sum for each action
        for a in actions:
            ret_sum = sum(a.return_gems)
            assert ret_sum == expected_excess, (
                f"[{label}] key={key}: action has return_gems sum={ret_sum}, "
                f"expected {expected_excess}. return_gems={list(a.return_gems)}"
            )

        # Check for duplicates
        seen_returns = set()
        for a in actions:
            rt = return_tuple(a)
            if rt in seen_returns:
                total_duplicates += 1
            seen_returns.add(rt)

        # Compute ground-truth expected return combos
        expected_combos = compute_expected_return_combos(next_g, excess)

        # Check for missing combos (in MoveGenerator's output)
        missing = expected_combos - seen_returns
        if missing:
            total_missing += len(missing)
            print(f"  [{label}] key={key}: MISSING {len(missing)} return combos")
            for m in sorted(missing)[:5]:
                print(f"    {m}")

        # Check for extra combos (should not happen)
        extra = seen_returns - expected_combos
        assert len(extra) == 0, (
            f"[{label}] key={key}: {len(extra)} EXTRA return combos not in expected set"
        )

        # Verify post-action gem total <= 10
        for a in actions:
            final_total = sum(next_g) - sum(a.return_gems)
            assert final_total <= 10, (
                f"[{label}] key={key}: post-action total={final_total} > 10"
            )
            assert final_total == 10, (
                f"[{label}] key={key}: post-action total={final_total} != 10 "
                f"(should return exactly excess={expected_excess})"
            )

        total_actions += len(actions)

    return {
        "action_count": total_actions,
        "group_count": len(groups),
        "missing": total_missing,
        "duplicates": total_duplicates,
    }


# ─── Test: TAKE_DIFFERENT scenarios ───

class TestTakeDifferentExchange:
    """Test TAKE_DIFFERENT gem return (3 gems taken)."""

    def test_take3_return1(self):
        """3 taken, 1 returned (player at 8 gems → 11 → return 1)."""
        game = setup_game_with_gems([2, 2, 2, 1, 1, 0])  # sum=8
        result = verify_exchange_actions(
            game, ActionType.TAKE_DIFFERENT, expected_excess=1,
            label="TD_ret1"
        )
        print(f"TAKE_DIFFERENT return 1: {result['action_count']} actions "
              f"in {result['group_count']} groups")
        assert result["duplicates"] == 0, "Duplicates found"
        assert result["missing"] == 0, "Missing return combos"

    def test_take3_return2(self):
        """3 taken, 2 returned (player at 9 gems → 12 → return 2)."""
        game = setup_game_with_gems([2, 2, 2, 2, 1, 0])  # sum=9
        result = verify_exchange_actions(
            game, ActionType.TAKE_DIFFERENT, expected_excess=2,
            label="TD_ret2"
        )
        print(f"TAKE_DIFFERENT return 2: {result['action_count']} actions "
              f"in {result['group_count']} groups")
        assert result["duplicates"] == 0, "Duplicates found"
        assert result["missing"] == 0, "Missing return combos"

    def test_take3_return3(self):
        """3 taken, 3 returned (player at 10 gems → 13 → return 3)."""
        game = setup_game_with_gems([2, 2, 2, 2, 2, 0])  # sum=10
        result = verify_exchange_actions(
            game, ActionType.TAKE_DIFFERENT, expected_excess=3,
            label="TD_ret3"
        )
        print(f"TAKE_DIFFERENT return 3: {result['action_count']} actions "
              f"in {result['group_count']} groups")
        assert result["duplicates"] == 0, "Duplicates found"
        assert result["missing"] == 0, "Missing return combos"

    def test_take3_return3_with_gold(self):
        """Player has gold tokens; verify gold can be returned too."""
        game = setup_game_with_gems([1, 1, 1, 1, 1, 5])  # sum=10, 5 gold
        result = verify_exchange_actions(
            game, ActionType.TAKE_DIFFERENT, expected_excess=3,
            label="TD_ret3_gold"
        )
        print(f"TAKE_DIFFERENT return 3 (with gold): {result['action_count']} actions "
              f"in {result['group_count']} groups")
        assert result["duplicates"] == 0, "Duplicates found"
        assert result["missing"] == 0, "Missing return combos"


# ─── Test: TAKE_SAME scenarios ───

class TestTakeSameExchange:
    """Test TAKE_SAME gem return (2 gems taken of same color)."""

    def test_take2_return1(self):
        """2 taken, 1 returned (player at 9 gems → 11 → return 1)."""
        game = setup_game_with_gems([2, 2, 2, 2, 1, 0])  # sum=9
        result = verify_exchange_actions(
            game, ActionType.TAKE_SAME, expected_excess=1,
            label="TS_ret1"
        )
        print(f"TAKE_SAME return 1: {result['action_count']} actions "
              f"in {result['group_count']} groups")
        assert result["duplicates"] == 0, "Duplicates found"
        assert result["missing"] == 0, "Missing return combos"

    def test_take2_return2(self):
        """2 taken, 2 returned (player at 10 gems → 12 → return 2)."""
        game = setup_game_with_gems([2, 2, 2, 2, 2, 0])  # sum=10
        result = verify_exchange_actions(
            game, ActionType.TAKE_SAME, expected_excess=2,
            label="TS_ret2"
        )
        print(f"TAKE_SAME return 2: {result['action_count']} actions "
              f"in {result['group_count']} groups")
        assert result["duplicates"] == 0, "Duplicates found"
        assert result["missing"] == 0, "Missing return combos"

    def test_take2_return2_with_gold(self):
        """Player has gold; verify gold can also be returned."""
        game = setup_game_with_gems([1, 1, 1, 1, 3, 3])  # sum=10
        result = verify_exchange_actions(
            game, ActionType.TAKE_SAME, expected_excess=2,
            label="TS_ret2_gold"
        )
        print(f"TAKE_SAME return 2 (with gold): {result['action_count']} actions "
              f"in {result['group_count']} groups")
        assert result["duplicates"] == 0, "Duplicates found"
        assert result["missing"] == 0, "Missing return combos"


# ─── Test: RESERVE scenarios ───

class TestReserveExchange:
    """Test RESERVE gem return (1 gold received from bank)."""

    def test_reserve_visible_return1(self):
        """Reserve visible card, 1 gold received, 1 returned (player at 10)."""
        game = setup_game_with_gems([2, 2, 2, 2, 2, 0])  # sum=10
        # Ensure bank has gold
        assert game.board.bank[5] > 0, "Bank must have gold for this test"
        # Ensure player can reserve
        p0 = game.board.players[0]
        assert p0.reserved_count < 3, "Player must be able to reserve"

        result = verify_exchange_actions(
            game, ActionType.RESERVE_VISIBLE, expected_excess=1,
            label="RV_ret1"
        )
        print(f"RESERVE_VISIBLE return 1: {result['action_count']} actions "
              f"in {result['group_count']} groups")
        assert result["duplicates"] == 0, "Duplicates found"
        assert result["missing"] == 0, "Missing return combos"

    def test_reserve_deck_return1(self):
        """Reserve from deck, 1 gold received, 1 returned (player at 10)."""
        game = setup_game_with_gems([2, 2, 2, 2, 2, 0])  # sum=10
        assert game.board.bank[5] > 0, "Bank must have gold for this test"

        result = verify_exchange_actions(
            game, ActionType.RESERVE_DECK, expected_excess=1,
            label="RD_ret1"
        )
        print(f"RESERVE_DECK return 1: {result['action_count']} actions "
              f"in {result['group_count']} groups")
        assert result["duplicates"] == 0, "Duplicates found"
        assert result["missing"] == 0, "Missing return combos"

    def test_reserve_return1_with_gold(self):
        """Player already has gold; can return the gold just received."""
        game = setup_game_with_gems([1, 1, 1, 1, 1, 5])  # sum=10, 5 gold
        assert game.board.bank[5] > 0

        result = verify_exchange_actions(
            game, ActionType.RESERVE_VISIBLE, expected_excess=1,
            label="RV_ret1_gold"
        )
        print(f"RESERVE_VISIBLE return 1 (with gold): {result['action_count']} actions "
              f"in {result['group_count']} groups")
        assert result["duplicates"] == 0, "Duplicates found"
        assert result["missing"] == 0, "Missing return combos"


# ─── Test: Exhaustive gem-return coverage ───

class TestExhaustiveExchangeCoverage:
    """Broadly enumerate token distributions and verify return-gem coverage."""

    EXCHANGE_TYPES = {
        ActionType.TAKE_DIFFERENT,
        ActionType.TAKE_SAME,
        ActionType.RESERVE_VISIBLE,
        ActionType.RESERVE_DECK,
    }

    def test_full_bank_all_player_token_distributions(self):
        """
        With a normal full bank, every plausible player token distribution
        must generate exactly the expected return set for every exchange group.
        """
        checked_states = 0
        checked_groups = 0
        checked_actions = 0

        for gems in gem_distributions():
            game = setup_game_with_gems(gems)
            groups, actions = verify_all_return_groups(
                game, self.EXCHANGE_TYPES, label=f"full_bank gems={gems}"
            )
            checked_states += 1
            checked_groups += groups
            checked_actions += actions

        assert checked_states == 5498
        assert checked_groups > 0
        assert checked_actions > 0

    def test_depleted_bank_take_different_one_or_two_colors(self):
        """
        If the bank has fewer than 3 normal colors, TAKE_DIFFERENT takes all
        available colors. Cover the take-1 and take-2 return cases explicitly.
        """
        checked_states = 0
        checked_groups = 0
        checked_actions = 0

        for available_count in (1, 2):
            for mask in range(1, 1 << 5):
                if mask.bit_count() != available_count:
                    continue
                bank = tuple(1 if mask & (1 << i) else 0 for i in range(5)) + (5,)
                for gems in gem_distributions(total=10):
                    game = setup_game_with_gems(gems, bank=bank)
                    groups, actions = verify_all_return_groups(
                        game,
                        {ActionType.TAKE_DIFFERENT},
                        label=f"depleted_bank bank={bank} gems={gems}",
                    )
                    checked_states += 1
                    checked_groups += groups
                    checked_actions += actions

        assert checked_states == 15 * 1627
        assert checked_groups > 0
        assert checked_actions > 0

    def test_depleted_bank_no_gold_reserve_has_no_return(self):
        """
        Reserving when the bank has no gold must not invent a return action,
        even if the player already has 10 tokens.
        """
        checked_groups = 0
        checked_actions = 0
        for gems in gem_distributions(total=10):
            game = setup_game_with_gems(gems, bank=(4, 4, 4, 4, 4, 0))
            groups, actions = verify_all_return_groups(
                game,
                {ActionType.RESERVE_VISIBLE, ActionType.RESERVE_DECK},
                label=f"no_gold_reserve gems={gems}",
            )
            checked_groups += groups
            checked_actions += actions

        assert checked_groups > 0
        assert checked_actions > 0


# ─── Test: ActionEncoderV2 coverage ───

class TestEncoderV2Coverage:
    """Verify ActionEncoderV2 can encode all legal exchange actions."""

    def _check_encoder(self, game, label=""):
        """Check that every legal action can be encoded, and no collisions."""
        legals = game.legal_actions
        encoded_map = defaultdict(list)  # id -> list of actions
        encode_failures = []

        for a in legals:
            eid = ActionEncoderV2.encode(a, game)
            if eid < 0 or eid >= ActionEncoderV2.ACTION_SIZE:
                encode_failures.append((a, eid))
            else:
                encoded_map[eid].append(a)

        # Report failures
        if encode_failures:
            print(f"\n[{label}] ENCODE FAILURES ({len(encode_failures)}):")
            for a, eid in encode_failures[:10]:
                print(f"  {a} -> id={eid}")

        # Report collisions (different return_gems mapping to same id)
        collisions = 0
        for eid, actions in encoded_map.items():
            if len(actions) > 1:
                # Check if they have different return_gems
                returns = set(return_tuple(a) for a in actions)
                if len(returns) > 1:
                    collisions += 1
                    if collisions <= 3:
                        print(f"\n[{label}] COLLISION at id={eid}:")
                        for a in actions:
                            print(f"  {a}")

        return {
            "total": len(legals),
            "failures": len(encode_failures),
            "collisions": collisions,
        }

    def test_encoder_take_diff_return1(self):
        """Encoder covers TAKE_DIFFERENT with return 1."""
        game = setup_game_with_gems([2, 2, 2, 1, 1, 0])  # excess=1
        result = self._check_encoder(game, "enc_TD_ret1")
        print(f"Encoder TD ret1: {result}")
        assert result["failures"] == 0
        assert result["collisions"] == 0

    def test_encoder_take_diff_return2(self):
        """Encoder covers TAKE_DIFFERENT with return 2."""
        game = setup_game_with_gems([2, 2, 2, 2, 1, 0])  # excess=2
        result = self._check_encoder(game, "enc_TD_ret2")
        print(f"Encoder TD ret2: {result}")
        assert result["failures"] == 0
        assert result["collisions"] == 0

    def test_encoder_take_diff_return3(self):
        """Encoder covers TAKE_DIFFERENT with return 3."""
        game = setup_game_with_gems([2, 2, 2, 2, 2, 0])  # excess=3
        result = self._check_encoder(game, "enc_TD_ret3")
        print(f"Encoder TD ret3: {result}")
        assert result["failures"] == 0
        assert result["collisions"] == 0

    def test_encoder_take_same_return2(self):
        """Encoder covers TAKE_SAME with return 2."""
        game = setup_game_with_gems([2, 2, 2, 2, 2, 0])  # excess=2
        result = self._check_encoder(game, "enc_TS_ret2")
        print(f"Encoder TS ret2: {result}")
        assert result["failures"] == 0
        assert result["collisions"] == 0

    def test_encoder_reserve_return1(self):
        """Encoder covers RESERVE with return 1."""
        game = setup_game_with_gems([2, 2, 2, 2, 2, 0])  # excess=1
        result = self._check_encoder(game, "enc_RES_ret1")
        print(f"Encoder RES ret1: {result}")
        assert result["failures"] == 0
        assert result["collisions"] == 0


# ─── Test: Cross-verification (V2 encode-decode round-trip) ───

class TestEncoderRoundTrip:
    """Verify encode→decode_and_match round-trip preserves action semantics."""

    def test_round_trip_no_exchange(self):
        """Normal actions (no excess) round-trip correctly."""
        game = setup_game_with_gems([0, 0, 0, 0, 0, 0])  # sum=0, no returns
        legals = game.legal_actions
        failures = 0
        for a in legals:
            eid = ActionEncoderV2.encode(a, game)
            if eid < 0:
                continue
            decoded = ActionEncoderV2.decode_and_match(eid, game)
            if decoded.type != a.type:
                failures += 1
        assert failures == 0, f"{failures} round-trip type mismatches"

    def test_round_trip_exchange(self):
        """Exchange actions round-trip correctly."""
        game = setup_game_with_gems([2, 2, 2, 1, 1, 0])  # excess=1 for TD
        legals = game.legal_actions
        exchange_actions = [a for a in legals
                           if a.type == ActionType.TAKE_DIFFERENT and sum(a.return_gems) > 0]
        failures = 0
        for a in exchange_actions:
            eid = ActionEncoderV2.encode(a, game)
            if eid < 0:
                continue
            decoded = ActionEncoderV2.decode_and_match(eid, game)
            # return_gems should match
            if return_tuple(a) != return_tuple(decoded):
                failures += 1
        print(f"Exchange round-trip: {len(exchange_actions)} actions, "
              f"{failures} return_gems mismatches")
        assert failures == 0


# ─── Test: Comprehensive statistics ───

class TestExchangeStatistics:
    """Print comprehensive statistics about exchange action counts."""

    def test_print_stats(self):
        """Print detailed statistics for analysis (always passes)."""
        configs = [
            # (gems, label)
            ([2, 2, 2, 1, 1, 0], "8 gems (TD:ret1)"),
            ([2, 2, 2, 2, 1, 0], "9 gems (TD:ret2, TS:ret1)"),
            ([2, 2, 2, 2, 2, 0], "10 gems (TD:ret3, TS:ret2, RES:ret1)"),
            ([1, 1, 1, 1, 1, 5], "10 gems with gold"),
            ([0, 0, 0, 0, 0, 10], "10 gold only"),
        ]

        for gems, label in configs:
            game = setup_game_with_gems(gems)
            legals = game.legal_actions
            assert game.legal_action_count == len(legals)

            by_type = defaultdict(int)
            by_type_exchange = defaultdict(int)
            for a in legals:
                by_type[a.type] += 1
                if sum(a.return_gems) > 0:
                    by_type_exchange[a.type] += 1

            print(f"\n=== {label} (gems={gems}) ===")
            print(f"  Total legal actions: {len(legals)}")
            for t in [ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME,
                       ActionType.RESERVE_VISIBLE, ActionType.RESERVE_DECK,
                       ActionType.PURCHASE]:
                total = by_type.get(t, 0)
                exchange = by_type_exchange.get(t, 0)
                if total > 0:
                    print(f"  {t.name}: {total} total, {exchange} with exchange")

            # Detailed: for each TAKE_DIFFERENT combo, how many return variants?
            td_actions = [a for a in legals if a.type == ActionType.TAKE_DIFFERENT]
            if td_actions:
                groups = defaultdict(list)
                for a in td_actions:
                    groups[take_tuple(a)].append(a)
                print(f"  TAKE_DIFFERENT breakdown:")
                for key in sorted(groups.keys()):
                    taken_colors = [i for i in range(5) if key[i] > 0]
                    n = len(groups[key])
                    # Check how many involve returning a taken color
                    redundant = sum(1 for a in groups[key]
                                    if any(a.return_gems[c] > 0 for c in taken_colors))
                    non_redundant = n - redundant
                    print(f"    combo {taken_colors}: {n} variants "
                          f"({non_redundant} non-redundant, {redundant} return-taken)")

            # Detailed: for each TAKE_SAME color, how many return variants?
            ts_actions = [a for a in legals if a.type == ActionType.TAKE_SAME]
            if ts_actions:
                groups = defaultdict(list)
                for a in ts_actions:
                    color = next(i for i in range(5) if a.take[i] == 2)
                    groups[color].append(a)
                print(f"  TAKE_SAME breakdown:")
                for color in sorted(groups.keys()):
                    n = len(groups[color])
                    redundant = sum(1 for a in groups[color]
                                    if a.return_gems[color] > 0)
                    non_redundant = n - redundant
                    print(f"    color {color}: {n} variants "
                          f"({non_redundant} non-redundant, {redundant} return-taken)")

            assert sum(by_type.values()) == len(legals)
            assert sum(by_type_exchange.values()) > 0


# ─── Test: Verify non-redundant subset matches V2 encoder pattern counts ───

class TestNonRedundantCounts:
    """Verify the NON-REDUNDANT return variants match ActionEncoderV2's pattern counts."""

    def _count_non_redundant_returns(self, actions, taken_colors):
        """Count actions that don't return any taken color."""
        return sum(1 for a in actions
                   if all(a.return_gems[c] == 0 for c in taken_colors))

    def test_take_diff_non_redundant_return1(self):
        """TD excess=1: non-redundant should use 3 returnable colors, return 1 → 3 patterns."""
        game = setup_game_with_gems([2, 2, 2, 1, 1, 0])  # excess=1
        td_actions = [a for a in game.legal_actions
                      if a.type == ActionType.TAKE_DIFFERENT]
        groups = defaultdict(list)
        for a in td_actions:
            groups[take_tuple(a)].append(a)

        for key, actions in groups.items():
            taken = [i for i in range(5) if key[i] > 0]
            next_gems = simulate_next_gems(list(game.board.players[0].gems),
                                           actions[0], game.board)
            expected = {
                ret for ret in compute_expected_return_combos(next_gems, 1)
                if all(ret[c] == 0 for c in taken)
            }
            actual = {
                return_tuple(a) for a in actions
                if all(a.return_gems[c] == 0 for c in taken)
            }
            assert actual == expected

    def test_take_diff_non_redundant_return2(self):
        """TD excess=2: non-redundant from 3 colors, return 2 → H(3,2)=6 patterns."""
        game = setup_game_with_gems([2, 2, 2, 2, 1, 0])  # excess=2
        td_actions = [a for a in game.legal_actions
                      if a.type == ActionType.TAKE_DIFFERENT]
        groups = defaultdict(list)
        for a in td_actions:
            groups[take_tuple(a)].append(a)

        for key, actions in groups.items():
            taken = [i for i in range(5) if key[i] > 0]
            next_gems = simulate_next_gems(list(game.board.players[0].gems),
                                           actions[0], game.board)
            expected = {
                ret for ret in compute_expected_return_combos(next_gems, 2)
                if all(ret[c] == 0 for c in taken)
            }
            actual = {
                return_tuple(a) for a in actions
                if all(a.return_gems[c] == 0 for c in taken)
            }
            assert actual == expected

    def test_take_diff_non_redundant_return3(self):
        """TD excess=3: non-redundant from 3 colors, return 3 → H(3,3)=10 patterns max."""
        game = setup_game_with_gems([2, 2, 2, 2, 2, 0])  # excess=3
        td_actions = [a for a in game.legal_actions
                      if a.type == ActionType.TAKE_DIFFERENT]
        groups = defaultdict(list)
        for a in td_actions:
            groups[take_tuple(a)].append(a)

        for key, actions in groups.items():
            taken = [i for i in range(5) if key[i] > 0]
            next_gems = simulate_next_gems(list(game.board.players[0].gems),
                                           actions[0], game.board)
            expected = {
                ret for ret in compute_expected_return_combos(next_gems, 3)
                if all(ret[c] == 0 for c in taken)
            }
            actual = {
                return_tuple(a) for a in actions
                if all(a.return_gems[c] == 0 for c in taken)
            }
            assert actual == expected

    def test_encoder_v2_pattern_coverage_return3(self):
        """
        Critical test: V2 has only 10 patterns for TAKE_DIFFERENT (covers return 0,1,2).
        Return 3 requires H(3,3)=10 additional patterns. This test detects the gap.
        """
        game = setup_game_with_gems([2, 2, 2, 2, 2, 0])  # excess=3
        td_actions = [a for a in game.legal_actions
                      if a.type == ActionType.TAKE_DIFFERENT and sum(a.return_gems) == 3]

        # Among these, find non-redundant ones (don't return taken colors)
        encode_ok = 0
        encode_fail = 0
        encode_collision = 0
        seen_ids = defaultdict(list)

        for a in td_actions:
            taken = [i for i in range(5) if a.take[i] > 0]
            is_redundant = any(a.return_gems[c] > 0 for c in taken)

            eid = ActionEncoderV2.encode(a, game)

            if not is_redundant:
                if eid < 0 or eid >= ActionEncoderV2.ACTION_SIZE:
                    encode_fail += 1
                    print(f"  FAIL: take={list(a.take)} return={list(a.return_gems)} "
                          f"-> eid={eid}")
                else:
                    encode_ok += 1
                    seen_ids[eid].append(a)

        # Check collisions
        for eid, actions in seen_ids.items():
            if len(actions) > 1:
                encode_collision += 1

        print(f"\nV2 encoder coverage for TD return 3:")
        print(f"  Non-redundant: {encode_ok} encoded OK, {encode_fail} FAILED")
        print(f"  Collisions: {encode_collision}")

        if encode_fail > 0:
            print(f"\n  *** GAP DETECTED: {encode_fail} non-redundant return-3 actions "
                  f"cannot be encoded by ActionEncoderV2! ***")

        assert encode_fail == 0
        assert encode_collision == 0


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
