# 03. RNG・決定化・再現性設計

> 推奨仕様・実装担当: **Codex Sol Ultra**
> portable RNG utility、golden、Dirichlet samplerも同じ実在モデルを使い、対象contextを
> 当該phaseの仕様・header・testへ限定してトークンを節約する。
> 理由: seedの再現性だけでなく、information-set key、world-local legality、公開chance
> outcomeを同時に扱う必要があるため。

## 1. 現状と問題

現行には独立した二つのmutable RNGがある。

- `MCTS::rng_`
  - batch決定化seed
  - Dirichlet noise
- `MCTSSearcher::rng_`
  - callback searchの決定化seed

どちらも`std::random_device`で初期化され、同じconfig/rootでも経路間で乱数列が一致しない。
共有RNGをmutexで囲うだけでは、どのworkerが次の値を取るかがscheduler依存になる。

また、`Board::randomize_hidden_information(uint64_t)` は内部で `std::mt19937` を作る。
`mt19937`の単一seed constructorは実質32-bit入力として扱われ、64-bit seed identityを十分に
保持できない。さらに`std::shuffle`、`std::gamma_distribution`のbit列は標準library実装を
またいで保証されない。

## 2. 再現性の定義

次のレベルを分ける。

| Level | 名称 | 保証 |
|---|---|---|
| R0 | race-free | RNG/data raceがなく、lifecycle invariantを満たす |
| R1 | task seed reproducible | 同じlogical identityはworker数に関係なく同じseed/world |
| R2 | same-build deterministic epoch | 同一binary/NNでworker数・完了順を変えてtree digest一致 |
| R3 | cross-toolchain bitwise | compiler/libstdc++/libc++をまたいでworld/noiseまで一致 |

初回productionで必須なのはR0/R1、CI debug modeでR2とする。R3はsearch用shuffleに対して
達成するが、Dirichlet gammaとNN浮動小数点までの完全一致は別フェーズとする。

## 3. `SearchRandomContext`

```cpp
struct SearchRandomContext {
  uint64_t resolved_master_seed;
  TreeKey root_key;
  uint64_t search_nonce;
  uint64_t simulation_id_base;
  uint32_t rng_version;
};
```

### 3.1 master seed

- 明示seed `0`を有効値として扱う。
- `seed=None`の場合だけOS entropyを取得する。stateful shared-tree経路では
  **`MCTS`構築時に一度**解決し、searchごとやworkerごとに取り直さない。
  stateless root-parallel coordinatorもentryで一度だけ解決し全workerへ共有する。
- 実際に使った`resolved_master_seed`を結果とtraceへ出す。
- resultはseed/nonceと一緒に`rng_version`も返し、別versionのreplayを暗黙受理しない。
- workerは`random_device`を呼ばない。

### 3.2 search nonce

- `MCTS` instance内でsearch開始ごとにcoordinatorが単調増加させる。
- `clear()`はtree generationを変えるが、nonceを暗黙resetしない。
  - accidentalなworld列再利用を避けるためである。
- 完全replayが必要な場合はquiescent時専用の `reset_replay_sequence(seed, nonce)` を使う。
- traceはseed、nonce、simulation rangeを必ず記録する。
- Python/C++の`reset_replay_sequence(seed, nonce)`はquiescent時だけ呼び出せ、
  tree/generationは保持したまま次のresolved seed/nonce列を明示的に固定する。

### 3.3 simulation ID

- coordinatorが論理IDを割り当てたtaskをqueueへ入れる。
- workerがatomic counterを奪い合ってseed identityを決めない。
- IDはsearch内で一意、cancel/failedでも再利用しない。
- worker ID、OS thread ID、queue取得順をseedへ含めない。

## 4. Domain-separated seed derivation

最低限次のdomainを分離する。

```cpp
enum class RandomDomain : uint64_t {
  RootDeterminization,
  AdditionalWorld,
  RootDirichlet,
  PuctTieBreak,
  FinalTemperatureSample,
  StressScheduler,
};
```

論理入力:

```text
master seed
root TreeKey digest
search nonce
RNG version
domain tag
simulation ID
world ID
sub-index
```

導出関数はfieldを単純XORしない。順序とdomainを保つmixをrepository内で固定する。

```cpp
uint64_t derive_seed(const SearchRandomContext& ctx,
                     RandomDomain domain,
                     uint64_t simulation_id,
                     uint32_t world_id,
                     uint32_t sub_index);
```

期待契約:

