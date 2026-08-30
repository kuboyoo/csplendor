import random

import csplendor as cs
import scripts.puzzle_engine_adapter as puzzle_engine_adapter


def test_resolve_alphazero_path_accepts_environment_override(monkeypatch, tmp_path):
    legacy_root = tmp_path / "alphazero-general-ori"
    package = legacy_root / "splendor"
    package.mkdir(parents=True)
    (package / "SplendorGame.py").touch()
    (package / "SplendorNNet.py").touch()
    monkeypatch.setenv("ALPHAZERO_ORI_PATH", str(legacy_root))

    assert puzzle_engine_adapter._resolve_alphazero_path() == legacy_root.resolve()


def test_genbu_player_maps_stable_action_id_without_duplicate_search(monkeypatch):
    game = cs.Game(seed=0)
    expected = list(game.legal_actions)[3]

    class FakeEncoder:
        @staticmethod
        def encode(action, _game):
            return int(action.pack()) + 100000

    class FakeMCTS:
        action_encoder = FakeEncoder()

        def __init__(self):
            self.calls = 0

        def search(self, current, **_kwargs):
            self.calls += 1
            action_id = self.action_encoder.encode(expected, current)
            return action_id, {"visit_counts": {action_id: 1}}

    class FakeAdapter:
        def __init__(self):
            self._mcts = None

    adapter = FakeAdapter()
    mcts = FakeMCTS()
    monkeypatch.setattr(
        puzzle_engine_adapter,
        "_create_genbu_adapter",
        lambda _weights_path: adapter,
    )
    monkeypatch.setattr(
        puzzle_engine_adapter,
        "_create_compatible_genbu_mcts",
        lambda *_args, **_kwargs: mcts,
    )
    player = puzzle_engine_adapter.GenbuPuzzlePlayer(
        puzzle_engine_adapter.DEFAULT_GENBU_WEIGHTS,
        time_limit=0.1,
        num_simulations=5,
        rng=random.Random(0),
        best_action_rate=1.0,
        top_action_rate=0.0,
        top_action_count=4,
    )

    selected = player.select_action(game)

    assert int(selected.pack()) == int(expected.pack())
    assert mcts.calls == 1
