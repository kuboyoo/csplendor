# 02. 並列探索アーキテクチャ

> 推奨設計・実装担当: **Codex Sol Ultra**
> queue・metrics等も実際に選択できる同じモデルを使い、phaseごとに対象fileとcontextを
> 狭くしてトークンを節約する。
> 必須review: 実装者と別sessionの **Codex Sol Ultra** が、node lifetime、lock順序、GIL、
> exactly-once cleanupを横断確認する。

## 1. 要求

並列探索は次を同時に満たす必要がある。

- 1つのtreeを複数CPU coreで安全に共有する。
- virtual lossによりin-flight edgeを探索選択へ反映する。
- 同じleafのNN評価を重複させず、policy/valueを一度だけpublishする。
- workerごとに決定化worldと`Game`を所有し、rootを変更しない。
- Python inference callbackを同時呼出ししない。
- exception、cancel、timeout、malformed resultでもworkerとVLを完全にdrainする。
- active search中のclear/prune/config変更をdeadlockせず拒否する。
- 1 threadで既存探索結果を比較できる。
- debug/CI用のdeterministic modeと、実運用のthroughput modeを分ける。

## 2. 方式比較

| 方式 | Correctness | 探索効率 | Memory | Scaling | 判断 |
|---|---|---|---|---|---|
| worker別独立tree | 容易 | 重複探索が多い | thread数倍 | 良い場合あり | oracle/fallbackとして実装 |
| global tree mutex | 確立しやすい | shared | 1 tree | root競合で頭打ち | 最初のshared版 |
| sharded map + node lock | 中〜高難度 | shared | 1 tree | 最終候補 | coarse版合格後に採用 |
| edge atomic中心 | 高難度 | shared | node肥大 | 未知 | 初回不採用 |
| 全面lock-free | 非常に高難度 | shared | reclamation必要 | 理論上高い | profile後の別ADR |

global mutex版を削除せず、correctness oracleとfault-injection用backendとして残す。
sharded版のtree digestとledgerは同じ論理scheduleを与えたcoarse版と一致させる。

## 3. コンポーネント

### 3.0 build/translation-unit方針

現行CMakeの`csplendor_core`は`INTERFACE` libraryで、engine本体もheader中心である。PS-5〜PS-8の
parallel coreは当面header-only（`src/mcts_parallel_*.h`等）で実装し、native test/pybind targetの
双方へ同じ定義を伝播する。`.cpp`分割やcompiled core library化は、compile time、binary size、
profileのいずれかで必要性が確認できた場合に別変更として行う。

### 3.1 `TreeKey`

```cpp
enum class TreeDomain : uint8_t {
  Exact = 0,
  Observable = 1,
  LegacyExact = 2,
};

struct TreeKey {
  uint64_t position_hash = 0;
  uint32_t key_version = 0;
  uint8_t observer = 0;
  TreeDomain domain = TreeDomain::Exact;
  uint8_t mode_bits = 0;
};
```

- exact/observable、observer、Game modeを明示的に分離する。
- equality/hashはrepository内で固定する。
- determinization seedやworker IDは含めない。
- key version変更時はtree generationを更新し、旧requestをstaleにする。
- snapshot/traceのcanonical順は`position_hash, key_version, observer, domain, mode_bits`の
  field-wiseソートに固定し、`unordered_map`のiteration順に依存しない。
- 裸`uint64_t`を受ける既存APIは`LegacyExact`専用facadeへ閉じ込める。observer/domain/modeを
  推測して新treeへ変換せず、parallel treeには`TreeKey` APIだけでアクセスする。移行期間は
  legacy `uint64_t -> MCTSNode` mapとparallel `TreeKey -> NodeRecord` mapを物理的に分離する。

### 3.2 `NodeRecord`

公開`MCTSNode`へmutexを追加せず、内部wrapperを導入する。

