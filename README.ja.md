emlog -- 組み込み向けログデバイス (EMbedded-system LOG-device)
=======================================

*[English version](README.md)*

Version 0.72, 21 August 2026

作者:   Jeremy Elson <jelson@circlemud.org><br/>
Webページ:
* http://www.circlemud.org/~jelson/software/emlog
* https://github.com/nicupavel/emlog
* https://github.com/walkure/emlog (このフォーク)

--------------------------------------------------------------------------


emlogとは？
==============

emlogは、プロセスの最新の（そして*最新だけの*）出力に簡単にアクセスできるようにするLinuxカーネルモジュールです。ログファイルに対する"tail -f"のように動作しますが、必要なストレージ容量が決して増えないという違いがあります。これは、完全なログファイルを保持するだけのメモリやディスク容量がない組み込みシステムで、それでも直近のデバッグメッセージが（エラー発生後などに）必要になる場合に役立ちます。

emlogカーネルモジュールは単純なキャラクタデバイスドライバとして実装されています。このドライバは有限の循環バッファを持つ名前付きパイプのように振る舞います。バッファのサイズは簡単に設定できます。バッファにさらにデータが書き込まれると、最も古いデータが破棄されます。emlogデバイスから読み込むプロセスは、まず既存のバッファを読み、その後は新しいテキストが書き込まれるたびにそれを見ることになります。ちょうど"tail -f"でログファイルを監視するのと同じです（プロセスがブロックせずにログの現在の内容を取得したい場合のために、非ブロッキング読み込みにも対応しています）。

現行バージョンのemlogは、2.6.x系（少なくとも2.6.32以降）、3.x系、4.x系（少なくとも4.18-rc4まで）のほとんどのLinuxカーネルで動作するはずです。

emlogはフリーソフトウェアであり、GNU General Public License (GPL) version 2の下で配布されています。詳細は配布物に含まれるCOPYINGファイルを参照してください。


emlogの使い方
==================

### 1: emlogの設定・コンパイル・インストール

   現在動作しているカーネル向けにemlogをコンパイルしたいだけであれば、単に以下を実行します。
   ```bash
   make
   ```

   そうでない場合は、KVER（`/lib/modules/<KVER>/build`にあるLinuxカーネルソース向け）またはKDIR（その他のパス向け）のいずれかを設定する必要があります。
   ```bash
   make KDIR=/usr/src/linux
   ```

   クロスコンパイル（例えばARMターゲット向け）の場合は、CROSS_COMPILEを指定します。
   ```bash
   make CROSS_COMPILE=arm-linux-gnueabihf-
   ```

   カーネルモジュールのコンパイルにはKDIR/KVERと組み合わせることもできます。
   ```bash
   make KDIR=/path/to/kernel ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
   ```

   カーネルモジュール本体（`emlog.ko`）と、後述する2つのユーティリティ（`nbcat`と`emlog_stat`）の、計3つのファイルが生成されるはずです。libfuseが利用可能であれば、`emlog_fuse`も自動的にビルドされます。これらはカレントディレクトリから直接使うこともできますし、以下でインストールすることもできます。
   ```bash
   make install
   ```

#### Raspberry Pi OS上でのビルド

   emlogが存在する理由そのもの――決してサイズが増えない固定サイズのログバッファ――は、microSDカードから起動するRaspberry Piのようなボードにまさにぴったりです。そこでは、普通のログファイルが徐々にカードをすり減らしたり、容量を圧迫したりするのは避けたいところです。Raspberry Pi OS（Debianベース、arm64、汎用Debianカーネルではなく Raspberry Pi Foundation独自のカーネルを使用）上では、Debianの`linux-headers-$(uname -r)`ではなく、*一致する*カーネルヘッダパッケージが必要です。
   ```bash
   sudo apt-get update
   sudo apt-get install -y raspberrypi-kernel-headers libfuse-dev libcap-dev
   ```
   - `raspberrypi-kernel-headers`は`archive.raspberrypi.org`のaptソース（Raspberry Pi OSにはデフォルトで`/etc/apt/sources.list.d/raspi.list`として存在）から取得されます。`make`は`/lib/modules/$(uname -r)/build`に対してビルドを行うため、現在動作中の`raspberrypi-kernel`パッケージと**同一バージョン**である必要があります。もし後で`insmod`が"Invalid module format"エラーでモジュールを拒否した場合は、`dmesg`で`vermagic`や`disagrees about version of symbol module_layout`というメッセージが出ていないか確認してください。これはヘッダと動作中カーネルがずれてしまった（例えば`apt upgrade`で新しいカーネルが入ったが、まだそのカーネルで再起動していない、など）ことを意味しており、emlog自体の問題ではありません。
   - `libfuse-dev`（`libfuse3-dev`ではない）と`libcap-dev`は`emlog_fuse`（後述）が必要な場合にのみ必要です。それ以外はこれらが無くてもビルドできます。

   これらをインストールした後は、上記と全く同じように、単に`make`（`KDIR`/`KVER`の指定は不要）を実行するだけで全てビルドされます。

