# Splendor Mate Solver

`scripts/mate_solver.py` は、任意の2人戦局面について、指定した攻撃側が `N` 手以内に勝利を保証できるかを探索するための詰みソルバーです。

このソルバーは問題生成・検証用です。対局中に毎手高速実行するAIではなく、正確性を優先して、購入・予約後にめくれるカードを全候補に分岐します。

## 詰みの定義

攻撃側の手番では、勝てる手が1つでもあれば成功です。

```text
攻撃側手番:
  exists action . for all reveal . win(next_state, depth - 1)
```

防御側の手番では、防御側がどの合法手を選んでも勝てる必要があります。

```text
防御側手番:
  for all action . for all reveal . win(next_state, depth)
```

深さは攻撃側の着手回数だけで数えます。防御側の着手では深さを消費しません。

例:

- `--max-depth 1`: 攻撃側の次の1手で勝利を保証できるか。
- `--max-depth 2`: 攻撃側、防御側、攻撃側までで勝利を保証できるか。
- `--max-depth 3`: 攻撃側、防御側、攻撃側、防御側、攻撃側までで勝利を保証できるか。

勝敗は「15点到達」ではなく、エンジンの最終ラウンド終了後の `winner` を使います。同点で `winner == -2` の場合、攻撃側の詰みとは扱いません。

## 基本コマンド

初期局面から探索します。

```bash
python scripts/mate_solver.py --seed 0 --attacker 0 --max-depth 1 --pretty
```

棋譜を進めた局面から探索します。

```bash
python scripts/mate_solver.py \
  --seed 42 \
  --moves 'take:WUG,reserve:C12,buy:C7/pay:W0U0G0R4K0D0' \
  --attacker 0 \
  --max-depth 2 \
  --pretty
```

任意盤面JSONから探索します。

```bash
python scripts/mate_solver.py \
  --state-json /path/to/state.json \
  --attacker 1 \
  --max-depth 3 \
  --node-limit 500000 \
  --time-limit 30 \
  --pretty
```

USIプロトコルの `position` コマンドをそのまま渡すこともできます。

```bash
python scripts/mate_solver.py \
  --position 'position bank:W4U4G4R4K4D5 | visible:L1[0,8,16,24]L2[40,46,52,58]L3[70,74,78,82] | decks:36,26,16 | nobles:[0,3,7] | P0:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] | P1:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] 0' \
  --attacker 0 \
  --max-depth 2 \
  --pretty
```

長い場合はファイルから読み込めます。

```bash
python scripts/mate_solver.py \
  --position-file position.txt \
  --attacker 0 \
  --max-depth 2 \
  --jobs 0 \
  --pretty
```

証明木が大きい場合は、`--no-proof` を付けると結果と統計だけを出します。

```bash
python scripts/mate_solver.py --state-json state.json --attacker 0 --max-depth 2 --no-proof
```

## CLI引数

| 引数 | 必須 | 既定値 | 内容 |
| --- | --- | --- | --- |
| `--state-json PATH` | 任意 | なし | 任意盤面を表すJSONファイル。省略時は `--seed` から初期化します。 |
| `--position TEXT` | 任意 | なし | USI `position` コマンド、`startpos 2`、またはSPN文字列を直接渡します。 |
| `--position-file PATH` | 任意 | なし | USI `position` コマンド、`startpos 2`、またはSPN文字列をファイルから読み込みます。 |
| `--seed N` | 任意 | `0` | `--state-json` を使わない場合の初期局面seed。`--state-json` 内にも `seed` を書けます。 |
| `--moves TEXT` | 任意 | なし | 探索前に適用するUSI風の手。カンマ区切り、または複数回指定できます。 |
| `--attacker {0,1}` | 任意 | `0` | 詰みを探す側のプレイヤーID。 |
| `--max-depth N` | 必須 | なし | 攻撃側の着手回数で数えた探索深さ。 |
| `--node-limit N` | 任意 | `200000` | 探索ノード上限。超えると `Unknown`。 |
| `--time-limit SEC` | 任意 | `10.0` | 探索時間上限秒。超えると `Unknown`。 |
| `--jobs N` | 任意 | `1` | root候補手を並列探索するworkerプロセス数。`0` ならCPU数を使います。 |
| `--allow-deck-reserve` | 任意 | false | 伏せ予約を探索の合法手に含めます。通常の詰み問題では使いません。 |
| `--no-proof` | 任意 | false | `proof_tree` / `refutation` の生成を抑制します。 |
| `--pretty` | 任意 | false | JSON出力をインデントします。 |

