#include "time_sync.h"
#include "../logger/logger.h"
#include "sim/sim_dispatcher.h"
#include <time.h>
#include <sys/time.h>

// 公历日期 → 天数（Howard Hinnant 算法），避免依赖 timegm（ESP32 newlib 不一定有）
static long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

bool          TimeSync::s_synced = false;
unsigned long TimeSync::s_simRetryNext = 0;

void TimeSync::init() {
  setenv("TZ", "UTC-8", 1);
  tzset();
  LOG("TIME", "时间模块已初始化（默认时区 UTC+8）");
}

void TimeSync::syncFromSIM() {
  String resp;
  bool ok = SimDispatcher::sendCommand("AT+CCLK?", 3000, &resp, false);
  if (!ok && resp.indexOf("+CCLK:") < 0) {
    LOG("TIME", "AT+CCLK? 无响应，跳过 SIM 时间同步");
    return;
  }

  int cclkIdx = resp.indexOf("+CCLK:");
  int q1 = resp.indexOf('"', cclkIdx);
  int q2 = resp.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 <= q1) {
    LOG("TIME", "CCLK 格式解析失败");
    return;
  }
  String ts = resp.substring(q1 + 1, q2);

  int yy, mo, dd, hh, mi, ss, tz = 0;
  int parsed = sscanf(ts.c_str(), "%d/%d/%d,%d:%d:%d%d", &yy, &mo, &dd, &hh, &mi, &ss, &tz);
  if (parsed < 6) {
    LOG("TIME", "CCLK 时间字段解析失败，原始: %s", ts.c_str());
    return;
  }

  if (yy < 0 || yy > 99 || mo < 1 || mo > 12 || dd < 1 || dd > 31 ||
      hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 60) {
    LOG("TIME", "CCLK 时间字段越界，原始: %s", ts.c_str());
    return;
  }
  if (tz < -56 || tz > 56) {
    LOG("TIME", "CCLK 时区字段越界 (%d)，按 0 处理", tz);
    tz = 0;
  }

  // ML307A 的 +CCLK 给出的是 UTC 挂钟时间，tz 字段（单位 15 分钟）单独表示
  // 本地时区偏移。这里按 UTC 解释挂钟字段，偏移只用于设置 TZ 显示，不能再减一次
  // （早先 mktime() 之后又减一遍 tz，导致整机时间整整差 8 小时）。
  time_t utcTime = (time_t)daysFromCivil(2000 + yy, mo, dd) * 86400 +
                   (time_t)hh * 3600 + (time_t)mi * 60 + ss;

  struct timeval tv = { utcTime, 0 };
  settimeofday(&tv, nullptr);

  int tzHours = tz / 4;
  char tzStr[16];
  snprintf(tzStr, sizeof(tzStr), "UTC%+d", -tzHours);
  setenv("TZ", tzStr, 1);
  tzset();

  s_synced = true;
  LOG("TIME", "SIM时间同步成功: %s, 时区偏移: %d (UTC%+d)", ts.c_str(), tz, tzHours);
}

void TimeSync::syncNTP() {
  configTime(8 * 3600, 0, "pool.ntp.org", "ntp.ntsc.ac.cn", "ntp.aliyun.com");
  setenv("TZ", "UTC-8", 1);
  tzset();

  int retries = 0;
  while (time(nullptr) < 1000000 && retries < 100) {
    delay(100);
    retries++;
  }
  if (time(nullptr) >= 1000000) {
    s_synced = true;
    LOG("TIME", "NTP时间同步成功，UTC+8: %ld", (long)time(nullptr));
  } else {
    LOG("TIME", "NTP时间同步超时");
  }
}

bool TimeSync::isSynced() { return s_synced; }

void TimeSync::tick() {
  if (s_synced) {
    return;
  }
  if (millis() < s_simRetryNext) {
    return;
  }
  LOG("TIME", "SIM 时间重试...");
  syncFromSIM();
  if (time(nullptr) >= 1000000) {
    s_synced = true;
    LOG("TIME", "SIM 时间重试成功，停止重试");
  } else {
    s_simRetryNext = millis() + 10000;
  }
}

String TimeSync::dateStr() {
  time_t now = time(nullptr);
  if (now < 1000000) {
    return "未知";
  }
  struct tm t;
  localtime_r(&now, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S%z", &t);
  return String(buf);
}
