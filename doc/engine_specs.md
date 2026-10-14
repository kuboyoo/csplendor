# エンジン内部仕様

最終更新: 2026-10-05

この文書は現在実装の主要なゲーム・探索契約を要約する。数値契約の機械可読な正本は
`src/encoding_schema.h`と[`refactoring_contracts.json`](refactoring_contracts.json)である。

## 宝石と静的データ

宝石IDは全層で次の順序に固定する。

| ID | GemType | 色 | USI |
|---:|---|---|---|
| 0 | `DIAMOND` | White | `W` |
| 1 | `SAPPHIRE` | Blue | `U` |
| 2 | `EMERALD` | Green | `G` |
| 3 | `RUBY` | Red | `R` |
| 4 | `ONYX` | Black | `K` |
| 5 | `GOLD` | Wildcard | `D` |

5要素のcost/requirement/bonus配列はGoldを含まずID 0--4、6要素のgem/bank配列は
ID 0--5を使う。カードはID 0--89、貴族はID 0--11を静的データとして持つ。通常の2人用
初期局面では、この12枚から3枚を場へ選ぶ。

全IDの色・点数・コストは [カード・貴族カタログ](card_catalog.md) を参照。

## 局面と所有権

`Board`はbank、3 level×4枚のvisible card、3 deck、場の貴族、2人のplayer、手番、
round/終局状態を持つ。`PlayerState`はgems、bonuses、points、最大3枚の予約、購入履歴、
取得貴族を持つ。`packed_gems`、`packed_bonuses`、`noble_eligibility_mask`はderived値である。

`Game.clone()`はaction/undo journalを含む。`clone_light()`と`shuffled_clone*()`は現在局面と
modeだけを複製する。versioned snapshotも現在局面とmodeだけを保存し、journalとlazy hash
cacheを含めない。

## 手番進行・返却フェーズ・貴族

山札予約（`RESERVE_DECK`）は返却を持たない。予約で所持トークンが11枚（10枚＋金1枚）に
なった場合は返却フェーズに入る。

- 手番を維持し、`waiting_return=true`（`pending_decision=1`）とする。
- この間の合法手は`RETURN_GEM`（`ActionType`値7）だけで、手番playerが所持する色
  （金を含む6色）ごとに1つ、各1枚を返す。返却はめくれたカードを見た後に選ぶ。
- 返却後に以下の通常の手番終了処理（貴族判定・手番交代）を行う。順序は「返却 → 貴族」である。
- 返却フェーズ中だけ手番playerは11枚を保持できる。

めくれたカードは相手に非公開のまま、返却色は公開情報である。Zobrist hashは
player別の返却フェーズsaltをexact/observable hashの両方に含める。

通常action（返却フェーズがあればその返却）の適用後、そのplayerが条件を満たす場の貴族を調べる。

- 0枚: そのまま手番を終了する。
- 1枚: 自動取得してから手番を終了する。
- 2枚以上: `waiting_noble=true`とし、手番を維持する。この間の合法手は候補貴族を指定する
  `VISIT_NOBLE`だけであり、選択後に手番を終了する。

`Board.pending_decision`（読み取り専用）は保留中の判断を表す: `0` なし、`1` 返却
（`waiting_return`）、`2` 貴族選択（`waiting_noble`）。`waiting_noble`は互換のため残す。
両flagが同時に立つ状態と、`waiting_return`なのに手番playerの所持が10枚以下の状態は
不変条件`invalid_pending_decision`で拒否する。

手番終了時に行動playerが15点以上なら`final_round`を開始する。player 1からplayer 0へ
戻る時点でroundを完了し、final round中なら勝者を確定する。点数が高いplayer、同点なら
購入枚数が少ないplayerが勝ち、そこまで同じならdraw（winner `-2`）である。

通常の合法手が一つもない非終局局面だけ`PASS`を生成する。相手も行動不能ならpass loopを
作らずdrawにする（終局理由 `STALEMATE`）。

### 千日手（2026-10-14 改定）

対局の中で**同一局面が4回目に現れたら**千日手として引き分け（winner `-2`、終局理由
`REPETITION`）で終局する。出現は連続していなくてよい。先後を入れ替えた指し直しはしない。
（2026-10-07〜10-14の旧規則「両者が実質パスを3回ずつ連続」は、この規則に含まれるので置き換えた。
両者が実質パスを続けると同じ局面が0・2・4・6手目に現れ、6手目で4回目になる。）

- **局面**は完全な状態である: bank、場札、山札の残り枚数と順序、貴族、両playerのtoken・bonus・
  点数・予約（伏せ予約も）・獲得貴族、手番、返却待ち・貴族選択待ちの途中状態、最終ラウンド。
  turn番号は含めない。手番内の途中状態（`RETURN_GEM` や `VISIT_NOBLE` を待つ局面）も1局面として数える。
- カードか貴族が動く手（購入・予約・貴族獲得）や最終ラウンドの開始の後は、それより前の局面は二度と
  現れない。またtoken総数は返却待ち（予約の直後だけ）を除いて減らない（取得は `min(所持+取得, 10)` に
  なる）ので、どちらかのtoken総数が変わった後も前の局面は現れない。そのため判定に必要な履歴は
  「最後にこれらが起きた後の局面」だけであり、エンジンはその区間を `Board.repetition_keys` に持つ。
