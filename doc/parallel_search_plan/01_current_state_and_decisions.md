# 01. 現状監査と設計判断

> 推奨担当: **Codex Sol Ultra**
> トークン節約はモデルtierを下げず、現状値の採取、再現probe、一覧表更新を狭い
> phase contextへ分離して行う。
> Ultra が必要な理由: race と hidden-information semantics を別問題として切り分け、
> tree key の同値関係を定義する必要があるため。

## 1. 監査対象

主な対象は次のとおりである。

| 領域 | 現行ファイル | 主な責務 |
|---|---|---|
| tree/statistics | `src/mcts_tree.h` | node map、PUCT、VL、expand、backprop、prune、RNG |
| batch traversal | `src/mcts_orchestration.h` | 決定化、path replay、leaf request |
| callback traversal | `src/mcts_searcher.h` | Python featurizer/encoder、逐次 search |
| rule adapter | `src/mcts_game_adapter.h` | clone、hash、decode/apply、feature/mask |
| state/hash | `src/game.h`, `src/board.h`, `src/zobrist.h` | clone、hidden shuffle、exact/observable hash |
| public types | `src/mcts_types.h` | node/config/request/path layout |
| Python boundary | `src/bindings.cpp` | request変換、callback、GIL解放 |
| contract tests | `tests/test_phase*_mcts*.py`, `tests/test_mcts_correctness_review.py` | ABI、ownership、world/path/VL |

## 2. 現行の race inventory

現状の `MCTS` は single-thread 前提であり、virtual loss 配列が存在することは
thread-safe を意味しない。

| 共有状態・操作 | 現状 | 並列時の failure | 必要な契約 |
|---|---|---|---|
| `nodes_` | 無保護 `unordered_map` | lookup/insert/erase競合、UB | shard/table lock と stable handle |
| `access_count_` | `get_node()`でも書込み | read同士でもrace | node内`last_access`へ統合 |
| `access_counter_` | plain `uint64_t` | lost update | atomic relaxedまたはsession-local epoch |
| `MCTSNode::N/Q/total_visits` | plain array/scalar | lost update、torn snapshot | node lock下の一貫更新 |
| `virtual_loss` | plain `int32_t` | lost add/remove、他tokenのVLを誤解放 | selectと予約を一操作化、token ownership |
| `is_expanded` と policy/mask | 無同期 publication | readerが半初期化nodeを観測 | expansion state machine + node lock |
| `rng_` | 共有`mt19937` | data race、schedule依存seed | logical IDからworker-local seed派生 |
| `config_` | Pythonへmutable reference | 探索中変更race | search開始時snapshot、active中変更拒否 |
| `clear()` | 即時map clear | in-flight path/resultを破壊 | active中fail-fast、tree generation |
| `prune_if_needed()` | node erase | stale pointer、更新消失 | quiescent point限定 |
| batch request | plain dict/struct | duplicate/stale apply | consumed ticket + request/tree generation |
| `MCTSSearcher::path_` | instance共有vector | worker間path混線 | worker-local scratch |
| `pending_paths_` | instance共有vector | completion対応崩壊 | ticket所有またはcoordinator管理 |
| `Board::hash()` cache | `const`内でmutable write | shared root hash race | coordinatorで一度計算、workerはowning clone |

### 2.1 select と virtual loss の分離

現行 batch path は概ね次の順である。

```text
select_action_with_virtual_loss(hash)
add_virtual_loss(hash, action)
```

各関数を別々に mutex 化しても、二つの thread が同じ統計を読み、同じ action を選んで
からVLを加算できる。必要なのは関数単位の thread safety ではなく、**選択と予約を同じ
linearization point にすること**である。

### 2.2 pointer / reference API

`get_node()` は `MCTSNode*` を返す。内部 map を並列insert/pruneする設計では、lock外の
raw pointerを公開できない。また `config()` はmutable referenceを返す。

