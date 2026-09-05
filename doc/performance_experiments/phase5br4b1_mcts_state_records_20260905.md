# 5B-R / 4B-1：Game再利用の棄却・legacy管理表統合の採用

## 結論と基準

- **5B-R: REJECT_AND_REVERT**。Game scratch試作は実装・照合・正式/独立測定を完了。
  確保回数は減るが、事前のprimary 3%改善基準に届かず撤去した。
- **4B-1: ADOPT**。legacy `MCTS::nodes_ / node_aux_ / access_count_` を、node・aux・
  last_accessを持つ1つの`unordered_map`へ統合した。primary 5%改善基準を独立再測定でも通過。
- 4A-1/4A-2は引き続き保留。solver、3B sidecar、3C TT、3D採用コード、数値演算順は変更しない。

両チケットのbaselineは `0f73b38241eaa54497d85ac0493e10acc4332f26`、source digestは
`0a7f6d34f56e1d9738b52cdaaec03b1eebd096188a2dd629f8bd89d0a75b7e33`。
基準worktreeは`csplendor-reveal-transactions`、作業branchは`perf/mcts-state-records`。
元の`csplendor`の未コミットユーザー変更を取り込まず、開始前後のdigest/status一致を確認する。
候補source/binary digestと全rawのmanifestは
[evidence](phase5br4b1_evidence_20260905.json)を参照。

## 測定方法

[事前計画](phase5br4b1_plan_20260905.md)に従い、既存native benchmark、paired A/B、
semantic digest、固定inodeのbinary slot交換を再利用。portable Release、PERF/VERIFY OFF、
seed 42、batch16、各側warmup2、ABBA 22pairs/11 crossover blocks、bootstrap 10,000回。
単一threadはCPU4、非同期4TはCPU4–7。計時中にbuild/testを並走させていない。
外れ値を削除せず、表の倍率は各blockのB/A throughput比の中央値。倍率>1が改善。
旧Phaseの倍率を乗算せず、現行baselineへ直接比較した。

固定workのlegacy/決定的1Tはroot visits/Q・結果digest・既存128sim formal traceを一致させた。
追加prune adapterは計時対象全20,000simのpath/leaf特徴/評価要求と履歴nodeの削除対象を照合。
非同期4Tはroot分布/tree digestの一致を要求せず、完了数・予約解放・停止理由等のledgerを検証。
実NNはロードしていない。主要fixtureのnative 48-action＋模擬評価器のcharacterizationであり、
Python Genbu、V3/3133、PyTorch/Ray、AI全体への改善率を主張しない。

## 結果

| 独立チケット / slice | 正式倍率 [95% block CI] | 独立再測定倍率 [95% block CI] |
| --- | --- | --- |
| 5B-R primary: shared native 1T / five_moves / 20,000sim | 1.0281 [0.9797, 1.0338] | 1.0166 [1.0052, 1.0231] |
| 5B-R legacy / midgame_250 / 20,000sim | 0.9886 [0.9639, 1.0246] | 0.9971 [0.9821, 1.0143] |
| 4B-1 primary: legacy / midgame_250 / 20,000sim | 1.0877 [1.0472, 1.1146] | 1.1033 [1.0756, 1.1356] |
| 4B-1 legacy determinization / hidden_reserve / 10,000sim | 1.0407 [0.9656, 1.0653] | 1.0486 [1.0409, 1.0758] |
| 4B-1 legacy retained tree / 20,000sim | 1.0788 [1.0259, 1.0953] | 1.1058 [1.0833, 1.1206] |
| 4B-1 legacy opening / 10,000sim | 1.0696 [1.0526, 1.0862] | — |
| 4B-1 legacy batch1 / 10,000sim | 1.0759 [1.0530, 1.1075] | — |
| 4B-1 synthetic retained history＋prune / 20,000sim | 1.1146 [1.0875, 1.1304] | — |
| 4B-1 shared native 1T guard | 1.0021 [0.9826, 1.0127] | — |
| 4B-1 root-parallel 1T guard | 1.0066 [0.8826, 1.0263] | 1.0102 [0.9984, 1.0144] |
| 4B-1 asynchronous shared 4T guard | 1.0067 [0.9365, 1.0124] | — |

主要sliceでCIと独立再測定に裏付けられる2%超回帰は確認されなかった。
特に4Tは分散が大きく、2%以内の同等性を厳密に証明したわけではない。並列木への直接効果は
期待しない。prune guardは49,000個の人工的な既展開履歴nodeへ実探索を追加する限定testであり、
自然な対局履歴から得た長期性能とは区別する。

### allocation / メモリ

PERF diagnostic（速度gateには不使用）:

| 対象 | 基準 allocation calls → 候補 | 基準 allocated bytes → 候補 |
| --- | --- | --- |
| 5B-R primary | 420,309 → 345,627（約17.8%減） | 96,491,742 → 96,362,355 |
| 4B-1 primary | 360,485 → 320,493（約11.1%減） | 63,033,139 → 61,743,939 |
| 4B-1 legacy determinization | 146,040 → 126,050 | 34,702,750 → 34,065,598 |

