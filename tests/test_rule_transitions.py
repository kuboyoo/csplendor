"""Rule transition and reveal-search structural regression contracts."""

import hashlib
import json
from collections import Counter

import pytest

import csplendor as cs
from tests.support import game_state_signature as _game_signature

TARGET_TYPES = {
    cs.ActionType.RESERVE_VISIBLE,
    cs.ActionType.RESERVE_DECK,
    cs.ActionType.PURCHASE,
    cs.ActionType.VISIT_NOBLE,
}


def _sha256(payload):
    encoded = json.dumps(payload, separators=(",", ":"), sort_keys=False)
    return hashlib.sha256(encoded.encode()).hexdigest()


def test_reserve_and_purchase_full_state_corpus_digest():
    records = []
    coverage = Counter()
    for seed in range(16):
        game = cs.Game(seed=seed)
        for ply in range(36):
            actions = game.legal_actions
            for action in actions:
                if action.type not in TARGET_TYPES:
                    continue
                key = (
                    int(action.type),
                    bool(action.from_reserved)
                    if action.type == cs.ActionType.PURCHASE
                    else False,
                )
                coverage[key] += 1
                blank_modes = [False]
                if action.type in (
                    cs.ActionType.RESERVE_VISIBLE,
                    cs.ActionType.PURCHASE,
                ) and not action.from_reserved:
                    blank_modes.append(True)
                for blank_refill in blank_modes:
                    child = game.clone_light()
                    child.blank_refill_mode = blank_refill
                    applied = child.apply_trusted(action, False)
                    records.append(
                        [
                            seed,
                            ply,
                            int(action.pack()),
                            blank_refill,
                            applied,
                            _game_signature(child, include_hash=True),
                        ]
                    )

            if not actions or game.is_game_over():
                break
            preferred = [a for a in actions if a.type in TARGET_TYPES]
            choices = preferred or actions
            action = choices[(seed + ply * 7) % len(choices)]
            assert game.apply_trusted(action, False)

    assert coverage == Counter(
        {
            (int(cs.ActionType.RESERVE_VISIBLE), False): 1512,
            (int(cs.ActionType.RESERVE_DECK), False): 378,
            (int(cs.ActionType.PURCHASE), False): 449,
            (int(cs.ActionType.PURCHASE), True): 67,
        }
    )
    assert len(records) == 4367
    assert _sha256(records) == (
        "cfe99d9bc5d70ccd5926093396b8ec02f90bb7643d41ed0f4928bb51a93ad7ac"
    )


def test_noble_and_end_turn_full_state_corpus_digest():
    records = []
    for current_player in (0, 1):
        for noble_count in (0, 1, 2, 3):
            for points in (0, 14, 15):
                for other_points in (0, 14, 15, 18):
                    for purchased_counts in ((0, 0), (2, 3), (3, 2), (3, 3)):
                        game = cs.Game(seed=11)
                        game.board.current_player = current_player
                        game.board.nobles = list(range(noble_count))
                        game.board.turn = 7
                        for player_index in range(2):
                            player = game.board.get_player(player_index)
                            player.bonuses = (
                                [10] * 5
                                if player_index == current_player
                                else [0] * 5
                            )
                            player.points = (
                                points
                                if player_index == current_player
                                else other_points
                            )
                            player.purchased_count = purchased_counts[
                                player_index
                            ]
                            player.purchased_cards = list(
                                range(purchased_counts[player_index])
                            )
                            game.board.set_player(player_index, player)

                        action = next(
                            (
                                candidate
                                for candidate in game.legal_actions
                                if candidate.type
                                in (
                                    cs.ActionType.TAKE_DIFFERENT,
                                    cs.ActionType.TAKE_SAME,
                                )
                            ),
                            None,
                        )
                        if action is None:
                            continue
                        applied = game.apply_trusted(action, False)
                        records.append(
                            [
                                current_player,
                                noble_count,
                                points,
                                other_points,
                                purchased_counts,
                                int(action.pack()),
                                applied,
                                _game_signature(game, include_hash=True),
                            ]
                        )
                        if game.board.waiting_noble:
                            for noble_action in game.legal_actions:
                                child = game.clone_light()
                                applied = child.apply_trusted(noble_action, False)
                                records.append(
                                    [
                                        "visit",
                                        current_player,
                                        noble_count,
                                        points,
                                        other_points,
                                        purchased_counts,
                                        int(noble_action.pack()),
                                        applied,
                                        _game_signature(child, include_hash=True),
                                    ]
                                )

    assert len(records) == 864
    assert _sha256(records) == (
        "68c150a01838d0c8bb803f76af6150a012d30c6addfccd2851e5d6baa4391c04"
    )