計画上は次のように分離する。

- parallel core は `NodeHandle` と `get_node_snapshot()` だけを使う。
- Python `get_node()` はlock下で作った値copyを返す。
- 既存C++ `MCTSNode* get_node()` はlegacy single-thread APIとして段階的にdeprecated化する。
- active parallel search 中にlegacy pointer APIを使うことは拒否または未定義ではなく、
  debug/runtime guardで明示的に失敗させる。
- mutable config referenceは `get_config_snapshot()` / `set_config()` に置き換え、search sessionは
  構築時のcopyだけを読む。stable parallel APIではPython `mcts.config` getterを**detached copy**、
  property setterを`set_config()`呼出しに変更する。repo内の `mcts.config.field = value` は
  `cfg = mcts.get_config_snapshot(); cfg.field = value; mcts.set_config(cfg)`へ移行する。
- `set_config()`はMCTSのcontrol lockを通り、Active中は待たずに例外にする。過去に取得した
  config copyを書き換えてもMCTSには影響しない。内部参照を返す旧getterはPythonから削除し、
  C++に移行用unsafe APIを残す場合もlegacy single-thread buildに限定する。この互換性変更を
  stable parallel公開前に完了し、暗黙に「次searchだけへ反映」という挙動は採用しない。

`MCTSNode` は現在公開layoutがtestで固定されているため、直接 `std::mutex` を埋め込まない。
内部 `NodeRecord` がmutexとlifecycleを持ち、`MCTSNode`はsnapshot DTOとして維持する。

## 3. 並列化前の意味論 blocker

### 3.1 hidden reserved tier と observable hash

相手のhidden reserved cardについて、IDは非公開だがtierは公開情報である。現状の
observable hashはhidden cardの存在をsentinelで表す一方、slotごとのtierを区別しない。
一方、state featureはtierを含む。

したがって、次が起こり得る。

```text
observable_hash(A) == observable_hash(B)
features(A)        != features(B)
```

異なる情報集合が同じnodeを共有するため、並列化前に修正する。

修正契約:

- Zobristへ `hidden_reserved_level[player][slot][empty|L1|L2|L3]` 相当を追加する。
- card IDはhashしない。
- slotごとの公開signature `empty|L1|L2|L3` をhashする。
- 同tier内でhidden IDだけを変えた場合はobserver keyを維持する。
- slot signature vectorが変わった場合だけobserver keyを変える。同じtierのhidden IDをoccupied
  slot間で交換してsignature vectorが同じならkeyも同じである。
- key versionを持たせ、旧treeと新treeを混用しない。

### 3.2 異なる公開leafのmulti-world平均

現行 `prepare_batch_simulations()` はworld 0のleaf hashだけを保持し、追加worldはrootから
同じaction ID列をreplayしてfeature/maskを生成する。その後、全worldのpolicy/valueを
平均し、valid maskをORしてworld 0のnodeへpublishする。

しかし、deckからカードが公開されるactionでは、決定化ごとに公開結果が変わる。

```text
world 0: RESERVE_VISIBLE -> visible slotへ card A -> leaf key X
world 1: RESERVE_VISIBLE -> visible slotへ card B -> leaf key Y
X != Y
```

XとYは別の公開局面であり、同一nodeへ平均してはならない。

初期parallel版の契約:

- `use_determinization=true` では `num_determinizations=1` を一旦必須にする。
- 各logical simulationが1つのroot-sampled worldを持つ。
- `num_determinizations>1` 再導入時はworldごとに独立path/leaf keyを持つsimulationとして扱う。
- 異なるleaf keyのpolicy/value/maskを同一nodeへreduceしない。
- 同じleaf keyへgroupできる場合はfeature digest一致をdebug buildで検証する。
  world-local maskは同じ`TreeKey`でも異なり得る正常な入力であり、一致を要求しない。

