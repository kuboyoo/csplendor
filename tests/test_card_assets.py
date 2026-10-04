import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_card_and_noble_images_match_engine_data():
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "render_card_assets.py"), "--check"],
        capture_output=True,
        text=True,
        cwd=ROOT,
    )
    assert result.returncode == 0, result.stderr
