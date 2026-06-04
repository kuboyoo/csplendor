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

## 詰み探索

`scripts/dfpn_mate_solver.py` は、任意局面から player0 または player1 の強制勝利を探索します。

実用上は、公開カードだけで候補手順を高速探索し、その後に未公開カードのめくれ、相手の全応手、全支払いパターン、局面入力後の山札予約結果を検証する `--reveal-verified` モードを推奨します。

```bash
python scripts/dfpn_mate_solver.py \
  --position 'bank:... | visible:... | decks:... | nobles:... | P0:... | P1:... | 0' \
  --attacker 0 \
  --reveal-verified \
  --time-limit 30 \
  --pretty
```

完全な詰み応手を確認する場合は、証明に関係する局面だけを DAG 形式で出力できます。同一局面はノード ID で共有されるため、木を単純展開するよりメモリ使用量を抑えられます。

```bash
python scripts/dfpn_mate_solver.py \
  --position '...' \
  --attacker 0 \
  --reveal-verified \
  --reveal-proof-dag \
  --proof-dag-node-limit 100000 \
  --proof-dag-edge-limit 500000 \
  --time-limit 30
```

証明 DAG は `proof_tree.verification.proof_dag` に返ります。攻撃側は証明に採用した手、守備側は全合法応手、山札予約は全ドロー結果を保持します。上限超過時も詰み判定結果は維持し、DAG のみ破棄して理由を返します。

確認用の主手順を `splendorgui` で再生する場合は、`--kifu-output mate.kifu` を追加します。`--kifu-output` は既定で `--reveal-verified` を有効化し、検証済み候補主手順を Splendor KIFU として保存します。通常の DFPN 証明木から主手順を保存する場合は `--kifu-dfpn` も指定します。具体的なめくれカードを持つ DFPN 証明木では、棋譜コメントに `reveal:C<id>` 注釈を出力します。

`--simple-payment` を指定すると、購入時の支払いをゴールド温存パターンに限定できます。完全検証が必要な場合は指定しないでください。

### 詰め問題集の生成

`scripts/generate_mate_puzzles.py` は、`dlsplendor.search.genbu_adapter.GenbuAdapter` を使った Genbu AI 同士の対局から終盤局面を生成し、めくれまで検証済みの詰みだけを問題集として保存します。ランダムに選んだ終盤開始手数に到達した後は、詰みが初めて見つかるまで1手番ごとに候補局面を検証します。AI 対局中だけ簡易支払いモードを有効にします。詰みが見つかった局面のうち、点差が小さく、さらに正解手以外を選ぶと相手側の検証済み詰みが成立する局面だけを採用します。詰み検証では通常支払いモードに戻し、購入時の全支払いパターン、局面入力後の山札予約、めくれを検証します。

```bash
python scripts/generate_mate_puzzles.py \
  --output-dir generated/mate_puzzles \
  --count 100 \
  --max-attempts 10000 \
  --genbu-weights scripts/weights/genbu.pt \
  --genbu-simulations 100 \
  --time-limit 30
```

進捗は attempt 開始、詰み探索開始、誤答側詰み探索開始時に表示されます。棄却時は `stage=rejected`、棄却理由、完全な SPN `position` を表示します。Genbu 対局中の定期表示間隔は `--progress-seconds` で変更できます。

高コストなめくれ検証の前に、点差、合法手数、両者の楽観的な近未来得点、visible-only 探索で候補を絞ります。既定では両者が3手以内に15点へ到達しうる合法手12個以上の局面を対象とし、depth 3以上の詰みだけを採用します。詰み証明後は全合法初手を固定して再検証し、別解がない問題だけを保存します。条件は `--threat-turns`、`--min-legal-actions`、`--min-optimistic-score`、`--min-depth`、`--visible-prefilter-time-limit`、`--uniqueness-time-limit` で調整できます。

生成物は `depth_XX/<問題ID>/` に分類されます。`XX` はソルバー上の攻撃側手数深さです。各問題には局面情報 `problem.json`、代表手順 `answer.kifu`、完全応手DAG `strategy.json` が含まれます。`problem.json` の `quality.countermate_blunders` には相手側の詰みを許す誤答例が入ります。DAGは攻撃側の証明手、守備側の全合法応手、公開カード補充と山札予約を含む全めくれ結果を保持し、同一局面をノードIDで共有します。購入・予約でめくれたカードは DAG エッジの `reveal_card` に具体的なカード ID として保存されます。めくれ候補は現在の山札だけから取り、同じレベル・点数・ボーナス・コストのカードは同型として代表だけを検証します。公開カード補充は一度 blank として進めた局面から反例になりやすい reveal を推定し、危険度の高い候補から検証します。非公開カードを即購入・即予約する oracle 手は合法手順DAGには出力されません。既定上限に収まらず完全DAGを保存できない局面は採用されません。再現性を保つため、生成物内の SPN は伏せ予約カードを `?C<id>` 形式で保存します。通常の公開用 SPN における `?L<level>` と異なり、伏せ予約であることと実カードIDの両方を保持します。購入済みカードは `bought:[<id>,...]`、取得済み貴族は player section の `nobles:[<id>,...]` に保存します。

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
