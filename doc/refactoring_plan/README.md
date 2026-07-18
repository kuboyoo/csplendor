# csplendor 段階的リファクタリング計画

## 0. この文書の位置づけ

この文書は、2026-07-12 時点の `csplendor` を読み取り調査した結果と、性能・ゲーム挙動・公開 Python API を維持したまま進めるための実装計画である。この段階では C++/Python の実装コードを変更しない。

調査基準は commit `83404dcb51c73959be915be0e43bbc20fe02db10` と、その時点の作業ツリーである。調査時には本計画と無関係な未コミット変更が存在したため、実装担当者は各 Phase の開始時に現行コードとの整合性を再確認すること。行番号や呼び出し関係を盲目的に前提にしてはならない。

### 絶対条件

- 公開 Python API の名前、引数、既定値、返却型、shape/dtype、例外契約を変更しない。
- ゲームルール、合法手の集合、合法手の順序、`Action.pack()` のコードを変更しない。
- `simple_payment_mode` と `blank_refill_mode` の現行契約を維持する。
- hot path に意図しないヒープ allocation、仮想呼び出し、共有所有権を増やさない。
- 変更前後で通常テスト全件と `performance` マーカーを実行する。
- 統計的な揺らぎを超える性能低下があれば、その変更を確定しない。
- 各コミット前に diff、テスト結果、性能結果を確認する。
- 指定された Phase だけを実装し、次の Phase へ自動的に進まない。

## 1. 結論

1. `Game` は公開 facade、妥当性検証、状態遷移、履歴、ターン進行、終局判定を抱え、`Board` は局面データに加えて初期化、表示、hash、determinization を抱えている。`PlayerState` も canonical state、derived cache、履歴的情報、ルール照会を混在させている。
2. `MoveGenerator` の主経路は固定長版だが、`base_actions` と private helper に vector 版が残り、take/reserve/purchase/payment の実装がほぼ二重化している。最終的には allocation-free の単一 emitter と複数 sink に統合する。
3. `PlayerState::gems`/`bonuses` が canonical で、`packed_gems`/`packed_bonuses`/`noble_eligibility_mask` は derived である。現状の `sync_packed()` は token だけの変更でも貴族12枚を再評価し、同期漏れと不要計算の両方が起こり得る。
4. 現 toolchain では `Board` は 392 bytes、`Game` は 448 bytes だが、`PlayerState` 内の vector により `Board` copy は最大4個の deep allocation を伴う。履歴付き `Game` copy は履歴長に比例する。
5. `MoveList` は 43,010 bytes である。`generate_all_fixed()` の返却用 list と中間 list が同時に生存する経路では、概ね 86--91 KiB の stack を使用する。`MAX_MOVES=2048` 超過時は合法手を無言で切り捨てる。
6. pybind11 境界では `legal_actions` の Action object 化、Board の nested list 化、StateEncoder の list 化、MCTS batch の list/tuple/NumPy 変換が主なコストである。長時間 solver は GIL を解放する一方、`prepare_batch_simulations` の native 計算は GIL を保持している。
7. `ActionEncoder`/`ActionEncoderCpp` は48枠の圧縮表現、V2 は4869枠の slot-based full 表現、V3 は3133枠の card/noble ID-based full 表現であり、役割が異なる。V2 は repo 内の実行経路では使われないが公開互換 API であり、今回の条件下では廃止しない。
8. MCTS と DFPN は互いに直接依存せず、`Game`、`Board`、合法手、hash、packed `Action` を共有境界にしている。ただし MCTS は V1/48枠へ直接依存し、reveal solver は通常ルールの一部を再実装している。
9. 現 `Board::hash()` は incremental XOR update ではなく「変更時 invalidate、次回全再計算」の lazy cache である。invalidate 漏れを型で防げないうえ、deck、points、final state などが hash 定義から欠落している。これは純リファクタとは別の既存 correctness 問題である。
10. 既存テストは token return と payment の網羅性が強い一方、MCTS、hash field coverage、vector/fixed 差分、copy/allocation/stack、pybind/GIL、統計的 A/B benchmark が不足している。最初に Phase 0 の安全網を作る必要がある。

## 2. 現在の依存関係

```text
Python API / FastAPI / scripts
  |
  +-- pybind11 bindings
  |     +-- Game / Board / PlayerState / Action
  |     +-- StateEncoder
  |     +-- ActionEncoderCpp / V2 / V3
  |     +-- MCTS / MCTSSearcher
  |     +-- VisibleOnlySolver / RevealVerifiedSolver
  |
  +-- Python DFPN / mate solver
        +-- Game.clone_light(), legal_actions/codes, apply*
        +-- Board の hidden information
        +-- Action.pack()/unpack()

MCTS
  +-- ActionEncoderCpp (48固定)
  +-- StateEncoder
  +-- Game
        +-- MoveGenerator
              +-- Board
                    +-- PlayerState
```

MCTS と DFPN の間に直接依存はない。両者へ影響する変更点は rule state、合法手、action code、state key であり、探索アルゴリズムの整理とルール本体の整理を同じコミットに混ぜてはならない。

## 3. 責務分析

### 3.1 `Game`

主な実装は `src/game.h:12-617` にある。

- `Board`、Action history、Board snapshot history、2つの mode を所有する。
- `legal_actions`、`legal_action_codes`、count/index/random apply という公開 facade を提供する。
- `can_apply_*` で入力 Action を検証する。
- take、reserve、purchase、noble、gem return を適用する。
- noble choice の待機、turn 進行、final round、tie-break を処理する。
- full clone と history-less clone を提供する。

問題は、合法手の生成条件が `MoveGenerator`、受理条件が `Game::can_apply_*` に別実装されている点である。この二重化は defense-in-depth として価値があるが、一致契約が未定義である。実際、生成済み TAKE action の無関係な `card_id` だけを変更すると `is_legal()` は false、`apply()` は true になり得る。generator と validator の統合前に「canonical Action」と「受理可能 Action」のどちらを契約にするか決める必要がある。

目標責務は、公開 facade、履歴、mode の所有に寄せる。純粋な rule query/transition は内部コンポーネントへ分離しても、`Game` の Python surface は維持する。

### 3.2 `Board`

主な実装は `src/board.h:63-390` にある。

- bank、visible cards、decks、nobles、players、turn/final/winner を保持する。
- seed からの初期化と reset を行う。
- debug 文字列を生成する。
- exact/observable と称する Zobrist hash を計算、cache する。
- hidden information を shuffle する。

全 field と `FixedStack` mutator が public であり、変更と hash invalidation を不可分にできない。`Board` は将来的に「局面データ」と「局面操作」を分離したいが、いきなり private 化すると bindings、solver、loader を横断するため、最初は内部 mutation helper と invariant checker から始める。

### 3.3 `PlayerState`

主な実装は `src/player.h:12-85` にある。

- canonical resource: `gems`, `bonuses`。
- derived resource: `packed_gems`, `packed_bonuses`, `noble_eligibility_mask`。
- rule state: points、reserved、hidden flag、各 count。
- provenance: purchased card IDs、acquired noble IDs。
- query: total gems、reserve 可否、affordability。

通常遷移では `reserved_count == occupied reserved slots`、`purchased_count == purchased_cards.size()` になるが、editor/loader が unknown purchased card を表現する場合は provenance が不完全でも count を保持する可能性がある。このため count を単純な derived field と決めつけず、loader 契約を先に調べる。

目標は canonical data と derived cache の境界を明示し、colored gems 更新と bonus 更新で別々の同期を行えるようにすること。既存 `sync_packed()` は互換 wrapper として残す。

### 3.4 `MoveGenerator`

主な実装は `src/move_generator.h:9-649` にある。

- state を所有しない合法手列挙器である。
- take、reserve、purchase payment、gem return、noble choice を列挙する。
- vector、fixed list、count の複数入口を持つ。
- payment と return の再帰列挙を行う。

目標は、列挙順を1か所で定義する allocation-free emitter と、`MoveList`、`std::vector`、count、nth、mask などの sink に分離することである。runtime polymorphism や `std::function` は hot path に入れず、template/inlined sink を第一候補にする。

## 4. 表現、copy、計算量の調査

### 4.1 vector 版と固定長版

| 項目 | 現状 | 判断 |
|---|---|---|
| `generate_all()` | `generate_all_fixed()` を呼び、結果を vector 化 | 互換 adapter として維持 |
| `generate_base()` | legacy vector helper を直接使用 | canonical emitter の vector sink へ移行 |
| take/reserve/purchase helper | vector版とfixed版がほぼ重複 | 同一 template emitter へ統合 |
| vector `expand_with_returns` | 現入口から未使用 | parity確認後に削除 |
| vector noble/return helper | 現入口から未使用 | parity確認後に削除 |
| `MoveList::push_back` | 2048件で silent truncate | Phase 0 で overflow を可視化し、上限根拠を作る |

単に「fixed版を唯一の関数にして、すべてを一度 `MoveList` に入れる」だけでは不十分である。`base_actions` に43 KiB stackと2048 capを持ち込み、count/mask/index のためにも全件 materialize するからである。共通化の単位は container ではなく action emitter とする。

合法手順序は `legal_action_code_at()`、`apply_legal_action_index()`、`apply_random_action(random % size)`、encoder の最初の match に観測される。集合だけでなく順序も golden 化する。

### 4.2 packed 表現の同期条件

| canonical field | derived field | 同期が必要な変更 | 現在の主な更新箇所 |
|---|---|---|---|
| `gems[0..4]` | `packed_gems` | colored token の増減 | take、return、purchase、Player setter |
| `bonuses[0..4]` | `packed_bonuses` | card bonus の増加、loader/setter | purchase、Player setter |
| `bonuses[0..4]` | `noble_eligibility_mask` | bonus の変更のみ | `sync_packed()` |
| `Card.cost` | `Card.packed_cost` | static card data の編集 | 手書き constexpr table |
| `Noble.requirement` | `packed_requirement` | static noble data の編集 | 手書き constexpr table |

Gold は packed resource の対象外である。Gold だけの変更で `packed_gems` を作り直す必要はない。現 `sync_packed()` は gems と bonuses を両方 pack し、さらに全 noble を評価するので、take と visible reserve で過剰計算がある。take は action 適用直後と return 後の二度同期する経路もある。

`packed_gems` と `packed_bonuses` 自体が Python から writeable であり、C++ field も public である。公開 surface を即座に削れないため、次の順で強化する。

1. static card/noble の array と packed value の一致 test。
2. Player の post-action invariant test。
3. colored gems と bonus/noble mask の同期関数を分割。
4. 内部 mutation site を helper 経由へ移行。
5. debug build で invariant assertion。release hot path には再計算を残さない。

### 4.3 `Game` / `Board` の copy cost

GCC 13.3 / x86_64 / C++17 での調査値は次のとおりである。ABI 固有値なので gate では同じ toolchain で再計測する。

| 型 | `sizeof` | inline 外の主な所有物 |
|---|---:|---|
| `Action` | 21 B | なし |
| `MoveList` | 43,010 B | なし |
| `PlayerState` | 104 B | 2 vectors |
| `Board` | 392 B | players経由で最大4 vectors |
| `Game` | 448 B | Action history、Board history、Board内vectors |
| `MCTSNode` | 832 B | なし |

主要 copy path は次のとおりである。

- `clone_light()`: Board を1回 deep copy。history はcopyしない。
- `clone()`: current Board、Action history、全 Board snapshots をcopyする。
- `apply_unchecked(record_history=true)`: `previous = board` の後、成功時に `push_back(previous)` してもう一度 deep copy する。
- `undo()`: history末尾 Board を current Board へ copy してから捨てる。
- `MCTSSearcher`: `Game game = root_game` の full copy があり、呼出側でも search game をcopyする。
- DFS solver: branchごとに Board snapshotを多用する。

低リスク候補は `push_back(std::move(previous))`、`board = std::move(board_history.back())`、明らかにhistory不要な内部copyの `clone_light()` 化である。Player vectors の固定長化や delta undo は copy byte数、object size、stack/cache locality の trade-off があるため、profile 後の別 Phase とする。

### 4.4 legal action の stack と計算量

