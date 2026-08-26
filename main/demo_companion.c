// demo_companion.c —— AI 编程伴侣卡
//
// 屏幕: 显示 AI Agent (TRAE/Claude Code/Cursor) 的实时状态.
//   - STATE: IDLE / THINKING / TOOL_CALL / WAITING / DONE / ERROR
//   - TASK:  当前正在做的任务名
//   - METRICS: 累计对话数 / token 数 / 运行时长
//   - PROGRESS: 进度条
//
// 数据来源: console 命令 `companion push <state> <task> [token] [idx] [n]`
//           由电脑端 TRAE Skill 调用 passport_push.py 发送.
//
// 按键:
//   OK  短按 = ACK (告诉 Agent "已看到"),推送 {"t":"btn","act":"ack"}
//   UP  短按 = 上一项 (在多任务列表中)
//   DOWN 短按 = 下一项
//   OK  长按 = 返回菜单

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_console.h"

static const char *TAG = "companion";

#define MAX_TASK_LEN 64
#define MAX_TASKS    8
#define NVS_BLOB_SIZE 1024

typedef enum {
    S_IDLE = 0,
    S_THINKING,
    S_TOOL,
    S_WAITING,
    S_DONE,
    S_ERROR,
} state_t;

static const char *STATE_NAMES[] = {
    "IDLE", "THINKING", "TOOL", "WAITING", "DONE", "ERROR"
};
static const uint32_t STATE_COLORS[] = {
    0x808080, 0xFFD928, 0x3B8BFF, 0xFFB23E, 0x2EA043, 0xE03131
};

static state_t s_state = S_IDLE;
static char    s_task[MAX_TASK_LEN] = "(no task)";
static uint32_t s_tokens = 0;
static uint32_t s_turn = 0;
static int      s_task_idx = 0;
static int      s_task_n = 0;
static char     s_tasks[MAX_TASKS][MAX_TASK_LEN];

static int64_t s_state_start_us = 0;  // 进入当前 state 的时间

// 把 "hx:<hex>" 后缀的 16 进制串(仅 ASCII, 两两一字节)解码为 UTF-8 写入 out。
// 用途: ESP console REPL 会丢弃部分非 ASCII 输入, 中文任务名先由电脑端 hex 编码
//       成纯 ASCII 传输, 固件端再解码还原, 从而能在屏幕上正常显示中文。
static void companion_hex_decode(const char *hex, char *out, size_t n) {
    size_t i = 0, o = 0;
    if (n == 0) return;
    if (!hex) { out[0] = 0; return; }
    while (hex[i] && hex[i + 1] && o + 1 < n) {
        char a = hex[i], b = hex[i + 1];
        a = (a >= '0' && a <= '9') ? (char)(a - '0')
            : (a >= 'A' && a <= 'F') ? (char)(a - 'A' + 10)
            : (char)(a - 'a' + 10);
        b = (b >= '0' && b <= '9') ? (char)(b - '0')
            : (b >= 'A' && b <= 'F') ? (char)(b - 'A' + 10)
            : (char)(b - 'a' + 10);
        out[o++] = (char)((a << 4) | b);
        i += 2;
    }
    out[o] = 0;
}

// ---------- UI ----------
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_lab_time = NULL;
static lv_obj_t *s_bar_bat = NULL;
static lv_obj_t *s_lab_bat = NULL;
static lv_obj_t *s_lab_title = NULL;
static lv_obj_t *s_rect_state = NULL;
static lv_obj_t *s_lab_state = NULL;
static lv_obj_t *s_lab_state_label = NULL;
static lv_obj_t *s_lab_task_lbl = NULL;
static lv_obj_t *s_lab_task = NULL;
static lv_obj_t *s_lab_metrics = NULL;
static lv_obj_t *s_lab_elapsed = NULL;
static lv_obj_t *s_bar_prog = NULL;
static lv_obj_t *s_lab_hint = NULL;
static lv_timer_t *s_tick = NULL;

