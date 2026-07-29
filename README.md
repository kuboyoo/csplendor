[English](https://github.com/kuboyoo/csplendor/blob/main/README.en.md)

# csplendor: 高性能 Splendor エンジン

`csplendor` は、ボードゲーム Splendor 向けの高速な C++ ベースのエンジンです。2人対戦と機械学習の学習用途に最適化されています。

## 特長
- **高速なロジック**: C++17 実装により、合法手250件の中盤局面で Python の `legal_actions` 取得は約 26,000 回/秒、C++ 内部の合法手カウントは約 980,000 回/秒、C++ 内部適用の自己対戦は約 740,000 moves/sec で動作します（測定条件は下記）。
- **Python バインディング**: `pybind11` によりシームレスに連携できます。
- **機械学習対応**: 状態の特徴量化と行動空間のエンコードを内蔵しています。
- **Web API**: GUI 開発向けの FastAPI 連携を備えています。

### 性能目安

2026-07-13 に Ryzen 9 7900X、GCC 13.3、Release build、Python 3.12.1、
CPU 1論理コア固定で測定した値です。代表値は `tests/test_perf.py` と同じseed 42・
12手・合法手250件の中盤局面で、best-of-5を7回測定した中央値です。自己対戦行は
seed 0--9の10 gameを30標本測定した別workloadです。

| 処理 | リファクタ前 | Phase 0--7 後 | 高速化 |
|---|---:|---:|---:|
| Python `legal_actions` | 21,473 回/秒 | 26,586 回/秒 | 1.24倍 |
| C++ `legal_action_codes` | 61,313 回/秒 | 118,594 回/秒 | 1.93倍 |
| C++ `legal_action_count` | 316,991 回/秒 | 981,149 回/秒 | 3.10倍 |
| C++ 内部自己対戦 | 160,545 moves/sec | 740,538 moves/sec | 4.61倍 |

同じ250件局面の30-pair sustained A/Bでは、`legal_actions` は約1.19倍（95% CI:
1.11--1.19倍）、codesは約1.97倍、countは約3.11倍でした。一方、合法手5件の
固定中盤局面では固定長buffer初期化の削減が強く効き、`legal_actions` は5.07倍、
codesは9.17倍、countは9.62倍です。したがって合法手生成が一律5倍になったわけではなく、
Python Action object生成の割合と合法手数で倍率が変わります。

旧README掲載値との単純比較は、`legal_actions` が20,000から26,586回/秒で1.33倍、
countが330,000から981,149回/秒で2.97倍、自己対戦が160,000から740,538
moves/secで4.63倍です。リファクタリング効果の評価には、測定条件を揃えた上表または
paired A/Bの倍率を用いてください。

NN推論を除いた256 simulationのnative MCTS synthetic searchでは、非決定化が
70,690から94,427 simulations/sec（1.34倍）、決定化ありが65,001から108,441
simulations/sec（1.67倍）でした。履歴200・決定化ありのcopy中心microbenchmarkは
14.7倍ですが、履歴0ではほぼ同速です。実モデル込みの速度向上はNN推論時間の割合に
依存します。詳細と注意点は[リファクタリング計画の最終計測](https://github.com/kuboyoo/csplendor/blob/main/doc/refactoring_plan/README.md#phase-0--7-%E6%9C%80%E7%B5%82%E6%80%A7%E8%83%BD%E5%86%8D%E8%A8%882026-07-13)を参照してください。

### 実験的な並列MCTS

共有tree並列探索はStage Bのexperimental opt-inです。既定の`num_threads=1`はworker queueを
作らない低overheadなserial pathで、`num_threads>=2`のときだけnative traversal workerと単一の
inference coordinatorを使います。Python evaluator callbackは常に同期的・非並行に呼ばれます。

```python
import numpy as np
import csplendor as cs

game = cs.Game(seed=42)
mcts = cs.MCTS(cs.MCTSConfig())

options = cs.ParallelSearchOptions()
options.num_threads = 4
options.num_simulations = 800
options.max_tree_nodes = 50_000
options.tree_backend = cs.ParallelTreeBackend.SHARDED
options.mode = cs.ParallelSearchMode.THROUGHPUT
options.search_nonce = 1

def evaluator(requests):
    results = []
    for request in requests:
        policy = request["valid_actions"].astype(np.float32)
        policy /= policy.sum()
        results.append({
            "policy": policy,
            "value": np.zeros(2, dtype=np.float32),
        })
    return results

result = cs.mcts_search_parallel_native(
    mcts, game, options, evaluator, 1.0
)
```

`DETERMINISTIC_EPOCH`は単一coordinatorがtraversal、callback、commitを決定順で実行する
trace/replay oracleです。このmodeの`num_threads`は結果互換性の入力であり、並列completionの
reorderを発生させません。root-parallel APIで正の探索budgetを使う場合は、workerのseed範囲を
固定する明示`search_nonce`が必須です。また`timeout_ms`はcallback境界で観測するsoft timeoutで、
block中のevaluatorを強制中断しません。

`max_tree_nodes`の既定値50,000はshared-treeでは単一tree上限、root-parallelでは全active worker
treeの合計上限です。capacity到達後もrootが展開済みならpartial resultを返し、visitが0の場合は
legal action上で正規化したprior（設定時はroot noiseを混合）を使います。root未展開なら
`TreeCapacityReachedError`です。Python root-parallel callbackは直列化され、mutex待機後にも
timeout/cancelを再検査するため、期限切れのcallback backlogを流しません。

複数threadをstable/defaultへ昇格するには、scheduled sanitizer/soak、可変scheduler seed、
実NN、fixed-time探索品質に加え、展開済みnodeの二次feature signature照合gateが残っています。
現在のfeature digest検査は同一pendingへdeduplicateされたowner/waiter間です。問題時はlegacy API
または`num_threads=1`へ戻せます。
詳細は[並列探索の実装状況](https://github.com/kuboyoo/csplendor/blob/main/doc/parallel_search_plan/implementation_status.md)を参照してください。

## インストールとビルド

### 前提条件
- C++17 対応コンパイラ (例: GCC 9+)
- CMake 3.13+
- Python 3.8+

build依存とNumPyはpackage metadataから導入されます。FastAPIはoptionalなので、
Web service利用時は `pip install "csplendor[web]"` を使ってください。

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

### macOS Apple SiliconのCPUターゲット

`CSPLENDOR_CPU_TARGET`で、配布用とローカル最適化用を同じソースから
分けてビルドできます。

- `portable`（既定）: CPU固有フラグを追加しません。汎用arm64 wheelなどの
  配布物には必ずこちらを使用します。
- `native`: Apple SiliconのローカルCPUに合わせて`-mcpu=native`を使用します。
  M4 Pro上ではM4向けコードになります。

Pythonビルドのarchitectureは`CSPLENDOR_OSX_ARCHITECTURES`へ`arm64`、
`x86_64`、`universal2`のいずれかを指定できます。`ARCHFLAGS`などと競合する
指定はエラーになります。通常wheelでは選択したarchitectureとplatform tagも
照合するため、クロスビルドには一致するPythonまたは`_PYTHON_HOST_PLATFORM`が
必要です。

Python拡張のビルド例:

```bash
# 配布用の汎用arm64 wheel
MACOSX_DEPLOYMENT_TARGET=11.0 \
  CSPLENDOR_OSX_ARCHITECTURES=arm64 \
  CSPLENDOR_CPU_TARGET=portable \
  python -m pip wheel . --wheel-dir dist/arm64

# このMac用のローカル最適化版
CSPLENDOR_OSX_ARCHITECTURES=arm64 \
  CSPLENDOR_CPU_TARGET=native \
  python -m pip install -e .
```

CMakeを直接使う場合は、異なるbuild directoryを指定します。

```bash
cmake -S . -B build/macos-arm64-portable \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DCSPLENDOR_CPU_TARGET=portable
cmake --build build/macos-arm64-portable --parallel 2

cmake -S . -B build/macos-m4-native \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCSPLENDOR_CPU_TARGET=native
cmake --build build/macos-m4-native --parallel 2
```

`native`はeditable installまたはCMake直接ビルド専用です。通常wheelと同じ互換性tag
ではM4専用であることを表現できないため、native wheelの作成はエラーになります。
また、以前のprofileのバイナリを混入させないため、wheelの`--skip-build`も
使用できません。
例ではApple Siliconの最小OSであるmacOS 11.0をdeployment targetにしています。
サポート方針に応じて、これより新しい値へ変更できます。環境変数を省略したPython
ビルドでは、そのPython自身のdeployment targetをCMakeへ引き継ぎます。
wheelの互換性tagはビルドに使うPython自身の下限にも制約されるため、リリース時は
Mach-Oのminimum OSとwheel tagの両方を確認してください。
Rosetta、universal2、非Apple環境では`native`を使用できません。

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
pip install "csplendor[web]"
uvicorn csplendor.api:app --reload
```

game/session/replay endpointは単体で動作します。旧`/ai_move` bridgeは互換用の
optional integrationで、torchと外部`dlsplendor` packageを遅延loadします。
modelやNN探索コードはcsplendorへ同梱しません。外部stackがない場合はHTTP 503を返し、
ルールエンジンと他のWeb endpointには影響しません。

旧`.pkl` replay viewerはpickleを読み込むため、設定済みreplay data directoryへ
server管理者が配置した信頼済みローカルファイルだけを対象にしてください。
`/replay/load`はrealpathがdirectory内にある`.pkl`だけを受理し、directory外の
任意path、path traversal、directory外を指すsymlinkを拒否します。`/replay/files`は
絶対pathを公開せず、一覧取得のためにunpickleもしません。uploadや外部入力をそのまま
配置しないでください。

## 詰み探索

`scripts/dfpn_mate_solver.py` は、任意局面から player0 または player1 の強制勝利を探索します。

実用上は、公開カードだけで候補手順を高速探索し、その後に未公開カードのめくれ、相手の全応手、全支払いパターン、局面入力後の山札予約結果を検証する `--reveal-verified` モードを推奨します。めくれ検証では visible-only の最短主手順 prefix を固定して先に厳密検証し、証明できなかった場合は固定範囲を緩め、最後に通常の幅広い検証へ戻ります。

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
  --proof-dag-format compact \
  --proof-dag-node-limit 100000 \
  --proof-dag-edge-limit 500000 \
  --time-limit 30
```

証明 DAG は `proof_tree.verification.proof_dag` に返ります。攻撃側は証明に採用した手、守備側は全合法応手、山札予約は全ドロー結果を保持します。既定の `compact` 形式では、同じ action/child に進む複数の具体めくれカードをカードID bitset の reveal group としてまとめ、edge は action template と reveal group への参照で保存します。具体カード集合は保持するため、全めくれに対する応手情報は失いません。従来の辞書型 DAG が必要な場合は `--proof-dag-format v1`、比較用に両方出す場合は `--proof-dag-format both` を指定します。complete DAG は返却前に全 edge を合法手として再走査し、検査済みなら `validated: true` になります。上限超過時も詰み判定結果は維持し、DAG のみ破棄して理由を返します。

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

生成物は `depth_XX/<問題ID>/` に分類されます。`XX` はソルバー上の攻撃側手数深さです。各問題には局面情報 `problem.json`、代表手順 `answer.kifu`、完全応手DAG `strategy.json` が含まれます。`problem.json` の `quality.countermate_blunders` には相手側の詰みを許す誤答例が入ります。DAGは攻撃側の証明手、守備側の全合法応手、公開カード補充と山札予約を含む全めくれ結果を保持し、同一局面をノードIDで共有します。`strategy.json` は既定で compact DAG を保存し、めくれカード集合は reveal group の bitset として厳密に残します。従来形式が必要な場合は生成時に `--strategy-dag-format v1`、比較用に両方残す場合は `--strategy-dag-format both` を指定します。めくれ候補は現在の山札だけから取り、同じレベル・点数・ボーナス・コストのカードは同型として代表だけを検証します。公開カード補充は一度 blank として進めた局面から反例になりやすい reveal を推定し、危険度の高い候補から検証します。非公開カードを即購入・即予約する oracle 手は合法手順DAGには出力されません。既定上限に収まらず完全DAGを保存できない局面は採用されません。再現性を保つため、生成物内の SPN は伏せ予約カードを `?C<id>` 形式で保存します。通常の公開用 SPN における `?L<level>` と異なり、伏せ予約であることと実カードIDの両方を保持します。購入済みカードは `bought:[<id>,...]`、取得済み貴族は player section の `nobles:[<id>,...]` に保存します。

## ドキュメント
詳細な仕様は `doc/` ディレクトリを参照してください。
- [技術概要](https://github.com/kuboyoo/csplendor/blob/main/doc/overview.md)
- [エンジン仕様](https://github.com/kuboyoo/csplendor/blob/main/doc/engine_specs.md)
- [Python API リファレンス](https://github.com/kuboyoo/csplendor/blob/main/doc/api_ref.md)
- [ML 連携ガイド](https://github.com/kuboyoo/csplendor/blob/main/doc/ml_integration.md)
- [Web API リファレンス](https://github.com/kuboyoo/csplendor/blob/main/doc/web_api.md)
- [リリース検証記録](https://github.com/kuboyoo/csplendor/blob/main/doc/release_validation.md)

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
- **強制パス**: 通常手がない場合だけ `Game.legal_actions` は
  `ActionType.PASS` を1件返します。48枠MCTS policyには強制手の枠を増やさず、
  その局面をroot探索する前に `Game.apply_forced_pass()` を呼びます。
- **seedの移植性**: `Game(seed)` の初期配置/deck shuffleはrepository管理の
  portable shuffleを使い、libstdc++・libc++・MSVC間で同じ結果になります。
  native並列MCTSも別途version管理されたportable RNG契約を使います。
