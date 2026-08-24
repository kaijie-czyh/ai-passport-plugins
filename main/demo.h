// demo.h —— 拷贝覆盖 ai-passport/main/demo.h
//
// 在原有四个 demo 声明后,加入三个新产品的 demo 接口声明。

#pragma once

#include "bsp_button.h"

typedef struct {
    const char *name;
    void (*enter)(void);
    void (*exit)(void);
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);
} demo_entry_t;

void demo_display_enter(void); void demo_display_exit(void);
void demo_display_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void demo_button_enter(void); void demo_button_exit(void);
void demo_button_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void demo_audio_enter(void); void demo_audio_exit(void);
void demo_audio_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void demo_battery_enter(void); void demo_battery_exit(void);
void demo_battery_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// ----- 新增 3 个产品 demo -----
void demo_companion_enter(void); void demo_companion_exit(void);
void demo_companion_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void demo_companion_console_register(void);

void demo_quick_enter(void); void demo_quick_exit(void);
void demo_quick_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void demo_quick_console_register(void);

void demo_flow_enter(void); void demo_flow_exit(void);
void demo_flow_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void demo_flow_console_register(void);
