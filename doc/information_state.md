# Versioned information-state identity

`Game.serialize_information_state(observer)` は、定石DBやoffline解析で局面を
永続的に識別するためのobserver-safeなbinaryを返す。これは完全局面snapshotではなく、
指定observerが知り得る現在の情報集合のidentityである。非公開情報を復元できないため、
`Game`へdeserializeするAPIは持たない。

含まれる情報は次のとおり。

- observer、Game mode、手番、turn、終局phase
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
場合は`RULES_VERSION`を更新する。

色置換による対称性圧縮はversion 2では行わない。cardとnobleの実catalogueがその置換で
閉じていることを証明せずに色をcanonicalizeしてはならない。
