"""Final-review regressions for solver-specific rule transitions."""

import pytest

import csplendor as cs


@pytest.mark.parametrize("current_player", [0, 1])
def test_reveal_deck_reserve_keeps_preexisting_noble_choice(current_player):
    game = cs.Game(seed=5)
    game.board.current_player = current_player
    player = game.board.get_player(current_player)
    player.bonuses = [10] * 5
    game.board.set_player(current_player, player)
    action = next(
        candidate
        for candidate in game.legal_actions
        if candidate.type == cs.ActionType.RESERVE_DECK
    )

    direct = game.clone_light()
    assert direct.apply_trusted(action, False)
    assert direct.board.waiting_noble is True
    assert direct.current_player == current_player

    # The reveal solver injects each possible deck card itself. That custom
    # transition must still run Game's ordinary noble/final-round processing.
    result = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=current_player,
        depth=1,
        required_root_action=int(action.pack()),
    )
    assert result["stats"]["deck_reserve_branches"] == 1
    assert result["stats"]["nodes"] == 8
