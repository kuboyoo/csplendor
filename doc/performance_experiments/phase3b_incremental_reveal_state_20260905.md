# Phase 3B: 厳密めくれ探索の増分set-state/key

測定日: 2026-09-05

関連成果物:

- `phase3b_incremental_reveal_state_20260905.csv`: 正式A/Bとcontrolの集計
- `phase3b_incremental_reveal_state_evidence_20260905.json`: build identity、統計、semantic anchor
- `phase3b_incremental_reveal_state_diagnostics_20260905.json`: counter、oracle、fallback、test結果
- `raw/phase3b/*.json.gz`: paired benchmarkと検証の完全raw 18件

## 1. 状態

```text
Target phase: Phase 3B — 厳密めくれ探索の増分set-state/key
Baseline revision: 0c5eba654ab4536c70947e725872cf5790db5e92
Source diff SHA-256: ad07e94e123d7a6892001a2f124d3eb0756fddb6a3ebf9a7dcf51172ad818071
Branch: perf/codex56-engine-hotpaths
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release、-O3 -DNDEBUG、C++17、portable、Phase 1〜3A採用案=ON
A: CSPLENDOR_INCREMENTAL_REVEAL_SEARCH_STATE=OFF
B: CSPLENDOR_INCREMENTAL_REVEAL_SEARCH_STATE=ON
Formal measurement: instrumentation=OFF、reveal/hash verify=OFF
CPU affinity: CPU 4（sibling 4,16のうち4のみ、governor=performance）
```

正式A/Bは22 pair / 11 crossover block、各側3 warmup、10,000 bootstrap、ABBA、
固定inode 2-slot crossoverである。A/B binaryはともに1,097,072 bytes、GNU `size`のtextは
ともに931,394 bytes。SHA-256は
`5c5017b0b81a24c6992c06a4b260daac01dcfd7d649e17ecdecdb9939016d775` /
`b0c42a84d3e33a3fa02d7748305c7d3f968349646c851ed76adf7de67e4f328d`。

計測時点ではworktreeの`.git`へ書き込めず、計測前commitを作れなかった。そのためraw
manifestのrevisionはbaseline、`dirty=true`である。A/Bは同一source tree・同一compiler設定から
optionだけを変えて再buildし、binary SHA、全pair、source diffを保存したが、clean provenance gateは
未充足として扱う。本成果物は計測完了後のlocal commitへ保存し、依頼どおりpushは行わない。

## 2. 実装と判断

| hypothesis | implementation | compatibility risk | result |
|---|---|---|---|
| 毎nodeのset/key scan除去 | solver-owned `RevealSearchState`へremaining、acquired-hidden、claimed、rule/deck hashを保持 | StateKey意味論 | 採用 |
| 1回初期化 | `begin_search()`でcanonical検証・sidecar構築後、そのrootを再利用 | persistent solveと補助探索 | 採用 |
| 増分transition/rollback | apply前後の差分観測とRAII guardで全branchを更新・復元 | early return、cancel、node/proof limit | 採用 |
| canonical fast path | card/noble disjointness、level/count、provenance、派生値、token保存を入口で検証 | editor局面互換 | 採用 |
| scan fallback | 非canonical局面またはexact hash無効時は従来経路 | 性能のみ | 採用 |
| 独立oracle | remaining/acquired/claimed、exact/deck-order/set-deck hashを全走査で照合 | debug overhead | VERIFY限定 |

sidecarは公開`Board`へfieldを追加せずsolver内部に置いた。通常行動、具体的なblank refill、
deck reserve、oracle purchase/reserve、reserved purchase、noble acquisition、proof DAG構築・検証を
同じRAII rollback規約へ揃えた。具体的な非topカードのめくれでは従来のdeck順を維持しつつ、
positional exact hashとsidecarのdeck-order hashを同時に更新する。

`CSPLENDOR_INCREMENTAL_REVEAL_SEARCH_STATE`は既定ON。依存する
`CSPLENDOR_INCREMENTAL_EXACT_HASH`がOFFなら警告してscan fallbackし、
`CSPLENDOR_VERIFY_REVEAL_SEARCH_STATE=ON`で依存optionが欠ける構成はCMakeで拒否する。

## 3. correctness

| test | scope | result |
|---|---|---|
| Release A/B native | feature OFF / ON、CTest | 各33/33 pass |
| component oracle | canonical初期化、通常遷移、非top reveal、hidden reserve→purchase→rollback | pass |
| randomized differential | 1,000 seed、最大4合法遷移、4,000照合 | pass |
| fallback | duplicate/noncanonical局面の従来scan結果 | pass |
| search unwind | nested node-limit後のBoard/sidecar root復元 | pass |
| proof DAG | fast path、完全DAG構築・検証、rollback | 37 nodes / 36 edges、pass |
| independent full oracle | exact/deck-order/set-deck hashと全maskを全遷移で照合 | 224,668 checks、0 failure |
| Python full | fresh editable installで`python -m pytest -q` | 556 pass、2 skip |
| Python syntax / performance | `py_compile` / `pytest -m performance` | pass / 4 pass |
| 5手・7手詰み | known 5/7手とparallel iterative | 4 pass |
| ASan+UBSan | Debug、両VERIFY ON、leak検査のみ無効 | 33/33 pass |
| TSan | Debug、両VERIFY ON | 33/33 pass、warning 0 |
| option dependency | hash OFF時warning/fallback、無効VERIFY構成 | pass / configure拒否 |

公開solver終了時にもVERIFY oracleを実行し、通常終了だけでなくnode-limit、principal line、proof
build/validation後のroot復元を検査する。runtime fallbackは正式・oracle計測とも0だった。

