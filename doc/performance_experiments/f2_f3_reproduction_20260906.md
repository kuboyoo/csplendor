# F2/F3の再現と証跡の参照

F1測定code `b202e6a0cbb2eded9bc2ee5e59f750428e73ca49`、F1記録
`49878b661298bf45e39c5f5ca5afa6d0e363736a`。性能再測定は不要。
[F1報告](final_main_vs_candidate_20260906.md)のCSV/manifestを正本とし、F2の単発実NN時間を
paired A/BやCI付きの高速化率として扱わない。

F3 lint是正後はtestsを含むsource digestだけが変わった。本体/build/binaryは同一。
現在の結果・digestは[是正報告](f3_lint_correction_20260906.md)と
[新manifest](f3_lint_manifest_20260906.json)を参照する。旧証跡は履歴として残す。

## 証跡だけを確認する

- [F2/F3 manifest](f2_f3_manifest_20260906.json)のhashと、`raw/f2_f3_20260906/*.json.gz`を照合する。
- `start.json.gz`にF1 raw100件のhash照合結果、再利用したテスト結果、build条件、利用側のsource hash一覧がある。
- `real_model.json.gz`は初回検証補助の失敗。`real_model_v2.json.gz`がselfplay12通過、
  `recommended_model.json.gz`がselfplay17通過。どれも上書きしない。
- `frontier_transport_identity.json.gz`が子workerの実.so mapping付きの5/7手通信確認。
- `ci_lint.json.gz`はvenv tool不足。`ci_lint_available_tool.json.gz`は候補固有7件、
  `main_ci_lint.json.gz`は同一Ruffのmain通過。Ruff0.13.3。

## 追加受入を実行する場合

これは新しい記録名を使う限定再検証手順。F4の常用環境更新は含まない。
既存`start`を再実行するとexclusive-createにより拒否する。元rawを消して再実行しない。

作業cwdを `/home/kuboyu/workspace/repos/csplendor-final-candidate` にする。
`f2_f3_record_20260906.py`は公開の実行変数だけを指定し、環境全体をdumpしない。
使用したsource/モデル/config/拡張SHAが保存済みmanifestと同じことを事前確認する。
dlsplendorはdirtyなのでHEADだけをcheckoutして同じ検証とみなさず、入力source hashを照合する。
変わっていればその差を明記した新しい受入記録とする。

```bash
# 隔離venvと同一SHAのF1 buildが残っている場合の例。review2は未使用の記録名に置き換える。
python doc/performance_experiments/f2_f3_record_20260906.py run review2_model \
  build/f2-env/bin/python -B doc/performance_experiments/f2_acceptance_probe_20260906.py \
  model --family selfplay17
python doc/performance_experiments/f2_f3_record_20260906.py run review2_frontier \
  build/f2-env/bin/python -B doc/performance_experiments/f2_acceptance_probe_20260906.py frontier
```

probeはF1の拡張path/SHAにfail-closed。新しいbuild/OSでSHAが異なる際、assertを消すのではなく、
別candidate/build provenanceを作り期待identityを別の検証記録へ接続する。F1の測定済みbinaryと称さない。
モデルpathはGUI README/登録コードから確認したselfplay12/selfplay17だけを許可している。
外部package/modelをcopyせず読み取り、学習・大容量棋譜出力をしない。

lintの再現（読み取り専用。前回e486e27は7件FAIL、是正後は同じrule・対象でPASS）：

```bash
python -m ruff check --target-version py38 --select E4,E7,E9,F,W,I csplendor tests
python -m ruff check --target-version py38 --select S csplendor
```

是正検証の実コマンド・stdout/stderr・終了codeは `raw/f3_lint_20260906/`。
新規recorderもexclusive-createで既存rawを上書きしない。再検証する場合は未使用の記録名を使う。
旧`verify_f1()`／`start`はテストを含む旧source digestを要求する履歴用のため、
是正後へそのまま再実行して通過するとはしない。是正recorderの`finish`は
承認5ファイルだけの差・本体/build不変・旧raw hashを照合する（既存名の再生成は不可）。

F4用のclean build/installと旧版復帰は[runbook](f4_integration_runbook_20260906.md)を参照。
ブラウザ/GPU/Genbu/実NN A/B未実施を、上記probe通過によってPASSへ変更しない。
