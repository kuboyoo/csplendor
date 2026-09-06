#!/usr/bin/env python3
"""Explicit extension identity; training uses no holdout fixtures."""
import importlib.util
import sys
from pathlib import Path
import numpy as np

extension = Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location('_csplendor', extension)
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
assert Path(c.__file__).resolve() == extension
game = c.Game(17)
for i in range(500):
    for observer in (-1, 0, 1):
        a = np.asarray(c.StateEncoder.encode(game, observer), dtype=np.float32)
        b = c.StateEncoder.encode_numpy(game, observer)
        assert a.tobytes() == b.tobytes() and b.flags.owndata
    if game.is_game_over():
        game = c.Game(17 + i)
    assert game.apply_random_action(i*7919+17, False)
try:
    c.StateEncoder.encode_numpy(game, 128)
except TypeError:
    pass
else:
    raise AssertionError('observer overflow not rejected')
print('extension:', extension, 'training feature/hidden/mutation/invalid binding PASS')
