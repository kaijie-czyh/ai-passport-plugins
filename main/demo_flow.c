// demo_flow.c —— AI 心流节拍器
//
// 屏幕:
//   +--------------------------------+
//   | 09:41                  [##]92% |
//   |                                |
//   |  FLOW  POMODORO  RUNNING       |
//   |                                |
//   |         24:35                  |   大字倒计时
//   |   [============..........]      |   进度条
//   |                                |
//   |  TASK:  refactor button        |   当前任务 (console push)
//   |  STATE:  RUNNING  25min        |
//   |  FOCUS: 3  STREAK: 5           |
//   |                                |
//   |  OK=START/STOP  UP/DN=TASK     |
//   +--------------------------------+
//
// 按键:
//   OK  短按 = 启动 / 暂停 番茄钟
//   UP/DOWN  短按 = 上下任务
//   OK  长按 = 返回菜单

#include <stdio.h>
#include <string.h>
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

// GB2312 大字库(含常用汉字), 任务名可显示中文
LV_FONT_DECLARE(pas_cjk_16);

static const char *TAG = "flow";

#define DEFAULT_FOCUS_S (25 * 60)
#define DEFAULT_BREAK_S (5 * 60)
#define MAX_TASKS       8
#define TASK_NAME_LEN   32
#define NVS_BLOB_SIZE   1024

typedef enum { ST_IDLE = 0, ST_RUNNING, ST_PAUSED, ST_BREAK } flow_state_t;

static flow_state_t s_state = ST_IDLE;
static int32_t      s_remain_s = 0;
static int32_t      s_total_s  = 0;
static int32_t      s_focus_default_s = DEFAULT_FOCUS_S;
static int32_t      s_break_default_s = DEFAULT_BREAK_S;
static uint32_t     s_focus_count = 0;
static uint32_t     s_streak_days = 0;

static char s_tasks[MAX_TASKS][TASK_NAME_LEN];
static int  s_task_n = 0;
static int  s_task_idx = 0;

// 后台倒计时 task
static volatile bool s_task_exit = false;
static TaskHandle_t  s_tick_task = NULL;

// UI
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_lab_time = NULL;
static lv_obj_t *s_bar_bat = NULL;
static lv_obj_t *s_lab_bat = NULL;
static lv_obj_t *s_lab_title = NULL;
static lv_obj_t *s_lab_remain = NULL;
static lv_obj_t *s_bar_prog = NULL;
static lv_obj_t *s_lab_task_lbl = NULL;
static lv_obj_t *s_lab_task = NULL;
static lv_obj_t *s_lab_stats = NULL;
static lv_obj_t *s_lab_hint = NULL;
static lv_timer_t *s_tick = NULL;

// ---------- NVS ----------
static void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open("flow", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t *blob = (uint8_t *)malloc(NVS_BLOB_SIZE);
    if (!blob) goto out;
    size_t sz = NVS_BLOB_SIZE;
    if (nvs_get_blob(h, "tasks", blob, &sz) == ESP_OK) {
        int pos = 0;
        if (sz < 1) goto out_free;
        if (blob[pos] > MAX_TASKS) goto out_free;
        s_task_n = blob[pos++];
        for (int i = 0; i < s_task_n; i++) {
            if (pos >= sz) { s_task_n = i; break; }
            uint8_t l = blob[pos++];
            if (pos + l > sz) { s_task_n = i; break; }
            if (l >= TASK_NAME_LEN) l = TASK_NAME_LEN - 1;
            memcpy(s_tasks[i], &blob[pos], l);
            s_tasks[i][l] = 0;
            pos += l;
        }
    }
out_free:
    free(blob);
    uint32_t u32 = 0;
    if (nvs_get_u32(h, "focus_count", &u32) == ESP_OK) s_focus_count = u32;
    if (nvs_get_u32(h, "streak_days", &u32) == ESP_OK) s_streak_days = u32;
out:
    nvs_close(h);
}

