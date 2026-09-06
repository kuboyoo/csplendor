# 共通要件 §19–22：最終横断監査

## 範囲と採否の継承

5C-B完了 `3582909` までの採用コードを基準とする。元worktreeの別系統
`5f29b50`（遅延詰め探索の修正）は未統合であり、その変更を今回検証済みとはしない。
作業中にこの別系統は独立に`3d9dcf2`へ進んだ。Phase 6は元worktreeに書込みを行わず、
開始時/観測時の両source manifestを保存した。「元worktreeの全内容が開始時と一致」とは主張しない。
過去Phaseの速度は各報告の当時の実測であって、現行全体速度や乗算可能な値ではない。

| Phase | 判断を引き継ぐ内容 | 根拠（doc/performance_experiments/） |
|---|---|---|
| 0 | ACCEPT：既存計測・paired/digestを再利用 | baseline_20260902.md |
| 1A / 1B | exact hash採用 / observable cache棄却 | phase1a_exact_hash_20260902.md / phase1b_observable_hash_20260902.md |
| 2A | noble mask採用 / packed資源差分棄却 | phase2a_noble_mask_20260902.md / phase2a_packed_resources_rejected_20260902.md |
| 2B | single-pass、return count/table、packed sink採用 / purchase DP/filter棄却 | phase2b_*_20260902.md |
| 3A | bounded path、class集合、reason/forced action圧縮採用 / map単回lookup棄却 | phase3a_solver_containers_20260902.md |
| 3B | 増分sidecar採用、scan fallback維持 | phase3b_incremental_reveal_state_20260905.md |
| 3C | 用途別key/entry圧縮採用、unordered_map維持。flat TTは未着手 | phase3c_solver_tt_compaction_20260905.md |
| R0 | ローカル成果物・接続契約の照合完了 | post3c_review_baseline_20260905.md |
| 3D-P1/P2 | score一回計算・再帰scratch採用 | phase3dp1_score_once_20260905.md / phase3dp2_search_scratch_20260905.md |
| 3D-1 | visible通常手rollback採用、reveal全面compact案棄却 | phase3d1_normal_rollback_20260905.md |
| 3D-2/3 | 任意deck復元案棄却 / prefixは費用条件未達で本番未着手 | phase3d23_reveal_transactions_20260905.md |
| 4A-1/2 | ユーザー指定で保留。完了扱いにしない | 対話指示、phase4c_concurrency_20260906.md |
| 5B-R / 4B-1 | scratch Game棄却 / legacy record統合採用 | phase5br4b1_mcts_state_records_20260905.md |
| 4C-1/2/3 | 全試作棄却、PERF専用診断を保持 | phase4c_concurrency_20260906.md |
| 5D | V3 DP棄却、網羅oracle/benchmark保持 | phase5d_v3_payment_20260906.md |
| 3E / 5E / 5A | take代表化 / rank選択採用、48直接適用棄却 | phase3e5e5a_action_selection_20260906.md |
| 5C-B | owning NumPy採用、固定特徴量表棄却 | phase5cb_features_20260906.md |
| 6 | ビルド監査・独立比較・最終検証 | phase6_build_profiles_20260906.md |

棄却は試作後撤去、未着手は条件未達、保留はユーザー判断であり、同じ意味にまとめない。

## §19：測定・正しさ

- 過去のpaired runner・固定inode crossover・block bootstrapを維持。Phase 6のみ
  `--build-profile-axis cpu|lto|pgo` を明示指定し、当該option以外のflags、compiler、
  sanitizer/verify、argv、論理counter、semantic gateは従来どおり厳格に照合する。
- Release（採否）、PERF（機構診断）、reference/ASAN/TSan（正しさ）を混同しない。
  PGO generateは学習専用で、速度表に候補として載せない。
- 固定順solverとdeterministic epochはS0。node数・合法手数・TT hit・主手順・proof・
  digestを一致させる。時間/node上限のUNKNOWNを不詰み証明や既知7手詰め完了としない。
- shared throughputはS1。root/tree digest完全一致を要求せず、実completedを分子とし、
  issued/completed/cancelled/failed、VL・reservation回収、owner/publish・stop reasonを検査。
  phase4cの全試作が棄却された事実と、その試作で行ったfailure injection記録を保持する。
- numerical S2案（fast-math、加算順変更等）は採用しない。浮動小数の丸め・FMA等は
  compiler設定だけでも変わり得るので、native/LTOもS0の自動免除にしない。
- 全体テストは累積採用コード上で実行。正式結果はPhase 6報告とmanifestに収録。

## §20：経路別matrixと未計測

全backend×thread×fixtureの直積は再実行しない。過去の該当matrixは各報告・圧縮rawへリンクし、
今回は最終portable/native-profile候補、数値oracle、wheel、PGO holdoutを重点にする。

- native48：legacy、shared、root-parallelは別経路。root-parallel workerはConcurrentTree/
  DeterministicEpoch 1Tであり、legacy record統合の効果を直接外挿しない。
- V3：3133 IDのcodec/mask/公開binding。native48の速度と混同しない。
- Python：StateFeaturizerは5C-Bのowning NumPyを使用。canonical/public statisticsは旧経路維持。
- NN：repo内benchmarkは模擬評価器。外部Genbu、実NN、GPU、棋力、問題保存速度は未測定。
- memory：PERF allocation/累計bytesと、live/current/peak RSS、保持capacityを別記する。
  compiler profileにより論理cache上限を変更しない。Phase 3CのRSS減と4B-1の大量未展開node
  約18.6% RSS増という既知tradeoffも抹消しない。

## 3B/3C・rollback・公開契約

`src/`本体の変更をしないことで、以下のローカル契約を維持する。

- RevealSearchStateはsolver所有sidecar。残り集合・hidden獲得集合・claimed・rule/deck hash・
  activeをBoardと同じscopeで復元。非canonical editor/不正前提はscanへfallback。
- TTはfull-key equalityを行うunordered_map。64bit board hashの数学的無衝突は主張しない。
  rehashは要素参照を維持するがerase/clearは別。再帰途中にtrim/eraseしない。
- exact cacheはsolve前後trimであって、探索中のhard RSS capではない。warm/cold、
  attacker/mode、depth、generation/touch・eviction規則を維持する。
- rollbackは公開Gameのundoやwire formatを変更しない。深さ別scratchの非aliasと
  false/throw/cancel/例外巻戻しは既存oracle/失敗注入テストで確認する。
- mutable MCTSNode pointer、合法手順序/上限、hidden秘匿、owning配列、finite/overflow検査は維持。

## §21/22：停止条件・記録

棄却案の無根拠な復活、独自TT、守備応手間引き、deck swap-remove、数値規則変更は行わない。
hardware perf/TSan/Apple実機/NNなどの環境制約・未測定はPASSに置き換えない。
報告はbaseline/candidate digest、変更symbol、A/B/CI/holdout、memory、機構、制約、採否を含む。
記録はdoc下へ圧縮・SHA付きで保存し、build/profile/wheel/モデル/秘密情報はコミットしない。
最新ユーザー指示に従い作業ブランチのみ通常push。次のticketには自動で進まない。
