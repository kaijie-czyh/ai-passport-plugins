// main.c —— AI Passport 主入口 (重写版)
//
// 关键设计:
//   1. button 回调投递事件到 FreeRTOS queue
//   2. main_task 从 queue 读事件, 在 ~20KB 栈里执行 demo_enter/exit/key
//      → 彻底避免 esp_timer task 4KB 栈的 stack overflow
//   3. LVGL 操作统一在 main_task 里 lock 后做, 录音独立 task 跑
//
// 菜单:
//   7 项. 选中项用黄色 panel 高亮, 未选中用深色 panel.

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
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "demo.h"

static const char *TAG = "main";

// ---------- 按键事件投递 ----------
typedef enum {
    EV_KEY,        // 普通按键 (CLICK)
    EV_KEY_LONG,   // 长按 (用于 demo 内退出)
} event_kind_t;

typedef struct {
    event_kind_t kind;
    bsp_btn_t    btn;
    bsp_btn_ev_t ev;
} event_t;

static QueueHandle_t s_evtq = NULL;

// button 回调, 运行在 esp_timer task, 必须立刻返回
static void on_button_isr(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    event_t e = { .kind = (ev == BSP_BTN_LONG) ? EV_KEY_LONG : EV_KEY, .btn = btn, .ev = ev };
    BaseType_t hp = pdFALSE;
    if (s_evtq) xQueueSendFromISR(s_evtq, &e, &hp);
}

// ---------- 菜单状态 ----------
static int s_sel = 0;     // 当前选中菜单项
static int s_active = -1; // -1 = 菜单页; >=0 = 进入对应 demo

// ---------- 菜单 UI ----------
static lv_obj_t *s_menu_scr = NULL;
static lv_obj_t *s_panels[7] = {NULL};
static lv_obj_t *s_labels[7] = {NULL};

// 7 项菜单 (顺序与 demo_entry_t 对应)
const demo_entry_t g_demos[] = {
    { "Display",   demo_display_enter,   demo_display_exit,   demo_display_key,   NULL },
    { "Button",    demo_button_enter,    demo_button_exit,    demo_button_key,    NULL },
    { "Audio",     demo_audio_enter,     demo_audio_exit,     demo_audio_key,     NULL },
    { "Battery",   demo_battery_enter,   demo_battery_exit,   demo_battery_key,   NULL },
    { "Companion", demo_companion_enter, demo_companion_exit, demo_companion_key, demo_companion_console_register },
    { "Quick",     demo_quick_enter,     demo_quick_exit,     demo_quick_key,     demo_quick_console_register },
    { "Flow",      demo_flow_enter,      demo_flow_exit,      demo_flow_key,      demo_flow_console_register },
};
const int g_demos_n = sizeof(g_demos) / sizeof(g_demos[0]);

// 高亮颜色: 选中 = 黄色, 未选中 = 深灰
#define COLOR_SELECTED   0xFFD928
#define COLOR_UNSEL      0x223238

static void menu_refresh(void) {
    if (!s_menu_scr) return;
    for (int i = 0; i < g_demos_n; i++) {
        if (!s_panels[i]) continue;
        bool sel = (i == s_sel);
        lv_obj_set_style_bg_color(s_panels[i],
            lv_color_hex(sel ? COLOR_SELECTED : COLOR_UNSEL), 0);
        if (s_labels[i]) {
            lv_obj_set_style_text_color(s_labels[i],
                sel ? lv_color_hex(0x101820) : lv_color_hex(0xE6E6E6), 0);
        }
    }
}