static void nvs_save(void) {
    nvs_handle_t h;
    if (nvs_open("flow", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t *blob = (uint8_t *)malloc(NVS_BLOB_SIZE);
    if (!blob) { nvs_close(h); return; }
    int pos = 0;
    blob[pos++] = (uint8_t)s_task_n;
    for (int i = 0; i < s_task_n; i++) {
        int l = (int)strlen(s_tasks[i]);
        if (l >= TASK_NAME_LEN) l = TASK_NAME_LEN - 1;
        if (pos + 1 + l >= NVS_BLOB_SIZE) break;
        blob[pos++] = (uint8_t)l;
        memcpy(&blob[pos], s_tasks[i], l);
        pos += l;
    }
    nvs_set_blob(h, "tasks", blob, pos);
    nvs_set_u32(h, "focus_count", s_focus_count);
    nvs_set_u32(h, "streak_days", s_streak_days);
    nvs_commit(h);
    nvs_close(h);
    free(blob);
}

// ---------- 倒计时 task ----------
// 用"绝对截止时刻"驱动倒计时:暂停时冻结 remain,恢复时从 remain 重新设截止时刻。
// 这样既解决每 100ms 取整丢秒的问题,也让 FOCUS/BREAK 都能正常数秒。
// 💡 由按键/console 线程(main_task)写入、后台 tick task 读取,故加 volatile 防寄存器缓存。
static volatile int64_t s_deadline_us = 0;

static void flow_set_deadline(int32_t secs) {
    s_deadline_us = esp_timer_get_time() + (int64_t)secs * 1000000LL;
}

static void flow_tick_task(void *arg) {
    (void)arg;
    while (!s_task_exit) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_state != ST_RUNNING && s_state != ST_BREAK) continue;
        int64_t remain_s = (s_deadline_us - esp_timer_get_time()) / 1000000LL;
        if (remain_s < 0) remain_s = 0;
        if (remain_s != s_remain_s) s_remain_s = (int32_t)remain_s;
        if (s_remain_s > 0) continue;

        if (s_state == ST_RUNNING) {
            // focus 完成 → 进入 BREAK
            s_focus_count++;
            s_state = ST_BREAK;
            s_total_s = s_break_default_s;
            s_remain_s = s_total_s;
            flow_set_deadline(s_break_default_s);
            printf("{\"t\":\"event\",\"kind\":\"focus_done\"}\n");
        } else { // ST_BREAK 完成 → IDLE
            s_state = ST_IDLE;
            s_total_s = 0;
            s_remain_s = 0;
            printf("{\"t\":\"event\",\"kind\":\"break_done\"}\n");
        }
    }
    vTaskDelete(NULL);
}

// ---------- UI ----------
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

static void refresh_remain(void) {
    if (!s_lab_remain || !s_bar_prog) return;
    char buf[16];
    if (s_remain_s > 0) snprintf(buf, sizeof(buf), "%02ld:%02ld", (long)(s_remain_s / 60), (long)(s_remain_s % 60));
    else snprintf(buf, sizeof(buf), "--:--");
    lv_label_set_text(s_lab_remain, buf);
    int pct = 0;
    if (s_total_s > 0) pct = (int)((s_total_s - s_remain_s) * 100 / s_total_s);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(s_bar_prog, pct, LV_ANIM_OFF);
}

static void refresh_title(void) {
    if (!s_lab_title) return;
    const char *st = "IDLE";
    uint32_t col = 0x17202A;
    if (s_state == ST_RUNNING) { st = "RUNNING"; col = 0x2EA043; }
    else if (s_state == ST_PAUSED) { st = "PAUSED"; col = 0xFFB23E; }
    else if (s_state == ST_BREAK) { st = "BREAK"; col = 0x3B8BFF; }
    char buf[48];
    snprintf(buf, sizeof(buf), "FLOW %s", st);
    lv_label_set_text(s_lab_title, buf);
    lv_obj_set_style_text_color(s_lab_title, lv_color_hex(col), 0);
}

static void refresh_task(void) {
    if (!s_lab_task) return;
    if (s_task_n > 0) {
        lv_label_set_text(s_lab_task, s_tasks[s_task_idx]);
    } else {
        lv_label_set_text(s_lab_task, "(no task)");
    }
}

static void refresh_stats(void) {
    if (!s_lab_stats) return;
    char buf[48];
    snprintf(buf, sizeof(buf), "FOCUS %lu  STREAK %lu",
             (unsigned long)s_focus_count, (unsigned long)s_streak_days);
    lv_label_set_text(s_lab_stats, buf);
}

static void refresh_all(void) {
    refresh_top();
    refresh_remain();
    refresh_title();
    refresh_task();
    refresh_stats();
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    refresh_top();
    refresh_remain();
    refresh_title();   // RUNNING/BREAK 完成后标题要实时切换
    refresh_stats();
}

