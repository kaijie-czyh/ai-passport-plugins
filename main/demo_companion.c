// demo_companion.c — AI 编程伴侣卡
//
// 产品定位:把 AI Passport 当成 IDE / Agent 的"实物状态屏 + 物理快捷键"。
//
// 屏幕布局 (240x320):
//   +--------------------------------+
//   | 14:23                  [##] 87% |  顶栏:时间 + 电量条
//   |                                |
//   | > AI COMPANION                 |  标题
//   |                                |
//   | STATUS                         |
//   | [ THINKING ]                   |  状态徽章(颜色映射)
//   |                                |
//   | TASK                           |
//   | refactor bsp_button.c ...      |  任务名(>14 字滚动)
//   |                                |
//   | 02:14 / 12:00   [====    ]     |  进度(数字 + 条)
//   |                                |
//   | TURN 017  TOK 4.2k             |
//   |                                |
//   | OK=ACK  UP=PREV  DN=NEXT       |
//   +--------------------------------+
//
// 按键语义(全局规则,本页不另行实现):
//   OK  短按 = ACK(向电脑发 {"act":"ack"})
//   UP  短按 = 上一个任务
//   DOWN 短按 = 下一个任务
//   OK  长按 = 返回菜单(由 main.c 拦截)
//
// 通信:
//   设备 -> 电脑: 通过 USB Serial/JTAG 输出 JSON 行
//                {"t":"status","s":"thinking","task":"...","turn":17,"tok":4200,
//                 "elapsed":134,"total":720,"idx":2,"n":5}
//                {"t":"hello","ver":1}
//                {"t":"btn","btn":"ok","ev":"click"}
//   电脑 -> 设备: 通过 console 命令 (idf.py monitor 输入)
//                companion status thinking "refactor bsp_button.c"
//                companion tick 134 720
//                companion task "do X" "do Y" "do Z"
//                companion select 1
//                companion select-next
//                companion ping
//
// 硬件使用:
//   - 显示:LVGL,默认字体(不引入外部字体,避开 PSRAM 限制)
//   - 按键:复用 bsp_button,只在已有回调里分发
//   - 音频:不使用(节省堆)
//   - BLE/NVS:不使用(本次 demo 仅展示单机状态屏,
//              电脑端通过 USB Serial/JTAG 拉取或推状态)
//   - 电池:仅顶部读一次 SOC 用于显示
//
// 内存注意:
//   - 任务名最大 32 字节,任务列表上限 8 条,共 ~256 字节 BSS
//   - LVGL 对象数: ~12 个,均在 24KB 池内可接受
//   - 不创建额外任务,所有更新都在按键回调中直接做(回调已被 main 加锁)
//
// 验收:
//   1. idf.py build 通过
//   2. 刷入后菜单出现 "Companion" 项
//   3. 进入后默认显示 IDLE 状态
//   4. 在 monitor 中输入: companion status thinking "hello world"
//      屏幕状态徽章变黄并显示 "hello world"
//   5. 按 UP/DOWN 在 mock 任务列表(若有)间切换
//   6. 按 OK, monitor 出现 {"t":"btn",...} 行
//   7. 长按 OK 返回菜单

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "bsp_display.h"
#include "bsp_lvgl.h"   // bsp_lvgl_lock / unlock
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "argtable3/argtable3.h"

static const char *TAG = "companion";

#define MAX_TASKS        8
#define TASK_NAME_LEN    32
#define STATUS_NAME_LEN  16

// ---------- 共享状态 (BSS, 被 LVGL 回调 / console 回调共享) ----------
typedef enum {
    ST_IDLE = 0,
    ST_THINKING,
    ST_TOOL,
    ST_WAITING,
    ST_DONE,
    ST_ERROR,
    ST__COUNT
} status_t;

