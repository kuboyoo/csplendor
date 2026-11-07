# 先読みの並列化の設計（対人戦の待ち時間短縮、2026-11-08）

[ドキュメント索引](index.md) / [詰み探索の使い方](mate_usage.md) / [V3 多対局探索](mcts_v3.md) / [C++ 版 df-pn の設計](mate_dfpn_cpp_design.md)

目的は**学習の高速化ではなく、対人戦でユーザーが待つ時間を短くする**ことである。対象は 3 つの対人戦の経路
（dlsplendor の USI/ローカルエンジン、bgasplendor のホスト、splendorgui のブラウザ WASM）で、棋力は落とさない。
本書は設計と実装計画だけを扱い、実装は含まない。計測は Ryzen 9 7900X（12C/24T）＋ RTX 5070 Ti で行い、
最終的には M4 MacBook Pro と iPhone（どちらも主にブラウザ経由）で検証する。

## 0. 結論

1. **1 手の時間は NN 推論と詰み探索でほぼ決まり、木の操作は 1 探索あたり約 4 µs（800 探索で 3〜4 ms、25,600 探索で約 110 ms）で小さい**
   （第 1 節）。したがって「木を降りる処理を複数スレッドで行う」並列化（共有木・virtual loss・lock-free）は、
   対人戦の待ち時間をほとんど縮めない。5070 Ti＋TensorRT のエンジン経路は、`feature/engine-pipeline`（束 2 つの先行）で
   すでに GPU の処理量の上限（64 行で約 7 万行/秒、3,584 行でも約 8 万行/秒）に達している。
2. **詰み探索は 1 スレッドで、しかも MCTS と直列に動いている。** 終盤の 1 手では root の探り（最大 150 ms）と準詰み（最大 150 ms）が
   1 秒の手の最大 30% を占める。推奨は (a) **詰み探索を MCTS と同時に走らせる**（別スレッド／別 Worker。探索木の変更なし）、
   (b) **C++ の中で root 分割を 2 段のタスク列と共有置換表で行う**（第 2 節）。(a) で終盤の 1 手が最大 0.3 秒短くなり、
   (b) で同じ予算で深さ 4 の厳密探索が届くようになる（棋力は上がる方向）。
3. **MCTS は、木の並列化ではなく「NN の束を大きく・待ちを重ねる」ことで時間を縮める。** これは dlsplendor-e2 の
   `feature/engine-pipeline`（衝突の再試行・`max_in_flight`）そのものである。並列探索の担当は `src/mcts_v3.h` を触らず、
   root 並列が要るときも既存の多対局 `num_threads` 経路（同じ局面を seed 違いで K slot 登録）で足りる（第 3 節）。
4. **ブラウザ（M4・iPhone）は NN 推論が支配的**（13.9M パラメータ網は CPU 8 スレッドでも 1 行 2.4 ms。宝神の h128l3 でも
   1 行 0.1 ms 台と見込む）。csplendor.wasm に pthreads を入れる前に、(a) 詰み探索を**別 Worker の wasm インスタンス**で
   同時に走らせる（SharedArrayBuffer 不要）、(b) ORT の wasm スレッド数と WebGPU（iOS 26 の Safari は WebGPU を既定で
   有効）を端末ごとに実測して選ぶ（第 5 節）。pthreads 版 wasm は第 3 段で、COOP/COEP の付いた `/play/game` だけで使う。

## 1. 現状の把握

### 1.1 対人戦の 3 経路の流れ

| 経路 | 1 手の流れ | NN | スレッド |
|---|---|---|---|
| dlsplendor エンジン（`dlsplendor/engine/thinker.py`、USI/WebSocket） | `RootMateSearch.probe`（`root_mate_probe`）→ 束 2 つの先行つき MCTS（`ROWS=64`、`ROUND_SIMULATIONS=256`、`IN_FLIGHT=2`）→ `near_mate_override` | TensorRT fp16 または torch.compile bf16、CUDA graph、cuda 固定（cpu/mps の経路なし） | 木 1、詰み 1（直列） |
| bgasplendor ホスト（`host/thinker.py`） | `probe`（0.15 s）→ `V3SearchSession`（葉 8、`ROUND_SIMULATIONS=256`）→ `near_mate_override`（0.15 s） | torch fp32、CUDA graph 8 行 | 木 1、詰み 1（直列） |
| splendorgui（`src/worker/controller.ts`、Web Worker 1 つ） | `WebSearch.begin/collect/features/apply`（宝神 800 探索・葉 8、宝極 3,200、宝凰 20,000・葉 64）→ 宝神だけ `rootMateProbe(150 ms)` | onnxruntime-web。`wasm`（スレッド数 = 分離時 min(4, hardwareConcurrency)、Apple モバイル・低メモリは 1）と `webgpu`（Apple モバイルでは禁止）を実測で選ぶ | Worker 1（wasm は非 pthread、SIMD あり） |

