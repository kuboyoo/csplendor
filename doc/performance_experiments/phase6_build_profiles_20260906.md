# Phase 6：build profileの独立評価・最終統合検証

## 結論

**Release LTOをopt-inで採用。Linux nativeとPGOは棄却・撤去。**
portable既定、Apple Silicon既存profile、Python API、採用済みengine sourceを維持する。
Python拡張のLTOは既存pybind11で有効だったため `SKIP_ALREADY_DONE`。
したがって、今回の約1.06倍は**native実行ファイルの厳密めくれ探索**に限定したbuild効果で、
既定Python拡張・実NN・問題生成全体の追加高速化ではない。既定ビルドの速度は変わらない。

| 独立案 / primary | 正式 A→B (ms) | 正式倍率 [95% block CI] | 独立holdout倍率 [CI] | 判断 |
|---|---|---|---|---|
| Linux native | 1052.917→1034.288 | 1.0199 [1.0118, 1.0275] | 1.0133 [1.0058, 1.0248] | 棄却 |
| portable LTO | 1052.510→985.973 | 1.0610 [1.0530, 1.0783] | 1.0593 [1.0528, 1.0691] | opt-in採用 |
| portable PGO | 1081.147→1011.788 | 1.0732 [1.0653, 1.0861] | 1.0700 [1.0613, 1.0729] | guard回帰で棄却 |

primaryは全行 `exact_reveal/hidden_reserve/depth7/1,000,000 node limit`。
いずれも上限でUNKNOWNとなる同量探索であり、「7手詰めを完全に証明する時間」ではない。
棄却コード撤去後の最終LTO binaryでも **1.0561 [1.0526, 1.0669]**、
1057.056→1002.028 msを確認。最良のsmokeだけを採らず、全runを保存した。
表の倍率は2-pair crossover blockのrate比中央値で、時間中央値の比とは異なり得る。

## §22：基準・変更・契約

- Ticket: Phase 6 / 共通§19–22。
- Classification: 固定順はS0、非同期throughputの評価はS1。S2の演算順変更なし。
- Baseline commit: `3582909e7709d1c568e481c7ef965e41e1e62832`（5C-B完了）。
  source digest: `f4ccee37c9ee63fd538b7ad6303f62b36f36925121a9cad8e589f2eb39b72b8d`。
- Candidate: 本報告を含む `perf/build-profiles` commit。source digest:
  `763d84418fcedaebc363f79a7a1e2f61dbc26dffcb5c0ca16cbf0843e6567752`。
  全file hashは `raw/phase6/final_source.json.gz`。
- Reference: 同じ最終採用source・同じcompilerのportable Release。
  最終portable benchmarkは5C-BのbinaryとSHAが完全一致：
  `d8bf634fc0d5c52c3e841419bf121e0d3fa2ed76a0a29cbd16e49184cd230e54`。
- Changed symbols: CMake `CSPLENDOR_ENABLE_LTO`（既定OFF、Releaseのみ、IPO対応検査）、
  benchmark manifestのprofile軸fingerprint、paired runnerの明示軸とETXTBSY限定retry。
  `src/`、`csplendor/`、setup.py、公開API、ルール・solver・MCTS本体に差分なし。
- Prior phases / 3B/3C: [共通監査](phase6_common_audit_20260906.md)に全採否・sidecar/TT/
  rollback/cache/上限契約を整理。過去raw **2,162件**のSHAを再確認。既知tradeoffを保持。
- 元worktreeは本作業では変更しない。独立作業で`5f29b50`→`3d9dcf2`へ進んだため、
  その別系統を今回統合・検証済みとはしない。開始/観測sourceを両方保存した。
- Next single ticket: **なし。指定された最終監査で停止**。4A-1/2は引き続きユーザー指定の保留。

## 実flags監査

GCC **15.2.0**、CMake **4.2.3**、pybind11 **3.0.1**、Python **3.12.1**、
Ryzen 9 7900X / Linux x86_64。compiler/linker版、SHA、compile_commands、verbose build、
link.txtを保存した。CMake option名だけからLTO有効性を推測していない。

