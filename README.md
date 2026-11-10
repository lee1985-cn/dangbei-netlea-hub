# 当贝投影仪 + 尼特利滤桶 · ESP32-C3 双控网关

[![Hardware: ESP32-C3](https://img.shields.io/badge/Hardware-ESP32--C3-blue.svg)](#)
[![Integration: Home Assistant](https://img.shields.io/badge/Integration-Home%20Assistant-41BDF5.svg)](#)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

一块 ESP32-C3 同时干两件事：

1. **当贝投影仪纯本地控制** —— 开关机、音量、播放暂停，不依赖任何云
2. **尼特利变频过滤桶控制** —— 开关、1~20 档、喂食暂停

两个设备的控制互不干扰，全部通过局域网 HTTP 暴露给 Home Assistant。

> 本项目基于 [lee1985-cn/dangbei-projector-ha](https://github.com/lee1985-cn/dangbei-projector-ha) 的投影仪固件扩展而来，投影仪部分的逻辑（双模软重启架构、信标唤醒、防吞键序列）原样保留。

---

## 为什么需要两块 BLE 角色同时跑

| 设备 | ESP32 扮演的角色 | 协议 |
|---|---|---|
| 当贝投影仪 | **Peripheral（从机）** | HID 键盘 + 广播信标 |
| 尼特利滤桶 | **Central（主机）** | GATT 客户端读写 |

ESP32-C3 支持 BLE 双角色，NimBLE 默认允许 3 个连接，所以一块板子够用。

**前提是全局 `#define USE_NIMBLE`** —— 否则 BleKeyboard 会拉进来一套 Bluedroid，和滤桶的 GATT 客户端抢内存，两个都跑不稳。

---

## 投影仪部分怎么工作（继承自原项目）

当贝投影仪深度休眠时会过滤掉普通 BLE 键盘，所以固件用了**双模软重启**：

- **Mode A（唤醒）**：把 `MAGIC_WAKEUP` 写进 `RTC_NOINIT_ATTR` 内存 → `ESP.restart()` → 重启后不进正常逻辑，而是重放 3 个原厂唤醒广播包（各 1.5 秒）→ 再次重启
- **Mode B（常态）**：NimBLE 起 HID 键盘，和投影仪保持连接，收到 HTTP 命令就发按键

配对密钥存在 NVS 里（`nvs_flash_init` 强制初始化），断电不丢，配一次就行。

关机是「长按电源键 1.5 秒」，前面还垫了一个防吞键的短促包。

---

## 滤桶部分怎么工作

尼特利没有公开协议，这份是从官方微信小程序逆向出来的，完整文档见 [docs/PROTOCOL.md](docs/PROTOCOL.md)。

要点：

- 服务 `0003CBBB-0000-1000-8000-00805F9BFFF0`
- 控制写 `...9BEEE1`，状态通知 `...9BEEE4`
- **明文协议，无配对、无绑定密钥**，只有一层 XOR 混淆 + 自定义校验字节
- 控制帧 12 字节 → 编码后 13 字节

固件每次发命令都是「连接 → 写 → 等 150ms → 断开」，因为设备是单连接的（ESP32 占着连接时手机小程序就连不上）。

---

## 关于那个补丁（原项目最容易踩的坑）

本项目把打过补丁的 `ESP32-BLE-Keyboard` **直接 vendor 在 `lib/` 里**，所以**不需要**像原项目那样手动去改系统库。

如果想知道补丁是什么：

```diff
  // BleKeyboard.cpp 第 80 行，媒体键描述符
- USAGE(1),           0xB7,          //   USAGE (Stop)                ; bit 2: 4
+ USAGE(1),           0x30,          //   USAGE (Power)               ; bit 2: 4
```

**为什么是 bit 2 而不是 bit 7**：`main.cpp` 里 `KEY_PROJ_POWER = {0x04, 0x00}` 用的是 bit 2。库的媒体键位序是 bit0=Next、bit1=Prev、**bit2=Stop(0xB7)**、bit3=PlayPause、bit7=Mail(0x8A)。改错位置的症状是「按关机变成音量+」。

（原项目 README 里写的是改 bit 7 的 `0x09, 0x8A`，那行在库里根本不存在，已提 PR 修正。）

---

## 编译

### PlatformIO（推荐）

```bash
pio run -e esp32c3                    # 编译
pio run -e esp32c3 -t upload          # USB 烧录
pio run -e esp32c3_ota -t upload      # 局域网 OTA
```

固件约 1.6 MB，必须用 `huge_app` 分区（`platformio.ini` 里已配好）。

### Arduino CLI

```bash
arduino-cli core install esp32:esp32@2.0.17
arduino-cli lib install "NimBLE-Arduino@1.4.3"
# 把 lib/ESP32-BLE-Keyboard 拷到 ~/Arduino/libraries/
arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=huge_app \
  --build-property "compiler.cpp.extra_flags=-DUSE_NIMBLE" src
```

> ⚠️ `-DUSE_NIMBLE` **必须作为全局编译参数传**。写在 `.ino` 顶部的 `#define` 只对 sketch 生效，
> 不会传给库文件，结果是 BleKeyboard 用 Bluedroid、滤桶客户端用 NimBLE，两个蓝牙栈并存，
> NimBLE 根本没初始化，扫描和连接会静默失败。PlatformIO 的 `build_flags` 是全局的，没这个坑。

---

## 烧录前的准备

1. 改 `src/main.cpp` 顶部的 WiFi：

```cpp
const char* ssid = "你的wifi名字";
const char* password = "你的wifi密码";
```

2. **先备份原固件**（如果这块板子原来在跑别的固件）：

```bash
esptool.py --port COM3 read_flash 0 0x400000 backup.bin
```

3. 烧录。**只写 app，不要覆盖 bootloader**：

```bash
esptool.py --port COM3 write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x10000  app.bin
```

> ⚠️ **不要刷 bootloader**。实测把 arduino-cli 编出来的 `bootloader.bin` 写进去会导致无限重启
> （`rst:0x3` 循环，串口只剩 ROM 横幅）。板子上原有的 bootloader 是好用的，分区表与原厂逐字节一致，
> 两者都不用动。只换 app 分区（0x10000）即可。

> 不动 `0x9000` 的 NVS，投影仪的蓝牙配对密钥就还在，换固件不用重新配对。

## 刷完配网

连不上 WiFi 时板子会自动开热点：

```
热点名：Dangbei-Setup
```

手机连上，打开 `http://192.168.4.1`，填 WiFi 名和密码，保存自动重启。**不用重新刷机。**

## 投影仪配对（只做一次）

1. 原厂遥控器打开投影仪
2. 设置 → 蓝牙设置
3. 删掉残留的 `Dangbei_ESP32`
4. 重新搜索并连接 `Dangbei_ESP32`

之后 ESP32 随便断电移动都不会掉签。

---

## 接口

投影仪（原有）：

| 接口 | 作用 |
|---|---|
| `/state` | 连接状态 `{"state":"ON"/"OFF"}` |
| `/power_on` `/power_off` | 开/关机 |
| `/vol_up` `/vol_down` `/mute` `/play_pause` | 媒体控制 |

滤桶（新增）：

| 接口 | 作用 |
|---|---|
| `/filter/state` | 缓存状态 |
| `/filter/read` | 主动读一次 |
| `/filter/scan?s=8` | 扫描并记住滤桶 MAC |
| `/filter/setmac?mac=..` | 手动指定 MAC |
| `/filter/on` `/filter/off` | 开/停泵 |
| `/filter/speed?n=1..20` | 设档位 |
| `/filter/feed?min=1..60` | 喂食暂停 |
| `/filter/backble` | 切回蓝牙控制 |

Home Assistant 配置见 [docs/HA-CONFIG.md](docs/HA-CONFIG.md)。

---

## 已知限制

- **滤桶单连接**：ESP32 连着时手机小程序连不上。固件是"发完即断"，所以你在家随时能用小程序，代价是 HA 里的状态是缓存值。
- **自吸排气期禁控**：滤桶开机后 30 秒 ~ 15 分钟（`model=7`）会拒绝所有蓝牙命令，自动化要等状态变成 1 再发档位。
- **唤醒投影仪会断滤桶连接**：唤醒走的是 `ESP.restart()`，重启必然断 BLE，重连约 1~2 秒。
- **只支持尼特利变频过滤桶**：协议是从官方小程序逆向的，其它型号（尤其气泵、灯具）命令字不同，本项目没实现。

## 免责

逆向协议仅用于让自家设备接入自建 Home Assistant。改固件有变砖风险，刷机前务必备份原 flash。作者不对任何设备损坏负责。

## License

MIT
