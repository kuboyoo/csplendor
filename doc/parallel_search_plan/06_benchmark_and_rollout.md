# 06. Benchmark・探索品質・rollout計画

実施状況（2026-07-14）: Stage Bのexperimental Python opt-inまで実装・local検証済み。
固定実機native fake-inference結果は[benchmark_results.md](benchmark_results.md)に記録した。
Stage Cの実NN/fixed-time品質canary以降はrepo外の手動gateとして残している。

> Benchmark harness、結果解釈、方式継続、virtual loss tuning、文書更新担当:
> **Codex Sol Ultra**。計測実行と結果解釈は別の狭いcontextに分ける。
> 理由: runner実装は定型化できるが、並列効率と探索品質のtrade-off判断はNPSだけでは
> 決められないため。

## 1. 現在の基準値

2026-07-13の既存計測:

```text
CPU: Ryzen 9 7900X
compiler: GCC 13.3, Release
affinity: 1 logical core
native MCTS: batch 16, 256 simulations, 1 world, fake inference
root: seed 42を固定的に12手進めた合法手5件の局面
```

| Mode | 現行 | リファクタ前 | 改善 |
|---|---:|---:|---:|
| determinization off | 94,427 simulations/s | 70,690 | 1.34倍 |
| determinization on | 108,441 simulations/s | 65,001 | 1.67倍 |

この値はNN推論を含まず、低分岐固定局面である。parallel採用判断では次を分ける。

- native search-core scaling。
- simulated inference latency下のpipeline scaling。
- Python boundary込み。
- 実NN込みend-to-end。
- fixed-time探索品質。

## 2. Benchmark executable

追加候補:

```text
scripts/benchmark_mcts_parallel.cpp
scripts/run_parallel_benchmark.py
scripts/analyze_parallel_benchmark.py
```

既存 `scripts/benchmark_mcts_native.cpp` はsingle-thread historical baselineとして残す。
新benchmarkはCSV/JSON双方を出し、条件をmachine-readableにする。

### 2.1 出力metadata

```text
timestamp
source/worktree digest
compiler/version/flags
CPU model/topology/governor
affinity list
tree backend
parallel mode
threads/batch/max inflight
root fixture/hash/key version
determinization/worlds
seed/nonce range
inference model
simulation budget
warmup/sample index
```

### 2.2 出力metrics

```text
committed simulations/s
evaluated boards/s
search latency p50/p95/p99
speedup vs same-path 1 thread
parallel efficiency = speedup / threads
CPU utilization / context switches
batch fill ratio / queue depth
inference busy/idle time
node/shard lock wait time
expansion owner/waiter ratio
virtual-loss collision/max/residual
duplicate evaluations avoided
tree size / node count / peak RSS
issued/completed/cancelled/failed
root visit distribution digest
```

## 3. Workload matrix

### 3.1 Root fixture

| Fixture | 目的 |
|---|---|
| initial state | root action数とcold expansion |
| current 5-action midgame | READMEとの継続比較 |
| 250-legal-action state | raw move generation/decode負荷 |
| hidden reserve state | world-local availability |
| reveal-heavy state | public outcome分岐、tree shard分散 |
| near-terminal state | terminal completion競合 |
| warm deep tree | repeated selection/backprop contention |
| capacity-bound tree | node map/RSS/LRU behavior |

ActionEncoderのpolicy枠は48だが、raw legal actionが250件の局面ではdecode/canonical action生成の
負荷が異なるため、5件局面だけでparallel scalingを代表させない。

### 3.2 Thread/topology

```text
physical cores: 1, 2, 4, 8, 12（host上限まで）
SMT series: physical coreを埋めた後にsibling追加
oversubscribe: 16/32 software threads（stress専用、性能判断外）
```

`lscpu -e=CPU,CORE,SOCKET,NODE`等でtopologyを記録し、physical core seriesとSMT seriesを
混ぜない。OS用coreを避け、同じaffinity setでA/Bする。

### 3.3 Tree backend

