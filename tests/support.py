"""Shared deterministic fixtures and value signatures for contract tests."""

import random

from csplendor import Game


def action_signature(action):
    """Return every semantically relevant Action field as immutable values."""
    return (
        int(action.type),
        tuple(map(int, action.take)),
        int(action.card_id),
        int(action.deck_level),
        bool(action.from_reserved),
        tuple(map(int, action.gold_as)),
        tuple(map(int, action.return_gems)),
        int(action.noble_choice),
    )


def player_state_signature(player):
    """Return canonical, derived, and provenance player state."""
    return (
        int(player.points),
        tuple(map(int, player.gems)),
        int(player.packed_gems),
        tuple(map(int, player.bonuses)),
        int(player.packed_bonuses),
        int(player.reserved_count),
        tuple(map(int, player.reserved)),
        tuple(map(bool, player.reserved_is_hidden)),
        int(player.purchased_count),
        tuple(map(int, player.purchased_cards)),
        tuple(map(int, player.acquired_nobles)),
    )


def game_state_signature(game, *, include_hash=False):
    """Return a complete lightweight Game value signature.

    Undo history is deliberately excluded because ``clone_light`` and snapshot
    contracts exclude it.  ``include_hash`` is useful when testing invalidation;
    otherwise callers compare state without warming the cache as a side effect.
    """
    board = game.board
    signature = (
        tuple(map(int, board.bank)),
        tuple(tuple(map(int, level)) for level in board.visible),
        tuple(tuple(map(int, level)) for level in board.decks),
        tuple(map(int, board.nobles)),
        tuple(player_state_signature(board.get_player(i)) for i in range(2)),
        int(board.current_player),
        int(board.turn),
        bool(board.final_round),
        bool(board.waiting_noble),
        int(board.winner),
        bool(game.simple_payment_mode),
        bool(game.blank_refill_mode),
    )
    if include_hash:
        return signature + (int(game.board_hash()),)
    return signature


def set_current_player(game, *, gems=None, bonuses=None, reserved=None):
    """Edit the current player through the public copy/write-back API."""
    index = game.current_player
    player = game.board.get_player(index)
    if gems is not None:
        player.gems = list(gems)
    if bonuses is not None:
        player.bonuses = list(bonuses)
    if reserved is not None:
        player.reserved = list(reserved)
    game.board.set_player(index, player)


def reachable_state_corpus(seeds=(0, 1, 42, 123), plies=5):
    """Yield deterministic reachable lightweight states, including each root."""
    for seed in seeds:
        game = Game(seed=seed)
        yield game.clone_light()
        for ply in range(plies):
            codes = game.legal_action_codes
            if not codes or game.is_game_over():
                break
            assert game.apply_action_code_trusted(codes[ply % len(codes)], False)
            yield game.clone_light()


def random_reachable_state_corpus(*, master_seed=0, games=8, max_plies=32):
    """Yield a reproducible random reachable-state corpus for differential tests."""
    rng = random.Random(master_seed)
    for seed in range(games):
        game = Game(seed=seed)
        yield game.clone_light()
        for _ in range(max_plies):
            codes = game.legal_action_codes
            if not codes or game.is_game_over():
                break
            assert game.apply_action_code_trusted(codes[rng.randrange(len(codes))], False)
            yield game.clone_light()


def assert_differential_corpus(cases, reference, candidate, *, label="case"):
    """Assert two pure projections agree for every value in a corpus.

    The helper deliberately reports the corpus index so a randomized property
    failure remains reproducible from its fixed master seed.
    """

    checked = 0
    for index, case in enumerate(cases):
        expected = reference(case)
        actual = candidate(case)
        assert actual == expected, (
            f"{label} {index} differs: expected={expected!r}, actual={actual!r}"
        )
        checked += 1
    assert checked > 0, f"{label} corpus is empty"
