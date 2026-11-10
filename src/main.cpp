// 当贝投影仪 + 尼特利滤桶 双控固件 (ESP32-C3)
// 基于 lee1985-cn/dangbei-projector-ha 的 src/main.cpp 扩展
// 新增：滤桶 BLE 客户端、ArduinoOTA、滤桶 HTTP 接口
// 投影仪原有逻辑（双模软重启 + 信标唤醒 + HID 按键）完全保留

// 【核心补丁 1】强制 BleKeyboard 统一使用 NimBLE 协议栈
#define USE_NIMBLE

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BleKeyboard.h>
#include <NimBLEDevice.h>
#include <nvs_flash.h>
#include <ArduinoOTA.h>
#include <Preferences.h>

// ==================== 配置 ====================
const char* ssid = "你的wifi名字";
const char* password = "你的wifi密码";

const char* BLE_NAME = "Dangbei_ESP32";
const uint32_t MAGIC_WAKEUP = 0xAABBCCDD;
const unsigned long WIFI_TIMEOUT_MS = 15000;

RTC_NOINIT_ATTR uint32_t boot_mode_magic;

WebServer server(80);
BleKeyboard* bleKeyboard = nullptr;

// 【核心补丁 2】电源键掩码 bit2 -> 需把库里的 0xB7(Stop) 改成 0x30(Power)
const MediaKeyReport KEY_PROJ_POWER = {0x04, 0x00};

// 经过实测 100% 能够唤醒当贝主板的魔法数据包
const uint8_t pkt1[] = {0x02,0x01,0x05,0x05,0x02,0x0F,0x18,0x12,0x18,0x0E,0xFF,0x46,0x00,0x9B,0xAD,0x4A,0x6D,0x60,0x91,0x0C,0xFF,0xFF,0xFF,0xFF};
const uint8_t pkt2[] = {0x02,0x01,0x05,0x05,0x02,0x0F,0x18,0x12,0x18,0x0E,0xFF,0x46,0x00,0xC5,0xAD,0x4A,0x6D,0x60,0x91,0x0C,0xFF,0xFF,0xFF,0xFF};
const uint8_t pkt3[] = {0x02,0x01,0x05,0x05,0x02,0x0F,0x18,0x12,0x18,0x0E,0xFF,0x46,0x00,0xCB,0xAD,0x4A,0x6D,0x60,0x91,0x0C,0xFF,0xFF,0xFF,0xFF};

// ==================== 尼特利滤桶协议 ====================
// 服务 0003CBBB-0000-1000-8000-00805F9BFFF0
// 控制写 EEE1 / 状态通知 EEE4 / 语音&切回蓝牙 EEE5
static NimBLEUUID SVC_FILTER("0003cbbb-0000-1000-8000-00805f9bfff0");
static NimBLEUUID CH_CTL   ("0003cbbb-0000-1000-8000-00805f9beee1");
static NimBLEUUID CH_NOTIFY("0003cbbb-0000-1000-8000-00805f9beee4");
static NimBLEUUID CH_VOICE ("0003cbbb-0000-1000-8000-00805f9beee5");

static const uint8_t CRC_KEY[8] = {76,101,100,115,38,70,117,110}; // "Leds&Fun"

Preferences prefs;
String filterMac = "";
// 出厂默认滤桶 MAC（可用 /filter/setmac 覆盖；留空则开机时自动扫描）
#define FILTER_MAC_DEFAULT ""   // 留空则开机时用 /filter/scan 自动发现，或用 /filter/setmac 指定
int fAddrType = -1;   // 扫描时记下的地址类型，-1=未知          // 滤桶 MAC，首次可自动发现或手动设置

// 最近一次状态
int   fGear = -1;      // 档位 0=停
int   fModel = -1;     // 1档位 2档位+level 3喂食设置 5喂食中 6关 7自吸排气
int   fErr = -1;
int   fCharge = -1;
int   fElectric = -1;
bool  fConnected = false;
unsigned long fLastSeen = 0;

