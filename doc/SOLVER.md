# ソルバー実行方法

## DFPN詰みソルバー

改良版のDFPN詰みソルバーは `scripts/dfpn_mate_solver.py` で実行します。盤面入力は `scripts/mate_solver.py` と同じUSI準拠の指定を使います。

```bash
python scripts/dfpn_mate_solver.py --position "position startpos 2" --attacker 0 --max-depth 2 --pretty
```

SPNを直接渡す場合も `position` コマンドとして指定できます。

```bash
python scripts/dfpn_mate_solver.py \
  --position "position bank:W4U4G4R4K4D5 | visible:L1[0,1,2,3]L2[40,41,42,43]L3[70,71,72,73] | decks:36,26,16 | nobles:[0,1,2] | P0:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] | P1:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] | 0" \
  --attacker 0 \
  --max-depth 2 \
  --pretty
```

USIの `moves` 付き入力も受け付けます。

```bash
python scripts/dfpn_mate_solver.py \
  --position "position startpos 2 moves take:WUG reserve:C12" \
  --attacker 0 \
  --max-depth 3
```

局面をファイルから読む場合は `--position-file` を使います。ファイルには `position ...` 形式、またはSPN本文を記述します。

```bash
python scripts/dfpn_mate_solver.py --position-file position.txt --attacker 0 --max-depth 3 --pretty
```

追加のUSI着手をコマンドラインで適用する場合は `--moves` を使います。カンマ区切り、または複数回指定できます。

```bash
python scripts/dfpn_mate_solver.py \
  --position "position startpos 2" \
  --moves "take:WUG,reserve:C12" \
  --moves "buy:C12/pay:W0U0G0R0K0D0" \
  --attacker 0 \
  --max-depth 3
```

主なオプションは次の通りです。

- `--attacker 0|1`: 詰みを証明する側のプレイヤー。
- `--max-depth N`: 攻撃側手番を何手先まで読むか。
- `--node-limit N`: 探索ノード上限。
- `--time-limit SEC`: 探索時間上限。
- `--allow-deck-reserve`: 山札予約手も合法手候補に含める。
- `--no-memo`: 置換表を使わない。
- `--no-proof`: 証明木・反証木を出力しない。
- `--pretty`: JSONを整形して出力する。

終了コードは、`Mate` / `NoMate` の確定時が `0`、入力エラーが `1`、上限到達などで `Unknown` の場合が `2` です。

出力はJSONです。`status` には `Mate`、`NoMate`、`Unknown`、`InvalidInput` のいずれかが入ります。`stats.root_proof_number` と `stats.root_disproof_number` でルート局面の証明数・反証数を確認できます。
