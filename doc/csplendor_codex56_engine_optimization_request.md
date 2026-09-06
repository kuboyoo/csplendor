# csplendor 高速化実装・効果検証依頼書
## Codex-5.6 Ultra / Max 向けマスター計画

- 作成日: 2026-09-01
- 対象リポジトリ: `kuboyoo/csplendor`
- 静的解析基準コミット: `7835f642b23251d0cb91de180006084521c74aa6`
- 主対象環境: Ryzen 9 7900X / Linux / GCC 15.2 / Python 3.12.1
- 言語・ビルド: C++17 / CMake / pybind11
- 文書の目的: **ルール・探索量・公開APIを維持したまま、ゲームエンジン、MCTS、厳密めくれ探索を段階的に高速化し、同一条件のpaired A/Bで効果を証明する**

---

# 0. Codexへの最重要指示

この文書は全体ロードマップであり、**一度に全Phaseを実装する指示ではない**。

初回実行では、次の指示に従うこと。

> **Phase 0だけを実行し、計測基盤と基準値を提出した時点で停止すること。高速化実装へ進まないこと。**

後続Phaseも、原則として1回の作業につき1 Phase、または本文で明示した1つの独立仮説だけを扱うこと。
複数の最適化を一括して入れ、どれが効いたか分からない状態を作ってはならない。

## 0.1 静的解析とローカルソースの照合

本依頼書は上記コミットの公開ツリーを静的解析して作成した。作業開始時に必ず次を実行する。

```bash
git status --short
git rev-parse HEAD
git log -1 --oneline
```

基準コミットと一致しない場合:

1. ユーザーの変更をリセット、stash、checkout、上書きしない。
2. 現在のHEADと差分概要を報告する。
3. 本文中の行番号ではなく、**ファイル名・型名・関数名を基準に現行実装へ読み替える**。
4. 対象シンボルが既に変更済みなら、重複実装せずPhase 0で現状を再計測する。
5. 未コミット変更がある場合、その変更を壊さない独立branch/worktreeを使うか、作業を停止して報告する。

## 0.2 実装上の不変条件

以下は性能より優先する。

- Splendorの合法手集合、合法手順序、支払いパターン、返却パターンを変えない。
- `simple_payment_mode`、`blank_refill_mode`、強制PASS、貴族選択待ち、最終ラウンド、引き分け判定を変えない。
- 公開`Action`のpack/unpack、ActionEncoderCpp/V2/V3、特徴量schema、snapshot、Python APIの既存挙動を変えない。
- 厳密探索の正否、証明手、具体的めくれ集合、全応手検証を弱めない。
- 非アルゴリズムPhaseでは、探索順、訪問node数、合法手数、TT hit/store数を原則同一に保つ。
- hash-only照合で置換表を省メモリ化しない。キー衝突時に誤った同一視をしてはならない。
- portable buildを既定のまま維持する。
- 配布wheelへ`-march=native`、PGO対象CPU固有コードを混入させない。
- 外部依存を追加しない。必要性が測定で明確になった場合のみ、別提案として止めて報告する。
- lock-freeデータ構造へ先走らない。まず現在の待ち時間・競合・占有数を測る。
- benchmarkだけ速くなる特殊化を入れない。複数fixtureと実ワークロードで確認する。
- 性能向上を測定せず「高速化した」と記述しない。

## 0.3 Git運用

推奨branch:

```bash
git switch -c perf/codex56-engine-hotpaths
```

各独立仮説について:

1. baseline確認
2. 実装
3. 単体・差分・sanitizerテスト
4. paired A/B
5. 採用または完全revert
6. 採用した場合だけ1コミット

コミット例:

```text
bench: add reproducible engine hotpath harness
perf(hash): maintain exact zobrist hash incrementally
perf(solver): replace recursive-path hash set with stack
perf(mcts): use bit-rank compact edge lookup
```

次をコミットしない。

```text
build/
venv/
*.so
*.o
*.a
*.profraw
*.profdata
*.gcda
*.gcno
*.egg-info/
__pycache__/
未圧縮の測定時巨大ログ（確定raw記録は下記の永続化規約に従う）
モデル重み
大量棋譜
```

**pushは、この作業セッションでユーザーから明示承認がある場合だけ行うこと。**

## 0.4 計測記録の永続化

- 一意なbenchmark・診断・差分検証記録を`/tmp`だけに置かない。
- build directoryと再生成可能なbinaryは`/tmp`でよいが、計測出力は最初から
  `doc/performance_experiments/raw/<phase>/`へ保存する。
- 大きな確定済みraw記録は`gzip -n -9`で決定論的に圧縮し、
  `doc/performance_experiments/raw/manifest.tsv`へ元内容と圧縮後のSHA-256を記録する。
- Phase報告と後続作業は`/tmp`上の一時名ではなく、上記の永続パスを参照する。
- raw内容は計測後に書き換えず、要約や解釈は別のMarkdown・CSVへ記録する。

---

# 1. 現行性能と評価対象

READMEに記録された現行代表値は次のとおり。これは参考値であり、採否判定には必ず同一host・同一compiler・同一fixtureのpaired A/Bを使う。

## 1.1 ルールエンジン

| workload | 現行代表値 |
|---|---:|
| Python `legal_actions` | 27,084 calls/s |
| C++ `legal_action_codes` | 125,444 calls/s |
| C++ `legal_action_count` | 1,011,935 calls/s |
| C++内部自己対戦 | 892,607 moves/s |

## 1.2 MCTS

| mode/backend | 現行代表値 |
|---|---:|
| exact legacy 1 thread | 387,132 sim/s |
| exact sharded 1 thread | 222,253 sim/s |
| exact sharded 4 threads | 217,910 sim/s |
| exact sharded 8 threads | 194,405 sim/s |
| exact root-parallel 8 workers | 1,418,195 sim/s |
| determinized legacy 1 thread | 358,261 sim/s |
| determinized sharded 4 threads | 294,279 sim/s |
| determinized root-parallel 8 workers | 1,584,560 sim/s |

48手mask、action decode、dense mask走査、compact edgeなどは既に大幅高速化済みである。
したがって、それらを「新規案」として単に再実装してはならない。

## 1.3 厳密めくれ探索

| workload | 現行代表値 |
|---|---:|
| exact reveal、深さ7、10,000,000 nodes | 5,440,074 nodes/s |
| 同実時間 | 1.838 s |
| 合法手数 | 8,524,863 |
| TT hit | 778,150 |
| 保存局面 | 643,158 |

このworkloadでは、速度だけでなく次の完全一致を確認する。

- 訪問node数
- 合法手数
- TT hit/store数
- status
- winning root action
- principal line
- reveal orderまたはそのdigest
- proof DAGを生成するfixtureではDAGの意味的同値性

---

# 2. 静的解析の結論

## 2.1 優先順位

| 優先度 | 仮説 | 主対象 | 静的に見込まれる余地 | リスク |
|---|---|---|---|---|
| S | exact/observable Zobrist hashの差分維持 | MCTS | 大 | 中 |
| S | 厳密探索のstate key・残存カード集合の増分管理 | exact reveal | 大 | 高 |
| S | 厳密探索TTのkey/entry圧縮とflat化 | exact reveal/RSS | 大 | 高 |
| S | solver専用rollback | visible/exact solver | 中〜大 | 高 |
| S | parallel MCTSのwrite-only atomic・edge lookup・予約管理 | shared tree | 中〜大 | 中〜高 |
| A | legacy MCTSの同一key三重map統合 | legacy/root-parallel | 中 | 中 |
| A | 合法手count専用経路・code一回列挙 | rule engine | 中 | 中 |
| A | packed resource・貴族適格maskの差分更新 | self-play/apply | 小〜中 | 低 |
| A | 48手indexからの直接遷移 | MCTS | 小〜中 | 中 |
| B | MCTS専用provenance-free clone | MCTS clone | profile依存 | 中 |
| B | 内部packed action生成 | legal codes/self-play | profile依存 | 中 |
| B | Linux x86 native/LTO/PGO | 全体 | 実用上有望 | 低〜中 |
| C | Python NumPy/buffer code API | Python境界 | 利用形態依存 | 低 |