`MoveList` は `Action[2048]` を内包するため 43,010 bytes である。各Actionにdefault member initializerがあるため、単にstack address spaceを確保するだけでなく未使用要素まで初期化/storeするcodegenになり得る。Release相当の `-fstack-usage` 調査では、次の peak が問題になる。

| 経路 | 現状の概算 peak | 原因 |
|---|---:|---|
| `generate_all_fixed` 呼出し | 約86 KiB | callerの結果 + calleeのbase actions |
| encoder mask | 約86--91 KiB | mask + legal result + base actions |
| `count_all_fixed` | 約43 KiB以上 | base actionsを全件保持 |
| return recursion | 1 frame 数百B、深さ最大約7 | value渡しのAction/array |

base action の上限要素は take-different 10、take-same 5、visible reserve 12、deck reserve 3、purchase source 15 である。purchase payment は5色・Gold最大5なら、制約を緩めた上限が1カードあたり弱組成 `C(10,5)=252`。return は6色・excess最大3なら1 base actionあたり `C(8,5)=56`。どちらも出力数に比例する再帰で、payment depth 5、return depth 6 である。

この緩い上限は2048を超える。`doc/action_space_v2.md` の「1000 random gamesで最大607」は有用な観測だが証明ではない。またGold最大5、prestate token最大10、excess最大3は公式reachable stateの前提であり、Python editor/loaderは供給保存則やこの上限を完全には強制しない。容量保証を「公式reachable state」に限定するか「公開editorが受理する全state」まで含めるかをPhase 0で決める。後者では固定2048で完全列挙できない可能性がある。合法状態 corpus、editor境界state、可能なら制約を使った上限証明を用意するまで、capacity 縮小や overflow 無視を行わない。

目標は次のとおりである。

- materialize が必要な `legal_actions` でも中間 `MoveList` をなくし、peak stackを概ね半減する。
- count、is_legal、nth/index、mask は action を consumer へ流し、原則として `MoveList` 全体を持たない。
- Python listを返す経路のheap allocationは「返却値に必要な意図された allocation」として計測し、それ以外を増やさない。
- overflow は少なくとも debug/test で検知し、release behaviorを変更する前に容量を証明する。

## 5. pybind11 境界

### 5.1 現在のcopy/変換

| 境界 | 現在の挙動 | 主なコスト・注意 |
|---|---|---|
| `Game.board` | Game内部Boardへの参照 | Game lifetimeに依存。Board自体のcopyはない |
| `Board.visible/decks/nobles` | C++ vectorを一時生成してPython list化 | 呼出しごとのcontainer/all element変換 |
| `Board.players/get_player` | `PlayerState` を値copy | Player内vectorsもdeep copyし得る |
| `Game.legal_actions` | fixed list→`std::vector<Action>`→Python list | 合法手数ぶんのPython Action object |
| `legal_action_codes` | `std::vector<uint64_t>`→Python int list | Action objectは避けるがlist/int allocationあり |
| `StateEncoder` | `std::array<float,196>`→vector→Python float list | 利用側のNumPy化でさらに変換 |
| native encoder mask | stack array→所有NumPy array | V1 48 B、V3約3.1 KiB、V2約4.9 KiBの安全なcopy。返却後lifetimeに必要 |
| MCTS batch | leafごとにdict/list/tuple/NumPy array | object数、path再構築、要素copyが多い |
| proof DAG | C++ graph→大量のdict/list/string | 大規模DAGでは変換が探索後の支配項になり得る |

`py::array_t(shape, local.data())` は、base objectを渡していない現実装では所有NumPy bufferへcopyされる。local stackへのdangling referenceにはなっていない。ここをzero-copy化するより、batchで多数の小arrayを作る構造を先に計測する。

Python callback用 `PyFeaturizer`/`PyActionEncoder` は、callbackごとにGILを取得し、Python callとNumPy→`std::array` copyを行う。現コードの `py::cast(game)`/`py::cast(action)` はconst lvalueを既定の `automatic_reference` policyでcastするため値copyとなり、Gameではhistory/board historyもcopy対象になる。pybind11 version差を含む実copy回数はPhase 0でcopy constructor counterにより確認する。callbackが受け取ったobjectを保持・変更する可能性があるため、単純なborrowed reference化はlifetimeと隔離の挙動を変え得る。

### 5.2 GIL

- visible/reveal solver は native探索中にGILを解放し、結果のPython object化前に再取得する。
- `mcts_search` はsearch中にGILを解放し、Python inference callbackで取得する。
- `prepare_batch_simulations` binding は、native計算を含めてGILを保持している。
- Game、encoder、StateEncoder の短いmethodはGILを保持している。

GIL release/acquire自体にもコストがあるので、短いmethodへ一律に `gil_scoped_release` を付けない。最初の候補は長い `prepare_batch_simulations` の純C++ scopeだけである。同じ `MCTS`/`Game` objectを複数threadから触ることは現状thread-safeではないため、GIL解放前にownership/threading contractをtestする。

### 5.3 境界最適化の方針

1. 返却型、shape、dtype、NumPy ownership、list順序を契約testにする。
2. native計算時間とPython変換時間を別々に計測する。
3. 既存APIの返却型を変えず、内部一時vectorのreserveや不要な再copyを局所改善する。shared backing/view化はNumPyのownershipとaliasingを変えるため、挙動維持Phaseへ入れない。
4. 新しいfast APIの追加は「公開APIを変更しない」の解釈確認が必要なので、自動的には行わない。
5. `ai_manager.py` のnative maskは一時の所有NumPy arrayでC++ stateを共有しないため、`np.array(...)` から `np.asarray(...)` への置換候補は比較的低リスクである。それでもconsumer側のdtype、contiguity、保持/変更をtestする。

## 6. Action Encoder の役割

| Encoder | action space | 識別する情報 | 現在の用途 | 互換性上の位置づけ |
|---|---:|---|---|---|
| Python `ActionEncoder` | 48 | base action。payment/returnは代表を選ぶ | legacy Python/ML | 公開API |
| `ActionEncoderCpp` | 48 | 上と同型。native mask/heuristicあり | C++ MCTS、API AI | MCTSの固定契約 |
| `ActionEncoderV2` | 4869 | return/paymentを区別。purchase/nobleはslot-based | repo内runtime利用なし | 公開された互換用full encoder |
| `ActionEncoderV3` | 3133 | return/paymentを区別。purchaseはcard ID、nobleはID | `ai_manager.py` の現行NN経路 | 現行推奨 |

V2とV3の先頭1085枠は概ね同じ設計だが、purchase/nobleの意味とIDは互換でない。V2 checkpointの4869出力headをV3の3133 headへ固定変換することはできない。slot→card ID変換には各局面が必要であり、学習済みweightの厳密変換はさらに定義できない。

### 6.1 V2廃止判断

現時点の判断は「残す」である。理由は次のとおり。

- `csplendor.__init__` と `__all__` から明示公開されている。
- READMEが互換用APIと明記している。
- tests/docs以外のrepo内runtime利用はないが、外部package、dataset、4869-head checkpointを否定できない。
- V2はstatic helperであり、呼び出さなければGame/MCTSのruntime性能やnode memoryへほぼ影響しない。
- V2削除は保守量、compile time、binary sizeには効くが、探索hot pathの改善には直結しない。
- 今回は公開Python API不変が絶対条件である。

従って今回行うのは、V2/V3共通のreturn codec、組合せ、slot helper、mask反復を内部共通化することまでとする。V2をV3のaliasにはしない。

将来の廃止には、少なくとも次のすべてが必要である。

1. 関連repoと配布物を横断し、`ActionEncoderV2` importと4869出力headを棚卸しする。
2. checkpoint/dataset metadataにencoder名、version、action sizeを必須化する。
3. stateとpacked Actionを持つdataset向けにV3再encode converterを用意する。
4. 互換releaseでdeprecationを告知し、移行期間を置く。
5. major/breaking変更として公開API削除の承認を得る。
6. V2 header/binding/tests/docs、`csplendor/__init__.py` のimport/`__all__`、README/README.en、4869を参照する分析scriptを同じbreaking releaseで更新・削除する。

### 6.2 encoderで見つかった既存問題

以下は構造リファクタと混ぜない。

- V2/V3の `find_take_diff_index()` は3色取得を必須とするが、bankに1--2色しかないとき `MoveGenerator` は全残色取得を合法生成する。その合法手はencode=-1となりmaskから欠落していた（最終横断レビューで既存3色slotへのsubset mappingを追加して解決）。
- レビュー時点のV2/V3 PASSは `ActionType` ではなくpolicy sentinelで、decode結果をGameへ適用できなかった。最終release監査で到達可能な合法手ゼロ局面を再現し、後述Correctness Aとして実rule actionへ修正した。
- V3のpublic payment helperと、それを呼ぶ `ActionEncoderV3.encode()` は、`gold_as[i] <= card.cost[i]` を完全にはvalidationしないため、不正Actionを有効IDへ写していた（同レビューでraw card cost超過をrejectして解決）。
- `IActionEncoder`/`PyActionEncoder` はgenericに見えるが、MCTSのmask/nodeは48固定である。V2/V3を渡すとmaskが切り詰められる。
- Python legacy encoderとC++ encoderには不正入力時のfallback差がある。

これらを偶然直すと観測挙動が変わる。後述の Correctness Track で1件ずつ承認・修正する。

## 7. MCTS、DFPN、ルール本体

### 7.1 MCTS

`src/mcts.h` は `ActionEncoderCpp`、`StateEncoder`、`Game` を直接includeし、tree statistics、PUCT、LRU、determinization、batch requestを同じheaderに持つ。`MAX_ACTIONS=48` で、V2/V3は使わない。

`src/mcts_searcher.h` には別のorchestration経路があり、Python featurizer/encoder callbackを使う。ここにはfull `Game` copyがある。`process_inference_results()` は実質未実装である。`create_mcts_searcher` は `MCTSSearcher` 型をbindせず返そうとするため、現行呼出しはreturn value変換の `TypeError` になる。外部 `dlsplendor` 側の意図・利用を確認せず、片方を削除・統合しない。

調査で次の既存疑義が見つかった。

- `prepare_batch_simulations()` のworld 0はpath適用済み `search_game` から始まるのに、同じpathを再適用しているように見える（Phase 0--7最終レビューで修正済み）。
- C++のbatch result適用はleafのvalid maskを使うが、binding側は `policy > 0` からcombined validを再構築する（同レビューで修正済み）。
- Game modeはBoard hashに含まれないため、modeの異なるGameでtree keyを共有し得る（`GameAdapter` keyへのmode saltで修正済み）。
- virtual lossはあるが、tree container自体はthread-safeではない。

これはalgorithm/correctnessの問題であり、header分割やcopy削減と同じcommitに入れない。

### 7.2 DFPNと検証solver

Python DFPNは `Game.clone_light()`、`legal_actions`/`legal_action_codes`、`Action.pack()`、hidden deck操作に依存する。worker serializationはBoard fieldを手作業で列挙しているため、state field追加時の更新漏れが危険である。

C++ `VisibleOnlySolver` は主に `Game.apply_action_code_trusted()` を使う。`RevealVerifiedSolver` は `Game` に依存しつつ、任意のreveal結果、oracle purchase/reserve、noble取得、turn進行、終局判定の一部を再実装している。これはhidden outcomeを扱うために必要な差分を含む一方、通常ルールとの重複もある。

目標は、通常 transition と hidden outcome 注入点を分けた内部adapterを作ることである。solver固有の枝順、memo key、unknown-card同値類はGameへ移さない。DFPNの探索、DAG変換、CLI分割はPython内の構造整理として独立させる。

## 8. Zobrist hash と invalidate

### 8.1 現在の契約と危険

現 `Board::hash()` は次のfieldを含む。

- bank
- visible cards
- board nobles
- player gems/bonuses/reserved card IDs
- current player
- waiting noble

一方、次のrule-relevant stateを含まない。

- deck内容、順序、size
- points、purchased count/card provenance、acquired nobles
- `reserved_is_hidden`
- turn、final round、winner
- Gameの `simple_payment_mode`、`blank_refill_mode`

