# PS-12 parallel search benchmark結果

計測日: 2026-07-14

## 1. 結論

固定実機のnative fake-inference workloadでは、sharded shared-treeの1 threadはlegacy比で
determinization offが9.6%低速、onが1.2%高速となり、受入条件の10%以内だった。同じsharded
経路の1 thread比では次のscalingとなった。

| Determinization | 2 threads | 4 threads | 8 threads |
|---|---:|---:|---:|
| off | 1.818倍 | 3.304倍 | 3.989倍 |
| on, 1 world | 1.778倍 | 2.844倍 | 3.731倍 |

determinization off/onとも2/4/8 threadの暫定目標1.5/2.4/3.6倍をすべて満たした。shardedは
2 thread以上でcoarse backendを一貫して上回るため、shared-treeのexperimental backendとして
採用する。既定値は引き続き1 threadである。

この結果はNN推論を含まないsearch-core benchmarkであり、棋力や実NN込みend-to-end高速化を示す
ものではない。実NN、GPU、fixed-time探索品質/self-playはrepo外のmanual canaryを通してから
stable化を判断する。

## 2. 測定条件

```text
CPU:             AMD Ryzen 9 7900X
affinity:        logical CPU 0-7へ固定
compiler:        GCC 13.3
build:           Release
fixture:         seed 42から決定的に12 ply進めたmidgame、searchごとにnew tree
simulations:     4,096 / sample
samples:         15（表はmedian）
batch:           16
added latency:   0 us / batch
inference:       native固定policy/value callback（外部NNなし）
Dirichlet noise: off
determinizations: 1 world
threads:         1, 2, 4, 8
backends:        legacy / coarse shared / sharded shared / root-parallel
```

実行形式は次のとおりである。`$BENCH`はRelease buildした`benchmark_mcts_parallel`を指す。

```bash
taskset -c 0-7 "$BENCH" \
  --threads=1,2,4,8 --simulations=4096 --samples=15 \
  --batch=16 --latency-us=0 --seed=42 --backend=all --determinization=0

taskset -c 0-7 "$BENCH" \
  --threads=1,2,4,8 --simulations=4096 --samples=15 \
  --batch=16 --latency-us=0 --seed=42 --backend=all --determinization=1
```

`speedup`は各backendの1 thread中央値に対する倍率、`efficiency`は`speedup / threads`、
`vs legacy-1`は同じdeterminization条件で同時測定したlegacy 1 threadに対する倍率である。

## 3. Determinization off

| Backend | Threads | Median simulations/s | Speedup | Efficiency | vs legacy-1 |
|---|---:|---:|---:|---:|---:|
| legacy | 1 | 45,974.56 | 1.000 | 100.0% | 1.000 |
| coarse | 1 | 41,633.65 | 1.000 | 100.0% | 0.906 |
| coarse | 2 | 66,191.86 | 1.590 | 79.5% | 1.440 |
| coarse | 4 | 71,381.83 | 1.715 | 42.9% | 1.553 |
| coarse | 8 | 99,866.21 | 2.399 | 30.0% | 2.172 |
| sharded | 1 | 41,557.25 | 1.000 | 100.0% | 0.904 |
| sharded | 2 | 75,559.86 | 1.818 | 90.9% | 1.644 |
| sharded | 4 | 137,285.24 | 3.304 | 82.6% | 2.986 |
| sharded | 8 | 165,779.68 | 3.989 | 49.9% | 3.606 |
| root-parallel | 1 | 37,822.65 | 1.000 | 100.0% | 0.823 |
| root-parallel | 2 | 82,679.13 | 2.186 | 109.3% | 1.798 |
| root-parallel | 4 | 175,261.74 | 4.634 | 115.8% | 3.812 |
| root-parallel | 8 | 144,003.90 | 3.807 | 47.6% | 3.132 |

shardedはcoarseに対し2/4/8 threadでそれぞれ約1.14/1.92/1.66倍となった。coarseは4 threadで
頭打ちとなり、8 thread値にもsample間の揺れがある。global mutex競合と整合する結果だが、
lock-wait profileを伴う直接の原因測定ではない。shardedは8 threadでも49.9%のparallel
efficiencyを維持した。

## 4. Determinization on（1 world）