「静的に見込まれる余地」は仮説であり、倍率を保証しない。
各Phaseの予想レンジは重複し、**単純に乗算・加算してはならない**。

## 2.2 主なボトルネック仮説

### A. hashはキャッシュされるが、着手後は全再計算

`Board`は`cached_hash/hash_valid`を持つが、通常のtrusted transitionでも最初に`begin_unchecked_mutation()`を呼び、hashを無効化する。次回`Board::hash()`では`compute_hash_impl<true,true>()`が全状態を再走査する。

完全情報hashは、初期配置直後なら概ね次を走査する。

- bank 6
- visible 12
- nobles
- player scalar/array/reserved
- 残存deck順序: `36 + 26 + 16 = 78` card positions
- turn/state flags

exact MCTSの`GameAdapter::tree_key()`は各nodeで`board.hash()`を要求するため、apply後の全hash再計算がhot pathになり得る。

observable MCTSも`observable_hash(observer)`を毎回全再計算する。こちらはdeck内容ではなくdeck sizeを使うが、全公開状態を再走査する。

### B. 厳密めくれ探索は毎nodeのkey構築が重い

`RevealVerifiedSolver::state_key()`は概ね次を行う。

- `compute_set_deck_search_hash()`でboard fieldsを全再走査
- 3山の全cardを走査し`unseen` bitsetを再構築
- 購入履歴vectorを走査し`acquired_hidden`を再構築
- `is_claimed()`で購入済み、visible、reservedを繰り返し走査
- 大きな`DepthStateKey`を`unordered_map`/`unordered_set`へ渡す

10,000,000 node workloadでは、この固定費が有力。

### C. solverの枝遷移はBoard丸ごとcopy/restore

`VisibleOnlySolver`と`RevealVerifiedSolver`には、枝ごとに次の形が多数ある。

```cpp
const Board previous = game.board;
apply...
recurse...
game.board = previous;
```

`Board`は固定deck配列だけでなく、各playerの`purchased_cards`、`acquired_nobles`という`std::vector`も含む。深い探索ではcopyとallocator trafficが支配的になり得る。

既存`UndoRecord`は通常着手のdelta-undo検証候補だが、exact revealでは「山の任意位置からcardを除去して一時的にtopへ置く」処理があるため、deck countだけの復元では不十分。solver専用の拡張rollbackが必要。

### D. parallel treeには共有atomicとnode内allocationが残る

静的解析で確認した主な候補:

- `find/find_or_create`ごとにglobal `access_epoch.fetch_add`
- nodeごとに`last_access.store`
- `last_access`が実際のeviction等で読まれていなければwrite-only
- 多数の`SearchLedger` atomicを各workerが高頻度更新
- tree-global quiescence counter
- nodeごとの`std::unordered_set<uint64_t> live_reservations`
- tree-global monotonic reservation ID
- compact edge lookupで`lower_bound`
- selectionごとに全edgeのvirtual lossを合計
- path entryがtree/node/ledgerのshared ownershipを繰り返し保持
- mutex/CV + `std::deque`のbounded queue

### E. legacy MCTSは同じhashを3つのmapで管理

`MCTS`は同一keyに対して次を別々の`unordered_map`で持つ。

- `nodes_`
- `node_aux_`
- `access_count_`

`get_node()`はnode lookupの後にLRU mapも更新する。展開、selection、backprop、pruneで同一keyの複数lookupと複数node allocationが発生する。

### F. 合法手code取得は二重列挙

`Game::legal_action_codes()`は最初に`count_all_fixed()`で全生成相当のcountを行い、その後`consume_all_capped()`で再度全列挙する。

`MoveGenerator::generate_all()`も同じ構造。合法手が多い局面では正確なreserveの利益がある一方、count前走査が無視できない。

### G. count処理でもAction生成と再帰が残る

- 返却パターンは最大6色、正規局面の返却超過は最大3個だが、再帰countを使う。
- 購入支払いは5色の再帰列挙を使う。
- `MoveList`は`std::array<Action, 2048>`を持ち、`Action`は多数の既定初期化fieldを持つ。
- 「heap allocationなし」は「構築costなし」を意味しない。

### H. 購入時にpacked fieldと貴族maskを再走査

購入ではresource更新後に`sync_packed()`が呼ばれ、packed gems/bonusesを再構築し、12 nobles × 5 colors相当の適格性判定を行う。

貴族適格性は各color thresholdのbitmask ANDで同値計算できる。

---

# 3. Phase依存関係

推奨依存関係:

```text
Phase 0
  ├─ Phase 1A exact hash
  │    ├─ Phase 1B observable hash
  │    └─ Phase 5A direct MCTS apply
  ├─ Phase 2A rule-state micro optimizations
  ├─ Phase 2B legal count/code generation
  ├─ Phase 3A solver low-risk containers
  │    └─ Phase 3B incremental reveal state/key
  │          ├─ Phase 3C compact/flat TT
  │          └─ Phase 3D solver rollback
  ├─ Phase 4A MCTS edge-rank/dead metadata
  │    ├─ Phase 4B legacy tree record/compact node
  │    └─ Phase 4C parallel counters/reservations
  │          └─ Phase 4D queue/persistent worker pool（profile-gated）
  ├─ Phase 5B provenance-free search clone（profile-gated）
  ├─ Phase 5C internal packed action / Python buffer API
  └─ Phase 6 local native/LTO/PGO
```

Phase 0 profileの結果により、同一階層内の順序は変更してよい。
ただし、理由と測定根拠を報告すること。

---

# 4. 共通の測定規約

## 4.1 paired A/B

各変更は同じhost、同じCPU affinity、同じcompiler、同じflags、同じfixture、同じseedで比較する。

推奨:

- warmup 2〜5回
- 21〜30 pair
- A/B実行順をABBAまたはseedでランダム化
- 各pairの`B/A`比を保存
- median ratio
- bootstrap 95% CI
- absolute median
- p50/p95
- node数、合法手数、tree size、TT hit/store、RSSも保存

単純な「変更前を午前、変更後を午後に1回ずつ」は不可。

## 4.2 CPU固定

Linux例:

```bash
taskset -c 4 <command>
taskset -c 4-11 <parallel-command>
```

CPU番号は実機topologyを確認して決める。SMT siblingの混在を記録する。

次を記録する。

```bash
lscpu
uname -a
g++ --version
cmake --version
python --version
git rev-parse HEAD
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor 2>/dev/null || true
```

governorを勝手に変更しない。変更する場合はユーザー承認を得て、A/B両方に同じ条件を適用する。

## 4.3 perf

代表コマンド:

```bash
perf stat -r 15 \
  -e cycles,instructions,branches,branch-misses,\
cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,\
context-switches,cpu-migrations,page-faults \
  taskset -c 4 <benchmark>
```

call graph:

```bash
perf record -g --call-graph dwarf \
  taskset -c 4 <benchmark>
perf report
```

並列処理では以下も可能な範囲で測る。

- mutex wait/hold time
- futex
- atomic hotspot
- LLC miss
- false sharing
- queue empty/full wait
- batch fill率
- worker idle率

## 4.4 correctness digest

非アルゴリズムPhaseでは、最低限次を保存してA/B比較する。

### rule engine

- seed
- ply
- ordered packed legal action listのhash
- count
- 選択action code
- apply後のfull board snapshot hash
- exact hash / recomputed exact hash
- observable hash 0/1
- scores/current player/turn/winner

### MCTS deterministic fixture

- root key
- seed manifest
- expanded key sequence digest
- selected action sequence digest
- inference request digest
- root visit counts
- tree size
- completed simulations
- exact virtual loss balance
- deterministic replay trace

### solver

- status
- node count
- legal move count
- terminal count
- memo hit/store count
- ordered action digest
- reveal candidate/order digest
- winning action
- principal line
- proof DAG summary/digest

浮動小数演算順序を変えるPhaseでは、差が許容される理由を明示し、別途fixed-time品質評価を行う。

