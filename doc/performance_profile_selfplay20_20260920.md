# selfplay20 処理時間プロファイルと高速化の判断基準

計測日: 2026-09-20（日本時間12:06頃）。対象: 採用済みselfplay20の固定Python runtime＋既存csplendor。

この文書は `csplendor/doc/performance_profile_selfplay20_20260920.md` と
`dlsplendor/doc/performance_profile_selfplay20_20260920.md` に同一内容で保存する。
今後も両方を同時に更新し、末尾のcmpで一致を確認する。

## 1. 結論

- この12局面では、MCTS800＋追加rolloutの通常計時は平均 **708.56ms/着手**。
  詰み探索を実戦設定の20msにした別測定も平均710.34msだった。
- 独立した区間計測では、ネットワーク本体34.4%、特徴量9.6%、合法手ID取得8.0%、
  木の子選択12.5%、相手の戦術的脅威評価10.3%。
  **ニューラルネットと、通常MCTSのPython・binding周辺の両方に改善余地がある。**
- 追加rollout全体は17.65%。その内部の約58.2%がネットワーク本体。
  「追加rolloutだけC++化すれば全体が数倍速くなる」とは、この測定からは期待できない。
- NN単体ではbatch96のPolicy＋Valueが1 threadで6.760ms、4 threadsで3.865ms。
  約1.75倍だが、探索全体・自己対戦全体の高速化率ではない。
- 学習更新の計算形状を再現したメモリ上の試験では約88.58ms/更新。
  forward＋backwardで区間時間の約84.7%。探索移植だけでこの部分は速くならない。
- **棋力の比較・継続学習・高速化実装は今回行っていない。**
  改善案は以下の実測に基づく候補であり、改善後の速度や棋力を保証しない。

## 2. 実行環境・固定物

| 項目 | 内容 |
|---|---|
| CPU | AMD Ryzen 9 7900X、12 cores / 24 logical CPUs |
| OS kernel | Linux 7.0.0-31-generic |
| Python / PyTorch / NumPy | 3.12.1 / 2.10.0 / 2.2.6 |
| 実行device | CPU。今回の計測環境ではCUDA利用不可。GPU性能は未計測 |
| 並列度 | 子プロセスを作らず1 process。探索・更新はtorch intra/inter-opとも1。NN単体比較のみintra-opを4へ変更 |
| CPU割当 | affinity 0–23。特定core固定・core隔離・固定クロックはしていない |
| 実測中のhost CPU | busy 6.60%、iowait 7.52%（全24論理CPUを100%とした平均） |
| 計測部分のwall / process CPU | 69.761s / 70.470s |
| peak RSS | 856.48MiB（プロセス全体、モデル・データ・profilerを含む） |
| state / action dimension | 313 / 3133 |

過去に残存していた2本のSplendorベンチマークは停止済み。
開始前のホスト側プロセス名確認でも残存Python学習ジョブは見つからなかった。
通常計時の探索36回ではwall 25.508s / process CPU 25.506sであり、
この区間で大きなCPU待ち・I/O待ちは観測されていない。
ただしデスクトップ等と共用した環境で、厳密な専有CPUベンチではない。

固定物:

- checkpoint: `dlsplendor/models/selfplay20_verified/best.pt`
  SHA-256 `5fbf5e35ef6b8f2cd631c32117a95b8588f1cfa76498c97a56d8076477440b45`
- config: 同ディレクトリの `inference.yaml`
  SHA-256 `9341146f45082d4f0cc1eb68d6f1cb7e76548759a9676235b62b5af96d230fde`
- runtime: 同ディレクトリの `runtime/`。採用検証時のソース `be6e6a0ddae591e08173f5ea83ab5827ddb9ef7a` に対応。
  作業ツリーの後続実験コードとは混同しない。全Pythonファイルのhashはmanifestに記録。
- 実際のPython 3.12用native拡張:
  `_csplendor.cpython-312-x86_64-linux-gnu.so`
  SHA-256 `bd5630ab3bc3883b920c385e9e34d3a4aeb0838e843ccb7529b17f01e5d46593`
