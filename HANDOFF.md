# 引き継ぎメモ (HANDOFF)

> セッションをまたいで作業を継続するための正本。**未達成を暗黙にしない**。
> 各セッション終了時にここを更新する。会話言語は日本語。

最終更新: 2026-06-18（M0 完了 + 実機作業フェーズ追記 + **P-A 粗GO達成・IST PRO Report Map 採取済み**）
計画書: `/Users/tommy/.claude/plans/distributed-hatching-hejlsberg.md`
元ブリーフ: `/Users/tommy/Downloads/zmk-ble-hid-host-brief.md`

---

## 1. これは何か / 現在地

XIAO nRF52840 を「BLE トラックボール(Elecom IST PRO)を BLE central で受け、デコードして
ZMK の input subsystem に流す」モジュール。リマップ／出力は ZMK 本体に丸投げ（受信専用）。

- **現在のマイルストーン: M0 完了。次は M1。**
- ZMK 本体は fork しない。3層構成（zmk 無改変 + 本モジュール + ユーザ zmk-config）。
- **実機: ユーザ手元に XIAO nRF52840・Elecom IST PRO あり（焼き/USB/ペアリング等の実機作業は実施可能、2026-06-18 確認）。**

### 構成（確認済み・2026-06-18）

```
  Elecom IST PRO ──BLE(HOGP)──▶ XIAO nRF52840 ──USB(HID)──▶ PC
   (BLE マウス)                  ├ 本モジュール = BLE セントラル(ホスト)役
                                 └ ZMK 本体     = USB HID 出力役
```

- XIAO は二役: 「マウスに対して **BLE セントラル/ホスト**」かつ「PC に対して **USB HID デバイス**」。
- 「BT で IST PRO と XIAO を接続」という理解は**正しい**（その BT は BLE / HOGP）。
- 出力は USB なので機能上 BLE ペリフェラル(PC広告)は必須でないが、ZMK 本体が標準で広告を持つため
  実際は central+peripheral 共存（nRF52840 は同時対応可。ZMK split が既に両役で動作）。

## 2. 完了（M0）＝ 検証済み

- リポジトリ雛形（`driver` 型 Zephyr module）。ZMK 同梱の `app/module` 慣習に準拠。
  - `zephyr/module.yml` / ルート `CMakeLists.txt`・`Kconfig` / `drivers/{,input/}{CMakeLists,Kconfig}`
  - `drivers/input/ble_hid_host.c` … 仮想 input device 登録のみ（`input_mock.c` を手本）。BLE 無し。
  - `dts/bindings/input/zmk,ble-hid-host.yaml` … `compatible = "zmk,ble-hid-host"`、`device-name` 任意。
- `include/zmk_ble_hid_host/hid_report_parser.h` … **Zephyr 非依存の純粋デコード契約**
  （`zmk_hid_parse_report_map` / `zmk_hid_decode_report` と構造体）。実装は M2。
- `tests/parser/`（Makefile + test_runner.c）… ホスト単体テスト基盤。`cc` のみで動く。
  - **ローカルで `make -C tests/parser test` → green を確認済み。**
- CI: `.github/workflows/hosttest.yml`（push/PR でホストテスト実行）。
- `config-example/`（west.yml / overlay / keymap / conf スニペット）… 配線手順の雛形。
- `README.md`（アーキ・§8 実機採取手順・設計判断）/ `LICENSE`(MIT) / `.gitignore`。

> 検証の意味: M0 は「コードが正しい慣習で書かれ、ホストテストが green」まで。
> **ファームのビルドは未実施**（下記 §3）。

## 3. 未達成・未検証（暗黙にしない）

- [ ] **ファームの実ビルド未実施**。M0 の C はローカル ZMK の `input_mock.c` 慣習に忠実だが、
      `west build` も GH Actions ファームビルドも**まだ通していない**。→ M1 でファームビルド CI を立てて green にする。
- [ ] **実機検証ゼロ**（当方=Claude は実機を操作できない）。ただし**ユーザ手元に XIAO・IST PRO があり実機作業は実施可能**
      （2026-06-18 確認）。接続・採取・デコード精度・レイテンシ・bonding はユーザ実機で確認 → §6 実機作業フェーズ。
