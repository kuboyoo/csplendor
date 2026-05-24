[English](README.en.md)

# csplendor: 高性能 Splendor エンジン

`csplendor` は、ボードゲーム Splendor 向けの高速な C++ ベースのエンジンです。2人対戦と機械学習の学習用途に最適化されています。

## 特長
- **高速なロジック**: C++17 実装により、Python の `legal_actions` 取得は約 20,000 回/秒、C++ 内部の合法手カウントは約 330,000 回/秒、C++ 内部適用の自己対戦は約 160,000 moves/sec で動作します。
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
正しく動作していることを確認するには、検証スクリプトを実行してください。
```bash
PYTHONPATH=. python tests/test_random.py
PYTHONPATH=. python tests/test_ml.py
PYTHONPATH=. python tests/test_api.py
```

---

## 行動空間リファレンス (ActionEncoderV2)

> **バージョン**: V2 (749 actions, redundancy-free)  
> **ヘッダー**: `src/action_encoder_v2.h`  
> **Python**: `csplendor.ActionEncoderV2`

### 概要

| カテゴリ | オフセット | サイズ | 計算式 |
|----------|------------|--------|--------|
| TAKE_DIFFERENT | 0 | 100 | 10 combos x 10 return patterns |
| TAKE_SAME | 100 | 105 | 5 colors x 21 return patterns |
| RESERVE_VISIBLE | 205 | 336 | 12 slots x 28 return patterns |
| RESERVE_DECK | 541 | 84 | 3 levels x 28 return patterns |
| PURCHASE_VISIBLE | 625 | 96 | 12 slots x 8 payment patterns |
| PURCHASE_RESERVED | 721 | 24 | 3 slots x 8 payment patterns |
| VISIT_NOBLE | 745 | 3 | 3 nobles |
| PASS | 748 | 1 | なし |
| **合計** | なし | **749** | なし |

### Action ID の計算

```
TAKE_DIFFERENT: ID = combo_idx * 10 + return_pattern
TAKE_SAME:      ID = 100 + color * 21 + return_pattern
RESERVE_VISIBLE: ID = 205 + (level * 4 + slot) * 28 + return_pattern
RESERVE_DECK:   ID = 541 + level * 28 + return_pattern
PURCHASE_VISIBLE: ID = 625 + (level * 4 + slot) * 8 + payment_pattern
PURCHASE_RESERVED: ID = 721 + slot * 8 + payment_pattern
VISIT_NOBLE:    ID = 745 + noble_idx
PASS:           ID = 748
```

### TAKE_DIFFERENT (10 combos x 10 return patterns = 100)

**コンボインデックス -> 取得する色**:
| コンボ | 色 |
|--------|----|
| 0 | W(0), B(1), G(2) |
| 1 | W(0), B(1), R(3) |
| 2 | W(0), B(1), K(4) |
| 3 | W(0), G(2), R(3) |
| 4 | W(0), G(2), K(4) |
| 5 | W(0), R(3), K(4) |
| 6 | B(1), G(2), R(3) |
| 7 | B(1), G(2), K(4) |
| 8 | B(1), R(3), K(4) |
| 9 | G(2), R(3), K(4) |

**返却可能な色** (コンボごと): 取得していない2色 + Gold。

| コンボ | 返却可能 |
|--------|----------|
| 0 (WBG) | R(3), K(4), Gold(5) |
| 1 (WBR) | G(2), K(4), Gold(5) |
| ... | ... |

**返却パターンインデックス** (10 patterns):
| パターン | 説明 |
|----------|------|
| 0 | 返却なし |
| 1-3 | [r0, r1, gold] のうち1つを返却 |
| 4-9 | 2つを返却 (重複組合せ) |

> **制約**: 直前に取得した色は返却できません。

### TAKE_SAME (5 colors x 21 return patterns = 105)

**返却可能な色**: 取得していない4色 + Gold。

**返却パターンインデックス** (21 patterns):
| パターン | 説明 |
|----------|------|
| 0 | 返却なし |
| 1-5 | 返却可能な5色のうち1つを返却 |
| 6-20 | 2つを返却 (H(5,2) = 15 combinations) |

> **制約**: 直前に取得した色は返却できません。

### RESERVE (12/3 slots x 28 return patterns = 336/84)

**スロットインデックス**:
- VISIBLE: `level * 4 + slot` (0-11)
- DECK: `level` (0-2)

**返却パターンインデックス** (28 patterns):
| パターン | 説明 |
|----------|------|
| 0 | 返却なし |
| 1-6 | 6色のうち1つを返却 (gold を含む) |
| 7-27 | 2つを返却 (H(6,2) = 21 combinations) |

> 予約時の gold 受け取りは必須のため、gold も返却できます。

### PURCHASE (12/3 slots x 8 payment patterns = 96/24)

**スロットインデックス**: RESERVE と同じです。

**支払いパターンインデックス**: ワイルドカードとして使う gold の総数 (0-7)。

> 正確な gold_as の内訳は、合法手との照合により決定されます。

### 色インデックス

| インデックス | 色 | 記号 |
|--------------|----|------|
| 0 | White (Diamond) | W |
| 1 | Blue (Sapphire) | B |
| 2 | Green (Emerald) | G |
| 3 | Red (Ruby) | R |
| 4 | Black (Onyx) | K |
| 5 | Gold | $ |

### 互換性メモ

- **ActionEncoderCpp (V1)**: 48 actions, compressed (return/payment variants なし)
- **ori (genbu.pt)**: 406 actions, 別のエンコード方式
- **ActionEncoderV2**: 749 actions, full detail with redundancy elimination

エンコーダ間の対応付けには、`OriAdapter.py` のマッピング関数を使用してください。

### 検証スニペット

```python
from csplendor._csplendor import ActionEncoderV2, Game, ActionType

game = Game(42)
for action in game.legal_actions:
    if action.type == ActionType.TAKE_DIFFERENT:
        taken = {i for i in range(5) if action.take[i] > 0}
        returned = {i for i in range(5) if action.return_gems[i] > 0}
        assert not (taken & returned), "Redundant action detected!"

    encoded = ActionEncoderV2.encode(action, game)
    assert 0 <= encoded < 749, f"Invalid action ID: {encoded}"
```