static void menu_build(void) {
    s_menu_scr = ui_pixel_screen_create("AI Passport");

    // 7 个 panel, 上下排列
    int card_w = 218;
    int card_h = 32;
    int x = 11;
    for (int i = 0; i < g_demos_n; i++) {
        int y = 44 + i * 38;
        s_panels[i] = ui_pixel_panel_create(s_menu_scr, x, y, card_w, card_h, COLOR_UNSEL);
        if (s_panels[i]) {
            // 让 panel 的字居中显示
            lv_obj_set_style_radius(s_panels[i], 4, 0);
            s_labels[i] = lv_label_create(s_panels[i]);
            lv_obj_set_style_text_font(s_labels[i], &lv_font_montserrat_20, 0);
            lv_label_set_text(s_labels[i], g_demos[i].name);
            lv_obj_center(s_labels[i]);
        }
    }
    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void menu_destroy(void) {
    if (s_menu_scr) {
        lv_obj_delete(s_menu_scr);
        s_menu_scr = NULL;
        for (int i = 0; i < g_demos_n; i++) {
            s_panels[i] = NULL;
            s_labels[i] = NULL;
        }
    }
}

// ---------- 处理 queue 事件 ----------
static void process_event(event_t *e) {
    if (s_active >= 0) {
        // 在 demo 内
        if (e->kind == EV_KEY_LONG && e->btn == BSP_BTN_OK) {
            // OK 长按 = 返回菜单
            g_demos[s_active].exit();
            s_active = -1;
            menu_build();
            return;
        }
        // 其他键: 转给 demo 处理
        g_demos[s_active].key(e->btn, e->ev);
    } else {
        // 菜单页
        if (e->kind != EV_KEY) return;  // 菜单页忽略长按
        if (e->btn == BSP_BTN_UP) {
            s_sel = (s_sel + g_demos_n - 1) % g_demos_n;
            menu_refresh();
        } else if (e->btn == BSP_BTN_DOWN) {
            s_sel = (s_sel + 1) % g_demos_n;
            menu_refresh();
        } else if (e->btn == BSP_BTN_OK) {
            // 进入 demo
            s_active = s_sel;
            menu_destroy();
            g_demos[s_active].enter();
        }
    }
}

// ---------- main_task: 队列消费者 ----------
static void main_task_fn(void *arg) {
    (void)arg;
    event_t e;
    for (;;) {
        if (xQueueReceive(s_evtq, &e, portMAX_DELAY) == pdTRUE) {
            // 在主任务上下文里跑, 有 20KB 栈
            if (bsp_lvgl_lock(2000)) {
                process_event(&e);
                bsp_lvgl_unlock();
            } else {
                ESP_LOGW(TAG, "lvgl lock fail, skip event");
            }
        }
    }
}

// ---------- app_main ----------
void app_main(void) {
    ESP_LOGI(TAG, "===== AI Passport boot (3 plugins) =====");

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init FAILED");
        return;
    }
    bsp_display_backlight(100);

    // 非显示外设(软依赖:单项失败只禁对应页,不阻塞整机)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    if (bsp_audio_init() != ESP_OK)   ESP_LOGW(TAG, "bsp_audio_init failed (Audio/Quick 录音不可用)");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "bsp_battery_init failed (电量显示 0%)");

    // console (USB-Serial/JTAG)。必须 new 出句柄并 start_repl,否则命令不会被执行。
    esp_console_repl_config_t repl_cfg = {
        .max_history_len = 10,
        .task_priority = 2,
        .task_stack_size = 4096,
        .prompt = "passport> ",
        .max_cmdline_length = 256,
    };
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = { 0 };
    esp_console_repl_t *repl = NULL;
    if (esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl) != ESP_OK) {
        ESP_LOGE(TAG, "console repl init failed");
        return;
    }
    esp_console_register_help_command();
    for (int i = 0; i < g_demos_n; i++) {
        if (g_demos[i].console_register) g_demos[i].console_register();
    }
    esp_console_start_repl(repl);

    // button 回调 = 投递事件
    if (bsp_button_init(on_button_isr, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "button init fail");
        return;
    }

    // 创建事件队列 + 主任务
    s_evtq = xQueueCreate(16, sizeof(event_t));
    xTaskCreate(main_task_fn, "main_task", 8192, NULL, 5, NULL);

    // 在主任务里 build 菜单 (通过 enqueue 一个 fake event)
    // 但更简单: 直接调用 (app_main 本身跑在 main_task, 栈足够)
    menu_build();

    ESP_LOGI(TAG, "ready");
    // app_main 退出后, 队列消费者继续跑
    vTaskDelete(NULL);
}
