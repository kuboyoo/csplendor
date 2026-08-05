# 第2次リファクタリング R4 encoding schema一元化

## 目的と境界

48枠、V2、V3のaction spaceと196要素state featureについて、version、size、section
offset、action type、feature shape、gem色順序の正本を`encoding_schema.h`へ集約した。
公開ID、feature値、既定schema、Python API、無効入力fallbackは変更していない。

## schema descriptor

| schema | version | size | 主なsection |
|---|---:|---:|---|
| `ActionSpaceV1` | 1 | 48 | 10 / 5 / 12 / 3 / 12 / 3 / 3 |
| `ActionSpaceV2` | 2 | 4,869 | 840 / 140 / 84 / 21 / 3,024 / 756 / 3 / 1 |
| `ActionSpaceV3` | 3 | 3,133 | 840 / 140 / 84 / 21 / 2,035 / 12 / 1 |
| `StateFeatureV1` | 1 | 196 | bank 6、players 36×2、visible 96、deck 3、nobles 18、turn 1 |

各encoderの従来static constantはdescriptorへのcompatibility aliasとして維持した。
`StateEncoder`はshape、section、card/noble/player feature size、gem color ID/nameをPythonへ
公開する。descriptorのsectionはoffset順で連続し、末尾が必ず公開sizeと一致する。

state schema v1の色順序は
`diamond, sapphire, emerald, ruby, onyx, gold`、IDは`0..5`で固定する。既存の
fingerprintは変更せず、action schemaにはversioned fingerprintを追加した。

## Python compatibility wrapper

`StateFeaturizer.featurize()`は`StateEncoder.encode()`へ、`ActionEncoder`の通常encode、
decode、maskは`ActionEncoderCpp`へ委譲する。返却dtype、shape、独立所有、選択heuristicは
従来どおりである。

旧Python encoderには、状態に存在しないvisible/reserved card、不正deck level、候補外 noble、
4色以上のtakeなどに既定slotを返す固有挙動がある。これらはnativeの`-1`へ変更せず、
schema descriptorからoffsetを取得する明示的fallbackとして残した。privateなcard/noble
feature helperもsource互換のため残すが、通常featurize経路では利用しない。

## 全ID・feature同値性

`encoding_schema_unit`はseed 42の同一局面で全action IDをdecodeし、pack列のFNV-1aを
固定する。

| schema | 全ID数 | FNV-1a |
|---|---:|---|
| V1 | 48 | `0x2b6c6bfb7226c44d` |
| V2 | 4,869 | `0xb076fc64ccd1e74a` |
| V3 | 3,133 | `0xf3c8519bf281d5f3` |

state featureは16 seed・48 ply・3 observer、計768局面のfloat32 byte列を固定し、
FNV-1aは`0xcde3bf1dd313ae48`である。Python testは委譲前と独立した旧計算式を保持し、
reachable corpusとhidden reservationを含むeditor局面でbit-for-bit一致を確認する。

## 性能

2026-08-05、Ubuntu x86_64、Python 3.12.1、GCC 13.3、portable ReleaseでR3 mainと
比較した。隔離wheelを用い、実行順を反転した7組、各15 sample×1,000 iterationの
group中央値比を記録する。raw JSONは`/tmp`へ保存しcommitしない。

| workload | R4 / R3 中央値 |
|---|---:|
| Python state feature | 10.142 |
| Python action mask | 11.196 |
| Python action decode | 8.718 |
| native state feature binding | 1.017 |
| native action mask binding | 1.007 |
| native action decode binding | 1.061 |

高速化はPythonから多数のnested propertyとlegal actionを往復していた処理を、1回のnative
呼び出しへ置き換えた効果である。C++ encoder本体はconstexpr aliasへの機械的置換で、
native workloadに回帰はない。

再現には`scripts/benchmark_encoding.py`を使う。

```bash
python scripts/benchmark_encoding.py --label baseline --output /tmp/base.json
python scripts/benchmark_encoding.py --label candidate --output /tmp/candidate.json
python scripts/benchmark_encoding.py --compare /tmp/base.json /tmp/candidate.json
```

## 検証

- Python全test、legacy reference differential、`py_compile`、ruff
- GCC / Clang Release native test
- ASan/UBSan、TSan native test
- GCC / Clang strict binding build
- V1/V2/V3全ID golden、state feature byte golden
- paired encoding benchmark、clang-format、`git diff --check`

R4の完了条件を満たしたため、次の独立変更はR5のMCTS facade、owner、session、config
validation境界の整理とする。