`--state-json`, `--position`, `--position-file` は同時指定できません。`--position` や `--position-file` に含まれる `moves` を適用したあと、追加で `--moves` に指定した手が適用されます。

終了コード:

- `0`: `Mate` または `NoMate` まで判定できた。
- `1`: 入力不正などで `InvalidInput`。
- `2`: 制限到達などで `Unknown`。

## 出力

出力はJSONです。

```json
{
  "status": "Mate",
  "depth": 2,
  "proof_tree": {},
  "refutation": null,
  "stats": {
    "nodes": 1234,
    "memo_hits": 120,
    "terminal_nodes": 15,
    "reveal_branches": 300,
    "legal_moves": 6000,
    "elapsed_ms": 42.5,
    "max_depth_reached": 2,
    "unknown_reason": null
  }
}
```

`status` の意味:

- `Mate`: 指定深さ以内に完全詰みが証明されました。
- `NoMate`: 指定深さ以内に完全詰みがないことが証明されました。
- `Unknown`: ノード上限または時間上限で結論が出ませんでした。
- `InvalidInput`: JSONや手指定が不正です。

`depth` は `Mate` の場合に指定した `--max-depth` が入ります。最短詰み手数を知りたい場合は、呼び出し側で `--max-depth 1`, `2`, `3` のように順に実行してください。

## 手の渡し方

`--moves` は探索開始前にエンジンへ適用する手順です。探索中の合法手生成とは別です。

カンマ区切り:

```bash
--moves 'take:WUG,reserve:C12,buy:C7/pay:W0U0G0R4K0D0'
```

複数回指定:

```bash
--moves 'take:WUG' --moves 'reserve:C12' --moves 'noble:N3'
```

### トークン文字

USI風表記のトークン文字は次の通りです。

| 文字 | 色 | JSON配列index |
| --- | --- | --- |
| `W` | 白 / Diamond | 0 |
| `U` | 青 / Sapphire | 1 |
| `G` | 緑 / Emerald | 2 |
| `R` | 赤 / Ruby | 3 |
| `K` | 黒 / Onyx | 4 |
| `D` | 金 / Gold | 5 |

JSONの `bank` と `players[].gems` もこの順番です。

### 取得

```text
take:WUG
take:WW
take:WUG/return:D
```

`/return:` は10トークン超過時の返却です。返却が必要な局面で省略すると、その手に一致しません。

### 場カード予約

```text
reserve:C12
reserve:C12/return:D
```

`C12` はカードIDです。

### 伏せ予約

```text
reserve:L1
reserve:L2/return:W
```

`L1`, `L2`, `L3` はレベルです。`--moves` では既存エンジンの合法手として適用できますが、探索本体ではデフォルトで伏せ予約を除外します。探索にも含める場合だけ `--allow-deck-reserve` を指定してください。

### 購入

```text
buy:C7
buy:C7/pay:W0U0G0R4K0D0
buy:C7/gold:R1
```

`buy:C7` のように支払いを省略した場合は、一致する合法購入手の中から金使用量が少ないものを選びます。支払いを固定したい場合は `/pay:` を使って6色すべての支払い枚数を書いてください。

`/gold:` は金をどの色として使うかだけを指定します。

```text
buy:C18/gold:W1R1
```

