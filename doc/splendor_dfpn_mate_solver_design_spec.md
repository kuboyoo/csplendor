# Splendor 2人戦用「めくれ全称分岐つきDFPN詰み証明探索」設計仕様書

## 1. 目的

本仕様書は、Splendor 2人戦の任意局面に対して、指定深さ以内に「相手がどう受けても、購入・予約後に何がめくれても、攻撃側の勝利が保証される詰み」が存在するかを判定するための、DFPN（Depth-First Proof-Number Search）ベースのソルバー設計を定義する。

本ソルバーは、PlayGoの詰碁ランのような反射練習用問題集を生成・検証するためのものであり、対局中にリアルタイムで毎ターン使う評価AIではない。

前提として、通常の深さ優先AND-OR探索では、Splendor特有の以下の分岐により、depth 5〜7で探索爆発しやすい。

- 防御側の合法手が多い。
- 宝石取得と返却の組み合わせが多い。
- 場カード購入・予約後に全めくれ分岐がある。
- 貴族選択分岐がある。
- 最終ラウンド終了まで勝敗が確定しない。
- 山札順固定ではなく、未公開カード集合から任意カードがめくれるものとして扱う。

したがって、本仕様では、単純な深さ制限DFSではなく、証明数・反証数を用いて「証明または反証に最も近い枝」を優先展開するDFPNを主方式とする。

---

## 2. 対象範囲

### 2.1 対象ルール

- Splendor通常ルール。
- 2人戦限定。
- 拡張版は対象外。
- 伏せ予約は禁止。
- 場カード予約は許可。
- 山札順は固定しない。
- 場補充では、該当レベルの未公開カード集合から任意の1枚がめくれるものとする。
- めくれは確率分岐ではなく、全称分岐として扱う。
- 同点かつ購入枚数も同じ場合は、攻撃側勝利とはみなさない。

### 2.2 既存ゲームエンジンの前提

既存ゲームエンジンが以下を提供している前提とする。

- 局面から合法手を返す。
- 合法手を局面に適用できる。
- 宝石取得、返却、予約、購入、貴族、終局、勝者判定を処理できる。
- 2人戦ルールを正しく扱える。

ただし、通常エンジンが山札順固定補充を前提としている場合、ソルバー側またはAdapter層で、補充を「未公開集合からの任意めくれ」に差し替える。

---

## 3. 本ソルバーが扱う詰みの定義

### 3.1 完全詰み

本仕様での詰みとは、攻撃側が指定深さ以内に勝利を保証できることを指す。

より正確には、攻撃側が選ぶ手が存在し、その後、防御側がどの合法手を選んでも、また購入・予約後にどの未公開カードがめくれても、攻撃側の勝利が保証される場合に `Mate` とする。

量化構造は以下である。

```text
攻撃側手番:
  ∃ attack_move .
    ∀ reveal_after_attack .
      WIN(next_state, remaining_attack_depth - 1)

防御側手番:
  ∀ defense_move .
    ∀ reveal_after_defense .
      WIN(next_state, remaining_attack_depth)
```

### 3.2 深さ定義

`max_depth` は攻撃側の着手回数で数える。

```text
depth = 1:
  攻撃側が次の1手で勝利を保証する。

depth = 2:
  攻撃側 → 防御側 → 攻撃側で勝利を保証する。

depth = 3:
  攻撃側 → 防御側 → 攻撃側 → 防御側 → 攻撃側で勝利を保証する。
```

したがって、`depth = 7` は実質的に最大13 ply相当であり、Splendorでは非常に重い。

### 3.3 終局判定

以下の場合、探索ノードは終端とする。

```text
終局済みかつ winner == attacker:
  証明済み Mate

終局済みかつ winner != attacker:
  反証済み NoMate

未終局かつ remaining_attack_depth == 0:
  反証済み NoMate
```

注意点として、15点到達は即勝利ではない。最終ラウンド終了後の勝者判定を使用する。

---

## 4. なぜ単純AND-OR DFSではなくDFPNなのか

### 4.1 単純DFSの問題

単純なAND-OR DFSでは、以下のような巨大な木を愚直に展開してしまう。

```text
攻撃手候補
  × 攻撃手後の全めくれ
  × 防御側の全合法手
  × 防御手後の全めくれ
  × 次の攻撃手候補
  ...
```

