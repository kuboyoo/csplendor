# 第2次リファクタリング R2-B mutation gateway整理

## 目的と範囲

R2の第2の独立変更として、固定容量containerのoverflow方針、Python editor更新、
trusted rule/search更新、hash cache invalidationの境界を明示した。公開Python API、
`Board` / `PlayerState` / `Game`のlayout、snapshot、action/feature ID、合法手順序は
変更していない。

## 固定容量primitive

`FixedStack`を`board.h`から内部header `fixed_stack.h`へ分離した。既存C++ consumerの
source/layout互換性のため`data`、`count`と従来の`push_back()` / `pop_back()`は維持する。

- `try_push_back()` / `try_pop_back()`は失敗をboolで返し、data-dependentなloader、
  editor、reveal traversalで利用する。
- `push_back_unchecked()`はcapacityを局所的に証明できる初期化、determinizationの
  fixed poolでのみ利用する。
- 互換`push_back()`は従来どおりoverflowを無視し、互換`pop_back()`は空ならno-opとする。
- 破損したpublic `count`はR2-A checkerで検出し、hidden randomizationも境界外countを
  反復せず終了する。

## mutation gateway

`Board::begin_editor_mutation()`と`begin_unchecked_mutation()`は、ともに変更前にlazy hash
cacheをinvalidateし、呼び出し側のtrust boundaryを名前で区別する。

- `board_editor.h`はPython setterのpayload全体を検証・準備してからeditor gatewayへ
  入る。失敗時は局面とvalid cacheを変更しない。
- `Game`、visible/reveal solver、determinizationは検査済みまたはrollback前提の
  unchecked gatewayへ入ってからrule primitiveを呼ぶ。
- snapshot復元は全payload検査後にeditor gatewayを通り、未計算cacheとして公開する。
- `invalidate_hash()`は公開C++互換wrapperとして残す。public fieldの直接変更は引き続き
  可能だが、内部製品経路はnamed gatewayへ統一し、外部のinvalidate漏れはR2-A checkerで
  検出する。

editor実装を独立TUにするとclean buildが停止基準を超えたため、責務namespaceを保った
inline internal headerとした。これにより追加TUとtarget直列化を避けた。

## 同値性と安全性

- Python editorの受理値、例外型、copy semantics、失敗時atomicityを既存testで維持した。
- native testでFixedStackの明示/silent方針、editor失敗時のstate/cache非変更、
  set_playerのderived値同期、unchecked gateway、合法手後のreachable invariantを固定した。
- native probeの`PlayerState` 104 bytes、`Board` 392 bytes、`Game` 448 bytesは不変だった。

## build比較

2026-08-05、Ubuntu x86_64、Python 3.12.1、CMake 3.28.3、GCC 13.3、portable、
parallel=2で、実行順を反転した3組の中央値を記録する。raw JSONは`/tmp`へ保存し、
host固有値はcommitしない。

| 指標 | R2-B前 | R2-B後 | 比率 |
|---|---:|---:|---:|
| clean extension build | 12.115 s | 12.141 s | 1.002 |
| `bindings_rules.cpp` incremental | 4.551 s | 4.591 s | 1.009 |
| no-op build | 0.0442 s | 0.0463 s | 1.047 |
| child peak RSS上限 | 722,628 KiB | 722,428 KiB | 1.000 |
| extension size | 1,397,552 bytes | 1,397,552 bytes | 1.000 |

全項目が5%停止基準内である。no-opの約2.1 ms差は測定分解能に近い。

## runtime比較

固定midgameとplayout corpusを30 sampleずつ測定した中央値である。

| workload | R2-B前 | R2-B後 | 比率 |
|---|---:|---:|---:|
| C++ playout moves/s | 838,375 | 870,147 | 1.038 |
| legal action codes/s | 2,098,786 | 2,140,328 | 1.020 |
| legal action count/s | 4,681,834 | 4,832,575 | 1.032 |
| Python legal actions/s | 936,480 | 924,373 | 0.987 |

停止基準を下回るruntime回帰は検出しなかった。

## 検証

- `pip install -e .`
- Python全test、`py_compile`
- GCC/Clang Release native test 8件
- ASan/UBSan native test 8件
- TSan native test 8件（host ASLRによる起動時mapping衝突はCTest retryで回避し、
  race報告なし）
- native ABI/copy/overflow probe
- clang-format、`git diff --check`

次の独立変更はR2-Cのcopy/snapshot/delta ownershipとpublic field移行方針の確定とする。
