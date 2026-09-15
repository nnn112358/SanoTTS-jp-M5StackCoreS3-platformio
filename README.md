# SanoTTS-jp + M5Avatar 

[sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) と [M5Stack-Avatar](https://github.com/meganetaaan/m5stack-avatar) を
M5Stack CoreS3 で組み合わせた PlatformIO プロジェクト。
漢字かな交じり文をそのまま端末側で G2P してニューラル TTS で喋り（ネットワーク不要）、
喋っている音量に合わせてアバターの口が動く。

- 起動時に 1 文喋り、以後は画面をタッチするたびに次の文を喋る（4 文をループ）。
- 喋っている文は吹き出しにも出る（日本語フォント `efontJA_16`）。
- 辞書が無い場合はかな中間表現（`きょ][おわよ][いて][んきです°ね`）だけを喋る。

### リップシンクの仕組み

`SanoTTSSpeakerM5` を継承した `LipSyncSpeaker` がスピーカーへ流す PCM を横取りし、
チャンク（2,048 sample = 93 ms）ごとの RMS を「再生開始からのサンプル位置」つきで記録する。
`loop()` が「今鳴っているチャンク」の値を拾って `avatar.setMouthOpenRatio()` に渡す。

`tts.say()` は喋り終わるまでブロックするので TTS は別タスク（core 0）で回し、
`loop()`（core 1）は `M5.update()` と口の更新だけをする。

口の開きが小さすぎる / 大きすぎるときは `src/main.cpp` の `kMouthRefRms` を調整する
（シリアルに出る `absmax` が目安）。

## 必要なもの

| 項目 | 内容 |
|---|---|
| 板 | M5Stack CoreS3（ESP32-S3 / 16 MB flash）。 |
| PC 側 | Linux、`uv`、USB-C ケーブル |
| PlatformIO | 公式 `espressif32` は arduino-esp32 2.0.17 で止まっており動かない。`platformio.ini` で [pioarduino](https://github.com/pioarduino/platform-espressif32)（3.x）を指定済み |

## ファイル構成

| ファイル | 役割 |
|---|---|
| `platformio.ini` | ビルド設定（pioarduino / board `m5stack-cores3` / 16 MB パーティション表） |
| `src/main.cpp` | アバター表示、タッチで `tts.say()`、PCM の RMS で口を動かす |
| `sanotts_16mb.csv` | パーティション表（ライブラリ `extras/partitions/` からコピー） |
| `k1-dict-44000-2mb.bin` | 漢字辞書（手順 5 で dict パーティションに書き込む） |

依存ライブラリ（`platformio.ini` の `lib_deps` でバージョン固定）:

| ライブラリ | バージョン | 用途 |
|---|---|---|
| sanoTTS-jp | latest release | TTS 本体 |
| sanoTTS-jp-voice-tsukuyomi-v4 | latest release | 音声の重み（⚠️ MIT ではない。`LICENSE-MODEL.md` を読むこと） |
| M5Unified | 0.2.22 | 画面・スピーカー・タッチ |
| M5GFX | 0.2.29 | M5Unified の描画バックエンド |
| M5Stack-Avatar | 0.10.0 | アバターの顔 |

パーティション表の要点:

| 名前 | オフセット | サイズ | 用途 |
|---|---|---|---|
| factory | 0x10000 | 0x2C0000 | アプリ（重みは `.rodata` に埋め込み） |
| dict | 0x2D0000 | 0xD30000 | 漢字辞書（別途書き込む） |

---

## 手順

### 1. PlatformIO CLI を入れる（初回のみ）

```sh
uv tool install platformio
pio --version
```

`pio` が見つからない場合は `~/.local/bin` を PATH に追加する。

### 2. 板をつなぐ

CoreS3 を USB-C で接続し、シリアルデバイスを確認する。

```sh
ls /dev/ttyACM*
```

`Permission denied` になる場合は `dialout` グループに追加して再ログインする。

```sh
sudo usermod -aG dialout $USER
```

`platformio.ini` の `upload_port` / `monitor_port` は `/dev/ttyACM0` 固定。別名なら書き換える。

### 3. ビルドする

```sh
cd SanoTTS-jp-M5StackCoreS3-platformio
pio run
```

初回はプラットフォーム、ライブラリ本体、重み、M5Unified の取得で数分かかる。
最後に `[SUCCESS]` と出れば OK。Flash 使用量は 16 MB 中およそ 1.8 MB、RAM は 320 KB 中およそ 230 KB。

### 4. ファームウェアを書き込む

```sh
pio run -t upload
```

`Hash of data verified.` → `Hard resetting via RTS pin...` と出れば完了。

### 5. 漢字辞書を書き込む（漢字を喋らせるなら必須）

漢字を含む文を喋らせるには辞書が要る。辞書が無いと `tts.kanjiReady()` が false になり、
かな中間表現でしか喋れない。

辞書は [リリース](https://github.com/ayutaz/sanoTTS-jp/releases) から取得し、
どれを使う場合も **0x2D0000** に書き込む。

| ファイル | サイズ | 語数 | 備考 |
|---|---|---|---|
| `k1-dict-44000-2mb.bin` | 0.98 MB | 44,000 | 小さいが読みの精度は落ちる |


このリポジトリには `k1-dict-44000-2mb.bin` を同梱してある。別の辞書を使うならリリースから落とす。

```sh
# 同梱していない辞書を使う場合のみ
curl -LO https://github.com/ayutaz/sanoTTS-jp/releases/download/v1.1.0/k1-dict-44000-2mb.bin

uv run --with esptool python -m esptool \
  --chip esp32s3 -p /dev/ttyACM0 write_flash 0x2D0000 k1-dict-44000-2mb.bin
```

`Hash of data verified.` と出れば完了。10 秒ほどで終わる。

辞書が焼けていない板では起動ログに次の行が出る。

```
E (1659) saan_dict: jdict_open: -1 — 先頭が K1D1 でない（焼いていない / 別物）
kanji: NOT ready (dict missing)
```

辞書はアプリとは別領域なので、この手順は一度だけでよい。
以後 `pio run -t upload` を繰り返しても消えない。
辞書を差し替えるときは同じコマンドでファイル名だけ変える。

### 6. 動作を確認する

書き込み後に板が自動でリセットし、アバターが表示されて
「こんにちは。私はスタックチャンです。」と喋る。画面をタッチすると次の文を喋る。

```sh
pio device monitor
```

起動ログに `kanji: ready` と出れば辞書が認識されている。
`NOT ready (dict missing)` なら手順 5 をやり直す。

喋るたびにシリアルへ文とサンプル数、ピーク値が出る。

```
kanji: ready
こんにちは。私はスタックチャンです。 (49152 samples, absmax 9928)
```

`absmax` は int16 のピーク値。`kMouthRefRms`（既定 2500）はこの値のおよそ 1/4 を目安にしている。

起動ログに `esp_core_dump_flash: No core dump partition found!` と出るが、
パーティション表に coredump 領域が無いだけの警告で動作に影響はない。

`pio device monitor` は対話端末が必要なので、スクリプトからログを取るときは pyserial で直接読む。

```sh
uv run --with pyserial python -c "
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)   # リセット
t0 = time.time()
while time.time() - t0 < 12:
    line = s.readline()
    if line: print(line.decode('utf-8', 'replace').rstrip())
"
```

---

## スケッチを変更したとき

手順 3 と 4 だけ繰り返す。辞書（手順 5）は再書き込み不要。

```sh
pio run -t upload
```


