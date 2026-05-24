# genbu.pt アーキテクチャ仕様書

> **対象モデル**: `alphazero-general-ori/HeianKyo/genbu.pt`
> **フレームワーク**: PyTorch
> **用途**: 蒸留学習の教師モデル、csplendor-native モデル設計の参考

## 概要

genbu.pt は alphazero-general-ori フレームワークで学習されたSplendor AI モデルである。
本ドキュメントでは、ネットワーク構造と盤面のテンソル変換仕様を詳細に記載する。

---

## 1. 盤面テンソル変換 (Board → Tensor)

### 1.1 テンソル形状

```
観測サイズ: (32 + 10*N + N², 7)

N=2の場合: (56, 7) = 392 要素
```

各行は7要素のベクトルで、以下の意味を持つ：
```
[White, Blue, Green, Red, Black, Gold, Points]
```

### 1.2 テンソル構成 (N=2プレイヤー時)

| 行範囲 | 行数 | 内容 | 詳細 |
|---|---|---|---|
| 0 | 1 | 銀行 (Bank) | [W, B, G, R, K, Gold, Round] |
| 1-24 | 24 | 場のカード (12枚×2行) | カード i: 行 2i=コスト, 行 2i+1=ボーナス |
| 25-30 | 6 | デッキ残数 | Tier t: 行 2t=残枚数/色, 行 2t+1=ビットマスク |
| 31-33 | 3 | 貴族 (Nobles) | 各貴族の必要ボーナス |
| 34-35 | 2 | プレイヤーのジェム | players_gems[p] |
| 36-41 | 6 | プレイヤーの貴族 | players_nobles[p×3:(p+1)×3] |
| 42-43 | 2 | プレイヤーの購入済みカード | players_cards[p] |
| 44-55 | 12 | プレイヤーの予約カード | players_reserved[p×6:(p+1)×6] |

### 1.3 カードのエンコーディング

各カードは2行で表現：
```
行 2i:   [cost_W, cost_B, cost_G, cost_R, cost_K, 0, 0]  # コスト
行 2i+1: [bonus_W, bonus_B, bonus_G, bonus_R, bonus_K, 0, points]  # 獲得ボーナス
```

### 1.4 スロットベース設計の特徴

現行のgenbu.ptは**スロットベース**で盤面を表現している：
- 場の12スロット（3 Level × 4スロット）は固定位置にマップされる
- 同じカードでもスロットが異なれば、テンソル上の位置が異なる
- 学習済みモデルはカードのスロット位置に依存した表現を学習している

---

## 2. ネットワーク構造 (SplendorNNet)

### 2.1 全体アーキテクチャ

```
入力: (batch, 7, 56) ← 転置された盤面テンソル

┌────────────────────────────────────────────────────────────┐
│  dense2d_1: Linear(56→128) + BN(7) + ReLU                  │
│           → Linear(128→128) + ReLU                         │
├────────────────────────────────────────────────────────────┤
│  partialgpool_1: DenseAndPartialGPool(128→128, groups=4)   │
│           → Dropout                                        │
├────────────────────────────────────────────────────────────┤
│  dense2d_3: Linear(128→128) + ReLU                         │
│           → Dropout                                        │
├────────────────────────────────────────────────────────────┤
│  flatten_and_gpool: FlattenAndPartialGPool                 │
│           → MaxPool + AvgPool on first 64 dims             │
├────────────────────────────────────────────────────────────┤
│  dense1d_4: Linear(64*4 + 64*7 → 128) + ReLU               │
│           → Dropout                                        │
├────────────────────────────────────────────────────────────┤
│  partialgpool_4: DenseAndPartialGPool(128→128, groups=4)   │
│           → Dropout                                        │
├────────────────────────────────────────────────────────────┤
│  dense1d_5: Linear(128→128) + BN(1) + ReLU                 │
│           → Linear(128→128) + ReLU → Dropout               │
├────────────────────────────────────────────────────────────┤
│  partialgpool_5: DenseAndPartialGPool(128→128, groups=4)   │
│           → Dropout                                        │
└────────────────────────────────────────────────────────────┘
                    ↓
    ┌───────────────┼───────────────┐
    ↓               ↓               ↓
 output_PI       output_V       output_SDIFF
 Linear(128)     Linear(128)    Linear(128)
    ↓               ↓               ↓
 Linear(406)    Linear(2)      Linear(2×31)
    ↓               ↓               ↓
 log_softmax      tanh          log_softmax
 (方策)          (価値)         (スコア差分布)
```