- 区間内で変わるのはtokenの所持、手番、途中状態だけなので、`Board.repetition_position_key()`
  （両playerの6色の所持数5bitずつ、手番、貴族選択待ち、返却待ち）が区間内の局面を厳密に識別する
  （bankは総数から決まる）。`repetition_keys` は区間の局面を古い順に並べ、最後が現在局面である。
  空は「区間が現在局面から始まる」（初期配置、区間を切る手の直後、エディタでの編集後）を表す。
- `Board.repetition_count` は現在局面が区間に現れた回数（1以上）。4で千日手。
- 判定は手の適用（`apply` / `apply_trusted`、ソルバーのoracle手も同じ）の中で行う。そのため
  探索（MCTS）・詰みソルバー（厳密な詰み探索、準詰み）・Python・WASMのすべてが同じ終局を見る。
- 手番終了で最終ラウンドの勝敗が確定した場合はそれを優先し、両者パスの判定も千日手より優先する
  （両者パスで終わる `PASS` は `STALEMATE`）。
- 区間は局面の未来を決めるので、厳密hash・観測hash（多重集合として、順序は含めない）、snapshot、
  情報集合に含む。`observable_repetition_hash()` には含めない。探索の入力特徴量には含めない。
  区間が空の局面のhashは旧規則のときと同じである。
- 山札の順序は区間内で変わらないので、鍵に含めなくても結果は同じである。

`Board.end_reason`（`GameEndReason`）は終局理由を表す: `NONE`（未終局）、`NORMAL`（最終ラウンドの
決着）、`STALEMATE`（両者パス）、`REPETITION`（千日手）。

## 合法手

- 銀行にある色から最大3色を選んで各1個取得する。銀行に2色以下しかなければその色数だけ取得する。
- bankに4個以上ある同色を2個取得する。
- visible cardまたはdeck先頭を予約し、可能ならGoldを1個得る。
- 山札予約後の返却フェーズで1枚返す（`RETURN_GEM`）。
- visible/reserved cardを購入する。
- 複数貴族候補から1枚を選ぶ。
- 行動不能時だけpassする（千日手の局面の出現として数える）。

token上限を超える取得・公開予約では必要な返却組合せを、購入では色tokenとGoldの有効な
支払組合せをすべて別actionとして列挙する。合法手の集合、生成順、packed code、
editor状態で2048件を超えた場合に先頭2048件を保持する挙動まで互換契約である。

`Game.simple_payment_mode=True` は購入時だけGold最小使用の1通りに限定する。
`False` は全支払いを生成する。Gameの既定値はFalse、HTTP `POST /game` の既定値はTrue。
返却パターンは簡易支払いでも複数残る。通常対局では `blank_refill_mode=False` を使う。
`Game(seed=0)` も固定seedであり、ランダム初期化を要求する特別値ではない。

`Game.apply()`は入力を検査する公開入口である。`apply_*_trusted()`は生成済み合法手向けの
hot pathであり、不正入力の結果を保証しない。solver向け低レベルtransitionには失敗時の
部分更新をcallerがrollbackする契約がある。

## Encoding

| Schema | version | size | 用途 |
|---|---:|---:|---|
| action V1 | 1 | 48 | base action。逐次MCTSとlegacy MLの固定契約。返却フェーズではslot 0..5を返却色に再利用 |
| action V2 | 2 | 4869 | slotと支払/返却を含む互換API |
| action V3 | 3 | 3133 | card/noble IDと支払/返却を含むAPI。返却フェーズは表現不可 |
| action V4 | 4 | 3121 | V3の山札予約返却を分離し`RETURN_GEM` 6枠を追加。[仕様](action_space_v4.md) |
| state feature V1 | 1 | 196 | bank、players、visible、deck count、nobles、手番 |

schemaのversion、offset、fingerprint、宝石順はC++ descriptorを正本とし、Python wrapperも
同じ値を公開する。既存schemaの意味は変更せず、変更が必要なら新versionを追加する。

## MCTSとhidden information

逐次`MCTS`は安定した公開経路で、内部orchestrationもV1の48 actionを使用する。
determinization有効時はobserverから見えないdeck順と相手のhidden reserved cardを
seed付きでrandomizeし、observable domainのtree keyを使う。

共有tree throughput、deterministic epoch、独立tree root-parallelはexperimental opt-inである。
既定の`num_threads=1`はworker queueを作らないserial pathで、複数threadは既定化していない。
deterministic epochは決定順のtrace/replay oracleであり、parallel completion reorderを
再現するmodeではない。timeoutはcallback境界で観測するsoft deadlineである。

## Solver

visible-only solverは見えている局面だけを対象とする。reveal-verified solverはhidden outcomeを
列挙し、oracle metadataとproof DAGを構築する。両者は共通value typeと通常rule primitiveを
利用するが、探索順、memo key、proof semanticsはMCTSや`Game`へ混ぜない。C++の
reveal-verified headerと並列MCTS headerはexperimental分類を維持する。
山札予約後の返却は予約手の一部として扱い、詰み深さを消費しない。
