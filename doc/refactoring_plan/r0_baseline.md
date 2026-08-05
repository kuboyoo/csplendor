# 第2次リファクタリング R0 基準値

R1以降の構造変更を同じ条件で比較するため、API/runtimeの既存Phase 0計測に
build/include計測を追加した。raw JSONはhost・compiler依存のためrepositoryへ
commitせず、CI artifactまたは`/tmp`等の外部領域に保存する。

## 再現コマンド

```bash
python scripts/refactor_build_baseline.py \
  --label before-r1a \
  --build-dir /tmp/csplendor-refactor-before-r1a \
  --incremental-source src/bindings.cpp \
  --parallel 2 \
  --output /tmp/csplendor-refactor-before-r1a.json
python scripts/phase0_native_probe.py \
  --output /tmp/csplendor-refactor-native-before-r1a.json
python scripts/benchmark_phase0.py \
  --label before-r1a --samples 15 \
  --output /tmp/csplendor-refactor-runtime-before-r1a.json
```

変更後は未使用の新しいbuild directoryで同じコマンドを実行し、次で比較する。

```bash
python scripts/refactor_build_baseline.py \
  --compare /tmp/csplendor-refactor-before-r1a.json \
            /tmp/csplendor-refactor-after-r1a.json
python scripts/benchmark_phase0.py \
  --compare /tmp/csplendor-refactor-runtime-before-r1a.json \
            /tmp/csplendor-refactor-runtime-after-r1a.json
```

## R1-A前の基準

2026-08-05、Ubuntu x86_64、Python 3.12.1、CMake 3.28.3、GCC 13.3、
portable CPU target、parallel=2で取得した。

| 指標 | 基準値 |
|---|---:|
| local include node / direct edge | 38 / 106 |
| 最大transitive include（`bindings.cpp`） | 36 |
| configure | 0.588 s |
| clean extension build | 10.957 s |
| `bindings.cpp` incremental rebuild | 11.070 s |
| no-op incremental build | 0.038 s |
| extension size | 1,409,824 bytes |
| clean build child peak RSS上限 | 1,100,608 KiB |

RSSはPythonの`RUSAGE_CHILDREN`で観測した、その時点までのchild process最大値である。
単独processの厳密なpeakではないため、同じhost・同じ実行順のpaired比較にのみ使う。
source incrementalは対象ファイルのatime/mtimeを保存し、`touch`後のbuildが対象を
再コンパイルしたことをbuild出力で確認してからtimestampを復元する。build timeは
いずれも1回の診断値なので、5%境界付近では複数回測定して判断する。

同じ環境での15 sample runtime中央値は次の通りだった。

| workload | 中央値 |
|---|---:|
| `legal_actions` | 971,651 calls/s |
| `legal_action_codes` | 1,986,997 calls/s |
| `legal_action_count` | 4,025,198 calls/s |
| C++ playout | 794,573 moves/s |

native probeは`Action=21`、`MoveList=43010`、`PlayerState=104`、`Board=392`、
`Game=448`、`MCTSNode=832` bytesを再確認した。reachable random corpusでは最大596手、
overflow 0、非canonical editor境界では先頭2048手を保持し21,948件の追加emit試行を
観測した。

## 共通test support

`tests/support.py`をtest専用の正本とし、次を提供する。

- Action全fieldのimmutable signature
- canonical/derived/provenanceを含むPlayer/Game signature
- copy/write-back semanticsを通るcurrent-player editor
- deterministicおよびseeded-random reachable-state corpus

既存の合法手、MCTS、rule transition、cross-phase契約testをこのsupportへ移行した。
package探索は`csplendor*`に限定されるため、`tests` packageはwheelのruntime
packageには含まれない。
