# F0統合・F1：最新main対最終候補の累積比較

## 結論：READY_FOR_REVIEW（F1完了、ここで停止）

承認された隔離統合と互換修正を完了し、**候補`b202e6a`を変更せず最新mainと直接比較**した。
portable同士の厳密めくれ探索は正式 **2.3967倍**、独立再測定 **2.3726倍**。
Python特徴量取得は再測定 **12.8080倍**、特徴量＋環境stepは **6.0436倍**。
測定した経路・固定量に限った実測であり、全エンジン・実NN・問題生成全体の一律倍率ではない。

LTOは別枠で計測した。正式値は1.0719倍だが、再測定の95% CIが1を跨ぐため、
**統合後のLTO追加効果が両系列で確定したとはしない**。既存opt-inと既定OFFを維持する。
main→LTO候補の直接比較は2.5264倍。Phase別倍率の積は使っていない。

main、既存worktree、ユーザー変更を変更せず、push・PR・公開は未実施。
4A保留、棄却案、reference/VERIFY/fallbackの扱いは従来どおり。新しい高速化は行っていない。

## 候補・統合範囲

| 対象 | 固定SHA / source digest |
|---|---|
| BASE_MAIN_SHA | `f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc` |
| CANDIDATE_SHA | `b202e6a0cbb2eded9bc2ee5e59f750428e73ca49` |
| main source | `1256c8b63288c41bb73b971e6852e112fe31ef6c4858660ccdee6aee8e6a930c` |
| candidate source | `05e3c3b52b2e42e1156eeb5facde98c2ba4215214b61b62d0606705a99d11e06` |

source digestの選択・連結規則は[承認前F0報告](finalization_scope_20260906.md)と同じ。
候補の親はF0文書`f494c88`と最新main `f5ec6c5`。採用済み高速化`9415de5`と最新mainがともに祖先。
作業branchは`integration/final-candidate-20260906`、比較mainは新しいdetached worktree。
終了時のリモートmainも`f5ec6c5`で不変。ローカルmain/origin/mainは`7835f64`のまま変更していない。
報告・CSV・rawを追加した後続文書commitと、計測済みengine commitを区別する。

F0で特定したmain側21ファイルを保持。情報集合v2、frontierヒント引数と既定node予算、
Python 3.8互換修正、関連テスト/CI/文書を取り込んだ。R0の重複58ファイルは変更しない。
実際のmerge conflictは`src/reveal_verified_solver.cpp`の1ファイルだった。

コンパクト候補抽出にも**上限で切る前のヒント優先**を接続した。
`src/solver_action_filter.h`はヒントありの比較だけを追加し、ヒントなしは従来の比較処理を維持。
参照vector経路にもmainのsort→hint rotate→truncateを保持する。
テストは独立した全件sort/rotate oracleで、全候補/存在しないヒント、空/短い候補列、
順序変更、購入/貴族/take2種/reserve/passを照合。mainの枝刈り外ヒント回帰も通過した。
本番codeはこの統合後に固定し、正式計測中・後に変更していない。

## 計測方法・共通adapter

[事前契約](final_integration_plan_20260906.md)に従い、既存paired runner / fixed-slot crossover /
warmup2 / **22 pairs・11 blocks** / block bootstrap10,000を使用した。smokeのみ4 pairs。
主値は2-pair crossover blockのrate比中央値で、絶対時間中央値の比とは必ずしも一致しない。
native/Python primaryは同じ固定入力で独立した時間系列を再測定した。異なる未見fixtureの評価ではない。

Ryzen 9 7900X、Linux x86_64、GCC15.2.0、CMake4.2.3、Python3.12.1、pybind11 3.0.1。
CPU4を1T、CPU4–11をshared8T、CPU4–7をroot4Tに割当て。build/full testsと計時は並走させない。
nativeはportable Release `-O3 -DNDEBUG -std=c++17`、PERF/VERIFYなし。
coreは既存の`-fPIC`を保持、採用済みコード切替以外のcompiler/build条件は同じ。

mainには最新版benchmarkがないため、**既存benchmarkのfixture・計時・checksumをそのまま使用**する
[共通adapter](final_common_adapter/CMakeLists.txt)を作成した。
本番エンジンではなくbenchmark側から、mainにない内部型を使う`solver_state_key`、`solver_tt`、
`layout_probe`とそのinclude/補助を機械的に除外し、指定時は明示拒否する。
PERF OFFのno-op headerはbenchmark TUのみに供給する。mainへTT/sidecar等の実装を移植していない。
同じ生成benchmarkをA/B/Cへcompileし、source全文・SHAと実compile/link commandをrawに収録。
元のbenchmark・paired runner・semantic gate自体は変更していない。