- [ ] **BLE 受信(M1)・デコード(M2)・publish(M3)・配線(M4) は未実装**。
- [ ] `config-example/zmk-config.conf.snippet` の Kconfig 一式は名前を Zephyr 4.1 で確認済みだが、
      **同時有効化でビルドが通るかは未検証**（M1 のファームビルドで確定する）。

## 4. 次にやること（M1 = 受信。最大の関門）

ブランチ `feat/m1-hog-central` を切って実装 → PR。DoD = ファームビルド CI green ＋ コードレビュー。

実装ファイル（新規）: `drivers/input/hog_central.c`（`drivers/input/CMakeLists.txt` のコメント済み行を有効化）。

手本（ローカル checkout、行番号は変わり得るので関数名で追う）:
- `…/zmk/app/src/split/bluetooth/central.c` … scan→connect→discover→subscribe の状態機械の正本。
  - scan コールバック / `bt_le_scan_start`、`bt_conn_le_create`、`BT_CONN_CB_DEFINE`
  - `bt_gatt_discover`(PRIMARY→CHARACTERISTIC→DESCRIPTOR) の段階遷移、`bt_gatt_subscribe`
  - コールバックは BT スタック文脈 → `k_work`/`k_msgq` で重い処理を遅延させる作法
- Zephyr GATT/conn API: `…/zmk/zephyr/include/zephyr/bluetooth/{gatt.h,conn.h,uuid.h}`

M1 のサブステップ:
1. `CONFIG_BT_CENTRAL` 等を `select` するか、conf 依存にするか方針決定（README/conf スニペット参照）。
   ZMK 本体の BT 管理と競合しないよう、まずは conf 依存（ユーザ zmk-config で有効化）で開始が安全。
2. scan → 対象(HID service あり or Appearance=mouse、`device-name` 一致)を見つけて connect。
3. GATT discovery: HID svc `0x1812` → Report `0x2A4D` / Report Map `0x2A4B` / Protocol Mode `0x2A4E` → CCCD `0x2902`。
4. 各 Report に subscribe。notify コールバックで**生バイト列を `LOG_HEXDUMP_DBG`** で出すだけ（採取用）。
5. bonding/再接続（`CONFIG_BT_SETTINGS`、フィルタアクセプトリスト or 直接再接続）。
6. 7.5ms 接続間隔要求: `bt_conn_le_param_update` + `BT_LE_CONN_PARAM(6,6,0,timeout)`。
7. **ファームビルド CI を追加**（`.github/workflows/build.yml`）: `config/west.yml`＋`build.yaml` を用意し、
   ZMK の reusable build workflow で `seeeduino_xiao_ble` をビルド。push→`gh run` でログ確認し green まで反復。
   - 注意: XIAO は board 単体ではキーボードにならないので、最小 shield か keymap/overlay が要る。
     ZMK module の CI 構成（cirque-input-module 等）を参照して詰める。self 参照 west.yml がコツ。

## 5. M2〜M5 概要

- **M2 デコード**: `hid_report_parser.c`（Report Map item 解析 → field 位置）＋ `hid_report_decode.c`
  （生レポート＋layout → dx/dy/buttons/wheel）。両方 `hid_report_parser.h` の契約に実装。
  `tests/parser/` に標準マウス/boot mouse/report-ID 付き/実機採取(§8) の fixture を足し、
  `Makefile` の `SRCS +=`（コメント済み）を有効化して assert。Boot Protocol フォールバックも。
- **M3 input device 化**: `ble_hid_host.c` のコメント「M3 publish contract」を実装。
  ボタンは前回マスクとの差分でエッジ送出、最後の event に `sync=true`。
- **M4 配線**: `config-example/` を実機で詰めて README に確定値。input-processors で軸反転/scaling/snipe/scroll。
- **M5 ドングル**: ケース・常用（物理作業）。

## 6. 実機作業フェーズ（👤 ユーザ担当 / XIAO・IST PRO 手元あり）

実機作業は実施可能（ユーザ確認済み 2026-06-18）。コード(🤖 Claude)と実機(👤 ユーザ)の対応:

- [x] **P-A-1 macOS ペアリング成功＝粗GO** ✅（2026-06-18）。専用ドングルなしで汎用ホストにペア＆ボンド＝ベンダロックでない。
- [x] **P-A-2 Report Map 採取済み**（`ioreg`、PacketLogger 不要）→ fixture
      `tests/parser/fixtures/ist_pro.report_map.hex` ＋ 解説 `…/ist_pro_report_descriptor.md`。
      ELECOM IST PRO, VID `0x056E`/PID `0x018A`。**標準 HOGP 確定**。
      ポインタ = **Report ID 2**：byte1=buttons(1-8), 2-3=X(int16 LE), 4-5=Y(int16 LE), 6=wheel(int8), 7=AC Pan(int8)、計8B。
      自動デコードで検証済み＝汎用パーサで自明に解ける（**設計 GO**）。手順書 `docs/device-capture-macos.md`。