仮に平均値として以下を置く。

```text
攻撃側合法手: 40
防御側合法手: 80
平均めくれ候補: 10
```

1往復だけでも概算分岐は以下になる。

```text
40 × 10 × 80 × 10 = 320,000
```

depth 7ではこれが複数層続くため、単純DFSでは現実的でない。

### 4.2 DFPNの利点

DFPNはAND-OR木に対して、各ノードに以下を持たせる。

```text
pn: proof number
  そのノードを「詰み」と証明するために必要そうな未解決葉の数。

dn: disproof number
  そのノードを「詰みではない」と反証するために必要そうな未解決葉の数。
```

DFPNは、証明・反証の可能性が高い枝を優先して深く展開する。

Splendor詰み探索では特に、攻撃側候補手の多くは、防御側の1手または特定めくれで簡単に反証される。
そのため、反証を早く見つけられるDFPNは、単純DFSより適している。

---

## 5. 論理ノード設計

### 5.1 ノード種別

ソルバー内部では、以下の論理ノードを扱う。

```text
OR node:
  攻撃側手番。
  いずれか1つの攻撃手で勝てればよい。

AND node:
  防御側手番。
  全ての防御手で勝てなければならない。

REVEAL_AND node:
  場補充のめくれ分岐。
  全てのめくれで勝てなければならない。
```

ただし実装上は、`REVEAL_AND node` を独立ノードにしてもよいし、手の `Outcomes` 内でAND子として扱ってもよい。

推奨は、内部的に全てを一般化して以下に揃えることである。

```text
OR node:
  子のどれかを証明すればよい。

AND node:
  子の全てを証明する必要がある。
```

その場合、めくれはANDノードの子集合として表現する。

### 5.2 ノードの正規化

以下のように定義する。

```text
攻撃側手番:
  OR node
  children = attack move nodes

攻撃側の各move適用後:
  AND node
  children = all reveal outcomes

防御側手番:
  AND node
  children = defense move nodes

防御側の各move適用後:
  AND node
  children = all reveal outcomes
```

防御側手番は、`defense move` も `reveal` も両方ANDであるため、実装上は以下のように平坦化してよい。

```text
children = all (defense_move, reveal_outcome) pairs
```

ただし、証明木や反証説明を出すためには、`defense_move` と `reveal_outcome` の構造を保持する。

---

## 6. proof number / disproof number の定義

### 6.1 終端ノード

```text
攻撃側勝利終端:
  pn = 0
  dn = INF

攻撃側敗北・同点・未終局depth切れ:
  pn = INF
  dn = 0
```

### 6.2 未展開葉ノード

未展開ノードには初期値を与える。

基本値は以下。

```text
pn = 1
dn = 1
```

ただし、ヒューリスティック初期化を使う場合は、勝ちに近い局面の `pn` を小さく、反証されやすい局面の `dn` を小さくする。

初期実装では `1, 1` でよい。

### 6.3 ORノード

ORノードは、どれか1つの子を証明できればよい。

```text
pn(OR) = min pn(child)
dn(OR) = sum dn(child)
```

意味:

- 証明には一番証明しやすい子だけでよい。
- 反証には全ての子を反証する必要がある。

### 6.4 ANDノード

ANDノードは、全ての子を証明する必要がある。

```text
pn(AND) = sum pn(child)
dn(AND) = min dn(child)
```

意味:

- 証明には全ての子の証明が必要。
- 反証には1つの子を反証すればよい。

### 6.5 INF

`INF` は十分大きな飽和値とする。

```text
INF = 2^60
```

または実装言語に応じた安全な上限値を使う。

加算時は飽和加算を使う。

```text
sat_add(a, b) = min(INF, a + b)
```

---

## 7. 深さ制限つきDFPN

### 7.1 深さ制限の扱い

本ソルバーでは、通常の無制限DFPNではなく、攻撃側の残り着手数 `remaining_attack_depth` を状態に含める。

```text
SearchState
  information_state
  attacker
  remaining_attack_depth
```

攻撃側が手を指した場合のみ `remaining_attack_depth` を1減らす。

防御側手番では減らさない。

### 7.2 depth切れ

未終局かつ `remaining_attack_depth == 0` の場合、攻撃側はこれ以上勝利を証明できない。

```text
pn = INF
dn = 0
```

### 7.3 深さつきTT

