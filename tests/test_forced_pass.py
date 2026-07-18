"""Regression coverage for the forced PASS rule transition."""

from __future__ import annotations

import numpy as np
import pytest

import csplendor as cs
from csplendor.api.usi_kifu import (
    action_to_usi,
    find_legal_action_index_by_usi,
    game_to_spn,
    position_to_game,
)

STALEMATE_SEED = 10
STALEMATE_ACTION_CODES = [
    19,
    386,
    498,
    442,
    2208,
    610,
    8390772,
    2088,
    458,
    2592,
    2592,
    33563296,
    17,
    67117704,
    1048844,
    8623497864,
    648,
    8589943304,
    520,
    8589935104,
    252,
    1075841184,
    2176,
    536872960,
    2048,
]


def _forced_pass_game():
    game = cs.Game(seed=STALEMATE_SEED)
    for action_code in STALEMATE_ACTION_CODES:
        assert game.apply_action_code(action_code, False)
    assert not game.is_game_over()
    assert game.current_player == 1
    assert game.requires_forced_pass
    return game


def _board_signature(game):
    board = game.board
    return (
        tuple(map(int, board.bank)),
        tuple(tuple(map(int, level)) for level in board.visible),
        tuple(tuple(map(int, level)) for level in board.decks),
        tuple(map(int, board.nobles)),
        tuple(
            (
                int(player.points),
                tuple(map(int, player.gems)),
                tuple(map(int, player.bonuses)),
                tuple(map(int, player.reserved)),
                tuple(map(bool, player.reserved_is_hidden)),
                tuple(map(int, player.purchased_cards)),
                tuple(map(int, player.acquired_nobles)),
            )
            for player in board.players
        ),
        int(board.current_player),
        int(board.turn),
        bool(board.final_round),
        bool(board.waiting_noble),
        int(board.winner),
        int(game.board_hash()),
    )


def _parallel_config():
    config = cs.MCTSConfig()
    config.use_determinization = False
    config.use_dirichlet_noise = False
    config.forced_playouts = False
    config.num_simulations = 4
    return config


def _parallel_options():
    options = cs.ParallelSearchOptions()
    options.num_threads = 2
    options.batch_size = 2
    options.max_inflight = 4
    options.num_simulations = 4
    options.master_seed = 123
    options.search_nonce = 9
    options.evaluator_version = 404
    return options


def _evaluate(requests):
    results = []
    for request in requests:
        policy = np.asarray(request["valid_actions"], dtype=np.float32)
        if policy.sum():
            policy /= policy.sum()
        results.append(
            {"policy": policy, "value": np.zeros(2, dtype=np.float32)}
        )
    return results


def test_reachable_no_action_state_exposes_one_canonical_pass():
    game = _forced_pass_game()
    actions = game.legal_actions

    assert game.legal_action_count == 1
    assert game.legal_action_codes == [6]
    assert game.legal_action_code_at(0) == 6
    assert game.legal_action_code_at(1) == 0
    assert len(actions) == 1
    assert actions[0].type == cs.ActionType.PASS
    assert int(actions[0].pack()) == 6
    assert repr(actions[0]) == "PASS"
    assert game.is_legal(actions[0])

    unpacked = cs.Action.unpack(6)
    assert unpacked.type == cs.ActionType.PASS
    assert int(unpacked.pack()) == 6
    assert game.is_legal(unpacked)


def test_forced_pass_keeps_48_policy_unchanged_and_uses_full_encoder_sentinels():
    game = _forced_pass_game()
    action = game.legal_actions[0]

    assert cs.ActionEncoderV2.OFFSET_PASS == cs.ActionEncoderV2.ACTION_SIZE - 1
    assert cs.ActionEncoderV3.OFFSET_PASS == cs.ActionEncoderV3.ACTION_SIZE - 1
    assert cs.ActionEncoder().encode(action, game) == -1
    assert cs.ActionEncoderCpp.encode(action, game) == -1
    assert not np.asarray(cs.ActionEncoderCpp.get_action_mask(game)).any()

    for encoder in (cs.ActionEncoderV2, cs.ActionEncoderV3):
        assert encoder.encode(action, game) == encoder.ACTION_SIZE - 1
        active = np.flatnonzero(encoder.get_action_mask(game)).tolist()
        assert active == [encoder.ACTION_SIZE - 1]
        assert encoder.decode(encoder.ACTION_SIZE - 1, game).type == (
            cs.ActionType.PASS
        )
        assert encoder.decode_and_match(
            encoder.ACTION_SIZE - 1, game
        ).type == cs.ActionType.PASS