探索木はどの経路も 1 局 1 木・1 スレッド（`V3SearchConfig.num_threads=1`）。詰み探索はどの経路も MCTS の前後に同じスレッドで
同期的に呼ぶ。dlsplendor の時間配分は、締め切りの余白 min(5%, 150 ms)、詰み探索 min(max(150 ms, 2%), 15%)、
準詰み min(150 ms, 10%)。

### 1.2 時間の内訳（Ryzen 9 7900X＋RTX 5070 Ti、2026-11-08）

計測スクリプトは scratchpad の `bench_tree_ops.py`・`bench_nn.py`・`bench_mate.py`（一時物。`ulimit -v`・`nice` つき、CPU のみ。
GPU の値は dlsplendor `doc/local_engine_20261106.md` の 11-06 の計測を引く）。

**木の操作だけ**（`V3SearchSession`、一様事前確率、序盤・20 手目・40 手目の 3 局面、1 スレッド）:

| 探索数 | 葉 | collect＋apply | 1 探索あたり | 束数 | 束あたりの行 |
|---|---|---|---|---|---|
| 800 | 8 | 3.3〜3.8 ms | 4.1〜4.7 µs | 101〜103 | 7.8〜7.9 |
| 800 | 64 | 2.6〜3.0 ms | 3.2〜3.8 µs | 15〜22 | 36〜53 |
| 3,200 | 8 | 11.7〜14.5 ms | 3.6〜4.5 µs | 401〜403 | 8.0 |
| 3,200 | 64 | 11.1〜13.4 ms | 3.5〜4.2 µs | 54〜60 | 53〜59 |
| 20,000 | 64 | 83〜95 ms | 4.1〜4.8 µs | 316〜322 | 62〜63 |

`apply`（逆伝播）は全体の 5% 以下。葉 64 でも衝突（評価待ちの葉に再び当たる）で束は 36〜63 行に留まる
（`collision_retries` はこれを埋める）。

**NN 推論 1 束**（e2c_cq_all、entity_v2 384×8、13.9M パラメータ、316 次元）:

| 経路 | 8 行 | 64 行 | 256 行 | 行あたり（64 行） |
|---|---|---|---|---|
| 5070 Ti TensorRT fp16（dlsplendor 11-06） | 0.72 ms | 0.92 ms | 3.03 ms | 14 µs |
| 5070 Ti torch compile bf16＋graph（同） | 0.52 ms | 1.25 ms | 3.60 ms | 20 µs |
| CPU fp32 torch 1 スレッド（本計測） | 61 ms | 517 ms | — | 8,080 µs |
| CPU fp32 torch 4 スレッド | 29 ms | 224 ms | — | 3,500 µs |
| CPU fp32 torch 8 スレッド | 22 ms | 153 ms | — | 2,390 µs |

**1 手の合計**（dlsplendor 11-06、25,600 探索・葉 64）: TensorRT＋束 2 つの先行で **0.376 s**（71,800 探索/秒）。
内訳の見積もり: GPU 推論 400 束 × 0.92 ms ≈ 0.37 s、木 ≈ 0.11 s、両者が重なって GPU 側で律速。
3,200 探索は 0.049 s。bgasplendor（torch fp32、葉 8、束 1 つ）は 25,600 探索で 3.26 s（9,100 探索/秒）。

**詰み探索**（BENCH 局面、攻め側 42 手、Python の root 分割 `search_reveal_verified_mate_depths(jobs=…)`）:

| | jobs=1 | 2 | 4 | 8 | 12 |
|---|---|---|---|---|---|
| 深さ 3（11,535 ノード） | 0.01 s | 0.03 | 0.02 | 0.03 | 0.04 |
| 深さ 4（148.5 万ノード） | **1.30 s** | 1.16 | 0.89 | 0.80 | **0.79（1.65 倍）** |