```cpp
enum class ExpansionState : uint8_t {
  Unexpanded,
  Evaluating,
  Expanded,
  Terminal,
};

struct NodeRecord {
  mutable std::mutex mutex;
  NodeStats64 stats;                   // N/total_visits/availabilityはuint64_t
  std::array<float, MAX_ACTIONS> base_policy;
  std::array<uint8_t, MAX_ACTIONS> information_set_union;
  ExpansionState state;
  uint64_t generation;
  std::weak_ptr<PendingEvaluation> pending;
  std::atomic<uint64_t> last_access;
  std::array<uint64_t, MAX_ACTIONS> availability_count;
};
```

PS-5で同じ`NodeRecord`をsingle-thread serial coreとして先に導入する。PS-7では型と
状態遷移を変えず、`unordered_map<TreeKey, shared_ptr<NodeRecord>>`をcoarse global mutexで
共有可能に昇格する。

- mapからeraseされてもin-flight `NodeHandle` がlifetimeを保つ。
- `NodeRecord -> PendingEvaluation` はweak、ticket側からnodeはstrongとしcycleを防ぐ。
- active search中pruneを禁止するため、将来arena/`unique_ptr`へ変える余地を残す。
- parallel内部では公開`MCTSNode`をstats storageに使わない。`N`、`total_visits`は64-bitにし、
  Q/valueはdouble、base policyは48 action未mask値として保持する。
- `MCTSNode`の832 byte公開layoutはlegacy tree DTOとして当面維持する。parallel用には
  `MCTSNodeSnapshot64`を新設する。明示的なlegacy DTO変換で`UINT32_MAX`を超えた場合は
  clamp/wrapせずoverflow errorにする。
- Python `get_node_snapshot(TreeKey)` はnode lock下で64-bit値copyを返す。

### 3.3 `ConcurrentTree`

```text
ConcurrentTree
  tree_generation
  active_search state
  access_epoch
  backend = coarse | sharded
  coarse: one mutex + map
  sharded: 64 x {shared_mutex, map}
```

最終shard数はbenchmarkで決める。初期値64は設計上の仮値であり、hard-codeする前に
8/16/32/64を比較する。

lookup/insertの原則:

1. shard/table lockを取る。
2. `shared_ptr<NodeRecord>` を取得または作成する。
3. shard/table lockを解放する。
4. node lockを取ってstatisticsを操作する。

node lockを保持したままmapへ戻らない。

### 3.4 `SearchRootSnapshot`

search開始時、caller threadで次を一度だけ確定する。

```text
root Game clone
frozen MCTSConfig
observer
root TreeKey
root feature/mask bootstrap data
tree generation
resolved random context
```

- Python bindingではGIL保持中に作る。
- root `Game`はsession中immutable。
- rootのexact/observable hashはworkerから再計算しない。
- workerへはconst snapshotから作るowning cloneだけを渡す。

### 3.5 `SearchSession`

```cpp
class ParallelSearchSession {
  SearchRootSnapshot root_;
  ParallelSearchOptions options_;
  SearchRandomContext random_;
  SearchLedger ledger_;
  ParallelCancellationToken cancellation_;
  steady_clock::time_point deadline_;
  std::atomic<bool> stop_requested_;
  std::exception_ptr first_error_;
  BoundedQueue<SimulationTask> work_queue_;
  BoundedQueue<std::shared_ptr<PendingEvaluation>> inference_queue_;
  std::vector<std::thread> workers_;
};
```

責務:

- active guard取得・解放。
- logical simulation IDの範囲割当。
- root bootstrap。
- worker起動・停止・join。
- queue close/wake-all。
- outstanding ticketのcancel/drain。
- quiescent invariant検証。
- 最初の例外をcallerへ再送出。
- C++/Pythonから共有可能なone-shot cancellation tokenをcallback/queue/traversal境界で
  観測し、cancel後も全ticketとVLをdrainする。

destructorだけにcleanupを依存しない。正常・例外の両方で明示的な`finish()`を通し、
destructorはnoexceptな最終防衛とする。

