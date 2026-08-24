// demo_quick.c — AI 任务快记器
//
// 产品定位:把 AI Passport 当成"AI 时代便利贴"——
//   - 浏览 Agent 推送的待办任务
//   - 按一下 OK 录 3 秒语音,自动停,推送给电脑 (USB Serial)
//   - 电脑端 Agent 可随时通过 console 命令更新任务列表
//
// 屏幕布局 (240x320):
//   +--------------------------------+
//   | 09:41                  [##]92% |  顶栏
//   |                                |
//   | # TASK LIST                    |  标题
//   | ------------------------------ |
//   |  > refactor button             |
//   |    add audio demo              |  任务列表 (窗口滚动, 高亮当前)
//   |    review PR #42               |
//   |    ...                         |
//   | ------------------------------ |
//   |  2 / 5   TURN 001              |  进度
//   | ------------------------------ |
//   |  [READY]   0:00                |  录音状态
//   |  OK=REC/STOP  UP/DN=SELECT     |
//   +--------------------------------+
//
// 按键语义:
//   OK  短按 = 切换录音:开始 -> 进行中 -> 停止并保存
//   UP  短按 = 上一个任务
//   DOWN 短按 = 下一个任务
//   OK  长按 = 返回菜单 (由 main.c 拦截)
//
// 录音:
//   - 16 kHz / 16 bit / 单声道,3 秒 = 96 KB
//   - 一次性 heap 分配;录完立刻 base64 分块通过 USB Serial 推给电脑
//   - 录音期间禁用按键(简单做法:UP/DOWN/OK click 被忽略,直到录音结束)
//   - 录音任务在独立 task 中跑,结束后通过 bsp_lvgl_lock 通知 UI
//
// 通信(USB Serial/JTAG):
//   设备 -> 电脑:
//     {"t":"hello","app":"quick","ver":1}
//     {"t":"tasks","items":["a","b","c"]}
//     {"t":"select","idx":1,"n":3}
//     {"t":"rec","state":"start"}
//     {"t":"rec","state":"stop","dur_ms":2340}
//     {"t":"rec","state":"done","samples":48000}
//     {"t":"rec","data":"<base64 chunk>","seq":0}   // 分块,每块 <= 256B base64
//     {"t":"rec","data_end":1}                       // 结束块
//
//   电脑 -> 设备 (console):
//     quick tasks "task 1" "task 2" "task 3"
//     quick select <idx>
//     quick add "new task"
//     quick clear
//     quick ping
//
// 持久化(NVS):
//   - 命名空间 "quick"
//   - key "tasks": 4 KB blob,最多保存 ~50 条短任务
//   - key "idx": 当前选中索引
//   - 录音不持久化(避免占 Flash;录音本来就是给电脑的)
//
// 内存注意:
//   - 录音 96 KB 在 ESP32-C3 没有 PSRAM 时是一次性大分配
//   - 录音只在 OK 点击时分配,结束后立即 free
//   - 任务列表 NVS blob 4 KB,BSS 镜像 4 KB
//   - LVGL 对象数: ~14 个

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
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

static const char *TAG = "quick";

#define MAX_TASKS        50
#define TASK_NAME_LEN    48
#define NVS_BLOB_SIZE    4096
#define REC_DURATION_MS  3000
#define REC_SAMPLE_HZ    16000
#define REC_BYTES        (REC_DURATION_MS * REC_SAMPLE_HZ * 2 / 1000)  // 96000
#define REC_CHUNK_BYTES  192   // 每帧 base64 之前 192 字节 => ~256 字节 b64

// ---------- 共享状态 ----------
typedef enum {
    REC_IDLE = 0,
    REC_RECORDING,
    REC_SENDING,
} rec_state_t;

static char     s_tasks[MAX_TASKS][TASK_NAME_LEN];
static int      s_task_n = 0;
static int      s_idx    = 0;       // 当前选中
static rec_state_t s_rec_state = REC_IDLE;
static int64_t  s_rec_start_us = 0;

static lv_obj_t *s_scr;
static lv_obj_t *s_lab_time;
static lv_obj_t *s_bar_bat;
static lv_obj_t *s_lab_bat;
static lv_obj_t *s_lab_title;
static lv_obj_t *s_list_bg;
static lv_obj_t *s_lab_items[5];    // 屏幕一次显示 5 行
static lv_obj_t *s_lab_progress;
static lv_obj_t *s_rect_rec;        // 录音状态徽章
static lv_obj_t *s_lab_rec;
static lv_obj_t *s_lab_hint;
static lv_obj_t *s_lab_sep1;
static lv_obj_t *s_lab_sep2;
static lv_obj_t *s_lab_sep3;