`root_mate_probe`（既定、時間 150 ms）は同局面で 9〜10 ms（11,539 ノード、深さ 3 まで不詰み）。
現行の root 分割は 12 スレッドで 1.65 倍にしか伸びない。原因は枝（root の手 × めくれ）の大きさの偏りで、
不詰みの反証は「攻め側の全手にそれぞれ 1 つの反証応手」を要し、最も重い枝が全体の時間を決める（第 2.2 節）。

**まとめ**: 1 手 1 秒の設定で、GPU エンジンでは MCTS が 0.55〜0.7 s、詰み探索が終盤で 0.3 s（序盤は 10 ms）、木の操作は
MCTS の中の 2 割以下で GPU と重なっている。並列化で縮むのは詰み探索の分であり、MCTS 側は GPU の処理量が上限である。

## 2. 詰み探索の並列化

### 2.1 候補の比較

| 方式 | 内容 | 健全性 | 決定性 | 伸び | 実装量 |
|---|---|---|---|---|---|
| A. root 分割（現行を C++ スレッドへ） | root の手 × めくれを枝にして配り、枝ごとに別の表 | 各枝は完全な探索なので健全 | ノード上限なら決定的 | 1.6 倍（偏りで頭打ち） | 小 |
| **B. 2 段タスク列＋共有置換表**（推奨） | root と 1 手先（守り側の応手 × めくれ）までを仕事の単位にして work-stealing、決着した項だけを共有表に書く、OR で最初の証明／AND で最初の反証が兄弟を取り消す | 表には決着値だけ、1 項の書き込みは原子的 → 健全 | 判定は決定的、ノード数・主手順・予算内で届くかは非決定 | 4〜6 倍（8 スレッド） | 中 |
| C. Lazy SMP 風 | 同じ root を全スレッドが別順序で探索し表を共有 | 健全 | 非決定 | 不明（詰将棋の経験では 2〜3 倍） | 小 | 
| D. AND/OR 木の全深さ並列 | 任意の節点で子を配る | 健全 | 非決定 | B と同程度、粒度が細かく同期が増える | 大 |
| E. 並列 df-pn（SPDFPN、Kaneko 流） | pn/dn の表を共有し virtual proof number で分散 | 健全（表は見積もり） | 非決定 | 文献では 4〜8 倍 | 大（df-pn 自体が既定でない） |

**B を推奨する。** 理由は 3 つ。(1) いまの root 分割の弱点は偏りだけなので、仕事の単位を 1 段深くし動的に配れば直る
（1 段目 100〜200 個、2 段目 数千個の仕事）。(2) 置換表の共有は、現行の「枝ごとの表」で失っている重複（同じ局面に別の枝から
到達する。`memo_hits` のうち枝をまたぐ分）を取り戻し、メモリ予算をジョブ数で割らずに済む。(3) 決着値（PROVEN/REFUTED）
だけを共有するので、どのスレッドが先に書いても真偽は同じで、健全性の証明が「1 項の書き込みが原子的」の 1 点に収まる。
C は Splendor の太い木（1 段 60〜130 倍）では重複した探索が多く、E は df-pn が DFS より遅い現状（0.7〜0.9 倍）では
土台から直すことになる。

### 2.2 B の設計

- **仕事の単位**: `solve_reveal_verified_root_split_cpp` が返す辺（`RevealVerifiedFrontierEdge{action, reveal, depth, child}`）を
  1 段目とし、守り側の局面（1 段目の子）でもう一度同じ分割を行った辺を 2 段目とする。2 段目の子（攻め側の局面、残り深さ d−1）
  が 1 つの仕事で、既存の `RevealVerifiedSolver` をそのまま呼ぶ。深さ ≤ 2 なら分割しない（現行 `parallel_min_branches`）。
- **順序**: 1 段目は現行の順序（`proof_ordered_actions`、証明の移植、`preferred_attacker_actions`）、各 1 段目の中で 2 段目も
  現行の守り側の順序（反証しやすい応手を先に）。スレッドは「最初の 1 段目の、最初の 2 段目」から取る（LIFO ではなく順序どおり）。
  これで 1 スレッドのときの訪問順が現行の DFS と同じになり、`jobs=1` はビット一致の既存経路に落とす。
- **打ち切り**: 1 段目の手 m の下で 2 段目の 1 つが REFUTED になったら、m の残りの仕事を取り消す（AND の反証）。1 段目のどれかが
  PROVEN になったら root は PROVEN で全部を取り消す（OR の証明）。取り消しは既存の `RevealSearchCancellationToken` を仕事ごとに
  階層化する（root token → 手 m の token → 仕事の token）。
