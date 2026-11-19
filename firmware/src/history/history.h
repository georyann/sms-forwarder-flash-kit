#pragma once
#include <Arduino.h>

// 收件箱：把设备收到的短信/来电落到 LittleFS，供用户在工具箱里手动查看。
//
// 存储格式为 JSON Lines（每行一条 JSON 对象），追加写入，条数超过上限时
// 重写文件只保留最近若干条 —— 避免每次收信都整份重写 Flash。
constexpr int HISTORY_MAX_ITEMS = 50;

class History {
public:
  // 追加一条记录。type: "sms" / "call"；label 为子类型（如「WAP Push」）。
  static void add(const String& sender, const String& content, const String& timestamp,
                  const char* type, const String& label);

  // 已保存条数。
  static int count();

  // JSON 数组文本，最新的排在最前面。
  static String listJson();

  // 清空全部记录。
  static void clear();
};
