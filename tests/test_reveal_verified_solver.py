import csplendor as cs


def test_oracle_reserve_is_not_generated_when_reserve_slots_are_full():
    game = cs.Game(seed=0)
    visible = game.board.visible
    visible[0][0] = -1
    game.board.visible = visible
    player = game.board.get_player(1)
    player.reserved = [0, 1, 2]
    game.board.set_player(1, player)
    game.board.current_player = 1

    result = cs.solve_reveal_verified_mate_cpp(game, attacker=0, depth=0)

    assert result["stats"]["oracle_reserve_actions"] == 0