static const char *s_status_names[ST__COUNT] = {
    "IDLE", "THINKING", "TOOL", "WAITING", "DONE", "ERROR"
};
// 状态对应颜色 (RGB565)
static uint32_t s_status_color[ST__COUNT] = {
    0x7A7A7A, // IDLE   灰
    0xF4C20D, // THINK 黄
    0x3B8BFF, // TOOL   蓝
    0xFF8C1A, // WAIT   橙
    0x2EA043, // DONE   绿
    0xE03131, // ERROR  红
};

typedef struct {
    status_t status;
    char     task[TASK_NAME_LEN];
    uint32_t elapsed_s;
    uint32_t total_s;
    uint16_t turn;
    uint16_t tok;       // token 数 (k)
    uint8_t  task_idx;
    uint8_t  task_n;
} comp_state_t;

static comp_state_t s_state = {
    .status = ST_IDLE,
    .task   = "no task",
    .task_n = 1,
};
// 用户可注入的任务名列表 (通过 console command 设置)
static char s_tasks[MAX_TASKS][TASK_NAME_LEN];
static int  s_task_n_set = 0; // 用户设置的任务条数 (0 = 用 s_state.task 单条)

static lv_obj_t *s_scr;
static lv_obj_t *s_lab_time;
static lv_obj_t *s_bar_bat;
static lv_obj_t *s_lab_bat;
static lv_obj_t *s_lab_status;
static lv_obj_t *s_rect_status;   // 状态徽章背景
static lv_obj_t *s_lab_task;
static lv_obj_t *s_lab_progress;
static lv_obj_t *s_bar_progress;
static lv_obj_t *s_lab_metrics;
static lv_obj_t *s_lab_hint;

// ---------- 辅助:格式化 HH:MM ----------
static void format_time(char *out, size_t n) {
    int64_t us = esp_timer_get_time();
    int64_t s  = (us / 1000000LL) % 86400LL;
    int hh = (int)((s / 3600 + 8) % 24); // UTC+8 (Asia/Shanghai)
    int mm = (int)((s / 60) % 60);
    snprintf(out, n, "%02d:%02d", hh, mm);
}

// ---------- 刷新各 UI 元素 ----------
static void refresh_status_badge(void) {
    if (!s_rect_status || !s_lab_status) return;
    lv_obj_set_style_bg_color(s_rect_status,
        lv_color_hex(s_status_color[s_state.status]), 0);
    lv_label_set_text(s_lab_status, s_status_names[s_state.status]);
}

static void refresh_task(void) {
    if (!s_lab_task) return;
    // 如果用户注入了任务列表,按 idx 显示;否则直接用 s_state.task
    const char *txt = s_state.task;
    if (s_task_n_set > 0) {
        int idx = s_state.task_idx;
        if (idx < 0) idx = 0;
        if (idx >= s_task_n_set) idx = s_task_n_set - 1;
        txt = s_tasks[idx];
    }
    // LVGL label 在长字符串时会自动处理;这里手动加 "> " 前缀
    char buf[TASK_NAME_LEN + 4];
    snprintf(buf, sizeof(buf), "> %s", txt);
    lv_label_set_text(s_lab_task, buf);
}

static void refresh_progress(void) {
    if (!s_lab_progress || !s_bar_progress) return;
    char buf[32];
    uint32_t mm = s_state.elapsed_s / 60;
    uint32_t ss = s_state.elapsed_s % 60;
    uint32_t tmm = s_state.total_s / 60;
    uint32_t tss = s_state.total_s % 60;
    snprintf(buf, sizeof(buf), "%02" PRIu32 ":%02" PRIu32 " / %02" PRIu32 ":%02" PRIu32,
             mm, ss, tmm, tss);
    lv_label_set_text(s_lab_progress, buf);

    int pct = 0;
    if (s_state.total_s > 0) {
        pct = (int)(s_state.elapsed_s * 100 / s_state.total_s);
        if (pct > 100) pct = 100;
    }
    lv_bar_set_value(s_bar_progress, pct, LV_ANIM_OFF);
}

