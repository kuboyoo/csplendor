# エンジン内部仕様

最終更新: 2026-08-05

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

## 局面と所有権

`Board`はbank、3 level×4枚のvisible card、3 deck、場の貴族、2人のplayer、手番、
round/終局状態を持つ。`PlayerState`はgems、bonuses、points、最大3枚の予約、購入履歴、
取得貴族を持つ。`packed_gems`、`packed_bonuses`、`noble_eligibility_mask`はderived値である。

`Game.clone()`はaction/undo journalを含む。`clone_light()`と`shuffled_clone*()`は現在局面と
modeだけを複製する。versioned snapshotも現在局面とmodeだけを保存し、journalとlazy hash
cacheを含めない。

## 手番進行と貴族

通常actionの適用後、そのplayerが条件を満たす場の貴族を調べる。

- 0枚: そのまま手番を終了する。
- 1枚: 自動取得してから手番を終了する。
- 2枚以上: `waiting_noble=true`とし、手番を維持する。この間の合法手は候補貴族を指定する
  `VISIT_NOBLE`だけであり、選択後に手番を終了する。

手番終了時に行動playerが15点以上なら`final_round`を開始する。player 1からplayer 0へ
戻る時点でroundを完了し、final round中なら勝者を確定する。点数が高いplayer、同点なら
購入枚数が少ないplayerが勝ち、そこまで同じならdraw（winner `-2`）である。

通常の合法手が一つもない非終局局面だけ`PASS`を生成する。相手も行動不能ならpass loopを
作らずdrawにする。

## 合法手

- 異なる3色から各1個取得する。
- bankに4個以上ある同色を2個取得する。
- visible cardまたはdeck先頭を予約し、可能ならGoldを1個得る。
- visible/reserved cardを購入する。
- 複数貴族候補から1枚を選ぶ。
- 行動不能時だけpassする。

token上限を超える取得・予約では必要な返却組合せを、購入では色tokenとGoldの有効な
支払組合せをすべて別actionとして列挙する。合法手の集合、生成順、packed code、
editor状態で2048件を超えた場合に先頭2048件を保持する挙動まで互換契約である。

`Game.apply()`は入力を検査する公開入口である。`apply_*_trusted()`は生成済み合法手向けの
hot pathであり、不正入力の結果を保証しない。solver向け低レベルtransitionには失敗時の
部分更新をcallerがrollbackする契約がある。

## Encoding

| Schema | version | size | 用途 |
|---|---:|---:|---|
| action V1 | 1 | 48 | base action。逐次MCTSとlegacy MLの固定契約 |
| action V2 | 2 | 4869 | slotと支払/返却を含む互換API |
| action V3 | 3 | 3133 | card/noble IDと支払/返却を含むAPI |
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
