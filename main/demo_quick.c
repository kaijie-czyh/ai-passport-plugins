// demo_quick.c —— AI 任务快记器 (录音为主)
//
// 屏幕:
//   +--------------------------------+
//   | 09:41                  [##]92% |
//   |                                |
//   | # QUICK REC                    |
//   |                                |
//   |  ● ● ●  RECORDING  01:23       |   录音状态 + 时长
//   |                                |
//   |  Task list:                    |
//   |   > refactor button            |   (console push 的任务)
//   |     add audio demo             |
//   |     review PR #42              |
//   |                                |
//   |  OK=REC  UP/DN=TASK            |
//   +--------------------------------+
//
// 按键:
//   OK  短按 = 开始 / 停止录音
//   UP/DOWN  短按 = 上下任务
//   OK  长按 = 返回菜单
//
// 录音:
//   16 kHz / 16 bit / mono, 3 秒 = 96 KB
//   录音期间独立 task 跑 (heap 分配, 完成后 free)
//   实时通过 printf 分块推送 base64 到电脑
//   电脑端 passport_bridge.py 接收并保存为 WAV
//
// 任务列表: console 命令 quick tasks/select/add/clear 维护.

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
#include "esp_heap_caps.h"   // heap_caps_malloc / MALLOC_CAP_*

static const char *TAG = "quick";

#define MAX_TASKS     16
#define TASK_NAME_LEN 48
#define NVS_BLOB_SIZE 1024
#define REC_HZ        16000
#define REC_DURATION_MS 3000
#define REC_BYTES     (REC_HZ * REC_DURATION_MS / 1000 * 2)  // 96 KB
#define REC_CHUNK     192  // 每帧原始字节 -> ~256 base64

// ---------- 任务列表 ----------
static char s_tasks[MAX_TASKS][TASK_NAME_LEN];
static int  s_task_n = 0;
static int  s_task_idx = 0;

// ---------- 录音状态 ----------
typedef enum { REC_IDLE = 0, REC_RECORDING, REC_SENDING } rec_state_t;
static volatile rec_state_t s_rec_state = REC_IDLE;
static int64_t s_rec_start_us = 0;

// ---------- UI ----------
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_lab_time = NULL;
static lv_obj_t *s_bar_bat = NULL;
static lv_obj_t *s_lab_bat = NULL;
static lv_obj_t *s_lab_title = NULL;
static lv_obj_t *s_rect_rec = NULL;
static lv_obj_t *s_lab_rec = NULL;
static lv_obj_t *s_lab_rec_time = NULL;
static lv_obj_t *s_lab_tasks_hdr = NULL;
static lv_obj_t *s_lab_tasks[5] = {NULL};
static lv_obj_t *s_lab_hint = NULL;
static lv_obj_t *s_lab_sep1 = NULL;
static lv_obj_t *s_lab_sep2 = NULL;
static lv_timer_t *s_tick = NULL;

// ---------- NVS ----------
static void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open("quick", NVS_READONLY, &h) != ESP_OK) return;
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
out:
    nvs_close(h);
}

static void nvs_save(void) {
    nvs_handle_t h;
    if (nvs_open("quick", NVS_READWRITE, &h) != ESP_OK) return;
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
    nvs_commit(h);
    nvs_close(h);
    free(blob);
}

// ---------- UI helpers ----------
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

static void refresh_tasks(void) {
    int n = s_task_n;
    int start = s_task_idx - 2;
    if (start < 0) start = 0;
    if (n > 5 && start > n - 5) start = n - 5;
    if (start < 0) start = 0;
    for (int i = 0; i < 5; i++) {
        if (!s_lab_tasks[i]) continue;
        int idx = start + i;
        if (idx >= n) {
            lv_label_set_text(s_lab_tasks[i], "");
            continue;
        }
        char buf[TASK_NAME_LEN + 4];
        snprintf(buf, sizeof(buf), "%s %s",
                 (idx == s_task_idx) ? ">" : " ", s_tasks[idx]);
        lv_label_set_text(s_lab_tasks[i], buf);
        lv_obj_set_style_text_color(s_lab_tasks[i],
            (idx == s_task_idx) ? lv_color_hex(0xFFD166) : lv_color_hex(0xE6E6E6), 0);
    }
}

static void refresh_rec(void) {
    if (!s_rect_rec || !s_lab_rec) return;
    const char *txt = "READY";
    uint32_t col = 0x2EA043;
    if (s_rec_state == REC_RECORDING) { txt = "REC"; col = 0xE03131; }
    else if (s_rec_state == REC_SENDING) { txt = "SND"; col = 0x3B8BFF; }
    lv_obj_set_style_bg_color(s_rect_rec, lv_color_hex(col), 0);
    lv_label_set_text(s_lab_rec, txt);
}

