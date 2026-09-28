# 実験的な並列MCTS

[README](../README.md)の探索APIの補足です。


共有tree並列探索はStage Bのexperimental opt-inです。既定の`num_threads=1`はworker queueを
作らない低overheadなserial pathで、`num_threads>=2`のときだけnative traversal workerと単一の
inference coordinatorを使います。Python evaluator callbackは常に同期的・非並行に呼ばれます。

```python
import numpy as np
import csplendor as cs

game = cs.Game(seed=42)
mcts = cs.MCTS(cs.MCTSConfig())

options = cs.ParallelSearchOptions()
options.num_threads = 4
options.num_simulations = 800
options.max_tree_nodes = 50_000
options.tree_backend = cs.ParallelTreeBackend.SHARDED
options.mode = cs.ParallelSearchMode.THROUGHPUT
options.search_nonce = 1

def evaluator(requests):
    results = []
    for request in requests:
        policy = request["valid_actions"].astype(np.float32)
        policy /= policy.sum()
        results.append({
            "policy": policy,
            "value": np.zeros(2, dtype=np.float32),
        })
    return results

result = cs.mcts_search_parallel_native(
    mcts, game, options, evaluator, 1.0
)
```

`DETERMINISTIC_EPOCH`は単一coordinatorがtraversal、callback、commitを決定順で実行する
trace/replay oracleです。このmodeの`num_threads`は結果互換性の入力であり、並列completionの
reorderを発生させません。root-parallel APIで正の探索budgetを使う場合は、workerのseed範囲を
固定する明示`search_nonce`が必須です。また`timeout_ms`はcallback境界で観測するsoft timeoutで、
block中のevaluatorを強制中断しません。

`max_tree_nodes`の既定値50,000はshared-treeでは単一tree上限、root-parallelでは全active worker
treeの合計上限です。capacity到達後もrootが展開済みならpartial resultを返し、visitが0の場合は
legal action上で正規化したprior（設定時はroot noiseを混合）を使います。root未展開なら
`TreeCapacityReachedError`です。Python root-parallel callbackは直列化され、mutex待機後にも
timeout/cancelを再検査するため、期限切れのcallback backlogを流しません。

複数threadをstable/defaultへ昇格するには、scheduled sanitizer/soak、可変scheduler seed、
実NN、fixed-time探索品質に加え、展開済みnodeの二次feature signature照合gateが残っています。
現在のfeature digest検査は同一pendingへdeduplicateされたowner/waiter間です。問題時はlegacy API
または`num_threads=1`へ戻せます。
詳細は[並列探索の実装状況](parallel_search_plan/implementation_status.md)を参照してください。
