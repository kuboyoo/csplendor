#!/usr/bin/env python3
"""Bounded acceptance using existing consumer APIs, never a speed benchmark."""
import argparse
import copy
import dataclasses
import gc
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import time

import numpy as np
import csplendor as cs
from csplendor import _csplendor as core

ROOT = Path(__file__).resolve().parents[2]
DL = ROOT.parent / 'dlsplendor'
GUI = ROOT.parent / 'splendorgui'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def identity():
    manifest = json.loads((ROOT / 'doc/performance_experiments/final_main_vs_candidate_manifest_20260906.json').read_text())
    expected = manifest['python_binary_identity']['B']
    path = Path(core.__file__).resolve()
    assert path == Path(expected['path']).resolve()
    assert sha(path) == expected['sha256']
    return {'extension': str(path), 'sha256': sha(path), 'python': sys.executable,
            'numpy': np.__version__, 'build_evidence': 'final_main_vs_candidate_manifest_20260906.json:python_binary_identity.B',
            'flags': expected['flags'], 'link': expected['link']}


def describe_array(array):
    return {'dtype': str(array.dtype), 'shape': list(array.shape),
            'c_contiguous': bool(array.flags.c_contiguous),
            'owns_data': bool(array.flags.owndata), 'base_is_none': array.base is None,
            'sha256': hashlib.sha256(array.tobytes()).hexdigest()}