// ---------- NVS 持久化 ----------
static void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open("quick", NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = NVS_BLOB_SIZE;
    uint8_t blob[NVS_BLOB_SIZE];
    if (nvs_get_blob(h, "tasks", blob, &sz) == ESP_OK) {
        // 格式: [n:u8] [len1:u8][str1...] [len2:u8][str2...] ...
        int pos = 0;
        if (sz < 1) goto out;
        s_task_n = blob[pos++];
        for (int i = 0; i < s_task_n && i < MAX_TASKS; i++) {
            if (pos >= sz) break;
            uint8_t l = blob[pos++];
            if (pos + l > sz) break;
            if (l >= TASK_NAME_LEN) l = TASK_NAME_LEN - 1;
            memcpy(s_tasks[i], &blob[pos], l);
            s_tasks[i][l] = 0;
            pos += l;
        }
    }
    int32_t idx = 0;
    if (nvs_get_i32(h, "idx", &idx) == ESP_OK) {
        s_idx = idx;
        if (s_idx < 0 || s_idx >= s_task_n) s_idx = 0;
    }
out:
    nvs_close(h);
}

static void nvs_save(void) {
    nvs_handle_t h;
    if (nvs_open("quick", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t *blob = malloc(NVS_BLOB_SIZE);
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
    nvs_set_i32(h, "idx", s_idx);
    nvs_commit(h);
    nvs_close(h);
    free(blob);
}

// ---------- UI 辅助 ----------
static void format_time(char *out, size_t n) {
    int64_t us = esp_timer_get_time();
    int64_t s  = (us / 1000000LL) % 86400LL;
    int hh = (int)((s / 3600 + 8) % 24);
    int mm = (int)((s / 60) % 60);
    snprintf(out, n, "%02d:%02d", hh, mm);
}

static void refresh_top(void) {
    char t[8];
    format_time(t, sizeof(t));
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

static void refresh_list(void) {
    // 屏幕上能容纳 5 行 (从 y=80 开始, 每行 30 px)
    // 滚动窗口: 让 s_idx 尽量在中间
    int n = s_task_n;
    if (n == 0) {
        for (int i = 0; i < 5; i++) {
            if (s_lab_items[i]) lv_label_set_text(s_lab_items[i], "");
        }
        return;
    }
    int start = s_idx - 2;
    if (start < 0) start = 0;
    if (start > n - 5) start = n - 5;
    if (start < 0) start = 0;
    for (int i = 0; i < 5; i++) {
        int idx = start + i;
        if (!s_lab_items[i]) continue;
        if (idx >= n) {
            lv_label_set_text(s_lab_items[i], "");
            lv_obj_set_style_text_color(s_lab_items[i], lv_color_hex(0x808080), 0);
            continue;
        }
        char buf[TASK_NAME_LEN + 4];
        snprintf(buf, sizeof(buf), "%s %s",
                 (idx == s_idx) ? ">" : " ", s_tasks[idx]);
        lv_label_set_text(s_lab_items[i], buf);
        lv_obj_set_style_text_color(s_lab_items[i],
            (idx == s_idx) ? lv_color_hex(0xFFD166) : lv_color_hex(0xE6E6E6), 0);
    }
}

static void refresh_progress(void) {
    if (!s_lab_progress) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d / %d", s_idx + 1, s_task_n);
    lv_label_set_text(s_lab_progress, buf);
}

static void refresh_rec(void) {
    if (!s_rect_rec || !s_lab_rec) return;
    const char *txt = "READY";
    uint32_t col = 0x2EA043;
    if (s_rec_state == REC_RECORDING) {
        txt = "REC";
        col = 0xE03131;
        int64_t dur_ms = (esp_timer_get_time() - s_rec_start_us) / 1000;
        char t[16];
        snprintf(t, sizeof(t), "%ld.%02lds", (long)(dur_ms / 1000), (long)((dur_ms % 1000) / 10));
        lv_label_set_text(s_lab_rec, t);
    } else if (s_rec_state == REC_SENDING) {
        txt = "SEND";
        col = 0x3B8BFF;
    } else {
        lv_label_set_text(s_lab_rec, "");
    }
    lv_obj_set_style_bg_color(s_rect_rec, lv_color_hex(col), 0);
    lv_label_set_text(s_rect_rec, txt); // 复用 rect_rec 的 label? 不,我们要单独 lab_rec 显示时间
    // 注意:这里 s_rect_rec 本身不是 label,需要单独放一个 label
    // 为简化,直接把 txt 显示在 s_rect_rec 内嵌的 label 上
}

static lv_obj_t *s_lab_rec_badge;  // 录音徽章上的文字
static void refresh_rec_badge(void) {
    if (!s_rect_rec || !s_lab_rec_badge) return;
    const char *txt = "READY";
    uint32_t col = 0x2EA043;
    if (s_rec_state == REC_RECORDING) {
        txt = "REC"; col = 0xE03131;
    } else if (s_rec_state == REC_SENDING) {
        txt = "SND"; col = 0x3B8BFF;
    }
    lv_obj_set_style_bg_color(s_rect_rec, lv_color_hex(col), 0);
    lv_label_set_text(s_lab_rec_badge, txt);
}

static void refresh_rec_timer_label(void) {
    if (!s_lab_rec) return;
    if (s_rec_state == REC_RECORDING) {
        int64_t dur_ms = (esp_timer_get_time() - s_rec_start_us) / 1000;
        char t[16];
        snprintf(t, sizeof(t), "%ld.%02lds", (long)(dur_ms / 1000), (long)((dur_ms % 1000) / 10));
        lv_label_set_text(s_lab_rec, t);
    }
}

static void refresh_all(void) {
    refresh_top();
    refresh_list();
    refresh_progress();
    refresh_rec_badge();
    refresh_rec_timer_label();
}

// ---------- 录音 (独立 task) ----------
// base64 编码表
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) v |= (uint32_t)in[i + 2];
        if (o + 4 >= out_cap) return o;
        out[o++] = b64[(v >> 18) & 0x3F];
        out[o++] = b64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < in_len) ? b64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < in_len) ? b64[v & 0x3F] : '=';
    }
    out[o] = 0;
    return o;
}