同じ盤面でも、残り深さが違えば結論が変わる。

したがって、Transposition Tableのキーには必ず `remaining_attack_depth` を含める。

```text
TTKey =
  canonical_information_state_hash
  attacker
  remaining_attack_depth
```

---

## 8. DFPNの探索手順

### 8.1 基本インターフェース

```text
solve_dfpn(input):
  root = make_node(input.state, input.attacker, input.max_depth)
  dfpn(root, phi = INF, delta = INF)
  if root.pn == 0:
      return Mate with proof_tree
  if root.dn == 0:
      return NoMate with refutation
  return Unknown
```

### 8.2 DFPN関数の概念

DFPNは、ノード `n` を、証明しきい値 `phi` と反証しきい値 `delta` のもとで探索する。

概念的には以下。

```text
dfpn(n, phi, delta):
  n を評価・展開する
  while n.pn < phi and n.dn < delta:
      child = select_most_proving_child(n)
      (child_phi, child_delta) = compute_child_threshold(n, child, phi, delta)
      dfpn(child, child_phi, child_delta)
      n の pn/dn を更新する
      TTに保存する
```

### 8.3 most proving child

ノード種別ごとに、最も重要な子を選ぶ。

#### ORノード

ORノードでは、証明しやすい子を優先する。

```text
select child with minimum pn
tie-break by smaller dn, move ordering score
```

#### ANDノード

ANDノードでは、反証しやすい子を優先する。

```text
select child with minimum dn
tie-break by smaller pn, danger score
```

めくれANDでは、攻撃側にとって危険なめくれほど優先する。

### 8.4 child threshold

DFPNの実装には複数のバリエーションがある。
本仕様では実装しやすい標準的な考え方を採用する。

#### ORノードの子しきい値

ORノードでは、選択子 `c` について、他の子の反証数合計を考慮する。

```text
child_phi = phi
child_delta = min(
    delta - sum_dn_except(c),
    second_best_pn
)
```

実装では負値や0を避けるため、最小値1を保証する。

#### ANDノードの子しきい値

ANDノードでは、選択子 `c` について、他の子の証明数合計を考慮する。

```text
child_phi = min(
    phi - sum_pn_except(c),
    second_best_dn
)
child_delta = delta
```

実装では負値や0を避けるため、最小値1を保証する。

### 8.5 実装上の注意

DFPNのしきい値計算はバグが入りやすい。初期実装では、厳密なしきい値最適化よりも、以下を優先してよい。

```text
- pn/dn更新が正しいこと。
- most proving childが正しいこと。
- TTによる再訪削減が効いていること。
- node/time/memory limitでUnknownを返せること。
```

そのうえで、しきい値計算を段階的に改善する。

---

## 9. 子ノード生成戦略

### 9.1 eager展開を避ける

Splendorでは1ノードあたりの子が非常に多い。
そのため、全合法手・全めくれを毎回完全生成する eager 展開は避ける。

推奨は lazy child generation である。

```text
Node
  generated_children: list
  generator_state
  generation_complete: bool
```

DFPNが必要とした順に子を生成する。

### 9.2 ただし証明完了時は全子確認が必要

ANDノードを証明するには、全ての子が証明されている必要がある。

したがって、lazy生成を使う場合でも、ANDノードで `pn = 0` とするためには、未生成子が存在しないことを確認しなければならない。

```text
AND node is proven only if:
  generation_complete == true
  and every child.pn == 0
```

ORノードを反証する場合も同様に、全子生成済みかつ全子が反証済みである必要がある。

```text
OR node is disproven only if:
  generation_complete == true
  and every child.dn == 0
```

### 9.3 未生成子のpn/dn見積もり

lazy生成中のノードでは、未生成子が残っている限り、仮想子を1つ持つものとして扱う。

```text
unexpanded_virtual_child:
  pn = 1
  dn = 1
```

これにより、未生成子があるORノードを誤って反証済みにしたり、未生成子があるANDノードを誤って証明済みにしたりするのを防ぐ。

### 9.4 子生成の順序

子は、証明・反証が早まりそうな順に生成する。

#### 攻撃側ORノード

攻撃手は以下の順に生成する。

1. 最終勝利に直結する購入。
2. 15点以上に到達する購入。
3. 貴族獲得を伴う購入。
4. 相手の即勝ち候補を予約する手。
5. 次手に複数勝ち筋を作る宝石取得。
6. 金獲得を伴う予約。
7. 高得点カード購入。
8. その他。

