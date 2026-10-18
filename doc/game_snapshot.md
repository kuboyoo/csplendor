# Versioned Game snapshot

`Game.serialize_snapshot()`は、現在局面を復元するためのcompactなbinaryを
返す。undo用の`Game.history`と`Game.board_history`は含めず、次の状態を
保存する。

- bank、visible cards、nobles
- 各tierの完全なdeck order
- 両playerのtoken、bonus、得点、予約、購入済みcard、獲得noble
- 非公開予約flag
- current player、turn、final round、noble待ち、返却待ち（`waiting_return`）、winner
- 終局理由（`end_reason`）と千日手の判定区間（`repetition_keys`）
- `simple_payment_mode`と`blank_refill_mode`

envelopeはmagic、snapshot format version、rules version、card/noble定義の
fingerprint、payload length、checksumを持つ。整数はlittle endianであり、
C++ objectのmemory layoutやPython pickleには依存しない。

現在のformat versionとrules versionはともに5である（2026-10-18、場札の予約も返却を持たず
返却フェーズで返すルールに改めた際に更新）。payloadは`waiting_noble` byteの直後に`waiting_return` byteを、
`winner` byteの直後に`end_reason` byte、千日手の判定区間の局面数（u16）と各局面の
`repetition_position_key()`（u64、古い順）を持つ。区間が長くなりうるため、snapshotの上限は64KiBである。
`waiting_noble`と`waiting_return`が両方立つsnapshotは拒否する。

購入済みcardはID昇順で書き出す。読み込みでは並び順を問わない（重複や範囲外のIDは拒否する）。
engineは購入順を持たないため、購入順で並んだ過去のsnapshotも同じformatのまま読める。

読み込めるのはversion 5だけである。version 1〜4のsnapshotは拒否し、変換の手段も提供しない。

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
更新する。旧versionは読み込まない。

snapshotはauthoritativeな完全情報であり、山札順と相手の非公開予約も含む。
不完全情報ゲームのMCTSで直接読むと情報漏洩になるため、root observer視点の
determinizationを必ず適用する。