実測でも、points変更、deck内2枚swap、final_round変更の後にinvalidate/recomputeしてもhash値が変わらない。MCTSは通常このhashをtree keyに直接使うため、将来遷移やterminal valueが異なる局面を同じnodeへ統合し得る。solverは独自 `StateKey` で一部を補うが完全に同じ契約ではない。

`observable_hash()` はdeck sizeを入れるが、`min(size, 12)` にclampするため13--40枚を区別できず、専用saltではなく `bank_gems[level][size]` を再利用している。同じlevelでbank数とclamp後deck sizeが同値なら同じ値を二度XORして相殺するため、決定的な構造衝突を作れる。observer identity、score/final state、Game modeもkeyへ十分に入っていない。

hash tableの値域も暗黙である。bank/player gemsが13以上、bonusが16以上なら、そのfieldはXORされず省略される。公式reachable stateで常に範囲内かの証明と、Python editorがuint8として受理する非canonical stateの契約を分ける必要がある。

invalidate自体は通常のGame apply、Board setter、randomizeで行われるが、public fieldと `FixedStack` を直接変更できる。solverのclear/eraseは「現hashがdeckを見ない」「直後のGame applyがinvalidateする」といった呼出順に依存している。hash定義を完全化した瞬間に潜在漏れが顕在化する。

さらに `cached_hash`/`hash_valid` はmutableで、同じBoardへ複数threadから `hash()` を呼ぶとdata raceになる。現時点では「1 Game/Boardを同時に複数threadからmutate/hashしない」契約が必要である。

### 8.2 分離すべき4種類のkey

- exact rule-transition hash: deck順序、points/count、final/winnerなど、将来の合法手・遷移・valueに影響するstateのkey。
- full-state fingerprint: turn、購入/貴族provenanceなど、serialization/full equalityに必要だが通常ルールの将来へ影響しない可能性があるfieldも含む。
- observable information hash: observerから区別不能なstateのkey。deckはsize、hidden cardはplaceholderを使い、専用saltを持つ。
- solver canonical key: threat equivalenceやvisible-onlyなど、solverが意図的に同値化するkey。

これらを1つの `Board::hash()` へ押し込めない。購入card IDや取得noble ID、turnをexact transitionへ過剰に入れるとtranspositionを減らすため、full fingerprintとの境界を先に決める。pure uncached計算を正本にし、cacheはその上の最適化とする。Game modeはBoard外なので、MCTS側のstate keyでmixする。

ただし `Board.hash()` はPythonに公開され、値変更はsignatureを保ってもbehavior変更である。exact/observable hashの完全化は、挙動維持Phaseには含めず、別承認のCorrectness Trackとする。それまでは現hash domainを変えず、invalidate siteとdebug oracleだけを強化する。

## 9. 既存テストとbenchmark

### 9.1 強い部分

- `tests/test_exchange.py` はfull bankの5,498 token分布と、1--2色だけ残るdepleted bankを広く検査する。
- `tests/test_payment.py` は選択局面でpayment patternを独立oracleと比較する。
- apply/undo、random playout、determinization、USI/KIFU round-tripがある。
- DFPNは逐次/並列、枝刈り、DAG、visible/reveal verifiedを広くtestする。
- proof DAGのreplay/validation testがある。

### 9.2 不足

| 分野 | 不足している確認 |
|---|---|
| public API | import/member/signature、返却型、dtype/shape、例外、ownershipのsnapshot |
| rule/generator | generated全Actionがapply可能、validatorとの差、集合と順序のgolden、simple/full、waiting noble |
| vector/fixed | canonical base emitterとの集合・順序 parity、dead helperの無参照確認 |
| overflow | `MoveList` saturation検知、最大合法手census、上限証明 |
| packed | 全card/noble static pair、毎action後のPlayer derived state、loader/editor state |
| hash | 全ActionType/全setter、cache前後、copy/undo、field sensitivity、observer equivalence |
| copy/memory | clone/full/light/history、Board snapshot、allocation count、RSS、object size |
| stack | `-fstack-usage` gate、小stack worker/thread、worst payment/return state |
| encoder | V1 Python/C++ parity、V2/V3全pattern/golden ID、depleted bank、invalid input、PASS |
| MCTS | PUCT、expand/backprop、terminal、LRU、virtual loss、batch、determinization、2経路のE2E |
| solver | Python/C++/pruning on/offの固定corpus差分、input不変、NPS/RSS |
| pybind/GIL | nested getter copy、callback ownership、NumPy lifetime、1/2/4 thread、Python heartbeat |

### 9.3 現性能testの限界

`tests/test_perf.py` のperformance testは次の4系統である。

- legal actions生成
- legal count/codes
- Python objectを介したrandom playout
- C++内部random playout

現在はwarm-up後5 roundの最良値を使い、絶対下限だけをassertする。smoke testとしては有用だが、best値は回帰を隠しやすく、変更前後の統計比較には使えない。`scripts/benchmark.py` も単発・seed非固定である。

## 10. 全Phase共通の実装前gate

各Phaseの担当者は、コードを変更する前に次を報告する。

1. 計画に記載したsymbol、呼出し元、public bindingが現在も存在するか。
2. 対象Phaseより前のPhaseが完了し、baseline artifactが取得済みか。
3. 作業ツリーに利用者の未コミット変更がないか。あれば対象fileとの重なりを確認する。
4. そのPhaseが既知のcorrectness問題を偶然変更しないか。
5. 合法手順序、packed Action code、encoder ID、Python返却型のどれが影響を受け得るか。
6. stack、heap allocation、copy回数、code sizeのどれが増え得るか。
7. 外部 `dlsplendor`、checkpoint、solver fixtureを確認しないと判断できない事項がないか。

次の場合は実装前に停止し、利用者へ矛盾を報告する。

- 現コードが計画の前提と異なる。
- public APIまたは合法手集合/順序を保てない。
- `MAX_MOVES` の安全性を示せないまま列挙方法・capacityを変える必要がある。
- hashの数値/同値関係、MCTSのpath/mask、PASSなど既知挙動の修正が必要になる。
- benchmark baselineを同条件で再現できない。
- 対象Phaseを越える変更が必要になる。

## 11. 検証と性能比較protocol

### 11.1 通常確認

```bash
pip install -e ".[dev]"
python -m pytest -o addopts= -m "not performance"
python -m pytest -o addopts= -m performance \
  --junitxml=/tmp/csplendor-performance.xml
PYTHONPYCACHEPREFIX=/tmp/csplendor-pycache \
  python -m py_compile csplendor/*.py csplendor/api/*.py scripts/*.py
```

build isolationやinstall先の権限でeditable installが失敗した場合は、その事実を報告する。依存が既に揃っている環境では `--no-build-isolation`、install不能だがbuild可能な環境では `python setup.py build_ext --inplace` を明示的なfallbackとして使い、最終報告に残す。

通常testとperformance testを分けるのは、`pyproject.toml` の既定 `-m not performance` を確実に上書きするためである。実装時は対象testだけで終わらず、最終的に全通常testを実行する。

### 11.2 A/B条件

- baselineは各candidate commitの親commitとする。
- baseline/candidateを別worktree、別venvまたは別wheel、別Release build directoryでbuildする。editable installを同じPython環境で切り替えない。
- compiler、flags、Python、pybind11、NumPy、CPU、core affinityを同一にし、正確なversion、`csplendor.__file__`、`_csplendor.__file__` をartifactへ記録する。
- fixed seed/state corpusを使い、CPU warm-up後にA/Bを交互に実行する。
- 1 sampleが短すぎないよう反復数を調整し、最低15 paired samplesを採る。
- raw sampleをJSONで保存し、best値だけを保存しない。
- 95% paired bootstrap confidence intervalでcandidate/baseline比を出す。
- throughputは比が1未満、latency/cycles/RSSは比が1超なら悪化として扱う。
- 95% CIが完全に悪化側ならcommitを確定しない。CIが境界を跨ぐ場合は30 sampleまたは長いsampleで再測定し、それでも不明ならcommitを確定せず結果を報告する。
- 既存の絶対速度thresholdはsmoke gateとして残す。
- allocation countの意図しない増加、`MoveList` overflow、合法手digest差は統計ではなく即時failとする。

Linuxで利用可能なら、noise確認を次のように補助する。

```bash
taskset -c 2 python -m pytest -o addopts= -m performance
taskset -c 2 perf stat -r 15 \
  -e cycles,instructions,branches,branch-misses,cache-misses \
  python -m pytest -o addopts= -m performance
```

### 11.3 固定workload corpus

- initial stateを複数seed。
- fixed seedのearly/mid/late game。
- return excess 0/1/2/3。
- bank残色 1/2/3/5、Gold 0/5。
- payment候補の多いvisible/reserved card局面。
- `waiting_noble` とfinal round。
- full/simple payment、normal/blank refill。
- history length 0/50/200のGame。
- hidden reserved cardと複数determinization。
- 固定SPNのvisible solver、reveal solver、DFPN。

測定operationは legal actions/codes/count、apply/apply_trusted/history on/off、clone/full/light/undo、cached/uncached hash、V1/V2/V3 mask/encode/decode、StateEncoder、MCTS batch/search、solver NPSとする。各結果には action countまたはdigest、nodes、allocation、peak RSSを併記し、速いが結果が違う状態を成功としない。

### 11.4 stack/allocation

- Release buildへ一時的に `-fstack-usage` を付け、`.su` をbaseline/candidate比較する。
- materializing legal actions、count、各encoder maskの最大frameとcall-chain peakを記録する。
- test用C++ executableでglobal `operator new` をcountし、fixed generator/count/maskのnative pathを測る。
- Python返却経路はPython object生成を意図したallocationとして別metricにする。
- 小stack workerをtestする場合はまず128 KiB程度を使い、platform依存のhard thresholdにはしない。

artifactは担当sessionだけの `/tmp` pathで終わらせず、CI artifact IDまたは引き継ぎ可能な保存先、schema version、baseline/candidate commitを最終報告へ記載する。大容量のraw dataをrepoへcommitしない。

## 12. 段階的実装計画

### Phase 0: 契約とbaselineの固定

推奨担当: `codex-terra / high`。MCTS/hash/Action契約のreviewは `codex-sol / ultra`。

実装コードを整理する前に、現挙動、公開surface、結果digest、性能分布を固定するPhaseである。既知の矛盾をgreen testへ無理に合わせず、明示的なknown issue/strict xfail/診断として分離する。

最初に Commit 0.0相当の無変更snapshotを採る。既存test/performanceを現commitのまま実行し、module path、toolchain、raw samples、合法手最大観測値を保存する。test instrumentationを入れた後の数値を「元実装baseline」と呼ばない。

#### Commit 0.1 `テスト: 公開APIとアクション順序を固定`

- 変更対象: 新規public API contract test、`tests/test_rules.py`、`tests/test_encoders.py`。
- 目的: imports、`__all__`、class members、default引数、返却型、shape/dtype/ownership、例外、合法手codeの集合と順序をfixed corpusでgolden化する。V2はtop-level import、公開static constants/methods、mask shape/dtype/ownershipを明示する。
- リスク: implementation detailを過剰にgolden化する可能性。hash数値や既知bugは通常goldenへ混ぜない。
- 確認test: API、rules、exchange、payment、encoders、USI/KIFU、全通常test。
- 性能測定: production code変更なし。test実行時間だけ記録。

#### Commit 0.2a `テスト: 合法手の集合・順序・適用契約を追加`

- 変更対象: generator property test、境界state fixtures。
- 目的: generated Actionが適用可能、code round-trip、重複なし、順序安定を確認し、後のvector/base emitter parity用oracleを作る。
- リスク: `is_legal`と`apply`の既存差、irrelevant Action fieldが顕在化する。
- 確認test: exchange全件、payment exhaustive、random playout、simple payment、noble、終局。
- 性能測定: stateごとの返却件数と生成時間。2048件ちょうどでもoverflowなしとは断定しない。

#### Commit 0.2b `診断: MoveList overflowのtest専用probeを追加`