## 4.5 採用基準

### 小規模Phase

- 主対象workload medianで **3%以上**
- 95% CI下限が原則`1.00`超、可能なら`1.01`以上
- 他の主要workloadの悪化が2%以内
- RSS悪化なし、または理由が明確

### 大規模構造変更

次のいずれか:

- 主対象workloadで5%以上
- RSS 15%以上削減
- 8-thread shared-treeで10%以上
- それ未満でも複数fixtureで再現し、明確なcache/branch/atomic指標改善がある

かつ:

- 1-thread regression 3%以内
- correctness gate全通過
- 可読性・保守性が著しく悪化しない

### 棄却

次の場合は実装をrevertし、測定・理由だけ文書化する。

- CIが0を跨ぐ
- 単一fixtureだけ改善
- node数や探索順を無断で変える
- memory増加がspeed gainに見合わない
- sanitizer/invariant failure
- portable build regression
- public API/schema変更が必要
- 複雑性に対して効果が小さい

---

# 5. Phase 0 — 再現可能な計測基盤

**推奨モデル: Codex-5.6 Max**
**初回はこのPhaseだけ実施する。**

## 5.1 目的

最適化を一切入れず、現在の実装について以下を分離測定する。

1. legal count
2. legal codes
3. legal Action生成
4. random self-play apply
5. applyのみ
6. apply + exact hash
7. apply + observable hash
8. cold hash / cached hash
9. clone_light
10. determinization clone
11. StateEncoder
12. 48 action mask
13. decode + apply
14. legacy tree lookup/selection/backprop
15. shared tree lookup/selection/reservation/commit
16. solver state key
17. solver TT lookup/store
18. solver Board copy/restore
19. exact reveal end-to-end
20. RSS

## 5.2 追加するもの

推奨ファイル:

```text
scripts/benchmark_engine_hotpaths.cpp
scripts/run_paired_benchmarks.py
scripts/benchmark_manifest.py
doc/performance_experiments/baseline_YYYYMMDD.md
```

既存benchmarkを拡張してもよいが、既存CSV契約を壊さない。

CMake option例:

```cmake
option(CSPLENDOR_BUILD_ENGINE_BENCHMARK
       "Build native rule/solver hotpath benchmark" OFF)
```

instrumentation:

```cmake
option(CSPLENDOR_PERF_INSTRUMENTATION
       "Enable internal performance counters" OFF)
```

通常Releaseではcounter分岐・atomic・TLSを完全に除去すること。

## 5.3 fixture matrix

最低限:

### Rule engine

- seed 42、12 ply、約250 legal actionsの既存代表局面
- legal action 5件の既存fixture
- 初期局面
- 10 token直前/超過返却局面
- gold payment分岐が多い局面
- reserve上限付近
- 複数貴族eligible
- final round
- forced pass
- editor由来の非正規値を含むfallback fixture

### MCTS

- exact / determinized
- legacy / coarse / sharded / root-parallel
- 1/2/4/8 threads
- batch 1/16/64
- 0/50/250/1000 µs synthetic inference latency
- 4,096 / 65,536 simulations
- fresh tree / retained tree
- 250-action中盤
- reveal-heavy/hidden-reserve fixture

### Solver

- READMEの10,000,000 node exact reveal fixture
- visible-only
- deck reserve branch多
- visible refill branch多
- TT hit率高/低
- proof DAGなし/あり
- persistent `MateSearchSession`
- noncanonical/editor input fallback

## 5.4 内部counter

compile-time instrumentationで次を数える。

### Hash

- exact hash calls
- exact cache hits/misses
- exact fields visited
- deck card salts visited
- observable calls
- observable fields visited
- recompute oracle failures

### Copy/allocation

- `clone_light` calls
- Board copy/assignment calls
- purchased/acquired vector allocation count
- action vector reallocations
- solver temporary vector/set allocations

### Solver

- state key calls
- scanned deck cards
- scanned purchased IDs
- `is_claimed` calls/comparisons
- path set find/insert/erase
- TT probes
- TT key comparisons
- probe length histogram
- Board rollback count
- reveal candidate count
- card equivalence lookup count

### Parallel MCTS

- tree lookup count
- `access_epoch` updates
- edge lookup count/comparisons
- reservation occupancy histogram
- live reservation set allocation
- ledger atomic increments byfield
- node lock wait/hold
- shard lock wait/hold
- queue wait/full/empty
- worker idle time
- coordinator idle time
- batch fill率

## 5.5 Phase 0 deliverable

Codexは次を提出して停止する。

1. 変更ファイル一覧
2. build/testコマンド
3. benchmark fixture一覧
4. baseline CSV/JSON
5. hotspot上位20シンボル
6. cycles比率
7. allocation/RSS
8. 各仮説の優先順位更新
9. Phase 1Aを先に行うべきかの判断
10. commit hash

**Phase 0ではゲームロジックやデータ構造を最適化しない。**

---

# 6. Phase 1A — exact Zobrist hashの差分維持

**推奨モデル: Codex-5.6 Ultra**
**最優先候補。**

## 6.1 仮説

exact MCTSでは、各trusted apply後にhash cacheが無効化され、次のtree key生成で残存deck順を含むfull hashを再計算している。trusted transition中にZobrist hashを差分更新すれば、apply + hashを大幅短縮できる。

静的予想:

- exact legacy/root-parallel: 5〜20%
- hash microbenchmark: 5倍以上の可能性
- self-play without hash:原則ほぼ無影響

予想値は未検証。

## 6.2 対象シンボル

```text
src/board.h
  Board::hash
  Board::compute_hash_impl
  Board::begin_editor_mutation
  Board::begin_unchecked_mutation

src/game.h
  Game::apply_unchecked
  Game::apply_take_gems
  Game::apply_reserve_visible
  Game::apply_reserve_deck
  Game::apply_purchase
  Game::apply_noble_visit

src/rule_transition.h
  reserve_card_unchecked
  grant_reserve_gold
  return_gems_unchecked/checked
  purchase_card
  acquire_noble_unchecked
  end_turn
  finish_standard_action
```

## 6.3 設計要件

公開field layoutやeditor APIを壊さず、mutation経路を分ける。

### Editor/raw path

- `begin_editor_mutation()`は従来どおり全hash cacheをinvalidate。
- 任意のeditor state、範囲外uint8値、重複card等を受けられる。
- 現行のfallback hash semanticsを維持。

### Trusted rule path

- 着手開始時に無条件invalidateしない。
- cacheがvalidな場合だけold saltをXOR-outし、new saltをXOR-in。
- cacheがinvalidな場合はfieldだけ更新し、invalidのまま。
- 失敗途中のpartial mutationでは、rollbackまたはinvalidateに倒す。
- exception pathでvalidだが誤ったhashを残さない。

### 必要な差分primitive

最低限:

- bank[color] count
- player gems[color]
- packed fieldはhash対象外だがstate consistency維持
- player bonuses[color]
- points
- reserved count
- purchased count
- reserved slot card ID
- reserved hidden flag
- visible slot card ID
- noble slot/list
- current player
- waiting noble
- final round
- winner
- turn
- deck pop/push/erase

deck pop:

```text
old size = n
popped card = deck[n-1]
hash ^= Z.deck_cards[level][n-1][card]
```

deck pushは逆操作。
任意位置eraseは後続cardのposition saltが全て変わるため、exact reveal fallbackでは専用処理またはinvalidateを使う。通常ゲーム遷移のtop popと分ける。

### 実装形

次のいずれかを比較し、最も局所的で安全なものを採用する。

1. `Board::TrustedMutator`
2. `HashDelta` helper
3. 型安全な`set_*_trusted()` inline群
4. apply前後の小さなfield deltaから最後にまとめてXOR

全fieldをpublic setterへ置き換える大規模リファクタは避ける。

## 6.4 correctness

compile-time debug option:

```text
CSPLENDOR_VERIFY_INCREMENTAL_HASH
```

有効時、各successful trusted apply直後に:

```cpp
assert(board.hash_valid);
assert(board.cached_hash == board.compute_hash_uncached());
```

