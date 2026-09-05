#!/usr/bin/env python3
"""R0-only V3 slice, using the existing encoding benchmark's fixture/timer."""
import hashlib
import json
import statistics
import sys
from pathlib import Path

import csplendor as cs
import csplendor._csplendor as native
import numpy as np

sys.path.insert(0, '/tmp/csplendor-codex56-phase0/scripts')
from benchmark_encoding import _midgame, _rate


def main():
    game = _midgame()
    actions = game.legal_actions
    ids = [cs.ActionEncoderV3.encode(action, game) for action in actions]
    mask = np.asarray(cs.ActionEncoderV3.get_action_mask(game), dtype=bool)
    assert cs.ActionEncoderV3.ACTION_SIZE == 3133
    assert set(ids) == set(np.flatnonzero(mask))
    for action_id in set(ids):
        decoded = cs.ActionEncoderV3.decode_and_match(action_id, game)
        assert cs.ActionEncoderV3.encode(decoded, game) == action_id
    purchase = next(action for action in actions if action.type == cs.ActionType.PURCHASE)
    workloads = {
        'v3_mask_per_second': lambda: cs.ActionEncoderV3.get_action_mask(game),
        'v3_purchase_encode_per_second': lambda: cs.ActionEncoderV3.encode(purchase, game),
        'v3_legal_ids_via_actions_per_second': lambda: [
            cs.ActionEncoderV3.encode(action, game) for action in game.legal_actions],
        'native_48_mask_per_second': lambda: cs.ActionEncoderCpp.get_action_mask(game),
    }
    for _ in range(500):
        for workload in workloads.values():
            workload()
    rates = {name: [] for name in workloads}
    for _ in range(5):
        for name, workload in workloads.items():
            rates[name].append(_rate(workload, 5000))
    print(json.dumps({
        'schema': 'csplendor.r0.encoding_slice.v1', 'python': sys.version,
        'module': native.__file__, 'module_sha256': hashlib.sha256(
            Path(native.__file__).read_bytes()).hexdigest(),
        'fixture': 'existing benchmark_encoding._midgame, seed42, 16 legal plies',
        'legal_action_count': len(actions), 'unique_v3_ids': len(set(ids)),
        'v3_ids_digest': hashlib.sha256(json.dumps(ids).encode()).hexdigest(),
        'correct': True, 'samples': 5, 'iterations': 5000, 'rates': rates,
        'medians': {name: statistics.median(values) for name, values in rates.items()},
    }, indent=2))


if __name__ == '__main__':
    main()
