# 5B-R / 4B-1 事前計画

基準は3D-2/3採否評価済み `0f73b38241eaa54497d85ac0493e10acc4332f26`、
source digest `0a7f6d34f56e1d9738b52cdaaec03b1eebd096188a2dd629f8bd89d0a75b7e33`。
専用worktree `csplendor-mcts-state-records` / branch `perf/mcts-state-records`。
元repoのユーザー差分と旧worktreeを保持する。記録はdocへ保存。
4A-1/4A-2はユーザー判断で保留し、この作業でも変更しない。

## 5B-R

native parallelのworkerとdeterministic/caller search context、legacy native batchに
専有Gameを置く。rootのBoard copy-assignmentでprovenance capacityを再利用する。
初回だけclone_lightでNoInit生成し、Game(seed)の初期shuffleを追加しない。
全Board field、vector内容、raw hash/validity、両modeをresetし、journalsは空にする。
determinizationは既存RNG/seed/採番/順序を維持し、reset後に同じshuffleを行う。
native requestはowning feature/maskでありGame pointerを外へ渡さない。
Gameを保持できるPython callback版MCTSSearcherと公開clone APIは変更しない。
失敗時に例外を隠さず、次回resetで完全なrootへ復帰する。共有rootのcacheを新たに変更しない。

primary: parallel_scheduler / five_moves / exact / 1T / batch16 / 20,000 completed sims。
同じGame表現への単純なcopy-assignment再利用であり、成功条件は3%以上のE2E改善と
block CI下端>1、独立holdoutで再現。新しいrule state表現やdelta rollbackは導入しない。
guard: deterministic hidden-reserve、opening空provenance、batch1、retained tree、legacy、
root-parallel、shared throughput4T。2%超回帰はCI＋独立再測定で確認。
clone/reset microとallocation/retained capacityは診断であり、単独では採用理由にしない。

## 4B-1

5B-Rの採否を確定し、採用ならcommit＋clean sourceを別baselineへ固定する。
legacy nodes/aux/access mapだけを一つのunordered_map内のrecordへ統合する。
mutable node pointer/referenceをrehash越しに保持する公開意味、snapshotの非touch、
get/get_or_createのtouch、prune threshold/strict比較、counter reset、outstanding batchを維持。
dense node廃止、flat arena、VL式、backpropの追加lookup統合、4A系metadataは対象外。

primary: legacy_mcts / midgame_250 / exact / batch16 / 20,000 sims。
成功条件は5%以上のE2E改善または十分なRSS削減（20%以上を目安）、独立holdout、
他主要sliceの2%回帰guard。legacy determinization、retained tree、batch1、prune長期session、
mutable APIを書き換えた後の探索、node pointer安定性を確認する。
root-parallelはConcurrentTree経路であり、legacy統合の直接効果を主張しない。

### 5B-R採否確定後の4B-1開始条件

5B-R primary正式1.028079倍、独立holdout1.016616倍で事前3%基準に届かず棄却。
試作ソース・差分・失敗を含むrawは `raw/phase5br4b1/5br/v1` に保存し本番から撤去。
4B-1は同じ0f73b38をbaselineとし、5B-Rを組み合わせない。

## 共通

既存benchmark / semantic digest / paired runnerを再利用。最初は代表sliceだけで
unit→smoke→22pairs/11crossover blocks・warmup2・bootstrap10000→独立holdout。
portable Release、PERF/VERIFY OFF、1TはCPU4、複数threadは固定CPU集合、binary slot交差。
計時中はbuild/testを並走させない。diagnosticとdeployment timingを混同しない。
solver/決定的MCTSでは結果・root/tree/trace・選択手・leaf特徴等を一致させる。
非同期throughputでは完全tree digest一致を要求せず、完了数・評価数・path steps・
owner/waiter・停止理由とledger/VL回収を確認する。
採用候補はfull native/Python、ASan+UBSan、並列TSan/failure testsを実行する。
perf等の環境制約・基準にもあるfailure・候補固有failureを区別する。権限設定を変えない。
採用できなければ自分の試作だけ撤去し記録を残す。累積効果は直接A/Bし倍率を乗算しない。
最後に日本語commit＋作業branchへの通常pushまで行い、mainへ直接pushしない。