static void refresh_metrics(void) {
    if (!s_lab_metrics) return;
    char buf[40];
    snprintf(buf, sizeof(buf),
             "TURN %03u  TOK %u.%uk",
             s_state.turn, s_state.tok / 10, s_state.tok % 10);
    lv_label_set_text(s_lab_metrics, buf);
}

static void refresh_top_bar(void) {
    if (s_lab_time) {
        char t[8];
        format_time(t, sizeof(t));
        lv_label_set_text(s_lab_time, t);
    }
    int soc = bsp_battery_get_soc();
    if (soc < 0) soc = 0;
    if (soc > 100) soc = 100;
    if (s_bar_bat) lv_bar_set_value(s_bar_bat, soc, LV_ANIM_OFF);
    if (s_lab_bat) {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", soc);
        lv_label_set_text(s_lab_bat, b);
    }
}

// 整体刷新入口
static void refresh_all(void) {
    refresh_top_bar();
    refresh_status_badge();
    refresh_task();
    refresh_progress();
    refresh_metrics();
}

// ---------- 把当前状态以 JSON 行打到 USB Serial ----------
static void emit_state_json(void) {
    char task_esc[TASK_NAME_LEN * 2 + 4];
    const char *src = s_state.task;
    if (s_task_n_set > 0) {
        int idx = s_state.task_idx;
        if (idx < 0 || idx >= s_task_n_set) idx = 0;
        src = s_tasks[idx];
    }
    // 简单转义:替换 " 和 \ ,控制字符
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < sizeof(task_esc); i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { task_esc[j++] = '\\'; task_esc[j++] = c; }
        else if (c == '\n' || c == '\r') { task_esc[j++] = ' '; }
        else if ((unsigned char)c < 0x20) { /* skip */ }
        else { task_esc[j++] = c; }
    }
    task_esc[j] = 0;

    printf("{\"t\":\"status\",\"s\":\"%s\",\"task\":\"%s\","
           "\"elapsed\":%" PRIu32 ",\"total\":%" PRIu32 ","
           "\"turn\":%u,\"tok\":%u,\"idx\":%u,\"n\":%u,\"bat\":%d}\n",
           s_status_names[s_state.status], task_esc,
           s_state.elapsed_s, s_state.total_s,
           (unsigned)s_state.turn, (unsigned)s_state.tok,
           (unsigned)s_state.task_idx, (unsigned)s_state.task_n,
           bsp_battery_get_soc());
}

// ---------- demo 模板: enter / exit / key ----------

void demo_companion_enter(void) {
    ESP_LOGI(TAG, "companion enter");

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    // 顶栏: 时间 + 电池条
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
    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9CDCFE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "> AI COMPANION");
    lv_obj_set_pos(title, 8, 32);

    // STATUS 区域
    lv_obj_t *lab_s = lv_label_create(s_scr);
    lv_obj_set_style_text_color(lab_s, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(lab_s, &lv_font_montserrat_14, 0);
    lv_label_set_text(lab_s, "STATUS");
    lv_obj_set_pos(lab_s, 8, 70);

    s_rect_status = lv_obj_create(s_scr);
    lv_obj_set_size(s_rect_status, 130, 28);
    lv_obj_set_pos(s_rect_status, 8, 88);
    lv_obj_set_style_radius(s_rect_status, 4, 0);
    lv_obj_set_style_bg_opa(s_rect_status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_rect_status, 0, 0);

    s_lab_status = lv_label_create(s_rect_status);
    lv_obj_set_style_text_color(s_lab_status, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_lab_status, &lv_font_montserrat_20, 0);
    lv_obj_center(s_lab_status);

    // TASK 区域
    lv_obj_t *lab_t = lv_label_create(s_scr);
    lv_obj_set_style_text_color(lab_t, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(lab_t, &lv_font_montserrat_14, 0);
    lv_label_set_text(lab_t, "TASK");
    lv_obj_set_pos(lab_t, 8, 130);

    s_lab_task = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_task, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_lab_task, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(s_lab_task, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_lab_task, 224);
    lv_obj_set_pos(s_lab_task, 8, 150);

    // 进度数字 + 进度条
    s_lab_progress = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_progress, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_lab_progress, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lab_progress, 8, 190);

    s_bar_progress = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar_progress, 224, 8);
    lv_obj_set_pos(s_bar_progress, 8, 218);
    lv_bar_set_range(s_bar_progress, 0, 100);
    lv_obj_set_style_bg_color(s_bar_progress, lv_color_hex(0x2A2E33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_progress, lv_color_hex(0x3B8BFF), LV_PART_INDICATOR);

    // 指标
    s_lab_metrics = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_metrics, lv_color_hex(0xB5CEA8), 0);
    lv_obj_set_style_text_font(s_lab_metrics, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lab_metrics, 8, 240);

    // 底部按键提示
    s_lab_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_lab_hint, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(s_lab_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lab_hint, "OK=ACK  UP=PREV  DN=NEXT");
    lv_obj_set_pos(s_lab_hint, 8, 290);

    refresh_all();
    lv_screen_load(s_scr);

    // 自我介绍 (电脑端可以借此判定设备就绪)
    printf("{\"t\":\"hello\",\"app\":\"companion\",\"ver\":1}\n");
    emit_state_json();
}

