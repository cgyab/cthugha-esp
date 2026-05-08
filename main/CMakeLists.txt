set(srcs
    "main.c"
    "flames.c"
    "waves.c"
    "palettes.c"
    "translate.c"
    "display.c"
    "audio_capture.c"
    "touch_input.c"
    "boom_box.c"
    "logo_data.c"
)

if(CONFIG_CTHUGHA_SDCARD_ENABLED)
    list(APPEND srcs "sdcard_record.c")
endif()

set(requires
    driver
    esp_lcd
    esp_timer
    freertos
    esp_hw_support
    fatfs
    sdmmc
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES ${requires}
)
