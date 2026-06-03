import csplendor as cs
from scripts.mate_solver import load_game_from_usi_text


BENCH_POSITION = (
    "position bank:W1U3G3R3K0D4 | "
    "visible:L1[35,33,20,24]L2[46,61,51,66]L3[80,86,87,88] | "
    "decks:36,23,15 | nobles:[1,10,6] | "
    "P0:name:Player0;gems:W3U1G1R1K2D0;bonuses:W2U2G1R3K3;points:5;"
    "nobles:[-,-,-];reserved:[68];bought:[_,_,_,_,_,_,_,_,_,_,_] | "
    "P1:name:Player1;gems:W0U0G0R0K2D1;bonuses:W3U1G0R0K3;points:8;"
    "nobles:[-,-,-];reserved:[85,44,43];bought:[_,_,_,_,_,_,_] | 0"
)


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


def test_oracle_reserve_proof_edges_keep_concrete_hidden_card_ids():
    result = cs.solve_reveal_verified_mate_cpp(
        load_game_from_usi_text(BENCH_POSITION),
        attacker=0,
        depth=5,
        time_limit_seconds=30.0,
        include_proof_dag=True,
        proof_dag_node_limit=200000,
    )

    dag = result["proof_dag"]
    oracle_reserves = [
        edge
        for node in dag["nodes"]
        for edge in node["children"]
        if edge["oracle_reserve"]
    ]
    assert result["proven"] is True
    assert dag["complete"] is True
    assert all("scores" in node for node in dag["nodes"])
    assert all("winner" in node for node in dag["nodes"])
    assert oracle_reserves
    assert all(edge["oracle_reserve_card"] is not None for edge in oracle_reserves)