uint8_t nlCrc(const uint8_t* d, size_t n) {
  uint8_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t r = d[i];
    for (int k = 0; k < 8; k++) {
      if (r & 1) sum ^= CRC_KEY[k];
      r >>= 1;
    }
  }
  return sum;
}

// 明文 -> 发送帧（首字节校验 + 逐字节 XOR）
size_t nlEncode(const uint8_t* plain, size_t n, uint8_t* out) {
  uint8_t t = nlCrc(plain, n);
  out[0] = t;
  for (size_t i = 0; i < n; i++) out[i + 1] = t ^ plain[i];
  return n + 1;
}

// 接收帧 -> 明文，校验失败返回 0
size_t nlDecode(const uint8_t* frame, size_t n, uint8_t* out) {
  if (n < 2) return 0;
  uint8_t t = frame[0];
  for (size_t i = 0; i < n - 1; i++) out[i] = t ^ frame[i + 1];
  if (nlCrc(out, n - 1) != t) return 0;
  return n - 1;
}

// 控制帧 12 字节
void buildCtl(uint8_t* d, bool on, int model, int gear, int level, int timing) {
  if (!on) model = 6;
  d[0] = 0x02;
  d[1] = on ? 1 : 0;
  d[2] = 0;
  d[3] = 1;
  d[4] = 1;
  d[5] = model & 0xFF;
  d[6] = gear & 0xFF;
  d[7] = level & 0xFF;
  d[8] = timing & 0xFF;
  d[9] = 0;
  d[10] = 0x58;
  d[11] = 0x02;
}

// ==================== 滤桶 BLE 客户端 ====================
static NimBLEClient* fClient = nullptr;

class FilterNotifyCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override { fConnected = true; }
  void onDisconnect(NimBLEClient*) override { fConnected = false; }
};

void filterParseNotify(const uint8_t* frame, size_t n) {
  uint8_t plain[64];
  size_t m = nlDecode(frame, n, plain);
  if (m < 8) return;
  // 实测布局（NL NO.1S-G1）：[1]电量 [2]电压 [3]档位 [4]流量等级 [6]工作模式 [7]错误码
  fCharge   = plain[1];
  fElectric = plain[2];
  fGear     = plain[3];
  fModel    = plain[6];
  fErr      = plain[7];
  fLastSeen = millis();
  Serial.printf("[FILTER] gear=%d model=%d err=%d\n", fGear, fModel, fErr);
}

void filterNotifyCB(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify) {
  char hx[200]; int p = 0;
  for (size_t i = 0; i < len && p < 190; i++) p += snprintf(hx + p, sizeof(hx) - p, "%02x ", data[i]);
  ets_printf("[FILTER] notify len=%d raw=%s\n", (int)len, hx);
  filterParseNotify(data, len);
}

unsigned long fLastUse = 0;
const unsigned long FILTER_IDLE_MS = 20000;   // 空闲 20 秒自动断开，把连接让给手机小程序

// 建立/复用连接（带重试）。长连接，不再每次发完就断
bool filterConnect() {
  if (filterMac.length() == 0) return false;
  if (fClient && fClient->isConnected()) { fLastUse = millis(); return true; }

  if (!fClient) {
    fClient = NimBLEDevice::createClient();
    fClient->setClientCallbacks(new FilterNotifyCB(), false);
    fClient->setConnectTimeout(5);   // 参数是 uint8_t，单位秒
  }

  uint8_t types[2] = {BLE_ADDR_PUBLIC, BLE_ADDR_RANDOM};
  if (fAddrType == 0 || fAddrType == 1) {
    types[0] = fAddrType;
    types[1] = (fAddrType == 0) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
  }

  for (int attempt = 0; attempt < 3 && !fClient->isConnected(); attempt++) {
    for (int i = 0; i < 2 && !fClient->isConnected(); i++) {
      bool ok = fClient->connect(NimBLEAddress(std::string(filterMac.c_str()), types[i]));
      ets_printf("[FILTER] connect a=%d type=%d -> %d\n", attempt, (int)types[i], (int)ok);
      if (!ok) { fClient->disconnect(); delay(250); }
    }
    if (!fClient->isConnected()) delay(500);
  }
  if (!fClient->isConnected()) { ets_printf("[FILTER] connect failed\n"); return false; }

  auto* svc = fClient->getService(SVC_FILTER);
  if (!svc) { ets_printf("[FILTER] no svc\n"); fClient->disconnect(); return false; }
  auto* chN = svc->getCharacteristic(CH_NOTIFY);
  if (chN && chN->canNotify()) {
    chN->subscribe(true, filterNotifyCB);
    delay(200);   // 等 CCCD 生效
  }
  fConnected = true;
  fLastUse = millis();
  ets_printf("[FILTER] connected\n");
  return true;
}

