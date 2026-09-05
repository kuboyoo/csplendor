# 4C ledger / reservation / lock の接続契約

2f567baの実コードを確認。4B-1はlegacyのみで、以下のConcurrentTree側には同等実装なし。
5B-Rは棄却済みであり、今回もGame scratch再利用を再導入しない。

| state | 書込者 / 読取者 | 今回の扱い |
| --- | --- | --- |
| ConcurrentTree generation / next_reservation_id / next_pending_id | worker・coordinator / 世代・一意性検証 | correctness、変更禁止 |
| ConcurrentTree live_reservation_count / virtual_loss_count | selection worker・commit/abort thread / quiescence | correctness、global atomicを保持 |
| evaluating_node_count / pending_evaluation_count | claim/publish/cleanup / quiescence | correctness、変更禁止 |
| node N / total_visits / availability / VL / live token集合 | node lock下 / selection・overflow・prevalidation | correctness、意味を保持 |
| ticket state / pending_id / issued, terminalizedのscheduler局所値 | coordinator・worker / scheduler | correctness、変更禁止 |
| SearchLedger issued / max_inflight_observed | coordinator / snapshot (C++直接アクセス実例あり) | 既存atomic表現を保持 |
| selected / virtual_loss_added | selection thread / 最終snapshot、trace | metrics。選択時にthread別へ記録可能 |
| evaluation_owner, expansion_claimed / evaluation_waiter, expansion_waited | worker claim/attach / 最終snapshot、trace | metrics。writer別へ分散可能 |
| virtual_loss_released / reservations_committed, reservations_aborted | commit/abort実行thread (元workerとは限らない) / snapshot | metrics。書込時のlaneを使い、元workerのplain counterへ書かない |
| evaluation_requested / evaluated_boards / expansion_published | coordinator publish / snapshot | metrics。正常終了・重複評価診断用 |
| completed_evaluated / completed_terminal / completed_max_depth / cancelled / failed | coordinator / join後ledger整合性検証・result | metricsだが正確な最終集計は必須 |
| stale_result / duplicate_result / invalid_replay / integrity_errors | 検出thread / snapshot | error metrics。検出時の例外や停止はcounterとは別に維持 |

SearchLedgerは探索中の分岐・ID付与・overflow予約数の根拠ではない。
join後に `issued = completed + cancelled + failed` とVL balanceを検査する意味は保持する。
snapshotは現行から複数relaxed loadであり、active中の複数counter間の一貫性は保証されない。
分散後もatomic loadによるrace-free観測、quiescence後の完全な集計を要求する。

`validate_quiescent_fast()`はtree所有のglobal countersを読み、古いgenerationとしてmapから
外れたlive handleも検出する。snapshot_allだけで代替しない。予約はtree/node/ledgerを共有所有し、
controller破棄後や別threadのdestructorでも有効。TLSは所有者pointerを持たない。

reservationはnode内unordered_setでglobal monotonic IDを検証し、move-only tokenが保持する。
`ReservedPath::commit`は全entryのgeneration/token/value/playerを事前検査してから逆順backprop。
4C-2の表現変更でもこの走査を省かず、overflow用live count、二重消費拒否、旧世代abortを維持する。

4C-3はtraversalのstate_viewと選択の二重lockが対象。
feature/maskはGameに依存しnodeの保護対象ではない。feature生成/推論をlock中に移さず、
Unexpanded/Evaluatingでのclaim/attachとfeature digest照合、Expanded/Terminalへの再判定を維持。
float演算順、tie break、全path検証、queue/CV、memory_orderは別変更とする。
