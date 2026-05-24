[English](README.en.md)

# csplendor: 高性能 Splendor エンジン

`csplendor` は、ボードゲーム Splendor 向けの高速な C++ ベースのエンジンです。2人対戦と機械学習の学習用途に最適化されています。

## 特長
- **高速なロジック**: C++17 実装により約 20,000 moves/sec で動作します。
- **Python バインディング**: `pybind11` によりシームレスに連携できます。
- **機械学習対応**: 状態の特徴量化と行動空間のエンコードを内蔵しています。
- **Web API**: GUI 開発向けの FastAPI 連携を備えています。

## インストールとビルド

### 前提条件
- C++17 対応コンパイラ (例: GCC 9+)
- CMake 3.12+
- Python 3.8+
- `pybind11`, `numpy`, `fastapi`, `uvicorn`

### ソースからのビルド
C++ ソースファイルを変更した場合は、拡張モジュールを再ビルドする必要があります。

**方法 1: pip を使う (開発時の推奨)**
```bash
pip install -e .
```

**方法 2: 手動で CMake ビルドする**
```bash
mkdir -p build
cd build
cmake ..
make -j
# コンパイル済みライブラリをパッケージディレクトリへコピー
cp _csplendor.*.so ../csplendor/
```

## 基本的な使い方 (Python)

```python
import csplendor

# 1. ゲームを初期化
game = csplendor.Game(seed=42)

# 2. 合法手を取得
legals = game.legal_actions
print(f"Legal moves: {len(legals)}")

# 3. 行動を適用
action = legals[0]
game.apply(action)

# 4. 状態へアクセス
board = game.board
print(f"Current Turn: {board.turn}")
print(f"Scores: {game.scores}")

# 5. 機械学習向けに特徴量化
featurizer = csplendor.StateFeaturizer()
features = featurizer.featurize(game) # numpy array (196,)
```

## Web API の実行
GUI と連携する FastAPI サーバーを起動するには、次を実行します。
```bash
uvicorn csplendor.api:app --reload
```

## ドキュメント
詳細な仕様は `doc/` ディレクトリを参照してください。
- [技術概要](doc/overview.md)
- [エンジン仕様](doc/engine_specs.md)
- [Python API リファレンス](doc/api_ref.md)
- [ML 連携ガイド](doc/ml_integration.md)
- [Web API リファレンス](doc/web_api.md)

## テスト
通常のテストは次で実行します。
```bash
pip install -e ".[dev,web]"
python -m pytest
python -m compileall -q csplendor
```

性能確認は明示的に指定して実行します。
```bash
python -m pytest -m performance
```

---

## 行動空間リファレンス

現行の推奨エンコーダは `ActionEncoderV3` です。購入行動をカードIDベースで表すため、スロット位置に依存する重複を減らしています。

### ActionEncoderV3 (3133 actions)

| カテゴリ | オフセット | サイズ | 内容 |
|----------|------------|--------|------|
| TAKE_DIFFERENT | 0 | 840 | 10 combos x 84 return patterns |
| TAKE_SAME | 840 | 140 | 5 colors x 28 return patterns |
| RESERVE_VISIBLE | 980 | 84 | 12 slots x 7 return patterns |
| RESERVE_DECK | 1064 | 21 | 3 levels x 7 return patterns |
| PURCHASE | 1085 | 2035 | 90 cards x card-specific payment patterns |
| VISIT_NOBLE | 3120 | 12 | noble ID 0-11 |
| PASS | 3132 | 1 | なし |
| **合計** | なし | **3133** | なし |

### ActionEncoderV2 (4869 actions)

`ActionEncoderV2` は互換用のフル行動空間エンコーダです。購入行動を表示スロット/予約スロット別に表します。

| カテゴリ | オフセット | サイズ | 内容 |
|----------|------------|--------|------|
| TAKE_DIFFERENT | 0 | 840 | 10 combos x 84 return patterns |
| TAKE_SAME | 840 | 140 | 5 colors x 28 return patterns |
| RESERVE_VISIBLE | 980 | 84 | 12 slots x 7 return patterns |
| RESERVE_DECK | 1064 | 21 | 3 levels x 7 return patterns |
| PURCHASE_VISIBLE | 1085 | 3024 | 12 slots x 252 payment patterns |
| PURCHASE_RESERVED | 4109 | 756 | 3 slots x 252 payment patterns |
| VISIT_NOBLE | 4865 | 3 | visible noble slots |
| PASS | 4868 | 1 | なし |
| **合計** | なし | **4869** | なし |

### 互換性メモ

- **ActionEncoderCpp**: 48 actions, return/payment variants なしの圧縮表現。
- **ActionEncoderV2**: 4869 actions, return/payment variants をすべて含むスロットベース表現。
- **ActionEncoderV3**: 3133 actions, 現行推奨のカードIDベース表現。