| 対象 | 実compile/link設定 | 備考 |
|---|---|---|
| native portable | `-O3 -DNDEBUG -std=c++17`、LTOなし | 配布CPU既定 |
| native LTO | 上記＋`-flto=auto -fno-fat-lto-objects`、linkにもLTO | 最終採用opt-in |
| Python extension | `-O3 -DNDEBUG -fPIC -fvisibility=hidden -flto=auto -fno-fat-lto-objects` | pybind11が既に付与 |
| Linux native試作 | portable＋`-march=native -mtune=native` | 撤去済み |
| PGO generate | portable＋GCC generate/prefix-path/atomic、Pythonは既存LTOも有効 | 学習専用 |
| PGO use | portable＋GCC use/prefix-path、missing-profile/coverage-mismatchをerror | 撤去済み |
| ASAN/UBSAN・TSan | `-O1 -g -fno-omit-frame-pointer`、対応sanitizer | 採否時間に使わない |

`-Ofast`、`-ffast-math`、`-ffinite-math-only`は加えていない。Apple実機・Windows実行は未検証。
portable wheelの実build/import、native wheel拒否、skip-build拒否、native cross拒否を確認。
これはローカルLinux wheelのCPU設定確認で、manylinux認証や他OSでの動作保証ではない。

## 比較方法と回帰gate

[実施前契約](phase6_plan_20260906.md)：primary >=1.03、CI下端>1、独立再測定で再現。
guardの2%超回帰はCIと独立再測定で判断。既存runnerで22 pairs / 11固定inode crossover
blocks、warmup2、bootstrap10,000。smokeだけ4 pairs。1TはCPU4、8Tは4–11、4Tは4–7。
重いbuild/full testsと計時は並走させていない。新しい比較軸は明示指定だけ許可し、
compiler・他flags・sanitizer/verify・argv・S0 semantic gateを緩めない。

- Linux native：primaryは3%未達。visible-onlyも正式 **0.9653 [0.9600,0.9749]**、
  holdout **0.9670 [0.9414,0.9739]** と2%超の低下が再現したため撤去。
- PGO：primary/MCTS/visibleの改善はあるが、random selfplayは正式
  **0.9670 [0.9633,0.9693]**、holdout **0.9614 [0.9537,0.9670]**。回帰を無視して採用しない。
- LTO：warm exact **1.0653 [1.0588,1.0754]**、legacy **1.0017 [0.9843,1.0350]**、
  V3 selfplay **1.0183 [0.9894,1.0580]**。後二者の高速化は主張しない。

### LTOのMCTS tradeoffを隠さない

| serial native48 MCTS 20,000sim | 倍率 [95% block CI] |
|---|---|
| 正式 | 0.9892 [0.9689,1.0047] |
| holdout | 0.9850 [0.9698,1.0006] |
| 最終配置 | 0.9776 [0.9704,0.9878] |
| 閾値を跨いだため追加再測定 | 0.9834 [0.9726,0.9967] |

約1〜2%の低下傾向があり、最終配置の一系列は2%を超えた。再測定では2%超が再現せず、
primaryと事前guard規約に基づき**測定済みsolver向けopt-in**として採用する。
ただし厳密な2%非劣性は証明できていない。MCTS目的なら既定portableを維持する。
後から良いrunだけを選んだり、既定ONへ変更したりはしない。

shared8Tの正式 **0.9862 [0.9780,0.9910]**、最終 **0.9906 [0.9509,1.1606]**、
root4Tの最終 **0.9940 [0.9429,1.0159]**。並列MCTSの高速化・棋力改善は主張しない。
sharedは実completed、unique evaluated leaves/s、path steps/s、owner/waiter、stop reasonを
manifestに同時収録。全runでledger・VL・reservation回収を検査し、非同期root/tree一致は要求しない。

最終shared8Tの各run速度中央値（`raw/phase6/lto_final_shared.json.gz`）：

| S1指標 | portable | LTO |
|---|---|---|
| completed sims/s | 350,564 | 347,323 |
| unique evaluated leaves/s | 350,441 | 347,202 |
| path steps/s | 814,369 | 806,450 |
| owner/waiter件数比 | 2856.14 | 2677.57 |

全runのstop reasonはCompletedで、waiter=0のrunはなかった。requestedを分子にした値ではない。

## PGO：学習とholdout、失敗の扱い

同じsourceをgenerate/useの別directoryでbuild。CLI seed17でinitial/midgame_250/
reveal_heavy/editor_fallback/forced_passを学習。合法code/count、着手、exact/determinized MCTS、
hidden特徴量、node上限、proof、invalid observerを含む。fixed-replay/editor fixtureの実seedは
CLI値と異なる場合があり、rawのfixture seedを正とする。holdout primaryのhidden_reserveと
five_movesは学習に入れていない。random selfplay guardは別seedで同種workloadを測る。