- csplendor作業ツリーHEAD: `181a7ce`。今回C++ソースの変更・再ビルドはしていない。
  既存binaryのコンパイラ・最適化フラグ・debug symbol有無は未検証。
- 入力archive: `dlsplendor/models/selfplay20_long_selfplay_refresh/selfplay.sqlite3`
  SHA-256 `b849357c23eae8d0311a31dabc094e05989c61d5f1c26baa8e5c740b2b1efc19`
- 実測時の計測スクリプトSHA-256: `c3b9acb97a6ca4c84a40a691fd0e1491bbe0a732643c35fd2e8815a8c45fb1f4`

モデルロード41ms、archive80局のread/decode307ms程度だった初回測定は
`profile_full_20260920/manifest.json` に記録。OS page cacheを消していないため、
cold disk I/O性能や完全なプロセス起動時間としては扱わない。
最終測定の正確なロード時間は `profile_detailed_20260920/manifest.json` を参照する。

## 3. 局面・予算・計測方法

自己対戦archive80局から、max(両者得点)を0–4 / 5–9 / 10以上の3phaseに分類し、
各phase×物理的手番から2局面、合計12局面を固定seed `2026092011` で選択した。
同じ層内では異なる対局を選ぶ。終局局面は除外し、勝敗や測定時間では選別しない。
実際の対局におけるphaseの出現頻度で重み付けした平均ではない。

| index | phase | 手番（0=先手 / 1=後手） | 得点 [先手,後手] | game index | sample index |
|---|---|---|---|---|---|
| 0 | 0 | 0 | [1,1] | 38 | 24 |
| 1 | 0 | 0 | [1,2] | 68 | 30 |
| 2 | 0 | 1 | [0,0] | 64 | 1 |
| 3 | 0 | 1 | [4,2] | 34 | 37 |
| 4 | 1 | 0 | [9,8] | 58 | 54 |
| 5 | 1 | 0 | [3,7] | 74 | 48 |
| 6 | 1 | 1 | [4,7] | 75 | 51 |
| 7 | 1 | 1 | [5,5] | 49 | 45 |
| 8 | 2 | 0 | [9,11] | 76 | 56 |
| 9 | 2 | 0 | [10,8] | 42 | 50 |
| 10 | 2 | 1 | [14,7] | 13 | 53 |
| 11 | 2 | 1 | [14,8] | 12 | 51 |

snapshotと元の対局内容のhashはmanifestに保存。
36回という回数は **12局面×3反復** であり、36個の独立局面ではない。

共通条件:

- MCTS 800 simulations。追加rolloutは最大6候補×16未知配置×48継続遷移。
  貴族選択等も含む「状態遷移」で、常に48通常手番という意味ではない。
- 各検索で新しいMCTSを作り、木・推論cache・詰みsessionの局面間再利用なし。
  NN自体はロード後warm-up済み。
- root noise、playout cap randomization、stall guardを無効化。探索全体の時間制限なし。
  戦略候補・予約評価等、その他の設定は固定設定を利用。
- 主測定では詰み探索の時間上限だけ0（時間による中断なし）にし、
  深さ1–3・20,000 nodes等は維持。**詰み探索を無効にした意味ではない。**
  profilerの遅さで探索結果が変わるのを避けるため。
- 別passで詰み時間上限を設定通り20msに戻して測定。12局面とも詰み証明による早期終了はなし。
  詰みが多い局面群の代表値にはならない。
- 序・中・終盤の3局面をwarm-upし、通常計時は各反復で局面順をseed付きで変更。
- 通常計時、区間タイマー、cProfile、rolloutなし、詰み20msを別passで実行。
  いずれも検索内のwall timeを測り、モデルロード・MCTS作成・snapshot復元・結果保存は除外。
  cProfile行にはその結果集計の小さな追加負担も含む。
- 保存入力に上書きなし。archiveはSQLite `mode=ro&immutable=1` で開く。
  そのため計測中に別writerがarchiveを変更しないことが前提。
- 通常計時と区間計測・cProfileで、着手、訪問回数、root Q、
  rollout return、詰み判定等のsignatureを比較。**不一致0**。
  全検索で元の局面が不変、返した着手が合法であることも確認した。

