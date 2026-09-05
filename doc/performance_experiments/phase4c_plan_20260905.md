# 4C-1 / 4C-2 / 4C-3 事前計画

開始基準: 2f567ba77cff678d8fad0492f992d110aa67e7f8 (4B-1採用、5B-R棄却)。
source c45073b99c587aff12a95331665476a4d01c91a2fb28e3c4ed87174951740174。
元csplendorのdirty worktree、既存worktrees、0〜3D/4B採用コードを保持する。
branch perf/mcts-concurrency。4A-1/4A-2は保留のまま。

まずPERF限定のtraversal depth別lock/occupancy診断を既存計測へ追加し、
root/depth1/deepを観測する。coordinatorのcommit/cleanupは既存集計に残し、depth不明を
rootへ混ぜない。observer effectを許容する診断であり、診断時間を本番内訳と解釈しない。

3チケットは独立に実装・採否判断し、採用したものだけ次のbaselineへ含める。
4C-1: metrics-onlyをwriter別atomic lanesへ分散し、quiescence時snapshotで集約。
TLSに残すのはlane番号だけ。cross-thread RAIIは実際の書込threadのlaneを使い、
共有所有のledger自体がlane lifetimeを持つ。active snapshotはrace-freeなrelaxed近似、
quiescence後は正確。制御用tree live/VL/pending/evaluating/ID/generation counterと
既存の全path事前検証は変更しない。issued/max-inflightの既存直接アクセスも維持する。

4C-2: 現行unordered_setとの比較で、小さなinline token領域＋spillを検証。
root高占有ではspillし、dropしない。global ID・move-only・stale検証・全path事前検証を維持。
pool/ABA管理を新規導入する前に、観測された占有とallocationに対する最小案を試す。

4C-3: Expandedのstate確認とselectionを一度のlockに統合。
特徴/合法mask生成・NN callbackはlock外。Unexpanded/Evaluating/Terminal、late attach、
owner/waiter feature照合とcancel/overflow/部分backprop禁止を維持。数値演算順は変更しない。

primary (各チケット): shared throughput / sharded / hidden_reserve / determinization /
8T / batch16 / latency0 / 20,000 completed sims。全チケットとも5%以上の改善、
block CI下端>1、独立holdoutを採用条件とする。微小/局所改善だけでは採用しない。
guard: 1T決定的exact/observable、4T/16T、batch1/64、latency250us、retained tree、
coarse backend、root-parallel、legacy。全直積にせず段階的に代表sliceを選ぶ。
主要workloadの2%超回帰はCIと独立再測定で判定する。

既存paired runnerのfixed-inode crossover / ABBA / 22pairs・11blocks・bootstrap10000 / warmup2。
portable Release、PERF/VERIFY OFF。CPU4起点の固定cpuset。計時中はbuild/testを並走させない。
S0は決定的trace/float/ledger、S1はcompleted/evaluated/path steps/owner-waiter/stop reasonと
予約回収をgateとし、非同期のroot/tree digest完全一致を要求しない。
採用候補にfull native/Python、ASan+UBSan、TSan、race/failure injectionを実施。
実NN不使用を明記し、棋力改善を主張しない。perf不可ならN/A、権限設定は変更しない。
最終統合は共通基準と直接A/Bし、倍率を乗算しない。棄却試作とrawはdocへ保存して撤去。
最後に日本語commitと作業branchへの通常push。mainへの直接push、force、stash/resetは禁止。
