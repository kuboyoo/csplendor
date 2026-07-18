# ソルバー実行方法

## DFPN詰みソルバー

改良版のDFPN詰みソルバーは `scripts/dfpn_mate_solver.py` で実行します。盤面入力は `scripts/mate_solver.py` と同じUSI準拠の指定を使います。

```bash
python scripts/dfpn_mate_solver.py --position "position startpos 2" --attacker 0 --max-depth 2 --pretty
```

SPNを直接渡す場合も `--position` に指定できます。

```bash
python scripts/dfpn_mate_solver.py \
  --position "bank:W4U4G4R4K4D5 | visible:L1[0,1,2,3]L2[40,41,42,43]L3[70,71,72,73] | decks:36,26,16 | nobles:[0,1,2] | P0:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] | P1:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] | 0" \
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

並列探索を使う場合は `--jobs` を指定します。

```bash
python scripts/dfpn_mate_solver.py \
  --position-file position.txt \
  --attacker 0 \
  --max-depth 3 \
  --jobs 4 \
  --time-limit 60 \
  --node-limit 1000000
```

主なオプションは次の通りです。

- `--attacker 0|1`: 詰みを証明する側のプレイヤー。
- `--max-depth N`: 攻撃側手番を何手先まで読むか。
- `--node-limit N`: 探索ノード上限。`0` で無効。
- `--time-limit SEC`: 探索時間上限。`0` で無効。
- `--simple-payment`: 購入時の支払いを金を温存する代表パターンへ絞る。
- `--jobs N`: ルート直下の候補手をプロセス並列で探索する。`0` はCPU数を使う。
- `--no-threat-reveal-pruning`: 即勝ち脅威に基づくめくれ枝刈りを無効にし、従来通り全めくれを読む。
- `--no-equivalence-hash`: 脅威同値類ハッシュを無効にし、厳密な局面キーを使う。
- `--parallel-tt-limit N`: 並列ワーカーごとの置換表エントリ上限。`0` でワーカー内メモを無効化。
- `--parallel-start-method spawn|fork|forkserver`: 並列ワーカーの起動方式。デフォルトはC++拡張ロード後のforkデッドロックを避けやすい `spawn`。
- `--progress`: 探索進捗を標準エラーに定期表示する。最終結果JSONは標準出力のまま。
- `--progress-interval SEC`: 進捗表示間隔。
- `--allow-deck-reserve`: 山札予約手も合法手候補に含める。
- `--no-memo`: 置換表を使わない。
- `--no-proof`: 証明木・反証木を出力しない。
- `--pretty`: JSONを整形して出力する。

終了コードは、`Mate` / `NoMate` の確定時が `0`、入力エラーが `1`、上限到達などで `Unknown` の場合が `2` です。

出力はJSONです。`status` には `Mate`、`NoMate`、`Unknown`、`InvalidInput` のいずれかが入ります。`stats.root_proof_number` と `stats.root_disproof_number` でルート局面の証明数・反証数を確認できます。

長時間探索では `--progress` を付けると、探索中のノード数、探索速度、到達深さ、証明数、反証数、めくれ分岐数、省略できためくれ数などを標準エラーに表示します。進捗は同じ行を上書きするため、ログの行数は増えません。並列時の `nodes` には、完了済みタスクだけでなく実行中ワーカーの途中ノード数も共有カウンタ経由で含まれます。

```bash
python scripts/dfpn_mate_solver.py \
  --position-file position.txt \
  --attacker 0 \
  --max-depth 3 \
  --jobs 8 \
  --no-proof \
  --progress \
  --progress-interval 5