- **共有置換表**: `FlatMemoTable`（64 バイト・1 スロット、開番地法）を並行版にする。読みは鍵の比較、書きは「空スロットへの
  CAS で鍵を確保 → 項を書く → 鍵の『完成』ビットを立てる」の 3 段。読み手は完成ビットの立った項だけを採る。書くのは決着した
  項だけ（現行の DFS も未決は保存しない）。同じ鍵に 2 スレッドが同時に書く場合、どちらも同じ真偽なので後の書き手は捨てる。
  容量は探索の開始時に固定し（`memo_cap_` と同じ）、満杯では書かない（現行の flush と同じ意味で、健全性に影響しない）。
  移植の表（`solver_line_table.h`）は手の順序のヒントなので、スレッドごとに持つ（共有しない。結果の真偽に関わらない）。
- **統計**: `nodes`・`memo_hits` は原子加算でなく、スレッドごとに持って終わりに足す。`stats_.legal_moves` などは同じ。
- **メモリ**: 表は 1 つなので `solver_memory_budget_bytes(available, jobs=1)` を使い、スレッドごとの作業領域
  （`RecursionPath`・`legal_` など数 MB）だけを jobs 倍する。ブラウザは JS 側の予算をそのまま 1 表に渡す。
- **入口**: `search_reveal_verified_mate_depths(jobs=N)` の `_solve_depth_parallel`（Python の `ThreadPoolExecutor`）を、
  C++ の `solve_reveal_verified_mate_parallel_cpp(game, attacker, depth, jobs, …)` の 1 回呼び出しに置き換える。
  `MateSearchSession` / `root_mate_probe` / `near_mate` も同じ関数を使う（`RootMateProbeConfig.jobs` を追加）。
  Python の分割（`_solve_parallel_branch`・`_solve_positive_portfolio`）は C++ 版が第 6 節の検証を通ったら削除する（後方互換は持たない）。

### 2.3 健全性と決定性の扱い

- **健全性**（判定が変わらない）: 共有表に載る項は「完全に探索して決着した (局面, 残り深さ)」だけで、どのスレッドの探索も
  単独で完全なので、1 項の書き込みが原子的なら、共有表のある探索は「ヒットした部分木を省略した 1 スレッドの探索」と同値。
  予算切れ（ノード・時間・取り消し）は UNKNOWN で、表に書かない。検証は第 4 節。
- **決定性**: 判定（mate / no_mate / unknown のうち mate と no_mate）は入力だけで決まる。ノード数・`memo_hits`・主手順・
  証明 DAG・「予算内に届くか」はスレッドの進み方で変わり得る。対人戦の経路はいまも時間上限で動いていて決定的ではないので、
  これは受け入れる。決定性が要る用途（テスト、問題集の一意性検証、再現）は `jobs=1`（既存経路、ビット一致）を使う。
  問題集生成の `--mate-jobs` は時間無制限で判定だけを使うので並列でよい。
- **文書**: `SOLVER.md` の契約に「`jobs>1` で変わり得る出力」の一覧を加える。

## 3. MCTS の並列化

### 3.1 候補の比較（1 局 1 木・NN 推論ありの対人戦）

| 方式 | 内容 | 時間短縮の見込み（GPU 経路） | 棋力への影響 | 判断 |
|---|---|---|---|---|
| 共有木（virtual loss、ロック／lock-free） | 複数スレッドで同じ木を降り、同じ束に入れる | 木は 1 束あたり 0.26 ms（64 葉）で GPU の 0.92 ms に隠れている → **ほぼ 0** | virtual loss の増加で探索が広がる（同訪問数で弱くなり得る） | 行わない |
| root 並列（木を K 本） | 同じ局面を seed 違いで K 本探索し root の訪問を足す | NN 処理量が上限なので時間は縮まない。CPU 推論の端末では K 倍の束で行あたりの時間が減る分だけ | 木ごとの決定化が独立で、同じ総訪問数なら同等以上と期待（ISMCTS の決定化は本来独立） | 既存の多対局 `num_threads` 経路で可能。CPU 推論の端末で実測して判断 |
| leaf 並列 | 1 束の NN 推論を複数スレッドで | GPU では GPU が担う。CPU/WASM 推論では ORT の `numThreads` がこれ | なし（結果同一） | ブラウザは既にあり。スレッド数の決め方だけ直す（第 5 節） |
| **NN の束とスレッドの組合せ（pipeline）** | 束を大きく（衝突の再試行）、束 2 つを先行して木と推論を重ねる | 11-06 の計測で 3,200 探索 0.143 → 0.049 s、25,600 探索 0.842 → 0.376 s | 既定ではビット一致。先行は同探索数で `pipelined` 対 `serial` の対局で確認中（dlsplendor 8.2 節） | **dlsplendor-e2 の `feature/engine-pipeline` を採用** |