// ---------- demo 接口 ----------
void demo_flow_enter(void) {
    ESP_LOGI(TAG, "flow enter");
    nvs_load();
    // 空任务时给默认项, 避免一进来就显示 "(no task)"
    if (s_task_n == 0) {
        static const char *dflt[] = { "专注当前任务", "深度阅读", "写作" };
        for (int i = 0; i < (int)(sizeof(dflt) / sizeof(dflt[0])) && s_task_n < MAX_TASKS; i++) {
            strncpy(s_tasks[s_task_n], dflt[i], TASK_NAME_LEN - 1);
            s_tasks[s_task_n][TASK_NAME_LEN - 1] = 0;
            s_task_n++;
        }
        s_task_idx = 0;
    }

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0xF4F4EA), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    // 顶栏
    s_lab_time = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_time, lv_color_hex(0x17202A), 0);
    lv_obj_set_style_text_font(s_lab_time, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lab_time, 8, 6);

    s_bar_bat = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar_bat, 60, 8);
    lv_obj_set_pos(s_bar_bat, 130, 12);
    lv_bar_set_range(s_bar_bat, 0, 100);
    lv_obj_set_style_bg_color(s_bar_bat, lv_color_hex(0xD9E7EC), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_bat, lv_color_hex(0x2EA043), LV_PART_INDICATOR);

    s_lab_bat = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_bat, lv_color_hex(0x17202A), 0);
    lv_obj_set_style_text_font(s_lab_bat, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lab_bat, 196, 8);

    // 标题
    s_lab_title = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_title, lv_color_hex(0x17202A), 0);
    lv_obj_set_style_text_font(s_lab_title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lab_title, 8, 30);

    // 倒计时大字
    s_lab_remain = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_remain, lv_color_hex(0x17202A), 0);
    lv_obj_set_style_text_font(s_lab_remain, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lab_remain, 80, 70);

    // 进度条
    s_bar_prog = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar_prog, 224, 12);
    lv_obj_set_pos(s_bar_prog, 8, 110);
    lv_bar_set_range(s_bar_prog, 0, 100);
    lv_obj_set_style_bg_color(s_bar_prog, lv_color_hex(0xD9E7EC), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_prog, lv_color_hex(0xFFB23E), LV_PART_INDICATOR);

    // TASK
    s_lab_task_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_task_lbl, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(s_lab_task_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_task_lbl, "TASK:");
    lv_obj_set_pos(s_lab_task_lbl, 8, 140);

    s_lab_task = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_task, lv_color_hex(0xE43B2F), 0);
    lv_obj_set_style_text_font(s_lab_task, &pas_cjk_16, 0);
    lv_obj_set_pos(s_lab_task, 56, 134);
    lv_obj_set_width(s_lab_task, 180);

    // STATS
    s_lab_stats = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_stats, lv_color_hex(0x55951D), 0);
    lv_obj_set_style_text_font(s_lab_stats, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lab_stats, 8, 180);

    // HINT
    s_lab_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_hint, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(s_lab_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_hint, "OK=START/STOP  UP/DN=TASK");
    lv_obj_set_pos(s_lab_hint, 8, 304);

    refresh_all();
    s_tick = lv_timer_create(tick_cb, 250, NULL);
    lv_screen_load(s_scr);

    s_task_exit = false;
    xTaskCreate(flow_tick_task, "flow_tick", 4096, NULL, 4, &s_tick_task);

    printf("{\"t\":\"hello\",\"app\":\"flow\",\"ver\":1}\n");
}

void demo_flow_exit(void) {
    ESP_LOGI(TAG, "flow exit");
    nvs_save();
    s_task_exit = true;
    vTaskDelay(pdMS_TO_TICKS(150));
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_lab_time = s_bar_bat = s_lab_bat = s_lab_title = NULL;
    s_lab_remain = s_bar_prog = NULL;
    s_lab_task_lbl = s_lab_task = NULL;
    s_lab_stats = s_lab_hint = NULL;
    s_tick_task = NULL;
}