### 2.2 DenseAndPartialGPool レイヤー

部分的なグローバルプーリングを行うカスタムレイヤー：

```python
# 入力の後半部分を nb_groups × nb_items_in_groups のグループに分割
# 各グループに対して MaxPool と AvgPool を適用
# 前半部分（dense_input）は通常の Linear 変換

input = [..., group_1, group_2, ..., group_k, dense_part]
         ↓         ↓              ↓          ↓
       MaxPool  MaxPool        MaxPool   Linear
       AvgPool  AvgPool        AvgPool
         ↓         ↓              ↓          ↓
output = [max_1, max_2, ..., max_k, avg_1, ..., avg_k, dense_out]
```

**目的**: カードやジェムの置換不変性を部分的に学習できるようにする。

### 2.3 出力

| 出力 | サイズ | 活性化 | 説明 |
|---|---|---|---|
| pi (方策) | 406 | log_softmax | アクション確率分布 |
| v (価値) | 2 | tanh | 各プレイヤーの勝率 [-1, 1] |
| sdiff | 2×31 | log_softmax | スコア差の確率分布 |

---

## 3. アクション空間 (genbu)

### 3.1 アクション定義 (406通り)

```
ID範囲        | アクション種別 | パターン数
-------------|---------------|------------
0-11         | BUY_VISIBLE   | 12 (3 level × 4 slot)
12-26        | RESERVE       | 15 (12 visible + 3 deck)
27-29        | BUY_RESERVED  | 3
30-59        | TAKE_GEMS     | 30 (25 different + 5 same)
60-269       | EXCHANGE      | 210 (take + give combinations)
270-289      | TAKE1_GIVE1   | 20
290-364      | RESERVE_GIVE1 | 75 (15 reserve × 5 colors)
365-404      | TAKE3_GIVE3   | 40
405          | PASS          | 1
-------------|---------------|------------
合計         |               | 406
```

### 3.2 V3アクション空間との差異

| 特徴 | genbu (V1相当) | V3 (新設計) |
|---|---|---|
| PURCHASE | スロットベース (15通り) | カードIDベース (2035通り) |
| 支払いパターン | 暗黙的 (1パターン/カード) | 明示的 (gold_as 埋め込み) |
| VISIT_NOBLE | 暗黙的 (自動選択) | IDベース (12通り) |
| アクション総数 | 406 | 3133 |

---

## 4. csplendor-native モデル設計への示唆

### 4.1 カードIDベース表現の検討

**現行genbu方式（スロットベース）:**
```
テンソル位置 = slot_index
slot 0 にカード#5 → テンソル[0:2]
slot 1 にカード#5 → テンソル[2:4]  ← 別の表現になる
```

**提案方式（カードIDベース）:**
```
テンソル位置 = card_id
card_id=5 → テンソル[5×2:5×2+2]  ← スロットに関係なく同じ位置
見えていないカードはマスク（ゼロ埋め）
```

### 4.2 スパース性の懸念

カードIDベースで90枚すべてのカードに固定インデックスを割り当てると：

**テンソルサイズ:**
```
90 cards × 2 rows × 7 features = 1260 要素 (カード部分のみ)
vs
12 slots × 2 rows × 7 features = 168 要素 (現行)
```