### 3.2 `feature/engine-pipeline` との関係

`collision_retries`（衝突した経路に仮訪問を 1 つ足して別の下降を試す）と `max_in_flight`（評価前の束を 2 つ先行）は、
本書の「木を並列化しない」結論の下で MCTS 側に残る唯一の高速化で、既に実装・計測済みである。並列探索の担当はこれに
手を入れない。本書が MCTS 側に求めるのは次の 2 点だけで、どちらも `mcts_v3.h` の変更を要しない。

1. **詰み探索と同時に走ること**: `V3SearchSession` は 1 つのスレッドから使い、詰み探索は別スレッドで別の `Game` の複製を
   使う（共有する状態はない）。`root_mate_probe` と `solve_reveal_verified_mate_cpp` は GIL を解放している。
2. **root 並列が要るとき**: `add_game` を K 回（同じ局面、seed 違い、`root_noise=False`）して `num_threads=K`、`root_visits` を
   足す。既存の多対局経路で、結果はスレッド数によらず同一。

### 3.3 詰み探索を MCTS と同時に走らせる（各経路の変更）

- **dlsplendor `Thinker.think`**: `probe` を別スレッドで始め、MCTS の束のループを続ける。probe が「詰み」を返したら MCTS を
  締め切りで止めてその手を返す。`near_mate_override` も、MCTS の途中（残り時間が準詰みの予算に達した時点）から別スレッドで
  始め、MCTS の終了時に結果を合わせる。詰み側の予算は現行の配分のままで、MCTS は配分を差し引かず全時間を使える。
  CPU は MCTS 1＋詰み N スレッド（第 5 節）。
- **bgasplendor `host/thinker.py`**: 同じ変更。さらに dlsplendor のエンジン経路（TensorRT・束 2 つの先行）へ寄せると
  25,600 探索で 8.7 倍速くなる（dlsplendor の計測）。これは並列化ではなく経路の統一で、別件として扱う。
- **splendorgui `controller.ts`**: 第 5 節。

## 4. 棋力を落とさないことの検証

| 対象 | 方法 | 合格線 |
|---|---|---|
| 詰み探索の判定 | ランダム終盤 3,012 ケース（seeds 0〜59、2000〜2099、3000〜3099）と BENCH の深さ 3・4 を、時間無制限・ノード上限 15 万／40 万で `jobs=1` と `jobs=2,4,8,12` で解き、判定（mate/no_mate/unknown）と、mate の深さを比較 | 完全一致。unknown は `jobs=1` の unknown の部分集合（並列が先に予算を切ることはノード上限を枝で分けない限り起きない） |
| 詰み探索の主手順 | `jobs>1` の主手順を `jobs=1` の DFS で再検証（`required_root_action` で初手を固定し証明） | すべて証明できる |
| 詰み探索の速さと発見 | 実対局の終盤 81 局面（dlsplendor `native_v3_search_20260929.md` の集合）を 150 ms・1 s の時間上限で解き、詰み発見数とノード数 | 発見数は `jobs=1` 以上、時間は BENCH 深さ 4 で 8 スレッド 4 倍以上 |
| 同時実行の健全性 | ThreadSanitizer（`csplendor_sanitizer`、Clang）で並列版の単体テスト、`jobs=16` の soak（1 時間） | 競合報告 0 |
| MCTS（pipeline 既定値） | `tests/test_mcts_v3.py` のビット一致 | 一致 |
| MCTS（同訪問数の手の一致） | 実対局 200 局面で、`serial` と `pipelined`（または root 並列 K 本）の root の最多訪問手と訪問分布（KL） | 最多訪問手の一致 ≥ 95%、不一致はいずれも Q 差 < 0.02 |
| MCTS（同時間の勝率） | `scripts/match_engine.py`（dlsplendor）で候補対基準、1 手同秒、先後入替。SPRT（H0: −15 Elo、H1: 0、α=β=0.05）または固定 1,200 局で補正後 ≥ −15 | 非劣性 |
| 全体（端末） | 同じ局面集合で 1 手の時間の中央値・95 パーセンタイル、探索数、詰み発見の有無を Ryzen → M4 → iPhone で記録 | 第 6 節の見積もりとの照合 |