#### 防御側ANDノード

防御手は以下の順に生成する。

1. 防御側が15点以上に到達する購入。
2. 防御側が最終勝利で上回る購入。
3. 攻撃側の勝ちカードを予約する手。
4. 攻撃側の必要宝石を奪う手。
5. 貴族獲得を伴う購入。
6. 高得点カード購入。
7. 金獲得を伴う予約。
8. その他。

#### めくれANDノード

めくれ候補は以下の順に生成する。

1. 防御側が即購入でき、15点以上に到達するカード。
2. 防御側が即購入でき、最終勝利に近づくカード。
3. 防御側が購入すると貴族条件を満たすカード。
4. 攻撃側の勝ち筋を妨害する予約対象になりうるカード。
5. 防御側にだけ有利で攻撃側に不要なカード。
6. その他。

---

## 10. Outcomes設計

### 10.1 Outcomesの役割

`Outcomes(state, move)` は、手の適用後に生じる全ての次状態を返す。

含める処理:

- コスト支払い
- 宝石獲得
- 返却
- 予約
- 購入
- 場補充
- 貴族獲得
- 最終ラウンド状態更新
- 次プレイヤー移行

探索木に中間状態を残さない。

### 10.2 場補充がない手

以下の手では、めくれ分岐は発生しない。

- 宝石取得
- 予約済みカード購入
- 貴族獲得のみ
- 返却のみ

この場合、`Outcomes` は通常1状態である。
ただし、複数貴族候補がある場合は、貴族選択分岐により複数状態になりうる。

### 10.3 場補充がある手

以下の手では、補充分岐が発生する。

- 場カード購入
- 場カード予約

レベル `L` のカードが場から消えた場合、候補は以下。

```text
unseen_cards_by_level[L]
```

各候補カードを1枚場に出した次状態を生成する。

未公開カード集合が空なら、補充なし状態を1つ生成する。

### 10.4 複数貴族候補

手番終了時に複数貴族候補がある場合、手番プレイヤーが1枚選ぶ。

推奨実装:

```text
Move または Outcome に noble_choice を含める。
```

これにより、貴族選択は手番プレイヤーの意思決定として正しく扱える。

ソルバー内部で扱う場合は、以下の量化になる。

```text
攻撃側手番中の貴族選択:
  OR

防御側手番中の貴族選択:
  AND側の防御選択の一部
```

実装ミスを避けるため、既存エンジンの合法手生成またはAdapter層で、貴族選択済みの完全手として展開することを推奨する。

---

## 11. Transposition Table設計

### 11.1 TTの目的

DFPNは同じ局面を何度も訪れる。
Splendorでは、宝石取得順や返却順が違っても同一局面に合流することがある。

TTは以下を目的とする。

- 既に証明済み・反証済みの局面を再利用する。
- pn/dnの途中値を再利用する。
- 循環または反復探索の過剰展開を防ぐ。
- 証明木復元に必要な最善子を保存する。

### 11.2 TTキー

```text
TTKey
  canonical_information_state_hash
  attacker
  remaining_attack_depth
```

### 11.3 canonical_information_state_hashに含めるもの

- current_player
- bank_tokens
- 各プレイヤーのtokens
- 各プレイヤーのbonuses
- 各プレイヤーのscore
- 各プレイヤーのreserved_cards集合
- 各プレイヤーのpurchased_card_idsまたは購入枚数・ボーナス・点数
- visible_cards
- unseen_cards_by_level
- remaining_nobles
- final_round_state
- その他、勝敗や合法手に影響する状態

### 11.4 正規化

以下はソートしてハッシュ化する。

- reserved_cards
- purchased_card_ids
- unseen_cards_by_level
- remaining_nobles

場カードのスロット位置に意味がない場合は、場カードも集合として扱ってよい。
ただし、UIやエンジンがスロットを状態として扱う場合は、スロット位置を保持する。

### 11.5 TTエントリ

```text
TTEntry
  pn: int
  dn: int
  status: Unknown | Proven | Disproven
  node_type: OR | AND
  generation_complete: bool
  best_child_key: TTKey | null
  best_move: Move | null
  best_reveal: CardId | None | null
  lower_or_upper_bound_info: optional
  visit_count: int
  last_updated_generation: int
```

