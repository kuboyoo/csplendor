# 第2次リファクタリング R2-A state invariant診断

## 目的と範囲

R2の最初の独立変更として、`reachable`、`editor`、`search`、`serialized`の
状態契約を実行可能にする内部API `csplendor::state::validate_invariants()`を追加した。
公開Python API、snapshot形式、action/feature ID、合法手生成順、局面更新は変更していない。

診断は局面を修復せず、lazy hash cacheのvalid/invalid状態も変更しない。違反は安定した
64 bit maskで返し、ログやtest向けに決定論的な名称列へ変換できる。

## 検査する契約

- 全profile: status値、固定容量、card/noble ID、packed gems/bonuses、
  noble eligibility、予約数、provenance配列上限、valid cacheのstale判定。
- `reachable` / `search`: 上記に加え、card tier、予約slot配置、空slotのhidden flag、
  purchased/bonus/point provenance、宝石保存、10 token上限、card/noble一意性、
  noble partitionを検査する。
- `editor` / `serialized`: 資源保存とgame materialの一意性を意図的に緩和し、
  解析用の不完全局面とsnapshot互換性を維持する。

破損した`FixedStack::count`を検出した場合は、配列境界を越える反復やhash再計算を
行わない。`hash_valid`がtrueのときだけpureな`compute_hash_uncached()`と比較するため、
未計算cacheは正常、canonical state変更後のinvalidate漏れは`stale_hash_cache`となる。

## build境界

`state_invariants.cpp`はnative用`csplendor_core`にのみ追加した。Pythonから利用しない
内部診断をextensionへlinkしないことで、extensionのhot-path配置とbinaryをR2-A前と
byte-for-byte同一に維持した。

2026-08-05、Ubuntu x86_64、Python 3.12.1、CMake 3.28.3、GCC 13.3、portable、
parallel=2で測定した。raw JSONは`/tmp`へ保存し、host固有値はcommitしない。

| 指標 | R2-A前 | R2-A後 | 比率 |
|---|---:|---:|---:|
| clean extension build | 12.011 s | 12.039 s | 1.002 |
| `bindings_rules.cpp` incremental | 4.678 s | 4.613 s | 0.986 |
| no-op build | 0.0437 s | 0.0459 s | 1.051 |
| child peak RSS上限 | 723,048 KiB | 721,656 KiB | 0.998 |
| extension size | 1,397,552 bytes | 1,397,552 bytes | 1.000 |

no-opの約2.2 ms差は測定分解能に近い。製品extensionのSHA-256は変更前後とも
`97ab61b1451d8031ee13f7cdacda6933e620cb2d3b11eb237eb0d224e0574487`であり、
runtimeは同一binaryのため回帰しない。

## 検証

- 64 seed、4,000手超の各合法手後に`reachable` profileを検査。
- editor/serializedで許容する重複・資源不整合・provenance差と、reachable/searchでの
  拒否をprofile差分testで固定。
- packed値、予約数、ID、固定容量、stale cacheの各違反を個別testで固定。
- determinization後のsearch stateを32 seedで検査。
- Release native test 7件、Python全test、`py_compile`を通過。
- ASan/UBSan native test 7件を通過。
- strict warning buildとclang-formatを通過。

次の独立変更はR2-Bの固定容量primitiveとmutation/cache gateway整理とする。
