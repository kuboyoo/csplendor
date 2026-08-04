# MCTSホットパス高速化

最終更新: 2026-08-04

MCTSで支配的だった48手encoder、合法手mask走査、並列木の終了監査、node容量を
高速化した実装と検証方法を記録します。Python bindingと公開C++ DTOは従来どおり
48要素配列を返し、変換は探索境界だけで行います。

## 実装

1. `ActionEncoderCpp`は到達可能局面の48手maskを局面から直接生成し、decodeも選択された
   policy slotのcanonical actionだけを構築します。全合法手の列挙と43 KiBの`MoveList`生成は
   MCTSホットパスから外れました。2048手上限や重複cardを含む公開editor局面は、旧列挙へ
   自動的にfallbackします。
2. 並列木はreservation、virtual loss、evaluating node、pending evaluationをtree全体の
   atomic counterでも追跡します。Releaseの探索境界では`validate_quiescent_fast()`を使い、
   全nodeを調べる`validate_quiescent_full()`はDebug buildで自動使用します。Releaseでも
   明示的なtest/debug監査として呼び出せます。
3. MCTS内部の合法手maskは下位48 bitを使う`uint64_t`です。公開DTO、inference request、
   Python NumPy配列だけを48 byteのdense maskへ変換します。選択はset bitだけを昇順走査します。
4. 並列木の統計は48本の固定配列ではなく、information-set unionに現れた合法edgeだけを
   `EdgeStats64`へ保存します。公開snapshotは従来のdense DTOへ復元します。直列探索は公開
   `MCTSNode`のABIを維持し、world-local availability sidecarをcompact edge化しました。

## 同値test

`mcts_optimization_unit`は次を確認します。

- 到達可能な1,000局面以上について、旧maskと直接mask、および全48 policy slotの旧decodeと
  公開decodeが一致すること。全合法slotではtrusted decodeも比較し、2048手上限と重複cardの
  editor局面も含みます。
- 16 bitの全65,536 patternを3区間で調べ、dense maskとbitsetの集合、昇順、popcountが
  一致すること。
- reservation、abort、commit、evaluation publish/failureの各状態で、O(1)監査と完全監査の
  quiescence判定が一致すること。完全監査だけが一般統計破損も検出すること。
- 48 actionすべてをinformation-set unionへ順次追加し、compact treeの選択、N/Q、availability、
  snapshotが旧dense reference modelと各手で一致すること。

```bash
cmake -S . -B build/native \
  -DCSPLENDOR_BUILD_PYTHON_MODULE=OFF \
  -DCSPLENDOR_BUILD_NATIVE_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/native -j
ctest --test-dir build/native --output-on-failure
```

## ベンチマーク

専用microbenchmarkは次のコマンドで再現できます。

```bash
cmake -S . -B build/benchmark \
  -DCSPLENDOR_BUILD_PYTHON_MODULE=OFF \
  -DCSPLENDOR_BUILD_PARALLEL_BENCHMARK=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/benchmark -j
./build/benchmark/benchmark_mcts_optimizations
```

Ubuntu x64、Ryzen 9 7900X、GCC 13、portable Release buildでの代表値です。

| 対象 | 旧実装 | 新実装 | 改善 |
|---|---:|---:|---:|
| MCTS action mask | 605 ns | 33.3 ns | 18.14倍 |
| MCTS action decode | 3,111 ns | 14.1 ns | 220.97倍 |
| dense/bitset mask走査 | 19.5 ns | 4.2 ns | 4.62倍 |
| 20,000 node完全/O(1)監査 | 20.5 ms | 0.55 ns | node数非依存化 |
| 20,000 node dense/compact payload推定 | 34.33 MiB | 17.09 MiB | 2.01倍縮小 |

zero-latency native evaluatorを使った`origin/main`との同一host A/B結果です。数値は5 sampleの
medianで、tree size、seed、batch sizeは同一です。

| mode/backend | `origin/main` | 新実装 | 改善 |
|---|---:|---:|---:|
| exact legacy 1 thread | 37,487 sim/s | 387,132 sim/s | 10.33倍 |
| exact sharded 1 thread | 31,773 sim/s | 222,253 sim/s | 7.00倍 |
| exact sharded 4 threads | 94,819 sim/s | 217,910 sim/s | 2.30倍 |
| exact sharded 8 threads | 125,095 sim/s | 194,405 sim/s | 1.55倍 |
| exact root-parallel 8 workers | 286,487 sim/s | 1,418,195 sim/s | 4.95倍 |
| determinized legacy 1 thread | 56,969 sim/s | 358,261 sim/s | 6.29倍 |
| determinized sharded 4 threads | 156,161 sim/s | 294,279 sim/s | 1.88倍 |
| determinized root-parallel 8 workers | 440,313 sim/s | 1,584,560 sim/s | 3.60倍 |

40,000 simulationのsharded 8-thread実行では最大RSSが159,832 KiBから44,644 KiBへ
約72%減少しました。実運用ではNN推論時間、探索深さ、thread競合で改善率が変わるため、
benchmark値を固定CI gateにはせず、同一hostでの回帰比較に使います。