invalid cacheを許す経路では、`board.hash()`後にoracle一致を確認。

random differential:

- 1,000 seed以上
- 各gameで終局まで
- 全合法手または複数random trajectory
- simple/full payment
- reserve visible/deck
- hidden reserve purchase
- blank refill
- noble 0/1/複数
- forced pass
- final round/draw
- editor state
- clone
- snapshot roundtrip
- determinization
- undo

A/Bでordered legal listと全board fieldを比較する。

## 6.5 benchmark

必須:

- cold `compute_hash_uncached`
- cached `hash`
- apply only
- apply + hash
- exact legacy MCTS 1T
- exact root-parallel 4/8
- exact sharded 1/4/8
- determinized pathが悪化していないこと
- self-playが悪化していないこと

## 6.6 採否

採用目安:

- apply + exact hash 25%以上
- exact MCTS end-to-end 5%以上
- 95% CI下限 > 1.01
- self-play/legal regress 2%以内
- hash oracle failure 0

効果が小さい場合は、hash call割合とdeck salt scan割合を示して棄却する。

---

# 7. Phase 1B — observable hash cache/差分維持

**推奨モデル: Codex-5.6 Ultra**
**Phase 1Aのmutation primitiveを再利用する。**

## 7.1 仮説

determinized MCTSは`observable_hash(observer)`を各nodeで全再計算する。observer 0/1のpublic identityを差分維持すれば、determinized MCTSを短縮できる。

## 7.2 設計

候補:

```cpp
mutable std::array<uint64_t, 2> cached_observable_hash;
mutable uint8_t observable_hash_valid_mask;
```

または探索専用sidecar。

要件:

- observer自身のhidden reserve IDは見える。
- 相手のhidden reserveはIDを含めず、slot/tier signatureを含める。
- deckはsizeのみ。
- turnを含める。
- `observable_repetition_hash`の現行意味を維持。
- determinization shuffle前後で同observerのobservable hashが同じ。
- editor mutationは両observer cacheをinvalidate。
- exact hashの差分更新と二重にfield loopを増やさない。

## 7.3 採否

- determinized legacy/root/sharedの少なくとも主経路で3%以上
- exact path regress 1%以内
- observable hash differential 100%一致
- cache field追加によるBoard copy regressionを必ず測る

Board肥大化がclone costを悪化させる場合、public BoardではなくMCTS sidecarへ移す。

---

# 8. Phase 2A — rule transitionの小規模高速化

**推奨モデル: Codex-5.6 Max**

このPhaseでは1仮説ずつ別コミットにする。

## 8.1 貴族適格maskのtable化

現行はbonus更新時に全12 noble、各5色を再判定する。

compile-time table候補:

```cpp
NOBLE_MASK_BY_COLOR[color][bonus_count]
```

`bonus_count`以上の要求を満たすnoble bitを立て、最終mask:

```cpp
mask =
    table[0][b0] &
    table[1][b1] &
    table[2][b2] &
    table[3][b3] &
    table[4][b4];
```

要件:

- 12 noble全て
- editorのbonus範囲外値に対するclamp/fallback
- 現行`noble_eligibility_mask`と完全一致
- constexpr検証を追加

## 8.2 packed gems/bonusesの差分更新

着手処理で既に色ごとにold/new値を触るため、終了後に再度5色loopでpackしない。

例:

```cpp
packed &= ~(FIELD_MASK << shift);
packed |= uint64_t(new_value) << shift;
```

またはdelta加減算。ただしborrow/carryを避ける。

要件:

- public setter/snapshot loaderでは現行`sync_packed()`を維持
- trusted transitionだけ差分
- debugでarray→pack oracle一致
- hash delta helperと同じfield mutationを共有し、loopを重複させない

## 8.3 profile-gated derived masks

次はBoard fieldとして即追加しない。

- bank nonzero 5-bit mask
- bank count>=4 5-bit mask
- reserved occupancy
- total gems

Boardサイズ増加とcopy regressionを先に測る。局所計算の方が速い可能性もある。

## 8.4 採否

- purchase/apply micro 10%以上、またはself-play 1〜3%以上
- legal count/codes regress 1%以内
- Board size/copy悪化なし
- oracle一致

---

# 9. Phase 2B — 合法手count/code生成の専用化

**推奨モデル: Codex-5.6 MaxまたはUltra**

## 9.1 仮説1: code一回列挙

比較する候補:

### A. 現行

```text
count pass → exact reserve → emit pass
```

### B. 固定scratch

```cpp
std::array<uint64_t, MAX_MOVES> scratch; // zero初期化しない
uint16_t count = emit_codes(scratch);
return std::vector<uint64_t>(scratch.begin(), scratch.begin() + count);
```

### C. vector一回列挙

workloadに適した初期capacityを使う。固定値をREADME fixtureだけに過学習させない。

### D. size予測

base action数とtoken excessから安価に上限/近似を求める。

A/B/C/Dを同じfixture matrixで測り、合法手が5件と250件の両方を確認する。

## 9.2 仮説2: return countの閉形式

正規到達局面ではtoken actionによる必要返却数は0〜3。
6色のavailableについて:

```text
n1 = count(available[color] >= 1)
n2 = count(available[color] >= 2)
n3 = count(available[color] >= 3)
```

返却解数:

```text
excess 0: 1
excess 1: n1
excess 2: C(n1,2) + n2
excess 3: C(n1,3) + n2*(n1-1) + n3
```

要件:

- exhaustive brute-force oracleと比較
- editor stateでexcess > 3なら現行再帰へfallback
- `MAX_MOVES` capを維持
- token no-op除外等、現行の意味を勝手に変更しない

## 9.3 仮説3: purchase payment count専用DP

count APIでは全`Action`を生成せず、各cardについてbounded compositionの数だけ求める。

状態例:

```text
dp[color_index][gold_used]
```

制約:

- effective cost
- colored gems
- gold count
- simple payment mode
- cardごとの全gold allocation
- `MAX_MOVES` cap

全card・全到達可能resource stateについて、現行列挙件数と比較する。

## 9.4 仮説4: return/payment patternの表引きemit

- 6色、sum 0〜3のreturn patternをconstexpr列挙
- 現行再帰と同じ順序
- available capacityでmask/filter
- purchase gold patternはcard cost上限が固定なのでcard別pattern tableを利用可能
- player状態により不可能なpatternを高速filter

巨大な一般LUTを導入せず、I-cache/RSSも測る。

## 9.5 仮説5: 内部packed code sink

`MoveGenerator`内部でwide `Action`を作り、最後にpackする経路と、直接`uint64_t` codeをemitする経路を比較する。

公開`Action` APIは維持する。

- `legal_action_codes`はcode sink
- `legal_action_count`はcount sink
- `legal_actions`はAction sink
- 共通の順序・制約logicを共有し、3実装へ規則を複製しない

## 9.6 テスト

- count == emitted actions size
- code list == pack(action list)
- ordering digest一致
- 全Actionをapplyできる
- apply後state一致
- V2/V3 encode/decode一致
- forced passは通常手0件時だけ
- MAX_MOVES capの境界
- editor state fallback
- 10,000以上のrandom reachable states

## 9.7 採否

workload別:

- `legal_action_count`: 10%以上
- `legal_action_codes`: 3%以上
- self-play: 3%以上
- Python `legal_actions`: regression 2%以内

一回列挙が250件では改善しても5件で悪化する場合、adaptive strategyを検討する。ただし分岐判定自体も測る。

---

# 10. Phase 3A — solverの低リスクcontainer/metadata削減

**推奨モデル: Codex-5.6 Max**

## 10.1 再帰path setをstackへ

現行の再帰pathは深さが小さいLIFO構造なのに`unordered_set`を使う。

候補:

```cpp
std::vector<DepthStateKey> path;
```

または上限が証明できる固定stack。

- cycle判定はlinear scan
- RAII guardでpush/pop
- reserveを一度行う
- 深さ8前後ならhash tableより有利な可能性が高い
- proof DAG等の再帰深さも確認
- path length histogramをPhase 0で測る

