# Parallel search implementation status

更新日: 2026-08-05

状態: **PS-0〜PS-11のcore実装済み。PS-7〜PS-10は強化検証済み、PS-12〜PS-13の
nightly・実NN・品質canary・広範な計測matrixのstable化gateは継続中**

この文書は、計画に対する実装結果、予想外の環境差、未完了gateを追跡する。

## R5 ownership refactor: completed without stable promotion

- throughput search固有のworker、work/event queue、pending registry、active ticket、failure slot、
  ledgerを`ParallelSessionController`の単一所有へ移した。destructor fallbackは両queueをcloseし、
  全workerをjoinする。
- parallel execution固有のconfig検証を`MCTSConfigValidator`へ分離し、`MCTSConfig`は従来どおり
  passiveな公開value typeとして維持した。
- deterministic/throughput/root-parallelのscheduler semantics、trace version/bytes、tree表現、
  legacy batch API、Python surfaceは変更していない。
- owner/session/config境界の整理は完了したが、下記PS-13の外部品質gateは未完了である。
  この変更をstable昇格の根拠にはせず、複数threadは引き続きexperimental opt-inとする。

## PS-0: completed

- hidden tier hash alias、divergent public leaf、world-local availability、decode-as-drawを
  deterministic fixtureとして固定した。
- 既知hash集合をsortしたtree snapshot SHA-256を固定した。
- Dirichletは現行`random_device`経路のためbitwise goldenではなく、shape、finite、非負、
  合計1、illegal slot 0を固定した。明示seed goldenはPS-3 native testへ移した。
- single-thread benchmarkのraw 15 sampleを
  [baseline_results.md](baseline_results.md)へ保存した。

## PS-1: completed

- 相手hidden reserveのslot/tier saltを独立Zobrist streamで追加した。
- 同tier hidden IDはaliasし、tier signatureが違う公開情報は別hashになる。
- `TreeKey{hash, version, observer, domain, mode}`とfield-wise hasherを追加した。
- hidden-informationの`num_determinizations > 1`をtree mutation前に拒否した。
- legacy `MCTSNode`のlayoutを変えず、未mask base policy、availability union/countをsidecarへ
  分離した。
- native batch traversalは各worldのmaskで選択し、後発worldで初めてavailableになったactionも
  base priorを失わず選択できる。
- decode/replay invariant failureはdraw visitにせず、VLをrollbackしてerrorにする。

## PS-2: completed

- Python moduleとnative-only buildをCMake optionで分離した。
- `csplendor_core` / sanitizer interface target、CTest unit/stress labelを追加した。
- CMake minimumを3.13へ更新し、compile/link両方へsanitizer flagを伝播した。
- normal、Clang TSAN、Clang ASan+UBSanのnative testを通過した。

環境差:

- Ninjaは未導入なのでgeneratorを指定せずMakefilesを使用する。
- GCC TSANはこの環境で`unexpected memory mapping`となるためClang TSANをprimaryにする。
- LeakSanitizerはsandbox/ptrace制約で起動できないため、local ASanは`detect_leaks=0`、CIは
  `detect_leaks=1`とする。
- 通常の`pip install -e .`はbuild isolationがnetwork取得を要求し、user siteもread-onlyで
  installできない。local extension検証は`python setup.py build_ext --inplace`を使用する。
- sandboxがStarlette `TestClient`のsocket wakeupを`EPERM`で拒否するため、`tests/test_api.py`は
  calling-threadの`httpx.ASGITransport`を使う。この変更はWeb API test harnessだけの回避で、
  MCTS実装・callback・benchmark経路には入らず、並列探索の性能とは無関係である。

## PS-3: completed

- repository-owned SplitMix64 stream、rejection sampling、Fisher–Yatesを追加した。
- master seed、TreeKey、nonce、domain、simulation/world/sub IDからseedを導出する
  `SearchRandomContext`を追加した。
- `seed=None`はstateful経路で`MCTS`構築時に一度だけentropyを解決し、worker/searchごとに
  `random_device`を呼ばない。resultへ`resolved_seed`、`search_nonce`、`rng_version`を出す。
- quiescent時専用の`reset_replay_sequence(seed, nonce)`をC++/Pythonに追加し、tree/generationを
  消去せずreplay identityだけを明示resetできるようにした。
- legacy `Game(seed)` / `randomize_hidden_information()`は変更せず、parallel search専用portable
  determinizationを追加した。
- RNG sequence、bounded sample、shuffle、domain-separated seed、portable worldのnative goldenを
  normal/TSAN/ASan+UBSanで検証した。
- 10,000 logical tupleのseed manifestは衝突0で固定digestと一致し、1/2/4/8/16 workerへの
  割当を変えても不変であることを検証した。