| Backend | Threads | Median simulations/s | Speedup | Efficiency | vs legacy-1 |
|---|---:|---:|---:|---:|---:|
| legacy | 1 | 60,508.96 | 1.000 | 100.0% | 1.000 |
| coarse | 1 | 60,959.43 | 1.000 | 100.0% | 1.007 |
| coarse | 2 | 84,231.04 | 1.382 | 69.1% | 1.392 |
| coarse | 4 | 103,442.57 | 1.697 | 42.4% | 1.710 |
| coarse | 8 | 115,870.38 | 1.901 | 23.8% | 1.915 |
| sharded | 1 | 61,221.92 | 1.000 | 100.0% | 1.012 |
| sharded | 2 | 108,844.04 | 1.778 | 88.9% | 1.799 |
| sharded | 4 | 174,099.68 | 2.844 | 71.1% | 2.877 |
| sharded | 8 | 228,426.81 | 3.731 | 46.6% | 3.775 |
| root-parallel | 1 | 53,263.13 | 1.000 | 100.0% | 0.880 |
| root-parallel | 2 | 118,768.24 | 2.230 | 111.5% | 1.963 |
| root-parallel | 4 | 239,708.81 | 4.500 | 112.5% | 3.962 |
| root-parallel | 8 | 173,008.47 | 3.248 | 40.6% | 2.859 |

shardedはcoarseに対し2/4/8 threadで約1.29/1.68/1.97倍となった。8 threadの3.731倍は暫定
3.60倍目標を満たし、legacy 1 thread比では3.775倍である。

determinization on/offの絶対値は探索木形状や評価dedup件数も変わるため、互いを高速化倍率として
比較しない。採用判断には、それぞれの条件内で同じbackendの1 thread比を使う。

## 5. 250µs latency補助slice

単一inference coordinatorの挙動を確認するため、sharded・determinization offだけを
1,024 simulations、7 sample、batch 16、callbackごとに250µs待機で追加計測した。

| Threads | Median simulations/s | 1 thread比 |
|---:|---:|---:|
| 1 | 23,888.74 | 1.000 |
| 2 | 45,550.25 | 1.907 |
| 4 | 45,570.23 | 1.908 |
| 8 | 43,106.79 | 1.804 |

2 threadでほぼ飽和し、4/8 threadを増やしても改善しなかった。これはcallback待ちを単一
coordinatorが直列処理する現行設計と整合する。実NNではbatchあたりlatency、batch充足率、GPU
並列性が異なるため、この補助sliceだけでthread数を決めない。

## 6. Backend判断

### Coarse shared tree

correctness・TSAN oracleとして維持する。global mutex競合による4/8 threadの頭打ちが明確なため、
性能backendの既定候補にはしない。

### Sharded shared tree

2 thread以上でcoarseを上回り、1 thread overheadも受入範囲内である。shared-tree experimental
backendとして採用する。両条件で暫定scaling目標を満たしたが、実NN canary前にvirtual lossや
shard数をこのmicrobenchmarkだけに合わせて変更しない。

### Root-parallel

4 threadで最高のraw throughputを記録した一方、両条件とも8 threadで4 threadより低下した。
また独立treeへbudgetを分割するため、shared-treeと探索内容、重複評価、memory、root統合後の品質が
同一ではなく、superlinear値もそのworkload差を含む。したがってcorrectness oracleとfallbackとして
残すが、raw simulations/sだけでshared-treeの代替既定にはしない。

## 7. Gate判定と残作業

| Gate | 結果 | 判定 |
|---|---|---|
| shared path 1 threadがlegacy比0.90以上 | 0.904〜1.012 | pass |
| sharded 2 threadが1.50倍以上 | 1.778〜1.818 | pass |
| sharded 4 threadが2.40倍以上 | 2.844〜3.304 | pass |
| sharded 8 threadが3.60倍以上 | off 3.989 / on 3.731 | pass |
| shardedがcoarseより改善 | 2/4/8 threadで1.14〜1.97倍 | pass |
| normal/TSAN/ASan+UBSan/Python integration | 対象test成功 | pass |
| 実NN end-to-end、fixed-time品質 | repo外model/runnerが必要 | manual gate |

この測定は4,096 simulations、batch 16、latency 0、単一fixtureに限定される。
250µsのsharded補助slice以外の50/1000µsおよび全backend latency matrix、batch 1/64、
65,536 simulations、250-action/reveal-heavy fixture、RSS、実NN/GPU utilization、paired self-playは
stable rollout前のmanual canary matrixとして残る。

過去の[PS-0 baseline](baseline_results.md)は256 simulations等、今回とtree成長条件が異なるため、
94,427/108,441 simulations/sという絶対値と直接比較しない。今回の性能判定は同じ実行内のlegacy
baselineとのA/Bだけを使用する。
