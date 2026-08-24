// main/main.c —— 测试版本 2: 用 LVGL 内置默认字体, 用彩色矩形验证渲染
// 不依赖任何外部字体声明

#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "=== test2 start ===");

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "display_init FAILED");
        return;
    }
    ESP_LOGI(TAG, "display OK");

    if (!bsp_lvgl_init()) {
        ESP_LOGE(TAG, "lvgl init FAILED");
        return;
    }
    ESP_LOGI(TAG, "lvgl OK");

    bsp_display_backlight(100);

    // 简化: 完全不创建 label, 只创建彩色矩形测试渲染
    if (bsp_lvgl_lock(2000)) {
        lv_obj_t *scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x0000FF), 0); // 纯蓝
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_screen_load(scr);

        // 红绿黄三个大方块
        lv_obj_t *r1 = lv_obj_create(scr);
        lv_obj_set_size(r1, 100, 100);
        lv_obj_set_pos(r1, 20, 50);
        lv_obj_set_style_bg_color(r1, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_bg_opa(r1, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r1, 0, 0);

        lv_obj_t *r2 = lv_obj_create(scr);
        lv_obj_set_size(r2, 100, 100);
        lv_obj_set_pos(r2, 70, 50);
        lv_obj_set_style_bg_color(r2, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_bg_opa(r2, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r2, 0, 0);

        lv_obj_t *r3 = lv_obj_create(scr);
        lv_obj_set_size(r3, 100, 100);
        lv_obj_set_pos(r3, 120, 50);
        lv_obj_set_style_bg_color(r3, lv_color_hex(0xFFFF00), 0);
        lv_obj_set_style_bg_opa(r3, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r3, 0, 0);

        // 用 LVGL 默认字体 (LV_FONT_DEFAULT)
        lv_obj_t *lab = lv_label_create(scr);
        lv_label_set_text(lab, "TEST 2");
        lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(lab, LV_ALIGN_TOP_MID, 0, 10);

        bsp_lvgl_unlock();
        ESP_LOGI(TAG, "UI shown");
    }

    ESP_LOGI(TAG, "=== app_main done ===");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive");
    }
}