- determinization on/offのExact/Observable storage domain差で`RootDirichlet` noise arrayが変わらない
  回帰testを追加した。

## PS-4: completed

- `MCTS::SearchGuard`、tree generation、search nonceを導入し、同一`MCTS`の二重search、active中の
  `clear()`、prune、config/manual mutationを待機せず拒否するようにした。
- parallel treeをlegacy treeと物理的に分離し、config/node/tree-size APIをdetached snapshot化した。
- batch requestへtree generationを刻印し、stale requestをtree mutation前に拒否するようにした。
- Python bindingではroot、options、config/generationをGIL保持中にsnapshotし、search guard取得後に
  GILを解放する。これによりAPI entryと別Python threadのconfig変更のlinearizationを固定した。

## PS-5: completed

- parallel専用`NodeRecord`、64-bit `N`/total visits/availability、stable shared handleを導入した。
- move-only `ReservedPath`とreservation tokenでselect+virtual-loss addを一操作にし、commit、abort、
  destructor fallbackの全経路でexactly-once cleanupするようにした。
- Pending evaluationのpublish/failとsimulation ticketのcommit/abortを別状態機械にし、1回の評価を
  owner/waiterで共有しても、attached ticketを各1回commitするようにした。
- 同一pendingへdeduplicateするowner/waiter間はfeature digest一致を必須とする一方、world-local
  mask差を許容し、選択時にlocal maskとavailability unionを交差するようにした。展開済みnodeへ
  後から到達したfeatureは現時点で再照合しないため、全`TreeKey` lifetimeの一致保証ではない。
- 64-bit値をlegacy 32-bit DTOへ変換できない場合はwrapせず明示errorにした。
- availability、N、total visits、VL、live reservationの64-bit上限をmutation前にpreflightし、
  overflow時にunion/counterを部分更新しない回帰testを追加した。

## PS-6: completed

- workerごとに独立treeとsimulation ID範囲を持つroot-parallel oracle/fallbackを追加した。
- budgetを固定分割し、root `N`を64-bit加算、`Q`をvisit-weighted mergeする。
- root bootstrap/noiseは共有し、worker failure時も全threadをjoinしてから再送出する。
- `simulation budget < worker count`ではbudget 0のworkerを起動しない。0がoptions既定値へ解釈され
  余分なsimulationを発行していたreview検出バグを修正し、回帰testを追加した。
- `max_tree_nodes`は既定50,000のaggregate上限としてactive workerへ均等分配する。shared-treeの
  同optionは単一tree上限であり、root-parallelだけworker数倍へ暗黙に増やさない。
- worker結果のstop reasonは
  `Completed < Cancelled < TimedOut < TreeCapacityReached < CallbackError < WorkerError`で集約し、
  sibling failure時は内部session tokenで他workerをcooperative stopして全join後に再送出する。

## PS-7〜PS-8: core and strengthened local validation completed

- coarse global mutex shared-tree backendをcorrectness oracleとして実装した。
- bounded closeable MPMC queue、native traversal worker、単一inference coordinator、owner/waiter dedup、
  max-inflight/backpressureを持つthroughput schedulerを追加した。
- active ticket保持量はthroughputで`O(max_inflight)`、deterministic epochで
  `O(deterministic_epoch_size)`とし、全simulation budgetをregistryへ保持しない。
- timeout、capacity、callback/worker exceptionを共通shutdownへ集約し、queue drain、thread join、
  Pending/ticket終了、VL回収後にpartial resultまたは例外を返す。
- callback boundaryで元例外を保持し、callbackが投げた`TreeCapacityReachedError`を内部tree capacity
  partialへ誤分類せず元のtype/messageで再送出する。traversal workerのnoexcept境界は例外を共有
  failure slotへ保存し、work/event queue closeでcoordinatorをwakeする。
- copy間で状態を共有する`ParallelCancellationToken`をC++/Pythonに実装した。pre-cancelは
  tree/callbackに触れず、in-flight cancelは`issued == completed + cancelled`、VL=0、active guard
  解放後にbalanced partial resultを返し、同じMCTSを再利用できる。
- tree capacityはroot展開済みなら安全なpartial resultを返し、visit 0でもlegal mask上で正規化した
  base prior（設定時はroot noiseを混合）を返す。root未展開で有効なdistributionを作れない場合は
  `TreeCapacityReachedError`を送出する。
- root-parallelの`timeout_ms`はAPI entryからのend-to-end予算とし、evaluator factory生成と
  共通bootstrapの消費時間を含めた。workerには残余時間だけを渡す。
- coarse/sharded両backendで8 thread × 12,500回（計100,000 reservation/commit）のstressと、
  同一leaf dedupを検証した。
- `num_threads=1`はqueue/thread poolを使わないcaller-thread serial path、`num_threads>=2`は
  traversal worker + 単一inference coordinatorのpipelineである。
