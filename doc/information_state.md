# Versioned information-state identity

`Game.serialize_information_state(observer)` は、定石DBやoffline解析で局面を
永続的に識別するためのobserver-safeなbinaryを返す。これは完全局面snapshotではなく、
指定observerが知り得る現在の情報集合のidentityである。非公開情報を復元できないため、
`Game`へdeserializeするAPIは持たない。

含まれる情報は次のとおり。

- observer、Game mode、手番、turn、終局phase、貴族選択待ち・返却待ち（`waiting_return`）
- bank、公開カード、山札枚数、貴族
- observer-safeなtier別未知card pool
- 両playerのtoken、bonus、点数、購入枚数
- 公開予約、observer自身の伏せ予約、相手伏せ予約のtier
- 既知の購入card、未知購入枚数、獲得貴族

含まれない情報は次のとおり。

- 山札順
- 相手の伏せ予約card ID
- 相手の伏せ予約と物理山札のpartition
- undo/history、探索木、controller側の反復回避履歴

公開カード・貴族・予約・既知購入cardのスロットまたは格納順はrule上の意味を持たないため、
ID順にcanonicalizeする。USI/SPNで`bought:[_,...]`と表される購入cardは、
総購入枚数から既知ID数を引いた未知購入枚数として保持する。相手の伏せ予約と物理山札は、現在のMCTSと同じく
`observable_card_pool()`の集合として表す。従ってobserver-perspective
determinizationでhidden allocationだけを変更してもbytesは変わらない。

```python
game = csplendor.Game(seed=42)
identity = game.serialize_information_state(observer=0)
index = game.information_state_hash(observer=0)

# 64bit hashは索引専用。永続DBではbytesも比較する。
assert isinstance(identity, bytes)
assert isinstance(index, int)
```

envelopeはmagic、format version、rules version、card/noble数、ruleset
fingerprint、payload長、checksumを持つ。fieldの追加・削除・canonicalization変更時は
`information_state::FORMAT_VERSION`を更新する。同じlayoutのままrule上の意味を変更する
場合は`RULES_VERSION`を更新する。現在はformat version 6、rules version 5である
（2026-10-18、場札の予約も返却を持たず返却フェーズで返すルールに改めた際に更新）。千日手の判定区間
`repetition_keys` は公開情報で、引き分けを決めるため、局面数（u16）と昇順に並べた鍵（u64）として含める
（順序は未来に影響しないので含めない）。旧versionのbytesとは一致しないため、
永続DBのキーは再生成する。

山札予約で引いたカードは相手から見て非公開のままで、補充された場札と返却色は公開情報である。

色置換による対称性圧縮はversion 3でも行わない。cardとnobleの実catalogueがその置換で
閉じていることを証明せずに色をcanonicalizeしてはならない。