この修正により、旧 `num_determinizations>1` の結果は変化する。compatibilityより
correctnessを優先し、明示的な仕様変更としてtestと文書を更新する。

### 3.3 current-world action availability

treeはroot observerから見た情報集合で共有される。相手のhidden reserved card IDなどに
よって、そのworldで実行可能なactionが異なる場合がある。

nodeに保存したunion maskだけでactionを選ぶと、現在worldでdecode/apply不能なactionを
選び得る。現状の「decode失敗をdrawとしてbackpropagate」は探索統計を汚染する。

初期契約:

- workerは各node到達時にsimulation-local action maskを計算する。
- `select_and_reserve()` はnode lock内で、まず `information_set_union |= world_available` と
  availability counter更新を行い、その**更新後のunion**と`world_available`のintersectionだけを
  候補にする。後発worldで初めて合法になったactionを選択前の古いunionで失わない。
- policyは48 actionのbase priorとして保持し、current-world maskで選択時に再正規化する。
- nodeはavailability unionに加え、診断用 `availability_count[action]` を保持してよい。
- decode/apply失敗はticketをabortし、debug buildではinvariant failureにする。
- release buildでもdraw visitを加算せず、metricとtraceへ残す。

### 3.4 observer/domainを含むTreeKey

裸の64-bit hashへ暗黙のXORだけを足す方式ではなく、内部keyを構造化する。

```cpp
struct TreeKey {
  uint64_t position_hash;
  uint32_t key_version;
  uint8_t observer;
  uint8_t domain;      // exact / observable
  uint8_t mode_bits;   // simple_payment / blank_refill
};
```

要件:

- `TreeKey` equalityとhashを明示実装する。
- observer 0/1を別domainにする。
- exactとobservableを別domainにする。
- mode bitsを含める。
- key version変更時に既存treeを再利用しない。
- hidden determinization seed自体はkeyへ入れない。情報集合を共有する意図を維持する。

既存の裸`uint64_t` APIからobserver/domain/mode/versionは復元できないため、橋渡しを次に固定する。

- parallel coreと新diagnostic APIは`TreeKey`を必須にする。
- 既存 `get_node/expand_node/update_stats/get_action_probs(uint64_t)` は
  `LegacyExact{observer=None, mode_bits=0, legacy_key_version}` 専用tree facadeとして維持する。
  移行期間は既存`uint64_t -> MCTSNode` storageと新`TreeKey -> NodeRecord` storageを物理的に分ける。
- legacy facadeからparallel/observable treeを参照・更新することは禁止し、曖昧な自動変換をしない。
- 新APIには`get_node_snapshot(TreeKey)`等のoverloadを用意する。parallel searchの通常結果は
  root probabilitiesを直接返し、裸hashで再検索させない。
- 固定済みの旧signature contract testはlegacy facadeについて維持し、TreeKey APIのcontract
  testを別に追加する。deprecation期間と削除versionはrelease noteへ明記する。

## 4. GILと外部操作

`mcts_search` はroot snapshot後にGILを解放する。したがって別Python threadは同じ`MCTS`
へ `clear()`、`expand_node()`、config変更、別searchを呼べる。GILはshared treeのlockではない。

初期parallel APIは次を保証する。

- search開始時にatomicなactive guardを獲得する。
- 同じ`MCTS`への二重searchは即時例外。
- active中の `clear()`、prune、config変更、legacy manual mutationは即時例外。
- retained mutable `mcts.config` referenceをstable契約へ残さない。sessionがsnapshot後に
  `config_`を読まないだけでは、次searchの設定更新順と他public methodの安全性を定義できない。
- これらのAPIはsearch終了を待たない。
  - GIL保持threadが`clear()`内で待ち、coordinatorがPython callback用GILを待つdeadlockを
    避けるためである。
- 別`MCTS` object同士のsearchは並列実行できる。
- root snapshotはGIL保持中に作り、root key/hashもこのthreadで確定する。

