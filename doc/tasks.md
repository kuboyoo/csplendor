# csplendor 継続課題

最終更新: 2026-08-05

第1次リファクタリング（Phase 0--7）と第2次リファクタリング（R0--R8）は完了した。
完了内容は[`refactoring_plan/README.md`](refactoring_plan/README.md)と
[`refactoring_plan_v2.md`](refactoring_plan_v2.md)に残し、この文書では未完了の独立課題だけを扱う。

## 並列MCTSのstable化

複数threadは引き続きexperimental opt-inである。stable/defaultへ昇格する前に次を満たす。

- scheduled sanitizer/soakの継続成功と可変scheduler seedによるinterleaving拡張。
- 展開済みnodeへ再到達した場合の二次feature signature照合。
- 実NN/GPUでのlatency・batch・utilization計測。
- fixed-time探索品質とself-play canary。
- soft timeoutを補う外部watchdog運用の確認。

## 性能候補

現在のMCTS hot pathでは直接action mask/decode、bitset走査、compact edge、O(1) quiescence監査を
導入済みである。次の変更はprofileで支配項を確認してから独立A/Bとして行う。

- platform別SIMD affordabilityの試作とportable scalar実装との比較。
- bank/inventory maskの追加がbranch/cache missを実際に減らすかの検証。
- `Action`の内部compact表現。公開layout/packed codeは維持する。
- inference batch変換と実model待ち時間の分離計測。
- solver別のnodes/s、peak RSS、proof変換costの継続計測。

## Domainと互換性

- 公開C++ fieldを将来private化する場合の互換shimとmajor-version移行。
- production undoをdelta化するかは、full snapshotより優位な実workloadが確認できた場合だけ再評価する。
- legacy replay pickleを非実行形式へ移行する。現行readerは管理者配置の信頼済みローカルfile専用。
- encoder/feature/snapshot/traceの新version追加時に旧version readerと移行期間を定義する。

## Release運用

- C++ coverageはreport-onlyを継続し、複数回の分布と未被覆領域を確認後に閾値を提案する。
- PyPI公開前にmanylinux repair、TestPyPI、配布wheelのmacOS/Windows隔離installを行う。
- setuptoolsの2027年非互換化より前にlicense metadataをSPDX文字列へ移行する。
- `usi`仕様や`dlsplendor` consumer変更時はcross-repository compatibility testを同時に更新する。

完了済み項目をこの一覧へ戻さず、仕様変更・性能改善・release作業はそれぞれ独立PRとして扱う。
