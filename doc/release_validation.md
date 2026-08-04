# リリース検証記録

最終更新: 2026-08-04

この文書は、Phase 0--7のリファクタリングと並列探索実装を含む作業ツリーに対して
実施したローカル検証の記録です。ローカルで通過した項目と、PyPI公開前に別環境で
通過させる必要がある項目を分けています。特定時点の検証記録であり、以後の変更を
無条件に保証するものではありません。

## 追加監査で解消した主な問題

- sdistにCMake/native sourceが入らず、sdistからwheelをbuildできなかった構成を修正。
- action mask/policy bindingが一時`std::array`のmemoryを参照し得たため、所有権付きNumPy
  arrayへ必ずcopyするよう修正し、`OWNDATA`回帰testを追加。
- 合法手が存在しない非終局局面へforced PASSを実装し、apply/undo/hash/USI/encoder/MCTSを同期。
- root-parallel cancel競合のgeneration不一致、callback失敗後の再利用、virtual loss収支を修正・stress化。
- replay APIの任意path unpickleとabsolute path公開を廃止し、directory containment、symlink、
  size、shape、scalar、file descriptor再検査を追加。
- 外部AI stackのeager importと未定義`use_mcts`参照を解消し、optional dependency不在時を
  HTTP 503として分離。

## ローカル検証結果

| 区分 | 結果 | 確認内容 |
|---|---:|---|
| Python通常test | 439 passed, 4 deselected | performance markerを除くpytest全体、warningをerror化 |
| Python性能test | 4 passed | performance markerを明示実行 |
| Python coverage | 56.15% | 50% gate通過。action space 86%、features 96%、replay 90% |
| Python lint/security | 成功 | Ruff default/import/whitespace/security rulesをPython 3.8 targetで実行 |
| native CTest | 5/5 passed | native unit/stress/replayとMCTS高速化同値testを含む構成 |
| native反復 | 100/100成功 | 4 testを各25回反復 |
| ThreadSanitizer | 5/5 passed | Clang TSAN build |
| Address/UndefinedBehaviorSanitizer | 5/5 passed | GCC ASan+UBSan build |
| strict warning build | 成功 | GCC/Clang `-Wall -Wextra -Wpedantic -Werror` |
| Clang static analysis | 成功 | binding translation unitへ`clang-tidy`を実行 |
| source distribution | 成功 | sdistからCPython 3.12 wheelを再build |
| wheel smoke | 成功 | 上記wheelを隔離venvへinstallしてimport smokeを実行 |
| Python version smoke | 成功 | Python 3.9、3.10、3.11、3.12でsdist native build/installを確認 |

代表的なPython確認コマンドは次のとおりです。

```bash
python -m pytest -o addopts= -m "not performance"
python -m pytest -o addopts= -m performance
python -m compileall -q csplendor
```

LeakSanitizerの`detect_leaks=1`は、このsandboxで必要なptrace動作が許可されないため
実行できませんでした。これはtest failureではなく環境制約ですが、公開前に制約のない
CIまたはhostで通過させる必要があります。

## 継続的なクロスプラットフォーム検証

Pull Requestと`main`へのpushでは、Python 3.12を使って次の構成を検証します。

- macOS 15 arm64の`portable`および`native` CPU target。
- Ubuntu 24.04 arm64の`portable` CPU target。
- Windows 2025 x64の`portable` CPU target。

各構成でeditable install、Python compile、pytest、native CTestを実行します。
`portable`構成ではwheelも作成し、wheelへ入れ替えた後にimportと合法手生成を
smoke testします。macOSではuniversal2 Pythonをarm64プロセスとして実行する
editable buildを検証し、配布用arm64 wheelは明示的なplatform tagで別途buildして
Mach-Oがarm64専用であることを確認します。長時間のnightly soakとは分離し、
定期scheduleでは既存のLinux x64検証だけを実行します。

## Packagingで確認した境界

- sdistには`CMakeLists.txt`、`src/`、Python package、必要なbuild metadataを含めます。
- wheelのbuildはsdistを入力にしても成功し、CPython 3.12の隔離venvでimportできました。
- Linux host上で直接生成した未修復wheelを、そのまま汎用PyPI wheelとして公開しません。
- FastAPIは`web` extraです。ルールエンジン本体のimportにWeb依存は不要です。
- `/ai_move`は互換用のoptional integrationです。torchと外部`dlsplendor`は利用時に
  遅延loadされ、外部stackがない場合はHTTP 503になります。モデルやNN探索実装は
  csplendor packageへ同梱しません。

## Replay pickleの信頼境界

legacy replay APIはpickleを実行可能な形式として扱います。

- server管理者が設定済みreplay data directoryへ配置した、信頼済みローカル`.pkl`
  だけを読み込みます。
- `POST /replay/load`はfilenameまたは互換用pathをrealpathで検査し、directory外の
  任意path、path traversal、directory外へ抜けるsymlink、非`.pkl`を拒否します。
- `GET /replay/files`はserverの絶対pathを返さず、一覧取得のためのunpickleを行いません。
- file sizeとexample数に上限を設け、読込後にboard、policy、scalar metadataを検証します。
- user uploadや外部由来pickleをこのdirectoryへ直接配置してはいけません。信頼境界を
  越えてreplayを交換する用途では、JSON等の非実行形式へ移行してください。

## PyPI公開前の外部ゲート

次の項目は現在のsandboxだけでは完了していません。release workflowまたは専用hostで
すべて通過してから公開します。

| Gate | 必須確認 |
|---|---|
| manylinux | 対象manylinux imageでwheelをbuild/repairし、隔離installとnative smokeを行う |
| `auditwheel` | `auditwheel show`と必要な`repair`を実施し、platform tagと共有library依存を確認する |
| macOS | arm64 CIに加え、公開wheelのMach-O minimum OSとplatform tagを検査する |
| Windows | x64 CIに加え、公開対象wheelを隔離環境で最終確認する |
| Python 3.8 | 宣言済み最小versionでsdist/wheel build、install、testを行う |
| `twine` | 最終sdist/wheelへ`twine check`を実行する |
| TestPyPI | upload後、公開artifactだけを使う新規venv install/import smokeを行う |
| LeakSanitizer | ptrace制約のないLinux環境で`detect_leaks=1`を通す |

CIのPython 3.8--3.12 matrix、cross-platform matrix、native sanitizer、package buildを
release候補の同一revisionで再実行し、上記artifact検査が成功するまではbinary
distributionを公開可能とは判定しません。