#### DKMS経由でのインストール（非力なマシンやmicroSD運用機に推奨）

   別の環境でビルドした`.ko`を`insmod`するのは、その`vermagic`がターゲット機の動作中カーネルと完全に一致している場合にしか機能しません。これは、複数台のRaspberry Pi Zeroのような小型ボード群のように、それぞれが独立してアップデートされ、カーネルバージョンが徐々にバラバラになっていく運用では実際に問題になります。DKMSは、インストール時やカーネルアップグレード時に**ターゲット自身の上で**モジュールを再ビルドすることでこれを回避します。つまり、古くなって使えなくなる事前ビルド済みバイナリというものが存在しません。このプロジェクトには`dkms.conf`が同梱されているので、`dkms`と（上記の）対応するカーネルヘッダがインストールされていれば：
   ```bash
   sudo apt-get install -y dkms
   sudo make dkms_install
   ```
   これにより、ソースが`/usr/src/emlog-<version>/`に登録され、動作中のカーネルに対してビルドされ、インストールされます。その結果、`modprobe emlog`（および`modprobe -r emlog`）が他の一般的なカーネルモジュールと同じように使えるようになります――Raspberry Pi 4とRaspberry Pi Zero 2 Wの両方で、ビルド・`modprobe`・`/dev/emlog`の出現・`emlog_stat`によるクエリ・複数回のクリーンな`modprobe`/`rmmod`サイクルまで、一通り実機で検証済みです。`dkms.conf`の`AUTOINSTALL="yes"`により、今後`apt upgrade`で新しいカーネルがインストールされた際にも、DKMSが自動的にemlogを再ビルドします。

   **DKMSが対象とするのは`emlog.ko`のみです。** `dkms.conf`の`BUILT_MODULE_NAME[]`はカーネルモジュールだけを指しており、`make dkms_install`はこのプロジェクトのトップレベル`Makefile`ではなく、カーネル自身のout-of-treeモジュールビルド（`make -C <kernel build dir> M=<source dir>`）を実行します。そのため、`nbcat`・`mkemlog`・`emlog_stat`・`emlog_fuse`は、自動カーネルアップグレード時の再ビルドも含めて、**DKMSではビルドも最新化もされません**。これらは`dkms_install`とは別に、通常通り（`make` / `make install`、ステップ1参照）ビルド・インストールしてください。

   Pi Zeroのような非力な小型ボードの場合、これは（小さな単一ファイルの）モジュールが初回インストール時と各カーネルアップグレード後にネイティブでコンパイルされることを意味します――事前ビルド済みの`.ko`をコピーするより遅くはなりますが、バージョン不一致で壊れることは決してありません。これは、起動失敗をデバッグするためにいちいちSSHでログインしたくないようなボードでは、より重要な意味を持ちます。

   削除するには：
   ```bash
   sudo make dkms_remove
   ```
   これは`/usr/src/emlog-<version>/`を残す点に注意してください（Makefileの`dkms_remove`ターゲット内のコメントアウトされた行を参照）。これも消したい場合は手動で削除してください。


