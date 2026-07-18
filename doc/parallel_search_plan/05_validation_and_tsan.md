# 05. 検証・TSAN・stress計画

> Race oracle、failure解析、CMake・CTest・CI・runner担当: **Codex Sol Ultra**
> 実装sessionはTSAN reportを「false positive」と判断して抑制してはならない。
> 原因判定とsuppression承認は別review sessionの **Codex Sol Ultra** が行う。

実装状況: native unit/stress/replay、Clang TSAN、Clang ASan+UBSan、Python integrationのcore
検証と、productionと同じpublic APIを使う決定的race fixtureは実装済みである。当初の
「汎用named fault hookを全数実装」は必須gateとせず、競合をpublic APIで直接構成する
計画へ修正した。全named hook実装済みとは主張しない。100種類のscheduler seed sweep、
長時間/全matrix soak、実NN/品質canaryはstable化までの残作業である。

## 1. 検証方針

TSANの無警告だけでは正しさを証明できない。次の4層を独立に持つ。

1. **論理oracle**
   - ticket、reservation、visit、VL、expansion state、root不変性をassertする。
2. **deterministic race harness**
   - productionと同じpublic APIを使い、barrier、ブロック可能fake callback、固定seedで
     same-leaf、pending close/publish/cancel、capacity、terminal/duplicate/stale競合を再現する。
     未被覆の線形化点が判明した場合にだけtest-only named hookを追加する。
3. **sanitizer**
   - TSANでdata race、ASan/UBSanでlifetime/UBを検出する。
4. **soak / replay / Python integration**
   - 多数schedule、cancel、GIL、callback ownership、性能を検証する。

検証の正本は、CPythonや外部NN runtimeに依存しないnative C++ executableにする。
Python testはGIL、snapshot、callback、ndarray ownershipを検証する。

## 2. 公式tool制約

