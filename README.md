[English](https://github.com/kuboyoo/csplendor/blob/main/README.en.md)

# csplendor: 高性能 Splendor エンジン

`csplendor` は、ボードゲーム Splendor 向けの高速な C++ ベースのエンジンです。2人対戦と機械学習の学習用途に最適化されています。

## 特長
- **高速なロジック**: C++17 実装により、合法手250件の中盤局面で Python の `legal_actions` 取得は約 27,000 回/秒、C++ 内部の合法手カウントは約 1,012,000 回/秒、C++ 内部適用の自己対戦は約 893,000 moves/sec で動作します（測定条件は下記）。
- **Python バインディング**: `pybind11` によりシームレスに連携できます。
- **機械学習対応**: 状態の特徴量化と行動空間のエンコードを内蔵しています。
- **Web API**: GUI 開発向けの FastAPI 連携を備えています。

### 性能目安

`Phase 0--7 後`は2026-07-13にRyzen 9 7900X、GCC 13.3で、`現行`は
2026-08-30に同じCPU、GCC 15.2で測定しました。いずれもRelease build、Python 3.12.1、
CPU 1論理コア固定です。代表値は `tests/test_perf.py` と同じseed 42・12手・
合法手250件の中盤局面です。`Phase 0--7 後`はbest-of-5を7回、`現行`は同じ測定を
3 batch実行した21標本の中央値です。自己対戦行はseed 0--9の10 gameを1標本とし、
それぞれ30標本、90標本を測定した別workloadです。

| 処理 | リファクタ前 | Phase 0--7 後 | 現行 | 現行/リファクタ前 |
|---|---:|---:|---:|---:|
| Python `legal_actions` | 21,473 回/秒 | 26,586 回/秒 | 27,084 回/秒 | 1.26倍 |
| C++ `legal_action_codes` | 61,313 回/秒 | 118,594 回/秒 | 125,444 回/秒 | 2.05倍 |
| C++ `legal_action_count` | 316,991 回/秒 | 981,149 回/秒 | 1,011,935 回/秒 | 3.19倍 |
| C++ 内部自己対戦 | 160,545 moves/sec | 740,538 moves/sec | 892,607 moves/sec | 5.56倍 |

Phase 0--7 後を測定した同じ250件局面の30-pair sustained A/Bでは、
`legal_actions` は約1.19倍（95% CI:
1.11--1.19倍）、codesは約1.97倍、countは約3.11倍でした。一方、合法手5件の
固定中盤局面では固定長buffer初期化の削減が強く効き、`legal_actions` は5.07倍、
codesは9.17倍、countは9.62倍です。したがって合法手生成が一律5倍になったわけではなく、
Python Action object生成の割合と合法手数で倍率が変わります。

現行値とリファクタ前の単純比較は、`legal_actions` が1.26倍、codesが2.05倍、
countが3.19倍、自己対戦が5.56倍です。現行列はcompiler更新を含む再測定値であり、
Phase 0--7 後との差だけを個別最適化の効果とはみなしません。厳密な変更評価には、
同一build条件のpaired A/Bを用いてください。

#### MCTS探索性能

2026-08-04に同じRyzen 9 7900X、GCC 13、portable Release buildで、高速化前の`main`
（`6ddb47c`）とMCTSホットパス高速化後を同一host・seed・tree size・batch sizeで比較した
結果です。zero-latency native evaluatorを使い、5標本の中央値を示しています。

| mode/backend | 高速化前`main` | 高速化後 | 高速化 |
|---|---:|---:|---:|
| exact legacy 1 thread | 37,487 sim/s | 387,132 sim/s | 10.33倍 |
| exact sharded 1 thread | 31,773 sim/s | 222,253 sim/s | 7.00倍 |
| exact sharded 4 threads | 94,819 sim/s | 217,910 sim/s | 2.30倍 |
| exact sharded 8 threads | 125,095 sim/s | 194,405 sim/s | 1.55倍 |
| exact root-parallel 8 workers | 286,487 sim/s | 1,418,195 sim/s | 4.95倍 |
| determinized legacy 1 thread | 56,969 sim/s | 358,261 sim/s | 6.29倍 |
| determinized sharded 4 threads | 156,161 sim/s | 294,279 sim/s | 1.88倍 |
| determinized root-parallel 8 workers | 440,313 sim/s | 1,584,560 sim/s | 3.60倍 |

構成要素のmicrobenchmarkでは、48手action maskが624.6 nsから32.7 ns（19.10倍）、
action decodeが3,240.5 nsから15.4 ns（210.07倍）、dense mask走査が22.0 nsから
4.7 ns（4.69倍）になりました。40,000 simulationのsharded 8-thread実行では最大RSSが
159,832 KiBから44,644 KiBへ約72%減少しています。

実モデル込みの速度向上はNN推論時間の割合に依存します。測定方法、O(1)監査、compact
edgeの詳細は[MCTSホットパス高速化](https://github.com/kuboyoo/csplendor/blob/main/doc/mcts_hotpath_optimizations.md)を参照してください。

#### めくれ厳密詰み探索性能

2026-08-30にRyzen 9 7900X、GCC 15.2、portable Release build、Python 3.12.1、
CPU 1論理コア固定で測定しました。5手詰め収集局面の初手を固定し、深さ7、
`exact_reveal_search=True`、1実行1,000万ノードで、warmup 2回後の15標本の
中央値を比較しています。高速化項目だけを切り替え、探索順と訪問ノード数は同一です。

| 指標 | 高速化前 | 高速化後 | 効果 |
|---|---:|---:|---:|
| 探索速度 | 4,928,183 nodes/sec | 5,440,074 nodes/sec | 1.104倍 |
| 1,000万ノードの実時間 | 2.029秒 | 1.838秒 | 9.4%短縮 |

探索速度の改善率は10.39%で、bootstrap 95% CIは+9.19%--+11.73%でした。
両実装とも合法手8,524,863件、置換表hit 778,150件、保存局面643,158件で停止しており、
この測定では探索量を変えずにノード処理を高速化しています。

厳密めくれ探索では、山札を順列ではなく残存カード集合として扱います。探索専用hashから
山札順と絶対turnを除き、残存カードbitsetを別keyとして保持することで、完全情報用の
`Board.hash()`の意味を変えずに重複計算と同値局面の分断を避けます。さらに、node budgetから
置換表容量を保守的に事前確保してrehashを抑えます。node limitは従来どおり毎nodeで厳密に
検査し、wall-clockと外部cancelだけを64 nodeごとに検査します。node 0では必ず検査するため、
事前cancelと即時timeoutの挙動は維持されます。

2026-09-05のPhase 3Cでは、詰み探索の置換表を用途別に圧縮しました。portable Releaseの
paired A/Bで、exact 5手局面は3.91%高速化し、exact/visible solverのpeak RSSは局面により
11.91%〜27.38%減少しました。production型TT microは28.40%高速化した一方、key生成単体は
2.33%低下しています。探索量・候補順・5手/7手詰み・証明DAGは同一です。測定fixtureと方法が
上表とは異なるため倍率は合算していません。詳細は
[Phase 3C測定記録](doc/performance_experiments/phase3c_solver_tt_compaction_20260905.md)を参照してください。

Phase 3D-P1では、めくれ候補のスコアをsort比較のたびに再計算せず、一度だけ計算するように
しました。3C後を基準とする固定探索量のpaired A/Bで、代表deepは1.497倍（独立再測定1.486倍）、
shallowは1.307倍、warm sessionは1.544倍です。候補順・探索結果を維持し、5手/7手詰み、
proof/frontier、cache再利用、ASan/UBSanを検証しました。depth7の速度fixtureはnode上限で
UNKNOWNとなるため、7手詰みの完遂速度を意味しません。小さなproof計測の不確実性を含む詳細は
[Phase 3D-P1測定記録](doc/performance_experiments/phase3dp1_score_once_20260905.md)を参照してください。

Phase 3D-P2では、再帰呼出しごとに合法手・めくれ候補の一時配列を再利用するようにしました。
3D-P1後を基準に、代表deepは1.085倍（独立再測定1.082倍）、shallowは1.076倍、
warm sessionは1.073倍です。同じ100万ノード探索の確保回数は約938万回から330万回へ減少し、
5手/7手詰み、proof/frontier、cache再利用、ASan/UBSanの検証を通過しました。
過去Phaseとの倍率は乗算していません。極小proofの単発測定に残る制約を含む詳細は
[Phase 3D-P2測定記録](doc/performance_experiments/phase3dp2_search_scratch_20260905.md)を参照してください。

Phase 3D-1では、visible-only詰み探索の通常着手を軽量なRAII復元へ変更しました。
3D-P2比で代表sliceは1.182倍（独立再測定1.172倍）、確保回数は約255万回から81万回へ
減少しました。めくれ込みsolverへの適用案はproofの回帰基準未達で採用せず、従来方式を維持します。
5手/7手詰み・全回帰・ASan/UBSanを検証済みです。採用範囲と棄却判断の詳細は
[Phase 3D-1測定記録](doc/performance_experiments/phase3d1_normal_rollback_20260905.md)を参照してください。

Phase 3D-2/3D-3は採否評価を完了しました。対象山だけを復元する3D-2の試作は代表deepで
1.012倍（独立再測定1.010倍）にとどまり、採用基準未達で撤去しました。3D-3は代表探索で
購入ごとの実訪問めくれ数が1枚だったため、prefix共用を導入していません。
既存の高速化を維持し、PERF専用診断と記録のみ追加しています。追加の高速化は主張しません。
詳細は[Phase 3D-2/3D-3測定記録](doc/performance_experiments/phase3d23_reveal_transactions_20260905.md)を参照してください。

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
使用できません。PEP 660 editable installが内部で作る一時wheelは配布物ではないため、
配布wheel向けのarchitecture/tag照合は適用しません。
例ではApple Siliconの最小OSであるmacOS 11.0をdeployment targetにしています。
サポート方針に応じて、これより新しい値へ変更できます。環境変数を省略したPython
ビルドでは、そのPython自身のdeployment targetをCMakeへ引き継ぎます。
wheelの互換性tagはビルドに使うPython自身の下限にも制約されるため、リリース時は
Mach-Oのminimum OSとwheel tagの両方を確認してください。
universal2 Pythonも、arm64プロセスとして実行し、arm64専用拡張を選択したeditable
installまたはCMake直接ビルドでは`native`を使用できます。生成物はarm64専用なので、
同じPythonをRosettaでx86_64として実行した場合には読み込めません。Rosetta上のbuild、
universal2拡張、非Apple環境では`native`を使用できません。

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

完全DAGが大きすぎる場合は、`csplendor.expand_mate_frontier()` で現在局面の1層だけを検証・展開できます。攻撃側ノードでは証明手1手の全めくれ、守備側ノードでは全合法手の全めくれを返します。各 edge の `child_state` は、SPNに含まれない終局・最終ラウンド・貴族選択待ちも保持する版付きスナップショットです。次の呼び出しでは `load_mate_frontier_game(state=...)` で復元します。

```python
import csplendor as cs

game = cs.load_mate_frontier_game(position=position)
frontier = cs.expand_mate_frontier(game, attacker=1, depth=5)
edge = frontier["edges"][0]
child = cs.load_mate_frontier_game(state=edge["child_state"])
next_frontier = cs.expand_mate_frontier(
    child,
    attacker=1,
    depth=edge["child_depth"],
)
```

深さを1手ずつ増やして最初の詰みを調べる場合は
`cs.search_reveal_verified_mate_depths()` を使います。不詰みを確定した深さだけを
通過し、詰み、`Unknown`、累積予算、または最大深さで停止します。
終局済みの非勝利局面、または残る全カード・貴族の点を攻撃側が独占しても15点に
届かない局面では、深さに依存しない `permanent_no_mate` 証明で直ちに停止します。

```python
depth_search = cs.search_reveal_verified_mate_depths(
    game,
    attacker=1,
    min_depth=5,
    max_depth=8,
    max_nodes=10_000_000,
    time_limit_seconds=120,
)
```

実戦AIでは、同一対局中に1個の `MateSearchSession` を保持します。`search_anytime()`
は浅い深さが未確定でも次へ進み、正の詰み証明だけを返すため、持ち時間内の着手
選択に向いています。anytime探索中も一部のCPU予算で厳密置換表を育て、次の深さ
では浅い証明手・反例手を合法手順序へ反映します。相手応手後の局面が前手番の表に
あれば、同じ深さの結果は1ノードの参照で再利用されます。

```python
session = cs.MateSearchSession(
    attacker=ai_player,
    jobs=16,
    max_cache_states=2_000_000,
)

result = session.search_anytime(
    game,
    min_depth=1,
    max_depth=8,
    time_limit_seconds=2.0,
)
if result["status"] == "mate":
    action = cs.Action.unpack(result["winning_root_action"])
    # 通常のAI候補より優先して、この検証済み着手を選択する

# 対局終了・別対局開始時だけ破棄する
session.clear()
```

最短手数まで保証する解析用途では `session.search()` または
`search_reveal_verified_mate_depths()` を使います。こちらは各N手不詰みを全合法手・
具体的めくれについて確定してからN+1へ進むため、実戦向けより高コストです。
思考時間を外部から打ち切る場合は `session.cancel()` を別スレッドから呼べます。

確認用の主手順を `splendorgui` で再生する場合は、`--kifu-output mate.kifu` を追加します。`--kifu-output` は既定で `--reveal-verified` を有効化し、検証済み候補主手順を Splendor KIFU として保存します。通常の DFPN 証明木から主手順を保存する場合は `--kifu-dfpn` も指定します。具体的なめくれカードを持つ DFPN 証明木では、棋譜コメントに `reveal:C<id>` 注釈を出力します。

`--simple-payment` を指定すると、購入時の支払いをゴールド温存パターンに限定できます。完全検証が必要な場合は指定しないでください。

### 詰め問題集の生成

`scripts/generate_mate_puzzles.py` は、`dlsplendor.search.genbu_adapter.GenbuAdapter` を使った Genbu AI 同士の対局から終盤局面を生成し、めくれまで検証済みの詰みだけを問題集として保存します。ランダムに選んだ終盤開始手数に到達した後は、詰みが初めて見つかるまで1手番ごとに候補局面を検証します。AI 対局中だけ簡易支払いモードを有効にします。詰み検証では通常支払いモードに戻し、購入時の全支払いパターン、局面入力後の山札予約、めくれを検証します。

```bash
python scripts/generate_mate_puzzles.py \
  --output-dir generated/mate_puzzles \
  --count 100 \
  --max-attempts 10000 \
  --genbu-weights scripts/weights/genbu.pt \
  --genbu-simulations 100 \
  --min-depth 5 \
  --max-depth 7 \
  --no-strategy-dag \
  --mate-jobs 16 \
  --uniqueness-jobs 16 \
  --time-limit 30
```

旧Genbuモデルの実行に必要な `alphazero-general-ori` は、`dlsplendor` 直下、
同階層、および標準workspaceの `workspace/src/alphazero-general-ori` から自動検出します。
別の場所に置く場合は `ALPHAZERO_ORI_PATH=/path/to/alphazero-general-ori` を指定してください。
Numbaキャッシュは既定で一時ディレクトリへ保存するため、旧ソースツリーが読み取り専用でも
実行できます。

進捗は attempt 開始と詰み探索開始時に表示されます。`--min-losing-alternatives` を1以上にした場合は誤答側詰み探索も表示されます。棄却時は `stage=rejected`、棄却理由、完全な SPN `position` を表示します。Genbu 対局中の定期表示間隔は `--progress-seconds` で変更できます。

高コストなめくれ検証の前に、点差、合法手数、両者の楽観的な近未来得点、visible-only 探索で候補を絞ります。既定では両者が3手以内に15点へ到達しうる合法手12個以上の局面を対象とし、depth 3以上の詰みだけを採用します。詰み証明後は全合法初手を固定して再検証し、別解がない問題だけを保存します。条件は `--threat-turns`、`--min-legal-actions`、`--min-optimistic-score`、`--min-depth`、`--visible-prefilter-time-limit`、`--uniqueness-time-limit` で調整できます。`--uniqueness-max-depth 8` のように指定すると、各初手を問題の詰み深さから指定深さまで反復深化し、より長い別解も除外します。各初手では最初の `--uniqueness-positive-time-limit` 秒（既定2秒）で別解の正証明を高速に探し、残りの累積予算で全合法手・具体的めくれの不詰みを厳密検証します。各初手の累積予算は `--uniqueness-node-limit` と `--uniqueness-time-limit`、候補局面・誤答側の詰み探索は `--mate-jobs 16`、初手間のCPU並列数は `--uniqueness-jobs 16`（いずれも `0` は論理CPU数）で指定します。誤答時に相手の詰みまで成立することは既定の採用条件ではありません。必要なら `--min-losing-alternatives 1` 以上を指定します。

生成物は `depth_XX/<問題ID>/` に分類されます。`XX` はソルバー上の攻撃側手数深さです。各問題には局面情報 `problem.json`、検証済み代表手順 `answer.kifu`、代表手順とDAG状態を収める `strategy.json` が含まれます。DAG作成を有効にした場合は、攻撃側の証明手、守備側の全合法応手、公開カード補充と山札予約を含む全めくれ結果を保持し、同一局面をノードIDで共有します。既定ではDAG作成を試みますが、ノード・辺上限を超えても詰み判定と代表手順が検証済みなら保存します。完全DAGを必須にする場合は `--require-complete-dag`、DAG作成を省いて生成を優先する場合は `--no-strategy-dag` を指定します。DAGの有無と省略理由は `problem.json` の `strategy_dag` および `quality` に保存されます。`strategy.json` は既定で compact DAG を保存し、めくれカード集合は reveal group の bitset として厳密に残します。従来形式が必要な場合は生成時に `--strategy-dag-format v1`、比較用に両方残す場合は `--strategy-dag-format both` を指定します。めくれ候補は現在の山札だけから取り、同じレベル・点数・ボーナス・コストのカードは同型として代表だけを検証します。公開カード補充は一度 blank として進めた局面から反例になりやすい reveal を推定し、危険度の高い候補から検証します。非公開カードを即購入・即予約する oracle 手は合法手順DAGには出力されません。再現性を保つため、生成物内の SPN は伏せ予約カードを `?C<id>` 形式で保存します。通常の公開用 SPN における `?L<level>` と異なり、伏せ予約であることと実カードIDの両方を保持します。購入済みカードは `bought:[<id>,...]`、取得済み貴族は player section の `nobles:[<id>,...]` に保存します。

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