- serial legacy。
- single-thread ticket path。
- root-parallel。
- coarse shared tree。
- sharded shared tree。
- deterministic epoch（性能参考のみ）。
- throughput。

### 3.4 Inference model

| Mode | 実装 | 測るもの |
|---|---|---|
| zero-cost | fixed C++ arrays | tree/lockの上限 |
| deterministic CPU work | 固定回数の算術 | CPU overlap、再現性 |
| fixed batch latency | 50/250/1000µs/batch | queue/batch overlap |
| per-board + batch overhead | 例: 5µs/board + 50µs/batch | batch fill効果 |
| Python NumPy callback | owning contiguous arrays | GIL/binding overhead |
| actual policy/value model | repo外modelを手動指定 | 実用end-to-end |

`sleep`だけのlatencyはOS schedulerの影響を受けるため、busy CPU workとtimer latencyの両方を
用意する。実NN結果はmodel hash、backend、precision、device、batch shapeを記録する。

### 3.5 Search parameters

```text
simulations: 256, 4,096, 65,536
batch: 1, 16, 64
max inflight: threads, 2*threads, 4*threads, 2*batch
determinization: off, on
worlds: 初期parallel版は1
noise: off（速度基準）、on（専用case）
forced playout: off（速度基準）、on（専用case）
```

## 4. 測定手順

1. Release + `-O3 -DNDEBUG`、同じcompiler/flagsでbuildする。
2. CPU frequency governor、temperature、background loadを記録する。
3. processをphysical coreへpinする。
4. fixtureごとにwarm-upする。
5. baseline/candidateを別processで交互に実行する。
6. 15標本以上、重要比較は30 pair測定する。
7. median、paired speed ratio、bootstrap 95% CIを出す。
8.外れ値を理由なく除外しない。
9. sanitizer buildを速度比較しない。
10. raw JSON/CSVとsummaryを保存する。

tree reuseの有無を混ぜない。

- cold search: searchごとにtree clear。
- warm search:同root/treeを再利用。
- root advance:実game move後にsubtree reuse。

## 5. Scaling指標

```text
speedup(T) = throughput(T) / throughput(1)
efficiency(T) = speedup(T) / T
```

暫定目標:

| Threads | Speedup目標 | Efficiency |
|---:|---:|---:|
| 1 | 新pathがlegacy比0.90以上 | n/a |
| 2 | 1.50以上 | 75%以上 |
| 4 | 2.40以上 | 60%以上 |
| 8 | 3.60以上 | 45%以上 |

これは固定実機のnative/fake-inference向け継続判断目標であり、CI hard gateではない。
実NNが1 threadで既に推論deviceを飽和させる場合、CPU thread scalingが低くてもfailureとは限らない。
その場合は同じGPU saturationをより少ないCPUまたは低いlatencyで達成できるかを見る。

### 5.1 方式継続判断

- coarse版は性能不合格でもcorrectness oracleとして残す。
- sharded版がcoarse版を上回らなければ採用しない。
- 4 threadが1.5倍未満の場合、lock wait、root contention、batch starvationをprofileする。
- 2回の改善cycle後も4 thread 1.5倍未満なら、shared-tree細粒度化を止め、root-parallelを
  実用fallbackとして比較する。
- lock-free化はnode/shard lockが明確な支配コストと示された場合だけ検討する。

## 6. Amdahl評価

現行決定化native coreは約9.2µs/simulationである。単純に1 simulationあたり1 boardを
推論し、overlapしない近似では、推論throughputが20,000 boards/s（約50µs/board）なら、
native coreを無限に高速化しても全体改善上限は約1.18倍である。

並列探索の価値は次のいずれかにある。

- inference待ちとCPU traversalをoverlapする。
- batchを満たしてGPU throughputを上げる。
- tiny/CPU modelでsearch core自体を高速化する。
- 複数game/self-playを同時に処理する。

したがって、native zero-cost scalingだけで採否を決めない。

## 7. Contention diagnosis

### 7.1 必須profile

