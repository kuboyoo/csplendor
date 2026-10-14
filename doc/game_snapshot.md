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

現在のformat versionとrules versionはともに4である。version 4はpayloadの
`winner` byteの直後に`end_reason` byte、千日手の判定区間の局面数（u16）と各局面の
`repetition_position_key()`（u64、古い順）を持ち、「同一局面4回目で引き分け」の
ルールに対応する。区間が長くなりうるため、snapshotの上限を64KiBに広げた。
version 3は`winner` byteの直後に`passive_streak` byte（0〜6）と`end_reason` byteを持ち、
旧千日手ルール（実質パスの連続）に対応した。version 2はpayloadの`waiting_noble` byteの直後に`waiting_return` byteを
追加し、山札予約が返却を持たず別の`RETURN_GEM`で返すルールに対応した。
`waiting_noble`と`waiting_return`が両方立つsnapshotは拒否する。

version 2と3のsnapshotはそのまま読み込める（学習データを作り直せる状態に保つための例外）。
千日手の判定区間は空（その局面から始まる）として復元し、version 3の`passive_streak`は捨てる。
`end_reason`はversion 3ならそのまま、version 2なら終局していれば`NORMAL`、していなければ
`NONE`とする（version 2には両者パスと通常の引き分けの区別がない）。書き出しは常にversion 4である。

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
更新する。旧versionの読み込みは上記のversion 2だけを例外として持つ。

version 1のsnapshotは`Game.deserialize_snapshot()`で
`csplendor game snapshot version 1 must be converted with Game.upgrade_snapshot_v1 first`
として拒否される。保存済みのversion 1は一度だけ変換する。

```python
upgraded = csplendor.Game.upgrade_snapshot_v1(old_snapshot)  # bytes -> bytes
game = csplendor.Game.deserialize_snapshot(upgraded)
```

version 1の局面は返却待ちを持たないため、変換後の`waiting_return`は常に`False`である。

snapshotはauthoritativeな完全情報であり、山札順と相手の非公開予約も含む。
不完全情報ゲームのMCTSで直接読むと情報漏洩になるため、root observer視点の
determinizationを必ず適用する。