def test_forced_pass_apply_and_undo_moves_to_opponent_with_an_action():
    game = _forced_pass_game()
    before = _board_signature(game)
    before_turn = game.turn

    assert game.apply_forced_pass(True)
    assert game.current_player == 0
    assert game.turn == before_turn + 1
    assert not game.requires_forced_pass
    assert not game.is_game_over()
    assert any(action.type == cs.ActionType.PURCHASE for action in game.legal_actions)

    assert game.undo()
    assert _board_signature(game) == before
    assert game.requires_forced_pass
    assert not game.undo()


def test_forced_pass_is_rejected_atomically_when_an_ordinary_action_exists():
    game = cs.Game(seed=42)
    before = _board_signature(game)
    pass_action = cs.Action()
    pass_action.type = cs.ActionType.PASS

    assert not game.requires_forced_pass
    assert not game.is_legal(pass_action)
    assert not game.apply_forced_pass(True)
    assert not game.apply(pass_action, True)
    assert _board_signature(game) == before
    assert not game.undo()


def test_forced_pass_resolves_two_sided_stalemate_as_draw():
    game = _forced_pass_game()
    visible = game.board.visible
    decks = game.board.decks
    # Player 0's only ordinary action purchases C12. Swap in an unaffordable
    # same-tier card while keeping the editor state a complete card partition.
    visible[0][0], decks[0][decks[0].index(22)] = (
        22,
        visible[0][0],
    )
    game.board.visible = visible
    game.board.decks = decks

    opponent = game.clone_light()
    opponent.board.current_player = 0
    assert opponent.requires_forced_pass
    assert game.requires_forced_pass

    assert game.apply_forced_pass(True)
    assert game.is_game_over()
    assert game.winner == -2
    assert game.legal_actions == []
    assert game.legal_action_count == 0
    assert game.undo()
    assert game.requires_forced_pass
    assert game.winner == -1


def test_usi_pass_round_trip_and_position_replay():
    game = _forced_pass_game()
    action = game.legal_actions[0]

    assert action_to_usi(action, game=game) == "pass"
    assert find_legal_action_index_by_usi(game, "pass") == 0
    assert find_legal_action_index_by_usi(game, " PASS ") == 0
    with pytest.raises(ValueError, match="pass is not legal"):
        find_legal_action_index_by_usi(cs.Game(seed=42), "pass")

    spn = game_to_spn(game, reveal_hidden_reserved_ids=True)
    replayed = position_to_game(f"position {spn} moves pass")
    assert replayed.current_player == 0
    assert not replayed.requires_forced_pass
    assert any(
        candidate.type == cs.ActionType.PURCHASE
        for candidate in replayed.legal_actions
    )


def test_mcts_roots_require_caller_to_apply_forced_pass_before_searching():
    game = _forced_pass_game()
    config = _parallel_config()
    mcts = cs.MCTS(config)
    callback_calls = 0

    def evaluator(requests):
        nonlocal callback_calls
        callback_calls += 1
        return _evaluate(requests)

    with pytest.raises(ValueError, match="apply it before searching"):
        mcts.prepare_batch_simulations(game, 1, 1, 1, None)
    with pytest.raises(ValueError, match="apply it before searching"):
        cs.mcts_search_parallel_native(
            mcts, game, _parallel_options(), evaluator, 1.0
        )
    assert callback_calls == 0
    assert not mcts.is_parallel_search_active()

    root_options = _parallel_options()
    root_options.mode = cs.ParallelSearchMode.ROOT_PARALLEL
    with pytest.raises(ValueError, match="apply it before searching"):
        cs.mcts_search_root_parallel_native(
            config, game, 4, 2, root_options, evaluator, 1.0
        )
    assert callback_calls == 0

    assert game.apply_forced_pass(False)
    result = cs.mcts_search_parallel_native(
        mcts, game, _parallel_options(), evaluator, 1.0
    )
    assert result.stop_reason == cs.ParallelSearchStopReason.COMPLETED
    assert result.ledger.completed == 4


def test_rejected_forced_pass_root_does_not_consume_automatic_replay_nonce():
    game = _forced_pass_game()
    mcts = cs.MCTS(_parallel_config())
    mcts.reset_replay_sequence(77, 51)
    options = _parallel_options()
    options.master_seed = None
    options.search_nonce = (1 << 64) - 1

    with pytest.raises(ValueError, match="apply it before searching"):
        cs.mcts_search_parallel_native(mcts, game, options, _evaluate, 1.0)
    assert not mcts.is_parallel_search_active()

    assert game.apply_forced_pass(False)
    result = cs.mcts_search_parallel_native(mcts, game, options, _evaluate, 1.0)
    assert result.resolved_seed == 77
    assert result.search_nonce == 51