- root node lock wait。
- non-root node lock wait。
- shard/table lock wait。
- queue wait。
- inference idle/busy。
- expansion join率。
- current-world mask生成時間。
- ActionEncoder decode時間。
- hash時間。
- backprop時間。
- context switch/cache miss（利用可能なprofile toolで）。

### 7.2 判断例

| 観測 | 次の対策 |
|---|---|
| root node lock支配 | select critical section短縮、root-only統計分離を検討 |
| shard lock支配 | shard数/reserve/hash分散を比較 |
| node lock以外のdecode/hash支配 | parallelism維持、別最適化を分離 |
| expansion waiter過多 | root bootstrap、max inflight、batch policy調整 |
| inference queue空 | worker数/CPU traversal改善 |
| inference queue常時満杯 | GPU/NNが支配。worker追加を止める |
| VL collision高くroot分布悪化 | VL weight/max inflightを品質込みでtune |
| shared_ptr/RSS支配 | active中erase禁止を利用したarena/unique handle検討 |

## 8. Virtual loss tuning

correctness・TSAN・baseline互換が確立するまでweightを変更しない。構造変更と探索parameter
変更を同時に評価しない。

### 8.1 候補軸

```text
VL visit weight: 0.1, 0.3(current), 0.5, 1.0
Q penalty: current式、fixed loss、visit-only
max inflight per worker: 1, 2, 4
forced playout accounting: N、N+weighted VL
```

### 8.2 評価値

- throughput。
- unique leaf率。
- same-edge collision率。
- root entropy/visit distribution。
- top action agreement。
- fixed-simulation Q/value variance。
- fixed-time self-play quality。

最速設定ではなく、固定時間品質が最良の設定を採る。

## 9. 探索品質検証

### 9.1 Deterministic fake inference

同じroot/seed/modelで次を比較する。

- 1-thread serial oracle。
- 1-thread ticket path。
- deterministic epoch 2/4/8 worker。
- throughput 2/4/8 workerを複数scheduler seed。

metric:

- exact root `N` digest（deterministic mode）。
- top-1 action agreement。
- root probabilityのJensen-Shannon divergence。
- Q RMSE。
- unique expanded nodes / inference count。
- schedule間variance。

deterministic epochは完全一致を要求する。throughput modeは完全一致ではなく分布と品質を評価する。

### 9.2 Actual NN / fixed-time

このrepoへmodelを置かず、外部指定で手動/専用runner実行する。

- same model hash/backend/precision。
- paired game seeds。
- player sideを入れ替える。
- 同じtime control。
- worker数以外を固定する。
- games/s、simulations/move、batch fill、GPU utilizationを記録する。
- 勝率差のconfidence intervalを出す。

十分なgame数はvarianceから決める。小標本の数勝だけで採用しない。可能ならsequential testまたは
事前に決めたgame数とCIで判定する。

### 9.3 品質stop条件

- deterministic mode不一致。
- fixed simulationでtop action/Qが説明不能に大きくずれる。
- fixed timeで統計的に明確な悪化。
- invalid replay、unavailable action、hidden-info aliasが1件でも出る。
- speedupがVL過集中だけで生じ、unique leaf/棋力が悪化する。

## 10. Memory計測

現行`MCTSNode`は832 byteで、50,000 nodeだけでも約41.6MBである。shared_ptr、mutex、map、
pending ticketを含む実RSSを測る。

`ParallelSearchOptions.max_tree_nodes`の既定値50,000は、shared-treeでは単一tree上限、
root-parallelでは全active worker treeのaggregate上限である。root-parallelは商・余りでactive
workerへ分配するため、既定値のままworker数倍のnode budgetにはならない。

記録:

- bytes/node。
- map bucket数/load factor。
- NodeRecord/shared_ptr overhead。
- pending ticket/path peak。
- active ticket registry peak（throughputは`O(max_inflight)`、deterministicは
  `O(deterministic_epoch_size)`）。
- queue buffer。
- worker scratch。
- determinization Game copies。
- root-parallelのworker scratch/Game copyと、aggregate node上限内でのworker別tree memory。

受入目安:

