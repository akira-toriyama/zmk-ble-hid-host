# 引き継ぎメモ (HANDOFF)

> セッションをまたいで作業を継続するための正本。**未達成を暗黙にしない**。
> 各セッション終了時にここを更新する。会話言語は日本語。

最終更新: 2026-06-18（**M1 ✅ + M2 デコード核 ✅ + ファームビルド CI ✅ + hog_central 移植 ✅ + M3 publish 実装 ✅**。
このセッションで **M3 publish を実装**＝唯一の残りを解消: ① `hog_central.c` の discovery に **Report Reference
(0x2908) 読込→report-ID マッチング**を追加（DISC_REPORT_REF 新状態。各 report char の id を読み、**layout.report_id
と一致する pointer report だけ**を motion 化。keyboard/consumer report はログして捨てる）。② `ble_hid_host.c` に
**`ble_hid_host_publish()`** を実装（ボタン差分=`INPUT_BTN_0..4`、`INPUT_REL_X/Y/HWHEEL`、終端 `INPUT_REL_WHEEL`
を `sync=true` で flush）。③ `layout` を**バリア+volatile で atomic publish**（M2b の deferred 解消）。
**4観点の敵対レビュー実施→4 findings 全適用**: ④ disconnect で `k_msgq_purge`（前ペアの残レポートが次ペアの layout で
誤デコードされる窓を封鎖）、⑤ disconnect で `ble_hid_host_reset`（押下中ボタンが PC に latch するのを解除）、
⑥ ドロップしたボタン edge は `prev_buttons` を進めない（applied マスク）、⑦ 単一 report 機は id チェック bypass。
**default + logging 両 `.uf2` が Docker で green**（FLASH 24.4%/RAM 15.6%）。詳細 §M3。
**M3 完了 ✅ = P-C 実機実証（カーソルが動く, 2026-06-18, §6 P-C）。次は M4（リマップ）。** 設計の続きは §4。）
計画書: `/Users/tommy/.claude/plans/distributed-hatching-hejlsberg.md`
元ブリーフ: `/Users/tommy/Downloads/zmk-ble-hid-host-brief.md`

---

## 1. これは何か / 現在地

XIAO nRF52840 を「BLE トラックボール(Elecom IST PRO)を BLE central で受け、デコードして
ZMK の input subsystem に流す」モジュール。リマップ／出力は ZMK 本体に丸投げ（受信専用）。

- **現在のマイルストーン: M0 → M1 ✅（受信を実機実証, §M1）→ M2 デコード核 ✅（§M2）→ M2b ファームビルド CI +
  hog_central 移植 ✅（§M2b, P-B' 実機実証）→ M3 publish 実装 ✅（§M3, ビルド green）。
  → M3 publish ✅ P-C 実機実証（カーソルが動く, §6 P-C, 2026-06-18）。次は M4(リマップ)。最終像は §4。**
- ZMK 本体は fork しない。3層構成（zmk 無改変 + 本モジュール + ユーザ zmk-config）。
  - ユーザの既存 zmk-config = **`akira-toriyama/canon`**（Cyboard Imprint 分割キーボード）だが、本ドングルは別デバイス。
    **ドングル用 ZMK 設定は canon 以外の新規 public config リポに切り出す（2026-06-18 決定, §4 次セッション）**。
    動作安定後に canon 取り込みを検討。**今は canon を触らない。**
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

## §M1. 受信プローブ（実機進行中）— `probe/`

M1「受信だけ」を、**ZMK ではなく単体 Zephyr 診断アプリ**として `probe/` に実装。
xiao_ble 用に scan→接続→ボンド(Just Works/NIO)→全 HID input report を subscribe→
**USB CDC ACM に LOG_HEXDUMP**。HOGP コードは後で ZMK モジュールへ移植。

**ビルド（両方とも flashable .uf2 を生成・検証済み）**:
- ローカル(Docker、ユーザ要件): `cd probe && ./scripts/build.sh` → `probe/firmware/zephyr.uf2`。
  Zephyr v4.1.0 を import、**SDK 0.17.0 を volume にインストール**（image 同梱の 1.0.x は v4.1 非互換）。
  はまり所: compose は `$VAR` を食う→ビルド手順は `probe/scripts/docker-build.sh` に分離。volume は root 所有→`user: root`。
- CI: `.github/workflows/probe-build.yml`（action-zephyr-setup, sdk 0.17.0, `west build -b xiao_ble .`）。
  成果物 `probe-uf2`。CI と Docker は**バイト同一**（500224B, 977 blocks）。
- 焼く: XIAO リセット2回→ドライブ→`.uf2` D&D。ログ: `screen /dev/tty.usbmodem<XXXX> 115200`
  （プローブの tty は location 名 **`usbmodem21101`** 等。USB serial 名では出ない場合あり。
  プローブの USB は Product=`XIAO HOGP Probe`/Vendor=`ZEPHYR` で `ioreg`/`system_profiler` から特定可）。