- 変更対象: production bindingとは別にbuildするtest-only C++ executableまたはtest専用macroのuncapped counting sink。
- 目的: `MoveList` layout/hot pathを変えず、「2048件後もemitが試みられたか」を検知する。
- リスク: instrumented buildとRelease実装のcodegen差。Phase 2のcanonical uncapped emitter完成まではdefinitive proofにならない。
- 確認test: reachable rules state corpusと、公開editorが受理する非canonical stateを別集計する。容量保証をどちらのdomainに置くか実装前に決める。
- 性能測定: 最大attempted/retained actionsと該当state。instrumented速度をRelease baselineに使わない。

#### Commit 0.3a `テスト: packed表現の不変条件を追加`

- 変更対象: Player/Card/Noble invariant tests。
- 目的: static packed pair、post-action packed/mask、loader/editor stateを可視化する。
- リスク: standalone Playerのwriteable packed fieldやpartial provenanceを誤って禁止する。
- 確認test: 全ActionType、set_player、exchange/payment/noble、USI/SPN、全通常test。
- 性能測定: production code変更なし。test時間だけ記録。

#### Commit 0.3b `計測: Game・Board copy/allocation probeを追加`

- 変更対象: test-only C++ size/new counter、copy benchmark。
- 目的: `sizeof`、clone/full/light、snapshot/history、undoのcopy/allocationを計測する。
- リスク: benchmark compilerがextensionと異なる、global new counterの計測範囲誤り。
- 確認test: clone/undo full-state equality、probe schema、全通常test。
- 性能測定: history 0/50/200、allocation/op、bytes、RSS。

#### Commit 0.3c `テスト: 現hash cacheと既知欠落を診断`

- 変更対象: Board mutation matrix、cache/recompute tests、known-issue diagnostics。
- 目的: 現domain内のinvalidate/cache parityと、points/deck/final/observable salt等のdomain欠落を区別する。
- リスク: Zobrist数値をportable public constantとして固定する、既知bugをgreen仕様にする。
- 確認test: board、determinization、全setter/action、clone/undo、solver input不変、全通常test。
- 性能測定: hash cold/hot。数値digestは同一toolchainのA/B artifactだけに置く。

#### Commit 0.3d `Correctness: 完全局面/公開情報hashと実戦MCTSの分離`

- 変更対象: `Board::hash()`、`Board::observable_hash()`、Zobrist salt、MCTS設定、
  hash contract test。
- 目的: 完全局面hashにはdeck順序、得点/count、hidden flag、final/winner、turnを
  含める。公開情報hashにはobserverから見える得点/count、final/winner、turn、正確な
  deck枚数を含め、deck順序と相手のhidden card identityは含めない。
- MCTS: 実戦向けの既定をdeterminization有効とし、公開情報hashをtree keyに、
  shuffled hidden worldを評価に使う。完全情報の解析・棋譜検証は明示的に
  `use_determinization=False` を指定する。
- 確認test: exact field sensitivity、observable field sensitivity、hidden cardの
  observer同値性、clone/undo、MCTS default、通常testとperformance smoke。

#### Commit 0.4a `テスト: MCTSの固定contractを追加`

- 変更対象: 新規MCTS unit/E2E tests。
- 目的: PUCT、expand/backprop、terminal、LRU、batch、determinization不変条件を固定する。
- リスク: path二重再生、mask意味差、未binding型など既存疑義が顕在化する。完全digest一致はnoise/determinizationを無効化した経路だけに要求する。
- 確認test: ML、determinization、MCTS E2E。決定化はtest専用seed手段がなければ集合/shape/root不変を比較する。
- 性能測定: boards/s、tree size、RSS。既知不具合依存ならCorrectness Trackへ送る。

#### Commit 0.4b `テスト: C++ visible/reveal solver corpusを追加`

- 変更対象: visible/reveal solver fixturesとproof replay。
- 目的: 各solverのstatus/depth/action code/input不変をbaseline化し、同じ問題定義を持つ経路だけ差分比較する。
- リスク: visible-onlyとreveal-verifiedの異なる前提へ同一結果を要求する。
- 確認test: solverごとのgolden、意味が同じ直接Game遷移、proof DAG replay、全通常test。
- 性能測定: nodes/s、node数、memo hit、RSS。

#### Commit 0.4c `テスト: Python mate/DFPN差分corpusを追加`

- 変更対象: mate/DFPN fixtures、逐次/並列、枝刈りon/offの意味が同じpair。
- 目的: Python searchのstatus/depth/action code/input不変とserializationを固定する。
- リスク: solver固有のproblem definition、枝順、time/node limitを混同する。
- 確認test: 小さな決定的SPN、proof/line replay、worker serialization、全通常test。
- 性能測定: nodes/s、node数、memo hit、RSS。

#### Commit 0.5 `計測: paired A/B benchmark runnerを追加`

- 変更対象: `scripts/` のstdlib中心benchmark runner、performance testsのraw sample出力。
- 目的: fixed corpus、JSON、paired bootstrap CI、allocation/stack metadataを標準化する。
- リスク: CI時間、host noise、optional `perf` availability。
- 確認test: JSON schemaと統計計算の決定的unit test、既存performance smoke。
- 性能測定: baselineを最低15 samples採取。raw dataは引き継ぎ可能なCI artifact等へ保存し、artifact ID/schema/commitを報告する。一時 `/tmp` だけに残さず、大容量dataをrepoへ入れない。

#### Commit 0.6 `テスト: Python callbackのcopy・保持契約を固定`

- 変更対象: test-only copy counter、PyFeaturizer/PyActionEncoder callback fixture、対応するMCTS search test。
- 目的: callbackごとのGame/Action copy回数、history有無、callback内 `undo()`、保持、変更、例外時のroot隔離を実測して契約化する。対応floorのpybind11 2.10と現行versionの差も確認する。
- リスク: 未binding/未完成のMCTS経路が先に顕在化する。現行TypeErrorはknown issueとして分離する。
- 確認test: history 0/50/200、callback保持/変更/undo/例外、root full-state不変、全通常test。
- 性能測定: callback/s、Game copy/allocation、history長別latency。Phase 3.1とPhase 5.4の必須baselineにする。

Phase 0の終了条件は、対象となる後続commitの公開API/合法手digest、通常test、performance raw samples、必要なcopy契約、known issues一覧が揃うことである。未解決のMCTS/hash/encoder問題が、無関係なPhase 1.1--1.3まで一律にblockするわけではない。各commitが触る契約を固定できない場合だけ停止する。

### Phase 1: derived stateと低リスクcopyの整理

推奨担当: `codex-terra / high`。hash mutation reviewは `codex-sol / ultra`。

#### Commit 1.1 `整理: static resource packの正本を追加`

- 変更対象: `resource_bundle.h`、card/noble static data、対応test。
- 目的: constexpr pack helperまたはstatic assertionにより、arrayと手書きpacked値のdriftを検知する。
- リスク: constexpr/codegen、static initialization、card ID dataの誤変更。
- 確認test: 全90 cards、全12 nobles、affordability、payment、noble visit、全通常test。
- 性能測定: compile time/binary sizeとlegal generation。runtime codeが増えないこと。

実装済み: `ResourceBundle::pack()` をconstexprの正本とし、全90 cardと全12 nobleの
配列値と手書きpacked値を`static_assert`およびPython回帰テストで照合する。カード／
貴族データの値とruntimeの計算経路は変更しない。

#### Commit 1.2a `整理: PlayerStateの内部同期責務を分割`

- 変更対象: `src/player.h` とtrustedな `src/game.h` 内部call site。
- 目的: colored gems pack更新とbonus+noble mask更新を分け、`sync_packed()` は互換wrapperとして残す。まずGame内部の明白な二重同期だけを減らす。
- リスク: 同期漏れによりaffordability/貴族合法手が変わる。loaderがpartial provenanceを持つ可能性。
- 確認test: post-action invariant、全ActionType、exchange/payment/noble、全通常test。
- 性能測定: legal actions、apply history off、random playout、solver NPS、allocation不変。

公開 `PlayerState.gems=`、`bonuses=`、`Board.set_player()` は、他方の壊れたpacked値も現状修復するため、このcommitではfull `sync_packed()` を維持する。reveal solver内部のspecialized sync移行は、Game側の結果と差分testできる別Commit 1.2bとし、bindings/loaderは移行しない。

実装済み: `sync_packed_gems()` と
`sync_packed_bonuses_and_noble_eligibility()` に分離した。`Game` のtake/returnと
reserve visibleではgem packだけを更新し、公開setterと`Board.set_player()`は従来どおり
`sync_packed()` による全同期を行う。

#### Commit 1.2b `整理: reveal内部の同期をspecialize`

- 変更対象: `reveal_verified_solver.h` の直接resource mutation site。
- 目的: Gameと同じ内部sync helperを使い、不要な再計算を減らす。
- リスク: oracle/reveal固有遷移の同期漏れ。
- 確認test: 直接Game遷移とのpacked/mask/full-state差分、reveal solver/proof、全通常test。
- 性能測定: reveal solver nodes/s、legal generation、allocation。

実装済み: reveal solverのreserve/reveal branchはgem packのみを同期し、purchaseは
bonusと貴族eligibilityを含む全同期を維持する。通常`Game`遷移との整合性はsolver
corpusで回帰確認する。

#### Commit 1.3a `性能: Board snapshotのpush時deep copyを除去`

- 変更対象: `Game::apply_unchecked()` のhistory push。
- 目的: 成功時snapshotをmoveして、record_history=trueの不要なPlayer vector copyを1回除く。
- リスク: 失敗Actionでhistoryを壊す、moved-from objectを参照する、cached hash復元差。
- 確認test: apply失敗/成功、全ActionType、multiple history、clone/full/light、full state equality。
- 性能測定: history on apply、history length 0/50/200、allocation/op、RSS。history offを悪化させない。

実装済み: 成功した遷移の`previous` snapshotを`board_history`へmoveすることで、
履歴保存時の2回目の`Board` deep copyを除去した。失敗時はsnapshotを保存しない。

#### Commit 1.3b `性能: undo時の破棄予定snapshotをmove`

- 変更対象: `Game::undo()`。
- 目的: history末尾Boardをcurrent Boardへmoveし、直後に破棄するsourceのdeep copyを避ける。
- リスク: moved-from history、cached hash、vector ownership、multiple undo。
- 確認test: 1手/複数手undo、redo相当の再apply、hash/repr/full state、全通常test。
- 性能測定: undo latency/allocation、history length 1/50/200、RSS。

実装済み: `undo()` は履歴末尾の`Board`をcurrent boardへmoveしてから破棄する。
複数手undoで各snapshotのhashと表示が復元されることを回帰テストで確認する。

#### Commit 1.4a `整理: 現hash domainのpure計算helperを追加`

- 変更対象: `Board` のpure uncached hash helperとcache wrapperだけ。
- 目的: hashに含めるfieldは変えず、cache parityを検証可能にする。
- リスク: numeric hash drift、debug checkのrelease混入。
- 確認test: 同一toolchain A/B digest、全setter/action、randomize、clone/undo。domain外fieldは追加しない。
- 性能測定: cached/uncached hash、playout、MCTS/solver NPS。releaseのhot pathへ再計算を増やさない。

実装済み: `compute_hash_uncached()` を副作用のない正本として追加し、`hash()` は
lazy cacheのwrapperに限定した。`recompute_hash()` はuncached結果をcacheへ反映する
従来の観測可能な挙動を維持する。

#### Commit 1.4b `調査: hash mutation site inventoryを追加`

- 変更対象: test/documentationと明白な現domain invalidate漏れだけ。
- 目的: Game、bindings、visible/reveal solver、loaderの直接mutationを列挙し、新domainを有効化する前提を作る。
- リスク: subsystemを一括helper移行すると低リスクPhaseを越える。
- 確認test: 現domainのcache parity、known-issue diagnostics、全通常test。
- 性能測定: production変更がない場合は不要。mutation helperへの段階移行はCorrectness Dで行う。

実装済み監査: `Game::apply_unchecked()`、BoardのPython setter、hidden-information
randomize、reveal solverの独自遷移は、hash対象のmutation前後で無効化する。監査で
visible-only solverのdeck clear後に無効化が不足していたため修正した。`Board`のpublic
fieldと`FixedStack` mutator自体は引き続き直接操作できるため、型による強制は
Correctness Dで扱う。