void filterDisconnect() {
  if (fClient && fClient->isConnected()) fClient->disconnect();
  fConnected = false;
}

// 在 loop 里调用：空闲超时主动断开，避免长期占着设备
void filterIdleCheck() {
  if (fClient && fClient->isConnected() && fLastUse && (millis() - fLastUse > FILTER_IDLE_MS)) {
    ets_printf("[FILTER] idle disconnect\n");
    filterDisconnect();
  }
}

// 写控制帧（EEE1）
bool filterSendCtl(bool on, int model, int gear, int level, int timing) {
  if (!filterConnect()) return false;
  auto* svc = fClient->getService(SVC_FILTER);
  auto* ch = svc ? svc->getCharacteristic(CH_CTL) : nullptr;
  if (!ch) { filterDisconnect(); return false; }

  uint8_t plain[12], frame[16];
  buildCtl(plain, on, model, gear, level, timing);
  size_t n = nlEncode(plain, sizeof(plain), frame);
  bool ok = ch->writeValue(frame, n, false);
  unsigned long tw = millis();
  while (millis() - tw < 700) { delay(20); }
  fLastUse = millis();
  return ok;
}

// 写任意明文帧到指定特征值
bool filterSendRawTo(const uint8_t* plain, size_t n, NimBLEUUID target) {
  if (!filterConnect()) return false;
  auto* svc = fClient->getService(SVC_FILTER);
  auto* ch = svc ? svc->getCharacteristic(target) : nullptr;
  if (!ch) { filterDisconnect(); return false; }
  uint8_t frame[80];
  size_t fn = nlEncode(plain, n, frame);
  bool ok = ch->writeValue(frame, fn, false);
  unsigned long tw = millis();
  while (millis() - tw < 900) { delay(20); }
  fLastUse = millis();
  return ok;
}

bool filterSendRaw(const uint8_t* plain, size_t n) {
  return filterSendRawTo(plain, n, CH_CTL);
}

// 扫描：按服务 UUID 或名字找滤桶，同时把附近设备打到串口
int gScanCount = -1;
int gScanOk = -1;
unsigned long gScanMs = 0;

String filterScan(int seconds, bool verbose) {
  unsigned long t0 = millis();
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) { gScanCount = -2; gScanOk = -2; return ""; }
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
  // 用非阻塞 API，自己等，避免阻塞版在 start() 失败时静默返回
  bool ok = scan->start((uint32_t)seconds, nullptr, false);
  gScanOk = ok ? 1 : 0;
  ets_printf("[SCAN] start ok=%d\n", (int)ok);
  unsigned long w = millis();
  while (millis() - w < (unsigned long)seconds * 1000UL) { delay(50); }
  scan->stop();
  NimBLEScanResults res = scan->getResults();
  gScanMs = millis() - t0;
  gScanCount = res.getCount();
  ets_printf("[SCAN] done ok=%d count=%d elapsed=%lu\n", gScanOk, gScanCount, gScanMs);
  String found = "";
  String all = "";
  for (int i = 0; i < res.getCount(); i++) {
    NimBLEAdvertisedDevice d = res.getDevice(i);
    String name = d.haveName() ? String(d.getName().c_str()) : String("");
    String mac  = String(d.getAddress().toString().c_str());
    bool hasSvc = d.haveServiceUUID() && d.getServiceUUID().equals(SVC_FILTER);
    all += mac + "|" + name + "|" + String(d.getRSSI()) + "|type=" + String((int)d.getAddress().getType()) + "\n";
    ets_printf("[SCAN] %s %s rssi=%d type=%d\n", mac.c_str(), name.c_str(), d.getRSSI(), (int)d.getAddress().getType());
    if (hasSvc || name.indexOf("NETLEA") >= 0 || name.indexOf("netlea") >= 0 || name.indexOf("NTL") >= 0) {
      found = mac;
      fAddrType = (int)d.getAddress().getType();
      ets_printf("[SCAN] MATCH %s type=%d\n", mac.c_str(), fAddrType);
      if (verbose) Serial.println("[FILTER] found " + mac + " " + name);
    }
  }
  scan->clearResults();
  if (verbose) Serial.println(all);
  if (found.length() && filterMac.length() == 0) {
    filterMac = found;
    prefs.putString("fmac", filterMac);
  }
  return found;
}

