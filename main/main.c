// main/main.c —— AI Passport 自定义固件主入口
// 7 项菜单 (Display / Button / Audio / Battery / Companion / Quick / Flow) + esp_console 控制台.
//
// 与 ai-passport 官方 main.c 关键差异:
//  - DEMOS[] 多了 3 项 (Companion/Quick/Flow)
//  - 多注册 esp_console REPL, 暴露 quick tasks/companion push/flow 命令
//  - 按键事件名按 BSP 头文件用 BSP_BTN_CLICK / BSP_BTN_LONG

#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "lvgl.h"
#include "ui_pixel.h"
#include "esp_log.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"

#include "demo.h"

static const char *TAG = "main";

typedef struct {
    const char *name;
    bool       *ok_flag;
    void (*enter)(void);
    void (*exit)(void);
    void (*key)(bsp_btn_t, bsp_btn_ev_t);
    void (*console_register)(void);  // NULL = 不注册 console 命令
} menu_item_t;

static bool s_ok[7];
static int  s_sel = 0;
static int  s_active = -1;  // -1 = 在菜单; >=0 = 在对应 demo

// 7 项菜单
static const menu_item_t MENU[] = {
    { "Display",   &s_ok[0], demo_display_enter,   demo_display_exit,   demo_display_key,   NULL },
    { "Button",    &s_ok[1], demo_button_enter,    demo_button_exit,    demo_button_key,    NULL },
    { "Audio",     &s_ok[2], demo_audio_enter,     demo_audio_exit,     demo_audio_key,     NULL },
    { "Battery",   &s_ok[3], demo_battery_enter,   demo_battery_exit,   demo_battery_key,   NULL },
    { "Companion", &s_ok[4], demo_companion_enter, demo_companion_exit, demo_companion_key, demo_companion_console_register },
    { "Quick",     &s_ok[5], demo_quick_enter,     demo_quick_exit,     demo_quick_key,     demo_quick_console_register },
    { "Flow",      &s_ok[6], demo_flow_enter,      demo_flow_exit,      demo_flow_key,      demo_flow_console_register },
};
#define MENU_N (sizeof(MENU) / sizeof(MENU[0]))

// ====== Menu UI ======

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_rows[7];

static void menu_refresh(void) {
    if (!s_menu_scr) return;
    for (int i = 0; i < MENU_N; i++) {
        if (s_rows[i]) {
            lv_label_set_text_fmt(s_rows[i], "%s%s",
                                  MENU[i].name,
                                  *(MENU[i].ok_flag) ? "" : "  [FAIL]");
            lv_obj_set_style_text_color(s_rows[i],
                *(MENU[i].ok_flag) ? lv_color_hex(UI_INK) : lv_color_hex(0x7A2020),
                0);
        }
    }
}

static void menu_build(void) {
    s_menu_scr = ui_pixel_screen_create("AI Passport");

    // 7 项菜单, 2 列布局 (4 行 + 1 居中) -> 调整为竖直列表
    int card_w = 218;
    int card_h = 28;
    int x = 11;
    for (int i = 0; i < MENU_N; i++) {
        int y = 50 + i * 36;
        lv_obj_t *card = ui_pixel_panel_create(s_menu_scr, x, y, card_w, card_h, UI_PAPER);
        s_rows[i] = lv_label_create(card);
        lv_obj_set_style_text_font(s_rows[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_pad_left(s_rows[i], 12, 0);
        lv_obj_center(s_rows[i]);
    }
    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void menu_destroy(void) {
    if (s_menu_scr) {
        lv_obj_delete(s_menu_scr);
        s_menu_scr = NULL;
        for (int i = 0; i < MENU_N; i++) s_rows[i] = NULL;
    }
}

// 按键 callback: 运行在 button 组件任务, 操作 LVGL 必须加锁
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    if (s_active >= 0) {
        // 在 demo 中
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            MENU[s_active].exit();
            s_active = -1;
            menu_build();
        } else {
            MENU[s_active].key(btn, ev);
        }
    } else if (ev == BSP_BTN_CLICK) {
        // 在菜单中
        if (btn == BSP_BTN_UP) {
            s_sel = (s_sel + MENU_N - 1) % MENU_N;
            menu_refresh();
        } else if (btn == BSP_BTN_DOWN) {
            s_sel = (s_sel + 1) % MENU_N;
            menu_refresh();
        } else if (btn == BSP_BTN_OK) {
            if (!*(MENU[s_sel].ok_flag)) {
                // 不进入失败的项
            } else {
                s_active = s_sel;
                menu_destroy();
                MENU[s_active].enter();
            }
        }
    }

    bsp_lvgl_unlock();
}

// ====== app_main ======

void app_main(void) {
    ESP_LOGI(TAG, "===== AI Passport boot (3 plugins) =====");

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init FAILED");
        return;
    }
    bsp_display_backlight(100);

    s_ok[0] = true;  // Display 确认 OK
    s_ok[1] = (bsp_button_init(on_key, NULL) == ESP_OK);
    s_ok[2] = (bsp_audio_init() == ESP_OK);
    s_ok[3] = (bsp_battery_init() == ESP_OK);
    s_ok[4] = true;  // Companion 不依赖 bsp 子模块
    s_ok[5] = true;  // Quick 同上
    s_ok[6] = true;  // Flow 同上

    // console: USB-Serial/JTAG, sdkconfig 已配
    esp_console_repl_config_t repl_cfg = {
        .max_history_len = 10,
        .task_priority = 2,
        .task_stack_size = 4096,
        .prompt = "passport> ",
        .max_cmdline_length = 256,
    };
    esp_console_new_repl_usb_serial_jtag(&repl_cfg, NULL);
    esp_console_register_help_command();
    for (int i = 0; i < MENU_N; i++) {
        if (MENU[i].console_register) MENU[i].console_register();
    }

    if (bsp_lvgl_lock(1000)) {
        menu_build();
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "ready: D=%d B=%d A=%d Bat=%d C=%d Q=%d F=%d",
             s_ok[0], s_ok[1], s_ok[2], s_ok[3], s_ok[4], s_ok[5], s_ok[6]);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive");
    }
}