static void format_clock(char *out, size_t n) {
    int64_t us = esp_timer_get_time();
    int64_t s  = (us / 1000000LL) % 86400LL;
    int hh = (int)((s / 3600 + 8) % 24);
    int mm = (int)((s / 60) % 60);
    snprintf(out, n, "%02d:%02d", hh, mm);
}

static void refresh_top(void) {
    char t[8];
    format_clock(t, sizeof(t));
    if (s_lab_time) lv_label_set_text(s_lab_time, t);
    int soc = bsp_battery_soc();
    if (soc < 0) soc = 0;
    if (soc > 100) soc = 100;
    if (s_bar_bat) lv_bar_set_value(s_bar_bat, soc, LV_ANIM_OFF);
    if (s_lab_bat) {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", soc);
        lv_label_set_text(s_lab_bat, b);
    }
}

static void refresh_state(void) {
    if (!s_rect_state || !s_lab_state) return;
    uint32_t col = STATE_COLORS[s_state];
    lv_obj_set_style_bg_color(s_rect_state, lv_color_hex(col), 0);
    lv_label_set_text(s_lab_state, STATE_NAMES[s_state]);
    lv_obj_set_style_text_color(s_lab_state, lv_color_hex(0x101820), 0);
}

static void refresh_task(void) {
    if (!s_lab_task) return;
    if (s_task_n > 0) {
        // 前缀 %d/%d + 任务名最坏会超 64B,给足余量(否则 -Werror=format-truncation 报错)
        char buf[MAX_TASK_LEN * 2];
        snprintf(buf, sizeof(buf), "%d/%d %s", s_task_idx + 1, s_task_n, s_tasks[s_task_idx]);
        lv_label_set_text(s_lab_task, buf);
    } else {
        lv_label_set_text(s_lab_task, s_task);
    }
}

static void refresh_metrics(void) {
    if (!s_lab_metrics) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "TURN %lu  TOK %lu",
             (unsigned long)s_turn, (unsigned long)s_tokens);
    lv_label_set_text(s_lab_metrics, buf);
}

static void refresh_elapsed(void) {
    if (!s_lab_elapsed) return;
    int64_t us = esp_timer_get_time() - s_state_start_us;
    int sec = (int)(us / 1000000LL);
    char buf[24];
    if (sec < 60) snprintf(buf, sizeof(buf), "%ds", sec);
    else snprintf(buf, sizeof(buf), "%dm%02ds", sec / 60, sec % 60);
    lv_label_set_text(s_lab_elapsed, buf);
}

static void refresh_all(void) {
    refresh_top();
    refresh_state();
    refresh_task();
    refresh_metrics();
    refresh_elapsed();
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    refresh_top();
    refresh_elapsed();
}

// ---------- NVS ----------
static void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open("companion", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t u32 = 0;
    if (nvs_get_u32(h, "turn", &u32) == ESP_OK) s_turn = u32;
    if (nvs_get_u32(h, "tokens", &u32) == ESP_OK) s_tokens = u32;
    int8_t i8 = 0;
    if (nvs_get_i8(h, "state", &i8) == ESP_OK && i8 >= 0 && i8 <= S_ERROR) s_state = (state_t)i8;
    nvs_close(h);
}

