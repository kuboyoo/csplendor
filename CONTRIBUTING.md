# Contributing

このrepoは `csplendor` エンジン専用です。変更時は、合法手生成・局面更新・Python bindingの互換性を優先してください。

## 基本方針

- ルール変更は必ずテストを追加する。
- USIの表記・通信仕様を変更する場合は、先に `usi` を更新する。
- AI学習やGUI都合のコードは、原則として `dlsplendor` または `splendorgui` に置く。

## 確認

```bash
pip install -e .
python -m pytest
```
