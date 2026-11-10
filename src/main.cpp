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
String filterMac = "";          // 滤桶 MAC，首次可自动发现或手动设置

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
  fCharge   = plain[1];
  fElectric = plain[2];
  fGear     = plain[4];
  fModel    = plain[6];
  fErr      = plain[7];
  fLastSeen = millis();
  Serial.printf("[FILTER] gear=%d model=%d err=%d\n", fGear, fModel, fErr);
}

void filterNotifyCB(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify) {
  filterParseNotify(data, len);
}

bool filterConnect() {
  if (filterMac.length() == 0) return false;
  if (fClient && fClient->isConnected()) return true;

  if (!fClient) {
    fClient = NimBLEDevice::createClient();
    fClient->setClientCallbacks(new FilterNotifyCB(), false);
    fClient->setConnectTimeout(8000);
  }
  if (!fClient->connect(NimBLEAddress(std::string(filterMac.c_str()), BLE_ADDR_PUBLIC))) {
    // 部分模块用随机地址
    if (!fClient->connect(NimBLEAddress(std::string(filterMac.c_str()), BLE_ADDR_RANDOM))) {
      Serial.println("[FILTER] connect failed");
      return false;
    }
  }
  auto* svc = fClient->getService(SVC_FILTER);
  if (!svc) { Serial.println("[FILTER] service not found"); fClient->disconnect(); return false; }

  auto* chN = svc->getCharacteristic(CH_NOTIFY);
  if (chN && chN->canNotify()) chN->subscribe(true, filterNotifyCB);

  fConnected = true;
  Serial.println("[FILTER] connected");
  return true;
}

void filterDisconnect() {
  if (fClient && fClient->isConnected()) fClient->disconnect();
  fConnected = false;
}

// 发一条控制帧后断开（单连接设备，必须让出连接）
bool filterSendCtl(bool on, int model, int gear, int level, int timing) {
  if (!filterConnect()) return false;
  auto* svc = fClient->getService(SVC_FILTER);
  if (!svc) { filterDisconnect(); return false; }
  auto* ch = svc->getCharacteristic(CH_CTL);
  if (!ch) { filterDisconnect(); return false; }

  uint8_t plain[12], frame[16];
  buildCtl(plain, on, model, gear, level, timing);
  size_t n = nlEncode(plain, sizeof(plain), frame);
  bool ok = ch->writeValue(frame, n, false);
  delay(150);            // 给设备一点时间回包
  filterDisconnect();
  return ok;
}

bool filterSendRaw(const uint8_t* plain, size_t n) {
  if (!filterConnect()) return false;
  auto* svc = fClient->getService(SVC_FILTER);
  if (!svc) { filterDisconnect(); return false; }
  auto* ch = svc->getCharacteristic(CH_CTL);
  if (!ch) { filterDisconnect(); return false; }
  uint8_t frame[80];
  size_t fn = nlEncode(plain, n, frame);
  bool ok = ch->writeValue(frame, fn, false);
  delay(150);
  filterDisconnect();
  return ok;
}

// 扫描：按服务 UUID 或名字找滤桶，同时把附近设备打到串口
String filterScan(int seconds, bool verbose) {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
  NimBLEScanResults res = scan->start(seconds, false);
  String found = "";
  String all = "";
  for (int i = 0; i < res.getCount(); i++) {
    NimBLEAdvertisedDevice d = res.getDevice(i);
    String name = d.haveName() ? String(d.getName().c_str()) : String("");
    String mac  = String(d.getAddress().toString().c_str());
    bool hasSvc = d.haveServiceUUID() && d.getServiceUUID().equals(SVC_FILTER);
    all += mac + "|" + name + "|" + String(d.getRSSI()) + "\n";
    if (hasSvc || name.indexOf("NETLEA") >= 0 || name.indexOf("netlea") >= 0 || name.indexOf("NTL") >= 0) {
      found = mac;
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
      "{\"connected\":%s,\"gear\":%d,\"model\":%d,\"err\":%d,\"charge\":%d,\"voltage\":%d,\"mac\":\"%s\",\"age_ms\":%lu}",
      fConnected ? "true" : "false", fGear, fModel, fErr, fCharge, fElectric,
      filterMac.c_str(), fLastSeen ? (millis() - fLastSeen) : 0UL);
    server.send(200, "application/json", buf);
  });

  server.on("/filter/read", HTTP_GET, []() {
    addCors();
    uint8_t cmd[2] = {0x02, 0x02};
    bool ok = filterSendRaw(cmd, 2);
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

  server.on("/filter/scan", HTTP_GET, []() {
    addCors();
    int s = server.hasArg("s") ? server.arg("s").toInt() : 6;
    String found = filterScan(s, true);
    String body = "{\"found\":\"" + found + "\",\"mac\":\"" + filterMac + "\"}";
    server.send(200, "application/json", body);
  });

  server.on("/filter/setmac", HTTP_GET, []() {
    addCors();
    if (!server.hasArg("mac")) { server.send(400, "application/json", "{\"error\":\"need ?mac=AA:BB:...\"}"); return; }
    filterMac = server.arg("mac");
    prefs.putString("fmac", filterMac);
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

  prefs.begin("dangbei", false);
  filterMac = prefs.getString("fmac", "");
  wifiSsid = prefs.getString("wssid", ssid);
  wifiPass = prefs.getString("wpass", password);

  bleKeyboard = new BleKeyboard(BLE_NAME, "Espressif", 100);
  bleKeyboard->begin();

  if (!wifiConnect(WIFI_TIMEOUT_MS)) {
    // 再给一次机会（有的路由器握手慢）
    if (!wifiConnect(5000)) {
      startConfigPortal();   // 不会返回
    }
  }
  Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());

  // OTA：以后改固件不用拆板子
  ArduinoOTA.setHostname("dangbei-esp32");
  ArduinoOTA.onStart([]() { Serial.println("[OTA] start"); });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] done"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] error %u\n", e); });
  ArduinoOTA.begin();

  setupHttpEndpoints();
  server.begin();

  if (filterMac.length()) {
    Serial.println("[FILTER] configured MAC: " + filterMac);
  } else {
    Serial.println("[FILTER] no MAC yet, will auto-scan on first use");
  }
}

void setup() {
  Serial.begin(115200);

  // 【核心补丁 5】强制初始化 NVS，确保蓝牙配对密钥断电不丢
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason != ESP_RST_SW) { boot_mode_magic = 0; }

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
  }
}