void demo_companion_exit(void) {
    ESP_LOGI(TAG, "companion exit");
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    // 清空所有静态指针,避免悬空
    s_lab_time = s_lab_bat = s_bar_bat = NULL;
    s_lab_status = s_rect_status = NULL;
    s_lab_task = s_lab_progress = s_bar_progress = NULL;
    s_lab_metrics = s_lab_hint = NULL;
}

// 按键回调:已被 main.c 加锁,直接操作 LVGL
void demo_companion_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 只消费 CLICK;LONG 由 main 拦截返回菜单
    if (ev != BSP_BTN_CLICK) return;

    switch (btn) {
    case BSP_BTN_UP:
        if (s_task_n_set > 0 && s_state.task_idx > 0) {
            s_state.task_idx--;
            refresh_task();
        }
        printf("{\"t\":\"btn\",\"btn\":\"up\",\"ev\":\"click\"}\n");
        break;
    case BSP_BTN_DOWN:
        if (s_task_n_set > 0 && s_state.task_idx + 1 < s_task_n_set) {
            s_state.task_idx++;
            refresh_task();
        }
        printf("{\"t\":\"btn\",\"btn\":\"down\",\"ev\":\"click\"}\n");
        break;
    case BSP_BTN_OK:
        // ACK:向电脑端发送 ack 信号
        printf("{\"t\":\"btn\",\"btn\":\"ok\",\"ev\":\"click\",\"act\":\"ack\"}\n");
        break;
    default:
        break;
    }
}

// ---------- console 命令注册 ----------
// 用法: companion status <IDLE|THINKING|TOOL|WAITING|DONE|ERROR> [task]
//       companion tick <elapsed_s> <total_s>
//       companion task "name1" "name2" ...
//       companion select <idx>
//       companion select-next / select-prev
//       companion turn <n>  companion tok <n>
//       companion reset
//       companion ping

static int parse_status(const char *s, status_t *out) {
    for (int i = 0; i < ST__COUNT; i++) {
        if (strcasecmp(s, s_status_names[i]) == 0) {
            *out = (status_t)i;
            return 0;
        }
    }
    return -1;
}

