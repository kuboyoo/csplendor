# Phase 3C: solver TT key/entry圧縮

測定日: 2026-09-05

関連成果物:

- `phase3c_solver_tt_compaction_20260905.csv`: 正式Stage 1 A/B集計
- `phase3c_solver_tt_compaction_evidence_20260905.json`: build、統計、semantic anchor
- `phase3c_solver_tt_compaction_diagnostics_20260905.json`: layout、profile、test結果
- `raw/phase3c/*`: paired benchmark、build provenanceと検証の圧縮raw 23件

## 1. 状態

```text
Target phase: Phase 3C — solver TT key/entry圧縮とflat table
Baseline commit: affb80a1fac3d92dcbd0f7b3b0d480fca6cc4c52
Working commit: 本報告を含むローカルcommit（最終回答に記載）
Branch: perf/codex56-engine-hotpaths
Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 / CMake 4.2.3 / Python 3.12.1
Build flags: Release、-O3 -DNDEBUG、C++17、portable、Phase 1〜3B採用案=ON
A: CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES=OFF
B: CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES=ON
Formal measurement: instrumentation=OFF、reveal/hash verify=OFF
CPU affinity: CPU 4（sibling 4,16のうち4のみ、governor=performance）
```

正式A/Bは22 pair / 11 crossover block、各側3 warmup、10,000 bootstrap、ABBA、
固定inode 2-slot crossoverである。A/B binaryは1,123,632 / 1,133,736 bytes、GNU
`size`のtextは954,154 / 960,518 bytes、SHA-256は
`747f68313b07b3a7c8f40f43b0de34baebe678195a523964203234a4752a6d21` /
`8a69dca8ce5a50fa6c5592f460a3730b98da4e1824fd6669685af46316c1f28e`。

計測manifestはbaseline revisionとdirty worktreeを記録している。A/Bは同じsource tree・compiler
設定から当該optionだけを変えてbuildし、全7ケースを最終candidate binaryへ揃えた。実装、build、
test、contractに関わる9ファイルの内容manifest SHA-256は
`ecfbc3f68a68201656d0dc7f3a44b31519e9556aa441c21236213c9a6f6fd045`である。算出対象、
各file digest、算出手順、GNU `size`内訳は
`raw/phase3c/stage1_build_source_provenance_20260905.json.gz`に保存した。

## 2. 仮説と変更

Phase 3B後も、exact persistent tableの1要素はkey/valueだけで120 bytesを占め、通常memoも
persistent専用のgeneration/touchを保持していた。Stage 1では`unordered_map`を維持したまま、
用途ごとのkey/entryへ分離することでRSSとcache localityを改善する。Stage 2の自前flat tableは、
Stage 1後にもmapが主要hotspotである場合だけ実装する。

| file | symbol | change | compatibility risk |
|---|---|---|---|
| `src/solver_tt_types.h` | reveal/visible key、entry | metadataを64-bitへpackし、transient/persistent/exact/visibleを用途別に分離 | key同値性、整数幅 |
| `src/reveal_verified_solver.cpp` | `memo_` / `exact_memo_` | transientは24-byte entry、exactはroot-independent専用keyと32-byte persistent entryを使用 | cache再利用、proof再生 |
| `src/visible_only_solver.cpp` | minimax/forced TT | action count、score、status/reasonを小幅型へ格納し公開時にwide型へ戻す | reason、bound意味論 |
| `scripts/benchmark_engine_hotpaths.cpp` | `solver_tt` / layout probe | surrogateでなくproduction key/entryを直接計測し、全solver TT layoutを出力 | benchmark digest更新 |
| `scripts/benchmark_manifest.py` | CMake allowlist | Phase 3C optionを安全に記録しA/B fingerprint軸から除外 | provenanceのみ |
| `tests/solver_components_unit.cpp` | TT contract | 全metadata、境界値、layout、constant-hash衝突を検査 | testのみ |
| `doc/refactoring_contracts.json` | internal header | 新規内部headerを安定性分類へ登録 | 公開APIなし |