Phase 1は各commitを独立評価する。1.2の同期削減と1.3のcopy削減を同じcommitにしない。

### Phase 2: `MoveGenerator` の単一emitter化

推奨担当: `codex-sol / ultra`。hot path、再帰、順序、stack、overflowを同時に扱うため、本計画で最も慎重な性能リファクタの一つである。

#### Commit 2.1a `整理: take生成をsinkへ統合`

- 変更対象: take-different/take-sameのvector/fixed helper。
- 目的: `push(Action)` 相当のcompile-time sinkを受ける単一実装へ統合する。
- リスク: template code bloat、inlining差、順序drift、base/fullの意味混同。
- 確認test: Phase 0 golden、base actions、fixed/vector parity、exchange、全通常test。
- 性能測定: take-heavy state、instructions/code size、stack、allocation。

#### Commit 2.1b `整理: reserve生成をsinkへ統合`

- 変更対象: visible/deck reserveのvector/fixed helper。
- 目的: reserve条件と列挙順を一つにする。
- リスク: reserve limit、Gold 0/5、return展開との境界。
- 確認test: reserve limit、Goldなし、hidden/visible、base/full順序、全通常test。
- 性能測定: reserve-heavy state、stack、allocation、code size。

#### Commit 2.1c `整理: purchase/payment生成をsinkへ統合`

- 変更対象: purchase source scan、payment recursionのvector/fixed helper。
- 目的: 最も大きい重複を単一compile-time sinkへ移す。
- リスク: payment順序、simple/full、effective cost、再帰codegen。
- 確認test: payment exhaustive、visible/reserved、simple/full、golden順序、全通常test。
- 性能測定: payment候補最大corpus、instructions、stack、allocation、binary size。

#### Commit 2.1d `整理: noble/dead vector helperを整理`

- 変更対象: noble choiceと、無参照を確認済みのvector return/noble helper。
- 目的: noble列挙をsink化し、dead codeを削除する。
- リスク: waiting noble順序、誤ったdead-code判定。
- 確認test: 0/1/複数eligible noble、`rg` call-site監査、全通常test。
- 性能測定: code size/compile time。runtime非悪化。

#### Commit 2.2a `性能: fixed生成のreturnを最終sinkへ直接出力`

- 変更対象: `generate_all_fixed()`、return recursion。
- 目的: 中間base `MoveList` をなくし、base actionから返却先fixed listへ直接流す。fixed APIのpeak stackを削減する。
- リスク: return順序、Action value-copy、再帰frame、2048 saturation位置が変わる。
- 確認test: return excess 0/1/2/3 exhaustive、合法手code順序、Action pack、simple/full、全通常test。
- 性能測定: `-fstack-usage`、instructions/cache/store、worst return/payment state、allocation。`MoveList` は43 KiB確保だけでなく2048個のAction default初期化でstack writeを生じ得るため、frame sizeだけで判断しない。

#### Commit 2.2b `性能: vector生成を直接sink化`

- 変更対象: `generate_all()` のfixed→vector中間変換。
- 目的: Python `legal_actions` 用vectorへ直接emitし、43 KiB中間fixed listを除く。
- リスク: 現在の2048 cap/silent truncationを外すと合法手集合が変わる。Correctness Trackで決めるまでは同じ位置・順序で2048件に制限する。
- 確認test: fixed/vectorの集合・順序・cap parity、Python Action list、全通常test。
- 性能測定: Python legal_actions、stack、heap allocation、instructions/cache。返却vector以外のallocationを増やさない。

#### Commit 2.3 `性能: legal countをcount sinkへ移行`

- 変更対象: `count_all_fixed()` とreturn count helper。
- 目的: countのための43 KiB `MoveList` を除き、同じemitterをcountだけする。Correctness判断までは現行どおり2048でsaturateする。
- リスク: saturation、`uint16_t` overflow、generatorとcountの条件差。
- 確認test: 全corpusで `count == len(codes) == len(actions)`、overflow診断、全通常test。
- 性能測定: count throughput/latency、stack、allocation。出力を作らないcountがbaselineより遅ければ採用しない。

#### Commit 2.4a `性能: legal action codesをconsumer化`

- 変更対象: `Game::legal_action_codes`。
- 目的: packed code vectorへ直接emitする。現行の2048件capと順序は維持する。
- リスク: reserve順序、capacity、vector reserve量。
- 確認test: Action listとのcode/order完全一致、全通常test。
- 性能測定: codes throughput、stack、allocation。

#### Commit 2.4b `性能: nth/index適用をconsumer化`

- 変更対象: `legal_action_code_at`、`apply_legal_action_index`。
- 目的: nth actionまでのearly-stopで全listを避ける。
- リスク: index順序、範囲外sentinel、early-stop cleanup。
- 確認test: 先頭/中央/末尾/範囲外の全index mapping、全通常test。
- 性能測定: code_at/index applyの位置別latency、stack、allocation。

#### Commit 2.4c `性能: random actionをconsumer化`

- 変更対象: `apply_random_action`。
- 目的: count+nth等で全listを避け、`random_value % count` のmappingを維持する。
- リスク: 2-passが遅い、count/list cap差、random sequence drift。
- 確認test: fixed random valuesと全選択index、playout digest、全通常test。
- 性能測定: C++ random playout、count+nth overhead、stack、allocation。

#### Commit 2.4d `性能: is_legalをconsumer化`

- 変更対象: `Game::is_legal`。
- 目的: match時early-stopで全listを避ける。
- リスク: noncanonical Action/irrelevant fieldの現行比較契約が変わる。
- 確認test: canonical/noncanonical hit/miss、全ActionType、全通常test。
- 性能測定: hit先頭/中央/末尾/miss、stack、allocation。

#### Commit 2.5a `性能: V1 encoder maskをconsumer化`

- 変更対象: `ActionEncoderCpp::get_action_mask` とscore variant。
- 目的: emitted Actionを48枠maskへ直接encodeする。
- リスク: first-match順序、PASS sentinel、depleted-bank既知問題、template instantiation増加。
- 確認test: V1 Python/C++ parity、golden mask/decode、全通常test。
- 性能測定: V1 mask/score、stack、binary size、allocation。

#### Commit 2.5b `性能: V2 encoder mask/matchをconsumer化`

- 変更対象: V2 maskと `decode_and_match`。
- 目的: V2の43 KiB legal listを避ける。
- リスク: PASS、slot ID、depleted-bank既知問題を偶然修正する。
- 確認test: V2全pattern/golden/injectivity、known issue非変更、全通常test。
- 性能測定: V2 mask/match、stack、code size、allocation。

#### Commit 2.5c `性能: V3 encoder mask/matchをconsumer化`

- 変更対象: V3 maskと `decode_and_match`。
- 目的: V3の43 KiB legal listを避ける。
- リスク: card/noble ID、invalid payment、depleted-bank既知問題。
- 確認test: V3全pattern/golden/injectivity、known issue非変更、全通常test。
- 性能測定: V3 mask/match、stack、code size、allocation。

Phase 2終了時にも `generate_all_fixed()` と既存Python APIは残す。内部emitterを新しい公開APIにしない。Phase 3へ自動的に進まない。

### Phase 3: search内部の`Game` copy最適化

推奨担当: `codex-sol / ultra`。callback-visible historyと探索経路を扱うため高難度である。

#### Commit 3.1 `性能: search内部のfull Game copyを除去`

- 変更対象: `src/mcts_searcher.h` のsearch/run simulation copy path。
- 前提: Commit 0.6のcallback-visible history/copy契約が確定していること。矛盾する外部consumerがあれば実施しない。
- 目的: history不要と確認できた内部局面で `clone_light()` を使い、履歴長に比例するcopyを除く。
- リスク: Python callbackが受け取ったGameで `undo()` を呼ぶなど、非典型だが観測可能なhistory依存。MCTS二重再生問題を同時修正しない。
- 確認test: root不変、callback ownership/history contract、noise/determinization無効時のrequest/action/value、MCTS E2E、全通常test。MCTS RNGは `random_device` 初期化なので、決定化経路へ同一seed digestを要求せず、test専用seedまたは構造的不変条件を使う。
- 性能測定: history 0/50/200、simulation/s、allocation、RSS。決定的経路だけsearch結果digestも比較する。

Playerの `acquired_nobles`/`purchased_cards` は現在 `def_readwrite std::vector` であり、Pythonから任意長listを受理する。固定容量でreject/truncateする案は公開API/例外契約を変えるのでPhase 3へ入れない。storage変更は依存分離後または十分なadapterがある場合のPhase 7へ延期し、unbounded heap fallbackを持つsmall-buffer以外はCorrectness/API承認を必要とする。

### Phase 4: Encoder内部の重複整理

推奨担当: `codex-terra / high`。public ID stability reviewは `codex-sol / high`。

Phase 2.5が完了している場合、mask反復や `decode_and_match` の形は調査時点から変わっている。Phase 4開始時に重複を再計測し、Commit 4.2が不要または範囲変更になっていれば、計画どおりという理由だけで実施しない。

#### Commit 4.1a `整理: V2/V3共通return codecを抽出`

- 変更対象: 新規internal header、`action_encoder_v2.h`、`action_encoder_v3.h`。
- 目的: `H` table、return offset、rank/unrankを一つのconstexpr実装にする。
- リスク: public static constant、action ID、reverse-lex順序、ODR/codegen。
- 確認test: 全return patternsのrank/unrank、golden IDs、V2/V3 mask/decode、全通常test。
- 性能測定: encode/decode/mask、compile time、binary size。ID digestは完全一致必須。

実装済み: `action_encoder_common.h` にreturnのrank/unrankとH/offset tableを
集約し、全84 patternのconstexpr round-tripを追加した。V2/V3の公開定数名とIDは
参照aliasで維持する。

#### Commit 4.1b `整理: V2/V3共通take combinationを抽出`

- 変更対象: TAKE_DIFF combination tableとindex helper。
- 目的: 同じbase mappingを一つにし、public IDを維持する。
- リスク: depleted-bank既知問題を偶然修正する、combination順序drift。
- 確認test: take golden IDs、depleted-bank known issue非変更、全通常test。
- 性能測定: take encode/decode、compile time、binary size。

実装済み: TAKE_DIFFの10組合せとindex探索を共通化し、全combinationのID回帰を追加した。

#### Commit 4.2a `整理: V2/V3共通slot helperを抽出`

- 変更対象: visible/reserved slot探索だけ。
- 目的: class固有のpurchase/noble mappingを残し、同一のslot探索だけを共有する。
- リスク: V2 slotとV3 IDの意味を誤って共通化する、first matchの順序drift。
- 確認test: same cardを別slotへ置いたときV2 IDは変化/V3 IDは不変、reserved purchase、全通常test。
- 性能測定: V2/V3 encode/decode、code size。virtual dispatch/heap allocationは禁止。

実装済み: visible/reservedのfirst-slot探索だけをinline helperへ抽出した。purchaseは
V2がslotでIDを変え、V3がcard IDで不変という差をテストで固定する。reserve visibleは
両encoderともslot-basedである。

#### Commit 4.2b `整理: V2/V3 decode_and_match反復を抽出`

- 前提: Phase 2.5後にも同一反復が残っていること。残っていなければ実施しない。
- 変更対象: `decode_and_match` のlegal action scanだけ。
- 目的: first matching legal Actionの選択を共有する。
- リスク: first match順序、PASS、encoder固有mapping。
- 確認test: V2/V3全golden、reserved/noble/PASS、全通常test。
- 性能測定: decode_and_match、stack、code size。

実装済み: 共通helperがfirst-matchの結果選択とfallback decodeを担い、各encoderは
Phase 2.5のprivate consumerをfriend accessできる走査関数だけを渡す。

#### Commit 4.3 `文書: encoder契約を現実装へ合わせる`

- 変更対象: README、`doc/action_space_v2.md`、`doc/action_space_v3.md`、`doc/ml_integration.md`、binding comments。
- 目的: 45/48、3124/3133、当時のPASS契約、MCTS=48限定、V2互換用途を正確にする。
- リスク: なし。ただし既知bugを仕様として正当化しない。
- 確認test: doc link、example import、全通常test。
- 性能測定: 不要。