```

進捗行は標準エラーへ出るため、標準出力をファイルへリダイレクトしても結果JSONだけを保存できます。

デフォルトでは、表示カードの補充時に「そのカードをどちらかのプレイヤーが即購入すると15点以上になるか」を判定し、即勝ちに関わる危険カードだけを個別に読みます。カード素点だけでなく、購入後のボーナスで貴族条件を満たす場合の+3点も考慮します。危険でないカード群は代表1枚に畳むため、`stats.threat_pruned_reveals` で省略された安全めくれ数を確認できます。

また、置換表のキーにはデフォルトで脅威同値類ハッシュを使います。即勝ちに関わるカードはIDを残し、それ以外のカードはレベル、点数、色、現在の各プレイヤーから見た支払い不足、購入後に成立しうる貴族集合などで正規化します。これにより、カードIDは違っても探索上ほぼ同じ脅威として扱える局面を再利用しやすくします。厳密な局面一致に戻したい場合は `--no-equivalence-hash` を指定してください。

合法手の探索順は、`--max-depth` の範囲内で購入できそうな加点期待値の高いカードを優先します。カード素点に加えて、購入後に貴族条件を満たす場合の+3点を含めて評価します。購入手そのものを最優先し、そのカード購入に近づくトークン取得手、対象カードの予約手を続けて優先します。この move ordering は読む順序だけを変え、合法手自体は削りません。

購入時の支払いパターンが多すぎる場合は `--simple-payment` を使えます。これは同じカード購入について、金をできるだけ温存する代表支払いに寄せるモードです。探索は速くなりますが、支払い方の違いによる細かい将来局面は読まなくなるため、厳密探索ではなくなります。

DFPN版の並列化は、ルート直下の候補手をさらに「候補手 + めくれ結果 + 防御側応手 + 防御側めくれ結果」単位へ分割します。ワーカーが1タスクを調べ終わるたびに次のタスクを投入する動的スケジューリングを使うため、軽い枝が先に終わってもCPUを遊ばせにくくなっています。さらに並列ワーカーではメモリ削減のため、証明木の保持を無効化し、探索中に現在使っていない枝の子孫ノードを破棄します。ワーカー内の置換表は `--parallel-tt-limit` の範囲でのみ保持します。そのため並列時は `Mate` / `NoMate` の判定を優先し、詳細な `proof_tree` / `refutation` は省略される場合があります。

並列時の `--node-limit` はバッチ内の各ワーカーへ分配されるため、単一プロセス実行と完全に同じ探索順にはなりません。長時間探索でメモリを抑えたい場合は、まず `--jobs 2` から試し、メモリに余裕がある場合だけ増やしてください。`--jobs 8` は局面によっては8本分の探索が同時に走るため、メモリ使用量が大きくなります。

## DFPN 枝刈りオプション

`scripts/dfpn_mate_solver.py` は、`--simple-payment` に加えて以下の枝刈りを備えています。

- 返却パターン代表化: デフォルト有効です。支払い・返却だけが異なり、探索上同じ意味になる合法手は代表手だけを展開します。無効化する場合は `--no-return-pattern-pruning` を付けます。
- 即勝ち手 / 即防御手の終端判定: デフォルト有効です。展開前に、手番側が直ちに15点へ到達できる購入・貴族訪問があるかを判定し、詰み確定または防御成立として打ち切ります。無効化する場合は `--no-immediate-terminal-pruning` を付けます。
- 残り手数内の最大到達点による枝刈り: デフォルト有効です。攻撃側が残り手数で到達しうる楽観的な最大点が15点未満なら、その枝は詰みなしとして打ち切ります。無効化する場合は `--no-upper-bound-pruning` を付けます。
- 防御側応手の脅威対応フィルタ: デフォルト無効です。`--defender-threat-filter` を付けると、防御側の応手を「攻撃側の即時得点脅威を消す手」に絞ります。強い枝刈りなので、まずは検証用として使ってください。

例:

```bash
python scripts/dfpn_mate_solver.py \
  --position "..." \
  --attacker 0 \
  --max-depth 3 \
  --time-limit 0 \
  --node-limit 0 \
  --jobs 1 \
  --simple-payment \
  --defender-threat-filter \
  --progress
```