static int cmd_companion(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: companion <status|tick|task|select|select-next|select-prev|turn|tok|reset|ping>\n");
        return 1;
    }
    // 因为 console 命令运行在系统任务里,不持有 LVGL 锁;
    // 这里只更新 BSS 状态,然后通过 lvgl 定时器在下一帧刷新 UI
    // (lvgl timer 每 5ms 触发,延迟人眼不可见)
    if (strcmp(argv[1], "status") == 0) {
        if (argc < 3) { printf("need status name\n"); return 1; }
        status_t st;
        if (parse_status(argv[2], &st) != 0) { printf("unknown status\n"); return 1; }
        s_state.status = st;
        if (argc >= 4) {
            strncpy(s_state.task, argv[3], sizeof(s_state.task) - 1);
            s_state.task[sizeof(s_state.task) - 1] = 0;
            // 单条任务模式下,关闭列表模式
            s_task_n_set = 0;
        }
    } else if (strcmp(argv[1], "tick") == 0) {
        if (argc < 4) { printf("usage: companion tick <elapsed_s> <total_s>\n"); return 1; }
        s_state.elapsed_s = (uint32_t)atoi(argv[2]);
        s_state.total_s   = (uint32_t)atoi(argv[3]);
    } else if (strcmp(argv[1], "task") == 0) {
        if (argc < 3) { printf("usage: companion task \"name1\" \"name2\" ...\n"); return 1; }
        int n = argc - 2;
        if (n > MAX_TASKS) n = MAX_TASKS;
        for (int i = 0; i < n; i++) {
            strncpy(s_tasks[i], argv[2 + i], TASK_NAME_LEN - 1);
            s_tasks[i][TASK_NAME_LEN - 1] = 0;
        }
        s_task_n_set = n;
        s_state.task_n = (uint8_t)n;
        if ((int)s_state.task_idx >= n) s_state.task_idx = (uint8_t)(n - 1);
    } else if (strcmp(argv[1], "select") == 0) {
        if (argc < 3) { printf("usage: companion select <idx>\n"); return 1; }
        int idx = atoi(argv[2]);
        if (s_task_n_set == 0) { printf("no task list set\n"); return 1; }
        if (idx < 0 || idx >= s_task_n_set) { printf("idx out of range\n"); return 1; }
        s_state.task_idx = (uint8_t)idx;
    } else if (strcmp(argv[1], "select-next") == 0) {
        if (s_task_n_set > 0 && s_state.task_idx + 1 < s_task_n_set) s_state.task_idx++;
    } else if (strcmp(argv[1], "select-prev") == 0) {
        if (s_task_n_set > 0 && s_state.task_idx > 0) s_state.task_idx--;
    } else if (strcmp(argv[1], "turn") == 0) {
        if (argc < 3) return 1;
        s_state.turn = (uint16_t)atoi(argv[2]);
    } else if (strcmp(argv[1], "tok") == 0) {
        if (argc < 3) return 1;
        s_state.tok = (uint16_t)atoi(argv[2]);
    } else if (strcmp(argv[1], "reset") == 0) {
        memset(&s_state, 0, sizeof(s_state));
        s_state.status = ST_IDLE;
        strncpy(s_state.task, "no task", sizeof(s_state.task) - 1);
        s_task_n_set = 0;
    } else if (strcmp(argv[1], "ping") == 0) {
        printf("{\"t\":\"pong\",\"app\":\"companion\"}\n");
        return 0;
    } else {
        printf("unknown subcmd: %s\n", argv[1]);
        return 1;
    }

    // 调度一次 UI 刷新:简单做法是直接调 refresh_*,
    // 但 console 任务不持有 lvgl 锁,因此不能直接操作 lv_*
    // 解决:把"需要刷新"置位,让 lvgl timer 回调来执行。
    // 为简单起见,这里只更新 BSS,通过下一次的按键事件或下次 tick 触发刷新。
    // 但用户期望命令立即见效,所以我们抢一次锁:
    if (bsp_lvgl_lock(200)) {
        refresh_all();
        bsp_lvgl_unlock();
    }
    emit_state_json();
    return 0;
}

void demo_companion_console_register(void) {
    const esp_console_cmd_t cmd = {
        .command = "companion",
        .help    = "control companion app (status|tick|task|select|turn|tok|reset|ping)",
        .func    = &cmd_companion,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
