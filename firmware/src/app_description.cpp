// IDF 5.5 起 esp_app_desc_t 从 esp_app_format.h 移到独立的 esp_app_desc.h，
// 且结构体尾部新增了 efuse 版本 / mmu_page_size 字段。
// 用指定初始化器（C++20）写，避免字段顺序随 IDF 版本变化而错位。
#include <esp_app_desc.h>
#include "config/config.h"

// extern "C" directly on definition (not inside a block) gives true external
// C linkage — overriding the arduino-lib-builder copy in libapp_update.a.
// -Wl,--allow-multiple-definition in platformio.ini lets the linker accept
// both definitions and keep the first one found (this .o before the archive).
extern "C" const __attribute__((section(".rodata_desc")))
__attribute__((aligned(16)))
esp_app_desc_t esp_app_desc = {
    .magic_word    = ESP_APP_DESC_MAGIC_WORD,
    .secure_version = 0,
    .reserv1       = {0, 0},
    .version       = APP_VERSION,
    .project_name  = APP_NAME " by " APP_AUTHOR,
    .time          = APP_BUILD_TIME,
    .date          = APP_BUILD_DATE,
    .idf_ver       = IDF_VER,
    .app_elf_sha256 = {0},
    .min_efuse_blk_rev_full = 0,
    .max_efuse_blk_rev_full = 0,
    .mmu_page_size = 0,
    .reserv3       = {0, 0, 0},
    .reserv2       = {0},
};