exact keyからは、root-independent探索で必ず0となる`acquired_hidden` 128 bitだけを除いた。
非0値からexact keyを作ろうとするとReleaseでも例外にし、誤ったtransposition hitを防ぐ。
通常reveal keyは全fieldを保持する。persistent entryのgeneration/touchは`uint64_t`のまま、
replayable flagも保持した。通常revealのoracle行動数はeditor局面で16 bit上限を証明できないため
`size_t`を維持し、visibleは`MAX_MOVES=2048`に基づきchecked `uint16_t`、persistentはchecked
`uint32_t`とした。公開`Board`、`Game`、Action wire format、Python APIは変更していない。

## 3. layout

64-bit GCC/libstdc++ buildでの`sizeof`である。

| object | OFF | ON | delta |
|---|---:|---:|---:|
| reveal state / depth key | 56 / 64 | 48 / 56 | -14% / -13% |
| exact state / depth key | 56 / 64 | 32 / 40 | -43% / -38% |
| transient / persistent entry | 56 / 56 | 24 / 32 | -57% / -43% |
| transient / persistent map value | 120 / 120 | 80 / 72 | -33% / -40% |
| proof-node map value | 72 | 64 | -11% |
| visible memo / force entry | 32 / 32 | 16 / 16 | -50% / -50% |
| visible force bounds | 72 | 40 | -44% |
| visible memo / force / bounds map value | 48 / 56 / 88 | 32 / 40 / 56 | -33% / -29% / -36% |

compact hasherを`noexcept`にしたことで、libstdc++のnode内hash cache方針も変わる。そのためRSS
低下を`sizeof(value_type)`だけへ全量帰属させない。公開layoutはAction 21、Board 392、Game
448 bytesでA/B同一だった。

## 4. correctness

| test | command/scope | result |
|---|---|---|
| Release native A/B | compact OFF / ON、CTest | 各33/33 pass |
| ASan+UBSan | Debug、compact ON、hash/reveal VERIFY ON | 33/33 pass |
| Python full | fresh editable install、`python -m pytest -q` | 556 pass、2 skip |
| Python performance | `python -m pytest -q -m performance` | 4 pass |
| Python syntax | `python -m py_compile csplendor/*.py` | pass |
| 5手・7手詰み | known 5/7手、parallel iterative、lazy frontier | pass（対象7 test全通過） |
| proof DAG | exact reveal、完全DAG構築・検証 | proven、37 nodes / 36 edges |
| collision | constant hasher、128 full keys、lookup、duplicate update | pass |
| option matrix | compact reason ON/OFF × TT compact ON/OFFのunit | pass |

正式7 pairedの全sampleで、correctness counter、候補順、principal line、root action/outcome/reveal、
status/reason、digestがA/B一致した。Stage 1は`unordered_map`を維持するため、衝突testはfull-key
equalityを確認する。Stage 2固有のwrap-around、full-capacity、erase/rebuild試験はStage 2未着手の
ため実施していない。

## 5. 正式performance

rateはoperations/sまたはnodes/s、RSSはrunner peak中央値である。B/Aは各側絶対中央値の単純比
ではなく、2-pair crossover block比の中央値である。

| workload | A median | B median | B/A | 95% CI | RSS A | RSS B |
|---|---:|---:|---:|---:|---:|---:|
| production state key / reveal heavy | 21,527,027 | 21,005,497 | **0.9767** | **[0.9752, 0.9899]** | 5,718 | 5,708 |
| production solver TT / midgame | 16,317,412 | 21,096,688 | **1.2840** | **[1.2581, 1.3308]** | 5,726 | 5,694 |
| exact reveal / five moves | 364,944 | 378,020 | **1.0391** | **[1.0033, 1.0782]** | 16,446 | 13,184 |
| exact reveal / hidden reserve | 458,099 | 462,557 | 0.9559 | [0.8815, 1.0493] | 36,402 | 27,036 |
| exact reveal / persistent reuse | 635,091 | 709,464 | 1.0688 | [0.9782, 1.1101] | 48,582 | 35,278 |
| visible solver / five moves | 500,392 | 488,275 | 0.9631 | [0.9395, 1.0553] | 13,036 | 11,484 |
| visible solver / forced pass | 2,278,664 | 2,250,367 | 0.9901 | [0.9810, 1.0011] | 17,742 | 13,732 |