// ==================== 投影仪：唤醒信标模式 ====================
void sendRawAdv(NimBLEAdvertising* adv, const uint8_t* data, size_t len) {
  adv->stop();
  NimBLEAdvertisementData advData;
  advData.addData(std::string(reinterpret_cast<const char*>(data), len));
  adv->setAdvertisementData(advData);
  adv->start();
}

void broadcastPacket(NimBLEAdvertising* adv, const uint8_t* pkt, size_t len, unsigned long durationMs) {
  unsigned long t = millis();
  while (millis() - t < durationMs) {
    sendRawAdv(adv, pkt, len);
    delay(200);
  }
}

void runWakeupBeaconMode() {
  ets_printf("[A] wakeup beacon mode\n");
  Serial.println("\n[MODE A] 发射魔法唤醒包...");
  NimBLEDevice::init(BLE_NAME);
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();

  broadcastPacket(adv, pkt1, sizeof(pkt1), 1500);
  broadcastPacket(adv, pkt2, sizeof(pkt2), 1500);
  broadcastPacket(adv, pkt3, sizeof(pkt3), 1500);

  adv->stop();
  boot_mode_magic = 0;
  delay(100);
  ESP.restart();
}

// ==================== HTTP 接口 ====================
void addCors() { server.sendHeader("Access-Control-Allow-Origin", "*"); }

