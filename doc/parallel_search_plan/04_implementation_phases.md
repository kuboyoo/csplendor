# 04. 段階的実装計画

実装状況: **PS-0〜PS-11のcore実装済み。PS-7〜PS-10はpublic API経由の決定的race fixture、
canonical trace、sanitizer/stressによる強化検証まで完了**。長時間/可変scheduleのnightly、
PS-12の広い性能・実NN・品質matrix、PS-13のstable化gateは継続中であり、
複数threadはStage B experimental opt-inを維持する。実装結果は
[implementation_status.md](implementation_status.md)、固定実機結果は
[benchmark_results.md](benchmark_results.md)を参照する。

> 推奨統括・各phase担当: **Codex Sol Ultra**
> 方針: 利用環境で実際に選択できるSol Ultraを全partで使う。トークン節約はphaseごとに
> 対象file、contract、test、failure traceだけを渡すことで行い、各gateを満たすまで次へ進まない。

## 1. 依存関係

```text
PS-0 baseline
  |
  v
PS-1 information-set correctness
  |\
  | +--> PS-2 build/sanitizer infrastructure
  v
PS-3 random context
  |
  v
PS-4 lifecycle / TreeKey / snapshots
  |
  v
PS-5 single-thread ticket/reservation core
  |\
  | +--> PS-6 root-parallel oracle
  v
PS-7 coarse shared tree
  |
  v
PS-8 scheduler / inference coordinator / cancel
  |
  +--> PS-9 deterministic epoch / replay
  |
  v
PS-10 sharded tree
  |
  v
PS-11 Python binding
  |
  v
PS-12 benchmark / tuning / quality
  |
  v
PS-13 rollout / final audit
```

PS-2はPS-1と並行可能だが、shared tree実装はPS-1完了前に開始しない。

## 2. 変更単位の原則

- 1 partにつき1つの主要な意味論だけ変更する。
- correctness修正、並列構造、性能最適化を同じpartに混ぜない。
- 各partでsingle-thread oracleを先に通す。
- TSANが必要なpartはnative harnessを先に作り、Python integrationを後にする。
- failure時のseed/traceを保存してから修正する。flaky再実行で成功扱いにしない。
- 既存のdirty worktreeや他機能の変更を巻き込まない。
- `build/`、`.so`、egg-info、cache等を成果物へ含めない。
- 現行`csplendor_core`はCMake `INTERFACE` targetなので、parallel coreは当面header-onlyで
  追加する。`.cpp`分割はbuild/profile上の必要が確認できた場合に別変更とする。

## 3. PS-0: baseline・characterization・race probe

> 実装担当: **Codex Sol Ultra**
> Review: 別sessionの **Codex Sol Ultra**
> 即時停止条件: 現行結果のどれを互換契約にするか追加判断が必要になった場合。

### 目的

並列化前の正しい契約と、既知の誤った挙動を区別して固定する。

### 候補ファイル

```text
tests/test_parallel_mcts_characterization.py
tests/mcts_parallel_baseline.cpp
scripts/benchmark_mcts_parallel.cpp
doc/parallel_search_plan/baseline_results.md（実測時のみ）
```

### 作業

1. determinization offの固定root corpusと、determinization on/world=1のinvariant corpusを作る。
   現行`random_device`経路のbitwise goldenは作らず、PS-3の明示seed導入後に固定する。
2. tree snapshot digestを定義する。
   - sorted TreeKey
   - state、valid、prior、N、Q、VL、total visits
3. path/request/result digestを保存する。既知hash集合をsortして全node DTOをserializeする
   test-only tree snapshot digestも作る。
4. terminal、MAX_DEPTH、forced playout、Dirichletのshape/normalization、callback exceptionを含める。
5. 現行のhidden tier hash aliasを再現するcharacterization fixtureを作る。
6. divergent public leafをworld 0へmergeするfixtureを作る。
7. world-local mask差とdecode failureを再現する。
8. 同じMCTSへの別Python threadアクセスが危険であることを、release testではなく
   TSAN導入後に使うnative race probe仕様として記述する。
