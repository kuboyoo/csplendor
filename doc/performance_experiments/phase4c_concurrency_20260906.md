# Phase 4C-1〜4C-3：共有状態削減の試作・採否評価

## 結論

3チケットとも実装・照合・正式A/B・独立再測定まで行い、**REJECT_AND_REVERT**とした。
metrics分散、予約tokenのinline化、lock統合はいずれも事前のprimary 5%改善基準を満たさない。
試作の本番コード、CMake切替、新規試作unitを撤去し、復元可能なpatch/testを圧縮保存した。
今回、採用後の追加高速化は主張しない。

残したのはPERF専用のroot / depth1 / deep別lock・予約占有診断、対応テスト、再現記録。
0〜3C・3Dの採用経路、4B-1 legacy管理表統合、公開API、数値演算順は維持する。
4A-1/4A-2はユーザー指定どおり保留、棄却済み5B-RのGame scratch再利用も復活させない。

## 基準と実行経路

- 開始commit: `2f567ba77cff678d8fad0492f992d110aa67e7f8`。
  source digest: `c45073b99c587aff12a95331665476a4d01c91a2fb28e3c4ed87174951740174`。
- 共通A/B基準: 深さ診断だけを加えた `0fa867d64a6ab98ce1b7e77c1e5e9745ef9c7a57`。
  source digest: `99cabb84383dae5d757a6ff049a1c8541e751b32cf86f828f12d3fd0f1235323`。
  診断schemaを候補と揃えたclean detached worktreeを使用。先行試作は次の基準へ混ぜない。
- 最終engine/source digestは共通A/B基準と同一。最終Release benchmarkも試作前・対照・
  Python extension有効化後の全てでSHA-256が一致した。
  `c2cdd22793789ac5f784effc0ea17a17e4a04ba83b4ce94e0800920a6eddf61d`。
- 元`csplendor`のdirty作業ツリーと前Phaseのworktreeは開始前後のsource/HEAD/status一致を確認。
  作業branchは`perf/mcts-concurrency`。mainへの直接push、reset/stash、forceは行わない。

primaryはnative **48-action**、共有木sharded、8T、batch16、determinizationあり、
`hidden_reserve`、20,000 **completed** simulations、模擬評価器latency0。
探索master seedは42、fixture構築seedは20260726。NN推論、V3/3133、Python Genbu、
棋力、詰み探索や問題生成全体への改善率は測っていない。

既存native benchmark / semantic digest / fixed-inode crossover paired runnerを再利用。
portable Release、PERF/VERIFY OFF、warmup2、ABBA 22pairs / 11blocks、bootstrap10,000。
8TはCPU4–11、1TはCPU4に固定。重いbuild/full testは計時と並走させない。
倍率は各2pair crossover blockのB/A throughput比をまとめた中央値（>1が改善）。
表の時間中央値の比とは一致しない場合がある。外れ値の削除、最良runの選別はしない。

[事前計画](phase4c_plan_20260905.md)の採用条件は各チケットとも「primary >=1.05、
95% block CI下端>1、独立holdout」。小さな案だからと測定後に閾値を下げない。
通常1Tや4T/16T、batch1/64、250µs、warm、coarse、root-parallel、legacyをguard候補としたが、
primary未達で広い正式matrixへ進まなかった。単体テストでの両backend確認と速度matrixは区別する。

## 正式測定と独立holdout

primaryは全行同じ8T / hidden_reserve / 20,000sim。時間は各側中央値、CIは95% block CI。

| 試作 | 正式 A→B (ms) | 正式倍率 [CI] | 独立倍率 [CI] | 判断 |
| --- | --- | --- | --- | --- |
| 4C-1 writer別metrics | 56.107→57.159 | 0.9505 [0.8068, 1.0056] | 1.0019 [0.8727, 1.1104] | 棄却 |
| 4C-2 inline2＋spill | 56.167→56.010 | 0.9940 [0.8805, 1.0350] | 1.0146 [0.8787, 1.0601] | 棄却 |
| 4C-3 v2 両traversal統合 | 55.963→54.148 | 1.0446 [1.0016, 1.0548] | 1.0401 [1.0097, 1.0490] | 棄却 |
| 4C-3 v3 worker限定・template化 | 57.114→54.835 | 1.0477 [0.9924, 1.1061] | 1.0430 [1.0190, 1.0985] | 棄却 |

4C-2の1T exactは1.0229 [1.0186, 1.0281]。secondaryの小改善へprimaryを変更して採用しない。
4C-3 v2は1Tで正式0.9800、独立0.9731と低下した。そこで一度だけworker限定へ修正したが、
v3もprimary基準未達。1Tの中央値は正式0.9770、独立0.9773で、holdout CI
[0.9693, 1.0228]は1を含むため、厳密な2%超回帰の確定とは扱わない。

