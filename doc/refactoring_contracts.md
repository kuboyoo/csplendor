# リファクタリング互換性契約

この文書は、第2次リファクタリング中に維持する API、状態、不変条件、所有権の
基準である。機械可読な分類は
[`refactoring_contracts.json`](refactoring_contracts.json) を正とし、CI で実装との
一致を検査する。分類は安定性を表すものであり、`experimental` も現在の
`csplendor.__all__` に含まれる公開名である。

## API の安定性

- `public`: 既存名、import path、引数、default、返却型、例外型を維持する。
- `experimental`: 公開は維持するが、文書化した移行期間を設ければ将来変更できる。
  現時点では並列 MCTS と reveal-verified solver の C++ header が該当する。
- `internal`: top-level Python API へ昇格させない。C++ header は同一リポジトリ内で
  利用できるが、外部互換性を保証しない。

`GemType`、`ActionType`、`Action::pack()`、48/V2/V3 action ID、196 feature、
snapshot version は分類によらず数値契約である。変更する場合は新しい version と
移行手順を先に追加し、既存 version の意味を変更しない。

## 状態プロファイルと不変条件

| 条件 | reachable | editor | search | serialized |
|---|---|---|---|---|
| 配列長、ID範囲、固定容量 | 必須 | setter受理範囲で必須 | 必須 | deserialize時に必須 |
| bank/player間の宝石総数 | 必須 | 緩和可 | 入力状態を維持 | reachable由来なら必須 |
| visible/deck/reserve間のcard一意性 | 必須 | 緩和可 | determinization規則内で必須 | reachable由来なら必須 |
| `gems`/`bonuses` と packed値の同期 | 必須 | 公開setter後は必須 | 必須 | deserialize後は必須 |
| `reserved_count` と予約slot | 必須 | `reserved` setter後は同期 | 必須 | deserialize後は必須 |
| hash cacheがcanonical stateを反映 | 必須 | 公開setter後は必須 | 必須 | 復元直後は未計算でよい |
| undo history | 任意 | 任意 | `clone_light`では持たない | snapshot対象外 |
| hidden informationの実在性 | 必須 | 緩和可 | observerから整合する世界 | snapshot payloadどおり |

用語は次の意味で固定する。

- `reachable`: 正規の初期局面から公開された合法手だけで到達した局面。
- `editor`: Python setterや解析コードで組み立てた局面。局所的な形状・ID検査は通るが、
  ゲーム全体の資源保存やcard一意性までは保証しない。
- `search`: rollout、determinization、delta undo が一時所有する局面。外部に公開する前に
  search処理が自身の不変条件を回復する。
- `serialized`: versioned snapshotで表せる軽量状態。undo historyを含まない。

## フィールドの役割

| 区分 | 主なフィールド | 更新責任 |
|---|---|---|
| canonical | `Board.bank/visible/decks/nobles/players/turn/current_player/final_round/waiting_noble/winner`、playerのpoints・slot・count | `Game` transition、editor setter、snapshot restore |
| derived | `PlayerState.packed_gems`、`packed_bonuses` | gems/bonuses更新と同時に同期 |
| provenance | `reserved_is_hidden`、Gameのaction/board history | reserve処理または履歴記録処理 |
| cache | `Board` のposition hash cache | canonical/provenance state変更時にinvalidate |

`purchased_count` は公開card配列から単純再計算する補助値ではなく、終局時の
tie-breakに使うcanonical fieldとして扱う。

## 合法手・更新の契約

- 合法手の集合だけでなく生成順序も契約である。index適用、random値の剰余選択、
  encoderのfirst matchが順序を観測する。
- `MoveList` は最大2048手を保持する。editor状態で候補が上限を超える場合、例外では
  なく従来どおり生成順の先頭2048手を保持する。
- `Game.apply` は検査済みの公開入口、`apply_trusted` は既知の合法手向け入口である。
  trusted APIへ不正な手を渡した結果は互換性保証の対象外とする。
- `rule_transition.h` のsolver向け低レベル更新には、失敗までの部分更新を残して
  呼び出し側がrollbackする契約がある。リファクタリング中にtransactionalへ変えない。
- Python editor setterはpayload全体を検査してから書き込み、成功時にhashを
  invalidateする。失敗時に局面とcache状態を変更しない。

## copy、参照、メモリ所有権

- `Game.board` はGameが所有するboardへの参照であり、board setterは元のGameを更新する。
- `Board.players` と `Board.get_player()` はplayerのcopyを返す。変更は
  `Board.set_player()` で明示的に書き戻す。
- `Game.clone()` はundo historyを含む独立copy、`clone_light()` はhistoryを除いた
  独立copyである。
- encoder maskとMCTS callbackへ渡すNumPy配列は返却後も有効なowning arrayである。
  shape、dtype、後続呼び出しからの独立性を維持する。
- GILを解放するsolver/MCTSは、解放前にPythonから渡されたmutable inputをsnapshotし、
  実行中に呼び出し元のmutationを読まない。

## 変更時の確認

公開名と分類は `tests/test_refactoring_contracts.py`、具体的な順序・hash・ownershipは
`tests/test_phase0_contracts.py`、gem色とschema fingerprintは
`tests/test_gem_color_contract.py` が固定する。digestの変更が生じた場合はgolden値を
更新して通すのではなく、第2次計画の停止ゲートに従って仕様変更として分離する。