この例は金1枚を白、金1枚を赤として支払う指定です。

### 貴族選択

```text
noble:N3
```

複数貴族を同時に獲得可能な場合、エンジンは `waiting_noble` 状態になります。その場合は `noble:N3` のように別手として渡してください。

探索中に発生した貴族選択は、ソルバー側で手番プレイヤーの意思決定として展開します。

## 盤面JSONの渡し方

`--state-json` は、まず `seed` から通常局面を作り、その後JSONに書かれたフィールドだけを上書きします。したがって、初期局面やseed局面を少しだけ変えたい場合は差分だけを書けます。完全な任意局面を渡したい場合は、盤面・山札・プレイヤー状態をすべて明示してください。

最小例:

```json
{
  "seed": 42,
  "current_player": 0
}
```

部分上書き例:

```json
{
  "seed": 42,
  "current_player": 1,
  "turn": 8,
  "bank": [4, 4, 3, 4, 4, 5],
  "players": [
    {
      "gems": [0, 0, 1, 0, 0, 0],
      "points": 14
    },
    {
      "gems": [1, 0, 0, 0, 0, 0],
      "points": 10
    }
  ]
}
```

完全指定に近い例:

```json
{
  "seed": 0,
  "current_player": 0,
  "turn": 12,
  "final_round": false,
  "waiting_noble": false,
  "winner": -1,
  "bank": [4, 4, 4, 4, 4, 5],
  "visible": [
    [0, 8, 16, 24],
    [40, 46, 52, 58],
    [70, 74, 78, 82]
  ],
  "decks": [
    [1, 2, 3, 4, 5, 6],
    [41, 42, 43, 44],
    [71, 72, 73]
  ],
  "nobles": [0, 1, 2],
  "players": [
    {
      "gems": [0, 0, 0, 0, 0, 0],
      "bonuses": [3, 3, 3, 0, 0],
      "points": 14,
      "reserved": [-1, -1, -1],
      "reserved_is_hidden": [false, false, false],
      "reserved_count": 0,
      "purchased_count": 9,
      "purchased_cards": [7, 9, 10, 11, 45, 47, 53, 60, 75],
      "acquired_nobles": []
    },
    {
      "gems": [0, 0, 0, 0, 0, 0],
      "bonuses": [2, 2, 2, 1, 1],
      "points": 9,
      "reserved": [12, -1, -1],
      "reserved_is_hidden": [false, false, false],
      "reserved_count": 1,
      "purchased_count": 8,
      "purchased_cards": [13, 14, 15, 17, 48, 49, 50, 76],
      "acquired_nobles": []
    }
  ]
}
```

実際に使う局面では、同じカードIDが `visible`, `decks`, `reserved`, `purchased_cards` に重複して出ないようにしてください。JSONローダーは形と範囲の一部を検査しますが、完全なルール整合性までは検査しません。

### トップレベルフィールド

| フィールド | 型 | 内容 |
| --- | --- | --- |
| `seed` | int | 盤面上書き前に使う初期化seed。省略時は `0`。 |
| `simple_payment_mode` | bool | trueなら購入支払いの合法手生成を簡略化します。通常はfalse。 |
| `blank_refill_mode` | bool | trueならエンジン補充時に場を空欄にします。通常はfalse。 |
| `current_player` | int | 手番プレイヤー。`0` または `1`。 |
| `turn` | int | ターン番号。 |
| `final_round` | bool | 最終ラウンド中か。 |
| `waiting_noble` | bool | 貴族選択待ち状態か。通常はfalse。 |
| `winner` | int | `-1`: 継続中、`0`/`1`: 勝者、`-2`: 引き分け。 |
| `bank` | int[6] | 銀行トークン数。順番は `[W,U,G,R,K,D]`。 |
| `visible` | int[3][4] | 場カード。`visible[0]` がレベル1、`visible[2]` がレベル3。空欄は `-1`。 |
| `decks` | int[3][] | 未公開カード集合。`decks[0]` がレベル1、`decks[2]` がレベル3。 |
| `nobles` | int[] | 場に残っている貴族ID。 |
| `players` | object[2] | 各プレイヤー状態。 |

