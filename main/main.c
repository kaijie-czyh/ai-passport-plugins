// main/main.c —— 简化版: 只跑 LVGL 菜单 + 7 个 demo, 不开 esp_console
//
// 按键语义(全局统一):
// 上/下 短按 菜单中=移动选中项;演示页中=该页自定义
// 确定 短按 菜单中=进入选中项;演示页中=该页自定义
// 确定 长按 演示页中=返回菜单(由本文件统一拦截)

#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "demo.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "main";

static const demo_entry_t DEMOS[] = {
    { "Display",  demo_display_enter,  demo_display_exit,  demo_display_key  },
    { "Button",   demo_button_enter,   demo_button_exit,   demo_button_key   },
    { "Audio",    demo_audio_enter,    demo_audio_exit,    demo_audio_key    },
    { "Battery",  demo_battery_enter,  demo_battery_exit,  demo_battery_key  },
    { "Companion",demo_companion_enter,demo_companion_exit,demo_companion_key},
    { "Quick",    demo_quick_enter,    demo_quick_exit,    demo_quick_key    },
    { "Flow",     demo_flow_enter,     demo_flow_exit,     demo_flow_key     },
};

#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))

static bool s_ok[DEMO_COUNT];
static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[DEMO_COUNT];
static lv_obj_t *s_rows[DEMO_COUNT];
static lv_obj_t *s_mascot;
static int s_sel;
static int s_active = -1;

static void menu_refresh(void) {
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        lv_label_set_text_fmt(s_rows[i], "%s%s",
            DEMOS[i].name,
            s_ok[i] ? "" : " [FAIL]");
        lv_obj_set_style_border_color(s_cards[i],
            lv_color_hex((int)i == s_sel ? 0xFFD166 : 0x404040), 0);
        lv_obj_set_style_border_width(s_cards[i], (int)i == s_sel ? 3 : 1, 0);
        lv_obj_set_style_text_color(s_rows[i],
            s_ok[i] ? lv_color_hex(0xE6E6E6) : lv_color_hex(0x7A2020), 0);
    }
}

static void menu_build(void) {
    s_menu_scr = ui_pixel_screen_create("FoloToy");
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int y = 58 + (int)i * 38;
        s_cards[i] = ui_pixel_panel_create(s_menu_scr, 11, y, 218, 32, 0x1A1F25);
        s_rows[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_rows[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(s_rows[i], LV_ALIGN_LEFT_MID, 8, 0);
    }
    s_mascot = ui_pixel_mascot_create(s_menu_scr, 101, 290);
    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void enter_menu(void) {
    s_active = -1;
    menu_build();
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;
    if (s_active >= 0) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            DEMOS[s_active].exit();
            enter_menu();
        } else {
            DEMOS[s_active].key(btn, ev);
        }
    } else if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP)   { s_sel = (s_sel + DEMO_COUNT - 1) % DEMO_COUNT; menu_refresh(); }
        if (btn == BSP_BTN_DOWN) { s_sel = (s_sel + 1) % DEMO_COUNT; menu_refresh(); }
        if (btn == BSP_BTN_OK && s_ok[s_sel]) {
            s_active = s_sel;
            ui_pixel_mascot_jump(s_mascot);
            lv_obj_delete(s_menu_scr);
            s_menu_scr = NULL;
            s_mascot = NULL;
            DEMOS[s_active].enter();
        } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            ui_pixel_mascot_jump(s_mascot);
        }
    }
    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy-Card + 3 plugin demos");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs: erasing (%s)", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_LOGI(TAG, "nvs init: %s", esp_err_to_name(err));

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init fail");
        return;
    }
    ESP_LOGI(TAG, "display ok");
    bsp_display_backlight(100);

    s_ok[0] = true;
    s_ok[1] = (bsp_button_init(on_key, NULL) == ESP_OK);
    s_ok[2] = (bsp_audio_init() == ESP_OK);
    s_ok[3] = (bsp_battery_init() == ESP_OK);
    s_ok[4] = s_ok[1];
    s_ok[5] = s_ok[1] && s_ok[2];
    s_ok[6] = s_ok[1] && s_ok[2];

    if (bsp_lvgl_lock(1000)) { enter_menu(); bsp_lvgl_unlock(); }

    ESP_LOGI(TAG, "ready: D=%d B=%d A=%d Bat=%d C=%d Q=%d F=%d",
        s_ok[0], s_ok[1], s_ok[2], s_ok[3], s_ok[4], s_ok[5], s_ok[6]);
}