同一keyがpathにある場合のstatus semanticsを維持。

## 10.2 card equivalenceをconstexpr class IDへ

現行の`std::set<CardEquivalenceKey>`を枝ごとに作らない。

- 90 cardを`level/points/bonus/cost`でcompile-time分類
- `CARD_EQUIVALENCE_CLASS[90]`
- 既出classをbitsetで管理
- class数が64超なら2-word bitset
- representative card順を現行と一致
- compile-timeまたはunit testでtuple equalityとclass equalityを双方向確認

## 10.3 temporary vector削減

対象:

- reveal candidate list
- ordered actions
- per-action edge list
- TT trim scratch
- principal line materialization

reuse可能なscratchはsolver instanceに置くか、再帰frameへ固定bufferを持たせる。
再入可能性・並列solver instance・exception safetyを壊さない。

## 10.4 reason stringをenum化

`VisibleOnlySolver::Entry`の`std::string reason`等、hot TT entry内の可変長文字列をenum/小整数へ置換し、出力境界で文字列化する。

公開結果の文字列は現行と同じ。

## 10.5 map操作

- `find`後の`operator[]`による二重lookupを避ける
- `try_emplace/insert_or_assign`を使い分ける
- hash計算回数counterを追加
- reserve targetは実測に基づく

## 10.6 採否

- exact revealまたはvisible-onlyで3〜10%以上
- node/action/reveal digest完全一致
- RSS改善
- container allocation count大幅減
- 複雑性が小さい

---

# 11. Phase 3B — 厳密めくれ探索の増分set-state/key

**推奨モデル: Codex-5.6 Ultra**

## 11.1 目的

毎nodeの以下をなくす。

- deck全走査によるunseen bitset再構築
- purchased vector全走査によるacquired hidden再構築
- `is_claimed`の多重線形探索
- set-deck board hash全再計算

## 11.2 solver sidecar

例:

```cpp
struct RevealSearchState {
    CardIdSet remaining_by_level[3];
    CardIdSet remaining_all;
    CardIdSet acquired_hidden;
    CardIdSet claimed;
    uint64_t rule_hash;
    // 必要ならcard counts, class masks
};
```

公開`Board`へsolver固有fieldを入れない。

`begin_search()`で一度構築し、各branchのapply/rollbackで増分更新する。

## 11.3 canonical fast pathとfallback

公開solverはeditor由来の任意Boardも受けるため、入口で一度だけ検証する。

canonical fast path条件例:

- card ID valid
- visible/deck/reserved/purchasedの物理cardが矛盾なくdisjoint
- deck level一致
- count整合
- provenanceとcountの必要な整合
- duplicate noble等、key意味に影響する条件

条件を満たさない場合:

- 現行のscan-based pathへfallback
- 結果を変えない
- fast/fallback利用数をcounterへ記録

## 11.4 増分key

`compute_set_deck_search_hash()`と同じ意味を保つ。

- deck orderとabsolute turnを除外
- bank/visible/nobles/player/state flagsは含む
- remaining card setはkeyの別wordとして保持
- acquired hidden mask semanticsを維持
- points/counts/reserved count等のcollision guard metadataを勝手に削除しない

hash delta primitiveはPhase 1Aと共有可能だが、exact hashとset-deck hashを混同しない。

## 11.5 reveal操作

次の全操作でsidecar delta/rollbackを実装する。

- visible purchase + blank refill
- visible reserve + blank refill
- concrete visible refill
- deck reserveの具体card分岐
- oracle purchase
- oracle reserve
- reserved purchase
- noble acquisition
- failed branch
- early visitor stop
- exception/cancel/node limit

RAII guardで、どのreturn/throw経路でも復元する。

## 11.6 oracle

debug option:

```text
CSPLENDOR_VERIFY_REVEAL_SEARCH_STATE
```

一定間隔または全nodeで:

- remaining bitset == deck scan result
- acquired hidden == provenance scan result
- claimed mask == full scan
- incremental rule hash == full recompute
- state key == reference state key

10M nodeでは全node oracleが遅すぎるため、debug testとsampled Release instrumentationを分ける。

## 11.7 採否

- state key micro 2倍以上、または
- exact reveal end-to-end 8%以上
- node/legal/TT/action/reveal digest完全一致
- fallback fixture一致
- RSS悪化なし

---

# 12. Phase 3C — solver TT key/entry圧縮とflat table

**推奨モデル: Codex-5.6 Ultra**

二段階で行う。

## 12.1 Stage 1: 現行`unordered_map`のまま圧縮

まず実測:

```cpp
sizeof(StateKey)
sizeof(DepthStateKey)
sizeof(Entry)
sizeof(unordered_map::value_type)
```

entryを用途別に分離する。

- 一回限り`memo_`
- persistent `exact_memo_`
- visible minimax
- forced bounds
- proof node map

通常memoに不要な:

- generation
- last_touched
- replay metadata
- large `size_t`
- padding

を持たせない。

候補:

- `action_count`: `uint16_t`
- `reveal_card`: `int8_t`
- flags/status: bitfieldではなく明示packed byteも比較
- generation/touchはoverflow semanticsを保つ幅
- metadata wordsを64-bitへ明示pack
- key field orderをpadding最小化

公開結果へ展開するときだけwide型へ戻す。

## 12.2 Stage 2: contiguous open-addressing TT

Stage 1後にprofileでmapが依然hotなら実装する。

要件:

- full keyをslotに保存
- equalityで全field照合
- hash fingerprintだけで同一視しない
- power-of-two capacity
- load factorを0.65/0.75/0.85で比較
- linear/quadratic/Robin Hoodはbenchmarkで選択
- probe length histogram
- empty/occupied/tombstoneを安全に表現
- insert failure/容量上限
- persistent cache trim
- cancellation/exception safety
- deterministic entry replacement
- memory cap
- `max_cache_states`のhard bound

persistent trimは、必要ならkeep対象を選んだ後に新tableへrebuildしてよい。trim時間も測る。

外部flat hash map依存は追加しない。自前版が複雑すぎる場合、結果を報告して止める。

## 12.3 collision test

- hash functionを意図的に低bitへ縮退させるtest mode
- 多数collisionでも正しいkeyを区別
- duplicate insert/update
- wrap-around probe
- full capacity
- erase/rebuild
- persistent reuse
- sanitizer

## 12.4 採否

Stage 1:

- RSS 10%以上、またはNPS 3%以上

Stage 2:

- NPS 5%以上、またはRSS 20%以上
- correctness完全一致
- 95% CI明確
- probe p99が許容範囲
- code複雑性に見合う

---

# 13. Phase 3D — solver専用delta rollback

**推奨モデル: Codex-5.6 Ultra**

## 13.1 方針

公開`Game::undo()`を全面置換しない。
solver内部だけで、枝applyと必ず対になるRAII rollbackを使う。

```cpp
auto guard = SolverBranchGuard::capture(game, reveal_state);
apply...
recurse...
// destructor or explicit restore
```

## 13.2 visible-only

visible-onlyはdeckを空にしてblank refillで進むため、比較的単純。

保存候補:

- acting player mutable fields
- bank
- visible
- nobles
- current player/turn/final/waiting/winner
- provenance vector sizes
- hash cache
- solver sidecar
- mode flagsが変わる箇所

既存`UndoRecord`を再利用・拡張できるか評価する。

## 13.3 exact reveal

deck countだけでは不十分。

具体的に保存する:

- 操作したlevel
- removeしたindex
- remove card
- shiftしたrange、または復元に必要な最小情報
- temporary push/pop
- visible slot old value
- reserved slot shifts
- purchased/acquired append前size
- card set sidecar delta
- exact/set hash old valueまたはinverse delta

複数fieldを変更した後に失敗する経路も復元する。

## 13.4 copy vs delta

次を比較する。

1. Board full copy
2. 既存UndoRecord
3. action-type別union delta
4. generic small snapshot
5. copy-on-writeは原則対象外

action-type別deltaが巨大化する場合、generic fixed snapshotが速い可能性もあるため、実測で選ぶ。