GCCの[公式仕様](https://gcc.gnu.org/onlinedocs/gcc-15.2.0/gcc/Instrumentation-Options.html)に従い、
counter更新をatomicとし、別build間のprofile名をprefix-pathで揃えた。
source/flags/compiler/profile SHAを保存してuse前に検査。stale sourceと既存profile再学習の拒否も確認。
初回はarchiveから未使用TUがlinkされずprofile欠落をerrorとして検出した。v2では学習実行ファイル
だけwhole-archiveでcounterを登録し直し、新しいprofile directoryで学習した。
本番use linkは通常のまま、missing/mismatch警告の抑制なしで成功した。
generateの時間を「最適化後」と報告していない。PGOは回帰で棄却したため、full PGO
Python/sanitizerを採用gateとして実施したとはしない。

## 機構・memory・build費用

LTOは翻訳単位を跨ぐ最適化をcompilerへ許可するもので、手順・枝刈り・訪問node数を減らす変更ではない。
静的textは **1,007,404→928,913 B**、実行ファイルは **1,193,312→1,104,392 B**。
選択4targetのbuild記録はportable 24.84秒、LTO 16.35秒だが、全targetsや他PCのbuild時間へ外挿しない。
hardware perfはparanoid=4でN/Aのため、cache missやcycles別の原因は未確定。

primary正式のcurrent RSSは **49,214→49,096 KiB**、runner peak RSSは **54,158→54,146 KiB**。
materialなRSS削減は主張しない。PERF専用の最終deep診断はallocation **3,297,636回**、
累計確保 **317,909,063 B**、scratch action/reveal最大capacity **474/64**。
これは現行の単独診断で、過去Phase 0の比率の流用やLTO前後のallocation削減率ではない。
retained TT/容量上限は不変。累計確保bytesをlive/peak RSSと混同しない。

## 統合検証・既知failure

- portable / LTO native：各 **42/42 PASS**。
- Python：**586 passed, 1 skipped, 4 deselected**。性能マーク **4 PASS**、py_compile PASS。
  既知5/6/7手詰め、session再利用/上限、frontier、公開binding、owning arrayを含む。
- ASAN+UBSAN **8 suites PASS**、TSan **4 suites PASS**。並列停止・失敗注入・NaN等も既存suiteで検査。
- hash/sidecar/rollback/score VERIFYをONにした強いoracle **3 suites PASS**。
- portable wheel作成・展開した実extensionの明示import/特徴量/hidden/invalid検査PASS。
- pairedの初回追加keyword漏れは4テストで検出し修正。engineのfailureではない。
- root-parallel fixed-slotのETXTBSYは基準A/Aでも再現。計時外で当該errnoだけ最大250ms retryし、
  fixed inode/SHA/非alias検査、他errno即時失敗は維持。成功・上限・他errnoを単体検証。
  修正後の22pair rootはPASS。retry回数もrawに保存し、元の失敗を削除しない。
- perf不可、PGO学習修正、期待したwheel/cross/stale拒否を、未知の候補固有failureと区別してmanifestへ保存。

## 利用・再現

```bash
cmake -S . -B build/portable-lto -DCMAKE_BUILD_TYPE=Release \
  -DCSPLENDOR_BUILD_PYTHON_MODULE=OFF -DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON \
  -DCSPLENDOR_CPU_TARGET=portable -DCSPLENDOR_ENABLE_LTO=ON
cmake --build build/portable-lto --parallel 4
```

正式なPython LTOは[pybind11の既存build動作](https://pybind11.readthedocs.io/en/stable/compiling.html)を維持。
PGO/nativeの本番切替は残さない。試作再現が必要な場合だけ、隔離した基準sourceへ
`raw/phase6/rejected_build_profiles.json.gz`のCMake sourceを復元し、記録済みorchestratorを使う。
`phase6_pgo_20260906.py`は撤去後の通常sourceで黙って非PGOを測定しないよう拒否する。
全実行command、binary/source SHA、A/B raw、memory、採否は
[manifest](phase6_evidence_20260906.json)に保存した。原本はdoc下、生成binary/profile/wheelは未追跡。