### 2: emlogモジュールをカーネルにロードする

   カレントディレクトリのemlogを直接使う場合は、`insmod`コマンドでモジュールを挿入します。
   ```bash
   insmod emlog.ko
   ```

   そうでなければ、`modprobe`でも動作するはずです。
   ```bash
   modprobe emlog
   ```

   最大バッファサイズの上限を別途指定するには：
   ```bash
   modprobe emlog emlog_max_size=2048
   ```

   デフォルト（`emlog_autofree=1`）では、そのデバイスをopenしているプロセスが誰もいなくなった時点でバッファは解放されます――つまり、何かが既にそのデバイスを開いた状態でない限り、バッファが解放される前の別々のopen/closeセッションをまたいでデータが生き残ることは**ありません**。openをまたいでバッファを永続させたい（従来のemlogの動作）場合は、`emlog_autofree=0`でロードしてください（これが実際どういう意味を持つかはステップ4と下記の"Other Usage Notes"を参照）。
   ```bash
   modprobe emlog emlog_autofree=0
   ```

   成功すると、以下のようなメッセージが
   ```
   emlog:emlog_init: version 0.72 running, major is 251, MINOR is 1, max size 1024 K.
   ```
   カーネルログに表示されるはずです（`dmesg`で確認できます）。また、`lsmod`や`cat /proc/modules`でモジュールが挿入されたことを確認できます。


### 3: emlog用のデバイスファイルを作成する

   デフォルトでは、（`/dev`にdevtmpfsがマウントされている、かつ/またはudevが動作している場合）最小限に割り当てられたバッファを持つデバイスファイル`/dev/emlog`が作成されます。これはすぐに書き込み/読み込みが可能な状態です。

   より多くのデバイス/バッファが必要な場合は、`mkemlog`プログラムを使ってプロセスが書き込めるデバイスファイルを作成できます。

   使い方: `mkemlog <logdevname> [size_in_kilobytes] [mode]`

#### 3.1: mkemlogの使用例

   8kバッファ、パーミッション0660でログファイルを作成する

   ```bash
   mkemlog /tmp/testlog
   ```

   17kバッファ、パーミッション0660でログファイルを作成する

   ```bash
   mkemlog /tmp/testlog 17
   ```

   12kバッファ、パーミッション0644でログファイルを作成する
   ```bash
   mkemlog /tmp/testlog_12k 12 0644
   ```

   18kバッファ、パーミッション0644で、UID==1000のユーザーが所有者となるログファイルを作成する
   ```bash
   mkemlog /tmp/testlog_18k 18 0644 1000
   ```

   mkemlogを使うには`/dev/emlog`ファイルが作成されている必要があります。

#### 3.2: emlogを手動で作成する

   `/dev`にdevtmpfsがマウントされていない、かつ/またはudevが動作していない場合は、`mknod`を使って手動でemlogを作成し、プロセスが書き込めるデバイスファイルを用意できます。メジャー番号とマイナー番号という2つの数字を知っておく必要があります。メジャー番号は以下のいずれかの方法で調べられます。
   ```bash
   ls -l /dev/emlog
   grep emlog /proc/devices
   (source /sys/class/emlog/emlog/uevent ; echo "$MAJOR")
   dmesg | grep emlog
   ```
   マイナー番号は、そのデバイスファイル用のリングバッファの*サイズ*をキロバイト単位（例: 1024バイト）で指定するのに使われます。例えば、'testlog'という名前の8Kバッファを作成するには：
   ```bash
   mknod /tmp/testlog c 251 8
   ```

   デバイスはいくつでも好きなだけ作成できます。内部的には、emlogはファイルのinode番号とデバイス番号を使って、そのファイルがどのバッファを指しているかを識別します。なお、内部バッファサイズは現在128Kに制限されています。