Pythonは各々新しい隔離venv（同じsystem packagesを参照）と独立buildを使用し、
global installを上書きしていない。既存の5C-B公開consumer benchmarkとELF launcherを再利用した。
各processでimport先が指定worktree内であることをassertし、実extensionのpath/SHAを別記録した。
**入替slotはlauncherであり.soではない**。runnerのnative-cache manifestとは別に、実extensionの
compiler flagsが両側で一致し、compile/linkに既存pybind11 LTOがあることを検査した。
新APIだけをmainへ移植せず、同じStateFeaturizer利用タスクを旧/新経路で比較している。

## A→B：portable同士の累積コード効果

絶対時間は1 runあたりの中央値。各fixtureと固定量は事前契約・CSV・rawを参照。

| workload | main ms | 候補 ms | 倍率 [95% block CI] |
|---|---:|---:|---|
| exact reveal deep、cold、depth7・100万node | 2510.356 | 1055.173 | 2.3967 [2.3496, 2.4413] |
| exact reveal shallow、depth3 | 3.377 | 1.719 | 1.9580 [1.9436, 2.0205] |
| exact reveal warm session、depth7 | 498.360 | 232.107 | 2.1395 [2.1256, 2.1669] |
| visible-only | 161.194 | 113.441 | 1.4139 [1.4010, 1.4388] |
| legal count、20万回 | 168.763 | 64.248 | 2.6289 [2.6147, 2.6511] |
| legal codes、20万回 | 1287.280 | 528.541 | 2.4383 [2.4290, 2.4466] |
| legal actions、20万回 | 1161.876 | 577.747 | 2.0079 [1.9592, 2.0570] |
| full-actionランダム自己対戦、10万手 | 60.408 | 36.538 | 1.6517 [1.6476, 1.6574] |
| legacy MCTS exact、1万sim | 25.403 | 22.874 | 1.1253 [1.0907, 1.1294] |
| shared backend 1T exact、2万sim | 100.036 | 96.349 | 1.0442 [1.0283, 1.0577] |
| shared8T observable、2万sim | 57.517 | 57.103 | 0.9887 [0.8586, 1.3241] |
| root4T observable、1万sim | 13.541 | 13.464 | 1.0133 [0.9804, 1.0720] |
| V3 selfplay、1万手 | 19.185 | 15.546 | 1.2289 [1.2002, 1.2504] |
| Python StateFeaturizer、5万回 | 354.944 | 27.714 | 12.6837 [12.5634, 13.1497] |
| Python特徴量→full-action step、5万手 | 419.310 | 67.480 | 6.1736 [6.0466, 6.3764] |

V3の改善は採用済みルール処理等を含む累積効果であり、棄却したV3 payment DPを採用した結果ではない。
shared8T・root4TのCIは1を跨ぐため、並列MCTSの高速化は確認できていない。
今回のguardに中央値2%超低下はなかったが、特にshared8TはCIが広く、2%非劣性の証明にはならない。

| 独立再測定 | main ms | 候補 ms | 倍率 [95% block CI] |
|---|---:|---:|---|
| native primary | 2505.494 | 1051.157 | 2.3726 [2.3606, 2.4031] |
| Python primary | 372.145 | 29.038 | 12.8080 [12.5138, 13.0483] |
| Python pipeline guard | 529.332 | 89.372 | 6.0436 [5.7318, 6.1644] |

primaryの改善は事前gate（3%以上、CI下端>1、独立再現）を満たした。
悪いrun・smoke・正式・holdoutをすべて保存し、有利な系列だけを選んでいない。

## B→C：LTO build効果とA→Cの直接比較

native primaryと同じworkloadだけを追加。Cには`-flto=auto -fno-fat-lto-objects`を付与し、linkにもLTO。

| 比較 | A ms | B ms | 倍率 [95% block CI] |
|---|---:|---:|---|
| 候補portable→同候補LTO、正式 | 1119.736 | 1046.459 | 1.0719 [1.0432, 1.1002] |
| 同上、独立再測定 | 1094.214 | 1044.160 | 1.0648 [0.9994, 1.1125] |
| main portable→候補LTO、直接測定 | 2538.984 | 1016.612 | 2.5264 [2.4680, 2.5768] |

最後の行は倍率の掛算ではない。各系列の絶対時間には変動があるため、異なる系列同士の時間比も使わない。
LTO再測定のCIが1を跨ぐことを隠さず、追加効果の再現gateを通過扱いにしない。
既存採用optionの撤去/既定ONへの変更はせず、MCTS含む全用途への推奨もしない。
Phase 6のMCTS低下傾向は既知tradeoffとして保持し、今回LTO版MCTSは再計測していない。

## RSS・並列整合性

native current RSSとrunnerがsampleしたpeak RSSは別指標。短いrunではsample peakが
native currentより小さい場合もあり、kernelの厳密な最大RSSを保証する値ではない。

