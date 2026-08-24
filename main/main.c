// main.c —— 拷贝覆盖 ai-passport/main/main.c
//
// 在原有 4 个 demo 注册后,加入 3 个新产品的注册和 console 命令注册。

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
#include "esp_console.h"
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

// 注:为了 7 个 demo 仍然能放进 240x320 菜单 (目前代码是 2 列网格 y=58+86i),
// 这里把 5..6 个 demo 单独放第三行. 如不需要美观可保留原菜单布局不动.
// 原 ui_pixel_panel_create 限制每张卡 102x72, 实际 (7+1)/2 = 4 行,会超出菜单区.
// 简单方案:把 7 个 demo 分两屏 (首页 4 个,翻页看 3 个),
// 或者把它们改成单列纵列展示. 这里选择"单列纵列"。
// 为保持改动最小,只在原网格上加溢出滚动:不再使用 UI_PAPER 卡片,直接一列。

static void menu_refresh(void) {
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        lv_label_set_text_fmt(s_rows[i], "%s%s",
            DEMOS[i].name,
            s_ok[i] ? "" : " [FAIL]");
        // 单列模式下,没有 ui_pixel_set_selected; 直接用边框颜色
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
        // 单列布局: 每行高度 36,起始 y=58
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
    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init fail");
        return;
    }
    bsp_display_backlight(100);

    s_ok[0] = true;
    s_ok[1] = (bsp_button_init(on_key, NULL) == ESP_OK);
    s_ok[2] = (bsp_audio_init() == ESP_OK);
    s_ok[3] = (bsp_battery_init() == ESP_OK);
    // 新 demo 依赖 button + display 已经初始化即可;
    // Companion 仅显示 + console,不需要 audio / battery (但用了 bsp_battery_get_soc 读电量,失败返回 -1,代码已兜底)
    s_ok[4] = s_ok[1]; // Companion
    s_ok[5] = s_ok[1] && s_ok[2]; // Quick 用了录音
    s_ok[6] = s_ok[1] && s_ok[2]; // Flow 用了 beep

    // 初始化 NVS (Quick 和 Flow 需要持久化)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 注册 console 命令
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "passport> ";
    repl_cfg.max_cmdline_length = 256;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl));
#endif
    if (repl) {
        demo_companion_console_register();
        demo_quick_console_register();
        demo_flow_console_register();
        ESP_ERROR_CHECK(esp_console_start_repl(repl));
    }

    if (bsp_lvgl_lock(1000)) { enter_menu(); bsp_lvgl_unlock(); }

    ESP_LOGI(TAG, "ready: D=%d B=%d A=%d Bat=%d C=%d Q=%d F=%d",
        s_ok[0], s_ok[1], s_ok[2], s_ok[3], s_ok[4], s_ok[5], s_ok[6]);
}
