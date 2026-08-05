# 第2次リファクタリング R1-B2 deterministic trace compiled core化

## 目的と範囲

R1-Bの第2の独立変更として、`mcts_parallel_trace.h`に置かれていた決定論的
traceのserialization、deserialization、verification、replay実装を
`mcts_parallel_trace.cpp`へ移した。trace記録中に使うdigest、差分snapshot生成、
chain更新はhot pathのためheader内のinline実装を維持した。

trace version、magic、field順序、little-endian byte列、digest、例外、snapshot上限、
thread数／tree backend間の決定性は変更していない。

## build境界

- `mcts_parallel_trace.cpp`を`CSPLENDOR_CORE_SOURCES`へ追加し、native consumerは
  `csplendor_core`経由、Python extensionは同じsourceを独立TUとしてcompileする。
- `DeterministicTrace::verify()`、`write()`、`read()`と
  `replay_deterministic_trace()`はheaderで宣言し、compiled coreで定義する。
- enum／snapshot検証と整数／snapshot I/O helperは`.cpp`内へ閉じた。
- headerの`<istream>`、`<ostream>`、`<limits>`依存を`<iosfwd>`へ置き換えた。
- headerは854行／39,667 bytesから395行／16,012 bytesへ縮小した。探索中の
  `capture_initial()`、`ensure_record_capacity()`、`record()`とdigest helperは
  inlineのままである。

## 同値性

- seed、search nonce、simulation rangeを固定した41 simulationのcanonical traceを、
  supported toolchainごとの既存値としてgolden化した。

| toolchain | bytes | byte digest |
|---|---:|---:|
| GCC/Clang（Linux/macOS） | 196,981 | `13940474573569027194` |
| MSVC（Windows） | 202,540 | `10287971718966836909` |

MSVCでは浮動小数点を含む探索選択列がGCC/Clangと異なり、結果としてevent delta数も
異なる。cross-platformで単一byte列になることは既存契約に追加せず、各toolchain内の
byte表現と、thread数／tree backend間の決定性を固定する。

- 1/2/4/8 threadおよびcoarse/sharded tree backendで、trace byte列、探索結果、
  replay後のtree digestが完全一致する既存testを維持した。
- round-trip、magic/version/truncation/tamper、enum、path/node/aggregate上限、
  chain/statistics不整合の拒否testを変更せず通した。

## build比較

2026-08-05、Ubuntu x86_64、Python 3.12.1、CMake 3.28.3、GCC 13.3、portable、
parallel=2で、R1-B1 mainと候補を交互に3組測定した中央値である。raw JSONは`/tmp`へ
保存し、host固有値はcommitしない。

| 指標 | R1-B2前 | R1-B2後 | 比率 |
|---|---:|---:|---:|
| clean extension build | 11.477 s | 11.942 s | 1.040 |
| `bindings_mcts.cpp` incremental | 7.406 s | 7.212 s | 0.974 |
| no-op build | 0.043 s | 0.045 s | 1.044 |
| child peak RSS上限 | 730,564 KiB | 722,796 KiB | 0.989 |
| extension size | 1,397,552 bytes | 1,397,552 bytes | 1.000 |

追加TUによりclean buildは約4.0%増えたが、停止基準の5%以内である。主目的である
MCTS binding差分buildは約2.6%短縮し、peak RSSは約1.1%減少、binary sizeは不変だった。
no-opの約2 ms差は測定分解能に近く、5%基準内である。

## runtime比較

変更前後の`mcts_parallel_replay` Release binaryを別々に保持し、各6回warmup後、実行順を
反転しながら各60回測定した。これは決定論的探索、write/read/verify、破損拒否、
root-parallel testを含むend-to-end値である。

| workload | R1-B2前 | R1-B2後 | 比率 |
|---|---:|---:|---:|
| `mcts_parallel_replay` median | 0.154480 s | 0.154066 s | 0.997 |

runtime回帰は検出しなかった。

## 検証

- `pip install -e .`
- Python通常test 452件、performance test 4件
- Release native test 6件
- GCC 13.3／Clang 18.1 strict binding build
- ASan/UBSan 6件、TSan 6件
- `py_compile`、ruff、clang-format、`git diff --check`
- sdist/wheel build、twine、auditwheel、sdist再build、fresh venv smoke test

次の独立変更はR2-Aのinvariant checkerとcache invalidation可視化とする。