## 4. 探索全体の所要時間

| 実行 | 測定回数 | 平均ms/局面 | 中央値ms | 最小–最大ms |
|---|---|---|---|---|
| 通常計時・固定ノード予算 | 36 | 708.56 | 697.15 | 398.24–934.33 |
| 詳細区間タイマー | 12 | 802.47 | 784.27 | 462.41–1056.26 |
| cProfile（参考） | 12 | 1134.56 | 1171.05 | 758.46–1422.69 |
| 追加rolloutなし | 12 | 582.46 | 589.33 | 295.98–834.18 |
| 詰み探索20ms・通常計時 | 12 | 710.34 | 688.31 | 401.61–946.95 |

区間タイマーは通常計時平均比 **+13.25%**、
cProfileは **+60.12%** の負担がある。
したがって、以下の区間内訳をそのまま非計測時の厳密な割合とは扱わない。
将来の高速化率は必ず通常計時同士で比較する。

| phase | 通常計時平均ms | rollout評価局面数/検索 | MCTS leaf batch回数/検索 | leaf batchサイズ平均 |
|---|---:|---:|---:|---:|
| 0（0–4点） | 855.16 | 2104.50 | 31.25 | 31.39 |
| 1（5–9点） | 676.03 | 817.25 | 29.25 | 29.37 |
| 2（10点以上） | 594.48 | 371.25 | 44.00 | 19.00 |

終盤では継続対戦が早く終わり、rolloutの実評価数が少ない。
leaf batchはMCTSの統計であり、補充候補のValue評価や追加rolloutを含む全NN呼出数とは違う。

## 5. 探索の処理時間内訳

区間タイマー12局面の合計は9.628s。
親区間は子区間を含むため、**以下は親子の重複を除いた分類**。
NN本体にはPyTorchの演算kernelとモデル内Python制御の両方が含まれ、
「PythonからC++へ移植すれば全部消える時間」ではない。

| 処理（重複なし） | 12局面の合計秒 | 1局面あたりms | 構成比 |
|---|---|---|---|
| ネットワーク本体 | 3.309 | 275.72 | 34.36% |
| 推論前後処理 | 0.171 | 14.27 | 1.78% |
| 特徴量作成 | 0.925 | 77.05 | 9.60% |
| 合法手ID取得 | 0.771 | 64.26 | 8.01% |
| 木の子選択（dynamic cpuct含む） | 1.206 | 100.54 | 12.53% |
| 相手の戦術的脅威評価 | 0.992 | 82.67 | 10.30% |
| 着手デコード・木への適用 | 0.392 | 32.63 | 4.07% |
| 木の値の逆伝播 | 0.059 | 4.94 | 0.62% |
| 詰み探索入口 | 0.021 | 1.79 | 0.22% |
| その他の探索制御 | 1.782 | 148.47 | 18.50% |

補足:

- 子選択1.206sのうち `_dynamic_cpuct_scale` は0.650s。両方を足してはいけない。
- NN本体3.309sの内訳はtrunk約2.040s、Policy生成約1.077s、
  探索Value補正約0.072s、その他約0.120s。Policyにはeconomic card adapterを含む。
- 12局面で特徴量encode 64,263回、合法手ID取得44,890回、
  木への着手適用72,920回、脅威評価49,865回、子選択25,732回。
  木の逆伝播は9,600回で800×12と一致。
- NN評価の呼出数は、通常MCTS側721 batch、rollout側195 batch。
  NNに渡す局面をまとめる処理は既に存在する。
- その他には木の構築・展開、確率分岐、候補手の計画、局面操作、
  rootの選択処理、タイマー自身の負担などを含む。これを全部「Python overhead」と断定しない。

### 5.1 追加rolloutだけの内訳

| rollout内の処理 | 合計秒 | rollout内の構成比 |
|---|---|---|
| ネットワーク本体 | 0.989 | 58.21% |
| 推論前後処理 | 0.099 | 5.85% |
| 特徴量作成 | 0.186 | 10.94% |
| 合法手ID取得 | 0.237 | 13.97% |
| 着手デコード | 0.026 | 1.53% |
| その他の進行管理 | 0.162 | 9.50% |