def model(family='selfplay12'):
    import torch
    from dlsplendor.network.action_encoder import ActionEncoder
    from dlsplendor.search.mcts import MCTS

    torch.manual_seed(42)
    worker = load('f2_gui_worker', GUI / 'scripts/dlsplendor_gui_engine.py')
    # The GUI registry supersedes its stale README: selfplay17 is recommended;
    # selfplay12 remains a supported legacy opponent. No model is copied.
    configuration = {'selfplay12': 'selfplay12', 'selfplay17': 'selfplay16_exact_mate'}[family]
    checkpoint = DL / 'models' / family / 'best.pt'
    config_path = DL / 'configs' / (configuration + '.yaml')
    model_id = family + '-best'
    checkpoint_sha = sha(checkpoint)
    started = time.perf_counter()
    loaded, load_ms = worker._load_model(model_id, str(checkpoint), str(config_path))
    search_config = copy.deepcopy(loaded.config.search)
    search_config.playout_cap_randomization = False  # Existing GUI override.
    mcts = MCTS(loaded.network, loaded.encoder, search_config, seed=42)
    game = cs.Game(seed=42)
    game.simple_payment_mode = loaded.config.game.simple_payment_mode
    rows = []
    for ply in range(2):
        snapshot = game.serialize_snapshot()
        restored = cs.Game.deserialize_snapshot(snapshot)
        assert restored.serialize_snapshot() == snapshot
        tic = time.perf_counter()
        encoded = loaded.encoder.encode(restored, restored.current_player)
        held = cs.StateFeaturizer().featurize(restored, restored.current_player)
        held_bytes = held.tobytes()
        assert held.flags.owndata and held.base is None
        assert encoded.dtype == np.float32 and encoded.flags.c_contiguous
        assert encoded.shape == (loaded.encoder.get_state_dim(),)
        assert encoded.flags.owndata and encoded.base is None
        encoded_bytes = encoded.tobytes()
        retained_tensor = torch.from_numpy(encoded)
        with torch.inference_mode():
            policy, value = loaded.network(retained_tensor.unsqueeze(0).to(worker.DEVICE))
            assert policy.shape == (1, ActionEncoder.action_dim)
            assert torch.isfinite(policy).all() and torch.isfinite(value).all()
            assert torch.allclose(policy.sum(dim=-1), torch.ones(1, device=worker.DEVICE), atol=1e-5)
            action_id, info = mcts.search(restored, num_simulations=400, add_root_noise=False)
        action = ActionEncoder().decode(action_id, restored)
        assert action is not None and restored.is_legal(action)
        before = restored.clone_light()
        assert restored.apply(action, False)
        mcts.observe_transition(before, action_id, restored)
        # Consumer and native owning arrays survive native search and mutation.
        assert held.tobytes() == held_bytes and encoded.tobytes() == encoded_bytes
        assert np.array_equal(retained_tensor.numpy(), encoded)
        rows.append({'ply': ply, 'input_snapshot_hex': snapshot.hex(),
                     'output_snapshot_hex': restored.serialize_snapshot().hex(),
                     'features': describe_array(encoded), 'native_features': describe_array(held),
                     'action_id': int(action_id), 'action_code': int(action.pack()),
                     'value': value.detach().cpu().tolist(),
                     'elapsed_ms': (time.perf_counter()-tic)*1000,
                     'search': {k: info.get(k) for k in ['simulations', 'requested_simulations',
                                'tree_reused', 'reused_visits', 'mate_search_attempted',
                                'mate_proven', 'chance_nodes']}})
        game = cs.Game.deserialize_snapshot(restored.serialize_snapshot())
        del restored, before
    del game, mcts
    gc.collect()
    assert held.tobytes() == held_bytes and retained_tensor.numpy().tobytes() == encoded_bytes

    # Actual GUI session handlers, with two legal AI moves and tree observation.
    session_payload = worker._handle('new_game', {
        'mode': 'ai-vs-ai', 'human_seat': None, 'simulations': 16, 'seed': 42,
        'player_kinds': ['checkpoint', 'checkpoint'],
        'model_ids': [model_id] * 2,
        'model_paths': [str(checkpoint)] * 2,
        'model_config_paths': [str(config_path)] * 2})
    session_id = session_payload['session_id']
    moves = [worker._handle('ai_action', {'session_id': session_id})['ai_move'] for _ in range(2)]
    assert len(worker.SESSIONS[session_id].moves) == 2
    assert worker._handle('delete_game', {'session_id': session_id})['deleted']

    # Activate the existing consumer mate oracle only in this local instance.
    # Real loaded NN remains available; no fake solver/network substitutes.
    config = copy.deepcopy(search_config)
    config.mate_search_enabled = True
    config.mate_search_min_points = 9
    config.mate_search_min_depth = 1
    config.mate_search_max_depth = 3
    config.mate_search_max_nodes = 20000
    # Keep the deployed 20 ms limit for the recommended selfplay17; this
    # tiny synthetic terminal fixture does not require a larger budget.
    config.mate_search_time_limit_ms = 20 if family == 'selfplay17' else 1000
    mate_mcts = MCTS(loaded.network, loaded.encoder, config, seed=42)
    fixture = load('f2_gui_mate_fixture', GUI / 'scripts/test_csplendor_mate_engine.py')
    from csplendor.mate_frontier import load_mate_frontier_game
    mate_game = load_mate_frontier_game(position=fixture._one_turn_mate_position())
    with torch.inference_mode():
        action_id, info = mate_mcts.search(mate_game, num_simulations=16, add_root_noise=False)
    assert info['mate_proven'] and info['simulations'] == 0
    action = ActionEncoder().decode(action_id, mate_game)
    assert action is not None and mate_game.is_legal(action)
    session = mate_mcts._mate_session(mate_game.current_player)
    assert mate_mcts._mate_session(mate_game.current_player) is session
    warm = session.search_anytime(mate_game, min_depth=1, max_depth=3,
                                max_nodes=20000, time_limit_seconds=1)
    assert warm['status'] == 'mate'
    mate_mcts.clear()
    assert sha(checkpoint) == checkpoint_sha
    return {'model': str(checkpoint), 'model_sha256': checkpoint_sha,
            'config': str(config_path), 'config_sha256': sha(config_path),
            'model_load_ms': load_ms, 'total_ms': (time.perf_counter()-started)*1000,
            'device': str(worker.DEVICE), 'precision': str(next(loaded.network.parameters()).dtype),
            'torch': torch.__version__, 'torch_threads': torch.get_num_threads(),
            'backend': MCTS.__module__ + '.' + MCTS.__name__ + ' (Python PUCT; not native legacy/shared/root)',
            'action_space': 'V3', 'action_dim': ActionEncoder.action_dim,
            'payment_mode': loaded.config.game.simple_payment_mode,
            'effective_search_config': dataclasses.asdict(search_config),
            'rows': rows, 'gui_handler_moves': moves,
            'mate_override': {'config': {k: v for k, v in dataclasses.asdict(config).items() if k.startswith('mate_')},
                              'action': action_id, 'proven': info['mate_proven'],
                              'depth': info['mate_depth'], 'warm_stats': warm['stats']},
            'owning_arrays_survive_search_and_destruction': True,
            'scope': 'Read-only current consumer working tree; uncommitted consumer edits are not integration inputs. No performance ratio or browser rendering claim.'}


