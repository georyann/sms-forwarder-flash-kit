#pragma once
#include <Arduino.h>

// SIM 卡到期提醒：可保存多个号码（不限于设备当前插入的卡），
// 到期前按用户勾选的档位（30/15/7/1 天）各推送一次续费提醒。
constexpr int SIM_EXPIRY_MAX_ENTRIES = 10;

// 提醒档位（天数），与 offsetsMask 的 bit 位一一对应
constexpr int SIM_EXPIRY_OFFSETS[4] = {30, 15, 7, 1};

struct SimExpiryEntry {
  String number;      // 号码（任意号码，不要求是插卡号码）
  String expiryDate;  // 到期日期 YYYY-MM-DD
  String note;        // 备注
  uint8_t offsetsMask;   // 勾选的提醒档位：bit0=30天 bit1=15天 bit2=7天 bit3=1天
  uint8_t notifiedMask;  // 已推送过的档位，避免重复提醒
  bool    enabled;       // 是否启用
};

class SimExpiry {
public:
  static void load();
  static void save();

  static int count();
  static const SimExpiryEntry& at(int index);

  // index < 0 时追加，否则覆盖该条。返回 false 表示越界/已满。
  static bool put(int index, const SimExpiryEntry& entry);
  static bool remove(int index);

  // 距到期天数；日期非法返回 -999，已过期返回负数。
  static int daysLeft(const String& expiryDate);

  // 主循环周期调用：时间同步后每天检查一次并推送提醒。
  static void tick();
};