active ticket registryは全探索budgetを保持しない。throughput modeでは完了時に逐次eraseして
`O(max_inflight)`、deterministic epoch modeではepoch終了ごとにclearして
`O(deterministic_epoch_size)`へ制限する。pending/pathの最大保持量もこのwindowに従う。

### 3.6 worker-local state

各workerだけが所有する。

```text
Game search_world
path / ReservedPath builder
feature buffer
current-world action mask
portable RNG instance（ticket seedから生成）
decode/action scratch
metrics shard
```

`MCTSSearcher::path_`、`pending_paths_`、`rng_`のようなinstance共有scratchを使わない。

### 3.7 `SimulationTicket`

```cpp
enum class TicketState {
  Created,
  Traversing,
  WaitingInference,
  Committing,
  Completed,
  Cancelled,
  Failed,
};

struct SimulationTicket {
  uint64_t search_id;
  uint64_t simulation_id;
  uint64_t tree_generation;
  SeedManifest seeds;
  ReservedPath path;
  TicketState state;
};
```

- move-onlyまたはsession管理のopaque handleにする。
- commit/cancelは最大1回。
- tree generation不一致のresultはstatsへ適用しない。
- Python dictからpathを再構築するlegacy APIをparallel coreでは使わない。
- trace用IDとledgerを必ず持つ。
- `Completed`は`EvaluatedLeaf | Terminal | MaxDepth`の排他的`CompletionKind`を伴う。
  `PendingEvaluation`のpublication stateを`SimulationTicket::state`へ流用しない。

## 4. Virtual lossを予約として扱う

### 4.1 linearization point

次を一つのnode lock critical sectionに統合する。

```cpp
SelectionReservation select_and_reserve(
    NodeHandle node,
    const std::array<uint8_t, MAX_ACTIONS>& world_mask,
    const SelectionContext& context);
```

node lock内の順序:

1. expansion state、terminal stateを確認する。
2. `N/Q/total_visits/virtual_loss/availability_count`の一貫した状態を読み、候補actionの
   availability、visit、VL、live reservationが64-bit上限を越えないことを全件preflightする。
   1件でもoverflowする場合はmutation前に例外とする。
3. `information_set_union |= world_mask`を行い、worldでavailableな全actionの
   `availability_count`を増やす。
4. **更新後**のnode unionとcurrent-world maskのintersectionを作る。
5. 48 actionの未mask base policyを候補mask上で一時正規化する。
6. PUCT、forced playout、root noiseを計算する。
7. actionを選択する。
8. 該当edgeのvirtual lossを増やす。
9. 一意なreservation IDを発行する。
10. move-only `ReservedPathEntry` を返す。

選択後・VL追加前というrace windowを作らない。

### 4.2 `ReservedPathEntry`

```text
NodeHandle
TreeKey
node generation
action
player
reservation ID
state = live | committed | aborted
```

公開`PathEntry`のhash/action/playerだけでは、stale treeと他threadのreservationを区別できない。
parallel内部pathはstrong `NodeHandle` とgenerationを持つ。

### 4.3 commit

pathを逆順に処理し、各nodeについて一つだけlockする。

node lock内で同時に行う。

- reservation token/generationを検証。
- virtual lossを1だけ減らす。
- `N[action]`、`total_visits`、`Q[action]`を更新する。
- reservationをcommittedにする。

VLが消えた後、real visitがまだ見えない隙間を作らない。

### 4.4 abort

- pathを逆順に処理する。
- statsは更新せず、自分のreservationだけ解放する。
- 0 clampで二重解放を隠さず、parallel内部ではassert/ledger errorにする。
- destructor cleanupは未処理entryだけに適用する。

### 4.5 不変条件

```text
virtual_loss(node, action)
  == live reservation token count(node, action)

virtual_loss >= 0

quiescent:
  all virtual_loss == 0
  added == released
  every ticket in Completed | Cancelled | Failed
```

debug/stress buildではtoken registryを持ってexact ownershipを検査してよい。Releaseでは
aggregate countだけにしてoverheadを避ける。

### 4.6 PUCTとforced playout

