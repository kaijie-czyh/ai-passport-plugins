# CMakeLists.txt —— 替换 ai-passport/main/CMakeLists.txt
#
# 新增 3 个 demo 源文件 + 声明额外组件依赖.

idf_component_register(
    SRCS "main.c"
          "ui_pixel.c"
          "ui_pixel_math.c"
          "demo_display.c"
          "demo_button.c"
          "demo_audio.c"
          "demo_battery.c"
          "demo_companion.c"
          "demo_quick.c"
          "demo_flow.c"
          "fonts/pas_cjk_16.c"
    INCLUDE_DIRS "."
    REQUIRES bsp
             console
             nvs_flash
             esp_timer
)
