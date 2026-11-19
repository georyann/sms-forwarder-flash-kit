#pragma once
#include <Arduino.h>
#include <esp_task_wdt.h>

// ---------------------------------------------------------------------------
// 安全喂狗封装
//
// 本板（ESP32-C3 + Arduino-ESP32 3.3.x）的 IDF 配置里：
//   - CONFIG_ESP_TASK_WDT_INIT=y，但 CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 未开启
//   - Arduino loopTask 也没有被订阅进 TWDT
// 因此 esp_task_wdt_reset() 一定会返回 ESP_ERR_NOT_FOUND。而 IDF 的 task_wdt
// 组件用编译期 LOG_LOCAL_LEVEL 打印这条错误，运行时 esp_log_level_set("task_wdt",
// ESP_LOG_NONE) 关不掉，串口会一直刷：
//     E (292) task_wdt: esp_task_wdt_reset(707): task not found
// 这些调用本来就是空操作（没有任务被监控），所以这里直接置空：
// 既不刷屏，也不改变看门狗行为。
//
// 若换到别的板子（空闲任务被监控 / loopTask 已订阅），仍然走真实的喂狗调用，
// 避免因为漏喂狗导致复位。
// ---------------------------------------------------------------------------
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 || !CONFIG_FREERTOS_UNICORE
#define APP_WDT_NEEDS_REAL_FEED 1
#endif

inline void wdtResetSafe() {
#ifdef APP_WDT_NEEDS_REAL_FEED
  esp_task_wdt_reset();
#endif
}

// 在 esp_task_wdt.h 之后定义宏，把本工程里所有 esp_task_wdt_reset() 调用
// 重定向到上面的安全封装（不改 IDF 头文件本身，因为宏定义在 include 之后）。
#define esp_task_wdt_reset() wdtResetSafe()