void setupHttpEndpoints() {
  // ---- 投影仪 ----
  server.on("/state", HTTP_GET, []() {
    addCors();
    if (bleKeyboard && bleKeyboard->isConnected()) server.send(200, "application/json", "{\"state\":\"ON\"}");
    else server.send(200, "application/json", "{\"state\":\"OFF\"}");
  });

  server.on("/power_on", HTTP_GET, []() {
    addCors();
    if (bleKeyboard && bleKeyboard->isConnected()) {
      server.send(200, "application/json", "{\"status\":\"already_on\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"waking_up\"}");
      boot_mode_magic = MAGIC_WAKEUP;
      delay(100);
      ESP.restart();
    }
  });

  server.on("/power_off", HTTP_GET, []() {
    addCors();
    if (bleKeyboard && bleKeyboard->isConnected()) {
      bleKeyboard->releaseAll();
      delay(50);
      bleKeyboard->press(KEY_PROJ_POWER);
      delay(100);
      bleKeyboard->releaseAll();
      delay(300);
      bleKeyboard->press(KEY_PROJ_POWER);
      delay(1500);
      bleKeyboard->releaseAll();
      server.send(200, "application/json", "{\"status\":\"turning_off\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"already_off\"}");
    }
  });

  server.on("/play_pause", HTTP_GET, []() {
    addCors();
    if (bleKeyboard && bleKeyboard->isConnected()) {
      bleKeyboard->write(KEY_MEDIA_PLAY_PAUSE);
      server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else server.send(200, "application/json", "{\"status\":\"not_connected\"}");
  });

  server.on("/vol_up", HTTP_GET, []() {
    addCors();
    if (bleKeyboard && bleKeyboard->isConnected()) {
      bleKeyboard->write(KEY_MEDIA_VOLUME_UP);
      server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else server.send(200, "application/json", "{\"status\":\"not_connected\"}");
  });

  server.on("/vol_down", HTTP_GET, []() {
    addCors();
    if (bleKeyboard && bleKeyboard->isConnected()) {
      bleKeyboard->write(KEY_MEDIA_VOLUME_DOWN);
      server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else server.send(200, "application/json", "{\"status\":\"not_connected\"}");
  });

  server.on("/mute", HTTP_GET, []() {
    addCors();
    if (bleKeyboard && bleKeyboard->isConnected()) {
      bleKeyboard->write(KEY_MEDIA_MUTE);
      server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else server.send(200, "application/json", "{\"status\":\"not_connected\"}");
  });

  // ---- 滤桶 ----
  server.on("/filter/state", HTTP_GET, []() {
    addCors();
    char buf[220];
    snprintf(buf, sizeof(buf),
      "{\"connected\":%s,\"gear\":%d,\"model\":%d,\"err\":%d,\"charge\":%d,\"voltage\":%d,\"mac\":\"%s\",\"age_ms\":%lu,\"uptime_ms\":%lu}",
      fConnected ? "true" : "false", fGear, fModel, fErr, fCharge, fElectric,
      filterMac.c_str(), fLastSeen ? (millis() - fLastSeen) : 0UL, millis());
    server.send(200, "application/json", buf);
  });

  server.on("/filter/read", HTTP_GET, []() {
    addCors();
    uint8_t cmd[2] = {0x02, 0x02};
    bool ok = filterSendRawTo(cmd, 2, CH_NOTIFY);   // 小程序把读命令写在 EEE4
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"sent\":%s,\"gear\":%d,\"model\":%d}", ok ? "true" : "false", fGear, fModel);
    server.send(200, "application/json", buf);
  });

  server.on("/filter/speed", HTTP_GET, []() {
    addCors();
    if (!server.hasArg("n")) { server.send(400, "application/json", "{\"error\":\"need ?n=1..20\"}"); return; }
    int n = server.arg("n").toInt();
    if (n < 1 || n > 20) { server.send(400, "application/json", "{\"error\":\"n must be 1..20\"}"); return; }
    bool ok = filterSendCtl(true, 1, n, 0, 0);
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"sent\":%s,\"gear\":%d}", ok ? "true" : "false", n);
    server.send(200, "application/json", buf);
  });

  server.on("/filter/on", HTTP_GET, []() {
    addCors();
    int g = fGear > 0 ? fGear : 8;
    bool ok = filterSendCtl(true, 1, g, 0, 0);
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"sent\":%s,\"gear\":%d}", ok ? "true" : "false", g);
    server.send(200, "application/json", buf);
  });

  server.on("/filter/off", HTTP_GET, []() {
    addCors();
    int g = fGear > 0 ? fGear : 8;
    bool ok = filterSendCtl(false, 6, g, 0, 0);
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"sent\":%s}", ok ? "true" : "false");
    server.send(200, "application/json", buf);
  });

  server.on("/filter/feed", HTTP_GET, []() {
    addCors();
    int m = server.hasArg("min") ? server.arg("min").toInt() : 10;
    if (m < 1 || m > 60) { server.send(400, "application/json", "{\"error\":\"min 1..60\"}"); return; }
    bool ok = filterSendCtl(true, 3, 0, 0, m);
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"sent\":%s,\"minutes\":%d}", ok ? "true" : "false", m);
    server.send(200, "application/json", buf);
  });

  server.on("/filter/backble", HTTP_GET, []() {
    addCors();
    uint8_t cmd[2] = {0x02, 0x04};
    bool ok = filterSendRaw(cmd, 2);
    server.send(200, "application/json", ok ? "{\"sent\":true}" : "{\"sent\":false}");
  });

  server.on("/filter/dump", HTTP_GET, []() {
    addCors();
    if (!filterConnect()) { server.send(200, "application/json", "{\"error\":\"connect failed\"}"); return; }
    auto* svc = fClient->getService(SVC_FILTER);
    if (!svc) { filterDisconnect(); server.send(200, "application/json", "{\"error\":\"no svc\"}"); return; }
    String j = "{\"chars\":[";
    auto* chars = svc->getCharacteristics(true);
    bool first = true;
    for (auto* c : *chars) {
      if (!first) j += ",";
      first = false;
      j += "{\"uuid\":\"" + String(c->getUUID().toString().c_str()) + "\"";
      j += ",\"read\":" + String(c->canRead() ? "true" : "false");
      j += ",\"write\":" + String(c->canWrite() ? "true" : "false");
      j += ",\"writeNR\":" + String(c->canWriteNoResponse() ? "true" : "false");
      j += ",\"notify\":" + String(c->canNotify() ? "true" : "false");
      j += ",\"indicate\":" + String(c->canIndicate() ? "true" : "false");
      j += "}";
      ets_printf("[DUMP] %s r=%d w=%d wnr=%d n=%d i=%d\n", c->getUUID().toString().c_str(),
                 c->canRead(), c->canWrite(), c->canWriteNoResponse(), c->canNotify(), c->canIndicate());
    }
    j += "]}";
    filterDisconnect();
    server.send(200, "application/json", j);
  });

  // 直接 GATT 读 EEE4 拿状态（比 notify 省事，设备所有特征值都支持 read）
  server.on("/filter/readgatt", HTTP_GET, []() {
    addCors();
    if (!filterConnect()) { server.send(200, "application/json", "{\"error\":\"connect failed\"}"); return; }
    auto* svc = fClient->getService(SVC_FILTER);
    auto* ch = svc ? svc->getCharacteristic(CH_NOTIFY) : nullptr;
    if (!ch) { filterDisconnect(); server.send(200, "application/json", "{\"error\":\"no char\"}"); return; }
    auto v = ch->readValue();
    uint8_t buf[64];
    size_t n = v.length() < sizeof(buf) ? v.length() : sizeof(buf);
    memcpy(buf, v.data(), n);
    char hx[220]; int q = 0;
    for (size_t i = 0; i < n && q < 210; i++) q += snprintf(hx + q, sizeof(hx) - q, "%02x ", buf[i]);
    ets_printf("[FILTER] readgatt len=%d raw=%s\n", (int)n, hx);
    filterParseNotify(buf, n);
    filterDisconnect();
    char out[320];
    snprintf(out, sizeof(out), "{\"len\":%d,\"raw\":\"%s\",\"gear\":%d,\"model\":%d,\"err\":%d,\"charge\":%d,\"voltage\":%d}",
             (int)n, hx, fGear, fModel, fErr, fCharge, fElectric);
    server.send(200, "application/json", out);
  });

  server.on("/filter/scan", HTTP_GET, []() {
    addCors();
    int s = server.hasArg("s") ? server.arg("s").toInt() : 6;
    String found = filterScan(s, true);
    String body = "{\"found\":\"" + found + "\",\"mac\":\"" + filterMac +
                  "\",\"ok\":" + String(gScanOk) + ",\"count\":" + String(gScanCount) +
                  ",\"elapsed_ms\":" + String(gScanMs) +
                  ",\"requested_s\":" + String(s) + "}";
    server.send(200, "application/json", body);
  });

  server.on("/filter/setmac", HTTP_GET, []() {
    addCors();
    if (!server.hasArg("mac")) { server.send(400, "application/json", "{\"error\":\"need ?mac=AA:BB:...\"}"); return; }
    filterMac = server.arg("mac");
    size_t w = prefs.putString("fmac", filterMac);
    ets_printf("[SETMAC] wrote=%d readback=[%s]\n", (int)w, prefs.getString("fmac","").c_str());
    server.send(200, "application/json", "{\"mac\":\"" + filterMac + "\"}");
  });
}