## 5. 採用する設計判断

### ADR-PS-001: final方式はshared tree、root-parallelはoracle

- 採用: 1 search / 1 shared tree。
- root-parallelはrace-free比較、fallback、scaling下限として実装する。
- 理由: 独立treeはmemoryとNN評価がworker数倍になり、深い探索統計を共有できない。

### ADR-PS-002: coarse lockを先に完成させる

- global tree mutex版でledger、ticket、shutdown、TSANを確立する。
- その後にshard/node lockへ置換する。
- coarse版は性能が低くてもcorrectness oracleとして残す。

### ADR-PS-003: 初回はlock-free不採用

- Q/N/VLだけatomic化してもmap、publication、prune、stale resultは解決しない。
- lock-freeはsharded版でlock waitが支配的と実測された場合だけ別ADRにする。

### ADR-PS-004: parallel APIはnative traversal限定から開始

- `ActionEncoderCpp`、`StateEncoder`、native Game transitionをworkerで使う。
- Python featurizer/encoderをworkerから呼ばない。
- Python callbackはcoordinatorによるbatch inferenceだけに限定する。
- 既存custom callback searchはsingle-threadのまま維持する。

### ADR-PS-005: prune/clearはquiescent point限定

- 初期版はactive search中のeraseを行わない。
- tree上限到達時は新規task発行を止め、未commit ticketをreason付きcancel/abortして
  `TreeCapacityReached` partial resultを返す。ephemeral evaluationは行わない。
- session drain、VL=0、ledger一致を確認し、pruneはsession終了後だけ実行する。
- silent eraseやworker待機はしない。

### ADR-PS-006: safetyとdeterminismを別契約にする

- throughput modeはrace-free・seed trace可能だがtree digestはschedule依存。
- deterministic epoch modeだけがworker数をまたぐdigest一致を保証する。

## 6. 非目標

今回の計画には次を含めない。

- GUI、学習モデル、棋譜データのrepo追加。
- GPU backend自体の実装。
- RIS-MCTS等へのアルゴリズム全面変更。
- 既存`Game(seed)`の初期配置乱数列変更。
- 初回リリースでのcross-platform bitwise一致するDirichlet gamma sampler。
- 全面lock-free tree。

ただし、実NN側のbatch APIと接続できるqueue/contiguous buffer境界は設計対象に含める。

## 7. PS-0で固定するbaseline

並列化前に最低限次を保存する。

- 現行single-thread native MCTSのtree digest、path digest、root policy。
- determinization off/on、world 1の固定seed corpus。
- callback searchとbatch searchの差分。
- 例外、MAX_DEPTH、terminal、forced playout、Dirichletの契約。
- 既存性能:
  - non-determinized synthetic: 94,427 simulations/s。
  - determinized synthetic: 108,441 simulations/s。
- `MCTSNode` layout、request ownership、root snapshot契約。
- 問題を再現するfixture:
  - hidden reserve tier hash alias。
  - divergent public leaf multi-world merge。
  - world-local mask差。

意味論上誤っている結果そのものをgoldenとして固定しない。fixtureは「現行問題を再現し、
PS-1後に正しい期待値へ反転する」characterization testとして扱う。

## 8. PS-1完了ゲート

並列構造へ進む前に、次をすべて満たす。

- hidden reserveのslot別`empty|tier` signature vectorがobservable keyへ反映される。
- 同tier内hidden ID変更ではobserver keyが変わらない。
- observer/domain/mode/key versionをTreeKeyが区別する。
- divergent leaf hashを同じnodeへ平均しない。
- current-worldで不可能なactionを選ばない。
- action replay失敗をdrawとしてvisit加算しない。
- root snapshotとhash cacheが不変。
- 既存通常test、hash corpus、determinization corpusが成功する。
- PS-1の意味論変更をdocumentし、旧挙動との非互換を明示する。
