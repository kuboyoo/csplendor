# 複数コア並列 MCTS 探索 実装計画

作成日: 2026-07-13
対象: `csplendor` の C++ MCTS、Python binding、決定化、検証基盤
状態: **PS-0〜PS-11のcore実装と強化検証が完了。PS-12は限定計測済み、PS-13のstable化gateは継続中。複数threadはexperimental opt-in**

> 推奨統括担当: **Codex Sol Ultra**
> 理由: tree の線形化点、hidden-information の同値関係、GIL と C++ thread の
> shutdown を横断して判断する必要があるため。利用環境で実際に選べるモデルは
> Sol Ultraまでなので、全phaseでSol Ultraを使い、対象contextと作業単位を狭くして
> トークンを節約する。

## 1. 結論

採用する基本方式は、**1 search / 1 shared tree、複数 native worker、1 inference
coordinator** である。ただし、最初から細粒度 lock や lock-free に進まない。

実装は次の順序で段階化する。

1. 並列化前の hash・決定化意味論を修正する。
2. single-thread の処理を move-only ticket / reservation API へ移す。
3. shared tree 全体を coarse-grained mutex で保護し、TSAN と lifecycle oracle を通す。
4. map shard と node 単位 lock へ置換する。
5. native traversal 専用の parallel API を Python へ opt-in 公開する。
6. 固定実機で scaling と実 NN 込み性能を確認してから既定値変更を判断する。

初回実装では全面 lock-free 化を行わない。`Q`、`N`、virtual loss、node expansion、
prune、stale inference result を一貫して扱う必要があり、atomic 配列だけでは tree map と
node lifetime の安全性を解決できないためである。

現行CMakeの`csplendor_core`は`INTERFACE` targetであるため、parallel coreは当面header-onlyで
実装する。PS-5で`NodeRecord`をserial coreとして導入し、PS-7で同じ状態遷移をcoarse global
mutexによるshared coreへ昇格する。translation unit分割はbuild/profile上の必要が確認できた
時点で別変更とする。

## 2. 並列化前に解決する blocking correctness

計画策定時のコードには、race 以前に並列探索で増幅される意味論上の問題があった。

1. **hidden reserve の tier が observable hash に含まれていない**
   - 相手の伏せ予約カードについて、card ID は隠すべきだが tier は公開情報である。
   - 現状は tier が違って feature が違う局面でも同じ observable hash になり得る。
2. **追加 determinization world の異なる公開 leaf を world 0 node へ平均している**
   - deck reveal 後の visible card が違えば別の公開局面である。
   - 異なる leaf hash の policy/value/mask を同じ node へ平均・OR してはならない。
3. **情報集合内で action availability が world ごとに違い得る**
   - tree node の union mask だけで選択せず、simulation-local mask と交差させる必要がある。
   - decode 失敗を draw として backpropagate してはならない。
4. **tree key に observer / exact・observable domain が明示されていない**
   - 異なる observer または search mode の tree を誤共有しない構造化 key が必要である。

これらは [現状監査と設計判断](01_current_state_and_decisions.md) および
[RNG・決定化設計](03_rng_and_determinization.md) で詳細化する。

## 3. 目標構成

```text
Python caller / native caller
          |
          v
  ParallelSearchSession -------------------------------+
    | immutable root/config/seed context               |
    |                                                   |
    +--> worker 0 -- selection/replay/encode --+        |
    +--> worker 1 -- selection/replay/encode --+--> bounded inference queue
    +--> worker N -- selection/replay/encode --+        |
                                                        v
                                             inference coordinator
                                             (Python callback は1本)
                                                        |
                                                        v
                                             publish / backpropagate
                                                        |
                                                        v
  ConcurrentTree <--- sharded map + NodeRecord lock ----+
```

主要な ownership は次のとおりとする。

- root `Game`、config、observer、root key は search 開始時に snapshot し immutable にする。
- worker は `Game`、path builder、feature/mask scratch を完全所有する。
- RNG stream を共有せず、logical simulation ID から seed を導出する。
- Python callback を呼ぶ thread は inference coordinator だけに限定する。
- tree node は内部 `NodeRecord` とし、公開 `MCTSNode` は lock 下で作る snapshot にする。
- parallel node の visit counter は64-bitとし、旧32-bit DTOと分離する。
- 64-bit counterはavailability、visit、VL、reservationを更新する前に上限を一括検査し、
  overflow時に一部だけmutationしない。
- `clear()`、prune、config 変更、同一 `MCTS` への二重 search は active 中 fail-fast する。
- Python `mcts.config` は detached copyを返し、更新はcontrol lockを通る`set_config()`だけにする。
- 旧裸hash APIは`LegacyExact` tree専用とし、構造化`TreeKey`のparallel treeへ自動変換しない。

詳細は [並列アーキテクチャ](02_parallel_architecture.md) を参照する。

## 4. 再現性モード

安全性と bitwise 再現性を分ける。

