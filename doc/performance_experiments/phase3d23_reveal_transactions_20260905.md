# Phase 3D-2 / 3D-3：任意めくれ復元・購入prefixの採否評価

## 結論

3D-2は2案を実装・検証したが、固定探索量の改善は約0〜1%で、事前の5%採用基準を
満たさず **REJECT_AND_REVERT**。自分が追加した復元実装・build option・専用unitを
撤去した。両案の全ソース差分、新規header/unit、測定結果は圧縮してdoc配下に保存した。

3D-3は費用モデルの評価を完了。代表的なdeep/warm探索では購入ごとの実訪問めくれ数が
すべて1で、共通購入処理を繰り返していなかった。3D-2の改善という着手条件も不成立のため、
prefix本番実装には進まない。現行コーパスに対する提案は **REJECT_AND_REVERT
（実装着手なし）** とする。全応手展開で複数訪問すること自体は別途確認した。

残すのはPERF専用の診断カウンター、回帰テスト、再現資料のみ。
既存0〜3D-1の採用実装・棄却判断を維持する。新しい高速化倍率は主張しない。
Phaseの採否評価完了と、本番高速化の採用・prefix実装完了を区別する。

## 基準と境界

- 分類：S0。同じ合法手・めくれ順・探索量を維持する提案。
- 基準commit：`763a910b8acb2269db2d10364059a5daf2da23ea`（3D-1採用版）。
- 基準source digest：`aff9b0e8407df4309401957f0c79455cddf86582d6384a159ee512d4f75a01dc`。
- 作業branch：`perf/reveal-transactions`、専用worktree：`csplendor-reveal-transactions`。
  元repoの未コミットユーザー差分、既存3D-1 worktreeは変更しない。
- 現行・各試作のsource digest、file hash、binary hash、flags、全rawのhashは
  [manifest](phase3d23_evidence_20260905.json)に記録する。
- 公開Game/undo、3B sidecar、3C TT、scratch/cache/上限、MCTS、支払い列挙は変更しない。
  3D-1で棄却されたreveal通常手全体のcompact化を復活させていない。

## 3D-2：試作と測定

v1は既存`NormalBranchRollback`と、触る山のactive prefix/countだけを保存するguardを
組み合わせた。guardはRelease 472 byte。v2はcanonical/fallbackをループ外で分け、
同じ`UndoRecord`と対象山prefixを持つ静的compact arm（184 byte）にした。
copyする山を1つに限定し、swap-removeや最小inverse shiftは導入していない。

対象はvisible refill / deck reserveの任意位置rotate→applyだけ。canonical sidecarが
activeな場合に限定し、editor/overlap、oracle、通常手、final-round shortcutは従来経路。
restoreは対象山内容→rule/hash/mode→sidecarの順。各再帰guardが自分の親prefixを所有し、
count復元で再activeになるslotも復元する。TT/cacheを巻き戻さない。
apply前にarmし、false/throw/visitor中断で割当不要・noexceptの復元を実行する。
RuleMutatorが破棄された後に親を復元し、古いcandidate hashを後からpublishしない。

事前計画は[plan](phase3d23_plan_20260905.md)。primaryはexact_reveal / hidden_reserve /
depth7 / 固定100万node。CPU4、portable Release、PERF/VERIFY OFF、既存paired runnerの
fixed-inode crossover、22 pairs / 11 blocks、warmup2、bootstrap10000。
build/testを性能測定と並走させていない。hardware perfはparanoid=4で利用不可、N/A。
権限・sysctlは変更していない。

倍率は2-pair crossover block比の中央値、時間は各側の中央値なので、表の時間の単純な比とは
一致しない。primaryはnode limitでUNKNOWNとなり、7手詰み完遂時間を意味しない。

| 試作・primary系列 | 基準 ms | 試作 ms | 倍率 | block 95% CI |
|---|---:|---:|---:|---:|
| v1 正式 | 1001.190 | 995.787 | 1.00244 | 0.99449–1.01319 |
| v2 正式 | 1014.844 | 1003.115 | 1.01165 | 0.99884–1.01762 |
| v2 独立holdout | 1009.938 | 996.500 | 1.01011 | 0.99401–1.01780 |