- world 0 seedは追加world数を変えても不変。
- determinization on/offでDirichlet seedが変わらない。
- cancelされたsimulationが後続seed列をずらさない。
- worker数・batch completion順を変えてもmanifestが同じ。
- domain間で同じ入力tupleを使ってもseedが重複しない。
- root position/observer/modeが変わればseed identityも変わる。ただし次項の
  `RootDirichlet`だけはexact/observableというstorage domain差を意図的に正規化する。
- exact/observable storage domainの切替で`RootDirichlet`の論理streamは変わらない。
  determinization on/offの比較で同じroot/master seed/nonceを使った場合、実際の
  root-noise arrayも一致することを固定testで検証する。

## 5. Portable search RNG

既存 `Game(seed)` の初期配置はgolden互換性のため変更しない。parallel search用のhidden
shuffleだけ、repository-owned RNGへ分離する。

候補API:

```cpp
class SearchRng {
public:
  explicit SearchRng(uint64_t seed);
  uint64_t next_u64();
  uint64_t uniform_bounded(uint64_t bound);
};

void randomize_hidden_information_portable(
    Board& board, uint8_t observer, SearchRng& rng);
```

要件:

- algorithmと`rng_version`を固定する。
- 64-bit state/outputを使う。
- bounded samplingはmodulo biasを避けるrejection samplingにする。
- Fisher–Yatesのindex順とbounded drawを明示する。
- fixed seed/world corpusのbyte digestをgolden化する。
- 既存public `Board.randomize_hidden_information(seed)` の互換挙動は維持し、新search pathだけ
  portable overloadを使う。

PRNGの選択自体より、versioning、domain separation、golden corpusが重要である。

## 6. Information-set key契約

### 6.1 observable keyに含めるもの

- bank、visible cards、nobles。
- 両playerの公開gems、bonuses、points、購入枚数等。
- reserved slotのempty/occupied。
- observer自身が知るreserved card ID。
- 相手hidden reservedのslotとtier。ただしcard IDは除外。
- current player、turn、waiting noble、final round、winner。
- observer、mode、key version。

### 6.2 含めないもの

- 未公開deck順。
- observerから見えないhidden reserved ID。
- determinization seed、worker ID、simulation ID。

### 6.3 必須同値test

```text
同じ公開情報 + hidden IDだけ違う     -> same observable TreeKey
slotごとのempty/tier vectorが違う    -> different TreeKey
同tier hidden IDをslot間交換しvector不変 -> same TreeKey
observerが違う                       -> different TreeKey domain
exact/observableが違う               -> different TreeKey domain
simple/blank modeが違う              -> different TreeKey
```

hash collisionは64-bit上理論上残る。debug/replay buildではTreeKeyに短いpublic-state digestを
併記し、同一hashでdigest不一致を検出できるようにする案を検討する。

## 7. 1 simulation / 1 determinization

初期parallel版のSO-ISMCTS契約を次に固定する。

1. root observerはsearch開始時playerで固定する。
2. simulation IDからroot determinization seedを導出する。
3. immutable root snapshotからworker-owned worldを作る。
4. traversal中、nodeごとにそのworldのaction maskを計算する。
5. `select_and_reserve(node, world_mask, ...)` を呼ぶ。
6. actionをworldへapplyする。
7. 公開状態が変わるたびに、そのworldのobservable TreeKeyを計算する。
8. leafはそのworldが実際に到達したTreeKeyへpublishする。

worldの`Game`/`Board`をthread間共有しない。`Board::hash()`のmutable cacheはworker-owned
instanceでだけ更新する。

## 8. World-local legality

情報集合でnodeを共有しても、private stateによりaction availabilityが違い得る。

選択時:

```text
node.information_set_union_mask |= simulation.world_action_mask
availability_count += simulation.world_action_mask
candidate_mask = updated_node.information_set_union_mask
               & simulation.world_action_mask
```

このread-modify-selectは一つのnode lock critical sectionで行う。古いunionとのintersectionを
先に計算してはならない。そうすると後続worldで初めてavailableになったactionがunionへ入る前に
捨てられ、永久に探索されなくなる。

policyの扱い:

- NNから受けた48 actionの未mask base policyを保存する。
- parallel callbackはworld maskを見てillegal actionのpolicyを破棄してはならない。別worldで
  そのactionがavailableになった時にpriorを復元できなくなるためである。