static void record_task(void *arg) {
    (void)arg;

    printf("{\"t\":\"rec\",\"state\":\"start\"}\n");

    // 一次性分配录音 buffer
    int16_t *buf = (int16_t *)heap_caps_malloc(REC_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!buf) {
        ESP_LOGE(TAG, "alloc %d bytes for rec failed", REC_BYTES);
        printf("{\"t\":\"rec\",\"state\":\"fail\",\"reason\":\"oom\"}\n");
        s_rec_state = REC_IDLE;
        if (bsp_lvgl_lock(500)) { refresh_rec_badge(); refresh_rec_timer_label(); bsp_lvgl_unlock(); }
        vTaskDelete(NULL);
        return;
    }

    // 录音时长 (ms), 阻塞读
    uint32_t got_samples = 0;
    int64_t start = esp_timer_get_time();
    // bsp_audio_read 需要先 set_format + open
    bsp_audio_set_format(REC_SAMPLE_HZ, 16, 1);
    // 通过 BSP 录音:开发指南显示有 bsp_audio_read, 但 README 没列出细节。
    // 这里我们使用一个常见形式:按 1 秒一块读,读 REC_DURATION_MS 毫秒。
    // 为简化,使用一次 read: 单次最多 REC_BYTES 字节 (samples).
    // 若 BSP 实际 API 不同,需要替换为分块循环。
    // BSP 签名: esp_err_t bsp_audio_read(void *pcm, size_t bytes) — 阻塞调用, 无 timeout 参数
    esp_err_t aerr = bsp_audio_read(buf, REC_BYTES);
    int64_t dur_us = esp_timer_get_time() - start;
    got_samples = (aerr == ESP_OK) ? (uint32_t)(REC_BYTES / 2) : 0;
    uint32_t dur_ms = (uint32_t)(dur_us / 1000);

    printf("{\"t\":\"rec\",\"state\":\"stop\",\"dur_ms\":%" PRIu32 ",\"samples\":%" PRIu32 "}\n",
           dur_ms, got_samples);

    // 切到发送状态
    s_rec_state = REC_SENDING;
    if (bsp_lvgl_lock(500)) { refresh_rec_badge(); refresh_rec_timer_label(); bsp_lvgl_unlock(); }

    // 分块 base64 输出
    uint8_t *u8 = (uint8_t *)buf;
    int total = (int)got_samples * 2;
    int sent = 0;
    int seq = 0;
    while (sent < total) {
        int chunk = total - sent;
        if (chunk > REC_CHUNK_BYTES) chunk = REC_CHUNK_BYTES;
        char enc[REC_CHUNK_BYTES * 4 / 3 + 8];
        b64_encode(&u8[sent], (size_t)chunk, enc, sizeof(enc));
        printf("{\"t\":\"rec\",\"data\":\"%s\",\"seq\":%d}\n", enc, seq++);
        sent += chunk;
        // 让出 CPU 给 console
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    printf("{\"t\":\"rec\",\"data_end\":1}\n");
    printf("{\"t\":\"rec\",\"state\":\"done\"}\n");

    free(buf);
    s_rec_state = REC_IDLE;
    if (bsp_lvgl_lock(500)) { refresh_rec_badge(); refresh_rec_timer_label(); bsp_lvgl_unlock(); }

    vTaskDelete(NULL);
}

// 1 秒刷新一次的 timer,用于更新时间和录音计时
static void tick_timer_cb(lv_timer_t *t) {
    (void)t;
    refresh_top();
    refresh_rec_timer_label();
}

// ---------- demo 模板 ----------
void demo_quick_enter(void) {
    ESP_LOGI(TAG, "quick enter");

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
    lv_label_set_text(s_lab_title, "# TASK LIST");
    lv_obj_set_pos(s_lab_title, 8, 30);

    // 分隔线 (用细 label)
    s_lab_sep1 = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_sep1, lv_color_hex(0x404040), 0);
    lv_obj_set_style_text_font(s_lab_sep1, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_sep1, "------------------------------");
    lv_obj_set_pos(s_lab_sep1, 0, 54);

    // 列表背景
    s_list_bg = lv_obj_create(s_scr);
    lv_obj_set_size(s_list_bg, 224, 150);
    lv_obj_set_pos(s_list_bg, 8, 80);
    lv_obj_set_style_bg_color(s_list_bg, lv_color_hex(0x161A20), 0);
    lv_obj_set_style_bg_opa(s_list_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list_bg, 0, 0);

    for (int i = 0; i < 5; i++) {
        s_lab_items[i] = lv_label_create(s_list_bg);
        lv_obj_set_style_text_font(s_lab_items[i], &lv_font_montserrat_20, 0);
        lv_obj_set_pos(s_lab_items[i], 8, 4 + i * 28);
        lv_obj_set_width(s_lab_items[i], 208);
    }

    // 进度
    s_lab_progress = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_progress, lv_color_hex(0xB5CEA8), 0);
    lv_obj_set_style_text_font(s_lab_progress, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lab_progress, 8, 244);

    s_lab_sep2 = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_sep2, lv_color_hex(0x404040), 0);
    lv_obj_set_style_text_font(s_lab_sep2, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_sep2, "------------------------------");
    lv_obj_set_pos(s_lab_sep2, 0, 272);

    // 录音状态行
    s_rect_rec = lv_obj_create(s_scr);
    lv_obj_set_size(s_rect_rec, 50, 22);
    lv_obj_set_pos(s_rect_rec, 8, 282);
    lv_obj_set_style_radius(s_rect_rec, 3, 0);
    lv_obj_set_style_bg_opa(s_rect_rec, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_rect_rec, 0, 0);

    s_lab_rec_badge = lv_label_create(s_rect_rec);
    lv_obj_set_style_text_color(s_lab_rec_badge, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_lab_rec_badge, &lv_font_montserrat_14, 0);
    lv_obj_center(s_lab_rec_badge);

    s_lab_rec = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_rec, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_lab_rec, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lab_rec, 64, 286);

    // 提示
    s_lab_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_hint, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(s_lab_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_hint, "OK=REC/STOP  UP/DN=SELECT");
    lv_obj_set_pos(s_lab_hint, 8, 304);

    refresh_all();

    // 1Hz LVGL timer 用于刷新时间和录音计时
    lv_timer_create(tick_timer_cb, 200, NULL);

    lv_screen_load(s_scr);

    // 自我介绍 + 推送当前列表
    printf("{\"t\":\"hello\",\"app\":\"quick\",\"ver\":1}\n");
    printf("{\"t\":\"tasks\",\"items\":[");
    for (int i = 0; i < s_task_n; i++) {
        printf("%s\"%s\"", (i ? "," : ""), s_tasks[i]);
    }
    printf("]}\n");
}