Clang公式のThreadSanitizer文書では、compile/linkの両方で`-fsanitize=thread`を使い、
意味のあるstack traceのため`-g`、実用速度のため`-O1`以上を推奨している。また一般に
5〜15倍の実行時間、5〜10倍のmemory overheadがあり、全codeをinstrumentしない場合は
race見落としやfalse positiveの可能性がある。
[Clang ThreadSanitizer公式文書](https://clang.llvm.org/docs/ThreadSanitizer.html)

GCC公式文書では、`-fsanitize=thread`は`-fsanitize=address`および
`-fsanitize=leak`と同じbinaryへ組み合わせられない。
[GCC instrumentation options](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html)

CMakeではplatformごとのthread compile/link要件を直接`-pthread`決め打ちせず、
`find_package(Threads)`と`Threads::Threads`で扱う。
[CMake FindThreads公式文書](https://cmake.org/cmake/help/latest/module/FindThreads.html)

このため、次のbuildを分ける。

- normal Debug/Release
- TSAN RelWithDebInfo
- ASan + UBSan RelWithDebInfo
- Release benchmark

sanitizer buildの速度をperformance採用判断に使わない。

## 3. Build構成

### 3.1 CMake option

```cmake
option(CSPLENDOR_BUILD_PYTHON_MODULE "Build pybind module" ON)
option(CSPLENDOR_BUILD_NATIVE_TESTS "Build native C++ tests" OFF)
option(CSPLENDOR_BUILD_PARALLEL_BENCHMARK "Build parallel benchmark" OFF)
set(CSPLENDOR_SANITIZER "none" CACHE STRING
    "none, thread, or address-undefined")
```

native sanitizer jobではPython moduleを必須にせず、project header/sourceとtest harnessを
すべて同じsanitizer flagsでbuildする。

現行`csplendor_core`は`INTERFACE` targetなので、PS-5以降のparallel coreは当面header-onlyで
追加する。native testとPython moduleが同じheader定義・sanitizer optionを使うことをbuild
matrixで確認し、`.cpp`を前提にtarget構成を分岐しない。

### 3.2 CMake target

```text
csplendor_core (INTERFACE)
csplendor_sanitizer (INTERFACE)
mcts_parallel_unit
mcts_parallel_stress
mcts_parallel_scheduler
mcts_parallel_replay
benchmark_mcts_parallel
_csplendor (optional pybind module)
```

計画上の`mcts_parallel_faults`はまだ存在しないためCIから呼ばない。汎用fault harnessを追加する
場合は別targetとして導入し、CTest登録とtimeoutを同じ変更で行う。

threadを使うtargetは `Threads::Threads` へlinkする。

### 3.3 共通flags

TSAN:

```text
-O1
-g
-fno-omit-frame-pointer
-fsanitize=thread        # compile and link
-D_GLIBCXX_ASSERTIONS
```

ASan + UBSan:

```text
-O1
-g
-fno-omit-frame-pointer
-fsanitize=address,undefined   # compile and link
-D_GLIBCXX_ASSERTIONS
```

warningは通常buildで厳格に扱う。GCC公式がsanitizerと`-Werror`併用で未初期化警告等の
false positiveが増え得ると注意しているため、sanitizer jobだけcompiler warningの
`-Werror`化を分離する。

### 3.4 実行例

前提としてCMakeは`CSPLENDOR_BUILD_PYTHON_MODULE=OFF`時に
`find_package(pybind11 REQUIRED)`とPython module定義を通らないようconditional化する。これを
満たさない現行CMakeでは、pybind11未導入環境のnative-only commandはconfigureできない。
また`CMakeLists.txt`、`cmake/`、`.github/`の変更は現行AGENTSの列挙境界外なので、PS-2開始時に
maintainerのscope許可を得る。

TSAN:

```bash
cmake -S . -B build/tsan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCSPLENDOR_BUILD_PYTHON_MODULE=OFF \
  -DCSPLENDOR_BUILD_NATIVE_TESTS=ON \
  -DCSPLENDOR_SANITIZER=thread

cmake --build build/tsan --parallel

TSAN_OPTIONS="halt_on_error=1:history_size=7:print_full_thread_history=1" \
  ctest --test-dir build/tsan -L tsan --output-on-failure -j1
```

ASan + UBSan:

```bash
cmake -S . -B build/asan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCSPLENDOR_BUILD_PYTHON_MODULE=OFF \
  -DCSPLENDOR_BUILD_NATIVE_TESTS=ON \
  -DCSPLENDOR_SANITIZER=address-undefined

cmake --build build/asan --parallel

ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:strict_init_order=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
  ctest --test-dir build/asan --output-on-failure -j1
```

Clang版を必須gate、GCC版をnightly/secondaryにする。compiler runtime差を隠すsuppressionを
入れる前に、最小native reproducerを作る。

local sandboxでLeakSanitizerがptrace制約により起動できない場合だけ`detect_leaks=0`を許可し、
CI/専用runnerでは`detect_leaks=1`を必須にする。GCC TSANの`unexpected memory mapping`等、
toolchain/runtime起因の起動失敗はrace成功扱いにせず、Clang primary結果と別に記録する。

## 4. Native test support

現存するnative test:

```text
tests/mcts_parallel_unit.cpp
tests/mcts_parallel_stress.cpp
tests/mcts_parallel_scheduler.cpp
tests/mcts_parallel_replay.cpp
```

fixture、fake inference、ledger検査は現在それぞれのtestへ置かれている。将来
`mcts_parallel_test_support.h`または`mcts_parallel_faults.cpp`へ共通化する場合の対象は次である。

- fixed root fixtures:
  - initial
  - 5-action midgame
  - 250-legal-action state
  - hidden reserve state
  - reveal outcome state
  - near-terminal state
  - MAX_DEPTH cycle fixture
- deterministic fake inference。
- configurable latency/reverse/random completion inference。
- C++17 compatible test barrier/latch。
- trace/digest serializer。
- `SearchLedger` validator。
- fault hook controller。
- timeout付きjoin helper。

test codeからproduction synchronizationを迂回しない。productionと同じpublic/internal APIを
通し、hookは停止点を作るだけにする。

## 5. Search ledger

### 5.1 Counter

```text
issued
selected
root_bootstrap_evaluations
evaluation_owner
evaluation_waiter
evaluation_requested
evaluated_boards
completed_evaluated
completed_terminal
completed_max_depth
cancelled
failed
virtual_loss_added
virtual_loss_released
expansion_claimed
expansion_published
expansion_waited
ticket_committed
stale_result
duplicate_result
invalid_replay
```

先頭3つの`completed_*`はticketごとに排他的で、`completed`/`committed`はその和から導出する。
`evaluation_requested`はdeduplicate後のcallback request数、`evaluated_boards`はNN入力world数であり、
simulation outcomeへ重複加算しない。ticketは`Completed`に`CompletionKind`を保持する。
`root_bootstrap_evaluations`はsearch準備の診断値で、`issued`、simulation ID、completed budgetへ
加算しない。`expansion_published`はPending/node単位、`ticket_committed`はsimulation ticket単位で
あり、同一leafへwaiterがいる場合は一致しないのが正常である。

### 5.2 Quiescent invariant

正常終了:

```text
completed = completed_evaluated + completed_terminal + completed_max_depth
issued == completed
cancelled == 0
failed == 0
virtual_loss_added == virtual_loss_released
all edge virtual_loss == 0
all tickets == Completed
inflight == 0
worker_alive == 0
issued == requested_num_simulations
```

cancel終了:

```text
issued == completed + cancelled
failed == 0
virtual_loss_added == virtual_loss_released
all edge virtual_loss == 0
all tickets == Completed | Cancelled
```

error終了:

```text
issued == completed + cancelled + failed
virtual_loss_added == virtual_loss_released
all edge virtual_loss == 0
all tickets are terminal states
active_search == false
```

### 5.3 Tree invariant

quiescent pointで全nodeをsnapshotし、次を検証する。

- `total_visits == sum(N)`。
- Nとtotal visitsは非負かつ単調増加。
- internal N/total visitsは64-bitで、`UINT32_MAX`境界を越えてもwrapしない。legacy 32-bit DTOへ
  overflow変換しようとした場合は明示errorになる。
- availability、N、total visits、VL、live reservationは`UINT64_MAX`境界をmutation前に全件
  preflightし、overflow失敗時にunion/counter/snapshotが部分更新されない。
- Q、value、未mask base policyはfinite。
- base policyはworldごとのillegal actionを永久0化していない。
- current selection mask上の一時正規化policyが有限でsum 1。
- unavailable actionを選んだ記録がない。
- `availability_count[action]` は、そのactionが利用可能だったnode訪問数と一致する。
- owner worldではunavailable、後発worldで初めてavailableなactionも、union更新とcandidate作成を
  同一critical sectionで行うことで実際に選択できる。
- Expandedならpolicy/value/state publicationが完成している。
- TerminalとEvaluatingを同時に表さない。
- generationごとのpublish回数は最大1。
- 同じpendingへdeduplicateされたowner/waiterのfeature digestは一致する。world-local mask digestは
  異なってよく、unionとticket-local maskのintersectionで合法性を保つ。展開済みnodeへ後から
  到達したfeatureは現実装では再検査しないため、全`TreeKey` lifetimeの一致保証とは数えない。
- live reservation registry数とVLが一致する。
- stale resultによるN/Q/state変更がない。

node base policyはworld-local availabilityに対して選択時正規化するため、node全体で常に
sum 1を要求しない。

### 5.4 Root invariant

- root exact/public fingerprint不変。
- history、board_history、mode不変。
- root hash cacheをworkerが共有更新しない。
- observer/key domain不変。
- callbackがcaller側rootを変更してもsession snapshot不変。
- capacity到達時、root展開済み・visit 0ならmasked prior/noise fallbackがfinite、sum 1、illegal 0。
  root未展開なら`TreeCapacityReachedError`であり、全zero partialを返さない。
- root-parallelのworker reason集約は
  `Completed < Cancelled < TimedOut < TreeCapacityReached < CallbackError < WorkerError`を保つ。

## 6. Deterministic race fixtureとoptional fault hook

random `yield()`だけではraceを確実に再現できない。現行はproductionと同じpublic APIに
固定seed、callback barrier/待機、明示的cancel/capacity境界を与え、次を決定的fixture化した。

- same-leaf owner/waiterとworld-local mask差のattach/publish/drain。
- callback実行中のcooperative cancellation、timeout、exception後のMCTS再利用。
- pending terminal/duplicate/stale競合、root展開済み/未展開のcapacity境界。
- exact/observable、coarse/sharded、thread数を変えたcanonical deterministic trace。

以下のnamed hookは当初の候補カタログであり、現時点で汎用controller/専用CTestとして
全数実装してはいない。public API fixtureで表現できないinterleavingと検出された場合の
最小hookとしてだけ導入し、存在しないhook名をCIから呼ばない。

```text
AfterNodeLookup
BeforeSelectAndReserve
AfterSelectBeforeReturn
AfterVirtualLossAdded
BeforeExpansionClaim
AfterExpansionClaim
BeforeExpansionPublish
AfterExpansionPublish
BeforePendingClose
BeforeBackpropNode
AfterVirtualLossRelease
BeforeVisitPublish
BeforeQueuePush
AfterQueuePop
BeforeSessionDrain
BeforeActiveGuardRelease
```

必要になったhook controllerは特定thread数が到着するまでbarrierで停止し、競合を必ず作る。

規則:

- production buildではhook codeをcompile outする。
- hook内で対象node/tree lockを意図せず追加しない。
- hookがlock保持中かどうかをscenario名に明示する。
- test timeoutを必ず設定する。
- timeoutはhangとしてfailureにし、自動再実行で隠さない。

## 7. 必須race scenario

### 7.1 Tree/node

1. same hashへ2/8 thread同時insert。
2. map rehash直前のlookup/insert。
3. 同じroot edgeを同時select。
4. VL add直後に別threadがselect。
5. 同じedgeへ10万回commit。
6. 異なるedgeへのmixed update。
7. transpositionへ別pathから同時到達。
8. node snapshotとupdateの競合。
9. LRU/access epoch更新競合。
10. tree limit直前のnode作成。
11. availability/N/total/VL/live reservationを`UINT64_MAX`直前へ設定し、reserve/commitのoverflowが
    mutation前にatomicに拒否される。

### 7.2 Expansion

1. 同じUnexpanded nodeへ全worker同時到達。
2. owner claim直後にwaiter attach。
3. pending `Open -> Closing`とlate waiter attachの競合。
4. policy/mask/value publication直前にreader停止。
5. inference successとcancelの競合。
6. terminal判定とNN publishの競合。
7. owner exceptionで全waiter rollback。
8. stale generation result。
9. duplicate completion。
10. queueがpendingの参照を手放す瞬間とsession cancelの競合。registry strong ownershipにより
    nodeが永久`Evaluating`にならない。
11. owner ticketだけcancelしてもwaiterが残るpendingは継続し、全ticketをexactly once処理する。
12. Pendingを1回publishした後、owner/waiter ticketを1件ずつcommitし、途中failureでも
    未処理ticketだけabortしてVL/ledgerをexactly onceで閉じる。

### 7.3 Virtual loss / path

1. VL add後、action apply前にcancel。
2. 深いpathの中間でexception。
3. MAX_DEPTH 300。
4. malformed player/action/path。
5. double commit / double abort。
6. destructor fallback cleanup。
7. reverse-order completion。
8. forced playoutと複数in-flight reservation。

### 7.4 Lifecycle

1. active中clear。
2. active中prune。
3. active中config変更。
   detached `mcts.config` copyのfield変更はengineに影響せず、`set_config()`は即時拒否する。
4. 同一MCTSへの二重search。
5. 別MCTSへの同時search。
6. stop request直後のqueue push。
7. worker exception中のcoordinator wait。
8. inference callback exception。
9. timeoutと正常completionの競合。
10. session破棄とlate result。
11. capacity到達で新規発行停止、全未commit ticket cancel、VL=0。root展開済みは
    partial status、root未展開は`TreeCapacityReachedError`になる。
12. throughput active ticket registryのpeakが`max_inflight`以下、deterministic ticket配列のpeakが
    `deterministic_epoch_size`以下であり、全budgetに比例して増えない。
13. noexcept workerが例外を共有slotへ保存しqueue closeでcoordinatorをwakeして、join後に再送出する。
14. callbackが投げる`TreeCapacityReachedError`/`overflow_error`のtype/messageを保持し、内部capacity
    partialへ誤分類せず、cleanup後に同じMCTSを再利用できる。
15. root-parallelのserialized callback mutex待機中にtimeout/cancelし、lock取得後の再検査でstale
    backlogをcallbackへ流さない。

### 7.5 Determinization

1. hidden reserveを含むroot。
2. 10,000 tupleのseed manifestがworker割当1/2/4/8/16で一致し、衝突0。
3. reveal outcomeが17種類等に分岐するfixture。
4. world-local maskが異なるfixture。
5. observer 0/1のtree key分離。
6. non-determinized searchがdeterminization RNGを消費しない。
7. cancelが後続seedをずらさない。
8. 同一pending・同一featureでworld-local maskだけが異なるfixtureを正常として受理する。
9. 同一pendingでfeature digestが異なるfixtureはinvariant failureにする。
10. root bootstrap後も`num_simulations`件を全件発行し、bootstrapをbudget/IDへ数えない。
11. determinization on/off（Exact/Observable storage domain差）で`RootDirichlet` seed/noise array一致。
12. stable化前に展開済みnodeへ二次feature signatureを保存し、後続到達のdigest差もinvariant
    failureにするfixtureを追加する。

## 8. Deterministic replay

### 8.1 Strict trace mode

traceへ次を記録する。

```text
schema/config/root/key/rng version
simulation/world IDs and seeds
selection and reservation events
world mask and leaf digest
inference input/result digest
expansion owner/waiter/publish
commit/cancel order
tree digest after each commit
ledger digest
```

schema v3は各eventに変更node deltaだけを保存し、v2のfull-tree/event snapshotによる
O(n²)増大を廃止する。aggregate snapshotは131,072までで、1 event当たり
`2 * MAX_DEPTH + 1`の保守的上限を用い、現行は約218 eventを超える予算をtree
mutation前に拒否する。parserもevent/path/snapshot capとenum値を検証する。

snapshot/deltaは`TreeKey`全fieldでcanonical sortする。single-thread replayerはinitial snapshotへ
pathのonline-mean `N/Q/total_visits`を独立適用し、event deltaとchained post-commit tree
digestを各commit後に比較する。publication/pending/reservation/availabilityの全過渡を再実行する
完全state-machine replayではない。

### 8.2 Seeded schedule mode

- hook release順、queue completion順、delayをscheduler seedから生成する。
- 同一build/thread数/seedではevent digest完全一致。
- raceを発見したseedはregression fixtureへ昇格する。

### 8.3 Throughput mode

- OS schedulingに任せる。
- final tree/action digest一致を要求しない。
- ledger、tree、root、seed manifest、sanitizerだけをgateにする。

## 9. Test matrix

### 9.1 Stable化に必要なPR matrix

| 軸 | 値 |
|---|---|
| thread | 1, 2, 8, 16（16はover-subscribe/seed assignmentとstress用） |
| determinization | off, on |
| batch | 1, 16 |
| world | 1 |
| completion | inline, reverse, seeded-random |
| tree backend | coarse、shardedは導入後追加 |
| root | initial, fixed midgame, hidden reserve |
| failure | exception, cancel, duplicate/stale result |
| mode | serial oracle, deterministic epoch, throughput smoke |

組合せ爆発を避けるためpairwise matrixを作り、全軸直積はnightlyへ回す。

現在のPR CIはPython full suite、native 4 executable、Clang TSAN、Clang ASan+UBSanを実行する。
各native executable内で1/2/4/8 thread、coarse/sharded、determinization fixture等を検証するが、
上表の軸をCI matrix引数として全pairwise展開してはいない。seed utilityは10,000 tupleを
1/2/4/8/16 workerに割り当てたmanifest、replayはcoarse/shardedと指定worker数のcanonical
trace一致を検証する。deterministic modeはserial coordinatorのため、async completion reorder検証には
数えない。

### 9.2 Nightly/soak

現在のscheduled workflowは、引数を持たない既存4 native testをRelease buildで25回ずつ
`--repeat until-fail:25`実行する。約100 test実行によってOS scheduleの揺らぎを増やす現実的な
soakであり、test内部の固定seedを100種類へ変えるものではない。

stable化までに追加する拡張matrix:

```text
threads: 1,2,4,8,16
batch: 1,16,64
determinization: off,on
completion: FIFO,LIFO,100 scheduler seeds
simulations: 100,000 / scenario
total: 1,000,000〜10,000,000
tree: cold,warm,capacity boundary
failure injection: public APIの決定的scenario + 必要になった最小named hook
```

GitHub runnerのcore数が少なくても、8/16 software threadのoversubscribeはrace検出に使える。
ただしscaling性能には使わない。

### 9.3 Weekly/manual固定実機

- 1/2/4/8 physical core + SMT別series。
- Release benchmark。
- actual NN callback。
- 30回以上のA/B sample。
- self-play/fixed-time quality。

## 10. Python/GIL integration test

追加候補: `tests/test_parallel_mcts_bindings.py`

必須case:

- C++ search中にheartbeat Python threadが進む。
- inference callbackの最大同時実行数が1。
- callbackを呼ぶthread IDが一貫する。
- callback中に別Python threadがsame-MCTS `clear()`を呼ぶと即時例外。
- same-MCTS二重searchを即時例外。
- 別MCTS searchは同時進行する。
- rootをcallback closureからmutateしてもsnapshot searchに影響しない。
- callbackがrequest ndarrayを保持してもOWNDATA/lifetime安全。
- callback exceptionのtype/messageがcleanup後に再送出される。
- malformed count/shape/dtype/nonfinite resultはtree mutation前に拒否。
- exception後に同じMCTSで正常searchできる。
- Python `ParallelCancellationToken`をcallback内または別threadからrequestし、partial resultの
  `issued == completed + cancelled`、VL=0、後続search成功を確認する。
- `seed=None`はMCTS構築時のresolved seedと単調nonceを使い、resultの`rng_version`と
  `reset_replay_sequence(seed, nonce)`の再現性を確認する。
- root-parallel timeoutはfactory/bootstrapを含むend-to-end時間で観測し、実行中callbackは
  preemptしないsoft timeoutであることを明示する。
- Python root-parallelのserialized callback待ちはmutex取得後にtimeout/cancelを再検査し、1つの
  overrun後にstale callback backlogを流さない。
- callbackが`TreeCapacityReachedError`を投げても内部capacity partialへ変換せず、元のtype/messageを
  cleanup後に再送出して同じMCTSを再利用できる。

`tests/test_api.py`がsandboxのsocket wakeup `EPERM`を避けるためcalling-threadの
`httpx.ASGITransport`を使う変更は、Web API test harnessだけの環境対応である。MCTS callback、
thread scheduling、benchmark経路には入らず、並列探索の性能改善として扱わない。

sanitized Python extensionはnon-instrumented CPythonや外部libraryとの組合せで検出漏れ・noiseが
生じ得るため、初期必須oracleにしない。native TSAN合格後、専用integration jobとして追加する。

## 11. Suppression policy

`tests/tsan.supp`を追加する場合の規則:

- third-party runtimeの確認済み既知問題だけを対象にする。
- suppressionごとに理由、toolchain、upstream issue、期限をcommentする。
- `src/mcts*`、`src/game*`、`src/board*`、parallel test supportをmatchさせない。
- broad wildcardや`called_from_lib`でproject frameを隠さない。
- suppressionなしnative harnessを必須gateにする。
- suppression追加diffは別sessionの **Codex Sol Ultra** がreviewする。

`no_sanitize("thread")`やignorelistも原則使わない。Clang公式文書が説明するように、
instrumentation除外は同期の観測を失いfalse positive/false negativeの原因になり得る。

## 12. CI構成

### 現在のPR/push CI

1. existing Python 3.12 normal tests。
2. native C++ 4 executableのnormal build/test。
3. native Clang ASan+UBSan。
4. native Clang TSAN（`tsan` label）。
5. `mcts_parallel_replay`内のdeterministic trace/replay test。
6. full Python suite内のbinding concurrency test。

sanitizer jobは成功/失敗にかかわらずCTest log、compiler/CMake/OS/revision metadataをartifactへ
14日保存する。

### 現在のscheduled/manual nightly

1. Release native 4 testを25回ずつ、約100 test実行。
2. CTest logとcompiler/CMake/OS/revision metadataをartifactへ14日保存。

このjobは既存testだけを呼ぶ。public APIの決定的race fixtureはそのnative test内で実行するが、
汎用named hook controllerは存在しない。可変scheduler seed引数、GCC TSAN jobもまだ存在しない。

### Stable化までのnightly拡張

1. TSAN full pairwise matrix。
2. scheduler seed 100件。
3. cancel/exception/late-result soak。
4. capacity/rehash/MAX_DEPTH soak。
5. GCC TSAN secondary。
6. ASan+UBSan extended。

### Weekly/manual

1. fixed host scaling。
2. actual NN end-to-end。
3. fixed-time quality/self-play。
4. compiler upgrade validation。

将来の完全なTSAN failure artifact（現在はlogとbuild metadataまで実装済み）:

```text
full sanitizer log
seed / search nonce / scheduler seed
trace
compiler and runtime version
CPU/OS
config/root fingerprint
test binary hash
source revision or workspace diff digest
```

## 13. Optional adaptive scheduling perturbation

新しいClang TSANには同期点へdelayを注入するadaptive delay機能がある。ただしcompiler/runtime
version依存なので、supportを検出できるnightly jobでだけ使う。named barrier fixtureの代替には
しない。unsupported optionでjob自体を不安定にしない。

## 14. Exit criteria

次をすべて満たすまでparallel featureをstableにしない。

- PR/nightlyのnative TSAN 0 report。
- ASan/UBSan 0 report。
- hang/timeout 0。
- race suppressionなしのcore harness成功。
- public APIの決定的race fixtureで対象線形化点のledger/VL/tree invariant成功。
  全named hook実装は必須条件とせず、未被覆interleavingが見つかった場合に最小hookを追加する。
- deterministic epoch/replay一致。
- throughput soakでroot/hidden-info invariant成功。
- 展開済みnodeの二次feature signatureを後続到達でも照合し、同一`TreeKey`のfeature差を拒否する。
- Python callback exception後の再利用成功。
- stale/duplicate/malformed resultでtree mutation 0。
- compiler更新時にsanitizer matrix再確認。

sanitizer failureを「低頻度」「再実行で成功」として許容しない。raceは再現率ではなく存在が
問題である。現在は100 scheduler seed、拡張nightly、計測matrix、実NN/fixed-time品質が
未完了のため、複数threadはStage B experimental opt-inに留める。
