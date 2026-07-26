# Versioned Game snapshot

`Game.serialize_snapshot()`は、現在局面を復元するためのcompactなbinaryを
返す。undo用の`Game.history`と`Game.board_history`は含めず、次の状態を
保存する。

- bank、visible cards、nobles
- 各tierの完全なdeck order
- 両playerのtoken、bonus、得点、予約、購入済みcard、獲得noble
- 非公開予約flag
- current player、turn、final round、noble待ち、winner
- `simple_payment_mode`と`blank_refill_mode`

envelopeはmagic、snapshot format version、rules version、card/noble定義の
fingerprint、payload length、checksumを持つ。整数はlittle endianであり、
C++ objectのmemory layoutやPython pickleには依存しない。

```python
snapshot = game.serialize_snapshot()
restored = csplendor.Game.deserialize_snapshot(snapshot)

assert restored.serialize_snapshot() == snapshot
assert restored.board_hash() == game.board_hash()
```

`deserialize_snapshot()`はhistoryを持たない`Game`を返すため、
直後の`undo()`は失敗する。snapshot format、rules、card/noble定義のいずれか
が一致しないbinaryや、破損・切詰め・過大payloadは拒否する。

formatを変更する場合は`GAME_SNAPSHOT_FORMAT_VERSION`を、同じbinary layoutで
rule transitionの意味を変更する場合は`GAME_SNAPSHOT_RULES_VERSION`を必ず
更新する。学習アーカイブで既存snapshotを維持する期間は、旧version decoder
も明示的に保持するか、対応する旧engineを固定して使用する。

snapshotはauthoritativeな完全情報であり、山札順と相手の非公開予約も含む。
不完全情報ゲームのMCTSで直接読むと情報漏洩になるため、root observer視点の
determinizationを必ず適用する。