### 4: 新しいデバイスファイルへの書き込みと読み込み

   デバイスファイルが作成されたら、通常の名前付きパイプと同じように、デバイスファイルに書き込むだけです。例えば：
   ```bash
   echo hello > /tmp/testlog
   ```

   バッファが容量不足になることは決してないため、ログへの書き込みがブロックすることはありません。古いデータは単に新しいデータで上書きされます。

   **注意点**: デフォルトの`emlog_autofree=1`では、上記のバッファは`echo`のシェルリダイレクトがファイルをcloseした瞬間に解放されます――つまり、（`echo`が既に終了した後の）新しいプロセスによる*別の*、後からの`cat /tmp/testlog`コマンドは、"hello"ではなく、空の新しく確保されたバッファを見ることになります。このように別々のopenをまたいでデータを残したい場合は、まずモジュールを`emlog_autofree=0`（ステップ2）でロードしてください。

   デフォルト設定のままこれを実際に動かして見るには、writer側がcloseする*前に*reader側を起動しておく必要があります――例えば、まず1つ目のターミナルで`cat`を起動します。
   ```bash
   cat /tmp/testlog
   _      [データを待ってブロックしている -- ちょうどtail -fのように]
   ```
   ...次に、別のターミナルで：
   ```bash
   echo hello > /tmp/testlog
   ```
   ...すると、1つ目のターミナルにはすぐに以下のように表示されます。
   ```
   hello  [たった今書き込まれたhelloが見える]
   _      [... そしてここにカーソルがある。'cat'プロセスはまだ
           ブロックされていて、新しい入力を待っている。新しいデータは
           他のプロセスによってデバイスに書き込まれるたびに表示される。]
   ^C     [読み込みを止めるには、例えばcontrol-cを使う。]
   ```

   バージョン0.40以降、emlogのバッファは複数の同時読み込みによって正しく読み込み・監視できます。emlogデバイスに書き込まれたデータは、より新しいデータで上書きされるか、emlogモジュールが削除されるまで消えることはありません。（バージョン0.30以前では、データは最初に読み込まれた時点でバッファから削除されていました。）


### 5: 使い終わったらemlogを削除する

   `rmmod emlog`または`modprobe -r emlog`と入力すると、emlogカーネルモジュールを削除し、関連する全てのバッファを解放します。これは全てのemlogデバイスファイルがcloseされるまでは実行できません。


その他の使用上の注意
=================

* emlogは、以下の2つの条件のいずれかが真である場合に、そのデバイスファイルのために固定サイズのバッファを確保します。

  1.  プロセスがそのファイルを読み込みまたは書き込み用にopenしている
  2.  プロセスがそのパイプにテキストを書き込んだことがある

*最後の*プロセスがデバイスをcloseした後もバッファが生き残るかどうかは、`emlog_autofree`モジュールパラメータ（ステップ2参照）に依存します。デフォルトの`emlog_autofree=1`では、そのデバイスを開いているプロセスが誰もいなくなった時点でバッファは解放されます。openをまたいでバッファを永続させたい（従来のemlogの動作）場合は、`emlog_autofree=0`でモジュールをロードしてください――その場合、たくさんの大きなemlogデバイスを作って全てに1バイトずつ書き込むことで、（当然ながら）仮想メモリを埋め尽くすことが可能になります。それはやめましょう。`emlog_autofree`の値に関わらず、emlogカーネルモジュールが削除されると全てのバッファが解放されます。

* 非ブロッキング読み込みは動作します。すなわち、ioctl()でO_NONBLOCKを設定すると、データの準備ができていない場合にEAGAINが返されます。加えて、select()やpoll()関数もemlogデバイスに対して正しく動作します。

* emlogの配布物には、`nbcat`という小さなユーティリティが含まれています。nbcatは`cat`に似ていますが、非ブロッキング読み込みを使用します。このユーティリティは、新しい入力を待ってブロックすることなく、emlogデバイスの現在の内容をコピーするために使えます。例えば：
   ```bash
   nbcat /var/log/emlog-device-instance > /tmp/saved-log-file
   ```
...とすると、指定したemlogデバイスの現在の内容が`/tmp`のファイルにコピーされます。あるいは、`dd`を使うこともできます。
   ```bash
   dd if=/var/log/emlog-device-instance of=/tmp/saved-log-file bs=4096 iflag=nonblock 2>/dev/null
   ```

* `emlog_stat`というユーティリティが、ioctl経由でバッファの状態を照会するために含まれています。このユーティリティは、バッファサイズ、現在のデータ長、書き込まれた総バイト数、開いているファイルディスクリプタの数を表示します。例えば：
   ```bash
   emlog_stat /tmp/testlog
   ```