- `timeout_ms`はqueue待機とcallback境界で観測するsoft timeoutである。既に実行中のPython/C++
  callbackを強制中断できないため、無期限blockするevaluatorには独立した外部watchdogが必要になる。
- same-leaf owner/waiter、pending terminal/duplicate/stale、cancel/capacity境界をproductionと同じ
  public APIで決定的に作るrace fixtureを追加した。当初のnamed fault hookを全数実装した
  とは扱わず、未被覆のinterleavingが判明した場合にだけ最小hookを追加する計画へ
  修正した。nightlyのseed sweepと長時間soakは引き続き未完了である。

## PS-9: core and canonical trace validation completed

- logical simulation ID順に単一coordinatorがtraversal/reservation、同期callback、commitを行う
  deterministic epochを実装した。`num_threads`は1/2/4/8入力の結果互換性を検証するため受理するが、
  このmodeではleaf evaluationを並列実行しない。
- binary traceをschema v3へ更新し、v2のfull-tree/event snapshotによるO(n²)増大を廃止して
  変更node deltaだけを保存するようにした。各eventのcanonical full-tree digestとheaderからの
  hash chainは維持している。
- aggregate node snapshotは131,072に制限し、現行`MAX_DEPTH`で保守的に安全な約218 eventを
  超えるtrace予算はtree mutation前に拒否する。parserはevent/path/snapshot capと、tree/
  expansion/completion/leaf-roleのenum値も検証する。
- `TreeKey`全fieldのcanonical sort、coarse/sharded、1/2/4/8指定のtrace byte一致、round-trip、
  改ざん/schema/truncation/過大event拒否を検証した。
- strict replayはinitial treeへpathのonline-mean `N/Q/total_visits`を独立適用し、chained
  publication delta/tree digestを各commit後に照合する。pending/reservation/availabilityの全過渡を
  再実行する完全state-machine replayではない。
- deterministic modeはserial coordinatorであり、parallel completion reorderの網羅検証ではない。

## PS-10: core and strengthened local validation completed

- map shard lockとnode lockを分離したsharded backendを、coarse backendと同じ状態遷移/APIで追加した。
- native fake-inference benchmarkでは2/4/8 threadすべてでcoarseを上回った。determinization offの
  sharded scalingは1.82/3.30/3.99倍である。詳細は
  [benchmark_results.md](benchmark_results.md)を参照する。
- local TSAN/ASan/UBSan、coarse/sharded stress、canonical backend trace、public API race fixtureは通過した。
  長時間nightly、異なるtoolchain、実NNでのcontention/quality gateはstable化前に残る。

## PS-11: experimental binding completed

- `ParallelSearchOptions`、ledger/result、experimental
  `mcts_search_parallel_native()`をPythonへ公開した。
- native traversal中はGILを解放し、Python inference callbackだけをcoordinatorが直列に呼ぶ。
  callback入力はretention可能なowning contiguous ndarrayとした。
- callback例外/malformed result後の再利用、heartbeat、callback同時実行数1、root mutation isolation、
  同一`MCTS`のfail-fast、別`MCTS`の同時searchをPython testで検証した。
- root-parallelは独立tree間のseed rangeを衝突させないため、budgetが正の場合に明示的な
  `search_nonce`を必須とする。共有tree APIは未指定nonceを`MCTS`所有の単調nonceへ解決できる。
- `ParallelCancellationToken`をPythonへ公開し、callback内からのin-flight cancel後もledger/VLが
  balancedで、同じMCTSを再利用できることを検証した。
- resultへ`rng_version`を公開し、Pythonから`MCTS.reset_replay_sequence(seed, nonce)`を
  quiescent時に呼び出せる。
- Python callbackはcoordinatorが同期的に呼ぶ。GIL解放により別Python threadは進行できるが、
  同一search内で複数callbackを並行実行する契約ではない。
- root-parallelのPython callback直列化はmutex取得後にtimeout/cancelを再検査する。遅いcallbackの
  後ろに待機したworkerをstale backlogとして順次callbackへ流さず、最大1件のoverrun後にdrainする。

## PS-12: limited fixed-host slice completed

- legacy/coarse/sharded/root-parallelを同一fixtureで比較するCSV benchmarkを追加した。
- Ryzen 9 7900X固定実機のzero-cost fake inferenceで、shared path 1 threadはlegacy比
  0.904〜1.012となり、10%以内の回帰条件を満たした。
- shardedはdeterminization offで1.818/3.304/3.989倍、onで1.778/2.844/3.731倍となり、
  2/4/8 threadの暫定目標を両条件ですべて満たした。
- 250µs callback latencyの補助sliceは2 threadで1.907倍、4 threadで1.908倍、8 threadで
  1.804倍と2 thread以降ほぼ飽和し、単一inference coordinatorの上限を確認した。
