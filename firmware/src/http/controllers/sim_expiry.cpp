#include "sim_expiry.h"
#include "sim/sim_expiry.h"
#include "sim/sim.h"
#include "http/body_accumulator.h"
#include "http/json_response.h"
#include "logger/logger.h"
#include <ArduinoJson.h>
#include <time.h>

// 今天日期 YYYY-MM-DD
static String todayStr() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}

void simExpiryGetController(AsyncWebServerRequest* request) {
  JsonDocument doc;
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (int i = 0; i < SimExpiry::count(); i++) {
    const SimExpiryEntry& e = SimExpiry::at(i);
    JsonObject o = arr.add<JsonObject>();
    o["number"]       = e.number;
    o["expiryDate"]   = e.expiryDate;
    o["note"]         = e.note;
    o["offsetsMask"]  = e.offsetsMask;
    o["notifiedMask"] = e.notifiedMask;
    o["enabled"]      = e.enabled;
    o["daysLeft"]     = SimExpiry::daysLeft(e.expiryDate);
  }

  JsonArray offsets = doc["offsets"].to<JsonArray>();
  for (int i = 0; i < 4; i++) {
    JsonObject o = offsets.add<JsonObject>();
    o["days"] = SIM_EXPIRY_OFFSETS[i];
    o["bit"]  = i;
  }

  doc["count"]         = SimExpiry::count();
  doc["max"]           = SIM_EXPIRY_MAX_ENTRIES;
  doc["currentNumber"] = Sim::phoneNum();
  doc["today"]         = todayStr();

  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void simExpiryPostController(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                             size_t index, size_t total) {
  const char* body = nullptr;
  if (!httpAccumulateBody(request, data, len, index, total, HTTP_JSON_BODY_MAX_BYTES, &body)) return;
  if (body == nullptr) return;

  JsonDocument doc;
  bool parseFailed = deserializeJson(doc, body) != DeserializationError::Ok || !doc.is<JsonObject>();
  httpReleaseAccumulatedBody(request);
  if (parseFailed) {
    JsonResp::err(request, 400, "请求格式错误");
    return;
  }

  String number = doc["number"] | "";
  String expiryDate = doc["expiryDate"] | "";
  String note = doc["note"] | "";
  int  targetIndex = doc["index"] | -1;
  bool enabled = doc["enabled"] | true;

  number.trim();
  expiryDate.trim();
  note.trim();

  if (number.length() == 0) {
    JsonResp::err(request, 400, "请填写要提醒的号码");
    return;
  }
  if (number.length() > 32) {
    JsonResp::err(request, 400, "号码最长 32 个字符");
    return;
  }
  if (SimExpiry::daysLeft(expiryDate) == -999) {
    JsonResp::err(request, 400, "到期日期格式不正确，请使用 YYYY-MM-DD");
    return;
  }

  uint8_t mask = 0;
  if (doc["offsets"].is<JsonArray>()) {
    for (JsonVariant v : doc["offsets"].as<JsonArray>()) {
      int days = v.as<int>();
      for (int i = 0; i < 4; i++) {
        if (SIM_EXPIRY_OFFSETS[i] == days) mask |= (1 << i);
      }
    }
  } else {
    mask = (uint8_t)(doc["offsetsMask"] | 0);
  }

  SimExpiryEntry entry;
  entry.number       = number;
  entry.expiryDate   = expiryDate;
  entry.note         = note;
  entry.offsetsMask  = mask;
  entry.notifiedMask = 0;
  entry.enabled      = enabled;

  if (targetIndex >= 0) {
    if (targetIndex >= SimExpiry::count()) {
      JsonResp::err(request, 400, "记录不存在，请刷新后重试");
      return;
    }
    const SimExpiryEntry& old = SimExpiry::at(targetIndex);
    if (old.expiryDate == expiryDate) {
      // 日期没变就保留「已提醒」状态，避免改个备注就把提醒重新推一遍
      entry.notifiedMask = old.notifiedMask;
    }
    SimExpiry::put(targetIndex, entry);
    LOG("SEXP", "已更新到期提醒: %s 到期 %s", number.c_str(), expiryDate.c_str());
  } else {
    if (SimExpiry::count() >= SIM_EXPIRY_MAX_ENTRIES) {
      JsonResp::err(request, 400, "最多保存 10 条到期提醒，请先删除不用的");
      return;
    }
    SimExpiry::put(-1, entry);
    LOG("SEXP", "已新增到期提醒: %s 到期 %s", number.c_str(), expiryDate.c_str());
  }

  SimExpiry::save();
  JsonResp::ok(request, targetIndex >= 0 ? "到期提醒已更新" : "到期提醒已添加");
}

void simExpiryDeleteController(AsyncWebServerRequest* request) {
  int targetIndex = request->hasParam("index") ? request->getParam("index")->value().toInt() : -1;
  if (targetIndex < 0 || targetIndex >= SimExpiry::count()) {
    JsonResp::err(request, 400, "记录不存在，请刷新后重试");
    return;
  }
  SimExpiry::remove(targetIndex);
  SimExpiry::save();
  JsonResp::ok(request, "已删除该到期提醒");
}
