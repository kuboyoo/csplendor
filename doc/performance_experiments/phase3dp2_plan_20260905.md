# 3D-P2 事前計画（2026-09-05）

基準は採用済み3D-P1 `ff8b1f646b91ae094e47fc6898fff214f9e596ee`。
元repoの未コミット変更は持ち込まず、専用worktree/branchで作業する。
3B sidecar、3C TT、3D-P1 score-once、rollback・上限・探索順序は維持する。

仮説: ordered actions と reveal IDs のvector capacityを再帰呼出しの
寿命ごとに再利用すると、固定node探索のallocationと経過時間が減る。
frameはstable address、RAIIで返却し、実際の入れ子数でgrowする。
bounded mate depthをframe上限としない。solver copyは独立scratchを持つ。
既存score-onceの小さな固定score配列は既にheap確保がなく、変更しない。
新しい2048 wide Action固定配列は導入しない。

## 事前に固定する評価

- primary: exact_reveal / hidden_reserve / depth 7 / 1,000,000 nodes の
  deployment Release E2E throughput。目標は5%以上、独立holdoutでも改善。
- guards: depth 3、persistent warm、visible_solver、forced-pass長経路、
  editor fallback、proof on/off、defender。2%超の悪化はCIと再測定で判断。
- reference: 上記commitのbuild/3dp1-release。candidateは同じRelease flags、
  instrumentation OFF。新しいscratch OFF経路もcorrectness referenceとして残す。
- 既存paired runnerを使用。CPU4、22 pairs/11 blocks、2 warmups、
  fixed-inode rotation、bootstrap 10,000。4-pair smokeは採否に使用しない。
- status/unknown、node/TT/legal/terminal counts、principal line、proof/frontier
  のsemantic digest一致。非同期MCTSには一致条件を拡張しない（変更対象外）。
- diagnostic buildのallocation/bytesとscratchの最大live depth・capacity・
  retained bytesを記録。初回とpersistent warmを分ける。
- fresh固定bufferとの比較はP1で採用済みのbounded score配列を維持し、
  可変長action/reveal IDsに巨大固定配列を置く場合のfootprintを併記する。
- 小テスト→smoke→formal/holdout→full native/Python、ASAN+UBSAN。
  hardware perfは権限不足ならN/A。権限設定を変更しない。

E2E改善が不十分ならprototypeを本番から撤去し、棄却理由・実測・patchを
doc下に残す。採否にかかわらずこのチケットで停止し、作業branchをcommit/push。