### 11.6 深さ単調性の利用

以下の単調性がある。

```text
depth = d で Mate なら、depth > d でも Mate。
depth = d で NoMate なら、depth < d でも NoMate。
```

そのため、TTに以下を追加で保存できる。

```text
proven_mate_min_depth
proven_no_mate_max_depth
```

ただし初期実装では、単純に `(state_hash, depth)` 単位で扱ってよい。
単調性利用は第2段階の最適化とする。

---

## 12. 循環・反復局面への対応

Splendorではカード・貴族は有限だが、宝石取得や返却により似た状態を行き来する可能性がある。

### 12.1 経路上再訪

現在の探索経路上に同じ `TTKey` が出た場合、即座に確定扱いしない。

推奨処理:

```text
現在経路上の同一キー再訪:
  pn = INF
  dn = INF
  または一時的なUnknown leafとして扱う
```

これにより、無限再帰を防ぐ。

ただし、深さ制限があるため、実際には攻撃側手番が進むたびにdepthが減り、完全な無限再帰にはなりにくい。

### 12.2 TTと経路スタック

実装では以下を持つ。

```text
active_stack_keys: Set<TTKey>
```

`dfpn` 入場時に追加し、退出時に削除する。

---

## 13. 計算量設計

### 13.1 理論上の最悪計算量

完全な全称めくれ付き探索の最悪計算量は指数的である。

概算:

```text
A = 攻撃側平均合法手数
D = 防御側平均合法手数
R = 平均めくれ候補数
k = 攻撃側残り手数
```

単純探索では概ね以下に近い。

```text
O((A * R * D * R)^(k-1) * A * R)
```

これは実用上巨大である。

### 13.2 DFPNで期待する削減

DFPNは最悪計算量を多項式にするものではない。
ただし、以下の性質により実用上の探索量を大きく削減できる。

- ORノードでは、勝てる候補を1つ見つければよい。
- ANDノードでは、反証子を1つ見つければその候補を棄却できる。
- Splendorでは、多くの候補攻撃手は危険めくれや防御手で早期に反証される。
- TTにより、同一情報局面の再探索を回避できる。
- lazy展開により、必要になるまで全めくれ・全合法手を生成しない。

### 13.3 実用上のボトルネック

優先的に監視すべき指標は以下。

```text
expanded_nodes
generated_children
evaluated_reveals
legal_move_calls
outcome_calls
tt_hit_rate
tt_cut_count
max_frontier_size
max_recursion_depth
avg_children_per_or
avg_children_per_and
avg_reveals_per_refill
```

特に、`legal_move_calls` と `outcome_calls` は高コストになりやすい。

### 13.4 目標値の目安

初期実装では以下を目標にする。

```text
depth 1:
  ほぼ即時

depth 3:
  数秒〜数十秒以内

depth 5:
  局面により数十秒〜数分

depth 7:
  良く絞られた終盤局面のみ対象
  node/time/memory limit前提
```

depth 7を常に完全解決できるとは期待しない。
問題生成では、depth 7は候補局面を十分に絞ったうえで検証する。

---

## 14. 枝刈りと正確性

### 14.1 正確性を保つ最適化

以下は正確性を落とさない。

- TTによる同一局面再利用。
- canonical hashによる重複排除。
- 探索順序付け。
- lazy child generation。
- 証明済み・反証済みノードの打ち切り。
- 飽和pn/dnによる打ち切り。
- 深さ制限。

### 14.2 正確性を落とす可能性がある最適化

以下は最終判定に使ってはならない。

- 危険でなさそうなめくれを完全に無視する。
- 防御側の弱そうな手を除外する。
- カード特徴量抽象化だけでMate判定する。
- 確率の低いめくれを除外する。
- 評価値が低い手を合法手から削る。

これらは候補生成や探索順序付けには使ってよいが、`Mate` と返す前の証明には使用しない。

### 14.3 Two-phase運用

推奨運用は以下。

```text
Phase 1: Candidate Search
  ヒューリスティック、抽象化、候補初手制限を使って詰み候補局面・候補初手を探す。

Phase 2: Exact DFPN Verification
  全合法手、全防御、全めくれを対象にDFPNで厳密検証する。
```

問題集に採用できるのは Phase 2 を通ったもののみ。