rolloutは全体の17.65%。
MCTS800のままrolloutを外した通常計時は708.56→582.46ms（約17.8%短縮）だった。
これは機能を外した対照測定で、**同じ棋力を保った高速化ではない**。

この構成比をそのまま使った上限の例:

- rollout全体がゼロ時間になったとしても全体は約1.21倍。
- rollout内のNN本体を残し、それ以外をゼロ時間にしても約1.08倍。
- 実際のC++移植では処理時間はゼロにならない。これらは予測値でも達成値でもなく、
  対象範囲だけを速くした場合のAmdahl型の上限例。

## 6. Python / native境界のmicrobenchmark

6局面（各phase×手番から1局面）で20回warm-up後、
200回×5反復。表は各局面の中央値を平均した値。
検索中とはcache状態・呼出し方が異なるため、この値×検索call数で
探索全体を再構成したり高速化率を保証したりしない。

| 処理 | 平均µs/回 | 局面別中央値の最小–最大µs |
|---|---|---|
| 局面clone_light | 0.251 | 0.237–0.259 |
| 合法手Action一覧（warm） | 4.008 | 0.429–6.633 |
| 合法手ID一覧（Python wrapper） | 11.894 | 1.010–19.507 |
| 313次元の全特徴量（wrapper） | 13.272 | 12.904–14.728 |
| 196次元の基本特徴量のみ（native） | 2.542 | 2.515–2.592 |
| 合法性確認付きAction decode | 0.847 | 0.822–0.895 |
| clone＋decode＋着手適用 | 1.400 | 1.382–1.420 |
| 未知配置のshuffled_clone | 1.576 | 1.558–1.603 |

特に注意:

- 313次元wrapperと196次元nativeは**同一処理ではない**。
  wrapperには公開カード統計117次元、型・shape検査、配列結合もある。
  この比率を「C++化で5倍」と解釈してはいけない。
- 合法手数は対象6局面で1 / 27 / 27 / 25 / 25 / 41。
  合法手が1個の局面を含むため、単純平均を全局面に当てはめない。
- `legal_actions` はwarm状態のbinding呼出し。
  C++内部の合法手生成本体とPythonオブジェクト生成を分離した値ではない。
- 現在の `ActionEncoder.legal_action_ids` はAction一覧をPythonに取り出し、
  各Actionをもう一度C++のencodeへ渡す。
  **合法IDを一括で返すAPI、特徴量と合法IDをまとめて返すAPI**は試す価値がある。
- `clone_light` 単体は約0.25µs。この局面群では、
  cloneだけの最適化よりも繰り返し評価・一括処理の優先度が高い。

## 7. ニューラルネット推論

12局面の特徴量・合法手集合を繰り返して指定batchを作成。
入力作成は計時外。各条件3回warm-up＋9回通常計時の中央値。
`_predict_states_uncached` を直接使い、cache hitを速度改善と取り違えない。
mask作成・合法手に絞った出力の取り出しを含む。GPU転送なし。

| batch | Policy＋Value・1 thread (ms) | Policy＋Value・4 threads (ms) | Valueのみ・1 thread (ms) | Valueのみ・4 threads (ms) |
|---|---|---|---|---|
| 1 | 1.159 | 1.164 | 0.468 | 0.467 |
| 16 | 2.196 | 1.943 | 0.852 | 0.638 |
| 32 | 3.035 | 2.642 | 1.200 | 1.033 |
| 96 | 6.760 | 3.865 | 2.641 | 1.401 |
| 128 | 8.573 | 4.789 | 3.306 | 1.754 |

- batch1のPolicy＋Valueはthreadsを増やしてもほぼ改善しない。
- batch96では4 threadsが約1.75倍だが、使うCPU資源も増える。
  自己対戦を多processで実行する場合に全workerを4 threadsにすると過剰並列になり得る。
  **実戦の1着手latencyと、自己対戦全体のpositions/secは別の評価軸。**
- NN batch拡大は1局面あたりのコストを下げるが、
  探索結果を逐次反映する頻度も変わる。バッチを大きくするだけで棋力が維持されるとは限らない。