static void nvs_save(void) {
    nvs_handle_t h;
    if (nvs_open("companion", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "turn", s_turn);
    nvs_set_u32(h, "tokens", s_tokens);
    nvs_set_i8(h, "state", (int8_t)s_state);
    nvs_commit(h);
    nvs_close(h);
}

// ---------- demo 接口 ----------
void demo_companion_enter(void) {
    ESP_LOGI(TAG, "companion enter");
    nvs_load();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    // 顶栏
    s_lab_time = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_time, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_lab_time, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lab_time, 8, 6);

    s_bar_bat = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar_bat, 60, 8);
    lv_obj_set_pos(s_bar_bat, 130, 12);
    lv_bar_set_range(s_bar_bat, 0, 100);
    lv_obj_set_style_bg_color(s_bar_bat, lv_color_hex(0x2A2E33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_bat, lv_color_hex(0x2EA043), LV_PART_INDICATOR);

    s_lab_bat = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_bat, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_lab_bat, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lab_bat, 196, 8);

    // 标题
    s_lab_title = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_title, lv_color_hex(0x9CDCFE), 0);
    lv_obj_set_style_text_font(s_lab_title, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_lab_title, "AI Companion");
    lv_obj_set_pos(s_lab_title, 8, 28);

    // STATE 徽章
    s_rect_state = lv_obj_create(s_scr);
    lv_obj_set_size(s_rect_state, 160, 36);
    lv_obj_set_pos(s_rect_state, 40, 64);
    lv_obj_set_style_radius(s_rect_state, 6, 0);
    lv_obj_set_style_bg_opa(s_rect_state, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_rect_state, 0, 0);

    s_lab_state = lv_label_create(s_rect_state);
    lv_obj_set_style_text_font(s_lab_state, &lv_font_montserrat_20, 0);
    lv_obj_center(s_lab_state);

    s_lab_state_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_state_label, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(s_lab_state_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_state_label, "STATE");
    lv_obj_set_pos(s_lab_state_label, 8, 72);

    s_lab_elapsed = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_elapsed, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(s_lab_elapsed, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lab_elapsed, 206, 72);

    // TASK
    s_lab_task_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_task_lbl, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(s_lab_task_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_task_lbl, "TASK:");
    lv_obj_set_pos(s_lab_task_lbl, 8, 116);

    s_lab_task = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_task, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_lab_task, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_pos(s_lab_task, 56, 110);
    lv_obj_set_width(s_lab_task, 180);

    // 进度条 (turn 完成度)
    s_bar_prog = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar_prog, 224, 10);
    lv_obj_set_pos(s_bar_prog, 8, 152);
    lv_bar_set_range(s_bar_prog, 0, 100);
    lv_obj_set_style_bg_color(s_bar_prog, lv_color_hex(0x2A2E33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_prog, lv_color_hex(0x9CDCFE), LV_PART_INDICATOR);

    // METRICS
    s_lab_metrics = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_metrics, lv_color_hex(0xB5CEA8), 0);
    lv_obj_set_style_text_font(s_lab_metrics, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lab_metrics, 8, 180);

    // HINT
    s_lab_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_hint, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(s_lab_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_hint, "OK=ACK  UP/DN=TASK");
    lv_obj_set_pos(s_lab_hint, 8, 304);

    refresh_all();
    s_tick = lv_timer_create(tick_cb, 250, NULL);
    lv_screen_load(s_scr);

    s_state_start_us = esp_timer_get_time();
    printf("{\"t\":\"hello\",\"app\":\"companion\",\"ver\":1}\n");
    printf("{\"t\":\"status\",\"s\":\"%s\",\"task\":\"%s\",\"tok\":%lu,\"turn\":%lu}\n",
           STATE_NAMES[s_state], s_task, (unsigned long)s_tokens, (unsigned long)s_turn);
}

void demo_companion_exit(void) {
    ESP_LOGI(TAG, "companion exit");
    nvs_save();
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_lab_time = s_bar_bat = s_lab_bat = s_lab_title = NULL;
    s_rect_state = s_lab_state = s_lab_state_label = NULL;
    s_lab_task_lbl = s_lab_task = NULL;
    s_lab_metrics = s_lab_elapsed = s_bar_prog = s_lab_hint = NULL;
}

