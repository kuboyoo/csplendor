# Web API

[README](../README.md#web-apiと対戦サービスへの組み込み) / [ドキュメント索引](index.md)

## 起動

```bash
python -m pip install -e '.[web]'
python -m uvicorn csplendor.api:app --host 127.0.0.1 --port 8000
```

OpenAPI: `http://127.0.0.1:8000/docs`。現在のリクエスト・レスポンス定義は [app.py](../csplendor/api/app.py) と [schemas.py](../csplendor/api/schemas.py) が正本です。

## 対局API

| method / path | 入力 | 応答 |
|---|---|---|
| `POST /game` | query: `seed=0`, `simple_payment_mode=true`, `player0_name`, `player1_name` | `{"session_id":"..."}` |
| `GET /game/{session_id}` | なし | `GameStateSchema` |
| `POST /game/{session_id}/action` | query: `action_idx`, 任意の `time_ms`, `comment` | 更新後の `GameStateSchema` |
| `POST /game/{session_id}/action_usi` | JSON: `usi_move`, 任意の `time_ms`, `comment` | `action_idx`, `action_usi`, `state` |
| `POST /game/{session_id}/undo` | なし | 更新後の `GameStateSchema` |
| `POST /game/{session_id}/ai_move` | query: `ai_type` など | 外部AIによる着手結果 |

`seed=0` も再現可能な固定seedです。ランダムな初期配置を作る場合は呼出し側でseedを生成します。

`action_idx` は**その局面の `legal_actions` 一覧内の添字**で、V2/V3/V4の行動IDやpacked codeではありません。局面更新後は一覧を取り直します。対局終了時の合法手は空です。`PASS`（type 6）・`RETURN_GEM`（type 7）も一覧の添字で適用できます。

`action_usi` の `usi_move` は正規の1手（例: `reserve:L2`、続けて `return:W`）を受け付けます。旧来の1手表記 `reserve:L2/return:W`（返却色はちょうど1文字）も受け付け、山札予約と返却の2手に展開して原子的に適用します（失敗時は局面も棋譜も変更しません）。この場合、棋譜には2手が記録され、応答の `action_usi` は `"reserve:L2 return:W"` のように正規の2手を空白区切りで返します（`action_idx` は最後の手の、その時点の合法手一覧での添字）。

### 支払いモード

- `simple_payment_mode=true`（HTTP既定）: 購入ごとにGold最小使用の1通り。
- `simple_payment_mode=false`: 有効なGold代替・色トークン支払いをすべて列挙。

Pythonの `Game` は `False` が既定です。対局サーバー・AI・棋譜に同じモードを設定してください。簡易モードでもトークン返却の選択肢は残ります。

購入Actionの `gold_as` は白・青・緑・赤・黒の5要素です。例: `[0,2,1,0,0]` は青2個分と緑1個分を金で払います。同じ `card_id` に複数の支払Actionが存在できます。GUIでは対象カードを絞った後、支払いと返却の選択肢を提示します。

## 状態レスポンス

`GameStateSchema` は次を返します。

- `board`: `bank`, `visible_cards`, `deck_counts`, `nobles`, `current_player`, `turn`, `waiting_noble`, `waiting_return`, `pending_decision`, `game_over`, `winner`。`pending_decision` は `0` なし、`1` トークン返却、`2` 貴族選択。
- `players`: 各プレイヤーのトークン、ボーナス、点数、予約・購入カードID、獲得貴族ID。
- `legal_actions`: 行動種別、対象、取得・返却、支払い、貴族選択、USI表記。

初期銀行は `[4,4,4,4,4,5]`、公開後の初期山札残数は `[36,26,16]` です。色順は白・青・緑・赤・黒・金で、5要素の配列は金を含みません。カード・貴族の描画用データは [カタログ](card_catalog.md) または `get_all_cards()` / `get_all_nobles()` から取得できます。

`waiting_noble` の間は同じプレイヤーが貴族を選びます。`waiting_return` の間（山札予約で11枚になった直後）は同じプレイヤーが `RETURN_GEM` で1枚返します。返却はめくれたカードを見てから選び、その後に貴族判定を行います。状態の購入Actionの `usi` は `/pay:` 拡張を含むことがあるため、外部USIエンジンの対応表記を確認してください。

## 外部AIと棋譜

`/ai_move` は互換用のoptional bridgeです。外部 `dlsplendor`・PyTorch・モデルがない場合は503を返します。`[ml]` はPyTorchのみを提供し、モデルやAI実装を同梱しません。不明なAIモードや必要な固定探索予算の欠落は、モデルをloadする前に400となります。

組み込みアプリは `AIProvider` と `set_ai_provider()` で独自のAIを接続できます。エンジン/Webアプリのimportだけでは外部repoやモデルを探索しません。互換bridgeの外部ルートは `CSPLENDOR_DLSPLENDOR_PATH`、`CSPLENDOR_ALPHAZERO_PATH`、`CSPLENDOR_DEEPSETS_PATH`、`CSPLENDOR_NNUE_PATH` で指定できます。

棋譜のメタデータ更新・保存・再生APIもあります。ルート一覧はOpenAPI、形式は [KIFU](KIFU.md) を参照してください。

旧replay API `GET /replay/files`、`POST /replay/load`、`GET /replay/{session_id}/game/{game_idx}/{step}` は管理者が置いた信頼済みpickle向けです。load対象は設定directory内の `.pkl` に制限され、外部pathやdirectory外へ出るsymlinkは拒否されます。pickleはコード実行を伴う形式なので、ユーザーuploadの入力形式として使わないでください。

## 公開対戦サービスとの境界

現行APIのセッションはプロセス内メモリです。永続化、複数worker共有、認証、対局ごとの排他・局面version検査はサービス側で用意します。

**現在の状態レスポンスには、両プレイヤーの伏せ予約の実カードIDが含まれます。** 観測者別の秘匿レスポンスではありません。プレイヤーや観戦者への配信前に、`reserved_is_hidden` と観測者を使って秘匿してください。完全snapshot・山札順もサーバー内部だけに保持します。

ゲームを管理するworkerとAIプロセスを分け、持ち時間、切断、不正手、`stop` 後も終了しないAIの扱いを対局規定として定めます。基本構成は [README](../README.md#web-apiと対戦サービスへの組み込み) を参照してください。
