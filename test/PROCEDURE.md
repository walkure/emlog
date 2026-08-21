# 実機テスト手順書

`emlog.c` / `emlog_fuse.c` の6件のバグ修正（Finding 1〜6、`NOTES.md` 参照）を実機で確認するための手順。

## 重要: リスクについて

- 全スクリプトが **`sudo insmod` / `sudo rmmod`** を実行し、`emlog.ko`（今回パッチを当てたカーネルモジュール）をこのホストに実際にロードします。
- カーネルモジュールのロードは原理上、バグがあれば **kernel panicやハングを引き起こしうる** 操作です。特に `run_kernel_race_test.sh` は、修正前のコードなら確実にUAFを踏むはずだった競合パターンを実機で意図的に再現し、修正が効いているかを確認するテストです。修正が正しければ問題は起きないはずですが、「絶対に落ちない」保証はできません。
- Raspberry Piでkernel panicが起きた場合、SSH越しの操作は失われ、**物理的な電源再投入が必要になる可能性があります**。再起動してよい状態（他の重要な作業が動いていない等）で実行してください。
- 各スクリプトは自分でモジュールをアンロードするtrapを持っていますが、途中でパニックした場合はtrapも実行されません。

**推奨**: 最初は `run_basic_smoke_test.sh`（一番安全）から実行し、問題なければ徐々にレース系のテストに進んでください。

## 事前準備

```sh
cd /home/walkure/emlog
make            # ビルド確認（各スクリプトも内部でmakeしますが先にやっておくと早い）
lsmod | grep emlog   # 何もロードされていないことを確認
```

もし何か既にロードされていたら `sudo rmmod emlog` してから始めてください。

## 個別テスト

### 1. 基本動作確認（最も安全） — `test/run_basic_smoke_test.sh`

insmod → `/dev/emlog` の存在・open確認（Finding 3の回帰チェックも含む）→ `mknod` した手動デバイスへの書き込み・`nbcat`での読み出し・`emlog_stat`確認 → rmmod、という一連の基本機能確認です。

```sh
test/run_basic_smoke_test.sh
```

### 2. Finding 3 専用: 小さいemlog_max_sizeでの起動確認 — `test/run_small_maxsize_test.sh`

`insmod emlog.ko emlog_max_size=64`（256未満）でロードし、`/dev/emlog` が開けることを確認します。**修正前のコードだとここで `-ENXIO` になっていたはずの箇所**です。

```sh
test/run_small_maxsize_test.sh
```

### 3. Finding 1 専用: open/closeレースのストレステスト — `test/run_kernel_race_test.sh`

**一番リスクが高いテストです。** `test/stress_open_close.c` をコンパイルし、同一のemlogデバイスに対して複数プロセス（デフォルト16並列 × 20000回）が高速に open→write→close を繰り返します。`emlog_autofree` はこのフォークではデフォルトtrueなので、参照カウントが頻繁に0になり、修正前のコードなら `emlog_open()` のUAFレースウィンドウを高確率で踏みます。

実行中/実行後に `dmesg` を確認し、`BUG:`、`Oops`、`WARNING:`、`general protection fault` 等が出ていないか、また終了後に `rmmod` が正常に通るか（＝カーネル内部状態が壊れていないか）を確認します。

```sh
test/run_kernel_race_test.sh
# 並列数・回数を変えたい場合:
WORKERS=32 ITERS=50000 test/run_kernel_race_test.sh
```

**実行前に、この後説明を読んで「実行してよい」と判断してから叩いてください。** 何か問題が起きた場合、シリアルコンソールや物理アクセスがないと復旧できない可能性があります。

### 4. Finding 2・5・6 専用: emlog_fuseの動作確認 — `test/run_fuse_tests.sh`

事前に `sudo insmod emlog.ko` が必要です（このスクリプト自身はモジュールをロードしません）。

```sh
sudo insmod ./emlog.ko
test/run_fuse_tests.sh
sudo rmmod emlog
```

内容:
- **テストA (Finding 2)**: ファイルを開いたまま`rm`（unlink）し、開いたままのfdへの書き込みが引き続き成功すること、`unlink`後に同名で作り直したファイルに古いfdのデータが混入しないことを確認。
- **テストB (Finding 5)**: 開いたままの `B.log` に別ファイル `A.log` を `mv`（rename）で上書きし、`B.log` が正しく置き換わること、置き換えられた古い`B.log`のfdが書き込み可能なまま残ること（クラッシュしないこと）を確認。
- **テストC (Finding 6)**: 一度closeした後に同じファイルを再度開いても、以前書き込んだ内容が消えずに残っていること（append時も既存データが保持されること）を確認。

このテストはFUSEマウント権限の都合上 `sudo ./emlog_fuse` をバックグラウンド起動します（`--allow-other`は使わずrootのまま）。失敗時のログは `/tmp/emlog_fuse_test.log` に残ります。

## まとめて実行

上記4つを順番に自動実行するランナーもあります（失敗したら即座に停止します）。

```sh
test/run_all.sh
```

## トラブルシュート

- **`rmmod` が "Device or resource busy" で失敗する**: どこかにemlogデバイスを開いたままのプロセスが残っています。`lsof | grep emlog` や、各スクリプトが使う一時デバイス（`/tmp/emlog_*`）を開いているシェルが残っていないか確認してください。
- **`insmod` が "File exists" 等で失敗する**: 既にモジュールがロードされています。`sudo rmmod emlog` してから再実行してください。
- **`run_fuse_tests.sh` でマウントがタイムアウトする**: `/tmp/emlog_fuse_test.log` を確認してください。emlogモジュール未ロード、`/dev/fuse` の権限、他プロセスによる同名マウントの残留などが典型的な原因です。
- **何かがハングして戻ってこない**: 別ターミナル（別セッション）から `dmesg | tail -50` を見て、カーネル側で何か起きていないか確認してください。ハングしたまま応答がない場合、残念ながら電源再投入が必要になることがあります。