- 初回は現行のVL weight `0.3` とQ penalty式を維持し、並列構造変更とtuningを分ける。
- forced playout thresholdはcompleted `N`だけでなくin-flight reservationも考慮し、
  全workerが同じforced edgeへ集中しないようにする。
- `availability_count[action]` は、そのactionがcurrent worldで利用可能だったnode訪問回数を
  node lock下で増やす。worldで利用不能だった機会をforced playoutの母数に含めない。
- 初期PUCTは互換性のためparent `total_visits`を使うが、availabilityが不均一なfixtureでは
  rare actionへ探索bonusが過剰にならないかを記録する。PUCTの分子をavailability基準へ
  変更する場合は、並列安全化と混ぜず探索アルゴリズムの別ADR・品質試験にする。
- legacy互換で`current_sim`を残す箇所ではwall-clock完了数ではなく、coordinatorが割り当てた
  logical simulation ordinalを使う。parallel forced playoutの機会数は上記availability
  countを正とし、global ordinalだけを母数にしない。
- tieは初回はaction ID順を維持し、random tie-break導入は別変更にする。

## 5. Expansion競合

### 5.1 state machine

```text
Unexpanded --claim--> Evaluating --publish--> Expanded
                                \--terminal--> Terminal
                                \--failure---> Unexpanded

Expanded --公開terminal確認済み--> Terminal
```

`is_expanded`だけを先に/後に書くpublicationは使わない。state、policy、value、maskは
node lockの下で一貫してpublishする。

### 5.2 single-owner + waiter

最初に`Unexpanded`を見たticketがevaluation ownerとなり、`PendingEvaluation`を作る。
同じnodeへ到達したticketは重複推論せずwaiterとしてattachする。

```text
owner path ----+
waiter path ---+--> PendingEvaluation --> 1 NN evaluation
waiter path ---+                         --> publish once
                                         --> each path commits once
```

要件:

- waiterも一つのlogical simulationとしてbudgetとvisitに数える。
- waiterのVLは結果publishまで保持する。
- 同じ`PendingEvaluation`へdeduplicateされるowner/waiter間だけはfeature digest一致を検査する。
  現実装は展開済みnodeへ後から到達したfeatureを再照合しないため、「同一`TreeKey`なら常に
  feature一致を実行時保証する」とはしない。展開時に二次feature signatureをnodeへ保存し、
  後続到達でも照合することをstable化前のhardening gateとする。
- current-world maskはwaiterごとに保持し、node base policyとselection時に交差させる。
- owner callback failure時はowner/waiter全pathをabortする。
- node generationごとにpublishは最大1回。
- `PendingEvaluation`のpublishはnodeへ評価結果を一度だけ公開する操作であり、ticketの
  simulation commitではない。publish後、attached ticketを1件ずつ独立にcommitし、各ticketの
  path/VL/ledgerをexactly onceで完了させる。

`PendingEvaluation`はticketとは別のstate machineにする。

```cpp
enum class PendingState {
  Open,
  Closing,
  Published,
  Failed,
  Cancelled,
};

struct PendingEvaluation {
  std::mutex mutex;
  PendingState state;
  TreeKey key;
  uint64_t tree_generation;
  std::vector<uint64_t> attached_ticket_ids;  // claimantを先頭に含む
  EvaluationPayload payload;
};
```

ownership契約:

- `SearchSession::ticket_registry`が全ticketをterminal stateまでstrong所有する。
- `SearchSession::pending_registry`がpendingを`Open`からterminal state・waiter drain完了まで
  strong所有し、bounded queueも処理中だけ`shared_ptr`を持つ。
- nodeはpendingをweak所有する。pendingのattached listはclaimantを含むticket IDだけを持ち、
  session registryから解決するため、node/pending/ticket間にstrong cycleを作らない。
- evaluationは「claimしたowner ticket」ではなくsession/pendingの仕事である。owner ticketだけが
  cancelされても、waiterが残る限りevaluationを継続できる。

### 5.3 pending close手順