## 4. 正式performance

rateはoperations/sまたはnodes/s、RSSはrunner peak中央値である。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| state key / reveal heavy | 6,730,590 | 23,221,359 | **3.3740** | **[3.3346, 3.4525]** | 5,708 | 5,706 |
| exact reveal / five moves | 658,148 | 687,570 | **1.0431** | **[1.0411, 1.0504]** | 16,426 | 16,416 |
| exact reveal / hidden reserve | 849,292 | 888,905 | **1.0468** | **[1.0406, 1.0556]** | 36,394 | 36,392 |
| exact reveal / persistent reuse | 1,159,054 | 1,252,311 | **1.0781** | **[1.0656, 1.0983]** | 48,580 | 48,594 |

state-key microは3.37倍で採用条件の2倍を超えた。通常E2Eはfive_movesで4.31%、
hidden_reserveで4.68%改善し、いずれもCI下限が1を超える。persistent reuseは7.81%改善したが、
prime探索がtimed区間外なので通常E2E 8% gateの根拠ではなく補助結果とする。microの倍率を
E2Eへ外挿せず、実戦寄りの効果は約4〜8%と評価する。

## 5. controlとfallback

| case | B/A | 95% CI | 判断 |
|---|---:|---:|---|
| solver TT | 0.9999 | [0.9947, 1.0120] | 中立 |
| visible solver | 1.0028 | [1.0000, 1.0129] | 中立 |
| board copy/restore | 0.9957 | [0.9671, 1.0382] | 中立、CI広め |
| noncanonical exact fallback | 0.9899 | [0.9786, 1.0730] | scan経路・結果一致 |

control 3件は最終root-copy修正前の同一Phase 3B source系列で測った補助記録であり、正式採否には
使わない。修正はexact reveal solverのroot sidecar初期化順だけに限定され、control workloadは
そのコードを通らない。noncanonical exact fixtureではA/Bともfallback初期化1回、fast初期化0回、
nodes 12、legal 153、memo 1、states 6、全scan counterとdigestが一致した。

## 6. counter、RSS、oracle

instrumentation ONのfive_moves（3 pair）で、探索量を固定したcounter中央値は次のとおり。

| metric | A | B |
|---|---:|---:|
| state-key calls | 84,789 | 84,789 |
| set-deck hash full calls | 84,789 | 1 |
| state-key fields visited | 5,511,285 | 65 |
| deck cards scanned | 5,704,323 | 0 |
| claimed queries | 1,778,015 | 0 |
| claimed linear comparisons | 40,264,466 | 0 |
| fast/fallback initializations | 0 / 1 | 1 / 0 |
| fast key reads | 0 | 84,789 |
| sidecar transitions | 0 | 139,877 |
| runtime fallback | 0 | 0 |

nodes 139,868、legal 692,386、memo hits/states 17,163 / 67,615、TT
probes/hits/stores 98,704 / 17,173 / 67,615、allocation calls/bytes
1,281,407 / 140,916,189はA=B。つまり改善は探索量やallocationの変化ではなく、対象scanの
除去による。正式4 workloadのrunner/native RSSはいずれもmaterialな悪化がない。

VERIFY buildではfive_movesを224,668回照合してfailure 0、proof DAGを224回照合してfailure 0。
VERIFY自身のfull scanにより診断counterは増えるため、正式速度計測とは分離した。hardware perf
counterは`perf_event_paranoid=4`のため取得不能である。

## 7. semantic equality

```text
five_moves nodes/legal/memo/states: 139,868 / 692,386 / 17,163 / 67,615、A=B
five_moves digest: 9dd9c11919a4581a、A=B
five_moves PL action/reveal: ee6f8466fd88410b / 70ce0d9a743220ca、A=B
five_moves root action/outcome/reveal: edfd4c52f12a9c7a / 65bbe0573367e0a1 / b8027749d8c198fb、A=B
hidden_reserve nodes/legal/memo/states: 418,336 / 2,119,495 / 60,855 / 194,950、A=B
hidden_reserve digest: 684280a42eaabf21、A=B
hidden_reserve PL action/reveal: 6384177c6a05dfc3 / d760fb0049f218d9、A=B
hidden_reserve root action/outcome/reveal: fec2cd9a7cf799c2 / 8973fcf16c663709 / b91d571c6c22b239、A=B
persistent nodes/legal/memo/states: 280,909 / 910,976 / 145,274 / 282,879、A=B
persistent cache before/after/hits: 210,454 / 282,879 / 8,801、A=B
persistent digest / PL action / reveal: 34eff58fdf34805c / 9474cf5beefd9a8d / ee76aa5723b59e01、A=B
fallback digest: a7c6bbca12e669e4、A=B
proof DAG digest/status/nodes/edges: 7e76e5820a15c692 / proven / 37 / 36、validated
```

公開`Board` layout、Python binding、Action wire format、StateKey field/collision metadataは変更して
いない。候補列挙順、principal line、root action/outcome/reveal順、公開status/reason/winning actionも
A/Bで一致した。

## 8. 結論

```text
ACCEPT
```

state-key microが2倍条件を超え、通常exact revealも2 fixtureで有意に改善した。canonical fast
path、editor-compatible fallback、hidden reserveからの購入、node-limit rollback、proof DAG、独立
hash/mask oracleを含むcorrectness gateを通過し、探索量・TT・順序digest・公開結果は同一、RSSも
中立であるため、Phase 3Bを既定ONで採用する。残るclean commit上でのprovenance再計測は性能・
correctnessの採否とは分離し、必要時に実施する。