## 13.5 correctness

各action/reveal branchについて:

```text
pre-state snapshot
capture
apply
restore
post-restore full equality
```

比較対象:

- 全field
- deck全要素と順序
- vector内容/size
- packed fields
- noble mask
- cached hash validity/value
- solver sidecar
- mode flags

ASan/UBSan必須。exception/cancel injection testを追加。

## 13.6 採否

- Board copy/assignment数を大幅削減
- solver NPS 5%以上
- node/order digest完全一致
- restore oracle failure 0
- codeがaction追加時に壊れにくい

---

# 14. Phase 4A — MCTS compact edge bit-rankとdead metadata

**推奨モデル: Codex-5.6 Max**

## 14.1 bit-rank lookup

compact edgesはaction昇順で、`availability_union`のset bit順と一致する。
actionが存在する場合のindex:

```cpp
const uint64_t lower = mask & (bit(action) - 1);
const size_t index = popcount(lower);
```

これにより`lower_bound`を削減できる。

要件:

- action 0
- action 47
- missing action
- insertion rank
- union更新前後
- dense snapshot materialization
- legacy/concurrent両方
- debug invariant:
  `edges[i].action == i番目のset bit`

## 14.2 virtual loss合計

selectionごとに全edgeのvirtual lossをscanしない。

候補:

```cpp
uint64_t total_virtual_loss;
```

reservation時にincrement、commit/abort時にdecrement。
既存global quiescence counterとは別にnode invariantを持つ。

test-only counter mutation APIも整合させる。

## 14.3 write-only access metadata

全repoを`rg`して次がreadされているか確認する。

```text
ConcurrentTreeState::access_epoch
NodeRecord::last_access
```

eviction、snapshot、debug、binding、traceで未使用なら削除する。

単に`memory_order_relaxed`へ変えるだけで済ませず、不要なRMW自体を消す。

## 14.4 採否

- edge lookup micro 20%以上
- shared tree 1T/4T/8Tの少なくとも主条件で3〜5%以上
- deterministic trace一致
- virtual loss invariant一致
- TSAN pass

---

# 15. Phase 4B — legacy treeのrecord統合・compact node

**推奨モデル: Codex-5.6 Ultra**

## 15.1 Stage 1: 三重map統合

候補:

```cpp
struct LegacyTreeRecord {
    MCTSNode node;
    LegacyNodeAux aux;
    uint64_t last_access;
};
std::unordered_map<uint64_t, LegacyTreeRecord> records;
```

効果:

- key hash計算削減
- bucket traversal削減
- allocation削減
- prune単純化
- clear/set_config単純化

要件:

- public snapshot API維持
- pointer/reference invalidationを監査
- rehash中に保持pointerを跨がない
- prune exact semantics
- tree generation
- pending batchとのownership契約

## 15.2 Stage 2: internal compact node

公開`MCTSNode`はDTOとして残し、内部だけcompact化する。

候補:

```cpp
struct LegacyCompactEdge {
    uint32_t N;
    int32_t virtual_loss;
    float Q;
    uint64_t availability_count;
    uint8_t action;
};
```

node:

- compact edge vector/inline storage
- action union bitset
- base policy
- total visits
- value
- state flags
- last access

検討点:

- exact full-informationでavailability sidecarが不要な場合の簡略化
- determinizationではworld maskごとのavailabilityを維持
- policyは48 denseのままが速いか、edge-localが速いか比較
- edge inline capacityのoccupancy histogram
- public snapshot時だけ48配列をmaterialize

generic `PlayerState` small-vector化の過去失敗を繰り返さず、MCTS node専用に限定する。

## 15.3 採否

Stage 1:

- legacy/root-parallel 5%以上、またはRSS 10%以上

Stage 2:

- speed 5%以上、またはRSS 20%以上
- exact/determinized trace一致
- max tree/prune stress
- public snapshot一致

---

# 16. Phase 4C — parallel MCTSのcounter・reservation・path所有権

**推奨モデル: Codex-5.6 Ultra**

必ず小分けにする。

## 16.1 SearchLedger sharding

高頻度eventを全workerが同じatomicへ加算しない。

候補:

```cpp
struct alignas(64) WorkerCounters { ... };
```

- workerごと
- coordinatorごと
- roleごと
- search終了時reduce

公開`SearchLedgerSnapshot`は現行と同値。

cancel/failure中もexact countを保つ。
外部からactive search中にcounterを読む契約があるなら監査し、必要なfieldだけatomicを残す。

## 16.2 tree quiescence counter sharding

- shard-local counter
- worker-local delta
- audit時sum

ただし`validate_quiescent_fast()`のO(1)性を保つ。shard数64のsumは厳密にはO(shards)なので、契約上許されない場合はglobal counterを残すか、低頻度batched RMWを使う。

## 16.3 reservation occupancy計測

まずnodeごとの同時live reservation数histogramを取る。

その後、`std::unordered_set<uint64_t>`を次と比較する。

- inline fixed small set + spill
- sorted small vector
- unsorted small vector linear scan
- dense generation token
- bitsetはID範囲次第

典型占有数が0〜4ならsmall linear structureが有力。

## 16.4 reservation ID

現行のtree-global monotonic atomic採番が必要か、API/test/trace契約を監査する。

最低条件:

- 同一node内でlive tokenが一意
- stale tokenが別reservationをcommitできない
- move後/destructor exactly-once
- overflow検出
- trace/replay契約

global uniquenessが公開契約でなければ、node mutex内のnode-local monotonic IDを検討する。
外部に観測可能な意味がある場合は維持する。

## 16.5 path ownership

各path entryが同じtree/ledger shared_ptrを繰り返し保持しない。

候補:

```text
ReservedPath owns tree + ledger
entry owns NodeHandle + action + token
```

またはstable node index/generation。

要件:

- tree clear/replacement中のlifetime
- stale generation
- exception
- move-only
- destructor abort
- partial commit禁止

## 16.6 採否

- shared sharded 8Tで10%以上、またはCI下限1.05以上
- 1T regression 3%以内
- atomic RMW/LLC/futex減少
- TSAN/soak/replay pass
- virtual loss balance
- ledger exact equality

---

# 17. Phase 4D — queue・worker pool（profile-gated）

**推奨モデル: Codex-5.6 Ultra**

Phase 0/4Cでqueue/thread lifecycleが上位hotspotの場合だけ実行。

## 17.1 bounded ring buffer

`std::deque`を固定capacity ringへ置換する。

- 同じmutex/CVで開始
- push/pop bulk API
- close semantics
- queued items drain可能
- blocked producer/consumer wake
- cancellation
- spurious wake
- destruction

lock-free化は別提案にする。

## 17.2 persistent worker pool

searchごとのthread生成がmaterialな場合だけ。

要件:

- MCTS instance lifecycle
- Python interpreter/GIL境界
- fork安全性
- config/tree generation変更
- exception propagation
- clean shutdown
- no background work after API return
- sanitizer

## 17.3 採否

- synthetic zero-latencyだけでなく50〜1000 µs latency matrix
- small 256/1024 simulation searchも改善
- long search regressなし
- correctness/lifecycle全通過

---

# 18. Phase 5A — 48手indexからの直接trusted transition

**推奨モデル: Codex-5.6 MaxまたはUltra**

## 18.1 仮説

現在のnative MCTSは48手indexを`Action`へdecodeし、`Game::apply_trusted()`がcard IDからvisible/reserved sourceを再検索する。

48手indexはsource slot/deckを既に表すため、内部専用経路で直接遷移できる。

## 18.2 API候補

```cpp
bool Game::apply_mcts_action_index_trusted(
    int action_index,
    bool record_history = false);
```

より望ましいのはpublic Game APIへ露出せず、`mcts_internal`のfriend/internal helperに置くこと。

## 18.3 要件

- 48 action schemaを一箇所だけ正とする
- visible slot 0〜11
- reserved slot 0〜2
- deck reserve 0〜2
- take different/same
- canonical payment
- noble
- invalid index
- forced passは48 policy外の現行処理を維持
- blank refill
- exact hash delta
- observable hash delta
- purchased/bonus/noble/final round