### プレイヤーフィールド

| フィールド | 型 | 内容 |
| --- | --- | --- |
| `gems` | int[6] | 所持トークン数。順番は `[W,U,G,R,K,D]`。 |
| `bonuses` | int[5] | 所持ボーナス数。順番は `[W,U,G,R,K]`。 |
| `points` | int | 現在得点。 |
| `reserved` | int[3] | 予約カードID。空きは `-1`。 |
| `reserved_is_hidden` | bool[3] | 伏せ予約かどうか。詰み問題では通常すべてfalseにします。 |
| `reserved_count` | int | 予約枚数。`reserved` の非 `-1` 枚数と一致させてください。 |
| `purchased_count` | int | 購入済みカード枚数。同点時の勝者判定に使います。 |
| `purchased_cards` | int[] | 購入済みカードID。 |
| `acquired_nobles` | int[] | 獲得済み貴族ID。 |

### カードIDとレベル

カードIDは `0` から `89` です。

| レベル | ID範囲 |
| --- | --- |
| 1 | `0`-`39` |
| 2 | `40`-`69` |
| 3 | `70`-`89` |

`decks[0]` にはレベル1のカードだけ、`decks[1]` にはレベル2のカードだけ、`decks[2]` にはレベル3のカードだけを入れてください。違うレベルのカードを入れると `InvalidInput` になります。

カード内容を確認したい場合は、Pythonから参照できます。

```bash
python - <<'PY'
import csplendor as cs
for card in cs.get_all_cards():
    print(card.id, card.level, card.points, int(card.bonus), list(card.cost))
PY
```

貴族IDも同様です。

```bash
python - <<'PY'
import csplendor as cs
for noble in cs.get_all_nobles():
    print(noble.id, noble.points, list(noble.requirement))
PY
```

## 山札とめくれ分岐

探索開始後、`decks` は順序付き山札ではなく、未公開カード集合として扱われます。場カード購入または場カード予約で補充が起きる場合、同レベルの `decks[level]` に含まれる全カードが「めくれ候補」になります。

例えば `decks[0]` が `[1, 2, 3]` の状態でレベル1の場カードを予約すると、ソルバーは次の3通りをすべて調べます。

```text
reveal C1
reveal C2
reveal C3
```

攻撃側の手でも、防御側の手でも、めくれは全称分岐です。つまり、攻撃側の選んだ手が詰み手と認められるには、どのカードがめくれても勝てる必要があります。

注意点:

- `--moves` は探索前に通常エンジンで適用されます。この段階で補充が起きる場合だけ、`decks[level]` の最後の要素がエンジン上のトップカードとして使われます。
- 探索本体に入った後は、`decks[level]` の順序は無視されます。
- 予約済みカード購入では場の補充が起きないため、めくれ分岐は発生しません。
- 山札が空なら補充なしの1分岐になります。

## Python APIから使う

簡単な呼び出し:

```python
import csplendor as cs
from scripts.mate_solver import SolverOptions, solve_game

game = cs.Game(seed=42)
options = SolverOptions(max_nodes=200000, time_limit=10.0, include_proof=False)
result = solve_game(game, attacker=0, max_depth=2, options=options)

print(result.status)
print(result.to_dict()["stats"])
```

`SolverState` を直接作ると、エンジンの `board.decks` とは別に未公開カード集合を渡せます。

```python
import csplendor as cs
from scripts.mate_solver import MateSolver, SolverOptions, SolverState

game = cs.Game(seed=0)
state = SolverState(
    game=game,
    unseen_by_level=(
        frozenset([1, 2, 3]),
        frozenset([41, 42]),
        frozenset([71]),
    ),
)

solver = MateSolver(attacker=0, max_depth=2, options=SolverOptions())
result = solver.solve(state)
```