- [ ] **P-A 残（PacketLogger 不要に。M1 で吸収）**: 実 BLE 通知の生バイト（report-ID の有無/実バイト順）は **M1 の `LOG_HEXDUMP`** で取得。
      **アドレス種別(RPA か)/再接続広告/NoInputNoOutput 受諾は macOS では確認不可 → 真のゲートは XIAO の M1 ファーム。**
      （passkey が macOS で出ても NO-GO ではない＝NIO 中央は Just Works に解決。）
- [ ] **P-B M1 動作確認**（🤖 が焼ける .uf2＋ファームビルド CI を用意した後） 👤
      .uf2 を XIAO に焼く（リセット2回でブートローダのドライブ表示 → .uf2 を D&D）→
      USB シリアルでログ確認 → IST PRO をペアリングモードに → 接続して**生レポートがログに出るか**。
- [ ] **P-C M3 動作確認** 👤  PC で実際にカーソルが動くか。
- [ ] **P-D M4 調整** 👤  リマップ(軸反転/swap/scaling/snipe/scroll)が効くか、processor パラメータ実調整。
- [ ] **P-E M5 常用** 👤  ケース装着・常用。

実機でしか確定しない項目: pairing 方式 / bond 永続化 / 7.5ms 接続間隔の受諾可否 / レイテンシ体感。

> **並行方針**: P-A 採取と 🤖M1 コード実装は**並行可能**。採取は M1 のブロッカーではなく M2 の入力。
> M1 ファーム自体が生レポートを log するので P-A 無しでも M1 受信確認はできるが、
> P-A を先にやると HOGP 互換の事前確認＋M2 テスト fixture が得られ手戻りが減る。

## 7. オープン項目 / 既知の不確定

- IST PRO の HID レポート構造は**非公開**（Linux `hid-elecom.c` にも IST PRO は無い）。実機採取で確定。
- ELECOM はボタン数を誤申告する descriptor quirk の前科あり（旧機種）。Report Map パース時に注意。
- ファームビルド CI の module 自己参照 west 構成は要詰め（M1-7）。

## 8. 設計判断（確定。蒸し返さない）

- HOGP は **素の Zephyr GATT で自作**（NCS `bt_hogp` は gatt_dm/NCS settings 依存で移植不可）。
- デコードは **実行時 Report-Map パーサ**を中核（任意 HOGP マウス対応）＋ Boot Protocol フォールバック。
- デコード中核は **Zephyr 非依存の純粋関数**（ホスト単体テスト可能にするため）。
- リポジトリ **Public**（`akira-toriyama/zmk-ble-hid-host`）。新規 repo への push は明示許可済み
  （「gh 使用OK／ブランチ作製OK」）。既存 repo の push 禁止ルールとは別扱い。
- pin: ZMK同梱 **Zephyr v4.1.0+zmk-fixes**。検証済み API:
  `input_report_rel(dev,code,value,sync,k_timeout_t)`、module.yml は `build.{cmake,kconfig,settings.dts_root}`。

## 9. ビルド/テスト早見

```sh
# ホスト単体テスト（実機/Zephyr 不要）
make -C tests/parser test          # → "all host tests passed"

# CI 状況
gh run list -R akira-toriyama/zmk-ble-hid-host
```

## 10. 参考パス（ローカル checkout）

- ZMK 本体: `/Volumes/workspace/github.com/zmkfirmware/zmk`
  - 入力 API: `zephyr/include/zephyr/input/input.h`
  - module 慣習の手本: `app/module/`（特に `drivers/input/{CMakeLists.txt,Kconfig,input_mock.c}`）
  - GATT central 手本: `app/src/split/bluetooth/central.c`
  - listener/processors: `app/dts/bindings/zmk,input-listener.yaml`、`app/dts/input/processors.dtsi`
  - module 作成正典: `docs/docs/development/module-creation.md`
  - 配線例: `docs/docs/development/hardware-integration/pointing.mdx`