非同期では実行順・探索木が変わるのでroot分布/tree digestの完全一致は要求しない。
全paired runで完了予算、`issued = completed + cancelled + failed`、VL balance、
全reservation回収、owner/publish/evaluated整合、エラー0、停止理由Completedを確認した。
以下は最終v3の独立holdoutでの各run速度中央値。raw/manifestには全試作・各測定を収録。

| S1 quality/throughput指標 | 基準A | 棄却v3 B |
| --- | --- | --- |
| completed sims/s | 342,419 | 357,916 |
| unique evaluated leaves/s (=owners/s) | 342,290 | 357,809 |
| path steps/s | 795,423 | 831,144 |
| waiter attach/s | 123.5 | 143.4 |
| waiter件数中央値 / 20,000sim | 8 | 8.5 |

owner/waiter比のrun別集計と停止理由も[evidence](phase4c_evidence_20260906.json)へ保存した。
1T決定的経路は現行の41sim formal trace、root visits/Q、結果・論理counterをA/B一致させた。
固定順oracleの一致を非同期木の一致へ拡張していない。

## 深さ診断と試作の仕組み

共通基準のPERF計測（8T / batch16、20,000sim、各1run）：

| traversal深さ | lock取得回数 | wait合計 (ms) | hold合計 (ms) | 予約占有最大 |
| --- | --- | --- | --- | --- |
| root | 40,000 | 525.072 | 94.594 | 31 |
| depth1 | 40,000 | 1.194 | 71.337 | 1 |
| deep (>=2) | 52,910 | 1.282 | 44.721 | 0 |

batch64ではroot最大126、depth1最大4、deep最大0。単純な大きなinline配列ではrootの多数予約と
深いnodeの無駄な常設領域が両立しにくい。占有は挿入直前のsampleであり、runtime上限とは別。
全体node lock waitは623.219ms、shard waitは4.952ms、queue waitは525.422msだった。
これらは複数threadの累積であり、wall-clock時間へ加算したり、本番の費用割合とみなさない。
診断のatomic/clock/TLS自体がscheduleを変える。coordinatorのcommit/cleanupは既存全体集計に
残し、traversalのroot/depth1/deepへ誤って帰属させない。

### 4C-1：metrics-onlyを16 atomic lanesへ分散

21種のmetricsをcache line整列したwriter laneへ分散し、snapshotでreduceする試作。
TLSにはlane番号だけを置き、RAII tokenのcommit/abort/destructorは実際の書込threadのlaneを
使う。lane本体は共有所有ledger内にあり、40 writersなど16を超える衝突もatomicで保護した。
active snapshotはrace-freeな複数relaxed loadの近似、全writer join後は正確。
global ID、treeのcorrectness counter、issued/max_inflight、全path prevalidationは維持。
ledger payloadは約3KiB増え、atomic回数そのものは消えない。主要速度改善が再現せず棄却。

[counter接続契約](phase4c_ledger_contract_20260905.md)には書込者・読取者・lifetimeと
`validate_quiescent_fast()`が旧世代のmap外live handleを検出する意味を記録した。

### 4C-2：小さなinline2＋unordered_set spill

inline IDは2件だけに抑え、root多数予約はspill側へ入れる。storageは56→80 byte/node。
global monotonic ID、move-only、二重/古いtoken拒否を保持し、inline枠が空いたときにも
spill側の同一IDを確認。上限超過のdropなし。poolやABA付き再利用は導入していない。

| PERF diagnostic | 基準 → 棄却4C-2 |
| --- | --- |
| primary allocation calls | 270,665 → 239,028 |
| primary allocated bytes | 92,543,090 → 92,190,066 |
| primary token allocation / rehash | 46,455 / 3,457 → 18,350 / 3 |
| 1T exact allocation calls（selected=114,554固定） | 420,309 → 329,160 |
| 1T exact allocated bytes | 96,491,742 → 94,714,286 |

primaryはS1なのでselectedも46,455→46,375と僅かに異なる。1Tは同じ処理量での確保減少。
正式primaryのnative RSS中央値は21,232→22,160 KiB、holdoutは21,272→21,626 KiBであり、
大きなRSS削減も得られていない。このbenchmarkの`rss_kind`は**current_resident_set**で、
peak RSSとは呼ばない。runner側RSSと混ぜない。単発batch64診断のRSS増もrawに残している。

### 4C-3：state_view＋selectionを同じlockへ