9. 既存native benchmarkを1/2/4/8 worker対応前の1-thread基準として保存する。

### Gate

- 正しいdeterminization-off single-thread契約がgolden/digest化され、determinization-onは
  PS-3までinvariant testで固定される。
- blocker fixtureは「現状を再現」し、PS-1で期待値を反転できる。
- benchmark条件、CPU pinning、compiler、sample数が記録される。
- 通常testを一切悪化させない。

## 4. PS-1: information-set correctness

> 担当: **Codex Sol Ultra**
> 独立review: 別sessionの **Codex Sol Ultra**

### 目的

shared treeが共有してよい状態の同値関係を正す。

### 候補ファイル

```text
src/zobrist.h
src/board.h
src/state_encoder.h
src/mcts_game_adapter.h
src/mcts_orchestration.h
src/mcts_tree.h
src/mcts_types.h
tests/test_hash_mutation_review.py
tests/test_determinization.py
tests/test_mcts_information_set.py
tests/test_mcts_correctness_review.py
```

### 作業

1. hidden reservedのslot/empty/tier saltを追加する。
2. observer/domain/mode/key versionを持つ`TreeKey`を導入する。
3. full/observable key生成を一箇所へ集約する。裸`uint64_t`の既存APIは
   `LegacyExact`専用facadeへ固定し、新parallel treeとの自動変換を禁止する。
4. worker/current-world maskを選択APIへ渡せる下地を作る。
5. node policyを未mask base policyとして扱い、current-world mask上で選択する契約を作る。
   parallel callbackがmasked policyだけを返すlegacy contractとは分離する。
6. decode/apply失敗をdraw backpropしない。
7. `num_determinizations>1`の異なるleaf keyをmergeしない。
8. 初期parallel対象ではworld=1へ制限し、legacy呼出しへのerror/messageを定義する。
9. root observer固定SO-ISMCTSの範囲を文書化する。
10. hash/key version変更に伴うtree再利用境界をtestする。

### Tests

- hidden IDだけ変更: same key。
- slotごとのempty/tier vector変更: different key。同tier hidden IDだけをslot交換してvectorが
  不変ならsame key。
- observer/domain/mode変更: different key。
- 旧`uint64_t` signatureはLegacyExact treeだけを操作し、parallel TreeKey nodeを参照できない。
- determinization前後: observer public key不変。
- known corpusではfeature差とTreeKey同一のfixtureが0。runtime feature digest検査はpending
  owner/waiterのdedup中に限定され、展開済みnodeへの二次signature照合はstable hardeningへ残す。
- reveal outcomeが違うworld: different leaf nodes。
- current-world unavailable action: selection対象外。
- owner worldではunavailable、後発waiter/worldでavailableになったaction: node lock内で
  union更新後にcandidateを作り、base priorを保持して実際に選択可能。
- availability count: unavailableだったsimulationをforced playout母数へ含めない。
- invalid replay: visit/VL残留なし。
- random corpusでpublic fingerprint/key/feature整合。

### Gate