...とすると、そのemlogデバイスの現在の状態が表示されます。複数のデバイスを一度に照会することもできます。
   ```bash
   emlog_stat /tmp/testlog /dev/emlog
   ```


emlog_fuse
==========

`emlog_fuse`は、ファイルへの書き込みを透過的にemlogの循環バッファデバイスへリダイレクトするFUSEファイルシステムです。マウントポイント以下に作成されたファイルは、自動的にemlogカーネルバッファによってバックされます。これにより、アプリケーション側のコード変更なしに、通常のパスへログを書き込ませつつ、実際のストレージはemlogカーネルモジュールが管理する固定サイズの循環バッファにする、ということが可能になります。

### ビルド要件

* `libfuse-dev`（libfuse2、FUSE 2.6以降）
* `libcap-dev`（省略可、非rootでのcapabilityチェック用）

libfuseが利用可能であれば、`emlog_fuse`は`make all`の一部として自動的にビルドされます。libfuseが見つからない場合は、黙ってスキップされます。

### 使い方

`emlog_fuse`を起動する前に、emlogカーネルモジュールがロードされている必要があります。

```bash
# 基本的な使い方（root）
sudo emlog_fuse /var/log/myapp

# バッファサイズをカスタマイズ（ファイルごとに256 KB）
sudo emlog_fuse /var/log/myapp -o buffer_size=256

# ユーザー名でファイルの所有者を設定（passwdからuidとgidを解決）
sudo emlog_fuse /var/log/myapp -o user=myapp

# 数値のuid/gidでファイルの所有者を設定
sudo emlog_fuse /var/log/myapp -o uid=1000,gid=1000

# アンマウント
sudo fusermount -u /var/log/myapp
```

### 非root環境での使い方

`emlog_fuse`がemlogデバイスノードを作成するには`CAP_MKNOD`が必要です。ファイルの所有者が実行中のユーザーと異なる場合は`CAP_CHOWN`も必要です。以下でcapabilityを付与できます。
```bash
sudo setcap 'cap_mknod,cap_chown+ep' ./emlog_fuse
./emlog_fuse /var/log/myapp -o user=myapp
```

### オプション

| オプション | 説明 |
|---|---|
| `-o buffer_size=N` | ファイルごとのバッファサイズ（KB単位、デフォルト: 128） |
| `-o dev_dir=PATH` | バックエンドのデバイスファイルを置くディレクトリ（デフォルト: `/dev/.emlog_fuse_devs`） |
| `-o uid=UID` | ファイル所有者のUIDまたはユーザー名 |
| `-o gid=GID` | ファイル所有者のGIDまたはグループ名 |
| `-o user=NAME` | passwdエントリからuidとgidの両方を設定する |
| `--allow-other` | FUSEの`allow_other`を強制的に有効化する |
| `--no-allow-other` | FUSEの`allow_other`を強制的に無効化する |
| `-d` | FUSEデバッグモード（詳細な出力） |

`uid`がroot以外のユーザーに設定されている場合、指定したユーザーがマウントにアクセスできるよう、`allow_other`が自動的に有効化されます。

### 仕組み

マウントポイント以下でファイルが作成またはopenされると、`emlog_fuse`は（`mknod`経由で）emlogカーネルモジュールにバックされたキャラクタデバイスノードを作成します。FUSEファイルへの全ての書き込みはこのデバイスに転送されます。読み込みは現在のバッファ内容を返します（非ブロッキング）。あるファイルの全てのファイルディスクリプタがcloseされると、バックエンドのデバイスは自動的に削除されます。


Emlogとdevtmpfs
==================

デフォルトでは、emlogは最小のバッファサイズを持つデバイスを`/dev/emlog`（あるいはdevtmpfsがマウントされている場所）に1つだけ作成します。あらゆる可能なバッファサイズのデバイスをあらかじめ作成しておくことにはあまり意味がありません。emlogでは、ファイルシステム上の好きな場所にいくつでもログデバイスを作成できます――モジュールはinode番号に基づいてそれらを区別します。単一のログデバイスが常に単一の場所（/dev）に存在するというのは、それほど有用ではありません。