`inspect_or_select_bits`でExpandedの状態確認と元のPUCT処理を一つのcritical sectionへ統合。
world mask/feature/NN callbackはlock外、owner/waiter feature一致検査、状態機械、全path
事前検証を残し、float加算順・tie break・memory_orderを変えなかった。
v2は同期/非同期とも適用、v3は非同期workerだけに適用して補助関数をtemplate特殊化した。

primaryのnode lock取得は245,817→199,255、traversal rootは40,000→20,000へ減少（v3）。
一方v3の1Tはselected=114,554、node lock=518,193とも基準と同じ。処理数削減がない1Tの
実時間差にはcode layout等も関与し得るが、原因を確定したとは主張しない。
primaryの約4%改善は確認できても事前5%基準に届かず、局所call削減だけでは採用しなかった。

## 最終回帰と制約

- 最終Release native **39/39 PASS**。
- この作業ツリーで作ったPython extensionを実際にimportし、**565 passed / 1 skipped /
  4 performance deselected**。performance別実行は**4 passed**。package py_compileもPASS。
- 最終PERF ONでASan＋UBSan **8 suites PASS**、TSan **4 suites PASS**。
  depth TLS/atomic計測、ownership、並列stress、cancel/capacity/failure injectionを確認した。
  sanitizerは棄却試作ではなく、実際に残す診断コードを有効にしたもの。
- 各試作はRelease / PERF / reference OFFで**9 suitesずつPASS**。
  4C-1は40 writers・active snapshot・別thread RAII・旧世代live検出。
  4C-2は10万回のset differential・128 root予約・cross-thread commit/abort。
  4C-3は両backend状態機械・feature不一致拒否・256選択の数値一致・部分backprop禁止。
- 試作は性能gate未達なのでfull Python/sanitizer/広い正式matrixへは進めていない。
- hardware perfは`perf_event_paranoid=4`で**N/A**。権限設定を変更していない。

開始commit `2f567ba`と最終配置binaryの直接22pair比較も行った。8T primaryは
1.0024 [0.9722, 1.1466]。1T exactは初回0.9697 [0.9114, 0.9870]だったため独立に再測定し、
0.9950 [0.9749, 1.0099]となった。初回低下を削除していない。継続的な2%超回帰は再現して
いないが、2%以内の等価性を厳密に証明できる精度ではない。追加高速化の数字には用いない。
試作前の共通診断基準`0fa867d`とのRelease binary完全一致は別途確認済み。

失敗分類も保存した。4C-3 v1の新unitが既にExpandedのnodeをTerminalへ変えようとして失敗。
本来拒否される遷移なので、その拒否を確認し、別の未展開nodeでTerminalを検査するよう修正した。
既存8 suitesはその時点でもPASS。基準engineのfailureや候補の状態機械破損とは区別する。
hardware perfの環境failureを成功扱いせず、全nonzero invocationをmanifestに列挙した。

## 記録と再現

- [manifest](phase4c_evidence_20260906.json)：source/binary digest、全A/B、S1 quality、診断、
  全圧縮artifactのSHA-256。
- [実行script](phase4c_record_20260905.py)：既存paired runnerの薄いorchestration。
  `CSPLENDOR_4C_PHASE=final`、`CSPLENDOR_4C_VARIANT=v2`等の未使用名で
  `build / diagnostics / deploy / sanitizers`を実行できる。既存rawは上書きしない。
- 試作再現は共通基準`0fa867d`の**別worktree**で、
  `raw/phase4c/{4c1/v1,4c2/v1,4c3/v2,4c3/v3}/prototype_sources.json.gz`の
  `tracked_patch`と`new_files`を復元する。各source/binary digest付き。
  撤去後の本番で単にPHASEを指定しても試作は有効にならない。
- 対照worktreeの場所は`CSPLENDOR_4C_BASE_SOURCE`、buildは`CSPLENDOR_4C_BASE_BUILD`で指定。
  `smoke / formal / holdout`は保存済み計画に対応する。全matrix実行は今回行った範囲と区別する。
- [集計audit](phase4c_finalize_20260906.py)はsource保全・採否・ledger・test・artifactを再照合し、
  `--output 新規ファイル名`でmanifestを生成する。手書きの成功判定でrawを置換しない。
- 原依頼書は前Phaseの`raw/phase5br4b1/5br/v1/request.md.gz`を参照。記録は`doc/`配下に保存。

次の独立候補は**5D：V3 payment rank/unrankの静的composition DP**。
実際にV3を使う呼出し経路の確認から始め、48-action MCTSの改善と混同しない。
queue/worker poolは待機時間だけで主費用と断定できないので自動着手しない。
この3チケットで停止し、次の実装はユーザー指示を待つ。
