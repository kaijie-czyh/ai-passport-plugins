// demo_flow.c — AI 心流节拍器
//
// 产品定位:把 AI Passport 变成"番茄钟 + 任务上下文屏",服务于 AI 重度工作者的
// 长时间专注、休息节奏、任务上下文。
//
// 屏幕布局 (240x320):
//   +--------------------------------+
//   | 10:23                  [##]87% |  顶栏
//   |                                |
//   |        FLOW SESSION            |  标题
//   |                                |
//   |         18:42                  |  剩余时间大字
//   |                                |
//   |      [ ===========     ]       |  进度条
//   |                                |
//   |        TASK                    |
//   |   refactor bsp_button.c        |  当前任务(滚动)
//   |                                |
//   |     FOCUS x3  STREAK 5d        |  累计统计
//   |                                |
//   |  POMODORO 25min   STATE: ON    |  状态行
//   |                                |
//   |  OK=PAUSE  UP/DN=SWITCH TASK   |
//   +--------------------------------+
//
// 状态机:
//   IDLE      -> 没有 session,屏幕显示 00:00, OK 开始一个 session
//   RUNNING   -> 倒计时;每分钟蜂鸣一次短音;到 0 蜂鸣三声长音 -> BREAK
//   PAUSED    -> 暂停倒计时;OK 恢复
//   BREAK     -> 休息阶段(短时长);到 0 蜂鸣 -> IDLE
//
// 按键:
//   OK  短按 = 暂停 / 恢复  (IDLE 时 = 开始)
//   UP       = 上一个任务
//   DOWN     = 下一个任务
//   OK  长按 = 返回菜单
//
// 通信(USB Serial):
//   设备 -> 电脑:
//     {"t":"hello","app":"flow","ver":1}
//     {"t":"tick","state":"running","remain":1122,"total":1500,"task":"..."}
//     {"t":"event","kind":"focus_done","dur_s":1500}
//     {"t":"event","kind":"break_done","dur_s":300}
//   电脑 -> 设备 (console):
//     flow tasks "task 1" "task 2" ...
//     flow start 25        (开始 25 分钟 focus)
//     flow pause / resume / stop
//     flow ping
//
// 持久化(NVS):
//   - "flow" namespace
//   - key "tasks": 任务列表 blob (同 quick 格式)
//   - key "stats": { focus_count:u32, streak_days:u32, last_day:u32 }
//   - key "dur":   当前 session 总秒数(暂停时保存,断电可恢复)
//
// 内存注意:
//   - LVGL 对象数 ~14
//   - 任务列表 4 KB NVS blob,BSS 镜像 4 KB
//   - 没有大 buffer (96KB 录音不属于本 demo)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_console.h"

static const char *TAG = "flow";

#define MAX_TASKS       50
#define TASK_NAME_LEN   48
#define NVS_BLOB_SIZE   4096
#define DEFAULT_FOCUS_S (25 * 60)
#define DEFAULT_BREAK_S (5  * 60)

typedef enum {
    ST_IDLE = 0,
    ST_RUNNING,
    ST_PAUSED,
    ST_BREAK,
} flow_state_t;

typedef struct {
    flow_state_t state;
    int32_t      remain_s;   // 剩余秒数
    int32_t      total_s;    // 本段总秒数
    int32_t      focus_default_s;
    int32_t      break_default_s;
    uint32_t     focus_count; // 累计完成 focus 数
    uint32_t     streak_days;
    uint32_t     last_day;    // unix day
    int          task_idx;
    int          task_n;
    char         tasks[MAX_TASKS][TASK_NAME_LEN];
    char         cur_task[TASK_NAME_LEN]; // 缓存当前显示任务
} flow_t;

static flow_t s_f = {
    .state = ST_IDLE,
    .remain_s = 0, .total_s = 0,
    .focus_default_s = DEFAULT_FOCUS_S,
    .break_default_s = DEFAULT_BREAK_S,
};