根拠：`raw/phase3d2/v1/formal_deep.json.gz`、
`raw/phase3d2/v2/{formal,holdout}_deep.json.gz`。
shallow/warm/visible/forced-pass/editor/proof on-off/defenderも各22 pairsで確認。
全系列（悪化・広いCI・smokeを含む）はmanifestとrawに残す。初回smokeのwarm約1.09倍は
正式測定では再現せず、採用根拠に使わない。最良のsliceへprimaryを変更していない。

v2の同量deep診断では以下の機構改善は確認したが、E2E採用条件とは分ける。

| 診断項目 | full snapshot reference | v2 |
|---|---:|---:|
| allocation回数 | 3,297,636 | 2,449,806 |
| 累積allocated bytes | 317,909,063 | 316,740,335 |
| Board snapshot回数 | 1,442,934 | 795,021 |
| 対象山compact capture回数 | 0 | 647,913 |

確保回数は25.7%減だが、累積確保byteは約0.37%減。runner peak RSS中央値はv2正式で
54,126→54,156 KiB、holdoutで54,134→54,196 KiBであり、大きなRSS改善もない。
retained TT/scratch容量の削減は行っていない。
対象山copyは17,795,380 card（uint8）であり、この最大40 byteのcopyだけがなおhotである
証拠は得ていない。複雑なinverse patchへは進めない。

v1/v2はRelease・PERF・reference・強照合の全4 buildでunitを通過。
v1 1,728 / v2 3,456遷移、空/1枚/先頭/中央/末尾、両支払いmode、hash有効/無効、
fallback、同山/別山への入れ子、途中eraseからの例外復元、restore時の無確保を確認。
実solverのrotation前後へ各1/7/17回目のfaultを注入し、再利用を検証した。
9代表sliceのstrong oracle・semantic digestも一致。
採用gate未達の試作にはfull Python/ASanや短時間proofの2000回集約速度gateを追加せず、
この未実施を「PASS」と扱わない。試作の棄却理由はprimary未達であり、proofの単発ノイズではない。

## 3D-3：生成数と実訪問数

撤去後の3D-1経路にPERF専用counterを入れ、候補リスト長・実際のvisitor呼出し数・
共通apply呼出し数を区別した。中断/例外でもscope終了時に0/1/2〜4/5以上の分布を記録する。
visitは成功childをvisitorへ渡した回数で、visitor内の上限チェックで中断したものも含む。

| 通常のsolve計測 | 購入scope数 | 生成候補総数 | 実訪問総数 | 生成平均 / 実訪問平均 |
|---|---:|---:|---:|---:|
| deep / depth7 | 378,797 | 12,038,805 | 378,797 | 31.782 / 1.000 |
| warm / depth7 | 70,230 | 1,954,790 | 70,230 | 27.834 / 1.000 |
| shallow / depth3 | 278 | 8,556 | 278 | 30.777 / 1.000 |
| defender / depth3 | 1,274 | 44,269 | 1,274 | 34.748 / 1.000 |

根拠：`raw/phase3d2/final/retained_diagnostic_*.json.gz`。
deep/warmのcommon apply回数も実訪問数と一致。shallow/defenderのapply総数には
ループ外の手順再生も含まれるため、scope数との差をprefix節約可能回数と数えない。
すべての通常購入scopeが実訪問1で、2枚目以降の共通apply節約可能回数は0。
同じ候補順・反駁時の中断を保つなら、prefix preparationの追加費用を回収できない。
これは「全応手を省略してよい」という変更ではなく、既存探索の実測である。

一方、既存fixtureと実エンジンの`split_root`を使い、five_movesの最初の合法visible purchaseを
固定して全応手をmaterializeすると、生成33 / 実訪問33 / apply33 / restore33となった。
強照合版と全child exact hashを含むdigestも一致（`021211e0f4a669e1`）。
通常solveとroot materializationは別workloadであり、この33を通常探索の平均へ混ぜない。
hidden_reserve/reveal_heavyの根には該当する合法visible purchaseがなく、追加診断はN/A。
これはengine failureではない。standalone probeのPASSを代用せず、エンジンAPIを実行した。