- [01のPS-1完了ゲート](01_current_state_and_decisions.md#8-ps-1完了ゲート)を満たす。
- blocker fixtureが正しい期待値へ反転する。
- serial MCTSの正しい既存契約が維持される。
- hash/action/encoder/determinizationのfull testが成功する。

## 5. PS-2: native build・CTest・sanitizer基盤

> 実装・sanitizer option review・文書更新: **Codex Sol Ultra**

### 目的

Python runtimeに依存しないparallel coreのrace oracleを常時実行できるようにする。

### 候補ファイル

```text
CMakeLists.txt
cmake/Sanitizers.cmake
tests/CMakeLists.txt
tests/mcts_parallel_test_support.h
tests/mcts_parallel_unit.cpp
tests/mcts_parallel_stress.cpp
tests/mcts_parallel_replay.cpp
.github/workflows/sanitizers.yml
```

### CMake option

```text
CSPLENDOR_BUILD_NATIVE_TESTS=ON|OFF
CSPLENDOR_BUILD_PYTHON_MODULE=ON|OFF
CSPLENDOR_SANITIZER=none|thread|address-undefined
CSPLENDOR_BUILD_PARALLEL_BENCHMARK=ON|OFF
```

### 作業

1. `find_package(Threads REQUIRED)` と `Threads::Threads` を使う。
2. `find_package(pybind11 REQUIRED)`とPython module targetを
   `CSPLENDOR_BUILD_PYTHON_MODULE=ON`のbranch内へ移し、native-only TSAN buildをpybind11なしで
   configure可能にする。
3. sanitizer compile/link flagsをinterface targetへ集約する。
4. native testをCTest label `unit`, `tsan`, `stress`, `replay`へ分ける。
5. Clang/GCCの両方で通常buildを確認する。
6. TSANとASan+UBSanを別buildにする。
7. source内raceをsuppressionしないpolicyを置く。
8. CI artifactへseed/trace/compiler情報を保存する。

`CMakeLists.txt`、`cmake/`、`.github/`は現行AGENTSの列挙外である。本計画全体の実装依頼を
maintainer authorizationとして記録し、PS-2ではCMake/test基盤だけへ変更を限定する。

local環境でbuild isolationがnetwork取得を要求する場合、extension再buildは
`python setup.py build_ext --inplace`を使う。生成物はgit対象外とする。

### Gate

- 通常、TSAN、ASan+UBSanの3 buildが構成できる。
- 意図的race probeをTSANが検出することを一度確認し、probe自体は通常gateから外す。
- sanitizerなしnative unitと既存Python testが共存する。
- build directory以外を汚さない。

## 6. PS-3: `SearchRandomContext`・portable search RNG

> 仕様・utility実装担当: **Codex Sol Ultra**
> golden/cross-compiler review: 別sessionの **Codex Sol Ultra**。

### 候補ファイル

```text
src/mcts_rng.h
src/mcts_tree_key.h
src/board.h
src/game.h
src/mcts_game_adapter.h
src/mcts_types.h
tests/mcts_rng_unit.cpp
tests/test_determinization.py
```

### 作業

1. master seed、nonce、simulation/world/domain IDのcontractを実装する。
2. repository-owned 64-bit RNGとunbiased bounded samplingを実装する。
3. search専用Fisher–Yates determinization overloadを追加する。
4. 既存`Game(seed)`とpublic randomize APIの乱数列を変更しない。
5. world seedとroot Dirichlet seedを分離する。
6. `resolved_seed`、nonce、rng versionをsearch metadataへ出せる型を作る。
7. `seed=None`は`MCTS`構築時に一度entropyを解決し、resultへ`rng_version`を含める。
8. quiescent時専用の`reset_replay_sequence(seed, nonce)`をC++/Pythonへ公開する。
9. 10,000 tupleと1/2/4/8/16 worker assignmentを模したseed manifest testを作る。
10. determinization on/offで`RootDirichlet` seed/noiseがstorage domain差に引きずられないことを検証する。

### Gate

- GCC/Clangでportable shuffle golden一致。
- worker/order/cancelに関係なくlogical tupleのworld digest一致。
- seed domain重複なし。
- explicit seed 0が再現可能。
- random seed選択はsearch開始時一度だけ。

## 7. PS-4: lifecycle・snapshot・safe facade

> 担当: **Codex Sol Ultra**
> 理由: GIL deadlock、active guard、public raw pointer/config APIを同時に整理するため。

### 候補ファイル

```text
src/mcts_tree.h
src/mcts_types.h
src/mcts_lifecycle.h
src/mcts_game_adapter.h
src/bindings.cpp
tests/mcts_parallel_lifecycle.cpp
tests/test_parallel_mcts_bindings.py
tests/test_phase6_mcts_contracts.py
```

### 作業

1. `Idle/Active` search guardとtree generationを導入する。
2. search開始時にroot/config/observer/keyをsnapshotする。
3. active中のclear/prune/config/manual mutationをfail-fastする。
4. `get_node_snapshot()`を追加する。
5. Python `get_node()`をsnapshot経由へ移す。
6. legacy raw pointer APIの利用範囲/deprecationを固定する。
7. `get_config_snapshot()` / `set_config()`を追加する。Python `mcts.config` getterはdetached
   copy、property setterは`set_config()`へ変更し、repo内の`mcts.config.field = value` consumerを
   copy-edit-setへmigrationする。内部referenceを返すPython propertyは削除する。
8. `clear()`とrandom sequence resetを分離する。
9. tree size/countersをthread-safe snapshotで返す。
10. 裸hash既存APIはLegacyExact専用、新`TreeKey` overloadはparallel tree専用に分け、移行中は
    両storageも物理的に分離する。旧signature contract testと新contract testを併存させる。

### Tests

- active guard二重取得を拒否。
- active中clear/prune/config changeを即時拒否。
- retained config referenceからactive設定を変更できない。
- detached config copy変更はengineへ影響せず、`set_config()`だけがIdle時に反映される。
- legacy裸hash APIからparallel/observable nodeへアクセスできない。
- APIが待たず、GIL deadlockしない。
- session終了後は操作成功。
- generation違いrequestを拒否。
- root callback mutationがsnapshotへ影響しない。
- 別MCTS objectは並行利用可能。

### Gate

- lifecycleの全状態遷移がunit testで固定される。
- active終了時にinflight=0でなければfailする。
- existing single-thread APIが既定条件で動く。

## 8. PS-5: single-thread ticket / reservation core

> 担当: **Codex Sol Ultra**

### 目的

threadを増やす前に、exactly-once virtual loss、expansion、commit/cancelを新APIへ移す。
同時にparallel専用`NodeRecord`と64-bit statsをsingle-thread serial coreとして導入し、PS-7が
同じ型・状態遷移へ同期を追加できるoracleを作る。

### 候補ファイル

```text
src/mcts_concurrency.h
src/mcts_parallel_types.h
src/mcts_concurrent_tree.h
src/mcts_tree.h
src/mcts_orchestration.h
src/mcts_searcher.h
tests/mcts_reservation_unit.cpp
tests/test_mcts_correctness_review.py
```

### 作業

1. move-only `SelectionReservation` / `ReservedPath`を実装する。
2. `select_and_reserve()`でselectionとVL addを統合する。
3. `commit()`でVL releaseとN/Q updateを統合する。
4. `abort()`とnoexcept fallback cleanupを作る。
5. tree/node generationをpath entryへ持たせる。
6. request/ticket consumed stateを実装する。
7. parallel専用`NodeRecord`、`NodeStats64`、`TreeKey -> NodeRecord` serial storageをheader-onlyで
   導入し、lookup/select/publish/commit/abortのsingle-thread oracleにする。
8. `SimulationTicket`と別型の`PendingEvaluation{Open, Closing, Published, Failed, Cancelled}`を
   1 threadで導入する。session registryが両者をterminal/drainまでstrong所有する。
   Pending publishはnode evaluationの一度限りの公開だけを担当し、attached ticketはpublish後に
   ticket単位で個別commitする。
9. internal node statsを64-bit化し、parallel snapshotも64-bit DTOにする。legacy 32-bit DTOへの
   overflow変換はerrorにする。
10. legacy pathと新pathのdeterministic digestを比較する。
11. legacy `clear_virtual_losses()`をparallel内部から排除する。
12. availability、N、total visits、VL、live reservationの64-bit加算をmutation前に一括preflightし、
    overflow時にunion/counterの一部だけを書き換えない。

### Tests

- commit/abort exactly once。
- double commit、double abortを拒否。
- destructor fallbackでVL=0。
- malformed resultのvalidation前はmutationなし。
- terminal/MAX_DEPTH/inference exceptionでVL=0。
- stale generationでstats不変。
- `total_visits == sum(N)`。
- `UINT32_MAX`境界を越えてもinternal N/total visitsがwrapせず、legacy DTO変換は明示error。
- `UINT64_MAX`境界でreserve/availability/commitを拒否し、失敗前後のsnapshotが同一。
- Pendingの最後のqueue参照消失、late attach、publish-vs-cancelでも永久Evaluating/VL残留なし。
- 1回のPending publishへ複数ticketをattachしても、publishは1回、各ticket commitは各1回、
  completed budgetはticket数と一致する。
- current forced playout/root noise behaviorの比較。

### Gate

- 1 threadでPS-0の正しいdigestと一致。
- 全failure injection pointでVL=0。
- ticket state/ledger invariant成功。
- 新API以外のVL add/removeをparallel pathが呼ばない。

## 9. PS-6: root-parallel oracle/fallback

> 担当: **Codex Sol Ultra**
> merge semantics review: 別sessionの **Codex Sol Ultra**。

### 目的

共有tree raceなしの複数core比較値とfallbackを得る。

### 作業

1. workerごとに独立MCTS/tree/search seed rangeを持つ。
2. simulation budgetを固定的に分割する。
3. root action `N`を64-bitで合算する。
4. value/Qを単純平均せず、visit-weightedに集約する。
5. Dirichletはsearch共有1配列かworker別かを明示する。
   - oracleでは共有1配列を推奨。
6. worker failure時に全threadをjoinする。
7. memory/tree数/duplicate evaluationを記録する。
8. `max_tree_nodes`は既定50,000のaggregate上限としてactive workerへ均等分配し、budget 0のworkerを
   capacity計算から除外する。
9. 複数workerのpartial理由は
   `Completed < Cancelled < TimedOut < TreeCapacityReached < CallbackError < WorkerError`で集約する。
10. rootが展開済みでvisit 0のcapacity partialでは、legal mask上で正規化したprior（noise設定時は
    search共有noiseを混合）を返す。

### Gate

- 同じworker allocationでreplay可能。
- root mergeのsum(N)とbudget一致。
- shared tree方式が不採用になった場合にもopt-in fallbackとして使える。

## 10. PS-7: global mutex shared tree

> 担当: **Codex Sol Ultra**
> 理由: 最初の本物の共有状態とlinearizationを導入するpartであるため。
> 実績: core実装とlocal sanitizer/stress、public APIを使う決定的race fixtureは完了。
> 可変scheduler seedと長時間nightly gateは未完了。

### 作業

1. PS-5のserial `NodeRecord`/state machineを、stable shared handleを保ったままcoarse shared
   backendへ昇格する。
2. tree map、node stats、LRUをglobal mutexで保護する。
3. Game transition/feature/inference中はtree mutexを持たない。
4. same-leaf owner/waiterを実装する。
5. root bootstrapをsimulation budget外のsearch準備として実装し、bootstrap後に要求された
   logical simulationを全件発行する。
6. commit/cancelとnode publicationをcoarse lock下で正しく動かす。
7. ledgerを追加し、same-leaf owner/waiter、pending close/publish/cancel、terminal/duplicate/
   stale競合をproductionと同じpublic APIで決定的に作るfixtureを追加する。当初予定の
   汎用named hookの全実装を完了条件とはせず、未被覆のinterleavingが判明した場合にだけ
   test-only hookを追加する計画へ修正する。
8. 2/4/8 software threadのnative harnessを作る。
9. tree capacity時は新規発行を止め、全未commit ticketをreason付きcancelし、
   root展開済みなら`TreeCapacityReached` partial resultを返す。root未展開なら
   `TreeCapacityReachedError`を送出する。ephemeral評価は実装しない。
10. shared-treeの`max_tree_nodes`は既定50,000の単一tree hard limitとし、root展開済み・visit 0の
    partialはmasked normalized prior/noise fallbackで有効なdistributionを返す。

### Race scenarios

- same root edge同時reserve。
- same leaf同時claim。
- expand途中reader。
- terminal vs NN completion。
- reverse completion。
- cancel/add-VL直後。
- stale/duplicate result。
- active中clear/prune。
- tree capacity/MAX_DEPTH。

### Gate

- native TSAN full scenarioが0 report。
- coarse版deterministic traceがsingle-thread modelと一致。
- exception/cancel後に再利用可能。
- deadlock/hangが0。
- 性能はこのpartの採否条件にしない。

## 11. PS-8: worker scheduler・inference coordinator

> 担当: **Codex Sol Ultra**
> shutdown/GIL review: 別sessionの **Codex Sol Ultra**。
> 実績: core実装済み。`num_threads=1`はcaller-thread serial path、`num_threads>=2`は
> traversal workerと単一inference coordinatorのpipelineになった。

### 候補ファイル

```text
src/mcts_parallel_searcher.h
src/mcts_work_queue.h
src/mcts_parallel_types.h
tests/mcts_parallel_scheduler.cpp
```

### 作業

1. fixed worker poolとbounded queuesを実装する。
2. coordinatorがsimulation ID付きtaskを発行する。
3. owner evaluationをbatchへまとめる。
4. waiter pathをpending ticketへattachする。
5. fake/native inference callbackでpipelineを完成させる。
6. stop、timeout、exception、queue closeを一つのshutdown pathへ統合する。
7. metricsをworker-localに集めて終了時reduceする。
8. max inflightとbackpressureを実装する。
9. copy間で状態を共有する`ParallelCancellationToken`をC++/Pythonへ追加し、pre-cancelと
   in-flight cancelの両方でbalanced partial result、VL=0、MCTS再利用を保証する。
10. throughputのactive ticket registryは完了ごとにeraseして`O(max_inflight)`、deterministic側は
    epochごとにclearして`O(deterministic_epoch_size)`にする。
11. worker entryをnoexcept境界にし、例外を共有slotへ保存してqueue closeでcoordinatorをwakeする。
12. callback例外を専用boundaryで保持し、callback由来`TreeCapacityReachedError`を内部capacity
    partialへ誤分類せず元のtype/messageで再送出する。

実装中の計画修正:

- 1 threadはqueue/thread pool overheadを避けるserial pathとし、複数thread時だけpipelineを作る。
- `timeout_ms`はqueue待機とcallback境界で観測するsoft deadlineとする。実行中callbackの強制中断は
  lifetime/GILを壊し得るため行わず、無期限blockするevaluatorは外部watchdogで隔離する。
- root-parallelはAPI entryからのend-to-end soft deadlineを使い、evaluator factoryと共通
  bootstrapの時間を差し引いた残余時間だけをworkerへ渡す。
- Python root-parallelのserialized callbackはmutex取得後にtimeout/cancelを再検査し、遅いcallbackの
  後ろに溜まったstale waiterをPythonへ順次流さない。

### Gate

- 1/2/4/8 software threadでscheduler/stress完走。16 worker割当は10,000 tuple seed manifestで
  検証済みとし、16-thread scheduler soakはnightly拡張に残す。
- queue close/handoffにlost wakeupがない。
- 全inference completion順でledger一致。
- max inflightを超えない。
- callback/worker exception後に全thread join。
- callback中にtree/node/queue lockを保持しない。

## 12. PS-9: deterministic epoch・trace replay

> protocol・serializer・runner担当: **Codex Sol Ultra**。
> 実績: trace/replay coreは実装済み。parallel completion reorderの検証runnerではなく、
> 単一coordinatorによる決定的protocol oracleへ計画を修正した。

### 作業

1. epoch sizeとlogical orderをconfig化する。
2. epoch sizeをworker数非依存の固定値にし、coordinatorがsimulation ID順にdeterminization、
   Game transition、world mask、leafまでのfull traversal/reserveを直列実行する。
3. Python callbackの「同時実行数1」契約を優先し、評価callbackもcoordinatorから同期的に呼ぶ。
4. inference batch membership/orderとcommit順をsimulation IDで固定する。
5. `num_threads`は1/2/4/8指定の結果互換性入力として受理するが、このmodeでは並列workerや
   completion reorderを発生させない。
6. event trace schema/versionを定義する。
7. single-thread replay interpreterを作る。
8. failure時artifactを最小化して保存する。

実装中の追加修正:

- trace schema v3はfull-tree/eventのv2を廃止し、変更node deltaとcanonical full-tree
  digest/hash chainを記録する。aggregate snapshotは131,072、現行`MAX_DEPTH`の保守的
  safe event数は約218で、超過予算はtree mutation前に拒否する。
- serializer/parserは`TreeKey`全fieldのcanonical sort、event/path/snapshot cap、enum値、
  duplicate key、非有限統計を検証する。
- strict replayはpathの`N/Q/total_visits`とchained publication deltaを検証する。pending/
  reservation/availabilityの全過渡を再実行する完全state-machine replayではない。

### Gate

- 1/2/4/8指定とcoarse/shardedでcanonical trace bytes/tree digest一致。
- 10,000 tupleのseed manifestが1/2/4/8/16 worker割当で一致する。
- traceをsingle-threadで検証し、各commit後のpath statisticsとchained tree digestが一致。
- throughput modeへdeterministic barrier overheadが混入しない。
- reverse/random completionを含む並列reorder検証はこのmodeの達成済みgateに数えず、throughput
  seeded-schedule harnessの未完了項目として追跡する。

## 13. PS-10: sharded map・node lock

> 担当: **Codex Sol Ultra**
> 実績: coarseと同じ状態機械を使うcore実装、local sanitizer/stress、public API race
> fixture、canonical backend trace、限定固定host benchmarkは完了。
> 長時間nightly、異なるtoolchain、実NN contention/quality gateは未完了。

### 作業

1. coarse backend interfaceを保ったままsharded backendを追加する。
2. shard lockはlookup/insert中だけ保持する。
3. node lock内でselect+reserve、publish、commit/abortを行う。
4. `access_count_`別mapを廃止しnodeの`last_access`へ統合する。
5. node/ticket/queueのlock overlapを除去する。
6. lock wait、contention、owner/waiter率を計測する。
7. shard count、reserve、shared_ptr overheadをbenchmarkする。
8. active中prune禁止を維持する。

### Gate

- coarse版と同じdeterministic trace/digest。
- TSAN/ASan/UBSan 0 report。
- lock-order assertion違反0。
- coarse版より固定実機で改善。
- 1 thread overheadが許容範囲内。

改善しない場合はsharded版を採用せず、coarseまたはroot-parallelを残す。

## 14. PS-11: Python native parallel binding

> 担当: **Codex Sol Ultra**
> GIL/ownership/shutdown最終review: 別sessionの **Codex Sol Ultra**。
> 実績: experimental APIとして実装済み。stable APIへの昇格はPS-13 gate後に判断する。

### 候補ファイル

```text
src/bindings.cpp
src/mcts_parallel_searcher.h
csplendor/__init__.py
tests/test_parallel_mcts_bindings.py
```

### 作業

1. `ParallelSearchOptions`と`ParallelSearchResult`をbindingする。
2. `mcts_search_parallel_native()`をinternal/experimental APIとして追加する。
3. GIL保持中にroot/config snapshotを作る。
4. traversal中はGILを解放する。
5. inference coordinatorだけがGILを取得してPython callbackを呼ぶ。
6. contiguous owning input bufferを渡す。
7. result全件をvalidateしてからtreeへpublishする。
8. callback例外をC++ cleanup後に再送出する。
9. same-MCTS concurrent callをfail-fastする。
10. heartbeat threadでGIL解放を検証する。
11. `ParallelCancellationToken`をbindingし、callback内/別Python threadからのcooperative cancel後に
    balanced partial resultとMCTS再利用を検証する。
12. resultへ`rng_version`を公開し、`MCTS.reset_replay_sequence(seed, nonce)`のIdle-only
    契約をbinding testで固定する。
13. root-parallelのPython callback直列化mutex取得後にtimeout/cancelを再検査し、stale backlogが
    callbackへ入らないことをbinding testで固定する。

公開時の制約:

- Python inference callbackは単一coordinatorが同期実行し、同一search内で並行callbackしない。
- root-parallelでbudgetが正の場合は再現可能かつworker間で衝突しない明示`search_nonce`を必須とする。
- `timeout_ms`はcallback自体をpreemptしないsoft timeoutである。

### Gate

- callback同時実行数1。
- callback thread identityが契約どおり。
- retained ndarray lifetime安全。
- callback exception後にworker/VL/ticket 0。
- cooperative cancel後に`issued == completed + cancelled`、VL=0、active guard解放。
- root mutation isolation。
- 別MCTS searchは並行実行可能。
- 既存public exports/APIを不用意に変更しない。

## 15. PS-12: benchmark・virtual loss tuning・品質

> runner・結果解釈・継続判断・結果表担当: **Codex Sol Ultra**。
> 実績: zero-cost fake inference、world=1、固定hostの限定sliceを取得済み。以下の全matrixと品質gateは
> 完了していないため、PS-12全体をcompletedとは扱わない。

### 作業

1. 1/2/4/8/16 thread scalingを固定実機で測る。
2. determinization off/on、branch 5/250、cold/warm treeを分ける。
3. fake inference 0/50/250/1000µsと実NNを分ける。
4. batch 1/16/64、world 1を測る。
5. lock/queue/expansion/VL contention metricsをprofileする。
6. root-parallel/coarse/shardedを同条件で比較する。
7. VL weightを正しさ確立後にだけtuneする。
8. fixed-time root distribution/self-play qualityを比較する。
9. 1 thread回帰とmemory/RSSを確認する。

### Gate

- [benchmark受入条件](06_benchmark_and_rollout.md)を満たす。
- scaling改善がNN込みend-to-endへ反映される。
- NPSだけ上がり固定時間品質が落ちる設定を採用しない。

## 16. PS-13: rollout・最終監査

> 担当: 実装者と別sessionの **Codex Sol Ultra**。
> 現在地: Stage B experimental rollout。stable化gateは未完了。

### 作業

1. concurrency、hash、determinization、GIL、lifetimeを横断reviewする。
2. sanitizer full/nightly matrixを完走する。
3. feature flagと既定値1 threadを確認する。
4. canary telemetryとrollback手順を確認する。
5. API migrationと既知制限をdocumentする。
6. fixed host benchmark結果をREADMEへ反映するか判断する。
7. `num_threads>1`をexperimentalからstableへ上げる条件を記録する。
8. pending dedupだけでなく、展開済みnodeへ保存した二次feature signatureを後続到達でも照合する。

現時点のrollout判断:

- 既定は1 threadで、これは低overhead serial pathを使う。
- 複数thread shared-treeとroot-parallelは明示opt-inに限る。
- CIのscheduled soakは既存4 native testを25回ずつ反復する約100 test実行であり、100種類の
  scheduler seedを意味しない。全named hookの実装を完了とは主張せず、productionと同じ
  public APIによる決定的race fixtureを主oracleにする計画へ修正した。
- rollbackはlegacy APIまたは`num_threads=1`への設定変更で行う。
- 現時点のfeature digest検査は同一pendingのowner/waiter間に限定される。展開済みnodeの
  二次signature照合を実装・stress検証するまではstable gate未完了とする。

### Gate

- 全correctness/sanitizer/performance/quality gate成功。
- race suppressionがproject sourceを隠していない。
- unresolved blockerが明示されている。
- rollbackがconfigだけで可能。
- final reviewerがno-go項目0を確認する。

## 17. Part間handoff形式

トークンを節約しつつ誤解を防ぐため、各担当は次だけを次担当へ渡す。

```text
Part ID / objective
変更ファイル一覧
確定したcontract / ADR
追加testとその意図
実行コマンドと結果digest
未解決事項
sanitizer/benchmark artifact path
次partの開始条件
```

全会話履歴や全ソースを渡さず、この計画書、対象diff、failure trace、関連testだけを渡す。
contract変更の必要が判明した場合は、そのphase内で計画とtest oracleを先に更新してから
実装を再開する。
