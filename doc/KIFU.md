# Splendor 棋譜ファイル仕様 (.kifu)

**Version:** 1.0 Draft  
**Date:** 2026-02-11  
**依存仕様:** [USI.md](USI.md) — アクション記法・SPN盤面記法・宝石色記号

---

## 1. 概要

`.kifu` ファイルは Splendor の対局記録をテキスト形式で保存するためのファイルフォーマットである。
将棋の KIF/KI2/CSA 棋譜フォーマットを参考に、USI プロトコルのアクション記法 (USI Move Notation) および盤面記法 (SPN) と完全に互換性を持つよう設計されている。

### 1.1 設計原則

1. **人間可読性**: プレーンテキストで記述し、テキストエディタで直接閲覧・編集可能。
2. **USI 互換**: アクション表記は USI Move Notation をそのまま使用する。
3. **完全再現性**: 初期盤面 (SPN) + 手順で対局を完全に再現可能。
4. **拡張性**: メタデータヘッダにより任意の情報を付加可能。
5. **エンコーディング**: UTF-8。改行は `\n` (LF)。

---

## 2. ファイル構造

```
[ヘッダセクション]
（空行）
[盤面セクション]
（空行）
[手順セクション]
（空行）
[結果セクション]
```

各セクションは空行で区切る。

---

## 3. ヘッダセクション

`Key: Value` 形式のメタデータ行。順不同。

### 3.1 必須ヘッダ

| キー | 説明 | 例 |
|------|------|----|
| `Format` | ファイルフォーマット識別子 | `Splendor KIFU v1.0` |
| `Players` | プレイヤー数 | `2` |
| `Player0` | 先手プレイヤー名 | `Genbu v2.0` |
| `Player1` | 後手プレイヤー名 | `DeepSets v1.0` |

### 3.2 省略可能ヘッダ

| キー | 説明 | 例 |
|------|------|----|
| `Date` | 対局日時 (ISO 8601) | `2026-02-11T21:00:00+09:00` |
| `Player2`, `Player3` | 3-4人戦時のプレイヤー名 | `Human` |
| `Event` | 大会・イベント名 | `AI Championship 2026` |
| `Round` | ラウンド番号 | `3` |
| `Result` | 対局結果 | `0` (勝者のプレイヤー番号), `draw` |
| `TimeControl` | 持ち時間 | `30s/move` |
| `Seed` | 乱数シード（再現用） | `42` |
| `Engine0` | P0 のエンジン情報 | `Genbu v2.0 (MCTS 1600)` |
| `Engine1` | P1 のエンジン情報 | `DeepSets v1.0 (MCTS 800)` |
| `Tags` | 自由タグ (カンマ区切り) | `distillation,test` |

---

## 4. 盤面セクション

初期盤面を SPN (Splendor Position Notation) で記述する。

```
Position: <SPN文字列>
```

または初期局面の場合:

```
Position: startpos 2
```

> [!NOTE]
> Splendor はカードの初期配置がランダムのため、`startpos` だけでは盤面を一意に決定できない。
> 再現性が必要な場合は `Seed` ヘッダ、または完全な SPN を使用すること。

---

## 5. 手順セクション

1手ごとに1行。以下の書式:

```
<turn>. P<player> <usi_move> [<time_ms>] [# <comment>]
```

| フィールド | 説明 | 必須 |
|-----------|------|------|
| `<turn>` | 手番号（1始まり、全プレイヤー通し番号） | ○ |
| `P<player>` | プレイヤー番号 (P0, P1, ...) | ○ |
| `<usi_move>` | USI アクション記法 | ○ |
| `[<time_ms>]` | 思考時間（ミリ秒、角括弧で囲む） | 任意 |
| `[# <comment>]` | コメント（`#` 以降） | 任意 |

### 5.1 例

```
1. P0 take:WUG [2341]
2. P1 take:RKG [1520]
3. P0 take:RR [890]
4. P1 reserve:C71 [3200] # レベル3カードを確保
5. P0 buy:C27 [1100]
6. P1 take:WUK [950]
7. P0 buy:C2/gold:K1 [2800] # 金を黒に充当
8. P1 buy:C42 [1200]
9. P0 noble:N5 [0] # 自動訪問（候補が1つの場合）
```

### 5.2 貴族訪問の記録

貴族訪問は独立した手として記録する。購入直後に自動発生する場合でも、独立した行に記載する。

```
15. P0 buy:C84 [3500]
16. P0 noble:N3 [0]
```

> [!NOTE]
> 候補が1つの場合は `[0]`（0ミリ秒 = 自動選択）で記録する。
> 複数候補から選択した場合はその思考時間を記載する。

### 5.3 パスの記録

```
20. P0 pass [0]
```

### 5.4 コメント専用行

手順とは独立して、コメント行を挿入できる。

```
# 中盤戦: P0がレベル3カードの購入を狙い始める
12. P0 reserve:C75 [4100]
```

---

## 6. 結果セクション

対局結果を記載する。

```
Result: <result_type> [<detail>]
```

