# 第2次リファクタリング R1-A binding分割

## 目的と範囲

1,831行の単一`src/bindings.cpp`に集中していたPython登録を、挙動を変えず責務別の
翻訳単位へ分割した。ルール、action/feature ID、MCTS/solver algorithm、公開名、
引数、default、返却型、例外、GIL/lifetime/NumPy ownershipは変更していない。

## 構成

| ファイル | 責務 |
|---|---|
| `bindings.cpp` | 唯一の`PYBIND11_MODULE`と登録順 |
| `bindings_domain.cpp` | enum、Card、Noble、PlayerState、Action、静的data照会 |
| `bindings_rules.cpp` | Board、Game、snapshot、editor、合法手・局面更新 |
| `bindings_encoding.cpp` | StateEncoder、48/V2/V3 ActionEncoder |
| `bindings_mcts.cpp` | serial/batch/parallel MCTSとPython callback bridge |
| `bindings_solvers.cpp` | visible-only/reveal-verified solverとproof DAG変換 |
| `bindings_array.h` | callback/mask用owning NumPy copy helper |
| `bindings.h` | 登録関数の内部宣言 |

登録順は次で固定し、静的testとfresh-process import testで確認する。

```text
domain -> rules -> encoding -> mcts -> solvers
```

domain value typeを最初に登録することで、後続groupのmethod引数・返却値が既登録型を
参照する。`PYBIND11_MODULE`は9行の初期化fileに留め、各翻訳単位は必要なheaderだけを
直接includeする。

## 同値性

- `csplendor.__all__`とnative internal分類はR0契約台帳どおりである。
- fresh processでGame/Board/Action、StateEncoder、MCTS、solverを順に利用できる。
- encoder mask、batch request、parallel callbackのNumPy配列は引き続き独立した
  owning arrayである。
- native probeの型サイズ、clone allocation、reachable/editor overflow結果は
  R1-A前と完全一致した。
- golden action列、hash、snapshot、MCTS trace、proof DAGを含む全testを変更せず通す。

## build/include比較

R0の同一Ubuntu/GCC/portable/parallel=2条件で取得した1回の診断値である。

| 指標 | R1-A前 | R1-A後 | 比率 |
|---|---:|---:|---:|
| clean build | 10.957 s | 11.384 s | 1.039 |
| 登録source 1本のincremental build | 11.070 s | 4.396 s | 0.397 |
| child peak RSS上限 | 1,100,608 KiB | 730,380 KiB | 0.664 |
| extension size | 1,409,824 bytes | 1,393,456 bytes | 0.988 |
| 最大TU transitive include | 36 | 32 | 0.889 |
| no-op build | 0.038 s | 0.042 s | 1.112 |

clean buildは+3.9%で計画閾値の+5%以内、通常の登録変更を模したincremental buildは
約60%短縮した。no-opは翻訳単位増加により約4 ms増え比率上は5%を超えたが、実compile
を伴うincrementalとclean buildは基準内であり、CMake dependency scanの固定費として
記録する。RSS診断値は約34%、binary sizeは約1.2%減少した。

## runtime比較

初回測定でCPU governorの立ち上がりが先頭5〜6 sampleへ混入したため、Phase 0 runnerに
明示的なwrapper/playout warmupを追加した。旧extensionと新extensionを各30 sample、
実行順を反転した2組で比較し、どちらの順でも回帰を検出しなかった。反転組での
candidate/baseline 95%区間は`legal_actions=[1.015, 1.067]`、
`legal_action_codes=[1.125, 1.159]`、`legal_action_count=[1.237, 1.268]`、
`C++ playout=[1.163, 1.200]`だった。

これは同一のrule/search codeに対する回帰確認値であり、binding分割そのものによる
runtime高速化とは解釈しない。