トラブルシューティング
===============

Q: モジュールを挿入した時*以外*のタイミングで"I/O error"が出ます。

A:  おっと -- emlogのバグを見つけてしまいましたね。報告してください。


Q:  emlogデバイスファイルへの読み込みまたは書き込みをしようとすると、"no such device"というエラーが出ます。

A:  これはおそらく、emlogカーネルモジュールがロードされていないか、デバイスファイルのメジャー番号がemlogが登録したメジャー番号と一致していないことを意味します。emlogが使用しているメジャー番号を確認するには、以下のいずれかの方法を使ってください。
```bash
grep emlog /proc/devices
(source /sys/class/emlog/emlog/uevent ; echo "$MAJOR")
dmesg | grep emlog
```


Q:  emlogデバイスファイルへの読み込みまたは書き込みをしようとすると、"invalid argument"というエラーが出ます。

A:  emlogデバイスファイルの*マイナー*番号は、emlogのリングバッファに使用するキロバイト（1,024バイト）数を表す、1から128の間の数値でなければなりません。`mknod`文に有効なマイナー番号を指定していることを確認してください。0は使わないでください。


Q:  新しいemlogファイルをopenしようとすると、"no memory"エラーが出ます。

A:  仮想メモリが不足しているようですね。


Q:  emlogドライバを削除（`rmmod emlog`）しようとすると、"Device or resource busy"や"rmmod: ERROR: Module emlog is in use"というエラーが出ます。

A:  これは、あるプロセスが現在emlogデバイスを使用中であることを意味します。ドライバを削除できるようになるには、全てのプロセスが全てのemlogデバイスファイルをcloseするまで待つ必要があります。`lsof`を使って、どのファイルがどのプロセスによって使用中かを確認してみてください。


Q:  `cp /tmp/emlog-test /tmp/saved-log-copy`と入力して、現在のemlogバッファのコピーを別のファイルに保存しようとしていますが、cpがそのまま永遠に固まってしまいます。

A:  `cp`は、emlogデバイスに対して`cat`が行うのと同じように、さらなるデータを待ってブロックしています。emlogの配布物に含まれる非ブロッキング版のcatユーティリティ、`nbcat`を使ってください。例えば：
   ```bash
   nbcat /tmp/emlog-test > /tmp/saved-log-copy
   ```


Q:  お前のせいで私のコンピュータがクラッシュした。

A:  申し訳ない。もし問題を再現できるなら、修正を試みます。