## 5. 端末ごとの方針

### 5.1 スレッド数

- **原則**: MCTS の木 1 スレッド、NN は推論器（GPU／ORT スレッド／WebGPU）、詰み探索 = 残りの物理コア。
  `jobs = clamp(physical_cores − 2, 1, 8)`（Ryzen 9 7900X は 8。SMT の論理コアは数えない。詰み探索はメモリ帯域と置換表の
  キャッシュミスが支配的で、SMT の利得が小さい）。共有マシンで学習が並走しているときは環境変数（`CSPLENDOR_MATE_JOBS`）で下げる。
- **M4（P コア・E コア）**: ネイティブ（Python）では `jobs = P コア数`（M4 Pro 12 コアなら 8、M4 10 コアなら 4）。スレッドの
  QoS を `QOS_CLASS_USER_INITIATED` にして E コアだけに寄せられないようにする（`pthread_set_qos_class_self_np`。macOS だけ、
  `#if defined(__APPLE__)`）。ブラウザからはコアの種類も QoS も選べないので、`hardwareConcurrency` から 2（UI＋MCTS の Worker）を
  引いた数を上限に、第 5.3 節の実測で決める。
- **iPhone**: `hardwareConcurrency` は 6（2P＋4E）が多い。Worker は MCTS 1＋詰み 1、ORT の `numThreads` は 2〜4 を実測で選ぶ。
  熱で数十秒後に性能が落ちるので、計測は 1 手ずつでなく 20 手の連続で行う。

### 5.2 ブラウザの WASM で pthreads を使う条件と戻り方

- **条件**: `crossOriginIsolated`（レスポンスに `Cross-Origin-Opener-Policy: same-origin` と `Cross-Origin-Embedder-Policy: require-corp`
  （または `credentialless`。Safari は `require-corp` のみ）があり、読み込む全リソースが CORS か CORP つき）、`SharedArrayBuffer` が
  存在すること。splendorgui の `public/_headers` は `/play/game`、`/review`、`/dev/bench` にだけ付いていて（`/` と `/account` は
  Google ログインのため `same-origin-allow-popups` で COEP なし）、Cloudflare Pages の `_headers` で配信される。ORT の wasm スレッドは
  この条件で既に動いている（`backend.ts` が非分離なら 1 スレッド）。
- **Safari の制約**: iOS 15.2 以降で SAB は使えるが、(1) Worker の中からさらに Worker を作る「入れ子 Worker」は iOS Safari で
  長く未対応だった（Emscripten の pthreads は Worker から pthread を起こすので、エンジンの Worker から詰み探索の pthread を
  起こすには入れ子が要る。対応版の確認が必要）、(2) wasm のメモリは共有メモリでは `MAXIMUM_MEMORY` を固定し、iOS では 1 タブの
  実用上限が 1〜1.5 GB、(3) 低メモリ端末は `lowMemory.ts` が localStorage の記録で 1 スレッドに落としている。
- **第 1 段では csplendor.wasm に pthreads を入れない。** 代わりに **詰み探索用の 2 つ目の Worker** を立て、そこに同じ単一スレッドの
  `csplendor.wasm` をもう 1 つインスタンス化して `rootMateProbe` を呼ぶ。Worker どうしはメッセージだけで、SAB も COOP/COEP も
  要らない（`/` でも動く）。MCTS の Worker と並行に走るので待ち時間は max(MCTS, 詰み) になる。wasm のメモリは 2 倍になるので、
  詰み側は `memory_budget_bytes` を 64〜128 MiB に抑える（表は使った分だけ伸びる）。
- **第 3 段（任意）**: 詰み探索の並列版（第 2 節）を wasm で使うときは、`-pthread -sPTHREAD_POOL_SIZE=N -sMAXIMUM_MEMORY=…` の別ビルド
  `csplendor_mt.wasm` を作り、`crossOriginIsolated && typeof SharedArrayBuffer !== 'undefined'` かつ入れ子 Worker が使えるときだけ
  読み込む。それ以外は単一スレッド版（第 1 段）に戻る。切り替えは `backend.ts` の読み込み時 1 回で、探索の途中では変えない。
  pthread 版の wasm でも、仕事の単位を Worker 間メッセージで配る「root 分割 A」なら共有メモリなしで組めるので、2.3 節の
  決定性が要るブラウザの用途（詰将棋モードの解答表示）はそちらにする。

