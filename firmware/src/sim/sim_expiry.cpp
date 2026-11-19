#include "sim_expiry.h"
#include "push/push.h"
#include "time/time_sync.h"
#include "logger/logger.h"
#include "common/nvs_helper.h"
#include <ArduinoJson.h>
#include <time.h>

static const char* kNs  = "sim_expiry";
static const char* kKey = "list";

static SimExpiryEntry s_entries[SIM_EXPIRY_MAX_ENTRIES];
static int            s_count = 0;
static unsigned long  s_lastCheckMs = 0;
static bool           s_checked = false;

// 检查间隔：5 分钟一次（档位按天判定，间隔只需保证不漏掉跨天的那次检查）
static const unsigned long kCheckIntervalMs = 5UL * 60UL * 1000UL;

// 剩余天数 → 提醒档位（bit 位）。返回 -1 表示当前不触发任何档位。
// 分档而不是「≤阈值即触发」，否则同一天会连推 30/15/7 天多条。
static int bandForDays(int days) {
  if (days < 0)   return -1;  // 已过期
  if (days > 30)  return -1;
  if (days > 15)  return 0;   // 提前 30 天
  if (days > 7)   return 1;   // 提前 15 天
  if (days > 1)   return 2;   // 提前 7 天
  return 3;                   // 提前 1 天（含到期当天）
}

void SimExpiry::load() {
  s_count = 0;
  // millis() 是开机以来的时间，setup() 执行时已经跑了几分钟，
  // 所以不能拿它做「立即检查」的基准，改用独立的 s_checked 标记。
  s_checked = false;
  s_lastCheckMs = 0;

  String raw = Nvs::getStr(kNs, kKey, "");
  if (raw.length() == 0) return;

  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) {
    LOG("SEXP", "到期提醒数据解析失败，已忽略");
    return;
  }
  JsonArray arr = doc["entries"].as<JsonArray>();
  for (JsonVariant v : arr) {
    if (s_count >= SIM_EXPIRY_MAX_ENTRIES) break;
    SimExpiryEntry e;
    e.number       = v["number"] | "";
    e.expiryDate   = v["expiryDate"] | "";
    e.note         = v["note"] | "";
    e.offsetsMask  = (uint8_t)(v["offsetsMask"] | 0);
    e.notifiedMask = (uint8_t)(v["notifiedMask"] | 0);
    e.enabled      = v["enabled"] | true;
    if (e.number.length() == 0 || e.expiryDate.length() == 0) continue;
    s_entries[s_count++] = e;
  }
  LOG("SEXP", "已加载 %d 条 SIM 到期提醒", s_count);
}

void SimExpiry::save() {
  JsonDocument doc;
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (int i = 0; i < s_count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["number"]       = s_entries[i].number;
    o["expiryDate"]   = s_entries[i].expiryDate;
    o["note"]         = s_entries[i].note;
    o["offsetsMask"]  = s_entries[i].offsetsMask;
    o["notifiedMask"] = s_entries[i].notifiedMask;
    o["enabled"]      = s_entries[i].enabled;
  }
  String out;
  serializeJson(doc, out);

  NvsScope scope(kNs);
  if (!scope.ok()) {
    LOG("SEXP", "NVS 打开失败，到期提醒未能保存");
    return;
  }
  scope->putString(kKey, out);
  LOG("SEXP", "已保存 %d 条到期提醒", s_count);
}

int SimExpiry::count() {
  return s_count;
}

const SimExpiryEntry& SimExpiry::at(int index) {
  static SimExpiryEntry dummy;
  if (index < 0 || index >= s_count) {
    dummy = SimExpiryEntry();
    return dummy;
  }
  return s_entries[index];
}

bool SimExpiry::put(int index, const SimExpiryEntry& entry) {
  if (index < 0) {
    if (s_count >= SIM_EXPIRY_MAX_ENTRIES) return false;
    s_entries[s_count++] = entry;
    return true;
  }
  if (index >= s_count) return false;
  s_entries[index] = entry;
  return true;
}

bool SimExpiry::remove(int index) {
  if (index < 0 || index >= s_count) return false;
  for (int i = index; i < s_count - 1; i++) {
    s_entries[i] = s_entries[i + 1];
  }
  s_count--;
  s_entries[s_count] = SimExpiryEntry();
  return true;
}

int SimExpiry::daysLeft(const String& expiryDate) {
  int y = 0, m = 0, d = 0;
  if (sscanf(expiryDate.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return -999;
  if (y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return -999;

  struct tm tmExp = {};
  tmExp.tm_year  = y - 1900;
  tmExp.tm_mon   = m - 1;
  tmExp.tm_mday  = d;
  tmExp.tm_hour  = 12;   // 取正午，避开夏令时切换导致的差一天
  tmExp.tm_isdst = -1;
  time_t exp = mktime(&tmExp);
  if (exp == (time_t)-1) return -999;

  time_t now = time(nullptr);
  struct tm tmNow = {};
  localtime_r(&now, &tmNow);
  tmNow.tm_hour  = 12;
  tmNow.tm_min   = 0;
  tmNow.tm_sec   = 0;
  tmNow.tm_isdst = -1;
  time_t today = mktime(&tmNow);
  if (today == (time_t)-1) return -999;

  return (int)((exp - today) / 86400);
}

void SimExpiry::tick() {
  if (s_count == 0) return;
  if (!TimeSync::isSynced()) return;   // 没时间就没法算剩余天数

  unsigned long now = millis();
  // 时间刚同步成功后立刻查一次，之后每 kCheckIntervalMs 查一次
  if (s_checked && now - s_lastCheckMs < kCheckIntervalMs) return;
  s_lastCheckMs = now;
  s_checked = true;

  bool changed = false;
  for (int i = 0; i < s_count; i++) {
    SimExpiryEntry& e = s_entries[i];
    if (!e.enabled) continue;

    int days = daysLeft(e.expiryDate);
    if (days == -999 || days < 0) continue;

    int band = bandForDays(days);
    if (band < 0) continue;
    if (!(e.offsetsMask & (1 << band))) continue;
    if (e.notifiedMask & (1 << band)) continue;

    e.notifiedMask |= (1 << band);
    changed = true;

    String msg = "🔔 SIM卡到期提醒\n";
    msg += "📱 号码: " + e.number + "\n";
    msg += "📅 到期时间: " + e.expiryDate + "（还剩 " + String(days) + " 天）\n";
    if (e.note.length() > 0) msg += "📝 备注: " + e.note + "\n";
    msg += "💡 请记得及时续费，避免号码停机";

    Push::send(e.number, msg, TimeSync::dateStr(), MsgTypeInfo(MSG_TYPE_SIM, "SIM到期提醒"));
    LOG("SEXP", "已推送到期提醒: %s（还剩 %d 天）", e.number.c_str(), days);
  }

  if (changed) save();
}