既知のバグ
==========
 * ~~[einfoの割り当て/破棄における競合状態](https://github.com/nicupavel/emlog/issues/10)~~ --
   このフォークで修正済み。`emlog_open()`はかつて、既存のeinfoを検索し、リストロックを解放してから、その後で参照を取得していました。この間に、並行して実行された`emlog_release()`が最後の他のfdをcloseする（`emlog_autofree`がデフォルトで`1`になった今、einfoが解放される）と、use-after-freeが発生していました。現在は検索と参照取得が単一のアトミックなクリティカルセクションになっています。
 * [モジュールの再ロード時の"sysfs: cannot create duplicate filename"](https://github.com/nicupavel/emlog/issues/12) --
   このフォークで修正済み。`emlog_remove()`は誤った`dev_t`（`/dev/emlog`が実際に`device_create()`された際のマイナー番号ではなく、chrdev領域のベースマイナー番号）で`device_destroy()`を呼び出していたため、アンロード時にデバイスのsysfsエントリが実際には削除されず、それ以降の`insmod`が全て`-EEXIST`で失敗する原因になっていました。パッチが当たっていないモジュールで一度これが発生してしまうと、再起動（またはkexec）以外に回復方法はありません――その後パッチ済みのモジュールをロードし直しても、以前の未パッチのロードで既にリークしたsysfsエントリを遡って片付けることはできません。


バグ報告、パッチ、苦情、賞賛、そしてCentral Services Form 27B/6の提出は、[Emlog githubページ](https://github.com/nicupavel/emlog)で歓迎されています。


バージョン履歴
===============
### Version 0.72 (August 21, 2026) -- [walkure/emlog](https://github.com/walkure/emlog) fork
 - `emlog_init()`内の`class_create()`/`device_create()`の失敗判定を修正：どちらも`NULL`との比較でチェックされていましたが、現代のカーネルAPIは失敗時に（非NULLのエラーを埋め込んだポインタである）`ERR_PTR()`を返すため、実際の失敗（例えば0.71で修正されたsysfs重複ファイル名のケース）がモジュールロードの成功として黙って報告されてしまっていました。2台目のデバイス（Raspberry Pi Zero 2 W）へデプロイする中で発見。
 - DKMS経由のインストール（Pi Zeroのような非力な/microSD運用機に推奨）と、Raspberry Pi OS全般でのビルドについてのドキュメントを追加。
 - GitHub Actions CIワークフローを追加：pushのたびにビルド＋スモークテストを実行し、加えてamd64/arm64/armv7(armhf)/armv6（オリジナルのPi Zero/Zero W/Pi 1向け、Raspberry Pi OSベースのツールチェイン経由）向けにクロスビルドした`nbcat`/`mkemlog`/`emlog_stat`/`emlog_fuse`のバイナリを、タグpush時にGitHub Releaseへ添付。

### Version 0.71 (August 21, 2026) -- [walkure/emlog](https://github.com/walkure/emlog) fork
 - `emlog_fuse`を追加：通常のファイルを透過的にemlogの循環バッファデバイスでバックするFUSEファイルシステム。
 - ioctlインターフェース（`EMLOG_GET_STATUS`）と、バッファサイズ・現在のデータ長・書き込まれた総バイト数・開いているfdの参照カウントを照会する`emlog_stat`ユーティリティを追加。
 - `emlog_autofree`のデフォルトを`1`に変更（従来は`0`）。これがバッファの永続性に対して何を意味するかは"Other Usage Notes"と上記のステップ2を参照。
 - デバイス作成時の所有権（uid/gid）指定を可能に。
 - `emlog_open()`内のカーネルuse-after-freeを修正：既存のeinfoを検索する処理と、それへの参照を取得する処理が別々のクリティカルセクションになっており、並行する`emlog_release()`がその間にeinfoを解放できてしまっていた
   （[upstream issue #10](https://github.com/nicupavel/emlog/issues/10)）。
 - 誤った`dev_t`で`device_destroy()`が呼ばれていた問題を修正。これにより、アンロード時に`/dev/emlog`のsysfsエントリが削除されず、それ以降の`insmod`が全て`-EEXIST`で失敗していた
   （[upstream issue #12](https://github.com/nicupavel/emlog/issues/12)）。
 - `emlog_max_size`を256未満でロードした場合に`/dev/emlog`が開けなくなる（`-ENXIO`）問題を修正。
 - `emlog_fuse`が、unlink/置き換え時だけでなく通常のcloseでもファイルのバッファを破棄してしまっていた問題と、古い開きっぱなしのハンドルがunlinkやrename後に無関係な別ファイルへデータを漏洩させうる問題を修正。
 - `emlog_stat`が、全てのクエリが失敗していても常に終了コード`0`を返していた問題を修正。
 - `test/`以下に回帰テストスイートを追加（`test/PROCEDURE.md`参照）。

### Version 0.70 (July 10, 2018)
 - /dev/emlogのデフォルトサイズを1KBから256KBに変更。
 - emlogデバイスを最大1MBまで大きくできるように。
 - 最近のカーネル/glibc向けの修正、mkemlogの修正。
 - ログデバイスの所有権を指定できるように。
 - e-infoごとのrwlockのサポートを追加し、デバッグ出力を強化。
   （厄介なreader対writerの競合状態を修正）
 - module_param経由でemlog_max_sizeを動的にサイズ変更できるように。

### Version 0.60 (September 25, 2016)
 - mkemlogユーティリティを追加。
 - Autofreeモジュールオプション（最後のcloseで関連バッファを解放）。
 - デフォルトで使用可能な/dev/emlogを作成（非ゼロサイズのバッファ付き）。
 - カーネル3.19以降のサポート。
 - カーネル2.6.20未満のサポートを終了。
 - 単なるprintk()の代わりにpr_err()などを使用するように。
 - Kbuildファイルの分離とmakefileの更新。
 - クリーンアップ: 型、static指定など。
 - READMEをMarkdown構文に変換。

### Version 0.52 (September 4, 2012)
 - miscデバイスの代わりにchar device regionに切り替え。
 - 2.6.x系と3.x系の両方のカーネルをサポート。
 - printk()に適切なログレベルを設定。
 - ソースコードのインデントを再調整（タブをスペースに変換）。

#### Andreas Neustifter <andreas.neustifter at gmail.com>による変更 (September 2, 2012)
 - 安定性の修正
 - モジュールのinitとremoveを書き直し

### Version 0.51 (August 31, 2011)
 - 3.0カーネルのサポート。
 - udevによる/dev/emlogの自動作成のためmiscデバイスに変更。

#### Andriy Stepanov <stanv at altlinux.ru>による変更 (August 31, 2011)
 - 3.0.3カーネルでのビルドを修正
 - udevによる/dev/emlogの自動登録

### Version 0.50 (year 2006?)
 - 2.6.xカーネルでコンパイル・動作するように更新。

#### Nicu Pavel <npavel at mini-box.com>による変更 (August 14, 2006)
 - MODULE_PARMマクロをmodule_param関数に置き換え

#### Nicu Pavel <npavelat mini-box.com>による変更 (June 12, 2006)
 - Darien版から2.6カーネル関数の更新を取り込み。
 - 2.6カーネル向けMakefile

#### Darien Kindlund <kindlund at mitre.org>による変更
 - Linux 2.6カーネルと互換性を持つようemlogのコードを修正。

### Version 0.40 (August 13, 2001)
 - 複数の読み込みと書き込みの同時実行が正しくサポートされるようになった
   （以前のバージョンのように、最初に読み込まれた時点でデータが消費されることはなくなった）。
 - 大きな連続した物理メモリブロックをロックすることを避けるため、emlogのリングバッファがkmallocではなくvmallocを使って確保されるようになった。
 - MODVERSIONSサポートを追加。
 - 'nbcat'ユーティリティを追加 -- catに似ているが、データの末尾でブロックしない。
 - バグ修正: デバイス番号とinode番号の両方が内部的に保存されるようになった（以前はinode番号のみ）。これにより、異なるファイルシステム上のemlogが1つのバッファを共有してしまう（可能性は低いものの）不具合を防止。

### Version 0.30 (March 1, 2001)
 - 2.4系カーネルで正しくコンパイルされるように。
 - select()とpoll()がemlogデバイスに対して正しく動作するように。
 - バグ修正: 全てのインスタンスが1つの待機キューを共有してはならない！

### Version 0.20 (June 14, 2000)
 - 最初の公開リリース。


emlogは誰が、なぜ書いたのか？
=========================

Emlogは、SCADDSプロジェクト<http://www.isi.edu/scadds>の一環として、南カリフォルニア大学情報科学研究所（USC/Information Sciences Institute）のJeremy Elson <jelson@circlemud.org>によって書かれました。SCADDSは組み込みシステムの研究プロジェクトです。私たちは、Linuxを使った小型のPC/104バスベースのシングルボードPCを使用しています。特定のプロセスからのデバッグ出力を保存したかったのですが、これらのマシンにはディスク容量16MB、RAM32MBしかなかったため、完全なログファイルを保持するという選択肢はありませんでした。とはいえ、これらの小さなノードにはPPPを実行するシリアルポートが付いているため、ノードのところまでラップトップを持って歩いて行き、シリアルケーブルを接続してtelnetでボックスに入ることは可能です。emlogを使うことで、私たちは常にプロセスからの最新のデバッグメッセージを保持できます。エラーが発生した場合には、デバッグ用のコンソールを接続して何が起きたかを確認できます。

この作業は、SCADDSプロジェクトの一環としてDARPAのグラントNo. DABT63-99-1-0011によって支援されており、また、Cisco Systemsからの支援によっても実現しています。