node lockとpending lockの同時保持を避けるため、次の手順を固定する。

1. pending lockで`Open -> Closing`。新規waiter attachを止める。
2. pending lockを解放する。
3. node lockでpolicy/value/stateをpublishする。
4. node lockを解放する。
5. pending lockでattached ticket listをswapし`Published`にする。
6. pending lockを解放する。
7. publish済みpayloadを各ticketへ渡し、owner/waiter pathをticket単位で個別にcommitする。

Closing中にattachできなかったworkerはnode publicationを再確認する。node lockとpending lockを
逆順に取得しない。

競合ごとのlinearizationを固定する。

| 操作 | linearization point | 結果 |
|---|---|---|
| attach | pending lock下の`Open`確認とticket ID追加 | 成功時だけwaiter VLをpendingへ委譲 |
| close | pending lock下の`Open -> Closing` | 以後attachは失敗しnodeを再読込 |
| publish | node lock下の`Evaluating -> Expanded/Terminal` | policy/value/stateを一括公開 |
| callback failure | pending close後、node lock下の`Evaluating -> Unexpanded` | 全ticketを一度だけabortし`Failed` |
| session cancel | pending close後、nodeを`Unexpanded`へ戻す | 全ticketをabortし`Cancelled` |
| late result | pendingが`Published/Failed/Cancelled`かgeneration不一致 | stats無変更、duplicate/stale metric |

publish/failure/cancel後、waiter terminal化とVL解放を終えてからpending registryをeraseする。
queueから最後のstrong referenceが消えてもnodeが永久に`Evaluating`へ残らないことをstress testする。

### 5.4 root bootstrap

root未展開のまま全workerを開始すると、budget全体が1つのpending root評価へwaiter attach
し得る。coordinatorがsimulation発行前のbootstrap処理としてrootを同期的にexpandし、その後
workerを開始する。

- bootstrapはsimulation budget、simulation ID、ticket ledgerに含めない。
- `num_simulations`件のlogical simulationはbootstrap完了後に別途すべて発行する。
- rootがterminalならworkerを開始しない。
- root policy確定後、Dirichlet noiseをsearchにつき一度生成する。

## 6. Scheduler

### 6.1 common topology

- caller/controller thread: session lifecycle、GIL境界、最終結果。
- N native workers: tree traversal、Game transition、feature/mask生成。
- 1 inference coordinator: batch形成、Python/NN callback、result validation。
- 初期版のpublish/backprop: coordinatorが行う。
- 将来版: native inferenceならcommit workerを分けられるが、まずserializeしてoracleを作る。

### 6.2 bounded queues

初回はmutex + condition variableのbounded queueを使う。

- lock-free queueは使わない。
- `max_inflight_simulations`を明示する。
- queue close時は全waiterをwakeする。
- queue lock中にtree/node lockを取らない。
- node lock中にqueue pushしない。
- backpressureによりVLとpending pathの増加を制限する。
- active ticket registryは完了eventごとにeraseし、保持量を`O(max_inflight_simulations)`にする。

推奨初期値:

```text
num_threads = 1（公開既定値）
inference_batch_size = 16
max_inflight_simulations = max(2 * num_threads, inference_batch_size * 2)
batch_wait_us = 実NN profile後に決定
```

### 6.3 throughput mode

- workerがtask queueからsimulation ID付きtaskを取る。
- tree更新は到着順に行われる。
- random seedはsimulation IDに固定され、worker割当には依存しない。
- 最終tree/actionのschedule依存を許容する。
- ledger、VL、lifecycle、information-set invariantは常に保証する。

### 6.4 deterministic epoch mode

debug/CI用であり、性能機能ではない。

1. worker数と無関係な固定`epoch_size`（初期test値32）でsimulation ID範囲を作る。末尾だけ
   partial epochを許し、前epochは全commit/abortしてから次へ進む。
2. coordinatorがsimulation ID順に、determinization作成、各plyのGame transition、world mask
   計算、selection/reservationを含む**leafまでのfull traversal**を直列実行する。
