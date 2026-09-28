# macOS Apple Siliconのビルド設定

[READMEのセットアップ](../README.md#セットアップ)からの補足です。


`CSPLENDOR_CPU_TARGET`で、配布用とローカル最適化用を同じソースから
分けてビルドできます。

- `portable`（既定）: CPU固有フラグを追加しません。汎用arm64 wheelなどの
  配布物には必ずこちらを使用します。
- `native`: Apple SiliconのローカルCPUに合わせて`-mcpu=native`を使用します。
  M4 Pro上ではM4向けコードになります。

Pythonビルドのarchitectureは`CSPLENDOR_OSX_ARCHITECTURES`へ`arm64`、
`x86_64`、`universal2`のいずれかを指定できます。`ARCHFLAGS`などと競合する
指定はエラーになります。通常wheelでは選択したarchitectureとplatform tagも
照合するため、クロスビルドには一致するPythonまたは`_PYTHON_HOST_PLATFORM`が
必要です。

Python拡張のビルド例:

```bash
# 配布用の汎用arm64 wheel
MACOSX_DEPLOYMENT_TARGET=11.0 \
  CSPLENDOR_OSX_ARCHITECTURES=arm64 \
  CSPLENDOR_CPU_TARGET=portable \
  python -m pip wheel . --wheel-dir dist/arm64

# このMac用のローカル最適化版
CSPLENDOR_OSX_ARCHITECTURES=arm64 \
  CSPLENDOR_CPU_TARGET=native \
  python -m pip install -e .
```

CMakeを直接使う場合は、異なるbuild directoryを指定します。

```bash
cmake -S . -B build/macos-arm64-portable \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DCSPLENDOR_CPU_TARGET=portable
cmake --build build/macos-arm64-portable --parallel 2

cmake -S . -B build/macos-m4-native \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCSPLENDOR_CPU_TARGET=native
cmake --build build/macos-m4-native --parallel 2
```

`native`はeditable installまたはCMake直接ビルド専用です。通常wheelと同じ互換性tag
ではM4専用であることを表現できないため、native wheelの作成はエラーになります。
また、以前のprofileのバイナリを混入させないため、wheelの`--skip-build`も
使用できません。PEP 660 editable installが内部で作る一時wheelは配布物ではないため、
配布wheel向けのarchitecture/tag照合は適用しません。
例ではApple Siliconの最小OSであるmacOS 11.0をdeployment targetにしています。
サポート方針に応じて、これより新しい値へ変更できます。環境変数を省略したPython
ビルドでは、そのPython自身のdeployment targetをCMakeへ引き継ぎます。
wheelの互換性tagはビルドに使うPython自身の下限にも制約されるため、リリース時は
Mach-Oのminimum OSとwheel tagの両方を確認してください。
universal2 Pythonも、arm64プロセスとして実行し、arm64専用拡張を選択したeditable
installまたはCMake直接ビルドでは`native`を使用できます。生成物はarm64専用なので、
同じPythonをRosettaでx86_64として実行した場合には読み込めません。Rosetta上のbuild、
universal2拡張、非Apple環境では`native`を使用できません。