## 18.4 differential

random reachable stateごとに全legal 48 indexについて:

```text
A = clone; decode_trusted + apply_trusted
B = clone; direct apply
```

比較:

- full board
- exact hash
- observable hash
- feature
- action mask
- winner/current player
- subsequent legal list

## 18.5 採否

- decode+apply micro 10%以上
- MCTS end-to-end 2〜5%以上
- code duplicationが小さい
- schema test完全一致

---

# 19. Phase 5B — MCTS専用provenance-free clone

**推奨モデル: Codex-5.6 Ultra**
**clone/vector allocationが5%以上のcyclesを占める場合だけ。**

## 19.1 仮説

`clone_light()`はhistoryを除外するが、`Board`内の次のvectorはdeep copyする。

- `purchased_cards`
- `acquired_nobles`

MCTSの現行rule、feature、hash、winnerは主に以下を使う。

- purchased count
- bonuses
- points
- noble mask
- reserved
- board/deck state

card ID provenance自体は通常MCTS traversalで不要。

## 19.2 設計

候補:

1. `SearchGame`専用型
2. internal `clone_for_mcts_search()`
3. `record_provenance=false` mode
4. root変換時にprovenance vectorを空にし、以後appendしない

公開`Game::clone_light()`の意味を変えない。

exact reveal solver、snapshot、replay、Pythonには適用しない。

## 19.3 監査

次がvector内容を参照しないことをwhole-repo call graphで確認する。

- MCTS exact hash
- observable hash
- StateEncoder
- ActionEncoderCpp
- legal generation
- winner
- determinization
- repetition logic
- debug feature signature
- trace

## 19.4 differential

- exact/determinized
- 全seed trajectory
- hidden reserve
- purchase/noble
- retained tree
- feature/hash/mask/terminal一致
- allocator count

## 19.5 採否

- clone micro 20%以上
- MCTS end-to-end 3%以上
- public semantics不変
- branch complexityが許容

---

# 20. Phase 5C — 内部compact actionとPython buffer API

**推奨モデル: Codex-5.6 Ultra / API部分はMax**

## 20.1 内部compact action

公開`Action`は維持し、hot path用に以下を比較する。

- packed `uint64_t`
- action-type別small descriptor
- 48-index
- source slot + payment/return code

適用対象:

- legal codes
- solver ordered action
- MCTS traversal
- self-play random selection

公開境界だけ`Action::unpack()`する。

## 20.2 wide MoveList初期化回避

`std::array<Action, MAX_MOVES>`全体の既定構築がprofileでmaterialなら:

- primitive packed scratch
- placement constructionした要素だけdestroy
- `std::array<uint64_t, MAX_MOVES>`の未初期化local
- small vectorではなくusage別buffer

UBを導入しない。sanitizer必須。

## 20.3 Python code buffer

既存:

```python
game.legal_action_codes  # list[int]
```

を維持し、additive APIを検討する。

例:

```python
game.legal_action_codes_numpy()
game.fill_legal_action_codes(out)
```

- NumPy contiguous `uint64`
- C++から直接copy
- lifetime安全
- GIL
- existing list API unchanged
- Python consumerが実際に利用できる場合だけ採用

## 20.4 採否

- internal path 3%以上
- Python boundaryはlist比20%以上、かつ利用側変更が現実的
- API追加はdoc/test付き
- schema/order不変

---

# 21. Phase 6 — Linux x86 native、LTO、PGO

**推奨モデル: Codex-5.6 Max**

アルゴリズム変更と別commit・別表で報告する。

## 21.1 Linux x86 local native profile

現行`CSPLENDOR_CPU_TARGET=native`はApple Silicon限定。
Linux x86_64のlocal editable/direct build向けに:

```text
-march=native
-mtune=native
```

を追加する案を実装する。

要件:

- portable default
- portable wheelのみ
- native wheel禁止
- cross build禁止
- compiler flag検査
- selected flagsをCMake outputへ表示
- build directory分離
- README更新

POPCNT/BMI等によるbit-rank高速化を個別に確認する。

## 21.2 LTO

option:

```cmake
CSPLENDOR_ENABLE_LTO
```

- Releaseのみ
- compiler capability check
- Python moduleとnative benchmark
- build time/size
- portable互換性
- duplicate symbol/visibility

## 21.3 PGO

profile generate/useを別build directoryで行う。

training corpus:

- legal count/codes/self-play
- exact/determinized MCTS
- exact reveal
- Python module import/主要binding
- fixtureを1つに偏らせない

profile mismatch、stale profileを検出し、silent reuseしない。

## 21.4 報告

次を分離する。

| build | algorithm commit | flags | workload | speed |
|---|---|---|---|---|
| portable | same | baseline | ... | ... |
| native | same | march native | ... | ... |
| portable LTO | same | lto | ... | ... |
| native PGO | same | native+pgo | ... | ... |

## 21.5 採否

- local useで3%以上なら採用候補
- portable regressionなし
- wheel guard完全
- correctness test pass
- PGO training/利用手順再現可能

---

# 22. 横断テストmatrix

## 22.1 Python

```bash
python -m pip install -e ".[dev,web]"
python -m pytest
python -m compileall -q csplendor
python -m pytest -m performance
```

## 22.2 native Release

```bash
cmake -S . -B build/native-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DCSPLENDOR_BUILD_PYTHON_MODULE=OFF \
  -DCSPLENDOR_BUILD_NATIVE_TESTS=ON

cmake --build build/native-tests --parallel
ctest --test-dir build/native-tests --output-on-failure
```

## 22.3 ASan + UBSan

```bash
cmake -S . -B build/asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCSPLENDOR_BUILD_PYTHON_MODULE=OFF \
  -DCSPLENDOR_BUILD_NATIVE_TESTS=ON \
  -DCSPLENDOR_SANITIZER=address-undefined

cmake --build build/asan --parallel
ctest --test-dir build/asan --output-on-failure
```

## 22.4 TSan

parallel Phaseで必須。

```bash
cmake -S . -B build/tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCSPLENDOR_BUILD_PYTHON_MODULE=OFF \
  -DCSPLENDOR_BUILD_NATIVE_TESTS=ON \
  -DCSPLENDOR_SANITIZER=thread

cmake --build build/tsan --parallel
ctest --test-dir build/tsan --output-on-failure
```

## 22.5 random differential

最低1,000 seed、可能なら100,000局面。

確認:

- ordered Action equality
- packed code equality
- count equality
- apply equality
- exact hash oracle
- observable hash oracle
- V2/V3 encode/decode
- 48 mask/decode/direct apply
- simple/full payment
- blank refill
- forced pass
- noble wait
- final round
- draw
- editor state
- snapshot
- deterministic shuffle
- portable RNG
- replay

## 22.6 parallel soak/failure injection

- scheduler seed variation
- 1/2/4/8/16 threads
- callback exception
- timeout
- cancellation
- capacity reached
- pending owner failure
- waiter attach
- stale generation
- destructor abort
- queue close while blocked
- retained tree
- repeated search
- Python callback seriality

---

# 23. 実装時に避ける案

以下は最初から禁止ではないが、現時点では優先しない。

## 23.1 generic Player small-vector化

過去の一般的なsmall-buffer化はBoardサイズ・copy costを増やし得る。
MCTS nodeやsolver scratch等、occupancyが測定済みの専用構造に限定する。

## 23.2 公開Game historyの全面delta-undo化

Python/editor mutationとundo契約が複雑。
まずsolver内部だけで実施する。

## 23.3 hash-only TT

64-bit hash一致だけで局面同一とみなさない。
完全key比較または現行以上のcollision guardを維持する。

## 23.4 lock-free MPMC queue

mutex queueが本当に上位hotspotか確認するまで実装しない。
memory ordering、close、exception、lifetimeのリスクが高い。

## 23.5 外部Swiss table依存の即時導入

flat/open-addressingの有効性は高い可能性があるが、まずrepo内専用TTで効果を検証する。依存追加は別承認。