3. Python callbackの「同一search内で同時実行数1」契約を優先し、leaf encode/evaluationも
   coordinatorから同期実行する。`num_threads`は結果互換性入力であり、このmodeは
   workerによる非同期completion reorderを作らない。
4. batch membershipと入力順をsimulation ID順に固定する。同一leafのowner/waiter決定も
   traversal順で一意になる。
5. callback戻り値を同期検証し、publish/backpropをsimulation ID順、world ID順に行う。
6. float reduction順も固定する。

同じtraceならworker数/backendを変えてもcanonical bytesとtree digest一致を要求する。
ただし非同期completion順の入替えはこのmodeの検証範囲外である。このmodeはtraversalと
evaluationを意図的に直列化するためspeedup評価には使わない。
epoch ticket配列はepoch終了ごとに解放し、保持量は全budgetではなく`O(epoch_size)`である。

### 6.5 cooperative cancellation / timeout

- `ParallelCancellationToken`はcopy間で状態を共有するone-shot tokenとし、C++ APIと
  Python bindingで同じ意味論を使う。pre-cancelはcallback/tree準備を行わず、in-flight
  cancelは新規発行を止めてpending/path/workerをdrainする。
- `timeout_ms`はsoft deadlineであり、queue待機、traversal、callback前後で観測する。
  実行中のC++/Python evaluatorをpreemptしないため、無限blockは外部watchdogで隔離する。
- root-parallelではdeadlineをAPI entryから測り、evaluator factory生成と共通root
  bootstrapの時間も予算に含める。workerには残り時間だけを渡す。
- Python root-parallelのserialized callbackは共有mutex取得後にcancel/deadlineを再検査する。
  遅いcallbackの後ろで待ったworkerを順にPythonへ流さず、stale callback backlogを捨ててdrainする。

### 6.6 simulation budget

- root bootstrap evaluationはsearch準備であり、public `num_simulations`のbudget外とする。
- public `num_simulations` は`Completed` ticket数を意味し、completion kindは
  `EvaluatedLeaf | Terminal | MaxDepth`のいずれか一つである。
- user cancelでは未発行budgetを補充しない。
- invariant failureはsilent retryせずsearch全体をfailする。
- expansion waiterはcancelではなく、同じ評価結果で一度commitする。
- pending publish回数とsimulation completion数を混同しない。1 publishへ複数ticketがattach
  した場合も、各ticketが1回ずつcommitしてbudgetを満たす。
- simulation IDは欠番を許容するが再利用しない。
- `completed_evaluated/completed_terminal/completed_max_depth/issued/cancelled/failed`を結果metadataへ
  返し、`completed`は先頭3 counterの和として導出する。NN callback回数は
  `evaluation_requests`としてsimulation outcomeとは分離する。

## 7. Tree lifecycle

### 7.1 active guard

```text
Idle --begin_search--> Active --finish/drain--> Idle
```

- 同一`MCTS`の二重searchはfail-fast。
- `clear()`、prune、config変更、manual expand/update/VL操作はActive中fail-fast。
- API内でActive終了を待たない。
- session終了はworker join、ticket drain、VL=0確認後だけ成立する。

### 7.2 tree generationとstale result

- `clear()`、key version変更、tree replacementでgenerationを増やす。
- request/ticketはgenerationを記録する。
- result適用前にgenerationとticket stateを検証する。
- stale/duplicate resultはstatsを変えず、metricへ記録して明示的エラーにする。
- external inferenceがsessionより長生きしないAPIを初期版の契約にする。

### 7.3 prune

初期版:

- search開始前または終了後だけ実行する。
- `ParallelSearchOptions.max_tree_nodes`の既定値は50,000である。shared-treeでは単一treeのhard
  limit、root-parallelでは全active worker treeを合わせたaggregate limitとして扱い、active
  workerへ商・余りで均等分配する。budget 0で起動しないworkerには割り当てない。