実装済み: ML integrationの旧45-action説明を48-actionへ更新し、V2/V3のサイズ、
当時のPASS sentinel、MCTSが48-action固定であることを明記した。PASSは後の
release監査で実rule actionへ変更したが、48枠policy自体は維持している。

V2のdeprecation warning追加も観測挙動であるため、このPhaseでは行わない。

### Phase 5: pybind11境界の局所最適化

推奨担当: `codex-terra / ultra`。thread safety reviewは `codex-sol / ultra`。

#### Commit 5.1 `調査: MCTS batchのGIL解放可否を判定`

- 変更対象: test/TSAN用probeと、条件を満たす場合だけ `bindings.cpp` の `prepare_batch_simulations` wrapper。
- 目的: pure C++計算中に別Python threadを進められるか、現行のGILによる直列化を壊さず判断する。
- リスク: GIL解放は同じMCTS/Gameへの並行callをdata race/UBに変える。通常stress testだけでは安全性を証明できず、mutable hash cacheとtree mutationが特に危険である。
- 確認test: まず別objectのthread testとTSANを使う。同一objectは明示的なper-object排他を設計できた場合だけtestし、exception、Python heartbeat、result deep equality、全通常testを確認する。
- 性能測定: batch 1/16/64、world 1/8、single/multi thread throughput、GIL responsiveness。現行の同一object直列性を維持できないなら実装せず、Correctness/Concurrency Trackへ送る。single-thread有意悪化でも採用しない。

調査結果: native batch区間は技術的にはGIL解放可能だが、`MCTS`のtree/RNG/LRUと
`Board`のlazy hash cacheはいずれも無ロックである。全bindingをまたぐper-object排他と
root snapshotの設計なしに解放すると同一objectのdata raceとなるため、このPhaseでは
見送る。既存`mcts_search`のGIL解放経路も同じConcurrency Trackで監査する。

#### Commit 5.2a `性能: batch board/mask変換の一時allocationを削減`

- 変更対象: flat boards/validsのPython変換。
- 目的: 既存dict key、list、各array shape/dtype/`OWNDATA`/`.base`/mutation isolationを保ったまま、reserveや変換順で一時allocationを減らす。
- リスク: shared contiguous backing/viewはownershipとaliasingを変えるため、このcommitでは使用しない。array lifetime、writeability、consumerの保持/変更を確認する。
- 確認test: byte equality、flags/ownership、request保持後のlifetime、全通常test。
- 性能測定: array変換時間、Python object数、bytes copy、RSS、end-to-end boards/s。

実装済み: leaf数・world数でPython listを事前確保し、flat board/maskは一時ndarrayを
経由せず独立したowning ndarrayへ直接copyする。shape/dtype/OWNDATA/base/writeabilityと
各配列の独立性を回帰テストで固定する。

#### Commit 5.2b `性能: batch path変換の一時allocationを削減`

- 変更対象: leaf/terminal pathのlist/tuple生成と再構築。
- 目的: key、tuple内容、順序を保ったままreserveと変換重複を減らす。
- リスク: path index、player/action/hash順序、request lifetime。
- 確認test: path deep equality、terminal/leaf、apply_batch_results、全通常test。
- 性能測定: path変換時間、Python object数、allocation、RSS。

実装済み: leaf/terminal path listを事前確保し、`apply_batch_results`で復元する
`std::vector<PathEntry>`をpath長でreserveする。dict key、tuple内容、順序は維持する。

#### Commit 5.3 `性能: Board getterの一時vector allocationを削減`

- 変更対象: `Board.visible/decks/nobles/players` getterのreserve/構築方法。
- 目的: 公開list内容と値copy semanticsを変えず、一時vectorの再allocationだけを減らす。
- リスク: Player copy/lifetime、list順序、最適化が小さすぎてcode複雑度に見合わない。
- 確認test: 値/順序/独立性、setter round-trip、API、全通常test。
- 性能測定: getter別latency、C++/Python allocation、object count。改善が統計的に確認できなければ採用しない。

実装済み: `visible`、`decks`、`nobles`、`players` getterは直接Python listを構築し、
C++の中間vectorを除去した。`players`は明示的copy policyで値copy契約を保つ。

#### Commit 5.4 `条件付き性能: Python callbackの値copyを削減`

- 前提: Commit 0.6のcopy/保持/undo/隔離契約があり、値copyを減らしても同じ契約を満たす方法が証明されていること。
- 変更対象: binding policyまたはcallbackへ渡す軽量snapshot。安全と証明できる場合のみ実装。
- 目的: 現行のGame full-history値copyを、callback-visible挙動を変えず削減する。
- リスク: borrowed referenceの保持、mutation、dangling、search中のdata race。
- 確認test: callbackがobjectを保持/変更/undo/例外化するケース、root不変、全通常test。
- 性能測定: history 0/50/200でcallback/s、copy/allocation。安全性が証明できなければpolicyを変えない。

実装済み（限定）: leafの`get_action_mask`後はsimulation `Game`を使わないため、
`PyActionEncoder`だけへ最後のGameをmoveで移管する。featurizer/decode callbackは従来の
独立した値copyを維持し、保持・変更・undo・例外時のPhase 3契約を崩さない。

### Phase 6: 探索層とrule transitionの依存分離

推奨担当: C++横断部分は `codex-sol / ultra`、Python DFPNの純粋なfile分割は `codex-luna / high`。

#### Commit 6.1 `整理: MCTSの型・tree・orchestrationを内部分割`

- 変更対象: `mcts.h` の内部header分割。既存include/APIはfacadeで維持。
- 目的: node/tree statisticsをGame/StateEncoderから分離し、依存方向を明確にする。
- リスク: ODR、include順、pybind type registration、inlining/code size。
- 確認test: MCTS contract、bindings、ML、全通常test。
- 性能測定: `sizeof(MCTSNode)`、select/update、50k node RSS、compile time。layoutとNPSを悪化させない。

実装済み: 公開型を`mcts_types.h`、Game非依存のtree/statisticsを`mcts_tree.h`、
Game依存batch処理を`mcts_orchestration.h`へ分離し、`mcts.h`は既存APIを公開する
facadeとして維持する。MCTSNode layout、public signature、既知batch挙動は変更しない。

#### Commit 6.2 `整理: MCTS局面操作をinline adapterへ集約`

- 変更対象: clone、terminal、hash、decode/apply、feature/mask call site。
- 目的: MCTSを `Game` 内部layoutから隔離し、後続refactorの影響範囲を限定する。
- リスク: virtual callやheap ownership導入、determinization、V1 state-dependent decode、mode key。
- 確認test: MCTS全corpus、determinization、root不変、全ActionType、全通常test。
- 性能測定: batch/search NPS、allocation、stack、RSS。adapterは非仮想inlineを基本とする。

実装済み: `mcts_game_adapter.h`の非仮想inline境界へclone、determinization、
terminal/value、hash、feature/mask、decode/applyを集約する。Phase 3/5のcallback所有権と
world 0 path再生を含む既知挙動を維持する。

#### Commit 6.3a `整理: reveal solverのreserve transitionを共通化`

- 変更対象: reveal solverのvisible/deck reserve重複。hidden outcome注入点は残す。
- 目的: 通常reserve処理とoracle/reveal固有処理の境界を明確にする。
- リスク: 最も高い。deck操作順、return、noble wait、final round、proof edge、memo key、枝順を変え得る。
- 確認test: 直接Game遷移との全field差分、reveal全候補、proof DAG replay、Python/C++ solver corpus、全通常test。
- 性能測定: reveal solver nodes/s、node数、memo hit、Board copy/allocation、RSS。探索node数の変化も挙動差として報告する。

実装済み: `rule_transition.h`へreserve格納、Gold付与、gem returnを抽出する。
visible/deckの選択、remove/refill、hidden outcome列挙とproof edgeはsolver側に残す。

#### Commit 6.3b `整理: reveal solverのpurchase transitionを共通化`

- 変更対象: oracle/通常purchaseの共通部分。
- 目的: payment、bonus、points、refill前後を再利用し、oracle card選択はsolver側に残す。
- リスク: gold_as、reserved source、blank refill、packed/mask、proof edge。
- 確認test: payment全pattern、visible/reserved/oracle、full-state差分、proof replay、全通常test。
- 性能測定: purchase-heavy fixtureのnodes/s、node数、allocation、RSS。

実装済み: paymentとカード獲得をchecked/unchecked共通primitiveへ抽出し、通常Gameと
oracle purchaseで共有する。source removal、blank refill、oracle card選択は呼出側に残す。

#### Commit 6.3c `整理: reveal solverのnoble/end-turn transitionを共通化`

- 変更対象: noble取得、waiting state、current player、turn、final/winner/tie-break。
- 目的: 通常の手番進行をGameと共有する。
- リスク: 最終roundと勝敗を変え得る最重要commit。
- 確認test: 0/1/複数noble、final round全分岐、tie-break、proof replay、全通常test。
- 性能測定: final-round fixtureのnode数/NPS、allocation。結果完全一致を必須とする。

実装済み: noble取得、standard action完了、end-turn、final winner/tie-breakを共通化する。
全field corpus、solver node/proof digest、deck reserve全reveal edge replayで同値を固定する。

#### Commit 6.4a `整理: DFPNのDAG/KIFU変換を分離`

- 変更対象: `scripts/dfpn_mate_solver.py` のpure DAG/KIFU/serialization部分。旧import名は再exportする。
- 目的: search logicとpure output変換を分離する。
- リスク: import cycle、JSON key/order、monkeypatch target、worker pickle。
- 確認test: DFPN、USI/KIFU、generate puzzles、byte/semantic JSON equality、全通常test。
- 性能測定: solver node数/NPS/RSSは統計揺らぎ内、output digest一致。algorithmは変更しない。

実装済み: DAG codec、size集計、KIFU serialization/writerを`dfpn_output.py`へ分離し、
旧moduleは同じsignatureとmonkeypatch挙動を保つwrapperを再exportする。

#### Commit 6.4b `整理: DFPN CLIを分離`

- 変更対象: argument parser、entry point、出力/終了code。旧entry pointは維持する。
- 目的: CLI orchestrationをsearch moduleから分離する。
- リスク: default option、help、exit code、stdout/stderr、monkeypatch path。
- 確認test: CLI全option、既存import path、captured output、全通常test。
- 性能測定: search node数/NPS不変、CLI output digest一致。

実装済み: parserとCLI orchestrationを`dfpn_cli.py`へ分離する。旧module namespaceを
call-time注入し、旧`solve_*` monkeypatch、script/module entry、help、stdout/stderr、
exit codeを維持する。

### Phase 7: Player storage / delta undo（profileで必要な場合のみ）

推奨担当: `codex-sol / ultra`。

Board snapshot copyがsolver/MCTSの支配項とPhase 0/3で証明された場合だけ着手する。storage変更はbindings/serializer/solver依存が整理済みであることを前提にする。公開の任意長Python list受理を保つため、固定容量でreject/truncateするcontainerは使わない。

#### Commit 7.1a `実験: acquired noblesのunbounded small-bufferを比較`

- 変更対象: benchmark用prototypeのみ。現vectorとheap fallback付きsmall-bufferを比較する。
- 目的: 小vector allocation削減とinline size増加のtrade-offを測る。
- リスク: 任意長Python list、C++ caster、serialization順序。
- 確認test: 通常/任意長list round-trip、noble/SPN/proof、clone/undo。
- 性能測定: Board size、copy/allocation、solver/MCTS NPS、RSS/cache miss。利益がなければ実装しない。

比較完了・本番不採用: heap fallback付きinline 3 prototypeは任意長Python list、例外時の
旧値保持、clone/undo、SPN/proof順序、ASan/UBSanを維持した。貴族3件かつ履歴200の
人工上限局面ではMCTSが約35%改善した一方、`Board`は392から408 bytesへ増え、empty
局面のRSSは約3--4%増、empty履歴200のMCTSは約3%低下、fallback 16件のfull copyは
約20%低下した。reveal solverの改善も約1.2%に留まり、主要workloadの採用条件を満たさない。

#### Commit 7.1b `実験: purchased cardsのunbounded small-bufferを比較`