// ==================== 模式 B：常态控制 ====================
// WiFi 凭据：优先用 NVS 里存的（配网页面写入），否则用编译进去的默认值
String wifiSsid, wifiPass;

bool wifiConnect(unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  return WiFi.status() == WL_CONNECTED;
}

// 连不上就开热点配网，省得为了改个密码重新刷机
void startConfigPortal() {
  Serial.println("\n[WIFI] 连接失败，启动配网热点 Dangbei-Setup");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Dangbei-Setup");
  Serial.print("[WIFI] AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, []() {
    String h = "<meta charset='utf-8'><h2>配置 WiFi</h2><form action='/wifi'>"
               "SSID: <input name='ssid' value='" + wifiSsid + "'><br><br>"
               "密码: <input name='pass' type='password'><br><br>"
               "<button>保存并重启</button></form>";
    server.send(200, "text/html; charset=utf-8", h);
  });
  server.on("/wifi", HTTP_GET, []() {
    if (server.hasArg("ssid")) {
      prefs.putString("wssid", server.arg("ssid"));
      prefs.putString("wpass", server.arg("pass"));
      server.send(200, "text/html; charset=utf-8", "<meta charset='utf-8'>已保存，正在重启…");
      delay(800);
      ESP.restart();
    } else server.send(400, "text/plain", "need ssid");
  });
  server.begin();
  while (true) { server.handleClient(); delay(2); }
}

