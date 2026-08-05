# 第2次リファクタリング R3 rule primitive整理

## 目的と境界

合法手生成、`Game`のvalidation、rule transitionに重複していた純粋照会を
`rule_query.h`へ集約した。公開Python/C++ API、合法手集合・順序、`Action.pack()`、
snapshot、hash、失敗時のpartial mutation契約は変更していない。

依存方向は次のとおりである。

```text
Board / PlayerState / Action
        |
        v
rule_query.h（非所有・allocation-freeな純粋照会）
   |             |                 |
   v             v                 v
MoveGenerator  Game validation  rule_transition.h
```

## 共通化したprimitive

- visible / reserved cardの探索。editor状態で同一IDが重複した場合も従来どおり先頭を返す。
- bonus適用後の実効カード費用とGold支払検証。
- token action後の所持数、必要返却数、返却内容の検証。
- cached eligibility maskと盤上順序に基づく貴族候補。
- final round開始条件と、得点・購入枚数tie-breakによる勝者判定。

`MoveGenerator::EligibleNobles`と公開wrapperはsource互換のため維持し、内部実体を
`FixedStack` aliasへ移した。エンコーダのvisible/reserved source解決も同じ正本へ委譲した。

validationは入力全体を検査してから既存transitionへ渡す。低レベルtransitionは探索側の
rollbackを前提とする既存契約を保ち、色順に更新した後で後続色またはGold検査が失敗した
場合のpartial mutationも意図的に変更していない。

## 同値性

`rule_query_unit`は抽出前の式を独立したreferenceとして保持し、32 seed・最大96 plyの
reachable corpusで次を検査する。

- 2,770局面、97,354合法手の全pack列と順序
- 全生成手がsemantic validationを通ること
- source、実効費用、返却、支払、貴族、終局照会の旧新一致
- 各局面の先頭・中央・末尾手についてvalidated/trusted transition後のsnapshotとhash一致
- duplicate ID、不正返却、過剰Goldなどeditor境界値

コーパスbyte列は、seed=`uint32 LE`、ply=`uint32 LE`、手数=`uint16 LE`、各code=`uint64 LE`
で固定する。SHA-256は
`07e91f5e8f547073284876060674a4755c2c73edc36c5d105ebaf81e1e3347c7`、native
FNV-1a fixtureは`0x5048f8689f1dcee7`である。

## 性能

2026-08-05、Ubuntu x86_64、Python 3.12.1、GCC 13.3、portable ReleaseでR2-Cと比較した。
隔離wheelを使い、実行順を反転した7組、各20 sampleのgroup中央値比を記録する。

| workload | R3 / R2-C 中央値 |
|---|---:|
| Python legal actions/s | 1.041 |
| legal action codes/s | 0.989 |
| legal action count/s | 0.981 |
| C++ playout moves/s | 1.003 |

合法手countの差は1.9%で5%停止基準内、MCTSに近いC++ playoutは回帰しなかった。
primitiveはすべてinline・固定容量で、hot pathへheap allocationやruntime polymorphismを
追加していない。

## 検証

- Python全testと`py_compile`
- GCC / Clang Release native test
- ASan/UBSan、TSan native test
- GCC / Clang strict binding build
- reachable全手differential/golden test
- paired runtime benchmark、clang-format、`git diff --check`

R3の完了条件を満たしたため、次の独立変更はR4のversioned encoder / feature schema
一元化とする。