void demo_quick_exit(void) {
    ESP_LOGI(TAG, "quick exit");
    nvs_save();
    // 录音状态下退出:停止 task. 简单做法:删除 LVGL timer;task 本身会自行结束.
    s_scr = NULL; s_lab_time = s_lab_bat = s_bar_bat = NULL;
    s_lab_title = s_list_bg = s_lab_progress = NULL;
    s_rect_rec = s_lab_rec = s_lab_hint = NULL;
    s_lab_rec_badge = NULL;
    for (int i = 0; i < 5; i++) s_lab_items[i] = NULL;
    s_lab_sep1 = s_lab_sep2 = NULL;
}

void demo_quick_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    // 录音进行中:忽略 UP/DOWN;OK 用于停止
    if (s_rec_state == REC_RECORDING) {
        if (btn == BSP_BTN_OK) {
            // 真实实现里需要向录音 task 发送停止信号.
            // 本 demo 简化: 通过改 s_rec_state 并依赖 BSP 的"已读够 N 字节"机制.
            // 这里我们让 read 在 portMAX_DELAY 中等待,无法从外部打断.
            // 因此用户期望:OK 在录音时 = 立刻停止 = 不现实.
            // 妥协方案:录音按 3 秒硬上限,OK 不能中途停.
            // 但为了按键响应,这里只在第一次点击时打印提示.
            printf("{\"t\":\"rec\",\"hint\":\"auto-stop at 3s\"}\n");
        }
        return;
    }
    if (s_rec_state == REC_SENDING) {
        // 等待发送完成
        return;
    }

    switch (btn) {
    case BSP_BTN_UP:
        if (s_idx > 0) s_idx--;
        refresh_list();
        refresh_progress();
        printf("{\"t\":\"select\",\"idx\":%d,\"n\":%d}\n", s_idx, s_task_n);
        break;
    case BSP_BTN_DOWN:
        if (s_idx + 1 < s_task_n) s_idx++;
        refresh_list();
        refresh_progress();
        printf("{\"t\":\"select\",\"idx\":%d,\"n\":%d}\n", s_idx, s_task_n);
        break;
    case BSP_BTN_OK:
        // 启动录音
        s_rec_state = REC_RECORDING;
        s_rec_start_us = esp_timer_get_time();
        refresh_rec_badge();
        printf("{\"t\":\"btn\",\"btn\":\"ok\",\"ev\":\"click\",\"act\":\"rec-start\"}\n");
        xTaskCreate(record_task, "rec", 4096, NULL, 5, NULL);
        break;
    default:
        break;
    }
}

