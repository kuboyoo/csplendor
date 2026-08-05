from tests.support import game_state_signature, random_reachable_state_corpus


def test_random_reachable_state_corpus_is_reproducible_and_independent():
    first = list(
        random_reachable_state_corpus(master_seed=17, games=3, max_plies=8)
    )
    second = list(
        random_reachable_state_corpus(master_seed=17, games=3, max_plies=8)
    )

    assert [game_state_signature(game) for game in first] == [
        game_state_signature(game) for game in second
    ]
    assert len(first) > 3

    original = game_state_signature(first[1])
    first[0].board.bank = [0, 0, 0, 0, 0, 0]
    assert game_state_signature(first[1]) == original