- active中のtree size上限到達時は`TreeCapacityReached`系のpartial/errorとして安全終了する。
  ephemeral leaf evaluationは採用しない。
- 新task発行を止め、capacityを検出したticketと未commit ticketをreason付き`Cancelled`として
  abortし、既commit statsは維持する。全worker/pendingをdrainしVL=0を確認する。
- rootが展開済みならpartial probabilitiesと`status=TreeCapacityReached`、completed/target budgetを
  返す。root未展開なら明示的errorにする。silentに要求budget達成として扱わない。
- silent concurrent eraseはしない。

将来active pruneを実装する場合も、NodeHandle lifetime、tree generation、pending evaluationを
含む別フェーズとする。

## 8. Lock規約

### 8.1 conceptual order

```text
SearchSession control
  -> TreeShard/Table
    -> NodeRecord
      -> PendingEvaluation
        -> WorkQueue
```

ただし通常実装では複数lockを重ねない。

- shardからhandle取得後、shard lockを解放してnode lockを取る。
- node更新後、node lockを解放してqueueへpushする。
- pending close後、pending lockを解放してnodeへpublishする。
- callback中はsession controlを除くtree/node/ticket/queue lockを一切持たない。
- backpropはpath上のnodeを一つずつ逆順にlockする。

debug buildではlock-rank assertionまたはthread-local held-lock maskを導入し、違反を早期検出する。

### 8.2 prohibited patterns

- node lockを保持してPython/GPU callbackを呼ぶ。
- GIL保持中にsearch終了を待つpublic mutator。
- pending/ticket lockを保持してcondition variable待機し、completion側が同lockを必要とする構造。
- map element raw pointerをlock/lifetime guardなしで保持する。
- `clear_virtual_losses()`をactive searchの通常cleanupに使う。

## 9. Python API案

新APIは既存custom encoder searchと分離する。

```python
token = core.ParallelCancellationToken()
options = core.ParallelSearchOptions()
options.num_threads = 4
options.num_simulations = 4096
options.batch_size = 16
options.max_inflight = 32
options.max_tree_nodes = 50_000
options.mode = core.ParallelSearchMode.THROUGHPUT
options.master_seed = 42
options.cancellation_token = token
result = core.mcts_search_parallel_native(
    mcts, root, options, inference, 1.0
)
```

現行の戻り値:

```text
visits[48] / q_values[48] / probabilities[48]
ledger（issued/completed_evaluated/completed_terminal/completed_max_depth/cancelled/failed/evaluated_boards等）
stop_reason / partial
resolved_seed / search_nonce / rng_version
tree_generation / tree_size / elapsed_microseconds
```

契約:

- `num_threads=1`が既定。
- worker traversalはnative encoder/featurizer固定。
- inference resultは48枠の未mask base policyを返す。MCTS側がticketごとのworld maskを適用する。
- `flat_valids`等を使ってcallback側でillegal actionのpriorを永久に0へ落とすlegacy contractは
  parallel APIへ流用せず、adapter/migrationまたはsingle-thread legacy pathを使う。
- configは `get_config_snapshot()` / `set_config()` で扱う。Python `mcts.config` getterも
  detached copy、property setterは`set_config()`とし、repo内consumerを移行する。`set_config()`は
  control lockを通りActive中fail-fastする。内部referenceを返すpropertyはstable parallel APIに
  残さず、過去に取得したcopyのfield変更はengineへ影響しない。
- Python inference callbackはcoordinator threadから逐次呼出し。
- callback inputはcontiguous owning buffer。
- callback戻り値はcount、shape、dtype、finiteをtree mutation前に全件validate。
- callback exceptionはworker停止・ticket cancel・join後に元のPython例外を再送出。
- callbackが`TreeCapacityReachedError`を送出しても内部tree capacity partialへ分類せず、cleanup後に
  同じ例外type/messageを再送出する。
- Pythonから`ParallelCancellationToken.request_cancel()`を呼び出せ、返値は
  `stop_reason=Cancelled`のpartial resultとbalanced ledgerを持つ。
