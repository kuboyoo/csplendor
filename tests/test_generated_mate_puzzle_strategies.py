import json
from collections import deque
from pathlib import Path

import pytest

import csplendor as cs
from csplendor.api.usi_kifu import spn_to_game
from scripts.dfpn_mate_solver import compact_proof_dag_to_v1


GENERATED_MATE_PUZZLES2 = Path(__file__).resolve().parents[1] / "generated" / "mate_puzzles2"
REPRESENTATIVE_PUZZLE_IDS = (
    "6e5729c524acbd3b",
)


def _node_by_id(strategy):
    dag = strategy["strategy_dag"]
    if dag.get("format") == "strategy_dag_compact_v1":
        dag = compact_proof_dag_to_v1(dag)
    return {int(node["id"]): node for node in dag["nodes"]}


def _visible_refill_level_and_slot(game, action):
    if int(action.type) == int(cs.ActionType.RESERVE_VISIBLE):
        target = int(action.card_id)
    elif int(action.type) == int(cs.ActionType.PURCHASE) and not bool(action.from_reserved):
        target = int(action.card_id)
    else:
        return None
    for level, row in enumerate(game.board.visible):
        for slot, card_id in enumerate(row):
            if int(card_id) == target:
                return level, slot
    return None


def _put_reveal_on_deck_top(game, level, card_id):
    decks = [list(deck) for deck in game.board.decks]
    deck = decks[level]
    if card_id not in [int(value) for value in deck]:
        raise AssertionError(f"reveal card C{card_id} is not in level {level + 1} deck")
    deck.remove(card_id)
    deck.append(card_id)
    game.board.decks = decks


def _apply_strategy_edge(game, edge):
    assert edge.get("oracle_card") is None
    assert edge.get("oracle_reserve") is False
    assert edge.get("oracle_reserve_card") is None

    action_code = int(edge["action_code"])
    legal_codes = {int(code) for code in game.legal_action_codes}
    assert action_code in legal_codes

    action = cs.Action.unpack(action_code)
    reveal_card = edge.get("reveal_card")
    if reveal_card is not None:
        reveal_card = int(reveal_card)
        if int(action.type) == int(cs.ActionType.RESERVE_DECK):
            level = int(action.deck_level)
            _put_reveal_on_deck_top(game, level, reveal_card)
        else:
            refill = _visible_refill_level_and_slot(game, action)
            assert refill is not None
            level, _slot = refill
            _put_reveal_on_deck_top(game, level, reveal_card)

    assert game.apply_action_code_trusted(action_code, False)


def _assert_node_metadata_matches_game(node, game):
    assert int(node["player"]) == int(game.board.current_player)
    if "scores" not in node:
        return
    assert [int(v) for v in node["scores"]] == [int(v) for v in game.scores]
    assert int(node["winner"]) == int(game.board.winner)
    assert bool(node["waiting_noble"]) == bool(game.board.waiting_noble)
    assert [int(v) for v in node["nobles"]] == [int(v) for v in game.board.nobles]
    assert node["acquired_nobles"] == [
        [int(v) for v in game.board.players[0].acquired_nobles],
        [int(v) for v in game.board.players[1].acquired_nobles],
    ]


@pytest.mark.skipif(
    not GENERATED_MATE_PUZZLES2.exists(),
    reason="generated/mate_puzzles2 is not present",
)
def test_generated_mate_puzzle_strategy_dags_contain_only_legal_actions():
    strategy_paths = [
        path
        for puzzle_id in REPRESENTATIVE_PUZZLE_IDS
        for path in GENERATED_MATE_PUZZLES2.glob(f"depth_*/{puzzle_id}/strategy.json")
    ]
    if not strategy_paths:
        pytest.skip("representative generated strategy.json files are not present")

    for strategy_path in strategy_paths:
        strategy = json.loads(strategy_path.read_text())
        assert strategy["strategy_dag"]["complete"] is True, strategy_path
        nodes = _node_by_id(strategy)
        root = int(strategy["strategy_dag"]["root"])
        games = {root: spn_to_game(strategy["position"])}
        queue = deque([root])
        visited = set()

        while queue:
            node_id = queue.popleft()
            if node_id in visited:
                continue
            visited.add(node_id)
            node = nodes[node_id]
            game = games[node_id]
            _assert_node_metadata_matches_game(node, game)

            for edge in node.get("children", []):
                child_id = int(edge["child"])
                child_game = game.clone()
                _apply_strategy_edge(child_game, edge)
                if child_id not in games:
                    games[child_id] = child_game
                queue.append(child_id)

        assert visited == set(nodes), strategy_path