def frontier():
    # Test the actual GUI JSON-lines worker, not a replacement transport.
    import subprocess
    import os
    import selectors
    worker = subprocess.Popen([sys.executable, '-B', str(GUI / 'scripts/csplendor_mate_engine.py')],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, cwd=ROOT, env=os.environ.copy())
    selector = selectors.DefaultSelector()
    selector.register(worker.stdout, selectors.EVENT_READ)
    counter = 0
    def request(command, payload):
        nonlocal counter
        counter += 1
        worker.stdin.write(json.dumps({'request_id': counter, 'command': command, 'payload': payload})+'\n')
        worker.stdin.flush()
        assert selector.select(timeout=30), 'GUI worker response timeout'
        response = json.loads(worker.stdout.readline())
        assert response['request_id'] == counter and response['ok'], response
        return response['result']
    records = []
    try:
        assert request('ping', {})['protocol_version'] == 1
        mapped = {line.split()[-1] for line in Path(f'/proc/{worker.pid}/maps').read_text().splitlines()
                  if '/_csplendor' in line and '.so' in line}
        assert mapped == {str(Path(core.__file__).resolve())}
        worker_identity = {p: sha(p) for p in mapped}
        fixtures = load('f2_engine_fixtures', ROOT / 'tests/test_reveal_verified_solver.py')
        for depth, factory in [(5, fixtures._five_move_mate_fixture),
                               (7, fixtures._seven_move_mate_fixture)]:
            game = factory()
            remaining = depth
            layers = []
            for _ in range(depth * 2 + 2):
                state = cs.encode_mate_frontier_state(game)
                payload = {'state': state, 'attacker': 0, 'depth': remaining,
                           'max_nodes': 200000, 'time_limit_seconds': 5, 'edge_limit': 10000}
                result = request('expand_frontier', payload)
                assert result['proven'] and result['complete']
                assert request('expand_frontier', payload) == result
                if game.is_game_over():
                    break
                if game.current_player != 0:
                    assert {int(x['action_code']) for x in result['edges']} == set(game.legal_action_codes)
                edge = result['edges'][0]
                # Persist only a tiny verification fixture in the new doc raw.
                # JSON serialization/deserialization is also the GUI boundary.
                reloaded = json.loads(json.dumps(edge))
                game = cs.decode_mate_frontier_state(reloaded['child_state'])
                remaining = int(reloaded['child_depth'])
                layers.append({'edge_count': len(result['edges']), 'chosen_edge': reloaded})
            assert game.is_game_over() and game.winner == 0
            records.append({'depth': depth, 'layers': layers, 'winner': int(game.winner)})
        cache = request('ping', {})
    finally:
        worker.stdin.close()
        try:
            worker.wait(timeout=5)
        except subprocess.TimeoutExpired:
            worker.terminate()
            worker.wait(timeout=5)
        selector.close()
    assert worker.returncode == 0
    return {'protocol': 'actual splendorgui JSON-lines worker', 'records': records,
            'loaded_worker_extension': worker_identity,
            'cache': cache, 'browser_rendering': 'NOT_EXECUTED',
            'stderr': worker.stderr.read()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['identity', 'model', 'frontier'])
    parser.add_argument('--family', choices=['selfplay12', 'selfplay17'], default='selfplay12')
    args = parser.parse_args()
    mode = args.mode
    result = {'identity': identity()}
    # Emit identity before any consumer import or execution failure.
    print(json.dumps({'loaded_identity': result['identity']}, sort_keys=True), flush=True)
    if mode != 'identity':
        result[mode] = model(args.family) if mode == 'model' else frontier()
    print(json.dumps(result, sort_keys=True, ensure_ascii=False), flush=True)