- Value-onlyが必要な箇所ではPolicy全体を計算しない方針が既に使われている。
  rollout打ち切り等に未使用のPolicy計算がないかも候補になるが、今回は変更していない。

torch.profilerによるbatch96・3回の別測定では、
`aten::addmm`（線形層の行列演算）がself CPU時間11.498ms、
次いでsub 1.155ms、mul 0.961ms、fill 0.908ms、bmm 0.841ms。
これはoperatorごとのCPU自己時間で、Python制御を含むwall timeの完全な内訳ではない。
operatorのinclusive時間とself時間を混ぜて加算しない。

## 8. 学習更新・データ準備

継続学習ではなく、ロードしたNNの**使い捨てコピー**による計測。
元モデル、best、採用設定、archiveは変更していない。

- archive先頭8局・450局面を使用。既存の軌跡・終局検証、tensor化を実行。
- 前回と同じ更新対象58 tensors（残差block末尾2個＋対象head等）、BN・dropout固定。
- 256新規＋128 anchorのbatch384、同じloss構成・AdamW・勾配clip。
- 旧anchor用データは同じ8局の別sampleで代用。
  **計算形状の再現であり、前回学習のデータ分布・収束性の再現ではない。**
- warm-up3更新、通常区間計測12更新、operator計測1更新。checkpoint保存なし。

データ準備30.74ms、anchor事前推論49.63ms。
更新の外側wallは平均88.58ms、
中央値87.85ms、範囲84.18–95.28ms。
下表の区間合計は平均86.96msで、区間外の一時tensor解放等とはわずかに異なる。

| 更新内の処理 | 1更新あたりms | 区間内構成比 |
|---|---|---|
| forward | 38.129 | 43.85% |
| backward | 35.497 | 40.82% |
| loss・anchor計算 | 7.334 | 8.43% |
| AdamW更新 | 3.462 | 3.98% |
| サンプリング・batch作成 | 1.529 | 1.76% |
| 勾配clip | 0.744 | 0.86% |
| zero_grad | 0.157 | 0.18% |
| その他 | 0.105 | 0.12% |

別operator計測1更新では `aten::mm` 19.705ms、`aten::addmm` 19.609msが大きい。
探索をC++化しても、このforward/backward自体は変わらない。

学習ループ全体について:

- 自己対戦データ生成・再解析・評価対局では、着手ごとの探索高速化が効く。
- 純粋な重み更新にはNN演算、batch、メモリ転送、deviceの最適化が必要。
- 過去の80局生成972秒等は他ジョブとCPUを共用した値なので、
  今回の単process値と割り算して新しい高速化率を作らない。
- 今回はarchive書込み、全データセットのvalidation、checkpoint保存、
  多process/Rayの通信・待ち時間を含む「学習1サイクル全体」は未計測。

## 9. ホットスポットから選ぶ改善方針

| 観測された負荷 | 最初に試す方針 | 主な変更先 | 必須の確認 |
|---|---|---|---|
| 子選択・chance Qの反復計算 | 同じ選択内でQの再計算を共有。必要なら同一仕様のC++選択処理 | dlsplendor → 必要箇所だけcsplendor | 訪問回数、root Q、tie-break、chance確率、乱数消費が一致 |
| 相手の脅威評価10.3% | プレイヤー状態・貴族情報をまとめて取得し、重複計算削減。native一括評価を検討 | 両repo | 公開情報のみ使用。得点・予約・貴族の意味を変えない |
| 特徴量＋合法ID17.6% | 全313次元特徴量・合法IDの一括返却、配列再利用、Action往復削減 | csplendor API＋dlsplendor呼出側 | 同一shape/dtype/schema、合法ID集合・順序・重複なし |
| NN本体34.4% | まずthread数・batch配置。次に演算融合/compile・推論backend・利用可能ならGPU | dlsplendor | wall latencyと総throughputを別々に計測。数値差・着手差・棋力確認 |
| rollout進行管理 | 局面群をC++に保持し、1層分をまとめて処理してNN batchを要求 | 両repo | 共通乱数、観測者、未知配置、終局・打切りValueの同等性 |
| clone単体 / 今回の詰み時間 | 今回は優先度を下げる。詰み多発局面群は別途計測 | csplendor | 局面分布を変えた再測定なしに一般化しない |
| 学習のforward/backward約84.7% | NN演算・batch・deviceの最適化 | dlsplendor | 更新範囲・BN固定・loss・精度維持 |
| 実運用で通信/待ちが大きい場合 | 推論要求の集約、worker数・thread数調整 | dlsplendor実行基盤 | 今回は未測定。まず通信・待ち時間を追加計測 |