- 変更対象: benchmark用prototypeのみ。
- 目的: provenance vectorのcopy allocationとlarge inline storageの比較。
- リスク: unknown bought cards、任意長list、tie-break/count非一致。
- 確認test: 90-card partition、unknown bought loader、任意長round-trip、proof state。
- 性能測定: Board/Game size、clone、DFS/MCTS/solver、RSS/cache miss。利益がなければ実装しない。

比較完了・本番不採用: 851,698 player-stateでは購入数のp95/p99/maxが16/20/29だった。
inline 8/16/24/32で`Board`は408/424/440/456 bytesとなり、実局面copyはvectorの
23.96 nsから20.62/17.12/16.35/16.51 nsへ改善した。ただし既定相当の決定化MCTSは
おおむね±2%、reveal solverは±2.5%で一貫した改善がなく、empty RSSとspill時の
object+heap costも増える。90-card、unknown/count不一致、0--1000件の任意長、順序、
copy/move/clone/undo契約はprototypeで一致した。cache missは実行環境の権限制約で未測定。

#### Commit 7.2a `性能: acquired noblesへsmall-bufferを導入`

- 前提: 7.1aが主要workloadで有意に改善し、Pythonの任意長list/例外契約を完全維持すること。
- 変更対象: acquired field、bindings、serializer。
- 目的: acquired noble copy時のheap allocationを減らす。
- リスク: object layout、fallback allocation、copy/move semantics。
- 確認test: Phase 0 corpus、任意長setter、noble/SPN/proof、clone/undo、全通常test。
- 性能測定: 7.1aの全metric。悪化時は実装しない。

見送り: 7.1aでempty/fallbackのNPS・RSS悪化が確認され、主要solverの改善も有意でない。
24-byte layoutを保てる安全なcontainerが得られた場合だけ再評価する。

#### Commit 7.2b `性能: purchased cardsへsmall-bufferを導入`

- 前提: 7.1bが主要workloadで有意に改善し、Pythonの任意長list/例外契約を完全維持すること。
- 変更対象: purchased field、bindings、serializer、solver。
- 目的: purchased provenance copy時のheap allocationを減らす。
- リスク: object layout、fallback allocation、partial provenance、countとの非一致、copy/move semantics。
- 確認test: Phase 0 corpus、任意長setter、loader/editor、tie-break、solver proof、clone/undo、全通常test。
- 性能測定: 7.1bの全metric。悪化時は実装しない。

見送り: 7.1bではBoard copyと長履歴full cloneは改善したが、主要MCTS/solverで有意な
一貫改善がなく、常時object sizeを増やすtrade-offに見合わない。長履歴full cloneが
支配項になった場合はinline 16を再検討候補とする。

#### Commit 7.3 `準備: UndoRecordとdual-run検証を追加`

- 変更対象: internal undo type、debug verification。production pathはまだsnapshot。
- 目的: 復元すべきfieldとcapacityを明示する。
- リスク: instrumentation overhead、hash cache、hidden reveal/mode。
- 確認test: 全ActionType、失敗Action、multi undo、clone、solver、全通常test。
- 性能測定: debugは参考のみ。UndoRecord sizeと想定bytesを記録。

実装済み: internal `UndoRecord`と`CSPLENDOR_VERIFY_DELTA_UNDO` debug dual-runを追加した。
全6 ActionType、blank refill、return、自動/選択nobleを含む1,150遷移と71手multi undoで、
hash cacheを含むfull snapshotとの一致を確認した。recordは128 bytesで`Board` 392 bytesの
約33%、購入apply/undo試作では30,000反復のallocationが60,000回から1回になった。
production履歴は引き続きfull snapshotを使用する。

#### Commit 7.4a `性能: takeのdelta undoへ移行`

- 変更対象: take-different/take-same/returnのundo record。
- 目的: token actionのsnapshotをdelta化する。
- リスク: bank/player gems、packed/hash、turn。
- 確認test: snapshotとのstep-by-step equality、全return、multi undo、全通常test。
- 性能測定: take apply/undo、playout/solver NPS、allocation、RSS。

見送り（7.4a--d共通）: solver/MCTSの主要経路は`record_history=false`または
`clone_light()`であり、履歴delta化のend-to-end利益を受けない。また公開Boardは
apply後にも任意編集でき、現行`undo()`はその編集も含めてaction前のfull snapshotへ戻す。
差分recordへの切替はこの契約を維持できず、full fallbackを保持すればmemory利益を失うため、
7.4a--dのproduction移行条件を満たさない。

#### Commit 7.4b `性能: reserveのdelta undoへ移行`

- 変更対象: visible/deck reserve、refill、hidden flag、return。
- 目的: reserve branchのsnapshotをdelta化する。
- リスク: deck top/order、blank refill、Gold、reserved compaction。
- 確認test: 全reserve/reveal/undo、snapshot equality、proof solver、全通常test。
- 性能測定: reserve apply/undo、solver NPS、allocation、RSS。

見送り: 7.4a記載のprofile・公開editor/undo契約による。

#### Commit 7.4c `性能: purchaseのdelta undoへ移行`

- 変更対象: visible/reserved purchase、payment、refill、provenance/bonus/points。
- 目的: purchase branchのsnapshotをdelta化する。
- リスク: 最も大きいrecord、vector/small-buffer、packed/mask、deck。
- 確認test: payment exhaustive、全source、snapshot equality、proof solver、全通常test。
- 性能測定: purchase apply/undo、solver NPS、allocation、RSS。

見送り: 7.4a記載のprofile・公開editor/undo契約による。

#### Commit 7.4d `性能: noble/end-turnのdelta undoへ移行`

- 変更対象: noble、waiting、current player、turn、final/winner。
- 目的: 残るturn progressionのsnapshotをdelta化する。
- リスク: rule state復元漏れ、final/tie-break、hash。
- 確認test: snapshot実装とのstep-by-step full state equality、proof solver、全通常test。
- 性能測定: apply/undo、DFS/MCTS/solver NPS、allocation、RSS。

見送り: 7.4a記載のprofile・公開editor/undo契約による。copy-on-writeも導入しない。

profile上の利益が小さい場合はPhase 7を実施しない。copy-on-writeはaliasingとthreadingを複雑化するため既定案にしない。

### Phase 0--7 最終横断レビュー

Phase 7完了後にhash mutation、合法手/encoder、MCTS所有権・batch、solver transitionを
独立に再監査し、純粋なfile分割だけでは解消していなかったcorrectness問題を個別に修正した。

- hash/editor: `visible`/`nobles`/`decks` setterをvalidate-before-mutate化し、例外時の
  部分更新とstale cacheを解消した。noble saltをslot×ID化して順序・duplicateを区別し、
  canonical table外のuint8 editor値にもfield-tagged fallback saltを与えた。
- state key: `simple_payment_mode`と`blank_refill_mode`をMCTS keyへmixした。通常の
  false/false keyは従来値を維持する。
- action/encoder: depleted bankの1--2色takeをV2/V3の既存3色slotへ一意に写し、
  V3のcard cost超過paymentをrejectした。終局後のrule actionを空にし、purchaseへ
  適用不能なtoken returnを付けない。
- MCTS: world 0のpath二重適用、full-information追加worldの誤shuffle、binding側の
  valid mask無視、0 world/MAX_DEPTH時のvirtual loss漏れ、forced playoutのsimulation
  index欠落、負のvirtual lossと不正player添字を修正した。
- ownership/thread boundary: request ndarrayをowning copyにし、MCTS rootとnative solver
  inputをGIL解放前にsnapshot化した。公開request/result構造体もzero initializeする。
- transition: reveal deck-reserveも通常Gameと同じnoble/end-turn処理を通す。

確認は300 seed・874,466合法手のpack/apply/V2/V3 corpus、hash/editor境界、MCTS
world/path/mask/lifetime、solver full-state/digest、Phase 7 dual-runで行った。通常testは
API環境依存3件を除き324件、performance testは4件、ASan/UBSan probeは全件成功した。
API testはproject指定外の`httpx 0.28.1`環境で最小`TestClient`自体が停止するため、
対応範囲の回帰とは分離した。

この時点の既知事項は、同一MCTS objectの並行利用、public C++ fieldの直接mutation、
trusted APIへの不正入力、observer salt、未登録`create_mcts_searcher`と空実装
`process_inference_results()`、48枠固定interface、policy sentinelのPASSであった。
最終release監査では未登録factoryと空実装を削除し、PASSを実rule actionへ変更した。
いずれも外部consumerまたは並行実行/API契約を決めずに局所変更しない。

### Phase 0--7 最終性能再計測（2026-07-13）

追跡済みのリファクタ前source（`83404dc`）とPhase 0--7完了後sourceを別directoryで
Release buildし、Ryzen 9 7900X、GCC 13.3、Python 3.12.1、同一論理CPU上で比較した。
合法手数によってPython object化と旧固定長buffer初期化の比率が変わるため、
`tests/test_perf.py`と同じ合法手250件の局面と、固定費が見えやすい合法手5件の局面を
分けて測定した。

250件局面はseed 42、random playout 12手、best-of-5を7回測定した中央値である。

| 処理（合法手250件） | リファクタ前 | Phase 0--7 後 | 中央値比 |
|---|---:|---:|---:|
| Python `legal_actions` | 21,473/s | 26,586/s | 1.24倍 |
| C++ `legal_action_codes` | 61,313/s | 118,594/s | 1.93倍 |
| C++ `legal_action_count` | 316,991/s | 981,149/s | 3.10倍 |

別processでA/Bを交互にした30 pairのsustained測定では、250件局面の速度比は
`legal_actions`が約1.19倍（paired-ratio meanの95% CI 1.11--1.19倍）、codesが
約1.97倍（1.87--2.00倍）、countが約3.11倍（3.05--3.35倍）だった。絶対値は
実行quotaの影響を受けたため、READMEの代表絶対値には上の単独best-of-5を用いる。

5件局面はseed 42から合法codeで固定的に12手進め、50,000 iteration×30標本を
同一coreで測定した。A/B交互測定でも同じ傾向を確認した。

| 処理（合法手5件） | リファクタ前 | Phase 0--7 後 | 中央値比 | paired速度比の95% CI |
|---|---:|---:|---:|---:|
| Python `legal_actions` | 190,126/s | 963,989/s | 5.07倍 | 5.05--5.31倍 |
| C++ `legal_action_codes` | 213,097/s | 1,953,375/s | 9.17倍 | 8.78--9.87倍 |
| C++ `legal_action_count` | 413,000/s | 3,973,442/s | 9.62倍 | 9.18--10.54倍 |

C++内部random playoutは別の固定10-game×30標本のworkloadで160,545から740,538 moves/s、
中央値比4.61倍、paired速度比の95% CIは4.56--4.63倍だった。

READMEの旧掲載値は20,000 `legal_actions`/s、330,000 count/s、160,000
moves/sであり、同じ250件局面の新しい代表値との単純比はそれぞれ約1.33倍、2.97倍、
4.63倍になる。リファクタリングによる改善幅は同一条件で再buildした上表のA/B比を
正とする。特に5.07倍は低分岐局面の値であり、公開`legal_actions`全般を一律5倍と
表現しない。

MCTSは実NNの推論時間とbatch sizeでend-to-end比が変わるため、engine-onlyの2種類を
分離して測定した。`scripts/benchmark_mcts_native.cpp` は同じ中盤局面、batch 16、
1 worldで、leaf準備は2,000 batch×15標本、synthetic searchはfake inferenceを使う
256 simulation/search×20 search×15標本の中央値である。

| native MCTS処理 | 決定化 | リファクタ前 | Phase 0--7 後 | 高速化 |
|---|:---:|---:|---:|---:|
| 未展開leaf準備 | off | 220,318 simulations/s | 3,136,270 simulations/s | 14.24倍 |
| 未展開leaf準備 | on | 176,102 simulations/s | 709,461 simulations/s | 4.03倍 |
| 256-sim synthetic search | off | 70,690 simulations/s | 94,427 simulations/s | 1.34倍 |
| 256-sim synthetic search | on | 65,001 simulations/s | 108,441 simulations/s | 1.67倍 |