static void refresh_rec_time(void) {
    if (!s_lab_rec_time) return;
    if (s_rec_state == REC_RECORDING) {
        int64_t dur_ms = (esp_timer_get_time() - s_rec_start_us) / 1000;
        char buf[16];
        snprintf(buf, sizeof(buf), "%ld.%01lds",
                 (long)(dur_ms / 1000), (long)((dur_ms % 1000) / 100));
        lv_label_set_text(s_lab_rec_time, buf);
    } else if (s_lab_rec_time) {
        lv_label_set_text(s_lab_rec_time, "0:00");
    }
}

static void refresh_all(void) {
    refresh_top();
    refresh_tasks();
    refresh_rec();
    refresh_rec_time();
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    refresh_top();
    refresh_rec_time();
}

// ---------- base64 ----------
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        if (o + 4 >= out_cap) return o;
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) v |= (uint32_t)in[i + 2];
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < in_len) ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < in_len) ? B64[v & 0x3F] : '=';
    }
    out[o] = 0;
    return o;
}

// ---------- 录音 task ----------
static void rec_task(void *arg) {
    (void)arg;
    int16_t *buf = (int16_t *)heap_caps_malloc(REC_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!buf) {
        ESP_LOGE(TAG, "alloc %d bytes failed", REC_BYTES);
        s_rec_state = REC_IDLE;
        if (bsp_lvgl_lock(200)) { refresh_rec(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"rec\",\"state\":\"fail\",\"reason\":\"oom\"}\n");
        vTaskDelete(NULL);
        return;
    }

    bsp_audio_set_format(REC_HZ, 16, 1);
    int64_t start = esp_timer_get_time();
    esp_err_t e = bsp_audio_read(buf, REC_BYTES);
    int64_t dur_us = esp_timer_get_time() - start;

    if (e != ESP_OK) {
        ESP_LOGW(TAG, "bsp_audio_read failed: %s", esp_err_to_name(e));
        printf("{\"t\":\"rec\",\"state\":\"fail\",\"reason\":\"audio_read\"}\n");
        free(buf);
        s_rec_state = REC_IDLE;
        if (bsp_lvgl_lock(200)) { refresh_rec(); bsp_lvgl_unlock(); }
        vTaskDelete(NULL);
        return;
    }

    // 切到 SENDING
    s_rec_state = REC_SENDING;
    if (bsp_lvgl_lock(200)) { refresh_rec(); bsp_lvgl_unlock(); }

    printf("{\"t\":\"rec\",\"state\":\"stop\",\"dur_ms\":%lld,\"samples\":%d}\n",
           (long long)(dur_us / 1000), REC_BYTES / 2);

    uint8_t *u8 = (uint8_t *)buf;
    int total = REC_BYTES;
    int sent = 0;
    int seq = 0;
    while (sent < total) {
        int chunk = total - sent;
        if (chunk > REC_CHUNK) chunk = REC_CHUNK;
        char enc[REC_CHUNK * 4 / 3 + 8];
        b64_encode(&u8[sent], (size_t)chunk, enc, sizeof(enc));
        printf("{\"t\":\"rec\",\"data\":\"%s\",\"seq\":%d}\n", enc, seq++);
        sent += chunk;
        vTaskDelay(pdMS_TO_TICKS(2));  // 让 console 能写出去
    }
    printf("{\"t\":\"rec\",\"data_end\":1}\n");
    printf("{\"t\":\"rec\",\"state\":\"done\"}\n");

    free(buf);
    s_rec_state = REC_IDLE;
    if (bsp_lvgl_lock(200)) { refresh_rec(); bsp_lvgl_unlock(); }
    vTaskDelete(NULL);
}

// ---------- demo 接口 ----------
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
    lv_label_set_text(s_lab_title, "# QUICK REC");
    lv_obj_set_pos(s_lab_title, 8, 30);

    // REC 徽章
    s_rect_rec = lv_obj_create(s_scr);
    lv_obj_set_size(s_rect_rec, 80, 32);
    lv_obj_set_pos(s_rect_rec, 8, 60);
    lv_obj_set_style_radius(s_rect_rec, 4, 0);
    lv_obj_set_style_bg_opa(s_rect_rec, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_rect_rec, 0, 0);

    s_lab_rec = lv_label_create(s_rect_rec);
    lv_obj_set_style_text_color(s_lab_rec, lv_color_hex(0x101820), 0);
    lv_obj_set_style_text_font(s_lab_rec, &lv_font_montserrat_20, 0);
    lv_obj_center(s_lab_rec);

    s_lab_rec_time = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_rec_time, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_lab_rec_time, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lab_rec_time, 100, 64);

    s_lab_sep1 = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_sep1, lv_color_hex(0x404040), 0);
    lv_obj_set_style_text_font(s_lab_sep1, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_sep1, "------------------------------");
    lv_obj_set_pos(s_lab_sep1, 0, 102);

    // 任务列表头
    s_lab_tasks_hdr = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_tasks_hdr, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(s_lab_tasks_hdr, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_tasks_hdr, "TASKS:");
    lv_obj_set_pos(s_lab_tasks_hdr, 8, 116);

    for (int i = 0; i < 5; i++) {
        s_lab_tasks[i] = lv_label_create(s_scr);
        lv_obj_set_style_text_color(s_lab_tasks[i], lv_color_hex(0xE6E6E6), 0);
        lv_obj_set_style_text_font(s_lab_tasks[i], &lv_font_montserrat_20, 0);
        lv_obj_set_pos(s_lab_tasks[i], 8, 138 + i * 28);
        lv_obj_set_width(s_lab_tasks[i], 220);
    }

    s_lab_sep2 = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_sep2, lv_color_hex(0x404040), 0);
    lv_obj_set_style_text_font(s_lab_sep2, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_sep2, "------------------------------");
    lv_obj_set_pos(s_lab_sep2, 0, 286);

    // HINT
    s_lab_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_hint, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(s_lab_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_hint, "OK=REC  UP/DN=TASK");
    lv_obj_set_pos(s_lab_hint, 8, 304);

    refresh_all();
    s_tick = lv_timer_create(tick_cb, 100, NULL);
    lv_screen_load(s_scr);

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
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_lab_time = s_bar_bat = s_lab_bat = s_lab_title = NULL;
    s_rect_rec = s_lab_rec = s_lab_rec_time = NULL;
    s_lab_tasks_hdr = s_lab_hint = NULL;
    s_lab_sep1 = s_lab_sep2 = NULL;
    for (int i = 0; i < 5; i++) s_lab_tasks[i] = NULL;
}