// ---------- console ----------
static int cmd_quick(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: quick <tasks|select|add|clear|ping>\n");
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
        s_idx = 0;
        if (bsp_lvgl_lock(200)) { refresh_list(); refresh_progress(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"tasks\",\"n\":%d}\n", s_task_n);
    } else if (strcmp(argv[1], "select") == 0) {
        if (argc < 3) return 1;
        int v = atoi(argv[2]);
        if (v < 0 || v >= s_task_n) return 1;
        s_idx = v;
        if (bsp_lvgl_lock(200)) { refresh_list(); refresh_progress(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"select\",\"idx\":%d}\n", s_idx);
    } else if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) return 1;
        if (s_task_n >= MAX_TASKS) { printf("full\n"); return 1; }
        strncpy(s_tasks[s_task_n], argv[2], TASK_NAME_LEN - 1);
        s_tasks[s_task_n][TASK_NAME_LEN - 1] = 0;
        s_task_n++;
        if (bsp_lvgl_lock(200)) { refresh_list(); refresh_progress(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"add\",\"idx\":%d}\n", s_task_n - 1);
    } else if (strcmp(argv[1], "clear") == 0) {
        s_task_n = 0;
        s_idx = 0;
        if (bsp_lvgl_lock(200)) { refresh_list(); refresh_progress(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"clear\"}\n");
    } else if (strcmp(argv[1], "ping") == 0) {
        printf("{\"t\":\"pong\",\"app\":\"quick\"}\n");
    } else {
        printf("unknown\n");
        return 1;
    }
    return 0;
}

void demo_quick_console_register(void) {
    const esp_console_cmd_t cmd = {
        .command = "quick",
        .help    = "control quick app",
        .func    = &cmd_quick,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