- callbackが元rootを変更してもsnapshotに影響しない。
- callbackがinput ndarrayを保持してもlifetimeが安全。

既存 `prepare_batch_simulations()` / `apply_batch_results()` はparallel ticket APIが安定するまで
single-thread manual APIとして扱う。同一MCTSで並行呼出し可能とはしない。

## 10. Failure handling

全failureで共通のshutdown順序を固定する。

1. 最初のerrorを`exception_ptr`へ保存する。
2. stop flagを立てる。
3. 新規task発行を止める。
4. work/inference queueをcloseし全waiterをwakeする。
5. pending owner/waiter ticketをcancelする。
6. 全ReservedPathをabortする。
7. workerをjoinする。
8. `inflight == 0`、VL=0、ledger一致を検証する。
9. active guardを解放する。
10. callerへerrorを再送出する。

traversal worker entryは`noexcept`とし、例外は共有failure slotへ保存してwork/event queueをcloseする。
full queueへerror eventをpushしないため、allocation/blocked pushによる`std::terminate`を避けつつ
coordinatorをwakeできる。root-parallelで複数workerがpartial終了した場合の集約優先順位は
`Completed < Cancelled < TimedOut < TreeCapacityReached < CallbackError < WorkerError`とし、
より重大なreasonをmerged resultへ残す。例外がある場合は全siblingをcooperative stop・joinした後に
例外を再送出する。

failure別の期待:

| Failure | Tree更新 | Budget | 再利用 |
|---|---|---|---|
| user cancel | 完了済みのみ保持 | 未完了をcancel | 可 |
| inference exception | validation前のticketは未commit | search失敗 | 可 |
| malformed result | 該当batchを全件未commit | search失敗 | 可 |
| decode invariant failure | drawを加算しない | search失敗 | 可、trace必須 |
| stale result | mutationなし | error metric | 可 |
| duplicate result | mutationなし | error metric | 可 |
| MAX_DEPTH | drawを一度commit | completed | 可 |
| tree capacity | 新規発行停止、未commit abort、partial result | `TreeCapacityReached` | 可 |

tree capacityでrootが未展開の場合は有効なprobabilityを構成できないため、上表のpartial
resultではなく`TreeCapacityReachedError`を送出する。root展開済みの場合だけpartial resultを返し、
root visitが0ならlegal mask上で正規化したbase priorをfallbackとする。Dirichlet設定時はsearch共有の
root noiseを混合して再正規化し、illegal actionを0に保つ。

## 11. 観測可能metrics

最低限次をsearch resultまたはdebug countersへ出す。

```text
issued / selected / completed_evaluated / completed_terminal / completed_max_depth
cancelled / failed
virtual_loss_added / virtual_loss_released / max_virtual_loss
expansion_claimed / expansion_joined / expansion_publish_conflict
stale_results / duplicate_results / invalid_replay
node_lock_wait_ns / shard_lock_wait_ns
inference_queue_depth / batch_fill / inference_idle_ns
evaluated_boards / duplicate_evaluations_avoided
tree_size / peak_inflight / elapsed_ns
```

metrics更新が新たなhot contentionにならないようworker-local counterをsession終了時にreduceする。
正しさに使うledgerだけはexactな同期を行う。

## 12. 完了条件

このアーキテクチャが実装完了とみなせるのは次を満たした場合である。

- coarse版とsharded版がcanonical `TreeKey`順の同一deterministic traceでbytes/tree
  digest一致。
- select+reserve、commit、abort、expand publishのlinearization testが成功。
- exception/cancel/timeoutの全点でVL=0、worker=0、ticket=0。
- active lifecycle APIがGIL deadlockなしにfail-fast。
- native TSAN full matrixが0 report。
- Python callback同時実行数が常に1。
- 1 thread互換oracleが成功。
- pending dedup中のfeature digest検査に加え、展開済みnodeの二次feature signature照合が成功。
- sharded版がcoarse版を固定実機で上回る。
- fixed-time探索品質がsingle-thread基準から許容範囲内。
