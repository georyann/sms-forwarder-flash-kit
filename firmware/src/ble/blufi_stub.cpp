// BLE(BluFi) 配网的降级实现。
//
// 原因：PlatformIO 的 espressif32 平台在 ESP32-C3 上只提供 BLE 控制器库
// （libbt.a），不含 Bluedroid 协议栈（esp_gap_ble_api.h / esp_blufi_api.h），
// 因此原始的 src/ble/blufi.cpp 无法编译。
//
// 这里用空实现替换：AP 热点 + 网页配网是等价的替代路径
// （连上 "SMS-Forwarder-AP" 后浏览器打开 http://192.168.4.1 配置 WiFi），
// 短信转发等核心功能不受影响。
#include "blufi.h"
#include "../logger/logger.h"

void Blufi::init() {
  LOG("BLUFI", "BLE 配网未启用（当前固件未编译 Bluedroid），请用 AP 热点网页配网");
}

void Blufi::deinit() {}
