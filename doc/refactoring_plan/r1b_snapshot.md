# 第2次リファクタリング R1-B1 snapshot compiled core化

## 目的と範囲

R1-Bの最初の独立変更として、`game_snapshot.h`に置かれていた非templateの
snapshot serialization実装を`game_snapshot.cpp`へ移した。snapshot format、rules
version、magic、checksum、ruleset fingerprint、byte列、例外、`Game`の復元結果は
変更していない。traceとsolverの移動は別PRとする。

## build境界

- `csplendor_core_config`はinclude path、C++ standard、CPU target、sanitizerを伝播する。
- `csplendor_core`は`game_snapshot.cpp`を持つ実体のあるstatic targetで、native C++
  consumerとtestがリンクする。
- Python extensionは同じ`game_snapshot.cpp`を独立TUとして直接compileする。これにより
  pybind11 moduleと同じLTO・hidden visibilityを適用し、binding TUとの並列compileを
  維持する。
- `game_snapshot.h`は定数、`Writer` / `Reader`の型と関数宣言、公開serialize APIを
  保持する。class layoutとmethod signatureも変更していない。

static targetをそのままPython moduleへリンクする構成も検証したが、Unix Makefilesの
target-level dependencyによりcore完了までbinding TUが開始されず、clean buildを直列化
した。またcoreだけLTO/hidden visibilityから外すとsnapshot速度が低下した。moduleでは
source TUを直接並列compileする構成にして、両方の回帰を解消した。

## 同値性

- Python golden snapshotは190 bytes、SHA-256
  `e9986b8a5db6a7e20ac8a797d0c44d71b08a802e6b059b7ea7f35674c22c360c`
  を維持する。
- reachable/editor、hidden reservation、future deck order、mode flag、corruption rejectionの
  既存testを変更せず通す。
- `game_snapshot_unit`は`game_snapshot.h`だけをproduct headerとしてincludeし、static
  coreへのlink、round-trip、hash、合法手順序、破損拒否を確認する。
- snapshot実装294行の最初の移動は旧header本文との機械的一致を確認した。追加で
  `Writer` / `Reader`の非template method本体も`.cpp`へ移した。

## build比較

2026-08-05、Ubuntu x86_64、Python 3.12.1、CMake 3.28.3、GCC 13.3、portable、
parallel=2で、旧commitと候補を交互に3組測定した中央値である。raw JSONは`/tmp`へ
保存し、host固有値はcommitしない。

| 指標 | R1-B1前 | R1-B1後 | 比率 |
|---|---:|---:|---:|
| clean extension build | 11.752 s | 11.740 s | 0.999 |
| `bindings_rules.cpp` incremental | 4.653 s | 4.590 s | 0.986 |
| no-op build | 0.045 s | 0.044 s | 0.978 |
| child peak RSS上限 | 730,532 KiB | 730,496 KiB | 1.000 |
| extension size | 1,393,456 bytes | 1,397,552 bytes | 1.003 |

clean/no-op/RSSは同等、通常のrules binding差分buildは約1.4%短縮した。binaryは4,096
bytes（約0.3%）増えたが5%閾値内である。

## runtime比較

seed=42から18 ply進めた184-byte snapshotを使用した。旧新を別processで実行順を
反転し、各processを2秒warmupした後、serialize 50,000回・deserialize 10,000回を
15 sampleずつ、旧新各4 run測定した中央値である。

| workload | R1-B1前 | R1-B1後 | 比率 |
|---|---:|---:|---:|
| serialize | 1,879,542 calls/s | 1,884,982 calls/s | 1.003 |
| deserialize | 572,343 calls/s | 573,692 calls/s | 1.002 |

snapshot runtimeの回帰は検出しなかった。後続のR1-B2は
[`r1b_trace.md`](r1b_trace.md)に記録する。
