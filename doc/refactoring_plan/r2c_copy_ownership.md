# 第2次リファクタリング R2-C state copy ownership

## 目的と範囲

R2の最後の独立変更として、局面フィールドの役割と、full copy、search copy、永続
snapshot、undo snapshot、delta undoの所有範囲を固定した。公開Python/C++ API、型layout、
snapshot byte列、合法手、hashは変更していない。

## フィールド分類

`state_field_roles.h`を機械検査可能な正本とし、次の分類を用いる。

| owner | canonical | derived | provenance | cache |
|---|---|---|---|---|
| `Board` | bank、visible、decks、nobles、players、current_player、turn、final_round、waiting_noble、winner | - | - | cached_hash、hash_valid |
| `PlayerState` | gems、bonuses、points、reserved、reserved_is_hidden、reserved_count、purchased_count | packed_gems、packed_bonuses、noble_eligibility_mask | purchased_cards、acquired_nobles | - |
| `Game` | board、simple_payment_mode、blank_refill_mode | - | history、board_history | - |

canonicalはルール結果を決める正本、derivedはcanonicalから再計算する高速化表現、
provenanceは表示・検証・undoに必要な履歴情報、cacheは結果を変えず破棄可能な遅延計算値で
ある。`purchased_count`は勝敗tie-breakとhashで直接使うため、provenance vectorとは分けて
canonicalとする。

## copyとsnapshotの責務

| operation | Board current state | mode | action/undo journal | hash cache |
|---|---|---|---|---|
| `clone()` | copy | copy | copy | copy |
| `clone_light()` | copy | copy | exclude | copy |
| `shuffled_clone*()` | copy後にhidden情報だけ再決定 | copy | exclude | invalidate |
| versioned snapshot | serialize | serialize | exclude | excludeし復元時invalid |
| production undo | action前の完全Board | Game側に保持 | ActionとBoardを並行保持 | exact restore |
| `UndoRecord` | 遷移で変化し得るfieldとprovenance長 | exclude | exclude | exact restore |

`clone_light()`と2種類のdeterminization cloneは、同じprivate
`copy_current_state()`を使う。これによりhistory除外とmode保持の責務を1経路にした。

versioned snapshotはprocess外へ持ち出す現在局面の形式であり、undo journalではない。
deserialize後の`history` / `board_history`は空で、hash cacheは再計算される。

production `undo()`は引き続き完全`Board` snapshotを使用する。`UndoRecord`は128-byteの
検証候補で、通常遷移では完全snapshotと一致するが、apply後に公開editorで任意変更できる
既存契約を単独では復元できない。またMCTS/solverの主要経路は`record_history=false`で、
delta化のend-to-end利益を受けない。このため本番採用は見送る。

## public fieldの判断

`Board`、`PlayerState`、`Game`のpublic data memberは既存の公開C++契約であり、private化は
source互換を壊す。R2-Bのvalidated/unchecked gatewayとR2-Aのstale-cache診断が内部経路を
保護するため、現行majorではfieldを維持する。

単なるreference getterを先に追加してもpublic field利用者の移行を強制できず、API面だけを
二重化するため追加しない。private化は次のすべてが揃うmajor-version migrationでのみ行う。

1. downstream C++利用箇所のinventoryと移行期間
2. canonical read accessorとeditor/unchecked write API
3. source互換を外すversion方針
4. layout、copy、MCTS/solver性能の再計測

## 検証

- native `state_copy_unit`: full/light/determinization ownership、copy隔離、snapshot journal除外、
  editor後undo、delta parity、field inventory
- 既存`phase7_undo_probe`: 全ActionTypeとmulti-stepのdelta/full snapshot同値、allocation比較
- Python全test、`py_compile`
- GCC/Clang native test、ASan/UBSan、TSan
- phase0 copy/allocation probeとruntime benchmark

2026-08-05、Ubuntu x86_64、GCC 13.3、portable ReleaseでR2-B mainと比較した。
phase0 probeの型sizeとallocationは完全一致し、history 50/200のfull cloneは39/124回、
light cloneは2/3回、`Board` / `Game`は392/448 bytesのままだった。

legacy MCTSを2組×5 sample、2048 simulation、batch 16で交互実行したgroup中央値は
469,538から485,808 simulations/s（1.035倍）で、回帰はなかった。関連hot-pathの
5 sample中央値もtrusted mask 29.99→30.05 ns、trusted decode 13.85→13.99 ns、bitset
走査4.27→4.27 ns、compact edge reserve/abort 139.03→140.40 nsで、停止基準内だった。

ローカルTSanはこのhost固有のASLR mapping衝突を起動時に断続的に起こすため、non-PIE
buildとCTest retryで全9件を完走した。race報告はなく、PR上の通常TSan jobもmerge gate
とする。

R2の完了条件を満たしたため、次の独立変更はR3-Aのrule query/validation/transition primitive
整理とする。