| result_type | 説明 |
|------------|------|
| `P0_WIN` | P0 の勝利 |
| `P1_WIN` | P1 の勝利 |
| `P2_WIN`, `P3_WIN` | 3-4人戦時 |
| `DRAW` | 引き分け（同点 + 同カード枚数） |
| `DRAW_TIMEOUT` | 最大ターン数到達による引き分け |

追加情報を同セクションに記載可能:

```
Result: P0_WIN
FinalScores: P0=16 P1=12
TotalTurns: 54
```

| キー | 説明 |
|------|------|
| `FinalScores` | 各プレイヤーの最終スコア |
| `TotalTurns` | 総手数 |
| `FinalCards` | 各プレイヤーの購入カード枚数 |

---

## 7. 完全なファイル例

```kifu
Format: Splendor KIFU v1.0
Players: 2
Player0: Genbu v2.0
Player1: DeepSets v1.0
Date: 2026-02-11T21:00:00+09:00
Event: AI Benchmark
Engine0: Genbu v2.0 (MCTS 1600, cpuct=2.5)
Engine1: DeepSets v1.0 (MCTS 800, cpuct=2.5)
Seed: 42

Position: bank:W4U4G4R4K4D5 | visible:L1[2,11,18,27]L2[41,49,53,60]L3[71,75,79,84] | decks:36,24,12 | nobles:[1,5,9] | P0:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] | P1:gems:W0U0G0R0K0D0;bonuses:W0U0G0R0K0;points:0;reserved:[];bought:[] 0

# === 序盤 ===
1. P0 take:WUG [2341]
2. P1 take:RKG [1520]
3. P0 take:RR [890]
4. P1 reserve:C71 [3200]
5. P0 buy:C27 [1100]
6. P1 take:WUK [950]

# === 中盤 ===
7. P0 buy:C2/gold:K1 [2800]
8. P1 buy:C49 [1200]
9. P0 take:WUG [500]
10. P1 take:RR [620]
11. P0 buy:C41 [1800]
12. P1 buy:C53/gold:R1 [2100]

# === 終盤 ===
13. P0 buy:C60 [3500]
14. P1 buy:C71/gold:W2U1 [4200]
15. P0 buy:C84 [2900]
16. P0 noble:N5 [0]

Result: P0_WIN
FinalScores: P0=16 P1=12
TotalTurns: 16
```

---

## 8. バリエーション記法（解析用・拡張）

棋譜解析やAI間比較で、分岐手順を記録したい場合に使用する。

```
V{
  <分岐手順>
}
```

### 8.1 例

```
10. P1 take:RR [620]
V{
  # AI候補手2: 予約の方が評価値は高かった
  10. P1 reserve:C75 [620]
  11. P0 buy:C41 [1800]
}
11. P0 buy:C41 [1800]
```

---

## 9. AI 解析注釈（拡張）

各手に AI の評価情報を付加できる。`@` プレフィックスで記述する。

```
<turn>. P<player> <move> [<time>]
  @eval <winrate>
  @pv <move1> <move2> ...
  @policy <move1>:<prob1> <move2>:<prob2> ...
  @nodes <n>
```

### 9.1 例

```
7. P0 buy:C2/gold:K1 [2800]
  @eval 0.62
  @nodes 1600
  @pv buy:C2/gold:K1 take:WUG buy:C41
  @policy buy:C2/gold:K1:0.45 take:WUG:0.22 reserve:C75:0.18
```

---

## 10. パーサー実装ガイド

### 10.1 読み込み手順

```
1. UTF-8 テキストとして読み込み
2. 空行で3-4セクションに分割
3. ヘッダ: "Key: Value" を辞書にパース
4. 盤面: "Position:" 行から SPN をパース
5. 手順: 各行を正規表現で分解
6. 結果: "Result:" 行をパース
```

### 10.2 手順行の正規表現

```regex
^(\d+)\.\s+P(\d+)\s+(\S+)(?:\s+\[(\d+)\])?(?:\s+#\s*(.*))?$
```

| グループ | 内容 |
|---------|------|
| 1 | 手番号 |
| 2 | プレイヤー番号 |
| 3 | USI アクション文字列 |
| 4 | 思考時間 (ms)、省略可 |
| 5 | コメント、省略可 |

### 10.3 注釈行の正規表現

```regex
^\s+@(\w+)\s+(.+)$
```

---

## 11. 他フォーマットとの変換

| 変換元 | 変換先 | 方法 |
|--------|--------|------|
| `.pkl` (ori) | `.kifu` | `board_ori` を SPN に変換、ori アクションを USI 記法に変換 |
| `.kifu` | `.pkl` | USI 記法をパースし、csplendor Game で再生して各ステップの state を pickle |
| `.kifu` | JSON | 各フィールドを JSON オブジェクトにマッピング |
| BGA ログ | `.kifu` | BGA スクレイパーの出力を USI 記法に変換 |

---

## 12. MIME タイプ・ファイル拡張子

| 項目 | 値 |
|------|----|
| ファイル拡張子 | `.kifu` |
| MIME タイプ | `application/x-splendor-kifu` |
| エンコーディング | UTF-8 (BOM なし) |
| 改行コード | LF (`\n`) |