通常は `SolverState.from_game(game)` または `solve_game()` を使えば十分です。

## 運用上の注意

- `Mate` のみを問題集生成に採用してください。`NoMate` は指定深さ内に詰みなし、`Unknown` は未判定です。
- 深さを1増やすと、防御手とめくれ分岐の組み合わせで探索量が大きく増えます。
- CPUを使い切って探索したい場合は `--jobs 0` を指定してください。root候補手ごとのプロセス並列なので、候補手が少ない局面や浅い探索では効果が小さいことがあります。
- 証明木は大きくなりやすいので、大量検証では `--no-proof` を使ってください。
- 伏せ予約は詰み問題用ルールとしてデフォルト除外です。通常は `--allow-deck-reserve` を使わないでください。
- JSONローダーは完全な局面合法性検証をしません。カード重複、購入枚数、得点、ボーナス、銀行トークン数は呼び出し側で整合させてください。

## USI position / SPNの渡し方

`doc/USI.md` の `position` コマンドとSPNを読み込めます。以下の3形式はいずれも有効です。

```text
position startpos 2 moves take:WUG reserve:C12
```

```text
position bank:W4U4G4R4K4D5 | visible:L1[0,8,16,24]L2[40,46,52,58]L3[70,74,78,82] | decks:36,26,16 | nobles:[0,3,7] | P0:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] | P1:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] 0 moves take:WUG
```

```text
bank:W4U4G4R4K4D5 | visible:L1[0,8,16,24]L2[40,46,52,58]L3[70,74,78,82] | decks:36,26,16 | nobles:[0,3,7] | P0:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] | P1:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] 0
```

`--position-file` に複数行のプロトコルログを渡した場合は、最後に現れる `position ...` 行を読み込みます。`position` 行がない場合は、ファイル全体をSPNとして連結して読み込みます。

SPNの基本形:

```text
<bank> | <visible> | <decks> | <nobles> | <player0> | <player1> <current_player>
```

各セクション:

```text
bank:W4U4G4R4K4D5
visible:L1[0,8,16,24]L2[40,46,52,58]L3[70,74,78,82]
decks:36,26,16
nobles:[0,3,7]
P0:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[]
P1:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[]
0
```

SPNの `decks` はカードIDではなく残数です。ソルバーは次の情報から未公開カード集合を復元します。

- 全90枚のカードID
- `visible` に出ているカードID
- 各プレイヤーの `reserved` にあるカードID
- 各プレイヤーの `bought` に具体カードIDがある場合、そのカードID
- `decks:<L1>,<L2>,<L3>` の残数

盤面編集エディタなどで購入済みカードIDを指定しない場合は、`bought:[_,_,_]` のように `_` を並べて購入枚数だけを渡せます。この場合、`_` は同点時の購入枚数 `purchased_count` には反映されますが、未公開カード集合からは除外されません。つまり「めくれているカードと予約カード以外はすべて山札に存在する」という仮定で検証します。

```text
P0:...;reserved:[68];bought:[_,_,_,_,_]
```

このため、SPN内の残数と既知カードIDから推定される残数が一致しない場合は `InvalidInput` になります。たとえばレベル1は全40枚なので、レベル1の場カード4枚だけが既知なら、`decks` のレベル1残数は `36` である必要があります。`bought:[_,_]` はこの残数から引かれません。

伏せ予約 `?L1` / `?L2` / `?L3` はUSI仕様上は表現できますが、詰みソルバーでは正確な合法手生成と購入可否判定にカードIDが必要です。そのため、ソルバー入力では `reserved:[?L1]` のような伏せ予約は `InvalidInput` とし、具体的なカードIDを渡してください。

```text
reserved:[12]       # OK
reserved:[?L1]      # ソルバーではInvalidInput
```