void demo_flow_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    switch (btn) {
    case BSP_BTN_UP:
        if (s_task_n > 0) {
            s_task_idx = (s_task_idx - 1 + s_task_n) % s_task_n;
            refresh_task();
            printf("{\"t\":\"select\",\"idx\":%d,\"n\":%d}\n", s_task_idx, s_task_n);
        }
        break;
    case BSP_BTN_DOWN:
        if (s_task_n > 0) {
            s_task_idx = (s_task_idx + 1) % s_task_n;
            refresh_task();
            printf("{\"t\":\"select\",\"idx\":%d,\"n\":%d}\n", s_task_idx, s_task_n);
        }
        break;
    case BSP_BTN_OK:
        if (s_state == ST_IDLE) {
            s_total_s = s_focus_default_s;
            s_remain_s = s_total_s;
            s_state = ST_RUNNING;
            flow_set_deadline(s_total_s);
            refresh_remain(); refresh_title();
            printf("{\"t\":\"event\",\"kind\":\"focus_start\",\"dur_s\":%ld}\n", (long)s_total_s);
        } else if (s_state == ST_RUNNING) {
            s_state = ST_PAUSED;   // 暂停:冻结 remain,deadline 不变,恢复时重设
            refresh_title();
            printf("{\"t\":\"event\",\"kind\":\"pause\"}\n");
        } else if (s_state == ST_PAUSED) {
            s_state = ST_RUNNING;
            if (s_remain_s > 0) flow_set_deadline(s_remain_s);   // 从暂停值续跑
            refresh_title();
            printf("{\"t\":\"event\",\"kind\":\"resume\"}\n");
        } else if (s_state == ST_BREAK) {
            s_state = ST_IDLE;
            s_total_s = 0;
            s_remain_s = 0;
            refresh_remain(); refresh_title();
            printf("{\"t\":\"event\",\"kind\":\"break_skip\"}\n");
        }
        break;
    default:
        break;
    }
}

// ---------- console ----------
static int cmd_flow(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: flow <tasks|select|add|clear|start|stop|ping>\n");
        return 1;
    }
    if (strcmp(argv[1], "tasks") == 0) {
        if (argc < 3) { printf("need >=1 task\n"); return 1; }
        s_task_n = 0;
        for (int i = 2; i < argc && s_task_n < MAX_TASKS; i++) {
            strncpy(s_tasks[s_task_n], argv[i], TASK_NAME_LEN - 1);
            s_tasks[s_task_n][TASK_NAME_LEN - 1] = 0;
            s_task_n++;
        }
        s_task_idx = 0;
        if (bsp_lvgl_lock(200)) { refresh_task(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"tasks\",\"n\":%d}\n", s_task_n);
    } else if (strcmp(argv[1], "select") == 0) {
        if (argc < 3) return 1;
        int v = atoi(argv[2]);
        if (v < 0 || v >= s_task_n) return 1;
        s_task_idx = v;
        if (bsp_lvgl_lock(200)) { refresh_task(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"select\",\"idx\":%d}\n", s_task_idx);
    } else if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) return 1;
        if (s_task_n >= MAX_TASKS) { printf("full\n"); return 1; }
        strncpy(s_tasks[s_task_n], argv[2], TASK_NAME_LEN - 1);
        s_tasks[s_task_n][TASK_NAME_LEN - 1] = 0;
        s_task_n++;
        if (bsp_lvgl_lock(200)) { refresh_task(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"add\",\"idx\":%d}\n", s_task_n - 1);
    } else if (strcmp(argv[1], "clear") == 0) {
        s_task_n = 0;
        s_task_idx = 0;
        if (bsp_lvgl_lock(200)) { refresh_task(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"clear\"}\n");
    } else if (strcmp(argv[1], "start") == 0) {
        if (s_state != ST_IDLE) { printf("not idle\n"); return 1; }
        s_total_s = s_focus_default_s;
        s_remain_s = s_total_s;
        s_state = ST_RUNNING;
        flow_set_deadline(s_total_s);
        if (bsp_lvgl_lock(200)) { refresh_remain(); refresh_title(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"event\",\"kind\":\"focus_start\",\"dur_s\":%ld}\n", (long)s_total_s);
    } else if (strcmp(argv[1], "stop") == 0) {
        s_state = ST_IDLE;
        s_total_s = 0;
        s_remain_s = 0;
        if (bsp_lvgl_lock(200)) { refresh_remain(); refresh_title(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"event\",\"kind\":\"stop\"}\n");
    } else if (strcmp(argv[1], "ping") == 0) {
        printf("{\"t\":\"pong\",\"app\":\"flow\"}\n");
    } else {
        printf("unknown\n");
        return 1;
    }
    return 0;
}

void demo_flow_console_register(void) {
    const esp_console_cmd_t cmd = {
        .command = "flow",
        .help    = "control flow app",
        .func    = &cmd_flow,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