未展開leaf準備はPhase 2のV1 mask consumer化の効果を強く観測するmicrobenchmarkであり、
14.24倍を実NN込みMCTS全体の倍率とは解釈しない。synthetic searchもNN時間を含まず、
最終reviewのworld replay/hash等のcorrectness修正を含む現行処理量の比較である。

別の1 simulation/tree-clear C++ microbenchmarkでは、履歴0は決定化off/onで
0.93/1.04倍とほぼ同速だった。履歴50は1.92/4.50倍、履歴200は2.01/14.71倍で、
履歴が長い決定化ほど`clone_light()`による履歴deep-copy除去の恩恵が大きい。
これはcopy overheadの上限に近い測定であり、実モデル込み探索の14.71倍高速化を
意味しない。Phase 6のtree選択単体は約1.02倍で実質同速、Phase 7 prototypeは本番不採用
なのでproduction速度へ寄与しない。

再現用の現行側コマンドは次のとおりである。A/Bでは同じsourceをリファクタ前の
`src/`に対してもcompileし、`scripts/benchmark_phase0.py`は各buildのPython packageを
明示的にimportする。

```bash
taskset -c 2 python -m pytest -o addopts= -m performance tests/test_perf.py \
  --junitxml=/tmp/csplendor-performance.xml
taskset -c 2 python scripts/benchmark_phase0.py \
  --label current --samples 30 --iterations 50000 --output /tmp/current.json
c++ -std=c++17 -O3 -DNDEBUG -Isrc scripts/benchmark_mcts_native.cpp \
  -o /tmp/csplendor-mcts-benchmark
taskset -c 2 /tmp/csplendor-mcts-benchmark 15 2000 20
```

## 13. 挙動変更を伴うCorrectness Track

以下は重要だが、「性能と挙動を維持したリファクタ」のcommitへ混ぜない。個別承認、個別test、個別performance比較を行う。推奨担当はいずれも `codex-sol / ultra` である。

### Correctness A: Action/Encoder契約

1. depleted bankの1--2色 TAKE_DIFFERENTをV2/V3でどうID化するか（解決: subsetを含む最初の既存3色slotへ写す）。
2. PASSをpolicy sentinelとして文書化するか、本当のrule actionにするか（解決:
   seed 10の到達可能局面で手番側だけ通常手ゼロ、相手側は購入可能となる停止を再現。
   `ActionType.PASS`を実rule actionとして追加し、双方通常手ゼロはdraw。48枠NN mappingは
   維持し、rootでは明示適用、探索内部では自動遷移する）。
3. V3 public payment helperと `ActionEncoderV3.encode()` のinvalid paymentをrejectするか（解決: card raw cost超過を`-1`にする）。
4. MCTSへ48以外のencoderを渡した場合にfail-fastするか、interfaceをsize-awareにするか。
5. generator、`is_legal`、`apply` がirrelevant Action fieldをどう扱うか（解決:
   generatorはcanonical Actionを返し、`is_legal`と`apply`は同じsemantic validatorを使う）。

各項目を1commitに分ける。合法手集合を変えずencoder maskだけを修正する場合でも、NN policy mappingと外部checkpointへの影響を報告する。

### Correctness B: exact/observable/state key

最終レビュー反映: item 5の公開editor境界、item 6のGame mode key、noble slot順序は
対応済み。observer salt、public C++ mutationの強制、hash migration全体は未完了である。

1. exact rule-transition、full-state fingerprint、observable、solver canonicalのfield contractを先に決める。
2. 新domain対象fieldの全mutation siteを列挙し、Correctness Dのinvalidate対応を先に完了する。
3. exact transition hashへdeck order/size、score/count、hidden flag、final/winnerなど将来rule-relevant fieldを追加する。provenance/turnはfull fingerprintとの境界を決めてから扱う。
4. observable hashへ公開score/final state、observer salt、専用deck-size/hidden-card saltを追加する。deck size saltはlevel別に0..`MAX_DECK_SIZE`を表し、12 clampを残さない。
5. gem/bonus/bank saltのtable rangeと、公式reachable state・Python editor受理stateのどちらをhash契約に含めるか決める。
6. MCTS state keyへ `simple_payment_mode`/`blank_refill_mode` をmixする。
7. solver canonical keyはexact/observableから独立させる。
8. public `Board.hash()` の値変更方針とmigration noteを決める。

hashは64-bitなので偶然衝突を「なくす」とは表現しない。field sensitivity、同一state安定性、observable同値性、copy/undo一致をtestする。hash値変更後はMCTS/solverのnode数と結果も再検証する。

### Correctness C: MCTS batch経路

1. world 0のpath再生起点を統一する（解決: path適用済みleafをそのままencodeする）。
2. bindingのcombined validをrequestのvalid maskと一致させる（解決）。
3. 未実装 `process_inference_results()` と未登録/未使用 `create_mcts_searcher` の扱いを決める。
4. 2つのMCTS orchestration経路の正本を、外部consumer調査後に決める。

これらはsearch result、node count、policyを変え得る。Phase 6のfile分割と同じcommitで直さない。

### Correctness D: hash invalidationを型で強制

exact hashのdomain決定後、Board mutation API/editorを導入し、Game、bindings、visible/reveal solver、loaderをsubsystem単位で移す。public C++ fieldのprivate化は最後に行い、Python APIはpropertyで維持する。deck clear/eraseのような「今はhash対象外だから安全」なsiteを先に列挙する。

安全な順序は「B: domain決定 → D: 新対象fieldの全mutation site inventory/invalidate保証 → B: 新hash domain有効化 → D: 段階的private化」である。deck等をhashへ追加してからinvalidate漏れを直す順にはしない。

### Correctness E: 並行実行契約

MCTS/Game/Boardは現状thread-safeではなく、Pythonからの同一object callはGILにより事実上直列化される。GIL解放を広げる場合は、per-object lock、object confinement、mutable hash cacheの同期、tree mutationの排他を先に定義する。現行の直列性を変える案は性能リファクタとして黙って導入せず、別承認とTSANを必須にする。

最終レビューではnative solver inputと`mcts_search` rootだけをGIL解放前snapshotへ隔離した。
同一MCTS treeを複数threadから更新するケースは引き続き非対応である。

### Integration F: 外部AI consumerのcopy削減

`ai_manager.py` の `np.array(native_owned_mask)` を `np.asarray` にする候補はC++ stateとの共有を生じず比較的低リスクだが、外部 `dlsplendor` とmodel/predictorのdtype/contiguity契約をこのrepo単独で再現できない可能性がある。Phase 5へ混ぜず、連携環境とmodel fixtureが利用可能なときにAI action/policy一致を確認して独立commitにする。

## 14. Phaseと推奨担当の一覧

| Phase | 内容 | 推奨担当 | 難易度 | 次へ進む条件 |
|---|---|---|---|---|
| 0 | 契約test、corpus、benchmark | codex-terra / high | 中 | baselineとknown issue確定 |
| 1 | packed同期、低リスクcopy、現hash audit | codex-terra / high | 中 | 全invariant、性能非悪化 |
| 2 | MoveGenerator emitter/consumer | codex-sol / ultra | 高 | 集合・順序・stack・性能合格 |
| 3 | search内部Game copy | codex-sol / ultra | 高 | callback契約と主要NPS非悪化 |
| 4 | Encoder共通化、V2維持 | codex-terra / high | 中 | 全ID digest一致 |
| 5 | pybind/GIL/変換 | codex-terra / ultra | 高 | lifetime/thread testとA/B合格 |
| 6a | MCTS/rule依存分離 | codex-sol / ultra | 最高 | search digest/NPS合格 |
| 6b | DFPN pure file分割 | codex-luna / high | 中 | output/search一致 |
| 7 | Player storage/delta undo、必要時のみ | codex-sol / ultra | 最高 | profileで支配項と証明済み |
| Correctness | encoder/hash/MCTS既存問題 | codex-sol / ultra | 最高 | 個別の挙動変更承認 |

担当名は目安であり、モデル名よりもPhase境界とreasoning levelを優先する。複数担当を並行させる場合も、同じhot headerやgolden contractを同時に編集させない。

## 15. 実装依頼テンプレート

```text
csplendor の段階的リファクタリング計画
doc/refactoring_plan/README.md の Phase <N> だけを実装してください。
Phase <N+1> 以降と Correctness Track には進まないでください。

実装前に、Phase <N> の対象symbol、呼出し元、公開Python APIが
現在のコードと矛盾していないか確認し、結果を先に報告してください。
計画を盲目的に適用せず、矛盾、合法手/順序の変化、heap allocation増加、
stack増加、性能上の危険、既知bugの偶発修正があれば変更前に停止してください。

各commitを計画記載の単位より大きくしないでください。
各commit候補について通常test全件とperformance A/Bを実行し、
95% CIが性能悪化側なら確定しないでください。
main/masterへ直接pushせず作業branchを使い、diff確認後に対象fileだけをstageし、
日本語の簡潔なcommit messageでcommitしてください。force pushはせず、完了時は通常pushしてください。
最後に変更点、test結果、性能結果、allocation/stack、残存riskを報告してください。
```

例:

```text
Sol / Ultra が作成したリファクタリング計画の Phase 2 だけを実装してください。
Phase 3 以降には進まないでください。
まず Phase 2 が現在の MoveGenerator、Action順序、MAX_MOVES と矛盾しないか
確認し、depleted-bank encoder問題をこのPhaseで修正しないことを明示してください。
```

## 16. 各commit/作業終了時の報告形式

```text
変更点:
- 対象file/symbol
- 責務または重複をどう整理したか
- 公開API・合法手集合・順序が不変である根拠

テスト:
- build/install commandと結果
- 通常pytest: passed/failed/skipped、所要時間
- py_compile結果
- 対象property/golden test結果

性能:
- baseline commit / candidate commit
- build/compiler/CPU条件
- workloadごとのraw sample数
- median比と95% CI
- allocation/op、peak stack、RSS、object size
- reject/rerunした変更の有無

残存risk:
- 未確認の外部consumer
- known correctness issue
- platform/toolchain依存
- 次Phaseへ持ち越す事項
```

## 17. 最終的に目指す内部境界

公開Python APIを変えず、内部依存を次の方向へ寄せる。

```text
Game facade + history + modes
  |
  +-- Rule query / transition
  |     +-- canonical Player/Board state
  |     +-- explicit derived-state maintenance
  |
  +-- allocation-free Action emitter
        +-- MoveList/vector adapter
        +-- count/nth/code/mask consumers

MCTS / DFPN / reveal solver
  +-- narrow position-operation adapter
  +-- explicit exact/observable/solver key
  +-- hidden-outcome extension point

pybind11
  +-- unchanged public surface
  +-- measured conversion and GIL scopes
```

この形への移行自体を目的にしない。各小commitが責務を明確にし、合法手/ルール/APIを維持し、実測で性能を落とさない場合だけ残す。特にhash完全化、MCTS batch修正、V2廃止、delta undoは、それぞれ独立した判断点として扱う。

## 18. 調査時点の検証記録

これは実装変更前のsmoke記録であり、Phase 0が要求する統計的A/B baselineの代わりではない。

- `python setup.py build_ext --inplace`: 成功。
- `python -m pytest -o addopts= -m "not performance" --ignore=tests/test_api.py`: 119 passed。
- `tests/test_api.py` 3件: 現環境の `TestClient` requestが停止し完走できなかった。環境のhttpx 0.28.1はprojectのdev指定 `<0.28` の外であり、依存を揃えた環境で再実行が必要。
- `python -m pytest -o addopts= -m performance`: 4 passed、122 deselected。
- `python -m py_compile csplendor/*.py csplendor/api/*.py scripts/*.py`: 成功。
- `pip install -e .`: build isolationがnetwork制限で失敗。`--no-build-isolation` ではwheel buildに成功したが、user siteがread-onlyでinstall段階に失敗した。

performance smokeのbest-of-5値は次のとおりである。同一commit内の一度の実行なので、回帰判定には使用しない。

| metric | value |
|---|---:|
| legal actions | 22,620.10 calls/s |
| legal action count | 328,430.03 calls/s |
| legal action codes | 65,267.42 calls/s |
| Python playout | 89,802.42 moves/s |
| C++ random playout | 168,287.10 moves/s |
