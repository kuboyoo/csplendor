# 3D-P1 事前計画

対象：action-localなめくれ候補scoreの一回計算のみ。3D-P2、rollback、TT、global cache、score式・数値演算順の変更は行わない。

- 基準：`35048e7`（R0で採用済みと確認した3C実内容を同一hashでcommit）。R0記録を取り込んだ開始HEADは`86b7473`。元の2worktreeのユーザー差分は変更しない。
- primary：exact hidden_reserve、depth7、1,000,000 nodes、CPU4。全体時間・同一処理量NPS。
- guards：exact five_moves depth3/500k、同depth7/500k warm、visible five_moves/100k、forced_pass/1M、editor_fallback exact、reveal_heavy proof on/off。守備reserveはattackerを手番と逆にしたfixtureで補完。
- 初期smoke後、primary/deep・shallow・warmは22pair/11 crossover blocks、10,000 bootstrap、既存fixed-slot runner。primaryを別実行の22pair holdoutで確認する。候補選定後の最終Releaseを採否対象とする。
- 採用目安：primaryで再現する2〜3%以上、95% CIが1を跨がない。主要guardの2%超の退行はCIと独立再測定で判定。microのみ改善なら棄却。
- semantic：全candidate score・全sort後IDを旧comparatorとVERIFY比較。固定探索のresult/unknown、ordered root actions/outcomes、nodes/legal/terminal/memo、主手順、proof/frontierを維持。diagnostic score回数は一致gateから除外。
- 段階：小単体＋differential→smoke→正式paired＋holdout→採用候補のみfull native/Python、ASan+UBSan。非同期MCTSはこの変更の対象外。hardware perfは既知の権限制限のためN/A、設定変更なし。
- 永続記録は本directoryと`raw/phase3dp1/`。build生成物はgit対象外。採否と未確認を報告し、このチケットで停止・作業branchへpushする。

正式測定後の追加確認：37nodeだけのproof-off guardは時間が短くCIが広かったため、コードや閾値を変更せず22pairを追加する。最良値を選ばず初回・追加をともに報告する。primaryの独立holdoutも予定どおり行う。