void demo_companion_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    switch (btn) {
    case BSP_BTN_UP:
        if (s_task_n > 0) {
            s_task_idx = (s_task_idx - 1 + s_task_n) % s_task_n;
            strncpy(s_task, s_tasks[s_task_idx], MAX_TASK_LEN - 1);
            s_task[MAX_TASK_LEN - 1] = 0;
            refresh_task();
            printf("{\"t\":\"select\",\"idx\":%d,\"n\":%d}\n", s_task_idx, s_task_n);
        }
        break;
    case BSP_BTN_DOWN:
        if (s_task_n > 0) {
            s_task_idx = (s_task_idx + 1) % s_task_n;
            strncpy(s_task, s_tasks[s_task_idx], MAX_TASK_LEN - 1);
            s_task[MAX_TASK_LEN - 1] = 0;
            refresh_task();
            printf("{\"t\":\"select\",\"idx\":%d,\"n\":%d}\n", s_task_idx, s_task_n);
        }
        break;
    case BSP_BTN_OK:
        // ACK: 通知 Agent 用户已看到
        printf("{\"t\":\"btn\",\"btn\":\"ok\",\"ev\":\"click\",\"act\":\"ack\"}\n");
        break;
    default:
        break;
    }
}

// ---------- console ----------
// 用法: companion push <state> [task] [tokens] [turn]
//   state ∈ {IDLE, THINKING, TOOL, WAITING, DONE, ERROR} (大小写不敏感)
// 'status' 是 'push' 的别名 —— 兼容老文档/桥接脚本里的 `companion status ...`。
static int cmd_companion(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: companion <push|status|ping>\n");
        return 1;
    }
    if (strcmp(argv[1], "push") == 0 || strcmp(argv[1], "status") == 0) {
        if (argc < 3) { printf("need state\n"); return 1; }
        state_t ns = S_IDLE;
        for (int i = 0; i <= S_ERROR; i++) {
            if (strcasecmp(argv[2], STATE_NAMES[i]) == 0) { ns = (state_t)i; break; }
        }
        s_state = ns;
        s_state_start_us = esp_timer_get_time();
        if (argc >= 4) {
            // 任务名可以是普通 UTF-8, 也能是 "hx:<hex>"(中文经 hex 编码传输, 绕过 console 非 ASCII 限制)
            char decoded[MAX_TASK_LEN];
            if (strncmp(argv[3], "hx:", 3) == 0) {
                companion_hex_decode(argv[3] + 3, decoded, sizeof(decoded));
            } else {
                strncpy(decoded, argv[3], sizeof(decoded) - 1);
                decoded[sizeof(decoded) - 1] = 0;
            }
            strncpy(s_task, decoded, MAX_TASK_LEN - 1);
            s_task[MAX_TASK_LEN - 1] = 0;
            // 同时加入任务列表
            if (s_task_n < MAX_TASKS) {
                strncpy(s_tasks[s_task_n], decoded, MAX_TASK_LEN - 1);
                s_tasks[s_task_n][MAX_TASK_LEN - 1] = 0;
                s_task_idx = s_task_n;
                s_task_n++;
            }
        }
        if (argc >= 5) s_tokens = (uint32_t)strtoul(argv[4], NULL, 10);
        if (argc >= 6) s_turn = (uint32_t)strtoul(argv[5], NULL, 10);
        // 在 main_task 里更新 UI
        if (bsp_lvgl_lock(200)) { refresh_all(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"status\",\"s\":\"%s\",\"task\":\"%s\",\"tok\":%lu,\"turn\":%lu}\n",
               STATE_NAMES[s_state], s_task, (unsigned long)s_tokens, (unsigned long)s_turn);
    } else if (strcmp(argv[1], "ping") == 0) {
        printf("{\"t\":\"pong\",\"app\":\"companion\"}\n");
    } else {
        printf("unknown\n");
        return 1;
    }
    return 0;
}

void demo_companion_console_register(void) {
    const esp_console_cmd_t cmd = {
        .command = "companion",
        .help    = "AI companion state",
        .func    = &cmd_companion,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
