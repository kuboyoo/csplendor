# R7: Python application層と外部AI adapterの整理

実施日: 2026-08-05

## 目的と維持契約

FastAPI、process-global dictionary、USI/KIFU codec、legacy replay、外部AI実装の
依存方向を整理した。既存endpointのpath、query/body、response schema、status code、
session/undo/replay semanticsは変更しない。`sessions`、`session_records`、
`kifu_sessions`、`replay._sessions`の辞書風アクセスも既存embedding/test向けに維持する。

## 新しい責務境界

- `stores.py`: `SessionStore` / `KifuStore` protocolと辞書互換`InMemoryStore`。
- `game_service.py`: game session、action適用、undo、KIFU記録。
- `kifu_service.py`: KIFU保存、読込、step replay、branch。
- `game_presenter.py`: native engine値からresponse schemaへの変換。
- `usi_tokens.py` / `usi_parser.py` / `usi_types.py`: token、parser、DTO。
- `usi_resolver.py` / `usi_serializer.py`: legal-action解決とUSI出力。
- `kifu_codec.py` / `spn_codec.py`: KIFU codecとSPN builder/serializer surface。
- `legacy_replay_reader.py`: trusted local pickleだけを扱う隔離reader。
- `ai_provider.py`: `AIProvider` protocol、request/decision DTO、legacy adapter。
- `external_ai_bridge.py`: 明示設定またはlegacy sibling checkoutのpath bridge。

`app.py`とreplay routerはrequest validation、application errorからHTTP errorへの変換、
response組立だけを担当する。永続storeやfake AI providerは差し替え可能である。

## legacy pickleのtrust boundary

pickleはロード時にコードを実行し得るunsafe formatである。readerは次を強制する。

1. 管理者が設定したdirectory内の`.pkl` regular fileだけを許可する。
2. traversal、directory外へ出るsymlink、NUL、別拡張子を拒否する。
3. 512 MiBと1,000,000 stepの上限を適用する。
4. file listingではunpickleしない。
5. `/replay/files`と`/replay/load`に`X-Replay-Format-Warning`を付ける。

uploadや外部入力を扱う用途ではpickle endpointを使わず、非実行形式へ移行する。

## 外部AIの段階移行とrollback

互換期間中は`LegacyAIProvider`を既定providerとする。通常の`csplendor`またはWeb app
importではtorch、外部repository、model fileを探索しない。AI moveまたはmodel listingを
明示的に呼んだ時だけlegacy adapterが動く。

移行順序:

1. `dlsplendor`側で`AIProvider.choose_action`相当のadapterを実装し、installed packageで
   提供する。
2. csplendor embedding側で`set_ai_provider()`にそのadapterを注入する。
3. model discovery、学習framework固有class、checkpoint解決を外部側へ移す。
4. 双方のreleaseを跨いでfake-provider/API contract testを維持する。
5. sibling fallback利用がなくなった後に別majorでlegacy managerを削除する。

移行中の明示pathは`CSPLENDOR_DLSPLENDOR_PATH`、`CSPLENDOR_ALPHAZERO_PATH`、
`CSPLENDOR_DEEPSETS_PATH`、`CSPLENDOR_NNUE_PATH`で指定できる。rollback時はprovider注入を
解除して`reset_ai_provider()`を呼ぶと`LegacyAIProvider`へ戻る。旧配置が必要なら上記path
または従来のsibling checkoutを復元できる。このfallbackは互換期間専用であり、通常import
では有効化されない。

## USI正本との関係

USI仕様は隣接`usi/docs/USI.md`を正本とする。本repositoryではv1の色順序、代表move、
全合法手serialize/resolve、KIFU goldenをcompatibility testに固定した。隣接checkoutがある
開発環境では正本文書の色ID表も直接照合し、単独checkoutのCIでは内蔵v1 fixtureを検証する。

`usi_kifu.py`は既存import pathの互換surfaceとして残し、新規application codeは分割moduleを
直接利用する。SPNの成熟した実装本体は互換期間中そのsurfaceの背後に維持する。

## 検証

- 既存API、KIFU、USI、replay、optional AI contract test。
- fake providerによるAI move/model endpoint再現。
- custom store注入と辞書互換性。
- Web application import時のmodel/sibling discovery禁止。
- structured logging fieldとunsafe replay warning header。
- 隣接`usi`正本の色ID/記号compatibility。

ローカルではPython 3.12で通常test 475件、performance test 4件、warnings-as-errors、
Ruffのerror/import/security rules、`py_compile`、editable install、sdist/wheel buildと
Twine checkを通過した。Python coverageは54%で既存50% gateを満たした。OS/Python matrixは
PRのGitHub Actionsをmerge gateとする。
