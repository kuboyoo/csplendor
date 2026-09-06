#!/usr/bin/env python3
"""Run existing tests against the wheel, not the repository's old extension.

Invoke under coverage run --source=csplendor so package imports are measured
before pytest loads the repository configuration and test helpers.
"""
import hashlib
import json
from pathlib import Path
import sys
import sysconfig

# Spawned workers inherit pytest's source path. Resolve the installed package
# first in both parent and child; do not run pytest again during spawn imports.
sys.path.insert(0, sysconfig.get_path('platlib'))
import csplendor
from csplendor import _csplendor as core
import pytest

extension = Path(core.__file__).resolve()
assert extension.is_relative_to(Path(sys.prefix).resolve())
identity = {'extension': str(extension), 'sha256': hashlib.sha256(extension.read_bytes()).hexdigest(),
            'package': csplendor.__file__, 'python': sys.version}
repository = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(repository))  # scripts/test fixtures only; package already installed/loaded
if __name__ == '__main__':
    print(json.dumps(identity), flush=True)
    result = pytest.main(['-W', 'error', str(repository / 'tests')])
    assert Path(core.__file__).resolve() == extension
    assert hashlib.sha256(extension.read_bytes()).hexdigest() == identity['sha256']
    raise SystemExit(result)