---

## 15. 証明木と反証木

### 15.1 Mate時の証明木

`Mate` の場合、証明木は以下を持つ。

```text
ProofTree
  root_state_summary
  attacker_move
  reveal_branches
  defense_branches
  terminal_win_nodes
```

攻撃側ノードでは、選ばれた1手だけを保存すればよい。
防御側ノードとめくれノードでは、全分岐に対する継続を保存する必要がある。

### 15.2 NoMate時の反証木

`NoMate` の場合、少なくとも以下を返せるとデバッグに有用である。

```text
RefutationTree
  refuted_attacker_move
  refuting_defense_move
  refuting_reveal
  reason
```

ただし、ルートがNoMateの場合、攻撃側の全合法手が反証されている必要がある。
完全な反証木は巨大になりやすいため、実装では以下の2段階を推奨する。

```text
compact_refutation:
  各攻撃手につき代表反証1つ。

full_refutation:
  デバッグ時のみ全反証を保持。
```

### 15.3 TTからの証明木復元

DFPN探索中に全ての親子ポインタを保持するとメモリを圧迫する。

推奨方式:

```text
TTEntryにbest_child情報を保存する。
探索完了後、TTをたどって証明木を再構築する。
```

Mate時:

- ORノードでは、証明済みのbest childを1つたどる。
- ANDノードでは、全ての証明済み子をたどる。

NoMate時:

- ORノードでは、全ての反証済み子をたどる。
- ANDノードでは、反証済みのbest childを1つたどれば代表反証になる。

---

## 16. 既存ゲームエンジンとのAdapter設計

### 16.1 必要なAdapter関数

既存ゲームエンジンに以下のような関数を提供するAdapterを用意する。

```text
get_legal_moves(info_state) -> Ordered/Lazy MoveIterator

apply_move_without_refill(info_state, move) -> PartialState

get_refill_level(partial_state, move) -> Level | None

apply_reveal(partial_state, level, card_id | None) -> StateAfterReveal

apply_noble_choice_and_finalize(state_after_reveal, choice) -> InformationState[]
```

実装都合により、これらをまとめて以下でもよい。

```text
generate_outcomes(info_state, move) -> Lazy OutcomeIterator
```

### 16.2 山札順固定エンジンへの対応

既存エンジンが山札順固定を前提にしている場合は、以下のいずれかを行う。

#### 方式A: draw処理を無効化できるAPIを追加

推奨。

```text
apply_move_until_before_refill
apply_specific_refill_card
finalize_turn
```

#### 方式B: 仮想山札を差し替えて各めくれを再実行

実装は簡単だが重い。

めくれ候補カード `c` ごとに、山札先頭を `c` にした仮想状態を作り、既存エンジンでmoveを適用する。

注意点:

- 未公開集合から `c` を除く。
- 他の未公開カード集合を壊さない。
- 場・予約・購入済みカードと重複しない。

#### 方式C: ソルバー側で補充だけ手動処理

エンジンが補充以外を正しく処理できる場合に有効。

---

## 17. データ構造

### 17.1 InformationState

```text
InformationState
  current_player: PlayerId
  players: PlayerState[2]
  bank_tokens: TokenVector
  visible_cards: VisibleCards
  unseen_cards_by_level: Set<CardId>[3]
  remaining_nobles: Set<NobleId>
  final_round_state: FinalRoundState
  rule_flags: RuleFlags
```

### 17.2 PlayerState

```text
PlayerState
  tokens: TokenVector
  bonuses: ColorVector
  score: int
  reserved_cards: Set<CardId>
  purchased_cards: Set<CardId>
  purchased_count: int
```

`purchased_cards` を保持するか、`purchased_count` と `bonuses` と `score` だけにするかは実装依存。
ただし、状態復元・デバッグ・正規化のためには `purchased_cards` を持つ方が安全である。

### 17.3 DFPN Node

```text
DFPNNode
  key: TTKey
  node_type: OR | AND | Terminal
  pn: int
  dn: int
  children: list[ChildRef]
  generator_state: ChildGeneratorState
  generation_complete: bool
  best_child: ChildRef | null
```

### 17.4 ChildRef

```text
ChildRef
  child_key: TTKey
  move: Move | null
  reveal: CardId | None | null
  transition_kind: Move | Reveal | NobleChoice | Flattened
  ordering_score: float | int
```