static lv_obj_t *s_scr;
static lv_obj_t *s_lab_time;
static lv_obj_t *s_bar_bat;
static lv_obj_t *s_lab_bat;
static lv_obj_t *s_lab_title;
static lv_obj_t *s_lab_remain;
static lv_obj_t *s_bar_prog;
static lv_obj_t *s_lab_task_lbl;
static lv_obj_t *s_lab_task;
static lv_obj_t *s_lab_stats;
static lv_obj_t *s_lab_state;
static lv_obj_t *s_lab_hint;

// ---------- 时间格式化 ----------
static void format_clock(char *out, size_t n) {
    int64_t us = esp_timer_get_time();
    int64_t s  = (us / 1000000LL) % 86400LL;
    int hh = (int)((s / 3600 + 8) % 24);
    int mm = (int)((s / 60) % 60);
    snprintf(out, n, "%02d:%02d", hh, mm);
}

// today_unix_day() 暂未实现 (需 SNTP 才能精确) — streak 仅基于 NVS 上次保存值

// ---------- NVS ----------
static void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open("flow", NVS_READONLY, &h) != ESP_OK) return;

    size_t sz = NVS_BLOB_SIZE;
    uint8_t blob[NVS_BLOB_SIZE];
    if (nvs_get_blob(h, "tasks", blob, &sz) == ESP_OK) {
        int pos = 0;
        if (sz < 1) goto stats;
        s_f.task_n = blob[pos++];
        for (int i = 0; i < s_f.task_n && i < MAX_TASKS; i++) {
            if (pos >= sz) break;
            uint8_t l = blob[pos++];
            if (pos + l > sz) break;
            if (l >= TASK_NAME_LEN) l = TASK_NAME_LEN - 1;
            memcpy(s_f.tasks[i], &blob[pos], l);
            s_f.tasks[i][l] = 0;
            pos += l;
        }
    }
stats:;
    uint32_t u32 = 0;
    if (nvs_get_u32(h, "focus_count", &u32) == ESP_OK) s_f.focus_count = u32;
    if (nvs_get_u32(h, "streak_days", &u32) == ESP_OK) s_f.streak_days = u32;
    if (nvs_get_u32(h, "last_day",    &u32) == ESP_OK) s_f.last_day = u32;

    int32_t i32 = 0;
    if (nvs_get_i32(h, "remain", &i32) == ESP_OK) s_f.remain_s = i32;
    if (nvs_get_i32(h, "total",  &i32) == ESP_OK) s_f.total_s  = i32;
    int8_t i8 = 0;
    if (nvs_get_i8(h, "state", &i8) == ESP_OK) s_f.state = (flow_state_t)i8;
    // 仅当上次是非 IDLE 时,启动时恢复为 PAUSED,避免误以为在跑
    if (s_f.state == ST_RUNNING) {
        s_f.state = ST_PAUSED;
    } else if (s_f.state == ST_BREAK) {
        s_f.state = ST_IDLE;
        s_f.remain_s = 0;
        s_f.total_s  = 0;
    }
    nvs_close(h);
}