全応手GUI展開をprimaryとする別依頼ならprefixを再検討する余地があるが、本件では
そのmicro効果を通常探索の高速化とみなさず、実装しない。blank applyによる二重山消費、
prepared stateのTT/proof/featureへの露出、rule処理の二重実装はいずれも導入していない。

## 最終状態の検証

- Python：565 passed / 1 skipped / 4 performance deselected。performance別実行は4 passed。
- C++ native：38/38、ASan+UBSan＋強照合：38/38。
- 強照合Python solver：29 passed。既知5手・7手、parallel depth search、proof/frontier、
  cache再利用を含む。貴族/最終round/予約上限/gold支払い/token返却の両modeも確認。
- 診断カウンターの1/3/20枚全展開とedge上限による例外中断、histogramをnative unitで確認。
- benchmark/headerテスト：31 passed。診断8項目をlogical correctness counterから分離。
- `py_compile`成功。候補固有engine failure・基準側engine failureは検出していない。
  hardware perf、追加root診断N/A、試作の未実施gateは別に記録する。

最終Releaseの`reveal_verified_solver.cpp.o`は基準と同一：
`14ffd1d781429306dee473acbba51c99a05f75b920fe914b0526bd66df811d2e`。
診断scopeを単なる空クラスにするだけでは生成コードが変わったため、最終形ではscope自体を
preprocessorで除外した。他のcore objectもcounter名テーブルを持つ`perf_counters.cpp.o`
以外は同一である。Python配置前後のbenchmark binary同一性も確認した。
最終配置のdeep/warm/visible paired A/Bをmanifestに保存する。過去倍率との乗算はしない。

| 最終配置 / 3D-1（各22 pairs） | 倍率 | block 95% CI |
|---|---:|---:|
| deep | 1.00289 | 0.99891–1.01504 |
| warm | 1.00424 | 0.99231–1.01018 |
| visible | 1.00077 | 0.99903–1.01579 |

いずれもCIは1を含む。診断OFFの探索コード同一性とも整合し、速度は維持と判断する。
根拠：`raw/phase3d2/final/retained_{deep,warm,visible}.json.gz`。

## 記録・再現・次のチケット

- [manifest](phase3d23_evidence_20260905.json)：全series、source/binary/hash、gateと未実施事項。
- [計測orchestration](phase3d23_record_20260905.py)：既存runnerとbootstrapを再利用。
  `CSPLENDOR_TX_VARIANT=final`が撤去後のtree用。v1/v2はarchiveの試作source専用。
  記録済みファイルは上書きしないため、再測定時は専用出力先を用意する。
- `raw/phase3d2/{v1,v2}/prototype_sources.json.gz`：`tracked_patch`と`new_files`に全試作差分。
  基準commitの新しいworktreeでのみ再現し、既存のdirty treeへ適用しない。
- [root診断adapter](phase3d3_purchase_visit_cost_20260905.cpp)：本番API＋既存fixtureを使用。
  `cmake -S doc/performance_experiments/purchase_visit_cost_driver -B build/purchase-cost
  -DENGINE_SOURCE_ROOT=$PWD -DCMAKE_BUILD_TYPE=Release`で構成し、`purchase_visit_cost`をbuild。
  `build/purchase-cost/purchase_visit_cost --fixture five_moves --depth 7`で再現できる。
  これは計数診断であり、この単発時間を速度比較に用いない。

次の独立チケットは **4A-1**。`ConcurrentTree`のaccess_epoch / last_accessのconsumerを
監査し、write-onlyと確認できた場合だけ削除する。legacy pruneのaccess_countは対象外。
primaryは実使用backendに対応する固定仕事量のselection/scheduler時間、referenceはこの
最終treeとする。決定的探索のsemantic gateと非同期throughputの会計・budget gateを分離し、
2〜3%の再現改善・他主要sliceの2%回帰guardを事前固定する。本件では着手しない。