- finite/non-negative等、既存policy契約を明示してvalidateする。
- candidate mask上でselection時に再正規化する。
- 新しいworldで初めて利用可能になったactionはunion/availability countへ反映する。
- unavailable actionへnode-globalな永久0 priorを確定しない。
- node訪問時、current worldでavailableなactionの`availability_count`を増やす。少なくとも
  forced playoutの機会数はglobal simulation IDではなくこのcountを基準にする。
- PUCT本体へavailability補正を入れるかは探索アルゴリズムの別ADRとし、初回のrace-safe化と
  同時にformulaを変えない。

apply失敗時:

- VLをrollbackする。
- statsを更新しない。
- `invalid_replay`を記録する。
- debug/CIでは即時failure。
- release throughputでもsilent drawにはしない。

## 9. `num_determinizations > 1` の再設計

初期parallel版では1に固定する。再導入時の既定設計は次のindependent world ticketsとし、
chance nodeへ変更する場合だけ新ADRを要求する。

### 推奨: independent world tickets

- **1 worldを1 logical simulation budget**として数え、worldごとに独立simulation IDと独立VL
  reservationを持つ。複数worldを一括生成する親IDはbatching/trace metadataに限り、statsや
  budgetの単位にしない。
- rootからworld固有path hash列を記録する。
- 公開outcomeが違えば別TreeKeyへ進む。
- leafごとに独立feature/mask/inference requestを作る。
- 同じleaf keyかつfeature digest一致のrequestだけdeduplicateする。
- parent edgeの期待値は各worldのbackprop結果として統計的に集約される。

### 不採用: 異なるleafをworld 0へ平均

- visible reveal outcome、mask、valueが異なるため使用しない。

### 将来検討: chance node

- reveal outcomeを明示chance edgeとして表現できるが、action space/tree APIを大きく変える。
- 初回parallel化と同時には行わない。

## 10. Root Dirichlet noise

- root bootstrapでvalid/public policyが確定した後、coordinatorが1回だけ生成する。
- 全workerへimmutable `array<float, 48>` を渡す。
- workerやselect呼出しごとに再生成しない。
- seedは`RootDirichlet` domainから導出する。
- traceにseedだけでなく実際のnoise digest、strict replayではnoise arrayを記録する。

`std::gamma_distribution`はstandard libraryをまたぐbitwise一致を保証しない。初期契約:

- same binary/ISAのdeterministic epochでは一致。
- cross-toolchain replayはtraceに保存したnoise arrayを再利用する。
- portable gamma samplerは必要性を測って別フェーズにする。

## 11. Deterministic epoch

RNGを固定しても、非同期commit順でPUCT結果は変わる。R2 modeでは次を固定する。

- worker数と無関係な固定epoch sizeとsimulation ID集合。
- coordinatorがsimulation ID順に実行するdeterminization、Game transition、world mask計算、
  leafまでのfull traversal、selection/reservation順。
- determinization seed manifest。
- inference batch membershipと入力順。
- result commit順。
- multi-world再導入時のworld順。
- policy/value reduction順。

当初案はleaf encode/evaluationだけを並列化する想定だったが、実装は次述のとおり
callback直列契約を優先した。このmodeはdebug/replay用で性能測定には使わない。

現行実装はPython callbackの同時実行数1契約を優先し、single coordinatorが
traversal、encode、同期callback、commitをすべてlogical ID順に実行する。したがって
`num_threads`は1/2/4/8等の結果互換性を検証する入力であり、実際の非同期NN
completion reorderはこのmodeでは検証しない。将来evaluationを並列化する場合は、NN側にも
deterministic実行を要求し、GPU kernelやbatch shapeの非決定性を別gateで扱う。

## 12. Replay trace

失敗をsingle-threadで再現できるよう、次を記録する。

```text
schema/rng/key version
compiler/build identifier
resolved seed / search nonce
root TreeKey / root public fingerprint
frozen config digest
simulation ID / world ID / domain seed manifest
selected TreeKey/action/player
reservation add/release ID
world action mask digest
leaf TreeKey / feature/mask digest
inference input/result digest
expansion owner/waiter/publish result
commit/cancel/error
final tree and ledger digest
```

現行schema v3は各commitの**変更node delta**だけをeventへ保存する。schema v2の
full-tree/event snapshotはsearch進行に対しO(n²)サイズになり得たため廃止した。ただし各
eventの`tree_digest`はcanonical TreeKey順の完全post-commit treeを認証し、headerからの
hash chainでpublication delta、path、seed、ledgerを連結する。