## 23.6 search algorithm変更との混在

以下はpure performance Phaseと分ける。

- pruning
- move ordering変更
- transposition replacement policy変更
- forced playout tuning
- cpuct/FPU
- virtual loss weight
- determinization数
- attacker action limit
- reveal equivalence定義
- proof simplification

探索量削減は別研究Phaseとして、棋力・完全性評価を伴って行う。

---

# 24. Codexの各Phase報告書式

各作業の最終回答は必ず次の形式にする。

## 24.1 状態

```text
Target phase:
Baseline commit:
Working commit:
Branch:
Working tree status:
Compiler:
Build flags:
CPU affinity:
```

## 24.2 仮説

```text
変更前に何がhotだと測定されたか
cycles割合
allocation/RSS
予想される機序
```

## 24.3 変更

| file | symbol | change | compatibility risk |
|---|---|---|---|

## 24.4 correctness

| test | command | result |
|---|---|---|

加えて:

- differential seed/state数
- hash/rollback oracle count
- solver digest
- MCTS trace
- sanitizer

## 24.5 performance

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|

perf:

| metric | A | B | delta |
|---|---:|---:|---:|
| cycles | | | |
| instructions | | | |
| IPC | | | |
| branch misses | | | |
| L1D misses | | | |
| LLC misses | | | |
| atomic RMW | | | |
| allocations | | | |

## 24.6 semantic equality

```text
node count:
legal moves:
TT hits/stores:
action-order digest:
reveal-order digest:
root visits:
tree size:
proof status:
```

## 24.7 結論

次のいずれかを明示。

```text
ACCEPT
REJECT_AND_REVERT
NEEDS_MORE_DATA
```

理由、残存リスク、commit hash、次に実行すべきPhaseを1つだけ記載する。

---

# 25. 初回にCodexへ渡すプロンプト

以下を、この依頼書と一緒に渡す。

```text
このリポジトリの高速化を段階的に進めます。
添付の「csplendor 高速化実装・効果検証依頼書」を正本として読み、
まずAGENTS.mdと現行ソース、テスト、benchmarkを確認してください。

今回はPhase 0だけを実行してください。
高速化実装には進まず、再現可能なbenchmark・instrumentation・paired A/B基盤を追加し、
現行のbaselineとhotspotを測定してください。

重要条件:
- ユーザーの未コミット変更を破壊しない。
- portable既定、公開API、合法手順序、探索量を変えない。
- build生成物や巨大ログをコミットしない。
- full test、native test、必要なsanitizer testを実行する。
- 測定値、コマンド、CPU affinity、compiler、flags、git hashを記録する。
- Phase 0完了後に報告して停止し、Phase 1へ進まない。
- pushしない。
```

---

# 26. 後続Phase用プロンプト

```text
添付の高速化依頼書のPhase <番号>だけを実行してください。

開始前に:
1. AGENTS.mdを読む。
2. git statusとHEADを確認し、ユーザー変更を壊さない。
3. Phase 0 baselineと直近採用commitを再確認する。
4. 今回の仮説以外を同時に実装しない。

実装後:
- reference pathとのrandom differential
- full test
- native test
- 指定sanitizer
- 同一条件paired A/B
- correctness digest
- RSS/perf counters
を実行してください。

採用基準を満たさなければ実装をrevertし、結果だけ記録してください。
採用時は1つの独立commitにしてください。
報告後に停止し、次Phaseへ進まないでください。
pushしないでください。
```

---

# 27. 完了条件

本高速化計画全体の完了は、単に全Phaseを実装した時点ではない。

以下を満たした採用Phaseだけを統合する。

- correctness gate全通過
- portable build全通過
- public API/schema互換
- exact solver完全性維持
- deterministic replay維持
- primary workloadで統計的に再現する改善
- memory/complexityが効果に見合う
- benchmarkとdoc更新
- accepted/rejected experimentの記録
- clean working tree
- 各最適化を独立commitとして追跡可能

最終報告では、次を別々に示す。

1. アルゴリズムを変えないC++実装高速化
2. MCTS shared-tree高速化
3. solver高速化
4. Python boundary高速化
5. native/LTO/PGOによるbuild profile差
6. それぞれの単独効果
7. 全採用変更を積み上げたend-to-end効果
8. 組合せによる非加算性
9. 残ったhotspot
10. 次の研究課題

---

# 付録A — 最初に確認すべきシンボル一覧

```text
src/board.h
  Board::hash
  Board::compute_hash_impl
  Board::observable_hash
  Board::randomize_hidden_information_impl

src/game.h
  Game::clone_light
  Game::copy_current_state
  Game::apply_unchecked
  Game::legal_action_count
  Game::legal_action_codes
  Game::apply_random_action

src/move_generator.h
  MoveGenerator::count_all_fixed
  MoveGenerator::consume_all_capped
  MoveGenerator::emit_purchase_options
  MoveGenerator::emit_gold_as_combinations
  MoveGenerator::count_return_combinations
  MoveGenerator::emit_return_combinations

src/player.h
  PlayerState::sync_packed
  PlayerState::update_noble_eligibility
  PlayerState::can_afford

src/rule_transition.h
  purchase_card
  return_gems_unchecked
  acquire_noble_unchecked
  finish_standard_action

src/mcts_game_adapter.h
  GameAdapter::tree_key
  GameAdapter::clone_for_batch
  GameAdapter::decode_and_apply_native

src/mcts_tree.h
  MCTS::get_node
  MCTS::get_or_create_node
  MCTS::select_action_with_virtual_loss_for_world_bits
  MCTS::backpropagate
  MCTS::prune_if_needed
  LegacyNodeAux::ensure_edge

src/mcts_concurrent_tree.h
  ConcurrentTree::find
  ConcurrentTree::find_or_create
  ConcurrentTree::select_and_reserve_bits
  NodeRecord
  SearchLedger
  SelectionReservation
  ReservedPath

src/mcts_bounded_queue.h
  BoundedQueue

src/visible_only_solver.cpp
  forced_win
  minimax
  representative_actions
  state_key

src/reveal_verified_solver.cpp
  forced_win
  state_key
  for_each_search_outcome
  for_each_proof_outcome
  for_each_visible_refill_outcome
  for_each_deck_reserve_outcome
  deck_reserve_cards
  visible_refill_cards

src/reveal_solver_components.cpp
  HiddenOutcomeCatalog::is_claimed
  HiddenOutcomeCatalog::unseen_cards
  HiddenOutcomeCatalog::acquired_hidden_cards

src/undo_record.h
  UndoRecord

scripts/benchmark_mcts_optimizations.cpp
scripts/benchmark_mcts_parallel.cpp
scripts/benchmark_solvers.py
tests/test_perf.py
tests/mcts_optimization_unit.cpp
tests/mcts_parallel_unit.cpp
tests/mcts_parallel_stress.cpp
tests/state_invariants_unit.cpp
tests/state_copy_unit.cpp
tests/rule_query_unit.cpp
```

# 付録B — 期待値の扱い

静的解析上の参考レンジ:

| Phase | 参考レンジ |
|---|---:|
| exact incremental hash | exact MCTS 5〜20% |
| observable incremental hash | determinized MCTS 3〜15% |
| noble/packed update | self-play 1〜5% |
| legal count/code specialization | 対象API 3〜50%、self-play 3〜10% |
| solver low-risk containers | 3〜15% |
| incremental reveal state/key | 8〜25% |
| compact/flat solver TT | 5〜25%、RSS大幅減の可能性 |
| solver rollback | 5〜25% |
| edge rank/dead atomics | shared MCTS 3〜15% |
| legacy record/compact node | 5〜20%、RSS改善 |
| parallel counters/reservations | 8Tで5〜30% |
| direct MCTS transition | 2〜8% |
| provenance-free clone | 2〜15%、profile依存 |
| native/LTO/PGO | 3〜20%、CPU/workload依存 |

これらは保証値ではない。重複するcostを削るPhase同士は、後段ほど効果が縮む。
必ず各単独commitと累積HEADの両方を測る。