static void nvs_save_state(void) {
    nvs_handle_t h;
    if (nvs_open("flow", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "remain", s_f.remain_s);
    nvs_set_i32(h, "total",  s_f.total_s);
    nvs_set_i8 (h, "state",  (int8_t)s_f.state);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_tasks(void) {
    nvs_handle_t h;
    if (nvs_open("flow", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t *blob = malloc(NVS_BLOB_SIZE);
    if (!blob) { nvs_close(h); return; }
    int pos = 0;
    blob[pos++] = (uint8_t)s_f.task_n;
    for (int i = 0; i < s_f.task_n; i++) {
        int l = (int)strlen(s_f.tasks[i]);
        if (l >= TASK_NAME_LEN) l = TASK_NAME_LEN - 1;
        if (pos + 1 + l >= NVS_BLOB_SIZE) break;
        blob[pos++] = (uint8_t)l;
        memcpy(&blob[pos], s_f.tasks[i], l);
        pos += l;
    }
    nvs_set_blob(h, "tasks", blob, pos);
    nvs_commit(h);
    nvs_close(h);
    free(blob);
}

static void nvs_save_stats(void) {
    nvs_handle_t h;
    if (nvs_open("flow", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "focus_count", s_f.focus_count);
    nvs_set_u32(h, "streak_days", s_f.streak_days);
    nvs_set_u32(h, "last_day",    s_f.last_day);
    nvs_commit(h);
    nvs_close(h);
}

// ---------- 蜂鸣 ----------
// BSP 没暴露独立 tone API 时,用一段 1 kHz 方波短音
// 这里调 bsp_audio_play,需要一段 buffer;为节省内存,用同一段 200 样本 1kHz 方波。
static int16_t *s_beep_buf = NULL;
static size_t   s_beep_len = 0;

static void beep_init(void) {
    if (s_beep_buf) return;
    s_beep_len = 16000 / 8; // 1/8 秒 1kHz @ 16kHz
    s_beep_buf = malloc(sizeof(int16_t) * s_beep_len);
    if (!s_beep_buf) return;
    for (size_t i = 0; i < s_beep_len; i++) {
        int sign = ((i / 8) & 1) ? 1 : -1;
        s_beep_buf[i] = (int16_t)(sign * 8000);
    }
    bsp_audio_set_format(16000, 16, 1);
}

static void beep_once(void) {
    if (!s_beep_buf) beep_init();
    if (!s_beep_buf) return;
    // BSP 签名: esp_err_t bsp_audio_write(const void *pcm, size_t bytes)
    (void)bsp_audio_write(s_beep_buf, s_beep_len * sizeof(int16_t));
}

static void beep_done(void) {
    for (int i = 0; i < 3; i++) {
        beep_once();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------- 倒计时 task ----------
static int64_t s_last_tick_ms = 0;

static void flow_tick_task(void *arg) {
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_f.state != ST_RUNNING && s_f.state != ST_BREAK) continue;
        if (s_f.remain_s > 0) s_f.remain_s--;
        // 每分钟一次短 beep (仅 focus 阶段)
        if (s_f.state == ST_RUNNING && (s_f.remain_s % 60 == 0) && s_f.remain_s > 0) {
            beep_once();
        }
        if (s_f.remain_s == 0) {
            if (s_f.state == ST_RUNNING) {
                // focus 完成
                s_f.state = ST_BREAK;
                s_f.remain_s = s_f.break_default_s;
                s_f.total_s  = s_f.break_default_s;
                s_f.focus_count++;
                nvs_save_stats();
                nvs_save_state();
                beep_done();
                printf("{\"t\":\"event\",\"kind\":\"focus_done\",\"dur_s\":%ld}\n",
                       (long)s_f.total_s);
            } else if (s_f.state == ST_BREAK) {
                s_f.state = ST_IDLE;
                s_f.remain_s = 0;
                s_f.total_s = 0;
                nvs_save_state();
                beep_done();
                printf("{\"t\":\"event\",\"kind\":\"break_done\"}\n");
            }
        }
        // UI 刷新 (已在 lvgl 任务中,只需要 lvgl timer 处理)
    }
}

// ---------- UI 刷新 ----------
static void refresh_top(void) {
    char t[8];
    format_clock(t, sizeof(t));
    if (s_lab_time) lv_label_set_text(s_lab_time, t);
    int soc = bsp_battery_soc();
    if (soc < 0) soc = 0;
    if (soc > 100) soc = 100;
    if (s_bar_bat) lv_bar_set_value(s_bar_bat, soc, LV_ANIM_OFF);
    if (s_lab_bat) {
        char b[8]; snprintf(b, sizeof(b), "%d%%", soc);
        lv_label_set_text(s_lab_bat, b);
    }
}

static void refresh_remain(void) {
    if (!s_lab_remain || !s_bar_prog) return;
    int m = s_f.remain_s / 60;
    int s = s_f.remain_s % 60;
    char buf[16];
    if (s_f.state == ST_IDLE) {
        snprintf(buf, sizeof(buf), "--:--");
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    }
    lv_label_set_text(s_lab_remain, buf);
    int pct = 0;
    if (s_f.total_s > 0) {
        pct = (int)((s_f.total_s - s_f.remain_s) * 100 / s_f.total_s);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    lv_bar_set_value(s_bar_prog, pct, LV_ANIM_OFF);
}

static void refresh_task(void) {
    if (!s_lab_task) return;
    const char *src = "no task";
    if (s_f.task_n > 0) {
        int idx = s_f.task_idx;
        if (idx < 0) idx = 0;
        if (idx >= s_f.task_n) idx = s_f.task_n - 1;
        src = s_f.tasks[idx];
    }
    char buf[TASK_NAME_LEN + 4];
    snprintf(buf, sizeof(buf), "> %s", src);
    lv_label_set_text(s_lab_task, buf);
}

static void refresh_stats(void) {
    if (!s_lab_stats) return;
    char buf[40];
    snprintf(buf, sizeof(buf), "FOCUS x%" PRIu32 "  STREAK %" PRIu32 "d",
             s_f.focus_count, s_f.streak_days);
    lv_label_set_text(s_lab_stats, buf);
}

static void refresh_state(void) {
    if (!s_lab_state) return;
    const char *stxt = "IDLE";
    uint32_t scol = 0x7A7A7A;
    switch (s_f.state) {
    case ST_IDLE:    stxt = "IDLE";    scol = 0x7A7A7A; break;
    case ST_RUNNING: stxt = "FOCUS";   scol = 0x2EA043; break;
    case ST_PAUSED:  stxt = "PAUSED";  scol = 0xF4C20D; break;
    case ST_BREAK:   stxt = "BREAK";   scol = 0x3B8BFF; break;
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "%s %ldmin   STATE: %s",
             (s_f.state == ST_BREAK) ? "BREAK" : "POMODORO",
             (long)((s_f.state == ST_BREAK) ? (s_f.break_default_s / 60)
                                            : (s_f.focus_default_s / 60)),
             stxt);
    lv_label_set_text(s_lab_state, buf);
    lv_obj_set_style_text_color(s_lab_state, lv_color_hex(scol), 0);
}

static void refresh_all(void) {
    refresh_top();
    refresh_remain();
    refresh_task();
    refresh_stats();
    refresh_state();
}

// 每秒一次的 LVGL timer
static void flow_tick_cb(lv_timer_t *t) {
    (void)t;
    refresh_top();
    refresh_remain();
    refresh_state();
}

// ---------- demo 模板 ----------
void demo_flow_enter(void) {
    ESP_LOGI(TAG, "flow enter");
    nvs_load();
    beep_init();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0F1115), 0);
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
    lv_obj_set_style_text_align(s_lab_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lab_title, "FLOW SESSION");
    lv_obj_set_pos(s_lab_title, 0, 36);
    lv_obj_set_width(s_lab_title, 240);

    // 倒计时大字
    s_lab_remain = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_remain, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lab_remain, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_lab_remain, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lab_remain, 0, 76);
    lv_obj_set_width(s_lab_remain, 240);

    // 进度条
    s_bar_prog = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar_prog, 200, 10);
    lv_obj_set_pos(s_bar_prog, 20, 138);
    lv_bar_set_range(s_bar_prog, 0, 100);
    lv_obj_set_style_bg_color(s_bar_prog, lv_color_hex(0x2A2E33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_prog, lv_color_hex(0x3B8BFF), LV_PART_INDICATOR);

    // TASK 区
    s_lab_task_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_task_lbl, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(s_lab_task_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_task_lbl, "TASK");
    lv_obj_set_pos(s_lab_task_lbl, 8, 170);

    s_lab_task = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_task, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_lab_task, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(s_lab_task, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_lab_task, 224);
    lv_obj_set_pos(s_lab_task, 8, 190);

    // 统计
    s_lab_stats = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_stats, lv_color_hex(0xB5CEA8), 0);
    lv_obj_set_style_text_font(s_lab_stats, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lab_stats, 8, 230);

    // 状态
    s_lab_state = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_lab_state, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lab_state, 8, 262);

    // 提示
    s_lab_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_hint, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(s_lab_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_hint, "OK=START/PAUSE  UP/DN=SWITCH TASK");
    lv_obj_set_pos(s_lab_hint, 8, 296);

    refresh_all();
    lv_timer_create(flow_tick_cb, 500, NULL);

    // 启动后台倒计时 task (已经在 main 启动后, 只在 RUNNING/BREAK 时 tick)
    if (s_f.state == ST_PAUSED && s_f.remain_s > 0) {
        // 恢复上次 session, 但保持 PAUSED
    }
    s_last_tick_ms = 0;

    lv_screen_load(s_scr);

    printf("{\"t\":\"hello\",\"app\":\"flow\",\"ver\":1}\n");
    printf("{\"t\":\"tick\",\"state\":\"%s\",\"remain\":%ld,\"total\":%ld}\n",
           s_f.state == ST_IDLE ? "idle" :
           s_f.state == ST_RUNNING ? "running" :
           s_f.state == ST_PAUSED ? "paused" : "break",
           (long)s_f.remain_s, (long)s_f.total_s);

    // 启动常驻倒计时 task (一次性)
    extern void flow_task_launch(void);
    flow_task_launch();
}

// 实现:启动一次后台倒计时 task. 使用静态分配避免再次 enter 时重复创建.
static TaskHandle_t s_tick_h = NULL;
void flow_task_launch(void) {
    if (s_tick_h) return;
    xTaskCreate(flow_tick_task, "flow-tick", 4096, NULL, 4, &s_tick_h);
}

void demo_flow_exit(void) {
    ESP_LOGI(TAG, "flow exit");
    nvs_save_state();
    // 后台 task 不删,继续 tick (因为它检查 s_f.state,exit 后不会刷新 UI 因为 s_scr=NULL)
    // 但实际中,s_f 仍是有效的 BSS,exit 后若其他 demo 改了 s_f 不合适.
    // 为安全:删后台 task.
    if (s_tick_h) {
        vTaskDelete(s_tick_h);
        s_tick_h = NULL;
    }
    if (s_beep_buf) { free(s_beep_buf); s_beep_buf = NULL; s_beep_len = 0; }
    s_scr = NULL;
    s_lab_time = s_lab_bat = s_bar_bat = NULL;
    s_lab_title = s_lab_remain = s_bar_prog = NULL;
    s_lab_task_lbl = s_lab_task = NULL;
    s_lab_stats = s_lab_state = s_lab_hint = NULL;
}

void demo_flow_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    switch (btn) {
    case BSP_BTN_UP:
        if (s_f.task_idx > 0) s_f.task_idx--;
        refresh_task();
        printf("{\"t\":\"task\",\"idx\":%d,\"name\":\"%s\"}\n",
               s_f.task_idx, (s_f.task_n > 0 ? s_f.tasks[s_f.task_idx] : ""));
        break;
    case BSP_BTN_DOWN:
        if (s_f.task_idx + 1 < s_f.task_n) s_f.task_idx++;
        refresh_task();
        printf("{\"t\":\"task\",\"idx\":%d,\"name\":\"%s\"}\n",
               s_f.task_idx, (s_f.task_n > 0 ? s_f.tasks[s_f.task_idx] : ""));
        break;
    case BSP_BTN_OK:
        switch (s_f.state) {
        case ST_IDLE:
            s_f.state = ST_RUNNING;
            s_f.total_s  = s_f.focus_default_s;
            s_f.remain_s = s_f.focus_default_s;
            refresh_state();
            nvs_save_state();
            beep_once();
            printf("{\"t\":\"event\",\"kind\":\"focus_start\",\"dur_s\":%ld}\n",
                   (long)s_f.total_s);
            break;
        case ST_RUNNING:
            s_f.state = ST_PAUSED;
            refresh_state();
            nvs_save_state();
            printf("{\"t\":\"event\",\"kind\":\"paused\"}\n");
            break;
        case ST_PAUSED:
            s_f.state = ST_RUNNING;
            refresh_state();
            nvs_save_state();
            beep_once();
            printf("{\"t\":\"event\",\"kind\":\"resumed\"}\n");
            break;
        case ST_BREAK:
            // 跳过 break,直接 IDLE
            s_f.state = ST_IDLE;
            s_f.remain_s = 0;
            s_f.total_s  = 0;
            refresh_state();
            refresh_remain();
            nvs_save_state();
            printf("{\"t\":\"event\",\"kind\":\"break_skipped\"}\n");
            break;
        }
        break;
    default: break;
    }
}