void demo_quick_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (s_rec_state == REC_RECORDING) {
        if (btn == BSP_BTN_OK) {
            printf("{\"t\":\"rec\",\"hint\":\"wait 3s auto-stop\"}\n");
        }
        return;
    }

    switch (btn) {
    case BSP_BTN_UP:
        if (s_task_n > 0 && s_task_idx > 0) s_task_idx--;
        refresh_tasks();
        printf("{\"t\":\"select\",\"idx\":%d,\"n\":%d}\n", s_task_idx, s_task_n);
        break;
    case BSP_BTN_DOWN:
        if (s_task_n > 0 && s_task_idx + 1 < s_task_n) s_task_idx++;
        refresh_tasks();
        printf("{\"t\":\"select\",\"idx\":%d,\"n\":%d}\n", s_task_idx, s_task_n);
        break;
    case BSP_BTN_OK:
        if (s_rec_state == REC_IDLE) {
            s_rec_state = REC_RECORDING;
            s_rec_start_us = esp_timer_get_time();
            refresh_rec();
            printf("{\"t\":\"rec\",\"state\":\"start\"}\n");
            xTaskCreate(rec_task, "rec", 4096, NULL, 5, NULL);
        }
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
        s_task_idx = 0;
        if (bsp_lvgl_lock(200)) { refresh_tasks(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"tasks\",\"n\":%d}\n", s_task_n);
    } else if (strcmp(argv[1], "select") == 0) {
        if (argc < 3) return 1;
        int v = atoi(argv[2]);
        if (v < 0 || v >= s_task_n) return 1;
        s_task_idx = v;
        if (bsp_lvgl_lock(200)) { refresh_tasks(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"select\",\"idx\":%d}\n", s_task_idx);
    } else if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) return 1;
        if (s_task_n >= MAX_TASKS) { printf("full\n"); return 1; }
        strncpy(s_tasks[s_task_n], argv[2], TASK_NAME_LEN - 1);
        s_tasks[s_task_n][TASK_NAME_LEN - 1] = 0;
        s_task_n++;
        if (bsp_lvgl_lock(200)) { refresh_tasks(); bsp_lvgl_unlock(); }
        printf("{\"t\":\"add\",\"idx\":%d}\n", s_task_n - 1);
    } else if (strcmp(argv[1], "clear") == 0) {
        s_task_n = 0;
        s_task_idx = 0;
        if (bsp_lvgl_lock(200)) { refresh_tasks(); bsp_lvgl_unlock(); }
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
