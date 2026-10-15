# HW-Lab3 Option 1 個人報告：用 GATTLIB 寫 BLE Central（my_write.c）

## 1. 目標與結果

我在 Raspberry Pi 3（Linux）上用 C 和 [GATTLIB](https://github.com/labapart/gattlib) 寫了一支 BLE central 程式 `my_write.c`。執行一次就會完成掃描、配對、連線，把字串 `"hello, I am Esther!"`（19 bytes）寫進 iPhone 的 characteristic，再讀回來確認內容一致。

- **Peripheral**：iPhone 上 LightBlue 的虛擬裝置 **Blank**
  - service：`0x1111`
  - characteristic：`b3fcaed7-8598-419f-89f9-ebf46229fc7c`（READ / WRITE / NOTIFY / INDICATE）
- **結果**：寫入和讀回都成功，btmon 錄到的封包和 LightBlue 顯示的值都對得上。
- **程式碼**：`gattlib/my_write/my_write.c`、`gattlib/my_write/Makefile`

## 2. 環境與 GATTLIB 安裝

| 項目 | 版本 |
|---|---|
| 硬體 | Raspberry Pi 3 Model B Rev 1.2 |
| OS | Raspberry Pi OS（Debian 13 trixie），kernel 6.18 |
| BlueZ | 5.82 |
| GATTLIB | commit `1580056`（2024-07-17），D-Bus backend |
| Peripheral | iPhone + LightBlue |

```bash
sudo apt install git cmake build-essential pkg-config \
    libbluetooth-dev libglib2.0-dev libreadline-dev libpcre2-dev
git clone --depth 1 https://github.com/labapart/gattlib.git gattlib-src
cd gattlib-src
grep -rl "REQUIRED libpcre)" examples | xargs sed -i "s/REQUIRED libpcre)/REQUIRED libpcre2-8)/"
mkdir build && cd build
cmake .. -DGATTLIB_LOG_BACKEND=printf -DGATTLIB_LOG_LEVEL=1
make -j2 && sudo make install && sudo ldconfig
```

Debian 13 已經移除 PCRE1（`libpcre`），官方範例的 CMake 會因此找不到套件。範例程式其實沒有用到 PCRE，所以改連 `libpcre2-8` 就能編譯。安裝後，用 `pkg-config --cflags --libs gattlib` 就能找到 `gattlib.h` 和 `libgattlib.so`。

## 3. 程式設計

### 3.1 流程

```
main()
 ├─ g_bus_get_sync()                       連上 system D-Bus
 ├─ register_agent()                       註冊配對 agent（org.bluez.Agent1）
 ├─ forget_phone()                         刪掉舊的配對紀錄（-k 可略過）
 └─ gattlib_mainloop(ble_task)             啟動 GLib 事件迴圈，在另一個 thread 跑 ble_task
      └─ ble_task()
           ├─ gattlib_adapter_open()                 開啟 hci0
           ├─ gattlib_adapter_scan_enable()          掃描最多 15 秒
           │     └─ on_device_found()                名稱是 "Blank" 就記下位址，並呼叫
           │                                         gattlib_adapter_scan_disable() 提早停止
           ├─ find_device_paths()                    透過 D-Bus 找出對應的 BlueZ 物件
           ├─ gattlib_connect()                      非同步連線，失敗最多重試 3 次
           │     └─ on_connected()
           │          ├─ gattlib_write_char_by_uuid()      寫入文字（每秒重試，最多 30 秒）
           │          ├─ gattlib_read_char_by_uuid()       讀回確認
           │          ├─ gattlib_characteristic_free_value()
           │          └─ (-i) gattlib_register_indication() + gattlib_indication_start()
           ├─ gattlib_disconnect()
           └─ gattlib_adapter_close()
```

`gattlib_connect()` 是非同步的，連線結果會在另一個 thread 的 callback（`on_connected`）裡回報。所以 `ble_task` 用 `pthread_mutex` 加 `pthread_cond` 等 callback 做完，再決定要結束還是重試。

### 3.2 用到的 API

| 類別 | API | 作用 |
|---|---|---|
| GATTLIB | `gattlib_mainloop` | 跑 GLib 主迴圈，接收 BlueZ 的 D-Bus 事件 |
| GATTLIB | `gattlib_adapter_open` / `gattlib_adapter_close` | 開啟 / 關閉藍牙 adapter |
| GATTLIB | `gattlib_adapter_scan_enable` / `gattlib_adapter_scan_disable` | 開始 / 停止 BLE 掃描 |
| GATTLIB | `gattlib_connect` / `gattlib_disconnect` | 建立 / 中斷連線 |
| GATTLIB | `gattlib_string_to_uuid` | 把 UUID 字串轉成 `uuid_t` |
| GATTLIB | `gattlib_write_char_by_uuid` | 寫入 characteristic 的值 |
| GATTLIB | `gattlib_read_char_by_uuid` / `gattlib_characteristic_free_value` | 讀取值並釋放 buffer |
| GATTLIB | `gattlib_register_indication` / `gattlib_indication_start` | 訂閱 characteristic（由 BlueZ 寫 CCCD） |
| GDBus | `g_dbus_connection_register_object` | 匯出配對 agent 物件 |
| GDBus | `AgentManager1.RegisterAgent` / `RequestDefaultAgent` | 把 agent 註冊給 BlueZ |
| GDBus | `ObjectManager.GetManagedObjects` | 列出 BlueZ 的裝置物件 |
| GDBus | `Adapter1.RemoveDevice` | 刪除舊的配對紀錄 |

### 3.3 參數

| 參數 | 說明 |
|---|---|
| `-c uuid` | 要寫入的 characteristic（需要有 WRITE 屬性） |
| `-t text` | 要寫入的文字 |
| `-n name` | 要找的廣播名稱，預設 `Blank` |
| `-i seconds` | 寫完後訂閱這個 characteristic，並持續接收指定的秒數 |
| `-k` | 保留舊的配對紀錄（預設會先刪掉） |

## 4. 編譯與執行

`Makefile`：

```make
CFLAGS  += -Wall -Wextra -O2 $(shell pkg-config --cflags gattlib gio-2.0)
LDLIBS  += $(shell pkg-config --libs gattlib gio-2.0) -lpthread

my_write: my_write.c
```

執行步驟：

1. iPhone 打開 LightBlue，進入 Blank，在 characteristic `b3fcaed7…fc7c` 開啟 Write 屬性，並讓畫面停在前景。
2. 在 Pi 上執行：

```bash
cd ~/my_write
make                       # -Wall -Wextra 沒有任何警告
sudo ./my_write -c b3fcaed7-8598-419f-89f9-ebf46229fc7c -t "hello, I am Esther!"
```

3. 出現 `Pairing: confirm passkey ...` 時，在 iPhone 跳出的配對視窗按「配對」。

## 5. 執行結果

### 5.1 程式輸出

```
Pairing agent registered
Scanning up to 15 s for 'Blank' ...
Found 'Blank' at 5E:FA:D6:AD:CC:FA
Pairing: confirm passkey 541021 -> yes (tap Pair on the phone)
Connected to 5E:FA:D6:AD:CC:FA
Wrote 19 bytes: 68656c6c6f2c204920616d2045737468657221  "hello, I am Esther!"
Read back 19 bytes: 68656c6c6f2c204920616d2045737468657221  "hello, I am Esther!"
Disconnected
```

在 iPhone 的 LightBlue 中，把該 characteristic 的顯示格式切成 UTF-8，值會顯示 `hello, I am Esther!`。

### 5.2 btmon 封包

```
< ATT: Prepare Write Request (0x16) len 23
        Handle: 0x003d Type: Vendor specific (b3fcaed7-8598-419f-89f9-ebf46229fc7c)
        Offset: 0x0000
          Data[19]: 68656c6c6f2c204920616d2045737468657221
> ATT: Prepare Write Response (0x17) len 23
        Handle: 0x003d
          Data[19]: 68656c6c6f2c204920616d2045737468657221
< ATT: Execute Write Request (0x18) len 1
...
> ATT: Read Response (0x0b) len 19
        Value[19]: 68656c6c6f2c204920616d2045737468657221
```

GATTLIB 的 `gattlib_write_char_by_uuid()` 會呼叫 BlueZ 的 D-Bus `WriteValue`。因為這個 characteristic 帶有 Extended Properties（descriptor `0x2900`），BlueZ 用的是可靠寫入：先送 Prepare Write（0x16）、對方回傳同樣的資料讓 BlueZ 檢查，再送 Execute Write（0x18）。Python 的 bluepy 寫同一個 handle 時，用的則是一般的 Write Request（0x12）。

## 6. 遇到的問題與解法

| # | 現象 | 原因 | 解法 |
|---|---|---|---|
| 1 | 連上 iPhone 後 D-Bus timeout，或 30 秒後被斷線 | BlueZ 連線後會讀取 iPhone 的所有 service，包括需要加密的 Apple service。iOS 回 `Insufficient Authentication (0x05)`，BlueZ 就自動發起 LE Secure Connections 配對（數字比對），但 Pi 上沒有程式負責確認 | 程式用 GDBus 實作 `org.bluez.Agent1`，在 `RequestConfirmation` 時自動同意 |
| 2 | 寫入時出現 ATT error `0x0e`，或回報找不到 characteristic | 配對和 service discovery 還沒完成時，iOS 會拒絕寫入，D-Bus 上也還看不到這個 characteristic | 寫入失敗就每秒重試一次，最多 30 秒 |
| 3 | 配對過後再連，出現 `gattlib_connect: Cannot find connection`（error 1） | 配對後 BlueZ 用 IRK 把 iPhone 的隨機位址解析成 identity address，但 D-Bus 物件路徑仍沿用最初看到的隨機位址。GATTLIB 用回報的位址重組路徑，所以找不到物件 | 用 `GetManagedObjects` 找出 `Address` 符合的物件，再用它的路徑去連線 |
| 4 | 配對過後再連，出現 `No ATT transport` | 配對後 BlueZ 以 public identity address 認得 iPhone，改用傳統藍牙（BR/EDR）連線。btmon 顯示 `Encryption: Enabled with AES-CCM (0x02)`，這是 BR/EDR 的加密方式，但 GATT 走的是 LE | 每次執行前用 `Adapter1.RemoveDevice` 刪掉舊的配對，讓 iPhone 被當成新的 LE 裝置重新配對 |
| 5 | 想用 `gattlib_indication_start()` 把 CCCD 設成 0x0002，btmon 卻顯示寫入的是 `0100` | GATTLIB 呼叫的是 BlueZ 的 `StartNotify`，由 BlueZ 自己選值。characteristic 同時支援 NOTIFY 時，BlueZ 會優先寫 `0x0001` | 在報告和程式訊息中照實說明。要寫 0x0002 只能讓 characteristic 只支援 INDICATE，或像 Basic 一樣直接寫 CCCD 的 handle |
| 6 | 偶爾出現 `le-connection-abort-by-local`，dmesg 有 `Frame reassembly failed` | Pi 電壓不足（`throttled=0x50005`），藍牙晶片的 UART 傳輸出錯 | 程式在連線失敗時重試 3 次；根本的解法是換 5V 2.5A 的電源 |

## 7. bluepy 與 GATTLIB 比較

| | bluepy（Basic，Python） | GATTLIB（Option 1，C） |
|---|---|---|
| 底層 | `bluepy-helper` 直接在 HCI 上收發 ATT | BlueZ 的 D-Bus API（`org.bluez`） |
| 程式模型 | 同步呼叫 | 非同步：`gattlib_mainloop` 加 callback，要自己處理 thread 同步 |
| 定位 characteristic | 用 handle（例如 `0x003d`） | 用 UUID，由 BlueZ 對應到 handle |
| 寫入方式 | Write Request（0x12） | BlueZ 自行決定，這次是 Prepare + Execute Write |
| CCCD | 自己指定要寫的值（`02 00`） | BlueZ 決定（有 NOTIFY 時寫 `01 00`） |
| 對 iPhone | 只碰指定的 handle，不需要配對 | BlueZ 會讀取加密的 service，因此必須配對 |

**心得**：GATTLIB 把連線、service discovery、CCCD 都交給 BlueZ 處理，寫起來比較高階，但也因此多了一層看不到的行為，例如自動配對、自動選擇 BR/EDR 或 LE、自動決定 CCCD 的值。這次靠 btmon 錄封包，加上把 GATTLIB 重新編譯成 debug log 版本，才找出這些問題，也比較清楚 BlueZ 在 central 這端做了哪些事。

## 附錄：my_write.c 完整原始碼

```c
/*
 * my_write.c - HW-Lab3 optional problem 1
 *
 * BLE central in C using GATTLIB. One program does the whole job:
 *   1. registers a BlueZ pairing agent (iOS asks to pair, see below)
 *   2. scans for the phone by its advertised name
 *   3. connects, writes a text string to a characteristic, reads it back
 *   4. optionally subscribes to the characteristic and prints what arrives
 *
 * Why an agent: after connecting, BlueZ reads every service of the iPhone,
 * including Apple services that need encryption. iOS answers "Insufficient
 * Authentication" and BlueZ starts LE Secure Connections pairing (numeric
 * comparison). Without an agent nobody confirms the passkey and the
 * connection is dropped, so this program confirms it (tap "Pair" on the
 * phone).
 *
 * Why forget old pairings first: once paired, BlueZ knows the iPhone by its
 * public identity address and connects to it over Classic Bluetooth
 * (BR/EDR) instead of LE, so there is no ATT transport ("No ATT transport").
 * It also keeps the D-Bus object path of the random address it first saw,
 * which GATTLIB cannot find ("Cannot find connection"). Removing the old
 * pairing before scanning gives a fresh LE connection every run.
 *
 * Usage:
 *   sudo ./my_write -c <char-uuid> -t "hello, I am Esther!" [-n Blank] [-i 30] [-k]
 *
 *   -n name     advertised name to look for (default Blank)
 *   -c uuid     characteristic to write (needs the WRITE property)
 *   -t text     text to write
 *   -i seconds  afterwards subscribe (BlueZ writes the CCCD) and listen
 *   -k          keep earlier pairings (default: forget them first)
 */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <gio/gio.h>

#include "gattlib.h"

#define SCAN_SECONDS    15
#define WRITE_RETRIES   30   /* seconds to wait for discovery / pairing */
#define MAX_CANDIDATES  4
#define CONNECT_ATTEMPTS 3
#define AGENT_PATH      "/lab3/agent"

static const char *want_name = "Blank";
static const char *char_text;
static const char *text;
static int indicate_seconds;
static int forget_first = 1;

static GDBusConnection *bus;
static char found_addr[18];
static uuid_t char_uuid;
static gattlib_connection_t *connection;

/* the connect callback runs on another thread; ble_task waits on this */
static pthread_mutex_t done_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER;
static int done, setup_ok, connect_error;

/* ------------------------------------------------------------------ */
/* Pairing agent (org.bluez.Agent1) that accepts every request         */
/* ------------------------------------------------------------------ */

static const char agent_xml[] =
	"<node><interface name='org.bluez.Agent1'>"
	"<method name='Release'/>"
	"<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
	"<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
	"<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
	"<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
	"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
	"<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='Cancel'/>"
	"</interface></node>";

static void agent_method_call(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *iface, const gchar *method,
		GVariant *params, GDBusMethodInvocation *inv, gpointer user_data) {
	(void)conn; (void)sender; (void)path; (void)iface; (void)user_data;

	if (strcmp(method, "RequestConfirmation") == 0) {
		const gchar *device;
		guint32 passkey;
		g_variant_get(params, "(&ou)", &device, &passkey);
		printf("Pairing: confirm passkey %06u -> yes (tap Pair on the phone)\n", passkey);
		fflush(stdout);
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (strcmp(method, "RequestPinCode") == 0) {
		g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "0000"));
	} else if (strcmp(method, "RequestPasskey") == 0) {
		g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 0));
	} else if (strcmp(method, "Cancel") == 0) {
		printf("Pairing canceled\n");
		g_dbus_method_invocation_return_value(inv, NULL);
	} else {
		/* Release, Display*, RequestAuthorization, AuthorizeService */
		g_dbus_method_invocation_return_value(inv, NULL);
	}
}

static const GDBusInterfaceVTable agent_vtable = { agent_method_call, NULL, NULL, { 0 } };

static int register_agent(void) {
	GError *error = NULL;
	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(agent_xml, &error);
	GVariant *reply;

	if (node == NULL) goto fail;
	if (!g_dbus_connection_register_object(bus, AGENT_PATH, node->interfaces[0],
			&agent_vtable, NULL, NULL, &error)) goto fail;

	reply = g_dbus_connection_call_sync(bus, "org.bluez", "/org/bluez",
			"org.bluez.AgentManager1", "RegisterAgent",
			g_variant_new("(os)", AGENT_PATH, "DisplayYesNo"),
			NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (reply == NULL) goto fail;
	g_variant_unref(reply);

	reply = g_dbus_connection_call_sync(bus, "org.bluez", "/org/bluez",
			"org.bluez.AgentManager1", "RequestDefaultAgent",
			g_variant_new("(o)", AGENT_PATH),
			NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (reply == NULL) goto fail;
	g_variant_unref(reply);

	g_dbus_node_info_unref(node);
	printf("Pairing agent registered\n");
	return 0;

fail:
	fprintf(stderr, "Cannot register pairing agent: %s\n", error ? error->message : "?");
	g_clear_error(&error);
	if (node) g_dbus_node_info_unref(node);
	return -1;
}

/* ------------------------------------------------------------------ */
/* BlueZ object lookup                                                 */
/* ------------------------------------------------------------------ */

/*
 * Collect the BlueZ Device1 objects whose Address (or Name, when name is
 * given) matches, and return the address each object path is named by,
 * e.g. /org/bluez/hci0/dev_50_90_36_5F_AF_DE -> "50:90:36:5F:AF:DE".
 */
static int find_device_paths(const char *addr, const char *name,
		char out[][18], int max) {
	GError *error = NULL;
	GVariant *reply, *objects, *ifaces;
	GVariantIter iter;
	const gchar *path;
	int n = 0;

	reply = g_dbus_connection_call_sync(bus, "org.bluez", "/",
			"org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
			NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
			-1, NULL, &error);
	if (reply == NULL) {
		fprintf(stderr, "GetManagedObjects failed: %s\n", error->message);
		g_error_free(error);
		return 0;
	}

	objects = g_variant_get_child_value(reply, 0);
	g_variant_iter_init(&iter, objects);
	while (n < max && g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
		GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", NULL);
		const char *tail = strstr(path, "/dev_");
		const gchar *value = NULL;

		if (dev && tail && strlen(tail) == 22) {
			int match = 0;
			if (addr && g_variant_lookup(dev, "Address", "&s", &value))
				match = strcasecmp(value, addr) == 0;
			if (!match && name && g_variant_lookup(dev, "Name", "&s", &value))
				match = strcmp(value, name) == 0;
			if (match) {
				int i;
				for (i = 0; i < 17; i++) out[n][i] = tail[5 + i] == '_' ? ':' : tail[5 + i];
				out[n][17] = '\0';
				n++;
			}
		}
		if (dev) g_variant_unref(dev);
		g_variant_unref(ifaces);
	}
	g_variant_unref(objects);
	g_variant_unref(reply);
	return n;
}

/* Remove paired devices (and cached ones advertising our name) so the
 * phone is found again as a new LE device. */
static void forget_phone(void) {
	GVariant *reply, *objects, *ifaces;
	GVariantIter iter;
	const gchar *path;

	reply = g_dbus_connection_call_sync(bus, "org.bluez", "/",
			"org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
			NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
			-1, NULL, NULL);
	if (reply == NULL) return;

	objects = g_variant_get_child_value(reply, 0);
	g_variant_iter_init(&iter, objects);
	while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
		GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", NULL);
		gboolean paired = FALSE;
		const gchar *name = NULL, *addr = "?";

		if (dev) {
			g_variant_lookup(dev, "Paired", "b", &paired);
			g_variant_lookup(dev, "Name", "&s", &name);
			g_variant_lookup(dev, "Address", "&s", &addr);
			if (paired || (name && strcmp(name, want_name) == 0)) {
				GVariant *r = g_dbus_connection_call_sync(bus, "org.bluez",
						"/org/bluez/hci0", "org.bluez.Adapter1", "RemoveDevice",
						g_variant_new("(o)", path), NULL, G_DBUS_CALL_FLAGS_NONE,
						-1, NULL, NULL);
				if (r) {
					printf("Forgot old %s %s\n", paired ? "pairing" : "entry", addr);
					g_variant_unref(r);
				}
			}
			g_variant_unref(dev);
		}
		g_variant_unref(ifaces);
	}
	g_variant_unref(objects);
	g_variant_unref(reply);
}

/* ------------------------------------------------------------------ */
/* GATTLIB part                                                        */
/* ------------------------------------------------------------------ */

static void print_value(const char *label, const uint8_t *data, size_t len) {
	size_t i;

	printf("%s %zu bytes: ", label, len);
	for (i = 0; i < len; i++) printf("%02x", data[i]);
	printf("  \"");
	for (i = 0; i < len; i++) putchar(isprint(data[i]) ? data[i] : '.');
	printf("\"\n");
	fflush(stdout);
}

static void on_device_found(gattlib_adapter_t *adapter, const char *addr,
		const char *name, void *user_data) {
	(void)user_data;

	if (found_addr[0] == '\0' && name && strcmp(name, want_name) == 0) {
		snprintf(found_addr, sizeof(found_addr), "%s", addr);
		printf("Found '%s' at %s\n", name, addr);
		gattlib_adapter_scan_disable(adapter);   /* stop scanning early */
	}
}

static void on_received(const uuid_t *uuid, const uint8_t *data,
		size_t len, void *user_data) {
	(void)uuid;
	(void)user_data;
	print_value("<<< received", data, len);
}

static void finish(int ok, int error) {
	pthread_mutex_lock(&done_mutex);
	setup_ok = ok;
	connect_error = error;
	done = 1;
	pthread_cond_signal(&done_cond);
	pthread_mutex_unlock(&done_mutex);
}

static void on_connected(gattlib_adapter_t *adapter, const char *dst,
		gattlib_connection_t *conn, int error, void *user_data) {
	void *value = NULL;
	size_t value_len = 0;
	int ret = GATTLIB_NOT_FOUND, i;
	(void)adapter;
	(void)user_data;

	if (error != GATTLIB_SUCCESS || conn == NULL) {
		finish(0, error);
		return;
	}
	connection = conn;
	printf("Connected to %s\n", dst);

	/* ---- write: the characteristic value ----
	 * Right after connecting the write can fail for a while: BlueZ is still
	 * exporting the characteristics, or iOS refuses it (ATT error 0x0e)
	 * until pairing has finished, so retry once a second. */
	for (i = 0; i < WRITE_RETRIES; i++) {
		ret = gattlib_write_char_by_uuid(conn, &char_uuid, text, strlen(text));
		if (ret == GATTLIB_SUCCESS) break;
		if (i == 0) printf("Waiting for service discovery / pairing ...\n");
		sleep(1);
	}
	if (ret != GATTLIB_SUCCESS) {
		fprintf(stderr, "Write failed (error %d). Does the characteristic have WRITE?\n", ret);
		finish(0, ret);
		return;
	}
	print_value("Wrote", (const uint8_t *)text, strlen(text));

	/* ---- read back ---- */
	if (gattlib_read_char_by_uuid(conn, &char_uuid, &value, &value_len) == GATTLIB_SUCCESS) {
		print_value("Read back", value, value_len);
		gattlib_characteristic_free_value(value);
	} else {
		printf("Read back failed (characteristic may not allow READ)\n");
	}

	/* ---- subscribe: BlueZ writes the CCCD for us ----
	 * gattlib_indication_start() ends up in BlueZ's D-Bus StartNotify, and
	 * BlueZ picks the CCCD value itself: 0x0001 (notification) when the
	 * characteristic supports NOTIFY, 0x0002 (indication) only when it
	 * supports INDICATE alone. */
	if (indicate_seconds > 0) {
		gattlib_register_indication(conn, on_received, NULL);
		ret = gattlib_indication_start(conn, &char_uuid);
		if (ret != GATTLIB_SUCCESS) {
			fprintf(stderr, "Failed to subscribe (error %d)\n", ret);
			finish(0, ret);
			return;
		}
		printf("Subscribed (BlueZ wrote the CCCD: 0x0001 if NOTIFY is supported, "
		       "else 0x0002), listening %d s ...\n", indicate_seconds);
	}
	finish(1, GATTLIB_SUCCESS);
}

static void *ble_task(void *arg) {
	gattlib_adapter_t *adapter = NULL;
	char candidates[MAX_CANDIDATES][18];
	int n, i, attempt, ok = 0;
	(void)arg;

	if (gattlib_adapter_open(NULL, &adapter) != GATTLIB_SUCCESS) {
		fprintf(stderr, "Failed to open the Bluetooth adapter\n");
		return NULL;
	}

	printf("Scanning up to %d s for '%s' ...\n", SCAN_SECONDS, want_name);
	gattlib_adapter_scan_enable(adapter, on_device_found, SCAN_SECONDS, NULL);
	if (found_addr[0] == '\0') {
		fprintf(stderr, "'%s' not found. Is the phone app in the foreground?\n", want_name);
		goto out;
	}

	/* the object path may be named by another (random) address */
	n = find_device_paths(found_addr, NULL, candidates, MAX_CANDIDATES);
	if (n == 0) {
		snprintf(candidates[0], sizeof(candidates[0]), "%s", found_addr);
		n = 1;
	}

	/* a connection attempt can be aborted (le-connection-abort-by-local),
	 * e.g. when the Pi's Bluetooth chip glitches, so try a few times */
	for (attempt = 1; attempt <= CONNECT_ATTEMPTS && !ok; attempt++) {
		if (attempt > 1) {
			printf("Retrying connection (%d/%d) ...\n", attempt, CONNECT_ATTEMPTS);
			sleep(2);
		}
		for (i = 0; i < n && !ok; i++) {
			pthread_mutex_lock(&done_mutex);
			done = 0;
			pthread_mutex_unlock(&done_mutex);

			if (strcasecmp(candidates[i], found_addr) != 0)
				printf("Using BlueZ object of %s for %s\n", candidates[i], found_addr);
			if (gattlib_connect(adapter, candidates[i], GATTLIB_CONNECTION_OPTIONS_NONE,
					on_connected, NULL) != GATTLIB_SUCCESS && i + 1 < n)
				continue;   /* this object is unknown to GATTLIB, try the next */

			pthread_mutex_lock(&done_mutex);
			while (!done) pthread_cond_wait(&done_cond, &done_mutex);
			ok = setup_ok;
			pthread_mutex_unlock(&done_mutex);
		}
		/* only a failed connection is worth retrying */
		if (!ok && connection != NULL) break;
	}
	if (!ok) {
		fprintf(stderr, "Failed (error %d)\n", connect_error);
		goto out;
	}

	if (indicate_seconds > 0) sleep(indicate_seconds);

out:
	if (connection) {
		gattlib_disconnect(connection, true);
		printf("Disconnected\n");
	}
	gattlib_adapter_close(adapter);
	return NULL;
}

static void usage(const char *prog) {
	fprintf(stderr, "Usage: %s -c <char-uuid> -t <text> [-n name] [-i seconds] [-k]\n", prog);
}

int main(int argc, char *argv[]) {
	GError *error = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "n:c:t:i:kh")) != -1) {
		switch (opt) {
		case 'n': want_name = optarg; break;
		case 'c': char_text = optarg; break;
		case 't': text = optarg; break;
		case 'i': indicate_seconds = atoi(optarg); break;
		case 'k': forget_first = 0; break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 1;
		}
	}
	if (char_text == NULL || text == NULL) {
		usage(argv[0]);
		return 1;
	}
	if (gattlib_string_to_uuid(char_text, strlen(char_text) + 1, &char_uuid) != GATTLIB_SUCCESS) {
		fprintf(stderr, "Invalid UUID: %s\n", char_text);
		return 1;
	}

	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (bus == NULL) {
		fprintf(stderr, "Cannot connect to the system D-Bus: %s\n", error->message);
		return 1;
	}
	/* registered before gattlib_mainloop(), so the agent's D-Bus calls are
	 * dispatched by the GLib main loop GATTLIB runs */
	if (register_agent() != 0) return 1;
	if (forget_first) forget_phone();

	if (gattlib_mainloop(ble_task, NULL) != GATTLIB_SUCCESS) {
		fprintf(stderr, "Failed to create the GATTLIB main loop\n");
		return 1;
	}
	return 0;
}
```