// ---------- console ----------
static int cmd_flow(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: flow <tasks|select|add|start|pause|resume|stop|skip|dur|ping>\n");
        return 1;
    }
    if (strcmp(argv[1], "tasks") == 0) {
        if (argc < 3) { printf("need >=1 task\n"); return 1; }
        s_f.task_n = 0;
        for (int i = 2; i < argc && s_f.task_n < MAX_TASKS; i++) {
            strncpy(s_f.tasks[s_f.task_n], argv[i], TASK_NAME_LEN - 1);
            s_f.tasks[s_f.task_n][TASK_NAME_LEN - 1] = 0;
            s_f.task_n++;
        }
        s_f.task_idx = 0;
        nvs_save_tasks();
        if (bsp_lvgl_lock(200)) { refresh_task(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"tasks\",\"n\":%d}\n", s_f.task_n);
    } else if (strcmp(argv[1], "select") == 0) {
        if (argc < 3) return 1;
        int v = atoi(argv[2]);
        if (v < 0 || v >= s_f.task_n) return 1;
        s_f.task_idx = v;
        if (bsp_lvgl_lock(200)) { refresh_task(); bsp_lvgl_unlock(); }
    } else if (strcmp(argv[1], "start") == 0) {
        int mins = (argc >= 3) ? atoi(argv[2]) : 25;
        if (mins < 1) mins = 1;
        if (mins > 180) mins = 180;
        s_f.state = ST_RUNNING;
        s_f.total_s  = mins * 60;
        s_f.remain_s = mins * 60;
        nvs_save_state();
        if (bsp_lvgl_lock(200)) { refresh_state(); refresh_remain(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"event\",\"kind\":\"focus_start\",\"dur_s\":%ld}\n", (long)s_f.total_s);
    } else if (strcmp(argv[1], "pause") == 0) {
        if (s_f.state == ST_RUNNING) {
            s_f.state = ST_PAUSED;
            nvs_save_state();
            if (bsp_lvgl_lock(200)) { refresh_state(); bsp_lvgl_unlock(); }
        }
    } else if (strcmp(argv[1], "resume") == 0) {
        if (s_f.state == ST_PAUSED) {
            s_f.state = ST_RUNNING;
            nvs_save_state();
            if (bsp_lvgl_lock(200)) { refresh_state(); bsp_lvgl_unlock(); }
        }
    } else if (strcmp(argv[1], "stop") == 0) {
        s_f.state = ST_IDLE;
        s_f.remain_s = 0; s_f.total_s = 0;
        nvs_save_state();
        if (bsp_lvgl_lock(200)) { refresh_state(); refresh_remain(); bsp_lvgl_unlock(); }
    } else if (strcmp(argv[1], "skip") == 0) {
        if (s_f.state == ST_BREAK) {
            s_f.state = ST_IDLE;
            s_f.remain_s = 0; s_f.total_s = 0;
            nvs_save_state();
            if (bsp_lvgl_lock(200)) { refresh_state(); refresh_remain(); bsp_lvgl_unlock(); }
        }
    } else if (strcmp(argv[1], "dur") == 0) {
        if (argc < 4) { printf("usage: flow dur <focus_min> <break_min>\n"); return 1; }
        s_f.focus_default_s = atoi(argv[2]) * 60;
        s_f.break_default_s = atoi(argv[3]) * 60;
        if (bsp_lvgl_lock(200)) { refresh_state(); bsp_lvgl_unlock(); }
    } else if (strcmp(argv[1], "ping") == 0) {
        printf("{\"t\":\"pong\",\"app\":\"flow\"}\n");
    } else {
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
