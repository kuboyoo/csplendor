# AGENTS.md

日本語で簡潔かつ丁寧に回答してください。

## このrepoの責務

- Splendorのルールエンジン、合法手生成、局面更新、C++/Python bindingを管理する。
- GUI、学習実験、大容量モデル、収集データはこのrepoに含めない。
- USI仕様そのものは `usi` を正とし、このrepoでは実装側の互換性を保つ。

## 触ってよい境界

- `src/`: C++エンジン本体。
- `csplendor/`: Python package とAPI補助。
- `tests/`: エンジンの単体・互換テスト。
- `scripts/`: 調査・ベンチマーク・再現用スクリプト。
- `doc/`: エンジン内部仕様。USI仕様の正本は `usi` 側に置く。

## 確認コマンド

```bash
pip install -e .
python -m pytest
python -m py_compile csplendor/*.py
```

## 禁止

- `venv/`, `build/`, `*.so`, `*.egg-info/`, `__pycache__/` をコミットしない。
- `.pt` モデルや大量棋譜データをこのrepoに置かない。
