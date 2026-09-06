#!/usr/bin/env python3
"""Exercise the normally installed wheel outside all source worktrees."""
import argparse
import ast
import gc
import hashlib
from importlib import metadata
import json
from pathlib import Path
import sys
import zipfile

import numpy as np
import csplendor as cs
from csplendor import _csplendor as core


def digest(data):
    return hashlib.sha256(data).hexdigest()


def identity(wheel):
    prefix = Path(sys.prefix).resolve()
    assert prefix != Path(sys.base_prefix).resolve()
    assert sys.flags.isolated
    assert Path(cs.__file__).resolve().is_relative_to(prefix)
    extension = Path(core.__file__).resolve()
    assert extension.is_relative_to(prefix)
    assert not any('workspace/repos' in path for path in sys.path)
    dist = metadata.distribution('csplendor')
    assert cs.__version__ == dist.version == '0.1.0'
    direct = json.loads(dist.read_text('direct_url.json'))
    assert 'archive_info' in direct and not direct.get('dir_info', {}).get('editable')
    assert direct['archive_info']['hashes']['sha256'] == digest(wheel.read_bytes())
    checked = {}
    with zipfile.ZipFile(wheel) as archive:
        for name in archive.namelist():
            if name.startswith('csplendor/') and name.endswith(('.py', '.so')):
                installed = Path(dist.locate_file(name)).resolve()
                assert installed.is_relative_to(prefix)
                assert installed.read_bytes() == archive.read(name), name
                checked[name] = digest(installed.read_bytes())
    return {'python': sys.version, 'executable': sys.executable, 'prefix': str(prefix),
            'cwd': str(Path.cwd()), 'sys_path': sys.path, 'package': cs.__file__,
            'extension': str(extension), 'extension_sha256': digest(extension.read_bytes()),
            'wheel': str(wheel), 'wheel_sha256': digest(wheel.read_bytes()),
            'wheel_bytes': wheel.stat().st_size, 'installed_files': checked,
            'versions': {p: metadata.version(p) for p in ['csplendor', 'numpy', 'pip']}}


def smoke():
    rows = []
    for simple in [False, True]:
        game = cs.Game(seed=42)
        game.simple_payment_mode = simple
        snapshot = game.serialize_snapshot()
        restored = cs.Game.deserialize_snapshot(snapshot)
        assert restored.serialize_snapshot() == snapshot
        assert restored.legal_action_count == len(restored.legal_actions) > 0
        mask = np.asarray(cs.ActionEncoderV3.get_action_mask(restored))
        assert mask.shape == (3133,)
        ids = []
        for action in restored.legal_actions:
            index = cs.ActionEncoderV3.encode(action, restored)
            assert mask[index]
            matched = cs.ActionEncoderV3.decode_and_match(index, restored)
            assert cs.ActionEncoderV3.encode(matched, restored) == index
            ids.append(index)
        assert int(mask.sum()) == len(set(ids))
        held = cs.StateFeaturizer().featurize(restored)
        expected = held.tobytes()
        assert held.dtype == np.float32 and held.flags.owndata and held.flags.c_contiguous
        assert held.base is None
        assert expected == np.asarray(cs.StateEncoder.encode(restored, -1), dtype=np.float32).tobytes()
        assert restored.apply_action_code(int(restored.legal_action_codes[0]), False)
        assert restored.serialize_snapshot() != snapshot
        token = cs.encode_mate_frontier_state(restored)
        saved = Path.cwd() / ('snapshot-' + str(simple) + '.json')
        with saved.open('x') as stream:
            json.dump({'state': token}, stream)
        reloaded = cs.decode_mate_frontier_state(json.loads(saved.read_text())['state'])
        assert restored.serialize_snapshot() == reloaded.serialize_snapshot()
        del game, restored, reloaded
        gc.collect()
        assert held.tobytes() == expected
        rows.append({'simple_payment_mode': simple, 'legal_v3_ids': len(set(ids)),
                     'features': held.size, 'snapshot_roundtrip': True, 'owning_numpy': True})
    return rows


def existing_cases(source, source_hashes):
    # Read only AST-selected, undecorated existing fixtures/tests. Never put the
    # source tree on sys.path or import its csplendor/tests/scripts packages.
    selected = {
        'test_reveal_verified_solver.py': [
            '_six_move_mate_fixture', '_seven_move_mate_fixture', '_five_move_mate_fixture',
            'test_reveal_verified_solver_proves_seven_move_mate_at_exact_depth',
            'test_exact_depth_search_honors_external_cancellation',
            'test_mate_search_session_reuses_position_after_opponent_response',
            'test_mate_search_session_resumes_incomplete_same_depth_search',
            'test_json_mate_frontier_state_round_trip_preserves_phase_fields',
            'test_lazy_frontier_reaches_terminal_for_five_and_seven_move_mates'],
        'test_state_feature_numpy.py': [
            'test_retained_featurizer_array_survives_search_and_game_destruction']}
    namespace = {'cs': cs, 'core': core, 'np': np}
    passed = []
    for filename, names in selected.items():
        path = source / 'tests' / filename
        data = path.read_bytes()
        assert digest(data) == source_hashes[filename]
        nodes = [node for node in ast.parse(data).body
                 if isinstance(node, ast.FunctionDef) and node.name in names]
        assert [node.name for node in nodes] == names
        assert all(not node.decorator_list for node in nodes)
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), 'exec'), namespace)
        for name in names:
            if name.startswith('test_'):
                namespace[name]()
                passed.append(name)
    return {'passed': passed, 'fixture_file_sha256': source_hashes,
            'MCTS': 'legacy 48-action, 8 simulations, batch=1, synthetic evaluator, no NN',
            'solver': 'existing 5/7 frontier, exact 7, session/cancel/resume; original asserts/budgets unchanged'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--wheel', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--solver-sha', required=True)
    parser.add_argument('--numpy-sha', required=True)
    args = parser.parse_args()
    result = {'identity': identity(args.wheel), 'smoke': smoke()}
    result['existing_cases'] = existing_cases(args.source, {
        'test_reveal_verified_solver.py': args.solver_sha,
        'test_state_feature_numpy.py': args.numpy_sha})
    result['status'] = 'PASS'
    print(json.dumps(result, sort_keys=True, indent=2))