| 正式workload | current RSS main→候補 KiB | 観測peak RSS main→候補 KiB |
|---|---|---|
| exact deep | 70,192→49,188 | 75,256→54,170 |
| exact warm | 44,204→31,626 | 46,490→33,830 |
| visible-only | 12,842→11,284 | 12,938→11,540 |
| legacy MCTS | 22,364→21,818 | 22,352→21,860 |
| shared8T | 20,938→21,174 | 38,924→38,918 |
| Python featurize | 未取得 | 40,700→40,856 |

deepのcurrent RSSは約30%、観測peakは約28%減。holdoutでも70,152→49,208 / 75,250→54,182 KiB。
LTO単独ではcurrent約49,186→49,056 KiBで、materialな追加省メモリ効果は主張しない。
allocation/累計確保bytesの新計測はしていない。大量未展開legacy nodeで約18.6% RSS増という
以前のtradeoffは、この別fixtureでの小幅減を理由に抹消しない。cacheのhard RSS上限も追加していない。

shared8TはS1。正式run速度の中央値は以下。root訪問分布/tree digestの完全一致は要求しない。

| S1指標 | main | 候補 |
|---|---:|---:|
| completed sims/s | 347,727.5 | 350,245.5 |
| unique evaluated leaves/s | 347,614.4 | 350,105.4 |
| path steps/s | 807,119.0 | 813,103.6 |
| owner/waiter件数比の中央値 | 2856.14 | 2677.57 |

waiter=0のrunは両側0件。全runがCompleted、partialなし。
issued=completed+cancelled+failed、VL追加/解放とreservation回収、stale/duplicate/invalid/integrity=0を照合。
速度中央値の比とcrossover block主値は異なり得るため、この表だけから高速化を結論しない。
root4Tは独立した決定的1T workerの合成で、既存harnessが検査するworker結果・merge・root出力も一致。
単にthread数が多いことを理由にS0 gateを緩めていない。

## 正しさ・検証済み範囲

- native：main **33/33**、候補 **44/44 PASS**。
- Python：main **531 passed**、候補 **595 passed**（双方1 skipped / 4 performance deselected）。
- 性能マーク：両側各 **4 PASS**。py_compile PASS。
- compact OFF＋hash/sidecar/rollback/score VERIFY：native **4 suites**、frontier/情報集合Python **38 PASS**。
- 統合影響範囲のASan/UBSan：solver components / information state / state copyの **3 suites PASS**。
- 既知5/6/7手、frontier遅延展開、ヒント上限外、session/cache容量/cancel・再開を既存Python suiteで確認。
- S0の全pairでstatus/UNKNOWN理由・digest・主手順・順序・論理統計が一致。
  deepは両側100万node、memo hit141,958、保持452,224で、UNKNOWN理由はnode limit。
  これは**7手詰めの完全証明や不詰み証明ではない**。

初回の追加nativeテストだけが未定義`ActionOrderKey::operator==`でcompile失敗した。
3フィールドの比較へ修正し、候補native全体を再実行して通過。失敗rawも保存している。
baseline/candidateの未解決runtime failureは検出していない。環境未検証をPASSへ置換していない。

Phase 6共通監査・採否・過去2,162件の監査は再実行せず、F0で照合済みの証跡を再利用した。
MCTS/並列C++ sourceは今回の統合で変更していないため、TSanはPhase 6の4 suitesを根拠として再利用。
今回の新規TSan、他OS/Apple実機/Python3.8 runtime、実NN/Genbu/GUI受入は**未実施**。
wheelのPhase 6検証は履歴として残すが、統合後wheel検証済みとはしない。今回のPythonは隔離したin-place build。

## 成果物と次の境界

[集計CSV](final_main_vs_candidate_20260906.csv)には全27系列の時間/rate/CI/RSS、
[最終manifest](final_main_vs_candidate_manifest_20260906.json)にはsource/binary SHA、全100件の圧縮rawのSHA、
counter/semantic照合、テスト、失敗分類と制約を収録。
raw原本は`doc/performance_experiments/raw/final_integration_20260906/`に保存した。
早期portable binary auditと固定sourceの差は追加テスト1ファイルのみで、本番入力は一致することも検査した。

再現用：[実行script](final_integration_record_20260906.py) / [集計検証script](final_integration_finalize_20260906.py)。
各command・flagsはrawに記録。記録先はexclusive-createのため、既存証跡へ上書き実行しない。
再実験時は別の隔離worktree/記録先を用意し、固定SHAからbuildする。
今回のcompile artifact/venv/.soはGitへ追加していない。

F0の統合承認待ちは解消し、**F1の測定範囲ではレビュー可能**。
F2実利用受入、F3正式PR準備、F4 main統合・導入は行わず停止する。
READMEの履歴表もF3前に書き換えず、実測の正本はこの報告・CSV・manifestとする。
mainへのmerge、push、PR/Release/PyPI公開、既存branch/worktree削除は別の明示承認が必要。
