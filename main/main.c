// main/main.c —— 最简化版本: 只验证 BSP 启动 + LVGL 显示一行字
// 用于诊断 app 启动崩溃问题

#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "=== minimal app start ===");

    // 1) I2C
    ESP_LOGI(TAG, "step 1: i2c");
    bsp_i2c_init();
    bsp_i2c_scan();

    // 2) Display
    ESP_LOGI(TAG, "step 2: display init");
    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "display_init FAILED");
        return;
    }
    ESP_LOGI(TAG, "step 2: display init OK");

    // 3) LVGL
    ESP_LOGI(TAG, "step 3: lvgl init");
    if (!bsp_lvgl_init()) {
        ESP_LOGE(TAG, "lvgl init FAILED");
        return;
    }
    ESP_LOGI(TAG, "step 3: lvgl init OK");

    // 4) Backlight
    ESP_LOGI(TAG, "step 4: backlight");
    bsp_display_backlight(100);
    ESP_LOGI(TAG, "step 4: backlight OK");

    // 5) 创建一个简单的 LVGL 标签
    ESP_LOGI(TAG, "step 5: lvgl UI");
    if (bsp_lvgl_lock(1000)) {
        lv_obj_t *scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
        lv_obj_t *label = lv_label_create(scr);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_label_set_text(label, "HELLO\nFROM DIY");
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        lv_screen_load(scr);
        bsp_lvgl_unlock();
        ESP_LOGI(TAG, "step 5: UI shown");
    }

    ESP_LOGI(TAG, "=== app_main done, sleeping ===");

    // 主循环: 每 5 秒打一条日志, 防止看门狗
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive, uptime=%lld", esp_timer_get_time() / 1000000);
    }
}