- 実NN、GPU utilization、fixed-time探索品質/self-playはmodelを含めないrepoの外で行う手動canary
  gateとして残す。このgateを通すまで複数threadをstable/defaultへ昇格しない。
- 現時点の数値はzero-cost fake inference、world=1、限られた固定host条件のsliceであり、
  branch 5/250、latency/batch全matrix、RSS、実NNを完了した総合判定ではない。

## PS-13: Stage B experimental rollout / stable gates pending

- concurrency、hash、determinization、GIL、lifetimeの横断reviewを行い、上記のGIL下snapshotと
  root-parallel少数budget問題を修正した。
- native、Clang TSAN、Clang ASan+UBSan、Python integrationのCI jobを追加した。
- scheduled CIは既存4 native testを25回ずつ反復する約100 test実行のinterleaving soakを追加した。
  test binaryはseed引数を持たないため、これは100種類のscheduler seed sweepではない。
- featureはexperimental opt-in、既定値は1 threadとした。問題時はlegacy API、または
  `num_threads=1`へ戻すだけでparallel rolloutを停止できる。
- Stage Bでは複数threadをexperimental opt-inのままにする。stable化にはremote CI/nightlyの継続成功、
  100 scheduler seed相当の可変schedule、未被覆interleavingの監査、計測matrix、実NN/fixed-time
  品質canary、timeout/watchdog運用の確認が残る。現時点で「stable gate完了」とは扱わない。

## 実装中に確定・修正した計画

1. root bootstrapは要求simulation budgetへ含めず、準備evaluationとして別計上する。
2. Pendingのnode publishと、attached simulation ticketのcommitを別線形化点にする。
3. 同一pendingへdeduplicateされるfeature差はinvariant違反だが、world-local mask差は正常入力として
   許容する。展開後の後続到達は二次signature gateへ分離する。
4. Python所有入力とMCTS config/generationはGIL保持中にsnapshot/guard取得してからGILを解放する。
5. root-parallelはbudget 0のworkerをskipし、`budget < workers`でも要求数を厳密に守る。
6. deterministic modeはPython callbackの直列契約とreplay oracleを優先し、単一coordinatorの同期実行へ
   修正した。`num_threads`は互換性入力であり、parallel reorderを発生させる設定ではない。
7. callback timeoutは強制cancelではなくsoft deadlineとし、evaluatorの無期限blockは外部watchdogの
   責務として明記した。
8. C++/Python共通のcooperative cancellation tokenを導入し、pre-cancel/in-flight cancelともに
   pending/ticket/VLをdrainしたpartial resultを返す。
9. root-parallel timeoutはfactory/bootstrapも含むend-to-end soft予算に修正した。
10. tree capacityはroot展開済みのみpartial result、root未展開は明示errorとした。
11. trace v3はdelta保存へ移行し、resource capとparser検証を追加した。strict replayは
    path statistics+chained publication deltaのoracleとし、完全state-machine replayとは呼ばない。
12. 汎用named hookの全実装ではなく、public APIの決定的race fixtureを主とする計画へ
    修正した。
13. feature digest一致の実行時検査は同一pendingのowner/waiter間に限定されることを明記した。
    展開済みnodeへ二次signatureを保存し後続到達でも照合する実装・stress testはstable hardening
    gateとして残す。
14. active ticket registryをthroughputは`max_inflight`、deterministicはepoch sizeでboundedにした。
15. `max_tree_nodes`をshared単一tree/root全active worker aggregateとして定義し、rootでは均等分配する。
16. zero-visit capacity partialのmasked prior/noise fallback、callback capacity例外保持、noexcept worker
    queue-close、64-bit counter preflight、root stop reason優先順位をreview回帰testで固定した。

## Verification snapshot

```text
native normal CTest:       4/4 passed
native Clang TSAN:         4/4 passed, 0 reports
native Clang ASan+UBSan:   4/4 passed
parallel Python pytest:    17/17 passed
full Python pytest:        352/352 passed (4 deselected)
```

local ASan+UBSanはsandboxのptrace制約によりLeakSanitizerを起動できないため、
`ASAN_OPTIONS=detect_leaks=0`で実行した。CIでは`detect_leaks=1`を指定する。上記はlocal実行結果であり、
remote CIの実行成功を意味しない。

通常native testは`mcts_parallel_unit`、`mcts_parallel_stress`、`mcts_parallel_scheduler`、
`mcts_parallel_replay`である。ここに記録した結果はlocal snapshotであり、scheduled workflowの成功、
100種類のscheduler seed、長時間nightly、実NN/品質canaryは別gateである。
全named fault hook実装はgateとせず、public API fixtureで未被覆の競合に限って最小hookを追加する。