| モード | 用途 | 契約 |
|---|---|---|
| `single_thread` | 互換 oracle | 既存 single-thread 結果と一致 |
| `deterministic_epoch` | CI、debug、trace replay | 単一coordinatorがtraversal・同期評価・commitをlogical ID順に実行。worker数は結果互換性入力で、非同期completion reorderは発生しない |
| `throughput` | 実運用 | race-free と seed 対応は保証。最終 tree は scheduler 依存を許容 |

非同期 shared-tree MCTS は、同じ乱数を使っても backpropagation の到着順で次の PUCT
選択が変わる。また浮動小数点の加算順も変わる。そのため、production throughput mode
に thread 数をまたいだ tree digest 完全一致を要求しない。

## 5. 計画書構成

| 文書 | 内容 | 主担当モデル |
|---|---|---|
| [01_current_state_and_decisions.md](01_current_state_and_decisions.md) | race inventory、意味論 blocker、ADR | Sol Ultra |
| [02_parallel_architecture.md](02_parallel_architecture.md) | shared tree、virtual loss、scheduler、GIL | Sol Ultra |
| [03_rng_and_determinization.md](03_rng_and_determinization.md) | seed、hash、world、再現性 | Sol Ultra |
| [04_implementation_phases.md](04_implementation_phases.md) | PS-0〜PS-13 の実装順・完了条件 | Sol Ultra |
| [05_validation_and_tsan.md](05_validation_and_tsan.md) | TSAN、stress、replay、CI | Sol Ultra |
| [06_benchmark_and_rollout.md](06_benchmark_and_rollout.md) | scaling、品質、feature flag、rollback | Sol Ultra |
| [07_codex_model_assignment.md](07_codex_model_assignment.md) | パート別context分割と引継ぎ規則 | Sol Ultra |
| [baseline_results.md](baseline_results.md) | PS-0 single-thread性能・correctness baseline | 計測記録 |
| [benchmark_results.md](benchmark_results.md) | PS-12 固定実機parallel benchmarkと採用判断 | 計測記録 |
| [implementation_status.md](implementation_status.md) | 実装進捗・環境差・検証結果 | 継続更新 |

## 6. 実装フェーズ概要

| ID | 目的 | 必須ゲート | 推奨担当 |
|---|---|---|---|
| PS-0 | baseline、race probe、digest 固定 | 現行 single-thread oracle 保存 | Sol Ultra |
| PS-1 | information-set hash と multi-world 修正 | hash/mask/world corpus 成功 | Sol Ultra |
| PS-2 | CMake/CTest/sanitizer 基盤 | native test を通常/TSAN/ASan でbuild | Sol Ultra |
| PS-3 | seed context と portable search RNG | thread 数非依存 seed manifest | Sol Ultra |
| PS-4 | lifecycle、config/root snapshot、TreeKey | active 中の危険操作を fail-fast | Sol Ultra |
| PS-5 | serial `NodeRecord` + ticket / reservation | 既存 digest、VL=0、例外 cleanup | Sol Ultra |
| PS-6 | 独立 tree root-parallel oracle | merge と memory 上限を検証 | Sol Ultra |
| PS-7 | serial coreをglobal mutex shared treeへ昇格 | TSAN 0 report、ledger 一致 | Sol Ultra |
| PS-8 | worker / batch / inference coordinator | cooperative cancel/timeout/exception 後に完全 drain | Sol Ultra |
| PS-9 | deterministic epoch / trace replay | canonical traceとpath statistics replayがworker数/backendで一致 | Sol Ultra |
| PS-10 | sharded map + node lock | coarse 版と同じ oracle、性能改善 | Sol Ultra |
| PS-11 | Python parallel binding | GIL・callback・ownership test | Sol Ultra |
| PS-12 | scaling、VL tuning、実 NN canary | 固定実機の採用基準達成 | Sol Ultra |
| PS-13 | rollout、最終横断 review | sanitizer/性能/品質全ゲート | 独立Sol Ultra session |

各フェーズの変更を混ぜない。特に PS-1 の探索意味論変更と PS-10 の lock 細粒度化を
同じ変更単位にすると、回帰原因を分離できなくなる。

## 7. 受入条件

### Correctness / safety

- native TSAN が 0 report。
- ASan/UBSan が 0 report。
- search の全終了経路で virtual loss が 0。
- `virtual_loss_added == virtual_loss_released`。
- request/ticket は最大1回だけ commit または cancel される。
- root bootstrapはsimulation budget外で、要求されたlogical simulationを減らさない。
- Pending publishはnode単位で最大1回、simulation commitはattached ticket単位で各1回行う。
- stale result、duplicate result は tree を変更しない。
- `node.total_visits == sum(node.N)` が quiescent point で成立する。
- active ticketの保持量はthroughput modeで`O(max_inflight)`、deterministic modeで
  `O(deterministic_epoch_size)`に制限する。
- tree capacity到達時は未commit ticketをabortしVL=0にする。root展開済みなら
  partial resultとし、visitが0でもlegal mask上で正規化したroot prior（設定時はroot noiseを
  混合）を返す。root未展開なら使用不能な全zero結果を返さず明示的な
  `TreeCapacityReachedError`とする。