---

## 18. 並列化方針

DFPNは逐次依存が強いため、素朴なマルチスレッド化は難しい。

### 18.1 安全な並列化候補

- ルート直下の攻撃手ごとに並列検証する。
- めくれ候補の危険度評価を並列化する。
- 合法手の静的スコアリングを並列化する。
- 問題候補局面ごとに並列化する。

### 18.2 推奨

問題生成用途では、1局面内探索を並列化するより、複数候補局面をプロセス並列で検証する方が実装が簡単で効率がよい。

```text
worker 1: puzzle candidate A
worker 2: puzzle candidate B
worker 3: puzzle candidate C
...
```

### 18.3 TT共有

複数プロセス間でTTを共有する必要は初期実装ではない。
局面ごとに独立TTでよい。

---

## 19. 探索制限とUnknown

### 19.1 制限項目

以下を設定可能にする。

```text
time_limit_ms
node_limit
tt_entry_limit
memory_limit_mb
max_generated_children_per_node optional
```

ただし `max_generated_children_per_node` は正確性を損なう可能性があるため、設定した場合は `Unknown` を返す。

### 19.2 Unknownを返す条件

- 時間制限到達。
- ノード制限到達。
- メモリ制限到達。
- TTが飽和し、正確な探索継続ができない。
- Adapterが不正状態を返した。
- 未対応ルール分岐に到達した。

問題集に採用するのは `Mate` のみ。
`NoMate` は「指定深さ以内に完全詰みなし」として利用可能。
`Unknown` は不採用。

---

## 20. ロギングとプロファイリング

### 20.1 必須ログ

```text
max_depth
root_status
root_pn
root_dn
expanded_nodes
generated_children
tt_hits
tt_misses
tt_proven_hits
tt_disproven_hits
legal_move_calls
outcome_calls
reveal_generated_count
elapsed_ms
peak_memory_mb
```

### 20.2 分岐統計

```text
avg_or_children
avg_and_children
avg_reveal_children
max_or_children
max_and_children
max_reveal_children
```

### 20.3 反証統計

```text
refutation_by_defense_move_type
refutation_by_reveal_level
refutation_by_reveal_card
refutation_at_depth
```

これにより、探索爆発の原因が「防御手」なのか「めくれ」なのか「貴族分岐」なのかを判定できる。

---

## 21. テスト仕様

### 21.1 ルールテスト

- 予約上限3枚。
- 伏せ予約が生成されない。
- 場カード予約時のみ補充が発生する。
- 場カード購入時のみ補充が発生する。
- 予約済みカード購入では補充しない。
- 宝石取得後の10枚制限。
- 複数貴族候補から1枚選択。
- 15点到達後の最終ラウンド。
- 同点時の購入枚数タイブレーク。
- 同点かつ購入枚数同じ場合は攻撃側勝利扱いしない。

### 21.2 めくれテスト

- レベル1カードを除去したら、レベル1未公開集合のみから補充される。
- レベル2カードを除去したら、レベル2未公開集合のみから補充される。
- レベル3カードを除去したら、レベル3未公開集合のみから補充される。
- 補充カードは未公開集合から削除される。
- 未公開集合が空なら補充なし状態になる。
- 場・予約・購入済みカードがめくれ候補に含まれない。

### 21.3 DFPN数式テスト

小さな人工AND-OR木で以下を検証する。

```text
OR:
  pn = min child.pn
  dn = sum child.dn

AND:
  pn = sum child.pn
  dn = min child.dn
```

終端:

```text
Win:
  pn = 0, dn = INF

Loss:
  pn = INF, dn = 0
```

### 21.4 量化テスト

- 攻撃側ORで1つ勝ち手があればMate。
- 攻撃側ORで全手が反証されればNoMate。
- 防御側ANDで1つでも逃げがあればNoMate。
- めくれANDで1つでも負けめくれがあればNoMate。
- 全防御・全めくれで勝てる場合のみMate。

### 21.5 TTテスト

- 同一情報局面が同じhashになる。
- 予約カード順序が違っても同じhashになる。
- 未公開集合順序が違っても同じhashになる。
- 残り深さが違えば別キーになる。
- proven Mate / NoMateが再利用される。

---

## 22. 推奨実装フェーズ

### Phase 1: 正確性優先の最小DFPN