void runNormalControlMode() {
  Serial.println("\n[MODE B] 进入常态控制模式...");

  ets_printf("[B] prefs.begin\n");
  bool pb = prefs.begin("dangbei", false);
  ets_printf("[B] prefs.begin=%d\n", (int)pb);
  filterMac = prefs.getString("fmac", "");
  if (filterMac.length() == 0) { filterMac = FILTER_MAC_DEFAULT; ets_printf("[B] use default fmac\n"); }
  ets_printf("[B] fmac=[%s]\n", filterMac.c_str());
  wifiSsid = prefs.getString("wssid", ssid);
  wifiPass = prefs.getString("wpass", password);

  ets_printf("[B] BleKeyboard new\n");
  bleKeyboard = new BleKeyboard(BLE_NAME, "Espressif", 100);
  ets_printf("[B] BleKeyboard begin\n");
  bleKeyboard->begin();
  ets_printf("[B] BleKeyboard ok\n");

  ets_printf("[B] wifi connect\n");
  if (!wifiConnect(WIFI_TIMEOUT_MS)) {
    // 再给一次机会（有的路由器握手慢）
    if (!wifiConnect(5000)) {
      startConfigPortal();   // 不会返回
    }
  }
  Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());

  ets_printf("[B] wifi ok ip=%s\n", WiFi.localIP().toString().c_str());
  // OTA：以后改固件不用拆板子
  ArduinoOTA.setHostname("dangbei-esp32");
  ArduinoOTA.onStart([]() { Serial.println("[OTA] start"); });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] done"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] error %u\n", e); });
  ArduinoOTA.begin();

  ets_printf("[B] endpoints\n");
  setupHttpEndpoints();
  server.begin();
  ets_printf("[B] server started\n");

  if (filterMac.length()) {
    Serial.println("[FILTER] configured MAC: " + filterMac);
  } else {
    Serial.println("[FILTER] no MAC yet, will auto-scan on first use");
  }
}

void setup() {
  ets_printf("\n[BOOT] reset_reason=%d\n", (int)esp_reset_reason());
  Serial.begin(115200);
  ets_printf("[BOOT] serial ready\n");

  // 【核心补丁 5】强制初始化 NVS，确保蓝牙配对密钥断电不丢
  ets_printf("[BOOT] nvs_init begin\n");
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ets_printf("[BOOT] nvs erase\n");
    nvs_flash_erase();
    nvs_flash_init();
  }
  ets_printf("[BOOT] nvs ok ret=%d\n", (int)ret);

  esp_reset_reason_t reason = esp_reset_reason();
  ets_printf("[BOOT] reason=%d magic=%08x\n", (int)reason, (unsigned)boot_mode_magic);
  if (reason != ESP_RST_SW) { boot_mode_magic = 0; }
  ets_printf("[BOOT] after clear magic=%08x\n", (unsigned)boot_mode_magic);

  if (boot_mode_magic == MAGIC_WAKEUP) {
    runWakeupBeaconMode();
  } else {
    runNormalControlMode();
  }
}

void loop() {
  if (boot_mode_magic != MAGIC_WAKEUP) {
    server.handleClient();
    ArduinoOTA.handle();
    filterIdleCheck();
  }
}