- `max_tree_nodes`の既定値は50,000。shared-treeでは単一treeの上限、root-parallelでは
  全active worker treeの合計上限として均等分配し、worker数倍へ暗黙に増やさない。
- root-parallelのmerged stop reasonは
  `Completed < Cancelled < TimedOut < TreeCapacityReached < CallbackError < WorkerError`の優先順とする。
- callbackが送出した`TreeCapacityReachedError`は内部tree capacityと区別して元の例外を再送出する。
  traversal workerの例外はnoexcept境界で保存してqueueをcloseし、coordinatorを確実にwakeする。
- C++/Python共通のcooperative cancellation tokenはpre-cancelとin-flight cancelを扱い、
  `issued == completed + cancelled`、VL=0、後続search再利用を保つ。
- `timeout_ms`はroot-parallelでfactory生成・共通bootstrapを含むend-to-end予算とする。
  実行中callbackは強制中断しないsoft/cooperative timeoutである。
- root `Game`、history、hash、mode は search 前後で不変。
- callback exception、cancel、MAX_DEPTH、malformed result 後も同じ `MCTS` を再利用できる。
- hidden information を feature、mask、tree key から漏らさない。
- 同一pendingへdeduplicateされるowner/waiter間ではfeature digest一致を検査し、world-local mask差は
  正常入力として扱う。展開済みnodeへ後から到達したfeatureの二次signature検証は未実装であり、
  stable化前のhardening gateとする。

### Compatibility

- `num_threads=1` が既定値。
- 新 parallel path の1 thread結果が serial oracle と一致する。
- 既存 Python API は、明示した deprecated API を除いて互換を保つ。
- custom Python featurizer/encoder 経路は、native parallel 契約が固まるまで single-thread
  のまま維持する。

### Replay / observability

- `seed=None`はstateful shared-tree経路で`MCTS`構築時に一度だけentropyを解決し、
  result/traceに`resolved_seed`、`search_nonce`、`rng_version`を返す。
- 同じ探索identity列をreplayする場合は、quiescent時専用の
  `reset_replay_sequence(seed, nonce)`を使う。
- trace schema v3は各eventに変更nodeだけを保存し、v2のfull-tree/eventによる
  O(n²)増大を廃止した。aggregate node snapshotは131,072、保守的なsafe event数は
  現行`MAX_DEPTH`で最大約218であり、予算超過はtree mutation前に拒否する。
- strict replayはpath statisticsとchained publication delta/tree digestを検証するoracleであり、
  pending/reservationの全過渡を再実行する完全state-machine replayではない。
- 10,000 tupleのseed manifestは1/2/4/8/16 worker割当で不変かつ衝突0、canonical traceは
  coarse/sharded backendとdeterministic modeの指定worker数でbyte一致を検証する。

### Performance

- 1 thread の新経路は既存 native MCTS 比で中央値 10% 以内の低下。
- 暫定目標として固定実機で 2 thread 1.5倍、4 thread 2.4倍、8 thread 3.6倍以上。
- shared-tree sharding は global mutex 版より実測改善した場合だけ採用する。
- 実 NN 込みで simulations/s だけでなく、GPU utilization、batch fill、games/s、
  固定時間の探索品質が改善する。

性能閾値は GitHub hosted runner では hard gate にしない。固定 CPU、core pinning、同一
compiler/build を使う専用計測で判定する。

固定実機のzero-cost fake inferenceでは、sharded shared-treeが2/4/8 threadで、
determinization offは1.82/3.30/3.99倍、onは1.78/2.84/3.73倍（各同経路1 thread比）となり、
上記の暫定目標を満たした。詳細と測定範囲は
[PS-12 benchmark結果](benchmark_results.md)を参照する。実NNを使った固定時間品質canaryは
modelを持たないこのrepositoryの外で実施する手動gateとして残すため、既定値は1 threadのまま、
`num_threads > 1`はexperimental opt-inとする。

## 8. 即時停止条件

次のいずれかが発生したフェーズは先へ進めない。

- sanitizer report、hang、deadlock が1件でも発生する。
- virtual loss 残留、ledger 不一致、二重 commit がある。
- observable hash が異なる公開状態を alias する。
- 異なる公開 leaf を同じ node へ平均する。
- inference callback 中に tree/node/queue lock を保持している。
- active search 中の `clear()` 等が待機して GIL deadlock を作り得る。
- 1 thread 回帰が10%を超え、profileで説明できない。
- 2回の改善サイクル後も4 threadが1.5倍未満なら、細粒度 shared tree を一旦止め、
  root-parallel方式を再比較する。

## 9. モデル割当の要約

この利用環境では全partを実際に選択できる **Sol Ultra** へ割り当てる。トークン節約は
存在しない下位tier名に依存せず、phase単位の狭いcontext、対象file制限、成功logのdigest化、
実装sessionとreview sessionの分離で行う。

詳細な委譲境界と停止ルールは
[Codexモデル割当](07_codex_model_assignment.md) に記載する。