**スパース性:**
- ゲーム中、見えているカードは最大12枚（場）+ 6枚（予約×2人）= 18枚
- 90枚中18枚が有効 → **80%がゼロ**

### 4.3 スパースすぎる場合の問題点

#### 4.3.1 学習効率の低下
- ほとんどの入力がゼロのため、勾配が流れにくい
- 有効なカードのパターンが限定的で汎化しにくい
- バッチ内での有効データ密度が低い

#### 4.3.2 メモリ・計算効率
- 無駄なゼロ要素の計算が発生
- GPUの並列性を活かしにくい

#### 4.3.3 表現力の問題
- カード間の相対的な位置関係が失われる
- 「このスロットにどんなカードがあるか」という情報は保持されるが、
  「場にどのカードが並んでいるか」という空間的配置が薄まる

### 4.4 スパース性への対策案

#### 案1: ハイブリッド表現
```
[場の12スロット (スロットベース)] + [90枚のカード存在フラグ (スパース)]
```
- カードの詳細はスロットベースで保持
- どのカード番号が見えているかはフラグで補助

#### 案2: 非ゼロ要素のみのパック表現
```
見えているカードのみを詰めて表現 + カードIDの明示的な埋め込み
[card_id, cost, bonus] × 見えているカード数
```
- 可変長入力をTransformerで処理

#### 案3: アテンションベース
```
固定長の90枚テンソル + 見えているカードへのマスク付きアテンション
```
- Transformer系の self-attention でスパース性を吸収
- 計算コストは高いが表現力が高い

#### 案4: グラフニューラルネットワーク (GNN)
```
カードをノード、関係性（同じスロット、購入可能など）をエッジとして表現
```
- スパース構造を直接モデル化

### 4.5 推奨アプローチ

**Phase 1 (蒸留学習)**:
- genbuと同じスロットベース表現を維持
- V3アクション空間へのマッピングのみ変更
- 蒸留でgenbu相当の強さを達成

**Phase 2 (自己対局)**:
- カードIDベースの表現を実験
- スパース性の影響を測定
- 必要に応じてハイブリッド表現に移行

---

## 5. 実装時の注意点

### 5.1 genbuからの蒸留

```python
# 蒸留損失
def distillation_loss(student_logits, teacher_logits, temperature=2.0):
    soft_targets = F.softmax(teacher_logits / temperature, dim=-1)
    soft_outputs = F.log_softmax(student_logits / temperature, dim=-1)
    return F.kl_div(soft_outputs, soft_targets, reduction='batchmean') * (temperature ** 2)
```

### 5.2 アクション空間のマッピング

genbu (406) → V3 (3133) のマッピングが必要：
- PURCHASE: スロット+暗黙的支払い → card_id + 明示的gold_as
- VISIT_NOBLE: 自動 → 明示的選択

### 5.3 盤面表現の変換

蒸留時は両方の表現を持つ必要がある：
```python
def convert_board(csplendor_board):
    genbu_tensor = slot_based_encode(csplendor_board)  # 教師用
    v3_tensor = card_id_based_encode(csplendor_board)  # 生徒用
    return genbu_tensor, v3_tensor
```

---

## 付録: パラメータ数

```
SplendorNNet 概算:
- dense2d_1:        56×128 + 128×128 ≈ 23K
- partialgpool_1:   Linear(96→112) ≈ 11K
- dense2d_3:        128×128 ≈ 16K
- dense1d_4:        704×128 ≈ 90K
- partialgpool_4:   Linear(112→112) ≈ 13K
- dense1d_5:        128×128×2 ≈ 33K
- partialgpool_5:   Linear(112→112) ≈ 13K
- output_PI:        128×128 + 128×406 ≈ 69K
- output_V:         128×128 + 128×2 ≈ 17K
- output_SDIFF:     128×128 + 128×62 ≈ 24K

合計: 約 310K パラメータ
```
