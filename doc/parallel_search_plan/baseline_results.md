# PS-0 baseline results

計測日: 2026-07-13
状態: PS-1着手前のsingle-thread baseline

## Environment

```text
CPU: AMD Ryzen 9 7900X 12-Core Processor
physical cores / threads: 12 / 24
affinity: CPU 0
compiler: g++ 13.3.0
language/build: C++17, -O3, -DNDEBUG
benchmark source: scripts/benchmark_mcts_native.cpp
samples: 15
leaf iterations/sample: 1000
synthetic searches/sample: 10
simulations/search: 256
batch size: 16
inference: fixed native C++ arrays
Dirichlet / forced playouts: off / off
```

Command:

```bash
c++ -std=c++17 -O3 -DNDEBUG -pthread -Isrc \
  scripts/benchmark_mcts_native.cpp -o /tmp/csplendor_mcts_baseline
CSPLENDOR_BENCH_RAW=1 taskset -c 0 \
  /tmp/csplendor_mcts_baseline 15 1000 10 \
  >/tmp/csplendor_mcts_baseline.csv \
  2>/tmp/csplendor_mcts_baseline.raw
```

## Throughput

| Metric | Determinization | Median simulations/s | Mean simulations/s |
|---|---:|---:|---:|
| leaf preparation | off | 3,398,060.01 | 3,384,078.72 |
| leaf preparation | on | 704,930.19 | 704,373.56 |
| synthetic search | off | 92,800.63 | 92,777.57 |
| synthetic search | on | 106,778.86 | 106,866.17 |

Summary CSV SHA-256:
`f0d61aa84f4be6c6348d8cf80874061a37a33111a5c55221d6341ce790925bd5`

Raw sample SHA-256:
`a442f51e576d42d58d2f6e57fe8839a20b70a626a5db7693ed9c209ec4e57a9c`

Raw ratesはmetricごとにsample 0〜14の順で次のとおり。

```text
leaf_prepare/off:
3349222.60 3440223.96 3398060.01 3439668.54 3319882.97
3295807.14 3446973.59 3398646.83 3427774.96 3395582.14
3411424.39 3341919.98 3404943.55 3358739.40 3332310.73

leaf_prepare/on:
693886.02 707229.40 704230.47 698120.52 706725.49
705529.40 703098.48 704930.19 707845.36 704232.95
703650.54 707389.46 699569.80 706962.65 712202.62

synthetic_search/off:
92040.35 93062.06 92549.68 93365.66 92706.45
93042.68 92800.63 92499.02 92880.85 92283.18
93455.98 93205.79 92315.69 92920.09 92535.50

synthetic_search/on:
107531.41 106778.86 106562.92 107451.05 106538.31
107057.22 106590.80 107111.83 106196.57 107333.16
106407.38 106756.15 107308.00 106416.47 106952.36
```

## Correctness oracle

PS-0時点で以下を固定した。

- `tests/test_mcts_contracts.py`のdeterminization-off batch digestと、既知hash集合をsortした
  test-only tree snapshot digest。
- determinization offのroot/path/mask/feature goldenと、determinization on/world=1のinvariant契約。
  現行RNGは`random_device`初期化のため、determinization-onのbitwise goldenはPS-3で追加する。
- terminal、MAX_DEPTH、forced playout、callback exception時の既存契約。
- hidden reserve tierがfeature差を持つ一方でobservable hashをaliasする再現fixture。
- reserve-visible後の20 worldが複数の公開featureへ分岐してもworld 0 hashへ集約されるfixture。
- 同一information set内でreserved purchase availabilityがworldごとに異なるfixture。
- unavailableなreserved purchaseを選んでdraw visitへ誤計上するfixture。

関連test command:

```bash
python -m pytest -o addopts= -q \
  tests/test_mcts_contracts.py \
  tests/test_mcts_correctness_review.py \
  tests/test_determinization.py \
  tests/test_hash_mutation_review.py \
  tests/test_parallel_mcts_characterization.py
```

## Native race probe specification

TSAN導入後、同一`MCTS`に対して次をbarrier同期して衝突させる。

1. 同一root edgeの`select_action_with_virtual_loss()`と`add_virtual_loss()`。
2. 同一leafの`expand_node()`。
3. map rehash中の`get_node()` / `get_or_create_node()`。
4. active search中の`clear()`、config変更、snapshot取得。
5. completion逆順、callback failure、cancel、stale generation。

PS-0では未同期legacy実装をTSAN必須testへ入れない。PS-7のcoarse shared tree導入後に、同じ
scenarioをrace oracleとして有効化する。