resource bound:

- initial+event deltaのaggregate snapshot数は131,072。
- 1 eventが最大`2 * MAX_DEPTH + 1`の変更snapshotを持つ保守的上限で事前計算し、
  現行`MAX_DEPTH`では安全な予算を約218 event以下とする。超過予算は最初の
  tree mutationより前に拒否する。
- binary parserはevent/path/snapshot件数の上限、`TreeDomain`、`ExpansionState`、
  `CompletionKind`、leaf roleのenum値、duplicate key、非有限統計を検査する。
- snapshot/deltaは`TreeKey`全fieldのcanonical順にsortし、backend/map iteration順を消す。

strict replayerくはinitial snapshotからpathのonline-mean `N/Q/total_visits`を独立適用し、
event deltaとchained full-tree digestを各commit後に照合する。publication、pending owner/waiter、
availability、reservationのすべての過渡状態を再実行する完全state-machine interpreterではない。

通常成功時はdigest中心、failure時だけ詳細eventをartifact化する。hidden card IDをtraceへ出す
場合はdebug artifactの取扱いを明示し、公開logへ漏らさない。

## 13. Test matrix

### 13.1 seed utility

- 10,000 logical tupleでseed重複がなく、manifest digestが固定値と一致する。
- field順、domain、observer、modeの違いでseedが変わる。
- 同じtupleはprocessをまたいで一致する。
- golden manifest digestが一致する。

### 13.2 portable shuffle

- fixed corpusのdeck/reserved digest。
- 同seedでbyte equality。
- 異なるsimulation/worldで十分な多様性。
- cardの重複・欠落なし。
- tierをまたいでhidden cardを移動しない。
- visible card、observerのknown reserve、公開fieldを変更しない。

### 13.3 information-set hash

- hidden IDだけの変更ではobserver TreeKey同一。
- slotごとのempty/tier vector変更ではTreeKey相違。hidden IDだけのslot交換でvector不変なら同一。
- determinization前後でroot observer key維持。
- featureが違うのにTreeKeyが同じfixtureを0件にする。
- observer 0/1、mode、domainを区別する。

### 13.4 parallel schedule

- 1/2/4/8/16 workerへの割当を変えてもsimulation IDごとのseed manifestが一致。
- normal/reverse/random completionでseed manifest一致。
- cancel挿入前後で未cancel simulationのworld一致。
- deterministic epochはcoarse/shardedと指定worker数でcanonical binary trace/tree digest一致。
- throughput modeはdigest一致を要求せずledger/invariantのみ確認。

### 13.5 world semantics

- revealでleaf public keyが違うworldを別nodeへ送る。
- world-local maskで不可能actionを選ばない。
- decode failureでdraw visitを増やさない。
- same leaf dedupはfeature digest一致を必須とする。world-local mask差は同じ`TreeKey`でも正常で、
  ticketごとのmaskとして保持・選択時に交差させる。
- root/history/hash/modeがsearch後も不変。

## 14. 実装順

1. 現行RNG消費とdeterminization corpusをcharacterizationする。
2. hidden reserve tierを含むobservable keyへ修正する。
3. observer/domain/versionを含むTreeKeyを導入する。
4. current-world mask選択へ移す。
5. divergent multi-world mergeを停止し、初期parallel版world=1にする。
6. `SearchRandomContext`とdomain-separated seedを導入する。
7. portable search RNG/Fisher–Yatesを追加する。
8. single-thread ticket pathを新seed APIへ移す。
9. worker数を変えたseed/world manifest testを追加する。
10. deterministic epochとtrace replayを追加する。
11. 必要なら「1 world = 1 simulation budget + 1 VL reservation」でmulti-world independent
    ticketsを再導入する。
12. 必要性が確認できた場合だけportable Dirichlet samplerを検討する。

## 15. 完了条件

- shared mutable RNGがparallel coreからなくなる。
- worker ID/thread scheduleがseed identityへ影響しない。
- exact/observable/observer/mode/key versionが混同されない。
- hidden tier featureとobservable keyの同値関係が一致する。
- 異なる公開leafを同じnodeへ平均しない。
- world-local legality違反が0。
- same-build deterministic epochが指定worker数/backendで一致する。ただし、非同期
  completion reorderの一致と同義ではない。
- seed/nonce/schema v3 traceからpath statisticsとchained publication deltaをsingle-threadで検証できる。
- portable shuffle goldenがGCC/ClangのCIで一致する。