- **Claude(別セッション含む)がこの Mac で採取する手順**: `screen` は対話的で不可。
  `cat /dev/cu.usbmodem21101 > /tmp/p.log 2>&1 & CPID=$!; sleep 15; kill $CPID` の background+kill で数秒キャプチャ
  →`sed -E 's/\x1b\[[0-9;]*m//g'` で ANSI 除去して読む（ポート open で起動時 DTR 待ち~10s が解除されログが流れる）。

**実機結果（2026-06-18, M1 達成 ✅）**:
- ✅ build/flash/boot/USB-log/BT有効/scan/connect/**bond/discovery/subscribe/HID 受信** 全通し（実機）。
  接続先は `CD:CF:BF:79:7E:00 (random)` name `IST PRO` appearance `0x03c2`(mouse)。
  HID svc handles 27..73、**notifiable report 5本**(value handle 41/49/56/60/64)に subscribe、
  **ボール移動で `HID report` の hexdump が 4500+ 件**流れた。`subscribed to 5 report(s)`。
- ✅ **7.5ms 接続間隔 受諾**（`conn params: interval=6 (7.50 ms) latency=44 timeout=2160 ms`）＝§6/§7 の懸案が解消。
- 採取した生バイト（7B、report-ID 無し）を全フィールド裏取りして fixture 化
  → `tests/parser/fixtures/ist_pro.live_reports.hex`（move/全ボタン1-8/縦wheel/横ACpan/idle）。
  **byte0=buttons(bit0=左), 1-2=dx int16LE, 3-4=dy int16LE, 5=wheel int8, 6=ACpan int8**（HANDOFF §6 の Report Map と一致）。

**ボンド失敗→成功の決定的知見（蒸し返さない）**:
- 真因は **`CONFIG_BT_SMP_SC_PAIR_ONLY` が Zephyr 既定 y** で、プローブが **LE Secure Connections 必須**に
  なっていたこと。**IST PRO は LE レガシー専用**（SMP Pairing Response = `io 0x03`(NoInputNoOutput),
  `auth_req 0x01`: **SC=0 / MITM=0**）なので、SC 必須要求を `status 0x03`(Authentication Requirements,
  = `bt_security_err 4`)で蹴っていた。macOS が繋がったのはレガシーにフォールバックできるから。
- **誤った遠回り（やらない）**: DisplayYesNo + 数値自動承認 + `L3`(MITM要求) は逆効果。NoInputNoOutput 相手に
  MITM を要求すると SMP が自分で `err 4` を出す（MITM 不能）。マウスに MITM は原理的に無理。
- **正しい直し**（`probe/` に投入済み・実機で bond 成功を確認）:
  ① `prj.conf`: **`CONFIG_BT_SMP_SC_PAIR_ONLY=n`**（レガシー許可＝本丸）＋ `CONFIG_LOG_BUFFER_SIZE=4096`
     ＋ `CONFIG_BT_SMP_LOG_LEVEL_DBG=y`（SMP の io/auth_req/status を可視化。bond 後は外してよい）。
  ② `main.c connected()`: `bt_conn_set_security(conn, **BT_SECURITY_L2**)`（MITM 要求しない＝Just Works）。
  ③ `main.c`: 認証コールバックは **`.cancel` のみ＝NoInputNoOutput**（passkey 系は付けない）。
  ④ ターゲット選別: `device_found`/`ad_parse_cb` で **HID service 広告だけでは match させない**
     （隣の **Imprint Dongle**=keyboard `0x03c1` が誤マッチして接続を奪っていた）。
     **appearance==mouse(0x03c2) か name に IST/ELECOM** のみ match。`saw` ログは match/HID/named のみ
     （無名 RPA の洪水で seen[] が溢れ dedup 崩壊＋ログバッファ溢れで SMP trace が消える対策）。

**M1 で確定した HOGP ホスト作法（M2/M3 の ZMK モジュール移植にそのまま効く）**:
- 接続後すぐ discovery しない。`connected`→`bt_conn_set_security(L2)`→**`security_changed` で初めて discovery**
  （HID read/CCC write は暗号化ゲート）。各 subscribe params は **個別 long-lived**（共有グローバル禁止）。
- レガシー Just Works を許可（`SC_PAIR_ONLY=n`）＋ NoInputNoOutput。ZMK 本体側 BT 設定との整合は M4 で要確認。

**実機フラッシュ手順（このセッションで確立・Claude が Mac から実施可）**:
- 1200bps タッチ（`stty -f <port> 1200` を open→close）は**当機では不安定**（成否まちまち）。
  **物理ダブルタップが確実**: `XIAO-SENSE` がマウント→`cp firmware/zephyr.uf2 /Volumes/XIAO-SENSE/CURRENT.UF2`。
  `cp` が `fcopyfile: Input/output error` を出しても**焼けている**（UF2 は最終ブロックで即リブートするため正常）。
  ドライブ出現を 0.5s ポーリングして即コピーする監視スクリプトで自動化した（`/tmp/flash_watch*.sh` 方式）。
- 焼き直すと CDC ポート名が変わる（`usbmodem21101`↔`21201`）。**幽霊ノードに注意**：片方は前ファームのバナー
  (`58a5874a446a`)をバッファした 0/46B の死にノード。`=== XIAO HOGP probe ===` と `LE SC enabled` が
  流れる方が live（このセッションでは `21101`）。採取は `cat /dev/cu.<live> > /tmp/x.log &` → 後で読む。

## §M2. デコード核（完了 ✅, 2026-06-18）— `feat/m2-decode`

M2 の中核＝**Zephyr 非依存の純粋デコード**を実装し、ホストテストで検証済み。実機焼き不要で正しさが確定する設計
（§8 方針）。実装ファイル（新規）:
- `drivers/input/hid_report_parser.c` … HID Report Map（USB HID 1.11 item ストリーム）を歩いて、**最初の
  マウス/ポインタ collection** の X/Y/Wheel/AC-Pan/Buttons のビット位置を割り出す汎用パーサ。
  Global/Local/Main item、Push/Pop、4byte 拡張 usage、Usage Min/Max レンジ vs 個別 usage リスト、
  per-slot usage 割当（report_count×report_size）、Report ID 切替でビットカーソル reset、
  Output/Feature は Input ビットを消費しない、Constant(padding) はビットを進める、を実装。
- `drivers/input/hid_report_decode.c` … layout＋生ペイロード → `{dx,dy,wheel,hwheel,buttons}`。
  LSB-first ビット抽出＋2の補数符号拡張。

**確定した重要な設計判断（M0 の契約コメントを上書き）**:
- **HOGP のレポートペイロードには report-ID バイトは付かない**（各レポートが個別 characteristic で届く＝
  M1 で実証済み、fixture ヘッダ参照）。よってデコーダは **report-ID の strip を一切しない**。`report` 引数 = フィールド
  ペイロードそのもの。`layout.report_id` は「どの Report characteristic の layout か」を示すメタ情報（M3 の
  `hog_central` が Report Reference 0x2908 で突き合わせる用）。`bit_offset` はペイロード先頭=最初のフィールド bit から。
  → 「長さで ID バイト有無を自動判別」は左クリック時に `byte0==report_id` で誤 strip するため**やらない**（レビュー指摘 #1）。
- 末尾の余剰バイト（モデル外フィールド）は無視。`len < need`（必要バイト未満）のみエラー。

**テスト**（`tests/parser/`, `make -C tests/parser test` → "all host tests passed"。CI は ubuntu/gcc `-Werror`）:
- 実機 Report Map（`ist_pro.report_map.hex`）→ layout 一致（report_id=2 / buttons off0 cnt8 / X off8 sz16 signed /
  Y off24 sz16 signed / Wheel off40 sz8 signed / AC-Pan off48 sz8 signed）。
- 実採取レポート16件（`ist_pro.live_reports.hex`、**コメントが正解表**）→ デコード結果が全件一致。
- 合成: 標準マウス（report-ID 付き・padding 付き）、boot マウス（3B・report-ID 無し）、
  「X/Y 両方無いと invalid」「複数ポインタ collection は最初を採る」を assert。
- Makefile: `SRCS +=` で両 .c を有効化。`-DFIXTURES_DIR` で cwd 非依存に fixture をロード。

**多エージェント敵対レビューで修正済み（12件）**: ① report-ID strip 廃止（上記）。② parser のビットカーソルを
`uint32_t` 化＋16bit に収まらない offset は記録しない（病的記述子の wraparound 防止）。③ ボタンは
`buttons.bit_size`（per-button 幅）でストライド。④ End Collection 不均衡（depth≠0）は invalid。⑤ 契約固定テスト追加。
（refute 8件＝long item / button cap / extract 境界などは元から対処済みと確認）。

**M2 で未達（次の本丸）**: `hog_central.c` 未作成（probe の HOGP ロジックをモジュール `drivers/input/` へ移植し、
この純粋デコードを呼ぶ配線）。**現状 `drivers/input/CMakeLists.txt` の parser/decoder/hog_central 行はまだコメントのまま**
（モジュール用ファームビルド CI が無く Zephyr ビルドで検証できないため、本 PR では純粋核＝ホスト検証済みのみに留めた）。
→ 次セッションで「ファームビルド CI（自己参照 west）→ CMake 有効化 → hog_central 移植 → M3 publish」。詳細 §4。

## §M2b. ファームビルド CI ＋ HOGP central 移植（完了 ✅, 2026-06-18）

**① モジュール用ファームビルド CI（M1-7 の最大関門）— GitHub Actions で green 実証**:
- このリポジトリは ZMK ビルドで**二役**: (a) zmk-config（`config/west.yml` を `west init -l config` が読む）、
  (b) Zephyr モジュール（`zephyr/module.yml` を reusable workflow が検出し `-DZMK_EXTRA_MODULES=<repo>` で注入）。
- 追加物: `config/west.yml`（zmkfirmware/zmk の `app/west.yml` を import、`self.path: config`。**probe/west.yml は
  生 Zephyr 用なので流用禁止**）／`build.yaml`（**リポジトリ root**。`build_matrix_path` 既定）／`.github/workflows/build.yml`
  （`zmkfirmware/zmk/.github/workflows/build-user-config.yml@main` を呼ぶだけ）／`zephyr/module.yml` に **`board_root: .`** 追加。
- **board は `xiao_ble/nrf52840/zmk`**（zmk@main の board variant＝`app/boards/seeed/xiao_ble/xiao_ble_zmk.dts`/`_defconfig`）。
  **素の `xiao_ble` は生 Zephyr 用（probe の値）で ZMK ビルドでは誤り**。手本＝`akira-toriyama/canon`（ユーザのキーボード
  config。`imprint_dongle` が同 board で green）。canon は ZMK に out-of-tree パッチを当てる自前 reusable を使うが、
  本モジュールはパッチ非依存なので**公式 reusable でOK**。
- 新規 shield `boards/shields/ble_hid_host_receiver/`: 物理キー無し dongle。`zmk,kscan-mock`（0 events、settings_reset と同手）で
  必須の `zmk,kscan` chosen を満たし、`zmk,ble-hid-host` ノード＋`zmk,input-listener` を実体化。`.conf`/`.keymap`/`.zmk.yml`/Kconfig.*。
- **ローカル検証**: probe と同じ Docker（`zmkfirmware/zmk-build-arm:stable`）で `west build -s zmk/app -b xiao_ble/nrf52840/zmk`。
  CI と Docker 両方で `.uf2` 産出。`make` 不要、ホストに gcc 無くても可。

**アーキ確定（USB 専用ドングル）— 最大リスク消滅**:
- ZMK_BLE は `select BT_SMP_SC_PAIR_ONLY`（zmk `app/Kconfig:151`）→ ZMK_BLE=y だと LE Secure 必須で **IST PRO（レガシー専用）と
  central bond 不能**。回避＝shield `.conf` で **`CONFIG_ZMK_BLE=n`＋`CONFIG_ZMK_USB=y`**（settings_reset が ZMK_BLE=n を実証済の手）。
- 結果、本モジュールが **BT スタックを自前所有**（`bt_enable`/`settings_load` を自分で呼ぶ＝probe と同じ）。実ビルドの `.config` で
  `# CONFIG_BT_SMP_SC_PAIR_ONLY is not set` / `# CONFIG_ZMK_BLE is not set` / `CONFIG_BT_CENTRAL=y` を確認。**蒸し返さない。**
- ユーザは PC に XIAO を2台接続（本トラックボール受信器＋canon キーボード dongle）。両方 USB・独立。**本機の最終配置も USB 出力**。
  → M4 の「自己完結 config か canon 統合か」は **自己完結（canon に触らない）で確定**。

**② `hog_central.c` 移植（probe → モジュール、§M1 作法を踏襲）**:
- `drivers/input/{hog_central.c,hog_central.h}` 新規。`ble_hid_host.c` init が `zmk_ble_hid_host_central_start(dev, device_name)` を呼ぶ。
- probe からほぼ逐語移植: scan→`device_found`（appearance==mouse か IST/ELECOM 名、または `device-name` 完全一致）→`bt_conn_le_create`
  (7.5ms)→`connected`→`bt_conn_set_security(L2)`→`security_changed` で discovery→PRIMARY HIDS→**Report Map(0x2A4B) read+`zmk_hid_parse_report_map`**
  →Report chrc(0x2A4D notifiable)→CCC(0x2902) per-report subscribe（各 `sub_params` は static 長寿命）。auth は `.cancel` のみ＝NIO Just Works。
- **新規追加**: Report Map を `bt_gatt_read`（read-long、512B バッファ）→ parse して `layout` 確定。`notify_cb` は BT RX 文脈なので
  `k_msgq`+`k_work` で退避し、work handler で `zmk_hid_decode_report`→**M2 はログのみ**（`report h=.. dx=.. dy=.. wheel=.. buttons=..`）。
- BT bring-up は device init（POST_KERNEL）から `k_work_delayable` で 500ms 遅延（flash/settings 準備待ち）。`CONFIG_BT_CENTRAL` でガード
  （無効時は no-op stub＝INPUT のみビルドもリンク可）。
- **敵対レビュー（4観点+triage）反映**: ① disconnect で `layout_valid`+`rm_len` リセット（再接続で stale layout デコード防止＝唯一の実バグ）。
  ② report-map discover 同期エラーで `disc_state=IDLE`。③ Report Map 512B 超過で LOG_WRN。④ layout の cross-workqueue アクセスを
  コメント明記（M2 良性、M3 publish 前に atomic 化）。「無限ループ/overread」指摘は zephyr gatt.c 照合で **false positive 確定**。

**検証用 logging variant**: `build.yaml` に `-DCONFIG_ZMK_USB_LOGGING=y`（artifact `..._logging`）を追加。`CONFIG_USB_CDC_ACM=y` が入り、
ユーザが **`cat /dev/cu.usbmodem<XXX>`** で connect/bond/discover/decode のログを M1 と同様に採取できる（M3 前の実機検証用）。

**現状の到達**: モジュールが「BLE 受信→Report Map 解析→デコード→**ログ**」する `.uf2` が CI/Docker で green、**かつ実機実証済み
（P-B' ✅, §6）**: 焼いて IST PRO と bond→320B Report Map 解析→ボール移動で `dx/dy` がデコードされログに追従。**カーソルはまだ動かない**
（M2＝ログのみ、publish は M3）。残りは §4 の M3 のみ＝この `dx/dy` を `input_report_*` に流すだけ。→ **§M3 で実装済み。**

## §M3. publish 実装（完了 ✅, 2026-06-18）— デコード→`input_report_*`＝カーソルが動く

「decode→**ログ**」だった work handler を「decode→**publish**」に拡張。これで USB HID 経由で PC のカーソルが動く（はず＝P-C 実機待ち）。
触ったのは3ファイル（`drivers/input/{ble_hid_host.c,hog_central.c,hog_central.h}`）。**ZMK 本体は無改変。**

**① Report Reference (0x2908) → report-ID マッチング**（`hog_central.c`）:
- discovery state machine に **`DISC_REPORT_REF`** を追加。各 report char ごとに「**0x2908 を discover→read で report-ID 取得**→
  **0x2902(CCC) を discover→subscribe**」の順に拡張（旧 CCC ロジックは `discover_ccc()` に分離。`subscribe_pending()` が 0x2908 から開始）。
- subscribe params を **`struct hid_subscription { bt_gatt_subscribe_params params; uint8_t report_id; }`** 化（`subs[]`）。
  `notify_cb` は `CONTAINER_OF(params, struct hid_subscription, params)` で report-ID を取り、`report_evt.report_id` に載せる。
- work handler は **`evt.report_id == layout.report_id` の report だけ** publish（他はログして捨てる）。**例外**: 通知 report が1本だけなら
  曖昧さが無いので id チェックを bypass（単一 report 機が 0x2908 read 失敗でも動く＝サイレント死防止、後述 review 適用 ⑦）。
- descriptor 探索窓は **`cur_report_end_handle()`**＝次 report の char 宣言の手前まで（0x2908/0x2902 を持たない report が隣の物を拾わない）。
- 0x2908 が**無い/読めない**場合は id=0 にフォールバック（`!attr` 分岐 or read err 分岐から `discover_ccc()`）。

**② publish 本体**（`ble_hid_host.c` `ble_hid_host_publish()`）:
- **ボタン**: `r->buttons ^ prev_buttons` の変化ビット `i`(0..4) ごとに `input_report_key(dev, INPUT_BTN_0+i, 押下, false, K_NO_WAIT)`。
  **`INPUT_BTN_0`(0x100) を使う**（ZMK の `input_listener.c` が消費するのは `INPUT_BTN_0..4`。`INPUT_BTN_LEFT`(0x110) ではない）。
  **bit0=左**→`INPUT_BTN_0`→mouse button0(左)。ZMK mouse HID は5ボタンなので **5個で cap**（`BLE_HID_HOST_PUBLISH_BTNS=5`）。
- **移動**: dx/dy/hwheel が非0なら `input_report_rel(dev, INPUT_REL_X/Y/HWHEEL, 値, false, K_NO_WAIT)`。
- **終端 sync**: `input_report_rel(dev, INPUT_REL_WHEEL, r->wheel, true, K_NO_WAIT)`（wheel==0 でも sync=true で listener が1回 flush）。
  listener は `evt->sync` で HID 更新を処理（`input_listener.c`）。`CONFIG_INPUT_MODE_THREAD=y`＝input_report は queue 投入（16深）→
  別スレッドで処理。だから **system workqueue から K_NO_WAIT 呼び出しはブロックしない**（満杯時は drop=-errno）。

**③ layout の atomic publish**（M2b deferred 解消）: `layout_valid` を **`volatile`** 化。writer(report_map_read_cb)は
`layout` 全書込→`barrier_dmem_fence_full()`→flag=true。reader(work handler)は flag 確認→`barrier_dmem_fence_full()`→`layout` 読込。
単一コア nRF52840 で half-written layout 由来の delta 注入を排除。

**4観点の敵対レビュー（10エージェント, GATT状態機械/並行性/publish意味論/edge）→ 4 findings 全適用**:
- ④【medium】disconnect で **`k_msgq_purge(&report_msgq)`**: 前ペアの残レポートが次ペアの layout で誤デコードされる窓を封鎖
  （report-id guard だけでは **同 id の2機**を取りこぼす。BT_MAX_PAIRED=2）。disconnect も notify_cb も BT RX 文脈なので race-free。
- ⑤【medium】disconnect で **`ble_hid_host_reset(host_dev)`**: 押下中ボタンの release を送って PC の latch を解除（圏外 drag 対策）。
  最後の release を sync=true で flush。`prev_buttons` も 0 クリア（次ペアを clean slate に）。
- ⑥【low】publish のボタンは **`input_report_key` が成功(==0)した bit だけ `prev_buttons` を進める**（applied マスク）。
  満杯 drop された edge を次レポートで再送＝取りこぼし press の latch を防ぐ。**式は `(prev & ~applied)|(buttons & applied)`**
  （review 提案の `~changed` は release-drop で誤るので訂正済み）。
- ⑦【low】上記①の「単一 report は id チェック bypass」＝0x2908 read 失敗時のサイレント死防止。
- refute された懸案: happy path（IST PRO report_id=2, 0x2908 read 成功）は全 finding の影響を受けない＝**カーソルは動く**。

**ビルド**: default `zmk.uf2` 393728B（FLASH 24.39%/RAM 15.56%）、logging variant 466944B、両方 Docker で green
（`validate-both.sh`）。`nm` で `ble_hid_host_publish`/`ble_hid_host_reset`/`k_msgq_purge` link 確認。**ホスト純粋核テストは無改変で
そのまま（M2 の parser/decoder は変更なし）。**

**P-C 実機実証 ✅（2026-06-18）**: logging variant を焼き、IST PRO 自動再接続（ボンド永続）→ `report map parsed report_id=2`→
5本 subscribe（各 0x2908 から id=1/2/6/4/9 取得）→ボール移動で `report h=49 dx/dy` 1432件・左/右ボタン。**publish 全件 h=49(id=2)のみ
＝id フィルタ実機実証**。tommy さん「動作OK」＝カーソルが動いた。→ **M3 完了。次は M4。**（production 用に default `.uf2` を焼くのは任意＝
logging variant も HID mouse + CDC 両方持つので機能上は同じ。default は CDC 無しで軽い。）

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

- [x] **モジュール用ファームの実ビルド ✅完了**（§M2b）。自己参照 west の ZMK ファーム（app=zmk+config+module）が
      **GitHub Actions で green**＋ローカル Docker でも `.uf2` 産出。board=`xiao_ble/nrf52840/zmk`。全 Zephyr 側コードの検証ゲート確立。
- [x] **BLE 受信(M1) ✅実機実証**（§M1）／ **デコード(M2核) ✅ホスト検証**（§M2）／ **`hog_central.c` 移植 ✅ビルド green**（§M2b）。
      `drivers/input/CMakeLists.txt` の parser/decoder/hog_central は**全て有効化済み**。
- [x] **M3 publish ✅実装＋ビルド green**（decode→`input_report_*`, §M3）。Report Reference(0x2908) id マッチング＋
      `ble_hid_host_publish`＋layout atomic 化＋敵対レビュー4件適用。default/logging 両 `.uf2` Docker green。**実機(P-C)は未。**
- [ ] **実機検証(P-C)＝唯一の残り**: M3 でカーソルが動くか（§6 P-C）。先に logging variant でログ確認推奨（§4 手順）。
      XIAO・IST PRO は tommy さん手元にあり実機作業可（2026-06-18 確認）。M2 受信/デコードは P-B' で実機実証済み。
- [x] `.conf` の Kconfig 同時有効化でビルドが通ることを**確認済み**（§M2b の実ビルド `.config`）。

## 4. 次にやること

### 🎯 ユーザの最終像（2026-06-18 確認）— これに向けて進める
```
① ドングルとペアリング状態で「通常操作」可能（カーソルが動く）       ✅完了（M3, P-C 実機実証）
② ZMK でリマップ：例「マウスのボタン4 → キー A」                       ← M4（次セッション。別 config リポで）
③ ZMK の設定を育てる（継続）                                          ← M4+ 別 config リポを育成
④ 汎用 BLE-HID ブリッジ：一般 BT キーボード/マウスもカバー            ← M6（north star 拡張, §ロードマップ）
```
- **① ✅達成**（§M3, §6 P-C）。マウスは「任意の HOGP マウス」を実行時 Report Map 解析で受けられる設計（IST PRO で実証）。
- **②（次セッション）**: ZMK で **マウスのボタンをキーボードキーに化かす**のは入力系とキーマップが別サブシステムなので自明でない。
  着手前に **zmk docs / claude-code-guide で「input event(BTN) → keymap behavior」の現行作法を裏取り**すること
  （移動・スクロール・軸反転/scaling は input-processors で素直に可能）。実装は **別 config リポ**で（下記 §次セッション）。
- **③の置き場所＝決定（2026-06-18）**: ZMK 設定（厳密にはマウス）は **canon 以外の専用 config リポに切り出す**（新規 public, push 可）。
  動作が安定したら canon 取り込みを検討。canon は当面触らない（§1 方針）。詳細は §次セッション。

### ▶ 次セッション = M4（別 config リポ作成 → マウスボタン4 → `a` リマップ）

M1〜M3 完了（カーソルが動く, P-C ✅）。次は **② リマップを実機で出す**。ユーザのゴール例＝**「マウスのボタン4 → キー `a`」**。

**Step 1. 別 config リポを作る（決定済・push 認可済 / public OK）**:
- **canon 以外の新規 public リポ**に ZMK 設定（マウス）を切り出す。動作安定後に canon 取り込みを検討（今は canon を触らない）。
- 名前案: `zmk-ble-hid-host-config` か `zmk-trackball-dongle` 等（着手時にユーザへ最終確認）。中身＝ZMK user-config:
  `config/west.yml`（zmk 本体 import ＋ **`akira-toriyama/zmk-ble-hid-host` を module として pin**）、`build.yaml`、
  自分の overlay/keymap（receiver shield を流用 or 取り込み）。**今 `zmk-ble-hid-host` 内にある receiver shield/build.yaml が雛形**。
- ビルドは新 config リポの CI（公式 reusable `build-user-config.yml`）で。board=`xiao_ble/nrf52840/zmk`、`.conf` は §M2b と同値
  （`ZMK_BLE=n`/`ZMK_USB=y`/`ZMK_POINTING=y`/`BT_CENTRAL=y`/`BT_SMP_SC_PAIR_ONLY=n`…）。
- **モジュール repo（`zmk-ble-hid-host`）はライブラリとして残す**。受信器 shield をモジュール内に残すか config 側へ移すかは着手時判断。

**Step 2.「マウスボタン4 → `a`」の feasibility を先に裏取り**（⚠️ 自明でない）:
- マウスボタンは **input subsystem（`INPUT_BTN_0..4`）**、キーボードキーは **keymap/behavior** で**別サブシステム**。
  「BTN イベント → キーコード `a` 出力」の現行 ZMK 作法を **zmk docs / claude-code-guide で先に調査**（カスタム
  input-processor か、behavior か、`zmk,input-listener` の機構か）。移動/スクロール/軸反転/scaling/snipe は input-processors で素直。
- 本モジュール側で「特定ボタンを publish しない（keymap に回す）」等の分岐が要るかも要検討。まず ZMK 標準機構で出来るか確認。

**Step 3. 実機**: 新 config リポでビルド→`.uf2` を XIAO に焼く（§M1 焼き手順）→ボタン4で `a` が入力されるか確認。

参考: `input_report_rel/key` 署名 `(dev, code, value, sync, k_timeout_t)`。input コード: `INPUT_REL_X`=0x00 `Y`=0x01 `HWHEEL`=0x06
`WHEEL`=0x08、`INPUT_BTN_0`=0x100。ローカル両ビルド: `docker run --rm -v <repo>:/repo:ro -v /Volumes/workspace/.zmk-blehh-build:/ws -w /ws zmkfirmware/zmk-build-arm:stable bash /ws/validate-both.sh`。

### ▶ ロードマップ（拡張・将来）— 2026-06-18 ユーザ確認

**M6（north star 拡張）= 汎用 BLE-HID ブリッジ：一般 BT キーボード/マウスをカバー**:
- 現状の達成度（正直に）: **ハード非依存 ✅**（nRF52840 系なら可）／**マウス全般 ✅**（実行時 Report Map 解析で任意 HOGP マウス、IST PRO 実証）／
  **BLE central の土台（scan/接続/bond/discovery/subscribe）は HID 汎用で完成・キーボードにもそのまま効く ✅**（M1 で Imprint キーボードに
  誤接続したのが証拠）。**残り＝キーボードの「キー解釈＋出力」**（マウスと同規模の追加）:
  ① `hid_report_parser.c` にキーボード collection（modifier + keycode 配列）抽出を追加、② キーボード decode、
  ③ **出力経路**＝キーは ZMK の input subsystem 経路ではない。**当面はパススルー**（受けた HID キーボードレポートを USB HID キーボードに
  そのまま出す）が現実的。キーのリマップは別問題（kscan/keymap 経路で自明でない）。→ M6 として別途スコープ。
- DoD（M6）: 手持ちの BT キーボードを繋いで USB でタイプできる（まずパススルー）。

**ペアリング方針（cross-cutting, 2026-06-18 決定＝推奨案 C）**: **「pairing モードにしたら自動ペア」を採用、ただしスコープ付き**:
- **既知ボンドは常に自動再接続**（現状どおり。電源 ON で勝手に繋がる）。
- **新規ペアは「トリガ後の pairing ウィンドウ」内だけ受け付ける**（例: 起動時ボタン/キー長押し→N 秒だけ新規 scan-to-pair）。
  これで「ペアリングモードにしたら繋がる」UX を保ちつつ、**隣の Cyboard Imprint キーボードを巻き込まない**（M1 の誤マッチ＝
  appearance 0x03c1 を掴んだ問題の再来を防ぐ）。device-name 許可リストは補助。
- **未実装＝将来作業**: 現状は「マウス appearance か既知ボンドを無条件 auto-connect・接続中は他を無視・forget/unpair UX 無し」。
  M6 でキーボードも match させる時に、このペアリングウィンドウ機構が**必須**（さもないと Imprint を奪う）。
- 関連: `BT_MAX_PAIRED=2`/`BT_MAX_CONN=1`。デバイス切替は当面「繋ぎたい方だけ電源 ON」or device-name 固定＋再フラッシュ。

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

## 5. マイルストーン概要（M2〜M6）

- **M2 デコード ✅**: `hid_report_parser.c`（Report Map 解析→field 位置）＋ `hid_report_decode.c`（生レポート＋layout→dx/dy/buttons/wheel）。
  ホスト単体テスト green（§M2）。
- **M3 publish ✅**: `ble_hid_host.c` の publish 実装＋Report Reference(0x2908) report-ID マッチング＋layout atomic 化。実機 P-C ✅（§M3, §6）。
- **M4 リマップ（次セッション）**: **別 config リポ**作成（canon 以外, public）＋ input-processors（軸反転/scaling/snipe/scroll）＋
  **マウスボタン4 → キー `a`**。BTN→keymap behavior の作法は着手前に裏取り（§4 次セッション）。
- **M5 ドングル**: ケース・常用（物理作業）。
- **M6 汎用 BLE-HID ブリッジ（north star 拡張）**: 一般 BT キーボード/マウス対応。central 土台は完成・残りはキーボードの
  キー解釈＋出力（まずパススルー）＋ペアリングウィンドウ機構（§4 ロードマップ）。

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
- [x] **P-B M1 動作確認 ✅（2026-06-18）**。物理ダブルタップで `probe/firmware/zephyr.uf2` を焼き、
      `cat /dev/cu.<live>` でログ採取。IST PRO をペアリングモードに → **bond→subscribe→生レポート 5742件**確認。
      決め手はレガシー許可（§M1）。7.5ms 接続間隔も受諾。fixture = `tests/parser/fixtures/ist_pro.live_reports.hex`。
- [x] **P-B' M2 動作確認 ✅（2026-06-18）= モジュール側パイプライン実機実証**。logging variant `.uf2`
      (`ble_hid_host_receiver-logging`) を XIAO に焼き、`cat /dev/cu.usbmodem21101` で採取。IST PRO をペアリングモードに →
      **`connected`→`secured (level 2)`→`paired (bonded=1)`→`report map parsed (320 B): report_id=2 buttons=8`→
      `subscribed to 5 report(s)`→ボール移動で `report h=49 dx=.. dy=..` が動きに追従**。**=レガシー JW bond / ランタイム
      Report Map 解析(read-long 320B)/ デコードが全部実機で正しいと確定**（M3 の手戻りリスク消滅）。
      軽微: subscribe 中に `bt_conn: Not enough buffer space for L2CAP data` が2回（自己回復、5本 subscribe 成功。M3 で
      負荷増えたら `CONFIG_BT_L2CAP_TX_BUF_COUNT`/`BT_BUF_ACL_*` 増を検討）。pointer は report_id=2＝value handle 49。
- [x] **P-C M3 動作確認 ✅（2026-06-18）= カーソルが動く**。logging variant `.uf2`（`build-log/zephyr/zmk.uf2`）を XIAO に焼き、
      IST PRO **自動再接続**（ボンド永続）→ `report map parsed report_id=2`→**5本 subscribe（各 0x2908 から id=1/2/6/4/9 を正しく取得）**
      →ボール移動で `report h=49 dx/dy` 1432件、左ボタン `0x0001`×107・右 `0x0002`×2。**publish は全件 h=49(id=2)のみ＝id フィルタ実機実証
      （非ポインタ report のゴミ移動ゼロ）**。tommy さん「動作OK」＝**PC カーソルが実際に動いた**。→ ①「カーソルが動く」完了。次は M4。
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

# CI 状況（host-tests / probe-build / "Build ZMK firmware"）
gh run list -R akira-toriyama/zmk-ble-hid-host

# ファームをローカル Docker でビルド（CI と同等、push 不要で検証可）
#   WS=任意の scratch（repo を汚さない）。初回 build-in-container.sh が
#   west init -l config → west update → west build。以後は incremental。
WS=/Volumes/workspace/.zmk-blehh-build      # このセッションで使った場所
docker run --rm -v <repo>:/repo:ro -v "$WS":/ws -w /ws \
  zmkfirmware/zmk-build-arm:stable bash /ws/rebuild-in-container.sh
#   → /ws/build/zephyr/zmk.uf2（fresh build dir は `west zephyr-export` を先に）
#   board は xiao_ble/nrf52840/zmk、SHIELD=ble_hid_host_receiver。
```

## 10. 参考パス（ローカル checkout）

- ZMK 本体: `/Volumes/workspace/github.com/zmkfirmware/zmk`
  - 入力 API: `zephyr/include/zephyr/input/input.h`
  - module 慣習の手本: `app/module/`（特に `drivers/input/{CMakeLists.txt,Kconfig,input_mock.c}`）
  - GATT central 手本: `app/src/split/bluetooth/central.c`
  - listener/processors: `app/dts/bindings/zmk,input-listener.yaml`、`app/dts/input/processors.dtsi`
  - module 作成正典: `docs/docs/development/module-creation.md`
  - 配線例: `docs/docs/development/hardware-integration/pointing.mdx`
