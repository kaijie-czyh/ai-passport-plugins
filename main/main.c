// main/main.c —— 测试 3: 直接复用 BSP 原版 ui_pixel_screen_create, 看 menu 是否显示
//
// 这个测试目的是确认 BSP + LVGL + ui_pixel 三者都正常工作.
// 如果能看到 "FoloToy" 标题 + 4 个 demo 卡片, 说明一切 OK.

#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_pixel.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "=== test3: original menu ===");

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init fail");
        return;
    }
    ESP_LOGI(TAG, "display + lvgl OK");
    bsp_display_backlight(100);

    if (bsp_lvgl_lock(2000)) {
        lv_obj_t *scr = ui_pixel_screen_create("FoloToy");

        // 测试 3 个 label: 用 BSP 已验证的 ui_pixel_label (字体肯定可用)
        lv_obj_t *l1 = ui_pixel_label(scr, "HELLO",
                                       &lv_font_montserrat_20, 0xFFFFFF);
        lv_obj_set_pos(l1, 80, 100);

        lv_obj_t *l2 = ui_pixel_label(scr, "WORLD",
                                       &lv_font_montserrat_20, 0xFFD166);
        lv_obj_set_pos(l2, 80, 140);

        lv_obj_t *l3 = ui_pixel_label(scr, "DIY OK!",
                                       &lv_font_montserrat_20, 0x00FF00);
        lv_obj_set_pos(l3, 80, 180);

        lv_screen_load(scr);
        bsp_lvgl_unlock();
        ESP_LOGI(TAG, "UI loaded");
    }

    ESP_LOGI(TAG, "=== app_main done ===");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive");
    }
}