コード上でも `ChanceNode.q_value` がoutcomeの重み付き和を再計算し、
子選択とdynamic cpuctの両方から参照されることを確認した。
cProfile参考call数は `_value_from_parent` 約486万回、
native `encode` 約137万回 / 12局面。
ただし次節の不整合があるため、これらは探索候補を探す参考値であり、
保証されたcall数や速度改善量の根拠にはしない。
子選択と脅威評価の優先度は、独立した区間タイマーでも確認できている。

csplendorには既存native MCTS・GILを解放する並列探索APIもあるが、
selfplay20の全てのchance処理・戦略候補・rolloutと同等とは確認していない。
単に既存APIへ切り替えて同じ棋力になるとは扱わない。

## 10. 計測の限界・異常の扱い

1. **区間タイマーの負担は約13.3%。** 内訳は判断用の概算であり、
   公式な高速化率はprofilerなしの反復測定で決める。
2. **cProfileの一部entryに不整合。** `decode` のprimitive call数0、
   一部関数でcumulative < selfを検出。
   rootごとに独立Profileを作って集計しても最終測定では5 entryで残った。
   原因は未特定。生データと `cprofile.invalid_entries` を残し、
   cProfileの累積時間を正式内訳・加速率の計算には使わない。
   詳細区間とtorch operator計測はcProfileとは別passで実行した。
3. torch.profilerはCPU指定でもGPU検出warning/errorを表示した。
   CUDA計測はしていない。CPU結果は取得でき、全計測プロセスはexit code 0。
   GPUが存在しない・GPU計算が失敗したという結論には使わない。
4. pybind境界の時間はC++内の詳細な関数別CPU時間ではない。
   C++内を掘る場合は、build flagとsymbolを確認してから
   `perf` 等のsampling profiler、またはnative区間タイマーが必要。
   今回はそれらを実行しておらず、cache miss、分岐予測、SIMD効率等は未測定。
5. 12局面・CPU1process・固定探索予算の結果。全局平均、P95/P99の実戦応答、
   GPU、複数worker、木の再利用のある連続対局、詰み高頻度局面への一般化はしない。
6. 現在の採用実装は探索全体に時間制限があると追加rolloutを省く。
   実戦の時間制限内で強化探索を使うには、残り時間への予算配分と中断処理も別途必要。
7. 高速化と棋力改善は別。未知情報の漏洩、乱数の共通条件の崩れ、打切りの扱い変更で
   速く見せていないかを検査する。

## 11. 再現方法・次回の比較手順

計測器:
`dlsplendor/scripts/profile_search_pipeline.py`。
テスト:
`dlsplendor/tests/test_profile_search_pipeline.py`。

以下は `dlsplendor/` を作業ディレクトリとして実行。
`--output` は必ず未使用のパスにする。既存パスへの上書きは拒否する。
採用済みpackageとarchiveはローカル資産であり、Gitだけのcloneには含まれない。

```bash
/home/kuboyu/.pyenv/versions/3.12.1/bin/python scripts/profile_search_pipeline.py \
  --runtime models/selfplay20_verified/runtime \
  --config models/selfplay20_verified/inference.yaml \
  --checkpoint models/selfplay20_verified/best.pt \
  --archive models/selfplay20_long_selfplay_refresh/selfplay.sqlite3 \
  --output models/selfplay20_search_diagnostics/profile_next_run \
  --roots 12 --repeats 3 --simulations 800 \
  --micro-iterations 200 --training-steps 12
```

これは学習を再開するコマンドではない。計測用コピーの更新結果は保存しない。
ただしCPUを使用するため、性能比較時には他の学習・評価ジョブを止めた状態を選ぶ。

次回は:

1. まず現行版で同じsnapshot・model/config・seed・thread数を再測定する。
2. 最適化版のPython runtimeを別ディレクトリで固定して、`--runtime` のみ切り替える。
   同じcheckpoint/config/archiveのhashと、選択snapshotのhashが一致することを確認。
3. native変更時は旧版・新版binaryのhashとbuild flagsを記録。
   実行中にライブラリを書き換えず、別環境/プロセスで比較する。
4. **baseline行の局面ごとの時間比**を比較し、phase・先後・合法手数別にも退行を確認。
   実運用上重要な場合は局面数と反復数を増やし、実行順もA/Bで交互にする。
5. 計測器内のsignature比較は同一runtimeの「計測あり/なし」の比較。
   **異なる実装間の同等性は別途、双方の出力signatureを比較する必要がある。**
   浮動小数点誤差を許容する場合は、許容幅と不一致の扱いを先に定める。
6. 同等な最適化でない場合は、同一時間・先後交換の棋力検証を別途実施。
   自己対戦はgames/hour、positions/sec、学習はupdates/sec、実戦は着手latencyを使い分ける。
7. 入力・runtime不変、子プロセスの終了、両docの一致を確認して終了する。

## 12. 生データ・検証記録

最終計測:
`dlsplendor/models/selfplay20_search_diagnostics/profile_detailed_20260920/`

| ファイル | SHA-256 |
|---|---|
| inference.json | `1fe6f7e2d63fdba1d5549aebfd916ad023163fe7f74d873d4c89bc38fd367bd5` |
| manifest.json | `128f82391808b4a3143722ea172ff0c6e02a010a9e33b1c2412ab5219138afcc` |
| native_micro.json | `07092944aed16a5047d252fe4746ad7943a3815c347ba0a85f41ba0c9dd3b5d1` |
| search.cprof | `fb9af673ae67ba62dd849bc91f9b7f8298bd9e1418bfd448c4d4e1ccf30f6bb0` |
| search.json | `0fcb8485f92a8a5de8e449de1e7518c47d975679acf5daeb45a55c3a5cd1c518` |
| training.json | `bf72d8359dffa2b9064f81aa41916d9c0c138ebb8ce0ba091b95fb6cb8cee505` |

`completion.json` に全出力hash、入力不変、runtime不変、
signature不一致0、CPU/RSS・終了時情報を記録。
cProfile生データは `python -m pstats <path>/search.cprof` で確認できるが、
前述の整合性制約を必ず適用する。

実測時の計測器を同じディレクトリの `measurement_script.py` にhash一致で保全した。
厳密に同じ計測器を再実行する場合は、再現コマンドのscript部分をこのファイルに替える。
リポジトリ側の計測器には実測後、cProfile不整合の警告表示と
`completion.cprofile_accounting_valid` を追加した。探索・区間計時・更新の内容は変更していない。
この保全ファイルは実測後に追加したため、上表の完了時出力一覧には含まれない。

初回の小規模動作確認は `profile_smoke_20260920/`、
最初の本計測は `profile_full_20260920/` に残した。
正式な表は最終の `profile_detailed_20260920/` に基づく。
初回本計測の通常平均713.12msと最終708.56msは約0.6%差。
生データはmodels配下にのみ保存し、モデル・archive・profilerバイナリをGitに追加しない。

確認済み:

- dlsplendor: `python -m pytest -q` → 494 passed（8.35秒）。
- csplendor: encoders / encoding_schema / information_state / game_snapshot の対象テスト → 24 passed。
- 新規計測器の14テストには区間の非二重計上、例外時の計測解除、
  入力局面の層化、出力上書き拒否、cProfile集計異常の検出を含む。
- 計測プロセスはすべて終了。新しい常駐worker・学習プロセスは起動していない。
- 今回の変更対象は計測スクリプト・テスト・この文書のみ。
  両repoに元からあった作業ツリー変更は保持する。

両文書の一致確認（どちらのrepoを作業ディレクトリにしても同じ）:

```bash
cmp ../csplendor/doc/performance_profile_selfplay20_20260920.md \
    ../dlsplendor/doc/performance_profile_selfplay20_20260920.md
```
