// demo.h —— 替换 ai-passport/main/demo.h
//
// 7 项菜单的 demo 接口声明. key 由 main.c 路由, enter/exit 在 main_task 上下文调用
// (避免 button 回调栈里跑 LVGL/NVS 导致 stack overflow).

#pragma once

#include "bsp_button.h"

typedef struct {
    const char *name;
    void (*enter)(void);
    void (*exit)(void);
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);
    void (*console_register)(void);  // NULL = 不注册 console 命令
} demo_entry_t;

void demo_display_enter(void); void demo_display_exit(void);
void demo_display_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_button_enter(void); void demo_button_exit(void);
void demo_button_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_audio_enter(void); void demo_audio_exit(void);
void demo_audio_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_battery_enter(void); void demo_battery_exit(void);
void demo_battery_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// ----- 三个新产品 demo -----
void demo_companion_enter(void); void demo_companion_exit(void);
void demo_companion_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void demo_companion_console_register(void);

void demo_quick_enter(void); void demo_quick_exit(void);
void demo_quick_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void demo_quick_console_register(void);

void demo_flow_enter(void); void demo_flow_exit(void);
void demo_flow_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void demo_flow_console_register(void);

// demo 列表 (main.c 用)
extern const demo_entry_t g_demos[];
extern const int g_demos_n;