### 5.3 NN 推論の方式（ブラウザ）

- 現状は `wasm` と `webgpu` を実測して速い方を選ぶが、**Apple モバイルは WebGPU を禁止**している。iOS 26 の Safari は WebGPU を
  既定で有効にしたので、iPhone 15 以降ではこの禁止を外して実測に任せる候補になる（網の h128l3 では WebGPU の起動と転送の
  固定費が勝つかもしれないので、宝凰の h384l8・葉 64 だけ WebGPU、宝神は wasm、という使い分けも実測で決める）。
- ORT wasm の `numThreads` は `min(4, hardwareConcurrency)` 固定ではなく、起動時に 1・2・4 スレッドで 8 行と 64 行の推論を
  各 10 回測って最速を選ぶ（既に EP の実測はあるので、その中に加える）。
- 束の大きさ: CPU 推論では行あたりの時間が束で下がる（本計測では 8 行 → 64 行で 1.15〜1.4 倍）。宝神の葉 8 を、`collision_retries`
  を入れた上で 16〜32 にする余地がある（同訪問数の手の一致で確認）。

### 5.4 メモリ予算とスレッド数

- 共有表（第 2 節）では予算を jobs で割らない。`solver_memory_budget_bytes(available, 1)` を 1 表に使い、スレッドの作業領域は
  別に見積もる（1 スレッド 8 MB 以下）。`mate_usage.md`「探索のメモリ予算」の表の `jobs` 列はこれに合わせて書き換える。
- ブラウザは JS が `navigator.deviceMemory`（Safari は未実装 → `isAppleMobile` なら 4 GB とみなす）から予算を決める。
  iPhone: MCTS Worker の wasm（網＋木）と詰み Worker の wasm（表）の合計を 512 MiB 以下に収める（詰みの表 128 MiB）。
  M4: 詰みの表 512 MiB。デスクトップ: 現行どおり空きメモリの 1/4（64 MiB〜2 GiB）。

## 6. 段階的な実装計画・テスト・計測・見積もり

| 段 | 内容 | 変更する場所 | テスト | 期待する短縮（根拠） |
|---|---|---|---|---|
| **1. 詰み探索を MCTS と同時に** | `Thinker.think` で probe・near_mate を別スレッド化。bgasplendor ホストも同じ。splendorgui は詰み用の 2 つ目の Worker | dlsplendor `engine/thinker.py`、bgasplendor `host/thinker.py`、splendorgui `controller.ts`・`engine.worker.ts` | 既存の対局テスト、probe の結果が直列版と同じ（時間上限なし） | 終盤の 1 手（1 s）で最大 0.3 s（probe 150 ms＋near_mate 150 ms が重なる）。序盤は 10 ms 以下。MCTS に時間を回すなら探索数 +30〜40%（≈ +10 Elo、2 倍で +24 Elo の換算） |
| **2. C++ の 2 段タスク列＋共有置換表** | `solve_reveal_verified_mate_parallel_cpp`、並行 `FlatMemoTable`、階層化 token、`RootMateProbeConfig.jobs`。Python の `_solve_depth_parallel` を置き換えて削除 | csplendor `src/reveal_verified_solver.cpp`、新規 `src/parallel_mate_solver.h`、`src/solver_flat_table.h`、`src/bindings_solvers.cpp`、`csplendor/mate_depth.py`、`doc/mate_usage.md`・`SOLVER.md` | 第 4 節の判定一致 3,012 ケース、TSAN、`jobs=1` のビット一致、BENCH 深さ 4 の時間 | BENCH 深さ 4: 1.30 s → 0.3 s 前後（8 スレッドで 4〜5 倍。1 段目 42 手 × めくれ、2 段目で数千の仕事になり偏りが消える。共有表で枝をまたぐ重複も消える）。深さ 3 の厳密探索（1 万ノード級、10 ms）は既に 150 ms に収まる。深さ 4（BENCH で 0.3 s）は probe の 150 ms には入らないが、第 1 段で詰み探索が MCTS と並走するなら 1 手全体（1 s）を詰み側にも使えるので `endgame_exact_depth` を 2 → 3〜4 に上げられる → 詰み発見の増加（棋力は上がる方向） |
| **3. ブラウザの推論の実測選択** | ORT スレッド数の実測、Apple モバイルの WebGPU 禁止の見直し、宝神の葉 16〜32 | splendorgui `backend.ts`、`lowMemory.ts`、`levels.ts` | 同訪問数の手の一致、M4・iPhone の 20 手連続の時間 | iPhone: ORT 1 → 2〜4 スレッドで NN が 1.5〜2.5 倍（本計測の CPU 1 → 4 スレッドが 2.1 倍）。WebGPU が使えれば宝凰でさらに 2〜5 倍（要実測） |
| **4.（任意）root 並列 K 本** | CPU 推論の端末で `add_game` × K・`num_threads=K` | dlsplendor / splendorgui の呼び出し側のみ | 同訪問数の手の一致、同時間の勝率 | CPU 推論で束 8 → 64 行の行あたり 1.15〜1.4 倍。GPU では 0 |
| **5.（任意）pthreads 版 wasm** | `csplendor_mt.wasm`、第 5.2 節の切り替え | splendorgui `engine/build.sh`、`backend.ts` | 分離あり／なし、Safari の入れ子 Worker | 2 の効果をブラウザへ（詰み探索の 4 倍）。宝神以外に詰み判定を広げる前提 |