- shared treeのsteady-state RSSが説明なく2倍を超えない。
- max inflightに比例してboundedである。
- deterministic modeのticket/path peakがepoch sizeに比例し、全simulation budgetに比例しない。
- cancel後にpending memoryがbaselineへ戻る。
- root-parallelはaggregate memory上限をconfigで強制し、active workerへの割当合計が上限以下になる。

## 11. Rollout段階

### Stage A: internal native only

- native test/benchmarkからのみ利用。
- `num_threads=1`。
- coarse/shardedをruntime/test optionで切替可能。
- TSAN/replayを完成させる。

### Stage B: experimental Python opt-in

- `mcts_search_parallel_native()`をinternal/experimental公開。
- defaultは1 thread。
- same-MCTS concurrent callはfail-fast。
- metricsとresolved seedを返す。
- existing custom callback pathは変更しない。
- `max_tree_nodes`のshared/root aggregate意味論、zero-visit capacity partialのmasked prior/noise
  fallback、callback例外保持をknown contractとして公開する。

### Stage C: canary

- 明示`parallel_mode="shared_tree"`, `num_threads>1`だけで有効。
- fixed host/実NNで使用。
- errors、VL residual、cancel、lock wait、batch fillを監視。
- configだけで1 threadへ戻せる。

### Stage D: stable opt-in

- sanitizer nightlyを複数週継続。
- fixed-time品質gate成功。
- API/known limitations/documentation完成。
- 展開済みnodeの二次feature signature照合を実装し、pending dedup以外の後続到達でも検証する。
- still default 1 threadでもよい。

### Stage E: default変更の検討

次を満たした場合だけdefault `num_threads > 1`を検討する。

- target実行環境が明確。
- 実NN end-to-endで安定した利益。
- memory/CPU使用増が許容範囲。
- determinization semanticsと再現性が利用者へ説明済み。
- single-thread fallbackが常に利用可能。

## 12. Runtime feature flags

候補:

```text
parallel_mode = single_thread | root_parallel | shared_tree
tree_backend = coarse | sharded
num_threads = 1..N
schedule_mode = throughput | deterministic_epoch
inference_batch_size
max_inflight_simulations
max_tree_nodes（shared: 単一tree、root-parallel: active worker tree合計）
master_seed / replay_nonce
emit_search_metrics
```

通常利用者へcoarse/sharded等の内部debug optionを恒久公開する必要はない。experimental期間だけ
比較用に保持し、stable化後はdebug build/configへ縮小できる。

## 13. Telemetryとprivacy

記録してよいもの:

- counters、latency、lock wait、queue depth。
- public root key/digest。
- seed/nonce（debug/replay）。
- action ID、tree size。

通常logへ出さないもの:

- hidden card ID、full exact state。
- model input全量。
- 個人情報や外部token。

failure traceにhidden stateが必要な場合はlocal test artifactに限定し、公開CI artifactではfixture
ID/digestへ置き換える。

## 14. Rollback条件

即時1 threadへ戻す。

- sanitizer/race report。
- deadlock/hang。
- VL residual/ledger mismatch。
- hidden information leak/TreeKey alias。
- 展開済みnodeの二次signatureでfeature不一致を検出。
- callback exception後にMCTS再利用不能。
- fixed-time品質の明確な悪化。
- memory無制限増加。
- active mutatorが待機してGIL deadlockを作る。

rollbackはdata migrationを要さず、`num_threads=1`またはparallel API不使用だけで成立させる。

## 15. 採用判定レポート

PS-12終了時に最低限次を1つのreportへまとめる。

```text
correctness/sanitizer summary
fixed host configuration
1/2/4/8 core scaling table and CI
root-parallel/coarse/sharded comparison
inference latency matrix
actual NN end-to-end result
contention breakdown
memory/RSS
VL tuning result
fixed-time quality result
known limitations
go/no-go and rollback recommendation
```

最終go/no-go文は別review sessionの **Codex Sol Ultra** がraw resultを確認して作成する。
runner担当の要約だけを根拠に
しない。
