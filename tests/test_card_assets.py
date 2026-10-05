import re
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


def _play_assets():
    sys.path.insert(0, str(ROOT / "scripts"))
    import render_card_assets

    return {
        path.relative_to(render_card_assets.ASSETS).as_posix(): text
        for path, text in render_card_assets.render().items()
        if path.relative_to(render_card_assets.ASSETS).parts[0] == "play"
    }


def test_play_edition_files():
    files = _play_assets()
    expected = (
        {f"play/cards/card_{i:02d}.svg" for i in range(90)}
        | {f"play/nobles/noble_{i:02d}.svg" for i in range(12)}
        | {f"play/decks/deck_l{level}.svg" for level in (1, 2, 3)}
    )
    assert set(files) == expected


def test_play_edition_shows_only_numerals():
    for name, text in _play_assets().items():
        assert "◆" not in text and "ID" not in re.sub(r"<[^>]*>", "", text), name
        labels = re.findall(r"<text[^>]*>([^<]*)</text>", text)
        assert all(label.isdigit() for label in labels), (name, labels)