計測の手順（各段の前後で同じ）: Ryzen は scratchpad のスクリプトと dlsplendor `scripts/benchmark_engine.py`（`--variants serial,pipelined`）。
M4・iPhone は splendorgui の `/dev/bench`（COOP/COEP あり）で、宝神・宝極・宝凰の 20 手連続の 1 手の時間の中央値と 95 パーセンタイル、
探索/秒、ORT の EP とスレッド数、詰みの発見を記録する。熱の影響を見るため最初の 5 手と最後の 5 手を分ける。

## 7. 担当の分け方

| 領域 | 担当 | 本書での役割 |
|---|---|---|
| `src/mcts_v3.h`、`src/bindings_mcts_v3.cpp` | dlsplendor-e2（pipeline）と「csplendor パフォーマンス最適化」 | **並列探索の担当は触らない。** 必要なのは「`V3SearchSession` は 1 スレッドから使い、詰み探索と並走できる」ことの文書化だけ（`mcts_v3.md` に 1 行） |
| `src/reveal_verified_solver.cpp`、`src/solver_flat_table.h`、新規 `src/parallel_mate_solver.h`、`src/bindings_solvers.cpp`、`csplendor/mate_depth.py`、`doc/mate_usage.md`・`SOLVER.md` | 並列探索の担当（本書） | 第 2 段 |
| `src/solver_memory_budget.h` | 並列探索の担当 | 共有表の予算（jobs で割らない） |
| dlsplendor `engine/thinker.py` | dlsplendor-e2 | 第 1 段（probe・near_mate の別スレッド化）。並列探索の担当は設計を渡し、レビューする |
| bgasplendor `host/thinker.py` | bgasplendor の担当 | 第 1 段。エンジン経路への統一は別件 |
| splendorgui `src/worker/controller.ts`・`engine.worker.ts`・`lib/engine/backend.ts`・`lowMemory.ts`・`shared/src/levels.ts` | splendorgui の担当 | 第 1 段（詰み用 Worker）、第 3 段（推論の実測選択）。`engine/build.sh`・`bindings_web.cpp` は第 5 段まで変えない |

splendorgui 側に必要な変更の一覧: (1) 詰み用の 2 つ目の Worker と、そこで `createEngine()` をもう 1 つ作って `rootMateProbe` を呼ぶ経路、
MCTS の `think()` と並行に走らせ、詰みなら MCTS を abort、(2) `memory_budget_bytes` を Worker ごとに分ける、(3) ORT の `numThreads` の実測選択、
(4) `gpuAllowed` の Apple モバイル禁止を iOS 26 以降で実測に任せる、(5) `/dev/bench` に 20 手連続の計測と結果の書き出し。
`_headers` は変えない（第 1 段は分離を要しない）。

## 8. 見送ったもの

- 共有木の並列 MCTS（旧 MCTS の `parallel_search_plan/` の Stage B）: 木が律速でないので対人戦では効果がない。学習の多対局は
  既存の `num_threads` 経路で足りる。
- 並列 df-pn: df-pn が既定でなく DFS より遅いため、df-pn 側の速さが DFS を上回ってから検討する。
- CPU 推論の最適化（int8、小さい網）: 並列化ではないが、ブラウザの待ち時間には最も効く。別件。
