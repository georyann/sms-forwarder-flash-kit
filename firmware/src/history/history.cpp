#include "history.h"
#include "logger/logger.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include <vector>

static const char* kHistoryPath = "/history.jsonl";
static const int   kContentMaxLen = 300;

// 已保存条数；-1 表示尚未统计（首次使用时再读文件）
static int s_count = -1;

static void ensureInit() {
  if (s_count >= 0) return;
  s_count = 0;
  if (!LittleFS.exists(kHistoryPath)) return;
  File f = LittleFS.open(kHistoryPath, "r");
  if (!f) return;
  while (f.available()) {
    if (f.read() == '\n') s_count++;
  }
  f.close();
}

// 读出全部记录行（按写入顺序）
static std::vector<String> readAllLines() {
  std::vector<String> lines;
  File f = LittleFS.open(kHistoryPath, "r");
  if (!f) return lines;
  String cur;
  while (f.available()) {
    char c = (char)f.read();
    if (c == '\n') {
      if (cur.length() > 0) lines.push_back(cur);
      cur = "";
    } else {
      cur += c;
    }
  }
  if (cur.length() > 0) lines.push_back(cur);
  f.close();
  return lines;
}

// 条数超限时重写文件，只保留最近 HISTORY_MAX_ITEMS 条
static void compact() {
  std::vector<String> lines = readAllLines();
  if ((int)lines.size() <= HISTORY_MAX_ITEMS) {
    s_count = (int)lines.size();
    return;
  }
  File f = LittleFS.open(kHistoryPath, "w");
  if (!f) return;
  for (size_t i = lines.size() - HISTORY_MAX_ITEMS; i < lines.size(); i++) {
    f.println(lines[i]);
  }
  f.close();
  s_count = HISTORY_MAX_ITEMS;
  LOG("HST", "历史记录已裁剪，保留最近 %d 条", HISTORY_MAX_ITEMS);
}

void History::add(const String& sender, const String& content, const String& timestamp,
                  const char* type, const String& label) {
  ensureInit();

  JsonDocument doc;
  doc["sender"]    = sender;
  doc["content"]   = content.substring(0, kContentMaxLen);
  doc["timestamp"] = timestamp;
  doc["type"]      = type;
  doc["label"]     = label;
  doc["ts"]        = (long long)time(nullptr);

  String line;
  serializeJson(doc, line);

  File f = LittleFS.open(kHistoryPath, "a");
  if (!f) {
    LOG("HST", "历史记录写入失败");
    return;
  }
  f.println(line);
  f.close();

  s_count++;
  if (s_count > HISTORY_MAX_ITEMS) compact();
}

int History::count() {
  ensureInit();
  return s_count;
}

String History::listJson() {
  ensureInit();
  std::vector<String> lines = readAllLines();
  String out = "[";
  for (int i = (int)lines.size() - 1; i >= 0; i--) {
    if (i != (int)lines.size() - 1) out += ",";
    out += lines[i];
  }
  out += "]";
  return out;
}

void History::clear() {
  ensureInit();
  LittleFS.remove(kHistoryPath);
  s_count = 0;
  LOG("HST", "历史记录已清空");
}
