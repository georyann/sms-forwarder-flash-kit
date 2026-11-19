#include "wifi.h"
#include "config/config.h"
#include "http/body_accumulator.h"
#include "http/json_response.h"
#include "wifi/wifi_manager.h"
#include "../../logger/logger.h"
#include <ArduinoJson.h>
#include <WiFi.h>

void wifiGetController(AsyncWebServerRequest* request) {
  JsonDocument doc;
  JsonArray arr = doc["wifiList"].to<JsonArray>();
  for (int i = 0; i < config.wifiCount; i++) {
    JsonObject entry = arr.add<JsonObject>();
    entry["ssid"]     = config.wifiList[i].ssid;
    entry["password"] = "";  // 安全起见，不回显密码
  }
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void wifiScanController(AsyncWebServerRequest* request) {
  // 阻塞扫描附近 WiFi：每信道最多 120ms，整轮大约 1.5~3 秒。
  // 扫描期间 STA 连接会短暂停顿，属于 ESP32 正常行为。
  int n = WiFi.scanNetworks(false /*async*/, false /*show_hidden*/, false /*passive*/, 120);
  if (n < 0) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "扫描失败，请稍后重试";
    doc["networks"].to<JsonArray>();
    String body;
    serializeJson(doc, body);
    request->send(500, "application/json", body);
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();
  // 同一 SSID 可能被多个 AP 广播（漫游 / mesh repeater），这里按 SSID 去重，
  // 只保留信号最强的一条，网页下拉框才不会出现一堆重复项。
  String s_ssid[24];
  int    s_rssi[24] = {0};
  bool   s_sec[24]  = {false};
  int    count = 0;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // 隐藏网络，网页上无法选择
    int rssi = WiFi.RSSI(i);
    bool sec = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);

    int idx = -1;
    for (int j = 0; j < count; j++) {
      if (s_ssid[j] == ssid) { idx = j; break; }
    }
    if (idx >= 0) {
      if (rssi > s_rssi[idx]) { s_rssi[idx] = rssi; s_sec[idx] = sec; }
      continue;
    }
    if (count >= 24) continue;
    s_ssid[count] = ssid;
    s_rssi[count] = rssi;
    s_sec[count]  = sec;
    count++;
  }
  for (int j = 0; j < count; j++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = s_ssid[j];
    o["rssi"] = s_rssi[j];
    o["secure"] = s_sec[j];
  }
  WiFi.scanDelete();  // 释放扫描结果内存

  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void wifiPostController(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  const char* body = nullptr;
  if (!httpAccumulateBody(request, data, len, index, total, HTTP_JSON_BODY_MAX_BYTES, &body)) return;
  if (body == nullptr) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  httpReleaseAccumulatedBody(request);
  if (err || !doc["wifiList"].is<JsonArray>()) {
    JsonResp::err(request, 400, "缺少wifiList数组");
    return;
  }

  JsonArray arr = doc["wifiList"].as<JsonArray>();
  if ((int)arr.size() > MAX_WIFI_ENTRIES) {
    JsonResp::err(request, 400, "wifiList超过最大限制（5条）");
    return;
  }

  int count = 0;
  for (JsonVariant v : arr) {
    if (!v.is<JsonObject>()) continue;
    JsonObject entry = v.as<JsonObject>();
    String ssid = entry["ssid"] | String("");
    if (ssid.length() == 0) continue;
    config.wifiList[count].ssid = ssid;
    String pw = entry["password"] | String("");
    if (pw.length() > 0) config.wifiList[count].password = pw;
    count++;
  }
  config.wifiCount = count;

  ConfigStore::save();
  LOG("HWIFI", "WiFi配置已更新，共 %d 条", config.wifiCount);

  JsonResp::okWithReboot(request, "WiFi配置已保存，设备将在2秒后自动重启");
}

// ---------------------------------------------------------------------------
// 设备自身热点（SoftAP）
//
// 作用：换到新环境、连不上任何已知路由器时的兜底 —— 手机/电脑连上设备自己的
// 热点，就能打开 http://192.168.4.1/ 进管理页重新配网。
// ---------------------------------------------------------------------------

void hotspotGetController(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["ssid"]     = WifiManager::apSsid();
  doc["passSet"]  = config.apPass.length() >= 8;   // 密码不回显，只告知是否已设置
  doc["alwaysOn"] = config.apAlwaysOn;
  doc["active"]   = WifiManager::apActive();       // 此刻是否真的在广播
  doc["apIp"]     = WifiManager::apIp();
  doc["mode"]     = (WifiManager::mode() == WIFI_MODE_AP_ACTIVE) ? "ap" : "sta";
  doc["staIp"]    = WifiManager::ip();
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void hotspotPostController(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  const char* body = nullptr;
  if (!httpAccumulateBody(request, data, len, index, total, HTTP_JSON_BODY_MAX_BYTES, &body)) return;
  if (body == nullptr) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  httpReleaseAccumulatedBody(request);
  if (err) {
    JsonResp::err(request, 400, "JSON 解析失败");
    return;
  }

  // 热点名称：留空表示不改
  if (doc["ssid"].is<const char*>()) {
    String ssid = doc["ssid"].as<String>();
    ssid.trim();
    if (ssid.length() > 0) {
      if (ssid.length() > 32) {
        JsonResp::err(request, 400, "热点名称最长 32 个字符");
        return;
      }
      config.apSsid = ssid;
    }
  }

  // 热点密码：留空表示不改；clearPass=true 表示改成无密码开放热点
  if (doc["clearPass"].is<bool>() && doc["clearPass"].as<bool>()) {
    config.apPass = "";
  } else if (doc["password"].is<const char*>()) {
    String pw = doc["password"].as<String>();
    pw.trim();
    if (pw.length() > 0) {
      if (pw.length() < 8) {
        JsonResp::err(request, 400, "热点密码至少 8 位（WPA2 要求），想用无密码热点请勾选「清除密码」");
        return;
      }
      config.apPass = pw;
    }
  }

  if (doc["alwaysOn"].is<bool>()) {
    config.apAlwaysOn = doc["alwaysOn"].as<bool>();
  }

  ConfigStore::save();
  LOG("HWIFI", "热点配置已更新：SSID=%s，密码=%s，常开=%d",
      config.apSsid.c_str(),
      config.apPass.length() >= 8 ? "已设置" : "无",
      config.apAlwaysOn ? 1 : 0);

  JsonResp::okWithReboot(request, "热点配置已保存，设备将在2秒后自动重启");
}