- `pn/dn` を実装。
- OR/ANDノードを実装。
- 深さ制限を実装。
- TTを実装。
- 子はeager生成でもよい。
- depth 1〜3で正確性確認。

### Phase 2: Lazy生成と順序付け

- child generatorをlazy化。
- 攻撃手、防御手、めくれの順序付けを導入。
- outcome重複排除を導入。
- depth 3〜5を現実的にする。

### Phase 3: 実用最適化

- TT単調性利用。
- 証明木復元。
- 反証木出力。
- プロファイリング。
- 問題候補局面の並列検証。
- depth 5〜7の一部局面を対象にする。

### Phase 4: 問題生成統合

- 初手一意性チェック。
- クリティカルめくれ抽出。
- 解説生成用の証明木整形。
- 難易度スコアリング。
- 悪問除外。

---

## 23. 問題品質評価指標

問題集作成では、探索結果に以下を付与する。

```text
mate_depth
winning_first_move_count
legal_first_move_count
proof_tree_size
max_defense_branch_count
max_reveal_branch_count
critical_reveal_count
critical_defense_count
terminal_win_type
elapsed_ms
```

良問条件の例:

```text
- Mateである。
- winning_first_move_count == 1。
- proof_tree_size が大きすぎない。
- critical_reveal_count が多すぎない。
- 終端勝利が人間に理解しやすい。
- めくれによる応手変化が説明可能。
```

---

## 24. 実装時の落とし穴

### 24.1 未生成子が残っているのに証明済みにしてしまう

lazy生成時の典型的なバグ。

ANDノードは全子生成済みでなければ `pn = 0` にしてはいけない。
ORノードは全子生成済みでなければ `dn = 0` にしてはいけない。

### 24.2 めくれをOR扱いしてしまう

めくれはランダムではあるが、完全詰み証明では全称分岐である。

```text
誤: どれか良いカードがめくれれば勝ち
正: どのカードがめくれても勝ち
```

### 24.3 防御側の手を削ってしまう

防御側の一見弱い手が、宝石枯らしや予約妨害で詰みを壊す場合がある。
最終検証では防御側合法手を削ってはいけない。

### 24.4 貴族選択の量化を間違える

貴族選択は手番プレイヤーの選択である。
攻撃側の貴族選択はOR、防御側の貴族選択は防御手の一部として扱う。

### 24.5 depthの減らし方を間違える

残り深さは攻撃側が手を指したときだけ減る。
防御側手番やめくれ分岐では減らさない。

### 24.6 15点到達を即勝利にしてしまう

通常ルールでは最終ラウンド後に勝者が決まる。
15点到達だけでMate終端にしてはいけない。

---

## 25. 最終的な推奨構成

```text
splendor_solver/
  information_state.*
  adapter.*
  dfpn.*
  proof_tree.*
  transposition_table.*
  move_ordering.*
  reveal_ordering.*
  outcome_generator.*
  puzzle_quality.*
  tests/
```

### 25.1 information_state

山札順を持たない情報局面を定義する。

### 25.2 adapter

既存ゲームエンジンとソルバーを接続する。

### 25.3 dfpn

DFPN本体。

### 25.4 proof_tree

Mate証明木、NoMate反証木の復元。

### 25.5 transposition_table

深さつきTT、pn/dn保存、best child保存。

### 25.6 move_ordering / reveal_ordering

探索順序付け。

### 25.7 outcome_generator

手適用、全めくれ、貴族選択、終端状態生成。

### 25.8 puzzle_quality

初手一意性、難易度、クリティカルめくれなどを評価する。

---

## 26. 結論

本仕様の中核は以下である。

```text
論理:
  攻撃側 = OR
  防御側 = AND
  めくれ = AND

探索:
  深さ制限つきDFPN

状態:
  山札順ではなく未公開カード集合を持つ情報局面

正確性:
  Mate判定時は全防御・全めくれを証明済みにする

高速化:
  pn/dn
  TT
  lazy child generation
  危険順序付け
  反証優先
```

DFPNは最悪計算量を消すものではないが、Splendorのように「多くの候補手が少数の防御・めくれで反証される」問題では、単純AND-OR DFSより大幅に実用的である。

特にdepth 5〜7を狙う場合、最初からDFPN、TT、lazy生成、めくれ危険度順序付けを組み合わせることが必須である。