5B-R primaryのclone_lightは20,001→2、再利用provenance最大capacityは6 byteであり、
大きなコピー費の削減にはならなかった。opening/observableの空履歴では効果がさらに小さい。
legacy smokeで見えた約7.8%改善も正式/holdoutでは再現せず、採用理由から除外した。

4B-1 primaryのprocess peak RSS中央値は34,794→33,746 KiB（約3.0%減）。RSS20%削減による
採用ではなく、E2E速度gateによる採用である。一方、50,000個規模の**未展開node中心の人工unit**
ではpeak RSS 49,356→58,560 KiB（約18.6%増）となった。従来は遅延作成だったauxをinlineに
常設するためであり、公開`get_or_create_node`で大量の未展開/terminal nodeを保持する用途には
注意が必要。native batchの通常leafはexpand時に格納される。unit RSSは各1回の診断値である。

## 互換性と実装境界

`MCTSNode`はrecord内の実体であり、公開mutable pointer/referenceが参照する対象は変わらない。
unordered_mapのrehashでaddressを移さず、snapshotはcopyかつLRU非touchのまま。
get_node成功/get_or_create/expand/terminal/backpropの従来touch位置を維持した。
pruneの50,000境界、降順index40,000、strict `<`による40,001個保持、同時刻tie、
1,000,000超でのreset、uint64 wrap、domain変更/clear、prune中の未処理batchを確認した。
public nodeを直接上書きしても既存auxが別管理時と同様に残る。

VL式・加算順・base policy・edge検索・backprop内のVL解除/stat更新の二重lookupは変更しない。
dense node廃止・flat arena・4A metadataも対象外。新しい共有可変stateは導入していない。
OOMの発生位置/確保回数は変わる。旧3mapが確保失敗時に残し得たaccess-only中間entryを
再現する契約は設けず、失敗したrecord挿入でもcounterは進みmapは有効な状態を保つ。
成功した固定workの探索意味と公開参照を同値gateの対象にしている。

referenceへ切り替える場合は、別build directoryで
`cmake -S . -B build/legacy-reference -DCSPLENDOR_MCTS_LEGACY_TREE_RECORDS=OFF`
を指定する（Python moduleを作らない場合は`-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF`も指定）。
全translation unitを同じ設定でビルドする。旧3map経路は削除していない。

## テストと制約

- Release full native: **39/39 PASS**。
- Python: **565 passed / 1 skipped / 4 performance deselected**。性能mark別実行は**4 passed**。
  実際にこのworktreeでビルドしたextensionのpathを記録。package py_compileもPASS。
- Release / PERF / reference-PERF / incremental-hash VERIFY: 各6 native MCTS suitesがPASS。
  5代表sliceの結果・semantics・digestを3profile間で照合。
- ASan＋UBSan: 変更に関係するMCTS 8 suites PASS。TSan: legacy records / ownership / parallel stress
  の3 suites PASS。対象はrawのコマンドに明記（solver全体のsanitizer再実行ではない）。
- 5B-R撤去前にも4profile×6 suites、full Board/history/hash/mode reset oracle、warm allocation、
  bad_alloc後の再利用を検証した。速度gate未達のためfull Python/sanitizerへは進めていない。
- hardware perf: `perf_event_paranoid=4`で**N/A**。sysctl等の権限変更を行っていない。

失敗を隠していない。5B-R最初のunit buildに候補テスト固有のGCC警告があり修正した。
measurementの`opening`は存在しないfixture名だったため`initial`へ訂正し、失敗rawと訂正理由を保存。
PERFだけが出す`ledger_instrumentation_totals_match`をVERIFYとの比較時に誤って比較したため、
当該flagが存在すればtrueであることを確認してから比較対象外にした。探索digest不一致ではなく、
一般のpaired semantic gateを弱めていない。基準側のエンジンfailureは本測定で発生していない。

## 再現記録

- [実行スクリプト](phase5br4b1_record_20260905.py): `CSPLENDOR_MCTS_PHASE=4b1`、stageを
  `build / diagnostics / smoke / formal / holdout / prune_build / prune_measure / deploy / sanitizers / throughput`
  から選択。既存rawを上書きしないため再実行時は`CSPLENDOR_MCTS_VARIANT=v2`等を指定する。
- [prune adapter](phase4b1_prune_session_20260905.cpp): 同一adapterを基準/候補headers・libraryへリンク。
- `raw/phase5br4b1/5br/v1/prototype_sources.json.gz`に棄却5B-Rのtracked patch、新規header/testと
  source/binary digestを保存。再測定には別worktreeでの試作復元が必要。本番には残していない。
- 原依頼書は`raw/phase5br4b1/5br/v1/request.md.gz`へ保存。新規記録を`/tmp`へ残していない。

この2チケットで停止し、4A系や次の最適化へは自動で進まない。
