# 第2次リファクタリング R5 MCTS ownershipとorchestration

## 目的とstable gate

公開`mcts.h` facade、`MCTS`が所有する逐次／並列tree、逐次`MCTSSearcher`、parallel
schedulerの挙動を変えず、探索session固有resourceの所有者とconfig validationの境界を
明確にした。

parallel search planのPS-13 stable gateは未完了である。実NN、fixed-time品質canary、広い
seed/host matrixは本repository外の運用確認を含むため、本フェーズではscheduler semanticsの
統合、複数threadの既定化、experimental APIの昇格を行わない。R5はstable gate前に許される
機械的な責務分離として完了させる。

## 責務と所有権

| component | 所有するもの | 終了責務 |
|---|---|---|
| `mcts.h` | 旧公開include surface | `MCTS`と逐次orchestrationを公開する |
| `MCTS` | legacy tree、parallel tree、config snapshot、generation、search identity | `SearchGuard`で同一ownerのsearchを直列化する |
| `MCTSSearcher` | 逐次探索の呼出し制御 | 従来のtree ownerを利用する |
| `ParallelSessionController` | worker、work/event queue、active ticket、pending registry、failure slot、ledger | stop、queue close、全worker joinを必ず行う |
| `MCTSConfigValidator` | parallel実行profileの制約 | tree mutation前に不正値を拒否する |
| `ParallelMCTSSearcher` | 選択、評価batch、publish、集約semantics | controllerのresourceを使いcleanup後に結果／例外を返す |

`MCTSConfig`はbinding互換のpassive value typeのままにした。validatorはNaN/inf、負の探索数、
Dirichlet範囲、hidden-informationのworld数を従来と同じ例外型・messageで拒否する。

controllerのdestructor fallbackは、通常完了だけでなくworker生成途中、callback例外、cancel、
timeout、tree capacity、cleanup検証例外でもstopを通知し、queueを閉じてjoinable threadを残さない。
通常経路では既存と同じ順序でworkerをjoinした後、pending、ticket、virtual lossをcleanupする。

## 固定した互換契約

- 同一seedの選択列、deterministic trace bytes/digest、root statistics、結果を変更しない。
- throughput、deterministic epoch、root-parallelの処理を共通化しない。
- compact edge、bitset、packed/search copyを変更しない。
- legacy batch APIとPythonのexperimental/public分類を変更しない。
- callbackの同期性、soft timeout、cooperative cancellation、例外再送出を変更しない。
- 複数threadはexperimental opt-in、既定は1 threadのままとする。

## 同値性とlifecycle検証

`mcts_ownership_unit`はtree/session ownerがcopy/move不能であること、configの全境界、明示stop時の
queue drain、event queue close、worker join、destructor fallback、worker共有ledgerのlifetimeを
検証する。

既存suiteはさらに次を固定する。

- deterministic traceのtoolchain別golden byte列とreplay digest
- 1/2/4/8 thread、coarse/sharded間のdeterministic結果一致
- callback/worker exception、cancel、timeout、capacity後の再利用
- pending/ticket/virtual-loss ledgerのbalance
- root snapshot、config/generationのAPI entry linearization
- throughput stress、TSan、ASan/UBSan

## 性能

2026-08-05、Ubuntu x86_64、GCC 13.3、portable Release、seed 42、batch 16で、R4 mainと
候補を交互に5組測定した。各組は32,768 simulationを3 sample実行し、組内中央値のさらに
中央値を比較した。raw出力は`/tmp`にのみ保持する。

| coarse shared tree | R4 main | R5 | 比率 |
|---|---:|---:|---:|
| 1 thread NPS | 210,283 | 210,716 | 1.002 |
| 4 thread NPS | 93,738 | 103,656 | 1.106 |

throughput modeの4 threadではinterleavingによりtree sizeとwaiter数が変動するため、改善値を
最適化効果とは扱わない。1 threadの探索統計は完全一致し、NPS回帰は検出しなかった。
4 thread・32,768 simulationを`/usr/bin/time`で補助測定したpeak RSSは36,760 KiBから
36,660 KiB、wall timeは0.35秒から0.36秒であり、測定揺らぎの範囲である。

## 検証

- Python全test、performance test、`py_compile`、ruff
- GCC / Clang Release native test
- ASan/UBSan、TSan native test
- GCC / Clang strict binding build
- deterministic trace/replay golden、parallel stress、ownership unit
- paired MCTS NPS/RSS benchmark、clang-format、`git diff --check`
- GitHub ActionsのUbuntu/macOS/Windows、Python 3.8〜3.12、portable/native matrix

R5の責務境界整理は完了した。stable rolloutは別のPS-13運用gateとして追跡し、次の独立変更は
R6のsolverとpuzzle tooling分割とする。