Stage 1固有の採用条件は「RSS 10%以上、またはNPS 3%以上」である。exact 3ケースのRSSは
19.83% / 25.73% / 27.38%、visible 2ケースは11.91% / 22.60%低下し、条件を明確に満たす。
速度はproduction TT microが28.40%、exact five_movesが3.91%改善した。hidden reserve、
persistent reuse、visible 2件はCIが1を跨ぐため速度中立と扱い、全solverが高速化したとは主張しない。

production state-key単体は2.33%の有意な回帰で、一般の2%回帰目安を0.33 point超えた。これは
metadata packの局所コストであり、exact E2EではTT locality改善に相殺され、再profileでもkey生成は
self sample 0だった。RSS採用条件を大幅に満たし、主要E2Eの有意な速度悪化もないためStage 1は
採用するが、このmicro回帰を既知の残存リスクとして記録する。Phase 3Cでproduction型へ作り直した
`solver_tt` / `solver_state_key`はPhase 3Bのsurrogate型と絶対値・digestを直接比較できない。

## 6. semantic equality

```text
five_moves nodes/legal/memo/states: 139,868 / 692,386 / 17,163 / 67,615、A=B
five_moves digest: 9dd9c11919a4581a、A=B
hidden_reserve nodes/legal/memo/states: 418,336 / 2,119,495 / 60,855 / 194,950、A=B
hidden_reserve digest: 684280a42eaabf21、A=B
persistent nodes/legal/memo/states: 280,909 / 910,976 / 145,274 / 282,879、A=B
persistent cache before/after/hits: 210,454 / 282,879 / 8,801、A=B
persistent digest: 34eff58fdf34805c、A=B
visible five nodes/legal/hits/states: 100,000 / 406,188 / 31,702 / 0、A=B
visible five digest: b6cdbdc80156e463、A=B
forced pass nodes/legal/hits/states: 1,000,000 / 1,571,214 / 339,838 / 125,771、A=B
forced pass digest: 46ac22ac2ea2fecf、A=B
proof DAG status/nodes/edges/digest: proven+validated / 37 / 36 / 7e76e5820a15c692
```

exact/visible E2E digestはPhase 3Bのsemantic anchorとも一致する。

## 7. Stage 2再profile

instrumentation ONのhidden-reserve検索では、418,336 nodesに対してTT probe 291,703、hit
60,865、store 194,950、full-key equality 182,529だった。equality呼出し数/probeは平均0.626、
分布は0回49.53%、1回40.75%、2回7.73%、3〜4回1.94%、5回以上0.045%で、p99は3〜4回の
bucket内である。これは`unordered_map`のequality呼出し数であり、open-addressingのslot probe長
ではない。

gprofは73 sampleと粗く、最適化後のidentical-code folding由来と考えられる不自然なsymbol帰属も
含むが、production TT `operator[]`のself sampleは0、callgraph totalは約0.6%だった。一方、公開
カード補充候補のsort、行動score、合法手生成が上位だった。hardware perf counterは
`perf_event_paranoid=4`で取得不能である。

したがって「Stage 1後もmapが主要hotspot」というStage 2開始条件は満たされない。Stage 2の
contiguous open-addressing tableは実装せず、load factor比較、置換戦略、hard memory boundなども
導入しない。282,879状態ではpower-of-two容量丸めだけでもpacked `unordered_map`よりRSSが増える
可能性があり、現時点では複雑性に見合う証拠がない。

## 8. 結論

```text
Stage 1: ACCEPT
Stage 2: NOT_ENTERED（profile gate未充足）
```

Stage 1は既定ONで採用する。full-key意味論、探索量、順序、persistent再利用、5手・7手、proof
DAG、sanitizerを維持しつつ、実solverのRSSを約12〜27%削減し、exact five_movesを3.91%、
production TT microを28.40%高速化した。残存リスクはstate-key単体2.33%回帰とABI依存のlayout
実測値である。次に実行すべき独立PhaseはPhase 3D（solver専用delta rollback）。pushは行わない。