def _solver_payload(game, attacker, depth, *, include_proof_dag=False):
    result = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=attacker,
        depth=depth,
        include_proof_dag=include_proof_dag,
        proof_dag_node_limit=200000,
        proof_dag_edge_limit=500000,
    )
    stats = {
        key: value
        for key, value in result["stats"].items()
        if key != "elapsed_ms"
    }
    payload = {
        "proven": result["proven"],
        "reason": result["reason"],
        "unknown_reason": result["unknown_reason"],
        "memoized_states": result["memoized_states"],
        "stats": stats,
        "line": result["line"],
        "proof_dag": result["proof_dag"],
    }
    return result, hashlib.sha256(
        json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


# Digests include every RevealVerifiedSearchStats counter. The six counters
# added on 2026-10-02 (hint_*, line_order_hits, probe_dropped_actions,
# memo_flushes) are all 0 here; without them each payload, node counts
# included, still hashes to its pre-change golden value.
# 2026-11-07: the last-move cut refutes attacker nodes with one move left
# that cannot reach 15 points without generating moves. The verdicts are
# unchanged; node counts shrink and an unproven line stops at the first cut
# node (lines are only consumed for proven mates). Previous digests:
# seed0_d1 060611af...cbb44c, seed7_d2 fe20c195...ec752, oracle_blank
# c880392b...61c63e.
# 2026-11-07 (blank dominance cut) adds the counters blank_cut_attempts and
# blank_cut_proofs (0 in all four fixtures). Every other field of every
# payload, node counts included, is unchanged from the previous build:
# seed0_d1 1e00dfdc...902bf, seed7_d2 b5171a73...ea29a, purchase_dag
# 867e5a82...6274, oracle_blank 9c9931a1...b57a.
# 2026-11-07 (oracle 予約の重複除去): the blank search no longer generates the
# defender's oracle reservations while the level's deck has cards (RESERVE_DECK
# covers them). Compared with the previous build, only legal_moves,
# oracle_reserve_actions and the line's action_count changed (seed7_d2: 403 ->
# 355 moves, 48 -> 0 reservations; oracle_blank: 65 -> 29, 36 -> 0); verdicts,
# node counts and lines are identical. Previous digests: seed7_d2
# 23ba3dfc...2ca286, oracle_blank 139a076c...45f0.
# 2026-11-07 (守り側の最終応手カット) adds the counters last_reply_cut_attempts
# and last_reply_cut_proofs (seed7_d2 tests 12 reply nodes, oracle_blank 1; no
# proof in any fixture). Every other field of every payload is unchanged:
# seed0_d1 949f9990...fc4948, seed7_d2 9bacde29...2eee7f, purchase_dag
# 0061b0e8...6bfa869, oracle_blank 14207fc5...263a26a.
# 2026-11-07 (攻め側の2手の到達上界による反証) adds the counter
# reach_cut_refutations. seed7_d2 (attacker at 0 points, two moves left) is now
# refuted at the root without generating moves (nodes 25 -> 1, legal_moves
# 355 -> 0, line empty); the other three payloads change only by the new key.
# Previous digests: seed0_d1 1054ca5d...0ee0dd83, seed7_d2 bc3aa5be...e79170f8,
# purchase_dag 4fc8e7c7...5de613d8, oracle_blank e1b3053d...7882939e.
# 2026-11-07 (守り側の応手の遅延生成): the defender's replies are generated
# group by group, so legal_moves counts only the generated moves. oracle_blank
# (defender to move, refuted by its first purchase) generates 11 moves instead
# of 29; the line's action_count stays 29 (resolved from the position), and
# every other field of every payload is unchanged. Previous digest:
# oracle_blank 1b6401ce...b3f25f.
# 2026-11-07 (C++ 版 df-pn) adds the counters dfpn_expansions, dfpn_table_stores,
# dfpn_table_replacements and dfpn_table_bytes (all 0 for the default DFS);
# every other field is unchanged. Previous digests: seed0_d1 c8f7160a...0a20f8,
# seed7_d2 b2bd1c91...1ba341, purchase_dag cb19043b...14434a, oracle_blank
# 8ab6054a...d6a4b6.
@pytest.mark.parametrize(
    ("fixture", "expected_digest"),
    [
        (
            "seed0_d1",
            "564e494a2b4d1a9ad09f97753e510433412ec0c009d3dc7bb20530a90dab2eac",
        ),
        (
            "seed7_d2",
            "46d3366bb36bbd7290504c2d42780baddb878c75c71c4d4b7509239fada569ea",
        ),
        (
            "purchase_dag",
            # Proof-DAG nodes gained "waiting_return" (2026-10-05); without it
            # the digest is a79c4e195b0d6c0d08049369ef21fedf8a39950f4675be1c4237f6adc59e53ff.
            # 2026-11-07: VISIT_NOBLE ends the attacker's turn, so the terminal
            # children of the noble choice are labelled depth 0 (was 1, digest
            # df6b13364b59ba3826ad566c1530e0a86405498cb392511f1a94e00bd2f0b097).
            "7fe83f26f976d00491f172c5cb03b0c444c8ed1e1afcec1d2f11dc3e34d7dadf",
        ),
        (
            "oracle_blank",
            "8edcdb33e3e41c2484486e4cf56862f3899148fc11c789fc66ff410a1711622b",
        ),
    ],
)
def test_reveal_solver_structure_and_node_count_digest(fixture, expected_digest):
    if fixture == "seed0_d1":
        game, attacker, depth, include_dag = cs.Game(seed=0), 0, 1, False
    elif fixture == "seed7_d2":
        game, attacker, depth, include_dag = cs.Game(seed=7), 0, 2, False
    elif fixture == "purchase_dag":
        game = cs.Game(seed=0)
        game.board.current_player = 1
        player = game.board.get_player(1)
        player.points = 14
        player.bonuses = [10] * 5
        game.board.set_player(1, player)
        attacker, depth, include_dag = 1, 1, True
    else:
        game = cs.Game(seed=3)
        visible = game.board.visible
        visible[0][0] = -1
        game.board.visible = visible
        game.board.current_player = 1
        attacker, depth, include_dag = 0, 1, False

    result, digest = _solver_payload(
        game, attacker, depth, include_proof_dag=include_dag
    )
    assert digest == expected_digest
    if include_dag:
        assert result["proof_dag"]["complete"] is True
        assert result["proof_dag"]["validated"] is True


def test_deck_reserve_reveal_edges_replay_to_final_round_children():
    game = cs.Game(seed=5)
    game.board.current_player = 1
    player = game.board.get_player(1)
    player.points = 15
    game.board.set_player(1, player)
    action = next(
        candidate
        for candidate in game.legal_actions
        if candidate.type == cs.ActionType.RESERVE_DECK
    )

    result = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=1,
        depth=1,
        required_root_action=int(action.pack()),
        include_proof_dag=True,
        proof_dag_node_limit=5000,
        proof_dag_edge_limit=20000,
    )

    dag = result["proof_dag"]
    root_edges = dag["nodes"][dag["root"]]["children"]
    assert result["proven"] is True
    assert dag["complete"] is True
    assert dag["validated"] is True
    assert result["stats"]["nodes"] == 37
    assert result["stats"]["deck_reserve_candidates"] == 36
    assert result["stats"]["deck_reserve_branches"] == 36
    assert len(root_edges) == 36
    assert {edge["action_code"] for edge in root_edges} == {int(action.pack())}
    assert all(edge["reveal_card"] is not None for edge in root_edges)
    assert all(edge["oracle_card"] is None for edge in root_edges)
    assert all(edge["oracle_reserve"] is False for edge in root_edges)
